#include "http_server.h"
#include "adapter/adapter.h"
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
#define LE_UPDATE_MAX 33554432
#define LE_UPDATE_PATH "/data/libreecho/update/incoming/manual.tar"
#define LE_UPDATE_TMP "/data/libreecho/update/incoming/manual.tar.tmp"
#define LE_UPDATE_LOCK "/data/libreecho/update/incoming/upload.lock"
struct client{int fd;
size_t used;
char buf[LE_REQ_MAX+1];
};

static const char*mime(const char*p){const char*e=strrchr(p,'.');
if(!e)return"application/octet-stream";
if(!strcmp(e,".html"))return"text/html; charset=utf-8";
if(!strcmp(e,".css"))return"text/css; charset=utf-8";
if(!strcmp(e,".js"))return"application/javascript; charset=utf-8";
if(!strcmp(e,".json"))return"application/json; charset=utf-8";
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
const char*reason=code==200?"OK":code==400?"Bad Request":code==403?"Forbidden":code==404?"Not Found":code==405?"Method Not Allowed":code==413?"Payload Too Large":code==429?"Too Many Requests":code==501?"Not Implemented":code==503?"Service Unavailable":"Error";
int z=snprintf(h,sizeof(h),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; script-src 'self'\r\n\r\n",code,reason,type,(unsigned long)n);
send_all(fd,h,(size_t)z);
if(n)send_all(fd,body,n);
}
static int stream_send_all(int fd,const void*body,size_t n){const char*p=body;while(n){ssize_t w=send(fd,p,n,0);if(w<=0)return-1;p+=w;n-=(size_t)w;}return 0;}
static int stream_microphone(int fd,int selected_channel){struct sockaddr_un address;int microphone=-1;char request[160],reply[LE_ADAPTER_MSG_MAX],buffer[8192],header[512];size_t used=0;ssize_t n;microphone=socket(AF_UNIX,SOCK_STREAM,0);if(microphone<0)goto unavailable;memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;strncpy(address.sun_path,LE_ADAPTER_MIC_SOCK,sizeof(address.sun_path)-1);if(connect(microphone,(struct sockaddr*)&address,sizeof(address))<0)goto unavailable;n=snprintf(request,sizeof(request),"{\"v\":1,\"id\":1,\"cmd\":\"stream_raw\",\"args\":{\"channel\":%d}}\n",selected_channel);if(n<0||stream_send_all(microphone,request,(size_t)n)<0)goto unavailable;while(used+1<sizeof(reply)){n=read(microphone,reply+used,1);if(n<=0)goto unavailable;if(reply[used++]=='\n')break;}reply[used]='\0';if(!strstr(reply,"\"ok\":true"))goto unavailable;signal(SIGPIPE,SIG_IGN);n=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nCache-Control: no-store\r\nX-LibreEcho-Audio: pcm_s24_3le;rate=16000;channels=9;container-bits=24;valid-bits=16;selected-channel=%d;calibration=none\r\nX-Content-Type-Options: nosniff\r\n\r\n",selected_channel);if(n<0||stream_send_all(fd,header,(size_t)n)<0)goto stop;while((n=read(microphone,buffer,sizeof(buffer)))>0){int z=snprintf(header,sizeof(header),"%zx\r\n",(size_t)n);if(z<0||stream_send_all(fd,header,(size_t)z)<0||stream_send_all(fd,buffer,(size_t)n)<0||stream_send_all(fd,"\r\n",2)<0)goto stop;}if(n==0)(void)stream_send_all(fd,"0\r\n\r\n",5);goto stop;unavailable:{const char*message="{\"ok\":false,\"data\":null,\"error\":{\"code\":\"microphone_unavailable\",\"message\":\"Microphone service could not start the stream\"}}";response(fd,503,"application/json",message,strlen(message));}stop:if(microphone>=0)close(microphone);close(fd);return 0;}
static int start_pcm_stream(int fd,int selected_channel){pid_t pid=fork();if(pid<0)return-1;if(pid==0){(void)stream_microphone(fd,selected_channel);_exit(0);}close(fd);return 0;}
static int write_all_file(int fd,const void*body,size_t n){const char*p=body;while(n){ssize_t w=write(fd,p,n);if(w<=0)return-1;p+=w;n-=(size_t)w;}return 0;}
static void update_error(int fd,int status,const char*code,const char*message){char body[512];int n=snprintf(body,sizeof(body),"{\"ok\":false,\"data\":null,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",code,message);response(fd,status,"application/json",body,(size_t)n);}
static int stream_update_upload(int fd,const char*initial,size_t initial_len,size_t content_len){static const char success[]="{\"ok\":true,\"data\":{\"installed\":true,\"state\":\"reboot-pending\"},\"error\":null}";char buffer[16384];size_t received=initial_len;int out=-1,lock=-1,status;ssize_t n;pid_t child;if(mkdir("/data/libreecho",0700)&&errno!=EEXIST)goto io;if(mkdir("/data/libreecho/update",0700)&&errno!=EEXIST)goto io;if(mkdir("/data/libreecho/update/incoming",0700)&&errno!=EEXIST)goto io;lock=open(LE_UPDATE_LOCK,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(lock<0){update_error(fd,409,"update_busy","Another update upload is active");goto done;}close(lock);lock=1;out=open(LE_UPDATE_TMP,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(out<0)goto io;if(initial_len&&write_all_file(out,initial,initial_len))goto io;while(received<content_len){size_t want=content_len-received;if(want>sizeof(buffer))want=sizeof(buffer);n=recv(fd,buffer,want,0);if(n<=0)goto io;if(write_all_file(out,buffer,(size_t)n))goto io;received+=(size_t)n;}if(fsync(out)||close(out)){out=-1;goto io;}out=-1;if(rename(LE_UPDATE_TMP,LE_UPDATE_PATH))goto io;child=fork();if(child<0)goto io;if(child==0){execl("/usr/local/sbin/libreecho-update","libreecho-update","install",LE_UPDATE_PATH,(char*)0);_exit(127);}if(waitpid(child,&status,0)<0||!WIFEXITED(status)||WEXITSTATUS(status)!=0){update_error(fd,400,"update_rejected","The signed update failed verification or installation");goto done;}response(fd,200,"application/json",success,sizeof(success)-1);goto done;io:if(out>=0)close(out);update_error(fd,503,"io_error","The update upload could not be stored");done:unlink(LE_UPDATE_TMP);if(lock==1)unlink(LE_UPDATE_LOCK);close(fd);return 0;}
static int start_update_upload(int fd,const char*initial,size_t initial_len,size_t content_len){pid_t pid=fork();if(pid<0)return-1;if(pid==0){(void)stream_update_upload(fd,initial,initial_len,content_len);_exit(0);}close(fd);return 0;}
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
static void process(struct client*c,const struct http_options*o,struct api_context*api){char*end=strstr(c->buf,"\r\n\r\n"),*body,*cl;
size_t headers,body_len=0,content_len=0;
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
if(!strcmp(q.path,"/api/v1/system/update/upload")){size_t initial;copy_header(q.origin,sizeof(q.origin),header(c->buf,"Origin"));copy_header(q.authorization,sizeof(q.authorization),header(c->buf,"Authorization"));copy_header(q.csrf,sizeof(q.csrf),header(c->buf,"X-LibreEcho-CSRF"));if(!content_len||content_len>LE_UPDATE_MAX){update_error(c->fd,413,"update_size","Update must be between 1 byte and 32 MiB");goto done;}if(!api_update_upload_authorize(api,&q,&r)){response(c->fd,r.status,r.type,r.body,r.length);goto done;}initial=c->used>headers?c->used-headers:0;if(initial>content_len)initial=content_len;if(start_update_upload(c->fd,end+4,initial,content_len)<0){update_error(c->fd,503,"io_error","The update upload could not start");goto done;}c->fd=-1;c->used=0;return;}
if(content_len>LE_BODY_MAX){response(c->fd,413,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"body_too_large\",\"message\":\"Request body exceeds 16 KiB\"}}",118);
goto done;
}if(c->used<headers+content_len)return;
body=end+4;
body_len=content_len;
q.body=body;
q.body_len=body_len;
body[body_len]=0;
copy_header(q.origin,sizeof(q.origin),header(c->buf,"Origin"));
copy_header(q.authorization,sizeof(q.authorization),header(c->buf,"Authorization"));
copy_header(q.csrf,sizeof(q.csrf),header(c->buf,"X-LibreEcho-CSRF"));
copy_header(q.confirm,sizeof(q.confirm),header(c->buf,"X-LibreEcho-Confirm"));
if(!strncmp(q.path,"/api/v1/baby-monitor/stream",27)){int card,device,channels,bits,selected_channel;if(!api_baby_monitor_stream_authorize(api,&q,&r,&card,&device,&channels,&bits,&selected_channel)){response(c->fd,r.status,r.type,r.body,r.length);goto done;}if(start_pcm_stream(c->fd,selected_channel)<0){response(c->fd,503,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"io\",\"message\":\"Microphone stream could not start\"}}",118);goto done;}c->fd=-1;c->used=0;return;}if(!strncmp(q.path,"/api/",5)){time_t now=time(0);
if(strstr(q.path,"/system/")&&strcmp(q.method,"GET")&&last_destructive&&now-last_destructive<3){response(c->fd,429,"application/json","{\"ok\":false,\"data\":null,\"error\":{\"code\":\"rate_limited\",\"message\":\"Wait before another device action\"}}",115);
goto done;
}if(strstr(q.path,"/system/")&&strcmp(q.method,"GET"))last_destructive=now;
api_handle(api,&q,&r);
if(r.status>=200&&r.status<300&&!strcmp(q.method,"PUT")&&(!strcmp(q.path,"/api/v1/audio")||!strcmp(q.path,"/api/v1/led")||!strcmp(q.path,"/api/v1/network")||!strcmp(q.path,"/api/v1/wake-word")||!strcmp(q.path,"/api/v1/buttons")||!strcmp(q.path,"/api/v1/privacy")||!strncmp(q.path,"/api/v1/integrations/",21))&&api_persist_configuration(api)){r.status=503;strcpy(r.type,"application/json; charset=utf-8");strcpy(r.body,"{\"ok\":false,\"data\":null,\"error\":{\"code\":\"io_error\",\"message\":\"Configuration change could not be saved\"}}");r.length=strlen(r.body);}
response(c->fd,r.status,r.type,r.body,r.length);
if(body_len)memset(body,0,body_len);
}else if(strcmp(q.method,"GET")&&strcmp(q.method,"HEAD"))response(c->fd,405,"text/plain","Method not allowed",18);
else if(serve_file(c->fd,o,!api->setup_completed&&!strcmp(q.path,"/")?"/setup.html":q.path))response(c->fd,404,"text/plain","Not found",9);

done:close(c->fd);
c->fd=-1;
c->used=0;
}
int http_server_run(const struct http_options*o,struct api_context*api,volatile int*running){int ls,i,yes=1,max=o->max_clients<1?LE_MAX_CLIENTS:o->max_clients;
struct sockaddr_in a;
struct client c[LE_MAX_CLIENTS];
struct pollfd p[LE_MAX_CLIENTS+1];
time_t last_tick=0;
if(max>LE_MAX_CLIENTS)max=LE_MAX_CLIENTS;
memset(c,0,sizeof(c));
for(i=0;
i<LE_MAX_CLIENTS;
i++)c[i].fd=-1;
ls=socket(AF_INET,SOCK_STREAM,0);
if(ls<0)return-1;
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
while(*running){p[0].fd=ls;
p[0].events=POLLIN;
for(i=0;
i<max;
i++){p[i+1].fd=c[i].fd;
p[i+1].events=POLLIN;
}if(poll(p,(nfds_t)(max+1),500)<0&&errno!=EINTR)break;
if(p[0].revents&POLLIN){int fd=accept(ls,0,0);
if(fd>=0){for(i=0;
i<max&&c[i].fd>=0;
i++){/* find free bounded slot */}if(i==max){response(fd,503,"text/plain","Server busy",11);
close(fd);
}else c[i].fd=fd;
}}for(i=0;
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
close(ls);
return 0;
}
