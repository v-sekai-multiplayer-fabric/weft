#ifndef XR_GRID_ENTITY_PACKET_H_
#define XR_GRID_ENTITY_PACKET_H_

#include <stdint.h>

/*
 * XRGridEntityPacket: the 100-byte fabric entity wire packet.
 *
 * NOT hand-written -- this is a direct C transcription of the canonical
 * Lean 4 spec in v-sekai-multiplayer-fabric/lean-entity-packet
 * (EntityPacket/Codec.lean), which is itself the source of truth the
 * engine's C++ XRGridEntityPacket::decode is checked against
 * (packet_diff.gd). Field layout, byte offsets, and little-endian
 * encoding all come directly from that file -- do not hand-edit the
 * offsets without re-checking Codec.lean.
 *
 * Task #10: this supersedes zf_entity_val_t's float-double placeholder
 * (src/zf_kv.h) as the real wire/storage entity type. The wire has no
 * floats at all -- position is int64 absolute micrometers, velocity is
 * i16 scaled to +/-PBVH_V_MAX_PHYSICAL_DEFAULT (500000 um/tick).
 *
 * | offset | field               | encoding                          |
 * |--------|---------------------|------------------------------------|
 * | 0      | global_id           | u32                                |
 * | 4      | position x/y/z      | int64 absolute micrometers x3 (24B)|
 * | 28     | velocity x/y/z      | i16 scaled x3 (6B)                 |
 * | 40     | hlc                 | u32 (frame<<8 | counter)           |
 * | 44     | class|owner         | u32                                |
 * | 48     | sub_index           | u32                                |
 * | 52     | rotation            | i16 swing-twist x3 (6B)            |
 * | 58     | payload             | 42 bytes userdata                  |
 */

#define XR_PACKET_SIZE 100
#define XR_PACKET_PAYLOAD_OFFSET 58
#define XR_PACKET_PAYLOAD_LEN (XR_PACKET_SIZE - XR_PACKET_PAYLOAD_OFFSET) /* 42 */

/* PBVH_V_MAX_PHYSICAL_DEFAULT, per README.md: velocity i16 is scaled to
 * +/- this many micrometers/tick. Carried over as documentation of the
 * scale factor; this codec does not itself apply/remove the scaling --
 * callers own the physical<->i16 conversion, matching how the Lean spec
 * only models the wire's integral Packet, not the physics-unit mapping. */
#define XR_PACKET_V_MAX_PHYSICAL_DEFAULT_UM_PER_TICK 500000

typedef struct {
    uint32_t global_id;
    int64_t  pos_um_x, pos_um_y, pos_um_z;
    int16_t  vel_x, vel_y, vel_z;
    uint32_t hlc;          /* (frame << 8) | counter */
    uint32_t class_owner;  /* (class << 24) | owner, per README */
    uint32_t sub_index;
    int16_t  rot_x, rot_y, rot_z; /* swing-twist */
    uint8_t  payload[XR_PACKET_PAYLOAD_LEN];
} xr_grid_entity_packet_t;

/* Encodes into buf (must be XR_PACKET_SIZE bytes). */
void xr_grid_entity_packet_encode(const xr_grid_entity_packet_t *p, uint8_t buf[XR_PACKET_SIZE]);

/* Decodes from buf (must be XR_PACKET_SIZE bytes). */
void xr_grid_entity_packet_decode(const uint8_t buf[XR_PACKET_SIZE], xr_grid_entity_packet_t *p);

#endif
