#pragma once

#include "app_config.hpp"
#include "package_metadata.hpp"
#include "source_build.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace system_source_upgrade_test_stub {

enum class EventKind {
    PreferenceDirectorySnapshot,
    SupportedOptionsGuard,
    StrictPreferenceRead,
    SourceIdentityResolution,
    SourceWorkItemPreparation,
    SourceInvocationPreparation,
    MetadataSessionOpen,
    InstalledPackageQuery,
    LocalPackageSnapshot,
    Progress,
    SystemCommand,
    RepositoryProviderTransaction,
    SourcePreparation,
    PackageBaseSourceExecution,
    SourceExecution,
};

struct Event {
    EventKind kind;
    std::string subject;

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
    ConfigSnapshot config;
};

struct SourcePreparationCall {
    std::string package_name;
    std::string package_base;
    SourceBuildRequest request;
    SourceBuildUpdatePolicy update_policy =
        SourceBuildUpdatePolicy::AlwaysBuild;
    ConfigSnapshot config;
};

struct PackageBaseSourceExecutionCall {
    std::string package_name;
    std::string package_base;
    SourceBuildRequest request;
    ConfigSnapshot config;
};

void reset();

void set_preference_directory(SourcePreferenceDirectorySnapshot snapshot);
void fail_preference_directory(std::string diagnostic);
void enqueue_preference_result(
    const std::string& package_name,
    StrictSourcePreferenceResult result);

void set_source_identity(
    const std::string& package_name,
    ResolvedSourceBuildIdentity identity);
void fail_source_identity(
    const std::string& package_name,
    std::string diagnostic);
void fail_source_work_item(
    const std::string& package_name,
    std::string diagnostic);
void set_selected_repository_provider(
    const std::string& package_name,
    ProvidedDependency provider);
void set_provider_candidates(
    const std::string& package_name,
    std::vector<ProvidedDependency> candidates);
void set_provider_selector(ProviderSelectionCallback select_provider);
void set_aur_invocation_plan(BuildPlan plan);
void fail_build_plan_guard(std::string diagnostic);
void fail_repository_provider_transaction(std::string diagnostic);
void fail_source_invocation(std::string diagnostic);
void fail_supported_options(std::string diagnostic);
void fail_cache_seed();
void fail_cache_activation();
void fail_repository_provider_cache_revalidation();

void enqueue_metadata_session(MetadataSessionScript script);

void set_system_command_exit_status(int exit_status);
void fail_system_command(std::string diagnostic);
void enqueue_source_success(SourceBuildExecutionResult result);
void enqueue_source_failure(std::string diagnostic);
void enqueue_source_exception(std::exception_ptr exception);
void enqueue_source_cache_failure();
void enqueue_source_cleanup_failure(
    ArtifactInstallExecutionOutcome outcome,
    std::string diagnostic);
void enqueue_source_unknown_failure();
void enqueue_package_base_success(
    std::string package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts = {},
    ProductionSourceBuildProvenance source_provenance = {});
void enqueue_package_base_cleanup_failure(
    std::string package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    std::string diagnostic);
void enqueue_package_base_selection_failure(
    PackageBaseArtifactIdentitySelectionFailure failure,
    std::string diagnostic);
void enqueue_package_base_mixed_reason_failure(
    MixedPackageBaseInstallReasonUnsupported failure,
    std::string diagnostic);
void enqueue_package_base_phase_failure(
    SeparatedPackageBaseSourceBuildFailurePhase phase,
    std::string diagnostic);
void enqueue_package_base_transaction_failure(
    PackageBaseArtifactInstallTransactionFailureKind failure_kind,
    std::string package_base,
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts,
    std::optional<int> exit_code,
    std::string diagnostic);
void enqueue_package_base_metadata_failure(
    PackageMetadataFailure failure);

void record_progress(const std::string& subject);

const std::vector<Event>& events();
const std::vector<ConfigSnapshot>& supported_option_configs();
const std::vector<ConfigSnapshot>& invocation_configs();
const std::vector<SourceExecutionCall>& source_execution_calls();
const std::vector<SourcePreparationCall>& source_preparation_calls();
const std::vector<PackageBaseSourceExecutionCall>&
package_base_source_execution_calls();
const std::vector<std::string>& system_commands();

void require_script_consumed();

} // namespace system_source_upgrade_test_stub
