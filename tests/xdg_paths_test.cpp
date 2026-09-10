#include "application_identity.hpp"
#include "xdg_paths.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void expect_path(
    const fs::path& actual,
    const fs::path& expected,
    const std::string& context) {
    if(actual != expected) {
        throw std::runtime_error(
            context + ": expected [" + expected.string() +
            "], actual [" + actual.string() + "]");
    }
}

void expect_creation_boundary(
    const xdg_paths::DirectoryCreationBoundary& boundary,
    xdg_paths::DirectorySource expected_source,
    const fs::path& expected_base_directory,
    const fs::path& expected_existing_anchor,
    const std::vector<std::string>& expected_creatable_components,
    const std::string& context) {
    expect(
        boundary.source == expected_source,
        context + ": unexpected directory source.");
    expect_path(
        boundary.base_directory, expected_base_directory,
        context + " base directory");
    expect_path(
        boundary.existing_anchor, expected_existing_anchor,
        context + " existing anchor");
    expect(
        boundary.creatable_components == expected_creatable_components,
        context + ": unexpected creatable components.");
}

template <typename Callable>
void expect_resolution_error(
    Callable callable,
    xdg_paths::DirectoryKind expected_directory_kind,
    xdg_paths::EnvironmentVariable expected_environment_variable,
    xdg_paths::ResolutionErrorCode expected_code,
    const std::string& expected_diagnostic_fragment,
    const std::string& forbidden_diagnostic_fragment = "") {
    try {
        callable();
    } catch(const xdg_paths::ResolutionError& error) {
        const xdg_paths::ResolutionFailure& failure = error.failure();
        expect(
            failure.directory_kind == expected_directory_kind,
            "Unexpected XDG error directory kind.");
        expect(
            failure.environment_variable == expected_environment_variable,
            "Unexpected XDG error environment variable.");
        expect(
            failure.code == expected_code,
            "Unexpected XDG error code.");

        const std::string diagnostic = error.what();
        expect(
            diagnostic.find(expected_diagnostic_fragment) !=
                std::string::npos,
            "XDG diagnostic is missing the expected classification.");
        if(!forbidden_diagnostic_fragment.empty()) {
            expect(
                diagnostic.find(forbidden_diagnostic_fragment) ==
                    std::string::npos,
                "XDG diagnostic exposed the raw environment value.");
        }
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Unexpected XDG exception category: " +
            std::string(error.what()));
    }

    throw std::runtime_error("Expected XDG path resolution failure.");
}

xdg_paths::EnvironmentSnapshot explicit_environment() {
    return xdg_paths::EnvironmentSnapshot{
        .xdg_config_home = "/xdg/config-base",
        .xdg_state_home = "/xdg/state-base",
        .xdg_cache_home = "/xdg/cache-base",
        .home = std::nullopt,
    };
}

void expect_explicit_paths(const xdg_paths::ResolvedPaths& paths) {
    expect_path(
        paths.config.directory, "/xdg/config-base/moguet",
        "Explicit config directory");
    expect_path(
        paths.state.directory, "/xdg/state-base/moguet",
        "Explicit state directory");
    expect_path(
        paths.cache.directory, "/xdg/cache-base/moguet",
        "Explicit cache directory");
}

void test_explicit_xdg_values() {
    const xdg_paths::ResolvedPaths paths =
        xdg_paths::resolve(explicit_environment());
    expect_explicit_paths(paths);
    expect_creation_boundary(
        paths.config.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/xdg/config-base", "/xdg/config-base", {"moguet"},
        "Explicit config creation boundary");
    expect_creation_boundary(
        paths.state.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/xdg/state-base", "/xdg/state-base", {"moguet"},
        "Explicit state creation boundary");
    expect_creation_boundary(
        paths.cache.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/xdg/cache-base", "/xdg/cache-base", {"moguet"},
        "Explicit cache creation boundary");
}

void test_unset_xdg_values_use_home_fallback() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = std::nullopt,
        .xdg_state_home = std::nullopt,
        .xdg_cache_home = std::nullopt,
        .home = "/home/test-user",
    };
    const xdg_paths::ResolvedPaths paths = xdg_paths::resolve(environment);

    expect_path(
        paths.config.directory, "/home/test-user/.config/moguet",
        "Unset XDG config fallback");
    expect_path(
        paths.state.directory, "/home/test-user/.local/state/moguet",
        "Unset XDG state fallback");
    expect_path(
        paths.cache.directory, "/home/test-user/.cache/moguet",
        "Unset XDG cache fallback");
    expect_creation_boundary(
        paths.config.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/home/test-user/.config", "/home/test-user",
        {".config", "moguet"},
        "HOME config creation boundary");
    expect_creation_boundary(
        paths.state.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/home/test-user/.local/state", "/home/test-user",
        {".local", "state", "moguet"},
        "HOME state creation boundary");
    expect_creation_boundary(
        paths.cache.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/home/test-user/.cache", "/home/test-user",
        {".cache", "moguet"},
        "HOME cache creation boundary");
}

void test_empty_xdg_values_use_home_fallback() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "",
        .xdg_state_home = "",
        .xdg_cache_home = "",
        .home = "/home/empty-xdg-user",
    };
    const xdg_paths::ResolvedPaths paths = xdg_paths::resolve(environment);

    expect_path(
        paths.config.directory,
        "/home/empty-xdg-user/.config/moguet",
        "Empty XDG config fallback");
    expect_path(
        paths.state.directory,
        "/home/empty-xdg-user/.local/state/moguet",
        "Empty XDG state fallback");
    expect_path(
        paths.cache.directory,
        "/home/empty-xdg-user/.cache/moguet",
        "Empty XDG cache fallback");
}

void test_mixed_xdg_and_home_fallbacks() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "/mixed/config",
        .xdg_state_home = std::nullopt,
        .xdg_cache_home = "",
        .home = "/mixed/home",
    };
    const xdg_paths::ResolvedPaths paths = xdg_paths::resolve(environment);

    expect_path(
        paths.config.directory, "/mixed/config/moguet",
        "Mixed explicit config directory");
    expect_path(
        paths.state.directory, "/mixed/home/.local/state/moguet",
        "Mixed state fallback");
    expect_path(
        paths.cache.directory, "/mixed/home/.cache/moguet",
        "Mixed cache fallback");
}

void test_application_component_and_derived_files() {
    const xdg_paths::ResolvedPaths paths =
        xdg_paths::resolve(explicit_environment());
    const fs::path application_component(
        std::string(application_identity::XDG_IDENTITY));

    expect(
        application_identity::XDG_IDENTITY == "moguet",
        "Unexpected application XDG identity.");
    expect_path(
        paths.config.directory.filename(), application_component,
        "Config application component");
    expect_path(
        paths.state.directory.filename(), application_component,
        "State application component");
    expect_path(
        paths.cache.directory.filename(), application_component,
        "Cache application component");
    expect_path(
        paths.config.config_file.parent_path(), paths.config.directory,
        "Config file parent");
    expect_path(
        paths.config.config_file.filename(), "config.toml",
        "Config filename");
    expect_path(
        paths.state.default_log_file.parent_path(), paths.state.directory,
        "Default log parent");
    expect_path(
        paths.state.default_log_file.filename(), "moguet.log",
        "Default log filename");
}

void test_relative_xdg_values_are_rejected_without_fallback() {
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_config_home = "relative/config-secret";
        environment.home = "/valid/fallback";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::Config,
            xdg_paths::EnvironmentVariable::XdgConfigHome,
            xdg_paths::ResolutionErrorCode::RelativePath,
            "XDG_CONFIG_HOME must be an absolute path",
            "relative/config-secret");
    }
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_state_home = "relative/state-secret";
        environment.home = "/valid/fallback";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::State,
            xdg_paths::EnvironmentVariable::XdgStateHome,
            xdg_paths::ResolutionErrorCode::RelativePath,
            "XDG_STATE_HOME must be an absolute path",
            "relative/state-secret");
    }
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_cache_home = "relative/cache-secret";
        environment.home = "/valid/fallback";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::Cache,
            xdg_paths::EnvironmentVariable::XdgCacheHome,
            xdg_paths::ResolutionErrorCode::RelativePath,
            "XDG_CACHE_HOME must be an absolute path",
            "relative/cache-secret");
    }
}

void test_home_fallback_failures_identify_the_base() {
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_config_home = std::nullopt;
        environment.home = std::nullopt;
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::Config,
            xdg_paths::EnvironmentVariable::Home,
            xdg_paths::ResolutionErrorCode::MissingHome,
            "XDG_CONFIG_HOME is unset or empty, and HOME is not set");
    }
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_state_home = "";
        environment.home = "";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::State,
            xdg_paths::EnvironmentVariable::Home,
            xdg_paths::ResolutionErrorCode::EmptyHome,
            "XDG_STATE_HOME is unset or empty, and HOME is empty");
    }
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_cache_home = std::nullopt;
        environment.home = "relative/home-secret";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::Cache,
            xdg_paths::EnvironmentVariable::Home,
            xdg_paths::ResolutionErrorCode::RelativePath,
            "HOME must be an absolute path",
            "relative/home-secret");
    }
}

void test_invalid_home_is_ignored_without_fallback() {
    xdg_paths::EnvironmentSnapshot environment = explicit_environment();
    environment.home = "relative/home-is-unused";
    expect_explicit_paths(xdg_paths::resolve(environment));
}

void test_redundant_separators_are_normalized() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "/normal//config///base/",
        .xdg_state_home = "///normal///state//base",
        .xdg_cache_home = "",
        .home = "/normal//home///user/",
    };
    const xdg_paths::ResolvedPaths paths = xdg_paths::resolve(environment);

    expect_path(
        paths.config.directory, "/normal/config/base/moguet",
        "Normalized config directory");
    expect_path(
        paths.state.directory, "/normal/state/base/moguet",
        "Normalized state directory");
    expect_path(
        paths.cache.directory, "/normal/home/user/.cache/moguet",
        "Normalized HOME fallback");
    expect_creation_boundary(
        paths.config.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/normal/config/base", "/normal/config/base", {"moguet"},
        "Normalized explicit config creation boundary");
    expect_creation_boundary(
        paths.state.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/normal/state/base", "/normal/state/base", {"moguet"},
        "Normalized explicit state creation boundary");
    expect_creation_boundary(
        paths.cache.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/normal/home/user/.cache", "/normal/home/user",
        {".cache", "moguet"},
        "Normalized HOME cache creation boundary");
}

void test_dot_components_are_rejected_before_normalization() {
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_config_home = "/unsafe/./config";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::Config,
            xdg_paths::EnvironmentVariable::XdgConfigHome,
            xdg_paths::ResolutionErrorCode::DotComponent,
            "XDG_CONFIG_HOME contains a '.' or '..' path component");
    }
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_state_home = "/unsafe/state/../other";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::State,
            xdg_paths::EnvironmentVariable::XdgStateHome,
            xdg_paths::ResolutionErrorCode::DotComponent,
            "XDG_STATE_HOME contains a '.' or '..' path component");
    }
    {
        xdg_paths::EnvironmentSnapshot environment = explicit_environment();
        environment.xdg_cache_home = std::nullopt;
        environment.home = "/unsafe/home/../other";
        expect_resolution_error(
            [&environment]() {
                static_cast<void>(xdg_paths::resolve(environment));
            },
            xdg_paths::DirectoryKind::Cache,
            xdg_paths::EnvironmentVariable::Home,
            xdg_paths::ResolutionErrorCode::DotComponent,
            "HOME contains a '.' or '..' path component");
    }
}

void test_ambiguous_double_leading_slash_is_rejected() {
    xdg_paths::EnvironmentSnapshot environment = explicit_environment();
    environment.xdg_config_home = "//implementation-defined/config";
    expect_resolution_error(
        [&environment]() {
            static_cast<void>(xdg_paths::resolve(environment));
        },
        xdg_paths::DirectoryKind::Config,
        xdg_paths::EnvironmentVariable::XdgConfigHome,
        xdg_paths::ResolutionErrorCode::AmbiguousLeadingDoubleSlash,
        "implementation-defined '//' form",
        "//implementation-defined/config");
}

void test_embedded_nul_is_rejected_without_disclosure() {
    xdg_paths::EnvironmentSnapshot environment = explicit_environment();
    environment.xdg_cache_home =
        std::string("/safe-prefix\0secret-suffix", 26);
    expect_resolution_error(
        [&environment]() {
            static_cast<void>(xdg_paths::resolve(environment));
        },
        xdg_paths::DirectoryKind::Cache,
        xdg_paths::EnvironmentVariable::XdgCacheHome,
        xdg_paths::ResolutionErrorCode::EmbeddedNull,
        "XDG_CACHE_HOME contains an embedded NUL byte",
        "safe-prefix");
}

class ScopedEnvironmentVariable final {
    std::string name_;
    std::optional<std::string> previous_value_;

public:
    ScopedEnvironmentVariable(
        std::string name,
        const std::optional<std::string>& value)
        : name_(std::move(name)) {
        const char* previous = std::getenv(name_.c_str());
        if(previous != nullptr) previous_value_ = previous;

        const int result = value.has_value()
                               ? setenv(name_.c_str(), value->c_str(), 1)
                               : unsetenv(name_.c_str());
        if(result != 0) {
            throw std::runtime_error(
                "Failed to set test environment variable: " + name_);
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() noexcept {
        if(previous_value_.has_value())
            static_cast<void>(setenv(
                name_.c_str(), previous_value_->c_str(), 1));
        else
            static_cast<void>(unsetenv(name_.c_str()));
    }
};

class TemporaryDirectory final {
    fs::path path_;

public:
    TemporaryDirectory() {
        const std::string template_text =
            (fs::temp_directory_path() /
             "moguet-xdg-paths-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create XDG paths test directory.");
        }
        path_ = created_path;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }
};

void test_process_adapter_ignores_sudo_user_and_root_inference() {
    TemporaryDirectory temporary_directory;
    const fs::path root_home = temporary_directory.path() / "root-home";

    ScopedEnvironmentVariable config_home("XDG_CONFIG_HOME", std::nullopt);
    ScopedEnvironmentVariable state_home("XDG_STATE_HOME", std::nullopt);
    ScopedEnvironmentVariable cache_home("XDG_CACHE_HOME", std::nullopt);
    ScopedEnvironmentVariable home(
        "HOME", std::optional<std::string>{root_home.string()});
    ScopedEnvironmentVariable user(
        "USER", std::optional<std::string>{"root"});
    ScopedEnvironmentVariable logname(
        "LOGNAME", std::optional<std::string>{"root"});

    xdg_paths::ResolvedPaths first_paths;
    {
        ScopedEnvironmentVariable sudo_user(
            "SUDO_USER", std::optional<std::string>{"ordinary-user"});
        first_paths = xdg_paths::resolve_process_environment();
    }
    xdg_paths::ResolvedPaths second_paths;
    {
        ScopedEnvironmentVariable sudo_user(
            "SUDO_USER", std::optional<std::string>{"different-user"});
        second_paths = xdg_paths::resolve_process_environment();
    }

    expect_path(
        first_paths.config.directory,
        root_home / ".config" / "moguet",
        "Root-like config fallback");
    expect_path(
        first_paths.state.directory,
        root_home / ".local" / "state" / "moguet",
        "Root-like state fallback");
    expect_path(
        first_paths.cache.directory,
        root_home / ".cache" / "moguet",
        "Root-like cache fallback");
    expect_path(
        second_paths.config.directory, first_paths.config.directory,
        "SUDO_USER-independent config directory");
    expect_path(
        second_paths.state.directory, first_paths.state.directory,
        "SUDO_USER-independent state directory");
    expect_path(
        second_paths.cache.directory, first_paths.cache.directory,
        "SUDO_USER-independent cache directory");
}

void test_config_only_resolver_uses_explicit_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "/config-only/base",
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "relative/cache-secret",
        .home = "relative/unused-home",
    };
    const xdg_paths::ConfigPaths paths =
        xdg_paths::resolve_config(environment);

    expect_path(
        paths.directory, "/config-only/base/moguet",
        "Config-only explicit directory");
    expect_path(
        paths.config_file, "/config-only/base/moguet/config.toml",
        "Config-only explicit config file");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/config-only/base", "/config-only/base", {"moguet"},
        "Config-only explicit creation boundary");
}

void test_config_only_resolver_uses_home_fallback() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = std::nullopt,
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "relative/cache-secret",
        .home = "/config-only/home",
    };
    const xdg_paths::ConfigPaths paths =
        xdg_paths::resolve_config(environment);

    expect_path(
        paths.directory, "/config-only/home/.config/moguet",
        "Config-only HOME fallback directory");
    expect_path(
        paths.config_file,
        "/config-only/home/.config/moguet/config.toml",
        "Config-only HOME fallback config file");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/config-only/home/.config", "/config-only/home",
        {".config", "moguet"},
        "Config-only HOME fallback creation boundary");
}

void test_config_only_resolver_treats_empty_xdg_as_unset() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "",
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "relative/cache-secret",
        .home = "/config-only/empty-xdg-home",
    };
    const xdg_paths::ConfigPaths paths =
        xdg_paths::resolve_config(environment);

    expect_path(
        paths.directory,
        "/config-only/empty-xdg-home/.config/moguet",
        "Config-only empty XDG fallback directory");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/config-only/empty-xdg-home/.config",
        "/config-only/empty-xdg-home", {".config", "moguet"},
        "Config-only empty XDG fallback creation boundary");
}

void test_config_only_resolver_rejects_relative_xdg_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = std::nullopt,
        .xdg_cache_home = std::nullopt,
        .home = "/valid/fallback",
    };
    expect_resolution_error(
        [&environment]() {
            static_cast<void>(xdg_paths::resolve_config(environment));
        },
        xdg_paths::DirectoryKind::Config,
        xdg_paths::EnvironmentVariable::XdgConfigHome,
        xdg_paths::ResolutionErrorCode::RelativePath,
        "XDG_CONFIG_HOME must be an absolute path",
        "relative/config-secret");
}

void test_config_process_adapter_reads_only_config_authority() {
    TemporaryDirectory temporary_directory;
    const fs::path config_home = temporary_directory.path() / "config-home";

    ScopedEnvironmentVariable config_environment(
        "XDG_CONFIG_HOME",
        std::optional<std::string>{config_home.string()});
    ScopedEnvironmentVariable state_home(
        "XDG_STATE_HOME",
        std::optional<std::string>{"relative/state-secret"});
    ScopedEnvironmentVariable cache_home(
        "XDG_CACHE_HOME",
        std::optional<std::string>{"relative/cache-secret"});
    ScopedEnvironmentVariable home(
        "HOME", std::optional<std::string>{"relative/unused-home"});
    ScopedEnvironmentVariable sudo_user(
        "SUDO_USER", std::optional<std::string>{"ordinary-user"});

    const xdg_paths::ConfigPaths paths =
        xdg_paths::resolve_config_process_environment();
    expect_path(
        paths.directory, config_home / "moguet",
        "Config process adapter directory");
    expect_path(
        paths.config_file, config_home / "moguet" / "config.toml",
        "Config process adapter config file");
    expect(
        !fs::exists(config_home),
        "Config process adapter mutated the filesystem.");
}

void test_source_preference_resolver_uses_explicit_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "/source-preference/base",
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "relative/cache-secret",
        .home = "relative/unused-home",
    };
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference(environment);

    expect_path(
        paths.directory,
        "/source-preference/base/moguet/source-build.d",
        "Source preference explicit directory");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/source-preference/base", "/source-preference/base",
        {"moguet", "source-build.d"},
        "Source preference explicit creation boundary");
}

void test_source_preference_resolver_uses_home_fallback() {
    for(const std::optional<std::string>& xdg_config_home :
        std::vector<std::optional<std::string>>{std::nullopt, ""}) {
        const xdg_paths::EnvironmentSnapshot environment{
            .xdg_config_home = xdg_config_home,
            .xdg_state_home = "relative/state-secret",
            .xdg_cache_home = "relative/cache-secret",
            .home = "/source-preference/home",
        };
        const xdg_paths::SourcePreferencePaths paths =
            xdg_paths::resolve_source_preference(environment);

        expect_path(
            paths.directory,
            "/source-preference/home/.config/moguet/source-build.d",
            "Source preference HOME fallback directory");
        expect_creation_boundary(
            paths.creation_boundary,
            xdg_paths::DirectorySource::HomeFallback,
            "/source-preference/home/.config",
            "/source-preference/home",
            {".config", "moguet", "source-build.d"},
            "Source preference HOME fallback creation boundary");
    }
}

void test_reviewed_source_state_resolver_uses_explicit_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = "/reviewed-state/base",
        .xdg_cache_home = "relative/cache-secret",
        .home = "relative/unused-home",
    };
    const xdg_paths::ReviewedSourceStatePaths paths =
        xdg_paths::resolve_reviewed_source_state(environment);

    expect_path(
        paths.directory,
        "/reviewed-state/base/moguet/reviewed-sources/aur",
        "Reviewed-source explicit directory");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/reviewed-state/base", "/reviewed-state/base",
        {"moguet", "reviewed-sources", "aur"},
        "Reviewed-source explicit creation boundary");
}

void test_reviewed_source_state_resolver_uses_home_fallback() {
    for(const std::optional<std::string>& xdg_state_home :
        std::vector<std::optional<std::string>>{std::nullopt, ""}) {
        const xdg_paths::EnvironmentSnapshot environment{
            .xdg_config_home = "relative/config-secret",
            .xdg_state_home = xdg_state_home,
            .xdg_cache_home = "relative/cache-secret",
            .home = "/reviewed-state/home",
        };
        const xdg_paths::ReviewedSourceStatePaths paths =
            xdg_paths::resolve_reviewed_source_state(environment);

        expect_path(
            paths.directory,
            "/reviewed-state/home/.local/state/moguet/reviewed-sources/aur",
            "Reviewed-source HOME fallback directory");
        expect_creation_boundary(
            paths.creation_boundary,
            xdg_paths::DirectorySource::HomeFallback,
            "/reviewed-state/home/.local/state",
            "/reviewed-state/home",
            {".local", "state", "moguet", "reviewed-sources", "aur"},
            "Reviewed-source HOME fallback creation boundary");
    }
}

void test_reviewed_source_state_resolver_rejects_relative_xdg_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = std::nullopt,
        .xdg_state_home = "relative/reviewed-state-secret",
        .xdg_cache_home = std::nullopt,
        .home = "/valid/fallback",
    };
    expect_resolution_error(
        [&environment]() {
            static_cast<void>(
                xdg_paths::resolve_reviewed_source_state(environment));
        },
        xdg_paths::DirectoryKind::State,
        xdg_paths::EnvironmentVariable::XdgStateHome,
        xdg_paths::ResolutionErrorCode::RelativePath,
        "XDG_STATE_HOME must be an absolute path",
        "relative/reviewed-state-secret");
}

void test_devel_build_provenance_resolver_is_separate_and_lazy() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = "/provenance-state/base",
        .xdg_cache_home = "relative/cache-secret",
        .home = "relative/unused-home",
    };
    const xdg_paths::DevelBuildProvenancePaths provenance =
        xdg_paths::resolve_devel_build_provenance(environment);
    const xdg_paths::ReviewedSourceStatePaths reviewed =
        xdg_paths::resolve_reviewed_source_state(environment);

    expect_path(
        provenance.directory,
        "/provenance-state/base/moguet/devel-build-provenance/aur",
        "Devel provenance explicit directory");
    expect_creation_boundary(
        provenance.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/provenance-state/base", "/provenance-state/base",
        {"moguet", "devel-build-provenance", "aur"},
        "Devel provenance explicit creation boundary");
    expect(
        provenance.managed_components ==
            std::vector<std::string>{"devel-build-provenance", "aur"},
        "Devel provenance managed components drifted");
    expect(provenance.directory != reviewed.directory,
           "Devel provenance resolver reused reviewed-source namespace");
}

void test_source_preference_resolver_rejects_relative_xdg_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/source-preference-secret",
        .xdg_state_home = std::nullopt,
        .xdg_cache_home = std::nullopt,
        .home = "/valid/fallback",
    };
    expect_resolution_error(
        [&environment]() {
            static_cast<void>(
                xdg_paths::resolve_source_preference(environment));
        },
        xdg_paths::DirectoryKind::Config,
        xdg_paths::EnvironmentVariable::XdgConfigHome,
        xdg_paths::ResolutionErrorCode::RelativePath,
        "XDG_CONFIG_HOME must be an absolute path",
        "relative/source-preference-secret");
}

void test_source_preference_process_adapter_uses_root_context() {
    TemporaryDirectory temporary_directory;
    const fs::path root_home = temporary_directory.path() / "root-home";

    ScopedEnvironmentVariable config_home("XDG_CONFIG_HOME", std::nullopt);
    ScopedEnvironmentVariable state_home(
        "XDG_STATE_HOME",
        std::optional<std::string>{"relative/state-secret"});
    ScopedEnvironmentVariable cache_home(
        "XDG_CACHE_HOME",
        std::optional<std::string>{"relative/cache-secret"});
    ScopedEnvironmentVariable home(
        "HOME", std::optional<std::string>{root_home.string()});
    ScopedEnvironmentVariable user(
        "USER", std::optional<std::string>{"root"});
    ScopedEnvironmentVariable logname(
        "LOGNAME", std::optional<std::string>{"root"});

    xdg_paths::SourcePreferencePaths first_paths;
    {
        ScopedEnvironmentVariable sudo_user(
            "SUDO_USER", std::optional<std::string>{"ordinary-user"});
        first_paths =
            xdg_paths::resolve_source_preference_process_environment();
    }
    xdg_paths::SourcePreferencePaths second_paths;
    {
        ScopedEnvironmentVariable sudo_user(
            "SUDO_USER", std::optional<std::string>{"different-user"});
        second_paths =
            xdg_paths::resolve_source_preference_process_environment();
    }

    expect_path(
        first_paths.directory,
        root_home / ".config" / "moguet" / "source-build.d",
        "Root-like source preference fallback");
    expect_path(
        second_paths.directory, first_paths.directory,
        "SUDO_USER-independent source preference directory");
    expect(
        !fs::exists(root_home),
        "Source preference process adapter mutated the filesystem.");
}

void test_state_only_resolver_ignores_unrelated_xdg_values() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = "/state-only/base",
        .xdg_cache_home = "relative/cache-secret",
        .home = std::nullopt,
    };
    const xdg_paths::StatePaths paths =
        xdg_paths::resolve_state(environment);

    expect_path(
        paths.directory, "/state-only/base/moguet",
        "State-only explicit directory");
    expect_path(
        paths.default_log_file,
        "/state-only/base/moguet/moguet.log",
        "State-only explicit default log");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/state-only/base", "/state-only/base", {"moguet"},
        "State-only explicit creation boundary");
}

void test_cache_only_resolver_uses_explicit_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "/cache-only/base",
        .home = "relative/unused-home",
    };
    const xdg_paths::CachePaths paths =
        xdg_paths::resolve_cache(environment);

    expect_path(
        paths.directory, "/cache-only/base/moguet",
        "Cache-only explicit directory");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::ExplicitXdg,
        "/cache-only/base", "/cache-only/base", {"moguet"},
        "Cache-only explicit creation boundary");
}

void test_cache_only_resolver_uses_home_fallback() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = std::nullopt,
        .home = "/cache-only/home",
    };
    const xdg_paths::CachePaths paths =
        xdg_paths::resolve_cache(environment);

    expect_path(
        paths.directory, "/cache-only/home/.cache/moguet",
        "Cache-only HOME fallback directory");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/cache-only/home/.cache", "/cache-only/home",
        {".cache", "moguet"},
        "Cache-only HOME fallback creation boundary");
}

void test_cache_only_resolver_treats_empty_xdg_as_unset() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = "relative/config-secret",
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "",
        .home = "/cache-only/empty-xdg-home",
    };
    const xdg_paths::CachePaths paths =
        xdg_paths::resolve_cache(environment);

    expect_path(
        paths.directory,
        "/cache-only/empty-xdg-home/.cache/moguet",
        "Cache-only empty XDG fallback directory");
    expect_creation_boundary(
        paths.creation_boundary,
        xdg_paths::DirectorySource::HomeFallback,
        "/cache-only/empty-xdg-home/.cache",
        "/cache-only/empty-xdg-home", {".cache", "moguet"},
        "Cache-only empty XDG fallback creation boundary");
}

void test_cache_only_resolver_rejects_relative_xdg_value() {
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = std::nullopt,
        .xdg_state_home = std::nullopt,
        .xdg_cache_home = "relative/cache-secret",
        .home = "/valid/fallback",
    };
    expect_resolution_error(
        [&environment]() {
            static_cast<void>(xdg_paths::resolve_cache(environment));
        },
        xdg_paths::DirectoryKind::Cache,
        xdg_paths::EnvironmentVariable::XdgCacheHome,
        xdg_paths::ResolutionErrorCode::RelativePath,
        "XDG_CACHE_HOME must be an absolute path",
        "relative/cache-secret");
}

void test_cache_process_adapter_reads_only_cache_authority() {
    TemporaryDirectory temporary_directory;
    const fs::path cache_home = temporary_directory.path() / "cache-home";

    ScopedEnvironmentVariable config_home(
        "XDG_CONFIG_HOME",
        std::optional<std::string>{"relative/config-secret"});
    ScopedEnvironmentVariable state_home(
        "XDG_STATE_HOME",
        std::optional<std::string>{"relative/state-secret"});
    ScopedEnvironmentVariable cache_environment(
        "XDG_CACHE_HOME",
        std::optional<std::string>{cache_home.string()});
    ScopedEnvironmentVariable home(
        "HOME", std::optional<std::string>{"relative/unused-home"});
    ScopedEnvironmentVariable sudo_user(
        "SUDO_USER", std::optional<std::string>{"ordinary-user"});
    ScopedEnvironmentVariable user(
        "USER", std::optional<std::string>{"unrelated-user"});
    ScopedEnvironmentVariable logname(
        "LOGNAME", std::optional<std::string>{"another-user"});

    const xdg_paths::CachePaths paths =
        xdg_paths::resolve_cache_process_environment();
    expect_path(
        paths.directory, cache_home / "moguet",
        "Cache process adapter directory");
    expect(
        !fs::exists(cache_home),
        "Cache process adapter mutated the filesystem.");
}

void test_cache_process_adapter_uses_home_without_identity_inference() {
    TemporaryDirectory temporary_directory;
    const fs::path home_path = temporary_directory.path() / "effective-home";

    ScopedEnvironmentVariable config_home(
        "XDG_CONFIG_HOME",
        std::optional<std::string>{"relative/config-secret"});
    ScopedEnvironmentVariable state_home(
        "XDG_STATE_HOME",
        std::optional<std::string>{"relative/state-secret"});
    ScopedEnvironmentVariable cache_home("XDG_CACHE_HOME", std::nullopt);
    ScopedEnvironmentVariable home(
        "HOME", std::optional<std::string>{home_path.string()});
    ScopedEnvironmentVariable sudo_user(
        "SUDO_USER", std::optional<std::string>{"inferred-user"});
    ScopedEnvironmentVariable user(
        "USER", std::optional<std::string>{"different-user"});
    ScopedEnvironmentVariable logname(
        "LOGNAME", std::optional<std::string>{"third-user"});

    const xdg_paths::CachePaths paths =
        xdg_paths::resolve_cache_process_environment();
    expect_path(
        paths.directory, home_path / ".cache" / "moguet",
        "Cache process adapter HOME fallback directory");
    expect(
        !fs::exists(home_path),
        "Cache process adapter created the HOME fallback tree.");
}

void test_state_process_adapter_reads_only_state_authority() {
    TemporaryDirectory temporary_directory;
    const fs::path state_home = temporary_directory.path() / "state-home";

    ScopedEnvironmentVariable config_home(
        "XDG_CONFIG_HOME",
        std::optional<std::string>{"relative/config-secret"});
    ScopedEnvironmentVariable state_environment(
        "XDG_STATE_HOME",
        std::optional<std::string>{state_home.string()});
    ScopedEnvironmentVariable cache_home(
        "XDG_CACHE_HOME",
        std::optional<std::string>{"relative/cache-secret"});
    ScopedEnvironmentVariable home("HOME", std::nullopt);
    ScopedEnvironmentVariable sudo_user(
        "SUDO_USER", std::optional<std::string>{"different-user"});

    const xdg_paths::StatePaths paths =
        xdg_paths::resolve_state_process_environment();
    expect_path(
        paths.directory, state_home / "moguet",
        "State process adapter directory");
    expect_path(
        paths.default_log_file,
        state_home / "moguet" / "moguet.log",
        "State process adapter default log");
    expect(
        !fs::exists(state_home),
        "State process adapter mutated the filesystem.");
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if(!file) {
        throw std::runtime_error(
            "Failed to read XDG test sentinel: " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::vector<std::string> snapshot_tree(const fs::path& root) {
    std::vector<std::string> snapshot;
    for(const auto& entry : fs::recursive_directory_iterator(root)) {
        std::string description =
            entry.path().lexically_relative(root).generic_string();
        if(entry.is_directory()) {
            description += ":directory";
        } else if(entry.is_regular_file()) {
            description += ":file:" + read_file(entry.path());
        } else if(entry.is_symlink()) {
            description += ":symlink";
        } else {
            description += ":other";
        }
        snapshot.push_back(std::move(description));
    }
    std::sort(snapshot.begin(), snapshot.end());
    return snapshot;
}

void test_resolution_does_not_mutate_filesystem() {
    TemporaryDirectory temporary_directory;
    const fs::path sentinel_directory =
        temporary_directory.path() / "sentinel";
    const fs::path sentinel_file = sentinel_directory / "keep";
    fs::create_directory(sentinel_directory);
    {
        std::ofstream file(sentinel_file, std::ios::binary);
        if(!file) {
            throw std::runtime_error(
                "Failed to create XDG test sentinel.");
        }
        file << "unchanged-sentinel";
    }

    const std::vector<std::string> before =
        snapshot_tree(temporary_directory.path());
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home =
            (temporary_directory.path() / "config-base").string(),
        .xdg_state_home =
            (temporary_directory.path() / "state-base").string(),
        .xdg_cache_home =
            (temporary_directory.path() / "cache-base").string(),
        .home = (temporary_directory.path() / "home").string(),
    };
    const xdg_paths::ResolvedPaths paths = xdg_paths::resolve(environment);
    const std::vector<std::string> after =
        snapshot_tree(temporary_directory.path());

    expect(before == after, "XDG path resolution changed the filesystem tree.");
    expect(
        read_file(sentinel_file) == "unchanged-sentinel",
        "XDG path resolution changed the sentinel.");
    expect(
        !fs::exists(paths.config.directory),
        "XDG path resolution created the config directory.");
    expect(
        !fs::exists(paths.state.directory),
        "XDG path resolution created the state directory.");
    expect(
        !fs::exists(paths.cache.directory),
        "XDG path resolution created the cache directory.");
    expect(
        !fs::exists(paths.config.config_file),
        "XDG path resolution created the config file.");
    expect(
        !fs::exists(paths.state.default_log_file),
        "XDG path resolution created the default log file.");
}

void test_config_only_resolution_does_not_mutate_filesystem() {
    TemporaryDirectory temporary_directory;
    const fs::path sentinel_directory =
        temporary_directory.path() / "sentinel";
    const fs::path sentinel_file = sentinel_directory / "keep";
    fs::create_directory(sentinel_directory);
    {
        std::ofstream file(sentinel_file, std::ios::binary);
        if(!file) {
            throw std::runtime_error(
                "Failed to create config-only XDG test sentinel.");
        }
        file << "unchanged-config-only-sentinel";
    }

    const fs::path config_base = temporary_directory.path() / "config-base";
    const fs::path state_base = temporary_directory.path() / "state-base";
    const fs::path cache_base = temporary_directory.path() / "cache-base";
    const std::vector<std::string> before =
        snapshot_tree(temporary_directory.path());
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = config_base.string(),
        .xdg_state_home = state_base.string(),
        .xdg_cache_home = cache_base.string(),
        .home = std::nullopt,
    };
    const xdg_paths::ConfigPaths paths =
        xdg_paths::resolve_config(environment);
    const std::vector<std::string> after =
        snapshot_tree(temporary_directory.path());

    expect(
        before == after,
        "Config-only XDG path resolution changed the filesystem tree.");
    expect(
        read_file(sentinel_file) == "unchanged-config-only-sentinel",
        "Config-only XDG path resolution changed the sentinel.");
    expect(
        !fs::exists(paths.directory),
        "Config-only XDG path resolution created the config directory.");
    expect(
        !fs::exists(paths.config_file),
        "Config-only XDG path resolution created the config file.");
    expect(
        !fs::exists(state_base),
        "Config-only XDG path resolution created the state directory.");
    expect(
        !fs::exists(cache_base),
        "Config-only XDG path resolution created the cache directory.");
}

void test_source_preference_resolution_does_not_mutate_filesystem() {
    TemporaryDirectory temporary_directory;
    const fs::path sentinel_directory =
        temporary_directory.path() / "sentinel";
    const fs::path sentinel_file = sentinel_directory / "keep";
    fs::create_directory(sentinel_directory);
    {
        std::ofstream file(sentinel_file, std::ios::binary);
        if(!file) {
            throw std::runtime_error(
                "Failed to create source preference XDG test sentinel.");
        }
        file << "unchanged-source-preference-sentinel";
    }

    const fs::path config_base = temporary_directory.path() / "config-base";
    const std::vector<std::string> before =
        snapshot_tree(temporary_directory.path());
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = config_base.string(),
        .xdg_state_home = "relative/state-secret",
        .xdg_cache_home = "relative/cache-secret",
        .home = std::nullopt,
    };
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference(environment);
    const std::vector<std::string> after =
        snapshot_tree(temporary_directory.path());

    expect(
        before == after,
        "Source preference XDG resolution changed the filesystem tree.");
    expect(
        read_file(sentinel_file) ==
            "unchanged-source-preference-sentinel",
        "Source preference XDG resolution changed the sentinel.");
    expect(
        !fs::exists(paths.directory),
        "Source preference XDG resolution created its directory.");
    expect(
        !fs::exists(config_base),
        "Source preference XDG resolution created the config base.");
}

void test_cache_only_resolution_does_not_mutate_filesystem() {
    TemporaryDirectory temporary_directory;
    const fs::path sentinel_directory =
        temporary_directory.path() / "sentinel";
    const fs::path sentinel_file = sentinel_directory / "keep";
    fs::create_directory(sentinel_directory);
    {
        std::ofstream file(sentinel_file, std::ios::binary);
        if(!file) {
            throw std::runtime_error(
                "Failed to create cache-only XDG test sentinel.");
        }
        file << "unchanged-cache-only-sentinel";
    }

    const fs::path config_base = temporary_directory.path() / "config-base";
    const fs::path state_base = temporary_directory.path() / "state-base";
    const fs::path cache_base = temporary_directory.path() / "cache-base";
    const std::vector<std::string> before =
        snapshot_tree(temporary_directory.path());
    const xdg_paths::EnvironmentSnapshot environment{
        .xdg_config_home = config_base.string(),
        .xdg_state_home = state_base.string(),
        .xdg_cache_home = cache_base.string(),
        .home = std::nullopt,
    };
    const xdg_paths::CachePaths paths =
        xdg_paths::resolve_cache(environment);
    const std::vector<std::string> after =
        snapshot_tree(temporary_directory.path());

    expect(
        before == after,
        "Cache-only XDG path resolution changed the filesystem tree.");
    expect(
        read_file(sentinel_file) == "unchanged-cache-only-sentinel",
        "Cache-only XDG path resolution changed the sentinel.");
    expect(
        !fs::exists(paths.directory),
        "Cache-only XDG path resolution created the cache directory.");
    expect(
        !fs::exists(config_base),
        "Cache-only XDG path resolution created the config directory.");
    expect(
        !fs::exists(state_base),
        "Cache-only XDG path resolution created the state directory.");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        run_case("explicit XDG values", test_explicit_xdg_values);
        run_case(
            "unset XDG values use HOME fallback",
            test_unset_xdg_values_use_home_fallback);
        run_case(
            "empty XDG values use HOME fallback",
            test_empty_xdg_values_use_home_fallback);
        run_case(
            "mixed XDG and HOME fallbacks",
            test_mixed_xdg_and_home_fallbacks);
        run_case(
            "application component and derived files",
            test_application_component_and_derived_files);
        run_case(
            "relative XDG values rejected without fallback",
            test_relative_xdg_values_are_rejected_without_fallback);
        run_case(
            "HOME fallback failures identify base",
            test_home_fallback_failures_identify_the_base);
        run_case(
            "invalid unused HOME ignored",
            test_invalid_home_is_ignored_without_fallback);
        run_case(
            "redundant separators normalized",
            test_redundant_separators_are_normalized);
        run_case(
            "dot components rejected before normalization",
            test_dot_components_are_rejected_before_normalization);
        run_case(
            "double leading slash rejected",
            test_ambiguous_double_leading_slash_is_rejected);
        run_case(
            "embedded NUL rejected without disclosure",
            test_embedded_nul_is_rejected_without_disclosure);
        run_case(
            "process adapter ignores sudo user and root inference",
            test_process_adapter_ignores_sudo_user_and_root_inference);
        run_case(
            "config-only resolver uses explicit value",
            test_config_only_resolver_uses_explicit_value);
        run_case(
            "config-only resolver uses HOME fallback",
            test_config_only_resolver_uses_home_fallback);
        run_case(
            "config-only resolver treats empty XDG as unset",
            test_config_only_resolver_treats_empty_xdg_as_unset);
        run_case(
            "config-only resolver rejects relative XDG value",
            test_config_only_resolver_rejects_relative_xdg_value);
        run_case(
            "config process adapter reads only config authority",
            test_config_process_adapter_reads_only_config_authority);
        run_case(
            "source preference resolver uses explicit value",
            test_source_preference_resolver_uses_explicit_value);
        run_case(
            "source preference resolver uses HOME fallback",
            test_source_preference_resolver_uses_home_fallback);
        run_case(
            "source preference resolver rejects relative XDG value",
            test_source_preference_resolver_rejects_relative_xdg_value);
        run_case(
            "reviewed-source resolver uses explicit value",
            test_reviewed_source_state_resolver_uses_explicit_value);
        run_case(
            "reviewed-source resolver uses HOME fallback",
            test_reviewed_source_state_resolver_uses_home_fallback);
        run_case(
            "reviewed-source resolver rejects relative XDG value",
            test_reviewed_source_state_resolver_rejects_relative_xdg_value);
        run_case(
            "devel provenance resolver is separate and lazy",
            test_devel_build_provenance_resolver_is_separate_and_lazy);
        run_case(
            "source preference process adapter uses root context",
            test_source_preference_process_adapter_uses_root_context);
        run_case(
            "state-only resolver ignores unrelated XDG values",
            test_state_only_resolver_ignores_unrelated_xdg_values);
        run_case(
            "cache-only resolver uses explicit value",
            test_cache_only_resolver_uses_explicit_value);
        run_case(
            "cache-only resolver uses HOME fallback",
            test_cache_only_resolver_uses_home_fallback);
        run_case(
            "cache-only resolver treats empty XDG as unset",
            test_cache_only_resolver_treats_empty_xdg_as_unset);
        run_case(
            "cache-only resolver rejects relative XDG value",
            test_cache_only_resolver_rejects_relative_xdg_value);
        run_case(
            "cache process adapter reads only cache authority",
            test_cache_process_adapter_reads_only_cache_authority);
        run_case(
            "cache process adapter uses HOME without identity inference",
            test_cache_process_adapter_uses_home_without_identity_inference);
        run_case(
            "state process adapter reads only state authority",
            test_state_process_adapter_reads_only_state_authority);
        run_case(
            "resolution does not mutate filesystem",
            test_resolution_does_not_mutate_filesystem);
        run_case(
            "config-only resolution does not mutate filesystem",
            test_config_only_resolution_does_not_mutate_filesystem);
        run_case(
            "source preference resolution does not mutate filesystem",
            test_source_preference_resolution_does_not_mutate_filesystem);
        run_case(
            "cache-only resolution does not mutate filesystem",
            test_cache_only_resolution_does_not_mutate_filesystem);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "XDG path tests: all checks passed\n";
    return 0;
}
