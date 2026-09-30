/* Real bounded native framing + AF_UNIX adapter IPC; private paths only.
 * Compile with -DESPHOMED_SOURCE='"/absolute/path/esphomed.c"' for a snapshot.
 * Normal runner default after landing: ../src/adapter/esphomed.c. */
#define _POSIX_C_SOURCE 200809L
#define main esphomed_main
#ifndef ESPHOMED_SOURCE
#define ESPHOMED_SOURCE "../src/adapter/esphomed.c"
#endif
#include ESPHOMED_SOURCE
#undef main
#include <assert.h>

static int listener,peers[32],voice_peer;
static unsigned peer_count,clears,events;
static char requests[32][4096];static size_t lengths[32];
static unsigned old_count,new_count,max_peers=32;static int hold_ack;
static char path[108],status_path[256],missing[256];
static void reset(const char *dir){
 memset(&S,0,sizeof S);S.owner=0;S.now=1000;S.last_audio_status=S.now;S.ha_selected=S.active_wake=1;S.turn_limit=120000;S.privacy=0;
 for(unsigned i=0;i<CLIENTS;i++)S.clients[i].fd=-1;for(unsigned i=0;i<JOBS;i++)S.jobs[i].fd=-1;S.wake.fd=S.mic.fd=-1;S.mdns_fd=-1;esp_playback_init(&S.playback);
#ifdef JOB_TIMER
 S.timer_cleanup.fd=-1; /* Candidate adds this reserved IO slot; baseline has none. */
#endif
 snprintf(path,sizeof path,"%s/timer.sock",dir);snprintf(status_path,sizeof status_path,"%s/status.json",dir);snprintf(missing,sizeof missing,"%s/absent.sock",dir);
 S.timer_path=path;S.status_path=status_path;S.led_path=S.audio_path=S.wake_path=S.radio_path=missing;
 struct sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,path,strlen(path)+1);
 listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0);assert(!bind(listener,(struct sockaddr*)&a,sizeof a));assert(!listen(listener,32));
 /* Preserve the bounded wake reload deadline without relaxing ordinary IO. */
 struct io probe;memset(&probe,0,sizeof probe);probe.fd=-1;
 assert(!io_open(&probe,path,"status","{}",5));assert(probe.deadline==S.now+6000);io_close(&probe);
 assert(!io_open(&probe,path,"status","{}",0));assert(probe.deadline==S.now+500);io_close(&probe);
 int v[2];assert(!socketpair(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,v));S.clients[0].fd=v[0];voice_peer=v[1];S.clients[0].mode=0;S.clients[0].hello=1;S.clients[0].deadline=UINT64_MAX;
}
static void timer_server(void){
 for(;;){int fd=accept(listener,NULL,NULL);if(fd<0){assert(errno==EAGAIN||errno==EWOULDBLOCK);break;}unsigned active=0;for(unsigned i=0;i<peer_count;i++)active+=peers[i]>=0;if(active>=max_peers){close(fd);continue;}assert(peer_count<32);int flags=fcntl(fd,F_GETFL,0);assert(flags>=0&&!fcntl(fd,F_SETFL,flags|O_NONBLOCK));peers[peer_count++]=fd;}
 for(unsigned i=0;i<peer_count;i++){if(peers[i]<0)continue;ssize_t n=recv(peers[i],requests[i]+lengths[i],sizeof requests[i]-1-lengths[i],MSG_DONTWAIT);if(n>0){lengths[i]+=(size_t)n;requests[i][lengths[i]]=0;}else if(!n){close(peers[i]);peers[i]=-1;continue;}else assert(errno==EAGAIN||errno==EWOULDBLOCK);
 if(!memchr(requests[i],'\n',lengths[i]))continue;
 char cmd[64],id[128];assert(json_get_string(requests[i],"cmd",cmd,sizeof cmd)>0);
 if(!strcmp(cmd,"remote_event")){assert(json_get_string(requests[i],"timer_id",id,sizeof id)>0);if(hold_ack)continue;events++;if(!strncmp(id,"old-",4))old_count++;else new_count++;}
 else if(!strcmp(cmd,"remote_clear")){clears++;old_count=new_count=0;}else assert(!strcmp(cmd,"status"));
 const char reply[]="{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n";assert(send(peers[i],reply,sizeof reply-1,MSG_NOSIGNAL)==(ssize_t)sizeof reply-1);close(peers[i]);peers[i]=-1;
 }
}
static void ticks(unsigned count){for(unsigned n=0;n<count;n++){jobs_tick();if(listener>=0)timer_server();S.now+=5;}}
static void append(unsigned type,const void *p,size_t n){struct client *c=&S.clients[0];size_t z=ef_plain_encode(c->rx+c->rx_n,sizeof c->rx-c->rx_n,type,p,n);assert(z);c->rx_n+=z;}
static void add_timer(unsigned n,int old){unsigned char b[256];char id[64];struct ep_writer w={b,sizeof b,0,0};snprintf(id,sizeof id,"%s-%u",old?"old":"new",n);ep_uint(&w,1,0);ep_string(&w,2,id);ep_string(&w,3,"fixture");ep_uint(&w,4,60);ep_uint(&w,5,60);ep_uint(&w,6,1);append(115,b,w.length);}
static void burst(void){for(unsigned i=0;i<JOBS;i++)add_timer(i,1);assert(!process_rx(0));unsigned count=0;for(unsigned i=0;i<JOBS;i++)count+=S.jobs[i].fd>=0;assert(count==JOBS);}
static void unsubscribe(void){append(89,NULL,0);assert(!process_rx(0));assert(S.owner==-1);}
static void subscribe(void){unsigned char b[8];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,1);ep_uint(&w,2,4);append(89,b,w.length);assert(!process_rx(0));assert(S.owner==0);}
int main(int argc,char **argv){assert(argc==3);setbuf(stdout,NULL);reset(argv[2]);
 if(!strcmp(argv[1],"unsubscribe-full")){burst();unsubscribe();ticks(40);client_close(0);ticks(40);assert(clears>0&&"pool-full unsubscribe lost remote_clear");assert(old_count==0&&"retired owner events recreated timers after cleanup");}
 else if(!strcmp(argv[1],"disconnect-full")){burst();client_close(0);ticks(40);assert(clears>0&&"pool-full disconnect lost remote_clear");assert(old_count==0);}
 else if(!strcmp(argv[1],"reconnect-full")){burst();unsubscribe();ticks(40);subscribe();add_timer(0,0);assert(!process_rx(0));ticks(40);assert(clears>0&&"old owner cleanup must survive unsubscribe/reconnect");assert(old_count==0&&new_count==1&&"new owner timers must survive cleanup without old timers");}
 else if(!strcmp(argv[1],"reconnect-before-clear")){burst();unsubscribe();subscribe();add_timer(0,0);assert(!process_rx(0)&&"replacement-owner timer must fit after retiring unsent old jobs");ticks(40);assert(clears>0&&old_count==0&&new_count==1&&"cleanup must precede replacement-owner timer dispatch");}
 else if(!strcmp(argv[1],"sent-before-clear")){add_timer(0,1);assert(!process_rx(0));hold_ack=1;ticks(4);unsubscribe();ticks(4);assert(!clears&&"cleanup overtook submitted old-owner requests");hold_ack=0;ticks(40);assert(clears>0&&old_count==0&&"submitted old timers must settle before retriable cleanup");}
 else if(!strcmp(argv[1],"sent-full")){burst();hold_ack=1;ticks(4);unsubscribe();ticks(4);assert(!clears);hold_ack=0;ticks(40);assert(clears>0&&old_count==0&&"submitted full-pool jobs must settle before cleanup");}
 else if(!strcmp(argv[1],"other-jobs-full")){for(unsigned i=0;i<JOBS;i++)assert(!job(S.timer_path,"status","{}",0,0));unsubscribe();ticks(40);assert(clears>0&&"ownership cleanup needs a reserved path independent of general job capacity");}
 else if(!strcmp(argv[1],"cleanup-retry")){burst();unsubscribe();close(listener);listener=-1;unlink(path);ticks(40);struct sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,path,strlen(path)+1);listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0&&!bind(listener,(struct sockaddr*)&a,sizeof a)&&!listen(listener,32));ticks(400);assert(clears>0&&"cleanup must retry after adapter outage");assert(old_count==0);}
 else if(!strcmp(argv[1],"reconnect-adapter-capacity")){burst();unsubscribe();close(listener);listener=-1;unlink(path);ticks(40);struct sockaddr_un a;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,path,strlen(path)+1);listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);assert(listener>=0&&!bind(listener,(struct sockaddr*)&a,sizeof a)&&!listen(listener,32));max_peers=4;subscribe();for(unsigned i=0;i<4;i++)add_timer(i,0);assert(!process_rx(0));ticks(400);assert(clears>0&&"queued replacement-owner sockets must not starve reserved cleanup at timerd's four-client limit");assert(old_count==0&&new_count==4&&"replacement timers must not expire while cleanup retries");}
 else assert(!"unknown test");
 for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0)client_close(i);close(voice_peer);for(unsigned i=0;i<JOBS;i++)io_close(&S.jobs[i]);for(unsigned i=0;i<peer_count;i++)if(peers[i]>=0)close(peers[i]);close(listener);unlink(path);unlink(status_path);esp_playback_close(&S.playback);printf("%s: PASS (events=%u clears=%u old=%u new=%u)\n",argv[1],events,clears,old_count,new_count);return 0;
}
