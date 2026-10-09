#ifndef LE_ESPHOME_PROTO_H
#define LE_ESPHOME_PROTO_H
#include <stdint.h>
#include <stddef.h>
/* No allocation, recursion, groups, or unbounded unknown-field scan. */
struct ep_writer { unsigned char *data; size_t capacity,length; int error; };
struct ep_reader { const unsigned char *data; size_t length,position; unsigned fields; int error; };
struct ep_field { unsigned number,wire; uint64_t value; const unsigned char *data; size_t length; };
void ep_reader_init(struct ep_reader *,const void *,size_t);
int ep_next(struct ep_reader *,struct ep_field *);
int ep_text(const struct ep_field *,char *,size_t);
void ep_uint(struct ep_writer *,unsigned,uint64_t);
void ep_fixed32(struct ep_writer *,unsigned,uint32_t);
void ep_bytes(struct ep_writer *,unsigned,const void *,size_t);
void ep_string(struct ep_writer *,unsigned,const char *);
uint64_t ep_get_uint(const void *,size_t,unsigned,uint64_t);
int ep_get_text(const void *,size_t,unsigned,char *,size_t);
#endif
