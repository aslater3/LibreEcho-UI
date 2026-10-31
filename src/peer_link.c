/* Paired-device check for the device list (#450). See peer_link.h. */
#include "peer_link.h"
#include "json.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const char *le_peer_trust_name(int trust)
{
    switch (trust) {
    case LE_PEER_TRUST_SELF: return "self";
    case LE_PEER_TRUST_PAIRED: return "paired";
    case LE_PEER_TRUST_UNPAIRED: return "unpaired";
    case LE_PEER_TRUST_FAILED: return "failed";
    default: return "off";
    }
}

static int text_ok(const char *s, size_t cap, int allow_empty)
{
    size_t i;
    if (!s || (!allow_empty && !s[0])) return 0;
    for (i = 0; s[i]; i++) { unsigned char ch = (unsigned char)s[i]; if (i + 1 >= cap || ch < 0x20 || ch == 0x7f) return 0; }
    return 1;
}

static int proof_fields_ok(const struct le_peer_proof *p)
{
    return p && text_ok(p->name, sizeof(p->name), 0) && text_ok(p->room, sizeof(p->room), 1) &&
           text_ok(p->version, sizeof(p->version), 1) && text_ok(p->host, sizeof(p->host), 0);
}

int le_peer_proof_body(const struct le_peer_proof *proof, char *out, size_t capacity)
{
    int n;
    if (!proof_fields_ok(proof) || !out) return -1;
    n = snprintf(out, capacity, "%s\n%s\n%s\n%s", proof->name, proof->room, proof->version, proof->host);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}

int le_peer_proof_json(const struct le_peer_key *key, const struct le_peer_proof *proof, const char *nonce, long long ts, char *out, size_t capacity)
{
    char body[256], mac[LE_PEER_MAC_HEX + 1], name[400], room[400], version[200], host[300];
    int n = le_peer_proof_body(proof, body, sizeof(body));
    if (n < 0 || !out || le_peer_mac(key, LE_PEER_PROOF_TYPE, ts, nonce, body, (size_t)n, mac)) return -1;
    json_escape(name, sizeof(name), proof->name); json_escape(room, sizeof(room), proof->room);
    json_escape(version, sizeof(version), proof->version); json_escape(host, sizeof(host), proof->host);
    n = snprintf(out, capacity, "{\"key_id\":\"%s\",\"type\":\"" LE_PEER_PROOF_TYPE "\",\"ts\":%lld,\"nonce\":\"%s\",\"name\":\"%s\",\"room\":\"%s\",\"version\":\"%s\",\"host\":\"%s\",\"mac\":\"%s\"}",
                 key->id, ts, nonce, name, room, version, host, mac);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}

int le_peer_proof_verify(const struct le_peer_key *key, int status, const char *body, const char *nonce, const char *host, struct le_peer_proof *proof)
{
    struct le_peer_proof got;
    char id[16], type[40], got_nonce[40], mac[80], code[40], signed_body[256];
    long long ts;
    int n;
    if (!key || !key->set) return LE_PEER_TRUST_OFF;
    if (!body || !nonce || !host) return LE_PEER_TRUST_FAILED;
    /* Older firmware without the endpoint: 404, or 401 when it has accounts
       (the endpoint is not exempt from its login). */
    if (status == 404 || status == 401) return LE_PEER_TRUST_UNPAIRED;
    if (status == 409 && json_get_string(body, "code", code, sizeof(code)) > 0 && !strcmp(code, "not_paired")) return LE_PEER_TRUST_UNPAIRED;
    if (status != 200 || !json_valid_object(body, strlen(body))) return LE_PEER_TRUST_FAILED;
    memset(&got, 0, sizeof(got));
    if (json_get_string(body, "key_id", id, sizeof(id)) <= 0) return LE_PEER_TRUST_FAILED;
    if (strcmp(id, key->id)) return LE_PEER_TRUST_UNPAIRED;
    if (json_get_string(body, "type", type, sizeof(type)) <= 0 || strcmp(type, LE_PEER_PROOF_TYPE) ||
        json_get_string(body, "nonce", got_nonce, sizeof(got_nonce)) <= 0 || strcmp(got_nonce, nonce) ||
        json_get_int64(body, "ts", &ts) != 1 || ts < 0 ||
        json_get_string(body, "name", got.name, sizeof(got.name)) <= 0 ||
        json_get_string(body, "room", got.room, sizeof(got.room)) != 1 ||
        json_get_string(body, "version", got.version, sizeof(got.version)) != 1 ||
        json_get_string(body, "host", got.host, sizeof(got.host)) <= 0 || strcmp(got.host, host) ||
        json_get_string(body, "mac", mac, sizeof(mac)) <= 0)
        return LE_PEER_TRUST_FAILED;
    n = le_peer_proof_body(&got, signed_body, sizeof(signed_body));
    if (n < 0 || le_peer_check(key, LE_PEER_PROOF_TYPE, ts, nonce, signed_body, (size_t)n, mac) != LE_PEER_OK) return LE_PEER_TRUST_FAILED;
    if (proof) *proof = got;
    return LE_PEER_TRUST_PAIRED;
}

static long long now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}

struct probe {
    int fd, sent;
    size_t used;
    char host[48], nonce[LE_PEER_NONCE_HEX + 1];
    char buf[LE_PEER_RESPONSE_MAX + 1];
};

static void probe_finish(const struct le_peer_key *key, struct probe *p, int *trust)
{
    char *body;
    int status = 0;
    p->buf[p->used] = 0;
    body = strstr(p->buf, "\r\n\r\n");
    if (body && sscanf(p->buf, "HTTP/1.%*1[01] %3d", &status) == 1) *trust = le_peer_proof_verify(key, status, body + 4, p->nonce, p->host, NULL);
    else *trust = LE_PEER_TRUST_FAILED;
}

void le_peer_verify_all(const struct le_peer_key *key, const struct le_peer *list, size_t count, const int *skip, int timeout_ms, int *trust)
{
    static struct probe probes[LE_PEERS_MAX];
    struct pollfd fds[LE_PEERS_MAX];
    size_t map[LE_PEERS_MAX], i, open_count = 0;
    long long deadline;
    if (!trust) return;
    if (count > LE_PEERS_MAX) count = LE_PEERS_MAX;
    for (i = 0; i < count; i++) trust[i] = !key || !key->set ? LE_PEER_TRUST_OFF : skip && skip[i] ? LE_PEER_TRUST_SELF : LE_PEER_TRUST_FAILED;
    if (!key || !key->set || !list) return;
    deadline = now_ms();
    if (deadline < 0) return;
    deadline += timeout_ms > 0 ? timeout_ms : LE_PEER_VERIFY_MS;
    for (i = 0; i < count; i++) {
        struct sockaddr_in a;
        struct probe *p = &probes[i];
        p->fd = -1; p->sent = 0; p->used = 0;
        if (skip && skip[i]) continue;
        memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons((unsigned short)list[i].port);
        if (!list[i].port || list[i].port > 65535 || inet_pton(AF_INET, list[i].address, &a.sin_addr) != 1) continue;
        snprintf(p->host, sizeof(p->host), "%s:%u", list[i].address, list[i].port);
        if (le_peer_nonce(p->nonce)) continue;
        p->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (p->fd < 0) continue;
        if (connect(p->fd, (const struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) { close(p->fd); p->fd = -1; continue; }
    }
    for (;;) {
        long long left = deadline - now_ms();
        int rc;
        open_count = 0;
        for (i = 0; i < count; i++) if (probes[i].fd >= 0 && !(skip && skip[i])) { fds[open_count].fd = probes[i].fd; fds[open_count].events = probes[i].sent ? POLLIN : POLLOUT; fds[open_count].revents = 0; map[open_count++] = i; }
        if (!open_count || left <= 0) break;
        rc = poll(fds, (nfds_t)open_count, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) break;
        for (i = 0; i < open_count; i++) {
            struct probe *p = &probes[map[i]];
            if (!fds[i].revents) continue;
            if (!p->sent) {
                char req[256];
                int err = 0, n;
                socklen_t len = sizeof(err);
                if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &len) || err) { close(p->fd); p->fd = -1; continue; }
                n = snprintf(req, sizeof(req), "GET /api/v1/peer/proof?nonce=%s HTTP/1.1\r\nHost: %s\r\nAccept: application/json\r\nConnection: close\r\n\r\n", p->nonce, p->host);
                if (n < 0 || (size_t)n >= sizeof(req) || send(p->fd, req, (size_t)n, MSG_NOSIGNAL) != (ssize_t)n) { close(p->fd); p->fd = -1; continue; }
                p->sent = 1;
            } else {
                ssize_t n = recv(p->fd, p->buf + p->used, LE_PEER_RESPONSE_MAX - p->used, 0);
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                if (n > 0) p->used += (size_t)n;
                /* An answer larger than the buffer is not a proof. */
                if (n > 0 && p->used >= LE_PEER_RESPONSE_MAX) { close(p->fd); p->fd = -1; continue; }
                if (n <= 0) { if (n == 0) probe_finish(key, p, &trust[map[i]]); close(p->fd); p->fd = -1; }
            }
        }
    }
    for (i = 0; i < count; i++) if (probes[i].fd >= 0) { close(probes[i].fd); probes[i].fd = -1; }
}
