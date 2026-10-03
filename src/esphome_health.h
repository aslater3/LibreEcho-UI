#ifndef LE_ESPHOME_HEALTH_H
#define LE_ESPHOME_HEALTH_H
/* Linux-only, bounded process/status/socket evidence shared by all HA owners.
 * Never infer executable identity from argv[0], comm, kill(pid,0), or a port.
 * Overrides are trusted launcher settings, not HTTP/request configuration. */
#include "json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline const char *le_eh_setting(const char *name,const char *fallback)
{
    const char *value=getenv(name);return value&&*value?value:fallback;
}
static inline int le_eh_read(const char *path,char *data,size_t size,int regular)
{
    int fd;ssize_t n;struct stat st;
    fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_NOFOLLOW);
    if(fd<0)return 0;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(regular&&(st.st_size<=0||(size_t)st.st_size>=size))){close(fd);return 0;}
    n=read(fd,data,size-1);close(fd);
    if(n<=0||(size_t)n>=size-1||(regular&&n!=st.st_size))return 0;
    data[n]=0;return (int)n;
}
static inline int le_eh_decimal(const char *text)
{
    if(!text||!*text)return 0;
    for(;*text;text++)if(*text<'0'||*text>'9')return 0;
    return 1;
}
static inline int le_eh_pid(const char *path)
{
    char data[64],*end;long pid;
    if(!le_eh_read(path,data,sizeof data,1))return 0;
    data[strcspn(data,"\r\n")]=0;if(!le_eh_decimal(data))return 0;
    errno=0;pid=strtol(data,&end,10);return !errno&&!*end&&pid>1&&pid<=INT_MAX?(int)pid:0;
}
static inline int le_eh_executable(int pid,const char *daemon)
{
    char path[64],actual[4096],trusted[4096];int fd;ssize_t a,b;
    struct stat expected,observed;
    fd=open(daemon,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
    snprintf(path,sizeof path,"/proc/self/fd/%d",fd);
    b=readlink(path,trusted,sizeof trusted-1);
    snprintf(path,sizeof path,"/proc/%d/exe",pid);
    a=readlink(path,actual,sizeof actual-1);
    int ok=a>0&&b>0&&(size_t)a<sizeof actual-1&&(size_t)b<sizeof trusted-1&&
        !fstat(fd,&expected)&&S_ISREG(expected.st_mode)&&!stat(path,&observed)&&
        expected.st_dev==observed.st_dev&&expected.st_ino==observed.st_ino;
    close(fd);if(!ok)return 0;actual[a]=0;trusted[b]=0;
    return !strcmp(actual,trusted);
}
static inline int le_eh_start(int pid,char start[32])
{
    char path[64],data[2048],*field,*end,*save;unsigned number=3;
    snprintf(path,sizeof path,"/proc/%d/stat",pid);
    if(!le_eh_read(path,data,sizeof data,0))return 0;
    end=strrchr(data,')');if(!end||end[1]!=' ')return 0;
    field=strtok_r(end+2," \n",&save);
    if(!field||field[0]=='Z'||field[0]=='X')return 0;
    while(field&&number<22){field=strtok_r(NULL," \n",&save);number++;}
    if(!field||!le_eh_decimal(field)||strlen(field)>=32)return 0;
    snprintf(start,32,"%s",field);return 1;
}
static inline int le_eh_boot(char boot[64])
{
    if(!le_eh_read("/proc/sys/kernel/random/boot_id",boot,64,0))return 0;
    boot[strcspn(boot,"\r\n")]=0;return strlen(boot)==36;
}
static inline unsigned le_eh_port(void)
{
    const char *value=le_eh_setting("LIBREECHO_ESPHOME_PORT","6053");char *end;
    unsigned long port;if(!le_eh_decimal(value))return 0;
    errno=0;port=strtoul(value,&end,10);return !errno&&!*end&&port>0&&port<=65535?(unsigned)port:0;
}
static inline int le_eh_listener(int pid,unsigned port,const char *inode)
{
    char path[64],line[1024],want[64];DIR *fds;struct dirent *entry;
    FILE *net;unsigned count=0;int owned=0,listening=0;
    if(!port||!le_eh_decimal(inode)||!strcmp(inode,"0")||strlen(inode)>20)return 0;
    snprintf(want,sizeof want,"socket:[%s]",inode);
    snprintf(path,sizeof path,"/proc/%d/fd",pid);fds=opendir(path);if(!fds)return 0;
    while((entry=readdir(fds))!=NULL&&count++<4096){char fdpath[128],target[64];ssize_t n;
        if(!le_eh_decimal(entry->d_name))continue;
        if(snprintf(fdpath,sizeof fdpath,"%s/%s",path,entry->d_name)>=(int)sizeof fdpath)continue;
        n=readlink(fdpath,target,sizeof target-1);
        if(n<=0||(size_t)n>=sizeof target-1)continue;
        target[n]=0;
        if(!strcmp(target,want)){owned=1;break;}
    }
    closedir(fds);if(!owned)return 0;
    snprintf(path,sizeof path,"/proc/%d/net/tcp",pid);net=fopen(path,"r");if(!net)return 0;
    count=0;
    while(fgets(line,sizeof line,net)&&count++<4096){char *save,*token,*local=NULL,*state=NULL,*ino=NULL;unsigned field=0;
        for(token=strtok_r(line," \t\n",&save);token;token=strtok_r(NULL," \t\n",&save),field++){
            if(field==1)local=token;else if(field==3)state=token;else if(field==9){ino=token;break;}
        }
        if(local&&state&&ino&&!strcmp(state,"0A")&&!strcmp(ino,inode)){
            char *colon=strrchr(local,':'),*end;unsigned long p;
            if(!colon)continue;
            p=strtoul(colon+1,&end,16);
            if(!*end&&p==port){listening=1;break;}
        }
    }
    fclose(net);return listening;
}
/* process_only is for safe stop/status: not-ready daemon output still binds
 * identity. Health/connected additionally require that process's own listener. */
static inline int le_esphome_health(const char *pidfile,const char *status,const char *daemon,unsigned port,const char *flag,int process_only)
{
    char data[2049],start[32],after[32],recorded[32],boot[64],recorded_boot[64],inode[32];
    int pid=le_eh_pid(pidfile),ready=0,value=0,n;long long creator=0;
    static const char *const fields[]={"pid","start_time","boot_id","listener_inode","port","ready","connected"};
    if(!pid||!le_eh_executable(pid,daemon)||!le_eh_start(pid,start)||!le_eh_boot(boot))return 0;
    n=le_eh_read(status,data,sizeof data,1);if(!n||!json_valid_object(data,(size_t)n))return 0;
    for(unsigned i=0;i<sizeof fields/sizeof fields[0];i++)if(json_duplicate_key(data,(size_t)n,fields[i]))return 0;
    if(json_get_int64_top_level(data,"pid",&creator)!=1||creator!=pid||
       json_get_string_top_level(data,"start_time",recorded,sizeof recorded)!=1||strcmp(start,recorded)||
       json_get_string_top_level(data,"boot_id",recorded_boot,sizeof recorded_boot)!=1||strcmp(boot,recorded_boot))return 0;
    if(!process_only){long long reported_port=0;int connected=0;
        if(json_get_top_level_bool(data,(size_t)n,"connected",&connected)!=1||
           json_get_top_level_bool(data,(size_t)n,"ready",&ready)!=1||!ready||
           json_get_top_level_bool(data,(size_t)n,flag,&value)!=1||!value||
           json_get_int64_top_level(data,"port",&reported_port)!=1||reported_port!=(int)port||
           json_get_string_top_level(data,"listener_inode",inode,sizeof inode)!=1||!le_eh_listener(pid,port,inode))return 0;
    }
    return le_eh_start(pid,after)&&!strcmp(start,after)&&le_eh_executable(pid,daemon)&&le_eh_pid(pidfile)==pid;
}
static inline int le_esphome_health_default(const char *pidfile,const char *flag)
{
    return le_esphome_health(pidfile,le_eh_setting("LIBREECHO_ESPHOME_STATUS_FILE","/run/libreecho/esphome-status.json"),
        le_eh_setting("LIBREECHO_ESPHOMED_DAEMON","/usr/local/sbin/libreecho-esphomed"),le_eh_port(),flag,0);
}
#endif
