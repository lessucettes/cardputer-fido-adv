#include "text.h"
#include <cstring>

namespace cardputer {
namespace {

constexpr std::uint32_t kReplacement = 0xFFFDu;
constexpr std::uint32_t kEllipsis = '.';  // ASCII, available in every font

bool is_bidi_control(std::uint32_t cp) {
    return (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) ||
           cp == 0x200E || cp == 0x200F || cp == 0x061C;
}

bool is_control(std::uint32_t cp) {
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
}

// Decodes one UTF-8 scalar. Returns 0 on malformed input and advances by one
// byte so the caller can substitute U+FFFD and continue.
std::uint32_t decode_one(const std::uint8_t* data, std::size_t len,
                         std::size_t* used) {
    if (len == 0) {
        *used = 0;
        return 0;
    }
    const std::uint8_t first = data[0];
    if (first < 0x80) {
        *used = 1;
        return first;
    }
    std::size_t need = 0;
    std::uint32_t cp = 0;
    if ((first & 0xE0) == 0xC0) {
        need = 1;
        cp = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        need = 2;
        cp = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        need = 3;
        cp = first & 0x07;
    } else {
        *used = 1;
        return 0;
    }
    if (len < need + 1) {
        *used = 1;
        return 0;
    }
    for (std::size_t i = 1; i <= need; ++i) {
        if ((data[i] & 0xC0) != 0x80) {
            *used = 1;
            return 0;
        }
        cp = (cp << 6) | (data[i] & 0x3F);
    }
    if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
        (need == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *used = 1;
        return 0;
    }
    *used = need + 1;
    return cp;
}

std::size_t encode_one(std::uint32_t cp, char* out, std::size_t room) {
    if (room == 0) {
        return 0;
    }
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        if (room < 2) {
            return 0;
        }
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (room < 3) {
            return 0;
        }
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    if (room < 4) {
        return 0;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

}  // namespace

bool valid_utf8(const std::uint8_t* data, std::size_t len) {
    std::size_t offset = 0;
    while (offset < len) {
        std::size_t used = 0;
        const std::uint32_t cp = decode_one(data + offset, len - offset, &used);
        if (cp == 0 && !(used == 1 && data[offset] == 0)) {
            return false;
        }
        offset += used;
    }
    return true;
}

std::size_t sanitize_text(const char* input, std::size_t input_len, char* out,
                          std::size_t cap, bool* truncated) {
    if (truncated != nullptr) {
        *truncated = false;
    }
    if (out == nullptr || cap == 0) {
        return 0;
    }
    if (input == nullptr) {
        out[0] = '\0';
        return 0;
    }
    std::size_t written = 0;
    std::size_t offset = 0;
    bool cut = false;
    while (offset < input_len && input[offset] != '\0') {
        std::size_t used = 0;
        std::uint32_t cp = decode_one(reinterpret_cast<const std::uint8_t*>(input) + offset,
                                      input_len - offset, &used);
        offset += used == 0 ? 1 : used;
        if (cp == 0) {
            cp = kReplacement;
        }
        if (is_bidi_control(cp)) {
            continue;
        }
        if (is_control(cp)) {
            cp = ' ';
        }
        char encoded[4];
        const std::size_t encoded_len = encode_one(cp, encoded, sizeof(encoded));
        if (encoded_len == 0) {
            cp = kReplacement;
        }
        const std::size_t length = encoded_len == 0
                                       ? encode_one(cp, encoded, sizeof(encoded))
                                       : encoded_len;
        if (length == 0) {
            break;
        }
        if (written + length + 1 > cap) {
            cut = true;
            break;
        }
        std::memcpy(out + written, encoded, length);
        written += length;
    }
    if (cut && cap >= 4) {
        // Reserve three bytes plus the terminator for a visible marker.
        while (written > 0 && written + 4 > cap) {
            --written;
            while (written > 0 && (static_cast<unsigned char>(out[written]) & 0xC0) == 0x80) {
                --written;
            }
        }
        out[written++] = static_cast<char>(kEllipsis);
        out[written++] = static_cast<char>(kEllipsis);
        out[written++] = static_cast<char>(kEllipsis);
        if (truncated != nullptr) {
            *truncated = true;
        }
    } else if (cut && truncated != nullptr) {
        *truncated = true;
    }
    out[written] = '\0';
    return written;
}

std::size_t shorten_middle(const char* input, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    if (input == nullptr) {
        out[0] = '\0';
        return 0;
    }
    const std::size_t len = std::strlen(input);
    if (len + 1 <= cap) {
        std::memcpy(out, input, len + 1);
        return len;
    }
    if (cap < 8) {
        return sanitize_text(input, len, out, cap);
    }
    const std::size_t keep = (cap - 1 - 3) / 2;
    const std::size_t tail = cap - 1 - 3 - keep;
    std::size_t written = 0;
    std::memcpy(out + written, input, keep);
    written += keep;
    out[written++] = '.';
    out[written++] = '.';
    out[written++] = '.';
    std::memcpy(out + written, input + len - tail, tail);
    written += tail;
    out[written] = '\0';
    return written;
}

}  // namespace cardputer
