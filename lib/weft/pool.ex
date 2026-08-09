defmodule Weft.Pool.Config do
  @moduledoc """
  Serverless runner-pool configuration. Mirrors the serverless fields of
  `rivet_types::runner_configs::RunnerConfigKind::Serverless`.
  """

  @type t :: %__MODULE__{
          slots_per_runner: pos_integer(),
          min_runners: non_neg_integer(),
          max_runners: non_neg_integer(),
          runners_margin: non_neg_integer()
        }

  defstruct slots_per_runner: 1, min_runners: 0, max_runners: 100, runners_margin: 0
end

defmodule Weft.Pool do
  @moduledoc """
  The level-triggered reconciler's pure core: how many runners a pool should have,
  derived entirely from *observed live demand*.

  This is the Elixir port of `read_desired` in
  `engine/packages/pegboard/src/workflows/runner_pool.rs` and of `desiredRunners`
  in `engine/packages/pegboard/proofs/serverless_pool_jam.lean`.

  The point of the port: rivet drove scaling from an accumulated
  `ServerlessDesiredSlotsKey` counter that drifted negative under churn and wedged
  the pool at zero desired runners forever (`counter_can_jam` / `jam_is_sink`).
  Here there is no accumulator at all. Desired is recomputed from reality every
  tick, so it cannot drift, and the jam state is unrepresentable. This is the OTP
  supervisor discipline: observe the live set, converge to the spec, never count.
  """

  alias Weft.Pool.Config

  @doc """
  Desired runner count for an observed `demand` (in slots).

  `demand` is the number of live actor-slots wanted: actors waiting in the pending
  queue plus actors already running (used slots). Overcounting is safe (a spare
  runner drains next tick); undercounting would drain a runner hosting a live
  actor, so callers bias the count upward.
  """
  @spec desired_runners(non_neg_integer(), Config.t()) :: non_neg_integer()
  def desired_runners(demand, %Config{} = config) when is_integer(demand) and demand >= 0 do
    slots_per_runner = max(config.slots_per_runner, 1)

    (config.runners_margin + ceil_div(demand, slots_per_runner))
    |> max(config.min_runners)
    |> min(config.max_runners)
  end

  defp ceil_div(numerator, denominator), do: div(numerator + denominator - 1, denominator)
end
