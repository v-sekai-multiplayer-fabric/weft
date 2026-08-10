// Native hot-path packet benchmark: decode a movement datagram and apply it to an
// entity slab, measuring packets/sec (the real ">15M pps" metric). No network:
// this isolates the DECODE+APPLY compute cost, so we can see whether 15M pps is
// compute-bound (it is not) or I/O-bound (it is -> AF_XDP/DPDK/SmartNIC).
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

typedef struct __attribute__((packed)) {
  uint32_t id; int32_t x,y,z; int16_t qx,qy,qz,qw;   // 24-byte player movement pkt
} pkt_t;
typedef struct { int32_t x,y,z; int16_t qx,qy,qz,qw; } ent_t;

#define MAX_ENTITIES (1<<20)      // 1M entities/core
#define BATCH 4096
#define N 1000000000L             // 1e9 packets/core

static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

static void* work(void* _){
  static __thread ent_t world[MAX_ENTITIES];
  pkt_t buf[BATCH];
  for(int i=0;i<BATCH;i++){ buf[i]=(pkt_t){i,i,i,i,0,0,0,1}; }
  volatile int64_t sink=0;
  for(long p=0;p<N;p++){
    pkt_t* pk=&buf[p&(BATCH-1)];
    ent_t* e=&world[pk->id&(MAX_ENTITIES-1)];
    e->x=pk->x; e->y=pk->y; e->z=pk->z; e->qx=pk->qx; e->qy=pk->qy; e->qz=pk->qz; e->qw=pk->qw;
    sink+=e->x;
  }
  (void)sink; return 0;
}

int main(int argc,char**argv){
  int T = argc>1 ? atoi(argv[1]) : 1;
  pthread_t th[64];
  double t0=now();
  for(int i=0;i<T;i++) pthread_create(&th[i],0,work,0);
  for(int i=0;i<T;i++) pthread_join(th[i],0);
  double s=now()-t0;
  double pps = (double)N*T/s;
  printf("%2d core(s): %6.0fM pps  (%.2f ns/pkt/core, %.1f GB/s in @24B)\n",
         T, pps/1e6, s/N*1e9, pps*24/1e9);
  return 0;
}
