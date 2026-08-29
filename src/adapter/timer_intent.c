#include "timer_intent.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define NORMAL_MAX 256

/* Spoken numbers. Speech-to-text writes "ten", not "10", so a digits-only
   parser misses most of what people actually say. */
static const struct {
    const char *word;
    int value;
} WORD_NUMBERS[] = {
    {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5},
    {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10},
    {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14},
    {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18},
    {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40},
    {"fifty", 50}, {"sixty", 60}, {"ninety", 90},
    /* "a minute" and "an hour" are ordinary phrasings, not sloppy ones. */
    {"a", 1}, {"an", 1}
};

/*
 * Lower-case, strip punctuation, collapse whitespace, and pad with a space at
 * each end so a word search can require boundaries without special-casing the
 * ends of the string.
 */
static void normalise(const char *in, char *out, size_t size)
{
    size_t used = 0;
    int last_space = 1;

    if (!size)
        return;
    if (used + 1 < size)
        out[used++] = ' ';
    for (; in && *in && used + 2 < size; ++in) {
        unsigned char c = (unsigned char)*in;

        if (isalnum(c)) {
            out[used++] = (char)tolower(c);
            last_space = 0;
        } else if (!last_space) {
            out[used++] = ' ';
            last_space = 1;
        }
    }
    if (!last_space && used + 1 < size)
        out[used++] = ' ';
    out[used] = '\0';
}

static int has_word(const char *haystack, const char *word)
{
    char padded[64];

    if (snprintf(padded, sizeof(padded), " %s ", word) >= (int)sizeof(padded))
        return 0;
    return strstr(haystack, padded) != NULL;
}

/* Where a word starts, or NULL. */
static const char *find_word(const char *haystack, const char *word)
{
    char padded[64];
    const char *hit;

    if (snprintf(padded, sizeof(padded), " %s ", word) >= (int)sizeof(padded))
        return NULL;
    hit = strstr(haystack, padded);
    return hit ? hit + 1 : NULL;
}

static int starts_word(const char *text, const char *word)
{
    size_t length = strlen(word);

    return !strncmp(text, word, length) &&
           (text[length] == '\0' || text[length] == ' ');
}

static const char *after_word(const char *text, const char *word)
{
    if (!starts_word(text, word))
        return text;
    text += strlen(word);
    while (*text == ' ')
        ++text;
    return text;
}

static const char *first_cancel_verb(const char *text, size_t *length)
{
    static const char *const VERBS[] = {"cancel", "delete", "remove", "clear"};
    const char *verb = NULL;
    size_t i;

    if (length)
        *length = 0;
    for (i = 0; i < sizeof(VERBS) / sizeof(VERBS[0]); ++i) {
        const char *hit = find_word(text, VERBS[i]);

        if (hit && (!verb || hit < verb)) {
            verb = hit;
            if (length)
                *length = strlen(VERBS[i]);
        }
    }
    return verb;
}

/*
 * Read a count immediately before `unit`. Handles digits, single words, and
 * the two-word forms speech produces ("twenty five"). Returns -1 when there
 * is no number there, which is not the same as zero.
 */
static int number_before(const char *text, const char *unit)
{
    const char *at = find_word(text, unit);
    const char *cursor;
    char words[3][32];
    int count = 0;
    size_t i;
    int total = -1;

    if (!at || at == text)
        return -1;

    /* Walk back over the two words preceding the unit. */
    cursor = at - 1;
    while (count < 2 && cursor > text) {
        const char *end;
        size_t length;

        while (cursor > text && *cursor == ' ')
            --cursor;
        if (cursor <= text)
            break;
        end = cursor + 1;
        while (cursor > text && *(cursor - 1) != ' ')
            --cursor;
        length = (size_t)(end - cursor);
        if (length >= sizeof(words[0]))
            break;
        if (snprintf(words[count], sizeof(words[count]), "%.*s",
                     (int)length, cursor) < 0)
            break;
        ++count;
        if (cursor > text)
            --cursor;
    }
    if (!count)
        return -1;

    /* words[0] is nearest the unit. */
    for (i = 0; i < (size_t)count; ++i) {
        const char *word = words[i];
        int value = -1;
        size_t k;

        if (isdigit((unsigned char)word[0])) {
            value = 0;
            for (k = 0; word[k]; ++k) {
                if (!isdigit((unsigned char)word[k])) {
                    value = -1;
                    break;
                }
                value = value * 10 + (word[k] - '0');
                if (value > 100000)
                    break;
            }
        } else {
            for (k = 0; k < sizeof(WORD_NUMBERS) / sizeof(WORD_NUMBERS[0]);
                 ++k) {
                if (!strcmp(word, WORD_NUMBERS[k].word)) {
                    value = WORD_NUMBERS[k].value;
                    break;
                }
            }
        }
        if (value < 0)
            break;
        if (total < 0)
            total = value;
        else if (value >= 20 && total < 10)
            /* "twenty five" -- the tens word precedes the units word. */
            total += value;
        else
            break;
    }
    return total;
}

static long long unit_seconds(const char *text, const char *singular,
                              const char *plural, long long multiplier,
                              int *found)
{
    int count = number_before(text, plural);

    if (count < 0)
        count = number_before(text, singular);
    /* No count is no duration. "a" and "an" are counts of one in the table
       above, so "for an hour" is covered without guessing on behalf of a
       phrase that has no number in it at all. */
    if (count < 0)
        return 0;
    *found = 1;
    return (long long)count * multiplier;
}

static long long parse_duration(const char *text, int *found)
{
    long long total = 0;
    int part = 0;

    *found = 0;
    total += unit_seconds(text, "hour", "hours", 3600, &part);
    if (part) *found = 1;
    part = 0;
    total += unit_seconds(text, "minute", "minutes", 60, &part);
    if (part) *found = 1;
    part = 0;
    total += unit_seconds(text, "second", "seconds", 1, &part);
    if (part) *found = 1;

    /* "an hour and a half", "half an hour". */
    if (has_word(text, "half")) {
        if (has_word(text, "hour") || has_word(text, "hours")) {
            if (total >= 3600 && has_word(text, "and"))
                total += 1800;
            else if (total == 3600)
                total = 1800;
            *found = 1;
        } else if (has_word(text, "minute") || has_word(text, "minutes")) {
            total += 30;
            *found = 1;
        }
    }
    return total;
}

static int copy_label(char *out, size_t size, const char *start, size_t length)
{
    int written;

    if (!out || !size || !start || length > (size_t)INT_MAX)
        return 0;
    while (length && start[length - 1] == ' ')
        --length;
    if (!length || length >= size)
        return 0;
    written = snprintf(out, size, "%.*s", (int)length, start);
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return 0;
    }
    /* A duration is not a name: "ten minute" is when, not a label. */
    if (strstr(out, "minute") || strstr(out, "hour") ||
        strstr(out, "second")) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

static int is_number_token(const char *start, size_t length)
{
    size_t i;

    if (!start || !length)
        return 0;
    if (isdigit((unsigned char)start[0])) {
        for (i = 1; i < length; ++i)
            if (!isdigit((unsigned char)start[i]))
                return 0;
        return 1;
    }
    for (i = 0; i < sizeof(WORD_NUMBERS) / sizeof(WORD_NUMBERS[0]); ++i)
        if (strlen(WORD_NUMBERS[i].word) == length &&
            !strncmp(start, WORD_NUMBERS[i].word, length))
            return 1;
    return 0;
}

static int span_is_number_sequence(const char *start, size_t length)
{
    size_t offset = 0;
    int found = 0;

    while (offset < length) {
        size_t token_length;

        while (offset < length && start[offset] == ' ')
            ++offset;
        if (offset >= length)
            break;
        token_length = offset;
        while (token_length < length && start[token_length] != ' ')
            ++token_length;
        if (token_length == offset ||
            !is_number_token(start + offset, token_length - offset))
            return 0;
        found = 1;
        offset = token_length;
    }
    return found;
}

static int span_has_word(const char *start, size_t length, const char *word)
{
    size_t word_length = strlen(word);
    size_t offset = 0;

    while (offset < length) {
        size_t token_length;

        while (offset < length && start[offset] == ' ')
            ++offset;
        token_length = offset;
        while (token_length < length && start[token_length] != ' ')
            ++token_length;
        if (token_length - offset == word_length &&
            !strncmp(start + offset, word, word_length))
            return 1;
        offset = token_length;
    }
    return 0;
}

static int copy_cancel_label(char *out, size_t size, const char *start,
                             size_t length)
{
    if (!out || !size || !start || !length)
        return 0;
    /* Quantities and duration units select a timer; they are never stored
       names. Reject them before copying transcript bytes into the label. */
    if (span_is_number_sequence(start, length) ||
        span_has_word(start, length, "minute") ||
        span_has_word(start, length, "minutes") ||
        span_has_word(start, length, "hour") ||
        span_has_word(start, length, "hours") ||
        span_has_word(start, length, "second") ||
        span_has_word(start, length, "seconds")) {
        out[0] = '\0';
        return 0;
    }
    return copy_label(out, size, start, length);
}

/*
 * The trailing "for X" of "set a timer for the pasta". Only taken when it is
 * not the duration, so "for ten minutes" never becomes a label.
 */
static void extract_label(const char *text, char *out, size_t size)
{
    static const char *const MARKERS[] = {" for the ", " for my ", " called "};
    const char *stop;
    size_t i;

    out[0] = '\0';
    for (i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); ++i) {
        const char *hit = strstr(text, MARKERS[i]);
        size_t length;

        if (!hit)
            continue;
        hit += strlen(MARKERS[i]);
        /* A label runs to the next "for", so "for the pasta for ten minutes"
           names the pasta and not the duration after it. */
        stop = strstr(hit, " for ");
        length = stop ? (size_t)(stop - hit) : strlen(hit);
        if (copy_label(out, size, hit, length))
            return;
    }
}

static void extract_cancel_label(const char *text, char *out, size_t size)
{
    static const char *const NOUNS[] = {"timer", "timers", "alarm", "alarms"};
    const char *verb;
    const char *cancel;
    const char *noun = NULL;
    const char *marker;
    const char *start;
    const char *stop;
    size_t verb_length;
    size_t length;
    size_t i;

    out[0] = '\0';
    verb = first_cancel_verb(text, &verb_length);
    if (!verb)
        return;
    cancel = verb + verb_length;
    while (*cancel == ' ')
        ++cancel;

    /* "cancel timer called pasta" and "delete the timer for the pasta". */
    marker = strstr(cancel, " called ");
    if (!marker)
        marker = strstr(cancel, " for the ");
    if (!marker)
        marker = strstr(cancel, " for my ");
    if (marker) {
        if (!strncmp(marker, " called ", strlen(" called ")))
            start = marker + strlen(" called ");
        else if (!strncmp(marker, " for the ", strlen(" for the ")))
            start = marker + strlen(" for the ");
        else
            start = marker + strlen(" for my ");
        stop = strstr(start, " for ");
        length = stop ? (size_t)(stop - start) : strlen(start);
        (void)copy_cancel_label(out, size, start, length);
        return;
    }

    /* "cancel the pasta timer" / "remove the kitchen alarm". */
    for (i = 0; i < sizeof(NOUNS) / sizeof(NOUNS[0]); ++i) {
        const char *hit = find_word(cancel, NOUNS[i]);

        if (hit && (!noun || hit > noun))
            noun = hit;
    }
    if (!noun)
        return;
    start = cancel;
    if (!strncmp(start, "the ", 4))
        start += 4;
    else if (!strncmp(start, "my ", 3))
        start += 3;
    else if (!strncmp(start, "a ", 2))
        start += 2;
    else if (!strncmp(start, "an ", 3))
        start += 3;
    length = (size_t)(noun - start);
    (void)copy_cancel_label(out, size, start, length);
}

static int has_later_timer_noun(const char *text)
{
    static const char *const NOUNS[] = {"timer", "timers", "alarm", "alarms"};
    size_t i;

    for (i = 0; i < sizeof(NOUNS) / sizeof(NOUNS[0]); ++i)
        if (starts_word(text, NOUNS[i]) || find_word(text, NOUNS[i]))
            return 1;
    return 0;
}

static int has_universal_timer_noun(const char *text)
{
    static const char *const QUANTIFIERS[] = {"all", "every", "each"};
    static const char *const NOUNS[] = {"timer", "timers", "alarm", "alarms"};
    static const char *const DETERMINERS[] = {
        "the", "my", "these", "those"
    };
    size_t i;

    for (i = 0; i < sizeof(QUANTIFIERS) / sizeof(QUANTIFIERS[0]); ++i) {
        const char *cursor = text;
        const char *quantifier;

        while ((quantifier = find_word(cursor, QUANTIFIERS[i]))) {
            const char *noun = quantifier + strlen(QUANTIFIERS[i]);
            size_t j;

            while (*noun == ' ')
                ++noun;
            /* "every one of my timers" and "every single timer" have
               optional words between the quantifier and the noun. Consume
               them before applying the same `of`/determiner walk as
               "every timer". */
            if ((!strcmp(QUANTIFIERS[i], "every") ||
                 !strcmp(QUANTIFIERS[i], "each")) &&
                starts_word(noun, "single"))
                noun = after_word(noun, "single");
            if ((!strcmp(QUANTIFIERS[i], "every") ||
                 !strcmp(QUANTIFIERS[i], "each")) &&
                starts_word(noun, "one"))
                noun = after_word(noun, "one");
            if (starts_word(noun, "of"))
                noun = after_word(noun, "of");
            for (j = 0; j < sizeof(DETERMINERS) / sizeof(DETERMINERS[0]);
                 ++j) {
                if (starts_word(noun, DETERMINERS[j])) {
                    noun = after_word(noun, DETERMINERS[j]);
                    break;
                }
            }
            for (j = 0; j < sizeof(NOUNS) / sizeof(NOUNS[0]); ++j) {
                if (starts_word(noun, NOUNS[j]) &&
                    !has_later_timer_noun(after_word(noun, NOUNS[j])))
                    return 1;
            }
            cursor = quantifier + strlen(QUANTIFIERS[i]);
        }
    }
    return 0;
}

static int mentions_timer(const char *text)
{
    return has_word(text, "timer") || has_word(text, "timers") ||
           has_word(text, "alarm") || has_word(text, "alarms");
}

static int negates_setting(const char *text)
{
    return has_word(text, "not") || has_word(text, "never") ||
           (has_word(text, "don") && has_word(text, "t"));
}

static int negates_cancellation(const char *text, const char *verb)
{
    const char *not = find_word(text, "not");
    const char *never = find_word(text, "never");
    const char *don = find_word(text, "don");
    const char *t = find_word(text, "t");

    return (not && not < verb) || (never && never < verb) ||
           (don && t && don < verb && t < verb);
}

enum le_timer_intent_kind le_timer_intent_parse(
    const char *transcript, struct le_timer_intent *intent)
{
    char text[NORMAL_MAX];
    const char *cancel_verb;
    size_t cancel_verb_length;
    int found = 0;
    long long seconds;

    if (!intent)
        return LE_TIMER_INTENT_NONE;
    memset(intent, 0, sizeof(*intent));
    if (!transcript || !transcript[0])
        return LE_TIMER_INTENT_NONE;
    normalise(transcript, text, sizeof(text));

    /* Silence first: while something is ringing this is the only thing
       anyone is trying to say, and it is the shortest phrasing. */
    if (has_word(text, "stop") || has_word(text, "dismiss") ||
        (has_word(text, "shut") && has_word(text, "up")) ||
        (has_word(text, "turn") && has_word(text, "off") &&
         mentions_timer(text))) {
        intent->kind = LE_TIMER_INTENT_DISMISS;
        return intent->kind;
    }

    if (!mentions_timer(text))
        return LE_TIMER_INTENT_NONE;

    cancel_verb = first_cancel_verb(text, &cancel_verb_length);
    if (cancel_verb) {
        /* A negated cancellation is not an instruction to mutate the
           schedule. Check this before extracting labels or universal
           quantifiers, because both paths can issue destructive commands. */
        if (negates_cancellation(text, cancel_verb))
            return LE_TIMER_INTENT_NONE;
        intent->kind = LE_TIMER_INTENT_CANCEL;
        intent->cancel_all = has_universal_timer_noun(
            cancel_verb + cancel_verb_length);
        if (!intent->cancel_all)
            extract_cancel_label(text, intent->label, sizeof(intent->label));
        return intent->kind;
    }

    /* "how long is left", "how much time", "what timers do I have". */
    if (has_word(text, "how") || has_word(text, "what") ||
        has_word(text, "left") || has_word(text, "remaining") ||
        has_word(text, "check")) {
        intent->kind = LE_TIMER_INTENT_QUERY;
        return intent->kind;
    }

    if (negates_setting(text))
        return LE_TIMER_INTENT_NONE;

    seconds = parse_duration(text, &found);
    if (found && seconds > 0) {
        intent->kind = LE_TIMER_INTENT_SET;
        intent->seconds = seconds;
        extract_label(text, intent->label, sizeof(intent->label));
        return intent->kind;
    }

    /*
     * "set a timer" with no duration. The model cannot start one either, and
     * guessing a length would be worse than asking, so this is a query --
     * the caller answers by asking how long.
     */
    if (has_word(text, "set") || has_word(text, "start")) {
        intent->kind = LE_TIMER_INTENT_SET;
        intent->seconds = 0;
        return intent->kind;
    }
    return LE_TIMER_INTENT_NONE;
}

int le_timer_intent_say_duration(long long seconds, char *out, size_t size)
{
    long long hours = seconds / 3600;
    long long minutes = (seconds % 3600) / 60;
    long long rest = seconds % 60;

    if (!out || !size)
        return 0;
    if (seconds <= 0)
        return snprintf(out, size, "no time");
    if (hours && minutes && rest)
        return snprintf(out, size, "%lld hour%s, %lld minute%s and %lld "
                        "second%s", hours, hours == 1 ? "" : "s", minutes,
                        minutes == 1 ? "" : "s", rest, rest == 1 ? "" : "s");
    if (hours && rest)
        return snprintf(out, size, "%lld hour%s and %lld second%s", hours,
                        hours == 1 ? "" : "s", rest, rest == 1 ? "" : "s");
    if (hours && minutes)
        return snprintf(out, size, "%lld hour%s and %lld minute%s", hours,
                        hours == 1 ? "" : "s", minutes,
                        minutes == 1 ? "" : "s");
    if (hours)
        return snprintf(out, size, "%lld hour%s", hours,
                        hours == 1 ? "" : "s");
    if (minutes && rest)
        return snprintf(out, size, "%lld minute%s and %lld second%s", minutes,
                        minutes == 1 ? "" : "s", rest, rest == 1 ? "" : "s");
    if (minutes)
        return snprintf(out, size, "%lld minute%s", minutes,
                        minutes == 1 ? "" : "s");
    return snprintf(out, size, "%lld second%s", rest, rest == 1 ? "" : "s");
}

int le_timer_intent_speech(const struct le_timer_intent *intent, int count,
                           long long seconds, char *out, size_t size)
{
    char duration[64];

    if (!intent || !out || !size)
        return 0;

    switch (intent->kind) {
    case LE_TIMER_INTENT_SET:
        if (intent->seconds <= 0)
            return snprintf(out, size, "How long should the timer be?");
        le_timer_intent_say_duration(intent->seconds, duration,
                                     sizeof(duration));
        if (intent->label[0])
            return snprintf(out, size, "%s timer for %s, starting now.",
                            duration, intent->label);
        return snprintf(out, size, "%s, starting now.", duration);
    case LE_TIMER_INTENT_CANCEL:
        if (count < 0)
            return snprintf(out, size, "Which timer should I cancel?");
        if (count == 0)
            return snprintf(out, size, "There are no timers to cancel.");
        if (count == 1)
            return snprintf(out, size, "Timer cancelled.");
        return snprintf(out, size, "All %d timers cancelled.", count);
    case LE_TIMER_INTENT_DISMISS:
        return snprintf(out, size, "%s", "");
    case LE_TIMER_INTENT_QUERY:
        if (count <= 0)
            return snprintf(out, size, "There are no timers set.");
        le_timer_intent_say_duration(seconds, duration, sizeof(duration));
        if (count == 1)
            return snprintf(out, size, "%s left.", duration);
        return snprintf(out, size, "%d timers, the next in %s.", count,
                        duration);
    case LE_TIMER_INTENT_NONE:
    default:
        out[0] = '\0';
        return 0;
    }
}
