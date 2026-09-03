#include "backend_internal.h"
#include "adapter/timer_schedule.h"
#include "config_store.h"
#include "json.h"
#include "version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct mock_state { struct le_audio_state audio;struct le_led_state led;struct le_network_state net;struct le_wake_word_state wake;struct le_bluetooth_state bluetooth;struct le_airplay_state airplay;struct le_playback_state playback;struct le_wifi_scan scan;char device_name[64],config_path[384],pending_ssid[64],fail_next[32],power[24];unsigned rng;time_t started,connect_at,recover_at;int temp_base,update_progress,wake_available;struct le_timer_entry timers[16];long long timer_due[16];int timer_count;unsigned next_timer_id,timer_missed;};
#ifdef LE_MOCK_TESTING
static time_t mock_test_now;
static int mock_test_clock_enabled;

void le_mock_test_set_time(time_t now)
{
    mock_test_now = now;
    mock_test_clock_enabled = 1;
}

void le_mock_test_use_real_time(void)
{
    mock_test_clock_enabled = 0;
}
#endif

static long long mock_timer_now_ms(void)
{
    struct timespec ts;

#ifdef LE_MOCK_TESTING
    if (mock_test_clock_enabled)
        return (long long)mock_test_now * 1000LL;
#endif
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
static struct mock_state*M(struct le_backend*b){return(struct mock_state*)b->data;} static void mock_timers_tick(struct mock_state*); static unsigned rnd(struct mock_state*m){m->rng=m->rng*1103515245u+12345u;return(m->rng>>16)&0x7fff;}
static void mock_sync_led(struct mock_state*m){size_t i;for(i=0;i<LE_LED_PIXELS;i++){m->led.pixels[i].r=(uint8_t)((m->led.current.r*m->led.current.brightness+50)/100);m->led.pixels[i].g=(uint8_t)((m->led.current.g*m->led.current.brightness+50)/100);m->led.pixels[i].b=(uint8_t)((m->led.current.b*m->led.current.brightness+50)/100);}}
/* The API owns the canonical configuration document. Mock controls may update
 * runtime state, but must never replace that document with a partial snapshot. */
static int save(struct mock_state*m){(void)m;return 0;}
static void defaults(struct mock_state*m,unsigned seed){static const char*ssids[]={"LibreNet-5G","LibreNet-IoT","Neighbourhood WiFi","Open Test Network"};static const char*secs[]={"wpa2","wpa2","wpa2","open"};size_t i;memset(m,0,sizeof(*m));m->rng=seed?seed:0x4c454348;m->next_timer_id=1;m->started=time(0);m->temp_base=47;m->wake_available=1;strcpy(m->power,"online");strcpy(m->device_name,"Kitchen LibreEcho");strcpy(m->net.hostname,"libreecho-dev");strcpy(m->net.state,"connected");strcpy(m->net.connectivity,"healthy");strcpy(m->net.recovery_stage,"none");strcpy(m->net.ssid,"LibreNet-5G");strcpy(m->net.ip,"198.51.100.42");strcpy(m->net.gateway,"198.51.100.1");strcpy(m->net.dns,"198.51.100.1");m->net.signal=82;m->net.gateway_reachable=1;m->net.liveness_failures=0;m->net.internet=1;m->net.dhcp=1;m->scan.count=4;for(i=0;i<m->scan.count;i++){strcpy(m->scan.networks[i].ssid,ssids[i]);strcpy(m->scan.networks[i].security,secs[i]);m->scan.networks[i].signal=88-(int)i*17;}m->audio.volume=64;m->audio.microphone_gain=70;m->audio.notification_volume=55;m->audio.startup_sound=1;m->audio.amplifier_on=1;m->audio.output_available=1;m->audio.noise_remaining_seconds=-1;strcpy(m->audio.noise_colour,"white");strcpy(m->audio.tts_voice,"southern-female");m->led.current=(struct le_led_profile){72,216,118,72,50};m->led.boot=m->led.current;m->led.listening=(struct le_led_profile){72,185,255,80,50};m->led.thinking=(struct le_led_profile){168,115,239,80,70};m->led.error=(struct le_led_profile){239,80,80,90,40};m->led.dnd=(struct le_led_profile){190,35,35,45,20};m->led.night=(struct le_led_profile){255,40,0,12,50};m->led.night_enabled=0;m->led.night_start_minute=22*60;m->led.night_end_minute=7*60;m->led.visualizer_enabled=1;strcpy(m->wake.wake_word,"LibreEcho");strcpy(m->wake.model_status,"ready");m->wake.enabled=1;m->wake.sensitivity=68;m->wake.cooldown_ms=1500;m->wake.cpu_cost=8;m->wake.memory_cost_mb=34;m->bluetooth.available=1;m->bluetooth.classic=1;m->bluetooth.le=1;m->bluetooth.ssp=1;m->bluetooth.secure_connection=1;m->bluetooth.connectable=1;m->bluetooth.bondable=1;strcpy(m->bluetooth.state,"off");strcpy(m->bluetooth.transport,"mock");strcpy(m->bluetooth.hci,"none");strcpy(m->bluetooth.local_name,"Kitchen LibreEcho");strcpy(m->bluetooth.profile_state,"pairing-only");strcpy(m->bluetooth.profile_error,"No userspace Bluetooth profile service is registered");m->bluetooth.discovered_count=2;strcpy(m->bluetooth.discovered[0].address,"10:20:30:40:50:60");strcpy(m->bluetooth.discovered[0].name,"Mock Speaker");m->bluetooth.discovered[0].type=0;m->bluetooth.discovered[0].rssi=-42;strcpy(m->bluetooth.discovered[1].address,"20:30:40:50:60:70");strcpy(m->bluetooth.discovered[1].name,"Mock Headphones");m->bluetooth.discovered[1].type=0;m->bluetooth.discovered[1].rssi=-58;strcpy(m->playback.state,"playing");strcpy(m->playback.source,"airplay2");m->playback.media_active=1;m->playback.metadata_available=1;strcpy(m->playback.title,"Open Source Radio");strcpy(m->playback.artist,"LibreEcho");strcpy(m->playback.album,"Development Sessions");}
static void load_profile(struct mock_state*m,const char*p,unsigned cli_seed){char j[16384],*at;int v;size_t count=0;if(!p||config_read(p,j,sizeof(j))<0||!json_valid_object(j,strlen(j)))return;if(json_get_int(j,"temperature_c",&v)>0&&v>=0&&v<=120)m->temp_base=v;if(!cli_seed&&json_get_int(j,"seed",&v)>0)m->rng=(unsigned)v;at=j;while(count<LE_MAX_WIFI&&(at=strstr(at,"\"ssid\""))){char object[512],*end=strchr(at,'}');size_t n;if(!end)break;n=(size_t)(end-at+1);if(n>=sizeof(object)){at=end+1;continue;}memcpy(object,at,n);object[n]=0;if(json_get_string(object,"ssid",m->scan.networks[count].ssid,sizeof(m->scan.networks[count].ssid))>0){if(json_get_string(object,"security",m->scan.networks[count].security,sizeof(m->scan.networks[count].security))<1)strcpy(m->scan.networks[count].security,"wpa2");if(json_get_int(object,"signal",&v)<1)v=60;m->scan.networks[count].signal=v<0?0:v>100?100:v;count++;}at=end+1;}if(count)m->scan.count=count;}
static void load(struct mock_state*m,const char*p){char j[16384],s[128];int v;if(!p||config_read(p,j,sizeof(j))<0||!json_valid_object(j,strlen(j)))return;if(json_get_string(j,"device_name",s,sizeof(s))>0)strcpy(m->device_name,s);if(json_get_string(j,"hostname",s,sizeof(s))>0)strcpy(m->net.hostname,s);if(json_get_int(j,"volume",&v)>0&&v>=0&&v<=100)m->audio.volume=v;if(json_get_int(j,"microphone_gain",&v)>0&&v>=0&&v<=100)m->audio.microphone_gain=v;if(json_get_bool(j,"microphone_muted",&v)>0)m->audio.muted=v;if(json_get_int(j,"led_r",&v)>0&&v>=0&&v<=255)m->led.current.r=(uint8_t)v;if(json_get_int(j,"led_g",&v)>0&&v>=0&&v<=255)m->led.current.g=(uint8_t)v;if(json_get_int(j,"led_b",&v)>0&&v>=0&&v<=255)m->led.current.b=(uint8_t)v;if(json_get_int(j,"led_brightness",&v)>0&&v>=0&&v<=100)m->led.current.brightness=v;if(json_get_bool(j,"led_visualizer_enabled",&v)>0)m->led.visualizer_enabled=v;if(json_get_string(j,"wake_word",s,sizeof(s))>0)strcpy(m->wake.wake_word,s);if(json_get_bool(j,"wake_enabled",&v)>0)m->wake.enabled=v;if(json_get_int(j,"wake_sensitivity",&v)>0&&v>=0&&v<=100)m->wake.sensitivity=v;}
static int fail(struct mock_state*m,const char*op){if(!strcmp(m->fail_next,op)){m->fail_next[0]=0;return 1;}return 0;}
static void mock_cpu_status(struct mock_state*m,struct le_system_status*o){size_t i;o->cpu_count=4;for(i=0;i<o->cpu_count;i++){o->cpus[i].online=1;o->cpus[i].utilization=12+(int)(rnd(m)%36);o->cpus[i].frequency_khz=1200000+(int)(rnd(m)%4)*100000;}}
static void destroy(struct le_backend*b){save(M(b));free(b->data);}static int status(struct le_backend*b,struct le_system_status*o){struct mock_state*m=M(b);int drift=(int)(rnd(m)%7)-3;memset(o,0,sizeof(*o));o->uptime=difftime(time(0),m->started);o->cpu=14+(int)(rnd(m)%18);o->memory=44+(int)(rnd(m)%5);o->storage=19;o->storage_available=1;o->temperature=m->temp_base+drift;o->memory_total_mb=512;o->memory_used_mb=o->memory_total_mb*o->memory/100;o->storage_total_mb=4096;o->storage_used_mb=o->storage_total_mb*o->storage/100;strcpy(o->storage_state,"filesystem");strcpy(o->device_state,m->power);mock_cpu_status(m,o);return LE_OK;}
static int device(struct le_backend*b,struct le_device_info*o){struct mock_state*m=M(b);memset(o,0,sizeof(*o));strcpy(o->name,m->device_name);strcpy(o->hostname,m->net.hostname);strcpy(o->model,"Amazon Echo (2nd generation)");strcpy(o->serial,"DEV-MOCK-4C454348");snprintf(o->os_version,sizeof(o->os_version),"%s",LE_OS_VERSION_STRING);strcpy(o->kernel,"3.18-compatible mock");strcpy(o->hardware_revision,"MT8163 development profile");strcpy(o->backend,"mock");return LE_OK;}
static int audio(struct le_backend*b,struct le_audio_state*o){*o=M(b)->audio;return LE_OK;}static int volume(struct le_backend*b,int v){if(v<0||v>100)return LE_INVALID;if(fail(M(b),"audio"))return LE_IO;M(b)->audio.volume=v;return save(M(b));}static int gain(struct le_backend*b,int v){if(v<0||v>100)return LE_INVALID;M(b)->audio.microphone_gain=v;return save(M(b));}static int mute(struct le_backend*b,int v){M(b)->audio.muted=!!v;return save(M(b));}static int tone(struct le_backend*b){return fail(M(b),"audio-test")?LE_IO:LE_OK;}static int tts_voice(struct le_backend*b,const char*v){if(strcmp(v,"northern-male")&&strcmp(v,"southern-female"))return LE_INVALID;strcpy(M(b)->audio.tts_voice,v);return save(M(b));}
static int led(struct le_backend*b,struct le_led_state*o){mock_sync_led(M(b));*o=M(b)->led;if(!o->visualizer_mood[0])strcpy(o->visualizer_mood,o->visualizer_active?"balanced":"idle");return LE_OK;}static int colour(struct le_backend*b,uint8_t r,uint8_t g,uint8_t bl){if(fail(M(b),"led"))return LE_IO;M(b)->led.current.r=r;M(b)->led.current.g=g;M(b)->led.current.b=bl;return save(M(b));}static int brightness(struct le_backend*b,int v){if(v<0||v>100)return LE_INVALID;M(b)->led.current.brightness=v;return save(M(b));}static int visualizer_enabled(struct le_backend*b,int v){if(v!=0&&v!=1)return LE_INVALID;M(b)->led.visualizer_enabled=v;if(!v){M(b)->led.visualizer_active=0;M(b)->led.visualizer_owner[0]=0;strcpy(M(b)->led.visualizer_mood,"idle");memset(M(b)->led.visualizer_levels,0,sizeof(M(b)->led.visualizer_levels));}return save(M(b));}static int boot_led(struct le_backend*b,const struct le_led_profile*p){M(b)->led.boot=*p;return save(M(b));}static int led_profile(struct le_backend*b,const char*n,const struct le_led_profile*p){if(!n||!p||p->brightness<0||p->brightness>100)return LE_INVALID;if(!strcmp(n,"listening"))M(b)->led.listening=*p;else if(!strcmp(n,"thinking"))M(b)->led.thinking=*p;else if(!strcmp(n,"error"))M(b)->led.error=*p;else if(!strcmp(n,"dnd"))M(b)->led.dnd=*p;else if(!strcmp(n,"night"))M(b)->led.night=*p;else return LE_INVALID;return save(M(b));}static int led_test(struct le_backend*b){return fail(M(b),"led-test")?LE_IO:LE_OK;}
static int network(struct le_backend*b,struct le_network_state*o){*o=M(b)->net;return LE_OK;}static void mock_wifi_details(struct le_wifi_network *network, size_t index)
{
    static const int frequencies[] = {5180, 2412, 2462, 2437};
    if (!network)
        return;
    network->frequency_mhz = frequencies[index % 4];
    network->channel = network->frequency_mhz >= 5000 ?
                       (network->frequency_mhz - 5000) / 5 :
                       (network->frequency_mhz - 2407) / 5;
    snprintf(network->band, sizeof(network->band), "%s",
             network->frequency_mhz >= 5000 ? "5 GHz" : "2.4 GHz");
    network->rssi_dbm = -35 - (int)index * 12;
    if (!strcmp(network->security, "wpa3-transition")) {
        snprintf(network->capabilities, sizeof(network->capabilities),
                 "WPA2-PSK, WPA3-SAE");
        network->wpa2_attempt = 1;
    } else if (!strcmp(network->security, "wpa3-only")) {
        snprintf(network->capabilities, sizeof(network->capabilities),
                 "WPA3-SAE");
        network->wpa2_attempt = 0;
    } else if (!strcmp(network->security, "wpa2")) {
        snprintf(network->capabilities, sizeof(network->capabilities),
                 "WPA2-PSK");
        network->wpa2_attempt = 1;
    } else {
        snprintf(network->capabilities, sizeof(network->capabilities), "open");
        network->wpa2_attempt = 0;
    }
}
static int scan(struct le_backend*b,struct le_wifi_scan*o){size_t i;if(fail(M(b),"wifi-scan"))return LE_IO;*o=M(b)->scan;for(i=0;i<o->count;i++){mock_wifi_details(&o->networks[i],i);o->networks[i].signal+=(int)(rnd(M(b))%7)-3;if(o->networks[i].signal<0)o->networks[i].signal=0;if(o->networks[i].signal>100)o->networks[i].signal=100;}return LE_OK;}
static int connect_wifi(struct le_backend*b,const struct le_wifi_credentials*c){struct mock_state*m=M(b);if(!c->ssid[0])return LE_INVALID;if(fail(m,"wifi-connect")||!strcmp(c->ssid,"FailNet")){strcpy(m->net.state,"failed");strcpy(m->net.connectivity,"disconnected");m->net.gateway_reachable=-1;m->recover_at=time(0)+4;return LE_IO;}strcpy(m->net.state,"connecting");strcpy(m->net.connectivity,"unknown");m->net.gateway_reachable=-1;memcpy(m->pending_ssid,c->ssid,sizeof(m->pending_ssid));m->pending_ssid[sizeof(m->pending_ssid)-1]=0;m->connect_at=time(0)+2;m->net.internet=0;return LE_OK;}static int disconnect_wifi(struct le_backend*b){struct mock_state*m=M(b);strcpy(m->net.state,"disconnected");strcpy(m->net.connectivity,"disconnected");m->net.gateway_reachable=-1;m->net.ssid[0]=m->net.ip[0]=0;m->net.signal=m->net.internet=0;return save(m);}static int hostname(struct le_backend*b,const char*s){size_t i,n=strlen(s);if(n<1||n>63)return LE_INVALID;for(i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'))return LE_INVALID;strcpy(M(b)->net.hostname,s);return save(M(b));}
static int wake(struct le_backend*b,struct le_wake_word_state*o){if(!M(b)->wake_available)return LE_NOT_SUPPORTED;*o=M(b)->wake;return LE_OK;}static int wake_set(struct le_backend*b,const char*s){if(!M(b)->wake_available)return LE_NOT_SUPPORTED;if(!s[0]||strlen(s)>=LE_TEXT)return LE_INVALID;strcpy(M(b)->wake.wake_word,s);return save(M(b));}static int sensitivity(struct le_backend*b,int v){if(!M(b)->wake_available)return LE_NOT_SUPPORTED;if(v<0||v>100)return LE_INVALID;M(b)->wake.sensitivity=v;return save(M(b));}static int wake_test(struct le_backend*b){if(!M(b)->wake_available)return LE_NOT_SUPPORTED;if(!M(b)->wake.enabled)return LE_BUSY;M(b)->wake.detected_count++;return LE_OK;}
static int bluetooth(struct le_backend*b,struct le_bluetooth_state*o){*o=M(b)->bluetooth;return LE_OK;}static int bluetooth_set(struct le_backend*b,int enabled){if(enabled!=0&&enabled!=1)return LE_INVALID;M(b)->bluetooth.activation_attempted=1;M(b)->bluetooth.enabled=enabled;strcpy(M(b)->bluetooth.state,enabled?"up":"off");strcpy(M(b)->bluetooth.hci,enabled?"hci0":"none");return LE_OK;}
static struct le_bluetooth_device*mock_bt_find(struct mock_state*m,const char*a,int type){size_t i;for(i=0;i<m->bluetooth.discovered_count;i++)if(!strcmp(m->bluetooth.discovered[i].address,a)&&m->bluetooth.discovered[i].type==type)return&m->bluetooth.discovered[i];for(i=0;i<m->bluetooth.known_count;i++)if(!strcmp(m->bluetooth.known[i].address,a)&&m->bluetooth.known[i].type==type)return&m->bluetooth.known[i];return NULL;}
static int bluetooth_scan_mock(struct le_backend*b,int start){if(!M(b)->bluetooth.enabled)return LE_BUSY;M(b)->bluetooth.scanning=!!start;return LE_OK;}
static int bluetooth_pair_mock(struct le_backend*b,const char*a,int type,int io){struct mock_state*m=M(b);struct le_bluetooth_device*d;size_t i;if(io<0||io>4)return LE_INVALID;d=mock_bt_find(m,a,type);if(!d)return LE_INVALID;for(i=0;i<m->bluetooth.known_count;i++)if(!strcmp(m->bluetooth.known[i].address,a)&&m->bluetooth.known[i].type==type)break;if(i<m->bluetooth.known_count){m->bluetooth.known[i].paired=1;m->bluetooth.known[i].connected=1;}else if(m->bluetooth.known_count<LE_MAX_BLUETOOTH_DEVICES){m->bluetooth.known[m->bluetooth.known_count]=*d;m->bluetooth.known[m->bluetooth.known_count].paired=1;m->bluetooth.known[m->bluetooth.known_count].connected=1;m->bluetooth.known_count++;}m->bluetooth.pairing=0;return LE_OK;}
static int bluetooth_unpair_mock(struct le_backend*b,const char*a,int type){struct mock_state*m=M(b);size_t i;for(i=0;i<m->bluetooth.known_count;i++)if(!strcmp(m->bluetooth.known[i].address,a)&&m->bluetooth.known[i].type==type){memmove(&m->bluetooth.known[i],&m->bluetooth.known[i+1],(m->bluetooth.known_count-i-1)*sizeof(m->bluetooth.known[0]));m->bluetooth.known_count--;return LE_OK;}return LE_INVALID;}
static int bluetooth_disconnect_mock(struct le_backend*b,const char*a,int type){struct le_bluetooth_device*d=mock_bt_find(M(b),a,type);if(!d)return LE_INVALID;d->connected=0;return LE_OK;}
static int bluetooth_pairing_response_mock(struct le_backend*b,const char*a,int type,const char*m,unsigned int v,const char*p){(void)a;(void)type;(void)m;(void)v;(void)p;M(b)->bluetooth.pairing=0;return LE_OK;}
static int bluetooth_discoverable_mock(struct le_backend*b,int enabled){if(enabled!=0&&enabled!=1)return LE_INVALID;M(b)->bluetooth.discoverable=enabled;return LE_OK;}
static int bluetooth_connectable_mock(struct le_backend*b,int enabled){if(enabled!=0&&enabled!=1)return LE_INVALID;M(b)->bluetooth.connectable=enabled;return LE_OK;}
static int bluetooth_pairing_mode_mock(struct le_backend*b,int enabled){if(enabled!=0&&enabled!=1)return LE_INVALID;M(b)->bluetooth.pairing_mode=enabled;M(b)->bluetooth.connectable=enabled;M(b)->bluetooth.discoverable=enabled;M(b)->bluetooth.bondable=enabled;return LE_OK;}
static int airplay(struct le_backend*b,struct le_airplay_state*o){*o=M(b)->airplay;o->available=1;return LE_OK;}static int airplay_set(struct le_backend*b,int enabled){if(enabled!=0&&enabled!=1)return LE_INVALID;if(fail(M(b),"airplay"))return LE_IO;M(b)->airplay.enabled=enabled;M(b)->airplay.nqptp_running=enabled;M(b)->airplay.shairport_running=enabled;return LE_OK;}
static int playback(struct le_backend*b,struct le_playback_state*o){*o=M(b)->playback;return LE_OK;}
static int reboot(struct le_backend*b){struct mock_state*m=M(b);strcpy(m->power,"rebooting");m->recover_at=time(0)+3;return LE_OK;}static int shutdown(struct le_backend*b){strcpy(M(b)->power,"shutting_down");return LE_OK;}static int reset(struct le_backend*b){char p[384];unsigned seed=M(b)->rng;strcpy(p,M(b)->config_path);defaults(M(b),seed);strcpy(M(b)->config_path,p);return save(M(b));}
static int tick(struct le_backend*b){struct mock_state*m=M(b);time_t now=time(0);mock_timers_tick(m);if(m->connect_at&&now>=m->connect_at){m->connect_at=0;strcpy(m->net.state,"connected");strcpy(m->net.connectivity,"healthy");m->net.gateway_reachable=1;strcpy(m->net.ssid,m->pending_ssid);strcpy(m->net.ip,"198.51.100.42");m->net.signal=76;m->net.internet=1;save(m);}if(m->recover_at&&now>=m->recover_at){m->recover_at=0;if(!strcmp(m->power,"rebooting")){strcpy(m->power,"online");m->started=now;}if(!strcmp(m->net.state,"failed")){strcpy(m->net.state,"disconnected");strcpy(m->net.connectivity,"disconnected");m->net.gateway_reachable=-1;}}return LE_OK;}
static int control(struct le_backend*b,const char*a,const char*v){struct mock_state*m=M(b);if(!strcmp(a,"set-temperature")){int n=atoi(v);if(n<0||n>120)return LE_INVALID;m->temp_base=n;}else if(!strcmp(a,"set-wifi")){if(strcmp(v,"connected")&&strcmp(v,"disconnected")&&strcmp(v,"failed"))return LE_INVALID;strcpy(m->net.state,v);strcpy(m->net.connectivity,!strcmp(v,"connected")?"healthy":"disconnected");m->net.gateway_reachable=!strcmp(v,"connected")?1:-1;m->net.internet=!strcmp(v,"connected");}else if(!strcmp(a,"set-wake-available")){if(strcmp(v,"true")&&strcmp(v,"false"))return LE_INVALID;m->wake_available=!strcmp(v,"true");}else if(!strcmp(a,"fail-next")){if(strlen(v)>=sizeof(m->fail_next))return LE_INVALID;strcpy(m->fail_next,v);}else if(!strcmp(a,"trigger")&&!strcmp(v,"wake-word")){m->wake.detected_count++;}else if(!strcmp(a,"set-update-progress")){int n=atoi(v);if(n<0||n>100)return LE_INVALID;m->update_progress=n;}else if(!strcmp(a,"reset"))return reset(b);else return LE_INVALID;return save(m);}
static int night_mock(struct le_backend *b, int enabled, int start, int end)
{
    struct mock_state *m = b->data;

    if (start < 0 || start > 1439 || end < 0 || end > 1439)
        return LE_INVALID;
    m->led.night_enabled = enabled ? 1 : 0;
    m->led.night_start_minute = start;
    m->led.night_end_minute = end;
    return LE_OK;
}

static int noise_start_mock(struct le_backend*b,const char*colour,int level,int minutes){
 if(!colour||(strcmp(colour,"white")&&strcmp(colour,"pink")&&strcmp(colour,"brown")))return LE_INVALID;
 if(level<1||level>100||minutes<0||minutes>600)return LE_INVALID;
 M(b)->audio.noise_active=1;M(b)->audio.noise_level=level;
 strcpy(M(b)->audio.noise_colour,colour);
 M(b)->audio.noise_remaining_seconds=minutes?(long)minutes*60:-1;
 return LE_OK;}
static int noise_stop_mock(struct le_backend*b){
 M(b)->audio.noise_active=0;M(b)->audio.noise_level=0;
 M(b)->audio.noise_remaining_seconds=-1;strcpy(M(b)->audio.noise_colour,"white");
 return LE_OK;}
static int simulate_audio_mock(struct le_backend*b,const char*text){
 (void)b; if(!text||!text[0])return LE_INVALID; return LE_OK;}
static void mock_timer_remove(struct mock_state*m,int i){memmove(&m->timers[i],&m->timers[i+1],(size_t)(m->timer_count-i-1)*sizeof(m->timers[0]));memmove(&m->timer_due[i],&m->timer_due[i+1],(size_t)(m->timer_count-i-1)*sizeof(m->timer_due[0]));--m->timer_count;}
static void mock_timers_tick(struct mock_state*m){long long now=mock_timer_now_ms();int i=0;while(i<m->timer_count){long long remaining;if(!strcmp(m->timers[i].state,"ringing")){if(now>=m->timer_due[i]){mock_timer_remove(m,i);continue;}++i;continue;}if(strcmp(m->timers[i].state,"pending")){++i;continue;}if(now>=m->timer_due[i]){if(now-m->timer_due[i]>LE_TIMER_MISS_GRACE_SECONDS*1000LL){mock_timer_remove(m,i);++m->timer_missed;continue;}strcpy(m->timers[i].state,"ringing");m->timers[i].seconds_remaining=0;m->timer_due[i]=now+LE_TIMER_RING_SECONDS*1000LL;}else{remaining=m->timer_due[i]-now;m->timers[i].seconds_remaining=(long)(remaining/1000LL);}++i;}}
static int timers_mock(struct le_backend*b,struct le_timer_list*o){size_t i;mock_timers_tick(M(b));memset(o,0,sizeof(*o));o->available=1;o->missed=(int)M(b)->timer_missed;for(i=0;i<(size_t)M(b)->timer_count&&i<sizeof(o->items)/sizeof(o->items[0]);i++){o->items[o->count++]=M(b)->timers[i];if(!strcmp(M(b)->timers[i].state,"ringing"))o->ringing++;}return LE_OK;}
static int timer_add_mock(struct le_backend*b,int seconds,const char*label,unsigned*id){struct mock_state*m=M(b);struct le_timer_entry*e;if(seconds<1||seconds>604800)return LE_INVALID;if(m->timer_count>=(int)(sizeof(m->timers)/sizeof(m->timers[0])))return LE_BUSY;e=&m->timers[m->timer_count++];memset(e,0,sizeof(*e));e->id=m->next_timer_id++;strcpy(e->kind,"countdown");strcpy(e->state,"pending");e->seconds_remaining=seconds;m->timer_due[m->timer_count-1]=mock_timer_now_ms()+seconds*1000LL;if(label)snprintf(e->label,sizeof(e->label),"%s",label);if(id)*id=e->id;return LE_OK;}
static int timer_cancel_mock(struct le_backend*b,unsigned id){struct mock_state*m=M(b);int i;mock_timers_tick(m);for(i=0;i<m->timer_count;i++){if(m->timers[i].id!=id)continue;if(strcmp(m->timers[i].state,"pending"))return LE_INVALID;memmove(&m->timers[i],&m->timers[i+1],(size_t)(m->timer_count-i-1)*sizeof(m->timers[0]));memmove(&m->timer_due[i],&m->timer_due[i+1],(size_t)(m->timer_count-i-1)*sizeof(m->timer_due[0]));--m->timer_count;return LE_OK;}return LE_INVALID;}
static int timer_dismiss_mock(struct le_backend*b,int*stopped){struct mock_state*m=M(b);int i=0,n=0;while(i<m->timer_count){if(!strcmp(m->timers[i].state,"ringing")){memmove(&m->timers[i],&m->timers[i+1],(size_t)(m->timer_count-i-1)*sizeof(m->timers[0]));memmove(&m->timer_due[i],&m->timer_due[i+1],(size_t)(m->timer_count-i-1)*sizeof(m->timer_due[0]));--m->timer_count;++n;continue;}++i;}if(stopped)*stopped=n;return LE_OK;}
static const struct le_backend_ops ops={destroy,status,device,audio,volume,gain,mute,tone,tts_voice,0,0,noise_start_mock,noise_stop_mock,simulate_audio_mock,led,colour,brightness,visualizer_enabled,boot_led,led_profile,night_mock,led_test,network,scan,connect_wifi,disconnect_wifi,hostname,wake,wake_set,sensitivity,wake_test,bluetooth,bluetooth_set,bluetooth_scan_mock,bluetooth_pair_mock,bluetooth_unpair_mock,bluetooth_disconnect_mock,bluetooth_pairing_response_mock,bluetooth_discoverable_mock,bluetooth_connectable_mock,bluetooth_pairing_mode_mock,airplay,airplay_set,playback,reboot,shutdown,reset,tick,control,timers_mock,timer_add_mock,timer_cancel_mock,timer_dismiss_mock};
int le_mock_create(struct le_backend*b,const char*mock,const char*cfg,unsigned seed){struct mock_state*m=calloc(1,sizeof(*m));if(!m)return LE_IO;defaults(m,seed);load_profile(m,mock,seed);if(cfg){strncpy(m->config_path,cfg,sizeof(m->config_path)-1);load(m,cfg);}b->data=m;b->ops=&ops;return LE_OK;}
