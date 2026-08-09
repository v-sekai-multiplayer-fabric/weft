#include <stdint.h>
#include <stdio.h>
#include <time.h>
#define E 8
#define SLOTS (2 + E*3)
static volatile int64_t ring[SLOTS];
static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }
int main(){
  const long N = 500000000L; // 500M snapshots, 8 entities each
  double t0 = now();
  for(long tick=1; tick<=N; tick++){
    int64_t gen = ring[0];
    ring[0] = gen+1;                     // seqlock: odd
    ring[1] = tick;
    for(int i=0;i<E*3;i++) ring[2+i] = tick + i;
    ring[0] = gen+2;                     // seqlock: even
  }
  double s = now()-t0;
  printf("native C ring (1 core): %.1fM snapshots/sec  (%.2f ns/snapshot)\n", N/s/1e6, s/N*1e9);
  return 0;
}
