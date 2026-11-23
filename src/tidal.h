/*
 * TIDAL client: device-code sign-in, catalogue search and stream lookup.
 *
 * Sign-in is the OAuth 2.0 device authorisation grant (RFC 8628) that TIDAL's
 * own TV and desktop apps use: the device asks for a short code, the owner
 * opens link.tidal.com on any phone or computer, signs in and approves the
 * code, and the device collects the tokens. No TIDAL password ever reaches the
 * speaker.
 *
 * Every request goes through the image's curl with certificate verification
 * against the system CA bundle. Tokens travel to curl on a pipe, never on the
 * command line, and are stored in a 0600 file under the secrets tree.
 *
 * Playback asks for LOSSLESS, which TIDAL serves as a single unencrypted FLAC
 * file behind a signed URL (valid for about an hour). Anything else -- DASH
 * manifests, encrypted streams, AAC in MP4 -- is refused rather than guessed
 * at; radiod decodes the FLAC.
 *
 * The network calls block. Callers run them in a worker process (the web
 * server's API worker, or radiod's player child), never in an event loop.
 */
#ifndef LIBREECHO_TIDAL_H
#define LIBREECHO_TIDAL_H

#include <stddef.h>

#define LE_TIDAL_ID_MAX 48
#define LE_TIDAL_TEXT_MAX 192
#define LE_TIDAL_URL_MAX 1024
#define LE_TIDAL_QUEUE_MAX 200
#define LE_TIDAL_RESULTS_MAX 10

/* Return codes. Negative values are failures. */
enum {
    LE_TIDAL_OK = 0,
    LE_TIDAL_PENDING = 1,        /* login poll: owner has not approved yet */
    LE_TIDAL_ERR = -1,           /* local failure (file, memory, curl) */
    LE_TIDAL_NETWORK = -2,       /* TIDAL unreachable or answered nonsense */
    LE_TIDAL_SIGNED_OUT = -3,    /* no session, or TIDAL revoked it */
    LE_TIDAL_EXPIRED = -4,       /* login code expired or was refused */
    LE_TIDAL_UNSUPPORTED = -5,   /* stream is not a plain FLAC file */
    LE_TIDAL_NOT_FOUND = -6,
    LE_TIDAL_INVALID = -7        /* bad caller input */
};

struct le_tidal_paths {
    char curl[256];
    char ca[384];
    char session[256];           /* tokens, 0600 */
    char login[256];             /* pending device code, 0600 */
    char queue[256];             /* track ids handed to radiod */
    char auth_base[128];         /* https://auth.tidal.com/v1/oauth2 */
    char api_base[128];          /* https://api.tidal.com/v1 */
};

/* Fill defaults, then apply LIBREECHO_TIDAL_* environment overrides (used by
   the tests to point at a fake curl and a scratch directory). */
void le_tidal_default_paths(struct le_tidal_paths *paths);

/* curl and a CA bundle are present on this image. */
int le_tidal_available(const struct le_tidal_paths *paths);

struct le_tidal_login {
    char user_code[32];
    char link[128];              /* https://link.tidal.com/XXXXX */
    long long expires_at;        /* unix seconds */
    int interval;                /* seconds between polls */
};

struct le_tidal_status {
    int signed_in;
    int login_pending;
    struct le_tidal_login login;
    char country[8];
    char username[LE_TIDAL_TEXT_MAX];
};

/* Local state only: never touches the network. */
int le_tidal_status(const struct le_tidal_paths *paths,
                    struct le_tidal_status *out);

/* Ask TIDAL for a device code and remember it. */
int le_tidal_login_start(const struct le_tidal_paths *paths,
                         struct le_tidal_login *out);

/* One token request for the remembered code. LE_TIDAL_OK once approved (the
   session is saved), LE_TIDAL_PENDING while waiting, LE_TIDAL_EXPIRED when the
   code timed out or was refused. */
int le_tidal_login_poll(const struct le_tidal_paths *paths);

/* Forget the session and any pending code. */
int le_tidal_logout(const struct le_tidal_paths *paths);

enum le_tidal_kind { LE_TIDAL_TRACK, LE_TIDAL_ALBUM, LE_TIDAL_PLAYLIST };

struct le_tidal_item {
    enum le_tidal_kind kind;
    char id[LE_TIDAL_ID_MAX];
    char title[LE_TIDAL_TEXT_MAX];
    char artist[LE_TIDAL_TEXT_MAX];
    char album[LE_TIDAL_TEXT_MAX];
    char image[64];              /* resources.tidal.com image id */
    int duration;                /* seconds, tracks only */
    int tracks;                  /* albums and playlists */
};

struct le_tidal_results {
    struct le_tidal_item tracks[LE_TIDAL_RESULTS_MAX];
    struct le_tidal_item albums[LE_TIDAL_RESULTS_MAX];
    struct le_tidal_item playlists[LE_TIDAL_RESULTS_MAX];
    size_t track_count, album_count, playlist_count;
};

int le_tidal_search(const struct le_tidal_paths *paths, const char *query,
                    struct le_tidal_results *out);

/* Track ids to play for a track, album or playlist, in order. */
int le_tidal_track_ids(const struct le_tidal_paths *paths,
                       enum le_tidal_kind kind, const char *id,
                       char ids[][LE_TIDAL_ID_MAX], size_t capacity,
                       size_t *count);

/* Signed FLAC URL plus display metadata for one track. */
int le_tidal_stream(const struct le_tidal_paths *paths, const char *track_id,
                    char *url, size_t url_size, struct le_tidal_item *meta);

/*
 * Stream a signed media URL through curl from byte `offset`, with TLS
 * verified against the CA bundle. Returns a read fd and the curl pid; read to
 * EOF, then le_tidal_media_close (at_eof = 1) says whether the whole body
 * arrived (LE_TIDAL_OK) or the transfer broke and should resume from the
 * bytes already read (LE_TIDAL_NETWORK). at_eof = 0 abandons a transfer.
 */
int le_tidal_media_open(const struct le_tidal_paths *paths, const char *url,
                        long long offset, int *fd, int *pid);
int le_tidal_media_close(int fd, int pid, int at_eof);

/* The play queue radiod reads for the URL "tidal:queue". */
int le_tidal_queue_write(const struct le_tidal_paths *paths,
                         char ids[][LE_TIDAL_ID_MAX], size_t count);
int le_tidal_queue_read(const struct le_tidal_paths *paths,
                        char ids[][LE_TIDAL_ID_MAX], size_t capacity,
                        size_t *count);

#define LE_TIDAL_QUEUE_URL "tidal:queue"

/* ---- parsers, exposed for the host tests ---- */
int le_tidal_parse_search(const char *json, size_t len,
                          struct le_tidal_results *out);
int le_tidal_parse_track(const char *json, size_t len,
                         struct le_tidal_item *out);
int le_tidal_parse_track_list(const char *json, size_t len,
                              char ids[][LE_TIDAL_ID_MAX], size_t capacity,
                              size_t *count);
/* playbackinfo document -> FLAC URL, or LE_TIDAL_UNSUPPORTED. */
int le_tidal_parse_playback(const char *json, size_t len, char *url,
                            size_t url_size);
int le_tidal_valid_id(enum le_tidal_kind kind, const char *id);

#endif
