// SPDX-License-Identifier: MIT
// Sendspin companion blob store: one private 0600 state file, atomic whole-
// file replacement, key validation and fail-closed parsing. No SDK.
#include "blob_store.h"

#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using libreecho::sendspin::BlobStore;

static int failures = 0;
#define REQUIRE(c, m) do { if (!(c)) { std::fprintf(stderr, "FAIL: %s\n", m); ++failures; } } while (0)

static mode_t mode_of(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0;
}

static size_t entries(const std::string& dir) {
    size_t n = 0;
    DIR* d = ::opendir(dir.c_str());
    while (d != nullptr) {
        dirent* e = ::readdir(d);
        if (e == nullptr) break;
        if (std::string(e->d_name) != "." && std::string(e->d_name) != "..") ++n;
    }
    if (d != nullptr) ::closedir(d);
    return n;
}

static void write_file(const std::string& path, const std::string& text, mode_t mode = 0600) {
    ::unlink(path.c_str());
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    ::chmod(path.c_str(), mode);
}

static bool opens(const std::string& path) {
    std::string error;
    return BlobStore::open(path, &error).has_value();
}

int main() {
    char base_template[] = "/tmp/le-sendspin-store.XXXXXX";
    const std::string dir = ::mkdtemp(base_template);
    const std::string path = dir + "/sendspin-state";
    std::string error;

    // Absent file = empty first-boot state; nothing is created until a save.
    REQUIRE(!BlobStore::open("relative/state", &error), "relative path accepted");
    REQUIRE(!BlobStore::open(dir + "/missing-dir/state", &error), "missing directory accepted");
    auto store = BlobStore::open(path, &error);
    REQUIRE(store.has_value(), "first boot refused");
    if (!store) return 1;
    REQUIRE(!store->load("keypair") && entries(dir) == 0, "first boot not empty");

    // Round trip, 0600, one plain file, no temporary left behind.
    const std::vector<uint8_t> key(32, 0xab);
    const std::vector<uint8_t> rec = {'{', '}', 0, 255};
    REQUIRE(store->save("keypair", key.data(), key.size()), "save failed");
    REQUIRE(store->save("rec_0", rec.data(), rec.size()), "second save failed");
    REQUIRE(store->save("rec_1", nullptr, 0), "empty save failed");
    REQUIRE(mode_of(path) == 0600, "state file not 0600");
    REQUIRE(entries(dir) == 1, "temporary or extra file left behind");

    // A fresh open (next boot) reads exactly what was saved.
    auto reopened = BlobStore::open(path, &error);
    REQUIRE(reopened.has_value(), "saved state does not reopen");
    if (reopened) {
        REQUIRE(reopened->load("keypair") == key, "keypair lost across reopen");
        REQUIRE(reopened->load("rec_0") == rec, "binary record lost across reopen");
        auto empty = reopened->load("rec_1");
        REQUIRE(empty && empty->empty(), "emptied slot not an empty blob");
        REQUIRE(reopened->erase("rec_0") && reopened->erase("absent"), "erase failed");
    }
    reopened = BlobStore::open(path, &error);
    REQUIRE(reopened && !reopened->load("rec_0") && reopened->load("keypair") == key,
            "erase not durable or removed the wrong key");

    // Keys cannot be malformed; oversized blobs refused.
    for (const char* bad : {"", "../keypair", "a b", "UPPER", "x\ny",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        REQUIRE(!BlobStore::valid_key(bad), "unsafe key accepted");
        REQUIRE(!store->save(bad, key.data(), key.size()), "unsafe key written");
    }
    std::vector<uint8_t> big(BlobStore::kMaxBlobBytes + 1, 7);
    REQUIRE(!store->save("big", big.data(), big.size()), "oversized blob written");

    // A crash's leftover temporary is removed on open.
    write_file(path + ".tmp.abc123", "partial");
    REQUIRE(opens(path) && entries(dir) == 1, "stale temporary not removed");

    // Corrupt or unsafe state is refused, never treated as a first boot.
    const std::string good = std::string(BlobStore::kVersionLine) + "\nkeypair 00ff\n";
    write_file(path, good);
    REQUIRE(opens(path), "valid hand-written state refused");
    for (const char* bad : {"garbage\n",
                            "libreecho-sendspin-state 2\n",
                            "libreecho-sendspin-state 1\nkeypair 0f0\n",         // odd hex length
                            "libreecho-sendspin-state 1\nkeypair zz\n",          // not hex
                            "libreecho-sendspin-state 1\nkeypair 00\nkeypair 00\n",  // duplicate
                            "libreecho-sendspin-state 1\nBAD 00\n",              // bad key
                            "libreecho-sendspin-state 1\nkeypair 00",            // unterminated
                            "libreecho-sendspin-state 1\nnospace\n"}) {
        write_file(path, bad);
        REQUIRE(!opens(path), "corrupt state accepted");
    }
    write_file(path, good, 0644);
    REQUIRE(!opens(path), "world-readable state accepted");
    write_file(path, good);
    ::unlink((dir + "/alias").c_str());
    REQUIRE(::symlink(path.c_str(), (dir + "/alias").c_str()) == 0, "symlink setup");
    REQUIRE(!opens(dir + "/alias"), "symlinked state followed");
    ::mkdir((dir + "/subdir").c_str(), 0700);
    REQUIRE(!opens(dir + "/subdir"), "directory accepted as state file");

    const std::string cleanup = "rm -rf " + dir;
    (void)!std::system(cleanup.c_str());
    if (failures) return 1;
    std::printf("test_sendspin_blob_store: private atomic state file PASS\n");
    return 0;
}
