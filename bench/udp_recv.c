// Kernel UDP receive-throughput benchmark: a saturating sender blasts 24-byte
// datagrams to a loopback socket; the receiver drains with recv() vs recvmmsg().
// Locates the "standard sockets" I/O ceiling relative to the >15M pps goal.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 44544
#define PAYLOAD 24
#define BATCH 1024
#define DURATION 3.0

static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static volatile int stop_flag = 0;

static void* sender(void* _){
  int s = socket(AF_INET,SOCK_DGRAM,0);
  struct sockaddr_in a = {0}; a.sin_family=AF_INET; a.sin_port=htons(PORT);
  inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
  connect(s,(struct sockaddr*)&a,sizeof(a));
  static char buf[PAYLOAD];
  struct mmsghdr msgs[BATCH]; struct iovec iov[BATCH];
  for(int i=0;i<BATCH;i++){ iov[i].iov_base=buf; iov[i].iov_len=PAYLOAD;
    memset(&msgs[i],0,sizeof(msgs[i])); msgs[i].msg_hdr.msg_iov=&iov[i]; msgs[i].msg_hdr.msg_iovlen=1; }
  while(!stop_flag) sendmmsg(s,msgs,BATCH,0);
  close(s); return 0;
}

static double recv_bench(int use_batch){
  int s = socket(AF_INET,SOCK_DGRAM,0);
  int rb = 64*1024*1024; setsockopt(s,SOL_SOCKET,SO_RCVBUF,&rb,sizeof(rb));
  struct sockaddr_in a = {0}; a.sin_family=AF_INET; a.sin_port=htons(PORT); a.sin_addr.s_addr=htonl(INADDR_ANY);
  bind(s,(struct sockaddr*)&a,sizeof(a));
  stop_flag=0; pthread_t th; pthread_create(&th,0,sender,0);
  char bufs[BATCH][PAYLOAD];
  struct mmsghdr msgs[BATCH]; struct iovec iov[BATCH];
  for(int i=0;i<BATCH;i++){ iov[i].iov_base=bufs[i]; iov[i].iov_len=PAYLOAD;
    memset(&msgs[i],0,sizeof(msgs[i])); msgs[i].msg_hdr.msg_iov=&iov[i]; msgs[i].msg_hdr.msg_iovlen=1; }
  long count=0; double t0=now(), t1=t0;
  while((t1=now())-t0 < DURATION){
    if(use_batch){ int n=recvmmsg(s,msgs,BATCH,0,0); if(n>0) count+=n; }
    else { char b[PAYLOAD]; if(recv(s,b,PAYLOAD,0)>0) count++; }
  }
  stop_flag=1; pthread_join(th,0); close(s);
  return count/(t1-t0);
}

int main(){
  printf("UDP loopback receive (1 receiver core):\n");
  printf("  recv() one-at-a-time:  %7.2fM pps\n", recv_bench(0)/1e6);
  printf("  recvmmsg(1024) batch:  %7.2fM pps\n", recv_bench(1)/1e6);
  return 0;
}
