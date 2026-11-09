#ifndef LIBREECHO_EVENT_SOUND_H
#define LIBREECHO_EVENT_SOUND_H

/*
 * Owner-uploaded sounds for device events (#410).
 *
 * The web daemon writes them and audiod is the only reader, so the table of
 * events, their length limits and the on-disk layout live here once and
 * cannot drift between the two.
 *
 * Layout in the sound directory, one pair of names per event:
 *   sound-<event>.raw        the owner's sound, selected: audiod plays it
 *   sound-<event>.raw.saved  the owner's sound, kept but not selected
 * Both are raw mono S16LE at 48 kHz, the same format as the bundled sounds.
 * No file means the built-in behaviour, so a device that never had a sound
 * uploaded behaves exactly as before.
 *
 * They are plain files directly in /data/libreecho/config, not a directory
 * of their own: the boot data contract of older images rejects an unknown
 * directory under /data/libreecho (and under config/) and blocks every
 * service, which would turn a rollback into a brick, while it tolerates an
 * unknown regular file in config/. Living in config/ also means the backup
 * tool captures them and a factory reset clears them.
 */

#include <stdlib.h>
#include <string.h>

#define LE_EVENT_SOUND_DIR_DEFAULT "/data/libreecho/config"
#define LE_EVENT_SOUND_PREFIX "sound-"
#define LE_EVENT_SOUND_DIR_ENV "LIBREECHO_USER_SOUND_DIR"
#define LE_EVENT_SOUND_RATE 48000
#define LE_EVENT_SOUND_COUNT 3
/* Every converted sound fits in this many mono samples (the longest limit). */
#define LE_EVENT_SOUND_MAX_SAMPLES (LE_EVENT_SOUND_RATE * 5)

struct le_event_sound_slot {
    const char *id;
    const char *name;
    /* What plays when the owner has not chosen their own sound. */
    const char *fallback;
    unsigned max_ms;
};

/*
 * The wake chime plays while the recogniser is listening, and the device
 * hears its own speaker, so it is kept as short as the existing cue cap
 * allows for anything spoken over. The end chime follows the turn and can be
 * a little longer. A timer rings every two seconds until dismissed; a longer
 * sound simply rings back to back.
 */
static const struct le_event_sound_slot le_event_sound_slots[LE_EVENT_SOUND_COUNT] = {
    { "wake", "Listening chime", "chirp", 1000 },
    { "end", "Finished-listening chime", "none", 2000 },
    { "timer", "Timer and alarm ring", "two-tone", 5000 },
};

static inline int le_event_sound_index(const char *id)
{
    int i;

    if (!id)
        return -1;
    for (i = 0; i < LE_EVENT_SOUND_COUNT; ++i)
        if (!strcmp(id, le_event_sound_slots[i].id))
            return i;
    return -1;
}

static inline const char *le_event_sound_dir(void)
{
    const char *dir = getenv(LE_EVENT_SOUND_DIR_ENV);

    return dir && dir[0] == '/' ? dir : LE_EVENT_SOUND_DIR_DEFAULT;
}

/* Largest stored file for an event, in bytes of mono S16LE. */
static inline unsigned long le_event_sound_max_bytes(int index)
{
    if (index < 0 || index >= LE_EVENT_SOUND_COUNT)
        return 0;
    return (unsigned long)le_event_sound_slots[index].max_ms *
           (LE_EVENT_SOUND_RATE / 1000) * 2UL;
}

#endif
