/*
 * XRGridEntityPacket codec. Direct C transcription of
 * lean-entity-packet/EntityPacket/Codec.lean's putU32/getU32/putI64/
 * getI64/putI16/getI16/encode/decode -- little-endian throughout, same
 * byte offsets. See xr_grid_entity_packet.h for the field table.
 */

#include "xr_grid_entity_packet.h"

#include <string.h>

static void put_u32(uint8_t *b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[off + i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
}

static uint32_t get_u32(const uint8_t *b, size_t off)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= ((uint32_t)b[off + i]) << (i * 8);
    }
    return v;
}

static void put_i64(uint8_t *b, size_t off, int64_t v)
{
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        b[off + i] = (uint8_t)((u >> (i * 8)) & 0xFF);
    }
}

static int64_t get_i64(const uint8_t *b, size_t off)
{
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= ((uint64_t)b[off + i]) << (i * 8);
    }
    return (int64_t)u;
}

static void put_i16(uint8_t *b, size_t off, int16_t v)
{
    uint16_t u = (uint16_t)v;
    b[off] = (uint8_t)(u & 0xFF);
    b[off + 1] = (uint8_t)((u >> 8) & 0xFF);
}

static int16_t get_i16(const uint8_t *b, size_t off)
{
    uint16_t u = (uint16_t)b[off] | ((uint16_t)b[off + 1] << 8);
    return (int16_t)u;
}

void xr_grid_entity_packet_encode(const xr_grid_entity_packet_t *p, uint8_t buf[XR_PACKET_SIZE])
{
    memset(buf, 0, XR_PACKET_SIZE);

    put_u32(buf, 0, p->global_id);
    put_i64(buf, 4, p->pos_um_x);
    put_i64(buf, 12, p->pos_um_y);
    put_i64(buf, 20, p->pos_um_z);
    put_i16(buf, 28, p->vel_x);
    put_i16(buf, 30, p->vel_y);
    put_i16(buf, 32, p->vel_z);
    put_u32(buf, 40, p->hlc);
    put_u32(buf, 44, p->class_owner);
    put_u32(buf, 48, p->sub_index);
    put_i16(buf, 52, p->rot_x);
    put_i16(buf, 54, p->rot_y);
    put_i16(buf, 56, p->rot_z);
    memcpy(buf + XR_PACKET_PAYLOAD_OFFSET, p->payload, XR_PACKET_PAYLOAD_LEN);
}

void xr_grid_entity_packet_decode(const uint8_t buf[XR_PACKET_SIZE], xr_grid_entity_packet_t *p)
{
    p->global_id = get_u32(buf, 0);
    p->pos_um_x = get_i64(buf, 4);
    p->pos_um_y = get_i64(buf, 12);
    p->pos_um_z = get_i64(buf, 20);
    p->vel_x = get_i16(buf, 28);
    p->vel_y = get_i16(buf, 30);
    p->vel_z = get_i16(buf, 32);
    p->hlc = get_u32(buf, 40);
    p->class_owner = get_u32(buf, 44);
    p->sub_index = get_u32(buf, 48);
    p->rot_x = get_i16(buf, 52);
    p->rot_y = get_i16(buf, 54);
    p->rot_z = get_i16(buf, 56);
    memcpy(p->payload, buf + XR_PACKET_PAYLOAD_OFFSET, XR_PACKET_PAYLOAD_LEN);
}
