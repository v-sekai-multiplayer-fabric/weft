defmodule Weft.DataPlane.Snapshot do
  @moduledoc """
  Digested world state crossing the physics -> BEAM boundary (contract 2 in
  `docs/reference/data-plane.md`). This is what the data plane hands the control plane: not
  packets, but a per-tick summary the BEAM reads at tick rate.
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
  Contract for a zone's data-plane worker: the C++ Seastar + iceoryx + Jolt stack,
  or a stub. The real implementation is a Port or dirty-NIF to a separate OS
  process that owns pinned cores; it **pushes** digested snapshots to the
  subscriber as messages, so the BEAM stays event-driven and never busy-polls. See
  `docs/reference/data-plane.md`.

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
  this module with a Port/NIF to Seastar+iceoryx+Jolt; the contract is identical.
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
