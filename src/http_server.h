#ifndef LE_HTTP_SERVER_H
#define LE_HTTP_SERVER_H
#include "api.h"
struct http_options{char listen_host[64];int port;char web_root[384];int max_clients;};
int http_server_run(const struct http_options*,struct api_context*,volatile int*running);
#endif
