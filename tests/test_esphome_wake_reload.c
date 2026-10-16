/* Real waked command/PCM paths and wake_worker lifecycle. Only the ONNX
 * compute engine is scripted; sockets, queue, inference thread, decoder, AEC,
 * VAD and stream framing are production code. No device paths are opened. */
#define _POSIX_C_SOURCE 200809L
#define main waked_program_main
#include "../src/adapter/waked.c"
#undef main
#include "adapter/wake_engine.h"
#include <assert.h>
#include <pthread.h>

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "wake reload regression: %s\n", message);
        exit(1);
    }
}

#ifdef LE_WAKE_ENGINE_ONNX
#define BLOCK 1280U
#define ENGINE_LIMIT 16U
struct le_wake_engine { unsigned int generation, feeds; int live; };
static struct le_wake_engine engines[ENGINE_LIMIT];
static pthread_mutex_t engine_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t engine_condition = PTHREAD_COND_INITIALIZER;
static unsigned int creates, destroys, compute_active, max_compute;
static unsigned int total_feeds, fail_load, fail_feed;
static unsigned int slow_load, slow_destroy;
static unsigned int last_generation, last_threads;
static int last_first_sample;
static char last_directory[1024];
static pthread_t inference_thread;
static int have_inference_thread;

struct le_wake_engine *le_wake_engine_create(const char *directory,
                                             unsigned int threads)
{
    struct le_wake_engine *engine;
    if (slow_load) {
        struct timespec delay = {6, 0};
        slow_load = 0;
        (void)nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&engine_mutex);
    require(compute_active == 0, "model loading competed with inference");
    require(creates < ENGINE_LIMIT, "fixture engine capacity exhausted");
    ++creates;
    require(strlen(directory) < sizeof(last_directory), "directory overflow");
    snprintf(last_directory, sizeof(last_directory), "%s", directory);
    last_threads = threads;
    if (fail_load) {
        --fail_load;
        pthread_mutex_unlock(&engine_mutex);
        return NULL;
    }
    engine = &engines[creates - 1];
    engine->generation = creates;
    engine->live = 1;
    pthread_mutex_unlock(&engine_mutex);
    return engine;
}

int le_wake_engine_feed(struct le_wake_engine *engine,
                        const int16_t *samples, size_t count,
                        float *score, int *new_score)
{
    struct timespec delay = {0, 2000000};
    int failed;
    require(count == BLOCK, "worker changed inference block size");
    pthread_mutex_lock(&engine_mutex);
    require(engine->live, "inference used a destroyed model");
    if (have_inference_thread)
        require(pthread_equal(inference_thread, pthread_self()),
                "reload replaced or added the inference thread");
    else {
        inference_thread = pthread_self();
        have_inference_thread = 1;
    }
    ++compute_active;
    if (compute_active > max_compute) max_compute = compute_active;
    failed = fail_feed != 0;
    if (failed) --fail_feed;
    pthread_mutex_unlock(&engine_mutex);
    (void)nanosleep(&delay, NULL);
    pthread_mutex_lock(&engine_mutex);
    ++engine->feeds;
    ++total_feeds;
    last_generation = engine->generation;
    last_first_sample = samples[0];
    --compute_active;
    pthread_cond_broadcast(&engine_condition);
    pthread_mutex_unlock(&engine_mutex);
    *score = (float)samples[0] / 1000.0f;
    *new_score = 1;
    return failed ? -1 : 0;
}

unsigned int le_wake_engine_last_inference_us(const struct le_wake_engine *engine)
{ (void)engine; return 2000; }

void le_wake_engine_destroy(struct le_wake_engine *engine)
{
    if (slow_destroy) {
        struct timespec delay = {5, 500000000};
        slow_destroy = 0;
        (void)nanosleep(&delay, NULL);
    }
    pthread_mutex_lock(&engine_mutex);
    require(compute_active == 0, "model destroyed during inference");
    require(engine->live, "model destroyed twice");
    engine->live = 0;
    ++destroys;
    pthread_mutex_unlock(&engine_mutex);
}

static void wait_feeds(unsigned int target)
{
    struct timespec deadline;
    require(clock_gettime(CLOCK_REALTIME, &deadline) == 0, "clock failed");
    deadline.tv_sec += 2;
    pthread_mutex_lock(&engine_mutex);
    while (total_feeds < target)
        require(pthread_cond_timedwait(&engine_condition, &engine_mutex,
                                      &deadline) == 0, "inference timed out");
    pthread_mutex_unlock(&engine_mutex);
}

static void feed(struct le_wake_worker *worker, int score,
                 size_t count, uint64_t sample)
{
    int16_t block[BLOCK];
    struct le_wake_observation observation = {sample, 1.0f, 1, 0};
    size_t i;
    require(count <= BLOCK, "fixture block overflow");
    for (i = 0; i < count; ++i) block[i] = (int16_t)score;
    require(le_wake_worker_submit(worker, block, count, &observation) == 0,
            "PCM submit failed or dropped");
}
#endif

static void initialize(struct waked_ipc *ipc)
{
    size_t i;
    memset(ipc, 0, sizeof(*ipc));
    ipc->listen_fd = -1;
    ipc->event_pipe[0] = ipc->event_pipe[1] = -1;
    ipc->sensitivity = 0;
    for (i = 0; i < MAX_WAKE_SUBSCRIBERS; ++i) ipc->subscribers[i] = -1;
    for (i = 0; i < MAX_AUDIO_SUBSCRIBERS; ++i) ipc->audio_subscribers[i] = -1;
    require(pipe(ipc->event_pipe) == 0, "event pipe failed");
    require(set_nonblocking(ipc->event_pipe[0]) == 0 &&
            set_nonblocking(ipc->event_pipe[1]) == 0, "nonblocking failed");
}

static void command(struct waked_ipc *ipc, struct le_wake_worker *worker,
                    const char *cmd, const char *args, int expected,
                    const char *diagnostic)
{
    struct waked_metrics metrics = {0};
    int pair[2], length;
    ssize_t received;
    char request[LE_ADAPTER_MSG_MAX], response[LE_ADAPTER_MSG_MAX];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "socketpair failed");
    length = snprintf(request, sizeof(request),
        "{\"v\":1,\"id\":71,\"cmd\":\"%s\",\"args\":%s}\n", cmd, args);
    require(length > 0 && (size_t)length < sizeof(request), "request overflow");
    require(write_all(pair[0], request, (size_t)length) == 0, "request failed");
    handle_control_client(ipc, worker, &metrics, pair[1]);
    received = read(pair[0], response, sizeof(response) - 1);
    require(received > 0, "missing response");
    response[received] = '\0';
    require(strstr(response, "\"id\":71") != NULL, "response ID changed");
    require(strstr(response, expected ? "\"ok\":true" : "\"ok\":false") != NULL,
            diagnostic);
    if (expected) require(strstr(response, "\"data\":{}") != NULL,
                          "success envelope changed");
    close(pair[0]);
}

#ifdef LE_WAKE_ENGINE_ONNX
static unsigned int events(struct waked_ipc *ipc)
{
    struct le_wake_event event;
    ssize_t count;
    unsigned int found = 0;
    while ((count = read(ipc->event_pipe[0], &event, sizeof(event))) > 0) {
        require(count == (ssize_t)sizeof(event), "partial wake event");
        require(!strcmp(event.model_name, "alexa_v0.1"), "wrong model event");
        ++found;
    }
    require(count < 0 && errno == EAGAIN, "event pipe read failed");
    return found;
}

/* The zero-score fence is entered only after the previous block's real
 * decoder/callback completed. Waiting on the engine's condition avoids
 * unbounded sleeps and does not substitute for the production worker. */
static void sequence(struct le_wake_worker *worker, struct waked_ipc *ipc,
                     int score, uint64_t first_sample, unsigned int expected)
{
    unsigned int target = total_feeds + 3;
    feed(worker, score, BLOCK, first_sample);
    feed(worker, score, BLOCK, first_sample + BLOCK);
    feed(worker, 0, BLOCK, first_sample + 2 * BLOCK);
    wait_feeds(target);
    require(events(ipc) == expected, "decoder/threshold/lockout was not reinitialized");
}

static void start(struct le_wake_worker *worker, struct waked_ipc *ipc,
                  const char *directory)
{
    char owned_directory[1024];
    require(strlen(directory) < sizeof(owned_directory), "fixture path too long");
    snprintf(owned_directory, sizeof(owned_directory), "%s", directory);
    require(le_wake_worker_start(worker, owned_directory, 3, 0.77f,
                                 wake_event_received, ipc) == 0, "start failed");
    /* Reload must use owned startup settings, not a borrowed stack pointer. */
    memset(owned_directory, 'X', strlen(owned_directory));
}

static void validation(struct le_wake_worker *worker, struct waked_ipc *ipc)
{
    static const char *invalid[] = {
        "{}", "{\"note\":\"Alexa\"}",
        "{\"word\":\"LibreEcho\"}", "{\"word\":\"Computer\"}",
        "{\"word\":\"Echo\"}", "{\"word\":\"Custom model\"}",
        "{\"word\":\"other\",\"note\":\"Alexa\"}",
        "{\"nested\":{\"word\":\"Alexa\"}}",
        "{\"word\":[\"Alexa\"]}", "{\"word\":{\"name\":\"Alexa\"}}",
        "{\"word\":null,\"note\":\"Alexa\"}",
        "{\"word\":\"Alexa\",\"word\":\"other\"}",
        "{\"word\":\"Alexa\",\"w\\u006frd\":\"other\"}",
        "{\"word\":\"Alexa suffix\"}", "{\"word\":\"prefix Alexa\"}",
        "{\"word\" \"Alexa\"}", "{\"word\":\"Alexa\",}",
        "{\"word\":true,\"note\":\"Alexa\"}",
        "{\"word\":17,\"note\":\"alexa\"}",
        "{\"word\":\"Alexa\"}junk"
    };
    size_t i;
    char oversized[256];
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        unsigned int before = creates;
        command(ipc, worker, "set_word", invalid[i], 0,
                "invalid set_word accepted an unrelated/ambiguous Alexa substring");
        require(creates == before, "invalid request reloaded the model");
    }
    memset(oversized, 'a', sizeof(oversized));
    memcpy(oversized, "{\"word\":\"", 9);
    memcpy(oversized + sizeof(oversized) - 4, "\"}", 3);
    command(ipc, worker, "set_word", oversized, 0, "oversized word accepted");
}

static void successful_reload(struct le_wake_worker *worker,
                              struct waked_ipc *ipc, const char *directory)
{
    int age;
    unsigned int before;
    /* Live sensitivity overrides the startup threshold and survives reload. */
    command(ipc, worker, "set_sensitivity", "{\"sensitivity\":0}", 1,
            "sensitivity failed");
    sequence(worker, ipc, 800, BLOCK, 0);
    sequence(worker, ipc, 900, 10000, 1);
    feed(worker, 900, 160, 14000); /* incomplete old PCM must not cross reload */
    before = creates;
    command(ipc, worker, "set_word", " { \"word\" : \"Alexa\" } ", 1,
            "valid set_word failed");
    require(creates == before + 1 && destroys == 1,
            "set_word acknowledged without replacing the actual engine");
    require(!strcmp(last_directory, directory) && last_threads == 3,
            "reload lost stored directory/thread configuration");
    require(le_wake_worker_health(worker, &age) && age == -1,
            "reload did not reset inference health age");
    before = total_feeds;
    feed(worker, 800, BLOCK - 160, 15000);
    require(total_feeds == before, "old partial PCM survived reload");
    feed(worker, 800, 160, 16000);
    wait_feeds(before + 1);
    require(last_generation == creates && last_first_sample == 800,
            "new engine did not receive fresh PCM");
    sequence(worker, ipc, 800, 17000, 0);
    /* Inside the old generation's lockout: fresh decoder must detect. */
    sequence(worker, ipc, 900, 20000, 1);
    command(ipc, worker, "set_word", "{\"word\":\"alexa\"}", 1,
            "lowercase supported word failed");
    require(creates == 3 && destroys == 2, "repeat selection was a no-op");
    sequence(worker, ipc, 900, 24000, 1);
}

static void failed_reload(struct le_wake_worker *worker, struct waked_ipc *ipc)
{
    int age;
    sequence(worker, ipc, 900, BLOCK, 1);
    feed(worker, 900, 160, 40000);
    fail_load = 1;
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 0,
            "failed model load reported success");
    require(creates == 2 && destroys == 0 && engines[0].live,
            "failed reload destroyed the valid current model");
    require(le_wake_worker_health(worker, &age) && age >= 0,
            "failed reload lost healthy worker/health history");
    feed(worker, 900, BLOCK - 160, 42000);
    wait_feeds(4);
    sequence(worker, ipc, 900, 45000, 1);
    require(last_generation == 1, "failed reload did not retain old inference");
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 1,
            "failed reload prevented retry");
    require(creates == 3 && destroys == 1, "retry did not replace the model");
    sequence(worker, ipc, 900, 46000, 1);
}

static void inference_recovery(struct le_wake_worker *worker,
                               struct waked_ipc *ipc)
{
    int age;
    struct timespec delay = {0, 1000000};
    unsigned int tries;
    fail_feed = 1;
    feed(worker, 900, BLOCK, BLOCK);
    wait_feeds(1);
    for (tries = 0; tries < 100 && le_wake_worker_health(worker, &age); ++tries)
        (void)nanosleep(&delay, NULL);
    require(!le_wake_worker_health(worker, &age), "inference failure not reported");
    fail_load = 1;
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 0,
            "failed recovery reported success");
    require(!le_wake_worker_health(worker, &age) && destroys == 0,
            "failed recovery falsified health or destroyed retained engine");
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 1,
            "failed worker could not reload");
    require(le_wake_worker_health(worker, &age) && age == -1,
            "successful recovery did not restore worker health");
    sequence(worker, ipc, 900, 10000, 1);
}

static void queued_reload(struct le_wake_worker *worker, struct waked_ipc *ipc)
{
    unsigned int i;
    for (i = 0; i < 8; ++i) feed(worker, 0, BLOCK, (i + 1) * BLOCK);
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 1,
            "queued reload failed");
    require(creates == 2 && destroys == 1, "queued reload was a no-op");
    require(compute_active == 0 && max_compute <= 1,
            "reload ran competing inference workers");
    sequence(worker, ipc, 900, 20000, 1);
}

static void capture_continues(struct le_wake_worker *worker, struct waked_ipc *ipc)
{
    struct le_voice_aec aec = {0};
    struct le_voice_reference reference = {0};
    struct le_voice_vad vad;
    struct waked_metrics metrics = {0};
    int16_t microphone[LE_VOICE_AEC_FRAME_SAMPLES] = {0};
    int16_t preroll[PREROLL_SAMPLES] = {0};
    size_t position = 0;
    int pair[2];
    unsigned int i;
    struct le_voice_stream_frame frame;
    reference.fd = -1;
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "PCM socket failed");
    ipc->audio_subscribers[0] = pair[1];
    le_voice_vad_init(&vad);
    for (i = 0; i < 16; ++i) {
        if (i == 8)
            command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 1,
                    "capture reload failed");
        require(process_frame(&aec, &reference, &vad, worker, ipc, microphone,
                              preroll, &position, -1, 0, &metrics) == 0,
                "capture processing stopped across reload");
        require(le_voice_stream_read_frame(pair[0], &frame) == 1,
                "post-AEC stream stopped across reload");
        require(frame.first_sample == (uint64_t)i * LE_VOICE_AEC_FRAME_SAMPLES,
                "capture sample index reset across model reload");
    }
    require(metrics.processed_frames == 16 && metrics.last_capture_ns != 0,
            "reload altered capture health counters");
    close(pair[0]); close(pair[1]); ipc->audio_subscribers[0] = -1;
}

static void timed_out_reload(struct le_wake_worker *worker, struct waked_ipc *ipc)
{
    uint64_t started;
    int age;
    sequence(worker, ipc, 900, BLOCK, 1);
    slow_load = 1;
    started = monotonic_nanoseconds();
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 0,
            "load timeout reported success");
    require(monotonic_nanoseconds() - started < 5500000000ULL,
            "reload response exceeded its bounded deadline");
    require(le_wake_worker_health(worker, &age), "timeout lost the current model");
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 0,
            "another reload started before cancelled initialization retired");
    /* A feed on the retained engine fences completion of cancelled creation. */
    sequence(worker, ipc, 900, 40000, 1);
    require(last_generation == 1 && engines[0].live && destroys == 1,
            "timed-out initialization published late or discarded the old model");
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 1,
            "timeout prevented a later retry");
    sequence(worker, ipc, 900, 44000, 1);
}

static void slow_retirement(struct le_wake_worker *worker, struct waked_ipc *ipc)
{
    uint64_t started = monotonic_nanoseconds();
    slow_destroy = 1;
    command(ipc, worker, "set_word", "{\"word\":\"Alexa\"}", 1,
            "published replacement was reported failed during old-engine retirement");
    require(monotonic_nanoseconds() - started < 5500000000ULL,
            "retirement extended the reload response deadline");
    require(engines[1].live, "successful response had no loaded replacement");
    /* stop joins the same worker and therefore also fences retired cleanup */
}
#endif

int main(int argc, char **argv)
{
    struct le_wake_worker worker = {0};
    struct waked_ipc ipc;
    require(argc == 3, "usage: fixture CASE MODEL_DIRECTORY");
    alarm(8);
    initialize(&ipc);
#ifdef LE_WAKE_ENGINE_ONNX
    start(&worker, &ipc, argv[2]);
    if (!strcmp(argv[1], "validation")) validation(&worker, &ipc);
    else if (!strcmp(argv[1], "reload")) successful_reload(&worker, &ipc, argv[2]);
    else if (!strcmp(argv[1], "failure")) failed_reload(&worker, &ipc);
    else if (!strcmp(argv[1], "recovery")) inference_recovery(&worker, &ipc);
    else if (!strcmp(argv[1], "queued")) queued_reload(&worker, &ipc);
    else if (!strcmp(argv[1], "capture")) capture_continues(&worker, &ipc);
    else if (!strcmp(argv[1], "timeout")) timed_out_reload(&worker, &ipc);
    else if (!strcmp(argv[1], "retirement")) slow_retirement(&worker, &ipc);
    else require(0, "unknown fixture case");
    le_wake_worker_stop(&worker, NULL);
    require(!worker.implementation && compute_active == 0, "worker cleanup failed");
    for (unsigned int i = 0; i < ENGINE_LIMIT; ++i)
        require(!engines[i].live, "worker leaked a current/retired engine");
#else
    command(&ipc, &worker, "set_word", "{\"word\":\"Alexa\"}", 0,
            "ONNX-disabled daemon claimed successful reload");
#endif
    close(ipc.event_pipe[0]); close(ipc.event_pipe[1]);
    printf("wake reload %s: ok\n", argv[1]);
    return 0;
}
