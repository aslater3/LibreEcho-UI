#include "../src/adapter/esphome_proto.h"
#include "../src/adapter/esphome_frame.h"
#include "test_esphomed_goldens.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 unsigned char b[64], out[128]; struct ep_writer w={b,sizeof b,0,0}; struct ep_reader r; struct ep_field f;
 ep_uint(&w,1,1);ep_string(&w,2,"fixture");ep_fixed32(&w,3,0x12345678);
 assert(!w.error); ep_reader_init(&r,b,w.length);assert(ep_next(&r,&f)==1 && f.number==1 && f.value==1);
 assert(ep_next(&r,&f)==1 && f.number==2 && f.length==7);assert(ep_next(&r,&f)==1 && f.value==0x12345678);assert(ep_next(&r,&f)==0);
 {unsigned char bad[]={8,255,255,255,255,255,255,255,255,255,2};ep_reader_init(&r,bad,sizeof bad);assert(ep_next(&r,&f)==-1);}
 {unsigned char bad[]={0,1};ep_reader_init(&r,bad,sizeof bad);assert(ep_next(&r,&f)==-1);}
 {unsigned char bad[]={18,127};ep_reader_init(&r,bad,sizeof bad);assert(ep_next(&r,&f)==-1);}
 {size_t n=ef_plain_encode(out,sizeof out,7,b,w.length),used=0,len=0;unsigned type=0;assert(n>0);
 for(size_t i=0;i<n;i++)assert(ef_parse(out,i,0,&used,&type,&len)==0);
 assert(ef_parse(out,n,0,&used,&type,&len)==1 && type==7 && len==w.length && used==n);
 }
 {unsigned char bad[]={0,129,128,4,1};size_t u,n;unsigned t;assert(ef_parse(bad,sizeof bad,0,&u,&t,&n)==-1);}
 w.length=0;ep_string(&w,1,"fixture");ep_uint(&w,2,1);ep_uint(&w,3,14);assert(w.length==sizeof golden_hello_request&&!memcmp(b,golden_hello_request,w.length));
 w.length=0;ep_uint(&w,1,1);ep_uint(&w,2,4);assert(w.length==sizeof golden_subscribe_voice&&!memcmp(b,golden_subscribe_voice,w.length));
 w.length=0;ep_fixed32(&w,1,2);ep_uint(&w,2,1);assert(w.length==sizeof golden_switch_mute&&!memcmp(b,golden_switch_mute,w.length));
 w.length=0;ep_bytes(&w,1,"\x34\x12\x78\x56",4);ep_uint(&w,2,1);assert(w.length==sizeof golden_api_audio&&!memcmp(b,golden_api_audio,w.length));
 {unsigned char key[32];for(unsigned i=0;i<32;i++)key[i]=(unsigned char)i;w.length=0;ep_bytes(&w,1,key,32);assert(w.length==sizeof golden_set_key&&!memcmp(b,golden_set_key,w.length));}
 {unsigned char fields[514];for(unsigned i=0;i<257;i++){fields[2*i]=8;fields[2*i+1]=1;}ep_reader_init(&r,fields,sizeof fields);for(unsigned i=0;i<256;i++)assert(ep_next(&r,&f)==1);assert(ep_next(&r,&f)==-1);}
 {unsigned char tiny[1];struct ep_writer x={tiny,sizeof tiny,0,0};ep_string(&x,1,"too long");assert(x.error&&x.length<=sizeof tiny);}
 puts("protobuf/frame bounds and schema-derived vectors: OK");return 0;
}
