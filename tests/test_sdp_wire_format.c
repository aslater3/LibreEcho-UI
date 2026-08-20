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
 *   4. The response body carries a 2-byte AttributeListsByteCount that
 *      covers only the AttributeListsList (never the continuation state),
 *      followed by an outer DES wrapping each matching record list and a
 *      trailing one-byte null continuation state.
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

static int contains_bytes(const uint8_t *haystack, size_t haystack_length,
                          const uint8_t *needle, size_t needle_length)
{
    size_t i;

    if (!needle_length || needle_length > haystack_length)
        return 0;
    for (i = 0; i + needle_length <= haystack_length; ++i) {
        if (!memcmp(haystack + i, needle, needle_length))
            return 1;
    }
    return 0;
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
        static const uint8_t service_name[] = {
            0x09, 0x01, 0x00, 0x25, 0x09,
            'L', 'i', 'b', 'r', 'e', 'E', 'c', 'h', 'o'
        };
        static const uint8_t malformed_language_base[] = {
            0x09, 0x00, 0x06, 0x25
        };
        static const uint8_t speaker_features[] = {
            0x09, 0x03, 0x11, 0x09, 0x00, 0x02
        };
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
            /* Payload must be an AttributeListsByteCount (2 bytes) followed
             * by an outer DES and a trailing 1-byte null continuation. */
            check(n >= 6 && response[n - 1] == 0x00,
                  "search-attr ends with a null continuation state");
            check(n >= 8 && (response[7] == 0x35 || response[7] == 0x36),
                  "search-attr AttributeListsList DES starts at offset 7");
            {
                uint16_t count = be16(response + 5);
                check(count == n - 8,
                      "AttributeListsByteCount covers only the list, not "
                      "the continuation state");
            }
            check(contains_bytes(response, (size_t)n, service_name,
                                 sizeof(service_name)),
                  "Audio Sink uses primary-language Service Name attribute 0x0100");
            check(!contains_bytes(response, (size_t)n,
                                  malformed_language_base,
                                  sizeof(malformed_language_base)),
                  "Audio Sink does not encode text as Language Base attribute 0x0006");
            check(contains_bytes(response, (size_t)n, speaker_features,
                                 sizeof(speaker_features)),
                  "Audio Sink advertises the A2DP Speaker feature bit");
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
            check(n >= 8 && be16(response + 5) > 0,
                  "browse-group query reports a non-empty list byte count");
            check(n >= 9 && response[7] == 0x35,
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
            check(n >= 8 && be16(response + 5) > 0,
                  "uuid128 browse-group reports a non-empty list byte count");
            check(n >= 9 && response[7] == 0x35,
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

    /* --- 3b. Apple full-SDP discovery first asks for the SDP Server --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x0108;
        size_t len;

        request[0] = SDP_PDU_SERVICE_SEARCH_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x35; /* DES{ UUID16 0x1000 } */
        request[offset++] = 0x03;
        request[offset++] = 0x19;
        request[offset++] = 0x10;
        request[offset++] = 0x00;
        request[offset++] = 0x00; /* MaximumServiceRecordCount = 12 */
        request[offset++] = 0x0c;
        request[offset++] = 0x00; /* continuation */
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 14, "SDP-server service-search returns a record handle");
        check(n >= 14 && response[0] == SDP_PDU_SERVICE_SEARCH_RSP,
              "SDP-server service-search returns a success PDU");
        if (n >= 14) {
            uint16_t total = be16(response + 5);
            uint16_t current = be16(response + 7);
            uint32_t handle = ((uint32_t)response[9] << 24) |
                              ((uint32_t)response[10] << 16) |
                              ((uint32_t)response[11] << 8) |
                              response[12];

            check(total == 1,
                  "SDP-server service-search reports exactly one record");
            check(current == 1,
                  "SDP-server service-search returns one handle");
            check(handle == 0x00000000,
                  "SDP-server service record uses the reserved handle 0");
            check(response[13] == 0x00,
                  "SDP-server service-search ends with null continuation");
        }
    }

    /* --- 3c. SDP Server attributes use their required wire types/values --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x010b;
        size_t len;
        static const uint8_t expected_attributes[] = {
            0x35, 0x20,             /* AttributeList DES, 32 bytes */
            0x09, 0x00, 0x00,       /* ServiceRecordHandle attribute */
            0x0a, 0x00, 0x00, 0x00, 0x00,
            0x09, 0x00, 0x01,       /* ServiceClassIDList attribute */
            0x35, 0x03, 0x19, 0x10, 0x00,
            0x09, 0x02, 0x00,       /* VersionNumberList attribute */
            0x35, 0x03, 0x09, 0x01, 0x00,
            0x09, 0x02, 0x01,       /* ServiceDatabaseState attribute */
            0x0a, 0x00, 0x00, 0x00, 0x01,
        };

        request[0] = SDP_PDU_SERVICE_ATTR_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x00; /* reserved SDP Server handle */
        request[offset++] = 0x00;
        request[offset++] = 0x00;
        request[offset++] = 0x00;
        request[offset++] = 0xff; /* MaximumAttributeByteCount */
        request[offset++] = 0xff;
        request[offset++] = 0x35; /* AttributeIDList: 0x0000-0xffff */
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
        check(n == 7 + sizeof(expected_attributes) + 1,
              "SDP-server service-attr returns the complete record");
        check(n > 0 && response[0] == SDP_PDU_SERVICE_ATTR_RSP,
              "SDP-server service-attr returns a success PDU");
        check(n == 7 + sizeof(expected_attributes) + 1 &&
                  be16(response + 5) == sizeof(expected_attributes),
              "SDP-server AttributeListsByteCount matches the record");
        check(n == 7 + sizeof(expected_attributes) + 1 &&
                  memcmp(response + 7, expected_attributes,
                         sizeof(expected_attributes)) == 0,
              "SDP-server attributes have the required wire encoding");
        check(n == 7 + sizeof(expected_attributes) + 1 &&
                  response[n - 1] == 0x00,
              "SDP-server service-attr ends with null continuation");
    }

    /* --- 3d. Every serialized service class is directly searchable --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x0109;
        size_t len;

        request[0] = SDP_PDU_SERVICE_SEARCH_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x35; /* DES{ UUID16 AVRCP Target 0x110c } */
        request[offset++] = 0x03;
        request[offset++] = 0x19;
        request[offset++] = 0x11;
        request[offset++] = 0x0c;
        request[offset++] = 0x00;
        request[offset++] = 0x0c;
        request[offset++] = 0x00;
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 14 && response[0] == SDP_PDU_SERVICE_SEARCH_RSP,
              "AVRCP Target service-search returns a record");
        check(n >= 14 && be16(response + 5) == 1 &&
                  be16(response + 7) == 1,
              "AVRCP Target service class is directly searchable");
    }

    /* --- 3e. Protocol UUIDs embedded in records are directly searchable --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x010c;
        size_t len;

        request[0] = SDP_PDU_SERVICE_SEARCH_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x35; /* DES{ L2CAP 0x0100 }, as sent by macOS */
        request[offset++] = 0x03;
        request[offset++] = 0x19;
        request[offset++] = 0x01;
        request[offset++] = 0x00;
        request[offset++] = 0x00;
        request[offset++] = 0x0c;
        request[offset++] = 0x00;
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n >= 18 && response[0] == SDP_PDU_SERVICE_SEARCH_RSP,
              "L2CAP protocol service-search returns matching records");
        check(n >= 18 && be16(response + 5) == 2 &&
                  be16(response + 7) == 2,
              "L2CAP search finds A2DP and AVRCP service records");
    }

    /* --- 3f. Multi-UUID search patterns use SDP AND semantics --- */
    {
        size_t offset = 5;
        uint16_t tid = 0x010a;
        size_t len;

        request[0] = SDP_PDU_SERVICE_SEARCH_REQ;
        request[1] = (uint8_t)(tid >> 8);
        request[2] = (uint8_t)tid;
        request[offset++] = 0x35; /* DES{ Audio Sink, AVRCP Target } */
        request[offset++] = 0x06;
        request[offset++] = 0x19;
        request[offset++] = 0x11;
        request[offset++] = 0x0b;
        request[offset++] = 0x19;
        request[offset++] = 0x11;
        request[offset++] = 0x0c;
        request[offset++] = 0x00;
        request[offset++] = 0x0c;
        request[offset++] = 0x00;
        len = offset;
        request[3] = (uint8_t)((len - 5) >> 8);
        request[4] = (uint8_t)(len - 5);

        n = le_profile_test_sdp_exchange(&profiles, request, len, response,
                                         sizeof(response));
        check(n == 10 && response[0] == SDP_PDU_SERVICE_SEARCH_RSP,
              "multi-UUID no-match returns an empty success response");
        check(n == 10 && be16(response + 5) == 0 &&
                  be16(response + 7) == 0,
              "multi-UUID service-search requires every UUID to match");
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
            check(n >= 9 && be16(response + 5) == n - 8,
                  "service-attr AttributeListsByteCount covers only the list");
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
