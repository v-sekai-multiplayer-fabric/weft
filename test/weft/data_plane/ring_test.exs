defmodule Weft.DataPlane.RingTest do
  use ExUnit.Case, async: true

  alias Weft.DataPlane.Ring

  test "write then read round-trips the tick and coordinates" do
    ring = Ring.new(3)
    :ok = Ring.write(ring, 7, [1, 2, 3, 4, 5, 6, 7, 8, 9])
    assert {7, [1, 2, 3, 4, 5, 6, 7, 8, 9]} == Ring.read(ring)
  end

  test "read reflects the latest write (overwrite semantics)" do
    ring = Ring.new(2)
    Ring.write(ring, 1, [0, 0, 0, 0, 0, 0])
    Ring.write(ring, 2, [10, 11, 12, 13, 14, 15])
    assert {2, [10, 11, 12, 13, 14, 15]} == Ring.read(ring)
  end

  test "reads stay tear-free under a concurrent writer (seqlock)" do
    ring = Ring.new(2)
    iterations = 100_000

    # The writer sets every coordinate equal to the tick, so a torn read (coords
    # not all equal to the tick) would be observable if the seqlock were broken.
    writer =
      Task.async(fn ->
        Enum.each(1..iterations, fn n -> Ring.write(ring, n, List.duplicate(n, 6)) end)
      end)

    Enum.each(1..iterations, fn _ ->
      {tick, coords} = Ring.read(ring)
      assert Enum.all?(coords, &(&1 == tick)), "torn read: tick=#{tick} coords=#{inspect(coords)}"
    end)

    Task.await(writer, 10_000)
  end
end
