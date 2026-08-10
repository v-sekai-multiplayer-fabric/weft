defmodule Weft.VocabularyTest do
  use ExUnit.Case, async: true

  @moduledoc """
  One name for one concept, enforced.

  A decision that is written in two places drifts, and the copy that goes stale still
  reads as authoritative. That happened four times: `Seastar` after the harness stopped
  being Seastar, `iceoryx2` after v1 was picked, a company name after it was removed,
  and old document paths after the split. Each was found by hand.

  One entry below reversed. `iceoryx2` was retired and is now the choice, and `RouDi` and
  `iceoryx v1` are retired in its place. A retirement is not permanent, and this test does
  not claim it is. It claims one name for one concept today.

  This test finds them instead. A term below is retired, and it may appear only in the
  file that records why it was retired.
  """

  @retired [
    {"Seastar", ["docs/essays/runtime-choice.md", "lib/weft.ex", "CLAUDE.md"],
     "The harness is a thin C++ layer over iceoryx v1. See lib/weft.ex."},
    {"RouDi", ["docs/essays/runtime-choice.md"],
     "iceoryx2 is brokerless. No daemon runs beside a plane. See CLAUDE.md."},
    {"iceoryx v1",
     ["docs/essays/runtime-choice.md", "CLAUDE.md", "deploy/Containerfile",
      "test/weft/vocabulary_test.exs"],
     "weft uses iceoryx2. v1 does not build here, because it needs libacl. See CLAUDE.md."},
    {"VRChat", [], "Do not name another company or product. See CLAUDE.md."},
    {"docs/planes.md", [], "It is the Weft moduledoc in lib/weft.ex."},
    {"docs/reference/architecture.md", [], "It is the Weft moduledoc in lib/weft.ex."},
    {"docs/reference/store.md", [], "It is the Weft.Actor.Store moduledoc."},
    {"docs/reference/data-plane.md", [], "It is the Weft.DataPlane moduledoc."},
    {"docs/reference/protocol.md", [], "It is the Weft.Gateway moduledoc."},
    {"docs/latency.md", [], "It is docs/essays/latency.md."},
    {"docs/reference/", [], "A rule lives with its code. See CLAUDE.md."},
    {"docs/benchmarks.md", [], "It is docs/essays/benchmarks.md."},
    {"docs/topology.md", [], "It is docs/essays/topology.md."}
  ]

  @roots ["lib", "docs", "deploy", "test", "native", ".github", "CLAUDE.md", "README.md"]

  # git lists the files, and not `Path.wildcard/2`. Two reasons, and the first is
  # correctness. A CMake build directory holds object files and binaries that carry the
  # source text inside them, so a retired term appears there long after it left the
  # source. git already knows those are ignored.
  #
  # The second is that this list has no extension filter to keep growing. `--cached` is
  # what is tracked and `--others --exclude-standard` is what is new and not ignored, so a
  # file added but not yet committed is still checked.
  defp files do
    {out, 0} = System.cmd("git", ["ls-files", "--cached", "--others", "--exclude-standard"])

    out
    |> String.split("\n", trim: true)
    |> Enum.filter(fn path ->
      Enum.any?(@roots, &(path == &1 or String.starts_with?(path, &1 <> "/")))
    end)
    |> Enum.filter(&File.regular?/1)
    |> Enum.reject(&(Path.extname(&1) in [".png", ".gif", ".mp4", ".so", ".cast"]))
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
