/*
 * Contract coverage for the bounded update check record reader.
 *
 * The device records the check it finished -- status, source, channel, latest
 * version, timestamps and the candidate identity that check resolved (the
 * immutable GitHub release tag and the OTA package SHA-256) -- in one
 * check-status record, and this reader is the only path by which those fields
 * reach the API. The cases here are the ones that decide whether a client can
 * trust what it shows: a complete record is returned exactly, and every other
 * shape (absent key, a record written before these keys existed, an empty value
 * left behind by a failed check or a channel change, a malformed value, an
 * oversized value, a value that does not fit the caller's buffer, a missing
 * record) reports absence instead of a truncated or invented identity.
 *
 * The record is committed by the update helper with an atomic rename, and the
 * check and channel actions run in child workers while GETs stay serviceable,
 * so a reader that opened the record once per key could describe two different
 * checks at once: an old status or version beside the next check's identity.
 * The torn-read regression at the end of this file is deterministic: fopen() is
 * wrapped for this test binary (see the Makefile's -Wl,--wrap=fopen), so the
 * fixture writer can commit the next check at the one boundary a reader cannot
 * avoid -- immediately after an open of the record has resolved. The removed
 * per-key design is then shown, through that same harness, to publish one
 * check's status with the next check's identity, while the snapshot reader
 * always reports fields of a single generation.
 */
#include "update_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_PATH "build/test-update-identity.status"
#define RECORD_LINE_MAX 512

static const char *const TAG =
    "radar-puffin-build-0123456-0123456789abcdef-fedcba9876543210";
static const char *const NIGHTLY_TAG =
    "radar-puffin-nightly-abcdef0-0123456789abcdef-fedcba9876543210";
static const char *const SHA =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
/* A second check of the same shape: what the next successful check resolves. */
static const char *const NEXT_TAG =
    "radar-puffin-build-9876543-fedcba9876543210-0123456789abcdef";
static const char *const NEXT_SHA =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

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

/* ---- the check writer, and the boundary a torn read needs ---------------- */

/* The real fopen, reached through the linker's --wrap indirection, so fixture
   writes are never mistaken for an open by the reader under test. */
extern FILE *__real_fopen(const char *path, const char *mode);

static const char *writer_status;
static const char *writer_version;
static const char *writer_tag;
static const char *writer_sha;
static int writer_armed;
static int record_opens;
static int generations_committed;
static char committed_record[512];

static void write_fixture(const char *path, const char *body)
{
    FILE *f = __real_fopen(path, "w");
    if (!f) {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        exit(2);
    }
    fputs(body, f);
    fclose(f);
}

/* One check as the update helper writes it. */
static void record_body(char *out, size_t size, const char *status,
                        const char *version, const char *tag, const char *sha)
{
    snprintf(out, size,
             "schema=1\nsource=github-releases\nchannel=dev\n"
             "status=%s\nsource_reachable=true\nlatest_version=%s\n"
             "last_check_epoch=1789000000\nlast_success_epoch=1789000000\n"
             "resolved_release_tag=%s\nota_sha256=%s\n",
             status, version, tag, sha);
}

static void write_record(const char *body)
{
    write_fixture(RECORD_PATH, body);
}

/* Commit a check exactly as the update helper does: write the new record beside
   the old one and rename it over the path. */
static void commit_generation(const char *status, const char *version,
                              const char *tag, const char *sha)
{
    record_body(committed_record, sizeof(committed_record), status, version,
                tag, sha);
    write_fixture(RECORD_PATH ".next", committed_record);
    if (rename(RECORD_PATH ".next", RECORD_PATH)) {
        fprintf(stderr, "FAIL: cannot commit %s\n", RECORD_PATH);
        exit(2);
    }
    generations_committed++;
}

static void arm_writer(const char *status, const char *version, const char *tag,
                       const char *sha)
{
    writer_status = status;
    writer_version = version;
    writer_tag = tag;
    writer_sha = sha;
    writer_armed = 1;
}

/* Every fopen() in this binary resolves here. An open of the record is counted,
   and an armed check is committed as soon as that open has resolved -- the
   interleaving a worker cannot avoid, because the descriptor is already bound
   to the generation it opened while the path names the next one. */
FILE *__wrap_fopen(const char *path, const char *mode)
{
    FILE *f;

    if (!path || strcmp(path, RECORD_PATH))
        return __real_fopen(path, mode);
    record_opens++;
    f = __real_fopen(path, mode);
    if (f && writer_armed) {
        writer_armed = 0;
        commit_generation(writer_status, writer_version, writer_tag, writer_sha);
    }
    return f;
}

/* ---- the reader under test ---------------------------------------------- */

struct record_read {
    char status[64];
    char version[96];
    char tag[LE_UPDATE_TAG_SIZE];
    char sha[LE_UPDATE_SHA_SIZE];
};

/* Reads the fields the API publishes together out of one call, starting from
   the values the API initialises them with: the identity buffers are poisoned,
   because an unresolved identity must be an empty string, and the opaque fields
   start on their documented defaults, which an absent key must leave standing
   so an older record keeps the envelope's documented shape. */
static int read_record(struct record_read *out)
{
    struct le_update_field fields[]={
        {"status",out->status,sizeof(out->status)},
        {"latest_version",out->version,sizeof(out->version)},
        {LE_UPDATE_TAG_KEY,out->tag,sizeof(out->tag)},
        {LE_UPDATE_SHA_KEY,out->sha,sizeof(out->sha)}
    };

    strcpy(out->status, "not-checked");
    out->version[0] = '\0';
    memset(out->tag, 'Z', sizeof(out->tag));
    memset(out->sha, 'Z', sizeof(out->sha));
    return update_record_read(RECORD_PATH, fields,
                              sizeof(fields)/sizeof(fields[0]));
}

static void check_read(int resolved, const char *status, const char *version,
                       const char *tag, const char *sha, const char *what)
{
    struct record_read out;
    int rc = read_record(&out);
    check(rc == resolved && !strcmp(out.status, status) &&
          !strcmp(out.version, version) && !strcmp(out.tag, tag) &&
          !strcmp(out.sha, sha), what);
}

/* The reader this PR replaced read one key per open. It is reproduced here so
   the regression stays pinned against the design that allowed it. */
static int legacy_read_key(const char *path, const char *key, char *out,
                           size_t size)
{
    FILE *f;
    char line[RECORD_LINE_MAX];
    size_t key_len = strlen(key);
    int found = 0;

    out[0] = '\0';
    if (!(f = fopen(path, "r")))
        return 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len;
        if (strncmp(line, key, key_len) || line[key_len] != '=')
            continue;
        len = strcspn(line + key_len + 1, "\r\n");
        found = 1;
        if (len < size && len + key_len + 1 < sizeof(line)) {
            memcpy(out, line + key_len + 1, len);
            out[len] = '\0';
        }
        break;
    }
    fclose(f);
    return found && out[0] != '\0';
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
    struct record_read out;
    size_t i;
    int rc;

    /* A completed dev check: every field is published exactly. */
    record_body(record, sizeof(record), "update-available", "0.14.0", TAG, SHA);
    write_record(record);
    check_read(4, "update-available", "0.14.0", TAG, SHA,
               "recorded check returned exactly");

    /* The record's key order is the writer's business, not the reader's. */
    snprintf(record, sizeof(record),
             "schema=1\nlatest_version=0.14.0\nstatus=update-available\n"
             "ota_sha256=%s\nresolved_release_tag=%s\n", SHA, TAG);
    write_record(record);
    check_read(4, "update-available", "0.14.0", TAG, SHA,
               "check returned in either key order");

    /* CRLF records do not leave the terminator in a value. */
    snprintf(record, sizeof(record),
             "status=update-available\r\nlatest_version=0.14.0\r\n"
             "resolved_release_tag=%s\r\nota_sha256=%s\r\n", TAG, SHA);
    write_record(record);
    check_read(4, "update-available", "0.14.0", TAG, SHA,
               "CRLF record returns values without terminators");

    /* A record written before the identity keys existed (older device, or a
       stable candidate): the opaque fields are read, the identity is empty. */
    write_record("schema=1\nsource=github-releases\nchannel=dev\n"
                 "status=up-to-date\nsource_reachable=true\n"
                 "latest_version=0.13.9\n");
    check_read(2, "up-to-date", "0.13.9", "", "",
               "absent identity keys are empty");

    /* A record that carries only the identity: the fields it does not name stay
       on the defaults the caller initialised, not on an empty string. */
    snprintf(record, sizeof(record),
             "resolved_release_tag=%s\nota_sha256=%s\n", TAG, SHA);
    write_record(record);
    rc = read_record(&out);
    check(rc == 2 && !strcmp(out.status, "not-checked") && !out.version[0] &&
          !strcmp(out.tag, TAG) && !strcmp(out.sha, SHA),
          "absent opaque fields keep the caller's default while the identity resolves");

    /* Channel change and failed checks write the record with the keys cleared,
       so nothing can be inherited from an earlier check. */
    write_record("schema=1\nsource=github-releases\nchannel=stable\n"
                 "status=not-checked\nsource_reachable=unknown\n"
                 "latest_version=\nresolved_release_tag=\nota_sha256=\n");
    check_read(2, "not-checked", "", "", "",
               "cleared identity after a channel change is empty");
    write_record("schema=1\nstatus=error\nerror=download_transport\n"
                 "latest_version=\nresolved_release_tag=\nota_sha256=\n");
    check_read(2, "error", "", "", "",
               "cleared identity after a failed check is empty");

    /* A stable candidate has no immutable identity. */
    write_record("status=up-to-date\nlatest_version=0.14.0\n"
                 "resolved_release_tag=\nota_sha256=\n");
    check_read(2, "up-to-date", "0.14.0", "", "",
               "stable candidate exposes no identity");

    /* Each value is validated on its own: a malformed tag cannot withdraw a
       recorded digest, and neither is repaired into shape. */
    snprintf(record, sizeof(record),
             "status=update-available\nlatest_version=0.14.0\n"
             "resolved_release_tag=RADAR-PUFFIN-BUILD-0123456-"
             "0123456789abcdef-fedcba9876543210\nota_sha256=%s\n", SHA);
    write_record(record);
    check_read(3, "update-available", "0.14.0", "", SHA,
               "malformed tag reported absent, digest unaffected");

    {
        char short_sha[80];
        memset(short_sha, 'a', 63);
        short_sha[63] = '\0';
        snprintf(record, sizeof(record),
                 "status=update-available\nlatest_version=0.14.0\n"
                 "resolved_release_tag=0.14.0\nota_sha256=%s\n", short_sha);
        write_record(record);
    }
    check_read(2, "update-available", "0.14.0", "", "",
               "version string and short digest are both absent");

    write_record("resolved_release_tag= radar-puffin-build-0123456-"
                 "0123456789abcdef-fedcba9876543210 \nota_sha256="
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeF\n");
    check_read(0, "not-checked", "", "", "",
               "padded tag and mixed-case digest are absent");

    /* A key's name appearing inside another value never fabricates a field. */
    snprintf(record, sizeof(record),
             "error=resolved_release_tag=%s\nota_sha256=\n", TAG);
    write_record(record);
    check_read(0, "not-checked", "", "", "",
               "a key name inside another value does not fabricate an identity");

    /* The record writer never repeats a key; if one is repeated the first
       occurrence wins, so a later line cannot displace a value that already
       parsed. */
    snprintf(record, sizeof(record),
             "resolved_release_tag=%s\nresolved_release_tag=%s\nota_sha256=%s\n",
             TAG, NEXT_TAG, SHA);
    write_record(record);
    check_read(2, "not-checked", "", TAG, SHA,
               "the first occurrence of a repeated key wins");

    /* Field bounds: an oversized identity is absent, not truncated into shape,
       while an opaque field keeps the bounded copy the envelope reports. */
    record[0] = '\0';
    strcpy(record, "latest_version=");
    for (i = strlen(record); i < 4000; i++)
        record[i] = 'b';
    record[i] = '\0';
    strcat(record, "\nresolved_release_tag=");
    for (i = strlen(record); i < 8000; i++)
        record[i] = 'a';
    record[i] = '\0';
    strcat(record, "\nota_sha256=");
    for (i = strlen(record); i < 8190; i++)
        record[i] = 'c';
    record[i] = '\0';
    strcat(record, "\n");
    write_record(record);
    rc = read_record(&out);
    check(rc == 1 && out.version[0] == 'b' &&
          strlen(out.version) == sizeof(out.version) - 1 &&
          out.tag[0] == '\0' && out.sha[0] == '\0',
          "an oversized identity is empty while an opaque field is copied bounded");

    /* A long unrelated value earlier in the record must not disturb parsing. */
    record[0] = '\0';
    strcat(record, "error_detail=");
    for (i = strlen(record); i < 2000; i++)
        record[i] = 'x';
    record[i] = '\0';
    strcat(record, "\nstatus=update-available\nlatest_version=0.14.0\n"
                   "resolved_release_tag=");
    strcat(record, TAG);
    strcat(record, "\nota_sha256=");
    strcat(record, SHA);
    strcat(record, "\n");
    write_record(record);
    check_read(4, "update-available", "0.14.0", TAG, SHA,
               "check read after a long unrelated value");

    /* The final keys are readable without a trailing newline. */
    snprintf(record, sizeof(record),
             "status=update-available\nresolved_release_tag=%s\nota_sha256=%s",
             TAG, SHA);
    write_record(record);
    check_read(3, "update-available", "", TAG, SHA,
               "final keys without a trailing newline are read");

    /* A value that does not fit the caller's buffer is empty rather than a
       buffer-sized prefix; the other identity is still resolved, because the
       fields are resolved independently. */
    record_body(record, sizeof(record), "update-available", "0.14.0", TAG, SHA);
    write_record(record);
    {
        char small_tag[8];
        char full_sha[LE_UPDATE_SHA_SIZE];
        struct le_update_field fields[]={
            {LE_UPDATE_TAG_KEY,small_tag,sizeof(small_tag)},
            {LE_UPDATE_SHA_KEY,full_sha,sizeof(full_sha)}
        };
        memset(small_tag, 'Z', sizeof(small_tag));
        memset(full_sha, 'Z', sizeof(full_sha));
        rc = update_record_read(RECORD_PATH, fields,
                                sizeof(fields)/sizeof(fields[0]));
        check(rc == 1 && small_tag[0] == '\0' && !strcmp(full_sha, SHA),
              "an identity larger than its buffer is empty, the other still resolves");
    }

    /* Rejected calls and an unusable table resolve nothing. */
    {
        struct le_update_field fields[]={
            {LE_UPDATE_TAG_KEY,out.tag,sizeof(out.tag)},
            {LE_UPDATE_SHA_KEY,out.sha,sizeof(out.sha)}
        };
        check(update_record_read(NULL, fields, 2) == 0, "null path is rejected");
        check(update_record_read(RECORD_PATH, NULL, 2) == 0,
              "a missing field table is rejected");
        check(update_record_read(RECORD_PATH, fields, 0) == 0,
              "an empty field table is rejected");
    }
    {
        struct le_update_field fields[]={
            {LE_UPDATE_TAG_KEY,NULL,0},
            {LE_UPDATE_SHA_KEY,out.sha,sizeof(out.sha)}
        };
        memset(out.sha, 'Z', sizeof(out.sha));
        rc = update_record_read(RECORD_PATH, fields,
                                sizeof(fields)/sizeof(fields[0]));
        check(rc == 1 && !strcmp(out.sha, SHA),
              "a table entry without a buffer is skipped, the usable one resolves");
    }
    {
        struct le_update_field fields[]={
            {NULL,out.sha,sizeof(out.sha)}
        };
        memset(out.sha, 'Z', sizeof(out.sha));
        rc = update_record_read(RECORD_PATH, fields,
                                sizeof(fields)/sizeof(fields[0]));
        check(rc == 0 && out.sha[0] == 'Z',
              "a table entry without a key writes nothing");
    }

    /* An unreadable record reports absence rather than an error state, and the
       caller's initialised defaults stand. */
    remove(RECORD_PATH);
    rc = read_record(&out);
    check(rc == 0 && !strcmp(out.status, "not-checked") && !out.version[0] &&
          !out.tag[0] && !out.sha[0],
          "missing record exposes no identity and keeps the defaults");
}

/* Two checks of the same shape, for the interrupted-read loop. */
static void generated_identity(unsigned generation, char *tag, size_t tag_size,
                              char *sha, size_t sha_size)
{
    unsigned long long a = 0x0123456789abcdefULL ^
        (unsigned long long)generation * 0x100000001b3ULL;
    unsigned long long b = 0xfedcba9876543210ULL ^
        (unsigned long long)generation * 0x9e3779b97f4a7c15ULL;

    snprintf(tag, tag_size, "radar-puffin-build-%07x-%016llx-%016llx",
             (unsigned)generation * 0x5bd1U & 0xfffffffU, a, b);
    snprintf(sha, sha_size, "%016llx%016llx%016llx%016llx", a, b, a ^ b, a + b);
}

static void torn_read_cases(void)
{
    char record[8192];
    char status[64];
    char version[96];
    char tag[LE_UPDATE_TAG_SIZE];
    char sha[LE_UPDATE_SHA_SIZE];
    char prev_version[96];
    char prev_tag[LE_UPDATE_TAG_SIZE];
    char prev_sha[LE_UPDATE_SHA_SIZE];
    char next_version[96];
    char next_tag[LE_UPDATE_TAG_SIZE];
    char next_sha[LE_UPDATE_SHA_SIZE];
    char what[96];
    struct record_read out;
    unsigned i;
    int rc;

    check(strcmp(TAG, NEXT_TAG) && strcmp(SHA, NEXT_SHA),
          "the fixture checks are distinguishable");

    /* The interleaving: a check worker commits the next check as soon as the
       reader has opened the record. The reader opened it once, so every field
       still comes from the generation it opened. */
    record_body(record, sizeof(record), "update-available", "0.14.0", TAG, SHA);
    write_record(record);
    record_opens = 0;
    generations_committed = 0;
    writer_armed = 0;
    arm_writer("update-available", "0.15.0", NEXT_TAG, NEXT_SHA);
    rc = read_record(&out);
    check(rc == 4, "a torn-read interleaving still reports every field");
    check(record_opens == 1,
          "the whole record is read with exactly one open");
    check(!strcmp(out.status, "update-available") && !strcmp(out.version, "0.14.0") &&
          !strcmp(out.tag, TAG) && !strcmp(out.sha, SHA),
          "every field comes from the generation the reader opened");
    check(generations_committed == 1 && strstr(committed_record, NEXT_TAG) &&
          strstr(committed_record, NEXT_SHA),
          "the check writer committed the next check during the read");
    check(strcmp(out.tag, NEXT_TAG) && strcmp(out.sha, NEXT_SHA) &&
          strcmp(out.version, "0.15.0"),
          "no field comes from the check that landed mid-read");
    check_read(4, "update-available", "0.15.0", NEXT_TAG, NEXT_SHA,
               "the next read sees the committed check whole");

    /* The pairing the reviewer named: a check that is up to date is on disk
       with no candidate identity, and the next check -- which resolved one --
       lands while the response is assembled. */
    record_body(record, sizeof(record), "up-to-date", "0.13.9", "", "");
    write_record(record);
    record_opens = 0;
    writer_armed = 0;
    arm_writer("update-available", "0.14.0", NEXT_TAG, NEXT_SHA);
    rc = read_record(&out);
    check(rc == 2 && record_opens == 1 && !strcmp(out.status, "up-to-date") &&
          !strcmp(out.version, "0.13.9") && !out.tag[0] && !out.sha[0],
          "an interrupted read reports one check, identity included");
    check(strcmp(out.status, "up-to-date") || !out.tag[0],
          "an up-to-date status is never published beside another check's identity");

    /* The same harness against the design this reader replaced: it read each
       field over its own open, so the commit that lands between its opens makes
       it publish the first check's status and version beside the second check's
       identity -- the mismatch that can tell a user an update is available while
       the status says the device is up to date. */
    record_body(record, sizeof(record), "up-to-date", "0.13.9", "", "");
    write_record(record);
    record_opens = 0;
    writer_armed = 0;
    arm_writer("update-available", "0.14.0", NEXT_TAG, NEXT_SHA);
    memset(status, 'Z', sizeof(status));
    memset(version, 'Z', sizeof(version));
    memset(tag, 'Z', sizeof(tag));
    memset(sha, 'Z', sizeof(sha));
    rc = legacy_read_key(RECORD_PATH, "status", status, sizeof(status));
    rc += legacy_read_key(RECORD_PATH, "latest_version", version, sizeof(version));
    rc += legacy_read_key(RECORD_PATH, LE_UPDATE_TAG_KEY, tag, sizeof(tag));
    rc += legacy_read_key(RECORD_PATH, LE_UPDATE_SHA_KEY, sha, sizeof(sha));
    check(rc == 4 && record_opens == 4,
          "the removed reader resolved each field over its own open");
    check(!strcmp(status, "up-to-date") && !strcmp(version, "0.14.0") &&
          !strcmp(tag, NEXT_TAG) && !strcmp(sha, NEXT_SHA),
          "the removed per-field reader published one check's status beside the next check's identity");

    /* The identity pairing on its own, for the digest that belongs with the
       tag: two opens, one per key. */
    record_body(record, sizeof(record), "update-available", "0.14.0", TAG, SHA);
    write_record(record);
    record_opens = 0;
    writer_armed = 0;
    arm_writer("update-available", "0.15.0", NEXT_TAG, NEXT_SHA);
    memset(tag, 'Z', sizeof(tag));
    memset(sha, 'Z', sizeof(sha));
    rc = legacy_read_key(RECORD_PATH, LE_UPDATE_TAG_KEY, tag, sizeof(tag));
    rc += legacy_read_key(RECORD_PATH, LE_UPDATE_SHA_KEY, sha, sizeof(sha));
    check(rc == 2 && record_opens == 2,
          "the removed reader resolved both identity keys over two opens");
    check(!strcmp(tag, TAG) && !strcmp(sha, NEXT_SHA),
          "the removed two-open reader paired one check's tag with the next check's digest");

    /* Every generation boundary the reader can be interrupted at: it has one,
       the open. Commit the next check at that point on each of eight successive
       checks and every published field still belongs to one check. */
    generated_identity(0, prev_tag, sizeof(prev_tag), prev_sha, sizeof(prev_sha));
    strcpy(prev_version, "0.14.0");
    record_body(record, sizeof(record), "update-available", prev_version,
                prev_tag, prev_sha);
    write_record(record);
    for (i = 1; i <= 8; i++) {
        generated_identity(i, next_tag, sizeof(next_tag), next_sha,
                           sizeof(next_sha));
        snprintf(next_version, sizeof(next_version), "0.14.%u", i);
        record_opens = 0;
        arm_writer("update-available", next_version, next_tag, next_sha);
        rc = read_record(&out);
        snprintf(what, sizeof(what),
                 "generation %u read whole while the next was committed", i);
        check(rc == 4 && record_opens == 1 && !strcmp(out.version, prev_version) &&
              !strcmp(out.tag, prev_tag) && !strcmp(out.sha, prev_sha), what);
        writer_armed = 0;
        memcpy(prev_tag, next_tag, sizeof(prev_tag));
        memcpy(prev_sha, next_sha, sizeof(prev_sha));
        strcpy(prev_version, next_version);
    }
}

int main(void)
{
    validator_cases();
    record_cases();
    torn_read_cases();
    remove(RECORD_PATH);
    if (failures) {
        fprintf(stderr, "update identity reader: %d/%d checks failed\n",
                failures, checks);
        return 1;
    }
    printf("update identity reader: ok (%d checks)\n", checks);
    return 0;
}
