// DRAM-bound apply: random writes into an entity table far larger than cache, to
// find the real hot-path ceiling for large worlds (vs the cache-hot pps_native).
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

typedef struct { int32_t x,y,z; int16_t qx,qy,qz,qw; uint8_t pad[12]; } ent_t; // 32B
#define N_ENT (64UL*1024*1024)   // 64M * 32B = 2 GB, >> any LLC
#define ITERS 100000000L         // 100M ops/thread

static ent_t* world;
static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

static void* work(void* arg){
  uint64_t s = (uint64_t)(long)arg * 2654435761u + 1;
  for(long i=0;i<ITERS;i++){
    s ^= s<<13; s ^= s>>7; s ^= s<<17;          // xorshift64 -> random index
    ent_t* e = &world[s % N_ENT];
    e->x=(int32_t)i; e->y=(int32_t)s; e->z=(int32_t)i; e->qw=1;
  }
  return 0;
}

int main(int argc,char**argv){
  int T = argc>1?atoi(argv[1]):1;
  world = calloc(N_ENT, sizeof(ent_t));
  if(!world){ perror("calloc"); return 1; }
  for(uint64_t i=0;i<N_ENT;i+=64/sizeof(ent_t)) world[i].qw=0;   // fault pages in
  pthread_t th[64]; double t0=now();
  for(int i=0;i<T;i++) pthread_create(&th[i],0,work,(void*)(long)(i+1));
  for(int i=0;i<T;i++) pthread_join(th[i],0);
  double sec=now()-t0, pps=(double)ITERS*T/sec;
  printf("%2d core(s): %6.1fM pps random apply into %luMB world (%.1f ns/op/core)\n",
         T, pps/1e6, (N_ENT*sizeof(ent_t))>>20, sec/ITERS*1e9);
  return 0;
}
