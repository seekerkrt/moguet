#include "app_config.hpp"
#include "upgrade_all_operation.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// CLI boundaryだけを決定的に検証するaggregate operation stub。
// POLICY(#281): result helperはproduction TUをlinkし、このfileはone-shot capabilityと
// prepare/execute APIだけを差し替える。presentationやexit判定はstub化しない。
namespace {

struct UnknownFixtureException {};

ProductionSourceBuildProvenance localized_reviewed_source_provenance() {
    ProductionSourceBuildProvenance provenance;
    provenance.review_status = ProductionSourceReviewStatus::Reviewed;
    provenance.editor_overlay =
        ReviewedSourceEditorOverlayStatus::InvocationLocal;
    provenance.reviewed_upstream_base_revision =
        SourceRevisionIdentity::git_commit(
            "4444444444444444444444444444444444444444");
    provenance.publication_status =
        ReviewedSourcePublicationStatus::Published;
    provenance.reviewed_outcome =
        ProductionReviewedSourceOutcome::UpdateReview;
    provenance.reviewed_state_generation = 31;
    return provenance;
}

ProductionSourceBuildStagedOutcome localized_reviewed_source_outcome() {
    return ProductionSourceBuildStagedOutcome{
        localized_reviewed_source_provenance(),
        ProductionSourceBuildCommandOutcome::Succeeded,
        ProductionSourceInstallOutcome::Succeeded};
}

std::string current_scenario() {
    const char* value = std::getenv("MOGUET_TEST_UPGRADE_ALL_SCENARIO");
    if(value == nullptr || value[0] == '\0') {
        throw std::logic_error(
            "MOGUET_TEST_UPGRADE_ALL_SCENARIO is required.");
    }
    return value;
}

std::string current_matrix_kind() {
    const char* value =
        std::getenv("MOGUET_TEST_UPGRADE_ALL_MATRIX_KIND");
    if(value == nullptr || value[0] == '\0') {
        throw std::logic_error(
            "MOGUET_TEST_UPGRADE_ALL_MATRIX_KIND is required.");
    }
    return value;
}

std::size_t current_matrix_index() {
    const char* value =
        std::getenv("MOGUET_TEST_UPGRADE_ALL_MATRIX_INDEX");
    if(value == nullptr || value[0] == '\0') {
        throw std::logic_error(
            "MOGUET_TEST_UPGRADE_ALL_MATRIX_INDEX is required.");
    }

    std::size_t parsed_length = 0;
    const unsigned long long parsed =
        std::stoull(value, &parsed_length, 10);
    if(value[parsed_length] != '\0' ||
       parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::logic_error(
            "MOGUET_TEST_UPGRADE_ALL_MATRIX_INDEX is invalid.");
    }
    return static_cast<std::size_t>(parsed);
}

template <typename Enum, std::size_t Size>
Enum matrix_enum_value(
    const std::array<Enum, Size>& values,
    std::size_t index) {
    if(index < values.size()) return values[index];
    if(index == values.size()) return static_cast<Enum>(-1);
    throw std::logic_error(
        "Upgrade-all presentation matrix index is out of range.");
}

// POLICY(#281): production formatterのswitchと同じconcrete enumerator集合を
// test dataとして明示する。各array直後の1 indexは未知値fail-closed用。
constexpr std::array UPGRADE_ALL_AUR_PHASE_STATUSES{
    UpgradeAllAurPhaseStatus::NotAttempted,
    UpgradeAllAurPhaseStatus::NoUpdates,
    UpgradeAllAurPhaseStatus::Completed,
    UpgradeAllAurPhaseStatus::BlockedBeforeExecution,
    UpgradeAllAurPhaseStatus::StoppedOnProviderTransactionFailure,
    UpgradeAllAurPhaseStatus::StoppedOnWorkItemFailure,
    UpgradeAllAurPhaseStatus::StoppedAfterCleanupFailure,
    UpgradeAllAurPhaseStatus::InconsistentResult};

constexpr std::array UPGRADE_ALL_NOT_ATTEMPTED_REASONS{
    UpgradeAllNotAttemptedReason::PreparationBlocked,
    UpgradeAllNotAttemptedReason::SystemFailure,
    UpgradeAllNotAttemptedReason::SourceFailure,
    UpgradeAllNotAttemptedReason::SourceCleanupFailure,
    UpgradeAllNotAttemptedReason::SystemSourceIncomplete,
    UpgradeAllNotAttemptedReason::ForeignInventoryFailure,
    UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
    UpgradeAllNotAttemptedReason::PriorAggregateInconsistency};

constexpr std::array AUR_TARGET_STATUSES{
    AurUpdateOperationTargetStatus::Updated,
    AurUpdateOperationTargetStatus::NoChange,
    AurUpdateOperationTargetStatus::Skipped,
    AurUpdateOperationTargetStatus::Unsupported,
    AurUpdateOperationTargetStatus::Incomplete,
    AurUpdateOperationTargetStatus::Failed,
    AurUpdateOperationTargetStatus::UpdatedCleanupFailed,
    AurUpdateOperationTargetStatus::NoChangeCleanupFailed,
    AurUpdateOperationTargetStatus::NotAttempted};

constexpr std::array AUR_OPERATION_STATUSES{
    AurUpdateOperationStatus::NoUpdates,
    AurUpdateOperationStatus::Completed,
    AurUpdateOperationStatus::BlockedBeforeExecution,
    AurUpdateOperationStatus::StoppedOnProviderTransactionFailure,
    AurUpdateOperationStatus::StoppedOnWorkItemFailure,
    AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure,
    AurUpdateOperationStatus::InconsistentResult};

constexpr std::array AUR_PREFLIGHT_REASONS{
    AurUpdateExecutionReason::None,
    AurUpdateExecutionReason::UpToDate,
    AurUpdateExecutionReason::DevelRequiresCheck,
    AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck,
    AurUpdateExecutionReason::NonAurForeign,
    AurUpdateExecutionReason::AurMetadataUnavailable,
    AurUpdateExecutionReason::VersionComparisonUnavailable,
    AurUpdateExecutionReason::InstalledReasonUnknown,
    AurUpdateExecutionReason::UpdatePlanInconsistent,
    AurUpdateExecutionReason::DuplicateUpdateTarget,
    AurUpdateExecutionReason::RepositoryMetadataUnavailable,
    AurUpdateExecutionReason::AurDependencyMetadataUnavailable,
    AurUpdateExecutionReason::ProviderMetadataUnavailable,
    AurUpdateExecutionReason::UnresolvedDependency,
    AurUpdateExecutionReason::VersionConstraintUnverified,
    AurUpdateExecutionReason::DependencyCycle,
    AurUpdateExecutionReason::BuildPlanInconsistent,
    AurUpdateExecutionReason::PackageBaseMismatch,
    AurUpdateExecutionReason::SplitPackageSelectionRequired,
    AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase,
    AurUpdateExecutionReason::AmbiguousProvider,
    AurUpdateExecutionReason::ConflictsOrReplacesUnresolved,
    AurUpdateExecutionReason::InstalledPackageMetadataUnavailable};

constexpr std::array AUR_PREPARATION_REASONS{
    AurUpdatePreparationReason::None,
    AurUpdatePreparationReason::BlockingPreflight,
    AurUpdatePreparationReason::PreflightInconsistent,
    AurUpdatePreparationReason::DevelRequiresCheckPolicyInconsistent,
    AurUpdatePreparationReason::BuildPlanMissing,
    AurUpdatePreparationReason::BuildPlanOrderEmpty,
    AurUpdatePreparationReason::RootAttributionInconsistent,
    AurUpdatePreparationReason::PackageTargetAttributionInconsistent,
    AurUpdatePreparationReason::DesiredInstallReasonMissing,
    AurUpdatePreparationReason::SourcePreferenceUnavailable,
    AurUpdatePreparationReason::SourcePreferencePkgdestConflict,
    AurUpdatePreparationReason::StaticWorkItemInvalid,
    AurUpdatePreparationReason::PacmanDatabaseUnavailable,
    AurUpdatePreparationReason::GenericPreparationInconsistent,
    AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
    AurUpdatePreparationReason::ExternalSatisfactionInconsistent};

constexpr std::array AUR_EXECUTION_FAILURE_KINDS{
    AurUpdateWorkItemFailureKind::None,
    AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
    AurUpdateWorkItemFailureKind::CleanupFailedAfterPackageTransaction,
    AurUpdateWorkItemFailureKind::UnknownException,
    AurUpdateWorkItemFailureKind::PriorWorkItemStopped};

constexpr std::array AUR_REDUCTION_STAGES{
    AurUpdateOperationReductionStage::Preflight,
    AurUpdateOperationReductionStage::Preparation,
    AurUpdateOperationReductionStage::Execution};

constexpr std::array AUR_REDUCTION_REASONS{
    AurUpdateOperationReductionReason::
        DuplicatePreflightUpdatePlanIndex,
    AurUpdateOperationReductionReason::
        OutOfRangePreflightUpdatePlanIndex,
    AurUpdateOperationReductionReason::PreflightTargetOrderInconsistent,
    AurUpdateOperationReductionReason::DuplicatePreparationAttribution,
    AurUpdateOperationReductionReason::UnknownPreparationUpdatePlanIndex,
    AurUpdateOperationReductionReason::PreparationAttributionInconsistent,
    AurUpdateOperationReductionReason::
        PreparationTargetSnapshotInconsistent,
    AurUpdateOperationReductionReason::DuplicateExecutionWorkItemIndex,
    AurUpdateOperationReductionReason::
        ExecutionWorkItemOrderInconsistent,
    AurUpdateOperationReductionReason::DuplicateExecutionAttribution,
    AurUpdateOperationReductionReason::UnknownExecutionUpdatePlanIndex,
    AurUpdateOperationReductionReason::MissingExecutionAttribution,
    AurUpdateOperationReductionReason::
        DuplicateExecutionChildAttribution,
    AurUpdateOperationReductionReason::
        MissingExecutionChildAttribution,
    AurUpdateOperationReductionReason::
        UnexpectedExecutionChildAttribution,
    AurUpdateOperationReductionReason::
        UnknownExecutionChildUpdatePlanIndex,
    AurUpdateOperationReductionReason::
        ExecutionChildSnapshotInconsistent,
    AurUpdateOperationReductionReason::UnexpectedSelectedArtifact,
    AurUpdateOperationReductionReason::
        UnexpectedUnselectedArtifactIdentity,
    AurUpdateOperationReductionReason::
        ExecutionResultWithPreparationIssues,
    AurUpdateOperationReductionReason::MissingExecutionResult,
    AurUpdateOperationReductionReason::
        DevelRequiresCheckPolicyInconsistent,
    AurUpdateOperationReductionReason::UnknownEnumValue,
    AurUpdateOperationReductionReason::WorkItemResultInconsistent,
    AurUpdateOperationReductionReason::InvocationResultInconsistent,
    AurUpdateOperationReductionReason::OtherCorrelationInconsistent};

constexpr std::array FILTERED_AUR_ISSUE_KINDS{
    FilteredAurUpdateOperationIssueKind::UnknownUpdateClassification,
    FilteredAurUpdateOperationIssueKind::
        DevelRequiresCheckPolicyInconsistent,
    FilteredAurUpdateOperationIssueKind::
        TargetPlannerMappingInconsistent,
    FilteredAurUpdateOperationIssueKind::
        FilteredTargetMappingInconsistent,
    FilteredAurUpdateOperationIssueKind::
        PreflightTargetMappingInconsistent,
    FilteredAurUpdateOperationIssueKind::
        PreflightInvocationIndexOutOfRange,
    FilteredAurUpdateOperationIssueKind::
        PreflightInvocationIdentityMismatch,
    FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexMissing,
    FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexOutOfRange,
    FilteredAurUpdateOperationIssueKind::BuildPlanRootIdentityMismatch,
    FilteredAurUpdateOperationIssueKind::
        BuildPlanRootPackageIdentityMismatch,
    FilteredAurUpdateOperationIssueKind::BuildUnitOrderIdentityMismatch,
    FilteredAurUpdateOperationIssueKind::
        BuildUnitRootAttributionInconsistent,
    FilteredAurUpdateOperationIssueKind::
        BuildUnitSelectionMappingInconsistent,
    FilteredAurUpdateOperationIssueKind::
        ExecutionBuildUnitMappingInconsistent,
    FilteredAurUpdateOperationIssueKind::ReducedTargetMappingInconsistent};

constexpr std::array UPGRADE_ALL_PLANNING_ISSUE_KINDS{
    UpgradeAllPlanningIssueKind::ExplicitPreferencePackageNameMissing,
    UpgradeAllPlanningIssueKind::ExplicitProducedPackageNameMissing,
    UpgradeAllPlanningIssueKind::ExplicitPackageBaseAbsent,
    UpgradeAllPlanningIssueKind::ExplicitPackageBaseResolutionFailed,
    UpgradeAllPlanningIssueKind::ExplicitPackageBaseEmpty,
    UpgradeAllPlanningIssueKind::ExplicitSourceIdentityAbsent,
    UpgradeAllPlanningIssueKind::ExplicitSourceIdentityResolutionFailed,
    UpgradeAllPlanningIssueKind::ExplicitSourceIdentityEmpty,
    UpgradeAllPlanningIssueKind::
        ConflictingExplicitSourceIdentityDefinition,
    UpgradeAllPlanningIssueKind::ConflictingExplicitPackageName,
    UpgradeAllPlanningIssueKind::ConflictingExplicitPackageBase,
    UpgradeAllPlanningIssueKind::AurTargetPackageNameMissing,
    UpgradeAllPlanningIssueKind::AurTargetPackageBaseAbsent,
    UpgradeAllPlanningIssueKind::AurTargetPackageBaseResolutionFailed,
    UpgradeAllPlanningIssueKind::AurTargetPackageBaseEmpty,
    UpgradeAllPlanningIssueKind::UnsupportedAurTarget,
    UpgradeAllPlanningIssueKind::IncompleteAurTarget,
    UpgradeAllPlanningIssueKind::BuildUnitPackageBaseAbsent,
    UpgradeAllPlanningIssueKind::BuildUnitPackageBaseResolutionFailed,
    UpgradeAllPlanningIssueKind::BuildUnitPackageBaseEmpty,
    UpgradeAllPlanningIssueKind::BuildUnitHasNoRootAttribution,
    UpgradeAllPlanningIssueKind::BuildUnitTargetIndexOutOfRange,
    UpgradeAllPlanningIssueKind::DuplicateSelectedTargetPackageBase,
    UpgradeAllPlanningIssueKind::DuplicateSelectedBuildUnitPackageBase};

constexpr std::array UPGRADE_ALL_TARGET_DISPOSITIONS{
    UpgradeAllTargetDisposition::Selected,
    UpgradeAllTargetDisposition::ExcludedByExplicitPackageName,
    UpgradeAllTargetDisposition::ExcludedByExplicitPackageBase,
    UpgradeAllTargetDisposition::Unsupported,
    UpgradeAllTargetDisposition::IdentityIncomplete,
    UpgradeAllTargetDisposition::ConflictingExplicitSourceIdentity,
    UpgradeAllTargetDisposition::ConflictingSelectedPackageBase};

constexpr std::array UPGRADE_ALL_BUILD_UNIT_ROLES{
    UpgradeAllBuildUnitRole::Root,
    UpgradeAllBuildUnitRole::RuntimeDependency,
    UpgradeAllBuildUnitRole::BuildDependency,
    UpgradeAllBuildUnitRole::CheckDependency};

void append_event(const std::string& event) {
    const char* event_log_path = std::getenv("MOGUET_TEST_COMMAND_LOG");
    if(event_log_path == nullptr || event_log_path[0] == '\0') {
        throw std::logic_error("MOGUET_TEST_COMMAND_LOG is required.");
    }

    std::ofstream event_log(event_log_path, std::ios::app);
    if(!event_log) {
        throw std::runtime_error(
            "Cannot open the upgrade-all command event log.");
    }
    event_log << event << '\n';
}

const char* bool_text(bool value) {
    return value ? "true" : "false";
}

std::string config_snapshot(const AppConfig& config) {
    return "noedit=" + std::string(bool_text(config.user_config.review.pkgbuild == ReviewPolicy::Skip)) +
           " nodiff=" + bool_text(config.user_config.review.diff == ReviewPolicy::Skip) +
           " noconfirm=" + bool_text(config.no_confirm) +
           " rebuild=" + bool_text(config.user_config.build.mode == BuildMode::Rebuild) +
           " cleanbuild=" + bool_text(config.user_config.build.mode == BuildMode::Clean) +
           " rmdeps=" + bool_text(config.rm_deps);
}

AurUpdatePlanEntry make_update_entry(
    std::string package_name,
    std::string package_base = {}) {
    if(package_base.empty()) package_base = package_name;

    AurUpdatePlanEntry entry;
    entry.installed_name = std::move(package_name);
    entry.installed_version = "1.0-1";
    entry.install_reason = InstalledPackageReason::Explicit;
    entry.classification = AurUpdateClassification::UpdateAvailable;
    entry.aur_package = AurUpdateRemotePackage{
        entry.installed_name,
        std::move(package_base),
        "2.0-1",
        AurVersionRelation::NewerThanInstalled};
    return entry;
}

AurUpdateOperationTargetResult make_aur_target(
    std::string package_name,
    AurUpdateOperationTargetStatus status,
    std::string package_base = {}) {
    AurUpdateOperationTargetResult target;
    target.update_plan_index = 0;
    target.update = make_update_entry(
        std::move(package_name), std::move(package_base));
    target.package_base = target.update.aur_package->package_base;
    target.status = status;
    return target;
}

AurUpdateOperationTargetResult make_issue_449_split_up_to_date_target(
    std::size_t update_plan_index) {
    AurUpdateOperationTargetResult target = make_aur_target(
        "ttf-noto-sans-mono-cjk-vf",
        AurUpdateOperationTargetStatus::Skipped,
        "ttf-noto-sans-cjk-vf");
    target.update_plan_index = update_plan_index;
    target.update.installed_version = "2.004-1";
    target.update.classification = AurUpdateClassification::UpToDate;
    target.update.aur_package->version = "2.004-1";
    target.update.aur_package->version_relation =
        AurVersionRelation::SameAsInstalled;
    target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    target.preflight_issues.emplace_back(
        AurUpdateExecutionReason::UpToDate,
        "ttf-noto-sans-mono-cjk-vf",
        "ttf-noto-sans-cjk-vf",
        std::nullopt,
        "fixture wording is not the UpToDate classification authority");
    return target;
}

FilteredAurUpdateExecutionResult make_filtered_result(
    AurUpdateOperationStatus status) {
    FilteredAurUpdateExecutionResult filtered;
    filtered.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.reduced_operation_result.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.reduced_operation_result.status = status;
    if(status == AurUpdateOperationStatus::Completed) {
        filtered.reduced_operation_result.execution_status =
            AurUpdateInvocationExecutionStatus::Completed;
    }
    return filtered;
}

UpgradeAllOperationResult make_base_result(
    UpgradeAllOperationStatus status,
    PackageStateChange system_package_state,
    UpgradeAllAurPhaseStatus aur_status,
    AurUpdateOperationStatus nested_aur_status) {
    UpgradeAllOperationResult result;
    result.status = status;
    result.stopped_phase = UpgradeAllOperationPhase::None;
    result.system_source.status = SystemSourceUpgradeStatus::Completed;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::None;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change = system_package_state;
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::Completed;
    result.aur.status = aur_status;
    result.aur.operation_result.emplace(
        make_filtered_result(nested_aur_status));
    return result;
}

UpgradeAllOperationResult make_success_result(
    UpgradeAllOperationStatus status,
    PackageStateChange package_state) {
    const bool no_updates = status == UpgradeAllOperationStatus::NoUpdates;
    return make_base_result(
        status,
        package_state,
        no_updates ? UpgradeAllAurPhaseStatus::NoUpdates
                   : UpgradeAllAurPhaseStatus::Completed,
        no_updates ? AurUpdateOperationStatus::NoUpdates
                   : AurUpdateOperationStatus::Completed);
}

UpgradeAllOperationResult make_snapshot_unavailable_result(
    bool is_before_snapshot) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::Unknown);
    const std::string snapshot_position =
        is_before_snapshot ? "before" : "after";
    const PackageMetadataFailure failure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture " + snapshot_position + " snapshot unavailable"};
    if(is_before_snapshot) {
        result.system_source.system.before_snapshot_failure = failure;
    } else {
        result.system_source.system.after_snapshot_failure = failure;
    }

    const std::string diagnostic =
        "Failed to snapshot local package versions " +
        snapshot_position + " the system upgrade: " +
        failure.diagnostic;
    SystemSourceUpgradeIssue issue;
    issue.kind =
        SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable;
    issue.impact = SystemSourceUpgradeIssueImpact::ObservabilityOnly;
    issue.phase = SystemSourceUpgradePhase::System;
    issue.package_metadata_failure = failure;
    issue.diagnostic = diagnostic;
    result.system_source.issues.push_back(std::move(issue));

    SystemSourceUpgradeDiagnostic detail;
    detail.phase = SystemSourceUpgradePhase::System;
    detail.stops_execution = false;
    detail.diagnostic = diagnostic;
    result.system_source.diagnostics.push_back(std::move(detail));
    return result;
}

RegisteredSourcePreferenceSnapshot make_source_snapshot(
    std::size_t index,
    const std::string& package_name,
    const std::optional<std::string>& resolved_package_base) {
    RegisteredSourcePreferenceSnapshot snapshot;
    snapshot.original_preference_index = index;
    snapshot.preference_package_name = package_name;
    snapshot.entry_path = "/fixture/preferences/" + package_name;
    snapshot.canonical_source_identity_key =
        "repository:https://sources.example/" + package_name;
    snapshot.resolved_package_base = resolved_package_base;
    return snapshot;
}

RegisteredSourceUpgradeResult make_source_result(
    std::size_t index,
    std::string package_name,
    RegisteredSourceUpgradeStatus status,
    RegisteredSourceUpgradeFailureKind failure_kind,
    PackageStateChange package_state,
    std::optional<std::string> diagnostic = std::nullopt,
    std::optional<std::string> cleanup_diagnostic = std::nullopt) {
    RegisteredSourceUpgradeResult source;
    source.original_preference_index = index;
    source.preference_package_name = std::move(package_name);
    source.canonical_source_identity_key =
        "repository:https://sources.example/" +
        source.preference_package_name;
    source.resolved_package_base =
        source.preference_package_name + "-base";
    source.status = status;
    source.failure_kind = failure_kind;
    source.package_state_change = package_state;
    source.diagnostic = std::move(diagnostic);
    source.cleanup_diagnostic = std::move(cleanup_diagnostic);
    return source;
}

void add_source(
    UpgradeAllOperationResult& result,
    RegisteredSourceUpgradeResult source) {
    result.prepared_snapshot.system_source.registered_sources.push_back(
        make_source_snapshot(
            source.original_preference_index,
            source.preference_package_name,
            source.resolved_package_base));
    result.system_source.prepared_snapshot.registered_sources.push_back(
        make_source_snapshot(
            source.original_preference_index,
            source.preference_package_name,
            source.resolved_package_base));
    result.system_source.registered_source_results.push_back(
        std::move(source));
}

UpgradeAllOperationIssue make_aggregate_issue(
    UpgradeAllOperationIssueKind kind,
    UpgradeAllOperationPhase phase,
    std::string diagnostic) {
    UpgradeAllOperationIssue issue;
    issue.kind = kind;
    issue.phase = phase;
    issue.diagnostic = std::move(diagnostic);
    return issue;
}

void add_stopping_diagnostic(
    UpgradeAllOperationResult& result,
    UpgradeAllOperationPhase phase,
    const std::string& diagnostic) {
    result.diagnostics.push_back(
        UpgradeAllOperationDiagnostic{phase, true, diagnostic});
}

AurUpdateWorkItemExecutionResult make_work_item_result(
    std::size_t index,
    const std::string& package_name,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic);

UpgradeAllOperationResult make_completed_changed_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::Changed);
    add_source(
        result,
        make_source_result(
            0,
            "source-updated",
            RegisteredSourceUpgradeStatus::Updated,
            RegisteredSourceUpgradeFailureKind::None,
            PackageStateChange::Changed));
    add_source(
        result,
        make_source_result(
            1,
            "source-no-change",
            RegisteredSourceUpgradeStatus::NoChange,
            RegisteredSourceUpgradeFailureKind::None,
            PackageStateChange::NoChange));

    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    filtered.reduced_operation_result.targets.push_back(
        make_aur_target(
            "aur-updated",
            AurUpdateOperationTargetStatus::Updated));
    filtered.reduced_operation_result.targets.push_back(
        make_aur_target(
            "aur-no-change",
            AurUpdateOperationTargetStatus::NoChange));
    filtered.reduced_operation_result.execution_work_items.push_back(
        make_work_item_result(
            0,
            "aur-updated",
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None,
            std::nullopt));
    filtered.reduced_operation_result.execution_work_items.push_back(
        make_work_item_result(
            1,
            "aur-no-change",
            AurUpdateWorkItemExecutionStatus::NoChange,
            AurUpdateWorkItemFailureKind::None,
            std::nullopt));

    result.warnings.push_back(UpgradeAllOperationWarning{
        UpgradeAllOperationWarningKind::RegisteredSourcePreference,
        UpgradeAllOperationPhase::Preparation,
        0,
        "source-updated",
        "fixture registered source warning"});
    result.warnings.push_back(UpgradeAllOperationWarning{
        UpgradeAllOperationWarningKind::AurPreparation,
        UpgradeAllOperationPhase::AurPreparation,
        std::nullopt,
        "aur-updated",
        "fixture AUR preparation warning"});
    return result;
}

UpgradeAllDuplicateExcludedAurTarget make_duplicate_exclusion(
    std::size_t index,
    const std::string& package_name,
    const std::string& package_base,
    UpgradeAllTargetDisposition disposition,
    std::optional<std::string> matched_package_name,
    std::optional<std::string> matched_package_base,
    std::string source_identity) {
    UpgradeAllTargetPlanEntry planner_entry;
    planner_entry.original_target_index = index;
    planner_entry.target.package_name = package_name;
    planner_entry.target.package_base =
        UpgradeAllResolvedPackageBase{package_base};
    planner_entry.target.status = UpgradeAllAurTargetStatus::Candidate;
    planner_entry.disposition = disposition;
    planner_entry.explicit_source = UpgradeAllExplicitSourceAttribution{
        {index},
        {std::move(source_identity)},
        std::move(matched_package_name),
        std::move(matched_package_base)};

    UpgradeAllDuplicateExcludedAurTarget exclusion;
    exclusion.planner_target_index = index;
    exclusion.original_query_plan_index = index;
    exclusion.planner_entry = std::move(planner_entry);
    exclusion.query_entry = make_update_entry(package_name, package_base);
    return exclusion;
}

UpgradeAllOperationResult make_duplicate_exclusion_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.duplicate_excluded_aur_targets.push_back(
        make_duplicate_exclusion(
            0,
            "duplicate-by-name",
            "name-base",
            UpgradeAllTargetDisposition::
                ExcludedByExplicitPackageName,
            "duplicate-by-name",
            std::nullopt,
            "repository:https://sources.example/duplicate-by-name"));
    result.duplicate_excluded_aur_targets.push_back(
        make_duplicate_exclusion(
            1,
            "duplicate-by-base-child",
            "shared-base",
            UpgradeAllTargetDisposition::
                ExcludedByExplicitPackageBase,
            std::nullopt,
            "shared-base",
            "repository:https://sources.example/shared-base"));
    return result;
}

UpgradeAllOperationResult make_external_satisfaction_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);

    UpgradeAllExplicitSourceAdapterEntry source;
    source.adapter_index = 0;
    source.original_preference_index = 0;
    source.preference_package_name = "explicit-provider";
    source.canonical_source_identity_key =
        "repository:https://sources.example/explicit-provider";
    source.resolved_package_base = "external-base";
    source.affected_package_names = {"explicit-provider"};
    result.prepared_snapshot.explicit_source_adapter.entries.push_back(
        std::move(source));

    AurUpdateExternallySatisfiedBuildUnit operation_unit;
    operation_unit.build_plan_order_index = 0;
    operation_unit.package_name = "external-dependency";
    operation_unit.package_base = "external-base";
    operation_unit.plan_package_names = {"external-dependency"};
    operation_unit.affected_update_plan_indices = {0};
    operation_unit.affected_roots = {{0, "aur-root"}};
    operation_unit.roles = {PackageRole::BuildDependency};
    operation_unit.external_satisfaction.explicit_source_indexes = {0};
    operation_unit.external_satisfaction.source_identity_keys = {
        "repository:https://sources.example/explicit-provider"};
    operation_unit.external_satisfaction.matched_package_base =
        "external-base";

    FilteredAurUpdateBuildUnitRootCorrelation root;
    root.preflight_root = {0, "aur-root"};
    root.planner_target_index = 0;
    root.original_query_plan_index = 0;
    root.selected_target_index = 0;
    root.role = UpgradeAllBuildUnitRole::BuildDependency;
    result.externally_satisfied_aur_build_units.push_back(
        UpgradeAllExternallySatisfiedAurBuildUnit{
            std::move(operation_unit), {std::move(root)}});
    return result;
}

UpgradeAllOperationResult make_blocked_result() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::BlockedBeforeMutation;
    result.stopped_phase = UpgradeAllOperationPhase::Preparation;
    result.system_source.status =
        SystemSourceUpgradeStatus::BlockedBeforeMutation;
    result.system_source.stopped_phase =
        SystemSourceUpgradePhase::Preparation;
    result.system_source.system.status =
        SystemUpgradePhaseStatus::NotAttempted;
    add_source(
        result,
        make_source_result(
            0,
            "source-unsupported",
            RegisteredSourceUpgradeStatus::Unsupported,
            RegisteredSourceUpgradeFailureKind::InvalidPreferenceName,
            PackageStateChange::NoChange,
            "fixture unsupported source preference"));
    add_source(
        result,
        make_source_result(
            1,
            "source-incomplete",
            RegisteredSourceUpgradeStatus::Incomplete,
            RegisteredSourceUpgradeFailureKind::PreferenceUnavailable,
            PackageStateChange::NoChange,
            "fixture source preparation failed"));
    SystemSourceUpgradeIssue blocking_issue;
    blocking_issue.kind =
        SystemSourceUpgradeIssueKind::SourceWorkItemPreparationFailed;
    blocking_issue.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    blocking_issue.phase = SystemSourceUpgradePhase::Preparation;
    blocking_issue.original_preference_index = 1;
    blocking_issue.preference_package_name = "source-incomplete";
    blocking_issue.resolved_package_base = "source-incomplete-base";
    blocking_issue.diagnostic =
        std::string{"fixture blocking system/source issue\nunsafe-after"} +
        std::string{"\x1b", 1} + std::string{"\xe2\x80\xae", 3} +
        std::string{"\xef\xbb\xbf", 3} + std::string{"\xff", 1};
    result.system_source.issues.push_back(blocking_issue);

    SystemSourceUpgradeDiagnostic blocking_detail;
    blocking_detail.phase = SystemSourceUpgradePhase::Preparation;
    blocking_detail.original_preference_index = 1;
    blocking_detail.preference_package_name = "source-incomplete";
    blocking_detail.resolved_package_base = "source-incomplete-base";
    blocking_detail.stops_execution = true;
    blocking_detail.diagnostic = blocking_issue.diagnostic;
    result.system_source.diagnostics.push_back(std::move(blocking_detail));
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PreparationBlocked;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PreparationBlocked;
    result.issues.push_back(make_aggregate_issue(
        UpgradeAllOperationIssueKind::ExplicitSourceAdapterInvalid,
        UpgradeAllOperationPhase::Preparation,
        "fixture aggregate preparation blocked"));
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::Preparation,
        "fixture aggregate preparation blocked");
    return result;
}

UpgradeAllOperationResult make_system_failure_result() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::StoppedOnSystemFailure;
    result.stopped_phase = UpgradeAllOperationPhase::System;
    result.system_source.status =
        SystemSourceUpgradeStatus::StoppedOnSystemFailure;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::System;
    result.system_source.system.status = SystemUpgradePhaseStatus::Failed;
    result.system_source.system.package_state_change =
        PackageStateChange::Unknown;
    result.system_source.system.command_exit_status = 42;
    result.system_source.system.diagnostic =
        "fixture system upgrade failed";
    add_source(
        result,
        make_source_result(
            0,
            "source-after-system",
            RegisteredSourceUpgradeStatus::NotAttempted,
            RegisteredSourceUpgradeFailureKind::PriorPhaseStopped,
            PackageStateChange::NoChange));
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason =
        UpgradeAllNotAttemptedReason::SystemFailure;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::SystemFailure;
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::System,
        "fixture system upgrade failed");
    return result;
}

struct CrossSourceVersionLockFixture {
    std::string repository_name;
    std::string repository_package_name;
    std::string installed_repository_version;
    std::string repository_candidate_version;
    std::string installed_consumer_name;
    std::string installed_consumer_version;
    std::string installed_requirement;
    std::string replacement_version;
    std::string replacement_requirement;
};

CrossSourceVersionLockFixture virtualbox_version_lock_fixture() {
    return CrossSourceVersionLockFixture{
        "extra",
        "virtualbox",
        "7.2.14-1",
        "7.2.16-1",
        "virtualbox-ext-oracle",
        "7.2.14-1",
        "virtualbox=7.2.14",
        "7.2.16-1",
        "virtualbox=7.2.16"};
}

ConsumerDependencyRequirement fixture_consumer_requirement(
    const std::string& specification) {
    const DependencyRequirementParseResult parsed =
        parse_dependency_requirement(specification);
    if(parsed.failure() != nullptr || parsed.requirement() == nullptr) {
        throw std::logic_error(
            "Version-lock fixture dependency could not be parsed: " +
            specification);
    }
    const auto* requirement =
        std::get_if<ConsumerDependencyRequirement>(parsed.requirement());
    if(requirement == nullptr) {
        throw std::logic_error(
            "Version-lock fixture dependency is not a package requirement: " +
            specification);
    }
    return *requirement;
}

AurPackageConstraintMetadata make_fixture_aur_replacement(
    const CrossSourceVersionLockFixture& fixture,
    const std::string& version,
    const std::string& requirement) {
    return AurPackageConstraintMetadata{
        fixture.installed_consumer_name,
        fixture.installed_consumer_name,
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage, version),
        {fixture_consumer_requirement(requirement)},
        {},
        {},
        {},
        {}};
}

CrossSourceVersionLockAssessment make_possible_version_lock_assessment(
    CrossSourceVersionLockStatus status,
    const CrossSourceVersionLockFixture& fixture) {
    std::optional<ConsumerDependencyRequirement> replacement_requirement;
    std::optional<ConstraintEvaluation> replacement_evaluation;
    AurReplacementCandidateQueryResult aur_replacement =
        AurReplacementCandidateMetadataUnavailable{
            fixture.installed_consumer_name,
            fixture.installed_consumer_name,
            ObservedVersionUnknownReason::PartialSourceFailure};

    switch(status) {
        case CrossSourceVersionLockStatus::CompatibleReplacement:
            replacement_requirement = fixture_consumer_requirement(
                fixture.replacement_requirement);
            replacement_evaluation = ConstraintEvaluation::satisfied();
            aur_replacement = AurReplacementCandidateQuerySuccess{{make_fixture_aur_replacement(
                fixture,
                fixture.replacement_version,
                fixture.replacement_requirement)}};
            break;
        case CrossSourceVersionLockStatus::IncompatibleReplacement: {
            const std::string incompatible_requirement =
                fixture.repository_package_name + "=7.2.15";
            replacement_requirement = fixture_consumer_requirement(
                incompatible_requirement);
            replacement_evaluation = ConstraintEvaluation::unsatisfied();
            aur_replacement = AurReplacementCandidateQuerySuccess{{make_fixture_aur_replacement(
                fixture,
                fixture.replacement_version,
                incompatible_requirement)}};
            break;
        }
        case CrossSourceVersionLockStatus::MissingReplacement:
            aur_replacement = AurReplacementCandidateNotFound{
                fixture.installed_consumer_name};
            break;
        case CrossSourceVersionLockStatus::Unknown:
            break;
        case CrossSourceVersionLockStatus::QueryFailure:
            aur_replacement = AurReplacementCandidateQueryFailure{
                {fixture.installed_consumer_name},
                "fixture replacement query failure"};
            break;
        case CrossSourceVersionLockStatus::Ambiguous:
            aur_replacement = AurReplacementCandidateQuerySuccess{{make_fixture_aur_replacement(
                                                                       fixture,
                                                                       fixture.replacement_version,
                                                                       fixture.replacement_requirement),
                                                                   make_fixture_aur_replacement(
                                                                       fixture,
                                                                       fixture.replacement_version + ".ambiguous",
                                                                       fixture.replacement_requirement)}};
            break;
    }

    const ConsumerDependencyRequirement installed_requirement =
        fixture_consumer_requirement(fixture.installed_requirement);
    return CrossSourceVersionLockAssessment{
        status,
        CrossSourceVersionLockCandidateEvidence{
            RepositoryUpgradeCandidate{
                InstalledExactPackage{
                    fixture.repository_package_name,
                    ObservedVersion::available(
                        ObservedVersionSource::
                            InstalledExactPackage,
                        fixture.installed_repository_version)},
                RepositoryPackagePresent{
                    fixture.repository_name,
                    0,
                    fixture.repository_package_name,
                    fixture.repository_package_name,
                    ObservedVersion::available(
                        ObservedVersionSource::
                            RepositoryExactPackage,
                        fixture.repository_candidate_version),
                    std::vector<std::string>{
                        fixture.repository_name},
                    {}}},
            InstalledCrossSourceVersionLockConsumer{
                PackageRelationObservedPackage{
                    fixture.installed_consumer_name,
                    fixture.installed_consumer_name,
                    ObservedVersion::available(
                        ObservedVersionSource::
                            InstalledExactPackage,
                        fixture.installed_consumer_version),
                    {},
                    PackageRelationInstalledDatabaseIdentity{
                        "/fixture/root",
                        "/fixture/database"},
                    PackageRelationObservationRole::Installed,
                    {}},
                installed_requirement},
            std::move(aur_replacement)},
        ConstraintEvaluation::satisfied(),
        ConstraintEvaluation::unsatisfied(),
        std::move(replacement_requirement),
        std::move(replacement_evaluation)};
}

UpgradeAllOperationResult make_no_possible_version_lock_result(
    CrossSourceVersionLockObservationStatus observation_status) {
    UpgradeAllOperationResult result = make_system_failure_result();
    UpgradeAllCrossSourceVersionLockCorrelationResult correlation;
    correlation.observation = CrossSourceVersionLockObservationResult{
        observation_status, {}, {}};
    result.cross_source_version_lock_correlation = std::move(correlation);
    return result;
}

UpgradeAllOperationResult make_secondary_correlation_failure_result() {
    UpgradeAllOperationResult result = make_system_failure_result();
    UpgradeAllCrossSourceVersionLockCorrelationResult correlation;
    correlation.failure = UpgradeAllCrossSourceVersionLockCorrelationFailure{
        UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
            ResourceExhaustion,
        std::nullopt};
    result.cross_source_version_lock_correlation = std::move(correlation);
    return result;
}

UpgradeAllOperationResult make_possible_version_lock_result(
    CrossSourceVersionLockStatus status,
    CrossSourceVersionLockObservationStatus observation_status =
        CrossSourceVersionLockObservationStatus::Complete) {
    UpgradeAllOperationResult result = make_system_failure_result();
    CrossSourceVersionLockAssessment assessment =
        make_possible_version_lock_assessment(
            status, virtualbox_version_lock_fixture());
    UpgradeAllCrossSourceVersionLockCorrelationResult correlation;
    correlation.observation = CrossSourceVersionLockObservationResult{
        observation_status, {assessment.evidence}, {}};
    correlation.assessments.push_back(std::move(assessment));
    correlation.possible_blocker_assessment_indices.push_back(0);
    result.cross_source_version_lock_correlation = std::move(correlation);
    return result;
}

UpgradeAllOperationResult make_multiple_possible_version_lock_result() {
    UpgradeAllOperationResult result = make_system_failure_result();
    CrossSourceVersionLockFixture second_fixture{
        "core",
        "example-api",
        "1.4.0-1",
        "1.5.0-1",
        "example-addon",
        "1.4.0-2",
        "example-api=1.4.0",
        "1.5.0-1",
        "example-api=1.5.0"};
    CrossSourceVersionLockAssessment first =
        make_possible_version_lock_assessment(
            CrossSourceVersionLockStatus::CompatibleReplacement,
            virtualbox_version_lock_fixture());
    CrossSourceVersionLockAssessment second =
        make_possible_version_lock_assessment(
            CrossSourceVersionLockStatus::CompatibleReplacement,
            second_fixture);
    UpgradeAllCrossSourceVersionLockCorrelationResult correlation;
    correlation.observation = CrossSourceVersionLockObservationResult{
        CrossSourceVersionLockObservationStatus::Complete,
        {first.evidence, second.evidence},
        {}};
    correlation.assessments.push_back(std::move(first));
    correlation.assessments.push_back(std::move(second));
    correlation.possible_blocker_assessment_indices = {0, 1};
    result.cross_source_version_lock_correlation = std::move(correlation);
    return result;
}

UpgradeAllOperationResult make_invalid_version_lock_index_result() {
    UpgradeAllOperationResult result = make_possible_version_lock_result(
        CrossSourceVersionLockStatus::CompatibleReplacement);
    result.cross_source_version_lock_correlation
        ->possible_blocker_assessment_indices = {1};
    return result;
}

UpgradeAllOperationResult make_source_failure_result(
    bool cleanup_failure,
    bool no_change_cleanup_failure = false) {
    UpgradeAllOperationResult result;
    result.status = cleanup_failure
                        ? UpgradeAllOperationStatus::StoppedAfterSourceCleanupFailure
                        : UpgradeAllOperationStatus::StoppedOnSourceFailure;
    result.stopped_phase = UpgradeAllOperationPhase::RegisteredSource;
    result.system_source.status = cleanup_failure
                                      ? SystemSourceUpgradeStatus::StoppedAfterSourceCleanupFailure
                                      : SystemSourceUpgradeStatus::StoppedOnSourceFailure;
    result.system_source.stopped_phase =
        SystemSourceUpgradePhase::RegisteredSource;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change =
        no_change_cleanup_failure ? PackageStateChange::NoChange
                                  : PackageStateChange::Changed;
    add_source(
        result,
        cleanup_failure
            ? make_source_result(
                  0,
                  no_change_cleanup_failure
                      ? "source-no-change-cleanup-failed"
                      : "source-cleanup-failed",
                  no_change_cleanup_failure
                      ? RegisteredSourceUpgradeStatus::
                            NoChangeCleanupFailed
                      : RegisteredSourceUpgradeStatus::
                            UpdatedCleanupFailed,
                  RegisteredSourceUpgradeFailureKind::
                      CleanupFailedAfterPackageTransaction,
                  no_change_cleanup_failure
                      ? PackageStateChange::NoChange
                      : PackageStateChange::Changed,
                  std::nullopt,
                  "fixture source cleanup failed")
            : make_source_result(
                  0,
                  "source-failed",
                  RegisteredSourceUpgradeStatus::Failed,
                  RegisteredSourceUpgradeFailureKind::
                      BuildOrInstallFailed,
                  PackageStateChange::Unknown,
                  "fixture source build or install failed"));
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason = cleanup_failure
                                                        ? UpgradeAllNotAttemptedReason::SourceCleanupFailure
                                                        : UpgradeAllNotAttemptedReason::SourceFailure;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason = cleanup_failure
                                          ? UpgradeAllNotAttemptedReason::SourceCleanupFailure
                                          : UpgradeAllNotAttemptedReason::SourceFailure;
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::RegisteredSource,
        cleanup_failure ? "fixture source cleanup failed"
                        : "fixture source build or install failed");
    return result;
}

AurUpdateExecutionIssue make_preflight_issue() {
    AurUpdateExecutionIssue issue;
    issue.reason =
        AurUpdateExecutionReason::SplitPackageSelectionRequired;
    issue.package_name = "aur-preflight-blocked";
    issue.package_base = "aur-preflight-base";
    issue.diagnostic = "fixture AUR preflight blocker";
    return issue;
}

AurUpdatePreparationIssue make_preparation_issue() {
    AurUpdatePreparationIssue issue;
    issue.reason = AurUpdatePreparationReason::SourcePreferenceUnavailable;
    issue.affected_update_plan_indices = {1};
    issue.package_name = "aur-preparation-blocked";
    issue.package_base = "aur-preparation-base";
    issue.diagnostic = "fixture AUR preparation blocker";
    return issue;
}

UpgradeAllOperationResult make_stopped_before_aur_result() {
    UpgradeAllOperationResult result = make_base_result(
        UpgradeAllOperationStatus::StoppedBeforeAurExecution,
        PackageStateChange::NoChange,
        UpgradeAllAurPhaseStatus::BlockedBeforeExecution,
        AurUpdateOperationStatus::BlockedBeforeExecution);
    result.stopped_phase = UpgradeAllOperationPhase::AurPreparation;
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;

    AurUpdateOperationTargetResult unsupported = make_aur_target(
        "aur-preflight-blocked",
        AurUpdateOperationTargetStatus::Unsupported,
        "aur-preflight-base");
    unsupported.update_plan_index = 0;
    unsupported.preflight_issues.push_back(make_preflight_issue());
    filtered.reduced_operation_result.targets.push_back(
        std::move(unsupported));

    AurUpdateOperationTargetResult incomplete = make_aur_target(
        "aur-preparation-blocked",
        AurUpdateOperationTargetStatus::Incomplete,
        "aur-preparation-base");
    incomplete.update_plan_index = 1;
    incomplete.preparation_issues.push_back(make_preparation_issue());
    filtered.reduced_operation_result.targets.push_back(
        std::move(incomplete));
    filtered.reduced_operation_result.preparation_issues.push_back(
        make_preparation_issue());

    result.issues.push_back(make_aggregate_issue(
        UpgradeAllOperationIssueKind::FilteredAurPreparationFailed,
        UpgradeAllOperationPhase::AurPreparation,
        "fixture filtered AUR preparation failed"));
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::AurPreparation,
        "fixture filtered AUR preparation failed");
    return result;
}

UpgradeAllOperationResult make_aur_skip_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;

    AurUpdateOperationTargetResult up_to_date = make_aur_target(
        "aur-up-to-date", AurUpdateOperationTargetStatus::Skipped);
    up_to_date.update.classification = AurUpdateClassification::UpToDate;
    up_to_date.update.aur_package->version_relation =
        AurVersionRelation::SameAsInstalled;
    up_to_date.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    AurUpdateExecutionIssue up_to_date_issue;
    up_to_date_issue.reason = AurUpdateExecutionReason::UpToDate;
    up_to_date_issue.package_name = "aur-up-to-date";
    up_to_date.preflight_issues.push_back(std::move(up_to_date_issue));
    filtered.reduced_operation_result.targets.push_back(
        std::move(up_to_date));

    AurUpdateOperationTargetResult non_aur = make_aur_target(
        "non-aur-foreign", AurUpdateOperationTargetStatus::Skipped);
    non_aur.update.classification =
        AurUpdateClassification::NonAurForeign;
    non_aur.update.aur_package.reset();
    non_aur.package_base.reset();
    non_aur.skip_kind =
        AurUpdateExecutionSkipKind::NonAurForeign;
    AurUpdateExecutionIssue non_aur_issue;
    non_aur_issue.reason = AurUpdateExecutionReason::NonAurForeign;
    non_aur_issue.package_name = "non-aur-foreign";
    non_aur.preflight_issues.push_back(std::move(non_aur_issue));
    filtered.reduced_operation_result.targets.push_back(std::move(non_aur));
    return result;
}

UpgradeAllOperationResult make_many_current_one_attention_result() {
    UpgradeAllOperationResult result = make_aur_skip_result();
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    for(std::size_t index = 0; index < 48; ++index) {
        AurUpdateOperationTargetResult current = make_aur_target(
            "current-package-" + std::to_string(index),
            AurUpdateOperationTargetStatus::Skipped);
        current.update.classification = AurUpdateClassification::UpToDate;
        current.update.aur_package->version_relation =
            AurVersionRelation::SameAsInstalled;
        filtered.reduced_operation_result.targets.push_back(
            std::move(current));
    }

    AurUpdateOperationTargetResult attention = make_aur_target(
        "attention-package",
        AurUpdateOperationTargetStatus::Unsupported,
        "attention-suite");
    attention.update.classification =
        AurUpdateClassification::MetadataUnavailable;
    filtered.reduced_operation_result.targets.push_back(
        std::move(attention));
    return result;
}

AurUpdateWorkItemExecutionResult make_work_item_result(
    std::size_t index,
    const std::string& package_name,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic = std::nullopt) {
    AurUpdateWorkItemExecutionResult work_item;
    work_item.work_item_index = index;
    work_item.build_plan_order_index = index;
    work_item.package_name = package_name;
    work_item.package_base = package_name;
    work_item.plan_package_names = {package_name};
    work_item.affected_update_plan_indices = {index};
    work_item.affected_roots = {{index, package_name}};
    work_item.status = status;
    work_item.failure_kind = failure_kind;
    work_item.diagnostic = std::move(diagnostic);

    AurUpdateChildExecutionResult child;
    child.work_item_index = index;
    child.build_plan_order_index = index;
    child.required_child_index = 0;
    child.package_base = package_name;
    child.required_package_name = package_name;
    child.desired_install_reason = DesiredInstallReason::Explicit;
    child.affected_update_plan_indices = {index};
    child.affected_roots = {{index, package_name}};
    child.roles = {PackageRole::Root};
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::Updated:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::Installed;
            break;
        case AurUpdateWorkItemExecutionStatus::NoChange:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::SkippedAsNeeded;
            break;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status =
                AurUpdateChildExecutionStatus::InstalledCleanupFailed;
            break;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::
                SkippedAsNeededCleanupFailed;
            break;
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            child.status = AurUpdateChildExecutionStatus::NotAttempted;
            break;
    }
    work_item.child_results.push_back(std::move(child));
    return work_item;
}

AurUpdateChildExecutionResult make_child_result(
    std::size_t work_item_index,
    std::size_t required_child_index,
    const std::string& package_base,
    std::string package_name,
    DesiredInstallReason desired_reason,
    AurUpdateChildExecutionStatus status,
    std::size_t update_plan_index,
    std::string full_version) {
    AurUpdateChildExecutionResult child;
    child.work_item_index = work_item_index;
    child.build_plan_order_index = work_item_index;
    child.required_child_index = required_child_index;
    child.package_base = package_base;
    child.required_package_name = std::move(package_name);
    child.desired_install_reason = desired_reason;
    child.affected_update_plan_indices = {update_plan_index};
    child.affected_roots = {{update_plan_index, child.required_package_name}};
    child.roles = {desired_reason == DesiredInstallReason::Explicit
                       ? PackageRole::Root
                       : PackageRole::RuntimeDependency};
    child.status = status;
    if(status != AurUpdateChildExecutionStatus::NotAttempted) {
        child.selected_artifact = ArtifactPackageIdentity{
            child.required_package_name, std::move(full_version)};
    }
    return child;
}

AurUpdateWorkItemExecutionResult make_multi_child_work_item(
    std::size_t work_item_index,
    std::string package_base,
    std::vector<AurUpdateChildExecutionResult> children,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic = std::nullopt) {
    AurUpdateWorkItemExecutionResult work_item;
    work_item.work_item_index = work_item_index;
    work_item.build_plan_order_index = work_item_index;
    work_item.package_base = std::move(package_base);
    work_item.status = status;
    work_item.failure_kind = failure_kind;
    work_item.diagnostic = std::move(diagnostic);
    for(const AurUpdateChildExecutionResult& child : children) {
        work_item.plan_package_names.push_back(child.required_package_name);
        work_item.affected_update_plan_indices.insert(
            work_item.affected_update_plan_indices.end(),
            child.affected_update_plan_indices.begin(),
            child.affected_update_plan_indices.end());
        work_item.affected_roots.insert(
            work_item.affected_roots.end(),
            child.affected_roots.begin(), child.affected_roots.end());
    }
    if(children.size() == 1) {
        work_item.package_name = children.front().required_package_name;
    }
    work_item.child_results = std::move(children);
    return work_item;
}

AurUpdateOperationExecutionContribution make_child_contribution(
    const AurUpdateChildExecutionResult& child,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind) {
    AurUpdateOperationExecutionContribution contribution;
    contribution.work_item_index = child.work_item_index;
    contribution.required_child_index = child.required_child_index;
    contribution.package_name = child.required_package_name;
    contribution.package_base = child.package_base;
    contribution.selected_artifact = child.selected_artifact;
    contribution.desired_install_reason = child.desired_install_reason;
    contribution.affected_roots = child.affected_roots;
    contribution.roles = child.roles;
    contribution.status = status;
    contribution.failure_kind = failure_kind;
    return contribution;
}

UpgradeAllOperationResult make_aur_failure_result(bool cleanup_failure) {
    const AurUpdateOperationStatus nested_status = cleanup_failure
                                                       ? AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure
                                                       : AurUpdateOperationStatus::StoppedOnWorkItemFailure;
    UpgradeAllOperationResult result = make_base_result(
        cleanup_failure
            ? UpgradeAllOperationStatus::
                  StoppedAfterAurCleanupFailure
            : UpgradeAllOperationStatus::StoppedOnAurFailure,
        PackageStateChange::Changed,
        cleanup_failure
            ? UpgradeAllAurPhaseStatus::StoppedAfterCleanupFailure
            : UpgradeAllAurPhaseStatus::StoppedOnWorkItemFailure,
        nested_status);
    result.stopped_phase = UpgradeAllOperationPhase::AurExecution;

    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    filtered.reduced_operation_result.execution_status = cleanup_failure
                                                             ? AurUpdateInvocationExecutionStatus::
                                                                   StoppedAfterPackageCleanupFailure
                                                             : AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure;

    AurUpdateOperationTargetResult first = make_aur_target(
        "aur-first-updated",
        AurUpdateOperationTargetStatus::Updated);
    first.update_plan_index = 0;
    filtered.reduced_operation_result.targets.push_back(std::move(first));

    AurUpdateOperationTargetResult failed = make_aur_target(
        cleanup_failure ? "aur-cleanup-failed" : "aur-failed",
        cleanup_failure
            ? AurUpdateOperationTargetStatus::UpdatedCleanupFailed
            : AurUpdateOperationTargetStatus::Failed);
    failed.update_plan_index = 1;
    failed.execution_work_item_index = 1;
    failed.execution_failure_kind = cleanup_failure
                                        ? AurUpdateWorkItemFailureKind::
                                              CleanupFailedAfterPackageTransaction
                                        : AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
    failed.execution_diagnostic = cleanup_failure
                                      ? "fixture AUR cleanup failed"
                                      : "/private/workspace/upgrade-all-secret/transaction failed";
    filtered.reduced_operation_result.targets.push_back(std::move(failed));

    AurUpdateOperationTargetResult later = make_aur_target(
        "aur-later",
        AurUpdateOperationTargetStatus::NotAttempted);
    later.update_plan_index = 2;
    later.execution_failure_kind =
        AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    filtered.reduced_operation_result.targets.push_back(std::move(later));

    filtered.reduced_operation_result.execution_work_items.push_back(
        make_work_item_result(
            0,
            "aur-first-updated",
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None));
    filtered.reduced_operation_result.execution_work_items.push_back(
        make_work_item_result(
            1,
            cleanup_failure ? "aur-cleanup-failed" : "aur-failed",
            cleanup_failure
                ? AurUpdateWorkItemExecutionStatus::
                      UpdatedCleanupFailed
                : AurUpdateWorkItemExecutionStatus::Failed,
            cleanup_failure
                ? AurUpdateWorkItemFailureKind::
                      CleanupFailedAfterPackageTransaction
                : AurUpdateWorkItemFailureKind::
                      BuildOrInstallFailed,
            cleanup_failure
                ? "fixture AUR cleanup failed"
                : "/private/workspace/upgrade-all-secret/transaction failed"));
    filtered.reduced_operation_result.execution_work_items.push_back(
        make_work_item_result(
            2,
            "aur-later",
            AurUpdateWorkItemExecutionStatus::NotAttempted,
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped));
    if(!cleanup_failure) {
        AurUpdatePackageTransactionFailureSnapshot transaction;
        transaction.category =
            AurUpdatePackageTransactionFailureCategory::CommandFailed;
        transaction.attempted_artifacts = {
            {{"aur-failed", "4.2.0-1"},
             DesiredInstallReason::Explicit}};
        transaction.exit_code = 86;
        transaction.diagnostic =
            "/private/artifacts/aur-failed-4.2.0-1.pkg.tar.zst";
        AurUpdateWorkItemExecutionResult& failed_work_item =
            filtered.reduced_operation_result.execution_work_items[1];
        failed_work_item.transaction_failure = transaction;
        failed_work_item.failure_detail = transaction;
        AurUpdateOperationTargetResult& failed_target =
            filtered.reduced_operation_result.targets[1];
        failed_target.execution_failure_detail = transaction;
    }
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::AurExecution,
        cleanup_failure ? "fixture AUR cleanup failed"
                        : "/private/workspace/upgrade-all-secret/transaction failed");
    return result;
}

UpgradeAllOperationResult make_aur_no_change_cleanup_failure_result() {
    UpgradeAllOperationResult result = make_aur_failure_result(true);
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    AurUpdateOperationTargetResult& failed =
        filtered.reduced_operation_result.targets[1];
    failed.update.installed_name = "aur-no-change-cleanup-failed";
    failed.update.aur_package->aur_name = "aur-no-change-cleanup-failed";
    failed.update.aur_package->package_base =
        "aur-no-change-cleanup-failed";
    failed.package_base = "aur-no-change-cleanup-failed";
    failed.status =
        AurUpdateOperationTargetStatus::NoChangeCleanupFailed;
    AurUpdateWorkItemExecutionResult& work_item =
        filtered.reduced_operation_result.execution_work_items[1];
    work_item.package_name = "aur-no-change-cleanup-failed";
    work_item.package_base = "aur-no-change-cleanup-failed";
    work_item.plan_package_names = {"aur-no-change-cleanup-failed"};
    work_item.affected_roots = {{1, "aur-no-change-cleanup-failed"}};
    work_item.status =
        AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed;
    AurUpdateChildExecutionResult& child = work_item.child_results.front();
    child.package_base = "aur-no-change-cleanup-failed";
    child.required_package_name = "aur-no-change-cleanup-failed";
    child.affected_roots = {{1, "aur-no-change-cleanup-failed"}};
    child.selected_artifact = ArtifactPackageIdentity{
        "aur-no-change-cleanup-failed", "2.0-1"};
    child.status =
        AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed;
    return result;
}

UpgradeAllOperationResult make_aur_split_multiple_result() {
    UpgradeAllOperationResult result = make_completed_changed_result();
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;

    AurUpdateOperationTargetResult target = make_aur_target(
        "aur-split-main",
        AurUpdateOperationTargetStatus::Updated,
        "aur-split-suite");
    target.update_plan_index = 2;

    AurUpdateWorkItemExecutionResult work_item = make_multi_child_work_item(
        2,
        "aur-split-suite",
        {make_child_result(
             2,
             0,
             "aur-split-suite",
             "aur-split-main",
             DesiredInstallReason::Explicit,
             AurUpdateChildExecutionStatus::Installed,
             2,
             "7.0.1-2"),
         make_child_result(
             2,
             1,
             "aur-split-suite",
             "aur-split-dependency",
             DesiredInstallReason::Dependency,
             AurUpdateChildExecutionStatus::SkippedAsNeeded,
             2,
             "7.0.1-2")},
        AurUpdateWorkItemExecutionStatus::Updated,
        AurUpdateWorkItemFailureKind::None);
    work_item.unselected_artifacts = {
        {"aur-split-sibling", "7.0.1-2"},
        {"aur-split-suite-debug", "7.0.1-2"}};
    target.execution_contributions.push_back(make_child_contribution(
        work_item.child_results[0],
        AurUpdateWorkItemExecutionStatus::Updated,
        AurUpdateWorkItemFailureKind::None));
    target.execution_contributions.push_back(make_child_contribution(
        work_item.child_results[1],
        AurUpdateWorkItemExecutionStatus::NoChange,
        AurUpdateWorkItemFailureKind::None));
    filtered.reduced_operation_result.targets.push_back(std::move(target));
    filtered.reduced_operation_result.execution_work_items.push_back(
        std::move(work_item));
    return result;
}

UpgradeAllOperationResult make_issue_449_split_up_to_date_result() {
    UpgradeAllOperationResult result = make_aur_split_multiple_result();
    std::vector<AurUpdateOperationTargetResult>& targets =
        result.aur.operation_result->reduced_operation_result.targets;
    targets.push_back(
        make_issue_449_split_up_to_date_target(targets.size()));
    return result;
}

UpgradeAllOperationResult
make_issue_455_system_changed_aur_up_to_date_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::Changed);
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        make_issue_449_split_up_to_date_target(0));
    return result;
}

UpgradeAllOperationResult make_issue_455_aur_changed_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        make_aur_target(
            "issue-455-aur-updated",
            AurUpdateOperationTargetStatus::Updated));
    AurUpdateWorkItemExecutionResult work_item = make_work_item_result(
        0,
        "issue-455-aur-updated",
        AurUpdateWorkItemExecutionStatus::Updated,
        AurUpdateWorkItemFailureKind::None,
        std::nullopt);
    work_item.production_outcome = localized_reviewed_source_outcome();
    result.aur.operation_result->reduced_operation_result.execution_work_items.push_back(std::move(work_item));
    return result;
}

UpgradeAllOperationResult make_registered_package_base_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::Changed);

    RegisteredSourceUpgradeResult distinct = make_source_result(
        0,
        "registered-child",
        RegisteredSourceUpgradeStatus::Updated,
        RegisteredSourceUpgradeFailureKind::None,
        PackageStateChange::Changed);
    distinct.resolved_package_base = "registered-suite";
    distinct.package_base_execution =
        RegisteredSourcePackageBaseExecutionSnapshot{
            "registered-suite",
            PackageBaseSourceBuildSelectedResult{
                ArtifactPackageIdentity{
                    "registered-child", "4.2-3"},
                DesiredInstallReason::Explicit,
                ArtifactInstallExecutionOutcome::Installed},
            {ArtifactPackageIdentity{
                 "registered-sibling", "4.2-3"},
             ArtifactPackageIdentity{
                 "registered-child-debug", "4.2-3"}}};
    add_source(result, std::move(distinct));

    RegisteredSourceUpgradeResult ordinary = make_source_result(
        1,
        "registered-ordinary",
        RegisteredSourceUpgradeStatus::Updated,
        RegisteredSourceUpgradeFailureKind::None,
        PackageStateChange::Changed);
    ordinary.resolved_package_base = "registered-ordinary";
    ordinary.package_base_execution =
        RegisteredSourcePackageBaseExecutionSnapshot{
            "registered-ordinary",
            PackageBaseSourceBuildSelectedResult{
                ArtifactPackageIdentity{
                    "registered-ordinary", "1.0-2"},
                DesiredInstallReason::Explicit,
                ArtifactInstallExecutionOutcome::Installed},
            {}};
    add_source(result, std::move(ordinary));
    return result;
}

UpgradeAllOperationResult make_aur_mixed_cleanup_failure_result() {
    UpgradeAllOperationResult result = make_base_result(
        UpgradeAllOperationStatus::StoppedAfterAurCleanupFailure,
        PackageStateChange::Changed,
        UpgradeAllAurPhaseStatus::StoppedAfterCleanupFailure,
        AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure);
    result.stopped_phase = UpgradeAllOperationPhase::AurExecution;
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    filtered.reduced_operation_result.execution_status =
        AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure;

    AurUpdateOperationTargetResult target = make_aur_target(
        "aur-cleanup-main",
        AurUpdateOperationTargetStatus::UpdatedCleanupFailed,
        "aur-cleanup-suite");
    target.update_plan_index = 0;
    target.execution_work_item_index = 0;
    target.execution_failure_kind = AurUpdateWorkItemFailureKind::
        CleanupFailedAfterPackageTransaction;
    target.execution_failure_detail =
        AurUpdateWorkItemFailureDetail{std::monostate{}};
    target.execution_diagnostic =
        "/private/workspace/upgrade-all-secret/cleanup failed";

    AurUpdateWorkItemExecutionResult cleanup = make_multi_child_work_item(
        0,
        "aur-cleanup-suite",
        {make_child_result(
             0,
             0,
             "aur-cleanup-suite",
             "aur-cleanup-main",
             DesiredInstallReason::Explicit,
             AurUpdateChildExecutionStatus::InstalledCleanupFailed,
             0,
             "8.3.0-5"),
         make_child_result(
             0,
             1,
             "aur-cleanup-suite",
             "aur-cleanup-dependency",
             DesiredInstallReason::Dependency,
             AurUpdateChildExecutionStatus::
                 SkippedAsNeededCleanupFailed,
             0,
             "8.3.0-5")},
        AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
        AurUpdateWorkItemFailureKind::
            CleanupFailedAfterPackageTransaction,
        "/private/workspace/upgrade-all-secret/cleanup failed");
    cleanup.unselected_artifacts = {
        {"aur-cleanup-suite-debug", "8.3.0-5"}};
    target.execution_contributions.push_back(make_child_contribution(
        cleanup.child_results[0],
        AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
        AurUpdateWorkItemFailureKind::
            CleanupFailedAfterPackageTransaction));
    target.execution_contributions.push_back(make_child_contribution(
        cleanup.child_results[1],
        AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed,
        AurUpdateWorkItemFailureKind::
            CleanupFailedAfterPackageTransaction));
    filtered.reduced_operation_result.targets.push_back(std::move(target));

    AurUpdateOperationTargetResult later = make_aur_target(
        "aur-cleanup-later",
        AurUpdateOperationTargetStatus::NotAttempted);
    later.update_plan_index = 1;
    later.execution_work_item_index = 1;
    later.execution_failure_kind =
        AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    filtered.reduced_operation_result.targets.push_back(std::move(later));
    filtered.reduced_operation_result.execution_work_items.push_back(
        std::move(cleanup));
    filtered.reduced_operation_result.execution_work_items.push_back(
        make_work_item_result(
            1,
            "aur-cleanup-later",
            AurUpdateWorkItemExecutionStatus::NotAttempted,
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped));
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::AurExecution,
        "/private/workspace/upgrade-all-secret/cleanup failed");
    return result;
}

UpgradeAllOperationResult make_foreign_inventory_failure_result() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::StoppedBeforeAurExecution;
    result.stopped_phase = UpgradeAllOperationPhase::ForeignInventory;
    result.system_source.status = SystemSourceUpgradeStatus::Completed;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::None;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change =
        PackageStateChange::NoChange;
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::Failed;
    result.foreign_inventory.failure = PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture foreign inventory read failed"};
    result.foreign_inventory.diagnostic =
        "fixture foreign inventory read failed";
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::ForeignInventoryFailure;
    result.issues.push_back(make_aggregate_issue(
        UpgradeAllOperationIssueKind::ForeignInventoryReadFailed,
        UpgradeAllOperationPhase::ForeignInventory,
        "fixture foreign inventory read failed"));
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::ForeignInventory,
        "fixture foreign inventory read failed");
    return result;
}

UpgradeAllOperationResult
make_completed_direct_foreign_inventory_failure_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.foreign_inventory.failure = PackageMetadataFailure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "fixture direct foreign inventory failure"};
    result.foreign_inventory.diagnostic =
        "fixture direct foreign inventory failure";
    return result;
}

UpgradeAllOperationResult make_inconsistent_result() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::InconsistentResult;
    result.stopped_phase = UpgradeAllOperationPhase::Reduction;
    result.system_source.status = SystemSourceUpgradeStatus::Completed;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change =
        PackageStateChange::Unknown;
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency;
    result.issues.push_back(make_aggregate_issue(
        UpgradeAllOperationIssueKind::
            DuplicateExclusionCorrelationInconsistent,
        UpgradeAllOperationPhase::Reduction,
        "fixture aggregate correlation inconsistency"));
    add_stopping_diagnostic(
        result,
        UpgradeAllOperationPhase::Reduction,
        "fixture aggregate correlation inconsistency");
    return result;
}

UpgradeAllOperationResult make_nested_system_unavailable_result() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::InconsistentResult;
    result.stopped_phase = UpgradeAllOperationPhase::System;
    result.system_source.status =
        SystemSourceUpgradeStatus::InconsistentResult;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::System;
    result.system_source.system.status = SystemUpgradePhaseStatus::Failed;
    result.system_source.system.package_state_change =
        PackageStateChange::Unknown;
    result.system_source.system.diagnostic =
        "The system result is unavailable because an unexpected exception occurred after the phase started.";
    add_source(
        result,
        make_source_result(
            0,
            "source-after-unavailable-system",
            RegisteredSourceUpgradeStatus::NotAttempted,
            RegisteredSourceUpgradeFailureKind::PriorPhaseStopped,
            PackageStateChange::NoChange));
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency;
    return result;
}

UpgradeAllOperationResult make_nested_source_preserved_result() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::InconsistentResult;
    result.stopped_phase = UpgradeAllOperationPhase::RegisteredSource;
    result.system_source.status =
        SystemSourceUpgradeStatus::InconsistentResult;
    result.system_source.stopped_phase =
        SystemSourceUpgradePhase::RegisteredSource;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change =
        PackageStateChange::NoChange;
    add_source(
        result,
        make_source_result(
            0,
            "source-recorded-before-exception",
            RegisteredSourceUpgradeStatus::Updated,
            RegisteredSourceUpgradeFailureKind::None,
            PackageStateChange::Changed));
    add_source(
        result,
        make_source_result(
            1,
            "source-unavailable-after-start",
            RegisteredSourceUpgradeStatus::Incomplete,
            RegisteredSourceUpgradeFailureKind::UnknownException,
            PackageStateChange::Unknown,
            "The registered source result is unavailable because an unexpected exception occurred after the phase started."));
    add_source(
        result,
        make_source_result(
            2,
            "source-not-started",
            RegisteredSourceUpgradeStatus::NotAttempted,
            RegisteredSourceUpgradeFailureKind::PriorPhaseStopped,
            PackageStateChange::NoChange));
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency;
    return result;
}

UpgradeAllOperationResult make_completed_query_failure_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.aur.operation_result->query_result.recoverable_failures.push_back(
        AurUpdateQueryFailure{
            {"query-broken", "query-also-broken"},
            "fixture AUR query timeout"});
    return result;
}

UpgradeAllOperationResult make_completed_planning_issue_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    UpgradeAllPlanningIssue planning_issue;
    planning_issue.kind =
        UpgradeAllPlanningIssueKind::ConflictingExplicitPackageBase;
    planning_issue.explicit_source_indexes = {0, 1};
    planning_issue.package_base = "planning-conflict-base";
    filtered.upgrade_all_plan.issues.push_back(std::move(planning_issue));

    FilteredAurUpdateOperationIssue mapping_issue;
    mapping_issue.kind =
        FilteredAurUpdateOperationIssueKind::TargetPlannerMappingInconsistent;
    mapping_issue.original_query_plan_index = 0;
    mapping_issue.package_name = "mapping-broken";
    mapping_issue.diagnostic = "fixture target mapping issue";
    filtered.issues.push_back(std::move(mapping_issue));
    return result;
}

UpgradeAllOperationResult make_completed_inconsistency_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.issues.push_back(make_aggregate_issue(
        UpgradeAllOperationIssueKind::
            ExternalSatisfactionCorrelationInconsistent,
        UpgradeAllOperationPhase::Reduction,
        "fixture completed aggregate inconsistency"));

    AurUpdateOperationReductionIssue reduction_issue;
    reduction_issue.reason =
        AurUpdateOperationReductionReason::UnknownExecutionUpdatePlanIndex;
    reduction_issue.stage = AurUpdateOperationReductionStage::Execution;
    reduction_issue.affected_update_plan_indices = {99};
    reduction_issue.diagnostic = "fixture AUR reduction issue";
    result.aur.operation_result->reduced_operation_result.reduction_issues.push_back(std::move(reduction_issue));
    return result;
}

UpgradeAllOperationResult make_completed_cleanup_failure_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    AurUpdateOperationTargetResult target = make_aur_target(
        "defensive-cleanup",
        AurUpdateOperationTargetStatus::NoChangeCleanupFailed);
    target.execution_work_item_index = 0;
    target.execution_failure_kind =
        AurUpdateWorkItemFailureKind::
            CleanupFailedAfterPackageTransaction;
    target.execution_diagnostic =
        "fixture completed cleanup failure";
    filtered.reduced_operation_result.targets.push_back(std::move(target));
    return result;
}

UpgradeAllOperationResult make_completed_not_attempted_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    AurUpdateOperationTargetResult target = make_aur_target(
        "defensive-not-attempted",
        AurUpdateOperationTargetStatus::NotAttempted);
    target.execution_failure_kind =
        AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        std::move(target));
    return result;
}

AurUpdateExecutionIssue make_matrix_preflight_issue(
    AurUpdateExecutionReason reason) {
    AurUpdateExecutionIssue issue;
    issue.reason = reason;
    issue.package_name = "matrix-target";
    issue.package_base = "matrix-base";
    issue.diagnostic = "matrix preflight diagnostic";
    return issue;
}

AurUpdatePreparationIssue make_matrix_preparation_issue(
    AurUpdatePreparationReason reason) {
    AurUpdatePreparationIssue issue;
    issue.reason = reason;
    issue.affected_update_plan_indices = {0};
    issue.package_name = "matrix-target";
    issue.package_base = "matrix-base";
    issue.diagnostic = "matrix preparation diagnostic";
    return issue;
}

UpgradeAllOperationResult make_aur_phase_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.aur.status = matrix_enum_value(
        UPGRADE_ALL_AUR_PHASE_STATUSES, index);
    if(result.aur.status == UpgradeAllAurPhaseStatus::NotAttempted) {
        result.aur.not_attempted_reason =
            UpgradeAllNotAttemptedReason::PreparationBlocked;
    }
    return result;
}

UpgradeAllOperationResult make_not_attempted_reason_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason = matrix_enum_value(
        UPGRADE_ALL_NOT_ATTEMPTED_REASONS, index);
    return result;
}

UpgradeAllOperationResult make_target_status_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    const AurUpdateOperationTargetStatus status = matrix_enum_value(
        AUR_TARGET_STATUSES, index);
    AurUpdateOperationTargetResult target = make_aur_target(
        "matrix-target", status, "matrix-base");
    switch(status) {
        case AurUpdateOperationTargetStatus::Updated:
        case AurUpdateOperationTargetStatus::NoChange:
        case AurUpdateOperationTargetStatus::Unsupported:
        case AurUpdateOperationTargetStatus::Incomplete:
            break;
        case AurUpdateOperationTargetStatus::Skipped:
            target.update.classification =
                AurUpdateClassification::UpToDate;
            target.update.aur_package->version_relation =
                AurVersionRelation::SameAsInstalled;
            target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
            target.preflight_issues.push_back(
                AurUpdateExecutionIssue{
                    AurUpdateExecutionReason::UpToDate,
                    "matrix-target",
                    std::nullopt,
                    std::nullopt,
                    "matrix target is already up to date"});
            break;
        case AurUpdateOperationTargetStatus::Failed:
            target.execution_failure_kind =
                AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
            target.execution_diagnostic = "matrix execution diagnostic";
            break;
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
            target.execution_failure_kind = AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction;
            target.execution_diagnostic = "matrix cleanup diagnostic";
            break;
        case AurUpdateOperationTargetStatus::NotAttempted:
            target.execution_failure_kind =
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
            break;
    }
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        std::move(target));
    return result;
}

UpgradeAllOperationResult make_operation_status_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    AurUpdateOperationResult& operation =
        result.aur.operation_result->reduced_operation_result;
    operation.status = matrix_enum_value(AUR_OPERATION_STATUSES, index);
    operation.targets.push_back(make_aur_target(
        "matrix-target",
        AurUpdateOperationTargetStatus::NotAttempted,
        "matrix-base"));
    return result;
}

UpgradeAllOperationResult make_preflight_reason_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    const AurUpdateExecutionReason reason = matrix_enum_value(
        AUR_PREFLIGHT_REASONS, index);
    const bool is_normal_skip =
        reason == AurUpdateExecutionReason::UpToDate ||
        reason == AurUpdateExecutionReason::NonAurForeign;
    AurUpdateOperationTargetResult target = make_aur_target(
        "matrix-target",
        is_normal_skip ? AurUpdateOperationTargetStatus::Skipped
                       : AurUpdateOperationTargetStatus::Unsupported,
        "matrix-base");
    target.preflight_issues.push_back(
        make_matrix_preflight_issue(reason));
    if(reason == AurUpdateExecutionReason::UpToDate) {
        target.update.classification =
            AurUpdateClassification::UpToDate;
        target.update.aur_package->version_relation =
            AurVersionRelation::SameAsInstalled;
        target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    } else if(reason == AurUpdateExecutionReason::NonAurForeign) {
        target.update.classification =
            AurUpdateClassification::NonAurForeign;
        target.update.aur_package.reset();
        target.package_base.reset();
        target.skip_kind =
            AurUpdateExecutionSkipKind::NonAurForeign;
    }
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        std::move(target));
    return result;
}

UpgradeAllOperationResult make_preparation_reason_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    const AurUpdatePreparationReason reason = matrix_enum_value(
        AUR_PREPARATION_REASONS, index);
    AurUpdatePreparationIssue issue =
        make_matrix_preparation_issue(reason);
    AurUpdateOperationTargetResult target = make_aur_target(
        "matrix-target",
        AurUpdateOperationTargetStatus::Incomplete,
        "matrix-base");
    target.preparation_issues.push_back(issue);
    AurUpdateOperationResult& operation =
        result.aur.operation_result->reduced_operation_result;
    operation.targets.push_back(std::move(target));
    operation.preparation_issues.push_back(std::move(issue));
    return result;
}

UpgradeAllOperationResult make_execution_failure_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    const AurUpdateWorkItemFailureKind kind = matrix_enum_value(
        AUR_EXECUTION_FAILURE_KINDS, index);
    AurUpdateOperationTargetStatus status =
        AurUpdateOperationTargetStatus::Failed;
    if(kind == AurUpdateWorkItemFailureKind::
                   CleanupFailedAfterPackageTransaction) {
        status = AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
    } else if(kind == AurUpdateWorkItemFailureKind::PriorWorkItemStopped) {
        status = AurUpdateOperationTargetStatus::NotAttempted;
    }
    AurUpdateOperationTargetResult target = make_aur_target(
        "matrix-target", status, "matrix-base");
    target.execution_failure_kind = kind;
    target.execution_diagnostic = "matrix execution diagnostic";
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        std::move(target));
    return result;
}

AurUpdateOperationReductionIssue make_matrix_reduction_issue() {
    AurUpdateOperationReductionIssue issue;
    issue.stage = AurUpdateOperationReductionStage::Preflight;
    issue.reason = AurUpdateOperationReductionReason::
        DuplicatePreflightUpdatePlanIndex;
    issue.affected_update_plan_indices = {0};
    issue.diagnostic = "matrix reduction diagnostic";
    return issue;
}

UpgradeAllOperationResult make_reduction_stage_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    AurUpdateOperationReductionIssue issue = make_matrix_reduction_issue();
    issue.stage = matrix_enum_value(AUR_REDUCTION_STAGES, index);
    result.aur.operation_result->reduced_operation_result.reduction_issues.push_back(std::move(issue));
    return result;
}

UpgradeAllOperationResult make_reduction_reason_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    AurUpdateOperationReductionIssue issue = make_matrix_reduction_issue();
    issue.reason = matrix_enum_value(AUR_REDUCTION_REASONS, index);
    result.aur.operation_result->reduced_operation_result.reduction_issues.push_back(std::move(issue));
    return result;
}

UpgradeAllOperationResult make_filtered_issue_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    FilteredAurUpdateOperationIssue issue;
    issue.kind = matrix_enum_value(FILTERED_AUR_ISSUE_KINDS, index);
    issue.package_name = "matrix-target";
    issue.package_base = "matrix-base";
    issue.diagnostic = "matrix mapping diagnostic";
    result.aur.operation_result->issues.push_back(std::move(issue));
    return result;
}

UpgradeAllOperationResult make_planning_issue_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    UpgradeAllPlanningIssue issue;
    issue.kind = matrix_enum_value(
        UPGRADE_ALL_PLANNING_ISSUE_KINDS, index);
    issue.package_name = "matrix-target";
    issue.package_base = "matrix-base";
    result.aur.operation_result->upgrade_all_plan.issues.push_back(
        std::move(issue));
    return result;
}

UpgradeAllOperationResult make_target_disposition_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    result.duplicate_excluded_aur_targets.push_back(
        make_duplicate_exclusion(
            0,
            "matrix-duplicate",
            "matrix-base",
            matrix_enum_value(
                UPGRADE_ALL_TARGET_DISPOSITIONS, index),
            "matrix-duplicate",
            "matrix-base",
            "repository:https://sources.example/matrix"));
    return result;
}

UpgradeAllOperationResult make_build_unit_role_matrix_result(
    std::size_t index) {
    UpgradeAllOperationResult result =
        make_external_satisfaction_result();
    result.externally_satisfied_aur_build_units.front().root_correlations.front().role = matrix_enum_value(
        UPGRADE_ALL_BUILD_UNIT_ROLES, index);
    return result;
}

UpgradeAllOperationResult make_preparation_warning_matrix_result() {
    UpgradeAllOperationResult result = make_success_result(
        UpgradeAllOperationStatus::Completed,
        PackageStateChange::NoChange);
    AurUpdatePreparationWarning warning;
    warning.preference_name = "matrix-preference";
    warning.entry_path = "/fixture/preferences/matrix-preference";
    warning.affected_update_plan_indices = {0};
    warning.affected_roots = {{0, "matrix-target"}};
    warning.diagnostic = "matrix preparation warning diagnostic";
    result.aur.operation_result->reduced_operation_result.preparation_warnings.push_back(std::move(warning));
    return result;
}

UpgradeAllOperationResult make_external_attribution_missing_matrix_result() {
    UpgradeAllOperationResult result =
        make_external_satisfaction_result();
    result.externally_satisfied_aur_build_units.front().operation_unit.external_satisfaction.source_identity_keys.clear();
    return result;
}

UpgradeAllOperationResult make_aur_presentation_matrix_result() {
    const std::string kind = current_matrix_kind();
    const std::size_t index = current_matrix_index();
    if(kind == "aur-phase-status") {
        return make_aur_phase_matrix_result(index);
    }
    if(kind == "not-attempted-reason") {
        return make_not_attempted_reason_matrix_result(index);
    }
    if(kind == "target-status") {
        return make_target_status_matrix_result(index);
    }
    if(kind == "operation-status") {
        return make_operation_status_matrix_result(index);
    }
    if(kind == "preflight-reason") {
        return make_preflight_reason_matrix_result(index);
    }
    if(kind == "preparation-reason") {
        return make_preparation_reason_matrix_result(index);
    }
    if(kind == "execution-failure-kind") {
        return make_execution_failure_matrix_result(index);
    }
    if(kind == "reduction-stage") {
        return make_reduction_stage_matrix_result(index);
    }
    if(kind == "reduction-reason") {
        return make_reduction_reason_matrix_result(index);
    }
    if(kind == "filtered-issue-kind") {
        return make_filtered_issue_matrix_result(index);
    }
    if(kind == "planning-issue-kind") {
        return make_planning_issue_matrix_result(index);
    }
    if(kind == "target-disposition") {
        return make_target_disposition_matrix_result(index);
    }
    if(kind == "build-unit-role") {
        return make_build_unit_role_matrix_result(index);
    }
    if(kind == "preparation-warning") {
        if(index != 0) {
            throw std::logic_error(
                "Preparation warning matrix index is out of range.");
        }
        return make_preparation_warning_matrix_result();
    }
    if(kind == "external-attribution-missing") {
        if(index != 0) {
            throw std::logic_error(
                "External attribution matrix index is out of range.");
        }
        return make_external_attribution_missing_matrix_result();
    }
    throw std::logic_error(
        "Unknown upgrade-all presentation matrix kind: " + kind);
}

UpgradeAllOperationResult result_for_scenario(const std::string& scenario) {
    if(scenario == "no-updates") {
        return make_success_result(
            UpgradeAllOperationStatus::NoUpdates,
            PackageStateChange::NoChange);
    }
    if(scenario == "issue-455-system-changed-aur-up-to-date") {
        return make_issue_455_system_changed_aur_up_to_date_result();
    }
    if(scenario == "issue-455-aur-changed") {
        return make_issue_455_aur_changed_result();
    }
    if(scenario == "completed-changed") {
        return make_completed_changed_result();
    }
    if(scenario == "aur-split-multiple") {
        return make_aur_split_multiple_result();
    }
    if(scenario == "issue-449-split-up-to-date") {
        return make_issue_449_split_up_to_date_result();
    }
    if(scenario == "registered-packagebase-result") {
        return make_registered_package_base_result();
    }
    if(scenario == "completed-no-change") {
        return make_success_result(
            UpgradeAllOperationStatus::Completed,
            PackageStateChange::NoChange);
    }
    if(scenario == "before-snapshot-unavailable") {
        return make_snapshot_unavailable_result(true);
    }
    if(scenario == "after-snapshot-unavailable") {
        return make_snapshot_unavailable_result(false);
    }
    if(scenario == "completed-unknown") {
        return make_success_result(
            UpgradeAllOperationStatus::Completed,
            PackageStateChange::Unknown);
    }
    if(scenario == "aur-skips") {
        return make_aur_skip_result();
    }
    if(scenario == "many-current-one-attention") {
        return make_many_current_one_attention_result();
    }
    if(scenario == "duplicate-exclusions") {
        return make_duplicate_exclusion_result();
    }
    if(scenario == "external-satisfaction") {
        return make_external_satisfaction_result();
    }
    if(scenario == "stopped-on-system-failure") {
        return make_system_failure_result();
    }
    if(scenario == "stopped-system-version-lock-complete-zero") {
        return make_no_possible_version_lock_result(
            CrossSourceVersionLockObservationStatus::Complete);
    }
    if(scenario == "stopped-system-version-lock-partial-zero") {
        return make_no_possible_version_lock_result(
            CrossSourceVersionLockObservationStatus::Partial);
    }
    if(scenario == "stopped-system-version-lock-failed-zero") {
        return make_no_possible_version_lock_result(
            CrossSourceVersionLockObservationStatus::Failed);
    }
    if(scenario == "stopped-system-version-lock-correlation-failure") {
        return make_secondary_correlation_failure_result();
    }
    if(scenario == "stopped-system-version-lock-compatible") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::CompatibleReplacement);
    }
    if(scenario == "stopped-system-version-lock-incompatible") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::IncompatibleReplacement);
    }
    if(scenario == "stopped-system-version-lock-missing") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::MissingReplacement);
    }
    if(scenario == "stopped-system-version-lock-unknown") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::Unknown);
    }
    if(scenario == "stopped-system-version-lock-query-failure") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::QueryFailure);
    }
    if(scenario == "stopped-system-version-lock-ambiguous") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::Ambiguous);
    }
    if(scenario == "stopped-system-version-lock-partial-compatible") {
        return make_possible_version_lock_result(
            CrossSourceVersionLockStatus::CompatibleReplacement,
            CrossSourceVersionLockObservationStatus::Partial);
    }
    if(scenario == "stopped-system-version-lock-multiple") {
        return make_multiple_possible_version_lock_result();
    }
    if(scenario == "stopped-system-version-lock-invalid-index") {
        return make_invalid_version_lock_index_result();
    }
    if(scenario == "stopped-on-source-failure") {
        return make_source_failure_result(false);
    }
    if(scenario == "stopped-after-source-cleanup-failure") {
        return make_source_failure_result(true);
    }
    if(scenario == "stopped-after-source-no-change-cleanup-failure") {
        return make_source_failure_result(true, true);
    }
    if(scenario == "stopped-before-aur-execution") {
        return make_stopped_before_aur_result();
    }
    if(scenario == "stopped-on-aur-failure") {
        return make_aur_failure_result(false);
    }
    if(scenario == "stopped-after-aur-cleanup-failure") {
        return make_aur_failure_result(true);
    }
    if(scenario == "stopped-after-aur-no-change-cleanup-failure") {
        return make_aur_no_change_cleanup_failure_result();
    }
    if(scenario == "stopped-after-aur-mixed-cleanup-failure") {
        return make_aur_mixed_cleanup_failure_result();
    }
    if(scenario == "foreign-inventory-failure") {
        return make_foreign_inventory_failure_result();
    }
    if(scenario == "completed-direct-foreign-inventory-failure") {
        return make_completed_direct_foreign_inventory_failure_result();
    }
    if(scenario == "inconsistent-result") {
        return make_inconsistent_result();
    }
    if(scenario == "nested-system-unavailable") {
        return make_nested_system_unavailable_result();
    }
    if(scenario == "nested-source-preserved") {
        return make_nested_source_preserved_result();
    }
    if(scenario == "completed-query-failure") {
        return make_completed_query_failure_result();
    }
    if(scenario == "completed-planning-issue") {
        return make_completed_planning_issue_result();
    }
    if(scenario == "completed-inconsistency") {
        return make_completed_inconsistency_result();
    }
    if(scenario == "completed-cleanup-failure") {
        return make_completed_cleanup_failure_result();
    }
    if(scenario == "completed-not-attempted") {
        return make_completed_not_attempted_result();
    }
    if(scenario == "aur-presentation-matrix") {
        return make_aur_presentation_matrix_result();
    }
    throw std::logic_error(
        "Unknown upgrade-all command test scenario: " + scenario);
}

} // namespace

struct PreparedUpgradeAllOperation::Impl {
    Impl(
        UpgradeAllOperationPreparedSnapshot prepared_snapshot,
        std::string prepared_scenario)
        : snapshot(std::move(prepared_snapshot)),
          scenario(std::move(prepared_scenario)) {
    }

    UpgradeAllOperationPreparedSnapshot snapshot;
    std::string scenario;
};

struct UpgradeAllOperationPreparationAccess {
    static PreparedUpgradeAllOperation make(
        UpgradeAllOperationPreparedSnapshot snapshot,
        std::string scenario) {
        return PreparedUpgradeAllOperation(
            std::make_unique<PreparedUpgradeAllOperation::Impl>(
                std::move(snapshot), std::move(scenario)));
    }
};

PreparedUpgradeAllOperation::PreparedUpgradeAllOperation(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

PreparedUpgradeAllOperation::PreparedUpgradeAllOperation(
    PreparedUpgradeAllOperation&&) noexcept = default;

PreparedUpgradeAllOperation::~PreparedUpgradeAllOperation() noexcept = default;

bool PreparedUpgradeAllOperation::is_valid() const noexcept {
    return impl_ != nullptr;
}

const UpgradeAllOperationPreparedSnapshot*
PreparedUpgradeAllOperation::snapshot() const noexcept {
    return impl_ == nullptr ? nullptr : &impl_->snapshot;
}

const UpgradeAllOperationProjectionAuthority*
PreparedUpgradeAllOperation::projection_authority() const noexcept {
    return nullptr;
}

PreparedUpgradeAllAurPreflight prepare_upgrade_all_aur_preflight(
    const UpgradeAllOperationPreparedSnapshot&,
    const AppConfig&) {
    throw std::logic_error(
        "Upgrade-all command fixture does not provide dry-run preflight authority.");
}

UpgradeAllOperationPreparation prepare_upgrade_all_operation(
    const AppConfig& config) {
    const std::string scenario = current_scenario();
    append_event("upgrade-all prepare " + config_snapshot(config));
    if(scenario == "prepare-exception") {
        throw std::runtime_error("fixture upgrade-all preparation exception");
    }
    if(scenario == "blocked-before-mutation") {
        return make_blocked_result();
    }

    return UpgradeAllOperationPreparationAccess::make({}, scenario);
}

UpgradeAllOperationResult execute_prepared_upgrade_all_operation(
    PreparedUpgradeAllOperation prepared,
    const AppConfig& config) {
    if(prepared.impl_ == nullptr) {
        throw std::logic_error(
            "Upgrade-all command passed an invalid prepared capability.");
    }

    std::unique_ptr<PreparedUpgradeAllOperation::Impl> state =
        std::move(prepared.impl_);
    append_event("upgrade-all execute " + config_snapshot(config));
    if(state->scenario == "execute-unknown-exception") {
        throw UnknownFixtureException{};
    }
    return result_for_scenario(state->scenario);
}
