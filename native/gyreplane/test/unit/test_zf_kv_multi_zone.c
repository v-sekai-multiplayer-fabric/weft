/*
 * Multi-zone isolation test for zf_kv's keyspace -- task #12's direct
 * response to being told a zone is a fabric of zones, not a singleton.
 * Every earlier test used a single z_id (7); this proves the property
 * every zone-server-h2o *process* depends on regardless of which zone
 * it is configured for (the z_id, set via main.c's
 * -z flag): N zones sharing one FDB keyspace never see each other's
 * entities, for any z_id spacing (not just adjacent small integers).
 * The isolation matters even though each process only ever touches its
 * own one zone -- FDB is the shared store multiple such processes all
 * write into, so no process may ever be able to read or write another
 * zone's keys, by construction of the keyspace itself.
 */

#include "zf_kv.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NUM_TEST_ZONES 6

/* Contiguous small z_id values (0, 1, 2, 3, as a small deployment might
 * assign) plus wider/sparser values (1000 and 0xFFFFFFFE, near the u32
 * boundary) to catch off-by-one range-end bugs a purely-contiguous
 * test could miss. */
static const uint32_t test_zones[NUM_TEST_ZONES] = { 0, 1, 2, 3, 1000, 0xFFFFFFFEu };

int main(void)
{
    uint8_t ranges_begin[NUM_TEST_ZONES][64];
    uint8_t ranges_end[NUM_TEST_ZONES][64];
    size_t begin_len[NUM_TEST_ZONES], end_len[NUM_TEST_ZONES];

    for (int i = 0; i < NUM_TEST_ZONES; i++) {
        begin_len[i] = zf_kv_entity_range_begin(ranges_begin[i], test_zones[i]);
        end_len[i] = zf_kv_entity_range_end(ranges_end[i], test_zones[i]);
        assert(end_len[i] == begin_len[i] + 1); /* range end = begin + one 0xFF byte */
    }

    /* No two zones' [begin, end) ranges overlap: for every pair, either
     * zone A's range ends at or before zone B's begins, or vice versa.
     * FDB range order is lexicographic byte-string order -- reproduced
     * here with memcmp, not FDB itself (no live cluster in this
     * sandbox), which is exactly the ordering FDB's tuple/range API
     * uses for ASCII-prefixed keys like these. */
    for (int a = 0; a < NUM_TEST_ZONES; a++) {
        for (int b = 0; b < NUM_TEST_ZONES; b++) {
            if (a == b) continue;
            /* zone a's end must be <= zone b's begin, OR zone b's end <= zone a's begin */
            size_t min_a_end = end_len[a] < begin_len[b] ? end_len[a] : begin_len[b];
            int a_before_b = memcmp(ranges_end[a], ranges_begin[b], min_a_end) <= 0;
            size_t min_b_end = end_len[b] < begin_len[a] ? end_len[b] : begin_len[a];
            int b_before_a = memcmp(ranges_end[b], ranges_begin[a], min_b_end) <= 0;
            if (!(a_before_b || b_before_a)) {
                fprintf(stderr, "OVERLAP: zone %u and zone %u ranges are not disjoint\n",
                        test_zones[a], test_zones[b]);
                return 1;
            }
        }
    }

    /* For every zone, every entity key built under that zone falls
     * strictly inside that zone's own [begin, end) range, and strictly
     * outside every *other* zone's range -- the actual property
     * zf_zonetick's range-scan correctness depends on. */
    for (int owner = 0; owner < NUM_TEST_ZONES; owner++) {
        uint8_t entity_key[64];
        size_t klen = zf_kv_entity_key(entity_key, test_zones[owner], /* e_id */ 42);

        /* Inside its own zone's range: entity_key >= range_begin and
         * entity_key < range_end, using the same lexicographic
         * comparison rule as the cross-zone check below. */
        assert(memcmp(entity_key, ranges_begin[owner], begin_len[owner]) >= 0);
        {
            size_t cmp_len = klen < end_len[owner] ? klen : end_len[owner];
            int cmp = memcmp(entity_key, ranges_end[owner], cmp_len);
            assert(cmp < 0 || (cmp == 0 && klen < end_len[owner]));
        }

        for (int other = 0; other < NUM_TEST_ZONES; other++) {
            if (other == owner) continue;
            /* Outside every other zone's range: either the key sorts
             * before that zone's range begins, or at/after it ends.
             * Standard lexicographic (tuple) ordering: compare the
             * shared prefix first; only fall back to length when that
             * prefix is exactly equal (one string is a strict prefix of
             * the other). An earlier version of this check required
             * klen <= begin_len[other] unconditionally, which broke any
             * comparison where the entity key is longer than the range
             * bound it is being compared against (i.e. almost always) --
             * caught by this test failing on its first run, fixed here. */
            size_t cmp_len_begin = klen < begin_len[other] ? klen : begin_len[other];
            int cmp_begin = memcmp(entity_key, ranges_begin[other], cmp_len_begin);
            int before_other = cmp_begin < 0 || (cmp_begin == 0 && klen < begin_len[other]);
            size_t cmp_len_end = klen < end_len[other] ? klen : end_len[other];
            int cmp_end = memcmp(entity_key, ranges_end[other], cmp_len_end);
            int at_or_after_other_end = cmp_end > 0 || (cmp_end == 0 && klen >= end_len[other]);
            if (!(before_other || at_or_after_other_end)) {
                fprintf(stderr, "LEAK: zone %u's entity key falls inside zone %u's range\n",
                        test_zones[owner], test_zones[other]);
                return 1;
            }
        }
    }

    printf("zf_kv multi-zone isolation: %d zones (contiguous 0-3, plus 1000 and "
           "0x%08x near the u32 boundary), all pairwise disjoint, no entity-key "
           "leaks across zones\n", NUM_TEST_ZONES, test_zones[NUM_TEST_ZONES - 1]);
    return 0;
}
