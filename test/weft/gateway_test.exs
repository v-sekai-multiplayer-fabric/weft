defmodule Weft.GatewayTest do
  @moduledoc """
  Routing core for the H3/WebTransport gateway: resolve an actor or zone by id and
  dispatch. Transport is exercised by the native front; here we test the routing.
  """

  use ExUnit.Case, async: true

  alias Weft.{Zone}
  alias Weft.Gateway
  alias Weft.Gateway.Request

  test "routes a reliable request to an actor and round-trips" do
    key = "gw-#{System.unique_integer([:positive])}"

    assert {:ok, :ok} =
             Gateway.dispatch(%Request{target: {:actor, "player", key}, op: :put, args: [:hp, 9]})

    assert {:ok, 9} =
             Gateway.dispatch(%Request{target: {:actor, "player", key}, op: :get, args: [:hp]})
  end

  test "routes to a zone" do
    zone_id = "gw-zone-#{System.unique_integer([:positive])}"
    {:ok, _} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 3_600_000])

    assert {:ok, :ok} =
             Gateway.dispatch(%Request{
               target: {:zone, zone_id},
               op: :add_entity,
               args: ["e1", %{hp: 1}]
             })

    assert {:ok, %{"e1" => %{hp: 1}}} =
             Gateway.dispatch(%Request{target: {:zone, zone_id}, op: :entities})
  end

  test "a datagram may not carry a mutating op that requires reliability" do
    req = %Request{target: {:actor, "player", "k"}, op: :put, args: [:hp, 1], reliable: false}
    assert {:error, {:requires_reliable, :put}} = Gateway.dispatch(req)
  end

  test "unknown ops and missing zones are errors, not crashes" do
    assert {:error, {:unknown_op, :frobnicate}} =
             Gateway.dispatch(%Request{target: {:actor, "player", "k2"}, op: :frobnicate})

    assert {:error, :no_zone} =
             Gateway.dispatch(%Request{
               target: {:zone, "nope-#{System.unique_integer([:positive])}"},
               op: :entities
             })
  end
end
