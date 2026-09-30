#define main esphomed_main
#include "../src/adapter/esphomed.c"
#undef main
#include <assert.h>
int main(void){static struct client c;c.fd=1;c.mode=1;c.noise.ready=1;c.tx_n=TX_CAP-1;
 assert(send_msg(&c,8,NULL,0)==-1);assert(c.closing==1);en_free(&c.noise);puts("Noise TX backpressure fails closed: OK");return 0;}
