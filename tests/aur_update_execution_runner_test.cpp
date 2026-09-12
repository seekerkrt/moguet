#include "app_config.hpp"
#include "aur_update_execution_runner.hpp"
#include "stubs/aur-update-execution-preparation/preparation_stub.hpp"
#include "stubs/aur-update-execution-runner/execution_stub.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using AurUpdateRunnerFunction = AurUpdateSourceBuildExecutionResult (*)(
    PreparedAurUpdateSourceBuildInvocation,
    const AppConfig&);

static_assert(
    std::is_same_v<
        decltype(&execute_prepared_aur_update_source_build_invocation),
        AurUpdateRunnerFunction>);
static_assert(
    std::is_invocable_v<
        AurUpdateRunnerFunction,
        PreparedAurUpdateSourceBuildInvocation&&,
        const AppConfig&>);
static_assert(
    !std::is_invocable_v<
        AurUpdateRunnerFunction,
        PreparedAurUpdateSourceBuildInvocation&,
        const AppConfig&>);
static_assert(
    !std::is_invocable_v<
        AurUpdateRunnerFunction,
        const PreparedAurUpdateSourceBuildInvocation&,
        const AppConfig&>);

namespace {

namespace execution_stub = aur_update_execution_runner_test_stub;
namespace preparation_stub = aur_update_execution_preparation_test_stub;

constexpr const char* UNKNOWN_EXCEPTION_DIAGNOSTIC =
    "Prepared AUR update source-build work item failed with an unknown exception.";

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

AppConfig runner_config() {
    AppConfig config;
    config.user_config.review.pkgbuild = ReviewPolicy::Skip;
    config.user_config.review.diff = ReviewPolicy::Skip;
    config.user_config.build.mode = BuildMode::Clean;
    config.no_confirm = true;
    config.rm_deps = false;
    config.editor = "runner-test-editor";
    return config;
}

ProductionSourceBuildStagedOutcome reviewed_staged_outcome(
    ProductionSourceBuildCommandOutcome build_outcome,
    ProductionSourceInstallOutcome install_outcome) {
    ProductionSourceBuildProvenance provenance;
    provenance.review_status = ProductionSourceReviewStatus::Reviewed;
    provenance.reviewed_upstream_base_revision =
        SourceRevisionIdentity::git_commit(
            "1111111111111111111111111111111111111111");
    provenance.publication_status =
        ReviewedSourcePublicationStatus::Published;
    provenance.reviewed_outcome =
        ProductionReviewedSourceOutcome::UpdateReview;
    provenance.reviewed_state_generation = 9;
    return ProductionSourceBuildStagedOutcome{
        std::move(provenance), build_outcome, install_outcome};
}

RequiredPackageArtifactTarget required_target(
    const std::string& package_base,
    const std::string& package_name,
    DesiredInstallReason desired_reason) {
    return RequiredPackageArtifactTarget{
        package_base, package_name, desired_reason};
}

PackageBaseSourceBuildSelectedResult selected_child(
    const std::string& package_name,
    const std::string& full_version,
    DesiredInstallReason desired_reason,
    ArtifactInstallExecutionOutcome outcome) {
    return PackageBaseSourceBuildSelectedResult{
        ArtifactPackageIdentity{package_name, full_version},
        desired_reason,
        outcome};
}

PackageBaseArtifactInstallTransactionAttempt transaction_attempt(
    const std::string& package_name,
    const std::string& full_version,
    DesiredInstallReason desired_reason) {
    return PackageBaseArtifactInstallTransactionAttempt{
        ArtifactPackageIdentity{package_name, full_version},
        desired_reason};
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

AurUpdateExecutionPreflight single_root_preflight(
    const std::string& package_name,
    DesiredInstallReason desired_reason,
    const std::string& package_base) {
    const RootTargetIdentity root{0, package_name};
    BuildPlan plan;
    plan.root_targets.push_back(root);
    plan.package_targets.push_back(PlannedPackageTarget{
        package_name,
        package_base,
        {PackageRole::Root},
        {root}});
    plan.order.push_back(BuildPlanEntry{package_base, {package_name}});

    AurUpdateExecutionPreflight preflight;
    preflight.targets.push_back(executable_target(
        0, 0, package_name, desired_reason, package_base));
    preflight.build_plan = std::move(plan);
    return preflight;
}

ProvidedDependency selected_repository_provider() {
    ProvidedDependency provider = ProvidedDependency::from_repository(
        "extra", "selected-runner-provider", "virtual-runner-api",
        "virtual-runner-api=2", "2.0-1");
    provider.package_base = "selected-runner-provider-base";
    return provider;
}

AurUpdateExecutionPreflight selected_repository_provider_preflight() {
    AurUpdateExecutionPreflight preflight = single_root_preflight(
        "provider-root", DesiredInstallReason::Explicit,
        "provider-root");
    preflight.build_plan->dependency_edges.push_back(
        BuildPlanDependencyEdge{
            "provider-root",
            "provider-root",
            "virtual-runner-api",
            PackageRole::RuntimeDependency,
            DependencyKind::Provided,
            std::nullopt,
            std::nullopt,
            selected_repository_provider(),
            ProviderResolutionKind::UserSelected});
    return preflight;
}

AurUpdateExecutionPreflight three_singular_work_item_preflight() {
    const RootTargetIdentity root{0, "runner-root"};
    BuildPlan plan;
    plan.root_targets.push_back(root);
    plan.package_targets = {
        PlannedPackageTarget{
            "first-dependency",
            "first-dependency",
            {PackageRole::BuildDependency},
            {root}},
        PlannedPackageTarget{
            "second-dependency",
            "second-dependency",
            {PackageRole::RuntimeDependency},
            {root}},
        PlannedPackageTarget{
            "runner-root",
            "runner-root",
            {PackageRole::Root},
            {root}},
    };
    plan.order = {
        BuildPlanEntry{
            "first-dependency", {"first-dependency"}},
        BuildPlanEntry{
            "second-dependency", {"second-dependency"}},
        BuildPlanEntry{"runner-root", {"runner-root"}},
    };

    AurUpdateExecutionPreflight preflight;
    preflight.targets.push_back(executable_target(
        0, 0, "runner-root", DesiredInstallReason::Explicit,
        "runner-root"));
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateExecutionPreflight later_repository_provider_preflight() {
    AurUpdateExecutionPreflight preflight =
        three_singular_work_item_preflight();
    preflight.build_plan->dependency_edges.push_back(
        BuildPlanDependencyEdge{
            "second-dependency",
            "second-dependency",
            "virtual-runner-api",
            PackageRole::RuntimeDependency,
            DependencyKind::Provided,
            std::nullopt,
            std::nullopt,
            selected_repository_provider(),
            ProviderResolutionKind::UserSelected});
    return preflight;
}

AurUpdateExecutionPreflight same_package_base_multiple_preflight() {
    const RootTargetIdentity dependency_installed_root{0, "split-runtime"};
    const RootTargetIdentity explicit_root{1, "split-explicit"};

    BuildPlan plan;
    plan.root_targets = {dependency_installed_root, explicit_root};
    plan.package_targets = {
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

AurUpdateExecutionPreflight multiple_then_tail_preflight() {
    const RootTargetIdentity dependency_installed_root{0, "split-runtime"};
    const RootTargetIdentity explicit_root{1, "split-explicit"};
    const RootTargetIdentity tail_root{2, "tail-root"};

    BuildPlan plan;
    plan.root_targets = {
        dependency_installed_root, explicit_root, tail_root};
    plan.package_targets = {
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
        PlannedPackageTarget{
            "tail-root",
            "tail-root",
            {PackageRole::Root},
            {tail_root}},
    };
    plan.order = {
        BuildPlanEntry{
            "split-suite", {"split-runtime", "split-explicit"}},
        BuildPlanEntry{"tail-root", {"tail-root"}},
    };

    AurUpdateExecutionPreflight preflight;
    preflight.targets = {
        executable_target(
            0, 0, "split-runtime",
            DesiredInstallReason::Dependency, "split-suite"),
        executable_target(
            1, 1, "split-explicit",
            DesiredInstallReason::Explicit, "split-suite"),
        executable_target(
            2, 2, "tail-root",
            DesiredInstallReason::Explicit, "tail-root"),
    };
    preflight.build_plan = std::move(plan);
    return preflight;
}

AurUpdateSourceBuildPreparation prepare_fixture(
    AurUpdateExecutionPreflight preflight,
    bool needed,
    const AppConfig& config,
    const PacmanDatabasePaths& database_paths) {
    preparation_stub::reset();
    preparation_stub::set_database_paths(database_paths);
    preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Strict, needed, config);
    expect(
        preparation.is_prepared(),
        preparation.issues.empty()
            ? "Runner fixture was not prepared"
            : "Runner fixture was not prepared: " +
                  preparation.issues.front().diagnostic);
    expect(preparation.issues.empty(), "Runner fixture retained issues");
    expect(
        preparation.invocation.has_value(),
        "Runner fixture lost its invocation");
    expect(
        preparation_stub::database_call_count() == 1,
        "Runner fixture did not resolve exactly one DB snapshot");
    return preparation;
}

execution_stub::ExpectedExecution expected_execution(
    std::size_t call_index,
    const std::string& package_base,
    std::vector<RequiredPackageArtifactTarget> ordered_required_targets,
    bool needed,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    return execution_stub::ExpectedExecution{
        call_index,
        package_base,
        std::move(ordered_required_targets),
        needed,
        database_paths,
        config};
}

AurUpdateSourceBuildExecutionResult execute_without_escape(
    PreparedAurUpdateSourceBuildInvocation invocation,
    const AppConfig& config,
    const std::string& context) {
    try {
        return execute_prepared_aur_update_source_build_invocation(
            std::move(invocation), config);
    } catch(...) {
        throw std::runtime_error(
            context + ": runner leaked an executor exception");
    }
}

bool same_identity(
    const ArtifactPackageIdentity& actual,
    const ArtifactPackageIdentity& expected) {
    return actual.package_name == expected.package_name &&
           actual.full_version == expected.full_version;
}

void expect_child(
    const AurUpdateChildExecutionResult& child,
    std::size_t work_item_index,
    std::size_t build_plan_order_index,
    std::size_t required_child_index,
    const std::string& package_base,
    const std::string& required_package_name,
    DesiredInstallReason desired_reason,
    const std::vector<std::size_t>& affected_update_plan_indices,
    const std::vector<RootTargetIdentity>& affected_roots,
    const std::vector<PackageRole>& roles,
    AurUpdateChildExecutionStatus status,
    const std::optional<ArtifactPackageIdentity>& selected_artifact,
    const std::string& context) {
    expect(child.work_item_index == work_item_index,
           context + ": work-item index differs");
    expect(child.build_plan_order_index == build_plan_order_index,
           context + ": BuildPlan order index differs");
    expect(child.required_child_index == required_child_index,
           context + ": required-child index differs");
    expect(child.package_base == package_base,
           context + ": PackageBase differs");
    expect(child.required_package_name == required_package_name,
           context + ": required package name differs");
    expect(child.desired_install_reason == desired_reason,
           context + ": desired install reason differs");
    expect(child.affected_update_plan_indices == affected_update_plan_indices,
           context + ": update target attribution differs");
    expect(child.affected_roots == affected_roots,
           context + ": root attribution differs");
    expect(child.roles == roles, context + ": roles differ");
    expect(child.status == status, context + ": status differs");
    expect(child.selected_artifact.has_value() ==
               selected_artifact.has_value(),
           context + ": selected artifact presence differs");
    if(selected_artifact.has_value()) {
        expect(
            same_identity(*child.selected_artifact, *selected_artifact),
            context + ": selected artifact identity differs");
    }
}

void expect_no_fabricated_child_success(
    const AurUpdateWorkItemExecutionResult& work_item,
    const std::string& context) {
    for(const auto& child : work_item.child_results) {
        expect(
            child.status == AurUpdateChildExecutionStatus::NotAttempted,
            context + ": failure fabricated a completed child outcome");
        expect(
            !child.selected_artifact.has_value(),
            context + ": failure fabricated a selected artifact");
    }
    expect(
        work_item.unselected_artifacts.empty(),
        context + ": failure retained unselected artifacts as success");
}

void expect_events(
    const std::vector<execution_stub::EventKind>& expected,
    const std::string& context) {
    const auto& events = execution_stub::event_history();
    expect(events.size() == expected.size(),
           context + ": event count differs");
    for(std::size_t index = 0; index < expected.size(); ++index) {
        expect(events[index].kind == expected[index],
               context + ": event order differs at " +
                   std::to_string(index));
    }
}

template <typename Detail>
const Detail& require_failure_detail(
    const AurUpdateWorkItemExecutionResult& work_item,
    const std::string& context) {
    const Detail* detail = std::get_if<Detail>(&work_item.failure_detail);
    expect(detail != nullptr, context + ": failure detail type differs");
    return *detail;
}

void test_ordinary_size_one_uses_set_owner_strictly() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/ordinary/root", "/ordinary/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        single_root_preflight(
            "ordinary-package",
            DesiredInstallReason::Explicit,
            "ordinary-package"),
        false,
        config,
        database_paths);

    execution_stub::reset();
    execution_stub::enqueue_success(
        expected_execution(
            0,
            "ordinary-package",
            {required_target(
                "ordinary-package",
                "ordinary-package",
                DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        "ordinary-package",
        {selected_child(
            "ordinary-package",
            "2.0-1",
            DesiredInstallReason::Explicit,
            ArtifactInstallExecutionOutcome::Installed)});

    const AurUpdateSourceBuildExecutionResult result =
        execute_without_escape(
            std::move(*preparation.invocation),
            config,
            "ordinary size-one");
    execution_stub::require_script_consumed();

    expect(result.status == AurUpdateInvocationExecutionStatus::Completed,
           "Ordinary size-one invocation did not complete");
    expect(result.is_success() && result.changed_package_state(),
           "Ordinary size-one helpers differ");
    expect(result.work_item_results.size() == 1,
           "Ordinary size-one result count differs");
    const auto& work_item = result.work_item_results.front();
    expect(work_item.package_name == "ordinary-package" &&
               work_item.package_base == "ordinary-package" &&
               work_item.plan_package_names ==
                   std::vector<std::string>{"ordinary-package"},
           "Ordinary size-one compatibility identity differs");
    expect(work_item.status == AurUpdateWorkItemExecutionStatus::Updated &&
               work_item.failure_kind ==
                   AurUpdateWorkItemFailureKind::None,
           "Ordinary size-one aggregate state differs");
    expect(work_item.child_results.size() == 1,
           "Ordinary size-one child count differs");
    expect_child(
        work_item.child_results.front(),
        0,
        0,
        0,
        "ordinary-package",
        "ordinary-package",
        DesiredInstallReason::Explicit,
        {0},
        {{0, "ordinary-package"}},
        {PackageRole::Root},
        AurUpdateChildExecutionStatus::Installed,
        ArtifactPackageIdentity{"ordinary-package", "2.0-1"},
        "ordinary size-one child");
    expect_events(
        {execution_stub::EventKind::Checkout,
         execution_stub::EventKind::Build,
         execution_stub::EventKind::Install,
         execution_stub::EventKind::Cleanup},
        "ordinary size-one");
}

void test_confirmation_stops_preserve_partial_and_control_flow() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths paths{"/cancel/root", "/cancel/database"};
    for(const auto reason : {ConfirmationCancellationReason::ExplicitToken,
                             ConfirmationCancellationReason::EndOfInput}) {
        for(const std::size_t count : {std::size_t{1}, std::size_t{3}}) {
            for(std::size_t stop_index = 0; stop_index < count; ++stop_index) {
                auto preparation = prepare_fixture(
                    count == 1 ? single_root_preflight("runner-root", DesiredInstallReason::Explicit, "runner-root")
                               : three_singular_work_item_preflight(),
                    false, config, paths);
                execution_stub::reset();
                const std::vector<std::string> names = count == 1
                                                           ? std::vector<std::string>{"runner-root"}
                                                           : std::vector<std::string>{"first-dependency", "second-dependency", "runner-root"};
                for(std::size_t i = 0; i <= stop_index; ++i) {
                    const auto install_reason = i + 1 == count ? DesiredInstallReason::Explicit : DesiredInstallReason::Dependency;
                    auto expected = expected_execution(i, names[i], {required_target(names[i], names[i], install_reason)}, false, paths, config);
                    if(i == stop_index)
                        execution_stub::enqueue_confirmation_stop(std::move(expected), ConfirmationCancelled{reason});
                    else
                        execution_stub::enqueue_success(std::move(expected), names[i], {selected_child(names[i], "2.0-1", install_reason, ArtifactInstallExecutionOutcome::Installed)});
                }
                bool caught = false;
                try {
                    static_cast<void>(execute_prepared_aur_update_source_build_invocation(std::move(*preparation.invocation), config));
                    throw std::runtime_error("Cancellation returned ordinary execution result");
                } catch(AurUpdateExecutionCancelled& stop) {
                    auto result = std::move(stop).release_result();
                    caught = true;
                    expect(!result.is_success() && result.status == AurUpdateInvocationExecutionStatus::StoppedOnWorkItemCancellation,
                           "Cancellation lost invocation stop");
                    expect(result.stopped_work_item_index() == stop_index && result.work_item_results.size() == count,
                           "Cancellation lost stop index or suffix");
                    for(std::size_t i = 0; i < count; ++i) {
                        const auto& item = result.work_item_results[i];
                        expect(item.status == (i < stop_index ? AurUpdateWorkItemExecutionStatus::Updated : i == stop_index ? AurUpdateWorkItemExecutionStatus::Cancelled
                                                                                                                            : AurUpdateWorkItemExecutionStatus::NotAttempted),
                               "Cancellation changed partial states");
                        if(i == stop_index) {
                            expect(item.cancellation == ConfirmationCancelled{reason} && item.failure_kind == AurUpdateWorkItemFailureKind::None,
                                   "Cancellation reason was flattened into failure");
                        }
                        if(i >= stop_index) expect_no_fabricated_child_success(item, "cancelled/unattempted child");
                    }
                }
                expect(caught && execution_stub::call_history().size() == stop_index + 1, "Cancellation continued later work");
                expect(execution_stub::event_history().size() == stop_index * 4 + 1,
                       "Cancellation executed build/install or later checkout");
                execution_stub::require_script_consumed();
            }
        }
    }
    for(const ConfirmationResult& original : std::vector<ConfirmationResult>{
            ConfirmationDeclined{ConfirmationDecisionOrigin::ExplicitToken},
            ConfirmationUnavailable{ConfirmationUnavailableReason::NonInteractiveInput}, ConfirmationInputFailure{}}) {
        auto preparation = prepare_fixture(single_root_preflight("runner-root", DesiredInstallReason::Explicit, "runner-root"), false, config, paths);
        execution_stub::reset();
        execution_stub::enqueue_confirmation_stop(expected_execution(0, "runner-root", {required_target("runner-root", "runner-root", DesiredInstallReason::Explicit)}, false, paths, config), original);
        bool caught = false;
        try {
            static_cast<void>(execute_prepared_aur_update_source_build_invocation(std::move(*preparation.invocation), config));
        } catch(const ConfirmationOperationStopped& stop) {
            expect(stop.result() == original, "Non-cancel confirmation changed type");
            caught = true;
        }
        expect(caught, "Non-cancel stop lost existing propagation");
        execution_stub::require_script_consumed();
    }
}

void test_multiple_work_items_preserve_fifo_call_order_and_one_db_snapshot() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/ordered/root", "/ordered/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        three_singular_work_item_preflight(),
        false,
        config,
        database_paths);

    execution_stub::reset();
    const std::vector<std::pair<std::string, DesiredInstallReason>> expected{
        {"first-dependency", DesiredInstallReason::Dependency},
        {"second-dependency", DesiredInstallReason::Dependency},
        {"runner-root", DesiredInstallReason::Explicit},
    };
    for(std::size_t index = 0; index < expected.size(); ++index) {
        const auto& [package_name, desired_reason] = expected[index];
        execution_stub::enqueue_success(
            expected_execution(
                index,
                package_name,
                {required_target(
                    package_name,
                    package_name,
                    desired_reason)},
                false,
                database_paths,
                config),
            package_name,
            {selected_child(
                package_name,
                "2.0-1",
                desired_reason,
                ArtifactInstallExecutionOutcome::Installed)});
    }

    const auto result = execute_without_escape(
        std::move(*preparation.invocation),
        config,
        "ordered work items");
    execution_stub::require_script_consumed();
    expect(result.is_success() && result.work_item_results.size() == 3,
           "Ordered work items did not complete");
    const auto& calls = execution_stub::call_history();
    expect(calls.size() == 3,
           "Ordered work-item call count differs");
    for(std::size_t index = 0; index < calls.size(); ++index) {
        expect(calls[index].call_index == index &&
                   calls[index].package_base == expected[index].first &&
                   calls[index].database_paths.root_dir ==
                       database_paths.root_dir &&
                   calls[index].database_paths.db_path ==
                       database_paths.db_path,
               "Ordered work-item strict call snapshot differs at " +
                   std::to_string(index));
    }
    expect(execution_stub::event_history().size() == 12,
           "Ordered work-item lifecycle event count differs");
}

void test_selected_repository_provider_transaction_precedes_source_and_stops_failure() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/provider/root", "/provider/database"};

    AurUpdateSourceBuildPreparation successful_preparation = prepare_fixture(
        selected_repository_provider_preflight(), false, config,
        database_paths);
    expect(
        successful_preparation.invocation->production_invocation_for_test()
                .selected_repository_providers ==
            std::vector<ProvidedDependency>{
                selected_repository_provider()},
        "Runner fixture lost its selected repository provider");

    execution_stub::reset();
    execution_stub::enqueue_success(
        expected_execution(
            0,
            "provider-root",
            {required_target(
                "provider-root",
                "provider-root",
                DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        "provider-root",
        {selected_child(
            "provider-root",
            "2.0-1",
            DesiredInstallReason::Explicit,
            ArtifactInstallExecutionOutcome::Installed)});
    const AurUpdateSourceBuildExecutionResult success = execute_without_escape(
        std::move(*successful_preparation.invocation), config,
        "selected repository provider success");
    expect(success.is_success() &&
               success.selected_repository_provider_transaction.status ==
                   SelectedRepositoryProviderTransactionStatus::
                       Succeeded &&
               success.selected_repository_provider_transaction.package_state_change ==
                   PackageStateChange::Unknown &&
               success.selected_repository_provider_transaction.selected_providers ==
                   std::vector<ProvidedDependency>{
                       selected_repository_provider()},
           "Selected repository provider success did not complete");
    expect(
        execution_stub::invocation_event_history() ==
            std::vector<execution_stub::InvocationEventKind>{
                execution_stub::InvocationEventKind::CacheActivation,
                execution_stub::InvocationEventKind::
                    RepositoryProviderTransaction,
                execution_stub::InvocationEventKind::SourceExecution},
        "Cache/provider transaction did not precede source execution");
    execution_stub::require_script_consumed();

    AurUpdateSourceBuildPreparation failing_preparation = prepare_fixture(
        selected_repository_provider_preflight(), false, config,
        database_paths);
    execution_stub::reset();
    execution_stub::fail_repository_provider_transaction(
        "scripted repository provider transaction failure");
    const AurUpdateSourceBuildExecutionResult failure = execute_without_escape(
        std::move(*failing_preparation.invocation), config,
        "selected repository provider failure");
    expect(
        failure.status ==
                AurUpdateInvocationExecutionStatus::
                    StoppedOnProviderTransactionFailure &&
            failure.work_item_results.size() == 1,
        "Repository provider transaction failure lost invocation state");
    const AurUpdateWorkItemExecutionResult& failed_work_item =
        failure.work_item_results.front();
    expect(
        failed_work_item.status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted &&
            failed_work_item.failure_kind ==
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped &&
            !failed_work_item.diagnostic.has_value() &&
            failure.selected_repository_provider_transaction.status ==
                SelectedRepositoryProviderTransactionStatus::Failed &&
            failure.selected_repository_provider_transaction.package_state_change ==
                PackageStateChange::Unknown &&
            failure.selected_repository_provider_transaction.diagnostic ==
                std::optional<std::string>{
                    "scripted repository provider transaction failure"},
        "Repository provider transaction failure lost independent phase state");
    expect_no_fabricated_child_success(
        failed_work_item,
        "selected repository provider transaction failure");
    expect(
        execution_stub::call_history().empty() &&
            execution_stub::event_history().empty() &&
            execution_stub::invocation_event_history() ==
                std::vector<execution_stub::InvocationEventKind>{
                    execution_stub::InvocationEventKind::
                        CacheActivation,
                    execution_stub::InvocationEventKind::
                        RepositoryProviderTransaction},
        "Repository provider transaction failure reached source execution");
    execution_stub::require_script_consumed();

    AurUpdateSourceBuildPreparation later_owner_preparation = prepare_fixture(
        later_repository_provider_preflight(), false, config,
        database_paths);
    execution_stub::reset();
    execution_stub::fail_repository_provider_transaction(
        "scripted later-owner provider transaction failure");
    const AurUpdateSourceBuildExecutionResult later_owner_failure =
        execute_without_escape(
            std::move(*later_owner_preparation.invocation), config,
            "later selected repository provider owner failure");
    expect(
        later_owner_failure.status ==
                AurUpdateInvocationExecutionStatus::
                    StoppedOnProviderTransactionFailure &&
            later_owner_failure.work_item_results.size() == 3 &&
            later_owner_failure.work_item_results[0].status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted &&
            later_owner_failure.work_item_results[1].status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted &&
            later_owner_failure.work_item_results[1].failure_kind ==
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped &&
            later_owner_failure.work_item_results[2].status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted &&
            later_owner_failure.selected_repository_provider_transaction.selected_providers ==
                std::vector<ProvidedDependency>{
                    selected_repository_provider()},
        "Repository provider transaction failure was not kept independent of work items");
    for(const auto& work_item : later_owner_failure.work_item_results) {
        expect_no_fabricated_child_success(
            work_item,
            "later selected repository provider owner failure");
    }
    expect(
        execution_stub::call_history().empty() &&
            execution_stub::invocation_event_history() ==
                std::vector<execution_stub::InvocationEventKind>{
                    execution_stub::InvocationEventKind::
                        CacheActivation,
                    execution_stub::InvocationEventKind::
                        RepositoryProviderTransaction},
        "Later provider owner failure reached source execution");
    execution_stub::require_script_consumed();

    AurUpdateSourceBuildPreparation cache_failure_preparation =
        prepare_fixture(
            selected_repository_provider_preflight(), false, config,
            database_paths);
    execution_stub::reset();
    execution_stub::fail_cache_activation(TrustedCacheFailure{
        TrustedCacheStage::RootRevalidation,
        TrustedCacheErrorCode::ConcurrentReplacement,
        std::nullopt});
    std::optional<TrustedCacheFailure> observed_cache_failure;
    try {
        static_cast<void>(
            execute_prepared_aur_update_source_build_invocation(
                std::move(*cache_failure_preparation.invocation),
                config));
    } catch(const TrustedCacheError& error) {
        observed_cache_failure = error.failure();
    }
    expect(
        observed_cache_failure.has_value() &&
            observed_cache_failure->stage ==
                TrustedCacheStage::RootRevalidation &&
            observed_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement &&
            execution_stub::call_history().empty() &&
            execution_stub::invocation_event_history() ==
                std::vector<execution_stub::InvocationEventKind>{
                    execution_stub::InvocationEventKind::
                        CacheActivation},
        "Cache activation failure reached repository provider transaction");
    execution_stub::require_script_consumed();
}

void test_later_fatal_preflight_blocks_cache_provider_and_first_work_item() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/fatal/root", "/fatal/database"};
    preparation_stub::reset();
    preparation_stub::set_database_paths(database_paths);
    preparation_stub::fail_reviewed_state_preflight_on_call(
        2, "scripted later reviewed-state fatal observation");
    execution_stub::reset();

    AurUpdateExecutionPreflight preflight =
        later_repository_provider_preflight();
    preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    AurUpdateSourceBuildPreparation preparation =
        prepare_aur_update_source_build_invocation(
            preflight, DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Strict, false, config);
    expect(!preparation.is_prepared() &&
               !preparation.invocation.has_value() &&
               preparation.issues.size() == 1 &&
               preparation.issues.front().reason ==
                   AurUpdatePreparationReason::
                       GenericPreparationInconsistent &&
               preparation.issues.front().diagnostic.find(
                   "scripted later reviewed-state fatal observation") !=
                   std::string::npos,
           "Later fatal preflight did not stop aggregate preparation");
    expect(preparation_stub::reviewed_state_preflight_call_count() == 2 &&
               preparation_stub::database_call_count() == 0,
           "Later fatal preflight did not precede the DB invocation snapshot");
    expect(execution_stub::invocation_event_history().empty() &&
               execution_stub::call_history().empty() &&
               execution_stub::event_history().empty(),
           "Later fatal preflight reached cache/provider/source execution");
}

void test_requested_split_child_size_one_is_no_change() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/split-one/root", "/split-one/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        single_root_preflight(
            "split-child",
            DesiredInstallReason::Dependency,
            "split-suite"),
        true,
        config,
        database_paths);

    execution_stub::reset();
    execution_stub::enqueue_success(
        expected_execution(
            0,
            "split-suite",
            {required_target(
                "split-suite",
                "split-child",
                DesiredInstallReason::Dependency)},
            true,
            database_paths,
            config),
        "split-suite",
        {selected_child(
            "split-child",
            "3.2-4",
            DesiredInstallReason::Dependency,
            ArtifactInstallExecutionOutcome::SkippedAsNeeded)},
        {ArtifactPackageIdentity{"split-debug", "3.2-4"}});

    const AurUpdateSourceBuildExecutionResult result =
        execute_without_escape(
            std::move(*preparation.invocation),
            config,
            "requested split child size-one");
    execution_stub::require_script_consumed();

    const auto& work_item = result.work_item_results.front();
    expect(result.status == AurUpdateInvocationExecutionStatus::Completed &&
               result.is_success() &&
               !result.changed_package_state(),
           "Requested split child result helpers differ");
    expect(work_item.package_name == "split-child" &&
               work_item.package_base == "split-suite" &&
               work_item.status ==
                   AurUpdateWorkItemExecutionStatus::NoChange,
           "Requested split child aggregate differs");
    expect(work_item.unselected_artifacts.size() == 1 &&
               same_identity(
                   work_item.unselected_artifacts.front(),
                   {"split-debug", "3.2-4"}),
           "Requested split child lost its unselected sibling");
    expect_child(
        work_item.child_results.front(),
        0,
        0,
        0,
        "split-suite",
        "split-child",
        DesiredInstallReason::Dependency,
        {0},
        {{0, "split-child"}},
        {PackageRole::Root},
        AurUpdateChildExecutionStatus::SkippedAsNeeded,
        ArtifactPackageIdentity{"split-child", "3.2-4"},
        "requested split child");
}

void test_multiple_children_preserve_order_reason_outcome_and_attribution() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/multiple/root", "/multiple/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        same_package_base_multiple_preflight(),
        true,
        config,
        database_paths);

    execution_stub::reset();
    execution_stub::enqueue_success(
        expected_execution(
            0,
            "split-suite",
            {required_target(
                 "split-suite",
                 "split-runtime",
                 DesiredInstallReason::Dependency),
             required_target(
                 "split-suite",
                 "split-explicit",
                 DesiredInstallReason::Explicit)},
            true,
            database_paths,
            config),
        "split-suite",
        {selected_child(
             "split-runtime",
             "5.1-2",
             DesiredInstallReason::Dependency,
             ArtifactInstallExecutionOutcome::Installed),
         selected_child(
             "split-explicit",
             "5.1-2",
             DesiredInstallReason::Explicit,
             ArtifactInstallExecutionOutcome::SkippedAsNeeded)},
        {ArtifactPackageIdentity{"split-debug", "5.1-2"},
         ArtifactPackageIdentity{"split-docs", "5.1-2"}});

    const AurUpdateSourceBuildExecutionResult result =
        execute_without_escape(
            std::move(*preparation.invocation),
            config,
            "multiple child success");
    execution_stub::require_script_consumed();

    expect(result.status == AurUpdateInvocationExecutionStatus::Completed &&
               result.is_success() && result.changed_package_state(),
           "Multiple child invocation helpers differ");
    const auto& work_item = result.work_item_results.front();
    expect(work_item.package_name.empty(),
           "Multiple child result exposed a singular package name");
    expect(work_item.package_base == "split-suite" &&
               work_item.plan_package_names ==
                   std::vector<std::string>{
                       "split-runtime", "split-explicit"} &&
               work_item.affected_update_plan_indices ==
                   std::vector<std::size_t>{0, 2} &&
               work_item.affected_roots ==
                   std::vector<RootTargetIdentity>{
                       {0, "split-runtime"},
                       {1, "split-explicit"}} &&
               work_item.status ==
                   AurUpdateWorkItemExecutionStatus::Updated,
           "Multiple child aggregate identity differs");
    expect(work_item.child_results.size() == 2,
           "Multiple child result count differs");
    expect_child(
        work_item.child_results[0],
        0,
        0,
        0,
        "split-suite",
        "split-runtime",
        DesiredInstallReason::Dependency,
        {0, 2},
        {{0, "split-runtime"}, {1, "split-explicit"}},
        {PackageRole::Root, PackageRole::RuntimeDependency},
        AurUpdateChildExecutionStatus::Installed,
        ArtifactPackageIdentity{"split-runtime", "5.1-2"},
        "multiple dependency child");
    expect_child(
        work_item.child_results[1],
        0,
        0,
        1,
        "split-suite",
        "split-explicit",
        DesiredInstallReason::Explicit,
        {2},
        {{1, "split-explicit"}},
        {PackageRole::Root},
        AurUpdateChildExecutionStatus::SkippedAsNeeded,
        ArtifactPackageIdentity{"split-explicit", "5.1-2"},
        "multiple explicit child");
    expect(work_item.unselected_artifacts.size() == 2 &&
               same_identity(
                   work_item.unselected_artifacts[0],
                   {"split-debug", "5.1-2"}) &&
               same_identity(
                   work_item.unselected_artifacts[1],
                   {"split-docs", "5.1-2"}),
           "Multiple child result lost unselected produced order");
    const auto& calls = execution_stub::call_history();
    expect(calls.size() == 1 && calls.front().package_name.empty() &&
               calls.front().ordered_required_targets.size() == 2 &&
               calls.front().needed && calls.front().config.no_confirm,
           "Strict set-owner call snapshot differs");
}

void run_returned_correlation_failure(
    const std::string& context,
    const std::string& returned_package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    AurUpdateExecutionCorrelationFailureReason expected_reason) {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/correlation/root", "/correlation/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        same_package_base_multiple_preflight(),
        false,
        config,
        database_paths);
    execution_stub::reset();
    execution_stub::enqueue_success(
        expected_execution(
            0,
            "split-suite",
            {required_target(
                 "split-suite",
                 "split-runtime",
                 DesiredInstallReason::Dependency),
             required_target(
                 "split-suite",
                 "split-explicit",
                 DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        returned_package_base,
        std::move(selected_children),
        std::move(unselected_artifacts));

    const AurUpdateSourceBuildExecutionResult result =
        execute_without_escape(
            std::move(*preparation.invocation), config, context);
    execution_stub::require_script_consumed();
    expect(result.status == AurUpdateInvocationExecutionStatus::
                                StoppedOnWorkItemFailure,
           context + ": invocation status differs");
    const auto& work_item = result.work_item_results.front();
    expect(work_item.status == AurUpdateWorkItemExecutionStatus::Failed &&
               work_item.failure_kind ==
                   AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
           context + ": aggregate failure state differs");
    const auto& detail =
        require_failure_detail<AurUpdateExecutionCorrelationFailure>(
            work_item, context);
    expect(detail.reason == expected_reason,
           context + ": correlation reason differs");
    expect_no_fabricated_child_success(work_item, context);
}

void test_returned_result_correlation_failures_are_fail_closed() {
    const auto valid_children = []() {
        return std::vector<PackageBaseSourceBuildSelectedResult>{
            selected_child(
                "split-runtime",
                "6.0-1",
                DesiredInstallReason::Dependency,
                ArtifactInstallExecutionOutcome::Installed),
            selected_child(
                "split-explicit",
                "6.0-1",
                DesiredInstallReason::Explicit,
                ArtifactInstallExecutionOutcome::SkippedAsNeeded)};
    };

    run_returned_correlation_failure(
        "returned PackageBase mismatch",
        "other-suite",
        valid_children(),
        {},
        AurUpdateExecutionCorrelationFailureReason::PackageBaseMismatch);

    auto reversed = valid_children();
    std::swap(reversed[0], reversed[1]);
    run_returned_correlation_failure(
        "returned selected order mismatch",
        "split-suite",
        std::move(reversed),
        {},
        AurUpdateExecutionCorrelationFailureReason::
            SelectedArtifactIdentityMismatch);

    auto wrong_reason = valid_children();
    wrong_reason[0].desired_reason = DesiredInstallReason::Explicit;
    run_returned_correlation_failure(
        "returned reason mismatch",
        "split-suite",
        std::move(wrong_reason),
        {},
        AurUpdateExecutionCorrelationFailureReason::
            DesiredInstallReasonMismatch);

    auto empty_version = valid_children();
    empty_version[1].identity.full_version.clear();
    run_returned_correlation_failure(
        "returned empty version",
        "split-suite",
        std::move(empty_version),
        {},
        AurUpdateExecutionCorrelationFailureReason::
            EmptySelectedArtifactVersion);

    auto missing = valid_children();
    missing.pop_back();
    run_returned_correlation_failure(
        "returned missing selected child",
        "split-suite",
        std::move(missing),
        {},
        AurUpdateExecutionCorrelationFailureReason::MissingSelectedChild);

    auto extra = valid_children();
    extra.push_back(selected_child(
        "split-extra",
        "6.0-1",
        DesiredInstallReason::Dependency,
        ArtifactInstallExecutionOutcome::Installed));
    run_returned_correlation_failure(
        "returned extra selected child",
        "split-suite",
        std::move(extra),
        {},
        AurUpdateExecutionCorrelationFailureReason::ExtraSelectedChild);

    auto duplicate = valid_children();
    duplicate[1].identity.package_name = "split-runtime";
    run_returned_correlation_failure(
        "returned duplicate selected child",
        "split-suite",
        std::move(duplicate),
        {},
        AurUpdateExecutionCorrelationFailureReason::
            DuplicateSelectedChild);

    auto unknown_outcome = valid_children();
    unknown_outcome[0].outcome =
        static_cast<ArtifactInstallExecutionOutcome>(-1);
    run_returned_correlation_failure(
        "returned unknown child outcome",
        "split-suite",
        std::move(unknown_outcome),
        {},
        AurUpdateExecutionCorrelationFailureReason::UnknownChildOutcome);

    run_returned_correlation_failure(
        "returned selected-unselected overlap",
        "split-suite",
        valid_children(),
        {ArtifactPackageIdentity{"split-runtime", "6.0-1"}},
        AurUpdateExecutionCorrelationFailureReason::
            SelectedAndUnselectedIdentityOverlap);

    run_returned_correlation_failure(
        "returned invalid unselected identity",
        "split-suite",
        valid_children(),
        {ArtifactPackageIdentity{"split-debug", ""}},
        AurUpdateExecutionCorrelationFailureReason::
            InvalidUnselectedArtifactIdentity);

    run_returned_correlation_failure(
        "returned duplicate unselected identity",
        "split-suite",
        valid_children(),
        {ArtifactPackageIdentity{"split-debug", "6.0-1"},
         ArtifactPackageIdentity{"split-debug", "6.0-1"}},
        AurUpdateExecutionCorrelationFailureReason::
            DuplicateUnselectedArtifactIdentity);
}

void expect_typed_failure_base(
    const AurUpdateSourceBuildExecutionResult& result,
    const std::string& diagnostic,
    const std::string& context) {
    expect(result.status == AurUpdateInvocationExecutionStatus::
                                StoppedOnWorkItemFailure &&
               !result.is_success(),
           context + ": invocation failure state differs");
    expect(result.work_item_results.size() == 1,
           context + ": work-item count differs");
    const auto& work_item = result.work_item_results.front();
    expect(work_item.status == AurUpdateWorkItemExecutionStatus::Failed &&
               work_item.failure_kind ==
                   AurUpdateWorkItemFailureKind::BuildOrInstallFailed &&
               work_item.diagnostic ==
                   std::optional<std::string>{diagnostic},
           context + ": work-item failure state differs");
    expect_no_fabricated_child_success(work_item, context);
}

template <typename Enqueue>
AurUpdateSourceBuildExecutionResult run_one_multiple_failure(
    const std::string& context,
    Enqueue enqueue_failure) {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/typed-failure/root", "/typed-failure/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        same_package_base_multiple_preflight(),
        false,
        config,
        database_paths);
    execution_stub::reset();
    enqueue_failure(expected_execution(
        0,
        "split-suite",
        {required_target(
             "split-suite",
             "split-runtime",
             DesiredInstallReason::Dependency),
         required_target(
             "split-suite",
             "split-explicit",
             DesiredInstallReason::Explicit)},
        false,
        database_paths,
        config));
    AurUpdateSourceBuildExecutionResult result = execute_without_escape(
        std::move(*preparation.invocation), config, context);
    execution_stub::require_script_consumed();
    return result;
}

void test_selection_mixed_reason_metadata_and_phase_failures_are_typed() {
    const std::string selection_context = "selection failure";
    const std::string selection_diagnostic = "typed selection failure";
    PackageBaseArtifactIdentitySelectionFailure selection_failure{};
    selection_failure.package_base = "split-suite";
    selection_failure.missing_required_artifacts.push_back(
        MissingRequiredArtifact{
            1,
            required_target(
                "split-suite",
                "split-explicit",
                DesiredInstallReason::Explicit)});
    const auto selection_result = run_one_multiple_failure(
        selection_context,
        [failure = std::move(selection_failure),
         selection_diagnostic](
            execution_stub::ExpectedExecution expected) mutable {
            execution_stub::enqueue_selection_failure(
                std::move(expected),
                std::move(failure),
                selection_diagnostic);
        });
    expect_typed_failure_base(
        selection_result,
        selection_diagnostic,
        selection_context);
    const auto& selection_detail = require_failure_detail<
        PackageBaseArtifactIdentitySelectionFailure>(
        selection_result.work_item_results.front(),
        selection_context);
    expect(selection_detail.package_base == "split-suite" &&
               selection_detail.missing_required_artifacts.size() == 1,
           "Selection failure lost its typed detail");

    const std::string mixed_diagnostic = "typed mixed reason failure";
    MixedPackageBaseInstallReasonUnsupported mixed_failure{};
    mixed_failure.package_base = "split-suite";
    const auto mixed_result = run_one_multiple_failure(
        "mixed reason failure",
        [failure = std::move(mixed_failure), mixed_diagnostic](
            execution_stub::ExpectedExecution expected) mutable {
            execution_stub::enqueue_mixed_reason_failure(
                std::move(expected),
                std::move(failure),
                mixed_diagnostic);
        });
    expect_typed_failure_base(
        mixed_result, mixed_diagnostic, "mixed reason failure");
    expect(require_failure_detail<
               MixedPackageBaseInstallReasonUnsupported>(
               mixed_result.work_item_results.front(),
               "mixed reason failure")
                   .package_base == "split-suite",
           "Mixed reason failure lost its typed detail");

    PackageBaseArtifactIdentitySelectionFailure mismatched_selection{};
    mismatched_selection.package_base = "other-suite";
    const auto mismatched_selection_result = run_one_multiple_failure(
        "selection failure PackageBase mismatch",
        [failure = std::move(mismatched_selection)](
            execution_stub::ExpectedExecution expected) mutable {
            execution_stub::enqueue_selection_failure(
                std::move(expected), std::move(failure),
                "mismatched selection failure");
        });
    expect(require_failure_detail<
               AurUpdateExecutionCorrelationFailure>(
               mismatched_selection_result.work_item_results.front(),
               "selection failure PackageBase mismatch")
                   .reason ==
               AurUpdateExecutionCorrelationFailureReason::
                   PackageBaseMismatch,
           "Selection failure PackageBase mismatch was not fail-closed");

    MixedPackageBaseInstallReasonUnsupported mismatched_mixed{};
    mismatched_mixed.package_base = "other-suite";
    const auto mismatched_mixed_result = run_one_multiple_failure(
        "mixed reason failure PackageBase mismatch",
        [failure = std::move(mismatched_mixed)](
            execution_stub::ExpectedExecution expected) mutable {
            execution_stub::enqueue_mixed_reason_failure(
                std::move(expected), std::move(failure),
                "mismatched mixed reason failure");
        });
    expect(require_failure_detail<
               AurUpdateExecutionCorrelationFailure>(
               mismatched_mixed_result.work_item_results.front(),
               "mixed reason failure PackageBase mismatch")
                   .reason ==
               AurUpdateExecutionCorrelationFailureReason::
                   PackageBaseMismatch,
           "Mixed reason failure PackageBase mismatch was not fail-closed");

    const PackageMetadataFailure metadata_failure{
        PackageMetadataErrorCode::QueryFailed,
        "typed metadata failure"};
    const auto metadata_result = run_one_multiple_failure(
        "metadata failure",
        [metadata_failure](execution_stub::ExpectedExecution expected) {
            execution_stub::enqueue_metadata_failure(
                std::move(expected), metadata_failure);
        });
    expect_typed_failure_base(
        metadata_result,
        metadata_failure.diagnostic,
        "metadata failure");
    expect(require_failure_detail<PackageMetadataFailure>(
               metadata_result.work_item_results.front(),
               "metadata failure")
                   .code == PackageMetadataErrorCode::QueryFailed,
           "Metadata failure lost its typed code");

    const ProductionSourceBuildStagedOutcome staged_metadata_outcome =
        reviewed_staged_outcome(
            ProductionSourceBuildCommandOutcome::Succeeded,
            ProductionSourceInstallOutcome::Failed);
    const auto staged_metadata_result = run_one_multiple_failure(
        "post-build metadata failure",
        [metadata_failure, staged_metadata_outcome](
            execution_stub::ExpectedExecution expected) {
            execution_stub::enqueue_phase_failure(
                std::move(expected),
                SeparatedPackageBaseSourceBuildFailurePhase::
                    InstallPreparation,
                metadata_failure.diagnostic, std::nullopt,
                metadata_failure, staged_metadata_outcome);
        });
    const AurUpdateWorkItemExecutionResult& staged_metadata =
        staged_metadata_result.work_item_results.front();
    expect(
        staged_metadata.production_outcome.has_value() &&
            staged_metadata.production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::Reviewed &&
            staged_metadata.production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            staged_metadata.production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Failed &&
            require_failure_detail<PackageMetadataFailure>(
                staged_metadata,
                "post-build metadata failure")
                    .code ==
                PackageMetadataErrorCode::QueryFailed,
        "AUR aggregate lost reviewed/build outcome or typed metadata failure");

    for(const auto& [phase, expected_category, diagnostic] :
        std::vector<std::tuple<
            SeparatedPackageBaseSourceBuildFailurePhase,
            AurUpdateSourceBuildFailureCategory,
            std::string>>{
            {SeparatedPackageBaseSourceBuildFailurePhase::Build,
             AurUpdateSourceBuildFailureCategory::Build,
             "typed build failure"},
            {SeparatedPackageBaseSourceBuildFailurePhase::
                 ArtifactValidation,
             AurUpdateSourceBuildFailureCategory::ArtifactValidation,
             "typed artifact validation failure"},
            {SeparatedPackageBaseSourceBuildFailurePhase::
                 ArtifactIdentity,
             AurUpdateSourceBuildFailureCategory::ArtifactIdentity,
             "typed artifact identity failure"}}) {
        const auto phase_result = run_one_multiple_failure(
            diagnostic,
            [phase, diagnostic](
                execution_stub::ExpectedExecution expected) {
                execution_stub::enqueue_phase_failure(
                    std::move(expected), phase, diagnostic);
            });
        expect_typed_failure_base(phase_result, diagnostic, diagnostic);
        expect(require_failure_detail<AurUpdateSourceBuildFailureSnapshot>(
                   phase_result.work_item_results.front(), diagnostic)
                       .category == expected_category,
               diagnostic + ": category differs");
    }

    const PackageBaseIdentity reviewed_package_base =
        PackageBaseIdentity::make(
            PackageSourceIdentity::aur(
                SourceLocationIdentity::known_git_remote(
                    "https://aur.archlinux.org/split-suite.git")),
            "split-suite");
    const ReviewedSourceState uncertain_state = ReviewedSourceState::make(
        reviewed_package_base,
        SourceRevisionIdentity::git_commit(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    const ReviewedSourceProductionFailure published_uncertain{
        ReviewedSourceProductionFailureStage::StatePublication,
        ReviewedSourceProductionFailureReason::PublishedUncertain,
        ReviewedSourcePublicationUncertain{
            ReviewedSourceStateStorePublishedUncertain{
                uncertain_state, std::nullopt,
                ReviewedSourceStatePostPublicationIssue::
                    DirectorySyncUncertain,
                ReviewedSourceStateStoreFailureKind::SyncFailed,
                "/state/split-suite/2.toml", std::nullopt,
                std::nullopt}}};
    const std::string uncertain_diagnostic =
        "typed reviewed publication uncertainty";
    const auto uncertain_result = run_one_multiple_failure(
        uncertain_diagnostic,
        [published_uncertain, uncertain_diagnostic](
            execution_stub::ExpectedExecution expected) {
            execution_stub::enqueue_phase_failure(
                std::move(expected),
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                uncertain_diagnostic, published_uncertain);
        });
    expect_typed_failure_base(
        uncertain_result, uncertain_diagnostic,
        uncertain_diagnostic);
    const auto& uncertain_snapshot = require_failure_detail<
        AurUpdateSourceBuildFailureSnapshot>(
        uncertain_result.work_item_results.front(),
        uncertain_diagnostic);
    expect(uncertain_snapshot.reviewed_source_failure.has_value() &&
               uncertain_snapshot.reviewed_source_failure->stage ==
                   ReviewedSourceProductionFailureStage::
                       StatePublication &&
               uncertain_snapshot.reviewed_source_failure->reason ==
                   ReviewedSourceProductionFailureReason::
                       PublishedUncertain,
           "AUR update aggregate flattened PublishedUncertain classification");
    const auto* uncertain_detail = std::get_if<
        ReviewedSourcePublicationUncertain>(
        &uncertain_snapshot.reviewed_source_failure->detail);
    const auto* expected_uncertain_detail = std::get_if<
        ReviewedSourcePublicationUncertain>(
        &published_uncertain.detail);
    expect(uncertain_detail != nullptr &&
               expected_uncertain_detail != nullptr &&
               uncertain_detail->store_result ==
                   expected_uncertain_detail->store_result,
           "AUR update aggregate lost PublishedUncertain store payload");
}

void test_transaction_failure_has_attempts_without_child_success_and_suffix() {
    const std::string context = "transaction failure";
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/transaction/root", "/transaction/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        multiple_then_tail_preflight(),
        false,
        config,
        database_paths);
    const std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts{
        transaction_attempt(
            "split-runtime",
            "7.0-1",
            DesiredInstallReason::Dependency),
        transaction_attempt(
            "split-explicit",
            "7.0-1",
            DesiredInstallReason::Explicit)};
    const std::string diagnostic = "pacman transaction failed";
    const ProductionSourceBuildStagedOutcome transaction_outcome =
        reviewed_staged_outcome(
            ProductionSourceBuildCommandOutcome::Succeeded,
            ProductionSourceInstallOutcome::Failed);

    execution_stub::reset();
    execution_stub::enqueue_transaction_failure(
        expected_execution(
            0,
            "split-suite",
            {required_target(
                 "split-suite",
                 "split-runtime",
                 DesiredInstallReason::Dependency),
             required_target(
                 "split-suite",
                 "split-explicit",
                 DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        PackageBaseArtifactInstallTransactionFailureKind::NonzeroExit,
        attempts,
        1,
        diagnostic,
        std::nullopt,
        transaction_outcome);

    const AurUpdateSourceBuildExecutionResult result =
        execute_without_escape(
            std::move(*preparation.invocation),
            config,
            "transaction failure");
    execution_stub::require_script_consumed();
    expect(result.work_item_results.size() == 2,
           "Transaction failure lost the planned suffix");
    const auto& failed = result.work_item_results[0];
    expect(failed.status == AurUpdateWorkItemExecutionStatus::Failed &&
               failed.diagnostic ==
                   std::optional<std::string>{diagnostic} &&
               failed.production_outcome.has_value() &&
               failed.production_outcome->source_provenance.review_status ==
                   ProductionSourceReviewStatus::Reviewed &&
               failed.production_outcome->build_outcome ==
                   ProductionSourceBuildCommandOutcome::Succeeded &&
               failed.production_outcome->install_outcome ==
                   ProductionSourceInstallOutcome::Failed,
           "Transaction failure aggregate differs");
    expect_no_fabricated_child_success(failed, "transaction failure");
    expect(failed.transaction_failure.has_value() &&
               failed.transaction_failure->attempted_artifacts.size() == 2,
           "Transaction failure lost safe attempt snapshots");
    const auto& transaction_failure = *failed.transaction_failure;
    expect(same_identity(
               transaction_failure.attempted_artifacts[0].identity,
               {"split-runtime", "7.0-1"}) &&
               transaction_failure.attempted_artifacts[0]
                       .desired_reason ==
                   DesiredInstallReason::Dependency &&
               same_identity(
                   transaction_failure.attempted_artifacts[1]
                       .identity,
                   {"split-explicit", "7.0-1"}) &&
               transaction_failure.attempted_artifacts[1]
                       .desired_reason ==
                   DesiredInstallReason::Explicit,
           "Transaction attempt identity/reason differs");
    const auto& detail = require_failure_detail<
        AurUpdatePackageTransactionFailureSnapshot>(
        failed, context);
    expect(detail.category ==
                   AurUpdatePackageTransactionFailureCategory::
                       CommandFailed &&
               detail.attempted_artifacts.size() == 2 &&
               detail.exit_code == std::optional<int>{1} &&
               transaction_failure.exit_code ==
                   std::optional<int>{1},
           "Transaction failure detail differs");

    const auto& suffix = result.work_item_results[1];
    expect(suffix.status == AurUpdateWorkItemExecutionStatus::NotAttempted &&
               suffix.failure_kind ==
                   AurUpdateWorkItemFailureKind::PriorWorkItemStopped &&
               suffix.child_results.size() == 1,
           "Transaction failure suffix aggregate differs");
    expect_child(
        suffix.child_results.front(),
        1,
        1,
        0,
        "tail-root",
        "tail-root",
        DesiredInstallReason::Explicit,
        {2},
        {{2, "tail-root"}},
        {PackageRole::Root},
        AurUpdateChildExecutionStatus::NotAttempted,
        std::nullopt,
        "transaction failure suffix child");
    expect(execution_stub::call_history().size() == 1,
           "Transaction failure executed a later work item");
    expect_events(
        {execution_stub::EventKind::Checkout,
         execution_stub::EventKind::Build,
         execution_stub::EventKind::Install},
        "transaction failure");

    AurUpdateSourceBuildPreparation mismatch_preparation = prepare_fixture(
        multiple_then_tail_preflight(),
        false,
        config,
        database_paths);
    execution_stub::reset();
    execution_stub::enqueue_transaction_failure(
        expected_execution(
            0,
            "split-suite",
            {required_target(
                 "split-suite",
                 "split-runtime",
                 DesiredInstallReason::Dependency),
             required_target(
                 "split-suite",
                 "split-explicit",
                 DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        PackageBaseArtifactInstallTransactionFailureKind::NonzeroExit,
        attempts,
        73,
        "mismatched transaction failure",
        "other-suite");
    const std::string mismatch_context =
        "transaction correlation mismatch";
    const AurUpdateSourceBuildExecutionResult mismatch_result =
        execute_without_escape(
            std::move(*mismatch_preparation.invocation),
            config,
            mismatch_context);
    execution_stub::require_script_consumed();
    const auto& mismatch = mismatch_result.work_item_results.front();
    expect_no_fabricated_child_success(
        mismatch, mismatch_context);
    const auto& mismatch_detail = require_failure_detail<
        AurUpdateExecutionCorrelationFailure>(
        mismatch, mismatch_context);
    expect(mismatch_detail.reason ==
                   AurUpdateExecutionCorrelationFailureReason::
                       PackageBaseMismatch &&
               mismatch.transaction_failure.has_value() &&
               mismatch.transaction_failure->category ==
                   AurUpdatePackageTransactionFailureCategory::
                       CommandFailed &&
               mismatch.transaction_failure->exit_code ==
                   std::optional<int>{73} &&
               mismatch.transaction_failure->attempted_artifacts.size() ==
                   2 &&
               mismatch_result.work_item_results[1].status ==
                   AurUpdateWorkItemExecutionStatus::NotAttempted,
           "Transaction correlation mismatch lost safe typed evidence");
}

void test_cleanup_failure_preserves_mixed_children_and_unselected_suffix() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/cleanup/root", "/cleanup/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        multiple_then_tail_preflight(),
        true,
        config,
        database_paths);
    const std::string diagnostic = "aggregate cleanup failed";

    execution_stub::reset();
    execution_stub::enqueue_cleanup_failure(
        expected_execution(
            0,
            "split-suite",
            {required_target(
                 "split-suite",
                 "split-runtime",
                 DesiredInstallReason::Dependency),
             required_target(
                 "split-suite",
                 "split-explicit",
                 DesiredInstallReason::Explicit)},
            true,
            database_paths,
            config),
        "split-suite",
        {selected_child(
             "split-runtime",
             "8.0-1",
             DesiredInstallReason::Dependency,
             ArtifactInstallExecutionOutcome::Installed),
         selected_child(
             "split-explicit",
             "8.0-1",
             DesiredInstallReason::Explicit,
             ArtifactInstallExecutionOutcome::SkippedAsNeeded)},
        {ArtifactPackageIdentity{"split-debug", "8.0-1"}},
        diagnostic);

    const AurUpdateSourceBuildExecutionResult result =
        execute_without_escape(
            std::move(*preparation.invocation),
            config,
            "cleanup mixed children");
    execution_stub::require_script_consumed();
    expect(result.status == AurUpdateInvocationExecutionStatus::
                                StoppedAfterPackageCleanupFailure &&
               !result.is_success() && result.changed_package_state() &&
               result.has_cleanup_failure() &&
               result.has_not_attempted_items(),
           "Cleanup failure invocation helpers differ");
    expect(result.stopped_work_item_index() == 0,
           "Cleanup failure stop index differs");

    const auto& completed = result.work_item_results[0];
    expect(completed.status ==
                   AurUpdateWorkItemExecutionStatus::
                       UpdatedCleanupFailed &&
               completed.failure_kind ==
                   AurUpdateWorkItemFailureKind::
                       CleanupFailedAfterPackageTransaction &&
               completed.diagnostic ==
                   std::optional<std::string>{diagnostic},
           "Cleanup failure aggregate differs");
    expect(completed.child_results[0].status ==
                   AurUpdateChildExecutionStatus::
                       InstalledCleanupFailed &&
               completed.child_results[1].status ==
                   AurUpdateChildExecutionStatus::
                       SkippedAsNeededCleanupFailed,
           "Cleanup failure flattened mixed child outcomes");
    expect(completed.unselected_artifacts.size() == 1 &&
               same_identity(
                   completed.unselected_artifacts.front(),
                   {"split-debug", "8.0-1"}),
           "Cleanup failure lost unselected identity snapshot");
    const auto& suffix = result.work_item_results[1];
    expect(suffix.status == AurUpdateWorkItemExecutionStatus::NotAttempted &&
               suffix.child_results.front().status ==
                   AurUpdateChildExecutionStatus::NotAttempted &&
               !suffix.child_results.front().selected_artifact.has_value(),
           "Cleanup failure executed or erased its suffix child");
    expect(execution_stub::call_history().size() == 1,
           "Cleanup failure executed a later work item");
}

void test_unknown_failure_is_contained_without_success() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/unknown/root", "/unknown/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        single_root_preflight(
            "unknown-root",
            DesiredInstallReason::Explicit,
            "unknown-root"),
        false,
        config,
        database_paths);
    execution_stub::reset();
    execution_stub::enqueue_unknown_failure(expected_execution(
        0,
        "unknown-root",
        {required_target(
            "unknown-root",
            "unknown-root",
            DesiredInstallReason::Explicit)},
        false,
        database_paths,
        config));

    const auto result = execute_without_escape(
        std::move(*preparation.invocation),
        config,
        "unknown failure");
    execution_stub::require_script_consumed();
    const auto& work_item = result.work_item_results.front();
    expect(work_item.failure_kind ==
                   AurUpdateWorkItemFailureKind::UnknownException &&
               work_item.diagnostic ==
                   std::optional<std::string>{
                       UNKNOWN_EXCEPTION_DIAGNOSTIC},
           "Unknown failure was not contained deterministically");
    expect_no_fabricated_child_success(work_item, "unknown failure");
}

void test_trusted_cache_failure_escapes_with_typed_detail() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/trusted-cache/root", "/trusted-cache/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        single_root_preflight(
            "trusted-cache-root",
            DesiredInstallReason::Explicit,
            "trusted-cache-root"),
        false,
        config,
        database_paths);
    execution_stub::reset();
    execution_stub::enqueue_trusted_cache_failure(
        expected_execution(
            0,
            "trusted-cache-root",
            {required_target(
                "trusted-cache-root",
                "trusted-cache-root",
                DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        TrustedCacheFailure{
            TrustedCacheStage::ChildValidation,
            TrustedCacheErrorCode::ConcurrentReplacement,
            std::nullopt});

    std::optional<TrustedCacheFailure> observed_failure;
    try {
        static_cast<void>(
            execute_prepared_aur_update_source_build_invocation(
                std::move(*preparation.invocation), config));
    } catch(const TrustedCacheError& error) {
        observed_failure = error.failure();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Trusted cache failure was flattened by the AUR update runner: " +
            std::string(error.what()));
    }

    expect(
        observed_failure.has_value() &&
            observed_failure->stage ==
                TrustedCacheStage::ChildValidation &&
            observed_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement &&
            !observed_failure->system_error.has_value(),
        "AUR update runner changed trusted cache failure detail");
    expect(
        execution_stub::call_history().size() == 1,
        "Trusted cache failure crossed an unexpected runner boundary");
    execution_stub::require_script_consumed();
}

void test_prepared_correlation_is_rejected_before_first_executor_call() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/prevalidation/root", "/prevalidation/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        three_singular_work_item_preflight(),
        false,
        config,
        database_paths);

    // Test hookから得たsnapshotを意図的に壊し、runner全件validationが
    // executor callより前に走ることだけを検証する。
    auto& production_invocation = const_cast<
        PreparedProductionSourceBuildInvocation&>(
        preparation.invocation->production_invocation_for_test());
    production_invocation.work_items.back()
        .required_targets.front()
        .package_name = "corrupted-tail";

    execution_stub::reset();
    bool rejected = false;
    try {
        static_cast<void>(
            execute_prepared_aur_update_source_build_invocation(
                std::move(*preparation.invocation), config));
    } catch(const std::logic_error&) {
        rejected = true;
    } catch(...) {
        throw std::runtime_error(
            "Prepared correlation mismatch raised an unexpected exception");
    }
    expect(rejected,
           "Prepared correlation mismatch was not rejected");
    expect(execution_stub::call_history().empty() &&
               execution_stub::event_history().empty(),
           "Prepared correlation mismatch reached the executor");
    execution_stub::require_script_consumed();
}

void test_moved_from_replay_and_unconsumed_expectation_are_rejected() {
    const AppConfig config = runner_config();
    const PacmanDatabasePaths database_paths{
        "/replay/root", "/replay/database"};
    AurUpdateSourceBuildPreparation preparation = prepare_fixture(
        single_root_preflight(
            "replay-root",
            DesiredInstallReason::Explicit,
            "replay-root"),
        false,
        config,
        database_paths);
    execution_stub::reset();
    execution_stub::enqueue_success(
        expected_execution(
            0,
            "replay-root",
            {required_target(
                "replay-root",
                "replay-root",
                DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        "replay-root",
        {selected_child(
            "replay-root",
            "9.0-1",
            DesiredInstallReason::Explicit,
            ArtifactInstallExecutionOutcome::Installed)});
    const auto first = execute_without_escape(
        std::move(*preparation.invocation), config, "first execution");
    expect(first.is_success(), "First one-shot execution failed");
    execution_stub::require_script_consumed();
    expect(!preparation.is_prepared() &&
               !preparation.invocation->is_valid(),
           "Consumed preparation retained an active capability");

    execution_stub::reset();
    bool replay_rejected = false;
    try {
        static_cast<void>(
            execute_prepared_aur_update_source_build_invocation(
                std::move(*preparation.invocation), config));
    } catch(const std::logic_error&) {
        replay_rejected = true;
    } catch(...) {
        throw std::runtime_error(
            "Moved-from replay raised an unexpected exception");
    }
    expect(replay_rejected, "Moved-from replay was not rejected");
    expect(execution_stub::call_history().empty(),
           "Moved-from replay reached the executor");
    execution_stub::require_script_consumed();

    execution_stub::enqueue_success(
        expected_execution(
            0,
            "never-called",
            {required_target(
                "never-called",
                "never-called",
                DesiredInstallReason::Explicit)},
            false,
            database_paths,
            config),
        "never-called",
        {selected_child(
            "never-called",
            "1-1",
            DesiredInstallReason::Explicit,
            ArtifactInstallExecutionOutcome::Installed)});
    bool unconsumed_rejected = false;
    try {
        execution_stub::require_script_consumed();
    } catch(const std::logic_error&) {
        unconsumed_rejected = true;
    }
    expect(unconsumed_rejected,
           "Strict stub accepted an unconsumed expectation");
    execution_stub::reset();
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        test_confirmation_stops_preserve_partial_and_control_flow();
        run_case(
            "ordinary size-one set owner",
            test_ordinary_size_one_uses_set_owner_strictly);
        run_case(
            "multiple work-item FIFO and DB snapshot",
            test_multiple_work_items_preserve_fifo_call_order_and_one_db_snapshot);
        run_case(
            "selected repository provider phase transaction",
            test_selected_repository_provider_transaction_precedes_source_and_stops_failure);
        run_case(
            "later fatal preflight blocks aggregate mutation",
            test_later_fatal_preflight_blocks_cache_provider_and_first_work_item);
        run_case(
            "requested split child size-one",
            test_requested_split_child_size_one_is_no_change);
        run_case(
            "multiple child exact result",
            test_multiple_children_preserve_order_reason_outcome_and_attribution);
        run_case(
            "returned result correlation failures",
            test_returned_result_correlation_failures_are_fail_closed);
        run_case(
            "typed pre-transaction failures",
            test_selection_mixed_reason_metadata_and_phase_failures_are_typed);
        run_case(
            "transaction failure and NotAttempted suffix",
            test_transaction_failure_has_attempts_without_child_success_and_suffix);
        run_case(
            "cleanup mixed children and NotAttempted suffix",
            test_cleanup_failure_preserves_mixed_children_and_unselected_suffix);
        run_case(
            "unknown failure containment",
            test_unknown_failure_is_contained_without_success);
        run_case(
            "trusted cache failure propagation",
            test_trusted_cache_failure_escapes_with_typed_detail);
        run_case(
            "prepared correlation prevalidation",
            test_prepared_correlation_is_rejected_before_first_executor_call);
        run_case(
            "one-shot replay and strict expectation",
            test_moved_from_replay_and_unconsumed_expectation_are_rejected);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "AUR update execution runner tests: all checks passed\n";
    return 0;
}

bool revalidate_devel_tracking_bootstrap(const DevelTrackingBootstrapTrial&) {
    throw std::logic_error("Runner fixture has no bootstrap observation authority.");
}
