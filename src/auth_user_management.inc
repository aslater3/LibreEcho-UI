
static int valid_username(const char *username)
{
    size_t i, length;
    if (!username)
        return 0;
    length = strlen(username);
    if (!length || length >= LE_AUTH_USERNAME_MAX)
        return 0;
    for (i = 0; i < length; ++i) {
        char c = username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static void random_hex(char *out, size_t bytes)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[32];
    size_t i;
    if (bytes > sizeof(random) || random_bytes(random, bytes) < 0) {
        out[0] = '\0';
        return;
    }
    for (i = 0; i < bytes; ++i) {
        out[i * 2] = hex[random[i] >> 4];
        out[i * 2 + 1] = hex[random[i] & 15];
    }
    out[bytes * 2] = '\0';
}

static int write_users_file(const char *path, const struct le_auth_user *users,
                            size_t count)
{
    char temporary[512];
    FILE *file;
    int fd;
    size_t i;
    if (!path || !path[0] || !users || !count || count > LE_AUTH_MAX_USERS ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", path) >=
            (int)sizeof(temporary))
        return -1;
    unlink(temporary);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    file = fdopen(fd, "w");
    if (!file) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (fprintf(file, "%s:sha256:%s:%s\n", users[i].username,
                    users[i].salt, users[i].digest) < 0) {
            fclose(file);
            unlink(temporary);
            return -1;
        }
    }
    if (fflush(file) || fsync(fileno(file)) || fchmod(fileno(file), 0600)) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    if (fclose(file) || rename(temporary, path)) {
        unlink(temporary);
        return -1;
    }
    chmod(path, 0600);
    return 0;
}

int le_auth_add_user(struct le_auth_db *db, const char *path,
                     const char *username, const char *password)
{
    struct le_auth_user users[LE_AUTH_MAX_USERS];
    char salt[65], digest[65], folded[LE_AUTH_USERNAME_MAX];
    size_t i, count;
    if (!db || !path || !path[0] || !valid_username(username) || !password ||
        strlen(password) < 8 || strlen(password) > LE_AUTH_PASSWORD_MAX ||
        db->user_count >= LE_AUTH_MAX_USERS)
        return -1;
    /* Store the folded name so the file only ever holds lowercase, and so a
       second account differing only in case cannot be created. */
    le_auth_fold_username(folded, sizeof(folded), username);
    for (i = 0; i < db->user_count; ++i)
        if (!strcmp(db->users[i].username, folded))
            return -1;
    count = db->user_count;
    memcpy(users, db->users, count * sizeof(users[0]));
    random_hex(salt, 32);
    if (!salt[0])
        return -1;
    hash_password(salt, password, digest);
    memset(&users[count], 0, sizeof(users[count]));
    strncpy(users[count].username, folded, sizeof(users[count].username) - 1);
    strcpy(users[count].salt, salt);
    strcpy(users[count].digest, digest);
    if (write_users_file(path, users, count + 1))
        return -1;
    memcpy(db->users, users, (count + 1) * sizeof(users[0]));
    db->user_count = count + 1;
    db->enabled = 1;
    return 0;
}

/*
 * Is this one users-file line, in the canonical form?
 *
 * The scheme is the same salted SHA-256 the loader and every writer here use:
 * a username that valid_username() accepts, the sha256 method, a salt of 16 to
 * 64 hex characters, and a 64-character hex digest. Those are the loader's
 * rules, restated in one place so a line can be checked before it is adopted
 * rather than only while it is being written.
 *
 * A trailing newline is how the file stores a line, so it is accepted and
 * ignored; anything else beyond it means this is not one line.
 */
int le_auth_users_line_valid(const char *line)
{
    char copy[LE_AUTH_USERS_LINE_MAX];
    char *user, *method, *salt, *digest, *end;

    if (!line || !line[0] || strlen(line) >= sizeof(copy))
        return 0;
    snprintf(copy, sizeof(copy), "%s", line);
    end = copy + strlen(copy);
    while (end > copy && (end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    user = copy;
    if ((end = strchr(user, ':')) == NULL)
        return 0;
    *end = '\0';
    method = end + 1;
    if ((end = strchr(method, ':')) == NULL)
        return 0;
    *end = '\0';
    salt = end + 1;
    if ((end = strchr(salt, ':')) == NULL)
        return 0;
    *end = '\0';
    digest = end + 1;
    if (strchr(digest, ':'))
        return 0;
    if (!valid_username(user) || strcmp(method, "sha256") ||
        strlen(salt) < 16 || strlen(salt) > 64 ||
        !valid_hex(salt, strlen(salt)) || strlen(digest) != 64 ||
        !valid_hex(digest, 64))
        return 0;
    /* Stored names are folded, so the digest was derived over the folded name.
       An uppercase name here would be a line this daemon never wrote. */
    if (strspn(user, "abcdefghijklmnopqrstuvwxyz0123456789._-") != strlen(user))
        return 0;
    memset(copy, 0, sizeof(copy));
    return 1;
}

/*
 * Adopt an already-derived users-file line.
 *
 * The installer's provisioning file carries the account as one line in exactly
 * the format le_auth_add_user() writes -- username, method, salt and digest --
 * because the digest is salted SHA-256 of the passphrase and there is no second
 * derivation to invent here. The line is checked by the same validator the
 * loader's rules come from and written by the same atomic 0600 writer, so a
 * delivered account is indistinguishable from one created here.
 */
int le_auth_add_users_line(struct le_auth_db *db, const char *path,
                           const char *line)
{
    struct le_auth_user users[LE_AUTH_MAX_USERS];
    char copy[LE_AUTH_USERS_LINE_MAX];
    char *user, *method, *salt, *digest, *end;
    char folded[LE_AUTH_USERNAME_MAX];
    size_t i, count;

    if (!db || !path || !path[0] || !le_auth_users_line_valid(line))
        return -1;
    /* Validated, so every separator is present and this walk is bounded. */
    snprintf(copy, sizeof(copy), "%s", line);
    end = copy + strlen(copy);
    while (end > copy && (end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    user = copy;
    end = strchr(user, ':');
    *end = '\0';
    method = end + 1;
    end = strchr(method, ':');
    *end = '\0';
    salt = end + 1;
    end = strchr(salt, ':');
    *end = '\0';
    digest = end + 1;
    le_auth_fold_username(folded, sizeof(folded), user);
    count = db->user_count;
    if (count >= LE_AUTH_MAX_USERS)
        return -1;
    for (i = 0; i < count; ++i)
        if (!strcmp(db->users[i].username, folded))
            return -1;
    memcpy(users, db->users, count * sizeof(users[0]));
    memset(&users[count], 0, sizeof(users[count]));
    snprintf(users[count].username, sizeof(users[count].username), "%s", folded);
    snprintf(users[count].salt, sizeof(users[count].salt), "%s", salt);
    snprintf(users[count].digest, sizeof(users[count].digest), "%s", digest);
    if (write_users_file(path, users, count + 1))
        return -1;
    memcpy(db->users, users, (count + 1) * sizeof(users[0]));
    db->user_count = count + 1;
    db->enabled = 1;
    memset(copy, 0, sizeof(copy));
    return 0;
}

int le_auth_remove_user(struct le_auth_db *db, const char *path,
                        const char *username)
{
    struct le_auth_user users[LE_AUTH_MAX_USERS];
    char folded[LE_AUTH_USERNAME_MAX];
    size_t i, found = LE_AUTH_MAX_USERS, count = 0;
    if (!db || !path || !path[0] || !valid_username(username) ||
        db->user_count <= 1)
        return -1;
    le_auth_fold_username(folded, sizeof(folded), username);
    for (i = 0; i < db->user_count; ++i) {
        if (!strcmp(db->users[i].username, folded))
            found = i;
        else
            users[count++] = db->users[i];
    }
    if (found == LE_AUTH_MAX_USERS)
        return -1;
    if (write_users_file(path, users, count))
        return -1;
    memset(db->users, 0, sizeof(db->users));
    memcpy(db->users, users, count * sizeof(users[0]));
    db->user_count = count;
    db->enabled = count > 0;
    for (i = 0; i < LE_AUTH_MAX_SESSIONS; ++i)
        if (!strcmp(db->sessions[i].username, folded))
            db->sessions[i].token[0] = '\0';
    return 0;
}
