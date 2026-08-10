#include "xr_grid_entity_packet.h"

#ifndef PACKET_GOLDEN_CSV_PATH
#define PACKET_GOLDEN_CSV_PATH "test/unit/packet_golden.csv"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = (uint8_t)((hexval(hex[i*2]) << 4) | hexval(hex[i*2+1]));
    }
}

/* Simple CSV field splitter, good enough for this well-formed golden file. */
static int split_csv(char *line, char **fields, int max_fields)
{
    int n = 0;
    char *tok = strtok(line, ",\n");
    while (tok != NULL && n < max_fields) {
        fields[n++] = tok;
        tok = strtok(NULL, ",\n");
    }
    return n;
}

int main(void)
{
    FILE *f = fopen(PACKET_GOLDEN_CSV_PATH, "r");
    if (!f) { perror("open packet_golden.csv"); return 1; }

    char line[1024];
    /* skip header */
    if (!fgets(line, sizeof(line), f)) { fprintf(stderr, "empty file\n"); return 1; }

    int vectors = 0;
    while (fgets(line, sizeof(line), f)) {
        char linecopy[1024];
        strncpy(linecopy, line, sizeof(linecopy));
        char *fields[16];
        int n = split_csv(linecopy, fields, 16);
        if (n < 10) continue;

        const char *hex = fields[0];
        int64_t gid = atoll(fields[1]);
        int64_t pumx = atoll(fields[2]);
        int64_t pumy = atoll(fields[3]);
        int64_t pumz = atoll(fields[4]);
        int64_t velx = atoll(fields[5]);
        int64_t vely = atoll(fields[6]);
        int64_t velz = atoll(fields[7]);
        int pay0 = atoi(fields[8]);
        int pay41 = atoi(fields[9]);

        assert(strlen(hex) == XR_PACKET_SIZE * 2);
        uint8_t buf[XR_PACKET_SIZE];
        hex_to_bytes(hex, buf, XR_PACKET_SIZE);

        xr_grid_entity_packet_t p;
        xr_grid_entity_packet_decode(buf, &p);

        if ((int64_t)p.global_id != gid) { fprintf(stderr, "gid mismatch: got %u want %lld\n", p.global_id, (long long)gid); return 1; }
        if (p.pos_um_x != pumx) { fprintf(stderr, "pumx mismatch: got %lld want %lld\n", (long long)p.pos_um_x, (long long)pumx); return 1; }
        if (p.pos_um_y != pumy) { fprintf(stderr, "pumy mismatch: got %lld want %lld\n", (long long)p.pos_um_y, (long long)pumy); return 1; }
        if (p.pos_um_z != pumz) { fprintf(stderr, "pumz mismatch: got %lld want %lld\n", (long long)p.pos_um_z, (long long)pumz); return 1; }
        if (p.vel_x != velx) { fprintf(stderr, "velx mismatch: got %d want %lld\n", p.vel_x, (long long)velx); return 1; }
        if (p.vel_y != vely) { fprintf(stderr, "vely mismatch: got %d want %lld\n", p.vel_y, (long long)vely); return 1; }
        if (p.vel_z != velz) { fprintf(stderr, "velz mismatch: got %d want %lld\n", p.vel_z, (long long)velz); return 1; }
        if (p.payload[0] != (uint8_t)pay0) { fprintf(stderr, "pay0 mismatch: got %u want %d\n", p.payload[0], pay0); return 1; }
        if (p.payload[41] != (uint8_t)pay41) { fprintf(stderr, "pay41 mismatch: got %u want %d\n", p.payload[41], pay41); return 1; }

        /* Round-trip: re-encode and confirm byte-identical to the golden hex. */
        uint8_t reencoded[XR_PACKET_SIZE];
        xr_grid_entity_packet_encode(&p, reencoded);
        if (memcmp(reencoded, buf, XR_PACKET_SIZE) != 0) {
            fprintf(stderr, "round-trip byte mismatch on vector gid=%lld\n", (long long)gid);
            return 1;
        }

        vectors++;
    }

    fclose(f);
    printf("%d/%d golden vectors passed (decode matches CSV, encode round-trips byte-identical)\n", vectors, vectors);
    return vectors > 0 ? 0 : 1;
}
