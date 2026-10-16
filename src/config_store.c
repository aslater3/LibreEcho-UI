#include "config_store.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
int config_read(const char*p,char*b,size_t z){int fd;ssize_t n;if(!p||!b||z<2)return-1;fd=open(p,O_RDONLY);if(fd<0)return-1;n=read(fd,b,z-1);close(fd);if(n<0||(size_t)n>=z-1)return-1;b[n]=0;return(int)n;}
int config_write_atomic(const char*p,const char*b,size_t n){char tmp[512],bak[512];int fd;ssize_t w;if(!p||strlen(p)>450)return-1;snprintf(tmp,sizeof(tmp),"%s.tmp",p);snprintf(bak,sizeof(bak),"%s.bak",p);fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return-1;if(fchmod(fd,0600)){close(fd);unlink(tmp);return-1;}w=write(fd,b,n);if(w!=(ssize_t)n||fsync(fd)){close(fd);unlink(tmp);return-1;}if(close(fd)){unlink(tmp);return-1;}unlink(bak);if(chmod(p,0600)<0&&errno!=ENOENT){unlink(tmp);return-1;}if(link(p,bak)<0&&errno!=ENOENT){/* backup is best effort */}if(rename(tmp,p)){unlink(tmp);return-1;}chmod(p,0600);return 0;}
int config_copy_defaults(const char*from,const char*to){char b[16384];int n=config_read(from,b,sizeof(b));return n<0?-1:config_write_atomic(to,b,(size_t)n);}
/* Canonical 32-byte PSK encoding: 43 alphabet characters and exactly one '='.
 * The low two pad bits must be zero; accept empty only as unprovisioned. */
int config_esphome_key_valid(const char *key)
{
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;const char *last;
    if(!key)return 0;
    if(!key[0])return 1;
    if(strlen(key)!=44||key[43]!='=')return 0;
    for(i=0;i<43;i++)if(!strchr(alphabet,key[i]))return 0;
    last=strchr(alphabet,key[42]);return last&&((last-alphabet)&3)==0;
}
int config_esphome_read(const char *path, struct le_esphome_config *config)
{
    char data[16384];int n,field;
    if(!config)return -1;
    memset(config,0,sizeof(*config));snprintf(config->ha_protocol,sizeof(config->ha_protocol),"esphome");
    if(!path||!path[0])return 0;
    /* config_read may reject oversized input without an errno of its own.
     * Never mistake stale ENOENT for an absent key-bearing configuration. */
    errno=0;
    n=config_read(path,data,sizeof(data));
    if(n<0)return errno==ENOENT?0:-1;
    if(!json_valid_object(data,(size_t)n)||json_duplicate_key(data,(size_t)n,"esphome_noise_key"))return -1;
    field=json_get_string_top_level(data,"esphome_noise_key",config->esphome_noise_key,sizeof(config->esphome_noise_key));
    if(field<0||!config_esphome_key_valid(config->esphome_noise_key)){memset(config->esphome_noise_key,0,sizeof(config->esphome_noise_key));return -1;}
    if(json_duplicate_key(data,(size_t)n,"esphome_active_wake_word"))return -1;
    field=json_get_string_top_level(data,"esphome_active_wake_word",config->esphome_active_wake_word,sizeof(config->esphome_active_wake_word));
    if(field<0||(field>0&&config->esphome_active_wake_word[0]&&strcmp(config->esphome_active_wake_word,"alexa_v0.1")))return -1;
    config->esphome_active_wake_word_present=field>0;
    return 0;
}
