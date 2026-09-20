#include "invocation_owned_cleanup_model.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

void run_invocation_owned_cleanup_adapter_tests();
void run_trusted_alpm_receipt_tests();
void run_source_artifact_install_receipt_evidence_tests();
void run_remote_aur_cleanup_candidate_collector_tests();
void run_dependency_cleanup_interaction_tests();

// The focused target compiles the existing typed dependency requirement
// implementation but never evaluates a version constraint. Keeping this
// deterministic stub local proves that the cleanup classifier has no libalpm
// runtime dependency.
#ifndef MOGUET_ENABLE_REMOTE_AUR_CLEANUP_COLLECTOR_TEST_HOOKS
extern "C" int alpm_pkg_vercmp(const char* lhs, const char* rhs) {
    return std::strcmp(lhs, rhs);
}
#endif

namespace {

static_assert(
    !std::is_same_v<
        CleanupBaselineObservation, CleanupCausalOwnership>);
static_assert(
    !std::is_assignable_v<
        CleanupCausalOwnership&, CleanupBaselineObservation>);
static_assert(
    !std::is_same_v<
        CleanupCorrelationCoverage, CleanupEvidenceVerification>);
static_assert(!std::is_default_constructible_v<
              InvocationOwnedCleanupCandidate>);
static_assert(
    !std::is_default_constructible_v<CleanupClassificationResult>);
static_assert(!std::is_default_constructible_v<PacmanTransactionReceipt>);

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

bool has_reason(
    const CleanupClassificationResult& result,
    CleanupClassificationReason reason) {
    for(const CleanupClassificationReason candidate : result.reasons()) {
        if(candidate == reason) return true;
    }
    return false;
}

bool has_receipt_issue(
    const PacmanTransactionReceipt& receipt,
    PacmanTransactionReceiptIssueKind issue) {
    return std::find(
               receipt.issues().begin(), receipt.issues().end(), issue) !=
           receipt.issues().end();
}

std::string transaction_token(char digit = 'a') {
    return std::string(64, digit);
}

PacmanTransactionReceipt complete_receipt(
    std::vector<PacmanTransactionPackageObservation> operations,
    InvocationDependencyTransactionOwner owner =
        InvocationDependencyTransactionOwner::
            SelectedRepositoryProvider,
    const std::string& expected_token = transaction_token()) {
    return validate_pacman_transaction_receipt(
        expected_token, owner,
        PacmanTransactionReceiptObservation{
            PacmanTransactionReceiptObservationState::Complete,
            expected_token, owner, std::move(operations)});
}

void expect_classification(
    const InvocationOwnedCleanupCandidate& candidate,
    CleanupClassification expected,
    const std::string& message) {
    const CleanupClassificationResult result =
        classify_invocation_owned_cleanup(candidate);
    expect(result.classification() == expected, message);
}

void expect_reasons(
    const CleanupClassificationResult& result,
    std::vector<CleanupClassificationReason> expected,
    const std::string& message) {
    expect(result.reasons() == expected, message);
}

PackageSourceIdentity aur_source(const std::string& package_base) {
    return PackageSourceIdentity::aur(
        SourceLocationIdentity::known_git_remote(
            "https://aur.archlinux.org/" + package_base + ".git"));
}

PackageChildIdentity package_child(
    std::string package_base, std::string package_name) {
    PackageSourceIdentity source = aur_source(package_base);
    return PackageChildIdentity::make(
        PackageBaseIdentity::make(
            std::move(source), std::move(package_base)),
        std::move(package_name));
}

SourceAwarePackageIdentity cleanup_package_identity(
    PackageVersionIdentity package_version =
        PackageVersionIdentity::composite("1.0-1")) {
    return SourceAwarePackageIdentity::make(
        package_child("cleanup-tools", "cleanup-tool"),
        SourceRevisionIdentity::unknown(),
        std::move(package_version),
        PackageArchitectureIdentity::known({"x86_64"}));
}

DependencyRequirement dependency_requirement(
    std::string package_name = "cleanup-tool") {
    const std::string raw_specification = package_name;
    return ConsumerDependencyRequirement(
        raw_specification, std::move(package_name), std::nullopt);
}

CleanupProviderCorrelation selected_aur_provider() {
    return CleanupProviderCorrelation{
        ProvidedDependency::from_aur(
            "cleanup-tool", "cleanup-tools", "virtual-build-tool",
            "virtual-build-tool", "1.0-1"),
        ProviderResolutionKind::UserSelected};
}

CleanupPackageCorrelation dependency_correlation(
    const PackageChildIdentity& package,
    PackageRole role = PackageRole::BuildDependency,
    std::size_t root_index = 0,
    std::string root_name = "application-root",
    std::string requiring_base = "application-base",
    std::size_t edge_index = 0,
    std::optional<CleanupProviderCorrelation> provider = std::nullopt) {
    return CleanupPackageCorrelation{
        RootTargetIdentity{root_index, std::move(root_name)},
        package,
        role,
        CleanupDependencyEdgeCorrelation{
            edge_index,
            package_child(
                requiring_base, requiring_base + "-child"),
            dependency_requirement(
                provider.has_value() ? "virtual-build-tool"
                                     : "cleanup-tool"),
            std::move(provider)},
        CleanupEvidenceVerification::Verified};
}

InvocationOwnedCleanupCandidate eligible_candidate(
    PackageRole role = PackageRole::BuildDependency,
    PackageVersionIdentity package_version =
        PackageVersionIdentity::composite("1.0-1")) {
    SourceAwarePackageIdentity package =
        cleanup_package_identity(std::move(package_version));
    CleanupPackageCorrelation correlation =
        dependency_correlation(package.package(), role);
    return InvocationOwnedCleanupCandidate{
        std::move(package),
        CleanupBaselineObservation::NewlyObserved,
        CleanupCurrentPackageEvidence{
            CleanupInstalledState::Present,
            InstalledPackageMetadata{
                "cleanup-tool", "1.0-1",
                InstalledPackageReason::Dependency,
                InstalledPackageBaseIdentity::known("cleanup-tools"),
                InstalledPackageArchitectureIdentity::known("x86_64")},
            CleanupEvidenceVerification::Verified},
        CleanupCausalOwnership::Unknown,
        CleanupSharedRequirementState::NoLongerRequired,
        CleanupPolicyProtection::NotProtected,
        CleanupCorrelationCoverage::Complete,
        {std::move(correlation)}};
}

void test_make_or_check_only_is_eligible() {
    for(const PackageRole role : {
            PackageRole::BuildDependency,
            PackageRole::CheckDependency}) {
        InvocationOwnedCleanupCandidate candidate = eligible_candidate(role);
        candidate.causal_ownership = CleanupCausalOwnership::InvocationOwned;
        const CleanupClassificationResult result =
            classify_invocation_owned_cleanup(candidate);
        expect(result.classification() == CleanupClassification::Eligible,
               "Verified Make/Check-only evidence was not Eligible.");
        expect_reasons(
            result,
            {CleanupClassificationReason::EligibleEvidenceComplete},
            "Eligible classification reason is not canonical.");
    }
}

void test_complete_transaction_receipt_tracks_actual_installs() {
    const std::string token = transaction_token();
    PacmanTransactionReceipt receipt = complete_receipt({
        {PacmanTransactionPackageOperation::Install, "first-tool"},
        {PacmanTransactionPackageOperation::Install, "second-tool"},
        {PacmanTransactionPackageOperation::Upgrade, "existing-tool"},
    });

    expect(
        receipt.state() == PacmanTransactionReceiptState::Complete &&
            receipt.issues().empty() &&
            receipt.is_complete_for(
                token,
                InvocationDependencyTransactionOwner::
                    SelectedRepositoryProvider),
        "complete transaction receipt was not authoritative");
    expect(
        receipt.package_operations().size() == 3 &&
            receipt.newly_installed_packages() ==
                std::vector<PacmanInstalledPackageReceipt>{
                    {"first-tool"}, {"second-tool"}} &&
            receipt.contains_newly_installed_package("first-tool") &&
            !receipt.contains_newly_installed_package(
                "existing-tool"),
        "Install and Upgrade receipt operations were flattened");
}

void test_malformed_transaction_receipts_are_invalid() {
    PacmanTransactionReceipt duplicate = complete_receipt({
        {PacmanTransactionPackageOperation::Install, "duplicate-tool"},
        {PacmanTransactionPackageOperation::Install, "duplicate-tool"},
    });
    expect(
        duplicate.state() == PacmanTransactionReceiptState::Invalid &&
            has_receipt_issue(
                duplicate,
                PacmanTransactionReceiptIssueKind::
                    DuplicatePackageName) &&
            duplicate.newly_installed_packages().empty(),
        "duplicate package receipt was not invalid");

    PacmanTransactionReceipt invalid_name = complete_receipt({
        {PacmanTransactionPackageOperation::Install, "../unsafe"},
    });
    expect(
        invalid_name.state() == PacmanTransactionReceiptState::Invalid &&
            has_receipt_issue(
                invalid_name,
                PacmanTransactionReceiptIssueKind::
                    InvalidPackageName),
        "invalid package receipt was not invalid");

    PacmanTransactionReceipt unexpected_operation = complete_receipt({
        {PacmanTransactionPackageOperation::Remove, "removed-tool"},
    });
    expect(
        unexpected_operation.state() ==
                PacmanTransactionReceiptState::Invalid &&
            has_receipt_issue(
                unexpected_operation,
                PacmanTransactionReceiptIssueKind::
                    InvalidPackageOperation),
        "unexpected transaction operation was not invalid");
}

void test_missing_incomplete_and_mismatched_receipts_fail_closed() {
    const std::string token = transaction_token();
    const auto owner = InvocationDependencyTransactionOwner::
        SelectedRepositoryProvider;
    PacmanTransactionReceipt missing = validate_pacman_transaction_receipt(
        token, owner,
        PacmanTransactionReceiptObservation{
            PacmanTransactionReceiptObservationState::Missing,
            std::nullopt,
            std::nullopt,
            {}});
    expect(
        missing.state() == PacmanTransactionReceiptState::Unavailable &&
            has_receipt_issue(
                missing,
                PacmanTransactionReceiptIssueKind::ReceiptMissing),
        "missing receipt did not remain unavailable");

    PacmanTransactionReceipt incomplete = validate_pacman_transaction_receipt(
        token, owner,
        PacmanTransactionReceiptObservation{
            PacmanTransactionReceiptObservationState::Incomplete,
            token,
            owner,
            {{PacmanTransactionPackageOperation::Install,
              "partial-tool"}}});
    expect(
        incomplete.state() == PacmanTransactionReceiptState::Incomplete &&
            incomplete.newly_installed_packages().empty() &&
            has_receipt_issue(
                incomplete,
                PacmanTransactionReceiptIssueKind::
                    ReceiptIncomplete),
        "partial receipt exposed an Install package");

    PacmanTransactionReceipt mismatch = validate_pacman_transaction_receipt(
        token, owner,
        PacmanTransactionReceiptObservation{
            PacmanTransactionReceiptObservationState::Complete,
            transaction_token('b'),
            owner,
            {{PacmanTransactionPackageOperation::Install,
              "mismatch-tool"}}});
    expect(
        mismatch.state() == PacmanTransactionReceiptState::Invalid &&
            has_receipt_issue(
                mismatch,
                PacmanTransactionReceiptIssueKind::
                    TransactionTokenMismatch),
        "transaction token mismatch was not invalid");

    PacmanTransactionReceipt unexpected_missing_data =
        validate_pacman_transaction_receipt(
            token, owner,
            PacmanTransactionReceiptObservation{
                PacmanTransactionReceiptObservationState::Missing,
                token,
                owner,
                {}});
    expect(
        unexpected_missing_data.state() ==
                PacmanTransactionReceiptState::Invalid &&
            has_receipt_issue(
                unexpected_missing_data,
                PacmanTransactionReceiptIssueKind::
                    UnexpectedReceiptData),
        "unexpected pre-existing receipt data was accepted as missing");
}

void test_current_package_version_identity() {
    InvocationOwnedCleanupCandidate mismatch = eligible_candidate();
    mismatch.current_package.metadata->version = "2.0-1";
    const CleanupClassificationResult mismatch_result =
        classify_invocation_owned_cleanup(mismatch);
    expect(mismatch_result.classification() == CleanupClassification::Invalid &&
               has_reason(
                   mismatch_result,
                   CleanupClassificationReason::
                       CurrentPackageVersionMismatch),
           "Known current package version contradiction was not Invalid.");

    InvocationOwnedCleanupCandidate unknown = eligible_candidate(
        PackageRole::BuildDependency,
        PackageVersionIdentity::unknown());
    const CleanupClassificationResult unknown_result =
        classify_invocation_owned_cleanup(unknown);
    expect(unknown_result.classification() == CleanupClassification::Unknown &&
               has_reason(
                   unknown_result,
                   CleanupClassificationReason::
                       CurrentPackageVersionUnknown),
           "Unknown package version authority became Eligible.");

    InvocationOwnedCleanupCandidate unavailable = eligible_candidate(
        PackageRole::BuildDependency,
        PackageVersionIdentity::unavailable(
            IdentityUnavailableReason::ObservationFailed));
    const CleanupClassificationResult unavailable_result =
        classify_invocation_owned_cleanup(unavailable);
    expect(unavailable_result.classification() ==
                   CleanupClassification::Unknown &&
               has_reason(
                   unavailable_result,
                   CleanupClassificationReason::
                       CurrentPackageVersionUnavailable),
           "Unavailable package version authority became Eligible.");
}

void test_current_package_base_and_architecture_identity() {
    InvocationOwnedCleanupCandidate wrong_base = eligible_candidate();
    wrong_base.current_package.metadata->package_base =
        InstalledPackageBaseIdentity::known("other-tools");
    const CleanupClassificationResult wrong_base_result =
        classify_invocation_owned_cleanup(wrong_base);
    expect(
        wrong_base_result.classification() == CleanupClassification::Invalid &&
            has_reason(
                wrong_base_result,
                CleanupClassificationReason::CurrentPackageIdentityMismatch),
        "Wrong current PackageBase was not Invalid.");

    InvocationOwnedCleanupCandidate wrong_architecture = eligible_candidate();
    wrong_architecture.current_package.metadata->architecture =
        InstalledPackageArchitectureIdentity::known("aarch64");
    const CleanupClassificationResult wrong_architecture_result =
        classify_invocation_owned_cleanup(wrong_architecture);
    expect(
        wrong_architecture_result.classification() ==
                CleanupClassification::Invalid &&
            has_reason(
                wrong_architecture_result,
                CleanupClassificationReason::CurrentPackageIdentityMismatch),
        "Wrong current architecture was not Invalid.");

    const std::vector<InstalledPackageBaseIdentity> incomplete_package_bases{
        InstalledPackageBaseIdentity::missing(),
        InstalledPackageBaseIdentity::malformed(),
        InstalledPackageBaseIdentity::unavailable(),
        InstalledPackageBaseIdentity::unknown()};
    for(const InstalledPackageBaseIdentity& package_base :
        incomplete_package_bases) {
        InvocationOwnedCleanupCandidate candidate = eligible_candidate();
        candidate.current_package.metadata->package_base = package_base;
        const CleanupClassificationResult result =
            classify_invocation_owned_cleanup(candidate);
        expect(
            result.classification() == CleanupClassification::Unknown &&
                has_reason(
                    result,
                    CleanupClassificationReason::CurrentPackageBaseUnknown),
            "Incomplete current PackageBase became Eligible.");
    }

    const std::vector<InstalledPackageArchitectureIdentity>
        incomplete_architectures{
            InstalledPackageArchitectureIdentity::missing(),
            InstalledPackageArchitectureIdentity::malformed(),
            InstalledPackageArchitectureIdentity::unavailable(),
            InstalledPackageArchitectureIdentity::unknown()};
    for(const InstalledPackageArchitectureIdentity& architecture :
        incomplete_architectures) {
        InvocationOwnedCleanupCandidate candidate = eligible_candidate();
        candidate.current_package.metadata->architecture = architecture;
        const CleanupClassificationResult result =
            classify_invocation_owned_cleanup(candidate);
        expect(
            result.classification() == CleanupClassification::Unknown &&
                has_reason(
                    result,
                    CleanupClassificationReason::
                        CurrentPackageArchitectureUnknown),
            "Incomplete current architecture became Eligible.");
    }

    InvocationOwnedCleanupCandidate expected_architecture_unknown = eligible_candidate();
    expected_architecture_unknown.package = SourceAwarePackageIdentity::make(
        expected_architecture_unknown.package.package(),
        SourceRevisionIdentity::unknown(),
        PackageVersionIdentity::composite("1.0-1"),
        PackageArchitectureIdentity::unknown());
    expect_classification(
        expected_architecture_unknown, CleanupClassification::Unknown,
        "Unknown expected architecture became Eligible.");
}

void test_correlation_coverage_authority() {
    InvocationOwnedCleanupCandidate complete = eligible_candidate();
    expect_classification(
        complete, CleanupClassification::Eligible,
        "Complete correlation coverage did not permit Eligible.");

    InvocationOwnedCleanupCandidate unknown = eligible_candidate();
    unknown.correlation_coverage = CleanupCorrelationCoverage::Unknown;
    const CleanupClassificationResult unknown_result =
        classify_invocation_owned_cleanup(unknown);
    expect(unknown_result.classification() == CleanupClassification::Unknown &&
               has_reason(
                   unknown_result,
                   CleanupClassificationReason::
                       CorrelationCoverageUnknown),
           "Unknown correlation-set coverage became Eligible.");

    InvocationOwnedCleanupCandidate incomplete = eligible_candidate();
    incomplete.correlation_coverage = CleanupCorrelationCoverage::Incomplete;
    const CleanupClassificationResult incomplete_result =
        classify_invocation_owned_cleanup(incomplete);
    expect(incomplete_result.classification() ==
                   CleanupClassification::Unknown &&
               has_reason(
                   incomplete_result,
                   CleanupClassificationReason::
                       CorrelationCoverageIncomplete),
           "Incomplete correlation-set coverage became Eligible.");
    expect(incomplete.correlations[0].verification ==
               CleanupEvidenceVerification::Verified,
           "Incomplete-set regression did not retain a verified edge.");

    InvocationOwnedCleanupCandidate invalid = eligible_candidate();
    invalid.correlation_coverage =
        static_cast<CleanupCorrelationCoverage>(-1);
    const CleanupClassificationResult invalid_result =
        classify_invocation_owned_cleanup(invalid);
    expect(invalid_result.classification() == CleanupClassification::Invalid &&
               has_reason(
                   invalid_result,
                   CleanupClassificationReason::InvalidTypedState),
           "Invalid correlation coverage enum was not Invalid.");
}

void test_not_owned_and_invalid_causal_states() {
    InvocationOwnedCleanupCandidate not_owned = eligible_candidate();
    not_owned.causal_ownership =
        CleanupCausalOwnership::NotInvocationOwned;
    const CleanupClassificationResult protected_result =
        classify_invocation_owned_cleanup(not_owned);
    expect(protected_result.classification() ==
                   CleanupClassification::Protected &&
               has_reason(
                   protected_result,
                   CleanupClassificationReason::
                       KnownNotInvocationOwned),
           "Known-not-invocation-owned package was not Protected.");

    InvocationOwnedCleanupCandidate invalid = eligible_candidate();
    invalid.causal_ownership = static_cast<CleanupCausalOwnership>(-1);
    const CleanupClassificationResult invalid_result =
        classify_invocation_owned_cleanup(invalid);
    expect(invalid_result.classification() == CleanupClassification::Invalid,
           "Invalid causal enum became eligible after causal proof was relaxed.");
    expect_reasons(
        invalid_result, {CleanupClassificationReason::InvalidTypedState},
        "Invalid causal enum lost its structural reason.");
}

void test_pre_existing_is_always_protected() {
    InvocationOwnedCleanupCandidate ownership_unknown = eligible_candidate();
    ownership_unknown.baseline = CleanupBaselineObservation::PreExisting;
    ownership_unknown.causal_ownership = CleanupCausalOwnership::Unknown;
    expect_classification(
        ownership_unknown, CleanupClassification::Protected,
        "Pre-existing package with unknown ownership was not Protected.");

    InvocationOwnedCleanupCandidate eligible_looking = eligible_candidate();
    eligible_looking.causal_ownership = CleanupCausalOwnership::InvocationOwned;
    eligible_looking.baseline = CleanupBaselineObservation::PreExisting;
    const CleanupClassificationResult result =
        classify_invocation_owned_cleanup(eligible_looking);
    expect(result.classification() == CleanupClassification::Protected &&
               has_reason(
                   result, CleanupClassificationReason::PreExisting),
           "Pre-existing otherwise-eligible evidence became Eligible.");
}

void test_explicit_root_and_runtime_are_protected() {
    InvocationOwnedCleanupCandidate explicit_package = eligible_candidate();
    explicit_package.current_package.metadata->reason =
        InstalledPackageReason::Explicit;
    explicit_package.shared_requirement =
        CleanupSharedRequirementState::Unknown;
    expect_classification(
        explicit_package, CleanupClassification::Protected,
        "Explicit package was not Protected before shared Unknown.");

    InvocationOwnedCleanupCandidate root = eligible_candidate();
    root.correlations[0].role = PackageRole::Root;
    root.correlations[0].dependency_edge.reset();
    root.causal_ownership = CleanupCausalOwnership::Unknown;
    expect_classification(
        root, CleanupClassification::Protected,
        "Root target was not Protected before ownership Unknown.");

    InvocationOwnedCleanupCandidate runtime =
        eligible_candidate(PackageRole::RuntimeDependency);
    runtime.causal_ownership = CleanupCausalOwnership::Unknown;
    expect_classification(
        runtime, CleanupClassification::Protected,
        "Runtime dependency was not Protected before ownership Unknown.");
}

void test_mixed_runtime_roles_are_protected() {
    for(const PackageRole build_role : {
            PackageRole::BuildDependency,
            PackageRole::CheckDependency}) {
        InvocationOwnedCleanupCandidate candidate =
            eligible_candidate(build_role);
        candidate.correlation_coverage =
            CleanupCorrelationCoverage::Complete;
        candidate.correlations.push_back(dependency_correlation(
            candidate.package.package(), PackageRole::RuntimeDependency,
            1, "second-root", "second-base", 1));
        expect_classification(
            candidate, CleanupClassification::Protected,
            "Make/Check + Runtime package was not Protected.");
    }

    InvocationOwnedCleanupCandidate coverage_unknown = eligible_candidate();
    coverage_unknown.correlation_coverage =
        CleanupCorrelationCoverage::Unknown;
    coverage_unknown.correlations.push_back(dependency_correlation(
        coverage_unknown.package.package(),
        PackageRole::RuntimeDependency, 1, "second-root", "second-base",
        1));
    expect_classification(
        coverage_unknown, CleanupClassification::Protected,
        "Runtime protection did not precede Unknown set coverage.");
}

void test_shared_requirement_states() {
    InvocationOwnedCleanupCandidate required = eligible_candidate();
    required.shared_requirement =
        CleanupSharedRequirementState::StillRequired;
    expect_classification(
        required, CleanupClassification::Protected,
        "Still-required package was not Protected.");

    InvocationOwnedCleanupCandidate unknown = eligible_candidate();
    unknown.shared_requirement = CleanupSharedRequirementState::Unknown;
    expect_classification(
        unknown, CleanupClassification::Unknown,
        "Unknown shared lifetime did not remain Unknown.");
}

void test_unknown_baseline_reason_state_and_verification() {
    InvocationOwnedCleanupCandidate reason_unknown = eligible_candidate();
    reason_unknown.current_package.metadata->reason =
        InstalledPackageReason::Unknown;
    expect_classification(
        reason_unknown, CleanupClassification::Unknown,
        "Unknown install reason did not remain Unknown.");

    InvocationOwnedCleanupCandidate baseline_unknown = eligible_candidate();
    baseline_unknown.baseline = CleanupBaselineObservation::Unknown;
    expect_classification(
        baseline_unknown, CleanupClassification::Unknown,
        "Unknown baseline did not remain Unknown.");

    InvocationOwnedCleanupCandidate state_unknown = eligible_candidate();
    state_unknown.current_package.state = CleanupInstalledState::Unknown;
    state_unknown.current_package.metadata.reset();
    expect_classification(
        state_unknown, CleanupClassification::Unknown,
        "Unknown current installed state did not remain Unknown.");

    InvocationOwnedCleanupCandidate metadata_unavailable = eligible_candidate();
    metadata_unavailable.current_package.metadata.reset();
    expect_classification(
        metadata_unavailable, CleanupClassification::Unknown,
        "Present state without current metadata became Eligible.");

    InvocationOwnedCleanupCandidate unverified = eligible_candidate();
    unverified.current_package.verification =
        CleanupEvidenceVerification::Unverified;
    expect_classification(
        unverified, CleanupClassification::Unknown,
        "Unverified current identity became Eligible.");

    unverified = eligible_candidate();
    unverified.correlations[0].verification =
        CleanupEvidenceVerification::Unverified;
    expect_classification(
        unverified, CleanupClassification::Unknown,
        "Unverified correlation became Eligible.");

    InvocationOwnedCleanupCandidate missing_correlations = eligible_candidate();
    missing_correlations.correlations.clear();
    expect_classification(
        missing_correlations, CleanupClassification::Unknown,
        "Empty correlations became Eligible.");

    InvocationOwnedCleanupCandidate missing_edge = eligible_candidate();
    missing_edge.correlations[0].dependency_edge.reset();
    expect_classification(
        missing_edge, CleanupClassification::Unknown,
        "Build role without an exact dependency edge became Eligible.");
}

// POLICY(#486): complete ordinary safety evidence permits eligibility without
// turning the independent observation into a causal ownership assertion.
void test_newly_observed_dependency_with_complete_safety_evidence_is_eligible_without_strict_causal_proof() {
    for(const PackageRole role : {
            PackageRole::BuildDependency, PackageRole::CheckDependency}) {
        const InvocationOwnedCleanupCandidate candidate = eligible_candidate(role);
        expect(candidate.causal_ownership == CleanupCausalOwnership::Unknown,
               "Receipt-independent positive fixture claimed causal ownership.");
        const CleanupClassificationResult result =
            classify_invocation_owned_cleanup(candidate);
        expect(result.classification() == CleanupClassification::Eligible,
               "Complete newly-observed Dependency safety evidence required strict causal proof.");
        expect_reasons(
            result, {CleanupClassificationReason::EligibleEvidenceComplete},
            "Receipt-independent eligibility retained an unknown causal reason.");
        expect(candidate.causal_ownership == CleanupCausalOwnership::Unknown,
               "Eligibility changed the factual causal ownership evidence.");
    }
}

void test_selected_provider_does_not_classify_by_itself() {
    InvocationOwnedCleanupCandidate provider_only = eligible_candidate();
    provider_only.correlations[0] = dependency_correlation(
        provider_only.package.package(), PackageRole::BuildDependency, 0,
        "application-root", "application-base", 0,
        selected_aur_provider());
    provider_only.correlations[0].role.reset();
    expect_classification(
        provider_only, CleanupClassification::Unknown,
        "Selected provider fact alone became Eligible or Protected.");

    InvocationOwnedCleanupCandidate provider_build = eligible_candidate();
    provider_build.correlations[0] = dependency_correlation(
        provider_build.package.package(), PackageRole::BuildDependency,
        0, "application-root", "application-base", 0,
        selected_aur_provider());
    expect_classification(
        provider_build, CleanupClassification::Eligible,
        "Verified selected build provider could not become Eligible.");

    InvocationOwnedCleanupCandidate provider_runtime = eligible_candidate();
    provider_runtime.correlations[0] = dependency_correlation(
        provider_runtime.package.package(),
        PackageRole::RuntimeDependency, 0, "application-root",
        "application-base", 0, selected_aur_provider());
    expect_classification(
        provider_runtime, CleanupClassification::Protected,
        "Selected runtime provider was not Protected.");
}

void test_dependency_requirement_identity_contradictions_are_invalid() {
    InvocationOwnedCleanupCandidate direct = eligible_candidate();
    direct.correlations[0].dependency_edge->requirement =
        dependency_requirement("different-package");
    const CleanupClassificationResult direct_result =
        classify_invocation_owned_cleanup(direct);
    expect(direct_result.classification() == CleanupClassification::Invalid &&
               has_reason(
                   direct_result,
                   CleanupClassificationReason::
                       DependencyRequirementIdentityMismatch),
           "Direct dependency requirement contradiction was not Invalid.");

    InvocationOwnedCleanupCandidate provider = eligible_candidate();
    provider.correlations[0] = dependency_correlation(
        provider.package.package(), PackageRole::BuildDependency, 0,
        "application-root", "application-base", 0,
        selected_aur_provider());
    provider.correlations[0]
        .dependency_edge->provider->provider.provided_dependency_name =
        "different-virtual-package";
    const CleanupClassificationResult provider_result =
        classify_invocation_owned_cleanup(provider);
    expect(provider_result.classification() ==
                   CleanupClassification::Invalid &&
               has_reason(
                   provider_result,
                   CleanupClassificationReason::
                       ProviderRequirementIdentityMismatch),
           "Provider requirement contradiction was not Invalid.");

    InvocationOwnedCleanupCandidate unverified = eligible_candidate();
    unverified.correlations[0] = dependency_correlation(
        unverified.package.package(), PackageRole::BuildDependency, 0,
        "application-root", "application-base", 0,
        selected_aur_provider());
    unverified.correlations[0]
        .dependency_edge->provider->provider.provided_dependency_name
        .clear();
    unverified.correlations[0].verification =
        CleanupEvidenceVerification::Unverified;
    const CleanupClassificationResult unverified_result =
        classify_invocation_owned_cleanup(unverified);
    expect(unverified_result.classification() ==
                   CleanupClassification::Unknown &&
               has_reason(
                   unverified_result,
                   CleanupClassificationReason::
                       DependencyCorrelationUnverified),
           "Ambiguous provider association did not remain Unknown.");
}

void test_multiple_package_base_lifetime_is_preserved() {
    InvocationOwnedCleanupCandidate candidate = eligible_candidate();
    candidate.correlations.push_back(dependency_correlation(
        candidate.package.package(), PackageRole::CheckDependency, 1,
        "later-root", "later-package-base", 7));
    candidate.shared_requirement =
        CleanupSharedRequirementState::StillRequired;

    const CleanupClassificationResult result =
        classify_invocation_owned_cleanup(candidate);
    expect(candidate.correlations.size() == 2 &&
               candidate.correlations[0]
                       .dependency_edge->requiring_package
                       .package_base()
                       .package_base() !=
                   candidate.correlations[1]
                       .dependency_edge->requiring_package
                       .package_base()
                       .package_base() &&
               result.classification() ==
                   CleanupClassification::Protected &&
               has_reason(
                   result, CleanupClassificationReason::StillRequired),
           "Later PackageBase requirement was flattened or not Protected.");
}

void test_absent_and_policy_states_fail_safe() {
    InvocationOwnedCleanupCandidate absent = eligible_candidate();
    absent.current_package.state = CleanupInstalledState::Absent;
    absent.current_package.metadata.reset();
    expect_classification(
        absent, CleanupClassification::Protected,
        "Currently absent package was considered removable.");

    InvocationOwnedCleanupCandidate protected_by_policy = eligible_candidate();
    protected_by_policy.policy_protection =
        CleanupPolicyProtection::Protected;
    expect_classification(
        protected_by_policy, CleanupClassification::Protected,
        "Typed policy protection was not honored.");

    InvocationOwnedCleanupCandidate policy_unknown = eligible_candidate();
    policy_unknown.policy_protection = CleanupPolicyProtection::Unknown;
    expect_classification(
        policy_unknown, CleanupClassification::Unknown,
        "Unknown policy evidence became Eligible.");

    InvocationOwnedCleanupCandidate policy_not_protected_only =
        eligible_candidate();
    policy_not_protected_only.causal_ownership =
        CleanupCausalOwnership::Unknown;
    policy_not_protected_only.shared_requirement =
        CleanupSharedRequirementState::Unknown;
    expect_classification(
        policy_not_protected_only, CleanupClassification::Unknown,
        "NotProtected policy alone bypassed unknown shared lifetime.");
}

void test_structural_contradiction_is_invalid() {
    InvocationOwnedCleanupCandidate candidate = eligible_candidate();
    candidate.correlations[0].package =
        package_child("other-tools", "other-tool");
    const CleanupClassificationResult result =
        classify_invocation_owned_cleanup(candidate);
    expect(result.classification() == CleanupClassification::Invalid &&
               has_reason(
                   result,
                   CleanupClassificationReason::
                       CorrelationPackageIdentityMismatch),
           "Contradictory correlation identity was not Invalid.");

    InvocationOwnedCleanupCandidate wrong_name = eligible_candidate();
    wrong_name.current_package.metadata->name = "other-tool";
    expect_classification(
        wrong_name, CleanupClassification::Invalid,
        "Current package name mismatch was not Invalid.");

    InvocationOwnedCleanupCandidate malformed_root = eligible_candidate();
    malformed_root.correlations[0].requested_root.requested_name = "../root";
    const CleanupClassificationResult root_result =
        classify_invocation_owned_cleanup(malformed_root);
    expect(root_result.classification() == CleanupClassification::Invalid &&
               has_reason(root_result,
                          CleanupClassificationReason::MalformedRequestedRootIdentity),
           "Malformed requested root was not Invalid.");

    InvocationOwnedCleanupCandidate wrong_provider = eligible_candidate();
    wrong_provider.correlations[0] = dependency_correlation(
        wrong_provider.package.package(), PackageRole::BuildDependency, 0,
        "application-root", "application-base", 0, selected_aur_provider());
    wrong_provider.correlations[0].dependency_edge->provider->provider.package_name =
        "other-tool";
    const CleanupClassificationResult provider_result =
        classify_invocation_owned_cleanup(wrong_provider);
    expect(provider_result.classification() == CleanupClassification::Invalid &&
               has_reason(provider_result,
                          CleanupClassificationReason::ProviderIdentityMismatch),
           "Contradictory provider package identity was not Invalid.");
}

void test_precedence_and_reason_ordering() {
    InvocationOwnedCleanupCandidate protected_candidate = eligible_candidate();
    protected_candidate.baseline = CleanupBaselineObservation::PreExisting;
    protected_candidate.current_package.state = CleanupInstalledState::Absent;
    protected_candidate.current_package.metadata->reason =
        InstalledPackageReason::Explicit;
    protected_candidate.causal_ownership =
        CleanupCausalOwnership::NotInvocationOwned;
    protected_candidate.correlations[0].role = PackageRole::Root;
    protected_candidate.correlations[0].dependency_edge.reset();
    protected_candidate.correlations.push_back(dependency_correlation(
        protected_candidate.package.package(),
        PackageRole::RuntimeDependency, 1, "second-root", "second-base",
        1));
    protected_candidate.shared_requirement =
        CleanupSharedRequirementState::StillRequired;
    protected_candidate.policy_protection =
        CleanupPolicyProtection::Protected;

    const CleanupClassificationResult protected_result =
        classify_invocation_owned_cleanup(protected_candidate);
    expect(protected_result.classification() ==
               CleanupClassification::Protected,
           "Protection did not precede incomplete evidence.");
    expect_reasons(
        protected_result,
        {CleanupClassificationReason::PreExisting,
         CleanupClassificationReason::CurrentPackageAbsent,
         CleanupClassificationReason::ExplicitInstallReason,
         CleanupClassificationReason::KnownNotInvocationOwned,
         CleanupClassificationReason::RootTarget,
         CleanupClassificationReason::RuntimeDependency,
         CleanupClassificationReason::StillRequired,
         CleanupClassificationReason::PolicyProtected},
        "Protected reasons are not in deterministic canonical order.");

    InvocationOwnedCleanupCandidate unknown_candidate = eligible_candidate();
    unknown_candidate.baseline = CleanupBaselineObservation::Unknown;
    unknown_candidate.current_package.state = CleanupInstalledState::Unknown;
    unknown_candidate.current_package.metadata.reset();
    unknown_candidate.current_package.verification =
        CleanupEvidenceVerification::Unverified;
    unknown_candidate.causal_ownership = CleanupCausalOwnership::Unknown;
    unknown_candidate.correlations[0].role.reset();
    unknown_candidate.correlations[0].verification =
        CleanupEvidenceVerification::Unverified;
    unknown_candidate.shared_requirement =
        CleanupSharedRequirementState::Unknown;
    unknown_candidate.policy_protection = CleanupPolicyProtection::Unknown;

    const CleanupClassificationResult unknown_result =
        classify_invocation_owned_cleanup(unknown_candidate);
    expect_reasons(
        unknown_result,
        {CleanupClassificationReason::BaselineObservationUnknown,
         CleanupClassificationReason::CurrentInstalledStateUnknown,
         CleanupClassificationReason::InstallReasonUnknown,
         CleanupClassificationReason::CurrentPackageEvidenceUnverified,
         CleanupClassificationReason::DependencyRoleUnknown,
         CleanupClassificationReason::DependencyCorrelationUnverified,
         CleanupClassificationReason::
             BuildOrCheckDependencyCorrelationMissing,
         CleanupClassificationReason::SharedRequirementUnknown,
         CleanupClassificationReason::PolicyProtectionUnknown},
        "Unknown reasons are not in deterministic canonical order.");

    InvocationOwnedCleanupCandidate invalid_candidate = protected_candidate;
    invalid_candidate.current_package.metadata->name = "different-package";
    invalid_candidate.correlations[0].package =
        package_child("different-base", "different-package");
    invalid_candidate.correlations[0].dependency_edge =
        dependency_correlation(
            invalid_candidate.package.package(),
            PackageRole::BuildDependency)
            .dependency_edge;
    const CleanupClassificationResult invalid_result =
        classify_invocation_owned_cleanup(invalid_candidate);
    expect(invalid_result.classification() == CleanupClassification::Invalid,
           "Invalid evidence did not precede protection.");
    expect_reasons(
        invalid_result,
        {CleanupClassificationReason::CurrentPackageIdentityMismatch,
         CleanupClassificationReason::
             CorrelationPackageIdentityMismatch,
         CleanupClassificationReason::CorrelationShapeMismatch,
         CleanupClassificationReason::
             DependencyRequirementIdentityMismatch},
        "Invalid reasons are not in deterministic canonical order.");
}

} // namespace

void run_dependency_cleanup_execution_tests();

int main(int argc, char* argv[]) {
    try {
        if(argc == 2 && std::string(argv[1]) == "--execution-only") {
            run_dependency_cleanup_execution_tests();
            return 0;
        }
        if(argc == 2 && std::string(argv[1]) == "--interaction-only") {
            run_dependency_cleanup_interaction_tests();
            return 0;
        }
        if(argc == 2 &&
           std::string(argv[1]) == "--collector-only") {
            run_remote_aur_cleanup_candidate_collector_tests();
            return 0;
        }
        if(argc != 1) {
            throw std::runtime_error(
                "unknown invocation-owned cleanup test mode");
        }
        test_complete_transaction_receipt_tracks_actual_installs();
        test_malformed_transaction_receipts_are_invalid();
        test_missing_incomplete_and_mismatched_receipts_fail_closed();
        test_make_or_check_only_is_eligible();
        test_current_package_version_identity();
        test_current_package_base_and_architecture_identity();
        test_correlation_coverage_authority();
        test_not_owned_and_invalid_causal_states();
        test_pre_existing_is_always_protected();
        test_explicit_root_and_runtime_are_protected();
        test_mixed_runtime_roles_are_protected();
        test_shared_requirement_states();
        test_unknown_baseline_reason_state_and_verification();
        test_newly_observed_dependency_with_complete_safety_evidence_is_eligible_without_strict_causal_proof();
        test_selected_provider_does_not_classify_by_itself();
        test_dependency_requirement_identity_contradictions_are_invalid();
        test_multiple_package_base_lifetime_is_preserved();
        test_absent_and_policy_states_fail_safe();
        test_structural_contradiction_is_invalid();
        test_precedence_and_reason_ordering();
        run_invocation_owned_cleanup_adapter_tests();
        run_trusted_alpm_receipt_tests();
        run_source_artifact_install_receipt_evidence_tests();
        run_remote_aur_cleanup_candidate_collector_tests();
        run_dependency_cleanup_interaction_tests();
        run_dependency_cleanup_execution_tests();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "invocation-owned cleanup model tests: all checks passed\n";
    return 0;
}
