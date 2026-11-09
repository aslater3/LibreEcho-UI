#ifndef LE_PEER_SETTINGS_H
#define LE_PEER_SETTINGS_H
#include <stddef.h>

/* Copy settings between paired devices (#449).
 *
 * The owner picks setting groups on one device and sends them to another
 * paired device as a signed "settings-copy" packet (peer_auth.h, the house
 * key from #450). The body is a flat JSON object holding the values of the
 * chosen groups. The receiving device applies only the keys listed here, and
 * only after the packet verifies, so a device outside the house or a replayed
 * packet changes nothing.
 *
 * Identity, network, accounts and secrets are never listed: hostname, MAC
 * addresses, Wi-Fi, SSH and API exposure, room, timezone, privacy, feature
 * switches, buttons (short and long), appearance, the ESPHome key and every
 * password or token stay on the device they belong to. */

#define LE_SETTINGS_COPY_TYPE "settings-copy"
#define LE_SETTINGS_BODY_MAX 1024
#define LE_SETTINGS_KEYS_MAX 24
#define LE_SETTINGS_KEY_MAX 32

enum le_settings_group {
    LE_SETTINGS_NONE = 0,
    LE_SETTINGS_VOICE = 1u << 0, /* wake word, its sensitivity, speech end and VAD tuning */
    LE_SETTINGS_AUDIO = 1u << 1, /* speaker volume and microphone gain */
    LE_SETTINGS_HA = 1u << 2,    /* Home Assistant address */
    LE_SETTINGS_LED = 1u << 3    /* LED brightness, visualizer, action and mute ring brightness */
};
#define LE_SETTINGS_ALL (LE_SETTINGS_VOICE | LE_SETTINGS_AUDIO | LE_SETTINGS_HA | LE_SETTINGS_LED)

/* "voice", "audio", "ha" or "led" to its bit; LE_SETTINGS_NONE otherwise. */
unsigned le_settings_group_from_name(const char *name);
/* The name of one group bit, or NULL when group is not exactly one known bit. */
const char *le_settings_group_name(unsigned group);
/* The group bit a setting key is copied under, or LE_SETTINGS_NONE when the
 * key is not copyable (identity, network, secrets and everything else). */
unsigned le_settings_key_group(const char *key);
#endif
