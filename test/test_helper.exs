# Persist actor state under a throwaway directory for the test run.
data_dir = Path.join(System.tmp_dir!(), "rivet_ex_test_#{System.unique_integer([:positive])}")
File.mkdir_p!(data_dir)
Application.put_env(:rivet_ex, :data_dir, data_dir)

ExUnit.after_suite(fn _ -> File.rm_rf(data_dir) end)

ExUnit.start()
