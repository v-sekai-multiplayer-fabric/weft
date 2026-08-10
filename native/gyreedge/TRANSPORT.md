# The transport

This directory holds the browser client. It now also holds the transport that serves the
client. This page records what moved here, and why.

## Why it moved

A plane has no networking. An edge is a plane with networking.

`zone-server-h2o` terminated QUIC in the same process that holds authority. That is not a
plane, and it is not an edge either, because an edge holds no authority. So the transport
left the plane.

The move is a move and not a rewrite. The bytes are the same.

## What moved

| here | from |
| --- | --- |
| `transport/webtransport_server.c`, `transport/wt_session.c` | `zone-server-h2o` `src/transport/` |
| `thirdparty/picoquic` | `zone-server-h2o` `thirdparty/picoquic` |
| `thirdparty/picotls` | `zone-server-h2o` `thirdparty/picotls` |
| `thirdparty/picoquic-godot-patches` | `zone-server-h2o` `thirdparty/` |
| `cmake/picoquic.cmake` | `zone-server-h2o` `cmake/` |
| `scripts/generate-tls-cert.sh` | `zone-server-h2o` `scripts/` |

## What did not move

The TLS material and the process that loaded it. `zone-server-h2o` `src/main.c` held the
cert resolution, the `-t` and `-k` flags, and the `TLS_CERT` and `TLS_KEY` fallback. The
plane does not need any of it, so it is gone from there.

That code is not lost. It is in the history of this repository, in the squashed subtree
commit for `native/gyreplane`. Recover it when the edge gains its own entry point.

There is no `CMakeLists.txt` here yet. The transport had no `main` of its own. It was
called from the plane, and the edge that will call it does not exist yet.

## The build dependency that came with it

The plane README used to say the build needs `mbedtls`, built from source and not from the
system package, because Apt's `libmbedtls-dev` does not carry `mbedtls_config.h`. That
requirement belonged to picotls, and not to the zone tick. So it is recorded here.

`ci-local` records a later switch from mbedtls to OpenSSL. Check which one picotls builds
against before you trust either note.

## Two facts the plane learned the hard way

Both were found by running the code, not by reading it. They apply to the edge now.

**picoquic takes file paths, and not PEM buffers.** `picoquic_create` wants a certificate
file and a key file. A deployment that keeps the PEM content in a secret must write it to
disk first.

**SELinux denies the certificate read.** On a host with `getenforce` set to `Enforcing`, a
bind mounted certificate directory needs `:Z` on the mount. Without it,
`ptls_load_certificates` fails with "Cannot load certificate", even when the file
permissions are correct and the container runs as root.

The plane had no certificate wired at all. Its own README stated that the certificate and
the key were `NULL`. So a browser could not connect, and that gap is now the edge's gap.

## Credit

picoquic, by Christian Huitema. <https://github.com/private-octopus/picoquic>

It terminates QUIC and WebTransport. The citation entry moved out of
`../gyreplane/CITATION.cff` with the code, because that plane no longer uses it.
