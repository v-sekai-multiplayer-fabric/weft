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

    Process.sleep(20)
    assert {t2, _} = Native.sample(ref)
    assert t2 > t1

    assert :ok = Native.stop(ref)
  end

  test "reports the native production rate" do
    assert {:ok, ref} = Native.start(8)

    {t0, _} = Native.sample(ref)
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
