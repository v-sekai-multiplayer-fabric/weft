defmodule RivetEx.PoolTest do
  @moduledoc """
  The Elixir counterpart of `engine/packages/engine/tests/serverless_pool_reconcile.rs`
  and of the Lean theorems in `proofs/serverless_pool_jam.lean`.

  In the Rust engine the jam was reachable: a drifting counter read zero desired
  while an actor waited, permanently. Here there is no counter, so the analogous
  test is a positive statement that observed demand is always served, and the
  `reconciler_never_jams` property holds for every input.
  """

  use ExUnit.Case, async: true
  use ExUnitProperties

  alias RivetEx.Pool
  alias RivetEx.Pool.{Config, Reconciler}

  describe "desired_runners/2 (pure core, port of read_desired / desiredRunners)" do
    test "zero demand wants no runners (above the min floor)" do
      assert Pool.desired_runners(0, %Config{min_runners: 0}) == 0
    end

    test "demand rounds up to whole runners by slots_per_runner" do
      config = %Config{slots_per_runner: 4}
      assert Pool.desired_runners(1, config) == 1
      assert Pool.desired_runners(4, config) == 1
      assert Pool.desired_runners(5, config) == 2
    end

    test "margin, min and max bound the result" do
      config = %Config{slots_per_runner: 1, runners_margin: 2, min_runners: 1, max_runners: 3}
      assert Pool.desired_runners(0, config) == 2
      assert Pool.desired_runners(10, config) == 3
    end

    # The Lean `reconciler_never_jams`: for any slots_per_runner >= 1 and any
    # positive demand, at least one runner is desired. No trace can wedge it.
    property "positive demand always wants at least one runner (jam-free)" do
      check all(
              demand <- integer(1..1_000_000),
              slots_per_runner <- integer(1..64),
              margin <- integer(0..8),
              max_runners <- integer(1..500)
            ) do
        config = %Config{
          slots_per_runner: slots_per_runner,
          runners_margin: margin,
          min_runners: 0,
          max_runners: max_runners
        }

        assert Pool.desired_runners(demand, config) > 0
      end
    end
  end

  describe "Reconciler (level-triggered convergence)" do
    test "starts a runner to serve observed demand" do
      test_pid = self()
      {:ok, demand} = Agent.start_link(fn -> 0 end)
      name = unique_name()

      start_supervised!({
        Reconciler,
        # Long tick so the assertion exercises the bump path, not the timer.
        name: name,
        config: %Config{slots_per_runner: 1, max_runners: 1},
        observe: fn -> Agent.get(demand, & &1) end,
        start_runner: fn -> start_signalling_runner(test_pid) end,
        tick_ms: 60_000
      })

      # No demand yet: nothing started.
      refute_receive :runner_started, 100

      # A queued actor appears. Bump, and the pool must start a runner.
      Agent.update(demand, fn _ -> 1 end)
      Reconciler.bump(name)
      assert_receive :runner_started, 1_000
      assert Reconciler.runner_count(name) == 1
    end

    test "self-heals when a runner crashes (reality re-observed, no counter to correct)" do
      test_pid = self()
      name = unique_name()

      start_supervised!(
        {Reconciler,
         name: name,
         config: %Config{slots_per_runner: 1, min_runners: 1, max_runners: 1},
         observe: fn -> 1 end,
         start_runner: fn -> start_signalling_runner(test_pid) end,
         tick_ms: 60_000}
      )

      assert_receive {:runner_started, first_pid}, 1_000
      assert Reconciler.runner_count(name) == 1

      # Kill the runner. The reconciler observes the smaller live set on the DOWN
      # and re-converges to the desired 1 by starting a fresh runner.
      Process.exit(first_pid, :kill)
      assert_receive {:runner_started, second_pid}, 1_000
      assert second_pid != first_pid
      assert Reconciler.runner_count(name) == 1
    end

    test "drains runners when demand falls" do
      {:ok, demand} = Agent.start_link(fn -> 3 end)
      name = unique_name()

      start_supervised!(
        {Reconciler,
         name: name,
         config: %Config{slots_per_runner: 1, max_runners: 5},
         observe: fn -> Agent.get(demand, & &1) end,
         start_runner: fn -> RivetEx.Pool.Runner.start([]) end,
         tick_ms: 60_000}
      )

      assert Reconciler.runner_count(name) == 3

      Agent.update(demand, fn _ -> 1 end)
      Reconciler.bump(name)
      assert Reconciler.runner_count(name) == 1
    end
  end

  defp unique_name, do: :"pool_#{System.unique_integer([:positive])}"

  # A runner that reports both a bare and a tagged "started" so tests can either
  # just wait for a start or capture the pid.
  defp start_signalling_runner(test_pid) do
    RivetEx.Pool.Runner.start(
      on_start: fn ->
        send(test_pid, :runner_started)
        send(test_pid, {:runner_started, self()})
      end
    )
  end
end
