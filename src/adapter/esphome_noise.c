/* Noise responder, pinned aioesphomeapi 46.2.0 NNpsk0. No key logging.
 * All primitives are provided by mbedTLS 3.6 LTS; never home-grown crypto. */
#include "esphome_noise.h"
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/platform_util.h>
#include <string.h>
#include <limits.h>
void en_init(struct en_session *s){memset(s,0,sizeof *s);}
void en_free(struct en_session *s){mbedtls_platform_zeroize(s,sizeof *s);}
static int hash(const unsigned char *p,size_t n,unsigned char out[32]){return mbedtls_sha256(p,n,out,0);}
static int mixhash(unsigned char h[32],const unsigned char *p,size_t n){unsigned char b[96];if(n>64)return -1;memcpy(b,h,32);memcpy(b+32,p,n);return hash(b,32+n,h);}
static int hkdf(unsigned char ck[32],const unsigned char *in,size_t n,unsigned char a[32],unsigned char b[32],unsigned char *c){
 unsigned char temp[32],block[33];const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);int rc=-1;
 if(mbedtls_md_hmac(md,ck,32,in,n,temp))goto done;block[0]=1;if(mbedtls_md_hmac(md,temp,32,block,1,a))goto done;
 memcpy(block,a,32);block[32]=2;if(mbedtls_md_hmac(md,temp,32,block,33,b))goto done;
 if(c){memcpy(block,b,32);block[32]=3;if(mbedtls_md_hmac(md,temp,32,block,33,c))goto done;}rc=0;
 done:mbedtls_platform_zeroize(temp,sizeof temp);mbedtls_platform_zeroize(block,sizeof block);return rc;
}
static int crypt(int encrypt,const unsigned char key[32],uint64_t count,const unsigned char *ad,size_t ad_n,const unsigned char *in,size_t n,unsigned char *out){
 unsigned char nonce[12]={0};mbedtls_chachapoly_context c;int rc;for(unsigned i=0;i<8;i++)nonce[4+i]=(unsigned char)(count>>(8*i));mbedtls_chachapoly_init(&c);
 rc=mbedtls_chachapoly_setkey(&c,key);if(!rc){if(encrypt)rc=mbedtls_chachapoly_encrypt_and_tag(&c,n,nonce,ad,ad_n,in,out,out+n);else if(n>=16)rc=mbedtls_chachapoly_auth_decrypt(&c,n-16,nonce,ad,ad_n,in+n-16,in,out);else rc=-1;}mbedtls_chachapoly_free(&c);return rc;
}
int en_encrypt(struct en_session *s,const unsigned char *p,size_t n,unsigned char *out,size_t cap,size_t *len){if(!s->ready||s->tx_nonce==UINT64_MAX||cap<16||n>cap-16)return -1;if(crypt(1,s->tx_key,s->tx_nonce,NULL,0,p,n,out))return -1;s->tx_nonce++;*len=n+16;return 0;}
int en_decrypt(struct en_session *s,const unsigned char *p,size_t n,unsigned char *out,size_t cap,size_t *len){if(!s->ready||s->rx_nonce==UINT64_MAX||n<16||n-16>cap)return -1;if(crypt(0,s->rx_key,s->rx_nonce,NULL,0,p,n,out))return -1;s->rx_nonce++;*len=n-16;return 0;}
int en_handshake(struct en_session *s,const unsigned char psk[32],const unsigned char *msg,size_t n,unsigned char *reply,size_t *len){
 static const unsigned char name[]="Noise_NNpsk0_25519_ChaChaPoly_SHA256",prologue[]="NoiseAPIInit\0\0";
 unsigned char ck[32],h[32],k[32],a[32],b[32],c[32],dh[32],pub[32],empty[1];int rc=-1;
 mbedtls_entropy_context entropy;mbedtls_ctr_drbg_context rng;mbedtls_ecp_keypair e;mbedtls_ecp_point peer;mbedtls_mpi shared;
 mbedtls_entropy_init(&entropy);mbedtls_ctr_drbg_init(&rng);mbedtls_ecp_keypair_init(&e);mbedtls_ecp_point_init(&peer);mbedtls_mpi_init(&shared);
 if(n!=49||msg[0]!=0)goto done;
 if(hash(name,sizeof name-1,h))goto done;memcpy(ck,h,32);if(mixhash(h,prologue,sizeof prologue-1))goto done;
 if(hkdf(ck,psk,32,a,b,c))goto done;memcpy(ck,a,32);if(mixhash(h,b,32))goto done;memcpy(k,c,32);
 if(mixhash(h,msg+1,32)||hkdf(ck,msg+1,32,a,b,NULL))goto done;memcpy(ck,a,32);memcpy(k,b,32);
 if(crypt(0,k,0,h,32,msg+33,16,empty)||mixhash(h,msg+33,16))goto done;
 if(mbedtls_ctr_drbg_seed(&rng,mbedtls_entropy_func,&entropy,(const unsigned char*)"libreecho-noise",14))goto done;
 if(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_CURVE25519,&e,mbedtls_ctr_drbg_random,&rng))goto done;
 {size_t z;if(mbedtls_ecp_point_write_binary(&e.MBEDTLS_PRIVATE(grp),&e.MBEDTLS_PRIVATE(Q),MBEDTLS_ECP_PF_UNCOMPRESSED,&z,pub,32)||z!=32)goto done;}
 if(mixhash(h,pub,32)||hkdf(ck,pub,32,a,b,NULL))goto done;memcpy(ck,a,32);memcpy(k,b,32);
 if(mbedtls_ecp_point_read_binary(&e.MBEDTLS_PRIVATE(grp),&peer,msg+1,32)||mbedtls_ecdh_compute_shared(&e.MBEDTLS_PRIVATE(grp),&shared,&peer,&e.MBEDTLS_PRIVATE(d),mbedtls_ctr_drbg_random,&rng)||mbedtls_mpi_write_binary_le(&shared,dh,32))goto done;
 {unsigned v=0;for(unsigned i=0;i<32;i++)v|=dh[i];if(!v)goto done;}
 if(hkdf(ck,dh,32,a,b,NULL))goto done;memcpy(ck,a,32);memcpy(k,b,32);reply[0]=0;memcpy(reply+1,pub,32);
 if(crypt(1,k,0,h,32,empty,0,reply+33)||mixhash(h,reply+33,16)||hkdf(ck,NULL,0,a,b,NULL))goto done;
 memcpy(s->rx_key,a,32);memcpy(s->tx_key,b,32);s->rx_nonce=s->tx_nonce=0;s->ready=1;*len=49;rc=0;
 done:mbedtls_ecp_keypair_free(&e);mbedtls_ecp_point_free(&peer);mbedtls_mpi_free(&shared);mbedtls_ctr_drbg_free(&rng);mbedtls_entropy_free(&entropy);
 mbedtls_platform_zeroize(ck,32);mbedtls_platform_zeroize(k,32);mbedtls_platform_zeroize(a,32);mbedtls_platform_zeroize(b,32);mbedtls_platform_zeroize(c,32);mbedtls_platform_zeroize(dh,32);return rc;
}
