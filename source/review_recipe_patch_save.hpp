#pragma once

#include "generated_recipe_patch.hpp"
#include "local_patch_association.hpp"

struct AppConfig;
struct SourceBuildRequest;
struct ReviewedDevelSourceBuildIntent;
class ValidatedCachePath;
class ValidatedCacheRoot;

enum class RecipePatchSaveStage {
    UnsupportedDevel,
    Destination,
    Generation,
    IdentityValidation,
    Publication,
    Registration,
};

struct RecipePatchSaveFailure {
    RecipePatchSaveStage stage;
    std::optional<RecipePatchGenerationFailure> generation = std::nullopt;
    std::optional<PatchAssociationFailure> association = std::nullopt;
    std::exception_ptr exception = nullptr;
    std::optional<LocalSourceWorkspaceFailure> cleanup = std::nullopt;
    std::filesystem::path retained_material = {};
    bool registration_completed = false;
};

// Keeps save failures intact across generic preparation/build error adapters.
// The material and registry commit points are independent; no rollback/retry.
class RecipePatchSaveError final : public std::runtime_error {
    RecipePatchSaveFailure failure_;

public:
    RecipePatchSaveError(RecipePatchSaveFailure failure, const std::string& diagnostic)
        : std::runtime_error(diagnostic), failure_(std::move(failure)) {
    }
    const RecipePatchSaveFailure& failure() const noexcept {
        return failure_;
    }
};

// Checkout preparation exit, after Proceed/cwd restoration and the ordinary
// reviewed pin, before build execution. Explicit unsupported devel stops before
// its existing selection guard. No edit/marker means zero save interaction.
// Decline means zero generation, destination, material or registry I/O.
void save_review_recipe_edit(
    const ReviewRecipeEditCorrelation* edit, bool unsupported_devel_edit,
    const SourceBuildRequest& request, const ValidatedCachePath& checkout,
    const ValidatedCacheRoot& cache_root, const AppConfig& config,
    const ReviewedDevelSourceBuildIntent* intent);
