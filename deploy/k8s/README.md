# Run weft on Kubernetes

The Pod is the machine. `docs/planes.md` says iceoryx does not cross machines, so every
plane and every edge of one machine runs as a container in one Pod. They share memory
through one `/dev/shm` volume. FoundationDB is the only boundary that crosses nodes, so
it runs in its own Pods.

## What is here

| File                   | Holds                                                          |
| ---------------------- | -------------------------------------------------------------- |
| `namespace.yaml`       | the `weft` namespace                                           |
| `fdb-cluster.yaml`     | the FoundationDB cluster, as an operator resource              |
| `weft.yaml`            | the control plane Deployment and its headless Service          |
| `versitygw.yaml`       | the S3-compatible cold tier                                    |
| `kind-cluster.yaml`    | a four node local cluster, one control plane and three workers |
| `show-beam-cluster.sh` | proof that one weft node talks to another weft node            |

## Before you start

Install the FoundationDB operator. It owns the `FoundationDBCluster` resource that
`fdb-cluster.yaml` creates.

```sh
kubectl apply -f https://raw.githubusercontent.com/FoundationDB/fdb-kubernetes-operator/main/config/crd/bases/apps.foundationdb.org_foundationdbclusters.yaml
kubectl apply -f https://raw.githubusercontent.com/FoundationDB/fdb-kubernetes-operator/main/config/samples/deployment.yaml
```

Pin these to an operator release before you use this on a real cluster. Check the CRD
version that the operator you install serves, and make `fdb-cluster.yaml` agree.

## Start

```sh
kubectl apply -f deploy/k8s/namespace.yaml
kubectl apply -f deploy/k8s/fdb-cluster.yaml
kubectl -n weft wait --for=condition=Available foundationdbcluster/weft-fdb --timeout=10m
kubectl apply -f deploy/k8s/versitygw.yaml
kubectl apply -f deploy/k8s/weft.yaml
```

The operator writes the cluster file into a ConfigMap named `weft-fdb-config`.
`weft.yaml` mounts that ConfigMap and points `WEFT_FDB_CLUSTER_FILE` at it, so weft
reaches FoundationDB with no copy step.

## How the BEAM finds its peers

`weft-headless` is a headless Service with `publishNotReadyAddresses: true`. libcluster's
`Kubernetes.DNS` strategy reads its A records and connects to each address, so Horde sees
every node. Three environment variables drive it:

- `WEFT_K8S_SERVICE` is the headless Service name. Discovery starts only when it is set.
- `RELEASE_DISTRIBUTION=name` and `RELEASE_NODE=weft@<pod ip>` give each node a routable
  name. The Pod IP comes from the downward API.
- `RELEASE_COOKIE` must be the same on every node. Use a Secret. The example uses a
  literal value, which is not safe for a real cluster.
- `POD_IP` comes from the downward API. `RELEASE_NODE` is `weft@$(POD_IP)`, which
  Kubernetes expands, and `RELEASE_DISTRIBUTION` is `name`. An OTP release reads both.

libcluster does not name the node, and it cannot. A BEAM node gets its name when the VM
starts, which is before any application runs, so the name is already fixed when
libcluster starts. libcluster builds the name of each peer as
`<application_name>@<pod ip>` and connects to it. This node must use the same form, or
the names do not match and no node connects.

Kubernetes runs the `release` target, which is the OTP release. A release reads
`RELEASE_NODE` and `RELEASE_DISTRIBUTION` before it starts the VM, so the name is set in
time. This is also why Kubernetes does not run the `dev` image: `mix` ignores both
variables, and a node with no name cannot be reached.

```sh
docker build -f deploy/Containerfile --target release -t weft:release .
kind load docker-image weft:release --name weft
```

A `podAntiAffinity` rule puts one weft node on each machine. Two nodes on one machine
still cluster, but then the loss of a machine proves nothing about handoff.

## See the BEAM cluster work

```sh
deploy/k8s/show-beam-cluster.sh
```

Every line of its output comes from one Pod talking to a different Pod. It prints where
each node runs, the peers each node found, and then it creates an actor on one machine
and reads it from another. The read goes over BEAM distribution, so it proves the
BEAM-to-BEAM boundary rather than describing it. It uses `bin/weft rpc`, which the OTP
release provides.

That boundary carries control only: placement, lifecycle, and handoff. A game packet
never rides it. See `docs/data-plane.md`.

## Planes and edges are not here yet

The Deployment holds the control plane only. No plane and no edge is built yet, and
there is no iceoryx code yet, so there is nothing to add as a container. The Pod already
has the shape they need:

- `/dev/shm` is an `emptyDir` with `medium: Memory`, shared by every container in the
  Pod. A plane and an edge get the same mount, and that is the iceoryx segment.
- Containers in a Pod share the IPC namespace, so iceoryx discovery works between them.

Two things that this directory cannot do, and that a real deployment needs:

1. **Kubernetes cannot keep the network from a plane.** Every container in a Pod shares
   one network namespace, and a NetworkPolicy applies to a Pod, not to a container. So
   the rule "a plane has no networking" stays enforced inside the container, with the
   bubblewrap `--unshare-net` sandbox that `docs/planes.md` specifies. Do not expect the
   Pod boundary to do it.
2. **A thread-per-core plane needs pinned cores.** That needs Guaranteed QoS (equal
   integer CPU requests and limits) and the kubelet `static` CPU manager policy. Add
   `hugepages-2Mi` for the shared segment, and the `single-numa-node` Topology Manager
   policy to keep the cores and the memory together. A default cluster, such as the one
   in Rancher Desktop, does none of this, so it tests the wiring and not the latency.
