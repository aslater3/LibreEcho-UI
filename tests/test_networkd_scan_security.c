/* Regression fixture for an encrypted RSN network without a PSK AKM. */
#define main libreecho_networkd_main
#include "../src/adapter/networkd.c"
#undef main

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    /* SSID "Enterprise"; RSN uses 802.1X AKM (00:0f:ac:1), not PSK/SAE. */
    static const unsigned char ies[] = {
        0, 10, 'E', 'n', 't', 'e', 'r', 'p', 'r', 'i', 's', 'e',
        48, 18,
        1, 0, 0, 15, 172, 4,
        1, 0, 0, 15, 172, 4,
        1, 0, 0, 15, 172, 1
    };
    char ssid[IW_ESSID_MAX_SIZE + 1];
    char capabilities[128];
    int encrypted = 0;

    nl80211_parse_ies(ies, sizeof(ies), ssid, sizeof(ssid), &encrypted,
                      capabilities, sizeof(capabilities));
    CHECK(!strcmp(ssid, "Enterprise"));
    CHECK(encrypted);
    CHECK(!strcmp(capabilities, "encrypted"));
    CHECK(!strcmp(scan_security(capabilities), "wpa"));
    puts("encrypted non-PSK RSN scan classification: ok");
    return 0;
}
