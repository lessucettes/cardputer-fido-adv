#pragma once
#include <cstddef>
#include "backup_codec.h"

namespace cardputer {
class BackupPassword {
public:
    BackupPassword() = default;
    ~BackupPassword() { clear(); }
    BackupPassword(const BackupPassword&) = delete;
    BackupPassword& operator=(const BackupPassword&) = delete;
    void clear() {
        volatile unsigned char* p = bytes_;
        for (std::size_t i = 0; i < sizeof(bytes_); ++i) { p[i] = 0; }
        length_ = 0;
    }
    void append(const char* text) {
        for (; text && *text; ++text) {
            const auto ch = static_cast<unsigned char>(*text);
            if (ch >= 32 && ch <= 126 && length_ < CF_BACKUP_PASSWORD_MAX) {
                bytes_[length_++] = ch;
            }
        }
    }
    void backspace() {
        if (length_ > 0) { bytes_[--length_] = 0; }
    }
    std::size_t size() const { return length_; }
    const unsigned char* data() const { return bytes_; }
    bool equals(const BackupPassword& other) const {
        unsigned diff = static_cast<unsigned>(length_ ^ other.length_);
        for (std::size_t i = 0; i < sizeof(bytes_); ++i) { diff |= bytes_[i] ^ other.bytes_[i]; }
        return diff == 0;
    }
private:
    unsigned char bytes_[CF_BACKUP_PASSWORD_MAX] = {};
    std::size_t length_ = 0;
};
}  // namespace cardputer
