# The harness

Goal: one runtime model for every plane. A thin C++ thread-per-core loop over iceoryx2.

State: the library exists and the loop does not.

- **Built.** `weft::harness`, a library every plane and edge links. It holds the bus, the
  limits, and the payload type.
- **Proved.** `native/harness/proof` passes a message between two processes with no copy
  and no daemon. The run is below.
- **Not built.** The thread-per-core loop. No plane uses the library yet.

## One harness, not one for each plane

weft has several planes and two edges, and the number grows. Each one needs the bus and
each one needs the limits.

Left alone, each would grow its own copy of both, and the copies would drift the way a
decision written twice always drifts. That is the failure `Weft.VocabularyTest` was
written for, in a different form.

So there is one. `native/CMakeLists.txt` builds the harness first, and every plane below
it links `weft::harness`.

| what it gives | where | why it is shared |
| --- | --- | --- |
| the bus | `iceoryx2.sigs`, and the table generated from it | one C ABI, one dispatch table |
| the limits | `include/weft/limits.hpp` | every value is `Weft.Limits`, which is rivet's |
| the payload | `include/weft/snapshot.hpp` | both ends of a service must agree exactly |

`Weft.PlaneNetworkingTest` holds that shape. It fails if a second `.sigs` file appears, if
a plane declares a limit of its own, or if a directory with a `CMakeLists.txt` is missing
from the root build.

## Nothing links iceoryx2

`native/harness/iceoryx2.sigs` lists the 25 C ABI functions the harness calls. Chromium's
`generate_stubs.py`, vendored at `../../native/thirdparty/generate_stubs`, turns that list
into a dlsym dispatch table. The pattern comes from `fabric-godot-core`, which uses it for
GStreamer.

So the harness builds on a machine that has never seen iceoryx2, and it fails at start
rather than at link when the library is absent. `ldd` on either binary lists no iceoryx2.

That matters more here than it did for GStreamer. iceoryx2 is Rust, and weft writes no
Rust. A dlopen keeps the Rust artifact out of weft's build graph as well as out of its
source.

Three generated pieces come from that one file, and each has a reason.

- `iceoryx2_stubs.cc`, the dispatch table.
- `iox2_api.h`, the prototypes. `generate_stubs.py` emits none, because Chromium's callers
  include the real library headers. A prototype that disagrees with the table it calls
  through is a crash with no diagnostic, so both come from the same file.
- `src/iox2_decls.h` is hand-written, and it is the one piece that is not generated. It
  declares the opaque types. A handle is a pointer, so an incomplete struct is exact. A
  storage struct, `iox2_..._t`, is a real sized struct, and transcribing it would be a
  silent memory bug the day upstream adds a field. It stays incomplete, and the harness
  passes NULL for every one. iceoryx2 then allocates on the heap, which is the documented
  contract.

## What the proof is

Two programs and one struct.

- `src/snapshot.hpp` holds `weft::Snapshot`: a tick, an entity, and three positions in
  micrometres. This is the fixed point the data plane already uses.
- `src/publisher.cpp` loans a sample, writes the struct, and sends the loan. It does not
  serialize and it does not copy. `loan_uninit` returns memory the subscriber already
  maps, so a send is a pointer handoff.
- `src/subscriber.cpp` receives and checks. It checks the tick order and the payload it
  derives from the tick.

The check is the point. A bus that delivers garbage must fail here, and not print a count.

## How to run it

The build needs no iceoryx2. The run does. Build and install iceoryx2 v0.9.3 somewhere the
loader can find at run time. The prefix below matches the one `.gitignore` already
excludes.

    cmake -S <iceoryx2-src> -B <iceoryx2-src>/build \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/.iceoryx
    cmake --build <iceoryx2-src>/build -j
    cmake --install <iceoryx2-src>/build

Then build the proof and run both ends. Start the subscriber first.

    cmake -S native/harness -B native/harness/build -DCMAKE_BUILD_TYPE=Release
    cmake --build native/harness/build -j

    export LD_LIBRARY_PATH=$PWD/.iceoryx/lib64   # or set WEFT_ICEORYX2_PATH
    ./native/harness/build/weft-harness-subscriber 8 &
    ./native/harness/build/weft-harness-publisher 8

## The run

One machine, 16 cores, Fedora, GCC 16.1.1, iceoryx2 v0.9.3, Release.

    publisher: sent 8
    subscriber: tick 1 entity 42 x 1000 z -250
    ...
    subscriber: tick 8 entity 42 x 8000 z -2000
    subscriber: received 8, in order, intact

The subscriber exits 0. No daemon runs, and none is started.

`ldd native/harness/build/weft-harness-publisher` lists no iceoryx2. The library arrives
through `dlopen` at start.

## What this run does not measure

The latency and the rate. Eight messages at a 20 ms cycle measures that a message
arrives, and nothing else. A number belongs in `../logbook/data_plane.md` with the machine
and the settings that produced it, and this run produces none.

## Why iceoryx2 and not iceoryx v1

iceoryx v2.0.8, which is the C++ project, does not build here. Its Linux platform layer
includes `<sys/acl.h>` and links `acl`, and libacl is not allowed.

Neither part can be turned off from the command line. `LINUX` is a normal CMake variable
that shadows the cache, so `-DLINUX=OFF` does nothing, and `ICEORYX_PLATFORM` is a
`CACHE PATH FORCE`. Upstream ships an ACL-free `unix` layer, and reaching it needs a patch
to two build files.

`../essays/runtime-choice.md` holds the full reversal, and the cost of it.

## What comes next

1. The thread-per-core loop, once, because every plane uses it.
2. iceoryx2 in the container image, so CI runs this proof rather than a person.
3. The first plane behind it. `native/gyreplane` is the candidate, because its zone tick
   has no input at all until the bus carries one.
