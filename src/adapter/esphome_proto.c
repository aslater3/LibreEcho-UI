#include "esphome_proto.h"
#include <string.h>
static int var(const unsigned char *p,size_t n,size_t *used,uint64_t *v) {
 unsigned i; *v=0;for(i=0;i<10;i++){if(i>=n)return 0;if(i==9 && p[i]>1)return -1;*v|=(uint64_t)(p[i]&127)<<(7*i);if(!(p[i]&128)){*used=i+1;return 1;}}return -1;
}
void ep_reader_init(struct ep_reader *r,const void *p,size_t n){r->data=p;r->length=n;r->position=0;r->fields=0;r->error=0;}
int ep_next(struct ep_reader *r,struct ep_field *f){
 uint64_t tag,v;size_t u;const unsigned char *p;size_t n;
 if(r->error)return -1;if(r->position==r->length)return 0;
 if(++r->fields>256)goto bad;p=r->data+r->position;n=r->length-r->position;
 if(var(p,n,&u,&tag)!=1||tag<8||tag>>3>0x1fffffff)goto bad;
 r->position+=u;f->number=(unsigned)(tag>>3);f->wire=(unsigned)(tag&7);f->data=NULL;f->length=0;f->value=0;
 p=r->data+r->position;n=r->length-r->position;
 switch(f->wire){case 0:if(var(p,n,&u,&v)!=1)goto bad;f->value=v;r->position+=u;break;
 case 1:case 5:u=f->wire==1?8:4;if(n<u)goto bad;for(size_t i=0;i<u;i++)f->value|=(uint64_t)p[i]<<(8*i);r->position+=u;break;
 case 2:if(var(p,n,&u,&v)!=1||v>n-u)goto bad;r->position+=u;f->data=r->data+r->position;f->length=(size_t)v;r->position+=(size_t)v;break;
 default:goto bad;}return 1;
 bad:r->error=1;return -1;
}
int ep_text(const struct ep_field *f,char *out,size_t cap){if(f->wire!=2||f->length>=cap||memchr(f->data,0,f->length))return -1;memcpy(out,f->data,f->length);out[f->length]=0;return 0;}
static void raw(struct ep_writer *w,const void *p,size_t n){if(w->error)return;if(n>w->capacity-w->length){w->error=1;return;}memcpy(w->data+w->length,p,n);w->length+=n;}
static void putvar(struct ep_writer *w,uint64_t v){unsigned char p[10];size_t n=0;do{p[n]=(unsigned char)(v&127);v>>=7;if(v)p[n]|=128;n++;}while(v);raw(w,p,n);}
void ep_uint(struct ep_writer *w,unsigned f,uint64_t v){putvar(w,(uint64_t)f<<3);putvar(w,v);}
void ep_fixed32(struct ep_writer *w,unsigned f,uint32_t v){unsigned char p[4];for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(i*8));putvar(w,((uint64_t)f<<3)|5);raw(w,p,4);}
void ep_bytes(struct ep_writer *w,unsigned f,const void *p,size_t n){putvar(w,((uint64_t)f<<3)|2);putvar(w,n);raw(w,p,n);}
void ep_string(struct ep_writer *w,unsigned f,const char *s){ep_bytes(w,f,s,strlen(s));}
uint64_t ep_get_uint(const void *p,size_t n,unsigned number,uint64_t fallback){struct ep_reader r;struct ep_field f;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0)if(f.number==number)return f.value;return fallback;}
int ep_get_text(const void *p,size_t n,unsigned number,char *out,size_t cap){struct ep_reader r;struct ep_field f;out[0]=0;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0)if(f.number==number)return ep_text(&f,out,cap);return r.error?-1:0;}
