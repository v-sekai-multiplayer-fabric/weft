# Actor control-plane op microbenchmark (SQLite store).
# Run: mix run test/bench/actor.exs
alias Weft.{Actor, Actors}

data_dir = Path.join(System.tmp_dir!(), "weft_bench_#{System.unique_integer([:positive])}")
File.mkdir_p!(data_dir)
Application.put_env(:weft, :data_dir, data_dir)

{:ok, warm} = Actors.get_or_create("bench", "warm")
Actor.put(warm, :k, 1)

Benchee.run(
  %{
    "actor put (call + write-through)" => fn ->
      Actor.put(warm, :k, :rand.uniform(1_000_000))
    end,
    "actor get (call + memory cache)" => fn -> Actor.get(warm, :k) end,
    "get_or_create warm (registry hit)" => fn -> Actors.get_or_create("bench", "warm") end,
    "get_or_create cold (spawn+open+restore)" => fn ->
      {:ok, _} = Actors.get_or_create("bench", "cold-#{System.unique_integer([:positive])}")
    end
  },
  time: 3,
  warmup: 1,
  print: [fast_warning: false]
)
