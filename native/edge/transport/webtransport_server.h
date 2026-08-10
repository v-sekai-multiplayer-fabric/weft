#pragma once

#include <h2o.h>
#include <picoquic.h>
#include <h3zero_common.h>
#include <stdint.h>

#include "../fdb_database.h"

/* Task #17 (parity check against the original Godot FabricZone):
 * fabric_zone.cpp's own tick rate is Engine::get_singleton()->
 * get_physics_ticks_per_second(), initialized to PBVH_SIM_TICK_HZ = 20
 * (thirdparty/misc/predictive_bvh.h in fabric-godot-core). This project
 * runs faster, not just matching: 64 Hz, independent of client
 * datagram traffic (see the zonetick_timer_* fields and
 * on_zonetick_timer_fire() below -- the old on_wt_datagram-only path
 * left entities with no nearby client frozen, since nothing else ever
 * called zf_zonetick_run() for them; confirmed by reading this file's
 * own then-current code, not assumed). */
#define ZONE_TICK_HZ 64

/* CORRECTED: a zone fabric is multiple processes, each handling exactly
 * one zone (1 process : 1 zone), not one process ticking a fixed array
 * of zones internally. The earlier WT_SERVER_ZONE_FABRIC_SIZE=4 design
 * (N zones ticked in a loop inside a single process) was wrong -- it
 * does not match zone-server/AGENTS.md's actual deployment shape (one
 * UDP port per zone instance, up to 100 concurrent zones, i.e. up to
 * 100 concurrent *processes*), and it does not match RFD 0002's
 * core-scaling argument either: "each core processes independent
 * zones" describes independent processes/cores, not one process
 * internally looping over several zones. This process now handles
 * exactly one zone, whose z_id is supplied at startup (see main.c's
 * -z<zone_id> flag). Coordinating many such processes/zones is
 * `multiplayer-fabric-manuals rfd/0086`'s gossip/VClock question
 * -- still real work, not yet done, but now correctly scoped as
 * "multiple processes, multiple zones, 1-1" instead of folded into a
 * single-process loop that never needed it. */

typedef struct {
    picoquic_quic_t *quic;

    int udp_fd;
    h2o_socket_t *udp_sock;

    /* picoquic's own protocol timer (retransmission/idle-timeout
     * housekeeping) -- its interval changes dynamically per
     * picoquic_get_next_wake_time(), NOT the zonetick cadence. See
     * zonetick_timer_fd below for that. */
    int timer_fd;
    h2o_socket_t *timer_sock;

    /* The independent ZONE_TICK_HZ driver -- ticks every entity in this
     * zone on a fixed period, regardless of client datagram traffic.
     * Separate from timer_fd on purpose: picoquic's timer is
     * single-shot and rearmed to a different interval every fire
     * (protocol-driven), while this one is periodic at a fixed rate
     * from the start (timerfd's own it_interval, not rearmed by hand
     * each time). */
    int zonetick_timer_fd;
    h2o_socket_t *zonetick_timer_sock;

    h2o_loop_t *loop;
    int port;

    fdb_thread_state_t *fdb_state;

    /* This process handles exactly one zone -- z_id, set at startup.
     * zone_in_flight guards that single zone's FDB transaction so a
     * second ZoneTick never starts before the previous one commits
     * (the datagram-triggered path and the ZONE_TICK_HZ timer both
     * call zonetick_fdb_this_zone(), and share this same guard). */
    uint32_t z_id;
    bool zone_in_flight;

    /* H3/WebTransport session context (task #12) -- owns the path table
     * routing ZONE_WT_PATH to the session callbacks in wt_session.c. */
    h3zero_callback_ctx_t *wt_ctx;
} webtransport_server_t;

/* Binds UDP `port`, creates the picoquic context (cert_file/key_file are
 * PEM paths, matching zone-server's TLS_CERT/TLS_KEY per zone-server/AGENTS.md),
 * and wires both the UDP socket and a timerfd into `loop`. z_id is the
 * one zone this process handles (see the "fabric = multiple processes"
 * correction above). Returns 0 on success. */
int webtransport_server_init(webtransport_server_t *server, h2o_loop_t *loop,
                              int port, const char *cert_file, const char *key_file,
                              fdb_thread_state_t *fdb_state, uint32_t z_id);

void webtransport_server_close(webtransport_server_t *server);
