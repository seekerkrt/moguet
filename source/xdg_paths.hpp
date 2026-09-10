#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace xdg_paths {

enum class DirectoryKind {
    Config,
    State,
    Cache,
};

enum class EnvironmentVariable {
    XdgConfigHome,
    XdgStateHome,
    XdgCacheHome,
    Home,
};

enum class DirectorySource {
    ExplicitXdg,
    HomeFallback,
};

enum class ResolutionErrorCode {
    MissingHome,
    EmptyHome,
    RelativePath,
    DotComponent,
    AmbiguousLeadingDoubleSlash,
    EmbeddedNull,
    MalformedPath,
};

struct ResolutionFailure {
    DirectoryKind directory_kind;
    EnvironmentVariable environment_variable;
    ResolutionErrorCode code;
};

class ResolutionError final : public std::runtime_error {
    ResolutionFailure failure_;

public:
    explicit ResolutionError(ResolutionFailure failure);

    const ResolutionFailure& failure() const noexcept {
        return failure_;
    }
};

// unsetとdefined-emptyを区別し、environment値をowning copyとして保持する。
struct EnvironmentSnapshot {
    std::optional<std::string> xdg_config_home;
    std::optional<std::string> xdg_state_home;
    std::optional<std::string> xdg_cache_home;
    std::optional<std::string> home;
};

// Directory safety capabilityがenvironmentやpath suffixを再解決せずに使う、
// resolver-ownedの作成境界。existing_anchor自体は作成対象に含めない。
struct DirectoryCreationBoundary {
    DirectorySource source = DirectorySource::ExplicitXdg;
    std::filesystem::path base_directory;
    std::filesystem::path existing_anchor;
    std::vector<std::string> creatable_components;
};

struct ConfigPaths {
    std::filesystem::path directory;
    std::filesystem::path config_file;
    DirectoryCreationBoundary creation_boundary;
};

struct SourcePreferencePaths {
    std::filesystem::path directory;
    DirectoryCreationBoundary creation_boundary;
};

struct StatePaths {
    std::filesystem::path directory;
    std::filesystem::path default_log_file;
    DirectoryCreationBoundary creation_boundary;
};

// Resolver-owned XDG state subdirectory. The semantic resolver fixes the
// managed components; low-level filesystem consumers must not derive them
// from an untrusted record key.
struct StateStorePaths {
    std::filesystem::path directory;
    DirectoryCreationBoundary creation_boundary;
    std::vector<std::string> managed_components;
};

using ReviewedSourceStatePaths = StateStorePaths;
using DevelBuildProvenancePaths = StateStorePaths;

struct CachePaths {
    std::filesystem::path directory;
    DirectoryCreationBoundary creation_boundary;
};

struct ResolvedPaths {
    ConfigPaths config;
    StatePaths state;
    CachePaths cache;
};

// Pure resolver。filesystemの参照・作成やcurrent working directory補正を行わない。
ResolvedPaths resolve(const EnvironmentSnapshot& environment);

// Config consumerが無関係なstate/cache environmentをauthorityへ
// 取り込まず、config pathだけを解決するpure resolver。
ConfigPaths resolve_config(const EnvironmentSnapshot& environment);

// Source-build preference consumerがconfig authorityだけから
// moguet/source-build.dを解決するpure resolver。
SourcePreferencePaths resolve_source_preference(
    const EnvironmentSnapshot& environment);

// State log consumerが無関係なconfig/cache environmentをauthorityへ
// 取り込まず、state pathだけを解決するpure resolver。
StatePaths resolve_state(const EnvironmentSnapshot& environment);

// Cache consumerが無関係なconfig/state environmentをauthorityへ
// 取り込まず、cache pathだけを解決するpure resolver。
CachePaths resolve_cache(const EnvironmentSnapshot& environment);

// Production adapter。実processのXDG_CONFIG_HOME / XDG_STATE_HOME /
// XDG_CACHE_HOME / HOMEだけをsnapshot化し、pure resolverへ渡す。
ResolvedPaths resolve_process_environment();

// Config consumer専用adapter。XDG_CONFIG_HOME / HOMEだけをsnapshot化する。
ConfigPaths resolve_config_process_environment();

// Source-build preference専用adapter。XDG_CONFIG_HOME / HOMEだけをsnapshot化する。
SourcePreferencePaths resolve_source_preference_process_environment();

// Reviewed-source store consumerが無関係なconfig/cache environmentを
// authorityへ取り込まず、state pathだけを解決するpure resolver。
ReviewedSourceStatePaths resolve_reviewed_source_state(
    const EnvironmentSnapshot& environment);

// Devel build provenance uses a separate state namespace from reviewed source
// state. PackageBase leaves remain the semantic store's responsibility.
DevelBuildProvenancePaths resolve_devel_build_provenance(
    const EnvironmentSnapshot& environment);

// Default state log専用adapter。XDG_STATE_HOME / HOMEだけをsnapshot化する。
StatePaths resolve_state_process_environment();

// Reviewed-source store専用adapter。XDG_STATE_HOME / HOMEだけをsnapshot化する。
ReviewedSourceStatePaths resolve_reviewed_source_state_process_environment();

// Devel-build provenance store adapter. Only XDG_STATE_HOME / HOME are read.
DevelBuildProvenancePaths
resolve_devel_build_provenance_process_environment();

// Cache consumer専用adapter。XDG_CACHE_HOME / HOMEだけをsnapshot化する。
CachePaths resolve_cache_process_environment();

} // namespace xdg_paths
