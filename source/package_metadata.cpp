#include "package_metadata.hpp"

#include "localization.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "shell_words.hpp"

#include <alpm.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char* PACMAN_DATABASE_PATH_COMMAND =
    "pacman-conf --verbose RootDir DBPath 2>/dev/null";
constexpr const char* PACMAN_REPOSITORY_LIST_COMMAND =
    "pacman-conf --repo-list 2>/dev/null";
constexpr const char* PACMAN_HOLD_PACKAGE_COMMAND =
    "pacman-conf HoldPkg 2>/dev/null";
constexpr std::size_t MAX_ALPM_DIAGNOSTIC_LENGTH = 160;

// POLICY: pacman-managed on-disk sync DBをread-onlyで構造/cache parseする。
// pacman.confのsignature policyを再実行するものではない。
constexpr alpm_siglevel_t READ_ONLY_SYNC_DATABASE_SIGLEVEL =
    static_cast<alpm_siglevel_t>(0);
constexpr const char* CLEANUP_POLICY_BASE_DEVEL_NAME = "base-devel";

[[noreturn]] void throw_package_metadata_error(
    PackageMetadataErrorCode code,
    const std::string& diagnostic) {
    throw PackageMetadataError(PackageMetadataFailure{code, diagnostic});
}

fs::path normalize_absolute_path(
    const fs::path& path,
    const std::string& field_name) {
    if(path.empty() || !path.is_absolute()) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::ConfigurationMalformed,
            localization::format_translated_message(
                // TRANSLATORS: The placeholder is a pacman configuration key.
                "{} must be an absolute path.", field_name));
    }
    if(path.string().find('\0') != std::string::npos) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::ConfigurationMalformed,
            localization::format_translated_message(
                // TRANSLATORS: The placeholder is a pacman configuration key.
                "{} must not contain a null byte.", field_name));
    }

    fs::path normalized = path.lexically_normal();
    // POLICY: pacman-confの末尾slashには意味を持たせないが、root path自体は保持する。
    while(normalized != normalized.root_path() && normalized.filename().empty()) {
        normalized = normalized.parent_path();
    }
    return normalized;
}

PacmanDatabasePaths normalize_pacman_database_paths(
    const PacmanDatabasePaths& paths) {
    return PacmanDatabasePaths{
        normalize_absolute_path(paths.root_dir, "RootDir"),
        normalize_absolute_path(paths.db_path, "DBPath")};
}

PacmanDatabasePaths parse_pacman_database_paths(const std::string& output) {
    std::string root_dir;
    std::string db_path;
    bool has_root_dir = false;
    bool has_db_path = false;

    std::stringstream output_stream(output);
    std::string line;
    while(std::getline(output_stream, line)) {
        if(line.empty() || line.find('\r') != std::string::npos) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::format_translated_message(
                    "{} returned a malformed database path line.",
                    "pacman-conf"));
        }

        const std::string separator = " = ";
        std::size_t separator_position = line.find(separator);
        if(separator_position == std::string::npos) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::format_translated_message(
                    "{} returned a malformed database path line.",
                    "pacman-conf"));
        }

        std::string key = line.substr(0, separator_position);
        std::string value = line.substr(separator_position + separator.size());
        if(value.empty()) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::format_translated_message(
                    "{} returned an empty database path.",
                    "pacman-conf"));
        }

        if(key == "RootDir") {
            if(has_root_dir) {
                throw_package_metadata_error(
                    PackageMetadataErrorCode::ConfigurationMalformed,
                    localization::format_translated_message(
                        "{} returned {} more than once.",
                        "pacman-conf", "RootDir"));
            }
            has_root_dir = true;
            root_dir = std::move(value);
        } else if(key == "DBPath") {
            if(has_db_path) {
                throw_package_metadata_error(
                    PackageMetadataErrorCode::ConfigurationMalformed,
                    localization::format_translated_message(
                        "{} returned {} more than once.",
                        "pacman-conf", "DBPath"));
            }
            has_db_path = true;
            db_path = std::move(value);
        } else {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::format_translated_message(
                    "{} returned an unexpected database path key.",
                    "pacman-conf"));
        }
    }

    if(!has_root_dir || !has_db_path) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::ConfigurationMalformed,
            localization::format_translated_message(
                "{} did not return both {} and {}.", "pacman-conf",
                "RootDir", "DBPath"));
    }

    return normalize_pacman_database_paths(
        PacmanDatabasePaths{fs::path(root_dir), fs::path(db_path)});
}

bool contains_control_character(const std::string& value) {
    for(unsigned char character : value) {
        if(std::iscntrl(character)) return true;
    }
    return false;
}

void validate_repository_names(const std::vector<std::string>& repository_names) {
    std::set<std::string> seen_repository_names;
    for(const auto& repository_name : repository_names) {
        if(repository_name.empty() || contains_control_character(repository_name)) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::translate_message(
                    "Repository configuration contains an invalid repository name."));
        }
        if(!seen_repository_names.insert(repository_name).second) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::translate_message(
                    "Repository configuration contains a duplicate repository name."));
        }
    }
}

std::vector<std::string> parse_pacman_repository_names(
    const std::string& output) {
    std::vector<std::string> repository_names;
    std::stringstream output_stream(output);
    std::string line;
    while(std::getline(output_stream, line)) {
        if(line.empty() || contains_control_character(line)) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::format_translated_message(
                    "{} returned a malformed repository list.",
                    "pacman-conf"));
        }
        repository_names.push_back(std::move(line));
    }
    validate_repository_names(repository_names);
    return repository_names;
}

PacmanRepositoryConfiguration normalize_pacman_repository_configuration(
    const PacmanRepositoryConfiguration& configuration) {
    validate_repository_names(configuration.repository_names);
    return PacmanRepositoryConfiguration{
        normalize_pacman_database_paths(configuration.database_paths),
        configuration.repository_names};
}

struct PacmanRepositoryUsage {
    bool search = false;
    bool install = false;
};

PacmanRepositoryUsage parse_pacman_repository_usage(
    const std::string& output) {
    bool has_all = false;
    bool has_sync = false;
    bool has_search = false;
    bool has_install = false;
    bool has_upgrade = false;
    bool has_value = false;

    std::stringstream output_stream(output);
    std::string line;
    while(std::getline(output_stream, line)) {
        if(line.empty() || contains_control_character(line)) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::translate_message(
                    "Repository Usage configuration is malformed."));
        }

        bool* usage = nullptr;
        if(line == "All") {
            usage = &has_all;
        } else if(line == "Sync") {
            usage = &has_sync;
        } else if(line == "Search") {
            usage = &has_search;
        } else if(line == "Install") {
            usage = &has_install;
        } else if(line == "Upgrade") {
            usage = &has_upgrade;
        } else {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::translate_message(
                    "Repository Usage configuration is malformed."));
        }

        if(*usage) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationMalformed,
                localization::translate_message(
                    "Repository Usage configuration is malformed."));
        }
        *usage = true;
        has_value = true;
    }

    if(!has_value || (has_all && (has_sync || has_search || has_install || has_upgrade))) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::ConfigurationMalformed,
            localization::translate_message(
                "Repository Usage configuration is malformed."));
    }

    return PacmanRepositoryUsage{
        has_all || has_search,
        has_all || has_install};
}

std::string pacman_repository_usage_command(
    const std::string& repository_name) {
    return "pacman-conf --repo " + shell_words::quote(repository_name) +
           " Usage 2>/dev/null";
}

std::string bounded_alpm_error_text(alpm_errno_t error_code) {
    const char* raw_text = alpm_strerror(error_code);
    if(raw_text == nullptr || raw_text[0] == '\0') return "";

    std::string bounded_text;
    for(std::size_t index = 0;
        raw_text[index] != '\0' && index < MAX_ALPM_DIAGNOSTIC_LENGTH;
        ++index) {
        unsigned char character = static_cast<unsigned char>(raw_text[index]);
        bounded_text.push_back(std::iscntrl(character) ? ' ' : raw_text[index]);
    }
    if(raw_text[bounded_text.size()] != '\0') bounded_text += "...";
    return bounded_text;
}

struct AlpmFailureDiagnostics {
    std::string no_error_detail;
    std::string unknown_error;
    std::string external_error;
};

std::string alpm_failure_diagnostic(
    alpm_errno_t error_code, AlpmFailureDiagnostics diagnostics) {
    if(error_code == ALPM_ERR_OK) return diagnostics.no_error_detail;
    if(bounded_alpm_error_text(error_code).empty()) {
        return diagnostics.unknown_error;
    }
    return diagnostics.external_error;
}

std::string repository_package_query_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Repository package query failed: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package query failed: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package query failed: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string foreign_inventory_initialization_failure(
    alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to initialize foreign package inventory: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to initialize foreign package inventory: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to initialize foreign package inventory: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string local_database_access_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to access the local package database: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to access the local package database: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to access the local package database: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string local_database_validation_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Local package database validation failed: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Local package database validation failed: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Local package database validation failed: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string named_repository_database_registration_failure(
    const std::string& repository_name, alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to register repository package database '{}': {} reported no error detail.",
                repository_name, "libalpm"),
            localization::format_translated_message(
                "Failed to register repository package database '{}': unknown {} error.",
                repository_name, "libalpm"),
            localization::format_translated_message(
                "Failed to register repository package database '{}': {}.",
                repository_name,
                bounded_alpm_error_text(error_code))});
}

std::string repository_database_validation_failure(
    alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Repository package database validation failed: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package database validation failed: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package database validation failed: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string local_database_load_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to load the local package database: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to load the local package database: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to load the local package database: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string repository_database_load_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to load a repository package database: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to load a repository package database: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to load a repository package database: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string repository_membership_query_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Repository package membership query failed: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package membership query failed: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package membership query failed: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string package_metadata_session_initialization_failure(
    alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to initialize package metadata session: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to initialize package metadata session: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to initialize package metadata session: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string installed_package_query_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Installed package query failed: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Installed package query failed: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Installed package query failed: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string repository_metadata_session_initialization_failure(
    alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to initialize repository package metadata session: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to initialize repository package metadata session: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to initialize repository package metadata session: {}.",
                bounded_alpm_error_text(error_code))});
}

std::string repository_database_registration_failure(
    alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Failed to register a repository package database: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to register a repository package database: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Failed to register a repository package database: {}.",
                bounded_alpm_error_text(error_code))});
}

struct AlpmHandleReleaser {
    void operator()(alpm_handle_t* handle) const noexcept {
        if(handle != nullptr) static_cast<void>(alpm_release(handle));
    }
};

using UniqueAlpmHandle = std::unique_ptr<alpm_handle_t, AlpmHandleReleaser>;

struct AlpmListReleaser {
    void operator()(alpm_list_t* list) const noexcept {
        if(list != nullptr) alpm_list_free(list);
    }
};

using UniqueAlpmList = std::unique_ptr<alpm_list_t, AlpmListReleaser>;

InstalledPackageReason map_install_reason(alpm_pkgreason_t reason) {
    switch(reason) {
        case ALPM_PKG_REASON_EXPLICIT:
            return InstalledPackageReason::Explicit;
        case ALPM_PKG_REASON_DEPEND:
            return InstalledPackageReason::Dependency;
        case ALPM_PKG_REASON_UNKNOWN:
        default:
            // POLICY: malformed/将来値をExplicitやDependencyへ推測変換しない。
            return InstalledPackageReason::Unknown;
    }
}

InstalledPackageBaseIdentity snapshot_installed_package_base(
    alpm_pkg_t* package) {
    const char* raw_package_base = alpm_pkg_get_base(package);
    if(raw_package_base == nullptr) {
        return InstalledPackageBaseIdentity::missing();
    }

    std::string package_base(raw_package_base);
    if(!is_valid_package_name(package_base)) {
        return InstalledPackageBaseIdentity::malformed();
    }
    return InstalledPackageBaseIdentity::known(std::move(package_base));
}

bool is_valid_package_architecture_metadata(
    const std::string& architecture) noexcept {
    return !architecture.empty() &&
           std::none_of(
               architecture.begin(), architecture.end(),
               [](unsigned char character) {
                   return character <= 0x20 || character == 0x7f;
               });
}

InstalledPackageArchitectureIdentity snapshot_installed_package_architecture(
    alpm_pkg_t* package) {
    const char* raw_architecture = alpm_pkg_get_arch(package);
    if(raw_architecture == nullptr) {
        return InstalledPackageArchitectureIdentity::missing();
    }

    std::string architecture(raw_architecture);
    if(!is_valid_package_architecture_metadata(architecture)) {
        return InstalledPackageArchitectureIdentity::malformed();
    }
    return InstalledPackageArchitectureIdentity::known(
        std::move(architecture));
}

RepositoryProvidedPackageRelation map_provided_package_relation(
    alpm_depmod_t relation) noexcept {
    switch(relation) {
        case ALPM_DEP_MOD_ANY:
            return RepositoryProvidedPackageRelation::Unversioned;
        case ALPM_DEP_MOD_EQ:
            return RepositoryProvidedPackageRelation::Equal;
        case ALPM_DEP_MOD_GE:
            return RepositoryProvidedPackageRelation::GreaterThanOrEqual;
        case ALPM_DEP_MOD_LE:
            return RepositoryProvidedPackageRelation::LessThanOrEqual;
        case ALPM_DEP_MOD_GT:
            return RepositoryProvidedPackageRelation::GreaterThan;
        case ALPM_DEP_MOD_LT:
            return RepositoryProvidedPackageRelation::LessThan;
    }
    return RepositoryProvidedPackageRelation::Unsupported;
}

using RepositoryProvidedPackageMetadataResult = std::variant<
    std::vector<RepositoryProvidedPackageMetadata>,
    PackageMetadataFailure>;

using InstalledRuntimeDependencySpecificationsResult = std::variant<
    std::vector<std::string>,
    PackageMetadataFailure>;

using RepositoryPackageBaseSnapshotResult = std::variant<
    std::string,
    PackageMetadataFailure>;

using RepositoryPackageArchitectureSnapshotResult = std::variant<
    std::string,
    PackageMetadataFailure>;

RepositoryPackageBaseSnapshotResult snapshot_repository_package_base(
    alpm_pkg_t* package) {
    const char* package_base = alpm_pkg_get_base(package);
    if(package_base == nullptr || !is_valid_package_name(package_base)) {
        return PackageMetadataFailure{
            PackageMetadataErrorCode::MalformedMetadata,
            localization::format_translated_message(
                "Repository package metadata contains an invalid {}.",
                "PackageBase")};
    }
    return std::string(package_base);
}

RepositoryPackageArchitectureSnapshotResult
snapshot_repository_package_architecture(alpm_pkg_t* package) {
    const char* raw_architecture = alpm_pkg_get_arch(package);
    if(raw_architecture == nullptr) {
        return PackageMetadataFailure{
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Repository package metadata is missing its architecture.")};
    }

    std::string architecture(raw_architecture);
    if(!is_valid_package_architecture_metadata(architecture)) {
        return PackageMetadataFailure{
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Repository package metadata contains an invalid architecture.")};
    }
    return architecture;
}

RepositoryProvidedPackageMetadataResult snapshot_package_provides(
    alpm_pkg_t* package) {
    std::vector<RepositoryProvidedPackageMetadata> provides;
    for(alpm_list_t* node = alpm_pkg_get_provides(package);
        node != nullptr;
        node = node->next) {
        if(node->data == nullptr) {
            return PackageMetadataFailure{
                PackageMetadataErrorCode::MalformedMetadata,
                "Package metadata contains an invalid provided capability."};
        }

        const auto* dependency = static_cast<const alpm_depend_t*>(node->data);
        provides.push_back(RepositoryProvidedPackageMetadata{
            dependency->name == nullptr
                ? std::nullopt
                : std::optional<std::string>(dependency->name),
            dependency->version == nullptr
                ? std::nullopt
                : std::optional<std::string>(dependency->version),
            map_provided_package_relation(dependency->mod)});
    }
    return provides;
}

InstalledRuntimeDependencySpecificationsResult
snapshot_package_runtime_dependency_specifications(alpm_pkg_t* package) {
    std::vector<std::string> dependencies;
    for(alpm_list_t* node = alpm_pkg_get_depends(package);
        node != nullptr;
        node = node->next) {
        if(node->data == nullptr) {
            return PackageMetadataFailure{
                PackageMetadataErrorCode::MalformedMetadata,
                "Installed package metadata contains an invalid runtime dependency."};
        }

        const auto* dependency =
            static_cast<const alpm_depend_t*>(node->data);
        std::unique_ptr<char, decltype(&std::free)> specification(
            alpm_dep_compute_string(dependency), &std::free);
        if(specification == nullptr || specification.get()[0] == '\0') {
            return PackageMetadataFailure{
                PackageMetadataErrorCode::MalformedMetadata,
                "Installed package metadata contains an invalid runtime dependency."};
        }
        dependencies.emplace_back(specification.get());
    }
    return dependencies;
}

PackageMetadataFailure query_failure(
    PackageMetadataErrorCode code,
    const std::string& diagnostic) {
    return PackageMetadataFailure{code, diagnostic};
}

std::string repository_package_search_failure(alpm_errno_t error_code) {
    return alpm_failure_diagnostic(
        error_code,
        AlpmFailureDiagnostics{
            localization::format_translated_message(
                "Repository package search failed: {} reported no error detail.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package search failed: unknown {} error.",
                "libalpm"),
            localization::format_translated_message(
                "Repository package search failed: {}.",
                bounded_alpm_error_text(error_code))});
}

std::optional<std::string> optional_package_metadata_text(
    const char* raw_value) {
    if(raw_value == nullptr) return std::nullopt;
    return std::string(raw_value);
}

using RepositoryPackageSearchMatchResult = std::variant<
    RepositoryPackageSearchMatch,
    PackageMetadataFailure>;

RepositoryPackageSearchMatchResult make_repository_package_search_match(
    alpm_pkg_t* package,
    const std::string& repository_name,
    RepositoryPackageSearchMatchKind kind,
    std::optional<std::string> group_name) {
    if(package == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Repository package search returned an invalid package entry."));
    }

    const char* raw_package_name = alpm_pkg_get_name(package);
    if(raw_package_name == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Repository package search metadata contains an invalid package name."));
    }

    return RepositoryPackageSearchMatch{
        repository_name,
        raw_package_name,
        optional_package_metadata_text(alpm_pkg_get_version(package)),
        optional_package_metadata_text(alpm_pkg_get_desc(package)),
        kind,
        std::move(group_name)};
}

RepositoryPackageQueryResult query_repository_database(
    alpm_handle_t* handle,
    alpm_db_t* database,
    const std::string& repository_name,
    const std::string& package_name) {
    alpm_pkg_t* package = alpm_db_get_pkg(database, package_name.c_str());
    if(package == nullptr) {
        alpm_errno_t query_error = alpm_errno(handle);
        if(query_error == ALPM_ERR_PKG_NOT_FOUND) return PackageNotFound{};
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            repository_package_query_failure(query_error));
    }

    // POLICY: libalpmのnonnull returnがquery成功であり、以前の失敗errnoは参照しない。
    const char* returned_name = alpm_pkg_get_name(package);
    if(returned_name == nullptr || package_name != returned_name) {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Repository package metadata contains an invalid package name."));
    }

    off_t package_size = alpm_pkg_get_size(package);
    off_t installed_size = alpm_pkg_get_isize(package);
    if(package_size < static_cast<off_t>(0) ||
       installed_size < static_cast<off_t>(0)) {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Repository package metadata contains an invalid package size."));
    }

    return RepositoryPackageMetadata{
        repository_name,
        returned_name,
        static_cast<std::uint64_t>(package_size),
        static_cast<std::uint64_t>(installed_size)};
}

ForeignPackageInventory query_foreign_package_inventory_read_phase(
    const PacmanRepositoryConfiguration& configuration) {
    PacmanRepositoryConfiguration normalized_configuration =
        normalize_pacman_repository_configuration(configuration);
    if(normalized_configuration.repository_names.empty()) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            localization::translate_message(
                "No package repositories are configured for foreign package inventory."));
    }

    std::string root_dir = normalized_configuration.database_paths.root_dir.string();
    std::string db_path = normalized_configuration.database_paths.db_path.string();

    alpm_errno_t initialization_error = ALPM_ERR_OK;
    alpm_handle_t* raw_handle =
        alpm_initialize(root_dir.c_str(), db_path.c_str(), &initialization_error);
    if(raw_handle == nullptr) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::InitializationFailed,
            foreign_inventory_initialization_failure(
                initialization_error));
    }
    UniqueAlpmHandle handle(raw_handle);

    // POLICY: nonnull pointerがAPI上の成功条件。handle-global errnoは成功時に
    // 以前の値を保持し得るため、失敗時のdiagnosticにだけ使用する。
    alpm_db_t* local_db = alpm_get_localdb(handle.get());
    if(local_db == nullptr) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::LocalDatabaseUnavailable,
            local_database_access_failure(alpm_errno(handle.get())));
    }

    if(alpm_db_get_valid(local_db) != 0) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::LocalDatabaseUnavailable,
            local_database_validation_failure(alpm_errno(handle.get())));
    }

    std::vector<alpm_db_t*> sync_databases;
    sync_databases.reserve(normalized_configuration.repository_names.size());
    for(const auto& repository_name : normalized_configuration.repository_names) {
        alpm_db_t* database = alpm_register_syncdb(
            handle.get(), repository_name.c_str(),
            READ_ONLY_SYNC_DATABASE_SIGLEVEL);
        if(database == nullptr) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::SyncDatabaseUnavailable,
                named_repository_database_registration_failure(
                    repository_name, alpm_errno(handle.get())));
        }
        sync_databases.push_back(database);
    }

    for(alpm_db_t* database : sync_databases) {
        if(alpm_db_get_valid(database) != 0) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::SyncDatabaseUnavailable,
                repository_database_validation_failure(
                    alpm_errno(handle.get())));
        }
    }

    // POLICY(#266): cacheをlookup前に全件loadし、正常emptyとload failureを
    // nullptr直後のerrnoで区別する。local listはこのreturn値を一度だけ走査する。
    alpm_list_t* local_package_cache = alpm_db_get_pkgcache(local_db);
    if(local_package_cache == nullptr) {
        alpm_errno_t package_cache_error = alpm_errno(handle.get());
        if(package_cache_error != ALPM_ERR_OK) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::LocalDatabaseUnavailable,
                local_database_load_failure(package_cache_error));
        }
    }

    for(alpm_db_t* database : sync_databases) {
        alpm_list_t* package_cache = alpm_db_get_pkgcache(database);
        if(package_cache == nullptr) {
            alpm_errno_t package_cache_error = alpm_errno(handle.get());
            if(package_cache_error != ALPM_ERR_OK) {
                throw_package_metadata_error(
                    PackageMetadataErrorCode::SyncDatabaseUnavailable,
                    repository_database_load_failure(
                        package_cache_error));
            }
        }
    }

    ForeignPackageInventory inventory;
    // POLICY(#266): pacman -Qmと同じくlocal cache順を正本にし、独自sortしない。
    for(alpm_list_t* node = local_package_cache; node != nullptr; node = node->next) {
        if(node->data == nullptr) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::QueryFailed,
                localization::translate_message(
                    "Local package cache contains an invalid package entry."));
        }

        auto* local_package = static_cast<alpm_pkg_t*>(node->data);
        const char* raw_package_name = alpm_pkg_get_name(local_package);
        if(raw_package_name == nullptr || raw_package_name[0] == '\0') {
            throw_package_metadata_error(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid package name."));
        }

        std::string package_name(raw_package_name);
        if(!is_valid_package_name(package_name)) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid package name."));
        }

        bool is_native_package = false;
        for(alpm_db_t* database : sync_databases) {
            alpm_pkg_t* sync_package =
                alpm_db_get_pkg(database, package_name.c_str());
            if(sync_package == nullptr) {
                alpm_errno_t query_error = alpm_errno(handle.get());
                if(query_error == ALPM_ERR_PKG_NOT_FOUND) continue;
                throw_package_metadata_error(
                    PackageMetadataErrorCode::QueryFailed,
                    repository_membership_query_failure(query_error));
            }

            const char* returned_name = alpm_pkg_get_name(sync_package);
            if(returned_name == nullptr || returned_name[0] == '\0') {
                throw_package_metadata_error(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Repository package metadata contains an invalid package name."));
            }
            std::string returned_package_name(returned_name);
            if(!is_valid_package_name(returned_package_name) ||
               package_name != returned_package_name) {
                throw_package_metadata_error(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Repository package metadata contains an invalid package name."));
            }
            is_native_package = true;
            break;
        }
        if(is_native_package) continue;

        const char* installed_version = alpm_pkg_get_version(local_package);
        if(installed_version == nullptr || installed_version[0] == '\0') {
            throw_package_metadata_error(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Foreign package metadata contains an invalid version."));
        }

        inventory.push_back(InstalledPackageMetadata{
            std::move(package_name),
            installed_version,
            map_install_reason(alpm_pkg_get_reason(local_package))});
    }
    return inventory;
}

using CleanupPolicyCandidateMetadataResult = std::variant<
    CleanupPolicyCandidatePackageMetadata,
    PackageMetadataFailure>;
using CleanupPolicyMetaPackageMetadataResult = std::variant<
    CleanupPolicyMetaPackageMetadata,
    PackageMetadataFailure>;
using CleanupPolicyCandidateEvaluationResult = std::variant<
    CleanupPolicyCandidateEvaluation,
    PackageMetadataFailure>;

CleanupPolicyAuthorityEvidence make_unobserved_cleanup_policy_authority(
    CleanupPolicyAuthorityKind authority_kind) {
    return CleanupPolicyAuthorityEvidence{
        authority_kind,
        CleanupPolicyAuthorityObservation::NotObserved,
        CleanupPolicyMetadataCompleteness::Incomplete,
        CleanupPolicyCandidateEvaluation::NotEvaluated,
        CleanupPolicyMetadataCompleteness::Incomplete,
        {},
        std::nullopt};
}

CleanupPolicyProtectionEvidence make_empty_cleanup_policy_evidence() {
    return CleanupPolicyProtectionEvidence{
        CleanupPolicyMetadataCompleteness::Incomplete,
        CleanupPolicyMetadataCompleteness::Incomplete,
        std::nullopt,
        make_unobserved_cleanup_policy_authority(
            CleanupPolicyAuthorityKind::InstalledBaseDevelMetaPackage),
        make_unobserved_cleanup_policy_authority(
            CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage),
        make_unobserved_cleanup_policy_authority(
            CleanupPolicyAuthorityKind::BaseDevelGroupCompatibility),
        CleanupPolicyEvidenceConsistency::Consistent,
        {}};
}

using CleanupPolicyDependencySnapshotResult = std::variant<
    std::vector<std::string>,
    PackageMetadataFailure>;

CleanupPolicyDependencySnapshotResult snapshot_cleanup_policy_dependencies(
    alpm_list_t* dependencies,
    const std::string& malformed_diagnostic,
    bool require_nonempty) {
    std::vector<std::string> snapshot;
    std::set<std::string> observed_dependencies;
    for(alpm_list_t* node = dependencies; node != nullptr; node = node->next) {
        if(node->data == nullptr) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                malformed_diagnostic);
        }

        const auto* dependency =
            static_cast<const alpm_depend_t*>(node->data);
        if(dependency->name == nullptr ||
           !is_valid_package_name(dependency->name)) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                malformed_diagnostic);
        }

        std::unique_ptr<char, decltype(&std::free)> specification(
            alpm_dep_compute_string(dependency), &std::free);
        if(specification == nullptr || specification.get()[0] == '\0') {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                malformed_diagnostic);
        }

        std::string owned_specification(specification.get());
        if(!observed_dependencies.insert(owned_specification).second) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                malformed_diagnostic);
        }
        snapshot.push_back(std::move(owned_specification));
    }

    if(require_nonempty && snapshot.empty()) {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            malformed_diagnostic);
    }
    return snapshot;
}

using CleanupPolicyGroupSnapshotResult = std::variant<
    std::vector<std::string>,
    PackageMetadataFailure>;

CleanupPolicyGroupSnapshotResult snapshot_cleanup_policy_groups(
    alpm_pkg_t* package);
CleanupPolicyCandidateMetadataResult snapshot_cleanup_policy_candidate(
    alpm_pkg_t* package,
    const std::string& expected_package_name);
CleanupPolicyMetaPackageMetadataResult snapshot_cleanup_policy_meta_package(
    alpm_pkg_t* package,
    CleanupPolicyAuthorityKind authority_kind,
    std::optional<std::size_t> configured_repository_order,
    std::optional<std::string> repository_name);
CleanupPolicyCandidateEvaluationResult
evaluate_cleanup_policy_candidate_satisfaction(
    alpm_pkg_t* candidate,
    const std::vector<std::string>& dependency_specifications);
std::vector<std::string> canonical_dependency_inventory(
    const CleanupPolicyMetaPackageMetadata& metadata);

} // namespace

struct PackageMetadataSession::Impl {
    Impl(
        UniqueAlpmHandle owned_handle,
        alpm_db_t* borrowed_local_db,
        alpm_list_t* borrowed_local_package_cache) noexcept
        : handle(std::move(owned_handle)),
          local_db(borrowed_local_db),
          local_package_cache(borrowed_local_package_cache) {
    }

    UniqueAlpmHandle handle;
    alpm_db_t* local_db;
    alpm_list_t* local_package_cache;
};

struct RepositoryPackageMetadataSession::Impl {
    struct RegisteredRepository {
        std::string repository_name;
        alpm_db_t* database;
        alpm_list_t* package_cache;
    };

    Impl(
        UniqueAlpmHandle owned_handle,
        std::vector<RegisteredRepository> registered_repositories) noexcept
        : handle(std::move(owned_handle)),
          repositories(std::move(registered_repositories)) {
    }

    UniqueAlpmHandle handle;
    std::vector<RegisteredRepository> repositories;
};

CleanupPolicyProtectionEvidence query_cleanup_policy_protection_evidence(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& candidate_package_name) {
    CleanupPolicyProtectionEvidence evidence =
        make_empty_cleanup_policy_evidence();

    if(!is_valid_package_name(candidate_package_name)) {
        evidence.candidate_metadata_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        evidence.failures.push_back(query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            "Cleanup policy candidate package name is invalid."));
        return evidence;
    }

    std::optional<PackageMetadataSession> local_session;
    try {
        local_session.emplace(PackageMetadataSession::open(
            configuration.database_paths));
    } catch(const PackageMetadataError& error) {
        evidence.local_database_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        evidence.failures.push_back(error.failure());
        return evidence;
    }
    evidence.local_database_completeness =
        CleanupPolicyMetadataCompleteness::Complete;

    PackageMetadataSession::Impl* local = local_session->impl_.get();
    alpm_pkg_t* candidate = alpm_db_get_pkg(
        local->local_db, candidate_package_name.c_str());
    if(candidate == nullptr) {
        const alpm_errno_t query_error = alpm_errno(local->handle.get());
        if(query_error == ALPM_ERR_PKG_NOT_FOUND) {
            evidence.candidate_metadata_completeness =
                CleanupPolicyMetadataCompleteness::Incomplete;
        } else {
            evidence.candidate_metadata_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(query_failure(
                PackageMetadataErrorCode::QueryFailed,
                installed_package_query_failure(query_error)));
        }
        return evidence;
    }

    CleanupPolicyCandidateMetadataResult candidate_metadata =
        snapshot_cleanup_policy_candidate(candidate, candidate_package_name);
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&candidate_metadata);
       failure != nullptr) {
        evidence.candidate_metadata_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        evidence.failures.push_back(*failure);
        return evidence;
    }
    evidence.candidate =
        std::get<CleanupPolicyCandidatePackageMetadata>(
            std::move(candidate_metadata));
    evidence.candidate_metadata_completeness =
        CleanupPolicyMetadataCompleteness::Complete;

    CleanupPolicyAuthorityEvidence& installed_authority =
        evidence.installed_base_devel;
    alpm_pkg_t* installed_base_devel = alpm_db_get_pkg(
        local->local_db, CLEANUP_POLICY_BASE_DEVEL_NAME);
    if(installed_base_devel == nullptr) {
        const alpm_errno_t query_error = alpm_errno(local->handle.get());
        if(query_error != ALPM_ERR_PKG_NOT_FOUND) {
            installed_authority.observation =
                CleanupPolicyAuthorityObservation::Unavailable;
            installed_authority.inventory_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            installed_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(query_failure(
                PackageMetadataErrorCode::QueryFailed,
                installed_package_query_failure(query_error)));
            return evidence;
        }
        installed_authority.observation =
            CleanupPolicyAuthorityObservation::Absent;
        installed_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Complete;
    } else {
        installed_authority.observation =
            CleanupPolicyAuthorityObservation::Present;
        CleanupPolicyMetaPackageMetadataResult metadata =
            snapshot_cleanup_policy_meta_package(
                installed_base_devel,
                CleanupPolicyAuthorityKind::InstalledBaseDevelMetaPackage,
                std::nullopt, std::nullopt);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&metadata);
           failure != nullptr) {
            installed_authority.inventory_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            installed_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(*failure);
            return evidence;
        }

        installed_authority.meta_packages.push_back(
            std::get<CleanupPolicyMetaPackageMetadata>(
                std::move(metadata)));
        installed_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Complete;

        if(evidence.candidate->package_name ==
           CLEANUP_POLICY_BASE_DEVEL_NAME) {
            installed_authority.candidate_evaluation =
                CleanupPolicyCandidateEvaluation::Protected;
            installed_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Complete;
            return evidence;
        }

        CleanupPolicyCandidateEvaluationResult evaluation =
            evaluate_cleanup_policy_candidate_satisfaction(
                candidate,
                installed_authority.meta_packages.front().dependencies);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&evaluation);
           failure != nullptr) {
            installed_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(*failure);
            return evidence;
        }
        installed_authority.candidate_evaluation =
            std::get<CleanupPolicyCandidateEvaluation>(evaluation);
        installed_authority.evaluation_completeness =
            CleanupPolicyMetadataCompleteness::Complete;
        return evidence;
    }

    std::optional<RepositoryPackageMetadataSession> sync_session;
    try {
        sync_session.emplace(
            RepositoryPackageMetadataSession::open(configuration));
    } catch(const PackageMetadataError& error) {
        CleanupPolicyAuthorityEvidence& sync_authority =
            evidence.configured_sync_base_devel;
        sync_authority.observation =
            CleanupPolicyAuthorityObservation::Unavailable;
        sync_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        sync_authority.evaluation_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        evidence.failures.push_back(error.failure());
        return evidence;
    }

    RepositoryPackageMetadataSession::Impl* sync = sync_session->impl_.get();
    CleanupPolicyAuthorityEvidence& sync_authority =
        evidence.configured_sync_base_devel;
    if(sync->repositories.empty()) {
        sync_authority.observation =
            CleanupPolicyAuthorityObservation::Unavailable;
        sync_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        sync_authority.evaluation_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        evidence.failures.push_back(query_failure(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            "No configured sync database is available for cleanup policy metadata."));
        return evidence;
    }
    bool saw_exact_meta_package = false;
    bool saw_sync_failure = false;
    bool saw_complete_positive = false;
    bool saw_complete_negative = false;
    std::optional<std::vector<std::string>> expected_dependency_inventory;

    for(std::size_t repository_order = 0;
        repository_order < sync->repositories.size();
        ++repository_order) {
        const auto& repository = sync->repositories[repository_order];
        alpm_pkg_t* package = alpm_db_get_pkg(
            repository.database, CLEANUP_POLICY_BASE_DEVEL_NAME);
        if(package == nullptr) {
            const alpm_errno_t query_error = alpm_errno(sync->handle.get());
            if(query_error == ALPM_ERR_PKG_NOT_FOUND) continue;

            saw_sync_failure = true;
            evidence.failures.push_back(query_failure(
                PackageMetadataErrorCode::QueryFailed,
                repository_package_query_failure(query_error)));
            continue;
        }

        saw_exact_meta_package = true;
        CleanupPolicyMetaPackageMetadataResult metadata =
            snapshot_cleanup_policy_meta_package(
                package,
                CleanupPolicyAuthorityKind::
                    ConfiguredSyncBaseDevelMetaPackage,
                repository_order, repository.repository_name);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&metadata);
           failure != nullptr) {
            saw_sync_failure = true;
            evidence.failures.push_back(*failure);
            continue;
        }

        CleanupPolicyMetaPackageMetadata owned_metadata =
            std::get<CleanupPolicyMetaPackageMetadata>(
                std::move(metadata));
        const std::vector<std::string> dependency_inventory =
            canonical_dependency_inventory(owned_metadata);
        if(!expected_dependency_inventory.has_value()) {
            expected_dependency_inventory = dependency_inventory;
        } else if(expected_dependency_inventory.value() !=
                  dependency_inventory) {
            evidence.consistency =
                CleanupPolicyEvidenceConsistency::Contradictory;
        }

        CleanupPolicyCandidateEvaluationResult evaluation =
            evaluate_cleanup_policy_candidate_satisfaction(
                candidate, owned_metadata.dependencies);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&evaluation);
           failure != nullptr) {
            saw_sync_failure = true;
            evidence.failures.push_back(*failure);
        } else if(std::get<CleanupPolicyCandidateEvaluation>(evaluation) ==
                  CleanupPolicyCandidateEvaluation::Protected) {
            saw_complete_positive = true;
        } else {
            saw_complete_negative = true;
        }
        sync_authority.meta_packages.push_back(
            std::move(owned_metadata));
    }

    if(saw_exact_meta_package) {
        sync_authority.observation =
            CleanupPolicyAuthorityObservation::Present;
        sync_authority.inventory_completeness =
            saw_sync_failure
                ? CleanupPolicyMetadataCompleteness::Incomplete
                : CleanupPolicyMetadataCompleteness::Complete;
        if(saw_complete_positive) {
            // A complete positive satisfier observation is sufficient to
            // protect even when a separate configured source failed.  The
            // same partial inventory can never prove NotProtected.
            sync_authority.candidate_evaluation =
                CleanupPolicyCandidateEvaluation::Protected;
            sync_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Complete;
        } else if(!saw_sync_failure && saw_complete_negative) {
            sync_authority.candidate_evaluation =
                CleanupPolicyCandidateEvaluation::NotProtected;
            sync_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Complete;
        } else {
            sync_authority.evaluation_completeness =
                saw_sync_failure
                    ? CleanupPolicyMetadataCompleteness::Failed
                    : CleanupPolicyMetadataCompleteness::Incomplete;
        }
        return evidence;
    }

    if(saw_sync_failure) {
        sync_authority.observation =
            CleanupPolicyAuthorityObservation::Unavailable;
        sync_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Incomplete;
        sync_authority.evaluation_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        return evidence;
    }

    sync_authority.observation =
        CleanupPolicyAuthorityObservation::Absent;
    sync_authority.inventory_completeness =
        CleanupPolicyMetadataCompleteness::Complete;

    CleanupPolicyAuthorityEvidence& group_authority =
        evidence.base_devel_group;
    CleanupPolicyGroupMetadata group_metadata{
        CLEANUP_POLICY_BASE_DEVEL_NAME, {}, {}, {}};
    group_metadata.repository_order.reserve(sync->repositories.size());
    bool found_exact_group = false;
    for(const auto& repository : sync->repositories) {
        group_metadata.repository_order.push_back(
            repository.repository_name);
        alpm_group_t* group = alpm_db_get_group(
            repository.database, CLEANUP_POLICY_BASE_DEVEL_NAME);
        // POLICY: open() preloaded every configured package cache.  At this
        // point nullptr is an authoritative exact-group miss; group fallback
        // is never inferred from a missing cache or registration failure.
        if(group == nullptr) continue;
        found_exact_group = true;
        if(group->name == nullptr ||
           std::string_view(group->name) !=
               CLEANUP_POLICY_BASE_DEVEL_NAME) {
            group_authority.observation =
                CleanupPolicyAuthorityObservation::Present;
            group_authority.inventory_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            group_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                "Cleanup policy base-devel group metadata is malformed."));
            return evidence;
        }
        group_metadata.repositories_with_group.push_back(
            repository.repository_name);
    }

    if(!found_exact_group) {
        group_authority.observation =
            CleanupPolicyAuthorityObservation::Absent;
        group_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Complete;
        return evidence;
    }

    std::vector<alpm_list_t> database_nodes(sync->repositories.size());
    for(std::size_t index = 0; index < sync->repositories.size(); ++index) {
        database_nodes[index].data = sync->repositories[index].database;
        database_nodes[index].prev =
            index == 0 ? nullptr : &database_nodes[index - 1];
        database_nodes[index].next =
            index + 1 == database_nodes.size()
                ? nullptr
                : &database_nodes[index + 1];
    }
    alpm_list_t* database_list =
        database_nodes.empty() ? nullptr : &database_nodes.front();
    UniqueAlpmList group_packages(alpm_find_group_pkgs(
        database_list, CLEANUP_POLICY_BASE_DEVEL_NAME));
    if(group_packages == nullptr) {
        group_authority.observation =
            CleanupPolicyAuthorityObservation::Present;
        group_authority.inventory_completeness =
            CleanupPolicyMetadataCompleteness::Incomplete;
        group_authority.evaluation_completeness =
            CleanupPolicyMetadataCompleteness::Failed;
        evidence.failures.push_back(query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            "Cleanup policy base-devel group inventory is empty or unavailable."));
        return evidence;
    }

    std::set<std::string> observed_group_members;
    for(alpm_list_t* node = group_packages.get();
        node != nullptr;
        node = node->next) {
        auto* package = static_cast<alpm_pkg_t*>(node->data);
        alpm_db_t* package_database =
            package == nullptr ? nullptr : alpm_pkg_get_db(package);
        std::optional<std::size_t> source_order;
        for(std::size_t index = 0;
            index < sync->repositories.size();
            ++index) {
            if(sync->repositories[index].database == package_database) {
                source_order = index;
                break;
            }
        }
        const char* package_name =
            package == nullptr ? nullptr : alpm_pkg_get_name(package);
        if(!source_order.has_value() || package_name == nullptr ||
           !is_valid_package_name(package_name) ||
           !observed_group_members.insert(package_name).second) {
            group_authority.observation =
                CleanupPolicyAuthorityObservation::Present;
            group_authority.inventory_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            group_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                "Cleanup policy base-devel group inventory is malformed."));
            return evidence;
        }

        CleanupPolicyGroupSnapshotResult package_groups =
            snapshot_cleanup_policy_groups(package);
        const auto* group_failure =
            std::get_if<PackageMetadataFailure>(&package_groups);
        const auto* groups =
            std::get_if<std::vector<std::string>>(&package_groups);
        if(group_failure != nullptr || groups == nullptr ||
           std::find(
               groups->begin(), groups->end(),
               CLEANUP_POLICY_BASE_DEVEL_NAME) == groups->end()) {
            group_authority.observation =
                CleanupPolicyAuthorityObservation::Present;
            group_authority.inventory_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            group_authority.evaluation_completeness =
                CleanupPolicyMetadataCompleteness::Failed;
            evidence.failures.push_back(
                group_failure != nullptr
                    ? *group_failure
                    : query_failure(
                          PackageMetadataErrorCode::MalformedMetadata,
                          "Cleanup policy base-devel group membership is inconsistent."));
            return evidence;
        }

        group_metadata.members.push_back(
            CleanupPolicyGroupMemberMetadata{
                source_order.value(),
                sync->repositories[source_order.value()].repository_name,
                package_name});
    }

    group_authority.observation =
        CleanupPolicyAuthorityObservation::Present;
    group_authority.inventory_completeness =
        CleanupPolicyMetadataCompleteness::Complete;
    group_authority.group = std::move(group_metadata);
    const bool candidate_declares_exact_group =
        std::find(
            evidence.candidate->groups.begin(),
            evidence.candidate->groups.end(),
            CLEANUP_POLICY_BASE_DEVEL_NAME) !=
        evidence.candidate->groups.end();
    group_authority.candidate_evaluation =
        candidate_declares_exact_group
            ? CleanupPolicyCandidateEvaluation::Protected
            : CleanupPolicyCandidateEvaluation::NotProtected;
    group_authority.evaluation_completeness =
        CleanupPolicyMetadataCompleteness::Complete;
    return evidence;
}

namespace {

CleanupPolicyGroupSnapshotResult snapshot_cleanup_policy_groups(
    alpm_pkg_t* package) {
    std::vector<std::string> groups;
    std::set<std::string> observed_groups;
    for(alpm_list_t* node = alpm_pkg_get_groups(package);
        node != nullptr;
        node = node->next) {
        if(node->data == nullptr) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                "Candidate package metadata contains an invalid group entry.");
        }
        const auto* raw_group_name = static_cast<const char*>(node->data);
        if(raw_group_name[0] == '\0' ||
           contains_control_character(raw_group_name)) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                "Candidate package metadata contains an invalid group entry.");
        }

        std::string group_name(raw_group_name);
        if(!observed_groups.insert(group_name).second) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                "Candidate package metadata contains a duplicate group entry.");
        }
        groups.push_back(std::move(group_name));
    }
    return groups;
}

CleanupPolicyCandidateMetadataResult snapshot_cleanup_policy_candidate(
    alpm_pkg_t* package,
    const std::string& expected_package_name) {
    if(package == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            "Cleanup policy candidate query returned an invalid package entry.");
    }

    const char* raw_package_name = alpm_pkg_get_name(package);
    const char* raw_version = alpm_pkg_get_version(package);
    if(raw_package_name == nullptr ||
       expected_package_name != raw_package_name ||
       !is_valid_package_name(raw_package_name) || raw_version == nullptr ||
       raw_version[0] == '\0') {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            "Cleanup policy candidate metadata is incomplete or malformed.");
    }

    CleanupPolicyDependencySnapshotResult provides =
        snapshot_cleanup_policy_dependencies(
            alpm_pkg_get_provides(package),
            "Cleanup policy candidate metadata contains an invalid provided capability.",
            false);
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&provides);
       failure != nullptr) {
        return *failure;
    }

    CleanupPolicyGroupSnapshotResult groups =
        snapshot_cleanup_policy_groups(package);
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&groups);
       failure != nullptr) {
        return *failure;
    }

    return CleanupPolicyCandidatePackageMetadata{
        raw_package_name,
        raw_version,
        std::get<std::vector<std::string>>(std::move(provides)),
        std::get<std::vector<std::string>>(std::move(groups))};
}

CleanupPolicyMetaPackageMetadataResult snapshot_cleanup_policy_meta_package(
    alpm_pkg_t* package,
    CleanupPolicyAuthorityKind authority_kind,
    std::optional<std::size_t> configured_repository_order,
    std::optional<std::string> repository_name) {
    if(package == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            "Cleanup policy meta-package query returned an invalid package entry.");
    }

    const char* raw_package_name = alpm_pkg_get_name(package);
    const char* raw_version = alpm_pkg_get_version(package);
    if(raw_package_name == nullptr ||
       std::string_view(raw_package_name) != CLEANUP_POLICY_BASE_DEVEL_NAME ||
       raw_version == nullptr || raw_version[0] == '\0') {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            "Cleanup policy base-devel metadata is incomplete or malformed.");
    }

    CleanupPolicyDependencySnapshotResult dependencies =
        snapshot_cleanup_policy_dependencies(
            alpm_pkg_get_depends(package),
            "Cleanup policy base-devel dependency metadata is incomplete or malformed.",
            true);
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&dependencies);
       failure != nullptr) {
        return *failure;
    }

    return CleanupPolicyMetaPackageMetadata{
        authority_kind,
        configured_repository_order,
        std::move(repository_name),
        raw_package_name,
        raw_version,
        std::get<std::vector<std::string>>(std::move(dependencies))};
}

CleanupPolicyCandidateEvaluationResult
evaluate_cleanup_policy_candidate_satisfaction(
    alpm_pkg_t* candidate,
    const std::vector<std::string>& dependency_specifications) {
    if(candidate == nullptr || dependency_specifications.empty()) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            "Cleanup policy satisfier evaluation received incomplete metadata.");
    }

    alpm_list_t candidate_node{candidate, nullptr, nullptr};
    for(const std::string& dependency : dependency_specifications) {
        if(dependency.empty()) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                "Cleanup policy satisfier evaluation received an invalid dependency.");
        }
        alpm_pkg_t* satisfier =
            alpm_find_satisfier(&candidate_node, dependency.c_str());
        if(satisfier == candidate) {
            return CleanupPolicyCandidateEvaluation::Protected;
        }
        if(satisfier != nullptr) {
            return query_failure(
                PackageMetadataErrorCode::QueryFailed,
                "Cleanup policy satisfier evaluation returned an unrelated package.");
        }
    }
    return CleanupPolicyCandidateEvaluation::NotProtected;
}

std::vector<std::string> canonical_dependency_inventory(
    const CleanupPolicyMetaPackageMetadata& metadata) {
    std::vector<std::string> dependencies = metadata.dependencies;
    std::sort(dependencies.begin(), dependencies.end());
    return dependencies;
}

} // namespace

PackageMetadataError::PackageMetadataError(PackageMetadataFailure failure)
    : std::runtime_error(failure.diagnostic), failure_(std::move(failure)) {
}

const PackageMetadataFailure& PackageMetadataError::failure() const noexcept {
    return failure_;
}

ConfiguredHoldPackagePatternsResult query_configured_hold_package_patterns() {
    const auto command = capture_command_output_raw(PACMAN_HOLD_PACKAGE_COMMAND);
    if(command.exit_code != 0 || command.stdout_capture_limit_exceeded) {
        return PackageMetadataFailure{PackageMetadataErrorCode::ConfigurationUnavailable, {}};
    }
    // pacman-conf prints each effective list value followed by a newline, or
    // no bytes for an empty list. Do not split pacman.conf syntax/whitespace,
    // trim malformed output, or interpret the patterns as package names.
    if(!command.output.empty() && command.output.back() != '\n') {
        return PackageMetadataFailure{PackageMetadataErrorCode::ConfigurationMalformed, {}};
    }
    ConfiguredHoldPackagePatterns result;
    std::istringstream output(command.output);
    std::string pattern;
    while(std::getline(output, pattern)) {
        if(pattern.empty() || std::any_of(pattern.begin(), pattern.end(), [](unsigned char byte) {
               return byte <= 0x20 || byte == 0x7f;
           })) {
            return PackageMetadataFailure{PackageMetadataErrorCode::ConfigurationMalformed, {}};
        }
        result.patterns.push_back(std::move(pattern));
    }
    return result;
}

PacmanDatabasePaths resolve_pacman_database_paths() {
    CapturedCommandResult command_result =
        capture_command_output_raw(PACMAN_DATABASE_PATH_COMMAND);
    if(command_result.exit_code != 0) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::ConfigurationUnavailable,
            localization::format_translated_message(
                "{} failed with exit code {}.", "pacman-conf",
                command_result.exit_code));
    }
    return parse_pacman_database_paths(command_result.output);
}

PacmanRepositoryConfiguration resolve_pacman_repository_configuration() {
    PacmanDatabasePaths database_paths = resolve_pacman_database_paths();

    CapturedCommandResult command_result =
        capture_command_output_raw(PACMAN_REPOSITORY_LIST_COMMAND);
    if(command_result.exit_code != 0) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::ConfigurationUnavailable,
            localization::format_translated_message(
                "{} repository list failed with exit code {}.",
                "pacman-conf", command_result.exit_code));
    }

    return PacmanRepositoryConfiguration{
        std::move(database_paths),
        parse_pacman_repository_names(command_result.output)};
}

PacmanRepositoryConfiguration
resolve_pacman_root_search_repository_configuration() {
    PacmanRepositoryConfiguration configuration =
        resolve_pacman_repository_configuration();

    std::vector<std::string> eligible_repository_names;
    eligible_repository_names.reserve(configuration.repository_names.size());
    for(const auto& repository_name : configuration.repository_names) {
        CapturedCommandResult command_result = capture_command_output_raw(
            pacman_repository_usage_command(repository_name).c_str());
        if(command_result.exit_code != 0) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::ConfigurationUnavailable,
                localization::format_translated_message(
                    "{} repository Usage query failed with exit code {}.",
                    "pacman-conf", command_result.exit_code));
        }

        PacmanRepositoryUsage usage =
            parse_pacman_repository_usage(command_result.output);
        if(usage.search && usage.install) {
            eligible_repository_names.push_back(repository_name);
        }
    }

    configuration.repository_names = std::move(eligible_repository_names);
    return configuration;
}

RepositoryPackageSearchResult query_repository_root_package_search(
    const std::string& query) {
    try {
        PacmanRepositoryConfiguration configuration =
            resolve_pacman_root_search_repository_configuration();
        RepositoryPackageMetadataSession session =
            RepositoryPackageMetadataSession::open(configuration);
        return session.query_root_package_search(query);
    } catch(const PackageMetadataError& error) {
        return error.failure();
    }
}

ForeignPackageInventoryResult query_foreign_package_inventory(
    const PacmanRepositoryConfiguration& configuration) {
    try {
        // read phaseのRAII資源はprivate helperからreturnする前に必ず解放される。
        return query_foreign_package_inventory_read_phase(configuration);
    } catch(const PackageMetadataError& error) {
        // Expected metadata/libalpm failuresだけをvalueへ戻す。
        // std::bad_alloc等のruntime failureはflattenしない。
        return error.failure();
    }
}

PackageMetadataSession::PackageMetadataSession(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

PackageMetadataSession::PackageMetadataSession(PackageMetadataSession&&) noexcept = default;

PackageMetadataSession& PackageMetadataSession::operator=(PackageMetadataSession&&) noexcept =
    default;

PackageMetadataSession::~PackageMetadataSession() noexcept = default;

PackageMetadataSession PackageMetadataSession::open(
    const PacmanDatabasePaths& paths) {
    PacmanDatabasePaths normalized_paths = normalize_pacman_database_paths(paths);
    std::string root_dir = normalized_paths.root_dir.string();
    std::string db_path = normalized_paths.db_path.string();

    alpm_errno_t initialization_error = ALPM_ERR_OK;
    alpm_handle_t* raw_handle =
        alpm_initialize(root_dir.c_str(), db_path.c_str(), &initialization_error);
    if(raw_handle == nullptr) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::InitializationFailed,
            package_metadata_session_initialization_failure(
                initialization_error));
    }

    UniqueAlpmHandle handle(raw_handle);
    if(initialization_error != ALPM_ERR_OK) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::InitializationFailed,
            package_metadata_session_initialization_failure(
                initialization_error));
    }

    alpm_db_t* local_db = alpm_get_localdb(handle.get());
    alpm_errno_t local_db_error = alpm_errno(handle.get());
    if(local_db == nullptr || local_db_error != ALPM_ERR_OK) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::LocalDatabaseUnavailable,
            local_database_access_failure(local_db_error));
    }

    int validation_result = alpm_db_get_valid(local_db);
    alpm_errno_t validation_error = alpm_errno(handle.get());
    if(validation_result != 0 || validation_error != ALPM_ERR_OK) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::LocalDatabaseUnavailable,
            local_database_validation_failure(validation_error));
    }

    // LANDMINE: libalpm 16 maps a cache-load failure inside alpm_db_get_pkg() to
    // ALPM_ERR_PKG_NOT_FOUND. Preload here so a later not-found means a lookup miss.
    // nullptr+OK remains a valid empty DB.
    alpm_list_t* local_package_cache = alpm_db_get_pkgcache(local_db);
    alpm_errno_t package_cache_error = alpm_errno(handle.get());
    if(package_cache_error != ALPM_ERR_OK) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::LocalDatabaseUnavailable,
            local_database_load_failure(package_cache_error));
    }

    auto impl = std::make_unique<Impl>(
        std::move(handle), local_db, local_package_cache);
    return PackageMetadataSession(std::move(impl));
}

InstalledPackageQueryResult PackageMetadataSession::query_installed_package(
    const std::string& package_name) const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Package metadata session is not open."));
    }
    if(!is_valid_package_name(package_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }

    alpm_pkg_t* package = alpm_db_get_pkg(impl_->local_db, package_name.c_str());
    if(package == nullptr) {
        alpm_errno_t query_error = alpm_errno(impl_->handle.get());
        if(query_error == ALPM_ERR_PKG_NOT_FOUND) return PackageNotFound{};
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            installed_package_query_failure(query_error));
    }

    // POLICY: libalpmのpublic contractではnonnull returnが成功を表す。
    // 成功時のhandle errnoは以前の失敗を保持し得るため、判定には使わない。

    const char* returned_name = alpm_pkg_get_name(package);
    if(returned_name == nullptr || package_name != returned_name) {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Installed package metadata contains an invalid package name."));
    }

    const char* installed_version = alpm_pkg_get_version(package);
    if(installed_version == nullptr || installed_version[0] == '\0') {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Installed package metadata contains an invalid version."));
    }

    return InstalledPackageMetadata{
        returned_name,
        installed_version,
        map_install_reason(alpm_pkg_get_reason(package)),
        snapshot_installed_package_base(package),
        snapshot_installed_package_architecture(package)};
}

InstalledExactPackageMetadataQueryResult
PackageMetadataSession::query_installed_exact_package_metadata(
    const std::string& package_name) const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Package metadata session is not open."));
    }
    if(!is_valid_package_name(package_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }

    alpm_pkg_t* package = alpm_db_get_pkg(impl_->local_db, package_name.c_str());
    if(package == nullptr) {
        const alpm_errno_t query_error = alpm_errno(impl_->handle.get());
        if(query_error == ALPM_ERR_PKG_NOT_FOUND) return PackageNotFound{};
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            installed_package_query_failure(query_error));
    }

    const char* returned_name = alpm_pkg_get_name(package);
    if(returned_name == nullptr || package_name != returned_name) {
        return query_failure(
            PackageMetadataErrorCode::MalformedMetadata,
            localization::translate_message(
                "Installed package metadata contains an invalid package name."));
    }

    const char* installed_version = alpm_pkg_get_version(package);
    return InstalledExactPackageMetadata{
        returned_name,
        installed_version == nullptr
            ? std::nullopt
            : std::optional<std::string>(installed_version)};
}

LocalPackageVersionSnapshotResult
PackageMetadataSession::snapshot_local_package_versions() const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Package metadata session is not open."));
    }

    LocalPackageVersionSnapshot snapshot;
    // POLICY: open()がpreloadした同じcacheを一度だけ走査する。個別lookupや
    // 再loadを挟まないため、snapshot内の全entryは同じread phaseに属する。
    for(alpm_list_t* node = impl_->local_package_cache;
        node != nullptr;
        node = node->next) {
        if(node->data == nullptr) {
            return query_failure(
                PackageMetadataErrorCode::QueryFailed,
                localization::translate_message(
                    "Local package cache contains an invalid package entry."));
        }

        auto* package = static_cast<alpm_pkg_t*>(node->data);
        const char* raw_package_name = alpm_pkg_get_name(package);
        if(raw_package_name == nullptr || raw_package_name[0] == '\0') {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid package name."));
        }

        std::string package_name(raw_package_name);
        if(!is_valid_package_name(package_name)) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid package name."));
        }

        const char* package_version = alpm_pkg_get_version(package);
        if(package_version == nullptr || package_version[0] == '\0') {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid version."));
        }

        bool inserted = snapshot.emplace(
                                    std::move(package_name), package_version)
                            .second;
        if(!inserted) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains a duplicate package name."));
        }
    }
    return snapshot;
}

InstalledPackageStateSnapshotResult
PackageMetadataSession::snapshot_installed_package_states() const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Package metadata session is not open."));
    }

    InstalledPackageStateSnapshot snapshot;
    // POLICY(#404): open()がpreloadしたcacheを一度だけ走査し、reasonを含む
    // coherentなowned inventoryを作る。malformed entryをabsenceへ落とさない。
    for(alpm_list_t* node = impl_->local_package_cache;
        node != nullptr;
        node = node->next) {
        if(node->data == nullptr) {
            return query_failure(
                PackageMetadataErrorCode::QueryFailed,
                localization::translate_message(
                    "Local package cache contains an invalid package entry."));
        }

        auto* package = static_cast<alpm_pkg_t*>(node->data);
        const char* raw_package_name = alpm_pkg_get_name(package);
        if(raw_package_name == nullptr || raw_package_name[0] == '\0' ||
           !is_valid_package_name(raw_package_name)) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid package name."));
        }

        const char* raw_package_version = alpm_pkg_get_version(package);
        if(raw_package_version == nullptr || raw_package_version[0] == '\0') {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains an invalid version."));
        }

        std::string package_name(raw_package_name);
        InstalledPackageMetadata metadata{
            package_name,
            raw_package_version,
            map_install_reason(alpm_pkg_get_reason(package)),
            snapshot_installed_package_base(package),
            snapshot_installed_package_architecture(package)};
        if(!snapshot.emplace(std::move(package_name), std::move(metadata))
                .second) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Local package metadata contains a duplicate package name."));
        }
    }
    return snapshot;
}

InstalledPackageStateSnapshotResult snapshot_installed_package_states(
    const PacmanDatabasePaths& paths) {
    try {
        PackageMetadataSession session = PackageMetadataSession::open(paths);
        return session.snapshot_installed_package_states();
    } catch(const PackageMetadataError& error) {
        return error.failure();
    }
}

InstalledPackageRelationMetadataInventoryResult
PackageMetadataSession::snapshot_installed_package_relation_metadata() const {
    if(impl_ == nullptr) {
        return InstalledPackageRelationMetadataInventoryFailure{
            {},
            std::nullopt,
            query_failure(
                PackageMetadataErrorCode::QueryFailed,
                localization::translate_message(
                    "Package metadata session is not open."))};
    }

    InstalledPackageRelationMetadataInventory inventory;
    std::set<std::string> observed_package_names;
    std::size_t package_index = 0;
    for(alpm_list_t* node = impl_->local_package_cache;
        node != nullptr;
        node = node->next, ++package_index) {
        if(node->data == nullptr) {
            return InstalledPackageRelationMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::QueryFailed,
                    localization::translate_message(
                        "Local package cache contains an invalid package entry."))};
        }

        auto* package = static_cast<alpm_pkg_t*>(node->data);
        const char* raw_package_name = alpm_pkg_get_name(package);
        if(raw_package_name == nullptr ||
           !is_valid_package_name(raw_package_name)) {
            return InstalledPackageRelationMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Local package metadata contains an invalid package name."))};
        }
        std::string package_name(raw_package_name);
        if(!observed_package_names.insert(package_name).second) {
            return InstalledPackageRelationMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Local package metadata contains a duplicate package name."))};
        }

        const char* raw_version = alpm_pkg_get_version(package);
        std::optional<std::string> version = raw_version == nullptr
                                                 ? std::nullopt
                                                 : std::optional<std::string>(raw_version);
        if(version.has_value() && version->empty()) {
            return InstalledPackageRelationMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Local package metadata contains an invalid version."))};
        }

        RepositoryProvidedPackageMetadataResult provides_result =
            snapshot_package_provides(package);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&provides_result);
           failure != nullptr) {
            return InstalledPackageRelationMetadataInventoryFailure{
                std::move(inventory), package_index, *failure};
        }

        inventory.push_back(InstalledPackageRelationMetadata{
            std::move(package_name),
            std::move(version),
            std::get<std::vector<RepositoryProvidedPackageMetadata>>(
                std::move(provides_result))});
    }
    return inventory;
}

InstalledPackageRuntimeDependencyMetadataInventoryResult
PackageMetadataSession::
    snapshot_installed_package_runtime_dependency_metadata() const {
    if(impl_ == nullptr) {
        return InstalledPackageRuntimeDependencyMetadataInventoryFailure{
            {},
            std::nullopt,
            query_failure(
                PackageMetadataErrorCode::QueryFailed,
                localization::translate_message(
                    "Package metadata session is not open."))};
    }

    InstalledPackageRuntimeDependencyMetadataInventory inventory;
    std::set<std::string> observed_package_names;
    std::size_t package_index = 0;
    for(alpm_list_t* node = impl_->local_package_cache;
        node != nullptr;
        node = node->next, ++package_index) {
        if(node->data == nullptr) {
            return InstalledPackageRuntimeDependencyMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::QueryFailed,
                    localization::translate_message(
                        "Local package cache contains an invalid package entry."))};
        }

        auto* package = static_cast<alpm_pkg_t*>(node->data);
        const char* raw_package_name = alpm_pkg_get_name(package);
        if(raw_package_name == nullptr ||
           !is_valid_package_name(raw_package_name)) {
            return InstalledPackageRuntimeDependencyMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Local package metadata contains an invalid package name."))};
        }
        std::string package_name(raw_package_name);
        if(!observed_package_names.insert(package_name).second) {
            return InstalledPackageRuntimeDependencyMetadataInventoryFailure{
                std::move(inventory),
                package_index,
                query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Local package metadata contains a duplicate package name."))};
        }

        InstalledRuntimeDependencySpecificationsResult dependencies =
            snapshot_package_runtime_dependency_specifications(package);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&dependencies);
           failure != nullptr) {
            return InstalledPackageRuntimeDependencyMetadataInventoryFailure{
                std::move(inventory), package_index, *failure};
        }

        inventory.push_back(InstalledPackageRuntimeDependencyMetadata{
            std::move(package_name),
            std::get<std::vector<std::string>>(
                std::move(dependencies)),
            alpm_pkg_get_version(package) == nullptr
                ? std::nullopt
                : std::optional<std::string>(alpm_pkg_get_version(package))});
    }
    return inventory;
}

std::variant<std::vector<std::string>, PackageMetadataFailure>
PackageMetadataSession::query_installed_runtime_consumers(
    const std::string& candidate_name) const {
    const auto runtime_result = snapshot_installed_package_runtime_dependency_metadata();
    if(const auto* failure = std::get_if<InstalledPackageRuntimeDependencyMetadataInventoryFailure>(&runtime_result)) {
        return failure->failure;
    }
    if(!is_valid_package_name(candidate_name)) {
        return PackageMetadataFailure{PackageMetadataErrorCode::InvalidPackageName, {}};
    }
    alpm_pkg_t* candidate = nullptr;
    for(auto* node = impl_->local_package_cache; node != nullptr; node = node->next) {
        auto* package = static_cast<alpm_pkg_t*>(node->data);
        if(candidate_name == alpm_pkg_get_name(package)) candidate = package;
    }
    if(candidate == nullptr) return PackageMetadataFailure{PackageMetadataErrorCode::QueryFailed, {}};
    // Reuse the strict Provides validator and libalpm's satisfaction authority;
    // no dependency grammar or provider selection is implemented here.
    const auto metadata = snapshot_cleanup_policy_candidate(candidate, candidate_name);
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&metadata)) return *failure;
    std::vector<std::string> consumers;
    for(auto* node = impl_->local_package_cache; node != nullptr; node = node->next) {
        auto* package = static_cast<alpm_pkg_t*>(node->data);
        const auto dependencies = snapshot_cleanup_policy_dependencies(alpm_pkg_get_depends(package), {}, false);
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&dependencies)) return *failure;
        const auto& specifications = std::get<std::vector<std::string>>(dependencies);
        if(specifications.empty()) continue;
        const auto match = evaluate_cleanup_policy_candidate_satisfaction(candidate, specifications);
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&match)) return *failure;
        if(std::get<CleanupPolicyCandidateEvaluation>(match) == CleanupPolicyCandidateEvaluation::Protected) {
            consumers.emplace_back(alpm_pkg_get_name(package));
        }
    }
    return consumers;
}

InstalledPackageRuntimeDependencyMetadataInventoryResult
query_installed_package_runtime_dependency_metadata(
    const PacmanDatabasePaths& paths) {
    try {
        PackageMetadataSession session = PackageMetadataSession::open(paths);
        return session.snapshot_installed_package_runtime_dependency_metadata();
    } catch(const PackageMetadataError& error) {
        return InstalledPackageRuntimeDependencyMetadataInventoryFailure{
            {}, std::nullopt, error.failure()};
    }
}

RepositoryPackageMetadataSession::RepositoryPackageMetadataSession(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

RepositoryPackageMetadataSession::RepositoryPackageMetadataSession(
    RepositoryPackageMetadataSession&&) noexcept = default;

RepositoryPackageMetadataSession& RepositoryPackageMetadataSession::operator=(
    RepositoryPackageMetadataSession&&) noexcept = default;

RepositoryPackageMetadataSession::~RepositoryPackageMetadataSession() noexcept = default;

RepositoryPackageMetadataSession RepositoryPackageMetadataSession::open(
    const PacmanRepositoryConfiguration& configuration) {
    PacmanRepositoryConfiguration normalized_configuration =
        normalize_pacman_repository_configuration(configuration);
    std::string root_dir = normalized_configuration.database_paths.root_dir.string();
    std::string db_path = normalized_configuration.database_paths.db_path.string();

    alpm_errno_t initialization_error = ALPM_ERR_OK;
    alpm_handle_t* raw_handle =
        alpm_initialize(root_dir.c_str(), db_path.c_str(), &initialization_error);
    if(raw_handle == nullptr) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::InitializationFailed,
            repository_metadata_session_initialization_failure(
                initialization_error));
    }

    UniqueAlpmHandle handle(raw_handle);
    if(initialization_error != ALPM_ERR_OK) {
        throw_package_metadata_error(
            PackageMetadataErrorCode::InitializationFailed,
            repository_metadata_session_initialization_failure(
                initialization_error));
    }

    std::vector<Impl::RegisteredRepository> repositories;
    repositories.reserve(normalized_configuration.repository_names.size());
    for(const auto& repository_name : normalized_configuration.repository_names) {
        alpm_db_t* database = alpm_register_syncdb(
            handle.get(), repository_name.c_str(),
            READ_ONLY_SYNC_DATABASE_SIGLEVEL);
        alpm_errno_t registration_error = alpm_errno(handle.get());
        if(database == nullptr || registration_error != ALPM_ERR_OK) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::SyncDatabaseUnavailable,
                repository_database_registration_failure(
                    registration_error));
        }

        int validation_result = alpm_db_get_valid(database);
        alpm_errno_t validation_error = alpm_errno(handle.get());
        if(validation_result != 0 || validation_error != ALPM_ERR_OK) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::SyncDatabaseUnavailable,
                repository_database_validation_failure(
                    validation_error));
        }

        // LANDMINE: missing/corrupt sync DB can surface only while loading the cache.
        // nullptr+OK is retained as a valid empty repository database.
        alpm_list_t* package_cache = alpm_db_get_pkgcache(database);
        alpm_errno_t package_cache_error = alpm_errno(handle.get());
        if(package_cache_error != ALPM_ERR_OK) {
            throw_package_metadata_error(
                PackageMetadataErrorCode::SyncDatabaseUnavailable,
                repository_database_load_failure(
                    package_cache_error));
        }

        repositories.push_back(Impl::RegisteredRepository{
            repository_name, database, package_cache});
    }

    auto impl = std::make_unique<Impl>(std::move(handle), std::move(repositories));
    return RepositoryPackageMetadataSession(std::move(impl));
}

RepositoryPackageQueryResult RepositoryPackageMetadataSession::query_repository_package(
    const RepositoryPackageLookup& lookup) const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Repository package metadata session is not open."));
    }
    if(!is_valid_package_name(lookup.package_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }

    if(lookup.exact_repository_name.has_value()) {
        for(const auto& repository : impl_->repositories) {
            if(repository.repository_name != lookup.exact_repository_name.value()) continue;
            return query_repository_database(
                impl_->handle.get(), repository.database,
                repository.repository_name, lookup.package_name);
        }
        return query_failure(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            localization::translate_message(
                "Requested repository is not configured."));
    }

    for(const auto& repository : impl_->repositories) {
        RepositoryPackageQueryResult result = query_repository_database(
            impl_->handle.get(), repository.database,
            repository.repository_name, lookup.package_name);
        if(std::holds_alternative<PackageNotFound>(result)) continue;
        return result;
    }
    return PackageNotFound{};
}

RepositoryExactPackageMetadataQueryResult
RepositoryPackageMetadataSession::query_repository_exact_package_metadata(
    const std::string& package_name) const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Repository package metadata session is not open."));
    }
    if(!is_valid_package_name(package_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }
    if(impl_->repositories.empty()) {
        return query_failure(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            localization::translate_message(
                "Requested repository is not configured."));
    }

    RepositoryExactPackageMetadataSnapshot snapshot;
    snapshot.repository_order.reserve(impl_->repositories.size());
    snapshot.source_results.reserve(impl_->repositories.size());
    for(std::size_t repository_order = 0;
        repository_order < impl_->repositories.size();
        ++repository_order) {
        const auto& repository = impl_->repositories[repository_order];
        snapshot.repository_order.push_back(repository.repository_name);

        alpm_pkg_t* package = alpm_db_get_pkg(
            repository.database, package_name.c_str());
        if(package == nullptr) {
            const alpm_errno_t query_error = alpm_errno(impl_->handle.get());
            if(query_error == ALPM_ERR_PKG_NOT_FOUND) {
                snapshot.source_results.push_back(
                    RepositoryExactPackageMetadataNotFound{
                        repository_order,
                        repository.repository_name,
                        package_name});
            } else {
                snapshot.source_results.push_back(
                    RepositoryExactPackageMetadataSourceFailure{
                        repository_order,
                        repository.repository_name,
                        package_name,
                        query_failure(
                            PackageMetadataErrorCode::QueryFailed,
                            repository_package_query_failure(
                                query_error))});
            }
            continue;
        }

        const char* returned_name = alpm_pkg_get_name(package);
        if(returned_name == nullptr || package_name != returned_name) {
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    repository_order,
                    repository.repository_name,
                    package_name,
                    query_failure(
                        PackageMetadataErrorCode::MalformedMetadata,
                        localization::translate_message(
                            "Repository package metadata contains an invalid package name."))});
            continue;
        }

        RepositoryPackageBaseSnapshotResult package_base_result =
            snapshot_repository_package_base(package);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&package_base_result);
           failure != nullptr) {
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    repository_order,
                    repository.repository_name,
                    package_name,
                    *failure});
            continue;
        }

        RepositoryPackageArchitectureSnapshotResult architecture_result =
            snapshot_repository_package_architecture(package);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&architecture_result);
           failure != nullptr) {
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    repository_order,
                    repository.repository_name,
                    package_name,
                    *failure});
            continue;
        }

        RepositoryProvidedPackageMetadataResult provides_result =
            snapshot_package_provides(package);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&provides_result);
           failure != nullptr) {
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    repository_order,
                    repository.repository_name,
                    package_name,
                    *failure});
            continue;
        }

        const char* package_version = alpm_pkg_get_version(package);
        snapshot.source_results.push_back(RepositoryExactPackageMetadata{
            repository_order,
            repository.repository_name,
            returned_name,
            std::get<std::string>(std::move(package_base_result)),
            package_version == nullptr
                ? std::nullopt
                : std::optional<std::string>(package_version),
            std::move(std::get<std::vector<RepositoryProvidedPackageMetadata>>(
                provides_result)),
            std::get<std::string>(std::move(architecture_result))});
    }
    return snapshot;
}

RepositoryProviderPackageMetadataQueryResult
RepositoryPackageMetadataSession::query_repository_provider_package_metadata(
    const std::string& dependency_name) const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Repository package metadata session is not open."));
    }
    if(!is_valid_package_name(dependency_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }
    if(impl_->repositories.empty()) {
        return query_failure(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            localization::translate_message(
                "Requested repository is not configured."));
    }

    RepositoryProviderPackageMetadataSnapshot snapshot;
    snapshot.repository_order.reserve(impl_->repositories.size());
    snapshot.source_results.reserve(impl_->repositories.size());
    for(std::size_t repository_order = 0;
        repository_order < impl_->repositories.size();
        ++repository_order) {
        const auto& repository = impl_->repositories[repository_order];
        snapshot.repository_order.push_back(repository.repository_name);
        RepositoryProviderPackageMetadataSourceSnapshot source{
            repository_order, repository.repository_name, {}};
        std::optional<PackageMetadataFailure> source_failure;

        alpm_list_t* package_cache = repository.package_cache;

        for(alpm_list_t* node = package_cache;
            node != nullptr && !source_failure.has_value();
            node = node->next) {
            if(node->data == nullptr) {
                source_failure = query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Repository package metadata contains an invalid package record."));
                break;
            }

            auto* package = static_cast<alpm_pkg_t*>(node->data);
            RepositoryProvidedPackageMetadataResult provides_result =
                snapshot_package_provides(package);
            if(const auto* failure =
                   std::get_if<PackageMetadataFailure>(&provides_result);
               failure != nullptr) {
                source_failure = *failure;
                break;
            }

            std::vector<RepositoryProvidedPackageMetadata> provides =
                std::get<std::vector<RepositoryProvidedPackageMetadata>>(
                    std::move(provides_result));
            const bool matches = std::any_of(
                provides.begin(), provides.end(),
                [&dependency_name](
                    const RepositoryProvidedPackageMetadata& provided) {
                    return provided.package_name == dependency_name;
                });
            if(!matches) continue;

            const char* package_name = alpm_pkg_get_name(package);
            if(package_name == nullptr ||
               !is_valid_package_name(package_name)) {
                source_failure = query_failure(
                    PackageMetadataErrorCode::MalformedMetadata,
                    localization::translate_message(
                        "Repository package metadata contains an invalid package name."));
                break;
            }
            RepositoryPackageBaseSnapshotResult package_base_result =
                snapshot_repository_package_base(package);
            if(const auto* failure =
                   std::get_if<PackageMetadataFailure>(
                       &package_base_result);
               failure != nullptr) {
                source_failure = *failure;
                break;
            }
            RepositoryPackageArchitectureSnapshotResult architecture_result =
                snapshot_repository_package_architecture(package);
            if(const auto* failure =
                   std::get_if<PackageMetadataFailure>(
                       &architecture_result);
               failure != nullptr) {
                source_failure = *failure;
                break;
            }
            const char* package_version = alpm_pkg_get_version(package);
            source.packages.push_back(RepositoryExactPackageMetadata{
                repository_order,
                repository.repository_name,
                package_name,
                std::get<std::string>(
                    std::move(package_base_result)),
                package_version == nullptr
                    ? std::nullopt
                    : std::optional<std::string>(package_version),
                std::move(provides),
                std::get<std::string>(std::move(architecture_result))});
        }

        if(source_failure.has_value()) {
            snapshot.source_results.push_back(
                RepositoryProviderPackageMetadataSourceFailure{
                    repository_order,
                    repository.repository_name,
                    std::move(source_failure.value())});
        } else {
            snapshot.source_results.push_back(std::move(source));
        }
    }
    return snapshot;
}

RepositoryExactPackageMetadataQueryResult
query_configured_repository_exact_package_metadata(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& package_name) {
    PacmanRepositoryConfiguration normalized_configuration;
    try {
        normalized_configuration =
            normalize_pacman_repository_configuration(configuration);
    } catch(const PackageMetadataError& error) {
        return error.failure();
    }

    if(!is_valid_package_name(package_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }
    if(normalized_configuration.repository_names.empty()) {
        return query_failure(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            localization::translate_message(
                "Requested repository is not configured."));
    }

    RepositoryExactPackageMetadataSnapshot snapshot;
    snapshot.repository_order = normalized_configuration.repository_names;
    snapshot.source_results.reserve(
        normalized_configuration.repository_names.size());

    for(std::size_t configured_order = 0;
        configured_order < normalized_configuration.repository_names.size();
        ++configured_order) {
        const std::string& repository_name =
            normalized_configuration.repository_names[configured_order];
        PacmanRepositoryConfiguration source_configuration{
            normalized_configuration.database_paths,
            {repository_name}};

        std::optional<RepositoryPackageMetadataSession> source_session;
        try {
            source_session.emplace(
                RepositoryPackageMetadataSession::open(
                    source_configuration));
        } catch(const PackageMetadataError& error) {
            if(error.failure().code !=
               PackageMetadataErrorCode::SyncDatabaseUnavailable) {
                return error.failure();
            }
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    configured_order,
                    repository_name,
                    package_name,
                    error.failure()});
            continue;
        }

        RepositoryExactPackageMetadataQueryResult source_query =
            source_session->query_repository_exact_package_metadata(
                package_name);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&source_query);
           failure != nullptr) {
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    configured_order,
                    repository_name,
                    package_name,
                    *failure});
            continue;
        }

        RepositoryExactPackageMetadataSnapshot source_snapshot =
            std::get<RepositoryExactPackageMetadataSnapshot>(
                std::move(source_query));
        if(source_snapshot.source_results.size() != 1) {
            snapshot.source_results.push_back(
                RepositoryExactPackageMetadataSourceFailure{
                    configured_order,
                    repository_name,
                    package_name,
                    query_failure(
                        PackageMetadataErrorCode::QueryFailed,
                        localization::translate_message(
                            "Repository package metadata query returned an invalid source result."))});
            continue;
        }

        RepositoryExactPackageMetadataSourceResult source_result =
            std::move(source_snapshot.source_results.front());
        std::visit(
            [configured_order, &repository_name](auto& result) {
                result.configured_repository_order = configured_order;
                result.repository_name = repository_name;
            },
            source_result);
        snapshot.source_results.push_back(std::move(source_result));
    }
    return snapshot;
}

RepositoryProviderPackageMetadataQueryResult
query_configured_repository_provider_package_metadata(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& dependency_name) {
    PacmanRepositoryConfiguration normalized_configuration;
    try {
        normalized_configuration =
            normalize_pacman_repository_configuration(configuration);
    } catch(const PackageMetadataError& error) {
        return error.failure();
    }

    if(!is_valid_package_name(dependency_name)) {
        return query_failure(
            PackageMetadataErrorCode::InvalidPackageName,
            localization::translate_message("Package name is invalid."));
    }
    if(normalized_configuration.repository_names.empty()) {
        return query_failure(
            PackageMetadataErrorCode::RepositoryNotConfigured,
            localization::translate_message(
                "Requested repository is not configured."));
    }

    RepositoryProviderPackageMetadataSnapshot snapshot;
    snapshot.repository_order = normalized_configuration.repository_names;
    snapshot.source_results.reserve(
        normalized_configuration.repository_names.size());
    for(std::size_t configured_order = 0;
        configured_order < normalized_configuration.repository_names.size();
        ++configured_order) {
        const std::string& repository_name =
            normalized_configuration.repository_names[configured_order];
        PacmanRepositoryConfiguration source_configuration{
            normalized_configuration.database_paths,
            {repository_name}};

        std::optional<RepositoryPackageMetadataSession> source_session;
        try {
            source_session.emplace(
                RepositoryPackageMetadataSession::open(
                    source_configuration));
        } catch(const PackageMetadataError& error) {
            if(error.failure().code !=
               PackageMetadataErrorCode::SyncDatabaseUnavailable) {
                return error.failure();
            }
            snapshot.source_results.push_back(
                RepositoryProviderPackageMetadataSourceFailure{
                    configured_order,
                    repository_name,
                    error.failure()});
            continue;
        }

        RepositoryProviderPackageMetadataQueryResult source_query =
            source_session->query_repository_provider_package_metadata(
                dependency_name);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&source_query);
           failure != nullptr) {
            snapshot.source_results.push_back(
                RepositoryProviderPackageMetadataSourceFailure{
                    configured_order,
                    repository_name,
                    *failure});
            continue;
        }

        RepositoryProviderPackageMetadataSnapshot source_snapshot =
            std::get<RepositoryProviderPackageMetadataSnapshot>(
                std::move(source_query));
        if(source_snapshot.source_results.size() != 1) {
            snapshot.source_results.push_back(
                RepositoryProviderPackageMetadataSourceFailure{
                    configured_order,
                    repository_name,
                    query_failure(
                        PackageMetadataErrorCode::QueryFailed,
                        localization::translate_message(
                            "Repository provider metadata query returned an invalid source result."))});
            continue;
        }

        RepositoryProviderPackageMetadataSourceResult source_result =
            std::move(source_snapshot.source_results.front());
        std::visit(
            [configured_order, &repository_name](auto& result) {
                result.configured_repository_order = configured_order;
                result.repository_name = repository_name;
                if constexpr(requires { result.packages; }) {
                    for(auto& package : result.packages) {
                        package.configured_repository_order =
                            configured_order;
                        package.repository_name = repository_name;
                    }
                }
            },
            source_result);
        snapshot.source_results.push_back(std::move(source_result));
    }
    return snapshot;
}

RepositoryPackageSearchResult
RepositoryPackageMetadataSession::query_root_package_search(
    const std::string& query) const {
    if(impl_ == nullptr) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Repository package metadata session is not open."));
    }
    if(query.find('\0') != std::string::npos) {
        return query_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Repository package search query is invalid."));
    }

    RepositoryPackageSearchSnapshot snapshot;
    snapshot.repository_order.reserve(impl_->repositories.size());
    for(const auto& repository : impl_->repositories) {
        snapshot.repository_order.push_back(repository.repository_name);
    }

    std::string query_needle = query;
    alpm_list_t query_node{query_needle.data(), nullptr, nullptr};
    for(const auto& repository : impl_->repositories) {
        alpm_list_t* raw_search_results = nullptr;
        int search_status = alpm_db_search(
            repository.database, &query_node, &raw_search_results);
        UniqueAlpmList search_results(raw_search_results);
        if(search_status != 0) {
            return query_failure(
                PackageMetadataErrorCode::QueryFailed,
                repository_package_search_failure(
                    alpm_errno(impl_->handle.get())));
        }

        for(alpm_list_t* node = search_results.get();
            node != nullptr;
            node = node->next) {
            auto match_result = make_repository_package_search_match(
                static_cast<alpm_pkg_t*>(node->data),
                repository.repository_name,
                RepositoryPackageSearchMatchKind::Search,
                std::nullopt);
            if(const auto* failure =
                   std::get_if<PackageMetadataFailure>(&match_result);
               failure != nullptr) {
                return *failure;
            }
            snapshot.matches.push_back(
                std::get<RepositoryPackageSearchMatch>(
                    std::move(match_result)));
        }
    }

    bool is_exact_group = false;
    for(const auto& repository : impl_->repositories) {
        alpm_group_t* group =
            alpm_db_get_group(repository.database, query.c_str());
        // POLICY: package cache preload後のnullptrはexact group missである。
        // libalpmのhandle errnoは以前の失敗を保持し得るため参照しない。
        if(group == nullptr) continue;

        if(group->name == nullptr || query != group->name) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Repository package group metadata contains an invalid group name."));
        }
        is_exact_group = true;
    }

    if(!is_exact_group) return snapshot;

    std::vector<alpm_list_t> database_nodes(impl_->repositories.size());
    for(std::size_t index = 0; index < impl_->repositories.size(); ++index) {
        database_nodes[index].data = impl_->repositories[index].database;
        database_nodes[index].prev =
            index == 0 ? nullptr : &database_nodes[index - 1];
        database_nodes[index].next =
            index + 1 == database_nodes.size()
                ? nullptr
                : &database_nodes[index + 1];
    }
    alpm_list_t* database_list =
        database_nodes.empty() ? nullptr : &database_nodes.front();

    alpm_list_t* raw_group_packages =
        alpm_find_group_pkgs(database_list, query.c_str());
    UniqueAlpmList group_packages(raw_group_packages);
    // POLICY: public APIはnullptrでemptyとfailureを区別するstatusを返さない。
    // caller-owned listがない状態をempty group resultとして扱い、stale errnoを
    // metadata failureへ誤変換しない。

    for(alpm_list_t* node = group_packages.get();
        node != nullptr;
        node = node->next) {
        auto* package = static_cast<alpm_pkg_t*>(node->data);
        alpm_db_t* package_database =
            package == nullptr ? nullptr : alpm_pkg_get_db(package);

        const Impl::RegisteredRepository* source_repository = nullptr;
        for(const auto& repository : impl_->repositories) {
            if(repository.database == package_database) {
                source_repository = &repository;
                break;
            }
        }
        if(source_repository == nullptr) {
            return query_failure(
                PackageMetadataErrorCode::MalformedMetadata,
                localization::translate_message(
                    "Repository package group contains an invalid package source."));
        }

        auto match_result = make_repository_package_search_match(
            package,
            source_repository->repository_name,
            RepositoryPackageSearchMatchKind::ExactGroup,
            query);
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&match_result);
           failure != nullptr) {
            return *failure;
        }
        snapshot.matches.push_back(
            std::get<RepositoryPackageSearchMatch>(
                std::move(match_result)));
    }

    return snapshot;
}
