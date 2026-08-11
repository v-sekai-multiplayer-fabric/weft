# Persist actor state under a throwaway directory for the test run.
data_dir = Path.join(System.tmp_dir!(), "weft_test_#{System.unique_integer([:positive])}")
File.mkdir_p!(data_dir)
Application.put_env(:weft, :data_dir, data_dir)

ExUnit.after_suite(fn _ -> File.rm_rf(data_dir) end)

# Make the test node distributed so `:peer` can spawn cluster members.
#
# Every branch below names the result it handles. An earlier version ended each of these
# with `_ -> false`, which turned every failure into an excluded tag. A broken cluster
# file, a stopped database and a missing one all became the same green run. A skipped
# test proves nothing, and a suite that reports success while proving nothing is worse
# than a suite that fails.
distributed? =
  case Node.start(:"weft_primary@127.0.0.1", :longnames) do
    {:ok, _pid} -> true
    {:error, {:already_started, _pid}} -> true
    {:error, _reason} -> false
  end

if distributed?, do: Node.set_cookie(:weft_test)

# Find FoundationDB, and tell the difference between "none was asked for" and "the one
# that was asked for does not work".
#
# `WEFT_FDB_CLUSTER_FILE` is a request. A request that cannot be met raises here and stops
# the run, because the operator asked for the `:fdb` tests and must not be told they
# passed. With no request, weft looks in the default place and excludes the tag when it
# finds nothing, which is the case where nobody asked for a database.
requested = System.get_env("WEFT_FDB_CLUSTER_FILE")
fdb_cluster_file = requested || "/tmp/weft_fdb/fdb.cluster"

probe = fn path ->
  Application.put_env(:weft, :fdb_cluster_file, path)

  task =
    Task.async(fn ->
      db = :erlfdb.open(path)
      :erlfdb.wait(:erlfdb.get(db, "weft/probe"))
      :reachable
    end)

  # The FoundationDB client retries an unreachable coordinator forever, so a probe with
  # no bound never returns. This bound is not a tuning constant. It is the point where a
  # hang becomes a report, and the report says which it was.
  case Task.yield(task, 5_000) || Task.shutdown(task, :brutal_kill) do
    {:ok, :reachable} -> :reachable
    {:exit, reason} -> {:unreachable, reason}
    nil -> {:unreachable, :no_answer_in_5s}
  end
end

fdb? =
  case {requested, File.exists?(fdb_cluster_file)} do
    {nil, false} ->
      false

    {path, false} ->
      raise """
      WEFT_FDB_CLUSTER_FILE names #{inspect(path)} and no file is there.

      The :fdb tests were asked for and they cannot run, so this run stops instead of
      excluding them. Start the local database and create it once:

          systemctl --user start weft-fdb.service
          fdbcli -C #{path} --exec "configure new single ssd"
      """

    {_requested_or_nil, true} ->
      case probe.(fdb_cluster_file) do
        :reachable ->
          true

        {:unreachable, reason} when is_nil(requested) ->
          IO.puts("""
          :fdb tests excluded. #{fdb_cluster_file} exists and did not answer: \
          #{inspect(reason)}
          """)

          false

        {:unreachable, reason} ->
          raise """
          WEFT_FDB_CLUSTER_FILE names #{inspect(fdb_cluster_file)} and it did not answer:
          #{inspect(reason)}

          The :fdb tests were asked for, so this run stops rather than reporting a pass
          it did not earn. Check the database is up and created:

              systemctl --user status weft-fdb.service
              fdbcli -C #{fdb_cluster_file} --exec "status minimal"
          """
      end
  end

desync? = System.find_executable("desync") != nil

excluded =
  [] ++
    if(distributed?, do: [], else: [:distributed]) ++
    if(fdb?, do: [], else: [:fdb]) ++
    if(desync?, do: [], else: [:desync])

if excluded != [] do
  IO.puts("excluded tags: #{inspect(excluded)}")
end

ExUnit.start(exclude: excluded)
