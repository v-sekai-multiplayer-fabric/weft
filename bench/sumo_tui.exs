# Console playback of the SUMO stress bench. It plays frames into a zone ring and
# prints an ASCII grid each tick. On CI, `asciinema rec` records this terminal to a
# .cast file; locally, `asciinema play <file>.cast` replays it on the desktop.
#
#   mix run bench/sumo_tui.exs [frame_count] [frame_delay_ms]
#
# It uses bench/sumo/scenario/frames.bin when present (see extract_frames.py), and a
# synthetic moving swarm otherwise, so it runs with no SUMO install.

alias Weft.DataPlane.{AsciiMap, Ring, Sumo}

[count, delay] =
  case System.argv() do
    [c, d] -> [String.to_integer(c), String.to_integer(d)]
    [c] -> [String.to_integer(c), 50]
    _ -> [120, 50]
  end

# Synthetic frames: a swarm orbiting the grid centre, in float metres (the SUMO frame
# unit). A 25x25 grid of 200 m cells spans 0..5000 m.
synth = fn ->
  centre = 2500.0
  radius = 1500.0

  for t <- 0..119 do
    for i <- 0..49 do
      a = :math.pi() * 2 * (i / 50) + t * 0.08
      {i, centre + radius * :math.cos(a), centre + radius * :math.sin(a)}
    end
  end
end

frames =
  case File.read("bench/sumo/scenario/frames.bin") do
    {:ok, bin} ->
      case Sumo.decode_frames(bin) do
        {:ok, fs} -> fs
        :error -> synth.()
      end

    _ ->
      synth.()
  end

ring = Ring.new(64)
{:ok, pid} = Sumo.start_link("bench", ring, frames, interval_ms: 0)

opts = [width: 60, height: 24, x_range: {0, 5_000_000}, y_range: {0, 5_000_000}]

for _ <- 1..count do
  Sumo.step(pid)
  {tick, grid} = AsciiMap.of_ring(ring, opts)
  # Clear the screen so playback animates.
  IO.write("\e[H\e[2J")
  IO.puts("weft SUMO stress bench — tick #{tick}")
  IO.puts(grid)
  if delay > 0, do: Process.sleep(delay)
end

Sumo.stop(pid)
