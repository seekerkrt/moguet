#pragma once

#include "local_source_build.hpp"
#include "source_package_identity.hpp"
#include "interactive_confirmation.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// Owning bytes in caller-selected order. No paths, registration, persistence
// or digest lookup belong to this consumer. One entry is one textual patch.
struct LocalRecipePatch {
    std::string bytes;
};

enum class LocalRecipePatchOutcome { NotAttempted,
                                     Applied,
                                     Failed };
enum class LocalRecipeCandidatePhase {
    Preflight,
    Snapshot,
    PrepatchMetadata,
    Apply,
    PostpatchMetadata,
    Identity,
    Plan,
    Review
};
enum class LocalRecipeCandidateFailureReason {
    InvalidMaterial,
    UnsupportedPatch,
    ToolFailure,
    CandidateChanged,
    MetadataFailure,
    IdentityChanged,
    PlanFailure,
    PreparationFailure,
    ReviewStopped
};

// Pure shape check shared with acquisition; not applicability/authorization.
std::optional<LocalRecipeCandidateFailureReason> validate_local_recipe_patch(const std::string& bytes);

struct LocalRecipeCandidateFailure {
    LocalRecipeCandidatePhase phase;
    LocalRecipeCandidateFailureReason reason;
    std::vector<LocalRecipePatchOutcome> patches;
    std::optional<int> tool_exit_code;
    std::optional<LocalSourceWorkspaceFailure> cleanup_failure;
    std::filesystem::path candidate_path; // Diagnostic only, never a capability.
    std::optional<std::size_t> rejected_patch_index = std::nullopt;
    std::optional<ConfirmationResult> review_stop = std::nullopt;
};

// Shared PKGBUILD-only operation over an already isolated, caller-owned recipe
// directory. The caller retains the workspace owner and metadata/review policy.
// Returns the exact modified snapshot; never reopens external patch material.
LocalSourceRoot apply_recipe_patch_series(
    const LocalSourceRoot& before, const std::vector<LocalRecipePatch>& patches,
    LocalRecipeCandidateFailure& failure);

// Optional public composition gate over the exact modified snapshot. No
// persistence or UI policy belongs to the candidate owner itself.
using LocalRecipeReviewCallback = std::function<ConfirmationResult(const LocalSourceFileSnapshot&)>;

class LocalRecipeCandidateError final : public std::runtime_error {
    LocalRecipeCandidateFailure failure_;

public:
    LocalRecipeCandidateError(LocalRecipeCandidateFailure failure,
                              const std::string& diagnostic);
    const LocalRecipeCandidateFailure& failure() const noexcept;
};

// The physical build request is deliberately not exposed as a normal local
// source projection: its temporary path must never become semantic identity.
// This owner retains original identity and the early workspace through plan
// observation/dependency preparation and transfers the same candidate to build.
class PreparedLocalRecipeBuild final {
    LocalSourceRoot original_;
    PackageBaseIdentity identity_;
    LocalSourceFileSnapshot prepatch_;
    PreparedLocalSourceBuild build_;
    LocalSourceBuildResult execute();

    PreparedLocalRecipeBuild(LocalSourceRoot original,
                             PackageBaseIdentity identity,
                             LocalSourceFileSnapshot prepatch,
                             LocalSourceBuildRequest request,
                             LocalSourceWorkspace& workspace);
    friend PreparedLocalRecipeBuild prepare_local_recipe_build(
        LocalSourceRoot, ValidatedCacheRoot, SourceBuildEnvironment,
        std::vector<LocalRecipePatch>, ArtifactMakepkgBuildOptions,
        const ProviderSelectionCallback&, std::optional<PackageBaseIdentity>, const LocalRecipeReviewCallback&);
    friend LocalSourceBuildResult execute_local_recipe_build(
        PreparedLocalRecipeBuild);

public:
    PreparedLocalRecipeBuild(const PreparedLocalRecipeBuild&) = delete;
    PreparedLocalRecipeBuild& operator=(const PreparedLocalRecipeBuild&) = delete;
    PreparedLocalRecipeBuild(PreparedLocalRecipeBuild&&) noexcept = default;
    PreparedLocalRecipeBuild& operator=(PreparedLocalRecipeBuild&&) = delete;
    const PackageBaseIdentity& source_identity() const noexcept;
    const LocalSourceFileSnapshot& prepatch_candidate() const noexcept;
    const LocalSourceFileSnapshot& modified_candidate() const noexcept;
    const LocalSourceBuildMetadata& metadata() const noexcept;
    const LocalBuildPlan& plan() const noexcept;
    void require_unchanged_identity() const;
};

// Mutation-capable internal boundary: the caller must authorize both candidate
// metadata evaluations before calling. No CLI dispatch or implicit consent.
// Resolves a plan ONLY after successful ordered apply + fresh evaluation.
PreparedLocalRecipeBuild prepare_local_recipe_build(
    LocalSourceRoot original, ValidatedCacheRoot cache_root,
    SourceBuildEnvironment environment, std::vector<LocalRecipePatch> patches,
    ArtifactMakepkgBuildOptions options = {},
    const ProviderSelectionCallback& select_provider = {},
    std::optional<PackageBaseIdentity> expected_source = std::nullopt,
    const LocalRecipeReviewCallback& review_modified = {});

// Local unit only; the caller handles plan dependencies through existing
// authority first. The returned artifact capability feeds the existing install
// owner. Build/cleanup failures keep LocalSourceBuildPhaseError semantics.
LocalSourceBuildResult execute_local_recipe_build(PreparedLocalRecipeBuild prepared);
