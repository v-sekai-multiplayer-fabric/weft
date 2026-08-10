# Decision: add The Gyre as a second MUD domain, not a second engine

**Status:** done for the smallest loop (PR #5); not verified end to end.

## Context

`mud/guest/mud_guest.cpp` served exactly one setting, Middleham (a
spy-thriller MUD: gate/market/temple rooms, trust/suspicion
objectives), hardcoded throughout -- the starting room, the item
list, the objective logic, all fixed to that one world.

`multiplayer-fabric-manuals` RFD 0085 proposes a second setting, The
Gyre (a gig-economy-survival sci-fi MUD), reskinning the loot-action
core-loop shell (that repo's RFD 0045) rather than a new server
architecture. The RFD itself does not change any code here; this doc
records the code side.

## Decision: a domain switch, not a rewrite

`mud_boot()` now reads a `domain` CBOR field (`"middleham"` default,
`"the_gyre"` new). `MiddlehamStateMachine` keeps its name and its
existing behavior completely unchanged for the default domain -- a
rename or a full genericization would ripple across the whole file
for no behavior change, and stays a real follow-up once a third
domain lands, not part of this task. A `domain_` member and three
guarded branches (the constructor's start-room pick, `clone_rooms()`,
`objective_complete()`) cover the new domain, backed by a new
`gyre_room_templates()`: two rooms, `decanting_floor` and
`splicers_den`, one exit each way, no items, no NPCs -- the smallest
possible loop (look, go, look), not RFD 0085's full room graph.

The `domain` field threads through the same chain `objective` already
used: `mud_cbor_encode_boot_config()` -> `mud_session_get_or_create()`
-> `on_mud_command()`'s `POST /api/mud/command` handler, which now
reads `domain` from the request body and picks a matching default
objective (`explore_gyre` vs `gain_watch_trust`). That handler's own
prior comment already flagged an objective/domain picker as
website-UI scope, not its own -- this fills that gap rather than
inventing a new extension point.

`mud/web/index.html` + `mud.js` get a `#modeSelect` (Middleham / The
Gyre). Each mode keeps its own `localStorage` session id, since the
server only reads a session's domain once, on that id's first
request (`mud_session.c`'s own get-or-create semantics) -- reusing one
session id across modes would silently keep whichever domain created
it.

## Verification

Real, not assumed:

- `mud/guest/test/gyre_smoke_test.cpp`: a native (non-riscv64) link of
  `mud_guest.cpp` driving `mud_boot()`/`mud_step()` through the whole
  Gyre loop (look, go east, look). Built and run locally against
  QCBOR (`laurencelundblade/QCBOR`) -- real narration text, real
  `post_room` transitions, `objective_complete()` true after both
  rooms are visited. A native Middleham boot the same way, same seed
  DIFFERENTIAL_TEST.md uses, confirmed byte-identical "Middleham City
  Gate" narration to before this change -- the default path did not
  regress.
- The client change (mode selector, per-domain session id, `domain`
  field on the wire) was driven end to end with real Playwright, both
  Chromium and Camoufox (a Firefox-family browser), against a
  throwaway local Node stub matching the real API shape -- not
  committed, thrown away once the client logic was confirmed correct.
- `mud/web/test/gyre.spec.ts` is the real, committed spec, Camoufox-
  driven, following `mud.spec.ts`'s own house rule: `MUD_BASE_URL`
  must point at a real reachable instance, no mock. It has not run
  against one -- red until a real deployment with the Gyre domain
  exists, the same state `mud.spec.ts` itself documents before its
  own first real deploy.

Not verified: a `riscv64-musl` + `libriscv` build/run of the changed
guest code (no cross toolchain in the environment this was written
in). No FDB/H2O real build or deploy attempted.

## Revisit when

A real `-z` deploy exists with the Gyre domain reachable: run
`gyre.spec.ts` for real, and run a real riscv64 differential test
(matching `DIFFERENTIAL_TEST.md`'s own Middleham precedent) before
treating this as more than a reviewed, natively-tested diff. RFD
0085's fuller room graph, contract catalog, and item set are still
design only; porting them past the two-room smallest loop is a
separate, larger task, not implied by this one.
