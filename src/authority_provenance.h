#ifndef LIBREECHO_AUTHORITY_PROVENANCE_H
#define LIBREECHO_AUTHORITY_PROVENANCE_H

#include <stddef.h>

#define LE_AUTHORITY_PROVENANCE_OUTPUT_MAX 8192
#define LE_AUTHORITY_PROVENANCE_JSON_MAX 16384

/* Starts and advances one bounded helper read; it never waits for the child. */
void le_authority_provenance_tick(void);
void le_authority_provenance_json(char *out, size_t size);
void le_authority_provenance_shutdown(void);

#endif
