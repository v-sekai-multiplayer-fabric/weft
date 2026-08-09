defmodule RivetEx.Actor do
  @moduledoc """
  A stateful, single-writer actor addressed by `{name, key}`.

  Rivet's core invariant is that at most one actor instance for a given id may run
  or touch its storage at a time (the single-writer invariant for KV and SQLite).
  On the BEAM this is a property of the runtime, not a distributed lease: the
  `RivetEx.Registry` guarantees one process per `{name, key}`, and a process
  handles its mailbox one message at a time, so state mutations are serialized by
  construction. No lease keys, no ping/lost-timeout fencing.

  State here is an in-memory map standing in for per-actor KV. Durable persistence
  (SQLite/CubDB per actor) plugs in behind the same call interface later.
  """

  use GenServer

  @type id :: {name :: String.t(), key :: String.t()}

  @spec start_link(id()) :: GenServer.on_start()
  def start_link({_name, _key} = id) do
    GenServer.start_link(__MODULE__, id, name: via(id))
  end

  @doc "The `:via` tuple that addresses this actor through the registry."
  @spec via(id()) :: {:via, module(), {module(), id()}}
  def via({_name, _key} = id), do: {:via, Registry, {RivetEx.Registry, id}}

  @spec put(GenServer.server(), term(), term()) :: :ok
  def put(server, k, v), do: GenServer.call(server, {:put, k, v})

  @spec get(GenServer.server(), term()) :: term()
  def get(server, k), do: GenServer.call(server, {:get, k})

  @spec info(GenServer.server()) :: map()
  def info(server), do: GenServer.call(server, :info)

  @impl true
  def init({name, key}) do
    {:ok, %{name: name, key: key, kv: %{}, created_at: System.system_time(:millisecond)}}
  end

  @impl true
  def handle_call({:put, k, v}, _from, state) do
    {:reply, :ok, %{state | kv: Map.put(state.kv, k, v)}}
  end

  def handle_call({:get, k}, _from, state) do
    {:reply, Map.get(state.kv, k), state}
  end

  def handle_call(:info, _from, state) do
    {:reply, Map.take(state, [:name, :key, :created_at]), state}
  end
end
