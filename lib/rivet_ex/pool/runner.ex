defmodule RivetEx.Pool.Runner do
  @moduledoc """
  A serverless runner process: the unit the reconciler starts and drains to match
  demand. In the Rust engine this is the outbound serverless connection that POSTs
  `/start` and hosts actors for its lifespan. Here it is a plain supervised process
  so the reconciler's start/drain/self-heal behaviour can be exercised directly.
  """

  use GenServer

  @spec start_link(keyword()) :: GenServer.on_start()
  def start_link(opts \\ []), do: GenServer.start_link(__MODULE__, opts)

  @doc """
  Start a runner not linked to the caller. The reconciler owns runners through
  monitors, not links, so a runner crash re-converges the pool instead of taking
  the reconciler down with it. In production this is the runner supervisor's job;
  standalone `start/1` keeps the slice self-contained.
  """
  @spec start(keyword()) :: GenServer.on_start()
  def start(opts \\ []), do: GenServer.start(__MODULE__, opts)

  @doc "Gracefully drain and stop the runner."
  @spec drain(pid()) :: :ok
  def drain(pid), do: GenServer.stop(pid, :normal)

  @impl true
  def init(opts) do
    # Signal "started" the way the mock `/start` endpoint does in the Rust
    # integration test, so a pool scale-up is observable.
    case Keyword.get(opts, :on_start) do
      fun when is_function(fun, 0) -> fun.()
      _ -> :ok
    end

    {:ok, %{started_at: System.system_time(:millisecond)}}
  end
end
