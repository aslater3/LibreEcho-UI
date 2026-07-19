#ifndef LE_CONFIG_STORE_H
#define LE_CONFIG_STORE_H
#include <stddef.h>
int config_read(const char*,char*,size_t); int config_write_atomic(const char*,const char*,size_t); int config_copy_defaults(const char*,const char*);
#endif
