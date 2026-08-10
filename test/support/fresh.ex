defmodule Weft.Test.Fresh do
  @moduledoc """
  Identifiers for a test that writes to something that outlives the test run.

  `System.unique_integer/1` is unique inside one virtual machine, and it starts again at
  the next one. A test that names an actor `repl-1` gets `repl-1` again on the next run.
  Local state does not notice, because each run makes a new data directory. FoundationDB
  does notice. It keeps what the last run wrote, so the second run reads rows that the
  first one left under the same name.

  That is not a rare race. It failed for us on the second run against a cluster that was
  already used, and the failure named a value from another test, which sends a reader
  looking for a bug in the code under test.

  A UUID version 7 does not repeat across runs, because it carries a timestamp and random
  bits rather than a counter that resets. It also sorts by time, so the rows of one run sit
  together and a person can read them.

  Use `id/1` for anything that reaches FoundationDB or another store that persists. A test
  that only touches memory can keep `System.unique_integer/1`, which is cheaper.
  """

  @doc """
  A fresh name, with `prefix` in front of it so a person can tell what wrote the row.

      iex> Weft.Test.Fresh.id("repl")
      "repl-0191f0a1-...

  """
  @spec id(String.t()) :: String.t()
  def id(prefix), do: "#{prefix}-#{UUIDv7.generate()}"

  @doc "A fresh actor id, which is a name and a key."
  @spec actor_id(String.t(), String.t()) :: {String.t(), String.t()}
  def actor_id(name \\ "zone", prefix), do: {name, id(prefix)}
end
