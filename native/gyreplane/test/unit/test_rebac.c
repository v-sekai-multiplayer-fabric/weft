/*
 * Tests directly matching the theorems proved in lean-rebac-core's
 * Rebac/core/ReBAC.lean, plus the full grant matrix Action.minRelation /
 * Relation.rank define. See src/gen/rebac.h for provenance.
 */

#include "rebac.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* rebac_empty_denied: a claim with no relations is denied for every
     * action. */
    for (rebac_action_t a = REBAC_ACTION_OBSERVE; a <= REBAC_ACTION_MODIFY; a++) {
        assert(rebac_check(NULL, 0, a) == false);
    }

    /* rebac_public_observe: public relation suffices for .observe. */
    {
        rebac_relation_t rels[] = { REBAC_RELATION_PUBLIC };
        assert(rebac_check(rels, 1, REBAC_ACTION_OBSERVE) == true);
        /* ...but not for .interact or .modify. */
        assert(rebac_check(rels, 1, REBAC_ACTION_INTERACT) == false);
        assert(rebac_check(rels, 1, REBAC_ACTION_MODIFY) == false);
    }

    /* instanceMember suffices for .observe and .interact, not .modify --
     * this is the CMD_INSTANCE_ASSET "owner only" boundary zone.ex
     * documents ("modify: owner only"). */
    {
        rebac_relation_t rels[] = { REBAC_RELATION_INSTANCE_MEMBER };
        assert(rebac_check(rels, 1, REBAC_ACTION_OBSERVE) == true);
        assert(rebac_check(rels, 1, REBAC_ACTION_INTERACT) == true);
        assert(rebac_check(rels, 1, REBAC_ACTION_MODIFY) == false);
    }

    /* owner suffices for everything, including .modify -- the
     * CMD_INSTANCE_ASSET check itself. */
    {
        rebac_relation_t rels[] = { REBAC_RELATION_OWNER };
        assert(rebac_check(rels, 1, REBAC_ACTION_OBSERVE) == true);
        assert(rebac_check(rels, 1, REBAC_ACTION_INTERACT) == true);
        assert(rebac_check(rels, 1, REBAC_ACTION_MODIFY) == true);
    }

    /* friend / guildMember: above instanceMember, below owner -- neither
     * suffices for .modify. */
    {
        rebac_relation_t f[] = { REBAC_RELATION_FRIEND };
        rebac_relation_t g[] = { REBAC_RELATION_GUILD_MEMBER };
        assert(rebac_check(f, 1, REBAC_ACTION_MODIFY) == false);
        assert(rebac_check(g, 1, REBAC_ACTION_MODIFY) == false);
        assert(rebac_check(f, 1, REBAC_ACTION_INTERACT) == true);
        assert(rebac_check(g, 1, REBAC_ACTION_INTERACT) == true);
    }

    /* PlayerClaim.maxRelation: a claim holding multiple relations is
     * judged by the highest rank present, regardless of list order. */
    {
        rebac_relation_t low_then_high[] = { REBAC_RELATION_PUBLIC, REBAC_RELATION_OWNER };
        rebac_relation_t high_then_low[] = { REBAC_RELATION_OWNER, REBAC_RELATION_PUBLIC };
        assert(rebac_check(low_then_high, 2, REBAC_ACTION_MODIFY) == true);
        assert(rebac_check(high_then_low, 2, REBAC_ACTION_MODIFY) == true);

        rebac_relation_t mid[] = { REBAC_RELATION_PUBLIC, REBAC_RELATION_FRIEND, REBAC_RELATION_INSTANCE_MEMBER };
        assert(rebac_check(mid, 3, REBAC_ACTION_INTERACT) == true);  /* friend >= instanceMember */
        assert(rebac_check(mid, 3, REBAC_ACTION_MODIFY) == false);   /* nothing reaches owner */
    }

    /* Relation.rank ordering itself, exactly as defined in the Lean
     * source: public < instanceMember < friend < guildMember < owner. */
    assert(REBAC_RELATION_PUBLIC < REBAC_RELATION_INSTANCE_MEMBER);
    assert(REBAC_RELATION_INSTANCE_MEMBER < REBAC_RELATION_FRIEND);
    assert(REBAC_RELATION_FRIEND < REBAC_RELATION_GUILD_MEMBER);
    assert(REBAC_RELATION_GUILD_MEMBER < REBAC_RELATION_OWNER);

    printf("rebac: all checks passed (rebac_empty_denied, rebac_public_observe,\n"
           "       owner-only modify boundary, maxRelation order-independence)\n");
    return 0;
}
