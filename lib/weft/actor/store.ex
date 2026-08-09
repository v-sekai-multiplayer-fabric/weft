defmodule Weft.Actor.Store do
  @moduledoc """
  Durable storage behaviour for an actor's key/value state.

  One store per actor. The actor is the single writer for its store, so
  implementations need no locking or lease. This is the seam where rivet's
  SQLite-backed actor storage plugs in; `Weft.Actor.Store.Sqlite` is the
  default, and CubDB or a remote backend can implement the same behaviour.
  """

  @type handle :: term()

  @callback open(Weft.Actor.id()) :: {:ok, handle()} | {:error, term()}
  @callback load_all(handle()) :: %{optional(term()) => term()}
  @callback put(handle(), key :: term(), value :: term()) :: :ok
  @callback close(handle()) :: :ok

  @doc "The configured store implementation (default: SQLite)."
  @spec impl() :: module()
  def impl, do: Application.get_env(:weft, :actor_store, Weft.Actor.Store.Sqlite)
end
