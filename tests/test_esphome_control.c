/* Host-only control-plane contract. Every init/status/config path is injected. */
#define _POSIX_C_SOURCE 200809L
#ifndef LE_TEST_ROOT
#error LE_TEST_ROOT must name a private fixture directory
#endif
#define LE_INIT_AGENTD LE_TEST_ROOT "/agentd.init"
#define LE_INIT_STTD LE_TEST_ROOT "/sttd.init"
#define LE_INIT_TTSD LE_TEST_ROOT "/ttsd.init"
#define LE_INIT_ESPHOMED LE_TEST_ROOT "/esphomed.init"
#define LE_INIT_WYOMINGD LE_TEST_ROOT "/removed-wyomingd.init"
#define LE_INIT_MDNSD LE_TEST_ROOT "/mdnsd.init"
#include "../src/api.c"
#include "../src/backend_internal.h"
#include <assert.h>

static struct api_context ctx;
static struct api_response response;
static const struct le_backend_ops fixture_ops = {0};
static struct le_backend backend = { .ops = &fixture_ops, .mode = "linux" };
static void save(const char *path, const char *text) {
    assert(config_write_atomic(path,text,strlen(text))==0);
}
static void fixture(const char *name, int fail_start) {
    char path[512], script[4096];
    snprintf(path,sizeof(path),LE_TEST_ROOT "/%s.init",name);
    snprintf(script,sizeof(script),"#!/bin/sh\nprintf '%%s %%s\\n' '%s' \"$1\" >> '%s/actions'\ncase \"$1\" in start) sleep 0.1; exit %d;; esac\nexit 0\n",name,LE_TEST_ROOT,fail_start);
    if(!strcmp(name,"esphomed")&&!fail_start)
        snprintf(script,sizeof script,"#!/bin/sh\nprintf 'esphomed %%s\\n' \"$1\" >> '%s/actions'\nDAEMON=\"$LE_TEST_ESPHOMED_BIN\" PIDFILE='%s/esphomed.pid' STATUS_FILE='%s/status.json' LOGFILE='%s/esphomed.log' DEFAULTS='%s/no-defaults' PORT=\"$LIBREECHO_ESPHOME_PORT\" BIND=127.0.0.1 CONFIG='%s/config.json' WAKE_SOCKET='%s/wake.sock' AUDIO_SOCKET='%s/audio.sock' AUDIO_BUS='%s/system.pcm' MDNS_SOCKET='%s/mdns.sock' RADIO_SOCKET='%s/radio.sock' TIMER_SOCKET='%s/timer.sock' LED_SOCKET='%s/led.sock' PRIVACY_STATE='%s/privacy' IDME_ROOT='%s/idme' TLS_CA='%s/no-ca' exec sh \"$LE_TEST_ESPHOMED_INIT\" \"$1\"\n",LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT,LE_TEST_ROOT);
    save(path,script); assert(chmod(path,0700)==0);
}
static void setup(const char *mode) {
    memset(&ctx,0,sizeof(ctx)); ctx.backend=&backend;
    snprintf(ctx.config_path,sizeof(ctx.config_path),LE_TEST_ROOT "/config.json");
    snprintf(ctx.voice_pipeline_mode,sizeof(ctx.voice_pipeline_mode),"%s",mode);
    snprintf(ctx.stt_wyoming_uri,sizeof(ctx.stt_wyoming_uri),"tcp://127.0.0.1:10300");
    snprintf(ctx.tts_wyoming_uri,sizeof(ctx.tts_wyoming_uri),"tcp://127.0.0.1:10200");
    snprintf(ctx.stt_wyoming_model,sizeof(ctx.stt_wyoming_model),"saved-whisper");
    snprintf(ctx.tts_wyoming_voice,sizeof(ctx.tts_wyoming_voice),"saved-piper");
    ctx.stt_max_utterance_ms=6000;ctx.stt_end_silence_ms=1000;ctx.stt_vad_floor_rms=16;
    snprintf(ctx.button_action,sizeof(ctx.button_action),"sound");ctx.button_action_brightness=37;
    snprintf(ctx.mac_wifi,sizeof(ctx.mac_wifi),"02:11:22:33:44:55");
    fixture("agentd",0);fixture("sttd",0);fixture("ttsd",0);fixture("esphomed",0);
    setenv("LIBREECHO_ESPHOMED_PIDFILE",LE_TEST_ROOT "/esphomed.pid",1);
    setenv("LIBREECHO_ESPHOME_STATUS_FILE",LE_TEST_ROOT "/status.json",1);
    assert(persist_configuration(&ctx)==LE_OK);
    if(!strcmp(mode,"home-assistant"))assert(run_init_command(LE_INIT_ESPHOMED,"start")==0);
}
static int put(const char *body) {
    struct api_request request={0};snprintf(request.path,sizeof(request.path),"/api/v1/voice-pipeline");snprintf(request.method,sizeof(request.method),"PUT");request.body=body;request.body_len=strlen(body);
    memset(&response,0,sizeof(response));assert(handle_voice_pipeline(&ctx,&request,&response));return response.status;
}
static void finish(void) {
    struct timespec delay={0,20000000};unsigned i;
    for(i=0;i<500;i++){voice_pipeline_json(&ctx,&response);if(!voice_pipeline_restart_pending())return;nanosleep(&delay,NULL);}
    assert(!"bounded transition timed out");
}
static void protocol(void) {
    char saved[16384],protocol[32];
    setup("home-assistant");voice_pipeline_json(&ctx,&response);
    assert(config_copy_defaults(LE_TEST_ROOT "/esphomed.pid",LE_TEST_ROOT "/creator.pid")==0);
    assert(config_copy_defaults(LE_TEST_ROOT "/status.json",LE_TEST_ROOT "/creator.json")==0);
    assert(json_get_string(response.body,"protocol",protocol,sizeof(protocol))==1);
    assert(!strcmp(protocol,"esphome"));
    { int port,ready,connected;assert(json_get_int(response.body,"port",&port)==1&&port==6053);assert(json_get_bool(response.body,"ready",&ready)==1&&ready);assert(json_get_bool(response.body,"connected",&connected)==1&&!connected); }
    save(LE_TEST_ROOT "/status.json","{\"ready\":true,\"connected\":true}");
    voice_pipeline_json(&ctx,&response);
    { int ready,connected;assert(json_get_bool(response.body,"ready",&ready)==1&&!ready);assert(json_get_bool(response.body,"connected",&connected)==1&&!connected); }
    unlink(LE_TEST_ROOT "/esphomed.pid");voice_pipeline_json(&ctx,&response);
    { int connected;assert(json_get_bool(response.body,"connected",&connected)==1&&!connected); }
    assert(config_read(ctx.config_path,saved,sizeof(saved))>0);
    assert(json_get_string(saved,"ha_protocol",protocol,sizeof(protocol))==1&&!strcmp(protocol,"esphome"));
    /* Dead pidfile, missing/malformed status, and symlinks all fail closed. */
    save(LE_TEST_ROOT "/esphomed.pid","99999999\n");
    voice_pipeline_json(&ctx,&response);
    {int ready;assert(json_get_bool(response.body,"ready",&ready)==1&&!ready);}
    {char pid[64];snprintf(pid,sizeof(pid),"%ld\n",(long)getpid());save(LE_TEST_ROOT "/esphomed.pid",pid);}
    for(int k=0;k<3;k++){
        save(LE_TEST_ROOT "/status.json",k==0?"{}":k==1?"{bad}":"{\"ready\":false,\"connected\":false}");
        voice_pipeline_json(&ctx,&response);
        {int ready,connected;assert(json_get_bool(response.body,"ready",&ready)==1&&!ready);assert(json_get_bool(response.body,"connected",&connected)==1&&!connected);}
    }
    unlink(LE_TEST_ROOT "/status.json");
    assert(symlink(LE_TEST_ROOT "/config.json",LE_TEST_ROOT "/status.json")==0);
    voice_pipeline_json(&ctx,&response);
    {int ready;assert(json_get_bool(response.body,"ready",&ready)==1&&!ready);}
    unlink(LE_TEST_ROOT "/status.json");
    /* Missing and old protocol values have one canonical interpretation. */
    {struct le_esphome_config config;const char *values[]={"{}","{\"ha_protocol\":\"wyoming\"}","{\"ha_protocol\":\"invalid\"}"};
     for(size_t k=0;k<3;k++){save(ctx.config_path,values[k]);assert(config_esphome_read(ctx.config_path,&config)==0);assert(!strcmp(config.ha_protocol,"esphome"));assert(!config.esphome_noise_key[0]);}}

    save(ctx.config_path,"{\"integrations\":1,\"ha_protocol\":\"wyoming\"}");
    ctx.voice_pipeline_mode[0]=0;ctx.voice_pipeline_previous_valid=0;
    voice_pipeline_json(&ctx,&response);assert(!strcmp(ctx.voice_pipeline_mode,"home-assistant"));
    puts("ESPHome protocol and daemon connection status: PASS");
}
static void secrets(void) {
    char saved[16384],exported[8192];struct stat st;
    setup("custom");
    save(ctx.config_path,"{\"ha_protocol\":\"wyoming\",\"esphome_noise_key\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}");
    assert(persist_configuration(&ctx)==LE_OK);
    assert(config_read(ctx.config_path,saved,sizeof(saved))>0);
    assert(strstr(saved,"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
    assert(configuration_json(&ctx,exported,sizeof(exported))==LE_OK);
    assert(!strstr(exported,"esphome_noise_key")&&!strstr(exported,"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
    assert(stat(ctx.config_path,&st)==0&&(st.st_mode&0777)==0600);
    /* Existing permissive config must not become a readable key backup. */
    assert(chmod(ctx.config_path,0644)==0);
    assert(persist_configuration(&ctx)==LE_OK);
    {char path[512];snprintf(path,sizeof(path),"%s.bak",ctx.config_path);assert(stat(path,&st)==0&&(st.st_mode&0777)==0600);}
    {struct le_esphome_config config;const char *invalid[]={
      "{\"esphome_noise_key\":123}","{\"esphome_noise_key\":\"short\"}",
      "{\"esphome_noise_key\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB=\"}",
      "{\"esphome_noise_key\":\"\",\"esphome_noise_key\":\"\"}"};
     for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){save(ctx.config_path,invalid[i]);assert(config_esphome_read(ctx.config_path,&config)<0);}}
    {struct le_esphome_config config;char too_large[17000];memset(too_large,' ',sizeof(too_large));
     too_large[0]='{';too_large[sizeof(too_large)-1]='}';
     assert(config_write_atomic(ctx.config_path,too_large,sizeof(too_large))==0);
     errno=ENOENT; /* a previous optional-path check must not turn truncation into unprovisioned */
     assert(config_esphome_read(ctx.config_path,&config)<0);}
    save(ctx.config_path,"{}"); /* do not leak malformed config into the next case */
    puts("ESPHome key validation, persistence and ordinary export redaction: PASS");
}
static void transitions(void) {
    const char *modes[]={"local","custom"};size_t i;
    for(i=0;i<2;i++){
        setup(modes[i]);assert(put("{\"mode\":\"home-assistant\"}")==202);
        /* The inner integration handler has already set its requested bit.
         * Reject a duplicate enable without clearing the accepted HA intent. */
        {struct api_request q={0};snprintf(q.method,sizeof(q.method),"PUT");snprintf(q.path,sizeof(q.path),"/api/v1/integrations/home-assistant");q.body="{\"enabled\":true}";response.status=200;ctx.integrations|=1u;after_integration_change(&ctx,&q,&response);assert(response.status==409);assert(ctx.integrations&1u);}
        finish();
        assert(!strcmp(voice_pipeline_restart_state(),"ready"));
        assert(!strcmp(ctx.voice_pipeline_previous_mode,modes[i]));
        {char actions[4096];assert(config_read(LE_TEST_ROOT "/actions",actions,sizeof(actions))>0);
         assert(strstr(actions,"agentd stop")&&strstr(actions,"sttd stop")&&strstr(actions,"ttsd stop")&&strstr(actions,"esphomed start"));}
        assert(ctx.button_action_brightness==37&&!strcmp(ctx.mac_wifi,"02:11:22:33:44:55"));
        {struct api_request q={0};snprintf(q.method,sizeof(q.method),"PUT");snprintf(q.path,sizeof(q.path),"/api/v1/integrations/home-assistant");q.body="{\"enabled\":false}";response.status=200;ctx.integrations&=~1u;after_integration_change(&ctx,&q,&response);assert(response.status==202);}
        finish();assert(!strcmp(ctx.voice_pipeline_mode,modes[i]));
        {char actions[4096];assert(config_read(LE_TEST_ROOT "/actions",actions,sizeof(actions))>0);
         assert(strstr(actions,"esphomed stop")&&strstr(actions,"sttd start")&&strstr(actions,"ttsd start")&&strstr(actions,"agentd start"));}
        assert(!strcmp(ctx.stt_wyoming_model,"saved-whisper")&&!strcmp(ctx.tts_wyoming_voice,"saved-piper"));
        fixture("esphomed",1);assert(put("{\"mode\":\"home-assistant\"}")==202);
        ctx.button_tones=1;assert(persist_configuration(&ctx)==LE_OK);finish();
        assert(!strcmp(voice_pipeline_restart_state(),"failed"));
        assert(!strcmp(ctx.voice_pipeline_mode,modes[i]));assert(!(ctx.integrations&1u)&&ctx.button_tones==1);
        {char saved[16384],mode[32];assert(config_read(ctx.config_path,saved,sizeof(saved))>0);assert(json_get_string(saved,"voice_pipeline_mode",mode,sizeof(mode))==1&&!strcmp(mode,modes[i]));}
        (void)put("{\"mode\":\"local\"}"); /* acknowledge failure before another transition */
        unlink(LE_INIT_ESPHOMED);assert(put("{\"mode\":\"home-assistant\"}")==501);
        assert(!strcmp(ctx.voice_pipeline_mode,modes[i]));
    }
    setup("home-assistant");ctx.integrations=1u;
    ctx.voice_pipeline_previous_valid=1;snprintf(ctx.voice_pipeline_previous_mode,sizeof(ctx.voice_pipeline_previous_mode),"custom");
    save(LE_INIT_ESPHOMED,"#!/bin/sh\ncase \"$1\" in stop) exit 1;; esac\nexit 0\n");assert(chmod(LE_INIT_ESPHOMED,0700)==0);
    unlink(LE_TEST_ROOT "/actions");assert(put("{\"mode\":\"local\"}")==202);finish();
    assert(!strcmp(ctx.voice_pipeline_mode,"home-assistant"));
    {char actions[4096];assert(config_read(LE_TEST_ROOT "/actions",actions,sizeof(actions))>0);assert(!strstr(actions,"sttd start"));}
    puts("Local/Custom <-> HA, failure rollback, concurrent settings and missing daemon: PASS");
}
static void wake_save(void) {
    const char *values[]={"{}","{\"esphome_active_wake_word\":\"\"}","{\"esphome_active_wake_word\":\"alexa_v0.1\"}"};
    const char *expected[]={NULL,"","alexa_v0.1"};
    setup("custom");
    for(size_t i=0;i<3;i++){
        char saved[16384],word[64];struct le_esphome_config loaded;
        save(ctx.config_path,values[i]);
        assert(config_esphome_read(ctx.config_path,&loaded)==0);
        assert(loaded.esphome_active_wake_word_present==(expected[i]?1:0));
        assert(!expected[i]||!strcmp(loaded.esphome_active_wake_word,expected[i]));
        ctx.button_tones=(int)i; /* ordinary unrelated setting save */
        assert(api_persist_configuration(&ctx)==LE_OK);
        assert(config_read(ctx.config_path,saved,sizeof(saved))>0);
        int present=json_get_string_top_level(saved,"esphome_active_wake_word",word,sizeof word);
        assert(present==(expected[i]?1:0));
        assert(!expected[i]||!strcmp(word,expected[i]));
        assert(config_esphome_read(ctx.config_path,&loaded)==0);
        assert(loaded.esphome_active_wake_word_present==(expected[i]?1:0));
        assert(!expected[i]||!strcmp(loaded.esphome_active_wake_word,expected[i]));
        /* Re-read and repeat another ordinary save, not a secret merge helper. */
        assert(api_persist_configuration(&ctx)==LE_OK);
        assert(config_read(ctx.config_path,saved,sizeof(saved))>0);
        present=json_get_string_top_level(saved,"esphome_active_wake_word",word,sizeof word);
        assert(present==(expected[i]?1:0));assert(!expected[i]||!strcmp(word,expected[i]));
    }
    puts("ESPHome missing/default, disabled and selected wake choice survives ordinary saves: PASS");
}
int main(int argc,char **argv){assert(argc==2);if(!strcmp(argv[1],"wake-save"))wake_save();else if(!strcmp(argv[1],"protocol"))protocol();else if(!strcmp(argv[1],"secrets"))secrets();else transitions();return 0;}
