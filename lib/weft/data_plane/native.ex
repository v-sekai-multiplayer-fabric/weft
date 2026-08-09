defmodule Weft.DataPlane.Native do
  @moduledoc """
  The real native data-plane worker: a C NIF that runs an OS thread writing
  snapshots into a native-owned lock-free ring (seqlock), which the BEAM samples.
  This is the hot loop outside the scheduler that the Elixir stub stands in for.

  No exceptions: the NIF returns `{:ok, ref}` / `{:error, reason}` / badarg, never
  raises. If the NIF is not built, the stubs return `{:error, :nif_not_loaded}`.
  """

  @on_load :load_nif

  # The stub bodies only return {:error, :nif_not_loaded}; the NIF replaces them at
  # load with the full success/error surface in the specs. Suppress the resulting
  # extra_range without resorting to a raising :erlang.nif_error stub.
  @dialyzer {:nowarn_function, [start: 1, sample: 1, stop: 1]}

  @doc false
  def load_nif do
    path = :filename.join(:code.priv_dir(:weft), ~c"weft_dataplane_nif")

    case :erlang.load_nif(path, 0) do
      :ok -> :ok
      {:error, _} -> :ok
    end
  end

  @doc "Start a native producer for `entities` (1..64). Returns an opaque worker ref."
  @spec start(pos_integer()) :: {:ok, reference()} | {:error, term()}
  def start(_entities), do: {:error, :nif_not_loaded}

  @doc "Sample the latest snapshot as `{tick, [coord]}` (fixed-point ints)."
  @spec sample(reference()) :: {non_neg_integer(), [integer()]} | {:error, term()}
  def sample(_ref), do: {:error, :nif_not_loaded}

  @doc "Stop the native producer thread."
  @spec stop(reference()) :: :ok | {:error, term()}
  def stop(_ref), do: {:error, :nif_not_loaded}
end
