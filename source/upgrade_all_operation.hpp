#pragma once

#include "cross_source_version_lock.hpp"
#include "cross_source_version_lock_observation.hpp"
#include "filtered_aur_update_operation.hpp"
#include "package_metadata.hpp"
#include "system_source_upgrade.hpp"
#include "trusted_cache.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct AppConfig;

enum class UpgradeAllOperationPhase {
    None,
    Preparation,
    System,
    RegisteredSource,
    ForeignInventory,
    AurQuery,
    AurPreparation,
    AurExecution,
    Reduction,
};

enum class UpgradeAllOperationStatus {
    Completed,
    NoUpdates,
    BlockedBeforeMutation,
    StoppedOnSystemFailure,
    StoppedOnSourceFailure,
    StoppedAfterSourceCleanupFailure,
    StoppedBeforeAurExecution,
    StoppedOnAurFailure,
    StoppedAfterAurCleanupFailure,
    InconsistentResult,
    StoppedOnAurCancellation,
};

enum class UpgradeAllForeignInventoryPhaseStatus {
    NotAttempted,
    Completed,
    Failed,
};

enum class UpgradeAllAurPhaseStatus {
    NotAttempted,
    NoUpdates,
    Completed,
    BlockedBeforeExecution,
    StoppedOnProviderTransactionFailure,
    StoppedOnWorkItemFailure,
    StoppedAfterCleanupFailure,
    InconsistentResult,
    StoppedOnWorkItemCancellation,
};

enum class UpgradeAllNotAttemptedReason {
    PreparationBlocked,
    SystemFailure,
    SourceFailure,
    SourceCleanupFailure,
    SystemSourceIncomplete,
    ForeignInventoryFailure,
    CacheAuthorityFailure,
    PriorAggregateInconsistency,
};

enum class UpgradeAllExplicitSourceAdapterIssueKind {
    PreferencePackageNameMissing,
    PackageBaseUnavailable,
    CanonicalSourceIdentityUnavailable,
    DuplicateOriginalPreferenceIndex,
    AdapterCorrelationInconsistent,
};

struct UpgradeAllExplicitSourceAdapterIssue {
    UpgradeAllExplicitSourceAdapterIssueKind kind =
        UpgradeAllExplicitSourceAdapterIssueKind::
            AdapterCorrelationInconsistent;
    std::optional<std::size_t> adapter_index;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::string> preference_package_name;
    std::string diagnostic;
};

// PR2のprepared source intentとPR1 plannerのdense indexを同じrecordへ固定する。
// original_preference_indexはdirectory snapshot上の位置であり、adapter_indexとは
// 一致するとは限らない。
struct UpgradeAllExplicitSourceAdapterEntry {
    std::size_t adapter_index = 0;
    std::size_t original_preference_index = 0;
    std::string preference_package_name;
    std::optional<std::string> canonical_source_identity_key;
    std::optional<std::string> resolved_package_base;
    std::vector<std::string> affected_package_names;
    UpgradeAllExplicitSourceIdentity planner_identity;
};

struct UpgradeAllExplicitSourceIdentityAdapter {
    std::vector<UpgradeAllExplicitSourceAdapterEntry> entries;
    std::vector<UpgradeAllExplicitSourceAdapterIssue> issues;

    bool is_valid() const noexcept;
    std::vector<UpgradeAllExplicitSourceIdentity> planner_identities() const;
};

UpgradeAllExplicitSourceIdentityAdapter
adapt_prepared_source_identities_for_upgrade_all(
    const SystemSourceUpgradePreparedSnapshot& source_snapshot);

enum class UpgradeAllOperationWarningKind {
    RegisteredSourcePreference,
    AurPreparation,
};

struct UpgradeAllOperationWarning {
    UpgradeAllOperationWarningKind kind =
        UpgradeAllOperationWarningKind::RegisteredSourcePreference;
    UpgradeAllOperationPhase phase = UpgradeAllOperationPhase::Preparation;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::string> package_name;
    std::string diagnostic;
};

enum class UpgradeAllOperationIssueKind {
    ExplicitSourceAdapterInvalid,
    OptionSnapshotMismatch,
    SourceSnapshotMismatch,
    ExplicitSourceCorrelationInconsistent,
    PreparedCapabilityConsumed,
    SystemSourceExecutionFailedUnexpectedly,
    SystemSourcePhaseIncomplete,
    ForeignInventoryConfigurationFailed,
    ForeignInventoryReadFailed,
    CacheAuthorityInvalid,
    AurQueryFailed,
    FilteredAurPreparationFailed,
    FilteredAurExecutionFailed,
    DuplicateExclusionCorrelationInconsistent,
    ExternalSatisfactionCorrelationInconsistent,
    UnknownFailure,
};

struct UpgradeAllOperationIssue {
    UpgradeAllOperationIssueKind kind =
        UpgradeAllOperationIssueKind::UnknownFailure;
    UpgradeAllOperationPhase phase = UpgradeAllOperationPhase::Preparation;
    std::optional<std::size_t> adapter_index;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::size_t> original_query_plan_index;
    std::optional<std::size_t> build_plan_order_index;
    std::optional<std::string> package_name;
    std::optional<PackageMetadataFailure> package_metadata_failure;
    std::optional<xdg_paths::ResolutionFailure>
        cache_resolution_failure;
    std::optional<xdg_directory_safety::PreparationFailure>
        cache_preparation_failure;
    std::optional<TrustedCacheFailure> trusted_cache_failure;
    std::string diagnostic;
};

struct UpgradeAllOperationDiagnostic {
    UpgradeAllOperationPhase phase = UpgradeAllOperationPhase::Preparation;
    bool stops_execution = false;
    std::string diagnostic;
};

struct UpgradeAllOperationPreparedSnapshot {
    SystemSourceUpgradePreparedSnapshot system_source;
    UpgradeAllExplicitSourceIdentityAdapter explicit_source_adapter;
    std::vector<UpgradeAllOperationWarning> warnings;
};

// Aggregate prepared capabilityが所有するread-only projection seam。
// nested system/source authorityをborrowし、outer execution/cache capabilityは
// unified observationへ公開しない。
class UpgradeAllOperationProjectionAuthority final {
public:
    UpgradeAllOperationProjectionAuthority(
        const UpgradeAllOperationProjectionAuthority&) = delete;
    UpgradeAllOperationProjectionAuthority& operator=(
        const UpgradeAllOperationProjectionAuthority&) = delete;
    UpgradeAllOperationProjectionAuthority(
        UpgradeAllOperationProjectionAuthority&&) noexcept = default;
    UpgradeAllOperationProjectionAuthority& operator=(
        UpgradeAllOperationProjectionAuthority&&) noexcept = default;
    ~UpgradeAllOperationProjectionAuthority() = default;

    [[nodiscard]] const UpgradeAllOperationPreparedSnapshot& snapshot()
        const noexcept {
        return snapshot_.get();
    }
    [[nodiscard]] const SystemSourceUpgradeProjectionAuthority&
    system_source() const noexcept {
        return system_source_.get();
    }

private:
    UpgradeAllOperationProjectionAuthority(
        const UpgradeAllOperationPreparedSnapshot& snapshot,
        const SystemSourceUpgradeProjectionAuthority& system_source)
        : snapshot_(snapshot), system_source_(system_source) {
    }

    std::reference_wrapper<const UpgradeAllOperationPreparedSnapshot> snapshot_;
    std::reference_wrapper<const SystemSourceUpgradeProjectionAuthority>
        system_source_;

    friend class PreparedUpgradeAllOperation;
    friend struct UnifiedPlanProjectionTestAccess;
};

struct UpgradeAllForeignInventoryPhaseResult {
    UpgradeAllForeignInventoryPhaseStatus status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    std::optional<UpgradeAllNotAttemptedReason> not_attempted_reason;
    std::optional<PacmanRepositoryConfiguration> repository_configuration;
    ForeignPackageInventory inventory;
    std::optional<PackageMetadataFailure> failure;
    std::optional<std::string> diagnostic;
};

struct UpgradeAllAurPhaseResult {
    UpgradeAllAurPhaseStatus status =
        UpgradeAllAurPhaseStatus::NotAttempted;
    std::optional<UpgradeAllNotAttemptedReason> not_attempted_reason;
    std::optional<FilteredAurUpdateExecutionResult> operation_result;
    std::optional<std::string> diagnostic;
};

// duplicate targetはplanner-local indexとoriginal query indexの両方を保持する。
struct UpgradeAllDuplicateExcludedAurTarget {
    std::size_t planner_target_index = 0;
    std::size_t original_query_plan_index = 0;
    UpgradeAllTargetPlanEntry planner_entry;
    AurUpdatePlanEntry query_entry;
};

// root-roleの正本はPR3 correlationであり、affected_rootsとrolesをzipしない。
struct UpgradeAllExternallySatisfiedAurBuildUnit {
    AurUpdateExternallySatisfiedBuildUnit operation_unit;
    std::vector<FilteredAurUpdateBuildUnitRootCorrelation> root_correlations;
};

enum class UpgradeAllCrossSourceVersionLockCorrelationFailureKind {
    ResourceExhaustion,
    UnexpectedException,
    UnknownException,
};

struct UpgradeAllCrossSourceVersionLockCorrelationFailure {
    UpgradeAllCrossSourceVersionLockCorrelationFailureKind kind =
        UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
            UnknownException;
    std::optional<std::string> diagnostic;
};

// Secondary evidence collected only after a system-upgrade failure. The
// observation status remains the Complete/Partial/Failed authority. Indices
// identify only possible candidate correlations; they neither identify the
// pacman transaction target nor confirm the cause of its failure.
struct UpgradeAllCrossSourceVersionLockCorrelationResult {
    std::optional<CrossSourceVersionLockObservationResult> observation;
    std::vector<CrossSourceVersionLockAssessment> assessments;
    std::vector<std::size_t> possible_blocker_assessment_indices;
    std::optional<UpgradeAllCrossSourceVersionLockCorrelationFailure> failure;
};

struct UpgradeAllOperationResult {
    UpgradeAllOperationStatus status =
        UpgradeAllOperationStatus::InconsistentResult;
    UpgradeAllOperationPhase stopped_phase =
        UpgradeAllOperationPhase::Preparation;
    UpgradeAllOperationPreparedSnapshot prepared_snapshot;
    SystemSourceUpgradeResult system_source;
    UpgradeAllForeignInventoryPhaseResult foreign_inventory;
    UpgradeAllAurPhaseResult aur;
    std::vector<UpgradeAllDuplicateExcludedAurTarget>
        duplicate_excluded_aur_targets;
    std::vector<UpgradeAllExternallySatisfiedAurBuildUnit>
        externally_satisfied_aur_build_units;
    std::optional<UpgradeAllCrossSourceVersionLockCorrelationResult>
        cross_source_version_lock_correlation;
    std::vector<UpgradeAllOperationWarning> warnings;
    std::vector<UpgradeAllOperationIssue> issues;
    std::vector<UpgradeAllOperationDiagnostic> diagnostics;

    bool is_success() const noexcept;
    PackageStateChange package_state_change() const noexcept;
    bool has_partial_completion() const noexcept;
    bool has_not_attempted_phase() const noexcept;
    bool has_cleanup_failure() const noexcept;
    bool has_query_failure() const noexcept;
    bool has_planning_issue() const noexcept;
    bool has_duplicate_exclusions() const noexcept;
    bool has_external_satisfaction() const noexcept;
    bool has_inconsistency() const noexcept;
};

class PreparedUpgradeAllOperation final {
    struct Impl;

    explicit PreparedUpgradeAllOperation(std::unique_ptr<Impl> impl) noexcept;

    friend struct UpgradeAllOperationPreparationAccess;
    friend UpgradeAllOperationResult execute_prepared_upgrade_all_operation(
        PreparedUpgradeAllOperation prepared,
        const AppConfig& config);

    std::unique_ptr<Impl> impl_;
    std::optional<UpgradeAllOperationProjectionAuthority>
        projection_authority_;

public:
    PreparedUpgradeAllOperation(const PreparedUpgradeAllOperation&) = delete;
    PreparedUpgradeAllOperation& operator=(
        const PreparedUpgradeAllOperation&) = delete;
    PreparedUpgradeAllOperation(PreparedUpgradeAllOperation&&) noexcept;
    PreparedUpgradeAllOperation& operator=(PreparedUpgradeAllOperation&&) =
        delete;
    ~PreparedUpgradeAllOperation() noexcept;

    bool is_valid() const noexcept;
    const UpgradeAllOperationPreparedSnapshot* snapshot() const noexcept;
    const UpgradeAllOperationProjectionAuthority* projection_authority()
        const noexcept;

#ifdef MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS
    void make_source_snapshot_inconsistent_for_test();
    void make_explicit_source_correlation_inconsistent_for_test();
    void make_nested_system_source_correlation_inconsistent_for_test();
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
    void set_nested_system_source_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint point,
        bool unknown_exception = false);
#endif
#endif
};

// Fresh repository inventory, AUR query, and filtered production preflight.
// The exact authority is shared by dry-run projection and actual execution;
// cache activation remains owned by actual execution.
class PreparedUpgradeAllAurPreflight final {
    PreparedUpgradeAllAurPreflight() = default;

    void prepare_foreign_inventory_stage();
    void prepare_aur_query_stage();
    void prepare_filtered_operation_stage(
        const UpgradeAllOperationPreparedSnapshot& prepared,
        const AppConfig& config,
        std::optional<ValidatedCacheRoot> cache_root);

    UpgradeAllForeignInventoryPhaseResult foreign_inventory_;
    std::optional<AurUpdateQueryResult> aur_query_result_;
    std::optional<PreparedFilteredAurUpdateOperation> filtered_operation_;
    std::vector<UpgradeAllOperationIssue> issues_;
    UpgradeAllOperationPhase stopped_phase_ = UpgradeAllOperationPhase::None;
    std::optional<std::string> diagnostic_;

    friend PreparedUpgradeAllAurPreflight
    prepare_upgrade_all_aur_preflight(
        const UpgradeAllOperationPreparedSnapshot& prepared,
        const AppConfig& config);
    friend UpgradeAllOperationResult execute_prepared_upgrade_all_operation(
        PreparedUpgradeAllOperation prepared,
        const AppConfig& config);

public:
    PreparedUpgradeAllAurPreflight(
        const PreparedUpgradeAllAurPreflight&) = delete;
    PreparedUpgradeAllAurPreflight& operator=(
        const PreparedUpgradeAllAurPreflight&) = delete;
    PreparedUpgradeAllAurPreflight(
        PreparedUpgradeAllAurPreflight&&) noexcept = default;
    PreparedUpgradeAllAurPreflight& operator=(
        PreparedUpgradeAllAurPreflight&&) = delete;
    ~PreparedUpgradeAllAurPreflight() noexcept = default;

    [[nodiscard]] bool has_filtered_operation() const noexcept {
        return filtered_operation_.has_value();
    }
    [[nodiscard]] UpgradeAllOperationPhase stopped_phase() const noexcept {
        return stopped_phase_;
    }
    [[nodiscard]] const UpgradeAllForeignInventoryPhaseResult&
    foreign_inventory() const noexcept {
        return foreign_inventory_;
    }
    [[nodiscard]] const AurUpdateQueryResult* aur_query_result()
        const noexcept {
        if(filtered_operation_.has_value()) {
            return &filtered_operation_->original_query_result();
        }
        return aur_query_result_.has_value()
                   ? &aur_query_result_.value()
                   : nullptr;
    }
    [[nodiscard]] const AurUpdateExecutionPreflight* aur_preflight()
        const noexcept {
        return filtered_operation_.has_value()
                   ? &filtered_operation_->execution_preflight()
                   : nullptr;
    }
    [[nodiscard]] const PreparedFilteredAurUpdateOperation*
    filtered_operation() const noexcept {
        return filtered_operation_.has_value()
                   ? &filtered_operation_.value()
                   : nullptr;
    }
    [[nodiscard]] const std::vector<UpgradeAllOperationIssue>& issues()
        const noexcept {
        return issues_;
    }
    [[nodiscard]] const std::optional<std::string>& diagnostic()
        const noexcept {
        return diagnostic_;
    }
};

// blocked resultとexecutable capabilityを同時に返さないsum type。
using UpgradeAllOperationPreparation = std::variant<
    PreparedUpgradeAllOperation,
    UpgradeAllOperationResult>;

UpgradeAllOperationPreparation prepare_upgrade_all_operation(
    const AppConfig& config);

PreparedUpgradeAllAurPreflight prepare_upgrade_all_aur_preflight(
    const UpgradeAllOperationPreparedSnapshot& prepared,
    const AppConfig& config);

// by-value consumeによりouterとnested capabilityを最初のsystem mutation前に
// invalid化し、replayをtyped resultへ変換する。
UpgradeAllOperationResult execute_prepared_upgrade_all_operation(
    PreparedUpgradeAllOperation prepared,
    const AppConfig& config);
