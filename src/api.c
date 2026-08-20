#include "version.h"
#include "api.h"
#include "adapter/adapter.h"
#include "adapter/wyoming_client.h"
#include "config_store.h"
#include "json.h"
#include "logd.h"
#include "log.h"
#include <ctype.h>
#include <errno.h>
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
    int progress=0,pending=0,automatic=0;
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
    json_escape(escaped_installed,sizeof(escaped_installed),installed_version);
    json_escape(escaped_rollback,sizeof(escaped_rollback),rollback_version);
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
        "\"max_upload_bytes\":33554432},"
        "\"error\":null}",supported?"true":"false",current,inactive,escaped_state,
        progress,pending?"true":"false",escaped_version,escaped_installed,
        escaped_latest,escaped_channel,escaped_source,escaped_reachable,
        escaped_check_status,escaped_check_error,last_check,last_success,
        automatic?"true":"false",supported?"true":"false",escaped_rollback);
}
static int agent_command(const char*command,const char*args,char*output,size_t size){struct le_adapter*agent=le_adapter_connect(LE_ADAPTER_AGENT_SOCK,15000);int rc;if(!agent)return LE_NOT_SUPPORTED;rc=le_adapter_call(agent,command,args,output,size);le_adapter_close(agent);return rc==LE_ADAPTER_OK?LE_OK:rc==LE_ADAPTER_ERR_REJECTED?LE_INVALID:LE_IO;}
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
        "\"processing\":\"on-device\"}},\"error\":null}",
        c->voice_pipeline_mode, custom ? "wyoming" : "sherpa",
        stt_uri, stt_model, c->stt_wyoming_uri[0] ? "true" : "false",
        stt_reachable ? "true" : "false",
        custom ? "wyoming" : "sherpa", tts_uri, tts_voice,
        c->tts_wyoming_uri[0] ? "true" : "false",
        tts_reachable ? "true" : "false");
}
static int voice_pipeline_update(struct api_context *c, const char *json)
{
    char mode[sizeof(c->voice_pipeline_mode)];
    char stt_uri[sizeof(c->stt_wyoming_uri)];
    char stt_model[sizeof(c->stt_wyoming_model)];
    char tts_uri[sizeof(c->tts_wyoming_uri)];
    char tts_voice[sizeof(c->tts_wyoming_voice)];

    ensure_voice_pipeline_config(c);
    snprintf(mode, sizeof(mode), "%s", c->voice_pipeline_mode);
    snprintf(stt_uri, sizeof(stt_uri), "%s", c->stt_wyoming_uri);
    snprintf(stt_model, sizeof(stt_model), "%s", c->stt_wyoming_model);
    snprintf(tts_uri, sizeof(tts_uri), "%s", c->tts_wyoming_uri);
    snprintf(tts_voice, sizeof(tts_voice), "%s", c->tts_wyoming_voice);
    (void)json_get_string(json, "mode", mode, sizeof(mode));
    (void)json_get_string(json, "stt_wyoming_uri",
                          stt_uri, sizeof(stt_uri));
    (void)json_get_string(json, "stt_model",
                          stt_model, sizeof(stt_model));
    (void)json_get_string(json, "tts_wyoming_uri",
                          tts_uri, sizeof(tts_uri));
    (void)json_get_string(json, "tts_voice",
                          tts_voice, sizeof(tts_voice));
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
static int hex_digit(unsigned char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static int query_component_decode(char*outbuf,size_t out_size,const char*value,size_t value_len){size_t i=0,o=0;if(!outbuf||!out_size||!value)return -1;while(i<value_len){unsigned char c=(unsigned char)value[i++];if(c=='%'){int hi,lo;if(i+1>=value_len||(hi=hex_digit((unsigned char)value[i]))<0||(lo=hex_digit((unsigned char)value[i+1]))<0)return -1;c=(unsigned char)((hi<<4)|lo);i+=2;if(!c)return -1;}else if(c=='+')c=' ';if(o+1>=out_size)return -1;outbuf[o++]=(char)c;}outbuf[o]='\0';return 0;}
static void setup_json(struct api_context*c,struct api_response*r){struct le_audio_state a;struct le_network_state n;struct le_wake_word_state w;char host[128],wake[128],ssid[LE_TEXT*2],state[48];int rc;if((rc=le_get_audio_state(c->backend,&a))||(rc=le_get_network_state(c->backend,&n))||(rc=le_get_wake_word_state(c->backend,&w))){err(r,503,rc,"Setup state is unavailable");return;}json_escape(host,sizeof(host),n.hostname);json_escape(wake,sizeof(wake),w.wake_word);json_escape(ssid,sizeof(ssid),n.ssid);json_escape(state,sizeof(state),n.state);out(r,200,"{\"ok\":true,\"data\":{\"completed\":%s,\"mode\":\"first-boot-ap\",\"backend\":\"%s\",\"hostname\":\"%s\",\"volume\":%d,\"wake_word\":\"%s\",\"wake_sensitivity\":%d,\"local_only\":%s,\"diagnostic_telemetry\":%s,\"network_state\":\"%s\",\"ssid\":\"%s\",\"password_stored\":false},\"error\":null}",c->setup_completed?"true":"false",le_backend_mode(c->backend),host,a.volume,wake,w.sensitivity,c->privacy_local_only?"true":"false",c->privacy_telemetry?"true":"false",state,ssid);}
static int setup_apply(struct api_context*c,const char*j){struct le_wifi_credentials wifi;char hostname[LE_TEXT],wake[LE_TEXT];int volume,sensitivity,local_only,telemetry,rc;memset(&wifi,0,sizeof(wifi));if(c->setup_completed)return LE_BUSY;if(json_get_string(j,"hostname",hostname,sizeof(hostname))<1||!valid_hostname(hostname)||json_get_string(j,"ssid",wifi.ssid,sizeof(wifi.ssid))<1||!wifi.ssid[0]||json_get_string(j,"security",wifi.security,sizeof(wifi.security))<1||json_get_string(j,"password",wifi.password,sizeof(wifi.password))<1||json_get_int(j,"volume",&volume)<1||volume<0||volume>100||json_get_string(j,"wake_word",wake,sizeof(wake))<1||!wake[0]||json_get_int(j,"wake_sensitivity",&sensitivity)<1||sensitivity<0||sensitivity>100||json_get_bool(j,"local_only",&local_only)<1||json_get_bool(j,"diagnostic_telemetry",&telemetry)<1){memset(wifi.password,0,sizeof(wifi.password));return LE_INVALID;}if(strcmp(wifi.security,"open")&&strlen(wifi.password)<8){memset(wifi.password,0,sizeof(wifi.password));return LE_INVALID;}if(strcmp(wifi.security,"open")&&strcmp(wifi.security,"wpa2")&&strcmp(wifi.security,"wpa3")){memset(wifi.password,0,sizeof(wifi.password));return LE_INVALID;}if((rc=le_set_hostname(c->backend,hostname))||(rc=le_set_volume(c->backend,volume))||(rc=le_set_wake_word(c->backend,wake))||(rc=le_set_wake_word_sensitivity(c->backend,sensitivity))||(rc=le_connect_wifi(c->backend,&wifi))){memset(wifi.password,0,sizeof(wifi.password));return rc;}memset(wifi.password,0,sizeof(wifi.password));c->privacy_local_only=local_only;c->privacy_telemetry=telemetry;c->setup_completed=1;rc=persist_configuration(c);if(rc)c->setup_completed=0;return rc;}
int api_apply_persisted_configuration(struct api_context *c)
{
    char saved[16384], text[LE_TEXT];
    int value, red, green, blue, brightness;
    int hostname_persisted = 0;
    int rc, failed = 0;

    if (!c || !c->backend || strcmp(le_backend_mode(c->backend), "linux") ||
        geteuid() != 0 || access("/proc/idme/serial", R_OK) != 0 ||
        !c->config_path[0] ||
        config_read(c->config_path, saved, sizeof(saved)) <= 0)
        return LE_OK;

#define APPLY_CONFIG(call) do { \
    rc = (call); \
    if (rc != LE_OK && rc != LE_NOT_SUPPORTED) failed = 1; \
} while (0)

    if (json_get_int(saved, "volume", &value) > 0 &&
        value >= 0 && value <= 100)
        APPLY_CONFIG(le_set_volume(c->backend, value));
    if (json_get_int(saved, "microphone_gain", &value) > 0 &&
        value >= 0 && value <= 100)
        APPLY_CONFIG(le_set_microphone_gain(c->backend, value));
    if (json_get_bool(saved, "microphone_muted", &value) > 0)
        APPLY_CONFIG(le_set_microphone_muted(c->backend, value));
    if (json_get_int(saved, "led_r", &red) > 0 &&
        json_get_int(saved, "led_g", &green) > 0 &&
        json_get_int(saved, "led_b", &blue) > 0 &&
        red >= 0 && red <= 255 && green >= 0 && green <= 255 &&
        blue >= 0 && blue <= 255)
        APPLY_CONFIG(le_set_led_colour(c->backend, (uint8_t)red,
                                      (uint8_t)green, (uint8_t)blue));
    if (json_get_int(saved, "led_brightness", &brightness) > 0 &&
        brightness >= 0 && brightness <= 100)
        APPLY_CONFIG(le_set_led_brightness(c->backend, brightness));
    if (json_get_bool(saved, "led_visualizer_enabled", &value) > 0)
        APPLY_CONFIG(le_set_led_visualizer_enabled(c->backend, value));
    if (json_get_string(saved, "wake_word", text, sizeof(text)) > 0 &&
        text[0])
        APPLY_CONFIG(le_set_wake_word(c->backend, text));
    if (json_get_int(saved, "wake_sensitivity", &value) > 0 &&
        value >= 0 && value <= 100)
        APPLY_CONFIG(le_set_wake_word_sensitivity(c->backend, value));
    if (json_get_bool(saved, "hostname_persisted",
                      &hostname_persisted) > 0 &&
        hostname_persisted &&
        json_get_string(saved, "hostname", text, sizeof(text)) > 0 &&
        valid_hostname(text))
        APPLY_CONFIG(le_set_hostname(c->backend, text));

#undef APPLY_CONFIG
    return failed ? LE_IO : LE_OK;
}
int api_init(struct api_context*c,struct le_backend*b,int dev,int insecure,const char*token,const char*origin,const char*csrf,const char*config_path,const char*users_path){char saved[16384];int v;memset(c,0,sizeof(*c));c->backend=b;c->dev_controls=dev;c->allow_insecure_lan=insecure;c->privacy_local_only=1;c->privacy_log_hours=24;c->integrations=4u;strcpy(c->button_short,"Start listening");strcpy(c->button_long,"Open pairing mode");if(token)strncpy(c->auth_token,token,sizeof(c->auth_token)-1);if(origin)strncpy(c->allowed_origin,origin,sizeof(c->allowed_origin)-1);if(!csrf||strlen(csrf)!=64)return -1;strncpy(c->csrf_token,csrf,sizeof(c->csrf_token)-1);if(config_path)strncpy(c->config_path,config_path,sizeof(c->config_path)-1);if(users_path)strncpy(c->users_path,users_path,sizeof(c->users_path)-1);if(c->users_path[0]&&!access(c->users_path,F_OK)&&le_auth_load(&c->auth,c->users_path)!=0){fprintf(stderr,"Unable to load LibreEcho users file: %s\n",c->users_path);return -1;}if(c->config_path[0]&&config_read(c->config_path,saved,sizeof(saved))>0){json_get_bool(saved,"privacy_local_only",&c->privacy_local_only);json_get_bool(saved,"privacy_audio_retention",&c->privacy_audio_retention);json_get_bool(saved,"privacy_telemetry",&c->privacy_telemetry);json_get_bool(saved,"privacy_crash_reports",&c->privacy_crash_reports);if(json_get_int(saved,"privacy_log_hours",&v)>0)c->privacy_log_hours=v;if(json_get_int(saved,"integrations",&v)>0)c->integrations=(unsigned)v;json_get_bool(saved,"ssh",&c->net_ssh);json_get_bool(saved,"api_lan",&c->net_api_lan);json_get_string(saved,"button_short",c->button_short,sizeof(c->button_short));json_get_string(saved,"button_long",c->button_long,sizeof(c->button_long));}event_bus_init(&c->events);api_log(c,"info","LibreEcho web daemon started");return 0;}
static enum le_log_level api_log_level(const char*level){if(level&&!strcmp(level,"error"))return LE_LOG_ERROR;if(level&&(!strcmp(level,"warning")||!strcmp(level,"warn")))return LE_LOG_WARNING;if(level&&!strcmp(level,"debug"))return LE_LOG_DEBUG;return LE_LOG_INFO;}
void api_log(struct api_context*c,const char*level,const char*message){char escaped[180],line[256];time_t t=time(0);struct timespec mono;long boot_seconds=0;if(clock_gettime(CLOCK_MONOTONIC,&mono)==0)boot_seconds=(long)mono.tv_sec;json_escape(escaped,sizeof(escaped),message);snprintf(line,sizeof(line),"{\"timestamp\":%ld,\"boot_seconds\":%ld,\"level\":\"%s\",\"message\":\"%s\"}",(long)t,boot_seconds,level,escaped);snprintf(c->logs[c->log_next%LE_MAX_LOGS],sizeof(c->logs[0]),"%s",line);c->log_next++;if(c->log_count<LE_MAX_LOGS)c->log_count++;event_bus_publish(&c->events,"log",line);le_log(api_log_level(level),"%s",message);}
static int cpu_json(const struct le_system_status*s,char*outbuf,size_t size){size_t i,used=0;int n;if(!size)return -1;n=snprintf(outbuf,size,"{\"count\":%zu,\"cores\":[",s->cpu_count);if(n<0||(size_t)n>=size)return -1;used=(size_t)n;for(i=0;i<s->cpu_count&&i<LE_MAX_CPUS;i++){n=snprintf(outbuf+used,size-used,"%s{\"id\":%zu,\"online\":%s,\"utilization_percent\":%d,\"frequency_khz\":%d}",i?",":"",i,s->cpus[i].online?"true":"false",s->cpus[i].utilization,s->cpus[i].frequency_khz);if(n<0||(size_t)n>=size-used)return -1;used+=(size_t)n;}n=snprintf(outbuf+used,size-used,"]}");return n<0||(size_t)n>=size-used?-1:(int)(used+(size_t)n);}
static void status_json(struct api_context*c,struct api_response*r){struct le_system_status s;char cpus[768],storage_percent[24],storage_used[24],storage_total[24];int rc=le_get_system_status(c->backend,&s);if(rc){err(r,503,rc,"System status is unavailable");return;}if(cpu_json(&s,cpus,sizeof(cpus))<0){err(r,503,LE_IO,"CPU status is unavailable");return;}snprintf(storage_percent,sizeof(storage_percent),s.storage_available?"%d":"null",s.storage);snprintf(storage_used,sizeof(storage_used),s.storage_available?"%d":"null",s.storage_used_mb);snprintf(storage_total,sizeof(storage_total),s.storage_total_mb>=0?"%d":"null",s.storage_total_mb);out(r,200,"{\"ok\":true,\"data\":{\"backend\":\"%s\",\"simulated\":%s,\"uptime_seconds\":%.0f,\"cpu_percent\":%d,\"cpus\":%s,\"memory_percent\":%d,\"memory_used_mb\":%d,\"memory_total_mb\":%d,\"storage_percent\":%s,\"storage_used_mb\":%s,\"storage_total_mb\":%s,\"storage_available\":%s,\"storage_state\":\"%s\",\"temperature_c\":%d,\"device_state\":\"%s\"},\"error\":null}",le_backend_mode(c->backend),!strcmp(le_backend_mode(c->backend),"mock")?"true":"false",s.uptime,s.cpu,cpus,s.memory,s.memory_used_mb,s.memory_total_mb,storage_percent,storage_used,storage_total,s.storage_available?"true":"false",s.storage_state,s.temperature,s.device_state);}
static void device_json(struct api_context*c,struct api_response*r){struct le_device_info d;char n[LE_TEXT*2],h[LE_TEXT*2],model[LE_TEXT*2],serial[LE_TEXT*2],os[64],kernel[128],revision[64],backend[32];int rc=le_get_device_info(c->backend,&d);if(rc){err(r,503,rc,"Device information is unavailable");return;}json_escape(n,sizeof(n),d.name);json_escape(h,sizeof(h),d.hostname);json_escape(model,sizeof(model),d.model);json_escape(serial,sizeof(serial),d.serial);json_escape(os,sizeof(os),d.os_version);json_escape(kernel,sizeof(kernel),d.kernel);json_escape(revision,sizeof(revision),d.hardware_revision);json_escape(backend,sizeof(backend),d.backend);out(r,200,"{\"ok\":true,\"data\":{\"name\":\"%s\",\"hostname\":\"%s\",\"model\":\"%s\",\"serial\":\"%s\",\"os_version\":\"%s\",\"kernel\":\"%s\",\"hardware_revision\":\"%s\",\"backend\":\"%s\"},\"error\":null}",n,h,model,serial,os,kernel,revision,backend);}
static void audio_json(struct api_context*c,struct api_response*r){struct le_audio_state a;int rc=le_get_audio_state(c->backend,&a);if(rc){err(r,501,rc,"Audio hardware backend is not available");return;}out(r,200,"{\"ok\":true,\"data\":{\"volume\":%d,\"microphone_gain\":%d,\"notification_volume\":%d,\"microphone_muted\":%s,\"startup_sound\":%s,\"amplifier_on\":%s,\"output_available\":%s,\"tts_voice\":\"%s\",\"tts_voices\":[{\"id\":\"southern-female\",\"name\":\"Southern English — female\"},{\"id\":\"northern-male\",\"name\":\"Northern English — male\"}]},\"error\":null}",a.volume,a.microphone_gain,a.notification_volume,a.muted?"true":"false",a.startup_sound?"true":"false",a.amplifier_on?"true":"false",a.output_available?"true":"false",a.tts_voice[0]?a.tts_voice:"southern-female");}
static void playback_json(struct api_context*c,struct api_response*r){struct le_playback_state p;char state[48],source[80],title[LE_MEDIA_TEXT*2+2],artist[LE_MEDIA_TEXT*2+2],album[LE_MEDIA_TEXT*2+2],source_json[96],title_json[LE_MEDIA_TEXT*2+8],artist_json[LE_MEDIA_TEXT*2+8],album_json[LE_MEDIA_TEXT*2+8];int rc=le_get_playback_state(c->backend,&p);if(rc){err(r,503,rc,"Playback status is unavailable");return;}json_escape(state,sizeof(state),p.state);json_escape(source,sizeof(source),p.source);json_escape(title,sizeof(title),p.title);json_escape(artist,sizeof(artist),p.artist);json_escape(album,sizeof(album),p.album);if(source[0])snprintf(source_json,sizeof(source_json),"\"%s\"",source);else strcpy(source_json,"null");if(title[0])snprintf(title_json,sizeof(title_json),"\"%s\"",title);else strcpy(title_json,"null");if(artist[0])snprintf(artist_json,sizeof(artist_json),"\"%s\"",artist);else strcpy(artist_json,"null");if(album[0])snprintf(album_json,sizeof(album_json),"\"%s\"",album);else strcpy(album_json,"null");out(r,200,"{\"ok\":true,\"data\":{\"state\":\"%s\",\"source\":%s,\"buses\":{\"media\":%s,\"system\":%s,\"announcement\":%s,\"alarm\":%s},\"metadata\":{\"available\":%s,\"title\":%s,\"artist\":%s,\"album\":%s}},\"error\":null}",state,source_json,p.media_active?"true":"false",p.system_active?"true":"false",p.announcement_active?"true":"false",p.alarm_active?"true":"false",p.metadata_available?"true":"false",title_json,artist_json,album_json);}
static int miccal_read(int values[7],int present[7]){int i,v,count=0;char path[64];FILE*f;for(i=0;i<7;i++){values[i]=16384;present[i]=0;snprintf(path,sizeof(path),"/proc/idme/miccal.%d",i);f=fopen(path,"r");if(f){if(fscanf(f,"%d",&v)==1&&v>=0&&v<=65535){values[i]=v;present[i]=1;count++;}fclose(f);}}if(!count){f=fopen("/proc/idme/miccal","r");if(f){for(i=0;i<7;i++)if(fscanf(f,"%d",&v)==1&&v>=0&&v<=65535){values[i]=v;present[i]=1;count++;}fclose(f);}}return count;}
static void baby_monitor_local_json(struct api_context*c,struct api_response*r){FILE*f;char line[512],name[256],sources[30000],calibration[1024];int values[7],present[7],i,calibrated=miccal_read(values,present),available=0,simulated=!strcmp(le_backend_mode(c->backend),"mock");sources[0]='\0';calibration[0]='\0';for(i=0;i<7;i++)snprintf(calibration+strlen(calibration),sizeof(calibration)-strlen(calibration),"%s%d",i?",":"",values[i]);f=fopen("/proc/asound/pcm","r");if(f){while(fgets(line,sizeof(line),f)){unsigned card,device;if(sscanf(line,"%u-%u: %255[^\n]",&card,&device,name)!=3)continue;if(card!=0||device!=24||!strstr(name,"Capture"))continue;{char pcm[64],escaped[512];snprintf(pcm,sizeof(pcm),"/dev/snd/pcmC%uD%uc",card,device);if(!access(pcm,R_OK)){json_escape(escaped,sizeof(escaped),name);snprintf(sources,sizeof(sources),"{\"id\":\"0:24\",\"card\":0,\"device\":24,\"name\":\"%s\",\"rate\":16000,\"channels\":9,\"bits\":24,\"valid_bits\":16,\"encoding\":\"pcm_s24_3le\",\"microphones\":[{\"channel\":0,\"name\":\"Array microphone 1\"},{\"channel\":1,\"name\":\"Array microphone 2\"},{\"channel\":2,\"name\":\"Array microphone 3\"},{\"channel\":3,\"name\":\"Array microphone 4\"},{\"channel\":4,\"name\":\"Array microphone 5\"},{\"channel\":5,\"name\":\"Array microphone 6\"},{\"channel\":6,\"name\":\"Array microphone 7\"}]}",escaped);available=1;}}}fclose(f);}if(!available&&simulated){strcpy(sources,"{\"id\":\"0:0\",\"card\":0,\"device\":0,\"name\":\"Mock microphone\",\"rate\":16000,\"channels\":1,\"bits\":16,\"valid_bits\":16,\"encoding\":\"pcm_s16le\",\"microphones\":[{\"channel\":0,\"name\":\"Microphone 1\"}]}");available=1;}out(r,200,"{\"ok\":true,\"data\":{\"available\":%s,\"simulated\":%s,\"sources\":[%s],\"raw_channels\":9,\"active_microphone_channels\":7,\"inactive_transport_channels\":[7,8],\"logical_channels\":7,\"calibration\":{\"q14\":[%s],\"fallback\":16384,\"source\":\"/proc/idme/miccal\",\"values_found\":%d,\"complete\":%s,\"selected_logical_mics\":[0,3],\"applied_to_raw_stream\":false,\"mapping\":\"identity-provisional; lanes 0 and 3 measured with four-sample compensation\"}},\"error\":null}",available?"true":"false",simulated?"true":"false",sources,calibration,calibrated,calibrated==7?"true":"false");}
static void baby_monitor_json(struct api_context*c,struct api_response*r){struct le_adapter*a;char data[30000];int rc;if(!strcmp(le_backend_mode(c->backend),"mock")){baby_monitor_local_json(c,r);return;}a=le_adapter_connect(LE_ADAPTER_MIC_SOCK,1000);if(!a){err(r,503,LE_NOT_SUPPORTED,"Microphone service is unavailable");return;}rc=le_adapter_call(a,"status","{}",data,sizeof(data));le_adapter_close(a);if(rc!=LE_ADAPTER_OK){err(r,503,LE_IO,"Microphone service status is unavailable");return;}out(r,200,"{\"ok\":true,\"data\":%s,\"error\":null}",data);}
static void led_json(struct api_context*c,struct api_response*r){struct le_led_state l;char owner[64],mood[32];size_t n=0,i;int rc=le_get_led_state(c->backend,&l);if(rc){err(r,501,rc,"LED hardware backend is not available");return;}json_escape(owner,sizeof(owner),l.visualizer_owner);json_escape(mood,sizeof(mood),l.visualizer_mood[0]?l.visualizer_mood:"idle");n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"{\"ok\":true,\"data\":{\"colour\":{\"r\":%u,\"g\":%u,\"b\":%u},\"brightness\":%d,\"animation_active\":%s,\"animation_profile\":\"%s\",\"pattern_active\":%s,\"pattern\":\"%s\",\"visualizer_enabled\":%s,\"visualizer_active\":%s,\"visualizer_owner\":\"%s\",\"visualizer_mood\":\"%s\",\"visualizer_levels\":[",l.current.r,l.current.g,l.current.b,l.current.brightness,l.animation_active?"true":"false",l.animation_profile==0?"listening":l.animation_profile==1?"thinking":l.animation_profile==2?"error":l.animation_profile==3?"dnd":"none",l.pattern_active?"true":"false",l.pattern[0]?l.pattern:"none",l.visualizer_enabled?"true":"false",l.visualizer_active?"true":"false",owner,mood);for(i=0;i<LE_LED_PIXELS&&n<sizeof(r->body);i++)n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"%s%u",i?",":"",l.visualizer_levels[i]);n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"],\"pixels\":[");for(i=0;i<LE_LED_PIXELS&&n<sizeof(r->body);i++)n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"%s{\"r\":%u,\"g\":%u,\"b\":%u}",i?",":"",l.pixels[i].r,l.pixels[i].g,l.pixels[i].b);n+=(size_t)snprintf(r->body+n,sizeof(r->body)-n,"],\"boot_profile\":{\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":%d},\"profiles\":{\"listening\":\"#48b9ff\",\"thinking\":\"#a873ef\",\"error\":\"#ef5050\",\"dnd\":\"#be2323\"}},\"error\":null}",l.boot.r,l.boot.g,l.boot.b,l.boot.brightness);if(n>=sizeof(r->body)){err(r,503,LE_IO,"LED status response is too large");return;}r->status=200;strcpy(r->type,"application/json; charset=utf-8");r->length=n;}static void net_json(struct api_context*c,struct api_response*r){
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
static void wake_json(struct api_context*c,struct api_response*r){struct le_wake_word_state w;char wake[LE_TEXT*2],model[64];int rc=le_get_wake_word_state(c->backend,&w);if(rc){err(r,501,rc,"Wake-word service is not available");return;}json_escape(wake,sizeof(wake),w.wake_word);json_escape(model,sizeof(model),w.model_status);out(r,200,"{\"ok\":true,\"data\":{\"enabled\":%s,\"wake_word\":\"%s\",\"sensitivity\":%d,\"cooldown_ms\":%d,\"model_status\":\"%s\",\"detected_count\":%d,\"cpu_cost_percent\":%d,\"memory_cost_mb\":%d,\"local_processing\":true},\"error\":null}",w.enabled?"true":"false",wake,w.sensitivity,w.cooldown_ms,model,w.detected_count,w.cpu_cost,w.memory_cost_mb);}
static int configuration_item(char*out,size_t size,size_t*used,int*first,const char*fmt,...){va_list ap;int n;if(!out||!used||!first||!fmt||*used>=size)return -1;if(!*first){n=snprintf(out+*used,size-*used,",\n  ");if(n<0||(size_t)n>=size-*used)return -1;*used+=(size_t)n;}va_start(ap,fmt);n=vsnprintf(out+*used,size-*used,fmt,ap);va_end(ap);if(n<0||(size_t)n>=size-*used)return -1;*used+=(size_t)n;*first=0;return 0;}
static int configuration_json(struct api_context*c,char*out,size_t size){struct le_audio_state a;struct le_led_state l;struct le_wake_word_state w;struct le_network_state n;const char*unsupported[16];size_t unsupported_count=0,i,used=0;char wake[128],host[128],short_action[80],long_action[80];int audio_rc,led_rc,wake_rc,network_rc,first=0,nbytes;audio_rc=le_get_audio_state(c->backend,&a);led_rc=le_get_led_state(c->backend,&l);wake_rc=le_get_wake_word_state(c->backend,&w);network_rc=le_get_network_state(c->backend,&n);if(audio_rc){unsupported[unsupported_count++]="volume";unsupported[unsupported_count++]="microphone_gain";unsupported[unsupported_count++]="microphone_muted";}if(led_rc){unsupported[unsupported_count++]="led_r";unsupported[unsupported_count++]="led_g";unsupported[unsupported_count++]="led_b";unsupported[unsupported_count++]="led_brightness";unsupported[unsupported_count++]="led_visualizer_enabled";}if(wake_rc){unsupported[unsupported_count++]="wake_word";unsupported[unsupported_count++]="wake_sensitivity";}if(network_rc)unsupported[unsupported_count++]="hostname";nbytes=snprintf(out,size,"{\n  \"schema_version\": 1,\n  \"hostname_persisted\": true,\n  \"partial\": %s,\n  \"unsupported\": [",unsupported_count?"true":"false");if(nbytes<0||(size_t)nbytes>=size)return LE_IO;used=(size_t)nbytes;for(i=0;i<unsupported_count;i++){nbytes=snprintf(out+used,size-used,"%s\"%s\"",i?",":"",unsupported[i]);if(nbytes<0||(size_t)nbytes>=size-used)return LE_IO;used+=(size_t)nbytes;}nbytes=snprintf(out+used,size-used,"]");if(nbytes<0||(size_t)nbytes>=size-used)return LE_IO;used+=(size_t)nbytes;if(!audio_rc){if(configuration_item(out,size,&used,&first,"\"volume\": %d",a.volume)||configuration_item(out,size,&used,&first,"\"microphone_gain\": %d",a.microphone_gain)||configuration_item(out,size,&used,&first,"\"microphone_muted\": %s",a.muted?"true":"false"))return LE_IO;}if(!led_rc){if(configuration_item(out,size,&used,&first,"\"led_r\": %u",l.current.r)||configuration_item(out,size,&used,&first,"\"led_g\": %u",l.current.g)||configuration_item(out,size,&used,&first,"\"led_b\": %u",l.current.b)||configuration_item(out,size,&used,&first,"\"led_brightness\": %d",l.current.brightness)||configuration_item(out,size,&used,&first,"\"led_visualizer_enabled\": %s",l.visualizer_enabled?"true":"false"))return LE_IO;}if(!wake_rc){json_escape(wake,sizeof(wake),w.wake_word);if(configuration_item(out,size,&used,&first,"\"wake_word\": \"%s\"",wake)||configuration_item(out,size,&used,&first,"\"wake_sensitivity\": %d",w.sensitivity))return LE_IO;}if(!network_rc){json_escape(host,sizeof(host),n.hostname);if(configuration_item(out,size,&used,&first,"\"hostname\": \"%s\"",host))return LE_IO;}json_escape(short_action,sizeof(short_action),c->button_short);json_escape(long_action,sizeof(long_action),c->button_long);if(configuration_item(out,size,&used,&first,"\"ssh\": %s",c->net_ssh?"true":"false")||configuration_item(out,size,&used,&first,"\"api_lan\": %s",c->net_api_lan?"true":"false")||configuration_item(out,size,&used,&first,"\"button_short\": \"%s\"",short_action)||configuration_item(out,size,&used,&first,"\"button_long\": \"%s\"",long_action)||configuration_item(out,size,&used,&first,"\"privacy_local_only\": %s",c->privacy_local_only?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_audio_retention\": %s",c->privacy_audio_retention?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_telemetry\": %s",c->privacy_telemetry?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_crash_reports\": %s",c->privacy_crash_reports?"true":"false")||configuration_item(out,size,&used,&first,"\"privacy_log_hours\": %d",c->privacy_log_hours)||configuration_item(out,size,&used,&first,"\"integrations\": %u",c->integrations))return LE_IO;nbytes=snprintf(out+used,size-used,"\n}");return nbytes<0||(size_t)nbytes>=size-used?LE_IO:LE_OK;}
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
static int import_configuration(struct api_context*c,const char*j){char wake[LE_TEXT],hostname[LE_TEXT],short_action[32],long_action[32];int schema,volume,gain,muted,r,g,b,brightness,visualizer_enabled=1,visualizer_field,sensitivity,ssh,api_lan,local_only,audio_retention,telemetry,crash_reports,log_hours,integrations,rc;if(strcmp(le_backend_mode(c->backend),"mock"))return LE_NOT_SUPPORTED;visualizer_field=json_get_bool(j,"led_visualizer_enabled",&visualizer_enabled);if(visualizer_field<0||json_get_int(j,"schema_version",&schema)<1||schema!=1||json_get_int(j,"volume",&volume)<1||volume<0||volume>100||json_get_int(j,"microphone_gain",&gain)<1||gain<0||gain>100||json_get_bool(j,"microphone_muted",&muted)<1||json_get_int(j,"led_r",&r)<1||r<0||r>255||json_get_int(j,"led_g",&g)<1||g<0||g>255||json_get_int(j,"led_b",&b)<1||b<0||b>255||json_get_int(j,"led_brightness",&brightness)<1||brightness<0||brightness>100||json_get_string(j,"wake_word",wake,sizeof(wake))<1||json_get_int(j,"wake_sensitivity",&sensitivity)<1||sensitivity<0||sensitivity>100||json_get_string(j,"hostname",hostname,sizeof(hostname))<1||json_get_bool(j,"ssh",&ssh)<1||json_get_bool(j,"api_lan",&api_lan)<1||json_get_string(j,"button_short",short_action,sizeof(short_action))<1||json_get_string(j,"button_long",long_action,sizeof(long_action))<1||json_get_bool(j,"privacy_local_only",&local_only)<1||json_get_bool(j,"privacy_audio_retention",&audio_retention)<1||json_get_bool(j,"privacy_telemetry",&telemetry)<1||json_get_bool(j,"privacy_crash_reports",&crash_reports)<1||json_get_int(j,"privacy_log_hours",&log_hours)<1||(log_hours!=24&&log_hours!=168&&log_hours!=720)||json_get_int(j,"integrations",&integrations)<1||integrations<0||integrations>31)return LE_INVALID;if((rc=le_set_volume(c->backend,volume))||(rc=le_set_microphone_gain(c->backend,gain))||(rc=le_set_microphone_muted(c->backend,muted))||(rc=le_set_led_colour(c->backend,(uint8_t)r,(uint8_t)g,(uint8_t)b))||(rc=le_set_led_brightness(c->backend,brightness))||(rc=le_set_led_visualizer_enabled(c->backend,visualizer_enabled))||(rc=le_set_wake_word(c->backend,wake))||(rc=le_set_wake_word_sensitivity(c->backend,sensitivity))||(rc=le_set_hostname(c->backend,hostname)))return rc;c->net_ssh=ssh;c->net_api_lan=api_lan;c->privacy_local_only=local_only;c->privacy_audio_retention=audio_retention;c->privacy_telemetry=telemetry;c->privacy_crash_reports=crash_reports;c->privacy_log_hours=log_hours;c->integrations=(unsigned)integrations;strcpy(c->button_short,short_action);strcpy(c->button_long,long_action);return persist_configuration(c);}
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
static void system_json(struct api_context*c,struct api_response*r){time_t now=time(0);int valid=now>=1577836800,linux=!strcmp(le_backend_mode(c->backend),"linux");long synchronized=0,rtc_available=0,rtc_persisted=0,last_sync=0;char state[64]="unavailable",source[64],servers[1100]="",escaped_state[128],escaped_source[128],escaped_servers[2200];strcpy(source,valid?"system-clock":"unset");if(linux){time_status_value("state",state,sizeof(state));time_status_value("source",source,sizeof(source));time_status_value("servers",servers,sizeof(servers));time_status_int("synchronized",&synchronized);time_status_int("rtc_available",&rtc_available);time_status_int("rtc_persisted",&rtc_persisted);time_status_int("last_sync_epoch",&last_sync);if(!rtc_available&&access("/sys/class/rtc/rtc0",F_OK)==0)rtc_available=1;}json_escape(escaped_state,sizeof(escaped_state),state);json_escape(escaped_source,sizeof(escaped_source),source);json_escape(escaped_servers,sizeof(escaped_servers),servers);out(r,200,"{\"ok\":true,\"data\":{\"update_channel\":\"stable\",\"ota\":{\"supported\":false,\"design\":\"A/B\",\"current_slot\":\"A\",\"inactive_slot\":\"B\",\"state\":\"idle\",\"progress\":0,\"rollback_available\":false},\"timezone\":\"UTC\",\"ntp\":%s,\"ntp_state\":\"%s\",\"ntp_servers\":\"%s\",\"last_sync_epoch\":%ld,\"clock_valid\":%s,\"clock_source\":\"%s\",\"rtc_available\":%s,\"rtc_persisted\":%s},\"error\":null}",synchronized&&valid?"true":"false",escaped_state,escaped_servers,last_sync,valid?"true":"false",escaped_source,rtc_available?"true":"false",rtc_persisted?"true":"false");}
#define LE_AUTH_FAILURE_LIMIT 5
#define LE_AUTH_FAILURE_WINDOW 60
#define LE_AUTH_BLOCK_SECONDS 60
#define api_handle api_handle_inner
static int bluetooth_append(char *buffer,size_t size,size_t*used,const char*fmt,...){va_list ap;int n;if(!buffer||!used||*used>=size)return-1;va_start(ap,fmt);n=vsnprintf(buffer+*used,size-*used,fmt,ap);va_end(ap);if(n<0||(size_t)n>=size-*used)return-1;*used+=(size_t)n;return 0;}
static void bluetooth_json(struct api_context*c,struct api_response*r){struct le_bluetooth_state b;size_t i,n=0;int rc;char state[64],transport[64],hci[64],local_name[LE_TEXT*2],error[192],disconnect_reason[96],connect_failed_status[96],profile_state[32],profile_error[192],name[LE_TEXT*2];if((rc=le_get_bluetooth_state(c->backend,&b))){err(r,501,rc,"Bluetooth status is unavailable");return;}json_escape(state,sizeof(state),b.state);json_escape(transport,sizeof(transport),b.transport);json_escape(hci,sizeof(hci),b.hci);json_escape(local_name,sizeof(local_name),b.local_name);json_escape(error,sizeof(error),b.last_error);json_escape(disconnect_reason,sizeof(disconnect_reason),b.last_disconnect_reason);json_escape(connect_failed_status,sizeof(connect_failed_status),b.last_connect_failed_status);json_escape(profile_state,sizeof(profile_state),b.profile_state);json_escape(profile_error,sizeof(profile_error),b.profile_error);if(bluetooth_append(r->body,sizeof(r->body),&n,"{\"ok\":true,\"data\":{\"state\":\"%s\",\"available\":%s,\"enabled\":%s,\"activation_attempted\":%s,\"transport\":\"%s\",\"hci\":\"%s\",\"local_name\":\"%s\",\"last_error\":\"%s\",\"last_disconnect_reason\":\"%s\",\"last_connect_failed_status\":\"%s\",\"profile_state\":\"%s\",\"profile_error\":\"%s\",\"profile_services\":{\"sdp\":%s,\"a2dp_sink\":%s,\"avrcp\":%s,\"rfcomm\":%s,\"bnep\":%s,\"hidp\":%s},\"scanning\":%s,\"pairing\":%s,\"pairing_mode\":%s,\"capabilities\":{\"classic\":%s,\"le\":%s,\"ssp\":%s,\"secure_connection\":%s,\"connectable\":%s,\"discoverable\":%s,\"bondable\":%s},\"pending_pairing\":",state,b.available?"true":"false",b.enabled?"true":"false",b.activation_attempted?"true":"false",transport,hci,local_name,error,disconnect_reason,connect_failed_status,profile_state,profile_error,b.profile_sdp?"true":"false",b.profile_a2dp_sink?"true":"false",b.profile_avrcp?"true":"false",b.profile_rfcomm?"true":"false",b.profile_bnep?"true":"false",b.profile_hidp?"true":"false",b.scanning?"true":"false",b.pairing?"true":"false",b.pairing_mode?"true":"false",b.classic?"true":"false",b.le?"true":"false",b.ssp?"true":"false",b.secure_connection?"true":"false",b.connectable?"true":"false",b.discoverable?"true":"false",b.bondable?"true":"false")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}if(b.pairing){json_escape(name,sizeof(name),b.pending_pairing.address);{char method[48];json_escape(method,sizeof(method),b.pending_pairing.method);if(bluetooth_append(r->body,sizeof(r->body),&n,"{\"address\":\"%s\",\"type\":%d,\"method\":\"%s\",\"value\":%u}",name,b.pending_pairing.type,method,b.pending_pairing.value)){err(r,503,LE_IO,"Bluetooth status response is too large");return;}}}else if(bluetooth_append(r->body,sizeof(r->body),&n,"null")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}if(bluetooth_append(r->body,sizeof(r->body),&n,",\"discovered\":[")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}for(i=0;i<b.discovered_count&&i<LE_MAX_BLUETOOTH_DEVICES;i++){char address[32];json_escape(address,sizeof(address),b.discovered[i].address);json_escape(name,sizeof(name),b.discovered[i].name);if(bluetooth_append(r->body,sizeof(r->body),&n,"%s{\"address\":\"%s\",\"name\":\"%s\",\"type\":%d,\"rssi\":%d,\"rssi_valid\":%s,\"paired\":%s,\"connected\":%s}",i?",":"",address,name,b.discovered[i].type,b.discovered[i].rssi,b.discovered[i].rssi_valid?"true":"false",b.discovered[i].paired?"true":"false",b.discovered[i].connected?"true":"false")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}}if(bluetooth_append(r->body,sizeof(r->body),&n,"],\"known_devices\":[")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}for(i=0;i<b.known_count&&i<LE_MAX_BLUETOOTH_DEVICES;i++){char address[32];json_escape(address,sizeof(address),b.known[i].address);json_escape(name,sizeof(name),b.known[i].name);if(bluetooth_append(r->body,sizeof(r->body),&n,"%s{\"address\":\"%s\",\"name\":\"%s\",\"type\":%d,\"rssi\":%d,\"rssi_valid\":%s,\"connected\":%s}",i?",":"",address,name,b.known[i].type,b.known[i].rssi,b.known[i].rssi_valid?"true":"false",b.known[i].connected?"true":"false")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}}if(bluetooth_append(r->body,sizeof(r->body),&n,"]},\"error\":null}")){err(r,503,LE_IO,"Bluetooth status response is too large");return;}r->status=200;strcpy(r->type,"application/json; charset=utf-8");r->length=n;}
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
void api_handle(struct api_context*c,const struct api_request*q,struct api_response*r){const char*p=q->path;int rc=LE_OK,v;if(!security(c,q,r))return;if((!strcmp(p,"/api/v1/buttons")&&strcmp(q->method,"GET")&&strcmp(q->method,"PUT"))||(!strcmp(p,"/api/v1/privacy")&&strcmp(q->method,"GET")&&strcmp(q->method,"PUT"))||(!strcmp(p,"/api/v1/integrations")&&strcmp(q->method,"GET"))||(!strncmp(p,"/api/v1/integrations/",21)&&strcmp(q->method,"PUT"))){method_not_allowed(r);return;}if(changing(q->method)&&q->body_len&&!body_ok(q,r))return;if(!strcmp(p,"/api/v1/auth/bootstrap")&&!strcmp(q->method,"POST")){auth_bootstrap_json(c,q,r);return;}if(!strcmp(p,"/api/v1/auth/login")&&!strcmp(q->method,"POST")){auth_login_json(c,q,r);return;}if(!strcmp(p,"/api/v1/auth/users")&& !strcmp(q->method,"GET")){auth_users_json(c,r);return;}if(!strcmp(p,"/api/v1/auth/users")&& !strcmp(q->method,"POST")){auth_add_user_json(c,q,r);return;}if(!strncmp(p,"/api/v1/auth/users/",strlen("/api/v1/auth/users/"))&& !strcmp(q->method,"DELETE")){auth_remove_user_json(c,q,r);return;}if((!strcmp(p,"/api/v1/auth/users")||!strncmp(p,"/api/v1/auth/users/",strlen("/api/v1/auth/users/")))&&strcmp(q->method,"GET")&&strcmp(q->method,"POST")&&strcmp(q->method,"DELETE")){method_not_allowed(r);return;}if(!strcmp(p,"/api/v1/auth")&&!strcmp(q->method,"GET")){auth_current_json(c,q,r);return;}if(!strcmp(p,"/api/v1/auth/logout")&&!strcmp(q->method,"POST")){if(!strncmp(q->authorization,"Bearer ",7))le_auth_logout(&c->auth,q->authorization+7);ok(r,"{\"logged_out\":true}");return;}
 if(!strcmp(p,"/api/v1/config/export")&&!strcmp(q->method,"GET")){char config[4096];rc=configuration_json(c,config,sizeof(config));if(rc)err(r,501,rc,"Configuration export is unavailable for this backend");else ok(r,config);return;}if(!strcmp(p,"/api/v1/config/import")&&!strcmp(q->method,"POST")){rc=import_configuration(c,q->body);if(rc)err(r,rc==LE_INVALID?400:501,rc,rc==LE_INVALID?"Configuration file is invalid or incomplete":"Configuration restore is unavailable for this backend");else{api_log(c,"warning","Configuration restored from uploaded JSON");ok(r,"{\"restored\":true,\"schema_version\":1}");}return;}
 if(!strcmp(p,"/api/v1/assistant")){if(!strcmp(q->method,"GET")){agent_result(r,"status",NULL);return;}if(!strcmp(q->method,"PUT")){agent_result(r,"configure",q->body);return;}method_not_allowed(r);return;}
 if(!strcmp(p,"/api/v1/assistant/auth/start")&&!strcmp(q->method,"POST")){agent_result(r,"auth_start",NULL);return;}
 if(!strcmp(p,"/api/v1/assistant/auth/poll")&&!strcmp(q->method,"POST")){agent_result(r,"auth_poll",NULL);return;}
 if(!strcmp(p,"/api/v1/assistant/logout")&&!strcmp(q->method,"POST")){agent_result(r,"logout",NULL);return;}
 if(!strcmp(p,"/api/v1/assistant/respond")&&!strcmp(q->method,"POST")){agent_result(r,"respond",q->body);return;}
 if((!strcmp(p,"/api/v1")||!strcmp(p,"/api/v1/"))&&!strcmp(q->method,"GET")){ok(r,"{\"name\":\"LibreEcho API\",\"version\":\"v1\",\"status\":\"/api/v1/status\",\"playback\":\"/api/v1/playback\",\"openapi\":\"/openapi.json\",\"swagger\":\"/swagger.html\"}");return;}if(!strcmp(p,"/api/v1/status")&&!strcmp(q->method,"GET")){status_json(c,r);return;}if(!strcmp(p,"/api/v1/playback")&&!strcmp(q->method,"GET")){playback_json(c,r);return;}if(!strcmp(p,"/api/v1/device")&&!strcmp(q->method,"GET")){device_json(c,r);return;}if(!strcmp(p,"/api/v1/config")&&!strcmp(q->method,"GET")){out(r,200,"{\"ok\":true,\"data\":{\"api_version\":1,\"csrf_token\":\"%s\",\"authentication\":\"%s\",\"bootstrap_required\":%s,\"user_count\":%zu,\"bind_policy\":\"%s\",\"max_request_body\":16384},\"error\":null}",c->csrf_token,api_bootstrap_required_internal(c)?"bootstrap-required":c->auth.enabled?"users":c->auth_token[0]?"bearer-token":"development-disabled",api_bootstrap_required_internal(c)?"true":"false",c->auth.user_count,c->allow_insecure_lan?"lan-development":"loopback-default");return;}
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
 if(!strcmp(p,"/api/v1/led")){if(!strcmp(q->method,"GET")){led_json(c,r);return;}if(!strcmp(q->method,"PUT")){int rr,gg,bb,visualizer_enabled,visualizer_field=json_get_bool(q->body,"visualizer_enabled",&visualizer_enabled);if(visualizer_field<0)rc=LE_INVALID;if(!rc&&json_get_int(q->body,"r",&rr)>0&&json_get_int(q->body,"g",&gg)>0&&json_get_int(q->body,"b",&bb)>0){if(rr<0||rr>255||gg<0||gg>255||bb<0||bb>255)rc=LE_INVALID;else rc=le_set_led_colour(c->backend,(uint8_t)rr,(uint8_t)gg,(uint8_t)bb);}if(!rc&&json_get_int(q->body,"brightness",&v)>0)rc=le_set_led_brightness(c->backend,v);if(!rc&&visualizer_field>0)rc=le_set_led_visualizer_enabled(c->backend,visualizer_enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"LED setting could not be applied");return;}event_bus_publish(&c->events,"led","{\"changed\":true}");led_json(c,r);return;}}
 if(!strcmp(p,"/api/v1/led/test")&&!strcmp(q->method,"POST")){rc=le_run_led_test(c->backend);if(rc)err(r,501,rc,"LED test is not available");else ok(r,"{\"testing\":true}");return;}
 if(!strcmp(p,"/api/v1/led/profile")&&!strcmp(q->method,"PUT")){char name[24];struct le_led_profile profile;memset(&profile,0,sizeof(profile));if(json_get_string(q->body,"name",name,sizeof(name))<1||json_get_int(q->body,"r",&v)<1||v<0||v>255){err(r,400,LE_INVALID,"A valid LED profile and colour are required");return;}profile.r=(uint8_t)v;if(json_get_int(q->body,"g",&v)<1||v<0||v>255){err(r,400,LE_INVALID,"LED colour is invalid");return;}profile.g=(uint8_t)v;if(json_get_int(q->body,"b",&v)<1||v<0||v>255){err(r,400,LE_INVALID,"LED colour is invalid");return;}profile.b=(uint8_t)v;profile.brightness=100;if(json_get_int(q->body,"brightness",&v)>0){if(v<0||v>100){err(r,400,LE_INVALID,"LED brightness is invalid");return;}profile.brightness=v;}rc=le_set_led_profile(c->backend,name,&profile);if(rc){err(r,rc==LE_INVALID?400:501,rc,"LED profile could not be saved");return;}event_bus_publish(&c->events,"led","{\"changed\":true}");led_json(c,r);return;}
 if(!strcmp(p,"/api/v1/network")){if(!strcmp(q->method,"GET")){net_json(c,r);return;}if(!strcmp(q->method,"PUT")){char host[64];int changed=0;if(json_get_string(q->body,"hostname",host,sizeof(host))>0){rc=le_set_hostname(c->backend,host);changed=1;if(rc){err(r,rc==LE_INVALID?400:501,rc,"Hostname could not be applied");return;}}if(json_get_bool(q->body,"ssh",&v)>0){c->net_ssh=v;changed=1;}if(json_get_bool(q->body,"api_lan",&v)>0){c->net_api_lan=v;changed=1;}if(!changed){err(r,400,LE_INVALID,"No supported network setting was supplied");return;}net_json(c,r);return;}}if(!strcmp(p,"/api/v1/network/wifi/scan")&&!strcmp(q->method,"GET")){struct le_wifi_scan s;rc=le_scan_wifi(c->backend,&s);if(rc){err(r,501,rc,"Wi-Fi scan failed");return;}if(wifi_scan_json(r,&s)<0){err(r,503,LE_IO,"Wi-Fi scan response is too large");return;}return;}
 if(!strcmp(p,"/api/v1/network/wifi/connect")&&!strcmp(q->method,"POST")){struct le_wifi_credentials w;memset(&w,0,sizeof(w));if(json_get_string(q->body,"ssid",w.ssid,sizeof(w.ssid))<1){err(r,400,LE_INVALID,"SSID is required");return;}json_get_string(q->body,"password",w.password,sizeof(w.password));json_get_string(q->body,"security",w.security,sizeof(w.security));rc=le_connect_wifi(c->backend,&w);memset(w.password,0,sizeof(w.password));if(rc){err(r,503,rc,"Wi-Fi connection failed");return;}event_bus_publish(&c->events,"network","{\"state\":\"connecting\"}");ok(r,"{\"state\":\"connecting\"}");return;}if(!strcmp(p,"/api/v1/network/wifi/disconnect")&&!strcmp(q->method,"POST")){rc=le_disconnect_wifi(c->backend);if(rc)err(r,501,rc,"Wi-Fi disconnect is unavailable");else ok(r,"{\"state\":\"disconnected\"}");return;}
 if(!strcmp(p,"/api/v1/wake-word")){if(!strcmp(q->method,"GET")){wake_json(c,r);return;}if(!strcmp(q->method,"PUT")){char s[64];if(json_get_string(q->body,"wake_word",s,sizeof(s))>0)rc=le_set_wake_word(c->backend,s);if(!rc&&json_get_int(q->body,"sensitivity",&v)>0)rc=le_set_wake_word_sensitivity(c->backend,v);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Wake-word setting could not be applied");return;}wake_json(c,r);return;}}if(!strcmp(p,"/api/v1/wake-word/test")&&!strcmp(q->method,"POST")){rc=le_test_wake_word(c->backend);if(rc)err(r,409,rc,"Wake-word trigger failed");else ok(r,"{\"detected\":true}");return;}
 if(!strcmp(p,"/api/v1/buttons")){if(!strcmp(q->method,"PUT")){char short_action[32],long_action[32];if(json_get_string(q->body,"short_press",short_action,sizeof(short_action))>0)strcpy(c->button_short,short_action);if(json_get_string(q->body,"long_press",long_action,sizeof(long_action))>0)strcpy(c->button_long,long_action);}char short_action[64],long_action[64];json_escape(short_action,sizeof(short_action),c->button_short);json_escape(long_action,sizeof(long_action),c->button_long);out(r,200,"{\"ok\":true,\"data\":{\"short_press\":\"%s\",\"long_press\":\"%s\",\"hardware_mute\":true},\"error\":null}",short_action,long_action);return;}
 if(!strcmp(p,"/api/v1/privacy")&&!strcmp(q->method,"PUT")&&json_get_int(q->body,"log_retention_hours",&v)>0&&(v==24||v==168||v==720))c->privacy_log_hours=v;
 if(!strcmp(p,"/api/v1/privacy")){if(!strcmp(q->method,"PUT")){if(json_get_bool(q->body,"local_only",&v)>0)c->privacy_local_only=v;if(json_get_bool(q->body,"diagnostic_telemetry",&v)>0)c->privacy_telemetry=v;if(json_get_bool(q->body,"crash_reports",&v)>0)c->privacy_crash_reports=v;{char retention[16];if(json_get_string(q->body,"audio_retention",retention,sizeof(retention))>0)c->privacy_audio_retention=strcmp(retention,"none")!=0;}}out(r,200,"{\"ok\":true,\"data\":{\"local_only\":%s,\"audio_retention\":\"%s\",\"diagnostic_telemetry\":%s,\"log_retention_hours\":%d,\"crash_reports\":%s},\"error\":null}",c->privacy_local_only?"true":"false",c->privacy_audio_retention?"24h":"none",c->privacy_telemetry?"true":"false",c->privacy_log_hours,c->privacy_crash_reports?"true":"false");return;}if(!strcmp(p,"/api/v1/buttons")){ok(r,"{\"short_press\":\"start_listening\",\"long_press\":\"pairing_mode\",\"hardware_mute\":true}");return;}if(!strncmp(p,"/api/v1/integrations",20)){if(!strcmp(q->method,"PUT")){int enabled;if(json_get_bool(q->body,"enabled",&enabled)<1){err(r,400,LE_INVALID,"Enabled must be a boolean");return;}if(strstr(p,"home-assistant")){if(enabled)c->integrations|=1u;else c->integrations&=~1u;}else if(strstr(p,"mqtt")){if(enabled)c->integrations|=2u;else c->integrations&=~2u;}else if(strstr(p,"rest")){c->net_api_lan=enabled;}else if(strstr(p,"airplay2")){rc=le_set_airplay_enabled(c->backend,enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"AirPlay 2 control could not be applied");return;}if(enabled)c->integrations|=16u;else c->integrations&=~16u;}else if(strstr(p,"bluetooth")){rc=le_set_bluetooth_enabled(c->backend,enabled);if(rc){err(r,rc==LE_INVALID?400:501,rc,"Bluetooth control could not be applied");return;}if(enabled)c->integrations|=8u;else c->integrations&=~8u;}else{err(r,404,LE_INVALID,"Integration was not found");return;}}out(r,200,"{\"ok\":true,\"data\":{\"items\":[{\"id\":\"home-assistant\",\"name\":\"Home Assistant\",\"enabled\":%s},{\"id\":\"mqtt\",\"name\":\"MQTT\",\"enabled\":%s},{\"id\":\"rest\",\"name\":\"Local REST API\",\"enabled\":%s,\"forced\":%s},{\"id\":\"bluetooth\",\"name\":\"Bluetooth audio\",\"enabled\":%s},{\"id\":\"airplay2\",\"name\":\"AirPlay 2\",\"enabled\":%s}]},\"error\":null}",(c->integrations&1u)?"true":"false",(c->integrations&2u)?"true":"false",(c->allow_insecure_lan||c->net_api_lan)?"true":"false",c->allow_insecure_lan?"true":"false",(c->integrations&8u)?"true":"false",(c->integrations&16u)?"true":"false");return;}
 if(!strcmp(p,"/api/v1/system")&&!strcmp(q->method,"GET")){system_json(c,r);return;}
 if(!strcmp(p,"/api/v1/provenance")&&!strcmp(q->method,"GET")){provenance_json(r);return;}
 if(!strcmp(p,"/api/v1/system/update")&&!strcmp(q->method,"GET")){update_status_json(c,r);return;}
 if(!strcmp(p,"/api/v1/system/update/automatic")){int enabled;if(strcmp(q->method,"PUT")){method_not_allowed(r);return;}if(!update_fetch_supported(c)){err(r,501,LE_NOT_SUPPORTED,"GitHub OTA updates are unavailable on this image");return;}if(json_get_bool(q->body,"enabled",&enabled)<1){err(r,400,LE_INVALID,"Enabled must be a boolean");return;}rc=run_init_command(update_fetch_path(),enabled?"enable-automatic":"disable-automatic");if(rc){err(r,503,rc,"Automatic update preference could not be saved");return;}api_log(c,"info",enabled?"Automatic OTA installation enabled":"Automatic OTA installation disabled");update_status_json(c,r);return;}
 if(!strcmp(p,"/api/v1/system/update/channel")){char channel[16];if(!api_update_channel_authorize(c,q,r,channel,sizeof(channel)))return;if(run_init_command(update_fetch_path(),!strcmp(channel,"dev")?"set-channel-dev":"set-channel-stable")){err(r,503,LE_IO,"Update channel could not be saved");return;}api_log(c,"info","OTA update channel changed");update_status_json(c,r);return;}
 if((!strcmp(p,"/api/v1/system/reboot")||!strcmp(p,"/api/v1/system/shutdown")||!strcmp(p,"/api/v1/system/factory-reset"))&&!strcmp(q->method,"POST")){if(strcmp(q->confirm,"confirm-device-action")){err(r,403,LE_AUTH,"A valid destructive-action confirmation token is required");return;}rc=strstr(p,"factory-reset")?le_factory_reset(c->backend):strstr(p,"shutdown")?le_shutdown(c->backend):le_reboot(c->backend);if(rc){err(r,501,rc,"Device action is not available");return;}api_log(c,"warning",p);ok(r,"{\"accepted\":true}");return;}
 if(!strcmp(p,"/api/v1/logs")&&!strcmp(q->method,"GET")){logs_json(c,r);return;}if(!strcmp(p,"/api/v1/logs/stream")&&!strcmp(q->method,"GET")){logs_stream_json(c,r);return;}if(!strcmp(p,"/api/v1/diagnostics")&&!strcmp(q->method,"GET")){diagnostics_json(c,r);return;}if(!strcmp(p,"/api/v1/diagnostics/export")&&!strcmp(q->method,"POST")){ok(r,"{\"filename\":\"libreecho-diagnostics.json\",\"redacted\":true}");return;}
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
static int handle_privacy(struct api_context *c,
                          const struct api_request *q,
                          struct api_response *r)
{
    char retention[16];
    int parsed;
    int value;
    int rc;

    if (strcmp(q->path, "/api/v1/privacy"))
        return 0;
    if (!strcmp(q->method, "GET"))
        goto respond;
    if (strcmp(q->method, "PUT")) {
        method_not_allowed(r);
        return 1;
    }

    parsed = json_get_bool(q->body, "local_only", &value);
    if (parsed < 0) {
        err(r, 400, LE_INVALID, "local_only must be a boolean");
        return 1;
    }
    if (parsed > 0 && value &&
        !strcmp(c->voice_pipeline_mode, "custom")) {
        err(r, 409, LE_BUSY,
            "Disable the network voice pipeline before requiring local processing");
        return 1;
    }
    if (parsed > 0)
        c->privacy_local_only = value;

    parsed = json_get_bool(q->body, "diagnostic_telemetry", &value);
    if (parsed < 0) {
        err(r, 400, LE_INVALID,
            "diagnostic_telemetry must be a boolean");
        return 1;
    }
    if (parsed > 0)
        c->privacy_telemetry = value;

    parsed = json_get_bool(q->body, "crash_reports", &value);
    if (parsed < 0) {
        err(r, 400, LE_INVALID, "crash_reports must be a boolean");
        return 1;
    }
    if (parsed > 0)
        c->privacy_crash_reports = value;

    parsed = json_get_string(q->body, "audio_retention",
                             retention, sizeof(retention));
    if (parsed < 0 || (parsed > 0 && strcmp(retention, "none") &&
                       strcmp(retention, "24h"))) {
        err(r, 400, LE_INVALID, "audio_retention must be none or 24h");
        return 1;
    }
    if (parsed > 0)
        c->privacy_audio_retention = strcmp(retention, "none") != 0;

    parsed = json_get_int(q->body, "log_retention_hours", &value);
    if (parsed < 0 || (parsed > 0 && value != 24 &&
                       value != 168 && value != 720)) {
        err(r, 400, LE_INVALID,
            "log_retention_hours must be 24, 168, or 720");
        return 1;
    }
    if (parsed > 0)
        c->privacy_log_hours = value;

    rc = persist_configuration(c);
    if (rc) {
        err(r, 503, rc, "Privacy configuration could not be saved");
        return 1;
    }

respond:
    out(r, 200,
        "{\"ok\":true,\"data\":{\"local_only\":%s,"
        "\"audio_retention\":\"%s\",\"diagnostic_telemetry\":%s,"
        "\"log_retention_hours\":%d,\"crash_reports\":%s},"
        "\"error\":null}",
        c->privacy_local_only ? "true" : "false",
        c->privacy_audio_retention ? "24h" : "none",
        c->privacy_telemetry ? "true" : "false",
        c->privacy_log_hours,
        c->privacy_crash_reports ? "true" : "false");
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
void api_handle(struct api_context*c,const struct api_request*q,struct api_response*r){int rc;if(!security(c,q,r))return;if(changing(q->method)&&q->body_len&&!body_ok(q,r))return;if(!strcmp(q->path,"/api/v1/setup")){if(!strcmp(q->method,"GET")){setup_json(c,r);return;}if(!strcmp(q->method,"POST")){rc=setup_apply(c,q->body);if(rc){err(r,rc==LE_BUSY?409:rc==LE_INVALID?400:rc==LE_NOT_SUPPORTED?501:503,rc,rc==LE_BUSY?"Initial setup has already been completed":rc==LE_INVALID?"Setup details are invalid or incomplete":rc==LE_NOT_SUPPORTED?"A required hardware adapter is not available":"Initial setup could not be applied");return;}api_log(c,"info","Initial setup completed; access-point handoff requested");event_bus_publish(&c->events,"device_state","{\"setup_completed\":true}");ok(r,"{\"completed\":true,\"network_state\":\"connecting\",\"ap_mode_exit_required\":true,\"password_stored\":false}");return;}method_not_allowed(r);return;}if((!strcmp(q->path,"/api/v1")||!strcmp(q->path,"/api/v1/"))&&!strcmp(q->method,"GET")){ok(r,"{\"name\":\"LibreEcho API\",\"version\":\"v1\",\"status\":\"/api/v1/status\",\"playback\":\"/api/v1/playback\",\"setup\":\"/api/v1/setup\",\"openapi\":\"/openapi.json\",\"swagger\":\"/swagger.html\"}");return;}if(!strcmp(q->path,"/api/v1/config")&&!strcmp(q->method,"GET")){out(r,200,"{\"ok\":true,\"data\":{\"api_version\":1,\"csrf_token\":\"%s\",\"authentication\":\"%s\",\"bootstrap_required\":%s,\"user_count\":%zu,\"bind_policy\":\"%s\",\"max_request_body\":16384,\"setup_completed\":%s,\"setup_url\":\"/setup.html\"},\"error\":null}",c->csrf_token,api_bootstrap_required_internal(c)?"bootstrap-required":c->auth.enabled?"users":c->auth_token[0]?"bearer-token":"development-disabled",api_bootstrap_required_internal(c)?"true":"false",c->auth.user_count,c->allow_insecure_lan?"lan-development":"loopback-default",c->setup_completed?"true":"false");return;}api_handle_inner(c,q,r);}

int api_persist_configuration(struct api_context*c){return persist_configuration(c);}
int api_baby_monitor_stream_authorize(struct api_context*c,const struct api_request*q,struct api_response*r,int*card,int*device,int*channels,int*bits,int*channel){const char*prefix="/api/v1/baby-monitor/stream?source=";const char*value;char source[32],pcm[64],extra;size_t n;unsigned parsed_card,parsed_device,parsed_channel=0;if(!c||!q||!r||!card||!device||!channels||!bits||!channel)return 0;if(!security(c,q,r))return 0;if(strcmp(q->method,"GET")){method_not_allowed(r);return 0;}if(strncmp(q->path,prefix,strlen(prefix))){err(r,404,LE_INVALID,"Baby-monitor stream was not found");return 0;}value=q->path+strlen(prefix);n=strcspn(value,"&");if(n==0||query_component_decode(source,sizeof(source),value,n)){err(r,400,LE_INVALID,"A valid microphone source is required");return 0;}if(sscanf(source,"%u:%u%c",&parsed_card,&parsed_device,&extra)!=2||parsed_card>31||parsed_device>31){err(r,400,LE_INVALID,"A valid microphone source is required");return 0;}if(n&&value[n]){const char*ch=strstr(value+n,"&channel=");if(ch&&sscanf(ch+9,"%u%c",&parsed_channel,&extra)!=1){err(r,400,LE_INVALID,"A valid microphone channel is required");return 0;}}if(!strcmp(le_backend_mode(c->backend),"mock")){err(r,501,LE_NOT_SUPPORTED,"Microphone streaming is unavailable in the mock backend");return 0;}if(parsed_card!=0||parsed_device!=24||parsed_channel>6){err(r,400,LE_INVALID,"The selected Echo microphone channel is invalid");return 0;}snprintf(pcm,sizeof(pcm),"/dev/snd/pcmC%uD%uc",parsed_card,parsed_device);if(access(pcm,R_OK)){err(r,503,LE_NOT_SUPPORTED,"Selected microphone source is unavailable");return 0;}*card=(int)parsed_card;*device=(int)parsed_device;*channels=9;*bits=24;*channel=(int)parsed_channel;return 1;}
