defmodule RivetEx.MixProject do
  use Mix.Project

  def project do
    [
      app: :rivet_ex,
      version: "0.1.0",
      elixir: "~> 1.20",
      start_permanent: Mix.env() == :prod,
      deps: deps()
    ]
  end

  # Run "mix help compile.app" to learn about applications.
  def application do
    [
      extra_applications: [:logger],
      mod: {RivetEx.Application, []}
    ]
  end

  # Run "mix help deps" to learn about dependencies.
  defp deps do
    [
      {:exqlite, "~> 0.27"},
      {:stream_data, "~> 1.1", only: :test}
    ]
  end
end
