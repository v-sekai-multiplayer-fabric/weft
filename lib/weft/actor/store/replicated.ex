defmodule Weft.Actor.Store.Replicated do
  @moduledoc """
  Elixir prototype of the store plane's logic (see `Weft.Actor.Store`). In production
  the store runs natively in its own process for crash isolation, reached over
  iceoryx v1; this module proves the same logic in the BEAM and is tested against a
  live FoundationDB before the native port.

  A local SQLite file per actor in WAL mode is the fast primary
  write path: commits are local and take microseconds, and the caller is acked at
  once. Writes replicate asynchronously to FoundationDB through
  `Weft.Actor.Store.Replicator`, off the write path, so the write never waits for
  FoundationDB.

  On open, a process whose local file does not exist (first start, or handoff to a
  new machine) hydrates the file once from FoundationDB (SHARD plus DELTA), then
  serves all reads locally. The single-writer invariant comes from the actor process
  owning the id, so no lock or lease is needed.

  Latency first: durability is eventual. A crash can lose the last few writes that
  were not yet replicated. This store holds control-plane actor KV only, not game or
  world state (see `Weft.Actor.Store`).
  """

  @behaviour Weft.Actor.Store

  alias Exqlite.Sqlite3
  alias Weft.Actor.Store.Replicator

  @impl true
  def open({name, key} = id) do
    dir = Path.join(data_dir(), sanitize(name))
    File.mkdir_p!(dir)
    path = Path.join(dir, sanitize(key) <> ".db")
    fresh? = not File.exists?(path)

    {:ok, conn} = Sqlite3.open(path)
    # WAL keeps commits local and fast; the durable copy and handoff go through the
    # FoundationDB replica, not a WAL-checkpoint handoff, so WAL is safe here.
    :ok = Sqlite3.execute(conn, "PRAGMA journal_mode=WAL")
    :ok = Sqlite3.execute(conn, "PRAGMA synchronous=NORMAL")

    :ok =
      Sqlite3.execute(
        conn,
        "CREATE TABLE IF NOT EXISTS kv (key BLOB PRIMARY KEY, value BLOB) WITHOUT ROWID"
      )

    # A fresh local file means first start or handoff: rebuild it once from the
    # durable replica. Continue the write log from the highest replicated seq.
    _ =
      if fresh? do
        for {user_key, value} <- Replicator.hydrate(id), do: local_put(conn, user_key, value)
      end

    seq = :atomics.new(1, [])
    :atomics.put(seq, 1, Replicator.tip(id))

    {:ok, %{conn: conn, id: id, seq: seq}}
  end

  @impl true
  def load_all(%{conn: conn}) do
    {:ok, stmt} = Sqlite3.prepare(conn, "SELECT key, value FROM kv")
    rows = fetch_all(conn, stmt, [])
    _ = Sqlite3.release(conn, stmt)

    Map.new(rows, fn [k, v] ->
      {:erlang.binary_to_term(k), :erlang.binary_to_term(v)}
    end)
  end

  @impl true
  def put(%{conn: conn, id: id, seq: seq}, key, value) do
    # Local write is the durable-locally, fast primary. Ack after this.
    local_put(conn, key, value)
    # Replicate off the write path with a monotonic per-actor seq.
    next = :atomics.add_get(seq, 1, 1)
    Replicator.replicate(id, next, key, value)
    :ok
  end

  @impl true
  def close(%{conn: conn}), do: Sqlite3.close(conn)

  defp local_put(conn, key, value) do
    {:ok, stmt} =
      Sqlite3.prepare(
        conn,
        "INSERT INTO kv (key, value) VALUES (?1, ?2) " <>
          "ON CONFLICT(key) DO UPDATE SET value = excluded.value"
      )

    :ok =
      Sqlite3.bind(stmt, [
        {:blob, :erlang.term_to_binary(key)},
        {:blob, :erlang.term_to_binary(value)}
      ])

    :done = Sqlite3.step(conn, stmt)
    _ = Sqlite3.release(conn, stmt)
    :ok
  end

  defp fetch_all(conn, stmt, acc) do
    case Sqlite3.multi_step(conn, stmt) do
      {:done, rows} -> acc ++ rows
      {:rows, rows} -> fetch_all(conn, stmt, acc ++ rows)
      :busy -> fetch_all(conn, stmt, acc)
    end
  end

  defp data_dir do
    Application.get_env(:weft, :data_dir) || Path.join(System.tmp_dir!(), "weft")
  end

  defp sanitize(part), do: String.replace(part, ~r/[^A-Za-z0-9_.-]/, "_")
end
