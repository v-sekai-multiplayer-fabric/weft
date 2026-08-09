defmodule Weft.Gateway.SeqGuard do
  @moduledoc """
  Per-target last-write-wins sequence guard for unreliable datagrams. `fresh?/2`
  returns true and records the sequence only when it is newer than the last one
  seen for that target; a stale or out-of-order sequence returns false so the
  gateway can drop it. Best-effort by design (datagrams are), so a rare concurrent
  race admitting a marginally-stale packet is acceptable.
  """

  @table __MODULE__

  @spec fresh?(term(), non_neg_integer()) :: boolean()
  def fresh?(target, seq) when is_integer(seq) do
    ensure()
    last = :ets.lookup_element(@table, target, 2, -1)

    if seq > last do
      :ets.insert(@table, {target, seq})
      true
    else
      false
    end
  end

  @spec reset() :: :ok
  def reset do
    ensure()
    :ets.delete_all_objects(@table)
    :ok
  end

  defp ensure do
    case :ets.whereis(@table) do
      :undefined -> new_table()
      _tid -> :ok
    end
  end

  defp new_table do
    _ = :ets.new(@table, [:named_table, :public, :set, write_concurrency: true])
    :ok
  rescue
    ArgumentError -> :ok
  end
end
