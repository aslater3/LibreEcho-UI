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

/* Read the candidate identity out of an update check record.
 *
 * The two identity keys are format checked against the same grammar the update
 * helper enforces before recording them, so this reader can never publish a
 * tag or digest the device did not actually resolve:
 *
 *   resolved_release_tag  radar-puffin-(build|nightly)-<7 hex>-<16 hex>-<16 hex>
 *   ota_sha256            exactly 64 lower-case hexadecimal digits
 *
 * Both keys are read from a single opened snapshot of the record, and the
 * helper commits a new record with an atomic rename. Reading the tag and the
 * digest in separate passes could pair the tag from one check with the digest
 * from the next and publish a mixed identity that belongs to no artifact, so
 * callers that publish both must use this function rather than reading the
 * record twice.
 *
 * Returns the number of identity values resolved (0..2). Every unresolved
 * value is an empty string: a missing key, an absent or unreadable record, an
 * empty value, a malformed value, and a value that does not fit the caller's
 * buffer all report absence rather than a truncated or guessed identity. The
 * two values are resolved independently, so a record that carries only one
 * usable value still reports the other as absent. */
int update_identity_pair(const char *path, char *tag, size_t tag_size,
                         char *sha, size_t sha_size);

/* Pure validators, also used directly by the contract tests. */
int update_identity_tag_valid(const char *value);
int update_identity_sha256_valid(const char *value);

#endif
