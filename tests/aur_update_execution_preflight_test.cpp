#include "aur_update_execution_preflight.hpp"
#include "aur_update_required_devel_relation_projection.hpp"
#include "dependency_provider.hpp"
#include "stubs/aur-update-execution-preflight/preflight_stub.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using aur_update_execution_preflight_test_stub::reset_preflight_stub;
using aur_update_execution_preflight_test_stub::resolver_call_count;
using aur_update_execution_preflight_test_stub::resolver_calls;
using aur_update_execution_preflight_test_stub::resolver_selection_callback_presence;
using aur_update_execution_preflight_test_stub::set_resolver_handler;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan) {
    return ::resolve_aur_update_execution_preflight(
        update_plan, DevelRequiresCheckPolicy::BlockOperation);
}

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan,
    const ProviderSelectionCallback& select_provider) {
    return ::resolve_aur_update_execution_preflight(
        update_plan, DevelRequiresCheckPolicy::BlockOperation,
        select_provider);
}

AurUpdatePlanEntry remote_entry(
    const std::string& installed_name,
    InstalledPackageReason installed_reason,
    AurUpdateClassification classification =
        AurUpdateClassification::UpdateAvailable,
    const std::string& aur_name = {},
    const std::string& package_base = {}) {
    const std::string resolved_aur_name =
        aur_name.empty() ? installed_name : aur_name;
    const std::string resolved_package_base =
        package_base.empty() ? resolved_aur_name : package_base;
    return AurUpdatePlanEntry{
        installed_name,
        "1.0-1",
        installed_reason,
        AurUpdateRemotePackage{
            resolved_aur_name,
            resolved_package_base,
            "2.0-1",
            classification == AurUpdateClassification::UpToDate
                ? AurVersionRelation::SameAsInstalled
            : classification ==
                    AurUpdateClassification::
                        VersionComparisonUnavailable
                ? AurVersionRelation::Unavailable
                : AurVersionRelation::NewerThanInstalled},
        classification};
}

AurUpdatePlanEntry classified_update_entry(
    const std::string& package_name,
    const std::string& package_base = {}) {
    const std::string resolved_package_base =
        package_base.empty() ? package_name : package_base;
    return classify_aur_update(AurUpdatePlanInput{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            resolved_package_base,
            "2.0-1",
            AurVersionRelation::NewerThanInstalled}});
}

AurUpdatePlanEntry requires_check_entry(
    const std::string& package_name,
    const std::string& package_base = {},
    const std::string& remote_version = "1.0-1") {
    const std::string resolved_package_base =
        package_base.empty() ? package_name : package_base;
    return classify_aur_update(AurUpdatePlanInput{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            resolved_package_base,
            remote_version,
            AurVersionRelation::SameAsInstalled}});
}

AurUpdatePlanEntry entry_without_remote(
    const std::string& installed_name,
    AurUpdateClassification classification,
    InstalledPackageReason installed_reason =
        InstalledPackageReason::Unknown) {
    return AurUpdatePlanEntry{
        installed_name,
        "1.0-1",
        installed_reason,
        std::nullopt,
        classification};
}

struct RootFixture {
    std::string requested_name;
    std::string package_name;
    std::string package_base;
};

PlannedPackageTarget* find_package_target(
    BuildPlan& plan, const std::string& package_name) {
    auto found = std::find_if(
        plan.package_targets.begin(), plan.package_targets.end(),
        [&package_name](const PlannedPackageTarget& target) {
            return target.package_name == package_name;
        });
    return found == plan.package_targets.end() ? nullptr : &(*found);
}

void add_root_fixture(
    BuildPlan& plan, std::size_t invocation_index,
    const RootFixture& fixture) {
    RootTargetIdentity root{invocation_index, fixture.requested_name};
    plan.root_targets.push_back(root);

    PlannedPackageTarget* target =
        find_package_target(plan, fixture.package_name);
    if(target == nullptr) {
        plan.package_targets.push_back(PlannedPackageTarget{
            fixture.package_name,
            fixture.package_base,
            {PackageRole::Root},
            {root}});
    } else {
        if(std::find(target->roles.begin(), target->roles.end(), PackageRole::Root) ==
           target->roles.end()) {
            target->roles.push_back(PackageRole::Root);
        }
        target->roots.push_back(root);
    }

    auto same_base = [&fixture](const BuildPlanEntry& entry) {
        return entry.package_base == fixture.package_base;
    };
    auto order_entry =
        std::find_if(plan.order.begin(), plan.order.end(), same_base);
    if(order_entry == plan.order.end()) {
        plan.order.push_back(
            BuildPlanEntry{fixture.package_base, {fixture.package_name}});
    } else if(std::find(
                  order_entry->package_names.begin(),
                  order_entry->package_names.end(),
                  fixture.package_name) == order_entry->package_names.end()) {
        order_entry->package_names.push_back(fixture.package_name);
    }
}

BuildPlan build_plan_for(const std::vector<RootFixture>& roots) {
    BuildPlan plan;
    for(std::size_t i = 0; i < roots.size(); ++i) {
        add_root_fixture(plan, i, roots[i]);
    }
    return plan;
}

PackageRelationAssessment relation_assessment_fixture(
    PackageRelationAssessmentKind kind,
    std::string package_name = "relation-root",
    std::string package_base = "relation-root",
    std::vector<PackageRelationRootAttribution> roots = {
        {0, "relation-root"}}) {
    const PackageRelationKind declaration_kind =
        kind == PackageRelationAssessmentKind::PotentialReplacement
            ? PackageRelationKind::Replacement
            : PackageRelationKind::Conflict;
    const std::string source_name = package_name;
    const std::string source_base = package_base;
    PackageRelationObservedPackage declaring{
        package_name,
        package_base,
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage, "2"),
        {},
        PackageRelationAurSourceIdentity{source_name, source_base},
        PackageRelationObservationRole::PlannedTarget,
        std::move(roots)};
    DeclaredPackageRelation declaration(
        package_name, package_base, declaration_kind,
        "relation-target", "relation-target", std::nullopt);
    PackageRelationMatchingEvidence evidence{
        PackageRelationObservationCompleteness::Complete,
        {},
        {},
        {},
        {}};
    std::optional<PackageRelationMatchEvidence> package_evidence;
    std::optional<PackageRelationObservationFailure> observation_failure;

    if(kind == PackageRelationAssessmentKind::
                   ConfirmedInstalledConflict ||
       kind == PackageRelationAssessmentKind::PotentialReplacement) {
        const PackageRelationInstalledDatabaseIdentity source{
            "/", "/var/lib/pacman"};
        package_evidence = PackageRelationMatchEvidence{
            PackageRelationObservedPackage{
                "relation-target",
                std::nullopt,
                ObservedVersion::available(
                    ObservedVersionSource::
                        InstalledExactPackage,
                    "1"),
                {},
                source,
                PackageRelationObservationRole::Installed,
                {}},
            PackageRelationIdentityMatchKind::ExactPackage,
            PackageRelationVersionMatchKind::Unconstrained,
            {},
            std::nullopt};
        evidence.package_evidence.push_back(*package_evidence);
    } else if(kind == PackageRelationAssessmentKind::
                          ConfirmedPlannedTargetConflict) {
        package_evidence = PackageRelationMatchEvidence{
            PackageRelationObservedPackage{
                "relation-target",
                "relation-target-base",
                ObservedVersion::available(
                    ObservedVersionSource::AurExactPackage,
                    "1"),
                {},
                PackageRelationAurSourceIdentity{
                    "relation-target", "relation-target-base"},
                PackageRelationObservationRole::PlannedTarget,
                {{0, "relation-root"}}},
            PackageRelationIdentityMatchKind::ExactPackage,
            PackageRelationVersionMatchKind::Unconstrained,
            {},
            std::nullopt};
        evidence.package_evidence.push_back(*package_evidence);
    } else if(kind == PackageRelationAssessmentKind::Unknown ||
              kind == PackageRelationAssessmentKind::Invalid) {
        const bool is_invalid =
            kind == PackageRelationAssessmentKind::Invalid;
        evidence.observation_completeness = is_invalid
                                                ? PackageRelationObservationCompleteness::Invalid
                                                : PackageRelationObservationCompleteness::Unavailable;
        observation_failure = PackageRelationObservationFailure{
            is_invalid
                ? PackageRelationObservationFailureKind::
                      MalformedMetadata
                : PackageRelationObservationFailureKind::
                      SourceUnavailable,
            PackageRelationObservationRole::Installed,
            std::nullopt,
            std::nullopt,
            is_invalid ? "invalid relation observation"
                       : "relation observation unavailable"};
        evidence.observation_failures.push_back(*observation_failure);
    } else if(kind == PackageRelationAssessmentKind::DeclaredRelation) {
        evidence.observation_completeness =
            PackageRelationObservationCompleteness::Unavailable;
    }

    return PackageRelationAssessment{
        std::move(declaration),
        kind,
        std::move(declaring),
        std::move(evidence),
        std::nullopt,
        std::move(package_evidence),
        std::move(observation_failure)};
}

void add_dependency_target(
    BuildPlan& plan, const std::string& package_name,
    const std::string& package_base,
    const std::vector<RootTargetIdentity>& roots,
    PackageRole role = PackageRole::RuntimeDependency) {
    plan.package_targets.push_back(
        PlannedPackageTarget{package_name, package_base, {role}, roots});
}

BuildPlanDependencyEdge provided_dependency_edge(
    const std::string& parent_package_name,
    const std::string& dependency_spec,
    std::optional<ProvidedDependency> provider) {
    return BuildPlanDependencyEdge{
        parent_package_name,
        parent_package_name,
        dependency_spec,
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        std::move(provider)};
}

DependencyRequirement exact_requirement(
    const std::string& package_name) {
    return ConsumerDependencyRequirement(
        package_name, package_name, std::nullopt);
}

void add_required_package_target(
    BuildPlan& plan,
    const std::string& package_name,
    const std::string& package_base,
    const std::vector<RootTargetIdentity>& roots,
    PackageRole role = PackageRole::RuntimeDependency) {
    add_dependency_target(
        plan, package_name, package_base, roots, role);

    auto same_base = [&package_base](const BuildPlanEntry& entry) {
        return entry.package_base == package_base;
    };
    auto order = std::find_if(
        plan.order.begin(), plan.order.end(), same_base);
    if(order == plan.order.end()) {
        plan.order.insert(
            plan.order.begin(), BuildPlanEntry{package_base, {package_name}});
        return;
    }
    if(std::find(
           order->package_names.begin(), order->package_names.end(),
           package_name) == order->package_names.end()) {
        order->package_names.push_back(package_name);
    }
}

BuildPlanDependencyEdge typed_aur_exact_edge(
    const std::string& parent_package_name,
    const std::string& parent_package_base,
    const std::string& dependency_package_name,
    const std::string& dependency_package_base,
    PackageRole role = PackageRole::RuntimeDependency,
    const std::string& candidate_version = "1.0-1",
    ConstraintEvaluation evaluation =
        ConstraintEvaluation::unconstrained()) {
    return BuildPlanDependencyEdge{
        parent_package_name,
        parent_package_base,
        dependency_package_name,
        role,
        DependencyKind::Aur,
        dependency_package_name,
        dependency_package_base,
        std::nullopt,
        ProviderResolutionKind::Unique,
        exact_requirement(dependency_package_name),
        ResolvedDependencyCandidate{AurResolvedDependencyCandidate{
            dependency_package_name,
            dependency_package_base,
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                candidate_version)}},
        std::move(evaluation)};
}

BuildPlanDependencyEdge typed_repository_exact_edge(
    const std::string& parent_package_name,
    const std::string& parent_package_base,
    const std::string& dependency_package_name,
    const std::string& dependency_package_base,
    const std::string& candidate_version = "1.0-1") {
    return BuildPlanDependencyEdge{
        parent_package_name,
        parent_package_base,
        dependency_package_name,
        PackageRole::RuntimeDependency,
        DependencyKind::Repo,
        dependency_package_name,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        exact_requirement(dependency_package_name),
        ResolvedDependencyCandidate{RepositoryExactPackage{
            ConfiguredRepositoryIdentity{"extra", 0},
            dependency_package_name,
            dependency_package_base,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                candidate_version),
            {},
            std::optional<std::string>{"x86_64"}}},
        ConstraintEvaluation::unconstrained()};
}

BuildPlanDependencyEdge typed_installed_exact_edge(
    const std::string& parent_package_name,
    const std::string& parent_package_base,
    const std::string& dependency_package_name,
    const std::string& installed_version = "1.0-1") {
    return BuildPlanDependencyEdge{
        parent_package_name,
        parent_package_base,
        dependency_package_name,
        PackageRole::RuntimeDependency,
        DependencyKind::Installed,
        dependency_package_name,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        exact_requirement(dependency_package_name),
        ResolvedDependencyCandidate{InstalledExactPackage{
            dependency_package_name,
            ObservedVersion::available(
                ObservedVersionSource::InstalledExactPackage,
                installed_version)}},
        ConstraintEvaluation::unconstrained()};
}

ProvidedDependency typed_aur_provider(
    const std::string& package_name,
    const std::string& package_base,
    const std::string& dependency_name,
    const std::string& package_version = "1.0-1") {
    const ProviderCapability capability(
        dependency_name + "=" + package_version,
        dependency_name,
        package_version);
    return ProvidedDependency::from_aur_constraint_metadata(
        package_name, package_base,
        ProviderConstraintMetadata{
            capability,
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                package_version),
            ObservedVersion::available(
                ObservedVersionSource::AurProviderCapability,
                package_version)});
}

ProvidedDependency typed_repository_provider(
    const std::string& package_name,
    const std::string& package_base,
    const std::string& dependency_name,
    const std::string& package_version = "1.0-1") {
    const ProviderCapability capability(
        dependency_name + "=" + package_version,
        dependency_name,
        package_version);
    return ProvidedDependency::from_repository_constraint_metadata(
        "extra", 0, package_name, package_base, "x86_64",
        ProviderConstraintMetadata{
            capability,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                package_version),
            ObservedVersion::available(
                ObservedVersionSource::RepositoryProviderCapability,
                package_version)});
}

BuildPlanDependencyEdge typed_provider_edge(
    const std::string& parent_package_name,
    const std::string& parent_package_base,
    const std::string& dependency_name,
    const ProvidedDependency& provider,
    ProviderResolutionKind resolution = ProviderResolutionKind::Unique) {
    expect(
        provider.constraint_metadata.has_value(),
        "Typed provider fixture has no constraint metadata");
    return BuildPlanDependencyEdge{
        parent_package_name,
        parent_package_base,
        dependency_name,
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        provider,
        resolution,
        exact_requirement(dependency_name),
        ResolvedDependencyCandidate{ProviderResolvedDependencyCandidate{
            provider,
            provider.constraint_metadata->provided_version}},
        ConstraintEvaluation::unconstrained()};
}

const AurUpdateExecutionIssue& require_required_devel_issue(
    const AurUpdateExecutionTarget& target,
    AurUpdateRequiredDevelTargetRelation relation,
    std::optional<std::size_t> dependency_edge_index,
    std::optional<std::size_t> build_plan_order_index) {
    auto found = std::find_if(
        target.issues.begin(), target.issues.end(),
        [relation, dependency_edge_index,
         build_plan_order_index](const AurUpdateExecutionIssue& issue) {
            return issue.reason ==
                       AurUpdateExecutionReason::
                           RequiredDevelTargetRequiresCheck &&
                   issue.required_devel_target_blocker.has_value() &&
                   issue.required_devel_target_blocker->relation == relation &&
                   issue.required_devel_target_blocker
                           ->dependency_edge_index ==
                       dependency_edge_index &&
                   issue.required_devel_target_blocker
                           ->build_plan_order_index ==
                       build_plan_order_index;
        });
    if(found == target.issues.end()) {
        throw std::runtime_error(
            "Required-devel blocker relation is missing");
    }
    return *found;
}

void expect_required_devel_blocker(
    const AurUpdateExecutionTarget& target,
    AurUpdateRequiredDevelTargetRelation relation,
    std::size_t requires_check_update_plan_index,
    std::optional<std::size_t> dependency_edge_index,
    std::optional<std::size_t> build_plan_order_index,
    const std::string& package_name,
    const std::string& package_base,
    const std::vector<PackageRole>& roles,
    const std::vector<RootTargetIdentity>& affected_roots,
    const std::string& context) {
    const AurUpdateExecutionIssue& issue =
        require_required_devel_issue(
            target, relation, dependency_edge_index,
            build_plan_order_index);
    const AurUpdateRequiredDevelTargetBlocker& blocker =
        *issue.required_devel_target_blocker;
    expect(
        issue.devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly} &&
            issue.package_name ==
                std::optional<std::string>{package_name} &&
            issue.package_base ==
                std::optional<std::string>{package_base} &&
            blocker.requires_check_update_plan_index ==
                requires_check_update_plan_index &&
            blocker.dependency_edge_index == dependency_edge_index &&
            blocker.build_plan_order_index == build_plan_order_index &&
            blocker.package_name == package_name &&
            blocker.package_base ==
                std::optional<std::string>{package_base} &&
            blocker.roles == roles &&
            blocker.devel_requires_check_reason ==
                DevelRequiresCheckReason::SuffixCandidateOnly &&
            blocker.affected_roots == affected_roots,
        context + ": required-devel blocker payload differs");
}

void return_build_plan(BuildPlan plan) {
    set_resolver_handler(
        [plan = std::move(plan)](const std::vector<std::string>&) {
            return plan;
        });
}

bool has_issue(
    const AurUpdateExecutionTarget& target,
    AurUpdateExecutionReason reason) {
    return std::any_of(
        target.issues.begin(), target.issues.end(),
        [reason](const AurUpdateExecutionIssue& issue) {
            return issue.reason == reason;
        });
}

std::size_t issue_count(
    const AurUpdateExecutionTarget& target,
    AurUpdateExecutionReason reason) {
    return static_cast<std::size_t>(std::count_if(
        target.issues.begin(), target.issues.end(),
        [reason](const AurUpdateExecutionIssue& issue) {
            return issue.reason == reason;
        }));
}

void expect_status(
    const AurUpdateExecutionTarget& target,
    AurUpdateExecutionTargetStatus expected,
    const std::string& context) {
    expect(target.status == expected, context + ": status differs");
}

void expect_single_resolver_call(
    const std::vector<std::string>& expected_targets,
    const std::string& context) {
    expect(resolver_call_count() == 1, context + ": resolver call count differs");
    expect(
        resolver_calls().front() == expected_targets,
        context + ": resolver target vector differs");
}

void test_classification_order_and_combined_resolution() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry(
            "update-explicit", InstalledPackageReason::Explicit),
        remote_entry(
            "current-package", InstalledPackageReason::Unknown,
            AurUpdateClassification::UpToDate),
        entry_without_remote(
            "foreign-package",
            AurUpdateClassification::NonAurForeign),
        remote_entry(
            "update-dependency", InstalledPackageReason::Dependency),
        entry_without_remote(
            "metadata-failed",
            AurUpdateClassification::MetadataUnavailable),
        remote_entry(
            "version-failed", InstalledPackageReason::Explicit,
            AurUpdateClassification::VersionComparisonUnavailable),
    }};
    return_build_plan(build_plan_for({
        {"update-explicit", "update-explicit", "update-explicit"},
        {"update-dependency", "update-dependency", "update-dependency"},
    }));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);

    expect(
        preflight.devel_requires_check_policy ==
            std::optional<DevelRequiresCheckPolicy>{
                DevelRequiresCheckPolicy::BlockOperation},
        "Combined classification lost the explicit RequiresCheck policy");
    expect(preflight.targets.size() == update_plan.entries.size(), "Target count differs");
    for(std::size_t i = 0; i < preflight.targets.size(); ++i) {
        expect(
            preflight.targets[i].update_plan_index == i,
            "Original update-plan index differs at " + std::to_string(i));
        expect(
            preflight.targets[i].update.installed_name ==
                update_plan.entries[i].installed_name,
            "Target order differs at " + std::to_string(i));
    }
    expect_single_resolver_call(
        {"update-explicit", "update-dependency"},
        "Combined classification plan");
    expect(preflight.build_plan.has_value(), "Combined BuildPlan is missing");

    expect_status(
        preflight.targets[0], AurUpdateExecutionTargetStatus::Executable,
        "Explicit update");
    expect(
        preflight.targets[0].build_plan_root_index ==
            std::optional<std::size_t>{0},
        "First candidate root index differs");
    expect(
        preflight.targets[0].desired_install_reason ==
            std::optional<DesiredInstallReason>{
                DesiredInstallReason::Explicit},
        "Explicit update reason differs");

    expect_status(
        preflight.targets[1], AurUpdateExecutionTargetStatus::Skipped,
        "Up-to-date target");
    expect(
        has_issue(preflight.targets[1], AurUpdateExecutionReason::UpToDate),
        "Up-to-date reason is missing");
    expect(
        preflight.targets[1].skip_kind ==
            std::optional<AurUpdateExecutionSkipKind>{
                AurUpdateExecutionSkipKind::UpToDate},
        "Up-to-date typed skip kind is missing");
    expect(!preflight.targets[1].build_plan_root_index.has_value(), "Skipped target has a root index");

    expect_status(
        preflight.targets[2], AurUpdateExecutionTargetStatus::Skipped,
        "Non-AUR target");
    expect(
        has_issue(
            preflight.targets[2],
            AurUpdateExecutionReason::NonAurForeign),
        "Non-AUR reason is missing");
    expect(
        preflight.targets[2].skip_kind ==
            std::optional<AurUpdateExecutionSkipKind>{
                AurUpdateExecutionSkipKind::NonAurForeign},
        "Non-AUR typed skip kind is missing");

    expect_status(
        preflight.targets[3], AurUpdateExecutionTargetStatus::Executable,
        "Dependency update");
    expect(
        preflight.targets[3].build_plan_root_index ==
            std::optional<std::size_t>{1},
        "Second candidate root index differs");
    expect(
        preflight.targets[3].desired_install_reason ==
            std::optional<DesiredInstallReason>{
                DesiredInstallReason::Dependency},
        "Dependency update reason differs");

    expect_status(
        preflight.targets[4], AurUpdateExecutionTargetStatus::Incomplete,
        "Metadata failure");
    expect(
        has_issue(
            preflight.targets[4],
            AurUpdateExecutionReason::AurMetadataUnavailable),
        "Metadata-unavailable reason is missing");
    expect_status(
        preflight.targets[5], AurUpdateExecutionTargetStatus::Incomplete,
        "Version comparison failure");
    expect(
        has_issue(
            preflight.targets[5],
            AurUpdateExecutionReason::VersionComparisonUnavailable),
        "Version-comparison reason is missing");

    expect(has_executable_targets(preflight), "Executable targets were not detected");
    expect(has_blocking_targets(preflight), "Blocking targets were not detected");
    expect(!can_execute(preflight), "Incomplete invocation was executable");
}

void test_devel_requires_check_blocks_without_candidate_promotion() {
    reset_preflight_stub();
    const AurUpdatePlanEntry requires_check = classify_aur_update(
        AurUpdatePlanInput{
            "manual-check-git",
            "1.0-1",
            InstalledPackageReason::Explicit,
            AurUpdateRemotePackage{
                "manual-check-git",
                "manual-check-git",
                "1.0-1",
                AurVersionRelation::SameAsInstalled}});
    AurUpdatePlan update_plan{{
        remote_entry(
            "normal-update-git", InstalledPackageReason::Explicit),
        requires_check,
    }};
    return_build_plan(build_plan_for({
        {"normal-update-git", "normal-update-git", "normal-update-git"},
    }));

    const AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);

    expect(
        preflight.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::BlockOperation} &&
            !preflight.targets[1].skip_kind.has_value(),
        "RequiresCheck BlockOperation policy or non-skip shape was lost");
    expect_single_resolver_call(
        {"normal-update-git"},
        "RequiresCheck candidate firewall");
    expect_status(
        preflight.targets[0],
        AurUpdateExecutionTargetStatus::Executable,
        "Normal update precedence");
    expect_status(
        preflight.targets[1],
        AurUpdateExecutionTargetStatus::Incomplete,
        "Devel RequiresCheck target");
    expect(
        has_issue(
            preflight.targets[1],
            AurUpdateExecutionReason::DevelRequiresCheck),
        "Devel RequiresCheck reason is missing");
    expect(
        preflight.targets[1].issues.size() == 1 &&
            preflight.targets[1]
                    .issues.front()
                    .devel_requires_check_reason ==
                DevelRequiresCheckReason::SuffixCandidateOnly &&
            preflight.targets[1]
                    .issues.front()
                    .package_base ==
                std::optional<std::string>{"manual-check-git"},
        "Devel RequiresCheck reason or PackageBase was flattened");
    expect(
        has_executable_targets(preflight) &&
            has_blocking_targets(preflight) &&
            !can_execute(preflight),
        "Mixed RequiresCheck invocation bypassed all-target preflight");
}

void test_skip_independent_target_keeps_full_identity_and_update_precedence() {
    reset_preflight_stub();
    const AurUpdatePlanEntry update =
        classified_update_entry("normal-update-git");
    const AurUpdatePlanEntry requires_check = requires_check_entry(
        "independent-check-git", "independent-check-base");
    const AurUpdatePlanEntry current = remote_entry(
        "current-package", InstalledPackageReason::Explicit,
        AurUpdateClassification::UpToDate);
    const AurUpdatePlan update_plan{{update, requires_check, current}};
    return_build_plan(build_plan_for({
        {"normal-update-git", "normal-update-git", "normal-update-git"},
    }));

    const AurUpdateExecutionPreflight preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);

    expect_single_resolver_call(
        {"normal-update-git"},
        "SkipIndependentTarget planner subset");
    expect(
        preflight.targets.size() == 3 &&
            preflight.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::SkipIndependentTarget},
        "SkipIndependentTarget lost target order or policy");
    expect_status(
        preflight.targets[0], AurUpdateExecutionTargetStatus::Executable,
        "UpdateAvailable devel target precedence");
    expect(
        preflight.targets[0].update.devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck &&
            project_aur_update_effective_state(
                preflight.targets[0].update) ==
                AurUpdateEffectiveState::UpdateAvailable,
        "UpdateAvailable devel assessment was reclassified as a policy skip");

    const AurUpdateExecutionTarget& independent = preflight.targets[1];
    expect_status(
        independent, AurUpdateExecutionTargetStatus::Skipped,
        "Independent RequiresCheck target");
    expect(
        independent.update_plan_index == 1 &&
            independent.update == requires_check &&
            !independent.build_plan_root_index.has_value() &&
            !independent.desired_install_reason.has_value() &&
            independent.skip_kind ==
                std::optional<AurUpdateExecutionSkipKind>{
                    AurUpdateExecutionSkipKind::
                        IndependentDevelRequiresCheck} &&
            independent.issues.size() == 1 &&
            independent.issues.front().reason ==
                AurUpdateExecutionReason::DevelRequiresCheck &&
            independent.issues.front().devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly},
        "Independent RequiresCheck target lost its exact skip identity");
    expect_status(
        preflight.targets[2], AurUpdateExecutionTargetStatus::Skipped,
        "Normal UpToDate target");
    expect(
        preflight.targets[2].skip_kind ==
                std::optional<AurUpdateExecutionSkipKind>{
                    AurUpdateExecutionSkipKind::UpToDate} &&
            has_valid_aur_update_execution_policy_snapshot(preflight) &&
            has_executable_targets(preflight) &&
            !has_blocking_targets(preflight) && can_execute(preflight),
        "Independent RequiresCheck became a blocker or malformed skip");

    reset_preflight_stub();
    const AurUpdateExecutionPreflight skip_only =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{requires_check}},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        resolver_call_count() == 0 && skip_only.targets.size() == 1 &&
            skip_only.targets.front().status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            skip_only.targets.front().skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            project_aur_update_effective_state(
                skip_only.targets.front().update) ==
                AurUpdateEffectiveState::RequiresCheck &&
            !has_executable_targets(skip_only) &&
            !has_blocking_targets(skip_only) && !can_execute(skip_only),
        "RequiresCheck-only capability was resolved, blocked, or flattened");
}

void test_required_devel_exact_dependency_is_root_local() {
    reset_preflight_stub();
    const RootTargetIdentity affected_root{0, "affected-root"};
    const RootTargetIdentity clean_root{1, "clean-root"};
    BuildPlan plan = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    add_required_package_target(
        plan, "required-devel-git", "required-devel-base",
        {affected_root});
    plan.dependency_edges.push_back(typed_aur_exact_edge(
        "affected-root", "affected-root", "required-devel-git",
        "required-devel-base"));
    return_build_plan(std::move(plan));

    const AurUpdateExecutionPreflight preflight =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "affected-root", InstalledPackageReason::Explicit),
                requires_check_entry(
                    "required-devel-git", "required-devel-base"),
                remote_entry(
                    "clean-root", InstalledPackageReason::Explicit),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);

    expect_single_resolver_call(
        {"affected-root", "clean-root"},
        "Required-devel exact dependency subset");
    expect_status(
        preflight.targets[0], AurUpdateExecutionTargetStatus::Incomplete,
        "Required-devel affected root");
    expect_status(
        preflight.targets[1], AurUpdateExecutionTargetStatus::Skipped,
        "Required-devel skipped target");
    expect_status(
        preflight.targets[2], AurUpdateExecutionTargetStatus::Executable,
        "Required-devel unrelated root");
    expect(
        preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck &&
            !has_issue(
                preflight.targets[2],
                AurUpdateExecutionReason::
                    RequiredDevelTargetRequiresCheck),
        "Required-devel blocker leaked to the skipped or unrelated target");
    expect_required_devel_blocker(
        preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1, 0, std::nullopt, "required-devel-git", "required-devel-base",
        {PackageRole::RuntimeDependency}, {affected_root},
        "AUR exact dependency");
    expect(
        !can_execute(preflight) && has_executable_targets(preflight) &&
            has_blocking_targets(preflight) &&
            has_complete_aur_update_required_devel_relation_snapshot(
                preflight),
        "Root-local blocker bypassed the current global execution barrier");
}

void test_required_devel_relation_snapshot_complete_equality() {
    reset_preflight_stub();
    const RootTargetIdentity affected_root{0, "complete-root"};
    const RootTargetIdentity unrelated_root{1, "complete-unrelated"};
    BuildPlan plan = build_plan_for({
        {"complete-root", "complete-root", "complete-root"},
        {"complete-unrelated", "complete-unrelated",
         "complete-unrelated"},
    });
    add_required_package_target(
        plan, "complete-required-git", "complete-required-base",
        {affected_root});
    plan.dependency_edges.push_back(typed_aur_exact_edge(
        "complete-root", "complete-root", "complete-required-git",
        "complete-required-base"));
    return_build_plan(std::move(plan));

    const AurUpdateExecutionPreflight baseline =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "complete-root", InstalledPackageReason::Explicit),
                requires_check_entry(
                    "complete-required-git", "complete-required-base"),
                remote_entry(
                    "complete-unrelated",
                    InstalledPackageReason::Explicit),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        has_complete_aur_update_required_devel_relation_snapshot(
            baseline),
        "Coherent required-devel relation set was rejected");

    const auto required_issue = [](
                                    AurUpdateExecutionTarget& target,
                                    AurUpdateRequiredDevelTargetRelation
                                        relation)
        -> AurUpdateExecutionIssue& {
        const auto found = std::find_if(
            target.issues.begin(), target.issues.end(),
            [relation](const AurUpdateExecutionIssue& issue) {
                return issue.required_devel_target_blocker.has_value() &&
                       issue.required_devel_target_blocker->relation ==
                           relation;
            });
        if(found == target.issues.end()) {
            throw std::runtime_error(
                "Required-devel completeness fixture lost a blocker");
        }
        return *found;
    };
    const auto expect_incomplete = [](
                                       const AurUpdateExecutionPreflight&
                                           preflight,
                                       const std::string& context) {
        expect(
            !has_complete_aur_update_required_devel_relation_snapshot(
                preflight),
            context + ": malformed relation set was accepted");
    };

    AurUpdateExecutionPreflight missing = baseline;
    std::erase_if(
        missing.targets[0].issues,
        [](const AurUpdateExecutionIssue& issue) {
            return issue.reason == AurUpdateExecutionReason::
                                       RequiredDevelTargetRequiresCheck;
        });
    missing.targets[0].status =
        AurUpdateExecutionTargetStatus::Executable;
    missing.targets[1].skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
    expect_incomplete(missing, "Missing blockers");

    AurUpdateExecutionPreflight extra = baseline;
    AurUpdateExecutionIssue fabricated = required_issue(
        extra.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency);
    fabricated.required_devel_target_blocker->relation =
        AurUpdateRequiredDevelTargetRelation::RepositoryExactDependency;
    extra.targets[0].issues.push_back(std::move(fabricated));
    expect_incomplete(extra, "Extra blocker");

    AurUpdateExecutionPreflight wrong_relation = baseline;
    required_issue(
        wrong_relation.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency)
        .required_devel_target_blocker->relation =
        AurUpdateRequiredDevelTargetRelation::RepositoryExactDependency;
    expect_incomplete(wrong_relation, "Wrong relation");

    AurUpdateExecutionPreflight wrong_roots = baseline;
    required_issue(
        wrong_roots.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency)
        .required_devel_target_blocker->affected_roots =
        {affected_root, unrelated_root};
    expect_incomplete(wrong_roots, "Wrong affected roots");

    AurUpdateExecutionPreflight duplicate = baseline;
    duplicate.targets[0].issues.push_back(required_issue(
        duplicate.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency));
    expect_incomplete(duplicate, "Duplicate blocker");

    AurUpdateExecutionPreflight independent_contradiction = baseline;
    independent_contradiction.targets[1].skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
    expect_incomplete(
        independent_contradiction,
        "Required relation with Independent kind");

    reset_preflight_stub();
    const AurUpdateExecutionPreflight independent =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{requires_check_entry(
                "complete-independent-git",
                "complete-independent-base")}},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        has_complete_aur_update_required_devel_relation_snapshot(
            independent),
        "Independent baseline relation set was rejected");
    AurUpdateExecutionPreflight required_contradiction = independent;
    required_contradiction.targets[0].skip_kind =
        AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck;
    expect_incomplete(
        required_contradiction,
        "Independent target with Required kind");

    reset_preflight_stub();
    return_build_plan(build_plan_for({
        {"no-reentry-root", "no-reentry-root", "no-reentry-root"},
    }));
    AurUpdateExecutionPreflight fabricated_without_reentry =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "no-reentry-root",
                    InstalledPackageReason::Explicit),
                requires_check_entry(
                    "no-reentry-git", "no-reentry-base"),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        has_complete_aur_update_required_devel_relation_snapshot(
            fabricated_without_reentry),
        "No-reentry baseline relation set was rejected");
    AurUpdateExecutionPreflight combined_installed_drift =
        fabricated_without_reentry;
    combined_installed_drift.build_plan->dependency_edges.push_back(
        BuildPlanDependencyEdge{
            "no-reentry-root",
            "no-reentry-root",
            "no-reentry-git",
            PackageRole::RuntimeDependency,
            DependencyKind::Installed,
            "different-installed-child",
            std::nullopt,
            std::nullopt,
            ProviderResolutionKind::Unique,
            DependencyRequirement{ConsumerDependencyRequirement(
                "no-reentry-git", "no-reentry-git", std::nullopt)},
            ResolvedDependencyCandidate{InstalledExactPackage{
                "different-installed-child",
                ObservedVersion::available(
                    ObservedVersionSource::InstalledExactPackage,
                    "1.0-1")}},
            ConstraintEvaluation::unconstrained()});
    expect_incomplete(
        combined_installed_drift,
        "Combined InstalledExact discovery drift");

    fabricated_without_reentry.targets[0].status =
        AurUpdateExecutionTargetStatus::Incomplete;
    AurUpdateRequiredDevelTargetBlocker fabricated_blocker{
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1,
        "no-reentry-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    fabricated_blocker.dependency_edge_index = 0;
    fabricated_blocker.package_base = "no-reentry-base";
    fabricated_blocker.roles = {PackageRole::RuntimeDependency};
    fabricated_blocker.affected_roots = {{0, "no-reentry-root"}};
    AurUpdateExecutionIssue fabricated_issue{
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck,
        "no-reentry-git",
        "no-reentry-base",
        "no-reentry-git",
        "Fabricated required-devel blocker without BuildPlan re-entry."};
    fabricated_issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    fabricated_issue.required_devel_target_blocker =
        std::move(fabricated_blocker);
    fabricated_without_reentry.targets[0].issues.push_back(
        std::move(fabricated_issue));
    fabricated_without_reentry.targets[1].skip_kind =
        AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck;
    expect_incomplete(
        fabricated_without_reentry,
        "Fabricated blocker without BuildPlan re-entry");
}

void test_required_devel_provider_and_repository_reentry_are_source_aware() {
    const AurUpdatePlan update_plan{{
        remote_entry("provider-root", InstalledPackageReason::Explicit),
        requires_check_entry(
            "same-name-devel-git", "same-name-devel-base"),
    }};
    const RootTargetIdentity root{0, "provider-root"};

    reset_preflight_stub();
    BuildPlan aur_provider_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    add_required_package_target(
        aur_provider_plan, "same-name-devel-git",
        "same-name-devel-base", {root});
    const ProvidedDependency aur_provider = typed_aur_provider(
        "same-name-devel-git", "same-name-devel-base",
        "virtual-devel-api");
    aur_provider_plan.dependency_edges.push_back(typed_provider_edge(
        "provider-root", "provider-root", "virtual-devel-api",
        aur_provider));
    aur_provider_plan.provided.push_back(BuildPlanProvidedDependency{
        "virtual-devel-api", aur_provider,
        ProviderResolutionKind::Unique});
    return_build_plan(std::move(aur_provider_plan));

    const AurUpdateExecutionPreflight aur_provider_preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        aur_provider_preflight.targets[0],
        AurUpdateExecutionTargetStatus::Incomplete,
        "AUR provider required-devel root");
    expect_status(
        aur_provider_preflight.targets[1],
        AurUpdateExecutionTargetStatus::Skipped,
        "AUR provider RequiresCheck target");
    expect_required_devel_blocker(
        aur_provider_preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurProvider,
        1, 0, std::nullopt, "same-name-devel-git", "same-name-devel-base",
        {PackageRole::RuntimeDependency}, {root}, "AUR provider");
    const BuildPlanDependencyEdge& retained_aur_edge =
        aur_provider_preflight.build_plan->dependency_edges.front();
    expect(
        retained_aur_edge.requirement.has_value() &&
            retained_aur_edge.resolved_candidate.has_value() &&
            std::holds_alternative<ProviderResolvedDependencyCandidate>(
                *retained_aur_edge.resolved_candidate) &&
            std::holds_alternative<AurProviderOrigin>(
                retained_aur_edge.resolved_provider->origin),
        "AUR provider test lost typed requirement or source identity");

    reset_preflight_stub();
    BuildPlan repository_exact_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    repository_exact_plan.configured_repository_order =
        std::vector<std::string>{"extra"};
    repository_exact_plan.dependency_edges.push_back(
        typed_repository_exact_edge(
            "provider-root", "provider-root", "same-name-devel-git",
            "same-name-devel-base"));
    return_build_plan(std::move(repository_exact_plan));

    const AurUpdateExecutionPreflight repository_exact_preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        repository_exact_preflight.targets[0],
        AurUpdateExecutionTargetStatus::Incomplete,
        "Repository exact required-devel root");
    expect_required_devel_blocker(
        repository_exact_preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::RepositoryExactDependency,
        1, 0, std::nullopt, "same-name-devel-git",
        "same-name-devel-base", {PackageRole::RuntimeDependency}, {root},
        "Repository exact dependency");
    const auto* repository_candidate = std::get_if<RepositoryExactPackage>(
        &*repository_exact_preflight.build_plan
              ->dependency_edges.front()
              .resolved_candidate);
    expect(
        repository_candidate != nullptr &&
            repository_candidate->repository ==
                ConfiguredRepositoryIdentity{"extra", 0} &&
            repository_candidate->package_name ==
                "same-name-devel-git" &&
            repository_candidate->package_base ==
                "same-name-devel-base" &&
            repository_candidate->package_version.version() != nullptr &&
            *repository_candidate->package_version.version() == "1.0-1",
        "Repository exact same-name re-entry lost source/version identity");

    reset_preflight_stub();
    BuildPlan repository_provider_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    repository_provider_plan.configured_repository_order =
        std::vector<std::string>{"extra"};
    const ProvidedDependency repository_provider =
        typed_repository_provider(
            "same-name-devel-git", "same-name-devel-base",
            "virtual-devel-api");
    repository_provider_plan.dependency_edges.push_back(
        typed_provider_edge(
            "provider-root", "provider-root", "virtual-devel-api",
            repository_provider,
            ProviderResolutionKind::UserSelected));
    repository_provider_plan.provided.push_back(
        BuildPlanProvidedDependency{
            "virtual-devel-api", repository_provider,
            ProviderResolutionKind::UserSelected});
    return_build_plan(std::move(repository_provider_plan));

    const AurUpdateExecutionPreflight repository_provider_preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        repository_provider_preflight.targets[0],
        AurUpdateExecutionTargetStatus::Incomplete,
        "Repository provider required-devel root");
    expect_required_devel_blocker(
        repository_provider_preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::RepositoryProvider,
        1, 0, std::nullopt, "same-name-devel-git",
        "same-name-devel-base", {PackageRole::RuntimeDependency}, {root},
        "Repository provider");
    const ProvidedDependency& retained_repository_provider =
        *repository_provider_preflight.build_plan
             ->dependency_edges.front()
             .resolved_provider;
    const auto* repository_origin = std::get_if<RepositoryProviderOrigin>(
        &retained_repository_provider.origin);
    expect(
        repository_origin != nullptr &&
            repository_origin->repository_name == "extra" &&
            repository_origin->configured_order ==
                std::optional<std::size_t>{0} &&
            retained_repository_provider.package_name ==
                "same-name-devel-git" &&
            retained_repository_provider.package_base ==
                "same-name-devel-base",
        "Repository provider same-name re-entry was compared by name only");

    reset_preflight_stub();
    BuildPlan ambiguous_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    ambiguous_plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "provider-root",
        "provider-root",
        "ambiguous-devel-api",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        exact_requirement("ambiguous-devel-api"),
        std::nullopt,
        std::nullopt});
    ambiguous_plan.ambiguous_providers.push_back(
        AmbiguousProvidedDependency{
            "ambiguous-devel-api",
            {
                typed_aur_provider(
                    "same-name-devel-git", "same-name-devel-base",
                    "ambiguous-devel-api"),
                typed_repository_provider(
                    "same-name-devel-git", "same-name-devel-base",
                    "ambiguous-devel-api"),
            }});
    return_build_plan(std::move(ambiguous_plan));

    const AurUpdateExecutionPreflight ambiguous_preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        ambiguous_preflight.targets[0],
        AurUpdateExecutionTargetStatus::Unsupported,
        "Ambiguous RequiresCheck provider root");
    expect(
        has_issue(
            ambiguous_preflight.targets[0],
            AurUpdateExecutionReason::AmbiguousProvider) &&
            !has_issue(
                ambiguous_preflight.targets[0],
                AurUpdateExecutionReason::
                    RequiredDevelTargetRequiresCheck) &&
            ambiguous_preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped,
        "Ambiguous RequiresCheck provider was auto-selected or lost its blocker");

    reset_preflight_stub();
    BuildPlan installed_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    installed_plan.dependency_edges.push_back(
        typed_installed_exact_edge(
            "provider-root", "provider-root",
            "same-name-devel-git"));
    return_build_plan(std::move(installed_plan));
    const AurUpdateExecutionPreflight installed_preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        installed_preflight.targets[0].status ==
                AurUpdateExecutionTargetStatus::Executable &&
            installed_preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            installed_preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            !has_issue(
                installed_preflight.targets[0],
                AurUpdateExecutionReason::
                    RequiredDevelTargetRequiresCheck),
        "Exact installed satisfaction was treated as a mutation re-entry");

    reset_preflight_stub();
    BuildPlan forged_constraint_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    BuildPlanDependencyEdge forged_constraint_edge =
        typed_installed_exact_edge(
            "provider-root", "provider-root",
            "same-name-devel-git");
    forged_constraint_edge.dependency_spec =
        "same-name-devel-git>=2";
    forged_constraint_edge.requirement = DependencyRequirement{
        ConsumerDependencyRequirement(
            "same-name-devel-git>=2", "same-name-devel-git",
            DependencyVersionConstraint(
                DependencyVersionRelation::GreaterThanOrEqual,
                "2"))};
    forged_constraint_edge.constraint_evaluation =
        ConstraintEvaluation::unconstrained();
    forged_constraint_plan.dependency_edges.push_back(
        std::move(forged_constraint_edge));
    return_build_plan(std::move(forged_constraint_plan));

    const AurUpdateExecutionPreflight forged_constraint_preflight =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        forged_constraint_preflight.targets[0].status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            forged_constraint_preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            forged_constraint_preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck,
        "Forged InstalledExact constraint remained independent");
    expect_required_devel_blocker(
        forged_constraint_preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::IdentityDrift,
        1, 0, std::nullopt, "same-name-devel-git",
        "same-name-devel-base", {PackageRole::RuntimeDependency}, {root},
        "Forged InstalledExact constraint");

    reset_preflight_stub();
    BuildPlan combined_identity_drift_plan = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    BuildPlanDependencyEdge combined_identity_drift_edge =
        typed_installed_exact_edge(
            "provider-root", "provider-root",
            "same-name-devel-git");
    combined_identity_drift_edge.resolved_package_name =
        "different-installed-child";
    std::get<InstalledExactPackage>(
        *combined_identity_drift_edge.resolved_candidate)
        .package_name = "different-installed-child";
    combined_identity_drift_plan.dependency_edges.push_back(
        std::move(combined_identity_drift_edge));
    return_build_plan(std::move(combined_identity_drift_plan));

    const AurUpdateExecutionPreflight combined_identity_drift =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        combined_identity_drift.targets[0].status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            combined_identity_drift.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            combined_identity_drift.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck,
        "Combined InstalledExact identity drift was not required");
    expect_required_devel_blocker(
        combined_identity_drift.targets[0],
        AurUpdateRequiredDevelTargetRelation::IdentityDrift,
        1, 0, std::nullopt, "same-name-devel-git",
        "same-name-devel-base", {PackageRole::RuntimeDependency}, {root},
        "Combined InstalledExact identity drift");
}

void test_required_devel_package_base_uses_exact_required_children() {
    const AurUpdatePlan update_plan{{
        remote_entry(
            "shared-root", InstalledPackageReason::Explicit,
            AurUpdateClassification::UpdateAvailable, "shared-root",
            "shared-suite"),
        requires_check_entry(
            "shared-devel-git", "shared-suite"),
    }};
    const RootTargetIdentity root{0, "shared-root"};

    reset_preflight_stub();
    return_build_plan(build_plan_for({
        {"shared-root", "shared-root", "shared-suite"},
    }));
    const AurUpdateExecutionPreflight unselected_sibling =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        unselected_sibling.targets[0],
        AurUpdateExecutionTargetStatus::Executable,
        "Unselected same-PackageBase sibling root");
    expect_status(
        unselected_sibling.targets[1],
        AurUpdateExecutionTargetStatus::Skipped,
        "Unselected same-PackageBase RequiresCheck sibling");
    expect(
        !has_issue(
            unselected_sibling.targets[0],
            AurUpdateExecutionReason::
                RequiredDevelTargetRequiresCheck) &&
            unselected_sibling.build_plan->order.size() == 1 &&
            unselected_sibling.build_plan
                    ->order.front()
                    .package_names ==
                std::vector<std::string>{"shared-root"},
        "PackageBase equality alone blocked an unselected sibling");

    reset_preflight_stub();
    BuildPlan required_child_plan = build_plan_for({
        {"shared-root", "shared-root", "shared-suite"},
    });
    add_required_package_target(
        required_child_plan, "shared-devel-git", "shared-suite",
        {root});
    required_child_plan.dependency_edges.push_back(
        typed_aur_exact_edge(
            "shared-root", "shared-suite", "shared-devel-git",
            "shared-suite"));
    return_build_plan(std::move(required_child_plan));

    const AurUpdateExecutionPreflight required_sibling =
        ::resolve_aur_update_execution_preflight(
            update_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        required_sibling.targets[0],
        AurUpdateExecutionTargetStatus::Incomplete,
        "Required same-PackageBase sibling root");
    expect_status(
        required_sibling.targets[1],
        AurUpdateExecutionTargetStatus::Skipped,
        "Required same-PackageBase RequiresCheck sibling");
    expect(
        required_sibling.build_plan->order.size() == 1 &&
            required_sibling.build_plan
                    ->order.front()
                    .package_names ==
                std::vector<std::string>{
                    "shared-root", "shared-devel-git"},
        "Same-PackageBase required child set differs");
    expect_required_devel_blocker(
        required_sibling.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1, 0, std::nullopt, "shared-devel-git", "shared-suite",
        {PackageRole::RuntimeDependency}, {root},
        "Same-PackageBase exact dependency");
    expect_required_devel_blocker(
        required_sibling.targets[0],
        AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild,
        1, std::nullopt, 0, "shared-devel-git", "shared-suite",
        {PackageRole::RuntimeDependency}, {root},
        "Same-PackageBase required artifact child");
}

void test_required_devel_shared_dependency_preserves_edge_roots() {
    reset_preflight_stub();
    const RootTargetIdentity first_root{0, "shared-first-root"};
    const RootTargetIdentity second_root{1, "shared-second-root"};
    BuildPlan plan = build_plan_for({
        {"shared-first-root", "shared-first-root", "shared-first-root"},
        {"shared-second-root", "shared-second-root", "shared-second-root"},
    });
    add_required_package_target(
        plan, "shared-required-git", "shared-required-base",
        {first_root, second_root});
    plan.dependency_edges.push_back(typed_aur_exact_edge(
        "shared-first-root", "shared-first-root",
        "shared-required-git", "shared-required-base"));
    plan.dependency_edges.push_back(typed_aur_exact_edge(
        "shared-second-root", "shared-second-root",
        "shared-required-git", "shared-required-base"));
    return_build_plan(std::move(plan));

    const AurUpdateExecutionPreflight preflight =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "shared-first-root",
                    InstalledPackageReason::Explicit),
                requires_check_entry(
                    "shared-required-git", "shared-required-base"),
                remote_entry(
                    "shared-second-root",
                    InstalledPackageReason::Explicit),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);

    expect(
        preflight.targets[0].status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck &&
            preflight.targets[2].status ==
                AurUpdateExecutionTargetStatus::Incomplete,
        "Shared required dependency did not retain localized target states");
    expect_required_devel_blocker(
        preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1, 0, std::nullopt, "shared-required-git",
        "shared-required-base", {PackageRole::RuntimeDependency},
        {first_root}, "Shared dependency first edge");
    expect_required_devel_blocker(
        preflight.targets[2],
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1, 1, std::nullopt, "shared-required-git",
        "shared-required-base", {PackageRole::RuntimeDependency},
        {second_root}, "Shared dependency second edge");
    expect_required_devel_blocker(
        preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild,
        1, std::nullopt, 0, "shared-required-git",
        "shared-required-base", {PackageRole::RuntimeDependency},
        {first_root, second_root}, "Shared dependency artifact first root");
    expect_required_devel_blocker(
        preflight.targets[2],
        AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild,
        1, std::nullopt, 0, "shared-required-git",
        "shared-required-base", {PackageRole::RuntimeDependency},
        {first_root, second_root}, "Shared dependency artifact second root");
}

void test_required_devel_identity_drift_and_unattributed_state_fail_closed() {
    const AurUpdatePlan drift_plan{{
        remote_entry("drift-root", InstalledPackageReason::Explicit),
        requires_check_entry(
            "drift-devel-git", "expected-devel-base"),
    }};
    const RootTargetIdentity drift_root{0, "drift-root"};

    auto expect_drift = [&](BuildPlan plan,
                            bool artifact_identity_is_drifted,
                            const std::string& context) {
        reset_preflight_stub();
        return_build_plan(std::move(plan));
        const AurUpdateExecutionPreflight preflight =
            ::resolve_aur_update_execution_preflight(
                drift_plan,
                DevelRequiresCheckPolicy::SkipIndependentTarget);
        expect_status(
            preflight.targets[0],
            AurUpdateExecutionTargetStatus::Incomplete,
            context + " root");
        expect_status(
            preflight.targets[1],
            AurUpdateExecutionTargetStatus::Skipped,
            context + " RequiresCheck target");
        expect_required_devel_blocker(
            preflight.targets[0],
            AurUpdateRequiredDevelTargetRelation::IdentityDrift,
            1, 0, std::nullopt, "drift-devel-git", "expected-devel-base",
            {PackageRole::RuntimeDependency}, {drift_root}, context);
        expect_required_devel_blocker(
            preflight.targets[0],
            artifact_identity_is_drifted
                ? AurUpdateRequiredDevelTargetRelation::IdentityDrift
                : AurUpdateRequiredDevelTargetRelation::
                      RequiredArtifactChild,
            1, std::nullopt, 0, "drift-devel-git",
            "expected-devel-base", {PackageRole::RuntimeDependency},
            {drift_root}, context + " artifact");
        expect(
            !has_issue(
                preflight.targets[0],
                AurUpdateExecutionReason::DevelRequiresCheck) &&
                preflight.targets[1].skip_kind ==
                    AurUpdateExecutionSkipKind::
                        RequiredDevelRequiresCheck,
            context + ": drift promoted the RequiresCheck target");
    };

    BuildPlan base_drift = build_plan_for({
        {"drift-root", "drift-root", "drift-root"},
    });
    add_required_package_target(
        base_drift, "drift-devel-git", "observed-devel-base",
        {drift_root});
    base_drift.dependency_edges.push_back(typed_aur_exact_edge(
        "drift-root", "drift-root", "drift-devel-git",
        "observed-devel-base"));
    expect_drift(
        std::move(base_drift), true, "PackageBase drift");

    BuildPlan version_drift = build_plan_for({
        {"drift-root", "drift-root", "drift-root"},
    });
    add_required_package_target(
        version_drift, "drift-devel-git", "expected-devel-base",
        {drift_root});
    version_drift.dependency_edges.push_back(typed_aur_exact_edge(
        "drift-root", "drift-root", "drift-devel-git",
        "expected-devel-base", PackageRole::RuntimeDependency,
        "9.0-1"));
    expect_drift(
        std::move(version_drift), false, "Remote version drift");

    BuildPlan child_drift = build_plan_for({
        {"drift-root", "drift-root", "drift-root"},
    });
    add_required_package_target(
        child_drift, "drift-devel-git", "expected-devel-base",
        {drift_root});
    BuildPlanDependencyEdge child_drift_edge = typed_aur_exact_edge(
        "drift-root", "drift-root", "drift-devel-git",
        "expected-devel-base");
    std::get<AurResolvedDependencyCandidate>(
        *child_drift_edge.resolved_candidate)
        .package_name = "different-devel-child";
    child_drift.dependency_edges.push_back(
        std::move(child_drift_edge));
    expect_drift(
        std::move(child_drift), false, "Resolved child drift");

    BuildPlan source_drift = build_plan_for({
        {"drift-root", "drift-root", "drift-root"},
    });
    source_drift.configured_repository_order =
        std::vector<std::string>{"extra"};
    add_required_package_target(
        source_drift, "drift-devel-git", "expected-devel-base",
        {drift_root});
    BuildPlanDependencyEdge source_drift_edge = typed_aur_exact_edge(
        "drift-root", "drift-root", "drift-devel-git",
        "expected-devel-base");
    source_drift_edge.resolved_candidate = RepositoryExactPackage{
        ConfiguredRepositoryIdentity{"extra", 0},
        "drift-devel-git",
        "expected-devel-base",
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage, "1.0-1"),
        {},
        std::optional<std::string>{"x86_64"}};
    source_drift.dependency_edges.push_back(
        std::move(source_drift_edge));
    expect_drift(
        std::move(source_drift), false, "Resolved source drift");

    BuildPlan provider_source_drift = build_plan_for({
        {"drift-root", "drift-root", "drift-root"},
    });
    add_required_package_target(
        provider_source_drift, "drift-devel-git",
        "expected-devel-base", {drift_root});
    ProvidedDependency drifted_provider = typed_aur_provider(
        "drift-devel-git", "expected-devel-base",
        "drift-virtual-api");
    drifted_provider.constraint_metadata->provided_version =
        ObservedVersion::available(
            ObservedVersionSource::LocalProviderCapability,
            "1.0-1");
    BuildPlanDependencyEdge drifted_provider_edge =
        typed_provider_edge(
            "drift-root", "drift-root", "drift-virtual-api",
            drifted_provider);
    std::get<ProviderResolvedDependencyCandidate>(
        *drifted_provider_edge.resolved_candidate)
        .provided_version =
        drifted_provider.constraint_metadata->provided_version;
    provider_source_drift.dependency_edges.push_back(
        std::move(drifted_provider_edge));
    expect_drift(
        std::move(provider_source_drift), false,
        "Provider capability source drift");

    reset_preflight_stub();
    BuildPlan installed_source_drift = build_plan_for({
        {"drift-root", "drift-root", "drift-root"},
    });
    BuildPlanDependencyEdge installed_drift_edge =
        typed_installed_exact_edge(
            "drift-root", "drift-root", "drift-devel-git");
    installed_drift_edge.resolved_candidate =
        AurResolvedDependencyCandidate{
            "drift-devel-git", "expected-devel-base",
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                "1.0-1")};
    installed_source_drift.dependency_edges.push_back(
        std::move(installed_drift_edge));
    return_build_plan(std::move(installed_source_drift));
    const AurUpdateExecutionPreflight installed_drift_preflight =
        ::resolve_aur_update_execution_preflight(
            drift_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(
        installed_drift_preflight.targets[0].status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            installed_drift_preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            installed_drift_preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck,
        "Installed source drift was treated as exact installed satisfaction");
    expect_required_devel_blocker(
        installed_drift_preflight.targets[0],
        AurUpdateRequiredDevelTargetRelation::IdentityDrift,
        1, 0, std::nullopt, "drift-devel-git",
        "expected-devel-base", {PackageRole::RuntimeDependency},
        {drift_root}, "Installed source drift");

    reset_preflight_stub();
    BuildPlan unattributed = build_plan_for({
        {"unattributed-root-a", "unattributed-root-a",
         "unattributed-root-a"},
        {"unattributed-root-c", "unattributed-root-c",
         "unattributed-root-c"},
    });
    add_required_package_target(
        unattributed, "unattributed-devel-git",
        "unattributed-devel-base", {});
    unattributed.dependency_edges.push_back(typed_aur_exact_edge(
        "orphan-parent", "orphan-base", "unattributed-devel-git",
        "unattributed-devel-base"));
    return_build_plan(std::move(unattributed));

    const AurUpdateExecutionPreflight unattributed_preflight =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "unattributed-root-a",
                    InstalledPackageReason::Explicit),
                requires_check_entry(
                    "unattributed-devel-git",
                    "unattributed-devel-base"),
                remote_entry(
                    "unattributed-root-c",
                    InstalledPackageReason::Explicit),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect_status(
        unattributed_preflight.targets[0],
        AurUpdateExecutionTargetStatus::Incomplete,
        "Unattributed first root");
    expect_status(
        unattributed_preflight.targets[1],
        AurUpdateExecutionTargetStatus::Skipped,
        "Unattributed RequiresCheck target");
    expect_status(
        unattributed_preflight.targets[2],
        AurUpdateExecutionTargetStatus::Incomplete,
        "Unattributed second root");
    for(const std::size_t target_index : {std::size_t{0}, std::size_t{2}}) {
        expect_required_devel_blocker(
            unattributed_preflight.targets[target_index],
            AurUpdateRequiredDevelTargetRelation::IdentityDrift,
            1, 0, std::nullopt, "unattributed-devel-git",
            "unattributed-devel-base",
            {PackageRole::RuntimeDependency}, {},
            "Unattributed edge fallback");
        expect_required_devel_blocker(
            unattributed_preflight.targets[target_index],
            AurUpdateRequiredDevelTargetRelation::IdentityDrift,
            1, std::nullopt, 0, "unattributed-devel-git",
            "unattributed-devel-base",
            {PackageRole::RuntimeDependency}, {},
            "Unattributed artifact fallback");
        expect(
            has_issue(
                unattributed_preflight.targets[target_index],
                AurUpdateExecutionReason::BuildPlanInconsistent),
            "Unattributed required-devel state did not fail closed globally");
    }
    expect(
        unattributed_preflight.targets[1].skip_kind ==
            AurUpdateExecutionSkipKind::
                RequiredDevelRequiresCheck,
        "Unattributed fallback changed the skipped target identity");
}

void test_duplicate_requires_check_identity_is_not_independent() {
    reset_preflight_stub();
    return_build_plan(build_plan_for({
        {"duplicate-check-root", "duplicate-check-root",
         "duplicate-check-root"},
    }));
    const AurUpdatePlanEntry duplicate = requires_check_entry(
        "duplicate-check-git", "duplicate-check-base");
    const AurUpdateExecutionPreflight preflight =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "duplicate-check-root",
                    InstalledPackageReason::Explicit),
                duplicate,
                duplicate,
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);

    expect_single_resolver_call(
        {"duplicate-check-root"},
        "Duplicate RequiresCheck identity");
    expect_status(
        preflight.targets[0], AurUpdateExecutionTargetStatus::Executable,
        "Duplicate RequiresCheck executable root");
    for(const std::size_t index : {std::size_t{1}, std::size_t{2}}) {
        expect_status(
            preflight.targets[index],
            AurUpdateExecutionTargetStatus::Incomplete,
            "Duplicate RequiresCheck target");
        expect(
            has_issue(
                preflight.targets[index],
                AurUpdateExecutionReason::UpdatePlanInconsistent) &&
                !preflight.targets[index].skip_kind.has_value(),
            "Duplicate RequiresCheck identity was guessed independent");
    }
    expect(
        has_blocking_targets(preflight) && !can_execute(preflight),
        "Duplicate RequiresCheck identity failed open");
}

void test_required_devel_localization_keeps_other_hard_blockers() {
    reset_preflight_stub();
    const RootTargetIdentity root{0, "hard-blocker-root"};
    BuildPlan plan = build_plan_for({
        {"hard-blocker-root", "hard-blocker-root", "hard-blocker-root"},
    });
    add_required_package_target(
        plan, "hard-devel-git", "hard-devel-base", {root});
    plan.dependency_edges.push_back(typed_aur_exact_edge(
        "hard-blocker-root", "hard-blocker-root", "hard-devel-git",
        "hard-devel-base", PackageRole::RuntimeDependency, "1.0-1",
        ConstraintEvaluation::unsatisfied()));

    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "hard-blocker-root",
        "hard-blocker-root",
        "hard-ambiguous-api",
        PackageRole::BuildDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        exact_requirement("hard-ambiguous-api"),
        std::nullopt,
        std::nullopt});
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "hard-ambiguous-api",
        {
            typed_aur_provider(
                "hard-provider-a", "hard-provider-a",
                "hard-ambiguous-api"),
            typed_repository_provider(
                "hard-provider-b", "hard-provider-b",
                "hard-ambiguous-api"),
        }});

    add_dependency_target(
        plan, "uncovered-artifact", "uncovered-artifact",
        {root}, PackageRole::CheckDependency);
    plan.dependency_edges.push_back(typed_aur_exact_edge(
        "hard-blocker-root", "hard-blocker-root",
        "uncovered-artifact", "uncovered-artifact",
        PackageRole::CheckDependency));
    plan.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::ConfirmedInstalledConflict,
        "hard-blocker-root", "hard-blocker-root",
        {{0, "hard-blocker-root"}}));
    return_build_plan(std::move(plan));

    const AurUpdateExecutionPreflight preflight =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                remote_entry(
                    "hard-blocker-root",
                    InstalledPackageReason::Explicit),
                requires_check_entry(
                    "hard-devel-git", "hard-devel-base"),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    const AurUpdateExecutionTarget& affected = preflight.targets[0];
    expect_status(
        affected, AurUpdateExecutionTargetStatus::Incomplete,
        "RequiresCheck with hard blockers");
    expect_status(
        preflight.targets[1], AurUpdateExecutionTargetStatus::Skipped,
        "RequiresCheck target with hard blockers");
    expect_required_devel_blocker(
        affected,
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1, 0, std::nullopt, "hard-devel-git", "hard-devel-base",
        {PackageRole::RuntimeDependency}, {root},
        "Hard blocker exact dependency");
    expect(
        has_issue(
            affected,
            AurUpdateExecutionReason::VersionConstraintUnverified) &&
            has_issue(
                affected,
                AurUpdateExecutionReason::AmbiguousProvider) &&
            has_issue(
                affected,
                AurUpdateExecutionReason::
                    ConflictsOrReplacesUnresolved) &&
            has_issue(
                affected,
                AurUpdateExecutionReason::BuildPlanInconsistent) &&
            std::any_of(
                affected.issues.begin(), affected.issues.end(),
                [](const AurUpdateExecutionIssue& issue) {
                    return issue.build_plan_projection_issue.has_value() &&
                           issue.build_plan_projection_issue->kind ==
                               BuildPlanArtifactTargetProjectionIssueKind::
                                   UncoveredPlannedPackageTarget;
                }) &&
            preflight.targets[1].issues.size() == 1 &&
            preflight.targets[1].issues.front().reason ==
                AurUpdateExecutionReason::DevelRequiresCheck,
        "RequiresCheck localization filtered or reassigned a hard blocker");
}

void test_unknown_requires_check_policy_fails_closed() {
    reset_preflight_stub();
    const AurUpdateExecutionPreflight preflight =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{remote_entry(
                "unknown-policy", InstalledPackageReason::Explicit)}},
            static_cast<DevelRequiresCheckPolicy>(-1));

    expect(
        resolver_call_count() == 0,
        "Unknown RequiresCheck policy reached dependency resolution");
    expect(
        preflight.devel_requires_check_policy.has_value() &&
            !is_known_devel_requires_check_policy(
                *preflight.devel_requires_check_policy) &&
            has_blocking_targets(preflight) &&
            !can_execute(preflight),
        "Unknown RequiresCheck policy was rounded to executable success");
}

void test_executable_hidden_required_devel_payload_fails_closed() {
    AurUpdateExecutionTarget target;
    target.update_plan_index = 0;
    target.update = remote_entry(
        "hidden-required-devel", InstalledPackageReason::Explicit);
    target.status = AurUpdateExecutionTargetStatus::Executable;
    target.desired_install_reason = DesiredInstallReason::Explicit;

    AurUpdateExecutionIssue hidden_issue;
    hidden_issue.required_devel_target_blocker =
        AurUpdateRequiredDevelTargetBlocker{
            AurUpdateRequiredDevelTargetRelation::AurExactDependency,
            1,
            "required-devel-git",
            DevelRequiresCheckReason::SuffixCandidateOnly};
    target.issues.push_back(std::move(hidden_issue));

    const AurUpdateExecutionPreflight preflight{
        {std::move(target)}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    expect(
        !is_valid_aur_update_execution_target_skip_snapshot(
            preflight.targets.front()) &&
            !has_valid_aur_update_execution_policy_snapshot(preflight) &&
            has_blocking_targets(preflight) && !can_execute(preflight),
        "Executable hidden required-devel payload did not fail closed");
}

void test_required_devel_target_blocker_foundation_is_lossless() {
    const std::vector<RootTargetIdentity> affected_roots{
        {0, "dependent-root"}};
    AurUpdateRequiredDevelTargetBlocker blocker{
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        1,
        "required-devel-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    blocker.dependency_edge_index = 3;
    blocker.build_plan_order_index = 2;
    blocker.package_base = "required-devel-base";
    blocker.roles = {PackageRole::RuntimeDependency};
    blocker.affected_roots = affected_roots;

    AurUpdateExecutionIssue issue;
    issue.reason =
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck;
    issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    issue.required_devel_target_blocker = blocker;
    expect(
        issue.required_devel_target_blocker == blocker &&
            issue.required_devel_target_blocker
                    ->requires_check_update_plan_index ==
                1 &&
            issue.required_devel_target_blocker
                    ->dependency_edge_index ==
                std::optional<std::size_t>{3} &&
            issue.required_devel_target_blocker
                    ->build_plan_order_index ==
                std::optional<std::size_t>{2} &&
            issue.required_devel_target_blocker->package_name ==
                "required-devel-git" &&
            issue.required_devel_target_blocker->package_base ==
                std::optional<std::string>{"required-devel-base"} &&
            issue.required_devel_target_blocker->roles ==
                std::vector<PackageRole>{
                    PackageRole::RuntimeDependency} &&
            issue.required_devel_target_blocker->affected_roots ==
                affected_roots &&
            is_known_aur_update_required_devel_target_relation(
                issue.required_devel_target_blocker->relation) &&
            !is_known_aur_update_required_devel_target_relation(
                static_cast<AurUpdateRequiredDevelTargetRelation>(-1)),
        "Required devel target blocker foundation lost typed identity");
}

void test_five_field_suffix_up_to_date_is_inconsistent() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "legacy-current-git", InstalledPackageReason::Explicit,
        AurUpdateClassification::UpToDate)}};

    const AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);

    expect(
        resolver_call_count() == 0,
        "Inconsistent five-field suffix entry called the resolver");
    expect_status(
        preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Five-field suffix UpToDate consistency");
    expect(
        has_issue(
            preflight.targets.front(),
            AurUpdateExecutionReason::UpdatePlanInconsistent) &&
            !has_issue(
                preflight.targets.front(),
                AurUpdateExecutionReason::UpToDate),
        "Five-field suffix entry remained a normal up-to-date skip");
}

void test_empty_and_skip_only_plans_suppress_resolution() {
    reset_preflight_stub();
    AurUpdateExecutionPreflight empty =
        resolve_aur_update_execution_preflight(AurUpdatePlan{});
    expect(empty.targets.empty(), "Empty update plan produced targets");
    expect(!empty.build_plan.has_value(), "Empty update plan produced a BuildPlan");
    expect(resolver_call_count() == 0, "Empty update plan called the resolver");
    expect(!has_executable_targets(empty), "Empty preflight has executable targets");
    expect(!has_blocking_targets(empty), "Empty preflight has blocking targets");
    expect(!can_execute(empty), "Empty preflight was executable");

    reset_preflight_stub();
    AurUpdatePlan skip_only{{
        remote_entry(
            "current-package", InstalledPackageReason::Unknown,
            AurUpdateClassification::UpToDate),
        entry_without_remote(
            "foreign-package",
            AurUpdateClassification::NonAurForeign),
    }};
    AurUpdateExecutionPreflight skipped =
        resolve_aur_update_execution_preflight(skip_only);
    expect(resolver_call_count() == 0, "Skip-only update plan called the resolver");
    expect(!skipped.build_plan.has_value(), "Skip-only update plan produced a BuildPlan");
    expect(!has_executable_targets(skipped), "Skip-only preflight has executable targets");
    expect(!has_blocking_targets(skipped), "Normal skips were treated as blockers");
    expect(!can_execute(skipped), "Skip-only preflight was executable");
}

void test_installed_reason_mapping_and_root_dependency_overlap() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry("dependency-root", InstalledPackageReason::Dependency),
        remote_entry("explicit-root", InstalledPackageReason::Explicit),
        remote_entry("unknown-root", InstalledPackageReason::Unknown),
    }};
    BuildPlan plan = build_plan_for({
        {"dependency-root", "dependency-root", "dependency-root"},
        {"explicit-root", "explicit-root", "explicit-root"},
        {"unknown-root", "unknown-root", "unknown-root"},
    });
    PlannedPackageTarget* dependency_root =
        find_package_target(plan, "dependency-root");
    expect(dependency_root != nullptr, "Dependency root fixture is missing");
    dependency_root->roles.push_back(PackageRole::RuntimeDependency);
    dependency_root->roots.push_back(RootTargetIdentity{1, "explicit-root"});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "explicit-root",
        "explicit-root",
        "dependency-root",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"dependency-root"},
        std::optional<std::string>{"dependency-root"},
        std::nullopt});
    return_build_plan(std::move(plan));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);

    expect_single_resolver_call(
        {"dependency-root", "explicit-root", "unknown-root"},
        "Installed reason plan");
    expect(
        preflight.targets[0].desired_install_reason ==
            std::optional<DesiredInstallReason>{
                DesiredInstallReason::Dependency},
        "Dependency root was promoted to explicit");
    expect_status(
        preflight.targets[0], AurUpdateExecutionTargetStatus::Executable,
        "Dependency root overlap");
    expect(
        preflight.targets[1].desired_install_reason ==
            std::optional<DesiredInstallReason>{
                DesiredInstallReason::Explicit},
        "Explicit root reason differs");
    expect(
        !preflight.targets[2].desired_install_reason.has_value(),
        "Unknown root acquired an install reason");
    expect_status(
        preflight.targets[2], AurUpdateExecutionTargetStatus::Incomplete,
        "Unknown installed reason");
    expect(
        has_issue(
            preflight.targets[2],
            AurUpdateExecutionReason::InstalledReasonUnknown),
        "Unknown installed reason issue is missing");
}

void test_typed_unsatisfied_constraint_blocks_update_preflight() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry("constraint-root", InstalledPackageReason::Explicit),
    }};
    BuildPlan plan = build_plan_for({
        {"constraint-root", "constraint-root", "constraint-root"},
    });
    const RootTargetIdentity root{0, "constraint-root"};
    add_dependency_target(
        plan, "constraint-child", "constraint-child", {root});
    plan.order.insert(
        plan.order.begin(),
        BuildPlanEntry{"constraint-child", {"constraint-child"}});
    ConsumerDependencyRequirement requirement(
        "constraint-child>=3", "constraint-child",
        DependencyVersionConstraint(
            DependencyVersionRelation::GreaterThanOrEqual, "3"));
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "constraint-root",
        "constraint-root",
        "constraint-child>=3",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"constraint-child"},
        std::optional<std::string>{"constraint-child"},
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{requirement},
        ResolvedDependencyCandidate{AurResolvedDependencyCandidate{
            "constraint-child",
            "constraint-child",
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                "2.0-1")}},
        ConstraintEvaluation::unsatisfied()});
    return_build_plan(std::move(plan));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Typed Unsatisfied update preflight");
    expect(
        has_issue(
            preflight.targets.front(),
            AurUpdateExecutionReason::VersionConstraintUnverified),
        "Typed Unsatisfied update constraint did not produce a blocker");
    expect(!can_execute(preflight), "Unsatisfied update preflight was executable");
}

void test_duplicate_update_targets_suppress_resolution() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry("duplicate-root", InstalledPackageReason::Explicit),
        remote_entry("duplicate-root", InstalledPackageReason::Dependency),
    }};

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);

    expect(resolver_call_count() == 0, "Duplicate roots reached the resolver");
    expect(!preflight.build_plan.has_value(), "Duplicate roots produced a BuildPlan");
    for(const auto& target : preflight.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Incomplete,
            "Duplicate update target");
        expect(
            has_issue(
                target,
                AurUpdateExecutionReason::DuplicateUpdateTarget),
            "Duplicate update issue is missing");
    }
    expect(!can_execute(preflight), "Duplicate update preflight was executable");
}

void test_update_plan_and_build_plan_consistency() {
    reset_preflight_stub();
    AurUpdatePlan invalid_update{{entry_without_remote(
        "missing-update-metadata",
        AurUpdateClassification::UpdateAvailable,
        InstalledPackageReason::Explicit)}};
    return_build_plan(build_plan_for({
        {"missing-update-metadata", "missing-update-metadata",
         "missing-update-metadata"},
    }));
    AurUpdateExecutionPreflight invalid =
        resolve_aur_update_execution_preflight(invalid_update);
    expect_single_resolver_call(
        {"missing-update-metadata"},
        "Inconsistent UpdateAvailable target");
    expect_status(
        invalid.targets.front(), AurUpdateExecutionTargetStatus::Incomplete,
        "Inconsistent update plan");
    expect(
        has_issue(
            invalid.targets.front(),
            AurUpdateExecutionReason::UpdatePlanInconsistent),
        "Update-plan inconsistency issue is missing");

    reset_preflight_stub();
    AurUpdatePlan mismatched{{remote_entry(
        "split-cli", InstalledPackageReason::Explicit,
        AurUpdateClassification::UpdateAvailable,
        "split-cli", "expected-base")}};
    return_build_plan(build_plan_for(
        {{"split-cli", "split-cli", "different-base"}}));
    AurUpdateExecutionPreflight mismatch =
        resolve_aur_update_execution_preflight(mismatched);
    expect_status(
        mismatch.targets.front(), AurUpdateExecutionTargetStatus::Incomplete,
        "PackageBase mismatch");
    expect(
        has_issue(
            mismatch.targets.front(),
            AurUpdateExecutionReason::PackageBaseMismatch),
        "PackageBase mismatch issue is missing");

    reset_preflight_stub();
    AurUpdatePlan missing_root{{remote_entry(
        "missing-root", InstalledPackageReason::Explicit)}};
    return_build_plan(BuildPlan{});
    AurUpdateExecutionPreflight missing =
        resolve_aur_update_execution_preflight(missing_root);
    expect_status(
        missing.targets.front(), AurUpdateExecutionTargetStatus::Incomplete,
        "Missing BuildPlan root");
    expect(
        has_issue(
            missing.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Missing BuildPlan root inconsistency is absent");

    reset_preflight_stub();
    AurUpdatePlan duplicate_package_target{{remote_entry(
        "duplicate-plan-root", InstalledPackageReason::Explicit)}};
    BuildPlan duplicate_plan = build_plan_for({
        {"duplicate-plan-root", "duplicate-plan-root", "duplicate-plan-root"},
    });
    duplicate_plan.package_targets.push_back(
        duplicate_plan.package_targets.front());
    return_build_plan(std::move(duplicate_plan));
    AurUpdateExecutionPreflight duplicate =
        resolve_aur_update_execution_preflight(duplicate_package_target);
    expect_status(
        duplicate.targets.front(), AurUpdateExecutionTargetStatus::Incomplete,
        "Duplicate BuildPlan package target");
    expect(
        has_issue(
            duplicate.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Duplicate BuildPlan target inconsistency is absent");
}

void test_projection_payload_keeps_distinct_target_indices() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "payload-root", InstalledPackageReason::Explicit)}};
    const RootTargetIdentity root{0, "payload-root"};
    BuildPlan plan;
    plan.root_targets.push_back(root);
    const PlannedPackageTarget duplicated_target{
        "payload-root",
        "payload-root",
        {PackageRole::Root},
        {root, root}};
    plan.package_targets = {duplicated_target, duplicated_target};
    plan.order.push_back(
        BuildPlanEntry{"payload-root", {"payload-root"}});
    return_build_plan(std::move(plan));

    const AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    const AurUpdateExecutionTarget& target = preflight.targets.front();
    std::vector<std::size_t> projection_target_indices;
    for(const auto& issue : target.issues) {
        if(!issue.build_plan_projection_issue.has_value()) continue;
        const auto& projection = *issue.build_plan_projection_issue;
        if(projection.kind !=
               BuildPlanArtifactTargetProjectionIssueKind::
                   RootAttributionInconsistent ||
           projection.package_target_indices.size() != 1) {
            continue;
        }
        projection_target_indices.push_back(
            projection.package_target_indices.front());
    }

    expect_status(
        target,
        AurUpdateExecutionTargetStatus::Incomplete,
        "Projection payload distinction");
    expect(
        projection_target_indices == std::vector<std::size_t>{0, 1},
        "Projection issues differing only by target index were deduplicated");
}

void test_incomplete_build_plan_issues_are_typed_and_deduplicated() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "incomplete-root", InstalledPackageReason::Explicit)}};
    BuildPlan plan = build_plan_for({
        {"incomplete-root", "incomplete-root", "incomplete-root"},
    });
    const RootTargetIdentity root{0, "incomplete-root"};
    add_dependency_target(plan, "cycle-package", "cycle-base", {root});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "incomplete-root",
        "incomplete-root",
        "missing-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::Unknown,
        std::nullopt,
        std::nullopt,
        std::nullopt});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "incomplete-root",
        "incomplete-root",
        "constrained-dependency>=2",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"constrained-dependency"},
        std::optional<std::string>{"constrained-dependency"},
        std::nullopt});
    plan.unresolved = {
        "missing-dependency",
        "constrained-dependency>=2 (version constraint is not verified)",
    };
    plan.cycles = {"cycle-base"};

    BuildPlanResolutionFailure metadata_failure{
        BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
        std::optional<std::string>{"incomplete-root"},
        std::optional<std::string>{"incomplete-root"},
        "metadata-dependency",
        std::optional<std::string>{"metadata-dependency"},
        {root},
        "metadata unavailable"};
    plan.resolution_failures.push_back(metadata_failure);
    plan.resolution_failures.push_back(metadata_failure);
    plan.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::
            InstalledPackageMetadataUnavailable,
        std::optional<std::string>{"incomplete-root"},
        std::optional<std::string>{"incomplete-root"},
        "installed-dependency",
        std::optional<std::string>{"installed-dependency"},
        {root},
        "installed metadata unavailable"});
    plan.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
        std::optional<std::string>{"incomplete-root"},
        std::optional<std::string>{"incomplete-root"},
        "repository-dependency",
        std::optional<std::string>{"repository-dependency"},
        {root},
        "repository metadata unavailable"});
    plan.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
        std::optional<std::string>{"incomplete-root"},
        std::optional<std::string>{"incomplete-root"},
        "virtual-dependency",
        std::optional<std::string>{"virtual-dependency"},
        {root},
        "provider search unavailable"});
    plan.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable,
        std::optional<std::string>{"incomplete-root"},
        std::optional<std::string>{"incomplete-root"},
        "provider-candidate",
        std::optional<std::string>{"virtual-dependency"},
        {root},
        "provider candidate unavailable"});
    return_build_plan(std::move(plan));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    const AurUpdateExecutionTarget& target = preflight.targets.front();

    expect_status(
        target, AurUpdateExecutionTargetStatus::Incomplete,
        "Typed incomplete BuildPlan");
    expect(
        has_issue(target, AurUpdateExecutionReason::UnresolvedDependency),
        "Unresolved dependency issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::
                InstalledPackageMetadataUnavailable),
        "Installed package metadata issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::RepositoryMetadataUnavailable),
        "Repository metadata issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::VersionConstraintUnverified),
        "Version constraint issue is missing");
    expect(
        has_issue(target, AurUpdateExecutionReason::DependencyCycle),
        "Dependency cycle issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::AurDependencyMetadataUnavailable),
        "AUR dependency metadata issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::ProviderMetadataUnavailable),
        "Provider metadata issue is missing");
    expect(
        issue_count(
            target,
            AurUpdateExecutionReason::AurDependencyMetadataUnavailable) == 1,
        "Duplicate metadata failure produced duplicate target issues");
}

void test_complete_split_build_plan_is_model_valid() {
    reset_preflight_stub();
    AurUpdatePlan split_root_plan{{remote_entry(
        "split-cli", InstalledPackageReason::Explicit,
        AurUpdateClassification::UpdateAvailable,
        "split-cli", "split-suite")}};
    BuildPlan root_plan = build_plan_for({
        {"split-cli", "split-cli", "split-suite"},
    });
    root_plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{"split-suite", "split-cli"});
    return_build_plan(std::move(root_plan));
    AurUpdateExecutionPreflight split_root =
        resolve_aur_update_execution_preflight(split_root_plan);
    expect_status(
        split_root.targets.front(),
        AurUpdateExecutionTargetStatus::Executable,
        "Split update root");
    expect(
        !has_issue(
            split_root.targets.front(),
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
        "Complete split root retained a model-level split blocker");

    reset_preflight_stub();
    AurUpdatePlan complete_multiple_plan{{remote_entry(
        "complete-root", InstalledPackageReason::Explicit)}};
    BuildPlan complete_multiple = build_plan_for({
        {"complete-root", "complete-root", "complete-root"},
    });
    const RootTargetIdentity complete_root{0, "complete-root"};
    add_dependency_target(
        complete_multiple, "complete-child-a", "complete-suite",
        {complete_root});
    add_dependency_target(
        complete_multiple, "complete-child-b", "complete-suite",
        {complete_root});
    complete_multiple.order.push_back(BuildPlanEntry{
        "complete-suite", {"complete-child-a", "complete-child-b"}});
    for(const char* child : {"complete-child-a", "complete-child-b"}) {
        complete_multiple.dependency_edges.push_back(BuildPlanDependencyEdge{
            "complete-root",
            "complete-root",
            child,
            PackageRole::RuntimeDependency,
            DependencyKind::Aur,
            std::optional<std::string>{child},
            std::optional<std::string>{"complete-suite"},
            std::nullopt});
    }
    return_build_plan(std::move(complete_multiple));

    AurUpdateExecutionPreflight complete_preflight =
        resolve_aur_update_execution_preflight(complete_multiple_plan);
    expect_status(
        complete_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Executable,
        "Complete same-Base multiple target plan");
    expect(
        !has_issue(
            complete_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Complete same-Base coverage was marked inconsistent");
    expect(
        !has_issue(
            complete_preflight.targets.front(),
            AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase),
        "Complete same-Base coverage retained a multiple-target blocker");
}

void test_incomplete_same_base_coverage_is_typed_failure() {
    reset_preflight_stub();

    AurUpdatePlan update_plan{{remote_entry(
        "unsupported-root", InstalledPackageReason::Explicit)}};
    BuildPlan plan = build_plan_for({
        {"unsupported-root", "unsupported-root", "unsupported-root"},
    });
    const RootTargetIdentity root{0, "unsupported-root"};
    add_dependency_target(plan, "split-child", "shared-suite", {root});
    add_dependency_target(plan, "second-child", "shared-suite", {root});
    // same-base blind spotはpackage_namesからsecond-childだけが落ちる形で作る。
    plan.order.push_back(BuildPlanEntry{"shared-suite", {"split-child"}});
    plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{"shared-suite", "split-child"});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "unsupported-root",
        "unsupported-root",
        "split-child",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"split-child"},
        std::optional<std::string>{"shared-suite"},
        std::nullopt});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "unsupported-root",
        "unsupported-root",
        "second-child",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"second-child"},
        std::optional<std::string>{"shared-suite"},
        std::nullopt});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "unsupported-root",
        "unsupported-root",
        "virtual-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt});
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "virtual-dependency",
        {
            ProvidedDependency::from_repository(
                "extra", "provider-a"),
            ProvidedDependency::from_aur("provider-b"),
        }});
    plan.metadata_risks.push_back(BuildPlanMetadataRisk{
        "second-child", "shared-suite", {"old-package"}, {"renamed-package"}});
    plan.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::ConfirmedInstalledConflict,
        "second-child", "shared-suite", {{0, "unsupported-root"}}));
    return_build_plan(std::move(plan));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    const AurUpdateExecutionTarget& target = preflight.targets.front();

    expect_status(
        target, AurUpdateExecutionTargetStatus::Incomplete,
        "Incomplete same-Base coverage plan");
    expect(
        !has_issue(
            target,
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
        "Incomplete coverage retained a split-only model blocker");
    expect(
        has_issue(target, AurUpdateExecutionReason::AmbiguousProvider),
        "Ambiguous provider issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::ConflictsOrReplacesUnresolved),
        "Conflicts/replaces issue is missing");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Missing same-Base child coverage was not a typed inconsistency");
    const auto uncovered_issue = std::find_if(
        target.issues.begin(), target.issues.end(),
        [](const AurUpdateExecutionIssue& issue) {
            return issue.build_plan_projection_issue.has_value() &&
                   issue.build_plan_projection_issue->kind ==
                       BuildPlanArtifactTargetProjectionIssueKind::
                           UncoveredPlannedPackageTarget;
        });
    expect(
        uncovered_issue != target.issues.end() &&
            uncovered_issue->build_plan_projection_issue
                    ->package_name ==
                std::optional<std::string>{"second-child"} &&
            uncovered_issue->build_plan_projection_issue
                    ->package_base ==
                std::optional<std::string>{"shared-suite"} &&
            uncovered_issue->build_plan_projection_issue
                    ->package_target_indices.size() == 1,
        "Preflight lost the typed uncovered-target projection payload");
    expect(
        !has_issue(
            target,
            AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase),
        "Incomplete coverage was flattened to the legacy multiple-target blocker");
    expect(!can_execute(preflight), "Unsupported invocation was executable");
}

void test_typed_relation_assessment_preflight_mapping() {
    const std::vector<PackageRelationAssessmentKind> blocking_kinds = {
        PackageRelationAssessmentKind::ConfirmedInstalledConflict,
        PackageRelationAssessmentKind::ConfirmedPlannedTargetConflict,
        PackageRelationAssessmentKind::PotentialReplacement,
        PackageRelationAssessmentKind::DeclaredRelation,
        PackageRelationAssessmentKind::Unknown,
        PackageRelationAssessmentKind::Invalid};
    for(const PackageRelationAssessmentKind kind : blocking_kinds) {
        reset_preflight_stub();
        AurUpdatePlan update_plan{{remote_entry(
            "relation-root", InstalledPackageReason::Explicit)}};
        BuildPlan plan = build_plan_for({{"relation-root", "relation-root", "relation-root"}});
        plan.metadata_risks.push_back(BuildPlanMetadataRisk{
            "relation-root", "relation-root", {"relation-target"}, {}});
        plan.relation_assessments.push_back(
            relation_assessment_fixture(kind));
        return_build_plan(std::move(plan));

        const AurUpdateExecutionPreflight preflight =
            resolve_aur_update_execution_preflight(update_plan);
        const AurUpdateExecutionTarget& target = preflight.targets.front();
        expect_status(
            target, AurUpdateExecutionTargetStatus::Unsupported,
            "typed relation blocker");
        expect(
            issue_count(
                target,
                AurUpdateExecutionReason::
                    ConflictsOrReplacesUnresolved) == 1,
            "Typed relation blocker was duplicated by raw metadata");
        const auto issue = std::find_if(
            target.issues.begin(), target.issues.end(),
            [](const AurUpdateExecutionIssue& candidate) {
                return candidate.relation_reason.has_value();
            });
        expect(
            issue != target.issues.end() &&
                issue->package_name ==
                    std::optional<std::string>{"relation-root"} &&
                issue->package_base ==
                    std::optional<std::string>{"relation-root"} &&
                issue->relation_reason->assessment.kind == kind &&
                issue->relation_reason->assessment.declaration
                        .target_component() ==
                    "relation-target" &&
                issue->relation_reason->assessment.declaring_package
                        .roots ==
                    std::vector<
                        PackageRelationRootAttribution>{
                        {0, "relation-root"}},
            "Preflight lost typed relation target or root attribution");
        const std::string& diagnostic = issue->diagnostic;
        const std::string expected_outcome = [&]() {
            switch(kind) {
                case PackageRelationAssessmentKind::
                    ConfirmedInstalledConflict:
                    return std::string("Installed conflict confirmed");
                case PackageRelationAssessmentKind::
                    ConfirmedPlannedTargetConflict:
                    return std::string("Planned-target conflict confirmed");
                case PackageRelationAssessmentKind::PotentialReplacement:
                    return std::string("Potential replacement impact");
                case PackageRelationAssessmentKind::DeclaredRelation:
                    return std::string("Declared relation awaiting assessment");
                case PackageRelationAssessmentKind::Unknown:
                    return std::string("Relation judgment unavailable");
                case PackageRelationAssessmentKind::Invalid:
                    return std::string(
                        "Invalid relation metadata or observation");
                case PackageRelationAssessmentKind::
                    ConfirmedNoMatchingCurrentOrPlannedTarget:
                    break;
            }
            return std::string();
        }();
        expect(
            diagnostic.find(expected_outcome) != std::string::npos &&
                diagnostic.find("relation-root") !=
                    std::string::npos &&
                diagnostic.find("relation-target") !=
                    std::string::npos &&
                diagnostic.find("ConfirmedInstalledConflict") ==
                    std::string::npos &&
                diagnostic.find("PotentialReplacement") ==
                    std::string::npos,
            "Preflight public relation diagnostic lost typed semantics");
        if(kind == PackageRelationAssessmentKind::PotentialReplacement) {
            expect(
                diagnostic.find("no automatic replacement is performed") !=
                        std::string::npos &&
                    diagnostic.find("review is required") !=
                        std::string::npos,
                "Replacement preflight diagnostic implies automatic action");
        }
        if(kind == PackageRelationAssessmentKind::Unknown) {
            expect(
                diagnostic.find("not a confirmed absence") !=
                    std::string::npos,
                "Unknown preflight diagnostic implies a completed absence");
        }
        if(kind == PackageRelationAssessmentKind::Invalid) {
            expect(
                diagnostic.find("fail-closed") != std::string::npos,
                "Invalid preflight diagnostic lost fail-closed semantics");
        }
        if(kind == PackageRelationAssessmentKind::
                       ConfirmedInstalledConflict ||
           kind == PackageRelationAssessmentKind::
                       ConfirmedPlannedTargetConflict ||
           kind == PackageRelationAssessmentKind::PotentialReplacement) {
            expect(
                issue->relation_reason->assessment
                    .attributed_package_evidence.has_value(),
                "Confirmed relation lost matched source evidence");
            const PackageRelationObservationRole expected_role =
                kind == PackageRelationAssessmentKind::
                            ConfirmedPlannedTargetConflict
                    ? PackageRelationObservationRole::PlannedTarget
                    : PackageRelationObservationRole::Installed;
            expect(
                issue->relation_reason->assessment
                        .attributed_package_evidence
                        ->observed_package.role == expected_role,
                "Confirmed relation source role differs");
        }
    }

    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "relation-root", InstalledPackageReason::Explicit)}};
    BuildPlan no_match = build_plan_for({{"relation-root", "relation-root", "relation-root"}});
    no_match.metadata_risks.push_back(BuildPlanMetadataRisk{
        "relation-root", "relation-root", {"relation-target"}, {}});
    no_match.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::
            ConfirmedNoMatchingCurrentOrPlannedTarget));
    return_build_plan(std::move(no_match));
    const AurUpdateExecutionPreflight assessed_clear =
        resolve_aur_update_execution_preflight(update_plan);
    expect(
        assessed_clear.targets.front().status ==
                AurUpdateExecutionTargetStatus::Executable &&
            !has_issue(
                assessed_clear.targets.front(),
                AurUpdateExecutionReason::
                    ConflictsOrReplacesUnresolved),
        "Confirmed NoMatch retained a preflight relation blocker");

    reset_preflight_stub();
    BuildPlan raw_only = build_plan_for({{"relation-root", "relation-root", "relation-root"}});
    raw_only.metadata_risks.push_back(BuildPlanMetadataRisk{
        "relation-root", "relation-root", {"relation-target"}, {}});
    return_build_plan(std::move(raw_only));
    const AurUpdateExecutionPreflight compatibility_only =
        resolve_aur_update_execution_preflight(update_plan);
    expect(
        !has_issue(
            compatibility_only.targets.front(),
            AurUpdateExecutionReason::
                ConflictsOrReplacesUnresolved),
        "Raw compatibility metadata remained a preflight authority");
}

void test_incomplete_status_preserves_provider_failure_without_split_blocker() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "mixed-root", InstalledPackageReason::Explicit)}};
    BuildPlan plan = build_plan_for({
        {"mixed-root", "mixed-root", "mixed-root"},
    });
    const RootTargetIdentity root{0, "mixed-root"};
    plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{"mixed-root", "mixed-root"});
    plan.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable,
        std::optional<std::string>{"mixed-root"},
        std::optional<std::string>{"mixed-root"},
        "provider-candidate",
        std::optional<std::string>{"virtual-dependency"},
        {root},
        "provider candidate unavailable"});
    return_build_plan(std::move(plan));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    const AurUpdateExecutionTarget& target = preflight.targets.front();
    expect_status(
        target, AurUpdateExecutionTargetStatus::Incomplete,
        "Incomplete/unsupported reducer");
    expect(
        has_issue(
            target,
            AurUpdateExecutionReason::ProviderMetadataUnavailable),
        "Incomplete issue was lost");
    expect(
        !has_issue(
            target,
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
        "Model-valid split summary retained a preflight split blocker");
}

void test_issue_attribution_and_global_fallback() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry("affected-root", InstalledPackageReason::Explicit),
        remote_entry("clean-root", InstalledPackageReason::Explicit),
    }};
    BuildPlan attributed_plan = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    attributed_plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "affected-root",
        "affected-root",
        "missing-child",
        PackageRole::RuntimeDependency,
        DependencyKind::Unknown,
        std::nullopt,
        std::nullopt,
        std::nullopt});
    attributed_plan.unresolved.push_back("missing-child");
    return_build_plan(std::move(attributed_plan));

    AurUpdateExecutionPreflight attributed =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        attributed.targets[0], AurUpdateExecutionTargetStatus::Incomplete,
        "Affected root attribution");
    expect(
        has_issue(
            attributed.targets[0],
            AurUpdateExecutionReason::UnresolvedDependency),
        "Affected root unresolved issue is missing");
    expect_status(
        attributed.targets[1], AurUpdateExecutionTargetStatus::Executable,
        "Unaffected root attribution");
    expect(
        !has_issue(
            attributed.targets[1],
            AurUpdateExecutionReason::UnresolvedDependency),
        "Unresolved issue leaked to an unaffected root");

    reset_preflight_stub();
    BuildPlan global_plan = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    global_plan.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
        std::optional<std::string>{"orphan-parent"},
        std::optional<std::string>{"orphan-base"},
        "orphan-virtual",
        std::optional<std::string>{"orphan-virtual"},
        {},
        "unattributed provider search failure"});
    return_build_plan(std::move(global_plan));

    AurUpdateExecutionPreflight global =
        resolve_aur_update_execution_preflight(update_plan);
    for(const auto& target : global.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Incomplete,
            "Global blocker attribution");
        expect(
            has_issue(
                target,
                AurUpdateExecutionReason::ProviderMetadataUnavailable),
            "Global typed blocker is missing");
        expect(
            has_issue(
                target,
                AurUpdateExecutionReason::BuildPlanInconsistent),
            "Global attribution inconsistency marker is missing");
    }

    reset_preflight_stub();
    BuildPlan mismatched_plan = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    mismatched_plan.resolution_failures.push_back(
        BuildPlanResolutionFailure{
            BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
            std::optional<std::string>{"affected-root"},
            std::optional<std::string>{"affected-root"},
            "virtual-dependency",
            std::optional<std::string>{"virtual-dependency"},
            {{1, "clean-root"}},
            "failure attributed to an unrelated known root"});
    return_build_plan(std::move(mismatched_plan));

    AurUpdateExecutionPreflight mismatched =
        resolve_aur_update_execution_preflight(update_plan);
    for(const auto& target : mismatched.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Incomplete,
            "Mismatched failure attribution");
        expect(
            has_issue(
                target,
                AurUpdateExecutionReason::ProviderMetadataUnavailable),
            "Mismatched typed blocker did not fall back globally");
        expect(
            has_issue(
                target,
                AurUpdateExecutionReason::BuildPlanInconsistent),
            "Mismatched failure attribution was not rejected");
    }
}

void test_resolution_failure_root_validation() {
    AurUpdatePlan update_plan{{
        remote_entry("affected-root", InstalledPackageReason::Explicit),
        remote_entry("clean-root", InstalledPackageReason::Explicit),
    }};
    const RootTargetIdentity affected_root{0, "affected-root"};
    const RootTargetIdentity clean_root{1, "clean-root"};

    auto expect_global_fallback = [&](BuildPlan plan, const std::string& context) {
        reset_preflight_stub();
        return_build_plan(std::move(plan));
        AurUpdateExecutionPreflight preflight =
            resolve_aur_update_execution_preflight(update_plan);
        for(const auto& target : preflight.targets) {
            expect_status(
                target, AurUpdateExecutionTargetStatus::Incomplete,
                context);
            expect(
                has_issue(
                    target,
                    AurUpdateExecutionReason::ProviderMetadataUnavailable),
                context + ": typed issue did not fall back globally");
            expect(
                has_issue(
                    target,
                    AurUpdateExecutionReason::BuildPlanInconsistent),
                context + ": attribution inconsistency is missing");
        }
    };

    BuildPlan extra_root = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    extra_root.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
        std::optional<std::string>{"affected-root"},
        std::optional<std::string>{"affected-root"},
        "virtual-dependency",
        std::optional<std::string>{"virtual-dependency"},
        {affected_root, clean_root},
        "failure contains an extra root"});
    expect_global_fallback(std::move(extra_root), "Failure with extra root");

    BuildPlan unknown_root = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    unknown_root.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
        std::optional<std::string>{"affected-root"},
        std::optional<std::string>{"affected-root"},
        "virtual-dependency",
        std::optional<std::string>{"virtual-dependency"},
        {{99, "unknown-root"}},
        "failure contains an unknown root"});
    expect_global_fallback(std::move(unknown_root), "Failure with unknown root");

    BuildPlan missing_parent = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    missing_parent.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
        std::optional<std::string>{"missing-parent"},
        std::optional<std::string>{"missing-parent"},
        "virtual-dependency",
        std::optional<std::string>{"virtual-dependency"},
        {affected_root},
        "failure parent target is missing"});
    expect_global_fallback(std::move(missing_parent), "Failure with missing parent");

    BuildPlan incomplete_parent_identity = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    incomplete_parent_identity.resolution_failures.push_back(
        BuildPlanResolutionFailure{
            BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
            std::optional<std::string>{"affected-root"},
            std::nullopt,
            "virtual-dependency",
            std::optional<std::string>{"virtual-dependency"},
            {affected_root},
            "failure parent identity is incomplete"});
    expect_global_fallback(
        std::move(incomplete_parent_identity),
        "Failure with incomplete parent identity");

    reset_preflight_stub();
    BuildPlan shared_parent = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    add_dependency_target(
        shared_parent, "shared-parent", "shared-parent",
        {affected_root, clean_root});
    shared_parent.order.insert(
        shared_parent.order.begin(),
        BuildPlanEntry{"shared-parent", {"shared-parent"}});
    for(const auto& root : {affected_root, clean_root}) {
        shared_parent.dependency_edges.push_back(BuildPlanDependencyEdge{
            root.requested_name,
            root.requested_name,
            "shared-parent",
            PackageRole::RuntimeDependency,
            DependencyKind::Aur,
            std::optional<std::string>{"shared-parent"},
            std::optional<std::string>{"shared-parent"},
            std::nullopt});
    }
    shared_parent.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
        std::optional<std::string>{"shared-parent"},
        std::optional<std::string>{"shared-parent"},
        "shared-child",
        std::optional<std::string>{"shared-child"},
        {affected_root, clean_root},
        "valid shared dependency failure"});
    return_build_plan(std::move(shared_parent));
    AurUpdateExecutionPreflight shared =
        resolve_aur_update_execution_preflight(update_plan);
    for(const auto& target : shared.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Incomplete,
            "Valid shared failure attribution");
        expect(
            has_issue(
                target,
                AurUpdateExecutionReason::AurDependencyMetadataUnavailable),
            "Shared failure did not reach every owning root");
        expect(
            !has_issue(
                target,
                AurUpdateExecutionReason::BuildPlanInconsistent),
            "Valid shared failure attribution was rejected");
    }

    reset_preflight_stub();
    BuildPlan root_failure = build_plan_for({
        {"affected-root", "affected-root", "affected-root"},
        {"clean-root", "clean-root", "clean-root"},
    });
    root_failure.package_targets.erase(root_failure.package_targets.begin());
    root_failure.order.erase(root_failure.order.begin());
    root_failure.resolution_failures.push_back(BuildPlanResolutionFailure{
        BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
        std::nullopt,
        std::nullopt,
        "affected-root",
        std::nullopt,
        {affected_root},
        "valid root metadata failure"});
    return_build_plan(std::move(root_failure));
    AurUpdateExecutionPreflight root =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        root.targets[0], AurUpdateExecutionTargetStatus::Incomplete,
        "Valid root metadata failure");
    expect(
        has_issue(
            root.targets[0],
            AurUpdateExecutionReason::AurDependencyMetadataUnavailable),
        "Root metadata failure lost its owning root");
    expect_status(
        root.targets[1], AurUpdateExecutionTargetStatus::Executable,
        "Root metadata failure leaked to unrelated root");
}

void test_fail_closed_cross_field_consistency() {
    AurUpdatePlan update_plan{{remote_entry(
        "consistency-root", InstalledPackageReason::Explicit)}};

    reset_preflight_stub();
    BuildPlan missing_order = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    missing_order.order.clear();
    return_build_plan(std::move(missing_order));
    AurUpdateExecutionPreflight missing_order_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        missing_order_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Missing BuildPlan execution order");
    expect(
        has_issue(
            missing_order_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Missing execution order was not rejected");

    reset_preflight_stub();
    BuildPlan wrong_order_name = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    wrong_order_name.order.front().package_names = {"unrelated-package"};
    return_build_plan(std::move(wrong_order_name));
    AurUpdateExecutionPreflight wrong_order_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        wrong_order_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Mismatched BuildPlan order package name");

    reset_preflight_stub();
    BuildPlan ambiguous_edge_only = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    ambiguous_edge_only.dependency_edges.push_back(BuildPlanDependencyEdge{
        "consistency-root",
        "consistency-root",
        "missing-provider-summary",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt});
    return_build_plan(std::move(ambiguous_edge_only));
    AurUpdateExecutionPreflight ambiguous_edge_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        ambiguous_edge_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Ambiguous edge without summary");
    expect(
        has_issue(
            ambiguous_edge_preflight.targets.front(),
            AurUpdateExecutionReason::AmbiguousProvider),
        "Ambiguous edge blocker was not derived from the typed edge");
    expect(
        has_issue(
            ambiguous_edge_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Missing ambiguous summary was not rejected");

    reset_preflight_stub();
    AurUpdatePlan split_plan{{remote_entry(
        "split-only-cli", InstalledPackageReason::Explicit,
        AurUpdateClassification::UpdateAvailable,
        "split-only-cli", "split-only-suite")}};
    return_build_plan(build_plan_for({
        {"split-only-cli", "split-only-cli", "split-only-suite"},
    }));
    AurUpdateExecutionPreflight split_without_summary =
        resolve_aur_update_execution_preflight(split_plan);
    expect_status(
        split_without_summary.targets.front(),
        AurUpdateExecutionTargetStatus::Executable,
        "Split identity without summary");
    expect(
        !has_issue(
            split_without_summary.targets.front(),
            AurUpdateExecutionReason::SplitPackageSelectionRequired),
        "Split identity retained a preflight lifecycle blocker");

    reset_preflight_stub();
    BuildPlan rootless_dependency = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    add_dependency_target(
        rootless_dependency, "orphan-dependency", "orphan-dependency", {});
    rootless_dependency.order.insert(
        rootless_dependency.order.begin(),
        BuildPlanEntry{"orphan-dependency", {"orphan-dependency"}});
    return_build_plan(std::move(rootless_dependency));
    AurUpdateExecutionPreflight rootless_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        rootless_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Rootless dependency target");
    expect(
        has_issue(
            rootless_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Rootless dependency target was not a global blocker");

    reset_preflight_stub();
    BuildPlan orphan_dependency = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    add_dependency_target(
        orphan_dependency, "orphan-dependency", "orphan-dependency",
        {{0, "consistency-root"}});
    orphan_dependency.order.insert(
        orphan_dependency.order.begin(),
        BuildPlanEntry{"orphan-dependency", {"orphan-dependency"}});
    return_build_plan(std::move(orphan_dependency));
    AurUpdateExecutionPreflight orphan_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        orphan_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Dependency target without an incoming edge");
    expect(
        has_issue(
            orphan_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Known-root orphan dependency target was not rejected");

    reset_preflight_stub();
    BuildPlan orphan_cycle = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    add_dependency_target(
        orphan_cycle, "orphan-cycle", "orphan-cycle",
        {{0, "consistency-root"}});
    orphan_cycle.order.insert(
        orphan_cycle.order.begin(),
        BuildPlanEntry{"orphan-cycle", {"orphan-cycle"}});
    orphan_cycle.dependency_edges.push_back(BuildPlanDependencyEdge{
        "orphan-cycle",
        "orphan-cycle",
        "orphan-cycle",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"orphan-cycle"},
        std::optional<std::string>{"orphan-cycle"},
        std::nullopt});
    return_build_plan(std::move(orphan_cycle));
    AurUpdateExecutionPreflight orphan_cycle_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        orphan_cycle_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Unreachable dependency self-cycle");
    expect(
        has_issue(
            orphan_cycle_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Unreachable self-cycle justified its own root ownership");

    reset_preflight_stub();
    BuildPlan reachable_cycle = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    PlannedPackageTarget* cycle_root =
        find_package_target(reachable_cycle, "consistency-root");
    expect(cycle_root != nullptr, "Reachable cycle root fixture is missing");
    cycle_root->roles.push_back(PackageRole::RuntimeDependency);
    reachable_cycle.dependency_edges.push_back(BuildPlanDependencyEdge{
        "consistency-root",
        "consistency-root",
        "consistency-root",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"consistency-root"},
        std::optional<std::string>{"consistency-root"},
        std::nullopt});
    return_build_plan(std::move(reachable_cycle));
    AurUpdateExecutionPreflight reachable_cycle_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        reachable_cycle_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Reachable self-cycle without summary");
    expect(
        has_issue(
            reachable_cycle_preflight.targets.front(),
            AurUpdateExecutionReason::DependencyCycle),
        "Typed graph cycle was lost when the cycle summary was missing");
    expect(
        has_issue(
            reachable_cycle_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Missing cycle summary was not marked inconsistent");

    reset_preflight_stub();
    BuildPlan repository_edge_target = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    add_dependency_target(
        repository_edge_target, "repository-only-dependency",
        "repository-only-dependency", {{0, "consistency-root"}});
    repository_edge_target.order.insert(
        repository_edge_target.order.begin(),
        BuildPlanEntry{
            "repository-only-dependency",
            {"repository-only-dependency"}});
    repository_edge_target.dependency_edges.push_back(
        BuildPlanDependencyEdge{
            "consistency-root",
            "consistency-root",
            "repository-only-dependency",
            PackageRole::RuntimeDependency,
            DependencyKind::Repo,
            std::optional<std::string>{
                "repository-only-dependency"},
            std::nullopt,
            std::nullopt});
    return_build_plan(std::move(repository_edge_target));
    AurUpdateExecutionPreflight repository_edge_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        repository_edge_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Repository edge cannot justify an AUR dependency target");
    expect(
        has_issue(
            repository_edge_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Repository edge incorrectly justified an AUR target");

    reset_preflight_stub();
    BuildPlan wrong_repo_identity = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    wrong_repo_identity.dependency_edges.push_back(BuildPlanDependencyEdge{
        "consistency-root",
        "consistency-root",
        "expected-repository-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::Repo,
        std::optional<std::string>{"different-repository-package"},
        std::nullopt,
        std::nullopt});
    return_build_plan(std::move(wrong_repo_identity));
    AurUpdateExecutionPreflight wrong_repo_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        wrong_repo_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Mismatched direct repository edge identity");

    reset_preflight_stub();
    AurUpdatePlan two_roots{{
        remote_entry("owner-root", InstalledPackageReason::Explicit),
        remote_entry("other-root", InstalledPackageReason::Explicit),
    }};
    BuildPlan wrong_aur_ownership = build_plan_for({
        {"owner-root", "owner-root", "owner-root"},
        {"other-root", "other-root", "other-root"},
    });
    add_dependency_target(
        wrong_aur_ownership, "owned-dependency", "owned-dependency",
        {{1, "other-root"}});
    wrong_aur_ownership.order.insert(
        wrong_aur_ownership.order.begin(),
        BuildPlanEntry{"owned-dependency", {"owned-dependency"}});
    wrong_aur_ownership.dependency_edges.push_back(BuildPlanDependencyEdge{
        "owner-root",
        "owner-root",
        "owned-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"owned-dependency"},
        std::optional<std::string>{"owned-dependency"},
        std::nullopt});
    return_build_plan(std::move(wrong_aur_ownership));
    AurUpdateExecutionPreflight wrong_ownership_preflight =
        resolve_aur_update_execution_preflight(two_roots);
    expect_status(
        wrong_ownership_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "AUR edge resolved target with wrong root ownership");

    reset_preflight_stub();
    BuildPlan missing_roles = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    missing_roles.package_targets.push_back(PlannedPackageTarget{
        "roleless-dependency",
        "roleless-dependency",
        {},
        {{0, "consistency-root"}}});
    missing_roles.order.insert(
        missing_roles.order.begin(),
        BuildPlanEntry{"roleless-dependency", {"roleless-dependency"}});
    return_build_plan(std::move(missing_roles));
    AurUpdateExecutionPreflight missing_roles_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        missing_roles_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Roleless planned dependency target");

    reset_preflight_stub();
    BuildPlan root_role_edge = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    root_role_edge.dependency_edges.push_back(BuildPlanDependencyEdge{
        "consistency-root",
        "consistency-root",
        "repository-dependency",
        PackageRole::Root,
        DependencyKind::Repo,
        std::optional<std::string>{"repository-dependency"},
        std::nullopt,
        std::nullopt});
    return_build_plan(std::move(root_role_edge));
    AurUpdateExecutionPreflight root_role_edge_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        root_role_edge_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Dependency edge with Root role");

    reset_preflight_stub();
    BuildPlan missing_provider = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    missing_provider.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency", std::nullopt));
    return_build_plan(std::move(missing_provider));
    AurUpdateExecutionPreflight missing_provider_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        missing_provider_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Provided dependency without resolved provider");
    expect(
        has_issue(
            missing_provider_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Missing resolved provider was accepted");

    reset_preflight_stub();
    BuildPlan empty_repository_origin = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    empty_repository_origin.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency",
            ProvidedDependency::from_repository(
                "", "provider-package")));
    return_build_plan(std::move(empty_repository_origin));
    AurUpdateExecutionPreflight empty_repository_origin_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        empty_repository_origin_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Provided dependency with empty repository origin");
    expect(
        has_issue(
            empty_repository_origin_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Empty repository provider origin was accepted");

    reset_preflight_stub();
    BuildPlan provider_with_control_origin = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    provider_with_control_origin.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency",
            ProvidedDependency::from_repository(
                "co\nre", "provider-package")));
    return_build_plan(std::move(provider_with_control_origin));
    AurUpdateExecutionPreflight provider_with_control_origin_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        provider_with_control_origin_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Provided dependency with control-character repository origin");
    expect(
        has_issue(
            provider_with_control_origin_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Control-character provider origin was accepted");

    reset_preflight_stub();
    BuildPlan repository_provider = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    repository_provider.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency",
            ProvidedDependency::from_repository(
                "aur", "provider-package")));
    return_build_plan(std::move(repository_provider));
    AurUpdateExecutionPreflight repository_provider_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        repository_provider_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Executable,
        "Repository provider with valid origin");

    reset_preflight_stub();
    BuildPlan invalid_provider_package = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    invalid_provider_package.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency",
            ProvidedDependency::from_repository(
                "aur", "invalid/provider")));
    return_build_plan(std::move(invalid_provider_package));
    AurUpdateExecutionPreflight invalid_provider_package_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        invalid_provider_package_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Provided dependency with invalid provider package");
    expect(
        has_issue(
            invalid_provider_package_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Invalid provider package was accepted");

    reset_preflight_stub();
    BuildPlan missing_aur_provider_target = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    missing_aur_provider_target.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency",
            ProvidedDependency::from_aur("missing-provider")));
    return_build_plan(std::move(missing_aur_provider_target));
    AurUpdateExecutionPreflight missing_aur_provider_target_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        missing_aur_provider_target_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "AUR provider without planned target");
    expect(
        has_issue(
            missing_aur_provider_target_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "AUR provider without matching target was accepted");

    reset_preflight_stub();
    BuildPlan orphan_repository_provider = build_plan_for({
        {"consistency-root", "consistency-root", "consistency-root"},
    });
    const RootTargetIdentity consistency_root{0, "consistency-root"};
    add_dependency_target(
        orphan_repository_provider, "repository-provider",
        "repository-provider", {consistency_root});
    orphan_repository_provider.order.insert(
        orphan_repository_provider.order.begin(),
        BuildPlanEntry{
            "repository-provider", {"repository-provider"}});
    orphan_repository_provider.dependency_edges.push_back(
        provided_dependency_edge(
            "consistency-root", "virtual-dependency",
            ProvidedDependency::from_repository(
                "aur", "repository-provider")));
    return_build_plan(std::move(orphan_repository_provider));
    AurUpdateExecutionPreflight orphan_repository_provider_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect_status(
        orphan_repository_provider_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Incomplete,
        "Repository provider with orphan source target");
    expect(
        has_issue(
            orphan_repository_provider_preflight.targets.front(),
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Repository provider incorrectly grounded a source target");
}

void test_rooted_aur_dependency_graph() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry("first-root", InstalledPackageReason::Explicit),
        remote_entry("second-root", InstalledPackageReason::Explicit),
    }};
    BuildPlan shared = build_plan_for({
        {"first-root", "first-root", "first-root"},
        {"second-root", "second-root", "second-root"},
    });
    const std::vector<RootTargetIdentity> roots{
        {0, "first-root"},
        {1, "second-root"},
    };
    add_dependency_target(
        shared, "shared-dependency", "shared-dependency", roots);
    shared.order.insert(
        shared.order.begin(),
        BuildPlanEntry{"shared-dependency", {"shared-dependency"}});
    for(const auto& root : roots) {
        shared.dependency_edges.push_back(BuildPlanDependencyEdge{
            root.requested_name,
            root.requested_name,
            "shared-dependency",
            PackageRole::RuntimeDependency,
            DependencyKind::Aur,
            std::optional<std::string>{"shared-dependency"},
            std::optional<std::string>{"shared-dependency"},
            std::nullopt});
    }
    return_build_plan(std::move(shared));

    AurUpdateExecutionPreflight shared_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    for(const auto& target : shared_preflight.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Executable,
            "Shared dependency rooted graph");
    }

    reset_preflight_stub();
    BuildPlan mutual_cycle = build_plan_for({
        {"first-root", "first-root", "first-root"},
        {"second-root", "second-root", "second-root"},
    });
    PlannedPackageTarget* first_root =
        find_package_target(mutual_cycle, "first-root");
    PlannedPackageTarget* second_root =
        find_package_target(mutual_cycle, "second-root");
    expect(
        first_root != nullptr && second_root != nullptr,
        "Mutual cycle root fixtures are missing");
    first_root->roles.push_back(PackageRole::RuntimeDependency);
    second_root->roles.push_back(PackageRole::RuntimeDependency);
    first_root->roots.push_back(roots[1]);
    second_root->roots.push_back(roots[0]);
    mutual_cycle.dependency_edges.push_back(BuildPlanDependencyEdge{
        "first-root",
        "first-root",
        "second-root",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"second-root"},
        std::optional<std::string>{"second-root"},
        std::nullopt});
    mutual_cycle.dependency_edges.push_back(BuildPlanDependencyEdge{
        "second-root",
        "second-root",
        "first-root",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"first-root"},
        std::optional<std::string>{"first-root"},
        std::nullopt});
    // resolverのcycle summaryはSCC全memberではなく、再訪したPackageBaseを持つ。
    mutual_cycle.cycles.push_back("first-root");
    return_build_plan(std::move(mutual_cycle));

    AurUpdateExecutionPreflight mutual_cycle_preflight =
        resolve_aur_update_execution_preflight(update_plan);
    for(const auto& target : mutual_cycle_preflight.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Incomplete,
            "Mutual root cycle");
        expect(
            has_issue(target, AurUpdateExecutionReason::DependencyCycle),
            "Mutual root cycle was not attributed to every root");
        expect(
            !has_issue(
                target,
                AurUpdateExecutionReason::BuildPlanInconsistent),
            "Valid cycle summary was compared as an exact back-edge set");
    }

    reset_preflight_stub();
    AurUpdatePlan same_base_cycle_plan{{remote_entry(
        "same-base-cycle-a", InstalledPackageReason::Explicit,
        AurUpdateClassification::UpdateAvailable,
        "same-base-cycle-a", "same-base-cycle-suite")}};
    BuildPlan same_base_cycle = build_plan_for({
        {"same-base-cycle-a", "same-base-cycle-a",
         "same-base-cycle-suite"},
    });
    PlannedPackageTarget* same_base_cycle_a =
        find_package_target(same_base_cycle, "same-base-cycle-a");
    expect(
        same_base_cycle_a != nullptr,
        "Same-base cycle root fixture is missing");
    same_base_cycle_a->roles.push_back(PackageRole::RuntimeDependency);
    add_dependency_target(
        same_base_cycle, "same-base-cycle-b",
        "same-base-cycle-suite", {{0, "same-base-cycle-a"}});
    same_base_cycle.order.front().package_names.push_back(
        "same-base-cycle-b");
    same_base_cycle.dependency_edges.push_back(BuildPlanDependencyEdge{
        "same-base-cycle-a",
        "same-base-cycle-suite",
        "same-base-cycle-b",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"same-base-cycle-b"},
        std::optional<std::string>{"same-base-cycle-suite"},
        std::nullopt});
    same_base_cycle.dependency_edges.push_back(BuildPlanDependencyEdge{
        "same-base-cycle-b",
        "same-base-cycle-suite",
        "same-base-cycle-a",
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        std::optional<std::string>{"same-base-cycle-a"},
        std::optional<std::string>{"same-base-cycle-suite"},
        std::nullopt});

    BuildPlan same_base_cycle_without_summary = same_base_cycle;
    return_build_plan(std::move(same_base_cycle_without_summary));
    AurUpdateExecutionPreflight missing_summary_preflight =
        resolve_aur_update_execution_preflight(same_base_cycle_plan);
    const AurUpdateExecutionTarget& missing_summary_target =
        missing_summary_preflight.targets.front();
    expect(
        has_issue(
            missing_summary_target,
            AurUpdateExecutionReason::DependencyCycle) &&
            has_issue(
                missing_summary_target,
                AurUpdateExecutionReason::BuildPlanInconsistent),
        "Same-base typed graph did not detect a missing cycle summary");

    reset_preflight_stub();
    same_base_cycle.cycles.push_back("same-base-cycle-suite");
    return_build_plan(std::move(same_base_cycle));

    AurUpdateExecutionPreflight same_base_cycle_preflight =
        resolve_aur_update_execution_preflight(same_base_cycle_plan);
    const AurUpdateExecutionTarget& same_base_cycle_target =
        same_base_cycle_preflight.targets.front();
    expect_status(
        same_base_cycle_target,
        AurUpdateExecutionTargetStatus::Incomplete,
        "Same-base real dependency cycle");
    expect(
        has_issue(
            same_base_cycle_target,
            AurUpdateExecutionReason::DependencyCycle),
        "Same-base real cycle was lost by PackageBase contraction");
    expect(
        !has_issue(
            same_base_cycle_target,
            AurUpdateExecutionReason::BuildPlanInconsistent),
        "Same-base real cycle did not match its resolver summary");

    reset_preflight_stub();
    AurUpdatePlan provider_plan{{remote_entry(
        "provider-root", InstalledPackageReason::Explicit)}};
    BuildPlan provider = build_plan_for({
        {"provider-root", "provider-root", "provider-root"},
    });
    add_dependency_target(
        provider, "aur-provider", "aur-provider",
        {{0, "provider-root"}});
    provider.order.insert(
        provider.order.begin(),
        BuildPlanEntry{"aur-provider", {"aur-provider"}});
    provider.dependency_edges.push_back(BuildPlanDependencyEdge{
        "provider-root",
        "provider-root",
        "virtual-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        ProvidedDependency::from_aur("aur-provider")});
    return_build_plan(std::move(provider));

    AurUpdateExecutionPreflight provider_preflight =
        resolve_aur_update_execution_preflight(provider_plan);
    expect_status(
        provider_preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Executable,
        "AUR provider rooted graph");
}

void test_ambiguous_provider_specification_normalization() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{
        remote_entry("first-root", InstalledPackageReason::Explicit),
        remote_entry("second-root", InstalledPackageReason::Explicit),
    }};
    BuildPlan plan = build_plan_for({
        {"first-root", "first-root", "first-root"},
        {"second-root", "second-root", "second-root"},
    });
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "first-root", "first-root", "shared-virtual",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt, std::nullopt, std::nullopt});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "second-root", "second-root", " shared-virtual ",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt, std::nullopt, std::nullopt});
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "shared-virtual",
        {
            ProvidedDependency::from_repository(
                "aur", "shared-provider"),
            ProvidedDependency::from_aur("shared-provider"),
        }});
    return_build_plan(std::move(plan));

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect(
        preflight.build_plan.has_value() &&
            preflight.build_plan->ambiguous_providers.size() == 1,
        "Typed ambiguous provider summary is missing");
    expect(
        preflight.build_plan->ambiguous_providers.front().candidates ==
            std::vector<ProvidedDependency>{
                ProvidedDependency::from_repository(
                    "aur", "shared-provider"),
                ProvidedDependency::from_aur("shared-provider"),
            },
        "Typed ambiguous provider candidates were reordered or deduplicated");
    for(const auto& target : preflight.targets) {
        expect_status(
            target, AurUpdateExecutionTargetStatus::Unsupported,
            "Normalized ambiguous provider attribution");
        expect(
            has_issue(target, AurUpdateExecutionReason::AmbiguousProvider),
            "Ambiguous provider did not reach every affected root");
    }
}

void test_skipped_identity_mismatch_is_incomplete() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "installed-name", InstalledPackageReason::Unknown,
        AurUpdateClassification::UpToDate,
        "different-aur-name", "different-aur-name")}};

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(update_plan);
    expect(resolver_call_count() == 0, "Up-to-date identity mismatch called resolver");
    expect_status(
        preflight.targets.front(), AurUpdateExecutionTargetStatus::Incomplete,
        "Up-to-date AUR identity mismatch");
    expect(
        has_issue(
            preflight.targets.front(),
            AurUpdateExecutionReason::UpdatePlanInconsistent),
        "Up-to-date identity mismatch was treated as a normal skip");
}

void test_invocation_helpers() {
    AurUpdateExecutionTarget executable;
    executable.status = AurUpdateExecutionTargetStatus::Executable;
    AurUpdateExecutionTarget skipped;
    skipped.update = remote_entry(
        "skipped", InstalledPackageReason::Unknown,
        AurUpdateClassification::UpToDate);
    skipped.status = AurUpdateExecutionTargetStatus::Skipped;
    skipped.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    skipped.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::UpToDate,
        "skipped",
        std::nullopt,
        std::nullopt,
        "Already up to date."});
    AurUpdateExecutionTarget unsupported;
    unsupported.status = AurUpdateExecutionTargetStatus::Unsupported;
    AurUpdateExecutionTarget incomplete;
    incomplete.status = AurUpdateExecutionTargetStatus::Incomplete;

    AurUpdateExecutionPreflight executable_only{
        {executable, skipped}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    expect(has_executable_targets(executable_only), "Executable helper returned false");
    expect(!has_blocking_targets(executable_only), "Executable plan has a blocker");
    expect(can_execute(executable_only), "Executable plan could not execute");

    AurUpdateExecutionPreflight skip_only{
        {skipped}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    expect(!has_executable_targets(skip_only), "Skip-only helper found executable work");
    expect(!has_blocking_targets(skip_only), "Skip-only helper found a blocker");
    expect(!can_execute(skip_only), "Skip-only helper allowed execution");

    AurUpdateExecutionPreflight with_unsupported{
        {executable, unsupported}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    expect(has_blocking_targets(with_unsupported), "Unsupported blocker was not found");
    expect(!can_execute(with_unsupported), "Unsupported plan allowed execution");

    AurUpdateExecutionPreflight with_incomplete{
        {executable, incomplete}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    expect(has_blocking_targets(with_incomplete), "Incomplete blocker was not found");
    expect(!can_execute(with_incomplete), "Incomplete plan allowed execution");
}

void test_preflight_uses_combined_resolver_seam() {
    reset_preflight_stub();
    AurUpdatePlan update_plan{{remote_entry(
        "read-only-root", InstalledPackageReason::Explicit)}};
    return_build_plan(build_plan_for({
        {"read-only-root", "read-only-root", "read-only-root"},
    }));

    const ProviderSelectionCallback select_provider =
        [](const std::string&,
           const std::vector<ProvidedDependency>&)
        -> std::optional<ProvidedDependency> {
        return std::nullopt;
    };
    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(
            update_plan, select_provider);

    expect_status(
        preflight.targets.front(),
        AurUpdateExecutionTargetStatus::Executable,
        "Resolver seam preflight");
    expect(
        resolver_call_count() == 1 &&
            resolver_calls() ==
                std::vector<std::vector<std::string>>{{"read-only-root"}},
        "Preflight did not use the combined resolver seam exactly once");
    expect(
        resolver_selection_callback_presence() ==
            std::vector<bool>{true},
        "Preflight did not forward the invocation provider selector");

    reset_preflight_stub();
    return_build_plan(build_plan_for({
        {"read-only-root", "read-only-root", "read-only-root"},
    }));
    static_cast<void>(resolve_aur_update_execution_preflight(update_plan));
    expect(
        resolver_selection_callback_presence() ==
            std::vector<bool>{false},
        "BlockOperation test helper did not delegate with an empty provider selector");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

void test_authoritative_devel_route_policy_matrix() {
    for(const auto reason : {DevelRequiresCheckReason::ProvenanceMissing, DevelRequiresCheckReason::InstalledArtifactDrift, DevelRequiresCheckReason::AurRecipeAdvanced}) {
        for(const auto policy : {DevelRequiresCheckPolicy::BlockOperation, DevelRequiresCheckPolicy::SkipIndependentTarget}) {
            reset_preflight_stub();
            auto entry = requires_check_entry("current-git", "current");
            entry.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
            entry.devel_assessment = DevelUpdateAssessment::requires_check(reason);
            const auto result = ::resolve_aur_update_execution_preflight(AurUpdatePlan{{entry}}, policy);
            expect(resolver_call_count() == 0 && !can_execute(result), "RequiresCheck became a build root");
            expect(result.targets.front().status == (policy == DevelRequiresCheckPolicy::BlockOperation ? AurUpdateExecutionTargetStatus::Incomplete : AurUpdateExecutionTargetStatus::Skipped), "strict/independent policy differs");
            expect(result.targets.front().issues.front().devel_requires_check_reason == reason && has_valid_aur_update_execution_policy_snapshot(result), "current reason/snapshot lost");
        }
    }
    for(const auto assessment : {DevelUpdateAssessment::up_to_date(), DevelUpdateAssessment::unknown(DevelUnknownReason::RemoteObservationFailed), DevelUpdateAssessment::unsupported(DevelUnsupportedReason::UnsupportedVcs)}) {
        reset_preflight_stub();
        auto entry = requires_check_entry("current-git", "current");
        entry.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
        entry.devel_assessment = assessment;
        const auto result = ::resolve_aur_update_execution_preflight(AurUpdatePlan{{entry}}, DevelRequiresCheckPolicy::SkipIndependentTarget);
        expect(resolver_call_count() == 0 && !can_execute(result), "non-positive observation became build root");
        expect(has_blocking_targets(result) == (assessment.state() != DevelUpdateAssessmentState::UpToDate), "Unknown/Unsupported silently skipped");
    }
    reset_preflight_stub();
    auto required = requires_check_entry("required-devel-git", "required-devel-base");
    required.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
    required.devel_assessment = DevelUpdateAssessment::requires_check(DevelRequiresCheckReason::ProvenanceMissing);
    BuildPlan plan = build_plan_for({{"affected-root", "affected-root", "affected-root"}});
    add_required_package_target(plan, "required-devel-git", "required-devel-base", {RootTargetIdentity{0, "affected-root"}});
    plan.dependency_edges.push_back(typed_aur_exact_edge("affected-root", "affected-root", "required-devel-git", "required-devel-base"));
    return_build_plan(std::move(plan));
    const auto blocked = ::resolve_aur_update_execution_preflight(AurUpdatePlan{{remote_entry("affected-root", InstalledPackageReason::Explicit), required}}, DevelRequiresCheckPolicy::SkipIndependentTarget);
    expect(has_blocking_targets(blocked) && !can_execute(blocked), "current RequiresCheck dependency re-entry was not blocked");
    bool retained = false;
    for(const auto& target : blocked.targets)
        for(const auto& issue : target.issues)
            if(issue.required_devel_target_blocker && issue.required_devel_target_blocker->devel_requires_check_reason == DevelRequiresCheckReason::ProvenanceMissing) retained = true;
    expect(retained, "required dependency reason was rewritten to suffix-only");
}

} // namespace

int main() {
    try {
        test_authoritative_devel_route_policy_matrix();
        run_case(
            "classification order and combined resolution",
            test_classification_order_and_combined_resolution);
        run_case(
            "devel RequiresCheck blocks without candidate promotion",
            test_devel_requires_check_blocks_without_candidate_promotion);
        run_case(
            "SkipIndependentTarget keeps identity and update precedence",
            test_skip_independent_target_keeps_full_identity_and_update_precedence);
        run_case(
            "required devel exact dependency is root-local",
            test_required_devel_exact_dependency_is_root_local);
        run_case(
            "required devel relation snapshot complete equality",
            test_required_devel_relation_snapshot_complete_equality);
        run_case(
            "required devel provider and repository re-entry is source-aware",
            test_required_devel_provider_and_repository_reentry_are_source_aware);
        run_case(
            "required devel PackageBase uses exact required children",
            test_required_devel_package_base_uses_exact_required_children);
        run_case(
            "required devel shared dependency preserves edge roots",
            test_required_devel_shared_dependency_preserves_edge_roots);
        run_case(
            "required devel drift and unattributed state fail closed",
            test_required_devel_identity_drift_and_unattributed_state_fail_closed);
        run_case(
            "duplicate RequiresCheck identity is not independent",
            test_duplicate_requires_check_identity_is_not_independent);
        run_case(
            "required devel localization keeps hard blockers",
            test_required_devel_localization_keeps_other_hard_blockers);
        run_case(
            "unknown RequiresCheck policy fails closed",
            test_unknown_requires_check_policy_fails_closed);
        run_case(
            "executable hidden required-devel payload fails closed",
            test_executable_hidden_required_devel_payload_fails_closed);
        run_case(
            "required devel target blocker foundation is lossless",
            test_required_devel_target_blocker_foundation_is_lossless);
        run_case(
            "five-field suffix UpToDate is inconsistent",
            test_five_field_suffix_up_to_date_is_inconsistent);
        run_case(
            "empty and skip-only plans suppress resolution",
            test_empty_and_skip_only_plans_suppress_resolution);
        run_case(
            "installed reason mapping and root/dependency overlap",
            test_installed_reason_mapping_and_root_dependency_overlap);
        run_case(
            "typed Unsatisfied constraint blocks update preflight",
            test_typed_unsatisfied_constraint_blocks_update_preflight);
        run_case(
            "duplicate update targets suppress resolution",
            test_duplicate_update_targets_suppress_resolution);
        run_case(
            "update-plan and BuildPlan consistency",
            test_update_plan_and_build_plan_consistency);
        run_case(
            "projection payload keeps distinct target indices",
            test_projection_payload_keeps_distinct_target_indices);
        run_case(
            "incomplete BuildPlan issues are typed and deduplicated",
            test_incomplete_build_plan_issues_are_typed_and_deduplicated);
        run_case(
            "complete split BuildPlan is model-valid",
            test_complete_split_build_plan_is_model_valid);
        run_case(
            "incomplete same-base coverage is typed failure",
            test_incomplete_same_base_coverage_is_typed_failure);
        run_case(
            "typed relation assessment preflight mapping",
            test_typed_relation_assessment_preflight_mapping);
        run_case(
            "incomplete status preserves provider failure without split blocker",
            test_incomplete_status_preserves_provider_failure_without_split_blocker);
        run_case(
            "issue attribution and global fallback",
            test_issue_attribution_and_global_fallback);
        run_case(
            "resolution failure root validation",
            test_resolution_failure_root_validation);
        run_case(
            "fail-closed BuildPlan cross-field consistency",
            test_fail_closed_cross_field_consistency);
        run_case(
            "rooted AUR dependency graph",
            test_rooted_aur_dependency_graph);
        run_case(
            "ambiguous provider specification normalization",
            test_ambiguous_provider_specification_normalization);
        run_case(
            "skipped identity mismatch is incomplete",
            test_skipped_identity_mismatch_is_incomplete);
        run_case("invocation helpers", test_invocation_helpers);
        run_case(
            "preflight uses combined resolver seam",
            test_preflight_uses_combined_resolver_seam);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "AUR update execution preflight tests: all checks passed\n";
    return 0;
}
