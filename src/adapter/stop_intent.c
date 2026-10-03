#include "stop_intent.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Normalise a complete bounded utterance, never a truncated prefix. Quotes
 * inside contractions are retained as part of the word by removing them;
 * neither "don't" nor a curly-apostrophe spelling can become "stop". */
int le_stop_intent_matches(const char *transcript)
{
    static const char *const commands[] = {
        "stop", "stop it", "stop playing", "stop playback",
        "stop the music", "stop music", "stop the radio", "stop radio",
        "stop the song", "stop the sound", "stop audio", "stop the audio",
        "stop talking", "stop speaking", "stop speech", "stop the noise",
        "stop noise", "stop the timer", "stop timer", "stop the alarm",
        "stop alarm", "dismiss", "dismiss the timer", "dismiss the alarm",
        "quiet", "be quiet", "silence", "shut up",
        "turn off the music", "turn the music off", "turn off music",
        "turn off the radio", "turn the radio off", "turn off radio",
        "turn off the sound", "turn off the noise", "turn the noise off",
        "turn off the timer", "turn the timer off", "turn off the alarm",
        "turn the alarm off"
    };
    static const char *const prefixes[] = {
        "alexa ", "please ", "can you ", "could you ", "would you "
    };
    char text[256];
    const unsigned char *input = (const unsigned char *)transcript;
    char *command;
    size_t n = 0, i, consumed = 0;
    unsigned pass;

    if (!input)
        return 0;
    while (*input) {
        unsigned char c = *input++;
        if (++consumed >= sizeof(text))
            return 0;
        if (c == 0xe2 && input[0] == 0x80 && input[1] == 0x99) {
            input += 2;
            consumed += 2;
            continue;
        }
        if (c >= 0x80)
            return 0;
        if (c == '\'')
            continue;
        if (isalpha(c)) {
            if (n + 1 >= sizeof(text))
                return 0;
            text[n++] = (char)tolower(c);
        } else if (isspace(c) || c == '.' || c == '!' || c == '?' || c == ',') {
            if (n && text[n - 1] != ' ')
                text[n++] = ' ';
        } else {
            return 0;
        }
    }
    if (n && text[n - 1] == ' ')
        --n;
    text[n] = '\0';
    command = text;
    for (pass = 0; pass < 3; ++pass) {
        for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
            size_t length = strlen(prefixes[i]);
            if (!strncmp(command, prefixes[i], length)) {
                command += length;
                break;
            }
        }
        if (i == sizeof(prefixes) / sizeof(prefixes[0]))
            break;
    }
    n = strlen(command);
    if (n > 7 && !strcmp(command + n - 7, " please"))
        command[n - 7] = '\0';
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i)
        if (!strcmp(command, commands[i]))
            return 1;
    return 0;
}
