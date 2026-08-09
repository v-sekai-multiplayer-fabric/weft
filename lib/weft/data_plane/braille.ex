defmodule Weft.DataPlane.Braille do
  @moduledoc """
  Render pixel points as Unicode braille (U+2800 to U+28FF). Each character is a 2 by
  4 dot grid, so a character cell holds 8 sub-pixels. This gives pixel-scale terminal
  graphics: a grid of `w` by `h` characters draws `2*w` by `4*h` pixels.

  Dot bit values, by pixel column (0 or 1) and row (0 to 3):

      col 0: {0x01, 0x02, 0x04, 0x40}
      col 1: {0x08, 0x10, 0x20, 0x80}
  """

  @base 0x2800
  @bits {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}}

  @doc """
  Render a list of pixel coordinates `{px, py}` as a braille string of `h` rows by `w`
  characters. `px` runs 0 to `2*w - 1`. `py` runs 0 to `4*h - 1`. Pixels outside the
  range are dropped.
  """
  @spec render([{integer(), integer()}], pos_integer(), pos_integer()) :: String.t()
  def render(pixels, w, h) do
    cells =
      Enum.reduce(pixels, %{}, fn {px, py}, acc ->
        if px >= 0 and px < 2 * w and py >= 0 and py < 4 * h do
          cx = div(px, 2)
          cy = div(py, 4)
          bit = elem(elem(@bits, rem(px, 2)), rem(py, 4))
          Map.update(acc, {cx, cy}, bit, &Bitwise.bor(&1, bit))
        else
          acc
        end
      end)

    for cy <- 0..(h - 1) do
      for cx <- 0..(w - 1) do
        <<@base + Map.get(cells, {cx, cy}, 0)::utf8>>
      end
      |> Enum.join()
    end
    |> Enum.join("\n")
  end
end
