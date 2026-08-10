make_problem = fn n ->
  done_init = for i <- 1..n, into: %{}, do: {"i#{i}", false}

  actions =
    for i <- 1..n, into: %{} do
      name = "mark_#{i}"
      {name,
       %{
         "params" => [],
         "body" => [
           %{"pointer/set" => "/done/i#{i}", "value" => true}
         ]
       }}
    end

  base_checks =
    (for i <- 1..n, do: %{"pointer" => "/done/i#{i}", "eq" => true}) ++
      [%{"pointer" => "/target/t", "eq" => true}]

  recurse_alts =
    for i <- 1..n do
      %{
        "name" => "choose_#{i}",
        "check" => [%{"pointer" => "/done/i#{i}", "eq" => false}],
        "subtasks" => [["mark_#{i}"], ["finish"]]
      }
    end

  %{
    "@context" => %{
      "vsekai" => "https://v-sekai.org/",
      "domain" => "vsekai:planning/domain/"
    },
    "@type" => "domain:Definition",
    "name" => "adversarial_subset_permutations",
    "variables" => [
      %{"name" => "done", "init" => done_init},
      %{"name" => "target", "init" => %{"t" => false}}
    ],
    "actions" => actions,
    "methods" => %{
      "finish" => %{
        "params" => [],
        "alternatives" =>
          [
            %{"name" => "done_and_target", "check" => base_checks, "subtasks" => []}
            | recurse_alts
          ]
      }
    },
    "todo_list" => [["finish"]]
  }
end

bench = fn n, iters ->
  payload = Jason.encode!(make_problem.(n))
  for _ <- 1..10, do: Taskweft.plan(payload)

  times =
    for _ <- 1..iters do
      {us, _} = :timer.tc(fn -> Taskweft.plan(payload) end)
      us
    end

  avg = Enum.sum(times) / iters
  p50 = Enum.at(Enum.sort(times), div(iters, 2))
  IO.puts("n=#{n} avg_us=#{Float.round(avg, 2)} p50_us=#{p50}")
end

IO.puts("branch=#{String.trim(System.cmd("git", ["branch", "--show-current"]) |> elem(0))}")
IO.puts("commit=#{String.trim(System.cmd("git", ["rev-parse", "--short", "HEAD"]) |> elem(0))}")
Enum.each([10, 12, 14, 16], fn n -> bench.(n, 200) end)
