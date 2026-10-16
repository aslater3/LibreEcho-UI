#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_BACKEND_LINUX_TESTING
#include "../src/backend_linux.c"
#include "../src/wake_diagnostic.h"
#include <assert.h>
static const char *reply;
int le_backend_linux_test_adapter_command(const char *sock, const char *cmd,
 const char *args, char *out, size_t size)
{ (void)sock;(void)cmd;(void)args;snprintf(out,size,"%s",reply);return LE_OK; }
int main(void)
{
 struct le_wake_word_state w;
 assert(le_wake_age_ms(9000000,8000000)==1);
 assert(le_wake_age_ms(9000000,0)==-1);
 assert(le_wake_age_ms(1,2)==-1);
 assert(le_wake_age_ms(UINT64_MAX,1)==INT_MAX);
 reply="{\"enabled\":true,\"model_status\":\"loaded\"}";
 assert(wake(NULL,&w)==LE_OK && !w.health_available);
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,0,0),"degraded"));
 reply="{\"enabled\":true,\"model_loaded\":true,\"capture_active\":true,\"processed_frames\":50,\"capture_age_ms\":3,\"inference_active\":true,\"inference_age_ms\":100}";
 assert(wake(NULL,&w)==LE_OK && w.health_available);
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,0,0),"ok"));
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,0,1),"development"));
 w.capture_age_ms=2001;
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,0,0),"degraded"));
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,1,0),"muted"));
 w.enabled=0;
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,0,0),"disabled"));
 reply="{\"enabled\":true,\"model_loaded\":true,\"capture_active\":true,\"processed_frames\":50,\"capture_age_ms\":3000,\"inference_active\":true,\"inference_age_ms\":100}";
 assert(wake(NULL,&w)==LE_OK && !w.capture_active);
 assert(!strcmp(le_wake_diagnostic(&w,LE_OK,0,0),"degraded"));
 reply="{\"enabled\":true,\"model_loaded\":true,\"capture_active\":true,\"processed_frames\":50,\"capture_age_ms\":0,\"inference_active\":true,\"inference_age_ms\":5001}";
 assert(wake(NULL,&w)==LE_OK && !w.inference_active);
 reply="{\"enabled\":true,\"model_loaded\":true,\"capture_active\":true,\"processed_frames\":-1,\"capture_age_ms\":0,\"inference_active\":true,\"inference_age_ms\":0}";
 assert(wake(NULL,&w)==LE_OK && !w.health_available);
 reply="{\"enabled\":true,\"model_loaded\":true,\"capture_active\":true,\"processed_frames\":\"50\",\"capture_age_ms\":0,\"inference_active\":true,\"inference_age_ms\":0}";
 assert(wake(NULL,&w)==LE_OK && !w.health_available);
 puts("wake health: live, stalled, unknown, disabled, muted and invalid evidence: ok");
 return 0;
}
