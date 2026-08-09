defmodule RivetEx.ActorsTest do
  use ExUnit.Case, async: true

  alias RivetEx.{Actor, Actors}

  test "an actor is a single writer addressed by {name, key}" do
    key = "z-#{System.unique_integer([:positive])}"
    {:ok, p1} = Actors.get_or_create("zone", key)
    {:ok, p2} = Actors.get_or_create("zone", key)

    # Same id resolves to the same process: one writer, enforced by the registry.
    assert p1 == p2

    Actor.put(p1, :players, 3)
    assert Actor.get(p2, :players) == 3
  end

  test "concurrent get_or_create for the same id yields one process" do
    key = "race-#{System.unique_integer([:positive])}"

    pids =
      1..25
      |> Task.async_stream(
        fn _ ->
          {:ok, pid} = Actors.get_or_create("zone", key)
          pid
        end,
        max_concurrency: 25
      )
      |> Enum.map(fn {:ok, pid} -> pid end)

    assert pids |> Enum.uniq() |> length() == 1
  end
end
