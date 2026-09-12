#pragma once

#include "aur_update_execution_runner.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class AurUpdateOperationTargetStatus {
    Updated,
    NoChange,
    Skipped,
    Unsupported,
    Incomplete,
    Failed,
    UpdatedCleanupFailed,
    NoChangeCleanupFailed,
    NotAttempted,
    Cancelled,
};

enum class AurUpdateOperationStatus {
    NoUpdates,
    Completed,
    BlockedBeforeExecution,
    StoppedOnProviderTransactionFailure,
    StoppedOnWorkItemFailure,
    StoppedAfterPackageCleanupFailure,
    InconsistentResult,
    StoppedOnWorkItemCancellation,
};

enum class AurUpdateOperationReductionStage {
    Preflight,
    Preparation,
    Execution,
};

// Reducerが通常入力として扱えない相関を、partial resultと分離して保持する。
enum class AurUpdateOperationReductionReason {
    DuplicatePreflightUpdatePlanIndex,
    OutOfRangePreflightUpdatePlanIndex,
    PreflightTargetOrderInconsistent,

    DuplicatePreparationAttribution,
    UnknownPreparationUpdatePlanIndex,
    PreparationAttributionInconsistent,
    PreparationTargetSnapshotInconsistent,

    DuplicateExecutionWorkItemIndex,
    ExecutionWorkItemOrderInconsistent,
    DuplicateExecutionAttribution,
    UnknownExecutionUpdatePlanIndex,
    MissingExecutionAttribution,
    DuplicateExecutionChildAttribution,
    MissingExecutionChildAttribution,
    UnexpectedExecutionChildAttribution,
    UnknownExecutionChildUpdatePlanIndex,
    ExecutionChildSnapshotInconsistent,
    UnexpectedSelectedArtifact,
    UnexpectedUnselectedArtifactIdentity,

    ExecutionResultWithPreparationIssues,
    MissingExecutionResult,

    DevelRequiresCheckPolicyInconsistent,
    UnknownEnumValue,
    WorkItemResultInconsistent,
    InvocationResultInconsistent,
    OtherCorrelationInconsistent,
};

struct AurUpdateOperationReductionIssue {
    AurUpdateOperationReductionReason reason =
        AurUpdateOperationReductionReason::OtherCorrelationInconsistent;
    AurUpdateOperationReductionStage stage =
        AurUpdateOperationReductionStage::Preflight;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<std::size_t> preflight_target_positions;
    std::optional<std::size_t> execution_work_item_index;
    std::string diagnostic;
};

// 同じupdate targetへ複数PackageBaseが正常に帰属するため、最終statusへ
// flattenする前のtyped outcomeもtarget単位でowned保持する。
struct AurUpdateOperationExecutionContribution {
    std::size_t work_item_index = 0;
    std::size_t required_child_index = 0;
    std::string package_name;
    std::string package_base;
    std::optional<ArtifactPackageIdentity> selected_artifact;
    std::optional<DesiredInstallReason> desired_install_reason;
    std::vector<RootTargetIdentity> affected_roots;
    std::vector<PackageRole> roles;
    AurUpdateWorkItemExecutionStatus status =
        AurUpdateWorkItemExecutionStatus::NotAttempted;
    AurUpdateWorkItemFailureKind failure_kind =
        AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    AurUpdateWorkItemFailureDetail failure_detail;
    std::optional<std::string> diagnostic;
    std::optional<ConfirmationCancelled> cancellation = std::nullopt;
    std::optional<AurUpdateBootstrapDecision> bootstrap_decision = std::nullopt;
    std::vector<std::size_t> bootstrap_skipped_roots = {};
};

struct AurUpdateOperationTargetResult {
    std::size_t update_plan_index = 0;
    AurUpdatePlanEntry update;
    std::optional<std::string> package_base;

    AurUpdateOperationTargetStatus status =
        AurUpdateOperationTargetStatus::Incomplete;

    std::vector<AurUpdateExecutionIssue> preflight_issues;
    std::vector<AurUpdatePreparationIssue> preparation_issues;

    // final statusを決めたdecisive work itemのtyped detail。
    std::optional<std::size_t> execution_work_item_index;
    std::optional<AurUpdateWorkItemFailureKind> execution_failure_kind;
    std::optional<AurUpdateWorkItemFailureDetail> execution_failure_detail;
    std::optional<std::string> execution_diagnostic;

    std::vector<AurUpdateOperationExecutionContribution>
        execution_contributions;
    std::optional<AurUpdateExecutionSkipKind> skip_kind;
    std::optional<ConfirmationCancelled> cancellation = std::nullopt;
    std::optional<AurUpdateBootstrapDecision> bootstrap_decision = std::nullopt;
    std::vector<std::size_t> bootstrap_skipped_roots = {};
};

struct AurUpdateOperationResult {
    AurUpdateOperationStatus status =
        AurUpdateOperationStatus::InconsistentResult;
    std::vector<AurUpdateOperationTargetResult> targets;

    // attributionが壊れてtargetへ射影できない場合も、runnerが返した既知の
    // partial outcome自体はoperation-level snapshotとして失わない。
    std::optional<AurUpdateInvocationExecutionStatus> execution_status;
    std::vector<AurUpdateWorkItemExecutionResult> execution_work_items;
    SelectedRepositoryProviderTransactionResult
        selected_repository_provider_transaction;

    // operation-level snapshotはglobal/target-attributedを分けず、入力順と
    // nested typed failureを保って保持する。
    std::vector<AurUpdatePreparationIssue> preparation_issues;
    std::vector<AurUpdatePreparationWarning> preparation_warnings;
    std::vector<AurUpdateOperationReductionIssue> reduction_issues;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;

    bool is_success() const noexcept;
    PackageStateChange package_state_change() const noexcept;
    bool changed_package_state() const noexcept;
    bool has_partial_completion() const noexcept;
    bool has_not_attempted_targets() const noexcept;
    bool has_cleanup_failure() const noexcept;
    bool has_blocking_targets() const noexcept;
};

// Preflight、preparation、optional executionを、元update plan順の
// target-level owned resultへ畳む。通常の相関不整合はthrowせずissue化する。
AurUpdateOperationResult reduce_aur_update_operation_result(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateSourceBuildPreparation& preparation,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    const std::optional<AurUpdateSourceBuildExecutionResult>& execution);
