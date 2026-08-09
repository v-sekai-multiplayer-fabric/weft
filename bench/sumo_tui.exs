# Console playback of the weft stress bench, as a 3D scope: three orthographic
# profiles (top, front, side) and one isometric render, with the benchmark stats.
# On CI, asciinema records this terminal and uploads it; locally, asciinema play.
#
#   mix run bench/sumo_tui.exs [frame_count] [frame_delay_ms]
#
# It plays a real SUMO trace (bench/sumo/scenario/frames.bin) through the zone ring.
# The map is 3D, so it always renders the three profiles and the isometric view. SUMO
# traffic is on the ground plane, so the front and side profiles show that plane.
# Generate the trace with bench/sumo (see bench/sumo/README.md).

alias Weft.DataPlane.{AsciiScope, Ring, Sumo}

[count, delay] =
  case System.argv() do
    [c, d] -> [String.to_integer(c), String.to_integer(d)]
    [c] -> [String.to_integer(c), 50]
    _ -> [120, 50]
  end

path = "bench/sumo/scenario/frames.bin"

frames =
  case File.read(path) do
    {:ok, bin} ->
      case Sumo.decode_frames(bin) do
        {:ok, fs} -> fs
        :error -> []
      end

    _ ->
      []
  end

if frames == [] do
  IO.puts("no SUMO trace at #{path}; generate it with bench/sumo (see bench/sumo/README.md)")
  System.halt(1)
end

max_entities = 64
ring = Ring.new(max_entities)
{:ok, pid} = Sumo.start_link("bench", ring, frames, interval_ms: 0)

# Size the panels to the current terminal so the scope fills the window. Two panels
# plus a three-column gap span the width; two panel rows plus labels, a blank line, and
# the stats line span the height. The dither keeps the color smooth at any size.
{cols, rows} =
  case {:io.columns(), :io.rows()} do
    {{:ok, c}, {:ok, r}} -> {c, r}
    _ -> {80, 24}
  end

opts = [
  width: max(10, div(cols - 3, 2)),
  height: max(4, div(max(6, rows - 6), 2)),
  x_range: {0, 5_000_000},
  y_range: {0, 5_000_000},
  z_range: {0, 5_000_000}
]

# Hide the cursor and clear the screen once. The loop then overwrites in place.
IO.write("\e[?25l\e[2J")

for tick <- 0..(count - 1) do
  Sumo.step(pid)

  # Measure the ring sample cost for the stats line.
  t0 = System.monotonic_time(:nanosecond)
  _ = Ring.read(ring)
  sample_us = (System.monotonic_time(:nanosecond) - t0) / 1000

  {_t, grid} =
    AsciiScope.of_ring(
      ring,
      Keyword.put(opts, :stats, [
        "sample #{:erlang.float_to_binary(sample_us, decimals: 1)}us",
        "frame #{tick}"
      ])
    )

  # Home the cursor and overwrite in place. Each line ends with erase-to-end (\e[K),
  # and the frame ends with erase-below (\e[J). The screen never blanks, so the frame
  # does not flicker. One write keeps the frame whole.
  frame = [
    "\e[H",
    "weft stress bench - 3D scope (top/front/side/iso)\e[K\n",
    String.replace(grid, "\n", "\e[K\n"),
    "\e[K\e[J"
  ]

  IO.write(frame)
  if delay > 0, do: Process.sleep(delay)
end

# Show the cursor again.
IO.write("\e[?25h")
Sumo.stop(pid)
