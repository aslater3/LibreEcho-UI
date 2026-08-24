#ifndef LE_API_H
#define LE_API_H
#include "backend.h"
#include "auth.h"
#include "event_bus.h"
#include <stddef.h>
struct api_request{char method[8],path[256],host[256],origin[256],authorization[256],csrf[96],confirm[96];const char*body;size_t body_len;};
struct api_response{int status;char type[64];char body[32768];size_t length;};
#define LE_MAX_RADIO_STATIONS 32
/* The OTA upload ceiling, and where an uploaded package is staged on its way
   to the installer. Shared so the size the API advertises and the size the
   HTTP layer enforces cannot drift apart. */
#define LE_UPDATE_MAX_BYTES 33554432u
#define LE_UPDATE_STAGE_DIR "/data/libreecho/update/incoming"
#define LE_UPDATE_STAGE_PARENT "/data"
/* Room the installer needs beside the package it is unpacking. */
#define LE_UPDATE_STAGE_MARGIN_BYTES (4u * 1024u * 1024u)
struct api_context{struct le_backend*backend;struct le_event_bus events;struct le_auth_db auth;int dev_controls,allow_insecure_lan,setup_completed,privacy_local_only,privacy_audio_retention,privacy_telemetry,privacy_crash_reports,privacy_log_hours,privacy_audio_retention_hours,privacy_audio_max_mb,net_ssh,net_api_lan;unsigned integrations,auth_failures;time_t auth_window_started,auth_blocked_until;char privacy_audio_mode[8],privacy_audio_remote_url[256];char button_short[32],button_long[32],auth_token[192],allowed_origin[256],csrf_token[65],config_path[384],users_path[384];char voice_pipeline_mode[24],stt_wyoming_uri[320],stt_wyoming_model[128],tts_wyoming_uri[320],tts_wyoming_voice[128];int stt_max_utterance_ms,stt_end_silence_ms,stt_vad_floor_rms;char timezone[64];int boot_estimate_seconds;int feature_simulation;int feature_usb_host,usb_host_applied;int feature_https,https_port;char https_cert[384];
/* Empty means "use the board's address". Applied at boot, not live: changing a
   MAC on a running interface drops the link and the connection carrying the
   request that changed it. */
char mac_wifi[24],mac_bt[24];char sessions_path[384];int https_active;struct le_radio_station{char word[32],name[64],url[512];int enabled;} radio[LE_MAX_RADIO_STATIONS];size_t radio_count;
/* The station the transport control would start again. It is deliberately not
   persisted: after a reboot nothing is playing and nothing was interrupted, so
   offering to resume something would be a guess. */
char radio_last_word[32];char logs[LE_MAX_LOGS][256];size_t log_count,log_next;};
int api_bootstrap_required(const struct api_context*);
void api_set_https_active(struct api_context*,int);
int api_init(struct api_context*,struct le_backend*,int,int,const char*,const char*,const char*,const char*,const char*);int api_apply_persisted_configuration(struct api_context*,char*,size_t);int api_persist_configuration(struct api_context*);void api_log(struct api_context*,const char*,const char*);void api_handle(struct api_context*,const struct api_request*,struct api_response*);
int api_baby_monitor_stream_authorize(struct api_context*,const struct api_request*,struct api_response*,int*,int*,int*,int*,int*);
size_t le_update_max_upload_bytes(void);
int api_update_upload_authorize(struct api_context*,const struct api_request*,struct api_response*);
int api_update_fetch_authorize(struct api_context*,const struct api_request*,struct api_response*);
int api_update_channel_authorize(struct api_context*,const struct api_request*,struct api_response*,char*,size_t);
void diagnostics_export_json(struct api_context*,struct api_response*);
#endif
