// Replay the real SUMO trace through the nasty hot-path decode+apply.
// Each entity update is a bitpacked (u32 slot, f32 x, f32 y) struct; decode is a
// cast, apply writes the entity slab. This is the ">15M pps" apply number on real
// traffic movement instead of synthetic random writes.
//
// build: cc -O3 -march=native -pthread replay.c -o replay
// run:   ./replay scenario/frames.bin [threads] [repeats]
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct upd {
	uint32_t slot;
	float x, y;
};
struct vec2 {
	float x, y;
};

static uint8_t *g_buf;
static uint32_t g_max_slots, g_nframes;
// per-frame: pointer to first upd, and count.
static struct upd **g_frame;
static uint32_t *g_count;
static uint64_t g_total_updates;

static int threads = 1, repeats = 1;

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

static void *worker(void *arg) {
	(void)arg;
	struct vec2 *slab = calloc(g_max_slots, sizeof(struct vec2));
	double acc = 0.0;
	for (int r = 0; r < repeats; r++) {
		for (uint32_t f = 0; f < g_nframes; f++) {
			struct upd *u = g_frame[f];
			uint32_t c = g_count[f];
			for (uint32_t i = 0; i < c; i++) {
				// decode = cast (u is already the struct), apply = slab write.
				slab[u[i].slot].x = u[i].x;
				slab[u[i].slot].y = u[i].y;
			}
		}
	}
	// keep the slab live so the compiler cannot drop the loop.
	for (uint32_t s = 0; s < g_max_slots; s++) acc += slab[s].x + slab[s].y;
	free(slab);
	return (void *)(uintptr_t)(acc != 0.0);
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "scenario/frames.bin";
	if (argc > 2) threads = atoi(argv[2]);
	if (argc > 3) repeats = atoi(argv[3]);

	FILE *fh = fopen(path, "rb");
	if (!fh) {
		perror("open");
		return 1;
	}
	fseek(fh, 0, SEEK_END);
	long sz = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	g_buf = malloc(sz);
	if (fread(g_buf, 1, sz, fh) != (size_t)sz) return 1;
	fclose(fh);

	uint32_t magic;
	memcpy(&magic, g_buf, 4);
	memcpy(&g_max_slots, g_buf + 4, 4);
	memcpy(&g_nframes, g_buf + 8, 4);
	if (magic != 0x53554D4Fu) {
		fprintf(stderr, "bad magic\n");
		return 1;
	}
	g_frame = malloc(g_nframes * sizeof(struct upd *));
	g_count = malloc(g_nframes * sizeof(uint32_t));
	uint8_t *p = g_buf + 12;
	for (uint32_t f = 0; f < g_nframes; f++) {
		uint32_t c;
		memcpy(&c, p, 4);
		p += 4;
		g_count[f] = c;
		g_frame[f] = (struct upd *)p;
		p += (size_t)c * sizeof(struct upd);
		g_total_updates += c;
	}

	uint64_t applies = g_total_updates * (uint64_t)repeats * (uint64_t)threads;
	pthread_t th[256];
	double t0 = now();
	for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, NULL);
	for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
	double dt = now() - t0;

	double pps = applies / dt;
	printf("max_slots=%u frames=%u updates/pass=%llu threads=%d repeats=%d\n",
	       g_max_slots, g_nframes, (unsigned long long)g_total_updates, threads,
	       repeats);
	printf("applies=%llu  time=%.3fs  pps=%.1fM  ns/apply/core=%.2f\n",
	       (unsigned long long)applies, dt, pps / 1e6, dt * 1e9 / applies * threads);
	return 0;
}
