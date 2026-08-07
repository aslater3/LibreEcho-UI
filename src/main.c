#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include "api.h"
#include "backend.h"
#include "http_server.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
static volatile int running=1;
static void copy_arg(char*dst,size_t size,const char*src){size_t n;if(!size)return;n=strlen(src?src:"");if(n>=size)n=size-1;if(n)memcpy(dst,src,n);dst[n]=0;}
static void stop(int s){(void)s;
running=0;
}static void usage(const char*p){fprintf(stderr,"Usage: %s [--backend mock|linux] [--listen IP:PORT] [--web-root PATH] [--config PATH] [--mock-config PATH] [--seed N] [--dev-controls] [--auth-token-file PATH] [--users-file PATH] [--allowed-origin URL] [--user NAME] [--allow-insecure-lan] [--verbose] [--debug] [--quiet]\n",p);
}
static int read_token(const char*path,char*out,size_t size){FILE*f;size_t n;struct stat st;if(!path)return 0;if(stat(path,&st)||!S_ISREG(st.st_mode)||(st.st_mode&077))return-1;f=fopen(path,"r");if(!f)return-1;n=fread(out,1,size-1,f);fclose(f);while(n&&(out[n-1]=='\n'||out[n-1]=='\r'||out[n-1]==' '||out[n-1]=='\t'))n--;out[n]=0;return n>=16?0:-1;}
static int random_csrf(char*out,size_t size){static const char hex[]="0123456789abcdef";unsigned char bytes[32];size_t got=0,i;ssize_t n;int fd;if(!out||size<65)return-1;fd=open("/dev/urandom",O_RDONLY);if(fd<0)return-1;while(got<sizeof(bytes)){n=read(fd,bytes+got,sizeof(bytes)-got);if(n<=0){close(fd);return-1;}got+=(size_t)n;}close(fd);for(i=0;i<sizeof(bytes);i++){out[i*2]=hex[bytes[i]>>4];out[i*2+1]=hex[bytes[i]&15];}out[64]=0;return 0;}
static int valid_users_path(const char*path){struct stat st;if(!path||!path[0]||stat(path,&st)||!S_ISREG(st.st_mode)||(st.st_mode&077))return 0;return 1;}
int main(int argc,char**argv){const char*mode="mock",*cfg="./config/runtime.json",*mock="./config/mock-state.json",*token_path=0,*users_path=0,*allowed_origin=0;
unsigned seed=0;
int dev=0,insecure_lan=0,i;
struct le_backend*b=0;
struct api_context api;
struct http_options o;
char listen[96]="127.0.0.1:8080",token[192]={0},csrf[65],*colon;
le_log_init("libreecho-web",argc,argv);
memset(&o,0,sizeof(o));
strcpy(o.web_root,"./web");
o.max_clients=16;
for(i=1;
i<argc;
i++){if(!strcmp(argv[i],"--backend")&&i+1<argc)mode=argv[++i];
else if(!strcmp(argv[i],"--listen")&&i+1<argc)strncpy(listen,argv[++i],sizeof(listen)-1);
else if(!strcmp(argv[i],"--web-root")&&i+1<argc)strncpy(o.web_root,argv[++i],sizeof(o.web_root)-1);
else if(!strcmp(argv[i],"--config")&&i+1<argc)cfg=argv[++i];
else if(!strcmp(argv[i],"--mock-config")&&i+1<argc)mock=argv[++i];
else if(!strcmp(argv[i],"--seed")&&i+1<argc)seed=(unsigned)strtoul(argv[++i],0,10);
else if(!strcmp(argv[i],"--dev-controls"))dev=1;
else if(!strcmp(argv[i],"--auth-token-file")&&i+1<argc)token_path=argv[++i];
else if(!strcmp(argv[i],"--users-file")&&i+1<argc)users_path=argv[++i];
else if(!strcmp(argv[i],"--allowed-origin")&&i+1<argc)allowed_origin=argv[++i];
else if(!strcmp(argv[i],"--user")&&i+1<argc)strncpy(o.run_user,argv[++i],sizeof(o.run_user)-1);
else if(!strcmp(argv[i],"--allow-insecure-lan"))insecure_lan=1;
else if(!strcmp(argv[i],"--verbose")||!strcmp(argv[i],"--debug")||!strcmp(argv[i],"--quiet")||!strcmp(argv[i],"--syslog")){}
else if(!strcmp(argv[i],"--help")){usage(argv[0]);
return 0;
}else{usage(argv[0]);
return 2;
}}colon=strrchr(listen,':');
if(!colon){usage(argv[0]);
return 2;
}*colon=0;
copy_arg(o.listen_host,sizeof(o.listen_host),listen);
o.port=atoi(colon+1);
if(o.port<1||o.port>65535){fprintf(stderr,"Invalid port\n");
return 2;
}if(token_path&&read_token(token_path,token,sizeof(token))){fprintf(stderr,"Authentication token file must be readable and contain at least 16 characters\n");return 2;}
if(strcmp(o.listen_host,"127.0.0.1")&&strcmp(o.listen_host,"::1")&&!token[0]&&!users_path&&!insecure_lan){fprintf(stderr,"Refusing unauthenticated LAN bind; use --auth-token-file, --users-file or explicit --allow-insecure-lan\n");return 2;}
if(users_path&&access(users_path,F_OK)==0&&!valid_users_path(users_path)){fprintf(stderr,"Users file must be a regular private file: %s\n",users_path);return 2;}
if(le_backend_init(&b,mode,mock,cfg,seed)!=LE_OK){fprintf(stderr,"Unable to initialise %s backend\n",mode);
return 1;
}if(random_csrf(csrf,sizeof(csrf))){fprintf(stderr,"Unable to obtain secure CSRF token\n");le_backend_destroy(b);return 2;}if(api_init(&api,b,dev,insecure_lan,token,allowed_origin,csrf,cfg,users_path)){fprintf(stderr,"Unable to initialise authentication\n");le_backend_destroy(b);return 2;}if(cfg&&access(cfg,F_OK)==0)api.setup_completed=1;
{int apply_rc=LE_IO,apply_try;for(apply_try=0;apply_try<12&&apply_rc;apply_try++){apply_rc=api_apply_persisted_configuration(&api);if(apply_rc)usleep(250000);}if(apply_rc)api_log(&api,"warning","One or more persisted hardware settings could not be restored");}
if(api.integrations&8u){int bluetooth_rc=le_set_bluetooth_enabled(b,1);if(bluetooth_rc)api_log(&api,"error","Bluetooth could not be started from saved configuration");}
if(api.integrations&16u){int airplay_rc=LE_IO,airplay_try;for(airplay_try=0;airplay_try<12&&airplay_rc;airplay_try++){airplay_rc=le_set_airplay_enabled(b,1);if(airplay_rc)usleep(500000);}if(airplay_rc){api_log(&api,"error","AirPlay 2 could not be started from saved configuration");fprintf(stderr,"Unable to start configured AirPlay 2 integration: %d\n",airplay_rc);}}
signal(SIGINT,stop);
signal(SIGTERM,stop);
signal(SIGPIPE,SIG_IGN);
i=http_server_run(&o,&api,&running);
le_backend_destroy(b);
return i?1:0;
}
