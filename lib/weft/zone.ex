defmodule Weft.Zone do
  @moduledoc """
  A game zone: the control-plane owner of a data-plane worker, and the authority
  for the entities currently in its region.

  This is the port of the rivet custom zone actors onto weft's control/data-plane
  split (`Weft.DataPlane`):

    * **authority** — the zone is the single writer for its entities. Ownership and
      handoff decisions live here in the BEAM; hot entity *positions* live in
      `Weft.DataPlane.Ring` (>15M snapshots/sec), not in these messages.
    * **fanout** — subscribers receive digested snapshots at tick rate. This is the
      60Hz digested stream, never a per-packet fan-out.
    * **handoff** — an entity crossing the area-of-interest boundary moves from one
      zone to another with `handoff/3`, cluster-wide via `Horde`.

  A zone is addressed through the distributed registry, so `Horde` keeps one zone
  per id across the cluster and hands it off on node loss.
  """

  use GenServer

  alias Weft.DataPlane.Snapshot

  @spec start_link(keyword()) :: GenServer.on_start()
  def start_link(opts) do
    zone_id = Keyword.fetch!(opts, :zone_id)
    GenServer.start_link(__MODULE__, opts, name: via(zone_id))
  end

  @doc "Distributed address of a zone."
  @spec via(term()) :: {:via, module(), {module(), {:zone, term()}}}
  def via(zone_id), do: {:via, Horde.Registry, {Weft.Registry, {:zone, zone_id}}}

  ## Data-plane worker (contract 2/3)

  @doc "The latest digested snapshot from the data plane, or nil before the first tick."
  @spec latest(term()) :: Snapshot.t() | nil
  def latest(zone_id), do: GenServer.call(via(zone_id), :latest)

  @doc "The latest tick the data plane has produced (0 before the first)."
  @spec tick(term()) :: non_neg_integer()
  def tick(zone_id), do: GenServer.call(via(zone_id), :tick)

  @doc "Send a control command to the data-plane worker (e.g. :pause, :resume)."
  @spec command(term(), term()) :: :ok
  def command(zone_id, cmd), do: GenServer.call(via(zone_id), {:command, cmd})

  ## Authority: the zone owns its entities

  @doc "Add or update an entity this zone is authoritative for."
  @spec add_entity(term(), term(), term()) :: :ok
  def add_entity(zone_id, entity_id, data) do
    :telemetry.span([:weft, :zone, :add_entity], %{}, fn ->
      {GenServer.call(via(zone_id), {:add_entity, entity_id, data}), %{}}
    end)
  end

  @doc """
  Write an avatar's state under `epoch`, and refuse it if the epoch is superseded.

  **This is the fence, and it is the only place one exists.** The zone is the single writer
  for its entities, so it is the only process that can compare an epoch and apply a write
  without a gap between them. It keeps the highest epoch it has accepted for each avatar and
  refuses anything lower.

  A check somewhere else cannot do this. `Weft.Authority.check/3` reads FoundationDB and
  returns, and the write happens afterwards, so a controller seized from in between passes
  the check and still writes. The gap is small and it is real. Here there is no gap: a
  GenServer handles one message at a time, so the comparison and the write are one step, and
  a seizure arrives as another message rather than during this one.

  The epoch comes from `Weft.Authority.claim/2` or `seize/2`, which hold the claim in
  FoundationDB because two connections may land on two machines that never talk. That is a
  rare operation. This is the packet-rate one, and it touches no database.
  """
  @spec drive(term(), term(), non_neg_integer(), term()) :: :ok | {:error, :fenced}
  def drive(zone_id, avatar, epoch, data) when is_integer(epoch) do
    GenServer.call(via(zone_id), {:drive, avatar, epoch, data})
  end

  @doc "Remove an entity from this zone."
  @spec remove_entity(term(), term()) :: :ok
  def remove_entity(zone_id, entity_id),
    do: GenServer.call(via(zone_id), {:remove_entity, entity_id})

  @doc "The entities this zone is authoritative for, as `%{entity_id => data}`."
  @spec entities(term()) :: %{optional(term()) => term()}
  def entities(zone_id), do: GenServer.call(via(zone_id), :entities)

  ## Handoff: an entity crosses the AOI boundary from one zone to another

  @doc """
  Move an entity from `from_zone_id` to `to_zone_id`, cluster-wide. This is the
  fast in-memory view; it gives up authority on the source and takes it on the
  destination, so exactly one zone owns the entity (barring a crash between the two
  steps). For a durable, crash-safe crossing use `Weft.Entities.handoff/3`, which
  moves the entity-owner pointer in a single FoundationDB transaction, the way
  rivet does it.
  """
  @spec handoff(term(), term(), term()) :: :ok | {:error, :not_found}
  def handoff(from_zone_id, to_zone_id, entity_id) do
    :telemetry.span([:weft, :zone, :handoff], %{}, fn ->
      result =
        case GenServer.call(via(from_zone_id), {:take_entity, entity_id}) do
          {:ok, data} ->
            :ok = GenServer.call(via(to_zone_id), {:put_entity, entity_id, data})
            :ok

          :error ->
            {:error, :not_found}
        end

      {result, %{}}
    end)
  end

  ## Fanout: subscribers get digested snapshots at tick rate

  @doc "Subscribe the calling process to this zone's digested snapshots."
  @spec subscribe(term()) :: :ok
  def subscribe(zone_id), do: GenServer.call(via(zone_id), {:subscribe, self()})

  ## Server

  @impl true
  def init(opts) do
    zone_id = Keyword.fetch!(opts, :zone_id)
    worker_mod = Keyword.get(opts, :worker, Weft.DataPlane.Stub)
    worker_opts = Keyword.get(opts, :worker_opts, [])

    # The worker is linked: if the hot loop dies, the zone dies with it and is
    # restarted (by Horde) on a healthy node, which is the correct failure mode.
    {:ok, worker} = worker_mod.start_link(zone_id, self(), worker_opts)

    {:ok,
     %{
       zone_id: zone_id,
       worker_mod: worker_mod,
       worker: worker,
       latest: nil,
       entities: %{},
       subscribers: MapSet.new(),
       # The highest epoch this zone has accepted for each avatar. This is the fence.
       # See `drive/4`.
       epochs: %{}
     }}
  end

  @impl true
  def handle_info({:dp_snapshot, _zone_id, %Snapshot{} = snapshot}, state) do
    # Fan the digested snapshot out to subscribers (60Hz digested stream).
    Enum.each(state.subscribers, fn pid ->
      send(pid, {:zone_snapshot, state.zone_id, snapshot})
    end)

    {:noreply, %{state | latest: snapshot}}
  end

  def handle_info({:DOWN, _ref, :process, pid, _reason}, state) do
    {:noreply, %{state | subscribers: MapSet.delete(state.subscribers, pid)}}
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

  def handle_call({:add_entity, id, data}, _from, state) do
    {:reply, :ok, %{state | entities: Map.put(state.entities, id, data)}}
  end

  def handle_call({:drive, avatar, epoch, data}, _from, state) do
    # The fence. The comparison and the write are one operation, because this process is
    # the single writer and a GenServer handles one message at a time. Nothing can seize
    # the avatar between the test and the write, since a seizure reaches this zone as
    # another message and messages do not interleave.
    seen = Map.get(state.epochs, avatar, 0)

    if epoch < seen do
      {:reply, {:error, :fenced}, state}
    else
      {:reply, :ok,
       %{
         state
         | entities: Map.put(state.entities, avatar, data),
           epochs: Map.put(state.epochs, avatar, epoch)
       }}
    end
  end

  def handle_call({:remove_entity, id}, _from, state) do
    {:reply, :ok, %{state | entities: Map.delete(state.entities, id)}}
  end

  def handle_call(:entities, _from, state), do: {:reply, state.entities, state}

  def handle_call({:put_entity, id, data}, _from, state) do
    {:reply, :ok, %{state | entities: Map.put(state.entities, id, data)}}
  end

  def handle_call({:take_entity, id}, _from, state) do
    if Map.has_key?(state.entities, id) do
      {data, rest} = Map.pop(state.entities, id)
      {:reply, {:ok, data}, %{state | entities: rest}}
    else
      {:reply, :error, state}
    end
  end

  def handle_call({:subscribe, pid}, _from, state) do
    Process.monitor(pid)
    {:reply, :ok, %{state | subscribers: MapSet.put(state.subscribers, pid)}}
  end

  @impl true
  def terminate(_reason, state) do
    if is_pid(state[:worker]) and Process.alive?(state.worker) do
      state.worker_mod.stop(state.worker)
    end

    :ok
  end
end
