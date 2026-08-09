defmodule Weft.EntitiesTest do
  @moduledoc """
  Durable, atomic entity ownership in FoundationDB. Runs only when an FDB cluster
  is reachable (tagged `:fdb`).
  """

  use ExUnit.Case, async: false

  @moduletag :fdb

  alias Weft.Entities

  setup do
    # Unique zone/entity ids per test so the shared FDB stays isolated.
    n = System.unique_integer([:positive])
    {:ok, a: "zoneA-#{n}", b: "zoneB-#{n}", e: "player-#{n}"}
  end

  test "ownership is durable and readable", %{a: a, e: e} do
    :ok = Entities.put(e, a, %{hp: 7, name: "atlantis"})
    assert {:ok, ^a, %{hp: 7, name: "atlantis"}} = Entities.get(e)
    assert Entities.by_zone(a) == [e]
  end

  test "handoff atomically moves ownership and the zone index", %{a: a, b: b, e: e} do
    :ok = Entities.put(e, a, %{hp: 7})

    :ok = Entities.handoff(e, a, b)

    assert {:ok, ^b, %{hp: 7}} = Entities.get(e)
    assert Entities.by_zone(a) == []
    assert Entities.by_zone(b) == [e]
  end

  test "handoff from the wrong owner is refused without writing", %{a: a, b: b, e: e} do
    :ok = Entities.put(e, a, %{hp: 7})

    assert {:error, {:owned_by, ^a}} = Entities.handoff(e, "someone-else", b)
    # Ownership is unchanged.
    assert {:ok, ^a, _} = Entities.get(e)
  end

  test "handoff of an unknown entity reports not_found", %{a: a, b: b} do
    assert Entities.handoff("ghost-#{System.unique_integer([:positive])}", a, b) ==
             {:error, :not_found}
  end

  test "concurrent handoffs of the same entity yield exactly one winner", %{a: a, e: e} do
    targets = for i <- 1..8, do: "dest-#{i}-#{System.unique_integer([:positive])}"
    :ok = Entities.put(e, a, %{hp: 1})

    results =
      targets
      |> Task.async_stream(fn to -> {to, Entities.handoff(e, a, to)} end, max_concurrency: 8)
      |> Enum.map(fn {:ok, r} -> r end)

    winners = for {to, :ok} <- results, do: to
    assert length(winners) == 1

    {:ok, owner, _} = Entities.get(e)
    assert owner == hd(winners)
  end
end
