/*
 * Sendspin companion — durable blob store for the SDK's persistence keys.
 *
 * The SDK owns the keyspace and every blob's format; this is a plain byte
 * store kept in ONE file (default /data/libreecho/config/sendspin-state).
 * One plain file, not a directory: the boot-time data contract of older
 * images rejects unknown directories under config/, and a rollback must not
 * be blocked by state the newer image wrote.
 *
 *   * The file is 0600, owned by this user, and never a symlink; anything
 *     else is refused rather than trusted.
 *   * Format: a version line, then one "<key> <lowercase hex>" line per blob
 *     (an empty blob is "<key> "). Keys are the SDK's short [a-z0-9_] names.
 *     Anything that does not parse is corrupt: open() fails, so the caller
 *     fails closed instead of treating it as a first boot.
 *   * Every save/erase rewrites the whole file through a 0600 temporary in
 *     the same directory, fsync, rename and directory fsync, so a reader
 *     sees the old or the new state, never a torn one. Temporaries left by a
 *     crash are removed on open.
 *
 * No Sendspin SDK dependency; exercised by the dependency-free companion lane.
 */
#ifndef LIBREECHO_SENDSPIN_BLOB_STORE_H
#define LIBREECHO_SENDSPIN_BLOB_STORE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace libreecho::sendspin {

class BlobStore {
public:
    static constexpr size_t kMaxBlobBytes = 16 * 1024;
    static constexpr size_t kMaxFileBytes = 128 * 1024;
    static constexpr size_t kMaxKeyLength = 32;
    static constexpr const char* kVersionLine = "libreecho-sendspin-state 1";

    // Loads the state file (absent = empty first-boot state). Returns nullopt
    // with a reason in *error when the file is unsafe or corrupt, or its
    // directory is unusable.
    static std::optional<BlobStore> open(const std::string& path, std::string* error);

    static bool valid_key(const std::string& key);

    std::optional<std::vector<uint8_t>> load(const std::string& key) const;
    bool save(const std::string& key, const uint8_t* data, size_t length);
    bool erase(const std::string& key);

    const std::string& path() const { return path_; }

private:
    using Blobs = std::map<std::string, std::vector<uint8_t>>;
    BlobStore(std::string path, Blobs blobs) : path_(std::move(path)), blobs_(std::move(blobs)) {}
    bool write(const Blobs& blobs) const;
    std::string path_;
    Blobs blobs_;
};

}  // namespace libreecho::sendspin

#endif
