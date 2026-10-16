#ifndef LE_JSON_H
#define LE_JSON_H
#include <stddef.h>
int json_valid_object(const char*,size_t); int json_duplicate_key(const char*,size_t,const char*); int json_get_int(const char*,const char*,int*); int json_get_int64(const char*,const char*,long long*); int json_get_int64_top_level(const char*,const char*,long long*); int json_get_array_top_level(const char*,const char*,const char**,size_t*); int json_get_uint(const char*,const char*,unsigned int*); int json_get_bool(const char*,const char*,int*); int json_get_string(const char*,const char*,char*,size_t); int json_get_string_top_level(const char*,const char*,char*,size_t); void json_escape(char*,size_t,const char*);
int json_get_top_level_bool(const char*,size_t,const char*,int*);
/*
 * Bounded member enumeration for a flat JSON object.
 *
 * find_key()/json_get_string() locate a member by a raw substring search, so
 * they cannot tell one member's value from an identically named member of a
 * nested object, and they cannot tell a known member from an unknown one. Both
 * matter wherever an unknown key must be rejected outright. The spans below
 * point into the caller's own buffer (no copy), and name_len/value_len are
 * exact, so a caller can decode a value without a terminating NUL and can
 * compare a name without re-parsing the document.
 */
struct json_member { const char *name; size_t name_len; const char *value; size_t value_len; };
int json_object_members(const char*,size_t,struct json_member*,size_t,size_t*);
/* Decode exactly the bytes of one value span; -1 for a wrong type or shape. */
int json_string_span(const char*,size_t,char*,size_t);
int json_int_span(const char*,size_t,int*);
int json_bool_span(const char*,size_t,int*);
#endif
