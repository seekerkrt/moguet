#include "operation_stub.hpp"

#include "artifact_install_plan.hpp"
#include "artifact_workspace.hpp"
#include "process.hpp"
#include "separated_source_build.hpp"

#include <algorithm>
#include <deque>
#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {

namespace stub = upgrade_all_operation_test_stub;

struct ScriptFailure {
    std::string diagnostic;
};

struct ResponseScriptFailure {
    std::string diagnostic;
};

enum class ScriptedSourceExecutionKind {
    Success,
    Failure,
    CacheFailure,
    CleanupFailure,
    UnknownFailure,
};

struct ScriptedPackageBaseExecution {
    std::string package_base;
    ArtifactPackageIdentity selected_child;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
};

struct ScriptedSourceExecution {
    ScriptedSourceExecutionKind kind =
        ScriptedSourceExecutionKind::Success;
    SourceBuildExecutionResult result;
    ArtifactInstallExecutionOutcome cleanup_outcome =
        ArtifactInstallExecutionOutcome::Installed;
    std::optional<ScriptedPackageBaseExecution> package_base_execution;
    std::string diagnostic;
};

enum class ScriptedAurExecutionKind {
    Success,
    OrdinaryFailure,
    CleanupFailure,
    UnknownFailure,
};

struct ScriptedAurExecution {
    ScriptedAurExecutionKind kind =
        ScriptedAurExecutionKind::Success;
    std::vector<ArtifactInstallExecutionOutcome> child_outcomes;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
    std::string diagnostic;
};

using RepositoryConfigurationScript = std::variant<
    PacmanRepositoryConfiguration,
    PackageMetadataFailure>;
using ForeignInventoryScript = std::variant<
    ForeignPackageInventory,
    PackageMetadataFailure>;
using DatabasePathsScript = std::variant<
    PacmanDatabasePaths,
    PackageMetadataFailure>;
using InfoManyScript = std::variant<
    std::map<std::string, AurPackageInfo>,
    ScriptFailure,
    ResponseScriptFailure>;
using InfoStrictScript = std::variant<
    std::optional<AurPackageInfo>,
    ScriptFailure,
    ResponseScriptFailure>;
using VercmpScript = std::variant<std::string, ScriptFailure>;

struct OperationStubState {
    SourcePreferenceDirectorySnapshot preference_directory;
    std::optional<std::string> preference_directory_failure;
    std::map<std::string, std::deque<StrictSourcePreferenceResult>>
        preference_results;
    std::function<void()> after_next_strict_preference_read_hook;
    std::map<std::string, ResolvedSourceBuildIdentity> identities;
    std::map<std::string, std::string> identity_failures;
    std::map<std::string, std::string> work_item_failures;

    std::deque<stub::MetadataSessionScript> metadata_sessions;
    int system_exit_status = 0;
    std::optional<std::string> system_failure;
    std::function<void()> after_system_command_hook;
    bool cache_seed_failure = false;
    std::function<void()> after_next_cache_seed_hook;
    bool cache_activation_failure = false;
    std::deque<ScriptedSourceExecution> source_executions;

    RepositoryConfigurationScript repository_configuration =
        PacmanRepositoryConfiguration{
            PacmanDatabasePaths{
                "/upgrade-all-stub/root",
                "/upgrade-all-stub/database"},
            {"upgrade-all-stub-repository"}};
    ForeignInventoryScript foreign_inventory = ForeignPackageInventory{};
    std::function<void()> after_foreign_inventory_hook;
    InstalledPackageRelationInventoryResult installed_relation_inventory =
        InstalledPackageRelationInventory{
            PackageRelationInstalledDatabaseIdentity{
                "/upgrade-all-stub/root",
                "/upgrade-all-stub/database"},
            {}};
    InstalledPackageRuntimeDependencyMetadataInventoryResult
        installed_runtime_dependency_inventory =
            InstalledPackageRuntimeDependencyMetadataInventory{};
    std::map<std::string, StrictRepositoryPackageQueryResult>
        repository_candidate_results;
    std::deque<InfoManyScript> info_many_scripts;
    std::function<void()> after_info_many_hook;
    std::deque<InfoStrictScript> info_strict_scripts;
    std::deque<VercmpScript> vercmp_scripts;

    stub::ResolverHandler resolver_handler;
    std::vector<std::vector<std::string>> resolver_calls;

    DatabasePathsScript database_paths = PacmanDatabasePaths{
        "/upgrade-all-stub/root",
        "/upgrade-all-stub/database"};
    std::deque<DatabasePathsScript> database_path_scripts;
    std::optional<std::string> supported_options_failure;
    std::optional<std::string> source_invocation_failure;
    std::optional<std::size_t> pkgdest_failure_call;
    std::string pkgdest_failure_diagnostic;
    std::vector<bool> supported_options_guards;
    std::vector<SourceBuildEnvironment> pkgdest_guards;
    std::size_t database_calls = 0;

    std::deque<ScriptedAurExecution> aur_executions;
    const PacmanDatabasePaths* first_aur_database_paths_address = nullptr;

    std::vector<stub::Event> events;
    std::vector<std::string> strict_preference_reads;
    std::vector<stub::SourceExecutionCall> source_calls;
    std::vector<stub::SourcePreparationCall> source_preparation_calls;
    std::vector<stub::PackageBaseSourceExecutionCall>
        package_base_source_calls;
    std::vector<stub::AurExecutionCall> aur_calls;
    std::vector<std::string> system_commands;
    std::size_t repository_configuration_call_count = 0;
    std::vector<PacmanRepositoryConfiguration> inventory_configurations;
    std::size_t installed_relation_inventory_call_count = 0;
    std::size_t installed_runtime_dependency_inventory_call_count = 0;
    std::vector<std::string> repository_candidate_calls;
    std::vector<std::vector<std::string>> info_many_calls;
    std::vector<std::string> info_strict_calls;
    std::vector<std::string> vercmp_calls;
    std::optional<std::string> expectation_failure;
};

struct UnknownSourceExecutionFailure {};
struct UnknownAurExecutionFailure {};

OperationStubState g_state;

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

void record_event(
    stub::EventKind kind,
    std::string subject,
    std::vector<std::string> package_names = {}) {
    g_state.events.push_back(stub::Event{
        kind,
        std::move(subject),
        std::move(package_names)});
}

[[noreturn]] void fail_unexpected(const std::string& diagnostic) {
    g_state.expectation_failure = diagnostic;
    throw std::logic_error(diagnostic);
}

template <typename Script>
Script take_script(
    std::deque<Script>& scripts,
    const std::string& missing_diagnostic) {
    if(scripts.empty()) fail_unexpected(missing_diagnostic);

    Script script = std::move(scripts.front());
    scripts.pop_front();
    return script;
}

ResolvedSourceBuildIdentity default_identity(
    const std::string& package_name) {
    return ResolvedSourceBuildIdentity{
        ResolvedRepositorySourceBuildIdentity{
            RepositoryPackagePresent{
                "core", 0, package_name, package_name}}};
}

bool is_registered_preference_name(const std::string& package_name) {
    return std::any_of(
        g_state.preference_directory.entries.begin(),
        g_state.preference_directory.entries.end(),
        [&package_name](const SourcePreferenceEntrySnapshot& entry) {
            return entry.is_regular_file &&
                   entry.package_name == package_name;
        });
}

std::string environment_subject(const SourceBuildEnvironment& environment) {
    if(environment.ordered_assignments.empty()) return "<empty>";

    std::string subject;
    for(std::size_t index = 0;
        index < environment.ordered_assignments.size(); ++index) {
        if(index > 0) subject += ",";
        const SourceEnvironmentAssignment& assignment =
            environment.ordered_assignments[index];
        subject += assignment.key + "=" + assignment.value;
    }
    return subject;
}

void record_aur_lifecycle_event(
    std::size_t call_index,
    stub::EventKind kind) {
    stub::AurExecutionCall& call = g_state.aur_calls.at(call_index);
    call.lifecycle_events.push_back(kind);
    record_event(
        kind,
        call.package_name.empty() ? call.package_base
                                  : call.package_name,
        call.plan_package_names);
}

void enqueue_aur_execution(
    ScriptedAurExecutionKind kind,
    std::vector<ArtifactInstallExecutionOutcome> child_outcomes = {},
    std::vector<ArtifactPackageIdentity> unselected_artifacts = {},
    std::string diagnostic = {}) {
    g_state.aur_executions.push_back(
        ScriptedAurExecution{
            kind, std::move(child_outcomes),
            std::move(unselected_artifacts),
            std::move(diagnostic)});
}

} // namespace

namespace upgrade_all_operation_test_stub {

void reset() {
    g_state = OperationStubState{};
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

void enqueue_source_preference_result(
    const std::string& package_name,
    StrictSourcePreferenceResult result) {
    enqueue_preference_result(package_name, std::move(result));
}

void set_after_next_strict_preference_read_hook(
    std::function<void()> hook) {
    g_state.after_next_strict_preference_read_hook = std::move(hook);
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

void set_after_system_command_hook(std::function<void()> hook) {
    g_state.after_system_command_hook = std::move(hook);
}

void fail_cache_seed() {
    g_state.cache_seed_failure = true;
}

void set_after_next_cache_seed_hook(std::function<void()> hook) {
    g_state.after_next_cache_seed_hook = std::move(hook);
}

void fail_cache_activation() {
    g_state.cache_activation_failure = true;
}

void enqueue_source_success(SourceBuildExecutionResult result) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::Success;
    execution.result = std::move(result);
    g_state.source_executions.push_back(std::move(execution));
}

void enqueue_package_base_source_success(
    std::string package_base,
    ArtifactPackageIdentity selected_child,
    std::vector<ArtifactPackageIdentity> unselected_artifacts) {
    ScriptedSourceExecution execution;
    execution.kind = ScriptedSourceExecutionKind::Success;
    execution.result.status = SourceBuildExecutionStatus::Installed;
    execution.package_base_execution = ScriptedPackageBaseExecution{
        std::move(package_base), std::move(selected_child),
        std::move(unselected_artifacts)};
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

void set_repository_configuration(
    PacmanRepositoryConfiguration configuration) {
    g_state.repository_configuration = std::move(configuration);
}

void set_repository_configuration_failure(PackageMetadataFailure failure) {
    g_state.repository_configuration = std::move(failure);
}

void set_foreign_inventory(ForeignPackageInventory inventory) {
    g_state.foreign_inventory = std::move(inventory);
}

void set_foreign_inventory_failure(PackageMetadataFailure failure) {
    g_state.foreign_inventory = std::move(failure);
}

void set_after_foreign_inventory_hook(std::function<void()> hook) {
    g_state.after_foreign_inventory_hook = std::move(hook);
}

void set_installed_relation_inventory(
    InstalledPackageRelationInventoryResult inventory) {
    g_state.installed_relation_inventory = std::move(inventory);
}

void set_installed_runtime_dependency_inventory(
    InstalledPackageRuntimeDependencyMetadataInventoryResult inventory) {
    g_state.installed_runtime_dependency_inventory = std::move(inventory);
}

void set_repository_candidate_result(
    const std::string& package_name,
    StrictRepositoryPackageQueryResult result) {
    g_state.repository_candidate_results.insert_or_assign(
        package_name, std::move(result));
}

void enqueue_info_many_result(
    std::map<std::string, AurPackageInfo> result) {
    g_state.info_many_scripts.push_back(std::move(result));
}

void set_after_info_many_hook(std::function<void()> hook) {
    g_state.after_info_many_hook = std::move(hook);
}

void enqueue_info_many_failure(std::string diagnostic) {
    g_state.info_many_scripts.push_back(
        ScriptFailure{std::move(diagnostic)});
}

void enqueue_info_many_response_failure(std::string diagnostic) {
    g_state.info_many_scripts.push_back(
        ResponseScriptFailure{std::move(diagnostic)});
}

void enqueue_info_strict_result(std::optional<AurPackageInfo> result) {
    g_state.info_strict_scripts.push_back(std::move(result));
}

void enqueue_info_strict_failure(std::string diagnostic) {
    g_state.info_strict_scripts.push_back(
        ScriptFailure{std::move(diagnostic)});
}

void enqueue_info_strict_response_failure(std::string diagnostic) {
    g_state.info_strict_scripts.push_back(
        ResponseScriptFailure{std::move(diagnostic)});
}

void enqueue_vercmp_result(std::string output) {
    g_state.vercmp_scripts.push_back(std::move(output));
}

void enqueue_vercmp_failure(std::string diagnostic) {
    g_state.vercmp_scripts.push_back(
        ScriptFailure{std::move(diagnostic)});
}

void set_resolver_handler(ResolverHandler handler) {
    g_state.resolver_handler = std::move(handler);
}

void set_database_paths(PacmanDatabasePaths paths) {
    g_state.database_paths = std::move(paths);
}

void set_database_failure(PackageMetadataFailure failure) {
    g_state.database_paths = std::move(failure);
}

void enqueue_database_paths(PacmanDatabasePaths paths) {
    g_state.database_path_scripts.push_back(std::move(paths));
}

void enqueue_database_failure(PackageMetadataFailure failure) {
    g_state.database_path_scripts.push_back(std::move(failure));
}

void fail_supported_options_guard(std::string diagnostic) {
    g_state.supported_options_failure = std::move(diagnostic);
}

void fail_supported_options(std::string diagnostic) {
    fail_supported_options_guard(std::move(diagnostic));
}

void fail_source_invocation(std::string diagnostic) {
    g_state.source_invocation_failure = std::move(diagnostic);
}

void fail_pkgdest_guard_on_call(
    std::size_t one_based_call_index,
    std::string diagnostic) {
    g_state.pkgdest_failure_call = one_based_call_index;
    g_state.pkgdest_failure_diagnostic = std::move(diagnostic);
}

void enqueue_aur_success(ArtifactInstallExecutionOutcome outcome) {
    enqueue_aur_successes({outcome});
}

void enqueue_aur_successes(
    std::vector<ArtifactInstallExecutionOutcome> child_outcomes,
    std::vector<ArtifactPackageIdentity> unselected_artifacts) {
    enqueue_aur_execution(
        ScriptedAurExecutionKind::Success,
        std::move(child_outcomes),
        std::move(unselected_artifacts));
}

void enqueue_aur_ordinary_failure(std::string diagnostic) {
    enqueue_aur_execution(
        ScriptedAurExecutionKind::OrdinaryFailure,
        {}, {},
        std::move(diagnostic));
}

void enqueue_aur_cleanup_failure(
    ArtifactInstallExecutionOutcome outcome,
    std::string diagnostic) {
    enqueue_aur_cleanup_failure(
        {outcome}, {}, std::move(diagnostic));
}

void enqueue_aur_cleanup_failure(
    std::vector<ArtifactInstallExecutionOutcome> child_outcomes,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    std::string diagnostic) {
    enqueue_aur_execution(
        ScriptedAurExecutionKind::CleanupFailure,
        std::move(child_outcomes),
        std::move(unselected_artifacts),
        std::move(diagnostic));
}

void enqueue_aur_unknown_failure() {
    enqueue_aur_execution(ScriptedAurExecutionKind::UnknownFailure);
}

void record_progress(const std::string& subject) {
    record_event(EventKind::Progress, subject);
}

const std::vector<Event>& event_history() {
    return g_state.events;
}

const std::vector<Event>& events() {
    return g_state.events;
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

const std::vector<AurExecutionCall>& aur_execution_calls() {
    return g_state.aur_calls;
}

const std::vector<std::string>& system_commands() {
    return g_state.system_commands;
}

std::size_t repository_configuration_calls() {
    return g_state.repository_configuration_call_count;
}

std::size_t inventory_calls() {
    return g_state.inventory_configurations.size();
}

std::size_t installed_relation_inventory_calls() {
    return g_state.installed_relation_inventory_call_count;
}

std::size_t installed_runtime_dependency_inventory_calls() {
    return g_state.installed_runtime_dependency_inventory_call_count;
}

const std::vector<PacmanRepositoryConfiguration>&
inventory_configuration_history() {
    return g_state.inventory_configurations;
}

const std::vector<std::string>& repository_candidate_call_history() {
    return g_state.repository_candidate_calls;
}

const std::vector<std::vector<std::string>>& info_many_call_history() {
    return g_state.info_many_calls;
}

const std::vector<std::string>& info_strict_call_history() {
    return g_state.info_strict_calls;
}

const std::vector<std::string>& vercmp_call_history() {
    return g_state.vercmp_calls;
}

std::size_t resolver_call_count() {
    return g_state.resolver_calls.size();
}

const std::vector<std::vector<std::string>>& resolver_calls() {
    return g_state.resolver_calls;
}

const std::vector<bool>& supported_options_guard_history() {
    return g_state.supported_options_guards;
}

const std::vector<SourceBuildEnvironment>& pkgdest_guard_history() {
    return g_state.pkgdest_guards;
}

std::size_t database_call_count() {
    return g_state.database_calls;
}

const std::vector<std::string>& strict_preference_read_history() {
    return g_state.strict_preference_reads;
}

void require_script_consumed() {
    if(g_state.expectation_failure.has_value()) {
        throw std::logic_error(*g_state.expectation_failure);
    }
    for(const auto& [package_name, results] : g_state.preference_results) {
        if(!results.empty()) {
            throw std::logic_error(
                "Unconsumed source preference result for " +
                package_name + ".");
        }
    }
    if(!g_state.metadata_sessions.empty()) {
        throw std::logic_error("Unconsumed metadata session script.");
    }
    if(!g_state.source_executions.empty()) {
        throw std::logic_error("Unconsumed source execution script.");
    }
    if(!g_state.info_many_scripts.empty()) {
        throw std::logic_error("Unconsumed AUR info_many script.");
    }
    if(!g_state.info_strict_scripts.empty()) {
        throw std::logic_error("Unconsumed AUR info_strict script.");
    }
    if(!g_state.vercmp_scripts.empty()) {
        throw std::logic_error("Unconsumed vercmp script.");
    }
    if(!g_state.database_path_scripts.empty()) {
        throw std::logic_error("Unconsumed Pacman database script.");
    }
    if(!g_state.aur_executions.empty()) {
        throw std::logic_error("Unconsumed AUR execution script.");
    }
}

} // namespace upgrade_all_operation_test_stub

SourcePreferenceDirectorySnapshot snapshot_source_preference_directory() {
    record_event(
        stub::EventKind::PreferenceDirectorySnapshot,
        "source-build.d");
    if(g_state.preference_directory_failure.has_value()) {
        throw std::runtime_error(*g_state.preference_directory_failure);
    }
    return g_state.preference_directory;
}

StrictSourcePreferenceResult read_source_preference_strict(
    const std::string& package_name) {
    g_state.strict_preference_reads.push_back(package_name);
    record_event(
        stub::EventKind::StrictPreferenceRead,
        package_name,
        {package_name});

    auto scripted = g_state.preference_results.find(package_name);
    if(scripted == g_state.preference_results.end() ||
       scripted->second.empty()) {
        // POLICY(#281): directory snapshotに載った明示sourceのunscripted readは
        // test setup漏れ。AUR build unitの通常のpreference absenceだけをdefault化する。
        if(is_registered_preference_name(package_name)) {
            fail_unexpected(
                "Unexpected strict source preference read for registered source " +
                package_name + ".");
        }
        return SourcePreferenceAbsent{};
    }

    StrictSourcePreferenceResult result =
        std::move(scripted->second.front());
    scripted->second.pop_front();
    if(g_state.after_next_strict_preference_read_hook) {
        std::function<void()> hook =
            std::move(g_state.after_next_strict_preference_read_hook);
        hook();
    }
    return result;
}

ResolvedSourceBuildIdentity resolve_source_build_identity(
    const std::string& package_name) {
    record_event(
        stub::EventKind::SourceIdentityResolution,
        package_name,
        {package_name});
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
    record_event(
        stub::EventKind::SourceWorkItemPreparation,
        identity.requested_name(),
        {identity.requested_name()});
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
    const ProviderSelectionCallback&) {
    return prepare_resolved_source_build_work_item(
        identity, std::move(environment), only_if_updated, needed);
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

PackageMetadataError::PackageMetadataError(PackageMetadataFailure failure)
    : std::runtime_error(failure.diagnostic),
      failure_(std::move(failure)) {
}

const PackageMetadataFailure& PackageMetadataError::failure() const noexcept {
    return failure_;
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
    record_event(
        stub::EventKind::MetadataSessionOpen,
        paths.db_path.string());
    stub::MetadataSessionScript script;
    if(g_state.metadata_sessions.empty()) {
        if(!g_state.preference_directory.entries.empty()) {
            fail_unexpected("Unexpected package metadata session open.");
        }
        // POLICY(#350): The default system-only fixture represents an empty
        // authoritative local database. Explicit queued scripts still own
        // changed snapshots and observation failures.
    } else {
        script = std::move(g_state.metadata_sessions.front());
        g_state.metadata_sessions.pop_front();
    }
    if(script.open_failure.has_value()) {
        throw PackageMetadataError(*script.open_failure);
    }
    return PackageMetadataSession(std::make_unique<Impl>(std::move(script)));
}

InstalledPackageQueryResult PackageMetadataSession::query_installed_package(
    const std::string& package_name) const {
    record_event(
        stub::EventKind::InstalledPackageQuery,
        package_name,
        {package_name});
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
    record_event(
        stub::EventKind::LocalPackageSnapshot,
        "local-packages");
    if(impl_ == nullptr) {
        return PackageMetadataFailure{
            PackageMetadataErrorCode::QueryFailed,
            "Stub package metadata session is closed."};
    }
    return impl_->script.local_package_snapshot;
}

void require_supported_separated_install_options(bool rm_deps) {
    g_state.supported_options_guards.push_back(rm_deps);
    record_event(
        stub::EventKind::SeparatedInstallOptionsGuard,
        rm_deps ? "rm_deps=true" : "rm_deps=false");
    if(g_state.supported_options_failure.has_value()) {
        throw std::runtime_error(*g_state.supported_options_failure);
    }
}

void require_unclaimed_artifact_pkgdest(
    const SourceBuildEnvironment& environment) {
    g_state.pkgdest_guards.push_back(environment);
    record_event(
        stub::EventKind::ArtifactPkgdestGuard,
        environment_subject(environment));

    if(g_state.source_invocation_failure.has_value()) {
        throw std::runtime_error(*g_state.source_invocation_failure);
    }
    if(g_state.pkgdest_failure_call.has_value() &&
       g_state.pkgdest_guards.size() == *g_state.pkgdest_failure_call) {
        throw std::runtime_error(g_state.pkgdest_failure_diagnostic);
    }
}

std::shared_ptr<ReviewedSourceFatalStatePreflightSlot>
preflight_reviewed_source_fatal_state_for_production(
    const SourceBuildRequest&) {
    return nullptr;
}

PacmanDatabasePaths resolve_pacman_database_paths() {
    ++g_state.database_calls;
    record_event(
        stub::EventKind::PacmanDatabaseResolution,
        "pacman-database");

    DatabasePathsScript script = g_state.database_paths;
    if(!g_state.database_path_scripts.empty()) {
        script = std::move(g_state.database_path_scripts.front());
        g_state.database_path_scripts.pop_front();
    }
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&script)) {
        throw PackageMetadataError(*failure);
    }
    return std::get<PacmanDatabasePaths>(std::move(script));
}

int run_command(const std::string& command) {
    g_state.system_commands.push_back(command);
    record_event(stub::EventKind::SystemCommand, command);
    if(g_state.system_failure.has_value()) {
        throw std::runtime_error(*g_state.system_failure);
    }
    if(g_state.after_system_command_hook) {
        std::function<void()> hook =
            std::move(g_state.after_system_command_hook);
        hook();
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
            "Upgrade-all registered repository preparation received an inconsistent PackageBase work item.");
    }
    const PacmanDatabasePaths database_paths{
        "/upgrade-all-stub/root", "/upgrade-all-stub/database"};
    const stub::ConfigSnapshot config_snapshot = snapshot_config(config);
    g_state.source_preparation_calls.push_back(stub::SourcePreparationCall{
        work_item.request.package_name,
        work_item.request.checkout_name,
        work_item.request,
        update_policy,
        database_paths,
        config_snapshot});
    record_event(
        stub::EventKind::SourcePreparation,
        work_item.request.package_name,
        {work_item.request.package_name});
    // 既存aggregate testのsource開始観測はpreparation開始へ対応させる。
    g_state.source_calls.push_back(stub::SourceExecutionCall{
        work_item.request.package_name,
        work_item.request.checkout_name,
        work_item.request,
        database_paths,
        config_snapshot});
    record_event(
        stub::EventKind::SourceExecution,
        work_item.request.package_name,
        {work_item.request.package_name});
    if(g_state.source_executions.empty()) {
        fail_unexpected(
            "Unexpected upgrade-all registered repository preparation for " +
            work_item.request.package_name + ".");
    }
    ScriptedSourceExecution& execution = g_state.source_executions.front();
    if(execution.kind == ScriptedSourceExecutionKind::Success) {
        if(execution.result.status == SourceBuildExecutionStatus::UpToDate) {
            SourceBuildUpToDate outcome{
                std::move(execution.result.diagnostic), std::nullopt};
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
                std::move(execution.result.diagnostic), std::nullopt};
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
    if(work_item.required_targets.size() != 1) {
        fail_unexpected(
            "Upgrade-all registered repository PackageBase execution received an inconsistent target.");
    }
    g_state.package_base_source_calls.push_back(
        stub::PackageBaseSourceExecutionCall{
            work_item.request.package_name,
            work_item.request.checkout_name,
            work_item.request,
            database_paths,
            snapshot_config(config)});
    record_event(
        stub::EventKind::PackageBaseSourceExecution,
        work_item.request.package_name,
        {work_item.request.package_name});
    if(g_state.source_executions.empty()) {
        fail_unexpected(
            "Unexpected upgrade-all registered repository PackageBase execution for " +
            work_item.request.package_name + ".");
    }
    ScriptedSourceExecution execution =
        std::move(g_state.source_executions.front());
    g_state.source_executions.pop_front();
    const RequiredPackageArtifactTarget& required =
        work_item.required_targets.front();
    const auto result = [&](ArtifactInstallExecutionOutcome outcome) {
        std::string package_base = work_item.request.checkout_name;
        ArtifactPackageIdentity selected_child{
            required.package_name, "2.0-1"};
        std::vector<ArtifactPackageIdentity> unselected_artifacts;
        if(execution.package_base_execution.has_value()) {
            package_base =
                execution.package_base_execution->package_base;
            selected_child =
                execution.package_base_execution->selected_child;
            unselected_artifacts =
                execution.package_base_execution->unselected_artifacts;
        }
        return RegisteredSourcePackageBaseExecutionResult(
            std::move(package_base),
            std::vector<PackageBaseSourceBuildSelectedResult>{
                PackageBaseSourceBuildSelectedResult{
                    std::move(selected_child),
                    required.desired_reason,
                    outcome}},
            std::move(unselected_artifacts));
    };
    switch(execution.kind) {
        case ScriptedSourceExecutionKind::Success:
            if(execution.result.status == SourceBuildExecutionStatus::Installed) {
                return result(ArtifactInstallExecutionOutcome::Installed);
            }
            if(execution.result.status ==
               SourceBuildExecutionStatus::SkippedAsNeeded) {
                return result(ArtifactInstallExecutionOutcome::SkippedAsNeeded);
            }
            fail_unexpected(
                "Prepared registered repository execution received a closed preparation outcome.");
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
                result(execution.cleanup_outcome), execution.diagnostic);
        case ScriptedSourceExecutionKind::UnknownFailure:
            throw UnknownSourceExecutionFailure{};
    }
    throw std::logic_error(
        "Unknown upgrade-all registered repository execution script.");
}

SourceBuildExecutionResult execute_prepared_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    if(work_item.required_targets.size() != 1 ||
       work_item.request.package_name.empty() ||
       work_item.request.package_name !=
           work_item.required_targets.front().package_name ||
       work_item.request.checkout_name !=
           work_item.required_targets.front().package_base) {
        fail_unexpected(
            "Upgrade-all registered-source singular execution received an inconsistent required target.");
    }
    g_state.source_calls.push_back(stub::SourceExecutionCall{
        work_item.request.package_name,
        work_item.request.checkout_name,
        work_item.request,
        database_paths,
        snapshot_config(config)});
    record_event(
        stub::EventKind::SourceExecution,
        work_item.request.package_name,
        {work_item.request.package_name});
    if(g_state.source_executions.empty()) {
        fail_unexpected(
            "Unexpected registered-source execution for " +
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
    }
    throw std::logic_error("Unknown registered-source execution script.");
}

PacmanRepositoryConfiguration resolve_pacman_repository_configuration() {
    ++g_state.repository_configuration_call_count;
    record_event(
        stub::EventKind::RepositoryConfigurationResolution,
        "repository-configuration");

    if(const auto* failure = std::get_if<PackageMetadataFailure>(
           &g_state.repository_configuration)) {
        throw PackageMetadataError(*failure);
    }
    return std::get<PacmanRepositoryConfiguration>(
        g_state.repository_configuration);
}

ForeignPackageInventoryResult query_foreign_package_inventory(
    const PacmanRepositoryConfiguration& configuration) {
    g_state.inventory_configurations.push_back(configuration);
    record_event(
        stub::EventKind::ForeignInventoryQuery,
        "foreign-inventory");

    if(const auto* failure = std::get_if<PackageMetadataFailure>(
           &g_state.foreign_inventory)) {
        return *failure;
    }
    ForeignPackageInventory inventory =
        std::get<ForeignPackageInventory>(g_state.foreign_inventory);
    if(g_state.after_foreign_inventory_hook) {
        std::function<void()> hook =
            std::move(g_state.after_foreign_inventory_hook);
        hook();
    }
    return inventory;
}

InstalledPackageRelationInventoryResult query_installed_package_relations(
    const PacmanDatabasePaths& paths) {
    ++g_state.installed_relation_inventory_call_count;
    record_event(
        stub::EventKind::InstalledRelationInventoryQuery,
        "installed-relations");

    const auto* configuration =
        std::get_if<PacmanRepositoryConfiguration>(
            &g_state.repository_configuration);
    if(configuration == nullptr ||
       configuration->database_paths.root_dir != paths.root_dir ||
       configuration->database_paths.db_path != paths.db_path) {
        fail_unexpected(
            "Installed relation query received different database paths.");
    }
    return g_state.installed_relation_inventory;
}

InstalledPackageRuntimeDependencyMetadataInventoryResult
query_installed_package_runtime_dependency_metadata(
    const PacmanDatabasePaths& paths) {
    ++g_state.installed_runtime_dependency_inventory_call_count;
    record_event(
        stub::EventKind::InstalledRuntimeDependencyInventoryQuery,
        "installed-runtime-dependencies");

    const auto* configuration =
        std::get_if<PacmanRepositoryConfiguration>(
            &g_state.repository_configuration);
    if(configuration == nullptr ||
       configuration->database_paths.root_dir != paths.root_dir ||
       configuration->database_paths.db_path != paths.db_path) {
        fail_unexpected(
            "Installed runtime dependency query received different database paths.");
    }
    return g_state.installed_runtime_dependency_inventory;
}

StrictRepositoryPackageQueryResult query_repository_package_strict(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& package_name) {
    g_state.repository_candidate_calls.push_back(package_name);
    record_event(
        stub::EventKind::RepositoryCandidateQuery,
        package_name,
        {package_name});

    const auto* expected =
        std::get_if<PacmanRepositoryConfiguration>(
            &g_state.repository_configuration);
    if(expected == nullptr ||
       expected->repository_names != configuration.repository_names ||
       expected->database_paths.root_dir !=
           configuration.database_paths.root_dir ||
       expected->database_paths.db_path !=
           configuration.database_paths.db_path) {
        fail_unexpected(
            "Repository candidate query received a different configuration.");
    }
    const auto result =
        g_state.repository_candidate_results.find(package_name);
    return result == g_state.repository_candidate_results.end()
               ? StrictRepositoryPackageQueryResult{
                     RepositoryPackageNotFound{
                         configuration.repository_names}}
               : result->second;
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    g_state.info_many_calls.push_back(package_names);
    record_event(
        stub::EventKind::AurInfoMany,
        "aur-info-many",
        package_names);

    InfoManyScript script = take_script(
        g_state.info_many_scripts,
        "Unexpected AurClient::info_many call with no pending script.");
    if(const auto* failure = std::get_if<ScriptFailure>(&script)) {
        throw std::runtime_error(failure->diagnostic);
    }
    if(const auto* failure = std::get_if<ResponseScriptFailure>(&script)) {
        throw AurRpcResponseError(failure->diagnostic);
    }
    std::map<std::string, AurPackageInfo> result =
        std::get<std::map<std::string, AurPackageInfo>>(
            std::move(script));
    if(g_state.after_info_many_hook) {
        std::function<void()> hook =
            std::move(g_state.after_info_many_hook);
        hook();
    }
    return result;
}

std::optional<AurPackageInfo> AurClient::info_strict(
    const std::string& package_name) {
    g_state.info_strict_calls.push_back(package_name);
    record_event(
        stub::EventKind::AurInfoStrict,
        package_name,
        {package_name});

    InfoStrictScript script = take_script(
        g_state.info_strict_scripts,
        "Unexpected AurClient::info_strict call with no pending script.");
    if(const auto* failure = std::get_if<ScriptFailure>(&script)) {
        throw std::runtime_error(failure->diagnostic);
    }
    if(const auto* failure = std::get_if<ResponseScriptFailure>(&script)) {
        throw AurRpcResponseError(failure->diagnostic);
    }
    return std::get<std::optional<AurPackageInfo>>(std::move(script));
}

std::string exec_command(const char* command) {
    if(command == nullptr) {
        fail_unexpected("AUR query stub received a null command.");
    }

    const std::string command_string(command);
    g_state.vercmp_calls.push_back(command_string);
    record_event(stub::EventKind::VersionCompare, command_string);

    VercmpScript script = take_script(
        g_state.vercmp_scripts,
        "Unexpected vercmp call with no pending script.");
    if(const auto* failure = std::get_if<ScriptFailure>(&script)) {
        throw std::runtime_error(failure->diagnostic);
    }
    return std::get<std::string>(std::move(script));
}

BuildPlan resolve_build_plan_for_preflight(
    const std::vector<std::string>& targets) {
    return resolve_build_plan_for_preflight(
        targets, ProviderSelectionCallback{});
}

BuildPlan resolve_build_plan_for_preflight(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    g_state.resolver_calls.push_back(targets);
    record_event(
        stub::EventKind::BuildPlanResolution,
        "build-plan",
        targets);
    if(!g_state.resolver_handler) {
        fail_unexpected(
            "Unexpected AUR preflight resolver call with no handler.");
    }
    return g_state.resolver_handler(targets, select_provider);
}

BuildPlan resolve_build_plan(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    return resolve_build_plan_for_preflight(
        targets, select_provider);
}

const std::string&
PackageBaseSourceBuildExecutionResult::package_base() const noexcept {
    return package_base_;
}

const ProductionSourceBuildProvenance&
PackageBaseSourceBuildExecutionResult::source_provenance() const noexcept {
    return production_outcome_.source_provenance;
}

const ProductionSourceBuildStagedOutcome&
PackageBaseSourceBuildExecutionResult::production_outcome()
    const noexcept {
    return production_outcome_;
}

const std::vector<PackageBaseSourceBuildSelectedResult>&
PackageBaseSourceBuildExecutionResult::selected_children() const noexcept {
    return selected_children_;
}

const std::vector<ArtifactPackageIdentity>&
PackageBaseSourceBuildExecutionResult::unselected_artifacts() const noexcept {
    return unselected_artifacts_;
}

bool PackageBaseSourceBuildExecutionResult::installed_any() const noexcept {
    return std::any_of(
        selected_children_.begin(), selected_children_.end(),
        [](const PackageBaseSourceBuildSelectedResult& child) {
            return child.outcome ==
                   ArtifactInstallExecutionOutcome::Installed;
        });
}

bool PackageBaseSourceBuildExecutionResult::all_skipped_as_needed()
    const noexcept {
    return !selected_children_.empty() && std::all_of(
                                              selected_children_.begin(), selected_children_.end(),
                                              [](const PackageBaseSourceBuildSelectedResult& child) {
                                                  return child.outcome ==
                                                         ArtifactInstallExecutionOutcome::SkippedAsNeeded;
                                              });
}

std::string
PackageBaseSourceBuildExecutionResult::release_package_base() && noexcept {
    return std::move(package_base_);
}

std::vector<PackageBaseSourceBuildSelectedResult>
PackageBaseSourceBuildExecutionResult::release_selected_children() && noexcept {
    return std::move(selected_children_);
}

std::vector<ArtifactPackageIdentity>
PackageBaseSourceBuildExecutionResult::release_unselected_artifacts() && noexcept {
    return std::move(unselected_artifacts_);
}

SeparatedPackageBaseSourceBuildFailurePhase
SeparatedPackageBaseSourceBuildPhaseError::phase() const noexcept {
    return phase_;
}

const std::optional<ReviewedSourceProductionFailure>&
SeparatedPackageBaseSourceBuildPhaseError::reviewed_source_failure()
    const noexcept {
    return reviewed_source_failure_;
}

const std::optional<PackageMetadataFailure>&
SeparatedPackageBaseSourceBuildPhaseError::package_metadata_failure()
    const noexcept {
    return package_metadata_failure_;
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPhaseError::production_outcome()
    const noexcept {
    return production_outcome_;
}

const PackageBaseArtifactIdentitySelectionFailure*
PackageBaseArtifactInstallPreparationFailure::selection_failure()
    const noexcept {
    return std::get_if<PackageBaseArtifactIdentitySelectionFailure>(&failure_);
}

const MixedPackageBaseInstallReasonUnsupported*
PackageBaseArtifactInstallPreparationFailure::mixed_reason_failure()
    const noexcept {
    return std::get_if<MixedPackageBaseInstallReasonUnsupported>(&failure_);
}

const PackageBaseArtifactInstallPreparationFailure&
SeparatedPackageBaseSourceBuildPreparationError::failure() const noexcept {
    return failure_;
}

const PackageBaseArtifactIdentitySelectionFailure*
SeparatedPackageBaseSourceBuildPreparationError::selection_failure()
    const noexcept {
    return failure_.selection_failure();
}

const MixedPackageBaseInstallReasonUnsupported*
SeparatedPackageBaseSourceBuildPreparationError::mixed_reason_failure()
    const noexcept {
    return failure_.mixed_reason_failure();
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPreparationError::production_outcome()
    const noexcept {
    return production_outcome_;
}

const PackageBaseSourceBuildExecutionResult&
SeparatedPackageBaseSourceBuildCleanupError::result() const noexcept {
    return result_;
}

PackageBaseSourceBuildExecutionResult
SeparatedPackageBaseSourceBuildCleanupError::release_result() && noexcept {
    return std::move(result_);
}

PackageBaseArtifactInstallTransactionFailureKind
PackageBaseArtifactInstallTransactionError::failure_kind() const noexcept {
    return failure_kind_;
}

const std::string&
PackageBaseArtifactInstallTransactionError::package_base() const noexcept {
    return package_base_;
}

const std::vector<PackageBaseArtifactInstallTransactionAttempt>&
PackageBaseArtifactInstallTransactionError::attempts() const noexcept {
    return attempts_;
}

const std::optional<int>&
PackageBaseArtifactInstallTransactionError::exit_code() const noexcept {
    return exit_code_;
}

const std::optional<ProductionSourceBuildStagedOutcome>&
PackageBaseArtifactInstallTransactionError::production_outcome()
    const noexcept {
    return production_outcome_;
}

void PackageBaseArtifactInstallTransactionError::attach_production_outcome(
    ProductionSourceBuildStagedOutcome production_outcome) {
    if(production_outcome_.has_value()) {
        throw std::logic_error(
            "Scripted transaction failure already has a production outcome.");
    }
    production_outcome_ = std::move(production_outcome);
}

std::vector<PackageBaseArtifactInstallTransactionAttempt>
PackageBaseArtifactInstallTransactionError::release_attempts() && noexcept {
    return std::move(attempts_);
}

SourceBuildPackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    if(work_item.required_targets.empty()) {
        fail_unexpected(
            "AUR set execution received no required package target.");
    }
    std::vector<std::string> required_package_names;
    required_package_names.reserve(work_item.required_targets.size());
    for(const auto& target : work_item.required_targets) {
        required_package_names.push_back(target.package_name);
    }
    const std::string compatibility_package_name =
        work_item.required_targets.size() == 1
            ? work_item.required_targets.front().package_name
            : std::string{};

    // POLICY(#267): runnerが全work itemへ同じdatabase snapshot参照を渡す契約を
    // lifecycle差し替え側でも検証する。
    if(g_state.first_aur_database_paths_address == nullptr) {
        g_state.first_aur_database_paths_address = &database_paths;
    } else if(g_state.first_aur_database_paths_address != &database_paths) {
        g_state.expectation_failure =
            "AUR runner did not reuse one Pacman database snapshot.";
    }

    const std::size_t call_index = g_state.aur_calls.size();
    g_state.aur_calls.push_back(stub::AurExecutionCall{
        call_index,
        compatibility_package_name,
        work_item.request.checkout_name,
        required_package_names,
        work_item.required_targets,
        database_paths,
        snapshot_config(config),
        {}});

    if(g_state.aur_executions.empty()) {
        fail_unexpected(
            "Unexpected AUR source-build execution with no pending script.");
    }
    ScriptedAurExecution scripted =
        std::move(g_state.aur_executions.front());
    g_state.aur_executions.pop_front();

    // POLICY(#267): ordinary/unknown failureはbuild中、cleanup failureは
    // package transaction完了後としてcross-phase historyへ固定する。
    record_aur_lifecycle_event(call_index, stub::EventKind::AurCheckout);
    record_aur_lifecycle_event(call_index, stub::EventKind::AurBuild);
    switch(scripted.kind) {
        case ScriptedAurExecutionKind::Success:
        case ScriptedAurExecutionKind::CleanupFailure: {
            if(scripted.child_outcomes.size() !=
               work_item.required_targets.size()) {
                fail_unexpected(
                    "AUR set execution child outcome count differs from required targets.");
            }
            std::vector<PackageBaseSourceBuildSelectedResult>
                selected_children;
            selected_children.reserve(scripted.child_outcomes.size());
            for(std::size_t child_index = 0;
                child_index < scripted.child_outcomes.size();
                ++child_index) {
                const ArtifactInstallExecutionOutcome outcome =
                    scripted.child_outcomes[child_index];
                switch(outcome) {
                    case ArtifactInstallExecutionOutcome::Installed:
                    case ArtifactInstallExecutionOutcome::SkippedAsNeeded:
                        break;
                    default:
                        fail_unexpected(
                            "AUR set execution received an unknown child outcome.");
                }
                const RequiredPackageArtifactTarget& target =
                    work_item.required_targets[child_index];
                selected_children.push_back(
                    PackageBaseSourceBuildSelectedResult{
                        ArtifactPackageIdentity{
                            target.package_name, "2.0-1"},
                        target.desired_reason,
                        outcome});
            }
            PackageBaseSourceBuildExecutionResult result =
                PackageBaseSourceBuildExecutionResult::
                    make_for_aur_update_runner_test(
                        work_item.request.checkout_name,
                        std::move(selected_children),
                        std::move(
                            scripted.unselected_artifacts));
            record_aur_lifecycle_event(
                call_index, stub::EventKind::AurInstall);
            record_aur_lifecycle_event(
                call_index, stub::EventKind::AurCleanup);
            if(scripted.kind ==
               ScriptedAurExecutionKind::CleanupFailure) {
                throw SeparatedPackageBaseSourceBuildCleanupError::
                    make_for_aur_update_runner_test(
                        std::move(result),
                        scripted.diagnostic);
            }
            return result;
        }
        case ScriptedAurExecutionKind::OrdinaryFailure:
            throw std::runtime_error(scripted.diagnostic);
        case ScriptedAurExecutionKind::UnknownFailure:
            throw UnknownAurExecutionFailure{};
    }
    throw std::logic_error("Unknown AUR execution script.");
}

SelectedRepositoryProviderTransactionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig&) {
    // Aggregate tests cover orchestration composition; the focused runner and
    // system/source phase tests own provider-transaction ordering and failure.
    SelectedRepositoryProviderTransactionResult result;
    result.selected_providers = invocation.selected_repository_providers;
    if(!result.selected_providers.empty()) {
        result.status =
            SelectedRepositoryProviderTransactionStatus::Succeeded;
        result.package_state_change = PackageStateChange::Unknown;
        result.command_exit_status = 0;
    }
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
    if(g_state.after_next_cache_seed_hook) {
        std::function<void()> hook =
            std::move(g_state.after_next_cache_seed_hook);
        hook();
    }
}

void activate_production_source_build_cache(
    PreparedProductionSourceBuildInvocation& invocation) {
    if(g_state.cache_activation_failure) {
        throw TrustedCacheError(TrustedCacheFailure{
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::ConcurrentReplacement,
            std::nullopt});
    }
    if(!invocation.cache_root.has_value()) {
        throw std::logic_error(
            "Upgrade-all operation lost its shared cache authority.");
    }
    seed_production_source_build_cache(
        invocation, invocation.cache_root.value());
}
