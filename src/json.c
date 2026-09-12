#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

struct json_cursor { const char *s; size_t n, i; int depth; };

static int ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void skip_ws(struct json_cursor *c)
{
    while (c->i < c->n && ws(c->s[c->i])) c->i++;
}

static int hex(char c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int parse_string(struct json_cursor *c)
{
    if (c->i >= c->n || c->s[c->i++] != '"') return 0;
    while (c->i < c->n) {
        unsigned char ch = (unsigned char)c->s[c->i++];
        if (ch == '"') return 1;
        if (ch < 0x20) return 0;
        if (ch != '\\') continue;
        if (c->i >= c->n) return 0;
        ch = (unsigned char)c->s[c->i++];
        if (ch == 'u') {
            int j;
            for (j = 0; j < 4; j++)
                if (c->i >= c->n || !hex(c->s[c->i++])) return 0;
        } else if (ch != '"' && ch != '\\' && ch != '/' &&
                   ch != 'b' && ch != 'f' && ch != 'n' &&
                   ch != 'r' && ch != 't') {
            return 0;
        }
    }
    return 0;
}

static int literal(struct json_cursor *c, const char *value)
{
    size_t n = strlen(value);
    if (c->i + n > c->n || memcmp(c->s + c->i, value, n)) return 0;
    c->i += n;
    return 1;
}

static int parse_number(struct json_cursor *c)
{
    size_t start = c->i;
    if (c->i < c->n && c->s[c->i] == '-') c->i++;
    if (c->i >= c->n) return 0;
    if (c->s[c->i] == '0') {
        c->i++;
        if (c->i < c->n && isdigit((unsigned char)c->s[c->i])) return 0;
    } else {
        if (!isdigit((unsigned char)c->s[c->i])) return 0;
        while (c->i < c->n && isdigit((unsigned char)c->s[c->i])) c->i++;
    }
    if (c->i < c->n && c->s[c->i] == '.') {
        c->i++;
        if (c->i >= c->n || !isdigit((unsigned char)c->s[c->i])) return 0;
        while (c->i < c->n && isdigit((unsigned char)c->s[c->i])) c->i++;
    }
    if (c->i < c->n && (c->s[c->i] == 'e' || c->s[c->i] == 'E')) {
        c->i++;
        if (c->i < c->n && (c->s[c->i] == '+' || c->s[c->i] == '-')) c->i++;
        if (c->i >= c->n || !isdigit((unsigned char)c->s[c->i])) return 0;
        while (c->i < c->n && isdigit((unsigned char)c->s[c->i])) c->i++;
    }
    return c->i > start;
}

static int parse_value(struct json_cursor *c);

static int parse_object(struct json_cursor *c)
{
    if (c->i >= c->n || c->s[c->i++] != '{' || ++c->depth > JSON_MAX_DEPTH) return 0;
    skip_ws(c);
    if (c->i < c->n && c->s[c->i] == '}') { c->i++; c->depth--; return 1; }
    for (;;) {
        if (!parse_string(c)) return 0;
        skip_ws(c);
        if (c->i >= c->n || c->s[c->i++] != ':') return 0;
        if (!parse_value(c)) return 0;
        skip_ws(c);
        if (c->i < c->n && c->s[c->i] == '}') { c->i++; c->depth--; return 1; }
        if (c->i >= c->n || c->s[c->i++] != ',') return 0;
        skip_ws(c);
        if (c->i >= c->n || c->s[c->i] == '}') return 0;
    }
}

static int parse_array(struct json_cursor *c)
{
    if (c->i >= c->n || c->s[c->i++] != '[' || ++c->depth > JSON_MAX_DEPTH) return 0;
    skip_ws(c);
    if (c->i < c->n && c->s[c->i] == ']') { c->i++; c->depth--; return 1; }
    for (;;) {
        if (!parse_value(c)) return 0;
        skip_ws(c);
        if (c->i < c->n && c->s[c->i] == ']') { c->i++; c->depth--; return 1; }
        if (c->i >= c->n || c->s[c->i++] != ',') return 0;
        skip_ws(c);
        if (c->i >= c->n || c->s[c->i] == ']') return 0;
    }
}

static int parse_value(struct json_cursor *c)
{
    skip_ws(c);
    if (c->i >= c->n) return 0;
    switch (c->s[c->i]) {
    case '"': return parse_string(c);
    case '{': return parse_object(c);
    case '[': return parse_array(c);
    case 't': return literal(c, "true");
    case 'f': return literal(c, "false");
    case 'n': return literal(c, "null");
    default: return parse_number(c);
    }
}

int json_valid_object(const char *s, size_t n)
{
    struct json_cursor c = {s, n, 0, 0};
    if (!s) return 0;
    skip_ws(&c);
    if (!parse_object(&c)) return 0;
    skip_ws(&c);
    return c.i == c.n;
}

int json_duplicate_key(const char *s, size_t n, const char *key)
{
    size_t i = 0, key_len, count = 0;
    int depth = 0;
    if (!s || !key) return 0;
    key_len = strlen(key);
    while (i < n) {
        if (s[i] == '"') {
            size_t start = ++i;
            int escaped = 0;
            while (i < n) {
                char ch = s[i++];
                if (escaped) { escaped = 0; continue; }
                if (ch == '\\') { escaped = 1; continue; }
                if (ch == '"') break;
            }
            if (i > n || !i || s[i - 1] != '"') return 0;
            if (depth == 1 && i - start - 1 == key_len &&
                !memcmp(s + start, key, key_len)) {
                size_t j = i;
                while (j < n && ws(s[j])) j++;
                if (j < n && s[j] == ':' && ++count > 1) return 1;
            }
            continue;
        }
        if (s[i] == '{' || s[i] == '[') depth++;
        else if ((s[i] == '}' || s[i] == ']') && depth > 0) depth--;
        i++;
    }
    return 0;
}

static const char *find_key(const char *s, const char *k)
{
    char key[96];
    snprintf(key, sizeof(key), "\"%s\"", k);
    s = strstr(s, key);
    if (!s) return 0;
    s += strlen(key);
    while (isspace((unsigned char)*s)) s++;
    if (*s++ != ':') return 0;
    while (isspace((unsigned char)*s)) s++;
    return s;
}

/* Return a value for a key belonging to the document's outer object. */
static const char *find_top_level_key(const char *s, const char *k)
{
    struct json_cursor c;
    size_t key_len;

    if (!s || !k)
        return NULL;
    key_len = strlen(k);
    c.s = s;
    c.n = strlen(s);
    c.i = 0;
    c.depth = 0;
    skip_ws(&c);
    if (c.i >= c.n || c.s[c.i++] != '{')
        return NULL;
    for (;;) {
        size_t start, end;
        const char *value;

        skip_ws(&c);
        if (c.i >= c.n || c.s[c.i] == '}')
            return NULL;
        if (c.s[c.i] != '"')
            return NULL;
        start = c.i + 1;
        if (!parse_string(&c))
            return NULL;
        end = c.i - 1;
        skip_ws(&c);
        if (c.i >= c.n || c.s[c.i++] != ':')
            return NULL;
        skip_ws(&c);
        value = c.s + c.i;
        if (end - start == key_len && !memcmp(c.s + start, k, key_len))
            return value;
        if (!parse_value(&c))
            return NULL;
        skip_ws(&c);
        if (c.i < c.n && c.s[c.i] == '}')
            return NULL;
        if (c.i >= c.n || c.s[c.i++] != ',')
            return NULL;
    }
}

int json_get_int(const char *s, const char *k, int *out)
{
    char *e; long v; const char *p = find_key(s, k);
    if (!p) return 0;
    v = strtol(p, &e, 10);
    if (e == p) return -1;
    *out = (int)v; return 1;
}

static int json_get_int64_at(const char *p, long long *out)
{
    char *e;
    long long value;

    if (!p)
        return -1;
    errno = 0;
    value = strtoll(p, &e, 10);
    if (e == p || errno == ERANGE ||
        (*e && !isspace((unsigned char)*e) && *e != ',' && *e != '}'))
        return -1;
    *out = value;
    return 1;
}

int json_get_int64(const char *s, const char *k, long long *out)
{
    const char *p = find_key(s, k);

    return p ? json_get_int64_at(p, out) : 0;
}

int json_get_int64_top_level(const char *s, const char *k, long long *out)
{
    const char *p = find_top_level_key(s, k);

    return p ? json_get_int64_at(p, out) : 0;
}

int json_get_array_top_level(const char *s, const char *k,
                             const char **out, size_t *size)
{
    struct json_cursor c;
    const char *p = find_top_level_key(s, k);
    size_t start;

    if (!p)
        return 0;
    if (*p != '[')
        return -1;
    start = (size_t)(p - s);
    c.s = s;
    c.n = strlen(s);
    c.i = start;
    c.depth = 0;
    if (!parse_array(&c))
        return -1;
    if (out)
        *out = p;
    if (size)
        *size = c.i - start;
    return 1;
}

int json_get_uint(const char *s, const char *k, unsigned int *out)
{
    char *e; unsigned long v; const char *p = find_key(s, k);

    if (!p) return 0;
    if (*p == '+' || *p == '-') return -1;
    errno = 0;
    v = strtoul(p, &e, 10);
    if (e == p || errno == ERANGE || v > UINT_MAX) return -1;
    while (isspace((unsigned char)*e)) e++;
    if (*e && *e != ',' && *e != '}') return -1;
    if (out) *out = (unsigned int)v;
    return 1;
}

int json_get_bool(const char *s, const char *k, int *out)
{
    const char *p = find_key(s, k);
    if (!p) return 0;
    if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    return -1;
}

static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int append_codepoint(char *out, size_t z, size_t *used,
                            uint32_t codepoint)
{
    size_t width;

    if (codepoint <= 0x7f) width = 1;
    else if (codepoint <= 0x7ff) width = 2;
    else if (codepoint <= 0xffff) width = 3;
    else width = 4;
    if (*used + width + 1 > z)
        return -1;
    if (width == 1) {
        out[(*used)++] = (char)codepoint;
    } else if (width == 2) {
        out[(*used)++] = (char)(0xc0 | (codepoint >> 6));
        out[(*used)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (width == 3) {
        out[(*used)++] = (char)(0xe0 | (codepoint >> 12));
        out[(*used)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
        out[(*used)++] = (char)(0xf0 | (codepoint >> 18));
        out[(*used)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        out[(*used)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (codepoint & 0x3f));
    }
    return 0;
}

static int json_get_string_at(const char *p, char *out, size_t z)
{
    size_t used = 0;

    if (!p || !out || z == 0 || *p++ != '"')
        return -1;
    while (*p && *p != '"') {
        uint32_t codepoint;
        unsigned char c = (unsigned char)*p++;

        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'u') {
                if (!p[0] || !p[1] || !p[2] || !p[3])
                    return -1;
                int h0 = hex_digit_value(p[0]);
                int h1 = hex_digit_value(p[1]);
                int h2 = hex_digit_value(p[2]);
                int h3 = hex_digit_value(p[3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                    return -1;
                codepoint = (uint32_t)((h0 << 12) | (h1 << 8) |
                                       (h2 << 4) | h3);
                p += 4;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    int low0, low1, low2, low3;
                    if (!p[0] || !p[1] || p[0] != '\\' || p[1] != 'u' ||
                        !p[2] || !p[3] || !p[4] || !p[5])
                        return -1;
                    low0 = hex_digit_value(p[2]);
                    low1 = hex_digit_value(p[3]);
                    low2 = hex_digit_value(p[4]);
                    low3 = hex_digit_value(p[5]);
                    if (low0 < 0 || low1 < 0 || low2 < 0 || low3 < 0)
                        return -1;
                    {
                        uint32_t low = (uint32_t)((low0 << 12) |
                                                  (low1 << 8) |
                                                  (low2 << 4) | low3);
                        if (low < 0xdc00 || low > 0xdfff)
                            return -1;
                        codepoint = 0x10000 +
                                    ((codepoint - 0xd800) << 10) +
                                    (low - 0xdc00);
                    }
                    p += 6;
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    return -1;
                }
                if (codepoint < 0x20 || codepoint > 0x10ffff)
                    return -1;
                if (append_codepoint(out, z, &used, codepoint) < 0)
                    return -1;
                continue;
            }
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c == 'b') c = '\b';
            else if (c == 'f') c = '\f';
            else if (c != '"' && c != '\\' && c != '/') return -1;
        }
        if (c < 32 || used + 2 > z) return -1;
        out[used++] = (char)c;
    }
    if (*p != '"') return -1;
    out[used] = 0;
    return 1;
}

int json_get_string(const char *s, const char *k, char *out, size_t z)
{
    const char *p = find_key(s, k);

    return p ? json_get_string_at(p, out, z) : 0;
}

int json_get_string_top_level(const char *s, const char *k,
                              char *out, size_t z)
{
    const char *p = find_top_level_key(s, k);

    return p ? json_get_string_at(p, out, z) : 0;
}

void json_escape(char *out, size_t z, const char *in)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    while (*in) {
        unsigned char c = (unsigned char)*in;
        size_t width = (c == 34 || c == 92 || c == 10 || c == 13 || c == 9)
            ? 2 : (c < 32 ? 6 : 1);
        if (n + width + 1 > z)
            break;
        ++in;
        if (c == 34 || c == 92) {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c == 10) {
            out[n++] = '\\'; out[n++] = 'n';
        } else if (c == 13) {
            out[n++] = '\\'; out[n++] = 'r';
        } else if (c == 9) {
            out[n++] = '\\'; out[n++] = 't';
        } else if (c >= 32) {
            out[n++] = (char)c;
        } else {
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[c >> 4]; out[n++] = hex[c & 15];
        }
    }
    out[n] = 0;
}
