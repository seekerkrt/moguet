#include "aur_update_operation_result.hpp"

#include "aur_update_required_devel_relation_projection.hpp"
#include "localization.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace {

constexpr char DEVEL_REQUIRES_CHECK_POLICY_SNAPSHOT_DIAGNOSTIC[] =
    "Devel RequiresCheck policy snapshots are missing, unknown, or inconsistent.";

void add_reduction_issue(
    AurUpdateOperationResult& result,
    AurUpdateOperationReductionReason reason,
    AurUpdateOperationReductionStage stage,
    std::string diagnostic,
    std::vector<std::size_t> affected_update_plan_indices = {},
    std::vector<std::size_t> preflight_target_positions = {},
    std::optional<std::size_t> execution_work_item_index = std::nullopt) {
    result.reduction_issues.push_back(AurUpdateOperationReductionIssue{
        reason,
        stage,
        std::move(affected_update_plan_indices),
        std::move(preflight_target_positions),
        execution_work_item_index,
        std::move(diagnostic)});
}

// Diagnostic literals stay at their construction sites for extraction while
// this typed reducer keeps the translation lookup out of every call's noise.
template <std::size_t Size>
void add_reduction_issue(
    AurUpdateOperationResult& result,
    AurUpdateOperationReductionReason reason,
    AurUpdateOperationReductionStage stage,
    const char (&diagnostic)[Size],
    std::vector<std::size_t> affected_update_plan_indices = {},
    std::vector<std::size_t> preflight_target_positions = {},
    std::optional<std::size_t> execution_work_item_index = std::nullopt) {
    add_reduction_issue(
        result, reason, stage,
        localization::translate_message(diagnostic),
        std::move(affected_update_plan_indices),
        std::move(preflight_target_positions), execution_work_item_index);
}

std::optional<std::string> package_base_for_target(
    const AurUpdateExecutionTarget& target) {
    if(!target.update.aur_package.has_value()) return std::nullopt;
    return target.update.aur_package->package_base;
}

bool same_remote_package(
    const std::optional<AurUpdateRemotePackage>& lhs,
    const std::optional<AurUpdateRemotePackage>& rhs) {
    if(lhs.has_value() != rhs.has_value()) return false;
    if(!lhs.has_value()) return true;
    return lhs->aur_name == rhs->aur_name &&
           lhs->package_base == rhs->package_base &&
           lhs->version == rhs->version &&
           lhs->version_relation == rhs->version_relation;
}

bool same_update_entry(
    const AurUpdatePlanEntry& lhs,
    const AurUpdatePlanEntry& rhs) {
    return lhs.installed_name == rhs.installed_name &&
           lhs.installed_version == rhs.installed_version &&
           lhs.install_reason == rhs.install_reason &&
           same_remote_package(lhs.aur_package, rhs.aur_package) &&
           lhs.classification == rhs.classification &&
           lhs.devel_classification == rhs.devel_classification &&
           lhs.devel_assessment_origin == rhs.devel_assessment_origin &&
           lhs.devel_assessment == rhs.devel_assessment;
}

bool same_preflight_issue(
    const AurUpdateExecutionIssue& lhs,
    const AurUpdateExecutionIssue& rhs) {
    return lhs.reason == rhs.reason &&
           lhs.package_name == rhs.package_name &&
           lhs.package_base == rhs.package_base &&
           lhs.dependency_specification == rhs.dependency_specification &&
           lhs.diagnostic == rhs.diagnostic &&
           lhs.devel_requires_check_reason ==
               rhs.devel_requires_check_reason &&
           lhs.build_plan_projection_issue ==
               rhs.build_plan_projection_issue &&
           lhs.relation_reason == rhs.relation_reason &&
           lhs.required_devel_target_blocker ==
               rhs.required_devel_target_blocker;
}

bool same_preflight_target_snapshot(
    const AurUpdateExecutionTarget& lhs,
    const AurUpdateExecutionTarget& rhs) {
    return lhs.update_plan_index == rhs.update_plan_index &&
           lhs.build_plan_root_index == rhs.build_plan_root_index &&
           same_update_entry(lhs.update, rhs.update) &&
           lhs.status == rhs.status &&
           lhs.desired_install_reason == rhs.desired_install_reason &&
           lhs.skip_kind == rhs.skip_kind &&
           lhs.issues.size() == rhs.issues.size() &&
           std::equal(
               lhs.issues.begin(), lhs.issues.end(),
               rhs.issues.begin(), same_preflight_issue);
}

void validate_preparation_target_snapshot(
    AurUpdateOperationResult& result,
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateSourceBuildPreparation& preparation,
    const std::vector<bool>& expected_target_positions) {
    std::vector<std::size_t> expected_positions;
    std::vector<std::size_t> affected_indices;
    for(std::size_t position = 0; position < expected_target_positions.size();
        ++position) {
        if(expected_target_positions[position]) {
            expected_positions.push_back(position);
        }
    }
    for(const auto& target : preparation.affected_update_targets) {
        affected_indices.push_back(target.update_plan_index);
    }

    bool is_consistent = preparation.affected_update_targets.size() ==
                         expected_positions.size();
    const std::size_t comparable_count = std::min(
        preparation.affected_update_targets.size(),
        expected_positions.size());
    for(std::size_t index = 0; index < comparable_count; ++index) {
        if(!same_preflight_target_snapshot(
               preparation.affected_update_targets[index],
               preflight.targets[expected_positions[index]])) {
            is_consistent = false;
        }
    }
    if(is_consistent) return;

    add_reduction_issue(
        result,
        AurUpdateOperationReductionReason::
            PreparationTargetSnapshotInconsistent,
        AurUpdateOperationReductionStage::Preparation,
        "Preparation target snapshot does not match the targets eligible in this preflight phase.",
        std::move(affected_indices), std::move(expected_positions));
}

bool is_known_preflight_status(
    AurUpdateExecutionTargetStatus status) noexcept {
    switch(status) {
        case AurUpdateExecutionTargetStatus::Executable:
        case AurUpdateExecutionTargetStatus::Skipped:
        case AurUpdateExecutionTargetStatus::Unsupported:
        case AurUpdateExecutionTargetStatus::Incomplete:
            return true;
    }
    return false;
}

bool is_known_preflight_reason(AurUpdateExecutionReason reason) noexcept {
    switch(reason) {
        case AurUpdateExecutionReason::None:
        case AurUpdateExecutionReason::UpToDate:
        case AurUpdateExecutionReason::DevelRequiresCheck:
        case AurUpdateExecutionReason::DevelObservationUnknown:
        case AurUpdateExecutionReason::DevelUnsupported:
        case AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck:
        case AurUpdateExecutionReason::NonAurForeign:
        case AurUpdateExecutionReason::AurMetadataUnavailable:
        case AurUpdateExecutionReason::VersionComparisonUnavailable:
        case AurUpdateExecutionReason::InstalledReasonUnknown:
        case AurUpdateExecutionReason::UpdatePlanInconsistent:
        case AurUpdateExecutionReason::DuplicateUpdateTarget:
        case AurUpdateExecutionReason::InstalledPackageMetadataUnavailable:
        case AurUpdateExecutionReason::RepositoryMetadataUnavailable:
        case AurUpdateExecutionReason::AurDependencyMetadataUnavailable:
        case AurUpdateExecutionReason::ProviderMetadataUnavailable:
        case AurUpdateExecutionReason::UnresolvedDependency:
        case AurUpdateExecutionReason::VersionConstraintUnverified:
        case AurUpdateExecutionReason::DependencyCycle:
        case AurUpdateExecutionReason::BuildPlanInconsistent:
        case AurUpdateExecutionReason::PackageBaseMismatch:
        case AurUpdateExecutionReason::SplitPackageSelectionRequired:
        case AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase:
        case AurUpdateExecutionReason::AmbiguousProvider:
        case AurUpdateExecutionReason::ConflictsOrReplacesUnresolved:
            return true;
    }
    return false;
}

bool is_known_installed_package_reason(
    InstalledPackageReason reason) noexcept {
    switch(reason) {
        case InstalledPackageReason::Explicit:
        case InstalledPackageReason::Dependency:
        case InstalledPackageReason::Unknown:
            return true;
    }
    return false;
}

bool is_known_version_relation(AurVersionRelation relation) noexcept {
    switch(relation) {
        case AurVersionRelation::OlderThanInstalled:
        case AurVersionRelation::SameAsInstalled:
        case AurVersionRelation::NewerThanInstalled:
        case AurVersionRelation::Unavailable:
            return true;
    }
    return false;
}

bool is_known_update_classification(
    AurUpdateClassification classification) noexcept {
    switch(classification) {
        case AurUpdateClassification::UpdateAvailable:
        case AurUpdateClassification::UpToDate:
        case AurUpdateClassification::NonAurForeign:
        case AurUpdateClassification::MetadataUnavailable:
        case AurUpdateClassification::VersionComparisonUnavailable:
            return true;
    }
    return false;
}

bool is_known_desired_install_reason(DesiredInstallReason reason) noexcept {
    switch(reason) {
        case DesiredInstallReason::Explicit:
        case DesiredInstallReason::Dependency:
            return true;
    }
    return false;
}

bool has_valid_skipped_preflight_issues(
    const AurUpdateExecutionTarget& target,
    DevelRequiresCheckPolicy policy) noexcept {
    return is_valid_aur_update_execution_target_skip_snapshot(
        target, policy);
}

bool is_known_preparation_reason(AurUpdatePreparationReason reason) noexcept {
    switch(reason) {
        case AurUpdatePreparationReason::None:
        case AurUpdatePreparationReason::BlockingPreflight:
        case AurUpdatePreparationReason::PreflightInconsistent:
        case AurUpdatePreparationReason::
            DevelRequiresCheckPolicyInconsistent:
        case AurUpdatePreparationReason::BuildPlanMissing:
        case AurUpdatePreparationReason::BuildPlanOrderEmpty:
        case AurUpdatePreparationReason::RootAttributionInconsistent:
        case AurUpdatePreparationReason::PackageTargetAttributionInconsistent:
        case AurUpdatePreparationReason::DesiredInstallReasonMissing:
        case AurUpdatePreparationReason::SourcePreferenceUnavailable:
        case AurUpdatePreparationReason::SourcePreferencePkgdestConflict:
        case AurUpdatePreparationReason::StaticWorkItemInvalid:
        case AurUpdatePreparationReason::PacmanDatabaseUnavailable:
        case AurUpdatePreparationReason::GenericPreparationInconsistent:
        case AurUpdatePreparationReason::BuildUnitSelectionInconsistent:
        case AurUpdatePreparationReason::ExternalSatisfactionInconsistent:
            return true;
    }
    return false;
}

bool is_known_source_preference_failure_kind(
    SourcePreferenceFailureKind kind) noexcept {
    switch(kind) {
        case SourcePreferenceFailureKind::AuthorityUnavailable:
        case SourcePreferenceFailureKind::DirectoryEnumerationFailed:
        case SourcePreferenceFailureKind::InvalidEntryName:
        case SourcePreferenceFailureKind::StatusUnavailable:
        case SourcePreferenceFailureKind::UnsupportedFileType:
        case SourcePreferenceFailureKind::OwnershipMismatch:
        case SourcePreferenceFailureKind::UnsafePermissions:
        case SourcePreferenceFailureKind::OpenFailed:
        case SourcePreferenceFailureKind::LockFailed:
        case SourcePreferenceFailureKind::ReadFailed:
        case SourcePreferenceFailureKind::WriteFailed:
        case SourcePreferenceFailureKind::SyncFailed:
        case SourcePreferenceFailureKind::RenameFailed:
        case SourcePreferenceFailureKind::RemoveFailed:
        case SourcePreferenceFailureKind::ConcurrentReplacement:
        case SourcePreferenceFailureKind::CloseFailed:
            return true;
    }
    return false;
}

bool is_known_package_metadata_error_code(
    PackageMetadataErrorCode code) noexcept {
    switch(code) {
        case PackageMetadataErrorCode::ConfigurationUnavailable:
        case PackageMetadataErrorCode::ConfigurationMalformed:
        case PackageMetadataErrorCode::InitializationFailed:
        case PackageMetadataErrorCode::LocalDatabaseUnavailable:
        case PackageMetadataErrorCode::InvalidPackageName:
        case PackageMetadataErrorCode::QueryFailed:
        case PackageMetadataErrorCode::MalformedMetadata:
        case PackageMetadataErrorCode::SyncDatabaseUnavailable:
        case PackageMetadataErrorCode::RepositoryNotConfigured:
            return true;
    }
    return false;
}

bool is_executable_preflight_status(
    AurUpdateExecutionTargetStatus status) noexcept {
    return status == AurUpdateExecutionTargetStatus::Executable;
}

bool is_blocking_preflight_status(
    AurUpdateExecutionTargetStatus status) noexcept {
    return status == AurUpdateExecutionTargetStatus::Unsupported ||
           status == AurUpdateExecutionTargetStatus::Incomplete;
}

enum class PreparationCorrelationPhase {
    Executable,
    PreflightBlocked,
    NoTargets,
};

PreparationCorrelationPhase determine_preparation_correlation_phase(
    bool has_execution_result,
    bool has_preflight_blocker,
    bool has_executable_target) noexcept {
    if(has_execution_result) {
        return PreparationCorrelationPhase::Executable;
    }
    if(has_preflight_blocker) {
        return PreparationCorrelationPhase::PreflightBlocked;
    }
    if(has_executable_target) {
        return PreparationCorrelationPhase::Executable;
    }
    return PreparationCorrelationPhase::NoTargets;
}

bool is_preparation_issue_target_allowed(
    PreparationCorrelationPhase phase,
    AurUpdateExecutionTargetStatus status) noexcept {
    switch(phase) {
        case PreparationCorrelationPhase::Executable:
            return status == AurUpdateExecutionTargetStatus::Executable;
        case PreparationCorrelationPhase::PreflightBlocked:
            return is_blocking_preflight_status(status);
        case PreparationCorrelationPhase::NoTargets:
            return false;
    }
    return false;
}

bool is_preparation_warning_target_allowed(
    PreparationCorrelationPhase phase,
    AurUpdateExecutionTargetStatus status) noexcept {
    return phase == PreparationCorrelationPhase::Executable &&
           status == AurUpdateExecutionTargetStatus::Executable;
}

AurUpdateOperationTargetStatus initial_target_status(
    AurUpdateExecutionTargetStatus status) noexcept {
    switch(status) {
        case AurUpdateExecutionTargetStatus::Executable:
            return AurUpdateOperationTargetStatus::NotAttempted;
        case AurUpdateExecutionTargetStatus::Skipped:
            return AurUpdateOperationTargetStatus::Skipped;
        case AurUpdateExecutionTargetStatus::Unsupported:
            return AurUpdateOperationTargetStatus::Unsupported;
        case AurUpdateExecutionTargetStatus::Incomplete:
            return AurUpdateOperationTargetStatus::Incomplete;
    }
    return AurUpdateOperationTargetStatus::Incomplete;
}

bool is_known_work_item_status(
    AurUpdateWorkItemExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
        case AurUpdateWorkItemExecutionStatus::Updated:
        case AurUpdateWorkItemExecutionStatus::NoChange:
        case AurUpdateWorkItemExecutionStatus::Cancelled:
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return true;
    }
    return false;
}

bool is_known_child_status(AurUpdateChildExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateChildExecutionStatus::BootstrapSkipped:
        case AurUpdateChildExecutionStatus::Installed:
        case AurUpdateChildExecutionStatus::SkippedAsNeeded:
        case AurUpdateChildExecutionStatus::InstalledCleanupFailed:
        case AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed:
        case AurUpdateChildExecutionStatus::NotAttempted:
            return true;
    }
    return false;
}

bool is_known_failure_kind(AurUpdateWorkItemFailureKind kind) noexcept {
    switch(kind) {
        case AurUpdateWorkItemFailureKind::None:
        case AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete:
        case AurUpdateWorkItemFailureKind::BuildOrInstallFailed:
        case AurUpdateWorkItemFailureKind::CleanupFailedAfterPackageTransaction:
        case AurUpdateWorkItemFailureKind::UnknownException:
        case AurUpdateWorkItemFailureKind::PriorWorkItemStopped:
            return true;
    }
    return false;
}

bool is_terminal_status(AurUpdateWorkItemExecutionStatus status) noexcept {
    return status == AurUpdateWorkItemExecutionStatus::Cancelled ||
           status == AurUpdateWorkItemExecutionStatus::Failed ||
           status == AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed ||
           status ==
               AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed;
}

bool is_cleanup_failure_status(
    AurUpdateWorkItemExecutionStatus status) noexcept {
    return status == AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed ||
           status ==
               AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed;
}

bool has_consistent_failure_kind(
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
        case AurUpdateWorkItemExecutionStatus::Updated:
        case AurUpdateWorkItemExecutionStatus::NoChange:
            return failure_kind == AurUpdateWorkItemFailureKind::None;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
            return failure_kind == AurUpdateWorkItemFailureKind::None;
        case AurUpdateWorkItemExecutionStatus::Failed:
            return failure_kind ==
                       AurUpdateWorkItemFailureKind::BuildOrInstallFailed ||
                   failure_kind == AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete ||
                   failure_kind ==
                       AurUpdateWorkItemFailureKind::UnknownException;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return failure_kind == AurUpdateWorkItemFailureKind::
                                       CleanupFailedAfterPackageTransaction;
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return failure_kind ==
                   AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    }
    return false;
}

bool same_transaction_attempt(
    const AurUpdatePackageTransactionAttempt& lhs,
    const AurUpdatePackageTransactionAttempt& rhs) noexcept {
    return lhs.identity.package_name == rhs.identity.package_name &&
           lhs.identity.full_version == rhs.identity.full_version &&
           lhs.desired_reason == rhs.desired_reason;
}

bool transaction_failure_evidence_is_safe(
    const AurUpdatePackageTransactionFailureSnapshot& failure,
    bool allow_empty_attempts) noexcept {
    switch(failure.category) {
        case AurUpdatePackageTransactionFailureCategory::CommandFailed:
            if(!failure.exit_code.has_value() || *failure.exit_code == 0) {
                return false;
            }
            break;
        case AurUpdatePackageTransactionFailureCategory::CommandExecutionFailed:
        case AurUpdatePackageTransactionFailureCategory::Other:
            if(failure.exit_code.has_value()) return false;
            break;
        default:
            return false;
    }
    if(failure.diagnostic.empty() ||
       (!allow_empty_attempts && failure.attempted_artifacts.empty())) {
        return false;
    }
    return std::all_of(
        failure.attempted_artifacts.begin(),
        failure.attempted_artifacts.end(),
        [](const AurUpdatePackageTransactionAttempt& attempt) {
            return !attempt.identity.package_name.empty() &&
                   !attempt.identity.full_version.empty() &&
                   is_known_desired_install_reason(
                       attempt.desired_reason);
        });
}

bool same_transaction_failure_snapshot(
    const AurUpdatePackageTransactionFailureSnapshot& lhs,
    const AurUpdatePackageTransactionFailureSnapshot& rhs) noexcept {
    return lhs.category == rhs.category &&
           lhs.exit_code == rhs.exit_code &&
           lhs.diagnostic == rhs.diagnostic &&
           lhs.attempted_artifacts.size() ==
               rhs.attempted_artifacts.size() &&
           std::equal(
               lhs.attempted_artifacts.begin(),
               lhs.attempted_artifacts.end(),
               rhs.attempted_artifacts.begin(),
               same_transaction_attempt);
}

bool transaction_failure_payload_is_consistent(
    const AurUpdateWorkItemExecutionResult& work_item,
    const AurUpdatePackageTransactionFailureSnapshot& failure) noexcept {
    if(!work_item.transaction_failure.has_value() ||
       !transaction_failure_evidence_is_safe(failure, false) ||
       !same_transaction_failure_snapshot(
           failure, *work_item.transaction_failure) ||
       failure.attempted_artifacts.size() !=
           work_item.child_results.size()) {
        return false;
    }

    std::set<std::string> attempted_names;
    for(std::size_t child_index = 0;
        child_index < failure.attempted_artifacts.size(); ++child_index) {
        const AurUpdatePackageTransactionAttempt& attempt =
            failure.attempted_artifacts[child_index];
        const AurUpdateChildExecutionResult& child =
            work_item.child_results[child_index];
        if(attempt.identity.package_name.empty() ||
           attempt.identity.full_version.empty() ||
           attempt.identity.package_name != child.required_package_name ||
           attempt.desired_reason != child.desired_install_reason ||
           !is_known_desired_install_reason(attempt.desired_reason) ||
           !attempted_names.insert(attempt.identity.package_name).second) {
            return false;
        }
    }
    return true;
}

bool failure_payload_is_consistent(
    const AurUpdateWorkItemExecutionResult& work_item) noexcept {
    if(work_item.failure_detail.valueless_by_exception()) return false;
    const bool cancelled = work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled;
    if(cancelled != work_item.cancellation.has_value()) return false;
    if(work_item.cancellation &&
       work_item.cancellation->reason != ConfirmationCancellationReason::ExplicitToken &&
       work_item.cancellation->reason != ConfirmationCancellationReason::EndOfInput) return false;
    if(cancelled && (work_item.production_outcome || work_item.devel_execution || work_item.diagnostic)) return false;
    const bool has_no_detail =
        std::holds_alternative<std::monostate>(
            work_item.failure_detail);

    switch(work_item.failure_kind) {
        case AurUpdateWorkItemFailureKind::None:
        case AurUpdateWorkItemFailureKind::CleanupFailedAfterPackageTransaction:
        case AurUpdateWorkItemFailureKind::UnknownException:
        case AurUpdateWorkItemFailureKind::PriorWorkItemStopped:
            return has_no_detail &&
                   !work_item.transaction_failure.has_value();
        case AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete:
            return has_no_detail && !work_item.transaction_failure && work_item.devel_execution && work_item.devel_execution->owner && !work_item.devel_execution->complete;
        case AurUpdateWorkItemFailureKind::BuildOrInstallFailed:
            if(has_no_detail) return false;
            if(const auto* transaction = std::get_if<
                   AurUpdatePackageTransactionFailureSnapshot>(
                   &work_item.failure_detail)) {
                return transaction_failure_payload_is_consistent(
                    work_item, *transaction);
            }
            if(work_item.transaction_failure.has_value()) {
                return std::holds_alternative<
                           AurUpdateExecutionCorrelationFailure>(
                           work_item.failure_detail) &&
                       transaction_failure_evidence_is_safe(
                           *work_item.transaction_failure, true);
            }
            return true;
    }
    return false;
}

AurUpdateWorkItemExecutionStatus map_child_status(
    AurUpdateChildExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateChildExecutionStatus::BootstrapSkipped:
            return AurUpdateWorkItemExecutionStatus::BootstrapSkipped;
        case AurUpdateChildExecutionStatus::Installed:
            return AurUpdateWorkItemExecutionStatus::Updated;
        case AurUpdateChildExecutionStatus::SkippedAsNeeded:
            return AurUpdateWorkItemExecutionStatus::NoChange;
        case AurUpdateChildExecutionStatus::InstalledCleanupFailed:
            return AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed;
        case AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed:
            return AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed;
        case AurUpdateChildExecutionStatus::NotAttempted:
            return AurUpdateWorkItemExecutionStatus::NotAttempted;
    }
    return static_cast<AurUpdateWorkItemExecutionStatus>(-1);
}

AurUpdateOperationExecutionContribution make_contribution(
    const AurUpdateWorkItemExecutionResult& work_item,
    const AurUpdateChildExecutionResult& child,
    AurUpdateWorkItemExecutionStatus status) {
    return AurUpdateOperationExecutionContribution{
        work_item.work_item_index,
        child.required_child_index,
        child.required_package_name,
        child.package_base,
        child.selected_artifact,
        child.desired_install_reason,
        child.affected_roots,
        child.roles,
        status,
        work_item.failure_kind,
        work_item.failure_detail,
        work_item.diagnostic, work_item.cancellation, work_item.bootstrap_decision, work_item.bootstrap_skipped_roots};
}

AurUpdateOperationExecutionContribution make_planned_contribution(
    const AurUpdateWorkItemExecutionResult& work_item,
    const AurUpdateRequiredTargetAttribution& planned_child,
    std::size_t child_index) {
    AurUpdateChildExecutionResult child;
    child.work_item_index = work_item.work_item_index;
    child.build_plan_order_index = work_item.build_plan_order_index;
    child.required_child_index = child_index;
    child.package_base = planned_child.required_target.package_base;
    child.required_package_name =
        planned_child.required_target.package_name;
    child.desired_install_reason =
        planned_child.required_target.desired_reason;
    child.affected_update_plan_indices =
        planned_child.affected_update_plan_indices;
    child.affected_roots = planned_child.affected_roots;
    child.roles = planned_child.roles;
    return make_contribution(work_item, child, work_item.status);
}

AurUpdateOperationTargetStatus map_execution_status(
    AurUpdateWorkItemExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
            return AurUpdateOperationTargetStatus::Skipped;
        case AurUpdateWorkItemExecutionStatus::Updated:
            return AurUpdateOperationTargetStatus::Updated;
        case AurUpdateWorkItemExecutionStatus::NoChange:
            return AurUpdateOperationTargetStatus::NoChange;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
            return AurUpdateOperationTargetStatus::Cancelled;
        case AurUpdateWorkItemExecutionStatus::Failed:
            return AurUpdateOperationTargetStatus::Failed;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            return AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return AurUpdateOperationTargetStatus::NoChangeCleanupFailed;
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return AurUpdateOperationTargetStatus::NotAttempted;
    }
    return AurUpdateOperationTargetStatus::Incomplete;
}

int terminal_priority(AurUpdateWorkItemExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            return 3;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return 2;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
        case AurUpdateWorkItemExecutionStatus::Failed:
            return 1;
        default:
            return 0;
    }
}

void retain_decisive_contribution(
    AurUpdateOperationTargetResult& target,
    const AurUpdateOperationExecutionContribution& contribution) {
    target.cancellation = contribution.cancellation;
    if(contribution.bootstrap_decision) target.bootstrap_decision = contribution.bootstrap_decision;
    if(!contribution.bootstrap_skipped_roots.empty()) target.bootstrap_skipped_roots = contribution.bootstrap_skipped_roots;
    target.execution_work_item_index = contribution.work_item_index;
    if(contribution.failure_kind == AurUpdateWorkItemFailureKind::None) {
        target.execution_failure_kind.reset();
        target.execution_failure_detail.reset();
    } else {
        target.execution_failure_kind = contribution.failure_kind;
        target.execution_failure_detail = contribution.failure_detail;
    }
    target.execution_diagnostic = contribution.diagnostic;
}

void fold_execution_contributions(
    AurUpdateOperationResult& result,
    AurUpdateOperationTargetResult& target) {
    for(const auto& contribution : target.execution_contributions) {
        if(contribution.bootstrap_decision && contribution.package_name == target.update.installed_name &&
           has_aur_update_bootstrap_intent(target.update)) {
            target.bootstrap_decision = contribution.bootstrap_decision;
            if(contribution.bootstrap_decision->state != AurUpdateBootstrapDecisionState::Accepted) {
                target.status = AurUpdateOperationTargetStatus::Skipped;
                target.skip_kind = AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
                target.preflight_issues = {AurUpdateExecutionIssue{
                    AurUpdateExecutionReason::DevelRequiresCheck, target.update.installed_name,
                    target.update.aur_package->package_base, std::nullopt,
                    localization::translate_message("Devel tracking bootstrap was skipped; package remains unverified."),
                    std::nullopt, std::nullopt, DevelRequiresCheckReason::ProvenanceMissing}};
                retain_decisive_contribution(target, contribution);
                return;
            }
        }
    }
    const AurUpdateOperationExecutionContribution* terminal = nullptr;
    const AurUpdateOperationExecutionContribution* unknown = nullptr;
    const AurUpdateOperationExecutionContribution* not_attempted = nullptr;
    const AurUpdateOperationExecutionContribution* updated = nullptr;
    const AurUpdateOperationExecutionContribution* no_change = nullptr;
    std::set<std::size_t> terminal_work_item_indices;

    for(const auto& contribution : target.execution_contributions) {
        if(!is_known_work_item_status(contribution.status)) {
            if(unknown == nullptr) unknown = &contribution;
            continue;
        }
        if(is_terminal_status(contribution.status)) {
            terminal_work_item_indices.insert(contribution.work_item_index);
            if(terminal == nullptr ||
               terminal_priority(contribution.status) >
                   terminal_priority(terminal->status)) {
                terminal = &contribution;
            }
            continue;
        }
        if(contribution.status ==
           AurUpdateWorkItemExecutionStatus::NotAttempted) {
            if(not_attempted == nullptr) not_attempted = &contribution;
        } else if(contribution.status ==
                  AurUpdateWorkItemExecutionStatus::Updated) {
            updated = &contribution;
        } else if(contribution.status ==
                  AurUpdateWorkItemExecutionStatus::NoChange) {
            no_change = &contribution;
        }
    }

    if(terminal_work_item_indices.size() > 1) {
        add_reduction_issue(
            result,
            AurUpdateOperationReductionReason::WorkItemResultInconsistent,
            AurUpdateOperationReductionStage::Execution,
            localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "Terminal execution outcomes from multiple work items were attributed to one {} update target.",
                "AUR"),
            {target.update_plan_index}, {},
            terminal == nullptr
                ? std::nullopt
                : std::optional<std::size_t>{
                      terminal->work_item_index});
    }

    // POLICY(#267): terminal failureは後続NotAttemptedより優先し、unknownを
    // 含む場合も既知のfailure/cleanup partial resultを捨てない。
    const AurUpdateOperationExecutionContribution* decisive = terminal;
    if(decisive == nullptr && unknown != nullptr) {
        target.status = AurUpdateOperationTargetStatus::Incomplete;
        decisive = unknown;
    } else if(decisive == nullptr && not_attempted != nullptr) {
        target.status = AurUpdateOperationTargetStatus::NotAttempted;
        decisive = not_attempted;
    } else if(decisive == nullptr && updated != nullptr) {
        target.status = AurUpdateOperationTargetStatus::Updated;
        decisive = updated;
    } else if(decisive == nullptr && no_change != nullptr) {
        target.status = AurUpdateOperationTargetStatus::NoChange;
        decisive = no_change;
    }

    if(terminal != nullptr) {
        target.status = map_execution_status(terminal->status);
    }
    if(decisive != nullptr) retain_decisive_contribution(target, *decisive);
}

const AurUpdateProjectedBuildUnit* expected_projected_build_unit(
    const AurUpdateSourceBuildPreparation& preparation,
    const AurUpdateWorkItemExecutionResult& work_item) noexcept {
    const AurUpdateBuildUnitSelectionEntry* selected_entry = nullptr;
    for(const auto& entry : preparation.build_unit_selection.entries) {
        if(entry.status != AurUpdateBuildUnitSelectionStatus::
                               SelectedForAurExecution ||
           entry.selected_execution_index != work_item.work_item_index) {
            continue;
        }
        if(selected_entry != nullptr) return nullptr;
        selected_entry = &entry;
    }
    if(selected_entry == nullptr ||
       selected_entry->build_plan_order_index !=
           work_item.build_plan_order_index) {
        return nullptr;
    }

    const AurUpdateProjectedBuildUnit* projected = nullptr;
    for(const auto& candidate : preparation.projected_build_units) {
        if(candidate.build_plan_order_index !=
           selected_entry->build_plan_order_index) {
            continue;
        }
        if(projected != nullptr) return nullptr;
        projected = &candidate;
    }
    return projected;
}

std::vector<std::string> planned_package_names(
    const AurUpdateProjectedBuildUnit& projected) {
    std::vector<std::string> names;
    names.reserve(projected.required_target_attributions.size());
    for(const auto& child : projected.required_target_attributions) {
        names.push_back(child.required_target.package_name);
    }
    return names;
}

bool same_child_snapshot(
    const AurUpdateChildExecutionResult& child,
    const AurUpdateRequiredTargetAttribution& planned,
    const AurUpdateWorkItemExecutionResult& work_item,
    std::size_t child_index) noexcept {
    return child.work_item_index == work_item.work_item_index &&
           child.build_plan_order_index ==
               work_item.build_plan_order_index &&
           child.required_child_index == child_index &&
           child.package_base == planned.required_target.package_base &&
           child.required_package_name ==
               planned.required_target.package_name &&
           child.desired_install_reason ==
               planned.required_target.desired_reason &&
           child.affected_update_plan_indices ==
               planned.affected_update_plan_indices &&
           child.affected_roots == planned.affected_roots &&
           child.roles == planned.roles;
}

bool child_selected_artifact_is_coherent(
    const AurUpdateChildExecutionResult& child) noexcept {
    return child.selected_artifact.has_value() &&
           child.selected_artifact->package_name ==
               child.required_package_name &&
           !child.selected_artifact->package_name.empty() &&
           !child.selected_artifact->full_version.empty();
}

bool child_outcome_matches_work_item(
    AurUpdateWorkItemExecutionStatus work_item_status,
    AurUpdateChildExecutionStatus child_status, bool authoritative = false) noexcept {
    if(authoritative && work_item_status == AurUpdateWorkItemExecutionStatus::Failed && child_status == AurUpdateChildExecutionStatus::Installed) return true;
    switch(work_item_status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
            return child_status == AurUpdateChildExecutionStatus::BootstrapSkipped;
        case AurUpdateWorkItemExecutionStatus::Updated:
            return child_status == AurUpdateChildExecutionStatus::Installed ||
                   child_status ==
                       AurUpdateChildExecutionStatus::SkippedAsNeeded;
        case AurUpdateWorkItemExecutionStatus::NoChange:
            return child_status ==
                   AurUpdateChildExecutionStatus::SkippedAsNeeded;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            return child_status == AurUpdateChildExecutionStatus::
                                       InstalledCleanupFailed ||
                   child_status == AurUpdateChildExecutionStatus::
                                       SkippedAsNeededCleanupFailed;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return child_status == AurUpdateChildExecutionStatus::
                                       SkippedAsNeededCleanupFailed;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return child_status == AurUpdateChildExecutionStatus::NotAttempted;
    }
    return false;
}

bool work_item_child_outcomes_are_consistent(
    const AurUpdateWorkItemExecutionResult& work_item) noexcept {
    if(work_item.child_results.empty()) return false;

    bool has_installed = false;
    for(const auto& child : work_item.child_results) {
        if(!is_known_child_status(child.status)) return false;
        if(work_item.devel_execution && work_item.status == AurUpdateWorkItemExecutionStatus::Failed &&
           work_item.devel_execution->operation == DevelSourceArtifactInstallOperation::Succeeded &&
           work_item.devel_execution->proof == DevelSourceArtifactInstallProof::Complete &&
           work_item.devel_execution->artifact && child.status == AurUpdateChildExecutionStatus::Installed && child_selected_artifact_is_coherent(child)) continue;
        switch(work_item.status) {
            case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
                if(child.status != AurUpdateChildExecutionStatus::BootstrapSkipped || child.selected_artifact) return false;
                break;
            case AurUpdateWorkItemExecutionStatus::Updated:
                if((child.status != AurUpdateChildExecutionStatus::Installed &&
                    child.status !=
                        AurUpdateChildExecutionStatus::SkippedAsNeeded) ||
                   !child_selected_artifact_is_coherent(child)) {
                    return false;
                }
                has_installed = has_installed ||
                                child.status == AurUpdateChildExecutionStatus::Installed;
                break;
            case AurUpdateWorkItemExecutionStatus::NoChange:
                if(child.status !=
                       AurUpdateChildExecutionStatus::SkippedAsNeeded ||
                   !child_selected_artifact_is_coherent(child)) {
                    return false;
                }
                break;
            case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
                if((child.status != AurUpdateChildExecutionStatus::
                                        InstalledCleanupFailed &&
                    child.status != AurUpdateChildExecutionStatus::
                                        SkippedAsNeededCleanupFailed) ||
                   !child_selected_artifact_is_coherent(child)) {
                    return false;
                }
                has_installed = has_installed ||
                                child.status == AurUpdateChildExecutionStatus::
                                                    InstalledCleanupFailed;
                break;
            case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
                if(child.status != AurUpdateChildExecutionStatus::
                                       SkippedAsNeededCleanupFailed ||
                   !child_selected_artifact_is_coherent(child)) {
                    return false;
                }
                break;
            case AurUpdateWorkItemExecutionStatus::Cancelled:
            case AurUpdateWorkItemExecutionStatus::Failed:
            case AurUpdateWorkItemExecutionStatus::NotAttempted:
                if(child.status != AurUpdateChildExecutionStatus::NotAttempted ||
                   child.selected_artifact.has_value()) {
                    return false;
                }
                break;
        }
    }

    if(work_item.status == AurUpdateWorkItemExecutionStatus::Updated ||
       work_item.status ==
           AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed) {
        return has_installed;
    }
    return true;
}

bool invocation_result_is_consistent(
    const AurUpdateSourceBuildExecutionResult& execution) noexcept {
    const SelectedRepositoryProviderTransactionResult& provider_transaction =
        execution.selected_repository_provider_transaction;
    switch(provider_transaction.status) {
        case SelectedRepositoryProviderTransactionStatus::NotRequired:
            if(!provider_transaction.selected_providers.empty() ||
               provider_transaction.package_state_change !=
                   PackageStateChange::NoChange ||
               provider_transaction.command_exit_status.has_value() ||
               provider_transaction.diagnostic.has_value()) {
                return false;
            }
            break;
        case SelectedRepositoryProviderTransactionStatus::
            BlockedBeforeExecution:
            if(provider_transaction.selected_providers.empty() ||
               provider_transaction.package_state_change !=
                   PackageStateChange::NoChange ||
               provider_transaction.command_exit_status.has_value() ||
               !provider_transaction.diagnostic.has_value() ||
               provider_transaction.diagnostic->empty()) {
                return false;
            }
            break;
        case SelectedRepositoryProviderTransactionStatus::Succeeded:
            if(provider_transaction.selected_providers.empty() ||
               provider_transaction.package_state_change !=
                   PackageStateChange::Unknown ||
               provider_transaction.command_exit_status !=
                   std::optional<int>{0} ||
               provider_transaction.diagnostic.has_value()) {
                return false;
            }
            break;
        case SelectedRepositoryProviderTransactionStatus::Failed:
            if(provider_transaction.selected_providers.empty() ||
               provider_transaction.package_state_change !=
                   PackageStateChange::Unknown ||
               (provider_transaction.command_exit_status.has_value() &&
                *provider_transaction.command_exit_status == 0) ||
               !provider_transaction.diagnostic.has_value() ||
               provider_transaction.diagnostic->empty()) {
                return false;
            }
            break;
        case SelectedRepositoryProviderTransactionStatus::OutcomeUnknown:
            if(provider_transaction.selected_providers.empty() ||
               provider_transaction.package_state_change !=
                   PackageStateChange::Unknown ||
               provider_transaction.command_exit_status.has_value() ||
               !provider_transaction.diagnostic.has_value() ||
               provider_transaction.diagnostic->empty()) {
                return false;
            }
            break;
        default:
            return false;
    }

    std::size_t terminal_count = 0;
    std::size_t terminal_position = 0;
    bool has_unknown_status = false;
    for(std::size_t position = 0;
        position < execution.work_item_results.size(); ++position) {
        const auto status = execution.work_item_results[position].status;
        if(!is_known_work_item_status(status)) {
            has_unknown_status = true;
            continue;
        }
        if(!work_item_child_outcomes_are_consistent(
               execution.work_item_results[position])) {
            return false;
        }
        if(is_terminal_status(status)) {
            ++terminal_count;
            terminal_position = position;
        }
    }
    if(has_unknown_status || terminal_count > 1) return false;

    if(execution.phase == AurUpdateInvocationExecutionPhase::BootstrapDecisions) {
        return execution.status == AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation &&
               provider_transaction.status == SelectedRepositoryProviderTransactionStatus::NotRequired &&
               terminal_count == 1 && execution.work_item_results[terminal_position].status == AurUpdateWorkItemExecutionStatus::Cancelled &&
               std::all_of(execution.work_item_results.begin(), execution.work_item_results.end(), [](const auto& item) {
                   return item.status == AurUpdateWorkItemExecutionStatus::NotAttempted ||
                          item.status == AurUpdateWorkItemExecutionStatus::BootstrapSkipped ||
                          item.status == AurUpdateWorkItemExecutionStatus::Cancelled;
               });
    }
    if(execution.phase != AurUpdateInvocationExecutionPhase::WorkItems) return false;
    if(terminal_count == 1) {
        for(std::size_t position = 0;
            position < execution.work_item_results.size(); ++position) {
            const auto status = execution.work_item_results[position].status;
            if(position < terminal_position &&
               status != AurUpdateWorkItemExecutionStatus::Updated &&
               status != AurUpdateWorkItemExecutionStatus::NoChange &&
               status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped) {
                return false;
            }
            if(position > terminal_position &&
               status != AurUpdateWorkItemExecutionStatus::NotAttempted &&
               status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped) {
                return false;
            }
        }
    }

    switch(execution.status) {
        case AurUpdateInvocationExecutionStatus::Completed:
            return execution.selected_repository_provider_transaction.is_success() &&
                   terminal_count == 0 && std::all_of(execution.work_item_results.begin(), execution.work_item_results.end(), [](const AurUpdateWorkItemExecutionResult& work_item) {
                       return work_item.status ==
                                  AurUpdateWorkItemExecutionStatus::Updated ||
                              work_item.status ==
                                  AurUpdateWorkItemExecutionStatus::NoChange ||
                              work_item.status == AurUpdateWorkItemExecutionStatus::BootstrapSkipped;
                   });
        case AurUpdateInvocationExecutionStatus::
            StoppedOnProviderTransactionFailure:
            return (execution.selected_repository_provider_transaction.status ==
                        SelectedRepositoryProviderTransactionStatus::Failed ||
                    execution.selected_repository_provider_transaction.status ==
                        SelectedRepositoryProviderTransactionStatus::
                            OutcomeUnknown) &&
                   terminal_count == 0 && std::all_of(execution.work_item_results.begin(), execution.work_item_results.end(), [](const AurUpdateWorkItemExecutionResult& work_item) {
                       // The preflight-aware bootstrap coherence check still
                       // validates the decision and original root attribution.
                       return work_item.status ==
                                  AurUpdateWorkItemExecutionStatus::
                                      NotAttempted ||
                              work_item.status == AurUpdateWorkItemExecutionStatus::BootstrapSkipped;
                   });
        case AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation:
            return execution.selected_repository_provider_transaction.is_success() &&
                   terminal_count == 1 &&
                   execution.work_item_results[terminal_position].status ==
                       AurUpdateWorkItemExecutionStatus::Cancelled;
        case AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure:
            return execution.selected_repository_provider_transaction.is_success() &&
                   terminal_count == 1 &&
                   execution.work_item_results[terminal_position].status ==
                       AurUpdateWorkItemExecutionStatus::Failed;
        case AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure:
            return execution.selected_repository_provider_transaction.is_success() &&
                   terminal_count == 1 &&
                   is_cleanup_failure_status(
                       execution.work_item_results[terminal_position]
                           .status);
    }
    return false;
}

bool is_known_invocation_status(
    AurUpdateInvocationExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateInvocationExecutionStatus::Completed:
        case AurUpdateInvocationExecutionStatus::
            StoppedOnProviderTransactionFailure:
        case AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation:
        case AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure:
        case AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure:
            return true;
    }
    return false;
}

AurUpdateOperationStatus map_invocation_status(
    AurUpdateInvocationExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateInvocationExecutionStatus::Completed:
            return AurUpdateOperationStatus::Completed;
        case AurUpdateInvocationExecutionStatus::
            StoppedOnProviderTransactionFailure:
            return AurUpdateOperationStatus::
                StoppedOnProviderTransactionFailure;
        case AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation:
            return AurUpdateOperationStatus::StoppedOnWorkItemCancellation;
        case AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure:
            return AurUpdateOperationStatus::StoppedOnWorkItemFailure;
        case AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure:
            return AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure;
    }
    return AurUpdateOperationStatus::InconsistentResult;
}

bool is_valid_success_target_snapshot(
    const AurUpdateOperationTargetResult& target,
    DevelRequiresCheckPolicy policy) noexcept {
    if(target.cancellation.has_value()) return false;
    switch(target.status) {
        case AurUpdateOperationTargetStatus::Updated:
        case AurUpdateOperationTargetStatus::NoChange:
            return !target.skip_kind.has_value() &&
                   target.preflight_issues.empty() &&
                   target.preparation_issues.empty();
        case AurUpdateOperationTargetStatus::Skipped:
            return target.preparation_issues.empty() &&
                   (is_valid_aur_update_normal_skip_snapshot(
                        target.update, target.preflight_issues,
                        target.skip_kind) ||
                    is_valid_aur_update_independent_devel_skip_snapshot(
                        target.update, target.preflight_issues,
                        target.skip_kind, policy));
        case AurUpdateOperationTargetStatus::Unsupported:
        case AurUpdateOperationTargetStatus::Incomplete:
        case AurUpdateOperationTargetStatus::Cancelled:
        case AurUpdateOperationTargetStatus::Failed:
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
        case AurUpdateOperationTargetStatus::NotAttempted:
            return false;
    }
    return false;
}

bool bootstrap_execution_snapshots_are_consistent(
    const AurUpdateExecutionPreflight& preflight, const AurUpdateSourceBuildExecutionResult& execution) noexcept {
    const auto owner = [&](const AurUpdateWorkItemExecutionResult& item) -> const AurUpdateExecutionTarget* {
        if(item.affected_update_plan_indices.size() != 1 || item.child_results.size() != 1) return nullptr;
        const auto index = item.affected_update_plan_indices.front();
        if(index >= preflight.targets.size()) return nullptr;
        const auto& target = preflight.targets[index];
        const auto& child = item.child_results.front();
        if(target.update_plan_index != index || !has_aur_update_bootstrap_intent(target.update) ||
           item.package_name != target.update.installed_name || item.package_base != target.update.aur_package->package_base ||
           child.required_package_name != target.update.installed_name || child.roles.size() != 1 || child.roles.front() != PackageRole::Root) return nullptr;
        return &target;
    };
    for(const auto& item : execution.work_item_results) {
        const auto* target = owner(item);
        if(item.bootstrap_decision) {
            if(!target || preflight.devel_requires_check_policy != DevelRequiresCheckPolicy::SkipIndependentTarget) return false;
            const auto& decision = *item.bootstrap_decision;
            const auto* accepted = decision.confirmation ? std::get_if<ConfirmationAccepted>(&*decision.confirmation) : nullptr;
            const auto* declined = decision.confirmation ? std::get_if<ConfirmationDeclined>(&*decision.confirmation) : nullptr;
            switch(decision.state) {
                case AurUpdateBootstrapDecisionState::Accepted:
                    if(!accepted || accepted->origin != ConfirmationDecisionOrigin::ExplicitToken ||
                       item.status == AurUpdateWorkItemExecutionStatus::BootstrapSkipped) return false;
                    if(item.status == AurUpdateWorkItemExecutionStatus::Updated &&
                       (!item.devel_execution || !item.devel_execution->complete ||
                        item.devel_execution->publication != DevelBuildProvenancePublicationState::Complete)) return false;
                    break;
                case AurUpdateBootstrapDecisionState::Declined:
                    if(!declined || (declined->origin != ConfirmationDecisionOrigin::ExplicitToken && declined->origin != ConfirmationDecisionOrigin::Default)) return false;
                    if(item.status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped) return false;
                    break;
                case AurUpdateBootstrapDecisionState::ObservationChanged:
                    if(decision.confirmation && (!accepted || accepted->origin != ConfirmationDecisionOrigin::ExplicitToken)) return false;
                    if(item.status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped) return false;
                    break;
                case AurUpdateBootstrapDecisionState::InteractionUnavailable:
                    if(decision.confirmation || item.status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped) return false;
                    break;
                default: return false;
            }
        } else if(target &&
                  (execution.phase != AurUpdateInvocationExecutionPhase::BootstrapDecisions ||
                   (item.status != AurUpdateWorkItemExecutionStatus::NotAttempted && item.status != AurUpdateWorkItemExecutionStatus::Cancelled))) {
            return false;
        }
        if(item.status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped) {
            if(!item.bootstrap_skipped_roots.empty()) return false;
            continue;
        }
        if(item.bootstrap_skipped_roots.empty() || item.bootstrap_skipped_roots != item.affected_update_plan_indices ||
           item.production_outcome || item.devel_execution || item.diagnostic || item.transaction_failure ||
           !item.unselected_artifacts.empty() || item.failure_kind != AurUpdateWorkItemFailureKind::None) return false;
        for(const auto root : item.bootstrap_skipped_roots) {
            if(std::count(item.bootstrap_skipped_roots.begin(), item.bootstrap_skipped_roots.end(), root) != 1 ||
               !std::any_of(execution.work_item_results.begin(), execution.work_item_results.end(), [&](const auto& decision_item) {
                   const auto* root_target = owner(decision_item);
                   return root_target && root_target->update_plan_index == root && decision_item.bootstrap_decision &&
                          decision_item.bootstrap_decision->state != AurUpdateBootstrapDecisionState::Accepted;
               })) return false;
        }
    }
    return true;
}

} // namespace

bool AurUpdateOperationResult::is_success() const noexcept {
    if(!devel_requires_check_policy.has_value() ||
       !is_known_devel_requires_check_policy(
           *devel_requires_check_policy) ||
       (status != AurUpdateOperationStatus::NoUpdates &&
        status != AurUpdateOperationStatus::Completed) ||
       !selected_repository_provider_transaction.is_success()) {
        return false;
    }
    return std::all_of(
        targets.begin(), targets.end(),
        [this](const AurUpdateOperationTargetResult& target) {
            return is_valid_success_target_snapshot(
                target, *devel_requires_check_policy);
        });
}

PackageStateChange AurUpdateOperationResult::package_state_change()
    const noexcept {
    if(selected_repository_provider_transaction.package_state_change ==
       PackageStateChange::Changed) {
        return PackageStateChange::Changed;
    }
    if(std::any_of(
           execution_work_items.begin(), execution_work_items.end(),
           [](const AurUpdateWorkItemExecutionResult& work_item) {
               return std::any_of(
                   work_item.child_results.begin(),
                   work_item.child_results.end(),
                   [](const AurUpdateChildExecutionResult& child) {
                       return child.status ==
                                  AurUpdateChildExecutionStatus::
                                      Installed ||
                              child.status ==
                                  AurUpdateChildExecutionStatus::
                                      InstalledCleanupFailed;
                   });
           })) {
        return PackageStateChange::Changed;
    }
    for(const auto& target : targets) {
        if(target.status == AurUpdateOperationTargetStatus::Updated ||
           target.status ==
               AurUpdateOperationTargetStatus::UpdatedCleanupFailed) {
            return PackageStateChange::Changed;
        }
        for(const auto& contribution : target.execution_contributions) {
            if(contribution.status ==
                   AurUpdateWorkItemExecutionStatus::Updated ||
               contribution.status == AurUpdateWorkItemExecutionStatus::
                                          UpdatedCleanupFailed) {
                return PackageStateChange::Changed;
            }
        }
    }
    if(devel_requires_check_policy ==
           std::optional<DevelRequiresCheckPolicy>{
               DevelRequiresCheckPolicy::SkipIndependentTarget} &&
       std::any_of(
           targets.begin(), targets.end(),
           [this](const AurUpdateOperationTargetResult& target) {
               return target.status ==
                          AurUpdateOperationTargetStatus::Skipped &&
                      is_valid_aur_update_independent_devel_skip_snapshot(
                          target.update, target.preflight_issues,
                          target.skip_kind,
                          *devel_requires_check_policy);
           })) {
        return PackageStateChange::Unknown;
    }
    return selected_repository_provider_transaction.package_state_change ==
                   PackageStateChange::Unknown
               ? PackageStateChange::Unknown
               : PackageStateChange::NoChange;
}

bool AurUpdateOperationResult::changed_package_state() const noexcept {
    return package_state_change() == PackageStateChange::Changed;
}

bool AurUpdateOperationResult::has_partial_completion() const noexcept {
    return !is_success() &&
           (changed_package_state() ||
            selected_repository_provider_transaction.status ==
                SelectedRepositoryProviderTransactionStatus::Succeeded);
}

bool AurUpdateOperationResult::has_not_attempted_targets() const noexcept {
    return std::any_of(
        targets.begin(), targets.end(),
        [](const AurUpdateOperationTargetResult& target) {
            return target.status ==
                   AurUpdateOperationTargetStatus::NotAttempted;
        });
}

bool AurUpdateOperationResult::has_cleanup_failure() const noexcept {
    if(std::any_of(
           execution_work_items.begin(), execution_work_items.end(),
           [](const AurUpdateWorkItemExecutionResult& work_item) {
               return is_cleanup_failure_status(work_item.status);
           })) {
        return true;
    }
    for(const auto& target : targets) {
        if(target.status ==
               AurUpdateOperationTargetStatus::UpdatedCleanupFailed ||
           target.status == AurUpdateOperationTargetStatus::
                                NoChangeCleanupFailed) {
            return true;
        }
        if(std::any_of(
               target.execution_contributions.begin(),
               target.execution_contributions.end(),
               [](const AurUpdateOperationExecutionContribution&
                      contribution) {
                   return is_cleanup_failure_status(
                       contribution.status);
               })) {
            return true;
        }
    }
    return false;
}

bool AurUpdateOperationResult::has_blocking_targets() const noexcept {
    return std::any_of(
        targets.begin(), targets.end(),
        [](const AurUpdateOperationTargetResult& target) {
            return target.status ==
                       AurUpdateOperationTargetStatus::Unsupported ||
                   target.status ==
                       AurUpdateOperationTargetStatus::Incomplete;
        });
}

AurUpdateOperationResult reduce_aur_update_operation_result(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateSourceBuildPreparation& preparation,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    const std::optional<AurUpdateSourceBuildExecutionResult>& execution) {
    AurUpdateOperationResult result;
    result.devel_requires_check_policy =
        devel_requires_check_policy;
    result.preparation_issues = preparation.issues;
    result.preparation_warnings = preparation.warnings;
    if(execution.has_value()) {
        result.execution_status = execution->status;
        result.execution_work_items = execution->work_item_results;
        result.selected_repository_provider_transaction =
            execution->selected_repository_provider_transaction;
    }
    if(execution && !bootstrap_execution_snapshots_are_consistent(preflight, *execution)) {
        add_reduction_issue(result, AurUpdateOperationReductionReason::WorkItemResultInconsistent,
                            AurUpdateOperationReductionStage::Execution,
                            localization::translate_message("Devel bootstrap decisions or skipped-root correlations are inconsistent."));
    }
    result.targets.reserve(preflight.targets.size());

    const std::optional<DevelRequiresCheckPolicy> expected_policy{
        devel_requires_check_policy};
    if(!is_known_devel_requires_check_policy(
           devel_requires_check_policy) ||
       preflight.devel_requires_check_policy != expected_policy ||
       preparation.devel_requires_check_policy != expected_policy ||
       !has_valid_aur_update_execution_policy_snapshot(preflight)) {
        add_reduction_issue(
            result,
            AurUpdateOperationReductionReason::
                DevelRequiresCheckPolicyInconsistent,
            AurUpdateOperationReductionStage::Preflight,
            std::string{
                DEVEL_REQUIRES_CHECK_POLICY_SNAPSHOT_DIAGNOSTIC});
    }
    if(devel_requires_check_policy ==
           DevelRequiresCheckPolicy::SkipIndependentTarget &&
       preflight.devel_requires_check_policy == expected_policy &&
       has_valid_aur_update_execution_policy_snapshot(preflight) &&
       !has_complete_aur_update_required_devel_relation_snapshot(
           preflight)) {
        add_reduction_issue(
            result,
            AurUpdateOperationReductionReason::
                OtherCorrelationInconsistent,
            AurUpdateOperationReductionStage::Preflight,
            std::string{
                DEVEL_REQUIRES_CHECK_POLICY_SNAPSHOT_DIAGNOSTIC});
    }

    std::map<std::size_t, std::vector<std::size_t>>
        positions_by_update_plan_index;
    std::vector<bool> executable_positions(preflight.targets.size(), false);
    std::vector<bool> blocking_positions(preflight.targets.size(), false);
    bool has_executable_target = false;
    bool has_preflight_blocker = false;
    bool all_targets_are_skipped = true;

    for(std::size_t position = 0; position < preflight.targets.size();
        ++position) {
        const AurUpdateExecutionTarget& input = preflight.targets[position];
        bool has_unknown_target_enum = false;
        positions_by_update_plan_index[input.update_plan_index].push_back(
            position);

        AurUpdateOperationTargetResult target;
        target.update_plan_index = input.update_plan_index;
        target.update = input.update;
        target.package_base = package_base_for_target(input);
        target.status = initial_target_status(input.status);
        target.preflight_issues = input.issues;
        target.skip_kind = input.skip_kind;
        result.targets.push_back(std::move(target));

        if(!is_known_preflight_status(input.status)) {
            has_unknown_target_enum = true;
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preflight target has an unknown status.",
                    "AUR"),
                {input.update_plan_index}, {position});
            all_targets_are_skipped = false;
        } else {
            executable_positions[position] =
                is_executable_preflight_status(input.status);
            blocking_positions[position] =
                is_blocking_preflight_status(input.status);
            has_executable_target = has_executable_target ||
                                    executable_positions[position];
            has_preflight_blocker = has_preflight_blocker ||
                                    blocking_positions[position];
            all_targets_are_skipped = all_targets_are_skipped &&
                                      input.status == AurUpdateExecutionTargetStatus::Skipped;
        }

        for(const auto& issue : input.issues) {
            if(!is_known_preflight_reason(issue.reason)) {
                has_unknown_target_enum = true;
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::UnknownEnumValue,
                    AurUpdateOperationReductionStage::Preflight,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preflight issue has an unknown reason.",
                        "AUR"),
                    {input.update_plan_index}, {position});
            }
            if(!is_valid_aur_update_execution_issue_devel_payload(
                   issue)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        OtherCorrelationInconsistent,
                    AurUpdateOperationReductionStage::Preflight,
                    localization::format_translated_message(
                        "{} update preflight issue has an unknown reason.",
                        "AUR"),
                    {input.update_plan_index}, {position});
            }
        }
        if(input.status == AurUpdateExecutionTargetStatus::Executable &&
           !is_valid_aur_update_execution_target_skip_snapshot(
               input, devel_requires_check_policy)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Executable {} update target retains a blocking preflight issue.",
                    "AUR"),
                {input.update_plan_index}, {position});
        }
        if(!is_known_installed_package_reason(
               input.update.install_reason)) {
            has_unknown_target_enum = true;
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update target has an unknown installed package reason.",
                    "AUR"),
                {input.update_plan_index}, {position});
        }
        if(!is_known_update_classification(input.update.classification)) {
            has_unknown_target_enum = true;
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update target has an unknown update classification.",
                    "AUR"),
                {input.update_plan_index}, {position});
        }
        if(input.update.aur_package.has_value() &&
           !is_known_version_relation(
               input.update.aur_package->version_relation)) {
            has_unknown_target_enum = true;
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update target has an unknown version relation.",
                    "AUR"),
                {input.update_plan_index}, {position});
        }
        if(input.desired_install_reason.has_value() &&
           !is_known_desired_install_reason(
               *input.desired_install_reason)) {
            has_unknown_target_enum = true;
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update target has an unknown desired install reason.",
                    "AUR"),
                {input.update_plan_index}, {position});
        }
        if(input.status == AurUpdateExecutionTargetStatus::Skipped &&
           (has_unknown_target_enum ||
            !has_valid_skipped_preflight_issues(
                input, devel_requires_check_policy))) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Skipped {} update target cannot be confirmed as a normal skip.",
                    "AUR"),
                {input.update_plan_index}, {position});
            result.targets[position].status =
                AurUpdateOperationTargetStatus::Incomplete;
            all_targets_are_skipped = false;
        }
        if(input.status != AurUpdateExecutionTargetStatus::Skipped &&
           input.skip_kind.has_value()) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    "{} update preflight target has an unknown status.",
                    "AUR"),
                {input.update_plan_index}, {position});
            result.targets[position].status =
                AurUpdateOperationTargetStatus::Incomplete;
            all_targets_are_skipped = false;
        }

        if(input.update_plan_index >= preflight.targets.size()) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OutOfRangePreflightUpdatePlanIndex,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preflight target has an out-of-range update plan index.",
                    "AUR"),
                {input.update_plan_index}, {position});
            if(executable_positions[position]) {
                result.targets[position].status =
                    AurUpdateOperationTargetStatus::Incomplete;
            }
        }
        if(input.update_plan_index != position) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreflightTargetOrderInconsistent,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preflight target index differs from its original-order position.",
                    "AUR"),
                {input.update_plan_index}, {position});
        }
    }

    std::map<std::size_t, std::size_t> unique_position_by_update_plan_index;
    for(const auto& [update_plan_index, positions] :
        positions_by_update_plan_index) {
        if(positions.size() > 1) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    DuplicatePreflightUpdatePlanIndex,
                AurUpdateOperationReductionStage::Preflight,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preflight contains a duplicate update plan index.",
                    "AUR"),
                {update_plan_index}, positions);
            for(const std::size_t position : positions) {
                if(executable_positions[position]) {
                    result.targets[position].status =
                        AurUpdateOperationTargetStatus::Incomplete;
                }
            }
            continue;
        }
        if(update_plan_index < preflight.targets.size()) {
            unique_position_by_update_plan_index.emplace(
                update_plan_index, positions.front());
        }
    }

    const PreparationCorrelationPhase preparation_phase =
        determine_preparation_correlation_phase(
            execution.has_value(), has_preflight_blocker,
            has_executable_target);

    // POLICY(#267): normal preparation/executionは全Executable snapshot、
    // preflight blockerは全blocking target snapshotを正本にする。
    const std::vector<bool>& expected_preparation_target_positions =
        preparation_phase == PreparationCorrelationPhase::PreflightBlocked
            ? blocking_positions
            : executable_positions;
    validate_preparation_target_snapshot(
        result, preflight, preparation,
        expected_preparation_target_positions);
    if(preparation_phase == PreparationCorrelationPhase::NoTargets &&
       !preparation.affected_roots.empty()) {
        add_reduction_issue(
            result,
            AurUpdateOperationReductionReason::
                PreparationAttributionInconsistent,
            AurUpdateOperationReductionStage::Preparation,
            "Preparation retains root attribution without a target-bearing phase.");
    }

    const bool apply_preparation_failures = !execution.has_value() &&
                                            !has_preflight_blocker && !preparation.issues.empty();
    for(const auto& issue : preparation.issues) {
        if(issue.affected_update_plan_indices.empty() &&
           preparation_phase != PreparationCorrelationPhase::Executable) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationAttributionInconsistent,
                AurUpdateOperationReductionStage::Preparation,
                "Global preparation issue exists outside the executable preparation phase.");
        }
        if(!is_known_preparation_reason(issue.reason)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preparation issue has an unknown reason.",
                    "AUR"),
                issue.affected_update_plan_indices);
        } else if(issue.reason == AurUpdatePreparationReason::None) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preparation issue has no typed reason.",
                    "AUR"),
                issue.affected_update_plan_indices);
        }
        if(issue.preflight_issue.has_value() &&
           !is_known_preflight_reason(issue.preflight_issue->reason)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Nested {} update preflight issue has an unknown reason.",
                    "AUR"),
                issue.affected_update_plan_indices);
        }
        if(issue.source_preference_failure.has_value() &&
           !is_known_source_preference_failure_kind(
               issue.source_preference_failure->kind)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preparation issue has an unknown source preference failure kind.",
                    "AUR"),
                issue.affected_update_plan_indices);
        }
        if(issue.package_metadata_failure.has_value() &&
           !is_known_package_metadata_error_code(
               issue.package_metadata_failure->code)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preparation issue has an unknown package metadata error code.",
                    "AUR"),
                issue.affected_update_plan_indices);
        }
        std::set<std::size_t> seen_indices;
        for(const std::size_t update_plan_index :
            issue.affected_update_plan_indices) {
            if(!seen_indices.insert(update_plan_index).second) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        DuplicatePreparationAttribution,
                    AurUpdateOperationReductionStage::Preparation,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preparation issue contains duplicate target attribution.",
                        "AUR"),
                    {update_plan_index});
                continue;
            }

            const auto position =
                unique_position_by_update_plan_index.find(
                    update_plan_index);
            if(position == unique_position_by_update_plan_index.end()) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        UnknownPreparationUpdatePlanIndex,
                    AurUpdateOperationReductionStage::Preparation,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preparation issue refers to an unknown update plan index.",
                        "AUR"),
                    {update_plan_index});
                continue;
            }

            AurUpdateOperationTargetResult& target =
                result.targets[position->second];
            if(!is_preparation_issue_target_allowed(
                   preparation_phase,
                   preflight.targets[position->second].status)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        PreparationAttributionInconsistent,
                    AurUpdateOperationReductionStage::Preparation,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preparation issue is attributed to a target outside the current preparation phase.",
                        "AUR"),
                    {update_plan_index}, {position->second});
            }
            target.preparation_issues.push_back(issue);
            if(apply_preparation_failures &&
               executable_positions[position->second]) {
                target.status = AurUpdateOperationTargetStatus::Failed;
            }
        }
    }

    for(const auto& warning : preparation.warnings) {
        if(warning.affected_update_plan_indices.empty() &&
           !warning.affected_roots.empty()) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationAttributionInconsistent,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preparation warning has root attribution without update target attribution.",
                    "AUR"));
        }
        std::set<std::size_t> seen_indices;
        for(const std::size_t update_plan_index :
            warning.affected_update_plan_indices) {
            if(!seen_indices.insert(update_plan_index).second) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        DuplicatePreparationAttribution,
                    AurUpdateOperationReductionStage::Preparation,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preparation warning contains duplicate target attribution.",
                        "AUR"),
                    {update_plan_index});
                continue;
            }
            if(!unique_position_by_update_plan_index.contains(
                   update_plan_index)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        UnknownPreparationUpdatePlanIndex,
                    AurUpdateOperationReductionStage::Preparation,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preparation warning refers to an unknown update plan index.",
                        "AUR"),
                    {update_plan_index});
                continue;
            }

            const std::size_t target_position =
                unique_position_by_update_plan_index.at(
                    update_plan_index);
            if(!is_preparation_warning_target_allowed(
                   preparation_phase,
                   preflight.targets[target_position].status)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        PreparationAttributionInconsistent,
                    AurUpdateOperationReductionStage::Preparation,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update preparation warning is attributed to a target outside the warning-producing phase.",
                        "AUR"),
                    {update_plan_index}, {target_position});
            }
        }
    }

    if(!execution.has_value()) {
        const bool has_prepared_execution_snapshot =
            preparation.invocation.has_value() || std::any_of(
                                                      preparation.affected_update_targets.begin(),
                                                      preparation.affected_update_targets.end(),
                                                      [](const AurUpdateExecutionTarget& target) {
                                                          return target.status ==
                                                                 AurUpdateExecutionTargetStatus::Executable;
                                                      });
        if(preparation.invocation.has_value() &&
           !preparation.issues.empty()) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent,
                AurUpdateOperationReductionStage::Preparation,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update preparation contains both an invocation and blocking issues.",
                    "AUR"));
        } else if(
            preparation_phase ==
                PreparationCorrelationPhase::PreflightBlocked &&
            preparation.invocation.has_value()) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent,
                AurUpdateOperationReductionStage::Preparation,
                "Preflight-blocked preparation unexpectedly contains an execution invocation.");
        }
        if(!has_preflight_blocker && preparation.issues.empty() &&
           (has_executable_target || has_prepared_execution_snapshot)) {
            std::vector<std::size_t> missing_indices;
            for(std::size_t position = 0;
                position < preflight.targets.size(); ++position) {
                if(executable_positions[position]) {
                    missing_indices.push_back(
                        preflight.targets[position].update_plan_index);
                }
            }
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::MissingExecutionResult,
                AurUpdateOperationReductionStage::Execution,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Prepared {} update targets have no execution result.",
                    "AUR"),
                std::move(missing_indices));
        }
    } else {
        if(!preparation.issues.empty()) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    ExecutionResultWithPreparationIssues,
                AurUpdateOperationReductionStage::Execution,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update execution result exists together with preparation issues.",
                    "AUR"));
        }
        if(has_preflight_blocker || !has_executable_target) {
            if(has_preflight_blocker) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        OtherCorrelationInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution result exists for a preflight-blocked operation.",
                        "AUR"));
            } else {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        OtherCorrelationInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution result exists without an executable preflight target.",
                        "AUR"));
            }
        }

        auto project_contribution =
            [&](const AurUpdateWorkItemExecutionResult& work_item,
                const AurUpdateOperationExecutionContribution& contribution,
                const std::vector<std::size_t>& update_plan_indices) {
                std::set<std::size_t> seen_child_indices;
                for(const std::size_t update_plan_index :
                    update_plan_indices) {
                    if(!seen_child_indices.insert(update_plan_index).second) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                DuplicateExecutionChildAttribution,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "{} update execution child contains duplicate target attribution.",
                                "AUR"),
                            {update_plan_index}, {},
                            work_item.work_item_index);
                        continue;
                    }

                    const auto position =
                        unique_position_by_update_plan_index.find(
                            update_plan_index);
                    if(position ==
                       unique_position_by_update_plan_index.end()) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                UnknownExecutionChildUpdatePlanIndex,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "{} update execution child refers to an unknown update plan index.",
                                "AUR"),
                            {update_plan_index}, {},
                            work_item.work_item_index);
                        continue;
                    }

                    const std::size_t target_position = position->second;
                    result.targets[target_position]
                        .execution_contributions.push_back(
                            contribution);
                    if(!executable_positions[target_position]) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                OtherCorrelationInconsistent,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "{} update execution child is attributed to a non-executable preflight target.",
                                "AUR"),
                            {update_plan_index}, {target_position},
                            work_item.work_item_index);
                    }
                }
            };

        std::map<std::size_t, std::size_t> first_result_position_by_work_index;
        for(std::size_t result_position = 0;
            result_position < execution->work_item_results.size();
            ++result_position) {
            const AurUpdateWorkItemExecutionResult& work_item =
                execution->work_item_results[result_position];
            const bool inserted =
                first_result_position_by_work_index.emplace(
                                                       work_item.work_item_index, result_position)
                    .second;
            if(!inserted) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        DuplicateExecutionWorkItemIndex,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution result contains a duplicate work item index.",
                        "AUR"),
                    {}, {}, work_item.work_item_index);
            }
            if(work_item.work_item_index != result_position) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        ExecutionWorkItemOrderInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution work item index differs from result order.",
                        "AUR"),
                    {}, {}, work_item.work_item_index);
            }

            const bool known_status =
                is_known_work_item_status(work_item.status);
            const bool known_failure_kind =
                is_known_failure_kind(work_item.failure_kind);
            if(!known_status) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::UnknownEnumValue,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update work item result has an unknown status.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }
            if(!known_failure_kind) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::UnknownEnumValue,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update work item result has an unknown failure kind.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }
            if(known_status && known_failure_kind &&
               !has_consistent_failure_kind(
                   work_item.status, work_item.failure_kind)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        WorkItemResultInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update work item status and failure kind disagree.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }
            if(known_failure_kind &&
               !failure_payload_is_consistent(work_item)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        WorkItemResultInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update work item failure detail or transaction attempt snapshot is inconsistent.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }
            if(work_item.affected_update_plan_indices.empty()) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        WorkItemResultInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update work item result has no target attribution.",
                        "AUR"),
                    {}, {}, work_item.work_item_index);
            }

            const AurUpdateProjectedBuildUnit* projected =
                expected_projected_build_unit(preparation, work_item);
            if(projected == nullptr) {
                // Pure reducer callers historically supply an execution-only
                // snapshot. Production preparations always carry both models;
                // a partial/nonempty model must therefore correlate exactly.
                if(!preparation.build_unit_selection.entries.empty() ||
                   !preparation.projected_build_units.empty()) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            WorkItemResultInconsistent,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution work item cannot be correlated to one selected projected build unit.",
                            "AUR"),
                        work_item.affected_update_plan_indices, {},
                        work_item.work_item_index);
                }

                // Correlation issueがあっても、self-contained child snapshotの
                // known completionはoperation-level/target-levelの両方へ残す。
                std::set<std::size_t> seen_child_positions;
                for(const auto& child : work_item.child_results) {
                    if(!seen_child_positions.insert(
                                                child.required_child_index)
                            .second) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                DuplicateExecutionChildAttribution,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "{} update execution result contains a duplicate required child index.",
                                "AUR"),
                            child.affected_update_plan_indices, {},
                            work_item.work_item_index);
                        continue;
                    }
                    if(!is_known_child_status(child.status)) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                ExecutionChildSnapshotInconsistent,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "Uncorrelated {} update execution child has an inconsistent outcome.",
                                "AUR"),
                            child.affected_update_plan_indices, {},
                            work_item.work_item_index);
                        continue;
                    }
                    if(!is_known_work_item_status(work_item.status)) {
                        project_contribution(
                            work_item,
                            make_contribution(
                                work_item, child,
                                work_item.status),
                            child.affected_update_plan_indices);
                        continue;
                    }
                    if(!child_outcome_matches_work_item(
                           work_item.status, child.status, work_item.devel_execution.has_value())) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                ExecutionChildSnapshotInconsistent,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "Uncorrelated {} update execution child has an inconsistent outcome.",
                                "AUR"),
                            child.affected_update_plan_indices, {},
                            work_item.work_item_index);
                        continue;
                    }
                    const bool should_have_selected_artifact =
                        work_item.status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped &&
                        work_item.status != AurUpdateWorkItemExecutionStatus::Cancelled &&
                        work_item.status !=
                            AurUpdateWorkItemExecutionStatus::Failed &&
                        work_item.status != AurUpdateWorkItemExecutionStatus::
                                                NotAttempted;
                    if(should_have_selected_artifact !=
                           child.selected_artifact.has_value() ||
                       (should_have_selected_artifact &&
                        !child_selected_artifact_is_coherent(child))) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                UnexpectedSelectedArtifact,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "Uncorrelated {} update execution child has an inconsistent selected artifact.",
                                "AUR"),
                            child.affected_update_plan_indices, {},
                            work_item.work_item_index);
                        continue;
                    }
                    const AurUpdateWorkItemExecutionStatus child_status =
                        work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled
                            ? AurUpdateWorkItemExecutionStatus::Cancelled
                        : work_item.status ==
                                AurUpdateWorkItemExecutionStatus::
                                    Failed
                            ? AurUpdateWorkItemExecutionStatus::Failed
                            : map_child_status(child.status);
                    project_contribution(
                        work_item,
                        make_contribution(
                            work_item, child, child_status),
                        child.affected_update_plan_indices);
                }
                continue;
            }

            const std::vector<std::string> expected_package_names =
                planned_package_names(*projected);
            const std::string expected_compatibility_name =
                expected_package_names.size() == 1
                    ? expected_package_names.front()
                    : std::string{};
            if(work_item.package_base != projected->package_base ||
               work_item.package_name != expected_compatibility_name ||
               work_item.plan_package_names != expected_package_names ||
               work_item.affected_update_plan_indices !=
                   projected->affected_update_plan_indices ||
               work_item.affected_roots != projected->affected_roots) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        WorkItemResultInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution work-item aggregate snapshot differs from preparation.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }
            if(!work_item_child_outcomes_are_consistent(work_item)) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        ExecutionChildSnapshotInconsistent,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution child outcomes do not agree with the work-item aggregate state.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }

            const auto& planned_children =
                projected->required_target_attributions;
            std::vector<bool> matched_children(
                planned_children.size(), false);
            std::set<std::size_t> seen_child_positions;
            std::set<std::string> selected_names;
            for(const auto& child : work_item.child_results) {
                for(const std::size_t update_plan_index :
                    child.affected_update_plan_indices) {
                    if(!unique_position_by_update_plan_index.contains(
                           update_plan_index)) {
                        add_reduction_issue(
                            result,
                            AurUpdateOperationReductionReason::
                                UnknownExecutionChildUpdatePlanIndex,
                            AurUpdateOperationReductionStage::Execution,
                            localization::format_translated_message(
                                // TRANSLATORS: AUR is a runtime project identity.
                                "{} update execution child refers to an unknown update plan index.",
                                "AUR"),
                            {update_plan_index}, {},
                            work_item.work_item_index);
                    }
                }

                if(!is_known_child_status(child.status)) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            UnknownEnumValue,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution child has an unknown status.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }
                if(child.required_child_index >= planned_children.size()) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            UnexpectedExecutionChildAttribution,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution result contains an extra required child.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }
                if(!seen_child_positions.insert(
                                            child.required_child_index)
                        .second) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            DuplicateExecutionChildAttribution,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution result contains a duplicate required child index.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }

                const std::size_t child_index = child.required_child_index;
                const AurUpdateRequiredTargetAttribution& planned_child =
                    planned_children[child_index];
                if(!same_child_snapshot(
                       child, planned_child, work_item, child_index)) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            ExecutionChildSnapshotInconsistent,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution required child snapshot differs from preparation.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }
                matched_children[child_index] = true;

                const bool should_have_selected_artifact =
                    work_item.status != AurUpdateWorkItemExecutionStatus::BootstrapSkipped &&
                    work_item.status != AurUpdateWorkItemExecutionStatus::Cancelled &&
                    work_item.status !=
                        AurUpdateWorkItemExecutionStatus::Failed &&
                    work_item.status != AurUpdateWorkItemExecutionStatus::
                                            NotAttempted;
                if(should_have_selected_artifact !=
                       child.selected_artifact.has_value() ||
                   (should_have_selected_artifact &&
                    !child_selected_artifact_is_coherent(child))) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            UnexpectedSelectedArtifact,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution child selected artifact is missing or inconsistent.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }
                if(child.selected_artifact.has_value() &&
                   !selected_names.insert(
                                      child.selected_artifact->package_name)
                        .second) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            UnexpectedSelectedArtifact,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution result contains a duplicate selected artifact identity.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }
                if(!child_outcome_matches_work_item(
                       work_item.status, child.status, work_item.devel_execution.has_value())) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            ExecutionChildSnapshotInconsistent,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update execution child outcome disagrees with the work-item state.",
                            "AUR"),
                        child.affected_update_plan_indices, {},
                        work_item.work_item_index);
                    continue;
                }

                if(work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled ||
                   work_item.status ==
                       AurUpdateWorkItemExecutionStatus::Failed ||
                   work_item.status == AurUpdateWorkItemExecutionStatus::
                                           NotAttempted) {
                    continue;
                }
                const AurUpdateWorkItemExecutionStatus child_status =
                    map_child_status(child.status);
                project_contribution(
                    work_item,
                    make_contribution(
                        work_item, child, child_status),
                    child.affected_update_plan_indices);
            }

            for(std::size_t child_index = 0;
                child_index < matched_children.size(); ++child_index) {
                if(matched_children[child_index]) continue;
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        MissingExecutionChildAttribution,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "{} update execution result is missing a prepared required child.",
                        "AUR"),
                    planned_children[child_index]
                        .affected_update_plan_indices,
                    {}, work_item.work_item_index);
            }

            if((work_item.status == AurUpdateWorkItemExecutionStatus::BootstrapSkipped ||
                work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled ||
                work_item.status ==
                    AurUpdateWorkItemExecutionStatus::Failed ||
                work_item.status ==
                    AurUpdateWorkItemExecutionStatus::NotAttempted) &&
               !work_item.unselected_artifacts.empty()) {
                add_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        UnexpectedUnselectedArtifactIdentity,
                    AurUpdateOperationReductionStage::Execution,
                    localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "Failed or not-attempted {} update work item unexpectedly contains unselected artifact identities.",
                        "AUR"),
                    work_item.affected_update_plan_indices, {},
                    work_item.work_item_index);
            }

            for(const ArtifactPackageIdentity& unselected :
                work_item.unselected_artifacts) {
                if(unselected.package_name.empty() ||
                   unselected.full_version.empty() ||
                   selected_names.contains(unselected.package_name)) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            UnexpectedUnselectedArtifactIdentity,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update work-item unselected artifact identity is invalid or overlaps a selected child.",
                            "AUR"),
                        {}, {}, work_item.work_item_index);
                }
            }
            for(std::size_t artifact_index = 0;
                artifact_index < work_item.unselected_artifacts.size();
                ++artifact_index) {
                const std::string& package_name =
                    work_item.unselected_artifacts[artifact_index]
                        .package_name;
                const bool is_duplicate = std::any_of(
                    work_item.unselected_artifacts.begin(),
                    work_item.unselected_artifacts.begin() +
                        artifact_index,
                    [&package_name](const ArtifactPackageIdentity& other) {
                        return other.package_name == package_name;
                    });
                if(is_duplicate) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            UnexpectedUnselectedArtifactIdentity,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "{} update work-item contains a duplicate unselected artifact identity.",
                            "AUR"),
                        {}, {}, work_item.work_item_index);
                }
            }

            // Cancelled/failed and later NotAttempted work items carry no selected
            // child outcome. Their target projection comes from preparation.
            if(work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled ||
               work_item.status == AurUpdateWorkItemExecutionStatus::Failed ||
               work_item.status ==
                   AurUpdateWorkItemExecutionStatus::NotAttempted) {
                for(std::size_t child_index = 0;
                    child_index < planned_children.size(); ++child_index) {
                    const auto& planned_child = planned_children[child_index];
                    project_contribution(
                        work_item,
                        make_planned_contribution(
                            work_item, planned_child, child_index),
                        planned_child.affected_update_plan_indices);
                }
            }
        }

        for(std::size_t position = 0; position < result.targets.size();
            ++position) {
            if(!executable_positions[position]) continue;
            if(result.targets[position].execution_contributions.empty()) {
                const std::size_t update_plan_index =
                    result.targets[position].update_plan_index;
                if(unique_position_by_update_plan_index.contains(
                       update_plan_index)) {
                    add_reduction_issue(
                        result,
                        AurUpdateOperationReductionReason::
                            MissingExecutionAttribution,
                        AurUpdateOperationReductionStage::Execution,
                        localization::format_translated_message(
                            // TRANSLATORS: AUR is a runtime project identity.
                            "Executable {} update target has no execution attribution.",
                            "AUR"),
                        {update_plan_index}, {position});
                }
                continue;
            }
            fold_execution_contributions(result, result.targets[position]);
        }

        if(!is_known_invocation_status(execution->status)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::UnknownEnumValue,
                AurUpdateOperationReductionStage::Execution,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update invocation result has an unknown status.",
                    "AUR"));
        } else if(!invocation_result_is_consistent(*execution)) {
            add_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    InvocationResultInconsistent,
                AurUpdateOperationReductionStage::Execution,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} update invocation status and work item outcomes disagree.",
                    "AUR"));
        }
    }

    if(!result.reduction_issues.empty()) {
        result.status = AurUpdateOperationStatus::InconsistentResult;
    } else if(execution.has_value()) {
        result.status = map_invocation_status(execution->status);
    } else if(has_preflight_blocker || !preparation.issues.empty()) {
        result.status = AurUpdateOperationStatus::BlockedBeforeExecution;
    } else if(!has_executable_target && all_targets_are_skipped) {
        result.status = AurUpdateOperationStatus::NoUpdates;
    } else {
        // 上のmissing-result検査で通常は到達しない。将来fieldが増えても
        // empty successへ倒れないようfail-closedにする。
        add_reduction_issue(
            result,
            AurUpdateOperationReductionReason::
                OtherCorrelationInconsistent,
            AurUpdateOperationReductionStage::Execution,
            localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "{} update operation could not be reduced to a known status.",
                "AUR"));
        result.status = AurUpdateOperationStatus::InconsistentResult;
    }
    return result;
}
