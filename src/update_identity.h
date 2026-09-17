#ifndef LE_UPDATE_IDENTITY_H
#define LE_UPDATE_IDENTITY_H

#include <stddef.h>

/* Keys the update helper records in /data/libreecho/update/check-status to
   name the candidate a completed check resolved. */
#define LE_UPDATE_TAG_KEY "resolved_release_tag"
#define LE_UPDATE_SHA_KEY "ota_sha256"

/* Storage for one bounded record value. The enforced grammars are 60 or 62
   characters for the tag and exactly 64 for the digest, so these buffers hold
   any value the helper can legitimately write without truncation. */
#define LE_UPDATE_TAG_SIZE 96
#define LE_UPDATE_SHA_SIZE 80

/* Read one identity value out of an update check record.
 *
 * The two identity keys are format checked against the same grammar the update
 * helper enforces before recording them, so this reader can never publish a
 * tag or digest the device did not actually resolve:
 *
 *   resolved_release_tag  radar-puffin-(build|nightly)-<7 hex>-<16 hex>-<16 hex>
 *   ota_sha256            exactly 64 lower-case hexadecimal digits
 *
 * Any other key is returned bounded but unvalidated, so a record written by a
 * newer helper stays readable. The read is bounded by `size`: a value that does
 * not fit the caller's buffer, a missing key, an absent or unreadable record, an
 * empty value, and a malformed value all report absence (return 0, out[0] = 0)
 * rather than a truncated or guessed identity. */

int update_identity_value(const char *path, const char *key, char *out,
                          size_t size);

/* Pure validators, also used directly by the contract tests. */
int update_identity_tag_valid(const char *value);
int update_identity_sha256_valid(const char *value);

#endif
