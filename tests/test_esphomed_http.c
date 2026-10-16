#include "../src/adapter/esphome_playback.c"
#include <assert.h>
#include <sys/stat.h>
static struct esp_playback p;
static char bus[256];
static int check_header(const char *s){esp_playback_init(&p);p.expected=SIZE_MAX;p.received=strlen(s);memcpy(p.buffer,s,p.received);int rc=headers(&p);esp_playback_close(&p);return rc;}
static int check_chunks(const char *s){esp_playback_init(&p);p.body_len=strlen(s);memcpy(p.buffer,s,p.body_len);int rc=unchunk(&p);esp_playback_close(&p);return rc;}
static void put32(unsigned char *b,uint32_t n){for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(n>>(i*8));}
static void wav_header(unsigned char *b,size_t n){memset(b,0,n);memcpy(b,"RIFF",4);put32(b+4,(uint32_t)(n-8));memcpy(b+8,"WAVEfmt ",8);put32(b+16,16);b[20]=1;b[22]=1;put32(b+24,24000);put32(b+28,48000);b[32]=2;b[34]=16;memcpy(b+36,"data",4);put32(b+40,(uint32_t)(n-44));}
static void expect_prepare(const unsigned char *b,size_t n,int expected,const char *name){
 esp_playback_init(&p);memcpy(p.buffer,b,n);p.body_len=n;snprintf(p.bus,sizeof p.bus,"%s",bus);int rc=prepare(&p,0);
 if(rc!=expected)fprintf(stderr,"FAIL %s: prepare returned %d, expected %d\n",name,rc,expected);
 assert(rc==expected);if(!rc){assert(p.state==6&&p.wav&&p.pcm_offset+p.pcm_bytes<=n);}else assert(p.bus_fd==-1);
 esp_playback_close(&p);
}
static void wav_boundaries(void){
 unsigned char b[64];wav_header(b,52);expect_prepare(b,52,0,"ordinary finite WAV exact sizes");
 put32(b+4,51);expect_prepare(b,52,-1,"finite RIFF size mismatch");
 put32(b+4,UINT32_MAX);expect_prepare(b,52,0,"completed sentinel RIFF with finite data");
 put32(b+40,UINT32_MAX);expect_prepare(b,52,0,"completed RIFF/data nonseekable sentinels");
 put32(b+4,44);expect_prepare(b,52,0,"exact finite RIFF with sentinel data");
 put32(b+4,UINT32_MAX);put32(b+40,UINT32_MAX-1);expect_prepare(b,52,-1,"only exact data sentinel is supported");
 put32(b+40,10);expect_prepare(b,52,-1,"sentinel RIFF cannot hide truncated finite data");
 put32(b+40,UINT32_MAX);expect_prepare(b,51,-1,"sentinel data must end on a PCM sample boundary");
 b[22]=2;b[32]=4;expect_prepare(b,50,-1,"sentinel stereo data must end on a PCM frame boundary");b[22]=1;b[32]=2;
 b[20]=3;expect_prepare(b,52,-1,"sentinels cannot enable non-PCM WAV");b[20]=1;
 b[34]=8;expect_prepare(b,52,-1,"sentinels cannot enable unsupported sample width");b[34]=16;
 b[32]=4;expect_prepare(b,52,-1,"sentinels cannot hide bad block alignment");b[32]=2;
 put32(b+24,0);expect_prepare(b,52,-1,"sentinels cannot enable invalid rate");put32(b+24,24000);
 put32(b+16,UINT32_MAX);expect_prepare(b,52,-1,"fmt chunk sentinel is not supported");put32(b+16,16);
 expect_prepare(b,19,-1,"sentinel RIFF cannot hide a truncated chunk header");
 expect_prepare(b,30,-1,"sentinel RIFF cannot hide a truncated fmt chunk");
 expect_prepare(b,40,-1,"sentinel RIFF cannot hide a truncated data header");
 wav_header(b,54);put32(b+4,UINT32_MAX);put32(b+40,8);expect_prepare(b,54,-1,"sentinel RIFF cannot hide trailing partial chunk");
 wav_header(b,62);put32(b+4,UINT32_MAX);put32(b+40,8);memcpy(b+52,"JUNK",4);put32(b+56,UINT32_MAX);expect_prepare(b,62,-1,"metadata chunk sentinel is not supported");
 put32(b+56,1);expect_prepare(b,61,-1,"odd metadata chunk requires its padding byte");expect_prepare(b,62,0,"finite odd metadata with padding remains valid");
 /* Keep the oversized payload sample-aligned so only the hard cap rejects it. */
 static unsigned char large[ESP_PLAY_BODY_MAX+2];wav_header(large,sizeof large);put32(large+4,UINT32_MAX);put32(large+40,UINT32_MAX);
 expect_prepare(large,ESP_PLAY_BODY_MAX,0,"sentinel body at hard cap");
 expect_prepare(large,sizeof large,-1,"sentinel body over hard cap");
 puts("WAV exact/sentinel sizes, malformed chunks, PCM alignment and hard cap: OK");
}
static int start_fetch(void){int f[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,f)==0);esp_playback_init(&p);p.fd=f[0];p.state=5;p.expected=SIZE_MAX;p.deadline=15000;snprintf(p.bus,sizeof p.bus,"%s",bus);return f[1];}
static void send_bytes(int fd,const void *b,size_t n){assert(send(fd,b,n,MSG_NOSIGNAL)==(ssize_t)n);}
static void receive_bytes(size_t total){for(unsigned i=0;p.received<total&&i<1024;i++){assert(esp_playback_tick(&p,0)==0);assert(p.state==5);}assert(p.received==total);assert(p.bus_fd==-1&&!p.frames);}
static void end_fetch(int peer,int expected){assert(shutdown(peer,SHUT_WR)==0);int rc=esp_playback_tick(&p,0);if(rc!=expected)fprintf(stderr,"FAIL completed HTTP body: tick returned %d, expected %d\n",rc,expected);assert(rc==expected);if(!rc)assert(p.state==6&&p.wav);else assert(p.state==0&&p.bus_fd==-1&&!p.frames);close(peer);}
static void http_boundaries(const unsigned char *b,size_t n){
 const char *h="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";char size[32];int z=snprintf(size,sizeof size,"%zx\r\n",n);assert(z>0&&(size_t)z<sizeof size);
 int peer=start_fetch();send_bytes(peer,h,strlen(h));send_bytes(peer,size,(size_t)z);send_bytes(peer,b,n);send_bytes(peer,"\r\n",2);receive_bytes(strlen(h)+(size_t)z+n+2);
 send_bytes(peer,"0\r\n\r\n",5);receive_bytes(strlen(h)+(size_t)z+n+7);end_fetch(peer,0);assert(p.body_len==n&&!memcmp(p.buffer+p.body_start,b,n));esp_playback_close(&p);
 peer=start_fetch();send_bytes(peer,h,strlen(h));send_bytes(peer,size,(size_t)z);send_bytes(peer,b,n);send_bytes(peer,"\r\n",2);receive_bytes(strlen(h)+(size_t)z+n+2);end_fetch(peer,-1);
 const char *eof="HTTP/1.1 200 OK\r\n\r\n";peer=start_fetch();send_bytes(peer,eof,strlen(eof));send_bytes(peer,b,n);receive_bytes(strlen(eof)+n);end_fetch(peer,0);esp_playback_close(&p);
 peer=start_fetch();send_bytes(peer,eof,strlen(eof));send_bytes(peer,b,n);receive_bytes(strlen(eof)+n);assert(esp_playback_tick(&p,15001)==-1);assert(p.bus_fd==-1&&!p.frames);close(peer);
 char finite[96];int hn=snprintf(finite,sizeof finite,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",n);assert(hn>0&&(size_t)hn<sizeof finite);
 peer=start_fetch();send_bytes(peer,finite,(size_t)hn);send_bytes(peer,b,n-1);receive_bytes((size_t)hn+n-1);send_bytes(peer,b+n-1,1);assert(esp_playback_tick(&p,0)==0);assert(p.state==6&&p.wav);close(peer);esp_playback_close(&p);
 peer=start_fetch();send_bytes(peer,finite,(size_t)hn);send_bytes(peer,b,n-1);receive_bytes((size_t)hn+n-1);end_fetch(peer,-1);
 peer=start_fetch();p.header_done=1;p.received=ESP_PLAY_BODY_MAX;memset(p.buffer,0,p.received);send_bytes(peer,"X",1);assert(esp_playback_tick(&p,0)==-1);assert(p.bus_fd==-1&&!p.frames);close(peer);
 puts("HTTP completion before prepare, truncated framing, hard cap and fetch deadline: OK");
}
static void ffmpeg_wav(const char *path){
 static unsigned char b[ESP_PLAY_BODY_MAX+1];FILE *f=fopen(path,"rb");assert(f);size_t n=fread(b,1,sizeof b,f);assert(!ferror(f)&&feof(f));assert(fclose(f)==0);assert(n>=44&&n<=ESP_PLAY_BODY_MAX);
 assert(!memcmp(b,"RIFF",4)&&u32(b+4)==UINT32_MAX&&!memcmp(b+8,"WAVE",4));
 expect_prepare(b,n,0,"actual ffmpeg nonseekable WAV output");
 esp_playback_init(&p);memcpy(p.buffer,b,n);p.body_len=n;snprintf(p.bus,sizeof p.bus,"%s",bus);assert(prepare(&p,0)==0);assert(p.rate==24000&&p.channels==1);assert(u32(b+p.pcm_offset-4)==UINT32_MAX);assert(p.pcm_bytes==4800);
 int output=open(bus,O_RDWR|O_TRUNC);assert(output>=0);int rc=0;for(unsigned i=0;i<512&&!rc;i++)rc=esp_playback_tick(&p,(uint64_t)i*10);assert(rc==1&&p.result==1&&p.state==0&&p.frames==4800);
 unsigned char pcm[19200];assert(lseek(output,0,SEEK_SET)==0);assert(read(output,pcm,sizeof pcm)==(ssize_t)sizeof pcm);unsigned char extra;assert(read(output,&extra,1)==0);for(size_t i=0;i<sizeof pcm;i+=2)assert((short)u16(pcm+i)==1000);assert(close(output)==0);
 http_boundaries(b,n);printf("Actual unchanged ffmpeg pipe WAV: %zu bytes, 4800 stereo output frames: OK\n",n);
}
int main(int argc,char **argv){
 const char *tmp=getenv("TMPDIR");assert(tmp&&*tmp);int n=snprintf(bus,sizeof bus,"%s/esphome-wav-bus-XXXXXX",tmp);assert(n>0&&(size_t)n<sizeof bus);int fd=mkstemp(bus);assert(fd>=0&&close(fd)==0);
 esp_playback_init(&p);memset(p.buffer,'A',8196);memcpy(p.buffer,"HTTP/1.1 200 OK\r\nX-Test: ",25);memcpy(p.buffer+8191,"\r\n\r\n",4);p.received=8195;assert(headers(&p)==-1);esp_playback_close(&p);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")==1);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n")==-1);
 assert(check_header("HTTP/1.1 200 OK\r\nContent-Length: 2097153\r\n\r\n")==-1);
 assert(check_chunks("3\r\nabc\r\n2\r\nde\r\n0\r\nX-Test: yes\r\n\r\n")==0);assert(p.body_len==5);assert(!memcmp(p.buffer,"abcde",5));
 static const char *bad[]={"", "0\r\n", "3\r\nab\r\n0\r\n\r\n", "z\r\nabc\r\n0\r\n\r\n", "FFFFFFFFFFFFFFFF\r\n", "1\r\naXX0\r\n\r\n", "0\r\n\r\nextra", "0\r\nInvalid trailer\r\n\r\n"};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++)assert(check_chunks(bad[i])==-1);
 puts("HTTP header ceiling and bounded finite chunked decoding: OK");
 /* Run the untouched external capture first so RED identifies the real bug. */
 if(argc==2)ffmpeg_wav(argv[1]);else assert(argc==1);
 wav_boundaries();unsigned char b[52];wav_header(b,sizeof b);put32(b+4,UINT32_MAX);put32(b+40,UINT32_MAX);http_boundaries(b,sizeof b);
 assert(unlink(bus)==0);return 0;
}
