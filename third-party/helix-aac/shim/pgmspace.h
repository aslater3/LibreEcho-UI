/*
 * Minimal <pgmspace.h> shim for the vendored Helix fixed-point AAC decoder.
 *
 * On AVR/Arduino targets the decoder's constant tables are placed in program
 * flash with PROGMEM and read back through the pgm_read_* helpers. On the host
 * (and on ARM Linux) there is a single address space, so PROGMEM is empty and
 * the helpers are plain dereferences. The tables stay ordinary objects; only
 * the read syntax is kept so the vendored sources compile unmodified.
 *
 * LibreEcho-authored file (MIT), not part of the upstream Helix distribution.
 */
#ifndef LE_HELIX_AAC_SHIM_PGMSPACE_H
#define LE_HELIX_AAC_SHIM_PGMSPACE_H

#define PROGMEM

#define pgm_read_byte(p) (*(const unsigned char *)(p))
#define pgm_read_word(p) (*(const unsigned short *)(p))

#endif /* LE_HELIX_AAC_SHIM_PGMSPACE_H */
