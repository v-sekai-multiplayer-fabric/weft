defmodule Weft.GatewaySeqTest do
  use ExUnit.Case, async: true

  alias Weft.Gateway
  alias Weft.Gateway.{Request, SeqGuard}

  test "SeqGuard keeps only strictly-newer sequences per target" do
    t = {:zone, "sg-#{System.unique_integer([:positive])}"}

    assert SeqGuard.fresh?(t, 1)
    assert SeqGuard.fresh?(t, 2)
    refute SeqGuard.fresh?(t, 2)
    refute SeqGuard.fresh?(t, 1)
    assert SeqGuard.fresh?(t, 5)
    refute SeqGuard.fresh?(t, 3)
    assert SeqGuard.fresh?(t, 6)
  end

  test "gateway drops a stale datagram and processes a newer one" do
    key = "seq-#{System.unique_integer([:positive])}"
    target = {:actor, "player", key}
    dg = fn s -> %Request{target: target, op: :get, args: [:x], reliable: false, seq: s} end

    assert {:ok, _} = Gateway.dispatch(dg.(2))
    assert {:error, :stale} = Gateway.dispatch(dg.(1))
    assert {:error, :stale} = Gateway.dispatch(dg.(2))
    assert {:ok, _} = Gateway.dispatch(dg.(3))
  end
end
