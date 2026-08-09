defmodule Weft.Assets.StageTier do
  @moduledoc """
  The stage tier of the asset CDN (`docs/runtime-choice.md`). It holds baked OpenUSD
  stages and distributes them to clients like a CDN. It uses casync through the
  `desync` tool, the same format as `fabric-casync-central`, not a bespoke content
  addressing.

  A publish runs `desync make` to split a stage into content-defined chunks, write
  them to a shared chunk store, and produce a small `.caibx` index. Content-defined
  chunking deduplicates at the chunk level, so a new stage version stores only its
  changed chunks. A fetch runs `desync extract` to rebuild the stage from the index
  and the store, pulling only the chunks it needs. The index is the reference a client
  receives.

  The chunk store is not a plain directory or S3. The chunks are cut up into weft's
  store, SQLite to FoundationDB (the store plane in `docs/store.md`), and served over
  an on-demand H3/WebTransport chunk endpoint, spawned when a client needs a stage and
  torn down when the transfer is done (scale-to-zero, the actor lifecycle). It is not
  an HTTP/1.1 store, because the transport is H3/WebTransport (`docs/protocol.md`).
  desync gives the chunk format and dedup; the transport is ours. So the asset CDN and
  the actor store share one durable substrate. In this prototype a local directory
  stands in for that store.

  This is the control-plane half of the asset CDN. Baking (glb to OpenUSD) is the
  native baker plane. `desync` must be on the path (see
  https://github.com/folbricht/desync).
  """

  @doc "True when the `desync` tool is available on the path."
  @spec available?() :: boolean()
  def available?, do: System.find_executable("desync") != nil

  @doc """
  Publish a baked stage under `name`. Splits it into the chunk store and writes a
  `name.caibx` index. Returns the index path.
  """
  @spec publish(String.t(), binary()) :: {:ok, Path.t()} | {:error, term()}
  def publish(name, bytes) when is_binary(name) and is_binary(bytes) do
    with :ok <- validate(name) do
      File.mkdir_p!(store_dir())
      File.mkdir_p!(index_dir())
      input = Path.join(index_dir(), name <> ".usd")
      File.write!(input, bytes)
      index = index_path(name)

      case desync(["make", "--store", store_dir(), index, input]) do
        {:ok, _} -> {:ok, index}
        error -> error
      end
    end
  end

  @doc "Fetch a whole stage by `name`, rebuilding it from the index and the chunk store."
  @spec fetch(String.t()) :: {:ok, binary()} | {:error, term()}
  def fetch(name) when is_binary(name) do
    with :ok <- validate(name),
         index = index_path(name),
         true <- File.exists?(index) or {:error, :not_found} do
      out = Path.join(System.tmp_dir!(), "weft-stage-#{System.unique_integer([:positive])}.usd")

      try do
        case desync(["extract", "--store", store_dir(), index, out]) do
          {:ok, _} -> {:ok, File.read!(out)}
          error -> error
        end
      after
        File.rm_rf(out)
      end
    end
  end

  @doc "Path to the `.caibx` index for a stage name."
  @spec index_path(String.t()) :: Path.t()
  def index_path(name), do: Path.join(index_dir(), name <> ".caibx")

  defp desync(args) do
    case System.cmd("desync", args, stderr_to_stdout: true) do
      {out, 0} -> {:ok, out}
      {out, code} -> {:error, {:desync_failed, code, out}}
    end
  rescue
    e in ErlangError -> {:error, {:desync_unavailable, e}}
  end

  defp validate(name) do
    if name =~ ~r/\A[A-Za-z0-9_.-]+\z/, do: :ok, else: {:error, :bad_name}
  end

  defp store_dir, do: Path.join(base_dir(), "store")
  defp index_dir, do: Path.join(base_dir(), "index")

  defp base_dir do
    base = Application.get_env(:weft, :assets_dir) || Path.join(System.tmp_dir!(), "weft-assets")
    Path.join(base, "stages")
  end
end
