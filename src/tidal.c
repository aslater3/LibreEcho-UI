/*
 * TIDAL client. See tidal.h for the scope and the trust decisions.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "tidal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "json.h"

/*
 * The client identity of TIDAL's own device-code apps, as used by the
 * open-source TIDAL clients (tidalapi, tiddl, Music Assistant). TIDAL offers
 * no device-code client to third parties; this is the same method those
 * projects use, and it can stop working if TIDAL rotates the key. The service
 * environment can substitute another pair with LIBREECHO_TIDAL_CLIENT_ID and
 * LIBREECHO_TIDAL_CLIENT_SECRET without a rebuild.
 */
#define TIDAL_CLIENT_ID "4N3n6Q1x95LL5K7p"
#define TIDAL_CLIENT_SECRET "oKOXfJW371cX6xaZ0PyhgGNBdNLlBZd4AKKYougMjik="
#define TIDAL_SCOPE "r_usr+w_usr+w_sub"
#define TIDAL_DEVICE_GRANT "urn:ietf:params:oauth:grant-type:device_code"

#define RESPONSE_MAX (768 * 1024)
#define REFRESH_MARGIN 300      /* refresh when under five minutes remain */
#define TOKEN_MAX 4096

struct span {
    const char *p;
    size_t n;
};

/* ------------------------------------------------------------------ paths */

static void env_copy(char *out, size_t size, const char *name,
                     const char *fallback)
{
    const char *value = getenv(name);

    snprintf(out, size, "%s", value && *value ? value : fallback);
}

static const char *first_executable(const char *const *candidates)
{
    size_t i;

    for (i = 0; candidates[i]; ++i)
        if (!access(candidates[i], X_OK))
            return candidates[i];
    return candidates[0];
}

static const char *first_readable(const char *const *candidates,
                                  const char *fallback)
{
    size_t i;

    for (i = 0; candidates[i]; ++i)
        if (!access(candidates[i], R_OK))
            return candidates[i];
    return fallback;
}

void le_tidal_default_paths(struct le_tidal_paths *paths)
{
    static const char *const curls[] = {
        "/usr/bin/curl", "/usr/local/libexec/libreecho-curl", NULL
    };
    /* An owner-added private CA bundle already includes the system roots. */
    static const char *const cas[] = {
        "/data/libreecho/config/trust-bundle.pem",
        "/etc/ssl/certs/ca-certificates.crt",
        "/usr/local/share/libreecho/cacert.pem", NULL
    };

    memset(paths, 0, sizeof(*paths));
    env_copy(paths->curl, sizeof(paths->curl), "LIBREECHO_TIDAL_CURL",
             first_executable(curls));
    env_copy(paths->ca, sizeof(paths->ca), "LIBREECHO_TIDAL_CA",
             first_readable(cas, "/etc/ssl/certs/ca-certificates.crt"));
    env_copy(paths->session, sizeof(paths->session), "LIBREECHO_TIDAL_SESSION",
             "/data/libreecho/secrets/tidal.json");
    env_copy(paths->login, sizeof(paths->login), "LIBREECHO_TIDAL_LOGIN",
             "/run/libreecho/tidal-login.json");
    env_copy(paths->queue, sizeof(paths->queue), "LIBREECHO_TIDAL_QUEUE",
             "/run/libreecho/tidal-queue");
    env_copy(paths->auth_base, sizeof(paths->auth_base),
             "LIBREECHO_TIDAL_AUTH_BASE", "https://auth.tidal.com/v1/oauth2");
    env_copy(paths->api_base, sizeof(paths->api_base),
             "LIBREECHO_TIDAL_API_BASE", "https://api.tidal.com/v1");
}

int le_tidal_available(const struct le_tidal_paths *paths)
{
    return paths && !access(paths->curl, X_OK) && !access(paths->ca, R_OK);
}

/* ------------------------------------------------------------------ files */

static int ensure_parent(const char *path)
{
    char dir[256];
    char *slash;

    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir))
        return -1;
    slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';
    if (mkdir(dir, 0700) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Temp file, fsync, rename: a crash leaves the old file or the new one. */
static int write_private(const char *path, const char *data, size_t length)
{
    char tmp[300];
    int fd;
    size_t done = 0;

    if (ensure_parent(path) < 0 ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;
    (void)unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        done += (size_t)n;
    }
    if (fsync(fd) < 0 || close(fd) < 0 || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static long read_file(const char *path, char *out, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    size_t used = 0;

    if (fd < 0 || size < 2) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    for (;;) {
        ssize_t n = read(fd, out + used, size - 1 - used);

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
        if (used == size - 1) {
            close(fd);
            return -1;              /* larger than any file this module writes */
        }
    }
    close(fd);
    out[used] = '\0';
    return (long)used;
}

/* Bounded copy that always terminates; truncation is acceptable for display. */
static void copy_text(char *out, size_t size, const char *in)
{
    size_t n = strlen(in);

    if (!size)
        return;
    if (n >= size)
        n = size - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;

    while (n--)
        *v++ = 0;
}

/* ------------------------------------------------------------------- json */

static int skip_value(const char *s, size_t n, size_t *i)
{
    int depth = 0;
    size_t start;

    while (*i < n && isspace((unsigned char)s[*i]))
        ++*i;
    start = *i;
    while (*i < n) {
        char c = s[*i];

        if (c == '"') {
            for (++*i; *i < n && s[*i] != '"'; ++*i)
                if (s[*i] == '\\')
                    ++*i;
            if (*i >= n)
                return -1;
            ++*i;
            if (!depth)
                return 0;
        } else if (c == '{' || c == '[') {
            ++depth;
            ++*i;
        } else if (c == '}' || c == ']') {
            if (!depth)
                return *i > start ? 0 : -1;   /* ends a bare scalar */
            ++*i;
            if (!--depth)
                return 0;
        } else if (!depth && (c == ',' || isspace((unsigned char)c))) {
            return *i > start ? 0 : -1;
        } else {
            ++*i;
        }
    }
    return depth || *i == start ? -1 : 0;
}

/* Walk an array span: first call with *pos = 0. */
static int array_next(struct span arr, size_t *pos, struct span *elem)
{
    size_t i = *pos, start;

    if (i == 0) {
        while (i < arr.n && isspace((unsigned char)arr.p[i]))
            ++i;
        if (i >= arr.n || arr.p[i] != '[')
            return 0;
        ++i;
    }
    while (i < arr.n && (isspace((unsigned char)arr.p[i]) || arr.p[i] == ','))
        ++i;
    if (i >= arr.n || arr.p[i] == ']')
        return 0;
    start = i;
    if (skip_value(arr.p, arr.n, &i) < 0)
        return 0;
    elem->p = arr.p + start;
    elem->n = i - start;
    *pos = i;
    return 1;
}

static int member(struct span obj, const char *name, struct span *out)
{
    struct json_member members[160];
    size_t count = 0, i, len = strlen(name);

    if (!obj.p || json_object_members(obj.p, obj.n, members, 160, &count) < 0)
        return 0;
    for (i = 0; i < count; ++i)
        if (members[i].name_len == len &&
            !memcmp(members[i].name, name, len)) {
            out->p = members[i].value;
            out->n = members[i].value_len;
            return 1;
        }
    return 0;
}

static int member_path(struct span obj, const char *a, const char *b,
                       struct span *out)
{
    struct span mid;

    return member(obj, a, &mid) && member(mid, b, out);
}

static int span_text(struct span v, char *out, size_t size)
{
    out[0] = '\0';
    return v.p && json_string_span(v.p, v.n, out, size) == 1;
}

static int member_text(struct span obj, const char *name, char *out,
                       size_t size)
{
    struct span v;

    out[0] = '\0';
    return member(obj, name, &v) && span_text(v, out, size);
}

static int member_int(struct span obj, const char *name, int *out)
{
    struct span v;

    return member(obj, name, &v) && json_int_span(v.p, v.n, out) == 1;
}

/* A numeric or string id, copied as text. */
static int member_id(struct span obj, const char *name, char *out,
                     size_t size)
{
    struct span v;
    size_t i = 0, j = 0;

    out[0] = '\0';
    if (!member(obj, name, &v))
        return 0;
    if (v.n && v.p[0] == '"')
        return span_text(v, out, size);
    for (; i < v.n && j + 1 < size; ++i) {
        if (!isdigit((unsigned char)v.p[i]))
            break;
        out[j++] = v.p[i];
    }
    out[j] = '\0';
    return j > 0;
}

/* ---------------------------------------------------------------- helpers */

int le_tidal_valid_id(enum le_tidal_kind kind, const char *id)
{
    size_t i, n = id ? strlen(id) : 0;

    if (!n || n >= LE_TIDAL_ID_MAX)
        return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)id[i];

        if (kind == LE_TIDAL_PLAYLIST ? !(isxdigit(c) || c == '-')
                                      : !isdigit(c))
            return 0;
    }
    return 1;
}

static void url_encode(char *out, size_t size, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;

    for (; *in && j + 4 < size; ++in) {
        unsigned char c = (unsigned char)*in;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = hex[c >> 4];
            out[j++] = hex[c & 15];
        }
    }
    out[j] = '\0';
}

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static long b64_decode(const char *in, char *out, size_t size)
{
    unsigned long acc = 0;
    int bits = 0;
    size_t j = 0;

    for (; *in && *in != '='; ++in) {
        int v = b64_value((unsigned char)*in);

        if (v < 0) {
            if (isspace((unsigned char)*in))
                continue;
            return -1;
        }
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (j + 1 >= size)
                return -1;
            out[j++] = (char)((acc >> bits) & 0xff);
        }
    }
    out[j] = '\0';
    return (long)j;
}

/* curl config values are quoted strings; refuse anything that would end one. */
static int config_safe(const char *value)
{
    for (; *value; ++value)
        if ((unsigned char)*value < 0x20 || *value == '"' || *value == '\\' ||
            *value == 0x7f)
            return 0;
    return 1;
}

static const char *client_id(void)
{
    const char *v = getenv("LIBREECHO_TIDAL_CLIENT_ID");

    return v && *v ? v : TIDAL_CLIENT_ID;
}

static const char *client_secret(void)
{
    const char *v = getenv("LIBREECHO_TIDAL_CLIENT_SECRET");

    return v && *v ? v : TIDAL_CLIENT_SECRET;
}

/* ------------------------------------------------------------------- http */

struct http_reply {
    int status;
    char *body;                 /* malloc'd, NUL-terminated */
    size_t length;
};

static int write_all(int fd, const char *data, size_t length)
{
    while (length) {
        ssize_t n = write(fd, data, length);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        data += n;
        length -= (size_t)n;
    }
    return 0;
}

/*
 * One request through curl. The config (URL, headers, credentials) goes on a
 * pipe that curl reads as a file, so no token is ever visible in /proc. A
 * form body, when there is one, goes on stdin.
 */
static int http(const struct le_tidal_paths *paths, const char *url,
                const char *bearer, int basic, const char *form,
                struct http_reply *reply)
{
    char config[TOKEN_MAX + 2048];
    int config_pipe[2] = {-1, -1}, body_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    pid_t child = -1;
    int status = 0, used, rc = LE_TIDAL_NETWORK, pipe_saved = 0;
    size_t cap = 65536;
    struct sigaction saved_pipe;
    char *nl;

    memset(reply, 0, sizeof(*reply));
    if (!config_safe(url) || (bearer && !config_safe(bearer)) ||
        !config_safe(paths->ca) || !config_safe(client_id()) ||
        !config_safe(client_secret()))
        return LE_TIDAL_ERR;
    used = snprintf(config, sizeof(config),
                    "url = \"%s\"\n"
                    "cacert = \"%s\"\n"
                    "silent\n"
                    "connect-timeout = 10\nmax-time = 25\n"
                    "proto = \"=https\"\nproto-redir = \"=https\"\n"
                    "user-agent = \"LibreEcho\"\n"
                    "header = \"Accept: application/json\"\n"
                    "write-out = \"\\n%%{http_code}\"\n",
                    url, paths->ca);
    if (used < 0 || (size_t)used >= sizeof(config))
        return LE_TIDAL_ERR;
    if (bearer && bearer[0]) {
        int n = snprintf(config + used, sizeof(config) - (size_t)used,
                         "header = \"Authorization: Bearer %s\"\n", bearer);

        if (n < 0 || (size_t)n >= sizeof(config) - (size_t)used)
            return LE_TIDAL_ERR;
        used += n;
    }
    if (basic) {
        int n = snprintf(config + used, sizeof(config) - (size_t)used,
                         "user = \"%s:%s\"\n", client_id(), client_secret());

        if (n < 0 || (size_t)n >= sizeof(config) - (size_t)used)
            return LE_TIDAL_ERR;
        used += n;
    }
    reply->body = malloc(cap);
    if (!reply->body)
        return LE_TIDAL_ERR;
    if (pipe(config_pipe) < 0 || pipe(body_pipe) < 0 || pipe(out_pipe) < 0)
        goto done;
    child = fork();
    if (child < 0)
        goto done;
    if (child == 0) {
        char config_path[64];
        int null_fd = open("/dev/null", O_WRONLY);

        signal(SIGPIPE, SIG_DFL);
        close(config_pipe[1]);
        close(body_pipe[1]);
        close(out_pipe[0]);
        if (dup2(body_pipe[0], STDIN_FILENO) < 0 ||
            dup2(out_pipe[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        snprintf(config_path, sizeof(config_path), "/proc/self/fd/%d",
                 config_pipe[0]);
        if (form)
            execl(paths->curl, paths->curl, "--config", config_path,
                  "--data-binary", "@-", (char *)NULL);
        else
            execl(paths->curl, paths->curl, "--config", config_path,
                  (char *)NULL);
        _exit(127);
    }
    close(config_pipe[0]);
    config_pipe[0] = -1;
    close(body_pipe[0]);
    body_pipe[0] = -1;
    close(out_pipe[1]);
    out_pipe[1] = -1;
    /* curl dying early must fail this request, not kill the caller. */
    {
        struct sigaction ignore;

        memset(&ignore, 0, sizeof(ignore));
        ignore.sa_handler = SIG_IGN;
        (void)sigaction(SIGPIPE, &ignore, &saved_pipe);
        pipe_saved = 1;
    }
    if (write_all(config_pipe[1], config, strlen(config)) < 0)
        goto done;
    close(config_pipe[1]);
    config_pipe[1] = -1;
    if (form && write_all(body_pipe[1], form, strlen(form)) < 0)
        goto done;
    close(body_pipe[1]);
    body_pipe[1] = -1;
    for (;;) {
        ssize_t n;

        if (reply->length + 1 >= cap) {
            char *grown;

            if (cap >= RESPONSE_MAX)
                goto done;
            cap *= 2;
            grown = realloc(reply->body, cap);
            if (!grown)
                goto done;
            reply->body = grown;
        }
        n = read(out_pipe[0], reply->body + reply->length,
                 cap - 1 - reply->length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto done;
        if (n == 0)
            break;
        reply->length += (size_t)n;
    }
    reply->body[reply->length] = '\0';
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            goto done;
    child = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        goto done;
    nl = strrchr(reply->body, '\n');
    if (!nl)
        goto done;
    reply->status = atoi(nl + 1);
    *nl = '\0';
    reply->length = (size_t)(nl - reply->body);
    if (reply->status < 100 || reply->status > 599)
        goto done;
    rc = LE_TIDAL_OK;
done:
    if (config_pipe[0] >= 0) close(config_pipe[0]);
    if (config_pipe[1] >= 0) close(config_pipe[1]);
    if (body_pipe[0] >= 0) close(body_pipe[0]);
    if (body_pipe[1] >= 0) close(body_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (child > 0) {
        kill(child, SIGTERM);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    if (pipe_saved)
        (void)sigaction(SIGPIPE, &saved_pipe, NULL);
    wipe(config, sizeof(config));
    if (rc != LE_TIDAL_OK) {
        free(reply->body);
        reply->body = NULL;
        reply->length = 0;
    }
    return rc;
}

static void reply_free(struct http_reply *reply)
{
    if (reply->body) {
        wipe(reply->body, reply->length);
        free(reply->body);
    }
    reply->body = NULL;
}

/* ---------------------------------------------------------------- session */

struct session {
    char access[TOKEN_MAX];
    char refresh[TOKEN_MAX];
    long long expires_at;
    char country[8];
    char username[LE_TIDAL_TEXT_MAX];
};

static int session_load(const struct le_tidal_paths *paths, struct session *s)
{
    char text[3 * TOKEN_MAX];
    long long expires = 0;
    int rc;

    memset(s, 0, sizeof(*s));
    if (read_file(paths->session, text, sizeof(text)) <= 0)
        return LE_TIDAL_SIGNED_OUT;
    rc = json_get_string(text, "refresh_token", s->refresh,
                         sizeof(s->refresh)) == 1 && s->refresh[0];
    (void)json_get_string(text, "access_token", s->access, sizeof(s->access));
    (void)json_get_string(text, "country", s->country, sizeof(s->country));
    (void)json_get_string(text, "username", s->username, sizeof(s->username));
    if (json_get_int64(text, "expires_at", &expires) == 1)
        s->expires_at = expires;
    wipe(text, sizeof(text));
    return rc ? LE_TIDAL_OK : LE_TIDAL_SIGNED_OUT;
}

static int session_save(const struct le_tidal_paths *paths,
                        const struct session *s)
{
    char text[3 * TOKEN_MAX], user[LE_TIDAL_TEXT_MAX * 2],
        country[16];
    int n, rc;

    if (!config_safe(s->access) || !config_safe(s->refresh))
        return LE_TIDAL_ERR;
    json_escape(user, sizeof(user), s->username);
    json_escape(country, sizeof(country), s->country);
    n = snprintf(text, sizeof(text),
                 "{\"schema\":1,\"access_token\":\"%s\",\"refresh_token\":\"%s\","
                 "\"expires_at\":%lld,\"country\":\"%s\",\"username\":\"%s\"}\n",
                 s->access, s->refresh, s->expires_at, country, user);
    if (n < 0 || (size_t)n >= sizeof(text))
        return LE_TIDAL_ERR;
    rc = write_private(paths->session, text, (size_t)n) ? LE_TIDAL_ERR
                                                         : LE_TIDAL_OK;
    wipe(text, sizeof(text));
    return rc;
}

/* Take tokens and account details from a token-endpoint reply. */
static int session_from_token(struct session *s, const char *json,
                              size_t length)
{
    struct span doc = {json, length}, v;
    char refresh[TOKEN_MAX];
    int expires_in = 0;

    if (!member_text(doc, "access_token", s->access, sizeof(s->access)) ||
        !s->access[0])
        return LE_TIDAL_NETWORK;
    /* A refresh reply keeps the old refresh token unless it sends a new one. */
    if (member_text(doc, "refresh_token", refresh, sizeof(refresh)) &&
        refresh[0])
        memcpy(s->refresh, refresh, sizeof(s->refresh));
    wipe(refresh, sizeof(refresh));
    if (!s->refresh[0])
        return LE_TIDAL_NETWORK;
    if (!member_int(doc, "expires_in", &expires_in) || expires_in <= 0)
        expires_in = 3600;
    s->expires_at = (long long)time(NULL) + expires_in;
    if (member(doc, "user", &v)) {
        char text[LE_TIDAL_TEXT_MAX];

        if (member_text(v, "countryCode", text, sizeof(text)) && text[0])
            copy_text(s->country, sizeof(s->country), text);
        if (member_text(v, "username", text, sizeof(text)) && text[0])
            copy_text(s->username, sizeof(s->username), text);
        else if (member_text(v, "email", text, sizeof(text)) && text[0])
            copy_text(s->username, sizeof(s->username), text);
    }
    return LE_TIDAL_OK;
}

static int session_refresh(const struct le_tidal_paths *paths,
                           struct session *s)
{
    char url[256], form[TOKEN_MAX + 256];
    struct http_reply reply;
    int rc;

    snprintf(url, sizeof(url), "%s/token", paths->auth_base);
    if (snprintf(form, sizeof(form),
                 "client_id=%s&refresh_token=%s&grant_type=refresh_token"
                 "&scope=" TIDAL_SCOPE, client_id(), s->refresh) >=
        (int)sizeof(form))
        return LE_TIDAL_ERR;
    rc = http(paths, url, NULL, 1, form, &reply);
    wipe(form, sizeof(form));
    if (rc)
        return rc;
    if (reply.status == 400 || reply.status == 401 || reply.status == 403) {
        /* TIDAL refused the refresh token: the owner signed this device out
           or changed their password. Forget it so the UI offers sign-in. */
        reply_free(&reply);
        (void)unlink(paths->session);
        return LE_TIDAL_SIGNED_OUT;
    }
    rc = reply.status == 200 ? session_from_token(s, reply.body, reply.length)
                             : LE_TIDAL_NETWORK;
    reply_free(&reply);
    if (rc == LE_TIDAL_OK)
        rc = session_save(paths, s);
    return rc;
}

static int session_ready(const struct le_tidal_paths *paths, struct session *s)
{
    int rc = session_load(paths, s);

    if (rc)
        return rc;
    if (!s->access[0] ||
        s->expires_at - (long long)time(NULL) < REFRESH_MARGIN)
        rc = session_refresh(paths, s);
    if (!rc && !s->country[0])
        snprintf(s->country, sizeof(s->country), "US");
    return rc;
}

/* GET an API path with the session; one refresh-and-retry on 401. */
static int api_get(const struct le_tidal_paths *paths, const char *path_query,
                   struct http_reply *reply)
{
    struct session s;
    char url[LE_TIDAL_URL_MAX];
    int rc, attempt;

    rc = session_ready(paths, &s);
    for (attempt = 0; !rc && attempt < 2; ++attempt) {
        if (snprintf(url, sizeof(url), "%s/%s%scountryCode=%s",
                     paths->api_base, path_query,
                     strchr(path_query, '?') ? "&" : "?", s.country) >=
            (int)sizeof(url)) {
            rc = LE_TIDAL_INVALID;
            break;
        }
        rc = http(paths, url, s.access, 0, NULL, reply);
        if (rc)
            break;
        if (reply->status == 401 && attempt == 0) {
            reply_free(reply);
            rc = session_refresh(paths, &s);
            continue;
        }
        if (reply->status == 200)
            break;
        rc = reply->status == 404 ? LE_TIDAL_NOT_FOUND
           : reply->status == 401 ? LE_TIDAL_SIGNED_OUT : LE_TIDAL_NETWORK;
        reply_free(reply);
        break;
    }
    wipe(&s, sizeof(s));
    return rc;
}

/* ------------------------------------------------------------------ login */

int le_tidal_status(const struct le_tidal_paths *paths,
                    struct le_tidal_status *out)
{
    struct session s;
    char text[2048];
    long long expires = 0;
    int interval = 0;

    memset(out, 0, sizeof(*out));
    if (session_load(paths, &s) == LE_TIDAL_OK) {
        out->signed_in = 1;
        snprintf(out->country, sizeof(out->country), "%s", s.country);
        snprintf(out->username, sizeof(out->username), "%s", s.username);
    }
    wipe(&s, sizeof(s));
    if (!out->signed_in && read_file(paths->login, text, sizeof(text)) > 0) {
        if (json_get_int64(text, "expires_at", &expires) == 1 &&
            expires > (long long)time(NULL) &&
            json_get_string(text, "user_code", out->login.user_code,
                            sizeof(out->login.user_code)) == 1 &&
            json_get_string(text, "link", out->login.link,
                            sizeof(out->login.link)) == 1) {
            out->login_pending = 1;
            out->login.expires_at = expires;
            if (json_get_int(text, "interval", &interval) == 1)
                out->login.interval = interval;
        }
        wipe(text, sizeof(text));
    }
    return LE_TIDAL_OK;
}

int le_tidal_login_start(const struct le_tidal_paths *paths,
                         struct le_tidal_login *out)
{
    char url[256], form[256], device[512], text[2048], link[160];
    struct http_reply reply;
    struct span doc;
    int expires_in = 0, interval = 0, rc, n;

    memset(out, 0, sizeof(*out));
    snprintf(url, sizeof(url), "%s/device_authorization", paths->auth_base);
    snprintf(form, sizeof(form), "client_id=%s&scope=" TIDAL_SCOPE,
             client_id());
    rc = http(paths, url, NULL, 0, form, &reply);
    if (rc)
        return rc;
    doc.p = reply.body;
    doc.n = reply.length;
    if (reply.status != 200 ||
        !member_text(doc, "deviceCode", device, sizeof(device)) ||
        !member_text(doc, "userCode", out->user_code,
                     sizeof(out->user_code)) ||
        !config_safe(device) || !device[0] || !out->user_code[0]) {
        reply_free(&reply);
        return LE_TIDAL_NETWORK;
    }
    if (!member_text(doc, "verificationUriComplete", link, sizeof(link)) ||
        !link[0])
        snprintf(link, sizeof(link), "link.tidal.com/%s", out->user_code);
    if (!member_int(doc, "expiresIn", &expires_in) || expires_in <= 0)
        expires_in = 300;
    if (!member_int(doc, "interval", &interval) || interval <= 0)
        interval = 2;
    reply_free(&reply);
    {
        char full[sizeof(link) + 8];

        snprintf(full, sizeof(full), "%s%s",
                 strncmp(link, "http", 4) ? "https://" : "", link);
        copy_text(out->link, sizeof(out->link), full);
    }
    out->expires_at = (long long)time(NULL) + expires_in;
    out->interval = interval;
    {
        char code[64], esc_link[256];

        json_escape(code, sizeof(code), out->user_code);
        json_escape(esc_link, sizeof(esc_link), out->link);
        n = snprintf(text, sizeof(text),
                     "{\"schema\":1,\"device_code\":\"%s\",\"user_code\":\"%s\","
                     "\"link\":\"%s\",\"expires_at\":%lld,\"interval\":%d}\n",
                     device, code, esc_link, out->expires_at, interval);
    }
    wipe(device, sizeof(device));
    if (n < 0 || (size_t)n >= sizeof(text))
        return LE_TIDAL_ERR;
    rc = write_private(paths->login, text, (size_t)n) ? LE_TIDAL_ERR
                                                       : LE_TIDAL_OK;
    wipe(text, sizeof(text));
    return rc;
}

int le_tidal_login_poll(const struct le_tidal_paths *paths)
{
    char text[2048], device[512], url[256], form[768], error[64];
    struct http_reply reply;
    struct session s;
    long long expires = 0;
    int rc;

    if (read_file(paths->login, text, sizeof(text)) <= 0)
        return LE_TIDAL_EXPIRED;
    if (json_get_string(text, "device_code", device, sizeof(device)) != 1 ||
        json_get_int64(text, "expires_at", &expires) != 1) {
        wipe(text, sizeof(text));
        (void)unlink(paths->login);
        return LE_TIDAL_EXPIRED;
    }
    wipe(text, sizeof(text));
    if (expires <= (long long)time(NULL)) {
        (void)unlink(paths->login);
        return LE_TIDAL_EXPIRED;
    }
    snprintf(url, sizeof(url), "%s/token", paths->auth_base);
    snprintf(form, sizeof(form),
             "client_id=%s&device_code=%s&grant_type=" TIDAL_DEVICE_GRANT
             "&scope=" TIDAL_SCOPE, client_id(), device);
    wipe(device, sizeof(device));
    rc = http(paths, url, NULL, 1, form, &reply);
    wipe(form, sizeof(form));
    if (rc)
        return rc;
    if (reply.status == 200) {
        memset(&s, 0, sizeof(s));
        rc = session_from_token(&s, reply.body, reply.length);
        reply_free(&reply);
        if (!rc)
            rc = session_save(paths, &s);
        wipe(&s, sizeof(s));
        if (!rc)
            (void)unlink(paths->login);
        return rc;
    }
    error[0] = '\0';
    {
        struct span doc = {reply.body, reply.length};

        (void)member_text(doc, "error", error, sizeof(error));
    }
    reply_free(&reply);
    if (!strcmp(error, "authorization_pending") || !strcmp(error, "slow_down"))
        return LE_TIDAL_PENDING;
    if (error[0]) {
        (void)unlink(paths->login);
        return LE_TIDAL_EXPIRED;
    }
    return LE_TIDAL_NETWORK;
}

int le_tidal_logout(const struct le_tidal_paths *paths)
{
    int rc = LE_TIDAL_OK;

    if (unlink(paths->session) < 0 && errno != ENOENT)
        rc = LE_TIDAL_ERR;
    (void)unlink(paths->login);
    (void)unlink(paths->queue);
    return rc;
}

/* ---------------------------------------------------------------- catalog */

static void item_from_track(struct span t, struct le_tidal_item *item)
{
    struct span artists, first, album;
    size_t pos = 0;
    char version[LE_TIDAL_TEXT_MAX];

    memset(item, 0, sizeof(*item));
    item->kind = LE_TIDAL_TRACK;
    (void)member_id(t, "id", item->id, sizeof(item->id));
    (void)member_text(t, "title", item->title, sizeof(item->title));
    if (member_text(t, "version", version, sizeof(version)) && version[0] &&
        strlen(item->title) + strlen(version) + 4 < sizeof(item->title)) {
        strcat(item->title, " (");
        strcat(item->title, version);
        strcat(item->title, ")");
    }
    (void)member_int(t, "duration", &item->duration);
    if (member(t, "artists", &artists) && array_next(artists, &pos, &first))
        (void)member_text(first, "name", item->artist, sizeof(item->artist));
    if (!item->artist[0])
        if (!item->artist[0] && member_path(t, "artist", "name", &first))
        (void)span_text(first, item->artist, sizeof(item->artist));
    if (member(t, "album", &album)) {
        (void)member_text(album, "title", item->album, sizeof(item->album));
        (void)member_text(album, "cover", item->image, sizeof(item->image));
    }
}

static void item_from_album(struct span a, struct le_tidal_item *item)
{
    struct span artists, first;
    size_t pos = 0;

    memset(item, 0, sizeof(*item));
    item->kind = LE_TIDAL_ALBUM;
    (void)member_id(a, "id", item->id, sizeof(item->id));
    (void)member_text(a, "title", item->title, sizeof(item->title));
    (void)member_text(a, "cover", item->image, sizeof(item->image));
    (void)member_int(a, "numberOfTracks", &item->tracks);
    if (member(a, "artists", &artists) && array_next(artists, &pos, &first))
        (void)member_text(first, "name", item->artist, sizeof(item->artist));
}

static void item_from_playlist(struct span p, struct le_tidal_item *item)
{
    struct span creator;

    memset(item, 0, sizeof(*item));
    item->kind = LE_TIDAL_PLAYLIST;
    (void)member_id(p, "uuid", item->id, sizeof(item->id));
    (void)member_text(p, "title", item->title, sizeof(item->title));
    if (!member_text(p, "squareImage", item->image, sizeof(item->image)))
        (void)member_text(p, "image", item->image, sizeof(item->image));
    (void)member_int(p, "numberOfTracks", &item->tracks);
    if (member(p, "creator", &creator))
        (void)member_text(creator, "name", item->artist, sizeof(item->artist));
    if (!item->artist[0])
        snprintf(item->artist, sizeof(item->artist), "TIDAL");
}

static size_t collect(struct span doc, const char *section,
                      void (*convert)(struct span, struct le_tidal_item *),
                      struct le_tidal_item *out, enum le_tidal_kind kind)
{
    struct span items, elem;
    size_t pos = 0, count = 0;

    if (!member_path(doc, section, "items", &items))
        return 0;
    while (count < LE_TIDAL_RESULTS_MAX && array_next(items, &pos, &elem)) {
        convert(elem, &out[count]);
        if (le_tidal_valid_id(kind, out[count].id) && out[count].title[0])
            ++count;
    }
    return count;
}

int le_tidal_parse_search(const char *json, size_t len,
                          struct le_tidal_results *out)
{
    struct span doc = {json, len}, section;

    memset(out, 0, sizeof(*out));
    if (!member(doc, "tracks", &section) && !member(doc, "albums", &section) &&
        !member(doc, "playlists", &section))
        return LE_TIDAL_NETWORK;
    out->track_count = collect(doc, "tracks", item_from_track, out->tracks,
                               LE_TIDAL_TRACK);
    out->album_count = collect(doc, "albums", item_from_album, out->albums,
                               LE_TIDAL_ALBUM);
    out->playlist_count = collect(doc, "playlists", item_from_playlist,
                                  out->playlists, LE_TIDAL_PLAYLIST);
    return LE_TIDAL_OK;
}

int le_tidal_parse_track(const char *json, size_t len,
                         struct le_tidal_item *out)
{
    struct span doc = {json, len};

    item_from_track(doc, out);
    return le_tidal_valid_id(LE_TIDAL_TRACK, out->id) ? LE_TIDAL_OK
                                                      : LE_TIDAL_NETWORK;
}

int le_tidal_parse_track_list(const char *json, size_t len,
                              char ids[][LE_TIDAL_ID_MAX], size_t capacity,
                              size_t *count)
{
    struct span doc = {json, len}, items, elem, inner;
    size_t pos = 0;

    *count = 0;
    if (!member(doc, "items", &items))
        return LE_TIDAL_NETWORK;
    while (*count < capacity && array_next(items, &pos, &elem)) {
        char id[LE_TIDAL_ID_MAX], type[16];
        int ready = 1;
        struct span v;

        /* /playlists/{id}/items wraps each entry; /tracks does not. */
        if (member(elem, "item", &inner)) {
            if (member_text(elem, "type", type, sizeof(type)) &&
                strcmp(type, "track"))
                continue;               /* videos are not audio */
            elem = inner;
        }
        if (member(elem, "streamReady", &v))
            (void)json_bool_span(v.p, v.n, &ready);
        if (!ready)
            continue;
        if (member_id(elem, "id", id, sizeof(id)) &&
            le_tidal_valid_id(LE_TIDAL_TRACK, id))
            snprintf(ids[(*count)++], LE_TIDAL_ID_MAX, "%s", id);
    }
    return *count ? LE_TIDAL_OK : LE_TIDAL_NOT_FOUND;
}

int le_tidal_parse_playback(const char *json, size_t len, char *url,
                            size_t url_size)
{
    struct span doc = {json, len}, urls, first, mdoc;
    char mime[64], codec[32], encryption[32], *manifest, *decoded;
    size_t pos = 0;
    long decoded_len;
    int rc = LE_TIDAL_UNSUPPORTED;

    url[0] = '\0';
    if (!member_text(doc, "manifestMimeType", mime, sizeof(mime)))
        return LE_TIDAL_NETWORK;
    if (strcmp(mime, "application/vnd.tidal.bts"))
        return LE_TIDAL_UNSUPPORTED;    /* DASH (hi-res) is not handled */
    manifest = malloc(len + 1);
    decoded = malloc(len + 1);
    if (!manifest || !decoded) {
        free(manifest);
        free(decoded);
        return LE_TIDAL_ERR;
    }
    if (!member_text(doc, "manifest", manifest, len + 1) ||
        (decoded_len = b64_decode(manifest, decoded, len + 1)) <= 0) {
        rc = LE_TIDAL_NETWORK;
        goto done;
    }
    mdoc.p = decoded;
    mdoc.n = (size_t)decoded_len;
    if (!member_text(mdoc, "encryptionType", encryption, sizeof(encryption)) ||
        strcmp(encryption, "NONE"))
        goto done;                      /* never touch protected streams */
    if (!member_text(mdoc, "codecs", codec, sizeof(codec)) ||
        strcasecmp(codec, "flac"))
        goto done;                      /* AAC arrives in MP4: not decodable */
    if (!member(mdoc, "urls", &urls) || !array_next(urls, &pos, &first) ||
        !span_text(first, url, url_size) || strncmp(url, "https://", 8)) {
        url[0] = '\0';
        rc = LE_TIDAL_NETWORK;
        goto done;
    }
    rc = LE_TIDAL_OK;
done:
    free(manifest);
    free(decoded);
    return rc;
}

int le_tidal_search(const struct le_tidal_paths *paths, const char *query,
                    struct le_tidal_results *out)
{
    char encoded[512], path[700];
    struct http_reply reply;
    int rc;

    memset(out, 0, sizeof(*out));
    if (!query || !query[0] || strlen(query) > 120)
        return LE_TIDAL_INVALID;
    url_encode(encoded, sizeof(encoded), query);
    snprintf(path, sizeof(path),
             "search?query=%s&types=TRACKS,ALBUMS,PLAYLISTS&limit=%d",
             encoded, LE_TIDAL_RESULTS_MAX);
    rc = api_get(paths, path, &reply);
    if (rc)
        return rc;
    rc = le_tidal_parse_search(reply.body, reply.length, out);
    reply_free(&reply);
    return rc;
}

int le_tidal_track_ids(const struct le_tidal_paths *paths,
                       enum le_tidal_kind kind, const char *id,
                       char ids[][LE_TIDAL_ID_MAX], size_t capacity,
                       size_t *count)
{
    char path[160];
    struct http_reply reply;
    int rc;

    *count = 0;
    if (!le_tidal_valid_id(kind, id) || !capacity)
        return LE_TIDAL_INVALID;
    if (kind == LE_TIDAL_TRACK) {
        snprintf(ids[0], LE_TIDAL_ID_MAX, "%s", id);
        *count = 1;
        return LE_TIDAL_OK;
    }
    snprintf(path, sizeof(path), "%s/%s/tracks?limit=%u",
             kind == LE_TIDAL_ALBUM ? "albums" : "playlists", id,
             (unsigned)(capacity > 100 ? 100 : capacity));
    rc = api_get(paths, path, &reply);
    if (rc)
        return rc;
    rc = le_tidal_parse_track_list(reply.body, reply.length, ids, capacity,
                                   count);
    reply_free(&reply);
    return rc;
}

int le_tidal_stream(const struct le_tidal_paths *paths, const char *track_id,
                    char *url, size_t url_size, struct le_tidal_item *meta)
{
    char path[200];
    struct http_reply reply;
    int rc;

    url[0] = '\0';
    if (!le_tidal_valid_id(LE_TIDAL_TRACK, track_id))
        return LE_TIDAL_INVALID;
    if (meta) {
        memset(meta, 0, sizeof(*meta));
        snprintf(path, sizeof(path), "tracks/%s", track_id);
        if (api_get(paths, path, &reply) == LE_TIDAL_OK) {
            (void)le_tidal_parse_track(reply.body, reply.length, meta);
            reply_free(&reply);
        }
    }
    snprintf(path, sizeof(path),
             "tracks/%s/playbackinfopostpaywall?audioquality=LOSSLESS"
             "&playbackmode=STREAM&assetpresentation=FULL", track_id);
    rc = api_get(paths, path, &reply);
    if (rc)
        return rc;
    rc = le_tidal_parse_playback(reply.body, reply.length, url, url_size);
    reply_free(&reply);
    return rc;
}

/* ------------------------------------------------------------------ media */

int le_tidal_media_open(const struct le_tidal_paths *paths, const char *url,
                        long long offset, int *fd, int *pid)
{
    char config[LE_TIDAL_URL_MAX + 1024];
    int config_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, used;
    pid_t child;
    struct sigaction ignore, saved;

    *fd = -1;
    *pid = -1;
    if (!url || strncmp(url, "https://", 8) || !config_safe(url) ||
        !config_safe(paths->ca) || offset < 0)
        return LE_TIDAL_INVALID;
    /*
     * No overall time limit: a track streams for as long as it plays, paced
     * by the decoder through the pipe. A stalled server is caught by the
     * speed floor instead (under 1 KB/s for 30 s). Pausing the player stops
     * the reader but not curl, so a long pause trips the same floor; the
     * caller then sees a broken transfer and resumes from its byte offset,
     * exactly as it does after a network drop.
     */
    used = snprintf(config, sizeof(config),
                    "url = \"%s\"\n"
                    "cacert = \"%s\"\n"
                    "silent\nfail\n"
                    "connect-timeout = 10\n"
                    "speed-limit = 1024\nspeed-time = 30\n"
                    "proto = \"=https\"\nproto-redir = \"=https\"\n"
                    "location\nmax-redirs = 3\n"
                    "user-agent = \"LibreEcho\"\n"
                    "range = \"%lld-\"\n",
                    url, paths->ca, offset);
    if (used < 0 || (size_t)used >= sizeof(config))
        return LE_TIDAL_ERR;
    if (pipe(config_pipe) < 0)
        return LE_TIDAL_ERR;
    if (pipe(out_pipe) < 0) {
        close(config_pipe[0]);
        close(config_pipe[1]);
        return LE_TIDAL_ERR;
    }
    child = fork();
    if (child < 0) {
        close(config_pipe[0]);
        close(config_pipe[1]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        return LE_TIDAL_ERR;
    }
    if (child == 0) {
        char config_path[64];
        int null_fd = open("/dev/null", O_RDWR);

        signal(SIGPIPE, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        close(config_pipe[1]);
        close(out_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0)
            _exit(126);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO)
                close(null_fd);
        }
        snprintf(config_path, sizeof(config_path), "/proc/self/fd/%d",
                 config_pipe[0]);
        execl(paths->curl, paths->curl, "--config", config_path,
              (char *)NULL);
        _exit(127);
    }
    close(config_pipe[0]);
    close(out_pipe[1]);
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &ignore, &saved);
    used = write_all(config_pipe[1], config, strlen(config));
    (void)sigaction(SIGPIPE, &saved, NULL);
    close(config_pipe[1]);
    wipe(config, sizeof(config));
    if (used < 0) {
        le_tidal_media_close(out_pipe[0], child, 0);
        return LE_TIDAL_NETWORK;
    }
    *fd = out_pipe[0];
    *pid = (int)child;
    return LE_TIDAL_OK;
}

int le_tidal_media_close(int fd, int pid, int at_eof)
{
    int status = 0, tries;
    pid_t done = 0;

    if (fd >= 0)
        close(fd);                      /* a curl still writing gets EPIPE */
    if (pid <= 0)
        return LE_TIDAL_ERR;
    /*
     * After EOF curl is already on its way out but may not have exited yet;
     * give it up to five seconds to report how the transfer ended. Before
     * EOF the caller is abandoning the transfer, so it is stopped at once.
     */
    for (tries = at_eof ? 100 : 1; tries > 0; --tries) {
        do
            done = waitpid((pid_t)pid, &status, WNOHANG);
        while (done < 0 && errno == EINTR);
        if (done != 0)
            break;
        if (tries > 1) {
            struct timespec ts = {0, 50L * 1000000L};

            while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
                ;
        }
    }
    if (done == 0) {
        kill((pid_t)pid, SIGTERM);
        while (waitpid((pid_t)pid, &status, 0) < 0 && errno == EINTR)
            ;
        return LE_TIDAL_NETWORK;
    }
    if (done < 0)
        return LE_TIDAL_ERR;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? LE_TIDAL_OK
                                                         : LE_TIDAL_NETWORK;
}

/* ------------------------------------------------------------------ queue */

int le_tidal_queue_write(const struct le_tidal_paths *paths,
                         char ids[][LE_TIDAL_ID_MAX], size_t count)
{
    char text[LE_TIDAL_QUEUE_MAX * LE_TIDAL_ID_MAX];
    size_t used = 0, i;

    if (!count || count > LE_TIDAL_QUEUE_MAX)
        return LE_TIDAL_INVALID;
    for (i = 0; i < count; ++i) {
        size_t n = strlen(ids[i]);

        if (!le_tidal_valid_id(LE_TIDAL_TRACK, ids[i]) ||
            used + n + 2 > sizeof(text))
            return LE_TIDAL_INVALID;
        memcpy(text + used, ids[i], n);
        used += n;
        text[used++] = '\n';
    }
    return write_private(paths->queue, text, used) ? LE_TIDAL_ERR
                                                    : LE_TIDAL_OK;
}

int le_tidal_queue_read(const struct le_tidal_paths *paths,
                        char ids[][LE_TIDAL_ID_MAX], size_t capacity,
                        size_t *count)
{
    char text[LE_TIDAL_QUEUE_MAX * LE_TIDAL_ID_MAX + 2], *line, *save = NULL;

    *count = 0;
    if (read_file(paths->queue, text, sizeof(text)) <= 0)
        return LE_TIDAL_NOT_FOUND;
    for (line = strtok_r(text, "\n", &save); line && *count < capacity;
         line = strtok_r(NULL, "\n", &save))
        if (le_tidal_valid_id(LE_TIDAL_TRACK, line))
            snprintf(ids[(*count)++], LE_TIDAL_ID_MAX, "%s", line);
    return *count ? LE_TIDAL_OK : LE_TIDAL_NOT_FOUND;
}
