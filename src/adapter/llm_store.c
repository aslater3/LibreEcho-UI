#define _POSIX_C_SOURCE 200809L

#include "llm_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int token_safe(const char *token)
{
    const unsigned char *position =
        (const unsigned char *)(token ? token : "");

    if (!*position)
        return 0;
    for (; *position; ++position)
        if (*position <= 0x20U || *position == 0x7fU ||
            *position == '"' || *position == '\\')
            return 0;
    return 1;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *position = buffer;

    while (size) {
        ssize_t count = write(fd, position, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        position += count;
        size -= (size_t)count;
    }
    return 0;
}

static const char *value_for(const char *json, const char *key)
{
    char needle[64];
    const char *position;

    if (snprintf(needle, sizeof(needle), "\"%s\":\"", key) >=
        (int)sizeof(needle))
        return NULL;
    position = strstr(json, needle);
    return position ? position + strlen(needle) : NULL;
}

static int read_token(const char *json, const char *key,
                      char *output, size_t size)
{
    const char *value = value_for(json, key);
    const char *end;
    size_t length;

    if (!value || !(end = strchr(value, '"')))
        return -1;
    length = (size_t)(end - value);
    if (!length || length >= size)
        return -1;
    memcpy(output, value, length);
    output[length] = '\0';
    return token_safe(output) ? 0 : -1;
}

void le_llm_credentials_clear(struct le_llm_credentials *credentials)
{
    volatile unsigned char *position;
    size_t remaining;

    if (!credentials)
        return;
    position = (volatile unsigned char *)credentials;
    remaining = sizeof(*credentials);
    while (remaining--)
        *position++ = 0;
}

int le_llm_credentials_load(const char *path,
                            struct le_llm_credentials *credentials)
{
    char json[LE_LLM_TOKEN_MAX * 2U + 512U];
    struct stat status;
    ssize_t count;
    int fd;

    if (!path || !credentials || lstat(path, &status) < 0 ||
        !S_ISREG(status.st_mode) || (status.st_mode & 077) != 0)
        return -1;
    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    count = read(fd, json, sizeof(json) - 1);
    close(fd);
    if (count <= 0 || (size_t)count >= sizeof(json) - 1)
        return -1;
    json[count] = '\0';
    le_llm_credentials_clear(credentials);
    if (read_token(json, "access_token", credentials->access_token,
                   sizeof(credentials->access_token)) < 0 ||
        read_token(json, "refresh_token", credentials->refresh_token,
                   sizeof(credentials->refresh_token)) < 0)
        goto fail;
    {
        const char *expiry = strstr(json, "\"expires_at\":");
        char *end;
        unsigned long long value;

        if (!expiry)
            goto fail;
        expiry += strlen("\"expires_at\":");
        value = strtoull(expiry, &end, 10);
        if (end == expiry)
            goto fail;
        credentials->expires_at = (time_t)value;
    }
    {
        const char *account = value_for(json, "account_id");
        const char *end;
        size_t length;

        if (account && (end = strchr(account, '"')) != NULL) {
            length = (size_t)(end - account);
            if (length >= sizeof(credentials->account_id))
                goto fail;
            memcpy(credentials->account_id, account, length);
            credentials->account_id[length] = '\0';
        }
    }
    memset(json, 0, sizeof(json));
    return 0;

fail:
    memset(json, 0, sizeof(json));
    le_llm_credentials_clear(credentials);
    return -1;
}

int le_llm_credentials_save(const char *path,
                            const struct le_llm_credentials *credentials)
{
    char temporary[512];
    char json[LE_LLM_TOKEN_MAX * 2U + 512U];
    int length;
    int fd;
    int result = -1;

    if (!path || !credentials ||
        !token_safe(credentials->access_token) ||
        !token_safe(credentials->refresh_token) ||
        snprintf(temporary, sizeof(temporary), "%s.new", path) >=
            (int)sizeof(temporary))
        return -1;
    length = snprintf(
        json, sizeof(json),
        "{\"version\":1,\"access_token\":\"%s\","
        "\"refresh_token\":\"%s\",\"account_id\":\"%s\","
        "\"expires_at\":%llu}\n",
        credentials->access_token, credentials->refresh_token,
        credentials->account_id,
        (unsigned long long)credentials->expires_at);
    if (length <= 0 || length >= (int)sizeof(json))
        return -1;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC |
                         O_NOFOLLOW, 0600);
    if (fd < 0)
        goto cleanup;
    if (fchmod(fd, 0600) < 0 ||
        write_all(fd, json, (size_t)length) < 0 ||
        fsync(fd) < 0) {
        close(fd);
        goto cleanup;
    }
    if (close(fd) < 0)
        goto cleanup;
    if (rename(temporary, path) < 0)
        goto cleanup;
    result = 0;

cleanup:
    if (result < 0)
        unlink(temporary);
    memset(json, 0, sizeof(json));
    return result;
}

int le_llm_credentials_remove(const char *path)
{
    if (!path)
        return -1;
    if (unlink(path) == 0 || errno == ENOENT)
        return 0;
    return -1;
}
