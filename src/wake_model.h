#ifndef LIBREECHO_WAKE_MODEL_H
#define LIBREECHO_WAKE_MODEL_H

#include <stddef.h>

/*
 * Owner-staged wake models: the host-side contract shared by the web control
 * centre (validation before a model is accepted or selected) and waked (the
 * label it reports for the model it is running).
 *
 * The rules mirror docs/OPERATIONS.md and the device contract in
 * docs/wake-word-training.md. A model is a folder holding exactly
 * melspectrogram.onnx, embedding_model.onnx and alexa_v0.1.onnx (the classifier,
 * whatever the word is). Only graph input/output names, operator set, opset,
 * external data references and file sizes are checked here; tensor shapes are
 * left to the detector, which refuses a model it cannot load and falls back.
 */

#define LE_WAKE_MODEL_STOCK_NAME "alexa_v0.1"
#define LE_WAKE_MODEL_STOCK_PHRASE "Alexa"
#define LE_WAKE_MODEL_NAME_MAX 64
#define LE_WAKE_MODEL_MIN_BYTES 1024UL
#define LE_WAKE_MODEL_MAX_BYTES (8UL * 1024UL * 1024UL)
#define LE_WAKE_MODEL_FILE_COUNT 3

extern const char *const le_wake_model_files[LE_WAKE_MODEL_FILE_COUNT];

/* 1..64 characters from [A-Za-z0-9._-], not starting with '.'. */
int le_wake_model_name_valid(const char *name);

/* Writes the phrase and the model name waked reports for a model directory.
 * The stock directory (basename "openwakeword") reports the stock labels; any
 * other directory reports its own folder name. Returns 0 or -1. */
int le_wake_model_label(const char *model_directory, char *phrase, size_t phrase_size,
                        char *model, size_t model_size);

/* Validates one staged file of a model folder: regular file (not a symlink),
 * size within bounds, and for the three known names the graph contract. On
 * rejection writes a short reason into reason. Returns 0 when accepted. */
int le_wake_model_validate_file(const char *path, const char *file_name,
                                char *reason, size_t reason_size);

/* Validates a whole model folder: a real directory holding exactly the three
 * known files, each passing le_wake_model_validate_file. Returns 0 when accepted. */
int le_wake_model_validate_directory(const char *directory,
                                    char *reason, size_t reason_size);

#endif
