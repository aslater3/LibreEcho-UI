#ifndef LE_MDNS_LEASE_H
#define LE_MDNS_LEASE_H
#include <stddef.h>

#define LE_MDNS_ESPHOME_PACKET_MAX 1536
#define LE_MDNS_ESPHOME_NOISE "Noise_NNpsk0_25519_ChaChaPoly_SHA256"
/* UTF-8, bounded NUL-terminated fields (sizes include NUL). All are required.
 * mac is the SAME stable configured/factory identity as DeviceInfo, either
 * 12 hex digits or colon-separated. No random/per-boot fallback is supplied.
 * platform: LibreEcho or Linux; board: radar-puffin; network: wifi/ethernet;
 * api_encryption: LE_MDNS_ESPHOME_NOISE. Never put keys/PSKs in discovery.
 * friendly_name is at most 63 UTF-8 bytes (DNS-SD instance-name limit). */
struct le_mdns_esphome_metadata {
    char version[32];
    char mac[18];
    char platform[16];
    char board[32];
    char network[16];
    char friendly_name[64];
    char api_encryption[48];
    char project_name[64];
    char project_version[32];
};
/* Wire: ESPHOME/1 <decimal-port> key=<hex-UTF8> ...\n, exactly nine
 * allowlisted keys, once each. No unescaped names, unknown keys or truncation.
 * Encode returns byte length or -1; decode returns 0 or -1. */
int le_mdns_esphome_encode(char *message, size_t capacity, unsigned int port,
                           const struct le_mdns_esphome_metadata *metadata);
int le_mdns_esphome_decode(const char *message, size_t length, unsigned int *port,
                           struct le_mdns_esphome_metadata *metadata);

/* A single fixed ESPHome owner. The supervisor owns the directory fd.
 * Return values describe local file state, never confirmed publication. */
struct le_mdns_lease {
    int directory_fd;
    int owner_fd;
    unsigned long generation;
    char filename[64];
};
void le_mdns_lease_init(struct le_mdns_lease *lease, int directory_fd);
int le_mdns_lease_register_esphome(struct le_mdns_lease *lease, int owner_fd,
                                  unsigned int port,
                                  const struct le_mdns_esphome_metadata *metadata);
int le_mdns_lease_withdraw(struct le_mdns_lease *lease, int owner_fd);
#endif
