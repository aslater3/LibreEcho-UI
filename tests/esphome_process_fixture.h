/* Genuine, private ESPHome process for legacy environment-boundary fixtures. */
#ifndef LE_TEST_ESPHOME_PROCESS_H
#define LE_TEST_ESPHOME_PROCESS_H
#include <assert.h>
#include <arpa/inet.h>
#include <sys/socket.h>
static pid_t fixture_esphomed_pid=-1;
static char fixture_esphomed_root[512];
static void fixture_esphomed_start(void)
{
    const char *binary=getenv("ESPHOMED_BIN"),*scratch=getenv("TMPDIR");
    char pidpath[600],status[600],config[600],privacy[600],missing[600],portstr[16],pidstr[32];
    int fd;struct sockaddr_in address;socklen_t size=sizeof address;unsigned port;
    if(!binary||!*binary)binary="build/libreecho-esphomed";
    if(!scratch||!*scratch)scratch="build";
    assert(snprintf(fixture_esphomed_root,sizeof fixture_esphomed_root,"%s/esphome-env-XXXXXX",scratch)<(int)sizeof fixture_esphomed_root);
    assert(mkdtemp(fixture_esphomed_root));
    snprintf(pidpath,sizeof pidpath,"%s/pid",fixture_esphomed_root);snprintf(status,sizeof status,"%s/status.json",fixture_esphomed_root);
    snprintf(config,sizeof config,"%s/config.json",fixture_esphomed_root);snprintf(privacy,sizeof privacy,"%s/privacy",fixture_esphomed_root);snprintf(missing,sizeof missing,"%s/missing.sock",fixture_esphomed_root);
    assert(config_write_atomic(config,"{}",2)==0);assert(config_write_atomic(privacy,"0\n",2)==0);
    memset(&address,0,sizeof address);address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);assert(bind(fd,(struct sockaddr*)&address,sizeof address)==0);assert(getsockname(fd,(struct sockaddr*)&address,&size)==0);port=ntohs(address.sin_port);close(fd);snprintf(portstr,sizeof portstr,"%u",port);
    fixture_esphomed_pid=fork();assert(fixture_esphomed_pid>=0);
    if(fixture_esphomed_pid==0){
        execl(binary,binary,"--bind","127.0.0.1","--port",portstr,"--config",config,"--status-file",status,
              "--name","env-fixture","--mac","02:00:00:00:00:01","--privacy-state",privacy,
              "--wake-socket",missing,"--audio-socket",missing,"--radio-socket",missing,
              "--timer-socket",missing,"--led-socket",missing,"--mdns-socket",missing,
              "--audio-bus",missing,"--tls-ca",missing,"--idme-root",missing,(char*)NULL);
        _exit(127);
    }
    snprintf(pidstr,sizeof pidstr,"%ld\n",(long)fixture_esphomed_pid);assert(config_write_atomic(pidpath,pidstr,strlen(pidstr))==0);
    assert(setenv("LIBREECHO_ESPHOMED_PIDFILE",pidpath,1)==0);assert(setenv("LIBREECHO_ESPHOME_STATUS_FILE",status,1)==0);
    assert(setenv("LIBREECHO_ESPHOMED_DAEMON",binary,1)==0);assert(setenv("LIBREECHO_ESPHOME_PORT",portstr,1)==0);
    for(unsigned i=0;i<300;i++){struct timespec delay={0,10000000};if(esphome_satellite_ready())return;nanosleep(&delay,NULL);}
    kill(fixture_esphomed_pid,SIGTERM);waitpid(fixture_esphomed_pid,NULL,0);assert(!"actual ESPHome fixture did not become ready");
}
static void fixture_esphomed_stop(void)
{
    char path[600];/* config.json.bak/.tmp: the daemon persists its generated API key with config_write_atomic. */
    const char *files[]={"pid","status.json","config.json","config.json.bak","config.json.tmp","privacy"};
    if(fixture_esphomed_pid>1){kill(fixture_esphomed_pid,SIGTERM);assert(waitpid(fixture_esphomed_pid,NULL,0)==fixture_esphomed_pid);}
    for(unsigned i=0;i<sizeof files/sizeof files[0];i++){snprintf(path,sizeof path,"%s/%s",fixture_esphomed_root,files[i]);unlink(path);}
    assert(rmdir(fixture_esphomed_root)==0);
}
#endif
