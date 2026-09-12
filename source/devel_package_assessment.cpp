#include "devel_package_assessment.hpp"

#include "devel_package_classification.hpp"
#include "package_identifier.hpp"

#include <stdexcept>
#include <utility>

namespace {
using Stage = DevelPackageAssessmentStage;
using Check = DevelRequiresCheckReason;
using Issue = DevelPackageAssessmentIssue;
#ifdef MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
DevelPackageAssessmentTestHooks g_assessment_hooks;
#endif

void enter(DevelPackageAssessment& result, Stage stage) {
    result.stage = stage;
#ifdef MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
    if(g_assessment_hooks.before_stage) g_assessment_hooks.before_stage(stage);
#endif
}

void requires_check(DevelPackageAssessment& result, Check reason) {
    result.assessment = DevelUpdateAssessment::requires_check(reason);
}

bool provenance_loaded(DevelPackageAssessment& result, const DevelBuildProvenanceStoreReadResult& read) {
    if(std::holds_alternative<DevelBuildProvenanceStoreLoaded>(read)) return true;
    Check reason = Check::BuildSourceProofUnavailable;
    if(std::holds_alternative<DevelBuildProvenanceStoreMissing>(read))
        reason = Check::ProvenanceMissing;
    else if(std::holds_alternative<DevelBuildProvenanceStoreInvalidDocument>(read))
        reason = Check::ProvenanceInvalid;
    else if(std::holds_alternative<DevelBuildProvenanceStoreCorruptRecord>(read))
        reason = Check::ProvenanceCorrupted;
    else if(std::holds_alternative<DevelBuildProvenanceStoreFutureSchema>(read))
        reason = Check::ProvenanceFutureSchema;
    else if(std::holds_alternative<DevelBuildProvenanceStoreSourceMismatch>(read) ||
            std::holds_alternative<DevelBuildProvenanceStorePackageBaseMismatch>(read))
        reason = Check::SourceIdentityChanged;
    requires_check(result, reason);
    return false;
}

bool installed_matches(DevelPackageAssessment& result, DevelPackageLocalObservations& observations,
                       const DevelBuildProvenance& baseline) {
    const auto* observed = std::get_if<CurrentInstalledArtifactBindingObserved>(&*observations.installed);
    if(!observed) {
        requires_check(result, std::holds_alternative<CurrentInstalledArtifactBindingAbsent>(*observations.installed)
                                   ? Check::InstalledArtifactDrift
                                   : Check::BuildSourceProofUnavailable);
        return false;
    }
    observations.installed_comparison = compare_installed_artifact_binding(baseline.installed_binding(), observed->binding());
    if(std::holds_alternative<InstalledArtifactBindingMatch>(*observations.installed_comparison)) return true;
    requires_check(result, Check::InstalledArtifactDrift);
    return false;
}

bool reviewed_matches(DevelPackageAssessment& result, DevelPackageLocalObservations& observations,
                      const DevelBuildProvenance& baseline) {
    const auto* read = std::get_if<ReviewedSourceStateStoreRead>(&*observations.reviewed);
    if(!read) {
        requires_check(result, Check::BuildSourceProofUnavailable);
        return false;
    }
    // Retain the comparator detail even for a non-Loaded semantic arm, while
    // outer unsafe/failure results never enter this pure comparator.
    observations.reviewed_comparison = compare_reviewed_source_state_record_binding(baseline.reviewed_source_binding(), *read);
    if(std::holds_alternative<ReviewedSourceStateRecordBindingMatch>(*observations.reviewed_comparison)) return true;
    Check reason = Check::BuildSourceProofUnavailable;
    if(std::holds_alternative<ReviewedSourceStateMissing>(read->observation))
        reason = Check::NoAuthoritativeBuildProvenance;
    else if(std::holds_alternative<ReviewedSourceStateSourceMismatch>(read->observation))
        reason = Check::SourceIdentityChanged;
    else if(std::holds_alternative<ReviewedSourceStateLoaded>(read->observation)) {
        using Mismatch = ReviewedSourceStateRecordBindingMismatchReason;
        switch(std::get<ReviewedSourceStateRecordBindingMismatch>(*observations.reviewed_comparison).reason) {
            case Mismatch::ReviewedRecipeRevisionMismatch: reason = Check::AurRecipeAdvanced; break;
            case Mismatch::SourceIdentityMismatch:
            case Mismatch::PackageBaseMismatch: reason = Check::SourceIdentityChanged; break;
            case Mismatch::MissingObservedRecord:
            case Mismatch::NonLoadedState:
            case Mismatch::ReviewedStateGenerationMismatch:
            case Mismatch::ReviewedStateDocumentDigestMismatch: reason = Check::NoAuthoritativeBuildProvenance; break;
        }
    }
    requires_check(result, reason);
    return false;
}

DevelUnknownReason remote_failure_reason(const GitRemoteRevisionObservationResult& remote) {
    if(std::holds_alternative<GitRemoteRevisionRefNotFound>(remote)) return DevelUnknownReason::RemoteRefNotFound;
    if(std::holds_alternative<GitRemoteRevisionTimeout>(remote)) return DevelUnknownReason::RemoteObservationTimedOut;
    if(std::holds_alternative<GitRemoteRevisionMalformedOutput>(remote)) return DevelUnknownReason::RemoteResultMalformed;
    if(std::holds_alternative<GitRemoteRevisionAmbiguousOutput>(remote)) return DevelUnknownReason::RemoteResultAmbiguous;
    return DevelUnknownReason::RemoteObservationFailed;
}
} // namespace

DevelPackageAssessment DevelPackageAssessmentAuthority::assess(const DevelPackageAssessmentTarget& target) {
    DevelPackageAssessment result;
    const auto& base = target.package_base;
    if(base.source().kind() != PackageSourceKind::Aur) {
        result.assessment = DevelUpdateAssessment::not_applicable();
        return result;
    }
    if(!is_valid_package_name(base.package_base()) || !base.source().location().value() ||
       base.source().location().value()->empty()) {
        result.issue = Issue::InvalidTarget;
        requires_check(result, Check::SourceIdentityChanged);
        return result;
    }
    if(target.installed_children.size() != 1) {
        result.issue = target.installed_children.empty() ? Issue::NoInstalledChild : Issue::MultipleInstalledChildren;
        requires_check(result, target.installed_children.empty() ? Check::InstalledArtifactDrift : Check::BuildSourceProofUnavailable);
        return result;
    }
    const auto& child = target.installed_children.front();
    if(child.package_base() != base || !is_valid_package_name(child.package_name())) {
        result.issue = Issue::InvalidTarget;
        requires_check(result, Check::SourceIdentityChanged);
        return result;
    }

    enter(result, Stage::Provenance);
    result.before.provenance = read_devel_build_provenance(base);
    if(!provenance_loaded(result, *result.before.provenance)) {
        if(std::holds_alternative<DevelBuildProvenanceStoreMissing>(*result.before.provenance) && !target.known_devel_context &&
           !DevelPackageSuffixEvidence::classify(base.package_base(), {child.package_name()}).has_candidate())
            result.assessment = DevelUpdateAssessment::not_applicable();
        return result;
    }
    const auto& selected = std::get<DevelBuildProvenanceStoreLoaded>(*result.before.provenance);
    const auto& baseline = selected.provenance;
    if(baseline.package_base() != base) {
        result.issue = Issue::SourceIdentityMismatch;
        requires_check(result, Check::SourceIdentityChanged);
        return result;
    }
    enter(result, Stage::Installed);
    result.before.installed = observe_current_installed_artifact_binding(child);
    if(!installed_matches(result, result.before, baseline)) return result;
    enter(result, Stage::Reviewed);
    result.before.reviewed = read_reviewed_source_state(base);
    if(!reviewed_matches(result, result.before, baseline)) return result;

    enter(result, Stage::Source);
    // Schema v1 is the S4/S6 single-child/single-floating-source projection.
    // #411 binds that exact historical recipe, not a newly evaluated URL.
    const auto& source = baseline.evaluated_source();
    if(source != baseline.actual_built_revision().revision().source()) {
        result.issue = Issue::SourceIdentityMismatch;
        requires_check(result, Check::SourceIdentityChanged);
        return result;
    }
    if(source.kind() != VcsKind::Git) {
        result.assessment = DevelUpdateAssessment::unsupported(DevelUnsupportedReason::UnsupportedVcs);
        return result;
    }
    if(source.architecture()) {
        result.issue = Issue::ArchitectureSpecificSource;
        requires_check(result, Check::ArchitectureSpecificSourceUnresolved);
        return result;
    }
    if(source.selector().tracking_behavior() != VcsSelectorTrackingBehavior::Floating ||
       (source.selector().kind() != VcsSelectorKind::DefaultHead && source.selector().kind() != VcsSelectorKind::Branch)) {
        result.issue = Issue::UnsupportedSelector;
        requires_check(result, Check::SelectorRequiresCheck);
        return result;
    }
    std::optional<ValidatedHttpsGitRemote> remote;
    try {
        remote = ValidatedHttpsGitRemote::make(source.source_location());
    } catch(const std::invalid_argument&) {
        result.issue = Issue::InvalidHttpsRemote;
        requires_check(result, Check::TransportRequiresCheck);
        return result;
    }
    auto selector = ValidatedGitRemoteSelector::default_head();
    if(source.selector().kind() == VcsSelectorKind::Branch) {
        if(!source.selector().value()) throw std::logic_error("Branch identity lacks its value.");
        result.branch_validation = validate_exact_git_branch(*source.selector().value());
        const auto* branch = std::get_if<ValidatedExactGitBranch>(&*result.branch_validation);
        if(!branch) {
            requires_check(result, Check::SelectorRequiresCheck);
            return result;
        }
        selector = ValidatedGitRemoteSelector::exact_branch(*branch);
    }
    // The only new #475 mint is inside this own-I/O owner, after P/I/R and
    // supported-source gates. No raw-tuple mint helper is exposed, even privately.
    const auto request = ValidatedGitRemoteRevisionRequest::make(
        AuthorityApprovedGitSourceIdentity(source),
        GitRemoteRevisionObservationKey::make(std::move(*remote), std::move(selector)));
    enter(result, Stage::Remote);
#ifdef MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
    if(g_assessment_hooks.remote)
        result.remote = g_assessment_hooks.remote(request);
    else
#endif
        result.remote = observe_git_remote_revision(request);
    const auto* observed = std::get_if<ObservedGitRemoteRevision>(&*result.remote);
    if(!observed) {
        result.assessment = DevelUpdateAssessment::unknown(remote_failure_reason(*result.remote));
        return result;
    }

    // All readers have returned their value snapshots: no DB/store lock is
    // retained across network. Success requests exactly one new local set,
    // with no retry/rebase/history search even when that set rejects the tip.
    result.post_check = DevelPackagePostCheck::Rejected;
    enter(result, Stage::PostProvenance);
    result.after.provenance = read_devel_build_provenance(base);
    enter(result, Stage::PostInstalled);
    result.after.installed = observe_current_installed_artifact_binding(child);
    enter(result, Stage::PostReviewed);
    result.after.reviewed = read_reviewed_source_state(base);
    result.stage = Stage::PostProvenance;
    if(!provenance_loaded(result, *result.after.provenance)) return result;
    const auto& after = std::get<DevelBuildProvenanceStoreLoaded>(*result.after.provenance);
    if(after.observed != selected.observed || after.provenance != baseline) {
        result.issue = Issue::ProvenanceTipChanged;
        requires_check(result, Check::NoAuthoritativeBuildProvenance);
        return result;
    }
    result.stage = Stage::PostInstalled;
    if(!installed_matches(result, result.after, baseline)) return result;
    if(std::get<CurrentInstalledArtifactBindingObserved>(*result.before.installed).world() !=
       std::get<CurrentInstalledArtifactBindingObserved>(*result.after.installed).world()) {
        result.issue = Issue::InstalledWorldChanged;
        requires_check(result, Check::InstalledArtifactDrift);
        return result;
    }
    result.stage = Stage::PostReviewed;
    if(!reviewed_matches(result, result.after, baseline)) return result;
    result.post_check = DevelPackagePostCheck::Validated;

    result.stage = Stage::Comparison;
    result.revision_comparison = compare_devel_git_revision(baseline.actual_built_revision().revision(), observed->revision());
    switch(*result.revision_comparison) {
        case DevelGitRevisionComparison::SameRevision: result.assessment = DevelUpdateAssessment::up_to_date(); break;
        case DevelGitRevisionComparison::DifferentRevision:
            result.assessment = DevelUpdateAssessment::update_available();
            result.update_basis = DevelPackageUpdateBasis::GitRevision;
            break;
        case DevelGitRevisionComparison::ObjectFormatMismatch:
        case DevelGitRevisionComparison::SourceMismatch:
            requires_check(result, Check::SourceIdentityChanged);
            return result;
        case DevelGitRevisionComparison::InvalidRevision:
            requires_check(result, Check::BuildSourceProofUnavailable);
            return result;
    }
    result.stage = Stage::Complete;
    return result;
}

DevelPackageAssessment assess_current_devel_package(const DevelPackageAssessmentTarget& target) {
    return DevelPackageAssessmentAuthority::assess(target);
}

#ifdef MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
void set_devel_package_assessment_test_hooks(DevelPackageAssessmentTestHooks hooks) {
    g_assessment_hooks = std::move(hooks);
}
#endif

DevelPackageLocalObservations observe_devel_bootstrap_local_state(const PackageChildIdentity& child) {
    DevelPackageLocalObservations out;
    out.provenance = read_devel_build_provenance(child.package_base());
    if(!std::holds_alternative<DevelBuildProvenanceStoreMissing>(*out.provenance)) return out;
    out.installed = observe_current_installed_artifact_binding(child);
    if(!std::holds_alternative<CurrentInstalledArtifactBindingObserved>(*out.installed)) return out;
    out.reviewed = read_reviewed_source_state(child.package_base());
    return out;
}
