#ifndef LE_CONFIG_STORE_H
#define LE_CONFIG_STORE_H
#include <stddef.h>
struct le_esphome_config { char ha_protocol[8]; char esphome_noise_key[45]; char esphome_active_wake_word[16]; int esphome_active_wake_word_present; };
int config_esphome_read(const char *, struct le_esphome_config *);
int config_esphome_key_valid(const char *);
int config_read(const char*,char*,size_t); int config_write_atomic(const char*,const char*,size_t); int config_copy_defaults(const char*,const char*);
#endif
