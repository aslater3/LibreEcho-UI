#define _POSIX_C_SOURCE 200809L
#include "feature_provenance.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static void test_incremental_hash_progress(void)
{
    char output[LE_FEATURE_COMPONENTS_JSON_MAX];
    char path[1024];
    int fd;
    setup();
    make_feature_dir("assistant");
    join_path(path, sizeof(path), feature_root, "/assistant/manifest.json");
    write_file(path, "{\"schema_version\":1,\"feature_id\":\"assistant\",\"format\":\"squashfs-lz4\",\"product_release\":\"radar-puffin-v0.13.11\",\"source_commit\":\"0123456789012345678901234567890123456789\",\"payload\":{\"filename\":\"assistant.squashfs\",\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},\"files\":{}}\n");
    join_path(path, sizeof(path), feature_root, "/assistant/payload.squashfs");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, 131072) == 0);
    assert(close(fd) == 0);

    render_once(output, sizeof(output));
    for (fd = 0; fd < 2; ++fd)
        le_feature_provenance_tick();
    render_once(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"pending\"");
    for (fd = 0; fd < 8; ++fd)
        le_feature_provenance_tick();
    render_once(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"pending\"");
    le_feature_provenance_tick();
    render_once(output, sizeof(output));
    assert_component_contains(output, "assistant", "\"effective\":\"mismatch\"");
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
    static const char *names[] = {"pending", "feature-commit", "installed"};
    size_t i;
    int kind;
    setup();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        for (kind = 0; kind < 3; ++kind)
            assert_invalid_record_unknown(names[i], kind);
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
    write_file(path, "transaction_id=txn-old\nphase=rolled-back\n");
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
    test_candidate_control_namespace_fails_closed();
    test_authority_identity_and_transaction_precedence();
    puts("feature provenance formatter: ok");
    return 0;
}
