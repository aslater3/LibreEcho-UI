#ifndef LE_PEER_AUTH_H
#define LE_PEER_AUTH_H
#include <stddef.h>

/* Signed device-to-device messages (#450).
 *
 * Devices in one home share a 32-byte house key. The owner creates it on one
 * device and types or pastes its pairing code into the others. Nothing here
 * sends anything: this is the key file, the HMAC-SHA256 message
 * authentication and a bounded replay cache that every device-to-device
 * channel uses (the peer check in the device list today; arbitration,
 * intercom and groups later). A device without the key does not sign and
 * accepts nothing.
 *
 * MAC = HMAC-SHA256(mac_key, "LESIG1\n" type "\n" ts "\n" nonce "\n" body)
 * where mac_key = HMAC-SHA256(house_key, "LibreEcho peer mac v1"). type is
 * [a-z0-9.-]{1,32}, ts is decimal Unix seconds, nonce is 32 lowercase hex
 * digits (16 random bytes). The type, ts and nonce cannot hold a newline, so
 * the input is unambiguous; the body is the rest of the bytes.
 *
 * The sealed wire form, for channels that carry raw packets, is one header
 * line then the body:
 *   "LESIG1 <key_id> <type> <ts> <nonce> <mac>\n<body>"
 * key_id is the first 4 bytes, in hex, of HMAC-SHA256(house_key,
 * "LibreEcho key id v1"); owners compare it to check two devices hold the
 * same key, and it never reveals the key. */

#define LE_PEER_KEY_BYTES 32
#define LE_PEER_KEY_ID_LEN 8
#define LE_PEER_NONCE_HEX 32
#define LE_PEER_MAC_HEX 64
#define LE_PEER_TYPE_MAX 32
/* 16 groups of 4 hex digits joined by '-'. */
#define LE_PEER_CODE_LEN 79
#define LE_PEER_REPLAY_SLOTS 64
#define LE_PEER_HEADER_MAX 160

struct le_peer_key {
    int set;
    unsigned char secret[LE_PEER_KEY_BYTES];
    unsigned char mac_key[LE_PEER_KEY_BYTES];
    char id[LE_PEER_KEY_ID_LEN + 1];
};

enum {
    LE_PEER_OK = 0,
    LE_PEER_EFORMAT = -1, /* not a well-formed sealed message */
    LE_PEER_EKEY = -2,    /* no key here, or sealed with another key */
    LE_PEER_EMAC = -3,    /* signature does not match: forged or altered */
    LE_PEER_ETYPE = -4,   /* valid, but not the message type expected */
    LE_PEER_ESTALE = -5,  /* outside the time window */
    LE_PEER_EREPLAY = -6  /* already accepted once */
};

/* Messages accepted inside the time window, so a captured packet cannot be
 * played again. When a slot still inside the window must be reused, floor is
 * raised to its time and anything at or below floor is refused: under a
 * flood the cache fails closed, never open. */
struct le_peer_replay {
    struct { char nonce[LE_PEER_NONCE_HEX + 1]; long long ts; } seen[LE_PEER_REPLAY_SLOTS];
    size_t next;
    long long floor;
};

void le_peer_hmac_sha256(const unsigned char *key, size_t key_len, const unsigned char *data,
                         size_t data_len, unsigned char out[32]);
int le_peer_key_from_secret(struct le_peer_key *key, const unsigned char secret[LE_PEER_KEY_BYTES]);
/* 32 random bytes from /dev/urandom. */
int le_peer_key_generate(struct le_peer_key *key);
/* Accepts 64 hex digits in either case, with any '-' or ' ' between them. */
int le_peer_key_from_code(struct le_peer_key *key, const char *code);
/* out needs LE_PEER_CODE_LEN + 1 bytes. */
int le_peer_key_code(const struct le_peer_key *key, char *out, size_t capacity);
void le_peer_key_clear(struct le_peer_key *key);
/* The key file holds the pairing code and a newline, mode 0600, written
 * atomically. load returns 1 when a key was read, 0 when there is no file
 * and -1 when the file is unreadable or invalid (key left unset). */
int le_peer_key_load(struct le_peer_key *key, const char *path);
int le_peer_key_save(const struct le_peer_key *key, const char *path);
int le_peer_key_remove(const char *path);

int le_peer_type_valid(const char *type);
int le_peer_nonce_valid(const char *nonce);
/* 16 random bytes as 32 lowercase hex digits. */
int le_peer_nonce(char out[LE_PEER_NONCE_HEX + 1]);
int le_peer_mac(const struct le_peer_key *key, const char *type, long long ts, const char *nonce,
                const void *body, size_t body_len, char out[LE_PEER_MAC_HEX + 1]);
/* Constant-time check of a hex MAC. Returns LE_PEER_OK or LE_PEER_EMAC. */
int le_peer_check(const struct le_peer_key *key, const char *type, long long ts, const char *nonce,
                  const void *body, size_t body_len, const char *mac);

/* Writes the sealed wire form; returns its length or -1 when it does not fit. */
int le_peer_seal(const struct le_peer_key *key, const char *type, long long ts, const char *nonce,
                 const void *body, size_t body_len, char *out, size_t capacity);
/* Verifies a sealed packet: well-formed, same key id, valid MAC, the expected
 * type, |now - ts| <= window_s, and (when replay is not NULL) never seen
 * before. Only a packet that passes everything is recorded. On success body
 * points into packet. */
int le_peer_open(const struct le_peer_key *key, struct le_peer_replay *replay, long long now,
                 long long window_s, const char *type, const char *packet, size_t length,
                 const char **body, size_t *body_len);
const char *le_peer_error(int code);
#endif
