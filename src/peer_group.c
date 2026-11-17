/* Device groups for group control (#448). See peer_group.h. */
#include "peer_group.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int text_ok(const char *s, size_t cap)
{
    size_t i;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (i + 1 >= cap || ch < 0x20 || ch == 0x7f) return 0;
    }
    return 1;
}

void le_group_set_init(struct le_group_set *set)
{
    if (!set) return;
    memset(set, 0, sizeof(*set));
    set->next_id = 1;
}

int le_group_create(struct le_group_set *set, const char *name)
{
    struct le_group *g;
    if (!set || !text_ok(name, LE_GROUP_NAME_MAX) || set->count >= LE_GROUPS_MAX || set->next_id < 1) return -1;
    g = &set->groups[set->count++];
    memset(g, 0, sizeof(*g));
    g->id = set->next_id++;
    snprintf(g->name, sizeof(g->name), "%s", name);
    return g->id;
}

int le_group_delete(struct le_group_set *set, int id)
{
    size_t i;
    if (!set) return -1;
    for (i = 0; i < set->count; i++) {
        if (set->groups[i].id != id) continue;
        memmove(&set->groups[i], &set->groups[i + 1], (set->count - i - 1) * sizeof(set->groups[i]));
        set->count--;
        memset(&set->groups[set->count], 0, sizeof(set->groups[set->count]));
        return 0;
    }
    return -1;
}

struct le_group *le_group_find(struct le_group_set *set, int id)
{
    size_t i;
    if (!set) return NULL;
    for (i = 0; i < set->count; i++) if (set->groups[i].id == id) return &set->groups[i];
    return NULL;
}

int le_group_add_member(struct le_group_set *set, int id, const char *address, unsigned int port, const char *name)
{
    struct le_group *g = le_group_find(set, id);
    struct in_addr a;
    size_t i;
    if (!g || !address || inet_pton(AF_INET, address, &a) != 1 || port < 1 || port > 65535) return -1;
    if (!text_ok(name, LE_GROUP_MEMBER_NAME_MAX) || g->member_count >= LE_GROUP_MEMBERS_MAX) return -1;
    for (i = 0; i < g->member_count; i++)
        if (!strcmp(g->members[i].address, address) && g->members[i].port == port) return -1;
    snprintf(g->members[g->member_count].address, sizeof(g->members[0].address), "%s", address);
    g->members[g->member_count].port = port;
    snprintf(g->members[g->member_count].name, sizeof(g->members[0].name), "%s", name);
    g->member_count++;
    return 0;
}

int le_group_set_format(const struct le_group_set *set, char *out, size_t capacity)
{
    size_t i, j, used = 0;
    int n;
    if (!set || !out || !capacity) return -1;
    n = snprintf(out, capacity, "next %d\n", set->next_id);
    if (n < 0 || (size_t)n >= capacity) return -1;
    used = (size_t)n;
    for (i = 0; i < set->count; i++) {
        const struct le_group *g = &set->groups[i];
        n = snprintf(out + used, capacity - used, "group %d %s\n", g->id, g->name);
        if (n < 0 || (size_t)n >= capacity - used) return -1;
        used += (size_t)n;
        for (j = 0; j < g->member_count; j++) {
            const struct le_group_member *m = &g->members[j];
            n = snprintf(out + used, capacity - used, "member %d %s %u %s\n", g->id, m->address, m->port, m->name);
            if (n < 0 || (size_t)n >= capacity - used) return -1;
            used += (size_t)n;
        }
    }
    return (int)used;
}

/* Reads one decimal number at *p, skipping one leading space. */
static int take_int(const char **p, long *out)
{
    char *end;
    long v;
    if (**p != ' ') return -1;
    (*p)++;
    errno = 0;
    v = strtol(*p, &end, 10);
    if (errno || end == *p || (*end != ' ' && *end != 0)) return -1;
    *p = end;
    *out = v;
    return 0;
}

/* Reads one token (no spaces) at *p, skipping one leading space. */
static int take_word(const char **p, char *out, size_t capacity)
{
    size_t n = 0;
    if (**p != ' ') return -1;
    (*p)++;
    while (**p && **p != ' ') {
        if (n + 1 >= capacity) return -1;
        out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return n ? 0 : -1;
}

/* Reads the rest of the line at *p, skipping one leading space. */
static int take_rest(const char **p, char *out, size_t capacity)
{
    size_t n = strlen(*p);
    if (**p != ' ' || n < 2) return -1;
    (*p)++;
    if (n - 1 >= capacity) return -1;
    memcpy(out, *p, n - 1);
    out[n - 1] = 0;
    return text_ok(out, capacity) ? 0 : -1;
}

static int parse_line(struct le_group_set *set, const char *line)
{
    long v;
    const char *p;
    if (!strncmp(line, "next", 4) && line[4] == ' ') {
        p = line + 4;
        if (take_int(&p, &v) || *p || v < 1 || v > 1000000) return -1;
        set->next_id = (int)v;
        return 0;
    }
    if (!strncmp(line, "group", 5) && line[5] == ' ') {
        char name[LE_GROUP_NAME_MAX];
        p = line + 5;
        if (take_int(&p, &v) || take_rest(&p, name, sizeof(name)) || v < 1) return -1;
        if (set->count >= LE_GROUPS_MAX || le_group_find(set, (int)v)) return -1;
        set->groups[set->count].id = (int)v;
        memcpy(set->groups[set->count].name, name, sizeof(name));
        set->count++;
        return 0;
    }
    if (!strncmp(line, "member", 6) && line[6] == ' ') {
        char addr[32], name[LE_GROUP_MEMBER_NAME_MAX];
        struct le_group *g;
        p = line + 6;
        if (take_int(&p, &v)) return -1;
        g = le_group_find(set, (int)v);
        if (!g) return -1;
        if (take_word(&p, addr, sizeof(addr))) return -1;
        if (take_int(&p, &v) || take_rest(&p, name, sizeof(name))) return -1;
        return le_group_add_member(set, g->id, addr, (unsigned int)v, name);
    }
    return -1;
}

int le_group_set_parse(struct le_group_set *set, const char *text)
{
    char line[LE_GROUP_FILE_MAX];
    const char *p = text;
    if (!set || !text) return -1;
    le_group_set_init(set);
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '\n') n++;
        if (n >= sizeof(line)) goto fail;
        memcpy(line, p, n);
        line[n] = 0;
        if (n && parse_line(set, line)) goto fail;
        p += n;
        if (*p == '\n') p++;
    }
    /* Every group id must be below next_id, or a later create would reuse one. */
    {
        size_t i;
        for (i = 0; i < set->count; i++) if (set->groups[i].id < 1 || set->groups[i].id >= set->next_id) goto fail;
    }
    if (set->next_id < 1) goto fail;
    return 0;
fail:
    le_group_set_init(set);
    return -1;
}

int le_group_set_load(struct le_group_set *set, const char *path)
{
    static char buf[LE_GROUP_FILE_MAX + 1];
    ssize_t n;
    int fd, rc;
    if (!set) return -1;
    le_group_set_init(set);
    if (!path || !path[0]) return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0 || (size_t)n >= sizeof(buf) - 1) return -1;
    buf[n] = 0;
    rc = le_group_set_parse(set, buf);
    return rc ? -1 : 1;
}

int le_group_set_save(const struct le_group_set *set, const char *path)
{
    static char text[LE_GROUP_FILE_MAX];
    char tmp[512];
    int fd, rc = -1, n;
    size_t len;
    if (!set || !path || !path[0] || strlen(path) + 5 > sizeof(tmp)) return -1;
    n = le_group_set_format(set, text, sizeof(text));
    if (n < 0) return -1;
    len = (size_t)n;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    unlink(tmp);
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        if (!fchmod(fd, 0600) && write(fd, text, len) == (ssize_t)len && !fsync(fd)) rc = 0;
        if (close(fd)) rc = -1;
        if (!rc && rename(tmp, path)) rc = -1;
        if (rc) unlink(tmp);
    }
    return rc;
}

enum le_group_action le_group_action_from_name(const char *name)
{
    if (!name) return LE_GROUP_ACTION_NONE;
    if (!strcmp(name, "mute")) return LE_GROUP_ACTION_MUTE;
    if (!strcmp(name, "unmute")) return LE_GROUP_ACTION_UNMUTE;
    if (!strcmp(name, "stop")) return LE_GROUP_ACTION_STOP;
    return LE_GROUP_ACTION_NONE;
}

const char *le_group_action_name(enum le_group_action action)
{
    switch (action) {
    case LE_GROUP_ACTION_MUTE: return "mute";
    case LE_GROUP_ACTION_UNMUTE: return "unmute";
    case LE_GROUP_ACTION_STOP: return "stop";
    default: return "";
    }
}
