defmodule Weft.DataPlane.AsciiMap do
  @moduledoc """
  Render a ring snapshot as an ASCII grid for the terminal. This is the console
  playback of the stress bench: the sim runs on GitHub Actions and prints these grids
  each tick, `asciinema` records the terminal to a `.cast` file, and the developer
  replays it locally with `asciinema play`. It is the pure-Elixir version of the
  headless Godot TUI observer.

  Coordinates are fixed-point millimetres, the ring layout in `Weft.DataPlane.Ring`.
  """

  alias Weft.DataPlane.Ring

  @dot "."
  @mark "O"

  @doc """
  Render entity coordinates as an ASCII grid string. `coords` is the ring's flat list
  `[x, y, z, ...]` in fixed-point millimetres. Options: `:width`, `:height`,
  `:x_range` and `:y_range` as `{min, max}` in millimetres. Padding entities at the
  origin `{0, 0, 0}` are skipped.
  """
  @spec render([integer()], keyword()) :: String.t()
  def render(coords, opts \\ []) do
    w = Keyword.get(opts, :width, 40)
    h = Keyword.get(opts, :height, 20)
    {min_x, max_x} = Keyword.get(opts, :x_range, {0, 5_000_000})
    {min_y, max_y} = Keyword.get(opts, :y_range, {0, 5_000_000})

    empty = for _ <- 1..h, do: for(_ <- 1..w, do: @dot)

    grid =
      coords
      |> entities()
      |> Enum.reduce(empty, fn {x, y, _z}, acc ->
        col = scale(x, min_x, max_x, w)
        row = scale(y, min_y, max_y, h)
        put(acc, row, col)
      end)

    grid |> Enum.map_join("\n", &Enum.join/1)
  end

  @doc "Read the latest ring snapshot and render it as `{tick, grid_string}`."
  @spec of_ring(Ring.t(), keyword()) :: {non_neg_integer(), String.t()}
  def of_ring(%Ring{} = ring, opts \\ []) do
    {tick, coords} = Ring.read(ring)
    {tick, render(coords, opts)}
  end

  defp entities([]), do: []
  defp entities([0, 0, 0 | rest]), do: entities(rest)
  defp entities([x, y, z | rest]), do: [{x, y, z} | entities(rest)]

  # Map a value in [min, max] to a grid index in [0, size - 1]. Out-of-range clamps.
  defp scale(_v, min, max, size) when max <= min, do: div(size - 1, 2)

  defp scale(v, min, max, size) do
    i = div((v - min) * (size - 1), max - min)
    i |> max(0) |> min(size - 1)
  end

  defp put(grid, row, col) do
    List.update_at(grid, row, fn line -> List.replace_at(line, col, @mark) end)
  end
end
