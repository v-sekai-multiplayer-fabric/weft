# weft-netbench — real-NIC packet-rate test on Fly

Measures the UDP receive ceiling loopback cannot, between two Fly machines over the
private 6PN network. Cost-bounded: smallest shared-CPU machines, run for seconds,
destroy immediately.

```sh
fly apps create weft-netbench
fly deploy --remote-only
fly scale count 2 --region sea          # two machines, same region
fly machine list                        # note the two 6PN IPs (fdaa:...)

# On machine A (server):
fly ssh console -s   # pick machine A
  netbench server 9999

# On machine B (client), aim at machine A's 6PN IP:
fly ssh console -s   # pick machine B
  netbench client fdaa:MACHINE_A_6PN 9999 4

# Read machine A's "recv X.XM pps" output, then tear everything down:
fly apps destroy weft-netbench -y
```

Estimated cost: two shared-cpu-1x machines for a few minutes ≈ a few US cents.
Always run `fly apps destroy` immediately after.
