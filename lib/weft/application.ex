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
  # `WEFT_CLUSTER_QUERY` holds a DNS name that resolves to every node of the cluster.
  # libcluster polls it and connects to each address. On Fly that name is
  # `<app>.internal`, which the private network serves. On a plain virtual machine it is
  # any name with one record for each node.
  #
  # Discovery starts only when the variable is set. A local run and a test run stay on
  # one node. This keeps one code path: the cluster is always Horde, and only the
  # discovery is configured.
  #
  # A world runs on one machine, so a world needs no cluster at all. This is for the
  # front door, which holds no world state and runs on more than one machine. See
  # `docs/topology.md`.
  defp cluster_children do
    case System.get_env("WEFT_CLUSTER_QUERY") do
      nil ->
        []

      query ->
        topologies = [
          weft: [
            strategy: Cluster.Strategy.DNSPoll,
            config: [
              query: query,
              node_basename: System.get_env("WEFT_NODE_BASENAME", "weft"),
              polling_interval: 5_000
            ]
          ]
        ]

        [{Cluster.Supervisor, [topologies, [name: Weft.ClusterSupervisor]]}]
    end
  end
end
