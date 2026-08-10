defmodule Weft.Assets do
  @moduledoc """
  Chunked asset distribution. A resumable upload, a convert step, and a paged fetch.

  This is control plane work. It is file backed and durable, and it is nowhere near the
  hot path. Bytes move in `chunk_bytes` pieces, because the original travels over a
  transport with no binary type. The core takes raw binaries, and a transport adapter
  handles any encoding.

  ## The converter is a plane

  `convert/2` takes a converter function, and the default is identity. It copies the bytes
  and reports the magic of the input.

  The real converter is the **asset baker plane**. `Weft` lists it: OpenUSD with the Adobe
  glTF plugin, request-response over iceoryx, out of the BEAM and crash isolated. It bakes
  a source glb into an OpenUSD stage.

  It is a plane and not a library for one reason. The baker parses a file from a person
  weft does not trust, so it must not have the network and it must not take the BEAM down
  when it crashes. That is the plane contract exactly.

  This module holds the protocol, and the plane holds the parse. The function argument is
  the seam between them.

  ## What is built, and what is not

  **Built.** This module, and `Weft.Assets.StageTier`, which is the desync adapter. The
  chunks go into SQLite and then into FoundationDB. An on-demand HTTP/3 and WebTransport
  endpoint serves them.

  **Not built.** The baker plane. It needs a native OpenUSD toolchain, which is not here
  yet, and the thread-per-core harness that every plane uses. See
  `../../fabric-harness`.

  **Next.** Build the baker on fabric-stage-runtime with the Adobe glTF plugin.
  """

  # 4 MiB for each chunk. This is rivet's max actor input, in `Weft.Limits` as
  # `:input_bytes`, and an asset arrives the same way an actor's input does.
  @chunk_bytes 4 * 1024 * 1024

  @type id :: String.t()

  @spec chunk_bytes() :: pos_integer()
  def chunk_bytes, do: @chunk_bytes

  @doc "Begin an upload; mints a hex id and an empty upload file."
  @spec begin(String.t()) :: {:ok, map()}
  def begin(name \\ "") do
    File.mkdir_p!(work_dir())
    id = :crypto.strong_rand_bytes(8) |> Base.encode16(case: :lower)
    File.write!(glb_path(id), "")
    {:ok, %{id: id, chunk_bytes: @chunk_bytes, name: name}}
  end

  @doc """
  Append a chunk. If `offset` is given it must equal the bytes received so far, so
  a duplicate or reordered chunk is refused and a caller can resume from
  `status/1`'s `received_bytes` rather than silently corrupting the file.
  """
  @spec chunk(id(), non_neg_integer() | nil, binary()) :: {:ok, map()} | {:error, term()}
  def chunk(id, offset, data) when is_binary(data) do
    with :ok <- validate_id(id),
         path = glb_path(id),
         true <- File.exists?(path) or {:error, :unknown_id},
         total = filesize(path),
         :ok <- check_offset(offset, total) do
      File.write!(path, data, [:append])
      {:ok, %{id: id, received_bytes: total + byte_size(data), appended: byte_size(data)}}
    end
  end

  @doc "Bytes received so far for a resumable upload."
  @spec status(id()) :: {:ok, map()} | {:error, term()}
  def status(id) do
    with :ok <- validate_id(id),
         path = glb_path(id),
         true <- File.exists?(path) or {:error, :unknown_id} do
      {:ok, %{id: id, received_bytes: filesize(path), chunk_bytes: @chunk_bytes}}
    end
  end

  @doc """
  Convert the uploaded asset, producing the fetchable output. `converter` is
  `(input_binary -> output_binary)`; the default copies the input so the protocol
  round-trips without a real converter.
  """
  @spec convert(id(), (binary() -> binary())) :: {:ok, map()} | {:error, term()}
  def convert(id, converter \\ &Function.identity/1) do
    with :ok <- validate_id(id),
         glb = glb_path(id),
         true <- File.exists?(glb) or {:error, :unknown_id} do
      input = File.read!(glb)
      output = converter.(input)
      File.write!(scn_path(id), output)
      size = byte_size(output)

      {:ok,
       %{
         id: id,
         scene_bytes: size,
         format: magic(output),
         chunk_bytes: @chunk_bytes,
         chunks: ceil_div(size, @chunk_bytes)
       }}
    end
  end

  @doc "Fetch the converted output one `chunk_bytes` page at a time by `seq`."
  @spec fetch(id(), non_neg_integer()) :: {:ok, map()} | {:error, term()}
  def fetch(id, seq) do
    with :ok <- validate_id(id),
         scn = scn_path(id),
         true <- File.exists?(scn) or {:error, :not_converted} do
      size = filesize(scn)
      offset = seq * @chunk_bytes
      len = max(min(@chunk_bytes, size - offset), 0)
      data = if len > 0, do: pread(scn, offset, len), else: <<>>
      {:ok, %{id: id, seq: seq, data: data, bytes: byte_size(data), eof: offset + len >= size}}
    end
  end

  defp check_offset(nil, _total), do: :ok
  defp check_offset(offset, total) when offset == total, do: :ok
  defp check_offset(_offset, total), do: {:error, {:offset_mismatch, total}}

  # Hex id, 8..64 chars, so an id can never escape the work dir.
  defp validate_id(id) when is_binary(id) do
    if byte_size(id) in 8..64 and String.match?(id, ~r/\A[0-9a-f]+\z/),
      do: :ok,
      else: {:error, :invalid_id}
  end

  defp work_dir,
    do: Application.get_env(:weft, :assets_dir) || Path.join(System.tmp_dir!(), "weft-assets")

  defp glb_path(id), do: Path.join(work_dir(), id <> ".glb")
  defp scn_path(id), do: Path.join(work_dir(), id <> ".scn")

  defp filesize(path) do
    case File.stat(path) do
      {:ok, %File.Stat{size: size}} -> size
      _ -> 0
    end
  end

  defp pread(path, offset, len) do
    {:ok, fd} = :file.open(path, [:read, :binary, :raw])
    {:ok, data} = :file.pread(fd, offset, len)
    :ok = :file.close(fd)
    data
  end

  defp magic(<<m::binary-size(4), _::binary>>), do: m
  defp magic(bin), do: bin

  defp ceil_div(a, b), do: div(a + b - 1, b)
end
