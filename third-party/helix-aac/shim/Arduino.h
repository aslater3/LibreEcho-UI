/*
 * Minimal Arduino.h shim for the vendored Helix fixed-point AAC decoder.
 *
 * The upstream sources in this directory are built for the Arduino core, so
 * aaccommon.h includes <Arduino.h> and <pgmspace.h>. LibreEcho builds the
 * decoder as plain C99 on a POSIX host (and cross-compiles it for ARM32 the
 * same way), so this shim only pulls in the C standard headers the decoder
 * actually uses. The program-space helpers live in the sibling pgmspace.h.
 *
 * LibreEcho-authored file (MIT), not part of the upstream Helix distribution.
 */
#ifndef LE_HELIX_AAC_SHIM_ARDUINO_H
#define LE_HELIX_AAC_SHIM_ARDUINO_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#endif /* LE_HELIX_AAC_SHIM_ARDUINO_H */
