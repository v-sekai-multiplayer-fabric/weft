defmodule RivetEx.Actor.Store.Fdb do
  @moduledoc """
  FoundationDB-backed actor store: durable per-actor state that any node in the
  cluster can reach, so an actor can be handed off to a different machine and still
  read its data. This is the backend that makes real multi-machine handoff work,
  and it is the same choice rivet makes.

  Each actor occupies a tuple-layer subspace `("rivet_ex", "actor", name, key)`.
  Keys and values are Erlang terms serialized with `term_to_binary`. A single
  database handle is shared process-wide; each `put` is its own committed
  transaction, and `load_all` reads the actor's subspace in one snapshot.

  Unlike node-local SQLite, this store has no filesystem affinity, so it composes
  with `Horde` handoff across machines. The single-writer invariant still comes
  from the actor process owning the id (one writer per subspace), so no explicit
  locking is needed.
  """

  @behaviour RivetEx.Actor.Store

  @prefix "rivet_ex"
  @kind "actor"

  @impl true
  def open({name, key}) do
    {:ok, {db(), name, key}}
  end

  @impl true
  def load_all({db, name, key}) do
    prefix = :erlfdb_tuple.pack({@prefix, @kind, name, key})

    db
    |> :erlfdb.get_range_startswith(prefix)
    |> Map.new(fn {packed_key, packed_value} ->
      {@prefix, @kind, ^name, ^key, user_key_bin} = :erlfdb_tuple.unpack(packed_key)
      {:erlang.binary_to_term(user_key_bin), :erlang.binary_to_term(packed_value)}
    end)
  end

  @impl true
  def put({db, name, key}, user_key, value) do
    entry_key =
      :erlfdb_tuple.pack({@prefix, @kind, name, key, :erlang.term_to_binary(user_key)})

    :erlfdb.set(db, entry_key, :erlang.term_to_binary(value))
    :ok
  end

  @impl true
  def close(_handle), do: :ok

  # One shared database handle for the node, opened lazily against the configured
  # cluster file. erlfdb's handle is safe to share across processes.
  defp db do
    case :persistent_term.get({__MODULE__, :db}, nil) do
      nil ->
        db = :erlfdb.open(cluster_file())
        :persistent_term.put({__MODULE__, :db}, db)
        db

      db ->
        db
    end
  end

  defp cluster_file do
    case Application.get_env(:rivet_ex, :fdb_cluster_file) do
      path when is_binary(path) -> path
      nil -> raise "RivetEx.Actor.Store.Fdb requires config :rivet_ex, :fdb_cluster_file"
    end
  end
end
