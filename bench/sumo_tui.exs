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

opts = [
  width: 74,
  height: 20,
  x_range: {0, 5_000_000},
  y_range: {0, 5_000_000},
  z_range: {0, 5_000_000}
]

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

  IO.write("\e[H\e[2J")
  IO.puts("weft stress bench - 3D scope (top/front/side/iso)")
  IO.puts(grid)
  if delay > 0, do: Process.sleep(delay)
end

Sumo.stop(pid)
