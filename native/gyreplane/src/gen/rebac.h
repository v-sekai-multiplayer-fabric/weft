#ifndef REBAC_H_
#define REBAC_H_

#include <stddef.h>
#include <stdbool.h>

/*
 * ReBAC access check, ported from
 * v-sekai-multiplayer-fabric/lean-rebac-core's Rebac/core/ReBAC.lean
 * (`rebacCheck`, `Relation`, `Relation.rank`, `Action`, `Action.minRelation`,
 * `PlayerClaim.maxRelation`) -- task #13.
 *
 * That repo's own doc comment is the authority-model note this directly
 * answers for zone.ex's CMD_INSTANCE_ASSET spec ("modify: owner only",
 * "observe: public"):
 *
 *   "The authority zone for entity E is the local coordinator for
 *    decisions about E. geometricAuthority(view, hilbert(E.pos)) is the
 *    only zone that evaluates rebacCheck -- all other zones must forward
 *    the request there... Interest zones may evaluate .observe locally
 *    (public relation always held). Only the authority zone evaluates
 *    .interact and .modify."
 *
 * This C port is the pure predicate only (rebacCheck itself) -- the
 * geometric-authority routing it depends on (which zone evaluates the
 * check) is task #7/#8's zone-authority logic, not duplicated here.
 *
 * NOTE: Rebac/core/NoGod.lean (imported by ReBAC.lean) is a much larger
 * module than its name suggests -- gossip-based vector-clock zone-range
 * consensus (geometricAuthority/geometricInterest, HLC), not itself an
 * access-control system. Only the ReBAC.lean layer built on top of it is
 * ported here; the gossip/consensus layer is out of scope for task #13
 * and relevant instead to task #8/#9's zone-authority and migration work.
 */

/* Relation.rank, exact order from the Lean source (public=0 .. owner=4). */
typedef enum {
    REBAC_RELATION_PUBLIC = 0,
    REBAC_RELATION_INSTANCE_MEMBER = 1,
    REBAC_RELATION_FRIEND = 2,
    REBAC_RELATION_GUILD_MEMBER = 3,
    REBAC_RELATION_OWNER = 4,
} rebac_relation_t;

typedef enum {
    REBAC_ACTION_OBSERVE,
    REBAC_ACTION_INTERACT,
    REBAC_ACTION_MODIFY,
} rebac_action_t;

/* Action.minRelation. */
rebac_relation_t rebac_action_min_relation(rebac_action_t action);

/* PlayerClaim.maxRelation: the highest-ranked relation in `relations`
 * (there are `count` of them), or false with *out unset if count == 0
 * (Lean's `none` case). Returns true and sets *out otherwise. */
bool rebac_max_relation(const rebac_relation_t *relations, size_t count, rebac_relation_t *out);

/* rebacCheck: grant iff the claim's max relation rank >= action's min
 * relation rank. False for an empty claim (rebac_empty_denied, proved in
 * the Lean source). */
bool rebac_check(const rebac_relation_t *relations, size_t count, rebac_action_t action);

#endif
