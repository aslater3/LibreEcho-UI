#ifndef LE_JSON_H
#define LE_JSON_H
#include <stddef.h>
int json_valid_object(const char *s,size_t n);
/* Return 1 for one top-level boolean, 0 when absent, and -1 for malformed or
   duplicate decoded member names. */
int json_get_top_level_bool(const char*,size_t,const char*,int*);
/* Return non-zero for a duplicate decoded top-level member name. */
int json_duplicate_key(const char*,size_t,const char*);
int json_get_int(const char*,const char*,int*); int json_get_uint(const char*,const char*,unsigned int*); int json_get_bool(const char*,const char*,int*); int json_get_string(const char*,const char*,char*,size_t); void json_escape(char*,size_t,const char*);
#endif
