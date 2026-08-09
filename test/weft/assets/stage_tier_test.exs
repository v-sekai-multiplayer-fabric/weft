defmodule Weft.Assets.StageTierTest do
  @moduledoc """
  The stage tier of the asset CDN: it stores and distributes baked stages with casync
  through the `desync` tool. The round-trip runs only when `desync` is on the path
  (tagged `:desync`); the guard tests run always.
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

  test "a missing stage is not_found, not a crash" do
    assert {:error, :not_found} = StageTier.fetch("never-published")
  end

  test "a bad name is rejected before touching desync" do
    assert {:error, :bad_name} = StageTier.publish("../evil", "x")
    assert {:error, :bad_name} = StageTier.fetch("../evil")
  end

  @tag :desync
  test "publish then fetch round-trips a stage through desync" do
    bytes = "USDA baked stage " <> :crypto.strong_rand_bytes(200_000)
    assert {:ok, index} = StageTier.publish("atlantis", bytes)
    assert String.ends_with?(index, "atlantis.caibx")
    assert {:ok, ^bytes} = StageTier.fetch("atlantis")
  end
end
