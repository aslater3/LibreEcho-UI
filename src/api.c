#include "version.h"
#include "api.h"
#include "adapter/adapter.h"
#include "adapter/timer_schedule.h"
#include "adapter/wyoming_client.h"
#include "config_store.h"
#include "json.h"
#include "logd.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
static void out(struct api_response*r,int status,const char*fmt,...){va_list ap;r->status=status;strcpy(r->type,"application/json; charset=utf-8");va_start(ap,fmt);r->length=(size_t)vsnprintf(r->body,sizeof(r->body),fmt,ap);va_end(ap);if(r->length>=sizeof(r->body))r->length=sizeof(r->body)-1;}
static void ok(struct api_response*r,const char*data){out(r,200,"{\"ok\":true,\"data\":%s,\"error\":null}",data?data:"{}");}static void err(struct api_response*r,int status,int rc,const char*msg){char e[256];json_escape(e,sizeof(e),msg);out(r,status,"{\"ok\":false,\"data\":null,\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",le_result_code(rc),e);}static void method_not_allowed(struct api_response*r){err(r,405,LE_INVALID,"HTTP method is not allowed for this endpoint");}
static int api_bootstrap_required_internal(const struct api_context*c){return c&&c->users_path[0]&&!c->auth.enabled&&!c->auth_token[0];}
int api_bootstrap_required(const struct api_context*c){return api_bootstrap_required_internal(c);}
static int changing(const char*m){return strcmp(m,"GET")&&strcmp(m,"HEAD");}static int constant_equal(const char*a,const char*b){size_t al=strlen(a),bl=strlen(b),i,n=al>bl?al:bl;unsigned diff=(unsigned)(al^bl);for(i=0;i<n;i++){unsigned ac=i<al?(unsigned char)a[i]:0,bc=i<bl?(unsigned char)b[i]:0;diff|=ac^bc;}return diff==0;}static int allowed_origin(const struct api_context*c,const struct api_request*q){const char*origin=q->origin;const char*scheme="http://";if(!origin[0]||c->allow_insecure_lan)return 1;if(c->allowed_origin[0])return constant_equal(origin,c->allowed_origin);if(q->host[0]&&!strncmp(origin,scheme,strlen(scheme))&&constant_equal(origin+strlen(scheme),q->host))return 1;return !strncmp(origin,"http://127.0.0.1",16)||!strncmp(origin,"http://localhost",16);}static int security(const struct api_context*c,const struct api_request*q,struct api_response*r){int authorized=0,bootstrap_path=!strcmp(q->path,"/api/v1/auth/bootstrap")&&!strcmp(q->method,"POST"),login_path=!strcmp(q->path,"/api/v1/auth/login")&&!strcmp(q->method,"POST");if(!strcmp(q->path,"/api/v1/config")||bootstrap_path||login_path)authorized=1;if(!authorized&&c->auth_token[0]){char expected[256];snprintf(expected,sizeof(expected),"Bearer %s",c->auth_token);authorized=constant_equal(q->authorization,expected);}if(!authorized&&c->auth.enabled&&!strncmp(q->authorization,"Bearer ",7))authorized=le_auth_session((struct le_auth_db*)&c->auth,q->authorization+7,0,0);if(api_bootstrap_required_internal(c)&&!authorized){err(r,401,LE_AUTH,"Initial account setup is required");return 0;}if((c->auth_token[0]||c->auth.enabled)&&!authorized){err(r,401,LE_AUTH,"Authentication is required");return 0;}if(!changing(q->method))return 1;if(!constant_equal(q->csrf,c->csrf_token)){err(r,403,LE_AUTH,"Missing or invalid CSRF token");return 0;}if(!allowed_origin(c,q)){err(r,403,LE_AUTH,"Origin is not allowed");return 0;}return 1;}
static int body_ok(const struct api_request*q,struct api_response*r){if(!json_valid_object(q->body,q->body_len)){err(r,400,LE_INVALID,"Request body must be a valid JSON object");return 0;}return 1;}
static int valid_utf8(const char*s,size_t n){const unsigned char*p=(const unsigned char*)s;size_t i=0;while(i<n){unsigned char c=p[i];if(c<=0x7f){i++;continue;}if(c>=0xc2&&c<=0xdf){if(i+1>=n||(p[i+1]&0xc0)!=0x80)return 0;i+=2;continue;}if(c==0xe0){if(i+2>=n||p[i+1]<0xa0||p[i+1]>0xbf||(p[i+2]&0xc0)!=0x80)return 0;i+=3;continue;}if((c>=0xe1&&c<=0xec)||(c>=0xee&&c<=0xef)){if(i+2>=n||(p[i+1]&0xc0)!=0x80||(p[i+2]&0xc0)!=0x80)return 0;i+=3;continue;}if(c==0xed){if(i+2>=n||p[i+1]<0x80||p[i+1]>0x9f||(p[i+2]&0xc0)!=0x80)return 0;i+=3;continue;}if(c==0xf0){if(i+3>=n||p[i+1]<0x90||p[i+1]>0xbf||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80)return 0;i+=4;continue;}if(c>=0xf1&&c<=0xf3){if(i+3>=n||(p[i+1]&0xc0)!=0x80||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80)return 0;i+=4;continue;}if(c==0xf4){if(i+3>=n||p[i+1]<0x80||p[i+1]>0x8f||(p[i+2]&0xc0)!=0x80||(p[i+3]&0xc0)!=0x80)return 0;i+=4;continue;}return 0;}return 1;}
static const char *update_path(void){const char *p=getenv("LIBREECHO_UPDATE");return p&&*p?p:"/usr/local/sbin/libreecho-update";}
static const char *update_fetch_path(void){const char *p=getenv("LIBREECHO_UPDATE_FETCH");return p&&*p?p:"/usr/local/sbin/libreecho-update-fetch";}
static int update_fetch_supported(const struct api_context*c){return !strcmp(le_backend_mode(c->backend),"linux")&&!access(update_path(),X_OK)&&!access(update_fetch_path(),X_OK);}

static int key_from_file(const char*,const char*,char*,size_t);
int api_update_upload_authorize(struct api_context*c,const struct api_request*q,struct api_response*r){if(strcmp(q->path,"/api/v1/system/update/upload")||strcmp(q->method,"POST")){method_not_allowed(r);return 0;}if(!security(c,q,r))return 0;if(strcmp(le_backend_mode(c->backend),"linux")||access("/usr/local/sbin/libreecho-update",X_OK)){err(r,501,LE_NOT_SUPPORTED,"Signed OTA updates are unavailable on this image");return 0;}return 1;}
int api_update_fetch_authorize(struct api_context*c,const struct api_request*q,struct api_response*r){int apply=!strcmp(q->path,"/api/v1/system/update/apply"),path_ok=!strcmp(q->path,"/api/v1/system/update/check")||apply;char status[64]="";if(!path_ok||strcmp(q->method,"POST")){method_not_allowed(r);return 0;}if(!security(c,q,r))return 0;if(!update_fetch_supported(c)){err(r,501,LE_NOT_SUPPORTED,"GitHub OTA updates are unavailable on this image");return 0;}if(apply&&(!key_from_file("/data/libreecho/update/check-status","status",status,sizeof(status))||strcmp(status,"update-available"))){err(r,409,LE_BUSY,"No checked update is currently available");return 0;}return 1;}
int api_update_channel_authorize(struct api_context*c,const struct api_request*q,struct api_response*r,char*channel,size_t channel_size){if(strcmp(q->method,"PUT")){method_not_allowed(r);return 0;}if(!security(c,q,r))return 0;if(!body_ok(q,r))return 0;if(!update_fetch_supported(c)){err(r,501,LE_NOT_SUPPORTED,"GitHub OTA updates are unavailable on this image");return 0;}if(!channel||json_get_string(q->body,"channel",channel,channel_size)!=1||(strcmp(channel,"stable")&&strcmp(channel,"dev"))||json_duplicate_key(q->body,q->body_len,"channel")){err(r,400,LE_INVALID,"Channel must be stable or dev");return 0;}return 1;}
static int key_from_stream(FILE*f,const char*key,char*out,size_t size){char line[256];size_t n=strlen(key);while(fgets(line,sizeof(line),f))if(!strncmp(line,key,n)&&line[n]=='='){size_t len=strcspn(line+n+1,"\r\n");if(len>=size)len=size-1;memcpy(out,line+n+1,len);out[len]=0;return 1;}return 0;}
static int key_from_file(const char*path,const char*key,char*out,size_t size){FILE*f=fopen(path,"r");int found;if(!f)return 0;found=key_from_stream(f,key,out,size);fclose(f);return found;}
static void update_status_json(struct api_context*c,struct api_response*r)
{
    FILE*f;
    char selected[4]="-",current[4]="-",inactive[4]="-",pending_slot[4]="-";
    char state[64]="idle",progress_text[16]="0",version[96]="";
    char installed_version[96]="",rollback_version[96]="",latest_version[96]="";
    char check_status[64]="not-checked",check_error[96]="";
    char source[64]="github-releases",channel[32]="stable",reachable[16]="unknown";
    char automatic_text[16]="0";
    char last_check_text[24]="0",last_success_text[24]="0";
    char escaped_state[128],escaped_version[192],escaped_installed[192];
    char escaped_rollback[192],escaped_latest[192],escaped_check_status[128];
    char escaped_check_error[192],escaped_source[128],escaped_channel[64];
    char escaped_reachable[32];
    int supported=!strcmp(le_backend_mode(c->backend),"linux")&&
        !access("/usr/local/sbin/libreecho-bootctl",X_OK)&&
        !access("/usr/local/sbin/libreecho-update",X_OK)&&
        !access("/usr/local/sbin/libreecho-update-fetch",X_OK);
    int progress=0,pending=0,automatic=0,allow_unsigned=0;
    long last_check=0,last_success=0;

    if(supported){
        key_from_file("/data/libreecho/update/state","state",state,sizeof(state));
        key_from_file("/data/libreecho/update/state","progress",progress_text,
                      sizeof(progress_text));
        pending=key_from_file("/data/libreecho/update/pending","version",version,
                              sizeof(version));
        key_from_file("/data/libreecho/update/pending","slot",pending_slot,
                      sizeof(pending_slot));
        key_from_file("/data/libreecho/update/installed","version",
                      installed_version,sizeof(installed_version));
        key_from_file("/data/libreecho/update/rolled-back","version",
                      rollback_version,sizeof(rollback_version));
        key_from_file("/data/libreecho/update/check-status","status",
                      check_status,sizeof(check_status));
        key_from_file("/data/libreecho/update/check-status","error",
                      check_error,sizeof(check_error));
        key_from_file("/data/libreecho/update/check-status","source",
                      source,sizeof(source));
        key_from_file("/data/libreecho/update/check-status","channel",
                      channel,sizeof(channel));
        key_from_file("/data/libreecho/update/check-status","source_reachable",
                      reachable,sizeof(reachable));
        key_from_file("/data/libreecho/update/check-status","latest_version",
                      latest_version,sizeof(latest_version));
        key_from_file("/data/libreecho/update/check-status","last_check_epoch",
                      last_check_text,sizeof(last_check_text));
        key_from_file("/data/libreecho/update/check-status","last_success_epoch",
                      last_success_text,sizeof(last_success_text));
        key_from_file("/data/libreecho/update/automatic-updates","enabled",
                      automatic_text,sizeof(automatic_text));
        automatic=!strcmp(automatic_text,"1");
        progress=atoi(progress_text);
        if(progress<0||progress>100)progress=0;
        last_check=atol(last_check_text);
        last_success=atol(last_success_text);
        if(last_check<0)last_check=0;
        if(last_success<0)last_success=0;
    }
    if(supported&&(f=popen("/usr/local/sbin/libreecho-bootctl status","r"))){
        key_from_stream(f,"selected_slot",selected,sizeof(selected));
        pclose(f);
        if(selected[0]=='a'&&!selected[1]){
            strcpy(current,"a");
            strcpy(inactive,"b");
        }else if(selected[0]=='b'&&!selected[1]){
            strcpy(current,"b");
            strcpy(inactive,"a");
        }
    }
    if(pending&&!strcmp(state,"reboot-pending")&&
       ((pending_slot[0]=='a'&&!pending_slot[1])||
        (pending_slot[0]=='b'&&!pending_slot[1]))&&
       !strcmp(selected,pending_slot)){
        strcpy(inactive,pending_slot);
        strcpy(current,pending_slot[0]=='a'?"b":"a");
    }
    json_escape(escaped_state,sizeof(escaped_state),state);
    json_escape(escaped_version,sizeof(escaped_version),version);
    if(!installed_version[0])snprintf(installed_version,sizeof(installed_version),"%s",LE_OS_VERSION_STRING);
    json_escape(escaped_installed,sizeof(escaped_installed),installed_version);
    json_escape(escaped_rollback,sizeof(escaped_rollback),rollback_version);
    /* Advertise unsigned manual upload only when the installed update tool
       actually supports --allow-unsigned; an older tool prints usage on the
       unknown 'capabilities' subcommand and exits non-zero, so the flag stays
       off and the UI hides the checkbox. Avoids a broken side-load path when
       this UI ships ahead of the matching platform change. */
    if(supported&&(f=popen("/usr/local/sbin/libreecho-update capabilities 2>/dev/null","r"))){
        char cap[64];
        while(fgets(cap,sizeof(cap),f)){cap[strcspn(cap,"\r\n")]=0;if(!strcmp(cap,"allow-unsigned"))allow_unsigned=1;}
        pclose(f);
    }
    json_escape(escaped_latest,sizeof(escaped_latest),latest_version);
    json_escape(escaped_check_status,sizeof(escaped_check_status),check_status);
    json_escape(escaped_check_error,sizeof(escaped_check_error),check_error);
    json_escape(escaped_source,sizeof(escaped_source),source);
    json_escape(escaped_channel,sizeof(escaped_channel),channel);
    json_escape(escaped_reachable,sizeof(escaped_reachable),reachable);
    out(r,200,"{\"ok\":true,\"data\":{\"supported\":%s,\"current_slot\":\"%s\","
        "\"inactive_slot\":\"%s\",\"state\":\"%s\",\"progress\":%d,"
        "\"pending_reboot\":%s,\"pending_version\":\"%s\","
        "\"installed_version\":\"%s\",\"latest_version\":\"%s\","
        "\"channel\":\"%s\",\"source\":\"%s\",\"source_reachable\":\"%s\","
        "\"check_status\":\"%s\",\"check_error\":\"%s\","
        "\"last_check_epoch\":%ld,\"last_success_epoch\":%ld,"
        "\"automatic_updates\":%s,"
        "\"rollback_available\":%s,\"rollback_version\":\"%s\","
        "\"allow_unsigned\":%s,\"max_upload_bytes\":33554432},"
        "\"error\":null}",supported?"true":"false",current,inactive,escaped_state,
        progress,pending?"true":"false",escaped_version,escaped_installed,
        escaped_latest,escaped_channel,escaped_source,escaped_reachable,
        escaped_check_status,escaped_check_error,last_check,last_success,
        automatic?"true":"false",supported?"true":"false",escaped_rollback,
        allow_unsigned?"true":"false");
}
static int agent_command(const char*command,const char*args,char*output,size_t size){struct le_adapter*agent=le_adapter_connect(LE_ADAPTER_AGENT_SOCK,15000);int rc;if(!agent)return LE_NOT_SUPPORTED;if(!strcmp(command,"respond"))le_adapter_set_io_timeout(agent,120000);rc=le_adapter_call(agent,command,args,output,size);le_adapter_close(agent);return rc==LE_ADAPTER_OK?LE_OK:rc==LE_ADAPTER_ERR_REJECTED?LE_INVALID:LE_IO;}
static void agent_result(struct api_response*r,const char*command,const char*args){char data[LE_ADAPTER_MSG_MAX];int rc=agent_command(command,args,data,sizeof(data));if(rc)err(r,rc==LE_INVALID?400:rc==LE_NOT_SUPPORTED?503:502,rc,rc==LE_NOT_SUPPORTED?"Voice assistant service is unavailable":rc==LE_INVALID?data:"Voice assistant service request failed");else ok(r,data);}
static int wifi_scan_json(struct api_response*r,const struct le_wifi_scan*s){size_t i,n=0;char ssid[LE_TEXT*2],security[32];int written;if(!r||!s)return -1;written=snprintf(r->body+n,sizeof(r->body)-n,"{\"ok\":true,\"data\":{\"networks\":[");if(written<0||(size_t)written>=sizeof(r->body)-n)return -1;n+=(size_t)written;for(i=0;i<s->count&&i<LE_MAX_WIFI;i++){json_escape(ssid,sizeof(ssid),s->networks[i].ssid);json_escape(security,sizeof(security),s->networks[i].security);written=snprintf(r->body+n,sizeof(r->body)-n,"%s{\"ssid\":\"%s\",\"security\":\"%s\",\"signal\":%d}",i?",":"",ssid,security,s->networks[i].signal);if(written<0||(size_t)written>=sizeof(r->body)-n)return -1;n+=(size_t)written;}written=snprintf(r->body+n,sizeof(r->body)-n,"]},\"error\":null}");if(written<0||(size_t)written>=sizeof(r->body)-n)return -1;r->status=200;strcpy(r->type,"application/json; charset=utf-8");r->length=n+(size_t)written;return 0;}
static int persist_configuration(struct api_context*);
static int run_init_command(const char *path, const char *arg)
{
    pid_t child = fork();
    int status;
    if (child < 0)
        return LE_IO;
    if (child == 0) {
        execl(path, path, arg, (char *)NULL);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0)
        return LE_IO;
    return LE_OK;
}
static int apply_home_assistant_mode(int enabled)
{
    static const char *const stop_local[] = {
        "/etc/init.d/libreecho-agentd.init", "stop", NULL
    };
    static const char *const start_local[] = {
        "/etc/init.d/libreecho-agentd.init", "start", NULL
    };
    static const char *const stop_stt[] = {
        "/etc/init.d/libreecho-sttd.init", "stop", NULL
    };
    static const char *const start_stt[] = {
        "/etc/init.d/libreecho-sttd.init", "start", NULL
    };
    static const char *const stop_tts[] = {
        "/etc/init.d/libreecho-ttsd.init", "stop", NULL
    };
    static const char *const start_tts[] = {
        "/etc/init.d/libreecho-ttsd.init", "start", NULL
    };
    static const char *const stop_wyoming[] = {
        "/etc/init.d/libreecho-wyomingd.init", "stop", NULL
    };
    static const char *const start_wyoming[] = {
        "/etc/init.d/libreecho-wyomingd.init", "start", NULL
    };
    if (access("/etc/init.d/libreecho-wyomingd.init", X_OK) < 0)
        return LE_OK;

    if (enabled) {
        if (run_init_command(stop_local[0], stop_local[1]) ||
            run_init_command(stop_stt[0], stop_stt[1]) ||
            run_init_command(stop_tts[0], stop_tts[1]) ||
            run_init_command(start_wyoming[0], start_wyoming[1]))
            return LE_IO;
    } else {
        if (run_init_command(stop_wyoming[0], stop_wyoming[1]) ||
            run_init_command(start_stt[0], start_stt[1]) ||
            run_init_command(start_tts[0], start_tts[1]) ||
            run_init_command(start_local[0], start_local[1]))
            return LE_IO;
    }
    return LE_OK;
}
static int valid_pipeline_token(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;

    if (!value || !value[0])
        return 0;
    while (*p) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
            return 0;
        ++p;
    }
    return 1;
}
static int apply_voice_pipeline_mode(const char *mode)
{
    static const char *const agent = "/etc/init.d/libreecho-agentd.init";
    static const char *const stt = "/etc/init.d/libreecho-sttd.init";
    static const char *const tts = "/etc/init.d/libreecho-ttsd.init";
    static const char *const satellite = "/etc/init.d/libreecho-wyomingd.init";

    if (access(stt, X_OK) < 0 || access(tts, X_OK) < 0 ||
        access(agent, X_OK) < 0)
        return LE_OK;
    if (run_init_command(agent, "stop") ||
        run_init_command(stt, "stop") ||
        run_init_command(tts, "stop"))
        return LE_IO;
    if (access(satellite, X_OK) == 0 &&
        run_init_command(satellite, "stop"))
        return LE_IO;
    if (!strcmp(mode, "home-assistant"))
        return access(satellite, X_OK) == 0
            ? run_init_command(satellite, "start") : LE_NOT_SUPPORTED;
    if (run_init_command(stt, "start") ||
        run_init_command(tts, "start") ||
        run_init_command(agent, "start"))
        return LE_IO;
    return LE_OK;
}
static int pipeline_endpoint_reachable(const struct api_context *c,
                                       const char *uri)
{
    int fd;

    if (!uri[0] || strcmp(le_backend_mode(c->backend), "linux"))
        return 0;
    fd = le_wyoming_connect(uri, 500);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}
static void ensure_voice_pipeline_config(struct api_context *c)
{
    char saved[16384];
    char value[320];

    if (c->voice_pipeline_mode[0])
        return;
    snprintf(c->voice_pipeline_mode, sizeof(c->voice_pipeline_mode),
             "%s", "local");
    snprintf(c->stt_wyoming_model, sizeof(c->stt_wyoming_model),
             "%s", "whisper-small");
    snprintf(c->tts_wyoming_voice, sizeof(c->tts_wyoming_voice),
             "%s", "en_GB-alan-medium");
    if (!c->config_path[0] ||
        config_read(c->config_path, saved, sizeof(saved)) <= 0)
        return;
    if (json_get_string(saved, "voice_pipeline_mode",
                        value, sizeof(value)) == 1 &&
        (!strcmp(value, "local") || !strcmp(value, "custom") ||
         !strcmp(value, "home-assistant")))
        strcpy(c->voice_pipeline_mode, value);
    if (json_get_string(saved, "stt_wyoming_uri",
                        value, sizeof(value)) == 1 &&
        (!value[0] || le_wyoming_uri_valid(value)))
        snprintf(c->stt_wyoming_uri, sizeof(c->stt_wyoming_uri),
                 "%s", value);
    if (json_get_string(saved, "stt_wyoming_model",
                        value, sizeof(value)) == 1 &&
        valid_pipeline_token(value) &&
        strlen(value) < sizeof(c->stt_wyoming_model))
        strcpy(c->stt_wyoming_model, value);
    if (json_get_string(saved, "tts_wyoming_uri",
                        value, sizeof(value)) == 1 &&
        (!value[0] || le_wyoming_uri_valid(value)))
        snprintf(c->tts_wyoming_uri, sizeof(c->tts_wyoming_uri),
                 "%s", value);
    if (json_get_string(saved, "tts_wyoming_voice",
                        value, sizeof(value)) == 1 &&
        valid_pipeline_token(value) &&
        strlen(value) < sizeof(c->tts_wyoming_voice))
        strcpy(c->tts_wyoming_voice, value);
}
/*
 * Endpointing tunables for libreecho-sttd.  The ranges mirror the ones
 * stt_engine_wyoming.c enforces, so a value accepted here cannot be
 * silently rejected by the daemon and fall back to its own default.
 *
 * The defaults are deliberately not the daemon's: on this hardware the mic
 * path never goes truly quiet, so a low VAD floor means end-of-speech never
 * fires and every turn runs to the maximum length.  A higher floor and a
 * shorter cap keep a turn ending promptly after the speaker stops.
 *
 * End-of-speech is 1.5s rather than the daemon's 600ms because that is
 * about the length of an ordinary pause mid-sentence.  Cutting at 600ms
 * truncates anyone who pauses to think, which costs a whole retry; waiting
 * a natural beat is the cheaper error.
 */
#define LE_STT_MAX_UTTERANCE_DEFAULT 6000
#define LE_STT_END_SILENCE_DEFAULT 1000
#define LE_STT_VAD_FLOOR_DEFAULT 16
/*
 * The device has no zoneinfo database, so the timezone is a POSIX TZ string
 * such as "CST6CDT,M3.2.0,M11.1.0" rather than "America/Chicago".  It ends
 * up exported into every daemon's environment by the init script, so keep
 * it to characters that cannot do anything surprising there.
 */
static int timezone_valid(const char *tz)
{
    size_t i;

    if (!tz || !tz[0] || strlen(tz) > 63)
        return 0;
    for (i = 0; tz[i]; ++i) {
        unsigned char ch = (unsigned char)tz[i];

        if (!isalnum(ch) && ch != '+' && ch != '-' && ch != ':' &&
            ch != ',' && ch != '.' && ch != '/')
            return 0;
    }
    return 1;
}

static int stt_max_utterance_valid(int v){return v>=2000&&v<=20000;}
static int stt_end_silence_valid(int v){return v>=200&&v<=3000;}
static int stt_vad_floor_valid(int v){return v>=1&&v<=16384;}
#define LE_AUDIO_RETENTION_DEFAULT_HOURS 24
#define LE_AUDIO_RETENTION_DEFAULT_MAX_MB 64
static int audio_retention_hours_valid(int v){return v==24||v==168||v==720;}
static int audio_retention_max_mb_valid(int v){return v==16||v==64||v==256;}
static int audio_retention_mode_valid(const char *mode){return mode&&(!strcmp(mode,"none")||!strcmp(mode,"local")||!strcmp(mode,"remote"));}
static int audio_remote_url_valid(const char *url)
{
    size_t i;
    if (!url || strncmp(url,"https://",8) || !url[8] || strlen(url)>=256 ||
        strchr(url,'@') || strchr(url,'?') || strchr(url,'#'))
        return 0;
    for (i=0; url[i]; ++i)
        if (isspace((unsigned char)url[i]) || url[i]=='\\' || url[i]=='"')
            return 0;
    return 1;
}
/* The release has a configuration contract but no remote-retention worker.
 * Keep this false until an authenticated TLS upload service is shipped. */
static int remote_retention_transport_available(const struct api_context *c)
{
    (void)c;
    return 0;
}
static const char *audio_retention_state(const struct api_context *c,
                                         const char **effective,
                                         const char **error)
{
    if (!strcmp(c->privacy_audio_mode,"none")) {
        *effective="none"; *error=""; return "disabled";
    }
    if (!strcmp(c->privacy_audio_mode,"local")) {
        *effective="local"; *error=""; return "ready";
    }
    if (!c->privacy_audio_remote_url[0]) {
        *effective="none";
        *error="Remote destination is not configured";
        return "not-configured";
    }
    if (!remote_retention_transport_available(c)) {
        *effective="none";
        *error="Remote retention transport is unavailable on this release";
        return "unavailable";
    }
    *effective="remote"; *error=""; return "ready";
}

static void voice_pipeline_json(struct api_context *c,
                                struct api_response *r)
{
    char stt_uri[640], stt_model[256], tts_uri[640], tts_voice[256];
    int custom;
    int stt_reachable;
    int tts_reachable;

    ensure_voice_pipeline_config(c);
    custom = !strcmp(c->voice_pipeline_mode, "custom");
    stt_reachable = custom &&
        pipeline_endpoint_reachable(c, c->stt_wyoming_uri);
    tts_reachable = custom &&
        pipeline_endpoint_reachable(c, c->tts_wyoming_uri);

    json_escape(stt_uri, sizeof(stt_uri), c->stt_wyoming_uri);
    json_escape(stt_model, sizeof(stt_model), c->stt_wyoming_model);
    json_escape(tts_uri, sizeof(tts_uri), c->tts_wyoming_uri);
    json_escape(tts_voice, sizeof(tts_voice), c->tts_wyoming_voice);
    out(r, 200,
        "{\"ok\":true,\"data\":{\"mode\":\"%s\","
        "\"stt\":{\"engine\":\"%s\",\"wyoming_uri\":\"%s\","
        "\"model\":\"%s\",\"configured\":%s,\"reachable\":%s},"
        "\"tts\":{\"engine\":\"%s\",\"wyoming_uri\":\"%s\","
        "\"voice\":\"%s\",\"configured\":%s,\"reachable\":%s},"
        "\"wake_word\":{\"engine\":\"openwakeword\","
        "\"processing\":\"on-device\"},"
        /* How long the device keeps listening after the wake word and how it
           decides the speaker has stopped.  Surfaced so an owner can see and
           tune them for a room with background conversation, where the VAD
           stays active and end-of-speech would otherwise never fire. */
        "\"listening\":{\"max_utterance_ms\":%d,"
        "\"end_silence_ms\":%d,\"vad_floor_rms\":%d}},\"error\":null}",
        c->voice_pipeline_mode, custom ? "wyoming" : "sherpa",
        stt_uri, stt_model, c->stt_wyoming_uri[0] ? "true" : "false",
        stt_reachable ? "true" : "false",
        custom ? "wyoming" : "sherpa", tts_uri, tts_voice,
        c->tts_wyoming_uri[0] ? "true" : "false",
        tts_reachable ? "true" : "false",
        c->stt_max_utterance_ms, c->stt_end_silence_ms,
        c->stt_vad_floor_rms);
}
static int voice_pipeline_update(struct api_context *c, const char *json)
{
    char mode[sizeof(c->voice_pipeline_mode)];
    char stt_uri[sizeof(c->stt_wyoming_uri)];
    char stt_model[sizeof(c->stt_wyoming_model)];
    char tts_uri[sizeof(c->tts_wyoming_uri)];
    char tts_voice[sizeof(c->tts_wyoming_voice)];
    int max_utterance = c->stt_max_utterance_ms;
    int end_silence = c->stt_end_silence_ms;
    int vad_floor = c->stt_vad_floor_rms;

    ensure_voice_pipeline_config(c);
    snprintf(mode, sizeof(mode), "%s", c->voice_pipeline_mode);
    snprintf(stt_uri, sizeof(stt_uri), "%s", c->stt_wyoming_uri);
    snprintf(stt_model, sizeof(stt_model), "%s", c->stt_wyoming_model);
    snprintf(tts_uri, sizeof(tts_uri), "%s", c->tts_wyoming_uri);
    snprintf(tts_voice, sizeof(tts_voice), "%s", c->tts_wyoming_voice);
    if (json_get_string(json, "mode", mode, sizeof(mode)) < 0 ||
        json_get_string(json, "stt_wyoming_uri",
                        stt_uri, sizeof(stt_uri)) < 0 ||
        json_get_string(json, "stt_model",
                        stt_model, sizeof(stt_model)) < 0 ||
        json_get_string(json, "tts_wyoming_uri",
                        tts_uri, sizeof(tts_uri)) < 0 ||
        json_get_string(json, "tts_voice",
                        tts_voice, sizeof(tts_voice)) < 0)
        return LE_INVALID;
    /*
     * Endpointing was readable but not writable, so the only way to change
     * it was to rebuild the image -- and because persist_configuration()
     * rewrites the file from this model, any value hand-edited into
     * web-config.json was dropped on the next save.  Carry the three
     * settings in the model so a PUT sticks across a reboot.
     */
    {
        int value;
        int field;

        field = json_get_int(json, "max_utterance_ms", &value);
        if (field < 0)
            return LE_INVALID;
        if (field == 1) {
            if (!stt_max_utterance_valid(value))
                return LE_INVALID;
            max_utterance = value;
        }
        field = json_get_int(json, "end_silence_ms", &value);
        if (field < 0)
            return LE_INVALID;
        if (field == 1) {
            if (!stt_end_silence_valid(value))
                return LE_INVALID;
            end_silence = value;
        }
        field = json_get_int(json, "vad_floor_rms", &value);
        if (field < 0)
            return LE_INVALID;
        if (field == 1) {
            if (!stt_vad_floor_valid(value))
                return LE_INVALID;
            vad_floor = value;
        }
    }
    if (strcmp(mode, "local") && strcmp(mode, "custom") &&
        strcmp(mode, "home-assistant"))
        return LE_INVALID;
    if (!valid_pipeline_token(stt_model) ||
        !valid_pipeline_token(tts_voice))
        return LE_INVALID;
    if (!strcmp(mode, "custom") &&
        (!le_wyoming_uri_valid(stt_uri) ||
         !le_wyoming_uri_valid(tts_uri)))
        return LE_INVALID;
    c->stt_max_utterance_ms = max_utterance;
    c->stt_end_silence_ms = end_silence;
    c->stt_vad_floor_rms = vad_floor;
    snprintf(c->voice_pipeline_mode, sizeof(c->voice_pipeline_mode),
             "%s", mode);
    snprintf(c->stt_wyoming_uri, sizeof(c->stt_wyoming_uri),
             "%s", stt_uri);
    snprintf(c->stt_wyoming_model, sizeof(c->stt_wyoming_model),
             "%s", stt_model);
    snprintf(c->tts_wyoming_uri, sizeof(c->tts_wyoming_uri),
             "%s", tts_uri);
    snprintf(c->tts_wyoming_voice, sizeof(c->tts_wyoming_voice),
             "%s", tts_voice);
    if (!strcmp(mode, "custom"))
        c->privacy_local_only = 0;
    return LE_OK;
}
static int valid_hostname(const char*s){size_t i,n;if(!s)return 0;n=strlen(s);if(!n||n>63||s[0]=='-'||s[n-1]=='-')return 0;for(i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='-')return 0;return 1;}
static int valid_wifi_security(const char*s){return s&&(!strcmp(s,"open")||!strcmp(s,"wpa2"));}
static int hex_digit(unsigned char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int query_component_decode(char*outbuf,size_t out_size,const char*value,size_t value_len){size_t i=0,o=0;if(!outbuf||!out_size||!value)return -1;while(i<value_len){unsigned char c=(unsigned char)value[i++];if(c=='%'){int hi,lo;if(i+1>=value_len||(hi=hex_digit((unsigned char)value[i]))<0||(lo=hex_digit((unsigned char)value[i+1]))<0)return -1;c=(unsigned char)((hi<<4)|lo);i+=2;if(!c)return -1;}else if(c=='+')c=' ';if(o+1>=out_size)return -1;outbuf[o++]=(char)c;}outbuf[o]='\0';return 0;}
static const char *vendor_status_path(void){const char*p=getenv("LIBREECHO_VENDOR_STATUS_PATH");return p&&*p?p:"/run/libreecho/vendor-import.status";}
static const char *vendor_force_marker_path(void){const char*p=getenv("LIBREECHO_VENDOR_FORCE_MARKER");return p&&*p?p:"/data/libreecho/config/vendor-import-force-next-boot";}
static const char *wlan0_path(void){const char*p=getenv("LIBREECHO_WLAN0_PATH");return p&&*p?p:"/sys/class/net/wlan0";}
static int safe_status_key(const char*path,const char*key,char*out,size_t size){int fd;FILE*f;struct stat st;int found=0;if(!path||!key||!out||size<2)return 0;out[0]=0;fd=open(path,O_RDONLY|O_CLOEXEC
#ifdef O_NOFOLLOW
|O_NOFOLLOW
#endif
);if(fd<0)return 0;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>2048){close(fd);return 0;}f=fdopen(fd,"r");if(!f){close(fd);return 0;}found=key_from_stream(f,key,out,size);fclose(f);return found;}
static int force_marker_pending(void){struct stat st;return !lstat(vendor_force_marker_path(),&st)&&S_ISREG(st.st_mode)&&!S_ISLNK(st.st_mode);}
static int write_force_marker(void){static const char payload[]="force-unverified-owner-local-import-v1\n";const char*path=vendor_force_marker_path();char parent[512],tmp[512];const char*slash;struct stat st;int fd;ssize_t written;if(!path||strlen(path)>=450)return LE_INVALID;slash=strrchr(path,'/');if(!slash||slash==path)return LE_INVALID;memcpy(parent,path,(size_t)(slash-path));parent[slash-path]=0;if(lstat(parent,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return LE_IO;if(!lstat(path,&st)&&(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)))return LE_IO;snprintf(tmp,sizeof(tmp),"%s.tmp.%ld",path,(long)getpid());fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC
#ifdef O_NOFOLLOW
|O_NOFOLLOW
#endif
,0600);if(fd<0)return LE_IO;written=write(fd,payload,sizeof(payload)-1);if(written!=(ssize_t)(sizeof(payload)-1)||fsync(fd)){close(fd);unlink(tmp);return LE_IO;}if(close(fd)){unlink(tmp);return LE_IO;}if(chmod(tmp,0600)||rename(tmp,path)){unlink(tmp);return LE_IO;}return LE_OK;}
static int setup_marker_path(char*out,size_t size,const char*config_path){int n;if(!out||!size||!config_path||!config_path[0])return LE_IO;n=snprintf(out,size,"%s.setup-complete",config_path);return n<0||(size_t)n>=size?LE_IO:LE_OK;}
static int write_setup_marker(const struct api_context*c,int present){char path[512];if(setup_marker_path(path,sizeof(path),c?c->config_path:NULL))return LE_IO;if(!present)return unlink(path)&&errno!=ENOENT?LE_IO:LE_OK;return config_write_atomic(path,"schema=1\n",8)?LE_IO:LE_OK;}
static void setup_json(struct api_context*c,struct api_response*r){struct le_audio_state a={0},next_audio;struct le_network_state n={0},next_network;struct le_wake_word_state w={0},next_wake;char host[128],wake[128],ssid[LE_TEXT*2],state[48],vendor_state[48]="unavailable",verification[48]="none",layout[64]="none",vendor_error[96]="status-unavailable";char evendor_state[96],everification[96],elayout[128],evendor_error[192];int is_linux=!strcmp(le_backend_mode(c->backend),"linux"),wlan_registered=is_linux&&!access(wlan0_path(),F_OK);a.volume=35;w.enabled=1;w.sensitivity=50;snprintf(w.wake_word,sizeof(w.wake_word),"LibreEcho");snprintf(n.hostname,sizeof(n.hostname),"libreecho");snprintf(n.state,sizeof(n.state),"unavailable");if(!le_get_audio_state(c->backend,&next_audio))a=next_audio;if(!le_get_network_state(c->backend,&next_network))n=next_network;if(!le_get_wake_word_state(c->backend,&next_wake))w=next_wake;if(is_linux){if(!safe_status_key(vendor_status_path(),"state",vendor_state,sizeof(vendor_state)))snprintf(vendor_state,sizeof(vendor_state),"unavailable");if(!safe_status_key(vendor_status_path(),"verification",verification,sizeof(verification)))snprintf(verification,sizeof(verification),"none");if(!safe_status_key(vendor_status_path(),"source_layout",layout,sizeof(layout)))snprintf(layout,sizeof(layout),"none");if(!safe_status_key(vendor_status_path(),"error",vendor_error,sizeof(vendor_error)))snprintf(vendor_error,sizeof(vendor_error),"status-unavailable");}else{snprintf(vendor_state,sizeof(vendor_state),"not-applicable");snprintf(verification,sizeof(verification),"not-applicable");snprintf(vendor_error,sizeof(vendor_error),"none");wlan_registered=1;}json_escape(host,sizeof(host),n.hostname);json_escape(wake,sizeof(wake),w.wake_word);json_escape(ssid,sizeof(ssid),n.ssid);json_escape(state,sizeof(state),n.state);json_escape(evendor_state,sizeof(evendor_state),vendor_state);json_escape(everification,sizeof(everification),verification);json_escape(elayout,sizeof(elayout),layout);json_escape(evendor_error,sizeof(evendor_error),vendor_error);out(r,200,"{\"ok\":true,\"data\":{\"completed\":%s,\"mode\":\"first-boot-ap\",\"backend\":\"%s\",\"hostname\":\"%s\",\"volume\":%d,\"wake_word\":\"%s\",\"wake_sensitivity\":%d,\"local_only\":%s,\"diagnostic_telemetry\":%s,\"network_state\":\"%s\",\"ssid\":\"%s\",\"password_stored\":false,\"vendor_firmware\":{\"state\":\"%s\",\"verification\":\"%s\",\"source_layout\":\"%s\",\"error\":\"%s\",\"force_next_boot\":%s},\"wlan0_registered\":%s},\"error\":null}",c->setup_completed?"true":"false",le_backend_mode(c->backend),host,a.volume,wake,w.sensitivity,c->privacy_local_only?"true":"false",c->privacy_telemetry?"true":"false",state,ssid,evendor_state,everification,elayout,evendor_error,is_linux&&force_marker_pending()?"true":"false",wlan_registered?"true":"false");}
static void setup_force_next_boot(struct api_context*c,const struct api_request*q,struct api_response*r){char confirmation[64];int rc;if(strcmp(q->method,"POST")){method_not_allowed(r);return;}if(strcmp(le_backend_mode(c->backend),"linux")){err(r,501,LE_NOT_SUPPORTED,"Owner-local firmware retry is unavailable on this backend");return;}if(json_get_string(q->body,"confirm",confirmation,sizeof(confirmation))!=1||strcmp(confirmation,"force-unverified-owner-local-import")){err(r,400,LE_INVALID,"Explicit forced-import confirmation is required");return;}rc=write_force_marker();if(rc){err(r,503,rc,"The next-boot firmware retry could not be scheduled");return;}api_log(c,"warning","One-shot unverified owner-local firmware import scheduled for next boot");ok(r,"{\"force_next_boot\":true,\"reboot_required\":true,\"verification\":\"forced-unverified\"}");}
enum setup_failure_stage{SETUP_STAGE_NONE,SETUP_STAGE_HOSTNAME,SETUP_STAGE_AUDIO,SETUP_STAGE_WAKE,SETUP_STAGE_NETWORK,SETUP_STAGE_PERSIST};
static const char*setup_failure_message(const struct api_context*c){switch(c->setup_failure_stage){case SETUP_STAGE_HOSTNAME:return "The device hostname could not be applied";case SETUP_STAGE_AUDIO:return "The initial audio volume could not be applied";case SETUP_STAGE_WAKE:return "The wake-word setting could not be applied";case SETUP_STAGE_NETWORK:return "The Wi-Fi connection could not be completed; check the access point and try again";case SETUP_STAGE_PERSIST:return "Setup settings could not be saved";default:return "Initial setup could not be completed";}}
static int setup_apply(struct api_context*c,const char*j){struct le_wifi_credentials wifi;char hostname[LE_TEXT],wake[LE_TEXT];int volume,sensitivity,local_only,telemetry,rc;memset(&wifi,0,sizeof(wifi));c->setup_failure_stage=SETUP_STAGE_NONE;if(c->setup_completed)return LE_BUSY;if(json_get_string(j,"hostname",hostname,sizeof(hostname))<1||!valid_hostname(hostname)||json_get_string(j,"ssid",wifi.ssid,sizeof(wifi.ssid))<1||!wifi.ssid[0]||json_get_string(j,"security",wifi.security,sizeof(wifi.security))<1||json_get_string(j,"password",wifi.password,sizeof(wifi.password))<1||json_get_int(j,"volume",&volume)<1||volume<0||volume>100||json_get_string(j,"wake_word",wake,sizeof(wake))<1||!wake[0]||json_get_int(j,"wake_sensitivity",&sensitivity)<1||sensitivity<0||sensitivity>100||json_get_bool(j,"local_only",&local_only)<1||json_get_bool(j,"diagnostic_telemetry",&telemetry)<1){memset(wifi.password,0,sizeof(wifi.password));return LE_INVALID;}if(strcmp(wifi.security,"open")&&strlen(wifi.password)<8){memset(wifi.password,0,sizeof(wifi.password));return LE_INVALID;}if(strcmp(wifi.security,"open")&&strcmp(wifi.security,"wpa2")){memset(wifi.password,0,sizeof(wifi.password));return LE_INVALID;}c->setup_failure_stage=SETUP_STAGE_HOSTNAME;if((rc=le_set_hostname(c->backend,hostname))){memset(wifi.password,0,sizeof(wifi.password));return rc;}c->setup_failure_stage=SETUP_STAGE_AUDIO;if((rc=le_set_volume(c->backend,volume))){memset(wifi.password,0,sizeof(wifi.password));return rc;}c->setup_failure_stage=SETUP_STAGE_WAKE;if((rc=le_set_wake_word(c->backend,wake))&&rc!=LE_NOT_SUPPORTED){memset(wifi.password,0,sizeof(wifi.password));return rc;}if((rc=le_set_wake_word_sensitivity(c->backend,sensitivity))&&rc!=LE_NOT_SUPPORTED){memset(wifi.password,0,sizeof(wifi.password));return rc;}c->setup_failure_stage=SETUP_STAGE_NETWORK;if((rc=le_connect_wifi(c->backend,&wifi))){memset(wifi.password,0,sizeof(wifi.password));return rc;}strncpy(c->configured_wake_word,wake,sizeof(c->configured_wake_word)-1);c->configured_wake_word[sizeof(c->configured_wake_word)-1]=0;c->configured_wake_sensitivity=sensitivity;c->configured_wake_valid=1;memset(wifi.password,0,sizeof(wifi.password));c->privacy_local_only=local_only;c->privacy_telemetry=telemetry;c->setup_completed=1;c->setup_failure_stage=SETUP_STAGE_PERSIST;rc=persist_configuration(c);if(!rc)rc=write_setup_marker(c,1);if(rc)c->setup_completed=0;else c->setup_failure_stage=SETUP_STAGE_NONE;return rc;}
/*
 * Is this the real device?
 *
 * The guard below used to test /proc/idme/serial alone. This hardware exposes
 * idme through the device tree instead, at
 * /sys/firmware/devicetree/base/idme/serial/value, so that test never passed
 * and the whole persisted-settings restore was skipped on every boot -- with
 * no log line, because being skipped is a success. Nothing noticed for a long
 * time: audiod and ledd restore their own state, so volume, gain and the LED
 * ring came back anyway, and the wake sensitivity, which has no daemon-side
 * persistence, was the only visible casualty.
 *
 * Accept either location, and honour the same override backend_linux uses so
 * a test can point this at a fixture.
 */
static int device_idme_present(void)
{
    const char *override = getenv("LIBREECHO_IDME_SERIAL_PATH");

    if (override && override[0])
        return access(override, R_OK) == 0;
    return access("/proc/idme/serial", R_OK) == 0 ||
           access("/sys/firmware/devicetree/base/idme/serial/value",
                  R_OK) == 0;
}

/*
 * Re-apply the saved settings to the hardware daemons at startup.
 *
 * `unrestored`, when given, is filled with the names of the settings that did
 * not take, so the caller can say which ones rather than "one or more".
 */
int api_apply_persisted_configuration(struct api_context *c, char *unrestored,
                                      size_t unrestored_size)
{
    char saved[16384], text[LE_TEXT];
    int value, red, green, blue, brightness;
    int hostname_persisted = 0;
    int rc, failed = 0;
    size_t used = 0;

    if (unrestored && unrestored_size)
        unrestored[0] = '\0';

    if (!c || !c->backend || strcmp(le_backend_mode(c->backend), "linux") ||
        geteuid() != 0 || !device_idme_present() ||
        !c->config_path[0] ||
        config_read(c->config_path, saved, sizeof(saved)) <= 0)
        return LE_OK;

/*
 * LE_NOT_SUPPORTED used to count as success here, which quietly threw away
 * every setting whose daemon had not finished starting: adapter_command maps
 * a missing or refused socket onto LE_NOT_SUPPORTED, so at boot the wake word
 * and its sensitivity were dropped, the caller's retry loop saw success and
 * stopped after one pass, and nothing was logged. The saved value survived in
 * the file and was never applied, so every reboot came back on the daemon
 * default.
 *
 * This function only runs against the linux backend on real hardware, where
 * the image ships all of these daemons -- LE_NOT_SUPPORTED here means "not up
 * yet", not "this device cannot do that" -- so treat it as retryable. Record
 * the name either way, so a setting that never applies is diagnosable.
 */
#define APPLY_CONFIG(name, call) do { \
    rc = (call); \
    if (rc != LE_OK) { \
        failed = 1; \
        if (unrestored && unrestored_size && used + 2 < unrestored_size) { \
            int n = snprintf(unrestored + used, unrestored_size - used, \
                             "%s%s", used ? ", " : "", (name)); \
            if (n > 0 && (size_t)n < unrestored_size - used) \
                used += (size_t)n; \
        } \
    } \
} while (0)

    if (json_get_int(saved, "volume", &value) > 0 &&
        value >= 0 && value <= 100)
        APPLY_CONFIG("volume",
                 le_set_volume(c->backend, value));
    if (json_get_int(saved, "microphone_gain", &value) > 0 &&
        value >= 0 && value <= 100)
        APPLY_CONFIG("microphone gain",
                 le_set_microphone_gain(c->backend, value));
    if (json_get_bool(saved, "microphone_muted", &value) > 0)
        APPLY_CONFIG("microphone mute",
                 le_set_microphone_muted(c->backend, value));
    if (json_get_int(saved, "led_r", &red) > 0 &&
        json_get_int(saved, "led_g", &green) > 0 &&
        json_get_int(saved, "led_b", &blue) > 0 &&
        red >= 0 && red <= 255 && green >= 0 && green <= 255 &&
        blue >= 0 && blue <= 255)
        APPLY_CONFIG("LED colour",
                 le_set_led_colour(c->backend, (uint8_t)red,
                                      (uint8_t)green, (uint8_t)blue));
    if (json_get_int(saved, "led_brightness", &brightness) > 0 &&
        brightness >= 0 && brightness <= 100)
        APPLY_CONFIG("LED brightness",
                 le_set_led_brightness(c->backend, brightness));
    if (json_get_bool(saved, "led_visualizer_enabled", &value) > 0)
        APPLY_CONFIG("LED visualizer",
                 le_set_led_visualizer_enabled(c->backend, value));
    if (json_get_string(saved, "wake_word", text, sizeof(text)) > 0 &&
        text[0])
        APPLY_CONFIG("wake word",
                 le_set_wake_word(c->backend, text));
    if (json_get_int(saved, "wake_sensitivity", &value) > 0 &&
        value >= 0 && value <= 100)
        APPLY_CONFIG("wake sensitivity",
                 le_set_wake_word_sensitivity(c->backend, value));
    if (json_get_bool(saved, "hostname_persisted",
                      &hostname_persisted) > 0 &&
        hostname_persisted &&
        json_get_string(saved, "hostname", text, sizeof(text)) > 0 &&
        valid_hostname(text))
        APPLY_CONFIG("hostname",
                 le_set_hostname(c->backend, text));

#undef APPLY_CONFIG
    return failed ? LE_IO : LE_OK;
}
/*
 * How long this boot took to reach the point where the web UI answers.
 * That is exactly what somebody waiting on a reboot is waiting for, so
 * it is a far better estimate for the next reboot than a fixed guess,
 * and it needs nothing persisted.  Falls back to a default when
 * /proc/uptime is unreadable.
 *
 * A value beyond any plausible boot means the daemon was restarted on its
 * own, hours into uptime, rather than started by a boot -- the reading says
 * nothing about boot time then, so fall back to the default rather than
 * clamping it into a wildly pessimistic estimate.
 */
#define LE_BOOT_ESTIMATE_DEFAULT 45
#define LE_BOOT_ESTIMATE_MIN 10
#define LE_BOOT_ESTIMATE_MAX 300
static int measure_boot_seconds(void){FILE*f=fopen("/proc/uptime","r");double up=0.0;int seconds;if(!f)return LE_BOOT_ESTIMATE_DEFAULT;if(fscanf(f,"%lf",&up)!=1){fclose(f);return LE_BOOT_ESTIMATE_DEFAULT;}fclose(f);seconds=(int)(up+0.5);if(seconds<LE_BOOT_ESTIMATE_MIN)seconds=LE_BOOT_ESTIMATE_MIN;if(seconds>LE_BOOT_ESTIMATE_MAX)return LE_BOOT_ESTIMATE_DEFAULT;return seconds;}
int api_init(struct api_context*c,struct le_backend*b,int dev,int insecure,const char*token,const char*origin,const char*csrf,const char*config_path,const char*users_path){char saved[16384];int v;memset(c,0,sizeof(*c));c->backend=b;c->dev_controls=dev;c->allow_insecure_lan=insecure;c->privacy_local_only=1;c->privacy_audio_retention=0;c->privacy_audio_retention_hours=LE_AUDIO_RETENTION_DEFAULT_HOURS;c->privacy_audio_max_mb=LE_AUDIO_RETENTION_DEFAULT_MAX_MB;strcpy(c->privacy_audio_mode,"none");c->privacy_audio_remote_url[0]=0;c->privacy_log_hours=24;c->integrations=4u;c->stt_max_utterance_ms=LE_STT_MAX_UTTERANCE_DEFAULT;c->stt_end_silence_ms=LE_STT_END_SILENCE_DEFAULT;c->stt_vad_floor_rms=LE_STT_VAD_FLOOR_DEFAULT;strcpy(c->timezone,"UTC");c->boot_estimate_seconds=measure_boot_seconds();strcpy(c->button_short,"Start listening");strcpy(c->button_long,"Open pairing mode");if(token)strncpy(c->auth_token,token,sizeof(c->auth_token)-1);if(origin)strncpy(c->allowed_origin,origin,sizeof(c->allowed_origin)-1);if(!csrf||strlen(csrf)!=64)return -1;strncpy(c->csrf_token,csrf,sizeof(c->csrf_token)-1);if(config_path)strncpy(c->config_path,config_path,sizeof(c->config_path)-1);if(users_path)strncpy(c->users_path,users_path,sizeof(c->users_path)-1);if(c->users_path[0]&&!access(c->users_path,F_OK)&&le_auth_load(&c->auth,c->users_path)!=0){fprintf(stderr,"Unable to load LibreEcho users file: %s\n",c->users_path);return -1;}if(c->config_path[0]&&config_read(c->config_path,saved,sizeof(saved))>0){if(json_get_string(saved,"wake_word",c->configured_wake_word,sizeof(c->configured_wake_word))>0&&json_get_int(saved,"wake_sensitivity",&c->configured_wake_sensitivity)>0&&c->configured_wake_sensitivity>=0&&c->configured_wake_sensitivity<=100)c->configured_wake_valid=1;json_get_bool(saved,"privacy_local_only",&c->privacy_local_only);json_get_bool(saved,"privacy_audio_retention",&c->privacy_audio_retention);{char mode[16],url[256];int mode_rc=json_get_string(saved,"privacy_audio_mode",mode,sizeof(mode));if(mode_rc==1&&audio_retention_mode_valid(mode))strcpy(c->privacy_audio_mode,mode);else if(c->privacy_audio_retention)strcpy(c->privacy_audio_mode,"local");if(json_get_int(saved,"privacy_audio_retention_hours",&v)>0&&audio_retention_hours_valid(v))c->privacy_audio_retention_hours=v;if(json_get_int(saved,"privacy_audio_max_mb",&v)>0&&audio_retention_max_mb_valid(v))c->privacy_audio_max_mb=v;if(json_get_string(saved,"privacy_audio_remote_url",url,sizeof(url))==1&&audio_remote_url_valid(url))strcpy(c->privacy_audio_remote_url,url);c->privacy_audio_retention=strcmp(c->privacy_audio_mode,"none")!=0;}json_get_bool(saved,"privacy_telemetry",&c->privacy_telemetry);json_get_bool(saved,"privacy_crash_reports",&c->privacy_crash_reports);if(json_get_int(saved,"privacy_log_hours",&v)>0)c->privacy_log_hours=v;if(json_get_int(saved,"stt_max_utterance_ms",&v)>0&&stt_max_utterance_valid(v))c->stt_max_utterance_ms=v;if(json_get_int(saved,"stt_end_silence_ms",&v)>0&&stt_end_silence_valid(v))c->stt_end_silence_ms=v;if(json_get_int(saved,"stt_vad_floor_rms",&v)>0&&stt_vad_floor_valid(v))c->stt_vad_floor_rms=v;{char tzbuf[64];if(json_get_string(saved,"timezone",tzbuf,sizeof(tzbuf))>0&&timezone_valid(tzbuf))strcpy(c->timezone,tzbuf);}if(json_get_int(saved,"integrations",&v)>0)c->integrations=(unsigned)v;json_get_bool(saved,"ssh",&c->net_ssh);json_get_bool(saved,"api_lan",&c->net_api_lan);json_get_string(saved,"button_short",c->button_short,sizeof(c->button_short));json_get_string(saved,"button_long",c->button_long,sizeof(c->button_long));}event_bus_init(&c->events);api_log(c,"info","LibreEcho web daemon started");return 0;}
static enum le_log_level api_log_level(const char*level){if(level&&!strcmp(level,"error"))return LE_LOG_ERROR;if(level&&(!strcmp(level,"warning")||!strcmp(level,"warn")))return LE_LOG_WARNING;if(level&&!strcmp(level,"debug"))return LE_LOG_DEBUG;return LE_LOG_INFO;}
void api_log(struct api_context*c,const char*level,const char*message){char escaped[180],line[256];time_t t=time(0);struct timespec mono;long boot_seconds=0;if(clock_gettime(CLOCK_MONOTONIC,&mono)==0)boot_seconds=(long)mono.tv_sec;json_escape(escaped,sizeof(escaped),message);snprintf(line,sizeof(line),"{\"timestamp\":%ld,\"boot_seconds\":%ld,\"level\":\"%s\",\"message\":\"%s\"}",(long)t,boot_seconds,level,escaped);snprintf(c->logs[c->log_next%LE_MAX_LOGS],sizeof(c->logs[0]),"%s",line);c->log_next++;if(c->log_count<LE_MAX_LOGS)c->log_count++;event_bus_publish(&c->events,"log",line);le_log(api_log_level(level),"%s",message);}
static int cpu_json(const struct le_system_status*s,char*outbuf,size_t size){size_t i,used=0;int n;if(!size)return -1;n=snprintf(outbuf,size,"{\"count\":%zu,\"cores\":[",s->cpu_count);if(n<0||(size_t)n>=size)return -1;used=(size_t)n;for(i=0;i<s->cpu_count&&i<LE_MAX_CPUS;i++){n=snprintf(outbuf+used,size-used,"%s{\"id\":%zu,\"online\":%s,\"utilization_percent\":%d,\"frequency_khz\":%d}",i?",":"",i,s->cpus[i].online?"true":"false",s->cpus[i].utilization,s->cpus[i].frequency_khz);if(n<0||(size_t)n>=size-used)return -1;used+=(size_t)n;}n=snprintf(outbuf+used,size-used,"]}");return n<0||(size_t)n>=size-used?-1:(int)(used+(size_t)n);}
static void status_json(struct api_context*c,struct api_response*r){struct le_system_status s;char cpus[768],storage_percent[24],storage_used[24],storage_total[24];int rc=le_get_system_status(c->backend,&s);if(rc){err(r,503,rc,"System status is unavailable");return;}if(cpu_json(&s,cpus,sizeof(cpus))<0){err(r,503,LE_IO,"CPU status is unavailable");return;}snprintf(storage_percent,sizeof(storage_percent),s.storage_available?"%d":"null",s.storage);snprintf(storage_used,sizeof(storage_used),s.storage_available?"%d":"null",s.storage_used_mb);snprintf(storage_total,sizeof(storage_total),s.storage_total_mb>=0?"%d":"null",s.storage_total_mb);out(r,200,"{\"ok\":true,\"data\":{\"backend\":\"%s\",\"simulated\":%s,\"uptime_seconds\":%.0f,\"cpu_percent\":%d,\"cpus\":%s,\"memory_percent\":%d,\"memory_used_mb\":%d,\"memory_total_mb\":%d,\"storage_percent\":%s,\"storage_used_mb\":%s,\"storage_total_mb\":%s,\"storage_available\":%s,\"storage_state\":\"%s\",\"temperature_c\":%d,\"device_state\":\"%s\"},\"error\":null}",le_backend_mode(c->backend),!strcmp(le_backend_mode(c->backend),"mock")?"true":"false",s.uptime,s.cpu,cpus,s.memory,s.memory_used_mb,s.memory_total_mb,storage_percent,storage_used,storage_total,s.storage_available?"true":"false",s.storage_state,s.temperature,s.device_state);}
static void device_json(struct api_context*c,struct api_response*r){struct le_device_info d;char n[LE_TEXT*2],h[LE_TEXT*2],model[LE_TEXT*2],serial[LE_TEXT*2],os[64],kernel[128],revision[64],backend[32];int rc=le_get_device_info(c->backend,&d);if(rc){err(r,503,rc,"Device information is unavailable");return;}json_escape(n,sizeof(n),d.name);json_escape(h,sizeof(h),d.hostname);json_escape(model,sizeof(model),d.model);json_escape(serial,sizeof(serial),d.serial);json_escape(os,sizeof(os),d.os_version);json_escape(kernel,sizeof(kernel),d.kernel);json_escape(revision,sizeof(revision),d.hardware_revision);json_escape(backend,sizeof(backend),d.backend);out(r,200,"{\"ok\":true,\"data\":{\"name\":\"%s\",\"hostname\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\",\"os_version\":\"%s\",\"kernel\":\"%s\",\"hardware_revision\":\"%s\",\"backend\":\"%s\"},\"error\":null}",n,h,model,serial,os,kernel,revision,backend);}
/* The colour arrives from audiod. Map it onto the known set rather than
   interpolating the daemon's string straight into the response. */
static const char*noise_colour_name(const struct le_audio_state*a){if(!strcmp(a->noise_colour,"pink"))return "pink";if(!strcmp(a->noise_colour,"brown"))return "brown";return "white";}
static void audio_json(struct api_context*c,struct api_response*r){struct le_audio_state a;int rc=le_get_audio_state(c->backend,&a);if(rc==LE_NOT_SUPPORTED){out(r,200,"{\"ok\":true,\"data\":{\"available\":false,\"unavailable\":true,\"message\":\"Audio hardware backend is not available\"},\"error\":null}");return;}if(rc){err(r,503,rc,"Audio hardware backend status is unavailable");return;}out(r,200,"{\"ok\":true,\"data\":{\"volume\":%d,\"microphone_gain\":%d,\"notification_volume\":%d,\"microphone_muted\":%s,\"startup_sound\":%s,\"amplifier_on\":%s,\"output_available\":%s,\"noise\":{\"active\":%s,\"colour\":\"%s\",\"level\":%d,\"remaining_seconds\":%ld},\"tts_voice\":\"%s\",\"tts_voices\":[{\"id\":\"southern-female\",\"name\":\"Southern English — female\"},{\"id\":\"northern-male\",\"name\":\"Northern English — male\"}]},\"error\":null}",a.volume,a.microphone_gain,a.notification_volume,a.muted?"true":"false",a.startup_sound?"true":"false",a.amplifier_on?"true":"false",a.output_available?"true":"false",a.noise_active?"true":"false",noise_colour_name(&a),a.noise_level,a.noise_remaining_seconds,a.tts_voice[0]?a.tts_voice:"southern-female");}
static void playback_json(struct api_context*c,struct api_response*r){struct le_playback_state p;char state[48],source[80],title[LE_MEDIA_TEXT*2+2],artist[LE_MEDIA_TEXT*2+2],album[LE_MEDIA_TEXT*2+2],source_json[96],title_json[LE_MEDIA_TEXT*2+8],artist_json[LE_MEDIA_TEXT*2+8],album_json[LE_MEDIA_TEXT*2+8];int rc=le_get_playback_state(c->backend,&p);if(rc){err(r,503,rc,"Playback status is unavailable");return;}json_escape(state,sizeof(state),p.state);json_escape(source,sizeof(source),p.source);json_escape(title,sizeof(title),p.title);json_escape(artist,sizeof(artist),p.artist);json_escape(album,sizeof(album),p.album);if(source[0])snprintf(source_json,sizeof(source_json),"\"%s\"",source);else strcpy(source_json,"null");if(title[0])snprintf(title_json,sizeof(title_json),"\"%s\"",title);else strcpy(title_json,"null");if(artist[0])snprintf(artist_json,sizeof(artist_json),"\"%s\"",artist);else strcpy(artist_json,"null");if(album[0])snprintf(album_json,sizeof(album_json),"\"%s\"",album);else strcpy(album_json,"null");out(r,200,"{\"ok\":true,\"data\":{\"state\":\"%s\",\"source\":%s,\"buses\":{\"media\":%s,\"system\":%s,\"announcement\":%s,\"alarm\":%s},\"metadata\":{\"available\":%s,\"title\":%s,\"artist\":%s,\"album\":%s}},\"error\":null}",state,source_json,p.media_active?"true":"false",p.system_active?"true":"false",p.announcement_active?"true":"false",p.alarm_active?"true":"false",p.metadata_available?"true":"false",title_json,artist_json,album_json);}
static int miccal_read(int values[7],int present[7]){int i,v,count=0;char path[64];FILE*f;for(i=0;i<7;i++){values[i]=16384;present[i]=0;snprintf(path,sizeof(path),"/proc/idme/miccal.%d",i);f=fopen(path,"r");if(f){if(fscanf(f,"%d",&v)==1&&v>=0&&v<=65535){values[i]=v;present[i]=1;count++;}fclose(f);}}if(!count){f=fopen("/proc/idme/miccal","r");if(f){for(i=0;i<7;i++)if(fscanf(f,"%d",&v)==1&&v>=0&&v<=65535){values[i]=v;present[i]=1;count++;}fclose(f);}}return count;}
static void baby_monitor_local_json(struct api_context*c,struct api_response*r){FILE*f;char line[512],name[256],sources[30000],calibration[1024];int values[7],present[7],i,calibrated=miccal_read(values,present),available=0,simulated=!strcmp(le_backend_mode(c->backend),"mock");sources[0]='\0';calibration[0]='\0';for(i=0;i<7;i++)snprintf(calibration+strlen(calibration),sizeof(calibration)-strlen(calibration),"%s%d",i?",":"",values[i]);f=fopen("/proc/asound/pcm","r");if(f){while(fgets(line,sizeof(line),f)){unsigned card,device;if(sscanf(line,"%u-%u: %255[^\n]",&card,&device,name)!=3)continue;if(card!=0||device!=24||!strstr(name,"Capture"))continue;{char pcm[64],escaped[512];snprintf(pcm,sizeof(pcm),"/dev/snd/pcmC%uD%uc",card,device);if(!access(pcm,R_OK)){json_escape(escaped,sizeof(escaped),name);snprintf(sources,sizeof(sources),"{\"id\":\"0:24\",\"card\":0,\"device\":24,\"name\":\"%s\",\"rate\":16000,\"channels\":9,\"bits\":24,\"valid_bits\":16,\"encoding\":\"pcm_s24_3le\",\"microphones\":[{\"channel\":0,\"name\":\"Array microphone 1\"},{\"channel\":1,\"name\":\"Array microphone 2\"},{\"channel\":2,\"name\":\"Array microphone 3\"},{\"channel\":3,\"name\":\"Array microphone 4\"},{\"channel\":4,\"name\":\"Array microphone 5\"},{\"channel\":5,\"name\":\"Array microphone 6\"},{\"channel\":6,\"name\":\"Array microphone 7\"}]}",escaped);available=1;}}}fclose(f);}if(!available&&simulated){strcpy(sources,"{\"id\":\"0:0\",\"card\":0,\"device\":0,\"name\":\"Mock microphone\",\"rate\":16000,\"channels\":1,\"bits\":16,\"valid_bits\":16,\"encoding\":\"pcm_s16le\",\"microphones\":[{\"channel\":0,\"name\":\"Microphone 1\"}]}");available=1;}out(r,200,"{\"ok\":true,\"data\":{\"available\":%s,\"simulated\":%s,\"sources\":[%s],\"raw_channels\":9,\"active_microphone_channels\":7,\"inactive_transport_channels\":[7,8],\"logical_channels\":7,\"calibration\":{\"q14\":[%s],\"fallback\":16384,\"source\":\"/proc/idme/miccal\",\"values_found\":%d,\"complete\":%s,\"selected_logical_mics\":[0,3],\"applied_to_raw_stream\":false,\"mapping\":\"identity-provisional; lanes 0 and 3 measured with four-sample compensation\"}},\"error\":null}",available?"true":"false",simulated?"true":"false",sources,calibration,calibrated,calibrated==7?"true":"false");}
static void baby_monitor_json(struct api_context*c,struct api_response*r){struct le_adapter*a;char data[30000];int rc;if(!strcmp(le_backend_mode(c->backend),"mock")){baby_monitor_local_json(c,r);return;}a=le_adapter_connect(LE_ADAPTER_MIC_SOCK,1000);if(!a){err(r,503,LE_NOT_SUPPORTED,"Microphone service is unavailable");return;}rc=le_adapter_call(a,"status","{}",data,sizeof(data));le_adapter_close(a);if(rc!=LE_ADAPTER_OK){err(r,503,LE_IO,"Microphone service status is unavailable");return;}out(r,200,"{\"ok\":true,\"data\":%s,\"error\":null}",data);}
static void led_json(struct api_context*c,struct api_response*r){struct le_led_state l;char owner[64],mood[32];size_t n=0,i;int rc=le_get_led_state(c->backend,&l);if(rc==LE_NOT_SUPPORTED){out(r,200,"{\"ok\":true,\"data\":{\"available\":false,\"unavailable\":true,\"message\":\"LED hardware backend is not available\"},\"error\":null}");return;}if(rc){err(r,503,rc,"LED hardware backend status is unavailable");return;}json_escape(owner,sizeof(owner),l.visualizer_owner);json_escape(mood,sizeof(mood),l.visualizer_mood[0]?l.visualizer_mood:"idle");n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"{\"ok\":true,\"data\":{\"colour\":{\"r\":%u,\"g\":%u,\"b\":%u},\"brightness\":%d,\"animation_active\":%s,\"animation_profile\":\"%s\",\"pattern_active\":%s,\"pattern\":\"%s\",\"visualizer_enabled\":%s,\"visualizer_active\":%s,\"visualizer_owner\":\"%s\",\"visualizer_mood\":\"%s\",\"visualizer_levels\":[",l.current.r,l.current.g,l.current.b,l.current.brightness,l.animation_active?"true":"false",l.animation_profile==0?"listening":l.animation_profile==1?"thinking":l.animation_profile==2?"error":l.animation_profile==3?"dnd":"none",l.pattern_active?"true":"false",l.pattern[0]?l.pattern:"none",l.visualizer_enabled?"true":"false",l.visualizer_active?"true":"false",owner,mood);for(i=0;i<LE_LED_PIXELS&&n<sizeof(r->body);i++)n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"%s%u",i?",":"",l.visualizer_levels[i]);n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"],\"pixels\":[");for(i=0;i<LE_LED_PIXELS&&n<sizeof(r->body);i++)n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"%s{\"r\":%u,\"g\":%u,\"b\":%u}",i?",":"",l.pixels[i].r,l.pixels[i].g,l.pixels[i].b);n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"],\"boot_profile\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d},\"profiles\":{\"listening\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d},\"thinking\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d},\"error\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d},\"dnd\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d},\"night\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d}},\"night\":{\"enabled\":%s,\"active\":%s,\"start_minute\":%d,\"end_minute\":%d}},\"error\":null}",l.boot.r,l.boot.g,l.boot.b,l.boot.brightness,l.listening.r,l.listening.g,l.listening.b,l.listening.brightness,l.thinking.r,l.thinking.g,l.thinking.b,l.thinking.brightness,l.error.r,l.error.g,l.error.b,l.error.brightness,l.dnd.r,l.dnd.g,l.dnd.b,l.dnd.brightness,l.night.r,l.night.g,l.night.b,l.night.brightness,l.night_enabled?"true":"false",l.night_active?"true":"false",l.night_start_minute,l.night_end_minute);if(n>=sizeof(r->body)){err(r,503,LE_IO,"LED status response is too large");return;}r->status=200;strcpy(r->type,"application/json; charset=utf-8");r->length=n;}static void net_json(struct api_context*c,struct api_response*r){
    struct le_network_state n;
    char state[48],connectivity[48],recovery_stage[48],ssid[LE_TEXT*2];
    char ip[96],gateway[96],dns[192],hostname[LE_TEXT*2];
    const char *gateway_reachable;
    int rc=le_get_network_state(c->backend,&n);
    if(rc){err(r,501,rc,"Network adapter is not available");return;}
    n.ssh=c->net_ssh;
    n.api_lan=c->net_api_lan;
    json_escape(state,sizeof(state),n.state);
    json_escape(connectivity,sizeof(connectivity),n.connectivity);
    json_escape(recovery_stage,sizeof(recovery_stage),n.recovery_stage);
    json_escape(ssid,sizeof(ssid),n.ssid);
    json_escape(ip,sizeof(ip),n.ip);
    json_escape(gateway,sizeof(gateway),n.gateway);
    json_escape(dns,sizeof(dns),n.dns);
    json_escape(hostname,sizeof(hostname),n.hostname);
    gateway_reachable=n.gateway_reachable<0?"null":n.gateway_reachable?"true":"false";
    out(r,200,"{\"ok\":true,\"data\":{\"state\":\"%s\",\"connectivity\":\"%s\",\"recovery_stage\":\"%s\",\"gateway_reachable\":%s,\"liveness_failures\":%d,\"ssid\":\"%s\",\"signal\":%d,\"rssi_dbm\":%d,\"ip\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\",\"hostname\":\"%s\",\"internet\":%s,\"dhcp\":%s,\"ssh\":%s,\"api_lan\":%s,\"api_lan_effective\":%s,\"api_lan_forced\":%s},\"error\":null}",state,connectivity,recovery_stage,gateway_reachable,n.liveness_failures,ssid,n.signal,n.rssi_dbm,ip,gateway,dns,hostname,n.internet?"true":"false",n.dhcp?"true":"false",n.ssh?"true":"false",n.api_lan?"true":"false",(c->allow_insecure_lan||n.api_lan)?"true":"false",c->allow_insecure_lan?"true":"false");
}
static void wake_json(struct api_context*c,struct api_response*r){struct le_wake_word_state w;char wake[LE_TEXT*2],model[64];int rc=le_get_wake_word_state(c->backend,&w);if(rc==LE_NOT_SUPPORTED){out(r,200,"{\"ok\":true,\"data\":{\"available\":false,\"unavailable\":true,\"message\":\"Wake-word service is not available\"},\"error\":null}");return;}if(rc){err(r,503,rc,"Wake-word service status is unavailable");return;}json_escape(wake,sizeof(wake),w.wake_word);json_escape(model,sizeof(model),w.model_status);out(r,200,"{\"ok\":true,\"data\":{\"enabled\":%s,\"wake_word\":\"%s\",\"sensitivity\":%d,\"cooldown_ms\":%d,\"model_status\":\"%s\",\"detected_count\":%d,\"cpu_cost_percent\":%d,\"memory_cost_mb\":%d,\"local_processing\":true},\"error\":null}",w.enabled?"true":"false",wake,w.sensitivity,w.cooldown_ms,model,w.detected_count,w.cpu_cost,w.memory_cost_mb);}
static int configuration_item(char*out,size_t size,size_t*used,int*first,const char*fmt,...){va_list ap;int n;if(!out||!used||!first||!fmt||*used>=size)return -1;if(!*first){n=snprintf(out+*used,size-*used,",\n  ");if(n<0||(size_t)n>=size-*used)return -1;*used+=(size_t)n;}va_start(ap,fmt);n=vsnprintf(out+*used,size-*used,fmt,ap);va_end(ap);if(n<0||(size_t)n>=size-*used)return -1;*used+=(size_t)n;*first=0;return 0;}
static int configuration_json(struct api_context*c,char*out,size_t size){struct le_audio_state a;struct le_led_state l;struct le_wake_word_state w;struct le_network_state n;const char*unsupported[16];size_t unsupported_count=0,i,used=0;char wake[128],host[128],short_action[80],long_action[80],remote_url[512];int audio_rc,led_rc,wake_rc,network_rc,first=0,nbytes;audio_rc=le_get_audio_state(c->backend,&a);led_rc=le_get_led_state(c->backend,&l);wake_rc=le_get_wake_word_state(c->backend,&w);network_rc=le_get_network_state(c->backend,&n);if(audio_rc){unsupported[unsupported_count++]="volume";unsupported[unsupported_count++]="microphone_gain";unsupported[unsupported_count++]="microphone_muted";}if(led_rc){unsupported[unsupported_count++]="led_r";unsupported[unsupported_count++]="led_g";unsupported[unsupported_count++]="led_b";unsupported[unsupported_count++]="led_brightness";unsupported[unsupported_count++]="led_visualizer_enabled";}if(wake_rc){unsupported[unsupported_count++]="wake_word";unsupported[unsupported_count++]="wake_sensitivity";}if(network_rc)unsupported[unsupported_count++]="hostname";nbytes=snprintf(out,size,"{\n  \"schema_version\": 1,\n  \"hostname_persisted\": true,\n  \"partial\": %s,\n  \"unsupported\": [",unsupported_count?"true":"false");if(nbytes<0||(size_t)nbytes>=size)return LE_IO;used=(size_t)nbytes;for(i=0;i<unsupported_count;i++){nbytes=snprintf(out+used,size-used,"%s\"%s\"",i?",":"",unsupported[i]);if(nbytes<0||(size_t)nbytes>=size-used)return LE_IO;used+=(size_t)nbytes;}nbytes=snprintf(out+used,size-used,"]");if(nbytes<0||(size_t)nbytes>=size-used)return LE_IO;used+=(size_t)nbytes;if(!audio_rc){if(configuration_item(out,size,&used,&first,"\"volume\": %d",a.volume)||configuration_item(out,size,&used,&first,"\"microphone_gain\": %d",a.microphone_gain)||configuration_item(out,size,&used,&first,"\"microphone_muted\": %s",a.muted?"true":"false"))return LE_IO;}if(!led_rc){if(configuration_item(out,size,&used,&first,"\"led_r\": %u",l.current.r)||configuration_item(out,size,&used,&first,"\"led_g\": %u",l.current.g)||configuration_item(out,size,&used,&first,"\"led_b\": %u",l.current.b)||configuration_item(out,size,&used,&first,"\"led_brightness\": %d",l.current.brightness)||configuration_item(out,size,&used,&first,"\"led_visualizer_enabled\": %s",l.visualizer_enabled?"true":"false"))return LE_IO;}if(!wake_rc){strncpy(c->configured_wake_word,w.wake_word,sizeof(c->configured_wake_word)-1);c->configured_wake_word[sizeof(c->configured_wake_word)-1]=0;c->configured_wake_sensitivity=w.sensitivity;c->configured_wake_valid=1;}if(!wake_rc||c->configured_wake_valid){json_escape(wake,sizeof(wake),!wake_rc?w.wake_word:c->configured_wake_word);if(configuration_item(out,size,&used,&first,"\"wake_word\": \"%s\"",wake)||configuration_item(out,size,&used,&first,"\"wake_sensitivity\": %d",!wake_rc?w.sensitivity:c->configured_wake_sensitivity))return LE_IO;}if(!network_rc){json_escape(host,sizeof(host),n.hostname);if(configuration_item(out,size,&used,&first,"\"hostname\": \"%s\"",host))return LE_IO;}json_escape(short_action,sizeof(short_action),c->button_short);json_escape(long_action,sizeof(long_action),c->button_long);json_escape(remote_url,sizeof(remote_url),c->privacy_audio_remote_url);if(configuration_item(out,size,&used,&first,"\"ssh\": %s",c->net_ssh?"true":"false")||configuration_item(out,size,&used,&first,"\"api_lan\": %s",c->net_api_lan?"true":"false")||configuration_item(out,size,&used,&first,"\"button_short\": \"%s\"",short_action)||configuration_item(out,size,&used,&first,"\"button_long\": \"%s\"",long_action)||configuration_item(out,size,&used,&first,"\"privacy_local_only\": %s",c->privacy_local_only?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_audio_retention\": %s",!strcmp(c->privacy_audio_mode,"local")?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_telemetry\": %s",c->privacy_telemetry?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_crash_reports\": %s",c->privacy_crash_reports?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_log_hours\": %d",c->privacy_log_hours)||configuration_item(out,size,&used,&first,"\"stt_max_utterance_ms\": %d",c->stt_max_utterance_ms)||configuration_item(out,size,&used,&first,"\"stt_end_silence_ms\": %d",c->stt_end_silence_ms)||configuration_item(out,size,&used,&first,"\"stt_vad_floor_rms\": %d",c->stt_vad_floor_rms)||configuration_item(out,size,&used,&first,"\"timezone\": \"%s\"",c->timezone)||configuration_item(out,size,&used,&first,"\"integrations\": %u",c->integrations)||configuration_item(out,size,&used,&first,"\"privacy_audio_mode\": \"%s\"",c->privacy_audio_mode)||configuration_item(out,size,&used,&first,"\"privacy_audio_retention_hours\": %d",c->privacy_audio_retention_hours)||configuration_item(out,size,&used,&first,"\"privacy_audio_max_mb\": %d",c->privacy_audio_max_mb)||configuration_item(out,size,&used,&first,"\"privacy_audio_remote_url\": \"%s\"",remote_url))return LE_IO;nbytes=snprintf(out+used,size-used,"\n}");return nbytes<0||(size_t)nbytes>=size-used?LE_IO:LE_OK;}
static int persist_configuration(struct api_context*c)
{
    char config[8192];
    char mode[48], stt_uri[640], stt_model[256];
    char tts_uri[640], tts_voice[256];
    size_t length;
    int rc;

    ensure_voice_pipeline_config(c);
    rc = configuration_json(c, config, sizeof(config));
    if (rc)
        return rc;
    length = strlen(config);
    if (!length || config[length - 1] != '}')
        return LE_IO;
    json_escape(mode, sizeof(mode), c->voice_pipeline_mode);
    json_escape(stt_uri, sizeof(stt_uri), c->stt_wyoming_uri);
    json_escape(stt_model, sizeof(stt_model), c->stt_wyoming_model);
    json_escape(tts_uri, sizeof(tts_uri), c->tts_wyoming_uri);
    json_escape(tts_voice, sizeof(tts_voice), c->tts_wyoming_voice);
    --length;
    if (snprintf(
            config + length, sizeof(config) - length,
            ",\n  \"voice_pipeline_mode\": \"%s\","
            "\n  \"stt_wyoming_uri\": \"%s\","
            "\n  \"stt_wyoming_model\": \"%s\","
            "\n  \"tts_wyoming_uri\": \"%s\","
            "\n  \"tts_wyoming_voice\": \"%s\"\n}",
            mode, stt_uri, stt_model, tts_uri, tts_voice) >=
        (int)(sizeof(config) - length))
        return LE_IO;
    if (!c->config_path[0])
        return LE_OK;
    return config_write_atomic(c->config_path, config, strlen(config))
        ? LE_IO : LE_OK;
}
static int import_configuration(struct api_context*c,const char*j){char wake[LE_TEXT],hostname[LE_TEXT],short_action[32],long_action[32],mode[16]="none",remote_url[256]="";int schema,volume,gain,muted,r,g,b,brightness,visualizer_enabled=1,visualizer_field,sensitivity,ssh,api_lan,local_only,audio_retention,telemetry,crash_reports,log_hours,integrations,max_utterance,end_silence,vad_floor,mode_field,hours_field,max_field,url_field,retention_hours=LE_AUDIO_RETENTION_DEFAULT_HOURS,max_mb=LE_AUDIO_RETENTION_DEFAULT_MAX_MB,rc;if(strcmp(le_backend_mode(c->backend),"mock"))return LE_NOT_SUPPORTED;mode_field=json_get_string(j,"privacy_audio_mode",mode,sizeof(mode));hours_field=json_get_int(j,"privacy_audio_retention_hours",&retention_hours);max_field=json_get_int(j,"privacy_audio_max_mb",&max_mb);url_field=json_get_string(j,"privacy_audio_remote_url",remote_url,sizeof(remote_url));if(mode_field==0)strcpy(mode,"none");if(mode_field==0&&json_get_bool(j,"privacy_audio_retention",&audio_retention)>0&&audio_retention)strcpy(mode,"local");if(!strcmp(mode,"remote")&&!remote_url[0])return LE_INVALID;if(mode_field<0||hours_field<0||max_field<0||url_field<0||(mode_field>0&&!audio_retention_mode_valid(mode))||(hours_field>0&&!audio_retention_hours_valid(retention_hours))||(max_field>0&&!audio_retention_max_mb_valid(max_mb))||(url_field>0&&remote_url[0]&&!audio_remote_url_valid(remote_url)))return LE_INVALID;visualizer_field=json_get_bool(j,"led_visualizer_enabled",&visualizer_enabled);if(visualizer_field<0||json_get_int(j,"schema_version",&schema)<1||schema!=1||json_get_int(j,"volume",&volume)<1||volume<0||volume>100||json_get_int(j,"microphone_gain",&gain)<1||gain<0||gain>100||json_get_bool(j,"microphone_muted",&muted)<1||json_get_int(j,"led_r",&r)<1||r<0||r>255||json_get_int(j,"led_g",&g)<1||g<0||g>255||json_get_int(j,"led_b",&b)<1||b<0||b>255||json_get_int(j,"led_brightness",&brightness)<1||brightness<0||brightness>100||json_get_string(j,"wake_word",wake,sizeof(wake))<1||json_get_int(j,"wake_sensitivity",&sensitivity)<1||sensitivity<0||sensitivity>100||json_get_string(j,"hostname",hostname,sizeof(hostname))<1||json_get_bool(j,"ssh",&ssh)<1||json_get_bool(j,"api_lan",&api_lan)<1||json_get_string(j,"button_short",short_action,sizeof(short_action))<1||json_get_string(j,"button_long",long_action,sizeof(long_action))<1||json_get_bool(j,"privacy_local_only",&local_only)<1||json_get_bool(j,"privacy_audio_retention",&audio_retention)<1||json_get_bool(j,"privacy_telemetry",&telemetry)<1||json_get_bool(j,"privacy_crash_reports",&crash_reports)<1||json_get_int(j,"privacy_log_hours",&log_hours)<1||(log_hours!=24&&log_hours!=168&&log_hours!=720)||json_get_int(j,"integrations",&integrations)<1||integrations<0||integrations>31||json_get_int(j,"stt_max_utterance_ms",&max_utterance)<1||!stt_max_utterance_valid(max_utterance)||json_get_int(j,"stt_end_silence_ms",&end_silence)<1||!stt_end_silence_valid(end_silence)||json_get_int(j,"stt_vad_floor_rms",&vad_floor)<1||!stt_vad_floor_valid(vad_floor))return LE_INVALID;if((rc=le_set_volume(c->backend,volume))||(rc=le_set_microphone_gain(c->backend,gain))||(rc=le_set_microphone_muted(c->backend,muted))||(rc=le_set_led_colour(c->backend,(uint8_t)r,(uint8_t)g,(uint8_t)b))||(rc=le_set_led_brightness(c->backend,brightness))||(rc=le_set_led_visualizer_enabled(c->backend,visualizer_enabled))||(rc=le_set_wake_word(c->backend,wake))||(rc=le_set_wake_word_sensitivity(c->backend,sensitivity))||(rc=le_set_hostname(c->backend,hostname)))return rc;c->net_ssh=ssh;c->net_api_lan=api_lan;c->privacy_local_only=local_only;c->privacy_audio_retention=strcmp(mode,"none")!=0;snprintf(c->privacy_audio_mode,sizeof(c->privacy_audio_mode),"%s",mode);c->privacy_audio_retention_hours=retention_hours;c->privacy_audio_max_mb=max_mb;snprintf(c->privacy_audio_remote_url,sizeof(c->privacy_audio_remote_url),"%s",remote_url);c->privacy_telemetry=telemetry;c->privacy_crash_reports=crash_reports;c->privacy_log_hours=log_hours;c->integrations=(unsigned)integrations;c->stt_max_utterance_ms=max_utterance;c->stt_end_silence_ms=end_silence;c->stt_vad_floor_rms=vad_floor;strcpy(c->button_short,short_action);strcpy(c->button_long,long_action);return persist_configuration(c);}
static int read_central_logs(char lines[LE_MAX_LOGS][LE_LOGD_MSG_MAX],size_t*count){FILE*f;char line[LE_LOGD_MSG_MAX],ordered[LE_MAX_LOGS][LE_LOGD_MSG_MAX];size_t next=0,total=0;if(!count)return 0;*count=0;f=fopen(LE_LOGD_FILE,"r");if(!f)return 0;while(fgets(line,sizeof(line),f)){size_t n=strlen(line);if(n&&line[n-1]=='\n')line[n-1]=0;memcpy(lines[next%LE_MAX_LOGS],line,sizeof(lines[0]));next++;if(total<LE_MAX_LOGS)total++;}fclose(f);if(total){size_t i,first=next-total;for(i=0;i<total;i++){memcpy(ordered[i],lines[(first+i)%LE_MAX_LOGS],sizeof(ordered[i]));}for(i=0;i<total;i++)memcpy(lines[i],ordered[i],sizeof(lines[i]));*count=total;}return *count>0;}
static void logs_json(struct api_context*c,struct api_response*r){char central[LE_MAX_LOGS][LE_LOGD_MSG_MAX],message[256],escaped[512],level[16],service[32];size_t central_count=0,first,i,n=0;int timestamp,boot_seconds;int from_central=read_central_logs(central,&central_count);n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"{\"ok\":true,\"data\":{\"entries\":[");if(from_central){for(i=0;i<central_count&&n<sizeof(r->body)-500;i++){timestamp=0;boot_seconds=0;level[0]=0;service[0]=0;message[0]=0;json_get_int(central[i],"ts",&timestamp);json_get_int(central[i],"boot_seconds",&boot_seconds);json_get_string(central[i],"level",level,sizeof(level));json_get_string(central[i],"service",service,sizeof(service));json_get_string(central[i],"msg",message,sizeof(message));if(service[0]&&strncmp(message,service,strlen(service))){char prefixed[256];snprintf(prefixed,sizeof(prefixed),"%s: %s",service,message);strncpy(message,prefixed,sizeof(message)-1);message[sizeof(message)-1]=0;}json_escape(escaped,sizeof(escaped),message);n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"%s{\"timestamp\":%d,\"boot_seconds\":%d,\"level\":\"%s\",\"message\":\"%s\"}",i?",":"",timestamp,boot_seconds,level[0]?level:"INFO",escaped);}}else{first=c->log_next-c->log_count;for(i=0;i<c->log_count&&n<sizeof(r->body)-500;i++)n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"%s%s",i?",":"",c->logs[(first+i)%LE_MAX_LOGS]);}n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"],\"source\":\"%s\",\"bounded\":true,\"capacity\":%d},\"error\":null}",from_central?"central-file":"web-memory",LE_MAX_LOGS);r->status=200;strcpy(r->type,"application/json; charset=utf-8");r->length=n;}
static void logs_stream_json(struct api_context*c,struct api_response*r){const char*prefix="event: logs\ndata: ",*suffix="\n\n";size_t prefix_len=strlen(prefix),suffix_len=strlen(suffix),n;logs_json(c,r);if(r->status!=200)return;if(r->length>sizeof(r->body)-prefix_len-suffix_len-1){err(r,503,LE_IO,"Log stream is too large");return;}n=r->length;memmove(r->body+prefix_len,r->body,n);memcpy(r->body,prefix,prefix_len);memcpy(r->body+prefix_len+n,suffix,suffix_len);r->length=prefix_len+n+suffix_len;r->body[r->length]=0;strcpy(r->type,"text/event-stream; charset=utf-8");}
static int service_ready(const char*pidfile,const char*socket_path){FILE*f;long pid=0;int pid_ok=0,socket_ok=0;if(pidfile){f=fopen(pidfile,"r");if(f){if(fscanf(f,"%ld",&pid)==1&&pid>1)pid_ok=kill((pid_t)pid,0)==0;fclose(f);}}if(socket_path)socket_ok=access(socket_path,F_OK)==0;return pid_ok&&(!socket_path||socket_ok);}
static const char*time_status_path(void){const char*p=getenv("LIBREECHO_TIME_STATUS");return p&&*p?p:"/run/libreecho/time.status";}
static int time_status_value(const char*key,char*value,size_t size){FILE*f;char line[1200],*equals;size_t key_len=strlen(key);if(!size)return 0;value[0]=0;f=fopen(time_status_path(),"r");if(!f)return 0;while(fgets(line,sizeof(line),f)){equals=strchr(line,'=');if(!equals)continue;if((size_t)(equals-line)!=key_len||strncmp(line,key,key_len))continue;equals++;equals[strcspn(equals,"\r\n")]=0;strncpy(value,equals,size-1);value[size-1]=0;fclose(f);return 1;}fclose(f);return 0;}
static int time_status_int(const char*key,long*fallback){char value[64],*end;long parsed;if(!time_status_value(key,value,sizeof(value)))return 0;errno=0;parsed=strtol(value,&end,10);if(errno||*end)return 0;*fallback=parsed;return 1;}
static void diagnostics_json(struct api_context*c,struct api_response*r){int linux=!strcmp(le_backend_mode(c->backend),"linux");const char*logging=linux?(service_ready("/var/run/libreecho-logd.pid",LE_LOGD_SOCK)?"ok":"degraded"):"development";const char*network=linux?(service_ready("/var/run/libreecho-networkd.pid","/run/libreecho/network.sock")?"ok":"degraded"):"development";const char*timekeeping=linux?(service_ready("/var/run/libreecho-timed.pid",NULL)?"ok":"degraded"):"development";const char*audio=linux?(service_ready("/var/run/libreecho-audiod.pid","/run/libreecho/audio.sock")?"ok":"degraded"):"development";const char*microphone=linux?(service_ready("/var/run/libreecho-micd.pid",LE_ADAPTER_MIC_SOCK)?"ok":"degraded"):"development";const char*led=linux?(service_ready("/var/run/libreecho-ledd.pid","/run/libreecho/led.sock")?"ok":"degraded"):"development";const char*bluetooth=linux?(service_ready("/var/run/libreecho-btd.pid","/run/libreecho/bluetooth.sock")?"ok":"degraded"):"development";const char*wifi=linux?(access("/sys/class/net/wlan0",F_OK)==0?"ok":"unavailable"):"development";out(r,200,"{\"ok\":true,\"data\":{\"checks\":[{\"name\":\"configuration\",\"status\":\"ok\"},{\"name\":\"backend\",\"status\":\"ok\"},{\"name\":\"central logging\",\"status\":\"%s\"},{\"name\":\"network adapter\",\"status\":\"%s\"},{\"name\":\"time synchronization\",\"status\":\"%s\"},{\"name\":\"audio adapter\",\"status\":\"%s\"},{\"name\":\"microphone adapter\",\"status\":\"%s\"},{\"name\":\"LED adapter\",\"status\":\"%s\"},{\"name\":\"Bluetooth adapter\",\"status\":\"%s\"},{\"name\":\"wlan0\",\"status\":\"%s\"}]},\"error\":null}",logging,network,timekeeping,audio,microphone,led,bluetooth,wifi);}
static void provenance_json(struct api_response*r){
    out(r,200,"{\"ok\":true,\"data\":{\"os_version\":\"%s\",\"source_commit\":\"%s\",\"source_dirty\":%s,\"source_digest\":\"%s\"},\"error\":null}",
        LE_OS_VERSION_STRING,LE_SOURCE_COMMIT,
        !strcmp(LE_SOURCE_DIRTY,"1")?"true":"false",LE_SOURCE_DIGEST);
}
static void system_json(struct api_context*c,struct api_response*r){time_t now=time(0);int valid=now>=1577836800,linux=!strcmp(le_backend_mode(c->backend),"linux");long synchronized=0,rtc_available=0,rtc_persisted=0,last_sync=0;char state[64]="unavailable",source[64],servers[1100]="",escaped_state[128],escaped_source[128],escaped_servers[2200];const char*tz=getenv("TZ");char escaped_tz[128];strcpy(source,valid?"system-clock":"unset");if(linux){time_status_value("state",state,sizeof(state));time_status_value("source",source,sizeof(source));time_status_value("servers",servers,sizeof(servers));time_status_int("synchronized",&synchronized);time_status_int("rtc_available",&rtc_available);time_status_int("rtc_persisted",&rtc_persisted);time_status_int("last_sync_epoch",&last_sync);if(!rtc_available&&access("/sys/class/rtc/rtc0",F_OK)==0)rtc_available=1;}json_escape(escaped_tz,sizeof(escaped_tz),tz&&tz[0]?tz:"UTC");json_escape(escaped_state,sizeof(escaped_state),state);json_escape(escaped_source,sizeof(escaped_source),source);json_escape(escaped_servers,sizeof(escaped_servers),servers);out(r,200,"{\"ok\":true,\"data\":{\"update_channel\":\"stable\",\"ota\":{\"supported\":false,\"design\":\"A/B\",\"current_slot\":\"A\",\"inactive_slot\":\"B\",\"state\":\"idle\",\"progress\":0,\"rollback_available\":false},\"timezone\":\"%s\",\"boot_estimate_seconds\":%d,\"ntp\":%s,\"ntp_state\":\"%s\",\"ntp_servers\":\"%s\",\"last_sync_epoch\":%ld,\"clock_valid\":%s,\"clock_source\":\"%s\",\"rtc_available\":%s,\"rtc_persisted\":%s},\"error\":null}",escaped_tz,c->boot_estimate_seconds,synchronized&&valid?"true":"false",escaped_state,escaped_servers,last_sync,valid?"true":"false",escaped_source,rtc_available?"true":"false",rtc_persisted?"true":"false");}
#define LE_AUTH_FAILURE_LIMIT 5
#define LE_AUTH_FAILURE_WINDOW 60
#define LE_AUTH_BLOCK_SECONDS 60
#define api_handle api_handle_inner
static int bluetooth_append(char *buffer,size_t size,size_t*used,const char*fmt,...){va_list ap;int n;if(!buffer||!used||*used>=size)return-1;va_start(ap,fmt);n=vsnprintf(buffer+*used,size-*used,fmt,ap);va_end(ap);if(n<0||(size_t)n>=size-*used)return-1;*used+=(size_t)n;return 0;}
static void bluetooth_json(struct api_context*c,struct api_response*r){struct le_bluetooth_state b;size_t i,n=0;int rc;char state[64],transport[64],hci[64],local_name[LE_TEXT*2],error[192],disconnect_reason[96],connect_failed_status[96],profile_state[32],profile_error[192],name[LE_TEXT*2];if((rc=le_get_bluetooth_state(c->backend,&b))==LE_NOT_SUPPORTED){out(r,200,"{\"ok\":true,\"data\":{\"available\":false,\"unavailable\":true,\"message\":\"Bluetooth status is unavailable\"},\"error\":null}");return;}else if(rc){err(r,503,rc,"Bluetooth status is unavailable");return;}json_escape(state,sizeof(state),b.state);json_escape(transport,sizeof(transport),b.transport);json_escape(hci,sizeof(hci),b.hci);json_escape(local_name,sizeof(local_name),b.local_name);json_escape(error,sizeof(error),b.last_error);json_escape(disconnect_reason,sizeof(disconnect_reason),b.last_disconnect_reason);json_escape(connect_failed_status,sizeof(connect_failed_status),b.last_connect_failed_status);json_escape(profile_state,sizeof(profile_state),b.profile_state);json_escape(profile_error,sizeof(profile_error),b.profile_error);if(bluetooth_append(r->body,sizeof(r->body),&n,"{\"ok\":true,\"data\":{\"state\":\"%s\",\"available\":%s,\"enabled\":%s,\"activation_attempted\":%s,\"transport\":\"%s\",\"hci\":\"%s\",\"local_name\":\"%s\",\"last_error\":\"%s\",\"last_disconnect_reason\":\"%s\",\"last_connect_failed_status\":\"%s\",\"profile_state\":\"%s\",\"profile_error\":\"%s\",\"profile_services\":{\"sdp\":%s,\"a2dp_sink\":%s,\"avrcp\":%s,\"rfcomm\":%s,\"bnep\":%s,\"hidp\":%s},\"scanning\":%s,\"pairing\":%s,\"pairing_mode\":%s,\"capabilities\":{\"classic\":%s,\"le\":%s,\"ssp\":%s,\"secure_connection\":%s,\"connectable\":%s,\"discoverable\":%s,\"bondable\":%s},\"pending_pairing\":",state,b.available?"true":"false",b.enabled?"true":"false",b.activation_attempted?"true":"false",transport,hci,local_name,error,disconnect_reason,connect_failed_status,profile_state,profile_error,b.profile_sdp?"true":"false",b.profile_a2dp_sink?"true":"false",b.profile_avrcp?"true":"false",b.profile_rfcomm?"true":"false",b.profile_bnep?"true":"false",b.profile_hidp?"true":"false",b.scanning?"true":"false",b.pairing?"true":"false",b.pairing_mode?"true":"false",b.classic?"true":"false",b.le?"true":"false",b.ssp?"true":"false",b.secure_connection?"true":"false",b.connectable?"true":"false",b.discoverable?"true":"false",b.bondable?"true":"false")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}if(b.pairing){json_escape(name,sizeof(name),b.pending_pairing.address);{char method[48];json_escape(method,sizeof(method),b.pending_pairing.method);if(bluetooth_append(r->body,sizeof(r->body),&n,"{\"address\":\"%s\",\"type\":%d,\"method\":\"%s\",\"value\":%u}",name,b.pending_pairing.type,method,b.pending_pairing.value)){err(r,503,LE_IO,"Bluetooth status response is too large");return;}}}else if(bluetooth_append(r->body,sizeof(r->body),&n,"null")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}if(bluetooth_append(r->body,sizeof(r->body),&n,",\"discovered\":[")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}for(i=0;i<b.discovered_count&&i<LE_MAX_BLUETOOTH_DEVICES;i++){char address[32];json_escape(address,sizeof(address),b.discovered[i].address);json_escape(name,sizeof(name),b.discovered[i].name);if(bluetooth_append(r->body,sizeof(r->body),&n,"%s{\"address\":\"%s\",\"name\":\"%s\",\"type\":%d,\"rssi\":%d,\"rssi_valid\":%s,\"paired\":%s,\"connected\":%s}",i?",":"",address,name,b.discovered[i].type,b.discovered[i].rssi,b.discovered[i].rssi_valid?"true":"false",b.discovered[i].paired?"true":"false",b.discovered[i].connected?"true":"false")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}}if(bluetooth_append(r->body,sizeof(r->body),&n,"],\"known_devices\":[")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}for(i=0;i<b.known_count&&i<LE_MAX_BLUETOOTH_DEVICES;i++){char address[32];json_escape(address,sizeof(address),b.known[i].address);json_escape(name,sizeof(name),b.known[i].name);if(bluetooth_append(r->body,sizeof(r->body),&n,"%s{\"address\":\"%s\",\"name\":\"%s\",\"type\":%d,\"rssi\":%d,\"rssi_valid\":%s,\"connected\":%s}",i?",":"",address,name,b.known[i].type,b.known[i].rssi,b.known[i].rssi_valid?"true":"false",b.known[i].connected?"true":"false")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}}if(bluetooth_append(r->body,sizeof(r->body),&n,"]},\"error\":null}")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}r->status=200;strcpy(r->type,"application/json; charset=utf-8");r->length=n;}
static int auth_rate_limited(struct api_context*c){time_t now=time(0);if(!c->auth.enabled)return 0;if(c->auth_blocked_until>now)return 1;if(c->auth_blocked_until)c->auth_blocked_until=0;if(!c->auth_window_started||now-c->auth_window_started>=LE_AUTH_FAILURE_WINDOW){c->auth_window_started=now;c->auth_failures=0;}return c->auth_failures>=LE_AUTH_FAILURE_LIMIT;}
static void auth_record_failure(struct api_context*c){time_t now=time(0);if(!c->auth_window_started||now-c->auth_window_started>=LE_AUTH_FAILURE_WINDOW){c->auth_window_started=now;c->auth_failures=0;}if(c->auth_failures<LE_AUTH_FAILURE_LIMIT)c->auth_failures++;if(c->auth_failures>=LE_AUTH_FAILURE_LIMIT)c->auth_blocked_until=now+LE_AUTH_BLOCK_SECONDS;}
static void auth_users_json(struct api_context*c,struct api_response*r)
{
    char data[1024];
    size_t i,used=0;
    used+=(size_t)snprintf(data+used,sizeof(data)-used,"{\"users\":[");
    for(i=0;i<c->auth.user_count&&used<sizeof(data);i++){
        char escaped[LE_AUTH_USERNAME_MAX*2];
        json_escape(escaped,sizeof(escaped),c->auth.users[i].username);
        used+=(size_t)snprintf(data+used,sizeof(data)-used,"%s{\"username\":\"%s\"}",i?",":"",escaped);
    }
    used+=(size_t)snprintf(data+used,sizeof(data)-used,"],\"count\":%zu}",c->auth.user_count);
    if(used>=sizeof(data)){err(r,503,LE_IO,"User list is too large");return;}
    ok(r,data);
}
static int auth_credentials(const struct api_request*q,char*username,char*password,char*confirm)
{
    return json_get_string(q->body,"username",username,LE_AUTH_USERNAME_MAX)>0&&
           json_get_string(q->body,"password",password,LE_AUTH_PASSWORD_MAX+1)>0&&
           json_get_string(q->body,"password_confirm",confirm,LE_AUTH_PASSWORD_MAX+1)>0;
}
static void auth_bootstrap_json(struct api_context*c,const struct api_request*q,struct api_response*r)
{
    char username[LE_AUTH_USERNAME_MAX],password[LE_AUTH_PASSWORD_MAX+1],confirm[LE_AUTH_PASSWORD_MAX+1],token[LE_AUTH_TOKEN_MAX],escaped[LE_AUTH_USERNAME_MAX*2];
    int expires;
    if(!api_bootstrap_required_internal(c)){err(r,409,LE_BUSY,"Initial account setup has already been completed");return;}
    if(!auth_credentials(q,username,password,confirm)||strcmp(password,confirm)){err(r,400,LE_INVALID,"Username, password, and matching password confirmation are required");goto clear;}
    if(le_auth_add_user(&c->auth,c->users_path,username,password)||le_auth_login(&c->auth,username,password,token,sizeof(token),&expires)){err(r,400,LE_INVALID,"Account details are invalid or could not be saved");goto clear;}
    if(write_setup_marker(c,0)){err(r,503,LE_IO,"Account was created but setup state could not be reopened");goto clear;}
    c->setup_completed=0;
    json_escape(escaped,sizeof(escaped),username);
    api_log(c,"info","Initial local account created");
    out(r,200,"{\"ok\":true,\"data\":{\"token\":\"%s\",\"username\":\"%s\",\"expires_in\":%d},\"error\":null}",token,escaped,expires);
clear:
    memset(password,0,sizeof(password));
    memset(confirm,0,sizeof(confirm));
}
static void auth_add_user_json(struct api_context*c,const struct api_request*q,struct api_response*r)
{
    char username[LE_AUTH_USERNAME_MAX],password[LE_AUTH_PASSWORD_MAX+1],confirm[LE_AUTH_PASSWORD_MAX+1];
    if(!auth_credentials(q,username,password,confirm)||strcmp(password,confirm)){err(r,400,LE_INVALID,"Username, password, and matching password confirmation are required");goto clear;}
    if(le_auth_add_user(&c->auth,c->users_path,username,password)){err(r,409,LE_INVALID,"User could not be added");goto clear;}
    api_log(c,"info","Local user added");
    auth_users_json(c,r);
clear:
    memset(password,0,sizeof(password));
    memset(confirm,0,sizeof(confirm));
}
static void auth_remove_user_json(struct api_context*c,const struct api_request*q,struct api_response*r)
{
    const char*username=q->path+strlen("/api/v1/auth/users/");
    if(!*username||strchr(username,'/')||le_auth_remove_user(&c->auth,c->users_path,username)){err(r,409,LE_BUSY,"User could not be removed; at least one user must remain");return;}
    api_log(c,"warning","Local user removed");
    auth_users_json(c,r);
}
static void auth_login_json(struct api_context*c,const struct api_request*q,struct api_response*r){char username[LE_AUTH_USERNAME_MAX],password[LE_AUTH_PASSWORD_MAX+1],token[LE_AUTH_TOKEN_MAX],escaped[LE_AUTH_USERNAME_MAX*2];int expires;if(json_get_string(q->body,"username",username,sizeof(username))<1||json_get_string(q->body,"password",password,sizeof(password))<1){err(r,400,LE_INVALID,"Username and password are required");return;}if(auth_rate_limited(c)){memset(password,0,sizeof(password));err(r,429,LE_BUSY,"Too many login attempts; try again later");return;}if(le_auth_login(&c->auth,username,password,token,sizeof(token),&expires)!=0){auth_record_failure(c);memset(password,0,sizeof(password));if(auth_rate_limited(c))err(r,429,LE_BUSY,"Too many login attempts; try again later");else err(r,401,LE_AUTH,"Invalid username or password");return;}c->auth_failures=0;c->auth_window_started=0;c->auth_blocked_until=0;memset(password,0,sizeof(password));json_escape(escaped,sizeof(escaped),username);api_log(c,"info","Authenticated user session created");out(r,200,"{\"ok\":true,\"data\":{\"token\":\"%s\",\"username\":\"%s\",\"expires_in\":%d},\"error\":null}",token,escaped,expires);}
static void auth_current_json(struct api_context*c,const struct api_request*q,struct api_response*r){char username[LE_AUTH_USERNAME_MAX],escaped[LE_AUTH_USERNAME_MAX*2];if(!strncmp(q->authorization,"Bearer ",7)&&le_auth_session(&c->auth,q->authorization+7,username,sizeof(username))){json_escape(escaped,sizeof(escaped),username);out(r,200,"{\"ok\":true,\"data\":{\"authenticated\":true,\"username\":\"%s\"},\"error\":null}",escaped);}else if(c->auth_token[0])ok(r,"{\"authenticated\":true,\"username\":\"token\"}");else err(r,401,LE_AUTH,"Authentication is required");}
static int button_status_value(const char *data, const char *key, char *out, size_t size)
{
    char pattern[64];
    const char *start, *end;
    size_t n;
    if (!data || !key || !out || !size || snprintf(pattern, sizeof(pattern), "%s=", key) >= (int)sizeof(pattern)) return 0;
    start = strstr(data, pattern);
    if (!start) return 0;
    start += strlen(pattern);
    end = strchr(start, '\n');
    n = end ? (size_t)(end - start) : strlen(start);
    if (!n || n >= size) return 0;
    memcpy(out, start, n); out[n] = 0;
    return 1;
}

static void buttons_json(struct api_context *c, struct api_response *r)
{
    char data[512], state[32] = "unavailable", value[16];
    char escaped_short[80], escaped_long[80];
    struct stat st;
    time_t now = time(NULL);
    int fd, fresh = 0, volume = 0, mute = 0, action = 0;
    ssize_t n;
    json_escape(escaped_short, sizeof(escaped_short), c->button_short);
    json_escape(escaped_long, sizeof(escaped_long), c->button_long);
    fd = open("/run/libreecho/buttond-status", O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
        n = read(fd, data, sizeof(data) - 1); data[n > 0 ? n : 0] = 0; close(fd);
        fresh = n > 0 && now >= st.st_mtime && now - st.st_mtime <= 15;
        if (fresh && button_status_value(data, "state", state, sizeof(state))) {
            if (button_status_value(data, "volume", value, sizeof(value))) volume = !strcmp(value, "1");
            if (button_status_value(data, "microphone_mute", value, sizeof(value))) mute = !strcmp(value, "1");
            if (button_status_value(data, "action", value, sizeof(value))) action = !strcmp(value, "1");
        }
    } else if (fd >= 0) close(fd);
    if (!fresh) strcpy(state, "stale");
    out(r, 200, "{\"ok\":true,\"data\":{\"short_press\":\"%s\",\"long_press\":\"%s\",\"available\":%s,\"state\":\"%s\",\"volume_capable\":%s,\"hardware_mute\":%s,\"action_capable\":%s,\"stale\":%s},\"error\":null}", escaped_short, escaped_long, fresh && !strcmp(state, "connected") ? "true" : "false", state, volume ? "true" : "false", mute ? "true" : "false", action ? "true" : "false", !fresh ? "true" : "false");
}

static int timer_id_from_path(const char *path, unsigned *id)
{
    const char *text = path + strlen("/api/v1/timers/");
    const char *it;
    char *end;
    unsigned long value;

    if (!text[0])
        return 0;
    for (it = text; *it; ++it)
        if (!isdigit((unsigned char)*it))
            return 0;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end || value == 0 ||
        value > UINT_MAX)
        return 0;
    if (id)
        *id = (unsigned)value;
    return 1;
}

static void timers_json(struct api_context*c,struct api_response*r){
 struct le_timer_list list;char body[4096];char escaped[128];size_t used=0;int i,written,rc;
 rc=le_get_timers(c->backend,&list);
 if(rc!=LE_OK){if(rc!=LE_NOT_SUPPORTED){err(r,503,rc,"Timer service could not be read");return;}memset(&list,0,sizeof(list));}
 written=snprintf(body,sizeof(body),"{\"available\":%s,\"ringing\":%d,\"missed\":%d,\"timers\":[",list.available?"true":"false",list.ringing,list.missed);
 if(written<0||(size_t)written>=sizeof(body)){err(r,500,LE_IO,"Timer list could not be rendered");return;}
 used=(size_t)written;
 for(i=0;i<list.count;i++){
  json_escape(escaped,sizeof(escaped),list.items[i].label);
  written=snprintf(body+used,sizeof(body)-used,"%s{\"id\":%u,\"kind\":\"%s\",\"state\":\"%s\",\"seconds_remaining\":%ld,\"label\":\"%s\"}",i?",":"",list.items[i].id,list.items[i].kind,list.items[i].state,list.items[i].seconds_remaining,escaped);
  if(written<0||(size_t)written>=sizeof(body)-used){err(r,500,LE_IO,"Timer list could not be rendered");return;}
  used+=(size_t)written;}
 written=snprintf(body+used,sizeof(body)-used,"]}");
 if(written<0||(size_t)written>=sizeof(body)-used){err(r,500,LE_IO,"Timer list could not be rendered");return;}
 ok(r,body);}

void api_handle(struct api_context*c,const struct api_request*q,struct api_response*r){const char*p=q->path;int rc=LE_OK,v;if(!security(c,q,r))return;if((!strcmp(p,"/api/v1/timers")&&(strcmp(q->method,"GET")&&strcmp(q->method,"POST")))||(!strcmp(p,"/api/v1/timers/dismiss")&&strcmp(q->method,"POST"))||(!strncmp(p,"/api/v1/timers/",15)&&strcmp(p,"/api/v1/timers/dismiss")&&strcmp(q->method,"DELETE"))){method_not_allowed(r);return;}if((!strcmp(p,"/api/v1/buttons")&&strcmp(q->method,"GET")&&strcmp(q->method,"PUT"))||(!strcmp(p,"/api/v1/privacy")&&strcmp(q->method,"GET")&&strcmp(q->method,"PUT"))||(!strcmp(p,"/api/v1/integrations")&&strcmp(q->method,"GET"))||(!strncmp(p,"/api/v1/integrations/",21)&&strcmp(q->method,"PUT"))){method_not_allowed(r);return;}if(changing(q->method)&&q->body_len&&!body_ok(q,r))return;if(!strcmp(p,"/api/v1/auth/bootstrap")&&!strcmp(q->method,"POST")){auth_bootstrap_json(c,q,r);return;}if(!strcmp(p,"/api/v1/auth/login")&&!strcmp(q->method,"POST")){auth_login_json(c,q,r);return;}if(!strcmp(p,"/api/v1/auth/users")&& !strcmp(q->method,"GET")){auth_users_json(c,r);return;}if(!strcmp(p,"/api/v1/auth/users")&& !strcmp(q->method,"POST")){auth_add_user_json(c,q,r);return;}if(!strncmp(p,"/api/v1/auth/users/",strlen("/api/v1/auth/users/"))&& !strcmp(q->method,"DELETE")){auth_remove_user_json(c,q,r);return;}if((!strcmp(p,"/api/v1/auth/users")||!strncmp(p,"/api/v1/auth/users/",strlen("/api/v1/auth/users/")))&&strcmp(q->method,"GET")&&strcmp(q->method,"POST")&&strcmp(q->method,"DELETE")){method_not_allowed(r);return;}if(!strcmp(p,"/api/v1/auth")&&!strcmp(q->method,"GET")){auth_current_json(c,q,r);return;}if(!strcmp(p,"/api/v1/auth/logout")&&!strcmp(q->method,"POST")){if(!strncmp(q->authorization,"Bearer ",7))le_auth_logout(&c->auth,q->authorization+7);ok(r,"{\"logged_out\":true}");return;}
 if(!strcmp(p,"/api/v1/config/export")&&!strcmp(q->method,"GET")){char config[4096];rc=configuration_json(c,config,sizeof(config));if(rc)err(r,501,rc,"Configuration export is unavailable for this backend");else ok(r,config);return;}if(!strcmp(p,"/api/v1/config/import")&&!strcmp(q->method,"POST")){rc=import_configuration(c,q->body);if(rc)err(r,rc==LE_INVALID?400:501,rc,rc==LE_INVALID?"Configuration file is invalid or incomplete":"Configuration restore is unavailable for this backend");else{api_log(c,"warning","Configuration restored from uploaded JSON");ok(r,"{\"restored\":true,\"schema_version\":1}");}return;}
 if(!strcmp(p,"/api/v1/assistant")){if(!strcmp(q->method,"GET")){agent_result(r,"status",NULL);return;}if(!strcmp(q->method,"PUT")){agent_result(r,"configure",q->body);return;}method_not_allowed(r);return;}
 if(!strcmp(p,"/api/v1/assistant/auth/start")&&!strcmp(q->method,"POST")){agent_result(r,"auth_start",NULL);return;}
 if(!strcmp(p,"/api/v1/assistant/auth/poll")&&!strcmp(q->method,"POST")){agent_result(r,"auth_poll",NULL);return;}
 if(!strcmp(p,"/api/v1/assistant/logout")&&!strcmp(q->method,"POST")){agent_result(r,"logout",NULL);return;}
 if(!strcmp(p,"/api/v1/assistant/respond")&&!strcmp(q->method,"POST")){agent_result(r,"respond",q->body);return;}
 if((!strcmp(p,"/api/v1")||!strcmp(p,"/api/v1/"))&&!strcmp(q->method,"GET")){ok(r,"{\"name\":\"LibreEcho API\",\"version\":\"v1\",\"status\":\"/api/v1/status\",\"playback\":\"/api/v1/playback\",\"openapi\":\"/openapi.json\",\"swagger\":\"/swagger.html\"}");return;}if(!strcmp(p,"/api/v1/status")&&!strcmp(q->method,"GET")){status_json(c,r);return;}if(!strcmp(p,"/api/v1/playback")&&!strcmp(q->method,"GET")){playback_json(c,r);return;}if(!strcmp(p,"/api/v1/timers")&&!strcmp(q->method,"GET")){timers_json(c,r);return;}if(!strcmp(p,"/api/v1/timers")&&!strcmp(q->method,"POST")){int seconds_result,label_result;long long seconds=0;char label[48]="";unsigned id=0;if(json_duplicate_key(q->body,q->body_len,"seconds")||json_duplicate_key(q->body,q->body_len,"label")){err(r,400,LE_INVALID,"Timer fields must not be duplicated");return;}if((seconds_result=json_get_int64_top_level(q->body,"seconds",&seconds))!=1||seconds<LE_TIMER_MIN_SECONDS||seconds>LE_TIMER_MAX_SECONDS){err(r,400,LE_INVALID,"Seconds is required or out of range");return;}label_result=json_get_string_top_level(q->body,"label",label,sizeof(label));if(label_result<0||!valid_utf8(label,strlen(label))){err(r,400,LE_INVALID,"Timer label is invalid or too long");return;}rc=le_add_timer(c->backend,seconds,label,&id);if(rc==LE_INVALID){err(r,400,LE_INVALID,"Timer length is out of range");return;}if(rc==LE_BUSY){err(r,409,LE_BUSY,"Timer capacity is full");return;}if(rc!=LE_OK){err(r,503,rc,"Timer service is unavailable");return;}out(r,201,"{\"ok\":true,\"data\":{\"id\":%u},\"error\":null}",id);return;}if(!strcmp(p,"/api/v1/timers/dismiss")&&!strcmp(q->method,"POST")){int stopped=0;rc=le_dismiss_timers(c->backend,&stopped);if(rc!=LE_OK){err(r,503,rc,"Timer service is unavailable");return;}out(r,200,"{\"ok\":true,\"data\":{\"dismissed\":%d},\"error\":null}",stopped);return;}if(!strncmp(p,"/api/v1/timers/",15)&&!strcmp(q->method,"DELETE")){unsigned id;if(!timer_id_from_path(p,&id)){err(r,404,LE_INVALID,"Timer was not found");return;}rc=le_cancel_timer(c->backend,id);if(rc==LE_INVALID){err(r,404,LE_INVALID,"Timer was not found");return;}if(rc!=LE_OK){err(r,503,rc,"Timer service is unavailable");return;}ok(r,"{\"cancelled\":true}");return;}if(!strcmp(p,"/api/v1/device")&&!strcmp(q->method,"GET")){device_json(c,r);return;}if(!strcmp(p,"/api/v1/config")&&!strcmp(q->method,"GET")){out(r,200,"{\"ok\":true,\"data\":{\"api_version\":1,\"csrf_token\":\"%s\",\"authentication\":\"%s\",\"bootstrap_required\":%s,\"user_count\":%zu,\"bind_policy\":\"%s\",\"max_request_body\":16384},\"error\":null}",c->csrf_token,api_bootstrap_required_internal(c)?"bootstrap-required":c->auth.enabled?"users":c->auth_token[0]?"bearer-token":"development-disabled",api_bootstrap_required_internal(c)?"true":"false",c->auth.user_count,c->allow_insecure_lan?"lan-development":"loopback-default");return;}
 if(!strcmp(p,"/api/v1/audio")){if(!strcmp(q->method,"GET")){audio_json(c,r);return;}if(!strcmp(q->method,"PUT")){char voice[32];if(json_get_int(q->body,"volume",&v)>0)rc=le_set_volume(c->backend,v);if(!rc&&json_get_int(q->body,"microphone_gain",&v)>0)rc=le_set_microphone_gain(c->backend,v);if(!rc&&json_get_bool(q->body,"microphone_muted",&v)>0)rc=le_set_microphone_muted(c->backend,v);if(!rc&&json_get_string(q->body,"tts_voice",voice,sizeof(voice))>0)rc=le_set_tts_voice(c->backend,voice);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Audio setting could not be applied");return;}api_log(c,"info","Audio settings updated");event_bus_publish(&c->events,"audio","{\"changed\":true}");audio_json(c,r);return;}}
 if(!strcmp(p,"/api/v1/baby-monitor")&&!strcmp(q->method,"GET")){baby_monitor_json(c,r);return;}
 if(!strcmp(p,"/api/v1/bluetooth/pairing-mode")&&!strcmp(q->method,"POST")){int enabled=1;if(json_get_bool(q->body,"enabled",&enabled)<1)enabled=1;rc=le_bluetooth_set_pairing_mode(c->backend,enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Bluetooth pairing mode could not be changed");return;}api_log(c,"info",enabled?"Bluetooth pairing mode enabled":"Bluetooth pairing mode disabled");bluetooth_json(c,r);return;}
 if(!strcmp(p,"/api/v1/bluetooth")){if(!strcmp(q->method,"GET")){bluetooth_json(c,r);return;}if(!strcmp(q->method,"PUT")){int changed=0;if(json_get_bool(q->body,"enabled",&v)>0){rc=le_set_bluetooth_enabled(c->backend,v);changed=1;}else if(json_get_bool(q->body,"discoverable",&v)>0){rc=le_bluetooth_set_discoverable(c->backend,v);changed=1;}else if(json_get_bool(q->body,"connectable",&v)>0){rc=le_bluetooth_set_connectable(c->backend,v);changed=1;}if(!changed){err(r,400,LE_INVALID,"A Bluetooth setting is required");return;}if(rc){err(r,rc==LE_INVALID?400:501,rc,"Bluetooth setting could not be applied");return;}bluetooth_json(c,r);return;}method_not_allowed(r);return;}
 if(!strcmp(p,"/api/v1/bluetooth/scan")&&!strcmp(q->method,"POST")){rc=le_bluetooth_scan(c->backend,1);if(rc){err(r,rc==LE_BUSY?409:501,rc,"Bluetooth discovery could not start");return;}bluetooth_json(c,r);return;}
 if(!strcmp(p,"/api/v1/bluetooth/scan/stop")&&!strcmp(q->method,"POST")){rc=le_bluetooth_scan(c->backend,0);if(rc){err(r,501,rc,"Bluetooth discovery could not stop");return;}bluetooth_json(c,r);return;}
 if((!strcmp(p,"/api/v1/bluetooth/pair")||!strcmp(p,"/api/v1/bluetooth/unpair")||!strcmp(p,"/api/v1/bluetooth/disconnect"))&&!strcmp(q->method,"POST")){char address[24];int type=0,io_capability=3;if(json_get_string(q->body,"address",address,sizeof(address))<1){err(r,400,LE_INVALID,"Bluetooth address is required");return;}json_get_int(q->body,"type",&type);if(!strcmp(p,"/api/v1/bluetooth/pair")){json_get_int(q->body,"io_capability",&io_capability);rc=le_bluetooth_pair(c->backend,address,type,io_capability);}else if(!strcmp(p,"/api/v1/bluetooth/unpair"))rc=le_bluetooth_unpair(c->backend,address,type);else rc=le_bluetooth_disconnect(c->backend,address,type);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Bluetooth device operation failed");return;}bluetooth_json(c,r);return;}
 if(!strcmp(p,"/api/v1/bluetooth/pairing/response")&&!strcmp(q->method,"POST")){char address[24],method[16],pin[24];int type=0,value=0;if(json_get_string(q->body,"address",address,sizeof(address))<1||json_get_string(q->body,"method",method,sizeof(method))<1){err(r,400,LE_INVALID,"Pairing address and method are required");return;}json_get_int(q->body,"type",&type);json_get_int(q->body,"value",&value);pin[0]=0;json_get_string(q->body,"pin",pin,sizeof(pin));if(value<0){err(r,400,LE_INVALID,"Pairing value is invalid");return;}rc=le_bluetooth_pairing_response(c->backend,address,type,method,(unsigned int)value,pin);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Bluetooth pairing response failed");return;}bluetooth_json(c,r);return;}
 if(!strcmp(p,"/api/v1/audio/test")&&!strcmp(q->method,"POST")){rc=le_play_test_tone(c->backend);if(rc)err(r,501,rc,"Test tone could not be played");else ok(r,"{\"playing\":true}");return;}
 if(!strcmp(p,"/api/v1/audio/announce")&&!strcmp(q->method,"POST")){char text[512];if(json_get_string(q->body,"text",text,sizeof(text))<1){err(r,400,LE_INVALID,"Text is required");return;}rc=le_announce(c->backend,text);if(rc)err(r,rc==LE_INVALID?400:501,rc,"Announcement could not be spoken");else{api_log(c,"info","Announcement spoken");ok(r,"{\"speaking\":true}");}return;}
 if(!strcmp(p,"/api/v1/audio/announce/stop")&&!strcmp(q->method,"POST")){rc=le_stop_speech(c->backend);if(rc)err(r,501,rc,"Speech could not be stopped");else ok(r,"{\"speaking\":false}");return;}

 if(!strcmp(p,"/api/v1/audio/simulate")&&!strcmp(q->method,"POST")){
  char text[512];
  if(json_get_string(q->body,"text",text,sizeof(text))<1||!text[0]){err(r,400,LE_INVALID,"text is required");return;}
  rc=le_simulate_audio(c->backend,text);
  if(rc){err(r,rc==LE_INVALID?400:rc==LE_NOT_SUPPORTED?501:503,rc,
             rc==LE_INVALID?"text is empty or too long":
             rc==LE_NOT_SUPPORTED?"Audio simulation is not available on this image":
             "The phrase could not be rendered");return;}
  api_log(c,"info","Simulated audio queued for the microphone path");
  ok(r,"{\"queued\":true}");return;}
 if(!strcmp(p,"/api/v1/audio/noise")){
  if(!strcmp(q->method,"POST")){
   char colour[16]="white";int level=40,minutes=0;
   if(json_get_string(q->body,"colour",colour,sizeof(colour))<0){err(r,400,LE_INVALID,"colour must be white, pink or brown");return;}
   if(json_get_int(q->body,"level",&level)<0||json_get_int(q->body,"minutes",&minutes)<0){err(r,400,LE_INVALID,"level and minutes must be numbers");return;}
   rc=le_start_noise(c->backend,colour,level,minutes);
   if(rc){err(r,rc==LE_INVALID?400:501,rc,"colour must be white, pink or brown, level 1-100 and minutes 0-600");return;}
   api_log(c,"info","Sleep noise started");audio_json(c,r);return;}
  if(!strcmp(q->method,"DELETE")){
   rc=le_stop_noise(c->backend);
   if(rc){err(r,501,rc,"Sleep noise could not be stopped");return;}
   api_log(c,"info","Sleep noise stopped");audio_json(c,r);return;}}
 if(!strcmp(p,"/api/v1/led")){if(!strcmp(q->method,"GET")){led_json(c,r);return;}if(!strcmp(q->method,"PUT")){int rr,gg,bb,visualizer_enabled,visualizer_field=json_get_bool(q->body,"visualizer_enabled",&visualizer_enabled);if(visualizer_field<0)rc=LE_INVALID;if(!rc&&json_get_int(q->body,"r",&rr)>0&&json_get_int(q->body,"g",&gg)>0&&json_get_int(q->body,"b",&bb)>0){if(rr<0||rr>255||gg<0||gg>255||bb<0||bb>255)rc=LE_INVALID;else rc=le_set_led_colour(c->backend,(uint8_t)rr,(uint8_t)gg,(uint8_t)bb);}if(!rc&&json_get_int(q->body,"brightness",&v)>0)rc=le_set_led_brightness(c->backend,v);if(!rc&&visualizer_field>0)rc=le_set_led_visualizer_enabled(c->backend,visualizer_enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"LED setting could not be applied");return;}event_bus_publish(&c->events,"led","{\"changed\":true}");led_json(c,r);return;}}
 if(!strcmp(p,"/api/v1/led/test")&&!strcmp(q->method,"POST")){rc=le_run_led_test(c->backend);if(rc)err(r,501,rc,"LED test is not available");else ok(r,"{\"testing\":true}");return;}
 if(!strcmp(p,"/api/v1/system/timezone")){if(!strcmp(q->method,"PUT")){char tz[64];if(json_get_string(q->body,"timezone",tz,sizeof(tz))<1||!timezone_valid(tz)){err(r,400,LE_INVALID,"timezone must be a POSIX TZ string such as CST6CDT,M3.2.0,M11.1.0");return;}strcpy(c->timezone,tz);rc=persist_configuration(c);if(rc){err(r,503,rc,"Timezone could not be saved");return;}api_log(c,"info","Timezone updated");out(r,200,"{\"ok\":true,\"data\":{\"timezone\":\"%s\",\"applies\":\"after restart\"},\"error\":null}",c->timezone);return;}if(!strcmp(q->method,"GET")){out(r,200,"{\"ok\":true,\"data\":{\"timezone\":\"%s\"},\"error\":null}",c->timezone);return;}method_not_allowed(r);return;}
 if(!strcmp(p,"/api/v1/led/night")){if(!strcmp(q->method,"PUT")){int en,st,e2;if(json_get_bool(q->body,"enabled",&en)<1||json_get_int(q->body,"start_minute",&st)<1||json_get_int(q->body,"end_minute",&e2)<1||st<0||st>1439||e2<0||e2>1439){err(r,400,LE_INVALID,"Night mode needs enabled, start_minute and end_minute (0-1439)");return;}rc=le_set_led_night(c->backend,en,st,e2);if(rc){err(r,rc==LE_NOT_SUPPORTED?501:503,rc,"Night mode could not be applied");return;}api_log(c,"info","Night mode updated");led_json(c,r);return;}method_not_allowed(r);return;}
 if(!strcmp(p,"/api/v1/led/profile")&&!strcmp(q->method,"PUT")){char name[24];struct le_led_profile profile;memset(&profile,0,sizeof(profile));if(json_get_string(q->body,"name",name,sizeof(name))<1||json_get_int(q->body,"r",&v)<1||v<0||v>255){err(r,400,LE_INVALID,"A valid LED profile and colour are required");return;}profile.r=(uint8_t)v;if(json_get_int(q->body,"g",&v)<1||v<0||v>255){err(r,400,LE_INVALID,"LED colour is invalid");return;}profile.g=(uint8_t)v;if(json_get_int(q->body,"b",&v)<1||v<0||v>255){err(r,400,LE_INVALID,"LED colour is invalid");return;}profile.b=(uint8_t)v;profile.brightness=100;if(json_get_int(q->body,"brightness",&v)>0){if(v<0||v>100){err(r,400,LE_INVALID,"LED brightness is invalid");return;}profile.brightness=v;}rc=le_set_led_profile(c->backend,name,&profile);if(rc){err(r,rc==LE_INVALID?400:501,rc,"LED profile could not be saved");return;}event_bus_publish(&c->events,"led","{\"changed\":true}");led_json(c,r);return;}
 if(!strcmp(p,"/api/v1/network")){if(!strcmp(q->method,"GET")){net_json(c,r);return;}if(!strcmp(q->method,"PUT")){char host[64];int changed=0;if(json_get_string(q->body,"hostname",host,sizeof(host))>0){rc=le_set_hostname(c->backend,host);changed=1;if(rc){err(r,rc==LE_INVALID?400:501,rc,"Hostname could not be applied");return;}}if(json_get_bool(q->body,"ssh",&v)>0){c->net_ssh=v;changed=1;}if(json_get_bool(q->body,"api_lan",&v)>0){c->net_api_lan=v;changed=1;}if(!changed){err(r,400,LE_INVALID,"No supported network setting was supplied");return;}net_json(c,r);return;}}if(!strcmp(p,"/api/v1/network/wifi/scan")&&!strcmp(q->method,"GET")){struct le_wifi_scan s;rc=le_scan_wifi(c->backend,&s);if(rc){err(r,501,rc,"Wi-Fi scan failed");return;}if(wifi_scan_json(r,&s)<0){err(r,503,LE_IO,"Wi-Fi scan response is too large");return;}return;}
 if(!strcmp(p,"/api/v1/network/wifi/connect")&&!strcmp(q->method,"POST")){struct le_wifi_credentials w;int security_result;memset(&w,0,sizeof(w));if(json_get_string(q->body,"ssid",w.ssid,sizeof(w.ssid))<1){err(r,400,LE_INVALID,"SSID is required");return;}json_get_string(q->body,"password",w.password,sizeof(w.password));security_result=json_get_string(q->body,"security",w.security,sizeof(w.security));if(security_result<0||(security_result>0&&!valid_wifi_security(w.security))){memset(w.password,0,sizeof(w.password));err(r,400,LE_INVALID,"security must be open or wpa2");return;}if(security_result==0)strcpy(w.security,"wpa2");rc=le_connect_wifi(c->backend,&w);memset(w.password,0,sizeof(w.password));if(rc){err(r,rc==LE_INVALID?400:503,rc,"Wi-Fi connection failed");return;}event_bus_publish(&c->events,"network","{\"state\":\"connecting\"}");ok(r,"{\"state\":\"connecting\"}");return;}if(!strcmp(p,"/api/v1/network/wifi/disconnect")&&!strcmp(q->method,"POST")){rc=le_disconnect_wifi(c->backend);if(rc)err(r,501,rc,"Wi-Fi disconnect is unavailable");else ok(r,"{\"state\":\"disconnected\"}");return;}
 if(!strcmp(p,"/api/v1/wake-word")){if(!strcmp(q->method,"GET")){wake_json(c,r);return;}if(!strcmp(q->method,"PUT")){char s[64];if(json_get_string(q->body,"wake_word",s,sizeof(s))>0)rc=le_set_wake_word(c->backend,s);if(!rc&&json_get_int(q->body,"sensitivity",&v)>0)rc=le_set_wake_word_sensitivity(c->backend,v);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Wake-word setting could not be applied");return;}/* The boot-time restore already applies wake_sensitivity, but nothing ever wrote it back, so every reboot silently reverted the wake word and sensitivity to the daemon defaults. */rc=persist_configuration(c);if(rc){err(r,503,rc,"Wake-word setting could not be saved");return;}wake_json(c,r);return;}}if(!strcmp(p,"/api/v1/wake-word/test")&&!strcmp(q->method,"POST")){rc=le_test_wake_word(c->backend);if(rc)err(r,409,rc,"Wake-word trigger failed");else ok(r,"{\"detected\":true}");return;}
 if(!strcmp(p,"/api/v1/buttons")){if(!strcmp(q->method,"PUT")){char short_action[32],long_action[32];if(json_get_string(q->body,"short_press",short_action,sizeof(short_action))>0)strcpy(c->button_short,short_action);if(json_get_string(q->body,"long_press",long_action,sizeof(long_action))>0)strcpy(c->button_long,long_action);}buttons_json(c,r);return;}
 if(!strcmp(p,"/api/v1/privacy")&&!strcmp(q->method,"PUT")&&json_get_int(q->body,"log_retention_hours",&v)>0&&(v==24||v==168||v==720))c->privacy_log_hours=v;
 if(!strcmp(p,"/api/v1/privacy")){if(!strcmp(q->method,"PUT")){if(json_get_bool(q->body,"local_only",&v)>0)c->privacy_local_only=v;if(json_get_bool(q->body,"diagnostic_telemetry",&v)>0)c->privacy_telemetry=v;if(json_get_bool(q->body,"crash_reports",&v)>0)c->privacy_crash_reports=v;{char retention[16];if(json_get_string(q->body,"audio_retention",retention,sizeof(retention))>0)c->privacy_audio_retention=strcmp(retention,"none")!=0;}}out(r,200,"{\"ok\":true,\"data\":{\"local_only\":%s,\"audio_retention\":\"%s\",\"diagnostic_telemetry\":%s,\"log_retention_hours\":%d,\"crash_reports\":%s},\"error\":null}",c->privacy_local_only?"true":"false",c->privacy_audio_retention?"24h":"none",c->privacy_telemetry?"true":"false",c->privacy_log_hours,c->privacy_crash_reports?"true":"false");return;}if(!strcmp(p,"/api/v1/buttons")){ok(r,"{\"short_press\":\"start_listening\",\"long_press\":\"pairing_mode\",\"hardware_mute\":true}");return;}if(!strncmp(p,"/api/v1/integrations",20)){if(!strcmp(q->method,"PUT")){int enabled;if(json_get_bool(q->body,"enabled",&enabled)<1){err(r,400,LE_INVALID,"Enabled must be a boolean");return;}if(strstr(p,"home-assistant")){if(enabled)c->integrations|=1u;else c->integrations&=~1u;}else if(strstr(p,"mqtt")){if(enabled)c->integrations|=2u;else c->integrations&=~2u;}else if(strstr(p,"rest")){c->net_api_lan=enabled;}else if(strstr(p,"airplay2")){rc=le_set_airplay_enabled(c->backend,enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"AirPlay 2 control could not be applied");return;}if(enabled)c->integrations|=16u;else c->integrations&=~16u;}else if(strstr(p,"bluetooth")){rc=le_set_bluetooth_enabled(c->backend,enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Bluetooth control could not be applied");return;}if(enabled)c->integrations|=8u;else c->integrations&=~8u;}else{err(r,404,LE_INVALID,"Integration was not found");return;}}out(r,200,"{\"ok\":true,\"data\":{\"items\":[{\"id\":\"home-assistant\",\"name\":\"Home Assistant\",\"enabled\":%s},{\"id\":\"mqtt\",\"name\":\"MQTT\",\"enabled\":%s},{\"id\":\"rest\",\"name\":\"Local REST API\",\"enabled\":%s,\"forced\":%s},{\"id\":\"bluetooth\",\"name\":\"Bluetooth audio\",\"enabled\":%s},{\"id\":\"airplay2\",\"name\":\"AirPlay 2\",\"enabled\":%s}]},\"error\":null}",(c->integrations&1u)?"true":"false",(c->integrations&2u)?"true":"false",(c->allow_insecure_lan||c->net_api_lan)?"true":"false",c->allow_insecure_lan?"true":"false",(c->integrations&8u)?"true":"false",(c->integrations&16u)?"true":"false");return;}
 if(!strcmp(p,"/api/v1/system")&&!strcmp(q->method,"GET")){system_json(c,r);return;}
 if(!strcmp(p,"/api/v1/provenance")&&!strcmp(q->method,"GET")){provenance_json(r);return;}
 if(!strcmp(p,"/api/v1/system/update")&&!strcmp(q->method,"GET")){update_status_json(c,r);return;}
 if(!strcmp(p,"/api/v1/system/update/automatic")){int enabled;if(strcmp(q->method,"PUT")){method_not_allowed(r);return;}if(!update_fetch_supported(c)){err(r,501,LE_NOT_SUPPORTED,"GitHub OTA updates are unavailable on this image");return;}if(json_get_bool(q->body,"enabled",&enabled)<1){err(r,400,LE_INVALID,"Enabled must be a boolean");return;}rc=run_init_command(update_fetch_path(),enabled?"enable-automatic":"disable-automatic");if(rc){err(r,503,rc,"Automatic update preference could not be saved");return;}api_log(c,"info",enabled?"Automatic OTA installation enabled":"Automatic OTA installation disabled");update_status_json(c,r);return;}
 if(!strcmp(p,"/api/v1/system/update/channel")){char channel[16];if(!api_update_channel_authorize(c,q,r,channel,sizeof(channel)))return;if(run_init_command(update_fetch_path(),!strcmp(channel,"dev")?"set-channel-dev":"set-channel-stable")){err(r,503,LE_IO,"Update channel could not be saved");return;}api_log(c,"info","OTA update channel changed");update_status_json(c,r);return;}
 if((!strcmp(p,"/api/v1/system/reboot")||!strcmp(p,"/api/v1/system/shutdown")||!strcmp(p,"/api/v1/system/factory-reset"))&&!strcmp(q->method,"POST")){if(strcmp(q->confirm,"confirm-device-action")){err(r,403,LE_AUTH,"A valid destructive-action confirmation token is required");return;}rc=strstr(p,"factory-reset")?le_factory_reset(c->backend):strstr(p,"shutdown")?le_shutdown(c->backend):le_reboot(c->backend);if(rc){err(r,501,rc,"Device action is not available");return;}api_log(c,"warning",p);ok(r,"{\"accepted\":true}");return;}
 if(!strcmp(p,"/api/v1/logs")&&!strcmp(q->method,"GET")){logs_json(c,r);return;}if(!strcmp(p,"/api/v1/logs/stream")&&!strcmp(q->method,"GET")){logs_stream_json(c,r);return;}if(!strcmp(p,"/api/v1/diagnostics")&&!strcmp(q->method,"GET")){diagnostics_json(c,r);return;}if(!strcmp(p,"/api/v1/diagnostics/export")){if(strcmp(q->method,"POST")){method_not_allowed(r);return;}diagnostics_export_json(c,r);return;}
 if(!strcmp(p,"/api/v1/events")&&!strcmp(q->method,"GET")){struct le_event e[8];size_t n,i,used=0;n=event_bus_since(&c->events,0,e,8);r->status=200;strcpy(r->type,"text/event-stream");for(i=0;i<n;i++)used+=(size_t)snprintf(r->body+used,sizeof(r->body)-used,"id: %lu\nevent: %s\ndata: %s\n\n",e[i].id,e[i].type,e[i].data);used+=(size_t)snprintf(r->body+used,sizeof(r->body)-used,"event: status\ndata: {\"refresh\":true}\n\n");r->length=used;return;}
#ifdef LE_DEV_CONTROLS
 if(!strcmp(p,"/api/v1/dev/mock")&&!strcmp(q->method,"POST")&&c->dev_controls){char a[64],v2[128];if(json_get_string(q->body,"action",a,sizeof(a))<1){err(r,400,LE_INVALID,"Mock action is required");return;}if(json_get_string(q->body,"value",v2,sizeof(v2))<1)v2[0]=0;rc=le_backend_mock_control(c->backend,a,v2);if(rc)err(r,400,rc,"Mock control rejected");else ok(r,"{\"applied\":true}");return;}
#endif
 err(r,404,LE_INVALID,"API endpoint was not found");}

#undef api_handle
static int handle_voice_pipeline(struct api_context *c,
                                 const struct api_request *q,
                                 struct api_response *r)
{
    int rc;

    if (strcmp(q->path, "/api/v1/voice-pipeline"))
        return 0;
    if (!strcmp(q->method, "GET")) {
        voice_pipeline_json(c, r);
        return 1;
    }
    if (strcmp(q->method, "PUT")) {
        method_not_allowed(r);
        return 1;
    }
    if (!q->body || !q->body_len) {
        err(r, 400, LE_INVALID,
            "Voice pipeline configuration is required");
        return 1;
    }
    rc = voice_pipeline_update(c, q->body);
    if (rc) {
        err(r, 400, rc,
            "Voice pipeline mode, endpoints, model, or voice is invalid");
        return 1;
    }
    rc = persist_configuration(c);
    if (!rc)
        rc = apply_voice_pipeline_mode(c->voice_pipeline_mode);
    if (rc) {
        err(r, rc == LE_NOT_SUPPORTED ? 501 : 503, rc,
            "Voice pipeline configuration could not be applied");
        return 1;
    }
    api_log(c, "info", "Voice pipeline configuration applied");
    voice_pipeline_json(c, r);
    return 1;
}
static void privacy_json(struct api_context *c, struct api_response *r)
{
    const char *effective, *state, *state_error;
    char url[512], error[256];
    state=audio_retention_state(c,&effective,&state_error);
    json_escape(url,sizeof(url),c->privacy_audio_remote_url);
    json_escape(error,sizeof(error),state_error);
    out(r,200,"{\"ok\":true,\"data\":{\"local_only\":%s,\"audio_retention\":\"%s\",\"audio_retention_mode\":\"%s\",\"audio_retention_hours\":%d,\"audio_retention_max_mb\":%d,\"audio_remote_destination\":{\"configured\":%s,\"url\":\"%s\",\"transport\":\"https-post\",\"authentication\":\"required-out-of-band\",\"credential_state\":\"not-configured\",\"available\":%s,\"state\":\"%s\",\"effective_mode\":\"%s\",\"fallback\":\"disabled\",\"last_error\":\"%s\"},\"diagnostic_telemetry\":%s,\"log_retention_hours\":%d,\"crash_reports\":%s},\"error\":null}",c->privacy_local_only?"true":"false",c->privacy_audio_mode,c->privacy_audio_mode,c->privacy_audio_retention_hours,c->privacy_audio_max_mb,c->privacy_audio_remote_url[0]?"true":"false",url,remote_retention_transport_available(c)?"true":"false",state,effective,error,c->privacy_telemetry?"true":"false",c->privacy_log_hours,c->privacy_crash_reports?"true":"false");
}
static int handle_privacy(struct api_context *c,
                          const struct api_request *q,
                          struct api_response *r)
{
    char mode[16], remote_url[256];
    const char *effective_mode, *effective_remote_url;
    int parsed, value, hours, max_mb;
    int mode_present=0, hours_present=0, max_present=0, url_present=0;
    int rc;
    if (strcmp(q->path, "/api/v1/privacy"))
        return 0;
    if (!strcmp(q->method, "GET")) {
        privacy_json(c,r);
        return 1;
    }
    if (strcmp(q->method, "PUT")) {
        method_not_allowed(r);
        return 1;
    }
    parsed=json_get_bool(q->body,"local_only",&value);
    if(parsed<0){err(r,400,LE_INVALID,"local_only must be a boolean");return 1;}
    if(parsed>0&&value&&!strcmp(c->voice_pipeline_mode,"custom")){err(r,409,LE_BUSY,"Disable the network voice pipeline before requiring local processing");return 1;}
    parsed=json_get_bool(q->body,"diagnostic_telemetry",&value);
    if(parsed<0){err(r,400,LE_INVALID,"diagnostic_telemetry must be a boolean");return 1;}
    if(parsed>0)c->privacy_telemetry=value;
    parsed=json_get_bool(q->body,"crash_reports",&value);
    if(parsed<0){err(r,400,LE_INVALID,"crash_reports must be a boolean");return 1;}
    if(parsed>0)c->privacy_crash_reports=value;
    parsed=json_get_string(q->body,"audio_retention",mode,sizeof(mode));
    if(parsed<0){err(r,400,LE_INVALID,"audio_retention must be none, local, or remote");return 1;}
    if(parsed>0){if(!strcmp(mode,"24h"))strcpy(mode,"local");if(!audio_retention_mode_valid(mode)){err(r,400,LE_INVALID,"audio_retention must be none, local, or remote");return 1;}mode_present=1;}
    parsed=json_get_string(q->body,"audio_retention_mode",mode,sizeof(mode));
    if(parsed<0||(parsed>0&&!audio_retention_mode_valid(mode))){err(r,400,LE_INVALID,"audio_retention_mode must be none, local, or remote");return 1;}
    if(parsed>0)mode_present=1;
    parsed=json_get_int(q->body,"audio_retention_hours",&hours);
    if(parsed<0||(parsed>0&&!audio_retention_hours_valid(hours))){err(r,400,LE_INVALID,"audio_retention_hours must be 24, 168, or 720");return 1;}
    if(parsed>0)hours_present=1;
    parsed=json_get_int(q->body,"audio_retention_max_mb",&max_mb);
    if(parsed<0||(parsed>0&&!audio_retention_max_mb_valid(max_mb))){err(r,400,LE_INVALID,"audio_retention_max_mb must be 16, 64, or 256");return 1;}
    if(parsed>0)max_present=1;
    parsed=json_get_string(q->body,"audio_remote_url",remote_url,sizeof(remote_url));
    if(parsed<0||(parsed>0&&remote_url[0]&&!audio_remote_url_valid(remote_url))){err(r,400,LE_INVALID,"audio_remote_url must be an HTTPS URL without embedded credentials");return 1;}
    if(parsed>0)url_present=1;
    effective_mode=mode_present?mode:c->privacy_audio_mode;
    effective_remote_url=url_present?remote_url:c->privacy_audio_remote_url;
    if(!strcmp(effective_mode,"remote")&&!effective_remote_url[0]){err(r,400,LE_INVALID,"A remote HTTPS destination is required for remote retention");return 1;}
    if(parsed>0&&url_present)snprintf(c->privacy_audio_remote_url,sizeof(c->privacy_audio_remote_url),"%s",remote_url);
    if(mode_present)snprintf(c->privacy_audio_mode,sizeof(c->privacy_audio_mode),"%s",mode);
    if(hours_present)c->privacy_audio_retention_hours=hours;
    if(max_present)c->privacy_audio_max_mb=max_mb;
    if(mode_present)c->privacy_audio_retention=strcmp(c->privacy_audio_mode,"none")!=0;
    parsed=json_get_int(q->body,"log_retention_hours",&value);
    if(parsed<0||(parsed>0&&!audio_retention_hours_valid(value))){err(r,400,LE_INVALID,"log_retention_hours must be 24, 168, or 720");return 1;}
    if(parsed>0)c->privacy_log_hours=value;
    parsed=json_get_bool(q->body,"local_only",&value);
    if(parsed>0)c->privacy_local_only=value;
    rc=persist_configuration(c);
    if(rc){err(r,503,rc,"Privacy configuration could not be saved");return 1;}
    privacy_json(c,r);
    return 1;
}
static void after_integration_change(struct api_context *c,
                                     const struct api_request *q,
                                     struct api_response *r)
{
    int enabled;
    int rc;

    if (strcmp(q->method, "PUT") ||
        strncmp(q->path, "/api/v1/integrations/", 21) ||
        r->status != 200)
        return;
    rc = persist_configuration(c);
    if (!rc && strstr(q->path, "home-assistant") &&
        json_get_bool(q->body, "enabled", &enabled) == 1)
        rc = apply_home_assistant_mode(enabled);
    if (rc)
        err(r, 503, rc, "Integration configuration could not be applied");
}
#define api_handle_inner(c,q,r) do { \
    if (!handle_voice_pipeline((c),(q),(r)) && \
        !handle_privacy((c),(q),(r))) { \
        api_handle_inner((c),(q),(r)); \
        after_integration_change((c),(q),(r)); \
    } \
} while (0)
void api_handle(struct api_context*c,const struct api_request*q,struct api_response*r){int rc;if(!security(c,q,r))return;if(changing(q->method)&&q->body_len&&!body_ok(q,r))return;if(!strcmp(q->path,"/api/v1/setup/vendor-import-force-next-boot")){setup_force_next_boot(c,q,r);return;}if(!strcmp(q->path,"/api/v1/setup")){if(!strcmp(q->method,"GET")){setup_json(c,r);return;}if(!strcmp(q->method,"POST")){rc=setup_apply(c,q->body);if(rc){err(r,rc==LE_BUSY?409:rc==LE_INVALID?400:rc==LE_NOT_SUPPORTED?501:503,rc,rc==LE_BUSY?"Initial setup has already been completed":rc==LE_INVALID?"Setup details are invalid or incomplete":rc==LE_NOT_SUPPORTED?"A required hardware adapter is not available":setup_failure_message(c));return;}api_log(c,"info","Initial setup completed; access-point handoff requested");event_bus_publish(&c->events,"device_state","{\"setup_completed\":true}");ok(r,"{\"completed\":true,\"network_state\":\"connecting\",\"ap_mode_exit_required\":true,\"password_stored\":false}");return;}method_not_allowed(r);return;}if((!strcmp(q->path,"/api/v1")||!strcmp(q->path,"/api/v1/"))&&!strcmp(q->method,"GET")){ok(r,"{\"name\":\"LibreEcho API\",\"version\":\"v1\",\"status\":\"/api/v1/status\",\"playback\":\"/api/v1/playback\",\"setup\":\"/api/v1/setup\",\"openapi\":\"/openapi.json\",\"swagger\":\"/swagger.html\"}");return;}if(!strcmp(q->path,"/api/v1/config")&&!strcmp(q->method,"GET")){out(r,200,"{\"ok\":true,\"data\":{\"api_version\":1,\"csrf_token\":\"%s\",\"authentication\":\"%s\",\"bootstrap_required\":%s,\"user_count\":%zu,\"bind_policy\":\"%s\",\"max_request_body\":16384,\"setup_completed\":%s,\"setup_url\":\"/setup.html\"},\"error\":null}",c->csrf_token,api_bootstrap_required_internal(c)?"bootstrap-required":c->auth.enabled?"users":c->auth_token[0]?"bearer-token":"development-disabled",api_bootstrap_required_internal(c)?"true":"false",c->auth.user_count,c->allow_insecure_lan?"lan-development":"loopback-default",c->setup_completed?"true":"false");return;}api_handle_inner(c,q,r);}

int api_persist_configuration(struct api_context*c){return persist_configuration(c);}
int api_baby_monitor_stream_authorize(struct api_context*c,const struct api_request*q,struct api_response*r,int*card,int*device,int*channels,int*bits,int*channel){const char*prefix="/api/v1/baby-monitor/stream?source=";const char*value;char source[32],pcm[64],extra;size_t n;unsigned parsed_card,parsed_device,parsed_channel=0;if(!c||!q||!r||!card||!device||!channels||!bits||!channel)return 0;if(!security(c,q,r))return 0;if(strcmp(q->method,"GET")){method_not_allowed(r);return 0;}if(strncmp(q->path,prefix,strlen(prefix))){err(r,404,LE_INVALID,"Baby-monitor stream was not found");return 0;}value=q->path+strlen(prefix);n=strcspn(value,"&");if(n==0||query_component_decode(source,sizeof(source),value,n)){err(r,400,LE_INVALID,"A valid microphone source is required");return 0;}if(sscanf(source,"%u:%u%c",&parsed_card,&parsed_device,&extra)!=2||parsed_card>31||parsed_device>31){err(r,400,LE_INVALID,"A valid microphone source is required");return 0;}if(n&&value[n]){const char*ch=strstr(value+n,"&channel=");if(ch&&sscanf(ch+9,"%u%c",&parsed_channel,&extra)!=1){err(r,400,LE_INVALID,"A valid microphone channel is required");return 0;}}if(!strcmp(le_backend_mode(c->backend),"mock")){err(r,501,LE_NOT_SUPPORTED,"Microphone streaming is unavailable in the mock backend");return 0;}if(parsed_card!=0||parsed_device!=24||parsed_channel>6){err(r,400,LE_INVALID,"The selected Echo microphone channel is invalid");return 0;}snprintf(pcm,sizeof(pcm),"/dev/snd/pcmC%uD%uc",parsed_card,parsed_device);if(access(pcm,R_OK)){err(r,503,LE_NOT_SUPPORTED,"Selected microphone source is unavailable");return 0;}*card=(int)parsed_card;*device=(int)parsed_device;*channels=9;*bits=24;*channel=(int)parsed_channel;return 1;}
