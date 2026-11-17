/* Device group store (#448): create, members, text form, malformed files
 * refused, the file saved 0600, and the action names. The network side is
 * covered by tests/test_peer_group.py through real daemons. */
#include "peer_group.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_create_and_members(void)
{
    struct le_group_set set;
    struct le_group *g;
    int id;
    size_t i;
    le_group_set_init(&set);
    CHECK(le_group_create(&set, "") < 0);
    CHECK(le_group_create(&set, "Bad\tname") < 0);
    CHECK(le_group_create(&set, "Downstairs") == 1);
    CHECK(le_group_create(&set, "Upstairs") == 2);
    CHECK(set.count == 2);
    CHECK(le_group_add_member(&set, 1, "192.168.1.20", 8080, "Kitchen") == 0);
    CHECK(le_group_add_member(&set, 1, "192.168.1.20", 8080, "Kitchen again") < 0);
    CHECK(le_group_add_member(&set, 1, "192.168.1.21", 0, "Hall") < 0);
    CHECK(le_group_add_member(&set, 1, "192.168.1.21", 65536, "Hall") < 0);
    CHECK(le_group_add_member(&set, 1, "not-an-address", 80, "Hall") < 0);
    CHECK(le_group_add_member(&set, 1, "192.168.1.21", 80, "") < 0);
    CHECK(le_group_add_member(&set, 1, "192.168.1.21", 80, "Hall\n") < 0);
    CHECK(le_group_add_member(&set, 9, "192.168.1.21", 80, "Hall") < 0);
    for (i = 1; i < LE_GROUP_MEMBERS_MAX; i++) {
        char addr[16];
        snprintf(addr, sizeof(addr), "10.0.0.%zu", i + 1);
        CHECK(le_group_add_member(&set, 1, addr, 80, "Device") == 0);
    }
    CHECK(le_group_add_member(&set, 1, "10.0.0.99", 80, "One too many") < 0);
    g = le_group_find(&set, 1);
    CHECK(g && g->member_count == LE_GROUP_MEMBERS_MAX);
    CHECK(le_group_find(&set, 2) && le_group_find(&set, 2)->member_count == 0);
    CHECK(le_group_delete(&set, 2) == 0);
    CHECK(le_group_delete(&set, 2) < 0);
    CHECK(le_group_find(&set, 2) == NULL);
    /* Ids keep counting up after a delete, so a removed id is not reused. */
    id = le_group_create(&set, "Garden");
    CHECK(id == 3);
    for (i = 0; i < LE_GROUPS_MAX; i++) if (set.count < LE_GROUPS_MAX) le_group_create(&set, "Filler");
    CHECK(set.count == LE_GROUPS_MAX);
    CHECK(le_group_create(&set, "Too many") < 0);
}

static void test_text_round_trip(void)
{
    struct le_group_set a, b;
    char text[LE_GROUP_FILE_MAX];
    int n;
    le_group_set_init(&a);
    le_group_create(&a, "Kitchen and hall");
    le_group_create(&a, "Empty");
    le_group_add_member(&a, 1, "192.168.1.20", 8080, "Kitchen echo");
    le_group_add_member(&a, 1, "192.168.1.21", 80, "Hall dot");
    n = le_group_set_format(&a, text, sizeof(text));
    CHECK(n > 0);
    CHECK(strstr(text, "member 1 192.168.1.21 80 Hall dot\n") != NULL);
    CHECK(le_group_set_parse(&b, text) == 0);
    CHECK(b.count == 2 && b.next_id == a.next_id);
    CHECK(!strcmp(b.groups[0].name, "Kitchen and hall") && b.groups[0].member_count == 2);
    CHECK(!strcmp(b.groups[0].members[1].address, "192.168.1.21") && b.groups[0].members[1].port == 80);
    CHECK(!strcmp(b.groups[0].members[0].name, "Kitchen echo"));
    CHECK(le_group_set_format(&b, text, 8) < 0);
}

static void test_malformed_text_refused(void)
{
    struct le_group_set set;
    static const char *bad[] = {
        "group 1 Kitchen\n",                       /* no next line */
        "next 0\n",                                /* next id must be positive */
        "next 2\nmember 1 10.0.0.2 80 Hall\n",     /* member of a missing group */
        "next 2\ngroup 1 A\nmember 1 10.0.0.2 0 Hall\n",
        "next 2\ngroup 1 A\nmember 1 10.0.0.2 80\n",
        "next 2\ngroup 1 A\nmember 1 nonsense 80 Hall\n",
        "next 2\ngroup 1 A\ngroup 1 B\n",          /* duplicate group */
        "next 2\ngroup 1 A\nthing 1 2\n",          /* unknown record */
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(le_group_set_parse(&set, bad[i]) < 0);
        CHECK(set.count == 0);
    }
}

static void test_file_round_trip(void)
{
    char dir[] = "/tmp/le-peer-group-XXXXXX";
    char path[256];
    struct stat st;
    struct le_group_set a, b;
    FILE *f;
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/peer-groups", dir);
    CHECK(le_group_set_load(&b, path) == 0);
    le_group_set_init(&a);
    le_group_create(&a, "Hall");
    le_group_add_member(&a, 1, "192.168.1.21", 80, "Hall dot");
    CHECK(le_group_set_save(&a, path) == 0);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(le_group_set_load(&b, path) == 1);
    CHECK(b.count == 1 && b.groups[0].member_count == 1);
    f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) { fputs("next 2\ngroup 1 Broken\nmember 1 x 1 y\n", f); fclose(f); }
    CHECK(le_group_set_load(&b, path) < 0);
    CHECK(b.count == 0);
    unlink(path);
    rmdir(dir);
}

static void test_actions(void)
{
    CHECK(le_group_action_from_name("mute") == LE_GROUP_ACTION_MUTE);
    CHECK(le_group_action_from_name("unmute") == LE_GROUP_ACTION_UNMUTE);
    CHECK(le_group_action_from_name("stop") == LE_GROUP_ACTION_STOP);
    CHECK(le_group_action_from_name("ring") == LE_GROUP_ACTION_NONE);
    CHECK(le_group_action_from_name("Mute") == LE_GROUP_ACTION_NONE);
    CHECK(le_group_action_from_name(NULL) == LE_GROUP_ACTION_NONE);
    CHECK(!strcmp(le_group_action_name(LE_GROUP_ACTION_STOP), "stop"));
}

int main(void)
{
    test_create_and_members();
    test_text_round_trip();
    test_malformed_text_refused();
    test_file_round_trip();
    test_actions();
    if (failures) {
        fprintf(stderr, "%d peer group check(s) failed\n", failures);
        return 1;
    }
    puts("peer group store: ok");
    return 0;
}
