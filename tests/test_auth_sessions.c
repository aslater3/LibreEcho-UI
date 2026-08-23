#include "auth.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

int main(void)
{
    char users[128], sessions[128], token[LE_AUTH_TOKEN_MAX];
    struct le_auth_db db, loaded;
    int expires;
    FILE *file;

    snprintf(users, sizeof(users), "/tmp/libreecho-auth-users-%ld", (long)getpid());
    snprintf(sessions, sizeof(sessions), "/tmp/libreecho-auth-sessions-%ld", (long)getpid());
    unlink(users);
    unlink(sessions);
    memset(&db, 0, sizeof(db));
    CHECK(le_auth_add_user(&db, users, "Alice", "alice-password-123") == 0);
    CHECK(le_auth_add_user(&db, users, "Bob", "bob-password-123") == 0);
    CHECK(le_auth_login(&db, "ALICE", "alice-password-123", token,
                        sizeof(token), &expires) == 0);
    CHECK(expires > 0);
    CHECK(le_auth_save_sessions(&db, sessions) == 0);
    CHECK(le_auth_remove_user(&db, users, "ALICE") == 0);
    CHECK(le_auth_save_sessions(&db, sessions) == 0);

    memset(&loaded, 0, sizeof(loaded));
    CHECK(le_auth_load(&loaded, users) == 0);
    le_auth_load_sessions(&loaded, sessions);
    CHECK(!le_auth_session(&loaded, token, NULL, 0));

    /* A stale record for a deleted account must also be rejected on load. */
    file = fopen(sessions, "w");
    CHECK(file != NULL);
    CHECK(fprintf(file, "%s alice %lld\n", token,
                  (long long)time(NULL) + 3600) > 0);
    CHECK(fclose(file) == 0);
    memset(&loaded.sessions, 0, sizeof(loaded.sessions));
    le_auth_load_sessions(&loaded, sessions);
    CHECK(!le_auth_session(&loaded, token, NULL, 0));

    unlink(users);
    unlink(sessions);
    puts("auth session deletion persistence: ok");
    return 0;
}
