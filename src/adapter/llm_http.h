#ifndef LIBREECHO_LLM_HTTP_H
#define LIBREECHO_LLM_HTTP_H

#include "llm_provider.h"

#include <stddef.h>

typedef int (*le_llm_http_event_fn)(void *context, const char *data);

struct le_llm_http_response {
    int status;
    char body[LE_LLM_BODY_MAX];
};

int le_llm_http_execute(const char *curl_path,
                        const struct le_llm_http_request *request,
                        le_llm_http_event_fn event_fn,
                        void *event_context,
                        struct le_llm_http_response *response);

#endif
