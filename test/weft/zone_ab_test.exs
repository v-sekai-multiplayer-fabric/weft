defmodule Weft.ZoneABTest do
  @moduledoc """
  Zone A / zone B acceptance tests for the ported rivet zone actors: authoritative
  isolation, entity handoff across the area-of-interest boundary, and per-zone
  fanout. See [[weft-port-rivet-zone-actors]] / `Weft.DataPlane`.
  """

  use ExUnit.Case, async: true

  alias Weft.Zone

  setup do
    a = "zoneA-#{System.unique_integer([:positive])}"
    b = "zoneB-#{System.unique_integer([:positive])}"
    {:ok, pa} = Zone.start_link(zone_id: a, worker_opts: [tick_ms: 5])
    {:ok, pb} = Zone.start_link(zone_id: b, worker_opts: [tick_ms: 5])

    on_exit(fn ->
      for p <- [pa, pb], do: if(Process.alive?(p), do: GenServer.stop(p))
    end)

    {:ok, a: a, b: b}
  end

  test "each zone is the sole authority for its own entities", %{a: a, b: b} do
    :ok = Zone.add_entity(a, "player-1", %{hp: 10})
    :ok = Zone.add_entity(b, "player-2", %{hp: 20})

    assert Zone.entities(a) == %{"player-1" => %{hp: 10}}
    assert Zone.entities(b) == %{"player-2" => %{hp: 20}}
    refute Map.has_key?(Zone.entities(a), "player-2")
    refute Map.has_key?(Zone.entities(b), "player-1")
  end

  test "an entity hands off from zone A to zone B, preserving its state", %{a: a, b: b} do
    :ok = Zone.add_entity(a, "player-1", %{hp: 7, name: "atlantis"})

    :ok = Zone.handoff(a, b, "player-1")

    # Exactly one zone owns it afterwards, and its state survived the crossing.
    assert Zone.entities(a) == %{}
    assert Zone.entities(b) == %{"player-1" => %{hp: 7, name: "atlantis"}}
  end

  test "handing off an entity no zone owns reports not_found", %{a: a, b: b} do
    assert Zone.handoff(a, b, "ghost") == {:error, :not_found}
  end

  test "fanout delivers a zone's snapshots only to its own subscribers", %{a: a, b: b} do
    :ok = Zone.subscribe(a)

    assert_receive {:zone_snapshot, ^a, snapshot}, 500
    assert snapshot.zone_id == a

    # We never subscribed to B, so none of its snapshots reach us.
    refute_received {:zone_snapshot, ^b, _}
  end
end
