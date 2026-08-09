defmodule RivetEx.Actors do
  @moduledoc """
  Facade for addressing actors, the OTP analogue of pegboard's `get_or_create`.

  `get_or_create/2` resolves an existing actor for `{name, key}` or starts one
  under the actor supervisor, race-safe: if two callers create the same id
  concurrently, the registry admits exactly one and the loser reuses it. This is
  the single-writer guarantee, enforced by the registry rather than by a
  distributed exclusivity protocol.
  """

  @spec get_or_create(String.t(), String.t()) :: {:ok, pid()} | {:error, term()}
  def get_or_create(name, key), do: get_or_create(name, key, 50)

  # Resolve a live actor or start one. A just-slept actor may still be in the
  # registry as a dead pid until the registry processes its :DOWN, and the
  # supervisor may briefly still hold the terminating child; both windows are
  # transient, so we treat a dead/absent entry as "start" and retry through the
  # cleanup rather than hand back a dead pid.
  defp get_or_create(_name, _key, 0), do: {:error, :registry_contended}

  defp get_or_create(name, key, tries) do
    id = {name, key}

    case whereis(name, key) do
      pid when is_pid(pid) ->
        if Process.alive?(pid), do: {:ok, pid}, else: retry(name, key, tries)

      nil ->
        case DynamicSupervisor.start_child(RivetEx.ActorSupervisor, {RivetEx.Actor, id}) do
          {:ok, pid} -> {:ok, pid}
          {:ok, pid, _info} -> {:ok, pid}
          {:error, {:already_started, pid}} -> if_alive(pid, name, key, tries)
          {:error, :already_present} -> retry(name, key, tries)
          :ignore -> {:error, :ignore}
          {:error, _} = err -> err
        end
    end
  end

  defp if_alive(pid, name, key, tries) do
    if Process.alive?(pid), do: {:ok, pid}, else: retry(name, key, tries)
  end

  defp retry(name, key, tries) do
    Process.sleep(1)
    get_or_create(name, key, tries - 1)
  end

  @spec whereis(String.t(), String.t()) :: pid() | nil
  def whereis(name, key) do
    case Registry.lookup(RivetEx.Registry, {name, key}) do
      [{pid, _value}] -> pid
      [] -> nil
    end
  end
end
