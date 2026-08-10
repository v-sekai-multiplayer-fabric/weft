defmodule Weft.VocabularyTest do
  use ExUnit.Case, async: true

  @moduledoc """
  One name for one concept, enforced.

  A decision that is written in two places drifts, and the copy that goes stale still
  reads as authoritative. That happened four times: `Seastar` after the harness stopped
  being Seastar, `iceoryx2` after v1 was picked, a company name after it was removed,
  and old document paths after the split. Each was found by hand.

  This test finds them instead. A term below is retired, and it may appear only in the
  file that records why it was retired.
  """

  @retired [
    {"Seastar", ["docs/essays/runtime-choice.md", "lib/weft.ex", "CLAUDE.md"],
     "The harness is a thin C++ layer over iceoryx v1. See lib/weft.ex."},
    {"iceoryx2", ["docs/essays/runtime-choice.md", "CLAUDE.md"],
     "weft uses iceoryx v1, which needs the RouDi daemon. See CLAUDE.md."},
    {"VRChat", [], "Do not name another company or product. See CLAUDE.md."},
    {"docs/planes.md", [], "It is the Weft moduledoc in lib/weft.ex."},
    {"docs/reference/architecture.md", [], "It is the Weft moduledoc in lib/weft.ex."},
    {"docs/reference/store.md", [], "It is the Weft.Actor.Store moduledoc."},
    {"docs/reference/data-plane.md", [], "It is the Weft.DataPlane moduledoc."},
    {"docs/reference/protocol.md", [], "It is the Weft.Gateway moduledoc."},
    {"docs/latency.md", [], "It is docs/essays/latency.md."},
    {"docs/benchmarks.md", [], "It is docs/essays/benchmarks.md."},
    {"docs/topology.md", [], "It is docs/essays/topology.md."}
  ]

  @roots ["lib", "docs", "deploy", "test", "native", ".github", "CLAUDE.md", "README.md"]

  defp files do
    @roots
    |> Enum.flat_map(fn root ->
      cond do
        File.regular?(root) -> [root]
        File.dir?(root) -> Path.wildcard(root <> "/**/*", match_dot: true)
        true -> []
      end
    end)
    |> Enum.filter(&File.regular?/1)
    |> Enum.reject(&(Path.extname(&1) in [".bin", ".png", ".gif", ".mp4", ".so", ".cast"]))
    # This file names every retired term, so it cannot check itself.
    |> Enum.reject(&(&1 == "test/weft/vocabulary_test.exs"))
  end

  test "a retired term appears only where its retirement is recorded" do
    all = files()

    offences =
      Enum.flat_map(@retired, fn {term, allowed, why} ->
        all
        |> Enum.reject(&(&1 in allowed))
        |> Enum.filter(fn path ->
          case File.read(path) do
            {:ok, body} -> String.contains?(body, term)
            _ -> false
          end
        end)
        |> Enum.map(&{term, &1, why})
      end)

    assert offences == [],
           "retired terms found:\n" <>
             Enum.map_join(offences, "\n", fn {term, path, why} ->
               "  #{path}: #{inspect(term)} — #{why}"
             end)
  end
end
