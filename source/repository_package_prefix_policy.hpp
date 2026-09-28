#pragma once

#include <chrono>
#include <cstddef>

// Private transport budget, shared by supervisor and fixed local worker.
// These are resource bounds, not a public CLI/configuration interface.
namespace repository_package_prefix {
inline constexpr std::size_t CANDIDATE_LIMIT = 256;
inline constexpr std::size_t OUTPUT_BYTE_LIMIT = 64 * 1024;
inline constexpr std::size_t CONFIGURATION_CAPTURE_LIMIT = 16 * 1024;
inline constexpr std::size_t PREFIX_BYTE_LIMIT = 256;
inline constexpr auto QUERY_TIMEOUT = std::chrono::milliseconds{500};
inline constexpr auto TERMINATION_GRACE = std::chrono::milliseconds{50};
} // namespace repository_package_prefix
