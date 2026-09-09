#pragma once

#include "artifact_install_plan.hpp"
#include "dependency_provider.hpp"
#include "dependency_plan.hpp"
#include "package_metadata.hpp"
#include "repository_query.hpp"
#include "separated_package_base_source_build.hpp"
#include "source_build.hpp"
#include "trusted_alpm_receipt_transport.hpp"
#include "trusted_cache.hpp"

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct AppConfig;
class LocalBuildPlan;
class LocalSourceBuildDependencyPreparation;
class RemoteAurCleanupCandidateCollector;
struct PreparedProductionSourceBuildInvocation;

PreparedProductionSourceBuildInvocation
prepare_local_source_build_dependency_invocation(
    LocalSourceBuildDependencyPreparation preparation,
    const ValidatedCacheRoot& cache_root,
    const AppConfig& config);

// Empty remote invocationをgeneric callerが捏造して通さないためのauthority。
// local rootを別ownerが保持するpreparationだけが生成できる。
class LocalSourceBuildInvocationAuthority final {
    LocalSourceBuildInvocationAuthority() noexcept = default;

    friend PreparedProductionSourceBuildInvocation
    prepare_local_source_build_dependency_invocation(
        LocalSourceBuildDependencyPreparation preparation,
        const ValidatedCacheRoot& cache_root,
        const AppConfig& config);

public:
    LocalSourceBuildInvocationAuthority(
        const LocalSourceBuildInvocationAuthority&) = default;
    LocalSourceBuildInvocationAuthority(
        LocalSourceBuildInvocationAuthority&&) noexcept = default;
    LocalSourceBuildInvocationAuthority& operator=(
        const LocalSourceBuildInvocationAuthority&) = default;
    LocalSourceBuildInvocationAuthority& operator=(
        LocalSourceBuildInvocationAuthority&&) noexcept = default;
    ~LocalSourceBuildInvocationAuthority() = default;
};

enum class SourceBuildSourceKind {
    Repository,
    Aur,
};

// PackageBaseだけを入力にcanonical key / official checkout URLを構成し、
// URL leafからPackageBaseを逆算させないclosed source identity。
class SourceCheckoutIdentity final {
public:
    [[nodiscard]] const std::string& package_base() const noexcept {
        return package_base_;
    }
    [[nodiscard]] const std::string& canonical_source_key() const noexcept {
        return canonical_source_key_;
    }
    [[nodiscard]] const std::string& git_url() const noexcept {
        return git_url_;
    }

    bool operator==(const SourceCheckoutIdentity&) const = default;

private:
    SourceCheckoutIdentity(
        SourceBuildSourceKind source_kind,
        std::string package_base)
        : package_base_(std::move(package_base)) {
        switch(source_kind) {
            case SourceBuildSourceKind::Repository:
                canonical_source_key_ = "repository:" + package_base_;
                git_url_ =
                    "https://gitlab.archlinux.org/archlinux/packaging/packages/" +
                    package_base_ + ".git";
                break;
            case SourceBuildSourceKind::Aur:
                canonical_source_key_ = "aur:" + package_base_;
                git_url_ = "https://aur.archlinux.org/" + package_base_ + ".git";
                break;
        }
    }

    std::string package_base_;
    std::string canonical_source_key_;
    std::string git_url_;

    friend class ResolvedRepositorySourceBuildIdentity;
    friend class ResolvedAurSourceBuildIdentity;
};

// strict repository exact observationと、それだけから導いたcheckout identityを
// 同じowned valueへ閉じ込める。requested childとPackageBaseは別fieldで保持する。
class ResolvedRepositorySourceBuildIdentity final {
public:
    explicit ResolvedRepositorySourceBuildIdentity(
        RepositoryPackagePresent exact_package)
        : exact_package_(std::move(exact_package)),
          checkout_(
              SourceBuildSourceKind::Repository,
              exact_package_.package_base) {
        if(exact_package_.repository_name.empty()) {
            throw std::invalid_argument(
                "Repository source-build identity has no repository name.");
        }
        if(exact_package_.configured_repository_order.has_value()) {
            const auto& order =
                exact_package_.configured_repository_order.value();
            if(exact_package_.configured_order >= order.size() ||
               order[exact_package_.configured_order] !=
                   exact_package_.repository_name) {
                throw std::invalid_argument(
                    "Repository source-build identity has inconsistent repository provenance.");
            }
        }
    }

    [[nodiscard]] const RepositoryPackagePresent& exact_package()
        const noexcept {
        return exact_package_;
    }
    [[nodiscard]] const std::string& requested_child() const noexcept {
        return exact_package_.package_name;
    }
    [[nodiscard]] const std::string& package_base() const noexcept {
        return exact_package_.package_base;
    }
    [[nodiscard]] const SourceCheckoutIdentity& checkout() const noexcept {
        return checkout_;
    }

    bool operator==(const ResolvedRepositorySourceBuildIdentity&) const =
        default;

private:
    RepositoryPackagePresent exact_package_;
    SourceCheckoutIdentity checkout_;
};

class ResolvedAurSourceBuildIdentity final {
public:
    ResolvedAurSourceBuildIdentity(
        std::string requested_name,
        std::string package_base)
        : requested_name_(std::move(requested_name)),
          checkout_(SourceBuildSourceKind::Aur, std::move(package_base)) {
    }

    [[nodiscard]] const std::string& requested_name() const noexcept {
        return requested_name_;
    }
    [[nodiscard]] const SourceCheckoutIdentity& checkout() const noexcept {
        return checkout_;
    }
    [[nodiscard]] bool has_distinct_package_base() const noexcept {
        return requested_name_ != checkout_.package_base();
    }

    bool operator==(const ResolvedAurSourceBuildIdentity&) const = default;

private:
    std::string requested_name_;
    SourceCheckoutIdentity checkout_;
};

// Resolver result自体もsource-specific alternativeへ閉じ、source kindと
// unrelated stringsをcallerが別々に組み立てる余地を持たせない。
class ResolvedSourceBuildIdentity final {
public:
    explicit ResolvedSourceBuildIdentity(
        ResolvedRepositorySourceBuildIdentity repository)
        : source_(std::move(repository)) {
    }
    explicit ResolvedSourceBuildIdentity(
        ResolvedAurSourceBuildIdentity aur)
        : source_(std::move(aur)) {
    }

    [[nodiscard]] SourceBuildSourceKind source_kind() const noexcept {
        return std::holds_alternative<ResolvedRepositorySourceBuildIdentity>(
                   source_)
                   ? SourceBuildSourceKind::Repository
                   : SourceBuildSourceKind::Aur;
    }
    [[nodiscard]] const std::string& requested_name() const noexcept {
        if(const auto* repository = repository_identity();
           repository != nullptr) {
            return repository->requested_child();
        }
        return std::get<ResolvedAurSourceBuildIdentity>(source_)
            .requested_name();
    }
    [[nodiscard]] const SourceCheckoutIdentity& checkout() const noexcept {
        if(const auto* repository = repository_identity();
           repository != nullptr) {
            return repository->checkout();
        }
        return std::get<ResolvedAurSourceBuildIdentity>(source_).checkout();
    }
    [[nodiscard]] const std::string& package_base() const noexcept {
        return checkout().package_base();
    }
    [[nodiscard]] const std::string& canonical_source_key() const noexcept {
        return checkout().canonical_source_key();
    }
    [[nodiscard]] const std::string& git_url() const noexcept {
        return checkout().git_url();
    }
    [[nodiscard]] bool has_distinct_package_base() const noexcept {
        if(const auto* aur =
               std::get_if<ResolvedAurSourceBuildIdentity>(&source_);
           aur != nullptr) {
            return aur->has_distinct_package_base();
        }
        return requested_name() != package_base();
    }
    [[nodiscard]] const ResolvedRepositorySourceBuildIdentity*
    repository_identity() const noexcept {
        return std::get_if<ResolvedRepositorySourceBuildIdentity>(&source_);
    }

    bool operator==(const ResolvedSourceBuildIdentity&) const = default;

private:
    std::variant<
        ResolvedRepositorySourceBuildIdentity,
        ResolvedAurSourceBuildIdentity>
        source_;
};

enum class RequiredTargetProvenance {
    Unspecified,
    RepositoryExactPackageProjection,
    AurBuildPlanProjection,
};

enum class ArtifactLifecycleIntent {
    Unspecified,
    SingularCompatibility,
    PackageBaseSet,
};

// production all-target preflightで確定し、mutation phaseまで保持する1 build unit。
// Artifact path/identity/install directiveはPR4 lifecycle内部でだけ生成する。
struct ProductionSourceBuildWorkItem {
    SourceBuildRequest request;
    // POLICY(#268): PackageBase execution unitのinstall対象とreasonはこのvectorが正本。
    // singular executor用request.package_nameはsize 1の場合だけ設定する。
    std::vector<RequiredPackageArtifactTarget> required_targets;
    // 利用者が選択したofficial providerを、対応するAUR build unitの
    // execution前dependency transactionまでtyped identityのまま保持する。
    std::vector<ProvidedDependency> selected_repository_providers;
    // AUR BuildPlan projectionだけが、artifact targetとselected repository
    // providerをexact edge indexへ束縛する。別routeはemptyのままであり、
    // 後段がpackage nameからedgeを再推定してはならない。
    std::vector<std::size_t> build_plan_dependency_edge_indices;
    std::vector<std::size_t> selected_repository_provider_edge_indices;
    // required targetのauthorityとartifact execution selectorを別domainで保持する。
    RequiredTargetProvenance required_target_provenance =
        RequiredTargetProvenance::Unspecified;
    ArtifactLifecycleIntent artifact_lifecycle_intent =
        ArtifactLifecycleIntent::Unspecified;
    // repository projectionだけがstrict exact observationを保持する。
    std::optional<ResolvedRepositorySourceBuildIdentity>
        repository_identity = std::nullopt;
    bool uses_system_update_baseline = false;
    // dependency/provider resolutionが問い合わせたconfiguration snapshot。
    // nulloptはrepository authority未問い合わせを表す。
    std::optional<std::vector<std::string>> configured_repository_order =
        std::nullopt;
    // 1 invocationでadoptした同一cache-root capabilityを全build unitが共有する。
    // static model生成中はemptyで、production invocation preparationだけが設定する。
    std::optional<ValidatedCacheRoot> cache_root;
};

// PacmanDatabasePathsはinvocationで1回だけ解決し、全build unitへvalueとして共有する。
struct PreparedProductionSourceBuildInvocation {
    std::vector<ProductionSourceBuildWorkItem> work_items;
    std::vector<ProvidedDependency> selected_repository_providers;
    PacmanDatabasePaths database_paths;
    std::optional<ValidatedCacheRoot> cache_root;
    std::optional<LocalSourceBuildInvocationAuthority>
        local_source_authority = std::nullopt;
};

enum class ReviewedSourceFatalStateObservationStatus {
    Inapplicable,
    Completed,
};

// Value-only request snapshot. The single-consumption reviewed-state slot is
// deliberately replaced with its completed/inapplicable observation status.
struct SourceBuildRequestObservation {
    std::string package_name;
    std::string checkout_name;
    std::string git_url;
    SourceBuildEnvironment custom_environment;
    SourceEnvironmentEmptyValuePolicy empty_value_policy =
        SourceEnvironmentEmptyValuePolicy::Omit;
    std::optional<SourceUpdateBaseline> update_baseline;
    std::optional<SourceInstalledSnapshot> installed_snapshot;
    bool only_if_updated = false;
    bool needed = false;
    std::optional<PackageBaseIdentity> aur_review_identity;
    ReviewedSourceFatalStateObservationStatus reviewed_state =
        ReviewedSourceFatalStateObservationStatus::Inapplicable;
};

// Value-only work-item projection. It intentionally has neither cache_root nor
// a ReviewedSourceFatalStatePreflightSlot and cannot be passed to an executor.
struct ProductionSourceBuildWorkItemObservation {
    SourceBuildRequestObservation request;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    std::vector<ProvidedDependency> selected_repository_providers;
    std::vector<std::size_t> build_plan_dependency_edge_indices;
    std::vector<std::size_t>
        selected_repository_provider_edge_indices;
    RequiredTargetProvenance required_target_provenance =
        RequiredTargetProvenance::Unspecified;
    ArtifactLifecycleIntent artifact_lifecycle_intent =
        ArtifactLifecycleIntent::Unspecified;
    std::optional<ResolvedRepositorySourceBuildIdentity>
        repository_identity;
    bool uses_system_update_baseline = false;
    std::optional<std::vector<std::string>>
        configured_repository_order;
};

// Read-only production preflight snapshot. Unlike
// PreparedProductionSourceBuildInvocation, this type is not accepted by any
// executor and never retains a cache capability.
struct ProductionSourceBuildPreparationObservation {
    std::vector<ProductionSourceBuildWorkItemObservation> work_items;
    std::vector<ProvidedDependency> selected_repository_providers;
    PacmanDatabasePaths database_paths;
};

enum class ProductionSourceBuildWorkItemStatus {
    NotAttempted,
    Succeeded,
    // Normal returned result; whole authoritative product is in devel_execution.
    AuthoritativePartial,
    Failed,
};

enum class ProductionSourceBuildFailureStage {
    Review,
    Build,
    ArtifactValidation,
    InstallPreparation,
    InstallTransaction,
    Cleanup,
    Other,
};

// Invocation entry exists before execution. Reached work advances the shared
// staged outcome; failure_exception retains the original typed error rather
// than reducing PackageMetadataError or transaction evidence to prose.
struct ProductionSourceBuildWorkItemOutcome {
    std::string package_base;
    ProductionSourceBuildWorkItemStatus status =
        ProductionSourceBuildWorkItemStatus::NotAttempted;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome;
    std::optional<ProductionSourceBuildFailureStage> failure_stage;
    std::optional<std::string> diagnostic;
    std::exception_ptr failure_exception;
    std::shared_ptr<const AurUpdateQueryResult> devel_update_query = nullptr;
    std::optional<ReviewedDevelExecutionSnapshot> devel_execution = std::nullopt;
};

struct ProductionSourceBuildInvocationResult {
    std::vector<ProductionSourceBuildWorkItemOutcome> work_items;

    bool is_success() const noexcept;
    // Partial is a normal result, but never complete command success.
    int command_exit_status() const noexcept;
};

// Normal source-build CLI preserves the complete preinitialized aggregate on
// failure while keeping the exact typed cause available for inspection.
class ProductionSourceBuildInvocationError final
    : public std::runtime_error {
    ProductionSourceBuildInvocationResult result_;
    std::size_t failed_work_item_index_ = 0;

public:
    ProductionSourceBuildInvocationError(
        ProductionSourceBuildInvocationResult result,
        std::size_t failed_work_item_index,
        const std::string& diagnostic);

    const ProductionSourceBuildInvocationResult& result() const noexcept {
        return result_;
    }

    std::size_t failed_work_item_index() const noexcept {
        return failed_work_item_index_;
    }

    ProductionSourceBuildFailureStage failure_stage() const;
    void rethrow_failure() const;
};

std::string format_production_source_build_invocation_failure(
    const ProductionSourceBuildInvocationError& error);

// Remote buildのroute identity、AUR plan（該当時）、cache未activateの
// production invocationを同じowned snapshotへ束ねる。dry-run projectionは
// このauthorityをborrowし、actual executionだけがcacheをactivateする。
struct PreparedRemoteSourceBuild {
    ResolvedSourceBuildIdentity source;
    std::optional<BuildPlan> aur_build_plan;
    PreparedProductionSourceBuildInvocation invocation;
};

struct RemoteSourceBuildPlanFailure {
    ResolvedSourceBuildIdentity source;
    BuildPlan plan;
};

using RemoteSourceBuildPreparation = std::variant<
    PreparedRemoteSourceBuild,
    RemoteSourceBuildPlanFailure>;

// LocalBuildPlanのlocal root unitをexecution consumerへ渡さず、remote AUR
// dependency unitと全edge由来のselected repository providerだけを保持する。
// Production callerはLocalBuildPlan projectionを経ずに生成できない。
class LocalSourceBuildDependencyPreparation final {
    std::vector<ProductionSourceBuildWorkItem> remote_work_items_;
    std::vector<ProvidedDependency> selected_repository_providers_;

    LocalSourceBuildDependencyPreparation(
        std::vector<ProductionSourceBuildWorkItem> remote_work_items,
        std::vector<ProvidedDependency> selected_repository_providers) noexcept;

    friend LocalSourceBuildDependencyPreparation
    prepare_local_source_build_dependencies(
        const LocalBuildPlan& plan,
        bool use_source_build_preferences,
        bool needed);
    friend void preflight_local_source_build_dependencies(
        LocalSourceBuildDependencyPreparation& preparation,
        const AppConfig& config);
    friend PreparedProductionSourceBuildInvocation
    prepare_local_source_build_dependency_invocation(
        LocalSourceBuildDependencyPreparation preparation,
        const ValidatedCacheRoot& cache_root,
        const AppConfig& config);

public:
    LocalSourceBuildDependencyPreparation(
        const LocalSourceBuildDependencyPreparation&) = delete;
    LocalSourceBuildDependencyPreparation(
        LocalSourceBuildDependencyPreparation&&) noexcept = default;
    LocalSourceBuildDependencyPreparation& operator=(
        const LocalSourceBuildDependencyPreparation&) = delete;
    LocalSourceBuildDependencyPreparation& operator=(
        LocalSourceBuildDependencyPreparation&&) noexcept = default;
    ~LocalSourceBuildDependencyPreparation() = default;

    const std::vector<ProductionSourceBuildWorkItem>& remote_work_items()
        const noexcept;
    const std::vector<ProvidedDependency>& selected_repository_providers()
        const noexcept;

#ifdef MOGUET_ENABLE_TEST_OVERRIDES
    static LocalSourceBuildDependencyPreparation
    make_for_production_source_build_test(
        std::vector<ProductionSourceBuildWorkItem> remote_work_items,
        std::vector<ProvidedDependency> selected_repository_providers);
#endif
};

enum class SelectedRepositoryProviderTransactionStatus {
    NotRequired,
    BlockedBeforeExecution,
    Succeeded,
    Failed,
    OutcomeUnknown,
};

// provider transactionをsource work-itemへ誤帰属させず、selected identityと
// package-stateの断言可能範囲をinvocation消費後もowned snapshotとして残す。
struct SelectedRepositoryProviderTransactionResult {
    SelectedRepositoryProviderTransactionStatus status =
        SelectedRepositoryProviderTransactionStatus::NotRequired;
    std::vector<ProvidedDependency> selected_providers;
    PackageStateChange package_state_change = PackageStateChange::NoChange;
    std::optional<int> command_exit_status;
    std::optional<std::string> diagnostic;

    bool is_success() const noexcept {
        switch(status) {
            case SelectedRepositoryProviderTransactionStatus::NotRequired:
            case SelectedRepositoryProviderTransactionStatus::Succeeded:
                return true;
            case SelectedRepositoryProviderTransactionStatus::
                BlockedBeforeExecution:
            case SelectedRepositoryProviderTransactionStatus::Failed:
            case SelectedRepositoryProviderTransactionStatus::OutcomeUnknown:
                return false;
        }
        return false;
    }
};

// An explicit type selects the trusted receipt-capable execution path. The
// existing two-argument transaction API remains the default and does not gain
// a hidden helper, /run, or hook dependency.
class SelectedRepositoryProviderTrustedReceiptRequest final {
public:
    [[nodiscard]] static SelectedRepositoryProviderTrustedReceiptRequest capture_actual_installs() noexcept {
        return SelectedRepositoryProviderTrustedReceiptRequest{};
    }

    [[nodiscard]] static SelectedRepositoryProviderTrustedReceiptRequest
    capture_actual_installs(CleanupInvocationAuthority authority) noexcept {
        return SelectedRepositoryProviderTrustedReceiptRequest(
            std::move(authority));
    }

    [[nodiscard]] const std::optional<CleanupInvocationAuthority>&
    invocation_authority() const noexcept {
        return invocation_authority_;
    }

private:
    SelectedRepositoryProviderTrustedReceiptRequest() noexcept = default;
    explicit SelectedRepositoryProviderTrustedReceiptRequest(
        CleanupInvocationAuthority authority) noexcept
        : invocation_authority_(std::move(authority)) {
    }

    std::optional<CleanupInvocationAuthority> invocation_authority_;
};

// One selected-provider transaction can satisfy several BuildPlan edges. The
// token therefore remains transaction-owned while each exact decision keeps
// its own work-item attribution.
struct SelectedRepositoryProviderTrustedExecutionBinding {
    std::size_t work_item_index;
    std::size_t build_plan_edge_index;
    std::string parent_package_base;
    DependencyRequirement requirement;
    BuildPlanProvidedDependency selected_decision;
    ProvidedDependency provider;
    ProviderResolutionKind resolution;
};

// Positive selected-provider factual authority. Raw receipt/capture/result
// aggregates cannot construct this value; only the executor that ran the
// session-owned exact transaction can close it.
class SelectedRepositoryProviderTrustedExecutionEvidence final {
public:
    SelectedRepositoryProviderTrustedExecutionEvidence() = delete;
    SelectedRepositoryProviderTrustedExecutionEvidence(
        const SelectedRepositoryProviderTrustedExecutionEvidence&) = default;
    SelectedRepositoryProviderTrustedExecutionEvidence(
        SelectedRepositoryProviderTrustedExecutionEvidence&&) noexcept =
        default;
    SelectedRepositoryProviderTrustedExecutionEvidence& operator=(
        const SelectedRepositoryProviderTrustedExecutionEvidence&) = default;
    SelectedRepositoryProviderTrustedExecutionEvidence& operator=(
        SelectedRepositoryProviderTrustedExecutionEvidence&&) noexcept =
        default;
    ~SelectedRepositoryProviderTrustedExecutionEvidence() = default;

    [[nodiscard]] InvocationDependencyTransactionOwner owner() const noexcept {
        return InvocationDependencyTransactionOwner::
            SelectedRepositoryProvider;
    }
    [[nodiscard]] const CleanupInvocationAuthority& invocation_authority()
        const noexcept {
        return authority_;
    }
    [[nodiscard]] const std::string& transaction_token() const noexcept {
        return transaction_token_;
    }
    [[nodiscard]] const std::vector<
        SelectedRepositoryProviderTrustedExecutionBinding>&
    bindings() const noexcept {
        return bindings_;
    }
    [[nodiscard]] const SelectedRepositoryProviderTransactionResult&
    transaction() const noexcept {
        return transaction_;
    }
    [[nodiscard]] const TrustedAlpmReceiptCaptureResult& receipt_capture()
        const noexcept {
        return receipt_capture_;
    }
    [[nodiscard]] const std::vector<ProvidedDependency>& selected_providers()
        const noexcept {
        return transaction_.selected_providers;
    }
    [[nodiscard]] const std::vector<std::string>& actual_install_set()
        const noexcept {
        return actual_install_set_;
    }

#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
    [[nodiscard]] static SelectedRepositoryProviderTrustedExecutionEvidence
    make_for_test(
        CleanupInvocationAuthority authority,
        std::string transaction_token,
        std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
            bindings,
        SelectedRepositoryProviderTransactionResult transaction,
        TrustedAlpmReceiptCaptureResult receipt_capture,
        std::vector<std::string> actual_install_set) noexcept {
        return SelectedRepositoryProviderTrustedExecutionEvidence(
            std::move(authority), std::move(transaction_token),
            std::move(bindings), std::move(transaction),
            std::move(receipt_capture), std::move(actual_install_set));
    }
#endif

private:
    SelectedRepositoryProviderTrustedExecutionEvidence(
        CleanupInvocationAuthority authority,
        std::string transaction_token,
        std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
            bindings,
        SelectedRepositoryProviderTransactionResult transaction,
        TrustedAlpmReceiptCaptureResult receipt_capture,
        std::vector<std::string> actual_install_set) noexcept
        : authority_(std::move(authority)),
          transaction_token_(std::move(transaction_token)),
          bindings_(std::move(bindings)),
          transaction_(std::move(transaction)),
          receipt_capture_(std::move(receipt_capture)),
          actual_install_set_(std::move(actual_install_set)) {
    }

    CleanupInvocationAuthority authority_;
    std::string transaction_token_;
    std::vector<SelectedRepositoryProviderTrustedExecutionBinding> bindings_;
    SelectedRepositoryProviderTransactionResult transaction_;
    TrustedAlpmReceiptCaptureResult receipt_capture_;
    std::vector<std::string> actual_install_set_;

    friend class SelectedRepositoryProviderTrustedReceiptExecutor;
};

// The public fields remain a non-authoritative diagnostic result. Making this
// a non-aggregate keeps the closed optional private: rewrapping a raw capture
// can never populate positive execution evidence.
class SelectedRepositoryProviderTrustedReceiptExecutionResult final {
public:
    SelectedRepositoryProviderTrustedReceiptExecutionResult() = default;
    SelectedRepositoryProviderTrustedReceiptExecutionResult(
        SelectedRepositoryProviderTransactionResult transaction,
        std::optional<TrustedAlpmReceiptCaptureResult> receipt_capture) noexcept;
    SelectedRepositoryProviderTrustedReceiptExecutionResult(
        const SelectedRepositoryProviderTrustedReceiptExecutionResult&) =
        default;
    SelectedRepositoryProviderTrustedReceiptExecutionResult(
        SelectedRepositoryProviderTrustedReceiptExecutionResult&&) noexcept =
        default;
    SelectedRepositoryProviderTrustedReceiptExecutionResult& operator=(
        const SelectedRepositoryProviderTrustedReceiptExecutionResult&) =
        default;
    SelectedRepositoryProviderTrustedReceiptExecutionResult& operator=(
        SelectedRepositoryProviderTrustedReceiptExecutionResult&&) noexcept =
        default;
    ~SelectedRepositoryProviderTrustedReceiptExecutionResult() = default;

    SelectedRepositoryProviderTransactionResult transaction;
    std::optional<TrustedAlpmReceiptCaptureResult> receipt_capture;

    [[nodiscard]] const std::optional<
        SelectedRepositoryProviderTrustedExecutionEvidence>&
    trusted_execution_evidence() const noexcept;

private:
    std::optional<SelectedRepositoryProviderTrustedExecutionEvidence>
        trusted_execution_evidence_;

    friend class SelectedRepositoryProviderTrustedReceiptExecutor;
};

#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
// phase/upgrade-allのfake-symbol binaryは#268 lower lifecycleをlinkしない。
// production factoryを公開せず、registered orchestration seamのtyped ABIだけを
// layout非依存のdoubleで保持する。
class RegisteredSourcePackageBaseExecutionResultTestDouble final {
    std::string package_base_;
    ProductionSourceBuildStagedOutcome production_outcome_;
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children_;
    std::vector<ArtifactPackageIdentity> unselected_artifacts_;

public:
    RegisteredSourcePackageBaseExecutionResultTestDouble(
        std::string package_base,
        std::vector<PackageBaseSourceBuildSelectedResult>
            selected_children,
        std::vector<ArtifactPackageIdentity> unselected_artifacts,
        ProductionSourceBuildProvenance source_provenance = {})
        : package_base_(std::move(package_base)),
          production_outcome_({.source_provenance = std::move(source_provenance),
                               .build_outcome =
                                   ProductionSourceBuildCommandOutcome::Succeeded,
                               .install_outcome =
                                   ProductionSourceInstallOutcome::Succeeded}),
          selected_children_(std::move(selected_children)),
          unselected_artifacts_(std::move(unselected_artifacts)) {
    }

    const std::string& package_base() const noexcept {
        return package_base_;
    }
    const ProductionSourceBuildProvenance& source_provenance()
        const noexcept {
        return production_outcome_.source_provenance;
    }
    const ProductionSourceBuildStagedOutcome& production_outcome()
        const noexcept {
        return production_outcome_;
    }
    const std::vector<PackageBaseSourceBuildSelectedResult>&
    selected_children() const noexcept {
        return selected_children_;
    }
    const std::vector<ArtifactPackageIdentity>&
    unselected_artifacts() const noexcept {
        return unselected_artifacts_;
    }
};

class RegisteredSourcePackageBaseCleanupErrorTestDouble final
    : public std::runtime_error {
    RegisteredSourcePackageBaseExecutionResultTestDouble result_;

public:
    RegisteredSourcePackageBaseCleanupErrorTestDouble(
        RegisteredSourcePackageBaseExecutionResultTestDouble result,
        const std::string& diagnostic)
        : std::runtime_error(diagnostic), result_(std::move(result)) {
    }

    const RegisteredSourcePackageBaseExecutionResultTestDouble& result()
        const noexcept {
        return result_;
    }
};

class RegisteredSourcePackageBasePreparationErrorTestDouble final
    : public std::runtime_error {
    std::variant<
        PackageBaseArtifactIdentitySelectionFailure,
        MixedPackageBaseInstallReasonUnsupported>
        failure_;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome_;

public:
    RegisteredSourcePackageBasePreparationErrorTestDouble(
        PackageBaseArtifactIdentitySelectionFailure failure,
        const std::string& diagnostic,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt)
        : std::runtime_error(diagnostic), failure_(std::move(failure)),
          production_outcome_(std::move(production_outcome)) {
    }
    RegisteredSourcePackageBasePreparationErrorTestDouble(
        MixedPackageBaseInstallReasonUnsupported failure,
        const std::string& diagnostic,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt)
        : std::runtime_error(diagnostic), failure_(std::move(failure)),
          production_outcome_(std::move(production_outcome)) {
    }

    const PackageBaseArtifactIdentitySelectionFailure* selection_failure()
        const noexcept {
        return std::get_if<
            PackageBaseArtifactIdentitySelectionFailure>(&failure_);
    }
    const MixedPackageBaseInstallReasonUnsupported* mixed_reason_failure()
        const noexcept {
        return std::get_if<
            MixedPackageBaseInstallReasonUnsupported>(&failure_);
    }
    const std::optional<ProductionSourceBuildStagedOutcome>&
    production_outcome() const noexcept {
        return production_outcome_;
    }
};

class RegisteredSourcePackageBasePhaseErrorTestDouble final
    : public std::runtime_error {
    SeparatedPackageBaseSourceBuildFailurePhase phase_;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure_;
    std::optional<PackageMetadataFailure> package_metadata_failure_;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome_;

public:
    RegisteredSourcePackageBasePhaseErrorTestDouble(
        SeparatedPackageBaseSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<ReviewedSourceProductionFailure>
            reviewed_source_failure = std::nullopt,
        std::optional<PackageMetadataFailure>
            package_metadata_failure = std::nullopt,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt)
        : std::runtime_error(diagnostic), phase_(phase),
          reviewed_source_failure_(std::move(reviewed_source_failure)),
          package_metadata_failure_(
              std::move(package_metadata_failure)),
          production_outcome_(std::move(production_outcome)) {
    }

    SeparatedPackageBaseSourceBuildFailurePhase phase() const noexcept {
        return phase_;
    }
    const std::optional<ReviewedSourceProductionFailure>&
    reviewed_source_failure() const noexcept {
        return reviewed_source_failure_;
    }
    const std::optional<PackageMetadataFailure>&
    package_metadata_failure() const noexcept {
        return package_metadata_failure_;
    }
    const std::optional<ProductionSourceBuildStagedOutcome>&
    production_outcome() const noexcept {
        return production_outcome_;
    }
};

class RegisteredSourcePackageTransactionErrorTestDouble final
    : public std::runtime_error {
    PackageBaseArtifactInstallTransactionFailureKind failure_kind_;
    std::string package_base_;
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts_;
    std::optional<int> exit_code_;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome_;

public:
    RegisteredSourcePackageTransactionErrorTestDouble(
        PackageBaseArtifactInstallTransactionFailureKind failure_kind,
        std::string package_base,
        std::vector<PackageBaseArtifactInstallTransactionAttempt>
            attempts,
        std::optional<int> exit_code,
        const std::string& diagnostic,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt)
        : std::runtime_error(diagnostic), failure_kind_(failure_kind),
          package_base_(std::move(package_base)),
          attempts_(std::move(attempts)), exit_code_(exit_code),
          production_outcome_(std::move(production_outcome)) {
    }

    PackageBaseArtifactInstallTransactionFailureKind failure_kind()
        const noexcept {
        return failure_kind_;
    }
    const std::string& package_base() const noexcept {
        return package_base_;
    }
    const std::vector<PackageBaseArtifactInstallTransactionAttempt>&
    attempts() const noexcept {
        return attempts_;
    }
    const std::optional<int>& exit_code() const noexcept {
        return exit_code_;
    }
    const std::optional<ProductionSourceBuildStagedOutcome>&
    production_outcome() const noexcept {
        return production_outcome_;
    }
};

using RegisteredSourcePackageBaseExecutionResult =
    RegisteredSourcePackageBaseExecutionResultTestDouble;
using RegisteredSourcePackageBaseCleanupError =
    RegisteredSourcePackageBaseCleanupErrorTestDouble;
using RegisteredSourcePackageBasePreparationError =
    RegisteredSourcePackageBasePreparationErrorTestDouble;
using RegisteredSourcePackageBasePhaseError =
    RegisteredSourcePackageBasePhaseErrorTestDouble;
using RegisteredSourcePackageTransactionError =
    RegisteredSourcePackageTransactionErrorTestDouble;
#else
using RegisteredSourcePackageBaseExecutionResult =
    PackageBaseSourceBuildExecutionResult;
using RegisteredSourcePackageBaseCleanupError =
    SeparatedPackageBaseSourceBuildCleanupError;
using RegisteredSourcePackageBasePreparationError =
    SeparatedPackageBaseSourceBuildPreparationError;
using RegisteredSourcePackageBasePhaseError =
    SeparatedPackageBaseSourceBuildPhaseError;
using RegisteredSourcePackageTransactionError =
    PackageBaseArtifactInstallTransactionError;
#endif

// checkoutやmetadata queryより前に確認できるwork item単体のstatic契約。
void require_static_production_source_build_work_item(
    const ProductionSourceBuildWorkItem& work_item);

// generic/sync/registered source routeが所有するsingular compatibility境界。
// standalone repository routeはPackageBaseSetを使い、multipleを先頭へ潰さない。
const RequiredPackageArtifactTarget& require_singular_required_package_target(
    const ProductionSourceBuildWorkItem& work_item);

void require_supported_production_source_build_options(
    const AppConfig& config);

bool build_source_target(
    const std::string& package_name,
    const SourceBuildEnvironment& custom_environment,
    const AppConfig& config);

RemoteSourceBuildPreparation prepare_remote_source_build(
    const std::string& package_name,
    const SourceBuildEnvironment& custom_environment,
    const AppConfig& config);

ResolvedSourceBuildIdentity resolve_source_build_identity(
    const std::string& package_name);

ResolvedSourceBuildIdentity make_repository_source_build_identity(
    const RepositoryPackagePresent& package);

// strict reader等で既にowned化したenvironmentを再readせずwork itemに射影する。
ProductionSourceBuildWorkItem prepare_resolved_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    bool only_if_updated,
    bool needed);
ProductionSourceBuildWorkItem prepare_resolved_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    bool only_if_updated,
    bool needed,
    const ProviderSelectionCallback& select_provider);

// registered system/source routeだけがsource kindに応じたlifecycle policyを
// 選ぶ。shared standalone/sync factoryの契約は変更しない。
ProductionSourceBuildWorkItem prepare_registered_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    const ProviderSelectionCallback& select_provider);

ProductionSourceBuildWorkItem prepare_smart_source_build_work_item(
    const std::string& package_name,
    bool only_if_updated,
    bool needed);
ProductionSourceBuildWorkItem prepare_smart_source_build_work_item(
    const std::string& package_name,
    bool only_if_updated,
    bool needed,
    const ProviderSelectionCallback& select_provider);

std::vector<ProductionSourceBuildWorkItem> prepare_aur_source_build_work_items(
    const BuildPlan& plan,
    bool use_source_build_preferences,
    bool needed);

LocalSourceBuildDependencyPreparation
prepare_local_source_build_dependencies(
    const LocalBuildPlan& plan,
    bool use_source_build_preferences,
    bool needed);

// Local planから構築したremote AUR dependency全件について、cache authorityを
// 作る前にfatal reviewed-stateを観測し、既存のsingle-consumption slotへ保持する。
void preflight_local_source_build_dependencies(
    LocalSourceBuildDependencyPreparation& preparation,
    const AppConfig& config);

PreparedProductionSourceBuildInvocation prepare_production_source_build_invocation(
    std::vector<ProductionSourceBuildWorkItem> work_items,
    const AppConfig& config);

// Runs the same mutation-free static, reviewed-source, and pacman-database
// preflight without minting an invocation that execution can consume.
ProductionSourceBuildPreparationObservation
observe_production_source_build_preparation(
    std::vector<ProductionSourceBuildWorkItem> work_items,
    const AppConfig& config);

inline ProductionSourceBuildWorkItemObservation
make_production_source_build_work_item_observation(
    const ProductionSourceBuildWorkItem& work_item) {
    const bool requires_reviewed_state =
        work_item.request.aur_review_identity.has_value();

    return ProductionSourceBuildWorkItemObservation{
        SourceBuildRequestObservation{
            work_item.request.package_name,
            work_item.request.checkout_name,
            work_item.request.git_url,
            work_item.request.custom_environment,
            work_item.request.empty_value_policy,
            work_item.request.update_baseline,
            work_item.request.installed_snapshot,
            work_item.request.only_if_updated,
            work_item.request.needed,
            work_item.request.aur_review_identity,
            requires_reviewed_state
                ? ReviewedSourceFatalStateObservationStatus::Completed
                : ReviewedSourceFatalStateObservationStatus::Inapplicable},
        work_item.required_targets,
        work_item.selected_repository_providers,
        work_item.build_plan_dependency_edge_indices,
        work_item.selected_repository_provider_edge_indices,
        work_item.required_target_provenance,
        work_item.artifact_lifecycle_intent,
        work_item.repository_identity,
        work_item.uses_system_update_baseline,
        work_item.configured_repository_order};
}

// Generic production preparationのnonempty契約は維持し、local root ownerが
// 存在するこの境界だけremote AUR dependency 0件を許可する。
PreparedProductionSourceBuildInvocation
prepare_local_source_build_dependency_invocation(
    LocalSourceBuildDependencyPreparation preparation,
    const ValidatedCacheRoot& cache_root,
    const AppConfig& config);

// Higher-level operationが先に確定したcache capabilityを、generic invocation
// と全work itemへ同一snapshotとして配る。environmentは再読込しない。
void seed_production_source_build_cache(
    PreparedProductionSourceBuildInvocation& invocation,
    const ValidatedCacheRoot& cache_root);

// Execution ownerが最初のexternal mutationより前に1回だけ呼び、invocation内の
// 全work itemへ同じretained cache-root capabilityを配る。
void activate_production_source_build_cache(
    PreparedProductionSourceBuildInvocation& invocation);

// invocation全体で選択済みのofficial providerをinstalled reason authorityで
// preflightし、1回のexact pacman transactionへ渡す。新規/Dependencyは
// Dependency、既存ExplicitはExplicitを維持し、混在時はmutation前に停止する。
SelectedRepositoryProviderTransactionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig& config);

// Production-capable internal API for Slice 3.6. It is intentionally not
// connected to the current public --rmdeps route.
SelectedRepositoryProviderTrustedReceiptExecutionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig& config,
    SelectedRepositoryProviderTrustedReceiptRequest receipt_request);

// source-neutralなPackageBase set execution owner。required_targetsをauthorityにし、
// child別outcomeとunselected artifact identityをflattenせず返す。
SourceBuildPackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

SourceBuildPreparationOutcome
prepare_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    SourceBuildUpdatePolicy update_policy,
    const AppConfig& config);

RegisteredSourcePackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

SourceBuildExecutionResult execute_prepared_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

// nulloptはgeneric only-if-updatedの正常skip。update runner用work itemは
// only_if_updated=falseなので、artifact install outcomeを必ず要求できる。
std::optional<ArtifactInstallExecutionOutcome>
execute_prepared_source_build_work_item(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

// lifecycle intentがSetならsource-neutral set owner、それ以外はroute ownerが
// 検証済みのsingular compatibilityへroutingする。DB snapshotを再queryしない。
// AuthoritativePartial returns the live result without an exception and stops
// the suffix. Callers must inspect is_success()/command_exit_status(). Existing
// thrown failures still use ProductionSourceBuildInvocationError.
ProductionSourceBuildInvocationResult execute_prepared_source_build_invocation(
    PreparedProductionSourceBuildInvocation invocation,
    const AppConfig& config);

// Closed remote-AUR collector entry. The collector owns the invocation/session
// and this function cannot mint or accept independent cleanup evidence.
ProductionSourceBuildInvocationResult
execute_prepared_remote_aur_cleanup_invocation(
    RemoteAurCleanupCandidateCollector& collector,
    const AppConfig& config);

#ifdef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
// Test profile replaces only invocation dispatch, not the aggregation loop.
// Callbacks must run real execution producers when testing authoritative results.
struct SourceInvocationExecutionTestHooks {
    std::function<SourceBuildExecutionResult(const ProductionSourceBuildWorkItem&, const PacmanDatabasePaths&, const AppConfig&)> singular;
    std::function<SourceBuildPackageBaseExecutionResult(const ProductionSourceBuildWorkItem&, const PacmanDatabasePaths&, const AppConfig&)> package_base;
};
void set_source_invocation_execution_test_hooks(SourceInvocationExecutionTestHooks hooks);
#endif
