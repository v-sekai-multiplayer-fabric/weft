defmodule Weft.DataPlane.BrailleTest do
  @moduledoc """
  Braille packs a 2 by 4 pixel grid into one character (U+2800 to U+28FF). Covers the
  dot bit mapping, the full cell, empty cells, layout across cells, and out-of-range.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.Braille

  test "each pixel maps to its dot bit" do
    assert Braille.render([{0, 0}], 1, 1) == <<0x2801::utf8>>
    assert Braille.render([{1, 0}], 1, 1) == <<0x2808::utf8>>
    assert Braille.render([{0, 3}], 1, 1) == <<0x2840::utf8>>
    assert Braille.render([{1, 3}], 1, 1) == <<0x2880::utf8>>
  end

  test "all eight dots make a full cell, none makes a blank cell" do
    all = for px <- 0..1, py <- 0..3, do: {px, py}
    assert Braille.render(all, 1, 1) == <<0x28FF::utf8>>
    assert Braille.render([], 1, 1) == <<0x2800::utf8>>
  end

  test "cells lay out left to right and top to bottom" do
    # {0,0} is cell (0,0); {2,0} is cell (1,0); each sets dot 1.
    assert Braille.render([{0, 0}, {2, 0}], 2, 1) == <<0x2801::utf8, 0x2801::utf8>>
    # {0,4} is cell (0,1) on the second row.
    assert Braille.render([{0, 4}], 1, 2) == <<0x2800::utf8, "\n", 0x2801::utf8>>
  end

  test "out-of-range pixels are dropped" do
    assert Braille.render([{5, 5}], 1, 1) == <<0x2800::utf8>>
  end
end
