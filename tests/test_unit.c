#include "config_store.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
int main(void){char b[256],s[32],too_long[512];int v;const char*p="/tmp/libreecho-config-unit.json",*j="{\"volume\":42,\"muted\":true}";CHECK(json_valid_object(j,strlen(j)));CHECK(!json_valid_object("{bad",4));CHECK(json_get_int("{\"volume\":42}","volume",&v)==1&&v==42);CHECK(json_get_string("{\"name\":\"LibreEcho\"}","name",s,sizeof(s))==1&&!strcmp(s,"LibreEcho"));CHECK(config_write_atomic(p,"{\"ok\":true}\n",12)==0);CHECK(config_read(p,b,sizeof(b))==12);CHECK(!strcmp(b,"{\"ok\":true}\n"));memset(too_long,'b',sizeof(too_long));memcpy(too_long,"/tmp/",5);too_long[451]=0;CHECK(config_write_atomic(too_long,"x",1)<0);unlink("/tmp/libreecho-config-unit.json");unlink("/tmp/libreecho-config-unit.json.bak");puts("unit: ok");return 0;}
