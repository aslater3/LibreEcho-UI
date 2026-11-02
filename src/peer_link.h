#ifndef LE_PEER_LINK_H
#define LE_PEER_LINK_H
#include <stddef.h>
#include "peer_auth.h"
#include "peers.h"

/* Paired-device check for the device list (#450).
 *
 * The browsing device picks a fresh nonce and asks each listed device
 *   GET /api/v1/peer/proof?nonce=<nonce>   (Host: <address>:<port>)
 * A paired device answers with its name, room, version and the Host it was
 * asked under, signed (type "peer-proof") with the house key over that nonce.
 * The answering device signs only when Host is one of its own IPv4 addresses
 * and its plain HTTP port, and the asking device checks the signed host is
 * the address it connected to, so a third machine cannot relay a real
 * device's proof under its own address. The nonce makes every proof single
 * use, so no clock agreement is needed. */

#define LE_PEER_PROOF_TYPE "peer-proof"
#define LE_PEER_VERIFY_MS 800
#define LE_PEER_RESPONSE_MAX 4096

struct le_peer_proof {
    char name[64];
    char room[64];
    char version[32];
    char host[48];
};

enum {
    LE_PEER_TRUST_OFF = 0,  /* this device has no house key; nothing checked */
    LE_PEER_TRUST_SELF,     /* this device */
    LE_PEER_TRUST_PAIRED,   /* answered with a valid proof for our key */
    LE_PEER_TRUST_UNPAIRED, /* answered: no house key, or another one */
    LE_PEER_TRUST_FAILED    /* no answer, or a proof that did not verify */
};
const char *le_peer_trust_name(int trust);

/* Canonical signed bytes: name, room, version and host joined by '\n'. The
 * fields cannot hold control characters, so the join is unambiguous. */
int le_peer_proof_body(const struct le_peer_proof *proof, char *out, size_t capacity);
/* The "data" object of the proof response, as JSON. */
int le_peer_proof_json(const struct le_peer_key *key, const struct le_peer_proof *proof, const char *nonce,
                       long long ts, char *out, size_t capacity);
/* Checks a whole proof response body (the API envelope) against the nonce
 * and host the caller used. Returns a LE_PEER_TRUST_* value; *proof is
 * filled only for LE_PEER_TRUST_PAIRED. */
int le_peer_proof_verify(const struct le_peer_key *key, int status, const char *body, const char *nonce,
                         const char *host, struct le_peer_proof *proof);
/* Asks every device in list (except those with skip[i] set) in parallel,
 * waiting at most timeout_ms in total, and writes trust[i] (skipped entries
 * are LE_PEER_TRUST_SELF). Without a key every entry, this device included,
 * is LE_PEER_TRUST_OFF and nothing is sent. */
void le_peer_verify_all(const struct le_peer_key *key, const struct le_peer *list, size_t count,
                        const int *skip, int timeout_ms, int *trust);
#endif
