#define _POSIX_C_SOURCE 200809L
#include "feature_provenance.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef LE_FEATURE_PROVENANCE_TESTING
int le_feature_provenance_test_persistable(const char *path);
void le_feature_provenance_test_cache(const char *path, const struct stat *st, int ok,
                                      const char *hash);
int le_feature_provenance_test_cached(const char *path, const struct stat *st);
int le_feature_provenance_test_cache_slots(void);
int le_feature_provenance_test_runtime_reserve(void);
void le_feature_provenance_test_persist_load(void);
void le_feature_provenance_test_persist_store(void);
#endif

static char root_template[] = "/tmp/libreecho-feature-test-XXXXXX";
static char root[sizeof(root_template)];
static char feature_root[1024];
static char update_root[1024];
static char run_root[1024];

static void mkdir_or_die(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void join_path(char *out, size_t size, const char *base, const char *suffix)
{
    size_t base_len = strlen(base), suffix_len = strlen(suffix);
    assert(base_len + suffix_len + 1 < size);
    memcpy(out, base, base_len);
    memcpy(out + base_len, suffix, suffix_len + 1);
}

static void setup(void)
{
    strcpy(root, root_template);
    assert(mkdtemp(root));
    snprintf(feature_root, sizeof(feature_root), "%s/features", root);
    snprintf(update_root, sizeof(update_root), "%s/update", root);
    snprintf(run_root, sizeof(run_root), "%s/run", root);
    mkdir_or_die(feature_root);
    mkdir_or_die(update_root);
    mkdir_or_die(run_root);
    setenv("LIBREECHO_FEATURE_ROOT", feature_root, 1);
    setenv("LIBREECHO_UPDATE_ROOT", update_root, 1);
    setenv("LIBREECHO_FEATURE_RUN_ROOT", run_root, 1);
}

static void make_feature_dir(const char *id)
{
    char path[1024];
    join_path(path, sizeof(path), feature_root, "/");
    assert(strlen(path) + strlen(id) + 1 < sizeof(path));
    strcat(path, id);
    mkdir_or_die(path);
}

static void assert_contains(const char *json, const char *text)
{
    assert(strstr(json, text) != NULL);
}

static void assert_component_contains(const char *json, const char *id,
                                      const char *text)
{
    char marker[64];
    const char *start, *end, *match;
    snprintf(marker, sizeof(marker), "{\"feature_id\":\"%s\"", id);
    start = strstr(json, marker);
    assert(start);
    end = strchr(start, '}');
    assert(end);
    match = strstr(start, text);
    if (!(match && match < end)) {
        fprintf(stderr, "component %s missing %s in %.*s\\n", id, text,
                (int)(end - start + 1), start);
        abort();
    }
}

static void render(char *out, size_t size)
{
    le_feature_transaction_state transaction;
    int i;
    le_feature_transaction_state_read(&transaction);
    le_feature_components_json(out, size, &transaction);
    for (i = 0; i < 512; ++i) {
        le_feature_provenance_tick();
        le_feature_transaction_state_read(&transaction);
        le_feature_components_json(out, size, &transaction);
    }
    assert(out[0] == '[');
}

static void render_once(char *out, size_t size)
{
    le_feature_transaction_state transaction;
    le_feature_transaction_state_read(&transaction);
    le_feature_components_json(out, size, &transaction);
    assert(out[0] == '[');
}

/* Issue #355: OTA v3 (libreecho-generation-transaction) writes schema-3
   pending/installed/rolled-back records with no phase= key. */
static void test_v3_transaction_records(void)
{
    static const char *invalid[] = {
        "schema=3\nslot=b\nmanifest_sha256=abc\n",
        "schema=3\nslot=c\ntransaction_id=txn-v3\nmanifest_sha256=abc\n",
        "schema=3\nslot=b\ntransaction_id=txn-v3\n",
        "schema=4\nslot=b\ntransaction_id=txn-v3\nmanifest_sha256=abc\n",
        "slot=b\ntransaction_id=txn-v3\nmanifest_sha256=abc\n"
    };
    char installed[1024], pending[1024], rolled_back[1024];
    le_feature_transaction_state transaction;
    size_t i;
    setup();
    join_path(installed, sizeof(installed), update_root, "/installed");
    join_path(pending, sizeof(pending), update_root, "/pending");
    join_path(rolled_back, sizeof(rolled_back), update_root, "/rolled-back");

    /* Committed install, as observed on a 0.14.0 device. */
    write_file(installed, "schema=3\nslot=b\ntransaction_id=txn-543403a62e22a333a651fb52\n"
               "version=0.14.0\nupdate_channel=stable\nmanifest_sha256=abc\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "none"));
    assert(!strcmp(transaction.last_result, "installed"));

    /* Earlier rollback history does not override the committed install. */
    write_file(rolled_back, "schema=3\nslot=a\ntransaction_id=txn-24e9c79ebdda247477a1a0d2\nmanifest_sha256=def\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "none"));
    assert(!strcmp(transaction.last_result, "installed"));

    /* Candidate booted and awaiting commit. */
    write_file(pending, "schema=3\nslot=a\ntransaction_id=txn-candidate\nmanifest_sha256=fed\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "pending"));
    assert(!strcmp(transaction.last_result, "pending"));
    assert(transaction.pending && !transaction.reboot_required);
    assert(unlink(pending) == 0);

    /* Rollback with no committed install yet. */
    assert(unlink(installed) == 0);
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "rollback"));
    assert(!strcmp(transaction.last_result, "rolled-back"));
    assert(transaction.rollback);

    /* Malformed schema-3 records still fail closed. */
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        write_file(installed, invalid[i]);
        le_feature_transaction_state_read(&transaction);
        assert(!strcmp(transaction.state, "unknown"));
        assert(!strcmp(transaction.last_result, "unknown"));
        write_file(pending, invalid[i]);
        assert(unlink(installed) == 0);
        le_feature_transaction_state_read(&transaction);
        assert(!strcmp(transaction.state, "unknown"));
        assert(unlink(pending) == 0);
        write_file(rolled_back, invalid[i]);
        le_feature_transaction_state_read(&transaction);
        assert(!strcmp(transaction.state, "unknown"));
        assert(!transaction.rollback);
    }
    assert(unlink(rolled_back) == 0);
    le_feature_provenance_shutdown();
}

static void write_v3_feature(const char *generations, const char *tx);

static void test_incremental_hash_progress(void)
{
    /* A tick hashes many bounded chunks (time/step budgeted), so a payload
       that is far larger than one chunk completes within a few ticks rather
       than one 16 KiB chunk per event-loop wakeup. */
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    int fd, ticks;
    setup();
    make_feature_dir("assistant");
    join_path(path, sizeof(path), feature_root, "/assistant/manifest.json");
    write_file(path, "{\"schema_version\":1,\"feature_id\":\"assistant\",\"format\":\"squashfs-lz4\",\"product_release\":\"radar-puffin-v0.13.11\",\"source_commit\":\"0123456789012345678901234567890123456789\",\"payload\":{\"filename\":\"assistant.squashfs\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},\"files\":{}}\n");
    join_path(path, sizeof(path), feature_root, "/assistant/payload.squashfs");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, 64L * 1024L * 1024L) == 0); /* 4096 chunks */
    assert(close(fd) == 0);

    render_once(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"pending\"");
    for (ticks = 0; ticks < 64; ++ticks) {
        le_feature_provenance_tick();
        render_once(output, sizeof(output));
        if (!strstr(output, "\"effective\":\"pending\""))
            break;
    }
    /* With the time budget lifted for tests, each tick runs exactly the
       256-step cap: the manifest read, then 4096 payload chunks plus EOF,
       so the job settles on tick 17 (index 16). One chunk per tick would
       need 4096 ticks. */
    assert(ticks == 16);
    assert_component_contains(output, "assistant", "\"effective\":\"mismatch\"");
    le_feature_provenance_shutdown();
}

static void write_record(const char *cache, const char *mode, const struct stat *st,
                         const char *digest, const char *path)
{
    FILE *f = fopen(cache, mode);
    assert(f);
    assert(fprintf(f, "v1 %llu %llu %llu %lld %ld %lld %ld %s %s\n",
                   (unsigned long long)st->st_dev, (unsigned long long)st->st_ino,
                   (unsigned long long)st->st_size, (long long)st->st_mtim.tv_sec,
                   (long)st->st_mtim.tv_nsec, (long long)st->st_ctim.tv_sec,
                   (long)st->st_ctim.tv_nsec, digest, path) > 0);
    assert(fclose(f) == 0);
}

static void plant_record(const char *cache, const struct stat *st, const char *digest,
                         const char *path)
{
    unlink(cache);
    write_record(cache, "w", st, digest, path);
}

static const char *output_for_restart(char *out, size_t size)
{
    render_once(out, size);
    return out;
}

static void count_ticks_to_settle(const char *id, const char *expected, int *ticks_out)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    int ticks;
    render_once(output, sizeof(output));
    for (ticks = 0; ticks < 4096 && strstr(output, "\"effective\":\"pending\""); ++ticks) {
        le_feature_provenance_tick();
        render_once(output, sizeof(output));
    }
    if (expected)
        assert_component_contains(output, id, expected);
    else
        assert(!strstr(output, "\"effective\":\"pending\""));
    *ticks_out = ticks;
}

static void test_generation_hash_persists_across_restart(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX], generations[1024], path[1024],
         cache[1024], payload[1200];
    struct stat st;
    int ticks;
    FILE *f;
    setup();
    unsetenv("LIBREECHO_FEATURE_ROOT");
    snprintf(generations, sizeof(generations), "%s/generations", root);
    mkdir_or_die(generations);
    setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
    write_v3_feature(generations, "txn-0123456789abcdef");
    join_path(path, sizeof(path), update_root, "/current");
    write_file(path, "txn-0123456789abcdef\n");
    join_path(cache, sizeof(cache), update_root, "/feature-hash-cache");

    /* First boot of a generation hashes and records the digest. */
    count_ticks_to_settle("assistant", "\"effective\":\"present\"", &ticks);
    assert(ticks > 0);
    assert(stat(cache, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    le_feature_provenance_shutdown();

    /* After a restart the stored digest is used instead of re-hashing: plant
       a record carrying a different digest for the payload's exact identity
       and observe it reported (a re-hash would report the real digest). */
    snprintf(payload, sizeof(payload), "%s/txn-0123456789abcdef/features/assistant/payload.squashfs", generations);
    assert(stat(payload, &st) == 0);
    plant_record(cache, &st, "1111111111111111111111111111111111111111111111111111111111111111", payload);
    assert(chmod(cache, 0600) == 0);
    /* The real payload matches its manifest, so "mismatch" here proves the
       stored digest was used rather than the file being re-hashed. */
    count_ticks_to_settle("assistant", "\"effective\":\"mismatch\"", &ticks);
    le_feature_provenance_shutdown();

    /* The genuine record written on first boot reports present again. */
    assert(unlink(cache) == 0);
    count_ticks_to_settle("assistant", "\"effective\":\"present\"", &ticks);
    le_feature_provenance_shutdown();
    render_once(output, sizeof(output));
    count_ticks_to_settle("assistant", "\"effective\":\"present\"", &ticks);
    assert_component_contains(output_for_restart(output, sizeof(output)), "assistant", "\"effective_payload_sha256\":\"0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69\"");
    le_feature_provenance_shutdown();

    /* Any change to the payload's identity invalidates the stored digest. */
    assert(chmod(payload, 0600) == 0); /* ctime changes */
    f = fopen(payload, "ab");
    assert(f && fputs("x", f) >= 0 && fclose(f) == 0);
    count_ticks_to_settle("assistant", "\"effective\":\"mismatch\"", &ticks);
    assert(ticks > 0);
    le_feature_provenance_shutdown();

    /* Untrusted caches are ignored. Each case plants a well-formed record for
       the payload's exact current identity carrying the manifest digest, so
       the payload (modified above) would wrongly read "present" if trusted. */
    {
        static const char *good = "0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69";
        char real_cache[1200];
        assert(stat(payload, &st) == 0);
        plant_record(cache, &st, good, payload);
        assert(chmod(cache, 0600) == 0);
        /* Control: a private, owned, regular cache is trusted ("present"
           although the bytes no longer match; the manifest read still takes
           one tick, so the result, not the tick count, is the discriminator). */
        count_ticks_to_settle("assistant", "\"effective\":\"present\"", &ticks);
        le_feature_provenance_shutdown();

        plant_record(cache, &st, good, payload);
        assert(chmod(cache, 0666) == 0); /* group/world writable */
        count_ticks_to_settle("assistant", "\"effective\":\"mismatch\"", &ticks);
        assert(ticks > 0);
        le_feature_provenance_shutdown();

        plant_record(cache, &st, good, payload);
        assert(chmod(cache, 0620) == 0); /* group writable */
        count_ticks_to_settle("assistant", "\"effective\":\"mismatch\"", &ticks);
        assert(ticks > 0);
        le_feature_provenance_shutdown();

        snprintf(real_cache, sizeof(real_cache), "%s.real", cache);
        unlink(real_cache);
        plant_record(real_cache, &st, good, payload);
        assert(chmod(real_cache, 0600) == 0);
        unlink(cache);
        assert(symlink(real_cache, cache) == 0); /* linked cache */
        count_ticks_to_settle("assistant", "\"effective\":\"mismatch\"", &ticks);
        assert(ticks > 0);
        le_feature_provenance_shutdown();
        unlink(cache);
        unlink(real_cache);

        /* Digests are only persisted for paths strictly inside the
           generations root: a prefix-confused sibling and a dot-dot path
           that escapes the root are hashed but never written to the cache. */
        {
            static const char *roots[] = { "-evil/txn/features", "/../outside/features" };
            char feature_dir[1400], file_path[1500];
            size_t r;
            char made[1400];
            snprintf(made, sizeof(made), "%s-evil", generations); mkdir_or_die(made);
            snprintf(made, sizeof(made), "%s-evil/txn", generations); mkdir_or_die(made);
            snprintf(made, sizeof(made), "%s/outside", root); mkdir_or_die(made);
            for (r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
                snprintf(feature_dir, sizeof(feature_dir), "%s%s", generations, roots[r]);
                mkdir_or_die(feature_dir);
                snprintf(file_path, sizeof(file_path), "%s/assistant", feature_dir); mkdir_or_die(file_path);
                snprintf(file_path, sizeof(file_path), "%s/assistant/manifest.json", feature_dir);
                write_file(file_path, "{\"schema_version\":1,\"feature_id\":\"assistant\",\"format\":\"squashfs-lz4\",\"payload\":{\"filename\":\"assistant.squashfs\",\"sha256\":\"0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69\"},\"files\":{}}\n");
                snprintf(file_path, sizeof(file_path), "%s/assistant/payload.squashfs", feature_dir);
                write_file(file_path, "libreecho v3 payload\n");
                setenv("LIBREECHO_FEATURE_ROOT", feature_dir, 1);
                unlink(cache);
                /* The sibling root hashes to "present"; the dot-dot root is
                   refused outright by the path-safety check. */
                count_ticks_to_settle("assistant", r == 0 ? "\"effective\":\"present\"" : NULL, &ticks);
                assert(stat(cache, &st) != 0);
                le_feature_provenance_shutdown();
            }
            unsetenv("LIBREECHO_FEATURE_ROOT");
        }
    }
    unsetenv("LIBREECHO_GENERATIONS_ROOT");
}

static void test_flat_feature_tree_is_not_persisted(void)
{
    char path[1024], cache[1024];
    struct stat st;
    int ticks;
    setup();
    make_feature_dir("assistant");
    join_path(path, sizeof(path), feature_root, "/assistant/manifest.json");
    write_file(path, "{\"schema_version\":1,\"feature_id\":\"assistant\",\"format\":\"squashfs-lz4\",\"product_release\":\"radar-puffin-v0.13.11\",\"source_commit\":\"0123456789012345678901234567890123456789\",\"payload\":{\"filename\":\"assistant.squashfs\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},\"files\":{}}\n");
    join_path(path, sizeof(path), feature_root, "/assistant/payload.squashfs");
    write_file(path, "payload");
    count_ticks_to_settle("assistant", "\"effective\":\"mismatch\"", &ticks);
    join_path(cache, sizeof(cache), update_root, "/feature-hash-cache");
    assert(stat(cache, &st) != 0); /* mutable v2 tree: never persisted */
    le_feature_provenance_shutdown();
}

static void test_canonical_hash_and_tamper_states(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    render(output, sizeof(output));
    assert_contains(output, "\"feature_id\":\"airplay2\"");
    assert_contains(output, "\"feature_id\":\"tts\"");
    assert_contains(output, "\"feature_id\":\"wakeword\"");
    assert_contains(output, "\"feature_id\":\"stt\"");
    assert_contains(output, "\"feature_id\":\"assistant\"");
    assert_contains(output, "\"source_commit\":\"unavailable\"");
    assert_contains(output, "\"effective\":\"missing\"");

    make_feature_dir("airplay2");
    join_path(path, sizeof(path), feature_root, "/airplay2/manifest.json");
    write_file(path, "{\"schema_version\":1,\"feature_id\":\"airplay2\",\"format\":\"squashfs-lz4\",\"payload\":{\"filename\":\"airplay2.squashfs\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\",\"size\":3},\"files\":{}}\n");
    join_path(path, sizeof(path), feature_root, "/airplay2/payload.squashfs");
    write_file(path, "abc");
    render(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective_payload_sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"");
    assert_component_contains(output, "airplay2", "\"effective\":\"present\"");
    assert_component_contains(output, "airplay2", "\"candidate_status\":\"missing\"");

    /* Same-size corruption must not retain the manifest identity. */
    write_file(path, "abd");
    render(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective_payload_sha256\":\"unavailable\"");
    assert_component_contains(output, "airplay2", "\"effective\":\"mismatch\"");
    assert_component_contains(output, "airplay2", "\"source_commit\":\"unavailable\"");

    unlink(path);
    render(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective_payload_sha256\":\"unavailable\"");
    assert_component_contains(output, "airplay2", "\"effective\":\"missing\"");
}

static void write_sized_control_manifest(const char *path, size_t target);

static void test_runtime_and_candidate_identity(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    char pid[32];
    setup();
    make_feature_dir("tts");
    join_path(path, sizeof(path), feature_root, "/tts/runtime-manifest.json");
    write_file(path, "{\"schema_version\":1,\"format\":\"squashfs-lz4\",\"base_payload_sha256\":\"141c6aaa77c6099e5e21ce8fd0ac457fa46f1bae811d8995d038a5000de0ffa7\",\"feature_id\":\"tts\",\"files\":{},\"payload\":{\"filename\":\"runtime.squashfs\",\"sha256\":\"365f9c5839a00baf3e84333994e5742723c9f87b1a232aa27fa65d67fc75f46a\"},\"product_release\":\"radar-puffin-v0.13.11\\\"fixture\",\"source_commit\":\"0123456789012345678901234567890123456789\"}\n");
    join_path(path, sizeof(path), feature_root, "/tts/payload.squashfs");
    write_file(path, "tts canonical base payload\n");
    join_path(path, sizeof(path), feature_root, "/tts/runtime.squashfs");
    write_file(path, "tts canonical runtime capsule\n");
    join_path(path, sizeof(path), run_root, "/libreecho-ttsd.pid");
    snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    write_file(path, pid);

    join_path(path, sizeof(path), update_root, "/staging");
    mkdir_or_die(path);
    join_path(path, sizeof(path), update_root, "/staging/features");
    mkdir_or_die(path);
    join_path(path, sizeof(path), update_root, "/staging/features/tts");
    mkdir_or_die(path);
    join_path(path, sizeof(path), update_root, "/staging/features/tts/libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs");
    write_file(path, "candidate runtime capsule\n");
    join_path(path, sizeof(path), update_root, "/staging/manifest");
    write_file(path, "feature_tts_action=runtime\nfeature_tts_asset=libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs\nfeature_tts_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nfeature_tts_activation=reboot\n");
    join_path(path, sizeof(path), update_root, "/feature-commit");
    write_file(path, "transaction_id=txn-live\nphase=prepared\n");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"release\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"source_commit\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"effective_payload_sha256\":\"141c6aaa77c6099e5e21ce8fd0ac457fa46f1bae811d8995d038a5000de0ffa7\"");
    assert_component_contains(output, "tts", "\"runtime_capsule_sha256\":\"365f9c5839a00baf3e84333994e5742723c9f87b1a232aa27fa65d67fc75f46a\"");
    assert_component_contains(output, "tts", "\"candidate_kind\":\"runtime\"");
    assert_component_contains(output, "tts", "\"candidate_status\":\"mismatch\"");
    assert_component_contains(output, "tts", "\"candidate_payload_sha256\":\"c5c3ff29b47990e0984b25aacfc36149cc597fe77b43538a23fd69fd547e3b67\"");
    assert_component_contains(output, "tts", "\"effective\":\"present\"");
    assert_component_contains(output, "tts", "\"activation\":\"reboot\"");
    assert_component_contains(output, "tts", "\"last_transaction_result\":\"commit-pending\"");

    join_path(path, sizeof(path), update_root, "/staging/manifest");
    write_file(path, "feature_tts_asset=libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs\nfeature_tts_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_kind\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"candidate_status\":\"unavailable\"");
    write_sized_control_manifest(path, 65536);
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_kind\":\"runtime\"");
    assert_component_contains(output, "tts", "\"candidate_status\":\"mismatch\"");
    write_sized_control_manifest(path, 65537);
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_kind\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"candidate_status\":\"unavailable\"");

    /* A missing runtime capsule prevents effective=present. */
    join_path(path, sizeof(path), feature_root, "/tts/runtime.squashfs");
    unlink(path);
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"runtime_capsule_sha256\":null");
    assert_component_contains(output, "tts", "\"effective\":\"missing\"");
}

static void write_exact_runtime_manifest(const char *path)
{
    FILE *f = fopen(path, "w");
    size_t i;
    assert(f);
    assert(fputc('{', f) != EOF);
    for (i = 0; i < 65536 - 2; ++i)
        assert(fputc(' ', f) != EOF);
    assert(fputc('}', f) != EOF);
    assert(fclose(f) == 0);
}

static void test_runtime_metadata_boundaries_fail_closed(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    make_feature_dir("tts");
    join_path(path, sizeof(path), feature_root, "/tts/manifest.json");
    write_file(path, "{\"feature_id\":\"tts\",\"payload\":{\"filename\":\"payload.squashfs\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}}\n");
    join_path(path, sizeof(path), feature_root, "/tts/payload.squashfs");
    write_file(path, "abc");
    join_path(path, sizeof(path), feature_root, "/tts/runtime.squashfs");
    write_file(path, "runtime capsule");

    /* A runtime capsule without runtime metadata must not become base evidence. */
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"effective_payload_sha256\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"effective\":\"unavailable\"");

    join_path(path, sizeof(path), feature_root, "/tts/runtime-manifest.json");
    write_exact_runtime_manifest(path);
    assert((long)strlen("{") + (65536 - 2) + strlen("}") == 65536);
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"release\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"effective_payload_sha256\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"effective\":\"unavailable\"");

    unlink(path);
    assert(mkdir(path, 0700) == 0);
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"effective_payload_sha256\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"effective\":\"unavailable\"");
    le_feature_provenance_shutdown();
}

static void test_malformed_manifest_keeps_ids_unavailable(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    make_feature_dir("tts");
    join_path(path, sizeof(path), feature_root, "/tts/runtime-manifest.json");
    write_file(path, "{\"feature_id\":\"tts\",\"product_release\":\"");
    render(output, sizeof(output));
    assert_contains(output, "\"release\":\"unavailable\"");
    assert_contains(output, "\"source_commit\":\"unavailable\"");
    assert_contains(output, "\"effective_payload_sha256\":\"unavailable\"");
    assert_component_contains(output, "tts", "\"runtime_capsule_sha256\":null");
}

static void write_sized_control_manifest(const char *path, size_t target)
{
    static const char prefix[] =
        "feature_tts_action=runtime\n"
        "feature_tts_asset=libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs\n"
        "feature_tts_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "feature_tts_activation=reboot\n"
        "feature_tts_release=";
    FILE *f = fopen(path, "w");
    size_t i;
    assert(f);
    assert(target > sizeof(prefix) - 1);
    assert(fputs(prefix, f) >= 0);
    for (i = sizeof(prefix) - 1; i < target; ++i)
        assert(fputc('x', f) != EOF);
    assert(fclose(f) == 0);
}

static void write_sized_base_manifest(const char *path, const char *feature,
                                      const char *filename, size_t target)
{
    FILE *f = fopen(path, "w");
    char prefix[512], tail[512];
    size_t prefix_len, tail_len, padding;
    assert(f);
    assert(snprintf(prefix, sizeof(prefix),
                    "{\"feature_id\":\"%s\",\"files\":{\"etc/fixture\":{\"mode\":\"0644\",\"sha256\":\"%064d\",\"size\":1},\"padding\":\"",
                    feature, 0) > 0);
    assert(snprintf(tail, sizeof(tail),
                    "\"},\"format\":\"squashfs-lz4\",\"payload\":{\"filename\":\"%s\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\",\"size\":3},\"schema_version\":1}\n",
                    filename) > 0);
    prefix_len = strlen(prefix);
    tail_len = strlen(tail);
    assert(target > prefix_len + tail_len);
    padding = target - prefix_len - tail_len;
    assert(fputs(prefix, f) >= 0);
    while (padding--)
        assert(fputc(' ', f) != EOF);
    assert(fputs(tail, f) >= 0);
    assert(fclose(f) == 0);
}

static void test_manifest_replacement_during_incremental_read(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    make_feature_dir("airplay2");
    join_path(path, sizeof(path), feature_root, "/airplay2/manifest.json");
    write_sized_base_manifest(path, "airplay2", "airplay2.squashfs", 139043);
    join_path(path, sizeof(path), feature_root, "/airplay2/payload.squashfs");
    write_file(path, "abc");
    render_once(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective\":\"pending\"");
    join_path(path, sizeof(path), feature_root, "/airplay2/manifest.json");
    write_file(path, "{}");
    render(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective\":\"unavailable\"");
    le_feature_provenance_shutdown();
}

static void test_production_manifest_bounds_and_filename_allowlist(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    make_feature_dir("airplay2");
    join_path(path, sizeof(path), feature_root, "/airplay2/manifest.json");
    write_sized_base_manifest(path, "airplay2", "airplay2.squashfs", 139043);
    join_path(path, sizeof(path), feature_root, "/airplay2/payload.squashfs");
    write_file(path, "abc");
    render(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective\":\"present\"");

    join_path(path, sizeof(path), feature_root, "/airplay2/manifest.json");
    write_sized_base_manifest(path, "airplay2", "payload.squashfs", 139043);
    render(output, sizeof(output));
    assert_component_contains(output, "airplay2", "\"effective\":\"unavailable\"");

    join_path(path, sizeof(path), feature_root, "/tts");
    make_feature_dir("tts");
    join_path(path, sizeof(path), feature_root, "/tts/manifest.json");
    write_sized_base_manifest(path, "tts", "tts.squashfs", 164016);
    join_path(path, sizeof(path), feature_root, "/tts/payload.squashfs");
    write_file(path, "abc");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"effective\":\"present\"");

    join_path(path, sizeof(path), feature_root, "/tts/manifest.json");
    write_sized_base_manifest(path, "tts", "tts.squashfs", 262145);
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"effective\":\"unavailable\"");
    le_feature_provenance_shutdown();
}

static void assert_invalid_record_unknown(const char *name, int kind)
{
    char path[1024];
    size_t root_len = strlen(update_root), name_len = strlen(name);
    int fd;
    le_feature_transaction_state transaction;
    assert(root_len + name_len + 2 < sizeof(path));
    memcpy(path, update_root, root_len);
    path[root_len] = '/';
    memcpy(path + root_len + 1, name, name_len + 1);
    unlink(path);
    if (kind == 0) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        assert(fd >= 0);
        assert(ftruncate(fd, 8192U + 1U) == 0);
        assert(close(fd) == 0);
    } else if (kind == 1) {
        assert(mkfifo(path, 0600) == 0);
    } else {
        assert(symlink("missing-record-target", path) == 0);
    }
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "unknown"));
    assert(!strcmp(transaction.last_result, "unknown"));
    assert(unlink(path) == 0);
}

static void test_invalid_transaction_records_fail_closed(void)
{
    static const char *names[] = {"pending", "feature-commit", "installed", "rolled-back"};
    size_t i;
    int kind;
    setup();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        for (kind = 0; kind < 3; ++kind)
            assert_invalid_record_unknown(names[i], kind);
    le_feature_provenance_shutdown();
}

static void test_rollback_record_validation(void)
{
    static const char *invalid[] = {
        "", "not a transaction record\n", "phase=prepared\n",
        "transaction_id=txn-failed\n",
        "transaction_id=\nphase=prepared\n",
        "transaction_id=txn-failed\nphase=corrupt\n",
        "transaction_id=txn-failed\nphase=installed\n",
        "transaction_id=txn-failed\nphase=confirmed\n"
    };
    char path[1024];
    size_t i;
    le_feature_transaction_state transaction;
    setup();
    join_path(path, sizeof(path), update_root, "/rolled-back");
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        write_file(path, invalid[i]);
        le_feature_transaction_state_read(&transaction);
        assert(!strcmp(transaction.state, "unknown"));
        assert(!strcmp(transaction.last_result, "unknown"));
        assert(!transaction.rollback);
    }
    /* Platform record_fallback() copies the prepared pending record verbatim. */
    write_file(path, "schema=2\ntransaction_id=txn-failed\nversion=0.13.14\n"
               "slot=B\nfeature_ids=tts\nphase=prepared\nstate=reboot-pending\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "rollback"));
    assert(!strcmp(transaction.last_result, "rolled-back"));
    assert(transaction.rollback);
    assert(!transaction.pending && !transaction.commit_pending && !transaction.reboot_required);
    assert(unlink(path) == 0);
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "none"));
    assert(!strcmp(transaction.last_result, "idle"));
    le_feature_provenance_shutdown();
}

static void test_candidate_control_namespace_fails_closed(void)
{
    static const char production_control[] =
        "format=libreecho-ota-v2\n"
        "manifest_version=1\n"
        "board=radar_puffin\n"
        "soc=mt8163\n"
        "architecture=armv7\n"
        "image_profile=ota\n"
        "transaction_type=system\n"
        "transaction_id=txn-fixture\n"
        "version=0.13.14\n"
        "update_channel=stable\n"
        "service_profile=production\n"
        "feature_policy=preserve\n"
        "minimum_updater_schema=2\n"
        "feature_asset_base=github-release-channel\n"
        "commit_policy=after-slot-confirm\n"
        "feature_ids=airplay2,tts,wakeword,stt,assistant\n"
        "boot_filename=boot.img\n"
        "boot_size=16777216\n"
        "boot_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "feature_airplay2_action=preserve\n"
        "feature_airplay2_activation=reboot\n"
        "feature_airplay2_base_payload_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "feature_airplay2_base_manifest_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "feature_airplay2_daemon_path=usr/local/sbin/libreecho-audio-engine\n"
        "feature_airplay2_daemon_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "feature_airplay2_release=0.13.14\n"
        "feature_airplay2_source_commit=1111111111111111111111111111111111111111\n"
        "feature_tts_action=runtime\n"
        "feature_tts_asset=libreecho-radar-puffin-0.13.14-tts.runtime.squashfs\n"
        "feature_tts_size=3\n"
        "feature_tts_sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
        "feature_tts_manifest_asset=libreecho-radar-puffin-0.13.14-tts.runtime-manifest.json\n"
        "feature_tts_manifest_size=128\n"
        "feature_tts_manifest_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\n"
        "feature_tts_activation=reboot\n"
        "feature_tts_base_payload_sha256=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\n"
        "feature_tts_base_manifest_sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n"
        "feature_tts_daemon_path=usr/local/sbin/libreecho-ttsd\n"
        "feature_tts_daemon_sha256=9999999999999999999999999999999999999999999999999999999999999999\n"
        "feature_tts_release=0.13.14\n"
        "feature_tts_source_commit=2222222222222222222222222222222222222222\n"
        "feature_wakeword_action=preserve\n"
        "feature_wakeword_activation=reboot\n"
        "feature_wakeword_base_payload_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "feature_wakeword_base_manifest_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "feature_wakeword_daemon_path=usr/local/sbin/libreecho-waked\n"
        "feature_wakeword_daemon_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "feature_wakeword_release=0.13.14\n"
        "feature_wakeword_source_commit=3333333333333333333333333333333333333333\n"
        "feature_stt_action=preserve\n"
        "feature_stt_activation=reboot\n"
        "feature_stt_base_payload_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "feature_stt_base_manifest_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "feature_stt_daemon_path=usr/local/sbin/libreecho-sttd\n"
        "feature_stt_daemon_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "feature_stt_release=0.13.14\n"
        "feature_stt_source_commit=4444444444444444444444444444444444444444\n"
        "feature_assistant_action=preserve\n"
        "feature_assistant_activation=reboot\n"
        "feature_assistant_base_payload_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
        "feature_assistant_base_manifest_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
        "feature_assistant_daemon_path=usr/local/sbin/libreecho-agentd\n"
        "feature_assistant_daemon_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n"
        "feature_assistant_release=0.13.14\n"
        "feature_assistant_source_commit=5555555555555555555555555555555555555555\n";
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_status\":\"missing\"");

    join_path(path, sizeof(path), update_root, "/staging");
    mkdir_or_die(path);
    join_path(path, sizeof(path), update_root, "/staging/features");
    mkdir_or_die(path);
    join_path(path, sizeof(path), update_root, "/staging/features/tts");
    mkdir_or_die(path);
    join_path(path, sizeof(path), update_root, "/staging/manifest");
    write_file(path, "feature_tts_acton=runtime\n");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_status\":\"unavailable\"");

    write_file(path, "feature_tts_action\n");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_status\":\"unavailable\"");

    write_file(path, production_control);
    join_path(path, sizeof(path), update_root,
              "/staging/features/tts/libreecho-radar-puffin-0.13.14-tts.runtime.squashfs");
    write_file(path, "abc");
    join_path(path, sizeof(path), update_root, "/staging/manifest");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_kind\":\"runtime\"");
    assert_component_contains(output, "tts", "\"candidate_status\":\"present\"");
    assert_component_contains(output, "tts", "\"candidate_payload_sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"");

    write_file(path, "feature_unknown_action=runtime\n");
    render(output, sizeof(output));
    assert_component_contains(output, "tts", "\"candidate_status\":\"unavailable\"");
    le_feature_provenance_shutdown();
}

static void test_authority_identity_and_transaction_precedence(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    setup();
    make_feature_dir("assistant");
    join_path(path, sizeof(path), feature_root, "/assistant/manifest.json");
    write_file(path, "{\"schema_version\":1,\"feature_id\":\"assistant\",\"format\":\"squashfs-lz4\",\"payload\":{\"filename\":\"assistant.squashfs\",\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\",\"size\":3},\"files\":{}}\n");
    join_path(path, sizeof(path), feature_root, "/assistant/payload.squashfs");
    write_file(path, "abc");
    join_path(path, sizeof(path), update_root, "/committed-manifest");
    write_file(path, "format=libreecho-ota-v2\ntransaction_id=txn-fixture\nfeature_ids=assistant\nfeature_assistant_action=preserve\nfeature_assistant_activation=reboot\nfeature_assistant_base_payload_sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\nfeature_assistant_base_manifest_sha256=e9991ec699a4a39666759187d01a28c7aad8918b3262c4e851c55a62ecd228b1\nfeature_assistant_daemon_path=usr/local/sbin/libreecho-agentd\nfeature_assistant_daemon_sha256=1111111111111111111111111111111111111111111111111111111111111111\nfeature_assistant_release=0.13.13\nfeature_assistant_source_commit=2222222222222222222222222222222222222222\n");
    join_path(path, sizeof(path), update_root, "/installed");
    write_file(path, "schema=2\ntransaction_id=txn-fixture\nphase=installed\nmanifest_sha256=fixture\n");
    render(output, sizeof(output));
    /* An unsigned committed manifest is not verified provenance authority. */
    assert_component_contains(output, "assistant", "\"release\":\"unavailable\"");
    assert_component_contains(output, "assistant", "\"source_commit\":\"unavailable\"");

    join_path(path, sizeof(path), update_root, "/rolled-back");
    write_file(path, "transaction_id=txn-old\nphase=prepared\n");
    le_feature_transaction_state transaction;
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "none"));
    assert(!strcmp(transaction.last_result, "installed"));

    join_path(path, sizeof(path), update_root, "/installed");
    write_file(path, "transaction_id=txn-live\nphase=corrupt\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "unknown"));
    assert(!strcmp(transaction.last_result, "unknown"));
    write_file(path, "schema=2\ntransaction_id=txn-fixture\nphase=installed\nmanifest_sha256=fixture\n");

    join_path(path, sizeof(path), update_root, "/pending");
    write_file(path, "transaction_id=txn-live\nphase=prepared\nstate=reboot-pending\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "pending"));
    assert(!strcmp(transaction.last_result, "reboot-required"));

    join_path(path, sizeof(path), update_root, "/feature-commit");
    write_file(path, "transaction_id=txn-live\nphase=prepared\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "commit"));
    assert(!strcmp(transaction.last_result, "commit-pending"));

    write_file(path, "transaction_id=txn-other\nphase=prepared\n");
    le_feature_transaction_state_read(&transaction);
    assert(!strcmp(transaction.state, "unknown"));
    assert(!strcmp(transaction.last_result, "unknown"));
    le_feature_provenance_shutdown();
}


static void write_v3_feature(const char *generations, const char *tx)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", generations, tx); mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/%s/features", generations, tx); mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/%s/features/assistant", generations, tx); mkdir_or_die(path);
    snprintf(path, sizeof(path), "%s/%s/features/assistant/manifest.json", generations, tx);
    write_file(path, "{\"schema_version\":1,\"feature_id\":\"assistant\",\"format\":\"squashfs-lz4\",\"payload\":{\"filename\":\"assistant.squashfs\",\"sha256\":\"0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69\"},\"files\":{}}\n");
    snprintf(path, sizeof(path), "%s/%s/features/assistant/payload.squashfs", generations, tx);
    write_file(path, "libreecho v3 payload\n");
}

/* OTA v3 commits features into generations/<update/current>/features; the v2
   flat feature root is empty on such devices and must not read as "missing". */
static void test_v3_generation_feature_root(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX], generations[1024], path[1024];
    setup();
    snprintf(generations, sizeof(generations), "%s/generations", root);
    mkdir_or_die(generations);
    setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
    write_v3_feature(generations, "txn-0123456789abcdef");
    join_path(path, sizeof(path), update_root, "/current");
    write_file(path, "txn-0123456789abcdef\n");

    /* An explicit feature root still wins (fixtures and v2 devices). */
    render(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"missing\"");

    unsetenv("LIBREECHO_FEATURE_ROOT");
    render(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"present\"");
    assert_component_contains(output, "assistant", "\"effective_payload_sha256\":\"0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69\"");

    /* A hostile or malformed current pointer never escapes generations/. */
    write_file(path, "../../etc\n");
    render(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"missing\"");
    write_file(path, "txn-0123456789abcdef\nextra\n");
    render(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"missing\"");
    write_file(path, "txn-absent\n");
    render(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"missing\"");
    /* The updater writes the id with a bare LF; CR is not part of the grammar. */
    write_file(path, "txn-0123456789abcdef\r\n");
    render(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"missing\"");

    /* A linked generation, features tree or generations root is refused even
       when it points at a well-formed generation. */
    {
        char link_path[1200], linked_root[1200];
        write_file(path, "txn-linked\n");
        snprintf(link_path, sizeof(link_path), "%s/txn-linked", generations);
        assert(symlink("txn-0123456789abcdef", link_path) == 0);
        render(output, sizeof(output));
        assert_component_contains(output, "assistant", "\"effective\":\"missing\"");

        write_v3_feature(generations, "txn-features-link");
        snprintf(link_path, sizeof(link_path), "%s/txn-features-link/features", generations);
        snprintf(linked_root, sizeof(linked_root), "%s/txn-features-link/features.real", generations);
        assert(rename(link_path, linked_root) == 0);
        assert(symlink("features.real", link_path) == 0);
        write_file(path, "txn-features-link\n");
        render(output, sizeof(output));
        assert_component_contains(output, "assistant", "\"effective\":\"missing\"");

        snprintf(linked_root, sizeof(linked_root), "%s/generations-link", root);
        assert(symlink(generations, linked_root) == 0);
        setenv("LIBREECHO_GENERATIONS_ROOT", linked_root, 1);
        write_file(path, "txn-0123456789abcdef\n");
        render(output, sizeof(output));
        assert_component_contains(output, "assistant", "\"effective\":\"missing\"");

        /* Control: the same generation through the real root is present. */
        setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
        render(output, sizeof(output));
        assert_component_contains(output, "assistant", "\"effective\":\"present\"");
    }
    unsetenv("LIBREECHO_GENERATIONS_ROOT");
}

#ifdef LE_FEATURE_PROVENANCE_TESTING
static void test_persistable_path_rejects_escapes(void)
{
    char generations[1024], path[1400];
    setup();
    snprintf(generations, sizeof(generations), "%s/generations", root);
    setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
    snprintf(path, sizeof(path), "%s/txn-a/features/assistant/payload.squashfs", generations);
    assert(le_feature_provenance_test_persistable(path));
    snprintf(path, sizeof(path), "%s/txn-a/..", generations);
    assert(!le_feature_provenance_test_persistable(path));
    snprintf(path, sizeof(path), "%s/txn-a/../../outside/payload.squashfs", generations);
    assert(!le_feature_provenance_test_persistable(path));
    snprintf(path, sizeof(path), "%s-evil/txn-a/features/assistant/payload.squashfs", generations);
    assert(!le_feature_provenance_test_persistable(path));
    snprintf(path, sizeof(path), "%s/", generations);
    assert(!le_feature_provenance_test_persistable(path));
    assert(!le_feature_provenance_test_persistable(generations));
    assert(!le_feature_provenance_test_persistable("/proc/1/exe"));
    unsetenv("LIBREECHO_GENERATIONS_ROOT");
    le_feature_provenance_shutdown();
}

static void test_eviction_keeps_persisted_digests(void)
{
    static const char *digest = "0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69";
    char generations[1024], path[1400];
    struct stat st;
    int i, slots = le_feature_provenance_test_cache_slots(), persisted = 5;
    setup();
    snprintf(generations, sizeof(generations), "%s/generations", root);
    setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
    memset(&st, 0, sizeof(st));
    st.st_size = 1;
    for (i = 0; i < persisted; ++i) {
        st.st_ino = (ino_t)(1000 + i);
        snprintf(path, sizeof(path), "%s/txn-a/features/f%d/payload.squashfs", generations, i);
        le_feature_provenance_test_cache(path, &st, 1, digest);
    }
    /* Churn far more non-persisted entries than there are slots: runtime
       executables and failed observations. */
    for (i = 0; i < slots * 4; ++i) {
        st.st_ino = (ino_t)(5000 + i);
        snprintf(path, sizeof(path), "/proc/%d/exe", 100 + i);
        le_feature_provenance_test_cache(path, &st, i % 2, digest);
    }
    for (i = 0; i < persisted; ++i) {
        st.st_ino = (ino_t)(1000 + i);
        snprintf(path, sizeof(path), "%s/txn-a/features/f%d/payload.squashfs", generations, i);
        assert(le_feature_provenance_test_cached(path, &st));
    }
    /* Re-caching the same path with a new identity reuses its slot rather
       than duplicating it or evicting another persisted digest. */
    st.st_ino = 9999;
    snprintf(path, sizeof(path), "%s/txn-a/features/f0/payload.squashfs", generations);
    le_feature_provenance_test_cache(path, &st, 1, digest);
    assert(le_feature_provenance_test_cached(path, &st));
    st.st_ino = 1000;
    assert(!le_feature_provenance_test_cached(path, &st));
    for (i = 1; i < persisted; ++i) {
        st.st_ino = (ino_t)(1000 + i);
        snprintf(path, sizeof(path), "%s/txn-a/features/f%d/payload.squashfs", generations, i);
        assert(le_feature_provenance_test_cached(path, &st));
    }
    unsetenv("LIBREECHO_GENERATIONS_ROOT");
    le_feature_provenance_shutdown();
}
/* The Radar regression: after many OTAs the persisted cache held a digest in
   every slot, so the two running daemons' executables evicted each other and
   libreecho-web re-hashed a 34 MB binary continuously (one core at 100%). */
static void test_full_persisted_cache_keeps_runtime_entries(void)
{
    static const char *digest = "0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69";
    char generations[1024], path[1400];
    struct stat st;
    int i, round, slots = le_feature_provenance_test_cache_slots(),
        runtime = le_feature_provenance_test_runtime_reserve();
    setup();
    snprintf(generations, sizeof(generations), "%s/generations", root);
    setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
    memset(&st, 0, sizeof(st));
    st.st_size = 1;
    for (i = 0; i < slots; ++i) {
        st.st_ino = (ino_t)(1000 + i);
        snprintf(path, sizeof(path), "%s/txn-%02d/features/f/payload.squashfs", generations, i);
        le_feature_provenance_test_cache(path, &st, 1, digest);
    }
    /* Every running daemon executable is cached once and then stays cached
       while the others are looked up and re-cached in turn. */
    for (round = 0; round < 3; ++round)
        for (i = 0; i < runtime; ++i) {
            st.st_ino = (ino_t)(5000 + i);
            snprintf(path, sizeof(path), "/proc/%d/exe", 100 + i);
            if (round)
                assert(le_feature_provenance_test_cached(path, &st));
            else
                le_feature_provenance_test_cache(path, &st, 1, digest);
        }
    /* Digests still fill everything outside the reserve. */
    for (i = 0, round = 0; i < slots; ++i) {
        st.st_ino = (ino_t)(1000 + i);
        snprintf(path, sizeof(path), "%s/txn-%02d/features/f/payload.squashfs", generations, i);
        round += le_feature_provenance_test_cached(path, &st);
    }
    assert(round == slots - runtime);
    unsetenv("LIBREECHO_GENERATIONS_ROOT");
    le_feature_provenance_shutdown();
}

/* Records for retired generations are dropped on load and never re-stored. */
static void test_retired_generation_records_are_pruned(void)
{
    static const char *digest = "0f6d5b80eac2c95711e382c1895f2e89ac758836c4e0ab29c43c016db431dc69";
    char generations[1024], cache[1024], live[1400], gone[1400], text[8192];
    struct stat st, fake;
    FILE *f;
    size_t n;
    setup();
    snprintf(generations, sizeof(generations), "%s/generations", root);
    mkdir_or_die(generations);
    setenv("LIBREECHO_GENERATIONS_ROOT", generations, 1);
    join_path(cache, sizeof(cache), update_root, "/feature-hash-cache");
    snprintf(live, sizeof(live), "%s/live.squashfs", generations);
    write_file(live, "payload");
    assert(stat(live, &st) == 0);
    snprintf(gone, sizeof(gone), "%s/txn-retired/features/tts/payload.squashfs", generations);
    fake = st;
    fake.st_ino = st.st_ino + 100000;
    plant_record(cache, &fake, digest, gone);
    write_record(cache, "a", &st, digest, live);
    /* Same path as a live file but a different identity: also unusable. */
    fake.st_ino = st.st_ino + 200000;
    write_record(cache, "a", &fake, digest, live);
    assert(chmod(cache, 0600) == 0);
    le_feature_provenance_test_persist_load();
    assert(le_feature_provenance_test_cached(live, &st));
    assert(!le_feature_provenance_test_cached(gone, &fake));
    /* The next store rewrites the file with the live record only. */
    le_feature_provenance_test_cache(live, &st, 1, digest);
    le_feature_provenance_test_persist_store();
    f = fopen(cache, "r");
    assert(f);
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    assert(strstr(text, live) && !strstr(text, "txn-retired"));
    assert(strchr(text, '\n') == text + n - 1);
    unsetenv("LIBREECHO_GENERATIONS_ROOT");
    le_feature_provenance_shutdown();
}
#endif

int main(void)
{
    test_incremental_hash_progress();
    test_canonical_hash_and_tamper_states();
    test_runtime_and_candidate_identity();
    test_runtime_metadata_boundaries_fail_closed();
    test_malformed_manifest_keeps_ids_unavailable();
    test_manifest_replacement_during_incremental_read();
    test_production_manifest_bounds_and_filename_allowlist();
    test_invalid_transaction_records_fail_closed();
    test_rollback_record_validation();
    test_candidate_control_namespace_fails_closed();
    test_authority_identity_and_transaction_precedence();
    test_v3_transaction_records();
    test_v3_generation_feature_root();
    test_generation_hash_persists_across_restart();
    test_flat_feature_tree_is_not_persisted();
#ifdef LE_FEATURE_PROVENANCE_TESTING
    test_persistable_path_rejects_escapes();
    test_eviction_keeps_persisted_digests();
    test_full_persisted_cache_keeps_runtime_entries();
    test_retired_generation_records_are_pruned();
#endif
    puts("feature provenance formatter: ok");
    return 0;
}
