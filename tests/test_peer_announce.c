/* Broadcast announcement bodies (#412): what may be spoken on another device.
 * The network side is covered by tests/test_peer_announce.py through real
 * daemons on loopback. */
#include "peer_announce.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_accepts_plain_short_text(void)
{
    CHECK(le_announce_body_valid("Dinner is ready", 15));
    CHECK(le_announce_body_valid("x", 1));
    /* UTF-8 bytes are allowed; they are not control characters. */
    CHECK(le_announce_body_valid("Caf\xc3\xa9 ready", 11));
}

static void test_refuses_empty_blank_and_control_text(void)
{
    CHECK(!le_announce_body_valid("", 0));
    CHECK(!le_announce_body_valid(NULL, 0));
    CHECK(!le_announce_body_valid("   ", 3));
    CHECK(!le_announce_body_valid("line\nbreak", 10));
    CHECK(!le_announce_body_valid("tab\there", 8));
    CHECK(!le_announce_body_valid("nul\0inside", 10));
    CHECK(!le_announce_body_valid("del\x7f", 4));
}

static void test_length_limit(void)
{
    char text[LE_ANNOUNCE_TEXT_MAX + 2];
    memset(text, 'a', sizeof text - 1);
    text[sizeof text - 1] = 0;
    CHECK(le_announce_body_valid(text, LE_ANNOUNCE_TEXT_MAX));
    CHECK(!le_announce_body_valid(text, LE_ANNOUNCE_TEXT_MAX + 1));
}

int main(void)
{
    test_accepts_plain_short_text();
    test_refuses_empty_blank_and_control_text();
    test_length_limit();
    if (failures) {
        fprintf(stderr, "%d announcement check(s) failed\n", failures);
        return 1;
    }
    puts("peer announce body tests passed");
    return 0;
}
