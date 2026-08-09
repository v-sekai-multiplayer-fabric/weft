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
      {:dialyxir, "~> 1.4", only: [:dev, :test], runtime: false}
    ]
  end

  # Standard OTP release. It bundles ERTS, so it is self-contained. Each runner builds
  # the release for its own operating system. No Burrito, no zig.
  defp releases do
    [
      weft: [
        include_executables_for: [:unix, :windows]
      ]
    ]
  end
end
