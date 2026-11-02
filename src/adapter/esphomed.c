/* LibreEcho finite ESPHome satellite, API 1.14 / aioesphomeapi 46.2.0.
 * One poll owner. Fixed queues; no fork, thread, blocking DNS or socket reads.
 * Native API capabilities: VA/API_AUDIO/TIMERS/ANNOUNCE/START_CONVERSATION,
 * deliberately NOT SPEAKER (HA must deliver finite URLs, not streamed PCM). */
#define _POSIX_C_SOURCE 200809L
#include "esphome_proto.h"
#include "esphome_frame.h"
#include "esphome_noise.h"
#include "esphome_playback.h"
#include "mdns_client.h"
#include "voice_ns.h"
#include "adapter.h"
#include "../config_store.h"
#include "../json.h"
#include "../esphome_health.h"
#include "../timezone.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>
#define CLIENTS 2
#define TX_CAP (256U*1024U)
#define RX_CAP (EF_MAX_FRAME+16U)
#define JOBS 8
#define JOB_TIMER 7
#define JOB_TIMER_RETIRED 8
#define RING_SAMPLES 48000U
#define MEDIA_KEY 1U
#define MUTE_KEY 2U
/* "Cancel request" button (#404); distinct from small sequential keys. */
#define CANCEL_KEY 404U
/* Separate reply volume (#409): a 0-100 % config number in Home Assistant. */
#define REPLY_KEY 3U
#define JOB_REPLY 9
/* Ambient light (#392): the board's TSL2540 lux as an HA illuminance sensor.
 * Read at most once a second, published on a real change (5% or 1 lx) or,
 * for smaller drift, at most once a minute -- never more than ~1/s per client. */
#define LIGHT_KEY 3U
#define LIGHT_POLL_MS 1000U
#define LIGHT_REFRESH_MS 60000U
/* Room level (#399): dBFS of the 16 kHz stream esphomed already reads for the ring. Each 1 s window is a
 * LEQ; a value is published at most every LEVEL_MIN_MS, on a LEVEL_STEP_DB change or each LEVEL_REFRESH_MS. */
#define LEVEL_KEY 73U
#define LEVEL_WINDOW_SAMPLES 16000U
#define LEVEL_STALE_MS 3000U
#define LEVEL_MIN_MS 5000U
#define LEVEL_REFRESH_MS 60000U
#define LEVEL_STEP_DB 2.0
#define LEVEL_FLOOR_DBFS -90.0
/* Hardware buttons as Home Assistant event entities (#394). buttond sends one
 * datagram per gesture, "<button> <gesture>", to the button socket. */
#define BUTTON_KEY_BASE 10U
#define BUTTON_COUNT 4U
#define GESTURE_COUNT 3U
#define BUTTON_MSG_MAX 40U
#define BUTTON_BATCH 8U
#define EVENT_LIST_TYPE 107U
#define EVENT_STATE_TYPE 108U
/* Device activity as event entities (#393): voice steps, playback and the
 * timer events Home Assistant sends. Event types only -- never transcript,
 * TTS/media URL or timer text. */
#define ACTIVITY_VOICE_KEY 20U
#define ACTIVITY_PLAYBACK_KEY 21U
#define ACTIVITY_TIMER_KEY 22U
/* LED ring as a light (#395): Home Assistant's state is ledd's owner-scoped overlay. */
#define LED_RING_KEY 30U
#define LED_OWNER "esphome-light"
/* Settings as HA entities (#396): wake word on/off is a switch; volume, wake sensitivity and
 * LED brightness are integer 0-100 numbers. Config category. Values are read back from the
 * owning daemons every SETTINGS_POLL_MS so changes made in the web UI reach HA too. */
#define WAKE_SWITCH_KEY 43U
#define VOLUME_NUMBER_KEY 40U
#define SENSITIVITY_NUMBER_KEY 41U
#define LED_BRIGHTNESS_KEY 42U
#define SETTINGS_POLL_MS 2000U
/* Last-turn text sensors (#398): the last wake word (waked's model name) is always listed; the last
 * transcript and reply carry speech, so they are listed and published only when the owner sets
 * esphome_speech_text_sensors. All three clear when the voice history is cleared (agentd's generation). */
#define TEXT_WAKE_KEY 70U
#define TEXT_TRANSCRIPT_KEY 71U
#define TEXT_REPLY_KEY 72U
#define TEXT_LIST_TYPE 18U
#define TEXT_STATE_TYPE 27U
#define TEXT_VALUE_MAX 1024U
#define HISTORY_GENERATION_KIND 13
#define NELEM(a) (sizeof(a)/sizeof((a)[0]))
struct client { int fd,mode,phase,hello,states,closing,light,led_light,text_speech,level;unsigned char rx[RX_CAP],tx[TX_CAP];size_t rx_n,tx_n,tx_sent;uint64_t deadline,last_seen,ping_at,time_at;struct en_session noise; };
struct io {int fd,stage,kind;char tx[2048],rx[4096];size_t tx_n,tx_sent,rx_n;uint64_t deadline;int target;};
struct state {
 struct client clients[CLIENTS];struct io jobs[JOBS],wake,mic,timer_cleanup;
 const char *config_path,*status_path,*control_path,*audio_path,*wake_path,*radio_path,*timer_path,*led_path,*mdns_path,*bus_path,*privacy_path,*ca_path,*idme_path;
 /* Optional room from web-config (#64), offered to Home Assistant as the suggested area when it adds this device. */
 char room[49];
 /* Light sensor file (--light-sensor, else the first readable TSL2540 als_lux); empty until found. */
 char light_path[160];int lux,lux_sent;uint64_t light_poll,light_sent_at;
 int button_fd;char button_path[160];
 /* HA light state for the LED ring (#395); the device's own indicators are never stored here. */
 int led_on,led_r,led_g,led_b,led_brightness,led_effect,led_dirty;
 /* Settings entities (#396): ledd global brightness and waked sensitivity, read back by status poll. */
 int led_master,led_master_known,sensitivity,sensitivity_known;uint64_t settings_poll;
 int set_sent_known[4],set_sent_value[4];
 char name[32],friendly[64],mac[18];unsigned char psk[32];int provisioned,plaintext,ha_selected,ready,owner,turn,muted,local_mute_gate,privacy,volume,output_muted,restore_volume,radio_state,active_wake,reply_volume,voice_open,server_wake,server_turn;
 uint64_t cancel_guard,control_retry;int control_fd;
 uint64_t now,server_retry,turn_deadline,turn_limit,last_audio_status,last_status_poll,wake_retry,mdns_retry,ring_first,ring_end,capture_next,timer_retry;
 int16_t ring[RING_SAMPLES];unsigned char mic_rx[24+2560];size_t mic_n;int mic_ack;
 /* Optional noise suppression on the speech sent to Home Assistant (#401), off unless web-config says voice_noise_suppression. It runs on every frame so the noise estimate is settled when a turn starts. */
 int noise_suppression;struct le_voice_ns ns;
 char conversation[128],tts_url[1024],next_url[1024],result[24];int continue_turn,run_end,early_tts,playing,announcement,pipeline,tts_done,timer_pending;
 time_t result_time;struct esp_playback playback;int mdns_fd;
 char process_start[32],boot_id[64],listener_inode[32];unsigned listener_port;
 /* set_ring (#397): when its ledd overlay ends; 0 when none is running. */
 uint64_t action_ring_end;
 /* Last-turn text sensors (#398) and the voice-history generation they clear on. */
 char last_wake[TEXT_VALUE_MAX],last_transcript[TEXT_VALUE_MAX],last_reply[TEXT_VALUE_MAX];int speech_text,history_known;long long history_generation;
 /* Room level sensor (#399), opt-in through esphome_room_level_sensor; the LEQ window and what was last published. */
 int room_level_sensor,level_known,level_sent_ok;float level_db,level_sent_db;double level_sum;unsigned level_n;uint64_t level_at,level_sent_at;
 const char *agent_path;
};
static struct state S;static volatile sig_atomic_t running=1,reload_config;
static void sigstop(int sig){if(sig==SIGHUP)reload_config=1;else running=0;}
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static void io_close(struct io *j){if(j->fd>=0)close(j->fd);j->fd=-1;j->stage=0;j->rx_n=j->tx_n=j->tx_sent=0;}
static int io_connect(struct io *j,const char *path){struct sockaddr_un a;
 if(j->fd>=0||strlen(path)>=sizeof a.sun_path)return -1;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,path,strlen(path)+1);
 j->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(j->fd<0)return -1;
 if(connect(j->fd,(struct sockaddr*)&a,sizeof a)<0&&errno!=EINPROGRESS){io_close(j);return -1;}
 return 0;
}
static int io_open(struct io *j,const char *path,const char *cmd,const char *args,int kind){int n;if(j->fd>=0||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return -1;
 n=snprintf(j->tx,sizeof j->tx,"{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":%s}\n",cmd,args);if(n<0||(size_t)n>=sizeof j->tx)return -1;
 int defer=kind==JOB_TIMER&&S.timer_pending;if(!defer&&io_connect(j,path)<0)return -1;j->tx_n=(size_t)n;j->tx_sent=j->rx_n=0;j->deadline=S.now+(kind==5?6000:500);j->stage=defer?3:1;j->kind=kind;return 0;
}
static int job(const char *path,const char *cmd,const char *args,int kind,int target){for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].fd<0&&!S.jobs[i].stage){if(io_open(&S.jobs[i],path,cmd,args,kind)<0)return -1;S.jobs[i].target=target;return 0;}return -1;}
/* Returns one complete response line; remaining bytes stay buffered for LVS1. */
static int io_tick(struct io *j){ssize_t n;if(j->fd<0)return -1;if(S.now>j->deadline)return -1;
 if(j->stage==1){n=send(j->fd,j->tx+j->tx_sent,j->tx_n-j->tx_sent,MSG_DONTWAIT|MSG_NOSIGNAL);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;if(n<=0)return -1;j->tx_sent+=(size_t)n;if(j->tx_sent<j->tx_n)return 0;j->stage=2;}
 char *end=memchr(j->rx,'\n',j->rx_n);if(end)return 1;
 if(j->rx_n>=sizeof j->rx-1)return -1;n=recv(j->fd,j->rx+j->rx_n,sizeof j->rx-1-j->rx_n,MSG_DONTWAIT);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;if(n<=0)return -1;j->rx_n+=(size_t)n;j->rx[j->rx_n]=0;return memchr(j->rx,'\n',j->rx_n)?1:0;
}
/* Retire unsent old-owner events. Submitted transactions settle before clear;
 * replacement-owner events stay queued without sockets until cleanup is acked
 * (timerd has only four clients; idle queued connections could starve clear). */
static void timer_owner_end(void){S.timer_pending=1;S.timer_retry=S.now;for(unsigned i=0;i<JOBS;i++){struct io *j=&S.jobs[i];if((j->fd>=0||j->stage==3)&&j->kind==JOB_TIMER){if(!j->tx_sent)io_close(j);else j->kind=JOB_TIMER_RETIRED;}}}
static void timer_cleanup_tick(void){if(!S.timer_pending)return;for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].fd>=0&&S.jobs[i].kind==JOB_TIMER_RETIRED)return;
 struct io *j=&S.timer_cleanup;if(j->fd<0){if(S.now<S.timer_retry)return;if(io_open(j,S.timer_path,"remote_clear","{}",0)<0){S.timer_retry=S.now+1000;return;}}
 int rc=io_tick(j);if(!rc)return;int ok=0;if(rc>0){char *end=memchr(j->rx,'\n',j->rx_n);*end=0;(void)json_get_bool(j->rx,"ok",&ok);}io_close(j);if(ok)S.timer_pending=0;else S.timer_retry=S.now+1000;
}
static int queued(struct client *c,const unsigned char *p,size_t n){if(c->tx_sent){memmove(c->tx,c->tx+c->tx_sent,c->tx_n-c->tx_sent);c->tx_n-=c->tx_sent;c->tx_sent=0;}if(n>sizeof c->tx-c->tx_n)return -1;memcpy(c->tx+c->tx_n,p,n);c->tx_n+=n;return 0;}
static int outer(struct client *c,const unsigned char *p,size_t n){unsigned char h[3]={1,(unsigned char)(n>>8),(unsigned char)n};if(n>65535||n+3>sizeof c->tx-c->tx_n)return -1;return queued(c,h,3)||queued(c,p,n)?-1:0;}
static int send_msg(struct client *c,unsigned type,const void *p,size_t n){unsigned char buf[RX_CAP],plain[RX_CAP];size_t z;
 if(c->fd<0||c->closing)return -1;
 if(c->mode==1){if(n>65515)return -1;plain[0]=(unsigned char)(type>>8);plain[1]=(unsigned char)type;plain[2]=(unsigned char)(n>>8);plain[3]=(unsigned char)n;if(n)memcpy(plain+4,p,n);if(en_encrypt(&c->noise,plain,n+4,buf,sizeof buf,&z))return -1;int rc=outer(c,buf,z);if(rc)c->closing=1;return rc;}
 z=ef_plain_encode(buf,sizeof buf,type,p,n);int rc=z?queued(c,buf,z):-1;if(rc)c->closing=1;return rc;
}
static int send_writer(struct client *c,unsigned t,struct ep_writer *w){return w->error?-1:send_msg(c,t,w->data,w->length);}
static void status_write(void){char b[512],tmp[512];int connected=0,fd,n;for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].hello)connected=1;
 n=snprintf(b,sizeof b,"{\"pid\":%ld,\"start_time\":\"%s\",\"boot_id\":\"%s\",\"listener_inode\":\"%s\",\"port\":%u,\"ready\":%s,\"connected\":%s,\"in_progress\":%s,\"server_wake\":%s,\"server_wake_streaming\":%s,\"last_result\":\"%s\",\"last_result_time\":%lld}\n",(long)getpid(),S.process_start,S.boot_id,S.listener_inode,S.listener_port,S.ready?"true":"false",connected?"true":"false",S.turn?"true":"false",S.server_wake?"true":"false",S.server_turn&&S.turn==2?"true":"false",S.result,(long long)S.result_time);
 if(n<0||(size_t)n>=sizeof b||snprintf(tmp,sizeof tmp,"%s.tmp",S.status_path)>=(int)sizeof tmp)return;
 fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return;if(write(fd,b,(size_t)n)==n){close(fd);if(rename(tmp,S.status_path))unlink(tmp);}else{close(fd);unlink(tmp);}
}
static int mic_muted(void){return S.local_mute_gate||S.muted||S.privacy!=0||!S.last_audio_status||S.now-S.last_audio_status>1500;}
/* Turn indicators use ledd's owner-scoped pattern layer, never "animate":
 * animate persists the theme as the ring's saved colour and breathes until
 * another command replaces it, and nothing ever ended it -- a failed turn left
 * the ring pulsing the error colour indefinitely. Colours are ledd's default
 * themes; brightness follows the user's theme. Errors flash briefly and expire;
 * idle releases only this owner, leaving the saved ring settings untouched. */
static void profile(const char *name){char a[192];if(!strcmp(name,"idle")){(void)job(S.led_path,"pattern","{\"name\":\"stop\",\"owner\":\"esphome\"}",0,0);return;}
 int error=!strcmp(name,"error");unsigned r=72,g=185,b=255;if(!strcmp(name,"thinking")){r=168;g=115;b=239;}else if(error){r=239;g=80;b=80;}
 snprintf(a,sizeof a,"{\"name\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":100,\"repeats\":%d,\"profile\":\"%s\",\"owner\":\"esphome\"}",error?"flash":"pulse",r,g,b,error?3:0,name);(void)job(S.led_path,"pattern",a,0,0);}
static void light_state(struct client *c){unsigned char b[16];struct ep_writer w={b,sizeof b,0,0};float f=(float)(S.lux<0?0:S.lux);uint32_t bits;memcpy(&bits,&f,4);ep_fixed32(&w,1,LIGHT_KEY);ep_fixed32(&w,2,bits);if(S.lux<0)ep_uint(&w,3,1);(void)send_writer(c,25,&w);}
/* Room level (#399): a level window is computed from the stream frames ring_frame() already receives, so there
 * is no second capture or subscriber. Muted, stale or disabled input reads as missing; only the number leaves. */
static int level_available(void){return S.room_level_sensor&&S.level_known&&S.now-S.level_at<=LEVEL_STALE_MS&&!mic_muted();}
static void level_state(struct client *c){unsigned char b[16];struct ep_writer w={b,sizeof b,0,0};int ok=level_available();float f=ok?S.level_db:0.0f;uint32_t bits;memcpy(&bits,&f,4);ep_fixed32(&w,1,LEVEL_KEY);ep_fixed32(&w,2,bits);if(!ok)ep_uint(&w,3,1);(void)send_writer(c,25,&w);}
static void level_frame(const unsigned char *pcm,unsigned count){
 if(mic_muted()){S.level_sum=0;S.level_n=0;return;}
 for(unsigned i=0;i<count;i++){double v=(double)(int16_t)((unsigned)pcm[2*i]|((unsigned)pcm[2*i+1]<<8));S.level_sum+=v*v;}
 S.level_n+=count;if(S.level_n<LEVEL_WINDOW_SAMPLES)return;
 {double ms=S.level_sum/(double)S.level_n/(32768.0*32768.0);double db=ms>0.0?10.0*log10(ms):LEVEL_FLOOR_DBFS;if(db<LEVEL_FLOOR_DBFS)db=LEVEL_FLOOR_DBFS;S.level_db=(float)db;S.level_known=1;S.level_at=S.now;}
 S.level_sum=0;S.level_n=0;
}
static void level_tick(void){
 int ok=level_available();
 if(!ok&&!S.level_sent_ok)return;
 if(S.now<S.level_sent_at+LEVEL_MIN_MS)return;
 if(ok&&S.level_sent_ok&&fabs((double)S.level_db-(double)S.level_sent_db)<LEVEL_STEP_DB&&S.now<S.level_sent_at+LEVEL_REFRESH_MS)return;
 S.level_sent_ok=ok;S.level_sent_db=S.level_db;S.level_sent_at=S.now;
 for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].states&&S.clients[i].level)level_state(&S.clients[i]);
}
/* LED ring as a Home Assistant light (#395). HA's state is one ledd overlay,
 * owner "esphome-light" (solid, pulse or flash, repeats 0). Device voice
 * indicators use owner "esphome"; ledd keeps the overlay as their previous layer
 * and restores it when they stop. A HA change while a turn is open, or while an
 * earlier overlay write is unacknowledged, waits (led_dirty) and is applied by
 * led_tick once the turn has ended, so device animations always win. */
static const char *const led_effects[]={"Solid","Pulse","Flash"};
static const char *const led_ledd_names[]={"solid","pulse","flash"};
static int led_busy(void){return S.turn||S.voice_open;}
static void led_apply(void){char a[192];S.led_dirty=0;
 if(!S.led_on){(void)job(S.led_path,"pattern","{\"name\":\"stop\",\"owner\":\"" LED_OWNER "\"}",0,0);return;}
 snprintf(a,sizeof a,"{\"name\":\"%s\",\"r\":%d,\"g\":%d,\"b\":%d,\"brightness\":%d,\"repeats\":0,\"owner\":\"" LED_OWNER "\"}",led_ledd_names[S.led_effect],S.led_r,S.led_g,S.led_b,S.led_brightness);
 (void)job(S.led_path,"pattern",a,0,0);}
/* Overlay writes are serialised: ledd sees the device's stop/flash before HA's
 * write, because the next write is only sent once the previous one is acknowledged. */
static int led_job_pending(void){for(unsigned i=0;i<JOBS;i++){struct io *j=&S.jobs[i];if((j->fd>=0||j->stage)&&j->kind==0&&strstr(j->tx,"\"cmd\":\"pattern\""))return 1;}return 0;}
static void led_changed(void){if(led_busy()||led_job_pending())S.led_dirty=1;else led_apply();}
static void led_tick(void){if(S.led_dirty&&!led_busy()&&!led_job_pending())led_apply();}
static void put_float(struct ep_writer *w,unsigned f,float v){uint32_t bits;memcpy(&bits,&v,4);ep_fixed32(w,f,bits);}
/* LightStateResponse: what HA shows. Brightness and colour are the HA-set values, not the ring's live output. */
static void led_state(struct client *c){unsigned char b[160];struct ep_writer w={b,sizeof b,0,0};float level=(float)S.led_brightness/100.0f;
 ep_fixed32(&w,1,LED_RING_KEY);ep_uint(&w,2,S.led_on?1:0);put_float(&w,3,level);ep_uint(&w,11,35);put_float(&w,10,level);
 put_float(&w,4,(float)S.led_r/255.0f);put_float(&w,5,(float)S.led_g/255.0f);put_float(&w,6,(float)S.led_b/255.0f);ep_string(&w,9,led_effects[S.led_effect]);(void)send_writer(c,24,&w);}
static void led_states_all(void){for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].states&&S.clients[i].led_light)led_state(&S.clients[i]);}
static int led_float(const void *p,size_t n,unsigned f,float *out){uint32_t bits=(uint32_t)ep_get_uint(p,n,f,0);float v;memcpy(&v,&bits,4);if(!isfinite(v)||v<0.0f||v>1.0f)return -1;*out=v;return 0;}
/* LightCommandRequest. Every field is validated before any state changes. */
static int led_command(const void *p,size_t n){float r=0,g=0,b=0,level=0;char effect[16];int on=S.led_on,index=S.led_effect,rr=S.led_r,gg=S.led_g,bb=S.led_b,brightness=S.led_brightness;
 if(ep_get_uint(p,n,1,0)!=LED_RING_KEY)return -1;
 if(ep_get_uint(p,n,2,0))on=ep_get_uint(p,n,3,0)!=0;
 if(ep_get_uint(p,n,6,0)){if(led_float(p,n,7,&r)||led_float(p,n,8,&g)||led_float(p,n,9,&b))return -1;rr=(int)lroundf(r*255.0f);gg=(int)lroundf(g*255.0f);bb=(int)lroundf(b*255.0f);}
 /* color_brightness is what HA sends for RGB lights; brightness is the legacy field. */
 if(ep_get_uint(p,n,20,0)){if(led_float(p,n,21,&level))return -1;brightness=(int)lroundf(level*100.0f);}
 else if(ep_get_uint(p,n,4,0)){if(led_float(p,n,5,&level))return -1;brightness=(int)lroundf(level*100.0f);}
 if(ep_get_uint(p,n,18,0)){if(ep_get_text(p,n,19,effect,sizeof effect))return -1;index=-1;for(unsigned i=0;i<NELEM(led_effects);i++)if(!strcmp(effect,led_effects[i]))index=(int)i;if(index<0)return -1;}
 S.led_on=on;S.led_effect=index;S.led_r=rr;S.led_g=gg;S.led_b=bb;S.led_brightness=brightness;
 led_changed();led_states_all();return 0;}
static const struct {unsigned key;const char *id,*label,*unit,*icon;} setting_numbers[]={
 {VOLUME_NUMBER_KEY,"volume","Volume","%","mdi:volume-high"},
 {SENSITIVITY_NUMBER_KEY,"wake_word_sensitivity","Wake word sensitivity","","mdi:tune-variant"},
 {LED_BRIGHTNESS_KEY,"led_brightness","LED brightness","%","mdi:brightness-6"}};
/* Settings entities by index: 0 volume, 1 wake sensitivity, 2 LED brightness, 3 wake word switch.
 * Returns whether the value is known; a daemon that has not reported yet is sent as missing. */
static int setting_now(unsigned i,int *v){switch(i){
 case 0:*v=S.volume;return S.last_audio_status!=0;
 case 1:*v=S.sensitivity;return S.sensitivity_known;
 case 2:*v=S.led_master;return S.led_master_known;
 case 3:*v=S.active_wake;return 1;}
 return 0;}
static void setting_send(struct client *c,unsigned i){int v=0,known=setting_now(i,&v);unsigned char b[64];struct ep_writer w={b,sizeof b,0,0};
 if(i==3){ep_fixed32(&w,1,WAKE_SWITCH_KEY);ep_uint(&w,2,(unsigned)v);(void)send_writer(c,26,&w);return;}
 ep_fixed32(&w,1,i==0?VOLUME_NUMBER_KEY:i==1?SENSITIVITY_NUMBER_KEY:LED_BRIGHTNESS_KEY);if(known)put_float(&w,2,(float)v);else ep_uint(&w,3,1);(void)send_writer(c,50,&w);}
/* Sends a settings entity only when it changed (or was never sent), so the 250 ms status polls stay quiet. */
static void settings_sync(void){for(unsigned i=0;i<4;i++){int v=0,known=setting_now(i,&v);
 if(known==S.set_sent_known[i]&&(!known||v==S.set_sent_value[i]))continue;
 S.set_sent_known[i]=known;S.set_sent_value[i]=v;
 for(unsigned k=0;k<CLIENTS;k++)if(S.clients[k].fd>=0&&S.clients[k].states)setting_send(&S.clients[k],i);}}
static void states_send(struct client *c){unsigned char b[64];struct ep_writer w={b,sizeof b,0,0};float f=(float)S.volume/100.0f;uint32_t bits;memcpy(&bits,&f,4);
 ep_fixed32(&w,1,MEDIA_KEY);ep_uint(&w,2,S.playing?2:S.radio_state);ep_fixed32(&w,3,bits);ep_uint(&w,4,S.output_muted);if(S.last_audio_status)(void)send_writer(c,64,&w);
 w.length=0;ep_fixed32(&w,1,MUTE_KEY);ep_uint(&w,2,mic_muted());(void)send_writer(c,26,&w);
 if(S.last_audio_status){f=(float)S.reply_volume;memcpy(&bits,&f,4);w.length=0;ep_fixed32(&w,1,REPLY_KEY);ep_fixed32(&w,2,bits);(void)send_writer(c,50,&w);}
}
static const char *const button_ids[BUTTON_COUNT]={"volume_up","volume_down","microphone_mute","action"};
static const char *const button_labels[BUTTON_COUNT]={"Volume up button","Volume down button","Microphone mute button","Action button"};
static const char *const gesture_ids[GESTURE_COUNT]={"press","long_press","double_press"};
/* EventResponse for one event. Only subscribed clients get it, as with other states. */
static void event_send(unsigned key,const char *type){unsigned char b[64];
 for(unsigned i=0;i<CLIENTS;i++){struct ep_writer w={b,sizeof b,0,0};if(S.clients[i].fd<0||!S.clients[i].states)continue;ep_fixed32(&w,1,key);ep_string(&w,2,type);(void)send_writer(&S.clients[i],EVENT_STATE_TYPE,&w);}}
static void button_event(unsigned button,unsigned gesture){event_send(BUTTON_KEY_BASE+button,gesture_ids[gesture]);}
/* Last-turn text sensors (#398). Values change only on a real turn event or a history clear. Clients get
 * the wake word once subscribed; transcript and reply only when they listed them (speech text on). */
static char *text_slot(unsigned key){return key==TEXT_WAKE_KEY?S.last_wake:key==TEXT_TRANSCRIPT_KEY?S.last_transcript:S.last_reply;}
static void text_state(struct client *c,unsigned key){unsigned char b[TEXT_VALUE_MAX+64];struct ep_writer w={b,sizeof b,0,0};const char *v=text_slot(key);
 ep_fixed32(&w,1,key);ep_string(&w,2,v);if(!v[0])ep_uint(&w,3,1);(void)send_writer(c,TEXT_STATE_TYPE,&w);}
static void text_states(struct client *c){text_state(c,TEXT_WAKE_KEY);if(c->text_speech){text_state(c,TEXT_TRANSCRIPT_KEY);text_state(c,TEXT_REPLY_KEY);}}
static void text_set(unsigned key,const char *value){char *slot=text_slot(key);if(!strcmp(slot,value))return;snprintf(slot,TEXT_VALUE_MAX,"%s",value);
 for(unsigned i=0;i<CLIENTS;i++){struct client *c=&S.clients[i];if(c->fd>=0&&c->states&&(key==TEXT_WAKE_KEY||c->text_speech))text_state(c,key);}}
static void text_clear_all(void){text_set(TEXT_WAKE_KEY,"");text_set(TEXT_TRANSCRIPT_KEY,"");text_set(TEXT_REPLY_KEY,"");}
/* agentd's voice_history_generation: every clear of the voice history bumps it (web clear or history_clear). */
static void history_generation_seen(long long generation){if(S.history_known&&generation!=S.history_generation)text_clear_all();S.history_known=1;S.history_generation=generation;}
/* Event types are fixed in each entity's list; the order here is the list order. */
static const char *const voice_events[]={"wake_word","listening","thinking","replying","finished","stopped"};
static const char *const playback_events[]={"started","stopped"};
/* LibreEcho timer event types 0-3 as Home Assistant sends them (STARTED, UPDATED, CANCELLED, FINISHED). */
static const char *const timer_events[]={"started","updated","cancelled","finished"};
static const struct {unsigned key;const char *id,*label;const char *const *types;unsigned count;} activity_entities[]={
 {ACTIVITY_VOICE_KEY,"voice_assistant","Voice assistant",voice_events,NELEM(voice_events)},
 {ACTIVITY_PLAYBACK_KEY,"playback","Playback",playback_events,NELEM(playback_events)},
 {ACTIVITY_TIMER_KEY,"timer","Timer",timer_events,NELEM(timer_events)}};
/* Datagrams are "<button> <gesture>" exactly; anything else is dropped unseen. */
static void button_tick(void){char msg[BUTTON_MSG_MAX+1];
 for(unsigned n=0;n<BUTTON_BATCH;n++){ssize_t got=recv(S.button_fd,msg,sizeof msg-1,MSG_DONTWAIT);if(got<0)break;msg[got]=0;char *sp=strchr(msg,' ');if(!sp)continue;*sp++=0;
  int button=-1,gesture=-1;for(unsigned i=0;i<BUTTON_COUNT;i++)if(!strcmp(msg,button_ids[i]))button=(int)i;for(unsigned g=0;g<GESTURE_COUNT;g++)if(!strcmp(sp,gesture_ids[g]))gesture=(int)g;
  if(button>=0&&gesture>=0)button_event((unsigned)button,(unsigned)gesture);}
}
static int button_socket_open(const char *path){struct sockaddr_un a;int fd;
 if(strlen(path)>=sizeof a.sun_path)return -1;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,path,strlen(path)+1);
 fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return -1;unlink(path);
 if(bind(fd,(struct sockaddr*)&a,sizeof a)<0){close(fd);return -1;}return fd;}
static void all_states(void){for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].states)states_send(&S.clients[i]);}
static int announce_done(int success){unsigned char b[4];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,(unsigned)success);return S.owner>=0?send_writer(&S.clients[S.owner],120,&w):-1;}
static void capture_end(void){if(S.turn==2&&S.owner>=0){unsigned char b[4];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,2,1);(void)send_writer(&S.clients[S.owner],106,&w);}if(S.turn==1||S.turn==2){S.turn=3;event_send(ACTIVITY_VOICE_KEY,"thinking");}profile("thinking");}
/* Playback ends through turn_clear or the tick; report it once, when S.playing drops. */
static void playback_stopped(void){if(!S.playing)return;S.playing=0;event_send(ACTIVITY_PLAYBACK_KEY,"stopped");}
static void turn_clear(const char *result,int notify){int active=S.turn||S.playing,server=S.server_turn;if(notify)(void)announce_done(0);playback_stopped();esp_playback_close(&S.playback);S.playing=S.turn=S.continue_turn=S.run_end=S.early_tts=S.announcement=S.pipeline=S.tts_done=S.server_turn=0;S.next_url[0]=S.tts_url[0]=S.conversation[0]=0;snprintf(S.result,sizeof S.result,"%s",result);S.result_time=time(NULL);
 /* A server-wake stream ends by timeout or HA error; retry after a second so a rejected run cannot spin. */
 if(server)S.server_retry=S.now+1000;
 /* One voice event per opened turn (wake, announcement or continued turn). */
 if(S.voice_open){S.voice_open=0;event_send(ACTIVITY_VOICE_KEY,strcmp(result,"success")?"stopped":"finished");}/* Error flash only for a turn that was really interrupted: an idle HA
 * reconnect is not a user-visible failure. */
 profile(active&&!server&&strcmp(result,"success")&&strcmp(result,"cancelled")?"error":"idle");status_write();all_states();}
static int voice_start(uint64_t sample,int continued);
static void cancel_turn(const char *reason){if(S.turn&&S.owner>=0){unsigned char b[4];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,0);(void)send_writer(&S.clients[S.owner],90,&w);}turn_clear(reason,S.announcement);}
/* A request the user cancelled (#404) ends quietly: ring straight to idle, no
 * error flash, and the HA pipeline is told to stop so no reply follows. The
 * ESPHome voice events carry no run id, so a brief guard keeps a fresh wake
 * from adopting the stopped run's late events. */
static void user_cancel(const char *source){if(!S.turn)return;fprintf(stderr,"esphomed: request cancelled (%s)\n",source);cancel_turn("cancelled");S.cancel_guard=S.now+1500;}
/* A second wake word while HA is thinking cancels that request. Listening and
 * replying are left to the button, HA and the stop word: there the user's own
 * command or the reply's echo could re-trigger the word. */
static void wake_detected(uint64_t sample){if(S.turn==3&&S.pipeline&&!S.announcement&&S.active_wake){user_cancel("wake word");return;}(void)voice_start(sample,0);}
/* Server-wake streaming ends with a stop request, never the on-device end-of-speech path. */
static void stop_capture(const char *reason){if(S.server_turn)cancel_turn(reason);else capture_end();}
static void client_close(unsigned i){struct client *c=&S.clients[i];if(c->fd>=0)close(c->fd);c->fd=-1;if(S.owner==(int)i){cancel_turn("disconnected");S.owner=-1;timer_owner_end();}en_free(&c->noise);memset(c,0,sizeof *c);c->fd=-1;status_write();}
static int voice_start(uint64_t sample,int continued){unsigned char b[256];struct ep_writer w={b,sizeof b,0,0};if(!S.ha_selected||S.owner<0||S.turn||S.now<S.cancel_guard||mic_muted()||(!continued&&!S.active_wake))return -1;
 ep_uint(&w,1,1);ep_string(&w,2,S.conversation);/* flags: USE_VAD only. USE_WAKE_WORD asks Home Assistant to run its own
    wake-word engine on the stream (ESPHome use_wake_word: true); the word was
    already detected here, so HA must start at STT -- with no HA wake engine the
    run otherwise fails instantly with wake-engine-missing. */
 ep_uint(&w,3,1);if(!continued)ep_string(&w,5,"Alexa");if(send_writer(&S.clients[S.owner],90,&w))return -1;
 S.capture_next=sample>3200?sample-3200:0;if(S.capture_next<S.ring_first)S.capture_next=S.ring_first;S.turn=1;S.turn_deadline=S.now+S.turn_limit;S.continue_turn=S.run_end=S.early_tts=S.tts_done=0;S.tts_url[0]=S.next_url[0]=0;S.announcement=0;S.pipeline=1;S.voice_open=1;if(!continued)event_send(ACTIVITY_VOICE_KEY,"wake_word");event_send(ACTIVITY_VOICE_KEY,"listening");profile("listening");status_write();return 0;
}
/* Server-side wake (esphome_server_wake, off by default): open an ESPHome run with
 * USE_VAD|USE_WAKE_WORD and no phrase, then stream the mic continuously so Home
 * Assistant's wake engine decides when a turn starts. Only while subscribed,
 * unmuted and privacy-off; mute, privacy and unsubscribe stop the stream at once. */
static int server_listen_start(void){unsigned char b[160];struct ep_writer w={b,sizeof b,0,0};uint64_t from;
 if(!S.server_wake||!S.ha_selected||S.owner<0||S.turn||mic_muted()||S.now<S.server_retry)return -1;
 ep_uint(&w,1,1);ep_string(&w,2,S.conversation);ep_uint(&w,3,3);if(send_writer(&S.clients[S.owner],90,&w)){S.server_retry=S.now+1000;return -1;}
 from=S.ring_end>16000?S.ring_end-16000:0;if(from<S.ring_first)from=S.ring_first;S.capture_next=from;
 S.turn=1;S.server_turn=1;S.turn_deadline=S.now+S.turn_limit;S.continue_turn=S.run_end=S.early_tts=S.tts_done=0;S.tts_url[0]=S.next_url[0]=0;S.announcement=0;S.pipeline=1;profile("listening");status_write();return 0;
}
static int playback_start(const char *url){if(!url[0]||S.playing)return -1;if(esp_playback_start(&S.playback,url,S.bus_path,S.ca_path,S.now)<0){esp_playback_close(&S.playback);return -1;}S.playing=1;S.turn=4;all_states();event_send(ACTIVITY_PLAYBACK_KEY,"started");return 0;}
/* HA may await the drain ack before RUN_END. Ack once, but keep this pipeline
 * alive until RUN_END so its delayed event cannot address a new capture. */
static void completed(void){if(!S.tts_done)(void)announce_done(1);S.tts_done=1;if(S.pipeline&&!S.run_end)return;char conv[128];int cont=S.continue_turn;snprintf(conv,sizeof conv,"%s",S.conversation);turn_clear("success",0);if(cont&&!mic_muted()){snprintf(S.conversation,sizeof S.conversation,"%s",conv);(void)voice_start(S.ring_end,1);}}
static int valid_proto(const void *p,size_t n){struct ep_reader r;struct ep_field f;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0){}return r.error?-1:0;}
static void device_info(struct client *c){unsigned char b[512];struct ep_writer w={b,sizeof b,0,0};ep_string(&w,2,S.name);ep_string(&w,3,S.mac);ep_string(&w,4,"2026.9.0");ep_string(&w,6,"radar-puffin");ep_string(&w,8,"libreecho.satellite");ep_string(&w,9,"0.14.0");ep_string(&w,12,"LibreEcho");ep_string(&w,13,S.friendly);if(S.room[0])ep_string(&w,16,S.room);ep_uint(&w,17,61);ep_uint(&w,19,1);ep_uint(&w,26,!S.provisioned);(void)send_writer(c,10,&w);}
/* User-defined ESPHome services (#397): Home Assistant automations call announce, play_sound and
 * set_ring. Each call is validated here, then sent to the daemon command the web UI and voice path
 * already use: audiod speak (which goes through ttsd), audiod sample (bundled sounds) and ledd pattern.
 * A call that arrives during a voice turn, playback or another LED write is ignored, never queued,
 * so device activity keeps priority. Malformed or out-of-range arguments close the client before any change. */
#define SERVICE_ANNOUNCE_KEY 60U
#define SERVICE_PLAY_SOUND_KEY 61U
#define SERVICE_SET_RING_KEY 62U
#define LIST_SERVICES_TYPE 41U
#define EXECUTE_SERVICE_TYPE 42U
#define SERVICE_TEXT_MAX 200U
#define SERVICE_NAME_MAX 48U
#define SERVICE_RING_MAX_SECONDS 300U
#define SERVICE_RING_OWNER "esphome-action"
#define SERVICE_ARG_INT 1U
#define SERVICE_ARG_STRING 3U
struct service_arg { const char *name; unsigned type; };
struct user_service { const char *name; unsigned key; const struct service_arg *args; unsigned count; };
static const struct service_arg announce_args[]={{"text",SERVICE_ARG_STRING}};
static const struct service_arg play_sound_args[]={{"name",SERVICE_ARG_STRING}};
static const struct service_arg set_ring_args[]={{"colour",SERVICE_ARG_STRING},{"effect",SERVICE_ARG_STRING},{"duration",SERVICE_ARG_INT}};
static const struct user_service user_services[]={
 {"announce",SERVICE_ANNOUNCE_KEY,announce_args,NELEM(announce_args)},
 {"play_sound",SERVICE_PLAY_SOUND_KEY,play_sound_args,NELEM(play_sound_args)},
 {"set_ring",SERVICE_SET_RING_KEY,set_ring_args,NELEM(set_ring_args)}};
/* ListEntitiesServicesResponse: name, key, then one ListEntitiesServicesArgument (name, type) per argument. */
static void services_list(struct client *c){for(unsigned i=0;i<NELEM(user_services);i++){unsigned char b[256];struct ep_writer w={b,sizeof b,0,0};ep_string(&w,1,user_services[i].name);ep_fixed32(&w,2,user_services[i].key);
 for(unsigned a=0;a<user_services[i].count;a++){unsigned char x[64];struct ep_writer n={x,sizeof x,0,0};ep_string(&n,1,user_services[i].args[a].name);ep_uint(&n,2,user_services[i].args[a].type);ep_bytes(&w,3,n.data,n.length);}
 (void)send_writer(c,LIST_SERVICES_TYPE,&w);}}
struct svc_value { const unsigned char *data; size_t length; };
/* ExecuteServiceRequest: fixed32 key (1), then ExecuteServiceArgument (2) in list order. */
static int service_call(const void *p,size_t n,struct svc_value *v,unsigned max,unsigned *count){struct ep_reader r;struct ep_field f;*count=0;ep_reader_init(&r,p,n);
 while(ep_next(&r,&f)>0)if(f.number==2){if(f.wire!=2||*count>=max)return -1;v[(*count)++]=(struct svc_value){f.data,f.length};}
 return r.error?-1:0;}
/* Strings are ExecuteServiceArgument.string_ (4); integers are int_ (5), sint32 as the pinned client sends them. */
static int svc_text(const struct svc_value *v,char *out,size_t cap){if(ep_get_text(v->data,v->length,4,out,cap)||!out[0])return -1;return 0;}
static int svc_int(const struct svc_value *v,int *out){uint64_t raw=ep_get_uint(v->data,v->length,5,0);uint32_t z;if(raw>UINT32_MAX)return -1;z=(uint32_t)raw;*out=(int)(int32_t)((z>>1)^(0U-(z&1U)));return 0;}
static int svc_plain_text(const char *s){for(const unsigned char *q=(const unsigned char*)s;*q;q++)if(*q<0x20||*q==0x7f)return 0;return 1;}
static int svc_sound_name(const char *s){size_t n=strlen(s);if(!n||n>SERVICE_NAME_MAX)return 0;for(size_t i=0;i<n;i++){char c=s[i];if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return 0;}return 1;}
/* "#RRGGBB" exactly. */
static int svc_colour(const char *s,unsigned *r,unsigned *g,unsigned *b){unsigned v[3];if(strlen(s)!=7||s[0]!='#')return -1;
 for(unsigned i=0;i<3;i++){char h[3]={s[1+2*i],s[2+2*i],0};char *end;if(!isxdigit((unsigned char)h[0])||!isxdigit((unsigned char)h[1]))return -1;v[i]=(unsigned)strtoul(h,&end,16);if(*end)return -1;}
 *r=v[0];*g=v[1];*b=v[2];return 0;}
/* Index into led_effects (Solid, Pulse, Flash), exact match as listed to Home Assistant. */
static int svc_effect(const char *s){for(unsigned i=0;i<NELEM(led_effects);i++)if(!strcmp(s,led_effects[i]))return (int)i;return -1;}
static int service_execute(const void *p,size_t n){struct svc_value v[3];unsigned count=0;unsigned key=(unsigned)ep_get_uint(p,n,1,0);
 char text[SERVICE_TEXT_MAX+1],escaped[2*SERVICE_TEXT_MAX+16],args[512];
 if(service_call(p,n,v,NELEM(v),&count))return -1;
 if(key==SERVICE_ANNOUNCE_KEY){if(count!=1||svc_text(&v[0],text,sizeof text)||!svc_plain_text(text))return -1;
  if(S.turn||S.playing)return 0;json_escape(escaped,sizeof escaped,text);snprintf(args,sizeof args,"{\"text\":\"%s\"}",escaped);return job(S.audio_path,"speak",args,0,0)?-1:0;}
 if(key==SERVICE_PLAY_SOUND_KEY){char name[SERVICE_NAME_MAX+2];if(count!=1||svc_text(&v[0],name,sizeof name)||!svc_sound_name(name))return -1;
  if(S.turn||S.playing)return 0;snprintf(args,sizeof args,"{\"name\":\"%s\"}",name);return job(S.audio_path,"sample",args,0,0)?-1:0;}
 if(key==SERVICE_SET_RING_KEY){char colour[8],effect[16];unsigned r,g,b;int index,seconds;
  if(count!=3||svc_text(&v[0],colour,sizeof colour)||svc_colour(colour,&r,&g,&b))return -1;
  if(svc_text(&v[1],effect,sizeof effect))return -1;index=svc_effect(effect);if(index<0)return -1;
  if(svc_int(&v[2],&seconds)||seconds<1||seconds>(int)SERVICE_RING_MAX_SECONDS)return -1;
  if(led_busy()||led_job_pending())return 0;
  snprintf(args,sizeof args,"{\"name\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":100,\"repeats\":0,\"owner\":\"" SERVICE_RING_OWNER "\"}",led_ledd_names[index],r,g,b);
  if(job(S.led_path,"pattern",args,0,0))return -1;S.action_ring_end=S.now+(uint64_t)seconds*1000U;return 0;}
 return -1;}
/* A set_ring overlay ends after its duration. Only its own owner is stopped, so ledd restores the layer beneath it. */
static void action_ring_tick(void){if(!S.action_ring_end||S.now<S.action_ring_end||led_job_pending())return;S.action_ring_end=0;(void)job(S.led_path,"pattern","{\"name\":\"stop\",\"owner\":\"" SERVICE_RING_OWNER "\"}",0,0);}
static void entities(struct client *c){unsigned char b[768],nested[80];struct ep_writer w={b,sizeof b,0,0};ep_string(&w,1,"media_player");ep_fixed32(&w,2,MEDIA_KEY);ep_string(&w,3,"Media player");ep_uint(&w,8,1);
 /* aioesphomeapi 46.2.0: PAUSE, VOLUME_SET/MUTE, PLAY_MEDIA, STOP, PLAY, MEDIA_ANNOUNCE. */
 ep_uint(&w,11,(1U<<0)|(1U<<2)|(1U<<3)|(1U<<9)|(1U<<12)|(1U<<14)|(1U<<20));
 for(unsigned i=0;i<3;i++){struct ep_writer n={nested,sizeof nested,0,0};ep_string(&n,1,i==1?"wav":"mp3");ep_uint(&n,2,48000);ep_uint(&n,3,i==1?1:2);ep_uint(&n,4,i?1:0);if(i==1)ep_uint(&n,5,2);ep_bytes(&w,9,n.data,n.length);}(void)send_writer(c,63,&w);
 w.length=0;ep_string(&w,1,"microphone_mute");ep_fixed32(&w,2,MUTE_KEY);ep_string(&w,3,"Microphone mute");ep_string(&w,5,"mdi:microphone-off");(void)send_writer(c,17,&w);
 w.length=0;ep_string(&w,1,"cancel_request");ep_fixed32(&w,2,CANCEL_KEY);ep_string(&w,3,"Cancel request");ep_string(&w,5,"mdi:close-circle-outline");(void)send_writer(c,61,&w);
 /* Number: min 0, max 100, step 1, config category, %, slider. Follows the master volume until set; the web UI can return it to following. */
 {float lo=0,hi=100,step=1;uint32_t bits;w.length=0;ep_string(&w,1,"reply_volume");ep_fixed32(&w,2,REPLY_KEY);ep_string(&w,3,"Reply volume");ep_string(&w,5,"mdi:account-voice");
 memcpy(&bits,&lo,4);ep_fixed32(&w,6,bits);memcpy(&bits,&hi,4);ep_fixed32(&w,7,bits);memcpy(&bits,&step,4);ep_fixed32(&w,8,bits);ep_uint(&w,10,1);ep_string(&w,11,"%");ep_uint(&w,12,2);(void)send_writer(c,49,&w);}
 /* Settings (#396), config category: wake word on/off (ListEntitiesSwitchResponse) ... */
 w.length=0;ep_string(&w,1,"wake_word");ep_fixed32(&w,2,WAKE_SWITCH_KEY);ep_string(&w,3,"Wake word");ep_string(&w,5,"mdi:account-voice");ep_uint(&w,7,1);(void)send_writer(c,17,&w);
 /* ... and the 0-100 numbers (ListEntitiesNumberResponse: min 8, max 9, step 10, unit 11). */
 for(unsigned i=0;i<NELEM(setting_numbers);i++){w.length=0;ep_string(&w,1,setting_numbers[i].id);ep_fixed32(&w,2,setting_numbers[i].key);ep_string(&w,3,setting_numbers[i].label);ep_string(&w,5,setting_numbers[i].icon);ep_uint(&w,7,1);put_float(&w,8,0.0f);put_float(&w,9,100.0f);put_float(&w,10,1.0f);if(setting_numbers[i].unit[0])ep_string(&w,11,setting_numbers[i].unit);(void)send_writer(c,49,&w);}
 /* ListEntitiesEventResponse, one per button: the gesture is the event type. */
 for(unsigned i=0;i<BUTTON_COUNT;i++){w.length=0;ep_string(&w,1,button_ids[i]);ep_fixed32(&w,2,BUTTON_KEY_BASE+i);ep_string(&w,3,button_labels[i]);for(unsigned g=0;g<GESTURE_COUNT;g++)ep_string(&w,9,gesture_ids[g]);(void)send_writer(c,EVENT_LIST_TYPE,&w);}
 /* Activity entities (#393): one event entity each, event types listed as field 9. */
 for(unsigned i=0;i<NELEM(activity_entities);i++){w.length=0;ep_string(&w,1,activity_entities[i].id);ep_fixed32(&w,2,activity_entities[i].key);ep_string(&w,3,activity_entities[i].label);for(unsigned t=0;t<activity_entities[i].count;t++)ep_string(&w,9,activity_entities[i].types[t]);(void)send_writer(c,EVENT_LIST_TYPE,&w);}
 /* ListEntitiesLightResponse (#395): the LED ring, RGB (colour mode 35) with brightness, one effect per ledd overlay kind. */
 w.length=0;ep_string(&w,1,"led_ring");ep_fixed32(&w,2,LED_RING_KEY);ep_string(&w,3,"LED ring");ep_uint(&w,12,35);for(unsigned e=0;e<NELEM(led_effects);e++)ep_string(&w,11,led_effects[e]);ep_string(&w,14,"mdi:led-strip-variant");(void)send_writer(c,15,&w);c->led_light=1;
 /* ListEntitiesSensorResponse: lx, illuminance, STATE_CLASS_MEASUREMENT; only when the board has a sensor. */
 if(S.light_path[0]){c->light=1;w.length=0;ep_string(&w,1,"illuminance");ep_fixed32(&w,2,LIGHT_KEY);ep_string(&w,3,"Illuminance");ep_string(&w,6,"lx");ep_uint(&w,7,0);ep_string(&w,9,"illuminance");ep_uint(&w,10,1);(void)send_writer(c,16,&w);}
 /* ListEntitiesSensorResponse (#399): room level in dBFS, one decimal, measurement; only when the owner opts in. */
 if(S.room_level_sensor){c->level=1;w.length=0;ep_string(&w,1,"room_level");ep_fixed32(&w,2,LEVEL_KEY);ep_string(&w,3,"Room level");ep_string(&w,5,"mdi:ear-hearing");ep_string(&w,6,"dBFS");ep_uint(&w,7,1);ep_uint(&w,10,1);(void)send_writer(c,16,&w);}
 /* ListEntitiesTextSensorResponse (#398): last wake word; transcript and reply only with speech text on. */
 c->text_speech=S.speech_text;w.length=0;ep_string(&w,1,"last_wake_word");ep_fixed32(&w,2,TEXT_WAKE_KEY);ep_string(&w,3,"Last wake word");ep_string(&w,5,"mdi:account-voice");(void)send_writer(c,TEXT_LIST_TYPE,&w);
 if(S.speech_text){w.length=0;ep_string(&w,1,"last_transcript");ep_fixed32(&w,2,TEXT_TRANSCRIPT_KEY);ep_string(&w,3,"Last transcript");ep_string(&w,5,"mdi:microphone-message");(void)send_writer(c,TEXT_LIST_TYPE,&w);
  w.length=0;ep_string(&w,1,"last_reply");ep_fixed32(&w,2,TEXT_REPLY_KEY);ep_string(&w,3,"Last reply");ep_string(&w,5,"mdi:message-reply-text");(void)send_writer(c,TEXT_LIST_TYPE,&w);}
 /* ListEntitiesServicesResponse (#397): the user-defined actions, before ListEntitiesDone. */
 services_list(c);
 (void)send_msg(c,19,NULL,0);
}
static void configuration(struct client *c){unsigned char b[256],x[128];struct ep_writer w={b,sizeof b,0,0},n={x,sizeof x,0,0};ep_string(&n,1,"alexa_v0.1");ep_string(&n,2,"Alexa");ep_string(&n,3,"en");ep_bytes(&w,1,x,n.length);if(S.active_wake)ep_string(&w,2,"alexa_v0.1");ep_uint(&w,3,1);(void)send_writer(c,122,&w);}
/* Preserve all unrelated fields; only replace a validated top-level string or
 * insert before the closing brace. config_store enforces 0600 + fsync/rename. */
static char *top_field(char *json,const char *needle){int depth=0;size_t len=strlen(needle);for(char *q=json;*q;q++){
 if(*q=='{'||*q=='['){depth++;continue;}if(*q=='}'||*q==']'){depth--;continue;}if(*q!='"')continue;
 char *start=q,*end=q+1;while(*end){if(*end=='\\'){if(!end[1])return NULL;end+=2;}else if(*end=='"')break;else end++;}if(!*end)return NULL;
 char *colon=end+1;while(isspace((unsigned char)*colon))colon++;if(depth==1&&*colon==':'&&(size_t)(end-start+1)==len&&!memcmp(start,needle,len))return start;q=end;
 }return NULL;}
static int config_set_string(const char *field,const char *value){char old[16384],out[16384],needle[128],escaped[512];int n=config_read(S.config_path,old,sizeof old);if(n<0||!json_valid_object(old,(size_t)n)||json_duplicate_key(old,(size_t)n,field))return -1;
 json_escape(escaped,sizeof escaped,value);snprintf(needle,sizeof needle,"\"%s\"",field);char *start=top_field(old,needle),*end;size_t prefix;
 if(start){/* Locate exactly the top-level field, rejecting ambiguous nesting. */
 char test[512];if(json_get_string_top_level(old,field,test,sizeof test)<=0)return -1;start=strchr(start+strlen(needle),':');if(!start)return -1;start++;while(isspace((unsigned char)*start))start++;if(*start!='\"')return -1;end=start+1;while(*end){if(*end=='\\'){if(!end[1])return -1;end+=2;}else if(*end=='\"'){end++;break;}else end++;}if(end[-1]!='\"')return -1;prefix=(size_t)(start-old);
 int z=snprintf(out,sizeof out,"%.*s\"%s\"%s",(int)prefix,old,escaped,end);if(z<0||(size_t)z>=sizeof out)return -1;return config_write_atomic(S.config_path,out,(size_t)z);
 }
 end=old+n;while(end>old&&isspace((unsigned char)end[-1]))end--;if(end==old||end[-1]!='}')return -1;prefix=(size_t)(end-old-1);char *q=old+1;while(isspace((unsigned char)*q))q++;int z=snprintf(out,sizeof out,"%.*s%s\"%s\":\"%s\"}\n",(int)prefix,old,*q=='}'?"":",",field,escaped);if(z<0||(size_t)z>=sizeof out)return -1;return config_write_atomic(S.config_path,out,(size_t)z);
}
/* Integer twin of config_set_string (#396): replaces an existing top-level number or inserts one.
 * The document is validated first; only a digit run may be replaced, so a string is never overwritten. */
static int config_set_int(const char *field,int value){char old[16384],out[16384],needle[128],num[16];int n=config_read(S.config_path,old,sizeof old);if(n<0||!json_valid_object(old,(size_t)n)||json_duplicate_key(old,(size_t)n,field))return -1;
 snprintf(num,sizeof num,"%d",value);snprintf(needle,sizeof needle,"\"%s\"",field);char *start=top_field(old,needle),*end;size_t prefix;
 if(start){char *colon=strchr(start+strlen(needle),':');if(!colon)return -1;char *v=colon+1;while(isspace((unsigned char)*v))v++;end=v;while(*end&&*end!=','&&*end!='}'&&!isspace((unsigned char)*end)){if(!isdigit((unsigned char)*end)&&*end!='-')return -1;end++;}if(end==v)return -1;prefix=(size_t)(v-old);
 int z=snprintf(out,sizeof out,"%.*s%s%s",(int)prefix,old,num,end);if(z<0||(size_t)z>=sizeof out)return -1;return config_write_atomic(S.config_path,out,(size_t)z);
 }
 end=old+n;while(end>old&&isspace((unsigned char)end[-1]))end--;if(end==old||end[-1]!='}')return -1;prefix=(size_t)(end-old-1);char *q=old+1;while(isspace((unsigned char)*q))q++;
 int z=snprintf(out,sizeof out,"%.*s%s\"%s\":%s}\n",(int)prefix,old,*q=='}'?"":",",field,num);if(z<0||(size_t)z>=sizeof out)return -1;return config_write_atomic(S.config_path,out,(size_t)z);
}
static int provision(struct client *c,const void *p,size_t n){struct ep_reader r;struct ep_field f;unsigned char key[32],b[8];char base64[45];size_t z;int found=0,ok=0;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0)if(f.number==1){if(found++||f.wire!=2||f.length!=32)return -1;memcpy(key,f.data,32);}if(!found)return -1;
 if(c->mode==1&&!mbedtls_base64_encode((unsigned char*)base64,sizeof base64,&z,key,32)&&z==44){base64[z]=0;if(config_set_string("esphome_noise_key",base64)==0){memcpy(S.psk,key,32);S.provisioned=1;ok=1;}}
 struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,ok);int rc=send_writer(c,125,&w);mbedtls_platform_zeroize(key,sizeof key);mbedtls_platform_zeroize(base64,sizeof base64);if(ok){/* current encrypted session can read its ack; all sessions reconnect */for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0)S.clients[i].closing=1;}return rc;
}
static int event_data(const void *p,size_t n,const char *name,char *out,size_t cap){struct ep_reader r;struct ep_field f;unsigned count=0;ep_reader_init(&r,p,n);out[0]=0;while(ep_next(&r,&f)>0)if(f.number==2&&f.wire==2){char k[64],v[1024];if(++count>32||valid_proto(f.data,f.length)||ep_get_text(f.data,f.length,1,k,sizeof k)||ep_get_text(f.data,f.length,2,v,sizeof v))return -1;if(!strcmp(k,name)){if(strlen(v)>=cap)return -1;memcpy(out,v,strlen(v)+1);return 1;}}return r.error?-1:0;}
static void voice_event(const void *p,size_t n){unsigned event=(unsigned)ep_get_uint(p,n,1,0);char v[1024];if(!S.turn)return;
 if(event_data(p,n,"url",v,sizeof v)<0){cancel_turn("bad_event");return;}if(v[0])snprintf(S.tts_url,sizeof S.tts_url,"%s",v);
 /* Speech text (#398): STT_END carries the transcript, TTS_START the reply text. Published only with the owner setting. */
 if(S.speech_text&&(event==4||event==7)){char t[TEXT_VALUE_MAX];if(event_data(p,n,"text",t,sizeof t)>0)text_set(event==4?TEXT_TRANSCRIPT_KEY:TEXT_REPLY_KEY,t);}
 if(event==0){cancel_turn("ha_error");return;}if(event==12)capture_end();
 if(event==6){if(event_data(p,n,"conversation_id",S.conversation,sizeof S.conversation)<0){cancel_turn("bad_event");return;}if(event_data(p,n,"continue_conversation",v,sizeof v)<0){cancel_turn("bad_event");return;}S.continue_turn=!strcmp(v,"1");}
 if(event==100){if(event_data(p,n,"tts_start_streaming",v,sizeof v)<0){cancel_turn("bad_event");return;}S.early_tts=!strcmp(v,"1");}
 if((event==100&&S.early_tts)||event==8){capture_end();if(!S.playing&&!S.tts_done&&S.tts_url[0]){event_send(ACTIVITY_VOICE_KEY,"replying");if(playback_start(S.tts_url)<0){(void)announce_done(0);cancel_turn("playback_error");return;}}}
 if(event==2){S.run_end=1;if(!S.playing&&(S.tts_done||!S.tts_url[0])){capture_end();completed();}}
}
/*
 * Time zone from Home Assistant (#371). Like an ESPHome `time: platform:
 * homeassistant`, ask the client for the time (GetTimeRequest 36); HA answers
 * with GetTimeResponse 37 whose field 2 is its configured zone as a POSIX
 * string. Only when the owner chose "Follow Home Assistant" in Device
 * settings (timezone_source in web-config.json, read fresh so no reload is
 * needed) is it stored as "timezone"; the daemons that render local time
 * pick that up live and the initramfs exports it at the next boot. An empty,
 * oversized or unusable zone is ignored, never a reason to drop the client.
 */
#define TIME_REQUEST_INTERVAL_MS 3600000U
static void time_request(struct client *c){c->time_at=S.now+TIME_REQUEST_INTERVAL_MS;(void)send_msg(c,36,NULL,0);}
static void time_response(const void *p,size_t n){char raw[96],tz[64],current[64],source[24],b[16384];int len;
 if(ep_get_text(p,n,2,raw,sizeof raw)||!raw[0])return;len=config_read(S.config_path,b,sizeof b);if(len<0||!json_valid_object(b,(size_t)len))return;
 if(json_get_string_top_level(b,"timezone_source",source,sizeof source)!=1||strcmp(source,"home_assistant"))return;
 if(le_timezone_normalize(raw,tz,sizeof tz)){fprintf(stderr,"esphomed: ignoring unusable Home Assistant time zone\n");return;}
 if(json_get_string_top_level(b,"timezone",current,sizeof current)==1&&!strcmp(current,tz))return;
 if(config_set_string("timezone",tz)||config_set_string("timezone_name",""))fprintf(stderr,"esphomed: Home Assistant time zone could not be saved\n");
 else fprintf(stderr,"esphomed: time zone set from Home Assistant: %s\n",tz);}
/* Wake word on/off: the same persisted selection and waked set_word that VoiceAssistantSetConfiguration uses. */
static int wake_set_active(int active){
 if(config_set_string("esphome_active_wake_word",active?"alexa_v0.1":"")<0)return -1;
 if(!active){S.active_wake=0;cancel_turn("wake_disabled");return 0;}
 if(job(S.wake_path,"set_word","{\"word\":\"Alexa\"}",5,1)<0)return -1;S.active_wake=1;return 0;}
/* NumberCommandRequest (#396). Integer 0-100 only, as the web API validates; each value goes to the
 * same daemon command the web API uses (set_volume, set_sensitivity, set_brightness). */
static int number_command(const void *p,size_t n){unsigned key=(unsigned)ep_get_uint(p,n,1,0);uint32_t bits=(uint32_t)ep_get_uint(p,n,2,0);float v;char args[64];int target;
 memcpy(&v,&bits,4);if(!isfinite(v)||v<0.0f||v>100.0f||v!=truncf(v))return -1;target=(int)v;
 switch(key){
 case VOLUME_NUMBER_KEY:snprintf(args,sizeof args,"{\"volume\":%d}",target);if(job(S.audio_path,"set_volume",args,4,target))return -1;
  /* Same LED idle auto-off activity as the web volume change; best effort. */
  (void)job(S.led_path,"activity","{\"source\":\"volume\"}",0,0);return 0;
 case SENSITIVITY_NUMBER_KEY:snprintf(args,sizeof args,"{\"sensitivity\":%d}",target);return job(S.wake_path,"set_sensitivity",args,12,target)?-1:0;
 case LED_BRIGHTNESS_KEY:snprintf(args,sizeof args,"{\"brightness\":%d}",target);return job(S.led_path,"set_brightness",args,9,target)?-1:0;
 default:return -1;}
}
static int dispatch(unsigned idx,unsigned type,const unsigned char *p,size_t n){struct client *c=&S.clients[idx];unsigned char b[128];struct ep_writer w={b,sizeof b,0,0};char args[2048],url[1024],escaped[2048];if(valid_proto(p,n))return -1;
 if(type==1){if(c->hello)return -1;ep_uint(&w,1,1);ep_uint(&w,2,14);ep_string(&w,3,"LibreEcho 0.14.0");ep_string(&w,4,S.name);c->hello=1;c->deadline=S.now+120000;status_write();return send_writer(c,2,&w);}
 if(type==3)return 0;/* Legacy Auth is obsolete; pinned client does not await it. */
 if(!c->hello)return -1;
 switch(type){case 7:return send_msg(c,8,NULL,0);case 8:return 0;case 5:if(send_msg(c,6,NULL,0))return -1;c->closing=1;return 0;case 9:device_info(c);return 0;case 11:entities(c);return 0;
 /* The lux state goes once on subscribe, then only from light_poll: all_states() runs on every adapter status change.
  * Only clients whose entity list carried the sensor get it (a late-found sensor waits for the next list). */
 case 20:c->states=1;states_send(c);for(unsigned i=0;i<4;i++)setting_send(c,i);if(c->light)light_state(c);if(c->level)level_state(c);if(c->led_light)led_state(c);text_states(c);time_request(c);return 0;
 case 37:time_response(p,n);return 0;
 /* LightCommandRequest (#395): HA sets the LED ring light; device indicators keep priority. */
 case 32:return led_command(p,n);
 /* ExecuteServiceRequest (#397): announce, play_sound, set_ring. */
 case EXECUTE_SERVICE_TYPE:return service_execute(p,n);
 /* NumberCommandRequest (#396): volume, wake sensitivity, LED brightness. */
 case 51:return number_command(p,n);
 case 89:if(!ep_get_uint(p,n,1,0)){if(S.owner==(int)idx){cancel_turn("unsubscribed");S.owner=-1;timer_owner_end();}return 0;}if(S.owner>=0&&S.owner!=(int)idx)return -1;
 /* Pinned client API_AUDIO subscription flag is 1<<2 (4), not 1. */
 if(!(ep_get_uint(p,n,2,0)&4))return -1;S.owner=(int)idx;return 0;
 case 91:if(S.owner!=(int)idx||S.turn!=1)return 0;if(ep_get_uint(p,n,2,0)||ep_get_uint(p,n,1,0)){cancel_turn("ha_rejected");return 0;}S.turn=2;if(S.server_turn)status_write();return 0;
 case 92:if(S.owner==(int)idx)voice_event(p,n);return 0;
 case 119:if(S.owner!=(int)idx)return -1;if(S.turn){(void)announce_done(0);return 0;}
 if(ep_get_text(p,n,1,url,sizeof url)||ep_get_text(p,n,3,S.next_url,sizeof S.next_url))return -1;S.turn=3;S.announcement=1;S.voice_open=1;S.turn_deadline=S.now+S.turn_limit;S.continue_turn=!!ep_get_uint(p,n,4,0);snprintf(S.tts_url,sizeof S.tts_url,"%s",url);
 if(S.next_url[0]){char pre[1024];snprintf(pre,sizeof pre,"%s",S.next_url);snprintf(S.next_url,sizeof S.next_url,"%s",url);if(playback_start(pre)<0)turn_clear("playback_error",1);}else if(playback_start(url)<0)turn_clear("playback_error",1);status_write();return 0;
 case 121:configuration(c);return 0;
 case 123:{struct ep_reader r;struct ep_field f;unsigned count=0;int active=0;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0)if(f.number==1){if(++count>1||ep_text(&f,url,sizeof url)||strcmp(url,"alexa_v0.1"))return -1;active=1;}
 if(wake_set_active(active))return -1;configuration(c);return 0;}
 case 124:return provision(c,p,n);
 case 33:if(ep_get_uint(p,n,1,0)==WAKE_SWITCH_KEY){if(wake_set_active(!!ep_get_uint(p,n,2,0)))return -1;all_states();settings_sync();return 0;}if(ep_get_uint(p,n,1,0)!=MUTE_KEY)return -1;{int mute=!!ep_get_uint(p,n,2,0);if(!mute&&S.privacy!=0){all_states();return 0;}if(mute){S.local_mute_gate=1;S.muted=1;if(S.turn==1)cancel_turn("muted");else if(S.turn==2)stop_capture("muted");all_states();}snprintf(args,sizeof args,"{\"muted\":%s}",mute?"true":"false");return job(S.audio_path,"set_mute",args,3,mute);}
 case 62:if(ep_get_uint(p,n,1,0)!=CANCEL_KEY)return -1;user_cancel("Home Assistant");return 0;
 case 51:if(ep_get_uint(p,n,1,0)!=REPLY_KEY)return -1;{uint32_t bits=(uint32_t)ep_get_uint(p,n,2,0);float level;memcpy(&level,&bits,4);if(!isfinite(level)||level<0||level>100)return -1;int target=(int)lroundf(level);snprintf(args,sizeof args,"{\"volume\":%d}",target);return job(S.audio_path,"set_reply_volume",args,JOB_REPLY,target)?-1:0;}
 case 65:if(ep_get_uint(p,n,1,0)!=MEDIA_KEY)return -1;
 if(ep_get_uint(p,n,4,0)){uint32_t bits=(uint32_t)ep_get_uint(p,n,5,0);float volume;memcpy(&volume,&bits,4);if(!isfinite(volume)||volume<0||volume>1)return -1;snprintf(args,sizeof args,"{\"volume\":%d}",(int)lroundf(volume*100));if(job(S.audio_path,"set_volume",args,4,(int)lroundf(volume*100)))return -1;}
 if(ep_get_uint(p,n,6,0)){if(ep_get_text(p,n,7,url,sizeof url)||strlen(url)>900)return -1;json_escape(escaped,sizeof escaped,url);snprintf(args,sizeof args,"{\"url\":\"%.1900s\"}",escaped);
 /* Finite announcement completion (type120) belongs to the voice owner.
 * Other clients retain ordinary radio/media controls, never this channel. */
 if(ep_get_uint(p,n,8,0)&&ep_get_uint(p,n,9,0)){if(S.owner!=(int)idx||S.turn)return 0;S.turn=3;S.turn_deadline=S.now+S.turn_limit;if(playback_start(url)<0)turn_clear("playback_error",1);}else if(job(S.radio_path,"play",args,0,0))return -1;}
 if(ep_get_uint(p,n,2,0)){unsigned command=(unsigned)ep_get_uint(p,n,3,0);const char *cmd=NULL;if(command==0)cmd="resume";else if(command==1)cmd="pause";else if(command==2)cmd="stop";else if(command==5)cmd=S.radio_state==2?"pause":"resume";
 if(cmd){if(job(S.radio_path,cmd,"{}",0,0))return -1;}else if(command==3||command==4){int target=command==3?0:S.restore_volume;if(command==3&&!S.output_muted)S.restore_volume=S.volume;snprintf(args,sizeof args,"{\"volume\":%d}",target);if(job(S.audio_path,"set_volume",args,6,command==3))return -1;}else return -1;}return 0;
 case 115:if(S.owner!=(int)idx)return -1;{char id[128],name[128],ei[256],en[256];unsigned event=(unsigned)ep_get_uint(p,n,1,0);uint64_t total=ep_get_uint(p,n,4,0),left=ep_get_uint(p,n,5,0);if(event>3||total>604800||left>604800||ep_get_text(p,n,2,id,sizeof id)||!id[0]||ep_get_text(p,n,3,name,sizeof name))return -1;json_escape(ei,sizeof ei,id);json_escape(en,sizeof en,name);snprintf(args,sizeof args,"{\"event_type\":%u,\"timer_id\":\"%s\",\"name\":\"%s\",\"total_seconds\":%llu,\"seconds_left\":%llu,\"is_active\":%s}",event,ei,en,(unsigned long long)total,(unsigned long long)left,ep_get_uint(p,n,6,0)?"true":"false");if(job(S.timer_path,"remote_event",args,JOB_TIMER,0))return -1;event_send(ACTIVITY_TIMER_KEY,timer_events[event]);return 0;}
 default:return 0;}
}
static int process_rx(unsigned idx){struct client *c=&S.clients[idx];unsigned packets=0;
 while(c->rx_n&&packets++<32){size_t used,len;unsigned type;int rc;if(c->mode<0){if(c->rx[0]==1){c->mode=1;c->phase=0;}else if(c->rx[0]==0&&S.plaintext&&!S.provisioned){c->mode=0;c->phase=2;}
 /* Real ESPHome answers a plaintext hello on an encrypted API with a Noise-framed
  * "\1Bad indicator byte"; clients map the 0x01 preamble to "requires encryption"
  * and Home Assistant then asks for the key instead of claiming the YAML lacks api:. */
 else if(c->rx[0]==0){static const unsigned char reject[]="\1Bad indicator byte";c->mode=1;c->phase=0;c->rx_n=0;if(outer(c,reject,sizeof reject-1))return -1;c->closing=1;return 0;}else return -1;}
 rc=ef_parse(c->rx,c->rx_n,c->mode==1,&used,&type,&len);if(rc<=0)return rc;
 const unsigned char *p=c->rx+used-len;if(c->mode==1&&c->phase<2){unsigned char reply[128];size_t z;
 if(c->phase==0){if(len)return -1;reply[0]=1;z=1;size_t n=strlen(S.name)+1;memcpy(reply+z,S.name,n);z+=n;/* ESPHome sends the bare 12-digit lowercase MAC here; aioesphomeapi caps this field at 16
    characters, so the colon form would arrive truncated and Home Assistant would treat a key
    change as a different device instead of asking for the new key. */
 {char hello_mac[13];size_t m=0;for(size_t i=0;S.mac[i]&&m<12;i++)if(S.mac[i]!=':')hello_mac[m++]=(char)tolower((unsigned char)S.mac[i]);hello_mac[m]=0;n=m+1;memcpy(reply+z,hello_mac,n);z+=n;}if(outer(c,reply,z))return -1;c->phase=1;}
 else{if(en_handshake(&c->noise,S.psk,p,len,reply,&z)){static const unsigned char err[]="\1Handshake MAC failure";if(outer(c,err,sizeof err-1))return -1;c->closing=1;}else{if(outer(c,reply,z))return -1;c->phase=2;}}
 }else if(c->mode==1){unsigned char plain[RX_CAP];size_t z;if(en_decrypt(&c->noise,p,len,plain,sizeof plain,&z)||z<4||(((size_t)plain[2]<<8)|plain[3])!=z-4)return -1;type=((unsigned)plain[0]<<8)|plain[1];int result=dispatch(idx,type,plain+4,z-4);mbedtls_platform_zeroize(plain,z);if(result)return -1;}
 else if(dispatch(idx,type,p,len))return -1;
 memmove(c->rx,c->rx+used,c->rx_n-used);c->rx_n-=used;if(c->closing)return 0;
 }return 0;
}
static void client_tick(unsigned idx,short events){struct client *c=&S.clients[idx];if(c->fd<0)return;if(S.now>c->deadline){client_close(idx);return;}
 /* aioesphomeapi only pings after hearing nothing, and our state stream keeps
  * it quiet, so probe a silent peer ourselves well inside the idle deadline. */
 if(c->hello&&!c->closing&&S.now>=c->last_seen+60000&&S.now>=c->ping_at+30000){c->ping_at=S.now;(void)send_msg(c,7,NULL,0);}
 if(c->hello&&!c->closing&&c->time_at&&S.now>=c->time_at)time_request(c);
 if((events&POLLIN)&&!c->closing){ssize_t n=recv(c->fd,c->rx+c->rx_n,sizeof c->rx-c->rx_n,MSG_DONTWAIT);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))n=-2;if(n==-2){}else if(n<=0){client_close(idx);return;}else{c->rx_n+=(size_t)n;c->last_seen=S.now;if(c->hello)c->deadline=S.now+120000;if(process_rx(idx)<0||c->rx_n==sizeof c->rx){client_close(idx);return;}}}
 if(c->tx_sent<c->tx_n){ssize_t n=send(c->fd,c->tx+c->tx_sent,c->tx_n-c->tx_sent,MSG_DONTWAIT|MSG_NOSIGNAL);if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){client_close(idx);return;}if(n>0)c->tx_sent+=(size_t)n;if(c->tx_sent==c->tx_n)c->tx_sent=c->tx_n=0;}
 if((c->closing&&!c->tx_n)||(events&(POLLERR|POLLNVAL))){client_close(idx);return;}
 /* A complete buffered burst is consumed on later ticks even without POLLIN. */
 if(c->rx_n&&!c->closing&&process_rx(idx)<0)client_close(idx);
}
static uint32_t le32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void ring_frame(const unsigned char *p){uint64_t first=le32(p+8)|((uint64_t)le32(p+12)<<32);unsigned count=le32(p+16);if(first>UINT64_MAX-count){cancel_turn("stream_error");return;}
 if(S.ring_end&&first!=S.ring_end){S.ring_first=S.ring_end=first;le_voice_ns_reset(&S.ns);if(S.turn==2)cancel_turn("stream_gap");}
 if(!S.ring_end)S.ring_first=first;{int16_t pcm[1280];for(unsigned i=0;i<count;i++){unsigned char const *a=p+24+2*i;pcm[i]=(int16_t)((unsigned)a[0]|((unsigned)a[1]<<8));}if(S.noise_suppression)le_voice_ns_process(&S.ns,pcm,count);for(unsigned i=0;i<count;i++)S.ring[(first+i)%RING_SAMPLES]=pcm[i];}S.ring_end=first+count;if(S.ring_end-S.ring_first>RING_SAMPLES)S.ring_first=S.ring_end-RING_SAMPLES;
 level_frame(p+24,count);
}
static void stream_loss(struct io *j){io_close(j);if(j==&S.mic){S.mic_ack=0;S.mic_n=0;S.ring_first=S.ring_end=0;le_voice_ns_reset(&S.ns);}if(S.turn==1||S.turn==2)cancel_turn("stream_error");S.wake_retry=S.now+1000;}
static void stream_tick(struct io *j,int audio){if(j->fd<0)return;
 if(j->stage<3){int rc=io_tick(j);if(rc<0){stream_loss(j);return;}if(!rc)return;char *end=memchr(j->rx,'\n',j->rx_n);*end=0;int ok=0;if(json_get_bool(j->rx,"ok",&ok)<=0||!ok){stream_loss(j);return;}
 if(audio){char format[32];int rate,channels,header,indexed;if(json_get_string(j->rx,"format",format,sizeof format)<=0||strcmp(format,"pcm_s16_le")||json_get_int(j->rx,"sample_rate",&rate)<=0||rate!=16000||json_get_int(j->rx,"channels",&channels)<=0||channels!=1||json_get_int(j->rx,"frame_header_bytes",&header)<=0||header!=24||json_get_bool(j->rx,"sample_indexed",&indexed)<=0||!indexed){stream_loss(j);return;}}
 size_t used=(size_t)(end-j->rx)+1;memmove(j->rx,j->rx+used,j->rx_n-used);j->rx_n-=used;j->stage=3;j->deadline=UINT64_MAX;}
 if(!audio){/* Bounded event lines, including fragmented/coalesced reads. */
 for(unsigned loop=0;loop<8;loop++){char *end=memchr(j->rx,'\n',j->rx_n);if(!end)break;*end=0;long long sample;char event[64];if(json_get_string(j->rx,"event",event,sizeof event)>0&&!strcmp(event,"wake_detected")){char model[TEXT_VALUE_MAX];if(json_get_string(j->rx,"model",model,sizeof model)>0)text_set(TEXT_WAKE_KEY,model);}
 if(json_get_string(j->rx,"event",event,sizeof event)>0&&!strcmp(event,"wake_detected")&&json_get_int64(j->rx,"detection_sample",&sample)>0&&sample>=0&&!S.server_wake)wake_detected((uint64_t)sample);size_t z=(size_t)(end-j->rx)+1;memmove(j->rx,j->rx+z,j->rx_n-z);j->rx_n-=z;}
 if(j->rx_n==sizeof j->rx-1){stream_loss(j);return;}ssize_t n=recv(j->fd,j->rx+j->rx_n,sizeof j->rx-1-j->rx_n,MSG_DONTWAIT);if(n>0){j->rx_n+=(size_t)n;j->rx[j->rx_n]=0;}else if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))stream_loss(j);return;
 }
 /* First consume bytes coalesced with the JSON stream handshake. */
 for(unsigned loop=0;loop<16;loop++){size_t need=24;if(S.mic_n>=24){unsigned count=le32(S.mic_rx+16);if(memcmp(S.mic_rx,"LVS1",4)||S.mic_rx[4]!=1||S.mic_rx[5]||S.mic_rx[6]||S.mic_rx[7]||le32(S.mic_rx+20)||!count||count>1280){stream_loss(j);return;}need+=count*2;}
 if(S.mic_n==need&&need>24){ring_frame(S.mic_rx);S.mic_n=0;continue;}
 size_t want=need-S.mic_n;if(j->rx_n){size_t z=j->rx_n<want?j->rx_n:want;memcpy(S.mic_rx+S.mic_n,j->rx,z);S.mic_n+=z;memmove(j->rx,j->rx+z,j->rx_n-z);j->rx_n-=z;continue;}
 ssize_t n=recv(j->fd,S.mic_rx+S.mic_n,want,MSG_DONTWAIT);if(n>0){S.mic_n+=(size_t)n;continue;}if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))stream_loss(j);break;}
}
static void capture_tick(void){if(S.turn!=2||S.owner<0)return;if(mic_muted()){stop_capture("muted");return;}if(S.capture_next<S.ring_first){cancel_turn("capture_overrun");return;}
 /* 20 ms PCM frames, at most four per tick, retaining indexed 200 ms preroll. */
 for(unsigned i=0;i<4&&S.ring_end-S.capture_next>=320;i++){unsigned char pcm[640],b[660];struct ep_writer w={b,sizeof b,0,0};for(unsigned k=0;k<320;k++){uint16_t v=(uint16_t)S.ring[(S.capture_next+k)%RING_SAMPLES];pcm[2*k]=(unsigned char)v;pcm[2*k+1]=(unsigned char)(v>>8);}ep_bytes(&w,1,pcm,sizeof pcm);
 if(send_writer(&S.clients[S.owner],106,&w)){cancel_turn("backpressure");return;}S.capture_next+=320;}
}
static void jobs_tick(void){for(unsigned i=0;i<JOBS;i++){struct io *j=&S.jobs[i];if(j->stage==3&&j->kind==JOB_TIMER){if(S.timer_pending)continue;if(io_connect(j,S.timer_path)<0){io_close(j);continue;}j->stage=1;j->deadline=S.now+500;}if(j->fd<0)continue;int rc=io_tick(j);if(!rc)continue;int ok=0;if(rc>0){char *end=memchr(j->rx,'\n',j->rx_n);*end=0;(void)json_get_bool(j->rx,"ok",&ok);}if(ok){
 if(j->kind==1){int muted,volume;if(json_get_bool(j->rx,"muted",&muted)>0&&json_get_int(j->rx,"volume",&volume)>0&&volume>=0&&volume<=100){S.muted=muted;S.volume=volume;S.last_audio_status=S.now;int reply;S.reply_volume=json_get_int(j->rx,"reply_volume",&reply)>0&&reply>=0&&reply<=100?reply:volume;}}
 if(j->kind==2){int playing=0,paused=0;(void)json_get_bool(j->rx,"playing",&playing);(void)json_get_bool(j->rx,"paused",&paused);S.radio_state=paused?3:playing?2:1;}
 if(j->kind==9){S.led_master=j->target;S.led_master_known=1;}
 if(j->kind==HISTORY_GENERATION_KIND){long long generation=-1;if(json_get_int64(j->rx,"history_generation",&generation)>0&&generation>=0)history_generation_seen(generation);}
 if(j->kind==12){S.sensitivity=j->target;S.sensitivity_known=1;(void)config_set_int("wake_sensitivity",j->target);}
 /* ledd status is flat: the first "brightness" is the global ring brightness (set_brightness). */
 if(j->kind==10){int v;if(json_get_int(j->rx,"brightness",&v)>0&&v>=0&&v<=100){S.led_master=v;S.led_master_known=1;}}
 if(j->kind==11){int v;if(json_get_int(j->rx,"sensitivity",&v)>0&&v>=0&&v<=100){S.sensitivity=v;S.sensitivity_known=1;}}
 if(j->kind==3){S.muted=j->target;S.local_mute_gate=j->target;}if(j->kind==4)S.volume=j->target;if(j->kind==JOB_REPLY)S.reply_volume=j->target;if(j->kind==6){S.output_muted=j->target;S.volume=j->target?0:S.restore_volume;}
 }else if(j->kind>=3&&j->kind<=6&&S.turn)cancel_turn("adapter_error");io_close(j);all_states();settings_sync();}timer_cleanup_tick();}
/* Local cancel (#404): buttond sends one "cancel" datagram on an Action press
 * while a request is in progress. Root-only 0600 socket; nothing else is read. */
static void control_open(void){struct sockaddr_un a;struct stat st;size_t len=S.control_path?strlen(S.control_path):0;if(S.control_fd>=0||S.now<S.control_retry)return;S.control_retry=S.now+1000;if(!len||len>=sizeof a.sun_path)return;
 if(!lstat(S.control_path,&st)){if(!S_ISSOCK(st.st_mode))return;(void)unlink(S.control_path);}
 int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,S.control_path,len+1);mode_t old=umask(077);int rc=bind(fd,(struct sockaddr*)&a,sizeof a);umask(old);if(rc){close(fd);return;}S.control_fd=fd;}
static void control_tick(void){control_open();if(S.control_fd<0)return;for(unsigned i=0;i<4;i++){char m[16];ssize_t n=recv(S.control_fd,m,sizeof m,MSG_DONTWAIT);if(n<0)return;if(n==6&&!memcmp(m,"cancel",6))user_cancel("action button");}}
static int light_read(const char *path){char b[32],*end;long v;if(config_read(path,b,sizeof b)<1)return -1;v=strtol(b,&end,10);if(end==b||v<0||v>1000000)return -1;return (int)v;}
static void light_poll(void){static const char *const probe[]={"/sys/bus/i2c/devices/0-0039/als_lux","/sys/bus/i2c/devices/1-0039/als_lux"};int lux,big;
 if(S.now<S.light_poll)return;S.light_poll=S.now+LIGHT_POLL_MS;
 /* Like backend_linux: only a successful discovery is kept, the driver may create its node late. */
 if(!S.light_path[0]){for(unsigned i=0;i<2;i++)if(access(probe[i],R_OK)==0){snprintf(S.light_path,sizeof S.light_path,"%s",probe[i]);break;}if(!S.light_path[0])return;}
 lux=light_read(S.light_path);S.lux=lux;if(lux==S.lux_sent)return;
 big=lux<0||S.lux_sent<0||abs(lux-S.lux_sent)*20>=(S.lux_sent>20?S.lux_sent:20);
 if(!big&&S.now<S.light_sent_at+LIGHT_REFRESH_MS)return;
 S.lux_sent=lux;S.light_sent_at=S.now;for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].states&&S.clients[i].light)light_state(&S.clients[i]);}
static void privacy_poll(void){char b[32];int n=config_read(S.privacy_path,b,sizeof b),old=S.privacy;if(n<0)S.privacy=-1;else if(b[0]=='0')S.privacy=0;else if(b[0]=='1')S.privacy=1;else S.privacy=-1;if(old!=S.privacy){if(mic_muted()&&(S.turn==1||S.turn==2))stop_capture("privacy");all_states();}}
static int normalize_mac(const char *input,char *out){unsigned char b[6];unsigned n=0,high=0;int hi=-1;for(size_t i=0;input[i];i++){int x=input[i];if(x==':'||isspace((unsigned char)x))continue;if(x>='0'&&x<='9')x-='0';else if(x>='a'&&x<='f')x=x-'a'+10;else if(x>='A'&&x<='F')x=x-'A'+10;else return -1;if(hi<0)hi=x;else{if(n>=6)return -1;b[n++]=(unsigned char)(hi*16+x);high|=b[n-1];hi=-1;}}if(n!=6||hi>=0||!high||(b[0]&1))return -1;snprintf(out,18,"%02x:%02x:%02x:%02x:%02x:%02x",b[0],b[1],b[2],b[3],b[4],b[5]);return 0;}
static int load_configuration(void){char b[16384],key[45],mac[64],name[64],wake[64];int n=config_read(S.config_path,b,sizeof b);struct le_esphome_config cfg;size_t z;if(n<0||config_esphome_read(S.config_path,&cfg))return -1;
 int selected=json_get_string_top_level(b,"esphome_active_wake_word",wake,sizeof wake);if(selected<0||json_duplicate_key(b,(size_t)n,"esphome_active_wake_word")||(selected>0&&wake[0]&&strcmp(wake,"alexa_v0.1")))return -1;S.active_wake=selected<=0||!!wake[0];
 /* Speech text sensors (#398) are off unless the owner sets esphome_speech_text_sensors; turning them off drops what was held. */
 {int speech=0;(void)json_get_bool(b,"esphome_speech_text_sensors",&speech);S.speech_text=speech!=0;if(!S.speech_text){text_set(TEXT_TRANSCRIPT_KEY,"");text_set(TEXT_REPLY_KEY,"");}}
 /* Room level sensor (#399) is off unless the owner sets esphome_room_level_sensor; a reload restarts the window. */
 {int room=0;(void)json_get_bool(b,"esphome_room_level_sensor",&room);S.room_level_sensor=room!=0;S.level_known=S.level_sent_ok=0;S.level_n=0;S.level_sum=0;}
 /* esphome_server_wake: Home Assistant runs wake detection on a continuous stream. Absent or invalid means off. */
 {int server=0;S.server_wake=json_get_bool(b,"esphome_server_wake",&server)>0&&server;}
 /* HA ownership is what the web API persists: integrations bit 1 or the
  * home-assistant pipeline mode (api.c applies the same rule on load).
  * voice_assistant_mode is a legacy field the API never writes. */
 {unsigned integrations=0;char pipeline[32];int mode=0;
  S.ha_selected=(json_get_uint(b,"integrations",&integrations)==1&&(integrations&1u))||
   (json_get_string_top_level(b,"voice_pipeline_mode",pipeline,sizeof pipeline)>0&&!strcmp(pipeline,"home-assistant"))||
   (json_get_int(b,"voice_assistant_mode",&mode)>0&&(mode&1));}memcpy(key,cfg.esphome_noise_key,sizeof key);
 /* Like an ESPHome YAML `api: encryption: key:`, the satellite always owns a
  * key. Generate one on first start so the owner can copy it from the web UI
  * into Home Assistant; a key Home Assistant later pushes replaces it. */
 if(!key[0]&&!S.plaintext){unsigned char fresh[32];size_t fz;int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC),got=fd>=0&&read(fd,fresh,32)==32;if(fd>=0)close(fd);
  if(!got||mbedtls_base64_encode((unsigned char*)key,sizeof key,&fz,fresh,32)||fz!=44||config_set_string("esphome_noise_key",key)){mbedtls_platform_zeroize(fresh,32);mbedtls_platform_zeroize(key,sizeof key);return -1;}
  key[44]=0;mbedtls_platform_zeroize(fresh,32);n=config_read(S.config_path,b,sizeof b);if(n<0)return -1;}
 S.provisioned=!!key[0];memset(S.psk,0,32);if(S.provisioned&&(mbedtls_base64_decode(S.psk,32,&z,(unsigned char*)key,44)||z!=32)){mbedtls_platform_zeroize(key,sizeof key);return -1;}mbedtls_platform_zeroize(key,sizeof key);mbedtls_platform_zeroize(&cfg,sizeof cfg);
 if(!S.name[0]){if(json_get_string(b,"hostname",name,sizeof name)<=0)snprintf(name,sizeof name,"libreecho");if(strlen(name)>31||!name[0])return -1;for(size_t i=0;name[i];i++)if(!(isalnum((unsigned char)name[i])||name[i]=='-'))return -1;snprintf(S.name,sizeof S.name,"%.31s",name);}
 if(!S.friendly[0])snprintf(S.friendly,sizeof S.friendly,"%s",S.name);
 {int on=0,was=S.noise_suppression;S.noise_suppression=json_get_top_level_bool(b,(size_t)n,"voice_noise_suppression",&on)==1&&on;if(S.noise_suppression&&!was)le_voice_ns_reset(&S.ns);if(S.noise_suppression!=was)fprintf(stderr,"esphomed: noise suppression %s\n",S.noise_suppression?"on":"off");}
 {char room[64];S.room[0]=0;if(json_get_string(b,"room",room,sizeof room)==1&&strlen(room)<=48){size_t i;for(i=0;room[i]&&(unsigned char)room[i]>=0x20&&room[i]!=0x7f&&room[i]!='"'&&room[i]!='\\';i++){}if(!room[i]&&i<sizeof S.room)memcpy(S.room,room,i+1);}}
 if(!S.mac[0]){if(json_get_string(b,"wifi_mac",mac,sizeof mac)>0&&mac[0]){if(normalize_mac(mac,S.mac))return -1;}else{static const char *fields[]={"mac_addr","macaddr","wifi_mac","wifi_mac_addr","mac"};int found=0;for(unsigned i=0;i<5;i++){char path[512];if(snprintf(path,sizeof path,"%s/%s/value",S.idme_path,fields[i])>=(int)sizeof path)return -1;if(config_read(path,mac,sizeof mac)>0&&!normalize_mac(mac,S.mac)){found=1;break;}}if(!found)return -1;}}
 return 0;
}
static void mdns_tick(unsigned port){if(!S.ha_selected){if(S.mdns_fd>=0){close(S.mdns_fd);S.mdns_fd=-1;}return;}if(S.mdns_fd>=0){if(le_mdns_receive(S.mdns_fd)<0){close(S.mdns_fd);S.mdns_fd=-1;S.mdns_retry=S.now+1000;}return;}if(S.now<S.mdns_retry)return;struct le_mdns_esphome_metadata m;memset(&m,0,sizeof m);snprintf(m.version,sizeof m.version,"2026.9.0");snprintf(m.mac,sizeof m.mac,"%s",S.mac);snprintf(m.platform,sizeof m.platform,"LibreEcho");snprintf(m.board,sizeof m.board,"radar-puffin");snprintf(m.network,sizeof m.network,"wifi");snprintf(m.friendly_name,sizeof m.friendly_name,"%s",S.friendly);snprintf(m.api_encryption,sizeof m.api_encryption,"%s",LE_MDNS_ESPHOME_NOISE);snprintf(m.project_name,sizeof m.project_name,"libreecho.satellite");snprintf(m.project_version,sizeof m.project_version,"0.14.0");S.mdns_fd=le_mdns_register_esphome(S.mdns_path,port,&m);S.mdns_retry=S.now+1000;}
int main(int argc,char **argv){const char *bind_addr="0.0.0.0";unsigned port=6053;struct sockaddr_in a;int listener;
 if(argc==7&&(!strcmp(argv[1],"--health-check")||!strcmp(argv[1],"--process-check"))){
  char *end;unsigned long p;if(!le_eh_decimal(argv[4]))return 2;p=strtoul(argv[4],&end,10);if(*end||p<1||p>65535)return 2;
  return le_esphome_health(argv[2],argv[3],argv[5],(unsigned)p,argv[6],!strcmp(argv[1],"--process-check"))?0:1;
 }
 memset(&S,0,sizeof S);S.owner=-1;S.mdns_fd=-1;S.muted=1;S.privacy=-1;S.radio_state=1;S.active_wake=1;S.restore_volume=66;S.turn_limit=120000;S.lux=S.lux_sent=-1;S.led_r=S.led_g=S.led_b=255;S.led_brightness=100;
 S.config_path=getenv("LE_CONFIG_PATH");if(!S.config_path||!S.config_path[0])S.config_path="/etc/libreecho/web-config.json";S.status_path="/run/libreecho/esphome-status.json";S.control_path="/run/libreecho/esphome-control.sock";S.agent_path=LE_ADAPTER_AGENT_SOCK;S.control_fd=-1;S.audio_path="/run/libreecho/audio.sock";S.wake_path="/run/libreecho/wakeword.sock";S.radio_path="/run/libreecho/radio.sock";S.timer_path="/run/libreecho/timer.sock";S.led_path="/run/libreecho/led.sock";S.mdns_path=LE_MDNS_SOCKET;S.bus_path="/run/libreecho-audio/system.pcm";S.privacy_path="/sys/devices/platform/amz_privacy/privacy_state";S.idme_path="/sys/firmware/devicetree/base/idme";
 for(unsigned i=0;i<CLIENTS;i++)S.clients[i].fd=-1;for(unsigned i=0;i<JOBS;i++)S.jobs[i].fd=-1;S.wake.fd=S.mic.fd=S.timer_cleanup.fd=-1;S.button_fd=-1;esp_playback_init(&S.playback);
 snprintf(S.button_path,sizeof S.button_path,"%s",LE_ADAPTER_BUTTON_EVENT_SOCK);
 for(int i=1;i<argc;i++){const char *v=i+1<argc?argv[i+1]:NULL;if(!strcmp(argv[i],"--plaintext")){S.plaintext=1;continue;}if(!v)return 2;
 if(!strcmp(argv[i],"--port")){char *end;unsigned long p=strtoul(v,&end,10);if(*end||p<1||p>65535)return 2;port=(unsigned)p;}
 else if(!strcmp(argv[i],"--bind")||!strcmp(argv[i],"--listen"))bind_addr=v;else if(!strcmp(argv[i],"--config"))S.config_path=v;else if(!strcmp(argv[i],"--status-file"))S.status_path=v;else if(!strcmp(argv[i],"--control-socket"))S.control_path=v;
 else if(!strcmp(argv[i],"--audio-socket"))S.audio_path=v;else if(!strcmp(argv[i],"--agent-socket"))S.agent_path=v;else if(!strcmp(argv[i],"--wake-socket"))S.wake_path=v;else if(!strcmp(argv[i],"--radio-socket"))S.radio_path=v;else if(!strcmp(argv[i],"--timer-socket"))S.timer_path=v;else if(!strcmp(argv[i],"--led-socket"))S.led_path=v;else if(!strcmp(argv[i],"--mdns-socket"))S.mdns_path=v;else if(!strcmp(argv[i],"--button-socket")){if(strlen(v)>=sizeof S.button_path)return 2;snprintf(S.button_path,sizeof S.button_path,"%s",v);}else if(!strcmp(argv[i],"--audio-bus"))S.bus_path=v;else if(!strcmp(argv[i],"--privacy-state"))S.privacy_path=v;else if(!strcmp(argv[i],"--tls-ca"))S.ca_path=v;else if(!strcmp(argv[i],"--idme-root"))S.idme_path=v;
 else if(!strcmp(argv[i],"--light-sensor")){if(!v[0]||strlen(v)>=sizeof S.light_path)return 2;snprintf(S.light_path,sizeof S.light_path,"%s",v);}
 else if(!strcmp(argv[i],"--name")){if(strlen(v)>=sizeof S.name)return 2;snprintf(S.name,sizeof S.name,"%s",v);}else if(!strcmp(argv[i],"--mac")){if(normalize_mac(v,S.mac))return 2;}
 else if(!strcmp(argv[i],"--turn-timeout-ms")){char *end;unsigned long p=strtoul(v,&end,10);if(*end||p<100||p>120000)return 2;S.turn_limit=p;}else return 2;i++;}
 if(le_voice_ns_init(&S.ns,LE_VOICE_NS_DEFAULT_ATTENUATION_DB))return 1;
 if(load_configuration()){fprintf(stderr,"esphomed: invalid config or missing stable MAC\n");return 1;}
 /* Explicit diagnostics never overrides a provisioned key. */
 memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons((uint16_t)port);if(inet_pton(AF_INET,bind_addr,&a.sin_addr)!=1)return 2;
 listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);int yes=1;if(listener<0)return 1;(void)setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);if(bind(listener,(struct sockaddr*)&a,sizeof a)||listen(listener,CLIENTS)){close(listener);return 1;}
 {struct stat st;if(fstat(listener,&st)||!le_eh_start((int)getpid(),S.process_start)||!le_eh_boot(S.boot_id)){close(listener);return 1;}snprintf(S.listener_inode,sizeof S.listener_inode,"%llu",(unsigned long long)st.st_ino);S.listener_port=port;}
 signal(SIGINT,sigstop);signal(SIGTERM,sigstop);signal(SIGHUP,sigstop);signal(SIGPIPE,SIG_IGN);S.now=now_ms();
 /* Without the button socket the satellite still runs; only button events are lost. */
 S.button_fd=button_socket_open(S.button_path);if(S.button_fd<0)fprintf(stderr,"esphomed: button event socket unavailable; hardware button events disabled\n");
 /* timerd outlives this process. Fence replacement events until a retried
  * reserved clear is ACKed, including recovery after SIGKILL/power loss. */
 timer_owner_end();S.ready=1;status_write();
 /* A previous process may have left its light overlay on the ring; HA starts from off. */
 led_apply();
 /* A set_ring overlay from before a restart must not run on: HA starts from off. */
 (void)job(S.led_path,"pattern","{\"name\":\"stop\",\"owner\":\"" SERVICE_RING_OWNER "\"}",0,0);
 while(running){struct pollfd fds[2+CLIENTS];fds[0]=(struct pollfd){listener,POLLIN,0};for(unsigned i=0;i<CLIENTS;i++)fds[i+1]=(struct pollfd){S.clients[i].fd,(short)(POLLIN|(S.clients[i].tx_n?POLLOUT:0)),0};fds[1+CLIENTS]=(struct pollfd){S.button_fd,POLLIN,0};int ready=poll(fds,2+CLIENTS,10);S.now=now_ms();if(ready<0&&errno!=EINTR)break;
 if(reload_config){reload_config=0;for(unsigned i=0;i<CLIENTS;i++)client_close(i);if(load_configuration())break;}
 if(fds[0].revents&POLLIN){for(unsigned n=0;n<4;n++){int fd=accept(listener,NULL,NULL);if(fd<0)break;int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);continue;}unsigned idx;for(idx=0;idx<CLIENTS;idx++)if(S.clients[idx].fd<0)break;if(idx==CLIENTS){close(fd);continue;}struct client *c=&S.clients[idx];memset(c,0,sizeof *c);c->fd=fd;c->mode=-1;c->deadline=S.now+15000;c->last_seen=S.now;en_init(&c->noise);}}
 for(unsigned i=0;i<CLIENTS;i++)client_tick(i,fds[i+1].revents);
 if(S.button_fd>=0&&(fds[1+CLIENTS].revents&POLLIN))button_tick();
 if(S.now>=S.settings_poll+SETTINGS_POLL_MS){S.settings_poll=S.now;(void)job(S.led_path,"status","{}",10,0);(void)job(S.wake_path,"status","{}",11,0);(void)job(S.agent_path,"voice_history_generation","{}",HISTORY_GENERATION_KIND,0);}
 if(S.now>=S.last_status_poll+250){S.last_status_poll=S.now;privacy_poll();light_poll();(void)job(S.audio_path,"status","{}",1,0);(void)job(S.radio_path,"status","{}",2,0);}
 jobs_tick();led_tick();action_ring_tick();level_tick();if(S.now>=S.wake_retry){if(S.wake.fd<0)(void)io_open(&S.wake,S.wake_path,"subscribe","{}",0);if(S.mic.fd<0)(void)io_open(&S.mic,S.wake_path,"stream_audio","{}",0);S.wake_retry=S.now+1000;}
 stream_tick(&S.mic,1);stream_tick(&S.wake,0);control_tick();capture_tick();
 if(!S.turn&&S.owner>=0)(void)server_listen_start();
 if(S.turn&&S.now>S.turn_deadline)cancel_turn("timeout");
 if(S.playing){int rc=esp_playback_tick(&S.playback,S.now);if(rc){playback_stopped();if(rc<0)turn_clear("playback_error",1);else if(S.next_url[0]){char next[1024];snprintf(next,sizeof next,"%s",S.next_url);S.next_url[0]=0;if(playback_start(next)<0)turn_clear("playback_error",1);}else completed();}}
 mdns_tick(port);
 }
 S.ready=0;for(unsigned i=0;i<CLIENTS;i++)client_close(i);
 close(listener);if(S.button_fd>=0){close(S.button_fd);unlink(S.button_path);S.button_fd=-1;}io_close(&S.wake);io_close(&S.mic);esp_playback_close(&S.playback);if(S.mdns_fd>=0)close(S.mdns_fd);if(S.control_fd>=0){close(S.control_fd);(void)unlink(S.control_path);}
 /* No new owners/events during shutdown. Retire unsent jobs, settle sent
  * ones, then clear via the reserved slot. Adapter outage must not hang exit;
  * the next startup keeps its cleanup fence until it receives an ACK. */
 S.now=now_ms();timer_owner_end();
 for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].kind!=JOB_TIMER_RETIRED)io_close(&S.jobs[i]);
 uint64_t cleanup_limit=S.now+1500;
 while(S.timer_pending&&S.now<cleanup_limit){jobs_tick();if(S.timer_pending)(void)poll(NULL,0,10);S.now=now_ms();}
 for(unsigned i=0;i<JOBS;i++)io_close(&S.jobs[i]);io_close(&S.timer_cleanup);mbedtls_platform_zeroize(S.psk,sizeof S.psk);return 0;
}
