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
 *
 * The record is committed by the update helper with an atomic rename, so a
 * reader that opened it once per key could pair the tag of one candidate with
 * the digest of the next and publish an identity that belongs to no artifact.
 * The torn-read regression at the end of this file is deterministic: fopen() is
 * wrapped for this test binary (see the Makefile's -Wl,--wrap=fopen), so the
 * fixture writer can commit the next check at the one boundary a reader cannot
 * avoid -- immediately after an open of the record has resolved -- and the
 * removed two-pass design is then shown, through that same harness, to publish
 * a mixed pair while the paired reader, which opens once, always publishes both
 * fields of a single generation.
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
/* A second candidate of the same shape: the generation a later check resolves. */
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

static void generation_body(char *out, size_t size, const char *tag,
                            const char *sha)
{
    snprintf(out, size,
             "schema=1\nsource=github-releases\nchannel=dev\n"
             "status=update-available\nsource_reachable=true\n"
             "latest_version=0.14.0\nlast_check_epoch=1789000000\n"
             "resolved_release_tag=%s\nota_sha256=%s\n", tag, sha);
}

static void write_record(const char *body)
{
    write_fixture(RECORD_PATH, body);
}

/* Commit a candidate exactly as the update helper does: write the new record
   beside the old one and rename it over the path. */
static void commit_generation(const char *tag, const char *sha)
{
    generation_body(committed_record, sizeof(committed_record), tag, sha);
    write_fixture(RECORD_PATH ".next", committed_record);
    if (rename(RECORD_PATH ".next", RECORD_PATH)) {
        fprintf(stderr, "FAIL: cannot commit %s\n", RECORD_PATH);
        exit(2);
    }
    generations_committed++;
}

static void arm_writer(const char *tag, const char *sha)
{
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
        commit_generation(writer_tag, writer_sha);
    }
    return f;
}

/* ---- the reader under test ---------------------------------------------- */

/* Reads both keys through the production reader, poisoning the buffers first so
   an unresolved value can only be an empty string. */
static int read_pair(char *tag, size_t tag_size, char *sha, size_t sha_size)
{
    memset(tag, 'Z', tag_size);
    memset(sha, 'Z', sha_size);
    return update_identity_pair(RECORD_PATH, tag, tag_size, sha, sha_size);
}

static void check_pair(int resolved, const char *tag, const char *sha,
                       const char *what)
{
    char got_tag[LE_UPDATE_TAG_SIZE];
    char got_sha[LE_UPDATE_SHA_SIZE];
    int rc = read_pair(got_tag, sizeof(got_tag), got_sha, sizeof(got_sha));
    check(rc == resolved && !strcmp(got_tag, tag) && !strcmp(got_sha, sha), what);
}

/* The reader this PR replaced opened the record once per key. It is reproduced
   here so the regression stays pinned against the design that allowed it: the
   tag is read in one pass, the digest in a second one. */
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
    char small[8];
    char tag[LE_UPDATE_TAG_SIZE];
    char sha[LE_UPDATE_SHA_SIZE];
    size_t i;
    int rc;

    /* A completed dev check: both values are published exactly. */
    generation_body(record, sizeof(record), TAG, SHA);
    write_record(record);
    check_pair(2, TAG, SHA, "recorded identity returned exactly");

    /* The record's key order is the writer's business, not the reader's. */
    snprintf(record, sizeof(record),
             "schema=1\nstatus=update-available\nota_sha256=%s\n"
             "resolved_release_tag=%s\n", SHA, TAG);
    write_record(record);
    check_pair(2, TAG, SHA, "identity returned in either key order");

    /* CRLF records do not leave the terminator in the value. */
    snprintf(record, sizeof(record),
             "status=update-available\nresolved_release_tag=%s\r\nota_sha256=%s\r\n",
             TAG, SHA);
    write_record(record);
    check_pair(2, TAG, SHA, "CRLF record returns values without terminators");

    /* A record written before these keys existed (older device). */
    write_record("schema=1\nsource=github-releases\nchannel=dev\n"
                 "status=update-available\nsource_reachable=true\n"
                 "latest_version=0.14.0\n");
    check_pair(0, "", "", "absent keys are empty");

    /* Channel change and failed checks write the record with the keys cleared,
       so nothing can be inherited from an earlier check. */
    write_record("schema=1\nsource=github-releases\nchannel=stable\n"
                 "status=not-checked\nsource_reachable=unknown\n"
                 "latest_version=\nresolved_release_tag=\nota_sha256=\n");
    check_pair(0, "", "", "cleared identity after a channel change is empty");
    write_record("schema=1\nstatus=error\nerror=download_transport\n"
                 "resolved_release_tag=\nota_sha256=\n");
    check_pair(0, "", "", "cleared identity after a failed check is empty");

    /* A stable candidate has no immutable identity. */
    write_record("status=up-to-date\nlatest_version=0.14.0\n"
                 "resolved_release_tag=\nota_sha256=\n");
    check_pair(0, "", "", "stable candidate exposes no identity");

    /* Each value is validated on its own: a malformed tag cannot withdraw a
       recorded digest, and neither is repaired into shape. */
    snprintf(record, sizeof(record),
             "status=update-available\nresolved_release_tag=RADAR-PUFFIN-BUILD-"
             "0123456-0123456789abcdef-fedcba9876543210\nota_sha256=%s\n", SHA);
    write_record(record);
    check_pair(1, "", SHA, "malformed tag reported absent, digest unaffected");

    {
        char short_sha[80];
        memset(short_sha, 'a', 63);
        short_sha[63] = '\0';
        snprintf(record, sizeof(record),
                 "status=update-available\nresolved_release_tag=0.14.0\n"
                 "ota_sha256=%s\n", short_sha);
        write_record(record);
    }
    check_pair(0, "", "", "version string and short digest are both absent");

    write_record("resolved_release_tag= radar-puffin-build-0123456-"
                 "0123456789abcdef-fedcba9876543210 \nota_sha256="
                 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeF\n");
    check_pair(0, "", "", "padded tag and mixed-case digest are absent");

    /* The record writer never repeats a key; if one is repeated the first
       occurrence wins, so a later line cannot displace a value that already
       parsed. */
    snprintf(record, sizeof(record),
             "resolved_release_tag=%s\nresolved_release_tag=%s\nota_sha256=%s\n",
             TAG, NEXT_TAG, SHA);
    write_record(record);
    check_pair(2, TAG, SHA, "the first occurrence of a repeated key wins");

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
    check_pair(0, "", "", "oversized recorded values are absent");

    /* A value that does not fit the caller's buffer stays absent rather than
       being cut down to a buffer-sized prefix; the other value is still read,
       because the two are resolved independently. */
    generation_body(record, sizeof(record), TAG, SHA);
    write_record(record);
    rc = read_pair(small, sizeof(small), sha, sizeof(sha));
    check(rc == 1 && small[0] == '\0' && !strcmp(sha, SHA),
          "value larger than its buffer is absent, the other still resolves");

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
    check_pair(2, TAG, SHA, "identity read after a long unrelated value");

    /* The final keys are readable without a trailing newline. */
    snprintf(record, sizeof(record),
             "status=update-available\nresolved_release_tag=%s\nota_sha256=%s",
             TAG, SHA);
    write_record(record);
    check_pair(2, TAG, SHA, "final keys without a trailing newline are read");

    /* A rejected call clears both buffers rather than leaving a stale identity,
       which is what a client would otherwise render. */
    memset(tag, 'Z', sizeof(tag));
    memset(sha, 'Z', sizeof(sha));
    check(update_identity_pair(NULL, tag, sizeof(tag), sha, sizeof(sha)) == 0 &&
          tag[0] == '\0' && sha[0] == '\0',
          "null path is rejected and clears both values");
    tag[0] = 'Z';
    sha[0] = 'Z';
    check(update_identity_pair(RECORD_PATH, tag, 0, sha, sizeof(sha)) == 0 &&
          tag[0] == 'Z', "zero-sized value buffer is rejected untouched");
    check(update_identity_pair(RECORD_PATH, NULL, sizeof(tag), sha,
                               sizeof(sha)) == 0,
          "null tag buffer is rejected");
    check(update_identity_pair(RECORD_PATH, tag, sizeof(tag), NULL,
                               sizeof(sha)) == 0,
          "null digest buffer is rejected");
    check(update_identity_pair(RECORD_PATH, tag, sizeof(tag), sha, 0) == 0,
          "zero-sized digest buffer is rejected");

    /* An unreadable record reports absence rather than an error state. */
    remove(RECORD_PATH);
    check_pair(0, "", "", "missing record exposes no identity");
}

/* Two generations of the same shape, for the interrupted-read loop. */
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
    char tag[LE_UPDATE_TAG_SIZE];
    char sha[LE_UPDATE_SHA_SIZE];
    char prev_tag[LE_UPDATE_TAG_SIZE];
    char prev_sha[LE_UPDATE_SHA_SIZE];
    char next_tag[LE_UPDATE_TAG_SIZE];
    char next_sha[LE_UPDATE_SHA_SIZE];
    char what[96];
    unsigned i;
    int rc;

    check(strcmp(TAG, NEXT_TAG) && strcmp(SHA, NEXT_SHA),
          "the fixture generations are distinguishable");

    /* The interleaving: a check worker commits the next candidate as soon as
       the reader has opened the record. The pair reader opened it once, so both
       fields still come from the generation it opened. */
    generation_body(record, sizeof(record), TAG, SHA);
    write_record(record);
    record_opens = 0;
    generations_committed = 0;
    writer_armed = 0;
    arm_writer(NEXT_TAG, NEXT_SHA);
    rc = read_pair(tag, sizeof(tag), sha, sizeof(sha));
    check(rc == 2, "a torn-read interleaving still resolves both values");
    check(record_opens == 1,
          "the pair is read with exactly one open of the record");
    check(!strcmp(tag, TAG) && !strcmp(sha, SHA),
          "both values come from the generation the reader opened");
    check(generations_committed == 1 && strstr(committed_record, NEXT_TAG) &&
          strstr(committed_record, NEXT_SHA),
          "the check writer committed the next candidate during the read");
    check(strcmp(sha, NEXT_SHA) != 0 && strcmp(tag, NEXT_TAG) != 0,
          "a pair mixed across generations is never published");

    /* The committed candidate is what the next read sees, whole. */
    check_pair(2, NEXT_TAG, NEXT_SHA,
               "the next read sees the committed candidate whole");

    /* The same harness against the removed design: it reads the tag in one pass
       and the digest in a second, so the commit that lands between its two opens
       makes it publish the first candidate's tag with the second candidate's
       digest -- an identity that belongs to no artifact. */
    generation_body(record, sizeof(record), TAG, SHA);
    write_record(record);
    record_opens = 0;
    writer_armed = 0;
    arm_writer(NEXT_TAG, NEXT_SHA);
    memset(tag, 'Z', sizeof(tag));
    memset(sha, 'Z', sizeof(sha));
    rc = legacy_read_key(RECORD_PATH, LE_UPDATE_TAG_KEY, tag, sizeof(tag));
    rc += legacy_read_key(RECORD_PATH, LE_UPDATE_SHA_KEY, sha, sizeof(sha));
    check(rc == 2 && record_opens == 2,
          "the removed reader resolved both keys over two opens");
    check(!strcmp(tag, TAG) && !strcmp(sha, NEXT_SHA),
          "the removed two-open reader paired one candidate's tag with the next candidate's digest");

    /* Every generation boundary the reader can be interrupted at: it has one,
       the open. Commit the next candidate at that point on each of eight
       successive checks and the published pair still belongs to one candidate. */
    generated_identity(0, prev_tag, sizeof(prev_tag), prev_sha, sizeof(prev_sha));
    generation_body(record, sizeof(record), prev_tag, prev_sha);
    write_record(record);
    for (i = 1; i <= 8; i++) {
        generated_identity(i, next_tag, sizeof(next_tag), next_sha,
                           sizeof(next_sha));
        record_opens = 0;
        arm_writer(next_tag, next_sha);
        rc = read_pair(tag, sizeof(tag), sha, sizeof(sha));
        snprintf(what, sizeof(what),
                 "generation %u read whole while the next was committed", i);
        check(rc == 2 && record_opens == 1 && !strcmp(tag, prev_tag) &&
              !strcmp(sha, prev_sha), what);
        writer_armed = 0;
        memcpy(prev_tag, next_tag, sizeof(prev_tag));
        memcpy(prev_sha, next_sha, sizeof(prev_sha));
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
