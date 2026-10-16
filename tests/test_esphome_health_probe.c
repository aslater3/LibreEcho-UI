/* Actual API and watchdog predicates; no process/socket mocks. */
#define _DEFAULT_SOURCE 1
#define main watchdog_main
#include "../src/adapter/watchdogd.c"
#undef main
#define LE_INIT_MDNSD "/nonexistent/esphome-fixture-mdns.init"
#include "../src/api.c"
int main(int argc,char **argv)
{
    struct service_desc daemon={"esphomed",PROBE_PIDFILE,NULL,NULL,0,NULL};
    if(argc>1&&!strcmp(argv[1],"watchdog"))return watchdog_main(argc-1,argv+1);
    if(argc!=2)return 2;
    daemon.probe_path=getenv("LIBREECHO_ESPHOMED_PIDFILE");
    if(!strcmp(argv[1],"probe")){
        printf("%d %d %d\n",esphome_satellite_ready(),esphome_satellite_connected(),probe(&daemon));
        return 0;
    }
    if(!strcmp(argv[1],"restart"))return voice_pipeline_restart("home-assistant")==LE_OK?0:1;
    return 2;
}
