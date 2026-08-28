#ifndef LE_BACKEND_H
#define LE_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define LE_TEXT 64
#define LE_MAX_WIFI 12
#define LE_MAX_LOGS 128
#define LE_MAX_CPUS 8
#define LE_MAX_BLUETOOTH_DEVICES 24
#define LE_LED_PIXELS 12
#define LE_MEDIA_TEXT 192

enum le_result { LE_OK=0, LE_INVALID=-1, LE_NOT_SUPPORTED=-2, LE_IO=-3, LE_BUSY=-4, LE_AUTH=-5 };

struct le_cpu_state { int online, utilization, frequency_khz; };
struct le_system_status { double uptime; int cpu, memory, storage, temperature; /* Ambient light in lux, or -1 when the board has no usable sensor. */ int light_lux; int memory_used_mb, memory_total_mb; int storage_used_mb, storage_total_mb, storage_available; char storage_state[32], device_state[24]; size_t cpu_count; struct le_cpu_state cpus[LE_MAX_CPUS]; };
/* factory_mac is the MAC recorded in idme, empty when it could not be read.
   mac_source names where it came from, so an image whose idme spells the
   field differently can be identified from the API rather than guessed at. */
struct le_device_info { char name[LE_TEXT], hostname[LE_TEXT], model[LE_TEXT], serial[LE_TEXT], os_version[32], kernel[64], hardware_revision[32], backend[16], factory_mac[24], mac_source[32]; };
struct le_audio_state { int volume, microphone_gain, notification_volume, muted, startup_sound, amplifier_on, output_available; char tts_voice[32];
 /* Sleep-noise generator. noise_remaining_seconds is -1 when it is
    running untimed, otherwise the seconds left on the sleep timer. */
 int noise_active, noise_level; long noise_remaining_seconds; char noise_colour[8]; };
/*
 * Spotify Connect. installed is 0 when the librespot-based daemon is not in
 * the image, which is different from being switched off: the UI must be able
 * to say "not installed" rather than offering a toggle that silently does
 * nothing. device_name is what appears in the Spotify app's device list.
 */
struct le_spotify_state { int installed, enabled, playing; char device_name[64], status[32]; };
struct le_light_state { int available, lux, calibrated_lux, ch0, ch1, gain, integration_us, auto_gain, powered; char bus[32]; };
struct le_led_profile { uint8_t r,g,b; int brightness, animation_speed; };
struct le_led_pixel { uint8_t r,g,b; };
struct le_led_state {
    struct le_led_profile current, boot, listening, thinking, error, dnd;
    /* Night is a cap applied on a local-time schedule, not an animation the
       daemon switches to, so it carries its window alongside the colour. */
    struct le_led_profile night;
    int night_enabled, night_active, night_start_minute, night_end_minute;
    int animation_active, animation_profile, pattern_active;
    int visualizer_enabled, visualizer_active;
    char pattern[16], visualizer_owner[32], visualizer_mood[16];
    uint8_t visualizer_levels[LE_LED_PIXELS];
    struct le_led_pixel pixels[LE_LED_PIXELS];
};
struct le_wifi_network { char ssid[LE_TEXT], security[16]; int signal; };
struct le_wifi_scan { struct le_wifi_network networks[LE_MAX_WIFI]; size_t count; };
struct le_wifi_credentials { char ssid[LE_TEXT], password[128], security[16]; };
struct le_network_state {
    char state[24], connectivity[24], recovery_stage[24];
    /* factory_* is what idme records for this board; the plain field is what
       the interface is using now. They differ whenever the driver invents an
       address instead of taking the board's, which is what makes the device
       land on a new DHCP lease after most reboots. */
    char wifi_mac[24], wifi_mac_factory[24], bt_mac[24], bt_mac_factory[24];
    char ssid[LE_TEXT], ip[48], gateway[48], dns[96], hostname[LE_TEXT];
    int signal, rssi_dbm, internet, dhcp, ssh, api_lan;
    int gateway_reachable, liveness_failures;
};
struct le_wake_word_state { char wake_word[LE_TEXT], model_status[24]; int enabled, sensitivity, cooldown_ms, detected_count, cpu_cost, memory_cost_mb; };
struct le_bluetooth_device { char address[18], name[LE_TEXT]; int type, rssi, rssi_valid, paired, connected; };
struct le_bluetooth_pairing { char address[18], method[24]; int type; unsigned int value; };
struct le_bluetooth_state {
    /* address is the adapter's current one, factory what idme records for the
       board; they differ when the controller was never given the board's. */
    char address[24], address_factory[24];
    char state[32], transport[32], hci[32], local_name[LE_TEXT], last_error[96];
    char last_disconnect_reason[48], last_connect_failed_status[48];
    char profile_state[24], profile_error[96];
    int available, enabled, activation_attempted, scanning, pairing, pairing_mode;
    int classic, le, ssp, secure_connection, connectable, discoverable, bondable;
    int profile_sdp, profile_a2dp_sink, profile_avrcp, profile_rfcomm;
    int profile_bnep, profile_hidp;
    size_t discovered_count, known_count;
    struct le_bluetooth_device discovered[LE_MAX_BLUETOOTH_DEVICES];
    struct le_bluetooth_device known[LE_MAX_BLUETOOTH_DEVICES];
    struct le_bluetooth_pairing pending_pairing;
};
struct le_airplay_state {
    int available, enabled, nqptp_running, shairport_running, playing;
    char playback_state[24], source[32];
    char title[LE_MEDIA_TEXT+1], artist[LE_MEDIA_TEXT+1], album[LE_MEDIA_TEXT+1];
};
/* What radiod is doing. title and station are whatever the stream said about
   itself over ICY; both are empty when it said nothing, and nothing here
   invents a substitute. */
struct le_radio_status {
    int playing;
    char url[512];
    char title[LE_MEDIA_TEXT+1], station[LE_MEDIA_TEXT+1];
};
struct le_playback_state {
    char state[24], source[32];
    int media_active, system_active, announcement_active, alarm_active;
    int metadata_available;
    char title[LE_MEDIA_TEXT+1], artist[LE_MEDIA_TEXT+1], album[LE_MEDIA_TEXT+1];
};
struct le_backend;

int le_backend_init(struct le_backend **out, const char *mode, const char *mock_path, const char *config_path, unsigned seed);
void le_backend_destroy(struct le_backend *b);
const char *le_backend_mode(struct le_backend *b);
const char *le_result_code(int rc);
int le_get_system_status(struct le_backend*,struct le_system_status*); int le_get_device_info(struct le_backend*,struct le_device_info*);
int le_get_audio_state(struct le_backend*,struct le_audio_state*); int le_set_volume(struct le_backend*,int); int le_set_microphone_gain(struct le_backend*,int); int le_set_microphone_muted(struct le_backend*,int); int le_play_test_tone(struct le_backend*); int le_set_tts_voice(struct le_backend*,const char*); int le_announce(struct le_backend*,const char*); int le_stop_speech(struct le_backend*); int le_start_noise(struct le_backend*,const char*,int,int); int le_stop_noise(struct le_backend*); int le_simulate_audio(struct le_backend*,const char*); int le_radio_play(struct le_backend*,const char*); int le_radio_stop(struct le_backend*); int le_radio_playing(struct le_backend*,struct le_radio_status*);
int le_get_led_state(struct le_backend*,struct le_led_state*); int le_set_led_colour(struct le_backend*,uint8_t,uint8_t,uint8_t); int le_set_led_brightness(struct le_backend*,int); int le_set_led_visualizer_enabled(struct le_backend*,int); int le_set_boot_led(struct le_backend*,const struct le_led_profile*); int le_set_led_profile(struct le_backend*,const char*,const struct le_led_profile*); int le_set_led_night(struct le_backend*,int,int,int); int le_run_led_test(struct le_backend*); int le_get_spotify_state(struct le_backend*,struct le_spotify_state*); int le_set_spotify_enabled(struct le_backend*,int); int le_get_light_state(struct le_backend*,struct le_light_state*);
 int le_play_sound_sample(struct le_backend*,const char*);int le_get_network_state(struct le_backend*,struct le_network_state*); int le_scan_wifi(struct le_backend*,struct le_wifi_scan*); int le_connect_wifi(struct le_backend*,const struct le_wifi_credentials*); int le_disconnect_wifi(struct le_backend*); int le_set_hostname(struct le_backend*,const char*);
int le_get_wake_word_state(struct le_backend*,struct le_wake_word_state*); int le_set_wake_word(struct le_backend*,const char*); int le_set_wake_word_sensitivity(struct le_backend*,int); int le_test_wake_word(struct le_backend*);
int le_get_bluetooth_state(struct le_backend*,struct le_bluetooth_state*); int le_set_bluetooth_enabled(struct le_backend*,int);
int le_bluetooth_scan(struct le_backend*,int); int le_bluetooth_pair(struct le_backend*,const char*,int,int); int le_bluetooth_unpair(struct le_backend*,const char*,int); int le_bluetooth_disconnect(struct le_backend*,const char*,int); int le_bluetooth_pairing_response(struct le_backend*,const char*,int,const char*,unsigned int,const char*); int le_bluetooth_set_discoverable(struct le_backend*,int); int le_bluetooth_set_connectable(struct le_backend*,int); int le_bluetooth_set_pairing_mode(struct le_backend*,int);
int le_get_airplay_state(struct le_backend*,struct le_airplay_state*); int le_set_airplay_enabled(struct le_backend*,int);
int le_get_playback_state(struct le_backend*,struct le_playback_state*);
int le_reboot(struct le_backend*); int le_shutdown(struct le_backend*); int le_factory_reset(struct le_backend*);
int le_backend_tick(struct le_backend*); int le_backend_mock_control(struct le_backend*,const char*,const char*);

#endif
