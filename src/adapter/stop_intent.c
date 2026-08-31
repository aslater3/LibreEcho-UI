#include "stop_intent.h"

#include <ctype.h>
#include <string.h>

/*
 * Lowercase the transcript and reduce everything that is not a letter to a
 * single space, so matching can be done on whole words with plain substring
 * searches. Punctuation varies with whatever the recogniser felt like emitting
 * ("Stop." and "stop" are the same request), and normalising once here keeps
 * every rule below simple.
 */
static void normalise(const char *in, char *out, size_t size)
{
    size_t n = 0;
    int last_space = 1;

    if (!out || size == 0)
        return;
    out[0] = '\0';
    if (!in)
        return;
    if (n + 1 < size)
        out[n++] = ' ';
    for (; *in && n + 2 < size; in++) {
        unsigned char ch = (unsigned char)*in;

        if (isalpha(ch)) {
            out[n++] = (char)tolower(ch);
            last_space = 0;
        } else if (ch == '\'') {
            /* A contraction is one word: "don't" must not split into "don t",
               or the negation check below silently stops matching it. */
            continue;
        } else if (!last_space) {
            out[n++] = ' ';
            last_space = 1;
        }
    }
    if (!last_space && n + 1 < size)
        out[n++] = ' ';
    out[n] = '\0';
}

/* `text` is space-delimited on both ends, so a padded search is a word match. */
static int has_word(const char *text, const char *word)
{
    char padded[32];

    if (strlen(word) + 3 > sizeof(padded))
        return 0;
    padded[0] = ' ';
    strcpy(padded + 1, word);
    strcat(padded, " ");
    return strstr(text, padded) != NULL;
}

static int has_phrase(const char *text, const char *phrase)
{
    return strstr(text, phrase) != NULL;
}

int le_stop_intent_matches(const char *transcript)
{
    char text[256];

    if (!transcript || !transcript[0])
        return 0;
    normalise(transcript, text, sizeof(text));

    /*
     * Negation first. "Don't stop" is the opposite request, and acting on it
     * would be the most annoying possible bug -- so it loses outright rather
     * than being weighed against anything below.
     */
    if (has_phrase(text, " dont ") || has_phrase(text, " do not ") ||
        has_word(text, "never"))
        return 0;

    if (has_word(text, "stop") || has_word(text, "quiet") ||
        has_word(text, "silence") || has_phrase(text, " shut up "))
        return 1;

    /*
     * "Turn it off" is as likely to be about the lights, so only accept the
     * turn-off phrasing when it names what is playing.
     */
    if (has_phrase(text, " turn ") && has_word(text, "off") &&
        (has_word(text, "music") || has_word(text, "radio") ||
         has_word(text, "song") || has_word(text, "sound")))
        return 1;

    return 0;
}

void le_stop_intent_plan(const struct le_stop_state *state,
                         struct le_stop_plan *plan)
{
    if (!plan)
        return;
    plan->action = LE_STOP_NONE;
    plan->stop_radio = 0;
    plan->stop_speech = 0;
    plan->stop_noise = 0;
    if (!state)
        return;

    /*
     * A ringing timer takes the word for itself. An alarm going off over a
     * song is the one case where "stop" is plainly about the alarm, and
     * silencing the song as well would be a nasty surprise.
     */
    if (state->timer_ringing) {
        plan->action = LE_STOP_TIMER;
        return;
    }

    /*
     * Otherwise everything the device drives stops together. Choosing a winner
     * between the radio and a talking assistant would be a guess the person
     * then has to learn; "stop" simply means make it quiet.
     */
    if (state->radio_playing || state->speaking || state->noise_active) {
        plan->action = LE_STOP_LOCAL;
        plan->stop_radio = state->radio_playing ? 1 : 0;
        plan->stop_speech = state->speaking ? 1 : 0;
        plan->stop_noise = state->noise_active ? 1 : 0;
        return;
    }

    /*
     * Nothing here can stop a phone: AirPlay and Bluetooth transport belongs to
     * the device that started them. Saying so is the whole point -- doing
     * nothing silently is the bug this replaces.
     */
    if (state->external_source)
        plan->action = LE_STOP_EXTERNAL;
}

const char *le_stop_external_speech(void)
{
    return "That's playing from your phone, so you'll need to stop it there.";
}
