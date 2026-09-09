#pragma once

#include "dependency_plan.hpp"
#include "package_metadata.hpp"
#include "source_install.hpp"
#include "source_environment.hpp"
#include "source_preference.hpp"
#include "trusted_cache.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct AppConfig;

enum class SystemSourceUpgradePhase {
    None,
    Preparation,
    System,
    RegisteredSource,
};

enum class SystemUpgradePhaseStatus {
    NotAttempted,
    Completed,
    Failed,
};

enum class RegisteredSourceUpgradeStatus {
    Updated,
    NoChange,
    Failed,
    UpdatedCleanupFailed,
    NoChangeCleanupFailed,
    NotAttempted,
    Unsupported,
    Incomplete,
};

enum class RegisteredSourceUpgradeFailureKind {
    None,
    InvalidPreferenceName,
    PreferenceUnavailable,
    PackageMetadataUnavailable,
    CacheAuthorityFailure,
    BuildOrInstallFailed,
    CleanupFailedAfterPackageTransaction,
    UpdateStatusUnknownSkipped,
    AuthoritativeExecutionIncomplete,
    DevelRequiresCheckSkipped,
    PriorPhaseStopped,
    UnknownException,
};

enum class SystemSourceUpgradeStatus {
    Completed,
    BlockedBeforeMutation,
    StoppedOnSystemFailure,
    StoppedOnSourceFailure,
    StoppedAfterSourceCleanupFailure,
    InconsistentResult,
};

enum class SystemSourceUpgradeIssueKind {
    PreferenceEnumerationUnavailable,
    PreferenceUnavailable,
    SourceIdentityResolutionFailed,
    SourceWorkItemPreparationFailed,
    SourceInvocationPreparationFailed,
    SourceBaselineSnapshotUnavailable,
    SystemPackageSnapshotUnavailable,
    PostSystemSourceSnapshotUnavailable,
    CacheAuthorityInvalid,
    InvalidPreferenceName,
    OptionSnapshotMismatch,
    PreparedCorrelationInconsistent,
    PreparedCapabilityConsumed,
    UnknownPreparationFailure,
};

enum class SystemSourceUpgradeIssueImpact {
    ObservabilityOnly,
    AffectsSuccess,
    BlocksExecution,
};

enum class SystemSourceUpgradeWarningKind {
    SourcePreference,
    InvalidPreferenceName,
};

enum class SystemSourceUpgradeEventKind {
    LoadingSourcePreference,
    SourcePreferenceWarning,
    SystemUpgradeStarting,
    CheckingSourcePackages,
    InvalidPreferenceWarning,
};

#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
enum class SystemSourceUpgradeUnexpectedExceptionPoint {
    SystemPhaseStarted,
    SystemPhaseCompleted,
    SourceWorkItemStarted,
    SourceResultRecorded,
};
#endif

struct SystemSourceUpgradeOptionSnapshot {
    bool no_edit = false;
    bool no_diff = false;
    bool no_confirm = false;
    bool rebuild = false;
    bool clean_build = false;
    bool rm_deps = false;
    std::string editor;
    // Registered-source/AUR update artifact installはproductionで常に
    // --neededなし。projectionもこのprepared policyを正本にする。
    bool needed = false;
};

// system mutation前に確定した利用者intentとsource identity。
// environmentはstrict readerが全byteを読めた場合だけ保持する。
struct RegisteredSourcePreferenceSnapshot {
    std::size_t original_preference_index = 0;
    std::string preference_package_name;
    std::filesystem::path entry_path;
    std::optional<SourceBuildEnvironment> environment;
    std::optional<std::string> canonical_source_identity_key;
    std::optional<std::string> resolved_package_base;
    std::vector<std::string> preference_load_warnings;
    std::optional<SourceBuildSourceKind> source_kind = std::nullopt;
    std::optional<ResolvedRepositorySourceBuildIdentity>
        repository_identity = std::nullopt;
    std::optional<RequiredTargetProvenance> required_target_provenance =
        std::nullopt;
    std::optional<ArtifactLifecycleIntent> artifact_lifecycle_intent =
        std::nullopt;
};

struct SystemSourceUpgradePreparedSnapshot {
    bool preference_root_exists = false;
    SystemSourceUpgradeOptionSnapshot options;
    std::vector<RegisteredSourcePreferenceSnapshot> registered_sources;
};

struct SystemSourceUpgradeIssue {
    SystemSourceUpgradeIssueKind kind =
        SystemSourceUpgradeIssueKind::UnknownPreparationFailure;
    SystemSourceUpgradeIssueImpact impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    SystemSourceUpgradePhase phase = SystemSourceUpgradePhase::Preparation;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::string> preference_package_name;
    std::optional<std::string> resolved_package_base;
    std::optional<SourcePreferenceFailure> source_preference_failure;
    std::optional<PackageMetadataFailure> package_metadata_failure;
    std::optional<xdg_paths::ResolutionFailure> cache_resolution_failure;
    std::optional<xdg_directory_safety::PreparationFailure>
        cache_preparation_failure;
    std::optional<TrustedCacheFailure> trusted_cache_failure;
    std::string diagnostic;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure;
};

// Prepared source invocation内のactual work itemから、source identityと
// pre-build required targetだけをborrowする。command/cache/executor capabilityは
// このviewへ公開しない。
class PreparedSystemSourceWorkReference final {
public:
    PreparedSystemSourceWorkReference(
        const PreparedSystemSourceWorkReference&) = delete;
    PreparedSystemSourceWorkReference& operator=(
        const PreparedSystemSourceWorkReference&) = delete;
    PreparedSystemSourceWorkReference(
        PreparedSystemSourceWorkReference&&) noexcept = default;
    PreparedSystemSourceWorkReference& operator=(
        PreparedSystemSourceWorkReference&&) noexcept = default;
    ~PreparedSystemSourceWorkReference() = default;

    [[nodiscard]] const RegisteredSourcePreferenceSnapshot& source()
        const noexcept {
        return source_.get();
    }
    [[nodiscard]] const std::vector<RequiredPackageArtifactTarget>&
    required_targets() const noexcept {
        return required_targets_.get();
    }
    [[nodiscard]] const std::string& requested_package_name() const noexcept {
        return requested_package_name_.get();
    }
    [[nodiscard]] const std::string& checkout_package_base() const noexcept {
        return checkout_package_base_.get();
    }
    [[nodiscard]] RequiredTargetProvenance required_target_provenance()
        const noexcept {
        return required_target_provenance_;
    }
    [[nodiscard]] ArtifactLifecycleIntent artifact_lifecycle_intent()
        const noexcept {
        return artifact_lifecycle_intent_;
    }
    [[nodiscard]] const std::optional<
        ResolvedRepositorySourceBuildIdentity>&
    repository_identity() const noexcept {
        return repository_identity_.get();
    }
    [[nodiscard]] bool uses_system_update_baseline() const noexcept {
        return uses_system_update_baseline_;
    }
    [[nodiscard]] bool needed() const noexcept {
        return needed_;
    }
    [[nodiscard]] bool only_if_updated() const noexcept {
        return only_if_updated_;
    }
    [[nodiscard]] const std::optional<std::vector<std::string>>&
    configured_repository_order() const noexcept {
        return configured_repository_order_.get();
    }
    [[nodiscard]] const std::vector<ProvidedDependency>&
    selected_repository_providers() const noexcept {
        return selected_repository_providers_.get();
    }

private:
    PreparedSystemSourceWorkReference(
        const RegisteredSourcePreferenceSnapshot& source,
        const ProductionSourceBuildWorkItem& work_item)
        : source_(source),
          required_targets_(work_item.required_targets),
          requested_package_name_(work_item.request.package_name),
          checkout_package_base_(work_item.request.checkout_name),
          required_target_provenance_(
              work_item.required_target_provenance),
          artifact_lifecycle_intent_(
              work_item.artifact_lifecycle_intent),
          repository_identity_(work_item.repository_identity),
          uses_system_update_baseline_(
              work_item.uses_system_update_baseline),
          needed_(work_item.request.needed),
          only_if_updated_(work_item.request.only_if_updated),
          configured_repository_order_(
              work_item.configured_repository_order),
          selected_repository_providers_(
              work_item.selected_repository_providers) {
    }

    std::reference_wrapper<const RegisteredSourcePreferenceSnapshot> source_;
    std::reference_wrapper<
        const std::vector<RequiredPackageArtifactTarget>>
        required_targets_;
    std::reference_wrapper<const std::string> requested_package_name_;
    std::reference_wrapper<const std::string> checkout_package_base_;
    RequiredTargetProvenance required_target_provenance_ =
        RequiredTargetProvenance::Unspecified;
    ArtifactLifecycleIntent artifact_lifecycle_intent_ =
        ArtifactLifecycleIntent::Unspecified;
    std::reference_wrapper<const std::optional<
        ResolvedRepositorySourceBuildIdentity>>
        repository_identity_;
    bool uses_system_update_baseline_ = false;
    bool needed_ = false;
    bool only_if_updated_ = false;
    std::reference_wrapper<
        const std::optional<std::vector<std::string>>>
        configured_repository_order_;
    std::reference_wrapper<const std::vector<ProvidedDependency>>
        selected_repository_providers_;

    friend class PreparedSystemSourceUpgrade;
    friend struct UnifiedPlanProjectionTestAccess;
};

// PreparedSystemSourceUpgradeが所有するread-only projection seam。
// すべての参照先は同じprepared capability内にあり、そのcapabilityをmoveまたは
// destroyする前に、このauthorityをborrowするprojectionを破棄する。
class SystemSourceUpgradeProjectionAuthority final {
public:
    SystemSourceUpgradeProjectionAuthority(
        const SystemSourceUpgradeProjectionAuthority&) = delete;
    SystemSourceUpgradeProjectionAuthority& operator=(
        const SystemSourceUpgradeProjectionAuthority&) = delete;
    SystemSourceUpgradeProjectionAuthority(
        SystemSourceUpgradeProjectionAuthority&&) noexcept = default;
    SystemSourceUpgradeProjectionAuthority& operator=(
        SystemSourceUpgradeProjectionAuthority&&) noexcept = default;
    ~SystemSourceUpgradeProjectionAuthority() = default;

    [[nodiscard]] const SystemSourceUpgradePreparedSnapshot& snapshot()
        const noexcept {
        return snapshot_.get();
    }
    [[nodiscard]] const BuildPlan* aur_invocation_plan() const noexcept {
        return aur_invocation_plan_.has_value()
                   ? &aur_invocation_plan_->get()
                   : nullptr;
    }
    [[nodiscard]] const std::vector<SystemSourceUpgradeIssue>& issues()
        const noexcept {
        return issues_.get();
    }
    [[nodiscard]] const std::vector<PreparedSystemSourceWorkReference>&
    source_work_items() const noexcept {
        return source_work_items_;
    }

private:
    SystemSourceUpgradeProjectionAuthority(
        const SystemSourceUpgradePreparedSnapshot& snapshot,
        const BuildPlan* aur_invocation_plan,
        const std::vector<SystemSourceUpgradeIssue>& issues,
        std::vector<PreparedSystemSourceWorkReference> source_work_items)
        : snapshot_(snapshot),
          aur_invocation_plan_(
              aur_invocation_plan == nullptr
                  ? std::nullopt
                  : std::optional<std::reference_wrapper<const BuildPlan>>{
                        std::cref(*aur_invocation_plan)}),
          issues_(issues), source_work_items_(std::move(source_work_items)) {
    }

    std::reference_wrapper<const SystemSourceUpgradePreparedSnapshot> snapshot_;
    std::optional<std::reference_wrapper<const BuildPlan>>
        aur_invocation_plan_;
    std::reference_wrapper<const std::vector<SystemSourceUpgradeIssue>> issues_;
    std::vector<PreparedSystemSourceWorkReference> source_work_items_;

    friend class PreparedSystemSourceUpgrade;
    friend struct UnifiedPlanProjectionTestAccess;
};

struct SystemSourceUpgradeWarning {
    SystemSourceUpgradeWarningKind kind =
        SystemSourceUpgradeWarningKind::SourcePreference;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::string> preference_package_name;
    std::optional<std::filesystem::path> entry_path;
    std::string diagnostic;
};

struct SystemSourceUpgradeDiagnostic {
    SystemSourceUpgradePhase phase = SystemSourceUpgradePhase::Preparation;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::string> preference_package_name;
    std::optional<std::string> resolved_package_base;
    bool stops_execution = false;
    std::string diagnostic;
};

struct SystemSourceUpgradeEvent {
    SystemSourceUpgradeEventKind kind =
        SystemSourceUpgradeEventKind::SystemUpgradeStarting;
    std::optional<std::size_t> original_preference_index;
    std::optional<std::string> preference_package_name;
    std::optional<std::filesystem::path> entry_path;
    std::string diagnostic;
};

using SystemSourceUpgradeEventObserver =
    std::function<void(const SystemSourceUpgradeEvent& event)>;

struct SystemUpgradePhaseResult {
    SystemUpgradePhaseStatus status = SystemUpgradePhaseStatus::NotAttempted;
    PackageStateChange package_state_change = PackageStateChange::NoChange;
    std::optional<int> command_exit_status;
    std::optional<PackageMetadataFailure> before_snapshot_failure;
    std::optional<PackageMetadataFailure> after_snapshot_failure;
    std::optional<std::string> diagnostic;
};

enum class RegisteredSourceBuildFailureCategory {
    Build,
    ArtifactValidation,
    ArtifactIdentity,
    InstallPreparation,
    InstallTransaction,
    Other,
};

struct RegisteredSourceBuildFailureSnapshot {
    RegisteredSourceBuildFailureCategory category =
        RegisteredSourceBuildFailureCategory::Other;
    std::string diagnostic;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure;
};

struct RegisteredSourcePackageTransactionFailureSnapshot {
    PackageBaseArtifactInstallTransactionFailureKind category =
        PackageBaseArtifactInstallTransactionFailureKind::UnknownException;
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts;
    std::optional<int> exit_code;
    std::string diagnostic;
};

enum class RegisteredSourceExecutionCorrelationFailureReason {
    PackageBaseMismatch,
    MissingSelectedChild,
    ExtraSelectedChild,
    SelectedArtifactIdentityMismatch,
    EmptySelectedArtifactVersion,
    DesiredInstallReasonMismatch,
    UnexpectedSkippedAsNeeded,
    UnknownChildOutcome,
    InvalidUnselectedArtifactIdentity,
    SelectedAndUnselectedIdentityOverlap,
    DuplicateUnselectedArtifactIdentity,
};

struct RegisteredSourceExecutionCorrelationFailure {
    RegisteredSourceExecutionCorrelationFailureReason reason =
        RegisteredSourceExecutionCorrelationFailureReason::
            PackageBaseMismatch;
    std::optional<std::size_t> required_child_index;
    std::optional<std::string> package_name;
    std::string diagnostic;
};

struct RegisteredSourcePackageBaseExecutionSnapshot {
    std::string package_base;
    PackageBaseSourceBuildSelectedResult selected_child;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
};

using RegisteredSourceUpgradeFailureDetail = std::variant<
    std::monostate,
    PackageBaseArtifactIdentitySelectionFailure,
    MixedPackageBaseInstallReasonUnsupported,
    PackageMetadataFailure,
    RegisteredSourceBuildFailureSnapshot,
    RegisteredSourcePackageTransactionFailureSnapshot,
    RegisteredSourceExecutionCorrelationFailure>;

struct RegisteredSourceUpgradeResult {
    std::shared_ptr<const AurUpdateQueryResult> devel_update_query = nullptr;
    std::optional<ConfirmationDecisionOrigin> devel_rebuild_confirmation = std::nullopt;
    std::optional<ReviewedDevelExecutionSnapshot> devel_execution = std::nullopt;
    std::size_t original_preference_index = 0;
    std::string preference_package_name;
    std::optional<std::string> canonical_source_identity_key;
    std::optional<std::string> resolved_package_base;
    RegisteredSourceUpgradeStatus status =
        RegisteredSourceUpgradeStatus::NotAttempted;
    RegisteredSourceUpgradeFailureKind failure_kind =
        RegisteredSourceUpgradeFailureKind::PriorPhaseStopped;
    PackageStateChange package_state_change = PackageStateChange::NoChange;
    std::optional<std::string> diagnostic;
    std::optional<std::string> cleanup_diagnostic;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome;
    std::optional<RegisteredSourcePackageBaseExecutionSnapshot>
        package_base_execution;
    // correlation failure時もsafe attempt evidenceを別slotで保持する。
    std::optional<RegisteredSourcePackageTransactionFailureSnapshot>
        package_transaction_failure;
    RegisteredSourceUpgradeFailureDetail failure_detail = std::monostate{};

    RegisteredSourceUpgradeResult() = default;

    // Slice 4以前のcoarse result構築は維持し、typed detailだけをdefaultで足す。
    RegisteredSourceUpgradeResult(
        std::size_t original_index,
        std::string package_name,
        std::optional<std::string> source_identity_key,
        std::optional<std::string> package_base,
        RegisteredSourceUpgradeStatus source_status,
        RegisteredSourceUpgradeFailureKind source_failure_kind,
        PackageStateChange state_change,
        std::optional<std::string> source_diagnostic,
        std::optional<std::string> source_cleanup_diagnostic);
};

struct SystemSourceUpgradeResult {
    SystemSourceUpgradeStatus status =
        SystemSourceUpgradeStatus::InconsistentResult;
    SystemSourceUpgradePhase stopped_phase = SystemSourceUpgradePhase::None;
    SystemSourceUpgradePreparedSnapshot prepared_snapshot;
    SystemUpgradePhaseResult system;
    SelectedRepositoryProviderTransactionResult
        selected_repository_provider_transaction;
    std::vector<RegisteredSourceUpgradeResult> registered_source_results;
    std::vector<SystemSourceUpgradeWarning> warnings;
    std::vector<SystemSourceUpgradeIssue> issues;
    std::vector<SystemSourceUpgradeDiagnostic> diagnostics;
    // Invocation-wide provider/dependency preflight authority. It is moved
    // from preparation and is never reconstructed per root for observation.
    std::optional<BuildPlan> aur_invocation_plan = std::nullopt;

    bool is_success() const noexcept;
    PackageStateChange package_state_change() const noexcept;
    bool definitely_changed_package_state() const noexcept;
    bool has_partial_completion() const noexcept;
    bool has_not_attempted_sources() const noexcept;
    bool has_cleanup_failure() const noexcept;
    bool has_blocking_issue() const noexcept;
    std::optional<std::string> failure_diagnostic() const;
};

class PreparedSystemSourceUpgrade final {
    struct Impl;

    explicit PreparedSystemSourceUpgrade(std::unique_ptr<Impl> impl);

    friend struct SystemSourceUpgradePreparationAccess;
    friend SystemSourceUpgradeResult execute_prepared_system_source_upgrade(
        PreparedSystemSourceUpgrade prepared,
        const AppConfig& config,
        const SystemSourceUpgradeEventObserver& observer,
        std::optional<ValidatedCacheRoot> shared_cache_root);

    std::unique_ptr<Impl> impl_;
    std::optional<SystemSourceUpgradeProjectionAuthority>
        projection_authority_;

public:
    PreparedSystemSourceUpgrade(const PreparedSystemSourceUpgrade&) = delete;
    PreparedSystemSourceUpgrade& operator=(const PreparedSystemSourceUpgrade&) = delete;
    PreparedSystemSourceUpgrade(PreparedSystemSourceUpgrade&&) noexcept;
    PreparedSystemSourceUpgrade& operator=(PreparedSystemSourceUpgrade&&) = delete;
    ~PreparedSystemSourceUpgrade() noexcept;

    bool is_valid() const noexcept;
    const SystemSourceUpgradePreparedSnapshot* snapshot() const noexcept;
    const BuildPlan* aur_invocation_plan() const noexcept;
    const std::vector<SystemSourceUpgradeIssue>& issues() const noexcept;
    const SystemSourceUpgradeProjectionAuthority* projection_authority()
        const noexcept;

#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
    void make_first_source_correlation_inconsistent_for_test();
    void set_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint point,
        bool unknown_exception = false);
#endif
};

// blocked resultとexecutable capabilityを同時に返さないsum type。
// Preparation is read-only. Cache authority is acquired only by execution.
using SystemSourceUpgradePreparation = std::variant<
    PreparedSystemSourceUpgrade,
    SystemSourceUpgradeResult>;

SystemSourceUpgradePreparation prepare_system_source_upgrade(
    const AppConfig& config,
    const SystemSourceUpgradeEventObserver& observer = {});

// by-value consumeにより、呼び出し元capabilityをmutation前にinvalid化する。
// shared_cache_root keeps one actual-execution authority across the nested
// system/source and later upgrade-all AUR phases.
SystemSourceUpgradeResult execute_prepared_system_source_upgrade(
    PreparedSystemSourceUpgrade prepared,
    const AppConfig& config,
    const SystemSourceUpgradeEventObserver& observer = {},
    std::optional<ValidatedCacheRoot> shared_cache_root = std::nullopt);
