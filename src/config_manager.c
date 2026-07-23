/*
 * LibreEcho central configuration manager.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "config_manager.h"
#include "json.h"
#include "log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char config_path[256] = LE_CONFIG_PATH;
static char config_buf[LE_CONFIG_BUF_MAX];
static int config_loaded = 0;

/* Find a top-level key and return pointer to its value (after the ':'). */
static const char *find_top_key(const char *json, const char *key)
{
    char needle[128];
    const char *p;
    size_t klen;

    snprintf(needle, sizeof(needle), "\"%s\"", key);
    klen = strlen(needle);
    p = json;
    while ((p = strstr(p, needle)) != NULL) {
        const char *v = p + klen;
        while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
            v++;
        if (*v == ':') {
            v++;
            while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')
                v++;
            return v;
        }
        p += klen;
    }
    return NULL;
}

/* Find the end of a JSON object starting at p. Returns pointer past '}'. */
static const char *object_end(const char *p)
{
    int depth = 0;
    int in_str = 0;
    int esc = 0;

    if (!p || *p != '{')
        return NULL;
    while (*p) {
        char c = *p;
        if (in_str) {
            if (esc)
                esc = 0;
            else if (c == '\\')
                esc = 1;
            else if (c == '"')
                in_str = 0;
        } else if (c == '"') {
            in_str = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0)
                return p + 1;
        }
        p++;
    }
    return NULL;
}

int le_config_init(const char *path)
{
    int fd;
    ssize_t n;

    if (path) {
        strncpy(config_path, path, sizeof(config_path) - 1);
        config_path[sizeof(config_path) - 1] = '\0';
    }

    fd = open(config_path, O_RDONLY);
    if (fd < 0) {
        le_log_warn("config: cannot open %s: %s", config_path, strerror(errno));
        return -1;
    }

    n = read(fd, config_buf, sizeof(config_buf) - 1);
    close(fd);

    if (n < 0) {
        le_log_error("config: read failed: %s", strerror(errno));
        return -1;
    }

    config_buf[n] = '\0';

    if (!json_valid_object(config_buf, strlen(config_buf))) {
        le_log_error("config: invalid JSON in %s", config_path);
        return -1;
    }

    config_loaded = 1;
    le_log_info("config: loaded %s (%zd bytes)", config_path, n);
    return 0;
}

int le_config_reload(void)
{
    char new_buf[LE_CONFIG_BUF_MAX];
    int fd;
    ssize_t n;

    fd = open(config_path, O_RDONLY);
    if (fd < 0) {
        le_log_error("config: reload failed, cannot open %s: %s", config_path, strerror(errno));
        return -1;
    }

    n = read(fd, new_buf, sizeof(new_buf) - 1);
    close(fd);

    if (n < 0) {
        le_log_error("config: reload failed, read error: %s", strerror(errno));
        return -1;
    }

    new_buf[n] = '\0';

    if (!json_valid_object(new_buf, strlen(new_buf))) {
        le_log_error("config: reload failed, invalid JSON");
        return -1;
    }

    memcpy(config_buf, new_buf, n + 1);
    le_log_info("config: reloaded %s (%zd bytes)", config_path, n);
    return 0;
}

int le_config_section(const char *section, char *out, size_t out_size)
{
    const char *start;
    const char *end;
    size_t len;

    if (!config_loaded || !section || !out || out_size == 0)
        return -1;

    start = find_top_key(config_buf, section);
    if (!start || *start != '{')
        return -1;

    end = object_end(start);
    if (!end)
        return -1;

    len = (size_t)(end - start);
    if (len >= out_size)
        return -1;

    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

int le_config_get_int(const char *section, const char *key, int *value)
{
    char section_buf[4096];

    if (le_config_section(section, section_buf, sizeof(section_buf)) < 0)
        return -1;

    if (json_get_int(section_buf, key, value) <= 0)
        return -1;

    return 0;
}

int le_config_get_bool(const char *section, const char *key, int *value)
{
    char section_buf[4096];

    if (le_config_section(section, section_buf, sizeof(section_buf)) < 0)
        return -1;

    if (json_get_bool(section_buf, key, value) <= 0)
        return -1;

    return 0;
}

int le_config_get_string(const char *section, const char *key, char *out, size_t out_size)
{
    char section_buf[4096];

    if (le_config_section(section, section_buf, sizeof(section_buf)) < 0)
        return -1;

    if (json_get_string(section_buf, key, out, out_size) <= 0)
        return -1;

    return 0;
}

static void save_history(void)
{
    char history_dir[256];
    char new_path[512];
    char timestamp[32];
    time_t now;
    struct tm tm_buf;
    DIR *dir;
    struct dirent *ent;
    char *files[LE_CONFIG_HISTORY_MAX + 5];
    int count = 0;
    int i;

    snprintf(history_dir, sizeof(history_dir), "%s", LE_CONFIG_HISTORY);
    mkdir(history_dir, 0755);

    /* Count existing history files */
    dir = opendir(history_dir);
    if (dir) {
        while ((ent = readdir(dir)) != NULL && count < LE_CONFIG_HISTORY_MAX + 5) {
            if (strncmp(ent->d_name, "config-", 7) == 0) {
                files[count] = strdup(ent->d_name);
                count++;
            }
        }
        closedir(dir);
    }

    /* Remove oldest if we have too many */
    if (count >= LE_CONFIG_HISTORY_MAX) {
        char old_path[512];
        snprintf(old_path, sizeof(old_path), "%s/%s", history_dir, files[0]);
        unlink(old_path);
    }

    /* Free allocated strings */
    for (i = 0; i < count; i++) {
        free(files[i]);
    }

    /* Save current config to history */
    now = time(NULL);
    localtime_r(&now, &tm_buf);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &tm_buf);

    snprintf(new_path, sizeof(new_path), "%s/config-%s.json", history_dir, timestamp);

    if (config_loaded) {
        int fd = open(new_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, config_buf, strlen(config_buf));
            close(fd);
            le_log_debug("config: saved history to %s", new_path);
        }
    }
}

int le_config_write(const char *json, size_t len)
{
    char tmp_path[512];
    int fd;
    ssize_t n;

    if (!json || len == 0)
        return -1;

    /* Save history before overwriting */
    save_history();

    /* Write to temp file */
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", config_path);
    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        le_log_error("config: cannot create %s: %s", tmp_path, strerror(errno));
        return -1;
    }

    n = write(fd, json, len);
    if (n != (ssize_t)len) {
        le_log_error("config: write failed: %s", strerror(errno));
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    fsync(fd);
    close(fd);

    /* Atomic rename */
    if (rename(tmp_path, config_path) < 0) {
        le_log_error("config: rename failed: %s", strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    /* Reload into memory */
    if (len < sizeof(config_buf)) {
        memcpy(config_buf, json, len);
        config_buf[len] = '\0';
        config_loaded = 1;
    }

    le_log_info("config: wrote %s (%zu bytes)", config_path, len);
    return 0;
}

const char *le_config_path(void)
{
    return config_path;
}
