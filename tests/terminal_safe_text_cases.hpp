#pragma once

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace terminal_safe_text_test_cases {

struct TerminalSafeTextCase {
    std::string_view label;
    std::string input;
    std::string expected;
};

inline std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    result.reserve(values.size());
    for(const unsigned int value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

inline std::vector<TerminalSafeTextCase> cases() {
    std::vector<TerminalSafeTextCase> result = {
        {"ASCII printable", "ASCII printable ~", "ASCII printable ~"},
        {"normal space", " ", " "},
        {"Japanese",
         bytes({0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e}),
         bytes({0xe6, 0x97, 0xa5, 0xe6, 0x9c, 0xac, 0xe8, 0xaa, 0x9e})},
        {"ordinary Unicode", bytes({0xc3, 0xa9}), bytes({0xc3, 0xa9})},
        {"emoji",
         bytes({0xf0, 0x9f, 0x98, 0x80}),
         bytes({0xf0, 0x9f, 0x98, 0x80})},
        {"backslash", "\\x41", "\\x5Cx41"},
        {"NUL", bytes({0x00}), "\\x00"},
        {"C0 SOH", bytes({0x01}), "\\x01"},
        {"TAB", bytes({0x09}), "\\x09"},
        {"LF", bytes({0x0a}), "\\x0A"},
        {"CR", bytes({0x0d}), "\\x0D"},
        {"ESC", bytes({0x1b}), "\\x1B"},
        {"C0 US", bytes({0x1f}), "\\x1F"},
        {"DEL", bytes({0x7f}), "\\x7F"},
        {"C1 U+0080", bytes({0xc2, 0x80}), "\\xC2\\x80"},
        {"C1 U+0085", bytes({0xc2, 0x85}), "\\xC2\\x85"},
        {"C1 U+009F", bytes({0xc2, 0x9f}), "\\xC2\\x9F"},
        {"U+2028", bytes({0xe2, 0x80, 0xa8}), "\\xE2\\x80\\xA8"},
        {"U+2029", bytes({0xe2, 0x80, 0xa9}), "\\xE2\\x80\\xA9"},
        {"U+061C", bytes({0xd8, 0x9c}), "\\xD8\\x9C"},
        {"U+200E", bytes({0xe2, 0x80, 0x8e}), "\\xE2\\x80\\x8E"},
        {"U+200F", bytes({0xe2, 0x80, 0x8f}), "\\xE2\\x80\\x8F"},
        {"U+202A", bytes({0xe2, 0x80, 0xaa}), "\\xE2\\x80\\xAA"},
        {"U+202B", bytes({0xe2, 0x80, 0xab}), "\\xE2\\x80\\xAB"},
        {"U+202C", bytes({0xe2, 0x80, 0xac}), "\\xE2\\x80\\xAC"},
        {"U+202D", bytes({0xe2, 0x80, 0xad}), "\\xE2\\x80\\xAD"},
        {"U+202E", bytes({0xe2, 0x80, 0xae}), "\\xE2\\x80\\xAE"},
        {"U+2066", bytes({0xe2, 0x81, 0xa6}), "\\xE2\\x81\\xA6"},
        {"U+2067", bytes({0xe2, 0x81, 0xa7}), "\\xE2\\x81\\xA7"},
        {"U+2068", bytes({0xe2, 0x81, 0xa8}), "\\xE2\\x81\\xA8"},
        {"U+2069", bytes({0xe2, 0x81, 0xa9}), "\\xE2\\x81\\xA9"},
        {"U+FEFF", bytes({0xef, 0xbb, 0xbf}), "\\xEF\\xBB\\xBF"},
        {"U+FEFF prefix",
         bytes({0xef, 0xbb, 0xbf, 0x41}),
         "\\xEF\\xBB\\xBFA"},
        {"U+FEFF middle",
         bytes({0x41, 0xef, 0xbb, 0xbf, 0x42}),
         "A\\xEF\\xBB\\xBFB"},
        {"U+FEFF suffix",
         bytes({0x41, 0xef, 0xbb, 0xbf}),
         "A\\xEF\\xBB\\xBF"},
        {"lone continuation", bytes({0x80}), "\\x80"},
        {"truncated 2-byte", bytes({0xc2}), "\\xC2"},
        {"truncated 3-byte", bytes({0xe2, 0x82}), "\\xE2\\x82"},
        {"truncated 4-byte",
         bytes({0xf0, 0x9f, 0x92}),
         "\\xF0\\x9F\\x92"},
        {"overlong", bytes({0xc0, 0xaf}), "\\xC0\\xAF"},
        {"UTF-8 surrogate",
         bytes({0xed, 0xa0, 0x80}),
         "\\xED\\xA0\\x80"},
        {"> U+10FFFF",
         bytes({0xf4, 0x90, 0x80, 0x80}),
         "\\xF4\\x90\\x80\\x80"},
        {"invalid leading byte",
         bytes({0xf5, 0x80, 0x80, 0x80}),
         "\\xF5\\x80\\x80\\x80"},
        {"invalid FF", bytes({0xff}), "\\xFF"},
        {"malformed prefix then ASCII",
         bytes({0xe2, 0x82, 0x41}),
         "\\xE2\\x82A"},
    };

    std::string mixed_input = "A";
    mixed_input += bytes({0xe6, 0x97, 0xa5, 0xff, 0xf0, 0x9f, 0x98, 0x80,
                          0xe2, 0x80, 0xae});
    mixed_input += "Z";
    std::string mixed_expected = "A";
    mixed_expected += bytes({0xe6, 0x97, 0xa5});
    mixed_expected += "\\xFF";
    mixed_expected += bytes({0xf0, 0x9f, 0x98, 0x80});
    mixed_expected += "\\xE2\\x80\\xAEZ";
    result.push_back(TerminalSafeTextCase{
        "valid + invalid + valid", std::move(mixed_input),
        std::move(mixed_expected)});
    return result;
}

} // namespace terminal_safe_text_test_cases
