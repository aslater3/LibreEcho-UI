/*
 * Owner-staged wake model contract: name rules, the labels waked reports, the
 * ONNX graph checks, and the web store's stage, commit, select and delete
 * flow. The ONNX files are synthesised here with a small protobuf writer so
 * every rejection path has a known-bad model to reject.
 */
#define _POSIX_C_SOURCE 200809L

#include "wake_model.h"
#include "wake_model_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static int checks;

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        }                                                                       \
    } while (0)

/* ---- protobuf writer ---- */

struct writer {
    unsigned char *p;
    size_t n;
    size_t cap;
};

static void put_byte(struct writer *w, unsigned char b)
{
    if (w->n < w->cap)
        w->p[w->n] = b;
    ++w->n;
}

static void put_varint(struct writer *w, unsigned long long v)
{
    while (v >= 0x80) {
        put_byte(w, (unsigned char)(v | 0x80));
        v >>= 7;
    }
    put_byte(w, (unsigned char)v);
}

static void put_tag(struct writer *w, unsigned field, unsigned wire)
{
    put_varint(w, ((unsigned long long)field << 3) | wire);
}

static void put_message(struct writer *w, unsigned field, const struct writer *sub)
{
    size_t i;

    put_tag(w, field, 2);
    put_varint(w, sub->n);
    for (i = 0; i < sub->n && i < sub->cap; ++i)
        put_byte(w, sub->p[i]);
}

static void put_string(struct writer *w, unsigned field, const char *text)
{
    struct writer sub = {(unsigned char *)text, strlen(text), strlen(text)};

    put_message(w, field, &sub);
}

static void put_uint(struct writer *w, unsigned field, unsigned long long value)
{
    put_tag(w, field, 0);
    put_varint(w, value);
}

/* ---- model builder ---- */

struct model_spec {
    const char *input;
    const char *output;
    const char *ops[6];
    int op_count;
    unsigned long long opset;
    int external;
    size_t pad;
};

static size_t build_model(unsigned char *out, size_t cap, const struct model_spec *spec)
{
    static unsigned char graph_bytes[8192];
    static unsigned char model_bytes[16384];
    struct writer graph = {graph_bytes, 0, sizeof(graph_bytes)};
    struct writer model = {model_bytes, 0, sizeof(model_bytes)};
    struct writer opset = {NULL, 0, 0};
    unsigned char opset_bytes[32];
    int i;

    for (i = 0; i < spec->op_count; ++i) {
        struct writer node = {NULL, 0, 0};
        unsigned char node_bytes[256];
        char produced[32];

        node.p = node_bytes;
        node.cap = sizeof(node_bytes);
        node.n = 0;
        put_string(&node, 1, spec->input);
        if (i == spec->op_count - 1)
            put_string(&node, 2, spec->output);
        else {
            snprintf(produced, sizeof(produced), "t%d", i);
            put_string(&node, 2, produced);
        }
        put_string(&node, 4, spec->ops[i]);
        put_message(&graph, 1, &node);
    }
    if (spec->external) {
        struct writer tensor = {NULL, 0, 0};
        unsigned char tensor_bytes[64];
        tensor.p = tensor_bytes;
        tensor.cap = sizeof(tensor_bytes);
        put_string(&tensor, 8, "weights");
        put_uint(&tensor, 14, 1);
        put_message(&graph, 5, &tensor);
    }
    {
        struct writer value = {NULL, 0, 0};
        unsigned char value_bytes[128];
        value.p = value_bytes;
        value.cap = sizeof(value_bytes);
        put_string(&value, 1, spec->input);
        put_message(&graph, 11, &value);
    }
    {
        struct writer value = {NULL, 0, 0};
        unsigned char value_bytes[128];
        value.p = value_bytes;
        value.cap = sizeof(value_bytes);
        put_string(&value, 1, spec->output);
        put_message(&graph, 12, &value);
    }
    opset.p = opset_bytes;
    opset.cap = sizeof(opset_bytes);
    put_uint(&opset, 2, spec->opset);
    put_message(&model, 8, &opset);
    put_message(&model, 7, &graph);
    {
        /* Padding keeps the file above the minimum size. */
        size_t j;
        unsigned char *pad = malloc(spec->pad + 1);
        struct writer doc = {NULL, 0, 0};

        if (pad) {
            for (j = 0; j < spec->pad; ++j)
                pad[j] = 'x';
            doc.p = pad;
            doc.cap = spec->pad;
            doc.n = spec->pad;
            put_message(&model, 6, &doc);
            free(pad);
        }
    }
    if (model.n > cap)
        return 0;
    memcpy(out, model_bytes, model.n);
    return model.n;
}

/* ---- filesystem helpers ---- */

static char root[512];

static void make_root(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof(root), "%s/wake-model-test-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        exit(1);
    }
}

static void write_file(const char *path, const unsigned char *data, size_t length)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fwrite(data, 1, length, f);
    fclose(f);
}

static void path_in(char *out, size_t size, const char *dir, const char *name)
{
    snprintf(out, size, "%s/%s", dir, name);
}

static const struct model_spec classifier_good = {
    "onnx::Flatten_0", "13", {"Flatten", "Gemm", "Relu", "Sigmoid"}, 4, 13, 0, 1100};

static const struct model_spec feature_mel = {"input", "output", {"Mel"}, 1, 13, 0, 1100};
static const struct model_spec feature_embedding = {
    "input_1", "conv2d_19", {"Conv"}, 1, 13, 0, 1100};

static void check_names(void)
{
    CHECK(le_wake_model_name_valid("my-word"));
    CHECK(le_wake_model_name_valid("Hey_Kitchen.2"));
    CHECK(!le_wake_model_name_valid(""));
    CHECK(!le_wake_model_name_valid(".hidden"));
    CHECK(!le_wake_model_name_valid("../escape"));
    CHECK(!le_wake_model_name_valid("with space"));
    CHECK(!le_wake_model_name_valid("slash/name"));
    CHECK(!le_wake_model_name_valid("quote\""));
    CHECK(!le_wake_model_name_valid(NULL));
}

static void check_labels(void)
{
    char phrase[64], model[64];

    CHECK(le_wake_model_label("/usr/share/libreecho/openwakeword", phrase, sizeof(phrase),
                              model, sizeof(model)) == 0);
    CHECK(!strcmp(phrase, "Alexa"));
    CHECK(!strcmp(model, "alexa_v0.1"));
    CHECK(le_wake_model_label("/data/libreecho/wakeword-models/my-word/", phrase,
                              sizeof(phrase), model, sizeof(model)) == 0);
    CHECK(!strcmp(phrase, "my-word"));
    CHECK(!strcmp(model, "my-word"));
    CHECK(le_wake_model_label("/data/libreecho/wakeword-models/bad name", phrase,
                              sizeof(phrase), model, sizeof(model)) < 0);
}

static void check_contract(void)
{
    char dir[600], path[700], reason[200];
    unsigned char bytes[16384];
    size_t length;

    make_root();
    path_in(dir, sizeof(dir), root, "contract");
    mkdir(dir, 0700);

    path_in(path, sizeof(path), dir, "alexa_v0.1.onnx");
    length = build_model(bytes, sizeof(bytes), &classifier_good);
    write_file(path, bytes, length);
    CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) == 0);

    {
        struct model_spec s = classifier_good;
        s.output = "14";
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
    }
    {
        struct model_spec s = classifier_good;
        s.input = "input";
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
    }
    {
        struct model_spec s = classifier_good;
        s.ops[1] = "Conv";
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "operators") != NULL);
    }
    {
        struct model_spec s = classifier_good;
        s.ops[3] = "Relu";
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "Sigmoid") != NULL);
    }
    {
        struct model_spec s = classifier_good;
        s.opset = 17;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
    }
    {
        struct model_spec s = classifier_good;
        s.external = 1;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "external") != NULL);
    }
    {
        struct model_spec s = classifier_good;
        s.pad = 0;
        length = build_model(bytes, sizeof(bytes), &s);
        write_file(path, bytes, length);
        CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", reason, sizeof(reason)) < 0);
        CHECK(strstr(reason, "size") != NULL);
    }
    {
        /* A symlink to a good model is refused, not followed. */
        char link_path[700];
        path_in(link_path, sizeof(link_path), dir, "link.onnx");
        length = build_model(bytes, sizeof(bytes), &classifier_good);
        write_file(path, bytes, length);
        symlink(path, link_path);
        CHECK(le_wake_model_validate_file(link_path, "alexa_v0.1.onnx", reason,
                                          sizeof(reason)) < 0);
        unlink(link_path);
    }
    CHECK(le_wake_model_validate_file(path, "unknown.onnx", reason, sizeof(reason)) < 0);
    CHECK(le_wake_model_validate_file(path, "alexa_v0.1.onnx", NULL, 0) == 0);
}

static void write_set(const char *dir, int bad_mel)
{
    unsigned char bytes[16384];
    char path[700];
    size_t length;
    struct model_spec mel = feature_mel;

    path_in(path, sizeof(path), dir, "melspectrogram.onnx");
    if (bad_mel)
        mel.input = "wrong";
    length = build_model(bytes, sizeof(bytes), &mel);
    write_file(path, bytes, length);
    path_in(path, sizeof(path), dir, "embedding_model.onnx");
    length = build_model(bytes, sizeof(bytes), &feature_embedding);
    write_file(path, bytes, length);
    path_in(path, sizeof(path), dir, "alexa_v0.1.onnx");
    length = build_model(bytes, sizeof(bytes), &classifier_good);
    write_file(path, bytes, length);
}

static void check_directory(void)
{
    char dir[600], extra[700], reason[200];

    make_root();
    path_in(dir, sizeof(dir), root, "folder");
    mkdir(dir, 0700);
    write_set(dir, 0);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) == 0);

    path_in(extra, sizeof(extra), dir, "notes.txt");
    write_file(extra, (const unsigned char *)"x", 1);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) < 0);
    unlink(extra);

    write_set(dir, 1);
    CHECK(le_wake_model_validate_directory(dir, reason, sizeof(reason)) < 0);
    CHECK(strstr(reason, "melspectrogram") != NULL);
}

/* ---- store ---- */

struct tally {
    int count;
    int valid;
    int selected;
};

static void tally_visit(const struct le_wake_model_entry *entry, void *opaque)
{
    struct tally *t = opaque;
    ++t->count;
    if (entry->valid)
        ++t->valid;
    if (entry->selected)
        ++t->selected;
}

static void stage_file(const struct le_wake_model_paths *paths, const char *name,
                       const char *file, const char *source_dir)
{
    char path[700];
    unsigned char bytes[16384];
    FILE *f;
    size_t n, offset = 0;
    unsigned long staged = 0;

    path_in(path, sizeof(path), source_dir, file);
    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    n = fread(bytes, 1, sizeof(bytes), f);
    fclose(f);
    /* Two chunks, so the offset rule is exercised. */
    while (offset < n) {
        size_t piece = n - offset > 700 ? 700 : n - offset;
        int rc = le_wake_model_stage_chunk(paths, name, file, (unsigned long)offset,
                                           bytes + offset, piece, &staged);
        CHECK(rc == LE_WAKE_MODEL_STORE_OK);
        offset += piece;
    }
    CHECK(staged == n);
}

static void check_store(void)
{
    char config_dir[600], config_file[700], source[600], reason[200], name[64];
    struct le_wake_model_paths paths;
    struct tally t;
    unsigned long staged = 0;
    unsigned char chunk[8];
    int rc;

    make_root();
    path_in(config_dir, sizeof(config_dir), root, "config");
    mkdir(config_dir, 0700);
    path_in(config_file, sizeof(config_file), config_dir, "web-config.json");
    CHECK(le_wake_model_paths_from_config(config_file, &paths) == 0);
    CHECK(strstr(paths.models, "/wakeword-models") != NULL);
    CHECK(strstr(paths.selection, "/config/wake-model") != NULL);

    /* Rejected names and files never touch the store. */
    CHECK(le_wake_model_stage_chunk(&paths, "../evil", "alexa_v0.1.onnx", 0, chunk, 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, "my-word", "other.bin", 0, chunk, 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, LE_WAKE_MODEL_STOCK_NAME, "alexa_v0.1.onnx", 0,
                                    chunk, 1, &staged) == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_stage_chunk(&paths, "my-word", "alexa_v0.1.onnx", 0, chunk,
                                    LE_WAKE_MODEL_STAGE_CHUNK_MAX + 1,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);

    /* Offsets must follow on from what is staged. */
    path_in(source, sizeof(source), root, "source");
    mkdir(source, 0700);
    write_set(source, 0);
    stage_file(&paths, "my-word", "melspectrogram.onnx", source);
    stage_file(&paths, "my-word", "embedding_model.onnx", source);
    stage_file(&paths, "my-word", "alexa_v0.1.onnx", source);
    CHECK(le_wake_model_stage_chunk(&paths, "my-word", "alexa_v0.1.onnx", 3, chunk, 2,
                                    &staged) == LE_WAKE_MODEL_STORE_INVALID);

    /* Committing a folder that does not pass is refused and cleaned up. */
    {
        unsigned char bytes[16384];
        struct model_spec bad = classifier_good;
        size_t length;
        bad.output = "wrong";
        length = build_model(bytes, sizeof(bytes), &bad);
        stage_file(&paths, "bad-word", "melspectrogram.onnx", source);
        stage_file(&paths, "bad-word", "embedding_model.onnx", source);
        CHECK(le_wake_model_stage_chunk(&paths, "bad-word", "alexa_v0.1.onnx", 0, bytes,
                                        length, &staged) == LE_WAKE_MODEL_STORE_OK);
        rc = le_wake_model_commit(&paths, "bad-word", reason, sizeof(reason));
        CHECK(rc == LE_WAKE_MODEL_STORE_INVALID);
        CHECK(le_wake_model_commit(&paths, "bad-word", reason, sizeof(reason)) ==
              LE_WAKE_MODEL_STORE_NOT_FOUND);
    }

    rc = le_wake_model_commit(&paths, "my-word", reason, sizeof(reason));
    CHECK(rc == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_commit(&paths, "my-word", reason, sizeof(reason)) ==
          LE_WAKE_MODEL_STORE_NOT_FOUND);

    memset(&t, 0, sizeof(t));
    CHECK(le_wake_model_scan(&paths, tally_visit, &t) == 1);
    CHECK(t.count == 1 && t.valid == 1 && t.selected == 0);

    /* Selection: stock by default, the folder once selected, stock again after. */
    CHECK(le_wake_model_selection_read(&paths, name, sizeof(name)) == 0);
    CHECK(le_wake_model_select(&paths, "missing") == LE_WAKE_MODEL_STORE_NOT_FOUND);
    CHECK(le_wake_model_select(&paths, "../my-word") == LE_WAKE_MODEL_STORE_INVALID);
    CHECK(le_wake_model_select(&paths, "my-word") == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_selection_read(&paths, name, sizeof(name)) == 1);
    CHECK(!strcmp(name, "my-word"));
    memset(&t, 0, sizeof(t));
    le_wake_model_scan(&paths, tally_visit, &t);
    CHECK(t.selected == 1);
    CHECK(le_wake_model_delete(&paths, "my-word") == LE_WAKE_MODEL_STORE_CONFLICT);
    CHECK(le_wake_model_select(&paths, LE_WAKE_MODEL_STOCK_NAME) == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_selection_read(&paths, name, sizeof(name)) == 0);

    CHECK(le_wake_model_delete(&paths, "my-word") == LE_WAKE_MODEL_STORE_OK);
    CHECK(le_wake_model_delete(&paths, "my-word") == LE_WAKE_MODEL_STORE_NOT_FOUND);
    memset(&t, 0, sizeof(t));
    CHECK(le_wake_model_scan(&paths, tally_visit, &t) == 0);
}

static void check_base64(void)
{
    unsigned char out[16];

    CHECK(le_wake_model_base64_decode("aGVsbG8=", 8, out, sizeof(out)) == 5);
    CHECK(!memcmp(out, "hello", 5));
    CHECK(le_wake_model_base64_decode("aGk=", 4, out, sizeof(out)) == 2);
    CHECK(!memcmp(out, "hi", 2));
    CHECK(le_wake_model_base64_decode("a!==", 4, out, sizeof(out)) < 0);
    CHECK(le_wake_model_base64_decode("abc", 3, out, sizeof(out)) < 0);
    CHECK(le_wake_model_base64_decode("aGVsbG8=", 8, out, 2) < 0);
}

int main(void)
{
    check_names();
    check_labels();
    check_contract();
    check_directory();
    check_store();
    check_base64();
    if (failures) {
        fprintf(stderr, "wake model: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("wake model: %d checks passed\n", checks);
    return 0;
}
