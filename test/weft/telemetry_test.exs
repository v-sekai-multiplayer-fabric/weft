defmodule Weft.TelemetryTest do
  @moduledoc """
  Drives a real actor workload and reads the numbers back off the spans. This is
  observability doing double duty: it proves the instrumentation captures the
  actor operations, and it prints live per-op latency for the actor system.
  """

  use ExUnit.Case, async: false

  alias Weft.{Actor, Actors, Gateway, Telemetry, Zone}
  alias Weft.Gateway.Request

  setup do
    Telemetry.attach()
    Telemetry.reset()
    on_exit(&Telemetry.detach/0)
    :ok
  end

  test "actor operations are measured, and their numbers are reported" do
    n = 500

    # A real workload: create actors, write and read their state.
    for i <- 1..n do
      {:ok, pid} = Actors.get_or_create("player", "tel-#{i}")
      Actor.put(pid, :hp, i)
      Actor.get(pid, :hp)
    end

    # Zone authority ops and a handoff.
    a = "telA-#{System.unique_integer([:positive])}"
    b = "telB-#{System.unique_integer([:positive])}"
    {:ok, _} = Zone.start_link(zone_id: a, worker_opts: [tick_ms: 3_600_000])
    {:ok, _} = Zone.start_link(zone_id: b, worker_opts: [tick_ms: 3_600_000])
    for i <- 1..200, do: Zone.add_entity(a, "e#{i}", %{hp: i})
    for i <- 1..50, do: Zone.handoff(a, b, "e#{i}")

    # Gateway-routed reads.
    for i <- 1..100 do
      Gateway.dispatch(%Request{target: {:actor, "player", "tel-#{i}"}, op: :get, args: [:hp]})
    end

    snap = Telemetry.snapshot()

    assert snap["actor.put"].count == n
    assert snap["actor.get"].count >= n
    assert snap["zone.add_entity"].count == 200
    assert snap["zone.handoff"].count == 50
    assert snap["gateway.dispatch"].count == 100
    assert snap["actors.get_or_create"].count >= n

    # Print the live numbers for our actors.
    IO.puts("\nactor telemetry (avg latency per op):")

    snap
    |> Enum.sort()
    |> Enum.each(fn {op, %{count: c, avg_us: us}} ->
      IO.puts(
        "  #{String.pad_trailing(op, 24)} #{String.pad_leading(to_string(c), 6)} ops   #{:erlang.float_to_binary(us, decimals: 2)} µs avg"
      )
    end)
  end
end
