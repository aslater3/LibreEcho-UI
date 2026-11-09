/* Only the nearest device answers (#408). See wake_arbiter.h. */
#define _POSIX_C_SOURCE 200809L
#include "wake_arbiter.h"
#include "../json.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_MAX 16384

static int hex_id(const char *s, size_t n){size_t i;for(i=0;i<n;i++)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')))return 0;return 1;}

int le_wake_claim_format(const struct le_wake_claim *c, char *out, size_t cap)
{
    int n;
    if (!c || !out || strlen(c->id) != LE_WAKE_ARB_ID_HEX || !hex_id(c->id, LE_WAKE_ARB_ID_HEX) ||
        c->level < LE_WAKE_ARB_LEVEL_MIN || c->level > 0 || c->score < 0 || c->score > 1000) return -1;
    n = snprintf(out, cap, "v=1 id=%s level=%d score=%d", c->id, c->level, c->score);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* Strict: exactly the format above, nothing before or after. */
int le_wake_claim_parse(const char *body, size_t length, struct le_wake_claim *c)
{
    char text[96], *p, *end;
    long v;
    if (!body || !c || length < 4 || length >= sizeof(text) || memchr(body, 0, length)) return -1;
    memcpy(text, body, length); text[length] = 0;
    if (strncmp(text, "v=1 id=", 7)) return -1;
    p = text + 7;
    if (strlen(p) < LE_WAKE_ARB_ID_HEX + 1 || !hex_id(p, LE_WAKE_ARB_ID_HEX) || p[LE_WAKE_ARB_ID_HEX] != ' ') return -1;
    memcpy(c->id, p, LE_WAKE_ARB_ID_HEX); c->id[LE_WAKE_ARB_ID_HEX] = 0;
    p += LE_WAKE_ARB_ID_HEX + 1;
    if (strncmp(p, "level=", 6)) return -1;
    p += 6;
    if (!((*p >= '0' && *p <= '9') || (*p == '-' && p[1] >= '0' && p[1] <= '9'))) return -1;
    errno = 0; v = strtol(p, &end, 10);
    if (errno || end == p || *end != ' ' || v < LE_WAKE_ARB_LEVEL_MIN || v > 0) return -1;
    c->level = (int)v;
    p = end + 1;
    if (strncmp(p, "score=", 6)) return -1;
    p += 6;
    if (*p < '0' || *p > '9') return -1;
    errno = 0; v = strtol(p, &end, 10);
    if (errno || end == p || *end || v < 0 || v > 1000) return -1;
    c->score = (int)v;
    return 0;
}

int le_wake_claim_rank(const struct le_wake_claim *a, const struct le_wake_claim *b)
{
    int id;
    if (a->level != b->level) return a->level > b->level ? 1 : -1;
    if (a->score != b->score) return a->score > b->score ? 1 : -1;
    id = strcmp(a->id, b->id);
    return id < 0 ? 1 : id > 0 ? -1 : 0;
}

int le_wake_level(const int16_t *samples, size_t count)
{
    double sum = 0.0, db;
    size_t i;
    if (!samples || !count) return LE_WAKE_ARB_LEVEL_MIN;
    for (i = 0; i < count; i++) sum += (double)samples[i] * (double)samples[i];
    sum /= (double)count;
    if (sum <= 0.0) return LE_WAKE_ARB_LEVEL_MIN;
    db = 10.0 * log10(sum / (32768.0 * 32768.0));
    if (db > 0.0) db = 0.0;
    if (db * 10.0 < (double)LE_WAKE_ARB_LEVEL_MIN) return LE_WAKE_ARB_LEVEL_MIN;
    return (int)floor(db * 10.0 + 0.5);
}

int le_wake_score_milli(float score)
{
    if (!(score > 0.0f)) return 0;
    if (score >= 1.0f) return 1000;
    return (int)(score * 1000.0f + 0.5f);
}

static void close_socket(struct le_wake_arbiter *a){if(a->fd>=0)close(a->fd);a->fd=-1;}

static int open_socket(struct le_wake_arbiter *a)
{
    struct sockaddr_in addr;
    int yes = 1, fd = socket(AF_INET, SOCK_DGRAM, 0), flags;
    if (fd < 0) return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) { close(fd); return -1; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(a->bind_port); addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    a->fd = fd;
    return 0;
}

static unsigned short port_value(const char *s, unsigned short fallback)
{
    char *end; long v;
    if (!s || !*s) return fallback;
    errno = 0; v = strtol(s, &end, 10);
    return errno || *end || v < 1 || v > 65535 ? fallback : (unsigned short)v;
}

void le_wake_arbiter_init(struct le_wake_arbiter *a, const char *config_path, const char *key_path, int use_env)
{
    const char *v;
    char nonce[LE_PEER_NONCE_HEX + 1];
    memset(a, 0, sizeof(*a));
    a->fd = -1;
    a->config_mtime = a->key_mtime = -1;
    a->bind_port = a->dest_port = LE_WAKE_ARB_PORT;
    snprintf(a->dest_host, sizeof(a->dest_host), "255.255.255.255");
    if (config_path) snprintf(a->config_path, sizeof(a->config_path), "%s", config_path);
    if (key_path) snprintf(a->key_path, sizeof(a->key_path), "%s", key_path);
    if (use_env) {
        if ((v = getenv("LE_WAKE_ARB_CONFIG")) && strlen(v) < sizeof(a->config_path)) snprintf(a->config_path, sizeof(a->config_path), "%s", v);
        if ((v = getenv("LE_WAKE_ARB_KEY")) && strlen(v) < sizeof(a->key_path)) snprintf(a->key_path, sizeof(a->key_path), "%s", v);
        a->bind_port = port_value(getenv("LE_WAKE_ARB_BIND_PORT"), a->bind_port);
        if ((v = getenv("LE_WAKE_ARB_DEST")) && *v && strlen(v) < sizeof(a->dest_host)) {
            const char *colon = strchr(v, ':');
            snprintf(a->dest_host, sizeof(a->dest_host), "%.*s", colon ? (int)(colon - v) : (int)strlen(v), v);
            if (colon) a->dest_port = port_value(colon + 1, a->dest_port);
        }
        if ((v = getenv("LE_WAKE_ARB_ID")) && strlen(v) == LE_WAKE_ARB_ID_HEX && hex_id(v, LE_WAKE_ARB_ID_HEX))
            snprintf(a->self.id, sizeof(a->self.id), "%s", v);
    }
    if (!a->self.id[0]) {
        if (le_peer_nonce(nonce)) {
            /* No randomness: fall back to time and pid. Uniqueness only
             * breaks ties between equal claims. */
            snprintf(nonce, sizeof(nonce), "%08lx%08lx", (unsigned long)time(NULL) & 0xffffffffUL, (unsigned long)getpid() & 0xffffffffUL);
        }
        memcpy(a->self.id, nonce, LE_WAKE_ARB_ID_HEX); a->self.id[LE_WAKE_ARB_ID_HEX] = 0;
    }
}

void le_wake_arbiter_close(struct le_wake_arbiter *a){close_socket(a);le_peer_key_clear(&a->key);a->pending=0;}

int le_wake_arbiter_active(const struct le_wake_arbiter *a){return a->enabled&&a->key.set&&a->fd>=0;}
int le_wake_arbiter_fd(const struct le_wake_arbiter *a){return le_wake_arbiter_active(a)?a->fd:-1;}

/* 1 when the file changed since last time (or appeared/disappeared). */
static int file_changed(const char *path, long long *mtime, long long *size, unsigned long long *ino)
{
    struct stat st;
    long long m = -2, s = -2; unsigned long long i = 0;
    if (path[0] && !stat(path, &st)) { m = (long long)st.st_mtime; s = (long long)st.st_size; i = (unsigned long long)st.st_ino; }
    if (m == *mtime && s == *size && i == *ino) return 0;
    *mtime = m; *size = s; *ino = i;
    return 1;
}

static int read_switch(const char *path)
{
    char buf[CONFIG_MAX + 1];
    size_t used = 0;
    ssize_t n;
    int fd, value = 0;
    if (!path[0]) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    while (used < CONFIG_MAX && (n = read(fd, buf + used, CONFIG_MAX - used)) > 0) used += (size_t)n;
    close(fd);
    if (used >= CONFIG_MAX) return 0;
    buf[used] = 0;
    if (!json_valid_object(buf, used) || json_get_bool(buf, "nearest_answers", &value) != 1) return 0;
    return value ? 1 : 0;
}

void le_wake_arbiter_reload(struct le_wake_arbiter *a, uint64_t now_ms)
{
    int was = le_wake_arbiter_active(a);
    a->reload_at_ms = now_ms + LE_WAKE_ARB_RELOAD_MS;
    if (file_changed(a->config_path, &a->config_mtime, &a->config_size, &a->config_ino)) a->enabled = read_switch(a->config_path);
    if (file_changed(a->key_path, &a->key_mtime, &a->key_size, &a->key_ino)) {
        char old[LE_PEER_KEY_ID_LEN + 1];
        snprintf(old, sizeof(old), "%s", a->key.set ? a->key.id : "");
        if (!a->key_path[0] || le_peer_key_load(&a->key, a->key_path) <= 0) le_peer_key_clear(&a->key);
        /* Another house key: claims sealed with the old one mean nothing. */
        if (strcmp(old, a->key.set ? a->key.id : "")) { memset(&a->replay, 0, sizeof(a->replay)); memset(a->heard, 0, sizeof(a->heard)); }
    }
    if (a->enabled && a->key.set) { if (a->fd < 0 && open_socket(a) < 0) a->send_errors++; }
    else close_socket(a);
    if (was != le_wake_arbiter_active(a))
        fprintf(stderr, "waked: nearest-device answering %s\n", le_wake_arbiter_active(a) ? "on" : !a->enabled ? "off" : !a->key.set ? "off (not paired)" : "off (socket unavailable)");
    if (!le_wake_arbiter_active(a)) a->pending = 0;
}

void le_wake_arbiter_heard(struct le_wake_arbiter *a, uint64_t now_ms, const struct le_wake_claim *c)
{
    size_t i;
    if (!strcmp(c->id, a->self.id)) return;
    a->heard_count++;
    /* One entry per device: a newer claim replaces its older one. */
    for (i = 0; i < LE_WAKE_ARB_HISTORY; i++)
        if (a->heard[i].used && !strcmp(a->heard[i].claim.id, c->id)) { a->heard[i].claim = *c; a->heard[i].heard_ms = now_ms; a->heard[i].settled = 0; return; }
    a->heard[a->heard_next].claim = *c;
    a->heard[a->heard_next].heard_ms = now_ms;
    a->heard[a->heard_next].used = 1;
    a->heard[a->heard_next].settled = 0;
    a->heard_next = (a->heard_next + 1) % LE_WAKE_ARB_HISTORY;
}

int le_wake_arbiter_packet(struct le_wake_arbiter *a, uint64_t now_ms, long long wall_s, const char *packet, size_t length)
{
    const char *body; size_t body_len; struct le_wake_claim c;
    int rc = le_peer_open(&a->key, &a->replay, wall_s, LE_WAKE_ARB_CLOCK_WINDOW_S, LE_WAKE_ARB_TYPE, packet, length, &body, &body_len);
    if (rc == LE_PEER_OK && le_wake_claim_parse(body, body_len, &c)) rc = LE_PEER_EFORMAT;
    if (rc == LE_PEER_EREPLAY) return rc; /* our resend, or a replay: already counted */
    if (rc) {
        a->rejected++;
        if (rc != a->last_reject) fprintf(stderr, "waked: nearest-device claim refused: %s\n", le_peer_error(rc));
        a->last_reject = rc;
        return rc;
    }
    if (strcmp(c.id, a->self.id)) {
        /* One line per wake per device: shows on hardware how late claims
         * arrive relative to ours. */
        if (a->pending) fprintf(stderr, "waked: claim from device %s level %d, %lld ms after ours\n", c.id, c.level, (long long)now_ms - (long long)a->claim_ms);
        else fprintf(stderr, "waked: claim from device %s level %d\n", c.id, c.level);
    }
    le_wake_arbiter_heard(a, now_ms, &c);
    return LE_PEER_OK;
}

static void send_claim(struct le_wake_arbiter *a)
{
    struct sockaddr_in to;
    if (a->fd < 0 || a->packet_len <= 0) return;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET; to.sin_port = htons(a->dest_port);
    if (inet_pton(AF_INET, a->dest_host, &to.sin_addr) != 1 ||
        sendto(a->fd, a->packet, (size_t)a->packet_len, 0, (struct sockaddr *)&to, sizeof(to)) != a->packet_len)
        a->send_errors++;
}

int le_wake_arbiter_claim(struct le_wake_arbiter *a, uint64_t now_ms, long long wall_s, int level, int score)
{
    char body[96], nonce[LE_PEER_NONCE_HEX + 1];
    size_t i;
    int n;
    if (!le_wake_arbiter_active(a)) return LE_WAKE_ARB_OFF;
    if (a->pending) return LE_WAKE_ARB_LOST;
    a->self.level = level < LE_WAKE_ARB_LEVEL_MIN ? LE_WAKE_ARB_LEVEL_MIN : level > 0 ? 0 : level;
    a->self.score = score < 0 ? 0 : score > 1000 ? 1000 : score;
    a->beaten_by[0] = 0;
    /* A device whose window has already closed has taken this turn. */
    for (i = 0; i < LE_WAKE_ARB_HISTORY; i++) {
        uint64_t age;
        if (!a->heard[i].used || a->heard[i].settled || a->heard[i].heard_ms > now_ms) continue;
        age = now_ms - a->heard[i].heard_ms;
        if (age > LE_WAKE_ARB_COMPARE_BEFORE_MS && age <= LE_WAKE_ARB_LOOKBACK_MS) {
            snprintf(a->beaten_by, sizeof(a->beaten_by), "%s", a->heard[i].claim.id);
            a->yielded++;
            fprintf(stderr, "waked: wake yielded: device %s answered %llu ms earlier\n", a->beaten_by, (unsigned long long)age);
            return LE_WAKE_ARB_LOST;
        }
    }
    if (le_wake_claim_format(&a->self, body, sizeof(body)) < 0 || le_peer_nonce(nonce) ||
        (n = le_peer_seal(&a->key, LE_WAKE_ARB_TYPE, wall_s, nonce, body, strlen(body), a->packet, sizeof(a->packet))) < 0) {
        /* Cannot claim: answer, as before arbitration. */
        a->send_errors++;
        return LE_WAKE_ARB_OFF;
    }
    a->packet_len = n;
    a->pending = 1;
    a->claim_ms = now_ms;
    a->decide_ms = now_ms + LE_WAKE_ARB_WINDOW_MS;
    a->resend_ms = now_ms + LE_WAKE_ARB_RESEND_MS;
    send_claim(a);
    return LE_WAKE_ARB_PENDING;
}

static int decide(struct le_wake_arbiter *a)
{
    size_t i;
    const struct le_wake_claim *best = NULL;
    uint64_t from = a->claim_ms > LE_WAKE_ARB_COMPARE_BEFORE_MS ? a->claim_ms - LE_WAKE_ARB_COMPARE_BEFORE_MS : 0;
    for (i = 0; i < LE_WAKE_ARB_HISTORY; i++) {
        if (!a->heard[i].used || a->heard[i].heard_ms < from || a->heard[i].heard_ms > a->decide_ms) continue;
        if (le_wake_claim_rank(&a->heard[i].claim, &a->self) > 0 && (!best || le_wake_claim_rank(&a->heard[i].claim, best) > 0)) best = &a->heard[i].claim;
    }
    /* Claims compared here are settled: a device that lost to us must not
     * look, at our next wake, like one that answered. */
    for (i = 0; i < LE_WAKE_ARB_HISTORY; i++)
        if (a->heard[i].used && a->heard[i].heard_ms <= a->decide_ms) a->heard[i].settled = 1;
    a->pending = 0;
    if (best) {
        snprintf(a->beaten_by, sizeof(a->beaten_by), "%s", best->id);
        a->yielded++;
        fprintf(stderr, "waked: wake yielded to device %s (level %d vs %d here)\n", best->id, best->level, a->self.level);
        return LE_WAKE_ARB_LOST;
    }
    a->won++;
    fprintf(stderr, "waked: wake answered here (level %d)\n", a->self.level);
    return LE_WAKE_ARB_WON;
}

int le_wake_arbiter_tick(struct le_wake_arbiter *a, uint64_t now_ms, long long wall_s)
{
    unsigned loops;
    if (now_ms >= a->reload_at_ms) le_wake_arbiter_reload(a, now_ms);
    if (a->fd >= 0) {
        /* Bounded drain: at most 16 packets a tick. */
        for (loops = 0; loops < 16; loops++) {
            char packet[LE_WAKE_ARB_PACKET_MAX + 1];
            ssize_t n = recv(a->fd, packet, sizeof(packet), MSG_DONTWAIT);
            if (n < 0) break;
            if ((size_t)n > LE_WAKE_ARB_PACKET_MAX || !le_wake_arbiter_active(a)) continue;
            (void)le_wake_arbiter_packet(a, now_ms, wall_s, packet, (size_t)n);
        }
    }
    if (!a->pending) return LE_WAKE_ARB_IDLE;
    if (a->resend_ms && now_ms >= a->resend_ms) { a->resend_ms = 0; send_claim(a); }
    if (now_ms < a->decide_ms) return LE_WAKE_ARB_PENDING;
    return decide(a);
}

int le_wake_arbiter_timeout(const struct le_wake_arbiter *a, uint64_t now_ms)
{
    uint64_t next;
    if (!a->pending) return -1;
    next = a->resend_ms && a->resend_ms < a->decide_ms ? a->resend_ms : a->decide_ms;
    return next <= now_ms ? 0 : (int)(next - now_ms);
}

int le_wake_arbiter_status_json(const struct le_wake_arbiter *a, char *out, size_t cap)
{
    int n = snprintf(out, cap, "{\"enabled\":%s,\"paired\":%s,\"active\":%s,\"won\":%lu,\"yielded\":%lu,\"heard\":%lu,\"rejected\":%lu}",
                     a->enabled ? "true" : "false", a->key.set ? "true" : "false", le_wake_arbiter_active(a) ? "true" : "false",
                     a->won, a->yielded, a->heard_count, a->rejected);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}
