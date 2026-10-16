/*
 * Regression fixture for the AP-compatible recovery scan path (UI #288).
 *
 * While the recovery AP owns the radio its net-up helper has stopped
 * wpa_supplicant, so a scan can only run through the kernel's AP-forced scan
 * (NL80211_SCAN_FLAG_AP -- the flag `iw dev <iface> scan ap-force` sets and the
 * platform stages `iw` for).  This links the real daemon source and inspects
 * the exact netlink message it builds for NL80211_CMD_TRIGGER_SCAN, so the flag
 * is proven on the wire rather than by grepping a comment.
 */
#define main libreecho_networkd_main
#include "../src/adapter/networkd.c"
#undef main

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static size_t attribute_offset(void)
{
    return (size_t)NLMSG_HDRLEN + (size_t)GENL_HDRLEN;
}

static const struct nlattr *find_flags(const unsigned char *buffer, size_t used)
{
    size_t start = attribute_offset();

    return nl_find(buffer + start, used - start, NL80211_ATTR_SCAN_FLAGS);
}

static int read_flags(const struct nlattr *attr, uint32_t *out)
{
    if (!attr || attr->nla_len < NLA_HDRLEN + (int)sizeof(uint32_t))
        return -1;
    memcpy(out, (const unsigned char *)attr + NLA_HDRLEN, sizeof(*out));
    return 0;
}

int main(void)
{
    unsigned char buffer[NL80211_BUFFER_SIZE];
    const struct nlattr *flags_attr;
    struct daemon_ctx ctx;
    size_t used;
    uint32_t value;

    memset(&ctx, 0, sizeof(ctx));

    /* Client mode sends no AP scan flag; the interface attribute is still the
     * first one so the message is a real TRIGGER_SCAN request. */
    used = nl80211_build_trigger_scan(buffer, sizeof(buffer), 7, 0);
    CHECK(used > attribute_offset());
    CHECK(find_flags(buffer, used) == NULL);
    CHECK(nl_find(buffer + attribute_offset(), used - attribute_offset(),
                  NL80211_ATTR_IFINDEX) != NULL);

    /* Recovery AP owns the radio: the TRIGGER_SCAN message must carry the
     * AP-forced scan flag, or the kernel refuses to scan a beaconing AP. */
    used = nl80211_build_trigger_scan(buffer, sizeof(buffer), 7,
                                      NL80211_SCAN_FLAG_AP);
    CHECK(used > attribute_offset());
    flags_attr = find_flags(buffer, used);
    CHECK(flags_attr != NULL);
    CHECK(read_flags(flags_attr, &value) == 0);
    CHECK(value == (uint32_t)NL80211_SCAN_FLAG_AP);

    /* The dispatch derives the flag from radio ownership, not from a mode
     * guess: none while the client plane owns it, the AP flag once net-up has
     * released it for the portal. */
    CHECK(scan_flags_for(&ctx) == 0u);
    ctx.recovery_configured = 1;
    CHECK(scan_flags_for(&ctx) == 0u);
    ctx.recovery.net_configured = 1;
    CHECK(scan_flags_for(&ctx) == (uint32_t)NL80211_SCAN_FLAG_AP);

    puts("networkd AP-forced recovery scan flag: ok");
    return 0;
}
