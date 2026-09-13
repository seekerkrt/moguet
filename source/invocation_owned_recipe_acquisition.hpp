#pragma once

#include "devel_tracking_bootstrap.hpp"
#include "process.hpp"
#include "trusted_git.hpp"

#include <memory>
#include <optional>
#include <variant>

enum class RecipeAcquisitionStage {
    WorkspaceCreation,
    Initialization,
    Fetch,
    RepositoryValidation,
    ExactCommit,
    Metadata,
    Cleanup,
};

enum class RecipeAcquisitionFailureReason {
    UnsafeFilesystem,
    CreationFailed,
    IoFailure,
    GitProcessFailed,
    Cancelled,
    ResourceLimit,
    ExpectedCommitUnavailable,
    WrongObjectType,
    ObjectFormatMismatch,
    ExactRevisionMismatch,
    MetadataMismatch,
    MalformedRepository,
    IdentityChanged,
};

struct RecipeAcquisitionCleanupFailure {
    RecipeAcquisitionFailureReason reason;
    std::optional<int> error_number;
};
using RecipeAcquisitionCleanupResult = std::optional<RecipeAcquisitionCleanupFailure>;

struct RecipeAcquisitionFailure {
    RecipeAcquisitionStage stage;
    RecipeAcquisitionFailureReason reason;
    std::optional<int> error_number = std::nullopt;
    std::optional<BoundedCapturedProcessResult> process = std::nullopt;
    RecipeAcquisitionCleanupResult cleanup = std::nullopt;
    // Diagnostic only, present when abort cleanup leaves owned residue.
    std::optional<std::filesystem::path> abandoned_root = std::nullopt;
    std::optional<TrustedCacheFailure> boundary_failure = std::nullopt;
};

class InvocationOwnedRecipeAcquisition;
using RecipeAcquisitionResult = std::variant<InvocationOwnedRecipeAcquisition, RecipeAcquisitionFailure>;

// Unreviewed transport/materialization evidence only. Hold this owner through
// full review, exact pin and S3's final recipe reproof. checkout() is borrowed;
// copying that capability does not extend the workspace's lifetime. Explicit
// cleanup is mandatory to observe failure; destruction is a noexcept backstop.
// No production route consumes this foundation until Issue #564 Slice 3B.
class InvocationOwnedRecipeAcquisition final {
public:
    InvocationOwnedRecipeAcquisition() = delete;
    InvocationOwnedRecipeAcquisition(const InvocationOwnedRecipeAcquisition&) = delete;
    InvocationOwnedRecipeAcquisition& operator=(const InvocationOwnedRecipeAcquisition&) = delete;
    InvocationOwnedRecipeAcquisition(InvocationOwnedRecipeAcquisition&&) noexcept;
    InvocationOwnedRecipeAcquisition& operator=(InvocationOwnedRecipeAcquisition&&) = delete;
    ~InvocationOwnedRecipeAcquisition() noexcept;

    [[nodiscard]] const ValidatedCachePath& checkout() const;
    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity() const;
    [[nodiscard]] const std::filesystem::path& workspace_path() const;
    [[nodiscard]] RecipeAcquisitionCleanupResult cleanup() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    explicit InvocationOwnedRecipeAcquisition(std::unique_ptr<State>) noexcept;
    friend RecipeAcquisitionResult acquire_invocation_owned_recipe(const DevelTrackingBootstrapTrial& trial);
};

// Neither a path nor an arbitrary URL/OID tuple can construct this owner.
// Does not reobserve HEAD, revalidate migration eligibility, or authorize review.
[[nodiscard]] RecipeAcquisitionResult acquire_invocation_owned_recipe(const DevelTrackingBootstrapTrial& trial);

#ifdef MOGUET_ENABLE_RECIPE_ACQUISITION_TEST_HOOKS
#include <functional>
struct RecipeAcquisitionTestHooks {
    std::optional<std::filesystem::path> parent;
    std::function<std::string()> random_suffix;
    std::function<void(RecipeAcquisitionStage, const std::filesystem::path&)> event;
    std::function<BoundedCapturedProcessResult(const ExplicitProcessInvocation&, const BoundedProcessPolicy&)> process;
    std::function<void(const std::filesystem::path&)> before_remove;
    std::optional<std::size_t> entry_limit;
    std::optional<std::uintmax_t> byte_limit;
};
void set_recipe_acquisition_test_hooks(RecipeAcquisitionTestHooks hooks);
#endif
