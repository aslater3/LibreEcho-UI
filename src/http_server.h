#ifndef LE_HTTP_SERVER_H
#define LE_HTTP_SERVER_H
#include "api.h"
struct http_options{char listen_host[64];int port;char web_root[384],run_user[64];int max_clients;
/* tls_port 0 leaves HTTPS off; HTTP always keeps listening either way. */
int tls_port;char tls_cert[384],tls_key[384];};
int http_server_run(const struct http_options*,struct api_context*,volatile int*running);
#endif
