#include "wake_engine.h"

#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kBlockSamples = 1280;
constexpr std::size_t kOverlapSamples = 480;
constexpr std::size_t kMelFrames = 76;
constexpr std::size_t kMelBins = 32;
constexpr std::size_t kFeatureFrames = 16;
constexpr std::size_t kFeatureBins = 96;

std::string model_path(const char *directory, const char *filename)
{
    std::string path(directory ? directory : "");

    if (!path.empty() && path.back() != '/')
        path.push_back('/');
    path += filename;
    return path;
}

Ort::SessionOptions session_options(unsigned int threads)
{
    Ort::SessionOptions options;
    const int count = threads >= 1 && threads <= 4
        ? static_cast<int>(threads) : 1;

    options.SetIntraOpNumThreads(count);
    options.SetInterOpNumThreads(1);
    /*
     * openWakeWord runs once per 80 ms audio block.  ONNX Runtime otherwise
     * keeps every pool worker busy-spinning between those short inferences,
     * which consumed more than three MT8163 cores while the room was silent.
     * Sleeping workers add negligible wake latency at this cadence and let
     * the SoC return to idle between blocks.
     */
    options.AddConfigEntry(
        "session.intra_op.allow_spinning", "0");
    options.AddConfigEntry(
        "session.inter_op.allow_spinning", "0");
    options.SetGraphOptimizationLevel(
        GraphOptimizationLevel::ORT_ENABLE_ALL);
    /*
     * The wake tensors are tiny and fixed except for the mel input length.
     * Avoid per-session arenas retaining peak allocations on a 512 MB unit.
     */
    options.DisableCpuMemArena();
    options.DisableMemPattern();
    return options;
}

class WakeEngine {
public:
    WakeEngine(const char *directory, unsigned int threads)
        : env_(ORT_LOGGING_LEVEL_WARNING, "libreecho-wake"),
          mel_(env_, model_path(directory, "melspectrogram.onnx").c_str(),
               session_options(threads)),
          embedding_(
              env_, model_path(directory, "embedding_model.onnx").c_str(),
              session_options(threads)),
          classifier_(env_, model_path(directory, "alexa_v0.1.onnx").c_str(),
                      session_options(threads)),
          memory_(Ort::MemoryInfo::CreateCpu(
              OrtArenaAllocator, OrtMemTypeDefault))
    {
        mel_history_.fill(1.0f);
        feature_history_.fill(0.0f);
        accumulated_.fill(0);
        overlap_.fill(0);
        prime_and_warm();
    }

    int Feed(const int16_t *samples, std::size_t count,
             float *score, int *new_score)
    {
        std::size_t offset = 0;

        if (!samples || !score || !new_score)
            return -1;
        *new_score = 0;
        while (offset < count) {
            const std::size_t available =
                kBlockSamples - accumulated_count_;
            const std::size_t copying =
                std::min(available, count - offset);

            std::memcpy(accumulated_.data() + accumulated_count_,
                        samples + offset, copying * sizeof(samples[0]));
            accumulated_count_ += copying;
            offset += copying;
            if (accumulated_count_ == kBlockSamples) {
                *score = ProcessBlock(accumulated_.data(), true);
                accumulated_count_ = 0;
                *new_score = 1;
            }
        }
        return 0;
    }

    unsigned int LastInferenceUs() const
    {
        return last_inference_us_;
    }

private:
    float ProcessBlock(const int16_t *block, bool suppress_startup)
    {
        const auto started = std::chrono::steady_clock::now();
        std::array<float, kOverlapSamples + kBlockSamples> audio{};
        std::size_t input_samples = overlap_count_ + kBlockSamples;
        std::array<int64_t, 2> mel_shape{
            1, static_cast<int64_t>(input_samples)};
        std::vector<float> mel_output;
        std::size_t mel_output_frames;
        float score = 0.0f;

        for (std::size_t i = 0; i < overlap_count_; ++i)
            audio[i] = static_cast<float>(overlap_[i]);
        for (std::size_t i = 0; i < kBlockSamples; ++i)
            audio[overlap_count_ + i] = static_cast<float>(block[i]);

        auto mel_input = Ort::Value::CreateTensor<float>(
            memory_, audio.data(), input_samples,
            mel_shape.data(), mel_shape.size());
        const char *mel_input_names[] = {"input"};
        const char *mel_output_names[] = {"output"};
        auto mel_values = mel_.Run(
            Ort::RunOptions{nullptr}, mel_input_names, &mel_input, 1,
            mel_output_names, 1);
        const auto mel_info =
            mel_values[0].GetTensorTypeAndShapeInfo();
        const std::size_t mel_elements = mel_info.GetElementCount();
        if (mel_elements == 0 || mel_elements % kMelBins != 0)
            throw std::runtime_error("invalid mel output shape");
        mel_output_frames = mel_elements / kMelBins;
        const float *mel_data =
            mel_values[0].GetTensorData<float>();
        mel_output.resize(mel_elements);
        for (std::size_t i = 0; i < mel_elements; ++i)
            mel_output[i] = mel_data[i] / 10.0f + 2.0f;
        AppendMel(mel_output.data(), mel_output_frames);

        std::array<int64_t, 4> embedding_shape{
            1, static_cast<int64_t>(kMelFrames),
            static_cast<int64_t>(kMelBins), 1};
        auto embedding_input = Ort::Value::CreateTensor<float>(
            memory_, mel_history_.data(), mel_history_.size(),
            embedding_shape.data(), embedding_shape.size());
        const char *embedding_input_names[] = {"input_1"};
        const char *embedding_output_names[] = {"conv2d_19"};
        auto embedding_values = embedding_.Run(
            Ort::RunOptions{nullptr}, embedding_input_names,
            &embedding_input, 1, embedding_output_names, 1);
        const auto embedding_info =
            embedding_values[0].GetTensorTypeAndShapeInfo();
        if (embedding_info.GetElementCount() != kFeatureBins)
            throw std::runtime_error("invalid embedding output shape");
        AppendFeature(
            embedding_values[0].GetTensorData<float>());

        if (feature_count_ >= kFeatureFrames) {
            std::array<int64_t, 3> classifier_shape{
                1, static_cast<int64_t>(kFeatureFrames),
                static_cast<int64_t>(kFeatureBins)};
            auto classifier_input = Ort::Value::CreateTensor<float>(
                memory_, feature_history_.data(),
                feature_history_.size(), classifier_shape.data(),
                classifier_shape.size());
            const char *classifier_input_names[] = {
                "onnx::Flatten_0"};
            const char *classifier_output_names[] = {"13"};
            auto classifier_values = classifier_.Run(
                Ort::RunOptions{nullptr}, classifier_input_names,
                &classifier_input, 1, classifier_output_names, 1);
            score =
                classifier_values[0].GetTensorData<float>()[0];
        }

        std::copy(block + kBlockSamples - kOverlapSamples,
                  block + kBlockSamples, overlap_.begin());
        overlap_count_ = kOverlapSamples;
        if (suppress_startup && prediction_count_++ < 5)
            score = 0.0f;
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        last_inference_us_ =
            static_cast<unsigned int>(elapsed.count());
        return std::max(0.0f, std::min(1.0f, score));
    }

    void AppendMel(const float *frames, std::size_t count)
    {
        if (count >= kMelFrames) {
            std::memcpy(
                mel_history_.data(),
                frames + (count - kMelFrames) * kMelBins,
                mel_history_.size() * sizeof(float));
            return;
        }
        const std::size_t retained = kMelFrames - count;
        std::memmove(
            mel_history_.data(),
            mel_history_.data() + count * kMelBins,
            retained * kMelBins * sizeof(float));
        std::memcpy(
            mel_history_.data() + retained * kMelBins,
            frames, count * kMelBins * sizeof(float));
    }

    void AppendFeature(const float *feature)
    {
        std::memmove(
            feature_history_.data(),
            feature_history_.data() + kFeatureBins,
            (kFeatureFrames - 1) * kFeatureBins * sizeof(float));
        std::memcpy(
            feature_history_.data() +
                (kFeatureFrames - 1) * kFeatureBins,
            feature, kFeatureBins * sizeof(float));
        if (feature_count_ < kFeatureFrames)
            ++feature_count_;
    }

    void prime_and_warm()
    {
        std::array<int16_t, kBlockSamples> silence{};

        /*
         * Warm every graph and establish neutral feature history once at
         * startup.  This removes first-utterance page faults without keeping
         * the Python implementation's random four-second initialization.
         */
        for (std::size_t i = 0; i < kFeatureFrames; ++i)
            (void)ProcessBlock(silence.data(), false);
        mel_history_.fill(1.0f);
        accumulated_.fill(0);
        overlap_.fill(0);
        accumulated_count_ = 0;
        overlap_count_ = 0;
        prediction_count_ = 0;
        last_inference_us_ = 0;
    }

    Ort::Env env_;
    Ort::Session mel_;
    Ort::Session embedding_;
    Ort::Session classifier_;
    Ort::MemoryInfo memory_;
    std::array<int16_t, kBlockSamples> accumulated_{};
    std::array<int16_t, kOverlapSamples> overlap_{};
    std::array<float, kMelFrames * kMelBins> mel_history_{};
    std::array<float, kFeatureFrames * kFeatureBins> feature_history_{};
    std::size_t accumulated_count_ = 0;
    std::size_t overlap_count_ = 0;
    std::size_t feature_count_ = 0;
    unsigned int prediction_count_ = 0;
    unsigned int last_inference_us_ = 0;
};

}  // namespace

struct le_wake_engine {
    std::unique_ptr<WakeEngine> implementation;
};

extern "C" struct le_wake_engine *
le_wake_engine_create(const char *model_directory, unsigned int threads)
{
    try {
        std::unique_ptr<le_wake_engine> engine(new le_wake_engine);
        engine->implementation.reset(
            new WakeEngine(model_directory, threads));
        return engine.release();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "wake engine initialization failed: %s\n",
                     error.what());
        return nullptr;
    }
}

extern "C" int le_wake_engine_feed(struct le_wake_engine *engine,
                                   const int16_t *samples,
                                   size_t count,
                                   float *score,
                                   int *new_score)
{
    if (!engine || !engine->implementation)
        return -1;
    try {
        return engine->implementation->Feed(
            samples, count, score, new_score);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "wake engine inference failed: %s\n",
                     error.what());
        return -1;
    }
}

extern "C" unsigned int le_wake_engine_last_inference_us(
    const struct le_wake_engine *engine)
{
    return engine && engine->implementation
        ? engine->implementation->LastInferenceUs() : 0;
}

extern "C" void le_wake_engine_destroy(struct le_wake_engine *engine)
{
    delete engine;
}
