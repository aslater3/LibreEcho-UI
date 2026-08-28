#include "config_store.h"
#include "json.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);return 1;}}while(0)
int main(void){char b[256],s[32],too_long[512],nul[]="{\"channel\":\"dev\"\0}";int v;const char*p="/tmp/libreecho-config-unit.json",*j="{\"volume\":42,\"muted\":true}";CHECK(json_valid_object(j,strlen(j)));CHECK(!json_valid_object("{bad",4));CHECK(!json_valid_object("{\"channel\":\"dev\"}garbage",sizeof("{\"channel\":\"dev\"}garbage")-1));CHECK(!json_valid_object(nul,sizeof(nul)-1));CHECK(json_duplicate_key("{\"channel\":\"dev\",\"channel\":\"stable\"}",sizeof("{\"channel\":\"dev\",\"channel\":\"stable\"}")-1,"channel"));CHECK(!json_duplicate_key("{\"channel\":\"dev\",\"note\":\"channel\"}",sizeof("{\"channel\":\"dev\",\"note\":\"channel\"}")-1,"channel"));/* json_get_bool answers 1 when the key is present and parseable, 0 when it is
   absent, and -1 only when it is present but malformed. A caller applying a
   non-zero default must therefore test <=0, not <0; see the visualizer_enabled
   guard in backend_linux.c, which read back "off" on an ledd that omitted the
   field while the ring was in fact reacting. */
CHECK(json_get_bool("{\"on\":true}","on",&v)==1&&v==1);
CHECK(json_get_bool("{\"on\":false}","on",&v)==1&&v==0);
CHECK(json_get_bool("{\"other\":1}","on",&v)==0);
CHECK(json_get_bool("{\"on\":\"yes\"}","on",&v)==-1);
{const char*keys[]={"simulation","https","acoustic_events","usb_host"};size_t i;char nested[128],mixed[256];for(i=0;i<sizeof(keys)/sizeof(keys[0]);i++){snprintf(nested,sizeof(nested),"{\"wrapper\":{\"%s\":true}}",keys[i]);CHECK(json_get_top_level_bool(nested,strlen(nested),keys[i],&v)==0);snprintf(mixed,sizeof(mixed),"{\"wrapper\":{\"%s\":true},\"%s\":false}",keys[i],keys[i]);CHECK(json_get_top_level_bool(mixed,strlen(mixed),keys[i],&v)==1&&v==0);}}
CHECK(json_get_int("{\"volume\":42}","volume",&v)==1&&v==42);CHECK(json_get_string("{\"name\":\"LibreEcho\"}","name",s,sizeof(s))==1&&!strcmp(s,"LibreEcho"));CHECK(config_write_atomic(p,"{\"ok\":true}\n",12)==0);CHECK(config_read(p,b,sizeof(b))==12);CHECK(!strcmp(b,"{\"ok\":true}\n"));memset(too_long,'b',sizeof(too_long));memcpy(too_long,"/tmp/",5);too_long[451]=0;CHECK(config_write_atomic(too_long,"x",1)<0);{char escaped[64];json_escape(escaped,sizeof(escaped),"track\n.mp3");CHECK(!strcmp(escaped,"track\\n.mp3"));json_escape(escaped,sizeof(escaped),"quote\"slash\\");CHECK(!strcmp(escaped,"quote\\\"slash\\\\"));json_escape(escaped,sizeof(escaped),"a\nb");CHECK(!strcmp(escaped,"a\\nb"));json_escape(escaped,sizeof(escaped),"a\tb\rc");CHECK(!strcmp(escaped,"a\\tb\\rc"));json_escape(escaped,sizeof(escaped),"x\007y");CHECK(!strcmp(escaped,"x\\u0007y"));json_escape(escaped,sizeof(escaped),"l1\nl2\nl3");CHECK(!strcmp(escaped,"l1\\nl2\\nl3"));json_escape(escaped,10,"\n\n\n\n");CHECK(strlen(escaped)==8&&escaped[strlen(escaped)]==0);{char q[32];memset(q,34,31);q[31]=0;json_escape(escaped,sizeof(escaped),q);CHECK(strlen(escaped)==62);}}unlink("/tmp/libreecho-config-unit.json");unlink("/tmp/libreecho-config-unit.json.bak");puts("unit: ok");return 0;}
