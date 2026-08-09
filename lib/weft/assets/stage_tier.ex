defmodule Weft.Assets.StageTier do
  @moduledoc """
  The stage tier of the asset CDN (`docs/runtime-choice.md`). It holds baked OpenUSD
  stages and distributes them to clients like a CDN: content-addressed, cached, and
  fanned out. The baker plane bakes a glb Character into a stage; this tier stores and
  serves it.

  Content addressing gives the CDN properties for free. The address is the SHA-256 of
  the bytes, so an identical stage deduplicates, and the stored file is immutable, so
  it is its own cache. Large stages are fetched by page, so many clients pull one
  stage without loading it whole per request.

  This is the control-plane half of the asset CDN. Baking (glb to OpenUSD) is the
  native baker plane. Storage here is file-backed and outside the hot path.
  """

  @type address :: String.t()

  @doc "Publish baked stage bytes. Returns the content address. Deduplicates by hash."
  @spec publish(binary()) :: {:ok, address()}
  def publish(bytes) when is_binary(bytes) do
    address = address(bytes)
    path = stage_path(address)

    unless File.exists?(path) do
      File.mkdir_p!(dir())
      File.write!(path, bytes)
    end

    {:ok, address}
  end

  @doc "Fetch a whole stage by its content address."
  @spec fetch(address()) :: {:ok, binary()} | {:error, term()}
  def fetch(address) do
    with :ok <- validate(address),
         path = stage_path(address),
         true <- File.exists?(path) or {:error, :not_found} do
      {:ok, File.read!(path)}
    end
  end

  @doc "Size of a stored stage in bytes."
  @spec size(address()) :: {:ok, non_neg_integer()} | {:error, term()}
  def size(address) do
    with :ok <- validate(address),
         path = stage_path(address),
         true <- File.exists?(path) or {:error, :not_found} do
      {:ok, File.stat!(path).size}
    end
  end

  @doc """
  Fetch one page of a stage for fanout: `page_bytes` bytes starting at
  `page * page_bytes`. The last page may be short. An out-of-range page is empty.
  """
  @spec fetch_page(address(), non_neg_integer(), pos_integer()) ::
          {:ok, binary()} | {:error, term()}
  def fetch_page(address, page, page_bytes) when page >= 0 and page_bytes > 0 do
    with :ok <- validate(address),
         path = stage_path(address),
         true <- File.exists?(path) or {:error, :not_found},
         {:ok, io} <- File.open(path, [:read, :binary]) do
      data = :file.pread(io, page * page_bytes, page_bytes)
      File.close(io)

      case data do
        {:ok, bin} -> {:ok, bin}
        :eof -> {:ok, ""}
        other -> other
      end
    end
  end

  @doc "The content address of some bytes: the lowercase hex SHA-256."
  @spec address(binary()) :: address()
  def address(bytes), do: :crypto.hash(:sha256, bytes) |> Base.encode16(case: :lower)

  defp validate(address) when is_binary(address) do
    if address =~ ~r/\A[a-f0-9]{64}\z/, do: :ok, else: {:error, :bad_address}
  end

  defp validate(_), do: {:error, :bad_address}

  defp stage_path(address), do: Path.join(dir(), address <> ".usd")

  defp dir do
    base = Application.get_env(:weft, :assets_dir) || Path.join(System.tmp_dir!(), "weft-assets")
    Path.join(base, "stages")
  end
end
