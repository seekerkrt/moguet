#include "phase_stub.hpp"

#include "artifact_install_executor.hpp"
#include "dependency_plan.hpp"
#include "separated_source_build.hpp"

#include <algorithm>
#include <deque>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

namespace stub = system_source_upgrade_test_stub;

enum class ScriptedSourceExecutionKind {
    Success,
    Failure,
    CacheFailure,
    CleanupFailure,
    UnknownFailure,
    PackageBaseSuccess,
    PackageBaseCleanupFailure,
    PackageBaseSelectionFailure,
    PackageBaseMixedReasonFailure,
    PackageBasePhaseFailure,
    PackageBaseTransactionFailure,
    PackageBaseMetadataFailure,
};

struct ScriptedSourceExecution {
    ScriptedSourceExecutionKind kind =
        ScriptedSourceExecutionKind::Success;
    SourceBuildExecutionResult result;
    ArtifactInstallExecutionOutcome cleanup_outcome =
        ArtifactInstallExecutionOutcome::Installed;
    std::string package_base;
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
    ProductionSourceBuildProvenance source_provenance;
    PackageBaseArtifactIdentitySelectionFailure selection_failure;
    MixedPackageBaseInstallReasonUnsupported mixed_reason_failure;
    SeparatedPackageBaseSourceBuildFailurePhase phase =
        SeparatedPackageBaseSourceBuildFailurePhase::Build;
    PackageBaseArtifactInstallTransactionFailureKind transaction_failure_kind =
        PackageBaseArtifactInstallTransactionFailureKind::UnknownException;
    std::vector<PackageBaseArtifactInstallTransactionAttempt>
        transaction_attempts;
    std::optional<int> transaction_exit_code;
    PackageMetadataFailure metadata_failure{
        PackageMetadataErrorCode::QueryFailed, {}};
    std::string diagnostic;
};

struct PhaseStubState {
    SourcePreferenceDirectorySnapshot preference_directory;
    std::optional<std::string> preference_directory_failure;
    std::map<std::string, std::deque<StrictSourcePreferenceResult>>
        preference_results;
    std::map<std::string, ResolvedSourceBuildIdentity> identities;
    std::map<std::string, std::string> identity_failures;
    std::map<std::string, std::string> work_item_failures;
    std::map<std::string, std::vector<ProvidedDependency>>
        selected_repository_providers;
    std::map<std::string, std::vector<ProvidedDependency>>
        provider_candidates;
    ProviderSelectionCallback provider_selector;
    std::optional<BuildPlan> aur_invocation_plan;
    std::optional<std::string> build_plan_guard_failure;
    std::optional<std::string> repository_provider_transaction_failure;
    std::optional<std::string> invocation_failure;
    std::optional<std::string> supported_options_failure;
    bool cache_seed_failure = false;
    bool cache_activation_failure = false;
    bool repository_provider_cache_revalidation_failure = false;
    std::deque<stub::MetadataSessionScript> metadata_sessions;
    int system_exit_status = 0;
    std::optional<std::string> system_failure;
    std::deque<ScriptedSourceExecution> source_executions;

    std::vector<stub::Event> events;
    std::vector<stub::ConfigSnapshot> supported_option_configs;
    std::vector<stub::ConfigSnapshot> invocation_configs;
    std::vector<stub::SourceExecutionCall> source_calls;
    std::vector<stub::SourcePreparationCall> source_preparation_calls;
    std::vector<stub::PackageBaseSourceExecutionCall>
        package_base_source_calls;
    std::vector<std::string> system_commands;
    std::optional<std::string> expectation_failure;
};

struct UnknownSourceExecutionFailure {};

PhaseStubState g_state;

stub::ConfigSnapshot snapshot_config(const AppConfig& config) {
    return stub::ConfigSnapshot{
        config.user_config.review.pkgbuild == ReviewPolicy::Skip,
        config.user_config.review.diff == ReviewPolicy::Skip,
        config.no_confirm,
        config.user_config.build.mode == BuildMode::Rebuild,
        config.user_config.build.mode == BuildMode::Clean,
        config.rm_deps,
        config.editor};
}

[[noreturn]] void fail_unexpected(const std::string& diagnostic) {
    g_state.expectation_failure = diagnostic;
    throw std::logic_error(diagnostic);
}

ResolvedSourceBuildIdentity default_identity(
    const std::string& package_name) {
    return ResolvedSourceBuildIdentity{
        ResolvedRepositorySourceBuildIdentity{
            RepositoryPackagePresent{
                "core", 0, package_name, package_name}}};
}

} // namespace

bool SourceBuildEnvironment::defines(const std::string& key) const {
    return std::any_of(ordered_assignments.begin(), ordered_assignments.end(),
                       [&](const auto& assignment) { return assignment.key == key; });
}

ProviderSelectionCallback provider_selection_callback(const AppConfig&) {
    return g_state.provider_selector;
}

void activate_production_source_build_cache(
    PreparedProductionSourceBuildInvocation&) {
    // Phase tests isolate cache/filesystem mutation behind this seam.
    if(g_state.cache_activation_failure) {
        throw TrustedCacheError(TrustedCacheFailure{
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::ConcurrentReplacement,
            std::nullopt});
    }
}

SelectedRepositoryProviderTransactionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig&) {
    SelectedRepositoryProviderTransactionResult result;
    result.selected_providers = invocation.selected_repository_providers;
    if(result.selected_providers.empty()) return result;
    if(g_state.repository_provider_cache_revalidation_failure) {
        throw TrustedCacheError(TrustedCacheFailure{
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::ConcurrentReplacement,
            std::nullopt});
    }
    g_state.events.push_back(stub::Event{
        stub::EventKind::RepositoryProviderTransaction,
        "selected-repository-providers"});
    if(g_state.repository_provider_transaction_failure.has_value()) {
        result.status =
            SelectedRepositoryProviderTransactionStatus::Failed;
        result.package_state_change = PackageStateChange::Unknown;
        result.diagnostic =
            *g_state.repository_provider_transaction_failure;
        return result;
    }
    result.status = SelectedRepositoryProviderTransactionStatus::Succeeded;
    result.package_state_change = PackageStateChange::Unknown;
    result.command_exit_status = 0;
    return result;
}

void seed_production_source_build_cache(
    PreparedProductionSourceBuildInvocation& invocation,
    const ValidatedCacheRoot& cache_root) {
    if(g_state.cache_seed_failure) {
        throw TrustedCacheError(TrustedCacheFailure{
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::ConcurrentReplacement,
            std::nullopt});
    }
    cache_root.require_unchanged_identity();
    invocation.cache_root = cache_root;
    for(auto& work_item : invocation.work_items) {
        work_item.cache_root = cache_root;
    }
}

namespace system_source_upgrade_test_stub {

void reset() {
    g_state = PhaseStubState{};
}

void set_preference_directory(SourcePreferenceDirectorySnapshot snapshot) {
    g_state.preference_directory = std::move(snapshot);
    g_state.preference_directory_failure.reset();
}

void fail_preference_directory(std::string diagnostic) {
    g_state.preference_directory_failure = std::move(diagnostic);
}

void enqueue_preference_result(
    const std::string& package_name,
    StrictSourcePreferenceResult result) {
    g_state.preference_results[package_name].push_back(std::move(result));
}

void set_source_identity(
    const std::string& package_name,
    ResolvedSourceBuildIdentity identity) {
    g_state.identities.insert_or_assign(
        package_name, std::move(identity));
    g_state.identity_failures.erase(package_name);
}

void fail_source_identity(
    const std::string& package_name,
    std::string diagnostic) {
    g_state.identity_failures[package_name] = std::move(diagnostic);
}

void fail_source_work_item(
    const std::string& package_name,
    std::string diagnostic) {
    g_state.work_item_failures[package_name] = std::move(diagnostic);
}

void set_selected_repository_provider(
    const std::string& package_name,
    ProvidedDependency provider) {
    g_state.selected_repository_providers[package_name].push_back(
        std::move(provider));
}

void set_provider_candidates(
    const std::string& package_name,
    std::vector<ProvidedDependency> candidates) {
    g_state.provider_candidates[package_name] = std::move(candidates);
}

void set_provider_selector(ProviderSelectionCallback select_provider) {
    g_state.provider_selector = std::move(select_provider);
}

void set_aur_invocation_plan(BuildPlan plan) {
    g_state.aur_invocation_plan.emplace(std::move(plan));
}

void fail_build_plan_guard(std::string diagnostic) {
    g_state.build_plan_guard_failure = std::move(diagnostic);
}

void fail_repository_provider_transaction(std::string diagnostic) {
    g_state.repository_provider_transaction_failure =
        std::move(diagnostic);
}

void fail_source_invocation(std::string diagnostic) {
    g_state.invocation_failure = std::move(diagnostic);
}

void fail_supported_options(std::string diagnostic) {
    g_state.supported_options_failure = std::move(diagnostic);
}

void fail_cache_seed() {
    g_state.cache_seed_failure = true;
}

void fail_cache_activation() {
    g_state.cache_activation_failure = true;
}

void fail_repository_provider_cache_revalidation() {
    g_state.repository_provider_cache_revalidation_failure = true;
}

void enqueue_metadata_session(MetadataSessionScript script) {
    g_state.metadata_sessions.push_back(std::move(script));
}

void set_system_command_exit_status(int exit_status) {
    g_state.system_exit_status = exit_status;
    g_state.system_failure.reset();
}

void fail_system_command(std::string diagnostic) {
    g_state.system_failure = std::move(diagnostic);
}

void enqueue_source_success(SourceBuildExecutionResult result) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::Success;
    execution.result = std::move(result);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_source_failure(std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::Failure;
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_source_cache_failure() {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::CacheFailure;
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_source_cleanup_failure(
    ArtifactInstallExecutionOutcome outcome,
    std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::CleanupFailure;
    execution.cleanup_outcome = outcome;
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_source_unknown_failure() {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::UnknownFailure;
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_success(
    std::string package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    ProductionSourceBuildProvenance source_provenance) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::PackageBaseSuccess;
    execution.package_base = std::move(package_base);
    execution.selected_children = std::move(selected_children);
    execution.unselected_artifacts = std::move(unselected_artifacts);
    execution.source_provenance = std::move(source_provenance);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_cleanup_failure(
    std::string package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::PackageBaseCleanupFailure;
    execution.package_base = std::move(package_base);
    execution.selected_children = std::move(selected_children);
    execution.unselected_artifacts = std::move(unselected_artifacts);
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_selection_failure(
    PackageBaseArtifactIdentitySelectionFailure failure,
    std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::PackageBaseSelectionFailure;
    execution.selection_failure = std::move(failure);
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_mixed_reason_failure(
    MixedPackageBaseInstallReasonUnsupported failure,
    std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind =
        ScriptedSourceExecutionKind::PackageBaseMixedReasonFailure;
    execution.mixed_reason_failure = std::move(failure);
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_phase_failure(
    SeparatedPackageBaseSourceBuildFailurePhase phase,
    std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::PackageBasePhaseFailure;
    execution.phase = phase;
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_transaction_failure(
    PackageBaseArtifactInstallTransactionFailureKind failure_kind,
    std::string package_base,
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts,
    std::optional<int> exit_code,
    std::string diagnostic) {
    ScriptedSourceExecution execution;
    execution.kind =
        ScriptedSourceExecutionKind::PackageBaseTransactionFailure;
    execution.transaction_failure_kind = failure_kind;
    execution.package_base = std::move(package_base);
    execution.transaction_attempts = std::move(attempts);
    execution.transaction_exit_code = exit_code;
    execution.diagnostic = std::move(diagnostic);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_metadata_failure(
    PackageMetadataFailure failure) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::PackageBaseMetadataFailure;
    execution.metadata_failure = std::move(failure);
    g_state.source_executions.push_back(std::move(execution));
}

void record_progress(const std::string& subject) {
    g_state.events.push_back(Event{EventKind::Progress, subject});
}

const std::vector<Event>& events() {
    return g_state.events;
}

const std::vector<ConfigSnapshot>& supported_option_configs() {
    return g_state.supported_option_configs;
}

const std::vector<ConfigSnapshot>& invocation_configs() {
    return g_state.invocation_configs;
}

const std::vector<SourceExecutionCall>& source_execution_calls() {
    return g_state.source_calls;
}

const std::vector<SourcePreparationCall>& source_preparation_calls() {
    return g_state.source_preparation_calls;
}

const std::vector<PackageBaseSourceExecutionCall>&
package_base_source_execution_calls() {
    return g_state.package_base_source_calls;
}

const std::vector<std::string>& system_commands() {
    return g_state.system_commands;
}

void require_script_consumed() {
    if(g_state.expectation_failure.has_value()) {
        throw std::logic_error(*g_state.expectation_failure);
    }
    for(const auto& [package_name, results] : g_state.preference_results) {
        if(!results.empty()) {
            throw std::logic_error(
                "Unconsumed source preference result for " + package_name + ".");
        }
    }
    if(!g_state.metadata_sessions.empty()) {
        throw std::logic_error("Unconsumed metadata session script.");
    }
    if(!g_state.source_executions.empty()) {
        throw std::logic_error("Unconsumed source execution script.");
    }
}

} // namespace system_source_upgrade_test_stub

SourcePreferenceDirectorySnapshot snapshot_source_preference_directory() {
    g_state.events.push_back(stub::Event{
        stub::EventKind::PreferenceDirectorySnapshot,
        "source-build.d"});
    if(g_state.preference_directory_failure.has_value()) {
        throw std::runtime_error(*g_state.preference_directory_failure);
    }
    return g_state.preference_directory;
}

StrictSourcePreferenceResult read_source_preference_strict(
    const std::string& package_name) {
    g_state.events.push_back(stub::Event{
        stub::EventKind::StrictPreferenceRead,
        package_name});
    auto found = g_state.preference_results.find(package_name);
    if(found == g_state.preference_results.end() || found->second.empty()) {
        fail_unexpected(
            "Unexpected strict source preference read for " +
            package_name + ".");
    }
    StrictSourcePreferenceResult result =
        std::move(found->second.front());
    found->second.pop_front();
    return result;
}

void require_supported_production_source_build_options(
    const AppConfig& config) {
    g_state.supported_option_configs.push_back(snapshot_config(config));
    g_state.events.push_back(stub::Event{
        stub::EventKind::SupportedOptionsGuard,
        "source-options"});
    if(g_state.supported_options_failure.has_value()) {
        throw std::runtime_error(*g_state.supported_options_failure);
    }
}

ResolvedSourceBuildIdentity resolve_source_build_identity(
    const std::string& package_name) {
    g_state.events.push_back(stub::Event{
        stub::EventKind::SourceIdentityResolution,
        package_name});
    auto failure = g_state.identity_failures.find(package_name);
    if(failure != g_state.identity_failures.end()) {
        throw std::runtime_error(failure->second);
    }
    auto identity = g_state.identities.find(package_name);
    return identity == g_state.identities.end()
               ? default_identity(package_name)
               : identity->second;
}

ProductionSourceBuildWorkItem prepare_resolved_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    bool only_if_updated,
    bool needed) {
    g_state.events.push_back(stub::Event{
        stub::EventKind::SourceWorkItemPreparation,
        identity.requested_name()});
    auto failure = g_state.work_item_failures.find(identity.requested_name());
    if(failure != g_state.work_item_failures.end()) {
        throw std::runtime_error(failure->second);
    }

    ProductionSourceBuildWorkItem work_item;
    work_item.request.package_name = identity.requested_name();
    work_item.request.checkout_name = identity.package_base();
    work_item.request.git_url = identity.git_url();
    work_item.request.custom_environment = std::move(environment);
    work_item.request.only_if_updated = only_if_updated;
    work_item.request.needed = needed;
    work_item.required_targets.push_back(RequiredPackageArtifactTarget{
        identity.package_base(),
        identity.requested_name(),
        DesiredInstallReason::Explicit});
    work_item.required_target_provenance =
        identity.source_kind() == SourceBuildSourceKind::Repository
            ? RequiredTargetProvenance::RepositoryExactPackageProjection
            : RequiredTargetProvenance::AurBuildPlanProjection;
    work_item.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility;
    if(const auto* repository = identity.repository_identity();
       repository != nullptr) {
        work_item.repository_identity = *repository;
        work_item.configured_repository_order =
            repository->exact_package().configured_repository_order;
    }
    work_item.uses_system_update_baseline =
        identity.source_kind() == SourceBuildSourceKind::Repository;
    return work_item;
}

ProductionSourceBuildWorkItem prepare_resolved_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    bool only_if_updated,
    bool needed,
    const ProviderSelectionCallback& select_provider) {
    ProductionSourceBuildWorkItem work_item =
        prepare_resolved_source_build_work_item(
            identity, std::move(environment), only_if_updated, needed);
    auto candidates = g_state.provider_candidates.find(
        identity.requested_name());
    if(candidates != g_state.provider_candidates.end()) {
        if(!select_provider) {
            throw std::runtime_error(
                "scripted registered-source provider remains ambiguous");
        }
        const std::optional<ProviderSelectionSet> selected =
            select_provider(
                "scripted-registered-source-dependency",
                candidates->second);
        if(!selected.has_value()) {
            throw std::runtime_error(
                "scripted registered-source provider remains ambiguous");
        }
        for(const ProvidedDependency& member : selected->members()) {
            if(!std::holds_alternative<RepositoryProviderOrigin>(
                   member.origin)) {
                throw std::runtime_error(
                    "scripted registered-source AUR provider is unsupported");
            }
            work_item.selected_repository_providers.push_back(member);
        }
    }
    auto providers = g_state.selected_repository_providers.find(
        identity.requested_name());
    if(providers != g_state.selected_repository_providers.end()) {
        work_item.selected_repository_providers = providers->second;
    }
    return work_item;
}

ProductionSourceBuildWorkItem prepare_registered_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    const ProviderSelectionCallback& select_provider) {
    ProductionSourceBuildWorkItem work_item =
        prepare_resolved_source_build_work_item(
            identity, std::move(environment),
            identity.source_kind() == SourceBuildSourceKind::Aur,
            false, select_provider);
    if(identity.source_kind() == SourceBuildSourceKind::Repository) {
        work_item.artifact_lifecycle_intent =
            ArtifactLifecycleIntent::PackageBaseSet;
        work_item.request.only_if_updated = false;
        work_item.uses_system_update_baseline = true;
    } else {
        work_item.artifact_lifecycle_intent =
            ArtifactLifecycleIntent::SingularCompatibility;
        work_item.request.only_if_updated = true;
        work_item.uses_system_update_baseline = false;
    }
    work_item.request.needed = false;
    return work_item;
}

PreparedProductionSourceBuildInvocation
prepare_production_source_build_invocation(
    std::vector<ProductionSourceBuildWorkItem> work_items,
    const AppConfig& config) {
    g_state.invocation_configs.push_back(snapshot_config(config));
    g_state.events.push_back(stub::Event{
        stub::EventKind::SourceInvocationPreparation,
        "source-invocation"});
    if(g_state.invocation_failure.has_value()) {
        throw std::runtime_error(*g_state.invocation_failure);
    }
    std::vector<ProvidedDependency> selected_repository_providers;
    for(const auto& work_item : work_items) {
        for(const auto& provider : work_item.selected_repository_providers) {
            const auto same = [&provider](
                                  const ProvidedDependency& existing) {
                return same_provider_identity(existing, provider);
            };
            if(std::find_if(
                   selected_repository_providers.begin(),
                   selected_repository_providers.end(), same) ==
               selected_repository_providers.end()) {
                selected_repository_providers.push_back(provider);
            }
        }
    }
    return PreparedProductionSourceBuildInvocation{
        std::move(work_items),
        std::move(selected_repository_providers),
        PacmanDatabasePaths{"/stub/root", "/stub/database"},
        std::nullopt};
}

PackageMetadataError::PackageMetadataError(PackageMetadataFailure failure)
    : std::runtime_error(failure.diagnostic),
      failure_(std::move(failure)) {
}

const PackageMetadataFailure& PackageMetadataError::failure() const noexcept {
    return failure_;
}

PacmanDatabasePaths resolve_pacman_database_paths() {
    return PacmanDatabasePaths{"/stub/root", "/stub/database"};
}

struct PackageMetadataSession::Impl {
    explicit Impl(stub::MetadataSessionScript scripted)
        : script(std::move(scripted)) {
    }

    stub::MetadataSessionScript script;
};

PackageMetadataSession::PackageMetadataSession(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

PackageMetadataSession::PackageMetadataSession(
    PackageMetadataSession&&) noexcept = default;

PackageMetadataSession& PackageMetadataSession::operator=(
    PackageMetadataSession&&) noexcept = default;

PackageMetadataSession::~PackageMetadataSession() noexcept = default;

PackageMetadataSession PackageMetadataSession::open(
    const PacmanDatabasePaths& paths) {
    g_state.events.push_back(stub::Event{
        stub::EventKind::MetadataSessionOpen,
        paths.db_path.string()});
    if(g_state.metadata_sessions.empty()) {
        fail_unexpected("Unexpected package metadata session open.");
    }
    stub::MetadataSessionScript script =
        std::move(g_state.metadata_sessions.front());
    g_state.metadata_sessions.pop_front();
    if(script.open_failure.has_value()) {
        throw PackageMetadataError(*script.open_failure);
    }
    return PackageMetadataSession(std::make_unique<Impl>(std::move(script)));
}

InstalledPackageQueryResult PackageMetadataSession::query_installed_package(
    const std::string& package_name) const {
    g_state.events.push_back(stub::Event{
        stub::EventKind::InstalledPackageQuery,
        package_name});
    if(impl_ == nullptr) {
        return PackageMetadataFailure{
            PackageMetadataErrorCode::QueryFailed,
            "Stub package metadata session is closed."};
    }
    auto found = impl_->script.installed_package_results.find(package_name);
    return found == impl_->script.installed_package_results.end()
               ? InstalledPackageQueryResult{PackageNotFound{}}
               : found->second;
}

LocalPackageVersionSnapshotResult
PackageMetadataSession::snapshot_local_package_versions() const {
    g_state.events.push_back(stub::Event{
        stub::EventKind::LocalPackageSnapshot,
        "local-packages"});
    if(impl_ == nullptr) {
        return PackageMetadataFailure{
            PackageMetadataErrorCode::QueryFailed,
            "Stub package metadata session is closed."};
    }
    return impl_->script.local_package_snapshot;
}

BuildPlan resolve_build_plan(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback&) {
    if(g_state.aur_invocation_plan.has_value()) {
        return g_state.aur_invocation_plan.value();
    }
    BuildPlan plan;
    for(std::size_t index = 0; index < targets.size(); ++index) {
        plan.root_targets.push_back(
            RootTargetIdentity{index, targets[index]});
    }
    return plan;
}

PlanStateProjection project_build_plan_state(const BuildPlan&) {
    // This phase-characterization binary already replaces the BuildPlan
    // execution guard below. It supplies an empty, already-projected readiness
    // seam for unrelated unified-plan paths; relation classification remains
    // covered by the dedicated production-model and unified-plan tests.
    return PlanStateProjection{};
}

const ExecutionReadiness& execution_readiness(
    const PlanStateProjection& projection,
    ExecutionCapability capability) noexcept {
    switch(capability) {
        case ExecutionCapability::Fetch:
            return projection.readiness[0];
        case ExecutionCapability::Build:
            return projection.readiness[1];
        case ExecutionCapability::Install:
            return projection.readiness[2];
    }
    return projection.readiness[0];
}

void require_executable_install_plan(
    const std::string&, const BuildPlan&) {
    if(g_state.build_plan_guard_failure.has_value()) {
        throw std::runtime_error(g_state.build_plan_guard_failure.value());
    }
}

int run_command(const std::string& command) {
    g_state.system_commands.push_back(command);
    g_state.events.push_back(stub::Event{
        stub::EventKind::SystemCommand,
        command});
    if(g_state.system_failure.has_value()) {
        throw std::runtime_error(*g_state.system_failure);
    }
    return g_state.system_exit_status;
}

SeparatedSourceBuildCleanupError::SeparatedSourceBuildCleanupError(
    ArtifactInstallExecutionOutcome install_outcome,
    ProductionSourceBuildStagedOutcome production_outcome,
    const std::string& diagnostic)
    : std::runtime_error(diagnostic),
      install_outcome_(install_outcome),
      production_outcome_(std::move(production_outcome)) {
}

SourceBuildPreparationOutcome
prepare_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    SourceBuildUpdatePolicy update_policy,
    const AppConfig& config) {
    if(work_item.required_targets.size() != 1 ||
       work_item.request.package_name.empty() ||
       work_item.request.package_name !=
           work_item.required_targets.front().package_name ||
       work_item.request.checkout_name !=
           work_item.required_targets.front().package_base ||
       work_item.required_target_provenance !=
           RequiredTargetProvenance::RepositoryExactPackageProjection ||
       work_item.artifact_lifecycle_intent !=
           ArtifactLifecycleIntent::PackageBaseSet ||
       work_item.request.only_if_updated || work_item.request.needed) {
        fail_unexpected(
            "Registered repository preparation received an inconsistent PackageBase work item.");
    }

    const stub::ConfigSnapshot config_snapshot = snapshot_config(config);
    g_state.source_preparation_calls.push_back(stub::SourcePreparationCall{
        work_item.request.package_name,
        work_item.request.checkout_name,
        work_item.request,
        update_policy,
        config_snapshot});
    g_state.events.push_back(stub::Event{
        stub::EventKind::SourcePreparation,
        work_item.request.package_name});
    // 既存phase testのsource単位観測はpreparation開始をexecution開始として保つ。
    g_state.source_calls.push_back(stub::SourceExecutionCall{
        work_item.request.package_name,
        work_item.request.checkout_name,
        work_item.request,
        config_snapshot});
    g_state.events.push_back(stub::Event{
        stub::EventKind::SourceExecution,
        work_item.request.package_name});

    if(g_state.source_executions.empty()) {
        fail_unexpected(
            "Unexpected registered repository source preparation for " +
            work_item.request.package_name + ".");
    }
    ScriptedSourceExecution& execution = g_state.source_executions.front();
    if(execution.kind == ScriptedSourceExecutionKind::Success) {
        if(execution.result.status == SourceBuildExecutionStatus::UpToDate) {
            SourceBuildUpToDate outcome{
                std::move(execution.result.diagnostic),
                execution.result.production_outcome.has_value()
                    ? std::optional<
                          ProductionSourceBuildProvenance>{
                          execution.result.production_outcome->source_provenance}
                    : std::nullopt};
            g_state.source_executions.pop_front();
            return outcome;
        }
        if(execution.result.status ==
           SourceBuildExecutionStatus::UpdateStatusUnknownSkipped) {
            SourceBuildUpdateStatusUnknownSkipped outcome{
                execution.result.update_status_unknown_skip_reason
                    .value_or(
                        SourceBuildUpdateStatusUnknownSkipReason::
                            NoConfirm),
                std::move(execution.result.diagnostic),
                execution.result.production_outcome.has_value()
                    ? std::optional<
                          ProductionSourceBuildProvenance>{
                          execution.result.production_outcome->source_provenance}
                    : std::nullopt};
            g_state.source_executions.pop_front();
            return outcome;
        }
    }
    return PreparedSourceBuildNeedsBuild::
        make_for_registered_source_build_test();
}

RegisteredSourcePackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    static_cast<void>(prepared);
    static_cast<void>(database_paths);
    if(work_item.required_targets.size() != 1) {
        fail_unexpected(
            "Registered repository PackageBase execution received an inconsistent required target.");
    }
    g_state.package_base_source_calls.push_back(
        stub::PackageBaseSourceExecutionCall{
            work_item.request.package_name,
            work_item.request.checkout_name,
            work_item.request,
            snapshot_config(config)});
    g_state.events.push_back(stub::Event{
        stub::EventKind::PackageBaseSourceExecution,
        work_item.request.package_name});
    if(g_state.source_executions.empty()) {
        fail_unexpected(
            "Unexpected registered repository PackageBase execution for " +
            work_item.request.package_name + ".");
    }

    ScriptedSourceExecution execution =
        std::move(g_state.source_executions.front());
    g_state.source_executions.pop_front();
    const RequiredPackageArtifactTarget& required =
        work_item.required_targets.front();
    const auto legacy_result = [&](ArtifactInstallExecutionOutcome outcome) {
        return RegisteredSourcePackageBaseExecutionResult(
            work_item.request.checkout_name,
            std::vector<PackageBaseSourceBuildSelectedResult>{
                PackageBaseSourceBuildSelectedResult{
                    ArtifactPackageIdentity{
                        required.package_name, "2.0-1"},
                    required.desired_reason,
                    outcome}},
            std::vector<ArtifactPackageIdentity>{});
    };

    switch(execution.kind) {
        case ScriptedSourceExecutionKind::Success:
            if(execution.result.status == SourceBuildExecutionStatus::Installed) {
                return legacy_result(ArtifactInstallExecutionOutcome::Installed);
            }
            if(execution.result.status ==
               SourceBuildExecutionStatus::SkippedAsNeeded) {
                return legacy_result(
                    ArtifactInstallExecutionOutcome::SkippedAsNeeded);
            }
            fail_unexpected(
                "Registered repository prepared execution received a closed preparation outcome.");
        case ScriptedSourceExecutionKind::Failure:
            throw RegisteredSourcePackageBasePhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                execution.diagnostic);
        case ScriptedSourceExecutionKind::CacheFailure:
            throw TrustedCacheError(TrustedCacheFailure{
                TrustedCacheStage::RootRevalidation,
                TrustedCacheErrorCode::ConcurrentReplacement,
                std::nullopt});
        case ScriptedSourceExecutionKind::CleanupFailure:
            throw RegisteredSourcePackageBaseCleanupError(
                legacy_result(execution.cleanup_outcome),
                execution.diagnostic);
        case ScriptedSourceExecutionKind::UnknownFailure:
            throw UnknownSourceExecutionFailure{};
        case ScriptedSourceExecutionKind::PackageBaseSuccess:
            return RegisteredSourcePackageBaseExecutionResult(
                std::move(execution.package_base),
                std::move(execution.selected_children),
                std::move(execution.unselected_artifacts),
                std::move(execution.source_provenance));
        case ScriptedSourceExecutionKind::PackageBaseCleanupFailure:
            throw RegisteredSourcePackageBaseCleanupError(
                RegisteredSourcePackageBaseExecutionResult(
                    std::move(execution.package_base),
                    std::move(execution.selected_children),
                    std::move(execution.unselected_artifacts)),
                execution.diagnostic);
        case ScriptedSourceExecutionKind::PackageBaseSelectionFailure:
            throw RegisteredSourcePackageBasePreparationError(
                std::move(execution.selection_failure),
                execution.diagnostic);
        case ScriptedSourceExecutionKind::PackageBaseMixedReasonFailure:
            throw RegisteredSourcePackageBasePreparationError(
                std::move(execution.mixed_reason_failure),
                execution.diagnostic);
        case ScriptedSourceExecutionKind::PackageBasePhaseFailure:
            throw RegisteredSourcePackageBasePhaseError(
                execution.phase, execution.diagnostic);
        case ScriptedSourceExecutionKind::PackageBaseTransactionFailure:
            throw RegisteredSourcePackageTransactionError(
                execution.transaction_failure_kind,
                std::move(execution.package_base),
                std::move(execution.transaction_attempts),
                execution.transaction_exit_code,
                execution.diagnostic);
        case ScriptedSourceExecutionKind::PackageBaseMetadataFailure:
            throw PackageMetadataError(std::move(execution.metadata_failure));
    }
    throw std::logic_error(
        "Unknown scripted registered repository execution kind.");
}

SourceBuildExecutionResult execute_prepared_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    static_cast<void>(database_paths);
    if(work_item.required_targets.size() != 1 ||
       work_item.request.package_name.empty() ||
       work_item.request.package_name !=
           work_item.required_targets.front().package_name ||
       work_item.request.checkout_name !=
           work_item.required_targets.front().package_base) {
        fail_unexpected(
            "System source upgrade singular execution received an inconsistent required target.");
    }
    g_state.source_calls.push_back(stub::SourceExecutionCall{
        work_item.request.package_name,
        work_item.request.checkout_name,
        work_item.request,
        snapshot_config(config)});
    g_state.events.push_back(stub::Event{
        stub::EventKind::SourceExecution,
        work_item.request.package_name});
    if(g_state.source_executions.empty()) {
        fail_unexpected(
            "Unexpected source execution for " +
            work_item.request.package_name + ".");
    }

    ScriptedSourceExecution execution =
        std::move(g_state.source_executions.front());
    g_state.source_executions.pop_front();
    switch(execution.kind) {
        case ScriptedSourceExecutionKind::Success:
            return execution.result;
        case ScriptedSourceExecutionKind::Failure:
            throw std::runtime_error(execution.diagnostic);
        case ScriptedSourceExecutionKind::CacheFailure:
            throw TrustedCacheError(TrustedCacheFailure{
                TrustedCacheStage::RootRevalidation,
                TrustedCacheErrorCode::ConcurrentReplacement,
                std::nullopt});
        case ScriptedSourceExecutionKind::CleanupFailure:
            throw SeparatedSourceBuildCleanupError(
                execution.cleanup_outcome,
                ProductionSourceBuildStagedOutcome{
                    .source_provenance = {},
                    .build_outcome =
                        ProductionSourceBuildCommandOutcome::
                            Succeeded,
                    .install_outcome =
                        ProductionSourceInstallOutcome::
                            Succeeded},
                execution.diagnostic);
        case ScriptedSourceExecutionKind::UnknownFailure:
            throw UnknownSourceExecutionFailure{};
        case ScriptedSourceExecutionKind::PackageBaseSuccess:
        case ScriptedSourceExecutionKind::PackageBaseCleanupFailure:
        case ScriptedSourceExecutionKind::PackageBaseSelectionFailure:
        case ScriptedSourceExecutionKind::PackageBaseMixedReasonFailure:
        case ScriptedSourceExecutionKind::PackageBasePhaseFailure:
        case ScriptedSourceExecutionKind::PackageBaseTransactionFailure:
        case ScriptedSourceExecutionKind::PackageBaseMetadataFailure:
            fail_unexpected(
                "Registered AUR singular execution received a PackageBase script.");
    }
    throw std::logic_error("Unknown scripted source execution kind.");
}

BuildPlan resolve_recipe_build_plan(const std::vector<std::string>&,
                                    const AurRecipeMetadataSet&,
                                    const ProviderSelectionCallback&) {
    throw std::logic_error("This query fixture has no evaluated recipe metadata.");
}

ProductionSourceBuildWorkItem prepare_registered_recipe_source_build_work_item(
    const ResolvedSourceBuildIdentity&, SourceBuildEnvironment,
    const ProviderSelectionCallback&, const AurRecipeMetadataSet&) {
    throw std::logic_error("This source fixture has no evaluated recipe metadata.");
}
