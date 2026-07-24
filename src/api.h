#ifndef LE_API_H
#define LE_API_H
#include "backend.h"
#include "auth.h"
#include "event_bus.h"
#include <stddef.h>
struct api_request{char method[8],path[256],origin[256],authorization[256],csrf[96],confirm[96];const char*body;size_t body_len;};
struct api_response{int status;char type[64];char body[32768];size_t length;};
struct api_context{struct le_backend*backend;struct le_event_bus events;struct le_auth_db auth;int dev_controls,allow_insecure_lan,setup_completed,privacy_local_only,privacy_audio_retention,privacy_telemetry,privacy_crash_reports,privacy_log_hours,net_ssh,net_api_lan;unsigned integrations,auth_failures;time_t auth_window_started,auth_blocked_until;char button_short[32],button_long[32],auth_token[192],allowed_origin[256],csrf_token[65],config_path[384];char logs[LE_MAX_LOGS][256];size_t log_count,log_next;};
int api_init(struct api_context*,struct le_backend*,int,int,const char*,const char*,const char*,const char*,const char*);int api_persist_configuration(struct api_context*);void api_log(struct api_context*,const char*,const char*);void api_handle(struct api_context*,const struct api_request*,struct api_response*);
#endif
