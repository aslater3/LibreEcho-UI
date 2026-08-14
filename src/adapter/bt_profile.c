/*
 * LibreEcho Bluetooth profile service layer implementation.
 *
 * Implements the userspace profile endpoints documented in bt_profile.h:
 * an SDP server, an AVDTP A2DP-SINK stream endpoint, and an AVRCP target.
 * See bt_profile.h for the design notes; the BlueZ specification constants
 * used here follow AVDTP 1.3, AVRCP 1.6, and SDP 3.0.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "adapter.h"
#include "bt_profile.h"
#include "bt-sbc/sbc.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LE_BTPROTO_L2CAP 0
#define LE_PSM_SDP 0x0001
#define LE_PSM_AVRCP 0x0017
#define LE_PSM_AVDTP 0x0019

#define LE_SOL_L2CAP 6
#define LE_L2CAP_OPTIONS 0x01
#define LE_L2CAP_IMTU 1024

/* AVDTP constants (A2DP transport profile, AVDTP 1.3). */
#define LE_AVDTP_MSG_COMMAND 0
#define LE_AVDTP_MSG_GENERAL_REJECT 1
#define LE_AVDTP_MSG_ACCEPT 2
#define LE_AVDTP_MSG_REJECT 3

#define LE_AVDTP_DISCOVER 0x01
#define LE_AVDTP_GET_CAPABILITIES 0x02
#define LE_AVDTP_SET_CONFIGURATION 0x03
#define LE_AVDTP_GET_CONFIGURATION 0x04
#define LE_AVDTP_RECONFIGURE 0x05
#define LE_AVDTP_OPEN 0x06
#define LE_AVDTP_START 0x07
#define LE_AVDTP_CLOSE 0x08
#define LE_AVDTP_SUSPEND 0x09
#define LE_AVDTP_ABORT 0x0A
#define LE_AVDTP_SECURITY_CONTROL 0x0B
#define LE_AVDTP_GET_ALL_CAPABILITIES 0x0C
#define LE_AVDTP_DELAY_REPORT 0x0D

#define LE_AVDTP_PKT_SINGLE 0
#define LE_AVDTP_PKT_START 1
#define LE_AVDTP_PKT_CONTINUE 2
#define LE_AVDTP_PKT_END 3

#define LE_AVDTP_CAT_MEDIA_TRANSPORT 0x01
#define LE_AVDTP_CAT_MEDIA_CODEC 0x07

#define LE_AVDTP_MEDIA_TYPE_AUDIO 0
#define LE_AVDTP_CODEC_SBC 0x00

#define LE_AVDTP_ERR_BAD_HEADER 0x01
#define LE_AVDTP_ERR_BAD_LENGTH 0x02
#define LE_AVDTP_ERR_BAD_ACP_SEID 0x03
#define LE_AVDTP_ERR_BAD_STATE 0x19
#define LE_AVDTP_ERR_BAD_CAPABILITIES 0x1b
#define LE_AVDTP_ERR_UNSUP_CAPABILITY 0x27

#define LE_AVDTP_SEID 1U
#define LE_MEDIA_RATE 48000
#define LE_MEDIA_CHUNK_FRAMES 512
#define LE_MEDIA_FIFO "/run/libreecho-audio/media.pcm"

/* AVRCP / AVCTP constants (AVRCP 1.6, AVCTP 1.4). */
#define LE_AVRCP_PID_CONTROL 0x110E
#define LE_AVRCP_COMPANY_BLUETOOTH 0x001958
#define LE_AVRCP_PDU_GET_CAPABILITIES 0x10
#define LE_AVRCP_PDU_GET_ELEMENT_ATTRIBUTES 0x20
#define LE_AVRCP_PDU_REGISTER_NOTIFICATION 0x31
#define LE_CAP_ID_COMPANIES 0x02
#define LE_CAP_ID_EVENTS 0x03
#define LE_AVRCP_EVENT_PLAYBACK_STATUS 0x01
#define LE_AVRCP_STATUS_RESPONSE 0x05
#define LE_AVRCP_STATUS_NOT_IMPLEMENTED 0x08
#define LE_AVRCP_STATUS_ACCEPTED 0x09
#define LE_AVRCP_STATUS_INTERIM 0x0f

/* SDP constants (SDP 3.0). */
#define LE_SDP_DE_NIL 0x00
#define LE_SDP_DE_UINT16 0x0a
#define LE_SDP_DE_UINT32 0x0c
#define LE_SDP_DE_TEXT8 0x25
#define LE_SDP_DE_UUID16 0x19
#define LE_SDP_DE_SEQUENCE 0x35

#define LE_SDP_PDU_ERROR 0x01
#define LE_SDP_PDU_SERVICE_SEARCH 0x02
#define LE_SDP_PDU_SERVICE_ATTR 0x04
#define LE_SDP_PDU_SERVICE_SEARCH_ATTR 0x06
#define LE_SDP_ERROR_INVALID_REQUEST 0x0002

#define LE_SDP_RECORD_BROWSE 0x00010000u
#define LE_SDP_RECORD_A2DP_SINK 0x00010001u
#define LE_SDP_RECORD_AVRCP 0x00010002u
#define LE_SDP_RECORD_PNP 0x00010004u

#define LE_SDP_ATTR_HANDLE 0x0000
#define LE_SDP_ATTR_CLASS_ID_LIST 0x0001
#define LE_SDP_ATTR_RECORD_STATE 0x0002
#define LE_SDP_ATTR_PROTOCOL_LIST 0x0004
#define LE_SDP_ATTR_BROWSE_LIST 0x0005
#define LE_SDP_ATTR_SERVICE_NAME 0x0006
#define LE_SDP_ATTR_PROFILE_LIST 0x0009
#define LE_SDP_ATTR_FEATURES 0x0311

struct sockaddr_l2_local {
    sa_family_t l2_family;
    unsigned short l2_psm;
    uint8_t l2_bdaddr[6];
    unsigned short l2_cid;
    uint8_t l2_bdaddr_type;
};

struct l2cap_options_local {
    uint16_t omtu;
    uint16_t imtu;
    uint16_t flush_to;
    uint8_t mode;
    uint8_t fcs;
    uint8_t max_tx;
    uint16_t txwin_size;
};

#define LE_MAX_SDP_SESSIONS 2
#define LE_MAX_AVDTP_SESSIONS 2
#define LE_MAX_AVRCP_SESSIONS 2

struct le_sdp_record {
    int in_use;
    uint32_t handle;
    int has_class_uuid16;
    uint16_t class_uuid16;
    uint8_t data[2048];
    size_t length;
};

struct le_sdp_session {
    int fd;
    uint8_t buf[2048];
    size_t used;
};

struct le_avdtp_session {
    int fd;
    int media_fd;
    int configured;
    int open;
    int streaming;
    struct sbc_struct sbc;
    int sbc_ready;
    uint8_t signal_buf[2048];
    size_t signal_used;
    uint8_t media_buf[65536];
    size_t media_used;
};

struct le_avrcp_session {
    int fd;
    uint8_t buf[2048];
    size_t used;
};

struct le_profile_sessions {
    struct le_sdp_session sdp[LE_MAX_SDP_SESSIONS];
    struct le_avdtp_session avdtp[LE_MAX_AVDTP_SESSIONS];
    struct le_avrcp_session avrcp[LE_MAX_AVRCP_SESSIONS];
    struct le_sdp_record records[8];
};

static int l2cap_seqpacket_listen(unsigned short psm)
{
    struct sockaddr_l2_local addr;
    int fd;

    fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, LE_BTPROTO_L2CAP);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    addr.l2_psm = psm;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void l2cap_set_imtu(int fd)
{
    struct l2cap_options_local options;
    socklen_t length = sizeof(options);

    if (getsockopt(fd, LE_SOL_L2CAP, LE_L2CAP_OPTIONS, &options, &length) == 0) {
        options.imtu = LE_L2CAP_IMTU;
        (void)setsockopt(fd, LE_SOL_L2CAP, LE_L2CAP_OPTIONS, &options,
                         sizeof(options));
    }
}

/* ---- SDP record serialization ----------------------------------------- */

static size_t sdp_put_uuid16(uint8_t *out, size_t size, size_t offset,
                             uint16_t uuid)
{
    if (offset + 3 > size)
        return offset;
    out[offset] = LE_SDP_DE_UUID16;
    out[offset + 1] = (uint8_t)(uuid >> 8);
    out[offset + 2] = (uint8_t)uuid;
    return offset + 3;
}

static size_t sdp_put_uint16(uint8_t *out, size_t size, size_t offset,
                             uint16_t value)
{
    if (offset + 3 > size)
        return offset;
    out[offset] = LE_SDP_DE_UINT16;
    out[offset + 1] = (uint8_t)(value >> 8);
    out[offset + 2] = (uint8_t)value;
    return offset + 3;
}

static size_t sdp_put_uint32(uint8_t *out, size_t size, size_t offset,
                             uint32_t value)
{
    if (offset + 5 > size)
        return offset;
    out[offset] = LE_SDP_DE_UINT32;
    out[offset + 1] = (uint8_t)(value >> 24);
    out[offset + 2] = (uint8_t)(value >> 16);
    out[offset + 3] = (uint8_t)(value >> 8);
    out[offset + 4] = (uint8_t)value;
    return offset + 5;
}

static size_t sdp_put_text(uint8_t *out, size_t size, size_t offset,
                           const char *text)
{
    size_t length = strlen(text);

    if (length > 255)
        length = 255;
    if (offset + 2 + length > size)
        return offset;
    out[offset] = LE_SDP_DE_TEXT8;
    out[offset + 1] = (uint8_t)length;
    memcpy(out + offset + 2, text, length);
    return offset + 2 + length;
}

static size_t sdp_put_attr(uint8_t *out, size_t size, size_t offset,
                           uint16_t attr)
{
    return sdp_put_uint16(out, size, offset, attr);
}

static size_t sdp_put_uuid_list_attr(uint8_t *out, size_t size, size_t offset,
                                     uint16_t attr, const uint16_t *uuids,
                                     int count)
{
    size_t seq_start;
    size_t start;
    int i;

    offset = sdp_put_attr(out, size, offset, attr);
    if (offset + 2 > size)
        return offset;
    seq_start = offset;
    out[offset] = LE_SDP_DE_SEQUENCE;
    out[offset + 1] = 0;
    offset += 2;
    start = offset;
    for (i = 0; i < count; ++i)
        offset = sdp_put_uuid16(out, size, offset, uuids[i]);
    if (offset - start > 255)
        return offset;
    out[seq_start + 1] = (uint8_t)(offset - start);
    return offset;
}

static size_t sdp_put_browse_list(uint8_t *out, size_t size, size_t offset)
{
    uint16_t browse[1] = { 0x1002 };
    return sdp_put_uuid_list_attr(out, size, offset, LE_SDP_ATTR_BROWSE_LIST,
                                  browse, 1);
}

/* L2CAP + protocol descriptor list: [[L2CAP, psm?], [protocol, version]] */
static size_t sdp_put_protocol_list(uint8_t *out, size_t size, size_t offset,
                                    uint16_t protocol_uuid, uint16_t version)
{
    size_t list_start;
    size_t seq;
    size_t start;

    offset = sdp_put_attr(out, size, offset, LE_SDP_ATTR_PROTOCOL_LIST);
    if (offset + 2 > size)
        return offset;
    list_start = offset;
    out[offset] = LE_SDP_DE_SEQUENCE;
    out[offset + 1] = 0;
    offset += 2;

    seq = offset;
    if (offset + 2 > size)
        return offset;
    out[offset] = LE_SDP_DE_SEQUENCE;
    out[offset + 1] = 0;
    offset += 2;
    start = offset;
    offset = sdp_put_uuid16(out, size, offset, 0x0100); /* L2CAP */
    if (protocol_uuid != 0x0100)
        offset = sdp_put_uint16(out, size, offset, protocol_uuid);
    out[seq + 1] = (uint8_t)(offset - start);

    seq = offset;
    if (offset + 2 > size)
        return offset;
    out[offset] = LE_SDP_DE_SEQUENCE;
    out[offset + 1] = 0;
    offset += 2;
    start = offset;
    offset = sdp_put_uuid16(out, size, offset, protocol_uuid);
    if (version)
        offset = sdp_put_uint16(out, size, offset, version);
    out[seq + 1] = (uint8_t)(offset - start);

    out[list_start + 1] = (uint8_t)(offset - (list_start + 2));
    return offset;
}

static size_t sdp_put_profile_list(uint8_t *out, size_t size, size_t offset,
                                   uint16_t uuid, uint16_t version)
{
    size_t list_start;
    size_t seq;
    size_t start;

    offset = sdp_put_attr(out, size, offset, LE_SDP_ATTR_PROFILE_LIST);
    if (offset + 2 > size)
        return offset;
    list_start = offset;
    out[offset] = LE_SDP_DE_SEQUENCE;
    out[offset + 1] = 0;
    offset += 2;
    seq = offset;
    out[offset] = LE_SDP_DE_SEQUENCE;
    out[offset + 1] = 0;
    offset += 2;
    start = offset;
    offset = sdp_put_uuid16(out, size, offset, uuid);
    offset = sdp_put_uint16(out, size, offset, version);
    out[seq + 1] = (uint8_t)(offset - start);
    out[list_start + 1] = (uint8_t)(offset - (list_start + 2));
    return offset;
}

static void build_record_set(struct le_profile_sessions *sessions,
                             const char *service_name)
{
    uint16_t a2dp_classes[1] = { 0x110b };
    uint16_t avrcp_classes[2] = { 0x110e, 0x110c };
    uint16_t pnp_classes[1] = { 0x1200 };
    uint16_t browse_classes[1] = { 0x1002 };
    struct le_sdp_record *record;

    memset(sessions->records, 0, sizeof(sessions->records));

    record = &sessions->records[0];
    record->in_use = 1;
    record->handle = LE_SDP_RECORD_BROWSE;
    record->has_class_uuid16 = 1;
    record->class_uuid16 = 0x1002;
    {
        size_t offset = 0;
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_HANDLE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset,
                                record->handle);
        offset = sdp_put_uuid_list_attr(record->data, sizeof(record->data),
                                        offset, LE_SDP_ATTR_CLASS_ID_LIST,
                                        browse_classes, 1);
        record->length = offset;
    }

    record = &sessions->records[1];
    record->in_use = 1;
    record->handle = LE_SDP_RECORD_A2DP_SINK;
    record->has_class_uuid16 = 1;
    record->class_uuid16 = 0x110b;
    {
        size_t offset = 0;
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_HANDLE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset,
                                record->handle);
        offset = sdp_put_uuid_list_attr(record->data, sizeof(record->data),
                                        offset, LE_SDP_ATTR_CLASS_ID_LIST,
                                        a2dp_classes, 1);
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_RECORD_STATE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset, 1);
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_SERVICE_NAME);
        offset = sdp_put_text(record->data, sizeof(record->data), offset,
                              service_name[0] ? service_name : "LibreEcho");
        offset = sdp_put_browse_list(record->data, sizeof(record->data),
                                     offset);
        offset = sdp_put_protocol_list(record->data, sizeof(record->data),
                                       offset, 0x0019, 0x0103); /* AVDTP 1.3 */
        offset = sdp_put_profile_list(record->data, sizeof(record->data),
                                      offset, 0x110b, 0x0103); /* A2DP SINK */
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_FEATURES);
        offset = sdp_put_uint16(record->data, sizeof(record->data), offset,
                                0x0001); /* A2DP_SINK role bit */
        record->length = offset;
    }

    record = &sessions->records[2];
    record->in_use = 1;
    record->handle = LE_SDP_RECORD_AVRCP;
    record->has_class_uuid16 = 1;
    record->class_uuid16 = 0x110e;
    {
        size_t offset = 0;
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_HANDLE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset,
                                record->handle);
        offset = sdp_put_uuid_list_attr(record->data, sizeof(record->data),
                                        offset, LE_SDP_ATTR_CLASS_ID_LIST,
                                        avrcp_classes, 2);
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_RECORD_STATE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset, 1);
        offset = sdp_put_browse_list(record->data, sizeof(record->data),
                                     offset);
        offset = sdp_put_protocol_list(record->data, sizeof(record->data),
                                       offset, 0x0017, 0x0106); /* AVRCP 1.6 */
        offset = sdp_put_profile_list(record->data, sizeof(record->data),
                                      offset, 0x110e, 0x0106);
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_FEATURES);
        offset = sdp_put_uint16(record->data, sizeof(record->data), offset,
                                0x0031); /* TG features */
        record->length = offset;
    }

    record = &sessions->records[3];
    record->in_use = 1;
    record->handle = LE_SDP_RECORD_PNP;
    record->has_class_uuid16 = 1;
    record->class_uuid16 = 0x1200;
    {
        size_t offset = 0;
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_HANDLE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset,
                                record->handle);
        offset = sdp_put_uuid_list_attr(record->data, sizeof(record->data),
                                        offset, LE_SDP_ATTR_CLASS_ID_LIST,
                                        pnp_classes, 1);
        offset = sdp_put_attr(record->data, sizeof(record->data), offset,
                              LE_SDP_ATTR_RECORD_STATE);
        offset = sdp_put_uint32(record->data, sizeof(record->data), offset, 1);
        offset = sdp_put_browse_list(record->data, sizeof(record->data),
                                     offset);
        record->length = offset;
    }
}

/* ---- SDP request handling --------------------------------------------- */

static int sdp_write_response(struct le_sdp_session *session,
                              const uint8_t *data, size_t length)
{
    ssize_t written = write(session->fd, data, length);
    return written == (ssize_t)length ? 0 : -1;
}

static void sdp_send_error(struct le_sdp_session *session, uint16_t tid,
                           uint16_t code)
{
    uint8_t response[7];

    response[0] = LE_SDP_PDU_ERROR;
    response[1] = (uint8_t)(tid >> 8);
    response[2] = (uint8_t)tid;
    response[3] = 0;
    response[4] = 2;
    response[5] = (uint8_t)(code >> 8);
    response[6] = (uint8_t)code;
    (void)sdp_write_response(session, response, sizeof(response));
}

static int sdp_match_record(const struct le_sdp_record *record,
                            const uint8_t *pattern, size_t pattern_length)
{
    size_t offset;

    if (pattern_length < 1 || pattern[0] != LE_SDP_DE_SEQUENCE)
        return 0;
    offset = 2;
    while (offset + 3 <= pattern_length && pattern[offset] == LE_SDP_DE_UUID16) {
        uint16_t uuid = (uint16_t)((pattern[offset + 1] << 8) |
                                   pattern[offset + 2]);
        if (uuid == 0xffff) /* wildcard matches everything */
            return 1;
        if (record->has_class_uuid16 && uuid == record->class_uuid16)
            return 1;
        offset += 3;
    }
    return 0;
}

static void sdp_handle_service_search_attr(struct le_profile_sessions *sessions,
                                           struct le_sdp_session *session,
                                           uint16_t tid,
                                           const uint8_t *params,
                                           size_t params_length)
{
    uint8_t response[4096];
    size_t response_offset;
    size_t total_list_bytes = 0;
    int record_matches = 0;
    size_t pattern_length;
    int i;

    if (params_length < 2) {
        sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
        return;
    }
    pattern_length = params[0];
    if (pattern_length + 1 > params_length) {
        sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
        return;
    }

    response[0] = LE_SDP_PDU_SERVICE_SEARCH_ATTR;
    response[1] = (uint8_t)(tid >> 8);
    response[2] = (uint8_t)tid;
    response[3] = 0;
    response[4] = 0;
    response_offset = 7;

    for (i = 0; i < 8; ++i) {
        const struct le_sdp_record *record = &sessions->records[i];
        size_t record_attr_start;

        if (!record->in_use)
            continue;
        if (!sdp_match_record(record, params + 1, pattern_length))
            continue;
        if (record->length + response_offset + 4 > sizeof(response))
            continue;

        record_attr_start = response_offset + 2;
        response[response_offset] = LE_SDP_DE_SEQUENCE;
        response[response_offset + 1] = 0;
        response_offset += 2;
        memcpy(response + response_offset, record->data, record->length);
        response_offset += record->length;
        response[record_attr_start - 1] =
            (uint8_t)(response_offset - record_attr_start);
        record_matches++;
        total_list_bytes += 2 + (response_offset - record_attr_start);
    }

    if (!record_matches) {
        sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
        return;
    }

    response[5] = (uint8_t)(total_list_bytes >> 8);
    response[6] = (uint8_t)total_list_bytes;
    response[3] = (uint8_t)((response_offset - 5) >> 8);
    response[4] = (uint8_t)(response_offset - 5);
    (void)sdp_write_response(session, response, response_offset);
}

static void sdp_handle_service_attr(struct le_profile_sessions *sessions,
                                    struct le_sdp_session *session,
                                    uint16_t tid, const uint8_t *params,
                                    size_t params_length)
{
    uint32_t handle;
    int i;

    if (params_length < 4) {
        sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
        return;
    }
    handle = ((uint32_t)params[0] << 24) | ((uint32_t)params[1] << 16) |
             ((uint32_t)params[2] << 8) | params[3];
    for (i = 0; i < 8; ++i) {
        const struct le_sdp_record *record = &sessions->records[i];
        uint8_t response[4096];
        size_t offset = 0;
        size_t list_start;

        if (!record->in_use || record->handle != handle)
            continue;
        response[offset++] = LE_SDP_PDU_SERVICE_ATTR;
        response[offset++] = (uint8_t)(tid >> 8);
        response[offset++] = (uint8_t)tid;
        response[offset++] = 0;
        response[offset++] = 0;
        list_start = offset;
        response[offset++] = LE_SDP_DE_SEQUENCE;
        response[offset++] = 0;
        if (record->length + offset > sizeof(response))
            continue;
        memcpy(response + offset, record->data, record->length);
        offset += record->length;
        response[list_start + 1] = (uint8_t)(offset - list_start - 2);
        response[3] = (uint8_t)((offset - 5) >> 8);
        response[4] = (uint8_t)(offset - 5);
        (void)sdp_write_response(session, response, offset);
        return;
    }
    sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
}

static void sdp_handle_service_search(struct le_profile_sessions *sessions,
                                      struct le_sdp_session *session,
                                      uint16_t tid, const uint8_t *params,
                                      size_t params_length)
{
    uint8_t response[256];
    size_t offset;
    uint16_t total;
    uint16_t current = 0;
    size_t pattern_length;
    int i;

    if (params_length < 2) {
        sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
        return;
    }
    pattern_length = params[0];
    total = 0;
    for (i = 0; i < 8; ++i) {
        if (sessions->records[i].in_use &&
            sdp_match_record(&sessions->records[i], params + 1, pattern_length))
            total++;
    }

    offset = 0;
    response[offset++] = LE_SDP_PDU_SERVICE_SEARCH;
    response[offset++] = (uint8_t)(tid >> 8);
    response[offset++] = (uint8_t)tid;
    response[offset++] = 0;
    response[offset++] = 0;
    response[offset++] = (uint8_t)(total >> 8);
    response[offset++] = (uint8_t)total;
    response[offset++] = (uint8_t)(current >> 8);
    response[offset++] = (uint8_t)current;
    for (i = 0; i < 8 && current < total && offset + 4 <= sizeof(response);
         ++i) {
        const struct le_sdp_record *record = &sessions->records[i];
        if (!record->in_use)
            continue;
        if (!sdp_match_record(record, params + 1, pattern_length))
            continue;
        response[offset++] = (uint8_t)(record->handle >> 24);
        response[offset++] = (uint8_t)(record->handle >> 16);
        response[offset++] = (uint8_t)(record->handle >> 8);
        response[offset++] = (uint8_t)record->handle;
        current++;
    }
    response[3] = (uint8_t)((offset - 5) >> 8);
    response[4] = (uint8_t)(offset - 5);
    (void)sdp_write_response(session, response, offset);
}

static void sdp_session_read(struct le_profile_sessions *sessions,
                             struct le_sdp_session *session)
{
    uint8_t *buffer = session->buf;
    ssize_t received = read(session->fd, buffer + session->used,
                            sizeof(session->buf) - session->used);

    if (received <= 0) {
        close(session->fd);
        session->fd = -1;
        session->used = 0;
        return;
    }
    session->used += (size_t)received;
    while (session->used >= 5) {
        uint8_t pdu = buffer[0];
        uint16_t tid = (uint16_t)((buffer[1] << 8) | buffer[2]);
        size_t param_length = ((size_t)buffer[3] << 8) | buffer[4];

        if (5 + param_length > session->used)
            break;
        switch (pdu) {
        case LE_SDP_PDU_SERVICE_SEARCH_ATTR:
            sdp_handle_service_search_attr(sessions, session, tid, buffer + 5,
                                           param_length);
            break;
        case LE_SDP_PDU_SERVICE_ATTR:
            sdp_handle_service_attr(sessions, session, tid, buffer + 5,
                                    param_length);
            break;
        case LE_SDP_PDU_SERVICE_SEARCH:
            sdp_handle_service_search(sessions, session, tid, buffer + 5,
                                      param_length);
            break;
        default:
            sdp_send_error(session, tid, LE_SDP_ERROR_INVALID_REQUEST);
            break;
        }
        session->used -= 5 + param_length;
        memmove(buffer, buffer + 5 + param_length, session->used);
    }
}

/* ---- SBC decode and media delivery ------------------------------------ */

static void media_write(struct le_profiles *p, const int16_t *samples,
                        size_t frames)
{
    ssize_t expected;

    if (frames == 0)
        return;
    if (p->media_bus_fd < 0) {
        p->media_bus_fd = open(LE_MEDIA_FIFO, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (p->media_bus_fd < 0)
            return;
    }
    expected = (ssize_t)(frames * 2 * sizeof(int16_t));
    if (write(p->media_bus_fd, samples, (size_t)expected) == expected)
        p->media_frames_written += frames;
}

static void sbc_decode_chunk(struct le_profiles *p,
                             struct le_avdtp_session *session)
{
    size_t offset = 0;

    while (session->media_used - offset >= 4) {
        uint8_t *frame = session->media_buf + offset;
        size_t available = session->media_used - offset;
        ssize_t parsed;
        int16_t pcm[8192];
        size_t pcm_written = 0;

        if (frame[0] != 0x9c) { /* SBC sync word */
            offset++;
            continue;
        }
        if (!session->sbc_ready) {
            if (sbc_init(&session->sbc, 0) < 0)
                break;
            session->sbc_ready = 1;
        } else {
            (void)sbc_reinit(&session->sbc, 0);
        }
        parsed = sbc_parse(&session->sbc, frame, available);
        if (parsed <= 0 || (size_t)parsed > available)
            break;
        if (sbc_decode(&session->sbc, frame, (size_t)parsed, pcm,
                       sizeof(pcm), &pcm_written) < 0) {
            offset++;
            continue;
        }
        offset += (size_t)parsed;
        if (pcm_written && session->sbc.frequency == SBC_FREQ_48000) {
            size_t frames = pcm_written /
                (session->sbc.mode == SBC_MODE_MONO ? 1 : 2);
            int16_t output[LE_MEDIA_CHUNK_FRAMES * 2];
            size_t out_frames = 0;
            size_t i;
            int mono = session->sbc.mode == SBC_MODE_MONO;

            for (i = 0; i < frames && out_frames < LE_MEDIA_CHUNK_FRAMES; ++i) {
                output[out_frames * 2] = pcm[i * (mono ? 1 : 2)];
                output[out_frames * 2 + 1] = mono ? pcm[i] :
                    pcm[i * 2 + 1];
                out_frames++;
            }
            media_write(p, output, out_frames);
        } else if (pcm_written) {
            /* Resample non-48k streams to the fixed 48 kHz media bus with
             * nearest-neighbour sampling; the bus contract does not allow
             * other rates. */
            unsigned int source_rate;
            int16_t output[LE_MEDIA_CHUNK_FRAMES * 2];
            size_t out_frames = 0;
            size_t frames;
            int mono;
            size_t i;

            switch (session->sbc.frequency) {
            case SBC_FREQ_16000: source_rate = 16000; break;
            case SBC_FREQ_32000: source_rate = 32000; break;
            case SBC_FREQ_44100: source_rate = 44100; break;
            default: source_rate = 48000; break;
            }
            mono = session->sbc.mode == SBC_MODE_MONO;
            frames = pcm_written / (mono ? 1 : 2);
            for (i = 0; i < LE_MEDIA_CHUNK_FRAMES; ++i) {
                size_t src = (i * source_rate) / LE_MEDIA_RATE;
                if (src >= frames)
                    break;
                output[out_frames * 2] = pcm[src * (mono ? 1 : 2)];
                output[out_frames * 2 + 1] = mono ? pcm[src] :
                    pcm[src * 2 + 1];
                out_frames++;
            }
            media_write(p, output, out_frames);
        }
    }
    if (offset) {
        session->media_used -= offset;
        memmove(session->media_buf, session->media_buf + offset,
                session->media_used);
    }
}

/* ---- AVDTP signaling --------------------------------------------------- */

static void avdtp_send(struct le_avdtp_session *session, uint8_t transaction,
                       uint8_t message_type, uint8_t signal_id,
                       const uint8_t *payload, size_t payload_length)
{
    uint8_t packet[2048];
    size_t total = 4 + payload_length;

    if (session->fd < 0 || total > sizeof(packet))
        return;
    packet[0] = (uint8_t)((transaction << 4) | LE_AVDTP_PKT_SINGLE);
    packet[1] = (uint8_t)((message_type << 4) | signal_id);
    packet[2] = (uint8_t)(payload_length >> 8);
    packet[3] = (uint8_t)payload_length;
    if (payload_length)
        memcpy(packet + 4, payload, payload_length);
    (void)write(session->fd, packet, total);
}

static void avdtp_accept(struct le_avdtp_session *session, uint8_t transaction,
                         uint8_t signal_id)
{
    avdtp_send(session, transaction, LE_AVDTP_MSG_ACCEPT, signal_id, NULL, 0);
}

static void avdtp_reject(struct le_avdtp_session *session, uint8_t transaction,
                         uint8_t signal_id, uint8_t error)
{
    uint8_t payload[1] = { error };
    avdtp_send(session, transaction, LE_AVDTP_MSG_REJECT, signal_id, payload,
               1);
}

static void avdtp_general_reject(struct le_avdtp_session *session,
                                 uint8_t transaction, uint8_t signal_id)
{
    avdtp_send(session, transaction, LE_AVDTP_MSG_GENERAL_REJECT, signal_id,
               NULL, 0);
}

static void avdtp_handle_discover(struct le_avdtp_session *session,
                                  uint8_t transaction)
{
    uint8_t payload[2];

    payload[0] = (uint8_t)((LE_AVDTP_SEID << 2) | 0x00);
    payload[1] = (uint8_t)((LE_AVDTP_MEDIA_TYPE_AUDIO << 4) | 0x00);
    avdtp_send(session, transaction, LE_AVDTP_MSG_ACCEPT, LE_AVDTP_DISCOVER,
               payload, sizeof(payload));
}

static void avdtp_handle_get_capabilities(struct le_avdtp_session *session,
                                          uint8_t transaction,
                                          uint8_t signal_id)
{
    uint8_t payload[16];
    size_t offset = 0;

    payload[offset++] = LE_AVDTP_CAT_MEDIA_TRANSPORT;
    payload[offset++] = 0;
    payload[offset++] = LE_AVDTP_CAT_MEDIA_CODEC;
    payload[offset++] = 6;
    payload[offset++] = (uint8_t)((LE_AVDTP_MEDIA_TYPE_AUDIO << 4) |
                                  LE_AVDTP_CODEC_SBC);
    payload[offset++] = 0xff; /* 16/32/44.1/48 kHz, all channel modes */
    payload[offset++] = 0xff; /* all block lengths/subbands/allocation */
    payload[offset++] = 2;    /* min bitpool */
    payload[offset++] = 53;   /* max bitpool */
    avdtp_send(session, transaction, LE_AVDTP_MSG_ACCEPT, signal_id, payload,
               offset);
}

static void avdtp_handle_set_configuration(struct le_profiles *p,
                                           struct le_avdtp_session *session,
                                           uint8_t transaction,
                                           const uint8_t *payload,
                                           size_t length)
{
    size_t offset;
    int codec_seen = 0;

    if (length < 2) {
        avdtp_reject(session, transaction, LE_AVDTP_SET_CONFIGURATION,
                     LE_AVDTP_ERR_BAD_ACP_SEID);
        return;
    }
    if (session->configured || session->open || session->streaming) {
        avdtp_reject(session, transaction, LE_AVDTP_SET_CONFIGURATION,
                     LE_AVDTP_ERR_BAD_STATE);
        return;
    }
    offset = 2; /* skip ACP_SEID + INT_SEID */
    while (offset + 2 <= length) {
        uint8_t category = payload[offset];
        uint8_t cap_length = payload[offset + 1];
        const uint8_t *value = payload + offset + 2;

        if (offset + 2 + cap_length > length)
            break;
        if (category == LE_AVDTP_CAT_MEDIA_CODEC && cap_length >= 6) {
            uint8_t media_type = (uint8_t)(value[0] >> 4);
            uint8_t codec = value[1];

            if (media_type != LE_AVDTP_MEDIA_TYPE_AUDIO ||
                codec != LE_AVDTP_CODEC_SBC) {
                avdtp_reject(session, transaction, LE_AVDTP_SET_CONFIGURATION,
                             LE_AVDTP_ERR_UNSUP_CAPABILITY);
                return;
            }
            /* The SBC codec information element is validated by reinitialising
             * the decoder from the negotiated configuration. */
            if (!session->sbc_ready && sbc_init(&session->sbc, 0) < 0) {
                avdtp_reject(session, transaction, LE_AVDTP_SET_CONFIGURATION,
                             LE_AVDTP_ERR_UNSUP_CAPABILITY);
                return;
            }
            session->sbc_ready = 1;
            switch ((value[2] >> 4) & 0x0f) {
            case 0x08: session->sbc.frequency = SBC_FREQ_16000; break;
            case 0x04: session->sbc.frequency = SBC_FREQ_32000; break;
            case 0x02: session->sbc.frequency = SBC_FREQ_44100; break;
            case 0x01: session->sbc.frequency = SBC_FREQ_48000; break;
            default: session->sbc.frequency = SBC_FREQ_48000; break;
            }
            switch (value[2] & 0x0f) {
            case 0x08: session->sbc.mode = SBC_MODE_MONO; break;
            case 0x04: session->sbc.mode = SBC_MODE_DUAL_CHANNEL; break;
            case 0x02: session->sbc.mode = SBC_MODE_STEREO; break;
            case 0x01: session->sbc.mode = SBC_MODE_JOINT_STEREO; break;
            default: session->sbc.mode = SBC_MODE_JOINT_STEREO; break;
            }
            switch ((value[3] >> 4) & 0x0f) {
            case 0x08: session->sbc.blocks = SBC_BLK_4; break;
            case 0x04: session->sbc.blocks = SBC_BLK_8; break;
            case 0x02: session->sbc.blocks = SBC_BLK_12; break;
            case 0x01: session->sbc.blocks = SBC_BLK_16; break;
            default: session->sbc.blocks = SBC_BLK_16; break;
            }
            session->sbc.subbands = (value[3] & 0x02) ? SBC_SB_8 : SBC_SB_4;
            session->sbc.allocation = (value[3] & 0x01) ? SBC_AM_SNR :
                SBC_AM_LOUDNESS;
            session->sbc.bitpool = value[5] > 53 ? 53 : value[5];
            codec_seen = 1;
        } else if (category != LE_AVDTP_CAT_MEDIA_TRANSPORT) {
            avdtp_reject(session, transaction, LE_AVDTP_SET_CONFIGURATION,
                         LE_AVDTP_ERR_BAD_CAPABILITIES);
            return;
        }
        offset += 2 + cap_length;
    }
    if (!codec_seen) {
        avdtp_reject(session, transaction, LE_AVDTP_SET_CONFIGURATION,
                     LE_AVDTP_ERR_BAD_CAPABILITIES);
        return;
    }
    session->configured = 1;
    avdtp_accept(session, transaction, LE_AVDTP_SET_CONFIGURATION);
    le_log_info("btd-profiles: a2dp stream configured");
    (void)p;
}

static void avdtp_handle_open(struct le_avdtp_session *session,
                              uint8_t transaction)
{
    if (!session->configured) {
        avdtp_reject(session, transaction, LE_AVDTP_OPEN,
                     LE_AVDTP_ERR_BAD_STATE);
        return;
    }
    session->open = 1;
    session->streaming = 0;
    avdtp_accept(session, transaction, LE_AVDTP_OPEN);
}

static void avdtp_handle_start(struct le_avdtp_session *session,
                               uint8_t transaction)
{
    if (!session->open && !session->streaming) {
        avdtp_reject(session, transaction, LE_AVDTP_START,
                     LE_AVDTP_ERR_BAD_STATE);
        return;
    }
    session->streaming = 1;
    session->media_used = 0;
    avdtp_accept(session, transaction, LE_AVDTP_START);
}

static void avdtp_handle_suspend(struct le_avdtp_session *session,
                                 uint8_t transaction)
{
    if (!session->streaming) {
        avdtp_reject(session, transaction, LE_AVDTP_SUSPEND,
                     LE_AVDTP_ERR_BAD_STATE);
        return;
    }
    session->streaming = 0;
    avdtp_accept(session, transaction, LE_AVDTP_SUSPEND);
}

static void avdtp_handle_close(struct le_avdtp_session *session,
                               uint8_t transaction)
{
    session->streaming = 0;
    session->open = 0;
    if (session->media_fd >= 0) {
        close(session->media_fd);
        session->media_fd = -1;
    }
    avdtp_accept(session, transaction, LE_AVDTP_CLOSE);
}

static void avdtp_handle_abort(struct le_avdtp_session *session,
                               uint8_t transaction)
{
    session->configured = 0;
    session->open = 0;
    session->streaming = 0;
    if (session->media_fd >= 0) {
        close(session->media_fd);
        session->media_fd = -1;
    }
    session->media_used = 0;
    avdtp_accept(session, transaction, LE_AVDTP_ABORT);
}

static void avdtp_dispatch(struct le_profiles *p,
                           struct le_avdtp_session *session,
                           const uint8_t *data, size_t length)
{
    uint8_t transaction;
    uint8_t message_type;
    uint8_t signal_id;
    size_t payload_length;
    const uint8_t *payload;

    if (length < 4)
        return;
    transaction = (uint8_t)(data[0] >> 4);
    message_type = (uint8_t)((data[1] >> 4) & 0x0f);
    signal_id = (uint8_t)(data[1] & 0x0f);
    payload_length = ((size_t)data[2] << 8) | data[3];
    payload = data + 4;
    if (4 + payload_length > length)
        payload_length = length - 4;
    if (message_type != LE_AVDTP_MSG_COMMAND)
        return;

    switch (signal_id) {
    case LE_AVDTP_DISCOVER:
        avdtp_handle_discover(session, transaction);
        break;
    case LE_AVDTP_GET_CAPABILITIES:
    case LE_AVDTP_GET_ALL_CAPABILITIES:
        avdtp_handle_get_capabilities(session, transaction, signal_id);
        break;
    case LE_AVDTP_SET_CONFIGURATION:
        avdtp_handle_set_configuration(p, session, transaction, payload,
                                       payload_length);
        break;
    case LE_AVDTP_GET_CONFIGURATION:
        avdtp_accept(session, transaction, signal_id);
        break;
    case LE_AVDTP_OPEN:
        avdtp_handle_open(session, transaction);
        break;
    case LE_AVDTP_START:
        avdtp_handle_start(session, transaction);
        break;
    case LE_AVDTP_SUSPEND:
        avdtp_handle_suspend(session, transaction);
        break;
    case LE_AVDTP_CLOSE:
        avdtp_handle_close(session, transaction);
        break;
    case LE_AVDTP_ABORT:
        avdtp_handle_abort(session, transaction);
        break;
    case LE_AVDTP_DELAY_REPORT:
        avdtp_accept(session, transaction, signal_id);
        break;
    case LE_AVDTP_SECURITY_CONTROL:
        avdtp_reject(session, transaction, signal_id,
                     LE_AVDTP_ERR_UNSUP_CAPABILITY);
        break;
    case LE_AVDTP_RECONFIGURE:
        avdtp_reject(session, transaction, signal_id, LE_AVDTP_ERR_BAD_STATE);
        break;
    default:
        avdtp_general_reject(session, transaction, signal_id);
        break;
    }
}

static void avdtp_session_read(struct le_profiles *p,
                               struct le_avdtp_session *session)
{
    uint8_t *buffer = session->signal_buf;
    ssize_t received = read(session->fd, buffer + session->signal_used,
                            sizeof(session->signal_buf) - session->signal_used);

    if (received <= 0) {
        close(session->fd);
        session->fd = -1;
        if (session->media_fd >= 0) {
            close(session->media_fd);
            session->media_fd = -1;
        }
        session->streaming = 0;
        session->signal_used = 0;
        return;
    }
    session->signal_used += (size_t)received;
    while (session->signal_used >= 4) {
        uint8_t packet_type = buffer[0] & 0x03;
        size_t signal_length = ((size_t)buffer[2] << 8) | buffer[3];
        size_t total = 4 + signal_length;

        if (packet_type != LE_AVDTP_PKT_SINGLE &&
            packet_type != LE_AVDTP_PKT_END) {
            /* Fragmented signaling is not expected from compliant peers for
             * the small responses this endpoint returns; drop and reset. */
            session->signal_used = 0;
            return;
        }
        if (total > sizeof(session->signal_buf)) {
            session->signal_used = 0;
            return;
        }
        if (session->signal_used < total)
            break;
        avdtp_dispatch(p, session, buffer, total);
        session->signal_used -= total;
        memmove(buffer, buffer + total, session->signal_used);
    }
}

static void avdtp_media_read(struct le_profiles *p,
                             struct le_avdtp_session *session)
{
    uint8_t packet[LE_L2CAP_IMTU + 64];
    ssize_t received = read(session->media_fd, packet, sizeof(packet));

    if (received <= 0) {
        if (session->media_fd >= 0)
            close(session->media_fd);
        session->media_fd = -1;
        session->streaming = 0;
        return;
    }
    if (!session->streaming)
        return;
    if (received <= 13) /* 12-byte RTP header + 1-byte SBC payload header */
        return;
    {
        /* The SBC media payload follows the fixed RTP header and the one-byte
         * fragmentation descriptor defined by the A2DP SBC payload format. */
        size_t payload_offset = 13;
        size_t payload_length = (size_t)received - payload_offset;

        if (payload_length > sizeof(session->media_buf) - session->media_used)
            payload_length = sizeof(session->media_buf) - session->media_used;
        memcpy(session->media_buf + session->media_used,
               packet + payload_offset, payload_length);
        session->media_used += payload_length;
        sbc_decode_chunk(p, session);
    }
}

/* ---- AVRCP over AVCTP --------------------------------------------------- */

static void avrcp_send_pdu(struct le_avrcp_session *session, uint8_t transaction,
                           uint8_t pdu_id, uint8_t status,
                           const uint8_t *payload, size_t payload_length)
{
    uint8_t packet[256];
    size_t offset = 0;

    if (payload_length + 14 > sizeof(packet))
        payload_length = sizeof(packet) - 14;
    packet[offset++] = (uint8_t)((transaction << 4) | 0x00); /* single, response */
    packet[offset++] = (uint8_t)(LE_AVRCP_PID_CONTROL >> 8);
    packet[offset++] = (uint8_t)(LE_AVRCP_PID_CONTROL & 0xff);
    packet[offset++] = status;               /* AV/C response ctype */
    packet[offset++] = 0x48;                 /* panel subunit id + type */
    packet[offset++] = 0x00;                 /* vendor dependent opcode */
    packet[offset++] = (uint8_t)((LE_AVRCP_COMPANY_BLUETOOTH >> 16) & 0xff);
    packet[offset++] = (uint8_t)((LE_AVRCP_COMPANY_BLUETOOTH >> 8) & 0xff);
    packet[offset++] = (uint8_t)(LE_AVRCP_COMPANY_BLUETOOTH & 0xff);
    packet[offset++] = pdu_id;
    packet[offset++] = 0x00;                 /* packet type: single */
    packet[offset++] = (uint8_t)(payload_length >> 8);
    packet[offset++] = (uint8_t)payload_length;
    if (payload_length)
        memcpy(packet + offset, payload, payload_length);
    (void)write(session->fd, packet, offset + payload_length);
}

static void avrcp_send_passthrough(struct le_avrcp_session *session,
                                   uint8_t transaction, uint8_t opcode,
                                   uint8_t state_flag)
{
    uint8_t packet[7];
    size_t offset = 0;

    packet[offset++] = (uint8_t)((transaction << 4) | 0x00);
    packet[offset++] = (uint8_t)(LE_AVRCP_PID_CONTROL >> 8);
    packet[offset++] = (uint8_t)(LE_AVRCP_PID_CONTROL & 0xff);
    packet[offset++] = LE_AVRCP_STATUS_ACCEPTED;
    packet[offset++] = 0x48;
    packet[offset++] = (uint8_t)(opcode | state_flag);
    packet[offset++] = 0x00; /* no operands */
    (void)write(session->fd, packet, offset);
}

static void avrcp_session_read(struct le_avrcp_session *session)
{
    uint8_t *buffer = session->buf;
    ssize_t received = read(session->fd, buffer + session->used,
                            sizeof(session->buf) - session->used);

    if (received <= 0) {
        close(session->fd);
        session->fd = -1;
        session->used = 0;
        return;
    }
    session->used += (size_t)received;
    while (session->used >= 6) {
        uint8_t transaction = (uint8_t)(buffer[0] >> 4);
        uint8_t is_command = (uint8_t)(buffer[0] >> 2) & 0x01;
        uint16_t profile_id = (uint16_t)((buffer[1] << 8) | buffer[2]);
        uint8_t ctype = buffer[3];
        uint8_t subunit = buffer[4];
        uint8_t opcode = buffer[5];

        (void)ctype;
        if (!is_command || profile_id != LE_AVRCP_PID_CONTROL) {
            session->used = 0;
            return;
        }
        if ((subunit >> 3) != 0x09) { /* panel subunit only */
            session->used = 0;
            return;
        }
        if (opcode == 0x7c) { /* vendor dependent */
            uint8_t pdu_id;
            size_t pdu_length;

            if (session->used < 13)
                break;
            pdu_id = buffer[9];
            pdu_length = ((size_t)buffer[11] << 8) | buffer[12];
            if (13 + pdu_length > session->used)
                break;
            switch (pdu_id) {
            case LE_AVRCP_PDU_GET_CAPABILITIES: {
                uint8_t payload[16];
                size_t offset = 0;

                payload[offset++] = 0x02; /* capability id: company */
                payload[offset++] = 1;
                payload[offset++] = (uint8_t)((LE_AVRCP_COMPANY_BLUETOOTH >> 16) & 0xff);
                payload[offset++] = (uint8_t)((LE_AVRCP_COMPANY_BLUETOOTH >> 8) & 0xff);
                payload[offset++] = (uint8_t)(LE_AVRCP_COMPANY_BLUETOOTH & 0xff);
                payload[offset++] = 0x03; /* capability id: events */
                payload[offset++] = 1;
                payload[offset++] = LE_AVRCP_EVENT_PLAYBACK_STATUS;
                avrcp_send_pdu(session, transaction, pdu_id,
                               LE_AVRCP_STATUS_RESPONSE, payload, offset);
                break;
            }
            case LE_AVRCP_PDU_GET_ELEMENT_ATTRIBUTES: {
                uint8_t payload[4] = { 0, 0, 0, 0 };
                avrcp_send_pdu(session, transaction, pdu_id,
                               LE_AVRCP_STATUS_RESPONSE, payload, 4);
                break;
            }
            case LE_AVRCP_PDU_REGISTER_NOTIFICATION: {
                /* Respond with STOPPED playback status; the event never
                 * changes for a sink that has no local transport state. */
                uint8_t payload[1] = { 0x00 };
                avrcp_send_pdu(session, transaction, pdu_id,
                               LE_AVRCP_STATUS_INTERIM, payload, 1);
                break;
            }
            default:
                avrcp_send_pdu(session, transaction, pdu_id,
                               LE_AVRCP_STATUS_NOT_IMPLEMENTED, NULL, 0);
                break;
            }
            session->used -= 13 + pdu_length;
            memmove(buffer, buffer + 13 + pdu_length, session->used);
            continue;
        }
        /* AV/C passthrough (play/pause/stop etc.) */
        {
            uint8_t operand_length = buffer[6];
            if (7 + operand_length > session->used)
                break;
            avrcp_send_passthrough(session, transaction, opcode & 0x7f,
                                   opcode & 0x80);
            session->used -= 7 + operand_length;
            memmove(buffer, buffer + 7 + operand_length, session->used);
        }
    }
}

/* ---- Public entry points ---------------------------------------------- */

int le_profile_open(struct le_profiles *p, const char *service_name)
{
    struct le_profile_sessions *sessions;
    int fd;

    memset(p, 0, sizeof(*p));
    p->sdp_listener = -1;
    p->avdtp_listener = -1;
    p->avrcp_listener = -1;
    p->media_bus_fd = -1;
    snprintf(p->service_name, sizeof(p->service_name), "%s",
             service_name && service_name[0] ? service_name : "LibreEcho");

    sessions = calloc(1, sizeof(*sessions));
    if (!sessions)
        return -1;
    p->sessions = sessions;
    build_record_set(sessions, p->service_name);

    fd = l2cap_seqpacket_listen(LE_PSM_SDP);
    if (fd >= 0) {
        p->sdp_listener = fd;
        p->registered_sdp = 1;
    } else {
        le_log_warn("btd-profiles: SDP listener failed: %s", strerror(errno));
    }
    fd = l2cap_seqpacket_listen(LE_PSM_AVDTP);
    if (fd >= 0) {
        p->avdtp_listener = fd;
        p->registered_a2dp_sink = 1;
    } else {
        le_log_warn("btd-profiles: AVDTP listener failed: %s", strerror(errno));
    }
    fd = l2cap_seqpacket_listen(LE_PSM_AVRCP);
    if (fd >= 0) {
        p->avrcp_listener = fd;
        p->registered_avrcp = 1;
    } else {
        le_log_warn("btd-profiles: AVRCP listener failed: %s", strerror(errno));
    }

    le_log_info("btd-profiles: sdp=%s avdtp=%s avrcp=%s",
                p->registered_sdp ? "listening" : "unavailable",
                p->registered_a2dp_sink ? "listening" : "unavailable",
                p->registered_avrcp ? "listening" : "unavailable");
    return p->registered_sdp || p->registered_a2dp_sink || p->registered_avrcp
        ? 0 : -1;
}

void le_profile_close(struct le_profiles *p)
{
    struct le_profile_sessions *sessions = p->sessions;
    int i;

    if (p->sdp_listener >= 0) close(p->sdp_listener);
    if (p->avdtp_listener >= 0) close(p->avdtp_listener);
    if (p->avrcp_listener >= 0) close(p->avrcp_listener);
    if (p->media_bus_fd >= 0) close(p->media_bus_fd);
    if (sessions) {
        for (i = 0; i < LE_MAX_SDP_SESSIONS; ++i)
            if (sessions->sdp[i].fd >= 0) close(sessions->sdp[i].fd);
        for (i = 0; i < LE_MAX_AVDTP_SESSIONS; ++i) {
            if (sessions->avdtp[i].fd >= 0) close(sessions->avdtp[i].fd);
            if (sessions->avdtp[i].media_fd >= 0) close(sessions->avdtp[i].media_fd);
            if (sessions->avdtp[i].sbc_ready) sbc_finish(&sessions->avdtp[i].sbc);
        }
        for (i = 0; i < LE_MAX_AVRCP_SESSIONS; ++i)
            if (sessions->avrcp[i].fd >= 0) close(sessions->avrcp[i].fd);
        free(sessions);
    }
    memset(p, 0, sizeof(*p));
    p->sdp_listener = -1;
    p->avdtp_listener = -1;
    p->avrcp_listener = -1;
    p->media_bus_fd = -1;
}

int le_profile_poll_setup(struct le_profiles *p, struct pollfd *pollfds,
                          int max_fds, int *fd_map)
{
    struct le_profile_sessions *sessions = p->sessions;
    int count = 0;
    int i;

    if (!sessions)
        return 0;
    if (p->sdp_listener >= 0 && count < max_fds) {
        pollfds[count].fd = p->sdp_listener;
        pollfds[count].events = POLLIN;
        fd_map[count] = 1;
        count++;
    }
    if (p->avdtp_listener >= 0 && count < max_fds) {
        pollfds[count].fd = p->avdtp_listener;
        pollfds[count].events = POLLIN;
        fd_map[count] = 2;
        count++;
    }
    if (p->avrcp_listener >= 0 && count < max_fds) {
        pollfds[count].fd = p->avrcp_listener;
        pollfds[count].events = POLLIN;
        fd_map[count] = 3;
        count++;
    }
    for (i = 0; i < LE_MAX_SDP_SESSIONS && count < max_fds; ++i) {
        if (sessions->sdp[i].fd >= 0) {
            pollfds[count].fd = sessions->sdp[i].fd;
            pollfds[count].events = POLLIN;
            fd_map[count] = 10 + i;
            count++;
        }
    }
    for (i = 0; i < LE_MAX_AVDTP_SESSIONS && count < max_fds; ++i) {
        if (sessions->avdtp[i].fd >= 0) {
            pollfds[count].fd = sessions->avdtp[i].fd;
            pollfds[count].events = POLLIN;
            fd_map[count] = 20 + i;
            count++;
        }
        if (sessions->avdtp[i].media_fd >= 0 && count < max_fds) {
            pollfds[count].fd = sessions->avdtp[i].media_fd;
            pollfds[count].events = POLLIN;
            fd_map[count] = 30 + i;
            count++;
        }
    }
    for (i = 0; i < LE_MAX_AVRCP_SESSIONS && count < max_fds; ++i) {
        if (sessions->avrcp[i].fd >= 0) {
            pollfds[count].fd = sessions->avrcp[i].fd;
            pollfds[count].events = POLLIN;
            fd_map[count] = 40 + i;
            count++;
        }
    }
    return count;
}

void le_profile_poll_events(struct le_profiles *p, const struct pollfd *pollfds,
                            const int *fd_map, int count)
{
    struct le_profile_sessions *sessions = p->sessions;
    int i;

    if (!sessions)
        return;
    for (i = 0; i < count; ++i) {
        if (!(pollfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
            continue;
        switch (fd_map[i]) {
        case 1: {
            int client = accept(p->sdp_listener, NULL, NULL);
            int slot;
            if (client < 0)
                break;
            for (slot = 0; slot < LE_MAX_SDP_SESSIONS; ++slot)
                if (sessions->sdp[slot].fd < 0)
                    break;
            if (slot == LE_MAX_SDP_SESSIONS) {
                close(client);
                break;
            }
            sessions->sdp[slot].fd = client;
            sessions->sdp[slot].used = 0;
            break;
        }
        case 2: {
            /* AVDTP PSM carries both the signaling channel and, after the
             * stream is opened, the media transport channel.  The first free
             * session takes the connection; an open stream waiting for media
             * claims it instead. */
            int client = accept(p->avdtp_listener, NULL, NULL);
            int slot;
            if (client < 0)
                break;
            for (slot = 0; slot < LE_MAX_AVDTP_SESSIONS; ++slot) {
                if (sessions->avdtp[slot].fd >= 0 &&
                    sessions->avdtp[slot].open &&
                    sessions->avdtp[slot].media_fd < 0) {
                    sessions->avdtp[slot].media_fd = client;
                    l2cap_set_imtu(client);
                    break;
                }
            }
            if (slot == LE_MAX_AVDTP_SESSIONS) {
                for (slot = 0; slot < LE_MAX_AVDTP_SESSIONS; ++slot)
                    if (sessions->avdtp[slot].fd < 0)
                        break;
                if (slot == LE_MAX_AVDTP_SESSIONS) {
                    close(client);
                    break;
                }
                memset(&sessions->avdtp[slot], 0,
                       sizeof(sessions->avdtp[slot]));
                sessions->avdtp[slot].fd = client;
                sessions->avdtp[slot].media_fd = -1;
            }
            break;
        }
        case 3: {
            int client = accept(p->avrcp_listener, NULL, NULL);
            int slot;
            if (client < 0)
                break;
            for (slot = 0; slot < LE_MAX_AVRCP_SESSIONS; ++slot)
                if (sessions->avrcp[slot].fd < 0)
                    break;
            if (slot == LE_MAX_AVRCP_SESSIONS) {
                close(client);
                break;
            }
            sessions->avrcp[slot].fd = client;
            sessions->avrcp[slot].used = 0;
            break;
        }
        default:
            if (fd_map[i] >= 10 && fd_map[i] < 10 + LE_MAX_SDP_SESSIONS) {
                sdp_session_read(sessions,
                                 &sessions->sdp[fd_map[i] - 10]);
            } else if (fd_map[i] >= 20 && fd_map[i] < 20 + LE_MAX_AVDTP_SESSIONS) {
                avdtp_session_read(p, &sessions->avdtp[fd_map[i] - 20]);
            } else if (fd_map[i] >= 30 && fd_map[i] < 30 + LE_MAX_AVDTP_SESSIONS) {
                avdtp_media_read(p, &sessions->avdtp[fd_map[i] - 30]);
            } else if (fd_map[i] >= 40 && fd_map[i] < 40 + LE_MAX_AVRCP_SESSIONS) {
                avrcp_session_read(&sessions->avrcp[fd_map[i] - 40]);
            }
            break;
        }
    }
    {
        int active = 0;
        int i2;
        for (i2 = 0; i2 < LE_MAX_AVDTP_SESSIONS; ++i2)
            if (sessions->avdtp[i2].streaming)
                active = 1;
        p->stream_active = active;
    }
}

int le_profile_registered_sdp(const struct le_profiles *p)
{
    return p->registered_sdp;
}

int le_profile_registered_a2dp_sink(const struct le_profiles *p)
{
    return p->registered_a2dp_sink;
}

int le_profile_registered_avrcp(const struct le_profiles *p)
{
    return p->registered_avrcp;
}

int le_profile_stream_active(const struct le_profiles *p)
{
    return p->stream_active;
}
