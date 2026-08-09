defmodule Weft.InterestTest do
  @moduledoc """
  The interest feed produces a read-only area-of-interest replica for an observer,
  the CH_INTEREST snapshot. Covers the range test, selection, and reading the replica
  from a ring snapshot.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.Ring
  alias Weft.Interest

  test "in_range? is true inside the sphere and false outside" do
    assert Interest.in_range?({0, 1000, 0, 0}, {0, 0, 0}, 2000)
    refute Interest.in_range?({0, 3000, 0, 0}, {0, 0, 0}, 2000)
  end

  test "select keeps only the entities inside the area of interest" do
    entities = [{0, 0, 0, 0}, {1, 1000, 0, 0}, {2, 5000, 0, 0}]
    assert Interest.select(entities, {0, 0, 0}, 2000) == [{0, 0, 0, 0}, {1, 1000, 0, 0}]
  end

  test "from_ring returns the replica for an observer, culling the rest" do
    # Three entities in a 3-slot ring; the observer sits near the first two.
    ring = Ring.new(3)
    Ring.write(ring, 7, [1000, 1000, 0, 1500, 1000, 0, 9000, 9000, 0])

    assert {7, replica} = Interest.from_ring(ring, %{center: {1000, 1000, 0}, radius: 1000})
    assert replica == [{0, 1000, 1000, 0}, {1, 1500, 1000, 0}]
  end
end
