#include "system_source_upgrade.hpp"

#include "app_config.hpp"
#include "cache_authority.hpp"
#include "dependency_plan.hpp"
#include "interactive_confirmation.hpp"
#include "localization.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "separated_source_build.hpp"
#include "shell_words.hpp"
#include "source_build.hpp"
#include "source_install.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

std::string unknown_system_failure_diagnostic() {
    return localization::translate_message(
        "The system upgrade failed with an unknown exception.");
}

std::string unknown_source_failure_diagnostic() {
    return localization::translate_message(
        "Registered source package processing failed with an unknown exception.");
}

std::string join_package_names(
    const std::vector<std::string>& package_names) {
    std::string joined;
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        if(index > 0) joined += ", ";
        joined += package_names[index];
    }
    return joined;
}

std::string post_upgrade_snapshot_failure_diagnostic(
    const std::string& detail) {
    return localization::format_translated_message(
        "The system upgrade completed, but the post-upgrade package metadata snapshot failed: {}",
        detail);
}

ProviderSelectionCallback registered_source_provider_selection_callback(
    ProviderSelectionCallback select_provider) {
    if(!select_provider) return {};
    return [select_provider = std::move(select_provider)](
               const std::string& dependency,
               const std::vector<ProvidedDependency>& candidates)
               -> std::optional<ProvidedDependency> {
        const bool has_aur_candidate = std::any_of(
            candidates.begin(), candidates.end(),
            [](const ProvidedDependency& candidate) {
                return std::holds_alternative<AurProviderOrigin>(
                    candidate.origin);
            });
        if(has_aur_candidate) return std::nullopt;
        return select_provider(dependency, candidates);
    };
}

using SourceUpdateBaselines = std::map<std::string, SourceUpdateBaseline>;
using SourceInstalledSnapshotResult = std::variant<
    SourceInstalledSnapshot,
    PackageMetadataFailure>;
using SourceInstalledSnapshotResults =
    std::map<std::string, SourceInstalledSnapshotResult>;

struct RegisteredSourceCorrelation {
    std::size_t snapshot_index = 0;
    bool has_valid_package_name = false;
    std::optional<std::size_t> work_item_index;
};

struct SystemSourceUpgradePreparationState {
    SystemSourceUpgradePreparedSnapshot snapshot;
    std::optional<BuildPlan> aur_invocation_plan;
    std::optional<PreparedProductionSourceBuildInvocation> source_invocation;
    std::vector<RegisteredSourceCorrelation> correlations;
    SourceUpdateBaselines update_baselines;
    std::optional<PacmanDatabasePaths> system_database_paths;
    std::optional<LocalPackageVersionSnapshot> before_system_snapshot;
    SystemUpgradePhaseResult system;
    std::vector<SystemSourceUpgradeWarning> warnings;
    std::vector<SystemSourceUpgradeIssue> issues;
    std::vector<SystemSourceUpgradeDiagnostic> diagnostics;
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
    std::optional<SystemSourceUpgradeUnexpectedExceptionPoint>
        unexpected_exception_point;
    bool unexpected_exception_is_unknown = false;
#endif
};

struct SourceBaselineFailure {
    std::size_t package_index = 0;
    PackageMetadataFailure failure;
};

void notify(
    const SystemSourceUpgradeEventObserver& observer,
    SystemSourceUpgradeEvent event) {
    if(observer) observer(event);
}

SystemSourceUpgradeOptionSnapshot snapshot_options(const AppConfig& config) {
    return SystemSourceUpgradeOptionSnapshot{
        config.user_config.review.pkgbuild == ReviewPolicy::Skip,
        config.user_config.review.diff == ReviewPolicy::Skip,
        config.no_confirm,
        config.user_config.build.mode == BuildMode::Rebuild,
        config.user_config.build.mode == BuildMode::Clean,
        config.rm_deps,
        config.editor, false};
}

bool options_match(
    const SystemSourceUpgradeOptionSnapshot& snapshot,
    const AppConfig& config) noexcept {
    return snapshot.no_edit ==
               (config.user_config.review.pkgbuild == ReviewPolicy::Skip) &&
           snapshot.no_diff ==
               (config.user_config.review.diff == ReviewPolicy::Skip) &&
           snapshot.no_confirm == config.no_confirm &&
           snapshot.rebuild ==
               (config.user_config.build.mode == BuildMode::Rebuild) &&
           snapshot.clean_build ==
               (config.user_config.build.mode == BuildMode::Clean) &&
           snapshot.rm_deps == config.rm_deps &&
           snapshot.editor == config.editor && !snapshot.needed;
}

std::string system_upgrade_command(const AppConfig& config) {
    std::vector<std::string> arguments = {"-Syu"};
    if(config.no_confirm) arguments.push_back("--noconfirm");
    return "sudo pacman " + shell_words::join(arguments);
}

SourceInstalledSnapshotResult map_source_installed_snapshot(
    const InstalledPackageQueryResult& result) {
    if(const auto* metadata = std::get_if<InstalledPackageMetadata>(&result)) {
        return SourceInstalledSnapshot{metadata->version};
    }
    if(std::holds_alternative<PackageNotFound>(result)) {
        return SourceInstalledSnapshot{std::nullopt};
    }
    return std::get<PackageMetadataFailure>(result);
}

std::optional<SourceBaselineFailure> snapshot_source_update_baselines(
    const PackageMetadataSession& session,
    const std::vector<std::string>& package_names,
    SourceUpdateBaselines& baselines) {
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        const std::string& package_name = package_names[index];
        SourceInstalledSnapshotResult snapshot_result =
            map_source_installed_snapshot(
                session.query_installed_package(package_name));
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&snapshot_result)) {
            return SourceBaselineFailure{
                index,
                PackageMetadataFailure{
                    failure->code,
                    localization::format_translated_message(
                        "Failed to query installed package metadata for {}: {}",
                        package_name, failure->diagnostic)}};
        }

        const auto& snapshot =
            std::get<SourceInstalledSnapshot>(snapshot_result);
        baselines.emplace(
            package_name,
            SourceUpdateBaseline{snapshot.installed_version});
    }
    return std::nullopt;
}

SourceInstalledSnapshotResults snapshot_post_upgrade_installed_packages(
    const PackageMetadataSession& session,
    const std::vector<std::string>& package_names) {
    SourceInstalledSnapshotResults snapshots;
    for(const auto& package_name : package_names) {
        snapshots.emplace(
            package_name,
            map_source_installed_snapshot(
                session.query_installed_package(package_name)));
    }
    return snapshots;
}

RegisteredSourceUpgradeResult make_not_attempted_source_result(
    const RegisteredSourcePreferenceSnapshot& source) {
    RegisteredSourceUpgradeResult result;
    result.original_preference_index = source.original_preference_index;
    result.preference_package_name = source.preference_package_name;
    result.canonical_source_identity_key =
        source.canonical_source_identity_key;
    result.resolved_package_base = source.resolved_package_base;
    return result;
}

SystemSourceUpgradeResult make_result_from_state(
    SystemSourceUpgradePreparationState&& state,
    SystemSourceUpgradeStatus status,
    SystemSourceUpgradePhase stopped_phase) {
    SystemSourceUpgradeResult result;
    result.status = status;
    result.stopped_phase = stopped_phase;
    result.prepared_snapshot = std::move(state.snapshot);
    result.system = std::move(state.system);
    result.warnings = std::move(state.warnings);
    result.issues = std::move(state.issues);
    result.diagnostics = std::move(state.diagnostics);
    result.aur_invocation_plan = std::move(state.aur_invocation_plan);
    result.registered_source_results.reserve(
        result.prepared_snapshot.registered_sources.size());
    for(const auto& source : result.prepared_snapshot.registered_sources) {
        result.registered_source_results.push_back(
            make_not_attempted_source_result(source));
    }
    return result;
}

SystemSourceUpgradeIssue make_issue(
    SystemSourceUpgradeIssueKind kind,
    SystemSourceUpgradeIssueImpact impact,
    SystemSourceUpgradePhase phase,
    std::string diagnostic) {
    SystemSourceUpgradeIssue issue;
    issue.kind = kind;
    issue.impact = impact;
    issue.phase = phase;
    issue.diagnostic = std::move(diagnostic);
    return issue;
}

void attribute_issue_to_source(
    SystemSourceUpgradeIssue& issue,
    const RegisteredSourcePreferenceSnapshot& source) {
    issue.original_preference_index = source.original_preference_index;
    issue.preference_package_name = source.preference_package_name;
    issue.resolved_package_base = source.resolved_package_base;
}

SystemSourceUpgradeDiagnostic make_diagnostic(
    SystemSourceUpgradePhase phase,
    std::string diagnostic,
    bool stops_execution) {
    SystemSourceUpgradeDiagnostic detail;
    detail.phase = phase;
    detail.stops_execution = stops_execution;
    detail.diagnostic = std::move(diagnostic);
    return detail;
}

#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
struct UnknownSystemSourceTestException {};

void throw_unexpected_exception_for_test(
    const SystemSourceUpgradePreparationState& state,
    SystemSourceUpgradeUnexpectedExceptionPoint point) {
    if(!state.unexpected_exception_point.has_value() ||
       *state.unexpected_exception_point != point) {
        return;
    }
    if(state.unexpected_exception_is_unknown) {
        throw UnknownSystemSourceTestException{};
    }
    // NO_TRANSLATE(Issue #308): this is a test-only injected exception value.
    throw std::runtime_error(
        "fixture unexpected system/source execution exception");
}
#endif

void record_unexpected_execution_failure(
    SystemSourceUpgradeResult& result,
    SystemSourceUpgradePhase active_phase,
    std::optional<std::size_t> active_source_position,
    bool system_package_state_finalized,
    const std::string& exception_diagnostic) {
    result.status = SystemSourceUpgradeStatus::InconsistentResult;
    result.stopped_phase = active_phase;

    std::string diagnostic = exception_diagnostic.empty()
                                 ? localization::translate_message(
                                       "The system/source result is inconsistent after an unexpected exception.")
                                 : localization::format_translated_message(
                                       "The system/source result is inconsistent after an unexpected exception: {}",
                                       exception_diagnostic);

    if(active_phase == SystemSourceUpgradePhase::System &&
       result.system.status == SystemUpgradePhaseStatus::NotAttempted) {
        const std::string unavailable = exception_diagnostic.empty()
                                            ? localization::translate_message(
                                                  "The system result is unavailable because an unexpected exception occurred after the phase started.")
                                            : localization::format_translated_message(
                                                  "The system result is unavailable because an unexpected exception occurred after the phase started: {}",
                                                  exception_diagnostic);
        result.system.status = SystemUpgradePhaseStatus::Failed;
        result.system.package_state_change = PackageStateChange::Unknown;
        result.system.diagnostic = unavailable;
        diagnostic = unavailable;
    } else if(result.system.status == SystemUpgradePhaseStatus::Completed &&
              !system_package_state_finalized) {
        // system command completionは既知でも、post-state snapshot前なら
        // package mutationの有無は断言しない。
        result.system.package_state_change = PackageStateChange::Unknown;
    }

    if(active_phase == SystemSourceUpgradePhase::RegisteredSource &&
       active_source_position.has_value() &&
       *active_source_position < result.registered_source_results.size()) {
        RegisteredSourceUpgradeResult& source =
            result.registered_source_results[*active_source_position];
        if(source.status == RegisteredSourceUpgradeStatus::NotAttempted) {
            const std::string unavailable = exception_diagnostic.empty()
                                                ? localization::translate_message(
                                                      "The registered source result is unavailable because an unexpected exception occurred after the phase started.")
                                                : localization::format_translated_message(
                                                      "The registered source result is unavailable because an unexpected exception occurred after the phase started: {}",
                                                      exception_diagnostic);
            source.status = RegisteredSourceUpgradeStatus::Incomplete;
            source.failure_kind =
                RegisteredSourceUpgradeFailureKind::UnknownException;
            source.package_state_change = PackageStateChange::Unknown;
            source.diagnostic = unavailable;
        }
    }

    SystemSourceUpgradeDiagnostic detail = make_diagnostic(
        active_phase, std::move(diagnostic), true);
    if(active_source_position.has_value() &&
       *active_source_position < result.registered_source_results.size()) {
        const RegisteredSourceUpgradeResult& source =
            result.registered_source_results[*active_source_position];
        detail.original_preference_index = source.original_preference_index;
        detail.preference_package_name = source.preference_package_name;
        detail.resolved_package_base = source.resolved_package_base;
    }
    result.diagnostics.push_back(std::move(detail));
}

SystemSourceUpgradeResult block_preparation(
    SystemSourceUpgradePreparationState&& state,
    SystemSourceUpgradeIssue issue,
    std::optional<std::size_t> source_position,
    RegisteredSourceUpgradeFailureKind source_failure_kind) {
    SystemSourceUpgradeDiagnostic diagnostic = make_diagnostic(
        SystemSourceUpgradePhase::Preparation,
        issue.diagnostic,
        true);
    diagnostic.original_preference_index = issue.original_preference_index;
    diagnostic.preference_package_name = issue.preference_package_name;
    diagnostic.resolved_package_base = issue.resolved_package_base;
    state.diagnostics.push_back(std::move(diagnostic));
    state.issues.push_back(std::move(issue));

    SystemSourceUpgradeResult result = make_result_from_state(
        std::move(state),
        SystemSourceUpgradeStatus::BlockedBeforeMutation,
        SystemSourceUpgradePhase::Preparation);
    if(source_position.has_value() &&
       source_position.value() < result.registered_source_results.size()) {
        RegisteredSourceUpgradeResult& source_result =
            result.registered_source_results[source_position.value()];
        source_result.status = RegisteredSourceUpgradeStatus::Incomplete;
        source_result.failure_kind = source_failure_kind;
        source_result.diagnostic = result.diagnostics.back().diagnostic;
    }
    return result;
}

SystemSourceUpgradeIssue make_cache_authority_issue(
    std::string diagnostic) {
    return make_issue(
        SystemSourceUpgradeIssueKind::CacheAuthorityInvalid,
        SystemSourceUpgradeIssueImpact::BlocksExecution,
        SystemSourceUpgradePhase::Preparation,
        std::move(diagnostic));
}

SystemSourceUpgradeResult block_cache_authority_preparation(
    SystemSourceUpgradePreparationState&& state,
    SystemSourceUpgradeIssue issue) {
    return block_preparation(
        std::move(state), std::move(issue), std::nullopt,
        RegisteredSourceUpgradeFailureKind::CacheAuthorityFailure);
}

void record_system_snapshot_failure(
    SystemSourceUpgradePreparationState& state,
    PackageMetadataFailure failure) {
    const std::string diagnostic =
        localization::format_translated_message(
            "Failed to snapshot local package versions before the system upgrade: {}",
            failure.diagnostic);
    state.system.before_snapshot_failure = failure;

    SystemSourceUpgradeIssue issue = make_issue(
        SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable,
        SystemSourceUpgradeIssueImpact::ObservabilityOnly,
        SystemSourceUpgradePhase::System,
        diagnostic);
    issue.package_metadata_failure = std::move(failure);
    state.issues.push_back(std::move(issue));
    state.diagnostics.push_back(make_diagnostic(
        SystemSourceUpgradePhase::System,
        diagnostic,
        false));
}

void record_post_system_snapshot_failure(
    SystemSourceUpgradeResult& result,
    PackageMetadataFailure failure) {
    const std::string diagnostic =
        localization::format_translated_message(
            "Failed to snapshot local package versions after the system upgrade: {}",
            failure.diagnostic);
    result.system.after_snapshot_failure = failure;
    result.system.package_state_change = PackageStateChange::Unknown;

    SystemSourceUpgradeIssue issue = make_issue(
        SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable,
        SystemSourceUpgradeIssueImpact::ObservabilityOnly,
        SystemSourceUpgradePhase::System,
        diagnostic);
    issue.package_metadata_failure = std::move(failure);
    result.issues.push_back(std::move(issue));
    result.diagnostics.push_back(make_diagnostic(
        SystemSourceUpgradePhase::System,
        diagnostic,
        false));
}

PackageMetadataFailure generic_metadata_failure(
    const std::string& diagnostic) {
    return PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        diagnostic};
}

void prepare_system_only_package_observation(
    SystemSourceUpgradePreparationState& state) {
    try {
        state.system_database_paths = resolve_pacman_database_paths();
        PackageMetadataSession session = PackageMetadataSession::open(
            state.system_database_paths.value());
        LocalPackageVersionSnapshotResult snapshot =
            session.snapshot_local_package_versions();
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&snapshot)) {
            record_system_snapshot_failure(state, *failure);
            return;
        }
        state.before_system_snapshot =
            std::get<LocalPackageVersionSnapshot>(std::move(snapshot));
    } catch(const PackageMetadataError& error) {
        record_system_snapshot_failure(state, error.failure());
    } catch(const std::exception& error) {
        record_system_snapshot_failure(
            state, generic_metadata_failure(error.what()));
    } catch(...) {
        record_system_snapshot_failure(
            state,
            generic_metadata_failure(localization::translate_message(
                "An unknown exception occurred.")));
    }
}

bool has_non_success_source_status(
    RegisteredSourceUpgradeStatus status) noexcept {
    return status != RegisteredSourceUpgradeStatus::Updated &&
           status != RegisteredSourceUpgradeStatus::NoChange;
}

bool validate_prepared_correlation(
    const SystemSourceUpgradePreparedSnapshot& snapshot,
    const std::vector<RegisteredSourceCorrelation>& correlations,
    const std::optional<PreparedProductionSourceBuildInvocation>& invocation) {
    if(correlations.size() != snapshot.registered_sources.size()) return false;

    std::set<std::size_t> work_item_indices;
    std::size_t valid_source_count = 0;
    for(std::size_t index = 0; index < correlations.size(); ++index) {
        const RegisteredSourceCorrelation& correlation = correlations[index];
        const RegisteredSourcePreferenceSnapshot& source =
            snapshot.registered_sources[index];
        if(correlation.snapshot_index != index) return false;
        if(correlation.has_valid_package_name !=
           is_valid_package_name(source.preference_package_name)) {
            return false;
        }
        if(!correlation.has_valid_package_name) {
            if(correlation.work_item_index.has_value()) return false;
            continue;
        }

        ++valid_source_count;
        if(!correlation.work_item_index.has_value() ||
           !source.environment.has_value() ||
           !source.canonical_source_identity_key.has_value() ||
           !source.resolved_package_base.has_value() ||
           !invocation.has_value()) {
            return false;
        }

        const std::size_t work_item_index =
            correlation.work_item_index.value();
        if(work_item_index >= invocation->work_items.size() ||
           !work_item_indices.insert(work_item_index).second) {
            return false;
        }
        const ProductionSourceBuildWorkItem& work_item =
            invocation->work_items[work_item_index];
        if(work_item.request.package_name != source.preference_package_name ||
           work_item.request.checkout_name !=
               source.resolved_package_base.value() ||
           source.required_target_provenance !=
               std::optional<RequiredTargetProvenance>{
                   work_item.required_target_provenance} ||
           source.artifact_lifecycle_intent !=
               std::optional<ArtifactLifecycleIntent>{
                   work_item.artifact_lifecycle_intent} ||
           source.repository_identity != work_item.repository_identity) {
            return false;
        }
    }

    if(valid_source_count == 0) return !invocation.has_value();
    return invocation.has_value() &&
           invocation->work_items.size() == valid_source_count &&
           work_item_indices.size() == valid_source_count;
}

void add_inconsistent_issue(
    SystemSourceUpgradeResult& result,
    SystemSourceUpgradeIssueKind kind,
    SystemSourceUpgradePhase phase,
    std::string diagnostic) {
    result.status = SystemSourceUpgradeStatus::InconsistentResult;
    result.stopped_phase = phase;
    result.issues.push_back(make_issue(
        kind,
        SystemSourceUpgradeIssueImpact::BlocksExecution,
        phase,
        diagnostic));
    result.diagnostics.push_back(make_diagnostic(
        phase,
        std::move(diagnostic),
        true));
}

void stop_for_source_metadata_failure(
    SystemSourceUpgradeResult& result,
    std::size_t source_position,
    PackageMetadataFailure failure,
    std::string diagnostic) {
    RegisteredSourceUpgradeResult& source_result =
        result.registered_source_results[source_position];
    source_result.status = RegisteredSourceUpgradeStatus::Incomplete;
    source_result.failure_kind =
        RegisteredSourceUpgradeFailureKind::PackageMetadataUnavailable;
    source_result.package_state_change = PackageStateChange::NoChange;
    source_result.diagnostic = diagnostic;
    const RegisteredSourcePreferenceSnapshot& prepared_source =
        result.prepared_snapshot.registered_sources[source_position];
    if(prepared_source.source_kind ==
       std::optional<SourceBuildSourceKind>{
           SourceBuildSourceKind::Repository}) {
        source_result.failure_detail.emplace<PackageMetadataFailure>(failure);
    }

    SystemSourceUpgradeIssue issue = make_issue(
        SystemSourceUpgradeIssueKind::PostSystemSourceSnapshotUnavailable,
        SystemSourceUpgradeIssueImpact::BlocksExecution,
        SystemSourceUpgradePhase::RegisteredSource,
        diagnostic);
    issue.original_preference_index = source_result.original_preference_index;
    issue.preference_package_name = source_result.preference_package_name;
    issue.resolved_package_base = source_result.resolved_package_base;
    issue.package_metadata_failure = std::move(failure);
    result.issues.push_back(std::move(issue));

    SystemSourceUpgradeDiagnostic detail = make_diagnostic(
        SystemSourceUpgradePhase::RegisteredSource,
        std::move(diagnostic),
        true);
    detail.original_preference_index = source_result.original_preference_index;
    detail.preference_package_name = source_result.preference_package_name;
    detail.resolved_package_base = source_result.resolved_package_base;
    result.diagnostics.push_back(std::move(detail));
    result.status = SystemSourceUpgradeStatus::StoppedOnSourceFailure;
    result.stopped_phase = SystemSourceUpgradePhase::RegisteredSource;
}

void stop_for_global_source_metadata_failure(
    SystemSourceUpgradeResult& result,
    std::optional<PackageMetadataFailure> failure,
    std::string diagnostic) {
    SystemSourceUpgradeIssue issue = make_issue(
        SystemSourceUpgradeIssueKind::PostSystemSourceSnapshotUnavailable,
        SystemSourceUpgradeIssueImpact::BlocksExecution,
        SystemSourceUpgradePhase::RegisteredSource,
        diagnostic);
    issue.package_metadata_failure = std::move(failure);
    result.issues.push_back(std::move(issue));
    result.diagnostics.push_back(make_diagnostic(
        SystemSourceUpgradePhase::RegisteredSource,
        std::move(diagnostic),
        true));
    result.status = SystemSourceUpgradeStatus::StoppedOnSourceFailure;
    result.stopped_phase = SystemSourceUpgradePhase::RegisteredSource;
}

void stop_for_repository_provider_transaction_failure(
    SystemSourceUpgradeResult& result,
    std::string diagnostic) {
    result.diagnostics.push_back(make_diagnostic(
        SystemSourceUpgradePhase::RegisteredSource,
        std::move(diagnostic), true));
    result.status = SystemSourceUpgradeStatus::StoppedOnSourceFailure;
    result.stopped_phase = SystemSourceUpgradePhase::RegisteredSource;
}

void map_source_execution_result(
    RegisteredSourceUpgradeResult& result,
    SourceBuildExecutionResult execution) {
    result.production_outcome = std::move(execution.production_outcome);
    result.diagnostic = execution.diagnostic.empty()
                            ? std::nullopt
                            : std::optional<std::string>(std::move(execution.diagnostic));
    result.cleanup_diagnostic = std::nullopt;
    result.devel_execution = std::move(execution.devel_execution);
    result.devel_update_query = execution.devel_update_query;
    result.devel_rebuild_confirmation = execution.devel_rebuild_confirmation;
    switch(execution.status) {
        case SourceBuildExecutionStatus::DevelRequiresCheckSkipped:
            result.status = RegisteredSourceUpgradeStatus::Incomplete;
            result.failure_kind = RegisteredSourceUpgradeFailureKind::DevelRequiresCheckSkipped;
            result.package_state_change = PackageStateChange::NoChange;
            return;
        case SourceBuildExecutionStatus::AuthoritativeIncomplete:
            result.status = RegisteredSourceUpgradeStatus::Incomplete;
            result.failure_kind = RegisteredSourceUpgradeFailureKind::AuthoritativeExecutionIncomplete;
            result.package_state_change = !result.devel_execution ? PackageStateChange::NoChange : result.devel_execution->operation == DevelSourceArtifactInstallOperation::Succeeded  ? PackageStateChange::Changed
                                                                                               : result.devel_execution->operation == DevelSourceArtifactInstallOperation::NotAttempted ? PackageStateChange::NoChange
                                                                                                                                                                                        : PackageStateChange::Unknown;
            return;
        case SourceBuildExecutionStatus::Installed:
            result.status = RegisteredSourceUpgradeStatus::Updated;
            result.failure_kind = RegisteredSourceUpgradeFailureKind::None;
            result.package_state_change = PackageStateChange::Changed;
            return;
        case SourceBuildExecutionStatus::SkippedAsNeeded:
        case SourceBuildExecutionStatus::UpToDate:
            result.status = RegisteredSourceUpgradeStatus::NoChange;
            result.failure_kind = RegisteredSourceUpgradeFailureKind::None;
            result.package_state_change = PackageStateChange::NoChange;
            return;
        case SourceBuildExecutionStatus::UpdateStatusUnknownSkipped:
            result.status = RegisteredSourceUpgradeStatus::Incomplete;
            result.failure_kind = RegisteredSourceUpgradeFailureKind::
                UpdateStatusUnknownSkipped;
            result.package_state_change = PackageStateChange::NoChange;
            return;
    }
    throw std::logic_error(localization::translate_message(
        "Unknown source-build execution status."));
}

void map_cleanup_failure(
    RegisteredSourceUpgradeResult& result,
    const SeparatedSourceBuildCleanupError& error) {
    result.production_outcome = error.production_outcome();
    switch(error.install_outcome()) {
        case ArtifactInstallExecutionOutcome::Installed:
            result.status =
                RegisteredSourceUpgradeStatus::UpdatedCleanupFailed;
            result.package_state_change = PackageStateChange::Changed;
            break;
        case ArtifactInstallExecutionOutcome::SkippedAsNeeded:
            result.status =
                RegisteredSourceUpgradeStatus::NoChangeCleanupFailed;
            result.package_state_change = PackageStateChange::NoChange;
            break;
    }
    result.failure_kind = RegisteredSourceUpgradeFailureKind::
        CleanupFailedAfterPackageTransaction;
    result.diagnostic = error.what();
    result.cleanup_diagnostic = error.what();
}

RegisteredSourceExecutionCorrelationFailure registered_correlation_failure(
    RegisteredSourceExecutionCorrelationFailureReason reason,
    std::string diagnostic,
    std::optional<std::size_t> required_child_index = std::nullopt,
    std::optional<std::string> package_name = std::nullopt) {
    return RegisteredSourceExecutionCorrelationFailure{
        reason,
        required_child_index,
        std::move(package_name),
        std::move(diagnostic)};
}

std::string registered_source_build_failure_diagnostic(
    const ProductionSourceBuildWorkItem& work_item,
    const std::string& diagnostic) {
    // TRANSLATORS: The placeholders are the PackageBase identity, an official repository PackageBase name, package name, and build/install diagnostic.
    return localization::format_translated_message(
        "Failed while building/installing {} {} ({}): {}",
        "PackageBase",
        work_item.request.checkout_name,
        work_item.request.package_name,
        diagnostic);
}

std::optional<RegisteredSourceExecutionCorrelationFailure>
validate_registered_package_base_result(
    const RegisteredSourcePackageBaseExecutionResult& completed,
    const ProductionSourceBuildWorkItem& work_item) {
    if(completed.package_base() != work_item.request.checkout_name) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                PackageBaseMismatch,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal Arch field name "PackageBase".
                "Registered repository source-build result does not match the prepared {}.",
                "PackageBase"));
    }
    if(completed.selected_children().size() != 1) {
        return registered_correlation_failure(
            completed.selected_children().empty()
                ? RegisteredSourceExecutionCorrelationFailureReason::
                      MissingSelectedChild
                : RegisteredSourceExecutionCorrelationFailureReason::
                      ExtraSelectedChild,
            localization::translate_message(
                "Registered repository source-build selected child count does not match preparation."));
    }

    const RequiredPackageArtifactTarget& required =
        work_item.required_targets.front();
    const PackageBaseSourceBuildSelectedResult& selected =
        completed.selected_children().front();
    if(selected.identity.package_name != required.package_name) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                SelectedArtifactIdentityMismatch,
            localization::translate_message(
                "Registered repository selected artifact identity does not match the requested child."),
            0, selected.identity.package_name);
    }
    if(selected.identity.full_version.empty()) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                EmptySelectedArtifactVersion,
            localization::translate_message(
                "Registered repository selected artifact has an empty version."),
            0, selected.identity.package_name);
    }
    if(selected.desired_reason != required.desired_reason ||
       selected.desired_reason != DesiredInstallReason::Explicit) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                DesiredInstallReasonMismatch,
            localization::translate_message(
                "Registered repository selected artifact install reason does not match preparation."),
            0, selected.identity.package_name);
    }
    switch(selected.outcome) {
        case ArtifactInstallExecutionOutcome::Installed:
            break;
        case ArtifactInstallExecutionOutcome::SkippedAsNeeded:
            return registered_correlation_failure(
                RegisteredSourceExecutionCorrelationFailureReason::
                    UnexpectedSkippedAsNeeded,
                localization::translate_message(
                    "Registered repository source-build unexpectedly reported skipped-as-needed with needed disabled."),
                0, selected.identity.package_name);
        default:
            return registered_correlation_failure(
                RegisteredSourceExecutionCorrelationFailureReason::
                    UnknownChildOutcome,
                localization::translate_message(
                    "Registered repository selected artifact has an unknown execution outcome."),
                0, selected.identity.package_name);
    }

    std::set<std::string> unselected_names;
    for(const ArtifactPackageIdentity& identity :
        completed.unselected_artifacts()) {
        if(identity.package_name.empty() || identity.full_version.empty()) {
            return registered_correlation_failure(
                RegisteredSourceExecutionCorrelationFailureReason::
                    InvalidUnselectedArtifactIdentity,
                localization::translate_message(
                    "Registered repository unselected artifact identity is incomplete."),
                std::nullopt, identity.package_name);
        }
        if(identity.package_name == selected.identity.package_name) {
            return registered_correlation_failure(
                RegisteredSourceExecutionCorrelationFailureReason::
                    SelectedAndUnselectedIdentityOverlap,
                localization::translate_message(
                    "Registered repository unselected artifact overlaps the selected child."),
                std::nullopt, identity.package_name);
        }
        if(!unselected_names.insert(identity.package_name).second) {
            return registered_correlation_failure(
                RegisteredSourceExecutionCorrelationFailureReason::
                    DuplicateUnselectedArtifactIdentity,
                localization::translate_message(
                    "Registered repository source-build contains a duplicate unselected artifact identity."),
                std::nullopt, identity.package_name);
        }
    }
    return std::nullopt;
}

std::optional<RegisteredSourceExecutionCorrelationFailure>
validate_registered_transaction_failure(
    const RegisteredSourcePackageTransactionError& error,
    const ProductionSourceBuildWorkItem& work_item) {
    if(error.package_base() != work_item.request.checkout_name) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                PackageBaseMismatch,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal Arch field name "PackageBase".
                "Registered repository package transaction failure does not match the prepared {}.",
                "PackageBase"));
    }
    if(error.attempts().size() != 1) {
        return registered_correlation_failure(
            error.attempts().empty()
                ? RegisteredSourceExecutionCorrelationFailureReason::
                      MissingSelectedChild
                : RegisteredSourceExecutionCorrelationFailureReason::
                      ExtraSelectedChild,
            localization::translate_message(
                "Registered repository package transaction attempt count does not match preparation."));
    }
    const RequiredPackageArtifactTarget& required =
        work_item.required_targets.front();
    const PackageBaseArtifactInstallTransactionAttempt& attempt =
        error.attempts().front();
    if(attempt.identity.package_name != required.package_name) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                SelectedArtifactIdentityMismatch,
            localization::translate_message(
                "Registered repository transaction attempt identity does not match the requested child."),
            0, attempt.identity.package_name);
    }
    if(attempt.identity.full_version.empty()) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                EmptySelectedArtifactVersion,
            localization::translate_message(
                "Registered repository transaction attempt has an empty version."),
            0, attempt.identity.package_name);
    }
    if(attempt.desired_reason != required.desired_reason ||
       attempt.desired_reason != DesiredInstallReason::Explicit) {
        return registered_correlation_failure(
            RegisteredSourceExecutionCorrelationFailureReason::
                DesiredInstallReasonMismatch,
            localization::translate_message(
                "Registered repository transaction attempt install reason does not match preparation."),
            0, attempt.identity.package_name);
    }
    return std::nullopt;
}

std::optional<RegisteredSourceExecutionCorrelationFailure>
validate_registered_preparation_failure(
    const RegisteredSourcePackageBasePreparationError& error,
    const ProductionSourceBuildWorkItem& work_item) {
    const std::string* package_base = nullptr;
    if(const auto* selection = error.selection_failure()) {
        package_base = &selection->package_base;
    } else if(const auto* mixed = error.mixed_reason_failure()) {
        package_base = &mixed->package_base;
    }
    if(package_base == nullptr ||
       *package_base == work_item.request.checkout_name) {
        return std::nullopt;
    }
    return registered_correlation_failure(
        RegisteredSourceExecutionCorrelationFailureReason::
            PackageBaseMismatch,
        localization::format_translated_message(
            // TRANSLATORS: {} is the literal Arch field name "PackageBase".
            "Registered repository install preparation failure does not match the prepared {}.",
            "PackageBase"),
        std::nullopt, *package_base);
}

RegisteredSourceBuildFailureCategory map_registered_source_build_phase(
    SeparatedPackageBaseSourceBuildFailurePhase phase) noexcept {
    switch(phase) {
        case SeparatedPackageBaseSourceBuildFailurePhase::Build:
            return RegisteredSourceBuildFailureCategory::Build;
        case SeparatedPackageBaseSourceBuildFailurePhase::ArtifactValidation:
            return RegisteredSourceBuildFailureCategory::ArtifactValidation;
        case SeparatedPackageBaseSourceBuildFailurePhase::ArtifactIdentity:
            return RegisteredSourceBuildFailureCategory::ArtifactIdentity;
        case SeparatedPackageBaseSourceBuildFailurePhase::InstallPreparation:
            return RegisteredSourceBuildFailureCategory::InstallPreparation;
        case SeparatedPackageBaseSourceBuildFailurePhase::InstallTransaction:
            return RegisteredSourceBuildFailureCategory::InstallTransaction;
    }
    return RegisteredSourceBuildFailureCategory::Other;
}

RegisteredSourceBuildFailureCategory map_registered_source_build_phase(
    SeparatedSourceBuildFailurePhase phase) noexcept {
    switch(phase) {
        case SeparatedSourceBuildFailurePhase::Build:
            return RegisteredSourceBuildFailureCategory::Build;
        case SeparatedSourceBuildFailurePhase::ArtifactValidation:
            return RegisteredSourceBuildFailureCategory::ArtifactValidation;
        case SeparatedSourceBuildFailurePhase::InstallPreparation:
            return RegisteredSourceBuildFailureCategory::InstallPreparation;
        case SeparatedSourceBuildFailurePhase::InstallTransaction:
            return RegisteredSourceBuildFailureCategory::InstallTransaction;
    }
    return RegisteredSourceBuildFailureCategory::Other;
}

void record_registered_correlation_failure(
    RegisteredSourceUpgradeResult& result,
    RegisteredSourceExecutionCorrelationFailure failure) {
    result.status = RegisteredSourceUpgradeStatus::Failed;
    result.failure_kind =
        RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed;
    result.package_state_change = PackageStateChange::Unknown;
    result.diagnostic = failure.diagnostic;
    result.cleanup_diagnostic = std::nullopt;
    result.package_base_execution = std::nullopt;
    result.failure_detail.emplace<
        RegisteredSourceExecutionCorrelationFailure>(
        std::move(failure));
}

std::optional<RegisteredSourceExecutionCorrelationFailure>
map_registered_package_base_result(
    RegisteredSourceUpgradeResult& result,
    const RegisteredSourcePackageBaseExecutionResult& completed,
    const ProductionSourceBuildWorkItem& work_item,
    bool cleanup_failed,
    const std::string& cleanup_diagnostic = {}) {
    if(auto failure = validate_registered_package_base_result(
           completed, work_item);
       failure.has_value()) {
        result.production_outcome = completed.production_outcome();
        return failure;
    }
    result.production_outcome = completed.production_outcome();
    result.package_base_execution =
        RegisteredSourcePackageBaseExecutionSnapshot{
            completed.package_base(),
            completed.selected_children().front(),
            completed.unselected_artifacts()};
    result.status = cleanup_failed
                        ? RegisteredSourceUpgradeStatus::UpdatedCleanupFailed
                        : RegisteredSourceUpgradeStatus::Updated;
    result.failure_kind = cleanup_failed
                              ? RegisteredSourceUpgradeFailureKind::
                                    CleanupFailedAfterPackageTransaction
                              : RegisteredSourceUpgradeFailureKind::None;
    result.package_state_change = PackageStateChange::Changed;
    result.diagnostic = cleanup_failed
                            ? std::optional<std::string>{cleanup_diagnostic}
                            : std::nullopt;
    result.cleanup_diagnostic = cleanup_failed
                                    ? std::optional<std::string>{cleanup_diagnostic}
                                    : std::nullopt;
    result.package_transaction_failure = std::nullopt;
    result.failure_detail = std::monostate{};
    return std::nullopt;
}

void map_registered_up_to_date(
    RegisteredSourceUpgradeResult& result,
    const SourceBuildUpToDate& outcome) {
    result.status = RegisteredSourceUpgradeStatus::NoChange;
    result.failure_kind = RegisteredSourceUpgradeFailureKind::None;
    result.package_state_change = PackageStateChange::NoChange;
    result.diagnostic = outcome.diagnostic.empty()
                            ? std::nullopt
                            : std::optional<std::string>{outcome.diagnostic};
    result.cleanup_diagnostic = std::nullopt;
    if(outcome.source_provenance.has_value()) {
        result.production_outcome =
            ProductionSourceBuildStagedOutcome{
                .source_provenance =
                    *outcome.source_provenance};
    }
    result.failure_detail = std::monostate{};
}

void map_registered_unknown_skip(
    RegisteredSourceUpgradeResult& result,
    const SourceBuildUpdateStatusUnknownSkipped& outcome) {
    result.status = RegisteredSourceUpgradeStatus::Incomplete;
    result.failure_kind = RegisteredSourceUpgradeFailureKind::
        UpdateStatusUnknownSkipped;
    result.package_state_change = PackageStateChange::NoChange;
    result.diagnostic = outcome.diagnostic.empty()
                            ? std::nullopt
                            : std::optional<std::string>{outcome.diagnostic};
    result.cleanup_diagnostic = std::nullopt;
    if(outcome.source_provenance.has_value()) {
        result.production_outcome =
            ProductionSourceBuildStagedOutcome{
                .source_provenance =
                    *outcome.source_provenance};
    }
    result.failure_detail = std::monostate{};
}

void record_registered_preparation_failure(
    RegisteredSourceUpgradeResult& result,
    const RegisteredSourcePackageBasePreparationError& error,
    const ProductionSourceBuildWorkItem& work_item) {
    result.status = RegisteredSourceUpgradeStatus::Failed;
    result.failure_kind =
        RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed;
    result.package_state_change = PackageStateChange::Unknown;
    result.diagnostic = registered_source_build_failure_diagnostic(
        work_item, error.what());
    result.production_outcome = error.production_outcome();
    if(const auto* selection = error.selection_failure()) {
        result.failure_detail.emplace<
            PackageBaseArtifactIdentitySelectionFailure>(*selection);
    } else if(const auto* mixed = error.mixed_reason_failure()) {
        result.failure_detail.emplace<
            MixedPackageBaseInstallReasonUnsupported>(*mixed);
    } else {
        result.failure_detail.emplace<RegisteredSourceBuildFailureSnapshot>(
            RegisteredSourceBuildFailureSnapshot{
                RegisteredSourceBuildFailureCategory::Other,
                error.what(), std::nullopt});
    }
}

void record_registered_phase_failure(
    RegisteredSourceUpgradeResult& result,
    const RegisteredSourcePackageBasePhaseError& error,
    const ProductionSourceBuildWorkItem& work_item) {
    result.status = RegisteredSourceUpgradeStatus::Failed;
    result.failure_kind =
        RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed;
    result.package_state_change = PackageStateChange::Unknown;
    result.diagnostic = registered_source_build_failure_diagnostic(
        work_item, error.what());
    result.production_outcome = error.production_outcome();
    if(error.package_metadata_failure().has_value()) {
        result.failure_detail.emplace<PackageMetadataFailure>(
            *error.package_metadata_failure());
        return;
    }
    result.failure_detail.emplace<RegisteredSourceBuildFailureSnapshot>(
        RegisteredSourceBuildFailureSnapshot{
            map_registered_source_build_phase(error.phase()),
            error.what(), error.reviewed_source_failure()});
}

void record_registered_singular_source_failure(
    RegisteredSourceUpgradeResult& result,
    const std::string& diagnostic,
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome,
    RegisteredSourceBuildFailureCategory category,
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure = std::nullopt) {
    result.status = RegisteredSourceUpgradeStatus::Failed;
    result.failure_kind =
        RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed;
    result.package_state_change = PackageStateChange::Unknown;
    result.diagnostic = diagnostic;
    result.production_outcome = std::move(production_outcome);
    result.failure_detail.emplace<RegisteredSourceBuildFailureSnapshot>(
        RegisteredSourceBuildFailureSnapshot{
            category,
            diagnostic, std::move(reviewed_source_failure)});
}

void record_registered_transaction_failure(
    RegisteredSourceUpgradeResult& result,
    const RegisteredSourcePackageTransactionError& error,
    const ProductionSourceBuildWorkItem& work_item) {
    RegisteredSourcePackageTransactionFailureSnapshot snapshot{
        error.failure_kind(), error.attempts(), error.exit_code(),
        error.what()};
    result.package_transaction_failure = snapshot;
    result.production_outcome = error.production_outcome();
    if(auto failure = validate_registered_transaction_failure(
           error, work_item);
       failure.has_value()) {
        record_registered_correlation_failure(
            result, std::move(failure.value()));
        return;
    }
    result.status = RegisteredSourceUpgradeStatus::Failed;
    result.failure_kind =
        RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed;
    result.package_state_change = PackageStateChange::Unknown;
    result.diagnostic = registered_source_build_failure_diagnostic(
        work_item, error.what());
    result.failure_detail.emplace<
        RegisteredSourcePackageTransactionFailureSnapshot>(
        std::move(snapshot));
}

} // namespace

struct PreparedSystemSourceUpgrade::Impl {
    explicit Impl(SystemSourceUpgradePreparationState&& prepared_state)
        : state(std::move(prepared_state)) {
    }

    SystemSourceUpgradePreparationState state;
};

struct SystemSourceUpgradePreparationAccess {
    static PreparedSystemSourceUpgrade make(
        SystemSourceUpgradePreparationState&& state) {
        return PreparedSystemSourceUpgrade(
            std::make_unique<PreparedSystemSourceUpgrade::Impl>(
                std::move(state)));
    }
};

RegisteredSourceUpgradeResult::RegisteredSourceUpgradeResult(
    std::size_t original_index,
    std::string package_name,
    std::optional<std::string> source_identity_key,
    std::optional<std::string> package_base,
    RegisteredSourceUpgradeStatus source_status,
    RegisteredSourceUpgradeFailureKind source_failure_kind,
    PackageStateChange state_change,
    std::optional<std::string> source_diagnostic,
    std::optional<std::string> source_cleanup_diagnostic)
    : original_preference_index(original_index),
      preference_package_name(std::move(package_name)),
      canonical_source_identity_key(std::move(source_identity_key)),
      resolved_package_base(std::move(package_base)),
      status(source_status), failure_kind(source_failure_kind),
      package_state_change(state_change),
      diagnostic(std::move(source_diagnostic)),
      cleanup_diagnostic(std::move(source_cleanup_diagnostic)) {
}

bool SystemSourceUpgradeResult::is_success() const noexcept {
    if(status != SystemSourceUpgradeStatus::Completed) return false;
    if(!selected_repository_provider_transaction.is_success()) return false;
    // ObservabilityOnly は実行結果ではなく package-state observation の
    // 信頼性を下げる。transaction 成功を failure へ丸めず、projection 側で
    // Succeeded + Unverified として保持する。
    if(std::any_of(
           issues.begin(), issues.end(),
           [](const SystemSourceUpgradeIssue& issue) {
               return issue.impact !=
                      SystemSourceUpgradeIssueImpact::ObservabilityOnly;
           })) {
        return false;
    }
    return std::none_of(
        registered_source_results.begin(),
        registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return has_non_success_source_status(source.status);
        });
}

PackageStateChange SystemSourceUpgradeResult::package_state_change()
    const noexcept {
    bool has_unknown = system.package_state_change ==
                           PackageStateChange::Unknown ||
                       selected_repository_provider_transaction.package_state_change ==
                           PackageStateChange::Unknown;
    if(system.package_state_change == PackageStateChange::Changed ||
       selected_repository_provider_transaction.package_state_change ==
           PackageStateChange::Changed) {
        return PackageStateChange::Changed;
    }
    for(const auto& source : registered_source_results) {
        if(source.package_state_change == PackageStateChange::Changed) {
            return PackageStateChange::Changed;
        }
        has_unknown = has_unknown ||
                      source.package_state_change == PackageStateChange::Unknown;
    }
    return has_unknown ? PackageStateChange::Unknown
                       : PackageStateChange::NoChange;
}

bool SystemSourceUpgradeResult::definitely_changed_package_state()
    const noexcept {
    return package_state_change() == PackageStateChange::Changed;
}

bool SystemSourceUpgradeResult::has_partial_completion() const noexcept {
    if(system.status != SystemUpgradePhaseStatus::Completed) return false;
    // system phase完了後の停止・内部相関failureは、source mutationの有無に
    // かかわらずaggregate全体としてpartial completionである。
    if(status != SystemSourceUpgradeStatus::Completed) return true;
    return std::any_of(
        registered_source_results.begin(),
        registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return has_non_success_source_status(source.status);
        });
}

bool SystemSourceUpgradeResult::has_not_attempted_sources() const noexcept {
    return std::any_of(
        registered_source_results.begin(),
        registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return source.status ==
                   RegisteredSourceUpgradeStatus::NotAttempted;
        });
}

bool SystemSourceUpgradeResult::has_cleanup_failure() const noexcept {
    return std::any_of(
        registered_source_results.begin(),
        registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return source.status ==
                       RegisteredSourceUpgradeStatus::
                           UpdatedCleanupFailed ||
                   source.status ==
                       RegisteredSourceUpgradeStatus::
                           NoChangeCleanupFailed;
        });
}

bool SystemSourceUpgradeResult::has_blocking_issue() const noexcept {
    return std::any_of(
        issues.begin(), issues.end(),
        [](const SystemSourceUpgradeIssue& issue) {
            return issue.impact ==
                   SystemSourceUpgradeIssueImpact::BlocksExecution;
        });
}

std::optional<std::string>
SystemSourceUpgradeResult::failure_diagnostic() const {
    for(const auto& diagnostic : diagnostics) {
        if(diagnostic.stops_execution) return diagnostic.diagnostic;
    }
    if(system.diagnostic.has_value() &&
       system.status == SystemUpgradePhaseStatus::Failed) {
        return system.diagnostic;
    }
    if(!selected_repository_provider_transaction.is_success() &&
       selected_repository_provider_transaction.diagnostic.has_value()) {
        return selected_repository_provider_transaction.diagnostic;
    }
    for(const auto& source : registered_source_results) {
        if(source.cleanup_diagnostic.has_value()) {
            return source.cleanup_diagnostic;
        }
        if(source.diagnostic.has_value() &&
           (source.status == RegisteredSourceUpgradeStatus::Failed ||
            source.status == RegisteredSourceUpgradeStatus::Incomplete)) {
            return source.diagnostic;
        }
    }
    return std::nullopt;
}

PreparedSystemSourceUpgrade::PreparedSystemSourceUpgrade(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
    if(impl_ == nullptr) return;

    SystemSourceUpgradePreparationState& state = impl_->state;
    if(state.correlations.size() !=
       state.snapshot.registered_sources.size()) {
        throw std::logic_error(
            "Prepared system/source projection correlation count is inconsistent.");
    }

    const std::size_t work_item_count = state.source_invocation.has_value()
                                            ? state.source_invocation->work_items.size()
                                            : 0;
    std::vector<bool> observed_work_items(work_item_count, false);
    std::vector<PreparedSystemSourceWorkReference> source_work_items;
    source_work_items.reserve(work_item_count);
    for(const RegisteredSourceCorrelation& correlation : state.correlations) {
        if(!correlation.work_item_index.has_value()) continue;
        if(correlation.snapshot_index >=
               state.snapshot.registered_sources.size() ||
           !state.source_invocation.has_value() ||
           correlation.work_item_index.value() >= work_item_count ||
           observed_work_items[correlation.work_item_index.value()]) {
            throw std::logic_error(
                "Prepared system/source work-item correlation is inconsistent.");
        }

        observed_work_items[correlation.work_item_index.value()] = true;
        source_work_items.push_back(PreparedSystemSourceWorkReference(
            state.snapshot.registered_sources[correlation.snapshot_index],
            state.source_invocation
                ->work_items[correlation.work_item_index.value()]));
    }
    if(std::any_of(
           observed_work_items.begin(), observed_work_items.end(),
           [](bool observed) { return !observed; })) {
        throw std::logic_error(
            "Prepared system/source work item has no source correlation.");
    }

    SystemSourceUpgradeProjectionAuthority authority(
        state.snapshot,
        state.aur_invocation_plan.has_value()
            ? &state.aur_invocation_plan.value()
            : nullptr,
        state.issues, std::move(source_work_items));
    projection_authority_.emplace(std::move(authority));
}

PreparedSystemSourceUpgrade::PreparedSystemSourceUpgrade(
    PreparedSystemSourceUpgrade&& other) noexcept
    : impl_(std::move(other.impl_)),
      projection_authority_(std::move(other.projection_authority_)) {
    other.projection_authority_.reset();
}

PreparedSystemSourceUpgrade::~PreparedSystemSourceUpgrade() noexcept = default;

bool PreparedSystemSourceUpgrade::is_valid() const noexcept {
    return impl_ != nullptr;
}

const SystemSourceUpgradePreparedSnapshot*
PreparedSystemSourceUpgrade::snapshot() const noexcept {
    return impl_ == nullptr ? nullptr : &impl_->state.snapshot;
}

const BuildPlan* PreparedSystemSourceUpgrade::aur_invocation_plan()
    const noexcept {
    if(impl_ == nullptr || !impl_->state.aur_invocation_plan.has_value()) {
        return nullptr;
    }
    return &impl_->state.aur_invocation_plan.value();
}

const std::vector<SystemSourceUpgradeIssue>&
PreparedSystemSourceUpgrade::issues() const noexcept {
    static const std::vector<SystemSourceUpgradeIssue> s_empty;
    return impl_ == nullptr ? s_empty : impl_->state.issues;
}

const SystemSourceUpgradeProjectionAuthority*
PreparedSystemSourceUpgrade::projection_authority() const noexcept {
    return impl_ != nullptr && projection_authority_.has_value()
               ? &projection_authority_.value()
               : nullptr;
}

#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
void PreparedSystemSourceUpgrade::
    make_first_source_correlation_inconsistent_for_test() {
    if(impl_ == nullptr || impl_->state.correlations.empty()) return;
    ++impl_->state.correlations.front().snapshot_index;
}

void PreparedSystemSourceUpgrade::set_unexpected_exception_for_test(
    SystemSourceUpgradeUnexpectedExceptionPoint point,
    bool unknown_exception) {
    if(impl_ == nullptr) return;
    impl_->state.unexpected_exception_point = point;
    impl_->state.unexpected_exception_is_unknown = unknown_exception;
}
#endif

SystemSourceUpgradePreparation prepare_system_source_upgrade(
    const AppConfig& config,
    const SystemSourceUpgradeEventObserver& observer) {
    SystemSourceUpgradePreparationState state;
    state.snapshot.options = snapshot_options(config);

    SourcePreferenceDirectorySnapshot directory_snapshot;
    try {
        directory_snapshot = snapshot_source_preference_directory();
    } catch(const SourcePreferenceError& error) {
        SystemSourceUpgradeIssue issue = make_issue(
            SystemSourceUpgradeIssueKind::
                PreferenceEnumerationUnavailable,
            SystemSourceUpgradeIssueImpact::BlocksExecution,
            SystemSourceUpgradePhase::Preparation,
            error.what());
        issue.source_preference_failure = error.failure();
        return block_preparation(
            std::move(state), std::move(issue), std::nullopt,
            RegisteredSourceUpgradeFailureKind::PreferenceUnavailable);
    } catch(const std::exception& error) {
        SystemSourceUpgradeIssue issue = make_issue(
            SystemSourceUpgradeIssueKind::
                PreferenceEnumerationUnavailable,
            SystemSourceUpgradeIssueImpact::BlocksExecution,
            SystemSourceUpgradePhase::Preparation,
            error.what());
        return block_preparation(
            std::move(state), std::move(issue), std::nullopt,
            RegisteredSourceUpgradeFailureKind::PreferenceUnavailable);
    } catch(...) {
        SystemSourceUpgradeIssue issue = make_issue(
            SystemSourceUpgradeIssueKind::
                PreferenceEnumerationUnavailable,
            SystemSourceUpgradeIssueImpact::BlocksExecution,
            SystemSourceUpgradePhase::Preparation,
            localization::translate_message(
                "Source preference enumeration failed with an unknown exception."));
        return block_preparation(
            std::move(state), std::move(issue), std::nullopt,
            RegisteredSourceUpgradeFailureKind::UnknownException);
    }

    for(const auto& entry : directory_snapshot.entries) {
        std::string diagnostic;
        if(!entry.is_regular_file) {
            diagnostic = localization::format_translated_message(
                "Source preference enumeration found a non-regular entry: {}",
                entry.entry_path.string());
        } else if(!is_valid_package_name(entry.package_name)) {
            diagnostic = localization::format_translated_message(
                "Source preference enumeration found an invalid entry name: {}",
                entry.entry_path.string());
        }
        if(diagnostic.empty()) continue;

        SystemSourceUpgradeIssue issue = make_issue(
            SystemSourceUpgradeIssueKind::
                PreferenceEnumerationUnavailable,
            SystemSourceUpgradeIssueImpact::BlocksExecution,
            SystemSourceUpgradePhase::Preparation,
            std::move(diagnostic));
        return block_preparation(
            std::move(state), std::move(issue), std::nullopt,
            RegisteredSourceUpgradeFailureKind::PreferenceUnavailable);
    }

    state.snapshot.preference_root_exists = directory_snapshot.root_exists;
    for(const auto& entry : directory_snapshot.entries) {
        state.snapshot.registered_sources.push_back(
            RegisteredSourcePreferenceSnapshot{
                entry.original_index,
                entry.package_name,
                entry.entry_path,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                {},
                std::nullopt});
        state.correlations.push_back(RegisteredSourceCorrelation{
            state.snapshot.registered_sources.size() - 1,
            is_valid_package_name(entry.package_name),
            std::nullopt});
    }

    const bool has_valid_source = std::any_of(
        state.correlations.begin(), state.correlations.end(),
        [](const RegisteredSourceCorrelation& correlation) {
            return correlation.has_valid_package_name;
        });
    if(has_valid_source) {
        try {
            require_supported_production_source_build_options(config);
        } catch(const std::exception& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceInvocationPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            return block_preparation(
                std::move(state), std::move(issue), std::nullopt,
                RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed);
        }
    }

    // Local preference failureはcache mutationより前に全件確定する。identity
    // resolutionはpacman/curlへ進み得るため、次のphaseでcacheを先にadoptする。
    for(std::size_t source_position = 0;
        source_position < state.snapshot.registered_sources.size();
        ++source_position) {
        RegisteredSourceCorrelation& correlation =
            state.correlations[source_position];
        RegisteredSourcePreferenceSnapshot& source =
            state.snapshot.registered_sources[source_position];
        if(!correlation.has_valid_package_name) continue;

        StrictSourcePreferenceResult preference =
            read_source_preference_strict(
                source.preference_package_name);
        if(const auto* loaded =
               std::get_if<SourcePreferenceLoaded>(&preference)) {
            source.entry_path = loaded->entry_path;
            source.environment = loaded->environment;
            source.preference_load_warnings = loaded->warnings;
            notify(observer, SystemSourceUpgradeEvent{
                                 SystemSourceUpgradeEventKind::LoadingSourcePreference,
                                 source.original_preference_index,
                                 source.preference_package_name,
                                 source.entry_path,
                                 {}});
            for(const auto& warning_text : loaded->warnings) {
                state.warnings.push_back(SystemSourceUpgradeWarning{
                    SystemSourceUpgradeWarningKind::SourcePreference,
                    source.original_preference_index,
                    source.preference_package_name,
                    source.entry_path,
                    warning_text});
                notify(observer, SystemSourceUpgradeEvent{
                                     SystemSourceUpgradeEventKind::SourcePreferenceWarning,
                                     source.original_preference_index,
                                     source.preference_package_name,
                                     source.entry_path,
                                     warning_text});
            }
            continue;
        }

        SystemSourceUpgradeIssue issue = make_issue(
            SystemSourceUpgradeIssueKind::PreferenceUnavailable,
            SystemSourceUpgradeIssueImpact::BlocksExecution,
            SystemSourceUpgradePhase::Preparation,
            {});
        attribute_issue_to_source(issue, source);
        if(const auto* failure =
               std::get_if<SourcePreferenceFailure>(&preference)) {
            issue.source_preference_failure = *failure;
            issue.diagnostic = failure->diagnostic;
        } else {
            issue.diagnostic = localization::format_translated_message(
                "The registered source preference disappeared before preparation: {}",
                source.entry_path.string());
        }
        return block_preparation(
            std::move(state), std::move(issue), source_position,
            RegisteredSourceUpgradeFailureKind::PreferenceUnavailable);
    }

    std::vector<std::optional<ResolvedSourceBuildIdentity>>
        resolved_identities(
            state.snapshot.registered_sources.size());
    ProviderSelectionCallback select_provider =
        registered_source_provider_selection_callback(
            provider_selection_callback(config));
    for(std::size_t source_position = 0;
        source_position < state.snapshot.registered_sources.size();
        ++source_position) {
        RegisteredSourceCorrelation& correlation =
            state.correlations[source_position];
        RegisteredSourcePreferenceSnapshot& source =
            state.snapshot.registered_sources[source_position];
        if(!correlation.has_valid_package_name) continue;

        try {
            ResolvedSourceBuildIdentity identity =
                resolve_source_build_identity(
                    source.preference_package_name);
            source.canonical_source_identity_key =
                identity.canonical_source_key();
            source.resolved_package_base = identity.package_base();
            source.source_kind = identity.source_kind();
            if(const auto* repository = identity.repository_identity();
               repository != nullptr) {
                source.repository_identity = *repository;
            }
            resolved_identities[source_position] = std::move(identity);
        } catch(const std::exception& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceIdentityResolutionFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            attribute_issue_to_source(issue, source);
            return block_preparation(
                std::move(state), std::move(issue), source_position,
                RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed);
        } catch(...) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceIdentityResolutionFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                localization::translate_message(
                    "Source identity resolution failed with an unknown exception."));
            attribute_issue_to_source(issue, source);
            return block_preparation(
                std::move(state), std::move(issue), source_position,
                RegisteredSourceUpgradeFailureKind::UnknownException);
        }
    }

    std::vector<std::string> aur_package_names;
    std::optional<std::size_t> first_aur_source_position;
    for(std::size_t source_position = 0;
        source_position < resolved_identities.size();
        ++source_position) {
        if(!resolved_identities[source_position].has_value() ||
           resolved_identities[source_position]->source_kind() !=
               SourceBuildSourceKind::Aur) {
            continue;
        }
        if(!first_aur_source_position.has_value()) {
            first_aur_source_position = source_position;
        }
        aur_package_names.push_back(
            resolved_identities[source_position]->requested_name());
    }
    if(!aur_package_names.empty()) {
        try {
            state.aur_invocation_plan.emplace(resolve_build_plan(
                aur_package_names, select_provider));
            require_executable_install_plan(
                join_package_names(aur_package_names),
                state.aur_invocation_plan.value());
        } catch(const std::exception& error) {
            const std::size_t source_position =
                first_aur_source_position.value();
            RegisteredSourcePreferenceSnapshot& source =
                state.snapshot.registered_sources[source_position];
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceWorkItemPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            attribute_issue_to_source(issue, source);
            return block_preparation(
                std::move(state), std::move(issue), source_position,
                RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed);
        } catch(...) {
            const std::size_t source_position =
                first_aur_source_position.value();
            RegisteredSourcePreferenceSnapshot& source =
                state.snapshot.registered_sources[source_position];
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceWorkItemPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                localization::translate_message(
                    "Source invocation constraint preflight failed with an unknown exception."));
            attribute_issue_to_source(issue, source);
            return block_preparation(
                std::move(state), std::move(issue), source_position,
                RegisteredSourceUpgradeFailureKind::UnknownException);
        }
    }

    std::vector<ProductionSourceBuildWorkItem> source_work_items;
    std::vector<std::string> package_names;
    for(std::size_t source_position = 0;
        source_position < state.snapshot.registered_sources.size();
        ++source_position) {
        RegisteredSourceCorrelation& correlation =
            state.correlations[source_position];
        RegisteredSourcePreferenceSnapshot& source =
            state.snapshot.registered_sources[source_position];
        if(!resolved_identities[source_position].has_value()) continue;

        try {
            correlation.work_item_index = source_work_items.size();
            ProductionSourceBuildWorkItem work_item =
                prepare_registered_source_build_work_item(
                    resolved_identities[source_position].value(),
                    source.environment.value(),
                    select_provider);
            source.required_target_provenance =
                work_item.required_target_provenance;
            source.artifact_lifecycle_intent =
                work_item.artifact_lifecycle_intent;
            source_work_items.push_back(std::move(work_item));
            package_names.push_back(source.preference_package_name);
        } catch(const std::exception& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceWorkItemPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            attribute_issue_to_source(issue, source);
            return block_preparation(
                std::move(state), std::move(issue), source_position,
                RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed);
        } catch(...) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceWorkItemPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                localization::translate_message(
                    "Source work-item preparation failed with an unknown exception."));
            attribute_issue_to_source(issue, source);
            return block_preparation(
                std::move(state), std::move(issue), source_position,
                RegisteredSourceUpgradeFailureKind::UnknownException);
        }
    }

    if(!source_work_items.empty()) {
        try {
            state.source_invocation =
                prepare_production_source_build_invocation(
                    std::move(source_work_items), config);
        } catch(const ReviewedSourceProductionError& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceInvocationPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            issue.reviewed_source_failure = error.failure();
            return block_preparation(
                std::move(state), std::move(issue), std::nullopt,
                RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed);
        } catch(const std::exception& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceInvocationPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            return block_preparation(
                std::move(state), std::move(issue), std::nullopt,
                RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed);
        } catch(...) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceInvocationPreparationFailed,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                localization::translate_message(
                    "Source invocation preparation failed with an unknown exception."));
            return block_preparation(
                std::move(state), std::move(issue), std::nullopt,
                RegisteredSourceUpgradeFailureKind::UnknownException);
        }

        const PacmanDatabasePaths database_paths =
            state.source_invocation->database_paths;
        state.system_database_paths = database_paths;
        try {
            PackageMetadataSession session =
                PackageMetadataSession::open(database_paths);
            std::optional<SourceBaselineFailure> baseline_failure =
                snapshot_source_update_baselines(
                    session,
                    package_names,
                    state.update_baselines);
            if(baseline_failure.has_value()) {
                const std::string& package_name =
                    package_names[baseline_failure->package_index];
                auto source = std::find_if(
                    state.snapshot.registered_sources.begin(),
                    state.snapshot.registered_sources.end(),
                    [&package_name](
                        const RegisteredSourcePreferenceSnapshot& candidate) {
                        return candidate.preference_package_name == package_name;
                    });
                const std::size_t source_position = static_cast<std::size_t>(
                    std::distance(
                        state.snapshot.registered_sources.begin(),
                        source));
                SystemSourceUpgradeIssue issue = make_issue(
                    SystemSourceUpgradeIssueKind::
                        SourceBaselineSnapshotUnavailable,
                    SystemSourceUpgradeIssueImpact::BlocksExecution,
                    SystemSourceUpgradePhase::Preparation,
                    baseline_failure->failure.diagnostic);
                issue.package_metadata_failure = baseline_failure->failure;
                if(source != state.snapshot.registered_sources.end()) {
                    attribute_issue_to_source(issue, *source);
                }
                return block_preparation(
                    std::move(state), std::move(issue), source_position,
                    RegisteredSourceUpgradeFailureKind::
                        PackageMetadataUnavailable);
            }

            LocalPackageVersionSnapshotResult system_snapshot =
                session.snapshot_local_package_versions();
            if(const auto* failure =
                   std::get_if<PackageMetadataFailure>(&system_snapshot)) {
                record_system_snapshot_failure(state, *failure);
            } else {
                state.before_system_snapshot =
                    std::get<LocalPackageVersionSnapshot>(
                        std::move(system_snapshot));
            }
        } catch(const PackageMetadataError& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceBaselineSnapshotUnavailable,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            issue.package_metadata_failure = error.failure();
            return block_preparation(
                std::move(state), std::move(issue), std::nullopt,
                RegisteredSourceUpgradeFailureKind::
                    PackageMetadataUnavailable);
        } catch(const std::exception& error) {
            SystemSourceUpgradeIssue issue = make_issue(
                SystemSourceUpgradeIssueKind::
                    SourceBaselineSnapshotUnavailable,
                SystemSourceUpgradeIssueImpact::BlocksExecution,
                SystemSourceUpgradePhase::Preparation,
                error.what());
            return block_preparation(
                std::move(state), std::move(issue), std::nullopt,
                RegisteredSourceUpgradeFailureKind::
                    PackageMetadataUnavailable);
        }
    } else {
        // POLICY(#350): system-only execution uses the same authoritative
        // local package database snapshots as the registered-source path.
        // Failure is observability-only and remains Unknown/Unverified.
        prepare_system_only_package_observation(state);
    }

    return SystemSourceUpgradePreparationAccess::make(std::move(state));
}

SystemSourceUpgradeResult execute_prepared_system_source_upgrade(
    PreparedSystemSourceUpgrade prepared,
    const AppConfig& config,
    const SystemSourceUpgradeEventObserver& observer,
    std::optional<ValidatedCacheRoot> shared_cache_root) {
    if(prepared.impl_ == nullptr) {
        SystemSourceUpgradeResult result;
        result.status = SystemSourceUpgradeStatus::InconsistentResult;
        result.stopped_phase = SystemSourceUpgradePhase::Preparation;
        add_inconsistent_issue(
            result,
            SystemSourceUpgradeIssueKind::PreparedCapabilityConsumed,
            SystemSourceUpgradePhase::Preparation,
            localization::translate_message(
                "The prepared system/source upgrade is invalid or has already been consumed."));
        return result;
    }

    SystemSourceUpgradePreparationState& state = prepared.impl_->state;
    if(!options_match(state.snapshot.options, config)) {
        SystemSourceUpgradeResult result = make_result_from_state(
            std::move(state),
            SystemSourceUpgradeStatus::InconsistentResult,
            SystemSourceUpgradePhase::Preparation);
        add_inconsistent_issue(
            result,
            SystemSourceUpgradeIssueKind::OptionSnapshotMismatch,
            SystemSourceUpgradePhase::Preparation,
            localization::translate_message(
                "The prepared system/source upgrade options differ from the execution options."));
        return result;
    }
    if(!validate_prepared_correlation(
           state.snapshot,
           state.correlations,
           state.source_invocation)) {
        SystemSourceUpgradeResult result = make_result_from_state(
            std::move(state),
            SystemSourceUpgradeStatus::InconsistentResult,
            SystemSourceUpgradePhase::Preparation);
        add_inconsistent_issue(
            result,
            SystemSourceUpgradeIssueKind::PreparedCorrelationInconsistent,
            SystemSourceUpgradePhase::Preparation,
            localization::translate_message(
                "The prepared system/source upgrade source correlation is inconsistent."));
        return result;
    }

    // Read-only preflightはcache capabilityを保持しない。actual executionだけが
    // current XDG stateからrootを取得し、system pacman前にseed/activateする。
    if(state.source_invocation.has_value()) {
        try {
            if(!shared_cache_root.has_value()) {
                shared_cache_root = prepare_process_cache_root();
            }
            seed_production_source_build_cache(
                state.source_invocation.value(),
                shared_cache_root.value());
            activate_production_source_build_cache(
                state.source_invocation.value());
        } catch(const xdg_paths::ResolutionError& error) {
            SystemSourceUpgradeIssue issue =
                make_cache_authority_issue(error.what());
            issue.cache_resolution_failure = error.failure();
            return block_cache_authority_preparation(
                std::move(state), std::move(issue));
        } catch(const xdg_directory_safety::PreparationError& error) {
            SystemSourceUpgradeIssue issue =
                make_cache_authority_issue(error.what());
            issue.cache_preparation_failure = error.failure();
            return block_cache_authority_preparation(
                std::move(state), std::move(issue));
        } catch(const TrustedCacheError& error) {
            SystemSourceUpgradeIssue issue =
                make_cache_authority_issue(error.what());
            issue.trusted_cache_failure = error.failure();
            return block_cache_authority_preparation(
                std::move(state), std::move(issue));
        }
    }

    // public snapshot/result detailだけを移し、executionに必要なprepared
    // invocation/correlation/baselineはone-shot capability内へ残す。
    SystemSourceUpgradeResult result = make_result_from_state(
        std::move(state),
        SystemSourceUpgradeStatus::Completed,
        SystemSourceUpgradePhase::None);

    SystemSourceUpgradePhase active_phase = SystemSourceUpgradePhase::System;
    std::optional<std::size_t> active_source_position;
    bool system_package_state_finalized = false;
    try {
        notify(observer, SystemSourceUpgradeEvent{
                             SystemSourceUpgradeEventKind::SystemUpgradeStarting,
                             std::nullopt,
                             std::nullopt,
                             std::nullopt,
                             {}});
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
        throw_unexpected_exception_for_test(
            state,
            SystemSourceUpgradeUnexpectedExceptionPoint::
                SystemPhaseStarted);
#endif
        int system_exit_status = 1;
        try {
            system_exit_status = run_command(system_upgrade_command(config));
        } catch(const std::exception& error) {
            result.system.status = SystemUpgradePhaseStatus::Failed;
            result.system.package_state_change = PackageStateChange::Unknown;
            result.system.diagnostic = error.what();
            result.status = SystemSourceUpgradeStatus::StoppedOnSystemFailure;
            result.stopped_phase = SystemSourceUpgradePhase::System;
            result.diagnostics.push_back(make_diagnostic(
                SystemSourceUpgradePhase::System,
                error.what(),
                true));
            return result;
        } catch(...) {
            result.system.status = SystemUpgradePhaseStatus::Failed;
            result.system.package_state_change = PackageStateChange::Unknown;
            result.system.diagnostic = unknown_system_failure_diagnostic();
            result.status = SystemSourceUpgradeStatus::StoppedOnSystemFailure;
            result.stopped_phase = SystemSourceUpgradePhase::System;
            result.diagnostics.push_back(make_diagnostic(
                SystemSourceUpgradePhase::System,
                unknown_system_failure_diagnostic(),
                true));
            return result;
        }
        result.system.command_exit_status = system_exit_status;
        if(system_exit_status != 0) {
            result.system.status = SystemUpgradePhaseStatus::Failed;
            result.system.package_state_change = PackageStateChange::Unknown;
            result.system.diagnostic = localization::translate_message(
                "The update failed.");
            result.status = SystemSourceUpgradeStatus::StoppedOnSystemFailure;
            result.stopped_phase = SystemSourceUpgradePhase::System;
            result.diagnostics.push_back(make_diagnostic(
                SystemSourceUpgradePhase::System,
                localization::translate_message("The update failed."),
                true));
            return result;
        }
        result.system.status = SystemUpgradePhaseStatus::Completed;
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
        throw_unexpected_exception_for_test(
            state,
            SystemSourceUpgradeUnexpectedExceptionPoint::
                SystemPhaseCompleted);
#endif

        const bool has_source_work_items = state.source_invocation.has_value();
        if(has_source_work_items) {
            active_phase = SystemSourceUpgradePhase::RegisteredSource;
            notify(observer, SystemSourceUpgradeEvent{
                                 SystemSourceUpgradeEventKind::CheckingSourcePackages,
                                 std::nullopt,
                                 std::nullopt,
                                 std::nullopt,
                                 {}});
        }

        SourceInstalledSnapshotResults installed_snapshots;
        if(state.system_database_paths.has_value() &&
           (state.before_system_snapshot.has_value() || has_source_work_items)) {
            try {
                PackageMetadataSession session = PackageMetadataSession::open(
                    state.system_database_paths.value());
                if(state.before_system_snapshot.has_value()) {
                    LocalPackageVersionSnapshotResult after_snapshot =
                        session.snapshot_local_package_versions();
                    if(const auto* failure =
                           std::get_if<PackageMetadataFailure>(
                               &after_snapshot)) {
                        record_post_system_snapshot_failure(result, *failure);
                    } else {
                        const LocalPackageVersionSnapshot& after =
                            std::get<LocalPackageVersionSnapshot>(
                                after_snapshot);
                        result.system.package_state_change =
                            state.before_system_snapshot.value() == after
                                ? PackageStateChange::NoChange
                                : PackageStateChange::Changed;
                    }
                } else {
                    result.system.package_state_change = PackageStateChange::Unknown;
                }
                system_package_state_finalized = true;

                if(has_source_work_items) {
                    std::vector<std::string> package_names;
                    package_names.reserve(
                        state.source_invocation->work_items.size());
                    for(const auto& work_item :
                        state.source_invocation->work_items) {
                        package_names.push_back(work_item.request.package_name);
                    }
                    installed_snapshots =
                        snapshot_post_upgrade_installed_packages(
                            session, package_names);
                }
            } catch(const PackageMetadataError& error) {
                if(!system_package_state_finalized) {
                    if(state.before_system_snapshot.has_value()) {
                        record_post_system_snapshot_failure(
                            result, error.failure());
                    } else {
                        result.system.package_state_change =
                            PackageStateChange::Unknown;
                    }
                    system_package_state_finalized = true;
                }
                if(has_source_work_items) {
                    const std::string diagnostic =
                        post_upgrade_snapshot_failure_diagnostic(
                            error.failure().diagnostic);
                    stop_for_global_source_metadata_failure(
                        result, error.failure(), diagnostic);
                    return result;
                }
            } catch(const std::exception& error) {
                if(!system_package_state_finalized) {
                    if(state.before_system_snapshot.has_value()) {
                        record_post_system_snapshot_failure(
                            result,
                            generic_metadata_failure(error.what()));
                    } else {
                        result.system.package_state_change =
                            PackageStateChange::Unknown;
                    }
                    system_package_state_finalized = true;
                }
                if(has_source_work_items) {
                    stop_for_global_source_metadata_failure(
                        result,
                        std::nullopt,
                        post_upgrade_snapshot_failure_diagnostic(
                            error.what()));
                    return result;
                }
            } catch(...) {
                const std::string diagnostic = localization::translate_message(
                    "The post-upgrade package metadata snapshot failed with an unknown exception.");
                if(!system_package_state_finalized) {
                    if(state.before_system_snapshot.has_value()) {
                        record_post_system_snapshot_failure(
                            result,
                            generic_metadata_failure(diagnostic));
                    } else {
                        result.system.package_state_change =
                            PackageStateChange::Unknown;
                    }
                    system_package_state_finalized = true;
                }
                if(has_source_work_items) {
                    stop_for_global_source_metadata_failure(
                        result,
                        std::nullopt,
                        post_upgrade_snapshot_failure_diagnostic(
                            diagnostic));
                    return result;
                }
            }
        } else if(result.system.before_snapshot_failure.has_value()) {
            result.system.package_state_change = PackageStateChange::Unknown;
        } else {
            result.system.package_state_change = PackageStateChange::Unknown;
        }
        system_package_state_finalized = true;

        // POLICY: post-Syu metadata failureは、1件でもsource mutationを始める前に
        // preference順で全correlationを検査する。
        for(std::size_t source_position = 0;
            source_position < state.correlations.size();
            ++source_position) {
            const RegisteredSourceCorrelation& correlation =
                state.correlations[source_position];
            if(!correlation.has_valid_package_name) continue;
            if(!correlation.work_item_index.has_value() ||
               !state.source_invocation.has_value()) {
                add_inconsistent_issue(
                    result,
                    SystemSourceUpgradeIssueKind::
                        PreparedCorrelationInconsistent,
                    SystemSourceUpgradePhase::RegisteredSource,
                    localization::translate_message(
                        "The prepared source work item is missing after the system upgrade."));
                return result;
            }

            const ProductionSourceBuildWorkItem& work_item =
                state.source_invocation->work_items[correlation.work_item_index.value()];
            auto installed_snapshot = installed_snapshots.find(
                work_item.request.package_name);
            if(installed_snapshot == installed_snapshots.end()) {
                add_inconsistent_issue(
                    result,
                    SystemSourceUpgradeIssueKind::
                        PreparedCorrelationInconsistent,
                    SystemSourceUpgradePhase::RegisteredSource,
                    localization::format_translated_message(
                        "The system upgrade completed, but the authoritative post-upgrade installed package snapshot is missing for {}; source processing did not start.",
                        work_item.request.package_name));
                return result;
            }
            if(const auto* metadata_failure =
                   std::get_if<PackageMetadataFailure>(
                       &installed_snapshot->second)) {
                const std::string diagnostic =
                    localization::format_translated_message(
                        "The system upgrade completed, but the post-upgrade package metadata query failed for {}: {} Source processing did not start.",
                        work_item.request.package_name,
                        metadata_failure->diagnostic);
                stop_for_source_metadata_failure(
                    result,
                    source_position,
                    *metadata_failure,
                    diagnostic);
                return result;
            }
        }

        if(state.source_invocation.has_value()) {
            try {
                // POLICY(#272): system phaseと全post-system metadata guardの後、
                // registered-source checkout/buildより前にphase全体で1回行う。
                result.selected_repository_provider_transaction =
                    execute_selected_repository_provider_transaction(
                        state.source_invocation.value(), config);
                if(!result.selected_repository_provider_transaction.is_success()) {
                    stop_for_repository_provider_transaction_failure(
                        result,
                        result.selected_repository_provider_transaction.diagnostic.value_or(
                            localization::translate_message(
                                "Failed to install selected repository providers.")));
                    return result;
                }
            } catch(const TrustedCacheError& error) {
                result.selected_repository_provider_transaction.status =
                    SelectedRepositoryProviderTransactionStatus::
                        BlockedBeforeExecution;
                result.selected_repository_provider_transaction.selected_providers =
                    state.source_invocation->selected_repository_providers;
                result.selected_repository_provider_transaction.package_state_change =
                    PackageStateChange::NoChange;
                result.selected_repository_provider_transaction.diagnostic =
                    error.what();
                SystemSourceUpgradeIssue issue = make_issue(
                    SystemSourceUpgradeIssueKind::CacheAuthorityInvalid,
                    SystemSourceUpgradeIssueImpact::BlocksExecution,
                    SystemSourceUpgradePhase::RegisteredSource,
                    error.what());
                issue.trusted_cache_failure = error.failure();
                result.issues.push_back(std::move(issue));
                result.diagnostics.push_back(make_diagnostic(
                    SystemSourceUpgradePhase::RegisteredSource,
                    error.what(), true));
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const std::exception& error) {
                result.selected_repository_provider_transaction.status =
                    SelectedRepositoryProviderTransactionStatus::
                        BlockedBeforeExecution;
                result.selected_repository_provider_transaction.selected_providers =
                    state.source_invocation->selected_repository_providers;
                result.selected_repository_provider_transaction.diagnostic =
                    error.what();
                stop_for_repository_provider_transaction_failure(
                    result, error.what());
                return result;
            } catch(...) {
                const std::string diagnostic =
                    localization::translate_message(
                        "Failed to install selected repository providers with an unknown exception.");
                result.selected_repository_provider_transaction.status =
                    SelectedRepositoryProviderTransactionStatus::
                        BlockedBeforeExecution;
                result.selected_repository_provider_transaction.selected_providers =
                    state.source_invocation->selected_repository_providers;
                result.selected_repository_provider_transaction.diagnostic =
                    diagnostic;
                stop_for_repository_provider_transaction_failure(
                    result, diagnostic);
                return result;
            }
        }

        for(std::size_t source_position = 0;
            source_position < state.correlations.size();
            ++source_position) {
            const RegisteredSourceCorrelation& correlation =
                state.correlations[source_position];
            RegisteredSourceUpgradeResult& source_result =
                result.registered_source_results[source_position];
            if(!correlation.has_valid_package_name) {
                const std::string diagnostic =
                    localization::format_translated_message(
                        "Ignoring invalid source-build preference filename: {}",
                        source_result.preference_package_name);
                source_result.status = RegisteredSourceUpgradeStatus::Unsupported;
                source_result.failure_kind =
                    RegisteredSourceUpgradeFailureKind::InvalidPreferenceName;
                source_result.package_state_change = PackageStateChange::NoChange;
                source_result.diagnostic = diagnostic;

                result.warnings.push_back(SystemSourceUpgradeWarning{
                    SystemSourceUpgradeWarningKind::InvalidPreferenceName,
                    source_result.original_preference_index,
                    source_result.preference_package_name,
                    result.prepared_snapshot.registered_sources[source_position].entry_path,
                    diagnostic});
                SystemSourceUpgradeIssue issue = make_issue(
                    SystemSourceUpgradeIssueKind::InvalidPreferenceName,
                    SystemSourceUpgradeIssueImpact::AffectsSuccess,
                    SystemSourceUpgradePhase::RegisteredSource,
                    diagnostic);
                issue.original_preference_index =
                    source_result.original_preference_index;
                issue.preference_package_name =
                    source_result.preference_package_name;
                result.issues.push_back(std::move(issue));
                notify(observer, SystemSourceUpgradeEvent{
                                     SystemSourceUpgradeEventKind::InvalidPreferenceWarning,
                                     source_result.original_preference_index,
                                     source_result.preference_package_name,
                                     result.prepared_snapshot.registered_sources[source_position].entry_path,
                                     diagnostic});
                continue;
            }

            ProductionSourceBuildWorkItem& work_item =
                state.source_invocation->work_items[correlation.work_item_index.value()];
            if(work_item.uses_system_update_baseline) {
                auto baseline = state.update_baselines.find(
                    work_item.request.package_name);
                if(baseline != state.update_baselines.end()) {
                    work_item.request.update_baseline = baseline->second;
                }
            }
            work_item.request.installed_snapshot =
                std::get<SourceInstalledSnapshot>(
                    installed_snapshots.at(
                        work_item.request.package_name));

            active_source_position = source_position;
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
            throw_unexpected_exception_for_test(
                state,
                SystemSourceUpgradeUnexpectedExceptionPoint::
                    SourceWorkItemStarted);
#endif
            const RegisteredSourcePreferenceSnapshot& prepared_source =
                result.prepared_snapshot.registered_sources[source_position];
            const bool is_repository_source =
                prepared_source.source_kind ==
                std::optional<SourceBuildSourceKind>{
                    SourceBuildSourceKind::Repository};
            auto add_source_diagnostic =
                [&](const std::string& diagnostic, bool stops_execution) {
                    SystemSourceUpgradeDiagnostic detail =
                        make_diagnostic(
                            SystemSourceUpgradePhase::
                                RegisteredSource,
                            diagnostic,
                            stops_execution);
                    detail.original_preference_index =
                        source_result.original_preference_index;
                    detail.preference_package_name =
                        source_result.preference_package_name;
                    detail.resolved_package_base =
                        source_result.resolved_package_base;
                    result.diagnostics.push_back(std::move(detail));
                };
            try {
                if(is_repository_source) {
                    SourceBuildPreparationOutcome preparation =
                        prepare_package_base_source_build_work_item_typed(
                            work_item,
                            SourceBuildUpdatePolicy::OnlyIfUpdated,
                            config);
                    if(const auto* up_to_date =
                           std::get_if<SourceBuildUpToDate>(
                               &preparation)) {
                        map_registered_up_to_date(
                            source_result, *up_to_date);
                    } else if(const auto* skipped = std::get_if<
                                  SourceBuildUpdateStatusUnknownSkipped>(
                                  &preparation)) {
                        map_registered_unknown_skip(
                            source_result, *skipped);
                        if(!skipped->diagnostic.empty()) {
                            add_source_diagnostic(
                                skipped->diagnostic, false);
                        }
                    } else {
                        RegisteredSourcePackageBaseExecutionResult execution =
                            execute_prepared_package_base_source_build_work_item_typed(
                                work_item,
                                std::move(std::get<
                                          PreparedSourceBuildNeedsBuild>(
                                    preparation)),
                                state.source_invocation->database_paths,
                                config);
                        if(auto failure = map_registered_package_base_result(
                               source_result, execution, work_item,
                               false);
                           failure.has_value()) {
                            record_registered_correlation_failure(
                                source_result,
                                std::move(failure.value()));
                            add_source_diagnostic(
                                source_result.diagnostic.value(), true);
                            result.status = SystemSourceUpgradeStatus::
                                StoppedOnSourceFailure;
                            result.stopped_phase =
                                SystemSourceUpgradePhase::
                                    RegisteredSource;
                            return result;
                        }
                    }
                } else {
                    if(prepared_source.source_kind !=
                           std::optional<SourceBuildSourceKind>{
                               SourceBuildSourceKind::Aur} ||
                       work_item.required_target_provenance !=
                           RequiredTargetProvenance::
                               AurBuildPlanProjection ||
                       work_item.artifact_lifecycle_intent !=
                           ArtifactLifecycleIntent::
                               SingularCompatibility ||
                       !work_item.request.only_if_updated ||
                       work_item.request.needed) {
                        throw std::logic_error(
                            "Registered AUR source-build route correlation is inconsistent.");
                    }
                    SourceBuildExecutionResult execution =
                        execute_prepared_source_build_work_item_typed(
                            work_item,
                            state.source_invocation->database_paths,
                            config);
                    map_source_execution_result(source_result, std::move(execution));
                    if(source_result.failure_kind == RegisteredSourceUpgradeFailureKind::AuthoritativeExecutionIncomplete) {
                        add_source_diagnostic(source_result.diagnostic.value_or("Authoritative execution incomplete; installation and publication outcomes are retained."), true);
                        result.status = SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                        result.stopped_phase = SystemSourceUpgradePhase::RegisteredSource;
                        return result;
                    }
                    if(execution.status ==
                           SourceBuildExecutionStatus::
                               UpdateStatusUnknownSkipped &&
                       !execution.diagnostic.empty()) {
                        add_source_diagnostic(
                            execution.diagnostic, false);
                    }
                }
            } catch(const RegisteredSourcePackageBaseCleanupError& error) {
                if(auto failure = map_registered_package_base_result(
                       source_result, error.result(), work_item, true,
                       error.what());
                   failure.has_value()) {
                    record_registered_correlation_failure(
                        source_result, std::move(failure.value()));
                    add_source_diagnostic(
                        source_result.diagnostic.value(), true);
                    result.status =
                        SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                } else {
                    add_source_diagnostic(error.what(), true);
                    result.status = SystemSourceUpgradeStatus::
                        StoppedAfterSourceCleanupFailure;
                }
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const RegisteredSourcePackageBasePreparationError& error) {
                if(auto failure = validate_registered_preparation_failure(
                       error, work_item);
                   failure.has_value()) {
                    record_registered_correlation_failure(
                        source_result, std::move(failure.value()));
                } else {
                    record_registered_preparation_failure(
                        source_result, error, work_item);
                }
                add_source_diagnostic(
                    source_result.diagnostic.value(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const RegisteredSourcePackageBasePhaseError& error) {
                record_registered_phase_failure(
                    source_result, error, work_item);
                add_source_diagnostic(
                    source_result.diagnostic.value(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const RegisteredSourcePackageTransactionError& error) {
                record_registered_transaction_failure(
                    source_result, error, work_item);
                add_source_diagnostic(
                    source_result.diagnostic.value(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const PackageMetadataError& error) {
                source_result.status = RegisteredSourceUpgradeStatus::Failed;
                source_result.failure_kind =
                    RegisteredSourceUpgradeFailureKind::
                        BuildOrInstallFailed;
                source_result.package_state_change =
                    PackageStateChange::Unknown;
                source_result.diagnostic = is_repository_source
                                               ? registered_source_build_failure_diagnostic(
                                                     work_item, error.what())
                                               : error.what();
                if(is_repository_source) {
                    source_result.failure_detail.emplace<
                        PackageMetadataFailure>(error.failure());
                }
                add_source_diagnostic(
                    source_result.diagnostic.value(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const SeparatedSourceBuildCleanupError& error) {
                map_cleanup_failure(source_result, error);
                add_source_diagnostic(error.what(), true);
                result.status = SystemSourceUpgradeStatus::
                    StoppedAfterSourceCleanupFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const SeparatedSourceBuildPhaseError& error) {
                record_registered_singular_source_failure(
                    source_result, error.what(),
                    error.production_outcome(),
                    map_registered_source_build_phase(error.phase()));
                if(error.package_metadata_failure().has_value()) {
                    source_result.failure_detail.emplace<
                        PackageMetadataFailure>(
                        *error.package_metadata_failure());
                }
                add_source_diagnostic(error.what(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const ReviewedSourceProductionError& error) {
                record_registered_singular_source_failure(
                    source_result, error.what(),
                    error.production_outcome(),
                    RegisteredSourceBuildFailureCategory::Other,
                    error.failure());
                add_source_diagnostic(error.what(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const TrustedCacheError& error) {
                source_result.status = RegisteredSourceUpgradeStatus::Failed;
                source_result.failure_kind =
                    RegisteredSourceUpgradeFailureKind::
                        CacheAuthorityFailure;
                source_result.package_state_change =
                    PackageStateChange::Unknown;
                source_result.diagnostic = error.what();

                SystemSourceUpgradeIssue issue = make_issue(
                    SystemSourceUpgradeIssueKind::CacheAuthorityInvalid,
                    SystemSourceUpgradeIssueImpact::BlocksExecution,
                    SystemSourceUpgradePhase::RegisteredSource,
                    error.what());
                issue.original_preference_index =
                    source_result.original_preference_index;
                issue.preference_package_name =
                    source_result.preference_package_name;
                issue.resolved_package_base =
                    source_result.resolved_package_base;
                issue.trusted_cache_failure = error.failure();
                result.issues.push_back(std::move(issue));

                add_source_diagnostic(error.what(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(const ConfirmationOperationStopped&) {
                throw;
            } catch(const std::exception& error) {
                source_result.status = RegisteredSourceUpgradeStatus::Failed;
                source_result.failure_kind =
                    RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed;
                source_result.package_state_change = PackageStateChange::Unknown;
                source_result.diagnostic = is_repository_source
                                               ? registered_source_build_failure_diagnostic(
                                                     work_item, error.what())
                                               : error.what();
                if(is_repository_source) {
                    source_result.failure_detail.emplace<
                        RegisteredSourceBuildFailureSnapshot>(
                        RegisteredSourceBuildFailureSnapshot{
                            RegisteredSourceBuildFailureCategory::
                                Other,
                            error.what(), std::nullopt});
                }
                add_source_diagnostic(
                    source_result.diagnostic.value(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            } catch(...) {
                source_result.status = RegisteredSourceUpgradeStatus::Failed;
                source_result.failure_kind =
                    RegisteredSourceUpgradeFailureKind::UnknownException;
                source_result.package_state_change = PackageStateChange::Unknown;
                source_result.diagnostic =
                    unknown_source_failure_diagnostic();
                if(is_repository_source) {
                    source_result.failure_detail.emplace<
                        RegisteredSourceBuildFailureSnapshot>(
                        RegisteredSourceBuildFailureSnapshot{
                            RegisteredSourceBuildFailureCategory::
                                Other,
                            unknown_source_failure_diagnostic(),
                            std::nullopt});
                }
                add_source_diagnostic(
                    unknown_source_failure_diagnostic(), true);
                result.status =
                    SystemSourceUpgradeStatus::StoppedOnSourceFailure;
                result.stopped_phase =
                    SystemSourceUpgradePhase::RegisteredSource;
                return result;
            }
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
            throw_unexpected_exception_for_test(
                state,
                SystemSourceUpgradeUnexpectedExceptionPoint::
                    SourceResultRecorded);
#endif
            active_source_position.reset();
        }

        return result;
    } catch(const ConfirmationOperationStopped&) {
        throw;
    } catch(const std::exception& error) {
        record_unexpected_execution_failure(
            result,
            active_phase,
            active_source_position,
            system_package_state_finalized,
            error.what());
        return result;
    } catch(...) {
        record_unexpected_execution_failure(
            result,
            active_phase,
            active_source_position,
            system_package_state_finalized,
            localization::translate_message(
                "An unknown exception occurred."));
        return result;
    }
}
