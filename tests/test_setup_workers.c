#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
static int fail_fork;
static pid_t config_test_fork(void) {
    if (fail_fork) { errno = EAGAIN; return -1; }
    return fork();
}
#define fork config_test_fork
#include "../src/http_server.c"
#undef fork
#include "../src/backend_internal.h"
#include <sys/time.h>

static const char csrf[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char token[] = "isolated-configuration-test-token";
static const char body[] = "{\"hostname\":\"fixture-echo\",\"ssid\":\"fixture\",\"security\":\"open\",\"password\":\"\",\"volume\":31,\"wake_word\":\"Alexa\",\"wake_sensitivity\":67,\"local_only\":false,\"diagnostic_telemetry\":true}";
static int fail_connect;
static int delayed_connect(struct le_backend *b, const struct le_wifi_credentials *w) {
    struct timespec pause = {0, 400000000L};
    (void)b; (void)w;
    nanosleep(&pause, NULL);
    return fail_connect ? LE_IO : LE_OK;
}
static int airplay_set(struct le_backend *b, int enabled) { (void)b; (void)enabled; return LE_OK; }
static int airplay_state(struct le_backend *b, struct le_airplay_state *a) {
    (void)b; memset(a, 0, sizeof(*a)); a->available=1; return LE_OK;
}
static long millis(void) {
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t)==0);
    return t.tv_sec*1000 + t.tv_nsec/1000000;
}
static int dispatch(struct api_context *api, const char *method, const char *path,
                    const char *data, const char *auth, const char *csrf_value,
                    const char *origin) {
    int pair[2]; struct client c; struct http_options o; struct timeval limit={3,0};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair)==0);
    assert(setsockopt(pair[1],SOL_SOCKET,SO_RCVTIMEO,&limit,sizeof(limit))==0);
    memset(&c,0,sizeof(c)); memset(&o,0,sizeof(o)); c.fd=pair[0]; strcpy(o.web_root,"web");
    c.used=(size_t)snprintf(c.buf,sizeof(c.buf),
        "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: %s\r\nAuthorization: %s\r\nX-LibreEcho-CSRF: %s\r\nContent-Length: %zu\r\n\r\n%s",
        method,path,origin,auth,csrf_value,strlen(data),data);
    assert(c.used<sizeof(c.buf));
    process(&c,&o,api,-1,-1,-1,NULL,0);
    assert(c.fd==-1);
    return pair[1];
}
static int request(struct api_context *api,const char *method,const char *path,const char *data) {
    return dispatch(api,method,path,data,"Bearer isolated-configuration-test-token",csrf,"http://127.0.0.1");
}
static void expect(int fd, int status, const char *text) {
    char reply[40000], expected[32]; size_t used=0; ssize_t n;
    while ((n=read(fd,reply+used,sizeof(reply)-1-used))>0) { used+=(size_t)n; assert(used<sizeof(reply)-1); }
    assert(n==0); reply[used]=0; close(fd);
    snprintf(expected,sizeof(expected),"HTTP/1.1 %d",status);
    if (!strstr(reply,expected) || (text && !strstr(reply,text))) { fprintf(stderr,"Unexpected response: %s\n",reply); abort(); }
}
static void finish(struct api_context *api) {
    struct timespec pause={0,1000000L}; int i;
    for(i=0;i<200 && config_worker_pending;i++){sync_configuration_worker(api);nanosleep(&pause,NULL);}
    assert(!config_worker_pending); assert(child_worker_counts[CHILD_WORKER_CONFIG]==0);
}
int main(void) {
    char dir[]="/tmp/libreecho-setup-worker.XXXXXX", cfg[384], marker[512], ready[512], auth[192];
    struct le_backend *backend; struct le_backend_ops ops; struct api_context api, restarted;
    int fd; long start; char saved[16384];
    alarm(20); assert(mkdtemp(dir));
    snprintf(cfg,sizeof(cfg),"%s/config.json",dir);
    snprintf(marker,sizeof(marker),"%s.setup-complete",cfg);
    snprintf(ready,sizeof(ready),"%s/ready",dir);
    assert(config_write_atomic(ready,"schema=1\n",9)==0);
    assert(setenv("LIBREECHO_SETUP_FEATURE_ACTIVATOR","/bin/true",1)==0);
    assert(setenv("LIBREECHO_SETUP_STARTUP_READY",ready,1)==0);
    assert(le_backend_init(&backend,"mock",NULL,NULL,7)==LE_OK);
    ops=*backend->ops; ops.connect=delayed_connect; ops.airplay=airplay_state; ops.airplay_set=airplay_set;
    backend->ops=&ops; strcpy(backend->mode,"linux");
    assert(api_init(&api,backend,1,0,token,NULL,csrf,cfg,NULL)==0);
    strcpy(auth,api.auth_token);
    /* Unauthorized or malformed operations never allocate a worker. */
    expect(dispatch(&api,"POST","/api/v1/setup",body,"",csrf,"http://127.0.0.1"),401,NULL);
    expect(dispatch(&api,"POST","/api/v1/setup",body,"Bearer isolated-configuration-test-token","bad","http://127.0.0.1"),403,NULL);
    expect(dispatch(&api,"POST","/api/v1/setup",body,"Bearer isolated-configuration-test-token",csrf,"http://untrusted.invalid"),403,NULL);
    expect(request(&api,"POST","/api/v1/setup","{"),400,NULL);
    assert(!config_worker_pending);
    /* Failed fork neither consumes a slot nor closes another descriptor. */
    fail_fork=1; expect(request(&api,"POST","/api/v1/setup",body),503,NULL); fail_fork=0;
    assert(!config_worker_pending && child_worker_counts[CHILD_WORKER_CONFIG]==0);
    fail_connect=1;
    start=millis();fd=request(&api,"POST","/api/v1/setup",body);
    assert(millis()-start<250);
    expect(request(&api,"GET","/api/v1/config",""),200,"\"setup_completed\":false");
    assert(millis()-start<250);
    expect(request(&api,"PUT","/api/v1/buttons","{\"tones\":false}"),409,"\"busy\"");
    expect(request(&api,"POST","/api/v1/setup",body),409,"\"busy\"");
    expect(fd,503,"Wi-Fi connection could not be completed");finish(&api);
    assert(!api.setup_completed && access(marker,F_OK)!=0);
    puts("setup worker: responsive reads, failure, serialization and auth: ok");
    fail_connect=0;
    start=millis();fd=request(&api,"POST","/api/v1/setup",body);assert(millis()-start<250);
    expect(fd,200,"\"completed\":true");
    /* Success must be visible on the next request, even before reaping. */
    expect(request(&api,"GET","/api/v1/config",""),200,"\"setup_completed\":true");
    assert(api.setup_completed && !api.privacy_local_only && api.privacy_telemetry);
    assert(api.configured_wake_sensitivity==67 && !strcmp(api.auth_token,auth));
    expect(request(&api,"GET","/",""),200,"LibreEcho Control Centre");
    finish(&api);
    expect(request(&api,"PUT","/api/v1/buttons","{\"tones\":false}"),200,NULL);
    assert(config_read(cfg,saved,sizeof(saved))>0);
    assert(strstr(saved,"\"privacy_local_only\": false") && strstr(saved,"\"button_tones\": false"));
    assert(api_init(&restarted,backend,1,0,token,NULL,csrf,cfg,NULL)==0);
    assert(refresh_setup_completed(&restarted)==0 && restarted.setup_completed);
    assert(!restarted.privacy_local_only && restarted.privacy_telemetry && !restarted.button_tones);
    puts("setup worker: completed state, subsequent writes and restart: ok");
    start=millis();fd=request(&api,"POST","/api/v1/network/wifi/connect","{\"ssid\":\"fixture\",\"security\":\"open\"}");
    assert(millis()-start<250);
    expect(request(&api,"GET","/api/v1/config",""),200,NULL);assert(millis()-start<250);
    expect(fd,200,"connecting");finish(&api);
    puts("Wi-Fi connect uses the bounded asynchronous worker: ok");
    /* Mock operations remain in the parent, preserving mock backend state. */
    strcpy(backend->mode,"mock");
    {struct api_request q;memset(&q,0,sizeof(q));strcpy(q.path,"/api/v1/setup");strcpy(q.method,"POST");assert(!configuration_worker_request(&api,&q));}
    le_backend_destroy(backend);
    unlink(cfg);unlink(marker);unlink(ready);
    snprintf(marker,sizeof(marker),"%s.bak",cfg);unlink(marker);rmdir(dir);
    return 0;
}
