#ifndef LE_BACKEND_H
#define LE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define LE_TEXT 64
#define LE_MAX_WIFI 12
#define LE_MAX_LOGS 128
#define LE_MAX_CPUS 8

enum le_result { LE_OK=0, LE_INVALID=-1, LE_NOT_SUPPORTED=-2, LE_IO=-3, LE_BUSY=-4, LE_AUTH=-5 };

struct le_cpu_state { int online, utilization, frequency_khz; };
struct le_system_status { double uptime; int cpu, memory, storage, temperature; int memory_used_mb, memory_total_mb; int storage_used_mb, storage_total_mb; char device_state[24]; size_t cpu_count; struct le_cpu_state cpus[LE_MAX_CPUS]; };
struct le_device_info { char name[LE_TEXT], hostname[LE_TEXT], model[LE_TEXT], serial[LE_TEXT], os_version[32], kernel[64], hardware_revision[32], backend[16]; };
struct le_audio_state { int volume, microphone_gain, notification_volume, muted, startup_sound, amplifier_on, output_available; };
struct le_led_profile { uint8_t r,g,b; int brightness, animation_speed; };
struct le_led_state { struct le_led_profile current, boot, listening, thinking, error, dnd; };
struct le_wifi_network { char ssid[LE_TEXT], security[16]; int signal; };
struct le_wifi_scan { struct le_wifi_network networks[LE_MAX_WIFI]; size_t count; };
struct le_wifi_credentials { char ssid[LE_TEXT], password[128], security[16]; };
struct le_network_state { char state[24], ssid[LE_TEXT], ip[48], gateway[48], dns[96], hostname[LE_TEXT]; int signal, rssi_dbm, internet, dhcp, ssh, api_lan; };
struct le_wake_word_state { char wake_word[LE_TEXT], model_status[24]; int enabled, sensitivity, cooldown_ms, detected_count, cpu_cost, memory_cost_mb; };
struct le_backend;

int le_backend_init(struct le_backend **out, const char *mode, const char *mock_path, const char *config_path, unsigned seed);
void le_backend_destroy(struct le_backend *b);
const char *le_backend_mode(struct le_backend *b);
const char *le_result_code(int rc);
int le_get_system_status(struct le_backend*,struct le_system_status*); int le_get_device_info(struct le_backend*,struct le_device_info*);
int le_get_audio_state(struct le_backend*,struct le_audio_state*); int le_set_volume(struct le_backend*,int); int le_set_microphone_gain(struct le_backend*,int); int le_set_microphone_muted(struct le_backend*,int); int le_play_test_tone(struct le_backend*);
int le_get_led_state(struct le_backend*,struct le_led_state*); int le_set_led_colour(struct le_backend*,uint8_t,uint8_t,uint8_t); int le_set_led_brightness(struct le_backend*,int); int le_set_boot_led(struct le_backend*,const struct le_led_profile*); int le_run_led_test(struct le_backend*);
int le_get_network_state(struct le_backend*,struct le_network_state*); int le_scan_wifi(struct le_backend*,struct le_wifi_scan*); int le_connect_wifi(struct le_backend*,const struct le_wifi_credentials*); int le_disconnect_wifi(struct le_backend*); int le_set_hostname(struct le_backend*,const char*);
int le_get_wake_word_state(struct le_backend*,struct le_wake_word_state*); int le_set_wake_word(struct le_backend*,const char*); int le_set_wake_word_sensitivity(struct le_backend*,int); int le_test_wake_word(struct le_backend*);
int le_reboot(struct le_backend*); int le_shutdown(struct le_backend*); int le_factory_reset(struct le_backend*);
int le_backend_tick(struct le_backend*); int le_backend_mock_control(struct le_backend*,const char*,const char*);

#endif
