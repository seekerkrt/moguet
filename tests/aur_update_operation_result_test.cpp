#include "app_config.hpp"
#include "aur_update_operation_result.hpp"
#include "stubs/aur-update-execution-preparation/preparation_stub.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace preparation_stub = aur_update_execution_preparation_test_stub;

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config) {
    AurUpdateExecutionPreflight snapshot = preflight;
    snapshot.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    return ::prepare_aur_update_source_build_invocation(
        snapshot, DevelRequiresCheckPolicy::BlockOperation,
        saved_source_preference_policy, needed, config);
}

AurUpdateOperationResult reduce_aur_update_operation_result(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateSourceBuildPreparation& preparation,
    const std::optional<AurUpdateSourceBuildExecutionResult>& execution) {
    AurUpdateExecutionPreflight preflight_snapshot = preflight;
    preflight_snapshot.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    if(preparation.devel_requires_check_policy.has_value()) {
        return ::reduce_aur_update_operation_result(
            preflight_snapshot, preparation,
            DevelRequiresCheckPolicy::BlockOperation, execution);
    }
    if(preparation.invocation.has_value()) {
        throw std::logic_error(
            "Policyless reducer fixture unexpectedly owns an invocation.");
    }
    AurUpdateSourceBuildPreparation preparation_snapshot;
    preparation_snapshot.issues = preparation.issues;
    preparation_snapshot.warnings = preparation.warnings;
    preparation_snapshot.affected_update_targets =
        preparation.affected_update_targets;
    preparation_snapshot.affected_roots = preparation.affected_roots;
    preparation_snapshot.build_unit_selection =
        preparation.build_unit_selection;
    preparation_snapshot.projected_build_units =
        preparation.projected_build_units;
    preparation_snapshot.externally_satisfied_build_units =
        preparation.externally_satisfied_build_units;
    preparation_snapshot.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    return ::reduce_aur_update_operation_result(
        preflight_snapshot, preparation_snapshot,
        DevelRequiresCheckPolicy::BlockOperation, execution);
}

AurUpdatePlanEntry update_entry(
    const std::string& package_name,
    const std::string& package_base = {}) {
    const std::string resolved_package_base =
        package_base.empty() ? package_name : package_base;
    return AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            resolved_package_base,
            "2.0-1",
            AurVersionRelation::NewerThanInstalled},
        AurUpdateClassification::UpdateAvailable};
}

AurUpdateExecutionTarget executable_target(
    std::size_t update_plan_index,
    const std::string& package_name) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.build_plan_root_index = update_plan_index;
    target.update = update_entry(package_name);
    target.status = AurUpdateExecutionTargetStatus::Executable;
    target.desired_install_reason = DesiredInstallReason::Explicit;
    return target;
}

AurUpdateExecutionTarget executable_target_with_base(
    std::size_t update_plan_index,
    const std::string& package_name,
    const std::string& package_base) {
    AurUpdateExecutionTarget target =
        executable_target(update_plan_index, package_name);
    target.update.aur_package->package_base = package_base;
    return target;
}

AurUpdateExecutionTarget up_to_date_target(
    std::size_t update_plan_index,
    const std::string& package_name) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.update = AurUpdatePlanEntry{
        package_name,
        "2.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            package_name,
            "2.0-1",
            AurVersionRelation::SameAsInstalled},
        AurUpdateClassification::UpToDate};
    target.status = AurUpdateExecutionTargetStatus::Skipped;
    target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::UpToDate,
        package_name,
        package_name,
        std::nullopt,
        "Installed package is already up to date."});
    return target;
}

AurUpdateExecutionTarget non_aur_target(
    std::size_t update_plan_index,
    const std::string& package_name) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.update = AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        std::nullopt,
        AurUpdateClassification::NonAurForeign};
    target.status = AurUpdateExecutionTargetStatus::Skipped;
    target.skip_kind = AurUpdateExecutionSkipKind::NonAurForeign;
    target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::NonAurForeign,
        package_name,
        std::nullopt,
        std::nullopt,
        "Installed foreign package is not present in AUR."});
    return target;
}

AurUpdateExecutionTarget devel_requires_check_target(
    std::size_t update_plan_index,
    const std::string& package_name) {
    DevelPackageClassification devel_classification =
        DevelPackageClassification::classify(
            DevelPackageSuffixEvidence::classify(
                package_name, {package_name}));
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.update = AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            package_name,
            "1.0-1",
            AurVersionRelation::SameAsInstalled},
        AurUpdateClassification::UpToDate,
        std::move(devel_classification),
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::SuffixCandidateOnly)};
    target.status = AurUpdateExecutionTargetStatus::Incomplete;
    AurUpdateExecutionIssue issue{
        AurUpdateExecutionReason::DevelRequiresCheck,
        package_name,
        package_name,
        std::nullopt,
        "Devel package suffix is candidate evidence only."};
    issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    target.issues.push_back(std::move(issue));
    return target;
}

AurUpdateExecutionTarget independent_devel_requires_check_target(
    std::size_t update_plan_index,
    const std::string& package_name,
    const std::string& package_base = {}) {
    const std::string resolved_package_base =
        package_base.empty() ? package_name : package_base;
    DevelPackageClassification devel_classification =
        DevelPackageClassification::classify(
            DevelPackageSuffixEvidence::classify(
                resolved_package_base, {package_name}));
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.update = AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            resolved_package_base,
            "1.0-1",
            AurVersionRelation::SameAsInstalled},
        AurUpdateClassification::UpToDate,
        std::move(devel_classification),
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::SuffixCandidateOnly)};
    target.status = AurUpdateExecutionTargetStatus::Skipped;
    target.skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
    AurUpdateExecutionIssue issue{
        AurUpdateExecutionReason::DevelRequiresCheck,
        package_name,
        resolved_package_base,
        std::nullopt,
        "Devel package suffix is candidate evidence only."};
    issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    target.issues.push_back(std::move(issue));
    return target;
}

AurUpdateExecutionPreflight required_devel_result_preflight() {
    const RootTargetIdentity root{0, "required-result-root"};
    AurUpdateExecutionTarget affected =
        executable_target(0, root.requested_name);
    affected.status = AurUpdateExecutionTargetStatus::Incomplete;

    AurUpdateExecutionTarget required =
        independent_devel_requires_check_target(
            1, "required-result-git", "required-result-base");
    required.skip_kind =
        AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck;

    AurUpdateRequiredDevelTargetBlocker blocker{
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1,
        "required-result-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    blocker.dependency_edge_index = 0;
    blocker.package_base = "required-result-base";
    blocker.roles = {PackageRole::RuntimeDependency};
    blocker.affected_roots = {root};
    AurUpdateExecutionIssue required_issue{
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck,
        "required-result-git",
        "required-result-base",
        "required-result-git",
        "Required devel result fixture."};
    required_issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    required_issue.required_devel_target_blocker = blocker;
    affected.issues.push_back(std::move(required_issue));

    AurUpdateRequiredDevelTargetBlocker artifact_blocker{
        AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild,
        1,
        "required-result-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    artifact_blocker.build_plan_order_index = 0;
    artifact_blocker.package_base = "required-result-base";
    artifact_blocker.roles = {PackageRole::RuntimeDependency};
    artifact_blocker.affected_roots = {root};
    AurUpdateExecutionIssue artifact_issue{
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck,
        "required-result-git",
        "required-result-base",
        std::nullopt,
        "Required devel artifact result fixture."};
    artifact_issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    artifact_issue.required_devel_target_blocker =
        std::move(artifact_blocker);
    affected.issues.push_back(std::move(artifact_issue));

    BuildPlan plan;
    plan.root_targets = {root};
    plan.package_targets = {
        PlannedPackageTarget{
            root.requested_name, root.requested_name, {PackageRole::Root}, {root}},
        PlannedPackageTarget{
            "required-result-git", "required-result-base", {PackageRole::RuntimeDependency}, {root}},
    };
    plan.order = {
        BuildPlanEntry{
            "required-result-base", {"required-result-git"}},
        BuildPlanEntry{
            root.requested_name, {root.requested_name}},
    };
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        root.requested_name,
        root.requested_name,
        "required-result-git",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"required-result-git"},
        std::optional<std::string>{"required-result-base"},
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{ConsumerDependencyRequirement(
            "required-result-git", "required-result-git",
            std::nullopt)},
        ResolvedDependencyCandidate{AurResolvedDependencyCandidate{
            "required-result-git",
            "required-result-base",
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                "1.0-1")}},
        ConstraintEvaluation::unconstrained()});

    return AurUpdateExecutionPreflight{
        {std::move(affected), std::move(required)},
        std::move(plan),
        DevelRequiresCheckPolicy::SkipIndependentTarget};
}

AurUpdateExecutionTarget blocking_target(
    std::size_t update_plan_index,
    const std::string& package_name,
    AurUpdateExecutionTargetStatus status,
    AurUpdateExecutionReason reason) {
    AurUpdateExecutionTarget target =
        executable_target(update_plan_index, package_name);
    target.status = status;
    target.issues.push_back(AurUpdateExecutionIssue{
        reason,
        package_name,
        package_name,
        std::nullopt,
        "Typed preflight blocker."});
    return target;
}

AurUpdateExecutionPreflight preflight_with(
    std::vector<AurUpdateExecutionTarget> targets) {
    AurUpdateExecutionPreflight preflight;
    preflight.targets = std::move(targets);
    preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    return preflight;
}

AurUpdateSourceBuildPreparation preparation_for_execution(
    const AurUpdateExecutionPreflight& preflight) {
    AurUpdateSourceBuildPreparation preparation;
    preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    for(const auto& target : preflight.targets) {
        if(target.status == AurUpdateExecutionTargetStatus::Executable) {
            preparation.affected_update_targets.push_back(target);
        }
    }
    return preparation;
}

AurUpdatePreparationIssue preparation_issue(
    AurUpdatePreparationReason reason,
    std::vector<std::size_t> affected_update_plan_indices,
    const std::string& diagnostic) {
    AurUpdatePreparationIssue issue;
    issue.reason = reason;
    issue.affected_update_plan_indices =
        std::move(affected_update_plan_indices);
    issue.diagnostic = diagnostic;
    return issue;
}

AurUpdateWorkItemExecutionResult work_item_result(
    std::size_t work_item_index,
    AurUpdateWorkItemExecutionStatus status,
    std::vector<std::size_t> affected_update_plan_indices,
    const std::string& package_base = {}) {
    const std::string resolved_package_base = package_base.empty()
                                                  ? "work-" + std::to_string(work_item_index)
                                                  : package_base;
    AurUpdateWorkItemExecutionResult result;
    result.work_item_index = work_item_index;
    result.build_plan_order_index = work_item_index;
    result.package_name = resolved_package_base;
    result.package_base = resolved_package_base;
    result.plan_package_names = {resolved_package_base};
    result.affected_update_plan_indices = affected_update_plan_indices;
    result.status = status;

    AurUpdateChildExecutionResult child;
    child.work_item_index = work_item_index;
    child.build_plan_order_index = work_item_index;
    child.required_child_index = 0;
    child.package_base = resolved_package_base;
    child.required_package_name = resolved_package_base;
    child.desired_install_reason = DesiredInstallReason::Explicit;
    child.affected_update_plan_indices = affected_update_plan_indices;
    for(const std::size_t update_plan_index : affected_update_plan_indices) {
        child.affected_roots.push_back(
            RootTargetIdentity{update_plan_index, resolved_package_base});
    }
    result.affected_roots = child.affected_roots;
    child.roles = {PackageRole::Root};

    switch(status) {
        case AurUpdateWorkItemExecutionStatus::Updated:
            child.selected_artifact = ArtifactPackageIdentity{
                resolved_package_base, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::Installed;
            result.failure_kind = AurUpdateWorkItemFailureKind::None;
            break;
        case AurUpdateWorkItemExecutionStatus::NoChange:
            child.selected_artifact = ArtifactPackageIdentity{
                resolved_package_base, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::SkippedAsNeeded;
            result.failure_kind = AurUpdateWorkItemFailureKind::None;
            break;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
            result.failure_kind = AurUpdateWorkItemFailureKind::None;
            result.cancellation = ConfirmationCancelled{ConfirmationCancellationReason::ExplicitToken};
            break;
        case AurUpdateWorkItemExecutionStatus::Failed:
            child.status = AurUpdateChildExecutionStatus::NotAttempted;
            result.failure_kind =
                AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
            result.failure_detail.emplace<AurUpdateSourceBuildFailureSnapshot>(
                AurUpdateSourceBuildFailureSnapshot{
                    AurUpdateSourceBuildFailureCategory::Other,
                    "scripted work item failure", std::nullopt});
            result.diagnostic = "scripted work item failure";
            break;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            child.selected_artifact = ArtifactPackageIdentity{
                resolved_package_base, "2.0-1"};
            child.status =
                AurUpdateChildExecutionStatus::InstalledCleanupFailed;
            result.failure_kind = AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction;
            result.diagnostic = "scripted cleanup failure";
            break;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            child.selected_artifact = ArtifactPackageIdentity{
                resolved_package_base, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::
                SkippedAsNeededCleanupFailed;
            result.failure_kind = AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction;
            result.diagnostic = "scripted cleanup failure";
            break;
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            child.status = AurUpdateChildExecutionStatus::NotAttempted;
            result.failure_kind =
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
            break;
    }
    result.child_results.push_back(std::move(child));
    return result;
}

AurUpdateSourceBuildExecutionResult execution_result(
    AurUpdateInvocationExecutionStatus status,
    std::vector<AurUpdateWorkItemExecutionResult> work_items,
    SelectedRepositoryProviderTransactionResult provider_transaction = {}) {
    return AurUpdateSourceBuildExecutionResult{
        status,
        std::move(work_items),
        std::move(provider_transaction)};
}

struct ExactChildExecutionSpec {
    std::string package_name;
    DesiredInstallReason desired_install_reason =
        DesiredInstallReason::Explicit;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    std::vector<PackageRole> roles;
    AurUpdateChildExecutionStatus status =
        AurUpdateChildExecutionStatus::Installed;
    std::string full_version = "2.0-1";
};

struct ExactWorkItemExecutionSpec {
    std::string package_base;
    std::vector<ExactChildExecutionSpec> children;
    AurUpdateWorkItemExecutionStatus status =
        AurUpdateWorkItemExecutionStatus::Updated;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
};

struct ExactReducerInput {
    AurUpdateExecutionPreflight preflight;
    AurUpdateSourceBuildPreparation preparation;
    AurUpdateSourceBuildExecutionResult execution;
};

ExactChildExecutionSpec exact_child(
    std::string package_name,
    std::vector<std::size_t> affected_update_plan_indices,
    std::vector<RootTargetIdentity> affected_roots,
    std::vector<PackageRole> roles,
    AurUpdateChildExecutionStatus status,
    DesiredInstallReason desired_install_reason =
        DesiredInstallReason::Explicit,
    std::string full_version = "2.0-1") {
    return ExactChildExecutionSpec{
        std::move(package_name),
        desired_install_reason,
        std::move(affected_update_plan_indices),
        std::move(affected_roots),
        std::move(roles),
        status,
        std::move(full_version)};
}

template <typename Value>
void append_unique(std::vector<Value>& values, const Value& value) {
    if(std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

AurUpdateWorkItemFailureKind failure_kind_for_status(
    AurUpdateWorkItemExecutionStatus status) {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::Updated:
        case AurUpdateWorkItemExecutionStatus::NoChange:
        case AurUpdateWorkItemExecutionStatus::Cancelled:
            return AurUpdateWorkItemFailureKind::None;
        case AurUpdateWorkItemExecutionStatus::Failed:
            return AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction;
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    }
    throw std::logic_error("Unknown exact work-item status fixture.");
}

ExactReducerInput exact_reducer_input(
    std::vector<AurUpdateExecutionTarget> targets,
    std::vector<ExactWorkItemExecutionSpec> work_item_specs,
    AurUpdateInvocationExecutionStatus invocation_status) {
    AurUpdateExecutionPreflight preflight =
        preflight_with(std::move(targets));
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    ExactReducerInput input{
        std::move(preflight),
        std::move(preparation),
        execution_result(invocation_status, {})};

    for(std::size_t work_item_index = 0;
        work_item_index < work_item_specs.size(); ++work_item_index) {
        ExactWorkItemExecutionSpec& spec = work_item_specs[work_item_index];

        AurUpdateBuildUnitSelectionEntry selection;
        selection.build_plan_order_index = work_item_index;
        selection.package_base = spec.package_base;
        selection.status =
            AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution;
        selection.selected_execution_index = work_item_index;

        AurUpdateProjectedBuildUnit projected;
        projected.build_plan_order_index = work_item_index;
        projected.package_base = spec.package_base;

        AurUpdateWorkItemExecutionResult work_item;
        work_item.work_item_index = work_item_index;
        work_item.build_plan_order_index = work_item_index;
        work_item.package_base = spec.package_base;
        work_item.status = spec.status;
        work_item.failure_kind = failure_kind_for_status(spec.status);
        work_item.unselected_artifacts =
            std::move(spec.unselected_artifacts);
        if(spec.status == AurUpdateWorkItemExecutionStatus::Failed) {
            work_item.failure_detail.emplace<
                AurUpdateSourceBuildFailureSnapshot>(
                AurUpdateSourceBuildFailureSnapshot{
                    AurUpdateSourceBuildFailureCategory::Build,
                    "exact scripted work-item failure",
                    std::nullopt});
            work_item.diagnostic = "exact scripted work-item failure";
        } else if(
            spec.status ==
                AurUpdateWorkItemExecutionStatus::
                    UpdatedCleanupFailed ||
            spec.status == AurUpdateWorkItemExecutionStatus::
                               NoChangeCleanupFailed) {
            work_item.diagnostic = "exact scripted cleanup failure";
        }

        for(std::size_t child_index = 0;
            child_index < spec.children.size(); ++child_index) {
            const ExactChildExecutionSpec& child_spec =
                spec.children[child_index];
            selection.package_names.push_back(child_spec.package_name);
            work_item.plan_package_names.push_back(child_spec.package_name);

            AurUpdateRequiredTargetAttribution planned_child;
            planned_child.required_target = RequiredPackageArtifactTarget{
                spec.package_base,
                child_spec.package_name,
                child_spec.desired_install_reason};
            planned_child.affected_update_plan_indices =
                child_spec.affected_update_plan_indices;
            planned_child.affected_roots = child_spec.affected_roots;
            planned_child.roles = child_spec.roles;
            projected.required_target_attributions.push_back(planned_child);

            AurUpdateChildExecutionResult child;
            child.work_item_index = work_item_index;
            child.build_plan_order_index = work_item_index;
            child.required_child_index = child_index;
            child.package_base = spec.package_base;
            child.required_package_name = child_spec.package_name;
            child.desired_install_reason =
                child_spec.desired_install_reason;
            child.affected_update_plan_indices =
                child_spec.affected_update_plan_indices;
            child.affected_roots = child_spec.affected_roots;
            child.roles = child_spec.roles;
            child.status = child_spec.status;
            if(child_spec.status !=
               AurUpdateChildExecutionStatus::NotAttempted) {
                child.selected_artifact = ArtifactPackageIdentity{
                    child_spec.package_name,
                    child_spec.full_version};
            }
            work_item.child_results.push_back(std::move(child));

            for(const std::size_t update_plan_index :
                child_spec.affected_update_plan_indices) {
                append_unique(
                    projected.affected_update_plan_indices,
                    update_plan_index);
                append_unique(
                    work_item.affected_update_plan_indices,
                    update_plan_index);
            }
            for(const RootTargetIdentity& root : child_spec.affected_roots) {
                append_unique(projected.affected_roots, root);
                append_unique(work_item.affected_roots, root);
                append_unique(input.preparation.affected_roots, root);
            }
        }
        if(spec.children.size() == 1) {
            work_item.package_name = spec.children.front().package_name;
        }

        input.preparation.build_unit_selection.entries.push_back(
            std::move(selection));
        input.preparation.projected_build_units.push_back(
            std::move(projected));
        input.execution.work_item_results.push_back(std::move(work_item));
    }
    return input;
}

bool has_reduction_issue(
    const AurUpdateOperationResult& result,
    AurUpdateOperationReductionReason reason) {
    return std::any_of(
        result.reduction_issues.begin(),
        result.reduction_issues.end(),
        [reason](const AurUpdateOperationReductionIssue& issue) {
            return issue.reason == reason;
        });
}

void expect_independent_shape_inconsistent(
    AurUpdateExecutionTarget target,
    DevelRequiresCheckPolicy policy,
    const std::string& context) {
    AurUpdateExecutionPreflight preflight;
    preflight.targets.push_back(std::move(target));
    preflight.devel_requires_check_policy = policy;
    AurUpdateSourceBuildPreparation preparation;
    preparation.devel_requires_check_policy = policy;
    const AurUpdateOperationResult result =
        ::reduce_aur_update_operation_result(
            preflight, preparation, policy, std::nullopt);
    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            !result.is_success() &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent) &&
            result.targets.size() == 1 &&
            result.targets.front().status ==
                AurUpdateOperationTargetStatus::Incomplete,
        context + ": malformed independent skip became success");
}

void expect_target_statuses(
    const AurUpdateOperationResult& result,
    const std::vector<AurUpdateOperationTargetStatus>& expected,
    const std::string& context) {
    expect(
        result.targets.size() == expected.size(),
        context + ": target count differs");
    for(std::size_t position = 0; position < expected.size(); ++position) {
        expect(
            result.targets[position].status == expected[position],
            context + ": target status differs at " +
                std::to_string(position));
    }
}

AurUpdateExecutionPreflight prepared_single_root_preflight(
    const std::string& package_name) {
    const RootTargetIdentity root{0, package_name};
    BuildPlan plan;
    plan.root_targets.push_back(root);
    plan.package_targets.push_back(PlannedPackageTarget{
        package_name,
        package_name,
        {PackageRole::Root},
        {root}});
    plan.order.push_back(BuildPlanEntry{package_name, {package_name}});

    AurUpdateExecutionPreflight preflight;
    preflight.targets.push_back(executable_target(0, package_name));
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateSourceBuildPreparation prepare_single_root(
    const AurUpdateExecutionPreflight& preflight) {
    preparation_stub::reset();
    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, SavedSourcePreferencePolicy::Strict, false,
            config);
    expect(preparation.is_prepared(), "Real preparation fixture was not prepared");
    return preparation;
}

void test_all_skipped_is_no_updates() {
    AurUpdateExecutionTarget current =
        up_to_date_target(0, "current-package");
    AurUpdateExecutionTarget foreign =
        non_aur_target(1, "foreign-package");
    // POLICY(#267): normal skipではproducerがinstall reasonを未解決のまま
    // 保持するため、Unknownは不整合ではない。
    current.update.install_reason = InstalledPackageReason::Unknown;
    foreign.update.install_reason = InstalledPackageReason::Unknown;
    const AurUpdateExecutionPreflight preflight = preflight_with({
        std::move(current),
        std::move(foreign),
    });
    const AurUpdateSourceBuildPreparation preparation;

    AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::NoUpdates,
        "Skip-only operation status differs");
    expect(result.is_success(), "Skip-only operation was not successful");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Skipped,
            AurUpdateOperationTargetStatus::Skipped,
        },
        "skip-only operation");
    expect(
        !result.changed_package_state(),
        "Skip-only operation reported package mutation");

    result.targets.front().skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
    expect(
        !result.is_success(),
        "Post-reduction independent RequiresCheck skip became success");
    result.targets.front().skip_kind =
        AurUpdateExecutionSkipKind::UpToDate;
    result.targets.front().update.devel_assessment =
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::SuffixCandidateOnly);
    expect(
        !result.is_success(),
        "Post-reduction RequiresCheck assessment became UpToDate success");
}

void test_independent_devel_requires_check_result_semantics() {
    AurUpdateExecutionPreflight requires_check_only;
    requires_check_only.targets.push_back(
        independent_devel_requires_check_target(
            0, "independent-result-git", "independent-result-base-git"));
    requires_check_only.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    AurUpdateSourceBuildPreparation noop_preparation;
    noop_preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;

    const AurUpdateOperationResult no_updates =
        ::reduce_aur_update_operation_result(
            requires_check_only, noop_preparation,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            std::nullopt);

    expect(
        no_updates.status == AurUpdateOperationStatus::NoUpdates &&
            no_updates.is_success() &&
            no_updates.reduction_issues.empty() &&
            no_updates.targets.size() == 1 &&
            no_updates.targets.front().status ==
                AurUpdateOperationTargetStatus::Skipped &&
            no_updates.targets.front().skip_kind ==
                std::optional<AurUpdateExecutionSkipKind>{
                    AurUpdateExecutionSkipKind::
                        IndependentDevelRequiresCheck} &&
            no_updates.package_state_change() ==
                PackageStateChange::Unknown &&
            !no_updates.changed_package_state(),
        "Independent RequiresCheck-only result was not a successful unverified skip");
    const AurUpdateOperationTargetResult& skipped =
        no_updates.targets.front();
    expect(
        skipped.update_plan_index == 0 &&
            skipped.update.installed_name == "independent-result-git" &&
            skipped.update.installed_version == "1.0-1" &&
            skipped.update.install_reason ==
                InstalledPackageReason::Explicit &&
            skipped.package_base ==
                std::optional<std::string>{
                    "independent-result-base-git"} &&
            skipped.update.aur_package.has_value() &&
            skipped.update.aur_package->aur_name ==
                "independent-result-git" &&
            skipped.update.aur_package->package_base ==
                "independent-result-base-git" &&
            skipped.update.aur_package->version == "1.0-1" &&
            skipped.update.aur_package->version_relation ==
                AurVersionRelation::SameAsInstalled &&
            skipped.update.classification ==
                AurUpdateClassification::UpToDate &&
            skipped.update.devel_assessment.requires_check_reason() !=
                nullptr &&
            *skipped.update.devel_assessment.requires_check_reason() ==
                DevelRequiresCheckReason::SuffixCandidateOnly &&
            skipped.preflight_issues.size() == 1 &&
            skipped.preflight_issues.front().reason ==
                AurUpdateExecutionReason::DevelRequiresCheck &&
            skipped.preflight_issues.front()
                    .devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly},
        "Independent RequiresCheck result lost full target identity or exact reason");

    AurUpdateExecutionPreflight mixed = preflight_with({
        executable_target(0, "updated-root"),
        independent_devel_requires_check_target(
            1, "independent-mixed-result-git"),
    });
    mixed.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    mixed.build_plan = BuildPlan{};
    mixed.build_plan->root_targets = {{0, "updated-root"}};
    mixed.build_plan->package_targets.push_back(PlannedPackageTarget{
        "updated-root",
        "updated-root",
        {PackageRole::Root},
        {{0, "updated-root"}}});
    mixed.build_plan->order.push_back(
        BuildPlanEntry{"updated-root", {"updated-root"}});
    AurUpdateSourceBuildPreparation mixed_preparation =
        preparation_for_execution(mixed);
    mixed_preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0, AurUpdateWorkItemExecutionStatus::Updated, {0},
            "updated-root")});

    const AurUpdateOperationResult changed =
        ::reduce_aur_update_operation_result(
            mixed, mixed_preparation,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            execution);
    expect(
        changed.status == AurUpdateOperationStatus::Completed &&
            changed.is_success() && changed.reduction_issues.empty() &&
            changed.targets.size() == 2 &&
            changed.targets[0].status ==
                AurUpdateOperationTargetStatus::Updated &&
            changed.targets[1].status ==
                AurUpdateOperationTargetStatus::Skipped &&
            changed.targets[1].skip_kind ==
                std::optional<AurUpdateExecutionSkipKind>{
                    AurUpdateExecutionSkipKind::
                        IndependentDevelRequiresCheck} &&
            changed.package_state_change() ==
                PackageStateChange::Changed &&
            changed.changed_package_state(),
        "Changed package evidence did not take precedence over independent RequiresCheck attention");
}

void test_required_devel_blocker_result_correlation() {
    preparation_stub::reset();
    const AppConfig config;
    AurUpdateExecutionPreflight preflight =
        required_devel_result_preflight();
    const AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, false, config);
    const AurUpdateOperationResult blocked =
        ::reduce_aur_update_operation_result(
            preflight, preparation,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            std::nullopt);
    expect(
        preparation.is_blocked() &&
            blocked.status ==
                AurUpdateOperationStatus::BlockedBeforeExecution &&
            blocked.reduction_issues.empty() &&
            blocked.targets.size() == 2 &&
            blocked.targets[0].status ==
                AurUpdateOperationTargetStatus::Incomplete &&
            blocked.targets[1].status ==
                AurUpdateOperationTargetStatus::Skipped &&
            blocked.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck &&
            !blocked.is_success(),
        "Required devel blocker did not reduce to a coherent blocked result");

    std::get<AurResolvedDependencyCandidate>(
        *preflight.build_plan->dependency_edges.front()
             .resolved_candidate)
        .package_version = ObservedVersion::available(
        ObservedVersionSource::AurExactPackage, "9.0-1");
    const AurUpdateOperationResult drifted =
        ::reduce_aur_update_operation_result(
            preflight, preparation,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            std::nullopt);
    expect(
        drifted.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                drifted,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent),
        "Required devel BuildPlan version drift passed the result firewall");
}

void test_missing_required_devel_relation_result_is_inconsistent() {
    AurUpdateExecutionPreflight malformed =
        required_devel_result_preflight();
    std::erase_if(
        malformed.targets[0].issues,
        [](const AurUpdateExecutionIssue& issue) {
            return issue.reason == AurUpdateExecutionReason::
                                       RequiredDevelTargetRequiresCheck;
        });
    malformed.targets[0].status =
        AurUpdateExecutionTargetStatus::Executable;
    malformed.targets[1].skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;

    const RootTargetIdentity root{0, "required-result-root"};
    ExactReducerInput input = exact_reducer_input(
        malformed.targets,
        {
            ExactWorkItemExecutionSpec{
                "required-result-base",
                {exact_child(
                    "required-result-git", {0}, {root},
                    {PackageRole::RuntimeDependency},
                    AurUpdateChildExecutionStatus::Installed,
                    DesiredInstallReason::Dependency)},
                AurUpdateWorkItemExecutionStatus::Updated,
                {}},
            ExactWorkItemExecutionSpec{
                "required-result-root",
                {exact_child(
                    "required-result-root", {0}, {root},
                    {PackageRole::Root},
                    AurUpdateChildExecutionStatus::Installed)},
                AurUpdateWorkItemExecutionStatus::Updated,
                {}},
        },
        AurUpdateInvocationExecutionStatus::Completed);
    input.preflight.build_plan = malformed.build_plan;
    input.preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    input.preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;

    const AurUpdateOperationResult missing =
        ::reduce_aur_update_operation_result(
            input.preflight, input.preparation,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            input.execution);
    expect(
        missing.status == AurUpdateOperationStatus::InconsistentResult &&
            !missing.is_success() &&
            std::any_of(
                missing.reduction_issues.begin(),
                missing.reduction_issues.end(),
                [](const AurUpdateOperationReductionIssue& issue) {
                    return issue.reason ==
                               AurUpdateOperationReductionReason::
                                   OtherCorrelationInconsistent &&
                           issue.stage ==
                               AurUpdateOperationReductionStage::Preflight;
                }),
        "Missing required-devel blocker became a successful result");

    AurUpdateExecutionTarget installed_root =
        executable_target(0, "installed-forged-root");
    AurUpdateExecutionTarget installed_requires_check =
        independent_devel_requires_check_target(
            1, "installed-forged-git", "installed-forged-base");
    ExactReducerInput installed_input = exact_reducer_input(
        {installed_root, installed_requires_check},
        {ExactWorkItemExecutionSpec{
            "installed-forged-root",
            {exact_child(
                "installed-forged-root", {0},
                {{0, "installed-forged-root"}},
                {PackageRole::Root},
                AurUpdateChildExecutionStatus::Installed)},
            AurUpdateWorkItemExecutionStatus::Updated,
            {}}},
        AurUpdateInvocationExecutionStatus::Completed);
    BuildPlan installed_plan;
    installed_plan.root_targets = {{0, "installed-forged-root"}};
    installed_plan.package_targets.push_back(PlannedPackageTarget{
        "installed-forged-root",
        "installed-forged-root",
        {PackageRole::Root},
        {{0, "installed-forged-root"}}});
    installed_plan.order.push_back(BuildPlanEntry{
        "installed-forged-root", {"installed-forged-root"}});
    installed_plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "installed-forged-root",
        "installed-forged-root",
        "installed-forged-git>=2",
        PackageRole::RuntimeDependency,
        DependencyKind::Installed,
        "installed-forged-git",
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{ConsumerDependencyRequirement(
            "installed-forged-git>=2", "installed-forged-git",
            DependencyVersionConstraint(
                DependencyVersionRelation::GreaterThanOrEqual,
                "2"))},
        ResolvedDependencyCandidate{InstalledExactPackage{
            "installed-forged-git",
            ObservedVersion::available(
                ObservedVersionSource::InstalledExactPackage,
                "1.0-1")}},
        ConstraintEvaluation::unconstrained()});
    installed_input.preflight.build_plan = std::move(installed_plan);
    installed_input.preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    installed_input.preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;

    const AurUpdateOperationResult installed_forged =
        ::reduce_aur_update_operation_result(
            installed_input.preflight, installed_input.preparation,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            installed_input.execution);
    expect(
        installed_forged.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                installed_forged,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent),
        "Forged InstalledExact constraint became a successful result");
}

void test_skip_order_and_typed_reasons_are_preserved() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        up_to_date_target(0, "up-to-date"),
        non_aur_target(1, "non-aur"),
    });
    const AurUpdateSourceBuildPreparation preparation;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.targets[0].update.installed_name == "up-to-date" &&
            result.targets[1].update.installed_name == "non-aur",
        "Skipped target order differs");
    expect(
        result.targets[0].update_plan_index == 0 &&
            result.targets[1].update_plan_index == 1,
        "Skipped update plan indices differ");
    expect(
        result.targets[0].update.installed_version == "2.0-1" &&
            result.targets[0].update.install_reason ==
                InstalledPackageReason::Explicit &&
            result.targets[0].update.classification ==
                AurUpdateClassification::UpToDate &&
            result.targets[0].update.aur_package.has_value() &&
            result.targets[0].update.aur_package->aur_name ==
                "up-to-date" &&
            result.targets[0].update.aur_package->package_base ==
                "up-to-date" &&
            result.targets[0].update.aur_package->version ==
                "2.0-1" &&
            result.targets[0].update.aur_package->version_relation ==
                AurVersionRelation::SameAsInstalled,
        "Original update plan entry was not retained as an owned value");
    expect(
        result.targets[0].preflight_issues.size() == 1 &&
            result.targets[0].preflight_issues.front().reason ==
                AurUpdateExecutionReason::UpToDate &&
            result.targets[1].preflight_issues.size() == 1 &&
            result.targets[1].preflight_issues.front().reason ==
                AurUpdateExecutionReason::NonAurForeign,
        "Skipped typed reasons were flattened or reordered");
    expect(
        result.targets[0].package_base ==
                std::optional<std::string>{"up-to-date"} &&
            !result.targets[1].package_base.has_value(),
        "Skipped PackageBase ownership differs");
}

void test_skipped_without_normal_reason_is_inconsistent() {
    AurUpdateExecutionTarget malformed = up_to_date_target(0, "malformed-skip");
    malformed.issues.clear();
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(malformed)});
    const AurUpdateSourceBuildPreparation preparation;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            !result.is_success() &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent),
        "Skipped target without a normal reason became NoUpdates");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Incomplete},
        "malformed skipped target");
}

void test_requires_check_policy_snapshots_fail_closed() {
    const auto expect_policy_inconsistent = [](
                                                AurUpdateExecutionPreflight preflight,
                                                AurUpdateSourceBuildPreparation preparation,
                                                DevelRequiresCheckPolicy policy,
                                                const std::string& context) {
        const AurUpdateOperationResult result =
            ::reduce_aur_update_operation_result(
                preflight, preparation, policy, std::nullopt);
        expect(
            result.status == AurUpdateOperationStatus::InconsistentResult &&
                !result.is_success() &&
                has_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        DevelRequiresCheckPolicyInconsistent),
            context + ": malformed policy snapshot became success");
    };

    AurUpdateExecutionPreflight missing =
        preflight_with({up_to_date_target(0, "missing-policy")});
    missing.devel_requires_check_policy.reset();
    AurUpdateSourceBuildPreparation block_preparation;
    block_preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    expect_policy_inconsistent(
        std::move(missing), std::move(block_preparation),
        DevelRequiresCheckPolicy::BlockOperation,
        "missing preflight RequiresCheck policy");

    AurUpdateExecutionPreflight mismatch =
        preflight_with({up_to_date_target(0, "mismatched-policy")});
    AurUpdateSourceBuildPreparation skip_preparation;
    skip_preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    expect_policy_inconsistent(
        std::move(mismatch), std::move(skip_preparation),
        DevelRequiresCheckPolicy::BlockOperation,
        "cross-layer RequiresCheck policy mismatch");

    const DevelRequiresCheckPolicy unknown_policy =
        static_cast<DevelRequiresCheckPolicy>(-1);
    AurUpdateExecutionPreflight unknown =
        preflight_with({up_to_date_target(0, "unknown-policy")});
    unknown.devel_requires_check_policy = unknown_policy;
    AurUpdateSourceBuildPreparation unknown_preparation;
    unknown_preparation.devel_requires_check_policy = unknown_policy;
    expect_policy_inconsistent(
        std::move(unknown), std::move(unknown_preparation),
        unknown_policy, "unknown RequiresCheck policy");

    expect_independent_shape_inconsistent(
        independent_devel_requires_check_target(
            0, "block-policy-result-git"),
        DevelRequiresCheckPolicy::BlockOperation,
        "BlockOperation independent RequiresCheck result");

    AurUpdateExecutionTarget missing_reason =
        independent_devel_requires_check_target(
            0, "missing-result-reason-git");
    missing_reason.issues.front().devel_requires_check_reason.reset();
    expect_independent_shape_inconsistent(
        std::move(missing_reason),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result without exact reason");

    AurUpdateExecutionTarget missing_classification =
        independent_devel_requires_check_target(
            0, "missing-result-classification-git");
    missing_classification.update.devel_classification.reset();
    expect_independent_shape_inconsistent(
        std::move(missing_classification),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result without producer classification");

    AurUpdateExecutionTarget base_evidence_drift =
        independent_devel_requires_check_target(
            0, "base-result-evidence-drift-git");
    base_evidence_drift.update.devel_classification =
        DevelPackageClassification::classify(
            DevelPackageSuffixEvidence::classify(
                "different-result-base-git",
                {"base-result-evidence-drift-git"}));
    expect_independent_shape_inconsistent(
        std::move(base_evidence_drift),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with PackageBase evidence drift");

    AurUpdateExecutionTarget child_evidence_drift =
        independent_devel_requires_check_target(
            0, "child-result-evidence-drift-git");
    child_evidence_drift.update.devel_classification =
        DevelPackageClassification::classify(
            DevelPackageSuffixEvidence::classify(
                "child-result-evidence-drift-git",
                {"different-result-child-git"}));
    expect_independent_shape_inconsistent(
        std::move(child_evidence_drift),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with child evidence drift");

    AurUpdateExecutionTarget reason_evidence_drift =
        independent_devel_requires_check_target(
            0, "reason-result-evidence-drift-git");
    reason_evidence_drift.update.devel_assessment =
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::NoAuthoritativeBuildProvenance);
    reason_evidence_drift.issues.front().devel_requires_check_reason =
        DevelRequiresCheckReason::NoAuthoritativeBuildProvenance;
    expect_independent_shape_inconsistent(
        std::move(reason_evidence_drift),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with reason evidence drift");

    AurUpdateExecutionTarget forged_assessment =
        independent_devel_requires_check_target(
            0, "forged-result-assessment-git");
    forged_assessment.update.devel_assessment =
        DevelUpdateAssessment::not_applicable();
    expect_independent_shape_inconsistent(
        std::move(forged_assessment),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with forged assessment");

    AurUpdateExecutionTarget forged_classification =
        independent_devel_requires_check_target(
            0, "forged-result-classification-git");
    forged_classification.update.classification =
        AurUpdateClassification::UpdateAvailable;
    forged_classification.update.aur_package->version_relation =
        AurVersionRelation::NewerThanInstalled;
    expect_independent_shape_inconsistent(
        std::move(forged_classification),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with forged classification");

    AurUpdateExecutionTarget rooted_skip =
        independent_devel_requires_check_target(
            0, "rooted-independent-result-git");
    rooted_skip.build_plan_root_index = 0;
    expect_independent_shape_inconsistent(
        std::move(rooted_skip),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with BuildPlan root");

    AurUpdateExecutionTarget install_candidate_skip =
        independent_devel_requires_check_target(
            0, "install-candidate-result-git");
    install_candidate_skip.desired_install_reason =
        DesiredInstallReason::Explicit;
    expect_independent_shape_inconsistent(
        std::move(install_candidate_skip),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with install reason");

    AurUpdateExecutionTarget contradictory_skip =
        independent_devel_requires_check_target(
            0, "contradictory-independent-result-git");
    AurUpdateRequiredDevelTargetBlocker required_blocker{
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        0,
        "contradictory-independent-result-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    required_blocker.dependency_edge_index = 0;
    required_blocker.package_base =
        "contradictory-independent-result-git";
    required_blocker.roles = {PackageRole::RuntimeDependency};
    contradictory_skip.issues.front().required_devel_target_blocker =
        std::move(required_blocker);
    expect_independent_shape_inconsistent(
        std::move(contradictory_skip),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent result with required blocker");
}

void test_unknown_preflight_reason_is_typed() {
    AurUpdateExecutionTarget malformed = up_to_date_target(0, "unknown-reason");
    malformed.issues.front().reason =
        static_cast<AurUpdateExecutionReason>(999);
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(malformed)});
    const AurUpdateSourceBuildPreparation preparation;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownEnumValue),
        "Unknown preflight reason was not typed");
}

void test_unknown_owned_plan_enum_is_typed() {
    AurUpdateExecutionTarget malformed = up_to_date_target(0, "unknown-plan");
    malformed.update.classification =
        static_cast<AurUpdateClassification>(999);
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(malformed)});
    const AurUpdateSourceBuildPreparation preparation;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownEnumValue),
        "Unknown owned plan enum was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Incomplete},
        "unknown owned plan enum");
}

void test_executable_preflight_issue_keeps_known_update() {
    AurUpdateExecutionTarget malformed =
        executable_target(0, "malformed-executable");
    malformed.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::BuildPlanInconsistent,
        "malformed-executable",
        "malformed-executable",
        std::nullopt,
        "Executable target retained a blocking issue."});
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(malformed)});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent),
        "Executable target accepted a blocking preflight issue");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Known update was lost after executable preflight inconsistency");
}

void test_executable_hidden_required_devel_payload_is_inconsistent() {
    AurUpdateExecutionTarget malformed =
        executable_target(0, "hidden-required-devel");
    AurUpdateExecutionIssue hidden_issue;
    hidden_issue.required_devel_target_blocker =
        AurUpdateRequiredDevelTargetBlocker{
            AurUpdateRequiredDevelTargetRelation::AurExactDependency,
            1,
            "required-devel-git",
            DevelRequiresCheckReason::SuffixCandidateOnly};
    malformed.issues.push_back(std::move(hidden_issue));
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(malformed)});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);
    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            !result.is_success() &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent) &&
            result.targets.front().preflight_issues.size() == 1 &&
            result.targets.front()
                .preflight_issues.front()
                .required_devel_target_blocker.has_value(),
        "Executable hidden required-devel payload became a successful result");
}

void test_executable_reason_none_issue_is_inconsistent() {
    AurUpdateExecutionTarget malformed =
        executable_target(0, "reason-none-issue");
    malformed.issues.emplace_back();
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(malformed)});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);
    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent),
        "Executable reason-None issue bypassed canonical result validation");
}

void test_preflight_blockers_keep_status_and_stop_executable_targets() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "executable"),
        blocking_target(
            1,
            "unsupported",
            AurUpdateExecutionTargetStatus::Unsupported,
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
        up_to_date_target(2, "skipped"),
        blocking_target(
            3,
            "incomplete",
            AurUpdateExecutionTargetStatus::Incomplete,
            AurUpdateExecutionReason::BuildPlanInconsistent),
        devel_requires_check_target(4, "manual-check-git"),
    });
    preparation_stub::reset();
    const AppConfig config;
    const AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, SavedSourcePreferencePolicy::Strict, false,
            config);

    expect(
        preparation.is_blocked() &&
            preparation.affected_update_targets.size() == 3 &&
            preparation.affected_update_targets[0]
                    .update_plan_index == 1 &&
            preparation.affected_update_targets[1]
                    .update_plan_index == 3 &&
            preparation.affected_update_targets[2]
                    .update_plan_index == 4 &&
            preparation.issues.size() == 3 &&
            preparation.issues[0].reason ==
                AurUpdatePreparationReason::BlockingPreflight &&
            preparation.issues[1].reason ==
                AurUpdatePreparationReason::BlockingPreflight &&
            preparation.issues[2].reason ==
                AurUpdatePreparationReason::BlockingPreflight,
        "Preflight blocker preparation fixture differs from the producer contract");

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status ==
            AurUpdateOperationStatus::BlockedBeforeExecution,
        "Preflight blocker operation status differs");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NotAttempted,
            AurUpdateOperationTargetStatus::Unsupported,
            AurUpdateOperationTargetStatus::Skipped,
            AurUpdateOperationTargetStatus::Incomplete,
            AurUpdateOperationTargetStatus::Incomplete,
        },
        "preflight blocker mapping");
    expect(result.has_blocking_targets(), "Blocking target helper returned false");
    expect(
        result.has_not_attempted_targets(),
        "Blocked executable target was not reported as unattempted");
    expect(
        result.targets[1].preflight_issues.front().reason ==
                AurUpdateExecutionReason::
                    SplitPackageSelectionRequired &&
            result.targets[3].preflight_issues.front().reason ==
                AurUpdateExecutionReason::BuildPlanInconsistent &&
            result.targets[4].preflight_issues.front().reason ==
                AurUpdateExecutionReason::DevelRequiresCheck &&
            result.targets[4]
                    .preflight_issues.front()
                    .devel_requires_check_reason ==
                DevelRequiresCheckReason::SuffixCandidateOnly &&
            result.targets[4].update.devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck,
        "Blocking preflight reasons were not retained");
    expect(
        result.reduction_issues.empty() &&
            !has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Normal preflight blocker preparation was treated as an executable snapshot mismatch");
}

void test_preflight_blocker_missing_snapshot_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "executable"),
        blocking_target(
            1,
            "unsupported",
            AurUpdateExecutionTargetStatus::Unsupported,
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
    });
    preparation_stub::reset();
    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, SavedSourcePreferencePolicy::Strict, false,
            config);
    preparation.affected_update_targets.clear();

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Preflight blocker accepted a missing blocker target snapshot");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NotAttempted,
            AurUpdateOperationTargetStatus::Unsupported,
        },
        "preflight blocker with missing target snapshot");
}

void test_projection_payload_snapshot_drift_is_inconsistent() {
    AurUpdateExecutionTarget blocked = blocking_target(
        0,
        "projection-root",
        AurUpdateExecutionTargetStatus::Incomplete,
        AurUpdateExecutionReason::BuildPlanInconsistent);
    blocked.issues.front().build_plan_projection_issue =
        BuildPlanArtifactTargetProjectionIssue{
            BuildPlanArtifactTargetProjectionIssueKind::
                MissingPlannedPackageTarget,
            std::size_t{0},
            std::size_t{0},
            {0},
            std::string{"projection-base"},
            std::string{"projection-child"},
            {RootTargetIdentity{0, "projection-root"}},
            "Typed projection failure."};
    const AurUpdateExecutionPreflight preflight =
        preflight_with({std::move(blocked)});
    preparation_stub::reset();
    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, SavedSourcePreferencePolicy::Strict, false,
            config);
    expect(
        preparation.affected_update_targets.size() == 1 &&
            preparation.affected_update_targets.front()
                .issues.front()
                .build_plan_projection_issue.has_value(),
        "Projection payload snapshot fixture was not retained");

    preparation.affected_update_targets.front()
        .issues.front()
        .build_plan_projection_issue->package_target_indices.push_back(1);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Projection-only snapshot drift was accepted");
}

void test_preflight_blocker_with_invocation_is_inconsistent() {
    AurUpdateSourceBuildPreparation invocation_source = prepare_single_root(
        prepared_single_root_preflight("prepared-invocation"));
    const AurUpdateExecutionPreflight preflight = preflight_with({
        blocking_target(
            0,
            "unsupported",
            AurUpdateExecutionTargetStatus::Unsupported,
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
    });
    preparation_stub::reset();
    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, SavedSourcePreferencePolicy::Strict, false,
            config);
    preparation.issues.clear();
    preparation.invocation.emplace(
        std::move(*invocation_source.invocation));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    OtherCorrelationInconsistent),
        "Preflight blocker accepted an execution invocation");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Unsupported},
        "preflight blocker with invocation");
}

void test_target_attributed_preparation_failure() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "untouched"),
        executable_target(1, "failed"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::SourcePreferenceUnavailable,
        {1},
        "typed target preparation failure"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status ==
            AurUpdateOperationStatus::BlockedBeforeExecution,
        "Attributed preparation failure operation status differs");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NotAttempted,
            AurUpdateOperationTargetStatus::Failed,
        },
        "attributed preparation failure");
    expect(
        result.targets[0].preparation_issues.empty() &&
            result.targets[1].preparation_issues.size() == 1 &&
            result.targets[1].preparation_issues.front().reason ==
                AurUpdatePreparationReason::
                    SourcePreferenceUnavailable,
        "Preparation issue target attribution differs");
    expect(
        result.reduction_issues.empty(),
        "Normal target-attributed preparation failure was inconsistent");
}

void test_reviewed_preparation_failure_retains_target_and_operation_payload() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "affected-a"),
        executable_target(1, "affected-b"),
        up_to_date_target(2, "untouched"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const ReviewedSourceFatalStateFailure fatal{
        ReviewedSourceFatalStateReason::UnsafeHistory,
        std::nullopt,
        ReviewedSourceStateStoreUnsafeHistory{
            ReviewedSourceStateStoreHistoryIssue::ForkDetected,
            "/fixture/reviewed-state",
            {"1.toml", "2-a.toml", "2-b.toml"},
            1,
            2},
        std::nullopt};
    AurUpdatePreparationIssue issue = preparation_issue(
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        {0, 1}, "presentation text is not the failure authority");
    issue.reviewed_source_failure = ReviewedSourceProductionFailure{
        ReviewedSourceProductionFailureStage::FatalStatePreflight,
        ReviewedSourceProductionFailureReason::UnsafeHistory,
        fatal};
    preparation.issues.push_back(std::move(issue));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(preflight, preparation, std::nullopt);
    expect(result.status == AurUpdateOperationStatus::BlockedBeforeExecution &&
               result.reduction_issues.empty() && !result.is_success() &&
               !result.execution_status && result.execution_work_items.empty(),
           "Reviewed preparation failure fabricated execution or inconsistency");
    expect_target_statuses(result,
                           {AurUpdateOperationTargetStatus::Failed,
                            AurUpdateOperationTargetStatus::Failed,
                            AurUpdateOperationTargetStatus::Skipped},
                           "reviewed preparation failure");
    const auto expect_payload = [&fatal](const AurUpdatePreparationIssue& retained) {
        expect(retained.reason == AurUpdatePreparationReason::GenericPreparationInconsistent &&
                   retained.affected_update_plan_indices == std::vector<std::size_t>{0, 1} &&
                   retained.reviewed_source_failure.has_value(),
               "Reviewed preparation issue lost its classification or attribution");
        const auto& failure = *retained.reviewed_source_failure;
        const auto* detail = std::get_if<ReviewedSourceFatalStateFailure>(&failure.detail);
        expect(failure.stage == ReviewedSourceProductionFailureStage::FatalStatePreflight &&
                   failure.reason == ReviewedSourceProductionFailureReason::UnsafeHistory &&
                   detail != nullptr && *detail == fatal,
               "Reviewed preparation issue lost its exact typed payload");
    };
    expect(result.preparation_issues.size() == 1,
           "Reviewed preparation failure lost the operation issue");
    expect_payload(result.preparation_issues.front());
    for(std::size_t index = 0; index < 2; ++index) {
        const auto& target = result.targets[index];
        expect(!target.execution_failure_kind && !target.execution_failure_detail &&
                   target.execution_contributions.empty() && target.preparation_issues.size() == 1,
               "Reviewed preparation failure fabricated execution or lost target issue");
        expect_payload(target.preparation_issues.front());
    }
    expect(result.targets[2].preparation_issues.empty(),
           "Reviewed preparation issue was reattributed to an unaffected target");
}

void test_split_and_multiple_lifecycle_reduce_from_child_results() {
    AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "singular-child"),
        executable_target(1, "multiple-child"),
        executable_target(2, "multiple-suite"),
    });
    preflight.targets[0].update =
        update_entry("singular-child", "singular-suite");
    preflight.targets[1].update =
        update_entry("multiple-child", "multiple-suite");
    preflight.targets[2].update =
        update_entry("multiple-suite", "multiple-suite");

    const RootTargetIdentity singular_root{0, "singular-child"};
    const RootTargetIdentity multiple_child_root{1, "multiple-child"};
    const RootTargetIdentity multiple_base_root{2, "multiple-suite"};
    BuildPlan plan;
    plan.root_targets = {
        singular_root,
        multiple_child_root,
        multiple_base_root,
    };
    plan.package_targets = {
        PlannedPackageTarget{
            "singular-child", "singular-suite", {PackageRole::Root}, {singular_root}},
        PlannedPackageTarget{
            "multiple-child", "multiple-suite", {PackageRole::Root}, {multiple_child_root}},
        PlannedPackageTarget{
            "multiple-suite", "multiple-suite", {PackageRole::Root}, {multiple_base_root}},
    };
    plan.order = {
        BuildPlanEntry{"singular-suite", {"singular-child"}},
        BuildPlanEntry{
            "multiple-suite",
            {"multiple-child", "multiple-suite"}},
    };
    preflight.build_plan = std::move(plan);

    preparation_stub::reset();
    const AppConfig config;
    const AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, SavedSourcePreferencePolicy::Strict, false,
            config);
    expect(
        preparation.is_prepared() && preparation.issues.empty() &&
            preparation.projected_build_units.size() == 2 &&
            preparation.invocation.has_value() &&
            preparation.invocation->work_item_attributions().size() ==
                2 &&
            preparation.invocation->work_item_attributions()[0]
                    .package_name == "singular-child" &&
            preparation.invocation->work_item_attributions()[1]
                .package_name.empty() &&
            preparation.invocation->work_item_attributions()[1]
                    .required_target_attributions.size() == 2,
        "Split/multiple lifecycle did not produce an exact prepared invocation");

    ExactReducerInput reducer_input = exact_reducer_input(
        preflight.targets,
        {
            ExactWorkItemExecutionSpec{
                "singular-suite",
                {exact_child(
                    "singular-child", {0},
                    {{0, "singular-child"}},
                    {PackageRole::Root},
                    AurUpdateChildExecutionStatus::Installed)},
                AurUpdateWorkItemExecutionStatus::Updated,
                {}},
            ExactWorkItemExecutionSpec{
                "multiple-suite",
                {
                    exact_child(
                        "multiple-child", {1},
                        {{1, "multiple-child"}},
                        {PackageRole::Root},
                        AurUpdateChildExecutionStatus::
                            Installed),
                    exact_child(
                        "multiple-suite", {2},
                        {{2, "multiple-suite"}},
                        {PackageRole::Root},
                        AurUpdateChildExecutionStatus::
                            SkippedAsNeeded),
                },
                AurUpdateWorkItemExecutionStatus::Updated,
                {}},
        },
        AurUpdateInvocationExecutionStatus::Completed);
    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            reducer_input.preflight,
            reducer_input.preparation,
            reducer_input.execution);

    expect(
        result.status == AurUpdateOperationStatus::Completed,
        "Split/multiple child result operation status differs");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::NoChange,
        },
        "split/multiple child projection");
    expect(
        result.preparation_issues.empty() &&
            result.targets[0].preflight_issues.empty() &&
            result.targets[1].preflight_issues.empty() &&
            result.targets[2].preflight_issues.empty() &&
            result.reduction_issues.empty(),
        "Removed lifecycle blocker leaked a legacy issue into reduction");
}

void test_global_preparation_failure_stays_operation_level() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first"),
        executable_target(1, "second"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        {},
        "global preparation failure"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NotAttempted,
            AurUpdateOperationTargetStatus::NotAttempted,
        },
        "global preparation failure");
    expect(
        result.preparation_issues.size() == 1 &&
            result.preparation_issues.front().reason ==
                AurUpdatePreparationReason::
                    GenericPreparationInconsistent &&
            result.preparation_issues.front().diagnostic ==
                "global preparation failure" &&
            result.targets[0].preparation_issues.empty() &&
            result.targets[1].preparation_issues.empty(),
        "Global preparation issue was not retained at operation level");
    expect(
        result.status ==
            AurUpdateOperationStatus::BlockedBeforeExecution,
        "Global preparation failure did not block execution");
    expect(
        result.reduction_issues.empty(),
        "Normal global preparation failure was inconsistent");
}

void test_blocked_preparation_missing_target_snapshot_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "failed"),
        executable_target(1, "missing-snapshot"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.affected_update_targets.pop_back();
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::SourcePreferenceUnavailable,
        {0},
        "typed target preparation failure"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Blocked preparation accepted a missing executable target snapshot");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Failed,
            AurUpdateOperationTargetStatus::NotAttempted,
        },
        "blocked preparation with missing target snapshot");
}

void test_blocked_preparation_target_snapshot_order_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first"),
        executable_target(1, "second"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    std::swap(
        preparation.affected_update_targets[0],
        preparation.affected_update_targets[1]);
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        {},
        "global preparation failure"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Blocked preparation accepted an out-of-order target snapshot");
}

void test_preparation_issue_attributed_to_skipped_target_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "executable"),
        up_to_date_target(1, "skipped"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::SourcePreferenceUnavailable,
        {1},
        "invalid skipped-target attribution"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationAttributionInconsistent),
        "Preparation issue accepted attribution to a skipped target");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NotAttempted,
            AurUpdateOperationTargetStatus::Skipped,
        },
        "preparation issue attributed to skipped target");
    expect(
        result.targets[1].preparation_issues.size() == 1,
        "Malformed preparation issue attribution was discarded");
}

void test_preparation_warning_attributed_to_skipped_target_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "executable"),
        up_to_date_target(1, "skipped"),
    });
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        {},
        "global preparation failure"));
    preparation.warnings.push_back(AurUpdatePreparationWarning{
        "skipped-warning",
        fs::path("/stub/preferences/skipped-warning"),
        {1},
        {},
        "invalid skipped-target warning attribution"});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            result.preparation_warnings.size() == 1 &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationAttributionInconsistent),
        "Preparation warning accepted attribution to a skipped target");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NotAttempted,
            AurUpdateOperationTargetStatus::Skipped,
        },
        "preparation warning attributed to skipped target");
    expect(
        result.preparation_warnings.front()
                .affected_update_plan_indices ==
            std::vector<std::size_t>{1},
        "Malformed preparation warning attribution was discarded");
}

void test_skip_only_global_warning_remains_no_updates() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({up_to_date_target(0, "skipped")});
    AurUpdateSourceBuildPreparation preparation;
    preparation.warnings.push_back(AurUpdatePreparationWarning{
        "global-warning",
        fs::path("/stub/preferences/global-warning"),
        {},
        {},
        "global warning"});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::NoUpdates &&
            result.is_success() &&
            result.preparation_warnings.size() == 1 &&
            result.reduction_issues.empty(),
        "A global warning changed a normal skip-only operation");
}

void test_skip_only_global_preparation_issue_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({up_to_date_target(0, "skipped")});
    AurUpdateSourceBuildPreparation preparation;
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        {},
        "impossible no-target preparation issue"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            result.preparation_issues.size() == 1 &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationAttributionInconsistent),
        "Skip-only operation accepted a preparation issue");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Skipped},
        "skip-only operation with preparation issue");
}

void test_skip_only_target_warning_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({up_to_date_target(0, "skipped")});
    AurUpdateSourceBuildPreparation preparation;
    preparation.warnings.push_back(AurUpdatePreparationWarning{
        "skipped-warning",
        fs::path("/stub/preferences/skipped-warning"),
        {0},
        {},
        "impossible no-target warning attribution"});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            result.preparation_warnings.size() == 1 &&
            result.preparation_warnings.front()
                    .affected_update_plan_indices ==
                std::vector<std::size_t>{0} &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationAttributionInconsistent),
        "Skip-only operation accepted target-attributed warning");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Skipped},
        "skip-only operation with target warning");
}

void test_skip_only_preparation_snapshot_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({up_to_date_target(0, "skipped")});
    AurUpdateSourceBuildPreparation preparation;
    preparation.affected_update_targets.push_back(preflight.targets.front());

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Skip-only operation accepted a preparation target snapshot");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Skipped},
        "skip-only operation with preparation snapshot");
}

void test_unknown_preparation_reason_is_typed() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "unknown-preparation")});
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.issues.push_back(preparation_issue(
        static_cast<AurUpdatePreparationReason>(999),
        {0},
        "unknown preparation reason"));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownEnumValue),
        "Unknown preparation reason was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Failed},
        "unknown preparation reason");
}

void test_build_unit_preparation_reasons_are_known_blockers() {
    for(const AurUpdatePreparationReason reason : {
            AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
            AurUpdatePreparationReason::ExternalSatisfactionInconsistent}) {
        const AurUpdateExecutionPreflight preflight =
            preflight_with({executable_target(0, "selection-blocked")});
        AurUpdateSourceBuildPreparation preparation =
            preparation_for_execution(preflight);
        preparation.issues.push_back(preparation_issue(
            reason, {0}, "typed build-unit preparation blocker"));

        const AurUpdateOperationResult result =
            reduce_aur_update_operation_result(
                preflight, preparation, std::nullopt);
        expect(
            result.status ==
                    AurUpdateOperationStatus::BlockedBeforeExecution &&
                !has_reduction_issue(
                    result,
                    AurUpdateOperationReductionReason::
                        UnknownEnumValue),
            "Build-unit preparation reason was not accepted as a known blocker");
        expect_target_statuses(
            result, {AurUpdateOperationTargetStatus::Failed},
            "typed build-unit preparation blocker");
    }
}

void test_preparation_warning_only_does_not_fail_execution() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "warning-root")});
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.warnings.push_back(AurUpdatePreparationWarning{
        "warning-root",
        fs::path("/stub/preferences/warning-root"),
        {0},
        {{0, "warning-root"}},
        "first warning"});
    preparation.warnings.push_back(AurUpdatePreparationWarning{
        "global-warning",
        fs::path("/stub/preferences/global-warning"),
        {},
        {},
        "second warning"});
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::NoChange,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.is_success(),
        "Warning-only operation was treated as failure");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NoChange},
        "warning-only operation");
    expect(
        result.preparation_warnings.size() == 2 &&
            result.preparation_warnings[0].diagnostic ==
                "first warning" &&
            result.preparation_warnings[1].diagnostic ==
                "second warning" &&
            result.preparation_warnings[0]
                    .affected_update_plan_indices ==
                std::vector<std::size_t>{0} &&
            result.preparation_warnings[0].affected_roots ==
                std::vector<RootTargetIdentity>{{0, "warning-root"}} &&
            result.preparation_warnings[1]
                .affected_update_plan_indices.empty() &&
            result.preparation_warnings[1].affected_roots.empty() &&
            result.preparation_warnings[1].entry_path ==
                fs::path("/stub/preferences/global-warning"),
        "Preparation warning order or attribution differs");
}

void test_unknown_warning_attribution_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({up_to_date_target(0, "warning-target")});
    AurUpdateSourceBuildPreparation preparation;
    preparation.warnings.push_back(AurUpdatePreparationWarning{
        "unknown-warning",
        fs::path("/stub/preferences/unknown-warning"),
        {99},
        {},
        "unknown warning attribution"});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            result.preparation_warnings.size() == 1 &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownPreparationUpdatePlanIndex),
        "Unknown warning attribution was accepted or discarded");
}

void test_all_updated() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first"),
        executable_target(1, "second"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0}),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::Updated,
                {1}),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::Updated,
        },
        "all-updated operation");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.changed_package_state() &&
            !result.has_partial_completion(),
        "All-updated helpers differ");
}

void test_all_no_change() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first"),
        executable_target(1, "second"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::NoChange,
                {0}),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::NoChange,
                {1}),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NoChange,
            AurUpdateOperationTargetStatus::NoChange,
        },
        "all-no-change operation");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            !result.changed_package_state(),
        "All-no-change helpers differ");
}

void test_updated_and_no_change_targets() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "updated-target"),
        executable_target(1, "unchanged-target"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0}),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::NoChange,
                {1}),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::NoChange,
        },
        "updated/no-change targets");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.changed_package_state() &&
            !result.has_partial_completion() &&
            result.reduction_issues.empty(),
        "Updated/no-change target aggregate differs");
}

void test_updated_and_no_change_contributions_fold_to_updated() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "mixed-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0},
                "dependency"),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::NoChange,
                {0},
                "mixed-root"),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated},
        "updated/no-change fold");
    expect(
        result.targets.front().execution_contributions.size() == 2 &&
            result.targets.front().execution_work_item_index ==
                std::optional<std::size_t>{0} &&
            !has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    DuplicateExecutionChildAttribution),
        "Normal many-to-one attribution was treated as duplicate");
}

void test_one_work_item_projects_to_multiple_targets() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first-root"),
        executable_target(1, "second-root"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::NoChange,
            {0, 1},
            "shared-dependency")});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::NoChange,
            AurUpdateOperationTargetStatus::NoChange,
        },
        "shared work-item projection");
    for(const auto& target : result.targets) {
        expect(
            target.execution_contributions.size() == 1 &&
                target.execution_work_item_index ==
                    std::optional<std::size_t>{0} &&
                target.execution_contributions.front().package_base ==
                    "shared-dependency",
            "Shared work-item contribution differs");
    }
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.reduction_issues.empty(),
        "Normal one-to-many execution attribution was rejected");
}

ExactReducerInput exact_two_child_updated_input() {
    const RootTargetIdentity first_root{0, "exact-first"};
    const RootTargetIdentity second_root{1, "exact-second"};
    return exact_reducer_input(
        {executable_target_with_base(
             0, "exact-first", "exact-split-base"),
         executable_target_with_base(
             1, "exact-second", "exact-split-base")},
        {ExactWorkItemExecutionSpec{
            "exact-split-base",
            {exact_child(
                 "exact-first", {0}, {first_root},
                 {PackageRole::Root},
                 AurUpdateChildExecutionStatus::Installed),
             exact_child(
                 "exact-second", {1}, {second_root},
                 {PackageRole::Root},
                 AurUpdateChildExecutionStatus::Installed)},
            AurUpdateWorkItemExecutionStatus::Updated,
            {}}},
        AurUpdateInvocationExecutionStatus::Completed);
}

void test_exact_multiple_children_project_independently() {
    const RootTargetIdentity first_root{0, "split-first"};
    const RootTargetIdentity second_root{1, "split-second"};
    ExactReducerInput input = exact_reducer_input(
        {executable_target_with_base(
             0, "split-first", "split-base"),
         executable_target_with_base(
             1, "split-second", "split-base")},
        {ExactWorkItemExecutionSpec{
            "split-base",
            {exact_child(
                 "split-first", {0}, {first_root},
                 {PackageRole::Root},
                 AurUpdateChildExecutionStatus::Installed,
                 DesiredInstallReason::Explicit, "3.0-1"),
             exact_child(
                 "split-second", {1}, {second_root},
                 {PackageRole::RuntimeDependency},
                 AurUpdateChildExecutionStatus::SkippedAsNeeded,
                 DesiredInstallReason::Dependency, "4.0-2")},
            AurUpdateWorkItemExecutionStatus::Updated,
            {ArtifactPackageIdentity{
                 "split-unselected-sibling", "9.0-1"},
             ArtifactPackageIdentity{
                 "split-debug-output", "1.0-1"}}}},
        AurUpdateInvocationExecutionStatus::Completed);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::NoChange},
        "exact split child projection");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.reduction_issues.empty() &&
            result.targets[0].execution_contributions.size() == 1 &&
            result.targets[1].execution_contributions.size() == 1,
        "Exact split child projection changed operation cardinality");
    const AurUpdateOperationExecutionContribution& first =
        result.targets[0].execution_contributions.front();
    const AurUpdateOperationExecutionContribution& second =
        result.targets[1].execution_contributions.front();
    expect(
        first.package_name == "split-first" &&
            first.selected_artifact.has_value() &&
            first.selected_artifact->full_version == "3.0-1" &&
            first.desired_install_reason ==
                std::optional<DesiredInstallReason>{
                    DesiredInstallReason::Explicit} &&
            first.status ==
                AurUpdateWorkItemExecutionStatus::Updated,
        "Installed split child contribution differs");
    expect(
        second.package_name == "split-second" &&
            second.selected_artifact.has_value() &&
            second.selected_artifact->full_version == "4.0-2" &&
            second.desired_install_reason ==
                std::optional<DesiredInstallReason>{
                    DesiredInstallReason::Dependency} &&
            second.status ==
                AurUpdateWorkItemExecutionStatus::NoChange,
        "Skipped split child contribution was flattened to Updated");
    for(const AurUpdateOperationTargetResult& target : result.targets) {
        expect(
            std::none_of(
                target.execution_contributions.begin(),
                target.execution_contributions.end(),
                [](const AurUpdateOperationExecutionContribution&
                       contribution) {
                    return contribution.package_name ==
                               "split-unselected-sibling" ||
                           contribution.package_name ==
                               "split-debug-output";
                }),
            "Unselected artifact produced an operation contribution");
    }
    expect(
        result.execution_work_items.size() == 1 &&
            result.execution_work_items.front()
                    .unselected_artifacts.size() == 2,
        "Operation-level snapshot lost unselected artifact identities");
}

void test_exact_one_child_projects_to_multiple_targets_and_roots() {
    const std::vector<RootTargetIdentity> roots = {
        {0, "shared-first-root"},
        {1, "shared-second-root"}};
    ExactReducerInput input = exact_reducer_input(
        {executable_target(0, "shared-first-root"),
         executable_target(1, "shared-second-root")},
        {ExactWorkItemExecutionSpec{
            "shared-dependency-base",
            {exact_child(
                "shared-dependency", {0, 1}, roots,
                {PackageRole::RuntimeDependency},
                AurUpdateChildExecutionStatus::SkippedAsNeeded,
                DesiredInstallReason::Dependency)},
            AurUpdateWorkItemExecutionStatus::NoChange,
            {}}},
        AurUpdateInvocationExecutionStatus::Completed);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NoChange,
         AurUpdateOperationTargetStatus::NoChange},
        "exact shared child projection");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.reduction_issues.empty(),
        "Exact shared child attribution was rejected");
    for(const AurUpdateOperationTargetResult& target : result.targets) {
        expect(
            target.execution_contributions.size() == 1 &&
                target.execution_contributions.front().package_name ==
                    "shared-dependency" &&
                target.execution_contributions.front().affected_roots ==
                    roots &&
                target.execution_contributions.front().roles ==
                    std::vector<PackageRole>{
                        PackageRole::RuntimeDependency},
            "Shared child lost multi-target root/role attribution");
    }
}

void test_exact_mixed_cleanup_partial_success_projects_per_child() {
    const RootTargetIdentity installed_root{0, "cleanup-installed"};
    const RootTargetIdentity skipped_root{1, "cleanup-skipped"};
    ExactReducerInput input = exact_reducer_input(
        {executable_target_with_base(
             0, "cleanup-installed", "cleanup-split-base"),
         executable_target_with_base(
             1, "cleanup-skipped", "cleanup-split-base")},
        {ExactWorkItemExecutionSpec{
            "cleanup-split-base",
            {exact_child(
                 "cleanup-installed", {0}, {installed_root},
                 {PackageRole::Root},
                 AurUpdateChildExecutionStatus::
                     InstalledCleanupFailed),
             exact_child(
                 "cleanup-skipped", {1}, {skipped_root},
                 {PackageRole::RuntimeDependency},
                 AurUpdateChildExecutionStatus::
                     SkippedAsNeededCleanupFailed,
                 DesiredInstallReason::Dependency)},
            AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
            {ArtifactPackageIdentity{
                "cleanup-unselected", "8.0-1"}}}},
        AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::UpdatedCleanupFailed,
         AurUpdateOperationTargetStatus::NoChangeCleanupFailed},
        "exact mixed cleanup projection");
    expect(
        result.status == AurUpdateOperationStatus::
                             StoppedAfterPackageCleanupFailure &&
            result.reduction_issues.empty() &&
            result.changed_package_state() &&
            result.has_partial_completion() &&
            result.has_cleanup_failure() &&
            result.targets[0].execution_contributions.front().status ==
                AurUpdateWorkItemExecutionStatus::
                    UpdatedCleanupFailed &&
            result.targets[1].execution_contributions.front().status ==
                AurUpdateWorkItemExecutionStatus::
                    NoChangeCleanupFailed,
        "Mixed cleanup child outcomes were flattened");
}

void test_exact_split_cancellation_retains_prepared_children() {
    const RootTargetIdentity first{0, "split-a"};
    const RootTargetIdentity second{1, "split-b"};
    const RootTargetIdentity later{2, "later"};
    auto input = exact_reducer_input(
        {executable_target(0, "split-a"), executable_target(1, "split-b"), executable_target(2, "later")},
        {ExactWorkItemExecutionSpec{"split-base",
                                    {exact_child("split-a", {0}, {first}, {PackageRole::Root}, AurUpdateChildExecutionStatus::NotAttempted),
                                     exact_child("split-b", {1}, {second}, {PackageRole::Root}, AurUpdateChildExecutionStatus::NotAttempted)},
                                    AurUpdateWorkItemExecutionStatus::Cancelled,
                                    {}},
         ExactWorkItemExecutionSpec{"later-base",
                                    {exact_child("later", {2}, {later}, {PackageRole::Root}, AurUpdateChildExecutionStatus::NotAttempted)},
                                    AurUpdateWorkItemExecutionStatus::NotAttempted,
                                    {}}},
        AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation);
    input.execution.work_item_results[0].cancellation = ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput};
    const auto result = reduce_aur_update_operation_result(input.preflight, input.preparation, input.execution);
    expect_target_statuses(result, {AurUpdateOperationTargetStatus::Cancelled, AurUpdateOperationTargetStatus::Cancelled, AurUpdateOperationTargetStatus::NotAttempted}, "split cancellation");
    expect(result.reduction_issues.empty(), "Split cancellation lost prepared correlation");
    for(std::size_t i = 0; i < 2; ++i) {
        expect(result.targets[i].execution_work_item_index == 0 && result.targets[i].execution_contributions.size() == 1 &&
                   result.targets[i].execution_contributions.front().required_child_index == i &&
                   result.targets[i].cancellation == ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput},
               "Split cancellation lost child index or reason");
    }
}

void test_exact_current_failure_and_later_not_attempted() {
    const RootTargetIdentity failed_root{0, "failed-child"};
    const RootTargetIdentity later_root{1, "later-child"};
    ExactReducerInput input = exact_reducer_input(
        {executable_target(0, "failed-child"),
         executable_target(1, "later-child")},
        {ExactWorkItemExecutionSpec{
             "failed-base",
             {exact_child(
                 "failed-child", {0}, {failed_root},
                 {PackageRole::Root},
                 AurUpdateChildExecutionStatus::NotAttempted)},
             AurUpdateWorkItemExecutionStatus::Failed,
             {}},
         ExactWorkItemExecutionSpec{
             "later-base",
             {exact_child(
                 "later-child", {1}, {later_root},
                 {PackageRole::Root},
                 AurUpdateChildExecutionStatus::NotAttempted)},
             AurUpdateWorkItemExecutionStatus::NotAttempted,
             {}}},
        AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Failed,
         AurUpdateOperationTargetStatus::NotAttempted},
        "exact failed/not-attempted projection");
    expect(
        result.status ==
                AurUpdateOperationStatus::StoppedOnWorkItemFailure &&
            result.reduction_issues.empty() &&
            result.targets[0].execution_failure_kind ==
                std::optional<AurUpdateWorkItemFailureKind>{
                    AurUpdateWorkItemFailureKind::
                        BuildOrInstallFailed} &&
            result.targets[0].execution_contributions.size() == 1 &&
            result.targets[1].execution_contributions.size() == 1 &&
            result.has_not_attempted_targets(),
        "Failed work item did not outrank the later NotAttempted child");

    input.execution.work_item_results[0].unselected_artifacts.push_back(
        ArtifactPackageIdentity{"failed-ghost", "1.0-1"});
    input.execution.work_item_results[1].unselected_artifacts.push_back(
        ArtifactPackageIdentity{"later-ghost", "1.0-1"});
    const AurUpdateOperationResult invalid_unselected =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);
    const std::size_t unexpected_unselected_issue_count =
        static_cast<std::size_t>(std::count_if(
            invalid_unselected.reduction_issues.begin(),
            invalid_unselected.reduction_issues.end(),
            [](const AurUpdateOperationReductionIssue& issue) {
                return issue.reason ==
                       AurUpdateOperationReductionReason::
                           UnexpectedUnselectedArtifactIdentity;
            }));
    expect(
        invalid_unselected.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            unexpected_unselected_issue_count == 2,
        "Failed and NotAttempted work items accepted unselected artifact identities");
}

void test_exact_failure_payload_coherence_is_typed() {
    const RootTargetIdentity failed_root{0, "payload-failed"};
    ExactReducerInput failed = exact_reducer_input(
        {executable_target(0, "payload-failed")},
        {ExactWorkItemExecutionSpec{
            "payload-base",
            {exact_child(
                "payload-failed", {0}, {failed_root},
                {PackageRole::Root},
                AurUpdateChildExecutionStatus::NotAttempted)},
            AurUpdateWorkItemExecutionStatus::Failed,
            {}}},
        AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure);
    failed.execution.work_item_results.front().failure_detail =
        std::monostate{};

    const AurUpdateOperationResult missing_failure_detail =
        reduce_aur_update_operation_result(
            failed.preflight, failed.preparation,
            failed.execution);
    expect(
        missing_failure_detail.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                missing_failure_detail,
                AurUpdateOperationReductionReason::
                    WorkItemResultInconsistent),
        "Failed work item accepted a missing typed failure detail");

    ExactReducerInput successful = exact_two_child_updated_input();
    successful.execution.work_item_results.front().failure_detail.emplace<AurUpdateSourceBuildFailureSnapshot>(
        AurUpdateSourceBuildFailureSnapshot{
            AurUpdateSourceBuildFailureCategory::Build,
            "impossible success failure detail", std::nullopt});
    successful.execution.work_item_results.front().transaction_failure =
        AurUpdatePackageTransactionFailureSnapshot{
            AurUpdatePackageTransactionFailureCategory::
                CommandFailed,
            {AurUpdatePackageTransactionAttempt{
                {"split-runtime", "2.0-1"},
                DesiredInstallReason::Dependency}},
            5,
            "impossible transaction evidence"};
    const AurUpdateOperationResult successful_with_failure_payload =
        reduce_aur_update_operation_result(
            successful.preflight, successful.preparation,
            successful.execution);
    expect(
        successful_with_failure_payload.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                successful_with_failure_payload,
                AurUpdateOperationReductionReason::
                    WorkItemResultInconsistent),
        "Successful work item accepted failure detail and transaction attempts");

    ExactReducerInput transaction = exact_reducer_input(
        {executable_target(0, "transaction-child")},
        {ExactWorkItemExecutionSpec{
            "transaction-base",
            {exact_child(
                "transaction-child", {0},
                {{0, "transaction-child"}},
                {PackageRole::Root},
                AurUpdateChildExecutionStatus::NotAttempted)},
            AurUpdateWorkItemExecutionStatus::Failed,
            {}}},
        AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure);
    AurUpdateWorkItemExecutionResult& transaction_work_item =
        transaction.execution.work_item_results.front();
    transaction_work_item.transaction_failure =
        AurUpdatePackageTransactionFailureSnapshot{
            AurUpdatePackageTransactionFailureCategory::
                CommandFailed,
            {AurUpdatePackageTransactionAttempt{
                {"transaction-child", "3.0-1"},
                DesiredInstallReason::Explicit}},
            9,
            "scripted transaction failure"};
    transaction_work_item.failure_detail.emplace<
        AurUpdatePackageTransactionFailureSnapshot>(
        *transaction_work_item.transaction_failure);
    transaction_work_item.diagnostic = "scripted transaction failure";

    const AurUpdateOperationResult coherent_transaction =
        reduce_aur_update_operation_result(
            transaction.preflight, transaction.preparation,
            transaction.execution);
    expect(
        coherent_transaction.status ==
                AurUpdateOperationStatus::
                    StoppedOnWorkItemFailure &&
            coherent_transaction.reduction_issues.empty(),
        "Coherent transaction failure payload was rejected");

    transaction_work_item.transaction_failure->attempted_artifacts.front()
        .identity.full_version = "3.0-2";
    const AurUpdateOperationResult mismatched_transaction =
        reduce_aur_update_operation_result(
            transaction.preflight, transaction.preparation,
            transaction.execution);
    expect(
        mismatched_transaction.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                mismatched_transaction,
                AurUpdateOperationReductionReason::
                    WorkItemResultInconsistent),
        "Mismatched transaction attempt snapshots were accepted");

    transaction_work_item.failure_detail.emplace<
        AurUpdateExecutionCorrelationFailure>(
        AurUpdateExecutionCorrelationFailure{
            AurUpdateExecutionCorrelationFailureReason::
                SelectedArtifactIdentityMismatch,
            0,
            "other-child",
            "transaction attempt correlation mismatch"});
    transaction_work_item.diagnostic =
        "transaction attempt correlation mismatch";
    const AurUpdateOperationResult correlation_with_evidence =
        reduce_aur_update_operation_result(
            transaction.preflight, transaction.preparation,
            transaction.execution);
    expect(
        correlation_with_evidence.status ==
                AurUpdateOperationStatus::
                    StoppedOnWorkItemFailure &&
            correlation_with_evidence.reduction_issues.empty() &&
            correlation_with_evidence.execution_work_items.front()
                .transaction_failure.has_value() &&
            correlation_with_evidence.execution_work_items.front()
                    .transaction_failure->exit_code ==
                std::optional<int>{9},
        "Correlation failure did not preserve safe transaction evidence");
}

void test_exact_duplicate_child_index_is_typed() {
    ExactReducerInput input = exact_two_child_updated_input();
    input.execution.work_item_results[0]
        .child_results[1]
        .required_child_index = 0;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    DuplicateExecutionChildAttribution) &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    MissingExecutionChildAttribution),
        "Duplicate required child index was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::NotAttempted},
        "duplicate exact child index");
}

void test_exact_missing_child_is_typed() {
    ExactReducerInput input = exact_two_child_updated_input();
    input.execution.work_item_results[0].child_results.pop_back();

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    MissingExecutionChildAttribution),
        "Missing required execution child was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::NotAttempted},
        "missing exact child");
}

void test_exact_extra_child_is_typed() {
    ExactReducerInput input = exact_two_child_updated_input();
    AurUpdateChildExecutionResult extra =
        input.execution.work_item_results[0].child_results.back();
    extra.required_child_index = 2;
    extra.required_package_name = "unexpected-third";
    extra.selected_artifact = ArtifactPackageIdentity{
        "unexpected-third", "5.0-1"};
    extra.affected_update_plan_indices = {0};
    extra.affected_roots = {{0, "exact-first"}};
    input.execution.work_item_results[0].child_results.push_back(
        std::move(extra));

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnexpectedExecutionChildAttribution),
        "Extra required execution child was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::Updated},
        "extra exact child");
    expect(
        result.targets[0].execution_contributions.size() == 1,
        "Extra execution child produced a target contribution");
}

void test_exact_unknown_child_update_index_is_typed() {
    ExactReducerInput input = exact_two_child_updated_input();
    input.execution.work_item_results[0]
        .child_results[0]
        .affected_update_plan_indices = {99};

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownExecutionChildUpdatePlanIndex) &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    ExecutionChildSnapshotInconsistent),
        "Unknown child update-plan index was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NotAttempted,
         AurUpdateOperationTargetStatus::Updated},
        "unknown exact child update index");
}

void test_exact_selected_identity_mismatch_is_typed() {
    ExactReducerInput input = exact_two_child_updated_input();
    input.execution.work_item_results[0]
        .child_results[0]
        .selected_artifact->package_name = "wrong-selected-identity";

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnexpectedSelectedArtifact),
        "Selected child identity mismatch was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NotAttempted,
         AurUpdateOperationTargetStatus::Updated},
        "selected child identity mismatch");
}

void test_exact_desired_reason_mismatch_is_typed() {
    ExactReducerInput input = exact_two_child_updated_input();
    input.execution.work_item_results[0]
        .child_results[0]
        .desired_install_reason = DesiredInstallReason::Dependency;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    ExecutionChildSnapshotInconsistent),
        "Desired install reason mismatch was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NotAttempted,
         AurUpdateOperationTargetStatus::Updated},
        "desired install reason mismatch");
}

void test_exact_incomplete_work_item_aggregate_keeps_children() {
    ExactReducerInput input = exact_two_child_updated_input();
    input.execution.work_item_results[0].plan_package_names.clear();

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    WorkItemResultInconsistent),
        "Incomplete work-item aggregate was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::Updated},
        "incomplete exact work-item aggregate");
    expect(
        result.changed_package_state() &&
            result.has_partial_completion() &&
            result.targets[0].execution_contributions.size() == 1 &&
            result.targets[1].execution_contributions.size() == 1,
        "Known child completion was lost with an incomplete aggregate");
}

void test_exact_ordinary_singular_regression() {
    const RootTargetIdentity root{0, "ordinary-singular"};
    ExactReducerInput input = exact_reducer_input(
        {executable_target(0, "ordinary-singular")},
        {ExactWorkItemExecutionSpec{
            "ordinary-singular",
            {exact_child(
                "ordinary-singular", {0}, {root},
                {PackageRole::Root},
                AurUpdateChildExecutionStatus::SkippedAsNeeded)},
            AurUpdateWorkItemExecutionStatus::NoChange,
            {}}},
        AurUpdateInvocationExecutionStatus::Completed);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            input.preflight, input.preparation, input.execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NoChange},
        "exact ordinary singular regression");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.reduction_issues.empty() &&
            result.execution_work_items.size() == 1 &&
            result.execution_work_items.front().package_name ==
                "ordinary-singular" &&
            result.targets.front().execution_contributions.size() == 1 &&
            result.targets.front()
                    .execution_contributions.front()
                    .package_name == "ordinary-singular",
        "Ordinary singular execution changed under child projection");
}

void test_cancellation_terminal_coherence() {
    using S = AurUpdateWorkItemExecutionStatus;
    for(const std::vector<S>& states : std::vector<std::vector<S>>{
            {S::Updated, S::Cancelled, S::NotAttempted}, {S::NoChange, S::Cancelled}, {S::Cancelled, S::NotAttempted}, {S::Updated, S::Updated, S::Cancelled}}) {
        std::vector<AurUpdateExecutionTarget> targets;
        std::vector<AurUpdateWorkItemExecutionResult> items;
        for(std::size_t i = 0; i < states.size(); ++i) {
            targets.push_back(executable_target(i, "cancel-target-" + std::to_string(i)));
            items.push_back(work_item_result(i, states[i], {i}));
        }
        auto preflight = preflight_with(std::move(targets));
        auto preparation = preparation_for_execution(preflight);
        auto execution = execution_result(AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation, std::move(items));
        auto reduced = reduce_aur_update_operation_result(preflight, preparation, execution);
        expect(reduced.status == AurUpdateOperationStatus::StoppedOnWorkItemCancellation && reduced.reduction_issues.empty() && !reduced.is_success(), "Valid cancellation became inconsistent/success");
        for(std::size_t i = 0; i < states.size(); ++i) {
            if(states[i] == S::Cancelled) expect(reduced.targets[i].status == AurUpdateOperationTargetStatus::Cancelled && reduced.targets[i].cancellation == execution.work_item_results[i].cancellation && !reduced.targets[i].execution_failure_kind,
                                                 "Target lost cancellation authority");
        }
        auto false_success = reduced;
        false_success.status = AurUpdateOperationStatus::Completed;
        for(auto& target : false_success.targets)
            target.status = AurUpdateOperationTargetStatus::Updated;
        expect(!false_success.is_success(), "Retained cancellation was hidden by success statuses");
        auto malformed = execution;
        malformed.status = AurUpdateInvocationExecutionStatus::Completed;
        expect(reduce_aur_update_operation_result(preflight, preparation, malformed).status == AurUpdateOperationStatus::InconsistentResult, "Cancelled invocation reported completed");
        malformed = execution;
        for(auto& item : malformed.work_item_results)
            if(item.status == S::Cancelled) item.cancellation.reset();
        expect(reduce_aur_update_operation_result(preflight, preparation, malformed).status == AurUpdateOperationStatus::InconsistentResult, "Cancellation missing reason accepted");
        malformed = execution;
        malformed.work_item_results.front().work_item_index = 99;
        expect(reduce_aur_update_operation_result(preflight, preparation, malformed).status == AurUpdateOperationStatus::InconsistentResult, "Wrong terminal/index correlation accepted");
    }
    const auto preflight = preflight_with({executable_target(0, "first"), executable_target(1, "last")});
    const auto preparation = preparation_for_execution(preflight);
    for(const auto second : {S::Updated, S::Failed, S::Cancelled}) {
        const auto execution = execution_result(AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation,
                                                {work_item_result(0, S::Cancelled, {0}), work_item_result(1, second, {1})});
        expect(reduce_aur_update_operation_result(preflight, preparation, execution).status == AurUpdateOperationStatus::InconsistentResult,
               "Post-cancel execution or multiple terminal outcomes accepted");
    }
}

void test_ordinary_failure_and_not_attempted_suffix() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "updated"),
        executable_target(1, "failed"),
        executable_target(2, "not-attempted"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0}),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::Failed,
                {1}),
            work_item_result(
                2,
                AurUpdateWorkItemExecutionStatus::NotAttempted,
                {2}),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status ==
            AurUpdateOperationStatus::StoppedOnWorkItemFailure,
        "Ordinary failure operation status differs");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::Failed,
            AurUpdateOperationTargetStatus::NotAttempted,
        },
        "ordinary failure mapping");
    expect(
        result.targets[1].execution_failure_kind ==
                std::optional<AurUpdateWorkItemFailureKind>{
                    AurUpdateWorkItemFailureKind::
                        BuildOrInstallFailed} &&
            result.targets[1].execution_diagnostic ==
                std::optional<std::string>{
                    "scripted work item failure"},
        "Ordinary failure typed detail differs");
    expect(
        result.has_partial_completion() &&
            result.has_not_attempted_targets(),
        "Ordinary failure helpers differ");
}

void test_updated_cleanup_failure() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "cleanup-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::UpdatedCleanupFailed},
        "updated cleanup failure");
    expect(
        result.status == AurUpdateOperationStatus::
                             StoppedAfterPackageCleanupFailure &&
            result.changed_package_state() &&
            result.has_partial_completion() &&
            result.has_cleanup_failure(),
        "Updated cleanup failure helpers differ");
}

void test_no_change_cleanup_failure() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "cleanup-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::
                NoChangeCleanupFailed,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NoChangeCleanupFailed},
        "no-change cleanup failure");
    expect(
        result.status == AurUpdateOperationStatus::
                             StoppedAfterPackageCleanupFailure &&
            !result.changed_package_state() &&
            !result.has_partial_completion() &&
            result.has_cleanup_failure(),
        "No-change cleanup failure helpers differ");
}

void test_cleanup_failure_after_prior_update_is_partial_completion() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "updated"),
        executable_target(1, "cleanup-failed"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0}),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::
                    NoChangeCleanupFailed,
                {1}),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::NoChangeCleanupFailed,
        },
        "cleanup failure after update");
    expect(
        result.changed_package_state() &&
            result.has_partial_completion() &&
            result.has_cleanup_failure(),
        "Cleanup partial-completion helpers differ");
}

void test_preflight_target_order_is_not_replaced_by_work_item_order() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first-target"),
        up_to_date_target(1, "skipped-target"),
        executable_target(2, "third-target"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::NoChange,
                {2}),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0}),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.targets[0].update.installed_name == "first-target" &&
            result.targets[1].update.installed_name ==
                "skipped-target" &&
            result.targets[2].update.installed_name == "third-target",
        "Result order followed work-item order");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::Skipped,
            AurUpdateOperationTargetStatus::NoChange,
        },
        "preflight-order result");
    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.reduction_issues.empty(),
        "Work-item order differing from target order was rejected");
}

void test_moved_from_preparation_uses_valid_execution_result() {
    const AurUpdateExecutionPreflight preflight =
        prepared_single_root_preflight("moved-root");
    AurUpdateSourceBuildPreparation preparation =
        prepare_single_root(preflight);

    PreparedAurUpdateSourceBuildInvocation consumed =
        std::move(*preparation.invocation);
    expect(consumed.is_valid(), "Moved invocation destination is invalid");
    expect(
        preparation.invocation.has_value() &&
            !preparation.invocation->is_valid(),
        "Preparation fixture is not in the moved-from state");

    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0},
            "moved-root")});
    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::Completed &&
            result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.reduction_issues.empty(),
        "Moved-from preparation was treated as execution failure");
}

void test_prepared_target_without_execution_result_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        prepared_single_root_preflight("missing-result-root");
    const AurUpdateSourceBuildPreparation preparation =
        prepare_single_root(preflight);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    MissingExecutionResult),
        "Missing execution result was not typed as inconsistent");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NotAttempted},
        "missing execution result");
}

void test_execution_without_preparation_snapshot_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "missing-snapshot")});
    const AurUpdateSourceBuildPreparation preparation;
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Execution without a preparation target snapshot was accepted");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Known update was lost after preparation snapshot mismatch");
}

void test_execution_with_mismatched_preparation_snapshot_keeps_update() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "mismatched-snapshot")});
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.affected_update_targets.front().update.installed_version =
        "unexpected-version";
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    PreparationTargetSnapshotInconsistent),
        "Execution accepted a preparation snapshot identity mismatch");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Known update was lost after preparation snapshot identity mismatch");
}

void test_duplicate_execution_attribution_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "duplicate-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0, 0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    DuplicateExecutionChildAttribution),
        "Duplicate execution attribution was not typed");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.targets.front().execution_contributions.size() == 1,
        "Duplicate attribution discarded or duplicated known update");
}

void test_out_of_range_execution_attribution_preserves_known_update() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "known-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0, 99})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownExecutionChildUpdatePlanIndex),
        "Out-of-range execution attribution was not typed");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Known update was lost after out-of-range attribution");
}

void test_only_unknown_execution_attribution_preserves_work_item() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "unmapped-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {99},
            "known-updated-base")});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownExecutionChildUpdatePlanIndex) &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    MissingExecutionAttribution),
        "Fully unknown execution attribution was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::NotAttempted},
        "fully unknown execution attribution");
    expect(
        result.execution_status ==
                std::optional<AurUpdateInvocationExecutionStatus>{
                    AurUpdateInvocationExecutionStatus::
                        Completed} &&
            result.execution_work_items.size() == 1 &&
            result.execution_work_items.front().package_base ==
                "known-updated-base" &&
            result.execution_work_items.front().status ==
                AurUpdateWorkItemExecutionStatus::Updated &&
            result.execution_work_items.front().failure_kind ==
                AurUpdateWorkItemFailureKind::None &&
            result.execution_work_items.front()
                    .affected_update_plan_indices ==
                std::vector<std::size_t>{99} &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Unattributed known update work item was discarded");
}

void test_missing_execution_attribution_is_inconsistent() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "known-root"),
        executable_target(1, "missing-root"),
    });
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    MissingExecutionAttribution),
        "Missing execution attribution was not typed");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::NotAttempted,
        },
        "missing execution attribution");
}

void test_execution_with_preparation_issue_keeps_known_update() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "known-root")});
    AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    preparation.issues.push_back(preparation_issue(
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        {0},
        "inconsistent preparation issue"));
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    ExecutionResultWithPreparationIssues),
        "Execution/preparation contradiction was not typed");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.targets.front().preparation_issues.size() == 1 &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Known update was lost after preparation contradiction");
}

void test_duplicate_preflight_index_keeps_exact_target_count() {
    const AurUpdateExecutionPreflight preflight = preflight_with({
        executable_target(0, "first-duplicate"),
        executable_target(0, "second-duplicate"),
    });
    const AurUpdateSourceBuildPreparation preparation;

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, std::nullopt);

    expect(
        result.targets.size() == preflight.targets.size() &&
            result.targets[0].update.installed_name ==
                "first-duplicate" &&
            result.targets[1].update.installed_name ==
                "second-duplicate",
        "Duplicate preflight index changed target count or order");
    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    DuplicatePreflightUpdatePlanIndex),
        "Duplicate preflight index was not typed");
    expect_target_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Incomplete,
            AurUpdateOperationTargetStatus::Incomplete,
        },
        "duplicate preflight index");
}

void test_unknown_execution_enum_is_typed_without_throwing() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "unknown-status-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    AurUpdateWorkItemExecutionResult unknown = work_item_result(
        0,
        AurUpdateWorkItemExecutionStatus::NoChange,
        {0});
    unknown.status = static_cast<AurUpdateWorkItemExecutionStatus>(999);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {std::move(unknown)});

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownEnumValue),
        "Unknown execution enum was not typed");
    expect_target_statuses(
        result,
        {AurUpdateOperationTargetStatus::Incomplete},
        "unknown execution enum");
}

void test_unknown_invocation_status_preserves_known_update() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "unknown-invocation")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::Completed,
        {work_item_result(
            0,
            AurUpdateWorkItemExecutionStatus::Updated,
            {0})});
    execution.status =
        static_cast<AurUpdateInvocationExecutionStatus>(999);

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.status == AurUpdateOperationStatus::InconsistentResult &&
            has_reduction_issue(
                result,
                AurUpdateOperationReductionReason::
                    UnknownEnumValue),
        "Unknown invocation status was not typed");
    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.changed_package_state() &&
            result.has_partial_completion(),
        "Known update was lost after unknown invocation status");
}

void test_repository_provider_phase_reduces_without_work_item_attribution() {
    const AurUpdateExecutionPreflight preflight = preflight_with({executable_target(0, "before-provider-owner"),
                                                                  executable_target(1, "provider-owner"),
                                                                  executable_target(2, "after-provider-owner")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);

    SelectedRepositoryProviderTransactionResult provider_failure;
    provider_failure.status =
        SelectedRepositoryProviderTransactionStatus::Failed;
    provider_failure.selected_providers = {
        ProvidedDependency::from_repository("extra", "provider-pkg")};
    provider_failure.package_state_change = PackageStateChange::Unknown;
    provider_failure.command_exit_status = 42;
    provider_failure.diagnostic = "scripted provider transaction failure";
    const AurUpdateSourceBuildExecutionResult failed_execution =
        execution_result(
            AurUpdateInvocationExecutionStatus::
                StoppedOnProviderTransactionFailure,
            {
                work_item_result(
                    0,
                    AurUpdateWorkItemExecutionStatus::
                        NotAttempted,
                    {0}, "before-provider-owner"),
                work_item_result(
                    1,
                    AurUpdateWorkItemExecutionStatus::
                        NotAttempted,
                    {1}, "provider-owner"),
                work_item_result(
                    2,
                    AurUpdateWorkItemExecutionStatus::
                        NotAttempted,
                    {2}, "after-provider-owner"),
            },
            provider_failure);

    const AurUpdateOperationResult failed =
        reduce_aur_update_operation_result(
            preflight, preparation, failed_execution);
    expect(
        failed.status == AurUpdateOperationStatus::
                             StoppedOnProviderTransactionFailure &&
            failed.reduction_issues.empty() &&
            failed.selected_repository_provider_transaction.selected_providers ==
                provider_failure.selected_providers &&
            failed.package_state_change() ==
                PackageStateChange::Unknown &&
            !failed.changed_package_state() &&
            !failed.has_partial_completion() &&
            failed.has_not_attempted_targets(),
        "Provider phase failure was attributed to a work item or lost by reduction");
    expect_target_statuses(
        failed,
        {AurUpdateOperationTargetStatus::NotAttempted,
         AurUpdateOperationTargetStatus::NotAttempted,
         AurUpdateOperationTargetStatus::NotAttempted},
        "provider phase failure");

    SelectedRepositoryProviderTransactionResult provider_unknown =
        provider_failure;
    provider_unknown.status =
        SelectedRepositoryProviderTransactionStatus::OutcomeUnknown;
    provider_unknown.command_exit_status.reset();
    provider_unknown.diagnostic =
        "scripted provider transaction outcome unknown";
    const AurUpdateSourceBuildExecutionResult unknown_execution =
        execution_result(
            AurUpdateInvocationExecutionStatus::
                StoppedOnProviderTransactionFailure,
            failed_execution.work_item_results, provider_unknown);
    const AurUpdateOperationResult unknown =
        reduce_aur_update_operation_result(
            preflight, preparation, unknown_execution);
    expect(
        unknown.status == AurUpdateOperationStatus::
                              StoppedOnProviderTransactionFailure &&
            unknown.reduction_issues.empty() &&
            unknown.selected_repository_provider_transaction.status ==
                SelectedRepositoryProviderTransactionStatus::
                    OutcomeUnknown &&
            !unknown.changed_package_state() &&
            unknown.has_not_attempted_targets(),
        "Unknown provider outcome became an inconsistent aggregate");

    SelectedRepositoryProviderTransactionResult provider_success;
    provider_success.status =
        SelectedRepositoryProviderTransactionStatus::Succeeded;
    provider_success.selected_providers =
        provider_failure.selected_providers;
    provider_success.package_state_change = PackageStateChange::Unknown;
    provider_success.command_exit_status = 0;
    const AurUpdateSourceBuildExecutionResult later_work_item_failure =
        execution_result(
            AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure,
            {
                work_item_result(
                    0,
                    AurUpdateWorkItemExecutionStatus::Failed,
                    {0}, "before-provider-owner"),
                work_item_result(
                    1,
                    AurUpdateWorkItemExecutionStatus::
                        NotAttempted,
                    {1}, "provider-owner"),
                work_item_result(
                    2,
                    AurUpdateWorkItemExecutionStatus::
                        NotAttempted,
                    {2}, "after-provider-owner"),
            },
            provider_success);
    const AurUpdateOperationResult partial =
        reduce_aur_update_operation_result(
            preflight, preparation, later_work_item_failure);
    expect(
        partial.status ==
                AurUpdateOperationStatus::StoppedOnWorkItemFailure &&
            partial.reduction_issues.empty() &&
            partial.package_state_change() ==
                PackageStateChange::Unknown &&
            partial.has_partial_completion(),
        "Successful provider phase was not retained as partial completion after a later failure");
}

void test_same_target_partial_update_and_failure_keeps_contributions() {
    const AurUpdateExecutionPreflight preflight =
        preflight_with({executable_target(0, "multi-work-root")});
    const AurUpdateSourceBuildPreparation preparation =
        preparation_for_execution(preflight);
    const AurUpdateSourceBuildExecutionResult execution = execution_result(
        AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure,
        {
            work_item_result(
                0,
                AurUpdateWorkItemExecutionStatus::Updated,
                {0},
                "dependency"),
            work_item_result(
                1,
                AurUpdateWorkItemExecutionStatus::Failed,
                {0},
                "multi-work-root"),
            work_item_result(
                2,
                AurUpdateWorkItemExecutionStatus::NotAttempted,
                {0},
                "suffix"),
        });

    const AurUpdateOperationResult result =
        reduce_aur_update_operation_result(
            preflight, preparation, execution);

    expect(
        result.targets.front().status ==
                AurUpdateOperationTargetStatus::Failed &&
            result.targets.front().execution_work_item_index ==
                std::optional<std::size_t>{1} &&
            result.targets.front().execution_contributions.size() == 3,
        "Same-target terminal fold differs");
    expect(
        result.changed_package_state() &&
            result.has_partial_completion() &&
            !result.has_not_attempted_targets(),
        "Same-target partial mutation helpers differ");
}

void test_all_query_helpers() {
    AurUpdateOperationResult missing_policy;
    missing_policy.status = AurUpdateOperationStatus::NoUpdates;
    expect(
        !missing_policy.is_success(),
        "NoUpdates with a missing RequiresCheck policy succeeded");

    AurUpdateOperationResult no_updates;
    no_updates.status = AurUpdateOperationStatus::NoUpdates;
    no_updates.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    expect(no_updates.is_success(), "NoUpdates helper returned failure");
    expect(
        !no_updates.changed_package_state() &&
            !no_updates.has_partial_completion() &&
            !no_updates.has_not_attempted_targets() &&
            !no_updates.has_cleanup_failure() &&
            !no_updates.has_blocking_targets(),
        "NoUpdates helper baseline differs");

    AurUpdateOperationResult completed;
    completed.status = AurUpdateOperationStatus::Completed;
    completed.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    AurUpdateOperationTargetResult updated;
    updated.status = AurUpdateOperationTargetStatus::Updated;
    completed.targets.push_back(std::move(updated));
    expect(
        completed.is_success() &&
            completed.changed_package_state() &&
            !completed.has_partial_completion(),
        "Completed helper semantics differ");
    AurUpdateRequiredDevelTargetBlocker required_blocker{
        AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild,
        1,
        "required-devel-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    AurUpdateExecutionIssue required_issue;
    required_issue.reason =
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck;
    required_issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    required_issue.required_devel_target_blocker =
        std::move(required_blocker);
    completed.targets.front().preflight_issues.push_back(
        std::move(required_issue));
    expect(
        !completed.is_success(),
        "Required devel blocker survived in a successful target snapshot");

    AurUpdateOperationResult partial;
    partial.status =
        AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure;
    AurUpdateOperationTargetResult cleanup;
    cleanup.status =
        AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
    AurUpdateOperationTargetResult not_attempted;
    not_attempted.status = AurUpdateOperationTargetStatus::NotAttempted;
    AurUpdateOperationTargetResult blocker;
    blocker.status = AurUpdateOperationTargetStatus::Unsupported;
    partial.targets = {
        std::move(cleanup),
        std::move(not_attempted),
        std::move(blocker)};
    expect(
        !partial.is_success() &&
            partial.changed_package_state() &&
            partial.has_partial_completion() &&
            partial.has_not_attempted_targets() &&
            partial.has_cleanup_failure() &&
            partial.has_blocking_targets(),
        "Failure-state helper semantics differ");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        test_exact_split_cancellation_retains_prepared_children();
        test_cancellation_terminal_coherence();
        run_case("all skipped is NoUpdates", test_all_skipped_is_no_updates);
        run_case(
            "independent RequiresCheck result semantics",
            test_independent_devel_requires_check_result_semantics);
        run_case(
            "required devel blocker result correlation",
            test_required_devel_blocker_result_correlation);
        run_case(
            "missing required devel relation result is inconsistent",
            test_missing_required_devel_relation_result_is_inconsistent);
        run_case(
            "skip order and typed reasons are preserved",
            test_skip_order_and_typed_reasons_are_preserved);
        run_case(
            "skipped target without normal reason is inconsistent",
            test_skipped_without_normal_reason_is_inconsistent);
        run_case(
            "RequiresCheck policy snapshots fail closed",
            test_requires_check_policy_snapshots_fail_closed);
        run_case(
            "unknown preflight reason is typed",
            test_unknown_preflight_reason_is_typed);
        run_case(
            "unknown owned plan enum is typed",
            test_unknown_owned_plan_enum_is_typed);
        run_case(
            "executable preflight issue keeps known update",
            test_executable_preflight_issue_keeps_known_update);
        run_case(
            "executable hidden required-devel payload is inconsistent",
            test_executable_hidden_required_devel_payload_is_inconsistent);
        run_case(
            "executable reason-None issue is inconsistent",
            test_executable_reason_none_issue_is_inconsistent);
        run_case(
            "preflight blockers preserve status and stop executables",
            test_preflight_blockers_keep_status_and_stop_executable_targets);
        run_case(
            "preflight blocker missing snapshot is inconsistent",
            test_preflight_blocker_missing_snapshot_is_inconsistent);
        run_case(
            "projection payload snapshot drift is inconsistent",
            test_projection_payload_snapshot_drift_is_inconsistent);
        run_case(
            "preflight blocker with invocation is inconsistent",
            test_preflight_blocker_with_invocation_is_inconsistent);
        run_case(
            "target-attributed preparation failure",
            test_target_attributed_preparation_failure);
        run_case(
            "reviewed preparation failure retains target and operation payload",
            test_reviewed_preparation_failure_retains_target_and_operation_payload);
        run_case(
            "split and multiple lifecycle reduce from child results",
            test_split_and_multiple_lifecycle_reduce_from_child_results);
        run_case(
            "global preparation failure stays operation-level",
            test_global_preparation_failure_stays_operation_level);
        run_case(
            "blocked preparation missing target snapshot is inconsistent",
            test_blocked_preparation_missing_target_snapshot_is_inconsistent);
        run_case(
            "blocked preparation target order is inconsistent",
            test_blocked_preparation_target_snapshot_order_is_inconsistent);
        run_case(
            "preparation issue cannot target a skipped target",
            test_preparation_issue_attributed_to_skipped_target_is_inconsistent);
        run_case(
            "preparation warning cannot target a skipped target",
            test_preparation_warning_attributed_to_skipped_target_is_inconsistent);
        run_case(
            "skip-only global warning remains NoUpdates",
            test_skip_only_global_warning_remains_no_updates);
        run_case(
            "skip-only preparation issue is inconsistent",
            test_skip_only_global_preparation_issue_is_inconsistent);
        run_case(
            "skip-only target warning is inconsistent",
            test_skip_only_target_warning_is_inconsistent);
        run_case(
            "skip-only preparation snapshot is inconsistent",
            test_skip_only_preparation_snapshot_is_inconsistent);
        run_case(
            "unknown preparation reason is typed",
            test_unknown_preparation_reason_is_typed);
        run_case(
            "build-unit preparation reasons are known blockers",
            test_build_unit_preparation_reasons_are_known_blockers);
        run_case(
            "preparation warning alone does not fail execution",
            test_preparation_warning_only_does_not_fail_execution);
        run_case(
            "unknown warning attribution is inconsistent",
            test_unknown_warning_attribution_is_inconsistent);
        run_case("all updated", test_all_updated);
        run_case("all no-change", test_all_no_change);
        run_case(
            "updated and no-change targets",
            test_updated_and_no_change_targets);
        run_case(
            "updated/no-change contributions fold to updated",
            test_updated_and_no_change_contributions_fold_to_updated);
        run_case(
            "one work item projects to multiple targets",
            test_one_work_item_projects_to_multiple_targets);
        run_case(
            "exact multiple children project independently",
            test_exact_multiple_children_project_independently);
        run_case(
            "exact one child projects to multiple targets and roots",
            test_exact_one_child_projects_to_multiple_targets_and_roots);
        run_case(
            "exact mixed cleanup partial success projects per child",
            test_exact_mixed_cleanup_partial_success_projects_per_child);
        run_case(
            "exact current failure and later child not-attempted",
            test_exact_current_failure_and_later_not_attempted);
        run_case(
            "exact failure payload coherence is typed",
            test_exact_failure_payload_coherence_is_typed);
        run_case(
            "exact duplicate child index is typed",
            test_exact_duplicate_child_index_is_typed);
        run_case(
            "exact missing child is typed",
            test_exact_missing_child_is_typed);
        run_case(
            "exact extra child is typed",
            test_exact_extra_child_is_typed);
        run_case(
            "exact unknown child update index is typed",
            test_exact_unknown_child_update_index_is_typed);
        run_case(
            "exact selected identity mismatch is typed",
            test_exact_selected_identity_mismatch_is_typed);
        run_case(
            "exact desired reason mismatch is typed",
            test_exact_desired_reason_mismatch_is_typed);
        run_case(
            "exact incomplete aggregate keeps child outcomes",
            test_exact_incomplete_work_item_aggregate_keeps_children);
        run_case(
            "exact ordinary singular regression",
            test_exact_ordinary_singular_regression);
        run_case(
            "ordinary failure and not-attempted suffix",
            test_ordinary_failure_and_not_attempted_suffix);
        run_case("updated cleanup failure", test_updated_cleanup_failure);
        run_case(
            "no-change cleanup failure",
            test_no_change_cleanup_failure);
        run_case(
            "cleanup failure after prior update is partial completion",
            test_cleanup_failure_after_prior_update_is_partial_completion);
        run_case(
            "preflight target order is preserved",
            test_preflight_target_order_is_not_replaced_by_work_item_order);
        run_case(
            "moved-from preparation uses valid execution result",
            test_moved_from_preparation_uses_valid_execution_result);
        run_case(
            "prepared target without execution result is inconsistent",
            test_prepared_target_without_execution_result_is_inconsistent);
        run_case(
            "execution without preparation snapshot is inconsistent",
            test_execution_without_preparation_snapshot_is_inconsistent);
        run_case(
            "execution snapshot mismatch keeps known update",
            test_execution_with_mismatched_preparation_snapshot_keeps_update);
        run_case(
            "duplicate execution attribution is inconsistent",
            test_duplicate_execution_attribution_is_inconsistent);
        run_case(
            "out-of-range attribution preserves known update",
            test_out_of_range_execution_attribution_preserves_known_update);
        run_case(
            "fully unknown attribution preserves work item",
            test_only_unknown_execution_attribution_preserves_work_item);
        run_case(
            "missing execution attribution is inconsistent",
            test_missing_execution_attribution_is_inconsistent);
        run_case(
            "execution with preparation issue keeps known update",
            test_execution_with_preparation_issue_keeps_known_update);
        run_case(
            "duplicate preflight index keeps exact target count",
            test_duplicate_preflight_index_keeps_exact_target_count);
        run_case(
            "unknown execution enum is typed",
            test_unknown_execution_enum_is_typed_without_throwing);
        run_case(
            "unknown invocation status preserves known update",
            test_unknown_invocation_status_preserves_known_update);
        run_case(
            "repository provider phase has no work-item attribution",
            test_repository_provider_phase_reduces_without_work_item_attribution);
        run_case(
            "same-target partial update and failure keeps contributions",
            test_same_target_partial_update_and_failure_keeps_contributions);
        run_case("all query helpers", test_all_query_helpers);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "AUR update operation result tests: all checks passed\n";
    return 0;
}
