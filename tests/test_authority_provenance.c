#define _POSIX_C_SOURCE 200809L
#include "authority_provenance.h"
#include <assert.h>
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

int main(void)
{
    test_signed_helper_result_is_async_and_bound();
    puts("authority provenance async reader: ok");
    return 0;
}
