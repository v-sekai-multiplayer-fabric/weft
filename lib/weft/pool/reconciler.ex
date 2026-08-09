defmodule Weft.Pool.Reconciler do
  @moduledoc """
  Level-triggered runner-pool reconciler.

  Every tick (and on `bump/1`) it observes live demand, computes the desired
  runner count with `Weft.Pool.desired_runners/2`, and converges the live set
  of runner processes toward it: starting runners when short, draining when long.

  Because desired is derived from `observe.()` and the live set is derived from
  monitored processes, both sides are observations of reality, never accumulators.
  A runner crash is just a smaller live set that the next reconcile refills, so the
  pool self-heals. This is the OTP port of the `pegboard_runner_pool2` loop plus
  `read_desired` in `engine/packages/pegboard/src/workflows/runner_pool.rs`, minus
  the `ServerlessDesiredSlotsKey` counter that made the Rust version jammable.

  Injectables (so the demand source and runner factory can be tested and later
  swapped for the real actor registry and runner supervisor):

    * `:observe` — `(-> non_neg_integer())` returning current demand in slots
    * `:start_runner` — `(-> {:ok, pid()})` starting one runner process
  """

  use GenServer

  alias Weft.Pool

  @type option ::
          {:name, GenServer.name()}
          | {:config, Pool.Config.t()}
          | {:observe, (-> non_neg_integer())}
          | {:start_runner, (-> {:ok, pid()})}
          | {:tick_ms, pos_integer()}

  @spec start_link([option()]) :: GenServer.on_start()
  def start_link(opts) do
    GenServer.start_link(__MODULE__, opts, name: Keyword.fetch!(opts, :name))
  end

  @doc "Wake the reconciler to re-observe demand immediately (the pool `Bump` signal)."
  @spec bump(GenServer.server()) :: :ok
  def bump(server), do: GenServer.cast(server, :reconcile)

  @doc "Current number of live runners the pool owns."
  @spec runner_count(GenServer.server()) :: non_neg_integer()
  def runner_count(server), do: GenServer.call(server, :runner_count)

  @impl true
  def init(opts) do
    state = %{
      config: Keyword.get(opts, :config, %Pool.Config{}),
      observe: Keyword.fetch!(opts, :observe),
      start_runner: Keyword.fetch!(opts, :start_runner),
      tick_ms: Keyword.get(opts, :tick_ms, 1_000),
      runners: MapSet.new()
    }

    schedule_tick(state.tick_ms)
    {:ok, reconcile(state)}
  end

  @impl true
  def handle_cast(:reconcile, state), do: {:noreply, reconcile(state)}

  @impl true
  def handle_call(:runner_count, _from, state) do
    {:reply, MapSet.size(state.runners), state}
  end

  @impl true
  def handle_info(:tick, state) do
    schedule_tick(state.tick_ms)
    {:noreply, reconcile(state)}
  end

  # A runner died. Drop it from the live set and re-converge from reality, so a
  # crash cannot leave the pool under-provisioned. No counter to correct.
  def handle_info({:DOWN, _ref, :process, pid, _reason}, state) do
    {:noreply, reconcile(%{state | runners: MapSet.delete(state.runners, pid)})}
  end

  defp reconcile(state) do
    demand = state.observe.()
    desired = Pool.desired_runners(demand, state.config)
    current = MapSet.size(state.runners)

    cond do
      desired > current -> Enum.reduce(1..(desired - current), state, fn _, s -> start_one(s) end)
      desired < current -> Enum.reduce(1..(current - desired), state, fn _, s -> drain_one(s) end)
      true -> state
    end
  end

  defp start_one(state) do
    {:ok, pid} = state.start_runner.()
    Process.monitor(pid)
    %{state | runners: MapSet.put(state.runners, pid)}
  end

  defp drain_one(state) do
    case Enum.take(state.runners, 1) do
      [pid] ->
        Weft.Pool.Runner.drain(pid)
        %{state | runners: MapSet.delete(state.runners, pid)}

      [] ->
        state
    end
  end

  defp schedule_tick(ms), do: Process.send_after(self(), :tick, ms)
end
