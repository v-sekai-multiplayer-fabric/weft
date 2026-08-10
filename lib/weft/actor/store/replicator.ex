defmodule Weft.Actor.Store.Replicator do
  @moduledoc """
  Node-local worker that replicates actor writes to FoundationDB off the write
  path, so `Weft.Actor.Store.Replicated` acks locally in microseconds.

  FoundationDB layout, per actor `{name, key}` (tuple layer):

    * DELTA `("weft","adelta",name,key,seq)` = `{user_key, value}` — an append log
      of writes, ordered by the monotonic `seq`.
    * SHARD `("weft","ashard",name,key,user_key)` = `value` — the compacted latest
      value per user key.
    * SEQ `("weft","aseq",name,key)` = the highest replicated `seq`, so a fresh
      process continues the log where the last one stopped.

  Compaction folds DELTA rows into the SHARD and deletes them, so the log cannot
  grow without bound (without cleanup the store would exhaust FoundationDB and
  halt). Hydrate reads SHARD then overlays remaining DELTA rows in `seq` order.

  When no FoundationDB cluster is configured the worker is a no-op and the store
  runs local-only: the same one store, without the durable replica.
  """

  use GenServer

  @prefix "weft"
  @delta "adelta"
  @shard "ashard"
  @seq "aseq"

  # Fold DELTA rows into the SHARD after this many replicated writes per actor.
  @compact_threshold 32

  # ── Client ────────────────────────────────────────────────────────────────

  def start_link(opts), do: GenServer.start_link(__MODULE__, opts, name: __MODULE__)

  @doc "Replicate one write asynchronously. Never blocks the caller's write path."
  def replicate(id, seq, user_key, value) do
    GenServer.cast(__MODULE__, {:replicate, id, seq, user_key, value})
  end

  # Both reads below run in the caller, not in the GenServer. The GenServer serializes
  # the replication writes, so a read sent to it queues behind the whole backlog. With
  # 200 queued writes at about 1 ms each, a read waits longer than the call timeout.
  # Neither read touches GenServer state, and the FoundationDB handle is in
  # `:persistent_term`, so the caller can read directly. Reads stay off the write path.

  @doc "Highest replicated seq for an actor, so a new process continues from it."
  def tip({name, key}) do
    case db() do
      nil -> 0
      db -> read_seq(db, name, key)
    end
  end

  @doc "Current durable state for an actor as a list of {user_key, value}."
  def hydrate({name, key}) do
    case db() do
      nil -> []
      db -> read_current(db, name, key)
    end
  end

  # ── Server ────────────────────────────────────────────────────────────────

  @impl true
  def init(_opts), do: {:ok, %{counts: %{}}}

  @impl true
  def handle_cast({:replicate, {name, key} = id, seq, user_key, value}, state) do
    case db() do
      nil ->
        {:noreply, state}

      db ->
        :erlfdb.set(db, delta_key(name, key, seq), :erlang.term_to_binary({user_key, value}))
        :erlfdb.set(db, seq_key(name, key), :erlang.term_to_binary(seq))

        count = Map.get(state.counts, id, 0) + 1

        counts =
          if count >= @compact_threshold do
            compact(db, name, key)
            Map.put(state.counts, id, 0)
          else
            Map.put(state.counts, id, count)
          end

        {:noreply, %{state | counts: counts}}
    end
  end

  # ── FoundationDB ──────────────────────────────────────────────────────────

  # Merge SHARD (base) with remaining DELTA rows in seq order into the current
  # state. This is rivet's read path, run once at open, not per read.
  #
  # Both ranges are read in one transaction, so the read sees one state of the store.
  # Two transactions lose a write. Compaction folds a DELTA row into the SHARD and clears
  # it in one transaction, so a read that takes the SHARD before that commit and the DELTA
  # rows after it misses every row that was folded between the two. `../spec/Store.lean`
  # states the rule as `read_preserved_by_compaction`.
  defp read_current(db, name, key) do
    :erlfdb.transactional(db, fn tx ->
      base =
        tx
        |> :erlfdb.get_range_startswith(shard_prefix(name, key))
        |> Map.new(fn {packed, value} ->
          {@prefix, @shard, ^name, ^key, uk_bin} = :erlfdb_tuple.unpack(packed)
          {:erlang.binary_to_term(uk_bin), :erlang.binary_to_term(value)}
        end)

      tx
      |> :erlfdb.get_range_startswith(delta_prefix(name, key))
      |> Enum.reduce(base, fn {_packed, packed_value}, acc ->
        {user_key, value} = :erlang.binary_to_term(packed_value)
        Map.put(acc, user_key, value)
      end)
      |> Map.to_list()
    end)
  end

  defp read_seq(db, name, key) do
    case :erlfdb.wait(:erlfdb.get(db, seq_key(name, key))) do
      :not_found -> 0
      bin -> :erlang.binary_to_term(bin)
    end
  end

  # Fold every DELTA row into the SHARD and delete it, in one transaction, so the
  # log stays bounded and the SHARD holds the latest value per user key.
  defp compact(db, name, key) do
    :erlfdb.transactional(db, fn tx ->
      tx
      |> :erlfdb.get_range_startswith(delta_prefix(name, key))
      |> Enum.each(fn {delta_k, packed_value} ->
        {user_key, value} = :erlang.binary_to_term(packed_value)
        :erlfdb.set(tx, shard_key(name, key, user_key), :erlang.term_to_binary(value))
        :erlfdb.clear(tx, delta_k)
      end)
    end)
  end

  # One shared FoundationDB handle for the node, opened lazily so it reads config
  # after startup. Returns nil when no cluster is configured (local-only mode).
  defp db do
    case :persistent_term.get({__MODULE__, :db}, :unset) do
      :unset ->
        db =
          case Application.get_env(:weft, :fdb_cluster_file) do
            path when is_binary(path) -> :erlfdb.open(path)
            _ -> nil
          end

        :persistent_term.put({__MODULE__, :db}, db)
        db

      db ->
        db
    end
  end

  defp delta_key(name, key, seq),
    do: :erlfdb_tuple.pack({@prefix, @delta, name, key, seq})

  defp delta_prefix(name, key), do: :erlfdb_tuple.pack({@prefix, @delta, name, key})

  defp shard_key(name, key, user_key),
    do: :erlfdb_tuple.pack({@prefix, @shard, name, key, :erlang.term_to_binary(user_key)})

  defp shard_prefix(name, key), do: :erlfdb_tuple.pack({@prefix, @shard, name, key})

  defp seq_key(name, key), do: :erlfdb_tuple.pack({@prefix, @seq, name, key})
end
