
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
        if (!strcmp(db->sessions[i].username, username))
            db->sessions[i].token[0] = '\0';
    return 0;
}
