defmodule Weft.AssetsTest do
  @moduledoc """
  Ported from the rivet Godot zone's asset command surface: resumable chunked
  upload -> convert -> paged fetch, round-tripped end to end.
  """

  use ExUnit.Case, async: false

  alias Weft.Assets

  setup do
    dir = Path.join(System.tmp_dir!(), "weft-assets-test-#{System.unique_integer([:positive])}")
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

  test "upload, convert, and fetch round-trips the bytes" do
    # Two chunks worth plus a tail, to exercise paging.
    payload = "glTF" <> :crypto.strong_rand_bytes(Assets.chunk_bytes() + 1000)

    {:ok, %{id: id}} = Assets.begin("duck.glb")

    # Upload in chunk_bytes pieces, offset-checked.
    payload
    |> chunks(Assets.chunk_bytes())
    |> Enum.reduce(0, fn piece, offset ->
      {:ok, %{received_bytes: got}} = Assets.chunk(id, offset, piece)
      got
    end)

    {:ok, %{received_bytes: total}} = Assets.status(id)
    assert total == byte_size(payload)

    {:ok, conv} = Assets.convert(id)
    assert conv.scene_bytes == byte_size(payload)
    assert conv.format == "glTF"

    # Fetch every page and reassemble.
    fetched =
      Stream.iterate(0, &(&1 + 1))
      |> Enum.reduce_while(<<>>, fn seq, acc ->
        {:ok, page} = Assets.fetch(id, seq)
        acc = acc <> page.data
        if page.eof, do: {:halt, acc}, else: {:cont, acc}
      end)

    assert fetched == payload
  end

  test "a chunk at the wrong offset is refused so the caller can resume" do
    {:ok, %{id: id}} = Assets.begin()
    {:ok, _} = Assets.chunk(id, 0, "hello")
    assert {:error, {:offset_mismatch, 5}} = Assets.chunk(id, 0, "world")
    # Resuming from the reported offset works.
    assert {:ok, %{received_bytes: 10}} = Assets.chunk(id, 5, "world")
  end

  test "two transfers stay isolated" do
    {:ok, %{id: a}} = Assets.begin()
    {:ok, %{id: b}} = Assets.begin()
    {:ok, _} = Assets.chunk(a, 0, "aaaa")
    {:ok, _} = Assets.chunk(b, 0, "bb")
    assert {:ok, %{received_bytes: 4}} = Assets.status(a)
    assert {:ok, %{received_bytes: 2}} = Assets.status(b)
  end

  test "unknown or malformed ids are rejected" do
    assert {:error, :invalid_id} = Assets.status("../etc")
    assert {:error, :unknown_id} = Assets.status("deadbeefdeadbeef")
  end

  defp chunks(binary, size) do
    Stream.unfold(binary, fn
      <<>> -> nil
      rest when byte_size(rest) <= size -> {rest, <<>>}
      <<head::binary-size(size), rest::binary>> -> {head, rest}
    end)
  end
end
