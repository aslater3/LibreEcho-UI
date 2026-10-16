/* Idle HA connection regressions (Radar pink flash every 120 s).
 * 1. A quiet but healthy owner must be pinged before the idle deadline and
 *    survive once it answers: aioesphomeapi only pings when it has received
 *    nothing, and esphomed streams state, so HA never pings on its own.
 * 2. Losing the owner while no voice turn is active must not flash the
 *    error colour on the ring. */
#define _POSIX_C_SOURCE 200809L
#define main esphomed_main
#ifndef ESPHOMED_SOURCE
#define ESPHOMED_SOURCE "../src/adapter/esphomed.c"
#endif
#include ESPHOMED_SOURCE
#undef main
#include <assert.h>

static int led_listener,peer;
static char led_path[108],status_path[256],missing[256];
static void setup(const char *dir){
 memset(&S,0,sizeof S);S.owner=0;S.now=1000;S.ha_selected=S.active_wake=1;S.turn_limit=120000;S.privacy=0;
 for(unsigned i=0;i<CLIENTS;i++)S.clients[i].fd=-1;for(unsigned i=0;i<JOBS;i++)S.jobs[i].fd=-1;S.wake.fd=S.mic.fd=-1;S.mdns_fd=-1;esp_playback_init(&S.playback);
#ifdef JOB_TIMER
 S.timer_cleanup.fd=-1;
#endif
 snprintf(led_path,sizeof led_path,"%s/led.sock",dir);snprintf(status_path,sizeof status_path,"%s/status.json",dir);snprintf(missing,sizeof missing,"%s/absent.sock",dir);
 S.led_path=led_path;S.status_path=status_path;S.timer_path=S.audio_path=S.wake_path=S.radio_path=missing;
 struct sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,led_path,strlen(led_path)+1);
 led_listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(led_listener>=0&&!bind(led_listener,(struct sockaddr*)&a,sizeof a)&&!listen(led_listener,16));
 int v[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,v));peer=v[1];
 struct client *c=&S.clients[0];c->fd=v[0];c->mode=0;c->hello=1;c->last_seen=S.now;c->deadline=S.now+120000;
}
/* Collect every LED request esphomed sent; return 1 if any used the error profile. */
static int led_error_requests(void){int seen=0;
 for(;;){int fd=accept(led_listener,NULL,NULL);if(fd<0)break;char b[2048];ssize_t n=recv(fd,b,sizeof b-1,MSG_DONTWAIT);if(n>0){b[n]=0;if(strstr(b,"\"profile\":\"error\""))seen=1;}close(fd);}
 return seen;}
static void pump(void){for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].fd>=0){(void)io_tick(&S.jobs[i]);}}
/* Advance time by ms in 1 s steps, running the client tick with no inbound data. */
static void idle(uint64_t ms){for(uint64_t t=0;t<ms&&S.clients[0].fd>=0;t+=1000){S.now+=1000;client_tick(0,0);}}
/* Read what esphomed sent to the peer; return 1 if it contains a PingRequest (type 7). */
static int peer_saw_ping(void){unsigned char b[4096];ssize_t n=recv(peer,b,sizeof b,MSG_DONTWAIT);if(n<=0)return 0;
 for(ssize_t i=0;i+2<n;i++)if(b[i]==0&&b[i+1]==0&&b[i+2]==7)return 1;return 0;}
static void peer_pong(void){unsigned char f[8];size_t z=ef_plain_encode(f,sizeof f,8,NULL,0);assert(z);assert(send(peer,f,z,MSG_NOSIGNAL)==(ssize_t)z);S.now+=10;client_tick(0,POLLIN);}
int main(int argc,char **argv){assert(argc==3);setbuf(stdout,NULL);setup(argv[2]);
 if(!strcmp(argv[1],"quiet-owner-pinged")){
  idle(90000);assert(S.clients[0].fd>=0&&"healthy owner closed before any ping");
  assert(peer_saw_ping()&&"esphomed must ping a quiet owner before its idle deadline");
  peer_pong();idle(90000);assert(S.clients[0].fd>=0&&"owner that answered the ping must not be dropped");
  assert(S.owner==0);
 }else if(!strcmp(argv[1],"silent-owner-dropped")){
  idle(200000);assert(S.clients[0].fd<0&&"an owner that never answers must still time out");
 }else if(!strcmp(argv[1],"idle-disconnect-no-error-flash")){
  client_close(0);pump();assert(!led_error_requests()&&"no active turn: disconnect must not flash the error colour");
 }else if(!strcmp(argv[1],"active-turn-disconnect-flashes")){
  S.turn=2;client_close(0);pump();assert(led_error_requests()&&"an interrupted voice turn must still show the error flash");
 }else assert(!"unknown test");
 if(S.clients[0].fd>=0)client_close(0);for(unsigned i=0;i<JOBS;i++)io_close(&S.jobs[i]);close(peer);close(led_listener);unlink(led_path);unlink(status_path);esp_playback_close(&S.playback);
 printf("%s: PASS\n",argv[1]);return 0;}
