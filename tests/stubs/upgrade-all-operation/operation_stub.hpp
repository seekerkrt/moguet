#pragma once

#include "app_config.hpp"
#include "artifact_install_executor.hpp"
#include "aur_rpc.hpp"
#include "dependency_plan.hpp"
#include "installed_package_relation_inventory.hpp"
#include "package_metadata.hpp"
#include "repository_query.hpp"
#include "source_build.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// upgrade-all operationのproduction orchestrationをlinkしたtestから、外部I/Oと
// mutationだけを決定的に差し替える。全phaseは1本のevent historyを共有し、
// aggregateがsystem/source完了前にinventory/AURへ進んでいないことも観測できる。
namespace upgrade_all_operation_test_stub {

enum class EventKind {
    PreferenceDirectorySnapshot,
    StrictPreferenceRead,
    SourceIdentityResolution,
    SourceWorkItemPreparation,
    SeparatedInstallOptionsGuard,
    ArtifactPkgdestGuard,
    PacmanDatabaseResolution,
    MetadataSessionOpen,
    InstalledPackageQuery,
    LocalPackageSnapshot,
    Progress,
    SystemCommand,
    SourcePreparation,
    PackageBaseSourceExecution,
    SourceExecution,
    RepositoryConfigurationResolution,
    ForeignInventoryQuery,
    InstalledRelationInventoryQuery,
    InstalledRuntimeDependencyInventoryQuery,
    RepositoryCandidateQuery,
    AurInfoMany,
    AurInfoStrict,
    VersionCompare,
    BuildPlanResolution,
    AurCheckout,
    AurBuild,
    AurInstall,
    AurCleanup,
};

struct Event {
    EventKind kind;
    std::string subject;
    std::vector<std::string> package_names;

    bool operator==(const Event&) const = default;
};

struct ConfigSnapshot {
    bool no_edit = false;
    bool no_diff = false;
    bool no_confirm = false;
    bool rebuild = false;
    bool clean_build = false;
    bool rm_deps = false;
    std::string editor;

    bool operator==(const ConfigSnapshot&) const = default;
};

struct MetadataSessionScript {
    std::optional<PackageMetadataFailure> open_failure;
    LocalPackageVersionSnapshotResult local_package_snapshot =
        LocalPackageVersionSnapshot{};
    std::map<std::string, InstalledPackageQueryResult>
        installed_package_results;
};

struct SourceExecutionCall {
    std::string package_name;
    std::string package_base;
    SourceBuildRequest request;
    PacmanDatabasePaths database_paths;
    ConfigSnapshot config;
};

struct SourcePreparationCall {
    std::string package_name;
    std::string package_base;
    SourceBuildRequest request;
    SourceBuildUpdatePolicy update_policy =
        SourceBuildUpdatePolicy::AlwaysBuild;
    PacmanDatabasePaths database_paths;
    ConfigSnapshot config;
};

struct PackageBaseSourceExecutionCall {
    std::string package_name;
    std::string package_base;
    SourceBuildRequest request;
    PacmanDatabasePaths database_paths;
    ConfigSnapshot config;
};

struct AurExecutionCall {
    std::size_t call_index = 0;
    // singular compatibility field。multiple PackageBaseではempty。
    std::string package_name;
    std::string package_base;
    std::vector<std::string> plan_package_names;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    PacmanDatabasePaths database_paths;
    ConfigSnapshot config;
    std::vector<EventKind> lifecycle_events;
};

using ResolverHandler =
    std::function<BuildPlan(
        const std::vector<std::string>& targets,
        const ProviderSelectionCallback& select_provider)>;

void reset();

// Registered source preparation.
void set_preference_directory(SourcePreferenceDirectorySnapshot snapshot);
void fail_preference_directory(std::string diagnostic);
void enqueue_preference_result(
    const std::string& package_name,
    StrictSourcePreferenceResult result);
// PR3 preparation stubの既存名。上と同じcross-phase queueへ追加する。
void enqueue_source_preference_result(
    const std::string& package_name,
    StrictSourcePreferenceResult result);
void set_after_next_strict_preference_read_hook(std::function<void()> hook);

void set_source_identity(
    const std::string& package_name,
    ResolvedSourceBuildIdentity identity);
void fail_source_identity(
    const std::string& package_name,
    std::string diagnostic);
void fail_source_work_item(
    const std::string& package_name,
    std::string diagnostic);

// Package metadata used by the system/source phase.
void enqueue_metadata_session(MetadataSessionScript script);

// System mutation and registered-source lifecycle.
void set_system_command_exit_status(int exit_status);
void fail_system_command(std::string diagnostic);
void set_after_system_command_hook(std::function<void()> hook);
void fail_cache_seed();
void set_after_next_cache_seed_hook(std::function<void()> hook);
void fail_cache_activation();
void enqueue_source_success(SourceBuildExecutionResult result);
void enqueue_package_base_source_success(
    std::string package_base,
    ArtifactPackageIdentity selected_child,
    std::vector<ArtifactPackageIdentity> unselected_artifacts);
void enqueue_source_failure(std::string diagnostic);
void enqueue_source_cache_failure();
void enqueue_source_cleanup_failure(
    ArtifactInstallExecutionOutcome outcome,
    std::string diagnostic);
void enqueue_source_unknown_failure();

// Fresh foreign inventory and AUR query transport.
void set_repository_configuration(
    PacmanRepositoryConfiguration configuration);
void set_repository_configuration_failure(PackageMetadataFailure failure);
void set_foreign_inventory(ForeignPackageInventory inventory);
void set_foreign_inventory_failure(PackageMetadataFailure failure);
void set_after_foreign_inventory_hook(std::function<void()> hook);
void set_installed_relation_inventory(
    InstalledPackageRelationInventoryResult inventory);
void set_installed_runtime_dependency_inventory(
    InstalledPackageRuntimeDependencyMetadataInventoryResult inventory);
void set_repository_candidate_result(
    const std::string& package_name,
    StrictRepositoryPackageQueryResult result);

void enqueue_info_many_result(
    std::map<std::string, AurPackageInfo> result);
void set_after_info_many_hook(std::function<void()> hook);
void enqueue_info_many_failure(std::string diagnostic);
void enqueue_info_many_response_failure(std::string diagnostic);
void enqueue_info_strict_result(std::optional<AurPackageInfo> result);
void enqueue_info_strict_failure(std::string diagnostic);
void enqueue_info_strict_response_failure(std::string diagnostic);
void enqueue_vercmp_result(std::string output);
void enqueue_vercmp_failure(std::string diagnostic);

// BuildPlan resolver used by actual PR3 preflight.
void set_resolver_handler(ResolverHandler handler);

// Generic source-build preparation I/O used by actual
// source_install_preparation.cpp in both PR2 and PR3.
void set_database_paths(PacmanDatabasePaths paths);
void set_database_failure(PackageMetadataFailure failure);
void enqueue_database_paths(PacmanDatabasePaths paths);
void enqueue_database_failure(PackageMetadataFailure failure);
void fail_supported_options_guard(std::string diagnostic);
void fail_supported_options(std::string diagnostic);
void fail_source_invocation(std::string diagnostic);
void fail_pkgdest_guard_on_call(
    std::size_t one_based_call_index,
    std::string diagnostic);

// AUR source-build lifecycle used by actual PR3 runner.
void enqueue_aur_success(ArtifactInstallExecutionOutcome outcome);
void enqueue_aur_successes(
    std::vector<ArtifactInstallExecutionOutcome> child_outcomes,
    std::vector<ArtifactPackageIdentity> unselected_artifacts = {});
void enqueue_aur_cancellation();
void enqueue_aur_ordinary_failure(std::string diagnostic);
void enqueue_aur_cleanup_failure(
    ArtifactInstallExecutionOutcome outcome,
    std::string diagnostic);
void enqueue_aur_cleanup_failure(
    std::vector<ArtifactInstallExecutionOutcome> child_outcomes,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    std::string diagnostic);
void enqueue_aur_unknown_failure();

// Aggregate observer向けの共通history hook。
void record_progress(const std::string& subject);

const std::vector<Event>& event_history();
const std::vector<Event>& events();
const std::vector<SourceExecutionCall>& source_execution_calls();
const std::vector<SourcePreparationCall>& source_preparation_calls();
const std::vector<PackageBaseSourceExecutionCall>&
package_base_source_execution_calls();
const std::vector<AurExecutionCall>& aur_execution_calls();
const std::vector<std::string>& system_commands();

std::size_t repository_configuration_calls();
std::size_t inventory_calls();
std::size_t installed_relation_inventory_calls();
std::size_t installed_runtime_dependency_inventory_calls();
const std::vector<PacmanRepositoryConfiguration>&
inventory_configuration_history();
const std::vector<std::string>& repository_candidate_call_history();
const std::vector<std::vector<std::string>>& info_many_call_history();
const std::vector<std::string>& info_strict_call_history();
const std::vector<std::string>& vercmp_call_history();

std::size_t resolver_call_count();
const std::vector<std::vector<std::string>>& resolver_calls();

const std::vector<bool>& supported_options_guard_history();
const std::vector<SourceBuildEnvironment>& pkgdest_guard_history();
std::size_t database_call_count();
const std::vector<std::string>& strict_preference_read_history();

void require_script_consumed();

} // namespace upgrade_all_operation_test_stub
