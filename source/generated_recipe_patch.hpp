#pragma once

#include "local_recipe_candidate.hpp"
#include "process.hpp"
#include "reviewed_source_lifecycle.hpp"

#include <system_error>
#include <exception>
#include <variant>

class ReviewRecipeEditCorrelation;
struct RecipePatchGenerationAccess;

// Owning, immutable invocation-local material. Only successful shape checking
// and byte-exact replay can mint it. No save consent or persistence authority.
class GeneratedRecipePatch final {
    AurReviewedSourceReviewIdentity identity_;
    std::string bytes_;
    GeneratedRecipePatch(AurReviewedSourceReviewIdentity identity, std::string bytes);
    friend struct RecipePatchGenerationAccess;

public:
    GeneratedRecipePatch(const GeneratedRecipePatch&) = default;
    GeneratedRecipePatch(GeneratedRecipePatch&&) noexcept = default;
    GeneratedRecipePatch& operator=(const GeneratedRecipePatch&) = delete;
    GeneratedRecipePatch& operator=(GeneratedRecipePatch&&) = delete;
    const AurReviewedSourceReviewIdentity& identity() const noexcept;
    const std::string& bytes() const noexcept;
};

struct RecipePatchNoChange {};
enum class RecipePatchGenerationFailureReason {
    UnsupportedEdit,
    TemporaryIoFailure,
    DiffToolFailure,
    DiffOutputLimit,
    PatchRejected,
    ApplyFailed,
    ReproductionMismatch,
    CandidateChanged,
    CleanupFailure,
    InternalFailure,
};
struct RecipePatchGenerationFailure {
    RecipePatchGenerationFailureReason reason;
    std::optional<BoundedCapturedProcessResult> diff_process = std::nullopt;
    std::optional<LocalRecipeCandidateFailureReason> rejected_shape = std::nullopt;
    std::optional<LocalRecipeCandidateFailure> replay = std::nullopt;
    std::optional<LocalSourceRootFailure> recipe_snapshot_failure = std::nullopt;
    std::optional<std::error_code> temporary_io_error = std::nullopt;
    // Unexpected internal errors and known apply exceptions retain their cause;
    // they are not reclassified as a Git exit or an unsupported edit.
    std::exception_ptr exception = nullptr;
    std::optional<std::error_code> cleanup_error = std::nullopt;
    std::filesystem::path abandoned_directory = {}; // Diagnostic only.
};
using RecipePatchGenerationResult = std::variant<
    RecipePatchNoChange, GeneratedRecipePatch, RecipePatchGenerationFailure>;

// Borrows both frozen byte strings from the SAME Slice 1 correlation. Never
// reads the original checkout, resolves a source, or evaluates PKGBUILD.
// Future save orchestration must establish explicit consent before calling.
[[nodiscard]] RecipePatchGenerationResult generate_recipe_patch(
    const ReviewRecipeEditCorrelation& edit);

#ifdef MOGUET_ENABLE_GENERATED_RECIPE_PATCH_TEST_HOOKS
// Component-only input/transport seam; absent from production construction.
using RecipePatchDiffProcessForTest = std::function<BoundedCapturedProcessResult(
    const ExplicitProcessInvocation&, const BoundedProcessPolicy&)>;
RecipePatchGenerationResult generate_recipe_patch_for_test(
    const AurReviewedSourceReviewIdentity& identity,
    const std::string& baseline, const std::string& accepted,
    const RecipePatchDiffProcessForTest& process = {});
#endif
