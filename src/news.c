#include "news.h"
#include "json.h"

#include <stdio.h>
#include <string.h>

/* Item links may only point at the project's own public sites. */
int le_news_url_allowed(const char *url)
{
    static const char *const hosts[] = { "github.com", "libreecho.org", "dl.libreecho.org" };
    const char *host, *end;
    size_t i, n;

    if (!url || strncmp(url, "https://", 8) || strlen(url) > 256)
        return 0;
    host = url + 8;
    end = host + strcspn(host, "/?#");
    n = (size_t)(end - host);
    for (i = 0; url[i]; i++)
        if ((unsigned char)url[i] <= 0x20 || url[i] == '"' || url[i] == '\\' ||
            url[i] == '<' || url[i] == '>' || (unsigned char)url[i] >= 0x7f)
            return 0;
    for (i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++)
        if (strlen(hosts[i]) == n && !strncmp(host, hosts[i], n))
            return 1;
    return 0;
}

static int member_is(const struct json_member *m, const char *name)
{
    return m->name_len == strlen(name) && !memcmp(m->name, name, m->name_len);
}

/* Plain single-line text: no control characters. Length is checked by the caller's buffer. */
static int plain_text(const char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f)
            return 0;
    return 1;
}

static int valid_id(const char *s)
{
    size_t i;
    if (!s[0] || strlen(s) > 64)
        return 0;
    for (i = 0; s[i]; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '.' || s[i] == '_' || s[i] == '-'))
            return 0;
    return 1;
}

static int valid_date(const char *s)
{
    int i;
    if (strlen(s) != 10 || s[4] != '-' || s[7] != '-')
        return 0;
    for (i = 0; i < 10; i++)
        if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9'))
            return 0;
    return (s[5] == '0' || (s[5] == '1' && s[6] <= '2')) && !(s[5] == '0' && s[6] == '0') &&
           !(s[8] == '0' && s[9] == '0') && (s[8] < '3' || (s[8] == '3' && s[9] <= '1'));
}

/* Next top-level element of a JSON array body; strings and nesting aware, bounded by len. */
static int next_element(const char *s, size_t len, size_t *pos, size_t *start, size_t *end)
{
    size_t i = *pos;
    int depth = 0, in_string = 0, escaped = 0;

    while (i < len && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t' || s[i] == ','))
        i++;
    if (i >= len || s[i] == ']')
        return 0;
    *start = i;
    for (; i < len; i++) {
        char c = s[i];
        if (in_string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') in_string = 1;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') {
            if (--depth == 0) { *end = i + 1; *pos = i + 1; return 1; }
            if (depth < 0) return -1;
        }
    }
    return -1;
}

static int parse_item(const char *s, size_t n, struct le_news_item *it, int *known_type)
{
    struct json_member m[12];
    size_t count = 0, k;
    int have = 0;

    memset(it, 0, sizeof(*it));
    if (json_object_members(s, n, m, 12, &count))
        return -1;
    for (k = 0; k < count; k++) {
        size_t j;
        for (j = 0; j < k; j++)
            if (m[j].name_len == m[k].name_len && !memcmp(m[j].name, m[k].name, m[k].name_len))
                return -1;
        if (member_is(&m[k], "id")) { if (json_string_span(m[k].value, m[k].value_len, it->id, sizeof(it->id)) < 0) return -1; have |= 1; }
        else if (member_is(&m[k], "type")) { if (json_string_span(m[k].value, m[k].value_len, it->type, sizeof(it->type)) < 0) return -1; have |= 2; }
        else if (member_is(&m[k], "date")) { if (json_string_span(m[k].value, m[k].value_len, it->date, sizeof(it->date)) < 0) return -1; have |= 4; }
        else if (member_is(&m[k], "title")) { if (json_string_span(m[k].value, m[k].value_len, it->title, sizeof(it->title)) < 0) return -1; have |= 8; }
        else if (member_is(&m[k], "summary")) { if (json_string_span(m[k].value, m[k].value_len, it->summary, sizeof(it->summary)) < 0) return -1; have |= 16; }
        else if (member_is(&m[k], "url")) { if (json_string_span(m[k].value, m[k].value_len, it->url, sizeof(it->url)) < 0) return -1; have |= 32; }
        else if (member_is(&m[k], "severity")) { if (json_string_span(m[k].value, m[k].value_len, it->severity, sizeof(it->severity)) < 0) return -1; }
        /* Unknown members are ignored for forward compatibility. */
    }
    if (have != 63 || !valid_id(it->id) || !valid_date(it->date) || !it->title[0] ||
        !plain_text(it->title) || !plain_text(it->summary) || !le_news_url_allowed(it->url))
        return -1;
    if (it->severity[0] && strcmp(it->severity, "info") && strcmp(it->severity, "important"))
        return -1;
    *known_type = !strcmp(it->type, "release") || !strcmp(it->type, "progress") ||
                  !strcmp(it->type, "announcement") || !strcmp(it->type, "security");
    return 0;
}

int le_news_parse(const char *doc, size_t len, struct le_news_item *items, size_t max, size_t *count)
{
    char schema[32];
    const char *array;
    size_t array_len, pos = 1, start, end, seen = 0, i, j;
    int r;

    *count = 0;
    if (!doc || len == 0 || len > LE_NEWS_MAX_BYTES || !json_valid_object(doc, len) ||
        json_duplicate_key(doc, len, "schema") || json_duplicate_key(doc, len, "items") ||
        json_get_string_top_level(doc, "schema", schema, sizeof(schema)) != 1 ||
        strcmp(schema, "libreecho-news-v1") ||
        json_get_array_top_level(doc, "items", &array, &array_len) != 1 || array_len < 2 || array[0] != '[')
        return -1;
    while ((r = next_element(array, array_len, &pos, &start, &end)) == 1) {
        struct le_news_item it;
        int known = 0;
        if (++seen > LE_NEWS_MAX_ITEMS || parse_item(array + start, end - start, &it, &known))
            return -1;
        if (!known)
            continue;
        for (i = 0; i < *count; i++)
            if (!strcmp(items[i].id, it.id))
                return -1;
        if (*count < max)
            items[(*count)++] = it;
    }
    if (r < 0)
        return -1;
    /* Newest first; ties keep id order so the result is stable. */
    for (i = 1; i < *count; i++)
        for (j = i; j > 0; j--) {
            int c = strcmp(items[j - 1].date, items[j].date);
            if (c > 0 || (c == 0 && strcmp(items[j - 1].id, items[j].id) <= 0))
                break;
            { struct le_news_item t = items[j]; items[j] = items[j - 1]; items[j - 1] = t; }
        }
    return 0;
}

int le_news_items_json(const struct le_news_item *items, size_t count, char *out, size_t size)
{
    size_t used = 0, i;
    char t[256], s[820], u[520];
    int n;

    n = snprintf(out, size, "[");
    if (n < 0 || (size_t)n >= size)
        return -1;
    used = (size_t)n;
    for (i = 0; i < count; i++) {
        json_escape(t, sizeof(t), items[i].title);
        json_escape(s, sizeof(s), items[i].summary);
        json_escape(u, sizeof(u), items[i].url);
        n = snprintf(out + used, size - used,
                     "%s{\"id\":\"%s\",\"type\":\"%s\",\"date\":\"%s\",\"title\":\"%s\",\"summary\":\"%s\",\"url\":\"%s\"%s%s%s}",
                     i ? "," : "", items[i].id, items[i].type, items[i].date, t, s, u,
                     items[i].severity[0] ? ",\"severity\":\"" : "", items[i].severity,
                     items[i].severity[0] ? "\"" : "");
        if (n < 0 || (size_t)n >= size - used)
            return -1;
        used += (size_t)n;
    }
    n = snprintf(out + used, size - used, "]");
    return n < 0 || (size_t)n >= size - used ? -1 : 0;
}
