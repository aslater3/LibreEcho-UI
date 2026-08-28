#ifndef LE_JSON_H
#define LE_JSON_H
#include <stddef.h>
int json_valid_object(const char *s,size_t n); int json_get_top_level_bool(const char*,size_t,const char*,int*); int json_duplicate_key(const char*,size_t,const char*); int json_get_int(const char*,const char*,int*); int json_get_bool(const char*,const char*,int*); int json_get_string(const char*,const char*,char*,size_t); void json_escape(char*,size_t,const char*);
#endif
