#define _POSIX_C_SOURCE 200809L
#include "voice_clip_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CLIP_EXTENSION ".wav"
#define CLIP_EXTENSION_BYTES 4U
#define SETTINGS_NAME "settings"
#define SETTINGS_TEMP ".settings.tmp"
#define CLIP_WAV_HEADER_BYTES 44U
#define CLIP_DIRECTORY_MAX 224U

static unsigned int clip_sequence;

const char *le_voice_clip_directory(void)
{
    const char *value = getenv(LE_VOICE_CLIP_ENV);

    return value && value[0] ? value : LE_VOICE_CLIP_DEFAULT_DIR;
}

static int directory_valid(const char *directory)
{
    return directory && directory[0] &&
           strlen(directory) < CLIP_DIRECTORY_MAX;
}

static int path_join(char *out, size_t size, const char *directory,
                     const char *name)
{
    int written;

    if (!directory_valid(directory) || !name)
        return -1;
    written = snprintf(out, size, "%s/%s", directory, name);
    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int ensure_directory(const char *directory)
{
    struct stat info;

    if (stat(directory, &info) == 0)
        return S_ISDIR(info.st_mode) ? 0 : -1;
    if (errno != ENOENT)
        return -1;
    /* Only the final component is created; the parent must already exist,
     * so a mistyped override cannot silently build a tree elsewhere. */
    if (mkdir(directory, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *cursor = data;

    while (length > 0) {
        ssize_t written = write(fd, cursor, length);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return 0;
}

/* Strict canonical shape: "<kind>-<10 digits>-<4 digits>", kind [a-z]{1,8}.
 * Anything else, including separators or dots, is rejected before any path is
 * built from it. */
static int parse_id(const char *id, char *kind, long long *created,
                    unsigned long *sequence)
{
    size_t length, kind_length, i;
    const char *dash;
    unsigned long long seconds = 0, number = 0;

    if (!id)
        return 0;
    length = strlen(id);
    if (length == 0 || length >= LE_VOICE_CLIP_ID_MAX)
        return 0;
    dash = strchr(id, '-');
    if (!dash)
        return 0;
    kind_length = (size_t)(dash - id);
    if (kind_length == 0 || kind_length >= LE_VOICE_CLIP_KIND_MAX)
        return 0;
    for (i = 0; i < kind_length; ++i) {
        if (id[i] < 'a' || id[i] > 'z')
            return 0;
    }
    if (length != kind_length + 1U + 10U + 1U + 4U)
        return 0;
    if (dash[1 + 10] != '-')
        return 0;
    for (i = 0; i < 10; ++i) {
        char digit = dash[1 + i];

        if (digit < '0' || digit > '9')
            return 0;
        seconds = seconds * 10U + (unsigned)(digit - '0');
    }
    for (i = 0; i < 4; ++i) {
        char digit = dash[1 + 10 + 1 + i];

        if (digit < '0' || digit > '9')
            return 0;
        number = number * 10U + (unsigned)(digit - '0');
    }
    if (kind) {
        memcpy(kind, id, kind_length);
        kind[kind_length] = '\0';
    }
    if (created)
        *created = (long long)seconds;
    if (sequence)
        *sequence = (unsigned long)number;
    return 1;
}

int le_voice_clip_id_valid(const char *id)
{
    return parse_id(id, NULL, NULL, NULL);
}

static int clip_name_from(const char *id, char *name, size_t size)
{
    int written = snprintf(name, size, "%s%s", id, CLIP_EXTENSION);

    return written < 0 || (size_t)written >= size ? -1 : 0;
}

static int compare_entries(const void *left, const void *right)
{
    const struct le_voice_clip_entry *a = left;
    const struct le_voice_clip_entry *b = right;

    if (a->created != b->created)
        return a->created < b->created ? -1 : 1;
    if (a->sequence != b->sequence)
        return a->sequence < b->sequence ? -1 : 1;
    return 0;
}

int le_voice_clip_settings_load(const char *directory,
                                struct le_voice_clip_settings *settings)
{
    char path[CLIP_DIRECTORY_MAX + 16];
    char text[32];
    FILE *file;
    size_t length;

    if (!settings)
        return -1;
    settings->near_miss_enabled = 0;
    if (path_join(path, sizeof(path), directory, SETTINGS_NAME) != 0)
        return 0;
    file = fopen(path, "r");
    if (!file)
        return 0;
    length = fread(text, 1, sizeof(text) - 1U, file);
    fclose(file);
    text[length] = '\0';
    settings->near_miss_enabled = strcmp(text, "near_miss=1\n") == 0;
    return 0;
}

int le_voice_clip_settings_save(const char *directory,
                                const struct le_voice_clip_settings *settings)
{
    char path[CLIP_DIRECTORY_MAX + 16], temporary[CLIP_DIRECTORY_MAX + 24];
    const char *text;
    int fd;

    if (!settings || ensure_directory(directory) != 0)
        return -1;
    if (path_join(path, sizeof(path), directory, SETTINGS_NAME) != 0 ||
        path_join(temporary, sizeof(temporary), directory, SETTINGS_TEMP) != 0)
        return -1;
    text = settings->near_miss_enabled ? "near_miss=1\n" : "near_miss=0\n";
    (void)unlink(temporary);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0)
        return -1;
    if (write_all(fd, text, strlen(text)) != 0 || fsync(fd) != 0) {
        close(fd);
        (void)unlink(temporary);
        return -1;
    }
    if (close(fd) != 0 || rename(temporary, path) != 0) {
        (void)unlink(temporary);
        return -1;
    }
    return 0;
}

int le_voice_clip_list(const char *directory, struct le_voice_clip_entry *entries,
                       unsigned capacity, unsigned *total_bytes)
{
    DIR *dir;
    struct dirent *item;
    unsigned count = 0;
    unsigned long bytes = 0;

    if (!entries || !total_bytes || !directory_valid(directory))
        return -1;
    *total_bytes = 0;
    dir = opendir(directory);
    if (!dir)
        return errno == ENOENT ? 0 : -1;
    while ((item = readdir(dir)) != NULL && count < capacity) {
        struct le_voice_clip_entry *entry = &entries[count];
        char name[LE_VOICE_CLIP_ID_MAX + CLIP_EXTENSION_BYTES + 1U];
        char path[CLIP_DIRECTORY_MAX + LE_VOICE_CLIP_ID_MAX + 8U];
        char stem[LE_VOICE_CLIP_ID_MAX];
        size_t length = strlen(item->d_name);
        struct stat info;

        if (length <= CLIP_EXTENSION_BYTES || length >= sizeof(stem) + CLIP_EXTENSION_BYTES)
            continue;
        if (strcmp(item->d_name + length - CLIP_EXTENSION_BYTES, CLIP_EXTENSION) != 0)
            continue;
        memcpy(stem, item->d_name, length - CLIP_EXTENSION_BYTES);
        stem[length - CLIP_EXTENSION_BYTES] = '\0';
        if (!parse_id(stem, entry->kind, &entry->created, &entry->sequence))
            continue;
        if (snprintf(name, sizeof(name), "%s", item->d_name) < 0 ||
            path_join(path, sizeof(path), directory, name) != 0)
            continue;
        if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        memcpy(entry->id, stem, sizeof(entry->id));
        entry->bytes = (unsigned long)info.st_size;
        bytes += entry->bytes;
        ++count;
    }
    closedir(dir);
    qsort(entries, count, sizeof(entries[0]), compare_entries);
    *total_bytes = (unsigned)(bytes > 0xffffffffUL ? 0xffffffffUL : bytes);
    return (int)count;
}

int le_voice_clip_path(const char *directory, const char *id,
                       char *path, size_t size)
{
    char name[LE_VOICE_CLIP_ID_MAX + CLIP_EXTENSION_BYTES + 1U];
    struct stat info;

    if (!le_voice_clip_id_valid(id) || clip_name_from(id, name, sizeof(name)) != 0 ||
        path_join(path, size, directory, name) != 0)
        return -1;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode))
        return -1;
    return 0;
}

int le_voice_clip_delete(const char *directory, const char *id)
{
    char name[LE_VOICE_CLIP_ID_MAX + CLIP_EXTENSION_BYTES + 1U];
    char path[CLIP_DIRECTORY_MAX + LE_VOICE_CLIP_ID_MAX + 8U];
    struct stat info;

    if (!le_voice_clip_id_valid(id) || clip_name_from(id, name, sizeof(name)) != 0 ||
        path_join(path, sizeof(path), directory, name) != 0)
        return -1;
    if (lstat(path, &info) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISREG(info.st_mode))
        return -1;
    return unlink(path) == 0 ? 1 : -1;
}

/* List, clear and write share a scratch array and are therefore not
 * reentrant. Each caller is one thread: the web API and the wake worker. */
int le_voice_clip_clear(const char *directory)
{
    static struct le_voice_clip_entry entries[LE_VOICE_CLIP_LIST_MAX];
    unsigned total = 0;
    int listed, i, removed = 0;

    listed = le_voice_clip_list(directory, entries, LE_VOICE_CLIP_LIST_MAX, &total);
    if (listed < 0)
        return -1;
    for (i = 0; i < listed; ++i) {
        if (le_voice_clip_delete(directory, entries[i].id) == 1)
            ++removed;
    }
    return removed;
}

int le_voice_clip_write(const char *directory, const char *kind,
                        const int16_t *samples, size_t count, long long now)
{
    static struct le_voice_clip_entry entries[LE_VOICE_CLIP_LIST_MAX];
    unsigned long long new_bytes;
    unsigned total = 0;
    unsigned attempt;
    int listed, i, fd = -1;
    char id[LE_VOICE_CLIP_ID_MAX], final_name[LE_VOICE_CLIP_ID_MAX + CLIP_EXTENSION_BYTES + 1U];
    char final_path[CLIP_DIRECTORY_MAX + LE_VOICE_CLIP_ID_MAX + 8U];
    char temp_path[CLIP_DIRECTORY_MAX + LE_VOICE_CLIP_ID_MAX + 16U];
    unsigned char header[CLIP_WAV_HEADER_BYTES];
    size_t kind_length;

    if (!kind || !samples || count == 0 || count > LE_VOICE_CLIP_MAX_SAMPLES)
        return -1;
    kind_length = strlen(kind);
    if (kind_length == 0 || kind_length >= LE_VOICE_CLIP_KIND_MAX)
        return -1;
    for (i = 0; i < (int)kind_length; ++i) {
        if (kind[i] < 'a' || kind[i] > 'z')
            return -1;
    }
    if (now < 0 || now > 9999999999LL || ensure_directory(directory) != 0)
        return -1;

    new_bytes = CLIP_WAV_HEADER_BYTES + (unsigned long long)count * 2ULL;
    if (new_bytes > LE_VOICE_CLIP_MAX_BYTES)
        return -1;

    /* Retention first: evict oldest-first until the new clip fits both bounds. */
    listed = le_voice_clip_list(directory, entries, LE_VOICE_CLIP_LIST_MAX, &total);
    if (listed < 0)
        return -1;
    i = 0;
    while (i < listed &&
           ((unsigned)(listed - i) >= LE_VOICE_CLIP_MAX_COUNT ||
            (unsigned long long)total + new_bytes > LE_VOICE_CLIP_MAX_BYTES)) {
        if (le_voice_clip_delete(directory, entries[i].id) == 1) {
            total -= (unsigned)entries[i].bytes;
        }
        ++i;
    }

    for (attempt = 0; attempt < 10000U && fd < 0; ++attempt) {
        struct stat info;

        clip_sequence = (clip_sequence + 1U) % 10000U;
        if (snprintf(id, sizeof(id), "%s-%010lld-%04u", kind, now,
                     clip_sequence) < 0)
            return -1;
        if (clip_name_from(id, final_name, sizeof(final_name)) != 0 ||
            path_join(final_path, sizeof(final_path), directory, final_name) != 0 ||
            snprintf(temp_path, sizeof(temp_path), "%s/.%s.tmp", directory, id) >=
                (int)sizeof(temp_path))
            return -1;
        if (lstat(final_path, &info) == 0)
            continue;
        fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 && errno != EEXIST)
            return -1;
    }
    if (fd < 0)
        return -1;

    le_voice_clip_wav_header(header, count);
    if (write_all(fd, header, sizeof(header)) != 0 ||
        write_all(fd, samples, count * sizeof(samples[0])) != 0 ||
        fsync(fd) != 0) {
        close(fd);
        (void)unlink(temp_path);
        return -1;
    }
    if (close(fd) != 0 || rename(temp_path, final_path) != 0) {
        (void)unlink(temp_path);
        return -1;
    }
    return 1;
}

void le_voice_clip_wav_header(unsigned char header[44], size_t count)
{
    unsigned long data_bytes = (unsigned long)(count * 2U);
    unsigned long riff_bytes = 36UL + data_bytes;
    unsigned long rate = LE_VOICE_CLIP_SAMPLE_RATE;
    unsigned long byte_rate = rate * 2UL;

    memcpy(header, "RIFF", 4);
    header[4] = (unsigned char)(riff_bytes & 0xffU);
    header[5] = (unsigned char)((riff_bytes >> 8) & 0xffU);
    header[6] = (unsigned char)((riff_bytes >> 16) & 0xffU);
    header[7] = (unsigned char)((riff_bytes >> 24) & 0xffU);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;           /* PCM */
    header[22] = 1; header[23] = 0;           /* mono */
    header[24] = (unsigned char)(rate & 0xffU);
    header[25] = (unsigned char)((rate >> 8) & 0xffU);
    header[26] = 0; header[27] = 0;
    header[28] = (unsigned char)(byte_rate & 0xffU);
    header[29] = (unsigned char)((byte_rate >> 8) & 0xffU);
    header[30] = (unsigned char)((byte_rate >> 16) & 0xffU);
    header[31] = (unsigned char)((byte_rate >> 24) & 0xffU);
    header[32] = 2; header[33] = 0;           /* block align */
    header[34] = 16; header[35] = 0;          /* bits per sample */
    memcpy(header + 36, "data", 4);
    header[40] = (unsigned char)(data_bytes & 0xffU);
    header[41] = (unsigned char)((data_bytes >> 8) & 0xffU);
    header[42] = (unsigned char)((data_bytes >> 16) & 0xffU);
    header[43] = (unsigned char)((data_bytes >> 24) & 0xffU);
}
