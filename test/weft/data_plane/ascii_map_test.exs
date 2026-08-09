defmodule Weft.DataPlane.AsciiMapTest do
  @moduledoc """
  The ASCII map renders a ring snapshot as a terminal grid, the console playback of
  the stress bench. Covers placement, origin-padding skip, and reading from a ring.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.AsciiMap
  alias Weft.DataPlane.Ring

  @opts [width: 4, height: 4, x_range: {0, 3000}, y_range: {0, 3000}]

  test "render places a mark per entity in the scaled grid" do
    grid = AsciiMap.render([1000, 0, 0, 3000, 3000, 0], @opts)
    assert grid == ".O..\n....\n....\n...O"
  end

  test "origin padding is skipped" do
    assert AsciiMap.render([0, 0, 0, 0, 0, 0], @opts) == String.duplicate(".", 4) |> rows(4)
  end

  test "of_ring reads the latest snapshot and renders it" do
    ring = Ring.new(2)
    Ring.write(ring, 5, [0, 0, 0, 3000, 3000, 0])
    assert {5, grid} = AsciiMap.of_ring(ring, @opts)
    assert grid == "....\n....\n....\n...O"
  end

  defp rows(row, n), do: List.duplicate(row, n) |> Enum.join("\n")
end
