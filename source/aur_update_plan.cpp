#include "aur_update_plan.hpp"

#include "localization.hpp"

#include <stdexcept>
#include <utility>

DevelUpdateAssessment project_conservative_devel_update_assessment(
    const DevelPackageClassification& classification) {
    switch(classification.evidence_level()) {
        case DevelEvidenceLevel::Normal:
            if(classification.source_form_disposition() !=
                   DevelSourceFormDisposition::NotApplicable ||
               classification.suffix_evidence().has_candidate()) {
                throw std::logic_error(
                    "Normal devel classification is inconsistent.");
            }
            return DevelUpdateAssessment::not_applicable();
        case DevelEvidenceLevel::SuffixCandidate:
            if(classification.source_form_disposition() !=
                   DevelSourceFormDisposition::RequiresCheck ||
               !classification.suffix_evidence().has_candidate() ||
               !classification.trusted_metadata().empty() ||
               !classification.successful_build_confirmations().empty()) {
                throw std::logic_error(
                    "Suffix-candidate devel classification is inconsistent.");
            }
            return DevelUpdateAssessment::requires_check(
                DevelRequiresCheckReason::SuffixCandidateOnly);
        case DevelEvidenceLevel::SourceMetadataConfirmed:
        case DevelEvidenceLevel::BuildSourceConfirmed:
            // POLICY(#270): these authorities require the #475/#476 connection;
            // this Slice must not manufacture provenance or tracking support.
            throw std::logic_error(
                "Confirmed devel evidence is outside the conservative AUR update connection.");
    }
    throw std::logic_error("Unknown devel evidence level.");
}

namespace {

bool has_no_devel_candidate_suffix(
    const AurUpdatePlanEntry& entry) noexcept {
    if(!entry.aur_package.has_value()) return false;
    try {
        return !devel_suffix_candidate_kind(entry.installed_name).has_value() &&
               !devel_suffix_candidate_kind(
                    entry.aur_package->package_base)
                    .has_value();
    } catch(...) {
        // POLICY(#270): invalid legacy identities cannot justify an implicit
        // NotApplicable projection.
        return false;
    }
}

bool has_consistent_conservative_devel_projection(
    const AurUpdatePlanEntry& entry) noexcept {
    if(!entry.aur_package.has_value() ||
       (entry.aur_package->version_relation !=
            AurVersionRelation::OlderThanInstalled &&
        entry.aur_package->version_relation !=
            AurVersionRelation::SameAsInstalled)) {
        return false;
    }

    if(!entry.devel_classification.has_value()) {
        // Compatibility remains available only for an ordinary exact-AUR
        // entry. A suffix-looking child or PackageBase requires the explicit
        // production projection and must not become a silent UpToDate.
        return entry.devel_assessment.state() ==
                   DevelUpdateAssessmentState::NotApplicable &&
               has_no_devel_candidate_suffix(entry);
    }

    const DevelPackageClassification& classification =
        *entry.devel_classification;
    const DevelPackageSuffixEvidence& suffix =
        classification.suffix_evidence();
    if(suffix.package_base() != entry.aur_package->package_base ||
       suffix.installed_children().size() != 1 ||
       suffix.installed_children().front().package_name() !=
           entry.installed_name) {
        return false;
    }

    try {
        return project_conservative_devel_update_assessment(classification) ==
               entry.devel_assessment;
    } catch(...) {
        // Confirmed/future evidence belongs to another producer authority.
        return false;
    }
}

} // namespace

AurUpdateEffectiveState project_aur_update_effective_state(
    const AurUpdatePlanEntry& entry) noexcept {
    switch(entry.classification) {
        case AurUpdateClassification::UpdateAvailable:
            // POLICY(#270): authoritative normal version precedence is never
            // downgraded by missing devel provenance.
            return AurUpdateEffectiveState::UpdateAvailable;
        case AurUpdateClassification::NonAurForeign:
            return AurUpdateEffectiveState::NonAurForeign;
        case AurUpdateClassification::MetadataUnavailable:
            return AurUpdateEffectiveState::MetadataUnavailable;
        case AurUpdateClassification::VersionComparisonUnavailable:
            return AurUpdateEffectiveState::VersionComparisonUnavailable;
        case AurUpdateClassification::UpToDate: {
            if(!entry.aur_package || entry.installed_name != entry.aur_package->aur_name) return AurUpdateEffectiveState::Inconsistent;
            if(entry.devel_assessment_origin == AurDevelAssessmentOrigin::CurrentObservation) {
                if(!entry.aur_package || entry.installed_name != entry.aur_package->aur_name ||
                   (entry.aur_package->version_relation != AurVersionRelation::SameAsInstalled &&
                    entry.aur_package->version_relation != AurVersionRelation::OlderThanInstalled))
                    return AurUpdateEffectiveState::Inconsistent;
                switch(entry.devel_assessment.state()) {
                    case DevelUpdateAssessmentState::NotApplicable:
                    case DevelUpdateAssessmentState::UpToDate: return AurUpdateEffectiveState::UpToDate;
                    case DevelUpdateAssessmentState::UpdateAvailable: return AurUpdateEffectiveState::UpdateAvailable;
                    case DevelUpdateAssessmentState::RequiresCheck: return AurUpdateEffectiveState::RequiresCheck;
                    case DevelUpdateAssessmentState::Unknown: return AurUpdateEffectiveState::Unknown;
                    case DevelUpdateAssessmentState::Unsupported: return AurUpdateEffectiveState::Unsupported;
                }
                return AurUpdateEffectiveState::Inconsistent;
            }
            if(entry.devel_assessment_origin != AurDevelAssessmentOrigin::Conservative || !has_consistent_conservative_devel_projection(entry)) {
                return AurUpdateEffectiveState::Inconsistent;
            }
            return entry.devel_assessment.state() ==
                           DevelUpdateAssessmentState::NotApplicable
                       ? AurUpdateEffectiveState::UpToDate
                       : AurUpdateEffectiveState::RequiresCheck;
        }
    }
    return AurUpdateEffectiveState::Inconsistent;
}

AurUpdatePlanEntry classify_aur_update(const AurUpdatePlanInput& input) {
    if(const auto* aur_package = std::get_if<AurUpdateRemotePackage>(&input.aur_metadata)) {
        AurUpdateClassification classification;
        switch(aur_package->version_relation) {
            case AurVersionRelation::OlderThanInstalled:
            case AurVersionRelation::SameAsInstalled:
                classification = AurUpdateClassification::UpToDate;
                break;
            case AurVersionRelation::NewerThanInstalled:
                classification = AurUpdateClassification::UpdateAvailable;
                break;
            case AurVersionRelation::Unavailable:
                classification = AurUpdateClassification::VersionComparisonUnavailable;
                break;
            default:
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Unknown {} version relation.", "AUR"));
        }

        DevelPackageClassification devel_classification =
            DevelPackageClassification::classify(
                DevelPackageSuffixEvidence::classify(
                    aur_package->package_base,
                    {input.installed_name}));
        DevelUpdateAssessment devel_assessment =
            project_conservative_devel_update_assessment(
                devel_classification);

        return AurUpdatePlanEntry{
            input.installed_name,
            input.installed_version,
            input.install_reason,
            *aur_package,
            classification,
            std::move(devel_classification),
            std::move(devel_assessment)};
    }

    if(std::holds_alternative<AurUpdateMetadataNotFound>(input.aur_metadata)) {
        return AurUpdatePlanEntry{
            input.installed_name,
            input.installed_version,
            input.install_reason,
            std::nullopt,
            AurUpdateClassification::NonAurForeign,
            std::nullopt,
            DevelUpdateAssessment::not_applicable()};
    }

    if(std::holds_alternative<AurUpdateMetadataUnavailable>(input.aur_metadata)) {
        return AurUpdatePlanEntry{
            input.installed_name,
            input.installed_version,
            input.install_reason,
            std::nullopt,
            AurUpdateClassification::MetadataUnavailable,
            std::nullopt,
            DevelUpdateAssessment::not_applicable()};
    }

    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} update metadata result.", "AUR"));
}

AurUpdatePlan make_aur_update_plan(const std::vector<AurUpdatePlanInput>& inputs) {
    AurUpdatePlan plan;
    plan.entries.reserve(inputs.size());

    // POLICY(#266): installed inventory順をplanでも維持する。
    for(const auto& input : inputs) {
        plan.entries.push_back(classify_aur_update(input));
    }

    return plan;
}
