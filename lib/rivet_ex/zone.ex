defmodule RivetEx.Zone do
  @moduledoc """
  A game zone: the control-plane owner of a data-plane worker.

  The zone's control and durable state live in the BEAM; the hot loop (datagram
  ingest, physics) runs in a data-plane worker outside the BEAM. The zone receives
  digested snapshots as messages (event-driven, contract 2) and issues lifecycle
  and control commands to the worker (contract 3). It never polls and never touches
  a packet. See `docs/data-plane.md`.

  A zone is addressed through the same distributed registry as actors, so `Horde`
  places one zone per id across the cluster and hands it off on node loss; the new
  owner starts a fresh data-plane worker and resumes durable state.
  """

  use GenServer

  alias RivetEx.DataPlane.Snapshot

  @spec start_link(keyword()) :: GenServer.on_start()
  def start_link(opts) do
    zone_id = Keyword.fetch!(opts, :zone_id)
    GenServer.start_link(__MODULE__, opts, name: via(zone_id))
  end

  @doc "Distributed address of a zone."
  @spec via(term()) :: {:via, module(), {module(), {:zone, term()}}}
  def via(zone_id), do: {:via, Horde.Registry, {RivetEx.Registry, {:zone, zone_id}}}

  @doc "The latest digested snapshot from the data plane, or nil before the first tick."
  @spec latest(GenServer.server()) :: Snapshot.t() | nil
  def latest(server), do: GenServer.call(server, :latest)

  @doc "The latest tick the data plane has produced (0 before the first)."
  @spec tick(GenServer.server()) :: non_neg_integer()
  def tick(server), do: GenServer.call(server, :tick)

  @doc "Send a control command to the data-plane worker (e.g. :pause, :resume)."
  @spec command(GenServer.server(), term()) :: :ok
  def command(server, cmd), do: GenServer.call(server, {:command, cmd})

  @impl true
  def init(opts) do
    zone_id = Keyword.fetch!(opts, :zone_id)
    worker_mod = Keyword.get(opts, :worker, RivetEx.DataPlane.Stub)
    worker_opts = Keyword.get(opts, :worker_opts, [])

    # The worker is linked: if the hot loop dies, the zone dies with it and is
    # restarted (by Horde) on a healthy node, which is the correct failure mode.
    {:ok, worker} = worker_mod.start_link(zone_id, self(), worker_opts)

    {:ok, %{zone_id: zone_id, worker_mod: worker_mod, worker: worker, latest: nil}}
  end

  @impl true
  def handle_info({:dp_snapshot, _zone_id, %Snapshot{} = snapshot}, state) do
    {:noreply, %{state | latest: snapshot}}
  end

  def handle_info(_msg, state), do: {:noreply, state}

  @impl true
  def handle_call(:latest, _from, state), do: {:reply, state.latest, state}

  def handle_call(:tick, _from, state) do
    {:reply, if(state.latest, do: state.latest.tick, else: 0), state}
  end

  def handle_call({:command, cmd}, _from, state) do
    :ok = state.worker_mod.command(state.worker, cmd)
    {:reply, :ok, state}
  end

  @impl true
  def terminate(_reason, state) do
    if is_pid(state[:worker]) and Process.alive?(state.worker) do
      state.worker_mod.stop(state.worker)
    end

    :ok
  end
end
