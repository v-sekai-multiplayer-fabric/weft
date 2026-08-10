# The H3 edge

Goal: terminate the browser transport, and give the decoded result to a plane over
iceoryx.

State: not started. This directory holds the contract and no code. Read
`../../docs/reference/gyre_plane.md` for the order the work has to happen in.

## What an edge is

An edge is a plane with networking. That is a definition and not a default, so there is no
exception to check.

An edge obeys every plane rule and adds one capability, the network. It terminates a
transport and gives the decoded result to a plane over iceoryx. It holds no authority, it
runs no simulation, and it keeps no durable state.

So this process may open a socket. It may not decide anything about the game.

## Why it is not a subtree of the client repo

`../gyreplane` is a subtree of `zone-guest-gyre`, which holds the client and its Fly
deployment. That deployment serves the client with h2o and talks to `zone-server-h2o`.

None of that is an edge. h2o is a web server, and the weft rule is HTTP/3 and WebTransport,
never HTTP/1.1. Copying the same subtree here would put 900 kB in the repository twice and
give the edge nothing to build on.

The edge is new code. What it borrows from the client repo is the shape of the request,
which is a command in and narration out.

## What it needs

- **picoquic**, for QUIC and HTTP/3. It needs picotls, and picotls needs a TLS library.
  Neither is in the container image yet.
- **iceoryx v1**, to reach the plane. It needs the RouDi daemon beside it.

Neither is present. The edge cannot compile until both are in the image, which is why this
directory holds no `CMakeLists.txt`. A build file that cannot build is worse than none,
because CI then reports a failure that says nothing about the code.

## The proof it works

Firefox. The client is a browser, so a command line client proves the transport and not
the product. Firefox speaks HTTP/3 and WebTransport, and `../gyreplane/web/test` already
drives the client with Playwright and Camoufox.
