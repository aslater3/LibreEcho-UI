#include "sound_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned read16(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long read32(const unsigned char *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 |
           (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}
static int16_t sample_at(const unsigned char *data, unsigned long frame, unsigned channels)
{
    const unsigned char *p = data + frame * channels * 2UL;
    long value = (int16_t)read16(p);

    if (channels == 2)
        value = (value + (int16_t)read16(p + 2)) / 2;
    return (int16_t)value;
}

const char *le_sound_result_message(int result)
{
    switch (result) {
    case LE_SOUND_OK: return "ok";
    case LE_SOUND_NOT_WAV: return "The file is not a WAV file";
    case LE_SOUND_UNSUPPORTED: return "The WAV file must be 16-bit PCM, mono or stereo, 8000-48000 Hz";
    case LE_SOUND_EMPTY: return "The WAV file holds no audio";
    case LE_SOUND_TOO_LONG: return "The sound is longer than this event allows";
    default: return "The sound could not be read";
    }
}

int le_sound_wav_convert(const unsigned char *in, size_t length,
                         unsigned max_ms, int16_t *out, size_t *out_samples)
{
    size_t offset = 12;
    const unsigned char *data = NULL;
    unsigned long data_bytes = 0, rate = 0, frames, count, i;
    unsigned channels = 0, bits = 0, format = 0;
    int have_format = 0;

    *out_samples = 0;
    if (!in || length < 12 || memcmp(in, "RIFF", 4) || memcmp(in + 8, "WAVE", 4))
        return LE_SOUND_NOT_WAV;
    /* Walk the chunks inside the bytes actually received; a size field is
       never trusted past the end of the buffer. */
    while (offset + 8 <= length) {
        const unsigned char *chunk = in + offset;
        unsigned long size = read32(chunk + 4);
        size_t available = length - offset - 8;

        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < 16 || size > available)
                return LE_SOUND_NOT_WAV;
            format = read16(chunk + 8);
            channels = read16(chunk + 10);
            rate = read32(chunk + 12);
            bits = read16(chunk + 22);
            /* WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes
               of the sub-format GUID. */
            if (format == 0xFFFEu) {
                if (size < 40)
                    return LE_SOUND_UNSUPPORTED;
                format = read16(chunk + 8 + 24);
            }
            have_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!have_format)
                return LE_SOUND_NOT_WAV;
            data = chunk + 8;
            /* Streamed writers leave the size unset (0xFFFFFFFF), and a
               truncated file claims more than it holds: take what arrived. */
            data_bytes = size > available ? (unsigned long)available : size;
            break;
        }
        if (size > available)
            return LE_SOUND_NOT_WAV;
        offset += 8 + (size_t)size + (size & 1UL);
    }
    if (!have_format || !data)
        return LE_SOUND_NOT_WAV;
    if (format != 1 || bits != 16 || channels < 1 || channels > 2 ||
        rate < 8000 || rate > LE_EVENT_SOUND_RATE)
        return LE_SOUND_UNSUPPORTED;
    frames = data_bytes / (channels * 2UL);
    if (frames == 0)
        return LE_SOUND_EMPTY;
    /* Length is judged on the source so an over-long file is refused before
       any conversion work, and the output bound below then always holds. */
    if (frames * 1000UL > (unsigned long)max_ms * rate)
        return LE_SOUND_TOO_LONG;
    count = (unsigned long)(((unsigned long long)frames * LE_EVENT_SOUND_RATE) / rate);
    if (count == 0)
        count = 1;
    if (count > LE_EVENT_SOUND_MAX_SAMPLES ||
        count > (unsigned long)max_ms * (LE_EVENT_SOUND_RATE / 1000))
        return LE_SOUND_TOO_LONG;
    for (i = 0; i < count; ++i) {
        /* Position in source frames, 16.16 fixed point. */
        unsigned long long position = ((unsigned long long)i * rate << 16) / LE_EVENT_SOUND_RATE;
        unsigned long index = (unsigned long)(position >> 16);
        long fraction = (long)(position & 0xFFFFu);
        long a = sample_at(data, index < frames ? index : frames - 1, channels);
        long b = sample_at(data, index + 1 < frames ? index + 1 : frames - 1, channels);

        out[i] = (int16_t)(a + ((b - a) * fraction) / 65536L);
    }
    *out_samples = count;
    return LE_SOUND_OK;
}

static int sound_path(char *out, size_t size, const char *dir, int index, const char *suffix)
{
    int n;

    if (index < 0 || index >= LE_EVENT_SOUND_COUNT)
        return -1;
    n = snprintf(out, size, "%s/" LE_EVENT_SOUND_PREFIX "%s%s", dir, le_event_sound_slots[index].id, suffix);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int sync_directory(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    int rc;

    if (fd < 0)
        return -1;
    rc = fsync(fd);
    close(fd);
    return rc;
}

static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);

        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int le_sound_store_write(const char *dir, int index, const int16_t *samples, size_t count)
{
    char path[320], temp[336], saved[336];
    unsigned char bytes[4096];
    size_t done = 0;
    int fd;

    if (!count || count > LE_EVENT_SOUND_MAX_SAMPLES ||
        count * 2UL > le_event_sound_max_bytes(index) ||
        sound_path(path, sizeof(path), dir, index, ".raw") ||
        sound_path(temp, sizeof(temp), dir, index, ".raw.tmp") ||
        sound_path(saved, sizeof(saved), dir, index, ".raw.saved"))
        return -1;
    if (mkdir(dir, 0700) && errno != EEXIST)
        return -1;
    fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    while (done < count) {
        size_t n = 0;

        while (done < count && n + 2 <= sizeof(bytes)) {
            unsigned value = (uint16_t)samples[done++];
            bytes[n++] = (unsigned char)(value & 0xFFu);
            bytes[n++] = (unsigned char)(value >> 8);
        }
        if (write_all(fd, bytes, n))
            goto fail;
    }
    if (fsync(fd)) goto fail;
    if (close(fd)) { fd = -1; goto fail; }
    fd = -1;
    if (rename(temp, path)) goto fail;
    /* A new upload replaces whatever the owner had kept for this event. */
    (void)unlink(saved);
    (void)sync_directory(dir);
    return 0;
fail:
    if (fd >= 0)
        close(fd);
    unlink(temp);
    return -1;
}

static int exists(const char *path, off_t *size)
{
    struct stat st;

    if (lstat(path, &st) || !S_ISREG(st.st_mode))
        return 0;
    if (size)
        *size = st.st_size;
    return 1;
}

int le_sound_store_select(const char *dir, int index, int custom)
{
    char path[320], saved[336];

    if (sound_path(path, sizeof(path), dir, index, ".raw") ||
        sound_path(saved, sizeof(saved), dir, index, ".raw.saved"))
        return -1;
    if (custom) {
        if (exists(path, NULL))
            return 0;
        if (!exists(saved, NULL))
            return 1;
        if (rename(saved, path))
            return -1;
    } else {
        if (!exists(path, NULL))
            return 0;
        if (rename(path, saved))
            return -1;
    }
    (void)sync_directory(dir);
    return 0;
}

int le_sound_store_uploaded(const char *dir, int index)
{
    char path[320], saved[336];

    if (sound_path(path, sizeof(path), dir, index, ".raw") ||
        sound_path(saved, sizeof(saved), dir, index, ".raw.saved"))
        return 0;
    return exists(path, NULL) || exists(saved, NULL);
}

int le_sound_store_delete(const char *dir, int index)
{
    char path[320], saved[336];
    int rc = 0;

    if (sound_path(path, sizeof(path), dir, index, ".raw") ||
        sound_path(saved, sizeof(saved), dir, index, ".raw.saved"))
        return -1;
    if (unlink(path) && errno != ENOENT)
        rc = -1;
    if (unlink(saved) && errno != ENOENT)
        rc = -1;
    (void)sync_directory(dir);
    return rc;
}

int le_sound_store_json(const char *dir, char *out, size_t size)
{
    size_t used;
    int i, n;

    n = snprintf(out, size, "{\"rate\":%d,\"format\":\"wav\",\"max_upload_bytes\":%u,\"sounds\":[",
                 LE_EVENT_SOUND_RATE, LE_SOUND_UPLOAD_MAX_BYTES);
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;
    for (i = 0; i < LE_EVENT_SOUND_COUNT; ++i) {
        char path[320], saved[336];
        off_t bytes = 0;
        int selected, kept;

        if (sound_path(path, sizeof(path), dir, i, ".raw") ||
            sound_path(saved, sizeof(saved), dir, i, ".raw.saved"))
            return -1;
        selected = exists(path, &bytes);
        kept = !selected && exists(saved, &bytes);
        n = snprintf(out + used, size - used,
                     "%s{\"id\":\"%s\",\"name\":\"%s\",\"selected\":\"%s\",\"uploaded\":%s,"
                     "\"duration_ms\":%ld,\"max_ms\":%u,\"default\":\"%s\"}",
                     i ? "," : "", le_event_sound_slots[i].id, le_event_sound_slots[i].name,
                     selected ? "custom" : "default", selected || kept ? "true" : "false",
                     selected || kept ? (long)(bytes / 2 * 1000 / LE_EVENT_SOUND_RATE) : 0L,
                     le_event_sound_slots[i].max_ms, le_event_sound_slots[i].fallback);
        if (n < 0 || (size_t)n >= size - used)
            return -1;
        used += (size_t)n;
    }
    n = snprintf(out + used, size - used, "]}");
    return n < 0 || (size_t)n >= size - used ? -1 : 0;
}
