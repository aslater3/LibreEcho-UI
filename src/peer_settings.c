/* Settings copy between paired devices (#449). See peer_settings.h. */
#include "peer_settings.h"
#include <string.h>

struct settings_key {
    const char *key;
    unsigned group;
};

/* The only keys a copy may carry. Adding a key here is a decision about
 * what another device may change: keep identity, network and secrets out. */
static const struct settings_key copyable[] = {
    {"wake_word", LE_SETTINGS_VOICE},
    {"wake_sensitivity", LE_SETTINGS_VOICE},
    {"stt_max_utterance_ms", LE_SETTINGS_VOICE},
    {"stt_end_silence_ms", LE_SETTINGS_VOICE},
    {"stt_vad_floor_rms", LE_SETTINGS_VOICE},
    {"volume", LE_SETTINGS_AUDIO},
    {"microphone_gain", LE_SETTINGS_AUDIO},
    {"home_assistant_url", LE_SETTINGS_HA},
    {"led_brightness", LE_SETTINGS_LED},
    {"led_visualizer_enabled", LE_SETTINGS_LED},
    {"button_action_brightness", LE_SETTINGS_LED},
    {"button_mute_brightness", LE_SETTINGS_LED}
};

static const struct { const char *name; unsigned group; } group_names[] = {
    {"voice", LE_SETTINGS_VOICE},
    {"audio", LE_SETTINGS_AUDIO},
    {"ha", LE_SETTINGS_HA},
    {"led", LE_SETTINGS_LED}
};

unsigned le_settings_group_from_name(const char *name)
{
    size_t i;

    if (!name)
        return LE_SETTINGS_NONE;
    for (i = 0; i < sizeof(group_names) / sizeof(group_names[0]); i++)
        if (!strcmp(name, group_names[i].name))
            return group_names[i].group;
    return LE_SETTINGS_NONE;
}

const char *le_settings_group_name(unsigned group)
{
    size_t i;

    for (i = 0; i < sizeof(group_names) / sizeof(group_names[0]); i++)
        if (group == group_names[i].group)
            return group_names[i].name;
    return NULL;
}

unsigned le_settings_key_group(const char *key)
{
    size_t i;

    if (!key)
        return LE_SETTINGS_NONE;
    for (i = 0; i < sizeof(copyable) / sizeof(copyable[0]); i++)
        if (!strcmp(key, copyable[i].key))
            return copyable[i].group;
    return LE_SETTINGS_NONE;
}
