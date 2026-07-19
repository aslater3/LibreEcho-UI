#include "api.h"
#include "backend.h"
#include "http_server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static volatile int running=1;
static void stop(int s){(void)s;
running=0;
}static void usage(const char*p){fprintf(stderr,"Usage: %s [--backend mock|linux] [--listen IP:PORT] [--web-root PATH] [--config PATH] [--mock-config PATH] [--seed N] [--dev-controls] [--auth-token-file PATH] [--allowed-origin URL] [--user NAME] [--allow-insecure-lan]\n",p);
}
static int read_token(const char*path,char*out,size_t size){FILE*f;size_t n;struct stat st;if(!path)return 0;if(stat(path,&st)||!S_ISREG(st.st_mode)||(st.st_mode&077))return-1;f=fopen(path,"r");if(!f)return-1;n=fread(out,1,size-1,f);fclose(f);while(n&&(out[n-1]=='\n'||out[n-1]=='\r'||out[n-1]==' '||out[n-1]=='\t'))n--;out[n]=0;return n>=16?0:-1;}
int main(int argc,char**argv){const char*mode="mock",*cfg="./config/runtime.json",*mock="./config/mock-state.json",*token_path=0,*allowed_origin=0;
unsigned seed=0;
int dev=0,insecure_lan=0,i;
struct le_backend*b=0;
struct api_context api;
struct http_options o;
char listen[96]="127.0.0.1:8080",token[192]={0},*colon;
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
else if(!strcmp(argv[i],"--allowed-origin")&&i+1<argc)allowed_origin=argv[++i];
else if(!strcmp(argv[i],"--user")&&i+1<argc)strncpy(o.run_user,argv[++i],sizeof(o.run_user)-1);
else if(!strcmp(argv[i],"--allow-insecure-lan"))insecure_lan=1;
else if(!strcmp(argv[i],"--help")){usage(argv[0]);
return 0;
}else{usage(argv[0]);
return 2;
}}colon=strrchr(listen,':');
if(!colon){usage(argv[0]);
return 2;
}*colon=0;
strncpy(o.listen_host,listen,sizeof(o.listen_host)-1);
o.port=atoi(colon+1);
if(o.port<1||o.port>65535){fprintf(stderr,"Invalid port\n");
return 2;
}if(token_path&&read_token(token_path,token,sizeof(token))){fprintf(stderr,"Authentication token file must be readable and contain at least 16 characters\n");return 2;}
if(strcmp(o.listen_host,"127.0.0.1")&&strcmp(o.listen_host,"::1")&&!token[0]&&!insecure_lan){fprintf(stderr,"Refusing unauthenticated LAN bind; use --auth-token-file or explicit --allow-insecure-lan\n");return 2;}
if(le_backend_init(&b,mode,mock,cfg,seed)!=LE_OK){fprintf(stderr,"Unable to initialise %s backend\n",mode);
return 1;
}api_init(&api,b,dev,token,allowed_origin,cfg);
signal(SIGINT,stop);
signal(SIGTERM,stop);
signal(SIGPIPE,SIG_IGN);
i=http_server_run(&o,&api,&running);
le_backend_destroy(b);
return i?1:0;
}
