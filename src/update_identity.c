#include "update_identity.h"

#include <stdio.h>
#include <string.h>

#define TAG_PREFIX_BUILD "radar-puffin-build-"
#define TAG_PREFIX_NIGHTLY "radar-puffin-nightly-"
#define RECORD_LINE_MAX 512

static int hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* A hex run is only consumed one character at a time, and a NUL is never a hex
   digit, so a short value stops the scan at its terminator instead of running
   past it. */
static int hex_run(const char *value, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (!hex_digit(value[i]))
            return 0;
    return 1;
}

int update_identity_tag_valid(const char *value)
{
    const char *rest;
    if (!value)
        return 0;
    if (!strncmp(value, TAG_PREFIX_BUILD, sizeof(TAG_PREFIX_BUILD) - 1))
        rest = value + sizeof(TAG_PREFIX_BUILD) - 1;
    else if (!strncmp(value, TAG_PREFIX_NIGHTLY, sizeof(TAG_PREFIX_NIGHTLY) - 1))
        rest = value + sizeof(TAG_PREFIX_NIGHTLY) - 1;
    else
        return 0;
    return hex_run(rest, 7) && rest[7] == '-' &&
        hex_run(rest + 8, 16) && rest[24] == '-' &&
        hex_run(rest + 25, 16) && rest[41] == '\0';
}

int update_identity_sha256_valid(const char *value)
{
    return value && hex_run(value, 64) && value[64] == '\0';
}

static int identity_key(const char *key)
{
    return !strcmp(key, LE_UPDATE_TAG_KEY) || !strcmp(key, LE_UPDATE_SHA_KEY);
}

/* Copy one key's value out of a record line that carries it, reporting whether
   this line was the key's own. A value that filled the line buffer without a
   terminator is never copied. An identity that does not fit the caller's buffer
   is not copied either, so it stays unresolved instead of being cut down to a
   buffer-sized prefix; an opaque field keeps the bounded copy this envelope has
   always reported. */
static int line_value(const char *line, const char *key, char *out, size_t size,
                      int exact)
{
    size_t key_len = strlen(key);
    size_t len;

    if (strncmp(line, key, key_len) || line[key_len] != '=')
        return 0;
    len = strcspn(line + key_len + 1, "\r\n");
    if (len + key_len + 1 >= RECORD_LINE_MAX)
        return 0;
    if (len >= size) {
        if (exact)
            return 0;
        len = size - 1;
    }
    memcpy(out, line + key_len + 1, len);
    out[len] = '\0';
    return 1;
}

int update_record_read(const char *path, struct le_update_field *fields,
                       size_t count)
{
    FILE *f;
    char line[RECORD_LINE_MAX];
    unsigned char taken[LE_UPDATE_RECORD_FIELD_MAX];
    size_t i, pending = 0, reported = 0;

    if (!path || !fields || !count || count > LE_UPDATE_RECORD_FIELD_MAX)
        return 0;
    memset(taken, 0, sizeof(taken));
    for (i = 0; i < count; i++)
        if (fields[i].key && fields[i].key[0] && fields[i].value && fields[i].size)
            pending++;
    if (!pending)
        return 0;
    /* One open, one pass: every field is resolved while this one descriptor is
       held, so no two of them can come from different generations of the
       record. The first occurrence of a key wins, which is what a record whose
       writer never repeats a key has always meant here. */
    if ((f = fopen(path, "r"))) {
        while (pending && fgets(line, sizeof(line), f)) {
            for (i = 0; i < count; i++) {
                if (taken[i] || !fields[i].key || !fields[i].key[0] ||
                    !fields[i].value || !fields[i].size)
                    continue;
                if (line_value(line, fields[i].key, fields[i].value,
                               fields[i].size, identity_key(fields[i].key))) {
                    taken[i] = 1;
                    pending--;
                }
            }
        }
        fclose(f);
    }
    /* Validation runs on the completed snapshot. An identity is only ever
       reported when the record carried that exact value, so an empty, malformed
       or oversized one -- and a record that could not be read at all -- leaves
       an empty string rather than a value the device did not resolve. */
    for (i = 0; i < count; i++) {
        if (!fields[i].key || !fields[i].key[0] || !fields[i].value ||
            !fields[i].size)
            continue;
        if (!strcmp(fields[i].key, LE_UPDATE_TAG_KEY)) {
            if (taken[i] && update_identity_tag_valid(fields[i].value)) {
                reported++;
            } else {
                fields[i].value[0] = '\0';
                taken[i] = 0;
            }
        } else if (!strcmp(fields[i].key, LE_UPDATE_SHA_KEY)) {
            if (taken[i] && update_identity_sha256_valid(fields[i].value)) {
                reported++;
            } else {
                fields[i].value[0] = '\0';
                taken[i] = 0;
            }
        } else if (taken[i]) {
            reported++;
        }
    }
    return (int)reported;
}
