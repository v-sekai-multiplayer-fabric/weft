#ifndef ZF_ZONETICK_H_
#define ZF_ZONETICK_H_

#include "fdb_database.h"

/*
 * ZoneTick: the FDB-backed read-tick-write operation RFD 0002 specifies --
 * "1 range read (200 KV pairs) + 1 commit (200 sets) per zone per tick."
 * This is task #7 (zonefabric M3), replacing task #11's in-memory-only
 * zonetick_step() placeholder with real durable entity state.
 *
 * Task #14: tick_count replaces the original float `dt` parameter.
 * lean-entity-packet's README is explicit that velocity is "scaled to
 * +/-PBVH_V_MAX_PHYSICAL_DEFAULT (500000 um/tick)" -- i.e. the wire
 * velocity is already a per-tick displacement, not a continuous rate a
 * variable dt would scale. The correct update is integer and per-tick:
 *   pos_um += (int64_t)vel_i16 * V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK / INT16_MAX
 * applied tick_count times (normally 1 -- the transport that fixed 30Hz
 * moved to native/gyreedge/transport, since a plane has no networking).
 *
 * On conflict, retries via fdb_handle_error the same way
 * src/handlers/tpcc_new_order.c's on_error_retry does -- not a new pattern.
 */

/* Reads every entity under zone z_id, applies the integer per-tick
 * position update described above tick_count times, writes them back,
 * and commits -- asynchronously. Calls done_cb(ctx, ok) when finished
 * (ok=false on an unretryable error). Safe to call again for the next
 * tick only after done_cb fires (one in-flight transaction per zone at a
 * time). */
typedef void (*zf_zonetick_done_cb)(void *ctx, bool ok);

int zf_zonetick_run(fdb_thread_state_t *fdb_state, uint32_t z_id, uint32_t tick_count,
                     zf_zonetick_done_cb done_cb, void *ctx);

#endif
