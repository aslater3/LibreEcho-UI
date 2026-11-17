#define _POSIX_C_SOURCE 200809L
#include "adapter/voice_clip_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            ++failures;                                                \
        }                                                              \
    } while (0)

static char root[256];

static void make_root(void)
{
    const char *base = getenv("TMPDIR");

    snprintf(root, sizeof(root), "%s/voice-clips-%ld",
             base && base[0] ? base : "/tmp", (long)getpid());
    (void)mkdir(root, 0700);
}

static void remove_tree(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *item;

    if (!dir)
        return;
    while ((item = readdir(dir)) != NULL) {
        char path[512];

        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
            continue;
        snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
        (void)unlink(path);
    }
    closedir(dir);
    (void)rmdir(directory);
}

static int file_mode(const char *path, mode_t *mode)
{
    struct stat info;

    if (stat(path, &info) != 0)
        return -1;
    *mode = info.st_mode & 07777;
    return 0;
}

static void test_id_validation(void)
{
    CHECK(le_voice_clip_id_valid("near-1700000000-0001"), "canonical id accepted");
    CHECK(le_voice_clip_id_valid("turn-1700000000-9999"), "turn kind accepted");
    CHECK(!le_voice_clip_id_valid(NULL), "NULL id rejected");
    CHECK(!le_voice_clip_id_valid(""), "empty id rejected");
    CHECK(!le_voice_clip_id_valid("../near-1700000000-0001"), "traversal rejected");
    CHECK(!le_voice_clip_id_valid("near-1700000000-0001/x"), "slash rejected");
    CHECK(!le_voice_clip_id_valid("Near-1700000000-0001"), "uppercase kind rejected");
    CHECK(!le_voice_clip_id_valid("near-170000000-0001"), "short seconds rejected");
    CHECK(!le_voice_clip_id_valid("near-1700000000-001"), "short sequence rejected");
    CHECK(!le_voice_clip_id_valid("near-1700000000-0001.wav"), "extension rejected");
    CHECK(!le_voice_clip_id_valid("near1700000000-0001"), "missing dash rejected");
}

static void test_settings_default_off(const char *directory)
{
    struct le_voice_clip_settings settings;

    CHECK(le_voice_clip_settings_load(directory, &settings) == 0, "missing settings loads");
    CHECK(settings.near_miss_enabled == 0, "capture is off by default");

    settings.near_miss_enabled = 1;
    CHECK(le_voice_clip_settings_save(directory, &settings) == 0, "settings save");
    CHECK(le_voice_clip_settings_load(directory, &settings) == 0, "settings reload");
    CHECK(settings.near_miss_enabled == 1, "enabled setting round-trips");

    settings.near_miss_enabled = 0;
    CHECK(le_voice_clip_settings_save(directory, &settings) == 0, "settings disable");
    CHECK(le_voice_clip_settings_load(directory, &settings) == 0, "settings reload off");
    CHECK(settings.near_miss_enabled == 0, "disable round-trips");
}

static void test_write_read_delete(const char *directory)
{
    int16_t samples[1280];
    struct le_voice_clip_entry entries[LE_VOICE_CLIP_LIST_MAX];
    unsigned total = 0;
    char path[512];
    unsigned char header[44];
    FILE *file;
    mode_t mode = 0;
    size_t i;
    int listed;

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        samples[i] = (int16_t)(i * 3);

    CHECK(le_voice_clip_write(directory, "near", samples, 1280, 1700000000LL) == 1,
          "write clip");
    CHECK(le_voice_clip_write(directory, "near", samples, 0, 1700000000LL) == -1,
          "empty clip rejected");
    CHECK(le_voice_clip_write(directory, "Near", samples, 1280, 1700000000LL) == -1,
          "uppercase kind rejected");
    CHECK(le_voice_clip_write(directory, "near", samples, 1280, -1LL) == -1,
          "negative time rejected");
    CHECK(le_voice_clip_write(directory, "near", samples,
                              LE_VOICE_CLIP_MAX_SAMPLES + 1U, 1700000000LL) == -1,
          "oversize clip rejected, not truncated");

    listed = le_voice_clip_list(directory, entries, LE_VOICE_CLIP_LIST_MAX, &total);
    CHECK(listed == 1, "one clip listed");
    CHECK(listed == 1 && strcmp(entries[0].id, "near-1700000000-0001") == 0,
          "listed id has canonical sequence");
    CHECK(listed == 1 && entries[0].bytes == 44U + 1280U * 2U, "listed size is exact");
    CHECK(total == 44U + 1280U * 2U, "total bytes match");

    snprintf(path, sizeof(path), "%s/near-1700000000-0001.wav", directory);
    CHECK(file_mode(path, &mode) == 0 && mode == 0600, "clip is mode 0600");
    file = fopen(path, "rb");
    CHECK(file != NULL, "clip file opens");
    if (file) {
        CHECK(fread(header, 1, sizeof(header), file) == sizeof(header), "header reads");
        fclose(file);
        CHECK(memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVEfmt ", 8) == 0,
              "RIFF/WAVE header present");
        CHECK(header[24] == 0x80 && header[25] == 0x3e && header[22] == 1,
              "16 kHz mono header");
        CHECK(header[34] == 16 && memcmp(header + 36, "data", 4) == 0,
              "16-bit data chunk");
    }

    CHECK(le_voice_clip_path(directory, "near-1700000000-0001", path, sizeof(path)) == 0,
          "path resolves for a stored id");
    CHECK(le_voice_clip_path(directory, "../etc", path, sizeof(path)) == -1,
          "path rejects traversal");
    CHECK(le_voice_clip_path(directory, "near-1700000000-0002", path, sizeof(path)) == -1,
          "path rejects missing id");

    CHECK(le_voice_clip_delete(directory, "near-1700000000-0001") == 1, "delete stored clip");
    CHECK(le_voice_clip_delete(directory, "near-1700000000-0001") == 0,
          "deleting an absent clip is a no-op");
    CHECK(le_voice_clip_delete(directory, "../x") == -1, "delete rejects traversal");
}

static void test_retention_bounds(const char *directory)
{
    static int16_t big[LE_VOICE_CLIP_MAX_SAMPLES];
    static struct le_voice_clip_entry entries[LE_VOICE_CLIP_LIST_MAX];
    unsigned total = 0;
    int listed, written, i;

    memset(big, 0, sizeof(big));
    for (i = 0; i < (int)LE_VOICE_CLIP_MAX_COUNT + 5; ++i) {
        written = le_voice_clip_write(directory, "near", big, 1280, 1700000100LL + i);
        CHECK(written == 1, "bounded write succeeds");
    }
    listed = le_voice_clip_list(directory, entries, LE_VOICE_CLIP_LIST_MAX, &total);
    CHECK(listed == (int)LE_VOICE_CLIP_MAX_COUNT, "count bound holds at 32 clips");
    /* 37 writes against a 32-clip bound evict the first five, so the oldest
     * survivor is the sixth write, five seconds after the first. */
    CHECK(listed > 0 && entries[0].created == 1700000105LL,
          "oldest clips were evicted first");
    CHECK(total <= LE_VOICE_CLIP_MAX_BYTES, "byte bound holds");

    /* Larger clips push the byte bound: each is about 64 KiB, so the 4 MiB
     * ceiling is reached well before the count ceiling. */
    for (i = 0; i < 70; ++i) {
        written = le_voice_clip_write(directory, "turn", big,
                                      LE_VOICE_CLIP_MAX_SAMPLES, 1700001000LL + i);
        CHECK(written == 1, "large write succeeds after eviction");
    }
    listed = le_voice_clip_list(directory, entries, LE_VOICE_CLIP_LIST_MAX, &total);
    CHECK(total <= LE_VOICE_CLIP_MAX_BYTES, "byte bound holds for large clips");
    CHECK(listed > 0 && listed <= (int)LE_VOICE_CLIP_MAX_COUNT, "list is bounded");

    CHECK(le_voice_clip_clear(directory) > 0, "clear removes clips");
    listed = le_voice_clip_list(directory, entries, LE_VOICE_CLIP_LIST_MAX, &total);
    CHECK(listed == 0 && total == 0, "store is empty after clear");
}

static void test_directory_override(void)
{
    setenv(LE_VOICE_CLIP_ENV, "/tmp/override-check", 1);
    CHECK(strcmp(le_voice_clip_directory(), "/tmp/override-check") == 0,
          "environment override honoured");
    unsetenv(LE_VOICE_CLIP_ENV);
    CHECK(strcmp(le_voice_clip_directory(), LE_VOICE_CLIP_DEFAULT_DIR) == 0,
          "device default used without override");
}

static void test_missing_directory_lists_empty(void)
{
    struct le_voice_clip_entry entries[4];
    unsigned total = 99;

    CHECK(le_voice_clip_list("/nonexistent-voice-clip-dir", entries, 4, &total) == 0,
          "missing directory lists as empty");
    CHECK(total == 0, "missing directory reports zero bytes");
}

int main(void)
{
    make_root();
    test_id_validation();
    test_directory_override();
    test_missing_directory_lists_empty();
    test_settings_default_off(root);
    test_write_read_delete(root);
    test_retention_bounds(root);
    remove_tree(root);
    if (failures) {
        fprintf(stderr, "voice clip store: %d failure(s)\n", failures);
        return 1;
    }
    printf("voice clip store: ok\n");
    return 0;
}
