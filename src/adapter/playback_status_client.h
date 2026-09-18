#ifndef LIBREECHO_PLAYBACK_STATUS_CLIENT_H
#define LIBREECHO_PLAYBACK_STATUS_CLIENT_H

/* Returns 1 when the selected priority bus is physically drained, 0 while it
 * is pending, and -1 for an unavailable/malformed status or unsupported bus. */
int le_playback_status_bus_drained(const char *path, const char *bus);

#endif
