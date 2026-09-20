#pragma once

#include <iostream>
#include <string_view>
#include <unistd.h>

// Small, stateless primitives shared with the AUR -Ss palette. Callers own
// layout and output policy; these helpers never inspect package metadata.
namespace package_text_style {

inline bool enabled_for(const std::ostream& output) {
    return &output == &std::cout && isatty(STDOUT_FILENO) != 0;
}

inline void identity(
    std::ostream& output, std::string_view source,
    std::string_view package, bool styled) {
    output << (styled ? "\033[1;35m" : "") << source
           << (styled ? "\033[0m/\033[1m" : "/") << package
           << (styled ? "\033[0m" : "");
}

inline void version(
    std::ostream& output, std::string_view value, bool styled) {
    output << (styled ? "\033[1;32m" : "") << value
           << (styled ? "\033[0m" : "");
}

inline void installed(
    std::ostream& output, std::string_view label, bool styled) {
    output << (styled ? "\033[1;36m" : "") << label
           << (styled ? "\033[0m" : "");
}

} // namespace package_text_style
