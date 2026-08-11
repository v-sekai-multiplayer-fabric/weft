defmodule Weft.PackingTest do
  use ExUnit.Case, async: true

  @moduledoc """
  A machine is a packing of planes and edges, and a ring forces co-location.

  iceoryx2 is shared memory. So two planes that exchange per-tick data are on one machine,
  and that is a property of the transport rather than a setting somebody can relax. A
  document that offers to relax it is describing a system weft does not have, and the reader
  cannot tell that from the page alone.

  This is a test rather than a comment for the reason `Weft.VocabularyTest` gives: a decision
  written in two places drifts, and the stale copy still reads as authoritative.
  """

  @roots ["lib", "docs", "deploy", "native", "CLAUDE.md", "README.md"]

  # A phrase that promises a ring between machines. iceoryx does not cross a machine, so each
  # of these describes something that cannot be built.
  @contradictions [
    "iceoryx over the network",
    "iceoryx across machines",
    "iceoryx2 over the network",
    "iceoryx2 across machines",
    "ring between machines",
    "ring across machines",
    "shared memory between machines",
    "remote ring"
  ]

  defp files do
    {out, 0} = System.cmd("git", ["ls-files", "--cached", "--others", "--exclude-standard"])

    out
    |> String.split("\n", trim: true)
    |> Enum.filter(fn path ->
      Enum.any?(@roots, &(path == &1 or String.starts_with?(path, &1 <> "/")))
    end)
    |> Enum.filter(&File.regular?/1)
    |> Enum.reject(&(Path.extname(&1) in [".png", ".gif", ".mp4", ".so", ".cast"]))
    |> Enum.reject(&(&1 == "test/weft/packing_test.exs"))
  end

  test "nothing promises a ring that crosses a machine" do
    offences =
      for path <- files(),
          {:ok, body} = File.read(path),
          phrase <- @contradictions,
          String.contains?(String.downcase(body), phrase),
          do: {path, phrase}

    assert offences == [],
           "iceoryx does not cross a machine:\n" <>
             Enum.map_join(offences, "\n", fn {path, phrase} ->
               "  #{path}: #{inspect(phrase)}"
             end)
  end

  test "the packing rule is stated where the architecture is" do
    # Both copies exist on purpose: CLAUDE.md is the rule a contributor reads, and the Weft
    # moduledoc is the architecture a reader reads. If one is edited alone this fails, which
    # is the drift the vocabulary test was written for.
    for path <- ["CLAUDE.md", "lib/weft.ex"] do
      body = File.read!(path)

      assert String.contains?(body, "A ring forces co-location"),
             "#{path} does not state that a ring forces co-location"

      assert String.contains?(body, "packing of planes and edges"),
             "#{path} does not say a machine is a packing of planes and edges"
    end
  end
end
