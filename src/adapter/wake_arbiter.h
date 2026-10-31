#ifndef LE_WAKE_ARBITER_H
#define LE_WAKE_ARBITER_H

/* Only the nearest device answers (#408).
 *
 * When several paired LibreEcho devices hear the same wake word, each one
 * broadcasts a signed claim on the LAN with how loud the wake word was here,
 * waits a short window for the others' claims, and only the loudest one hands
 * the wake to its voice assistant. The rest stay quiet.
 *
 * The claim is a LESIG1 packet (peer_auth.h) of type "wake.claim", signed
 * with the house key from pairing (#450), so a device without the key can
 * neither silence a device nor be heard by one. The body is one line:
 *   "v=1 id=<16 hex> level=<-1200..0> score=<0..1000>"
 * id is random per waked start (not a serial), level is the wake word's
 * level in tenths of a dBFS, score the detector's score in thousandths.
 *
 * Timing, on this device's monotonic clock (receive times, never the
 * sender's clock):
 *   - A claim heard up to LE_WAKE_ARB_LOOKBACK_MS before ours, but more than
 *     COMPARE_BEFORE_MS before it, belongs to a device whose window has
 *     already closed: it has answered, so we yield without sending a claim.
 *     Claims we already compared in an earlier window of ours are settled
 *     and do not count again.
 *   - Otherwise we send our claim, wait LE_WAKE_ARB_WINDOW_MS, and answer
 *     only when no claim heard from COMPARE_BEFORE_MS before ours to the end
 *     of the window ranks above ours.
 * Rank: higher level, then higher score, then the lower id. Every device
 * that sends a claim compares the same set, so the best one always answers;
 * a lost packet can only make two answer (the old behaviour), never none.
 *
 * Off unless the owner switches it on ("nearest_answers" in the web config)
 * and the device is paired. Both are re-read every few seconds, so pairing
 * or the switch take effect without a restart. Fixed-size state only. */

#include "../peer_auth.h"
#include <stddef.h>
#include <stdint.h>

#define LE_WAKE_ARB_PORT 6054
#define LE_WAKE_ARB_TYPE "wake.claim"
#define LE_WAKE_ARB_WINDOW_MS 450U
#define LE_WAKE_ARB_COMPARE_BEFORE_MS 150U
#define LE_WAKE_ARB_LOOKBACK_MS 1500U
#define LE_WAKE_ARB_RESEND_MS 60U
#define LE_WAKE_ARB_RELOAD_MS 2000U
/* Signed timestamps must be this close to our wall clock (seconds). */
#define LE_WAKE_ARB_CLOCK_WINDOW_S 30
#define LE_WAKE_ARB_HISTORY 8
#define LE_WAKE_ARB_ID_HEX 16
#define LE_WAKE_ARB_LEVEL_MIN (-1200)
#define LE_WAKE_ARB_PACKET_MAX 256

struct le_wake_claim {
    char id[LE_WAKE_ARB_ID_HEX + 1];
    int level;
    int score;
};

enum le_wake_arb_state {
    LE_WAKE_ARB_IDLE = 0,
    LE_WAKE_ARB_PENDING = 1, /* claim sent, window open */
    LE_WAKE_ARB_WON = 2,     /* answer */
    LE_WAKE_ARB_LOST = 3,    /* another device answers */
    LE_WAKE_ARB_OFF = 4      /* arbitration not active: answer at once */
};

struct le_wake_arbiter {
    /* Settings and key, reloaded from these paths. */
    char config_path[256];
    char key_path[256];
    int enabled;              /* owner switch */
    struct le_peer_key key;
    long long config_mtime, key_mtime;
    long long config_size, key_size;
    unsigned long long config_ino, key_ino;
    uint64_t reload_at_ms;
    int fd;
    unsigned short bind_port;
    char dest_host[64];
    unsigned short dest_port;
    struct le_wake_claim self;
    /* Claims heard from other devices, newest overwrite oldest. */
    struct { struct le_wake_claim claim; uint64_t heard_ms; int used, settled; } heard[LE_WAKE_ARB_HISTORY];
    size_t heard_next;
    struct le_peer_replay replay;
    /* The one claim in flight. */
    int pending;
    uint64_t claim_ms, decide_ms, resend_ms;
    char packet[LE_WAKE_ARB_PACKET_MAX];
    int packet_len;
    char beaten_by[LE_WAKE_ARB_ID_HEX + 1];
    /* Counters for status. */
    unsigned long won, yielded, heard_count, rejected, send_errors;
    int last_reject;
};

/* Pure helpers, used by the daemon and by the tests. */
int le_wake_claim_format(const struct le_wake_claim *claim, char *out, size_t capacity);
int le_wake_claim_parse(const char *body, size_t length, struct le_wake_claim *claim);
/* > 0 when a ranks above b, < 0 when below, 0 only for the same id. */
int le_wake_claim_rank(const struct le_wake_claim *a, const struct le_wake_claim *b);
/* Level of the samples in tenths of a dBFS, LE_WAKE_ARB_LEVEL_MIN for silence. */
int le_wake_level(const int16_t *samples, size_t count);
int le_wake_score_milli(float score);

/* Defaults to the given paths, broadcast to 255.255.255.255:LE_WAKE_ARB_PORT
 * from port LE_WAKE_ARB_PORT. env (may be NULL) can override, for tests:
 * LE_WAKE_ARB_CONFIG, LE_WAKE_ARB_KEY, LE_WAKE_ARB_BIND_PORT,
 * LE_WAKE_ARB_DEST (host or host:port), LE_WAKE_ARB_ID. */
void le_wake_arbiter_init(struct le_wake_arbiter *arb, const char *config_path, const char *key_path, int use_env);
void le_wake_arbiter_close(struct le_wake_arbiter *arb);
/* Re-reads the switch and key when their files change, and opens or closes
 * the socket to match. Called from tick; exposed for tests. */
void le_wake_arbiter_reload(struct le_wake_arbiter *arb, uint64_t now_ms);
int le_wake_arbiter_active(const struct le_wake_arbiter *arb);
int le_wake_arbiter_fd(const struct le_wake_arbiter *arb);

/* A wake was detected here. Returns LE_WAKE_ARB_OFF (answer now: not
 * active), LE_WAKE_ARB_LOST (another device already answered; nothing sent)
 * or LE_WAKE_ARB_PENDING (claim sent; wait for tick). A wake while a claim is
 * pending returns LE_WAKE_ARB_LOST: the first one is already being settled. */
int le_wake_arbiter_claim(struct le_wake_arbiter *arb, uint64_t now_ms, long long wall_s, int level, int score);
/* Records a claim heard from another device (also used by tests without a
 * socket). Claims with our own id are ignored. */
void le_wake_arbiter_heard(struct le_wake_arbiter *arb, uint64_t now_ms, const struct le_wake_claim *claim);
/* Verifies and records one received packet. Returns LE_PEER_OK or the
 * le_peer_open error. */
int le_wake_arbiter_packet(struct le_wake_arbiter *arb, uint64_t now_ms, long long wall_s, const char *packet, size_t length);
/* Drains the socket, resends once and settles the pending claim. Returns
 * LE_WAKE_ARB_WON or LE_WAKE_ARB_LOST once per claim when it settles, else
 * LE_WAKE_ARB_IDLE (or LE_WAKE_ARB_PENDING while waiting). */
int le_wake_arbiter_tick(struct le_wake_arbiter *arb, uint64_t now_ms, long long wall_s);
/* Milliseconds until tick next has work (decision or resend), or -1. */
int le_wake_arbiter_timeout(const struct le_wake_arbiter *arb, uint64_t now_ms);
/* {"enabled":..,"paired":..,"active":..,"won":..,"yielded":..,"heard":..,"rejected":..} */
int le_wake_arbiter_status_json(const struct le_wake_arbiter *arb, char *out, size_t capacity);
#endif
