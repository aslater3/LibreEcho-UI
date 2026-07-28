#ifndef LIBREECHO_WYOMING_CLIENT_H
#define LIBREECHO_WYOMING_CLIENT_H

#include <stddef.h>

#define LE_WYOMING_URI_MAX 320

int le_wyoming_uri_valid(const char *uri);
int le_wyoming_connect(const char *uri, int timeout_ms);

#endif
