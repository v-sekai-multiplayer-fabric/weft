defmodule Weft.AuthorityTest do
  use ExUnit.Case, async: true

  alias Weft.Authority

  @moduledoc """
  One controller for one avatar.

  The decision tests need no database, because every decision is a pure function of the
  stored record. The tagged tests below them prove that FoundationDB carries the decision
  across a transaction, which is what makes the rule hold between machines.
  """

  describe "decide_claim/2" do
    test "a free avatar is claimed at epoch 1" do
      assert {:ok, {"ada", 1}} = Authority.decide_claim(:not_found, "ada")
    end

    test "a second controller is refused, and is told who holds it" do
      {:ok, held} = Authority.decide_claim(:not_found, "ada")
      assert {:error, {:held_by, "ada"}} = Authority.decide_claim(held, "grace")
    end

    test "the holder may claim again, and keeps its epoch" do
      # Idempotent on purpose. A repeated claim must not fence the writes the same
      # controller already has in flight.
      assert {:ok, {"ada", 7}} = Authority.decide_claim({"ada", 7}, "ada")
    end

    test "a released avatar is claimed at the next epoch, never at the old one" do
      assert {:ok, {"grace", 8}} = Authority.decide_claim({nil, 7}, "grace")
    end
  end

  describe "decide_seize/2" do
    test "a seizure always succeeds and always raises the epoch" do
      assert {:ok, {"grace", 8}} = Authority.decide_seize({"ada", 7}, "grace")
    end

    test "a seizure of a free avatar starts at epoch 1" do
      assert {:ok, {"grace", 1}} = Authority.decide_seize(:not_found, "grace")
    end
  end

  describe "decide_check/3" do
    test "the holder writes under its own epoch" do
      assert :ok = Authority.decide_check({"ada", 7}, "ada", 7)
    end

    test "a controller that was seized from is fenced" do
      {:ok, after_seize} = Authority.decide_seize({"ada", 7}, "grace")
      assert {:error, :fenced} = Authority.decide_check(after_seize, "ada", 7)
      assert :ok = Authority.decide_check(after_seize, "grace", 8)
    end

    test "the holder is fenced under a stale epoch" do
      assert {:error, :fenced} = Authority.decide_check({"ada", 8}, "ada", 7)
    end

    test "an unclaimed avatar accepts nobody" do
      assert {:error, :fenced} = Authority.decide_check(:not_found, "ada", 1)
      assert {:error, :fenced} = Authority.decide_check({nil, 3}, "ada", 3)
    end
  end

  describe "decide_release/3" do
    test "the holder releases, and the epoch is kept" do
      assert {:ok, {nil, 7}} = Authority.decide_release({"ada", 7}, "ada", 7)
    end

    test "another controller may not release" do
      assert {:error, :fenced} = Authority.decide_release({"ada", 7}, "grace", 7)
    end

    test "a fenced controller may not release under its old epoch" do
      assert {:error, :fenced} = Authority.decide_release({"grace", 8}, "ada", 7)
    end

    test "an unknown avatar is not found" do
      assert {:error, :not_found} = Authority.decide_release(:not_found, "ada", 1)
    end
  end

  describe "the epoch never rewinds" do
    test "claim, release and claim again gives a strictly higher epoch each time" do
      # This is the property that removes the need for a lease. A controller fenced at
      # epoch 1 can never write again, whatever happens to the avatar afterwards.
      {:ok, {_c, e1} = r1} = Authority.decide_claim(:not_found, "ada")
      {:ok, r2} = Authority.decide_release(r1, "ada", e1)
      {:ok, {_c2, e2} = r3} = Authority.decide_claim(r2, "grace")
      {:ok, {_c3, e3}} = Authority.decide_seize(r3, "hedy")

      assert e1 < e2 and e2 < e3
      assert {:error, :fenced} = Authority.decide_check(r3, "ada", e1)
    end
  end

  describe "against FoundationDB" do
    @describetag :fdb

    setup do
      # A distinct avatar per test, so the tests stay async and share one database.
      {:ok, avatar: "avatar-#{System.unique_integer([:positive])}"}
    end

    test "one controller claims, and a second is refused", %{avatar: avatar} do
      assert {:ok, epoch} = Authority.claim(avatar, "ada")
      assert {:error, {:held_by, "ada"}} = Authority.claim(avatar, "grace")
      assert :ok = Authority.check(avatar, "ada", epoch)
      assert {:error, :fenced} = Authority.check(avatar, "grace", epoch)
    end

    test "a seizure fences the previous holder", %{avatar: avatar} do
      {:ok, first} = Authority.claim(avatar, "ada")
      {:ok, second} = Authority.seize(avatar, "grace")

      assert second > first
      assert {:error, :fenced} = Authority.check(avatar, "ada", first)
      assert :ok = Authority.check(avatar, "grace", second)
    end

    test "a release frees the avatar and the epoch still rises", %{avatar: avatar} do
      {:ok, first} = Authority.claim(avatar, "ada")
      assert :ok = Authority.release(avatar, "ada", first)
      assert {:ok, nil, ^first} = Authority.holder(avatar)

      {:ok, second} = Authority.claim(avatar, "grace")
      assert second > first
      assert {:error, :fenced} = Authority.check(avatar, "ada", first)
    end

    test "concurrent claims produce exactly one holder", %{avatar: avatar} do
      # The point of the whole module. Many controllers race for one avatar through
      # separate transactions, and FoundationDB must let exactly one win.
      results =
        1..16
        |> Task.async_stream(fn i -> Authority.claim(avatar, "controller-#{i}") end,
          max_concurrency: 16,
          timeout: 30_000
        )
        |> Enum.map(fn {:ok, r} -> r end)

      winners = Enum.count(results, &match?({:ok, _}, &1))
      assert winners == 1, "expected exactly one winner, got #{winners}"

      assert {:ok, holder, _epoch} = Authority.holder(avatar)
      assert holder != nil
    end
  end

  describe "the gateway refuses a fenced controller" do
    @describetag :fdb

    alias Weft.Gateway.Request

    test "a seized controller cannot reach a zone" do
      avatar = "avatar-#{System.unique_integer([:positive])}"
      {:ok, first} = Authority.claim(avatar, "ada")

      # Ada holds the avatar, so her request is authorised and reaches routing. It fails
      # for a routing reason and not an authority one, which is the distinction that
      # matters here.
      ada = %Request{
        target: {:zone, "no-such-zone"},
        op: :entities,
        avatar: avatar,
        controller: "ada",
        epoch: first
      }

      assert {:error, :no_zone} = Weft.Gateway.dispatch(ada)

      # Grace seizes the avatar. Ada is fenced from that moment, and her request is
      # refused before it is routed at all.
      {:ok, second} = Authority.seize(avatar, "grace")
      assert {:error, :fenced} = Weft.Gateway.dispatch(ada)

      grace = %{ada | controller: "grace", epoch: second}
      assert {:error, :no_zone} = Weft.Gateway.dispatch(grace)
    end

    test "an avatar named without a controller is refused" do
      avatar = "avatar-#{System.unique_integer([:positive])}"
      {:ok, _epoch} = Authority.claim(avatar, "ada")

      req = %Request{target: {:zone, "no-such-zone"}, op: :entities, avatar: avatar}
      assert {:error, :fenced} = Weft.Gateway.dispatch(req)
    end
  end
end
