#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace terminal_safe_text {
namespace detail {

inline bool is_utf8_continuation_byte(unsigned char byte) noexcept {
    return byte >= 0x80 && byte <= 0xbf;
}

inline bool decode_utf8_code_point(
    std::string_view value,
    std::size_t offset,
    char32_t& code_point,
    std::size_t& length) noexcept {
    const auto byte_at = [&value](std::size_t index) {
        return static_cast<unsigned char>(value[index]);
    };

    const unsigned char first = byte_at(offset);
    if(first <= 0x7f) {
        code_point = first;
        length = 1;
        return true;
    }
    if(first >= 0xc2 && first <= 0xdf) {
        if(offset + 1 >= value.size()) return false;
        const unsigned char second = byte_at(offset + 1);
        if(!is_utf8_continuation_byte(second)) return false;
        code_point =
            (static_cast<char32_t>(first & 0x1f) << 6) |
            static_cast<char32_t>(second & 0x3f);
        length = 2;
        return true;
    }
    if(first >= 0xe0 && first <= 0xef) {
        if(offset + 2 >= value.size()) return false;
        const unsigned char second = byte_at(offset + 1);
        const unsigned char third = byte_at(offset + 2);
        const bool valid_second =
            first == 0xe0 ? second >= 0xa0 && second <= 0xbf
            : first == 0xed
                ? second >= 0x80 && second <= 0x9f
                : is_utf8_continuation_byte(second);
        if(!valid_second || !is_utf8_continuation_byte(third)) return false;
        code_point =
            (static_cast<char32_t>(first & 0x0f) << 12) |
            (static_cast<char32_t>(second & 0x3f) << 6) |
            static_cast<char32_t>(third & 0x3f);
        length = 3;
        return true;
    }
    if(first >= 0xf0 && first <= 0xf4) {
        if(offset + 3 >= value.size()) return false;
        const unsigned char second = byte_at(offset + 1);
        const unsigned char third = byte_at(offset + 2);
        const unsigned char fourth = byte_at(offset + 3);
        const bool valid_second =
            first == 0xf0 ? second >= 0x90 && second <= 0xbf
            : first == 0xf4
                ? second >= 0x80 && second <= 0x8f
                : is_utf8_continuation_byte(second);
        if(!valid_second || !is_utf8_continuation_byte(third) ||
           !is_utf8_continuation_byte(fourth)) {
            return false;
        }
        code_point =
            (static_cast<char32_t>(first & 0x07) << 18) |
            (static_cast<char32_t>(second & 0x3f) << 12) |
            (static_cast<char32_t>(third & 0x3f) << 6) |
            static_cast<char32_t>(fourth & 0x3f);
        length = 4;
        return true;
    }
    return false;
}

inline bool is_bidi_control(char32_t code_point) noexcept {
    return code_point == 0x061c || code_point == 0x200e ||
           code_point == 0x200f ||
           (code_point >= 0x202a && code_point <= 0x202e) ||
           (code_point >= 0x2066 && code_point <= 0x2069);
}

inline bool is_terminal_safe_code_point(char32_t code_point) noexcept {
    return code_point >= 0x20 &&
           !(code_point >= 0x7f && code_point <= 0x9f) &&
           code_point != static_cast<char32_t>('\\') &&
           code_point != 0x2028 && code_point != 0x2029 &&
           code_point != 0xfeff && !is_bidi_control(code_point);
}

} // namespace detail

template <typename Sink>
[[nodiscard]] bool append_escaped_byte(
    unsigned char byte,
    Sink&& sink) {
    static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
    const char escaped[4]{
        '\\', 'x', HEX_DIGITS[(byte >> 4) & 0x0f],
        HEX_DIGITS[byte & 0x0f]};
    return sink(std::string_view(escaped, sizeof(escaped)));
}

// The sink owns allocation and size policy. Returning false stops emission
// before any later byte is observed by that sink.
template <typename Sink>
[[nodiscard]] bool append_escaped_utf8(
    std::string_view value,
    Sink&& sink) {
    std::size_t offset = 0;
    while(offset < value.size()) {
        char32_t code_point = 0;
        std::size_t length = 0;
        if(!detail::decode_utf8_code_point(
               value, offset, code_point, length)) {
            if(!append_escaped_byte(
                   static_cast<unsigned char>(value[offset]), sink)) {
                return false;
            }
            ++offset;
            continue;
        }
        if(detail::is_terminal_safe_code_point(code_point)) {
            if(!sink(value.substr(offset, length))) return false;
        } else {
            for(std::size_t index = 0; index < length; ++index) {
                if(!append_escaped_byte(
                       static_cast<unsigned char>(value[offset + index]),
                       sink)) {
                    return false;
                }
            }
        }
        offset += length;
    }
    return true;
}

inline std::string escape_utf8(std::string_view value) {
    std::string display;
    display.reserve(value.size());
    const bool completed = append_escaped_utf8(
        value, [&display](std::string_view segment) {
            display.append(segment);
            return true;
        });
    (void)completed;
    return display;
}

} // namespace terminal_safe_text
