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

static int identity_value_valid(const char *key, const char *value)
{
    if (!strcmp(key, LE_UPDATE_TAG_KEY))
        return update_identity_tag_valid(value);
    if (!strcmp(key, LE_UPDATE_SHA_KEY))
        return update_identity_sha256_valid(value);
    return value[0] != '\0';
}

int update_identity_value(const char *path, const char *key, char *out,
                          size_t size)
{
    FILE *f;
    char line[RECORD_LINE_MAX];
    size_t key_len;
    int found = 0;

    if (!out || !size)
        return 0;
    out[0] = '\0';
    if (!path || !key || !key[0])
        return 0;
    key_len = strlen(key);
    if (key_len + 2 >= sizeof(line))
        return 0;
    if (!(f = fopen(path, "r")))
        return 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len;
        if (strncmp(line, key, key_len) || line[key_len] != '=')
            continue;
        len = strcspn(line + key_len + 1, "\r\n");
        found = 1;
        /* A value that does not fit the caller's buffer, or that filled the
           line buffer without a terminator, is absent -- never truncated. */
        if (len < size && len < sizeof(line) - (key_len + 1)) {
            memcpy(out, line + key_len + 1, len);
            out[len] = '\0';
        }
        break;
    }
    fclose(f);
    if (!found || !identity_value_valid(key, out)) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}
