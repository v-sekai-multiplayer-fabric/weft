# The gateway edge

Terminates client control streams. Gives the result to the control plane.

State: not started. This directory holds the contract and no code.

## What it carries

Reliable, low-rate work. Login, chat, control, and asset pulls.

`Weft.Gateway` stays in the control plane, as the transport-agnostic routing core. This
edge gives it decoded requests. So the BEAM still touches no socket.

That split is the point. The routing rules are Elixir, where they are easy to change and
easy to supervise. The transport is C++, where it has to be fast and where a crash must not
reach the BEAM.

## The asset pull

An asset does not stream through the control plane. `Weft` gives the shape:

1. The client sends a hash to this edge, on a reliable stream.
2. The control plane reads the manifest for that hash from FoundationDB.
3. The chunks come back through the fetcher edge, not through here.

A FoundationDB value has a 100 kB limit and a transaction has a 10 MB limit. So an artifact
is cut into chunks before it is stored, and this edge never holds a whole artifact.

## What it must not do

It holds no authority, it runs no simulation, and it keeps no durable state. A session on
this edge decides nothing. It decodes a request and hands it to the control plane.

## What it needs first

1. **iceoryx v1 and RouDi**, to reach the control plane. Not in the container image.
2. **The thread-per-core harness.** Every plane uses it, so it is built once.
3. **A TLS certificate.** `../scripts/generate-tls-cert.sh` makes one for local use.
   `zone-server-h2o` never had one wired, so a browser has never connected.
4. **picoquic**, in `../transport`. That part is here already.

## The proof it works

Firefox. The client is a browser, so a command line client proves the transport and not the
product. Firefox speaks HTTP/3 and WebTransport.
