#include "user_config.hpp"

#include "localization.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

namespace {

namespace fs = std::filesystem;

constexpr std::int64_t SUPPORTED_SCHEMA_VERSION = 1;

std::string node_type_name(const toml::node& node) {
    std::ostringstream type;
    type << node.type();
    return type.str();
}

[[noreturn]] void throw_config_path_type_error(
    const fs::path& config_path, fs::file_type type) {
    const std::string path = config_path.string();
    switch(type) {
        case fs::file_type::not_found:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got missing target",
                path));
        case fs::file_type::directory:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got directory",
                path));
        case fs::file_type::symlink:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got symbolic link",
                path));
        case fs::file_type::block:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got block device",
                path));
        case fs::file_type::character:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got character device",
                path));
        case fs::file_type::fifo:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got {}",
                path, "FIFO"));
        case fs::file_type::socket:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got socket",
                path));
        case fs::file_type::regular:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got regular file",
                path));
        case fs::file_type::none:
        case fs::file_type::unknown:
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': config path must resolve to a readable regular file; got unknown file type",
                path));
    }
    throw UserConfigError(localization::format_translated_message(
        "User config error: '{}': config path must resolve to a readable regular file; got unknown file type",
        path));
}

std::string source_index_text(const toml::source_index& index) {
    std::ostringstream text;
    text << index;
    return text.str();
}

bool require_regular_config_file(const fs::path& config_path) {
    std::error_code error;
    const fs::file_status entry_status = fs::symlink_status(config_path, error);
    if(error == std::errc::no_such_file_or_directory) return false;
    if(error) {
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': unable to inspect config path: {}",
            config_path.string(), error.message()));
    }
    if(entry_status.type() == fs::file_type::not_found) return false;

    const fs::file_status target_status = fs::status(config_path, error);
    if(error) {
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': unable to inspect config target: {}",
            config_path.string(), error.message()));
    }
    if(!fs::is_regular_file(target_status)) {
        throw_config_path_type_error(config_path, target_status.type());
    }
    return true;
}

std::string read_config_file(const fs::path& config_path) {
    std::ifstream file(config_path, std::ios::binary);
    if(!file.is_open()) {
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': unable to open config file for reading",
            config_path.string()));
    }

    std::string contents;
    std::array<char, 16'384> buffer{};
    while(file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if(count > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(count));
        }
    }

    if(file.bad() || (file.fail() && !file.eof())) {
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': unable to read config file",
            config_path.string()));
    }
    return contents;
}

toml::table parse_config_file(const fs::path& config_path) {
    const std::string contents = read_config_file(config_path);
    try {
        return toml::parse(contents, config_path.string());
    } catch(const toml::parse_error& error) {
        const toml::source_region& source = error.source();
        if(source.begin) {
            // TRANSLATORS: The placeholders are a path, the literal TOML
            // identity, source line and column, and the parser's diagnostic.
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': {} parse error (line {}, column {}): {}",
                config_path.string(), "TOML",
                source_index_text(source.begin.line),
                source_index_text(source.begin.column),
                std::string(error.description())));
        }
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': {} parse error: {}",
            config_path.string(), "TOML",
            std::string(error.description())));
    }
}

void validate_top_level_keys(
    const toml::table& root, const fs::path& config_path) {
    for(const auto& [key, node] : root) {
        const std::string_view name = key.str();
        if(name == "schema_version" || name == "review" || name == "build") {
            continue;
        }

        if(node.is_table()) {
            // TRANSLATORS: The placeholders are a config path, a TOML section
            // name, source line/column, and literal accepted TOML keys.
            throw UserConfigError(localization::format_translated_message(
                "User config error: '{}': top-level section '{}' (line {}, column {}): unknown top-level section; accepted entries: {}, {}, {}",
                config_path.string(), name,
                source_index_text(key.source().begin.line),
                source_index_text(key.source().begin.column),
                "schema_version", "review", "build"));
        }
        // TRANSLATORS: The placeholders are a config path, a TOML key, source
        // line/column, and literal accepted TOML keys.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': top-level key '{}' (line {}, column {}): unknown top-level key; accepted entries: {}, {}, {}",
            config_path.string(), name,
            source_index_text(key.source().begin.line),
            source_index_text(key.source().begin.column),
            "schema_version", "review", "build"));
    }
}

std::int64_t parse_schema_version(
    const toml::table& root, const fs::path& config_path) {
    const toml::node* version_node = root.get("schema_version");
    if(version_node == nullptr) {
        // TRANSLATORS: schema_version is a literal TOML key.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': key '{}': missing required key; expected integer {}",
            config_path.string(), "schema_version",
            SUPPORTED_SCHEMA_VERSION));
    }

    const auto version = version_node->value_exact<std::int64_t>();
    if(!version) {
        // TRANSLATORS: schema_version is a literal TOML key; the remaining
        // placeholders are the supported integer, actual TOML type, and source
        // line/column.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': key '{}' (line {}, column {}): expected integer {}; got {}",
            config_path.string(), "schema_version",
            source_index_text(version_node->source().begin.line),
            source_index_text(version_node->source().begin.column),
            SUPPORTED_SCHEMA_VERSION, node_type_name(*version_node)));
    }
    if(*version != SUPPORTED_SCHEMA_VERSION) {
        // TRANSLATORS: schema_version is a literal TOML key; the remaining
        // placeholders are the actual/supported versions and source location.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': key '{}' (line {}, column {}): unsupported schema version {}; expected integer {}",
            config_path.string(), "schema_version",
            source_index_text(version_node->source().begin.line),
            source_index_text(version_node->source().begin.column),
            *version, SUPPORTED_SCHEMA_VERSION));
    }
    return *version;
}

const toml::table* optional_section(
    const toml::table& root, const fs::path& config_path,
    std::string_view section_name) {
    const toml::node* section_node = root.get(section_name);
    if(section_node == nullptr) return nullptr;

    const toml::table* section = section_node->as_table();
    if(section == nullptr) {
        // TRANSLATORS: The placeholders are a TOML section name, config path,
        // actual TOML type, and source location.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': section '{}' (line {}, column {}): expected table; got {}",
            config_path.string(), section_name,
            source_index_text(section_node->source().begin.line),
            source_index_text(section_node->source().begin.column),
            node_type_name(*section_node)));
    }
    return section;
}

void validate_review_keys(
    const toml::table& review, const fs::path& config_path) {
    for(const auto& [key, node] : review) {
        static_cast<void>(node);
        const std::string_view name = key.str();
        if(name == "pkgbuild" || name == "diff") continue;

        // TRANSLATORS: review, pkgbuild, and diff are literal TOML keys; the
        // other placeholders are a config path, unknown key, and source
        // location.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': section '{}', key '{}' (line {}, column {}): unknown key; accepted keys: {}, {}",
            config_path.string(), "review", name,
            source_index_text(key.source().begin.line),
            source_index_text(key.source().begin.column), "pkgbuild",
            "diff"));
    }
}

void validate_build_keys(
    const toml::table& build, const fs::path& config_path) {
    for(const auto& [key, node] : build) {
        static_cast<void>(node);
        const std::string_view name = key.str();
        if(name == "mode") continue;

        // TRANSLATORS: build and mode are literal TOML keys; the other
        // placeholders are a config path, unknown key, and source location.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': section '{}', key '{}' (line {}, column {}): unknown key; accepted key: {}",
            config_path.string(), "build", name,
            source_index_text(key.source().begin.line),
            source_index_text(key.source().begin.column), "mode"));
    }
}

ReviewPolicy parse_review_policy(
    const toml::node& node, const fs::path& config_path,
    std::string_view key_name) {
    const auto value = node.value_exact<std::string>();
    if(!value) {
        // TRANSLATORS: The placeholders are a literal TOML key, config path,
        // literal accepted values, actual TOML type, and source location.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': key '{}' (line {}, column {}): expected string; accepted values: {}, {}; got {}",
            config_path.string(), key_name,
            source_index_text(node.source().begin.line),
            source_index_text(node.source().begin.column),
            "prompt", "skip", node_type_name(node)));
    }
    if(*value == "prompt") return ReviewPolicy::Prompt;
    if(*value == "skip") return ReviewPolicy::Skip;

    // TRANSLATORS: The placeholders are a literal TOML key, config path,
    // actual value, literal accepted values, and source location.
    throw UserConfigError(localization::format_translated_message(
        "User config error: '{}': key '{}' (line {}, column {}): unsupported value '{}'; accepted values: {}, {}",
        config_path.string(), key_name,
        source_index_text(node.source().begin.line),
        source_index_text(node.source().begin.column),
        *value, "prompt", "skip"));
}

BuildMode parse_build_mode(
    const toml::node& node, const fs::path& config_path) {
    const auto value = node.value_exact<std::string>();
    if(!value) {
        // TRANSLATORS: The placeholders are the literal TOML key, config path,
        // literal accepted values, actual TOML type, and source location.
        throw UserConfigError(localization::format_translated_message(
            "User config error: '{}': key '{}' (line {}, column {}): expected string; accepted values: {}, {}, {}; got {}",
            config_path.string(), "build.mode",
            source_index_text(node.source().begin.line),
            source_index_text(node.source().begin.column),
            "normal", "rebuild", "clean", node_type_name(node)));
    }
    if(*value == "normal") return BuildMode::Normal;
    if(*value == "rebuild") return BuildMode::Rebuild;
    if(*value == "clean") return BuildMode::Clean;

    // TRANSLATORS: The placeholders are the literal TOML key, config path,
    // actual value, literal accepted values, and source location.
    throw UserConfigError(localization::format_translated_message(
        "User config error: '{}': key '{}' (line {}, column {}): unsupported value '{}'; accepted values: {}, {}, {}",
        config_path.string(), "build.mode",
        source_index_text(node.source().begin.line),
        source_index_text(node.source().begin.column), *value, "normal",
        "rebuild", "clean"));
}

void apply_review_config(
    UserConfig& config, const toml::table& root,
    const fs::path& config_path) {
    const toml::table* review = optional_section(root, config_path, "review");
    if(review == nullptr) return;

    validate_review_keys(*review, config_path);
    if(const toml::node* pkgbuild = review->get("pkgbuild")) {
        config.review.pkgbuild =
            parse_review_policy(*pkgbuild, config_path, "review.pkgbuild");
    }
    if(const toml::node* diff = review->get("diff")) {
        config.review.diff =
            parse_review_policy(*diff, config_path, "review.diff");
    }
}

void apply_build_config(
    UserConfig& config, const toml::table& root,
    const fs::path& config_path) {
    const toml::table* build = optional_section(root, config_path, "build");
    if(build == nullptr) return;

    validate_build_keys(*build, config_path);
    if(const toml::node* mode = build->get("mode")) {
        config.build.mode = parse_build_mode(*mode, config_path);
    }
}

} // namespace

UserConfig load_user_config(const std::filesystem::path& config_path) {
    if(!require_regular_config_file(config_path)) return UserConfig{};

    const toml::table root = parse_config_file(config_path);
    validate_top_level_keys(root, config_path);

    UserConfig config;
    config.schema_version = parse_schema_version(root, config_path);
    apply_review_config(config, root, config_path);
    apply_build_config(config, root, config_path);
    return config;
}
