#pragma once

#include "dependency_plan.hpp"
#include "aur_update_query.hpp"
#include "interactive_confirmation.hpp"
#include "source_build_request.hpp"
#include "reviewed_devel_source_build_execution.hpp"
#include "package_metadata.hpp"
#include "reviewed_source_production_failure.hpp"
#include "separated_package_base_source_build.hpp"
#include "source_environment.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct AppConfig;
class RemoteAurCleanupCandidateCollector;
class ReviewedSourceFatalStatePreflightSlot;

// Preserve acquisition/cleanup details across the existing preparation exception
// boundary. A cleanup failure may accompany an earlier review/confirmation stop;
// that original exception remains the primary operation outcome.
class BootstrapRecipeAcquisitionError final : public std::exception {
public:
    explicit BootstrapRecipeAcquisitionError(RecipeAcquisitionFailure failure,
                                             std::exception_ptr primary = nullptr) noexcept;
    const RecipeAcquisitionFailure& failure() const noexcept {
        return failure_;
    }
    const std::exception_ptr& primary() const noexcept {
        return primary_;
    }
    const char* what() const noexcept override {
        return "Devel bootstrap recipe acquisition or cleanup failed; typed details retained.";
    }

private:
    RecipeAcquisitionFailure failure_;
    std::exception_ptr primary_;
};

// Routing/diagnostic adapter only. The immutable owner retains the original
// move-only product; copies share its lifetime and cannot execute or publish.
struct ReviewedDevelExecutionSnapshot {
    std::shared_ptr<const ReviewedDevelSourceBuildExecutionResult> owner;
    bool complete = false;
    bool projection_failed = false;
    bool build_completed = false;
    std::optional<DevelSourceArtifactInstallOperation> operation;
    std::optional<int> pacman_exit_status;
    std::optional<DevelSourceArtifactInstallReceipt> receipt;
    std::optional<DevelSourceArtifactInstallProof> proof;
    std::optional<DevelBuildProvenancePublicationState> publication;
    std::optional<DevelSourceArtifactInstallCleanupState> cleanup;
    std::optional<ArtifactPackageIdentity> artifact;
    std::optional<ProductionSourceBuildStagedOutcome> production_outcome;
    std::optional<RecipeAcquisitionFailure> recipe_acquisition_failure = std::nullopt;
    // Diagnostic projection of the same immutable owner, not a new acceptance
    // authority. Keeps pure runner/reducer profiles independent of S4 linkage.
    std::optional<PinnedClosureReviewFailure> closure_review_failure = std::nullopt;
    std::optional<PinnedClosureFailure> closure_failure = std::nullopt;
    std::optional<ReviewedSourceOperationStop> required_review_decline = std::nullopt;
    std::vector<ArtifactPackageIdentity> selected_artifacts = {};
    std::vector<ArtifactPackageIdentity> unselected_artifacts = {};
};
using SourceBuildPackageBaseExecutionResult = std::variant<PackageBaseSourceBuildExecutionResult, ReviewedDevelExecutionSnapshot>;

enum class SourceBuildExecutionStatus {
    Installed,
    SkippedAsNeeded,
    UpToDate,
    UpdateStatusUnknownSkipped,
    AuthoritativeIncomplete,
    DevelRequiresCheckSkipped,
};

enum class SourceBuildUpdateStatusUnknownSkipReason {
    NoConfirm,
    NonInteractiveStdin,
    UserDeclined,
};

enum class SourceBuildUpdatePolicy {
    AlwaysBuild,
    OnlyIfUpdated,
};

struct SourceBuildUpToDate {
    std::string diagnostic;
    std::optional<ProductionSourceBuildProvenance> source_provenance;
};

struct SourceBuildUpdateStatusUnknownSkipped {
    SourceBuildUpdateStatusUnknownSkipReason reason =
        SourceBuildUpdateStatusUnknownSkipReason::NoConfirm;
    std::string diagnostic;
    std::optional<ProductionSourceBuildProvenance> source_provenance;
};

// generic source-buildの正常終了を、package transactionの有無まで潰さず返す。
// diagnosticはCLI出力の解析用ではなく、上位phaseがowned detailを保持するための値。
struct SourceBuildExecutionResult {
    SourceBuildExecutionStatus status =
        SourceBuildExecutionStatus::UpdateStatusUnknownSkipped;
    std::optional<SourceBuildUpdateStatusUnknownSkipReason>
        update_status_unknown_skip_reason;
    std::string diagnostic;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome;
    // Shared immutable diagnostic ownership; the contained live product is never copied or extracted.
    std::optional<ReviewedDevelExecutionSnapshot> devel_execution = std::nullopt;
    std::shared_ptr<const AurUpdateQueryResult> devel_update_query = nullptr;
    std::optional<ConfirmationDecisionOrigin> devel_rebuild_confirmation = std::nullopt;
};

// AUR requests return one single-consumption slot; repository requests return
// nullptr. Fatal observations throw before the invocation can reach mutation.
[[nodiscard]] std::shared_ptr<ReviewedSourceFatalStatePreflightSlot>
preflight_reviewed_source_fatal_state_for_production(
    const SourceBuildRequest& request);

// Frozen bytes from one ordinary reviewed AUR editor invocation, minted only
// after the existing Proceed acceptance and post-snapshot revalidation. This
// is neither persistent save consent nor a generic recipe/tree snapshot.
class ReviewRecipeEditCorrelation final {
    AurReviewedSourceReviewIdentity identity_;
    std::uintmax_t checkout_device_;
    std::uintmax_t checkout_inode_;
    std::string baseline_pkgbuild_;
    std::string accepted_pkgbuild_;

    ReviewRecipeEditCorrelation(AurReviewedSourceReviewIdentity identity,
                                std::uintmax_t checkout_device,
                                std::uintmax_t checkout_inode,
                                std::string baseline_pkgbuild,
                                std::string accepted_pkgbuild) noexcept;
    friend struct SourceBuildPreparationAccess;

public:
    ReviewRecipeEditCorrelation(const ReviewRecipeEditCorrelation&) = delete;
    ReviewRecipeEditCorrelation& operator=(const ReviewRecipeEditCorrelation&) = delete;
    ReviewRecipeEditCorrelation(ReviewRecipeEditCorrelation&&) noexcept = default;
    ReviewRecipeEditCorrelation& operator=(ReviewRecipeEditCorrelation&&) = delete;
    const AurReviewedSourceReviewIdentity& identity() const noexcept {
        return identity_;
    }
    std::uintmax_t checkout_device() const noexcept {
        return checkout_device_;
    }
    std::uintmax_t checkout_inode() const noexcept {
        return checkout_inode_;
    }
    const std::string& baseline_pkgbuild() const noexcept {
        return baseline_pkgbuild_;
    }
    const std::string& accepted_pkgbuild() const noexcept {
        return accepted_pkgbuild_;
    }
};

// checkout/update-check/reviewとprivate artifact rootを一度だけ通過した
// execution capability。raw pathはpreparation/executor ownerへ閉じる。
class PreparedSourceBuildNeedsBuild final {
    std::optional<ProductionArtifactSourceTree> source_tree_;
    std::optional<PreparedReviewedDevelSourceBuildExecution> devel_;
    std::optional<ValidatedPrivateCacheRoot> artifact_root_;
    bool rebuild_ = false;
    bool clean_build_ = false;
    std::optional<ReviewRecipeEditCorrelation> accepted_recipe_edit_;

    PreparedSourceBuildNeedsBuild(
        ProductionArtifactSourceTree source_tree,
        ValidatedPrivateCacheRoot artifact_root,
        bool rebuild,
        bool clean_build,
        std::optional<ReviewRecipeEditCorrelation> accepted_recipe_edit) noexcept
        : source_tree_(std::move(source_tree)),
          artifact_root_(std::move(artifact_root)), rebuild_(rebuild),
          clean_build_(clean_build),
          accepted_recipe_edit_(std::move(accepted_recipe_edit)) {
    }

    explicit PreparedSourceBuildNeedsBuild(PreparedReviewedDevelSourceBuildExecution devel) noexcept : devel_(std::move(devel)) {
    }
    PreparedSourceBuildNeedsBuild() noexcept = default;

    friend struct SourceBuildPreparationAccess;
    friend struct SourceBuildPreparedExecutionAccess;
#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS
    friend const ProductionSourceBuildProvenance&
    prepared_source_build_provenance_for_test(
        const PreparedSourceBuildNeedsBuild& prepared);
#endif

public:
    PreparedSourceBuildNeedsBuild(
        const PreparedSourceBuildNeedsBuild&) = delete;
    PreparedSourceBuildNeedsBuild& operator=(
        const PreparedSourceBuildNeedsBuild&) = delete;
    PreparedSourceBuildNeedsBuild(
        PreparedSourceBuildNeedsBuild&&) noexcept = default;
    PreparedSourceBuildNeedsBuild& operator=(
        PreparedSourceBuildNeedsBuild&&) = delete;
    ~PreparedSourceBuildNeedsBuild() = default;

    std::optional<ProductionSourceBuildProvenance> recipe_provenance() const {
        return source_tree_ ? std::optional<ProductionSourceBuildProvenance>(source_tree_->provenance()) : std::nullopt;
    }

    // Empty for no editor, byte-identical PKGBUILD (including .install-only
    // edits), and every route outside ordinary reviewed AUR.
    const std::optional<ReviewRecipeEditCorrelation>& accepted_recipe_edit() const noexcept {
        return accepted_recipe_edit_;
    }

#if defined(MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS)
    static PreparedSourceBuildNeedsBuild
    make_for_registered_source_build_test() noexcept {
        return PreparedSourceBuildNeedsBuild{};
    }
#endif
};

using SourceBuildPreparationOutcome = std::variant<
    SourceBuildUpToDate,
    SourceBuildUpdateStatusUnknownSkipped,
    PreparedSourceBuildNeedsBuild>;

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS
const ProductionSourceBuildProvenance&
prepared_source_build_provenance_for_test(
    const PreparedSourceBuildNeedsBuild& prepared);

using ReviewedSourceBeforePublicationHookForTest = void (*)();
void set_reviewed_recipe_after_snapshot_hook_for_test(
    ReviewedSourceBeforePublicationHookForTest hook);
void set_reviewed_source_before_publication_hook_for_test(
    ReviewedSourceBeforePublicationHookForTest hook);
#endif

SourceBuildPreparationOutcome prepare_source_build_for_execution(
    const SourceBuildRequest& request,
    const std::string& display_name,
    SourceBuildUpdatePolicy update_policy,
    const ValidatedCacheRoot& cache_root,
    const AppConfig& config,
    const ReviewedDevelSourceBuildIntent* execution_intent = nullptr);

SourceBuildPackageBaseExecutionResult
execute_prepared_source_build_package_base_typed(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

PackageBaseSourceBuildExecutionResult
execute_prepared_source_build_package_base_with_cleanup_authority(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index);

SourceBuildExecutionResult execute_source_build_typed(
    const SourceBuildRequest& request,
    const ValidatedCacheRoot& cache_root,
    DesiredInstallReason desired_reason,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

// source-neutralなPackageBase execution。ordered required_targetsを一つのfresh
// workspace/transactionへ渡し、multipleではrequest.package_nameを使わない。
SourceBuildPackageBaseExecutionResult
execute_source_build_package_base_typed(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    const ValidatedCacheRoot& cache_root,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config);

PackageBaseSourceBuildExecutionResult
execute_source_build_package_base_with_cleanup_authority(
    const SourceBuildRequest& request,
    const std::vector<RequiredPackageArtifactTarget>& required_targets,
    const ValidatedCacheRoot& cache_root,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index);

ProductionSourceBuildStagedOutcome project_reviewed_devel_execution_outcome(const ReviewedDevelSourceBuildExecutionResult& result);
bool reviewed_devel_execution_succeeded(const ReviewedDevelSourceBuildExecutionResult& result) noexcept;
