#define _POSIX_C_SOURCE 200809L
#include "../src/diagnostic_export.c"
#include <assert.h>
int main(void){char out[256];assert(redact_text(out,sizeof(out),"esphome_noise_key=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=")==1);assert(!strstr(out,"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));puts("ESPHome Noise key diagnostic redaction: PASS");return 0;}
