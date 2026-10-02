#ifdef LE_HTTP_SEND_BOUNDS_TEST
#include "../src/http_server.c"
#include <assert.h>

int main(void)
{
    int pair[2],size=4096,flags,status,rc;
    long long start,deadline,elapsed;
    size_t chunks=0;
    pid_t reader;
    char data[8192]={0},reply[1024];
    signal(SIGPIPE,SIG_IGN);
    alarm(5);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    flags=fcntl(pair[0],F_GETFL);
    response(pair[0],200,"text/plain","bounded",7);
    rc=(int)recv(pair[1],reply,sizeof(reply)-1,0);
    assert(rc>0);reply[rc]=0;
    assert(strstr(reply,"HTTP/1.1 200 OK\r\n")&&strstr(reply,"\r\n\r\nbounded"));
    assert(fcntl(pair[0],F_GETFL)==flags);
    close(pair[0]);close(pair[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    assert(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0);
    flags=fcntl(pair[0],F_GETFL);
    start=send_now_ms();deadline=start+LE_RESPONSE_TIMEOUT_MS;
    do{rc=send_all(pair[0],data,sizeof(data),deadline);}while(!rc);
    elapsed=send_now_ms()-start;
    assert(elapsed>=LE_SEND_TIMEOUT_MS-20&&elapsed<LE_SEND_TIMEOUT_MS+500);
    assert(fcntl(pair[0],F_GETFL)==flags);
    close(pair[0]);close(pair[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
    assert(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0);
    reader=fork();assert(reader>=0);
    if(!reader){struct timespec pause={0,20000000L};close(pair[0]);while(recv(pair[1],data,4096,0)>0)nanosleep(&pause,NULL);close(pair[1]);_exit(0);}
    close(pair[1]);flags=fcntl(pair[0],F_GETFL);
    start=send_now_ms();deadline=start+LE_RESPONSE_TIMEOUT_MS;
    /* Every chunk makes progress, but none may reset the response deadline. */
    do{rc=send_all(pair[0],data,sizeof(data),deadline);if(!rc)chunks++;}while(!rc);
    elapsed=send_now_ms()-start;
    assert(chunks>10&&elapsed>=LE_RESPONSE_TIMEOUT_MS-20&&elapsed<LE_RESPONSE_TIMEOUT_MS+500);
    assert(fcntl(pair[0],F_GETFL)==flags);
    close(pair[0]);assert(waitpid(reader,&status,0)==reader&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    puts("http send bounds: normal response, unread timeout, trickle total deadline and socket flags: ok");
    return 0;
}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int connect_client(int port,int slow)
{
    struct sockaddr_in address;
    int fd=socket(AF_INET,SOCK_STREAM,0),size=1024;
    if(fd<0)return -1;
    if(slow&&setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof(size))<0){close(fd);return -1;}
    memset(&address,0,sizeof(address));
    address.sin_family=AF_INET;
    address.sin_port=htons((unsigned short)port);
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(connect(fd,(struct sockaddr*)&address,sizeof(address))<0){close(fd);return -1;}
    return fd;
}

int main(int argc,char**argv)
{
    const char*asset="GET /js/app.js HTTP/1.1\r\nHost: localhost\r\n\r\n";
    const char*status="GET /api/v1/status HTTP/1.1\r\nHost: localhost\r\n\r\n";
    struct timespec pause={0,100000000L},start,now;
    char reply[16384];
    size_t used=0;
    int slow,fd,port;
    if(argc<2||argc>3)return 2;
    port=atoi(argv[1]);
    if(argc==3){fd=connect_client(port,0);if(fd<0)return 1;close(fd);return 0;}
    slow=connect_client(port,1);
    if(slow<0||send(slow,asset,strlen(asset),0)!=(ssize_t)strlen(asset))return 2;
    /* Leave the tiny receive window unread while the daemon sends the asset. */
    nanosleep(&pause,NULL);
    fd=connect_client(port,0);
    if(fd<0||send(fd,status,strlen(status),0)!=(ssize_t)strlen(status))return 2;
    clock_gettime(CLOCK_MONOTONIC,&start);
    for(;;){
        struct pollfd p={fd,POLLIN,0};
        int remaining,ready;
        ssize_t n;
        clock_gettime(CLOCK_MONOTONIC,&now);
        remaining=3000-(int)((now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000);
        if(remaining<=0||(ready=poll(&p,1,remaining))==0){fprintf(stderr,"http send deadline: status stalled behind unread static response\n");close(fd);close(slow);return 1;}
        if(ready<0){if(errno==EINTR)continue;return 2;}
        n=recv(fd,reply+used,sizeof(reply)-1-used,0);
        if(n<0)return 2;
        if(!n)break;
        used+=(size_t)n;
        if(used==sizeof(reply)-1)return 2;
    }
    reply[used]=0;
    close(fd);close(slow);
    if(!strstr(reply,"HTTP/1.1 200 OK\r\n")||!strstr(reply,"\"ok\":true")){fprintf(stderr,"http send deadline: incomplete status response\n");return 1;}
    puts("http send deadline: concurrent status answered within 3 seconds");
    return 0;
}
#endif
