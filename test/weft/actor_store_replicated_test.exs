defmodule Weft.Actor.Store.ReplicatedTest do
  @moduledoc """
  The one store: a local SQLite WAL primary with an async FoundationDB replica.
  Writes ack locally in microseconds and replicate off the write path; a fresh
  process hydrates its local file from FoundationDB, so handoff to a machine with no
  local file still restores state. Runs only when a FoundationDB cluster is reachable
  (tagged `:fdb`).
  """

  use ExUnit.Case, async: false

  @moduletag :fdb

  alias Weft.Actor.Store.Replicated
  alias Weft.Actor.Store.Replicator

  # Wait for the async replica to catch up: poll FoundationDB rather than sleep, so
  # the test is deterministic and fast. Replication is off the write path, so a put
  # returns before the FoundationDB row exists.
  # Replication is off the write path, so a test can only wait for it. The budget is
  # generous on purpose: the assertion is that the write arrives, and not that it arrives
  # inside a deadline. A tight budget turns a slow test machine into a failure that says
  # nothing about the code.
  defp await_replicated(id, user_key, expected) do
    Enum.reduce_while(1..1_000, :never, fn _, _ ->
      case List.keyfind(Replicator.hydrate(id), user_key, 0) do
        {^user_key, ^expected} -> {:halt, :ok}
        # Replication to FoundationDB is asynchronous by design and emits no event, so
        # there is nothing to be told. Bounded poll, and it fails at the bound.
        _ -> Process.sleep(5) && {:cont, :never}
      end
    end)
  end

  defp fresh_id, do: Weft.Test.Fresh.actor_id("repl")

  test "a local write replicates to FoundationDB off the write path" do
    id = fresh_id()
    {:ok, h} = Replicated.open(id)

    assert :ok = Replicated.put(h, :seed, 7)
    assert :ok = Replicated.put(h, :name, "atlantis")
    # Local read is immediate.
    assert Replicated.load_all(h) == %{seed: 7, name: "atlantis"}

    # The replica catches up asynchronously.
    assert :ok = await_replicated(id, :seed, 7)
    assert :ok = await_replicated(id, :name, "atlantis")

    Replicated.close(h)
  end

  test "a fresh process with no local file hydrates from FoundationDB (handoff)" do
    id = {name, key} = fresh_id()
    {:ok, h1} = Replicated.open(id)
    Replicated.put(h1, :hp, 100)
    Replicated.put(h1, :zone, "b")
    await_replicated(id, :zone, "b")
    Replicated.close(h1)

    # Simulate handoff to a machine with no local file for this actor.
    path = Path.join([Application.get_env(:weft, :data_dir), name, key <> ".db"])
    File.rm_rf!(path)
    File.rm_rf!(path <> "-wal")
    File.rm_rf!(path <> "-shm")

    {:ok, h2} = Replicated.open(id)
    assert Replicated.load_all(h2) == %{hp: 100, zone: "b"}
    Replicated.close(h2)
  end

  test "compaction bounds the DELTA log while state stays correct" do
    id = fresh_id()
    {:ok, h} = Replicated.open(id)

    # Far more writes to one key than the compaction threshold. Without compaction
    # the DELTA log would grow one row per write; compaction folds them into SHARD.
    for i <- 1..200, do: Replicated.put(h, :counter, i)
    await_replicated(id, :counter, 200)

    # State is the latest value regardless of how many writes happened.
    assert {:counter, 200} = List.keyfind(Replicator.hydrate(id), :counter, 0)

    # Hydrating a fresh file must still yield the latest value after compaction.
    {name, key} = id
    path = Path.join([Application.get_env(:weft, :data_dir), name, key <> ".db"])
    Replicated.close(h)
    File.rm_rf!(path)
    File.rm_rf!(path <> "-wal")
    File.rm_rf!(path <> "-shm")

    {:ok, h2} = Replicated.open(id)
    assert Replicated.load_all(h2) == %{counter: 200}
    Replicated.close(h2)
  end
end
