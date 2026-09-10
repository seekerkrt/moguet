#include "aur_update_plan.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

void run_devel_package_classification_tests();
void run_devel_update_model_tests();

namespace {

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename ExpectedException, typename Callable>
void expect_exception(Callable callable, const std::string& expected_message) {
    try {
        callable();
    } catch(const ExpectedException& error) {
        expect(
            std::string(error.what()) == expected_message,
            "Unexpected exception message: expected [" + expected_message +
                "], actual [" + error.what() + "]");
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Unexpected exception category: " + std::string(error.what()));
    }

    throw std::runtime_error("Expected exception: " + expected_message);
}

AurUpdatePlanInput remote_input(
    const std::string& installed_name,
    const std::string& installed_version,
    InstalledPackageReason install_reason,
    const std::string& aur_name,
    const std::string& package_base,
    const std::string& aur_version,
    AurVersionRelation version_relation) {
    return AurUpdatePlanInput{
        installed_name,
        installed_version,
        install_reason,
        AurUpdateRemotePackage{
            aur_name, package_base, aur_version, version_relation}};
}

AurUpdatePlanEntry five_field_entry(
    const std::string& installed_name,
    const std::string& package_base,
    AurVersionRelation version_relation,
    AurUpdateClassification classification) {
    return AurUpdatePlanEntry{
        installed_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            installed_name,
            package_base,
            version_relation == AurVersionRelation::NewerThanInstalled
                ? "2.0-1"
                : "1.0-1",
            version_relation},
        classification};
}

void test_newer_remote_version_is_update_available() {
    AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "sample-package", "1.0-1", InstalledPackageReason::Explicit,
        "sample-package", "sample-package", "2.0-1",
        AurVersionRelation::NewerThanInstalled));

    expect(
        entry.classification == AurUpdateClassification::UpdateAvailable,
        "Newer remote version was not classified as update available");
    expect(
        entry.install_reason == InstalledPackageReason::Explicit,
        "Update-available entry lost the explicit install reason");
    expect(entry.aur_package.has_value(), "Update entry lost AUR metadata");
    expect(entry.aur_package->version == "2.0-1", "Update entry AUR version differs");
}

void test_same_remote_version_is_up_to_date() {
    AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "sample-package", "1.0-1", InstalledPackageReason::Dependency,
        "sample-package", "sample-package", "1.0-1",
        AurVersionRelation::SameAsInstalled));

    expect(
        entry.classification == AurUpdateClassification::UpToDate,
        "Same remote version was not classified as up to date");
    expect(
        entry.install_reason == InstalledPackageReason::Dependency,
        "Up-to-date entry lost the dependency install reason");
}

void test_older_remote_version_is_up_to_date() {
    AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "sample-package", "2.0-1", InstalledPackageReason::Unknown,
        "sample-package", "sample-package", "1.0-1",
        AurVersionRelation::OlderThanInstalled));

    expect(
        entry.classification == AurUpdateClassification::UpToDate,
        "Older remote version was not classified as up to date");
}

void test_missing_aur_metadata_is_non_aur_foreign() {
    AurUpdatePlanEntry entry = classify_aur_update(AurUpdatePlanInput{
        "repository-only-package",
        "1.0-1",
        InstalledPackageReason::Unknown,
        AurUpdateMetadataNotFound{}});

    expect(
        entry.classification == AurUpdateClassification::NonAurForeign,
        "Missing AUR metadata was not classified as non-AUR foreign");
    expect(
        entry.install_reason == InstalledPackageReason::Unknown,
        "Non-AUR entry changed the unknown install reason");
    expect(!entry.aur_package.has_value(), "Non-AUR entry unexpectedly has AUR metadata");
}

void test_unavailable_metadata_remains_distinct() {
    AurUpdatePlanEntry entry = classify_aur_update(AurUpdatePlanInput{
        "unknown-package",
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateMetadataUnavailable{}});

    expect(
        entry.classification == AurUpdateClassification::MetadataUnavailable,
        "Unavailable metadata was not preserved as a distinct classification");
    expect(
        entry.install_reason == InstalledPackageReason::Explicit,
        "Metadata-unavailable entry lost the explicit install reason");
    expect(
        !entry.aur_package.has_value(),
        "Metadata-unavailable entry unexpectedly has AUR metadata");
}

void test_unavailable_version_comparison_remains_distinct() {
    AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "sample-package", "opaque-installed", InstalledPackageReason::Dependency,
        "sample-package", "sample-package", "opaque-remote",
        AurVersionRelation::Unavailable));

    expect(
        entry.classification ==
            AurUpdateClassification::VersionComparisonUnavailable,
        "Unavailable version comparison was not preserved");
    expect(
        entry.install_reason == InstalledPackageReason::Dependency,
        "Version-comparison failure lost the dependency install reason");
    expect(
        entry.aur_package.has_value(),
        "Version-comparison failure lost available AUR metadata");
}

void test_package_name_and_package_base_are_both_preserved() {
    AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "split-cli", "1.0-1", InstalledPackageReason::Explicit, "split-cli",
        "split-suite", "2.0-1", AurVersionRelation::NewerThanInstalled));

    expect(entry.installed_name == "split-cli", "Installed package name differs");
    expect(entry.aur_package.has_value(), "Split package entry lost AUR metadata");
    expect(entry.aur_package->aur_name == "split-cli", "AUR package name differs");
    expect(entry.aur_package->package_base == "split-suite", "PackageBase differs");
}

void test_plan_preserves_input_order() {
    const std::vector<AurUpdatePlanInput> inputs = {
        remote_input(
            "zeta-package", "1", InstalledPackageReason::Explicit,
            "zeta-package", "zeta-package", "2",
            AurVersionRelation::NewerThanInstalled),
        {"alpha-package",
         "1",
         InstalledPackageReason::Dependency,
         AurUpdateMetadataNotFound{}},
        remote_input(
            "middle-package", "1", InstalledPackageReason::Unknown,
            "middle-package", "middle-package", "1",
            AurVersionRelation::SameAsInstalled)};

    AurUpdatePlan plan = make_aur_update_plan(inputs);

    expect(plan.entries.size() == inputs.size(), "Plan entry count differs");
    expect(plan.entries[0].installed_name == "zeta-package", "First plan entry was reordered");
    expect(plan.entries[1].installed_name == "alpha-package", "Second plan entry was reordered");
    expect(plan.entries[2].installed_name == "middle-package", "Third plan entry was reordered");
    expect(
        plan.entries[0].install_reason == InstalledPackageReason::Explicit,
        "First plan entry lost its install reason");
    expect(
        plan.entries[1].install_reason == InstalledPackageReason::Dependency,
        "Second plan entry lost its install reason");
    expect(
        plan.entries[2].install_reason == InstalledPackageReason::Unknown,
        "Third plan entry lost its install reason");
}

void test_classifier_uses_relation_without_parsing_versions() {
    AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "sample-package", "not a pacman version", InstalledPackageReason::Unknown,
        "sample-package", "sample-package", "also:not/a/version",
        AurVersionRelation::NewerThanInstalled));

    expect(
        entry.classification == AurUpdateClassification::UpdateAvailable,
        "Classifier did not treat the supplied relation as authoritative");
    expect(
        entry.installed_version == "not a pacman version",
        "Classifier changed the opaque installed version");
    expect(entry.aur_package.has_value(), "Opaque version entry lost AUR metadata");
    expect(
        entry.aur_package->version == "also:not/a/version",
        "Classifier changed the opaque AUR version");
}

const DevelPackageClassification& require_devel_classification(
    const AurUpdatePlanEntry& entry,
    std::string_view context) {
    expect(
        entry.devel_classification.has_value(),
        std::string(context) + ": devel classification is missing");
    return *entry.devel_classification;
}

void test_normal_update_precedes_suffix_requires_check() {
    const AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "normal-newer-git", "1.0-1", InstalledPackageReason::Explicit,
        "normal-newer-git", "normal-newer-git", "2.0-1",
        AurVersionRelation::NewerThanInstalled));

    expect(
        entry.classification ==
            AurUpdateClassification::UpdateAvailable,
        "Normal suffix update lost its authoritative classification");
    expect(
        entry.devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck &&
            entry.devel_assessment.requires_check_reason() !=
                nullptr &&
            *entry.devel_assessment.requires_check_reason() ==
                DevelRequiresCheckReason::SuffixCandidateOnly,
        "Suffix candidate assessment was not retained orthogonally");
    expect(
        project_aur_update_effective_state(entry) ==
            AurUpdateEffectiveState::UpdateAvailable,
        "Devel RequiresCheck downgraded a normal version update");
}

void test_package_base_suffix_requires_check() {
    const AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "split-cli", "1.0-1", InstalledPackageReason::Explicit,
        "split-cli", "split-suite-git", "1.0-1",
        AurVersionRelation::SameAsInstalled));
    const DevelPackageClassification& devel =
        require_devel_classification(entry, "PackageBase suffix");
    const DevelPackageSuffixEvidence& suffix = devel.suffix_evidence();

    expect(
        suffix.package_base() == "split-suite-git" &&
            suffix.package_base_candidate_kind() != nullptr &&
            *suffix.package_base_candidate_kind() == VcsKind::Git,
        "PackageBase suffix evidence was not retained");
    expect(
        suffix.installed_children().size() == 1 &&
            suffix.installed_children().front().package_name() ==
                "split-cli" &&
            suffix.installed_children().front().candidate_kind() ==
                nullptr,
        "PackageBase suffix was flattened into child evidence");
    expect(
        entry.devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck &&
            project_aur_update_effective_state(entry) ==
                AurUpdateEffectiveState::RequiresCheck,
        "PackageBase suffix candidate was silently treated as up to date");
}

void test_installed_child_suffix_requires_check_without_confirming_base() {
    const AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "split-cli-git", "1.0-1", InstalledPackageReason::Dependency,
        "split-cli-git", "split-suite", "1.0-1",
        AurVersionRelation::SameAsInstalled));
    const DevelPackageClassification& devel =
        require_devel_classification(entry, "installed child suffix");
    const DevelPackageSuffixEvidence& suffix = devel.suffix_evidence();

    expect(
        suffix.package_base() == "split-suite" &&
            suffix.package_base_candidate_kind() == nullptr,
        "Child suffix promoted the PackageBase to a candidate");
    expect(
        suffix.installed_children().size() == 1 &&
            suffix.installed_children().front().candidate_kind() !=
                nullptr &&
            *suffix.installed_children().front().candidate_kind() ==
                VcsKind::Git,
        "Installed child suffix evidence was not retained");
    expect(
        devel.evidence_level() == DevelEvidenceLevel::SuffixCandidate &&
            devel.trusted_metadata().empty() &&
            devel.successful_build_confirmations().empty() &&
            project_aur_update_effective_state(entry) ==
                AurUpdateEffectiveState::RequiresCheck,
        "Child suffix was promoted to confirmed source authority");
}

void test_no_suffix_preserves_normal_up_to_date() {
    const AurUpdatePlanEntry entry = classify_aur_update(remote_input(
        "plain-package", "1.0-1", InstalledPackageReason::Explicit,
        "plain-package", "plain-package", "1.0-1",
        AurVersionRelation::SameAsInstalled));
    const DevelPackageClassification& devel =
        require_devel_classification(entry, "normal package");

    expect(
        devel.evidence_level() == DevelEvidenceLevel::Normal &&
            entry.devel_assessment.state() ==
                DevelUpdateAssessmentState::NotApplicable &&
            project_aur_update_effective_state(entry) ==
                AurUpdateEffectiveState::UpToDate,
        "No-suffix package changed its normal up-to-date semantics");
}

void test_five_field_package_base_suffix_fails_closed() {
    const AurUpdatePlanEntry entry = five_field_entry(
        "foo-git", "foo-git", AurVersionRelation::SameAsInstalled,
        AurUpdateClassification::UpToDate);

    expect(
        project_aur_update_effective_state(entry) ==
            AurUpdateEffectiveState::Inconsistent,
        "Five-field PackageBase suffix escaped as up to date");
}

void test_five_field_child_suffix_fails_closed() {
    const AurUpdatePlanEntry entry = five_field_entry(
        "foo-git", "foo", AurVersionRelation::SameAsInstalled,
        AurUpdateClassification::UpToDate);

    expect(
        project_aur_update_effective_state(entry) ==
            AurUpdateEffectiveState::Inconsistent,
        "Five-field installed child suffix escaped as up to date");
}

void test_five_field_ordinary_compatibility_remains_up_to_date() {
    const AurUpdatePlanEntry entry = five_field_entry(
        "foo-cli", "foo", AurVersionRelation::SameAsInstalled,
        AurUpdateClassification::UpToDate);

    expect(
        project_aur_update_effective_state(entry) ==
            AurUpdateEffectiveState::UpToDate,
        "Ordinary five-field compatibility entry stopped being up to date");
}

void test_current_requires_check_pair_is_exact() {
    AurUpdatePlanEntry future_reason = classify_aur_update(remote_input(
        "future-reason-git", "1.0-1",
        InstalledPackageReason::Explicit,
        "future-reason-git", "future-reason-git", "1.0-1",
        AurVersionRelation::SameAsInstalled));
    future_reason.devel_assessment =
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::
                NoAuthoritativeBuildProvenance);
    expect(
        project_aur_update_effective_state(future_reason) ==
            AurUpdateEffectiveState::Inconsistent,
        "Future RequiresCheck reason entered the conservative producer");

    AurUpdatePlanEntry missing_classification = five_field_entry(
        "missing-classification-git", "missing-classification-git",
        AurVersionRelation::SameAsInstalled,
        AurUpdateClassification::UpToDate);
    missing_classification.devel_assessment =
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::SuffixCandidateOnly);
    expect(
        project_aur_update_effective_state(missing_classification) ==
            AurUpdateEffectiveState::Inconsistent,
        "RequiresCheck without owned classification was accepted");

    const AurUpdatePlanEntry valid = classify_aur_update(remote_input(
        "valid-pair-git", "1.0-1", InstalledPackageReason::Explicit,
        "valid-pair-git", "valid-pair-git", "1.0-1",
        AurVersionRelation::SameAsInstalled));
    expect(
        project_aur_update_effective_state(valid) ==
            AurUpdateEffectiveState::RequiresCheck,
        "Current SuffixCandidateOnly pair was rejected");
}

void test_normal_update_precedes_missing_devel_projection() {
    const AurUpdatePlanEntry entry = five_field_entry(
        "legacy-update-git", "legacy-update-git",
        AurVersionRelation::NewerThanInstalled,
        AurUpdateClassification::UpdateAvailable);

    expect(
        project_aur_update_effective_state(entry) ==
            AurUpdateEffectiveState::UpdateAvailable,
        "Missing devel projection downgraded an authoritative normal update");
}

void test_confirmed_evidence_is_rejected_by_conservative_projection() {
    const VcsSourceIdentity source = VcsSourceIdentity::make(
        VcsKind::Git,
        "git+https://example.invalid/conservative-firewall.git",
        VcsSelector::default_head());
    const DevelPackageSuffixEvidence suffix =
        DevelPackageSuffixEvidence::classify(
            "confirmed-source", {"confirmed-source"});
    const DevelPackageClassification metadata_confirmed =
        DevelPackageClassification::classify(
            suffix,
            {TrustedDevelSourceMetadata::make(source)});
    const DevelPackageClassification build_confirmed =
        DevelPackageClassification::classify(
            suffix,
            {},
            {SuccessfulBuildSourceConfirmation::make(source)});

    expect_exception<std::logic_error>(
        [&metadata_confirmed]() {
            static_cast<void>(
                project_conservative_devel_update_assessment(
                    metadata_confirmed));
        },
        "Confirmed devel evidence is outside the conservative AUR update connection.");
    expect_exception<std::logic_error>(
        [&build_confirmed]() {
            static_cast<void>(
                project_conservative_devel_update_assessment(
                    build_confirmed));
        },
        "Confirmed devel evidence is outside the conservative AUR update connection.");
}

void test_non_aur_suffix_does_not_gain_devel_authority() {
    const AurUpdatePlanEntry entry = classify_aur_update(AurUpdatePlanInput{
        "foreign-only-git",
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateMetadataNotFound{}});

    expect(
        !entry.devel_classification.has_value() &&
            entry.devel_assessment.state() ==
                DevelUpdateAssessmentState::NotApplicable &&
            project_aur_update_effective_state(entry) ==
                AurUpdateEffectiveState::NonAurForeign,
        "Suffix-only foreign package gained AUR devel authority");
}

void test_failure_states_precede_suffix_assessment() {
    const AurUpdatePlanEntry metadata_failure = classify_aur_update(
        AurUpdatePlanInput{
            "metadata-failed-git",
            "1.0-1",
            InstalledPackageReason::Explicit,
            AurUpdateMetadataUnavailable{}});
    const AurUpdatePlanEntry comparison_failure = classify_aur_update(
        remote_input(
            "comparison-failed-git", "opaque-installed",
            InstalledPackageReason::Explicit,
            "comparison-failed-git", "comparison-failed-git",
            "opaque-remote", AurVersionRelation::Unavailable));

    expect(
        project_aur_update_effective_state(metadata_failure) ==
                AurUpdateEffectiveState::MetadataUnavailable &&
            !metadata_failure.devel_classification.has_value(),
        "Metadata failure was converted to devel success");
    expect(
        comparison_failure.devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck &&
            project_aur_update_effective_state(comparison_failure) ==
                AurUpdateEffectiveState::
                    VersionComparisonUnavailable,
        "Version comparison failure was replaced by RequiresCheck");
}

void test_unknown_version_relation_is_rejected() {
    AurUpdatePlanInput input = remote_input(
        "sample-package", "1.0-1", InstalledPackageReason::Unknown,
        "sample-package", "sample-package", "2.0-1",
        static_cast<AurVersionRelation>(4));

    expect_exception<std::logic_error>(
        [&input]() { static_cast<void>(classify_aur_update(input)); },
        "Unknown AUR version relation.");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

void test_authoritative_devel_effective_matrix() {
    using S = DevelUpdateAssessmentState;
    using E = AurUpdateEffectiveState;
    const std::vector<std::pair<DevelUpdateAssessment, E>> cases{
        {DevelUpdateAssessment::not_applicable(), E::UpToDate},
        {DevelUpdateAssessment::up_to_date(), E::UpToDate},
        {DevelUpdateAssessment::update_available(), E::UpdateAvailable},
        {DevelUpdateAssessment::requires_check(DevelRequiresCheckReason::ProvenanceMissing), E::RequiresCheck},
        {DevelUpdateAssessment::unknown(DevelUnknownReason::RemoteObservationFailed), E::Unknown},
        {DevelUpdateAssessment::unsupported(DevelUnsupportedReason::UnsupportedVcs), E::Unsupported}};
    for(const auto relation : {AurVersionRelation::SameAsInstalled, AurVersionRelation::OlderThanInstalled, AurVersionRelation::NewerThanInstalled}) {
        for(const auto& [assessment, expected] : cases) {
            auto entry = classify_aur_update(AurUpdatePlanInput{"matrix-git", "1", InstalledPackageReason::Explicit, AurUpdateRemotePackage{"matrix-git", "matrix", "1", relation}});
            entry.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
            entry.devel_assessment = assessment;
            const bool version = relation == AurVersionRelation::NewerThanInstalled;
            expect(project_aur_update_effective_state(entry) == (version ? E::UpdateAvailable : expected), "authoritative/RPC precedence mismatch");
            expect(aur_update_basis(entry) == (version ? std::optional{AurUpdateBasis::Version} : assessment.state() == S::UpdateAvailable ? std::optional{AurUpdateBasis::GitRevision}
                                                                                                                                           : std::nullopt),
                   "basis lost");
        }
    }
}

} // namespace

int main() {
    try {
        test_authoritative_devel_effective_matrix();
        run_case(
            "newer remote version is update available",
            test_newer_remote_version_is_update_available);
        run_case("same remote version is up to date", test_same_remote_version_is_up_to_date);
        run_case("older remote version is up to date", test_older_remote_version_is_up_to_date);
        run_case(
            "missing AUR metadata is non-AUR foreign",
            test_missing_aur_metadata_is_non_aur_foreign);
        run_case(
            "unavailable metadata remains distinct",
            test_unavailable_metadata_remains_distinct);
        run_case(
            "unavailable version comparison remains distinct",
            test_unavailable_version_comparison_remains_distinct);
        run_case(
            "package name and PackageBase are both preserved",
            test_package_name_and_package_base_are_both_preserved);
        run_case("plan preserves input order", test_plan_preserves_input_order);
        run_case(
            "classifier uses relation without parsing versions",
            test_classifier_uses_relation_without_parsing_versions);
        run_case(
            "normal update precedes suffix RequiresCheck",
            test_normal_update_precedes_suffix_requires_check);
        run_case(
            "PackageBase suffix requires check",
            test_package_base_suffix_requires_check);
        run_case(
            "installed child suffix requires check",
            test_installed_child_suffix_requires_check_without_confirming_base);
        run_case(
            "no suffix preserves normal up to date",
            test_no_suffix_preserves_normal_up_to_date);
        run_case(
            "five-field PackageBase suffix fails closed",
            test_five_field_package_base_suffix_fails_closed);
        run_case(
            "five-field child suffix fails closed",
            test_five_field_child_suffix_fails_closed);
        run_case(
            "five-field ordinary compatibility",
            test_five_field_ordinary_compatibility_remains_up_to_date);
        run_case(
            "current RequiresCheck pair is exact",
            test_current_requires_check_pair_is_exact);
        run_case(
            "normal update precedes missing devel projection",
            test_normal_update_precedes_missing_devel_projection);
        run_case(
            "confirmed evidence conservative firewall",
            test_confirmed_evidence_is_rejected_by_conservative_projection);
        run_case(
            "non-AUR suffix stays non-AUR",
            test_non_aur_suffix_does_not_gain_devel_authority);
        run_case(
            "failure states precede suffix assessment",
            test_failure_states_precede_suffix_assessment);
        run_case(
            "unknown version relation is rejected",
            test_unknown_version_relation_is_rejected);
        run_case(
            "devel package classification foundation",
            run_devel_package_classification_tests);
        run_case(
            "devel update assessment foundation",
            run_devel_update_model_tests);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "AUR update plan tests: all checks passed\n";
    return 0;
}
