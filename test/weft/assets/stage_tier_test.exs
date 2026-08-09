defmodule Weft.Assets.StageTierTest do
  @moduledoc """
  The stage tier of the asset CDN: content-addressed storage and paged distribution
  of baked stages. Covers dedup by hash, whole and paged fetch, and bad addresses.
  """

  use ExUnit.Case, async: false

  alias Weft.Assets.StageTier

  setup do
    dir = Path.join(System.tmp_dir!(), "weft-stage-test-#{System.unique_integer([:positive])}")
    prev = Application.get_env(:weft, :assets_dir)
    Application.put_env(:weft, :assets_dir, dir)

    on_exit(fn ->
      if prev,
        do: Application.put_env(:weft, :assets_dir, prev),
        else: Application.delete_env(:weft, :assets_dir)

      File.rm_rf(dir)
    end)

    :ok
  end

  test "publish is content-addressed and deduplicates identical bytes" do
    bytes = "USDA baked stage " <> :crypto.strong_rand_bytes(100)
    assert {:ok, addr} = StageTier.publish(bytes)
    assert addr == StageTier.address(bytes)
    # Publishing the same bytes returns the same address.
    assert {:ok, ^addr} = StageTier.publish(bytes)
    # Different bytes get a different address.
    assert {:ok, other} = StageTier.publish("something else")
    assert other != addr
  end

  test "fetch round-trips the stored stage" do
    bytes = :crypto.strong_rand_bytes(500)
    {:ok, addr} = StageTier.publish(bytes)
    assert {:ok, ^bytes} = StageTier.fetch(addr)
    assert {:ok, 500} = StageTier.size(addr)
  end

  test "fetch_page serves a stage in pages for fanout" do
    bytes = :crypto.strong_rand_bytes(2500)
    {:ok, addr} = StageTier.publish(bytes)

    assert {:ok, p0} = StageTier.fetch_page(addr, 0, 1000)
    assert {:ok, p1} = StageTier.fetch_page(addr, 1, 1000)
    assert {:ok, p2} = StageTier.fetch_page(addr, 2, 1000)
    assert byte_size(p0) == 1000
    assert byte_size(p2) == 500
    assert p0 <> p1 <> p2 == bytes
    # A page past the end is empty.
    assert {:ok, ""} = StageTier.fetch_page(addr, 3, 1000)
  end

  test "a bad address and a missing stage are errors, not crashes" do
    assert {:error, :bad_address} = StageTier.fetch("not-a-hash")
    missing = StageTier.address("never published " <> :crypto.strong_rand_bytes(8))
    assert {:error, :not_found} = StageTier.fetch(missing)
  end
end
