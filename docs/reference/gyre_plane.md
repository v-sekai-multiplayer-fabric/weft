# The Gyre plane

Goal: host The Gyre in weft, and let a browser play it.

State: the zone server is here. The game is not.

- **Here.** `native/gyreplane` is a fork of `zone-server-h2o`. It is an FDB zone tick and
  nothing else. It has no networking and no vendored dependency.
- **Specified.** `../spec/Gyre.lean` proves the properties of the room graph and the
  objective that any port must keep.
- **Missing.** The domain itself. Read the section below.
- **Proved, not wired.** The bus. `native/harness` passes a message between two processes
  over iceoryx2, with no daemon. No plane uses it. See `harness.md`.
- **Elsewhere.** The edges. `native/edge` is generic now, and it serves every client and
  not only this one. Read `../../native/edge/README.md`.

The domain does not run in the BEAM, and it must not. The BEAM runs the control plane
only, and every heavy plane is a native process outside it.

## Which side is which

`zone-server-h2o` holds authority. It runs one zone for each process, and it ticks against
FoundationDB. That is plane work.

It also terminated QUIC, which is not. That split is done, and the transport is in
`native/edge` now.

The browser client is neither. A client is not an edge, and it is not a plane. It lives in
`zone-guest-gyre`, and weft does not hold a copy.

## The domain is in neither repository

`../spec/Gyre.lean` specifies two rooms, `decanting_floor` and `splicers_den`. That logic
is not in `native/gyreplane` and it is not in `native/edge`.

`zone-server-h2o` deleted it. The commit is `f96d5b2`, "Remove the MUD subsystem (phase 2
of the CDN-guest move)". It removed `mud/`, `src/mud/`, and the QCBOR, libriscv, and
SlugHorn vendored copies that only the MUD used.

The logic moved to a third repository, `zone-guest-middleham`. The client repository kept
the renderer and the design record, `docs/0003-the-gyre-mud-domain-and-mode-selector.md`.
It did not keep the rules.

So the earlier plan step, "move the domain behind the harness", has nothing to move yet.
Either `zone-guest-middleham` comes in as a third directory, or the domain is written against
`../spec/Gyre.lean`, which is why that file exists.

## What the fork changed about the blockers

An earlier version of this page said picoquic and picotls were absent from weft. They are
here. `zone-server-h2o` vendors both, and its `src/transport` already terminates QUIC and
negotiates WebTransport. The transport work is not a green field.

It ran in the process that holds authority, which a plane may not do. So the code moved
rather than being written again.

- `src/transport` is now `native/edge/transport`.
- `thirdparty/picoquic`, `thirdparty/picotls`, and the Godot patches are now under
  `native/edge/thirdparty`.
- `cmake/picoquic.cmake` is now `native/edge/cmake/picoquic.cmake`.
- `src/main.c` lost the listener, the `-p` port flag, and the `-t` and `-k` TLS flags.
- `scripts/generate-tls-cert.sh` moved. A plane needs no certificate.
- The plane build links no picoquic and no picotls. Its `CMakeLists.txt` says so.
- The Fil-C workflow justified memory safety by the untrusted client input this process
  parsed. It parses none now. The reason is the untrusted guest under libriscv instead.

`native/edge/TRANSPORT.md` holds the full list and the two deployment facts that moved
with it.

The plane fell from 22 MB to 5.0 MB, and then to 280 kB. The edge rose to 18 MB.

The bus is no longer absent. `native/harness` proves it, and the thread-per-core loop that
sits on it is the part still missing. See `harness.md`.

## What is left of the plane

The plane is 280 kB. It was 22 MB when it arrived.

The transport left first. Then two more things left, because they had no caller once the
transport was gone.

**The h2o request half.** `src/worker_pool.c` dispatched `h2o_req_t` over an
`h2o_multithread` queue, and nothing called it. `src/utility.c` generated JSON response
bodies with yajl, and nothing called it either. `src/thread.c` held a second static
`fdb_global_t` that `src/main.c` already warned against. `src/event_loop.h` and
`src/database.h` had no reader at all, and `src/database.h` was empty.

`src/main.c` also lost `h2o_config_init`, `h2o_context_init`, and the dummy `default` host
that existed only to satisfy an h2o assertion.

**The guest sandbox.** `src/sandbox` and `thirdparty/libriscv`. A plane runs one runtime
model, which is the thread-per-core harness. A second sandbox inside it is not that model.
Removing it made the build C only again.

Two vendored copies then had no caller at all. `thirdparty/QCBOR` lost its last one when
upstream removed `src/mud/mud_cbor.c`. `thirdparty/taskweft-nif` never had one. So
`thirdparty` and `cmake` are both empty and gone.

The credits stay. `CITATION.cff` keeps the libriscv, QCBOR, and Bubblewrap entries, and
each one now says the code was removed. That follows the rule the CrucibleBench entry
already set: credit for work that shaped a project does not expire when the derived file
is deleted.

## What h2o is still for

One thing. `h2o_evloop_create` and `h2o_evloop_run`, so that `fdb_future_set_callback` has
a loop to fire on and `h2o_timer_t` has one to time out against.

That is the job the thread-per-core harness over iceoryx takes. When it lands, h2o leaves
the build, and the plane links FoundationDB and iceoryx and nothing else.

## The ring that iceoryx already is

`src/spsc_ring.c` is gone too. An earlier version of this page kept it, and the reason it
gave was wrong.

It queues `void *`. A process-local pointer cannot cross a shared memory segment, because
that segment maps at a different address in each process. iceoryx solves exactly that
problem, with a relative pointer, and it carries its own lock-free queue underneath the
subscriber.

The other half of the old reason was worse. It said the harness needs a ring between the
thread that receives from iceoryx and the thread that ticks. Thread per core means those
are the same thread. A handoff between them is the thing the model exists to remove.

`test/cbmc/spsc_harness.c` and `test/verification/` went with it. They proved that ring. A
proof of code that is not here proves nothing about what runs.

## The second FoundationDB client

`src/fdb_database.c` links `libfdb_c` and drives it with `fdb_future_set_callback` on the
event loop. `native/storeplane/fdb_vfs.c` links `libfdb_c` too, and drives it with
`fdb_future_block_until_ready`.

Both call `fdb_select_api_version`, `fdb_setup_network`, `fdb_run_network`,
`fdb_create_database`, `fdb_database_create_transaction`, and `fdb_transaction_on_error`.
That is the client bootstrap and the retry loop, written twice, in two styles.

`Weft` states the rule. The control plane holds `erlfdb`, the store plane holds
`libfdb_c`, and no other plane links a client. A plane that needs durable state asks the
store plane over iceoryx.

So `src/fdb_database.c` and `src/zf_kv.c` are temporary. The zone tick moves onto SQLite in
the store plane, which gives it a database rather than a key range.

It cannot move yet. The store plane has no thread-per-core loop either, which is the same
step 2 that blocks everything else here. The bus below it works. See `harness.md`.

## What the split costs

The plane and the edge were one process. Now they are two, and nothing joins them.

So the zone tick has no input at all. Before the split a QUIC datagram drove it directly.
That path is cut until iceoryx carries the decoded input across.

This is worse than it was, on purpose, and only until step 1 lands. One process was
simpler and it had one less hop. The rule buys an edge that holds no authority, so a
network attacker reaches a process that can decide nothing.

## What the edge needs

An edge is a plane with networking. It terminates a transport and gives the decoded result
to a plane over iceoryx. It holds no authority, it runs no simulation, and it keeps no
durable state.

The client transport is HTTP/3 and WebTransport, and never HTTP/1.1. Firefox speaks both.

- **picoquic**, for QUIC and HTTP/3. It is here. It has no `main` yet, because the plane
  used to call it.
- **iceoryx2**, to reach the plane. It is brokerless, so no daemon runs beside it.
- **A TLS certificate.** The server never had one wired, and its README said the
  certificate and the key were `NULL`. A browser will not connect without one, so this
  blocks the Firefox proof.

## The binaries the forks brought

weft does not commit a binary to git, and it uploads one as a CI artifact instead. Four
files now break that rule.

`native/edge/thirdparty/picoquic` carries two documentation images and two test fixtures.
They belong to picoquic and not to weft.

The other two are gone. `slughorn.wasm` at 188 kB and a vendored bundle at 672 kB came in
with the browser client, and the client left with them. Upstream commits them on purpose,
and its reason is real: its web server serves that directory from the docroot, and its
image carries no Emscripten toolchain. That reason belonged to the deployment upstream has.
weft does not have it.

`native/edge` adds 17 MB to a checkout. Almost all of it is `thirdparty`.

## The order to build it

1. Done. `native/harness` proves a publisher and a subscriber pass a message over
   iceoryx2. Put iceoryx2 in the container image next, so CI runs that proof.
2. Build the thread-per-core harness once, because every plane uses it.
3. Give the edge an entry point, and join it to the plane over iceoryx. The transport is
   already here. What is missing is the `main` that used to live in the plane.
4. Find the domain. Subtree `zone-guest-middleham`, or write it against `../spec/Gyre.lean`.
   It has no sandbox to run in now, so decide where a guest runs before you bring it back.
   The browser client stays in `zone-guest-gyre`, because a client is not an edge.
5. Wire a TLS certificate, and prove the result with Firefox. A command line client proves
   the transport and not the product, because the client is a browser.

## What blocks what

Step 2 blocks every other step now. Step 1 is done. A plane that cannot talk to the
control plane is a program, and not a plane.

Step 4 does not block step 3. The split is about where the transport runs, and it does not
need the rules of the game.
