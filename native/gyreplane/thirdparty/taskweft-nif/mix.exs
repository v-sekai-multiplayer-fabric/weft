defmodule Taskweft.NIF.MixProject do
  use Mix.Project

  @version "0.2.0-dev.19"

  def project do
    [
      app: :taskweft_nif,
      version: @version,
      elixir: "~> 1.17",
      compilers: [:elixir_make] ++ Mix.compilers(),
      deps: deps(),
      dialyzer: [plt_add_apps: [:mix]],
      description: "C++20 HTN planner NIF (RECTGTN model) + HRR for Elixir",
      package: package(),
      source_url: "https://github.com/taskweft/nif"
    ] ++ make_options()
  end

  defp package do
    [
      files: ~w(lib c_src standalone Makefile Makefile.win mix.exs LICENSE*),
      licenses: ["MIT"],
      links: %{"GitHub" => "https://github.com/taskweft/nif"}
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [
      {:elixir_make, "~> 0.9"},
      {:jason, "~> 1.4"},
      {:dialyxir, "~> 1.4", only: [:dev], runtime: false},
      {:propcheck, "~> 1.4", only: [:test, :dev], runtime: false},
      {:benchee, "~> 1.3", only: :dev, runtime: false},
      {:ex_doc, ">= 0.0.0", only: :dev, runtime: false}
    ]
  end

  defp make_options do
    case {:os.type(), System.get_env("VCINSTALLDIR")} do
      {{:win32, _}, vcdir} when is_binary(vcdir) and vcdir != "" ->
        [make_executable: "nmake", make_args: ["/F", "Makefile.win"]]

      {{:win32, _}, _} ->
        [make_executable: "mingw32-make"]

      _ ->
        []
    end
  end
end
