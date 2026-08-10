defmodule Weft.DataPlane.NativeTest do
  @moduledoc """
  The real native worker: a native thread produces snapshots into the ring, the
  BEAM samples them. Also reports the native production rate.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.Native

  test "native worker produces advancing snapshots the BEAM samples" do
    assert {:ok, ref} = Native.start(8)

    assert {t1, coords} = Native.sample(ref)
    assert length(coords) == 24

    # The producer is a native thread, so there is no message to wait for and no process
    # to call. Sample until the tick moves, rather than sleeping once and hoping it did.
    # This returns as soon as it moves, and it fails loudly if it never does.
    assert {t2, _} = advanced_past(ref, t1)
    assert t2 > t1

    assert :ok = Native.stop(ref)
  end

  # Sample until the tick passes `t`. The producer is native, so polling is the only
  # option: nothing here can be told. Bounded, so a stalled producer fails the test rather
  # than hanging it.
  defp advanced_past(ref, t, tries \\ 2_000) do
    case Native.sample(ref) do
      {t2, _} = sample when t2 > t -> sample
      # Bounded poll, per the comment above: a native thread has no mailbox.
      _ when tries > 0 -> Process.sleep(1) && advanced_past(ref, t, tries - 1)
      _ -> flunk("the native producer never advanced past tick #{t}")
    end
  end

  test "reports the native production rate" do
    assert {:ok, ref} = Native.start(8)

    {t0, _} = Native.sample(ref)
    # A real duration, and the one case where a sleep is right: the elapsed time is the
    # measurement, not a wait for something to happen.
    window_ms = 200
    Process.sleep(window_ms)
    {t1, _} = Native.sample(ref)

    rate = (t1 - t0) * 1000 / window_ms

    IO.puts(
      "\nnative worker: #{:erlang.float_to_binary(rate / 1_000_000, decimals: 1)}M snapshots/sec"
    )

    assert rate > 1_000_000
    Native.stop(ref)
  end
end
