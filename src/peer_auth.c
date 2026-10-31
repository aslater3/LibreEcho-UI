/* Signed device-to-device messages (#450). See peer_auth.h. */
#include "peer_auth.h"
#include "le_sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char hex_digits[] = "0123456789abcdef";

static void wipe(void *p, size_t n){volatile unsigned char *v = (volatile unsigned char *)p;while(n--)*v++ = 0;}
static int hex_value(int ch){if(ch>='0'&&ch<='9')return ch-'0';if(ch>='a'&&ch<='f')return ch-'a'+10;if(ch>='A'&&ch<='F')return ch-'A'+10;return -1;}
static void to_hex(const unsigned char *in, size_t n, char *out){size_t i;for(i=0;i<n;i++){out[i*2]=hex_digits[in[i]>>4];out[i*2+1]=hex_digits[in[i]&15];}out[n*2]=0;}

/* le_sha256.h finishes to hex; turn that back into the 32 raw bytes HMAC needs. */
static void sha256_raw(le_sha256 *ctx, unsigned char out[32]){char hex[65];size_t i;sha256_final(ctx,hex,sizeof(hex));for(i=0;i<32;i++)out[i]=(unsigned char)(hex_value(hex[i*2])<<4|hex_value(hex[i*2+1]));wipe(hex,sizeof(hex));}

void le_peer_hmac_sha256(const unsigned char *key, size_t key_len, const unsigned char *data, size_t data_len, unsigned char out[32])
{
    unsigned char block[64], pad[64], inner[32];
    le_sha256 ctx;
    size_t i;
    memset(block, 0, sizeof(block));
    if (key_len > sizeof(block)) { sha256_init(&ctx); sha256_update(&ctx, key, key_len); sha256_raw(&ctx, block); }
    else if (key_len) memcpy(block, key, key_len);
    for (i = 0; i < 64; i++) pad[i] = (unsigned char)(block[i] ^ 0x36);
    sha256_init(&ctx); sha256_update(&ctx, pad, 64); if (data_len) sha256_update(&ctx, data, data_len); sha256_raw(&ctx, inner);
    for (i = 0; i < 64; i++) pad[i] = (unsigned char)(block[i] ^ 0x5c);
    sha256_init(&ctx); sha256_update(&ctx, pad, 64); sha256_update(&ctx, inner, 32); sha256_raw(&ctx, out);
    wipe(block, sizeof(block)); wipe(pad, sizeof(pad)); wipe(inner, sizeof(inner)); wipe(&ctx, sizeof(ctx));
}

int le_peer_key_from_secret(struct le_peer_key *key, const unsigned char secret[LE_PEER_KEY_BYTES])
{
    static const char mac_label[] = "LibreEcho peer mac v1", id_label[] = "LibreEcho key id v1";
    unsigned char id[32];
    if (!key || !secret) return -1;
    memset(key, 0, sizeof(*key));
    memcpy(key->secret, secret, LE_PEER_KEY_BYTES);
    le_peer_hmac_sha256(key->secret, LE_PEER_KEY_BYTES, (const unsigned char *)mac_label, sizeof(mac_label) - 1, key->mac_key);
    le_peer_hmac_sha256(key->secret, LE_PEER_KEY_BYTES, (const unsigned char *)id_label, sizeof(id_label) - 1, id);
    to_hex(id, LE_PEER_KEY_ID_LEN / 2, key->id);
    key->set = 1;
    wipe(id, sizeof(id));
    return 0;
}

static int random_fill(unsigned char *out, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    if (fd < 0) return -1;
    while (got < n) { ssize_t r = read(fd, out + got, n - got); if (r < 0 && errno == EINTR) continue; if (r <= 0) { close(fd); return -1; } got += (size_t)r; }
    close(fd);
    return 0;
}

int le_peer_key_generate(struct le_peer_key *key)
{
    unsigned char secret[LE_PEER_KEY_BYTES];
    int rc;
    if (!key) return -1;
    if (random_fill(secret, sizeof(secret))) return -1;
    rc = le_peer_key_from_secret(key, secret);
    wipe(secret, sizeof(secret));
    return rc;
}

int le_peer_key_from_code(struct le_peer_key *key, const char *code)
{
    unsigned char secret[LE_PEER_KEY_BYTES];
    size_t digits = 0, i;
    int rc;
    if (!key || !code || strlen(code) > 128) return -1;
    for (i = 0; code[i]; i++) {
        int v = hex_value((unsigned char)code[i]);
        if (code[i] == '-' || code[i] == ' ') continue;
        if (v < 0 || digits >= LE_PEER_KEY_BYTES * 2) { wipe(secret, sizeof(secret)); return -1; }
        if (digits & 1) secret[digits / 2] = (unsigned char)(secret[digits / 2] | v); else secret[digits / 2] = (unsigned char)(v << 4);
        digits++;
    }
    if (digits != LE_PEER_KEY_BYTES * 2) { wipe(secret, sizeof(secret)); return -1; }
    rc = le_peer_key_from_secret(key, secret);
    wipe(secret, sizeof(secret));
    return rc;
}

int le_peer_key_code(const struct le_peer_key *key, char *out, size_t capacity)
{
    size_t i, used = 0;
    if (!key || !key->set || !out || capacity < LE_PEER_CODE_LEN + 1) return -1;
    for (i = 0; i < LE_PEER_KEY_BYTES; i++) {
        if (i && !(i & 1)) out[used++] = '-';
        out[used++] = hex_digits[key->secret[i] >> 4];
        out[used++] = hex_digits[key->secret[i] & 15];
    }
    out[used] = 0;
    return 0;
}

void le_peer_key_clear(struct le_peer_key *key){if(key)wipe(key,sizeof(*key));}

int le_peer_key_load(struct le_peer_key *key, const char *path)
{
    char buf[160];
    ssize_t n;
    int fd;
    if (!key) return -1;
    le_peer_key_clear(key);
    if (!path || !path[0]) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || (size_t)n >= sizeof(buf) - 1) { wipe(buf, sizeof(buf)); return -1; }
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    n = le_peer_key_from_code(key, buf) ? -1 : 1;
    wipe(buf, sizeof(buf));
    return (int)n;
}

/* Temp file, fsync, rename. No backup copy: an unpaired device must not keep
   the old key anywhere. */
int le_peer_key_save(const struct le_peer_key *key, const char *path)
{
    char code[LE_PEER_CODE_LEN + 2], tmp[512];
    int fd, rc = -1;
    size_t n;
    if (!key || !key->set || !path || !path[0] || strlen(path) + 5 > sizeof(tmp)) return -1;
    if (le_peer_key_code(key, code, sizeof(code))) return -1;
    n = strlen(code); code[n++] = '\n'; code[n] = 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (!fchmod(fd, 0600) && write(fd, code, n) == (ssize_t)n && !fsync(fd)) rc = 0;
        if (close(fd)) rc = -1;
        if (!rc && rename(tmp, path)) rc = -1;
        if (rc) unlink(tmp);
    }
    wipe(code, sizeof(code));
    return rc;
}

int le_peer_key_remove(const char *path)
{
    char tmp[512];
    if (!path || !path[0] || strlen(path) + 5 > sizeof(tmp)) return -1;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
    if (unlink(path) && errno != ENOENT) return -1;
    return 0;
}

int le_peer_type_valid(const char *type)
{
    size_t i;
    if (!type || !type[0]) return 0;
    for (i = 0; type[i]; i++) {
        unsigned char ch = (unsigned char)type[i];
        if (i >= LE_PEER_TYPE_MAX || !((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) return 0;
    }
    return 1;
}

int le_peer_nonce_valid(const char *nonce)
{
    size_t i;
    if (!nonce) return 0;
    for (i = 0; i < LE_PEER_NONCE_HEX; i++) if (!nonce[i] || !strchr(hex_digits, nonce[i])) return 0;
    return nonce[i] == 0;
}

int le_peer_nonce(char out[LE_PEER_NONCE_HEX + 1])
{
    unsigned char raw[LE_PEER_NONCE_HEX / 2];
    if (!out || random_fill(raw, sizeof(raw))) return -1;
    to_hex(raw, sizeof(raw), out);
    return 0;
}

static int mac_raw(const struct le_peer_key *key, const char *type, long long ts, const char *nonce, const void *body, size_t body_len, unsigned char out[32])
{
    unsigned char ipad[64], opad[64], inner[32];
    char head[LE_PEER_HEADER_MAX];
    le_sha256 ctx;
    size_t i;
    int n;
    if (!key || !key->set || !le_peer_type_valid(type) || !le_peer_nonce_valid(nonce) || ts < 0 || (body_len && !body)) return -1;
    n = snprintf(head, sizeof(head), "LESIG1\n%s\n%lld\n%s\n", type, ts, nonce);
    if (n < 0 || (size_t)n >= sizeof(head)) return -1;
    /* HMAC written out so the body is hashed in place instead of copied. */
    memset(ipad, 0x36, sizeof(ipad)); memset(opad, 0x5c, sizeof(opad));
    for (i = 0; i < LE_PEER_KEY_BYTES; i++) { ipad[i] ^= key->mac_key[i]; opad[i] ^= key->mac_key[i]; }
    sha256_init(&ctx); sha256_update(&ctx, ipad, 64); sha256_update(&ctx, (const unsigned char *)head, (size_t)n);
    if (body_len) sha256_update(&ctx, (const unsigned char *)body, body_len);
    sha256_raw(&ctx, inner);
    sha256_init(&ctx); sha256_update(&ctx, opad, 64); sha256_update(&ctx, inner, 32); sha256_raw(&ctx, out);
    wipe(ipad, sizeof(ipad)); wipe(opad, sizeof(opad)); wipe(inner, sizeof(inner)); wipe(&ctx, sizeof(ctx));
    return 0;
}

int le_peer_mac(const struct le_peer_key *key, const char *type, long long ts, const char *nonce, const void *body, size_t body_len, char out[LE_PEER_MAC_HEX + 1])
{
    unsigned char mac[32];
    if (!out || mac_raw(key, type, ts, nonce, body, body_len, mac)) return -1;
    to_hex(mac, sizeof(mac), out);
    wipe(mac, sizeof(mac));
    return 0;
}

int le_peer_check(const struct le_peer_key *key, const char *type, long long ts, const char *nonce, const void *body, size_t body_len, const char *mac)
{
    unsigned char want[32];
    unsigned diff = 0;
    size_t i;
    if (!key || !key->set) return LE_PEER_EKEY;
    if (!mac || strlen(mac) != LE_PEER_MAC_HEX) return LE_PEER_EFORMAT;
    if (mac_raw(key, type, ts, nonce, body, body_len, want)) return LE_PEER_EFORMAT;
    for (i = 0; i < 32; i++) {
        int hi = hex_value((unsigned char)mac[i * 2]), lo = hex_value((unsigned char)mac[i * 2 + 1]);
        if (hi < 0 || lo < 0) { wipe(want, sizeof(want)); return LE_PEER_EFORMAT; }
        diff |= (unsigned)(want[i] ^ (unsigned char)(hi << 4 | lo));
    }
    wipe(want, sizeof(want));
    return diff ? LE_PEER_EMAC : LE_PEER_OK;
}

int le_peer_seal(const struct le_peer_key *key, const char *type, long long ts, const char *nonce, const void *body, size_t body_len, char *out, size_t capacity)
{
    char mac[LE_PEER_MAC_HEX + 1];
    int n;
    if (!out || le_peer_mac(key, type, ts, nonce, body, body_len, mac)) return -1;
    n = snprintf(out, capacity, "LESIG1 %s %s %lld %s %s\n", key->id, type, ts, nonce, mac);
    if (n < 0 || (size_t)n >= capacity || body_len > capacity - (size_t)n) return -1;
    if (body_len) memcpy(out + n, body, body_len);
    if ((size_t)n + body_len < capacity) out[(size_t)n + body_len] = 0;
    return n + (int)body_len;
}

/* Splits the next space- or newline-terminated field of at most cap-1 bytes. */
static int field(const char **p, const char *end, char stop, char *out, size_t cap)
{
    /* Measure first, then copy: a byte-at-a-time copy into out trips GCC 15's
     * -Wstringop-overflow at -O3 on ARM (the waked build). */
    const char *q = *p;
    size_t n;
    while (q < end && *q != stop) { if (*q == ' ' || *q == '\n' || *q == 0) return -1; q++; }
    n = (size_t)(q - *p);
    if (q >= end || !n || n >= cap) return -1;
    memcpy(out, *p, n);
    out[n] = 0; *p = q + 1;
    return 0;
}

static int replay_seen(const struct le_peer_replay *r, const char *nonce, long long ts)
{
    size_t i;
    if (ts <= r->floor) return 1;
    for (i = 0; i < LE_PEER_REPLAY_SLOTS; i++) if (r->seen[i].nonce[0] && !strcmp(r->seen[i].nonce, nonce)) return 1;
    return 0;
}

static void replay_record(struct le_peer_replay *r, const char *nonce, long long ts, long long now, long long window_s)
{
    size_t i, slot = r->next % LE_PEER_REPLAY_SLOTS;
    /* Prefer an empty slot or one that has aged out of the window. */
    for (i = 0; i < LE_PEER_REPLAY_SLOTS; i++) {
        size_t s = (r->next + i) % LE_PEER_REPLAY_SLOTS;
        if (!r->seen[s].nonce[0] || r->seen[s].ts < now - window_s || r->seen[s].ts > now + window_s) { slot = s; break; }
    }
    if (i == LE_PEER_REPLAY_SLOTS && r->seen[slot].ts > r->floor) r->floor = r->seen[slot].ts;
    /* nonce was checked to be exactly LE_PEER_NONCE_HEX hex digits. */
    memcpy(r->seen[slot].nonce, nonce, LE_PEER_NONCE_HEX);
    r->seen[slot].nonce[LE_PEER_NONCE_HEX] = 0;
    r->seen[slot].ts = ts;
    r->next = (slot + 1) % LE_PEER_REPLAY_SLOTS;
}

int le_peer_open(const struct le_peer_key *key, struct le_peer_replay *replay, long long now, long long window_s, const char *type, const char *packet, size_t length, const char **body, size_t *body_len)
{
    char magic[8], id[LE_PEER_KEY_ID_LEN + 2], got_type[LE_PEER_TYPE_MAX + 2], ts_text[24], nonce[LE_PEER_NONCE_HEX + 2], mac[LE_PEER_MAC_HEX + 2];
    const char *p = packet, *end, *head_end;
    char *stop;
    long long ts;
    int rc;
    if (body) *body = NULL;
    if (body_len) *body_len = 0;
    if (!packet || !type) return LE_PEER_EFORMAT;
    if (!key || !key->set) return LE_PEER_EKEY;
    head_end = memchr(packet, '\n', length < LE_PEER_HEADER_MAX ? length : LE_PEER_HEADER_MAX);
    if (!head_end) return LE_PEER_EFORMAT;
    end = head_end + 1;
    if (field(&p, end, ' ', magic, sizeof(magic)) || strcmp(magic, "LESIG1") || field(&p, end, ' ', id, sizeof(id)) ||
        field(&p, end, ' ', got_type, sizeof(got_type)) || field(&p, end, ' ', ts_text, sizeof(ts_text)) ||
        field(&p, end, ' ', nonce, sizeof(nonce)) || field(&p, end, '\n', mac, sizeof(mac)) || p != end)
        return LE_PEER_EFORMAT;
    if (strlen(id) != LE_PEER_KEY_ID_LEN || !le_peer_type_valid(got_type) || !le_peer_nonce_valid(nonce) || ts_text[0] < '0' || ts_text[0] > '9') return LE_PEER_EFORMAT;
    errno = 0; ts = strtoll(ts_text, &stop, 10);
    if (errno || *stop || ts < 0) return LE_PEER_EFORMAT;
    /* key_id is public; comparing it first only says "another house". */
    if (strcmp(id, key->id)) return LE_PEER_EKEY;
    rc = le_peer_check(key, got_type, ts, nonce, end, length - (size_t)(end - packet), mac);
    if (rc) return rc;
    if (strcmp(got_type, type)) return LE_PEER_ETYPE;
    if (window_s < 0 || ts < now - window_s || ts > now + window_s) return LE_PEER_ESTALE;
    if (replay) { if (replay_seen(replay, nonce, ts)) return LE_PEER_EREPLAY; replay_record(replay, nonce, ts, now, window_s); }
    if (body) *body = end;
    if (body_len) *body_len = length - (size_t)(end - packet);
    return LE_PEER_OK;
}

const char *le_peer_error(int code)
{
    switch (code) {
    case LE_PEER_OK: return "ok";
    case LE_PEER_EFORMAT: return "malformed";
    case LE_PEER_EKEY: return "other-key";
    case LE_PEER_EMAC: return "bad-signature";
    case LE_PEER_ETYPE: return "wrong-type";
    case LE_PEER_ESTALE: return "stale";
    case LE_PEER_EREPLAY: return "replayed";
    default: return "error";
    }
}
