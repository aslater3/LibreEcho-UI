#define _POSIX_C_SOURCE 200809L

#include "wake_model.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const le_wake_model_files[LE_WAKE_MODEL_FILE_COUNT] = {
    "melspectrogram.onnx",
    "embedding_model.onnx",
    "alexa_v0.1.onnx",
};

/* The graph contract for each file, as docs/wake-word-training.md section 1. */
struct wake_model_contract {
    const char *file;
    const char *input;
    const char *output;
    int restrict_operators;
};

static const struct wake_model_contract contracts[LE_WAKE_MODEL_FILE_COUNT] = {
    {"melspectrogram.onnx", "input", "output", 0},
    {"embedding_model.onnx", "input_1", "conv2d_19", 0},
    {"alexa_v0.1.onnx", "onnx::Flatten_0", "13", 1},
};

static const char *const classifier_operators[] = {"Flatten", "Gemm", "Relu", "Sigmoid"};

static const struct wake_model_contract *contract_for(const char *file)
{
    size_t i;

    for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i)
        if (!strcmp(file, contracts[i].file))
            return &contracts[i];
    return NULL;
}

int le_wake_model_name_valid(const char *name)
{
    size_t i, length;

    if (!name)
        return 0;
    length = strlen(name);
    if (length == 0 || length >= LE_WAKE_MODEL_NAME_MAX || name[0] == '.')
        return 0;
    for (i = 0; i < length; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int le_wake_model_label(const char *model_directory, char *phrase, size_t phrase_size,
                        char *model, size_t model_size)
{
    char trimmed[LE_WAKE_MODEL_NAME_MAX + 256];
    const char *base;
    size_t length;

    if (!model_directory || !phrase || !model || !phrase_size || !model_size)
        return -1;
    length = strlen(model_directory);
    if (length >= sizeof(trimmed))
        return -1;
    memcpy(trimmed, model_directory, length + 1);
    while (length > 1 && trimmed[length - 1] == '/')
        trimmed[--length] = '\0';
    base = strrchr(trimmed, '/');
    base = base ? base + 1 : trimmed;
    if (!base[0] || !strcmp(base, "openwakeword")) {
        if (snprintf(phrase, phrase_size, "%s", LE_WAKE_MODEL_STOCK_PHRASE) >= (int)phrase_size ||
            snprintf(model, model_size, "%s", LE_WAKE_MODEL_STOCK_NAME) >= (int)model_size)
            return -1;
        return 0;
    }
    /* Only a name the selection contract accepts is reported; anything else
     * is refused so the status never carries an unescaped owner string. */
    if (!le_wake_model_name_valid(base))
        return -1;
    if (snprintf(phrase, phrase_size, "%s", base) >= (int)phrase_size ||
        snprintf(model, model_size, "%s", base) >= (int)model_size)
        return -1;
    return 0;
}

/* ---- Minimal protobuf wire reader (enough for ONNX ModelProto) ---- */

struct pb_cursor {
    const unsigned char *p;
    const unsigned char *end;
};

struct pb_field {
    unsigned number;
    unsigned wire;
    unsigned long long value;
    const unsigned char *data;
    size_t length;
};

static int pb_varint(struct pb_cursor *c, unsigned long long *value)
{
    unsigned shift = 0;

    *value = 0;
    while (c->p < c->end && shift < 64) {
        unsigned char b = *c->p++;
        *value |= (unsigned long long)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return 0;
        shift += 7;
    }
    return -1;
}

static int pb_next(struct pb_cursor *c, struct pb_field *f)
{
    unsigned long long tag, n;

    if (c->p >= c->end || pb_varint(c, &tag))
        return -1;
    f->number = (unsigned)(tag >> 3);
    f->wire = (unsigned)(tag & 7u);
    f->value = 0;
    f->data = NULL;
    f->length = 0;
    if (f->number == 0)
        return -1;
    switch (f->wire) {
    case 0:
        return pb_varint(c, &f->value);
    case 1:
        if ((size_t)(c->end - c->p) < 8)
            return -1;
        c->p += 8;
        return 0;
    case 5:
        if ((size_t)(c->end - c->p) < 4)
            return -1;
        c->p += 4;
        return 0;
    case 2:
        if (pb_varint(c, &n) || n > (unsigned long long)(c->end - c->p))
            return -1;
        f->data = c->p;
        f->length = (size_t)n;
        c->p += n;
        return 0;
    default:
        return -1;
    }
}

static int string_is(const struct pb_field *f, const char *text)
{
    size_t n = strlen(text);
    return f->wire == 2 && f->length == n && !memcmp(f->data, text, n);
}

/* ---- ONNX graph scan ---- */

struct graph_scan {
    const char *expected_input;
    const char *expected_output;
    int input_present;
    unsigned output_count;
    int output_matches;
    unsigned node_count;
    int operator_refused;
    int sigmoid_produces_output;
    int external_data;
    int opset_ok;
};

static int operator_allowed(const struct pb_field *op, int restrict_operators)
{
    size_t i;

    if (!restrict_operators)
        return 1;
    for (i = 0; i < sizeof(classifier_operators) / sizeof(classifier_operators[0]); ++i)
        if (string_is(op, classifier_operators[i]))
            return 1;
    return 0;
}

static void scan_node(const unsigned char *data, size_t length, struct graph_scan *scan,
                      int restrict_operators)
{
    struct pb_cursor c = {data, data + length};
    struct pb_field f, op = {0};
    int produces_output = 0, op_seen = 0, sigmoid = 0;

    while (c.p < c.end) {
        if (pb_next(&c, &f))
            return;
        if (f.number == 4 && f.wire == 2) {
            op = f;
            op_seen = 1;
        } else if (f.number == 2 && f.wire == 2 && string_is(&f, scan->expected_output)) {
            produces_output = 1;
        }
    }
    ++scan->node_count;
    if (!op_seen || !operator_allowed(&op, restrict_operators))
        scan->operator_refused = 1;
    if (op_seen && string_is(&op, "Sigmoid"))
        sigmoid = 1;
    if (produces_output && sigmoid)
        scan->sigmoid_produces_output = 1;
}

static void scan_tensor(const unsigned char *data, size_t length, struct graph_scan *scan)
{
    struct pb_cursor c = {data, data + length};
    struct pb_field f;

    while (c.p < c.end) {
        if (pb_next(&c, &f))
            return;
        /* TensorProto.external_data (13) and data_location (14, EXTERNAL = 1). */
        if (f.number == 13)
            scan->external_data = 1;
        if (f.number == 14 && f.wire == 0 && f.value == 1)
            scan->external_data = 1;
    }
}

static void scan_graph(const unsigned char *data, size_t length, struct graph_scan *scan,
                       int restrict_operators)
{
    struct pb_cursor c = {data, data + length};
    struct pb_field f;

    /* Pass one: the names. Output names are needed before nodes are read. */
    while (c.p < c.end) {
        if (pb_next(&c, &f))
            return;
        if (f.number == 11 && f.wire == 2) {
            struct pb_cursor v = {f.data, f.data + f.length};
            struct pb_field n;
            while (v.p < v.end && !pb_next(&v, &n))
                if (n.number == 1 && string_is(&n, scan->expected_input))
                    scan->input_present = 1;
        } else if (f.number == 12 && f.wire == 2) {
            struct pb_cursor v = {f.data, f.data + f.length};
            struct pb_field n;
            ++scan->output_count;
            while (v.p < v.end && !pb_next(&v, &n))
                if (n.number == 1 && string_is(&n, scan->expected_output))
                    scan->output_matches = 1;
        }
    }
    /* Pass two: nodes and initializers, now that the output name is known. */
    c.p = data;
    c.end = data + length;
    while (c.p < c.end) {
        if (pb_next(&c, &f))
            return;
        if (f.number == 1 && f.wire == 2)
            scan_node(f.data, f.length, scan, restrict_operators);
        else if (f.number == 5 && f.wire == 2)
            scan_tensor(f.data, f.length, scan);
    }
}

static void scan_model(const unsigned char *data, size_t length, struct graph_scan *scan,
                       int restrict_operators)
{
    struct pb_cursor c = {data, data + length};
    struct pb_field f;

    while (c.p < c.end) {
        if (pb_next(&c, &f))
            return;
        if (f.number == 7 && f.wire == 2)
            scan_graph(f.data, f.length, scan, restrict_operators);
        else if (f.number == 8 && f.wire == 2) {
            /* OperatorSetIdProto: domain (1), version (2). The stock classifier is opset 13. */
            struct pb_cursor v = {f.data, f.data + f.length};
            struct pb_field n;
            int domain_default = 1, version = -1;
            while (v.p < v.end && !pb_next(&v, &n)) {
                if (n.number == 1 && n.wire == 2)
                    domain_default = n.length == 0 || string_is(&n, "ai.onnx");
                else if (n.number == 2 && n.wire == 0)
                    version = (int)n.value;
            }
            if (domain_default && version == 13)
                scan->opset_ok = 1;
        }
    }
}

static int fail(char *reason, size_t reason_size, const char *text)
{
    if (reason && reason_size)
        snprintf(reason, reason_size, "%s", text);
    return -1;
}

int le_wake_model_validate_file(const char *path, const char *file_name,
                                char *reason, size_t reason_size)
{
    const struct wake_model_contract *contract;
    struct stat before, after;
    struct graph_scan scan;
    unsigned char *buffer;
    size_t total = 0;
    int fd;
    char message[160];

    if (!path || !file_name)
        return fail(reason, reason_size, "model file is missing");
    contract = contract_for(file_name);
    if (!contract)
        return fail(reason, reason_size, "unexpected model file");
    if (lstat(path, &before) < 0) {
        snprintf(message, sizeof(message), "%s is missing", file_name);
        return fail(reason, reason_size, message);
    }
    if (!S_ISREG(before.st_mode)) {
        snprintf(message, sizeof(message), "%s is not a regular file", file_name);
        return fail(reason, reason_size, message);
    }
    if (before.st_size < (off_t)LE_WAKE_MODEL_MIN_BYTES ||
        before.st_size > (off_t)LE_WAKE_MODEL_MAX_BYTES) {
        snprintf(message, sizeof(message), "%s size is outside the accepted range", file_name);
        return fail(reason, reason_size, message);
    }
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &after) < 0) {
        if (fd >= 0)
            close(fd);
        snprintf(message, sizeof(message), "%s could not be read", file_name);
        return fail(reason, reason_size, message);
    }
    if (!S_ISREG(after.st_mode) || after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_size != before.st_size) {
        close(fd);
        snprintf(message, sizeof(message), "%s changed while it was read", file_name);
        return fail(reason, reason_size, message);
    }
    buffer = malloc((size_t)after.st_size);
    if (!buffer) {
        close(fd);
        return fail(reason, reason_size, "model could not be checked: out of memory");
    }
    while (total < (size_t)after.st_size) {
        ssize_t n = read(fd, buffer + total, (size_t)after.st_size - total);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (total != (size_t)after.st_size) {
        free(buffer);
        snprintf(message, sizeof(message), "%s could not be read", file_name);
        return fail(reason, reason_size, message);
    }

    memset(&scan, 0, sizeof(scan));
    scan.expected_input = contract->input;
    scan.expected_output = contract->output;
    scan_model(buffer, total, &scan, contract->restrict_operators);
    free(buffer);

    if (scan.external_data) {
        snprintf(message, sizeof(message), "%s refers to external weight data", file_name);
        return fail(reason, reason_size, message);
    }
    if (!scan.input_present || !scan.output_matches || scan.output_count != 1) {
        snprintf(message, sizeof(message),
                 "%s graph inputs or outputs do not match the contract", file_name);
        return fail(reason, reason_size, message);
    }
    if (contract->restrict_operators) {
        if (!scan.opset_ok) {
            snprintf(message, sizeof(message), "%s is not opset 13", file_name);
            return fail(reason, reason_size, message);
        }
        if (scan.node_count == 0 || scan.operator_refused) {
            snprintf(message, sizeof(message), "%s uses operators outside the contract", file_name);
            return fail(reason, reason_size, message);
        }
        if (!scan.sigmoid_produces_output) {
            snprintf(message, sizeof(message), "%s does not end in Sigmoid", file_name);
            return fail(reason, reason_size, message);
        }
    }
    if (reason && reason_size)
        reason[0] = '\0';
    return 0;
}

int le_wake_model_validate_directory(const char *directory, char *reason, size_t reason_size)
{
    struct stat st;
    DIR *dir;
    struct dirent *entry;
    size_t i, found = 0;
    char path[1024];

    if (!directory)
        return fail(reason, reason_size, "model directory is missing");
    if (lstat(directory, &st) < 0 || !S_ISDIR(st.st_mode))
        return fail(reason, reason_size, "model directory is missing");
    dir = opendir(directory);
    if (!dir)
        return fail(reason, reason_size, "model directory could not be read");
    while ((entry = readdir(dir)) != NULL) {
        int known = 0;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i)
            if (!strcmp(entry->d_name, le_wake_model_files[i]))
                known = 1;
        if (!known) {
            closedir(dir);
            return fail(reason, reason_size, "model folder holds an unexpected file");
        }
        ++found;
    }
    closedir(dir);
    if (found != LE_WAKE_MODEL_FILE_COUNT)
        return fail(reason, reason_size, "model folder must hold exactly the three model files");
    for (i = 0; i < LE_WAKE_MODEL_FILE_COUNT; ++i) {
        if (snprintf(path, sizeof(path), "%s/%s", directory, le_wake_model_files[i]) >=
            (int)sizeof(path))
            return fail(reason, reason_size, "model path is too long");
        if (le_wake_model_validate_file(path, le_wake_model_files[i], reason, reason_size) < 0)
            return -1;
    }
    return 0;
}
