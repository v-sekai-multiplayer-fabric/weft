defmodule Weft.ActorPersistenceTest do
  @moduledoc """
  Durable per-actor state: rivet's core value. An actor's KV survives the process
  ending (sleep, crash, redeploy) because it is written through to a per-actor
  SQLite database. Because each actor is the single writer for its own database,
  there is no contention and no lease.
  """

  use ExUnit.Case, async: false

  alias Weft.{Actor, Actors}

  test "actor state survives a restart" do
    key = "persist-#{System.unique_integer([:positive])}"

    {:ok, pid} = Actors.get_or_create("zone", key)
    Actor.put(pid, :world_seed, 42)
    Actor.put(pid, :name, "atlantis")

    # End the actor process, as sleep/crash/redeploy would.
    ref = Process.monitor(pid)
    :ok = GenServer.stop(pid, :normal)
    assert_receive {:DOWN, ^ref, :process, ^pid, _}, 1_000

    # A fresh process for the same id restores durable state from disk.
    {:ok, pid2} = Actors.get_or_create("zone", key)
    assert pid2 != pid
    assert Actor.get(pid2, :world_seed) == 42
    assert Actor.get(pid2, :name) == "atlantis"
  end

  test "distinct actors keep isolated state" do
    {:ok, a} = Actors.get_or_create("zone", "iso-a-#{System.unique_integer([:positive])}")
    {:ok, b} = Actors.get_or_create("zone", "iso-b-#{System.unique_integer([:positive])}")

    Actor.put(a, :v, "a")
    Actor.put(b, :v, "b")

    assert Actor.get(a, :v) == "a"
    assert Actor.get(b, :v) == "b"
  end
end
