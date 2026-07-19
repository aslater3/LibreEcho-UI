#ifndef LE_API_H
#define LE_API_H
#include "backend.h"
#include "event_bus.h"
#include <stddef.h>
struct api_request{char method[8],path[256],origin[256],csrf[96],confirm[96];const char*body;size_t body_len;};
struct api_response{int status;char type[64];char body[32768];size_t length;};
struct api_context{struct le_backend*backend;struct le_event_bus events;int dev_controls;char logs[LE_MAX_LOGS][256];size_t log_count,log_next;};
void api_init(struct api_context*,struct le_backend*,int);void api_log(struct api_context*,const char*,const char*);void api_handle(struct api_context*,const struct api_request*,struct api_response*);
#endif
