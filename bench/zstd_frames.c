// "Last-frame zstd dictionary" trick for state replication bandwidth. A frame is
// E entities (20B each: pos i32x3 + quat i16x4). Consecutive frames differ only
// where entities moved, so compressing frame[i] with frame[i-1] as the zstd
// dictionary captures just the deltas. Measures ratio and throughput vs no-dict.
#include <zstd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define E 256                 // entities/frame
#define ESZ 20                // bytes/entity
#define FSZ (E*ESZ)           // frame bytes = 5120
#define F 2000                // frames in the stream
#define MOVE_FRAC 30          // % of entities that move each frame

static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static uint64_t rng=88172645463325252ull;
static uint64_t rnd(){ rng^=rng<<13; rng^=rng>>7; rng^=rng<<17; return rng; }

int main(){
  static uint8_t frames[F][FSZ];
  // Frame 0 random, then small per-entity deltas on a fraction of entities.
  for(int i=0;i<FSZ;i++) frames[0][i]=rnd();
  for(int f=1;f<F;f++){
    memcpy(frames[f],frames[f-1],FSZ);
    for(int e=0;e<E;e++){
      if((int)(rnd()%100) < MOVE_FRAC){
        int32_t* p=(int32_t*)(frames[f]+e*ESZ);
        p[0]+= (int)(rnd()%64)-32; p[1]+=(int)(rnd()%64)-32; p[2]+=(int)(rnd()%64)-32;
      }
    }
  }

  size_t cap=ZSTD_compressBound(FSZ);
  uint8_t* cbuf=malloc(cap); uint8_t* dbuf=malloc(FSZ);
  ZSTD_CCtx* cc=ZSTD_createCCtx(); ZSTD_DCtx* dc=ZSTD_createDCtx();

  for(int lvl=1; lvl<=3; lvl+=2){
    // No dictionary (each frame independent).
    double t0=now(); size_t tot_nodict=0;
    for(int f=0;f<F;f++) tot_nodict += ZSTD_compressCCtx(cc,cbuf,cap,frames[f],FSZ,lvl);
    double nod_s=now()-t0;

    // Last-frame dictionary: compress frame[f] using frame[f-1] as dict.
    t0=now(); size_t tot_dict=0;
    for(int f=1;f<F;f++) tot_dict += ZSTD_compress_usingDict(cc,cbuf,cap,frames[f],FSZ,frames[f-1],FSZ,lvl);
    double dict_s=now()-t0;

    // Decompress the last-frame-dict stream (correctness + throughput).
    t0=now();
    for(int f=1;f<F;f++){
      size_t cs=ZSTD_compress_usingDict(cc,cbuf,cap,frames[f],FSZ,frames[f-1],FSZ,lvl);
      size_t ds=ZSTD_decompress_usingDict(dc,dbuf,FSZ,cbuf,cs,frames[f-1],FSZ);
      if(ds!=FSZ || memcmp(dbuf,frames[f],FSZ)){ printf("MISMATCH lvl %d f %d\n",lvl,f); return 1; }
    }
    double dec_s=now()-t0;

    double raw=(double)FSZ*(F-1);
    printf("level %d:\n", lvl);
    printf("  raw/frame            %d B\n", FSZ);
    printf("  no-dict              %.0f B/frame  (%.1fx)   %.1fM frames/s  %.1f GB/s\n",
           (double)tot_nodict/F, raw/tot_nodict, F/nod_s/1e6, raw/nod_s/1e9);
    printf("  last-frame dict      %.0f B/frame  (%.1fx)   %.1fM frames/s  %.1f GB/s in\n",
           (double)tot_dict/(F-1), raw/tot_dict, (F-1)/dict_s/1e6, raw/dict_s/1e9);
    printf("  decompress                                    %.1fM frames/s\n", (F-1)/dec_s/1e6);
  }
  return 0;
}
