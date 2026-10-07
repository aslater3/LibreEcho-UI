/* Playback must keep ahead of real time at esphomed's real loop cadence.
 *
 * On a Radar (HZ=100) esphomed's poll loop wakes about every 20 ms. Before the
 * fix each esp_playback_tick() did one decode step OR one bus write, so a
 * 22.05 kHz announcement produced less output than wall time and the audio
 * engine starved, closing and reopening the PCM every ~2.5 s ("interrupted
 * every second or so"). Drive the real tick at that cadence and require the
 * written output to stay ahead of the clock for the whole stream. */
#include "../src/adapter/esphome_playback.c"
#include <assert.h>
#include <sys/stat.h>

static struct esp_playback p;

static void put32(unsigned char *b, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++)
        b[i] = (unsigned char)(n >> (i * 8));
}

static size_t make_wav(unsigned char *b, unsigned rate, unsigned seconds)
{
    size_t frames = (size_t)rate * seconds, n = 44 + frames * 2;

    memset(b, 0, n);
    memcpy(b, "RIFF", 4); put32(b + 4, (uint32_t)(n - 8));
    memcpy(b + 8, "WAVEfmt ", 8); put32(b + 16, 16); b[20] = 1; b[22] = 1;
    put32(b + 24, rate); put32(b + 28, rate * 2); b[32] = 2; b[34] = 16;
    memcpy(b + 36, "data", 4); put32(b + 40, (uint32_t)(frames * 2));
    for (size_t i = 0; i < frames; i++) {
        short v = (short)((i * 97) % 6000 - 3000);
        b[44 + 2 * i] = (unsigned char)v; b[45 + 2 * i] = (unsigned char)((unsigned short)v >> 8);
    }
    return n;
}

static void run(unsigned rate, unsigned tick_ms)
{
    char dir[] = "/tmp/esp-pacing-XXXXXX", fifo[64];
    static unsigned char wav[44 + 48000 * 2 * 3], sink[1 << 16];
    size_t n = make_wav(wav, rate, 3);
    uint64_t now = 1000, worst = UINT64_MAX;
    int reader, rc = 0, ticks = 0;

    assert(mkdtemp(dir));
    snprintf(fifo, sizeof fifo, "%s/bus", dir);
    assert(mkfifo(fifo, 0600) == 0);
    reader = open(fifo, O_RDONLY | O_NONBLOCK);
    assert(reader >= 0);

    esp_playback_init(&p);
    memcpy(p.buffer, wav, n); p.body_len = n;
    snprintf(p.bus, sizeof p.bus, "%s", fifo);
    assert(prepare(&p, now) == 0);

    while (!rc && ticks < 10000) {
        /* Engine side: consume whatever is queued, like the 2-period mixer. */
        while (read(reader, sink, sizeof sink) > 0) {}
        rc = esp_playback_tick(&p, now);
        if (!rc && p.state == 6 && now > p.origin + 200) {
            uint64_t written_ms = p.frames * 1000 / 48000, elapsed = now - p.origin;
            uint64_t lead = written_ms >= elapsed ? written_ms - elapsed : 0;
            if (written_ms < elapsed) {
                fprintf(stderr, "FAIL rate=%u tick=%ums: underrun at %llums, written %llums\n",
                        rate, tick_ms, (unsigned long long)elapsed, (unsigned long long)written_ms);
                assert(0);
            }
            if (lead < worst)
                worst = lead;
        }
        now += tick_ms; ticks++;
    }
    while (read(reader, sink, sizeof sink) > 0) {}
    assert(rc == 1);
    printf("pacing rate=%u tick=%ums: completed after %d ticks, min lead %llums: OK\n",
           rate, tick_ms, ticks, (unsigned long long)worst);
    close(reader); unlink(fifo); rmdir(dir);
}

int main(void)
{
    run(22050, 20);
    run(22050, 30);
    run(16000, 20);
    run(48000, 20);
    return 0;
}
