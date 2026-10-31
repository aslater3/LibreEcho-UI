/* Signed device-to-device messages (#450): HMAC vectors, the pairing code,
 * the key file, seal/open with every forged or replayed variant refused, and
 * the device-list proof. */
#include "peer_auth.h"
#include "peer_link.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void hex(const unsigned char *in, size_t n, char *out){size_t i;for(i=0;i<n;i++)sprintf(out+i*2,"%02x",in[i]);}

static void test_hmac_vectors(void)
{
    unsigned char key[131], out[32];
    char text[65];
    /* RFC 4231 test cases 1, 2 and 6 (key longer than the block). */
    memset(key, 0x0b, 20);
    le_peer_hmac_sha256(key, 20, (const unsigned char *)"Hi There", 8, out); hex(out, 32, text);
    CHECK(!strcmp(text, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    le_peer_hmac_sha256((const unsigned char *)"Jefe", 4, (const unsigned char *)"what do ya want for nothing?", 28, out); hex(out, 32, text);
    CHECK(!strcmp(text, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
    memset(key, 0xaa, sizeof(key));
    le_peer_hmac_sha256(key, sizeof(key), (const unsigned char *)"Test Using Larger Than Block-Size Key - Hash Key First", 54, out); hex(out, 32, text);
    CHECK(!strcmp(text, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static const char *CODE = "0011-2233-4455-6677-8899-aabb-ccdd-eeff-0123-4567-89ab-cdef-fedc-ba98-7654-3210";

static void test_code_and_key_id(void)
{
    struct le_peer_key a, b, c;
    char code[LE_PEER_CODE_LEN + 1];
    CHECK(!le_peer_key_from_code(&a, CODE));
    CHECK(a.set && strlen(a.id) == LE_PEER_KEY_ID_LEN);
    CHECK(!le_peer_key_code(&a, code, sizeof(code)) && !strcmp(code, CODE) && strlen(code) == LE_PEER_CODE_LEN);
    /* Case and separators do not matter; the key id is the same. */
    CHECK(!le_peer_key_from_code(&b, "00112233 44556677 8899AABB CCDDEEFF 01234567 89ABCDEF FEDCBA98 76543210"));
    CHECK(!strcmp(a.id, b.id) && !memcmp(a.mac_key, b.mac_key, sizeof(a.mac_key)));
    CHECK(le_peer_key_from_code(&c, "0011") < 0);
    CHECK(le_peer_key_from_code(&c, "00112233445566778899aabbccddeeff0123456789abcdeffedcba987654321") < 0);
    CHECK(le_peer_key_from_code(&c, "00112233445566778899aabbccddeeff0123456789abcdeffedcba98765432100") < 0);
    CHECK(le_peer_key_from_code(&c, "g0112233445566778899aabbccddeeff0123456789abcdeffedcba9876543210") < 0);
    CHECK(le_peer_key_from_code(&c, "") < 0);
    CHECK(!le_peer_key_generate(&c) && c.set && strcmp(c.id, a.id));
    /* The id never reveals the key. */
    CHECK(!strstr(CODE, a.id));
}

static void test_key_file(void)
{
    char dir[] = "/tmp/le-peer-auth-XXXXXX", path[128], tmp[140];
    struct le_peer_key a, b;
    struct stat st;
    FILE *f;
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/peer.key", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK(le_peer_key_load(&b, path) == 0 && !b.set);
    CHECK(!le_peer_key_from_code(&a, CODE));
    CHECK(!le_peer_key_save(&a, path));
    CHECK(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    CHECK(access(tmp, F_OK) != 0);
    CHECK(le_peer_key_load(&b, path) == 1 && b.set && !strcmp(a.id, b.id));
    f = fopen(path, "w"); CHECK(f != NULL); if (f) { fputs("not a key\n", f); fclose(f); }
    CHECK(le_peer_key_load(&b, path) == -1 && !b.set);
    CHECK(!le_peer_key_remove(path) && access(path, F_OK) != 0);
    CHECK(!le_peer_key_remove(path));
    CHECK(le_peer_key_load(&b, path) == 0 && !b.set);
    rmdir(dir);
}

static void flip(char *s, size_t at){s[at] = s[at] == 'a' ? 'b' : 'a';}

static void test_seal_open(void)
{
    struct le_peer_key key, other, none;
    struct le_peer_replay replay;
    char packet[512], forged[512], nonce[LE_PEER_NONCE_HEX + 1];
    const char *body;
    size_t len, body_len, head;
    const char msg[] = "{\"wake\":\"radar\",\"score\":0.91}";
    int n;
    long long now = 1800000000LL;
    memset(&replay, 0, sizeof(replay));
    memset(&none, 0, sizeof(none));
    CHECK(!le_peer_key_from_code(&key, CODE));
    CHECK(!le_peer_key_generate(&other));
    CHECK(!le_peer_nonce(nonce) && le_peer_nonce_valid(nonce));
    n = le_peer_seal(&key, "arbitration.wake", now, nonce, msg, strlen(msg), packet, sizeof(packet));
    CHECK(n > 0); len = (size_t)n;
    CHECK(!strncmp(packet, "LESIG1 ", 7));
    head = (size_t)(strchr(packet, '\n') - packet) + 1;
    CHECK(le_peer_open(&key, &replay, now + 3, 30, "arbitration.wake", packet, len, &body, &body_len) == LE_PEER_OK);
    CHECK(body_len == strlen(msg) && !memcmp(body, msg, body_len));
    /* The same packet again is a replay. */
    CHECK(le_peer_open(&key, &replay, now + 4, 30, "arbitration.wake", packet, len, &body, &body_len) == LE_PEER_EREPLAY);
    CHECK(body == NULL && body_len == 0);

    /* Forged packets: altered body, altered header fields, altered MAC. */
    memcpy(forged, packet, len); forged[len - 3] = '8';
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, len, &body, &body_len) == LE_PEER_EMAC);
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", packet, len - 1, &body, &body_len) == LE_PEER_EMAC);
    memcpy(forged, packet, len); memcpy(forged + len, "x", 1);
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, len + 1, &body, &body_len) == LE_PEER_EMAC);
    memcpy(forged, packet, len); flip(forged, head - 2);
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, len, &body, &body_len) == LE_PEER_EMAC);
    {   /* Timestamp moved forward by one second. */
        char *ts;
        memcpy(forged, packet, len); ts = strstr(forged, " 1800000000 "); CHECK(ts != NULL); if (ts) ts[10] = '1';
        CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, len, &body, &body_len) == LE_PEER_EMAC);
    }
    {   /* Same body, a type the sender did not sign. */
        char *t;
        n = snprintf(forged, sizeof(forged), "%s", packet); (void)n;
        t = strstr(forged, "arbitration.wake"); CHECK(t != NULL); if (t) t[0] = 'b';
        CHECK(le_peer_open(&key, NULL, now, 30, "brbitration.wake", forged, len, &body, &body_len) == LE_PEER_EMAC);
    }
    /* A genuine message of another type is not accepted as this one. */
    CHECK(le_peer_open(&key, NULL, now, 30, "intercom.call", packet, len, &body, &body_len) == LE_PEER_ETYPE);
    /* Sealed with another house key, or opened by a device with no key. */
    n = le_peer_seal(&other, "arbitration.wake", now, nonce, msg, strlen(msg), forged, sizeof(forged));
    CHECK(n > 0 && le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, (size_t)n, &body, &body_len) == LE_PEER_EKEY);
    {   /* Another key that claims our key id still fails on the MAC. */
        char *id = strchr(forged, ' ') + 1;
        memcpy(id, key.id, LE_PEER_KEY_ID_LEN);
        CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, (size_t)n, &body, &body_len) == LE_PEER_EMAC);
    }
    CHECK(le_peer_open(&none, NULL, now, 30, "arbitration.wake", packet, len, &body, &body_len) == LE_PEER_EKEY);
    CHECK(le_peer_seal(&none, "arbitration.wake", now, nonce, msg, strlen(msg), forged, sizeof(forged)) < 0);
    /* Stale and future packets. */
    CHECK(le_peer_open(&key, NULL, now + 31, 30, "arbitration.wake", packet, len, &body, &body_len) == LE_PEER_ESTALE);
    CHECK(le_peer_open(&key, NULL, now - 31, 30, "arbitration.wake", packet, len, &body, &body_len) == LE_PEER_ESTALE);
    /* Malformed. */
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", "LESIG1 x\n", 9, &body, &body_len) == LE_PEER_EFORMAT);
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", "hello", 5, &body, &body_len) == LE_PEER_EFORMAT);
    memcpy(forged, packet, len); forged[6] = '\t';
    CHECK(le_peer_open(&key, NULL, now, 30, "arbitration.wake", forged, len, &body, &body_len) == LE_PEER_EFORMAT);
    /* Bad inputs to seal. */
    CHECK(le_peer_seal(&key, "Bad Type", now, nonce, msg, 3, forged, sizeof(forged)) < 0);
    CHECK(le_peer_seal(&key, "ok", now, "abc", msg, 3, forged, sizeof(forged)) < 0);
    CHECK(le_peer_seal(&key, "ok", now, "0123456789ABCDEF0123456789abcdef", msg, 3, forged, sizeof(forged)) < 0);
    CHECK(le_peer_seal(&key, "ok", now, nonce, msg, strlen(msg), forged, 40) < 0);
    CHECK(!strcmp(le_peer_error(LE_PEER_EMAC), "bad-signature"));
}

static void test_replay_cache_fails_closed(void)
{
    struct le_peer_key key;
    struct le_peer_replay replay;
    char packets[LE_PEER_REPLAY_SLOTS + 1][256], nonce[LE_PEER_NONCE_HEX + 1];
    int lens[LE_PEER_REPLAY_SLOTS + 1], i;
    long long now = 1800000000LL;
    memset(&replay, 0, sizeof(replay));
    CHECK(!le_peer_key_from_code(&key, CODE));
    for (i = 0; i <= LE_PEER_REPLAY_SLOTS; i++) {
        CHECK(!le_peer_nonce(nonce));
        lens[i] = le_peer_seal(&key, "group.volume", now + i / 8, nonce, "{}", 2, packets[i], sizeof(packets[i]));
        CHECK(lens[i] > 0);
        CHECK(le_peer_open(&key, &replay, now + 10, 60, "group.volume", packets[i], (size_t)lens[i], NULL, NULL) == LE_PEER_OK);
    }
    /* The first packet's slot was reused while still in the window: it, and
       anything as old, is refused rather than accepted twice. */
    CHECK(le_peer_open(&key, &replay, now + 10, 60, "group.volume", packets[0], (size_t)lens[0], NULL, NULL) == LE_PEER_EREPLAY);
    for (i = 1; i <= LE_PEER_REPLAY_SLOTS; i++)
        CHECK(le_peer_open(&key, &replay, now + 10, 60, "group.volume", packets[i], (size_t)lens[i], NULL, NULL) == LE_PEER_EREPLAY);
    /* Slots that aged out are reused without raising the floor. */
    memset(&replay, 0, sizeof(replay));
    CHECK(le_peer_open(&key, &replay, now, 5, "group.volume", packets[0], (size_t)lens[0], NULL, NULL) == LE_PEER_OK);
    CHECK(!le_peer_nonce(nonce));
    lens[1] = le_peer_seal(&key, "group.volume", now + 100, nonce, "{}", 2, packets[1], sizeof(packets[1]));
    CHECK(le_peer_open(&key, &replay, now + 100, 5, "group.volume", packets[1], (size_t)lens[1], NULL, NULL) == LE_PEER_OK);
    CHECK(replay.floor == 0);
}

static void test_proof(void)
{
    struct le_peer_key key, other, none;
    struct le_peer_proof p, got;
    char data[1024], body[1200], nonce[LE_PEER_NONCE_HEX + 1], nonce2[LE_PEER_NONCE_HEX + 1], *at;
    memset(&none, 0, sizeof(none));
    CHECK(!le_peer_key_from_code(&key, CODE));
    CHECK(!le_peer_key_generate(&other));
    CHECK(!le_peer_nonce(nonce) && !le_peer_nonce(nonce2));
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "kitchen-echo"); snprintf(p.room, sizeof(p.room), "Kitchen \"Main\"");
    snprintf(p.version, sizeof(p.version), "0.14.6"); snprintf(p.host, sizeof(p.host), "192.0.2.10:8080");
    CHECK(le_peer_proof_json(&key, &p, nonce, 1800000000LL, data, sizeof(data)) > 0);
    snprintf(body, sizeof(body), "{\"ok\":true,\"data\":%s,\"error\":null}", data);
    CHECK(le_peer_proof_verify(&key, 200, body, nonce, "192.0.2.10:8080", &got) == LE_PEER_TRUST_PAIRED);
    CHECK(!strcmp(got.room, p.room) && !strcmp(got.name, p.name));
    /* Another nonce (replayed proof) or another address (relayed proof). */
    CHECK(le_peer_proof_verify(&key, 200, body, nonce2, "192.0.2.10:8080", NULL) == LE_PEER_TRUST_FAILED);
    CHECK(le_peer_proof_verify(&key, 200, body, nonce, "192.0.2.99:8080", NULL) == LE_PEER_TRUST_FAILED);
    /* Forged field. */
    at = strstr(body, "kitchen-echo"); CHECK(at != NULL); if (at) at[0] = 'K';
    CHECK(le_peer_proof_verify(&key, 200, body, nonce, "192.0.2.10:8080", NULL) == LE_PEER_TRUST_FAILED);
    /* Another house: signed with another key. */
    CHECK(le_peer_proof_json(&other, &p, nonce, 1800000000LL, data, sizeof(data)) > 0);
    snprintf(body, sizeof(body), "{\"ok\":true,\"data\":%s,\"error\":null}", data);
    CHECK(le_peer_proof_verify(&key, 200, body, nonce, "192.0.2.10:8080", NULL) == LE_PEER_TRUST_UNPAIRED);
    CHECK(le_peer_proof_verify(&key, 409, "{\"ok\":false,\"data\":null,\"error\":{\"code\":\"not_paired\",\"message\":\"x\"}}", nonce, "h", NULL) == LE_PEER_TRUST_UNPAIRED);
    CHECK(le_peer_proof_verify(&key, 404, "{}", nonce, "h", NULL) == LE_PEER_TRUST_UNPAIRED);
    CHECK(le_peer_proof_verify(&key, 500, "{}", nonce, "h", NULL) == LE_PEER_TRUST_FAILED);
    CHECK(le_peer_proof_verify(&key, 200, "not json", nonce, "h", NULL) == LE_PEER_TRUST_FAILED);
    CHECK(le_peer_proof_verify(&none, 200, body, nonce, "h", NULL) == LE_PEER_TRUST_OFF);
    /* Control characters cannot be signed (they would make the join ambiguous). */
    snprintf(p.room, sizeof(p.room), "a\nb");
    CHECK(le_peer_proof_json(&key, &p, nonce, 1, data, sizeof(data)) < 0);
    CHECK(!strcmp(le_peer_trust_name(LE_PEER_TRUST_PAIRED), "paired"));
}

int main(void)
{
    test_hmac_vectors();
    test_code_and_key_id();
    test_key_file();
    test_seal_open();
    test_replay_cache_fails_closed();
    test_proof();
    if (failures) { fprintf(stderr, "test-peer-auth: %d failure(s)\n", failures); return 1; }
    puts("test-peer-auth: ok");
    return 0;
}
