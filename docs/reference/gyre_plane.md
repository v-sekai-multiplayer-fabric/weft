# The Gyre plane and its edge

Goal: host The Gyre in weft, and let a browser play it. The Gyre is a MUD domain, and its
client is the web client in `zone-guest-gyre`, which is three.js and SlugHorn.

State: the source is here and nothing runs.

- **Here.** `native/gyreplane` is a subtree of `zone-guest-gyre`, which holds the client
  and its deployment.
- **Specified.** `../spec/Gyre.lean` proves the properties of the room graph and the
  objective that any port must keep.
- **Not built.** The plane. `Weft` says it plainly: there is no iceoryx code at all.
- **Not built.** The edge. `native/h3edge` holds its contract and no code.

The domain does not run in the BEAM, and it must not. The BEAM runs the control plane
only, and every heavy plane is a native process outside it.

## Why it is a plane, and not a zone

A zone simulates. The Gyre does not. It answers a command from one player at the rate a
person types, so it is not on the hot path and it is not packet rate work.

It is a plane because of where the code runs, and not because of how fast it runs. The
BEAM runs the control plane only. A domain that holds game logic is not control plane
work, whatever its rate.

The logic already exists in C++, in `zone-server-h2o`, and it runs under libriscv. weft
hosts it rather than ports it, so the logic that a differential test already covers stays
the logic that runs. `../spec/Gyre.lean` states what that logic must do, so a port has
something to fail against.

## The binaries the subtree brought

`native/gyreplane/web` holds `slughorn.wasm` at 188 kB and a vendored bundle at 672 kB.
weft does not commit a binary to git, and it uploads one as a CI artifact instead.

Upstream commits them on purpose, and it gives a reason: h2o serves that directory
straight from the docroot, and the image carries no Emscripten toolchain. Adding a
download of about 300 MB to regenerate two small files costs more than it buys there.

That reason belongs to the deployment upstream has, and not to the one weft has. So this
is a conflict to settle before the subtree is more than a copy. Either the build gains
Emscripten and the artifacts leave git, or weft records why this directory is an
exception.

## What the plane needs

The plane is a C++ process outside the BEAM. It has no networking, and it reaches the
control plane over Eclipse iceoryx v1.

Neither dependency is present. Both must be built and put in the container image before
any of this compiles.

- **Eclipse iceoryx v1.** It needs the RouDi daemon beside each plane, so the image gains
  a process as well as a library. `Weft` gives the two patterns a plane may use.
- **A thread-per-core harness.** One runtime model for every plane. `Weft` says which.

## What the edge needs

An edge is a plane with networking. It terminates the transport and gives the decoded
result to a plane over iceoryx. It holds no authority, runs no simulation, and keeps no
durable state.

The client transport is HTTP/3 and WebTransport, and never HTTP/1.1. Firefox speaks both.

- **picoquic**, for QUIC and HTTP/3. It needs picotls, which needs a TLS library.
- The edge maps a request to a command and a command to a reply. The web client posts a
  command and reads narration, so the shape is small.

## The order to build it

1. Put iceoryx v1 and RouDi in the container image, and prove a publisher and a
   subscriber pass a message. Nothing else can start before this.
2. Build the thread-per-core harness once, because every plane uses it.
3. Move `Weft.Gyre` behind the harness as the first plane, and keep the tests. The domain
   is already specified, so a port has something to fail against.
4. Build the edge on picoquic. Prove it with Firefox, and not with a command line client
   alone, because the client is a browser.

## What blocks what

Step 1 blocks every other step. A plane that cannot talk to the control plane is a
program, and not a plane.

The domain does not block anything. It is built, and it is where a browser will land.
