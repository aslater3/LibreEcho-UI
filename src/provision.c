/*
 * First-boot application of the installer's one-shot provision file.
 *
 * The web installer collects the setup page's fields on the host and writes
 * <config dir>/provision.json during the install. On the first boot after that
 * install this daemon applies it through the same validators, writers and
 * adapter calls the on-device wizard uses: there is no second configuration
 * system here, no second hashing scheme, and no hand-written
 * wpa_supplicant.conf.
 *
 * The transaction, and the reasons for its shape:
 *
 *  - The file is read O_NOFOLLOW, size- and mode-checked, and unlinked before
 *    anything else happens, whether or not it validates. It carries a
 *    plaintext Wi-Fi passphrase, so it must not sit on the partition after
 *    having been read, even if the daemon dies immediately afterwards.
 *  - The whole document is validated before anything is applied, so a bad
 *    document leaves the device exactly as it was.
 *  - The account and the settings are applied by this process, before the
 *    listener opens, and the Wi-Fi hand-off is issued by this process too. It
 *    has to be: the setup-complete marker is a claim about an association, and
 *    only the process that issued the hand-off can later observe that the
 *    backend actually joined the network and took an address. A forked child
 *    would hold the backend's network state to itself and leave this process
 *    unable to tell a working device from an idle one.
 *  - Only the wait is deferred. The setup-complete marker is written later,
 *    from the HTTP server's one-second tick, and only once the backend reports
 *    an association with the provisioned SSID and an address. Completion is
 *    therefore made against observed state rather than against the hand-off
 *    succeeding, and the web UI is up throughout the wait.
 *
 * Running before the listener is deliberate and matches what start-up already
 * does: the persisted-settings restore loop below it makes the same adapter
 * calls before the listener and bounds them. The Wi-Fi call is bounded too --
 * networkd answers it from its own association deadline -- and it happens once,
 * on the first boot after an install.
 */
#include "api.h"
#include "config_store.h"
#include "json.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LE_PROVISION_SCHEMA "libreecho-provision/1"
#define LE_PROVISION_FILE "provision.json"
#define LE_PROVISION_RESULT_FILE "provision.result"
#define LE_PROVISION_MAX_BYTES 4096
/* The contract bounds the delivered SSID and passphrase tighter than the
   setup endpoint does; anything longer was not produced by this installer. */
#define LE_PROVISION_SSID_MAX 32
#define LE_PROVISION_PASSWORD_MIN 8
#define LE_PROVISION_PASSWORD_MAX 63
#define LE_PROVISION_BINDING_VALUE_MAX 64
/* Ceiling on the association window. networkd returns from the connect
   command long before this; the timeout only decides how long the setup
   wizard stays pending when the access point never comes up. */
#define LE_PROVISION_ASSOC_TIMEOUT_DEFAULT 180
#define LE_PROVISION_ASSOC_TIMEOUT_MAX 3600
/* A bounded window over member names. The deepest object here is settings,
   which has six, so this is generous rather than tight. */
#define LE_PROVISION_MAX_MEMBERS 16

struct provision_doc {
    char users_line[192];
    int has_admin;
    int has_wifi;
    char ssid[LE_TEXT];
    char security[16];
    char password[128];
    int has_settings;
    char hostname[LE_TEXT];
    int has_hostname;
    int volume, has_volume;
    char wake_word[LE_TEXT];
    int has_wake_word;
    int sensitivity, has_sensitivity;
    int local_only, has_local_only;
    int telemetry, has_telemetry;
};

/* result, then the five contract fields, in the contract's order. */
struct provision_status {
    const char *result;
    const char *error;
    const char *admin;
    const char *wifi;
    const char *settings;
};

static const struct provision_status provision_applied = {
    "applied", "none", "created", "handed-off", "applied"
};
static const struct provision_status provision_skipped = {
    "rejected", "none", "skipped", "absent", "absent"
};

/* A sibling of the daemon's own config file, which is how the device finds
   both files: one directory, both names derived from the same place. */
static int provision_dir_path(const char *config_path, const char *name,
                              char *out, size_t size)
{
    const char *slash;
    int n;

    if (!config_path || !config_path[0] || !name || !out || size < 2)
        return -1;
    slash = strrchr(config_path, '/');
    if (!slash)
        return snprintf(out, size, "%s", name) < (int)size ? 0 : -1;
    if ((size_t)(slash - config_path) + 1 + strlen(name) + 1 > size)
        return -1;
    n = snprintf(out, size, "%.*s/%s", (int)(slash - config_path), config_path,
                 name);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

/*
 * The result document is written through the canonical atomic 0600 writer, and
 * it holds codes only: no member value from the document reaches it, so there
 * is nothing in it for a later reader to leak.
 */
static int write_status(const char *config_path,
                        const struct provision_status *status)
{
    char path[512], text[256];
    int n = snprintf(path, sizeof(path), "%s", LE_PROVISION_RESULT_FILE);

    if (n < 0 || (size_t)n >= sizeof(path) ||
        provision_dir_path(config_path, LE_PROVISION_RESULT_FILE, path,
                           sizeof(path)))
        return -1;
    n = snprintf(text, sizeof(text),
                 "result=%s\nerror=%s\nadmin=%s\nwifi=%s\nsettings=%s\n",
                 status->result, status->error, status->admin, status->wifi,
                 status->settings);
    if (n < 0 || (size_t)n >= sizeof(text))
        return -1;
    return config_write_atomic(path, text, (size_t)n);
}

/*
 * Reject any member that is not on the allow list, and any member that
 * appears twice. A duplicate matters as much as an unknown key: the lookups
 * below take the first occurrence, so which value would have been validated
 * would otherwise depend on member order.
 */
static int members_known(const char *text, size_t len,
                         const char *const *allowed, size_t allowed_count,
                         const char **code)
{
    struct json_member members[LE_PROVISION_MAX_MEMBERS];
    unsigned char seen[LE_PROVISION_MAX_MEMBERS];
    size_t count = 0, i, j;

    if (json_object_members(text, len, members, LE_PROVISION_MAX_MEMBERS, &count))
        return 0;
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < count; ++i) {
        for (j = 0; j < allowed_count; ++j) {
            size_t name_len = strlen(allowed[j]);
            if (name_len != members[i].name_len ||
                memcmp(members[i].name, allowed[j], name_len))
                continue;
            if (seen[j]) {
                *code = "duplicate-key";
                return 0;
            }
            seen[j] = 1;
            break;
        }
        if (j == allowed_count) {
            *code = "unknown-key";
            return 0;
        }
    }
    return 1;
}

static const struct json_member *member_find(const struct json_member *members,
                                             size_t count, const char *name)
{
    size_t i, name_len = strlen(name);

    for (i = 0; i < count; ++i)
        if (members[i].name_len == name_len &&
            !memcmp(members[i].name, name, name_len))
            return &members[i];
    return NULL;
}

static int member_string(const struct json_member *members, size_t count,
                         const char *name, char *out, size_t size,
                         const char **code)
{
    const struct json_member *member = member_find(members, count, name);

    if (!member)
        return 0;
    if (json_string_span(member->value, member->value_len, out, size) != 1) {
        *code = name;
        return -1;
    }
    return 1;
}

static int member_int(const struct json_member *members, size_t count,
                      const char *name, int *out, int low, int high,
                      const char **code)
{
    const struct json_member *member = member_find(members, count, name);

    if (!member)
        return 0;
    if (json_int_span(member->value, member->value_len, out) != 1 ||
        *out < low || *out > high) {
        *code = name;
        return -1;
    }
    return 1;
}

static int member_bool(const struct json_member *members, size_t count,
                       const char *name, int *out, const char **code)
{
    const struct json_member *member = member_find(members, count, name);

    if (!member)
        return 0;
    if (json_bool_span(member->value, member->value_len, out) != 1) {
        *code = name;
        return -1;
    }
    return 1;
}

static int parse_admin(const struct json_member *members, size_t count,
                       struct provision_doc *doc, const char **code)
{
    static const char *const allowed[] = { "users_line" };
    struct json_member inner[LE_PROVISION_MAX_MEMBERS];
    const struct json_member *admin = member_find(members, count, "admin");
    size_t inner_count = 0;
    int rc;

    /* admin is required: without it there is no account to create. */
    if (!admin) {
        *code = "admin-missing";
        return 0;
    }
    if (admin->value_len < 2 || admin->value[0] != '{' ||
        !members_known(admin->value, admin->value_len, allowed, 1, code) ||
        json_object_members(admin->value, admin->value_len, inner,
                            LE_PROVISION_MAX_MEMBERS, &inner_count)) {
        *code = *code ? *code : "admin-shape";
        return 0;
    }
    rc = member_string(inner, inner_count, "users_line", doc->users_line,
                       sizeof(doc->users_line), code);
    /* The line is checked here, not when it is adopted: the contract applies
       nothing on any validation failure, and the account is applied first, so
       a line that would fail in the writer would otherwise let the settings
       land before the document was found wanting. The rule is the loader's,
       in one place. */
    if (rc != 1 || !le_auth_users_line_valid(doc->users_line)) {
        *code = "admin-users-line";
        return 0;
    }
    doc->has_admin = 1;
    return 1;
}

static int parse_wifi(const struct json_member *members, size_t count,
                      struct provision_doc *doc, const char **code)
{
    static const char *const allowed[] = { "ssid", "security", "password" };
    struct json_member inner[LE_PROVISION_MAX_MEMBERS];
    const struct json_member *wifi = member_find(members, count, "wifi");
    size_t inner_count = 0, password_len;
    int rc;

    /* wifi is optional; its absence keeps the normal setup flow. */
    if (!wifi)
        return 1;
    if (wifi->value_len < 2 || wifi->value[0] != '{' ||
        !members_known(wifi->value, wifi->value_len, allowed,
                       sizeof(allowed) / sizeof(allowed[0]), code) ||
        json_object_members(wifi->value, wifi->value_len, inner,
                            LE_PROVISION_MAX_MEMBERS, &inner_count)) {
        *code = *code ? *code : "wifi-shape";
        return 0;
    }
    rc = member_string(inner, inner_count, "ssid", doc->ssid, sizeof(doc->ssid),
                       code);
    if (rc < 0) {
        *code = "wifi-ssid";
        goto fail;
    }
    if (!rc || !doc->ssid[0] || strlen(doc->ssid) > LE_PROVISION_SSID_MAX) {
        *code = "wifi-ssid";
        goto fail;
    }
    rc = member_string(inner, inner_count, "security", doc->security,
                       sizeof(doc->security), code);
    if (rc < 0) {
        *code = "wifi-security";
        goto fail;
    }
    if (!rc || !api_valid_wifi_security(doc->security)) {
        *code = "wifi-security";
        goto fail;
    }
    rc = member_string(inner, inner_count, "password", doc->password,
                       sizeof(doc->password), code);
    if (rc < 0) {
        *code = "wifi-password";
        goto fail;
    }
    password_len = strlen(doc->password);
    if (!strcmp(doc->security, "open")) {
        /* An open network carries no passphrase, so a non-empty one is a
           document that does not say what it says. */
        if (password_len) {
            *code = "wifi-password";
            goto fail;
        }
    } else if (password_len < LE_PROVISION_PASSWORD_MIN ||
               password_len > LE_PROVISION_PASSWORD_MAX) {
        *code = "wifi-password";
        goto fail;
    }
    doc->has_wifi = 1;
    return 1;
fail:
    memset(doc->password, 0, sizeof(doc->password));
    return 0;
}

static int parse_settings(const struct json_member *members, size_t count,
                          struct provision_doc *doc, const char **code)
{
    static const char *const allowed[] = {
        "hostname", "volume", "wake_word", "wake_sensitivity",
        "privacy_local_only", "privacy_telemetry"
    };
    struct json_member inner[LE_PROVISION_MAX_MEMBERS];
    const struct json_member *settings = member_find(members, count, "settings");
    size_t inner_count = 0;
    int rc;

    /* Every settings key is optional; a missing key means the shipped default. */
    if (!settings)
        return 1;
    if (settings->value_len < 2 || settings->value[0] != '{' ||
        !members_known(settings->value, settings->value_len, allowed,
                       sizeof(allowed) / sizeof(allowed[0]), code) ||
        json_object_members(settings->value, settings->value_len, inner,
                            LE_PROVISION_MAX_MEMBERS, &inner_count)) {
        *code = *code ? *code : "settings-shape";
        return 0;
    }
    rc = member_string(inner, inner_count, "hostname", doc->hostname,
                       sizeof(doc->hostname), code);
    if (rc < 0 || (rc && !api_valid_hostname(doc->hostname))) {
        *code = "settings-hostname";
        return 0;
    }
    doc->has_hostname = rc;
    rc = member_int(inner, inner_count, "volume", &doc->volume, 0, 100, code);
    if (rc < 0) {
        *code = "settings-volume";
        return 0;
    }
    doc->has_volume = rc;
    rc = member_string(inner, inner_count, "wake_word", doc->wake_word,
                       sizeof(doc->wake_word), code);
    if (rc < 0 || (rc && (!doc->wake_word[0] ||
                          strlen(doc->wake_word) >= LE_TEXT))) {
        *code = "settings-wake-word";
        return 0;
    }
    doc->has_wake_word = rc;
    rc = member_int(inner, inner_count, "wake_sensitivity", &doc->sensitivity,
                    0, 100, code);
    if (rc < 0) {
        *code = "settings-wake-sensitivity";
        return 0;
    }
    doc->has_sensitivity = rc;
    rc = member_bool(inner, inner_count, "privacy_local_only", &doc->local_only,
                     code);
    if (rc < 0) {
        *code = "settings-privacy-local-only";
        return 0;
    }
    doc->has_local_only = rc;
    rc = member_bool(inner, inner_count, "privacy_telemetry", &doc->telemetry,
                     code);
    if (rc < 0) {
        *code = "settings-privacy-telemetry";
        return 0;
    }
    doc->has_telemetry = rc;
    doc->has_settings = 1;
    return 1;
}

static int parse_binding(const struct json_member *members, size_t count)
{
    static const char *const allowed[] = { "release", "target" };
    struct json_member binding[LE_PROVISION_MAX_MEMBERS];
    const struct json_member *item = member_find(members, count, "binding");
    size_t count_inner = 0;
    const char *code = NULL;

    if (!item)
        return 1;
    if (item->value_len < 2 || item->value[0] != '{' ||
        !members_known(item->value, item->value_len, allowed, 2, &code) ||
        json_object_members(item->value, item->value_len, binding,
                            LE_PROVISION_MAX_MEMBERS, &count_inner))
        return 0;
    /* The binding is accepted and bounded but not compared: this daemon is
       the release's own payload, so the delivery-side check is the one that
       knows which release and target the file was built for. */
    return 1;
}

/*
 * Validate the whole document before the caller applies any of it. `text` is
 * zeroed by the caller as soon as this returns, so a rejected passphrase does
 * not outlive the read.
 */
static int document_parse(char *text, size_t len, struct provision_doc *doc,
                          const char **code)
{
    static const char *const allowed[] = {
        "schema", "binding", "admin", "wifi", "settings"
    };
    struct json_member members[LE_PROVISION_MAX_MEMBERS];
    char schema[sizeof(LE_PROVISION_SCHEMA)];
    size_t count = 0;

    memset(doc, 0, sizeof(*doc));
    *code = "shape";
    if (!json_valid_object(text, len) ||
        !members_known(text, len, allowed, sizeof(allowed) / sizeof(allowed[0]),
                       code)) {
        *code = *code ? *code : "shape";
        return 0;
    }
    if (json_object_members(text, len, members, LE_PROVISION_MAX_MEMBERS,
                            &count)) {
        *code = "shape";
        return 0;
    }
    if (member_string(members, count, "schema", schema, sizeof(schema), code) !=
            1 ||
        strcmp(schema, LE_PROVISION_SCHEMA)) {
        *code = "schema";
        return 0;
    }
    if (!parse_binding(members, count) || !parse_admin(members, count, doc, code) ||
        !parse_wifi(members, count, doc, code) ||
        !parse_settings(members, count, doc, code)) {
        *code = *code ? *code : "shape";
        memset(doc->password, 0, sizeof(doc->password));
        return 0;
    }
    return 1;
}

/*
 * Read, check and unlink. The unlink happens on every path out of here,
 * including the rejections, which is the point: the file holds a plaintext
 * passphrase and must not survive being read.
 */
static int provision_read(const char *config_path, char *text, size_t size,
                          size_t *len, const char **code)
{
    char path[512];
    struct stat st;
    ssize_t n;
    int fd;

    if (provision_dir_path(config_path, LE_PROVISION_FILE, path, sizeof(path))) {
        *code = "path";
        return 0;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
              );
    if (fd < 0) {
        /* A symlink fails here with ELOOP rather than being followed. */
        *code = errno == ELOOP ? "file-symlink" : "file-unreadable";
        goto unlink;
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        *code = "file-not-regular";
        goto unlink;
    }
    /* 0600 and owner-only, which on the device means root's own private file.
       config_write_atomic() enforces the same mode on everything it writes. */
    if ((st.st_mode & 07777) != 0600 || st.st_uid != geteuid()) {
        close(fd);
        *code = "file-mode";
        goto unlink;
    }
    if (st.st_size < 0 || st.st_size > (off_t)LE_PROVISION_MAX_BYTES ||
        (size_t)st.st_size >= size) {
        close(fd);
        *code = "file-size";
        goto unlink;
    }
    n = read(fd, text, (size_t)st.st_size);
    close(fd);
    if (n < 0 || (size_t)n != (size_t)st.st_size) {
        *code = "file-unreadable";
        unlink(path);
        return 0;
    }
    *len = (size_t)n;
    unlink(path);
    return 1;
unlink:
    unlink(path);
    return 0;
}

/*
 * Create the account, merge the settings, and hand Wi-Fi to the backend.
 *
 * This runs in the daemon process itself, before the listener opens, so the
 * context stays this process's own throughout: that is what lets the poll
 * finish the transaction against the same backend the hand-off was issued
 * through. Every exit writes the result file.
 *
 * Returns what the caller needs to decide whether there is anything left to
 * wait for: 1 when an account was created and the hand-off was issued, 0 when
 * there is no association to wait for, -1 when the result file could not be
 * written (the device still boots, and the wizard stays available).
 */
static int provision_apply(struct api_context *c, struct provision_doc *doc)
{
    struct provision_status status = {
        "partial", "none", "skipped", "absent", "absent"
    };
    struct le_wifi_credentials wifi;
    int rc;

    /* The account is created only when there is none, which is the same rule
       the bootstrap endpoint enforces; an existing account is never reset. */
    if (!c->users_path[0])
        status.admin = "skipped";
    else if (c->auth.user_count || !access(c->users_path, F_OK))
        status.admin = "kept";
    else if (le_auth_add_users_line(&c->auth, c->users_path, doc->users_line))
        status.admin = "skipped";
    else
        status.admin = "created";

    if (strcmp(status.admin, "created")) {
        /* The document validated but its account was not applied, so there is
           no provisioned setup to claim. Settings still merge: they are the
           user's own choices and nothing else depends on them here. */
        status.result = "partial";
        status.error = "admin-not-created";
        goto settings;
    }

settings:
    if (doc->has_settings) {
        /* The same order the wizard uses: hostname, audio, wake, then the
           canonical persist. persist_configuration() writes hostname together
           with hostname_persisted, which is what the contract asks for. */
        if (doc->has_hostname &&
            (rc = le_set_hostname(c->backend, doc->hostname))) {
            status.error = "settings-hostname";
            goto record;
        }
        if (doc->has_volume &&
            (rc = le_set_volume(c->backend, doc->volume))) {
            status.error = "settings-volume";
            goto record;
        }
        /* A wake adapter that is not up yet still gets the saved values: the
           wizard tolerates LE_NOT_SUPPORTED here too, and the boot-time
           restore applies them once the daemon is running. */
        if (doc->has_wake_word &&
            (rc = le_set_wake_word(c->backend, doc->wake_word)) &&
            rc != LE_NOT_SUPPORTED) {
            status.error = "settings-wake-word";
            goto record;
        }
        if (doc->has_sensitivity &&
            (rc = le_set_wake_word_sensitivity(c->backend, doc->sensitivity)) &&
            rc != LE_NOT_SUPPORTED) {
            status.error = "settings-wake-sensitivity";
            goto record;
        }
        if (doc->has_wake_word)
            snprintf(c->configured_wake_word, sizeof(c->configured_wake_word),
                     "%s", doc->wake_word);
        if (doc->has_sensitivity)
            c->configured_wake_sensitivity = doc->sensitivity;
        if (doc->has_wake_word || doc->has_sensitivity)
            c->configured_wake_valid = 1;
        if (doc->has_local_only)
            c->privacy_local_only = doc->local_only;
        if (doc->has_telemetry)
            c->privacy_telemetry = doc->telemetry;
        /* Setup always enables AirPlay 2; a setup-equivalent configuration
           must match that, and no other bit is touched. */
        c->integrations |= 16u;
        if (api_persist_configuration(c)) {
            status.error = "settings-write";
            goto record;
        }
        status.settings = "applied";
    }

    if (!doc->has_wifi) {
        /* Nothing to wait for, so there is nothing that could later prove the
           network works: the wizard stays available. */
        status.error = "wifi-absent";
        goto record;
    }
    memset(&wifi, 0, sizeof(wifi));
    snprintf(wifi.ssid, sizeof(wifi.ssid), "%s", doc->ssid);
    snprintf(wifi.security, sizeof(wifi.security), "%s", doc->security);
    memcpy(wifi.password, doc->password, sizeof(wifi.password));
    /* The same call /api/v1/setup makes. networkd and wpa_supplicant
       serialise the credentials; nothing here writes that file. */
    rc = le_connect_wifi(c->backend, &wifi);
    memset(wifi.password, 0, sizeof(wifi.password));
    if (rc) {
        status.wifi = "failed";
        status.error = rc == LE_INVALID ? "wifi-rejected" : "wifi-handoff";
        goto record;
    }
    status.wifi = "handed-off";
    status.error = "assoc-pending";
record:
    memset(doc->password, 0, sizeof(doc->password));
    memset(doc->users_line, 0, sizeof(doc->users_line));
    if (write_status(c->config_path, &status))
        return -1;
    /* Only a hand-off that was actually issued, for an account this run
       created, leaves an association that could still be waited for. The
       document carrying no settings is not a reason to withhold completion:
       the contract asks that the settings were applied, and a document with
       none to apply has nothing outstanding. */
    return !strcmp(status.admin, "created") &&
           !strcmp(status.wifi, "handed-off");
}

/*
 * Apply the document, inline.
 *
 * The account, the settings and the hand-off are all this process's work, so
 * what happened is returned rather than re-read from the result file: the file
 * is a report for a human and for the installer, not the mechanism this daemon
 * uses to talk to itself. A write failure there is logged and the boot
 * continues, because a device that cannot record a status line is still a
 * device, and the setup wizard stays available either way.
 */
static void provision_start(struct api_context *c, struct provision_doc *doc)
{
    int pending;

    if (doc->has_wifi) {
        /* Recorded here, because this is the process that will have to observe
           the association. The SSID is a network name, not the secret; the
           passphrase is zeroed by provision_apply() and never copied out. */
        snprintf(c->provision_ssid, sizeof(c->provision_ssid), "%s", doc->ssid);
        c->provision_started = time(0);
    }
    pending = provision_apply(c, doc);
    /* Only a hand-off that was actually issued leaves something to wait for.
       A refusal, a validation failure or an absent document leaves the wizard
       available, which is the contract's outcome for each of those. */
    if (pending == 1)
        c->provision_pending = 1;
    else
        memset(c->provision_ssid, 0, sizeof(c->provision_ssid));
    if (pending < 0)
        api_log(c, "warning",
                "Provisioned settings could not be recorded as applied on this boot");
}

int api_provision_start(struct api_context *c)
{
    struct provision_status status = provision_skipped;
    struct provision_doc doc;
    char path[512];
    char text[LE_PROVISION_MAX_BYTES + 1];
    size_t len = 0;
    struct stat st;
    const char *code = "shape";

    if (!c || !c->config_path[0])
        return 0;
    if (provision_dir_path(c->config_path, LE_PROVISION_FILE, path,
                           sizeof(path)))
        return 0;
    /* Absent is the normal state on every boot after the one that applied it. */
    if (lstat(path, &st))
        return 0;
    if (c->setup_completed) {
        /* A device whose owner has already been through setup keeps what they
           chose; the delivered document is not applied over it. */
        status.error = "already-complete";
        unlink(path);
        (void)write_status(c->config_path, &status);
        return 1;
    }
    if (!provision_read(c->config_path, text, sizeof(text), &len, &code)) {
        status.error = code;
        (void)write_status(c->config_path, &status);
        api_log(c, "warning", "Provision file was refused and removed");
        return 1;
    }
    if (!document_parse(text, len, &doc, &code)) {
        status.error = code;
        memset(text, 0, sizeof(text));
        memset(&doc, 0, sizeof(doc));
        (void)write_status(c->config_path, &status);
        api_log(c, "warning",
                "Provision file failed validation; nothing was applied");
        return 1;
    }
    memset(text, 0, sizeof(text));
    provision_start(c, &doc);
    return 1;
}

static long provision_assoc_timeout(void)
{
    const char *text = getenv("LIBREECHO_PROVISION_ASSOC_TIMEOUT_SECONDS");
    long value;
    char *end;

    if (!text || !*text)
        return LE_PROVISION_ASSOC_TIMEOUT_DEFAULT;
    value = strtol(text, &end, 10);
    if (*end || value < 1 || value > LE_PROVISION_ASSOC_TIMEOUT_MAX)
        return LE_PROVISION_ASSOC_TIMEOUT_DEFAULT;
    return value;
}

/*
 * Finish the transaction from the HTTP server's one-second tick. Called there
 * rather than from the apply because the association is a wait, and a wait
 * does not belong in front of the listener.
 */
void api_provision_poll(struct api_context *c)
{
    struct le_network_state network;
    struct provision_status status = provision_applied;

    if (!c || !c->provision_pending || !c->config_path[0])
        return;
    memset(&network, 0, sizeof(network));
    /* Completion is a claim about a working device, so it waits for this
       hand-off's own association, with an address, and not for the device
       merely being online: a device that was already joined to a different
       network has not proved that anything about this one. Not there yet is
       not a failure either -- the window is still open, so keep waiting. */
    if (!le_get_network_state(c->backend, &network) && network.ip[0] &&
        !strcmp(network.state, "connected") && network.ssid[0] &&
        !strcmp(network.ssid, c->provision_ssid)) {
        c->provision_pending = 0;
        if (api_write_setup_marker(c, 1)) {
            api_log(c, "error",
                    "Provisioned setup could not be recorded as complete");
            return;
        }
        /* The apply ran in this process, so the account and settings are
           already the ones this context holds; nothing has to be re-read. */
        c->setup_completed = 1;
        status = provision_applied;
        (void)write_status(c->config_path, &status);
        api_log(c, "info", "Provisioned setup applied and verified");
        return;
    }
    if (time(0) - c->provision_started < provision_assoc_timeout())
        return;
    /* Out of time: the account and settings stay applied, the hand-off is
       reported as failed, and no marker is written. */
    c->provision_pending = 0;
    status = (struct provision_status){
        "partial", "assoc-timeout", "created", "failed", "applied"
    };
    (void)write_status(c->config_path, &status);
    api_log(c, "warning",
            "Provisioned Wi-Fi did not associate in time; the setup wizard stays available");
}
