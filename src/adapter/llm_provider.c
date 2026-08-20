#include "llm_provider.h"

#include <string.h>

const struct le_llm_provider *le_llm_codex_provider(void);
const struct le_llm_provider *le_llm_openai_provider(void);

const struct le_llm_provider *le_llm_provider_by_id(const char *id)
{
    const struct le_llm_provider *provider;

    if (!id)
        return NULL;
    provider = le_llm_codex_provider();
    if (provider && !strcmp(provider->id, id))
        return provider;
    provider = le_llm_openai_provider();
    return provider && !strcmp(provider->id, id) ? provider : NULL;
}

const char *le_llm_default_voice_prompt(void)
{
    return
        "You are the voice assistant built into LibreEcho. Reply in concise, "
        "natural spoken English. Use one or two short sentences unless the "
        "user explicitly asks for more detail. Do not use markdown, lists, "
        "code blocks, URLs, citations, emoji, or stage directions. Avoid "
        "preambles. Never claim that you performed a device action unless a "
        "provided tool confirms it.";
}
