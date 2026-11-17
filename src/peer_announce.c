/* Broadcast announcement bodies between paired devices (#412). See peer_announce.h. */
#include "peer_announce.h"

int le_announce_body_valid(const char *text, size_t len)
{
    size_t i, visible = 0;
    if (!text || len < 1 || len > LE_ANNOUNCE_TEXT_MAX) return 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 0x20 || ch == 0x7f) return 0;
        if (ch != ' ') visible++;
    }
    return visible > 0;
}
