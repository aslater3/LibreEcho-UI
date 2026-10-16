#include "../src/adapter/esphome_noise.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){struct en_session a,b;unsigned char key[32]={0},c[80],p[80];size_t n,z;
 en_init(&a);en_init(&b);memcpy(a.tx_key,key,32);memcpy(b.rx_key,key,32);a.ready=b.ready=1;
 assert(en_encrypt(&a,(unsigned char*)"test",4,c,sizeof c,&n)==0 && n==20);
 assert(en_decrypt(&b,c,n,p,sizeof p,&z)==0 && z==4 && !memcmp(p,"test",4));
 assert(en_decrypt(&b,c,n,p,sizeof p,&z)==-1);en_free(&b);en_init(&b);b.ready=1;
 c[0]^=1;assert(en_decrypt(&b,c,n,p,sizeof p,&z)==-1);en_free(&a);en_free(&b);
 puts("Noise AEAD replay/tamper: OK");return 0;}
