// UDP packet-rate benchmark for real NICs (e.g. two Fly machines over 6PN).
//   netbench server [port]            -> receives, prints pps once per second
//   netbench client <host> [port] [threads]
// Measures the real kernel receive ceiling that loopback cannot.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

#define PAYLOAD 24
#define BATCH 1024

static double now(){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

static int mksock(const char* host, const char* port, int server, struct addrinfo** out){
  struct addrinfo hints={0}, *res;
  hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM;
  if(server) hints.ai_flags=AI_PASSIVE;
  if(getaddrinfo(host, port, &hints, &res)){ perror("getaddrinfo"); exit(1); }
  int s=socket(res->ai_family,res->ai_socktype,res->ai_protocol);
  if(s<0){ perror("socket"); exit(1); }
  int rb=64*1024*1024; setsockopt(s,SOL_SOCKET,SO_RCVBUF,&rb,sizeof(rb));
  setsockopt(s,SOL_SOCKET,SO_SNDBUF,&rb,sizeof(rb));
  if(server){ if(bind(s,res->ai_addr,res->ai_addrlen)){perror("bind");exit(1);} freeaddrinfo(res); }
  else { *out=res; }
  return s;
}

static void run_server(const char* port){
  int s=mksock(0,port,1,0);
  char bufs[BATCH][PAYLOAD]; struct mmsghdr m[BATCH]; struct iovec iov[BATCH];
  for(int i=0;i<BATCH;i++){iov[i].iov_base=bufs[i];iov[i].iov_len=PAYLOAD;memset(&m[i],0,sizeof(m[i]));m[i].msg_hdr.msg_iov=&iov[i];m[i].msg_hdr.msg_iovlen=1;}
  printf("server listening on :%s\n",port); fflush(stdout);
  long count=0; double t0=now();
  for(;;){
    int n=recvmmsg(s,m,BATCH,0,0); if(n>0) count+=n;
    double t=now(); if(t-t0>=1.0){ printf("recv %.2fM pps\n",count/(t-t0)/1e6); fflush(stdout); count=0; t0=t; }
  }
}

static const char* g_host; static const char* g_port;
static void* blaster(void* _){
  struct addrinfo* res; int s=mksock(g_host,g_port,0,&res);
  connect(s,res->ai_addr,res->ai_addrlen);
  char buf[PAYLOAD]={0}; struct mmsghdr m[BATCH]; struct iovec iov[BATCH];
  for(int i=0;i<BATCH;i++){iov[i].iov_base=buf;iov[i].iov_len=PAYLOAD;memset(&m[i],0,sizeof(m[i]));m[i].msg_hdr.msg_iov=&iov[i];m[i].msg_hdr.msg_iovlen=1;}
  for(;;) sendmmsg(s,m,BATCH,0);
  return 0;
}

int main(int argc,char**argv){
  if(argc>=2 && !strcmp(argv[1],"server")){ run_server(argc>2?argv[2]:"9999"); return 0; }
  if(argc>=3 && !strcmp(argv[1],"client")){
    g_host=argv[2]; g_port=argc>3?argv[3]:"9999"; int T=argc>4?atoi(argv[4]):1;
    printf("client -> [%s]:%s x%d threads\n",g_host,g_port,T); fflush(stdout);
    pthread_t th[64]; for(int i=0;i<T;i++) pthread_create(&th[i],0,blaster,0);
    for(int i=0;i<T;i++) pthread_join(th[i],0); return 0;
  }
  fprintf(stderr,"usage: netbench server [port] | client <host> [port] [threads]\n"); return 1;
}
