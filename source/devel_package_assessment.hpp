#pragma once

#include "devel_package_assessment_authority.hpp"
#include "current_installed_artifact_binding_observer.hpp"
#include "devel_build_provenance_store.hpp"
#include "devel_build_provenance_reviewed_binding.hpp"
#include "devel_git_revision_comparison.hpp"
#include "devel_update_model.hpp"
#include "git_remote_revision_observer.hpp"

#include <optional>
#include <vector>

// Resolved target context, not observed P/I/R evidence. The future caller owns
// source resolution and installed-inventory grouping. A known devel hint only
// prevents Missing from becoming NotApplicable; it never authorizes network.
struct DevelPackageAssessmentTarget {
    PackageBaseIdentity package_base;
    std::vector<PackageChildIdentity> installed_children;
    bool known_devel_context = false;
};

enum class DevelPackageAssessmentStage {
    Target,
    Provenance,
    Installed,
    Reviewed,
    Source,
    Remote,
    PostProvenance,
    PostInstalled,
    PostReviewed,
    Comparison,
    Complete,
};

enum class DevelPackageAssessmentIssue {
    InvalidTarget,
    NoInstalledChild,
    MultipleInstalledChildren,
    SourceIdentityMismatch,
    InvalidHttpsRemote,
    UnsupportedSelector,
    ArchitectureSpecificSource,
    ProvenanceTipChanged,
    InstalledWorldChanged,
};

enum class DevelPackagePostCheck {
    NotAttempted,
    Rejected,
    Validated,
};

enum class DevelPackageUpdateBasis {
    GitRevision,
};

struct DevelPackageLocalObservations {
    std::optional<DevelBuildProvenanceStoreReadResult> provenance;
    std::optional<CurrentInstalledArtifactBindingObservation> installed;
    std::optional<ReviewedSourceStateStoreReadResult> reviewed;
    std::optional<InstalledArtifactBindingComparisonResult> installed_comparison;
    std::optional<ReviewedSourceStateRecordBindingComparison> reviewed_comparison;
};

// A pure decision/diagnostic product, deliberately public and copyable. P0's
// Loaded arm retains the selected tip and exact observed identity. These are
// sequential observation snapshots, never a transaction authorization/lease.
struct DevelPackageAssessment {
    DevelUpdateAssessment assessment = DevelUpdateAssessment::requires_check(
        DevelRequiresCheckReason::BuildSourceProofUnavailable);
    DevelPackageAssessmentStage stage = DevelPackageAssessmentStage::Target;
    std::optional<DevelPackageAssessmentIssue> issue;
    DevelPackageLocalObservations before;
    DevelPackageLocalObservations after;
    std::optional<ExactGitBranchValidationResult> branch_validation;
    std::optional<GitRemoteRevisionObservationResult> remote;
    std::optional<DevelGitRevisionComparison> revision_comparison;
    DevelPackagePostCheck post_check = DevelPackagePostCheck::NotAttempted;
    std::optional<DevelPackageUpdateBasis> update_basis;
};

// Tip-only P/I/R gates, one #475 observation, then one fresh P/I/R set on
// remote success. No RPC version policy, history adoption, publication or
// normal route activation. Resource/internal exceptions propagate to the
// existing command boundary rather than fabricating a remote failure.
[[nodiscard]] DevelPackageAssessment assess_current_devel_package(
    const DevelPackageAssessmentTarget& target);

#ifdef MOGUET_ENABLE_DEVEL_PACKAGE_ASSESSMENT_TEST_HOOKS
#include <functional>
struct DevelPackageAssessmentTestHooks {
    std::function<void(DevelPackageAssessmentStage)> before_stage;
    std::function<GitRemoteRevisionObservationResult(const ValidatedGitRemoteRevisionRequest&)> remote;
};
void set_devel_package_assessment_test_hooks(DevelPackageAssessmentTestHooks hooks);
#endif
