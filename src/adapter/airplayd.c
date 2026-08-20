/* LibreEcho AirPlay 2 integration controller.
 *
 * The controller is started with the other local daemons, but AirPlay itself
 * is deliberately stopped until the user enables the integration.  NQPTP is
 * started before Shairport Sync because AirPlay 2 uses it for PTP timing.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "adapter.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INPUT_MAX LE_ADAPTER_MSG_MAX
#define AIRPLAY_METADATA_FIFO "/run/libreecho-audio/airplay.metadata"
#define AIRPLAY_METADATA_ITEM_MAX 8192
#define AIRPLAY_METADATA_FIELD_MAX 192
#define AIRPLAY_METADATA_AP2_PLIST_MAX 16384
#define AIRPLAY_METADATA_READ_MAX 16

struct airplay_metadata_parser {
    char item[AIRPLAY_METADATA_ITEM_MAX + 1];
    size_t used;
};

struct airplay_ctx {
    int listener;
    int metadata_fd;
    char socket_path[128];
    char metadata_path[128];
    char runtime_root[128];
    char nqptp_path[128];
    char shairport_path[128];
    char avahi_path[128];
    char dbus_path[128];
    char audio_path[128];
    char engine_path[128];
    char config_path[128];
    pid_t dbus_pid;
    pid_t avahi_pid;
    pid_t nqptp_pid;
    pid_t audio_pid;
    pid_t engine_pid;
    pid_t shairport_pid;
    int enabled;
    int playing;
    char title[AIRPLAY_METADATA_FIELD_MAX + 1];
    char artist[AIRPLAY_METADATA_FIELD_MAX + 1];
    char album[AIRPLAY_METADATA_FIELD_MAX + 1];
    struct airplay_metadata_parser metadata_parser;
};

static volatile sig_atomic_t running = 1;

#define AIRPLAY_CHROOT "/bin/busybox"

static void on_signal(int signo)
{
    if (signo == SIGTERM || signo == SIGINT)
        running = 0;
}

static int json_bool(const char *json, const char *key, int *value)
{
    char needle[64];
    const char *p;
    int n;

    n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;
    p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p + n, ':');
    if (!p)
        return -1;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (!strncmp(p, "true", 4)) {
        *value = 1;
        return 0;
    }
    if (!strncmp(p, "false", 5)) {
        *value = 0;
        return 0;
    }
    return -1;
}

static const char *bounded_find(const char *haystack, size_t haystack_length,
                                const char *needle, size_t needle_length)
{
    size_t i;

    if (!haystack || !needle || needle_length == 0 ||
        needle_length > haystack_length)
        return NULL;
    for (i = 0; i + needle_length <= haystack_length; ++i)
        if (!memcmp(haystack + i, needle, needle_length))
            return haystack + i;
    return NULL;
}

static void metadata_clear_fields(struct airplay_ctx *ctx)
{
    if (!ctx)
        return;
    ctx->playing = 0;
    ctx->title[0] = '\0';
    ctx->artist[0] = '\0';
    ctx->album[0] = '\0';
}

static void metadata_reset(struct airplay_ctx *ctx)
{
    if (!ctx)
        return;
    metadata_clear_fields(ctx);
    ctx->metadata_parser.used = 0;
    ctx->metadata_parser.item[0] = '\0';
}

static int metadata_element(const char *item, size_t item_length,
                            const char *name, const char **value,
                            size_t *value_length)
{
    char opening[32];
    char closing[32];
    const char *start;
    const char *end;
    int opening_length;
    int closing_length;
    size_t remaining;

    opening_length = snprintf(opening, sizeof(opening), "<%s>", name);
    closing_length = snprintf(closing, sizeof(closing), "</%s>", name);
    if (opening_length < 0 || closing_length < 0 ||
        (size_t)opening_length >= sizeof(opening) ||
        (size_t)closing_length >= sizeof(closing))
        return -1;
    start = bounded_find(item, item_length, opening, (size_t)opening_length);
    if (!start)
        return -1;
    start += opening_length;
    remaining = item_length - (size_t)(start - item);
    end = bounded_find(start, remaining, closing, (size_t)closing_length);
    if (!end)
        return -1;
    *value = start;
    *value_length = (size_t)(end - start);
    return 0;
}

static void trim_ascii_space(const char **value, size_t *length)
{
    while (*length > 0 &&
           (**value == ' ' || **value == '\t' ||
            **value == '\r' || **value == '\n')) {
        ++*value;
        --*length;
    }
    while (*length > 0) {
        char c = (*value)[*length - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        --*length;
    }
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int metadata_fourcc(const char *value, size_t length, char code[5])
{
    size_t i;

    trim_ascii_space(&value, &length);
    if (length == 4) {
        for (i = 0; i < 4; ++i)
            if ((unsigned char)value[i] < 0x20 ||
                (unsigned char)value[i] > 0x7e)
                return -1;
        memcpy(code, value, 4);
        code[4] = '\0';
        return 0;
    }
    if (length != 8)
        return -1;
    for (i = 0; i < 4; ++i) {
        int high = hex_nibble(value[i * 2]);
        int low = hex_nibble(value[i * 2 + 1]);
        if (high < 0 || low < 0)
            return -1;
        code[i] = (char)((high << 4) | low);
        if ((unsigned char)code[i] < 0x20 ||
            (unsigned char)code[i] > 0x7e)
            return -1;
    }
    code[4] = '\0';
    return 0;
}

static int metadata_length(const char *value, size_t length, size_t *result)
{
    size_t parsed = 0;
    size_t i;

    trim_ascii_space(&value, &length);
    if (length == 0)
        return -1;
    for (i = 0; i < length; ++i) {
        unsigned int digit;
        if (value[i] < '0' || value[i] > '9')
            return -1;
        digit = (unsigned int)(value[i] - '0');
        if (parsed > (AIRPLAY_METADATA_ITEM_MAX - digit) / 10)
            return -1;
        parsed = parsed * 10 + digit;
    }
    *result = parsed;
    return 0;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

static int metadata_base64_decode(const char *input, size_t input_length,
                                  unsigned char *output, size_t output_size,
                                  size_t *output_length)
{
    int quartet[4];
    size_t used = 0;
    size_t quartet_used = 0;
    size_t i;
    int finished = 0;

    for (i = 0; i < input_length; ++i) {
        int value;
        char c = input[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (finished)
            return -1;
        value = c == '=' ? -2 : base64_value(c);
        if (value < 0 && value != -2)
            return -1;
        quartet[quartet_used++] = value;
        if (quartet_used != 4)
            continue;
        if (quartet[0] < 0 || quartet[1] < 0 ||
            (quartet[2] == -2 && quartet[3] != -2))
            return -1;
        if (used >= output_size)
            return -1;
        output[used++] = (unsigned char)((quartet[0] << 2) |
                                        (quartet[1] >> 4));
        if (quartet[2] == -2) {
            finished = 1;
        } else {
            if (used >= output_size)
                return -1;
            output[used++] = (unsigned char)((quartet[1] << 4) |
                                            (quartet[2] >> 2));
            if (quartet[3] == -2) {
                finished = 1;
            } else {
                if (quartet[3] < 0 || used >= output_size)
                    return -1;
                output[used++] = (unsigned char)((quartet[2] << 6) |
                                                quartet[3]);
            }
        }
        quartet_used = 0;
    }
    if (quartet_used != 0)
        return -1;
    *output_length = used;
    return 0;
}

static int metadata_data(const char *item, size_t item_length,
                         const char **value, size_t *value_length)
{
    static const char data_start[] = "<data";
    static const char data_end[] = "</data>";
    static const char encoding_double[] = "encoding=\"base64\"";
    static const char encoding_single[] = "encoding='base64'";
    const char *start;
    const char *opening_end;
    const char *end;
    size_t remaining;
    size_t opening_length;

    start = bounded_find(item, item_length, data_start,
                         sizeof(data_start) - 1);
    if (!start)
        return -1;
    remaining = item_length - (size_t)(start - item);
    opening_end = bounded_find(start, remaining, ">", 1);
    if (!opening_end)
        return -1;
    opening_length = (size_t)(opening_end - start);
    if (!bounded_find(start, opening_length, encoding_double,
                      sizeof(encoding_double) - 1) &&
        !bounded_find(start, opening_length, encoding_single,
                      sizeof(encoding_single) - 1))
        return -1;
    ++opening_end;
    remaining = item_length - (size_t)(opening_end - item);
    end = bounded_find(opening_end, remaining, data_end,
                       sizeof(data_end) - 1);
    if (!end)
        return -1;
    *value = opening_end;
    *value_length = (size_t)(end - opening_end);
    return 0;
}

static int metadata_valid_utf8(const unsigned char *text, size_t length)
{
    size_t i = 0;

    while (i < length) {
        unsigned char c = text[i++];
        if (c == 0)
            return 0;
        if (c < 0x80)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            if (i >= length || text[i] < 0x80 || text[i] > 0xbf)
                return 0;
            ++i;
        } else if (c == 0xe0) {
            if (i + 1 >= length || text[i] < 0xa0 || text[i] > 0xbf ||
                text[i + 1] < 0x80 || text[i + 1] > 0xbf)
                return 0;
            i += 2;
        } else if ((c >= 0xe1 && c <= 0xec) ||
                   (c >= 0xee && c <= 0xef)) {
            if (i + 1 >= length || text[i] < 0x80 || text[i] > 0xbf ||
                text[i + 1] < 0x80 || text[i + 1] > 0xbf)
                return 0;
            i += 2;
        } else if (c == 0xed) {
            if (i + 1 >= length || text[i] < 0x80 || text[i] > 0x9f ||
                text[i + 1] < 0x80 || text[i + 1] > 0xbf)
                return 0;
            i += 2;
        } else if (c == 0xf0) {
            if (i + 2 >= length || text[i] < 0x90 || text[i] > 0xbf ||
                text[i + 1] < 0x80 || text[i + 1] > 0xbf ||
                text[i + 2] < 0x80 || text[i + 2] > 0xbf)
                return 0;
            i += 3;
        } else if (c >= 0xf1 && c <= 0xf3) {
            if (i + 2 >= length || text[i] < 0x80 || text[i] > 0xbf ||
                text[i + 1] < 0x80 || text[i + 1] > 0xbf ||
                text[i + 2] < 0x80 || text[i + 2] > 0xbf)
                return 0;
            i += 3;
        } else if (c == 0xf4) {
            if (i + 2 >= length || text[i] < 0x80 || text[i] > 0x8f ||
                text[i + 1] < 0x80 || text[i + 1] > 0xbf ||
                text[i + 2] < 0x80 || text[i + 2] > 0xbf)
                return 0;
            i += 3;
        } else {
            return 0;
        }
    }
    return 1;
}

struct bplist {
    const unsigned char *data;
    size_t length;
    size_t offset_table;
    uint8_t offset_size;
    uint8_t ref_size;
    uint64_t objects;
    uint64_t top;
};

static uint64_t read_be(const unsigned char *data, size_t length)
{
    uint64_t value = 0;
    size_t i;

    for (i = 0; i < length; ++i)
        value = (value << 8) | data[i];
    return value;
}

static int bplist_init(struct bplist *plist, const unsigned char *data,
                       size_t length)
{
    const unsigned char *trailer;
    uint64_t offset_table;
    uint64_t objects;

    if (!plist || !data || length < 40 || memcmp(data, "bplist00", 8))
        return -1;
    trailer = data + length - 32;
    objects = read_be(trailer + 8, 8);
    offset_table = read_be(trailer + 24, 8);
    if (trailer[6] < 1 || trailer[6] > 8 ||
        trailer[7] < 1 || trailer[7] > 8 ||
        objects == 0 || objects > 4096 ||
        offset_table >= length ||
        objects > (length - offset_table) / trailer[6])
        return -1;
    plist->data = data;
    plist->length = length;
    plist->offset_table = (size_t)offset_table;
    plist->offset_size = trailer[6];
    plist->ref_size = trailer[7];
    plist->objects = objects;
    plist->top = read_be(trailer + 16, 8);
    return plist->top < plist->objects ? 0 : -1;
}

static int bplist_offset(const struct bplist *plist, uint64_t object,
                         size_t *offset)
{
    size_t table_offset;
    uint64_t value;

    if (!plist || object >= plist->objects || !offset)
        return -1;
    table_offset = plist->offset_table + (size_t)object * plist->offset_size;
    if (table_offset > plist->length ||
        plist->offset_size > plist->length - table_offset)
        return -1;
    value = read_be(plist->data + table_offset, plist->offset_size);
    if (value >= plist->length)
        return -1;
    *offset = (size_t)value;
    return 0;
}

static int bplist_object_length(const struct bplist *plist, size_t *offset,
                                unsigned char info, uint64_t *length)
{
    size_t pos = *offset;
    unsigned char marker;
    unsigned char bytes;

    if (info < 0x0f) {
        *length = info;
        return 0;
    }
    if (pos >= plist->length)
        return -1;
    marker = plist->data[pos++];
    if ((marker >> 4) != 0x1)
        return -1;
    bytes = (unsigned char)1u << (marker & 0x0f);
    if (bytes > 8 || bytes > plist->length - pos)
        return -1;
    *length = read_be(plist->data + pos, bytes);
    *offset = pos + bytes;
    return 0;
}

static int utf8_put(char *out, size_t out_size, size_t *used, uint32_t code)
{
    unsigned char bytes[3];
    size_t count;

    if (code == 0 || (code >= 0xd800 && code <= 0xdfff))
        return -1;
    if (code < 0x80) {
        bytes[0] = (unsigned char)code;
        count = 1;
    } else if (code < 0x800) {
        bytes[0] = (unsigned char)(0xc0 | (code >> 6));
        bytes[1] = (unsigned char)(0x80 | (code & 0x3f));
        count = 2;
    } else if (code < 0x10000) {
        bytes[0] = (unsigned char)(0xe0 | (code >> 12));
        bytes[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (code & 0x3f));
        count = 3;
    } else {
        return -1;
    }
    if (*used + count >= out_size)
        return -1;
    memcpy(out + *used, bytes, count);
    *used += count;
    out[*used] = '\0';
    return 0;
}

static int bplist_copy_string(const struct bplist *plist, uint64_t object,
                              char *out, size_t out_size)
{
    size_t offset;
    size_t pos;
    uint64_t length;
    uint64_t i;
    unsigned char marker;
    unsigned char kind;
    size_t used = 0;

    if (!out || out_size == 0 ||
        bplist_offset(plist, object, &offset) < 0)
        return -1;
    marker = plist->data[offset++];
    kind = marker >> 4;
    if (kind != 0x5 && kind != 0x6)
        return -1;
    if (bplist_object_length(plist, &offset, marker & 0x0f, &length) < 0)
        return -1;
    pos = offset;
    if (kind == 0x5) {
        if (length >= out_size || length > plist->length - pos)
            return -1;
        memcpy(out, plist->data + pos, (size_t)length);
        out[length] = '\0';
        return metadata_valid_utf8((const unsigned char *)out,
                                   (size_t)length) ? 0 : -1;
    }
    if (length > (plist->length - pos) / 2)
        return -1;
    for (i = 0; i < length; ++i) {
        uint32_t code = (uint32_t)read_be(plist->data + pos + i * 2, 2);
        if (utf8_put(out, out_size, &used, code) < 0)
            return -1;
    }
    return 0;
}

static int bplist_dict_get(const struct bplist *plist, uint64_t dict,
                           const char *key, uint64_t *value)
{
    size_t offset;
    size_t keys;
    uint64_t count;
    uint64_t i;
    unsigned char marker;
    char name[96];

    if (bplist_offset(plist, dict, &offset) < 0)
        return -1;
    marker = plist->data[offset++];
    if ((marker >> 4) != 0xd ||
        bplist_object_length(plist, &offset, marker & 0x0f, &count) < 0 ||
        count > 1024)
        return -1;
    keys = offset;
    if (count > (plist->length - keys) / plist->ref_size / 2)
        return -1;
    for (i = 0; i < count; ++i) {
        size_t key_offset = keys + (size_t)i * plist->ref_size;
        size_t value_offset = keys + (size_t)(count + i) * plist->ref_size;
        uint64_t key_object = read_be(plist->data + key_offset,
                                      plist->ref_size);

        if (bplist_copy_string(plist, key_object, name, sizeof(name)) == 0 &&
            !strcmp(name, key)) {
            *value = read_be(plist->data + value_offset, plist->ref_size);
            return *value < plist->objects ? 0 : -1;
        }
    }
    return -1;
}

static void metadata_process_ap2_now_playing(struct airplay_ctx *ctx,
                                             const unsigned char *data,
                                             size_t length)
{
    struct bplist plist;
    uint64_t object;
    uint64_t inner;
    uint64_t fields;
    char type[64];

    if (length > AIRPLAY_METADATA_AP2_PLIST_MAX ||
        bplist_init(&plist, data, length) < 0 ||
        bplist_dict_get(&plist, plist.top, "type", &object) < 0 ||
        bplist_copy_string(&plist, object, type, sizeof(type)) < 0 ||
        strcmp(type, "updateMRNowPlayingInfo") ||
        bplist_dict_get(&plist, plist.top, "params", &inner) < 0 ||
        bplist_dict_get(&plist, inner, "params", &fields) < 0)
        return;

#define COPY_AP2_FIELD(key, target) \
    do { \
        if (bplist_dict_get(&plist, fields, (key), &object) == 0) \
            (void)bplist_copy_string(&plist, object, (target), \
                                     AIRPLAY_METADATA_FIELD_MAX + 1); \
    } while (0)
    COPY_AP2_FIELD("kMRMediaRemoteNowPlayingInfoTitle", ctx->title);
    COPY_AP2_FIELD("kMRMediaRemoteNowPlayingInfoArtist", ctx->artist);
    COPY_AP2_FIELD("kMRMediaRemoteNowPlayingInfoAlbum", ctx->album);
#undef COPY_AP2_FIELD
    if (ctx->title[0] || ctx->artist[0] || ctx->album[0])
        ctx->playing = 1;
}

static int metadata_process_item(struct airplay_ctx *ctx,
                                 const char *item, size_t item_length)
{
    const char *value;
    const char *encoded;
    size_t value_length;
    size_t encoded_length;
    size_t declared_length;
    size_t decoded_length;
    unsigned char decoded[AIRPLAY_METADATA_FIELD_MAX + 1];
    char type[5];
    char code[5];
    char *target;

    if (metadata_element(item, item_length, "type",
                         &value, &value_length) < 0 ||
        metadata_fourcc(value, value_length, type) < 0 ||
        metadata_element(item, item_length, "code",
                         &value, &value_length) < 0 ||
        metadata_fourcc(value, value_length, code) < 0 ||
        metadata_element(item, item_length, "length",
                         &value, &value_length) < 0 ||
        metadata_length(value, value_length, &declared_length) < 0)
        return -1;
    if (!strcmp(type, "ssnc") && !strcmp(code, "pbeg")) {
        metadata_clear_fields(ctx);
        ctx->playing = 1;
        return 0;
    }
    if (!strcmp(type, "ssnc") && !strcmp(code, "pres")) {
        ctx->playing = 1;
        return 0;
    }
    if (!strcmp(type, "ssnc") &&
        (!strcmp(code, "pend") || !strcmp(code, "paus"))) {
        metadata_clear_fields(ctx);
        return 0;
    }
    if (!strcmp(type, "ssnc") && !strcmp(code, "copl")) {
        unsigned char decoded_plist[AIRPLAY_METADATA_AP2_PLIST_MAX];

        if (declared_length <= sizeof(decoded_plist) &&
            metadata_data(item, item_length, &encoded, &encoded_length) == 0 &&
            metadata_base64_decode(encoded, encoded_length, decoded_plist,
                                   sizeof(decoded_plist), &decoded_length) == 0 &&
            decoded_length == declared_length)
            metadata_process_ap2_now_playing(ctx, decoded_plist,
                                             decoded_length);
        return 0;
    }
    if (strcmp(type, "core"))
        return 0;
    if (!strcmp(code, "minm"))
        target = ctx->title;
    else if (!strcmp(code, "asar"))
        target = ctx->artist;
    else if (!strcmp(code, "asal"))
        target = ctx->album;
    else
        return 0;
    if (declared_length > AIRPLAY_METADATA_FIELD_MAX ||
        metadata_data(item, item_length, &encoded, &encoded_length) < 0 ||
        metadata_base64_decode(encoded, encoded_length, decoded,
                               sizeof(decoded), &decoded_length) < 0 ||
        decoded_length != declared_length)
        return -1;
    if (decoded_length > 0 && decoded[decoded_length - 1] == '\0')
        --decoded_length;
    if (!metadata_valid_utf8(decoded, decoded_length))
        return -1;
    memcpy(target, decoded, decoded_length);
    target[decoded_length] = '\0';
    return 0;
}

static size_t metadata_start_suffix(const char *buffer, size_t length)
{
    static const char start[] = "<item>";
    size_t keep;

    for (keep = length < sizeof(start) - 1 ? length : sizeof(start) - 2;
         keep > 0; --keep)
        if (!memcmp(buffer + length - keep, start, keep))
            return keep;
    return 0;
}

static void metadata_parser_feed(struct airplay_ctx *ctx,
                                 const char *input, size_t input_length)
{
    static const char item_start[] = "<item>";
    static const char item_end[] = "</item>";
    size_t i;

    for (i = 0; i < input_length; ++i) {
        struct airplay_metadata_parser *parser = &ctx->metadata_parser;
        const char *start;
        const char *end;
        size_t complete_length;
        size_t keep;

        if (input[i] == '\0') {
            parser->used = 0;
            parser->item[0] = '\0';
            continue;
        }
        if (parser->used >= AIRPLAY_METADATA_ITEM_MAX) {
            parser->used = 0;
            parser->item[0] = '\0';
        }
        parser->item[parser->used++] = input[i];
        parser->item[parser->used] = '\0';
        for (;;) {
            start = bounded_find(parser->item, parser->used, item_start,
                                 sizeof(item_start) - 1);
            if (!start) {
                keep = metadata_start_suffix(parser->item, parser->used);
                if (keep > 0)
                    memmove(parser->item,
                            parser->item + parser->used - keep, keep);
                parser->used = keep;
                parser->item[parser->used] = '\0';
                break;
            }
            if (start != parser->item) {
                parser->used -= (size_t)(start - parser->item);
                memmove(parser->item, start, parser->used);
                parser->item[parser->used] = '\0';
            }
            end = bounded_find(parser->item, parser->used, item_end,
                               sizeof(item_end) - 1);
            if (!end) {
                if (parser->used >= AIRPLAY_METADATA_ITEM_MAX) {
                    parser->used = 0;
                    parser->item[0] = '\0';
                }
                break;
            }
            complete_length = (size_t)(end - parser->item) +
                              sizeof(item_end) - 1;
            (void)metadata_process_item(ctx, parser->item, complete_length);
            parser->used -= complete_length;
            if (parser->used > 0)
                memmove(parser->item, parser->item + complete_length,
                        parser->used);
            parser->item[parser->used] = '\0';
        }
    }
}

static int metadata_fifo_open(struct airplay_ctx *ctx)
{
    struct stat status;
    int flags;

    if (ctx->metadata_fd >= 0)
        return 0;
    if (lstat(ctx->metadata_path, &status) < 0) {
        if (errno != ENOENT || mkfifo(ctx->metadata_path, 0600) < 0)
            return -1;
    } else if (!S_ISFIFO(status.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    ctx->metadata_fd = open(ctx->metadata_path, O_RDWR | O_NONBLOCK);
    if (ctx->metadata_fd < 0)
        return -1;
    flags = fcntl(ctx->metadata_fd, F_GETFD);
    if (flags >= 0)
        (void)fcntl(ctx->metadata_fd, F_SETFD, flags | FD_CLOEXEC);
    return 0;
}

static void metadata_fifo_close(struct airplay_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->metadata_fd >= 0)
        close(ctx->metadata_fd);
    ctx->metadata_fd = -1;
    metadata_reset(ctx);
}

static void metadata_fifo_drain(struct airplay_ctx *ctx)
{
    char buffer[1024];
    int reads;

    for (reads = 0; reads < AIRPLAY_METADATA_READ_MAX; ++reads) {
        ssize_t length = read(ctx->metadata_fd, buffer, sizeof(buffer));
        if (length > 0) {
            metadata_parser_feed(ctx, buffer, (size_t)length);
            continue;
        }
        if (length < 0 && errno == EINTR) {
            --reads;
            continue;
        }
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (length < 0)
            metadata_fifo_close(ctx);
        return;
    }
}

static int json_append_raw(char *buffer, size_t size, size_t *used,
                           const char *text)
{
    size_t length = strlen(text);
    if (*used + length >= size)
        return -1;
    memcpy(buffer + *used, text, length);
    *used += length;
    buffer[*used] = '\0';
    return 0;
}

static int json_append_string(char *buffer, size_t size, size_t *used,
                              const char *text)
{
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)text;

    if (json_append_raw(buffer, size, used, "\"") < 0)
        return -1;
    while (*p) {
        char escaped[7];
        size_t escaped_length;
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
            escaped_length = 2;
        } else if (*p < 0x20) {
            escaped[0] = '\\';
            escaped[1] = 'u';
            escaped[2] = '0';
            escaped[3] = '0';
            escaped[4] = hex[*p >> 4];
            escaped[5] = hex[*p & 0x0f];
            escaped_length = 6;
        } else {
            escaped[0] = (char)*p;
            escaped_length = 1;
        }
        if (*used + escaped_length >= size)
            return -1;
        memcpy(buffer + *used, escaped, escaped_length);
        *used += escaped_length;
        buffer[*used] = '\0';
        ++p;
    }
    return json_append_raw(buffer, size, used, "\"");
}

static int metadata_append_status(const struct airplay_ctx *ctx,
                                  char *buffer, size_t size, size_t *used)
{
    if (json_append_raw(buffer, size, used,
                        ctx->playing ?
                        ",\"playback_state\":\"playing\",\"source\":\"airplay\"" :
                        ",\"playback_state\":\"stopped\"") < 0)
        return -1;
#define APPEND_FIELD(key, field) \
    do { \
        if ((field)[0] && \
            (json_append_raw(buffer, size, used, ",\"" key "\":") < 0 || \
             json_append_string(buffer, size, used, (field)) < 0)) \
            return -1; \
    } while (0)
    APPEND_FIELD("title", ctx->title);
    APPEND_FIELD("artist", ctx->artist);
    APPEND_FIELD("album", ctx->album);
#undef APPEND_FIELD
    return 0;
}

static int child_alive(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static int child_running(pid_t *pid)
{
    int status;
    pid_t result;

    if (!pid || *pid <= 0)
        return 0;
    result = waitpid(*pid, &status, WNOHANG);
    if (result == *pid) {
        *pid = -1;
        return 0;
    }
    return result < 0 ? 0 : child_alive(*pid);
}

static int wait_for_runtime_file(const struct airplay_ctx *ctx,
                                 const char *relative, int attempts)
{
    char path[256];
    int i;

    if (!ctx || !relative || attempts < 1)
        return 0;
    if (snprintf(path, sizeof(path), "%s%s", ctx->runtime_root, relative) < 0)
        return 0;
    for (i = 0; i < attempts; ++i) {
        if (access(path, F_OK) == 0)
            return 1;
        usleep(100000);
    }
    return 0;
}

static int reap(struct airplay_ctx *ctx)
{
    int status;
    pid_t pid;
    int lost = 0;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == ctx->dbus_pid) {
            ctx->dbus_pid = -1;
            lost = 1;
        }
        if (pid == ctx->avahi_pid) {
            ctx->avahi_pid = -1;
            lost = 1;
        }
        if (pid == ctx->nqptp_pid) {
            ctx->nqptp_pid = -1;
            lost = 1;
        }
        if (pid == ctx->audio_pid) {
            ctx->audio_pid = -1;
            lost = 1;
        }
        if (pid == ctx->engine_pid) {
            ctx->engine_pid = -1;
            lost = 1;
        }
        if (pid == ctx->shairport_pid) {
            ctx->shairport_pid = -1;
            lost = 1;
        }
    }
    return lost;
}

static void stop_child(pid_t *pid)
{
    int status;
    int i;

    if (!pid || *pid <= 0)
        return;
    (void)kill(*pid, SIGTERM);
    for (i = 0; i < 20; ++i) {
        if (waitpid(*pid, &status, WNOHANG) == *pid) {
            *pid = -1;
            return;
        }
        usleep(100000);
    }
    (void)kill(*pid, SIGKILL);
    (void)waitpid(*pid, &status, 0);
    *pid = -1;
}

static pid_t spawn_nqptp(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->nqptp_path, (char *)NULL);
    _exit(127);
}

static pid_t spawn_dbus(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->dbus_path, "--nofork", "--nopidfile",
          "--config-file=/etc/dbus-1/system.conf", (char *)NULL);
    _exit(127);
}

static pid_t spawn_avahi(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->avahi_path, "--no-chroot", "--no-drop-root",
          "--no-rlimits", (char *)NULL);
    _exit(127);
}

static pid_t spawn_shairport(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->shairport_path,
          "--configfile", ctx->config_path, (char *)NULL);
    _exit(127);
}

static pid_t spawn_audio(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->audio_path, (char *)NULL);
    _exit(127);
}

static pid_t spawn_engine(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->engine_path, (char *)NULL);
    _exit(127);
}

static int set_enabled(struct airplay_ctx *ctx, int enabled)
{
    int i;
    if (enabled == ctx->enabled && (!enabled ||
                                    (child_alive(ctx->engine_pid) &&
                                     child_alive(ctx->dbus_pid) &&
                                     child_alive(ctx->avahi_pid) &&
                                     child_alive(ctx->nqptp_pid) &&
                                     child_alive(ctx->audio_pid) &&
                                     child_alive(ctx->shairport_pid) &&
                                     ctx->metadata_fd >= 0)))
        return 0;
    if (!enabled) {
        stop_child(&ctx->shairport_pid);
        stop_child(&ctx->audio_pid);
        stop_child(&ctx->nqptp_pid);
        stop_child(&ctx->avahi_pid);
        stop_child(&ctx->dbus_pid);
        ctx->enabled = 0;
        metadata_fifo_close(ctx);
        le_log_info("airplayd: AirPlay 2 disabled");
        return 0;
    }
    char path[256];
#define RUNTIME_ACCESS(relative, mode) \
    (snprintf(path, sizeof(path), "%s%s", ctx->runtime_root, (relative)), \
     access(path, (mode)) < 0)
    if (access(AIRPLAY_CHROOT, X_OK) < 0 ||
        RUNTIME_ACCESS(ctx->dbus_path, X_OK) ||
        RUNTIME_ACCESS(ctx->avahi_path, X_OK) ||
        RUNTIME_ACCESS(ctx->nqptp_path, X_OK) ||
        RUNTIME_ACCESS(ctx->audio_path, X_OK) ||
        RUNTIME_ACCESS(ctx->engine_path, X_OK) ||
        RUNTIME_ACCESS(ctx->shairport_path, X_OK) ||
        RUNTIME_ACCESS(ctx->config_path, R_OK) ||
        !child_alive(ctx->engine_pid)) {
#undef RUNTIME_ACCESS
        return -1;
    }
#undef RUNTIME_ACCESS
    ctx->dbus_pid = spawn_dbus(ctx);
    if (ctx->dbus_pid < 0)
        return -1;
    if (!wait_for_runtime_file(ctx, "/run/dbus/system_bus_socket", 30))
        goto fail;
    ctx->avahi_pid = spawn_avahi(ctx);
    if (ctx->avahi_pid < 0) {
        stop_child(&ctx->dbus_pid);
        return -1;
    }
    for (i = 0; i < 10 && child_running(&ctx->avahi_pid); ++i)
        usleep(100000);
    if (ctx->avahi_pid <= 0)
        goto fail;
    ctx->nqptp_pid = spawn_nqptp(ctx);
    if (ctx->nqptp_pid < 0)
        goto fail;
    if (!wait_for_runtime_file(ctx, "/dev/shm/nqptp", 30))
        goto fail;
    ctx->audio_pid = spawn_audio(ctx);
    if (ctx->audio_pid < 0)
        goto fail;
    metadata_reset(ctx);
    if (metadata_fifo_open(ctx) < 0)
        goto fail;
    ctx->shairport_pid = spawn_shairport(ctx);
    if (ctx->shairport_pid < 0)
        goto fail;
    /* A successful fork is not a successful enable: a child may reject its
     * configuration or a required runtime mount may still be absent. */
    for (i = 0; i < 20; ++i) {
        if (!child_running(&ctx->dbus_pid) || !child_running(&ctx->avahi_pid) ||
            !child_running(&ctx->nqptp_pid) || !child_running(&ctx->audio_pid) ||
            !child_running(&ctx->shairport_pid))
            goto fail;
        usleep(100000);
    }
    ctx->enabled = 1;
    le_log_info("airplayd: AirPlay 2 enabled (D-Bus, Avahi, NQPTP, Shairport Sync)");
    return 0;
fail:
    stop_child(&ctx->shairport_pid);
    metadata_fifo_close(ctx);
    stop_child(&ctx->audio_pid);
    stop_child(&ctx->nqptp_pid);
    stop_child(&ctx->avahi_pid);
    stop_child(&ctx->dbus_pid);
    return -1;
}

static int request(struct airplay_ctx *ctx, char *message,
                   char *response, size_t response_size)
{
    char command[64];
    char *args;
    unsigned long id;
    int enabled;

    if (le_adapter_parse_request(message, command, sizeof(command), &args, &id) < 0)
        return le_adapter_respond_err(response, response_size, 0, "malformed request");
    if (!strcmp(command, "status")) {
        char data[LE_ADAPTER_MSG_MAX - 128];
        char path[256];
        int available = access(AIRPLAY_CHROOT, X_OK) == 0;
        int length;
        size_t used;
#define RUNTIME_AVAILABLE(relative, mode) \
        (snprintf(path, sizeof(path), "%s%s", ctx->runtime_root, (relative)), \
         access(path, (mode)) == 0)
        available = available && child_alive(ctx->engine_pid) &&
                    RUNTIME_AVAILABLE(ctx->dbus_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->avahi_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->nqptp_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->audio_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->engine_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->shairport_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->config_path, R_OK);
#undef RUNTIME_AVAILABLE
        length = snprintf(data, sizeof(data),
                          "{\"available\":%s,\"enabled\":%s,\"engine_running\":%s,\"dbus_running\":%s,\"avahi_running\":%s,\"nqptp_running\":%s,\"audio_running\":%s,\"shairport_running\":%s",
                          available ? "true" : "false",
                          ctx->enabled ? "true" : "false",
                          child_alive(ctx->engine_pid) ? "true" : "false",
                          child_alive(ctx->dbus_pid) ? "true" : "false",
                          child_alive(ctx->avahi_pid) ? "true" : "false",
                          child_alive(ctx->nqptp_pid) ? "true" : "false",
                          child_alive(ctx->audio_pid) ? "true" : "false",
                          child_alive(ctx->shairport_pid) ? "true" : "false");
        if (length < 0 || (size_t)length >= sizeof(data))
            return le_adapter_respond_err(response, response_size, id,
                                          "status response overflow");
        used = (size_t)length;
        if (metadata_append_status(ctx, data, sizeof(data), &used) < 0 ||
            json_append_raw(data, sizeof(data), &used, "}") < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "status response overflow");
        return le_adapter_respond_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "set_enabled")) {
        if (json_bool(args, "enabled", &enabled) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "enabled must be boolean");
        if (set_enabled(ctx, enabled) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "AirPlay 2 binaries or configuration are unavailable");
        return le_adapter_respond_ok(response, response_size, id, "{}");
    }
    return le_adapter_respond_err(response, response_size, id, "unknown command");
}

static int send_response(int fd, const char *response, int length)
{
    size_t sent = 0;
    while (sent < (size_t)length) {
        ssize_t n = write(fd, response + sent, (size_t)length - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct airplay_ctx ctx;
    struct sigaction action;
    int foreground = 0;
    int enable_on_start = 0;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.listener = -1;
    ctx.metadata_fd = -1;
    snprintf(ctx.runtime_root, sizeof(ctx.runtime_root),
             "/run/libreecho/features/airplay2/root");
    ctx.dbus_pid = -1;
    ctx.avahi_pid = -1;
    ctx.nqptp_pid = -1;
    ctx.audio_pid = -1;
    ctx.engine_pid = -1;
    ctx.shairport_pid = -1;
    snprintf(ctx.socket_path, sizeof(ctx.socket_path), "%s", LE_ADAPTER_AIRPLAY_SOCK);
    snprintf(ctx.metadata_path, sizeof(ctx.metadata_path), "%s",
             AIRPLAY_METADATA_FIFO);
    snprintf(ctx.nqptp_path, sizeof(ctx.nqptp_path), "/usr/local/sbin/nqptp");
    snprintf(ctx.shairport_path, sizeof(ctx.shairport_path), "/usr/local/sbin/shairport-sync");
    snprintf(ctx.avahi_path, sizeof(ctx.avahi_path), "/usr/local/sbin/avahi-daemon");
    snprintf(ctx.dbus_path, sizeof(ctx.dbus_path), "/usr/local/sbin/dbus-daemon");
    snprintf(ctx.audio_path, sizeof(ctx.audio_path), "/usr/local/sbin/libreecho-airplay-audio");
    snprintf(ctx.engine_path, sizeof(ctx.engine_path), "/usr/local/sbin/libreecho-audio-engine");
    snprintf(ctx.config_path, sizeof(ctx.config_path), "/etc/libreecho/airplay2.conf");
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) foreground = 1;
        else if (!strcmp(argv[i], "--enable-on-start")) enable_on_start = 1;
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) snprintf(ctx.socket_path, sizeof(ctx.socket_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(ctx.runtime_root, sizeof(ctx.runtime_root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--nqptp") && i + 1 < argc) snprintf(ctx.nqptp_path, sizeof(ctx.nqptp_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--shairport-sync") && i + 1 < argc) snprintf(ctx.shairport_path, sizeof(ctx.shairport_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) snprintf(ctx.audio_path, sizeof(ctx.audio_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) snprintf(ctx.engine_path, sizeof(ctx.engine_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(ctx.config_path, sizeof(ctx.config_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--metadata") && i + 1 < argc) snprintf(ctx.metadata_path, sizeof(ctx.metadata_path), "%s", argv[++i]);
        else { fprintf(stderr, "Usage: %s [--foreground] [--enable-on-start] [--socket PATH] [--root PATH] [--nqptp PATH] [--shairport-sync PATH] [--audio PATH] [--engine PATH] [--config PATH] [--metadata PATH]\n", argv[0]); return 1; }
    }
    (void)foreground;
    le_log_init("airplayd", argc, argv);
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    ctx.listener = le_adapter_listen(ctx.socket_path);
    if (ctx.listener < 0) {
        perror("airplayd: listen");
        return 1;
    }
    ctx.engine_pid = spawn_engine(&ctx);
    if (ctx.engine_pid < 0 ||
        !wait_for_runtime_file(&ctx, "/run/libreecho-audio/media.pcm", 30) ||
        !child_running(&ctx.engine_pid)) {
        le_log_warn("airplayd: shared audio engine unavailable at startup");
        stop_child(&ctx.engine_pid);
    } else {
        le_log_info("airplayd: shared audio engine ready");
    }
    le_log_info("airplayd: starting (socket=%s, enable_on_start=%s)",
                ctx.socket_path, enable_on_start ? "yes" : "no");
    if (enable_on_start) {
        if (set_enabled(&ctx, 1) < 0)
            le_log_warn("airplayd: persisted AirPlay enable failed at startup");
    }
    while (running) {
        struct pollfd pfd[2];
        nfds_t descriptors = 1;
        int poll_result;
        int client;
        if (reap(&ctx) && ctx.enabled) {
            /* A child can exit after enable succeeded, most notably when an
             * ALSA stream is opened.  Clear runtime state and reap the
             * remaining children so the next enable starts cleanly instead
             * of accumulating orphaned D-Bus/Avahi instances. */
            ctx.enabled = 0;
            le_log_warn("airplayd: AirPlay child exited; stopping remaining children");
            stop_child(&ctx.shairport_pid);
            stop_child(&ctx.audio_pid);
            stop_child(&ctx.nqptp_pid);
            stop_child(&ctx.avahi_pid);
            stop_child(&ctx.dbus_pid);
            metadata_fifo_close(&ctx);
        }
        if (!child_alive(ctx.engine_pid)) {
            ctx.engine_pid = spawn_engine(&ctx);
            if (ctx.engine_pid < 0 ||
                !wait_for_runtime_file(&ctx, "/run/libreecho-audio/media.pcm", 10) ||
                !child_running(&ctx.engine_pid))
                stop_child(&ctx.engine_pid);
            else
                le_log_info("airplayd: shared audio engine restarted");
        }
        if (ctx.enabled && ctx.metadata_fd < 0)
            (void)metadata_fifo_open(&ctx);
        pfd[0].fd = ctx.listener;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        if (ctx.metadata_fd >= 0) {
            pfd[1].fd = ctx.metadata_fd;
            pfd[1].events = POLLIN;
            pfd[1].revents = 0;
            descriptors = 2;
        }
        poll_result = poll(pfd, descriptors, 500);
        if (poll_result < 0 && errno != EINTR)
            break;
        if (poll_result <= 0)
            continue;
        if (descriptors == 2 && (pfd[1].revents & POLLIN))
            metadata_fifo_drain(&ctx);
        if (descriptors == 2 &&
            (pfd[1].revents & (POLLERR | POLLNVAL)))
            metadata_fifo_close(&ctx);
        if (!(pfd[0].revents & POLLIN))
            continue;
        client = le_adapter_accept(ctx.listener);
        if (client >= 0) {
            char message[INPUT_MAX], response[INPUT_MAX];
            ssize_t n = read(client, message, sizeof(message) - 1);
            if (n > 0) {
                int length;
                message[n] = '\0';
                length = request(&ctx, message, response, sizeof(response));
                if (length > 0) (void)send_response(client, response, length);
            }
            close(client);
        }
    }
    set_enabled(&ctx, 0);
    stop_child(&ctx.engine_pid);
    close(ctx.listener);
    unlink(ctx.socket_path);
    return 0;
}
