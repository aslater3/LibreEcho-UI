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
#define ESP_PLAY_BODY_MAX (2U*1024U*1024U)
/* Single finite playback slot. tick does bounded nonblocking work. */
struct esp_playback {
 int state,fd,bus_fd,tls,wav,header_done,result,dns_fd,chunked;
 unsigned port;char host[254],path[1024],bus[256],request[1536];size_t request_len,request_sent;
 unsigned char buffer[ESP_PLAY_BODY_MAX+8192];size_t received,body_start,body_len,expected,position;
 unsigned char dns_query[300];size_t dns_query_len;uint16_t dns_id;
 int rate,channels,source_rate;uint64_t input_frames;size_t pcm_offset,pcm_bytes;uint64_t deadline,origin,drain_until,frames;
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
