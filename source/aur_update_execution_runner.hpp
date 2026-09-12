#pragma once

#include "aur_update_execution_preparation.hpp"
#include "interactive_confirmation.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct AppConfig;

enum class AurUpdateWorkItemExecutionStatus {
    Updated,
    NoChange,
    Failed,
    UpdatedCleanupFailed,
    NoChangeCleanupFailed,
    NotAttempted,
    Cancelled,
    BootstrapSkipped,
};

enum class AurUpdateWorkItemFailureKind {
    None,
    BuildOrInstallFailed,
    AuthoritativeExecutionIncomplete,
    CleanupFailedAfterPackageTransaction,
    UnknownException,
    PriorWorkItemStopped,
};

enum class AurUpdateInvocationExecutionStatus {
    Completed,
    StoppedOnProviderTransactionFailure,
    StoppedOnWorkItemFailure,
    StoppedAfterPackageCleanupFailure,
    StoppedOnWorkItemCancellation,
};

enum class AurUpdateChildExecutionStatus {
    Installed,
    SkippedAsNeeded,
    InstalledCleanupFailed,
    SkippedAsNeededCleanupFailed,
    NotAttempted,
    BootstrapSkipped,
};

enum class AurUpdateSourceBuildFailureCategory {
    Build,
    ArtifactValidation,
    ArtifactIdentity,
    InstallPreparation,
    InstallTransaction,
    Other,
};

struct AurUpdateSourceBuildFailureSnapshot {
    AurUpdateSourceBuildFailureCategory category =
        AurUpdateSourceBuildFailureCategory::Other;
    std::string diagnostic;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure;
};

enum class AurUpdatePackageTransactionFailureCategory {
    CommandFailed,
    CommandExecutionFailed,
    Other,
};

using AurUpdatePackageTransactionAttempt =
    PackageBaseArtifactInstallTransactionAttempt;

struct AurUpdatePackageTransactionFailureSnapshot {
    AurUpdatePackageTransactionFailureCategory category =
        AurUpdatePackageTransactionFailureCategory::Other;
    std::vector<AurUpdatePackageTransactionAttempt> attempted_artifacts;
    std::optional<int> exit_code;
    std::string diagnostic;
};

enum class AurUpdateExecutionCorrelationFailureReason {
    PackageBaseMismatch,
    DesiredInstallReasonMismatch,
    SelectedArtifactIdentityMismatch,
    EmptySelectedArtifactVersion,
    UnknownChildOutcome,
    DuplicateSelectedChild,
    MissingSelectedChild,
    ExtraSelectedChild,
    InvalidUnselectedArtifactIdentity,
    SelectedAndUnselectedIdentityOverlap,
    DuplicateUnselectedArtifactIdentity,
};

struct AurUpdateExecutionCorrelationFailure {
    AurUpdateExecutionCorrelationFailureReason reason =
        AurUpdateExecutionCorrelationFailureReason::
            PackageBaseMismatch;
    std::optional<std::size_t> required_child_index;
    std::optional<std::string> package_name;
    std::string diagnostic;
};

using AurUpdateWorkItemFailureDetail = std::variant<
    std::monostate,
    PackageBaseArtifactIdentitySelectionFailure,
    MixedPackageBaseInstallReasonUnsupported,
    PackageMetadataFailure,
    AurUpdateSourceBuildFailureSnapshot,
    AurUpdatePackageTransactionFailureSnapshot,
    AurUpdateExecutionCorrelationFailure>;

enum class AurUpdateBootstrapDecisionState { Accepted,
                                             Declined,
                                             ObservationChanged,
                                             InteractionUnavailable };
struct AurUpdateBootstrapDecision {
    AurUpdateBootstrapDecisionState state;
    std::optional<ConfirmationResult> confirmation;
    bool operator==(const AurUpdateBootstrapDecision&) const = default;
};

// Preparationが確定したrequired child attributionを最初からowned保持し、
// transaction成功時だけselected identity/outcomeを埋める。
struct AurUpdateChildExecutionResult {
    std::size_t work_item_index = 0;
    std::size_t build_plan_order_index = 0;
    std::size_t required_child_index = 0;
    std::string package_base;
    std::string required_package_name;
    DesiredInstallReason desired_install_reason =
        static_cast<DesiredInstallReason>(-1);
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    std::vector<PackageRole> roles;
    std::optional<ArtifactPackageIdentity> selected_artifact;
    AurUpdateChildExecutionStatus status =
        AurUpdateChildExecutionStatus::NotAttempted;
};

// 1 PackageBaseのexecution outcomeと、次段のtarget-level reducerに必要な
// update target/root attributionをowned snapshotとして保持する。
struct AurUpdateWorkItemExecutionResult {
    std::size_t work_item_index = 0;
    std::size_t build_plan_order_index = 0;
    std::string package_name;
    std::string package_base;
    std::vector<std::string> plan_package_names;

    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome;

    // child_resultsがtarget projectionのauthority。unselected artifactは
    // attributionを持たず、diagnostic/presentation snapshotに限定する。
    std::vector<AurUpdateChildExecutionResult> child_results;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
    // Transaction correlation failureでも、success outcomeとは独立したsafe
    // attempt/category/exit snapshotを失わない。
    std::optional<AurUpdatePackageTransactionFailureSnapshot>
        transaction_failure;

    AurUpdateWorkItemExecutionStatus status =
        AurUpdateWorkItemExecutionStatus::NotAttempted;
    AurUpdateWorkItemFailureKind failure_kind =
        AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    AurUpdateWorkItemFailureDetail failure_detail;
    std::optional<std::string> diagnostic;
    std::optional<ReviewedDevelExecutionSnapshot> devel_execution = std::nullopt;
    // Confirmation authority is independent of ordinary execution failure.
    std::optional<ConfirmationCancelled> cancellation = std::nullopt;
    std::optional<AurUpdateBootstrapDecision> bootstrap_decision = std::nullopt;
    // Original query-plan indices, not names or compacted work-item indices.
    std::vector<std::size_t> bootstrap_skipped_roots = {};
};

enum class AurUpdateInvocationExecutionPhase { WorkItems,
                                               BootstrapDecisions };

struct AurUpdateSourceBuildExecutionResult {
    AurUpdateInvocationExecutionStatus status =
        AurUpdateInvocationExecutionStatus::Completed;
    std::vector<AurUpdateWorkItemExecutionResult> work_item_results;
    SelectedRepositoryProviderTransactionResult
        selected_repository_provider_transaction;

    AurUpdateInvocationExecutionPhase phase = AurUpdateInvocationExecutionPhase::WorkItems;

    bool is_success() const noexcept;
    PackageStateChange package_state_change() const noexcept;
    bool changed_package_state() const noexcept;
    bool has_not_attempted_items() const noexcept;
    bool has_cleanup_failure() const noexcept;
    std::optional<std::size_t> stopped_work_item_index() const noexcept;
};

// Owned partial facts travel with the stop signal; callers must not resume mutation.
class AurUpdateExecutionCancelled final : public std::exception {
public:
    explicit AurUpdateExecutionCancelled(AurUpdateSourceBuildExecutionResult result) noexcept;
    const AurUpdateSourceBuildExecutionResult& result() const noexcept;
    AurUpdateSourceBuildExecutionResult release_result() && noexcept;
    const char* what() const noexcept override;

private:
    AurUpdateSourceBuildExecutionResult result_;
};

// Correlated preparation snapshotをone-shot capabilityとしてconsumeし、逐次実行する。
// preflight、BuildPlan、source preference、Pacman DBは再queryせず、最初のfailureで
// 後続をNotAttemptedのまま返す。
AurUpdateSourceBuildExecutionResult
execute_prepared_aur_update_source_build_invocation(
    PreparedAurUpdateSourceBuildInvocation invocation,
    const AppConfig& config);

#ifdef MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
#include <functional>
// Production-connected bootstrap fixtures may replace unrelated legacy work.
// The runner never applies this seam to a bootstrap work item.
using AurUpdateNonBootstrapExecutionTestHook = std::function<std::optional<PackageBaseSourceBuildExecutionResult>(const ProductionSourceBuildWorkItem&)>;
void set_aur_update_non_bootstrap_execution_test_hook(AurUpdateNonBootstrapExecutionTestHook hook);
#endif
