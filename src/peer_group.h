#ifndef LE_PEER_GROUP_H
#define LE_PEER_GROUP_H
#include <stddef.h>

/* Device groups for group control (#448).
 *
 * The owner defines groups on one device: a name and member devices, each
 * given by the address and HTTP port shown in the device list (#413). A
 * group command (mute, unmute, stop) is sent to every member as a signed
 * "group-command" packet (peer_auth.h, the house key from #450). A member
 * executes it only after the packet verifies, so a device outside the house
 * or a replayed packet does nothing.
 *
 * The store is plain text, one record per line, saved atomically with mode
 * 0600 beside the configuration:
 *   next <id>
 *   group <id> <name>
 *   member <group id> <address> <port> <name>
 * Names are the rest of their line and hold no control characters. */

#define LE_GROUPS_MAX 8
#define LE_GROUP_MEMBERS_MAX 8
#define LE_GROUP_NAME_MAX 48
#define LE_GROUP_MEMBER_NAME_MAX 64
#define LE_GROUP_FILE_MAX 16384
#define LE_GROUP_COMMAND_TYPE "group-command"
#define LE_GROUP_COMMAND_WINDOW_S 300

enum le_group_action {
    LE_GROUP_ACTION_NONE = 0,
    LE_GROUP_ACTION_MUTE,   /* microphone muted */
    LE_GROUP_ACTION_UNMUTE, /* microphone unmuted */
    LE_GROUP_ACTION_STOP    /* speech and radio playback stopped */
};

struct le_group_member {
    char address[16];
    unsigned int port;
    char name[LE_GROUP_MEMBER_NAME_MAX];
};

struct le_group {
    int id;
    char name[LE_GROUP_NAME_MAX];
    size_t member_count;
    struct le_group_member members[LE_GROUP_MEMBERS_MAX];
};

struct le_group_set {
    int next_id;
    size_t count;
    struct le_group groups[LE_GROUPS_MAX];
};

void le_group_set_init(struct le_group_set *set);
/* Returns the new group id, or -1 when the name is invalid or the set is full. */
int le_group_create(struct le_group_set *set, const char *name);
/* 0 when removed, -1 when there is no such group. */
int le_group_delete(struct le_group_set *set, int id);
struct le_group *le_group_find(struct le_group_set *set, int id);
/* 0 when added. Fails on a bad IPv4 address, a port outside 1..65535, an
 * empty or control-character name, a full group, or a member already listed. */
int le_group_add_member(struct le_group_set *set, int id, const char *address, unsigned int port, const char *name);

/* Text form of the store; returns its length or -1 when it does not fit. */
int le_group_set_format(const struct le_group_set *set, char *out, size_t capacity);
/* 0 when the text is well formed, -1 otherwise (set is then cleared). */
int le_group_set_parse(struct le_group_set *set, const char *text);
/* 1 when a store was read, 0 when there is no file, -1 when it is unreadable
 * or malformed (set left empty). */
int le_group_set_load(struct le_group_set *set, const char *path);
/* Temp file, fsync, rename; mode 0600. */
int le_group_set_save(const struct le_group_set *set, const char *path);

/* Maps "mute", "unmute" or "stop" to an action; LE_GROUP_ACTION_NONE otherwise. */
enum le_group_action le_group_action_from_name(const char *name);
const char *le_group_action_name(enum le_group_action action);
#endif
