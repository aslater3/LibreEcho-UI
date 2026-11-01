/* Settings copy list (#449): group names and the copy boundary. The packet
 * handling (signed, validated, applied once) and the two-device flow are
 * covered by tests/test_peer_settings.py through real daemons. */
#include "peer_settings.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_group_names(void)
{
    CHECK(le_settings_group_from_name("voice") == LE_SETTINGS_VOICE);
    CHECK(le_settings_group_from_name("audio") == LE_SETTINGS_AUDIO);
    CHECK(le_settings_group_from_name("ha") == LE_SETTINGS_HA);
    CHECK(le_settings_group_from_name("led") == LE_SETTINGS_LED);
    CHECK(le_settings_group_from_name("Voice") == LE_SETTINGS_NONE);
    CHECK(le_settings_group_from_name("network") == LE_SETTINGS_NONE);
    CHECK(le_settings_group_from_name("") == LE_SETTINGS_NONE);
    CHECK(le_settings_group_from_name(NULL) == LE_SETTINGS_NONE);
    CHECK(!strcmp(le_settings_group_name(LE_SETTINGS_LED), "led"));
    CHECK(le_settings_group_name(LE_SETTINGS_VOICE | LE_SETTINGS_AUDIO) == NULL);
    CHECK(le_settings_group_name(LE_SETTINGS_NONE) == NULL);
}

static void test_copyable_keys(void)
{
    CHECK(le_settings_key_group("wake_word") == LE_SETTINGS_VOICE);
    CHECK(le_settings_key_group("wake_sensitivity") == LE_SETTINGS_VOICE);
    CHECK(le_settings_key_group("stt_max_utterance_ms") == LE_SETTINGS_VOICE);
    CHECK(le_settings_key_group("stt_end_silence_ms") == LE_SETTINGS_VOICE);
    CHECK(le_settings_key_group("stt_vad_floor_rms") == LE_SETTINGS_VOICE);
    CHECK(le_settings_key_group("volume") == LE_SETTINGS_AUDIO);
    CHECK(le_settings_key_group("microphone_gain") == LE_SETTINGS_AUDIO);
    CHECK(le_settings_key_group("home_assistant_url") == LE_SETTINGS_HA);
    CHECK(le_settings_key_group("led_brightness") == LE_SETTINGS_LED);
    CHECK(le_settings_key_group("led_visualizer_enabled") == LE_SETTINGS_LED);
    CHECK(le_settings_key_group("button_action_brightness") == LE_SETTINGS_LED);
    CHECK(le_settings_key_group("button_mute_brightness") == LE_SETTINGS_LED);
}

/* Identity, network, accounts and secrets must never be copyable. */
static void test_identity_and_secrets_never_copied(void)
{
    static const char *never[] = {
        "hostname", "mac_wifi", "mac_bt", "ssh", "api_lan", "discoverable", "room", "timezone",
        "privacy_local_only", "privacy_audio_retention", "privacy_audio_mode", "privacy_audio_remote_url",
        "privacy_telemetry", "privacy_crash_reports", "privacy_active_ping", "privacy_log_hours",
        "privacy_audio_retention_hours", "privacy_audio_max_mb", "feature_https", "feature_simulation",
        "feature_acoustic_events", "button_short", "button_long", "ui_theme", "color_mode", "news_enabled",
        "esphome_noise_key", "password", "wifi_psk", "token", "csrf_token", "auth_token", "peer_key",
        "integrations", "led_r", "led_g", "led_b", "microphone_muted", "volume_leveling", "schema_version",
        "", "VOLUME", "volume ", "wake_word_extra"
    };
    size_t i;
    for (i = 0; i < sizeof never / sizeof never[0]; i++)
        CHECK(le_settings_key_group(never[i]) == LE_SETTINGS_NONE);
    CHECK(le_settings_key_group(NULL) == LE_SETTINGS_NONE);
}

int main(void)
{
    test_group_names();
    test_copyable_keys();
    test_identity_and_secrets_never_copied();
    if (failures) {
        fprintf(stderr, "peer settings: %d failure(s)\n", failures);
        return 1;
    }
    printf("peer settings: ok\n");
    return 0;
}
