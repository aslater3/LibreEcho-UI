#include "feature_provenance.h"
#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LE_FEATURE_COUNT 5
#define LE_FEATURE_FILE_MAX 65536
#define LE_FEATURE_PATH_MAX 256
#define LE_FEATURE_HASH_CACHE_COUNT 32
#define LE_FEATURE_HASH_CHUNK 65536
#define LE_FEATURE_HASH_MAX (512ULL * 1024ULL * 1024ULL)

typedef enum {
    LE_HASH_MISSING = 0,
    LE_HASH_OK = 1,
    LE_HASH_UNAVAILABLE = -1,
    LE_HASH_PENDING = 2
} le_hash_result;

typedef struct {
    int valid;
    char path[LE_FEATURE_PATH_MAX];
    struct stat st;
    char hash[65];
    le_hash_result result;
} le_hash_cache_entry;

static le_hash_cache_entry hash_cache[LE_FEATURE_HASH_CACHE_COUNT];

/* Small self-contained SHA-256 implementation keeps the UI target dependency-free. */
typedef struct {
    unsigned int state[8];
    unsigned long long bits;
    unsigned char block[64];
    size_t used;
} le_sha256;

typedef struct {
    int active;
    int fd;
    char path[LE_FEATURE_PATH_MAX];
    struct stat before;
    le_sha256 sha;
    unsigned long long total;
} le_hash_job;

static le_hash_job hash_job = {0};

static unsigned int sha_rotr(unsigned int value, unsigned int count)
{
    return (value >> count) | (value << (32U - count));
}

static const unsigned int sha_k[64] = {
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

static void sha256_transform(le_sha256 *ctx, const unsigned char *block)
{
    unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
    unsigned int s0, s1, ch, maj;
    size_t i;
    for (i = 0; i < 16; ++i)
        w[i] = ((unsigned int)block[i * 4] << 24) |
               ((unsigned int)block[i * 4 + 1] << 16) |
               ((unsigned int)block[i * 4 + 2] << 8) |
               (unsigned int)block[i * 4 + 3];
    for (i = 16; i < 64; ++i) {
        s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        s1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        ch = (e & f) ^ (~e & g);
        t1 = h + s1 + ch + sha_k[i] + w[i];
        s0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(le_sha256 *ctx)
{
    static const unsigned int initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bits = 0;
    ctx->used = 0;
}

static void sha256_update(le_sha256 *ctx, const unsigned char *data, size_t size)
{
    while (size) {
        size_t take = sizeof(ctx->block) - ctx->used;
        if (take > size)
            take = size;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        ctx->bits += (unsigned long long)take * 8ULL;
        data += take;
        size -= take;
        if (ctx->used == sizeof(ctx->block)) {
            sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha256_final(le_sha256 *ctx, char *out, size_t out_size)
{
    unsigned char length[8];
    size_t i;
    if (out_size < 65)
        return;
    for (i = 0; i < sizeof(length); ++i)
        length[7 - i] = (unsigned char)(ctx->bits >> (i * 8));
    sha256_update(ctx, (const unsigned char *)"\x80", 1);
    while (ctx->used != 56)
        sha256_update(ctx, (const unsigned char *)"\0", 1);
    sha256_update(ctx, length, sizeof(length));
    for (i = 0; i < 8; ++i)
        snprintf(out + i * 8, 9, "%08x", ctx->state[i]);
    out[64] = 0;
}

static const char *feature_ids[LE_FEATURE_COUNT] = {
    "airplay2", "tts", "wakeword", "stt", "assistant"
};
static const char *feature_services[LE_FEATURE_COUNT] = {
    "airplayd", "ttsd", "waked", "sttd", "agentd"
};

static const char *update_root(void)
{
    const char *path = getenv("LIBREECHO_UPDATE_ROOT");
    return path && path[0] ? path : "/data/libreecho/update";
}

static const char *feature_root(void)
{
    const char *path = getenv("LIBREECHO_FEATURE_ROOT");
    return path && path[0] ? path : "/data/libreecho/features";
}

static const char *feature_run_root(void)
{
    const char *path = getenv("LIBREECHO_FEATURE_RUN_ROOT");
    return path && path[0] ? path : "/var/run";
}

static int path_join(char *out, size_t size, const char *root, const char *name)
{
    int n = snprintf(out, size, "%s/%s", root, name);
    return n >= 0 && (size_t)n < size;
}

static int read_bounded(const char *path, char *out, size_t size)
{
    int fd;
    struct stat st;
    ssize_t n = 0;
    size_t used = 0;

    if (!out || size < 2)
        return 0;
    out[0] = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || (off_t)st.st_size >= (off_t)size) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    while (used < size - 1 && (n = read(fd, out + used, size - 1 - used)) > 0)
        used += (size_t)n;
    close(fd);
    if (n < 0 || used >= size - 1) {
        out[0] = 0;
        return 0;
    }
    out[used] = 0;
    return 1;
}

static int text_value(const char *text, const char *key, char *out, size_t size)
{
    const char *line = text;
    size_t key_len = strlen(key);

    if (!text || !out || !size)
        return 0;
    out[0] = 0;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > key_len && !strncmp(line, key, key_len) && line[key_len] == '=') {
            length -= key_len + 1;
            if (!length || length >= size)
                return 0;
            memcpy(out, line + key_len + 1, length);
            out[length] = 0;
            return 1;
        }
        if (!end)
            break;
        line = end + 1;
    }
    return 0;
}

static int file_value(const char *path, const char *key, char *out, size_t size)
{
    char text[2048];
    return read_bounded(path, text, sizeof(text)) && text_value(text, key, out, size);
}

static int json_file_value(const char *text, const char *key, char *out, size_t size)
{
    char value[512];
    int rc = json_get_string(text, key, value, sizeof(value));
    if (rc != 1 || strlen(value) >= size)
        return 0;
    memcpy(out, value, strlen(value) + 1);
    return 1;
}

static int json_object_value(const char *text, const char *object,
                             const char *key, char *out, size_t size)
{
    char needle[256];
    const char *start;
    int n;
    n = snprintf(needle, sizeof(needle), "\"%s\"", object);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return 0;
    start = strstr(text, needle);
    if (!start)
        return 0;
    start = strchr(start + n, ':');
    if (!start)
        return 0;
    return json_file_value(start + 1, key, out, size);
}

static int valid_hash(const char *value)
{
    size_t i;
    if (!value || strlen(value) != 64)
        return 0;
    for (i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)value[i]))
            return 0;
    return 1;
}

static int copy_hash_json(const char *json, const char *object, const char *key,
                          char *out, size_t size)
{
    char value[80];
    if (!json_object_value(json, object, key, value, sizeof(value)) ||
        !valid_hash(value) || strlen(value) >= size)
        return 0;
    memcpy(out, value, strlen(value) + 1);
    return 1;
}

static int feature_manifest(int index, char *path, size_t path_size,
                            char *json, size_t json_size, int *runtime,
                            int *availability)
{
    char name[160];
    struct stat st;
    if (runtime)
        *runtime = 0;
    if (availability)
        *availability = 0;
    if (!path_join(name, sizeof(name), feature_ids[index], "runtime-manifest.json") ||
        !path_join(path, path_size, feature_root(), name))
        return 0;
    if (read_bounded(path, json, json_size)) {
        if (!json_valid_object(json, strlen(json))) {
            if (availability)
                *availability = -1;
            return 0;
        }
        if (runtime)
            *runtime = 1;
        if (availability)
            *availability = 1;
        return 1;
    }
    /* An existing runtime generation whose metadata is unreadable is not a
     * license to reinterpret the feature as the base generation. */
    if (!lstat(path, &st)) {
        if (availability)
            *availability = -1;
        return 0;
    }
    if (errno != ENOENT) {
        if (availability)
            *availability = -1;
        return 0;
    }
    if (!path_join(name, sizeof(name), feature_ids[index], "runtime.squashfs") ||
        !path_join(path, path_size, feature_root(), name))
        return 0;
    /* A runtime capsule without its metadata is an unavailable runtime
     * generation, distinct from a feature with no runtime generation. */
    if (!lstat(path, &st)) {
        if (availability)
            *availability = -1;
        return 0;
    }
    if (errno != ENOENT) {
        if (availability)
            *availability = -1;
        return 0;
    }
    if (!path_join(name, sizeof(name), feature_ids[index], "manifest.json") ||
        !path_join(path, path_size, feature_root(), name))
        return 0;
    if (!read_bounded(path, json, json_size)) {
        if (!lstat(path, &st) && availability)
            *availability = -1;
        return 0;
    }
    if (!json_valid_object(json, strlen(json))) {
        if (availability)
            *availability = -1;
        return 0;
    }
    if (availability)
        *availability = 1;
    return 1;
}

static int same_stat(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_size == right->st_size && left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
           left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
           left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
           left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

static int safe_prefixes(const char *path)
{
    char prefix[LE_FEATURE_PATH_MAX];
    size_t i, used = 0;
    struct stat st;
    if (!path || path[0] != '/' || strlen(path) >= sizeof(prefix) ||
        strstr(path, "//") || strstr(path, "/../") || strstr(path, "/..") ||
        strchr(path, 92))
        return 0;
    prefix[used++] = '/';
    for (i = 1; path[i]; ++i) {
        if (used + 1 >= sizeof(prefix))
            return 0;
        prefix[used++] = path[i];
        if (path[i] != '/' && path[i + 1] != 0)
            continue;
        prefix[used] = 0;
        if (!lstat(prefix, &st)) {
            if ((S_ISLNK(st.st_mode) && path[i + 1] != 0) ||
                (path[i + 1] != 0 && !S_ISDIR(st.st_mode)))
                return 0;
        } else if (errno != ENOENT) {
            return 0;
        } else {
            /* A missing parent makes the artifact missing, not trusted. */
            return 1;
        }
    }
    return 1;
}

static le_hash_cache_entry *cached_hash(const char *path, const struct stat *st)
{
    size_t i;
    for (i = 0; i < LE_FEATURE_HASH_CACHE_COUNT; ++i)
        if (hash_cache[i].valid && !strcmp(hash_cache[i].path, path) &&
            same_stat(&hash_cache[i].st, st))
            return &hash_cache[i];
    return NULL;
}

static void cache_hash(const char *path, const struct stat *st, le_hash_result result,
                       const char *hash)
{
    static size_t next;
    le_hash_cache_entry *entry = &hash_cache[next++ % LE_FEATURE_HASH_CACHE_COUNT];
    memset(entry, 0, sizeof(*entry));
    entry->valid = 1;
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->st = *st;
    entry->result = result;
    if (hash)
        snprintf(entry->hash, sizeof(entry->hash), "%s", hash);
}

static void hash_job_close(void)
{
    if (hash_job.active && hash_job.fd >= 0)
        close(hash_job.fd);
    memset(&hash_job, 0, sizeof(hash_job));
    hash_job.fd = -1;
}

static int hash_job_matches(const char *path, const struct stat *st)
{
    return hash_job.active && !strcmp(hash_job.path, path) &&
           same_stat(&hash_job.before, st);
}

static void hash_job_finish(le_hash_result result, const char *hash)
{
    struct stat current;
    if (result == LE_HASH_OK &&
        (stat(hash_job.path, &current) || !same_stat(&hash_job.before, &current)))
        result = LE_HASH_UNAVAILABLE;
    cache_hash(hash_job.path, &hash_job.before, result,
               result == LE_HASH_OK ? hash : NULL);
    hash_job_close();
}

static le_hash_result hash_job_start(const char *path, int fd,
                                     const struct stat *before)
{
    if (hash_job.active) {
        if (hash_job_matches(path, before)) {
            close(fd);
            return LE_HASH_PENDING;
        }
        if (strcmp(hash_job.path, path)) {
            close(fd);
            return LE_HASH_PENDING;
        }
        hash_job_close();
    }
    hash_job.active = 1;
    hash_job.fd = fd;
    snprintf(hash_job.path, sizeof(hash_job.path), "%s", path);
    hash_job.before = *before;
    hash_job.total = 0;
    sha256_init(&hash_job.sha);
    return LE_HASH_PENDING;
}

static le_hash_result hash_job_step(void)
{
    /* Regular-file O_NONBLOCK does not bound read latency; one chunk per loop does. */
    unsigned char buffer[LE_FEATURE_HASH_CHUNK];
    struct stat after;
    char hash[65];
    ssize_t count;
    if (!hash_job.active)
        return LE_HASH_UNAVAILABLE;
    count = read(hash_job.fd, buffer, sizeof(buffer));
    if (count > 0) {
        hash_job.total += (unsigned long long)count;
        if (hash_job.total > LE_FEATURE_HASH_MAX) {
            hash_job_finish(LE_HASH_UNAVAILABLE, NULL);
            return LE_HASH_UNAVAILABLE;
        }
        sha256_update(&hash_job.sha, buffer, (size_t)count);
        return LE_HASH_PENDING;
    }
    if (count < 0 && errno == EINTR)
        return LE_HASH_PENDING;
    if (count < 0 || hash_job.total != (unsigned long long)hash_job.before.st_size ||
        fstat(hash_job.fd, &after) || !same_stat(&hash_job.before, &after)) {
        hash_job_finish(LE_HASH_UNAVAILABLE, NULL);
        return LE_HASH_UNAVAILABLE;
    }
    sha256_final(&hash_job.sha, hash, sizeof(hash));
    if (!valid_hash(hash)) {
        hash_job_finish(LE_HASH_UNAVAILABLE, NULL);
        return LE_HASH_UNAVAILABLE;
    }
    hash_job_finish(LE_HASH_OK, hash);
    return LE_HASH_OK;
}

static le_hash_result hash_path(const char *path, int allow_final_symlink,
                                char *out, size_t out_size)
{
    struct stat listed, opened;
    le_hash_cache_entry *cached;
    int fd, flags = O_RDONLY | O_CLOEXEC;
    le_hash_result result;
    if (!out || out_size < 65 || !safe_prefixes(path))
        return LE_HASH_UNAVAILABLE;
    out[0] = 0;
    if (!allow_final_symlink && lstat(path, &listed))
        return errno == ENOENT ? LE_HASH_MISSING : LE_HASH_UNAVAILABLE;
    if (!allow_final_symlink && S_ISLNK(listed.st_mode))
        return LE_HASH_UNAVAILABLE;
    if (!allow_final_symlink && !S_ISREG(listed.st_mode))
        return LE_HASH_UNAVAILABLE;
#ifdef O_NOFOLLOW
    if (!allow_final_symlink)
        flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags);
    if (fd < 0)
        return errno == ENOENT ? LE_HASH_MISSING : LE_HASH_UNAVAILABLE;
    if (fstat(fd, &opened) || !S_ISREG(opened.st_mode)) {
        close(fd);
        return LE_HASH_UNAVAILABLE;
    }
    cached = cached_hash(path, &opened);
    if (cached) {
        result = cached->result;
        if (result == LE_HASH_OK)
            snprintf(out, out_size, "%s", cached->hash);
        close(fd);
        return result;
    }
    if (opened.st_size < 0 ||
        (unsigned long long)opened.st_size > LE_FEATURE_HASH_MAX) {
        cache_hash(path, &opened, LE_HASH_UNAVAILABLE, NULL);
        close(fd);
        return LE_HASH_UNAVAILABLE;
    }
    return hash_job_start(path, fd, &opened);
}

static void daemon_identity(int index, char *status, size_t status_size,
                            char *hash, size_t hash_size)
{
    char path[LE_FEATURE_PATH_MAX], value[32];
    char observed[65];
    long pid;
    char *end;
    le_hash_result result;

    snprintf(path, sizeof(path), "%s/libreecho-%s.pid", feature_run_root(), feature_services[index]);
    if (!read_bounded(path, value, sizeof(value))) {
        snprintf(path, sizeof(path), "/run/libreecho-%s.pid", feature_services[index]);
        if (!read_bounded(path, value, sizeof(value))) {
            snprintf(status, status_size, "not-running");
            snprintf(hash, hash_size, "not-running");
            return;
        }
    }
    value[strcspn(value, "\r\n")] = 0;
    errno = 0;
    pid = strtol(value, &end, 10);
    if (errno || end == value || *end || pid <= 1 || kill((pid_t)pid, 0) < 0) {
        snprintf(status, status_size, "not-running");
        snprintf(hash, hash_size, "not-running");
        return;
    }
    snprintf(status, status_size, "running");
    snprintf(hash, hash_size, "unavailable");
    snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
    result = hash_path(path, 1, observed, sizeof(observed));
    if (result == LE_HASH_OK)
        snprintf(hash, hash_size, "%s", observed);
}

static int feature_artifact_path(int index, const char *name, char *out, size_t size)
{
    char dir[LE_FEATURE_PATH_MAX];
    return path_join(dir, sizeof(dir), feature_root(), feature_ids[index]) &&
           path_join(out, size, dir, name);
}

static int valid_asset_name(const char *value)
{
    size_t i;
    if (!value || !value[0] || strlen(value) >= 160)
        return 0;
    for (i = 0; value[i]; ++i)
        if (!isalnum((unsigned char)value[i]) && value[i] != '.' && value[i] != '_' &&
            value[i] != '-' && value[i] != '+')
            return 0;
    return 1;
}

static void candidate_identity(int index, char *kind, size_t kind_size,
                               char *status, size_t status_size,
                               char *hash, size_t hash_size)
{
    char manifest_path[LE_FEATURE_PATH_MAX], text[2048], action[32], asset[160];
    char expected[80], path[LE_FEATURE_PATH_MAX], observed[65];
    le_hash_result result;
    snprintf(kind, kind_size, "unavailable");
    snprintf(status, status_size, "unavailable");
    snprintf(hash, hash_size, "unavailable");
    if (!path_join(manifest_path, sizeof(manifest_path), update_root(), "staging/manifest") ||
        !read_bounded(manifest_path, text, sizeof(text)))
        return;
    {
        char key[128];
        snprintf(key, sizeof(key), "feature_%s_action", feature_ids[index]);
        if (!text_value(text, key, action, sizeof(action)))
            return;
        snprintf(key, sizeof(key), "feature_%s_asset", feature_ids[index]);
        if (!text_value(text, key, asset, sizeof(asset)))
            return;
        snprintf(key, sizeof(key), "feature_%s_sha256", feature_ids[index]);
        if (!text_value(text, key, expected, sizeof(expected)) ||
            !valid_hash(expected) || !valid_asset_name(asset))
            return;
    }
    if (!strcmp(action, "runtime")) {
        if (strlen(asset) < strlen(".runtime.squashfs") ||
            strcmp(asset + strlen(asset) - strlen(".runtime.squashfs"), ".runtime.squashfs"))
            return;
        snprintf(kind, kind_size, "runtime");
    } else if (!strcmp(action, "replace")) {
        if (strlen(asset) < strlen(".payload.squashfs") ||
            strcmp(asset + strlen(asset) - strlen(".payload.squashfs"), ".payload.squashfs"))
            return;
        snprintf(kind, kind_size, "replacement");
    } else {
        return;
    }
    if (!path_join(path, sizeof(path), update_root(), "staging/features"))
        return;
    {
        char feature_dir[LE_FEATURE_PATH_MAX];
        if (!path_join(feature_dir, sizeof(feature_dir), path, feature_ids[index]) ||
            !path_join(path, sizeof(path), feature_dir, asset))
            return;
    }
    result = hash_path(path, 0, observed, sizeof(observed));
    if (result == LE_HASH_MISSING) {
        snprintf(status, status_size, "missing");
    } else if (result == LE_HASH_PENDING) {
        snprintf(status, status_size, "pending");
    } else if (result == LE_HASH_UNAVAILABLE) {
        snprintf(status, status_size, "unavailable");
    } else {
        snprintf(hash, hash_size, "%s", observed);
        snprintf(status, status_size, "%s", !strcmp(observed, expected) ? "present" : "mismatch");
    }
}

static void observe_expected(const char *path, const char *expected, char *observed,
                             size_t observed_size, char *status, size_t status_size)
{
    le_hash_result result;
    snprintf(observed, observed_size, "unavailable");
    snprintf(status, status_size, "unavailable");
    if (!valid_hash(expected))
        return;
    result = hash_path(path, 0, observed, observed_size);
    if (result == LE_HASH_MISSING) {
        snprintf(observed, observed_size, "unavailable");
        snprintf(status, status_size, "missing");
    } else if (result == LE_HASH_PENDING) {
        snprintf(observed, observed_size, "unavailable");
        snprintf(status, status_size, "pending");
    } else if (result == LE_HASH_UNAVAILABLE) {
        snprintf(observed, observed_size, "unavailable");
        snprintf(status, status_size, "unavailable");
    } else if (!strcmp(observed, expected)) {
        snprintf(status, status_size, "present");
    } else {
        snprintf(observed, observed_size, "unavailable");
        snprintf(status, status_size, "mismatch");
    }
}

static int append(char *out, size_t size, size_t *used, const char *format, ...)
{
    va_list ap;
    int n;
    if (*used >= size)
        return 0;
    va_start(ap, format);
    n = vsnprintf(out + *used, size - *used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - *used)
        return 0;
    *used += (size_t)n;
    return 1;
}

void le_feature_transaction_state_read(le_feature_transaction_state *state)
{
    char path[256], text[2048], value[64];
    int pending_file, commit_file, rollback_file;

    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    snprintf(state->state, sizeof(state->state), "none");
    snprintf(state->last_result, sizeof(state->last_result), "idle");

    pending_file = path_join(path, sizeof(path), update_root(), "pending") &&
                   read_bounded(path, text, sizeof(text));
    if (pending_file) {
        state->pending = 1;
        snprintf(state->state, sizeof(state->state), "pending");
        if (text_value(text, "state", value, sizeof(value)) && !strcmp(value, "reboot-pending")) {
            state->reboot_required = 1;
            snprintf(state->last_result, sizeof(state->last_result), "reboot-required");
        } else {
            snprintf(state->last_result, sizeof(state->last_result), "pending");
        }
    }

    commit_file = path_join(path, sizeof(path), update_root(), "feature-commit") &&
                  read_bounded(path, text, sizeof(text));
    if (commit_file) {
        state->commit_pending = 1;
        snprintf(state->state, sizeof(state->state), "commit");
        if (text_value(text, "phase", value, sizeof(value)) &&
            (!strcmp(value, "prepared") || !strcmp(value, "confirmed")))
            snprintf(state->last_result, sizeof(state->last_result), "commit-pending");
        else
            snprintf(state->last_result, sizeof(state->last_result), "pending");
    }

    rollback_file = path_join(path, sizeof(path), update_root(), "rolled-back") &&
                    read_bounded(path, text, sizeof(text));
    if (rollback_file) {
        state->rollback = 1;
        snprintf(state->state, sizeof(state->state), "rollback");
        snprintf(state->last_result, sizeof(state->last_result), "rolled-back");
    }
    if (!state->pending && !state->commit_pending && !state->rollback) {
        path_join(path, sizeof(path), update_root(), "installed");
        if (file_value(path, "phase", value, sizeof(value)) && !strcmp(value, "installed"))
            snprintf(state->last_result, sizeof(state->last_result), "installed");
    }
}

void le_feature_components_json(char *out, size_t size,
                                const le_feature_transaction_state *transaction)
{
    size_t used = 0;
    int i;
    if (!out || !size)
        return;
    out[0] = 0;
    if (!append(out, size, &used, "["))
        return;
    for (i = 0; i < LE_FEATURE_COUNT; ++i) {
        char path[LE_FEATURE_PATH_MAX], json[LE_FEATURE_FILE_MAX];
        char release[160] = "unavailable", source_commit[160] = "unavailable";
        char expected_effective[80] = "", expected_capsule[80] = "";
        char effective_payload[80] = "unavailable", runtime_capsule[80] = "";
        char effective_state[32] = "missing", capsule_state[32] = "unavailable";
        char candidate_kind[32], candidate_state[32], candidate_hash[80];
        char running_hash[80], running_status[32], activation[32] = "unavailable";
        char transaction_result[32] = "idle";
        char e_id[160], e_release[320], e_source[320], e_effective[160];
        char e_capsule[160], capsule_json[192], e_running[160], e_status[80];
        char e_effective_state[32], e_activation[80], e_result[80];
        char e_candidate_kind[80], e_candidate_state[80], e_candidate_hash[160];
        char expected_filename[160], canonical_path[LE_FEATURE_PATH_MAX];
        int runtime = 0, availability = 0, have_manifest;

        expected_filename[0] = 0;
        /* The allow-list is the identity even when all observations are absent. */
        daemon_identity(i, running_status, sizeof(running_status), running_hash,
                        sizeof(running_hash));
        candidate_identity(i, candidate_kind, sizeof(candidate_kind), candidate_state,
                           sizeof(candidate_state), candidate_hash, sizeof(candidate_hash));
        have_manifest = feature_manifest(i, path, sizeof(path), json, sizeof(json), &runtime,
                                         &availability);
        if (!have_manifest) {
            snprintf(effective_state, sizeof(effective_state), "%s",
                     availability < 0 ? "unavailable" : "missing");
        } else {
            /* Release and source are independent observations. */
            json_file_value(json, "product_release", release, sizeof(release));
            json_file_value(json, "source_commit", source_commit, sizeof(source_commit));
            if (runtime) {
                json_file_value(json, "base_payload_sha256", expected_effective,
                                sizeof(expected_effective));
                copy_hash_json(json, "payload", "sha256", expected_capsule,
                               sizeof(expected_capsule));
                json_object_value(json, "payload", "filename", expected_filename,
                                  sizeof(expected_filename));
                if (strcmp(expected_filename, "runtime.squashfs"))
                    expected_capsule[0] = 0;
                if (valid_hash(expected_effective)) {
                    if (feature_artifact_path(i, "payload.squashfs", canonical_path,
                                              sizeof(canonical_path)))
                        observe_expected(canonical_path, expected_effective,
                                         effective_payload, sizeof(effective_payload),
                                         effective_state, sizeof(effective_state));
                } else {
                    snprintf(effective_state, sizeof(effective_state), "unavailable");
                }
                if (valid_hash(expected_capsule)) {
                    if (feature_artifact_path(i, "runtime.squashfs", canonical_path,
                                              sizeof(canonical_path)))
                        observe_expected(canonical_path, expected_capsule, runtime_capsule,
                                         sizeof(runtime_capsule), capsule_state,
                                         sizeof(capsule_state));
                } else {
                    snprintf(effective_state, sizeof(effective_state), "unavailable");
                }
                if (strcmp(capsule_state, "present")) {
                    runtime_capsule[0] = 0;
                    if (!strcmp(effective_state, "present"))
                        snprintf(effective_state, sizeof(effective_state), "%s", capsule_state);
                }
            } else {
                json_object_value(json, "payload", "filename", expected_filename,
                                  sizeof(expected_filename));
                copy_hash_json(json, "payload", "sha256", expected_effective,
                               sizeof(expected_effective));
                if (strcmp(expected_filename, "payload.squashfs"))
                    expected_effective[0] = 0;
                if (valid_hash(expected_effective)) {
                    if (feature_artifact_path(i, "payload.squashfs", canonical_path,
                                              sizeof(canonical_path)))
                        observe_expected(canonical_path, expected_effective,
                                         effective_payload, sizeof(effective_payload),
                                         effective_state, sizeof(effective_state));
                } else {
                    snprintf(effective_state, sizeof(effective_state), "unavailable");
                }
            }
        }
        if (path_join(path, sizeof(path), update_root(), "staging/manifest")) {
            char key[128], value[64];
            snprintf(key, sizeof(key), "feature_%s_activation", feature_ids[i]);
            if (file_value(path, key, value, sizeof(value)) && !strcmp(value, "reboot"))
                snprintf(activation, sizeof(activation), "reboot");
        }
        if (transaction)
            snprintf(transaction_result, sizeof(transaction_result), "%s",
                     transaction->last_result);
        json_escape(e_id, sizeof(e_id), feature_ids[i]);
        json_escape(e_release, sizeof(e_release), release);
        json_escape(e_source, sizeof(e_source), source_commit);
        json_escape(e_effective, sizeof(e_effective), effective_payload);
        json_escape(e_capsule, sizeof(e_capsule), runtime_capsule);
        if (runtime_capsule[0])
            snprintf(capsule_json, sizeof(capsule_json), "\"%s\"", e_capsule);
        else
            snprintf(capsule_json, sizeof(capsule_json), "null");
        json_escape(e_running, sizeof(e_running), running_hash);
        json_escape(e_status, sizeof(e_status), running_status);
        json_escape(e_effective_state, sizeof(e_effective_state), effective_state);
        json_escape(e_activation, sizeof(e_activation), activation);
        json_escape(e_result, sizeof(e_result), transaction_result);
        json_escape(e_candidate_kind, sizeof(e_candidate_kind), candidate_kind);
        json_escape(e_candidate_state, sizeof(e_candidate_state), candidate_state);
        json_escape(e_candidate_hash, sizeof(e_candidate_hash), candidate_hash);
        if (!append(out, size, &used,
                    "%s{\"feature_id\":\"%s\",\"release\":\"%s\",\"source_commit\":\"%s\",\"effective_payload_sha256\":\"%s\",\"runtime_capsule_sha256\":%s,\"candidate_kind\":\"%s\",\"candidate_payload_sha256\":\"%s\",\"candidate_status\":\"%s\",\"running_daemon_sha256\":\"%s\",\"running_daemon_status\":\"%s\",\"effective\":\"%s\",\"activation\":\"%s\",\"last_transaction_result\":\"%s\"}",
                    i ? "," : "", e_id, e_release, e_source, e_effective,
                    capsule_json, e_candidate_kind, e_candidate_hash, e_candidate_state,
                    e_running, e_status, e_effective_state, e_activation, e_result)) {
            out[0] = 0;
            return;
        }
    }
    append(out, size, &used, "]");
}

void le_feature_provenance_tick(void)
{
    char scratch[LE_FEATURE_COMPONENTS_JSON_MAX];
    le_feature_transaction_state transaction;
    if (hash_job.active)
        (void)hash_job_step();
    if (!hash_job.active) {
        le_feature_transaction_state_read(&transaction);
        le_feature_components_json(scratch, sizeof(scratch), &transaction);
    }
}

void le_feature_provenance_shutdown(void)
{
    hash_job_close();
    memset(hash_cache, 0, sizeof(hash_cache));
}
