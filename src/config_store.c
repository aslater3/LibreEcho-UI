#include "config_store.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int config_read(const char*p,char*b,size_t z){int fd;ssize_t n;if(!p||!b||z<2)return-1;fd=open(p,O_RDONLY);if(fd<0)return-1;n=read(fd,b,z-1);close(fd);if(n<0||(size_t)n>=z-1)return-1;b[n]=0;return(int)n;}
int config_write_atomic(const char*p,const char*b,size_t n){char tmp[512],bak[512];int fd;ssize_t w;if(!p||strlen(p)>450)return-1;snprintf(tmp,sizeof(tmp),"%s.tmp",p);snprintf(bak,sizeof(bak),"%s.bak",p);fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return-1;w=write(fd,b,n);if(w!=(ssize_t)n||fsync(fd)){close(fd);unlink(tmp);return-1;}if(close(fd)){unlink(tmp);return-1;}unlink(bak);link(p,bak);if(rename(tmp,p)){unlink(tmp);return-1;}chmod(p,0600);return 0;}
int config_copy_defaults(const char*from,const char*to){char b[16384];int n=config_read(from,b,sizeof(b));return n<0?-1:config_write_atomic(to,b,(size_t)n);}
