#ifndef LIBREECHO_LLM_HTTP_H
#define LIBREECHO_LLM_HTTP_H

#include "llm_provider.h"

#include <stddef.h>

typedef int (*le_llm_http_event_fn)(void *context, const char *data);
typedef int (*le_llm_http_body_fn)(void *context, const char *data,
                                   size_t size);

struct le_llm_http_response {
    int status;
    char body[LE_LLM_BODY_MAX];
};

int le_llm_http_execute(const char *curl_path,
                        const struct le_llm_http_request *request,
                        le_llm_http_event_fn event_fn,
                        void *event_context,
                        struct le_llm_http_response *response);

/* Stream a bounded-body-independent response to body_fn without buffering it. */
int le_llm_http_execute_stream(const char *curl_path,
                               const struct le_llm_http_request *request,
                               le_llm_http_body_fn body_fn,
                               void *body_context,
                               struct le_llm_http_response *response);
#endif
