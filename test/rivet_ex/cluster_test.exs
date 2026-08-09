defmodule RivetEx.ClusterTest do
  @moduledoc """
  Multi-node proof: one actor instance per id across a cluster, and handoff when a
  node dies. This is the guarantee pegboard spends `pegboard-envoy` plus
  lost-timeout/ping fencing to provide; here it is `Horde.Registry` +
  `Horde.DynamicSupervisor`.

  Nodes are spawned with `:peer` and share this machine's filesystem, so the
  per-actor SQLite files are reachable from any node. On a real multi-machine
  cluster the store would need a shared/distributed backend (the reason rivet uses
  FoundationDB); see the store roadmap.
  """

  use ExUnit.Case, async: false

  @moduletag :distributed
  @moduletag timeout: 60_000

  alias RivetEx.Actors

  setup do
    data_dir = Application.get_env(:rivet_ex, :data_dir)
    peers = for i <- 1..2, do: start_peer("n#{i}", data_dir)
    peer_nodes = Enum.map(peers, fn {_pid, node} -> node end)
    all_nodes = [node() | peer_nodes]

    wait_for_horde_members(all_nodes)

    on_exit(fn -> Enum.each(peers, fn {pid, _node} -> safe_stop(pid) end) end)

    {:ok, peers: peers, peer_nodes: peer_nodes, all_nodes: all_nodes}
  end

  test "one instance per id, addressable from every node", %{peer_nodes: [n1, n2]} do
    key = "clustered-#{System.unique_integer([:positive])}"

    # Create from one node; resolve from another and from the primary. All three
    # must be the exact same process: a single writer cluster-wide.
    {:ok, pid_from_n1} = :erpc.call(n1, Actors, :get_or_create, ["zone", key])
    {:ok, pid_from_n2} = :erpc.call(n2, Actors, :get_or_create, ["zone", key])
    {:ok, pid_from_primary} = Actors.get_or_create("zone", key)

    assert pid_from_n1 == pid_from_n2
    assert pid_from_n1 == pid_from_primary
  end

  test "actor state hands off when its host node dies", %{peers: peers, peer_nodes: peer_nodes} do
    # Use a key whose actor Horde places on a peer (not the primary), so killing
    # its host does not kill the test node.
    {key, pid} = actor_on_a_peer(peer_nodes)
    host = node(pid)

    :ok = :erpc.call(host, RivetEx.Actor, :put, [pid, :hp, 99])

    # Kill the node hosting the actor.
    {host_pid, ^host} = Enum.find(peers, fn {_pid, n} -> n == host end)
    safe_stop(host_pid)

    # A survivor re-hosts the actor (Horde failover) and restores its durable
    # state from the shared store.
    survivor = Enum.find(peer_nodes, &(&1 != host))

    {:ok, restored_pid} =
      wait_until(fn ->
        case :erpc.call(survivor, Actors, :get_or_create, ["zone", key]) do
          {:ok, p} = ok when node(p) != host -> ok
          _ -> :retry
        end
      end)

    assert node(restored_pid) != host
    assert :erpc.call(node(restored_pid), RivetEx.Actor, :get, [restored_pid, :hp]) == 99
  end

  # Find a {key, pid} whose actor Horde placed on a peer node.
  defp actor_on_a_peer(peer_nodes, tries \\ 20) do
    key = "failover-#{System.unique_integer([:positive])}"
    {:ok, pid} = Actors.get_or_create("zone", key)

    cond do
      node(pid) in peer_nodes -> {key, pid}
      tries > 0 -> actor_on_a_peer(peer_nodes, tries - 1)
      true -> flunk("could not place an actor on a peer node")
    end
  end

  defp start_peer(name, data_dir) do
    {:ok, pid, node} =
      :peer.start_link(%{
        name: String.to_atom(name),
        host: ~c"127.0.0.1",
        longnames: true,
        args: [~c"-setcookie", ~c"rivet_ex_test"]
      })

    # Give the peer this node's code paths and the shared data dir, then boot the app.
    :ok = :erpc.call(node, :code, :add_pathsz, [:code.get_path()])
    _ = :erpc.call(node, Application, :put_env, [:rivet_ex, :data_dir, data_dir])
    {:ok, _} = :erpc.call(node, Application, :ensure_all_started, [:rivet_ex])

    {pid, node}
  end

  defp wait_for_horde_members(nodes) do
    wait_until(fn ->
      converged? =
        Enum.all?(nodes, fn n ->
          members = :erpc.call(n, Horde.Cluster, :members, [RivetEx.ActorSupervisor])
          length(members) == length(nodes)
        end)

      if converged?, do: :ok, else: :retry
    end)
  end

  defp wait_until(fun, deadline \\ nil) do
    deadline = deadline || System.monotonic_time(:millisecond) + 15_000

    case fun.() do
      :retry ->
        if System.monotonic_time(:millisecond) > deadline do
          flunk("condition not reached before deadline")
        else
          Process.sleep(50)
          wait_until(fun, deadline)
        end

      other ->
        other
    end
  end

  defp safe_stop(peer_pid) do
    :peer.stop(peer_pid)
  catch
    _, _ -> :ok
  end
end
