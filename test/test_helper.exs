# Persist actor state under a throwaway directory for the test run.
data_dir = Path.join(System.tmp_dir!(), "rivet_ex_test_#{System.unique_integer([:positive])}")
File.mkdir_p!(data_dir)
Application.put_env(:rivet_ex, :data_dir, data_dir)

ExUnit.after_suite(fn _ -> File.rm_rf(data_dir) end)

# Make the test node distributed so `:peer` can spawn cluster members. If it
# cannot start (no epmd / restricted env), distributed tests are excluded.
distributed? =
  case Node.start(:"rivet_primary@127.0.0.1", :longnames) do
    {:ok, _} -> true
    {:error, {:already_started, _}} -> true
    _ -> false
  end

if distributed?, do: Node.set_cookie(:rivet_ex_test)

ExUnit.start(exclude: if(distributed?, do: [], else: [:distributed]))
