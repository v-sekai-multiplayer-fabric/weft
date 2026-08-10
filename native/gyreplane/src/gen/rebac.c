/*
 * ReBAC access check. See rebac.h for provenance and scope.
 */

#include "rebac.h"

rebac_relation_t rebac_action_min_relation(rebac_action_t action)
{
    switch (action) {
    case REBAC_ACTION_OBSERVE:  return REBAC_RELATION_PUBLIC;
    case REBAC_ACTION_INTERACT: return REBAC_RELATION_INSTANCE_MEMBER;
    case REBAC_ACTION_MODIFY:   return REBAC_RELATION_OWNER;
    }
    return REBAC_RELATION_OWNER; /* unreachable; fail closed on an unknown action */
}

bool rebac_max_relation(const rebac_relation_t *relations, size_t count, rebac_relation_t *out)
{
    if (count == 0) {
        return false; /* PlayerClaim.maxRelation's `none` case */
    }
    rebac_relation_t best = relations[0];
    for (size_t i = 1; i < count; i++) {
        /* maxRelStep: "if r.rank >= acc.rank then r else acc" -- ties keep
         * the later element, matching the Lean fold exactly (not that it's
         * observable here, since rank is a total order with no duplicate
         * meaning, but matching the exact fold behavior is the point of a
         * port, not just an equivalent result). */
        if (relations[i] >= best) {
            best = relations[i];
        }
    }
    *out = best;
    return true;
}

bool rebac_check(const rebac_relation_t *relations, size_t count, rebac_action_t action)
{
    rebac_relation_t max_rel;
    if (!rebac_max_relation(relations, count, &max_rel)) {
        return false; /* rebac_empty_denied */
    }
    return max_rel >= rebac_action_min_relation(action);
}
