#ifndef LE_AUTH_H
#define LE_AUTH_H

#include <stddef.h>
#include <time.h>

#define LE_AUTH_MAX_USERS 8
#define LE_AUTH_MAX_SESSIONS 8
#define LE_AUTH_USERNAME_MAX 32
#define LE_AUTH_TOKEN_MAX 65
#define LE_AUTH_PASSWORD_MAX 128

struct le_auth_user {
    char username[LE_AUTH_USERNAME_MAX];
    char salt[65];
    char digest[65];
};

struct le_auth_session {
    char token[LE_AUTH_TOKEN_MAX];
    char username[LE_AUTH_USERNAME_MAX];
    time_t expires;
    int persisted;
};

struct le_auth_db {
    int enabled;
    size_t user_count;
    struct le_auth_user users[LE_AUTH_MAX_USERS];
    struct le_auth_session sessions[LE_AUTH_MAX_SESSIONS];
};

/*
 * Usernames are case-insensitive. Every entry point folds the supplied name to
 * lowercase and stored names are folded on load, so "Lucas" and "lucas" are the
 * same account and only the lowercase form is ever written or reported back.
 */
void le_auth_fold_username(char *out, size_t size, const char *in);

int le_auth_load(struct le_auth_db *db, const char *path);
int le_auth_login(struct le_auth_db *db, const char *username,
                  const char *password, char *token, size_t token_size,
                  int *expires_in);
int le_auth_session(struct le_auth_db *db, const char *token,
                    char *username, size_t username_size);
void le_auth_logout(struct le_auth_db *db, const char *token);

/*
 * Sessions live only in memory, so every restart of the web daemon --
 * including every reboot and every OTA -- invalidated them and forced a
 * fresh sign-in.  Persist the table so a signed-in browser stays signed in
 * across a restart, within the same twelve-hour lifetime it always had.
 *
 * Expired entries are dropped on load, so the file cannot extend a session
 * beyond its original expiry, and a corrupt or unreadable file simply
 * yields no sessions rather than failing startup.
 */
int le_auth_save_sessions(const struct le_auth_db *db, const char *path);
int le_auth_save_persisted_sessions(const struct le_auth_db *db,
                                    const char *path);
int le_auth_save_issued_session(struct le_auth_db *db, const char *path,
                                const char *token);
void le_auth_load_sessions(struct le_auth_db *db, const char *path);
int le_auth_add_user(struct le_auth_db *db, const char *path,
                     const char *username, const char *password);
int le_auth_remove_user(struct le_auth_db *db, const char *path,
                        const char *username);

#endif
