#define _POSIX_C_SOURCE 200809L
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "esphome_playback.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mbedtls/platform_util.h>
/* Streaming reply player. The response body is unframed into a fixed window
 * and decoded while it arrives: Home Assistant opens its TTS stream once the
 * LLM has written about 60 characters, so the reply starts while the rest is
 * still being generated instead of after the last byte. DNS uses a bounded
 * IPv4 UDP transaction, not blocking libc resolution or a worker process.
 *
 * The audio engine powers its output up (about 1.2 s) when a FIFO source gets
 * data and down again after 8 idle periods (about 340 ms). Paced silence keeps
 * it running while the reply is fetched and between streamed sentences; like
 * the reply itself it ducks media. Only a FIFO bus ever gets silence. */
#define IDLE_MS 20000U        /* no network progress */
#define TOTAL_MS 180000U      /* whole slot */
/* The audio engine closes its output, powering the amplifier down, as soon as
 * a whole 2048-frame period is not queued past its ~64 ms ring (~110 ms ahead
 * of the speaker), and reopening takes ~1.3 s. Both leads stay above that. */
#define AUDIO_LEAD_MS 200U    /* audio written ahead of real time */
#define SILENCE_LEAD_MS 160U  /* silence ahead of real time: a reply waits behind no more than this */
#define SILENCE_QUEUE 40960   /* and none past this while the engine stops draining the FIFO */
#define SILENCE_FRAMES 512U   /* 2 KiB, one atomic FIFO write */
#define PREBUFFER_MS 150U     /* WAV held before the first audio */
#define REBUFFER_MS 1500U     /* and after the stream ran dry: one pause, not a stutter, when TTS is slower than real time */
#define PREROLL_MS 250U       /* silence queued before work that blocks the loop (CA parse, a TLS handshake step) */
#define PREROLL_QUEUE 49152
#define WAV_HEADER_MAX (64U*1024U)
#define MP3_SKIP_MAX (1024U*1024U)
#define MP3_WINDOW 4612U      /* two largest free-format frames and the next header: minimp3 syncs on whole frames */
enum{FORMAT_NONE,FORMAT_WAV,FORMAT_MP3};
enum{WAV_RIFF,WAV_CHUNK,WAV_DATA,WAV_TRAIL};
enum{CHUNK_SIZE,CHUNK_SIZE_LF,CHUNK_DATA,CHUNK_DATA_CR,CHUNK_DATA_LF,CHUNK_TRAILER,CHUNK_DONE};
static unsigned u16(const unsigned char *p){return p[0]|((unsigned)p[1]<<8);}
static uint32_t u32(const unsigned char *p){return u16(p)|((uint32_t)u16(p+2)<<16);}
static unsigned be16(const unsigned char *p){return ((unsigned)p[0]<<8)|p[1];}
void esp_playback_init(struct esp_playback *p){memset(p,0,sizeof *p);p->fd=p->bus_fd=p->dns_fd=-1;mbedtls_ssl_init(&p->ssl);mbedtls_ssl_config_init(&p->config);mbedtls_x509_crt_init(&p->ca);mbedtls_entropy_init(&p->entropy);mbedtls_ctr_drbg_init(&p->rng);}
void esp_playback_close(struct esp_playback *p){if(p->fd>=0)close(p->fd);if(p->bus_fd>=0)close(p->bus_fd);if(p->dns_fd>=0)close(p->dns_fd);mbedtls_ssl_free(&p->ssl);mbedtls_ssl_config_free(&p->config);mbedtls_x509_crt_free(&p->ca);mbedtls_ctr_drbg_free(&p->rng);mbedtls_entropy_free(&p->entropy);p->fd=p->bus_fd=p->dns_fd=-1;p->state=0;}
static int tls_send(void *ctx,const unsigned char *buf,size_t n){int fd=*(int*)ctx;ssize_t w=send(fd,buf,n,MSG_DONTWAIT|MSG_NOSIGNAL);if(w<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return MBEDTLS_ERR_SSL_WANT_WRITE;return w<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:(int)w;}
static int tls_recv(void *ctx,unsigned char *buf,size_t n){int fd=*(int*)ctx;ssize_t w=recv(fd,buf,n,MSG_DONTWAIT);if(w<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return MBEDTLS_ERR_SSL_WANT_READ;return w<0?MBEDTLS_ERR_SSL_INTERNAL_ERROR:(int)w;}
static int connect_ip(struct esp_playback *p,struct in_addr ip){struct sockaddr_in a;memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons((uint16_t)p->port);a.sin_addr=ip;p->fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(p->fd<0)return -1;if(connect(p->fd,(struct sockaddr*)&a,sizeof a)<0&&errno!=EINPROGRESS)return -1;p->state=2;return 0;}
static int dns_start(struct esp_playback *p){struct sockaddr_in a;FILE *f;char line[512],server[64]="";size_t z=12;const char *s=p->host,*dot;
 f=fopen("/etc/resolv.conf","r");if(!f)return -1;while(fgets(line,sizeof line,f)){if(sscanf(line,"nameserver %63s",server)==1)break;}fclose(f);
 memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons(53);if(inet_pton(AF_INET,server,&a.sin_addr)!=1)return -1;
 p->dns_id=(uint16_t)(p->deadline^(unsigned)getpid());memset(p->dns_query,0,12);p->dns_query[0]=(unsigned char)(p->dns_id>>8);p->dns_query[1]=(unsigned char)p->dns_id;p->dns_query[2]=1;p->dns_query[5]=1;
 do{dot=strchr(s,'.');size_t n=dot?(size_t)(dot-s):strlen(s);if(!n||n>63||z+n+6>sizeof p->dns_query)return -1;p->dns_query[z++]=(unsigned char)n;memcpy(p->dns_query+z,s,n);z+=n;s=dot?dot+1:NULL;}while(s);
 p->dns_query[z++]=0;p->dns_query[z++]=0;p->dns_query[z++]=1;p->dns_query[z++]=0;p->dns_query[z++]=1;p->dns_query_len=z;
 p->dns_fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(p->dns_fd<0||connect(p->dns_fd,(struct sockaddr*)&a,sizeof a)<0||send(p->dns_fd,p->dns_query,z,MSG_DONTWAIT)!=(ssize_t)z)return -1;p->state=1;return 0;
}
static int dns_name(const unsigned char *b,size_t n,size_t *pos){for(unsigned labels=0;labels<128;labels++){if(*pos>=n)return -1;unsigned z=b[(*pos)++];if(!z)return 0;if((z&192)==192){if(*pos>=n)return -1;(*pos)++;return 0;}if(z>63||z>n-*pos)return -1;*pos+=z;}return -1;}
static int dns_tick(struct esp_playback *p){unsigned char b[1024];ssize_t n=recv(p->dns_fd,b,sizeof b,MSG_DONTWAIT);size_t z=12;struct in_addr ip;if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;if(n<12||be16(b)!=p->dns_id||(b[3]&15)||!(b[2]&128)||be16(b+4)!=1||be16(b+6)>32)return -1;
 if(dns_name(b,(size_t)n,&z)||z+4>(size_t)n)return -1;z+=4;for(unsigned i=0;i<be16(b+6);i++){if(dns_name(b,(size_t)n,&z)||z+10>(size_t)n)return -1;unsigned t=be16(b+z),cl=be16(b+z+2),len=be16(b+z+8);z+=10;if(z+len>(size_t)n)return -1;if(t==1&&cl==1&&len==4){memcpy(&ip,b+z,4);close(p->dns_fd);p->dns_fd=-1;return connect_ip(p,ip);}z+=len;}return -1;
}
static void preroll(struct esp_playback *,uint64_t);
static int bus_open(struct esp_playback *p){struct stat st;if(p->bus_fd>=0)return 0;p->bus_fd=open(p->bus,O_WRONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);if(p->bus_fd<0)return -1;p->fifo=!fstat(p->bus_fd,&st)&&S_ISFIFO(st.st_mode);return 0;}
int esp_playback_start(struct esp_playback *p,const char *url,const char *bus,const char *ca,uint64_t now){const char *s,*slash,*colon;size_t host_n;struct in_addr ip;char hostport[280];int warm=-1,clock;uint64_t origin,frames;
 if(p->state)return -1;
 /* Keep a warmed bus and its pacing: the reply follows the silence without a gap or a burst. */
 if(p->bus_fd>=0&&p->fifo&&!strcmp(p->bus,bus)){warm=p->bus_fd;p->bus_fd=-1;}clock=p->clock;origin=p->origin;frames=p->frames;
 esp_playback_close(p);esp_playback_init(p);if(warm>=0){p->bus_fd=warm;p->fifo=1;p->clock=clock;p->origin=origin;p->frames=frames;}
 if(!strncmp(url,"http://",7)){s=url+7;p->port=80;}else if(!strncmp(url,"https://",8)){s=url+8;p->port=443;p->tls=1;}else return -1;
 for(const char *v=s;*v;v++)if((unsigned char)*v<33||(unsigned char)*v>126||*v=='@'||*v=='\\'||*v=='#')return -1;
 slash=strchr(s,'/');host_n=slash?(size_t)(slash-s):strlen(s);if(!host_n||host_n>=sizeof hostport||strlen(bus)>=sizeof p->bus)return -1;memcpy(hostport,s,host_n);hostport[host_n]=0;
 colon=strchr(hostport,':');if(colon){char *end;unsigned long port=strtoul(colon+1,&end,10);if(!colon[1]||*end||port<1||port>65535)return -1;p->port=(unsigned)port;host_n=(size_t)(colon-hostport);}
 if(!host_n||host_n>=sizeof p->host)return -1;memcpy(p->host,hostport,host_n);p->host[host_n]=0;
 for(size_t i=0;i<host_n;i++)if(!(p->host[i]=='.'||p->host[i]=='-'||(p->host[i]>='0'&&p->host[i]<='9')||(p->host[i]>='a'&&p->host[i]<='z')||(p->host[i]>='A'&&p->host[i]<='Z')))return -1;
 if(slash&&strlen(slash)>=sizeof p->path)return -1;snprintf(p->path,sizeof p->path,"%s",slash?slash:"/");snprintf(p->bus,sizeof p->bus,"%s",bus);
 /* Best effort now, so the output can warm up during the fetch; audio retries and fails without it. */
 (void)bus_open(p);
 int n=snprintf(p->request,sizeof p->request,"GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept: audio/wav, audio/mpeg\r\n\r\n",p->path,p->host,p->port);if(n<0||(size_t)n>=sizeof p->request)return -1;p->request_len=(size_t)n;p->expected=SIZE_MAX;p->deadline=now+TOTAL_MS;p->idle_deadline=now+IDLE_MS;p->buffering=1;
 if(p->tls){preroll(p,now);if(!ca)ca="/etc/ssl/certs/ca-certificates.crt";if(mbedtls_x509_crt_parse_file(&p->ca,ca)||mbedtls_ctr_drbg_seed(&p->rng,mbedtls_entropy_func,&p->entropy,(const unsigned char*)"libreecho-https",14)||mbedtls_ssl_config_defaults(&p->config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return -1;mbedtls_ssl_conf_authmode(&p->config,MBEDTLS_SSL_VERIFY_REQUIRED);mbedtls_ssl_conf_ca_chain(&p->config,&p->ca,NULL);mbedtls_ssl_conf_rng(&p->config,mbedtls_ctr_drbg_random,&p->rng);if(mbedtls_ssl_setup(&p->ssl,&p->config)||mbedtls_ssl_set_hostname(&p->ssl,p->host))return -1;mbedtls_ssl_set_bio(&p->ssl,&p->fd,tls_send,tls_recv,NULL);}
 if(inet_pton(AF_INET,p->host,&ip)==1)return connect_ip(p,ip);return dns_start(p);
}
static int headers(struct esp_playback *p){size_t i;for(i=0;i+3<p->wire_n&&i<8192;i++)if(!memcmp(p->wire+i,"\r\n\r\n",4))break;if(i+3>=p->wire_n)return p->wire_n>=8192?-1:0;
 if(i+4>8192||memchr(p->wire,0,i+4))return -1;char h[8193];memcpy(h,p->wire,i+4);h[i+4]=0;if(strncmp(h,"HTTP/1.1 200 ",13)&&strncmp(h,"HTTP/1.0 200 ",13))return -1;
 char *line=strstr(h,"\r\n")+2;int have_len=0;while(*line&&strncmp(line,"\r\n",2)){char *end=strstr(line,"\r\n");if(!end)return -1;*end=0;
 if(!strncasecmp(line,"Content-Length:",15)){char *v=line+15,*q;while(*v==' ')v++;if(have_len++||*v<'0'||*v>'9')return -1;unsigned long long z=strtoull(v,&q,10);if(*q||z>ESP_PLAY_BODY_MAX)return -1;p->expected=(size_t)z;}
 if(!strncasecmp(line,"Transfer-Encoding:",18)){char *v=line+18;while(*v==' ')v++;if(p->chunked||strcasecmp(v,"chunked"))return -1;p->chunked=1;}
 if(!strncasecmp(line,"Content-Encoding:",17))return -1;line=end+2;}
 if(p->chunked&&have_len)return -1;
 p->wire_pos=i+4;p->header_done=1;return 1;
}
/* Move received body bytes into the payload window, removing chunked framing
 * incrementally. Stops when the window is full; the rest waits in wire. */
static int unframe(struct esp_playback *p){
 if(p->pay_pos&&(p->pay_n==sizeof p->payload||p->pay_pos>=sizeof p->payload/2)){memmove(p->payload,p->payload+p->pay_pos,p->pay_n-p->pay_pos);p->pay_n-=p->pay_pos;p->pay_pos=0;}
 while(p->wire_pos<p->wire_n){const unsigned char *w=p->wire+p->wire_pos;size_t in=p->wire_n-p->wire_pos,room=sizeof p->payload-p->pay_n,n;unsigned c=*w,v;
  if(!p->chunked||p->chunk_stage==CHUNK_DATA){n=in<room?in:room;if(p->chunked&&n>p->chunk_left)n=(size_t)p->chunk_left;if(!n)break;
   if(n>ESP_PLAY_BODY_MAX-p->body_total||(p->expected!=SIZE_MAX&&n>p->expected-p->body_total))return -1;
   memcpy(p->payload+p->pay_n,w,n);p->pay_n+=n;p->wire_pos+=n;p->body_total+=n;if(p->chunked&&!(p->chunk_left-=n))p->chunk_stage=CHUNK_DATA_CR;continue;}
  p->wire_pos++;
  switch(p->chunk_stage){
  case CHUNK_SIZE:if(c=='\r'){if(!p->chunk_digits)return -1;p->chunk_stage=CHUNK_SIZE_LF;break;}
   if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else if(c>='A'&&c<='F')v=c-'A'+10;else return -1;
   if(p->chunk_left>(ESP_PLAY_BODY_MAX-v)/16||++p->chunk_digits>16)return -1;p->chunk_left=p->chunk_left*16+v;break;
  case CHUNK_SIZE_LF:if(c!='\n'||p->chunk_left>ESP_PLAY_BODY_MAX-p->body_total)return -1;p->chunk_digits=0;p->chunk_stage=p->chunk_left?CHUNK_DATA:CHUNK_TRAILER;break;
  case CHUNK_DATA_CR:if(c!='\r')return -1;p->chunk_stage=CHUNK_DATA_LF;break;
  case CHUNK_DATA_LF:if(c!='\n')return -1;p->chunk_stage=CHUNK_SIZE;break;
  case CHUNK_TRAILER:if(++p->trailer_bytes>8192)return -1;
   if(p->trailer_cr){if(c!='\n')return -1;p->trailer_cr=0;if(!p->trailer_line){p->chunk_stage=CHUNK_DONE;break;}if(!p->trailer_colon)return -1;p->trailer_line=0;p->trailer_colon=0;break;}
   if(c=='\r'){p->trailer_cr=1;break;}if(!c||c=='\n')return -1;if(c==':')p->trailer_colon=1;p->trailer_line++;break;
  default:return -1;}
 }
 if(p->wire_pos==p->wire_n)p->wire_pos=p->wire_n=0;return 0;
}
/* The body is complete once its framing says so; a truncated one fails. */
static int net_complete(struct esp_playback *p){int drained=p->wire_pos==p->wire_n;if(p->net_done)return 0;
 if((p->chunked&&p->chunk_stage==CHUNK_DONE)||(!p->chunked&&p->expected!=SIZE_MAX&&p->body_total==p->expected)||(p->net_eof&&drained&&!p->chunked&&p->expected==SIZE_MAX)){if(p->fd>=0)close(p->fd);p->fd=-1;p->net_done=1;return 0;}
 return p->net_eof&&drained?-1:0;
}
/* One network step: 1 progress, 0 nothing to do now, -1 failed. */
static int net_once(struct esp_playback *p,uint64_t now){int rc;
 if(p->state==1){if(dns_tick(p)<0)return -1;return p->state!=1;}
 if(p->state==2){struct sockaddr_in a;socklen_t n=sizeof a;if(getpeername(p->fd,(struct sockaddr*)&a,&n)<0){int err=0;n=sizeof err;if(getsockopt(p->fd,SOL_SOCKET,SO_ERROR,&err,&n)<0||err)return -1;return 0;}p->state=p->tls?3:4;return 1;}
 if(p->state==3){preroll(p,now);rc=mbedtls_ssl_handshake(&p->ssl);if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;if(rc||mbedtls_ssl_get_verify_result(&p->ssl))return -1;p->state=4;return 1;}
 if(p->state==4){size_t left=p->request_len-p->request_sent;rc=p->tls?mbedtls_ssl_write(&p->ssl,(unsigned char*)p->request+p->request_sent,left):tls_send(&p->fd,(unsigned char*)p->request+p->request_sent,left);if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;if(rc<=0)return -1;p->request_sent+=(size_t)rc;if(p->request_sent==p->request_len)p->state=5;return 1;}
 if(p->state!=5||p->net_done||p->net_eof)return 0;
 if(p->wire_pos){memmove(p->wire,p->wire+p->wire_pos,p->wire_n-p->wire_pos);p->wire_n-=p->wire_pos;p->wire_pos=0;}
 size_t cap=sizeof p->wire-p->wire_n;if(!cap){p->idle_deadline=now+IDLE_MS;return 0;}/* the decoder is the bottleneck */
 rc=p->tls?mbedtls_ssl_read(&p->ssl,p->wire+p->wire_n,cap):tls_recv(&p->fd,p->wire+p->wire_n,cap);if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;if(rc==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)rc=0;if(rc<0)return -1;
 p->idle_deadline=now+IDLE_MS;if(rc)p->wire_n+=(size_t)rc;else p->net_eof=1;
 if(!p->header_done){int h=headers(p);if(h<0)return -1;if(!h)return p->net_eof?-1:1;}
 if(unframe(p)<0||net_complete(p)<0)return -1;return !p->net_eof&&!p->net_done;
}
static int wav_take(struct esp_playback *p,size_t n){p->pay_pos+=n;p->wav_bytes+=n;return p->riff!=UINT32_MAX&&p->wav_bytes>(uint64_t)p->riff+8?-1:0;}
/* Walk WAV chunks up to the data chunk, then whole chunks after it.
 * 1 data is next, 0 needs bytes, -1 invalid. fmt must precede data. */
static int wav_chunks(struct esp_playback *p){
 for(;;){size_t n=p->pay_n-p->pay_pos;const unsigned char *b=p->payload+p->pay_pos;
  if(p->skip){size_t z=n<p->skip?n:(size_t)p->skip;if(!z)return 0;if(wav_take(p,z))return -1;p->skip-=z;continue;}
  if(p->wav_stage==WAV_DATA)return 1;
  if(p->wav_stage==WAV_RIFF){if(n<12)return 0;if(memcmp(b,"RIFF",4)||memcmp(b+8,"WAVE",4))return -1;p->riff=u32(b+4);if(wav_take(p,12))return -1;p->wav_stage=WAV_CHUNK;continue;}
  if(n<8)return 0;
  uint32_t l=u32(b+4);
  if(p->wav_stage==WAV_CHUNK&&!memcmp(b,"data",4)){if(!p->rate||(l!=UINT32_MAX&&l%(2U*(unsigned)p->channels)))return -1;if(wav_take(p,8))return -1;p->data_unbounded=l==UINT32_MAX;p->data_left=l;p->wav_stage=WAV_DATA;continue;}
  if(l==UINT32_MAX)return -1;
  if(p->wav_stage==WAV_CHUNK&&!memcmp(b,"fmt ",4)){const unsigned char *f=b+8;unsigned channels;uint32_t rate;if(p->rate||l<16||l>64)return -1;if(n<8+(size_t)l)return 0;channels=u16(f+2);rate=u32(f+4);
   if(u16(f)!=1||u16(f+14)!=16||channels<1||channels>2||rate<8000||rate>48000||u16(f+12)!=channels*2)return -1;p->channels=(int)channels;p->rate=(int)rate;if(wav_take(p,8+(size_t)l))return -1;p->skip=l&1;continue;}
  if(p->wav_stage==WAV_CHUNK&&p->wav_bytes+8+(uint64_t)l+(l&1)>WAV_HEADER_MAX)return -1;
  if(wav_take(p,8))return -1;p->skip=(uint64_t)l+(l&1);
 }
}
/* One decode step: 1 progress (output may be staged), 0 needs input,
 * 2 input finished and valid, -1 invalid. */
static int decode(struct esp_playback *p){short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];int frames=0,channels=0,rate=0,end=p->net_done,rc;size_t n=p->pay_n-p->pay_pos;const unsigned char *b=p->payload+p->pay_pos;
 if(p->format==FORMAT_NONE){if(n<12&&!end)return 0;
  if(n>=12&&!memcmp(b,"RIFF",4)&&!memcmp(b+8,"WAVE",4)){p->format=FORMAT_WAV;p->riff=UINT32_MAX;}
  else{if(n>=3&&!memcmp(b,"ID3",3)){if(n<10||((b[6]|b[7]|b[8]|b[9])&128))return -1;p->skip=10+((uint64_t)b[6]<<21)+((uint64_t)b[7]<<14)+((uint64_t)b[8]<<7)+b[9];}p->format=FORMAT_MP3;mp3dec_init(&p->decoder);}}
 if(p->format==FORMAT_WAV){int h=wav_chunks(p);if(h<0)return -1;n=p->pay_n-p->pay_pos;b=p->payload+p->pay_pos;
  if(h==1){size_t frame=2U*(unsigned)p->channels,count;if(!p->data_unbounded&&n>p->data_left)n=(size_t)p->data_left;
   if(p->buffering&&!end&&n<(size_t)p->rate*frame*(p->buffering==2?REBUFFER_MS:PREBUFFER_MS)/1000&&(p->data_unbounded||n<p->data_left))return 0;
   count=n/frame;if(count>1024)count=1024;
   if(count){for(size_t i=0;i<count*(size_t)p->channels;i++)pcm[i]=(short)u16(b+2*i);if(wav_take(p,count*frame))return -1;if(!p->data_unbounded&&!(p->data_left-=count*frame))p->wav_stage=WAV_TRAIL;frames=(int)count;channels=p->channels;rate=p->rate;}}
  if(!frames){if(!end)return 0;n=p->pay_n-p->pay_pos;
   /* Unbounded data ends on a frame; finite data and the chunks after it end whole. */
   if(n||p->skip||!((p->wav_stage==WAV_DATA&&p->data_unbounded)||p->wav_stage==WAV_TRAIL))return -1;
   if(p->riff!=UINT32_MAX&&p->wav_bytes!=(uint64_t)p->riff+8)return -1;return p->input_frames?2:-1;}
 }else for(unsigned guard=0;!frames;guard++){mp3dec_frame_info_t info;if(guard==8)return 1;n=p->pay_n-p->pay_pos;b=p->payload+p->pay_pos;
  if(p->skip){size_t z=n<p->skip?n:(size_t)p->skip;if(!z)return end?-1:0;p->pay_pos+=z;p->skip-=z;if((p->mp3_skipped+=z)>MP3_SKIP_MAX)return -1;continue;}
  if(!n)return end?(p->input_frames?2:-1):0;
  if(n<MP3_WINDOW&&!end)return 0;
  frames=mp3dec_decode_frame(&p->decoder,b,(int)(n>65536?65536:n),pcm,&info);if(info.frame_bytes<=0)return -1;p->pay_pos+=(size_t)info.frame_bytes;
  if(!frames){if((p->mp3_skipped+=(uint64_t)info.frame_bytes)>MP3_SKIP_MAX)return -1;continue;}
  channels=info.channels;rate=info.hz;if(rate<8000||rate>48000||channels<1||channels>2)return -1;}
 if(p->source_rate&&p->source_rate!=rate)return -1;p->source_rate=rate;p->input_frames+=(unsigned)frames;
 /* A tiny streamed block may not yet complete an output frame. */
 rc=le_radio_resample(&p->resampler,pcm,frames,channels,rate,p->output,8192);if(rc<0)return -1;p->output_len=(size_t)rc*4;p->output_sent=0;p->buffering=0;return 1;
}
/* Never catch up a deficit: after a late tick or a stalled engine, real time
 * restarts from what was written, so the queue (and the reply) is not delayed. */
static void pace(struct esp_playback *p,uint64_t now){uint64_t written=p->frames*1000/48000;
 if(!p->clock){p->clock=1;p->origin=now;return;}
 if(now-p->origin>written){if(p->audio_frames&&!p->buffering)p->underruns++;p->origin=now-written;}
}
static int silence(struct esp_playback *p,uint64_t now){static const unsigned char zero[SILENCE_FRAMES*4];int queued=0;
 if(p->bus_fd<0||!p->fifo)return 0;pace(p,now);if(p->frames*1000/48000>now-p->origin+SILENCE_LEAD_MS)return 0;
 if(ioctl(p->bus_fd,FIONREAD,&queued)||queued>=SILENCE_QUEUE||write(p->bus_fd,zero,sizeof zero)!=(ssize_t)sizeof zero)return 0;
 p->frames+=SILENCE_FRAMES;if(p->audio_frames&&!p->buffering){p->underruns++;p->buffering=2;}return 1;
}
/* Before blocking work: queue enough silence that the engine, which powers its
 * output down after ~160-340 ms without data, keeps running through it. */
static void preroll(struct esp_playback *p,uint64_t now){static const unsigned char zero[SILENCE_FRAMES*4];int queued=0;
 if(p->bus_fd<0||!p->fifo)return;pace(p,now);
 while(p->frames*1000/48000<now-p->origin+PREROLL_MS&&!ioctl(p->bus_fd,FIONREAD,&queued)&&queued<PREROLL_QUEUE&&write(p->bus_fd,zero,sizeof zero)==(ssize_t)sizeof zero)p->frames+=SILENCE_FRAMES;
}
void esp_playback_warm(struct esp_playback *p,const char *bus,uint64_t now){
 if(p->state||strlen(bus)>=sizeof p->bus)return;
 if(p->bus_fd<0){if(p->warm_off&&!strcmp(p->bus,bus))return;snprintf(p->bus,sizeof p->bus,"%s",bus);p->warm_off=0;if(bus_open(p))return;
  if(!p->fifo){close(p->bus_fd);p->bus_fd=-1;p->warm_off=1;return;}p->clock=0;p->frames=p->audio_frames=0;p->underruns=0;}
 for(unsigned step=0;step<8&&silence(p,now);step++){}
}
/* Resampler tail, then wait for the engine to drain what was written. */
static int finish(struct esp_playback *p){uint64_t desired;
 if(!p->input_frames||!p->source_rate)return -1;desired=(p->input_frames*48000+(unsigned)p->source_rate-1)/(unsigned)p->source_rate;
 if(p->audio_frames<desired){uint64_t left=desired-p->audio_frames;short pad[6];int tail;if(left>32)return -1;for(unsigned i=0;i<3;i++){pad[2*i]=p->resampler.history[4];pad[2*i+1]=p->resampler.history[5];}tail=le_radio_resample(&p->resampler,pad,3,2,p->source_rate,p->output,(int)left);if(tail<=0)return -1;p->output_len=(size_t)tail*4;p->output_sent=0;return 1;}
 p->state=7;p->drain_until=p->origin+(p->frames*1000+47999)/48000+40;return 2;
}
/* One playback step: 1 progress, 0 waiting, 2 finished, -1 failed. */
static int play_once(struct esp_playback *p,uint64_t now){int rc;
 if(p->output_sent<p->output_len){ssize_t got;if(p->bus_fd<0&&bus_open(p))return -1;pace(p,now);if(p->frames*1000/48000>now-p->origin+AUDIO_LEAD_MS)return 0;
  got=write(p->bus_fd,(unsigned char*)p->output+p->output_sent,p->output_len-p->output_sent);if(got<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;if(got<=0)return -1;
  p->output_sent+=(size_t)got;if(p->output_sent<p->output_len)return 1;p->frames+=p->output_len/4;p->audio_frames+=p->output_len/4;p->output_len=p->output_sent=0;return 1;}
 if(p->header_done){rc=decode(p);if(rc<0)return -1;if(rc==1)return 1;if(rc==2)return finish(p);}
 return silence(p,now);
}
int esp_playback_tick(struct esp_playback *p,uint64_t now){
 if(!p->state)return p->result?p->result:-1;if(now>p->deadline||(!p->net_done&&now>p->idle_deadline))goto fail;
 if(p->state==7){int remaining=0;if(now<p->drain_until)return 0;if(ioctl(p->bus_fd,FIONREAD,&remaining)==0&&remaining>0)return 0;p->result=1;esp_playback_close(p);return 1;}
 for(unsigned step=0;step<32;step++){int rc=net_once(p,now);if(rc<0)goto fail;if(!rc)break;}
 /* esphomed's loop wakes about every 20 ms (HZ=100): keep stepping until the
  * pacing lead is full, the bus would block or input runs out. Each step
  * refills the payload window from bytes already received; the bound keeps one
  * tick from monopolising the loop. */
 for(unsigned step=0;step<64&&p->state!=7;step++){int rc=play_once(p,now);if(rc<0)goto fail;if(p->header_done&&!p->net_done&&(unframe(p)<0||net_complete(p)<0))goto fail;if(!rc)break;}
 return 0;
 fail:p->result=-1;esp_playback_close(p);return -1;
}
