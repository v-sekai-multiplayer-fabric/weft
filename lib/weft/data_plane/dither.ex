defmodule Weft.DataPlane.Dither do
  @moduledoc """
  Floyd-Steinberg error-diffusion dithering, ported from the Lean4 spec in
  `lean/Dither.lean`. The algorithm is from paperlesspaper/epdoptimize. Dithering maps
  a continuous intensity field onto a small palette, so it looks smooth at any size.
  This is why the scope can grow to the window size and still read well.

  Proven in Lean4 (`native_decide`, no Mathlib): the kernel weights sum to 16, so all
  quantization error is redistributed (`fs_conserves`), and `nearest` returns the
  closest palette level (`nearest_closest`).
  """

  # Floyd-Steinberg kernel: {dx, dy, weight}, weights over 16.
  @fs_kernel [{1, 0, 7}, {-1, 1, 3}, {0, 1, 5}, {1, 1, 1}]
  @fs_denom 16

  @doc "Distance between two integer levels."
  @spec dist(integer(), integer()) :: non_neg_integer()
  def dist(a, b), do: abs(a - b)

  @doc "Nearest palette level to `v`. Returns `v` for the empty palette."
  @spec nearest([integer()], integer()) :: integer()
  def nearest([], v), do: v

  def nearest([p | ps], v) do
    Enum.reduce(ps, p, fn c, best -> if dist(v, c) < dist(v, best), do: c, else: best end)
  end

  @doc "The Floyd-Steinberg kernel weights sum to the denominator (16)."
  @spec weight_sum() :: non_neg_integer()
  def weight_sum, do: Enum.reduce(@fs_kernel, 0, fn {_dx, _dy, w}, acc -> acc + w end)

  @doc """
  Dither a 2D intensity grid (a list of rows of integers) onto `palette` with
  Floyd-Steinberg error diffusion. Returns the grid with each cell a palette level.
  Error diffuses right and down per the kernel.
  """
  @spec dither([[integer()]], [integer()]) :: [[integer()]]
  def dither([], _palette), do: []

  def dither(grid, palette) do
    h = length(grid)
    w = length(hd(grid))

    cur =
      for {row, y} <- Enum.with_index(grid),
          {v, x} <- Enum.with_index(row),
          into: %{},
          do: {{x, y}, v}

    order = for y <- 0..(h - 1), x <- 0..(w - 1), do: {x, y}

    {out, _cur} =
      Enum.reduce(order, {%{}, cur}, fn {x, y}, {out, cur} ->
        old = Map.fetch!(cur, {x, y})
        new = nearest(palette, old)
        err = old - new

        cur =
          Enum.reduce(@fs_kernel, cur, fn {dx, dy, wt}, c ->
            key = {x + dx, y + dy}

            if Map.has_key?(c, key) do
              Map.update!(c, key, &(&1 + div(err * wt, @fs_denom)))
            else
              c
            end
          end)

        {Map.put(out, {x, y}, new), cur}
      end)

    for y <- 0..(h - 1), do: for(x <- 0..(w - 1), do: Map.fetch!(out, {x, y}))
  end
end
