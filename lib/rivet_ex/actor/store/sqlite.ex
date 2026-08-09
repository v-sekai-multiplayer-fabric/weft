defmodule RivetEx.Actor.Store.Sqlite do
  @moduledoc """
  SQLite-backed actor store: one database file per actor at
  `<data_dir>/<name>/<key>.db`, holding a single `kv` table. Keys and values are
  Erlang terms, serialized with `term_to_binary` and stored as blobs, so any term
  round-trips. Writes are write-through (durable on return).

  This is the OTP analogue of rivet's per-actor SQLite. The single-writer
  invariant is provided by the actor process owning the only handle, so no VFS
  fencing or lease is needed here.
  """

  @behaviour RivetEx.Actor.Store

  alias Exqlite.Sqlite3

  @impl true
  def open({name, key}) do
    dir = Path.join(data_dir(), sanitize(name))
    File.mkdir_p!(dir)
    path = Path.join(dir, sanitize(key) <> ".db")

    # Default rollback journal (not WAL): each write commits to the main database
    # file, so the fresh connection a woken actor opens always sees what the
    # previous one wrote, with no WAL-checkpoint handoff to race.
    with {:ok, conn} <- Sqlite3.open(path),
         :ok <-
           Sqlite3.execute(
             conn,
             "CREATE TABLE IF NOT EXISTS kv (key BLOB PRIMARY KEY, value BLOB) WITHOUT ROWID"
           ) do
      {:ok, conn}
    end
  end

  @impl true
  def load_all(conn) do
    {:ok, stmt} = Sqlite3.prepare(conn, "SELECT key, value FROM kv")

    rows = fetch_all(conn, stmt, [])
    Sqlite3.release(conn, stmt)

    Map.new(rows, fn [k, v] ->
      {:erlang.binary_to_term(k), :erlang.binary_to_term(v)}
    end)
  end

  @impl true
  def put(conn, key, value) do
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
    Sqlite3.release(conn, stmt)
    :ok
  end

  @impl true
  def close(conn), do: Sqlite3.close(conn)

  defp fetch_all(conn, stmt, acc) do
    case Sqlite3.multi_step(conn, stmt) do
      {:done, rows} -> acc ++ rows
      {:rows, rows} -> fetch_all(conn, stmt, acc ++ rows)
      :busy -> fetch_all(conn, stmt, acc)
    end
  end

  defp data_dir do
    Application.get_env(:rivet_ex, :data_dir) ||
      Path.join(System.tmp_dir!(), "rivet_ex")
  end

  # Keep ids that are not filesystem-safe from escaping their directory.
  defp sanitize(part) do
    String.replace(part, ~r/[^A-Za-z0-9_.-]/, "_")
  end
end
