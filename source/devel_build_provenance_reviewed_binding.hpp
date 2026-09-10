#pragma once

#include "devel_build_provenance.hpp"
#include "reviewed_source_state_store.hpp"

#include <variant>

class PinnedReviewedSourceBuild;

// Production-disconnected adapter from the #411 pinned build capability to a
// stable provenance binding. Raw generation/OID/PackageBase values cannot mint
// this binding through the production interface.
enum class ReviewedSourceStateRecordBindingFailureReason {
    InvalidPinnedBuild,
    EditorOverlayPresent,
    InvalidObservedGeneration,
    InconsistentObservedDocument,
};

struct ReviewedSourceStateRecordBindingFailure {
    ReviewedSourceStateRecordBindingFailureReason reason;

    bool operator==(
        const ReviewedSourceStateRecordBindingFailure&) const = default;
};

using ReviewedSourceStateRecordBindingResult = std::variant<
    ReviewedSourceStateRecordBinding,
    ReviewedSourceStateRecordBindingFailure>;

class ReviewedSourceStateRecordBindingAuthority final {
    ReviewedSourceStateRecordBindingAuthority() = delete;

    friend ReviewedSourceStateRecordBindingResult
    derive_reviewed_source_state_record_binding(
        const PinnedReviewedSourceBuild& pinned_build);

    [[nodiscard]] static ReviewedSourceStateRecordBinding make(
        PackageBaseIdentity package_base,
        AurRecipeRevision reviewed_recipe_revision,
        std::uint64_t generation,
        ReviewedSourceStateDocumentSha256Digest document_digest);
};

[[nodiscard]] ReviewedSourceStateRecordBindingResult
derive_reviewed_source_state_record_binding(
    const PinnedReviewedSourceBuild& pinned_build);

enum class ReviewedSourceStateRecordBindingMismatchReason {
    MissingObservedRecord,
    NonLoadedState,
    SourceIdentityMismatch,
    PackageBaseMismatch,
    ReviewedRecipeRevisionMismatch,
    ReviewedStateGenerationMismatch,
    ReviewedStateDocumentDigestMismatch,
};

struct ReviewedSourceStateRecordBindingMatch {
    bool operator==(
        const ReviewedSourceStateRecordBindingMatch&) const = default;
};

struct ReviewedSourceStateRecordBindingMismatch {
    ReviewedSourceStateRecordBindingMismatchReason reason;

    bool operator==(
        const ReviewedSourceStateRecordBindingMismatch&) const = default;
};

using ReviewedSourceStateRecordBindingComparison = std::variant<
    ReviewedSourceStateRecordBindingMatch,
    ReviewedSourceStateRecordBindingMismatch>;

// Reprove a persisted binding against one exact current #411 store read. Store
// failure/unsafe-history remain outside this pure comparison and are never
// converted into a mismatch or Missing result here.
[[nodiscard]] ReviewedSourceStateRecordBindingComparison
compare_reviewed_source_state_record_binding(
    const ReviewedSourceStateRecordBinding& expected,
    const ReviewedSourceStateStoreRead& current);
