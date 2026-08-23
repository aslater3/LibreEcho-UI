#include "http_server.h"
#include "tls.h"
#include "adapter/adapter.h"
#include "adapter/voice_stream.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
extern int setgroups(size_t,const gid_t*);
#else
extern int setgroups(int,const gid_t*);
#endif
#define LE_MAX_CLIENTS 16
#define LE_REQ_MAX 24576
#define LE_HEADER_MAX 8192
#define LE_BODY_MAX 16384
/* The ceiling lives in api.h so the size the API advertises and the size
   enforced here cannot drift apart. */
#define LE_UPDATE_PATH "/data/libreecho/update/incoming/manual.tar"
#define LE_UPDATE_TMP "/data/libreecho/update/incoming/manual.tar.tmp"
#define LE_UPDATE_LOCK "/data/libreecho/update/incoming/upload.lock"
#define LE_MAX_ASSISTANT_WORKERS 4
struct client{int fd;
size_t used;
char buf[LE_REQ_MAX+1];
};
static volatile sig_atomic_t assistant_workers;
static volatile sig_atomic_t assistant_pids[LE_MAX_ASSISTANT_WORKERS];
static void reap_assistant_workers(int signo){int i;int status;(void)signo;for(i=0;i<LE_MAX_ASSISTANT_WORKERS;i++)if(assistant_pids[i]>0&&waitpid((pid_t)assistant_pids[i],&status,WNOHANG)>0){assistant_pids[i]=0;if(assistant_workers>0)assistant_workers--;}}

static int close_on_exec(int fd)
{
int flags=fcntl(fd,F_GETFD);
return flags<0||fcntl(fd,F_SETFD,flags|FD_CLOEXEC)<0?-1:0;
}

static const char*mime(const char*p){const char*e=strrchr(p,'.');
if(!e)return"application/octet-stream";
if(!strcmp(e,".html"))return"text/html; charset=utf-8";
if(!strcmp(e,".css"))return"text/css; charset=utf-8";
if(!strcmp(e,".js"))return"application/javascript; charset=utf-8";
if(!strcmp(e,".json"))return"application/json; charset=utf-8";
if(!strcmp(e,".webmanifest"))return"application/manifest+json; charset=utf-8";
if(!strcmp(e,".svg"))return"image/svg+xml";
if(!strcmp(e,".png"))return"image/png";
return"application/octet-stream";
}
static void send_all(int fd,const void*b,size_t n){const char*p=b;
while(n){ssize_t w=send(fd,p,n,0);
if(w<=0)return;
p+=w;
n-=(size_t)w;
}}
static void response(int fd,int code,const char*type,const void*body,size_t n){char h[1024];
const char*reason=code==200?"OK":code==400?"Bad Request":code==403?"Forbidden":code==404?"Not Found":code==405?"Method Not Allowed":code==409?"Conflict":code==413?"Payload Too Large":code==429?"Too Many Requests":code==501?"Not Implemented":code==503?"Service Unavailable":"Error";
int z=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; script-src 'self'\r\n\r\n",code,reason,type,(unsigned long)n);
send_all(fd,h,(size_t)z);
if(n)send_all(fd,body,n);
}
static int stream_send_all(int fd,const void*body,size_t n){const char*p=body;while(n){ssize_t w=send(fd,p,n,0);if(w<=0)return-1;p+=w;n-=w;}return 0;}
static int stream_shared_audio(int fd){struct sockaddr_un address;int wake=-1;char request[128],reply[1024],header[512];size_t used=0;ssize_t n;int frame_result;struct le_voice_stream_frame frame;wake=socket(AF_UNIX,SOCK_STREAM,0);if(wake<0)return-2;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strncpy(address.sun_path,LE_ADAPTER_WAKEWORD_SOCK,sizeof(address.sun_path)-1);if(connect(wake,(struct sockaddr*)&address,sizeof(address))<0)goto unavailable;n=snprintf(request,sizeof(request),"{\"v\":1,\"id\":1,\"cmd\":\"stream_audio\",\"args\":{}}\n");if(n<0||stream_send_all(wake,request,(size_t)n)<0)goto unavailable;while(used+1<sizeof(reply)){n=read(wake,reply+used,1);if(n<=0)goto unavailable;if(reply[used++]=='\n')break;}reply[used]='\0';if(!strstr(reply,"\"ok\":true"))goto unavailable;signal(SIGPIPE,SIG_IGN);n=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nCache-Control: no-store\r\nX-LibreEcho-Audio: pcm_s16_le;rate=16000;channels=1;selected-channel=shared-wake;calibration=applied\r\nX-Content-Type-Options: nosniff\r\n\r\n");if(n<0||stream_send_all(fd,header,(size_t)n)<0){close(wake);return-1;}while((frame_result=le_voice_stream_read_frame(wake,&frame))>0){size_t bytes=(size_t)frame.sample_count*sizeof(frame.samples[0]);int z=snprintf(header,sizeof(header),"%zx\r\n",bytes);if(z<0||stream_send_all(fd,header,(size_t)z)<0||stream_send_all(fd,frame.samples,bytes)<0||stream_send_all(fd,"\r\n",2)<0){close(wake);return-1;}}if(frame_result==0)(void)stream_send_all(fd,"0\r\n\r\n",5);close(wake);return frame_result<0?-1:0;unavailable:close(wake);return-2;}
static int stream_microphone(int fd,int selected_channel){struct sockaddr_un address;int microphone=-1;char request[160],reply[LE_ADAPTER_MSG_MAX],buffer[8192],header[512];size_t used=0;ssize_t n;int shared_result=stream_shared_audio(fd);if(shared_result!=-2){close(fd);return 0;}microphone=socket(AF_UNIX,SOCK_STREAM,0);if(microphone<0)goto unavailable;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strncpy(address.sun_path,LE_ADAPTER_MIC_SOCK,sizeof(address.sun_path)-1);if(connect(microphone,(struct sockaddr*)&address,sizeof(address))<0)goto unavailable;n=snprintf(request,sizeof(request),"{\"v\":1,\"id\":1,\"cmd\":\"stream_raw\",\"args\":{\"channel\":%d}}\n",selected_channel);if(n<0||stream_send_all(microphone,request,(size_t)n)<0)goto unavailable;while(used+1<sizeof(reply)){n=read(microphone,reply+used,1);if(n<=0)goto unavailable;if(reply[used++]=='\n')break;}reply[used]='\0';if(!strstr(reply,"\"ok\":true"))goto unavailable;signal(SIGPIPE,SIG_IGN);n=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nCache-Control: no-store\r\nX-LibreEcho-Audio: pcm_s24_3le;rate=16000;channels=9;container-bits=24;valid-bits=16;selected-channel=%d;calibration=none\r\nX-Content-Type-Options: nosniff\r\n\r\n",selected_channel);if(n<0||stream_send_all(fd,header,(size_t)n)<0)goto stop;while((n=read(microphone,buffer,sizeof(buffer)))>0){int z=snprintf(header,sizeof(header),"%zx\r\n",(size_t)n);if(z<0||stream_send_all(fd,header,(size_t)z)<0||stream_send_all(fd,buffer,(size_t)n)<0||stream_send_all(fd,"\r\n",2)<0)goto stop;}if(n==0)(void)stream_send_all(fd,"0\r\n\r\n",5);goto stop;unavailable:{const char*message="{\"ok\":false,\"data\":null,\"error\":{\"code\":\"microphone_unavailable\",\"message\":\"Microphone service could not start the stream\"}}";response(fd,503,"application/json",message,strlen(message));}stop:if(microphone>=0)close(microphone);close(fd);return 0;}
static int start_pcm_stream(int fd,int selected_channel){pid_t pid=fork();if(pid<0)return-1;if(pid==0){(void)stream_microphone(fd,selected_channel);_exit(0);}close(fd);return 0;}
static int write_all_file(int fd,const void*body,size_t n){const char*p=body;while(n){ssize_t w=write(fd,p,n);if(w<=0)return-1;p+=w;n-=(size_t)w;}return 0;}
static void update_error(int fd,int status,const char*code,const char*message){char body[512];int n=snprintf(body,sizeof(body),"{\"ok\":false,\"data\":null,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",code,message);response(fd,status,"application/json",body,(size_t)n);}
/* libreecho-update reports why it refused a package by printing ERROR:<token>
   on stderr and exiting non-zero.  That token was previously discarded, so
   every distinct cause -- a channel mismatch, a bad signature, an unmountable
   userdata -- surfaced as one opaque message and could only be recovered with
   physical access to the device.  Capture it and hand it back to the caller. */
static void update_error_reason(int fd,int status,const char*code,const char*message,const char*reason){char body[640];int n;if(!reason||!*reason){update_error(fd,status,code,message);return;}n=snprintf(body,sizeof(body),"{\"ok\":false,\"data\":null,\"error\":{\"code\":\"%s\",\"reason\":\"%s\",\"message\":\"%s\"}}",code,reason,message);response(fd,status,"application/json",body,(size_t)n);}
/* Read the last ERROR: token from the complete bounded stderr capture. */
static void update_failure_reason(int f,char*out,size_t cap){char buf[4096],token[64],window[6]={0};ssize_t n;size_t token_len=0,window_len=0,i;int collecting=0;if(cap)out[0]='\0';if(lseek(f,0,SEEK_SET)<0)return;for(;;){n=read(f,buf,sizeof(buf));if(n<=0)break;for(i=0;i<(size_t)n;i++){char c=buf[i];if(collecting){if((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'){if(token_len+1<sizeof(token))token[token_len++]=c;continue;}token[token_len]='\0';if(cap){size_t copy=token_len<cap-1?token_len:cap-1;memcpy(out,token,copy);out[copy]='\0';}collecting=0;token_len=0;}if(window_len<sizeof(window)){window[window_len++]=c;}else{memmove(window,window+1,sizeof(window)-1);window[sizeof(window)-1]=c;}if(window_len==sizeof(window)&&!memcmp(window,"ERROR:",sizeof(window))){collecting=1;token_len=0;window_len=0;}}}if(collecting){token[token_len]='\0';if(cap){size_t copy=token_len<cap-1?token_len:cap-1;memcpy(out,token,copy);out[copy]='\0';}}}
static int stream_update_upload(int fd,const char*initial,size_t initial_len,size_t content_len,int allow_unsigned){static const char success[]="{\"ok\":true,\"data\":{\"installed\":true,\"state\":\"reboot-pending\"},\"error\":null}";char buffer[16384];size_t received=initial_len;int out=-1,lock=-1,errlog=-1,status,errpipe[2]={-1,-1};size_t captured=0;ssize_t n;pid_t child;char stderr_tail[65536],errpath[]="/data/libreecho/update/incoming/.update-error-XXXXXX";if(mkdir("/data/libreecho",0700)&&errno!=EEXIST)goto io;if(mkdir("/data/libreecho/update",0700)&&errno!=EEXIST)goto io;if(mkdir("/data/libreecho/update/incoming",0700)&&errno!=EEXIST)goto io;lock=open(LE_UPDATE_LOCK,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(lock<0){update_error(fd,409,"update_busy","Another update upload is active");goto done;}close(lock);lock=1;out=open(LE_UPDATE_TMP,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(out<0)goto io;if(initial_len&&write_all_file(out,initial,initial_len))goto io;while(received<content_len){size_t want=content_len-received;if(want>sizeof(buffer))want=sizeof(buffer);n=recv(fd,buffer,want,0);if(n<=0)goto io;if(write_all_file(out,buffer,(size_t)n))goto io;received+=(size_t)n;}if(fsync(out)||close(out)){out=-1;goto io;}out=-1;if(rename(LE_UPDATE_TMP,LE_UPDATE_PATH))goto io;errlog=mkstemp(errpath);if(errlog<0)goto io;unlink(errpath);if(pipe(errpipe)<0)goto io;child=fork();if(child<0)goto io;if(child==0){close(errpipe[0]);if(dup2(errpipe[1],STDERR_FILENO)<0)_exit(127);close(errpipe[1]);if(allow_unsigned)execl("/usr/local/sbin/libreecho-update","libreecho-update","install","--allow-unsigned",LE_UPDATE_PATH,(char*)0);else execl("/usr/local/sbin/libreecho-update","libreecho-update","install",LE_UPDATE_PATH,(char*)0);_exit(127);}close(errpipe[1]);errpipe[1]=-1;fcntl(errpipe[0],F_SETFL,O_NONBLOCK);{int child_reaped=0,pipe_open=1;char errbuf[4096];while(pipe_open||!child_reaped){struct pollfd pfd={errpipe[0],POLLIN|POLLHUP,0};pid_t waited;if(pipe_open){int poll_result=poll(&pfd,1,100);if(poll_result>0&&(pfd.revents&(POLLIN|POLLHUP))){n=read(errpipe[0],errbuf,sizeof(errbuf));if(n>0){size_t count=(size_t)n;if(captured+count<=sizeof(stderr_tail)){memcpy(stderr_tail+captured,errbuf,count);captured+=count;}else{size_t drop=captured+count-sizeof(stderr_tail);if(drop>=captured){size_t keep=sizeof(stderr_tail)<count?sizeof(stderr_tail):count;memcpy(stderr_tail,errbuf+count-keep,keep);captured=keep;}else{memmove(stderr_tail,stderr_tail+drop,captured-drop);memcpy(stderr_tail+captured-drop,errbuf,count);captured=sizeof(stderr_tail);}}}else if(n==0){close(errpipe[0]);errpipe[0]=-1;pipe_open=0;}}}waited=waitpid(child,&status,WNOHANG);if(waited==child)child_reaped=1;else if(waited<0){if(errno==EINTR)continue;child_reaped=1;status=0;}}}if(errpipe[0]>=0)close(errpipe[0]);if(ftruncate(errlog,0)<0||lseek(errlog,0,SEEK_SET)<0||write_all_file(errlog,stderr_tail,captured)<0){close(errlog);errlog=-1;}if(!WIFEXITED(status)||WEXITSTATUS(status)!=0){char reason[64];update_failure_reason(errlog,reason,sizeof(reason));update_error_reason(fd,400,"update_rejected",!strcmp(reason,"manifest_update_channel_mismatch")?"The update targets a different release channel than this device":"The signed update failed verification or installation",reason);goto done;}response(fd,200,"application/json",success,sizeof(success)-1);goto done;io:if(out>=0)close(out);update_error(fd,503,"io_error","The update upload could not be stored");done:if(errlog>=0)close(errlog);unlink(LE_UPDATE_TMP);if(lock==1)unlink(LE_UPDATE_LOCK);close(fd);return 0;}
static int start_update_upload(int fd,const char*initial,size_t initial_len,size_t content_len,int allow_unsigned){pid_t pid=fork();if(pid<0)return-1;if(pid==0){(void)stream_update_upload(fd,initial,initial_len,content_len,allow_unsigned);_exit(0);}close(fd);return 0;}
static const char *update_fetch_path(void){const char *p=getenv("LIBREECHO_UPDATE_FETCH");return p&&*p?p:"/usr/local/sbin/libreecho-update-fetch";}
static int run_update_fetch(int fd,const char*action){char success[160];int status,n,channel_action=!strncmp(action,"set-channel-",12);pid_t child=fork();if(child<0){update_error(fd,503,"io_error","The update command could not start");close(fd);return 0;}if(child==0){const char *helper=update_fetch_path();execl(helper,helper,action,(char*)0);_exit(127);}if(waitpid(child,&status,0)<0||!WIFEXITED(status)||WEXITSTATUS(status)!=0){if(channel_action)update_error(fd,503,"io_error","The update channel could not be saved");else update_error(fd,!strcmp(action,"check")?503:400,!strcmp(action,"check")?"update_check_failed":"update_rejected",!strcmp(action,"check")?"The GitHub release check failed":"The signed update failed verification or installation");close(fd);return 0;}if(channel_action){n=snprintf(success,sizeof(success),"{\"ok\":true,\"data\":{\"channel\":\"%s\"},\"error\":null}",action+12);}else{n=snprintf(success,sizeof(success),"{\"ok\":true,\"data\":{\"checked\":%s,\"installed\":%s,\"state\":\"%s\"},\"error\":null}",!strcmp(action,"check")?"true":"false",!strcmp(action,"install")?"true":"false",!strcmp(action,"check")?"checked":"reboot-pending");}response(fd,200,"application/json",success,(size_t)n);close(fd);return 0;}
static int start_update_fetch(int fd,const char*action){pid_t pid=fork();if(pid<0)return-1;if(pid==0){(void)run_update_fetch(fd,action);_exit(0);}close(fd);return 0;}
static int start_api_worker(int fd,struct api_context*api,const struct api_request*q){sigset_t blocked,previous;pid_t pid;int i,slot=-1;struct api_response r;sigemptyset(&blocked);sigaddset(&blocked,SIGCHLD);if(sigprocmask(SIG_BLOCK,&blocked,&previous)<0)return-1;for(i=0;i<LE_MAX_ASSISTANT_WORKERS;i++)if(assistant_pids[i]<=0){slot=i;break;}if(slot<0){sigprocmask(SIG_SETMASK,&previous,NULL);return-1;}pid=fork();if(pid<0){sigprocmask(SIG_SETMASK,&previous,NULL);return-1;}if(pid==0){sigprocmask(SIG_SETMASK,&previous,NULL);memset(&r,0,sizeof(r));api_handle(api,q,&r);response(fd,r.status,r.type,r.body,r.length);close(fd);_exit(0);}assistant_pids[slot]=pid;assistant_workers++;sigprocmask(SIG_SETMASK,&previous,NULL);close(fd);return 0;}
static char*header(char*s,const char*name){size_t n=strlen(name);
char*p=strstr(s,"\r\n");
while(p&&p[2]&&p[2]!='\r'){p+=2;
if(!strncasecmp(p,name,n)&&p[n]==':'){p+=n+1;
while(*p==' '||*p=='\t')p++;
return p;
}p=strstr(p,"\r\n");
}return 0;
}static void copy_header(char*out,size_t z,char*p){size_t n=0;
if(!p){out[0]=0;
return;
}while(p[n]&&p[n]!='\r'&&n+1<z)n++;
memcpy(out,p,n);
out[n]=0;
}
static int serve_file(int fd,const struct http_options*o,const char*url){char path[768],clean[256];
const char*q;
size_t url_len;
int f;
struct stat st;
char b[8192];
ssize_t n;
if(strstr(url,"..")||strchr(url,'\\'))return-1;
q=strchr(url,'?');
url_len=q?(size_t)(q-url):strlen(url);
if(url_len>=sizeof(clean))return-1;
memcpy(clean,url,url_len);
clean[url_len]=0;
if(!strcmp(clean,"/"))strcpy(clean,"/index.html");
if(snprintf(path,sizeof(path),"%s%s",o->web_root,clean)>=(int)sizeof(path))return-1;
f=open(path,O_RDONLY);
if(f<0||fstat(f,&st)||!S_ISREG(st.st_mode)){if(f>=0)close(f);
return-1;
} {char h[1024];
int z=snprintf(h,sizeof(h),"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nCache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nContent-Security-Policy: default-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; script-src 'self'\r\n\r\n",mime(path),(unsigned long)st.st_size);
send_all(fd,h,(size_t)z);
}while((n=read(f,b,sizeof(b)))>0)send_all(fd,b,(size_t)n);
close(f);
return 0;
}
static int destructive_path(const char*path){return !strcmp(path,"/api/v1/system/reboot")||!strcmp(path,"/api/v1/system/shutdown")||!strcmp(path,"/api/v1/system/factory-reset");}
static void process(struct client*c,const struct http_options*o,struct api_context*api){char*end=strstr(c->buf,"\r\n\r\n"),*body,*cl;
size_t headers,body_len=0,content_len=0;
const char *page_path;
struct api_request q;
struct api_response r;
static time_t last_destructive=0;
if(!end)return;
headers=(size_t)(end-c->buf)+4;
if(headers>LE_HEADER_MAX){response(c->fd,413,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"headers_too_large\",\"message\":\"Request headers exceed 8 KiB\"}}",122);
goto done;
}memset(&q,0,sizeof(q));
if(sscanf(c->buf,"%7s %255s",q.method,q.path)!=2){response(c->fd,400,"text/plain","Bad request",11);
goto done;
}cl=header(c->buf,"Content-Length");
if(cl)content_len=(size_t)strtoul(cl,0,10);
/* Two size gates: the fixed ceiling, and what the staging filesystem can
   actually hold. Refusing here costs the client one request; refusing after
   the stream costs it the whole upload. */
if(!strcmp(q.path,"/api/v1/system/update/upload")){size_t initial;copy_header(q.host,sizeof(q.host),header(c->buf,"Host"));copy_header(q.origin,sizeof(q.origin),header(c->buf,"Origin"));copy_header(q.authorization,sizeof(q.authorization),header(c->buf,"Authorization"));copy_header(q.csrf,sizeof(q.csrf),header(c->buf,"X-LibreEcho-CSRF"));{size_t limit=le_update_max_upload_bytes();char detail[128];if(!content_len||content_len>LE_UPDATE_MAX_BYTES){update_error(c->fd,413,"update_size","Update must be between 1 byte and 32 MiB");goto done;}if(content_len>limit){snprintf(detail,sizeof(detail),"Update is larger than the %lu bytes this device can stage",(unsigned long)limit);update_error(c->fd,413,"update_size",detail);goto done;}}if(!api_update_upload_authorize(api,&q,&r)){response(c->fd,r.status,r.type,r.body,r.length);goto done;}initial=c->used>headers?c->used-headers:0;if(initial>content_len)initial=content_len;{const char*au=header(c->buf,"X-LibreEcho-Allow-Unsigned");int allow_unsigned=au&&(*au=='1'||*au=='t'||*au=='T'||*au=='y'||*au=='Y');if(start_update_upload(c->fd,end+4,initial,content_len,allow_unsigned)<0){update_error(c->fd,503,"io_error","The update upload could not start");goto done;}}c->fd=-1;c->used=0;return;}
if(content_len>LE_BODY_MAX){response(c->fd,413,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"body_too_large\",\"message\":\"Request body exceeds 16 KiB\"}}",118);
goto done;
}if(c->used<headers+content_len)return;
body=end+4;
body_len=content_len;
q.body=body;
q.body_len=body_len;
body[body_len]=0;
copy_header(q.host,sizeof(q.host),header(c->buf,"Host"));
copy_header(q.origin,sizeof(q.origin),header(c->buf,"Origin"));
copy_header(q.authorization,sizeof(q.authorization),header(c->buf,"Authorization"));
copy_header(q.csrf,sizeof(q.csrf),header(c->buf,"X-LibreEcho-CSRF"));
copy_header(q.confirm,sizeof(q.confirm),header(c->buf,"X-LibreEcho-Confirm"));
if(!strcmp(q.path,"/api/v1/assistant/respond")&&!strcmp(q.method,"POST")){if(start_api_worker(c->fd,api,&q)<0){response(c->fd,503,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"io_error\",\"message\":\"Assistant request could not start\"}}",125);goto done;}c->fd=-1;c->used=0;return;}
if(!strcmp(q.path,"/api/v1/system/update/check")||!strcmp(q.path,"/api/v1/system/update/apply")){const char*action=!strcmp(q.path,"/api/v1/system/update/check")?"check":"install";if(!api_update_fetch_authorize(api,&q,&r)){response(c->fd,r.status,r.type,r.body,r.length);goto done;}if(start_update_fetch(c->fd,action)<0){update_error(c->fd,503,"io_error","The update command could not start");goto done;}c->fd=-1;c->used=0;return;}
if(!strcmp(q.path,"/api/v1/system/update/channel")){char channel[16],action[32];if(!api_update_channel_authorize(api,&q,&r,channel,sizeof(channel))){response(c->fd,r.status,r.type,r.body,r.length);goto done;}snprintf(action,sizeof(action),"set-channel-%s",channel);if(start_update_fetch(c->fd,action)<0){update_error(c->fd,503,"io_error","The update channel could not be changed");goto done;}c->fd=-1;c->used=0;return;}
if(!strncmp(q.path,"/api/v1/baby-monitor/stream",27)){int card,device,channels,bits,selected_channel;if(!api_baby_monitor_stream_authorize(api,&q,&r,&card,&device,&channels,&bits,&selected_channel)){response(c->fd,r.status,r.type,r.body,r.length);goto done;}if(start_pcm_stream(c->fd,selected_channel)<0){response(c->fd,503,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"io\",\"message\":\"Microphone stream could not start\"}}",118);goto done;}c->fd=-1;c->used=0;return;}if(!strncmp(q.path,"/api/",5)){time_t now=time(0);
if(destructive_path(q.path)&&last_destructive&&now-last_destructive<3){response(c->fd,429,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"rate_limited\",\"message\":\"Wait before another device action\"}}",115);
goto done;
}if(destructive_path(q.path))last_destructive=now;
api_handle(api,&q,&r);
if(r.status>=200&&r.status<300&&!strcmp(q.method,"PUT")&&(!strcmp(q.path,"/api/v1/audio")||!strcmp(q.path,"/api/v1/led")||!strcmp(q.path,"/api/v1/network")||!strcmp(q.path,"/api/v1/wake-word")||!strcmp(q.path,"/api/v1/buttons")||!strcmp(q.path,"/api/v1/privacy")||!strncmp(q.path,"/api/v1/integrations/",21))&&api_persist_configuration(api)){r.status=503;strcpy(r.type,"application/json; charset=utf-8");strcpy(r.body,"{\"ok\":false,\"data\":null,\"error\":{\"code\":\"io_error\",\"message\":\"Configuration change could not be saved\"}}");r.length=strlen(r.body);}
response(c->fd,r.status,r.type,r.body,r.length);
if(body_len)memset(body,0,body_len);
}else if(strcmp(q.method,"GET")&&strcmp(q.method,"HEAD"))response(c->fd,405,"text/plain","Method not allowed",18);
else {const char*base=strrchr(q.path,'/');base=base?base+1:q.path;page_path=q.path;if(!strcmp(q.path,"/login"))page_path="/login.html";else if(!strcmp(q.path,"/initial-setup"))page_path="/initial-setup.html";else if(!strcmp(q.path,"/")&&!api->setup_completed)page_path="/setup.html";else if(api_bootstrap_required(api)&&!strchr(base,'.'))page_path="/initial-setup.html";if(serve_file(c->fd,o,page_path)){if(strchr(base,'.')||serve_file(c->fd,o,"/index.html"))response(c->fd,404,"text/plain","Not found",9);}}

done:close(c->fd);
c->fd=-1;
c->used=0;
}
/*
 * HTTPS is served by terminating TLS in a short-lived child that relays
 * plaintext to the ordinary HTTP loop over loopback, rather than by wrapping
 * the request path in TLS directly. Two request paths (update upload and the
 * assistant worker) hand their raw fd to a forked child and keep reading it
 * there; a child cannot continue a TLS session owned by the parent, so
 * in-line termination would break exactly those two. Relaying keeps every
 * downstream byte identical and leaves plain HTTP listening, so a TLS
 * misconfiguration can never lock the device's own UI out.
 */
static void tls_relay(int cfd,const struct http_options*o){
struct le_tls*tls;int up=-1;struct sockaddr_in a;struct pollfd p[2];char buf[4096];long n;
tls=le_tls_server_open(cfd,o->tls_cert,o->tls_key);
if(!tls)return;
up=socket(AF_INET,SOCK_STREAM,0);
if(up<0){le_tls_close(tls);return;}
memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_port=htons((uint16_t)o->port);
/* inet_pton, not INADDR_LOOPBACK: the latter is not POSIX and is hidden by
   _POSIX_C_SOURCE on some platforms, so it built on Linux and failed the
   native build the e2e harness uses. */
if(inet_pton(AF_INET,"127.0.0.1",&a.sin_addr)!=1){close(up);le_tls_close(tls);return;}
if(connect(up,(struct sockaddr*)&a,sizeof(a))){close(up);le_tls_close(tls);return;}
/*
 * Pump both directions until either end finishes. Two things this loop has to
 * get right, both learned the hard way:
 *
 *  - Upstream is checked on every pass, even when TLS still holds buffered
 *    plaintext. Draining the whole request first looks harmless but means a
 *    server that answers early (a 401/403, or a 413 on an oversized body) has
 *    its reply sitting unread while we keep pushing at a socket it already
 *    closed. The write then fails and the reply is lost.
 *  - A failed upstream write is not the end of the exchange. The response may
 *    already be in our receive buffer, so drain and forward it before closing,
 *    or the client sees a bare TLS shutdown and no status at all.
 */
int client_done=0,up_writable=1;
for(;;){
int pending=le_tls_pending(tls)>0;
p[0].fd=cfd;p[0].events=POLLIN;p[0].revents=0;
p[1].fd=up;p[1].events=POLLIN;p[1].revents=0;
if(poll(p,2,pending?0:60000)<0)break;
if(!pending&&!p[0].revents&&!p[1].revents)break;   /* idle timeout */

/* upstream first: never let a ready response go unread */
if(p[1].revents&(POLLIN|POLLHUP|POLLERR)){
n=read(up,buf,sizeof(buf));
if(n<=0)break;
if(le_tls_write(tls,buf,(size_t)n)!=n)break;
continue;
}

if(!client_done&&(pending||(p[0].revents&(POLLIN|POLLHUP|POLLERR)))){
n=le_tls_read(tls,buf,sizeof(buf));
if(n<=0){client_done=1;shutdown(up,SHUT_WR);continue;}
if(up_writable&&write_all_file(up,buf,(size_t)n)<0){
/* stop sending, but keep draining whatever the server already replied */
up_writable=0;shutdown(up,SHUT_WR);
}}}

/* final drain: forward any response still buffered upstream */
for(;;){
struct pollfd d={up,POLLIN,0};
if(poll(&d,1,250)<=0)break;
n=read(up,buf,sizeof(buf));
if(n<=0)break;
if(le_tls_write(tls,buf,(size_t)n)!=n)break;
}
close(up);le_tls_close(tls);return;}

/* Double-fork so the relay is reaped by init and never becomes a zombie the
   existing SIGCHLD bookkeeping would have to account for. */
static void spawn_tls_relay(int cfd,const struct http_options*o){
pid_t pid=fork();
if(pid<0)return;
if(pid==0){
if(fork()==0){tls_relay(cfd,o);_exit(0);}
_exit(0);
}
waitpid(pid,NULL,0);
}

int http_server_run(const struct http_options*o,struct api_context*api,volatile int*running){int ls,tls_ls=-1,i,yes=1,max=o->max_clients<1?LE_MAX_CLIENTS:o->max_clients;struct sigaction child_action;
struct sockaddr_in a;
struct client c[LE_MAX_CLIENTS];
struct pollfd p[LE_MAX_CLIENTS+2];
time_t last_tick=0;memset(&child_action,0,sizeof(child_action));child_action.sa_handler=reap_assistant_workers;sigemptyset(&child_action.sa_mask);child_action.sa_flags=SA_RESTART;if(sigaction(SIGCHLD,&child_action,NULL)<0)return-1;
if(max>LE_MAX_CLIENTS)max=LE_MAX_CLIENTS;
memset(c,0,sizeof(c));
for(i=0;
i<LE_MAX_CLIENTS;
i++)c[i].fd=-1;
ls=socket(AF_INET,SOCK_STREAM,0);
if(ls<0)return-1;
if(close_on_exec(ls)<0){close(ls);return-1;}
setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
memset(&a,0,sizeof(a));
a.sin_family=AF_INET;
a.sin_port=htons((uint16_t)o->port);
if(inet_pton(AF_INET,o->listen_host,&a.sin_addr)!=1){close(ls);
return-1;
}if(bind(ls,(struct sockaddr*)&a,sizeof(a))||listen(ls,max)){close(ls);
return-1;
}if(o->run_user[0]){struct passwd*pw=getpwnam(o->run_user);if(!pw){fprintf(stderr,"Unknown privilege-drop user: %s\n",o->run_user);close(ls);return-1;}if(setgroups(0,0)||setgid(pw->pw_gid)||setuid(pw->pw_uid)){perror("privilege drop");close(ls);return-1;}fprintf(stderr,"Dropped privileges to %s\n",o->run_user);}
fprintf(stderr,"LibreEcho listening on http://%s:%d (%s backend)\n",o->listen_host,o->port,le_backend_mode(api->backend));
if(o->tls_port>0){
struct sockaddr_in ta;int tyes=1;
tls_ls=socket(AF_INET,SOCK_STREAM,0);
if(tls_ls>=0&&close_on_exec(tls_ls)<0){close(tls_ls);tls_ls=-1;}
if(tls_ls>=0){
setsockopt(tls_ls,SOL_SOCKET,SO_REUSEADDR,&tyes,sizeof(tyes));
memset(&ta,0,sizeof(ta));ta.sin_family=AF_INET;ta.sin_port=htons((uint16_t)o->tls_port);
if(inet_pton(AF_INET,o->listen_host,&ta.sin_addr)!=1||bind(tls_ls,(struct sockaddr*)&ta,sizeof(ta))||listen(tls_ls,max)){
/* HTTPS is best-effort: losing it must never take the HTTP UI down with it */
fprintf(stderr,"HTTPS listener on port %d unavailable: %s\n",o->tls_port,strerror(errno));
close(tls_ls);tls_ls=-1;
}else fprintf(stderr,"LibreEcho also listening on https://%s:%d\n",o->listen_host,o->tls_port);
}}
while(*running){p[0].fd=ls;
p[0].events=POLLIN;
p[max+1].fd=tls_ls;
p[max+1].events=POLLIN;
for(i=0;
i<max;
i++){p[i+1].fd=c[i].fd;
p[i+1].events=POLLIN;
}if(poll(p,(nfds_t)(max+2),500)<0&&errno!=EINTR)break;
if(p[0].revents&POLLIN){int fd=accept(ls,0,0);
if(fd>=0&&close_on_exec(fd)<0){close(fd);fd=-1;}
if(fd>=0){for(i=0;
i<max&&c[i].fd>=0;
i++){/* find free bounded slot */}if(i==max){response(fd,503,"text/plain","Server busy",11);
close(fd);
}else c[i].fd=fd;
}}if(tls_ls>=0&&(p[max+1].revents&POLLIN)){int tfd=accept(tls_ls,0,0);
if(tfd>=0&&close_on_exec(tfd)<0){close(tfd);tfd=-1;}
if(tfd>=0){spawn_tls_relay(tfd,o);close(tfd);}
}for(i=0;
i<max;
i++)if(c[i].fd>=0&&(p[i+1].revents&(POLLIN|POLLHUP|POLLERR))){ssize_t n=recv(c[i].fd,c[i].buf+c[i].used,LE_REQ_MAX-c[i].used,0);
if(n<=0){close(c[i].fd);
c[i].fd=-1;
}else{c[i].used+=(size_t)n;
c[i].buf[c[i].used]=0;
process(&c[i],o,api);
}}if(time(0)!=last_tick){last_tick=time(0);
le_backend_tick(api->backend);
}}for(i=0;
i<max;
i++)if(c[i].fd>=0)close(c[i].fd);
if(tls_ls>=0)close(tls_ls);
close(ls);
return 0;
}
