# Persist actor state under a throwaway directory for the test run.
data_dir = Path.join(System.tmp_dir!(), "weft_test_#{System.unique_integer([:positive])}")
File.mkdir_p!(data_dir)
Application.put_env(:weft, :data_dir, data_dir)

ExUnit.after_suite(fn _ -> File.rm_rf(data_dir) end)

# Make the test node distributed so `:peer` can spawn cluster members. If it
# cannot start (no epmd / restricted env), distributed tests are excluded.
distributed? =
  case Node.start(:"weft_primary@127.0.0.1", :longnames) do
    {:ok, _} -> true
    {:error, {:already_started, _}} -> true
    _ -> false
  end

if distributed?, do: Node.set_cookie(:weft_test)

# Detect a reachable FoundationDB so store-backend tests can run against it.
# Point at a local cluster file if present; probe with a timeout so an
# unreachable cluster excludes :fdb tests instead of hanging.
fdb_cluster_file = System.get_env("WEFT_FDB_CLUSTER_FILE", "/tmp/weft_fdb/fdb.cluster")

fdb? =
  File.exists?(fdb_cluster_file) and
    (
      Application.put_env(:weft, :fdb_cluster_file, fdb_cluster_file)

      task =
        Task.async(fn ->
          db = :erlfdb.open(fdb_cluster_file)
          :erlfdb.get(db, "weft/probe")
          true
        end)

      case Task.yield(task, 5_000) || Task.shutdown(task, :brutal_kill) do
        {:ok, true} -> true
        _ -> false
      end
    )

excluded =
  [] ++
    if(distributed?, do: [], else: [:distributed]) ++
    if(fdb?, do: [], else: [:fdb]) ++
    if(System.find_executable("desync"), do: [], else: [:desync])

ExUnit.start(exclude: excluded)
