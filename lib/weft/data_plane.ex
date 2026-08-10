defmodule Weft.DataPlane.Snapshot do
  @moduledoc ~S"""
  The game data plane is one instance of the general rule in `Weft`: the BEAM
  runs only the control plane, and every heavy plane is a native process outside the
  VM reached over iceoryx. This document details that boundary for the game hot path,
  a **ring**.

  weft is the **control plane**. It decides _which_ node owns a zone, keeps one
  writer per id, hands zones off on node loss, and holds durable state. It must
  never touch a game packet.

  The **data plane** decides _how fast_ one node ingests. At MMOG scale the hot path
  (datagram ingest, decode, spatial physics) runs outside the BEAM, in C or C++ on
  pinned cores or on NIC silicon. This document fixes the boundary between them so
  the two halves can be built independently.

  ## The stack

  ```mermaid
  flowchart TD
      NIC["NIC / SmartNIC-DPU<br/>15M+ pps, kernel bypass"]
      Ingest["ingest edge (C++ / picoquic)<br/>thread-per-core<br/>decode WebTransport/QUIC datagrams"]
      Iceoryx["Eclipse iceoryx<br/>zero-copy IPC (&lt;1 microsecond)"]
      Jolt["Jolt Physics (C++, own process)<br/>60Hz spatial hash, broadphase, raycast, collision"]
      Ring[("ring<br/>digested snapshots")]
      BEAM["Elixir / BEAM = control plane<br/>placement, lifecycle, state, chat, accounts"]

      NIC --> Ingest
      Ingest -->|"player inputs, zero-copy"| Iceoryx
      Iceoryx --> Jolt
      Jolt -->|"digested world state"| Ring
      Ring -->|"read at tick rate, event-driven"| BEAM
      BEAM -->|"control: spawn/despawn/place zone"| Jolt
  ```

  | Tier      | Tech                                     | Owns                                                                           | Peak           |
  | --------- | ---------------------------------------- | ------------------------------------------------------------------------------ | -------------- |
  | Network   | **ingest edge (C++/picoquic)**           | Datagram ingest + decode, kernel bypass                                       | 15M+ pps       |
  | IPC       | **Eclipse iceoryx**                      | Zero-copy handoff, network → physics                                           | <1 microsecond |
  | Game loop | **Jolt Physics (C++, separate process)** | 60Hz spatial hash, broadphase culling, raycast, collision                      | multi-core     |
  | Control   | **Elixir / BEAM (weft)**                 | See `Weft`                                                          | —              |

  ## The three contracts across the boundary

  1. **Network → Physics (iceoryx, zero-copy).** The ingest edge decodes datagrams and
     publishes decrypted player inputs into an iceoryx pool; Jolt subscribes.
     Zero-copy over iceoryx, sub-microsecond. weft is not involved.

  2. **Physics → BEAM (ring, read at tick rate).** Jolt writes _digested_ world state
     (not packets) into a ring, delivered over iceoryx publish-subscribe. The BEAM
     reads the latest snapshot at its tick rate through a small iceoryx NIF. The BEAM
     side is **event-driven or tick-scheduled, never a busy-poll**.

  3. **BEAM → Data plane (control channel).** weft issues lifecycle commands: spawn a
     zone's data-plane worker on this node, despawn it, migrate it. These ride an
     iceoryx request-response control path, low-rate.

  ## Hard rules

  - **No game packets in the BEAM.** Ever. Even at 500k pps the BEAM is the wrong
    packet pump (per-message copy, GC, preemptive scheduling). The 15M figure is the
    extreme tail; the BEAM should leave the hot path two orders of magnitude earlier.
  - **Never busy-poll inside a NIF.** A long-running NIF blocks a scheduler thread
    and wrecks whole-VM latency. The data plane owns the busy-poll on pinned cores in
    a _separate OS process_; the BEAM reads pre-assembled state off the ring via a
    small iceoryx NIF at tick rate.
  - **Data plane owns pinned cores.** DPDK/the ingest edge reactor cores run at 100%
    permanently and are excluded from the BEAM scheduler set.
  - **Interest management before hardware.** A server only faces 15M pps if it is
    _interested_ in 15M pps. Spatial sharding + area-of-interest culling (one zone =
    one actor, placed on one node) keeps per-server pps bounded. Reach for AF_XDP →
    DPDK → SmartNIC only for a single zone that cannot be sharded further.

  ## The pps escalation ladder

  Climb only when the tier below is genuinely saturated, and cull first.

  1. **OS UDP sockets** — fine to ~100k–500k pps. Default.
  2. **AF_XDP** — ~5–10M pps, some kernel involvement, no special NIC.
  3. **DPDK / the ingest edge** — 15–30M pps, dedicate 4–8 cores to polling.
  4. **SmartNIC / DPU** — 50–100M+ pps, offload decode/filter onto NIC silicon.

  ## Where weft fits

  weft decides _where_ a zone runs; the data plane decides _how fast_ that one
  server ingests. They compose through placement:

  - `Horde` single-writer + handoff (already built) tells the data plane **which box
    owns a zone's socket**. When a node dies, weft re-places the zone; the new
    owner's data-plane worker binds the socket.
  - The store (see `Weft.Actor.Store`) holds the zone's durable state so the new owner can
    resume it after handoff, with no filesystem affinity.
  - The zone's hot loop (the ingest edge/iceoryx/Jolt) is spawned and reaped by weft as
    part of that zone's lifecycle, but runs entirely outside the BEAM.

  The next step is a thin, honest prototype of contract (2) and (3): a `Weft.Zone`
  whose control/durable state lives in the BEAM, with a stubbed data-plane worker
  behind a behaviour, so the seam is exercised before the C++ exists. See
  `Weft.DataPlane`.
  """

  @enforce_keys [:zone_id, :tick, :entities, :generated_at]
  defstruct [:zone_id, :tick, :entities, :generated_at]

  @type entity :: %{id: term(), x: float(), y: float(), z: float()}
  @type t :: %__MODULE__{
          zone_id: term(),
          tick: non_neg_integer(),
          entities: [entity()],
          generated_at: integer()
        }
end

defmodule Weft.DataPlane.Worker do
  @moduledoc """
  Contract for a zone's data-plane worker: the C++ harness + iceoryx v1 + Jolt stack,
  or a stub. The real implementation is a Port or dirty-NIF to a separate OS
  process that owns pinned cores; it **pushes** digested snapshots to the
  subscriber as messages, so the BEAM stays event-driven and never busy-polls. See
  `Weft.DataPlane`.

  Snapshots are delivered to the subscriber as:

      {:dp_snapshot, zone_id, %Weft.DataPlane.Snapshot{}}
  """

  @callback start_link(zone_id :: term(), subscriber :: pid(), opts :: keyword()) ::
              {:ok, pid()} | {:error, term()}
  @callback command(pid(), term()) :: :ok
  @callback stop(pid()) :: :ok
end

defmodule Weft.DataPlane.Stub do
  @moduledoc """
  Stand-in data-plane worker for exercising the boundary before the C++ exists. It
  models the right shape: it schedules its own ticks (event-driven, not a busy
  loop) and pushes synthetic snapshots to the subscriber. The real worker replaces
  this module with a NIF to the native plane over iceoryx v1. The contract is identical.
  """

  @behaviour Weft.DataPlane.Worker

  use GenServer

  alias Weft.DataPlane.Snapshot

  @impl Weft.DataPlane.Worker
  def start_link(zone_id, subscriber, opts \\ []) do
    GenServer.start_link(__MODULE__, {zone_id, subscriber, opts})
  end

  @impl Weft.DataPlane.Worker
  def command(pid, cmd), do: GenServer.cast(pid, {:command, cmd})

  @impl Weft.DataPlane.Worker
  def stop(pid), do: GenServer.stop(pid, :normal)

  @impl GenServer
  def init({zone_id, subscriber, opts}) do
    state = %{
      zone_id: zone_id,
      subscriber: subscriber,
      tick: 0,
      interval: Keyword.get(opts, :tick_ms, 16),
      entities: Keyword.get(opts, :entities, 3),
      paused: false
    }

    schedule(state.interval)
    {:ok, state}
  end

  @impl GenServer
  def handle_info(:tick, %{paused: true} = state) do
    schedule(state.interval)
    {:noreply, state}
  end

  def handle_info(:tick, state) do
    tick = state.tick + 1

    snapshot = %Snapshot{
      zone_id: state.zone_id,
      tick: tick,
      entities: synth_entities(state.entities, tick),
      generated_at: System.system_time(:millisecond)
    }

    send(state.subscriber, {:dp_snapshot, state.zone_id, snapshot})
    schedule(state.interval)
    {:noreply, %{state | tick: tick}}
  end

  @impl GenServer
  def handle_cast({:command, :pause}, state), do: {:noreply, %{state | paused: true}}
  def handle_cast({:command, :resume}, state), do: {:noreply, %{state | paused: false}}
  def handle_cast({:command, _other}, state), do: {:noreply, state}

  defp synth_entities(count, tick) do
    for i <- 1..count, do: %{id: i, x: i * 1.0, y: 0.0, z: tick * 1.0}
  end

  defp schedule(ms), do: Process.send_after(self(), :tick, ms)
end
