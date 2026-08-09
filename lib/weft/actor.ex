defmodule Weft.Actor do
  @moduledoc """
  A stateful, single-writer actor addressed by `{name, key}`.

  Rivet's core invariant is that at most one actor instance for a given id may run
  or touch its storage at a time (the single-writer invariant for KV and SQLite).
  On the BEAM this is a property of the runtime, not a distributed lease: the
  `Weft.Registry` guarantees one process per `{name, key}`, and a process
  handles its mailbox one message at a time, so state mutations are serialized by
  construction. No lease keys, no ping/lost-timeout fencing.

  State here is an in-memory map standing in for per-actor KV. Durable persistence
  (SQLite/CubDB per actor) plugs in behind the same call interface later.
  """

  # :transient so a crashed actor is restarted (durable state restored) but an
  # idle actor that stops :normal stays down until the next request wakes it.
  use GenServer, restart: :transient

  @type id :: {name :: String.t(), key :: String.t()}

  @spec start_link(id()) :: GenServer.on_start()
  def start_link({_name, _key} = id) do
    GenServer.start_link(__MODULE__, id, name: via(id))
  end

  @doc "The `:via` tuple that addresses this actor through the distributed registry."
  @spec via(id()) :: {:via, module(), {module(), id()}}
  def via({_name, _key} = id), do: {:via, Horde.Registry, {Weft.Registry, id}}

  @spec put(GenServer.server(), term(), term()) :: :ok
  def put(server, k, v) do
    :telemetry.span([:weft, :actor, :put], %{}, fn ->
      {GenServer.call(server, {:put, k, v}), %{}}
    end)
  end

  @spec get(GenServer.server(), term()) :: term()
  def get(server, k) do
    :telemetry.span([:weft, :actor, :get], %{}, fn -> {GenServer.call(server, {:get, k}), %{}} end)
  end

  @spec info(GenServer.server()) :: map()
  def info(server), do: GenServer.call(server, :info)

  @impl true
  def init({name, key} = id) do
    store = Weft.Actor.Store.impl()
    {:ok, handle} = store.open(id)
    # Restore durable state so a fresh process for this id resumes where the last
    # one left off. This is the wake-from-sleep / restart-after-crash path.
    kv = store.load_all(handle)

    state = %{
      name: name,
      key: key,
      store: store,
      handle: handle,
      kv: kv,
      idle_ms: Application.get_env(:weft, :actor_idle_ms, :infinity),
      created_at: System.system_time(:millisecond)
    }

    {:ok, state, state.idle_ms}
  end

  @impl true
  def handle_call({:put, k, v}, _from, state) do
    # Write through to durable storage before acknowledging, then update the cache.
    :ok = state.store.put(state.handle, k, v)
    {:reply, :ok, %{state | kv: Map.put(state.kv, k, v)}, state.idle_ms}
  end

  def handle_call({:get, k}, _from, state) do
    {:reply, Map.get(state.kv, k), state, state.idle_ms}
  end

  def handle_call(:info, _from, state) do
    {:reply, Map.take(state, [:name, :key, :created_at]), state, state.idle_ms}
  end

  # No activity within the idle window: sleep by stopping normally. Durable state
  # is already on disk, so the next request wakes a fresh process that restores it.
  @impl true
  def handle_info(:timeout, state) do
    {:stop, :normal, state}
  end

  @impl true
  def terminate(_reason, state) do
    if state[:store] && state[:handle], do: state.store.close(state.handle)
    :ok
  end
end
