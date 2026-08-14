/*
 * LibreEcho Bluetooth profile service layer (SDP / AVDTP A2DP-SINK / AVRCP).
 *
 * The MT8163 kernel HCI transport exposes raw L2CAP sockets; this layer
 * registers the userspace profile endpoints on top of them:
 *
 *   - SDP server            PSM 0x0001  (service discovery)
 *   - AVDTP signaling/media PSM 0x0019  (A2DP-SINK stream endpoint)
 *   - AVRCP target          PSM 0x0017  (AVCTP control + browsing)
 *
 * Decoded SBC audio is written as S16_LE/48 kHz/stereo PCM to the shared
 * media bus consumed by the audio engine.  All inbound lengths are
 * bounds-checked; malformed requests are rejected.
 *
 * The SBC codec is the vendored BlueZ library under src/adapter/bt-sbc/
 * (SPDX-License-Identifier: LGPL-2.1-or-later).
 */
#ifndef LIBREECHO_BT_PROFILE_H
#define LIBREECHO_BT_PROFILE_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

/* Maximum file descriptors the profile layer can poll on. */
#define LE_PROFILE_MAX_FDS 24

struct le_profiles;

struct le_profiles {
    int sdp_listener;
    int avdtp_listener;
    int avrcp_listener;
    int media_bus_fd;
    int registered_sdp;
    int registered_a2dp_sink;
    int registered_avrcp;
    int stream_active;
    uint64_t media_frames_written;
    void *sessions;
    char service_name[64];
};

/*
 * Open the profile listeners.  service_name is used for the SDP service
 * name attribute.  Returns 0 when at least the SDP listener registered.
 */
int le_profile_open(struct le_profiles *p, const char *service_name);
void le_profile_close(struct le_profiles *p);

/*
 * Append the profile file descriptors to pollfds (up to max_fds) and return
 * the number added.  fd_map receives opaque indices consumed by
 * le_profile_poll_events.
 */
int le_profile_poll_setup(struct le_profiles *p, struct pollfd *pollfds,
                          int max_fds, int *fd_map);
void le_profile_poll_events(struct le_profiles *p, const struct pollfd *pollfds,
                            const int *fd_map, int count);

int le_profile_registered_sdp(const struct le_profiles *p);
int le_profile_registered_a2dp_sink(const struct le_profiles *p);
int le_profile_registered_avrcp(const struct le_profiles *p);
int le_profile_stream_active(const struct le_profiles *p);

#endif /* LIBREECHO_BT_PROFILE_H */
