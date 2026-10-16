#define _POSIX_C_SOURCE 200809L
#include "mdns_lease.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct field { const char *key; size_t offset, size; };
#define FIELD(name) { #name, offsetof(struct le_mdns_esphome_metadata, name), sizeof(((struct le_mdns_esphome_metadata *)0)->name) }
static const struct field fields[] = {
    FIELD(version), FIELD(mac), FIELD(platform), FIELD(board), FIELD(network),
    FIELD(friendly_name), FIELD(api_encryption), FIELD(project_name), FIELD(project_version)
};
#define FIELD_COUNT (sizeof(fields) / sizeof(fields[0]))

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/* Reject invalid/overlong UTF-8, surrogates and XML-invalid code points.
 * Controls (including CR/LF/NUL) are never accepted in discovery fields. */
static int utf8_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned int cp, minimum;
        unsigned int count, j;
        unsigned char c = s[i++];
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; count = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; count = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; count = 3; minimum = 0x10000; }
        else return 0;
        if (count > n - i) return 0;
        for (j = 0; j < count; ++j) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp == 0xfffe || cp == 0xffff || (cp >= 0x80 && cp <= 0x9f)) return 0;
    }
    return 1;
}
static int metadata_valid(const struct le_mdns_esphome_metadata *m)
{
    size_t i, n;
    unsigned int first = 0, nonzero = 0, nonff = 0;
    if (!m) return 0;
    for (i = 0; i < FIELD_COUNT; ++i) {
        const char *s = (const char *)m + fields[i].offset;
        n = strnlen(s, fields[i].size);
        if (!n || n >= fields[i].size || !utf8_valid((const unsigned char *)s, n)) return 0;
    }
    if ((strcmp(m->platform, "LibreEcho") && strcmp(m->platform, "Linux")) ||
        strcmp(m->board, "radar-puffin") ||
        (strcmp(m->network, "wifi") && strcmp(m->network, "ethernet")) ||
        strcmp(m->api_encryption, LE_MDNS_ESPHOME_NOISE)) return 0;
    n = strlen(m->mac);
    if (n != 12 && n != 17) return 0;
    for (i = 0; i < 6; ++i) {
        size_t p = i * (n == 17 ? 3 : 2);
        int high = hex((unsigned char)m->mac[p]), low = hex((unsigned char)m->mac[p + 1]);
        unsigned int byte;
        if (high < 0 || low < 0 || (n == 17 && i < 5 && m->mac[p + 2] != ':')) return 0;
        byte = (unsigned int)(high * 16 + low);
        if (!i) first = byte;
        nonzero |= byte; nonff |= byte ^ 255;
    }
    return nonzero && nonff && !(first & 1);
}
int le_mdns_esphome_encode(char *message, size_t capacity, unsigned int port,
                           const struct le_mdns_esphome_metadata *metadata)
{
    static const char digits[] = "0123456789abcdef";
    size_t i, j, used;
    int n;
    if (!message || !capacity || !port || port > 65535 || !metadata_valid(metadata)) return -1;
    n = snprintf(message, capacity, "ESPHOME/1 %u", port);
    if (n < 0 || (size_t)n >= capacity) return -1;
    used = (size_t)n;
    for (i = 0; i < FIELD_COUNT; ++i) {
        const unsigned char *s = (const unsigned char *)metadata + fields[i].offset;
        size_t length = strlen((const char *)s), key_length = strlen(fields[i].key);
        if (key_length + 2 + length * 2 >= capacity - used) return -1;
        message[used++] = ' ';
        memcpy(message + used, fields[i].key, key_length); used += key_length;
        message[used++] = '=';
        for (j = 0; j < length; ++j) {
            message[used++] = digits[s[j] >> 4]; message[used++] = digits[s[j] & 15];
        }
    }
    if (used + 2 > capacity || used + 2 > LE_MDNS_ESPHOME_PACKET_MAX) return -1;
    message[used++] = '\n'; message[used] = 0;
    return (int)used;
}
int le_mdns_esphome_decode(const char *message, size_t length, unsigned int *port,
                           struct le_mdns_esphome_metadata *metadata)
{
    struct le_mdns_esphome_metadata parsed;
    size_t p = 10, i;
    unsigned int number = 0, seen = 0;
    if (!message || !port || !metadata || length < 12 || length >= LE_MDNS_ESPHOME_PACKET_MAX ||
        memcmp(message, "ESPHOME/1 ", 10) || message[length - 1] != '\n' ||
        memchr(message, 0, length) || message[p] < '1' || message[p] > '9') return -1;
    memset(&parsed, 0, sizeof(parsed));
    while (p < length && message[p] >= '0' && message[p] <= '9') {
        number = number * 10 + (unsigned int)(message[p++] - '0');
        if (number > 65535) return -1;
    }
    while (p < length - 1) {
        size_t key, value, size, j;
        char *target;
        if (message[p++] != ' ') return -1;
        key = p;
        while (p < length - 1 && message[p] != '=' && message[p] != ' ') ++p;
        if (p >= length - 1 || message[p] != '=') return -1;
        for (i = 0; i < FIELD_COUNT; ++i)
            if (strlen(fields[i].key) == p - key && !memcmp(message + key, fields[i].key, p - key)) break;
        if (i == FIELD_COUNT || (seen & (1u << i))) return -1;
        seen |= 1u << i;
        value = ++p;
        while (p < length - 1 && message[p] != ' ') ++p;
        size = p - value;
        if (!size || size % 2 || size / 2 >= fields[i].size) return -1;
        target = (char *)&parsed + fields[i].offset;
        for (j = 0; j < size / 2; ++j) {
            int high = hex((unsigned char)message[value + j * 2]);
            int low = hex((unsigned char)message[value + j * 2 + 1]);
            if (high < 0 || low < 0 || (!high && !low)) return -1;
            target[j] = (char)(high * 16 + low);
        }
        target[j] = 0;
    }
    if (seen != (1u << FIELD_COUNT) - 1 || !metadata_valid(&parsed)) return -1;
    *metadata = parsed; *port = number;
    return 0;
}
static int append(char *out, size_t capacity, size_t *used, const char *text)
{
    size_t n = strlen(text);
    if (n >= capacity - *used) return -1;
    memcpy(out + *used, text, n + 1); *used += n;
    return 0;
}
static int append_xml(char *out, size_t capacity, size_t *used, const char *text)
{
    while (*text) {
        const char *escaped;
        char single[2] = { *text++, 0 };
        switch (single[0]) {
        case '&': escaped = "&amp;"; break;
        case '<': escaped = "&lt;"; break;
        case '>': escaped = "&gt;"; break;
        case '"': escaped = "&quot;"; break;
        case '\'': escaped = "&apos;"; break;
        default: escaped = single; break;
        }
        if (append(out, capacity, used, escaped) < 0) return -1;
    }
    return 0;
}
void le_mdns_lease_init(struct le_mdns_lease *lease, int directory_fd)
{
    memset(lease, 0, sizeof(*lease));
    lease->directory_fd = directory_fd;
    lease->owner_fd = -1;
}
int le_mdns_lease_withdraw(struct le_mdns_lease *lease, int owner_fd)
{
    if (owner_fd < 0 || lease->owner_fd != owner_fd) return -1;
    if (lease->filename[0] && unlinkat(lease->directory_fd, lease->filename, 0) < 0 && errno != ENOENT) return -1;
    lease->filename[0] = 0; lease->owner_fd = -1;
    return 0;
}
int le_mdns_lease_register_esphome(struct le_mdns_lease *lease, int owner_fd,
                                  unsigned int port,
                                  const struct le_mdns_esphome_metadata *metadata)
{
    char xml[4096] = "", name[64], temp[64], port_text[16];
    int fd, result = -1;
    size_t used = 0, written = 0, i;
    if (!lease || owner_fd < 0 || port < 1 || port > 65535 || !metadata_valid(metadata) ||
        (lease->owner_fd >= 0 && lease->owner_fd != owner_fd) || lease->generation == ULONG_MAX) return -1;
    (void)snprintf(port_text, sizeof(port_text), "%u", port);
#define ADD(text) do { if (append(xml, sizeof(xml), &used, text) < 0) return -1; } while (0)
#define XML(text) do { if (append_xml(xml, sizeof(xml), &used, text) < 0) return -1; } while (0)
    ADD("<?xml version=\"1.0\"?><!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
        "<service-group><name replace-wildcards=\"no\">");
    XML(metadata->friendly_name);
    ADD("</name><service><type>_esphomelib._tcp</type><port>"); ADD(port_text); ADD("</port>");
    for (i = 0; i < FIELD_COUNT; ++i) {
        ADD("<txt-record>"); ADD(fields[i].key); ADD("=");
        XML((const char *)metadata + fields[i].offset); ADD("</txt-record>");
    }
    ADD("</service></service-group>\n");
#undef ADD
#undef XML
    ++lease->generation;
    (void)snprintf(name, sizeof(name), "esphome-%lu.service", lease->generation);
    (void)snprintf(temp, sizeof(temp), "esphome-%lu.tmp", lease->generation);
    fd = openat(lease->directory_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    while (written < used) {
        ssize_t count = write(fd, xml + written, used - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) goto out;
        written += (size_t)count;
    }
    if (fsync(fd) < 0) goto out;
    if (close(fd) < 0) { fd = -1; goto out; }
    fd = -1;
    /* Never replace an unexpected existing generation, including symlinks. */
    if (linkat(lease->directory_fd, temp, lease->directory_fd, name, 0) < 0) goto out;
    if (lease->filename[0] && unlinkat(lease->directory_fd, lease->filename, 0) < 0 && errno != ENOENT) {
        (void)unlinkat(lease->directory_fd, name, 0); goto out;
    }
    memcpy(lease->filename, name, strlen(name) + 1); lease->owner_fd = owner_fd; result = 0;
out:
    if (fd >= 0) (void)close(fd);
    (void)unlinkat(lease->directory_fd, temp, 0);
    return result;
}
