#include "auth.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LE_AUTH_SESSION_SECONDS (12 * 60 * 60)

struct sha256 {
    uint32_t h[8];
    uint64_t bits;
    size_t used;
    unsigned char block[64];
};

static uint32_t rotr(uint32_t value, unsigned int count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t ch(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~x & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static const uint32_t k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_block(struct sha256 *ctx)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    unsigned int i;

    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)ctx->block[i * 4] << 24) |
               ((uint32_t)ctx->block[i * 4 + 1] << 16) |
               ((uint32_t)ctx->block[i * 4 + 2] << 8) |
               (uint32_t)ctx->block[i * 4 + 3];
    for (i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 64; ++i) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t t1 = h + s1 + ch(e, f, g) + k[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t t2 = s0 + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(struct sha256 *ctx)
{
    static const uint32_t initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->h, initial, sizeof(initial));
}

static void sha256_update(struct sha256 *ctx, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    while (size) {
        size_t n = sizeof(ctx->block) - ctx->used;
        if (n > size)
            n = size;
        memcpy(ctx->block + ctx->used, bytes, n);
        ctx->used += n;
        ctx->bits += (uint64_t)n * 8U;
        bytes += n;
        size -= n;
        if (ctx->used == sizeof(ctx->block)) {
            sha256_block(ctx);
            ctx->used = 0;
        }
    }
}

static void sha256_final(struct sha256 *ctx, unsigned char digest[32])
{
    unsigned int i;
    uint64_t bits = ctx->bits;
    ctx->block[ctx->used++] = 0x80;
    while (ctx->used != 56) {
        if (ctx->used == sizeof(ctx->block)) {
            sha256_block(ctx);
            ctx->used = 0;
        }
        ctx->block[ctx->used++] = 0;
    }
    for (i = 0; i < 8; ++i)
        ctx->block[56 + i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_block(ctx);
    for (i = 0; i < 8; ++i) {
        digest[i * 4] = (unsigned char)(ctx->h[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(ctx->h[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(ctx->h[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)ctx->h[i];
    }
}

static void digest_hex(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned int i;
    for (i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[64] = 0;
}

static void hash_password(const char *salt, const char *password,
                          char out[65])
{
    struct sha256 ctx;
    unsigned char digest[32];
    sha256_init(&ctx);
    sha256_update(&ctx, salt, strlen(salt));
    sha256_update(&ctx, ":", 1);
    sha256_update(&ctx, password, strlen(password));
    sha256_final(&ctx, digest);
    digest_hex(digest, out);
}

static int constant_equal(const char *a, const char *b)
{
    size_t al = strlen(a), bl = strlen(b), i;
    size_t n = al > bl ? al : bl;
    unsigned int diff = (unsigned int)(al ^ bl);
    for (i = 0; i < n; ++i) {
        unsigned int ac = i < al ? (unsigned char)a[i] : 0;
        unsigned int bc = i < bl ? (unsigned char)b[i] : 0;
        diff |= ac ^ bc;
    }
    return diff == 0;
}

static int valid_hex(const char *value, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

void le_auth_fold_username(char *out, size_t size, const char *in)
{
    size_t i;
    if (!out || !size) return;
    out[0] = '\0';
    if (!in) return;
    for (i = 0; i + 1 < size && in[i]; ++i)
        out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] - 'A' + 'a') : in[i];
    out[i] = '\0';
}

int le_auth_load(struct le_auth_db *db, const char *path)
{
    FILE *file;
    char line[256];

    if (!db)
        return -1;
    memset(db, 0, sizeof(*db));
    if (!path || !path[0])
        return 0;
    file = fopen(path, "r");
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file) &&
           db->user_count < LE_AUTH_MAX_USERS) {
        char *user, *method, *salt, *digest, *end;
        size_t length;
        user = line;
        while (*user == ' ' || *user == '\t')
            ++user;
        if (!*user || *user == '#')
            continue;
        end = strchr(user, '\n');
        if (end)
            *end = '\0';
        end = strchr(user, ':');
        if (!end)
            continue;
        *end = '\0';
        method = end + 1;
        end = strchr(method, ':');
        if (!end)
            continue;
        *end = '\0';
        salt = end + 1;
        end = strchr(salt, ':');
        if (!end)
            continue;
        *end = '\0';
        digest = end + 1;
        length = strlen(user);
        if (!length || length >= LE_AUTH_USERNAME_MAX ||
            strcmp(method, "sha256") || strlen(salt) < 16 ||
            strlen(salt) > 64 || !valid_hex(salt, strlen(salt)) ||
            strlen(digest) != 64 || !valid_hex(digest, 64))
            continue;
        le_auth_fold_username(db->users[db->user_count].username,
                              sizeof(db->users[db->user_count].username), user);
        strncpy(db->users[db->user_count].salt, salt,
                sizeof(db->users[db->user_count].salt) - 1);
        strncpy(db->users[db->user_count].digest, digest,
                sizeof(db->users[db->user_count].digest) - 1);
        db->user_count++;
    }
    fclose(file);
    db->enabled = db->user_count > 0;
    return db->enabled ? 0 : -1;
}

static int random_bytes(void *out, size_t size)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    unsigned char *bytes = (unsigned char *)out;
    size_t used = 0;
    if (fd < 0)
        return -1;
    while (used < size) {
        ssize_t n = read(fd, bytes + used, size - used);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        used += (size_t)n;
    }
    close(fd);
    return 0;
}

static int issue_token(struct le_auth_db *db, const char *username,
                       char *token, size_t token_size, int *expires_in)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[32];
    size_t i, slot = LE_AUTH_MAX_SESSIONS;
    time_t now = time(NULL);
    if (token_size < LE_AUTH_TOKEN_MAX ||
        random_bytes(random, sizeof(random)) < 0)
        return -1;
    for (i = 0; i < LE_AUTH_MAX_SESSIONS; ++i) {
        if (!db->sessions[i].token[0] || db->sessions[i].expires <= now) {
            slot = i;
            break;
        }
    }
    if (slot == LE_AUTH_MAX_SESSIONS)
        slot = 0;
    for (i = 0; i < sizeof(random); ++i) {
        token[i * 2] = hex[random[i] >> 4];
        token[i * 2 + 1] = hex[random[i] & 15];
    }
    token[64] = '\0';
    strncpy(db->sessions[slot].token, token,
            sizeof(db->sessions[slot].token) - 1);
    strncpy(db->sessions[slot].username, username,
            sizeof(db->sessions[slot].username) - 1);
    db->sessions[slot].expires = now + LE_AUTH_SESSION_SECONDS;
    if (expires_in)
        *expires_in = LE_AUTH_SESSION_SECONDS;
    return 0;
}

int le_auth_login(struct le_auth_db *db, const char *username,
                  const char *password, char *token, size_t token_size,
                  int *expires_in)
{
    size_t i;
    char digest[65], folded[LE_AUTH_USERNAME_MAX];
    if (!db || !db->enabled || !username || !password ||
        strlen(password) > LE_AUTH_PASSWORD_MAX)
        return -1;
    le_auth_fold_username(folded, sizeof(folded), username);
    for (i = 0; i < db->user_count; ++i) {
        if (!constant_equal(db->users[i].username, folded))
            continue;
        hash_password(db->users[i].salt, password, digest);
        if (!constant_equal(db->users[i].digest, digest))
            return -1;
        /* issue against the folded name so the session reports it that way */
        return issue_token(db, folded, token, token_size, expires_in);
    }
    return -1;
}

int le_auth_session(struct le_auth_db *db, const char *token,
                    char *username, size_t username_size)
{
    size_t i;
    time_t now = time(NULL);
    if (!db || !db->enabled || !token || !token[0])
        return 0;
    for (i = 0; i < LE_AUTH_MAX_SESSIONS; ++i) {
        if (db->sessions[i].expires <= now) {
            db->sessions[i].token[0] = '\0';
            continue;
        }
        if (constant_equal(db->sessions[i].token, token)) {
            if (username && username_size) {
                strncpy(username, db->sessions[i].username, username_size - 1);
                username[username_size - 1] = '\0';
            }
            return 1;
        }
    }
    return 0;
}

void le_auth_logout(struct le_auth_db *db, const char *token)
{
    size_t i;
    if (!db || !token)
        return;
    for (i = 0; i < LE_AUTH_MAX_SESSIONS; ++i) {
        if (constant_equal(db->sessions[i].token, token))
            db->sessions[i].token[0] = '\0';
    }
}

/* ------------------------- Session persistence -------------------------- */

int le_auth_save_sessions(const struct le_auth_db *db, const char *path)
{
    char temp[512];
    FILE *f;
    size_t i;
    int fd;

    if (!db || !path || !path[0])
        return -1;
    if ((size_t)snprintf(temp, sizeof(temp), "%s.tmp", path) >= sizeof(temp))
        return -1;
    /*
     * 0600 and written through a temporary with a fixed name: the data
     * contract that guards /data matches names exactly, so a uniquely
     * named leftover would be an unknown file and would block every
     * service at the next boot.
     */
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        (void)unlink(temp);
        return -1;
    }
    for (i = 0; i < LE_AUTH_MAX_SESSIONS; ++i) {
        if (!db->sessions[i].token[0])
            continue;
        fprintf(f, "%s %s %lld\n", db->sessions[i].token,
                db->sessions[i].username,
                (long long)db->sessions[i].expires);
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        (void)unlink(temp);
        return -1;
    }
    fclose(f);
    if (rename(temp, path) != 0) {
        (void)unlink(temp);
        return -1;
    }
    return 0;
}

void le_auth_load_sessions(struct le_auth_db *db, const char *path)
{
    char line[256];
    FILE *f;
    time_t now = time(NULL);
    size_t slot = 0;

    if (!db || !path || !path[0])
        return;
    f = fopen(path, "r");
    if (!f)
        return;
    while (slot < LE_AUTH_MAX_SESSIONS && fgets(line, sizeof(line), f)) {
        char token[LE_AUTH_TOKEN_MAX];
        char username[LE_AUTH_USERNAME_MAX];
        long long expires = 0;

        if (sscanf(line, "%64s %31s %lld", token, username, &expires) != 3)
            continue;
        /* A stored session can never outlive the expiry it was issued with. */
        if ((time_t)expires <= now)
            continue;
        snprintf(db->sessions[slot].token,
                 sizeof(db->sessions[slot].token), "%s", token);
        snprintf(db->sessions[slot].username,
                 sizeof(db->sessions[slot].username), "%s", username);
        db->sessions[slot].expires = (time_t)expires;
        ++slot;
    }
    fclose(f);
}

#include "auth_user_management.inc"

