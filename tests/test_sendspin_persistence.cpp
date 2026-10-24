// SPDX-License-Identifier: MIT
// Real SDK client over the companion's file-backed persistence: the identity
// and the Pairing PSK token survive a restart, and corrupt state is refused
// before start() instead of being mistaken for a first boot. No network.
#include "sdk_persistence.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include <sendspin/client.h>
#include <sendspin/config.h>

using libreecho::sendspin::BlobStore;
using libreecho::sendspin::SdkPersistence;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

struct NoNetwork : ::sendspin::SendspinNetworkProvider {
    bool is_network_ready() override { return false; }  // never listen
};

struct Started {
    std::string client_id;
    std::string token;
};

static Started start_client(const std::string& path) {
    auto store = BlobStore::open(path, nullptr);
    if (!store) return {};
    SdkPersistence persistence(std::move(*store));
    ::sendspin::SendspinClientConfig cfg;
    cfg.name = "persistence fixture";
    ::sendspin::SendspinClient client(cfg);
    NoNetwork network;
    client.set_network_provider(&network);
    client.set_persistence_provider(&persistence);
    ::sendspin::PlayerRoleConfig player;
    player.audio_formats.push_back({::sendspin::SendspinCodecFormat::PCM, 2, 48000, 16});
    client.add_player(player);
    Started out;
    if (!client.start()) return out;
    out.client_id = client.client_id();
    out.token = client.pairing_token().value_or("");
    client.stop();
    return out;
}

static bool verify(const std::string& path, std::string* why) {
    std::string error;
    auto store = BlobStore::open(path, &error);
    if (!store) {
        if (why != nullptr) *why = "open: " + error;
        return false;
    }
    SdkPersistence persistence(std::move(*store));
    return persistence.verify_state(::sendspin::SendspinClientConfig{}.max_pairing_records, why);
}

// Replace one blob through the store itself (a valid file holding a bad blob).
static void put(const std::string& path, const char* key, const std::string& bytes) {
    auto store = BlobStore::open(path, nullptr);
    if (store) store->save(key, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

int main() {
    char base_template[] = "/tmp/le-sendspin-persist.XXXXXX";
    const std::string base = ::mkdtemp(base_template);
    const std::string dir = base + "/sendspin-state";  // the state file
    std::string why;

    REQUIRE(verify(dir, &why), "empty (first boot) state refused");
    const Started first = start_client(dir);
    REQUIRE(first.client_id.size() == 43, "no client identity after first start");
    REQUIRE(first.token.rfind("SP:", 0) == 0, "no pairing token after first start");
    struct stat st{};
    REQUIRE(::stat(dir.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
            "identity not persisted in a 0600 file");
    REQUIRE(verify(dir, &why), "state written by the SDK does not verify");

    const Started second = start_client(dir);
    REQUIRE(second.client_id == first.client_id, "identity regenerated across a restart");
    REQUIRE(second.token == first.token, "pairing token changed across a restart");

    // A truncated identity is corrupt, not absent.
    put(dir, "keypair", std::string(31, 'x'));
    REQUIRE(!verify(dir, &why) && why == "keypair", "truncated identity accepted");
    put(dir, "keypair", std::string(32, 'x'));
    REQUIRE(verify(dir, &why), "valid-length identity refused");

    // Pairing records: an emptied slot is fine, garbage is corrupt.
    put(dir, "rec_0", "");
    REQUIRE(verify(dir, &why), "emptied record slot refused");
    put(dir, "rec_0", "{not json");
    REQUIRE(!verify(dir, &why) && why == "rec_0", "garbage pairing record accepted");
    put(dir, "rec_0", "");
    put(dir, "pairing_psk", "{\"v\":1,\"psk_id\":\"x\",\"psk\":\"short\"}");
    REQUIRE(!verify(dir, &why) && why == "pairing_psk", "bad Pairing PSK accepted");

    const std::string cleanup = "rm -rf " + base;
    (void)!std::system(cleanup.c_str());
    if (failures) return 1;
    std::printf("test_sendspin_persistence: identity/pairing persistence PASS\n");
    return 0;
}
