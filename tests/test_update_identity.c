/*
 * Contract coverage for the bounded update identity reader.
 *
 * The device records the candidate a completed check resolved -- the immutable
 * GitHub release tag and the OTA package SHA-256 -- in the update check record.
 * This reader is the only path by which those two values reach the API, so the
 * cases here are the ones that decide whether a client can trust what it shows:
 * a valid identity is returned exactly, and every other shape (absent key, a
 * record written before these keys existed, an empty value left behind by a
 * failed check or a channel change, a malformed value, an oversized value, a
 * value that does not fit the caller's buffer, a missing record) reports
 * absence instead of a truncated or invented identity.
 */
#include "update_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_PATH "build/test-update-identity.status"

static const char *const TAG =
    "radar-puffin-build-0123456-0123456789abcdef-fedcba9876543210";
static const char *const NIGHTLY_TAG =
    "radar-puffin-nightly-abcdef0-0123456789abcdef-fedcba9876543210";
static const char *const SHA =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static int checks;
static int failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void write_record(const char *body)
{
    FILE *f = fopen(RECORD_PATH, "w");
    if (!f) {
        fprintf(stderr, "FAIL: cannot write %s\n", RECORD_PATH);
        exit(2);
    }
    fputs(body, f);
    fclose(f);
}

/* Reads one key and reports both the result and the produced buffer. */
static int read_key(const char *key, char *out, size_t size)
{
    memset(out, 'Z', size);
    return update_identity_value(RECORD_PATH, key, out, size);
}

static void check_absent(const char *key, const char *what)
{
    char out[LE_UPDATE_TAG_SIZE];
    int rc = read_key(key, out, sizeof(out));
    check(rc == 0 && out[0] == '\0', what);
}

static void check_value(const char *key, const char *expected, const char *what)
{
    char out[LE_UPDATE_TAG_SIZE];
    int rc = read_key(key, out, sizeof(out));
    check(rc == 1 && !strcmp(out, expected), what);
}

static void validator_cases(void)
{
    check(update_identity_tag_valid(TAG) == 1, "build tag accepted");
    check(update_identity_tag_valid(NIGHTLY_TAG) == 1, "nightly tag accepted");
    check(update_identity_tag_valid(NULL) == 0, "null tag rejected");
    check(update_identity_tag_valid("") == 0, "empty tag rejected");
    check(update_identity_tag_valid("radar-puffin-build-") == 0,
          "prefix-only tag rejected");
    check(update_identity_tag_valid("radar-puffin-v0.14.0") == 0,
          "product version tag rejected");
    check(update_identity_tag_valid("radar-puffin-fc-0123456-0123456789abcdef-"
                                    "fedcba9876543210") == 0,
          "unknown tag class rejected");
    check(update_identity_tag_valid("Radar-Puffin-build-0123456-"
                                    "0123456789abcdef-fedcba9876543210") == 0,
          "uppercase prefix rejected");
    check(update_identity_tag_valid("radar-puffin-build-0123456-0123456789ABCDEF-"
                                    "fedcba9876543210") == 0,
          "uppercase hex rejected");
    check(update_identity_tag_valid("radar-puffin-build-012345-0123456789abcdef-"
                                    "fedcba9876543210") == 0,
          "short commit group rejected");
    check(update_identity_tag_valid("radar-puffin-build-01234567-0123456789abcdef-"
                                    "fedcba9876543210") == 0,
          "long commit group rejected");
    check(update_identity_tag_valid("radar-puffin-build-0123456-0123456789abcdef-"
                                    "fedcba98765g3210") == 0,
          "non-hex digest group rejected");
    check(update_identity_tag_valid("radar-puffin-build-0123456-0123456789abcdef-"
                                    "fedcba9876543210x") == 0,
          "trailing junk rejected");
    check(update_identity_tag_valid(" radar-puffin-build-0123456-"
                                    "0123456789abcdef-fedcba9876543210") == 0,
          "leading whitespace rejected");

    check(update_identity_sha256_valid(SHA) == 1, "digest accepted");
    check(update_identity_sha256_valid(NULL) == 0, "null digest rejected");
    check(update_identity_sha256_valid("") == 0, "empty digest rejected");
    {
        char short_sha[80], long_sha[96], upper_sha[80], nonhex_sha[80];
        memset(short_sha, 'a', 63);
        short_sha[63] = '\0';
        memcpy(long_sha, SHA, 64);
        long_sha[64] = '0';
        long_sha[65] = '\0';
        memcpy(upper_sha, SHA, 64);
        upper_sha[64] = '\0';
        upper_sha[0] = 'A';
        memcpy(nonhex_sha, SHA, 64);
        nonhex_sha[64] = '\0';
        nonhex_sha[63] = 'g';
        check(update_identity_sha256_valid(short_sha) == 0,
              "63-character digest rejected");
        check(update_identity_sha256_valid(long_sha) == 0,
              "65-character digest rejected");
        check(update_identity_sha256_valid(upper_sha) == 0,
              "uppercase digest rejected");
        check(update_identity_sha256_valid(nonhex_sha) == 0,
              "non-hex digest rejected");
    }
}

static void record_cases(void)
{
    char record[8192];
    char small[8];
    char out[LE_UPDATE_TAG_SIZE];
    size_t i;
    int rc;

    /* A completed dev check: both values are published exactly. */
    snprintf(record, sizeof(record),
             "schema=1\nsource=github-releases\nchannel=dev\n"
             "status=update-available\nsource_reachable=true\n"
             "latest_version=0.14.0\nlast_check_epoch=1789000000\n"
             "last_success_epoch=1789000000\n"
             "resolved_release_tag=%s\nota_sha256=%s\n", TAG, SHA);
    write_record(record);
    check_value(LE_UPDATE_TAG_KEY, TAG, "recorded tag returned");
    check_value(LE_UPDATE_SHA_KEY, SHA, "recorded digest returned");
    check_value("latest_version", "0.14.0", "unrelated key still readable");

    /* A record written before these keys existed (older device). */
    write_record("schema=1\nsource=github-releases\nchannel=dev\n"
                 "status=update-available\nsource_reachable=true\n"
                 "latest_version=0.14.0\n");
    check_absent(LE_UPDATE_TAG_KEY, "absent tag key is empty");
    check_absent(LE_UPDATE_SHA_KEY, "absent digest key is empty");

    /* Channel change and failed checks write the record with the keys cleared,
       so nothing can be inherited from an earlier check. */
    write_record("schema=1\nsource=github-releases\nchannel=stable\n"
                 "status=not-checked\nsource_reachable=unknown\n"
                 "latest_version=\nresolved_release_tag=\nota_sha256=\n");
    check_absent(LE_UPDATE_TAG_KEY, "cleared tag after channel change is empty");
    check_absent(LE_UPDATE_SHA_KEY, "cleared digest after channel change is empty");
    write_record("schema=1\nstatus=error\nerror=download_transport\n"
                 "resolved_release_tag=\nota_sha256=\n");
    check_absent(LE_UPDATE_TAG_KEY, "cleared tag after a failed check is empty");
    check_absent(LE_UPDATE_SHA_KEY, "cleared digest after a failed check is empty");

    /* A stable candidate has no immutable identity. */
    write_record("status=up-to-date\nlatest_version=0.14.0\n"
                 "resolved_release_tag=\nota_sha256=\n");
    check_absent(LE_UPDATE_TAG_KEY, "stable candidate exposes no tag");
    check_absent(LE_UPDATE_SHA_KEY, "stable candidate exposes no digest");

    /* Malformed values are reported as absent, never as an identity. */
    write_record("resolved_release_tag=RADAR-PUFFIN-BUILD-0123456-"
                 "0123456789abcdef-fedcba9876543210\nota_sha256=\n");
    check_absent(LE_UPDATE_TAG_KEY, "malformed recorded tag rejected");
    check_absent(LE_UPDATE_SHA_KEY, "empty recorded digest rejected");
    {
        char short_sha[80];
        memset(short_sha, 'a', 63);
        short_sha[63] = '\0';
        snprintf(record, sizeof(record),
                 "status=update-available\nresolved_release_tag=0.14.0\n"
                 "ota_sha256=%s\n", short_sha);
        write_record(record);
    }
    check_absent(LE_UPDATE_TAG_KEY, "version string is not a release tag");
    check_absent(LE_UPDATE_SHA_KEY, "short recorded digest rejected");
    write_record("resolved_release_tag= radar-puffin-build-0123456-"
                 "0123456789abcdef-fedcba9876543210 \nota_sha256=0123456789abcdef"
                 "0123456789abcdef0123456789abcdef0123456789abcdeF\n");
    check_absent(LE_UPDATE_TAG_KEY, "padded recorded tag rejected");
    check_absent(LE_UPDATE_SHA_KEY, "mixed-case recorded digest rejected");

    /* Field bounds: oversized values are absent, not truncated into shape. */
    record[0] = '\0';
    strcpy(record, "resolved_release_tag=");
    for (i = strlen(record); i < 4000; i++)
        record[i] = 'a';
    record[i] = '\0';
    strcat(record, "\nota_sha256=");
    for (i = strlen(record); i < 8000; i++)
        record[i] = 'b';
    record[i] = '\0';
    strcat(record, "\n");
    write_record(record);
    check_absent(LE_UPDATE_TAG_KEY, "oversized recorded tag is absent");
    check_absent(LE_UPDATE_SHA_KEY, "oversized recorded digest is absent");

    /* A value that does not fit the caller's buffer stays absent rather than
       being cut down to a buffer-sized prefix. */
    write_record("resolved_release_tag=");
    snprintf(out, sizeof(out), "%s\n", TAG);
    {
        FILE *f = fopen(RECORD_PATH, "a");
        if (!f) {
            fprintf(stderr, "FAIL: cannot append to %s\n", RECORD_PATH);
            exit(2);
        }
        fputs(out, f);
        fclose(f);
    }
    memset(small, 'Z', sizeof(small));
    rc = update_identity_value(RECORD_PATH, LE_UPDATE_TAG_KEY, small, sizeof(small));
    check(rc == 0 && small[0] == '\0', "value larger than the buffer is absent");

    /* A long unrelated value earlier in the record must not disturb parsing. */
    record[0] = '\0';
    strcat(record, "error_detail=");
    for (i = strlen(record); i < 2000; i++)
        record[i] = 'x';
    record[i] = '\0';
    strcat(record, "\nresolved_release_tag=");
    strcat(record, TAG);
    strcat(record, "\nota_sha256=");
    strcat(record, SHA);
    strcat(record, "\n");
    write_record(record);
    check_value(LE_UPDATE_TAG_KEY, TAG, "tag read after a long unrelated value");
    check_value(LE_UPDATE_SHA_KEY, SHA, "digest read after a long unrelated value");

    /* The final key is readable without a trailing newline. */
    write_record("status=update-available\nota_sha256=");
    {
        FILE *f = fopen(RECORD_PATH, "a");
        if (!f) {
            fprintf(stderr, "FAIL: cannot append to %s\n", RECORD_PATH);
            exit(2);
        }
        fputs(SHA, f);
        fclose(f);
    }
    check_value(LE_UPDATE_SHA_KEY, SHA, "final key without newline is read");

    /* An unreadable record reports absence rather than an error state. */
    remove(RECORD_PATH);
    check_absent(LE_UPDATE_TAG_KEY, "missing record exposes no tag");
    check_absent(LE_UPDATE_SHA_KEY, "missing record exposes no digest");

    check(update_identity_value(NULL, LE_UPDATE_TAG_KEY, out, sizeof(out)) == 0,
          "null path is rejected");
    out[0] = 'Z';
    check(update_identity_value(RECORD_PATH, LE_UPDATE_TAG_KEY, out, 0) == 0,
          "zero-sized buffer is rejected");
    check(update_identity_value(RECORD_PATH, "", out, sizeof(out)) == 0,
          "empty key is rejected");
}

int main(void)
{
    validator_cases();
    record_cases();
    remove(RECORD_PATH);
    if (failures) {
        fprintf(stderr, "update identity reader: %d/%d checks failed\n",
                failures, checks);
        return 1;
    }
    printf("update identity reader: ok (%d checks)\n", checks);
    return 0;
}
