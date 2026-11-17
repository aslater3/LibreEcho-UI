#define _POSIX_C_SOURCE 200809L

#include "wake_model_store.h"

#include "config_store.h"
#include "wake_model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STAGING_DIRECTORY ".staging"

static int known_file(const char *file)
{
    size_t i;

    for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i)
        if (!strcmp(file, le_wake_model_files[i]))
            return 1;
    return 0;
}

static int reserved_name(const char *name)
{
    return !strcmp(name, LE_WAKE_MODEL_STOCK_NAME);
}

/* Copies path without its last component. Returns 0 or -1. */
static int parent_of(char *out, size_t size, const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t length;

    if (!slash) {
        if (size < 2)
            return -1;
        memcpy(out, ".", 2);
        return 0;
    }
    length = slash == path ? 1 : (size_t)(slash - path);
    if (length + 1 > size)
        return -1;
    memcpy(out, path, length);
    out[length] = '\0';
    return 0;
}

static int base_is(const char *path, const char *base)
{
    const char *slash = strrchr(path, '/');
    return !strcmp(slash ? slash + 1 : path, base);
}

int le_wake_model_paths_from_config(const char *config_path, struct le_wake_model_paths *paths)
{
    char config_directory[320], root[320];

    if (!config_path || !paths || !config_path[0])
        return -1;
    if (parent_of(config_directory, sizeof(config_directory), config_path) < 0)
        return -1;
    /* The device keeps configuration in <root>/config; anywhere else (tests)
     * the folders sit beside the configuration file. */
    if (base_is(config_directory, "config")) {
        if (parent_of(root, sizeof(root), config_directory) < 0)
            return -1;
    } else {
        memcpy(root, config_directory, strlen(config_directory) + 1);
    }
    if (snprintf(paths->models, sizeof(paths->models), "%s/wakeword-models", root) >=
            (int)sizeof(paths->models) ||
        snprintf(paths->staging, sizeof(paths->staging), "%s/%s", paths->models,
                 STAGING_DIRECTORY) >= (int)sizeof(paths->staging) ||
        snprintf(paths->selection, sizeof(paths->selection), "%s/wake-model",
                 config_directory) >= (int)sizeof(paths->selection))
        return -1;
    return 0;
}

int le_wake_model_selection_read(const struct le_wake_model_paths *paths,
                                 char *name, size_t name_size)
{
    char buffer[LE_WAKE_MODEL_NAME_MAX + 2];
    int fd;
    ssize_t n;
    size_t length;

    if (!paths || !name || !name_size)
        return -1;
    fd = open(paths->selection, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n < 0)
        return -1;
    buffer[n] = '\0';
    length = strcspn(buffer, "\r\n");
    buffer[length] = '\0';
    if (!le_wake_model_name_valid(buffer) || reserved_name(buffer))
        return -1;
    if (length >= name_size)
        return -1;
    memcpy(name, buffer, length + 1);
    return 1;
}

static unsigned long file_bytes(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode))
        return 0;
    return (unsigned long)st.st_size;
}

int le_wake_model_scan(const struct le_wake_model_paths *paths,
                       le_wake_model_visitor visitor, void *opaque)
{
    DIR *dir;
    struct dirent *entry;
    char selected[LE_WAKE_MODEL_NAME_MAX];
    int have_selection, count = 0;

    if (!paths)
        return LE_WAKE_MODEL_STORE_IO;
    have_selection = le_wake_model_selection_read(paths, selected, sizeof(selected)) == 1;
    dir = opendir(paths->models);
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        struct le_wake_model_entry item;
        char folder[400], file[480];
        struct stat st;
        size_t i;

        if (!le_wake_model_name_valid(entry->d_name) || reserved_name(entry->d_name))
            continue;
        if (snprintf(folder, sizeof(folder), "%s/%s", paths->models, entry->d_name) >=
                (int)sizeof(folder) || lstat(folder, &st) < 0 || !S_ISDIR(st.st_mode))
            continue;
        memset(&item, 0, sizeof(item));
        snprintf(item.name, sizeof(item.name), "%.*s", (int)(sizeof(item.name) - 1), entry->d_name);
        item.valid = le_wake_model_validate_directory(folder, item.reason, sizeof(item.reason)) == 0;
        for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i) {
            if (snprintf(file, sizeof(file), "%s/%s", folder, le_wake_model_files[i]) <
                (int)sizeof(file))
                item.bytes += file_bytes(file);
        }
        item.selected = have_selection && !strcmp(selected, item.name);
        visitor(&item, opaque);
        ++count;
    }
    closedir(dir);
    return count;
}

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0700) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void remove_staged(const struct le_wake_model_paths *paths, const char *name)
{
    char folder[400], file[480];
    size_t i;

    if (snprintf(folder, sizeof(folder), "%s/%s", paths->staging, name) >= (int)sizeof(folder))
        return;
    for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i) {
        if (snprintf(file, sizeof(file), "%s/%s", folder, le_wake_model_files[i]) <
            (int)sizeof(file))
            unlink(file);
    }
    rmdir(folder);
}

int le_wake_model_stage_chunk(const struct le_wake_model_paths *paths, const char *name,
                              const char *file, unsigned long offset,
                              const unsigned char *data, size_t length,
                              unsigned long *staged_bytes)
{
    char folder[400], path[480], committed[400];
    struct stat st;
    unsigned long current;
    size_t written = 0;
    int fd;

    if (!paths || !name || !file || !data || !staged_bytes)
        return LE_WAKE_MODEL_STORE_INVALID;
    if (!le_wake_model_name_valid(name) || reserved_name(name) || !known_file(file))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (length == 0 || length > LE_WAKE_MODEL_STAGE_CHUNK_MAX ||
        offset > LE_WAKE_MODEL_MAX_BYTES || length > LE_WAKE_MODEL_MAX_BYTES - offset)
        return LE_WAKE_MODEL_STORE_INVALID;
    if (snprintf(committed, sizeof(committed), "%s/%s", paths->models, name) >=
        (int)sizeof(committed))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (offset == 0 && lstat(committed, &st) == 0)
        return LE_WAKE_MODEL_STORE_CONFLICT;
    if (ensure_directory(paths->models) < 0 || ensure_directory(paths->staging) < 0)
        return LE_WAKE_MODEL_STORE_IO;
    if (snprintf(folder, sizeof(folder), "%s/%s", paths->staging, name) >= (int)sizeof(folder) ||
        ensure_directory(folder) < 0)
        return LE_WAKE_MODEL_STORE_IO;
    if (snprintf(path, sizeof(path), "%s/%s", folder, file) >= (int)sizeof(path))
        return LE_WAKE_MODEL_STORE_INVALID;

    current = file_bytes(path);
    if (offset == 0) {
        current = 0;
    } else if (current != offset) {
        return LE_WAKE_MODEL_STORE_INVALID;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC | (offset == 0 ? O_TRUNC : 0), 0600);
    if (fd < 0)
        return LE_WAKE_MODEL_STORE_IO;
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
        close(fd);
        return LE_WAKE_MODEL_STORE_IO;
    }
    while (written < length) {
        ssize_t n = write(fd, data + written, length - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return LE_WAKE_MODEL_STORE_IO;
        }
        written += (size_t)n;
    }
    if (close(fd) < 0)
        return LE_WAKE_MODEL_STORE_IO;
    *staged_bytes = offset + length;
    return LE_WAKE_MODEL_STORE_OK;
}

int le_wake_model_commit(const struct le_wake_model_paths *paths, const char *name,
                         char *reason, size_t reason_size)
{
    char folder[400], committed[400];
    struct stat st;
    int rc;

    if (!paths || !name || !le_wake_model_name_valid(name) || reserved_name(name))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (snprintf(folder, sizeof(folder), "%s/%s", paths->staging, name) >= (int)sizeof(folder) ||
        snprintf(committed, sizeof(committed), "%s/%s", paths->models, name) >=
            (int)sizeof(committed))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (lstat(folder, &st) < 0 || !S_ISDIR(st.st_mode))
        return LE_WAKE_MODEL_STORE_NOT_FOUND;
    rc = le_wake_model_validate_directory(folder, reason, reason_size);
    if (rc < 0) {
        remove_staged(paths, name);
        return LE_WAKE_MODEL_STORE_INVALID;
    }
    if (lstat(committed, &st) == 0) {
        remove_staged(paths, name);
        if (reason && reason_size)
            snprintf(reason, reason_size, "%s", "a model with this name already exists");
        return LE_WAKE_MODEL_STORE_CONFLICT;
    }
    if (rename(folder, committed) < 0) {
        remove_staged(paths, name);
        return LE_WAKE_MODEL_STORE_IO;
    }
    rmdir(paths->staging);
    return LE_WAKE_MODEL_STORE_OK;
}

int le_wake_model_select(const struct le_wake_model_paths *paths, const char *name)
{
    char folder[400], text[LE_WAKE_MODEL_NAME_MAX + 2];
    struct stat st;
    char reason[160];
    int length;

    if (!paths)
        return LE_WAKE_MODEL_STORE_INVALID;
    if (!name || !name[0] || reserved_name(name)) {
        /* The stock model: no selection file. */
        if (unlink(paths->selection) < 0 && errno != ENOENT)
            return LE_WAKE_MODEL_STORE_IO;
        return LE_WAKE_MODEL_STORE_OK;
    }
    if (!le_wake_model_name_valid(name))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (snprintf(folder, sizeof(folder), "%s/%s", paths->models, name) >= (int)sizeof(folder))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (lstat(folder, &st) < 0 || !S_ISDIR(st.st_mode))
        return LE_WAKE_MODEL_STORE_NOT_FOUND;
    if (le_wake_model_validate_directory(folder, reason, sizeof(reason)) < 0)
        return LE_WAKE_MODEL_STORE_INVALID;
    length = snprintf(text, sizeof(text), "%s\n", name);
    if (length < 0 || (size_t)length >= sizeof(text))
        return LE_WAKE_MODEL_STORE_INVALID;
    return config_write_atomic(paths->selection, text, (size_t)length) < 0
               ? LE_WAKE_MODEL_STORE_IO
               : LE_WAKE_MODEL_STORE_OK;
}

int le_wake_model_delete(const struct le_wake_model_paths *paths, const char *name)
{
    char folder[400], file[480], selected[LE_WAKE_MODEL_NAME_MAX];
    struct stat st;
    size_t i;

    if (!paths || !name || !le_wake_model_name_valid(name) || reserved_name(name))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (le_wake_model_selection_read(paths, selected, sizeof(selected)) == 1 &&
        !strcmp(selected, name))
        return LE_WAKE_MODEL_STORE_CONFLICT;
    if (snprintf(folder, sizeof(folder), "%s/%s", paths->models, name) >= (int)sizeof(folder))
        return LE_WAKE_MODEL_STORE_INVALID;
    if (lstat(folder, &st) < 0 || !S_ISDIR(st.st_mode))
        return LE_WAKE_MODEL_STORE_NOT_FOUND;
    for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i) {
        if (snprintf(file, sizeof(file), "%s/%s", folder, le_wake_model_files[i]) <
            (int)sizeof(file) && unlink(file) < 0 && errno != ENOENT)
            return LE_WAKE_MODEL_STORE_IO;
    }
    return rmdir(folder) < 0 ? LE_WAKE_MODEL_STORE_IO : LE_WAKE_MODEL_STORE_OK;
}

int le_wake_model_base64_decode(const char *text, size_t text_length,
                                unsigned char *out, size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, produced = 0;
    unsigned accumulator = 0;
    int bits = 0, padding = 0;

    if (!text || !out || text_length % 4 != 0)
        return -1;
    for (i = 0; i < text_length; ++i) {
        const char *hit;
        char c = text[i];

        if (c == '=') {
            ++padding;
            if (padding > 2 || i < text_length - 2)
                return -1;
            continue;
        }
        if (padding)
            return -1;
        hit = strchr(alphabet, c);
        if (!c || !hit)
            return -1;
        accumulator = (accumulator << 6) | (unsigned)(hit - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (produced >= out_size)
                return -1;
            out[produced++] = (unsigned char)((accumulator >> bits) & 0xffu);
        }
    }
    return (int)produced;
}
