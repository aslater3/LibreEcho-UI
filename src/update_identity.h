#ifndef LE_UPDATE_IDENTITY_H
#define LE_UPDATE_IDENTITY_H

#include <stddef.h>

/* Keys the update helper records in /data/libreecho/update/check-status to name
   the candidate a completed check resolved. */
#define LE_UPDATE_TAG_KEY "resolved_release_tag"
#define LE_UPDATE_SHA_KEY "ota_sha256"

/* Storage for one bounded record value. The enforced grammars are 60 or 62
   characters for the tag and exactly 64 for the digest, so these buffers hold
   any value the helper can legitimately write without truncation. */
#define LE_UPDATE_TAG_SIZE 96
#define LE_UPDATE_SHA_SIZE 80

/* Fields one call reads out of the check record. The record describes a single
   check, so a caller's table holds the fields that must agree with each other. */
#define LE_UPDATE_RECORD_FIELD_MAX 16

/* One named field of that record: the key exactly as the helper writes it, the
   caller's buffer, and that buffer's size. */
struct le_update_field {
    const char *key;
    char *value;
    size_t size;
};

/* Read every named field of the update check record from a single opened
 * snapshot of it.
 *
 * The record is committed by the update helper with an atomic rename, and the
 * check and channel actions run in child workers while GETs stay serviceable,
 * so reading the record once per key can describe two different checks at once:
 * an old status or version beside the next check's identity, or a tag from one
 * check with the digest from the next. This reader holds one descriptor for the
 * whole table, so every value it reports belongs to one generation of the
 * record.
 *
 * The two identity keys are format checked against the same grammar the update
 * helper enforces before recording them, and their buffers always describe what
 * the device actually resolved: a missing, oversized or malformed identity is
 * reported as an empty string, never truncated or guessed.
 *
 *   resolved_release_tag  radar-puffin-(build|nightly)-<7 hex>-<16 hex>-<16 hex>
 *   ota_sha256            exactly 64 lower-case hexadecimal digits
 *
 * Every other key is copied bounded and unvalidated, as this envelope has always
 * reported those fields, and its buffer is left as the caller initialised it
 * when the record does not carry the key -- an older record therefore keeps the
 * documented default rather than an empty value.
 *
 * Returns the number of fields the record carried, counting an identity only
 * when it validated. At most LE_UPDATE_RECORD_FIELD_MAX fields are read; a
 * missing path, a missing table or an unreadable record reports none. */
int update_record_read(const char *path, struct le_update_field *fields,
                       size_t count);

/* Pure validators, also used directly by the contract tests. */
int update_identity_tag_valid(const char *value);
int update_identity_sha256_valid(const char *value);

#endif
