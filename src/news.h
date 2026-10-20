#ifndef LE_NEWS_H
#define LE_NEWS_H
/*
 * LibreEcho News (#121): a small curated feed of releases, progress,
 * announcements and security notes, fetched from a public file and shown on
 * the dashboard. The feed is plain text only and strictly validated here; the
 * browser renders the normalised items, never the downloaded bytes.
 */
#include <stddef.h>

#define LE_NEWS_MAX_BYTES 32768
#define LE_NEWS_MAX_ITEMS 20

struct le_news_item {
    char id[65], type[16], date[11], title[121], summary[401], url[257], severity[12];
};

/* Validate a feed document. Returns 0 and fills items/count, or -1 when the
   whole document must be rejected (syntax, size, schema, duplicate keys, more
   than LE_NEWS_MAX_ITEMS items, or an invalid item). Items of an unknown type
   are dropped, not fatal, for forward compatibility. */
int le_news_parse(const char *doc, size_t len, struct le_news_item *items, size_t max, size_t *count);

/* Serialise items as a JSON array into out; -1 if it does not fit. */
int le_news_items_json(const struct le_news_item *items, size_t count, char *out, size_t size);

/* A feed URL host the device and browser accept for item links. */
int le_news_url_allowed(const char *url);
#endif
