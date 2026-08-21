#define _POSIX_C_SOURCE 200809L

#include "adapter/llm_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char directory[] = "/tmp/libreecho-llm-store-XXXXXX";
    char path[256];
    struct le_llm_credentials saved;
    struct le_llm_credentials loaded;
    struct stat status;

    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof(path), "%s/oauth.json", directory);
    memset(&saved, 0, sizeof(saved));
    strcpy(saved.access_token, "access.token.value");
    strcpy(saved.refresh_token, "refresh-token-value");
    strcpy(saved.account_id, "account-123");
    saved.expires_at = (time_t)1900000000;
    CHECK(le_llm_credentials_save(path, &saved) == 0);
    CHECK(stat(path, &status) == 0);
    CHECK((status.st_mode & 0777) == 0600);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(le_llm_credentials_load(path, &loaded) == 0);
    CHECK(!strcmp(loaded.access_token, saved.access_token));
    CHECK(!strcmp(loaded.refresh_token, saved.refresh_token));
    CHECK(!strcmp(loaded.account_id, saved.account_id));
    CHECK(loaded.expires_at == saved.expires_at);
    le_llm_credentials_clear(&loaded);
    CHECK(loaded.access_token[0] == '\0');
    CHECK(le_llm_credentials_remove(path) == 0);
    CHECK(access(path, F_OK) != 0);
    memset(&saved, 0, sizeof(saved));
    strcpy(saved.api_key, "local-api-key");
    strcpy(saved.base_url, "http://127.0.0.1:8000/v1");
    CHECK(le_llm_credentials_save(path, &saved) == 0);
    CHECK(le_llm_credentials_load(path, &loaded) == 0);
    CHECK(!strcmp(loaded.api_key, saved.api_key));
    CHECK(!strcmp(loaded.base_url, saved.base_url));
    CHECK(loaded.access_token[0] == '\0');
    CHECK(le_llm_credentials_remove(path) == 0);

    /* Keyless openai-compatible endpoint: base_url only, no api key or OAuth
       tokens. It must round-trip; previously load() rejected it and the local
       LLM appeared signed-out after an agentd restart. */
    memset(&saved, 0, sizeof(saved));
    strcpy(saved.base_url, "http://127.0.0.1:11434/v1");
    CHECK(le_llm_credentials_save(path, &saved) == 0);
    memset(&loaded, 0, sizeof(loaded));
    CHECK(le_llm_credentials_load(path, &loaded) == 0);
    CHECK(!strcmp(loaded.base_url, saved.base_url));
    CHECK(loaded.api_key[0] == '\0');
    CHECK(loaded.access_token[0] == '\0');
    CHECK(le_llm_credentials_remove(path) == 0);
    rmdir(directory);
    puts("llm store: private atomic OAuth and local credentials: ok");
    return 0;
}
