#ifndef LIBREECHO_STOP_INTENT_H
#define LIBREECHO_STOP_INTENT_H

/* Conservative local commands: never send an unambiguous stop to an LLM. */
int le_stop_intent_matches(const char *transcript);

#endif
