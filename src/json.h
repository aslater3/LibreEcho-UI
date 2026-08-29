#ifndef LE_JSON_H
#define LE_JSON_H
#include <stddef.h>
int json_valid_object(const char*,size_t); int json_duplicate_key(const char*,size_t,const char*); int json_get_int(const char*,const char*,int*); int json_get_int64(const char*,const char*,long long*); int json_get_int64_top_level(const char*,const char*,long long*); int json_get_uint(const char*,const char*,unsigned int*); int json_get_bool(const char*,const char*,int*); int json_get_string(const char*,const char*,char*,size_t); int json_get_string_top_level(const char*,const char*,char*,size_t); void json_escape(char*,size_t,const char*);
#endif
