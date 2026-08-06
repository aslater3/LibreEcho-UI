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
};

struct le_auth_db {
    int enabled;
    size_t user_count;
    struct le_auth_user users[LE_AUTH_MAX_USERS];
    struct le_auth_session sessions[LE_AUTH_MAX_SESSIONS];
};

int le_auth_load(struct le_auth_db *db, const char *path);
int le_auth_login(struct le_auth_db *db, const char *username,
                  const char *password, char *token, size_t token_size,
                  int *expires_in);
int le_auth_session(struct le_auth_db *db, const char *token,
                    char *username, size_t username_size);
void le_auth_logout(struct le_auth_db *db, const char *token);
int le_auth_add_user(struct le_auth_db *db, const char *path,
                     const char *username, const char *password);
int le_auth_remove_user(struct le_auth_db *db, const char *path,
                        const char *username);

#endif
