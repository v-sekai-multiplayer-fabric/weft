defmodule Weft.DataPlane.AsciiScope do
  @moduledoc """
  Render a 3D entity cloud on a 2D terminal as four ASCII panels plus a stats line:
  three orthographic profiles (top x/y, front x/z, side y/z) and one isometric 3D
  view. This is the 3D version of `Weft.DataPlane.AsciiMap`. The stats line carries
  the benchmark numbers (tick, entity count, and any measures passed in).

  Coordinates are fixed-point millimetres, the ring layout in `Weft.DataPlane.Ring`.
  """

  alias Weft.DataPlane.Braille
  alias Weft.DataPlane.Dither
  alias Weft.DataPlane.Ring

  # Intensity levels for the density dither, and the ANSI 256 color per level. Six
  # levels echo the Spectra 6 palette that epdoptimize targets. The color runs cold to
  # hot: navy, blue, green, yellow, orange, red. Dithering the density onto these few
  # levels keeps the color smooth as the panel grows to the window size.
  @level_palette [0, 51, 102, 153, 204, 255]
  @level_colors %{0 => 17, 51 => 39, 102 => 46, 153 => 226, 204 => 208, 255 => 196}
  @blank 0x2800

  @doc """
  Render `coords` (the ring's flat `[x, y, z, ...]`) as the four-panel scope string.
  Each panel draws in Unicode braille, so it is pixel scale: 2 by 4 dots per
  character. Options: `:width`, `:height` per panel (in characters),
  `:x_range`/`:y_range`/`:z_range` as `{min, max}` millimetres, and `:stats` as a list
  of strings for the stats line. Origin padding `{0, 0, 0}` is skipped.
  """
  @spec render([integer()], keyword()) :: String.t()
  def render(coords, opts \\ []) do
    w = Keyword.get(opts, :width, 26)
    h = Keyword.get(opts, :height, 10)
    bx = Keyword.get(opts, :x_range, {0, 5_000_000})
    by = Keyword.get(opts, :y_range, {0, 5_000_000})
    bz = Keyword.get(opts, :z_range, {0, 5_000_000})
    stats = Keyword.get(opts, :stats, [])

    pts = entities(coords)
    {ix, iy} = iso_bounds(bx, by, bz)

    top = panel(pts, w, h, fn {x, y, _z} -> {x, bx, y, by} end)
    front = panel(pts, w, h, fn {x, _y, z} -> {x, bx, z, bz} end)
    side = panel(pts, w, h, fn {_x, y, z} -> {y, by, z, bz} end)
    iso = panel(pts, w, h, fn {x, y, z} -> {x - y, ix, div(x + y, 2) - z, iy} end)

    gap = "   "
    row1 = beside(labeled("top   x>y", top, w), labeled("front x>z", front, w), gap)
    row2 = beside(labeled("side  y>z", side, w), labeled("iso   3d", iso, w), gap)

    (row1 ++ [""] ++ row2 ++ ["", Enum.join(stats, "  |  ")])
    |> Enum.join("\n")
  end

  @doc "Read the latest ring snapshot and render the scope, with tick and count in stats."
  @spec of_ring(Ring.t(), keyword()) :: {non_neg_integer(), String.t()}
  def of_ring(%Ring{} = ring, opts \\ []) do
    {tick, coords} = Ring.read(ring)
    count = length(entities(coords))
    stats = ["tick #{tick}", "entities #{count}"] ++ Keyword.get(opts, :stats, [])
    {tick, render(coords, Keyword.put(opts, :stats, stats))}
  end

  # ── internals ──────────────────────────────────────────────────────────────

  defp entities([]), do: []
  defp entities([0, 0, 0 | rest]), do: entities(rest)
  defp entities([x, y, z | rest]), do: [{x, y, z} | entities(rest)]

  # One panel as h braille rows. Each character is a 2 by 4 dot cell, so the panel
  # draws 2*w by 4*h pixels. The dots come from entity positions. The color comes from
  # the per-cell density, dithered onto the level palette. Denser cells run hotter.
  # Labels and stats stay as plain text; only the plot is braille and colored.
  defp panel(pts, w, h, proj) do
    pixels =
      Enum.map(pts, fn p ->
        {a, ra, b, rb} = proj.(p)
        {sc(a, ra, 2 * w), sc(b, rb, 4 * h)}
      end)

    density = density_grid(pixels, w, h)
    dithered = Dither.dither(density, @level_palette)
    rows = Braille.render(pixels, w, h) |> String.split("\n")
    color_rows(rows, dithered)
  end

  # Count the set pixels in each 2 by 4 cell and scale to the 0..255 intensity range.
  # Eight pixels per cell, so each pixel adds 32.
  defp density_grid(pixels, w, h) do
    counts =
      Enum.reduce(pixels, %{}, fn {px, py}, acc ->
        Map.update(acc, {div(px, 2), div(py, 4)}, 1, &(&1 + 1))
      end)

    for cy <- 0..(h - 1) do
      for cx <- 0..(w - 1), do: min(255, Map.get(counts, {cx, cy}, 0) * 32)
    end
  end

  # Wrap each non-blank braille character in the ANSI color for its dithered level.
  defp color_rows(rows, dithered) do
    Enum.zip_with(rows, dithered, fn row, levels ->
      row
      |> String.to_charlist()
      |> Enum.zip(levels)
      |> Enum.map(fn {cp, level} -> colorize(cp, level) end)
      |> Enum.join()
    end)
  end

  defp colorize(@blank, _level), do: <<@blank::utf8>>

  defp colorize(cp, level) do
    color = Map.fetch!(@level_colors, level)
    "\e[38;5;#{color}m" <> <<cp::utf8>> <> "\e[0m"
  end

  defp labeled(title, rows, w), do: [String.pad_trailing(title, w) | rows]

  defp beside(a, b, gap) do
    Enum.zip_with(a, b, fn la, lb -> la <> gap <> lb end)
  end

  defp sc(_v, {mn, mx}, size) when mx <= mn, do: div(size - 1, 2)

  defp sc(v, {mn, mx}, size) do
    div((v - mn) * (size - 1), mx - mn) |> max(0) |> min(size - 1)
  end

  # Isometric screen bounds from the eight corners of the world box.
  defp iso_bounds({xmn, xmx}, {ymn, ymx}, {zmn, zmx}) do
    {{xmn - ymx, xmx - ymn}, {div(xmn + ymn, 2) - zmx, div(xmx + ymx, 2) - zmn}}
  end
end
