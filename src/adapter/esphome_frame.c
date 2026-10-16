#include "esphome_frame.h"
#include <string.h>
static int var(const unsigned char *p,size_t n,size_t *u,unsigned *v){*v=0;for(unsigned i=0;i<5;i++){if(i>=n)return 0;if(i==4&&p[i]>15)return -1;*v|=(unsigned)(p[i]&127)<<(7*i);if(!(p[i]&128)){*u=i+1;return 1;}}return -1;}
int ef_parse(const unsigned char *p,size_t n,int noise,size_t *used,unsigned *type,size_t *length){size_t h=1,u;unsigned l,t;int rc;if(!n)return 0;
 if(p[0]!=(noise?1:0))return -1;
 if(noise){if(n<3)return 0;h=3;l=((unsigned)p[1]<<8)|p[2];t=0;}
 else{rc=var(p+h,n-h,&u,&l);if(rc<=0)return rc;h+=u;if(l>EF_MAX_FRAME)return -1;rc=var(p+h,n-h,&u,&t);if(rc<=0)return rc;h+=u;if(t>65535)return -1;}
 if(l>EF_MAX_FRAME)return -1;if(n-h<l)return 0;*used=h+l;*length=l;*type=t;return 1;
}
static size_t put(unsigned char *p,unsigned v){size_t n=0;do{p[n]=(unsigned char)(v&127);v>>=7;if(v)p[n]|=128;n++;}while(v);return n;}
size_t ef_plain_encode(unsigned char *p,size_t cap,unsigned t,const void *data,size_t n){unsigned char h[11];size_t z=1;if(n>EF_MAX_FRAME||t>65535)return 0;h[0]=0;z+=put(h+z,(unsigned)n);z+=put(h+z,t);if(z+n>cap)return 0;memcpy(p,h,z);if(n)memcpy(p+z,data,n);return z+n;}
