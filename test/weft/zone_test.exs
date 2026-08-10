defmodule Weft.ZoneTest do
  @moduledoc """
  Exercises the control/data-plane boundary with a stub worker: the BEAM zone
  receives digested snapshots event-driven and steers the worker, without polling
  or touching a packet. See `Weft.DataPlane`.
  """

  use ExUnit.Case, async: true

  alias Weft.Zone

  test "a zone receives digested snapshots from its data-plane worker" do
    zone_id = "z-#{System.unique_integer([:positive])}"
    {:ok, zone} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 5, entities: 4])

    # `Weft.Zone.start_link` does not return until the name is registered, so this call
    # through `via/1` cannot race it. See the comment there.
    #
    # The zone fans every tick out to its subscribers, so this waits for a message rather
    # than polling a counter. `assert_receive` returns the moment the message lands, and
    # its bound is only there because absence has to be bounded somehow: 1000 ms against a
    # 5 ms tick is 200 ticks of headroom, so hitting it means the worker was starved, not
    # that the timeout was tuned too fine.
    :ok = Zone.subscribe(zone_id)

    assert_receive {:zone_snapshot, ^zone_id, snapshot}, 1_000
    assert snapshot.zone_id == zone_id
    assert length(snapshot.entities) == 4

    # A second tick proves it keeps going, and it arrives as a message too.
    assert_receive {:zone_snapshot, ^zone_id, later}, 1_000
    assert later.tick > snapshot.tick

    GenServer.stop(zone)
  end

  test "pause halts snapshot ticks and resume continues them" do
    zone_id = "z-#{System.unique_integer([:positive])}"
    {:ok, zone} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 5])

    :ok = Zone.subscribe(zone_id)
    assert_receive {:zone_snapshot, ^zone_id, _}, 1_000

    :ok = Zone.command(zone_id, :pause)

    # `Weft.DataPlane.Stub.command/2` is a cast, so `Zone.command/2` returns before the
    # worker applies it. A call to the worker orders after that cast, so this returns only
    # once the pause is applied. It is the mailbox doing the ordering, not a delay.
    _ = :sys.get_state(:sys.get_state(zone).worker)

    # Drain any tick that was already sent before the pause landed.
    receive do
      {:zone_snapshot, ^zone_id, _} -> :ok
    after
      0 -> :ok
    end

    # Absence is the one thing a message cannot announce, so it needs a window. This is
    # `refute_receive`, which is ExUnit's primitive for it, and the window is 30 ms
    # against a 5 ms tick: six chances for a broken pause to show.
    refute_receive {:zone_snapshot, ^zone_id, _}, 30

    :ok = Zone.command(zone_id, :resume)
    assert_receive {:zone_snapshot, ^zone_id, _}, 1_000

    GenServer.stop(zone)
  end

  test "the data-plane worker is stopped when the zone stops" do
    zone_id = "z-#{System.unique_integer([:positive])}"
    {:ok, zone} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 5])
    worker = :sys.get_state(zone).worker
    assert Process.alive?(worker)

    ref = Process.monitor(worker)
    GenServer.stop(zone)
    assert_receive {:DOWN, ^ref, :process, ^worker, _}, 1_000
  end

end
