#ifndef LE_PEER_ANNOUNCE_H
#define LE_PEER_ANNOUNCE_H
#include <stddef.h>

/* Broadcast announcements between paired devices (#412, first slice).
 *
 * The owner sends a short text to every paired device on the LAN, or to one
 * device, as a signed "announcement" packet (peer_auth.h, the house key from
 * #450). Each receiving device speaks the text through its own announcement
 * path, but only after the packet verifies, and each packet is accepted once.
 *
 * The packet body is the text itself: UTF-8 bytes, 1 to LE_ANNOUNCE_TEXT_MAX
 * long, with no control characters. It is signed, not encrypted; the text is
 * spoken aloud on the receiving devices anyway. A device without the house
 * key neither sends nor accepts announcements. */

#define LE_ANNOUNCE_TYPE "announcement"
#define LE_ANNOUNCE_TEXT_MAX 240

/* 1 when text is a well-formed announcement body of len bytes, 0 otherwise.
 * Blank text, control characters and anything longer than the limit are
 * refused. */
int le_announce_body_valid(const char *text, size_t len);
#endif
