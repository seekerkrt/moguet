#pragma once

#include "reviewed_devel_source_build_execution_authority.hpp"
#include "devel_build_provenance_publication.hpp"
#include "evaluated_devel_source_build.hpp"
#include "source_build_request.hpp"
#include "separated_package_base_source_build.hpp"

#include <memory>

enum class ReviewedProductionExecutionChoice { Legacy,
                                               AuthoritativeDevel };

// Ordinary execution intent, never planning OIDs or an assessment capability.
// Existing provider/dependency preparation remains the future caller's owner.
struct ReviewedDevelSourceBuildIntent {
    SourceBuildRequest request;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    PacmanDatabasePaths database_paths;
    bool rm_deps = false;
    ArtifactInstallExecutionOptions execution_options;
};

enum class ReviewedDevelSourceBuildStage {
    Intent,
    Context,
    Environment,
    Build,
    ArtifactCorrelation,
    InstallPolicy,
    Transport,
    Publication,
    Finished,
};

enum class ReviewedDevelSourceBuildIssue {
    InvalidPin,
    CheckoutMismatch,
    IdentityMismatch,
    UnsupportedChoice,
    UnsupportedCardinality,
    EditorOverlay,
    NeededRequested,
    DependencyCleanupRequested,
    UpdateSelectionRequired,
    InvalidInstallReason,
    ContextFailure,
    BuildFailure,
    ArtifactMismatch,
    DatabaseWorldUnavailable,
    DatabaseWorldMismatch,
    InstallPolicyFailure,
    InstallReasonUnsupported,
    InvalidFinalization,
    ResourceFailure,
    InternalFailure,
};

struct ReviewedDevelSourceBuildRejected {
    ReviewedDevelSourceBuildIssue issue;
};

struct ReviewedDevelSourceBuildExecutionState;

// A single owned typed pin, chosen before ProductionArtifactSourceTree erases
// its legacy lifetime. No raw provenance/path/void-pointer constructor.
class PreparedReviewedDevelSourceBuildExecution final {
public:
    PreparedReviewedDevelSourceBuildExecution() = delete;
    PreparedReviewedDevelSourceBuildExecution(const PreparedReviewedDevelSourceBuildExecution&) = delete;
    PreparedReviewedDevelSourceBuildExecution& operator=(const PreparedReviewedDevelSourceBuildExecution&) = delete;
    PreparedReviewedDevelSourceBuildExecution(PreparedReviewedDevelSourceBuildExecution&&) noexcept;
    PreparedReviewedDevelSourceBuildExecution& operator=(PreparedReviewedDevelSourceBuildExecution&&) = delete;
    ~PreparedReviewedDevelSourceBuildExecution() noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    friend class ReviewedDevelSourceBuildExecutionAuthority;
    explicit PreparedReviewedDevelSourceBuildExecution(std::unique_ptr<ReviewedDevelSourceBuildExecutionState>) noexcept;
    std::unique_ptr<ReviewedDevelSourceBuildExecutionState> state_;
};

// Owns the live S6 -> S5 -> S4 chain, including failure/Unknown. Finished is
// a pipeline boundary, not an all-success claim. No extraction/retry API.
class ReviewedDevelSourceBuildExecutionResult final {
public:
    ReviewedDevelSourceBuildExecutionResult() = delete;
    ReviewedDevelSourceBuildExecutionResult(const ReviewedDevelSourceBuildExecutionResult&) = delete;
    ReviewedDevelSourceBuildExecutionResult& operator=(const ReviewedDevelSourceBuildExecutionResult&) = delete;
    ReviewedDevelSourceBuildExecutionResult(ReviewedDevelSourceBuildExecutionResult&&) noexcept;
    ReviewedDevelSourceBuildExecutionResult& operator=(ReviewedDevelSourceBuildExecutionResult&&) = delete;
    ~ReviewedDevelSourceBuildExecutionResult() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ReviewedDevelSourceBuildStage stage() const;
    [[nodiscard]] std::optional<ReviewedDevelSourceBuildIssue> issue() const;
    // True only after a complete S4 proof, not a generic makepkg exit-status claim.
    [[nodiscard]] bool build_completed() const;
    // Diagnostic only; never a cleanup/reopen authority.
    [[nodiscard]] const std::filesystem::path& owned_root() const;
    [[nodiscard]] const ProductionSourceBuildProvenance& source_provenance() const;
    [[nodiscard]] const InvocationOwnedSourceBuildContextFailure* context_failure() const;
    [[nodiscard]] const EvaluatedDevelSourceBuildFailure* build_failure() const;
    [[nodiscard]] const InstalledDatabaseWorldResult* database_world() const;
    [[nodiscard]] const InstalledPackageQueryResult* install_policy_observation() const;
    [[nodiscard]] std::optional<InstallReasonDirective> install_reason_directive() const;
    [[nodiscard]] const DevelBuildProvenancePublicationResult* publication() const;

private:
    friend class ReviewedDevelSourceBuildExecutionAuthority;
    explicit ReviewedDevelSourceBuildExecutionResult(std::unique_ptr<ReviewedDevelSourceBuildExecutionState>) noexcept;
    const ReviewedDevelSourceBuildExecutionState& require_state() const;
    std::unique_ptr<ReviewedDevelSourceBuildExecutionState> state_;
};

// The normal 7-D owner selects this at finalize_aur_checkout_authority's typed
// pin boundary, before legacy lifetime erasure. Rejection never automatically
// chooses the other arm.
[[nodiscard]] ReviewedProductionSourceExecution prepare_reviewed_production_source_execution(
    ReviewedProductionExecutionChoice choice, ValidatedCachePath checkout, PinnedReviewedSourceBuild reviewed,
    ProductionReviewedSourceOutcome reviewed_outcome,
    std::optional<ReviewedSourceAbnormalStateReason> abnormal_state_reason,
    const ReviewedDevelSourceBuildIntent& intent);

// Nullopt only for moved-from input. Valid input consumes its one prepared
// state before any context/build side effect. No legacy fallback on failure.
[[nodiscard]] std::optional<ReviewedDevelSourceBuildExecutionResult> execute_reviewed_devel_source_build(
    PreparedReviewedDevelSourceBuildExecution prepared) noexcept;

// Distinct live result arm for future normal-owner orchestration. The legacy
// result's private construction and publication-free meaning are unchanged.
using ReviewedProductionSourceExecutionResult = std::variant<
    PackageBaseSourceBuildExecutionResult, ReviewedDevelSourceBuildExecutionResult>;

#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
struct ReviewedDevelSourceBuildExecutionTestHooks {
    std::function<void(ReviewedDevelSourceBuildStage, const EvaluatedDevelSourceBuildProof*)> before_stage;
    // Uses the existing S5 test entry; cannot inject a raw completed product.
    std::optional<std::string> exact_transaction_token;
};
void set_reviewed_devel_source_build_execution_test_hooks(ReviewedDevelSourceBuildExecutionTestHooks hooks);
#endif
