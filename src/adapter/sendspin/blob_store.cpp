#include "blob_store.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace libreecho::sendspin {

namespace {

bool write_all(int fd, const char* data, size_t length) {
    while (length > 0) {
        const ssize_t n = ::write(fd, data, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string dirname_of(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == 0 ? "/" : path.substr(0, slash);
}

// Remove "<base>.tmp.*" files left by a crash between mkstemp and rename.
void remove_stale_temporaries(const std::string& path) {
    const std::string dir = dirname_of(path);
    const std::string prefix = path.substr(path.rfind('/') + 1) + ".tmp.";
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return;
    while (dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, prefix.c_str(), prefix.size()) == 0)
            ::unlinkat(::dirfd(d), e->d_name, 0);
    }
    ::closedir(d);
}

}  // namespace

bool BlobStore::valid_key(const std::string& key) {
    if (key.empty() || key.size() > kMaxKeyLength) return false;
    for (const char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

std::optional<BlobStore> BlobStore::open(const std::string& path, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<BlobStore> {
        if (error != nullptr) *error = std::string(why) + ": " + path;
        return std::nullopt;
    };
    if (path.size() < 2 || path.front() != '/' || path.back() == '/')
        return fail("state file must be an absolute file path");
    struct stat st{};
    if (::stat(dirname_of(path).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return fail("state directory unavailable");
    remove_stale_temporaries(path);

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return BlobStore(path, {});
        return fail("state file unreadable or a symlink");
    }
    std::string text;
    bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok && st.st_uid != ::geteuid()) ok = false;
    if (ok && (st.st_mode & 077) != 0) ok = false;
    if (ok && static_cast<size_t>(st.st_size) > kMaxFileBytes) ok = false;
    if (ok) {
        text.resize(static_cast<size_t>(st.st_size));
        size_t got = 0;
        while (ok && got < text.size()) {
            const ssize_t n = ::read(fd, text.data() + got, text.size() - got);
            if (n < 0 && errno == EINTR) continue;
            ok = n > 0;
            if (ok) got += static_cast<size_t>(n);
        }
    }
    ::close(fd);
    if (!ok) return fail("state file unsafe (owner, mode, type or size)");

    Blobs blobs;
    size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        if (end == std::string::npos) return fail("state file corrupt (unterminated line)");
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        if (first) {
            if (line != kVersionLine) return fail("state file corrupt (version)");
            first = false;
            continue;
        }
        const size_t space = line.find(' ');
        if (space == std::string::npos) return fail("state file corrupt (line)");
        const std::string key = line.substr(0, space);
        const std::string hex = line.substr(space + 1);
        if (!valid_key(key) || blobs.count(key) != 0 || hex.size() % 2 != 0 ||
            hex.size() / 2 > kMaxBlobBytes)
            return fail("state file corrupt (entry)");
        std::vector<uint8_t> blob(hex.size() / 2);
        for (size_t i = 0; i < blob.size(); ++i) {
            const int hi = hex_value(hex[2 * i]), lo = hex_value(hex[2 * i + 1]);
            if (hi < 0 || lo < 0) return fail("state file corrupt (hex)");
            blob[i] = static_cast<uint8_t>(hi << 4 | lo);
        }
        blobs.emplace(key, std::move(blob));
    }
    if (first && !text.empty()) return fail("state file corrupt (version)");
    return BlobStore(path, std::move(blobs));
}

std::optional<std::vector<uint8_t>> BlobStore::load(const std::string& key) const {
    const auto it = blobs_.find(key);
    if (it == blobs_.end()) return std::nullopt;
    return it->second;
}

bool BlobStore::write(const Blobs& blobs) const {
    static const char digits[] = "0123456789abcdef";
    std::string text = std::string(kVersionLine) + "\n";
    for (const auto& [key, blob] : blobs) {
        text += key;
        text += ' ';
        for (const uint8_t b : blob) {
            text += digits[b >> 4];
            text += digits[b & 15];
        }
        text += '\n';
    }
    if (text.size() > kMaxFileBytes) return false;
    std::string temp = path_ + ".tmp.XXXXXX";
    const int fd = ::mkstemp(temp.data());
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, 0600) == 0 && write_all(fd, text.data(), text.size()) && ::fsync(fd) == 0;
    ok = ::close(fd) == 0 && ok;
    ok = ok && ::rename(temp.c_str(), path_.c_str()) == 0;
    if (!ok) {
        ::unlink(temp.c_str());
        return false;
    }
    const int dir = ::open(dirname_of(path_).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return false;
    ok = ::fsync(dir) == 0;
    ::close(dir);
    return ok;
}

bool BlobStore::save(const std::string& key, const uint8_t* data, size_t length) {
    if (!valid_key(key) || length > kMaxBlobBytes || (length > 0 && data == nullptr)) return false;
    Blobs next = blobs_;
    next[key] = std::vector<uint8_t>(data, data + length);
    if (!write(next)) return false;
    blobs_ = std::move(next);
    return true;
}

bool BlobStore::erase(const std::string& key) {
    if (!valid_key(key)) return false;
    if (blobs_.count(key) == 0) return true;
    Blobs next = blobs_;
    next.erase(key);
    if (!write(next)) return false;
    blobs_ = std::move(next);
    return true;
}

}  // namespace libreecho::sendspin
