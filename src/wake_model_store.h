#ifndef LIBREECHO_WAKE_MODEL_STORE_H
#define LIBREECHO_WAKE_MODEL_STORE_H

#include <stddef.h>

/*
 * Owner-staged wake models as the web control centre manages them: list,
 * stage in chunks, commit, select and delete. Only the web daemon links this;
 * waked reads the selection through the init script and needs only wake_model.h.
 *
 * Layout under the configuration directory's parent (normally /data/libreecho):
 *   wakeword-models/<name>/          committed model folders
 *   wakeword-models/.staging/<name>/ folder being uploaded, not yet committed
 *   config/wake-model                one line: the selected folder name
 */

#define LE_WAKE_MODEL_STAGE_CHUNK_MAX 8192UL

#define LE_WAKE_MODEL_STORE_OK 0
#define LE_WAKE_MODEL_STORE_INVALID (-1)
#define LE_WAKE_MODEL_STORE_CONFLICT (-2)
#define LE_WAKE_MODEL_STORE_NOT_FOUND (-3)
#define LE_WAKE_MODEL_STORE_IO (-4)

struct le_wake_model_paths {
    char models[320];
    char staging[336];
    char selection[320];
};

struct le_wake_model_entry {
    char name[64];
    int valid;
    char reason[160];
    unsigned long bytes;
    int selected;
};

typedef void (*le_wake_model_visitor)(const struct le_wake_model_entry *entry, void *opaque);

/* Derives the paths from the web configuration file path. Returns 0 or -1. */
int le_wake_model_paths_from_config(const char *config_path, struct le_wake_model_paths *paths);

/* Reads the selected folder name. Returns 1 when a folder is selected (name
 * written), 0 for the stock model, -1 when the selection file is unreadable. */
int le_wake_model_selection_read(const struct le_wake_model_paths *paths,
                                 char *name, size_t name_size);

/* Visits every committed folder with its validation result. Returns the count. */
int le_wake_model_scan(const struct le_wake_model_paths *paths,
                       le_wake_model_visitor visitor, void *opaque);

/* Writes one chunk of one file into the staging folder. offset must equal the
 * bytes already staged for that file (offset 0 starts it over). */
int le_wake_model_stage_chunk(const struct le_wake_model_paths *paths, const char *name,
                              const char *file, unsigned long offset,
                              const unsigned char *data, size_t length,
                              unsigned long *staged_bytes);

/* Validates the staged folder and moves it into place. On rejection the staged
 * files are removed and reason explains why. */
int le_wake_model_commit(const struct le_wake_model_paths *paths, const char *name,
                         char *reason, size_t reason_size);

/* Selects a committed folder, or the stock model when name is NULL or empty. */
int le_wake_model_select(const struct le_wake_model_paths *paths, const char *name);

/* Removes a committed folder. Refuses the selected folder. */
int le_wake_model_delete(const struct le_wake_model_paths *paths, const char *name);

/* Decodes standard base64 into out. Returns the decoded length or -1. */
int le_wake_model_base64_decode(const char *text, size_t text_length,
                                unsigned char *out, size_t out_size);

#endif
