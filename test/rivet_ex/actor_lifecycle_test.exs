defmodule RivetEx.ActorLifecycleTest do
  @moduledoc """
  Per-actor scale-to-zero. An actor with no activity for its idle window stops and
  releases its process (rivet's "sleep"), keeping only its durable SQLite state.
  The next request wakes a fresh process that restores that state. This is the
  serverless actor lifecycle without a keepalive counter: liveness is the process,
  and durability is the database.
  """

  use ExUnit.Case, async: false

  alias RivetEx.{Actor, Actors}

  setup do
    prev = Application.get_env(:rivet_ex, :actor_idle_ms)
    Application.put_env(:rivet_ex, :actor_idle_ms, 50)

    on_exit(fn ->
      if prev,
        do: Application.put_env(:rivet_ex, :actor_idle_ms, prev),
        else: Application.delete_env(:rivet_ex, :actor_idle_ms)
    end)

    :ok
  end

  test "an idle actor sleeps and wakes with durable state" do
    key = "sleep-#{System.unique_integer([:positive])}"

    {:ok, pid} = Actors.get_or_create("zone", key)
    Actor.put(pid, :hp, 7)

    # After the idle window with no activity, the actor stops and releases its
    # process (scale to zero). The released process is the deterministic invariant;
    # the registry clears its dead entry asynchronously, which get_or_create below
    # tolerates.
    ref = Process.monitor(pid)
    assert_receive {:DOWN, ^ref, :process, ^pid, :normal}, 1_000
    refute Process.alive?(pid)

    # Next access wakes a fresh process with state restored from disk.
    {:ok, pid2} = Actors.get_or_create("zone", key)
    assert pid2 != pid
    assert Actor.get(pid2, :hp) == 7
  end

  test "activity resets the idle timer" do
    key = "active-#{System.unique_integer([:positive])}"

    {:ok, pid} = Actors.get_or_create("zone", key)

    # Poke under the idle window repeatedly; each call resets the timer so the
    # actor stays alive well past a single idle window.
    for _ <- 1..5 do
      Process.sleep(20)
      Actor.put(pid, :beat, :ok)
    end

    assert Process.alive?(pid)
  end
end
