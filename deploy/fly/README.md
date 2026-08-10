# Run weft on Fly machines

A machine is the unit. `docs/planes.md` says iceoryx does not cross machines. So every
plane and every edge of one world runs on one machine. They share memory through
`/dev/shm`. A world does not cross a machine, and one machine holds many worlds. See
`docs/topology.md`.

There is no Kubernetes here, and there is no orchestrator to learn. A machine runs the
OTP release, the planes, and the edges. The `.deb` and the `.rpm` do the same with
systemd. See `deploy/packaging/`.

## Why not Kubernetes

Kubernetes gave nothing this deploy needs, and it took work to hold back:

- A plane must not have the network. Every container of a Pod shares one network
  namespace, and a NetworkPolicy applies to a Pod, so Kubernetes cannot enforce the rule.
  The bubblewrap sandbox does, on a machine as well as in a Pod.
- A thread-per-core plane needs pinned cores. That needs Guaranteed QoS and the `static`
  CPU manager policy of the kubelet. On a machine, the harness pins its own threads.
- One world is one machine, so there is nothing to schedule and nothing to spread.

The cost is also lower. See `docs/topology.md`.

## The image

Fly runs an OCI image, so it runs the `release` target of `deploy/Containerfile`. That is
the same OTP release the `.deb` and the `.rpm` hold, so each deploy carries one artifact.

```sh
fly deploy --config deploy/fly/fly.toml
```

## The BEAM cluster

A world needs no cluster. A world does not cross a machine, and an entity is
authoritative on exactly one zone. The front door is the part that runs on more than
one machine, and it holds no world state.

Two environment variables start discovery, and nothing starts it without them:

- `WEFT_CLUSTER_QUERY` is a DNS name with one record for each node. On Fly that is
  `<app>.internal`.
- `WEFT_NODE_BASENAME` is the name before the `@`. It is `weft` by default.

Two more name the node itself. A BEAM node is named when the VM starts, before any
application runs, so libcluster cannot do it:

- `RELEASE_DISTRIBUTION=name`
- `RELEASE_NODE=weft@$FLY_PRIVATE_IP`

**The Fly private network is IPv6.** BEAM distribution uses IPv4 by default and it will
not connect. `ERL_AFLAGS="-proto_dist inet6_tcp"` is in `fly.toml` for that reason.

`RELEASE_COOKIE` must be the same on every machine. Set it as a secret:

```sh
fly secrets set RELEASE_COOKIE="$(openssl rand -base64 32)" --config deploy/fly/fly.toml
```

## FoundationDB

FoundationDB is not here. It crosses machines, so it is not part of a world machine, and
Fly has no operator for it. Run it as its own machines with volumes, with `triple`
redundancy and an odd count of coordinators. `docs/topology.md` gives the
counts.

`WEFT_FDB_CLUSTER_FILE` points weft at that cluster.
