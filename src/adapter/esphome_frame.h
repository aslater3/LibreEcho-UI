#ifndef LE_ESPHOME_FRAME_H
#define LE_ESPHOME_FRAME_H
#include <stddef.h>
#define EF_MAX_FRAME 65536U
/* Incremental parser: 0 incomplete, -1 malformed, 1 complete. body length
 * and total consumed are returned; body starts at consumed-length. */
int ef_parse(const unsigned char *,size_t,int,size_t *,unsigned *,size_t *);
size_t ef_plain_encode(unsigned char *,size_t,unsigned,const void *,size_t);
#endif
