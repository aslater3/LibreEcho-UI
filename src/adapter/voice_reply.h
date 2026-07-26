#ifndef LIBREECHO_VOICE_REPLY_H
#define LIBREECHO_VOICE_REPLY_H

#include <stddef.h>

#define LE_VOICE_REPLY_SEGMENT_MAX 384
#define LE_VOICE_REPLY_FIRST_SOFT_LIMIT 32
#define LE_VOICE_REPLY_FIRST_HARD_LIMIT 48

typedef int (*le_voice_reply_segment_fn)(void *context,
                                         const char *text);

struct le_voice_reply {
    char pending[LE_VOICE_REPLY_SEGMENT_MAX];
    size_t used;
    size_t emitted;
    le_voice_reply_segment_fn emit;
    void *context;
};

void le_voice_reply_init(struct le_voice_reply *reply,
                         le_voice_reply_segment_fn emit,
                         void *context);
int le_voice_reply_feed(struct le_voice_reply *reply, const char *delta);
int le_voice_reply_finish(struct le_voice_reply *reply);

#endif
