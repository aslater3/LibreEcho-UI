#ifndef LE_ESPHOME_PLAYBACK_H
#define LE_ESPHOME_PLAYBACK_H
#include <stdint.h>
#include <stddef.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include "radio_resample.h"
#include "../../third-party/minimp3/minimp3.h"
/* Response body ceiling. The body streams through fixed windows, so this bounds
 * work, not memory: about 170 s of the 48 kHz mono WAV Home Assistant sends. */
#define ESP_PLAY_BODY_MAX (32U*1024U*1024U)
#define ESP_PLAY_WIRE (16U*1024U)
#define ESP_PLAY_PAYLOAD (64U*1024U)
/* Single streaming playback slot. tick does bounded nonblocking work. */
struct esp_playback {
 int state,fd,bus_fd,fifo,tls,header_done,result,dns_fd,chunked,net_eof,net_done,format,clock,buffering;
 unsigned port;char host[254],path[1024],bus[256],request[1536];size_t request_len,request_sent;
 /* Raw response bytes (headers, then the framed body) and the unframed body the decoder reads. */
 unsigned char wire[ESP_PLAY_WIRE];size_t wire_n,wire_pos,expected,body_total;
 unsigned char payload[ESP_PLAY_PAYLOAD];size_t pay_n,pay_pos;
 /* Chunked transfer framing, decoded a byte at a time. */
 int chunk_stage,trailer_cr,trailer_colon;unsigned chunk_digits;uint64_t chunk_left;size_t trailer_bytes,trailer_line;
 /* WAV chunk walk and MP3 resynchronisation. */
 int wav_stage,data_unbounded;uint32_t riff;uint64_t wav_bytes,skip,data_left,mp3_skipped;
 unsigned char dns_query[300];size_t dns_query_len;uint16_t dns_id;
 int rate,channels,source_rate;uint64_t input_frames,audio_frames,frames,origin,deadline,idle_deadline,drain_until;unsigned underruns;
 int16_t output[8192*2];size_t output_len,output_sent;
 mp3dec_t decoder;struct le_radio_resampler resampler;
 mbedtls_ssl_context ssl;mbedtls_ssl_config config;mbedtls_x509_crt ca;mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;
};
void esp_playback_init(struct esp_playback *);
void esp_playback_close(struct esp_playback *);
int esp_playback_start(struct esp_playback *,const char *,const char *,const char *,uint64_t);
/* 0 active, 1 drained successfully, -1 failed. */
int esp_playback_tick(struct esp_playback *,uint64_t);
#endif
