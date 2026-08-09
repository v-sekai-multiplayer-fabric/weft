defmodule Weft.DataPlane.AsciiScopeTest do
  @moduledoc """
  The scope renders a 3D entity cloud as four braille panels (top, front, side, iso)
  plus a text stats line. The panels are pixel scale; the labels and stats stay text.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.AsciiScope
  alias Weft.DataPlane.Ring

  @opts [width: 4, height: 2, x_range: {0, 3000}, y_range: {0, 3000}, z_range: {0, 3000}]

  defp has_braille_dot?(out) do
    Enum.any?(String.to_charlist(out), fn c -> c > 0x2800 and c <= 0x28FF end)
  end

  test "render draws four labelled panels, a stats line, and braille dots" do
    out = AsciiScope.render([1500, 0, 3000], @opts ++ [stats: ["hello 1"]])
    for label <- ["top", "front", "side", "iso"], do: assert(out =~ label)
    assert out =~ "hello 1"
    assert has_braille_dot?(out)
  end

  test "empty coords give only blank braille cells" do
    refute has_braille_dot?(AsciiScope.render([0, 0, 0], @opts))
  end

  test "populated panels color the braille with ANSI, blank cells stay uncolored" do
    out = AsciiScope.render([1500, 0, 3000], @opts)
    # A colored dot carries an ANSI 256 foreground escape from the level palette.
    assert out =~ "\e[38;5;"
    # An empty scope carries no color escape.
    refute AsciiScope.render([0, 0, 0], @opts) =~ "\e[38;5;"
  end

  test "of_ring reports tick and entity count" do
    ring = Ring.new(2)
    Ring.write(ring, 8, [1500, 0, 3000, 0, 0, 0])
    assert {8, out} = AsciiScope.of_ring(ring, @opts)
    assert out =~ "tick 8"
    assert out =~ "entities 1"
  end
end
