#pragma once
#include <cstddef>
#include <cstdint>

namespace cardputer {

constexpr std::size_t kRpIdCapacity = 256;
constexpr std::size_t kAccountCapacity = 65;

// Copies at most cap-1 bytes of display-safe UTF-8 and always terminates.
// Control characters and bidi formatting are replaced or removed; invalid
// input bytes become U+FFFD. Embedded NUL stops the copy. When the result is
// shortened, the last three output bytes are "...". Returns the output length.
std::size_t sanitize_text(const char* input, std::size_t input_len, char* out,
                          std::size_t cap, bool* truncated = nullptr);

// Keeps the beginning and the end of a long label so a deceptive suffix is
// still visible in a compact row. The full sanitized value remains available
// to the caller for a detail view.
std::size_t shorten_middle(const char* input, char* out, std::size_t cap);

bool valid_utf8(const std::uint8_t* data, std::size_t len);

}  // namespace cardputer
