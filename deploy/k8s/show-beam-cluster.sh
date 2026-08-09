#!/usr/bin/env bash
# Show the BEAM cluster: node to node comms between weft Pods, on different machines.
#
#   deploy/k8s/show-beam-cluster.sh
#
# Each step uses `bin/weft rpc`, which the OTP release provides. It runs the expression
# on the node in that Pod. When the expression names a different node, the answer
# crosses the network, so the output is proof and not a claim.
set -euo pipefail

NS=weft

say() { printf '\n\033[1m== %s\033[0m\n' "$1"; }

# Run an expression on the weft node inside Pod $1.
rpc() {
  kubectl -n "$NS" exec "$1" -- bin/weft rpc "$2"
}

say "weft Pods, and the machine each one runs on"
kubectl -n "$NS" get pods -l app=weft \
  -o custom-columns=POD:.metadata.name,IP:.status.podIP,NODE:.spec.nodeName

mapfile -t PODS < <(kubectl -n "$NS" get pods -l app=weft -o jsonpath='{.items[*].metadata.name}' | tr ' ' '\n')
mapfile -t IPS  < <(kubectl -n "$NS" get pods -l app=weft -o jsonpath='{.items[*].status.podIP}'  | tr ' ' '\n')

if [ "${#PODS[@]}" -lt 2 ]; then
  echo "Need two or more weft Pods. Found ${#PODS[@]}."
  exit 1
fi

say "Each node lists its peers (libcluster found them through the headless Service)"
for i in "${!PODS[@]}"; do
  printf '%s -> ' "${PODS[$i]}"
  rpc "${PODS[$i]}" 'Node.list() |> Enum.map(&to_string/1) |> Enum.join(", ") |> IO.puts()'
done

say "Pod 0 runs code on Pod 1, over BEAM distribution"
rpc "${PODS[0]}" ":rpc.call(:\"weft@${IPS[1]}\", :erlang, :node, []) |> IO.inspect(label: \"answered by\")"

say "Create an actor from Pod 0, then read it from Pod 1"
KEY="k8s-demo-$RANDOM"
rpc "${PODS[0]}" "
  {:ok, pid} = Weft.Actors.get_or_create(\"zone\", \"$KEY\")
  :ok = Weft.Actor.put(pid, :owner, to_string(node(pid)))
  IO.puts(\"created on #{node()}, the process runs on #{node(pid)}\")
"
rpc "${PODS[1]}" "
  {:ok, pid} = Weft.Actors.get_or_create(\"zone\", \"$KEY\")
  owner = Weft.Actor.get(pid, :owner)
  IO.puts(\"read from #{node()}, the actor runs on #{node(pid)}, it recorded #{owner}\")
"

say "What this shows"
cat <<'TXT'
The actor exists one time in the cluster. Horde put it in the registry of every node,
so the second Pod found it without a copy of the data. The second Pod read it through
the process on the first Pod, over BEAM distribution, between two machines.

This is the BEAM to BEAM boundary from docs/planes.md. It carries control only:
placement, lifecycle, and handoff. It never carries a game packet.

To watch a handoff, delete the Pod that holds the actor and read the key again. Horde
starts the actor on a survivor, and the store rebuilds its state from FoundationDB.
TXT
