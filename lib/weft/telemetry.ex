defmodule Weft.Telemetry do
  @moduledoc """
  Lean actor observability. The load-bearing actor operations emit `:telemetry`
  spans (start/stop with a duration); this module aggregates per-operation counts
  and average latency into one ETS table.

  Deliberately **not** OpenTelemetry: no collector, exporter, protobuf, sampler, or
  SDK. Just span timing and counts, a few dozen lines. Attach it when you want the
  numbers, detach when you don't; the spans themselves are near-free when nothing
  is attached.
  """

  @table __MODULE__

  # Span prefixes emitted by the instrumented operations. `:telemetry.span/3`
  # appends `:start` / `:stop`; we aggregate the `:stop` (which carries duration).
  @spans [
    [:weft, :actors, :get_or_create],
    [:weft, :actor, :put],
    [:weft, :actor, :get],
    [:weft, :zone, :add_entity],
    [:weft, :zone, :handoff],
    [:weft, :gateway, :dispatch]
  ]

  @doc "Start aggregating actor span timings into ETS."
  @spec attach() :: :ok | {:error, :already_exists}
  def attach do
    ensure_table()
    events = Enum.map(@spans, &(&1 ++ [:stop]))
    _ = :telemetry.detach("weft-telemetry")
    :telemetry.attach_many("weft-telemetry", events, &__MODULE__.handle/4, nil)
  end

  @doc "Stop aggregating."
  @spec detach() :: :ok | {:error, :not_found}
  def detach, do: :telemetry.detach("weft-telemetry")

  @doc "Clear all accumulated counters."
  @spec reset() :: :ok
  def reset do
    ensure_table()
    :ets.delete_all_objects(@table)
    :ok
  end

  @doc false
  def handle(event, %{duration: duration}, _meta, _cfg) do
    key = event |> Enum.drop(1) |> Enum.drop(-1) |> Enum.join(".")
    _ = :ets.update_counter(@table, key, [{2, 1}, {3, duration}], {key, 0, 0})
    :ok
  end

  @doc """
  Per-operation `%{count, avg_us}`, e.g.
  `%{"actor.put" => %{count: 500, avg_us: 68.4}, ...}`.
  """
  @spec snapshot() :: %{optional(String.t()) => %{count: non_neg_integer(), avg_us: float()}}
  def snapshot do
    ensure_table()

    for {key, count, sum} <- :ets.tab2list(@table), into: %{} do
      total_us = System.convert_time_unit(sum, :native, :microsecond)
      {key, %{count: count, avg_us: if(count > 0, do: total_us / count, else: 0.0)}}
    end
  end

  defp ensure_table do
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
