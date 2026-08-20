#include "voice_reply.h"

#include <ctype.h>
#include <string.h>

static void trim(char *text)
{
    char *start = text;
    size_t length;

    while (*start && isspace((unsigned char)*start))
        ++start;
    if (start != text)
        memmove(text, start, strlen(start) + 1);
    length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1]))
        text[--length] = '\0';
}

static int emit_pending(struct le_voice_reply *reply)
{
    int result;

    reply->pending[reply->used] = '\0';
    trim(reply->pending);
    if (!reply->pending[0]) {
        reply->used = 0;
        return 0;
    }
    result = reply->emit
        ? reply->emit(reply->context, reply->pending) : 0;
    if (result == 0)
        ++reply->emitted;
    reply->used = 0;
    reply->pending[0] = '\0';
    return result;
}

void le_voice_reply_init(struct le_voice_reply *reply,
                         le_voice_reply_segment_fn emit,
                         void *context)
{
    memset(reply, 0, sizeof(*reply));
    reply->emit = emit;
    reply->context = context;
}

int le_voice_reply_feed(struct le_voice_reply *reply, const char *delta)
{
    if (!reply || !delta)
        return -1;
    while (*delta) {
        unsigned char c = (unsigned char)*delta++;
        int first_phrase_end;
        int sentence_end;

        if (reply->used + 1 >= sizeof(reply->pending) &&
            emit_pending(reply) != 0)
            return -1;
        reply->pending[reply->used++] = (char)c;
        sentence_end = c == '.' || c == '?' || c == '!';
        if (sentence_end && emit_pending(reply) != 0)
            return -1;
        first_phrase_end =
            reply->emitted == 0 &&
            (((c == ',' || c == ';' || c == ':') &&
              reply->used >= LE_VOICE_REPLY_FIRST_SOFT_LIMIT) ||
             (isspace(c) &&
              reply->used >= LE_VOICE_REPLY_FIRST_HARD_LIMIT));
        if (first_phrase_end && emit_pending(reply) != 0)
            return -1;
        if (reply->emitted != 0 && (c == ';' || c == ':') &&
            reply->used >= 120 &&
            emit_pending(reply) != 0)
            return -1;
    }
    return 0;
}

int le_voice_reply_finish(struct le_voice_reply *reply)
{
    if (!reply)
        return -1;
    return emit_pending(reply);
}
