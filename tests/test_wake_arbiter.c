/* Only the nearest device answers (#408): claim format, ranking, level, and
 * two arbiters settling over real loopback UDP with the timing rules, the
 * owner switch, pairing, and forged, replayed and stale claims refused. */
#define _POSIX_C_SOURCE 200809L
#include "adapter/wake_arbiter.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char *CODE = "0011-2233-4455-6677-8899-aabb-ccdd-eeff-0123-4567-89ab-cdef-fedc-ba98-7654-3210";
static const char *OTHER = "ffee-2233-4455-6677-8899-aabb-ccdd-eeff-0123-4567-89ab-cdef-fedc-ba98-7654-3210";
static const long long WALL = 1790000000LL;
static char dir[] = "/tmp/le-wake-arb-XXXXXX";
static char config_on[160], config_off[160], key_path[160], other_key_path[160];

static void write_file(const char *path, const char *text){FILE *f = fopen(path, "w");CHECK(f != NULL);if(f){fputs(text, f);fclose(f);}}

static unsigned short free_port(void)
{
    struct sockaddr_in a; socklen_t n = sizeof(a);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    unsigned short port;
    memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(fd >= 0 && !bind(fd, (struct sockaddr *)&a, sizeof(a)) && !getsockname(fd, (struct sockaddr *)&a, &n));
    port = ntohs(a.sin_port);
    close(fd);
    return port;
}

/* Two arbiters wired to each other on loopback, both switched on and paired. */
static void pair_up(struct le_wake_arbiter *a, const char *id_a, struct le_wake_arbiter *b, const char *id_b)
{
    unsigned short pa = free_port(), pb = free_port();
    le_wake_arbiter_init(a, config_on, key_path, 0);
    le_wake_arbiter_init(b, config_on, key_path, 0);
    snprintf(a->self.id, sizeof(a->self.id), "%s", id_a);
    snprintf(b->self.id, sizeof(b->self.id), "%s", id_b);
    a->bind_port = pa; b->bind_port = pb;
    snprintf(a->dest_host, sizeof(a->dest_host), "127.0.0.1"); a->dest_port = pb;
    snprintf(b->dest_host, sizeof(b->dest_host), "127.0.0.1"); b->dest_port = pa;
    le_wake_arbiter_reload(a, 0);
    le_wake_arbiter_reload(b, 0);
    CHECK(le_wake_arbiter_active(a) && le_wake_arbiter_active(b));
}

/* Runs both arbiters' ticks at times now..until, returning each one's
 * settled result (WON/LOST) or IDLE if it never settled. */
static void run(struct le_wake_arbiter *a, int *ra, struct le_wake_arbiter *b, int *rb, uint64_t from, uint64_t until)
{
    uint64_t t;
    for (t = from; t <= until; t += 5) {
        int x = le_wake_arbiter_tick(a, t, WALL), y = le_wake_arbiter_tick(b, t, WALL);
        if (x == LE_WAKE_ARB_WON || x == LE_WAKE_ARB_LOST) *ra = x;
        if (y == LE_WAKE_ARB_WON || y == LE_WAKE_ARB_LOST) *rb = y;
        { struct timespec pause = {0, 200000L}; nanosleep(&pause, NULL); } /* let loopback deliver */
    }
}

static void test_format_parse(void)
{
    struct le_wake_claim c = {"0123456789abcdef", -253, 987}, d;
    char text[96];
    CHECK(le_wake_claim_format(&c, text, sizeof(text)) > 0);
    CHECK(!strcmp(text, "v=1 id=0123456789abcdef level=-253 score=987"));
    CHECK(!le_wake_claim_parse(text, strlen(text), &d) && !strcmp(d.id, c.id) && d.level == -253 && d.score == 987);
    CHECK(!le_wake_claim_parse("v=1 id=0123456789abcdef level=0 score=0", 39, &d) && d.level == 0 && d.score == 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789ABCDEF level=-1 score=1", 40, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=5 score=1", 39, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=-1201 score=1", 43, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=-1 score=1001", 43, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=-1 score=1 ", 41, &d) < 0);
    CHECK(le_wake_claim_parse("v=2 id=0123456789abcdef level=-1 score=1", 40, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcde level=-1 score=1", 39, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=- score=1", 39, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=-1 score=+1", 41, &d) < 0);
    CHECK(le_wake_claim_parse("v=1 id=0123456789abcdef level=-1\0score=1", 40, &d) < 0);
    c.level = 1; CHECK(le_wake_claim_format(&c, text, sizeof(text)) < 0);
}

static void test_rank_and_level(void)
{
    struct le_wake_claim loud = {"ffffffffffffffff", -200, 500}, quiet = {"0000000000000000", -300, 1000}, tie = {"0000000000000001", -200, 500};
    int16_t samples[1600];
    size_t i;
    CHECK(le_wake_claim_rank(&loud, &quiet) > 0 && le_wake_claim_rank(&quiet, &loud) < 0);
    /* Same level: score decides. */
    quiet.level = -200; CHECK(le_wake_claim_rank(&quiet, &loud) > 0);
    /* Same level and score: the lower id. */
    CHECK(le_wake_claim_rank(&tie, &loud) > 0 && le_wake_claim_rank(&loud, &tie) < 0 && le_wake_claim_rank(&loud, &loud) == 0);
    memset(samples, 0, sizeof(samples));
    CHECK(le_wake_level(samples, 1600) == LE_WAKE_ARB_LEVEL_MIN);
    CHECK(le_wake_level(NULL, 0) == LE_WAKE_ARB_LEVEL_MIN);
    for (i = 0; i < 1600; i++) samples[i] = (int16_t)(i & 1 ? 32767 : -32767);
    CHECK(le_wake_level(samples, 1600) == 0);
    for (i = 0; i < 1600; i++) samples[i] = (int16_t)(i & 1 ? 16384 : -16384);
    CHECK(le_wake_level(samples, 1600) == -60);
    for (i = 0; i < 1600; i++) samples[i] = (int16_t)(i & 1 ? 328 : -328);
    CHECK(le_wake_level(samples, 1600) == -400);
    CHECK(le_wake_score_milli(0.9876f) == 988 && le_wake_score_milli(2.0f) == 1000 && le_wake_score_milli(-1.0f) == 0);
}

static void test_simultaneous(void)
{
    struct le_wake_arbiter a, b;
    int ra = 0, rb = 0;
    /* The louder device answers even with the higher id. */
    pair_up(&a, "ffffffffffffffff", &b, "0000000000000000");
    CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -200, 900) == LE_WAKE_ARB_PENDING);
    CHECK(le_wake_arbiter_claim(&b, 1010, WALL, -350, 999) == LE_WAKE_ARB_PENDING);
    CHECK(le_wake_arbiter_timeout(&a, 1000) > 0);
    run(&a, &ra, &b, &rb, 1000, 1500);
    CHECK(ra == LE_WAKE_ARB_WON && rb == LE_WAKE_ARB_LOST);
    CHECK(a.won == 1 && b.yielded == 1 && !strcmp(b.beaten_by, "ffffffffffffffff"));
    /* The resend arrives as a replay and is not counted twice. */
    CHECK(a.heard_count == 1 && b.heard_count == 1);
    le_wake_arbiter_close(&a); le_wake_arbiter_close(&b);

    /* A dead heat: the lower id answers, on both sides. */
    ra = rb = 0;
    pair_up(&a, "1111111111111111", &b, "2222222222222222");
    CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -300, 900) == LE_WAKE_ARB_PENDING);
    CHECK(le_wake_arbiter_claim(&b, 1000, WALL, -300, 900) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 1000, 1500);
    CHECK(ra == LE_WAKE_ARB_WON && rb == LE_WAKE_ARB_LOST);

    /* The winner's next wake must not yield to the claim it already beat. */
    ra = rb = 0;
    CHECK(le_wake_arbiter_claim(&a, 2500, WALL, -300, 900) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 2500, 3000);
    CHECK(ra == LE_WAKE_ARB_WON);
    le_wake_arbiter_close(&a); le_wake_arbiter_close(&b);
}

static void test_late_device_yields_silently(void)
{
    struct le_wake_arbiter a, b;
    int ra = 0, rb = 0;
    unsigned long heard;
    pair_up(&a, "ffffffffffffffff", &b, "0000000000000000");
    CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -400, 900) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 1000, 1250);
    heard = a.heard_count;
    /* b detects 250 ms after hearing a's claim: a's window is nearly shut,
     * so b stays quiet even though it is louder, and sends nothing. */
    CHECK(le_wake_arbiter_claim(&b, 1250, WALL, -100, 999) == LE_WAKE_ARB_LOST);
    run(&a, &ra, &b, &rb, 1250, 1600);
    CHECK(ra == LE_WAKE_ARB_WON && rb == 0 && a.heard_count == heard);
    /* Inside the compare span the louder late device wins instead. */
    le_wake_arbiter_close(&a); le_wake_arbiter_close(&b);
    ra = rb = 0;
    pair_up(&a, "ffffffffffffffff", &b, "0000000000000000");
    CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -400, 900) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 1000, 1100);
    CHECK(le_wake_arbiter_claim(&b, 1100, WALL, -100, 999) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 1100, 1600);
    CHECK(ra == LE_WAKE_ARB_LOST && rb == LE_WAKE_ARB_WON);
    /* Long after, the old claim no longer silences anyone. */
    ra = rb = 0;
    CHECK(le_wake_arbiter_claim(&a, 5000, WALL, -400, 900) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 5000, 5500);
    CHECK(ra == LE_WAKE_ARB_WON);
    le_wake_arbiter_close(&a); le_wake_arbiter_close(&b);
}

static void test_refused_claims(void)
{
    struct le_wake_arbiter a, b;
    struct le_peer_key other, same;
    struct le_wake_claim loud = {"0000000000000000", 0, 1000};
    char body[96], packet[LE_WAKE_ARB_PACKET_MAX], nonce[LE_PEER_NONCE_HEX + 1];
    int n, ra = 0, rb = 0;
    pair_up(&a, "ffffffffffffffff", &b, "1111111111111111");
    CHECK(!le_peer_key_from_code(&other, OTHER) && !le_peer_key_from_code(&same, CODE));
    CHECK(le_wake_claim_format(&loud, body, sizeof(body)) > 0);
    /* Another house's key: refused, so it cannot silence us. */
    CHECK(!le_peer_nonce(nonce));
    n = le_peer_seal(&other, LE_WAKE_ARB_TYPE, WALL, nonce, body, strlen(body), packet, sizeof(packet));
    CHECK(n > 0 && le_wake_arbiter_packet(&a, 1000, WALL, packet, (size_t)n) == LE_PEER_EKEY);
    /* Altered after signing. */
    n = le_peer_seal(&same, LE_WAKE_ARB_TYPE, WALL, nonce, body, strlen(body), packet, sizeof(packet));
    CHECK(n > 0);
    packet[n - 1] = packet[n - 1] == '0' ? '1' : '0';
    CHECK(le_wake_arbiter_packet(&a, 1000, WALL, packet, (size_t)n) == LE_PEER_EMAC);
    /* Signed as another message kind. */
    CHECK(!le_peer_nonce(nonce));
    n = le_peer_seal(&same, "peer.proof", WALL, nonce, body, strlen(body), packet, sizeof(packet));
    CHECK(n > 0 && le_wake_arbiter_packet(&a, 1000, WALL, packet, (size_t)n) == LE_PEER_ETYPE);
    /* Too old for the clock window. */
    CHECK(!le_peer_nonce(nonce));
    n = le_peer_seal(&same, LE_WAKE_ARB_TYPE, WALL - 100, nonce, body, strlen(body), packet, sizeof(packet));
    CHECK(n > 0 && le_wake_arbiter_packet(&a, 1000, WALL, packet, (size_t)n) == LE_PEER_ESTALE);
    /* Valid signature, bad body. */
    CHECK(!le_peer_nonce(nonce));
    n = le_peer_seal(&same, LE_WAKE_ARB_TYPE, WALL, nonce, "v=1 id=zz", 9, packet, sizeof(packet));
    CHECK(n > 0 && le_wake_arbiter_packet(&a, 1000, WALL, packet, (size_t)n) == LE_PEER_EFORMAT);
    CHECK(a.heard_count == 0 && a.rejected == 5);
    /* None of those counted: a still answers alone. */
    CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -500, 500) == LE_WAKE_ARB_PENDING);
    run(&a, &ra, &b, &rb, 1000, 1500);
    CHECK(ra == LE_WAKE_ARB_WON);
    /* A genuine claim played again is refused the second time. */
    CHECK(!le_peer_nonce(nonce));
    n = le_peer_seal(&same, LE_WAKE_ARB_TYPE, WALL, nonce, body, strlen(body), packet, sizeof(packet));
    CHECK(n > 0 && le_wake_arbiter_packet(&a, 2000, WALL, packet, (size_t)n) == LE_PEER_OK);
    CHECK(le_wake_arbiter_packet(&a, 2001, WALL, packet, (size_t)n) == LE_PEER_EREPLAY && a.heard_count == 1);
    /* Our own claim, looped back by the broadcast, is ignored. */
    {
        struct le_wake_claim self = a.self;
        unsigned long before = a.heard_count;
        le_wake_arbiter_heard(&a, 2002, &self);
        CHECK(a.heard_count == before);
    }
    le_wake_arbiter_close(&a); le_wake_arbiter_close(&b);
}

static void test_switch_and_pairing(void)
{
    struct le_wake_arbiter a;
    /* Switch off (the default): answer at once, no socket. */
    le_wake_arbiter_init(&a, config_off, key_path, 0);
    a.bind_port = free_port();
    le_wake_arbiter_reload(&a, 0);
    CHECK(!le_wake_arbiter_active(&a) && a.fd < 0 && le_wake_arbiter_fd(&a) < 0);
    CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -100, 900) == LE_WAKE_ARB_OFF);
    le_wake_arbiter_close(&a);
    /* Switch on but not paired. */
    {
        char missing[200];
        snprintf(missing, sizeof(missing), "%s/no-such.key", dir);
        le_wake_arbiter_init(&a, config_on, missing, 0);
        a.bind_port = free_port();
        le_wake_arbiter_reload(&a, 0);
        CHECK(!le_wake_arbiter_active(&a) && le_wake_arbiter_claim(&a, 1000, WALL, -100, 900) == LE_WAKE_ARB_OFF);
        le_wake_arbiter_close(&a);
    }
    /* Paired and on, then switched off mid-window: the pending claim is
     * dropped and tick reports IDLE, so the caller answers. */
    {
        char cfg[200];
        snprintf(cfg, sizeof(cfg), "%s/live.json", dir);
        write_file(cfg, "{\"hostname\":\"x\",\"nearest_answers\":true}");
        le_wake_arbiter_init(&a, cfg, key_path, 0);
        a.bind_port = free_port();
        snprintf(a.dest_host, sizeof(a.dest_host), "127.0.0.1"); a.dest_port = free_port();
        le_wake_arbiter_reload(&a, 0);
        CHECK(le_wake_arbiter_active(&a));
        CHECK(le_wake_arbiter_claim(&a, 1000, WALL, -100, 900) == LE_WAKE_ARB_PENDING);
        /* A second wake while the first is settled is dropped. */
        CHECK(le_wake_arbiter_claim(&a, 1010, WALL, -100, 900) == LE_WAKE_ARB_LOST);
        write_file(cfg, "{\"hostname\":\"x\",\"nearest_answers\":false,\"pad\":1}");
        CHECK(le_wake_arbiter_tick(&a, 1000 + LE_WAKE_ARB_RELOAD_MS, WALL) == LE_WAKE_ARB_IDLE);
        CHECK(!le_wake_arbiter_active(&a) && !a.pending);
        /* Unpairing (key file removed) also switches it off. */
        write_file(cfg, "{\"hostname\":\"x\",\"nearest_answers\":true,\"pad\":4444}");
        le_wake_arbiter_reload(&a, 5000);
        CHECK(le_wake_arbiter_active(&a));
        {
            char moved[200];
            snprintf(moved, sizeof(moved), "%s/moved.key", dir);
            CHECK(!rename(key_path, moved));
            le_wake_arbiter_reload(&a, 6000);
            CHECK(!le_wake_arbiter_active(&a) && !a.key.set);
            CHECK(!rename(moved, key_path));
        }
        {
            char status[256];
            CHECK(le_wake_arbiter_status_json(&a, status, sizeof(status)) > 0);
            CHECK(strstr(status, "\"enabled\":true") && strstr(status, "\"paired\":false") && strstr(status, "\"active\":false"));
        }
        le_wake_arbiter_close(&a);
    }
}

int main(void)
{
    struct le_peer_key key;
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(config_on, sizeof(config_on), "%s/on.json", dir);
    snprintf(config_off, sizeof(config_off), "%s/off.json", dir);
    snprintf(key_path, sizeof(key_path), "%s/peer.key", dir);
    snprintf(other_key_path, sizeof(other_key_path), "%s/other.key", dir);
    write_file(config_on, "{\"hostname\":\"kitchen\",\"room\":\"Kitchen\",\"nearest_answers\":true}");
    write_file(config_off, "{\"hostname\":\"kitchen\"}");
    CHECK(!le_peer_key_from_code(&key, CODE) && !le_peer_key_save(&key, key_path));
    test_format_parse();
    test_rank_and_level();
    test_simultaneous();
    test_late_device_yields_silently();
    test_refused_claims();
    test_switch_and_pairing();
    {
        char cmd[200];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd)) {}
    }
    if (failures) { fprintf(stderr, "test_wake_arbiter: %d failure(s)\n", failures); return 1; }
    printf("test_wake_arbiter: ok\n");
    return 0;
}
