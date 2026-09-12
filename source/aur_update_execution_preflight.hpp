#pragma once

#include "aur_update_plan.hpp"
#include "build_plan_artifact_target_projection.hpp"
#include "dependency_plan.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// RequiresCheckのexecution解釈はsaved source preferenceとは独立した
// route policyである。ordinary exact target-less -Syuは
// SkipIndependentTargetを使い、explicit upgrade routesはBlockOperationを使う。
enum class DevelRequiresCheckPolicy {
    BlockOperation,
    SkipIndependentTarget,
};

constexpr bool is_known_devel_requires_check_policy(
    DevelRequiresCheckPolicy policy) noexcept {
    switch(policy) {
        case DevelRequiresCheckPolicy::BlockOperation:
        case DevelRequiresCheckPolicy::SkipIndependentTarget:
            return true;
    }
    return false;
}

enum class AurUpdateExecutionSkipKind {
    UpToDate,
    NonAurForeign,
    IndependentDevelRequiresCheck,
    RequiredDevelRequiresCheck,
};

constexpr bool is_known_aur_update_execution_skip_kind(
    AurUpdateExecutionSkipKind kind) noexcept {
    switch(kind) {
        case AurUpdateExecutionSkipKind::UpToDate:
        case AurUpdateExecutionSkipKind::NonAurForeign:
        case AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck:
        case AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck:
            return true;
    }
    return false;
}

enum class AurUpdateRequiredDevelTargetRelation {
    AurExactDependency,
    AurProvider,
    RepositoryExactDependency,
    RepositoryProvider,
    RequiredArtifactChild,
    IdentityDrift,
};

constexpr bool is_known_aur_update_required_devel_target_relation(
    AurUpdateRequiredDevelTargetRelation relation) noexcept {
    switch(relation) {
        case AurUpdateRequiredDevelTargetRelation::AurExactDependency:
        case AurUpdateRequiredDevelTargetRelation::AurProvider:
        case AurUpdateRequiredDevelTargetRelation::
            RepositoryExactDependency:
        case AurUpdateRequiredDevelTargetRelation::RepositoryProvider:
        case AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild:
        case AurUpdateRequiredDevelTargetRelation::IdentityDrift:
            return true;
    }
    return false;
}

constexpr bool is_known_devel_requires_check_reason(
    DevelRequiresCheckReason reason) noexcept {
    switch(reason) {
        case DevelRequiresCheckReason::SuffixCandidateOnly:
        case DevelRequiresCheckReason::NoAuthoritativeBuildProvenance:
        case DevelRequiresCheckReason::InstalledArtifactDrift:
        case DevelRequiresCheckReason::AurRecipeAdvanced:
        case DevelRequiresCheckReason::SourceMetadataMissing:
        case DevelRequiresCheckReason::SourceMetadataMalformed:
        case DevelRequiresCheckReason::SourceIdentityChanged:
        case DevelRequiresCheckReason::TransportRequiresCheck:
        case DevelRequiresCheckReason::SelectorRequiresCheck:
        case DevelRequiresCheckReason::MultipleFloatingSources:
        case DevelRequiresCheckReason::ArchitectureSpecificSourceUnresolved:
        case DevelRequiresCheckReason::ProvenanceMissing:
        case DevelRequiresCheckReason::ProvenanceInvalid:
        case DevelRequiresCheckReason::ProvenanceCorrupted:
        case DevelRequiresCheckReason::ProvenanceFutureSchema:
        case DevelRequiresCheckReason::BuildSourceProofUnavailable:
            return true;
    }
    return false;
}

// BuildPlan上の再登場をaffected rootへ帰属させるSkipIndependentTarget
// capabilityのlossless record。ordinary exact target-less -Syuで使用し、
// explicit upgrade routesはBlockOperationを維持する。
struct AurUpdateRequiredDevelTargetBlocker {
    AurUpdateRequiredDevelTargetBlocker() = delete;

    AurUpdateRequiredDevelTargetBlocker(
        AurUpdateRequiredDevelTargetRelation relation_value,
        std::size_t requires_check_update_plan_index_value,
        std::string package_name_value,
        DevelRequiresCheckReason devel_requires_check_reason_value)
        : relation(relation_value),
          requires_check_update_plan_index(
              requires_check_update_plan_index_value),
          package_name(std::move(package_name_value)),
          devel_requires_check_reason(
              devel_requires_check_reason_value) {
    }

    AurUpdateRequiredDevelTargetRelation relation;
    std::size_t requires_check_update_plan_index;
    std::optional<std::size_t> dependency_edge_index;
    std::optional<std::size_t> build_plan_order_index;
    std::string package_name;
    std::optional<std::string> package_base;
    std::vector<PackageRole> roles;
    DevelRequiresCheckReason devel_requires_check_reason;
    std::vector<RootTargetIdentity> affected_roots;

    bool operator==(
        const AurUpdateRequiredDevelTargetBlocker&) const = default;
};

enum class AurUpdateExecutionTargetStatus {
    Executable,
    Skipped,
    Unsupported,
    Incomplete
};

enum class AurUpdateExecutionReason {
    None,

    UpToDate,
    DevelRequiresCheck,
    DevelObservationUnknown,
    DevelUnsupported,
    RequiredDevelTargetRequiresCheck,
    NonAurForeign,

    AurMetadataUnavailable,
    VersionComparisonUnavailable,
    InstalledReasonUnknown,
    UpdatePlanInconsistent,
    DuplicateUpdateTarget,

    RepositoryMetadataUnavailable,
    AurDependencyMetadataUnavailable,
    ProviderMetadataUnavailable,
    UnresolvedDependency,
    VersionConstraintUnverified,
    DependencyCycle,
    BuildPlanInconsistent,
    PackageBaseMismatch,

    SplitPackageSelectionRequired,
    MultiplePackageTargetsForPackageBase,
    AmbiguousProvider,
    ConflictsOrReplacesUnresolved,

    InstalledPackageMetadataUnavailable
};

struct AurUpdateExecutionIssue {
    AurUpdateExecutionReason reason = AurUpdateExecutionReason::None;
    std::optional<std::string> package_name;
    std::optional<std::string> package_base;
    std::optional<std::string> dependency_specification;
    std::string diagnostic;
    std::optional<DevelRequiresCheckReason> devel_requires_check_reason;
    std::optional<BuildPlanArtifactTargetProjectionIssue>
        build_plan_projection_issue;
    std::optional<PlanDeclaredRelationReason> relation_reason;
    std::optional<AurUpdateRequiredDevelTargetBlocker>
        required_devel_target_blocker;

    AurUpdateExecutionIssue() = default;

    AurUpdateExecutionIssue(
        AurUpdateExecutionReason reason_value,
        std::optional<std::string> package_name_value,
        std::optional<std::string> package_base_value,
        std::optional<std::string> dependency_specification_value,
        std::string diagnostic_value,
        std::optional<BuildPlanArtifactTargetProjectionIssue>
            projection_issue = std::nullopt,
        std::optional<PlanDeclaredRelationReason>
            relation_reason_value = std::nullopt,
        std::optional<DevelRequiresCheckReason>
            devel_requires_check_reason_value = std::nullopt,
        std::optional<AurUpdateRequiredDevelTargetBlocker>
            required_devel_target_blocker_value = std::nullopt)
        : reason(reason_value),
          package_name(std::move(package_name_value)),
          package_base(std::move(package_base_value)),
          dependency_specification(
              std::move(dependency_specification_value)),
          diagnostic(std::move(diagnostic_value)),
          devel_requires_check_reason(
              devel_requires_check_reason_value),
          build_plan_projection_issue(std::move(projection_issue)),
          relation_reason(std::move(relation_reason_value)),
          required_devel_target_blocker(
              std::move(required_devel_target_blocker_value)) {
    }
};

inline bool is_known_required_devel_package_role(
    PackageRole role) noexcept {
    switch(role) {
        case PackageRole::RuntimeDependency:
        case PackageRole::BuildDependency:
        case PackageRole::CheckDependency:
            return true;
        case PackageRole::Root:
            return false;
    }
    return false;
}

inline bool is_valid_aur_update_required_devel_target_blocker(
    const AurUpdateRequiredDevelTargetBlocker& blocker) noexcept {
    if(!is_known_aur_update_required_devel_target_relation(
           blocker.relation) ||
       blocker.package_name.empty() ||
       !blocker.package_base.has_value() ||
       blocker.package_base->empty() ||
       (blocker.roles.empty() &&
        blocker.relation !=
            AurUpdateRequiredDevelTargetRelation::IdentityDrift) ||
       !std::all_of(
           blocker.roles.begin(), blocker.roles.end(),
           is_known_required_devel_package_role) ||
       !is_known_devel_requires_check_reason(
           blocker.devel_requires_check_reason)) {
        return false;
    }

    switch(blocker.relation) {
        case AurUpdateRequiredDevelTargetRelation::AurExactDependency:
        case AurUpdateRequiredDevelTargetRelation::AurProvider:
        case AurUpdateRequiredDevelTargetRelation::
            RepositoryExactDependency:
        case AurUpdateRequiredDevelTargetRelation::RepositoryProvider:
            return blocker.dependency_edge_index.has_value();
        case AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild:
            return blocker.build_plan_order_index.has_value();
        case AurUpdateRequiredDevelTargetRelation::IdentityDrift:
            return blocker.dependency_edge_index.has_value() ||
                   blocker.build_plan_order_index.has_value();
    }
    return false;
}

inline bool is_valid_aur_update_execution_issue_devel_payload(
    const AurUpdateExecutionIssue& issue) noexcept {
    if(issue.reason == AurUpdateExecutionReason::DevelRequiresCheck) {
        return issue.devel_requires_check_reason.has_value() &&
               is_known_devel_requires_check_reason(
                   *issue.devel_requires_check_reason) &&
               !issue.required_devel_target_blocker.has_value();
    }
    if(issue.reason == AurUpdateExecutionReason::
                           RequiredDevelTargetRequiresCheck) {
        return issue.devel_requires_check_reason.has_value() &&
               issue.required_devel_target_blocker.has_value() &&
               is_valid_aur_update_required_devel_target_blocker(
                   *issue.required_devel_target_blocker) &&
               issue.required_devel_target_blocker
                       ->devel_requires_check_reason ==
                   *issue.devel_requires_check_reason;
    }
    return !issue.devel_requires_check_reason.has_value() &&
           !issue.required_devel_target_blocker.has_value();
}

inline bool is_valid_aur_update_normal_skip_snapshot(
    const AurUpdatePlanEntry& update,
    const std::vector<AurUpdateExecutionIssue>& issues,
    const std::optional<AurUpdateExecutionSkipKind>& skip_kind) noexcept {
    if(skip_kind !=
           std::optional<AurUpdateExecutionSkipKind>{
               AurUpdateExecutionSkipKind::UpToDate} &&
       skip_kind !=
           std::optional<AurUpdateExecutionSkipKind>{
               AurUpdateExecutionSkipKind::NonAurForeign}) {
        return false;
    }

    std::size_t normal_skip_issue_count = 0;
    bool has_matching_reason = false;
    for(const auto& issue : issues) {
        if(issue.devel_requires_check_reason.has_value() ||
           issue.required_devel_target_blocker.has_value() ||
           issue.dependency_specification.has_value() ||
           issue.build_plan_projection_issue.has_value() ||
           issue.relation_reason.has_value()) {
            return false;
        }
        if(issue.reason == AurUpdateExecutionReason::None) continue;
        if(issue.reason == AurUpdateExecutionReason::UpToDate) {
            ++normal_skip_issue_count;
            has_matching_reason = has_matching_reason ||
                                  *skip_kind ==
                                      AurUpdateExecutionSkipKind::UpToDate;
            continue;
        }
        if(issue.reason == AurUpdateExecutionReason::NonAurForeign) {
            ++normal_skip_issue_count;
            has_matching_reason =
                has_matching_reason ||
                *skip_kind ==
                    AurUpdateExecutionSkipKind::NonAurForeign;
            continue;
        }
        return false;
    }
    if(normal_skip_issue_count != 1 || !has_matching_reason) return false;

    switch(*skip_kind) {
        case AurUpdateExecutionSkipKind::UpToDate:
            return update.classification ==
                       AurUpdateClassification::UpToDate &&
                   update.aur_package.has_value() &&
                   (update.devel_assessment == DevelUpdateAssessment::not_applicable() ||
                    (update.devel_assessment_origin == AurDevelAssessmentOrigin::CurrentObservation &&
                     update.devel_assessment.state() == DevelUpdateAssessmentState::UpToDate)) &&
                   (update.aur_package->version_relation ==
                        AurVersionRelation::OlderThanInstalled ||
                    update.aur_package->version_relation ==
                        AurVersionRelation::SameAsInstalled);
        case AurUpdateExecutionSkipKind::NonAurForeign:
            return update.classification ==
                       AurUpdateClassification::NonAurForeign &&
                   !update.aur_package.has_value() &&
                   update.devel_assessment ==
                       DevelUpdateAssessment::not_applicable();
        case AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck:
        case AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck:
            return false;
    }
    return false;
}

inline bool has_valid_requires_check_package_identity_shape(
    const std::string& name) noexcept {
    // Keep this snapshot firewall header-only: preparation/result link
    // profiles must not acquire the package_identifier implementation. The
    // accepted ASCII set is the same closed package-name shape used there.
    if(name.empty() || name == "." || name == ".." || name.front() == '-') {
        return false;
    }
    return std::all_of(
        name.begin(), name.end(), [](const unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '@' || character == '.' ||
                   character == '_' || character == '+' ||
                   character == '-';
        });
}

inline bool has_consistent_requires_check_producer_snapshot(
    const AurUpdatePlanEntry& update,
    DevelRequiresCheckReason reason) noexcept {
    if(update.devel_assessment_origin == AurDevelAssessmentOrigin::CurrentObservation) {
        return is_known_devel_requires_check_reason(reason) && update.aur_package &&
               update.installed_name == update.aur_package->aur_name &&
               update.devel_assessment == DevelUpdateAssessment::requires_check(reason);
    }
    if(update.devel_assessment_origin != AurDevelAssessmentOrigin::Conservative || reason != DevelRequiresCheckReason::SuffixCandidateOnly ||
       !update.devel_classification.has_value() ||
       update.devel_assessment !=
           DevelUpdateAssessment::requires_check(reason) ||
       !update.aur_package.has_value()) {
        return false;
    }
    const DevelPackageClassification& classification =
        *update.devel_classification;
    const DevelPackageSuffixEvidence& suffix =
        classification.suffix_evidence();
    return classification.evidence_level() ==
               DevelEvidenceLevel::SuffixCandidate &&
           classification.source_form_disposition() ==
               DevelSourceFormDisposition::RequiresCheck &&
           suffix.package_base() == update.aur_package->package_base &&
           suffix.installed_children().size() == 1 &&
           suffix.installed_children().front().package_name() ==
               update.installed_name &&
           suffix.has_candidate() &&
           classification.trusted_metadata().empty() &&
           classification.successful_build_confirmations().empty();
}

inline bool has_valid_aur_update_requires_check_identity_snapshot(
    const AurUpdatePlanEntry& update,
    DevelRequiresCheckReason reason) noexcept {
    return is_known_devel_requires_check_reason(reason) &&
           update.classification == AurUpdateClassification::UpToDate &&
           update.aur_package.has_value() &&
           has_valid_requires_check_package_identity_shape(
               update.installed_name) &&
           !update.installed_version.empty() &&
           update.installed_name == update.aur_package->aur_name &&
           has_valid_requires_check_package_identity_shape(
               update.aur_package->aur_name) &&
           has_valid_requires_check_package_identity_shape(
               update.aur_package->package_base) &&
           !update.aur_package->version.empty() &&
           (update.aur_package->version_relation ==
                AurVersionRelation::OlderThanInstalled ||
            update.aur_package->version_relation ==
                AurVersionRelation::SameAsInstalled) &&
           has_consistent_requires_check_producer_snapshot(update, reason);
}

inline bool is_valid_aur_update_devel_requires_check_skip_snapshot(
    const AurUpdatePlanEntry& update,
    const std::vector<AurUpdateExecutionIssue>& issues,
    const std::optional<AurUpdateExecutionSkipKind>& skip_kind,
    DevelRequiresCheckPolicy policy,
    AurUpdateExecutionSkipKind expected_skip_kind) noexcept {
    if(policy != DevelRequiresCheckPolicy::SkipIndependentTarget ||
       (expected_skip_kind !=
            AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck &&
        expected_skip_kind !=
            AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck) ||
       skip_kind !=
           std::optional<AurUpdateExecutionSkipKind>{expected_skip_kind} ||
       issues.size() != 1) {
        return false;
    }

    const AurUpdateExecutionIssue& issue = issues.front();
    if(!issue.devel_requires_check_reason.has_value() ||
       !is_known_devel_requires_check_reason(
           *issue.devel_requires_check_reason) ||
       !has_valid_aur_update_requires_check_identity_snapshot(
           update, *issue.devel_requires_check_reason)) {
        return false;
    }

    return issue.reason == AurUpdateExecutionReason::DevelRequiresCheck &&
           issue.package_name ==
               std::optional<std::string>{update.installed_name} &&
           issue.package_base ==
               std::optional<std::string>{
                   update.aur_package->package_base} &&
           !issue.dependency_specification.has_value() &&
           !issue.diagnostic.empty() &&
           !issue.build_plan_projection_issue.has_value() &&
           !issue.relation_reason.has_value() &&
           !issue.required_devel_target_blocker.has_value();
}

inline bool is_valid_aur_update_independent_devel_skip_snapshot(
    const AurUpdatePlanEntry& update,
    const std::vector<AurUpdateExecutionIssue>& issues,
    const std::optional<AurUpdateExecutionSkipKind>& skip_kind,
    DevelRequiresCheckPolicy policy) noexcept {
    return is_valid_aur_update_devel_requires_check_skip_snapshot(
        update, issues, skip_kind, policy,
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck);
}

inline bool is_valid_aur_update_required_devel_skip_snapshot(
    const AurUpdatePlanEntry& update,
    const std::vector<AurUpdateExecutionIssue>& issues,
    const std::optional<AurUpdateExecutionSkipKind>& skip_kind,
    DevelRequiresCheckPolicy policy) noexcept {
    return is_valid_aur_update_devel_requires_check_skip_snapshot(
        update, issues, skip_kind, policy,
        AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck);
}

struct AurUpdateExecutionTarget {
    std::size_t update_plan_index = 0;
    std::optional<std::size_t> build_plan_root_index;
    AurUpdatePlanEntry update;
    AurUpdateExecutionTargetStatus status =
        AurUpdateExecutionTargetStatus::Incomplete;
    std::optional<DesiredInstallReason> desired_install_reason;
    std::vector<AurUpdateExecutionIssue> issues;
    std::optional<AurUpdateExecutionSkipKind> skip_kind;
};

inline bool is_valid_aur_update_execution_target_skip_snapshot(
    const AurUpdateExecutionTarget& target,
    DevelRequiresCheckPolicy policy) noexcept {
    if(std::any_of(
           target.issues.begin(), target.issues.end(),
           [](const AurUpdateExecutionIssue& issue) {
               return !is_valid_aur_update_execution_issue_devel_payload(
                   issue);
           })) {
        return false;
    }
    switch(target.status) {
        case AurUpdateExecutionTargetStatus::Executable:
            // CONTRACT(#508): Executable is authority-free. Even a None
            // issue is a malformed retained snapshot, not a placeholder.
            return !target.skip_kind.has_value() && target.issues.empty();
        case AurUpdateExecutionTargetStatus::Skipped:
            return !target.build_plan_root_index.has_value() &&
                   !target.desired_install_reason.has_value() &&
                   (is_valid_aur_update_normal_skip_snapshot(
                        target.update, target.issues,
                        target.skip_kind) ||
                    is_valid_aur_update_independent_devel_skip_snapshot(
                        target.update, target.issues,
                        target.skip_kind, policy) ||
                    is_valid_aur_update_required_devel_skip_snapshot(
                        target.update, target.issues,
                        target.skip_kind, policy));
        case AurUpdateExecutionTargetStatus::Unsupported:
        case AurUpdateExecutionTargetStatus::Incomplete:
            return !target.skip_kind.has_value();
    }
    return false;
}

inline bool is_valid_aur_update_execution_target_skip_snapshot(
    const AurUpdateExecutionTarget& target) noexcept {
    return is_valid_aur_update_execution_target_skip_snapshot(
        target, DevelRequiresCheckPolicy::BlockOperation);
}

struct AurUpdateExecutionPreflight {
    std::vector<AurUpdateExecutionTarget> targets;
    std::optional<BuildPlan> build_plan;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;
};

inline bool required_devel_blocker_references_target(
    const AurUpdateExecutionIssue& issue, const AurUpdateExecutionTarget& target) noexcept {
    if(issue.reason != AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck ||
       !is_valid_aur_update_execution_issue_devel_payload(issue) || !target.update.aur_package) return false;
    const bool bootstrap = has_aur_update_bootstrap_intent(target.update);
    if(bootstrap) {
        if(target.status != AurUpdateExecutionTargetStatus::Incomplete ||
           !std::any_of(target.issues.begin(), target.issues.end(), [](const auto& retained) {
               return retained.reason == AurUpdateExecutionReason::DevelRequiresCheck &&
                      retained.devel_requires_check_reason == DevelRequiresCheckReason::ProvenanceMissing;
           })) return false;
    } else if(target.skip_kind != AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck || target.issues.size() != 1) {
        return false;
    }
    const auto& blocker = *issue.required_devel_target_blocker;
    const auto* reason = target.update.devel_assessment.requires_check_reason();
    return blocker.requires_check_update_plan_index == target.update_plan_index &&
           blocker.package_name == target.update.installed_name && blocker.package_name == target.update.aur_package->aur_name &&
           blocker.package_base == std::optional<std::string>{target.update.aur_package->package_base} &&
           reason && blocker.devel_requires_check_reason == *reason;
}

inline bool has_valid_aur_update_execution_policy_snapshot(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    if(!preflight.devel_requires_check_policy.has_value() ||
       !is_known_devel_requires_check_policy(
           *preflight.devel_requires_check_policy)) {
        return false;
    }
    for(const auto& target : preflight.targets) {
        if(!is_valid_aur_update_execution_target_skip_snapshot(
               target, *preflight.devel_requires_check_policy)) {
            return false;
        }
    }
    if(*preflight.devel_requires_check_policy !=
       DevelRequiresCheckPolicy::SkipIndependentTarget) {
        return true;
    }
    for(std::size_t owner_position = 0;
        owner_position < preflight.targets.size(); ++owner_position) {
        for(const AurUpdateExecutionIssue& issue :
            preflight.targets[owner_position].issues) {
            if(issue.reason != AurUpdateExecutionReason::
                                   RequiredDevelTargetRequiresCheck) {
                continue;
            }
            std::size_t referenced_target_count = 0;
            for(std::size_t target_position = 0;
                target_position < preflight.targets.size();
                ++target_position) {
                const AurUpdateExecutionTarget& target =
                    preflight.targets[target_position];
                if(required_devel_blocker_references_target(
                       issue, target)) {
                    if(!has_aur_update_bootstrap_intent(target.update) &&
                       (owner_position == target_position || target.skip_kind !=
                                                                 std::optional<AurUpdateExecutionSkipKind>{
                                                                     AurUpdateExecutionSkipKind::
                                                                         RequiredDevelRequiresCheck})) {
                        return false;
                    }
                    ++referenced_target_count;
                }
            }
            if(referenced_target_count != 1) return false;
        }
    }
    for(std::size_t target_position = 0;
        target_position < preflight.targets.size(); ++target_position) {
        const AurUpdateExecutionTarget& target =
            preflight.targets[target_position];
        const bool is_independent =
            target.skip_kind ==
            std::optional<AurUpdateExecutionSkipKind>{
                AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck};
        const bool is_required =
            target.skip_kind ==
            std::optional<AurUpdateExecutionSkipKind>{
                AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck};
        if(!is_independent && !is_required) continue;

        std::size_t update_index_count = 0;
        std::size_t blocker_reference_count = 0;
        for(std::size_t owner_position = 0;
            owner_position < preflight.targets.size(); ++owner_position) {
            const AurUpdateExecutionTarget& owner =
                preflight.targets[owner_position];
            if(owner.update_plan_index == target.update_plan_index) {
                ++update_index_count;
            }
            for(const AurUpdateExecutionIssue& issue : owner.issues) {
                if(required_devel_blocker_references_target(
                       issue, target)) {
                    if(owner_position == target_position) return false;
                    ++blocker_reference_count;
                }
            }
        }
        if(update_index_count != 1 ||
           (is_independent && blocker_reference_count != 0) ||
           (is_required && blocker_reference_count == 0)) {
            return false;
        }
    }
    return true;
}

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy);
AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    const ProviderSelectionCallback& select_provider);
bool has_executable_targets(const AurUpdateExecutionPreflight& preflight) noexcept;
bool has_blocking_targets(const AurUpdateExecutionPreflight& preflight) noexcept;
bool can_execute(const AurUpdateExecutionPreflight& preflight) noexcept;
