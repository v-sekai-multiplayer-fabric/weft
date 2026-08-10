defmodule Weft.DataPlane.Ring do
  @moduledoc """
  Lock-free shared slot for a zone's latest digested world state, the BEAM stand-in
  for the shared-memory ring in `Weft.DataPlane` (contract 2).

  The data-plane worker *overwrites* the slot as fast as it produces snapshots; the
  BEAM *samples* the latest at tick rate. Nothing is passed as a message and nothing
  is copied per snapshot, so ingestion is not bounded by mailbox throughput. Backed
  by `:atomics` (an off-heap, lock-free int array), the same shape a C++ worker
  would write through a NIF.

  Tear-free reads use a seqlock: the writer bumps a generation to odd before writing
  and to even after; a reader retries if the generation is odd or changed across the
  read. Coordinates are stored as fixed-point millimetres (i64).
  """

  @gen 1
  @tick 2
  @data_base 3

  @type t :: %__MODULE__{ref: :atomics.atomics_ref(), max_entities: pos_integer()}
  @enforce_keys [:ref, :max_entities]
  defstruct [:ref, :max_entities]

  @spec new(pos_integer()) :: t()
  def new(max_entities) when max_entities > 0 do
    ref = :atomics.new(@data_base - 1 + max_entities * 3, signed: true)
    %__MODULE__{ref: ref, max_entities: max_entities}
  end

  @doc """
  Overwrite the slot with a new snapshot. `entities` is a flat list of fixed-point
  integers `[x1, y1, z1, x2, y2, z2, ...]` (already the worker's native layout), so
  the write is pure `:atomics` stores with no per-entity term allocation.
  """
  @spec write(t(), non_neg_integer(), [integer()]) :: :ok
  def write(%__MODULE__{ref: ref}, tick, coords) do
    gen = :atomics.get(ref, @gen)
    :atomics.put(ref, @gen, gen + 1)
    :atomics.put(ref, @tick, tick)
    put_coords(ref, @data_base, coords)
    :atomics.put(ref, @gen, gen + 2)
    :ok
  end

  @doc "Sample the latest snapshot tear-free as `{tick, [coords]}`."
  @spec read(t()) :: {non_neg_integer(), [integer()]}
  def read(%__MODULE__{ref: ref, max_entities: max}) do
    gen1 = :atomics.get(ref, @gen)

    if rem(gen1, 2) == 1 do
      read(%__MODULE__{ref: ref, max_entities: max})
    else
      tick = :atomics.get(ref, @tick)
      coords = get_coords(ref, @data_base, max * 3, [])
      gen2 = :atomics.get(ref, @gen)
      if gen1 == gen2, do: {tick, coords}, else: read(%__MODULE__{ref: ref, max_entities: max})
    end
  end

  defp put_coords(_ref, _ix, []), do: :ok

  defp put_coords(ref, ix, [c | rest]) do
    :atomics.put(ref, ix, c)
    put_coords(ref, ix + 1, rest)
  end

  defp get_coords(_ref, _ix, 0, acc), do: :lists.reverse(acc)

  defp get_coords(ref, ix, n, acc) do
    get_coords(ref, ix + 1, n - 1, [:atomics.get(ref, ix) | acc])
  end
end
