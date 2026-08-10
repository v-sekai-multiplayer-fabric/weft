/*
 * Zonefabric key-value encoding for FoundationDB.
 * See zf_kv.h for scope and provenance.
 */

#include "zf_kv.h"

#include <string.h>

void zf_kv_encode_u32_be(uint8_t *buf, uint32_t val)
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

uint32_t zf_kv_decode_u32_be(const uint8_t *buf)
{
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static size_t append_prefix(uint8_t *buf, const char *prefix)
{
    size_t len = strlen(prefix);
    memcpy(buf, prefix, len);
    return len;
}

size_t zf_kv_zone_key(uint8_t *buf, uint32_t z_id)
{
    size_t off = append_prefix(buf, SS_ZF_ZONE);
    zf_kv_encode_u32_be(buf + off, z_id);
    return off + 4;
}

size_t zf_kv_entity_key(uint8_t *buf, uint32_t z_id, uint32_t e_id)
{
    size_t off = append_prefix(buf, SS_ZF_ENTITY);
    zf_kv_encode_u32_be(buf + off, z_id);
    off += 4;
    buf[off++] = '/';
    zf_kv_encode_u32_be(buf + off, e_id);
    return off + 4;
}

size_t zf_kv_entity_range_begin(uint8_t *buf, uint32_t z_id)
{
    size_t off = append_prefix(buf, SS_ZF_ENTITY);
    zf_kv_encode_u32_be(buf + off, z_id);
    off += 4;
    buf[off++] = '/';
    return off; /* nothing appended after the trailing '/' -- covers every e_id */
}

size_t zf_kv_entity_range_end(uint8_t *buf, uint32_t z_id)
{
    /* FDB range ends are exclusive; 0xFF is not a valid key-byte in our
     * ASCII-prefix scheme, so appending it after the '/' gives an
     * exclusive upper bound past every possible e_id under this zone,
     * matching tpcc_kv.c's kv_stock_range_end pattern. */
    size_t off = zf_kv_entity_range_begin(buf, z_id);
    buf[off++] = 0xFF;
    return off;
}

void zf_kv_encode_zone(uint8_t *buf, const zf_zone_val_t *val)
{
    memcpy(buf, val, sizeof(*val));
}

void zf_kv_decode_zone(const uint8_t *buf, int len, zf_zone_val_t *val)
{
    memset(val, 0, sizeof(*val));
    if (len >= ZF_ZONE_VAL_SIZE) {
        memcpy(val, buf, sizeof(*val));
    }
}

void zf_kv_encode_entity(uint8_t *buf, const zf_entity_val_t *val)
{
    /* val (zf_entity_val_t == xr_grid_entity_packet_t) has host struct
     * padding the wire format does not -- must go through the real codec,
     * not memcpy, or the FDB value bytes would not match what
     * xr_grid_entity_packet_decode() (and the client, and
     * test/unit/test_xr_grid_entity_packet.c's golden vectors) expect. */
    xr_grid_entity_packet_encode(val, buf);
}

void zf_kv_decode_entity(const uint8_t *buf, int len, zf_entity_val_t *val)
{
    memset(val, 0, sizeof(*val));
    if (len >= ZF_ENTITY_VAL_SIZE) {
        xr_grid_entity_packet_decode(buf, val);
    }
}
