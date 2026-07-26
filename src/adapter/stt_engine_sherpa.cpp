#include "stt_engine.h"

#include "sherpa-onnx/c-api/c-api.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

struct stt_engine {
    const SherpaOnnxOnlineRecognizer *recognizer;
};

struct stt_stream {
    stt_engine *engine;
    const SherpaOnnxOnlineStream *stream;
    bool finished;
};

extern "C" struct stt_engine *stt_engine_init(
    const char *model_dir, unsigned int threads)
{
    SherpaOnnxOnlineRecognizerConfig config;
    char tokens[512];
    char encoder[512];
    char decoder[512];
    char joiner[512];
    stt_engine *engine;

    if (!model_dir || !model_dir[0])
        return nullptr;
    std::memset(&config, 0, sizeof(config));
    std::snprintf(tokens, sizeof(tokens), "%s/tokens.txt", model_dir);
    std::snprintf(
        encoder, sizeof(encoder),
        "%s/encoder-epoch-99-avg-1.int8.onnx", model_dir);
    std::snprintf(
        decoder, sizeof(decoder),
        "%s/decoder-epoch-99-avg-1.int8.onnx", model_dir);
    std::snprintf(
        joiner, sizeof(joiner),
        "%s/joiner-epoch-99-avg-1.int8.onnx", model_dir);
    config.model_config.tokens = tokens;
    config.model_config.transducer.encoder = encoder;
    config.model_config.transducer.decoder = decoder;
    config.model_config.transducer.joiner = joiner;
    config.model_config.num_threads =
        static_cast<int32_t>(threads ? threads : 1U);
    config.model_config.provider = "cpu";
    config.decoding_method = "greedy_search";
    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 1.5f;
    config.rule2_min_trailing_silence = 0.5f;
    config.rule3_min_utterance_length = 15.0f;
    engine = new (std::nothrow) stt_engine;
    if (!engine)
        return nullptr;
    engine->recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
    if (!engine->recognizer) {
        delete engine;
        return nullptr;
    }
    return engine;
}

extern "C" void stt_engine_destroy(struct stt_engine *engine)
{
    if (!engine)
        return;
    SherpaOnnxDestroyOnlineRecognizer(engine->recognizer);
    delete engine;
}

extern "C" struct stt_stream *stt_engine_stream_create(
    struct stt_engine *engine)
{
    stt_stream *stream;

    if (!engine)
        return nullptr;
    stream = new (std::nothrow) stt_stream;
    if (!stream)
        return nullptr;
    stream->engine = engine;
    stream->stream =
        SherpaOnnxCreateOnlineStream(engine->recognizer);
    stream->finished = false;
    if (!stream->stream) {
        delete stream;
        return nullptr;
    }
    return stream;
}

extern "C" void stt_engine_stream_destroy(struct stt_stream *stream)
{
    if (!stream)
        return;
    SherpaOnnxDestroyOnlineStream(stream->stream);
    delete stream;
}

static void copy_result(stt_stream *stream, char *text, size_t text_size)
{
    const SherpaOnnxOnlineRecognizerResult *result =
        SherpaOnnxGetOnlineStreamResult(
            stream->engine->recognizer, stream->stream);

    if (text && text_size) {
        std::snprintf(
            text, text_size, "%s",
            result && result->text ? result->text : "");
    }
    if (result)
        SherpaOnnxDestroyOnlineRecognizerResult(result);
}

extern "C" int stt_engine_stream_accept(
    struct stt_stream *stream, const int16_t *samples, size_t count,
    char *text, size_t text_size)
{
    std::vector<float> converted;
    size_t i;

    if (!stream || stream->finished || !samples)
        return -1;
    try {
        converted.resize(count);
    } catch (...) {
        return -1;
    }
    for (i = 0; i < count; ++i)
        converted[i] = static_cast<float>(samples[i]) / 32768.0f;
    SherpaOnnxOnlineStreamAcceptWaveform(
        stream->stream, 16000, converted.data(),
        static_cast<int32_t>(count));
    while (SherpaOnnxIsOnlineStreamReady(
               stream->engine->recognizer, stream->stream))
        SherpaOnnxDecodeOnlineStream(
            stream->engine->recognizer, stream->stream);
    copy_result(stream, text, text_size);
    /*
     * Rule 1 can report an endpoint after leading silence.  A wake-triggered
     * stream must keep listening until at least one token has been decoded,
     * otherwise a short pause between the wake phrase and command closes the
     * turn before the user speaks.
     */
    return text && text[0] &&
           SherpaOnnxOnlineStreamIsEndpoint(
               stream->engine->recognizer, stream->stream) ? 1 : 0;
}

extern "C" int stt_engine_stream_finish(
    struct stt_stream *stream, char *text, size_t text_size)
{
    std::vector<float> tail(4800, 0.0f);

    if (!stream)
        return -1;
    if (!stream->finished) {
        SherpaOnnxOnlineStreamAcceptWaveform(
            stream->stream, 16000, tail.data(),
            static_cast<int32_t>(tail.size()));
        SherpaOnnxOnlineStreamInputFinished(stream->stream);
        while (SherpaOnnxIsOnlineStreamReady(
                   stream->engine->recognizer, stream->stream))
            SherpaOnnxDecodeOnlineStream(
                stream->engine->recognizer, stream->stream);
        stream->finished = true;
    }
    copy_result(stream, text, text_size);
    return 0;
}

extern "C" const char *stt_engine_name(const struct stt_engine *engine)
{
    (void)engine;
    return "sherpa-onnx-zipformer";
}
