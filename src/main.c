#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#include "api.h"
#include "backend.h"
#include "http_server.h"
#include "tls.h"
#include "log.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pwd.h>
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
/* mkdir -p; the TLS directory usually needs /data/libreecho/config created too. */
static void mkdir_p_mode(const char*path,mode_t mode){char tmp[320];size_t i,n;
n=strlen(path);if(n>=sizeof(tmp))return;memcpy(tmp,path,n+1);
for(i=1;i<n;i++)if(tmp[i]=='/'){tmp[i]=0;mkdir(tmp,mode);tmp[i]='/';}
mkdir(tmp,mode);}

int main(int argc,char**argv){const char*mode="mock",*cfg="./config/runtime.json",*mock="./config/mock-state.json",*token_path=0,*users_path=0,*allowed_origin=0;
unsigned seed=0;
int dev=0,insecure_lan=0,i;
struct le_backend*b=0;
struct api_context api;
struct http_options o;
char listen[96]="127.0.0.1:8080",token[192]={0},csrf[65],*colon;
char tls_dir[320]="/data/libreecho/config/tls";
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
else if(!strcmp(argv[i],"--tls-port")&&i+1<argc)o.tls_port=atoi(argv[++i]);
else if(!strcmp(argv[i],"--tls-dir")&&i+1<argc)strncpy(tls_dir,argv[++i],sizeof(tls_dir)-1);
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
{char unrestored[192];int apply_rc=LE_IO,apply_try;/* The daemons come up alongside this one, so the first pass usually runs before some of their sockets exist. The loop stops as soon as everything applies, so on a healthy boot this costs a pass or two. The 6s ceiling is deliberate: a setting that can never apply would otherwise hold the web UI down on every boot, and the named warning below is what surfaces that case rather than silently spinning. */for(apply_try=0;apply_try<24&&apply_rc;apply_try++){apply_rc=api_apply_persisted_configuration(&api,unrestored,sizeof(unrestored));if(apply_rc)usleep(250000);}if(apply_rc){char message[256];snprintf(message,sizeof(message),"Could not restore saved settings: %s",unrestored[0]?unrestored:"unknown");api_log(&api,"warning",message);}else if(apply_try>1)api_log(&api,"info","Saved settings restored once the hardware daemons were ready");}
if(api.integrations&8u){int bluetooth_rc=le_set_bluetooth_enabled(b,1);if(bluetooth_rc)api_log(&api,"error","Bluetooth could not be started from saved configuration");}
if(api.integrations&16u){int airplay_rc=LE_IO,airplay_try;for(airplay_try=0;airplay_try<12&&airplay_rc;airplay_try++){airplay_rc=le_set_airplay_enabled(b,1);if(airplay_rc)usleep(500000);}if(airplay_rc){api_log(&api,"error","AirPlay 2 could not be started from saved configuration");fprintf(stderr,"Unable to start configured AirPlay 2 integration: %d\n",airplay_rc);}}
/* The saved feature flag turns HTTPS on at boot; --tls-port still overrides
   it, and picks the port when the flag is what enabled it. */
if(!o.tls_port&&api.feature_https)o.tls_port=8443;
if(o.tls_port>0){
if(o.tls_port<1||o.tls_port>65535||o.tls_port==o.port){fprintf(stderr,"Invalid --tls-port\n");le_backend_destroy(b);return 2;}
snprintf(o.tls_cert,sizeof(o.tls_cert),"%s/server.crt",tls_dir);
snprintf(o.tls_key,sizeof(o.tls_key),"%s/server.key",tls_dir);
mkdir_p_mode(tls_dir,0700);
if(le_tls_ensure_self_signed(o.tls_cert,o.tls_key,"libreecho")!=LE_OK){
/* Never fatal: HTTP stays up so a certificate problem cannot lock out the UI */
fprintf(stderr,"HTTPS disabled: could not create a certificate in %s\n",tls_dir);
o.tls_port=0;
}else{
api.https_port=o.tls_port;
strncpy(api.https_cert,o.tls_cert,sizeof(api.https_cert)-1);
}
if(o.tls_port>0&&o.run_user[0]){
/* http_server_run drops privileges before the relay child reads these */
struct passwd*tp=getpwnam(o.run_user);
if(tp&&(chown(tls_dir,tp->pw_uid,tp->pw_gid)||chown(o.tls_cert,tp->pw_uid,tp->pw_gid)||chown(o.tls_key,tp->pw_uid,tp->pw_gid)))
fprintf(stderr,"Warning: could not give %s ownership of the TLS key\n",o.run_user);
}}
signal(SIGINT,stop);
signal(SIGTERM,stop);
signal(SIGPIPE,SIG_IGN);
i=http_server_run(&o,&api,&running);
le_backend_destroy(b);
return i?1:0;
}
