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
#include <unistd.h>
#include <mbedtls/platform_util.h>
/* Finite cache followed by bounded decode/write ticks. DNS uses a bounded
 * IPv4 UDP transaction, not blocking libc resolution or a worker process. */
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
int esp_playback_start(struct esp_playback *p,const char *url,const char *bus,const char *ca,uint64_t now){const char *s,*slash,*colon;size_t host_n;struct in_addr ip;char hostport[280];
 if(p->state)return -1;esp_playback_close(p);esp_playback_init(p);
 if(!strncmp(url,"http://",7)){s=url+7;p->port=80;}else if(!strncmp(url,"https://",8)){s=url+8;p->port=443;p->tls=1;}else return -1;
 for(const char *v=s;*v;v++)if((unsigned char)*v<33||(unsigned char)*v>126||*v=='@'||*v=='\\'||*v=='#')return -1;
 slash=strchr(s,'/');host_n=slash?(size_t)(slash-s):strlen(s);if(!host_n||host_n>=sizeof hostport||strlen(bus)>=sizeof p->bus)return -1;memcpy(hostport,s,host_n);hostport[host_n]=0;
 colon=strchr(hostport,':');if(colon){char *end;unsigned long port=strtoul(colon+1,&end,10);if(!colon[1]||*end||port<1||port>65535)return -1;p->port=(unsigned)port;host_n=(size_t)(colon-hostport);}
 if(!host_n||host_n>=sizeof p->host)return -1;memcpy(p->host,hostport,host_n);p->host[host_n]=0;
 for(size_t i=0;i<host_n;i++)if(!(p->host[i]=='.'||p->host[i]=='-'||(p->host[i]>='0'&&p->host[i]<='9')||(p->host[i]>='a'&&p->host[i]<='z')||(p->host[i]>='A'&&p->host[i]<='Z')))return -1;
 if(slash&&strlen(slash)>=sizeof p->path)return -1;snprintf(p->path,sizeof p->path,"%s",slash?slash:"/");snprintf(p->bus,sizeof p->bus,"%s",bus);
 int n=snprintf(p->request,sizeof p->request,"GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\nAccept: audio/wav, audio/mpeg\r\n\r\n",p->path,p->host,p->port);if(n<0||(size_t)n>=sizeof p->request)return -1;p->request_len=(size_t)n;p->expected=SIZE_MAX;p->deadline=now+15000;
 if(p->tls){if(!ca)ca="/etc/ssl/certs/ca-certificates.crt";if(mbedtls_x509_crt_parse_file(&p->ca,ca)||mbedtls_ctr_drbg_seed(&p->rng,mbedtls_entropy_func,&p->entropy,(const unsigned char*)"libreecho-https",14)||mbedtls_ssl_config_defaults(&p->config,MBEDTLS_SSL_IS_CLIENT,MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT))return -1;mbedtls_ssl_conf_authmode(&p->config,MBEDTLS_SSL_VERIFY_REQUIRED);mbedtls_ssl_conf_ca_chain(&p->config,&p->ca,NULL);mbedtls_ssl_conf_rng(&p->config,mbedtls_ctr_drbg_random,&p->rng);if(mbedtls_ssl_setup(&p->ssl,&p->config)||mbedtls_ssl_set_hostname(&p->ssl,p->host))return -1;mbedtls_ssl_set_bio(&p->ssl,&p->fd,tls_send,tls_recv,NULL);}
 if(inet_pton(AF_INET,p->host,&ip)==1)return connect_ip(p,ip);return dns_start(p);
}
static int headers(struct esp_playback *p){size_t i;for(i=0;i+3<p->received&&i<8192;i++)if(!memcmp(p->buffer+i,"\r\n\r\n",4))break;if(i+3>=p->received)return p->received>=8192?-1:0;
 if(i+4>8192||memchr(p->buffer,0,i+4))return -1;char h[8193];memcpy(h,p->buffer,i+4);h[i+4]=0;if(strncmp(h,"HTTP/1.1 200 ",13)&&strncmp(h,"HTTP/1.0 200 ",13))return -1;
 char *line=strstr(h,"\r\n")+2;int have_len=0;while(*line&&strncmp(line,"\r\n",2)){char *end=strstr(line,"\r\n");if(!end)return -1;*end=0;
 if(!strncasecmp(line,"Content-Length:",15)){char *v=line+15,*q;while(*v==' ')v++;if(have_len++||*v<'0'||*v>'9')return -1;unsigned long long z=strtoull(v,&q,10);if(*q||z>ESP_PLAY_BODY_MAX)return -1;p->expected=(size_t)z;}
 if(!strncasecmp(line,"Transfer-Encoding:",18)){char *v=line+18;while(*v==' ')v++;if(p->chunked||strcasecmp(v,"chunked"))return -1;p->chunked=1;}
 if(!strncasecmp(line,"Content-Encoding:",17))return -1;line=end+2;}
 if(p->chunked&&have_len)return -1;
 p->body_start=i+4;p->header_done=1;return 1;
}
/* HA early TTS URLs can be chunked. Cache the finite wire body within the same
 * hard cap/deadline, then compact it in place at EOF; never decode partial audio. */
static int unchunk(struct esp_playback *p){unsigned char *b=p->buffer+p->body_start;size_t n=p->body_len,in=0,out=0;
 while(in<n){size_t line=in,length=0;unsigned digits=0;while(in<n&&b[in]!='\r'){unsigned c=b[in],v;if(c>='0'&&c<='9')v=c-'0';else if(c>='a'&&c<='f')v=c-'a'+10;else if(c>='A'&&c<='F')v=c-'A'+10;else return -1;if(length>(ESP_PLAY_BODY_MAX-v)/16||++digits>16)return -1;length=length*16+v;in++;}
 if(!digits||in+2>n||b[in+1]!='\n'||in-line>16)return -1;in+=2;
 if(!length){size_t trailers=in;while(in+2<=n){size_t start=in;while(in+2<=n&&(b[in]!='\r'||b[in+1]!='\n')){if(!b[in]||b[in]=='\r'||b[in]=='\n')return -1;in++;}if(in+2>n||in-trailers>8192)return -1;if(in==start){in+=2;if(in!=n)return -1;p->body_len=out;return 0;}if(!memchr(b+start,':',in-start))return -1;in+=2;}return -1;}
 if(length>n-in||n-in-length<2||b[in+length]!='\r'||b[in+length+1]!='\n'||length>ESP_PLAY_BODY_MAX-out)return -1;memmove(b+out,b+in,length);out+=length;in+=length+2;
 }return -1;
}
/* Called only after the finite HTTP body is complete (and unchunked). ffmpeg's
 * nonseekable WAV sizes may be unknown, but the cached body must remain bounded. */
static int prepare(struct esp_playback *p,uint64_t now){unsigned char *b=p->buffer+p->body_start;size_t n=p->body_len,z=12;int fmt=0,data=0;
 if(n>ESP_PLAY_BODY_MAX)return -1;
 if(n>=12&&!memcmp(b,"RIFF",4)&&!memcmp(b+8,"WAVE",4)){p->wav=1;if(u32(b+4)!=UINT32_MAX&&u32(b+4)!=n-8)return -1;
 while(z+8<=n){size_t l=u32(b+z+4),start=z+8;if(!memcmp(b+z,"data",4)&&l==UINT32_MAX)l=n-start;if(l>n-start||((l&1)&&l==n-start))return -1;if(!memcmp(b+z,"fmt ",4)){if(l<16||u16(b+start)!=1||u16(b+start+14)!=16)return -1;p->channels=(int)u16(b+start+2);p->rate=(int)u32(b+start+4);if(p->channels<1||p->channels>2||p->rate<8000||p->rate>48000||u16(b+start+12)!=(unsigned)p->channels*2)return -1;fmt=1;}if(!memcmp(b+z,"data",4)){p->pcm_offset=start;p->pcm_bytes=l;data=1;}z=start+l+(l&1);}
 if(z!=n||!fmt||!data||p->pcm_bytes%(2*p->channels))return -1;p->position=p->pcm_offset;
 }else{mp3dec_init(&p->decoder);if(n>=10&&!memcmp(b,"ID3",3)){if((b[6]|b[7]|b[8]|b[9])&128)return -1;p->position=10+((size_t)b[6]<<21)+((size_t)b[7]<<14)+((size_t)b[8]<<7)+b[9];if(p->position>=n)return -1;}}
 le_radio_resample_reset(&p->resampler);p->bus_fd=open(p->bus,O_WRONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);if(p->bus_fd<0)return -1;p->origin=now;p->deadline=now+120000;p->state=6;return 0;
}
static int tick_once(struct esp_playback *p,uint64_t now){int rc;ssize_t got;unsigned char *b;if(!p->state)return p->result? p->result:-1;if(now>p->deadline)goto fail;
 if(p->state==1){if(dns_tick(p)<0)goto fail;return 0;}
 if(p->state==2){struct sockaddr_in a;socklen_t n=sizeof a;if(getpeername(p->fd,(struct sockaddr*)&a,&n)<0){int err=0;n=sizeof err;if(getsockopt(p->fd,SOL_SOCKET,SO_ERROR,&err,&n)<0||err)goto fail;return 0;}p->state=p->tls?3:4;}
 if(p->state==3){rc=mbedtls_ssl_handshake(&p->ssl);if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;if(rc||mbedtls_ssl_get_verify_result(&p->ssl))goto fail;p->state=4;}
 if(p->state==4){size_t left=p->request_len-p->request_sent;rc=p->tls?mbedtls_ssl_write(&p->ssl,(unsigned char*)p->request+p->request_sent,left):tls_send(&p->fd,(unsigned char*)p->request+p->request_sent,left);if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;if(rc<=0)goto fail;p->request_sent+=(size_t)rc;if(p->request_sent==p->request_len)p->state=5;return 0;}
 if(p->state==5){size_t cap=sizeof p->buffer-p->received;if(!cap)goto fail;if(cap>8192)cap=8192;rc=p->tls?mbedtls_ssl_read(&p->ssl,p->buffer+p->received,cap):tls_recv(&p->fd,p->buffer+p->received,cap);if(rc==MBEDTLS_ERR_SSL_WANT_READ||rc==MBEDTLS_ERR_SSL_WANT_WRITE)return 0;if(rc==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)rc=0;if(rc<0)goto fail;p->received+=(size_t)rc;
 if(!p->header_done){int h=headers(p);if(h<0)goto fail;if(!h){if(!rc)goto fail;return 0;}}
 p->body_len=p->received-p->body_start;if(p->body_len>ESP_PLAY_BODY_MAX|| (p->expected!=SIZE_MAX&&p->body_len>p->expected))goto fail;
 if((p->expected!=SIZE_MAX&&p->body_len==p->expected)||!rc){if(p->expected!=SIZE_MAX&&p->body_len!=p->expected)goto fail;if(p->chunked&&unchunk(p))goto fail;close(p->fd);p->fd=-1;if(prepare(p,now)<0)goto fail;}return 0;
 }
 if(p->state==7){int remaining=0;if(now<p->drain_until)return 0;if(ioctl(p->bus_fd,FIONREAD,&remaining)==0&&remaining>0)return 0;p->result=1;esp_playback_close(p);return 1;}
 if(p->state!=6)goto fail;
 if(p->output_sent<p->output_len){if(p->frames*1000/48000>now-p->origin+100)return 0;got=write(p->bus_fd,(unsigned char*)p->output+p->output_sent,p->output_len-p->output_sent);if(got<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return 0;if(got<=0)goto fail;p->output_sent+=(size_t)got;if(p->output_sent<p->output_len)return 0;p->frames+=p->output_len/4;p->output_len=p->output_sent=0;return 0;}
 b=p->buffer+p->body_start;size_t end=p->wav?p->pcm_offset+p->pcm_bytes:p->body_len;if(p->position>=end){if(!p->frames||!p->source_rate)goto fail;
 uint64_t desired=(p->input_frames*48000+(unsigned)p->source_rate-1)/(unsigned)p->source_rate;
 if(p->frames<desired){uint64_t left=desired-p->frames;if(left>32)goto fail;short pad[6];for(unsigned i=0;i<3;i++){pad[2*i]=p->resampler.history[4];pad[2*i+1]=p->resampler.history[5];}int tail=le_radio_resample(&p->resampler,pad,3,2,p->source_rate,p->output,(int)left);if(tail<=0)goto fail;p->output_len=(size_t)tail*4;p->output_sent=0;return 0;}
 p->state=7;p->drain_until=p->origin+(p->frames*1000+47999)/48000+40;return 0;}
 {short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];int frames,channels,rate;
 if(p->wav){size_t count=(end-p->position)/(2*p->channels);if(count>1024)count=1024;for(size_t i=0;i<count*(size_t)p->channels;i++)pcm[i]=(short)u16(b+p->position+2*i);p->position+=count*2*p->channels;frames=(int)count;channels=p->channels;rate=p->rate;}
 else{mp3dec_frame_info_t info;frames=mp3dec_decode_frame(&p->decoder,b+p->position,(int)((end-p->position)>65536?65536:end-p->position),pcm,&info);if(info.frame_bytes<=0)goto fail;p->position+=(size_t)info.frame_bytes;if(!frames)return 0;channels=info.channels;rate=info.hz;if(rate<8000||rate>48000||channels<1||channels>2)goto fail;}
 if(p->source_rate&&p->source_rate!=rate)goto fail;p->source_rate=rate;p->input_frames+=(unsigned)frames;rc=le_radio_resample(&p->resampler,pcm,frames,channels,rate,p->output,8192);if(rc<=0)goto fail;p->output_len=(size_t)rc*4;p->output_sent=0;return 0;}
 fail:p->result=-1;esp_playback_close(p);return -1;
}
/* One decode step (one MP3 frame or <=1024 WAV frames) and its bus write
 * happen on separate tick_once() calls. At 22.05 kHz that is only 13-46 ms of
 * 48 kHz output per two calls, while esphomed's poll loop runs every ~20 ms on
 * a HZ=100 kernel, so a single step per daemon tick fell behind real time and
 * the engine closed and reopened the PCM every couple of seconds. Keep
 * stepping until the pacing lead is full, the bus would block or the state
 * changes; the step bound keeps one tick from monopolising the loop. */
int esp_playback_tick(struct esp_playback *p,uint64_t now){int playing=p->state==6,rc=tick_once(p,now);
 /* Only an already-playing slot catches up; fetch/prepare transitions keep
  * their one-step-per-tick behaviour. */
 for(unsigned step=0;playing&&!rc&&p->state==6&&step<64;step++){size_t position=p->position,sent=p->output_sent,len=p->output_len;uint64_t frames=p->frames;
  rc=tick_once(p,now);if(!rc&&p->state==6&&p->position==position&&p->output_sent==sent&&p->output_len==len&&p->frames==frames)break;}
 return rc;
}
