defmodule Weft.Application do
  @moduledoc false

  use Application

  @impl true
  def start(_type, _args) do
    children = [
      # Actor addressing: one process per {name, key}, cluster-wide. Horde's
      # distributed registry is the single-writer invariant across a cluster,
      # replacing pegboard's exclusivity + lost-timeout/ping fencing. `members:
      # :auto` makes every connected node a member automatically.
      {Horde.Registry, name: Weft.Registry, keys: :unique, members: :auto},
      # Actor lifecycle, distributed: actors spread across the cluster and are
      # handed off to survivors when a node leaves. This is pegboard failover.
      {Horde.DynamicSupervisor,
       name: Weft.ActorSupervisor,
       strategy: :one_for_one,
       members: :auto,
       process_redistribution: :active},
      # Runner lifecycle: serverless runners started and drained by pool reconcilers.
      {DynamicSupervisor, name: Weft.RunnerSupervisor, strategy: :one_for_one},
      # Replicates actor writes to FoundationDB off the write path. No-op until a
      # cluster is configured, so the store runs local-only without it.
      Weft.Actor.Store.Replicator
    ]

    opts = [strategy: :one_for_one, name: Weft.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
