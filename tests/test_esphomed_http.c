/* Streaming reply playback: HTTP framing, WAV validation, completion,
 * deadlines, and the timing that matters for latency -- audio reaches the bus
 * while the body is still arriving, and a FIFO bus is kept warm with silence.
 * Run with the path of an actual ffmpeg pipe WAV to include that capture. */
#include "../src/adapter/esphome_playback.c"
#include <assert.h>
#include <netinet/in.h>
#include <sys/stat.h>
static struct esp_playback p;
static char bus[256],fifo[300];
static uint64_t now_ms=1000;
static void put32(unsigned char *b,uint32_t n){for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(n>>(i*8));}
static void wav_header(unsigned char *b,size_t n){memset(b,0,n);memcpy(b,"RIFF",4);put32(b+4,(uint32_t)(n-8));memcpy(b+8,"WAVEfmt ",8);put32(b+16,16);b[20]=1;b[22]=1;put32(b+24,24000);put32(b+28,48000);b[32]=2;b[34]=16;memcpy(b+36,"data",4);put32(b+40,(uint32_t)(n-44));}
/* A 48 kHz mono WAV of one constant sample, with ffmpeg's nonseekable sizes when unbounded. */
static size_t tone(unsigned char *b,size_t frames,short value,int unbounded){size_t n=44+frames*2;wav_header(b,n);put32(b+24,48000);put32(b+28,96000);if(unbounded){put32(b+4,UINT32_MAX);put32(b+40,UINT32_MAX);}for(size_t i=0;i<frames;i++){b[44+2*i]=(unsigned char)value;b[45+2*i]=(unsigned char)((unsigned short)value>>8);}return n;}
static int check_header(const char *s){esp_playback_init(&p);p.expected=SIZE_MAX;p.wire_n=strlen(s);memcpy(p.wire,s,p.wire_n);int rc=headers(&p);esp_playback_close(&p);return rc;}
/* Chunked framing fed in pieces of `step` bytes. */
static int frame_pieces(const char *s,size_t step){size_t n=strlen(s);esp_playback_init(&p);p.chunked=1;p.expected=SIZE_MAX;
 for(size_t i=0;i<n;i+=step){size_t z=n-i<step?n-i:step;memcpy(p.wire+p.wire_n,s+i,z);p.wire_n+=z;if(unframe(&p)<0)return -1;}return p.chunk_stage==CHUNK_DONE?0:-1;}
/* Whole and byte-at-a-time framing must agree on verdict and payload. */
static int check_chunks(const char *s){unsigned char whole_payload[64];int whole=frame_pieces(s,strlen(s)?strlen(s):1);size_t n=p.pay_n;assert(n<=sizeof whole_payload);memcpy(whole_payload,p.payload,n);esp_playback_close(&p);
 int bytewise=frame_pieces(s,1);assert(whole==bytewise);if(!whole)assert(p.pay_n==n&&!memcmp(p.payload,whole_payload,n));return whole;}
/* The server side: one response, released to the client a prefix at a time. */
static unsigned char resp[4U*1024U*1024U];static size_t resp_n,resp_sent,resp_allow;static int peer=-1,want_shut,shut_done;
static void resp_reset(void){resp_n=resp_sent=resp_allow=0;want_shut=shut_done=0;}
static void resp_add(const void *b,size_t n){assert(n<=sizeof resp-resp_n);memcpy(resp+resp_n,b,n);resp_n+=n;}
static void resp_text(const char *s){resp_add(s,strlen(s));}
static void resp_chunked(const unsigned char *b,size_t n,size_t piece){char size[32];for(size_t i=0;i<n;i+=piece){size_t z=n-i<piece?n-i:piece;snprintf(size,sizeof size,"%zx\r\n",z);resp_text(size);resp_add(b+i,z);resp_text("\r\n");}}
static void push(void){while(resp_sent<resp_allow){ssize_t w=send(peer,resp+resp_sent,resp_allow-resp_sent,MSG_NOSIGNAL|MSG_DONTWAIT);if(w<=0){assert(errno==EAGAIN||errno==EWOULDBLOCK);return;}resp_sent+=(size_t)w;}
 if(want_shut&&!shut_done&&resp_sent==resp_n){assert(shutdown(peer,SHUT_WR)==0);shut_done=1;}}
static void allow(size_t n){resp_allow=n<resp_n?n:resp_n;}
static void allow_all(int shut){resp_allow=resp_n;want_shut=shut;}
/* A slot already connected and past its request, as esp_playback_start leaves it. */
static void fetch(const char *path){int f[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,f)==0);esp_playback_init(&p);p.fd=f[0];p.state=5;p.expected=SIZE_MAX;p.deadline=now_ms+TOTAL_MS;p.idle_deadline=now_ms+IDLE_MS;p.buffering=1;snprintf(p.bus,sizeof p.bus,"%s",path);peer=f[1];}
static int reader=-1;static unsigned char heard[1U<<23];static size_t heard_n;
static void listen_bus(void){if(reader<0)return;for(;;){ssize_t r=read(reader,heard+heard_n,sizeof heard-heard_n);if(r<=0)break;heard_n+=(size_t)r;}}
static int tick(void){push();int rc=esp_playback_tick(&p,now_ms);listen_bus();now_ms+=10;return rc;}
static int run(unsigned limit){int rc=0;for(unsigned i=0;i<limit&&!rc;i++)rc=tick();return rc;}
static void end_fetch(void){if(peer>=0)close(peer);peer=-1;esp_playback_close(&p);}
static size_t bus_size(void){struct stat st;assert(stat(bus,&st)==0);return (size_t)st.st_size;}
static void bus_reset(void){int fd=open(bus,O_WRONLY|O_TRUNC);assert(fd>=0&&close(fd)==0);}
static void expect_stream(const unsigned char *b,size_t n,int expected,const char *name){
 bus_reset();resp_reset();resp_text("HTTP/1.1 200 OK\r\n\r\n");resp_add(b,n);allow_all(1);fetch(bus);int rc=run(4000);
 if(rc!=(expected?-1:1))fprintf(stderr,"FAIL %s: tick returned %d, expected %d\n",name,rc,expected?-1:1);assert(rc==(expected?-1:1));if(!expected)assert(p.audio_frames>0&&bus_size()==p.audio_frames*4);end_fetch();
}
static void wav_boundaries(void){
 unsigned char b[64];wav_header(b,52);expect_stream(b,52,0,"ordinary finite WAV exact sizes");
 put32(b+4,51);expect_stream(b,52,-1,"finite RIFF size mismatch");
 put32(b+4,UINT32_MAX);expect_stream(b,52,0,"completed sentinel RIFF with finite data");
 put32(b+40,UINT32_MAX);expect_stream(b,52,0,"completed RIFF/data nonseekable sentinels");
 put32(b+4,44);expect_stream(b,52,0,"exact finite RIFF with sentinel data");
 put32(b+4,UINT32_MAX);put32(b+40,UINT32_MAX-1);expect_stream(b,52,-1,"only exact data sentinel is supported");
 put32(b+40,10);expect_stream(b,52,-1,"sentinel RIFF cannot hide truncated finite data");
 put32(b+40,UINT32_MAX);expect_stream(b,51,-1,"sentinel data must end on a PCM sample boundary");
 b[22]=2;b[32]=4;expect_stream(b,50,-1,"sentinel stereo data must end on a PCM frame boundary");b[22]=1;b[32]=2;
 b[20]=3;expect_stream(b,52,-1,"sentinels cannot enable non-PCM WAV");b[20]=1;
 b[34]=8;expect_stream(b,52,-1,"sentinels cannot enable unsupported sample width");b[34]=16;
 b[32]=4;expect_stream(b,52,-1,"sentinels cannot hide bad block alignment");b[32]=2;
 put32(b+24,0);expect_stream(b,52,-1,"sentinels cannot enable invalid rate");put32(b+24,24000);
 put32(b+16,UINT32_MAX);expect_stream(b,52,-1,"fmt chunk sentinel is not supported");put32(b+16,16);
 expect_stream(b,19,-1,"sentinel RIFF cannot hide a truncated chunk header");
 expect_stream(b,30,-1,"sentinel RIFF cannot hide a truncated fmt chunk");
 expect_stream(b,40,-1,"sentinel RIFF cannot hide a truncated data header");
 wav_header(b,54);put32(b+4,UINT32_MAX);put32(b+40,8);expect_stream(b,54,-1,"sentinel RIFF cannot hide trailing partial chunk");
 wav_header(b,62);put32(b+4,UINT32_MAX);put32(b+40,8);memcpy(b+52,"JUNK",4);put32(b+56,UINT32_MAX);expect_stream(b,62,-1,"metadata chunk sentinel is not supported");
 put32(b+56,1);expect_stream(b,61,-1,"odd metadata chunk requires its padding byte");expect_stream(b,62,0,"finite odd metadata with padding remains valid");
 wav_header(b,52);memcpy(b+12,"data",4);memcpy(b+36,"fmt ",4);expect_stream(b,52,-1,"data before fmt is rejected");
 puts("WAV exact/sentinel sizes, malformed chunks and PCM alignment while streaming: OK");
}
static void http_boundaries(const unsigned char *b,size_t n){
 const char *h="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",*eof="HTTP/1.1 200 OK\r\n\r\n";char finite[96];int hn=snprintf(finite,sizeof finite,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",n);assert(hn>0&&(size_t)hn<sizeof finite);
 bus_reset();resp_reset();resp_text(h);resp_chunked(b,n,n);resp_text("0\r\n\r\n");allow_all(1);fetch(bus);assert(run(4000)==1);end_fetch();
 resp_reset();resp_text(h);resp_chunked(b,n,n);allow_all(1);fetch(bus);assert(run(4000)==-1);end_fetch();
 resp_reset();resp_text(eof);resp_add(b,n);allow_all(1);fetch(bus);assert(run(4000)==1);end_fetch();
 /* Without its end the slot fails once the network has been idle too long. */
 resp_reset();resp_text(eof);resp_add(b,n);allow_all(0);fetch(bus);uint64_t idle=now_ms;assert(run(4000)==-1);assert(now_ms-idle>IDLE_MS);end_fetch();
 /* A finite body completes at its last byte, without waiting for EOF. */
 bus_reset();resp_reset();resp_text(finite);resp_add(b,n);allow_all(0);fetch(bus);assert(run(4000)==1);end_fetch();
 resp_reset();resp_text(finite);resp_add(b,n-1);allow_all(1);fetch(bus);assert(run(4000)==-1);end_fetch();
 resp_reset();resp_text(finite);resp_add(b,n);resp_text("X");allow_all(1);fetch(bus);assert(run(4000)==-1);end_fetch();
 /* The body ceiling still applies to streamed bytes. */
 resp_reset();resp_text(eof);resp_add(b,n);allow_all(0);fetch(bus);p.body_total=ESP_PLAY_BODY_MAX-1;assert(run(50)==-1);end_fetch();
 puts("HTTP completion, truncated framing, hard cap and idle deadline: OK");
}
/* The first audio reaches the bus while the body is still arriving. */
static void streams_before_eof(void){static unsigned char b[44+48000*4];size_t n=tone(b,48000*2,1200,1);
 bus_reset();resp_reset();resp_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");resp_chunked(b,n,4096);size_t first=resp_n/8;resp_text("0\r\n\r\n");
 fetch(bus);allow(first);for(unsigned i=0;i<60;i++)assert(tick()==0);
 size_t early=bus_size();assert(early>0&&!p.net_done&&resp_sent<resp_n);
 unsigned char head[4];int fd=open(bus,O_RDONLY);assert(fd>=0&&read(fd,head,4)==4&&close(fd)==0);assert(head[0]==(unsigned char)1200&&head[2]==(unsigned char)1200);
 allow_all(1);assert(run(6000)==1);assert(p.input_frames==48000*2&&p.audio_frames==48000*2&&bus_size()==p.audio_frames*4);end_fetch();
 printf("Streaming: %zu bytes on the bus with 1/8 of the body received: OK\n",early);
}
/* Longer than the old 2 MiB cache and slower than the old 15 s fetch deadline. */
static void long_and_slow(void){static unsigned char b[44+48000*2*36];size_t n=tone(b,48000*36,700,1);assert(n>2U*1024U*1024U);
 bus_reset();resp_reset();resp_text("HTTP/1.1 200 OK\r\n\r\n");resp_add(b,n);allow_all(1);fetch(bus);assert(run(6000)==1);assert(p.input_frames==48000*36&&bus_size()==p.audio_frames*4);end_fetch();
 /* Eight seconds of audio released a ninth at a time, 3 s apart: 24 s in total. */
 bus_reset();resp_reset();resp_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");resp_chunked(b,44+48000*2*8,48000);resp_text("0\r\n\r\n");fetch(bus);uint64_t start=now_ms;
 int rc=0;for(unsigned i=0;i<4000&&!rc;i++){allow(resp_n*((now_ms-start)/3000+1)/9);if((now_ms-start)/3000>=8)allow_all(1);rc=tick();}
 assert(rc==1&&now_ms-start>15000&&p.input_frames==48000*8);end_fetch();
 puts("A 36 s reply over 2 MiB, and one delivered over 24 s: OK");
}
static void bus_fifo(void){snprintf(fifo,sizeof fifo,"%s.fifo",bus);unlink(fifo);assert(mkfifo(fifo,0600)==0);reader=open(fifo,O_RDONLY|O_NONBLOCK);assert(reader>=0);heard_n=0;}
static void bus_fifo_close(void){close(reader);reader=-1;assert(unlink(fifo)==0);}
static size_t first_sound(void){for(size_t i=0;i+1<heard_n;i+=2)if(heard[i]||heard[i+1])return i;return heard_n;}
static size_t after_sound(void){size_t i=heard_n;while(i>=2&&!heard[i-1]&&!heard[i-2])i-=2;return heard_n-i;}
static size_t sound_bytes(void){size_t n=0;for(size_t i=0;i+3<heard_n;i+=4)if(memcmp(heard+i,"\0\0\0\0",4))n+=4;return n;}
/* Real connect through esp_playback_start: a FIFO bus warms while waiting, the
 * queued silence stays bounded when the engine stops reading, and a stall in
 * the reply is filled with silence instead of letting the engine power down. */
static void warm_fifo(void){static unsigned char b[44+48000*2*2];size_t n=tone(b,48000*2,1200,1);struct sockaddr_in a;socklen_t z=sizeof a;int server=socket(AF_INET,SOCK_STREAM,0);
 memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);assert(server>=0&&bind(server,(struct sockaddr*)&a,sizeof a)==0&&listen(server,1)==0&&getsockname(server,(struct sockaddr*)&a,&z)==0);
 char url[64];snprintf(url,sizeof url,"http://127.0.0.1:%u/tts.wav",ntohs(a.sin_port));resp_reset();bus_fifo();esp_playback_init(&p);assert(esp_playback_start(&p,url,fifo,NULL,now_ms)==0);assert(p.fifo);
 for(unsigned i=0;i<100;i++){assert(tick()==0);int queued=0;assert(!ioctl(reader,FIONREAD,&queued)&&queued==0);}
 assert(heard_n>=48000*4*900/1000&&heard_n<=48000*4*1100/1000&&first_sound()==heard_n);
 int stalled=reader;reader=-1;for(unsigned i=0;i<50;i++)assert(tick()==0);int queued=0;assert(!ioctl(stalled,FIONREAD,&queued)&&queued>0&&queued<=SILENCE_QUEUE+(int)SILENCE_FRAMES*4);reader=stalled;listen_bus();
 size_t warmed=heard_n;peer=accept(server,NULL,NULL);assert(peer>=0);close(server);char request[512];ssize_t r=recv(peer,request,sizeof request-1,0);assert(r>0);request[r]=0;assert(!strncmp(request,"GET /tts.wav HTTP/1.1\r\n",23));
 resp_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");resp_chunked(b,n,9600);size_t half=resp_n/2;resp_text("0\r\n\r\n");allow(half);
 for(unsigned i=0;i<150;i++)assert(tick()==0);
 /* About 1 s of audio, then silence for the rest of the 1.5 s. */
 size_t sound=sound_bytes();assert(first_sound()>=warmed&&sound>=48000*4*900/1000&&sound<=48000*4*1100/1000);assert(p.underruns==1&&after_sound()>=48000*4*300/1000);
 allow_all(1);assert(run(1000)==1);assert(p.input_frames==48000*2&&sound_bytes()==p.audio_frames*4&&heard_n%4==0);end_fetch();bus_fifo_close();
 puts("FIFO bus warmed while connecting, bounded when not drained, fed through a stall: OK");
}
static void ffmpeg_wav(const char *path){
 static unsigned char b[1U<<20];FILE *f=fopen(path,"rb");assert(f);size_t n=fread(b,1,sizeof b,f);assert(!ferror(f)&&feof(f));assert(fclose(f)==0);assert(n>=44);
 assert(!memcmp(b,"RIFF",4)&&u32(b+4)==UINT32_MAX&&!memcmp(b+8,"WAVE",4));
 bus_reset();resp_reset();resp_text("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");resp_chunked(b,n,1000);resp_text("0\r\n\r\n");allow_all(1);fetch(bus);
 int rc=run(4000);assert(rc==1&&p.result==1&&p.state==0&&p.frames==4800&&p.rate==24000&&p.channels==1&&p.input_frames==2400);end_fetch();
 int output=open(bus,O_RDONLY);assert(output>=0);unsigned char pcm[19200];assert(read(output,pcm,sizeof pcm)==(ssize_t)sizeof pcm);unsigned char extra;assert(read(output,&extra,1)==0);for(size_t i=0;i<sizeof pcm;i+=2)assert((short)u16(pcm+i)==1000);assert(close(output)==0);
 http_boundaries(b,n);printf("Actual unchanged ffmpeg pipe WAV: %zu bytes, 4800 stereo output frames: OK\n",n);
}
int main(int argc,char **argv){
 const char *tmp=getenv("TMPDIR");assert(tmp&&*tmp);int n=snprintf(bus,sizeof bus,"%s/esphome-wav-bus-XXXXXX",tmp);assert(n>0&&(size_t)n<sizeof bus);int fd=mkstemp(bus);assert(fd>=0&&close(fd)==0);
 esp_playback_init(&p);memset(p.wire,'A',8196);memcpy(p.wire,"HTTP/1.1 200 OK\r\nX-Test: ",25);memcpy(p.wire+8191,"\r\n\r\n",4);p.wire_n=8195;assert(headers(&p)==-1);esp_playback_close(&p);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")==1);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nContent-Length: 33554433\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 404 Not Found\r\n\r\n")==-1);
 assert(check_chunks("3\r\nabc\r\n2\r\nde\r\n0\r\nX-Test: yes\r\n\r\n")==0);assert(p.pay_n==5);assert(!memcmp(p.payload,"abcde",5));esp_playback_close(&p);
 static const char *bad[]={"", "0\r\n", "3\r\nab\r\n0\r\n\r\n", "z\r\nabc\r\n0\r\n\r\n", "FFFFFFFFFFFFFFFF\r\n", "1\r\naXX0\r\n\r\n", "0\r\n\r\nextra", "0\r\nInvalid trailer\r\n\r\n", "3;ext=1\r\nabc\r\n0\r\n\r\n", "\r\n"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(check_chunks(bad[i])==-1);
 puts("HTTP header ceiling and bounded chunked decoding, whole and byte by byte: OK");
 /* Run the untouched external capture first so RED identifies the real bug. */
 if(argc==2)ffmpeg_wav(argv[1]);else assert(argc==1);
 wav_boundaries();unsigned char b[52];wav_header(b,sizeof b);put32(b+4,UINT32_MAX);put32(b+40,UINT32_MAX);http_boundaries(b,sizeof b);
 streams_before_eof();long_and_slow();warm_fifo();
 assert(unlink(bus)==0);return 0;
}
