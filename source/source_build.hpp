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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct AppConfig;
class RemoteAurCleanupCandidateCollector;
class ReviewedSourceFatalStatePreflightSlot;

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

// checkout/update-check/reviewとprivate artifact rootを一度だけ通過した
// execution capability。raw pathはpreparation/executor ownerへ閉じる。
class PreparedSourceBuildNeedsBuild final {
    std::optional<ProductionArtifactSourceTree> source_tree_;
    std::optional<PreparedReviewedDevelSourceBuildExecution> devel_;
    std::optional<ValidatedPrivateCacheRoot> artifact_root_;
    bool rebuild_ = false;
    bool clean_build_ = false;

    PreparedSourceBuildNeedsBuild(
        ProductionArtifactSourceTree source_tree,
        ValidatedPrivateCacheRoot artifact_root,
        bool rebuild,
        bool clean_build) noexcept
        : source_tree_(std::move(source_tree)),
          artifact_root_(std::move(artifact_root)), rebuild_(rebuild),
          clean_build_(clean_build) {
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
