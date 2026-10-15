#define _POSIX_C_SOURCE 200809L
#include "authority_provenance.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char root_template[] = "/tmp/libreecho-authority-test-XXXXXX";
static char root[sizeof(root_template)];
static char helper[1024];

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static void mkdir_path(const char *path)
{
    assert(mkdir(path, 0700) == 0);
}

static void setup(void)
{
    char update[1024], features[1024], run[1024], feature[1024];
    strcpy(root, root_template);
    assert(mkdtemp(root));
    snprintf(update, sizeof(update), "%s/update", root);
    snprintf(features, sizeof(features), "%s/features", root);
    snprintf(run, sizeof(run), "%s/run", root);
    snprintf(feature, sizeof(feature), "%s/features/tts", root);
    mkdir_path(update);
    mkdir_path(features);
    mkdir_path(run);
    mkdir_path(feature);
    setenv("LIBREECHO_UPDATE_ROOT", update, 1);
    setenv("LIBREECHO_FEATURE_ROOT", features, 1);
    setenv("LIBREECHO_FEATURE_RUN_ROOT", run, 1);
    snprintf(helper, sizeof(helper), "%s/helper", root);
}

static void install_helper(const char *format, ...)
{
    char script[8192];
    va_list ap;
    int n;
    memcpy(script, "#!/bin/sh\n", 10);
    va_start(ap, format);
    n = vsnprintf(script + 10, sizeof(script) - 10, format, ap);
    va_end(ap);
    assert(n >= 0 && (size_t)n < sizeof(script) - 10);
    script[10 + n] = '\n';
    script[11 + n] = 0;
    write_text(helper, script);
    assert(chmod(helper, 0700) == 0);
    setenv("LIBREECHO_FEATURE_TRANSACTION_HELPER", helper, 1);
}

static void test_signed_helper_result_is_async_and_bound(void)
{
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX];
    char path[1024];
    int i;
    setup();
    snprintf(path, sizeof(path), "%s/update/installed", root);
    write_text(path, "transaction_id=txn-01314\nphase=installed\n");
    snprintf(path, sizeof(path), "%s/update/committed-manifest", root);
    write_text(path, "signed manifest\n");
    snprintf(path, sizeof(path), "%s/update/committed-manifest.sig", root);
    write_text(path, "signed signature\n");
    snprintf(path, sizeof(path), "%s/features/tts/manifest.json", root);
    write_text(path, "base metadata\n");
    snprintf(path, sizeof(path), "%s/features/tts/runtime-manifest.json", root);
    write_text(path, "runtime metadata\n");
    snprintf(path, sizeof(path), "%s/features/tts/payload.squashfs", root);
    write_text(path, "base payload\n");
    snprintf(path, sizeof(path), "%s/features/tts/runtime.squashfs", root);
    write_text(path, "runtime payload\n");
    snprintf(path, sizeof(path), "%s/run/libreecho-ttsd.pid", root);
    snprintf(output, sizeof(output), "%ld\n", (long)getpid());
    write_text(path, output);
    install_helper("sleep 4\nprintf '%%b' '%s'", \
        "schema=libreecho-feature-provenance-v1\\n"
        "transaction_id=txn-01314\\n"
        "installed_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\n"
        "manifest_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\n"
        "manifest_sig_sha256=cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\\n"
        "feature_ids=airplay2,tts,wakeword,stt,assistant\\n"
        "feature_airplay2_action=preserve\\nfeature_airplay2_release=radar-puffin-v0.13.14\\nfeature_airplay2_source_commit=0123456789012345678901234567890123456789\\nfeature_airplay2_kind=base\\nfeature_airplay2_payload_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\\nfeature_airplay2_manifest_sha256=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\\nfeature_airplay2_runtime_sha256=none\\nfeature_airplay2_runtime_manifest_sha256=none\\nfeature_airplay2_daemon_sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\\n"
        "feature_tts_action=runtime\\nfeature_tts_release=radar-puffin-v0.13.14\\nfeature_tts_source_commit=0123456789012345678901234567890123456789\\nfeature_tts_kind=runtime\\nfeature_tts_payload_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\\nfeature_tts_manifest_sha256=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\\nfeature_tts_runtime_sha256=1111111111111111111111111111111111111111111111111111111111111111\\nfeature_tts_runtime_manifest_sha256=2222222222222222222222222222222222222222222222222222222222222222\\nfeature_tts_daemon_sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\\n"
        "feature_wakeword_action=preserve\\nfeature_wakeword_release=radar-puffin-v0.13.14\\nfeature_wakeword_source_commit=0123456789012345678901234567890123456789\\nfeature_wakeword_kind=base\\nfeature_wakeword_payload_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\\nfeature_wakeword_manifest_sha256=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\\nfeature_wakeword_runtime_sha256=none\\nfeature_wakeword_runtime_manifest_sha256=none\\nfeature_wakeword_daemon_sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\\n"
        "feature_stt_action=replace\\nfeature_stt_release=radar-puffin-v0.13.14\\nfeature_stt_source_commit=0123456789012345678901234567890123456789\\nfeature_stt_kind=base\\nfeature_stt_payload_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\\nfeature_stt_manifest_sha256=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\\nfeature_stt_runtime_sha256=none\\nfeature_stt_runtime_manifest_sha256=none\\nfeature_stt_daemon_sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\\n"
        "feature_assistant_action=preserve\\nfeature_assistant_release=radar-puffin-v0.13.14\\nfeature_assistant_source_commit=0123456789012345678901234567890123456789\\nfeature_assistant_kind=base\\nfeature_assistant_payload_sha256=dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\\nfeature_assistant_manifest_sha256=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\\nfeature_assistant_runtime_sha256=none\\nfeature_assistant_runtime_manifest_sha256=none\\nfeature_assistant_daemon_sha256=ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\\n");
    le_authority_provenance_tick();
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false") != NULL);
    for (i = 0; i < 600; ++i) {
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
        le_authority_provenance_tick();
        le_authority_provenance_json(output, sizeof(output));
        if (strstr(output, "\"available\":true"))
            break;
    }
    assert(i < 600);
    assert(strstr(output, "libreecho-feature-provenance-v1") != NULL);
    assert(strstr(output, "radar-puffin-v0.13.14") != NULL);
    write_text(path, "999999\n");
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false") != NULL);
    le_authority_provenance_shutdown();
}

#ifdef LE_AUTHORITY_PROVENANCE_TESTING
long le_authority_provenance_test_retry_delay(void);
void le_authority_provenance_test_retry_now(void);

/* ---- OTA v3: real Radar helper output, persisted per immutable generation ---- */

#define V3_TX "txn-717467f70c3e3d240e9f8947"
static char v3_fixture[8192];
static char v3_count_path[1024];

static void read_fixture(void)
{
    FILE *f = fopen(LE_AUTHORITY_V3_FIXTURE, "r");
    size_t n;
    assert(f);
    n = fread(v3_fixture, 1, sizeof(v3_fixture) - 1, f);
    assert(n > 0 && n < sizeof(v3_fixture) - 1);
    v3_fixture[n] = 0;
    assert(fclose(f) == 0);
}

static void v3_setup(void)
{
    static const char *features[] = {"airplay2", "tts", "wakeword", "stt", "assistant"};
    char path[1024];
    size_t i;
    setup();
    unsetenv("LIBREECHO_FEATURE_ROOT");
    snprintf(path, sizeof(path), "%s/generations", root);
    mkdir_path(path);
    setenv("LIBREECHO_GENERATIONS_ROOT", path, 1);
    snprintf(path, sizeof(path), "%s/generations/" V3_TX, root);
    mkdir_path(path);
    snprintf(path, sizeof(path), "%s/generations/" V3_TX "/features", root);
    mkdir_path(path);
    for (i = 0; i < 5; ++i) {
        snprintf(path, sizeof(path), "%s/generations/" V3_TX "/features/%s", root, features[i]);
        mkdir_path(path);
        snprintf(path, sizeof(path), "%s/generations/" V3_TX "/features/%s/payload.squashfs", root, features[i]);
        write_text(path, "payload\n");
        snprintf(path, sizeof(path), "%s/generations/" V3_TX "/features/%s/manifest.json", root, features[i]);
        write_text(path, "{}\n");
    }
    snprintf(path, sizeof(path), "%s/generations/" V3_TX "/COMPLETE", root);
    write_text(path, "complete\n");
    snprintf(path, sizeof(path), "%s/generations/" V3_TX "/target.manifest", root);
    write_text(path, "format=libreecho-target-v3\ntransaction_id=" V3_TX "\n"
               "release=radar-puffin-build-ecc683e-ec29d7e8c0f3c99b-993b82b6e79ab683\nversion=0.14.0\n");
    snprintf(path, sizeof(path), "%s/generations/" V3_TX "/target.manifest.sig", root);
    write_text(path, "sig\n");
    snprintf(path, sizeof(path), "%s/update/current", root);
    write_text(path, V3_TX "\n");
    snprintf(path, sizeof(path), "%s/authority-cache", root);
    setenv("LIBREECHO_AUTHORITY_PROVENANCE_CACHE", path, 1);
    snprintf(v3_count_path, sizeof(v3_count_path), "%s/helper-runs", root);
    snprintf(path, sizeof(path), "%s/v3-output", root);
    write_text(path, v3_fixture);
    install_helper("echo run >> '%s'\ncat '%s'", v3_count_path, path);
}

static int helper_runs(void)
{
    FILE *f = fopen(v3_count_path, "r");
    int c, n = 0;
    if (!f) return 0;
    while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f);
    return n;
}

static int settle(char *output, size_t size, const char *want)
{
    int i;
    for (i = 0; i < 500; ++i) {
        struct timespec delay = {0, 10000000};
        le_authority_provenance_tick();
        le_authority_provenance_json(output, size);
        if (strstr(output, want)) return 1;
        nanosleep(&delay, NULL);
    }
    return 0;
}

static void restart(void)
{
    le_authority_provenance_shutdown();
}

static void test_v3_helper_output_is_available(void)
{
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX];
    v3_setup();
    assert(settle(output, sizeof(output), "\"available\":true"));
    assert(strstr(output, "\"schema\":\"libreecho-feature-provenance-v3\""));
    assert(strstr(output, "\"generation\":\"" V3_TX "\""));
    assert(strstr(output, "\"release\":\"radar-puffin-build-ecc683e-ec29d7e8c0f3c99b-993b82b6e79ab683\""));
    assert(strstr(output, "\"version\":\"0.14.0\""));
    assert(strstr(output, "\"target_manifest_sha256\":\"02dd66d814ece8502cffb943f16f4f1d8d2cb126a26768b2c27b8f0655a15ed5\""));
    assert(strstr(output, "{\"feature_id\":\"stt\",\"payload_sha256\":\"6c1e866b364f1c06548ace326d8748d1a7f28a7488669627019bf99f9137e856\""));
    assert(helper_runs() == 1);
    restart();
}

static void test_v3_result_persists_across_restart_without_rerun(void)
{
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX], path[1024];
    struct stat st;
    v3_setup();
    assert(settle(output, sizeof(output), "\"available\":true"));
    assert(helper_runs() == 1);
    snprintf(path, sizeof(path), "%s/authority-cache", root);
    assert(stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    restart();
    le_authority_provenance_tick();
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":true"));
    assert(strstr(output, "\"generation\":\"" V3_TX "\""));
    assert(helper_runs() == 1);
    restart();
}

static void test_v3_cache_invalidated_by_changed_generation_file(void)
{
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX], path[1024];
    v3_setup();
    assert(settle(output, sizeof(output), "\"available\":true"));
    restart();
    snprintf(path, sizeof(path), "%s/generations/" V3_TX "/features/tts/payload.squashfs", root);
    write_text(path, "tampered payload\n");
    le_authority_provenance_tick();
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false"));
    assert(settle(output, sizeof(output), "\"available\":true"));
    assert(helper_runs() == 2);
    /* the live observation is also invalidated while running */
    write_text(path, "tampered again\n");
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false"));
    restart();
}

static void test_v3_cache_rejects_other_generation_and_untrusted_files(void)
{
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX], path[1024], target[1024];
    v3_setup();
    assert(settle(output, sizeof(output), "\"available\":true"));
    restart();
    snprintf(path, sizeof(path), "%s/authority-cache", root);
    assert(chmod(path, 0644) == 0);
    le_authority_provenance_tick();
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false"));
    assert(settle(output, sizeof(output), "\"available\":true"));
    assert(helper_runs() == 2);
    restart();
    /* a symlinked cache is refused */
    snprintf(target, sizeof(target), "%s/authority-cache.real", root);
    assert(rename(path, target) == 0);
    assert(symlink(target, path) == 0);
    le_authority_provenance_tick();
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false"));
    assert(settle(output, sizeof(output), "\"available\":true"));
    assert(helper_runs() == 3);
    restart();
    /* a cache for an older generation is not reused after an OTA */
    assert(unlink(path) == 0 || errno == ENOENT);
    assert(rename(target, path) == 0);
    snprintf(path, sizeof(path), "%s/update/current", root);
    write_text(path, "txn-other\n");
    snprintf(path, sizeof(path), "%s/generations/txn-other", root);
    mkdir_path(path);
    le_authority_provenance_tick();
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false"));
    restart();
}

static void test_v3_rejects_mismatch_and_malformed_output(void)
{
    static const char *const bad[] = {
        /* observed hash differs from signed target */
        "feature_stt_target_payload_sha256=6c1e866b364f1c06548ace326d8748d1a7f28a7488669627019bf99f9137e856",
        "feature_stt_target_payload_sha256=0000000000000000000000000000000000000000000000000000000000000000",
        /* generation does not match update/current */
        "generation=" V3_TX, "generation=txn-other",
        /* unknown key */
        "target_manifest_sha256=", "extra=1\ntarget_manifest_sha256=",
        /* duplicate key */
        "generation=" V3_TX, "generation=" V3_TX "\ngeneration=" V3_TX,
        /* missing feature line */
        "feature_assistant_target_manifest_sha256=d6f45c424b326dd87ac4d24fefe9c2e46252e25d5bc350626802261520af79d6\n", "",
        /* non-hex hash */
        "feature_tts_payload_sha256=a9defee5", "feature_tts_payload_sha256=Z9defee5"
    };
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX], path[1024], edited[8192], *at;
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i += 2) {
        int k;
        v3_setup();
        at = strstr(v3_fixture, bad[i]);
        assert(at);
        snprintf(edited, sizeof(edited), "%.*s%s%s", (int)(at - v3_fixture), v3_fixture, bad[i + 1], at + strlen(bad[i]));
        snprintf(path, sizeof(path), "%s/v3-output", root);
        write_text(path, edited);
        for (k = 0; k < 200 && helper_runs() < 1; ++k) {
            struct timespec delay = {0, 10000000};
            le_authority_provenance_tick();
            nanosleep(&delay, NULL);
        }
        for (k = 0; k < 50; ++k) {
            struct timespec delay = {0, 10000000};
            le_authority_provenance_tick();
            nanosleep(&delay, NULL);
        }
        le_authority_provenance_json(output, sizeof(output));
        assert(strstr(output, "\"available\":false"));
        snprintf(path, sizeof(path), "%s/authority-cache", root);
        assert(access(path, F_OK) != 0);
        restart();
    }
}

static void test_persistent_failure_backs_off_to_idle(void)
{
    char output[LE_AUTHORITY_PROVENANCE_JSON_MAX];
    int failures = 0, k;
    v3_setup();
    install_helper("echo run >> '%s'\nexit 1", v3_count_path);
    for (k = 0; k < 2000 && failures < 4; ++k) {
        struct timespec delay = {0, 5000000};
        le_authority_provenance_tick();
        nanosleep(&delay, NULL);
        if (helper_runs() > failures && le_authority_provenance_test_retry_delay() > 0) {
            failures = helper_runs();
            if (failures < 4) {
                assert(le_authority_provenance_test_retry_delay() <= 5);
                le_authority_provenance_test_retry_now();
            }
        }
    }
    assert(failures == 4);
    assert(le_authority_provenance_test_retry_delay() > 60);
    le_authority_provenance_json(output, sizeof(output));
    assert(strstr(output, "\"available\":false"));
    restart();
}
#endif

int main(void)
{
    test_signed_helper_result_is_async_and_bound();
#ifdef LE_AUTHORITY_PROVENANCE_TESTING
    read_fixture();
    test_v3_helper_output_is_available();
    test_v3_result_persists_across_restart_without_rerun();
    test_v3_cache_invalidated_by_changed_generation_file();
    test_v3_cache_rejects_other_generation_and_untrusted_files();
    test_v3_rejects_mismatch_and_malformed_output();
    test_persistent_failure_backs_off_to_idle();
    puts("authority provenance v3 parse/persist/backoff: ok");
#endif
    puts("authority provenance async reader: ok");
    return 0;
}
