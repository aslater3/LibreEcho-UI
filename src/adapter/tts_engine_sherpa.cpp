/*
 * sherpa-onnx TTS engine — real neural synthesis.
 *
 * Implements the tts_engine C ABI (tts_engine.h) using the sherpa-onnx
 * C API.  Compiled as C++ but exposes only C-linkage symbols so ttsd.c
 * (pure C99) can link against it without a C++ dependency leak.
 *
 * Auto-detects model type from the files present in model_dir:
 *   - ZipVoice: encoder*.onnx + decoder*.onnx + vocoder*.onnx
 *   - VITS:     single *.onnx model file
 *
 * The engine produces float32 mono PCM at the model's native rate.
 * ttsd resamples to the bus rate (48 kHz stereo).
 */
#include "tts_engine.h"

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "espeak-ng/speak_lib.h"
#include "flite.h"
#include "sherpa-onnx/c-api/c-api.h"

extern "C" cst_voice *register_cmu_us_slt(const char *voxdir);
extern "C" void unregister_cmu_us_slt(cst_voice *vox);
extern "C" cst_voice *register_cmu_us_awb(const char *voxdir);
extern "C" void unregister_cmu_us_awb(cst_voice *vox);
extern "C" cst_voice *register_cmu_us_rms(const char *voxdir);
extern "C" void unregister_cmu_us_rms(cst_voice *vox);
extern "C" cst_voice *register_cmu_us_kal(const char *voxdir);
extern "C" void unregister_cmu_us_kal(cst_voice *vox);

enum tts_model_type {
    TTS_MODEL_UNKNOWN = 0,
    TTS_MODEL_ZIPVOICE,
    TTS_MODEL_MATCHA,
    TTS_MODEL_KOKORO,
    TTS_MODEL_KITTEN,
    TTS_MODEL_POCKET,
    TTS_MODEL_SUPERTONIC,
    TTS_MODEL_VITS,
    TTS_MODEL_FLITE,
    TTS_MODEL_ESPEAK,
};

struct tts_engine {
    const SherpaOnnxOfflineTts *tts;
    int sample_rate;
    enum tts_model_type type;
    cst_voice *flite_voice;
    void (*flite_unregister)(cst_voice *);
    std::vector<float> pocket_reference;
    int pocket_reference_rate;
};

static std::vector<short> g_espeak_samples;

static int espeak_callback(short *wav, int numsamples, espeak_EVENT *)
{
    if (wav && numsamples > 0)
        g_espeak_samples.insert(g_espeak_samples.end(), wav,
                                wav + numsamples);
    return 0;
}

static std::string model_path(const char *dir, const char *file)
{
    std::string p(dir ? dir : ".");
    p += '/';
    p += file;
    return p;
}

static bool file_exists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static bool load_pcm16_wav(const std::string &path, std::vector<float> *out,
                           int *sample_rate)
{
    FILE *f = fopen(path.c_str(), "rb");
    unsigned char header[44];
    uint32_t data_size;
    uint16_t channels, bits;
    uint32_t rate;
    if (!f)
        return false;
    if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0 ||
        memcmp(header + 12, "fmt ", 4) != 0 ||
        read_le16(header + 20) != 1) {
        fclose(f);
        return false;
    }
    channels = read_le16(header + 22);
    rate = read_le32(header + 24);
    bits = read_le16(header + 34);
    if (channels != 1 || bits != 16 || rate == 0 ||
        memcmp(header + 36, "data", 4) != 0) {
        fclose(f);
        return false;
    }
    data_size = read_le32(header + 40);
    if (data_size == 0 || (data_size & 1)) {
        fclose(f);
        return false;
    }
    out->resize(data_size / 2);
    for (size_t i = 0; i < out->size(); ++i) {
        unsigned char sample[2];
        if (fread(sample, 1, sizeof(sample), f) != sizeof(sample)) {
            out->clear();
            fclose(f);
            return false;
        }
        (*out)[i] = (float)(int16_t)read_le16(sample) / 32768.0f;
    }
    fclose(f);
    *sample_rate = (int)rate;
    return true;
}

struct tts_engine *tts_engine_init(const char *model_dir, const char *voice)
{
    (void)voice;

    if (!model_dir || model_dir[0] == '\0')
        return nullptr;

    SherpaOnnxOfflineTtsConfig config;
    memset(&config, 0, sizeof(config));
    int thread_count = 1;  /* Echo has limited RAM */
    const char *thread_env = getenv("LE_TTS_THREADS");
    if (thread_env) {
        int requested = atoi(thread_env);
        if (requested >= 1 && requested <= 4)
            thread_count = requested;
    }
    config.model.num_threads = thread_count;
    std::string provider = "cpu";
    const char *ort_config = getenv("LE_TTS_ORT_CONFIG");
    if (ort_config && ort_config[0] != '\0') {
        provider += ':';
        provider += ort_config;
    }
    config.model.provider    = provider.c_str();
    config.model.debug       = 0;
    config.max_num_sentences = 1;

    enum tts_model_type type = TTS_MODEL_UNKNOWN;

    /* Strings must outlive SherpaOnnxCreateOfflineTts() — declare at
     * function scope so the .c_str() pointers stay valid. */
    std::string encoder, decoder, vocoder, acoustic, tokens, lexicon, data_dir;
    std::string voices, kitten_model, kokoro_model;
    std::string kokoro_tokens;
    std::string pocket_lm_flow, pocket_lm_main, pocket_encoder;
    std::string pocket_decoder, pocket_conditioner, pocket_vocab;
    std::string pocket_scores, pocket_reference;
    std::string supertonic_duration, supertonic_encoder;
    std::string supertonic_estimator, supertonic_vocoder;
    std::string supertonic_json, supertonic_indexer, supertonic_voice;
    std::string vits_model;

    const char *fast_engine = getenv("LE_TTS_ENGINE");
    if (fast_engine && !strcmp(fast_engine, "espeak")) {
        data_dir = model_path(model_dir, "espeak-ng-data");
        int rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, 0,
                                     data_dir.c_str(),
                                     espeakINITIALIZE_DONT_EXIT);
        if (rate < 0)
            return nullptr;
        (void)espeak_SetVoiceByName("en-gb");
        espeak_SetParameter(espeakRATE, 170, 0);
        espeak_SetSynthCallback(espeak_callback);
        struct tts_engine *engine =
            static_cast<struct tts_engine *>(calloc(1, sizeof(*engine)));
        if (!engine) {
            espeak_Terminate();
            return nullptr;
        }
        engine->sample_rate = rate;
        engine->type = TTS_MODEL_ESPEAK;
        return engine;
    }

    if (fast_engine && !strcmp(fast_engine, "flite")) {
        if (flite_init() < 0)
            return nullptr;
        const char *voice_name = getenv("LE_FLITE_VOICE");
        cst_voice *flite_voice = nullptr;
        void (*flite_unregister)(cst_voice *) = unregister_cmu_us_slt;
        if (voice_name && !strcmp(voice_name, "awb")) {
            flite_voice = register_cmu_us_awb(nullptr);
            flite_unregister = unregister_cmu_us_awb;
        } else if (voice_name && !strcmp(voice_name, "rms")) {
            flite_voice = register_cmu_us_rms(nullptr);
            flite_unregister = unregister_cmu_us_rms;
        } else if (voice_name && !strcmp(voice_name, "kal")) {
            flite_voice = register_cmu_us_kal(nullptr);
            flite_unregister = unregister_cmu_us_kal;
        } else {
            flite_voice = register_cmu_us_slt(nullptr);
        }
        if (!flite_voice)
            return nullptr;
        struct tts_engine *engine =
            static_cast<struct tts_engine *>(calloc(1, sizeof(*engine)));
        if (!engine) {
            unregister_cmu_us_slt(flite_voice);
            return nullptr;
        }
        engine->sample_rate = 16000;
        engine->type = TTS_MODEL_FLITE;
        engine->flite_voice = flite_voice;
        engine->flite_unregister = flite_unregister;
        return engine;
    }

    /* Try ZipVoice first (encoder + decoder + vocoder). */
    encoder = model_path(model_dir, "encoder.int8.onnx");
    if (!file_exists(encoder))
        encoder = model_path(model_dir, "encoder.onnx");
    decoder = model_path(model_dir, "decoder.int8.onnx");
    if (!file_exists(decoder))
        decoder = model_path(model_dir, "decoder.onnx");
    vocoder = model_path(model_dir, "vocos_24khz.onnx");

    if (file_exists(encoder) && file_exists(decoder) && file_exists(vocoder)) {
        tokens   = model_path(model_dir, "tokens.txt");
        lexicon  = model_path(model_dir, "lexicon.txt");
        data_dir = model_path(model_dir, "espeak-ng-data");

        config.model.zipvoice.encoder  = encoder.c_str();
        config.model.zipvoice.decoder  = decoder.c_str();
        config.model.zipvoice.vocoder  = vocoder.c_str();
        config.model.zipvoice.tokens   = tokens.c_str();
        config.model.zipvoice.lexicon  = lexicon.c_str();
        config.model.zipvoice.data_dir = data_dir.c_str();
        config.model.zipvoice.feat_scale     = 0.5f;
        config.model.zipvoice.t_shift        = 0.0f;
        config.model.zipvoice.target_rms     = 0.1f;
        config.model.zipvoice.guidance_scale = 1.0f;
        type = TTS_MODEL_ZIPVOICE;
    }

    if (type == TTS_MODEL_UNKNOWN) {
        /* Matcha uses a compact acoustic model plus a shared vocoder. */
        acoustic = model_path(model_dir, "model-steps-3.onnx");
        vocoder = model_path(model_dir, "vocos-22khz-univ.onnx");
        if (file_exists(acoustic) && file_exists(vocoder)) {
            tokens   = model_path(model_dir, "tokens.txt");
            data_dir = model_path(model_dir, "espeak-ng-data");

            config.model.matcha.acoustic_model = acoustic.c_str();
            config.model.matcha.vocoder        = vocoder.c_str();
            config.model.matcha.lexicon        = "";
            config.model.matcha.tokens         = tokens.c_str();
            config.model.matcha.data_dir       = data_dir.c_str();
            config.model.matcha.noise_scale    = 0.667f;
            config.model.matcha.length_scale   = 1.0f;
            type = TTS_MODEL_MATCHA;
        }
    }

    if (type == TTS_MODEL_UNKNOWN) {
        /* Kitten Nano is a compact int8 English model with voice embeddings. */
        kitten_model = model_path(model_dir, "model.int8.onnx");
        voices = model_path(model_dir, "voices.bin");
        if (file_exists(kitten_model) && file_exists(voices)) {
            tokens   = model_path(model_dir, "tokens.txt");
            data_dir = model_path(model_dir, "espeak-ng-data");

            config.model.kitten.model    = kitten_model.c_str();
            config.model.kitten.voices   = voices.c_str();
            config.model.kitten.tokens   = tokens.c_str();
            config.model.kitten.data_dir = data_dir.c_str();
            config.model.kitten.length_scale = 1.0f;
            type = TTS_MODEL_KITTEN;
        }
    }

    if (type == TTS_MODEL_UNKNOWN) {
        /* Kokoro English is a compact multi-speaker neural voice. */
        /* Keep the quantized filename distinct from Kitten's model.int8.onnx
         * so auto-detection cannot mistake one model family for the other. */
        kokoro_model = model_path(model_dir, "kokoro.int8.onnx");
        if (!file_exists(kokoro_model))
            kokoro_model = model_path(model_dir, "model.onnx");
        voices = model_path(model_dir, "voices.bin");
        kokoro_tokens = model_path(model_dir, "tokens.txt");
        data_dir = model_path(model_dir, "espeak-ng-data");
        if (file_exists(kokoro_model) && file_exists(voices) &&
            file_exists(kokoro_tokens) && file_exists(data_dir)) {
            config.model.kokoro.model = kokoro_model.c_str();
            config.model.kokoro.voices = voices.c_str();
            config.model.kokoro.tokens = kokoro_tokens.c_str();
            config.model.kokoro.data_dir = data_dir.c_str();
            config.model.kokoro.length_scale = 1.0f;
            config.model.kokoro.lang = "en-us";
            type = TTS_MODEL_KOKORO;
        }
    }

    if (type == TTS_MODEL_UNKNOWN) {
        /* Pocket TTS is zero-shot and needs a small reference recording. */
        pocket_lm_flow = model_path(model_dir, "lm_flow.int8.onnx");
        pocket_lm_main = model_path(model_dir, "lm_main.int8.onnx");
        pocket_encoder = model_path(model_dir, "encoder.onnx");
        pocket_decoder = model_path(model_dir, "decoder.int8.onnx");
        pocket_conditioner = model_path(model_dir, "text_conditioner.onnx");
        pocket_vocab = model_path(model_dir, "vocab.json");
        pocket_scores = model_path(model_dir, "token_scores.json");
        pocket_reference = model_path(model_dir, "test_wavs/bria.wav");
        if (file_exists(pocket_lm_flow) && file_exists(pocket_lm_main) &&
            file_exists(pocket_encoder) && file_exists(pocket_decoder) &&
            file_exists(pocket_conditioner) && file_exists(pocket_vocab) &&
            file_exists(pocket_scores) && file_exists(pocket_reference)) {
            config.model.pocket.lm_flow = pocket_lm_flow.c_str();
            config.model.pocket.lm_main = pocket_lm_main.c_str();
            config.model.pocket.encoder = pocket_encoder.c_str();
            config.model.pocket.decoder = pocket_decoder.c_str();
            config.model.pocket.text_conditioner = pocket_conditioner.c_str();
            config.model.pocket.vocab_json = pocket_vocab.c_str();
            config.model.pocket.token_scores_json = pocket_scores.c_str();
            config.model.pocket.voice_embedding_cache_capacity = 1;
            type = TTS_MODEL_POCKET;
        }
    }

    if (type == TTS_MODEL_UNKNOWN) {
        /* Supertonic 3 is a compact int8 flow-matching model. */
        supertonic_duration = model_path(model_dir, "duration_predictor.int8.onnx");
        supertonic_encoder = model_path(model_dir, "text_encoder.int8.onnx");
        supertonic_estimator = model_path(model_dir, "vector_estimator.int8.onnx");
        supertonic_vocoder = model_path(model_dir, "vocoder.int8.onnx");
        supertonic_json = model_path(model_dir, "tts.json");
        supertonic_indexer = model_path(model_dir, "unicode_indexer.bin");
        supertonic_voice = model_path(model_dir, "voice.bin");
        if (file_exists(supertonic_duration) && file_exists(supertonic_encoder) &&
            file_exists(supertonic_estimator) && file_exists(supertonic_vocoder) &&
            file_exists(supertonic_json) && file_exists(supertonic_indexer) &&
            file_exists(supertonic_voice)) {
            config.model.supertonic.duration_predictor = supertonic_duration.c_str();
            config.model.supertonic.text_encoder = supertonic_encoder.c_str();
            config.model.supertonic.vector_estimator = supertonic_estimator.c_str();
            config.model.supertonic.vocoder = supertonic_vocoder.c_str();
            config.model.supertonic.tts_json = supertonic_json.c_str();
            config.model.supertonic.unicode_indexer = supertonic_indexer.c_str();
            config.model.supertonic.voice_style = supertonic_voice.c_str();
            type = TTS_MODEL_SUPERTONIC;
        }
    }

    if (type == TTS_MODEL_UNKNOWN) {
        /* Fall back to VITS: look for any .onnx file in model_dir. */
        tokens   = model_path(model_dir, "tokens.txt");
        data_dir = model_path(model_dir, "espeak-ng-data");

        const char *vits_names[] = {
            "model.onnx", "model.int8.onnx",
            "en_GB-sweetbbak-amy.onnx",
            NULL
        };
        for (int i = 0; vits_names[i]; ++i) {
            std::string candidate = model_path(model_dir, vits_names[i]);
            if (file_exists(candidate)) {
                vits_model = candidate;
                break;
            }
        }

        if (vits_model.empty())
            return nullptr;

        config.model.vits.model    = vits_model.c_str();
        config.model.vits.tokens   = tokens.c_str();
        config.model.vits.data_dir = data_dir.c_str();
        config.model.vits.length_scale = 1.0f;
        config.model.vits.noise_scale  = 0.667f;
        config.model.vits.noise_scale_w = 0.8f;
        type = TTS_MODEL_VITS;
    }

    const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);
    if (!tts)
        return nullptr;

    struct tts_engine *engine =
        static_cast<struct tts_engine *>(calloc(1, sizeof(*engine)));
    if (!engine) {
        SherpaOnnxDestroyOfflineTts(tts);
        return nullptr;
    }

    engine->tts = tts;
    engine->sample_rate = SherpaOnnxOfflineTtsSampleRate(tts);
    engine->type = type;
    engine->pocket_reference_rate = 0;
    if (type == TTS_MODEL_POCKET &&
        !load_pcm16_wav(pocket_reference, &engine->pocket_reference,
                        &engine->pocket_reference_rate)) {
        SherpaOnnxDestroyOfflineTts(tts);
        free(engine);
        return nullptr;
    }
    return engine;
}

int tts_engine_sample_rate(const struct tts_engine *engine)
{
    return engine ? engine->sample_rate : 22050;
}

int tts_engine_synthesize(struct tts_engine *engine, const char *text,
                          short **out, size_t *n_out)
{
    if (!out || !n_out)
        return -1;
    *out = nullptr;
    *n_out = 0;

    if (!engine || !text || text[0] == '\0')
        return -1;

    if (engine->type == TTS_MODEL_ESPEAK) {
        g_espeak_samples.clear();
        if (espeak_Synth(text, 0, 0, POS_CHARACTER, 0, espeakCHARS_UTF8,
                         NULL, NULL) != EE_OK ||
            espeak_Synchronize() != EE_OK || g_espeak_samples.empty())
            return -1;
        short *samples = static_cast<short *>(malloc(
            g_espeak_samples.size() * sizeof(short)));
        if (!samples)
            return -1;
        memcpy(samples, g_espeak_samples.data(),
               g_espeak_samples.size() * sizeof(short));
        *out = samples;
        *n_out = g_espeak_samples.size();
        return 0;
    }

    if (engine->type == TTS_MODEL_FLITE) {
        cst_wave *wave = flite_text_to_wave(text, engine->flite_voice);
        if (!wave || !wave->samples || wave->num_samples <= 0) {
            if (wave)
                delete_wave(wave);
            return -1;
        }
        short *samples = static_cast<short *>(malloc(
            (size_t)wave->num_samples * sizeof(short)));
        if (!samples) {
            delete_wave(wave);
            return -1;
        }
        memcpy(samples, wave->samples,
               (size_t)wave->num_samples * sizeof(short));
        *out = samples;
        *n_out = (size_t)wave->num_samples;
        delete_wave(wave);
        return 0;
    }

    if (!engine->tts)
        return -1;

    const SherpaOnnxGeneratedAudio *audio;

    if (engine->type == TTS_MODEL_ZIPVOICE) {
        SherpaOnnxGenerationConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.speed = 1.0f;
        cfg.num_steps = 4;
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text,
                                                       &cfg, NULL, NULL);
    } else if (engine->type == TTS_MODEL_MATCHA) {
        SherpaOnnxGenerationConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.speed = 1.0f;
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text,
                                                       &cfg, NULL, NULL);
    } else if (engine->type == TTS_MODEL_KOKORO) {
        SherpaOnnxGenerationConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.speed = 1.0f;
        const char *sid_env = getenv("LE_KOKORO_SID");
        cfg.sid = sid_env ? atoi(sid_env) : 0;
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text,
                                                       &cfg, NULL, NULL);
    } else if (engine->type == TTS_MODEL_KITTEN) {
        SherpaOnnxGenerationConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.speed = 1.0f;
        cfg.sid = 0;
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text,
                                                       &cfg, NULL, NULL);
    } else if (engine->type == TTS_MODEL_POCKET) {
        SherpaOnnxGenerationConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.speed = 1.0f;
        cfg.reference_audio = engine->pocket_reference.data();
        cfg.reference_audio_len = (int32_t)engine->pocket_reference.size();
        cfg.reference_sample_rate = engine->pocket_reference_rate;
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text,
                                                       &cfg, NULL, NULL);
    } else if (engine->type == TTS_MODEL_SUPERTONIC) {
        SherpaOnnxGenerationConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.speed = 1.0f;
        cfg.num_steps = 8;
        cfg.extra = "{\"lang\":\"en\"}";
        audio = SherpaOnnxOfflineTtsGenerateWithConfig(engine->tts, text,
                                                       &cfg, NULL, NULL);
    } else {
        /* VITS: simple sid/speed interface. */
        audio = SherpaOnnxOfflineTtsGenerate(engine->tts, text, 0, 1.0f);
    }

    if (!audio || audio->n <= 0 || !audio->samples) {
        if (audio)
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        return -1;
    }

    /* Convert float32 [-1,1] → S16_LE. */
    size_t n = static_cast<size_t>(audio->n);
    short *samples = static_cast<short *>(malloc(n * sizeof(short)));
    if (!samples) {
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        return -1;
    }

    for (size_t i = 0; i < n; ++i) {
        float v = audio->samples[i];
        if (v > 1.0f)  v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        samples[i] = static_cast<short>(v * 32767.0f);
    }

    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

    *out = samples;
    *n_out = n;
    return 0;
}

void tts_engine_free_samples(short *samples)
{
    free(samples);
}

void tts_engine_destroy(struct tts_engine *engine)
{
    if (!engine)
        return;
    if (engine->tts)
        SherpaOnnxDestroyOfflineTts(engine->tts);
    if (engine->type == TTS_MODEL_FLITE && engine->flite_voice)
        if (engine->flite_unregister)
            engine->flite_unregister(engine->flite_voice);
    if (engine->type == TTS_MODEL_ESPEAK)
        espeak_Terminate();
    free(engine);
}
