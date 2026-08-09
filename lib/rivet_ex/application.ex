defmodule RivetEx.Application do
  @moduledoc false

  use Application

  @impl true
  def start(_type, _args) do
    children = [
      # Actor addressing: one process per {name, key}. This is the single-writer
      # invariant (KV + SQLite) enforced by the runtime, replacing pegboard's
      # distributed exclusivity protocol.
      {Registry, keys: :unique, name: RivetEx.Registry},
      # Actor lifecycle: the pegboard actor supervisor.
      {DynamicSupervisor, name: RivetEx.ActorSupervisor, strategy: :one_for_one},
      # Runner lifecycle: serverless runners started and drained by pool reconcilers.
      {DynamicSupervisor, name: RivetEx.RunnerSupervisor, strategy: :one_for_one}
    ]

    opts = [strategy: :one_for_one, name: RivetEx.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
