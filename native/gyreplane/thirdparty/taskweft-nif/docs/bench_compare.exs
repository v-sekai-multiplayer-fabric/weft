read_json = fn path ->
  {:ok, raw} = File.read(path)

  raw
  |> String.trim_leading("\uFEFF")
  |> Jason.decode!()
end

merge_domain_problem = fn domain, problem ->
  domain
  |> Map.put("variables", Map.get(problem, "variables", Map.get(domain, "variables", [])))
  |> Map.put("todo_list", Map.get(problem, "todo_list", Map.get(domain, "todo_list", [])))
  |> (fn m ->
    case Map.fetch(problem, "goals") do
      {:ok, goals} -> Map.put(m, "goals", goals)
      :error -> Map.delete(m, "goals")
    end
  end).()
end

domain_path = "bench/fixtures/domains/warehouse_domain.jsonld"
problem_paths = Path.wildcard("bench/fixtures/problems/*.jsonld") |> Enum.sort()
base_domain = read_json.(domain_path)

inputs =
  Enum.into(problem_paths, %{}, fn path ->
    problem = read_json.(path)
    merged = merge_domain_problem.(base_domain, problem)
    {Path.basename(path), Jason.encode!(merged)}
  end)

branch = String.trim(System.cmd("git", ["branch", "--show-current"]) |> elem(0))
commit = String.trim(System.cmd("git", ["rev-parse", "--short", "HEAD"]) |> elem(0))

IO.puts("branch=#{branch}")
IO.puts("commit=#{commit}")

Benchee.run(
  %{
    "Taskweft.plan" => fn merged_json ->
      case Taskweft.plan(merged_json) do
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
