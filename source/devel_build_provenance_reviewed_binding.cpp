#include "devel_build_provenance_reviewed_binding.hpp"

#include "reviewed_source_pinned_build.hpp"

#include <string>
#include <utility>

ReviewedSourceStateRecordBinding
ReviewedSourceStateRecordBindingAuthority::make(
    PackageBaseIdentity package_base,
    AurRecipeRevision reviewed_recipe_revision,
    std::uint64_t generation,
    ReviewedSourceStateDocumentSha256Digest document_digest) {
    return ReviewedSourceStateRecordBinding(
        std::move(package_base), std::move(reviewed_recipe_revision),
        ReviewedSourceStateRecordGeneration(generation),
        std::move(document_digest));
}

namespace {

ReviewedSourceStateRecordBindingResult binding_failure(
    ReviewedSourceStateRecordBindingFailureReason reason) {
    return ReviewedSourceStateRecordBindingFailure{reason};
}

ReviewedSourceStateRecordBindingComparison binding_mismatch(
    ReviewedSourceStateRecordBindingMismatchReason reason) {
    return ReviewedSourceStateRecordBindingMismatch{reason};
}

} // namespace

ReviewedSourceStateRecordBindingResult
derive_reviewed_source_state_record_binding(
    const PinnedReviewedSourceBuild& pinned_build) {
    if(!pinned_build.valid()) {
        return binding_failure(
            ReviewedSourceStateRecordBindingFailureReason::
                InvalidPinnedBuild);
    }
    if(pinned_build.editor_overlay_status() !=
       ReviewedSourceEditorOverlayStatus::None) {
        return binding_failure(
            ReviewedSourceStateRecordBindingFailureReason::
                EditorOverlayPresent);
    }

    const ReviewedSourceStateObservedRecord& observed =
        pinned_build.published_record();
    if(observed.generation == 0) {
        return binding_failure(
            ReviewedSourceStateRecordBindingFailureReason::
                InvalidObservedGeneration);
    }

    const ReviewedSourceStateInterpretation interpreted =
        interpret_reviewed_source_state(
            observed.raw_contents, pinned_build.identity().package_base());
    const auto* loaded = std::get_if<ReviewedSourceStateLoaded>(&interpreted);
    if(loaded == nullptr || loaded->state != pinned_build.reviewed_state() ||
       loaded->state.reviewed_revision() !=
           pinned_build.reviewed_upstream_base_revision()) {
        return binding_failure(
            ReviewedSourceStateRecordBindingFailureReason::
                InconsistentObservedDocument);
    }

    const std::string* reviewed_oid =
        loaded->state.reviewed_revision().git_commit();
    if(reviewed_oid == nullptr) {
        return binding_failure(
            ReviewedSourceStateRecordBindingFailureReason::
                InconsistentObservedDocument);
    }

    return ReviewedSourceStateRecordBindingAuthority::make(
        loaded->state.package_base(),
        AurRecipeRevision::git_commit(*reviewed_oid), observed.generation,
        ReviewedSourceStateDocumentSha256Digest::make(
            xdg_generation_store_raw_contents_sha256(
                observed.raw_contents)));
}

ReviewedSourceStateRecordBindingComparison
compare_reviewed_source_state_record_binding(
    const ReviewedSourceStateRecordBinding& expected,
    const ReviewedSourceStateStoreRead& current) {
    if(!current.observed.has_value()) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                MissingObservedRecord);
    }
    const auto* loaded =
        std::get_if<ReviewedSourceStateLoaded>(&current.observation);
    if(loaded == nullptr) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                NonLoadedState);
    }

    const PackageBaseIdentity& actual_package_base =
        loaded->state.package_base();
    if(actual_package_base.source() != expected.package_base().source()) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                SourceIdentityMismatch);
    }
    if(actual_package_base.package_base() !=
       expected.package_base().package_base()) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                PackageBaseMismatch);
    }
    if(loaded->state.reviewed_revision() !=
       expected.reviewed_recipe_revision().value()) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                ReviewedRecipeRevisionMismatch);
    }
    if(current.observed->generation != expected.generation().value()) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                ReviewedStateGenerationMismatch);
    }
    if(xdg_generation_store_raw_contents_sha256(
           current.observed->raw_contents) !=
       expected.document_digest().value()) {
        return binding_mismatch(
            ReviewedSourceStateRecordBindingMismatchReason::
                ReviewedStateDocumentDigestMismatch);
    }
    return ReviewedSourceStateRecordBindingMatch{};
}
