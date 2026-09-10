#include "reviewed_source_production_outcome.hpp"

#include "localization.hpp"

#include <stdexcept>
#include <string>

namespace {

constexpr const char* PACKAGE_BASE_FIELD = "PackageBase";
constexpr const char* STANDARD_INPUT_NAME = "stdin";

const std::string& reviewed_commit(
    const ProductionSourceBuildProvenance& provenance) {
    if(!provenance.reviewed_upstream_base_revision.has_value() ||
       provenance.reviewed_upstream_base_revision->state() !=
           SourceRevisionState::Known ||
       provenance.reviewed_upstream_base_revision->git_commit() == nullptr) {
        throw std::logic_error(
            "Reviewed production outcome has no exact upstream commit.");
    }
    return *provenance.reviewed_upstream_base_revision->git_commit();
}

void require_compatibility_provenance(
    const ProductionSourceBuildProvenance& provenance) {
    if(!provenance.compatibility_reason.has_value() ||
       provenance.reviewed_upstream_base_revision.has_value() ||
       provenance.publication_status.has_value() ||
       provenance.reviewed_outcome.has_value() ||
       provenance.abnormal_state_reason.has_value() ||
       provenance.reviewed_state_generation.has_value()) {
        throw std::logic_error(
            "Compatibility production outcome is incoherent.");
    }
}

void require_reviewed_provenance(
    const ProductionSourceBuildProvenance& provenance) {
    if(provenance.compatibility_reason.has_value() ||
       !provenance.reviewed_outcome.has_value() ||
       !provenance.publication_status.has_value() ||
       !provenance.reviewed_state_generation.has_value() ||
       *provenance.reviewed_state_generation == 0 ||
       ((provenance.reviewed_outcome ==
         ProductionReviewedSourceOutcome::
             AbnormalStateRebindFullReview) !=
        provenance.abnormal_state_reason.has_value())) {
        throw std::logic_error("Reviewed production outcome is incoherent.");
    }
    static_cast<void>(reviewed_commit(provenance));
}

std::string compatibility_line(
    const std::string& package_base,
    ReviewedSourceCompatibilityBuildReason reason) {
    switch(reason) {
        case ReviewedSourceCompatibilityBuildReason::NoDiff:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: {} skipped review acceptance; this invocation has compatibility-only source authority, and reviewed state was not advanced.",
                PACKAGE_BASE_FIELD, package_base, "--nodiff");
        case ReviewedSourceCompatibilityBuildReason::NoConfirm:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: {} did not create review acceptance; this invocation has compatibility-only source authority, and reviewed state was not advanced.",
                PACKAGE_BASE_FIELD, package_base, "--noconfirm");
        case ReviewedSourceCompatibilityBuildReason::NonInteractiveInput:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: {} is non-interactive, so no review acceptance was created; this invocation has compatibility-only source authority, and reviewed state was not advanced.",
                PACKAGE_BASE_FIELD, package_base, STANDARD_INPUT_NAME);
        case ReviewedSourceCompatibilityBuildReason::ExplicitReviewDecline:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: review acceptance was explicitly declined; this invocation has compatibility-only source authority, and reviewed state was not advanced.",
                PACKAGE_BASE_FIELD, package_base);
        case ReviewedSourceCompatibilityBuildReason::DefaultReviewDecline:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: review acceptance was declined by the safe default; this invocation has compatibility-only source authority, and reviewed state was not advanced.",
                PACKAGE_BASE_FIELD, package_base);
    }
    throw std::logic_error(
        "Compatibility production outcome has an unknown reason.");
}

std::string accepted_review_line(
    const std::string& package_base,
    const ProductionSourceBuildProvenance& provenance) {
    const std::string& commit = reviewed_commit(provenance);
    switch(*provenance.reviewed_outcome) {
        case ProductionReviewedSourceOutcome::InitialFullReview:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: initial full review accepted for exact upstream commit {}.",
                PACKAGE_BASE_FIELD, package_base, commit);
        case ProductionReviewedSourceOutcome::UpdateReview:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: update review accepted for exact upstream commit {}.",
                PACKAGE_BASE_FIELD, package_base, commit);
        case ProductionReviewedSourceOutcome::RebaselineFullReview:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: explicit full rebaseline accepted for exact upstream commit {}.",
                PACKAGE_BASE_FIELD, package_base, commit);
        case ProductionReviewedSourceOutcome::AbnormalStateRebindFullReview:
            switch(*provenance.abnormal_state_reason) {
                case ReviewedSourceAbnormalStateReason::Invalid:
                    return localization::format_translated_message(
                        "Reviewed-source outcome for {} {}: explicit rebind/rebaseline of invalid state accepted for exact upstream commit {}.",
                        PACKAGE_BASE_FIELD, package_base, commit);
                case ReviewedSourceAbnormalStateReason::Corrupted:
                    return localization::format_translated_message(
                        "Reviewed-source outcome for {} {}: explicit rebind/rebaseline of corrupted state accepted for exact upstream commit {}.",
                        PACKAGE_BASE_FIELD, package_base, commit);
                case ReviewedSourceAbnormalStateReason::SourceMismatch:
                    return localization::format_translated_message(
                        "Reviewed-source outcome for {} {}: explicit source-identity rebind/rebaseline accepted for exact upstream commit {}.",
                        PACKAGE_BASE_FIELD, package_base, commit);
            }
            throw std::logic_error(
                "Abnormal reviewed production outcome has an unknown reason.");
        case ProductionReviewedSourceOutcome::AlreadyReviewed:
            return localization::format_translated_message(
                "Reviewed-source outcome for {} {}: exact upstream commit {} was already reviewed; no review prompt or state rewrite occurred.",
                PACKAGE_BASE_FIELD, package_base, commit);
    }
    throw std::logic_error("Reviewed production outcome has an unknown kind.");
}

std::string publication_line(
    const std::string& package_base,
    const ProductionSourceBuildProvenance& provenance) {
    const std::uint64_t generation = *provenance.reviewed_state_generation;
    if(*provenance.reviewed_outcome ==
       ProductionReviewedSourceOutcome::AlreadyReviewed) {
        if(*provenance.publication_status !=
           ReviewedSourcePublicationStatus::AlreadyPublishedSameTarget) {
            throw std::logic_error(
                "Already-reviewed outcome unexpectedly published state.");
        }
        return localization::format_translated_message(
            "Reviewed state for {} {} remains at generation {}; build and install outcomes are reported separately.",
            PACKAGE_BASE_FIELD, package_base, generation);
    }
    switch(*provenance.publication_status) {
        case ReviewedSourcePublicationStatus::Published:
            return localization::format_translated_message(
                "Reviewed state for {} {} was finalized as generation {}; build and install outcomes are reported separately.",
                PACKAGE_BASE_FIELD, package_base, generation);
        case ReviewedSourcePublicationStatus::AlreadyPublishedSameTarget:
            return localization::format_translated_message(
                "Reviewed state for {} {} already matched the accepted target at publication; generation {} was retained and no state rewrite was needed.",
                PACKAGE_BASE_FIELD, package_base, generation);
    }
    throw std::logic_error(
        "Reviewed production outcome has an unknown publication status.");
}

std::string build_input_line(
    const std::string& package_base,
    const ProductionSourceBuildProvenance& provenance) {
    const std::string& commit = reviewed_commit(provenance);
    switch(provenance.editor_overlay) {
        case ReviewedSourceEditorOverlayStatus::None:
            return localization::format_translated_message(
                "Build input for {} {} is pinned to reviewed upstream commit {} with no invocation-local editor overlay.",
                PACKAGE_BASE_FIELD, package_base, commit);
        case ReviewedSourceEditorOverlayStatus::InvocationLocal:
            return localization::format_translated_message(
                "Build input for {} {} includes an invocation-local editor overlay on reviewed upstream commit {}; it is not the exact reviewed commit tree.",
                PACKAGE_BASE_FIELD, package_base, commit);
    }
    throw std::logic_error(
        "Reviewed production outcome has an unknown editor status.");
}

std::string build_outcome_line(
    const std::string& package_base,
    ProductionSourceBuildCommandOutcome outcome) {
    switch(outcome) {
        case ProductionSourceBuildCommandOutcome::NotAttempted:
            return localization::format_translated_message(
                "Build outcome for {} {}: not attempted.",
                PACKAGE_BASE_FIELD, package_base);
        case ProductionSourceBuildCommandOutcome::Started:
            return localization::format_translated_message(
                "Build outcome for {} {}: started; completion was not proven.",
                PACKAGE_BASE_FIELD, package_base);
        case ProductionSourceBuildCommandOutcome::Failed:
            return localization::format_translated_message(
                "Build outcome for {} {}: failed.",
                PACKAGE_BASE_FIELD, package_base);
        case ProductionSourceBuildCommandOutcome::Succeeded:
            return localization::format_translated_message(
                "Build outcome for {} {}: succeeded.",
                PACKAGE_BASE_FIELD, package_base);
    }
    throw std::logic_error("Production build outcome is unknown.");
}

std::string install_outcome_line(
    const std::string& package_base,
    ProductionSourceInstallOutcome outcome) {
    switch(outcome) {
        case ProductionSourceInstallOutcome::NotAttempted:
            return localization::format_translated_message(
                "Install outcome for {} {}: not attempted.",
                PACKAGE_BASE_FIELD, package_base);
        case ProductionSourceInstallOutcome::Started:
            return localization::format_translated_message(
                "Install outcome for {} {}: started; completion was not proven.",
                PACKAGE_BASE_FIELD, package_base);
        case ProductionSourceInstallOutcome::Failed:
            return localization::format_translated_message(
                "Install outcome for {} {}: failed.",
                PACKAGE_BASE_FIELD, package_base);
        case ProductionSourceInstallOutcome::Succeeded:
            return localization::format_translated_message(
                "Install outcome for {} {}: succeeded.",
                PACKAGE_BASE_FIELD, package_base);
    }
    throw std::logic_error("Production install outcome is unknown.");
}

} // namespace

ReviewedSourceProductionOutcomePresentation
format_reviewed_source_production_outcome(
    const std::string& package_base,
    const ProductionSourceBuildProvenance& provenance) {
    if(package_base.empty()) {
        throw std::invalid_argument(
            "Reviewed production outcome requires a PackageBase.");
    }

    ReviewedSourceProductionOutcomePresentation presentation;
    switch(provenance.review_status) {
        case ProductionSourceReviewStatus::NotApplicable:
            if(provenance.compatibility_reason.has_value() ||
               provenance.reviewed_upstream_base_revision.has_value() ||
               provenance.publication_status.has_value() ||
               provenance.reviewed_outcome.has_value() ||
               provenance.abnormal_state_reason.has_value() ||
               provenance.reviewed_state_generation.has_value()) {
                throw std::logic_error(
                    "Non-AUR production outcome contains reviewed provenance.");
            }
            return presentation;
        case ProductionSourceReviewStatus::CompatibilityWithoutReview:
            require_compatibility_provenance(provenance);
            presentation.info_lines.push_back(compatibility_line(
                package_base, *provenance.compatibility_reason));
            switch(provenance.editor_overlay) {
                case ReviewedSourceEditorOverlayStatus::None:
                    break;
                case ReviewedSourceEditorOverlayStatus::InvocationLocal:
                    presentation.info_lines.push_back(
                        localization::format_translated_message(
                            "Build input for {} {} includes invocation-local editor changes, but no reviewed upstream authority was created.",
                            PACKAGE_BASE_FIELD, package_base));
                    break;
                default:
                    throw std::logic_error(
                        "Compatibility production outcome has an unknown editor status.");
            }
            return presentation;
        case ProductionSourceReviewStatus::Reviewed:
            require_reviewed_provenance(provenance);
            presentation.info_lines.push_back(
                accepted_review_line(package_base, provenance));
            presentation.info_lines.push_back(
                publication_line(package_base, provenance));
            presentation.info_lines.push_back(
                build_input_line(package_base, provenance));
            return presentation;
    }
    throw std::logic_error(
        "Production source outcome has an unknown review status.");
}

ReviewedSourceProductionOutcomePresentation
format_production_source_build_staged_outcome(
    const std::string& package_base,
    const ProductionSourceBuildStagedOutcome& outcome) {
    ReviewedSourceProductionOutcomePresentation presentation =
        format_reviewed_source_production_outcome(
            package_base, outcome.source_provenance);
    presentation.info_lines.push_back(
        build_outcome_line(package_base, outcome.build_outcome));
    presentation.info_lines.push_back(
        install_outcome_line(package_base, outcome.install_outcome));
    return presentation;
}
