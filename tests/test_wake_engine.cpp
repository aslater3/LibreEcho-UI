#include "adapter/wake_engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <cstdlib>
#include <vector>

namespace {

uint16_t read_u16(const unsigned char *p)
{
    return static_cast<uint16_t>(
        static_cast<uint16_t>(p[0]) |
        static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32(const unsigned char *p)
{
    return static_cast<uint32_t>(p[0]) |
           static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

bool load_wav(const char *path, std::vector<int16_t> *samples)
{
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> bytes(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    std::size_t offset = 12;
    bool format_ok = false;

    if (bytes.size() < 12 ||
        std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        return false;
    while (offset + 8 <= bytes.size()) {
        const unsigned char *header = bytes.data() + offset;
        const std::size_t size = read_u32(header + 4);
        const std::size_t data_offset = offset + 8;

        if (data_offset + size > bytes.size())
            return false;
        if (std::memcmp(header, "fmt ", 4) == 0) {
            if (size < 16 ||
                read_u16(bytes.data() + data_offset) != 1 ||
                read_u16(bytes.data() + data_offset + 2) != 1 ||
                read_u32(bytes.data() + data_offset + 4) != 16000 ||
                read_u16(bytes.data() + data_offset + 14) != 16)
                return false;
            format_ok = true;
        } else if (std::memcmp(header, "data", 4) == 0) {
            if (!format_ok || size % sizeof(int16_t) != 0)
                return false;
            samples->resize(size / sizeof(int16_t));
            for (std::size_t i = 0; i < samples->size(); ++i) {
                const unsigned char *p =
                    bytes.data() + data_offset + i * 2;
                (*samples)[i] =
                    static_cast<int16_t>(read_u16(p));
            }
            return true;
        }
        offset = data_offset + size + (size & 1U);
    }
    return false;
}

}  // namespace

int main(int argc, char **argv)
{
    constexpr std::size_t block_samples = 1280;
    std::vector<int16_t> samples;
    le_wake_engine *engine;
    float max_score = 0.0f;
    unsigned int max_inference_us = 0;
    unsigned int block = 0;
    unsigned int threads = 1;

    if (argc != 3 && argc != 4) {
        std::fprintf(stderr,
                     "Usage: %s MODEL_DIRECTORY TEST.wav [THREADS]\n",
                     argv[0]);
        return 2;
    }
    if (argc == 4) {
        const long parsed = std::strtol(argv[3], nullptr, 10);

        if (parsed < 1 || parsed > 4) {
            std::fprintf(stderr, "THREADS must be between 1 and 4\n");
            return 2;
        }
        threads = static_cast<unsigned int>(parsed);
    }
    if (!load_wav(argv[2], &samples)) {
        std::fprintf(stderr, "unable to read 16 kHz mono PCM16 WAV: %s\n",
                     argv[2]);
        return 2;
    }
    engine = le_wake_engine_create(argv[1], threads);
    if (!engine)
        return 1;
    for (std::size_t offset = 0;
         offset + block_samples <= samples.size();
         offset += block_samples) {
        float score = 0.0f;
        int new_score = 0;

        if (le_wake_engine_feed(engine, samples.data() + offset,
                                block_samples, &score,
                                &new_score) < 0 ||
            !new_score) {
            le_wake_engine_destroy(engine);
            return 1;
        }
        max_score = std::max(max_score, score);
        max_inference_us = std::max(
            max_inference_us,
            le_wake_engine_last_inference_us(engine));
        std::printf("block=%u score=%.7f inference_us=%u\n",
                    block++, score,
                    le_wake_engine_last_inference_us(engine));
    }
    le_wake_engine_destroy(engine);
    std::printf("max_score=%.7f max_inference_us=%u\n",
                max_score, max_inference_us);
    if (block < 6 || max_score < 0.90f) {
        std::fprintf(stderr, "wake test failed\n");
        return 1;
    }
    return 0;
}
