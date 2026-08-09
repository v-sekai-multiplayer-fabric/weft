defmodule RivetEx.Actor.Store.FdbTest do
  @moduledoc """
  Durable actor state in FoundationDB. Unlike node-local SQLite, this state has no
  filesystem affinity, so an actor handed off to another machine still reads it.
  Runs only when a FoundationDB cluster is reachable (tagged `:fdb`).
  """

  use ExUnit.Case, async: false

  @moduletag :fdb

  alias RivetEx.{Actor, Actors}

  setup do
    prev = Application.get_env(:rivet_ex, :actor_store)
    Application.put_env(:rivet_ex, :actor_store, RivetEx.Actor.Store.Fdb)

    on_exit(fn ->
      if prev,
        do: Application.put_env(:rivet_ex, :actor_store, prev),
        else: Application.delete_env(:rivet_ex, :actor_store)
    end)

    :ok
  end

  test "actor state persists in FoundationDB across a restart" do
    key = "fdb-#{System.unique_integer([:positive])}"

    {:ok, pid} = Actors.get_or_create("zone", key)
    Actor.put(pid, :seed, 1234)
    Actor.put(pid, :name, "atlantis")

    ref = Process.monitor(pid)
    :ok = GenServer.stop(pid, :normal)
    assert_receive {:DOWN, ^ref, :process, ^pid, _}, 1_000

    # A fresh process restores from FoundationDB, not from any local file.
    {:ok, pid2} = Actors.get_or_create("zone", key)
    assert pid2 != pid
    assert Actor.get(pid2, :seed) == 1234
    assert Actor.get(pid2, :name) == "atlantis"
  end

  test "distinct actors occupy isolated FoundationDB subspaces" do
    {:ok, a} = Actors.get_or_create("zone", "fdb-iso-a-#{System.unique_integer([:positive])}")
    {:ok, b} = Actors.get_or_create("zone", "fdb-iso-b-#{System.unique_integer([:positive])}")

    Actor.put(a, :v, "a")
    Actor.put(b, :v, "b")

    assert Actor.get(a, :v) == "a"
    assert Actor.get(b, :v) == "b"
  end
end
