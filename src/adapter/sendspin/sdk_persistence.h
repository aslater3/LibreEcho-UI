/*
 * Sendspin companion — SDK persistence provider over BlobStore.
 *
 * The SDK writes the client identity (KEYPAIR), the Pairing PSK, the pairing
 * config and the long-term pairing records through this provider. A lost or
 * unreadable blob must not be mistaken for a first boot: the SDK would mint a
 * new identity and every paired server would lose this player. So
 * verify_state() is run before start() and the companion refuses to run on
 * corrupt state (the owner unpairs or resets from the Web UI instead).
 *
 * This header has the SDK dependency and is compiled only by the companion's
 * opt-in target.
 */
#ifndef LIBREECHO_SENDSPIN_SDK_PERSISTENCE_H
#define LIBREECHO_SENDSPIN_SDK_PERSISTENCE_H

#include <string>
#include <string_view>

#include <sendspin/client.h>
#include <sendspin/config.h>
#include <sendspin/persistence_codec.h>

#include "blob_store.h"

namespace libreecho::sendspin {

class SdkPersistence : public ::sendspin::SendspinPersistenceProvider {
public:
    explicit SdkPersistence(BlobStore store) : store_(std::move(store)) {}

    std::optional<std::vector<uint8_t>> load_blob(const std::string& key) override {
        return store_.load(key);
    }
    bool save_blob(const std::string& key, const uint8_t* data, size_t len) override {
        return store_.save(key, data, len);
    }
    bool erase_blob(const std::string& key) override { return store_.erase(key); }

    // Every stored blob the SDK will read back must be readable and decode.
    // Returns false with the offending key in *why.
    bool verify_state(size_t max_records, std::string* why) const {
        namespace keys = ::sendspin::persistence_keys;
        auto view = [](const std::vector<uint8_t>& b) {
            return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
        };
        // BlobStore::open() already refused an unreadable or malformed file;
        // here each blob the SDK will read back must also decode.
        auto check = [&](const std::string& key, auto decodes) {
            const auto blob = store_.load(key);
            if (blob && !decodes(*blob)) {
                if (why != nullptr) *why = key;
                return false;
            }
            return true;
        };
        if (!check(keys::KEYPAIR, [](const auto& b) { return b.size() == 32; })) return false;
        if (!check(keys::PAIRING_PSK, [&](const auto& b) {
                return b.empty() || ::sendspin::decode_pairing_psk(view(b)).has_value();
            }))
            return false;
        if (!check(keys::PAIR_CONFIG, [&](const auto& b) {
                return ::sendspin::decode_pairing_config(view(b)).has_value();
            }))
            return false;
        for (size_t slot = 0; slot < max_records; ++slot) {
            if (!check(keys::record_slot_key(slot), [&](const auto& b) {
                    return b.empty() || ::sendspin::decode_pairing_record(view(b)).has_value();
                }))
                return false;
        }
        return true;
    }

private:
    BlobStore store_;
};

}  // namespace libreecho::sendspin

#endif
