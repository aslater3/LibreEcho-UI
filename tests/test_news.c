/* LibreEcho News feed parser (#121): strict validation, forward compatibility, ordering. */
#include "news.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static int parse(const char *doc, struct le_news_item *items, size_t *count)
{
    return le_news_parse(doc, strlen(doc), items, LE_NEWS_MAX_ITEMS, count);
}

#define ITEM(id, type, date) "{\"id\":\"" id "\",\"type\":\"" type "\",\"date\":\"" date "\",\"title\":\"T " id "\",\"summary\":\"S\",\"url\":\"https://github.com/aslater3/LibreEcho/releases\"}"

int main(void)
{
    struct le_news_item items[LE_NEWS_MAX_ITEMS];
    size_t count = 0;
    char json[16384], doc[40000];
    int i;

    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"generated\":\"x\",\"items\":["
                ITEM("release-a", "release", "2026-09-01") ","
                ITEM("progress-b", "progress", "2026-10-16") ","
                ITEM("future-c", "podcast", "2026-10-17") "]}", items, &count) == 0, "valid feed parses");
    CHECK(count == 2, "unknown item type is dropped, not fatal");
    CHECK(!strcmp(items[0].id, "progress-b") && !strcmp(items[1].id, "release-a"), "newest first");

    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[]}", items, &count) == 0 && count == 0, "empty feed is valid");
    CHECK(parse("{\"schema\":\"libreecho-news-v2\",\"items\":[]}", items, &count) < 0, "wrong schema rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\"}", items, &count) < 0, "missing items rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"schema\":\"libreecho-news-v1\",\"items\":[]}", items, &count) < 0, "duplicate schema rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[" ITEM("a", "release", "2026-01-01") "," ITEM("a", "release", "2026-01-02") "]}", items, &count) < 0, "duplicate id rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"https://evil.example/x\"}]}", items, &count) < 0, "foreign link host rejects the document");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"http://github.com/x\"}]}", items, &count) < 0, "plain http link rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"release\",\"date\":\"2026-13-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"https://github.com/x\"}]}", items, &count) < 0, "invalid month rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"X\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"https://github.com/x\"}]}", items, &count) < 0, "uppercase id rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\\nU\",\"summary\":\"S\",\"url\":\"https://github.com/x\"}]}", items, &count) < 0, "control character in title rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"security\",\"date\":\"2026-01-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"https://libreecho.org/s\",\"severity\":\"panic\"}]}", items, &count) < 0, "unknown severity rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\",\"url\":\"https://github.com/x\"}]}", items, &count) < 0, "missing summary rejected");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"x\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"https://github.com/x\",\"channels\":[\"stable\"]}]}", items, &count) == 0 && count == 1, "unknown member ignored");
    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[" ITEM("a", "release", "2026-01-01"), items, &count) < 0, "truncated document rejected");

    strcpy(doc, "{\"schema\":\"libreecho-news-v1\",\"items\":[");
    for (i = 0; i < 21; i++) {
        char one[300];
        snprintf(one, sizeof(one), "%s{\"id\":\"i%d\",\"type\":\"release\",\"date\":\"2026-01-01\",\"title\":\"T\",\"summary\":\"S\",\"url\":\"https://github.com/x\"}", i ? "," : "", i);
        strcat(doc, one);
    }
    strcat(doc, "]}");
    CHECK(parse(doc, items, &count) < 0, "more than 20 items rejected");

    memset(doc, ' ', LE_NEWS_MAX_BYTES + 10);
    doc[0] = '{'; doc[LE_NEWS_MAX_BYTES + 9] = '}'; doc[LE_NEWS_MAX_BYTES + 10] = 0;
    CHECK(parse(doc, items, &count) < 0, "oversized document rejected");

    CHECK(parse("{\"schema\":\"libreecho-news-v1\",\"items\":[{\"id\":\"q\",\"type\":\"announcement\",\"date\":\"2026-02-03\",\"title\":\"Say \\\"hi\\\" <b>\",\"summary\":\"a/b\",\"url\":\"https://dl.libreecho.org/n\",\"severity\":\"info\"}]}", items, &count) == 0, "escaped quotes parse");
    CHECK(le_news_items_json(items, count, json, sizeof(json)) == 0, "serialise");
    CHECK(strstr(json, "\"title\":\"Say \\\"hi\\\" <b>\"") != NULL, "title re-escaped for JSON");
    CHECK(strstr(json, "\"severity\":\"info\"") != NULL, "severity kept");

    CHECK(le_news_url_allowed("https://github.com/aslater3/LibreEcho/releases/tag/v1"), "github allowed");
    CHECK(!le_news_url_allowed("https://github.com.evil.example/x"), "lookalike host refused");
    CHECK(!le_news_url_allowed("https://github.com/x\"onmouseover"), "quote in url refused");

    if (failures) { fprintf(stderr, "news parser: %d failure(s)\n", failures); return 1; }
    puts("news parser: ok");
    return 0;
}
