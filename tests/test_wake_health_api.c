#define _POSIX_C_SOURCE 200809L
#include "../src/api.h"
#include "../src/config_store.h"
#include "../src/json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char csrf[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void request(struct api_context *c, const char *path, struct api_response *r)
{
 struct api_request q; memset(&q,0,sizeof(q)); memset(r,0,sizeof(*r));
 strcpy(q.method,"GET");snprintf(q.path,sizeof(q.path),"%s",path);q.body="";
 api_handle(c,&q,r);assert(r->status==200);assert(json_valid_object(r->body,strlen(r->body)));
}
int main(void)
{
 char path[]="/tmp/le-wake-health-XXXXXX";int fd=mkstemp(path);unsigned i;
 const struct {const char *config,*status;} cases[]={
  {"{}","development"},
  {"{\"wake_capture_age_ms\":3000}","degraded"},
  {"{\"wake_inference_age_ms\":6000}","degraded"},
  {"{\"wake_model_loaded\":false}","degraded"},
  {"{\"wake_enabled\":false,\"wake_capture_age_ms\":-1}","disabled"},
  {"{\"microphone_muted\":true,\"wake_capture_age_ms\":3000}","muted"}
 };
 assert(fd>=0);close(fd);
 for(i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
  struct le_backend *b=NULL;struct api_context c;struct api_response r;char expected[100];int value;
  FILE *f=fopen(path,"w");assert(f);fputs(cases[i].config,f);fclose(f);
  assert(le_backend_init(&b,"mock",NULL,path,7)==LE_OK);
  assert(api_init(&c,b,1,1,NULL,NULL,csrf,NULL,NULL)==0);
  request(&c,"/api/v1/diagnostics",&r);
  snprintf(expected,sizeof(expected),"\"name\":\"wake word\",\"status\":\"%s\"",cases[i].status);
  assert(strstr(r.body,expected));
  request(&c,"/api/v1/wake-word",&r);
  assert(json_get_bool(r.body,"health_available",&value)==1 && value);
  assert(json_get_int(r.body,"capture_age_ms",&value)==1);
  assert(strstr(r.body,"\"processed_frames\":1"));
  le_backend_destroy(b);
 }
 unlink(path);puts("wake API: production response and diagnostic reflect capture/inference stalls: ok");return 0;
}
