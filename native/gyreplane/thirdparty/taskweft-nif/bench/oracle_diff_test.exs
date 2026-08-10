#!/usr/bin/env elixir
# MiniZinc-as-teacher differential test against Taskweft.plan/1.
#
# Real, run comparison, not a hypothetical: for each problem variant
# below, this calls the real Taskweft.plan/1 (the "student") and shells
# out to a real `minizinc --solver gecode` run against
# bench/fixtures/oracle/warehouse_2robots_3pkg.mzn (the "teacher") to
# get the true optimal total_minutes, then reports the gap.
#
# Needs a real `minizinc` binary on PATH (with the Gecode solver, which
# ships built in). Run with: mix run bench/oracle_diff_test.exs

defmodule OracleDiffTest do
  @oracle_dir Path.join([__DIR__, "fixtures", "oracle"])
  @oracle_mzn Path.join(@oracle_dir, "warehouse_2robots_3pkg.mzn")

  @domain_path Path.join([
                 __DIR__,
                 "fixtures",
                 "domains",
                 "warehouse_domain_2robots.jsonld"
               ])

  # Each case: {label, robot_1 start, robot_2 start, oracle .dzn file}.
  @cases [
    {"robot_1 starts at dock", "dock", "shipping", "robot1_dock.dzn"},
    {"robot_2 starts at dock", "shipping", "dock", "robot2_dock.dzn"}
  ]

  def run do
    domain = @domain_path |> File.read!() |> Jason.decode!()

    Enum.each(@cases, fn {label, r1_at, r2_at, dzn} ->
      student_minutes = plan_total_minutes(domain, r1_at, r2_at)
      teacher_minutes = oracle_optimal_minutes(dzn)
      gap = student_minutes - teacher_minutes

      status = if gap == 0, do: "OPTIMAL", else: "SUBOPTIMAL (gap=#{gap}min)"

      IO.puts(
        "#{label}: student=#{student_minutes}min teacher=#{teacher_minutes}min -- #{status}"
      )
    end)
  end

  defp plan_total_minutes(domain, r1_at, r2_at) do
    problem_vars = [
      %{
        "name" => "at",
        "init" => %{
          "robot_1" => r1_at,
          "robot_2" => r2_at,
          "package_1" => "storage",
          "package_2" => "storage",
          "package_3" => "storage"
        }
      },
      %{"name" => "carrying", "init" => %{"robot_1" => nil, "robot_2" => nil}},
      %{
        "name" => "delivered",
        "init" => %{"package_1" => false, "package_2" => false, "package_3" => false}
      },
      %{"name" => "charged", "init" => %{"robot_1" => false, "robot_2" => false}}
    ]

    combined =
      domain
      |> Map.put("variables", problem_vars)
      |> Map.put("todo_list", [
        %{
          "multigoal" => %{
            "delivered" => %{"package_1" => true, "package_2" => true, "package_3" => true}
          }
        }
      ])
      |> Map.delete("@type")

    {:ok, plan_json} = Taskweft.plan(Jason.encode!(combined))
    %{"temporal" => %{"total" => total_iso}} = Jason.decode!(plan_json)
    iso8601_duration_to_minutes(total_iso)
  end

  # Minimal ISO 8601 duration parser for this domain's own output shape
  # (PT<minutes>M or PT<hours>H, the only units these action durations
  # ever produce -- not a general ISO 8601 parser).
  defp iso8601_duration_to_minutes("PT" <> rest) do
    cond do
      String.ends_with?(rest, "H") ->
        rest |> String.trim_trailing("H") |> String.to_integer() |> Kernel.*(60)

      String.ends_with?(rest, "M") ->
        rest |> String.trim_trailing("M") |> String.to_integer()

      true ->
        raise "unrecognized duration unit: #{rest}"
    end
  end

  defp oracle_optimal_minutes(dzn_file) do
    dzn_path = Path.join(@oracle_dir, dzn_file)

    {output, 0} =
      System.cmd("minizinc", ["--solver", "gecode", @oracle_mzn, dzn_path])

    [_, minutes] = Regex.run(~r/total_minutes = (\d+);/, output)
    String.to_integer(minutes)
  end
end

OracleDiffTest.run()
