/*
 * Host tests for the TIDAL client (src/tidal.c).
 *
 * Parsers run against trimmed copies of real TIDAL API replies in
 * tests/fixtures/tidal. The network flow -- device code, pending poll,
 * approval, token refresh, search, album expansion and stream lookup -- runs
 * against tests/fake_tidal_curl.py, which stands in for curl and logs every
 * request so the test can check what was sent and that no token ever reached
 * a command line.
 *
 * Usage: test-tidal <repo-root> <scratch-dir>
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/tidal.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,          \
                    __LINE__, #cond);                                       \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

static char repo[512], scratch[512];

static char *slurp(const char *path, size_t *length)
{
    FILE *f = fopen(path, "rb");
    char *data;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) {
        data[n] = '\0';
        if (length)
            *length = (size_t)n;
    }
    return data;
}

static char *fixture(const char *name, size_t *length)
{
    char path[1024];

    snprintf(path, sizeof(path), "%s/tests/fixtures/tidal/%s", repo, name);
    return slurp(path, length);
}

static void b64(const char *in, char *out, size_t size)
{
    static const char a[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), i, j = 0;

    for (i = 0; i < n && j + 5 < size; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;

        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[j++] = a[(v >> 18) & 63];
        out[j++] = a[(v >> 12) & 63];
        out[j++] = i + 1 < n ? a[(v >> 6) & 63] : '=';
        out[j++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[j] = '\0';
}

static int playback_for(const char *mime, const char *manifest_json,
                        char *url, size_t size)
{
    char encoded[1024], doc[1400];

    b64(manifest_json, encoded, sizeof(encoded));
    snprintf(doc, sizeof(doc),
             "{\"trackId\":1,\"audioQuality\":\"LOSSLESS\","
             "\"manifestMimeType\":\"%s\",\"manifest\":\"%s\"}",
             mime, encoded);
    return le_tidal_parse_playback(doc, strlen(doc), url, size);
}

static void test_parsers(void)
{
    struct le_tidal_results results;
    struct le_tidal_item item;
    char ids[LE_TIDAL_QUEUE_MAX][LE_TIDAL_ID_MAX], url[LE_TIDAL_URL_MAX];
    size_t length = 0, count = 0;
    char *doc;

    doc = fixture("search.json", &length);
    CHECK(doc != NULL);
    CHECK(le_tidal_parse_search(doc, length, &results) == LE_TIDAL_OK);
    CHECK(results.track_count == 3);
    CHECK(results.album_count == 3);
    CHECK(results.playlist_count == 3);
    CHECK(!strcmp(results.tracks[0].id, "1550546"));
    CHECK(!strcmp(results.tracks[0].title, "One More Time"));
    CHECK(!strcmp(results.tracks[0].artist, "Daft Punk"));
    CHECK(!strcmp(results.tracks[0].album, "Discovery"));
    CHECK(results.tracks[0].duration == 320);
    CHECK(!strcmp(results.tracks[0].image,
                  "7d3b9810-5634-400c-ad89-50609e0ce800"));
    CHECK(!strcmp(results.albums[0].id, "20115556"));
    CHECK(!strcmp(results.albums[0].title, "Random Access Memories"));
    CHECK(results.albums[0].tracks == 13);
    CHECK(!strcmp(results.albums[0].artist, "Daft Punk"));
    CHECK(!strcmp(results.playlists[0].id,
                  "abd35446-6a67-4f2b-9f92-0636742c63cb"));
    CHECK(!strcmp(results.playlists[0].title, "Daft Punk Essentials"));
    CHECK(!strcmp(results.playlists[0].artist, "TIDAL"));
    free(doc);

    doc = fixture("album_tracks.json", &length);
    CHECK(le_tidal_parse_track_list(doc, length, ids, LE_TIDAL_QUEUE_MAX,
                                    &count) == LE_TIDAL_OK);
    CHECK(count == 13);
    CHECK(!strcmp(ids[0], "20115557"));
    CHECK(!strcmp(ids[12], "20115569"));
    CHECK(le_tidal_parse_track_list(doc, length, ids, 5, &count) ==
          LE_TIDAL_OK && count == 5);
    free(doc);

    /* The /items form wraps tracks and mixes in videos, which are skipped;
       a track TIDAL marks not stream-ready is skipped too. */
    {
        const char *wrapped =
            "{\"items\":[{\"item\":{\"id\":11,\"title\":\"a\"},\"type\":\"track\"},"
            "{\"item\":{\"id\":12,\"title\":\"v\"},\"type\":\"video\"},"
            "{\"item\":{\"id\":13,\"streamReady\":false},\"type\":\"track\"},"
            "{\"item\":{\"id\":14,\"streamReady\":true},\"type\":\"track\"}]}";

        CHECK(le_tidal_parse_track_list(wrapped, strlen(wrapped), ids, 10,
                                        &count) == LE_TIDAL_OK);
        CHECK(count == 2 && !strcmp(ids[0], "11") && !strcmp(ids[1], "14"));
        CHECK(le_tidal_parse_track_list("{\"items\":[]}", 12, ids, 10,
                                        &count) == LE_TIDAL_NOT_FOUND);
    }

    doc = fixture("track.json", &length);
    CHECK(le_tidal_parse_track(doc, length, &item) == LE_TIDAL_OK);
    CHECK(!strcmp(item.title, "One More Time"));
    CHECK(!strcmp(item.artist, "Daft Punk"));
    free(doc);
    {
        const char *versioned =
            "{\"id\":7,\"title\":\"Song\",\"version\":\"Remastered 2011\","
            "\"artists\":[{\"name\":\"Band \\\"X\\\"\"}],\"album\":{\"title\":\"LP\"}}";

        CHECK(le_tidal_parse_track(versioned, strlen(versioned), &item) ==
              LE_TIDAL_OK);
        CHECK(!strcmp(item.title, "Song (Remastered 2011)"));
        CHECK(!strcmp(item.artist, "Band \"X\""));
    }

    doc = fixture("playbackinfo.json", &length);
    CHECK(le_tidal_parse_playback(doc, length, url, sizeof(url)) ==
          LE_TIDAL_OK);
    CHECK(!strcmp(url, "https://media.example.invalid/track.flac?token=fixture"));
    free(doc);
    CHECK(playback_for("application/vnd.tidal.bts",
                       "{\"mimeType\":\"audio/flac\",\"codecs\":\"flac\","
                       "\"encryptionType\":\"OLD_AES\",\"keyId\":\"k\","
                       "\"urls\":[\"https://x.invalid/a.flac\"]}",
                       url, sizeof(url)) == LE_TIDAL_UNSUPPORTED);
    CHECK(url[0] == '\0');
    CHECK(playback_for("application/vnd.tidal.bts",
                       "{\"mimeType\":\"audio/mp4\",\"codecs\":\"mp4a.40.2\","
                       "\"encryptionType\":\"NONE\","
                       "\"urls\":[\"https://x.invalid/a.mp4\"]}",
                       url, sizeof(url)) == LE_TIDAL_UNSUPPORTED);
    CHECK(playback_for("application/dash+xml", "<MPD/>", url, sizeof(url)) ==
          LE_TIDAL_UNSUPPORTED);
    CHECK(playback_for("application/vnd.tidal.bts",
                       "{\"mimeType\":\"audio/flac\",\"codecs\":\"flac\","
                       "\"encryptionType\":\"NONE\","
                       "\"urls\":[\"http://x.invalid/a.flac\"]}",
                       url, sizeof(url)) == LE_TIDAL_NETWORK);

    CHECK(le_tidal_valid_id(LE_TIDAL_TRACK, "1550546"));
    CHECK(!le_tidal_valid_id(LE_TIDAL_TRACK, "15a"));
    CHECK(!le_tidal_valid_id(LE_TIDAL_TRACK, ""));
    CHECK(!le_tidal_valid_id(LE_TIDAL_ALBUM, "../1"));
    CHECK(le_tidal_valid_id(LE_TIDAL_PLAYLIST,
                            "abd35446-6a67-4f2b-9f92-0636742c63cb"));
    CHECK(!le_tidal_valid_id(LE_TIDAL_PLAYLIST, "abd/../x"));
}

static int log_contains(const char *needle)
{
    char path[1024], *log;
    int found;

    snprintf(path, sizeof(path), "%s/fake/requests.log", scratch);
    log = slurp(path, NULL);
    found = log && strstr(log, needle) != NULL;
    free(log);
    return found;
}

/* No access, refresh or device token may ever appear in a curl argv. */
static int argv_clean(void)
{
    char path[1024], *log, *line, *save = NULL;
    int clean = 1;

    snprintf(path, sizeof(path), "%s/fake/requests.log", scratch);
    log = slurp(path, NULL);
    if (!log)
        return 0;
    for (line = strtok_r(log, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *argv = strstr(line, "\"argv\"");
        char *end = argv ? strstr(argv, "]") : NULL;

        if (!argv || !end)
            continue;
        *end = '\0';
        if (strstr(argv, "access-") || strstr(argv, "refresh-") ||
            strstr(argv, "device-secret") || strstr(argv, "Bearer"))
            clean = 0;
    }
    free(log);
    return clean;
}

static void test_flow(void)
{
    struct le_tidal_paths paths;
    struct le_tidal_login login;
    struct le_tidal_status status;
    struct le_tidal_results results;
    struct le_tidal_item meta;
    char path[1024], fake[1024], url[LE_TIDAL_URL_MAX], *text;
    char ids[LE_TIDAL_QUEUE_MAX][LE_TIDAL_ID_MAX];
    char back[LE_TIDAL_QUEUE_MAX][LE_TIDAL_ID_MAX];
    size_t count = 0, back_count = 0;
    struct stat st;

    snprintf(fake, sizeof(fake), "%s/fake", scratch);
    mkdir(fake, 0700);
    setenv("FAKE_TIDAL_DIR", fake, 1);
    snprintf(path, sizeof(path), "%s/tests/fake_tidal_curl.py", repo);
    setenv("LIBREECHO_TIDAL_CURL", path, 1);
    snprintf(path, sizeof(path), "%s/tests/fixtures/tidal/search.json", repo);
    setenv("LIBREECHO_TIDAL_CA", path, 1);      /* any readable file */
    snprintf(path, sizeof(path), "%s/secrets/tidal.json", scratch);
    setenv("LIBREECHO_TIDAL_SESSION", path, 1);
    snprintf(path, sizeof(path), "%s/run/tidal-login.json", scratch);
    setenv("LIBREECHO_TIDAL_LOGIN", path, 1);
    snprintf(path, sizeof(path), "%s/run/tidal-queue", scratch);
    setenv("LIBREECHO_TIDAL_QUEUE", path, 1);
    le_tidal_default_paths(&paths);
    CHECK(le_tidal_available(&paths));

    CHECK(le_tidal_status(&paths, &status) == LE_TIDAL_OK);
    CHECK(!status.signed_in && !status.login_pending);
    CHECK(le_tidal_search(&paths, "daft punk", &results) ==
          LE_TIDAL_SIGNED_OUT);
    CHECK(le_tidal_login_poll(&paths) == LE_TIDAL_EXPIRED);

    /* Offline: a clear network failure, and nothing written. */
    setenv("FAKE_TIDAL_OFFLINE", "1", 1);
    CHECK(le_tidal_login_start(&paths, &login) == LE_TIDAL_NETWORK);
    CHECK(stat(paths.login, &st) < 0);
    unsetenv("FAKE_TIDAL_OFFLINE");

    CHECK(le_tidal_login_start(&paths, &login) == LE_TIDAL_OK);
    CHECK(!strcmp(login.user_code, "ABCDE"));
    CHECK(!strcmp(login.link, "https://link.tidal.com/ABCDE"));
    CHECK(login.interval == 2);
    CHECK(stat(paths.login, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(le_tidal_status(&paths, &status) == LE_TIDAL_OK);
    CHECK(status.login_pending && !strcmp(status.login.user_code, "ABCDE"));

    CHECK(le_tidal_login_poll(&paths) == LE_TIDAL_PENDING);
    CHECK(le_tidal_login_poll(&paths) == LE_TIDAL_OK);
    CHECK(stat(paths.login, &st) < 0);               /* code consumed */
    CHECK(stat(paths.session, &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(le_tidal_status(&paths, &status) == LE_TIDAL_OK);
    CHECK(status.signed_in && !status.login_pending);
    CHECK(!strcmp(status.country, "GB"));
    CHECK(!strcmp(status.username, "owner@example.invalid"));
    CHECK(log_contains("\"basic\": true"));

    CHECK(le_tidal_search(&paths, "daft punk", &results) == LE_TIDAL_OK);
    CHECK(results.track_count == 3 && results.album_count == 3);
    CHECK(log_contains("query=daft%20punk"));
    CHECK(le_tidal_search(&paths, "", &results) == LE_TIDAL_INVALID);

    CHECK(le_tidal_track_ids(&paths, LE_TIDAL_ALBUM, "20115556", ids,
                             LE_TIDAL_QUEUE_MAX, &count) == LE_TIDAL_OK);
    CHECK(count == 13);
    CHECK(le_tidal_track_ids(&paths, LE_TIDAL_PLAYLIST,
                             "abd35446-6a67-4f2b-9f92-0636742c63cb", ids,
                             LE_TIDAL_QUEUE_MAX, &count) == LE_TIDAL_OK);
    CHECK(count == 4);
    CHECK(le_tidal_queue_write(&paths, ids, count) == LE_TIDAL_OK);
    CHECK(le_tidal_queue_read(&paths, back, LE_TIDAL_QUEUE_MAX,
                              &back_count) == LE_TIDAL_OK);
    CHECK(back_count == count && !strcmp(back[0], ids[0]));
    CHECK(le_tidal_queue_write(&paths, ids, 0) == LE_TIDAL_INVALID);
    CHECK(le_tidal_track_ids(&paths, LE_TIDAL_ALBUM, "1/../2", ids,
                             LE_TIDAL_QUEUE_MAX, &count) == LE_TIDAL_INVALID);

    CHECK(le_tidal_stream(&paths, "1550546", url, sizeof(url), &meta) ==
          LE_TIDAL_OK);
    CHECK(!strcmp(url, "https://media.example.invalid/track.flac?token=fixture"));
    CHECK(!strcmp(meta.title, "One More Time"));
    CHECK(log_contains("audioquality=LOSSLESS"));

    /* A rejected access token is refreshed once and the call retried. */
    setenv("FAKE_TIDAL_FIRST_TOKEN_STALE", "1", 1);
    CHECK(le_tidal_search(&paths, "daft punk", &results) == LE_TIDAL_OK);
    unsetenv("FAKE_TIDAL_FIRST_TOKEN_STALE");
    text = slurp(paths.session, NULL);
    CHECK(text && strstr(text, "\"access_token\":\"access-2\""));
    CHECK(text && strstr(text, "\"refresh_token\":\"refresh-1\""));
    free(text);

    /* Revoked: the session is dropped so the UI offers sign-in again. */
    setenv("FAKE_TIDAL_REVOKED", "1", 1);
    setenv("FAKE_TIDAL_FIRST_TOKEN_STALE", "1", 1);
    {
        /* Force the stored token to look stale. */
        char doc[256];
        FILE *f;

        snprintf(doc, sizeof(doc),
                 "{\"schema\":1,\"access_token\":\"access-1\","
                 "\"refresh_token\":\"refresh-1\",\"expires_at\":1,"
                 "\"country\":\"GB\",\"username\":\"u\"}\n");
        f = fopen(paths.session, "w");
        if (f) {
            fputs(doc, f);
            fclose(f);
        }
    }
    CHECK(le_tidal_search(&paths, "daft punk", &results) ==
          LE_TIDAL_SIGNED_OUT);
    CHECK(stat(paths.session, &st) < 0);
    unsetenv("FAKE_TIDAL_REVOKED");
    unsetenv("FAKE_TIDAL_FIRST_TOKEN_STALE");

    /* A refused code ends the login and clears it. */
    CHECK(le_tidal_login_start(&paths, &login) == LE_TIDAL_OK);
    setenv("FAKE_TIDAL_DENY", "1", 1);
    CHECK(le_tidal_login_poll(&paths) == LE_TIDAL_EXPIRED);
    CHECK(stat(paths.login, &st) < 0);
    unsetenv("FAKE_TIDAL_DENY");

    CHECK(le_tidal_logout(&paths) == LE_TIDAL_OK);
    CHECK(le_tidal_status(&paths, &status) == LE_TIDAL_OK &&
          !status.signed_in);
    CHECK(argv_clean());
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <repo-root> <scratch-dir>\n", argv[0]);
        return 2;
    }
    snprintf(repo, sizeof(repo), "%s", argv[1]);
    snprintf(scratch, sizeof(scratch), "%s", argv[2]);
    test_parsers();
    test_flow();
    if (failures) {
        fprintf(stderr, "tidal: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tidal: ok");
    return 0;
}
