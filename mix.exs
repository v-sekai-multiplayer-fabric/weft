defmodule Weft.MixProject do
  use Mix.Project

  def project do
    [
      app: :weft,
      version: "0.1.0",
      elixir: "~> 1.20",
      start_permanent: Mix.env() == :prod,
      compilers: [:elixir_make | Mix.compilers()],
      make_clean: ["clean"],
      # Binary release caching: CI builds the NIFs per target and attaches them to
      # a GitHub release; `mix compile` downloads the matching prebuilt artifact
      # (checksum-verified) and only falls back to building from source when none
      # is available. This is what keeps the heavy OpenUSD NIF from recompiling on
      # every machine.
      make_precompiler: {:nif, CCPrecompiler},
      make_precompiler_url:
        "https://github.com/v-sekai-multiplayer-fabric/weft/releases/download/@{tag}/@{artefact_filename}",
      make_precompiler_filename: "weft_dataplane_nif",
      make_precompiler_nif_versions: [versions: ["2.16", "2.17", "2.18"]],
      deps: deps(),
      releases: releases(),
      dialyzer: [
        plt_local_path: "priv/plts",
        plt_add_apps: [:mix, :ex_unit],
        flags: [:error_handling, :extra_return, :missing_return, :unmatched_returns]
      ]
    ]
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      extra_applications: [:logger],
      mod: {Weft.Application, []}
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      {:exqlite, "~> 0.27"},
      {:erlfdb, "~> 0.3"},
      {:horde, "~> 0.9"},
      {:telemetry, "~> 1.2"},
      {:elixir_make, "~> 0.8", runtime: false},
      {:cc_precompiler, "~> 0.1", runtime: false},
      {:stream_data, "~> 1.1", only: :test},
      {:benchee, "~> 1.3", only: :dev},
      {:dialyxir, "~> 1.4", only: [:dev, :test], runtime: false},
      # Package the BEAM release as a single self-contained executable for the dev
      # release (RFD 0067). Burrito uses zig at release time; plain `mix compile`
      # does not need it.
      {:burrito, "~> 1.3"}
    ]
  end

  # Burrito wraps the release into one executable per target, for the fpm RPM and
  # the desync chunk store. See the release workflow.
  defp releases do
    [
      weft: [
        steps: [:assemble, &Burrito.wrap/1],
        burrito: [targets: [linux: [os: :linux, cpu: :x86_64]]]
      ]
    ]
  end
end
