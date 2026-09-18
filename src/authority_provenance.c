#define _POSIX_C_SOURCE 200809L
#include "authority_provenance.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AP_FEATURE_COUNT 5
#define AP_FEATURE_PATHS 9
#define AP_SNAPSHOT_MAX (AP_FEATURE_COUNT * AP_FEATURE_PATHS + 16)
#define AP_TIMEOUT_SECONDS 30
#define AP_RETRY_SECONDS 5
#define AP_RETRY_BURST 3

static const char *ap_features[AP_FEATURE_COUNT] = {
    "airplay2", "tts", "wakeword", "stt", "assistant"
};
static const char *ap_services[AP_FEATURE_COUNT] = {
    "airplayd", "ttsd", "waked", "sttd", "agentd"
};
static const char *ap_feature_fields[] = {
    "action", "release", "source_commit", "kind", "payload_sha256",
    "manifest_sha256", "runtime_sha256", "runtime_manifest_sha256",
    "daemon_sha256"
};
#define AP_FEATURE_FIELD_COUNT (sizeof(ap_feature_fields) / sizeof(ap_feature_fields[0]))

typedef struct {
    int present;
    int valid;
    struct stat st;
} ap_observation;

typedef struct {
    size_t count;
    char paths[AP_SNAPSHOT_MAX][512];
    ap_observation entries[AP_SNAPSHOT_MAX];
} ap_snapshot;

typedef struct {
    char action[16], release[128], source_commit[64], kind[16];
    char payload_sha256[65], manifest_sha256[65], runtime_sha256[65];
    char runtime_manifest_sha256[65], daemon_sha256[65];
} ap_feature;

typedef struct {
    char transaction_id[128], installed_sha256[65], manifest_sha256[65];
    char manifest_sig_sha256[65];
    ap_feature features[AP_FEATURE_COUNT];
} ap_record;

typedef struct {
    int active;

    int fd;
    pid_t pid;
    int child_reaped;
    int child_status;
    int cancelled;
    struct timespec started;
    size_t used;
    char output[LE_AUTHORITY_PROVENANCE_OUTPUT_MAX + 1];
    ap_snapshot before;
} ap_job;

static ap_job job = {.fd = -1};
static ap_record record;
static ap_snapshot completed_snapshot;
static int completed;
static time_t retry_after;
static time_t retry_window;
static int retry_count;

static const char *ap_update_root(void)
{
    const char *value = getenv("LIBREECHO_UPDATE_ROOT");
    return value && value[0] ? value : "/data/libreecho/update";
}

static const char *ap_feature_root(void)
{
    const char *value = getenv("LIBREECHO_FEATURE_ROOT");
    return value && value[0] ? value : "/data/libreecho/features";
}

static const char *ap_run_root(void)
{
    const char *value = getenv("LIBREECHO_FEATURE_RUN_ROOT");
    return value && value[0] ? value : "/var/run";
}

static const char *ap_helper(void)
{
    const char *value = getenv("LIBREECHO_FEATURE_TRANSACTION_HELPER");
    return value && value[0] ? value : "/usr/local/sbin/libreecho-feature-transaction";
}

static int ap_path(char *out, size_t size, const char *first, const char *second)
{
    int n = snprintf(out, size, "%s/%s", first, second);
    return n >= 0 && (size_t)n < size;
}

static int ap_add_path(ap_snapshot *snapshot, const char *path)
{
    size_t i;
    if (!snapshot || !path || !path[0] || strlen(path) >= sizeof(snapshot->paths[0]))
        return 0;
    for (i = 0; i < snapshot->count; ++i)
        if (!strcmp(snapshot->paths[i], path))
            return 1;
    if (snapshot->count >= AP_SNAPSHOT_MAX)
        return 0;
    snprintf(snapshot->paths[snapshot->count++], sizeof(snapshot->paths[0]), "%s", path);
    return 1;
}

static int ap_pid_from_file(const char *path, pid_t *pid)
{
    char value[32], *end;
    int fd;
    ssize_t n;
    long parsed;
    if (!pid)
        return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return 0;
    n = read(fd, value, sizeof(value) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    value[n] = 0;
    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno || end == value || (*end != 0 && *end != '\n' && *end != '\r') || parsed <= 1)
        return 0;
    *pid = (pid_t)parsed;
    return 1;
}

static int ap_build_snapshot(ap_snapshot *snapshot)
{
    char path[512], feature_dir[512], pid_path[512], proc_path[512], daemon_path[512];
    pid_t pid;
    int i;
    if (!snapshot)
        return 0;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!ap_path(path, sizeof(path), ap_update_root(), "pending") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "feature-commit") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "installed") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "staging/manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-manifest.sig") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-airplay2.manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-airplay2.sig") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-tts.manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-tts.sig") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-wakeword.manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-wakeword.sig") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-stt.manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-stt.sig") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-assistant.manifest") || !ap_add_path(snapshot, path) ||
        !ap_path(path, sizeof(path), ap_update_root(), "committed-runtime-assistant.sig") || !ap_add_path(snapshot, path) ||
        !ap_add_path(snapshot, "/usr/local/libexec/libreecho-update-verify") ||
        !ap_add_path(snapshot, "/etc/libreecho/ota-public-key.hex"))
        return 0;
    for (i = 0; i < AP_FEATURE_COUNT; ++i) {
        if (!ap_path(feature_dir, sizeof(feature_dir), ap_feature_root(), ap_features[i]) ||
            !ap_path(path, sizeof(path), feature_dir, "manifest.json") || !ap_add_path(snapshot, path) ||
            !ap_path(path, sizeof(path), feature_dir, "runtime-manifest.json") || !ap_add_path(snapshot, path) ||
            !ap_path(path, sizeof(path), feature_dir, "payload.squashfs") || !ap_add_path(snapshot, path) ||
            !ap_path(path, sizeof(path), feature_dir, "runtime.squashfs") || !ap_add_path(snapshot, path))
            return 0;
        {
            int n = snprintf(pid_path, sizeof(pid_path), "%s/libreecho-%s.pid", ap_run_root(), ap_services[i]);
            if (n < 0 || (size_t)n >= sizeof(pid_path) || !ap_add_path(snapshot, pid_path))
                return 0;
        }
        if (ap_pid_from_file(pid_path, &pid)) {
            int n = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/exe", (long)pid);
            if (n < 0 || (size_t)n >= sizeof(proc_path) || !ap_add_path(snapshot, proc_path))
                return 0;
        }
        if (snprintf(daemon_path, sizeof(daemon_path), "%s/libreecho/features/%s/root/usr/local/sbin/libreecho-%s", ap_run_root(), ap_features[i], i == 0 ? "audio-engine" : ap_services[i]) < 0 ||
            strlen(daemon_path) >= sizeof(daemon_path) || !ap_add_path(snapshot, daemon_path))
            return 0;
    }
    return 1;
}

static void ap_take_snapshot(ap_snapshot *snapshot)
{
    size_t i;
    if (!ap_build_snapshot(snapshot))
        return;
    for (i = 0; i < snapshot->count; ++i) {
        if (!lstat(snapshot->paths[i], &snapshot->entries[i].st)) {
            snapshot->entries[i].present = 1;
            snapshot->entries[i].valid = 1;
        } else if (errno == ENOENT) {
            snapshot->entries[i].present = 0;
            snapshot->entries[i].valid = 1;
        } else {
            snapshot->entries[i].valid = 0;
        }
    }
}

static int ap_same_stat(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino &&
           left->st_mode == right->st_mode && left->st_size == right->st_size &&
           left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
           left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
           left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
           left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
}

static int ap_snapshot_equal(const ap_snapshot *left, const ap_snapshot *right)
{
    size_t i;
    if (!left || !right || left->count != right->count)
        return 0;
    for (i = 0; i < left->count; ++i) {
        if (strcmp(left->paths[i], right->paths[i]) ||
            left->entries[i].valid != right->entries[i].valid ||
            left->entries[i].present != right->entries[i].present)
            return 0;
        if (left->entries[i].present &&
            !ap_same_stat(&left->entries[i].st, &right->entries[i].st))
            return 0;
    }
    return 1;
}

static long ap_elapsed_seconds(const struct timespec *started)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return AP_TIMEOUT_SECONDS + 1;
    return (long)(now.tv_sec - started->tv_sec);
}

static int ap_hex_hash(const char *value)
{
    size_t i;
    if (!value || strlen(value) != 64)
        return 0;
    for (i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)value[i]))
            return 0;
    return 1;
}

static int ap_token(const char *value, size_t max)
{
    size_t i;
    if (!value || !value[0] || strlen(value) >= max)
        return 0;
    for (i = 0; value[i]; ++i)
        if (!isalnum((unsigned char)value[i]) && value[i] != '.' && value[i] != '_' &&
            value[i] != '+' && value[i] != '~' && value[i] != ':' && value[i] != '-')
            return 0;
    return 1;
}

static int ap_field_index(const char *key, int *feature, int *field)
{
    char prefix[128];
    int i, j, n;
    if (!key || !feature || !field)
        return 0;
    if (!strcmp(key, "schema") || !strcmp(key, "transaction_id") ||
        !strcmp(key, "installed_sha256") || !strcmp(key, "manifest_sha256") ||
        !strcmp(key, "manifest_sig_sha256") || !strcmp(key, "feature_ids"))
        return 1;
    for (i = 0; i < AP_FEATURE_COUNT; ++i)
        for (j = 0; j < (int)AP_FEATURE_FIELD_COUNT; ++j) {
            n = snprintf(prefix, sizeof(prefix), "feature_%s_%s", ap_features[i], ap_feature_fields[j]);
            if (n >= 0 && (size_t)n < sizeof(prefix) && !strcmp(key, prefix)) {
                *feature = i;
                *field = j;
                return 2;
            }
        }
    return 0;
}

static int ap_assign(ap_record *parsed, const char *key, const char *value)
{
    int feature = -1, field = -1, kind, n;
    char *target = NULL;
    size_t target_size = 0;
    kind = ap_field_index(key, &feature, &field);
    if (!kind || !value || !value[0] || strchr(value, '\r') || strchr(value, '\n') || strchr(value, '='))
        return 0;
    if (!strcmp(key, "schema"))
        return !strcmp(value, "libreecho-feature-provenance-v1");
    if (!strcmp(key, "feature_ids"))
        return !strcmp(value, "airplay2,tts,wakeword,stt,assistant");
    if (!strcmp(key, "transaction_id")) {
        if (!ap_token(value, sizeof(parsed->transaction_id))) return 0;
        target = parsed->transaction_id; target_size = sizeof(parsed->transaction_id);
    } else if (!strcmp(key, "installed_sha256")) {
        if (!ap_hex_hash(value)) return 0;
        target = parsed->installed_sha256; target_size = sizeof(parsed->installed_sha256);
    } else if (!strcmp(key, "manifest_sha256")) {
        if (!ap_hex_hash(value)) return 0;
        target = parsed->manifest_sha256; target_size = sizeof(parsed->manifest_sha256);
    } else if (!strcmp(key, "manifest_sig_sha256")) {
        if (!ap_hex_hash(value)) return 0;
        target = parsed->manifest_sig_sha256; target_size = sizeof(parsed->manifest_sig_sha256);
    } else if (kind == 2) {
        ap_feature *item = &parsed->features[feature];
        target = field == 0 ? item->action : field == 1 ? item->release :
                 field == 2 ? item->source_commit : field == 3 ? item->kind :
                 field == 4 ? item->payload_sha256 : field == 5 ? item->manifest_sha256 :
                 field == 6 ? item->runtime_sha256 : field == 7 ? item->runtime_manifest_sha256 :
                 item->daemon_sha256;
        target_size = field == 1 ? sizeof(item->release) : field == 2 ? sizeof(item->source_commit) :
                      field == 0 || field == 3 ? 16 : sizeof(item->payload_sha256);
        if (field == 0) {
            if (strcmp(value, "preserve") && strcmp(value, "replace") && strcmp(value, "runtime")) return 0;
        } else if (field == 1) {
            if (!ap_token(value, sizeof(item->release))) return 0;
        } else if (field == 2) {
            if (strlen(value) != 40) return 0;
            for (n = 0; n < 40; ++n) if (!islower((unsigned char)value[n]) && !isdigit((unsigned char)value[n])) return 0;
        } else if (field == 3) {
            if (strcmp(value, "base") && strcmp(value, "runtime")) return 0;
        } else if (field == 6 || field == 7) {
            if (strcmp(value, "none") && !ap_hex_hash(value)) return 0;
        } else if (!ap_hex_hash(value)) return 0;
    } else {
        return 0;
    }
    if (!target || strlen(value) >= target_size)
        return 0;
    snprintf(target, target_size, "%s", value);
    return 1;
}

static int ap_parse(const char *text, size_t length, ap_record *parsed)
{
    const char *line = text, *end, *equals;
    unsigned char seen[AP_FEATURE_COUNT * AP_FEATURE_FIELD_COUNT + 6] = {0};
    size_t line_length, key_length, value_length, i;
    char key[128], value[256];
    int feature, field, kind, index;
    if (!text || !parsed || !length || text[length - 1] != '\n')
        return 0;
    memset(parsed, 0, sizeof(*parsed));
    while ((size_t)(line - text) < length) {
        end = memchr(line, '\n', length - (size_t)(line - text));
        if (!end)
            return 0;
        line_length = (size_t)(end - line);
        if (!line_length)
            return 0;
        equals = memchr(line, '=', line_length);
        if (!equals || equals == line || equals == end)
            return 0;
        key_length = (size_t)(equals - line);
        value_length = line_length - key_length - 1;
        if (key_length >= sizeof(key) || value_length >= sizeof(value))
            return 0;
        for (i = 0; i < key_length; ++i)
            if (line[i] < 'a' || line[i] > 'z')
                if (line[i] < '0' || line[i] > '9')
                    if (line[i] != '_') return 0;
        memcpy(key, line, key_length); key[key_length] = 0;
        memcpy(value, equals + 1, value_length); value[value_length] = 0;
        kind = ap_field_index(key, &feature, &field);
        if (!kind) return 0;
        index = kind == 1 ? (!strcmp(key, "schema") ? 0 : !strcmp(key, "transaction_id") ? 1 :
                 !strcmp(key, "installed_sha256") ? 2 : !strcmp(key, "manifest_sha256") ? 3 :
                 !strcmp(key, "manifest_sig_sha256") ? 4 : 5) :
                6 + feature * AP_FEATURE_FIELD_COUNT + field;
        if (index < 0 || index >= (int)(sizeof(seen) / sizeof(seen[0])) || seen[index]) return 0;
        seen[index] = 1;
        if (!ap_assign(parsed, key, value)) return 0;
        line = end + 1;
    }
    if (!seen[0] || !seen[1] || !seen[2] || !seen[3] || !seen[4] || !seen[5]) return 0;
    for (i = 0; i < AP_FEATURE_COUNT; ++i) {
        size_t base = 6 + i * AP_FEATURE_FIELD_COUNT;
        size_t j;
        for (j = 0; j < AP_FEATURE_FIELD_COUNT; ++j)
            if (!seen[base + j]) return 0;
        if ((!strcmp(parsed->features[i].action, "replace") && !strcmp(parsed->features[i].kind, "runtime")) ||
            (!strcmp(parsed->features[i].action, "runtime") && !strcmp(parsed->features[i].kind, "base")) ||
            (!strcmp(parsed->features[i].kind, "runtime") &&
             (!strcmp(parsed->features[i].runtime_sha256, "none") ||
              !strcmp(parsed->features[i].runtime_manifest_sha256, "none"))) ||
            (!strcmp(parsed->features[i].kind, "base") &&
             (strcmp(parsed->features[i].runtime_sha256, "none") ||
              strcmp(parsed->features[i].runtime_manifest_sha256, "none")))) return 0;
    }
    return 1;
}

static void ap_close_job(void)
{
    if (job.active && job.fd >= 0)
        close(job.fd);
    memset(&job, 0, sizeof(job));
    job.fd = -1;
}

static int ap_make_json(const ap_record *parsed, char *out, size_t size)
{
    size_t used = 0, i;
    int n;
    char runtime_hash[80], runtime_manifest_hash[80];
    if (!out || !size || !parsed) return 0;
    n = snprintf(out, size, "{\"available\":true,\"schema\":\"libreecho-feature-provenance-v1\",\"transaction_id\":\"%s\",\"installed_sha256\":\"%s\",\"manifest_sha256\":\"%s\",\"manifest_sig_sha256\":\"%s\",\"features\":[", parsed->transaction_id, parsed->installed_sha256, parsed->manifest_sha256, parsed->manifest_sig_sha256);
    if (n < 0 || (size_t)n >= size) return 0;
    used = (size_t)n;
    for (i = 0; i < AP_FEATURE_COUNT; ++i) {
        const ap_feature *f = &parsed->features[i];
        if (!strcmp(f->runtime_sha256, "none")) snprintf(runtime_hash, sizeof(runtime_hash), "null");
        else snprintf(runtime_hash, sizeof(runtime_hash), "\"%s\"", f->runtime_sha256);
        if (!strcmp(f->runtime_manifest_sha256, "none")) snprintf(runtime_manifest_hash, sizeof(runtime_manifest_hash), "null");
        else snprintf(runtime_manifest_hash, sizeof(runtime_manifest_hash), "\"%s\"", f->runtime_manifest_sha256);
        n = snprintf(out + used, size - used, "%s{\"feature_id\":\"%s\",\"action\":\"%s\",\"release\":\"%s\",\"source_commit\":\"%s\",\"kind\":\"%s\",\"payload_sha256\":\"%s\",\"manifest_sha256\":\"%s\",\"runtime_sha256\":%s,\"runtime_manifest_sha256\":%s,\"daemon_sha256\":\"%s\"}", i ? "," : "", ap_features[i], f->action, f->release, f->source_commit, f->kind, f->payload_sha256, f->manifest_sha256, runtime_hash, runtime_manifest_hash, f->daemon_sha256);
        if (n < 0 || (size_t)n >= size - used) return 0;
        used += (size_t)n;
    }
    n = snprintf(out + used, size - used, "]}");
    return n >= 0 && (size_t)n < size - used;
}

static void ap_unavailable(char *out, size_t size)
{
    if (size)
        snprintf(out, size, "{\"available\":false,\"schema\":\"libreecho-feature-provenance-v1\",\"reason\":\"unavailable\",\"transaction_id\":null,\"features\":[]}");
}

static void ap_schedule_retry(void)
{
    time_t now = time(NULL);
    completed = 0;
    if (!retry_window || now - retry_window >= 60) {
        retry_window = now;
        retry_count = 0;
    }
    retry_count++;
    retry_after = now + (retry_count <= AP_RETRY_BURST ? AP_RETRY_SECONDS : 30);
}

static void ap_cancel_child(void)
{
    int waited;
    if (!job.active) { ap_schedule_retry(); return; }
    (void)kill(job.pid, SIGKILL);
    waited = job.child_reaped ? 1 : waitpid(job.pid, NULL, WNOHANG);
    if (waited > 0 || waited < 0) ap_close_job();
    else {
        if (job.fd >= 0) { close(job.fd); job.fd = -1; }
        job.cancelled = 1;
    }
    ap_schedule_retry();
}

static void ap_finish_unavailable(void)
{
    ap_close_job();
    ap_schedule_retry();
}

static void ap_start(void)
{
    int pipefd[2], flags;
    pid_t pid;
    if (job.active || completed || time(NULL) < retry_after) return;
    if (access(ap_helper(), X_OK) < 0) { ap_schedule_retry(); return; }
    ap_take_snapshot(&job.before);
    if (!job.before.count || pipe(pipefd) < 0) { ap_schedule_retry(); return; }
    pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); ap_schedule_retry(); return; }
    if (!pid) {
        int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        if (sink >= 0) { dup2(sink, STDERR_FILENO); close(sink); }
        close(pipefd[1]);
        execl(ap_helper(), ap_helper(), "provenance", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    flags = fcntl(pipefd[0], F_GETFL);
    if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        close(pipefd[0]); kill(pid, SIGKILL); (void)waitpid(pid, NULL, WNOHANG); ap_schedule_retry(); return;
    }
    job.active = 1; job.fd = pipefd[0]; job.pid = pid; job.used = 0; job.cancelled = 0;
    clock_gettime(CLOCK_MONOTONIC, &job.started);
}

void le_authority_provenance_tick(void)
{
    char buffer[2048];
    ssize_t n;
    int status, waited;
    ap_record parsed;
    ap_snapshot after;
    if (job.cancelled) {
        waited = waitpid(job.pid, &status, WNOHANG);
        if (waited > 0 || (waited < 0 && errno == ECHILD)) ap_close_job();
        return;
    }
    ap_start();
    if (!job.active) return;
    if (ap_elapsed_seconds(&job.started) > AP_TIMEOUT_SECONDS) {
        ap_cancel_child(); return;
    }
    for (;;) {
        n = read(job.fd, buffer, sizeof(buffer));
        if (n > 0) {
            if (job.used + (size_t)n > LE_AUTHORITY_PROVENANCE_OUTPUT_MAX) {
                ap_cancel_child(); return;
            }
            memcpy(job.output + job.used, buffer, (size_t)n); job.used += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n < 0) { ap_cancel_child(); return; }
        if (!job.child_reaped) {
            waited = waitpid(job.pid, &status, WNOHANG);
            if (waited == 0) return;
            if (waited < 0) { ap_finish_unavailable(); return; }
            job.child_reaped = 1;
            job.child_status = status;
        }
        close(job.fd); job.fd = -1;
        if (!WIFEXITED(job.child_status) || WEXITSTATUS(job.child_status) != 0 ||
            !ap_parse(job.output, job.used, &parsed)) { ap_finish_unavailable(); return; }
        ap_take_snapshot(&after);
        if (!ap_snapshot_equal(&job.before, &after)) { ap_finish_unavailable(); return; }
        record = parsed; completed_snapshot = after; completed = 1; ap_close_job();
        return;
    }
    if (!job.child_reaped) {
        waited = waitpid(job.pid, &status, WNOHANG);
        if (waited < 0) ap_finish_unavailable();
        else if (waited > 0) { job.child_reaped = 1; job.child_status = status; }
    }
}

void le_authority_provenance_json(char *out, size_t size)
{
    ap_snapshot current;
    if (!completed) { ap_unavailable(out, size); return; }
    ap_take_snapshot(&current);
    if (!ap_snapshot_equal(&completed_snapshot, &current)) { ap_schedule_retry(); ap_unavailable(out, size); return; }
    if (!ap_make_json(&record, out, size)) ap_unavailable(out, size);
}

void le_authority_provenance_shutdown(void)
{
    if (job.active) { (void)kill(job.pid, SIGKILL); (void)waitpid(job.pid, NULL, WNOHANG); }
    ap_close_job();
    memset(&record, 0, sizeof(record));
    memset(&completed_snapshot, 0, sizeof(completed_snapshot));
    completed = 0;
    retry_after = 0;
    retry_window = 0;
    retry_count = 0;
}
