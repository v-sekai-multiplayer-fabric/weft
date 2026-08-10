# Store backend microbenchmark: node-local SQLite vs node-independent FoundationDB.
# Run: mix run test/bench/store.exs   (requires a reachable FDB; see test_helper)
#
# `Store.Sqlite` uses the default rollback journal. `Store.Replicated` uses WAL with
# `synchronous=NORMAL`, so it does not fsync on each commit. Replicated is the store the
# design turns on, so it is measured here and not assumed to match Sqlite.
alias Weft.Actor.Store

data_dir = Path.join(System.tmp_dir!(), "weft_bench_#{System.unique_integer([:positive])}")
File.mkdir_p!(data_dir)
Application.put_env(:weft, :data_dir, data_dir)

Application.put_env(
  :weft,
  :fdb_cluster_file,
  System.get_env("WEFT_FDB_CLUSTER_FILE", "/tmp/weft_fdb/fdb.cluster")
)

{:ok, sqlite} = Store.Sqlite.open({"bench", "sqlite"})
{:ok, fdb} = Store.Fdb.open({"bench", "fdb"})
{:ok, repl} = Store.Replicated.open({"bench", "replicated"})

# Seed 100 keys so load_all measures a realistic subspace scan.
for i <- 1..100 do
  Store.Sqlite.put(sqlite, {:seed, i}, i)
  Store.Fdb.put(fdb, {:seed, i}, i)
  Store.Replicated.put(repl, {:seed, i}, i)
end

Benchee.run(
  %{
    "sqlite put (rollback journal, fsync)" => fn ->
      Store.Sqlite.put(sqlite, :k, :rand.uniform(1_000_000))
    end,
    "replicated put (WAL, no fsync, async replica)" => fn ->
      Store.Replicated.put(repl, :k, :rand.uniform(1_000_000))
    end,
    "fdb put (one txn)" => fn -> Store.Fdb.put(fdb, :k, :rand.uniform(1_000_000)) end,
    "sqlite load_all (100 keys)" => fn -> Store.Sqlite.load_all(sqlite) end,
    "replicated load_all (100 keys)" => fn -> Store.Replicated.load_all(repl) end,
    "fdb load_all (100 keys)" => fn -> Store.Fdb.load_all(fdb) end
  },
  time: 3,
  warmup: 1,
  print: [fast_warning: false]
)
