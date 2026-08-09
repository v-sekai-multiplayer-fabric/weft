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
  def get_or_create(name, key) do
    id = {name, key}

    case whereis(name, key) do
      nil ->
        case DynamicSupervisor.start_child(RivetEx.ActorSupervisor, {RivetEx.Actor, id}) do
          {:ok, pid} -> {:ok, pid}
          {:error, {:already_started, pid}} -> {:ok, pid}
          other -> other
        end

      pid ->
        {:ok, pid}
    end
  end

  @spec whereis(String.t(), String.t()) :: pid() | nil
  def whereis(name, key) do
    case Registry.lookup(RivetEx.Registry, {name, key}) do
      [{pid, _value}] -> pid
      [] -> nil
    end
  end
end
