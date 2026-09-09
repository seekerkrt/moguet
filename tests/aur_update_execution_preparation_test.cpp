#include "app_config.hpp"
#include "aur_update_execution_preparation.hpp"
#include "stubs/aur-update-execution-preparation/preparation_stub.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

static_assert(
    !std::is_default_constructible_v<
        PreparedAurUpdateSourceBuildInvocation>);
static_assert(
    !std::is_copy_constructible_v<
        PreparedAurUpdateSourceBuildInvocation>);
static_assert(
    std::is_nothrow_move_constructible_v<
        PreparedAurUpdateSourceBuildInvocation>);
static_assert(
    !std::is_move_assignable_v<
        PreparedAurUpdateSourceBuildInvocation>);
static_assert(!std::is_aggregate_v<PreparedAurUpdateSourceBuildInvocation>);
static_assert(
    !std::is_constructible_v<
        PreparedAurUpdateSourceBuildInvocation,
        PreparedProductionSourceBuildInvocation,
        std::vector<AurUpdatePreparedWorkItemAttribution>>);

using FilteredPreparationFunction = AurUpdateSourceBuildPreparation (*)(
    const AurUpdateExecutionPreflight&,
    const AurUpdateBuildUnitSelection&,
    DevelRequiresCheckPolicy,
    SavedSourcePreferencePolicy,
    bool,
    const AppConfig&);
static_assert(std::is_same_v<
              decltype(static_cast<FilteredPreparationFunction>(
                  &prepare_aur_update_source_build_invocation)),
              FilteredPreparationFunction>);

namespace {

namespace fs = std::filesystem;
namespace stub = aur_update_execution_preparation_test_stub;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

AurUpdateExecutionPreflight with_block_operation_policy(
    const AurUpdateExecutionPreflight& preflight) {
    AurUpdateExecutionPreflight snapshot = preflight;
    snapshot.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    return snapshot;
}

// Existing cases are the Strict regression ledger. Ignore cases call the
// policy-bearing production overload explicitly.
AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    bool needed,
    const AppConfig& config) {
    AurUpdateExecutionPreflight snapshot =
        with_block_operation_policy(preflight);
    return ::prepare_aur_update_source_build_invocation(
        snapshot, DevelRequiresCheckPolicy::BlockOperation,
        SavedSourcePreferencePolicy::Strict, needed, config);
}

AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    bool needed,
    const AppConfig& config) {
    AurUpdateExecutionPreflight snapshot =
        with_block_operation_policy(preflight);
    return ::prepare_aur_update_source_build_invocation(
        snapshot, build_unit_selection,
        DevelRequiresCheckPolicy::BlockOperation,
        SavedSourcePreferencePolicy::Strict, needed, config);
}

SourceBuildEnvironment environment(
    std::initializer_list<SourceEnvironmentAssignment> assignments) {
    return SourceBuildEnvironment{
        std::vector<SourceEnvironmentAssignment>{assignments}};
}

bool same_environment(
    const SourceBuildEnvironment& lhs,
    const SourceBuildEnvironment& rhs) {
    if(lhs.ordered_assignments.size() != rhs.ordered_assignments.size()) {
        return false;
    }
    for(std::size_t index = 0; index < lhs.ordered_assignments.size(); ++index) {
        if(lhs.ordered_assignments[index].key !=
               rhs.ordered_assignments[index].key ||
           lhs.ordered_assignments[index].value !=
               rhs.ordered_assignments[index].value) {
            return false;
        }
    }
    return true;
}

SourcePreferenceLoaded loaded_preference(
    const std::string& preference_name,
    SourceBuildEnvironment source_environment = {},
    std::vector<std::string> warnings = {}) {
    return SourcePreferenceLoaded{
        .entry_path = fs::path("/stub/preferences") / preference_name,
        .environment = std::move(source_environment),
        .warnings = std::move(warnings),
        .raw_contents = {},
        .identity = std::nullopt,
    };
}

SourcePreferenceFailure preference_failure(
    const std::string& preference_name,
    SourcePreferenceFailureKind kind,
    std::optional<fs::file_type> observed_file_type = std::nullopt) {
    SourcePreferenceFailure failure{
        kind,
        fs::path("/stub/preferences") / preference_name,
        std::nullopt,
        observed_file_type,
        "strict preference failure for " + preference_name};
    if(kind != SourcePreferenceFailureKind::UnsupportedFileType) {
        failure.system_error =
            std::make_error_code(std::errc::permission_denied);
    }
    return failure;
}

AurUpdatePlanEntry update_entry(
    const std::string& package_name,
    DesiredInstallReason desired_reason,
    const std::string& package_base) {
    return AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        desired_reason == DesiredInstallReason::Explicit
            ? InstalledPackageReason::Explicit
            : InstalledPackageReason::Dependency,
        AurUpdateRemotePackage{
            package_name,
            package_base,
            "2.0-1",
            AurVersionRelation::NewerThanInstalled},
        AurUpdateClassification::UpdateAvailable};
}

AurUpdateExecutionTarget executable_target(
    std::size_t update_plan_index,
    std::size_t root_index,
    const std::string& package_name,
    DesiredInstallReason desired_reason,
    const std::string& package_base) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.build_plan_root_index = root_index;
    target.update = update_entry(
        package_name, desired_reason, package_base);
    target.status = AurUpdateExecutionTargetStatus::Executable;
    target.desired_install_reason = desired_reason;
    return target;
}

AurUpdateExecutionTarget skipped_target(
    std::size_t update_plan_index,
    const std::string& package_name) {
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
        AurUpdateClassification::UpToDate};
    target.status = AurUpdateExecutionTargetStatus::Skipped;
    target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::UpToDate,
        package_name,
        package_name,
        std::nullopt,
        "Already up to date."});
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

AurUpdateExecutionIssue build_plan_inconsistent_issue(
    const std::string& package_name) {
    return AurUpdateExecutionIssue{
        AurUpdateExecutionReason::BuildPlanInconsistent,
        package_name,
        package_name + "-base",
        "dependency>=2",
        "Skipped target retained a BuildPlan inconsistency."};
}

AurUpdateExecutionTarget blocking_target(
    std::size_t update_plan_index,
    const std::string& package_name,
    AurUpdateExecutionTargetStatus status,
    AurUpdateExecutionReason reason) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = update_plan_index;
    target.update = update_entry(
        package_name, DesiredInstallReason::Explicit, package_name);
    target.status = status;
    target.issues.push_back(AurUpdateExecutionIssue{
        reason,
        package_name,
        package_name,
        std::nullopt,
        "Typed blocking preflight issue."});
    return target;
}

AurUpdateExecutionPreflight single_root_preflight(
    const std::string& package_name = "single-root",
    DesiredInstallReason desired_reason = DesiredInstallReason::Explicit,
    const std::string& package_base = {}) {
    const std::string resolved_package_base =
        package_base.empty() ? package_name : package_base;
    const RootTargetIdentity root{0, package_name};

    BuildPlan plan;
    plan.root_targets.push_back(root);
    plan.package_targets.push_back(PlannedPackageTarget{
        package_name,
        resolved_package_base,
        {PackageRole::Root},
        {root}});
    plan.order.push_back(
        BuildPlanEntry{resolved_package_base, {package_name}});

    AurUpdateExecutionPreflight preflight;
    preflight.targets.push_back(executable_target(
        0, 0, package_name, desired_reason, resolved_package_base));
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateExecutionPreflight missing_required_devel_relation_preflight() {
    const std::string root_name = "missing-relation-root";
    const std::string devel_name = "missing-relation-git";
    const std::string devel_base = "missing-relation-base";
    const RootTargetIdentity root{0, root_name};

    AurUpdateExecutionPreflight preflight =
        single_root_preflight(root_name);
    preflight.targets.push_back(
        independent_devel_requires_check_target(
            1, devel_name, devel_base));
    preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;

    BuildPlan& plan = *preflight.build_plan;
    plan.package_targets.insert(
        plan.package_targets.begin(),
        PlannedPackageTarget{
            devel_name,
            devel_base,
            {PackageRole::RuntimeDependency},
            {root}});
    plan.order.insert(
        plan.order.begin(), BuildPlanEntry{devel_base, {devel_name}});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        root_name,
        root_name,
        devel_name,
        PackageRole::RuntimeDependency,
        DependencyKind::Aur,
        devel_name,
        devel_base,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{ConsumerDependencyRequirement(
            devel_name, devel_name, std::nullopt)},
        ResolvedDependencyCandidate{AurResolvedDependencyCandidate{
            devel_name,
            devel_base,
            ObservedVersion::available(
                ObservedVersionSource::AurExactPackage,
                "1.0-1")}},
        ConstraintEvaluation::unconstrained()});
    return preflight;
}

AurUpdateExecutionPreflight ordered_multi_root_preflight() {
    const RootTargetIdentity root_a{0, "root-a"};
    const RootTargetIdentity root_b{1, "root-b"};

    BuildPlan plan;
    plan.root_targets = {root_a, root_b};
    plan.package_targets = {
        PlannedPackageTarget{
            "private-dependency",
            "private-dependency",
            {PackageRole::BuildDependency},
            {root_a}},
        PlannedPackageTarget{
            "shared-dependency",
            "shared-dependency",
            {PackageRole::RuntimeDependency},
            {root_a, root_b}},
        // root-bはroot-aのdependencyでもある。Root roleからreasonを
        // 再計算するとDependency-installed rootがExplicitへ誤昇格するfixture。
        PlannedPackageTarget{
            "root-b",
            "root-b",
            {PackageRole::Root, PackageRole::RuntimeDependency},
            {root_a, root_b}},
        PlannedPackageTarget{
            "root-a",
            "root-a",
            {PackageRole::Root},
            {root_a}},
    };
    plan.order = {
        BuildPlanEntry{
            "private-dependency", {"private-dependency"}},
        BuildPlanEntry{
            "shared-dependency", {"shared-dependency"}},
        BuildPlanEntry{"root-b", {"root-b"}},
        BuildPlanEntry{"root-a", {"root-a"}},
    };

    AurUpdateExecutionPreflight preflight;
    preflight.targets = {
        executable_target(
            0, 0, "root-a", DesiredInstallReason::Explicit,
            "root-a"),
        skipped_target(1, "skipped-root"),
        executable_target(
            2, 1, "root-b", DesiredInstallReason::Dependency,
            "root-b"),
    };
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateExecutionPreflight same_package_base_multiple_preflight() {
    const RootTargetIdentity dependency_installed_root{0, "split-runtime"};
    const RootTargetIdentity explicit_root{1, "split-explicit"};

    BuildPlan plan;
    plan.root_targets = {dependency_installed_root, explicit_root};
    plan.package_targets = {
        // split-runtimeはinstalled Dependencyのupdate rootであると同時に、
        // split-explicitから到達するruntime dependencyでもある。
        PlannedPackageTarget{
            "split-runtime",
            "split-suite",
            {PackageRole::Root, PackageRole::RuntimeDependency},
            {dependency_installed_root, explicit_root}},
        PlannedPackageTarget{
            "split-explicit",
            "split-suite",
            {PackageRole::Root},
            {explicit_root}},
    };
    plan.order.push_back(BuildPlanEntry{
        "split-suite", {"split-runtime", "split-explicit"}});

    AurUpdateExecutionPreflight preflight;
    preflight.targets = {
        executable_target(
            0, 0, "split-runtime",
            DesiredInstallReason::Dependency, "split-suite"),
        skipped_target(1, "skipped-between-split-roots"),
        executable_target(
            2, 1, "split-explicit",
            DesiredInstallReason::Explicit, "split-suite"),
    };
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateExecutionPreflight external_multiple_dependency_preflight() {
    const RootTargetIdentity application_root{0, "application"};

    BuildPlan plan;
    plan.root_targets.push_back(application_root);
    plan.package_targets = {
        PlannedPackageTarget{
            "split-runtime",
            "split-suite",
            {PackageRole::RuntimeDependency},
            {application_root}},
        PlannedPackageTarget{
            "split-tools",
            "split-suite",
            {PackageRole::BuildDependency},
            {application_root}},
        PlannedPackageTarget{
            "application",
            "application",
            {PackageRole::Root},
            {application_root}},
    };
    plan.order = {
        BuildPlanEntry{
            "split-suite", {"split-runtime", "split-tools"}},
        BuildPlanEntry{"application", {"application"}},
    };

    AurUpdateExecutionPreflight preflight;
    preflight.targets.push_back(executable_target(
        0, 0, "application", DesiredInstallReason::Explicit,
        "application"));
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateBuildUnitSelection build_unit_selection_with_external(
    const AurUpdateExecutionPreflight& preflight,
    std::size_t external_order_index,
    AurUpdateExternalSatisfactionAttribution external_satisfaction) {
    const BuildPlan& plan = preflight.build_plan.value();
    AurUpdateBuildUnitSelection selection;
    selection.entries.reserve(plan.order.size());

    std::size_t selected_execution_index = 0;
    for(std::size_t order_index = 0; order_index < plan.order.size();
        ++order_index) {
        const BuildPlanEntry& entry = plan.order[order_index];
        if(order_index == external_order_index) {
            selection.entries.push_back(AurUpdateBuildUnitSelectionEntry{
                order_index,
                entry.package_base,
                entry.package_names,
                AurUpdateBuildUnitSelectionStatus::
                    ExternallySatisfiedByExplicitSourcePackageBase,
                std::nullopt,
                std::move(external_satisfaction)});
            continue;
        }
        selection.entries.push_back(AurUpdateBuildUnitSelectionEntry{
            order_index,
            entry.package_base,
            entry.package_names,
            AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution,
            selected_execution_index,
            std::nullopt});
        ++selected_execution_index;
    }
    return selection;
}

AurUpdateExternalSatisfactionAttribution external_satisfaction(
    const std::string& package_base) {
    return AurUpdateExternalSatisfactionAttribution{
        {7, 9},
        {"source://shared-dependency"},
        std::nullopt,
        package_base};
}

const AurUpdatePreparationIssue& require_issue(
    const AurUpdateSourceBuildPreparation& preparation,
    AurUpdatePreparationReason reason,
    std::string_view context) {
    auto found = std::find_if(
        preparation.issues.begin(), preparation.issues.end(),
        [reason](const AurUpdatePreparationIssue& issue) {
            return issue.reason == reason;
        });
    if(found == preparation.issues.end()) {
        throw std::runtime_error(
            std::string(context) + ": expected issue is missing");
    }
    return *found;
}

void expect_no_external_preparation_boundary(const std::string& context) {
    expect(
        stub::strict_preference_read_history().empty(),
        context + ": strict preference reader was called");
    expect(
        stub::supported_options_guard_history().empty(),
        context + ": generic option guard was called");
    expect(
        stub::pkgdest_guard_history().empty(),
        context + ": generic PKGDEST guard was called");
    expect(
        stub::database_call_count() == 0,
        context + ": Pacman DB resolver was called");
}

void expect_result_invariant(
    const AurUpdateSourceBuildPreparation& preparation,
    const std::string& context) {
    const int state_count =
        static_cast<int>(preparation.is_prepared()) +
        static_cast<int>(preparation.is_noop()) +
        static_cast<int>(preparation.is_blocked());
    expect(state_count == 1, context + ": result invariant is broken");
    expect(
        !preparation.invocation.has_value() ||
            preparation.issues.empty(),
        context + ": partial invocation escaped with issues");
    if(!preparation.invocation.has_value()) return;

    const PreparedProductionSourceBuildInvocation& production_invocation =
        preparation.invocation->production_invocation_for_test();
    const auto& attributions =
        preparation.invocation->work_item_attributions();
    expect(
        !production_invocation.work_items.empty() &&
            attributions.size() ==
                production_invocation.work_items.size(),
        context + ": prepared correlation count differs");
    for(std::size_t index = 0; index < attributions.size(); ++index) {
        const auto& attribution = attributions[index];
        const auto& work_item = production_invocation.work_items[index];
        expect(
            attribution.invocation_work_item_index == index &&
                attribution.package_name ==
                    work_item.request.package_name &&
                attribution.package_base ==
                    work_item.request.checkout_name &&
                !attribution.affected_update_plan_indices.empty() &&
                !attribution.affected_roots.empty(),
            context + ": prepared correlation identity differs at " +
                std::to_string(index));
    }
}

void expect_blocked_reason(
    const AurUpdateSourceBuildPreparation& preparation,
    AurUpdatePreparationReason reason,
    const std::string& context) {
    expect_result_invariant(preparation, context);
    expect(preparation.is_blocked(), context + ": result was not blocked");
    expect(!preparation.invocation.has_value(), context + ": partial invocation escaped");
    static_cast<void>(require_issue(preparation, reason, context));
}

void expect_work_item(
    const ProductionSourceBuildWorkItem& work_item,
    const std::string& package_name,
    const SourceBuildEnvironment& expected_environment,
    DesiredInstallReason desired_reason,
    bool needed,
    const std::string& context) {
    expect(
        work_item.request.package_name == package_name,
        context + ": package_name differs");
    expect(
        work_item.request.checkout_name == package_name,
        context + ": checkout_name differs");
    expect(
        work_item.request.git_url ==
            "https://aur.archlinux.org/" + package_name + ".git",
        context + ": git_url differs");
    expect(
        same_environment(
            work_item.request.custom_environment,
            expected_environment),
        context + ": custom_environment differs");
    expect(
        work_item.request.empty_value_policy ==
            SourceEnvironmentEmptyValuePolicy::Omit,
        context + ": empty value policy differs");
    expect(work_item.request.needed == needed, context + ": needed differs");
    expect(
        !work_item.request.only_if_updated,
        context + ": only_if_updated differs");
    expect(
        !work_item.request.installed_snapshot.has_value(),
        context + ": installed snapshot was synthesized");
    expect(
        !work_item.request.update_baseline.has_value(),
        context + ": update baseline was synthesized");
    expect(
        work_item.required_targets.size() == 1,
        context + ": ordinary work item required target count differs");
    const RequiredPackageArtifactTarget& required_target =
        work_item.required_targets.front();
    expect(
        required_target.package_base == package_name &&
            required_target.package_name == package_name &&
            required_target.desired_reason == desired_reason,
        context + ": required target identity or reason differs");
    expect(
        work_item.required_target_provenance ==
                RequiredTargetProvenance::
                    AurBuildPlanProjection &&
            work_item.artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet,
        context + ": BuildPlan provenance or lifecycle intent differs");
    expect(
        !work_item.uses_system_update_baseline,
        context + ": system update baseline marker differs");
}

void expect_attribution(
    const AurUpdatePreparedWorkItemAttribution& attribution,
    std::size_t work_item_index,
    std::size_t build_plan_order_index,
    const std::string& package_name,
    const std::string& package_base,
    const std::vector<std::size_t>& affected_update_plan_indices,
    const std::vector<RootTargetIdentity>& affected_roots,
    const std::string& context) {
    expect(
        attribution.invocation_work_item_index == work_item_index,
        context + ": work item index differs");
    expect(
        attribution.build_plan_order_index == build_plan_order_index,
        context + ": BuildPlan order index differs");
    expect(
        attribution.package_name == package_name,
        context + ": package name differs");
    expect(
        attribution.package_base == package_base,
        context + ": PackageBase differs");
    expect(
        attribution.affected_update_plan_indices ==
            affected_update_plan_indices,
        context + ": affected update-plan indices differ");
    expect(
        attribution.affected_roots == affected_roots,
        context + ": affected roots differ");
}

void expect_event_kinds(
    const std::vector<stub::EventKind>& expected,
    const std::string& context) {
    const std::vector<stub::Event>& events = stub::event_history();
    expect(events.size() == expected.size(), context + ": event count differs");
    for(std::size_t index = 0; index < expected.size(); ++index) {
        expect(
            events[index].kind == expected[index],
            context + ": event order differs at " +
                std::to_string(index));
    }
}

void test_noop_and_blocking_preflight_short_circuit() {
    const AppConfig config;

    stub::reset();
    AurUpdateSourceBuildPreparation empty =
        prepare_aur_update_source_build_invocation(
            AurUpdateExecutionPreflight{}, false, config);
    expect_result_invariant(empty, "empty preflight");
    expect(empty.is_noop(), "Empty preflight was not a normal no-op");
    expect(empty.issues.empty(), "Empty preflight produced issues");
    expect_no_external_preparation_boundary("empty preflight");

    stub::reset();
    AurUpdateExecutionPreflight skip_only;
    skip_only.targets.push_back(skipped_target(0, "skip-only"));
    AurUpdateSourceBuildPreparation skipped =
        prepare_aur_update_source_build_invocation(
            skip_only, false, config);
    expect_result_invariant(skipped, "skip-only preflight");
    expect(skipped.is_noop(), "Skip-only preflight was not a normal no-op");
    expect(skipped.issues.empty(), "Normal skip leaked into preparation issues");
    expect_no_external_preparation_boundary("skip-only preflight");

    stub::reset();
    AurUpdateExecutionPreflight non_aur_skip_only;
    AurUpdateExecutionTarget non_aur_skip =
        skipped_target(0, "non-aur-skip");
    non_aur_skip.update.classification =
        AurUpdateClassification::NonAurForeign;
    non_aur_skip.update.aur_package.reset();
    non_aur_skip.skip_kind =
        AurUpdateExecutionSkipKind::NonAurForeign;
    non_aur_skip.issues = {
        AurUpdateExecutionIssue{
            AurUpdateExecutionReason::NonAurForeign,
            "non-aur-skip",
            std::nullopt,
            std::nullopt,
            "Package is not present in AUR."}};
    non_aur_skip_only.targets.push_back(std::move(non_aur_skip));
    AurUpdateSourceBuildPreparation non_aur_skipped =
        prepare_aur_update_source_build_invocation(
            non_aur_skip_only, false, config);
    expect_result_invariant(non_aur_skipped, "Non-AUR skip-only preflight");
    expect(
        non_aur_skipped.is_noop(),
        "NonAurForeign skip-only preflight was not a normal no-op");
    expect(
        non_aur_skipped.issues.empty(),
        "NonAurForeign normal skip leaked into preparation issues");
    expect_no_external_preparation_boundary(
        "Non-AUR skip-only preflight");

    stub::reset();
    AurUpdateExecutionPreflight blocked = single_root_preflight();
    blocked.targets.push_back(blocking_target(
        1,
        "blocked-root",
        AurUpdateExecutionTargetStatus::Unsupported,
        AurUpdateExecutionReason::SplitPackageSelectionRequired));
    AurUpdateSourceBuildPreparation blocking =
        prepare_aur_update_source_build_invocation(
            blocked, false, config);
    expect_blocked_reason(
        blocking,
        AurUpdatePreparationReason::BlockingPreflight,
        "blocking preflight");
    const AurUpdatePreparationIssue& typed = require_issue(
        blocking,
        AurUpdatePreparationReason::BlockingPreflight,
        "blocking preflight typed issue");
    expect(
        typed.preflight_issue.has_value() &&
            typed.preflight_issue->reason ==
                AurUpdateExecutionReason::
                    SplitPackageSelectionRequired,
        "Blocking preflight issue lost its typed source");
    expect(
        typed.affected_update_plan_indices ==
            std::vector<std::size_t>{1},
        "Blocking preflight issue attribution differs");
    expect_no_external_preparation_boundary("blocking preflight");
}

void test_skipped_preflight_inconsistencies_fail_closed() {
    const AppConfig config;

    stub::reset();
    AurUpdateExecutionPreflight skip_only;
    AurUpdateExecutionTarget invalid_skip =
        skipped_target(0, "invalid-skip-only");
    const AurUpdateExecutionIssue skip_only_issue =
        build_plan_inconsistent_issue("invalid-skip-only");
    invalid_skip.issues = {skip_only_issue};
    skip_only.targets.push_back(std::move(invalid_skip));
    AurUpdateSourceBuildPreparation skip_only_result =
        prepare_aur_update_source_build_invocation(
            skip_only, false, config);
    expect_blocked_reason(
        skip_only_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "skip-only target with blocking issue");
    const AurUpdatePreparationIssue& retained_skip_only_issue =
        require_issue(
            skip_only_result,
            AurUpdatePreparationReason::PreflightInconsistent,
            "skip-only typed preflight issue");
    expect(
        retained_skip_only_issue.preflight_issue.has_value() &&
            retained_skip_only_issue.preflight_issue->reason ==
                skip_only_issue.reason &&
            retained_skip_only_issue.preflight_issue->package_name ==
                skip_only_issue.package_name &&
            retained_skip_only_issue.preflight_issue->package_base ==
                skip_only_issue.package_base &&
            retained_skip_only_issue.preflight_issue
                    ->dependency_specification ==
                skip_only_issue.dependency_specification &&
            retained_skip_only_issue.preflight_issue->diagnostic ==
                skip_only_issue.diagnostic,
        "Skip-only blocker lost its typed preflight issue");
    expect(
        retained_skip_only_issue.affected_update_plan_indices ==
            std::vector<std::size_t>{0},
        "Skip-only blocker attribution differs");
    expect_no_external_preparation_boundary(
        "skip-only target with blocking issue");

    stub::reset();
    AurUpdateExecutionPreflight executable_and_invalid_skip =
        ordered_multi_root_preflight();
    const AurUpdateExecutionIssue mixed_issue =
        build_plan_inconsistent_issue("skipped-root");
    executable_and_invalid_skip.targets[1].issues.push_back(mixed_issue);
    AurUpdateSourceBuildPreparation mixed_result =
        prepare_aur_update_source_build_invocation(
            executable_and_invalid_skip, false, config);
    expect_blocked_reason(
        mixed_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "executable and invalid skipped target");
    const AurUpdatePreparationIssue& retained_mixed_issue = require_issue(
        mixed_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "mixed typed skipped issue");
    expect(
        retained_mixed_issue.preflight_issue.has_value() &&
            retained_mixed_issue.preflight_issue->reason ==
                mixed_issue.reason &&
            retained_mixed_issue.preflight_issue->package_name ==
                mixed_issue.package_name &&
            retained_mixed_issue.preflight_issue->package_base ==
                mixed_issue.package_base &&
            retained_mixed_issue.preflight_issue
                    ->dependency_specification ==
                mixed_issue.dependency_specification &&
            retained_mixed_issue.preflight_issue->diagnostic ==
                mixed_issue.diagnostic &&
            retained_mixed_issue.affected_update_plan_indices ==
                std::vector<std::size_t>{1},
        "Mixed skipped blocker was not retained exactly");
    expect_no_external_preparation_boundary(
        "executable and invalid skipped target");

    stub::reset();
    AurUpdateExecutionPreflight conflicting_normal_skip;
    AurUpdateExecutionTarget conflicting_target =
        skipped_target(0, "conflicting-normal-skip");
    conflicting_target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::NonAurForeign,
        "conflicting-normal-skip",
        std::nullopt,
        std::nullopt,
        "Conflicting normal skip reason."});
    conflicting_normal_skip.targets.push_back(
        std::move(conflicting_target));
    AurUpdateSourceBuildPreparation conflicting_result =
        prepare_aur_update_source_build_invocation(
            conflicting_normal_skip, false, config);
    expect_blocked_reason(
        conflicting_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "Skipped target with conflicting normal skip reasons");
    expect_no_external_preparation_boundary(
        "Skipped target with conflicting normal skip reasons");

    stub::reset();
    AurUpdateExecutionPreflight reasonless_skip;
    AurUpdateExecutionTarget reasonless_target =
        skipped_target(0, "reasonless-skip");
    reasonless_target.issues.clear();
    reasonless_skip.targets.push_back(std::move(reasonless_target));
    AurUpdateSourceBuildPreparation reasonless_result =
        prepare_aur_update_source_build_invocation(
            reasonless_skip, false, config);
    expect_blocked_reason(
        reasonless_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "Skipped target without normal skip issue");
    const AurUpdatePreparationIssue& reasonless_issue = require_issue(
        reasonless_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "reasonless skipped issue");
    expect(
        !reasonless_issue.preflight_issue.has_value() &&
            reasonless_issue.affected_update_plan_indices ==
                std::vector<std::size_t>{0},
        "Reasonless skipped target inconsistency differs");
    expect_no_external_preparation_boundary(
        "Skipped target without normal skip issue");
}

void test_executable_structure_failures() {
    const AppConfig config;

    stub::reset();
    AurUpdateExecutionPreflight missing_plan = single_root_preflight();
    missing_plan.build_plan.reset();
    AurUpdateSourceBuildPreparation missing_plan_result =
        prepare_aur_update_source_build_invocation(
            missing_plan, false, config);
    expect_blocked_reason(
        missing_plan_result,
        AurUpdatePreparationReason::BuildPlanMissing,
        "missing BuildPlan");
    expect_no_external_preparation_boundary("missing BuildPlan");

    stub::reset();
    AurUpdateExecutionPreflight empty_order = single_root_preflight();
    empty_order.build_plan->order.clear();
    AurUpdateSourceBuildPreparation empty_order_result =
        prepare_aur_update_source_build_invocation(
            empty_order, false, config);
    expect_blocked_reason(
        empty_order_result,
        AurUpdatePreparationReason::BuildPlanOrderEmpty,
        "empty BuildPlan order");
    expect_no_external_preparation_boundary("empty BuildPlan order");

    stub::reset();
    AurUpdateExecutionPreflight missing_reason = single_root_preflight();
    missing_reason.targets.front().desired_install_reason.reset();
    AurUpdateSourceBuildPreparation missing_reason_result =
        prepare_aur_update_source_build_invocation(
            missing_reason, false, config);
    expect_blocked_reason(
        missing_reason_result,
        AurUpdatePreparationReason::DesiredInstallReasonMissing,
        "missing desired install reason");
    const RootTargetIdentity expected_missing_reason_root{0, "single-root"};
    const AurUpdatePreparationIssue& missing_reason_issue = require_issue(
        missing_reason_result,
        AurUpdatePreparationReason::DesiredInstallReasonMissing,
        "missing desired install reason attribution");
    expect(
        missing_reason_issue.affected_update_plan_indices ==
                std::vector<std::size_t>{0} &&
            missing_reason_issue.affected_roots ==
                std::vector<RootTargetIdentity>{
                    expected_missing_reason_root} &&
            missing_reason_result.affected_roots ==
                std::vector<RootTargetIdentity>{
                    expected_missing_reason_root},
        "Missing desired install reason lost its valid root attribution");
    expect_no_external_preparation_boundary("missing desired install reason");

    stub::reset();
    AurUpdateExecutionPreflight executable_with_issue =
        single_root_preflight("executable-with-issue");
    const AurUpdateExecutionIssue retained_preflight_issue{
        AurUpdateExecutionReason::BuildPlanInconsistent,
        std::optional<std::string>{"executable-with-issue"},
        std::optional<std::string>{"executable-with-issue"},
        std::optional<std::string>{"dependency>=1"},
        "Executable target retained a typed blocker."};
    executable_with_issue.targets.front().issues.push_back(
        retained_preflight_issue);
    AurUpdateSourceBuildPreparation executable_issue_result =
        prepare_aur_update_source_build_invocation(
            executable_with_issue, false, config);
    expect_blocked_reason(
        executable_issue_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "executable target with preflight issue");
    const AurUpdatePreparationIssue& executable_issue = require_issue(
        executable_issue_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "executable target typed preflight issue");
    expect(
        executable_issue.preflight_issue.has_value() &&
            executable_issue.preflight_issue->reason ==
                retained_preflight_issue.reason &&
            executable_issue.preflight_issue->package_name ==
                retained_preflight_issue.package_name &&
            executable_issue.preflight_issue->package_base ==
                retained_preflight_issue.package_base &&
            executable_issue.preflight_issue
                    ->dependency_specification ==
                retained_preflight_issue
                    .dependency_specification &&
            executable_issue.preflight_issue->diagnostic ==
                retained_preflight_issue.diagnostic,
        "Executable target preflight issue was not retained exactly");
    expect(
        executable_issue.affected_update_plan_indices ==
            std::vector<std::size_t>{0},
        "Executable target preflight issue attribution differs");
    expect_no_external_preparation_boundary(
        "executable target with preflight issue");

    stub::reset();
    AurUpdateExecutionPreflight hidden_required_devel =
        single_root_preflight("executable-hidden-required-devel");
    AurUpdateExecutionIssue hidden_required_devel_issue;
    hidden_required_devel_issue.required_devel_target_blocker =
        AurUpdateRequiredDevelTargetBlocker{
            AurUpdateRequiredDevelTargetRelation::AurExactDependency,
            1,
            "required-devel-git",
            DevelRequiresCheckReason::SuffixCandidateOnly};
    hidden_required_devel.targets.front().issues.push_back(
        hidden_required_devel_issue);
    const AurUpdateSourceBuildPreparation hidden_required_devel_result =
        prepare_aur_update_source_build_invocation(
            hidden_required_devel, false, config);
    expect_blocked_reason(
        hidden_required_devel_result,
        AurUpdatePreparationReason::PreflightInconsistent,
        "executable target with hidden required-devel payload");
    const AurUpdatePreparationIssue& retained_hidden_required_devel =
        require_issue(
            hidden_required_devel_result,
            AurUpdatePreparationReason::PreflightInconsistent,
            "hidden required-devel payload");
    expect(
        !hidden_required_devel_result.invocation.has_value() &&
            retained_hidden_required_devel.preflight_issue.has_value() &&
            retained_hidden_required_devel.preflight_issue
                    ->required_devel_target_blocker ==
                hidden_required_devel_issue
                    .required_devel_target_blocker,
        "Executable hidden required-devel payload was not retained as a blocker");
    expect_no_external_preparation_boundary(
        "executable target with hidden required-devel payload");

    stub::reset();
    AurUpdateExecutionPreflight wrong_root = single_root_preflight();
    wrong_root.build_plan->root_targets.front().requested_name =
        "different-root";
    AurUpdateSourceBuildPreparation wrong_root_result =
        prepare_aur_update_source_build_invocation(
            wrong_root, false, config);
    expect_blocked_reason(
        wrong_root_result,
        AurUpdatePreparationReason::RootAttributionInconsistent,
        "root identity mismatch");
    expect_no_external_preparation_boundary("root identity mismatch");

    stub::reset();
    AurUpdateExecutionPreflight missing_target = single_root_preflight();
    missing_target.build_plan->package_targets.clear();
    AurUpdateSourceBuildPreparation missing_target_result =
        prepare_aur_update_source_build_invocation(
            missing_target, false, config);
    expect_blocked_reason(
        missing_target_result,
        AurUpdatePreparationReason::PackageTargetAttributionInconsistent,
        "missing root package target");
    expect_no_external_preparation_boundary("missing root package target");

    stub::reset();
    AurUpdateExecutionPreflight wrong_update_index = single_root_preflight();
    wrong_update_index.targets.front().update_plan_index = 7;
    AurUpdateSourceBuildPreparation wrong_update_index_result =
        prepare_aur_update_source_build_invocation(
            wrong_update_index, false, config);
    expect_blocked_reason(
        wrong_update_index_result,
        AurUpdatePreparationReason::RootAttributionInconsistent,
        "update-plan index/position mismatch");
    expect_no_external_preparation_boundary(
        "update-plan index/position mismatch");
}

void test_single_root_exact_work_item_and_snapshot() {
    stub::reset();
    const SourceBuildEnvironment expected_environment =
        environment({{"MAKEFLAGS", "-j4"}, {"EMPTY", ""}});
    stub::enqueue_source_preference_result(
        "single-root",
        loaded_preference(
            "single-root",
            expected_environment,
            {"first warning", "second warning"}));
    stub::set_database_paths(
        PacmanDatabasePaths{"/snapshot/root", "/snapshot/database"});

    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            single_root_preflight(), true, config);

    expect_result_invariant(preparation, "single-root prepared result");
    expect(preparation.is_prepared(), "Single root was not prepared");
    const PreparedProductionSourceBuildInvocation& production_invocation =
        preparation.invocation->production_invocation_for_test();
    expect(production_invocation.work_items.size() == 1, "Single root work count differs");
    expect_work_item(
        production_invocation.work_items.front(),
        "single-root",
        expected_environment,
        DesiredInstallReason::Explicit,
        true,
        "single-root exact work item");
    const auto& attributions =
        preparation.invocation->work_item_attributions();
    expect(attributions.size() == 1, "Single root attribution count differs");
    expect_attribution(
        attributions.front(), 0, 0, "single-root", "single-root", {0},
        {{0, "single-root"}}, "single-root exact attribution");
    expect(
        production_invocation.database_paths.root_dir ==
                fs::path("/snapshot/root") &&
            production_invocation.database_paths.db_path ==
                fs::path("/snapshot/database"),
        "Pacman DB snapshot differs");
    expect(stub::database_call_count() == 1, "Prepared invocation resolved DB more than once");
    expect(
        preparation.warnings.size() == 2 &&
            preparation.warnings[0].diagnostic == "first warning" &&
            preparation.warnings[1].diagnostic == "second warning",
        "Strict preference warning order differs");
    for(const auto& warning : preparation.warnings) {
        expect(
            warning.preference_name == "single-root" &&
                warning.entry_path ==
                    fs::path("/stub/preferences/single-root") &&
                warning.affected_update_plan_indices ==
                    std::vector<std::size_t>{0} &&
                warning.affected_roots ==
                    std::vector<RootTargetIdentity>{{0, "single-root"}},
            "Strict preference warning attribution differs");
    }

    expect_event_kinds(
        {
            stub::EventKind::StrictPreferenceRead,
            stub::EventKind::SeparatedInstallOptionsGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::PacmanDatabaseResolution,
        },
        "single-root preparation order");
    expect(
        stub::pkgdest_guard_history().size() == 2 &&
            stub::pkgdest_guard_history()[0].ordered_assignments.empty() &&
            same_environment(
                stub::pkgdest_guard_history()[1],
                expected_environment),
        "Generic PKGDEST guard inputs differ");
}

void test_build_plan_order_skip_exclusion_and_install_reasons() {
    stub::reset();
    const SourceBuildEnvironment shared_environment =
        environment({{"CFLAGS", "-O2"}});
    stub::enqueue_source_preference_result(
        "private-dependency", SourcePreferenceAbsent{});
    stub::enqueue_source_preference_result(
        "shared-dependency",
        loaded_preference(
            "shared-dependency",
            shared_environment,
            {"shared warning"}));
    stub::enqueue_source_preference_result(
        "root-b", SourcePreferenceAbsent{});
    stub::enqueue_source_preference_result(
        "root-a",
        loaded_preference(
            "root-a", environment({{"MAKEFLAGS", "-j2"}})));
    stub::set_database_paths(
        PacmanDatabasePaths{"/multi/root", "/multi/database"});

    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            ordered_multi_root_preflight(), false, config);

    expect_result_invariant(preparation, "ordered multi-root result");
    expect(preparation.is_prepared(), "Ordered multi-root invocation was blocked");
    const PreparedProductionSourceBuildInvocation& production_invocation =
        preparation.invocation->production_invocation_for_test();
    const auto& work_items = production_invocation.work_items;
    expect(work_items.size() == 4, "Skipped target changed work item count");
    expect_work_item(
        work_items[0],
        "private-dependency",
        SourceBuildEnvironment{},
        DesiredInstallReason::Dependency,
        false,
        "private non-root dependency");
    expect_work_item(
        work_items[1],
        "shared-dependency",
        shared_environment,
        DesiredInstallReason::Dependency,
        false,
        "shared dependency");
    expect_work_item(
        work_items[2],
        "root-b",
        SourceBuildEnvironment{},
        DesiredInstallReason::Dependency,
        false,
        "dependency-installed root overlap");
    expect_work_item(
        work_items[3],
        "root-a",
        environment({{"MAKEFLAGS", "-j2"}}),
        DesiredInstallReason::Explicit,
        false,
        "explicit root");

    expect(
        stub::strict_preference_read_history() ==
            std::vector<std::string>{
                "private-dependency", "shared-dependency",
                "root-b", "root-a"},
        "Strict reads did not preserve BuildPlan::order");
    expect(
        std::none_of(
            work_items.begin(), work_items.end(),
            [](const ProductionSourceBuildWorkItem& work_item) {
                return work_item.request.package_name ==
                       "skipped-root";
            }),
        "Skipped target was projected into a work item");
    expect(
        preparation.affected_update_targets.size() == 2 &&
            preparation.affected_update_targets[0].update_plan_index == 0 &&
            preparation.affected_update_targets[1].update_plan_index == 2,
        "Executable target attribution lost original update-plan indices");
    expect(
        preparation.affected_roots ==
            std::vector<RootTargetIdentity>{
                {0, "root-a"}, {1, "root-b"}},
        "Prepared root snapshot differs");
    expect(
        preparation.warnings.size() == 1 &&
            preparation.warnings.front().affected_update_plan_indices ==
                std::vector<std::size_t>{0, 2} &&
            preparation.warnings.front().affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "root-a"}, {1, "root-b"}},
        "Shared dependency warning attribution differs");
    const auto& attributions =
        preparation.invocation->work_item_attributions();
    expect(attributions.size() == 4, "Multi-root attribution count differs");
    expect_attribution(
        attributions[0], 0, 0, "private-dependency", "private-dependency",
        {0}, {{0, "root-a"}}, "private dependency attribution");
    expect_attribution(
        attributions[1], 1, 1, "shared-dependency", "shared-dependency",
        {0, 2}, {{0, "root-a"}, {1, "root-b"}},
        "shared dependency attribution");
    expect_attribution(
        attributions[2], 2, 2, "root-b", "root-b", {0, 2},
        {{0, "root-a"}, {1, "root-b"}}, "root-b attribution");
    expect_attribution(
        attributions[3], 3, 3, "root-a", "root-a", {0},
        {{0, "root-a"}}, "root-a attribution");
    expect(stub::database_call_count() == 1, "Multi-root invocation resolved DB per work item");
    expect(
        production_invocation.database_paths.root_dir ==
                fs::path("/multi/root") &&
            production_invocation.database_paths.db_path ==
                fs::path("/multi/database"),
        "Multi-root invocation did not retain one owned DB snapshot");
    expect_event_kinds(
        {
            stub::EventKind::StrictPreferenceRead,
            stub::EventKind::StrictPreferenceRead,
            stub::EventKind::StrictPreferenceRead,
            stub::EventKind::StrictPreferenceRead,
            stub::EventKind::SeparatedInstallOptionsGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::PacmanDatabaseResolution,
        },
        "multi-root read/validation/DB order");
}

void test_selected_repository_provider_is_attached_to_parent_build_unit() {
    stub::reset();
    stub::set_database_paths(
        PacmanDatabasePaths{"/provider/root", "/provider/database"});

    AurUpdateExecutionPreflight preflight = single_root_preflight();
    BuildPlan& plan = preflight.build_plan.value();
    ProvidedDependency selected_repository_provider =
        ProvidedDependency::from_repository(
            "extra", "selected-provider", "virtual-dependency",
            "virtual-dependency=2", "2.0-1");
    selected_repository_provider.package_base =
        "selected-provider-base";
    ProvidedDependency unique_repository_provider =
        ProvidedDependency::from_repository(
            "core", "unique-provider", "unique-virtual",
            "unique-virtual=1", "1.0-1");
    unique_repository_provider.package_base = "unique-provider-base";
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "single-root",
        "single-root",
        "virtual-dependency>=2",
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        selected_repository_provider,
        ProviderResolutionKind::UserSelected});
    // 同じproviderを別dependency categoryでも参照してもtransactionは重複させない。
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "single-root",
        "single-root",
        "virtual-dependency>=2",
        PackageRole::BuildDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        selected_repository_provider,
        ProviderResolutionKind::UserSelected});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "single-root",
        "single-root",
        "aur-virtual",
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        ProvidedDependency::from_aur(
            "aur-provider", "aur-provider-base", "aur-virtual",
            "aur-virtual=1", "1.0-1"),
        ProviderResolutionKind::UserSelected});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "single-root",
        "single-root",
        "unique-virtual",
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        unique_repository_provider,
        ProviderResolutionKind::Unique});

    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, false, config);

    expect_result_invariant(
        preparation, "selected repository provider preparation");
    expect(
        preparation.is_prepared(),
        "Selected repository provider blocked update preparation");
    const PreparedProductionSourceBuildInvocation& invocation =
        preparation.invocation->production_invocation_for_test();
    expect(
        invocation.work_items.size() == 1 &&
            invocation.work_items.front()
                    .selected_repository_providers ==
                std::vector<ProvidedDependency>{
                    selected_repository_provider},
        "Selected repository provider was not attached exactly once to its parent PackageBase");
    expect(
        invocation.selected_repository_providers ==
            std::vector<ProvidedDependency>{
                selected_repository_provider},
        "Selected repository provider was not retained by the generic invocation");
}

void test_same_package_base_projection_retains_exact_child_attribution() {
    stub::reset();
    const SourceBuildEnvironment expected_environment =
        environment({{"CXXFLAGS", "-O3"}});
    stub::enqueue_source_preference_result(
        "split-suite",
        loaded_preference("split-suite", expected_environment));
    const AppConfig config;
    const AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            same_package_base_multiple_preflight(), false, config);

    expect_result_invariant(
        preparation, "same-PackageBase multiple preparation");
    expect(
        preparation.is_prepared() && preparation.issues.empty(),
        "Same-PackageBase multiple work item was not prepared");
    expect(
        preparation.projected_build_units.size() == 1,
        "Same-PackageBase BuildPlan entry was not projected exactly once");
    const auto& build_unit = preparation.projected_build_units.front();
    expect(
        build_unit.build_plan_order_index == 0 &&
            build_unit.package_base == "split-suite",
        "Projected same-PackageBase build unit identity differs");
    expect(
        build_unit.required_target_attributions.size() == 2,
        "Projected build unit lost a required child attribution");

    const auto& dependency_child =
        build_unit.required_target_attributions[0];
    expect(
        dependency_child.required_target.package_base == "split-suite" &&
            dependency_child.required_target.package_name ==
                "split-runtime" &&
            dependency_child.required_target.desired_reason ==
                DesiredInstallReason::Dependency,
        "Dependency-installed split child identity or reason differs");
    expect(
        dependency_child.affected_update_plan_indices ==
                std::vector<std::size_t>{0, 2} &&
            dependency_child.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "split-runtime"},
                    {1, "split-explicit"}} &&
            dependency_child.roles ==
                std::vector<PackageRole>{
                    PackageRole::Root,
                    PackageRole::RuntimeDependency},
        "Dependency-installed split child attribution differs");

    const auto& explicit_child =
        build_unit.required_target_attributions[1];
    expect(
        explicit_child.required_target.package_base == "split-suite" &&
            explicit_child.required_target.package_name ==
                "split-explicit" &&
            explicit_child.required_target.desired_reason ==
                DesiredInstallReason::Explicit,
        "Explicit split child identity or reason differs");
    expect(
        explicit_child.affected_update_plan_indices ==
                std::vector<std::size_t>{2} &&
            explicit_child.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {1, "split-explicit"}} &&
            explicit_child.roles ==
                std::vector<PackageRole>{PackageRole::Root},
        "Explicit split child attribution differs");

    expect(
        build_unit.affected_update_plan_indices ==
                std::vector<std::size_t>{0, 2} &&
            build_unit.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "split-runtime"},
                    {1, "split-explicit"}},
        "Same-PackageBase build-unit attribution union differs");
    expect(
        stub::strict_preference_read_history() ==
            std::vector<std::string>{"split-suite"},
        "Multiple-child work item did not read PackageBase preference exactly once");

    const PreparedProductionSourceBuildInvocation& production_invocation =
        preparation.invocation->production_invocation_for_test();
    expect(
        production_invocation.work_items.size() == 1,
        "Same-PackageBase projection created multiple work items");
    const ProductionSourceBuildWorkItem& work_item =
        production_invocation.work_items.front();
    expect(
        work_item.request.package_name.empty() &&
            work_item.request.checkout_name == "split-suite" &&
            work_item.required_targets.size() == 2 &&
            work_item.required_targets[0].package_name ==
                "split-runtime" &&
            work_item.required_targets[0].desired_reason ==
                DesiredInstallReason::Dependency &&
            work_item.required_targets[1].package_name ==
                "split-explicit" &&
            work_item.required_targets[1].desired_reason ==
                DesiredInstallReason::Explicit,
        "Same-PackageBase work item lost ordered child identity or reason");
    expect(
        same_environment(
            work_item.request.custom_environment,
            expected_environment),
        "Same-PackageBase strict preference environment was not applied");
    const AurUpdatePreparedWorkItemAttribution& attribution =
        preparation.invocation->work_item_attributions().front();
    expect(
        attribution.package_name.empty() &&
            attribution.package_base == "split-suite" &&
            attribution.required_target_attributions.size() == 2 &&
            attribution.required_target_attributions[0]
                    .required_target.package_name ==
                dependency_child.required_target.package_name &&
            attribution.required_target_attributions[0]
                    .affected_update_plan_indices ==
                dependency_child.affected_update_plan_indices &&
            attribution.required_target_attributions[1]
                    .required_target.package_name ==
                explicit_child.required_target.package_name &&
            attribution.required_target_attributions[1]
                    .affected_update_plan_indices ==
                explicit_child.affected_update_plan_indices &&
            attribution.affected_update_plan_indices ==
                std::vector<std::size_t>{0, 2} &&
            attribution.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "split-runtime"},
                    {1, "split-explicit"}},
        "Same-PackageBase prepared attribution differs from projection");
    expect(
        stub::supported_options_guard_history() ==
                std::vector<bool>{false} &&
            stub::pkgdest_guard_history().size() == 2 &&
            stub::database_call_count() == 1,
        "Same-PackageBase preparation guard or DB snapshot count differs");
}

void test_incomplete_same_package_base_plan_has_no_projection() {
    stub::reset();
    AurUpdateExecutionPreflight preflight =
        same_package_base_multiple_preflight();
    preflight.build_plan->order.front().package_names.pop_back();

    const AppConfig config;
    const AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, false, config);

    expect_blocked_reason(
        preparation,
        AurUpdatePreparationReason::PackageTargetAttributionInconsistent,
        "incomplete same-PackageBase BuildPlan");
    expect(
        preparation.projected_build_units.empty(),
        "Incomplete BuildPlan escaped a partial projected build unit");
    const auto uncovered_target_issue = std::find_if(
        preparation.issues.begin(), preparation.issues.end(),
        [](const AurUpdatePreparationIssue& issue) {
            return issue.build_plan_projection_issue.has_value() &&
                   issue.build_plan_projection_issue->kind ==
                       BuildPlanArtifactTargetProjectionIssueKind::
                           UncoveredPlannedPackageTarget;
        });
    expect(
        uncovered_target_issue != preparation.issues.end() &&
            uncovered_target_issue->build_plan_projection_issue
                    ->package_name ==
                std::optional<std::string>{"split-explicit"} &&
            uncovered_target_issue->build_plan_projection_issue
                    ->package_target_indices ==
                std::vector<std::size_t>{1},
        "Preparation lost the typed uncovered-target projection payload");
    expect_no_external_preparation_boundary(
        "incomplete same-PackageBase BuildPlan");
}

void test_external_satisfaction_overlay_preserves_original_order() {
    stub::reset();
    stub::set_database_paths(
        PacmanDatabasePaths{"/external/root", "/external/database"});

    AurUpdateExecutionPreflight preflight = ordered_multi_root_preflight();
    const AurUpdateExternalSatisfactionAttribution expected_external =
        external_satisfaction("shared-dependency");
    const AurUpdateBuildUnitSelection selection =
        build_unit_selection_with_external(
            preflight, 1, expected_external);
    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, selection, false, config);

    expect_result_invariant(preparation, "external satisfaction result");
    expect(
        preparation.is_prepared(),
        "External dependency satisfaction blocked selected roots");
    expect(
        preparation.build_unit_selection == selection,
        "Preparation changed the build-unit selection snapshot");

    const PreparedProductionSourceBuildInvocation& production_invocation =
        preparation.invocation->production_invocation_for_test();
    expect(
        production_invocation.work_items.size() == 3,
        "External build unit retained an execution capability");
    expect(
        stub::strict_preference_read_history() ==
            std::vector<std::string>{
                "private-dependency", "root-b", "root-a"},
        "External build unit reached the strict preference reader");

    const auto& attributions =
        preparation.invocation->work_item_attributions();
    expect(attributions.size() == 3, "Selected attribution count differs");
    expect_attribution(
        attributions[0], 0, 0, "private-dependency",
        "private-dependency", {0}, {{0, "root-a"}},
        "selected dependency after filtering");
    expect_attribution(
        attributions[1], 1, 2, "root-b", "root-b", {0, 2},
        {{0, "root-a"}, {1, "root-b"}},
        "selected dependency-installed root after filtering");
    expect_attribution(
        attributions[2], 2, 3, "root-a", "root-a", {0},
        {{0, "root-a"}}, "selected root after filtering");

    expect(
        preparation.externally_satisfied_build_units.size() == 1,
        "External build-unit snapshot count differs");
    const AurUpdateExternallySatisfiedBuildUnit& external =
        preparation.externally_satisfied_build_units.front();
    expect(
        external.build_plan_order_index == 1 &&
            external.package_name == "shared-dependency" &&
            external.package_base == "shared-dependency" &&
            external.required_target_attributions.size() == 1 &&
            external.required_target_attributions.front()
                    .required_target.package_name ==
                "shared-dependency" &&
            external.affected_update_plan_indices ==
                std::vector<std::size_t>{0, 2} &&
            external.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "root-a"}, {1, "root-b"}} &&
            external.roles ==
                std::vector<PackageRole>{
                    PackageRole::RuntimeDependency} &&
            external.desired_install_reason ==
                DesiredInstallReason::Dependency &&
            external.external_satisfaction == expected_external,
        "External build-unit snapshot lost identity or attribution");
    expect(
        stub::database_call_count() == 1,
        "Filtered invocation did not retain one database snapshot");
}

void test_multiple_child_external_satisfaction_retains_child_snapshot() {
    stub::reset();
    AurUpdateExecutionPreflight preflight =
        external_multiple_dependency_preflight();
    const AurUpdateExternalSatisfactionAttribution expected_external =
        external_satisfaction("split-suite");
    const AurUpdateBuildUnitSelection selection =
        build_unit_selection_with_external(
            preflight, 0, expected_external);
    const AppConfig config;
    const AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, selection, false, config);

    expect_result_invariant(
        preparation, "multiple-child external satisfaction");
    expect(
        preparation.is_prepared() &&
            preparation.externally_satisfied_build_units.size() == 1,
        "Multiple-child external unit blocked selected root preparation");
    expect(
        stub::strict_preference_read_history() ==
            std::vector<std::string>{"application"},
        "External multiple-child unit reached source preference IO");

    const AurUpdateExternallySatisfiedBuildUnit& external =
        preparation.externally_satisfied_build_units.front();
    expect(
        external.build_plan_order_index == 0 &&
            external.package_name.empty() &&
            external.package_base == "split-suite" &&
            external.plan_package_names ==
                std::vector<std::string>{
                    "split-runtime", "split-tools"} &&
            external.required_target_attributions.size() == 2 &&
            external.affected_update_plan_indices ==
                std::vector<std::size_t>{0} &&
            external.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "application"}} &&
            external.roles.empty() &&
            !external.desired_install_reason.has_value() &&
            external.external_satisfaction == expected_external,
        "External multiple-child aggregate snapshot differs");
    expect(
        external.required_target_attributions[0]
                    .required_target.package_name ==
                "split-runtime" &&
            external.required_target_attributions[0]
                    .required_target.desired_reason ==
                DesiredInstallReason::Dependency &&
            external.required_target_attributions[0].roles ==
                std::vector<PackageRole>{
                    PackageRole::RuntimeDependency} &&
            external.required_target_attributions[1]
                    .required_target.package_name ==
                "split-tools" &&
            external.required_target_attributions[1]
                    .required_target.desired_reason ==
                DesiredInstallReason::Dependency &&
            external.required_target_attributions[1].roles ==
                std::vector<PackageRole>{
                    PackageRole::BuildDependency},
        "External multiple-child ordered attribution differs");
    expect(
        preparation.invocation->production_invocation_for_test()
                    .work_items.size() == 1 &&
            preparation.invocation->production_invocation_for_test()
                    .work_items.front()
                    .request.package_name ==
                "application",
        "External multiple-child unit retained an execution capability");
}

void test_invalid_selection_and_external_root_stop_before_io() {
    const AppConfig config;

    stub::reset();
    AurUpdateExecutionPreflight missing_selection_preflight =
        single_root_preflight("missing-selection");
    const AurUpdateSourceBuildPreparation missing_selection =
        prepare_aur_update_source_build_invocation(
            missing_selection_preflight,
            AurUpdateBuildUnitSelection{}, false, config);
    expect_blocked_reason(
        missing_selection,
        AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
        "missing selection entry");
    expect_no_external_preparation_boundary("missing selection entry");

    stub::reset();
    AurUpdateExecutionPreflight invalid_external_preflight =
        single_root_preflight("invalid-external");
    AurUpdateBuildUnitSelection invalid_external =
        build_unit_selection_with_external(
            invalid_external_preflight, 0,
            external_satisfaction("wrong-package-base"));
    const AurUpdateSourceBuildPreparation invalid_external_result =
        prepare_aur_update_source_build_invocation(
            invalid_external_preflight, invalid_external, false,
            config);
    expect_blocked_reason(
        invalid_external_result,
        AurUpdatePreparationReason::ExternalSatisfactionInconsistent,
        "invalid external attribution");
    expect_no_external_preparation_boundary("invalid external attribution");

    stub::reset();
    AurUpdateExecutionPreflight external_root_preflight =
        single_root_preflight("external-root");
    const AurUpdateBuildUnitSelection external_root_selection =
        build_unit_selection_with_external(
            external_root_preflight, 0,
            external_satisfaction("external-root"));
    const AurUpdateSourceBuildPreparation external_root_result =
        prepare_aur_update_source_build_invocation(
            external_root_preflight, external_root_selection, false,
            config);
    expect_blocked_reason(
        external_root_result,
        AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
        "external executable root");
    expect(
        external_root_result.externally_satisfied_build_units.size() == 1,
        "Blocked external root lost its known satisfaction snapshot");
    expect_no_external_preparation_boundary("external executable root");
}

void expect_global_package_root_attribution_blocker(
    const AurUpdateSourceBuildPreparation& preparation,
    const std::string& package_name,
    const std::string& context) {
    expect_blocked_reason(
        preparation,
        AurUpdatePreparationReason::PackageTargetAttributionInconsistent,
        context);
    expect(
        preparation.issues.size() == 1,
        context + ": unexpected additional issues were produced");
    const AurUpdatePreparationIssue& issue = preparation.issues.front();
    expect(
        issue.package_name == package_name &&
            issue.package_base == package_name,
        context + ": package identity differs");
    expect(
        issue.affected_update_plan_indices ==
            std::vector<std::size_t>{0, 2},
        context + ": partial update target attribution escaped");
    expect(
        issue.affected_roots ==
            std::vector<RootTargetIdentity>{
                {0, "root-a"}, {1, "root-b"}},
        context + ": affected roots are not the exact global snapshot");
    expect(
        preparation.affected_roots ==
            std::vector<RootTargetIdentity>{
                {0, "root-a"}, {1, "root-b"}},
        context + ": preparation root snapshot is not exact");
    expect(
        issue.diagnostic.find("cannot be attributed exactly") !=
            std::string::npos,
        context + ": diagnostic does not identify attribution failure");
    expect(
        preparation.affected_update_targets.size() == 2 &&
            preparation.affected_update_targets[0]
                    .update_plan_index == 0 &&
            preparation.affected_update_targets[1]
                    .update_plan_index == 2,
        context + ": global executable target snapshot differs");
    expect_no_external_preparation_boundary(context);
}

void test_unknown_package_root_attribution_is_global() {
    const AppConfig config;
    const RootTargetIdentity unknown_root{9, "unknown-root"};

    stub::reset();
    AurUpdateExecutionPreflight shared_dependency =
        ordered_multi_root_preflight();
    shared_dependency.build_plan->package_targets[1].roots.push_back(
        unknown_root);
    AurUpdateSourceBuildPreparation shared_dependency_result =
        prepare_aur_update_source_build_invocation(
            shared_dependency, false, config);
    expect_global_package_root_attribution_blocker(
        shared_dependency_result,
        "shared-dependency",
        "shared dependency with unknown root");
    expect(
        std::find(
            shared_dependency_result.issues.front()
                .affected_roots.begin(),
            shared_dependency_result.issues.front()
                .affected_roots.end(),
            unknown_root) ==
            shared_dependency_result.issues.front()
                .affected_roots.end(),
        "Unknown raw root escaped as an exact affected root");

    stub::reset();
    AurUpdateExecutionPreflight missing_order_entry =
        ordered_multi_root_preflight();
    missing_order_entry.build_plan->package_targets.push_back(
        PlannedPackageTarget{
            "unordered-dependency",
            "unordered-dependency",
            {PackageRole::BuildDependency},
            {{0, "root-a"}, unknown_root}});
    AurUpdateSourceBuildPreparation missing_order_result =
        prepare_aur_update_source_build_invocation(
            missing_order_entry, false, config);
    expect_global_package_root_attribution_blocker(
        missing_order_result,
        "unordered-dependency",
        "unordered dependency with unknown root");
    expect(
        missing_order_result.issues.front().diagnostic.find(
            "must occur exactly once in BuildPlan execution order") !=
            std::string::npos,
        "Order-count attribution failure lost its primary diagnostic");
}

void expect_ignore_preference_boundary(
    const AurUpdateSourceBuildPreparation& preparation,
    std::size_t expected_work_item_count,
    const std::string& context) {
    expect_result_invariant(preparation, context);
    expect(
        preparation.is_prepared() && preparation.issues.empty() &&
            preparation.warnings.empty(),
        context + ": Ignore did not produce a clean prepared result");
    expect(
        preparation.invocation.has_value(),
        context + ": Ignore lost the prepared invocation");

    const PreparedProductionSourceBuildInvocation& invocation =
        preparation.invocation->production_invocation_for_test();
    expect(
        invocation.work_items.size() == expected_work_item_count,
        context + ": Ignore changed work-item count");
    for(const ProductionSourceBuildWorkItem& work_item :
        invocation.work_items) {
        expect(
            work_item.request.custom_environment.ordered_assignments.empty(),
            context + ": Ignore retained a preference-derived environment");
    }
    expect(
        stub::strict_preference_read_history().empty(),
        context + ": Ignore reached the strict preference reader");
    expect(
        stub::source_preference_directory_snapshot_call_count() == 0,
        context + ": Ignore enumerated the preference directory");
    expect(
        stub::supported_options_guard_history() ==
                std::vector<bool>{false} &&
            stub::database_call_count() == 1 &&
            stub::reviewed_state_preflight_call_count() ==
                expected_work_item_count,
        context + ": Ignore skipped a generic preparation authority");
    expect(
        stub::pkgdest_guard_history().size() ==
                expected_work_item_count + 1 &&
            std::all_of(
                stub::pkgdest_guard_history().begin(),
                stub::pkgdest_guard_history().end(),
                [](const SourceBuildEnvironment& environment) {
                    return environment.ordered_assignments.empty();
                }),
        context + ": Ignore changed PKGDEST guard coverage or input");
}

void test_independent_devel_requires_check_preparation_semantics() {
    const AppConfig config;

    stub::reset();
    AurUpdateExecutionPreflight requires_check_only;
    requires_check_only.targets.push_back(
        independent_devel_requires_check_target(
            0, "independent-only-git", "independent-only-base-git"));
    requires_check_only.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    const AurUpdateSourceBuildPreparation noop =
        ::prepare_aur_update_source_build_invocation(
            requires_check_only,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_result_invariant(noop, "independent RequiresCheck-only result");
    expect(
        noop.is_noop() && noop.issues.empty() &&
            noop.affected_update_targets.empty() &&
            !noop.invocation.has_value() &&
            noop.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::SkipIndependentTarget},
        "Independent RequiresCheck-only target was not a non-blocking no-op");
    expect_no_external_preparation_boundary(
        "independent RequiresCheck-only result");

    stub::reset();
    AurUpdateExecutionPreflight mixed =
        single_root_preflight("normal-update");
    mixed.targets.push_back(
        independent_devel_requires_check_target(
            1, "independent-mixed-git", "independent-mixed-base-git"));
    mixed.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    const AurUpdateSourceBuildPreparation prepared =
        ::prepare_aur_update_source_build_invocation(
            mixed, DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_result_invariant(
        prepared, "mixed independent RequiresCheck result");
    expect(
        prepared.is_prepared() && prepared.issues.empty() &&
            prepared.affected_update_targets.size() == 1 &&
            prepared.affected_update_targets.front().update_plan_index == 0 &&
            prepared.invocation.has_value() &&
            prepared.invocation->production_invocation_for_test()
                    .work_items.size() == 1 &&
            prepared.invocation->production_invocation_for_test()
                    .work_items.front()
                    .request.package_name == "normal-update",
        "Independent RequiresCheck target entered the mixed mutation candidate set");
}

void test_missing_required_devel_relation_blocks_before_io() {
    stub::reset();
    const AppConfig config;
    const AurUpdateSourceBuildPreparation preparation =
        ::prepare_aur_update_source_build_invocation(
            missing_required_devel_relation_preflight(),
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, false, config);

    expect_result_invariant(
        preparation, "missing required-devel relation");
    expect(
        preparation.is_blocked() && !preparation.invocation.has_value(),
        "Missing required-devel relation created an execution capability");
    require_issue(
        preparation,
        AurUpdatePreparationReason::
            DevelRequiresCheckPolicyInconsistent,
        "missing required-devel relation");
    expect_no_external_preparation_boundary(
        "missing required-devel relation");
    expect(
        stub::source_preference_directory_snapshot_call_count() == 0 &&
            stub::reviewed_state_preflight_call_count() == 0,
        "Missing required-devel relation crossed a pre-mutation authority");
}

void test_requires_check_policy_snapshot_mismatch_fails_before_io() {
    const AppConfig config;

    const auto expect_policy_block = [&config](
                                         AurUpdateExecutionPreflight preflight,
                                         DevelRequiresCheckPolicy policy,
                                         const std::string& context) {
        stub::reset();
        const AurUpdateSourceBuildPreparation preparation =
            ::prepare_aur_update_source_build_invocation(
                preflight, policy,
                SavedSourcePreferencePolicy::Ignore, false, config);
        expect(
            preparation.is_blocked() &&
                !preparation.is_noop() &&
                preparation.issues.size() == 1 &&
                preparation.issues.front().reason ==
                    AurUpdatePreparationReason::
                        DevelRequiresCheckPolicyInconsistent,
            context + ": malformed policy snapshot was not blocked");
        expect_no_external_preparation_boundary(context);
    };

    expect_policy_block(
        single_root_preflight("missing-policy"),
        DevelRequiresCheckPolicy::BlockOperation,
        "missing RequiresCheck policy");

    AurUpdateExecutionPreflight mismatched =
        single_root_preflight("mismatched-policy");
    mismatched.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    expect_policy_block(
        std::move(mismatched),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "mismatched RequiresCheck policy");

    AurUpdateExecutionPreflight unknown =
        single_root_preflight("unknown-policy");
    unknown.devel_requires_check_policy =
        static_cast<DevelRequiresCheckPolicy>(-1);
    expect_policy_block(
        std::move(unknown),
        static_cast<DevelRequiresCheckPolicy>(-1),
        "unknown RequiresCheck policy");

    const auto expect_independent_shape_block =
        [&expect_policy_block](
            AurUpdateExecutionTarget target,
            DevelRequiresCheckPolicy policy,
            const std::string& context) {
            AurUpdateExecutionPreflight preflight;
            preflight.targets.push_back(std::move(target));
            preflight.devel_requires_check_policy = policy;
            expect_policy_block(
                std::move(preflight), policy, context);
        };

    expect_independent_shape_block(
        independent_devel_requires_check_target(
            0, "block-policy-git"),
        DevelRequiresCheckPolicy::BlockOperation,
        "BlockOperation independent RequiresCheck skip");

    AurUpdateExecutionTarget missing_reason =
        independent_devel_requires_check_target(
            0, "missing-reason-git");
    missing_reason.issues.front().devel_requires_check_reason.reset();
    expect_independent_shape_block(
        std::move(missing_reason),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip without exact reason");

    AurUpdateExecutionTarget missing_classification =
        independent_devel_requires_check_target(
            0, "missing-classification-git");
    missing_classification.update.devel_classification.reset();
    expect_independent_shape_block(
        std::move(missing_classification),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip without producer classification");

    AurUpdateExecutionTarget base_evidence_drift =
        independent_devel_requires_check_target(
            0, "base-evidence-drift-git");
    base_evidence_drift.update.devel_classification =
        DevelPackageClassification::classify(
            DevelPackageSuffixEvidence::classify(
                "different-base-git",
                {"base-evidence-drift-git"}));
    expect_independent_shape_block(
        std::move(base_evidence_drift),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with PackageBase evidence drift");

    AurUpdateExecutionTarget child_evidence_drift =
        independent_devel_requires_check_target(
            0, "child-evidence-drift-git");
    child_evidence_drift.update.devel_classification =
        DevelPackageClassification::classify(
            DevelPackageSuffixEvidence::classify(
                "child-evidence-drift-git",
                {"different-child-git"}));
    expect_independent_shape_block(
        std::move(child_evidence_drift),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with child evidence drift");

    AurUpdateExecutionTarget reason_evidence_drift =
        independent_devel_requires_check_target(
            0, "reason-evidence-drift-git");
    reason_evidence_drift.update.devel_assessment =
        DevelUpdateAssessment::requires_check(
            DevelRequiresCheckReason::NoAuthoritativeBuildProvenance);
    reason_evidence_drift.issues.front().devel_requires_check_reason =
        DevelRequiresCheckReason::NoAuthoritativeBuildProvenance;
    expect_independent_shape_block(
        std::move(reason_evidence_drift),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with reason evidence drift");

    AurUpdateExecutionTarget forged_assessment =
        independent_devel_requires_check_target(
            0, "forged-assessment-git");
    forged_assessment.update.devel_assessment =
        DevelUpdateAssessment::not_applicable();
    expect_independent_shape_block(
        std::move(forged_assessment),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with forged assessment");

    AurUpdateExecutionTarget forged_classification =
        independent_devel_requires_check_target(
            0, "forged-classification-git");
    forged_classification.update.classification =
        AurUpdateClassification::UpdateAvailable;
    forged_classification.update.aur_package->version_relation =
        AurVersionRelation::NewerThanInstalled;
    expect_independent_shape_block(
        std::move(forged_classification),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with forged classification");

    AurUpdateExecutionTarget rooted_skip =
        independent_devel_requires_check_target(
            0, "rooted-independent-git");
    rooted_skip.build_plan_root_index = 0;
    expect_independent_shape_block(
        std::move(rooted_skip),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with BuildPlan root");

    AurUpdateExecutionTarget install_candidate_skip =
        independent_devel_requires_check_target(
            0, "install-candidate-independent-git");
    install_candidate_skip.desired_install_reason =
        DesiredInstallReason::Explicit;
    expect_independent_shape_block(
        std::move(install_candidate_skip),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with install reason");

    AurUpdateExecutionTarget contradictory_skip =
        independent_devel_requires_check_target(
            0, "contradictory-independent-git");
    AurUpdateRequiredDevelTargetBlocker required_blocker{
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        0,
        "contradictory-independent-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    required_blocker.dependency_edge_index = 0;
    required_blocker.package_base =
        "contradictory-independent-git";
    required_blocker.roles = {PackageRole::RuntimeDependency};
    contradictory_skip.issues.front().required_devel_target_blocker =
        std::move(required_blocker);
    expect_independent_shape_block(
        std::move(contradictory_skip),
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        "independent RequiresCheck skip with required blocker");
}

void test_ignore_saved_source_preferences_without_io_or_environment() {
    const AppConfig config;

    stub::reset();
    stub::enqueue_source_preference_result(
        "ignore-loaded",
        loaded_preference(
            "ignore-loaded",
            environment({{"CFLAGS", "-O3"}}),
            {"loaded warning must remain unread"}));
    const AurUpdateSourceBuildPreparation loaded =
        ::prepare_aur_update_source_build_invocation(
            with_block_operation_policy(
                single_root_preflight("ignore-loaded")),
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_ignore_preference_boundary(
        loaded, 1, "Ignore singular loaded preference");
    expect_work_item(
        loaded.invocation->production_invocation_for_test()
            .work_items.front(),
        "ignore-loaded", SourceBuildEnvironment{},
        DesiredInstallReason::Explicit, false,
        "Ignore singular loaded identity");

    struct FailureCase {
        std::string name;
        SourcePreferenceFailureKind kind;
        std::optional<fs::file_type> observed_file_type;
    };
    const std::vector<FailureCase> failures = {
        {"ignore-invalid", SourcePreferenceFailureKind::UnsupportedFileType,
         fs::file_type::symlink},
        {"ignore-read-failure", SourcePreferenceFailureKind::ReadFailed,
         std::nullopt},
    };
    for(const FailureCase& failure : failures) {
        stub::reset();
        stub::enqueue_source_preference_result(
            failure.name,
            preference_failure(
                failure.name, failure.kind,
                failure.observed_file_type));
        const AurUpdateSourceBuildPreparation preparation =
            ::prepare_aur_update_source_build_invocation(
                with_block_operation_policy(
                    single_root_preflight(failure.name)),
                DevelRequiresCheckPolicy::BlockOperation,
                SavedSourcePreferencePolicy::Ignore, false, config);
        expect_ignore_preference_boundary(
            preparation, 1, failure.name);
    }

    stub::reset();
    stub::enqueue_source_preference_result(
        "ignore-split-child", SourcePreferenceAbsent{});
    stub::enqueue_source_preference_result(
        "ignore-split-base",
        loaded_preference(
            "ignore-split-base",
            environment({{"MAKEFLAGS", "-j12"}})));
    const AurUpdateSourceBuildPreparation split =
        ::prepare_aur_update_source_build_invocation(
            with_block_operation_policy(single_root_preflight(
                "ignore-split-child", DesiredInstallReason::Explicit,
                "ignore-split-base")),
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_ignore_preference_boundary(
        split, 1, "Ignore split-child fallback candidate");
    const ProductionSourceBuildWorkItem& split_work_item =
        split.invocation->production_invocation_for_test()
            .work_items.front();
    expect(
        split_work_item.request.package_name == "ignore-split-child" &&
            split_work_item.request.checkout_name ==
                "ignore-split-base" &&
            split_work_item.required_targets.size() == 1 &&
            split_work_item.required_targets.front().package_name ==
                "ignore-split-child" &&
            split_work_item.required_targets.front().package_base ==
                "ignore-split-base" &&
            split_work_item.required_targets.front().desired_reason ==
                DesiredInstallReason::Explicit,
        "Ignore changed split child, PackageBase, or install reason");

    stub::reset();
    stub::enqueue_source_preference_result(
        "split-suite",
        loaded_preference(
            "split-suite",
            environment({{"CXXFLAGS", "-march=native"}})));
    const AurUpdateSourceBuildPreparation multiple =
        ::prepare_aur_update_source_build_invocation(
            with_block_operation_policy(
                same_package_base_multiple_preflight()),
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_ignore_preference_boundary(
        multiple, 1, "Ignore same-PackageBase multiple child");
    const ProductionSourceBuildWorkItem& multiple_work_item =
        multiple.invocation->production_invocation_for_test()
            .work_items.front();
    expect(
        multiple_work_item.request.package_name.empty() &&
            multiple_work_item.request.checkout_name == "split-suite" &&
            multiple_work_item.required_targets.size() == 2 &&
            multiple_work_item.required_targets[0].package_name ==
                "split-runtime" &&
            multiple_work_item.required_targets[0].desired_reason ==
                DesiredInstallReason::Dependency &&
            multiple_work_item.required_targets[1].package_name ==
                "split-explicit" &&
            multiple_work_item.required_targets[1].desired_reason ==
                DesiredInstallReason::Explicit,
        "Ignore changed multi-child identity or install reasons");
}

void test_ignore_preserves_preflight_and_generic_safety_authorities() {
    struct BlockingCase {
        std::string name;
        AurUpdateExecutionReason reason;
    };
    const std::vector<BlockingCase> blocking_cases = {
        {"ambiguous-provider", AurUpdateExecutionReason::AmbiguousProvider},
        {"conflict-replaces",
         AurUpdateExecutionReason::ConflictsOrReplacesUnresolved},
        {"requires-check", AurUpdateExecutionReason::DevelRequiresCheck},
        {"identity-mismatch", AurUpdateExecutionReason::PackageBaseMismatch},
    };
    const AppConfig config;
    for(const BlockingCase& blocking_case : blocking_cases) {
        stub::reset();
        AurUpdateExecutionPreflight preflight =
            single_root_preflight("ignore-safety-root");
        preflight.targets.push_back(blocking_target(
            1, blocking_case.name,
            AurUpdateExecutionTargetStatus::Unsupported,
            blocking_case.reason));
        const AurUpdateSourceBuildPreparation preparation =
            ::prepare_aur_update_source_build_invocation(
                with_block_operation_policy(preflight),
                DevelRequiresCheckPolicy::BlockOperation,
                SavedSourcePreferencePolicy::Ignore, false, config);
        const AurUpdatePreparationIssue& issue = require_issue(
            preparation, AurUpdatePreparationReason::BlockingPreflight,
            blocking_case.name);
        expect(
            issue.preflight_issue.has_value() &&
                issue.preflight_issue->reason == blocking_case.reason,
            blocking_case.name +
                ": Ignore changed the typed preflight blocker");
        expect(
            stub::strict_preference_read_history().empty() &&
                stub::source_preference_directory_snapshot_call_count() ==
                    0 &&
                stub::database_call_count() == 0 &&
                stub::reviewed_state_preflight_call_count() == 0,
            blocking_case.name +
                ": Ignore bypassed the preflight short-circuit boundary");
    }

    stub::reset();
    stub::enqueue_source_preference_result(
        "ignore-reviewed-safety",
        preference_failure(
            "ignore-reviewed-safety",
            SourcePreferenceFailureKind::ReadFailed));
    stub::fail_reviewed_state_preflight_on_call(
        1, "scripted reviewed-source safety failure");
    const AurUpdateSourceBuildPreparation reviewed =
        ::prepare_aur_update_source_build_invocation(
            with_block_operation_policy(
                single_root_preflight("ignore-reviewed-safety")),
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_blocked_reason(
        reviewed,
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        "Ignore reviewed-source safety");
    expect(
        stub::strict_preference_read_history().empty() &&
            stub::source_preference_directory_snapshot_call_count() == 0 &&
            stub::reviewed_state_preflight_call_count() == 1 &&
            stub::database_call_count() == 0 &&
            !stub::pkgdest_guard_history().empty(),
        "Ignore skipped or reordered downstream reviewed/artifact safety");

    stub::reset();
    stub::fail_reviewed_state_preflight_on_call(
        1, "scripted mixed reviewed-source safety failure");
    AurUpdateExecutionPreflight mixed_reviewed =
        single_root_preflight("mixed-reviewed-safety");
    mixed_reviewed.targets.push_back(
        independent_devel_requires_check_target(
            1, "mixed-reviewed-independent-git"));
    mixed_reviewed.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    const AurUpdateSourceBuildPreparation mixed_reviewed_result =
        ::prepare_aur_update_source_build_invocation(
            mixed_reviewed,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, false, config);
    expect_blocked_reason(
        mixed_reviewed_result,
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        "Independent RequiresCheck with reviewed-source safety");
    expect(
        mixed_reviewed_result.affected_update_targets.size() == 1 &&
            mixed_reviewed_result.affected_update_targets.front()
                    .update_plan_index == 0 &&
            stub::strict_preference_read_history().empty() &&
            stub::source_preference_directory_snapshot_call_count() == 0 &&
            stub::reviewed_state_preflight_call_count() == 1 &&
            stub::database_call_count() == 0,
        "Independent RequiresCheck suppressed or entered reviewed-source safety work");
}

void test_strict_absent_empty_valid_and_warning_results() {
    const AppConfig config;

    stub::reset();
    AurUpdateSourceBuildPreparation absent =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("absent-root"), false, config);
    expect(absent.is_prepared(), "Absent preference did not select an empty environment");
    expect(
        absent.invocation->production_invocation_for_test()
            .work_items.front()
            .request.custom_environment.ordered_assignments.empty(),
        "Absent preference synthesized assignments");

    stub::reset();
    stub::enqueue_source_preference_result(
        "empty-root", loaded_preference("empty-root"));
    AurUpdateSourceBuildPreparation empty =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("empty-root"), false, config);
    expect(empty.is_prepared(), "Loaded empty preference was treated as absent/failure");
    expect(empty.warnings.empty(), "Loaded empty preference synthesized warnings");

    stub::reset();
    const SourceBuildEnvironment valid_environment =
        environment({{"CC", "clang"}});
    stub::enqueue_source_preference_result(
        "valid-root",
        loaded_preference("valid-root", valid_environment));
    AurUpdateSourceBuildPreparation valid =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("valid-root"), false, config);
    expect(valid.is_prepared(), "Valid strict preference was rejected");
    expect(
        same_environment(
            valid.invocation->production_invocation_for_test()
                .work_items.front()
                .request.custom_environment,
            valid_environment),
        "Valid strict preference environment differs");

    stub::reset();
    stub::enqueue_source_preference_result(
        "malformed-only",
        loaded_preference(
            "malformed-only", {},
            {"line 1 is malformed", "line 2 is malformed"}));
    AurUpdateSourceBuildPreparation malformed_only =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("malformed-only"), false, config);
    expect(malformed_only.is_prepared(), "Malformed-only loaded preference was rejected");
    expect(
        malformed_only.warnings.size() == 2 &&
            malformed_only.warnings[0].diagnostic ==
                "line 1 is malformed" &&
            malformed_only.warnings[1].diagnostic ==
                "line 2 is malformed",
        "Malformed-only warning order differs");
}

void test_strict_typed_failures_are_not_flattened() {
    struct FailureCase {
        std::string name;
        SourcePreferenceFailureKind kind;
        std::optional<fs::file_type> observed_file_type;
    };
    const std::vector<FailureCase> cases = {
        {"status-failure", SourcePreferenceFailureKind::StatusUnavailable, std::nullopt},
        {"open-failure", SourcePreferenceFailureKind::OpenFailed, std::nullopt},
        {"read-failure", SourcePreferenceFailureKind::ReadFailed, std::nullopt},
        {"symlink-failure", SourcePreferenceFailureKind::UnsupportedFileType, fs::file_type::symlink},
        {"directory-failure", SourcePreferenceFailureKind::UnsupportedFileType, fs::file_type::directory},
        {"fifo-failure", SourcePreferenceFailureKind::UnsupportedFileType, fs::file_type::fifo},
    };
    const AppConfig config;

    for(const auto& test_case : cases) {
        stub::reset();
        const SourcePreferenceFailure scripted = preference_failure(
            test_case.name,
            test_case.kind,
            test_case.observed_file_type);
        stub::enqueue_source_preference_result(
            test_case.name, scripted);

        AurUpdateSourceBuildPreparation preparation =
            prepare_aur_update_source_build_invocation(
                single_root_preflight(test_case.name), false, config);
        expect_blocked_reason(
            preparation,
            AurUpdatePreparationReason::SourcePreferenceUnavailable,
            test_case.name);
        const AurUpdatePreparationIssue& issue = require_issue(
            preparation,
            AurUpdatePreparationReason::SourcePreferenceUnavailable,
            test_case.name);
        expect(
            issue.source_preference_failure.has_value(),
            test_case.name + ": typed failure is missing");
        expect(
            issue.source_preference_failure->kind == test_case.kind &&
                issue.source_preference_failure->entry_path ==
                    scripted.entry_path &&
                issue.source_preference_failure->system_error ==
                    scripted.system_error &&
                issue.source_preference_failure->observed_file_type ==
                    scripted.observed_file_type,
            test_case.name + ": typed failure fields differ");
        expect(
            stub::strict_preference_read_history() ==
                std::vector<std::string>{test_case.name},
            test_case.name + ": strict read count differs");
        expect(stub::database_call_count() == 0, test_case.name + ": failure reached DB resolution");
        expect(!preparation.invocation.has_value(), test_case.name + ": partial invocation escaped");
    }
}

void test_split_child_uses_package_to_base_preference_fallback() {
    const AppConfig config;

    stub::reset();
    const SourceBuildEnvironment expected_environment =
        environment({{"MAKEFLAGS", "-j6"}});
    stub::enqueue_source_preference_result(
        "split-cli", SourcePreferenceAbsent{});
    stub::enqueue_source_preference_result(
        "split-suite",
        loaded_preference("split-suite", expected_environment));
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            single_root_preflight(
                "split-cli",
                DesiredInstallReason::Explicit,
                "split-suite"),
            false,
            config);
    expect_result_invariant(preparation, "requested split child preparation");
    expect(
        preparation.is_prepared() && preparation.issues.empty(),
        "Requested split child did not produce a prepared invocation");
    expect(
        preparation.projected_build_units.size() == 1 &&
            preparation.projected_build_units.front()
                    .required_target_attributions.size() == 1 &&
            preparation.projected_build_units.front()
                    .required_target_attributions.front()
                    .required_target.package_name ==
                "split-cli",
        "Requested split child lost its exact required target projection");
    expect(
        stub::strict_preference_read_history() ==
                std::vector<std::string>{
                    "split-cli", "split-suite"} &&
            preparation.warnings.empty(),
        "Requested split child preference fallback order differs");
    expect(
        stub::database_call_count() == 1,
        "Requested split child did not retain one DB snapshot");
    expect(
        stub::supported_options_guard_history() ==
                std::vector<bool>{false} &&
            stub::pkgdest_guard_history().size() == 2,
        "Requested split child generic preparation count differs");

    const ProductionSourceBuildWorkItem& work_item =
        preparation.invocation->production_invocation_for_test()
            .work_items.front();
    expect(
        work_item.request.package_name == "split-cli" &&
            work_item.request.checkout_name == "split-suite" &&
            work_item.required_targets.size() == 1 &&
            work_item.required_targets.front().package_name ==
                "split-cli" &&
            work_item.required_targets.front().package_base ==
                "split-suite" &&
            work_item.required_targets.front().desired_reason ==
                DesiredInstallReason::Explicit,
        "Requested split child work item identity differs");
    expect(
        same_environment(
            work_item.request.custom_environment,
            expected_environment),
        "Requested split child PackageBase preference environment was not applied");
}

void test_pkgdest_conflicts_stop_before_database() {
    const AppConfig config;
    for(const std::string& value : {std::string{}, std::string{"/claimed"}}) {
        stub::reset();
        stub::enqueue_source_preference_result(
            "pkgdest-root",
            loaded_preference(
                "pkgdest-root",
                environment({{"PKGDEST", value}})));
        AurUpdateSourceBuildPreparation preparation =
            prepare_aur_update_source_build_invocation(
                single_root_preflight("pkgdest-root"), false, config);
        expect_blocked_reason(
            preparation,
            AurUpdatePreparationReason::SourcePreferencePkgdestConflict,
            value.empty() ? "empty PKGDEST" : "nonempty PKGDEST");
        expect(stub::database_call_count() == 0, "PKGDEST conflict reached DB resolution");
        expect(
            stub::supported_options_guard_history().empty() &&
                stub::pkgdest_guard_history().empty(),
            "PKGDEST conflict escaped update-owned validation");
    }
}

void test_database_failure_is_typed_and_has_no_partial_invocation() {
    stub::reset();
    const PackageMetadataFailure failure{
        PackageMetadataErrorCode::ConfigurationMalformed,
        "typed database configuration failure"};
    stub::set_database_failure(failure);

    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("database-root"), false, config);

    expect_blocked_reason(
        preparation,
        AurUpdatePreparationReason::PacmanDatabaseUnavailable,
        "typed Pacman DB failure");
    const AurUpdatePreparationIssue& issue = require_issue(
        preparation,
        AurUpdatePreparationReason::PacmanDatabaseUnavailable,
        "typed Pacman DB failure");
    expect(
        issue.package_metadata_failure.has_value() &&
            issue.package_metadata_failure->code == failure.code &&
            issue.package_metadata_failure->diagnostic ==
                failure.diagnostic,
        "PackageMetadataFailure fields were flattened");
    expect(stub::database_call_count() == 1, "Failed DB resolution was retried");
    expect(
        stub::strict_preference_read_history() ==
            std::vector<std::string>{"database-root"},
        "DB resolution ran before all strict reads");
    expect_event_kinds(
        {
            stub::EventKind::StrictPreferenceRead,
            stub::EventKind::SeparatedInstallOptionsGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::ArtifactPkgdestGuard,
            stub::EventKind::PacmanDatabaseResolution,
        },
        "typed DB failure order");
}

void test_unexpected_generic_failure_is_global_and_stops_before_database() {
    stub::reset();
    stub::fail_supported_options_guard("scripted generic guard failure");

    const AppConfig config;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            ordered_multi_root_preflight(), false, config);

    expect_blocked_reason(
        preparation,
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        "unexpected generic preparation failure");
    const AurUpdatePreparationIssue& issue = require_issue(
        preparation,
        AurUpdatePreparationReason::GenericPreparationInconsistent,
        "unexpected generic preparation failure");
    expect(
        issue.affected_update_plan_indices ==
                std::vector<std::size_t>{0, 2} &&
            issue.affected_roots ==
                std::vector<RootTargetIdentity>{
                    {0, "root-a"}, {1, "root-b"}},
        "Unexpected generic failure was not invocation-global");
    expect(
        stub::strict_preference_read_history() ==
            std::vector<std::string>{
                "private-dependency", "shared-dependency",
                "root-b", "root-a"},
        "Generic guard ran before strict preference completion");
    expect(stub::supported_options_guard_history().size() == 1, "Generic option guard call count differs");
    expect(stub::pkgdest_guard_history().empty(), "Generic failure continued into PKGDEST guards");
    expect(stub::database_call_count() == 0, "Unexpected generic failure reached DB resolution");
}

void test_result_state_helpers_reject_forbidden_combination() {
    AurUpdateSourceBuildPreparation missing_policy;
    expect(
        missing_policy.is_blocked() && !missing_policy.is_noop(),
        "Missing RequiresCheck policy was defaulted to no-op");

    AurUpdateSourceBuildPreparation noop;
    noop.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    expect_result_invariant(noop, "direct no-op model");
    expect(noop.is_noop(), "Explicit BlockOperation result is not no-op");

    AurUpdateSourceBuildPreparation blocked;
    blocked.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    AurUpdatePreparationIssue blocked_issue;
    blocked_issue.reason =
        AurUpdatePreparationReason::PreflightInconsistent;
    blocked_issue.diagnostic = "blocked";
    blocked.issues.push_back(std::move(blocked_issue));
    expect_result_invariant(blocked, "direct blocked model");
    expect(blocked.is_blocked(), "Issue-only result is not blocked");

    stub::reset();
    const AppConfig config;
    AurUpdateSourceBuildPreparation projection_corrupted =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("projection-corrupted-root"),
            false, config);
    expect(
        projection_corrupted.is_prepared(),
        "Projection corruption fixture is not prepared");
    projection_corrupted.projected_build_units.clear();
    expect(
        !projection_corrupted.is_prepared(),
        "Prepared capability ignored a missing projected build unit");

    stub::reset();
    AurUpdateSourceBuildPreparation role_corrupted =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("role-corrupted-root"),
            false, config);
    expect(
        role_corrupted.is_prepared(),
        "Role corruption fixture is not prepared");
    role_corrupted.projected_build_units.front()
        .required_target_attributions.front()
        .roles.front() = PackageRole::BuildDependency;
    expect(
        !role_corrupted.is_prepared(),
        "Prepared capability ignored a valid-but-different child role");

    stub::reset();
    AurUpdateSourceBuildPreparation prepared =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("prepared-root"), false, config);
    expect_result_invariant(prepared, "factory prepared model");
    expect(prepared.is_prepared(), "Factory result is not prepared");

    AurUpdatePreparationIssue forbidden_issue;
    forbidden_issue.reason =
        AurUpdatePreparationReason::GenericPreparationInconsistent;
    forbidden_issue.diagnostic = "forbidden";
    prepared.issues.push_back(std::move(forbidden_issue));
    expect(
        !prepared.is_prepared() && !prepared.is_noop() &&
            prepared.is_blocked(),
        "Forbidden invocation-plus-issues state did not fail closed");
}

void test_move_preserves_correlation_and_invalidates_source() {
    stub::reset();
    const AppConfig config;
    AurUpdateSourceBuildPreparation source =
        prepare_aur_update_source_build_invocation(
            single_root_preflight("move-root"), false, config);
    expect(source.is_prepared(), "Move source was not prepared");

    AurUpdateSourceBuildPreparation destination = std::move(source);
    expect(
        destination.is_prepared(),
        "Move destination lost prepared correlation");
    expect(
        destination.invocation->production_invocation_for_test()
                    .work_items.front()
                    .request.package_name == "move-root" &&
            destination.invocation->work_item_attributions()
                    .front()
                    .package_name == "move-root",
        "Move destination changed work-item attribution correlation");
    expect(
        !source.is_prepared(),
        "Moved-from preparation still reports prepared");
    expect(
        source.invocation.has_value() &&
            !source.invocation->is_valid(),
        "Moved-from preparation did not retain an invalid capability");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

void test_git_revision_intent_is_root_local() {
    stub::reset();
    for(const auto& name : {"private-dependency", "shared-dependency", "root-b", "root-a"})
        stub::enqueue_source_preference_result(name, SourcePreferenceAbsent{});
    stub::set_database_paths(PacmanDatabasePaths{"/", "/var/lib/pacman"});
    auto preflight = ordered_multi_root_preflight();
    auto& update = preflight.targets.front().update;
    update.classification = AurUpdateClassification::UpToDate;
    update.aur_package->version_relation = AurVersionRelation::SameAsInstalled;
    update.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
    update.devel_assessment = DevelUpdateAssessment::update_available();
    const AppConfig config;
    auto result = prepare_aur_update_source_build_invocation(preflight, false, config);
    expect(result.is_prepared(), "Git revision candidate failed normal preparation");
    const auto& work = result.invocation->production_invocation_for_test().work_items;
    expect(work.size() == 4, "Git preparation changed dependency units");
    for(const auto& item : work)
        expect(item.request.authoritative_devel_update == (item.request.checkout_name == "root-a"), "Git root selection intent leaked to dependency/other root");
}

} // namespace

int main() {
    try {
        run_case("Git revision intent remains root-local", test_git_revision_intent_is_root_local);
        run_case(
            "no-op and blocking preflight short circuit",
            test_noop_and_blocking_preflight_short_circuit);
        run_case(
            "skipped preflight inconsistencies fail closed",
            test_skipped_preflight_inconsistencies_fail_closed);
        run_case(
            "executable structure failures",
            test_executable_structure_failures);
        run_case(
            "single-root exact work item and DB snapshot",
            test_single_root_exact_work_item_and_snapshot);
        run_case(
            "BuildPlan order, skipped exclusion, and install reasons",
            test_build_plan_order_skip_exclusion_and_install_reasons);
        run_case(
            "selected repository provider parent build unit",
            test_selected_repository_provider_is_attached_to_parent_build_unit);
        run_case(
            "same-PackageBase child projection and preparation",
            test_same_package_base_projection_retains_exact_child_attribution);
        run_case(
            "incomplete same-PackageBase plan has no projection",
            test_incomplete_same_package_base_plan_has_no_projection);
        run_case(
            "external satisfaction preserves original BuildPlan order",
            test_external_satisfaction_overlay_preserves_original_order);
        run_case(
            "multiple-child external satisfaction keeps child snapshot",
            test_multiple_child_external_satisfaction_retains_child_snapshot);
        run_case(
            "invalid selection and external root stop before IO",
            test_invalid_selection_and_external_root_stop_before_io);
        run_case(
            "unknown package root attribution is global",
            test_unknown_package_root_attribution_is_global);
        run_case(
            "independent RequiresCheck preparation semantics",
            test_independent_devel_requires_check_preparation_semantics);
        run_case(
            "missing required-devel relation blocks before IO",
            test_missing_required_devel_relation_blocks_before_io);
        run_case(
            "RequiresCheck policy snapshot mismatch fails before IO",
            test_requires_check_policy_snapshot_mismatch_fails_before_io);
        run_case(
            "Ignore saved preferences has zero IO and empty environments",
            test_ignore_saved_source_preferences_without_io_or_environment);
        run_case(
            "Ignore preserves preflight and generic safety authorities",
            test_ignore_preserves_preflight_and_generic_safety_authorities);
        run_case(
            "strict absent, empty, valid, and warning results",
            test_strict_absent_empty_valid_and_warning_results);
        run_case(
            "strict typed failures are not flattened",
            test_strict_typed_failures_are_not_flattened);
        run_case(
            "split child uses package-to-base preference fallback",
            test_split_child_uses_package_to_base_preference_fallback);
        run_case(
            "PKGDEST conflicts stop before DB",
            test_pkgdest_conflicts_stop_before_database);
        run_case(
            "typed DB failure has no partial invocation",
            test_database_failure_is_typed_and_has_no_partial_invocation);
        run_case(
            "unexpected generic failure is global",
            test_unexpected_generic_failure_is_global_and_stops_before_database);
        run_case(
            "result state helpers reject forbidden combination",
            test_result_state_helpers_reject_forbidden_combination);
        run_case(
            "move preserves correlation and invalidates source",
            test_move_preserves_correlation_and_invalidates_source);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "AUR update execution preparation tests: all checks passed\n";
    return 0;
}
