#ifndef LIBREECHO_LLM_STORE_H
#define LIBREECHO_LLM_STORE_H

#include "llm_provider.h"

int le_llm_credentials_load(const char *path,
                            struct le_llm_credentials *credentials);
int le_llm_credentials_save(const char *path,
                            const struct le_llm_credentials *credentials);
int le_llm_credentials_remove(const char *path);
void le_llm_credentials_clear(struct le_llm_credentials *credentials);

#endif
