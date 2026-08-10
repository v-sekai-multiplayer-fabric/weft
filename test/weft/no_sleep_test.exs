defmodule Weft.NoSleepTest do
  use ExUnit.Case, async: true

  @moduledoc """
  A sleep is not a way to wait for something.

  Two flakes in one day had the same shape, and neither looked like the other at first.
  `Weft.GatewayTest` dispatched to a zone that `Horde.Registry.lookup` could not see yet.
  `Weft.ZoneTest` paused a worker with a cast and slept, hoping the cast had landed. Both
  were an asynchronous operation treated as synchronous, with a sleep covering the gap.

  Fixing them one at a time is whack-a-mole. The class is what needs closing.

  ## The rule

  A wait is a message or a call, and never a sleep.

  - **Something will happen** — subscribe and `assert_receive`. `Weft.Zone.subscribe/1`
    already fans every tick out, so a test is told rather than asking.
  - **Something has already happened** — call the process. A `GenServer` mailbox is a
    queue, so a call returns after every message sent before it. That is how
    `Weft.Gateway` resolves a registry miss, and how a paused worker is confirmed paused.
  - **Something will not happen** — `refute_receive`. Absence is the one thing no message
    can announce, so it needs a window, and this is ExUnit's primitive for it.
  - **Something ended** — monitor it and match the `:DOWN`.

  ## When a sleep is right

  When the elapsed time is the measurement, and not a wait. Two remain, and each says so
  on the line above it:

  - `Weft.DataPlane.NativeTest` sleeps for the window it divides by to get a rate.
  - `Weft.ActorLifecycleTest` sleeps to advance past an idle window, which is the thing
    under test.

  A bounded poll is the third case, and only where nothing can be told: a native thread
  has no mailbox. Those loops fail loudly at the bound rather than hanging.

  This test holds the rule. A new `Process.sleep` needs a comment on the line above it
  saying why, which is a low bar on purpose: the point is to make someone write the reason
  down, because writing it down is where a sleep-as-a-wait falls over.
  """

  @roots ["test/weft", "test/support"]

  test "every Process.sleep says why it is not a wait" do
    unexplained =
      @roots
      |> Enum.flat_map(&Path.wildcard(&1 <> "/**/*.{ex,exs}"))
      |> Enum.reject(&(&1 == __ENV__.file |> Path.relative_to_cwd()))
      |> Enum.flat_map(fn path ->
        lines = path |> File.read!() |> String.split("\n")

        lines
        |> Enum.with_index()
        |> Enum.filter(fn {line, _i} -> String.contains?(line, "Process.sleep") end)
        |> Enum.reject(fn {_line, i} ->
          # A reason on any of the three lines above it. Three, because a reason worth
          # writing rarely fits on one.
          lines
          |> Enum.slice(max(i - 3, 0), min(i, 3))
          |> Enum.any?(&String.contains?(&1, "#"))
        end)
        |> Enum.map(fn {line, i} -> "#{path}:#{i + 1}: #{String.trim(line)}" end)
      end)

    assert unexplained == [],
           "a Process.sleep with no reason above it. A wait is a message or a call, " <>
             "never a sleep. See the moduledoc of this file.\n" <>
             Enum.join(unexplained, "\n")
  end
end
