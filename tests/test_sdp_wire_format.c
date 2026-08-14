/*
 * Host-side regression test for the LibreEcho SDP server wire format.
 *
 * Drives the real profile-layer SDP handlers through the test seam over a
 * socketpair (no Bluetooth controller required).  Covers the defects found in
 * the first 0.12.0 acceptance pass:
 *
 *   1. Response PDU identifiers must be the *response* ids
 *      (0x03 / 0x05 / 0x07), never the request ids (0x02 / 0x04 / 0x06).
 *   2. The ServiceSearchPattern is a DataElementSequence whose length comes
 *      from the sequence size descriptor, not the first raw byte.
 *   3. A ServiceSearchAttribute with no matching record must return a valid
 *      empty AttributeListsList, not an error PDU.
 *   4. The response is framed as an outer DES wrapping each matching record
 *      list plus a trailing one-byte null continuation state.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adapter/bt_profile.h"

/* PDU identifiers from the SDP specification. */
#define SDP_PDU_ERROR_RSP 0x01
#define SDP_PDU_SERVICE_SEARCH_RSP 0x03
#define SDP_PDU_SERVICE_ATTR_RSP 0x05
#define SDP_PDU_SERVICE_SEARCH_ATTR_RSP 0x07
#define SDP_PDU_SERVICE_SEARCH_REQ 0x02
#define SDP_PDU_SERVICE_ATTR_REQ 0x04
#define SDP_PDU_SERVICE_SEARCH_ATTR_REQ 0x06

#define RESP_MAX 4096

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Build a ServiceSearchAttribute request: PDU 0x06, ServiceSearchPattern is
 * a DES of one UUID16 (class id), MaximumAttributeByteCount, AttributeIDList
 * wildcard, null continuation. */
static size_t build_search_attr(uint8_t *out, uint16_t tid, uint16_t uuid)
{
    size_t offset = 5;

    out[0] = SDP_PDU_SERVICE_SEARCH_ATTR_REQ;
    out[1] = (uint8_t)(tid >> 8);
    out[2] = (uint8_t)tid;
    /* ServiceSearchPattern: DES{ UUID16 } */
    out[offset++] = 0x35; /* DES, size index 5 (1-byte size) */
    out[offset++] = 0x03; /* content = 3 bytes */
    out[offset++] = 0x19; /* UUID16 */
    out[offset++] = (uint8_t)(uuid >> 8);
    out[offset++] = (uint8_t)uuid;
    /* MaximumServiceRecordCount */
    out[offset++] = 0x00;
    out[offset++] = 0x0c;
    /* AttributeIDList: DES{ uint32 0x0000ffff } */
    out[offset++] = 0x35;
    out[offset++] = 0x05;
    out[offset++] = 0x0a;
    out[offset++] = 0x00;
    out[offset++] = 0x00;
    out[offset++] = 0xff;
    out[offset++] = 0xff;
    /* continuation state */
    out[offset++] = 0x00;
    out[3] = (uint8_t)((offset - 5) >> 8);
    out[4] = (uint8_t)(offset - 5);
    return offset;
}

/* Build a ServiceSearchAttribute request whose ServiceSearchPattern carries a
 * full 128-bit Bluetooth base UUID (type 0x1c) — the form BlueZ sdptool uses
 * for browse groups and class UUIDs.  The 16-bit short form occupies bytes
 * 2..3 of the 16-byte UUID payload (Bluetooth base-UUID convention). */
static size_t build_search_attr_uuid128(uint8_t *out, uint16_t tid,
                                        uint16_t short_uuid)
{
    size_t offset = 5;
    size_t uuid_start;
    static const uint8_t base[16] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb,
    };

    out[0] = SDP_PDU_SERVICE_SEARCH_ATTR_REQ;
    out[1] = (uint8_t)(tid >> 8);
    out[2] = (uint8_t)tid;
    /* ServiceSearchPattern: DES{ UUID128 } */
    out[offset++] = 0x35; /* DES, 1-byte size */
    out[offset++] = 0x11; /* content = 17 bytes */
    out[offset++] = 0x1c; /* UUID128 */
    uuid_start = offset;
    memcpy(out + offset, base, sizeof(base));
    /* Patch the short form into UUID bytes 2..3. */
    out[uuid_start + 2] = (uint8_t)(short_uuid >> 8);
    out[uuid_start + 3] = (uint8_t)short_uuid;
    offset += sizeof(base);
    /* MaximumServiceRecordCount */
    out[offset++] = 0x00;
    out[offset++] = 0x0c;
    /* AttributeIDList: DES{ uint32 0x0000ffff } */
    out[offset++] = 0x35;
    out[offset++] = 0x05;
    out[offset++] = 0x0a;
    out[offset++] = 0x00;
    out[offset++] = 0x00;
    out[offset++] = 0xff;
    out[offset++] = 0xff;
    /* continuation state */
    out[offset++] = 0x00;
    out[3] = (uint8_t)((offset - 5) >> 8);
    out[4] = (uint8_t)(offset - 5);
    return offset;
}

int main(void)
{
    struct le_profiles profiles;
    uint8_t request[64];
    uint8_t response[RESP_MAX];
    ssize_t n;

    if (le_profile_test_init(&profiles, "LibreEcho") != 0) {
        printf("FAIL: test init\n");
        return 1;
    }

    /* --- 1. ServiceSearchAttribute for Audio Sink (0x110b) --- */
    {
        size_t len = build_search_attr(request, 0x0101, 0x110b);
        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 7, "search-attr returns a response");
        check(n >= 7 && response[0] == SDP_PDU_SERVICE_SEARCH_ATTR_RSP,
              "search-attr response PDU id is 0x07 (not the request id)");
        check(n >= 7 && be16(response + 1) == 0x0101,
              "search-attr echoes the transaction id");
        if (n >= 7) {
            uint16_t plen = be16(response + 3);
            check(5 + (ssize_t)plen == n,
                  "search-attr parameter length matches the PDU body");
            /* Payload must be an outer DES followed by a 1-byte null
             * continuation state. */
            check(n >= 6 && response[n - 1] == 0x00,
                  "search-attr ends with a null continuation state");
            check(n >= 6 && (response[5] == 0x35 || response[5] == 0x36),
                  "search-attr AttributeListsList is a DataElementSequence");
        }
    }

    /* --- 2. Empty result is a valid empty list, not an error PDU --- */
    {
        size_t len = build_search_attr(request, 0x0102, 0x9999);
        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 5, "no-match search-attr returns a response");
        check(n >= 5 && response[0] == SDP_PDU_SERVICE_SEARCH_ATTR_RSP,
              "no-match search-attr returns a success PDU, not an error");
        if (n >= 5) {
            uint16_t plen = be16(response + 3);
            check(5 + (ssize_t)plen == n,
                  "no-match parameter length matches the PDU body");
        }
    }

    /* --- 2b. PublicBrowseRoot group query (what `sdptool browse` sends) --- */
    {
        size_t len = build_search_attr(request, 0x0106, 0x1002);
        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 7, "browse-group search-attr returns a response");
        check(n >= 7 && response[0] == SDP_PDU_SERVICE_SEARCH_ATTR_RSP,
              "browse-group search-attr returns a success PDU");
        if (n >= 7) {
            uint16_t plen = be16(response + 3);
            check(5 + (ssize_t)plen == n,
                  "browse-group parameter length matches the PDU body");
            /* The PublicBrowseRoot group must match at least one record. */
            check(n >= 8 && response[5] == 0x35 && response[6] > 0,
                  "browse-group query returns a non-empty AttributeListsList");
        }
    }

    /* --- 2c. UUID128 browse group (the form BlueZ sdptool sends) --- */
    {
        size_t len = build_search_attr_uuid128(request, 0x0107, 0x1002);
        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 7, "uuid128 browse-group returns a response");
        check(n >= 7 && response[0] == SDP_PDU_SERVICE_SEARCH_ATTR_RSP,
              "uuid128 browse-group returns a success PDU");
        if (n >= 7) {
            uint16_t plen = be16(response + 3);
            check(5 + (ssize_t)plen == n,
                  "uuid128 browse-group parameter length matches");
            check(n >= 8 && response[5] == 0x35 && response[6] > 0,
                  "uuid128 browse-group returns a non-empty AttributeListsList");
        }
    }

    /* --- 3. ServiceSearch returns the response PDU id 0x03 --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x0103;
        size_t len;

        request[0] = SDP_PDU_SERVICE_SEARCH_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x35; /* DES{ UUID16 0x110b } */
        request[offset++] = 0x03;
        request[offset++] = 0x19;
        request[offset++] = 0x11;
        request[offset++] = 0x0b;
        request[offset++] = 0x00; /* MaximumServiceRecordCount = 12 */
        request[offset++] = 0x0c;
        request[offset++] = 0x00; /* continuation */
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 9, "service-search returns a response");
        check(n >= 9 && response[0] == SDP_PDU_SERVICE_SEARCH_RSP,
              "service-search response PDU id is 0x03 (not the request id)");
        if (n >= 9) {
            uint16_t total = be16(response + 5);
            uint16_t current = be16(response + 7);
            check(total >= 1, "service-search reports at least one Audio Sink");
            check(current == total,
                  "service-search returns all handles in one continuation");
            check(n >= 10 && response[n - 1] == 0x00,
                  "service-search ends with a null continuation state");
        }
    }

    /* --- 4. ServiceAttribute returns response PDU id 0x05 --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x0104;
        size_t len;
        uint32_t handle = 0x00010001; /* Audio Sink record */

        request[0] = SDP_PDU_SERVICE_ATTR_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = (uint8_t)(handle >> 24);
        request[offset++] = (uint8_t)(handle >> 16);
        request[offset++] = (uint8_t)(handle >> 8);
        request[offset++] = (uint8_t)handle;
        request[offset++] = 0xff; /* MaximumAttributeByteCount hi */
        request[offset++] = 0xff; /* lo */
        request[offset++] = 0x35; /* AttributeIDList: DES{ uint32 wildcard } */
        request[offset++] = 0x05;
        request[offset++] = 0x0a;
        request[offset++] = 0x00;
        request[offset++] = 0x00;
        request[offset++] = 0xff;
        request[offset++] = 0xff;
        request[offset++] = 0x00; /* continuation */
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 5, "service-attr returns a response");
        check(n >= 5 && response[0] == SDP_PDU_SERVICE_ATTR_RSP,
              "service-attr response PDU id is 0x05 (not the request id)");
        if (n >= 5) {
            uint16_t plen = be16(response + 3);
            check(5 + (ssize_t)plen == n,
                  "service-attr parameter length matches the PDU body");
            check(n >= 6 && response[n - 1] == 0x00,
                  "service-attr ends with a null continuation state");
        }
    }

    /* --- 5. Malformed pattern (not a DES) yields a syntax error PDU --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x0105;
        size_t len;

        request[0] = SDP_PDU_SERVICE_SEARCH_ATTR_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x19; /* bare UUID16, not a DES */
        request[offset++] = 0x11;
        request[offset++] = 0x0b;
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 7, "malformed pattern returns a response");
        check(n >= 7 && response[0] == SDP_PDU_ERROR_RSP,
              "malformed pattern returns the SDP error PDU");
    }

    le_profile_test_cleanup(&profiles);

    if (failures) {
        printf("sdp wire format: %d FAILURE(s)\n", failures);
        return 1;
    }
    printf("sdp wire format: ok\n");
    return 0;
}
