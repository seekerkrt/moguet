#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>

enum class ReviewPolicy {
    Prompt,
    Skip,
};

enum class BuildMode {
    Normal,
    Rebuild,
    Clean,
};

struct ReviewConfig {
    ReviewPolicy pkgbuild = ReviewPolicy::Prompt;
    ReviewPolicy diff = ReviewPolicy::Prompt;
};

struct BuildConfig {
    BuildMode mode = BuildMode::Normal;
};

struct UserConfig {
    std::int64_t schema_version = 1;
    ReviewConfig review;
    BuildConfig build;
};

class UserConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Missing fileだけはbuilt-in defaultを返す。既存entryのIO/schema failureは例外にする。
UserConfig load_user_config(const std::filesystem::path& config_path);
