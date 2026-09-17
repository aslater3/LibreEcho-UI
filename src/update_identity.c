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

/* Copy one key's value out of a record line that carries it, reporting whether
   this line was the key's own. A value that does not fit the caller's buffer,
   or that filled the line buffer without a terminator, is not copied: the key
   stays unresolved instead of yielding a truncated value. */
static int line_value(const char *line, const char *key, char *out, size_t size)
{
    size_t key_len = strlen(key);
    size_t len;
    if (strncmp(line, key, key_len) || line[key_len] != '=')
        return 0;
    len = strcspn(line + key_len + 1, "\r\n");
    if (len >= size || len + key_len + 1 >= RECORD_LINE_MAX)
        return 0;
    memcpy(out, line + key_len + 1, len);
    out[len] = '\0';
    return 1;
}

int update_identity_pair(const char *path, char *tag, size_t tag_size,
                         char *sha, size_t sha_size)
{
    FILE *f;
    char line[RECORD_LINE_MAX];
    int seen_tag = 0, seen_sha = 0, resolved = 0;

    if (!tag || !tag_size || !sha || !sha_size)
        return 0;
    tag[0] = '\0';
    sha[0] = '\0';
    if (!path)
        return 0;
    if (!(f = fopen(path, "r")))
        return 0;
    /* One open, one pass: both values come from the same snapshot of the
       record, so they always describe the same candidate. The first
       occurrence of each key wins, matching how a record written before these
       keys existed leaves them unresolved. */
    while (fgets(line, sizeof(line), f)) {
        if (!seen_tag && line_value(line, LE_UPDATE_TAG_KEY, tag, tag_size)) {
            seen_tag = 1;
            continue;
        }
        if (!seen_sha && line_value(line, LE_UPDATE_SHA_KEY, sha, sha_size))
            seen_sha = 1;
    }
    fclose(f);
    /* Validate once the snapshot is complete, so a malformed value is reported
       as absent and never reaches the caller as an identity. */
    if (update_identity_tag_valid(tag))
        resolved++;
    else
        tag[0] = '\0';
    if (update_identity_sha256_valid(sha))
        resolved++;
    else
        sha[0] = '\0';
    return resolved;
}
