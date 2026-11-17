#ifndef LE_OWNER_EQ_H
#define LE_OWNER_EQ_H
/*
 * Four-band owner equaliser and speaker boost (shared by audiod, the web API
 * and both backends, so the bands, presets and number rules exist once).
 *
 * Band gains are held as whole half-decibels (-12..12 = -6.0..+6.0 dB in 0.5
 * dB steps), so no floating point is needed to validate, compare or print
 * them. The preset name is display only: the four numbers are what apply.
 * The audio engine reads <runtime>/tone.conf (schema 2) and boost.conf, which
 * audiod writes only while they are not flat / 0 dB.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define LE_EQ_BANDS 4
#define LE_EQ_LIMIT_HALVES 12          /* +-6.0 dB */
#define LE_EQ_PRESET_MAX 16            /* longest name + NUL */
#define LE_EQ_PRESET_COUNT 7           /* named presets; "custom" is extra */
enum { LE_EQ_BASS, LE_EQ_LOW_MID, LE_EQ_HIGH_MID, LE_EQ_TREBLE };

/* API / status member names, in band order. */
static inline const char *le_eq_band_name(int band)
{
    switch (band) {
    case LE_EQ_BASS: return "bass";
    case LE_EQ_LOW_MID: return "low_mid";
    case LE_EQ_HIGH_MID: return "high_mid";
    case LE_EQ_TREBLE: return "treble";
    default: return NULL;
    }
}

/* Name and values (half-dB, band order) of named preset `index`; NULL past the end. */
static inline const char *le_eq_preset(int index, int halves[LE_EQ_BANDS])
{
    static const struct { const char *name; signed char h[LE_EQ_BANDS]; } presets[LE_EQ_PRESET_COUNT] = {
        {"flat", {0, 0, 0, 0}},
        {"bass_boost", {10, 2, 0, 0}},
        {"treble_boost", {0, 0, 4, 10}},
        {"vocal", {-4, 2, 8, 2}},
        {"loudness", {8, -2, 2, 8}},
        {"late_night", {-8, 0, 4, 0}},
        {"warm", {6, 2, -2, -6}}
    };
    int i;

    if (index < 0 || index >= LE_EQ_PRESET_COUNT)
        return NULL;
    if (halves)
        for (i = 0; i < LE_EQ_BANDS; i++)
            halves[i] = presets[index].h[i];
    return presets[index].name;
}

/* Index of a named preset, LE_EQ_PRESET_COUNT for "custom", -1 for anything else. */
static inline int le_eq_preset_index(const char *name)
{
    int i;
    const char *known;

    if (!name)
        return -1;
    for (i = 0; (known = le_eq_preset(i, NULL)) != NULL; i++)
        if (!strcmp(name, known))
            return i;
    return strcmp(name, "custom") ? -1 : LE_EQ_PRESET_COUNT;
}

static inline int le_eq_valid_halves(const int halves[LE_EQ_BANDS])
{
    int i;

    for (i = 0; i < LE_EQ_BANDS; i++)
        if (halves[i] < -LE_EQ_LIMIT_HALVES || halves[i] > LE_EQ_LIMIT_HALVES)
            return 0;
    return 1;
}

static inline int le_eq_is_flat(const int halves[LE_EQ_BANDS])
{
    return !halves[0] && !halves[1] && !halves[2] && !halves[3];
}

/*
 * The name shown for these four values when `requested` was asked for: a named
 * preset only while the values are exactly its own, "flat" whenever all four
 * are 0, otherwise "custom". NULL when `requested` is not a known name.
 */
static inline const char *le_eq_canonical_preset(const char *requested, const int halves[LE_EQ_BANDS])
{
    int index = le_eq_preset_index(requested), values[LE_EQ_BANDS];
    const char *name;

    if (index < 0)
        return NULL;
    if (le_eq_is_flat(halves))
        return "flat";
    if (index < LE_EQ_PRESET_COUNT) {
        name = le_eq_preset(index, values);
        if (!memcmp(values, halves, sizeof(values)))
            return name;
    }
    return "custom";
}

static inline int le_boost_valid(int db)
{
    return db == 0 || db == 3 || db == 6;
}

/*
 * Parse one JSON number from s[0..n) as whole half-decibels: an optional
 * minus, an integer part without leading zeros, and an optional fraction that
 * must be .0 or .5 followed only by zeros. No exponent, no plus. Returns the
 * number of bytes consumed, or 0 when the text is not such a number or is
 * outside -6..6 dB.
 */
static inline size_t le_eq_parse_halves(const char *s, size_t n, int *halves)
{
    size_t i = 0;
    int negative = 0, whole = 0, half = 0, digits = 0;

    if (!s || !n)
        return 0;
    if (s[i] == '-') {
        negative = 1;
        i++;
    }
    if (i >= n || s[i] < '0' || s[i] > '9')
        return 0;
    if (s[i] == '0' && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')
        return 0;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        if (++digits > 2)
            return 0;
        whole = whole * 10 + (s[i++] - '0');
    }
    if (i < n && s[i] == '.') {
        if (++i >= n || s[i] < '0' || s[i] > '9')
            return 0;
        if (s[i] == '5') {
            half = 1;
            i++;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (s[i] != '0')
                return 0;
            i++;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E' || s[i] == '.'))
        return 0;
    whole = whole * 2 + half;
    if (whole > LE_EQ_LIMIT_HALVES)
        return 0;
    *halves = negative ? -whole : whole;
    return i;
}

/* A dB value as text: JSON style ("-1.5", "4", "0") or, with fixed2, the
 * tone.conf style with two decimals ("-1.50", "4.00", "0.00"). */
static inline int le_eq_format_db(char *out, size_t size, int halves, int fixed2)
{
    int magnitude = halves < 0 ? -halves : halves;
    const char *sign = halves < 0 ? "-" : "";
    int n;

    if (magnitude > LE_EQ_LIMIT_HALVES)
        return -1;
    if (fixed2)
        n = snprintf(out, size, "%s%d.%s", sign, magnitude / 2, magnitude % 2 ? "50" : "00");
    else if (magnitude % 2)
        n = snprintf(out, size, "%s%d.5", sign, magnitude / 2);
    else
        n = snprintf(out, size, "%s%d", sign, magnitude / 2);
    return n > 0 && (size_t)n < size ? n : -1;
}

/*
 * Find "key": <number> in s[0..n) (the first occurrence of the quoted key
 * followed by a colon) and parse it as half-decibels. The number must end at
 * a JSON delimiter. 0: absent; 1: found and valid; -1: present but invalid.
 */
static inline int le_eq_find_number(const char *s, size_t n, const char *key, int *halves)
{
    size_t key_len = key ? strlen(key) : 0, i;

    if (!s || !key_len)
        return 0;
    for (i = 0; i + key_len + 2 <= n; i++) {
        size_t j, used;

        if (s[i] != '"' || memcmp(s + i + 1, key, key_len) || s[i + key_len + 1] != '"')
            continue;
        j = i + key_len + 2;
        while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n'))
            j++;
        if (j >= n || s[j] != ':')
            continue;
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r' || s[j] == '\n'))
            j++;
        used = le_eq_parse_halves(s + j, n - j, halves);
        if (!used)
            return -1;
        j += used;
        if (j < n && s[j] != ',' && s[j] != '}' && s[j] != ']' && s[j] != ' ' &&
            s[j] != '\t' && s[j] != '\r' && s[j] != '\n')
            return -1;
        return 1;
    }
    return 0;
}

/* The runtime tone.conf body for these values (schema 2, two decimals). */
static inline int le_eq_tone_conf(char *out, size_t size, const int halves[LE_EQ_BANDS])
{
    char v[LE_EQ_BANDS][12];
    int i, n;

    for (i = 0; i < LE_EQ_BANDS; i++)
        if (le_eq_format_db(v[i], sizeof(v[i]), halves[i], 1) < 0)
            return -1;
    n = snprintf(out, size, "schema=2\nbass_db=%s\nlow_mid_db=%s\nhigh_mid_db=%s\ntreble_db=%s\n",
                 v[0], v[1], v[2], v[3]);
    return n > 0 && (size_t)n < size ? n : -1;
}

/* The runtime boost.conf body (schema 1). */
static inline int le_boost_conf(char *out, size_t size, int db)
{
    int n = snprintf(out, size, "schema=1\nboost_db=%d\n", db);

    return n > 0 && (size_t)n < size ? n : -1;
}

/* The "eq" JSON object for these values, e.g. {"preset":"flat","bass":0,...}. */
static inline int le_eq_json(char *out, size_t size, const char *preset, const int halves[LE_EQ_BANDS])
{
    char v[LE_EQ_BANDS][12];
    int i, n;

    for (i = 0; i < LE_EQ_BANDS; i++)
        if (le_eq_format_db(v[i], sizeof(v[i]), halves[i], 0) < 0)
            return -1;
    n = snprintf(out, size, "{\"preset\":\"%s\",\"bass\":%s,\"low_mid\":%s,\"high_mid\":%s,\"treble\":%s}",
                 preset, v[0], v[1], v[2], v[3]);
    return n > 0 && (size_t)n < size ? n : -1;
}
#endif
