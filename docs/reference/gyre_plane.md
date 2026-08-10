# The Gyre plane and its edge

Goal: host The Gyre in weft, and let a browser play it.

State: two subtrees are here. Neither one holds the game.

- **Here.** `native/gyreplane` is a subtree of `zone-server-h2o`. It is the zone server:
  QUIC and WebTransport transport, an FDB zone tick, and a libriscv guest sandbox.
- **Here.** `native/gyreedge` is a subtree of `zone-guest-gyre`. It is the browser client,
  three.js and SlugHorn, and its deployment.
- **Specified.** `../spec/Gyre.lean` proves the properties of the room graph and the
  objective that any port must keep.
- **Missing.** The domain itself. Read the section below.
- **Not built.** The iceoryx link. `Weft` says it plainly: there is no iceoryx code at all.

The domain does not run in the BEAM, and it must not. The BEAM runs the control plane
only, and every heavy plane is a native process outside it.

## Which side is which

The plane is the server, and the edge is what faces the browser. This is the split the two
directories record.

`zone-server-h2o` holds authority. It runs one zone for each process, it ticks against
FoundationDB, and it runs guest code under libriscv. That is plane work.

`zone-guest-gyre` holds the client and the transport that serves it. The edge terminates
that transport. So the client repository sits on the edge side, because the edge exists to
serve it.

## The domain is in neither subtree

`../spec/Gyre.lean` specifies two rooms, `decanting_floor` and `splicers_den`. That logic
is not in `native/gyreplane` and it is not in `native/gyreedge`.

`zone-server-h2o` deleted it. The commit is `f96d5b2`, "Remove the MUD subsystem (phase 2
of the CDN-guest move)". It removed `mud/`, `src/mud/`, and the QCBOR, libriscv, and
SlugHorn vendored copies that only the MUD used.

The logic moved to a third repository, `zone-guest-middleham`. The client repository kept
the design record, `../../native/gyreedge/docs/0003-the-gyre-mud-domain-and-mode-selector.md`,
and the renderer. It did not keep the rules.

So the earlier plan step, "move the domain behind the harness", has nothing to move yet.
Either `zone-guest-middleham` becomes a third subtree, or the domain is written against
`../spec/Gyre.lean`, which is why that file exists.

## What the subtrees changed about the blockers

An earlier version of this page said picoquic and picotls were absent. That was true of
weft and it is no longer true.

`native/gyreplane/thirdparty` vendors picoquic, picotls, and libriscv, and
`native/gyreplane/src/transport` already terminates QUIC and negotiates WebTransport. The
transport work is not a green field. It exists, and it needs to move.

iceoryx is still absent. It is the one blocker that did not move.

## The rule the server breaks

A plane has no networking. `zone-server-h2o` opens a UDP socket and terminates QUIC in the
same process that holds authority.

That is not a plane under the weft definition, and it is not an edge either, because an
edge holds no authority. It is both at once, which is the shape weft splits.

So the work is a split and not a port. The transport in `src/transport` becomes the edge.
The zone tick in `src/zf_zonetick.c` stays the plane. What runs between them today is a
function call, and it becomes iceoryx.

That split is the cost of the rule. One process is simpler and it has one less hop.

## What the edge needs

An edge is a plane with networking. It terminates a transport and gives the decoded result
to a plane over iceoryx. It holds no authority, it runs no simulation, and it keeps no
durable state.

The client transport is HTTP/3 and WebTransport, and never HTTP/1.1. Firefox speaks both.

- **picoquic**, for QUIC and HTTP/3. It is vendored in the plane subtree, and it has to
  build from the edge instead.
- **iceoryx v1**, to reach the plane. It needs the RouDi daemon beside it.

The server has no TLS certificate wired. Its own README says the certificate and the key
are `NULL`. A browser will not connect without one, so this blocks the Firefox proof.

## The binaries the subtrees brought

weft does not commit a binary to git, and it uploads one as a CI artifact instead. Four
files now break that rule.

- `native/gyreedge/web/slughorn.wasm`, 188 kB, and a vendored bundle at 672 kB. Upstream
  commits them on purpose. Its web server serves that directory from the docroot, and its
  image carries no Emscripten toolchain.
- `native/gyreplane/thirdparty/picoquic` carries two documentation images and two test
  fixtures. They belong to picoquic and not to weft.

The reason upstream gives belongs to the deployment upstream has, and not to the one weft
has. This is a conflict to settle. Either the build gains the toolchain and the artifacts
leave git, or weft records the two directories as exceptions.

`native/gyreplane` adds 22 MB to a checkout. Almost all of it is `thirdparty`.

## The order to build it

1. Put iceoryx v1 and RouDi in the container image, and prove a publisher and a subscriber
   pass a message. Nothing else can start before this.
2. Build the thread-per-core harness once, because every plane uses it.
3. Split `zone-server-h2o`. The transport goes to the edge, the zone tick stays the plane,
   and iceoryx replaces the call between them.
4. Find the domain. Subtree `zone-guest-middleham`, or write it against `../spec/Gyre.lean`.
5. Wire a TLS certificate, and prove the result with Firefox. A command line client proves
   the transport and not the product, because the client is a browser.

## What blocks what

Step 1 blocks every other step. A plane that cannot talk to the control plane is a
program, and not a plane.

Step 4 does not block step 3. The split is about where the transport runs, and it does not
need the rules of the game.
