defmodule Weft.DataPlane.AsciiScopeTest do
  @moduledoc """
  The scope renders a 3D entity cloud as four ASCII panels (top, front, side, iso)
  plus a stats line. Covers the panel labels, one mark per panel per point, and the
  ring reader with tick and count stats.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.AsciiScope
  alias Weft.DataPlane.Ring

  @opts [width: 4, height: 4, x_range: {0, 3000}, y_range: {0, 3000}, z_range: {0, 3000}]

  test "render draws four labelled panels and a mark per panel per point" do
    out = AsciiScope.render([1500, 0, 3000], @opts ++ [stats: ["hello 1"]])
    for label <- ["top", "front", "side", "iso"], do: assert(out =~ label)
    assert out =~ "hello 1"
    # One point appears once in each of the four panels.
    assert out |> String.graphemes() |> Enum.count(&(&1 == "O")) == 4
  end

  test "origin padding is skipped" do
    out = AsciiScope.render([0, 0, 0], @opts)
    assert out |> String.graphemes() |> Enum.count(&(&1 == "O")) == 0
  end

  test "of_ring reads the snapshot and reports tick and entity count" do
    ring = Ring.new(2)
    Ring.write(ring, 8, [1500, 0, 3000, 0, 0, 0])
    assert {8, out} = AsciiScope.of_ring(ring, @opts)
    assert out =~ "tick 8"
    assert out =~ "entities 1"
  end
end
