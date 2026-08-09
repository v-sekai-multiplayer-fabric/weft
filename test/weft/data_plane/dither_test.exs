defmodule Weft.DataPlane.DitherTest do
  @moduledoc """
  Floyd-Steinberg dithering, ported from docs/spec/Dither.lean. These tests mirror the Lean
  proofs (nearest correctness, weight conservation) and cover the 2D dither.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.Dither

  test "nearest matches the Lean spec" do
    pal = [0, 85, 170, 255]
    assert Dither.nearest(pal, 100) == 85
    assert Dither.nearest(pal, 200) == 170
    assert Dither.nearest(pal, 43) == 85
    assert Dither.nearest([], 7) == 7
  end

  test "the kernel weights sum to 16 (error conservation, Lean fs_conserves)" do
    assert Dither.weight_sum() == 16
  end

  test "dithering maps every cell into the palette" do
    grid = for _ <- 1..4, do: for(_ <- 1..4, do: 128)
    out = Dither.dither(grid, [0, 255])
    flat = List.flatten(out)
    assert Enum.all?(flat, &(&1 in [0, 255]))
    # a flat mid-gray field dithers to a mix of both levels.
    assert 0 in flat and 255 in flat
  end

  test "a grid already on the palette dithers to itself" do
    grid = [[0, 255], [255, 0]]
    assert Dither.dither(grid, [0, 255]) == grid
  end
end
