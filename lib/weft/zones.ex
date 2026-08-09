defmodule Weft.Zones do
  @moduledoc """
  Facade for starting zones under the distributed supervisor, the zone analogue of
  `Weft.Actors`.

  `ensure/2` starts a zone under `Horde.DynamicSupervisor` so it is placed across
  the cluster and handed off on node loss, rather than linked to a transient caller
  that would take it down. Idempotent per `zone_id`.
  """

  @spec ensure(term(), keyword()) :: {:ok, pid()} | {:error, term()}
  def ensure(zone_id, opts \\ []) do
    child = %{
      id: {Weft.Zone, zone_id},
      start: {Weft.Zone, :start_link, [Keyword.put(opts, :zone_id, zone_id)]},
      restart: :transient
    }

    case Horde.DynamicSupervisor.start_child(Weft.ActorSupervisor, child) do
      {:ok, pid} -> {:ok, pid}
      {:ok, pid, _info} -> {:ok, pid}
      {:error, {:already_started, pid}} -> {:ok, pid}
      :ignore -> {:error, :ignore}
      {:error, _} = err -> err
    end
  end
end
