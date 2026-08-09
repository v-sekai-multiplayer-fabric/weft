defmodule Weft.DataPlane.SumoTest do
  @moduledoc """
  The SUMO game-data-plane producer plays a traffic trace into a zone's ring, one
  frame per tick; the BEAM samples the latest tear-free. Covers frame decoding,
  the coordinate mapping, deterministic stepping, and the live self-ticking feed.
  """

  use ExUnit.Case, async: true

  alias Weft.DataPlane.Ring
  alias Weft.DataPlane.Sumo

  test "decode_frames round-trips the compact SUMO binary" do
    # magic, max_slots=2, nframes=2; frame0 has 2 entities, frame1 has 1.
    bin =
      <<0x53554D4F::little-32, 2::little-32, 2::little-32>> <>
        <<2::little-32, 0::little-32, 1.0::little-float-32, 2.0::little-float-32, 1::little-32,
          3.0::little-float-32, 4.0::little-float-32>> <>
        <<1::little-32, 0::little-32, 5.0::little-float-32, 6.0::little-float-32>>

    assert {:ok, [f0, f1]} = Sumo.decode_frames(bin)
    assert f0 == [{0, 1.0, 2.0}, {1, 3.0, 4.0}]
    assert f1 == [{0, 5.0, 6.0}]
    assert Sumo.decode_frames(<<0, 1, 2>>) == :error
  end

  test "frame_to_coords maps to fixed-point mm and pads to max_entities" do
    assert Sumo.frame_to_coords([{0, 1.0, 2.0}], 2) == [1000, 2000, 0, 0, 0, 0]
  end

  test "stepping writes each frame into the ring, sampled tear-free" do
    frames = [[{0, 1.0, 2.0}, {1, 3.0, 4.0}], [{0, 5.0, 6.0}]]
    ring = Ring.new(2)
    {:ok, pid} = Sumo.start_link("zoneA", ring, frames, interval_ms: 0)

    assert Sumo.step(pid) == 0
    assert Ring.read(ring) == {0, [1000, 2000, 0, 3000, 4000, 0]}

    assert Sumo.step(pid) == 1
    assert Ring.read(ring) == {1, [5000, 6000, 0, 0, 0, 0]}

    # Wraps back to the first frame.
    assert Sumo.step(pid) == 0
    assert Ring.read(ring) == {0, [1000, 2000, 0, 3000, 4000, 0]}

    Sumo.stop(pid)
  end

  test "the live feed self-ticks and emits telemetry" do
    frames = [[{0, 1.0, 2.0}], [{0, 2.0, 3.0}]]
    ring = Ring.new(1)
    parent = self()

    :telemetry.attach(
      "sumo-test",
      [:weft, :sumo, :tick],
      fn _e, meas, meta, _ -> send(parent, {:sumo_tick, meas, meta}) end,
      nil
    )

    {:ok, pid} = Sumo.start_link("zoneB", ring, frames, interval_ms: 5)

    # Event-driven wait: the plane pushes a telemetry event per tick, so we await
    # the message rather than poll the ring.
    assert_receive {:sumo_tick, %{entities: 1}, %{zone_id: "zoneB", frame: 0}}, 500
    assert_receive {:sumo_tick, %{entities: 1}, %{zone_id: "zoneB", frame: 1}}, 500

    :telemetry.detach("sumo-test")
    Sumo.stop(pid)
  end
end
