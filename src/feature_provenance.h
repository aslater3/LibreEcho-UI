#ifndef LIBREECHO_FEATURE_PROVENANCE_H
#define LIBREECHO_FEATURE_PROVENANCE_H

#include <stddef.h>

#define LE_FEATURE_COMPONENTS_JSON_MAX 8192

typedef struct {
    int pending;
    int reboot_required;
    int commit_pending;
    int rollback;
    char state[16];
    char last_result[32];
} le_feature_transaction_state;

void le_feature_transaction_state_read(le_feature_transaction_state *state);
void le_feature_components_json(char *out, size_t size,
                                const le_feature_transaction_state *transaction);
void le_feature_provenance_tick(void);
void le_feature_provenance_shutdown(void);

#endif
