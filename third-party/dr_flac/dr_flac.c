/*
 * The single implementation unit for the vendored dr_flac (see
 * README.libreecho.md). Built with upstream warnings off, like the Helix AAC
 * decoder; LibreEcho's wrapper, src/adapter/radio_flac.c, keeps strict flags.
 */
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_SIMD
#include "dr_flac.h"
