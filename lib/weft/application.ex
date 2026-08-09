defmodule Weft.Application do
  @moduledoc false

  use Application

  @impl true
  def start(_type, _args) do
    children = cluster_children() ++ [
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

  # Node discovery for the BEAM cluster. Horde uses `members: :auto`, so it makes every
  # connected node a member. Nothing connected the nodes, so the cluster was one node.
  #
  # libcluster's Kubernetes.DNS strategy reads the A records of a headless Service and
  # connects to each address. The Service must set `publishNotReadyAddresses: true`, so
  # a node joins the cluster before it reports ready.
  #
  # Discovery starts only when `WEFT_K8S_SERVICE` is set. A local run and a test run
  # have no Kubernetes, so they get no strategy and stay on one node. This keeps one
  # code path: the cluster is always Horde, and only the discovery is configured.
  defp cluster_children do
    case System.get_env("WEFT_K8S_SERVICE") do
      nil ->
        []

      service ->
        topologies = [
          weft: [
            strategy: Cluster.Strategy.Kubernetes.DNS,
            config: [
              service: service,
              application_name: System.get_env("WEFT_K8S_APP_NAME", "weft"),
              polling_interval: 5_000
            ]
          ]
        ]

        [{Cluster.Supervisor, [topologies, [name: Weft.ClusterSupervisor]]}]
    end
  end
end
