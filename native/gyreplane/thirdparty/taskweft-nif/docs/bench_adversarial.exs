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
    (for i <- 1..n do
       %{"pointer" => "/done/i#{i}", "eq" => true}
     end) ++ [%{"pointer" => "/target/t", "eq" => true}]

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
        "alternatives" => [
          %{"name" => "done_and_target", "check" => base_checks, "subtasks" => []}
          | recurse_alts
        ]
      }
    },
    "todo_list" => [["finish"]]
  }
end

sizes = [6, 7, 8, 9, 10, 11, 12]

inputs =
  Enum.into(sizes, %{}, fn n ->
    {"n=#{n}", Jason.encode!(make_problem.(n))}
  end)

branch = String.trim(System.cmd("git", ["branch", "--show-current"]) |> elem(0))
commit = String.trim(System.cmd("git", ["rev-parse", "--short", "HEAD"]) |> elem(0))

IO.puts("branch=#{branch}")
IO.puts("commit=#{commit}")

Benchee.run(
  %{
    "Taskweft.plan adversarial" => fn payload ->
      case Taskweft.plan(payload) do
        {:ok, _plan_json} -> :ok
        {:error, _reason} -> :no_plan
      end
    end
  },
  inputs: inputs,
  warmup: 2,
  time: 8,
  memory_time: 2,
  formatters: [{Benchee.Formatters.Console, comparison: true}],
  print: [fast_warning: false]
)
