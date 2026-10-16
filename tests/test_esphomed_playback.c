#define _POSIX_C_SOURCE 200809L
#include "../src/adapter/esphome_playback.h"
#include <assert.h>
#include <stdio.h>
int main(void){static struct esp_playback p;esp_playback_init(&p);assert(esp_playback_start(&p,"file:///etc/passwd","/dev/null",NULL,0)==-1);assert(esp_playback_start(&p,"http://user:pass@localhost/a","/dev/null",NULL,0)==-1);assert(esp_playback_start(&p,"http://localhost/a\r\nX: y","/dev/null",NULL,0)==-1);esp_playback_close(&p);puts("playback URL boundary: OK");return 0;}
