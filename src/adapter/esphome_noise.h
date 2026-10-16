#ifndef LE_ESPHOME_NOISE_H
#define LE_ESPHOME_NOISE_H
#include <stdint.h>
#include <stddef.h>
#define ESPHOMED_NOISE 1
struct en_session { unsigned char rx_key[32],tx_key[32]; uint64_t rx_nonce,tx_nonce; int ready; };
void en_init(struct en_session *);
void en_free(struct en_session *);
int en_handshake(struct en_session *,const unsigned char psk[32],const unsigned char *,size_t,unsigned char *,size_t *);
int en_encrypt(struct en_session *,const unsigned char *,size_t,unsigned char *,size_t,size_t *);
int en_decrypt(struct en_session *,const unsigned char *,size_t,unsigned char *,size_t,size_t *);
#endif
