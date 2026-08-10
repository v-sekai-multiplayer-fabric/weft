/*
 * Standalone check of zf_zonetick.c's vel_to_um_per_tick() integer math
 * (task #14). No FDB/h2o dependency -- the conversion itself is pure
 * arithmetic, extracted here since zf_zonetick.c's copy is static.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK 500000

static int64_t vel_to_um_per_tick(int16_t v)
{
    return ((int64_t)v * XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK) / INT16_MAX;
}

int main(void)
{
    assert(vel_to_um_per_tick(0) == 0);
    assert(vel_to_um_per_tick(32767) == 500000); /* exact: INT16_MAX / INT16_MAX == 1 */

    /* INT16_MIN (-32768) has no positive counterpart in i16's asymmetric
     * range, so dividing by INT16_MAX (not INT16_MIN) overshoots the
     * -500000 target by a small, bounded amount -- documented behavior,
     * not a bug. Confirmed magnitude, not exact equality. */
    int64_t min_um = vel_to_um_per_tick(INT16_MIN);
    assert(min_um <= -500000 && min_um > -500100);

    /* No int64 overflow at the extremes. */
    assert(vel_to_um_per_tick(INT16_MIN) > INT64_MIN / 2);
    assert(vel_to_um_per_tick(INT16_MAX) < INT64_MAX / 2);

    printf("vel_to_um_per_tick: all checks passed (0=0, max=exact 500000, min=%lld)\n",
           (long long)min_um);
    return 0;
}
