defmodule Weft.ZoneTest do
  @moduledoc """
  Exercises the control/data-plane boundary with a stub worker: the BEAM zone
  receives digested snapshots event-driven and steers the worker, without polling
  or touching a packet. See `docs/data-plane.md`.
  """

  use ExUnit.Case, async: true

  alias Weft.Zone

  test "a zone receives digested snapshots from its data-plane worker" do
    zone_id = "z-#{System.unique_integer([:positive])}"
    {:ok, zone} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 5, entities: 4])

    # Snapshots arrive as messages; the zone never polls. Wait for the first tick.
    assert eventually(fn -> Zone.tick(zone) > 0 end)

    snapshot = Zone.latest(zone)
    assert snapshot.zone_id == zone_id
    assert length(snapshot.entities) == 4

    before = Zone.tick(zone)
    Process.sleep(30)
    assert Zone.tick(zone) > before

    GenServer.stop(zone)
  end

  test "pause halts snapshot ticks and resume continues them" do
    zone_id = "z-#{System.unique_integer([:positive])}"
    {:ok, zone} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 5])

    assert eventually(fn -> Zone.tick(zone) > 0 end)

    :ok = Zone.command(zone, :pause)
    # Let any in-flight tick settle, then confirm the tick stops advancing.
    Process.sleep(25)
    paused_at = Zone.tick(zone)
    Process.sleep(30)
    assert Zone.tick(zone) == paused_at

    :ok = Zone.command(zone, :resume)
    assert eventually(fn -> Zone.tick(zone) > paused_at end)

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

  defp eventually(fun, tries \\ 200) do
    cond do
      fun.() -> true
      tries == 0 -> false
      true -> Process.sleep(5) && eventually(fun, tries - 1)
    end
  end
end
