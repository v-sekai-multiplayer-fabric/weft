defmodule Weft.Actors do
  @moduledoc """
  Facade for addressing actors across the cluster, the OTP analogue of pegboard's
  `get_or_create` plus its exclusivity guarantee.

  `get_or_create/2` resolves an existing actor for `{name, key}` anywhere in the
  cluster or starts one under the distributed supervisor. `Horde.Registry`
  admits exactly one owner per id cluster-wide, so this is the single-writer
  guarantee without a lease. When a node leaves, `Horde.DynamicSupervisor` hands
  its actors to a surviving node (pegboard failover).

  Note on consistency: Horde is CRDT-based and chooses availability. During a
  network partition each side may briefly run its own instance; Horde resolves the
  conflict when the cluster heals. Strict single-writer *through* a partition needs
  a consensus/storage layer (what rivet gets from FoundationDB); see the store
  roadmap.
  """

  @spec get_or_create(String.t(), String.t()) :: {:ok, pid()} | {:error, term()}
  def get_or_create(name, key) do
    :telemetry.span([:weft, :actors, :get_or_create], %{name: name}, fn ->
      {get_or_create(name, key, 50), %{}}
    end)
  end

  # Resolve a live actor or start one. A just-slept or just-lost actor may linger
  # in the registry as a dead pid until Horde converges; we treat a dead/absent
  # entry as "start" and retry through the window rather than return a dead pid.
  defp get_or_create(_name, _key, 0), do: {:error, :registry_contended}

  defp get_or_create(name, key, tries) do
    id = {name, key}

    case whereis(name, key) do
      pid when is_pid(pid) ->
        if alive?(pid), do: {:ok, pid}, else: retry(name, key, tries)

      nil ->
        case Horde.DynamicSupervisor.start_child(Weft.ActorSupervisor, child_spec(id)) do
          {:ok, pid} -> {:ok, pid}
          {:ok, pid, _info} -> {:ok, pid}
          {:error, {:already_started, pid}} -> if_alive(pid, name, key, tries)
          {:error, :already_present} -> retry(name, key, tries)
          :ignore -> {:error, :ignore}
          {:error, _} = err -> err
        end
    end
  end

  @spec whereis(String.t(), String.t()) :: pid() | nil
  def whereis(name, key) do
    case Horde.Registry.lookup(Weft.Registry, {name, key}) do
      [{pid, _value}] -> pid
      [] -> nil
    end
  end

  # Unique child id per actor so Horde tracks and redistributes each distinctly.
  defp child_spec({_name, _key} = id) do
    %{id: {Weft.Actor, id}, start: {Weft.Actor, :start_link, [id]}, restart: :transient}
  end

  defp if_alive(pid, name, key, tries) do
    if alive?(pid), do: {:ok, pid}, else: retry(name, key, tries)
  end

  defp retry(name, key, tries) do
    Process.sleep(1)
    get_or_create(name, key, tries - 1)
  end

  # Liveness that works for both local and remote pids. `Process.alive?/1` only
  # accepts local pids, so a pid on another node is probed over RPC; a downed node
  # answers as not-alive.
  defp alive?(pid) do
    if node(pid) == node() do
      Process.alive?(pid)
    else
      :rpc.call(node(pid), Process, :alive?, [pid], 1_000) == true
    end
  end
end
