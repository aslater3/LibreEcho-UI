#ifndef LE_BACKEND_INTERNAL_H
#define LE_BACKEND_INTERNAL_H
#include "backend.h"
struct le_backend_ops {
 void (*destroy)(struct le_backend*); int (*status)(struct le_backend*,struct le_system_status*); int (*device)(struct le_backend*,struct le_device_info*);
 int (*audio)(struct le_backend*,struct le_audio_state*); int (*volume)(struct le_backend*,int); int (*gain)(struct le_backend*,int); int (*mute)(struct le_backend*,int); int (*tone)(struct le_backend*);
 int (*led)(struct le_backend*,struct le_led_state*); int (*colour)(struct le_backend*,uint8_t,uint8_t,uint8_t); int (*brightness)(struct le_backend*,int); int (*boot_led)(struct le_backend*,const struct le_led_profile*); int (*led_test)(struct le_backend*);
 int (*network)(struct le_backend*,struct le_network_state*); int (*scan)(struct le_backend*,struct le_wifi_scan*); int (*connect)(struct le_backend*,const struct le_wifi_credentials*); int (*disconnect)(struct le_backend*); int (*hostname)(struct le_backend*,const char*);
 int (*wake)(struct le_backend*,struct le_wake_word_state*); int (*wake_set)(struct le_backend*,const char*); int (*sensitivity)(struct le_backend*,int); int (*wake_test)(struct le_backend*);
 int (*reboot)(struct le_backend*); int (*shutdown)(struct le_backend*); int (*reset)(struct le_backend*); int (*tick)(struct le_backend*); int (*control)(struct le_backend*,const char*,const char*);
};
struct le_backend { const struct le_backend_ops *ops; void *data; char mode[16]; };
int le_mock_create(struct le_backend*,const char*,const char*,unsigned); int le_linux_create(struct le_backend*,const char*);
#endif
