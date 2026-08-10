defmodule Weft.GatewayTest do
  @moduledoc """
  Routing core for the H3/WebTransport gateway: resolve an actor or zone by id and
  dispatch. Transport is exercised by the native front; here we test the routing.
  """

  use ExUnit.Case, async: true

  alias Weft.{Zone}
  alias Weft.Gateway
  alias Weft.Gateway.Request

  # `Horde.Registry.register` returns before `Horde.Registry.lookup` can see the name.
  # Horde's `on_diffs/2` sends `{:crdt_update, diffs}` to the registry process, and that
  # process materialises the name into ETS when it handles the message. So a register
  # that succeeded is invisible until that mailbox drains.
  #
  # It is invisible for about 2 ms. `../../docs/logbook/control_plane.md` measures it: at
  # 32 concurrent registrations, 631 of 640 lookups straight after a register found
  # nothing, and every one of them appeared later, at a median of 1586 us and a maximum of
  # 2048 us. Sequentially it never happens, which is why this only failed in a full run.
  #
  # These tests are about routing, so they wait for the name rather than assert that
  # registration is instant. `Weft.Gateway.dispatch/1` returning `{:error, :no_zone}` in
  # that window is real behaviour, and it belongs to the gateway rather than to a test.
  defp await_registered(key, tries \\ 500) do
    case Horde.Registry.lookup(Weft.Registry, key) do
      [{_pid, _}] -> :ok
      [] when tries > 0 -> Process.sleep(1) && await_registered(key, tries - 1)
      [] -> flunk("#{inspect(key)} never appeared in Weft.Registry")
    end
  end

  test "routes a reliable request to an actor and round-trips" do
    key = Weft.Test.Fresh.id("gw")

    assert {:ok, :ok} =
             Gateway.dispatch(%Request{target: {:actor, "player", key}, op: :put, args: [:hp, 9]})

    assert {:ok, 9} =
             Gateway.dispatch(%Request{target: {:actor, "player", key}, op: :get, args: [:hp]})
  end

  test "routes to a zone" do
    zone_id = "gw-zone-#{System.unique_integer([:positive])}"
    {:ok, _} = Zone.start_link(zone_id: zone_id, worker_opts: [tick_ms: 3_600_000])
    await_registered({:zone, zone_id})

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
