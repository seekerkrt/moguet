#include "app_config.hpp"
#include "artifact_install_executor.hpp"
#include "commands_sync.hpp"
#include "filtered_aur_update_operation.hpp"
#include "stubs/aur-update-execution-preflight/preflight_stub.hpp"
#include "stubs/aur-update-execution-preparation/preparation_stub.hpp"
#include "stubs/aur-update-execution-runner/execution_stub.hpp"
#include "stubs/filtered-aur-update-operation/query_stub.hpp"
#include "system_aur_update_operation.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

template <typename T>
concept HasCacheCapability = requires(T value) { value.cache_root; };

template <typename T>
concept HasReviewedStateCapability = requires(T value) {
    value.reviewed_state_preflight;
};

static_assert(!std::is_default_constructible_v<
              PreparedFilteredAurUpdateOperation>);
static_assert(!std::is_copy_constructible_v<
              PreparedFilteredAurUpdateOperation>);
static_assert(std::is_nothrow_move_constructible_v<
              PreparedFilteredAurUpdateOperation>);
static_assert(!std::is_move_assignable_v<
              PreparedFilteredAurUpdateOperation>);
static_assert(!std::is_copy_constructible_v<
              PreparedSystemAurUpdateOperation>);
static_assert(std::is_nothrow_move_constructible_v<
              PreparedSystemAurUpdateOperation>);
static_assert(!std::is_move_assignable_v<
              PreparedSystemAurUpdateOperation>);
static_assert(!std::is_constructible_v<
              PreparedFilteredAurUpdateOperation,
              FilteredAurUpdateObservation>);
static_assert(!std::is_constructible_v<
              PreparedSystemAurUpdateOperation,
              SystemAurUpdateDryRunObservation>);
static_assert(!std::is_constructible_v<
              PreparedProductionSourceBuildInvocation,
              ProductionSourceBuildPreparationObservation>);
static_assert(!HasCacheCapability<ProductionSourceBuildWorkItemObservation>);
static_assert(!HasReviewedStateCapability<SourceBuildRequestObservation>);

namespace system_aur_update_repository_test_stub {

struct State {
    int command_exit_status = 0;
    std::vector<std::vector<std::string>> ordered_argument_calls;
    std::vector<bool> no_confirm_calls;
    std::function<void()> after_success;
};

State g_state;

void reset() {
    g_state = State{};
}

void set_command_exit_status(int status) {
    g_state.command_exit_status = status;
}

void set_after_success(std::function<void()> callback) {
    g_state.after_success = std::move(callback);
}

const std::vector<std::vector<std::string>>& ordered_argument_calls() {
    return g_state.ordered_argument_calls;
}

const std::vector<bool>& no_confirm_calls() {
    return g_state.no_confirm_calls;
}

} // namespace system_aur_update_repository_test_stub

int execute_ordered_repository_sync_transaction(
    const std::vector<std::string>& ordered_pacman_args,
    const AppConfig& config) {
    namespace repository_stub =
        system_aur_update_repository_test_stub;
    repository_stub::g_state.ordered_argument_calls.push_back(
        ordered_pacman_args);
    repository_stub::g_state.no_confirm_calls.push_back(
        config.no_confirm);
    const int status = repository_stub::g_state.command_exit_status;
    if(status == 0 && repository_stub::g_state.after_success) {
        repository_stub::g_state.after_success();
    }
    return status;
}

namespace {

namespace preflight_stub = aur_update_execution_preflight_test_stub;
namespace preparation_stub = aur_update_execution_preparation_test_stub;
namespace execution_stub = aur_update_execution_runner_test_stub;
namespace query_stub = filtered_aur_update_operation_query_test_stub;
namespace repository_stub = system_aur_update_repository_test_stub;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename ExpectedException, typename Callable>
void expect_exception(Callable callable, const std::string& context) {
    try {
        callable();
    } catch(const ExpectedException&) {
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " +
            error.what());
    }
    throw std::runtime_error(context + ": expected exception was not raised");
}

void reset_stubs() {
    repository_stub::reset();
    query_stub::reset();
    preflight_stub::reset_preflight_stub();
    preparation_stub::reset();
    execution_stub::reset();
}

PreparedFilteredAurUpdateOperation prepare_strict_filtered_aur_update_operation(
    AurUpdateQueryResult query_result,
    FilteredAurUpdateExplicitSourceSatisfaction
        explicit_source_satisfaction,
    const AppConfig& config,
    std::optional<ValidatedCacheRoot> cache_root = std::nullopt) {
    return ::prepare_filtered_aur_update_operation(
        std::move(query_result), std::move(explicit_source_satisfaction),
        DevelRequiresCheckPolicy::BlockOperation,
        SavedSourcePreferencePolicy::Strict, config,
        std::move(cache_root));
}

UpgradeAllExplicitSourceSatisfaction explicit_satisfaction(
    std::vector<UpgradeAllExplicitSourceIdentity> identities) {
    return UpgradeAllExplicitSourceSatisfaction{std::move(identities)};
}

AurUpdatePlanEntry update_entry(
    const std::string& package_name,
    const std::string& package_base = {},
    InstalledPackageReason reason = InstalledPackageReason::Explicit) {
    const std::string resolved_base =
        package_base.empty() ? package_name : package_base;
    return AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        reason,
        AurUpdateRemotePackage{
            package_name,
            resolved_base,
            "2.0-1",
            AurVersionRelation::NewerThanInstalled},
        AurUpdateClassification::UpdateAvailable};
}

AurUpdatePlanEntry current_entry(const std::string& package_name) {
    return AurUpdatePlanEntry{
        package_name,
        "2.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            package_name,
            "2.0-1",
            AurVersionRelation::SameAsInstalled},
        AurUpdateClassification::UpToDate};
}

AurUpdatePlanEntry requires_check_entry(
    const std::string& package_name,
    const std::string& package_base = {}) {
    const std::string resolved_base =
        package_base.empty() ? package_name : package_base;
    return classify_aur_update(AurUpdatePlanInput{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            resolved_base,
            "1.0-1",
            AurVersionRelation::SameAsInstalled}});
}

AurUpdatePlanEntry classified_update_entry(
    const std::string& package_name) {
    return classify_aur_update(AurUpdatePlanInput{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            package_name,
            package_name,
            "2.0-1",
            AurVersionRelation::NewerThanInstalled}});
}

AurUpdatePlanEntry non_aur_entry(const std::string& package_name) {
    return AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        InstalledPackageReason::Dependency,
        std::nullopt,
        AurUpdateClassification::NonAurForeign};
}

AurUpdatePlanEntry unavailable_entry(const std::string& package_name) {
    return AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        InstalledPackageReason::Explicit,
        std::nullopt,
        AurUpdateClassification::MetadataUnavailable};
}

AurUpdateQueryResult query_result(
    std::vector<AurUpdatePlanEntry> entries,
    std::vector<AurUpdateQueryFailure> failures = {}) {
    return AurUpdateQueryResult{
        AurUpdatePlan{std::move(entries)}, std::move(failures)};
}

UpgradeAllExplicitSourceIdentity explicit_source(
    const std::string& preference_name,
    const std::string& package_base,
    std::vector<std::string> produced_names = {},
    const std::string& source_key = {}) {
    return UpgradeAllExplicitSourceIdentity{
        preference_name,
        UpgradeAllResolvedPackageBase{package_base},
        std::move(produced_names),
        UpgradeAllResolvedSourceIdentity{
            source_key.empty()
                ? "source://" + preference_name
                : source_key}};
}

AurPackageInfo package_info(
    const std::string& package_name,
    const std::string& package_base = {},
    const std::string& version = "2.0-1") {
    AurPackageInfo info;
    info.Name = package_name;
    info.PackageBase = package_base.empty() ? package_name : package_base;
    info.Version = version;
    info.Description = "filtered AUR operation fixture";
    info.Maintainer = "moguet-test";
    return info;
}

struct RootSpec {
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
    return found == plan.package_targets.end() ? nullptr : &*found;
}

const PlannedPackageTarget& require_package_target(
    const BuildPlan& plan, const std::string& package_name) {
    auto found = std::find_if(
        plan.package_targets.begin(), plan.package_targets.end(),
        [&package_name](const PlannedPackageTarget& target) {
            return target.package_name == package_name;
        });
    if(found == plan.package_targets.end()) {
        throw std::logic_error(
            "BuildPlan fixture parent is missing: " + package_name);
    }
    return *found;
}

template <typename Value>
void append_unique(std::vector<Value>& values, const Value& value) {
    if(std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

BuildPlan root_plan(std::vector<RootSpec> roots) {
    BuildPlan plan;
    for(std::size_t index = 0; index < roots.size(); ++index) {
        const RootSpec& fixture = roots[index];
        const RootTargetIdentity root{index, fixture.package_name};
        plan.root_targets.push_back(root);
        plan.package_targets.push_back(PlannedPackageTarget{
            fixture.package_name,
            fixture.package_base,
            {PackageRole::Root},
            {root}});

        auto same_base = [&fixture](const BuildPlanEntry& entry) {
            return entry.package_base == fixture.package_base;
        };
        auto order = std::find_if(
            plan.order.begin(), plan.order.end(), same_base);
        if(order == plan.order.end()) {
            plan.order.push_back(BuildPlanEntry{
                fixture.package_base, {fixture.package_name}});
        } else {
            append_unique(order->package_names, fixture.package_name);
        }
    }
    return plan;
}

// dependency edgeとdeclared role/rootを同時に足し、actual preflightのgraph
// consistency checkを通るfixtureだけを作る。
void add_aur_dependency(
    BuildPlan& plan,
    const std::string& parent_package_name,
    const std::string& dependency_package_name,
    const std::string& dependency_package_base,
    PackageRole role = PackageRole::RuntimeDependency) {
    const PlannedPackageTarget& parent =
        require_package_target(plan, parent_package_name);
    const std::string parent_package_base = parent.package_base;
    const std::vector<RootTargetIdentity> parent_roots = parent.roots;

    PlannedPackageTarget* dependency =
        find_package_target(plan, dependency_package_name);
    if(dependency == nullptr) {
        plan.package_targets.push_back(PlannedPackageTarget{
            dependency_package_name,
            dependency_package_base,
            {role},
            parent_roots});
    } else {
        expect(
            dependency->package_base == dependency_package_base,
            "BuildPlan fixture dependency PackageBase differs");
        append_unique(dependency->roles, role);
        for(const RootTargetIdentity& root : parent_roots) {
            append_unique(dependency->roots, root);
        }
    }

    auto same_base = [&dependency_package_base](const BuildPlanEntry& entry) {
        return entry.package_base == dependency_package_base;
    };
    auto order = std::find_if(plan.order.begin(), plan.order.end(), same_base);
    if(order == plan.order.end()) {
        plan.order.insert(
            plan.order.begin(),
            BuildPlanEntry{
                dependency_package_base,
                {dependency_package_name}});
    } else {
        append_unique(order->package_names, dependency_package_name);
    }

    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        parent_package_name,
        parent_package_base,
        dependency_package_name,
        role,
        DependencyKind::Aur,
        dependency_package_name,
        dependency_package_base,
        std::nullopt});
}

void return_build_plan(
    BuildPlan plan, std::vector<std::string> expected_targets) {
    preflight_stub::set_resolver_handler(
        [plan = std::move(plan),
         expected_targets = std::move(expected_targets)](
            const std::vector<std::string>& targets) {
            if(targets != expected_targets) {
                throw std::logic_error(
                    "Filtered operation resolver target order differs");
            }
            return plan;
        });
}

const PreparedProductionSourceBuildInvocation& require_production_invocation(
    const PreparedFilteredAurUpdateOperation& prepared) {
    expect(
        prepared.source_build_preparation().has_value() &&
            prepared.source_build_preparation()
                ->invocation.has_value(),
        "Filtered operation fixture has no production invocation");
    return prepared.source_build_preparation()
        ->invocation->production_invocation_for_test();
}

execution_stub::ExpectedExecution expected_execution_at(
    const PreparedFilteredAurUpdateOperation& prepared,
    std::size_t work_item_index,
    const AppConfig& config) {
    const PreparedProductionSourceBuildInvocation& invocation =
        require_production_invocation(prepared);
    expect(
        work_item_index < invocation.work_items.size(),
        "Filtered operation execution expectation index is out of range");
    const ProductionSourceBuildWorkItem& work_item =
        invocation.work_items[work_item_index];
    return execution_stub::ExpectedExecution{
        work_item_index,
        work_item.request.checkout_name,
        work_item.required_targets,
        work_item.request.needed,
        invocation.database_paths,
        config};
}

std::vector<PackageBaseSourceBuildSelectedResult> selected_children_at(
    const PreparedFilteredAurUpdateOperation& prepared,
    std::size_t work_item_index,
    ArtifactInstallExecutionOutcome outcome) {
    const PreparedProductionSourceBuildInvocation& invocation =
        require_production_invocation(prepared);
    expect(
        work_item_index < invocation.work_items.size(),
        "Filtered operation selected result index is out of range");
    std::vector<PackageBaseSourceBuildSelectedResult> children;
    for(const auto& target :
        invocation.work_items[work_item_index].required_targets) {
        children.push_back(PackageBaseSourceBuildSelectedResult{
            ArtifactPackageIdentity{target.package_name, "2.0-1"},
            target.desired_reason,
            outcome});
    }
    return children;
}

void enqueue_outcome(
    const PreparedFilteredAurUpdateOperation& prepared,
    const AppConfig& config,
    std::size_t count,
    ArtifactInstallExecutionOutcome outcome) {
    const PreparedProductionSourceBuildInvocation& invocation =
        require_production_invocation(prepared);
    expect(
        count <= invocation.work_items.size(),
        "Filtered operation execution expectation count is out of range");
    for(std::size_t index = 0; index < count; ++index) {
        const std::string package_base =
            invocation.work_items[index].request.checkout_name;
        execution_stub::enqueue_success(
            expected_execution_at(prepared, index, config),
            package_base,
            selected_children_at(prepared, index, outcome));
    }
}

void enqueue_installed(
    const PreparedFilteredAurUpdateOperation& prepared,
    const AppConfig& config,
    std::size_t count) {
    enqueue_outcome(
        prepared,
        config,
        count,
        ArtifactInstallExecutionOutcome::Installed);
}

void enqueue_no_change(
    const PreparedFilteredAurUpdateOperation& prepared,
    const AppConfig& config,
    std::size_t count) {
    enqueue_outcome(
        prepared,
        config,
        count,
        ArtifactInstallExecutionOutcome::SkippedAsNeeded);
}

bool has_operation_issue(
    const std::vector<FilteredAurUpdateOperationIssue>& issues,
    FilteredAurUpdateOperationIssueKind expected) {
    return std::any_of(
        issues.begin(), issues.end(),
        [expected](const FilteredAurUpdateOperationIssue& issue) {
            return issue.kind == expected;
        });
}

bool has_planner_issue(
    const UpgradeAllPlan& plan,
    UpgradeAllPlanningIssueKind expected) {
    return std::any_of(
        plan.issues.begin(), plan.issues.end(),
        [expected](const UpgradeAllPlanningIssue& issue) {
            return issue.kind == expected;
        });
}

void expect_no_mutation(const std::string& context) {
    expect(
        execution_stub::call_history().empty(),
        context + ": source-build executor was called");
    expect(
        execution_stub::event_history().empty(),
        context + ": source-build lifecycle event was emitted");
    expect(
        execution_stub::invocation_event_history().empty(),
        context +
            ": provider/cache/source invocation event was emitted");
}

void expect_statuses(
    const FilteredAurUpdateExecutionResult& result,
    const std::vector<AurUpdateOperationTargetStatus>& expected,
    const std::string& context) {
    expect(
        result.selected_target_results.size() == expected.size(),
        context + ": selected result count differs");
    for(std::size_t index = 0; index < expected.size(); ++index) {
        expect(
            result.selected_target_results[index]
                    .operation_result.status == expected[index],
            context + ": target status differs at " +
                std::to_string(index));
    }
}

bool same_config(const AppConfig& lhs, const AppConfig& rhs) {
    return lhs.user_config.schema_version == rhs.user_config.schema_version &&
           lhs.user_config.review.pkgbuild ==
               rhs.user_config.review.pkgbuild &&
           lhs.user_config.review.diff == rhs.user_config.review.diff &&
           lhs.user_config.build.mode == rhs.user_config.build.mode &&
           lhs.no_confirm == rhs.no_confirm &&
           lhs.rm_deps == rhs.rm_deps &&
           lhs.editor == rhs.editor;
}

std::string prepared_diagnostic(
    const PreparedFilteredAurUpdateOperation& prepared) {
    std::string diagnostic =
        " operation-issues=" +
        std::to_string(prepared.operation_issues().size()) +
        " planner-issues=" +
        std::to_string(
            prepared.target_and_build_unit_plan().issues.size());
    if(!prepared.operation_issues().empty()) {
        diagnostic += " operation-first=" +
                      prepared.operation_issues().front().diagnostic;
    }
    if(prepared.source_build_preparation().has_value()) {
        diagnostic += " preparation-issues=" +
                      std::to_string(
                          prepared.source_build_preparation()->issues.size());
        if(!prepared.source_build_preparation()->issues.empty()) {
            diagnostic += " preparation-first=" +
                          prepared.source_build_preparation()
                              ->issues.front()
                              .diagnostic;
        }
    }
    return diagnostic;
}

void expect_success_lifecycle(
    const std::vector<execution_stub::ExecutionCall>& calls,
    const std::vector<std::string>& expected_package_names,
    const AppConfig& expected_config,
    const std::string& context) {
    const std::vector<execution_stub::EventKind> expected_events{
        execution_stub::EventKind::Checkout,
        execution_stub::EventKind::Build,
        execution_stub::EventKind::Install,
        execution_stub::EventKind::Cleanup};
    expect(calls.size() == expected_package_names.size(),
           context + ": executor call count differs");
    for(std::size_t index = 0; index < calls.size(); ++index) {
        expect(
            calls[index].call_index == index &&
                calls[index].package_name ==
                    expected_package_names[index] &&
                calls[index].events == expected_events &&
                same_config(calls[index].config, expected_config),
            context + ": call identity/options/lifecycle differs at " +
                std::to_string(index));
    }

    const std::vector<execution_stub::Event>& events =
        execution_stub::event_history();
    expect(
        events.size() == expected_package_names.size() *
                             expected_events.size(),
        context + ": global lifecycle count differs");
    for(std::size_t call_index = 0;
        call_index < expected_package_names.size(); ++call_index) {
        for(std::size_t event_index = 0;
            event_index < expected_events.size(); ++event_index) {
            const execution_stub::Event& event = events[call_index * expected_events.size() + event_index];
            expect(
                event.call_index == call_index &&
                    event.kind == expected_events[event_index] &&
                    event.package_name ==
                        expected_package_names[call_index],
                context + ": global lifecycle order differs");
        }
    }
}

PreparedSystemAurUpdateOperation prepare_system_aur_update_fixture(
    std::vector<std::string> ordered_pacman_args = {"-Syu"}) {
    AutoSystemUpdateRouteCandidate candidate{
        CompatibleAutoSystemUpdatePacmanArguments{},
        std::move(ordered_pacman_args)};
    std::optional<CompatibleSystemAurUpdateRequest> request =
        make_compatible_system_aur_update_request(
            std::move(candidate));
    expect(
        request.has_value(),
        "Compatible system+AUR fixture request was rejected");

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_operation(
            std::move(request.value()));
    expect(
        prepared.is_valid(),
        "System+AUR fixture did not create a valid prepared operation");
    return prepared;
}

SystemAurUpdateDryRunRequest make_auto_system_aur_dry_run_request(
    std::vector<std::string> ordered_pacman_args = {"-Syu"},
    bool repository_needed = false) {
    std::optional<SystemAurUpdateDryRunRequest> request =
        SystemAurUpdateDryRunRequest::from_auto_candidate(
            AutoSystemUpdateRouteCandidate{
                CompatibleAutoSystemUpdatePacmanArguments{},
                std::move(ordered_pacman_args), repository_needed});
    expect(
        request.has_value(),
        "Compatible system+AUR dry-run fixture request was rejected");
    return std::move(request.value());
}

const FilteredAurUpdateObservation& require_system_aur_dry_run_child(
    const SystemAurUpdateDryRunObservation& observation,
    const char* context) {
    expect(
        observation.aur_observation.has_value(),
        std::string(context) +
            ": dry-run aggregate lost its current-state AUR observation");
    return observation.aur_observation.value();
}

void expect_no_system_aur_dry_run_mutation(const std::string& context) {
    expect(
        repository_stub::ordered_argument_calls().empty() &&
            repository_stub::no_confirm_calls().empty(),
        context + ": repository transaction was executed");
    expect(
        execution_stub::call_history().empty(),
        context + ": source-build executor was called");
    expect(
        execution_stub::event_history().empty(),
        context + ": checkout/build/install/cleanup event was emitted");
    expect(
        execution_stub::invocation_event_history().empty(),
        context +
            ": provider/cache/source invocation event was emitted");
}

void expect_no_system_aur_later_authority(
    const std::string& context) {
    expect(
        query_stub::repository_configuration_calls() == 0 &&
            query_stub::inventory_calls() == 0 &&
            query_stub::info_many_call_history().empty() &&
            query_stub::info_strict_call_history().empty() &&
            query_stub::vercmp_call_history().empty(),
        context + ": fresh inventory or AUR query was called");
    expect(
        preflight_stub::resolver_call_count() == 0,
        context + ": provider/preflight resolver was called");
    expect(
        preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history().empty() &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::database_call_count() == 0 &&
            preparation_stub::reviewed_state_preflight_call_count() == 0,
        context + ": AUR source preparation authority was called");
    expect_no_mutation(context);
}

void require_no_pre_repository_authority() {
    if(query_stub::repository_configuration_calls() != 0 ||
       query_stub::inventory_calls() != 0 ||
       !query_stub::info_many_call_history().empty() ||
       preflight_stub::resolver_call_count() != 0 ||
       !preparation_stub::strict_preference_read_history().empty() ||
       preparation_stub::
               source_preference_directory_snapshot_call_count() !=
           0 ||
       preparation_stub::database_call_count() != 0 ||
       !execution_stub::call_history().empty()) {
        throw std::logic_error(
            "System+AUR authority was prepared before repository success");
    }
}

execution_stub::ExpectedExecution expected_system_aur_execution(
    std::size_t work_item_index,
    const std::string& package_name,
    const std::string& package_base,
    InstalledPackageReason installed_reason,
    const AppConfig& config,
    PacmanDatabasePaths database_paths =
        PacmanDatabasePaths{"/stub/root", "/stub/database"}) {
    const DesiredInstallReason desired_reason =
        installed_reason == InstalledPackageReason::Explicit
            ? DesiredInstallReason::Explicit
            : DesiredInstallReason::Dependency;
    return execution_stub::ExpectedExecution{
        work_item_index,
        package_base,
        {RequiredPackageArtifactTarget{
            package_base, package_name, desired_reason}},
        false,
        std::move(database_paths),
        config};
}

std::vector<PackageBaseSourceBuildSelectedResult>
selected_system_aur_child(
    const std::string& package_name,
    InstalledPackageReason installed_reason,
    ArtifactInstallExecutionOutcome outcome =
        ArtifactInstallExecutionOutcome::Installed) {
    return {PackageBaseSourceBuildSelectedResult{
        ArtifactPackageIdentity{package_name, "2.0-1"},
        installed_reason == InstalledPackageReason::Explicit
            ? DesiredInstallReason::Explicit
            : DesiredInstallReason::Dependency,
        outcome}};
}

const FilteredAurUpdateExecutionResult& require_system_aur_child(
    const SystemAurUpdateOperationResult& result,
    const char* context) {
    expect(
        result.aur.operation_result.has_value(),
        std::string(context) +
            ": aggregate lost its filtered AUR child");
    return result.aur.operation_result.value();
}

bool has_preflight_issue(
    const AurUpdateExecutionTarget& target,
    AurUpdateExecutionReason reason) {
    return std::any_of(
        target.issues.begin(), target.issues.end(),
        [reason](const AurUpdateExecutionIssue& issue) {
            return issue.reason == reason;
        });
}

void enqueue_exact_update_query(
    const std::vector<RootSpec>& roots,
    const std::string& version_relation = "1") {
    std::map<std::string, AurPackageInfo> packages;
    for(const RootSpec& root : roots) {
        packages.emplace(
            root.package_name,
            package_info(root.package_name, root.package_base));
    }
    query_stub::enqueue_info_many_result(std::move(packages));
    for(std::size_t index = 0; index < roots.size(); ++index) {
        query_stub::enqueue_vercmp_result(version_relation);
    }
}

ForeignPackageInventory inventory_for_roots(
    const std::vector<RootSpec>& roots,
    InstalledPackageReason reason = InstalledPackageReason::Explicit,
    const std::string& version = "1.0-1") {
    ForeignPackageInventory inventory;
    inventory.reserve(roots.size());
    for(const RootSpec& root : roots) {
        inventory.push_back(
            InstalledPackageMetadata{root.package_name, version, reason});
    }
    return inventory;
}

BuildPlan root_plan_with_selected_repository_provider(
    const RootSpec& root,
    const std::string& dependency,
    const ProvidedDependency& provider) {
    BuildPlan plan = root_plan({root});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        root.package_name,
        root.package_base,
        dependency,
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        provider,
        ProviderResolutionKind::UserSelected});
    return plan;
}

void test_system_aur_dry_run_auto_observes_current_update_without_capability() {
    reset_stubs();
    const RootSpec root{"dry-current-child", "dry-current-base"};
    const PacmanRepositoryConfiguration current_configuration{
        PacmanDatabasePaths{"/dry/root", "/dry/database"},
        {"dry-current-repository"}};
    query_stub::set_repository_configuration(current_configuration);
    query_stub::set_foreign_inventory(
        inventory_for_roots(
            {root}, InstalledPackageReason::Dependency, "1.0-7"));
    enqueue_exact_update_query({root});
    return_build_plan(root_plan({root}), {root.package_name});
    preparation_stub::enqueue_source_preference_result(
        root.package_name,
        SourcePreferenceFailure{
            SourcePreferenceFailureKind::UnsupportedFileType,
            "/stub/preferences/dry-current-child",
            std::nullopt,
            std::filesystem::file_type::directory,
            "must remain unread"});
    preparation_stub::enqueue_source_preference_result(
        root.package_base,
        SourcePreferenceFailure{
            SourcePreferenceFailureKind::ReadFailed,
            "/stub/preferences/dry-current-base",
            std::make_error_code(std::errc::io_error),
            std::nullopt,
            "must remain unread"});

    AppConfig config;
    config.no_confirm = true;
    config.editor = "dry-current-editor";
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(
                {"-Syu", "--needed"}, true),
            config);
    const FilteredAurUpdateObservation& child =
        require_system_aur_dry_run_child(
            observation, "system+AUR current update observation");
    expect(
        observation.request.mode() ==
                SystemAurUpdateDryRunMode::Auto &&
            observation.request.ordered_pacman_args() ==
                std::vector<std::string>{"-Syu", "--needed"} &&
            observation.request.repository_needed() &&
            observation.aur_observation_basis ==
                std::optional<SystemAurUpdateDryRunAurObservationBasis>{
                    SystemAurUpdateDryRunAurObservationBasis::
                        CurrentInstalledState} &&
            observation.actual_authority_refresh ==
                std::optional<SystemAurUpdateDryRunActualAuthorityRefresh>{
                    SystemAurUpdateDryRunActualAuthorityRefresh::
                        AfterRepositorySuccess} &&
            observation.explicit_source_satisfaction.has_value() &&
            observation.saved_source_preference_policy ==
                std::optional<SavedSourcePreferencePolicy>{
                    SavedSourcePreferencePolicy::Ignore} &&
            observation.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::
                        SkipIndependentTarget},
        "Auto dry-run lost route, freshness, or Ignore authority");
    expect(
        observation.is_ready() && !observation.is_blocked() &&
            observation.has_current_aur_update_intent() &&
            observation.issues.empty() && child.is_ready() &&
            !child.is_noop() && !child.is_blocked(),
        "Current AUR update candidate was not a ready observation");
    expect(
        observation.repository_configuration.has_value() &&
            observation.repository_configuration->database_paths.root_dir ==
                current_configuration.database_paths.root_dir &&
            observation.repository_configuration->database_paths.db_path ==
                current_configuration.database_paths.db_path &&
            observation.repository_configuration->repository_names ==
                current_configuration.repository_names &&
            observation.foreign_inventory.size() == 1 &&
            observation.foreign_inventory.front().name ==
                root.package_name &&
            observation.foreign_inventory.front().version == "1.0-7" &&
            observation.foreign_inventory.front().reason ==
                InstalledPackageReason::Dependency &&
            child.original_query_result().plan.entries.size() == 1 &&
            child.original_query_result()
                .plan.entries.front()
                .aur_package.has_value() &&
            child.original_query_result()
                    .plan.entries.front()
                    .aur_package->package_base == root.package_base,
        "Auto dry-run lost current installed or exact AUR identity");
    expect(
        query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {root.package_name}} &&
            query_stub::vercmp_call_history().size() == 1 &&
            preflight_stub::resolver_calls() ==
                std::vector<std::vector<std::string>>{
                    {root.package_name}},
        "Auto dry-run did not use one exact current-state query/preflight");

    const AurUpdateSourceBuildObservation& source_observation =
        child.source_build_preflight().value();
    expect(
        source_observation.is_ready() &&
            source_observation.production_preflight.has_value() &&
            source_observation.production_preflight->work_items.size() ==
                1,
        "Auto dry-run did not retain a capability-free source observation");
    const ProductionSourceBuildWorkItemObservation& work_item =
        source_observation.production_preflight->work_items.front();
    expect(
        work_item.request.package_name == root.package_name &&
            work_item.request.checkout_name == root.package_base &&
            !work_item.request.needed &&
            work_item.request.custom_environment
                .ordered_assignments.empty() &&
            work_item.required_targets.size() == 1 &&
            work_item.required_targets.front().package_name ==
                root.package_name &&
            work_item.required_targets.front().desired_reason ==
                DesiredInstallReason::Dependency &&
            child.upgrade_all_plan.explicit_sources.empty() &&
            work_item.request.reviewed_state ==
                ReviewedSourceFatalStateObservationStatus::Completed,
        "Auto dry-run minted cache authority or changed observed work-item identity");
    expect(
        preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history() ==
                std::vector<bool>{false} &&
            preparation_stub::pkgdest_guard_history().size() == 2 &&
            std::all_of(
                preparation_stub::pkgdest_guard_history().begin(),
                preparation_stub::pkgdest_guard_history().end(),
                [](const SourceBuildEnvironment& environment) {
                    return environment.ordered_assignments.empty();
                }) &&
            preparation_stub::database_call_count() == 1 &&
            preparation_stub::reviewed_state_preflight_call_count() == 1,
        "Auto dry-run crossed saved preference IO or skipped read-only safety");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR current update observation");

    const StrictSourcePreferenceResult unread_child_failure =
        read_source_preference_strict(root.package_name);
    const StrictSourcePreferenceResult unread_base_failure =
        read_source_preference_strict(root.package_base);
    expect(
        std::holds_alternative<SourcePreferenceFailure>(
            unread_child_failure) &&
            std::get<SourcePreferenceFailure>(unread_child_failure).kind ==
                SourcePreferenceFailureKind::UnsupportedFileType &&
            std::holds_alternative<SourcePreferenceFailure>(
                unread_base_failure) &&
            std::get<SourcePreferenceFailure>(unread_base_failure).kind ==
                SourcePreferenceFailureKind::ReadFailed &&
            preparation_stub::strict_preference_read_history() ==
                std::vector<std::string>{
                    root.package_name, root.package_base},
        "Ignore consumed or normalized an injected preference failure");

    FilteredAurUpdateObservation& drifted_child =
        *observation.aur_observation;
    AurUpdateExecutionIssue hidden_required_issue;
    hidden_required_issue.required_devel_target_blocker =
        AurUpdateRequiredDevelTargetBlocker{
            AurUpdateRequiredDevelTargetRelation::AurExactDependency,
            1,
            "required-devel-git",
            DevelRequiresCheckReason::SuffixCandidateOnly};
    drifted_child.source_build_observation
        ->affected_update_targets.front()
        .issues.push_back(std::move(hidden_required_issue));
    expect(
        drifted_child.is_blocked() && !drifted_child.is_ready() &&
            observation.is_blocked() && !observation.is_ready(),
        "Hidden required-devel observation drift remained Ready");
    expect_no_system_aur_dry_run_mutation(
        "hidden required-devel observation drift");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_auto_current_no_updates_is_not_blocked() {
    reset_stubs();
    const RootSpec root{"dry-current", "dry-current"};
    query_stub::set_foreign_inventory(
        inventory_for_roots(
            {root}, InstalledPackageReason::Explicit, "2.0-1"));
    enqueue_exact_update_query({root}, "0");

    const AppConfig config;
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    const FilteredAurUpdateObservation& child =
        require_system_aur_dry_run_child(
            observation, "system+AUR current no-updates observation");
    expect(
        observation.is_ready() && !observation.is_blocked() &&
            !observation.has_current_aur_update_intent() &&
            child.is_noop() && !child.is_ready() &&
            child.original_query_result().plan.entries.size() == 1 &&
            child.original_query_result()
                    .plan.entries.front()
                    .classification ==
                AurUpdateClassification::UpToDate,
        "Current AUR no-updates state was flattened to a blocker or update intent");
    expect(
        query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {root.package_name}} &&
            preflight_stub::resolver_call_count() == 0 &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history().empty() &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::reviewed_state_preflight_call_count() == 0 &&
            preparation_stub::database_call_count() == 0,
        "No-updates observation crossed unrelated planning/preference authority");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR current no-updates observation");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_repo_only_observes_repository_intent_only() {
    reset_stubs();
    query_stub::set_repository_configuration_failure(
        PackageMetadataFailure{
            PackageMetadataErrorCode::QueryFailed,
            "repo-only must not consume repository configuration"});
    query_stub::set_foreign_inventory_failure(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "repo-only must not consume foreign inventory"});
    preflight_stub::set_resolver_handler(
        [](const std::vector<std::string>&) -> BuildPlan {
            throw std::logic_error(
                "RepoOnly dry-run reached AUR provider authority");
        });
    preparation_stub::fail_supported_options_guard(
        "RepoOnly dry-run reached source preparation");
    preparation_stub::enqueue_source_preference_result(
        "repo-only-trap",
        SourcePreferenceFailure{
            SourcePreferenceFailureKind::ReadFailed,
            "/stub/preferences/repo-only-trap",
            std::make_error_code(std::errc::io_error),
            std::nullopt,
            "must remain unread"});

    AppConfig config;
    config.no_confirm = true;
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            SystemAurUpdateDryRunRequest{
                RepoOnlySystemUpdateRouteCandidate{
                    {"-Syu", "--config", "repo-only.conf"}, true}},
            config);
    expect(
        observation.request.mode() ==
                SystemAurUpdateDryRunMode::RepoOnly &&
            observation.request.ordered_pacman_args() ==
                std::vector<std::string>{
                    "-Syu", "--config", "repo-only.conf"} &&
            observation.request.repository_needed() &&
            observation.is_ready() && !observation.is_blocked() &&
            !observation.has_current_aur_update_intent() &&
            observation.issues.empty(),
        "RepoOnly dry-run lost exact repository-only request semantics");
    expect(
        !observation.aur_observation_basis.has_value() &&
            !observation.actual_authority_refresh.has_value() &&
            !observation.explicit_source_satisfaction.has_value() &&
            !observation.saved_source_preference_policy.has_value() &&
            !observation.repository_configuration.has_value() &&
            observation.foreign_inventory.empty() &&
            !observation.aur_observation.has_value(),
        "RepoOnly dry-run retained AUR observation authority");
    expect_no_system_aur_later_authority(
        "system+AUR RepoOnly dry-run");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR RepoOnly dry-run");

    const StrictSourcePreferenceResult unread_failure =
        read_source_preference_strict("repo-only-trap");
    expect(
        std::holds_alternative<SourcePreferenceFailure>(
            unread_failure) &&
            preparation_stub::strict_preference_read_history() ==
                std::vector<std::string>{"repo-only-trap"},
        "RepoOnly dry-run consumed the saved preference failure trap");
}

void test_system_aur_dry_run_independent_requires_check_is_attention() {
    reset_stubs();
    const RootSpec root{"dry-manual-check-git", "dry-manual-check-base"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    query_stub::enqueue_info_many_result(
        {{root.package_name,
          package_info(
              root.package_name, root.package_base, "1.0-1")}});
    query_stub::enqueue_vercmp_result("0");

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    const FilteredAurUpdateObservation& child =
        require_system_aur_dry_run_child(
            observation, "system+AUR dry-run RequiresCheck");
    expect(
        observation.is_ready() && !observation.is_blocked() &&
            !observation.has_current_aur_update_intent() &&
            observation.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::
                        SkipIndependentTarget} &&
            observation.saved_source_preference_policy ==
                std::optional<SavedSourcePreferencePolicy>{
                    SavedSourcePreferencePolicy::Ignore} &&
            observation.issues.empty() && child.is_noop() &&
            !child.is_blocked() &&
            child.has_consistent_devel_requires_check_policy_snapshot() &&
            child.execution_preflight().targets.size() == 1 &&
            child.execution_preflight().targets.front().status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            child.execution_preflight().targets.front().skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            has_preflight_issue(
                child.execution_preflight().targets.front(),
                AurUpdateExecutionReason::DevelRequiresCheck) &&
            child.execution_preflight()
                    .targets.front()
                    .issues.front()
                    .devel_requires_check_reason ==
                DevelRequiresCheckReason::SuffixCandidateOnly &&
            child.source_build_preflight().has_value() &&
            child.source_build_preflight()->is_noop(),
        "Dry-run independent RequiresCheck was blocked, flattened, or approved");
    expect(
        preflight_stub::resolver_call_count() == 0 &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history().empty() &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::reviewed_state_preflight_call_count() == 0 &&
            preparation_stub::database_call_count() == 0,
        "Dry-run independent RequiresCheck crossed later preference/preparation authority");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry-run RequiresCheck");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_ambiguous_provider_is_not_auto_selected() {
    reset_stubs();
    const RootSpec root{"dry-ambiguous-root", "dry-ambiguous-root"};
    const RootSpec requires_check{
        "dry-ambiguous-independent-git",
        "dry-ambiguous-independent-base"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root, requires_check}));
    query_stub::enqueue_info_many_result({{root.package_name,
                                           package_info(root.package_name, root.package_base)},
                                          {requires_check.package_name,
                                           package_info(
                                               requires_check.package_name,
                                               requires_check.package_base, "1.0-1")}});
    query_stub::enqueue_vercmp_result("1");
    query_stub::enqueue_vercmp_result("0");
    BuildPlan plan = root_plan({root});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        root.package_name,
        root.package_base,
        "dry-virtual-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt});
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "dry-virtual-dependency",
        {ProvidedDependency::from_repository(
             "extra", "dry-provider-a"),
         ProvidedDependency::from_aur("dry-provider-b")}});
    return_build_plan(std::move(plan), {root.package_name});

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    const FilteredAurUpdateObservation& child =
        require_system_aur_dry_run_child(
            observation, "system+AUR dry-run ambiguous provider");
    expect(
        observation.is_blocked() && !observation.is_ready() &&
            child.is_blocked() &&
            child.execution_preflight().targets.size() == 2 &&
            has_preflight_issue(
                child.execution_preflight().targets.front(),
                AurUpdateExecutionReason::AmbiguousProvider) &&
            child.execution_preflight().targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            child.execution_preflight().targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck,
        "Dry-run ambiguous provider was downgraded by independent RequiresCheck attention");
    expect(
        preflight_stub::resolver_selection_callback_presence() ==
                std::vector<bool>{true} &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history().empty() &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::reviewed_state_preflight_call_count() == 0 &&
            preparation_stub::database_call_count() == 0,
        "Dry-run --noconfirm selected an ambiguous provider or crossed preference IO");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry-run ambiguous provider");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_fatal_query_failure_is_typed_and_blocks() {
    reset_stubs();
    const RootSpec root{"dry-query-failed", "dry-query-failed"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    query_stub::enqueue_info_many_result(
        {{root.package_name, package_info(root.package_name)}});
    query_stub::enqueue_vercmp_failure(
        "fixture fatal dry-run version comparison failure");

    const AppConfig config;
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    expect(
        observation.is_blocked() && !observation.is_ready() &&
            observation.issues.size() == 1 &&
            observation.issues.front().kind ==
                SystemAurUpdateDryRunIssueKind::AurQueryFailure &&
            observation.issues.front().diagnostic ==
                "fixture fatal dry-run version comparison failure" &&
            !observation.aur_observation.has_value(),
        "Dry-run fatal query failure was erased or failed open");
    expect(
        preflight_stub::resolver_call_count() == 0 &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history().empty() &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::reviewed_state_preflight_call_count() == 0 &&
            preparation_stub::database_call_count() == 0,
        "Dry-run query failure reached provider/preference/preparation authority");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry-run query failure");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_identity_mismatch_blocks_before_mutation() {
    reset_stubs();
    const RootSpec root{"dry-identity-root", "dry-identity-root"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    enqueue_exact_update_query({root});
    BuildPlan plan = root_plan({root});
    plan.root_targets.front().requested_name = "dry-wrong-root";
    plan.package_targets.front().roots.front() =
        plan.root_targets.front();
    return_build_plan(std::move(plan), {root.package_name});

    const AppConfig config;
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    const FilteredAurUpdateObservation& child =
        require_system_aur_dry_run_child(
            observation, "system+AUR dry-run identity mismatch");
    expect(
        observation.is_blocked() && child.is_blocked() &&
            has_operation_issue(
                child.operation_issues(),
                FilteredAurUpdateOperationIssueKind::
                    BuildPlanRootIndexMissing) &&
            has_operation_issue(
                child.operation_issues(),
                FilteredAurUpdateOperationIssueKind::
                    PreflightInvocationIdentityMismatch) &&
            child.execution_preflight().targets.size() == 1 &&
            child.execution_preflight().targets.front().status ==
                AurUpdateExecutionTargetStatus::Incomplete,
        "Dry-run exact identity mismatch was not retained as a blocker");
    expect(
        preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::supported_options_guard_history().empty() &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::reviewed_state_preflight_call_count() == 0 &&
            preparation_stub::database_call_count() == 0,
        "Dry-run identity mismatch crossed preference/preparation authority");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry-run identity mismatch");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_preparation_blocker_stops_before_mutation() {
    reset_stubs();
    const RootSpec root{
        "dry-preparation-blocked", "dry-preparation-blocked"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    enqueue_exact_update_query({root});
    return_build_plan(root_plan({root}), {root.package_name});
    preparation_stub::fail_supported_options_guard(
        "fixture dry-run preparation failure");

    const AppConfig config;
    SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    const FilteredAurUpdateObservation& child =
        require_system_aur_dry_run_child(
            observation, "system+AUR dry-run preparation blocker");
    const AurUpdateSourceBuildObservation& source_observation =
        child.source_build_preflight().value();
    expect(
        observation.is_blocked() && child.is_blocked() &&
            source_observation.is_blocked() &&
            !source_observation.production_preflight.has_value() &&
            !source_observation.issues.empty() &&
            source_observation.issues.front().reason ==
                AurUpdatePreparationReason::
                    GenericPreparationInconsistent,
        "Dry-run preparation failure was flattened or failed open");
    expect(
        preparation_stub::supported_options_guard_history() ==
                std::vector<bool>{false} &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::pkgdest_guard_history().empty() &&
            preparation_stub::reviewed_state_preflight_call_count() == 0 &&
            preparation_stub::database_call_count() == 0,
        "Dry-run preparation blocker crossed saved preference or DB authority");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry-run preparation blocker");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_dry_run_world_is_not_reused_by_actual_world() {
    reset_stubs();
    const RootSpec dry_root{"dry-world-a-child", "dry-world-a-base"};
    ProvidedDependency dry_provider =
        ProvidedDependency::from_repository(
            "world-a-repository", "dry-provider-a",
            "dry-virtual", "dry-virtual=1", "1.0-1");
    dry_provider.package_base = "dry-provider-a-base";
    query_stub::set_repository_configuration(
        PacmanRepositoryConfiguration{
            PacmanDatabasePaths{"/world-a/root", "/world-a/database"},
            {"world-a-repository"}});
    query_stub::set_foreign_inventory(
        inventory_for_roots(
            {dry_root}, InstalledPackageReason::Explicit, "1.0-a"));
    enqueue_exact_update_query({dry_root});
    return_build_plan(
        root_plan_with_selected_repository_provider(
            dry_root, "dry-virtual", dry_provider),
        {dry_root.package_name});

    AppConfig dry_config;
    dry_config.editor = "world-a-editor";
    dry_config.provider_selection =
        make_provider_selection_session(dry_config.no_confirm);
    const std::shared_ptr<ProviderSelectionSession> dry_session =
        dry_config.provider_selection;
    SystemAurUpdateDryRunObservation dry_observation =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), dry_config);
    const FilteredAurUpdateObservation& dry_child =
        require_system_aur_dry_run_child(
            dry_observation, "system+AUR dry world A");
    expect(
        dry_observation.is_ready() && dry_child.is_ready() &&
            dry_observation.foreign_inventory.size() == 1 &&
            dry_observation.foreign_inventory.front().name ==
                dry_root.package_name &&
            dry_child.source_build_preflight()
                ->production_preflight.has_value() &&
            dry_child.source_build_preflight()
                    ->production_preflight
                    ->selected_repository_providers ==
                std::vector<ProvidedDependency>{dry_provider} &&
            dry_child.source_build_preflight()
                    ->production_preflight->work_items.front()
                    .request.checkout_name == dry_root.package_base,
        "Dry world A did not retain its own current-state observation");
    expect(
        query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {dry_root.package_name}} &&
            preflight_stub::resolver_calls() ==
                std::vector<std::vector<std::string>>{
                    {dry_root.package_name}},
        "Dry world A did not perform one exact current-state observation");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry world A");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();

    const RootSpec actual_root{
        "actual-world-b-child", "actual-world-b-base"};
    ProvidedDependency actual_provider =
        ProvidedDependency::from_repository(
            "world-b-repository", "actual-provider-b",
            "actual-virtual", "actual-virtual=1", "1.0-1");
    actual_provider.package_base = "actual-provider-b-base";
    AppConfig actual_config;
    actual_config.no_confirm = true;
    actual_config.editor = "world-b-editor";
    actual_config.provider_selection =
        make_provider_selection_session(actual_config.no_confirm);
    const std::shared_ptr<ProviderSelectionSession> actual_session =
        actual_config.provider_selection;
    expect(
        dry_session && actual_session &&
            dry_session.get() != actual_session.get(),
        "Dry and actual invocations unexpectedly share a provider session");

    repository_stub::set_after_success(
        [actual_root, actual_provider, actual_config] {
            query_stub::set_repository_configuration(
                PacmanRepositoryConfiguration{
                    PacmanDatabasePaths{
                        "/world-b/root", "/world-b/database"},
                    {"world-b-repository"}});
            query_stub::set_foreign_inventory(
                inventory_for_roots(
                    {actual_root},
                    InstalledPackageReason::Dependency, "1.0-b"));
            enqueue_exact_update_query({actual_root});
            return_build_plan(
                root_plan_with_selected_repository_provider(
                    actual_root, "actual-virtual",
                    actual_provider),
                {actual_root.package_name});
            preparation_stub::set_database_paths(
                PacmanDatabasePaths{
                    "/actual/root", "/actual/database"});
            execution_stub::enqueue_success(
                expected_system_aur_execution(
                    0, actual_root.package_name,
                    actual_root.package_base,
                    InstalledPackageReason::Dependency,
                    actual_config,
                    PacmanDatabasePaths{
                        "/actual/root", "/actual/database"}),
                actual_root.package_base,
                selected_system_aur_child(
                    actual_root.package_name,
                    InstalledPackageReason::Dependency));
        });

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture(
            {"-Syu", "--needed"});
    expect(
        repository_stub::ordered_argument_calls().empty() &&
            query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {dry_root.package_name}} &&
            preflight_stub::resolver_calls() ==
                std::vector<std::vector<std::string>>{
                    {dry_root.package_name}} &&
            preparation_stub::database_call_count() == 1 &&
            execution_stub::call_history().empty() &&
            execution_stub::event_history().empty() &&
            execution_stub::invocation_event_history().empty(),
        "Actual outer preparation reused or reopened dry-run authority before repository success");
    SystemAurUpdateOperationResult actual_result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), actual_config);
    const FilteredAurUpdateExecutionResult& actual_child =
        require_system_aur_child(
            actual_result, "system+AUR fresh actual world B");

    expect(
        dry_observation.foreign_inventory.front().name ==
                dry_root.package_name &&
            dry_child.original_query_result()
                    .plan.entries.front()
                    .installed_name == dry_root.package_name &&
            actual_result.foreign_inventory.inventory.size() == 1 &&
            actual_result.foreign_inventory.inventory.front().name ==
                actual_root.package_name &&
            actual_result.foreign_inventory.inventory.front().version ==
                "1.0-b" &&
            actual_result.foreign_inventory.inventory.front().reason ==
                InstalledPackageReason::Dependency &&
            actual_child.query_result.plan.entries.front().installed_name ==
                actual_root.package_name &&
            actual_child.query_result.plan.entries.front().installed_version ==
                "1.0-b" &&
            actual_child.execution.has_value() &&
            actual_child.execution
                    ->selected_repository_provider_transaction
                    .selected_providers ==
                std::vector<ProvidedDependency>{actual_provider} &&
            actual_child.execution
                    ->selected_repository_provider_transaction
                    .selected_providers !=
                std::vector<ProvidedDependency>{dry_provider},
        "Actual invocation reused dry-run inventory or AUR query evidence");
    expect(
        query_stub::repository_configuration_calls() == 2 &&
            query_stub::inventory_calls() == 2 &&
            query_stub::inventory_configuration_history().size() == 2 &&
            query_stub::inventory_configuration_history()
                    .at(0)
                    .repository_names ==
                std::vector<std::string>{"world-a-repository"} &&
            query_stub::inventory_configuration_history()
                    .at(1)
                    .repository_names ==
                std::vector<std::string>{"world-b-repository"} &&
            query_stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {dry_root.package_name},
                    {actual_root.package_name}} &&
            preflight_stub::resolver_calls() ==
                std::vector<std::vector<std::string>>{
                    {dry_root.package_name},
                    {actual_root.package_name}},
        "Actual invocation did not freshly query world B after repository success");
    expect(
        repository_stub::ordered_argument_calls() ==
                std::vector<std::vector<std::string>>{
                    {"-Syu", "--needed"}} &&
            execution_stub::call_history().size() == 1 &&
            execution_stub::call_history().front().package_name ==
                actual_root.package_name &&
            execution_stub::call_history().front().package_base ==
                actual_root.package_base &&
            execution_stub::call_history()
                    .front()
                    .database_paths.root_dir == "/actual/root" &&
            execution_stub::call_history()
                    .front()
                    .database_paths.db_path == "/actual/database" &&
            execution_stub::call_history().front().config.editor ==
                actual_config.editor &&
            execution_stub::call_history()
                    .front()
                    .config.provider_selection.get() ==
                actual_session.get() &&
            execution_stub::call_history()
                    .front()
                    .config.provider_selection.get() !=
                dry_session.get() &&
            std::find(
                execution_stub::invocation_event_history().begin(),
                execution_stub::invocation_event_history().end(),
                execution_stub::InvocationEventKind::
                    RepositoryProviderTransaction) !=
                execution_stub::invocation_event_history().end(),
        "Actual invocation reused dry-run source/session preparation");
    expect(
        actual_result.is_success() &&
            actual_result.package_state_change() ==
                PackageStateChange::Changed &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::database_call_count() == 2,
        "Fresh actual world B did not complete with Ignore semantics");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_repository_failure_stops_all_later_authority() {
    reset_stubs();
    const AppConfig config;

    AutoSystemUpdateRouteCandidate incompatible_candidate{
        IncompatibleAutoSystemUpdatePacmanArguments{
            AutoSystemUpdatePacmanIncompatibilityKind::UnsupportedOption,
            "--config"},
        {"-Syu", "--config", "fixture.conf"}};
    expect(
        !make_compatible_system_aur_update_request(
             std::move(incompatible_candidate))
             .has_value(),
        "Incompatible Auto request entered the coordinator capability");

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture(
            {"-Syu", "--needed"});
    expect(
        repository_stub::ordered_argument_calls().empty(),
        "System+AUR preparation executed the repository request");
    expect_no_system_aur_later_authority(
        "system+AUR outer preparation");

    repository_stub::set_command_exit_status(37);
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);

    expect(
        repository_stub::ordered_argument_calls() ==
                std::vector<std::vector<std::string>>{
                    {"-Syu", "--needed"}} &&
            result.repository.ordered_pacman_args ==
                std::vector<std::string>{"-Syu", "--needed"} &&
            result.repository.command_exit_status ==
                std::optional<int>{37},
        "Repository failure lost exact ordered request/exit evidence");
    expect(
        result.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::Failed &&
            result.foreign_inventory.status ==
                SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted &&
            result.foreign_inventory.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::RepositoryFailure} &&
            result.query.status ==
                SystemAurUpdateQueryPhaseStatus::NotAttempted &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::NotAttempted &&
            result.aur.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::RepositoryFailure},
        "Repository failure did not retain typed later NotAttempted phases");
    expect(
        result.status ==
                SystemAurUpdateOperationStatus::StoppedOnRepositoryFailure &&
            !result.is_success() &&
            !result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            !result.has_inconsistency(),
        "Repository failure aggregate semantics differ");
    expect_no_system_aur_later_authority(
        "system+AUR repository failure");

    result.foreign_inventory.inventory.push_back(
        InstalledPackageMetadata{
            "impossible-post-failure-inventory",
            "1.0-1",
            InstalledPackageReason::Explicit});
    SystemAurUpdateOperationResult malformed =
        reduce_system_aur_update_result(std::move(result));
    expect(
        malformed.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            malformed.has_inconsistency() &&
            malformed.foreign_inventory.inventory.size() == 1,
        "Repository failure tail accepted or erased later inventory payload");
}

void test_system_aur_success_without_aur_updates_is_not_false_noop() {
    reset_stubs();
    query_stub::set_foreign_inventory(
        ForeignPackageInventory{{"pre-repository-foreign",
                                 "1.0-1",
                                 InstalledPackageReason::Explicit}});
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    expect_no_system_aur_later_authority(
        "system+AUR no-update preparation");

    const AppConfig config;
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR no updates");

    expect(
        query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::info_many_call_history().empty(),
        "No-update path did not obtain exactly one fresh empty inventory");
    expect(
        result.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::Completed &&
            result.foreign_inventory.status ==
                SystemAurUpdateForeignInventoryPhaseStatus::Completed &&
            result.foreign_inventory.inventory.empty() &&
            result.query.status ==
                SystemAurUpdateQueryPhaseStatus::Completed &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::NoUpdates &&
            child.reduced_operation_result.status ==
                AurUpdateOperationStatus::NoUpdates,
        "No-update path lost completed phase evidence");
    expect(
        result.is_success() &&
            result.status == SystemAurUpdateOperationStatus::Completed &&
            result.package_state_change() == PackageStateChange::Unknown &&
            !result.has_partial_completion() &&
            !result.has_not_attempted_phase(),
        "Repository success + AUR NoUpdates was flattened to a false no-op or failure");
    expect_no_mutation("system+AUR no updates");
}

void test_system_aur_inventory_failure_is_partial() {
    reset_stubs();
    query_stub::set_foreign_inventory_failure(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture foreign inventory failure"});
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);

    expect(
        result.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::Completed &&
            result.foreign_inventory.status ==
                SystemAurUpdateForeignInventoryPhaseStatus::Failed &&
            result.foreign_inventory.failure.has_value() &&
            result.query.status ==
                SystemAurUpdateQueryPhaseStatus::NotAttempted &&
            result.query.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::
                        ForeignInventoryFailure} &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::NotAttempted &&
            result.aur.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::
                        ForeignInventoryFailure},
        "Inventory failure did not retain typed failure/NotAttempted evidence");
    expect(
        !result.is_success() && result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            result.has_query_failure() &&
            result.status ==
                SystemAurUpdateOperationStatus::StoppedBeforeAurExecution,
        "Inventory failure aggregate semantics differ");
    expect(
        query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::info_many_call_history().empty() &&
            preflight_stub::resolver_call_count() == 0 &&
            execution_stub::call_history().empty(),
        "Inventory failure reached AUR query/preflight/execution");

    result.query.query_result = AurUpdateQueryResult{};
    SystemAurUpdateOperationResult malformed =
        reduce_system_aur_update_result(std::move(result));
    expect(
        malformed.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            malformed.has_inconsistency() &&
            malformed.query.query_result.has_value(),
        "Inventory failure tail accepted or erased later query payload");
}

void test_system_aur_query_failure_is_retained() {
    reset_stubs();
    query_stub::set_foreign_inventory({{"query-failed",
                                        "1.0-1",
                                        InstalledPackageReason::Explicit}});
    query_stub::enqueue_info_many_failure(
        "fixture coordinator AUR transport failure");
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR query failure");

    expect(
        result.query.status ==
                SystemAurUpdateQueryPhaseStatus::Failed &&
            result.query.query_result.has_value() &&
            result.query.query_result->recoverable_failures.size() == 1 &&
            child.has_query_failure() &&
            child.query_result.recoverable_failures.front().diagnostic ==
                "fixture coordinator AUR transport failure" &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution,
        "Recoverable AUR query failure was flattened or lost");
    expect(
        !result.is_success() && result.has_partial_completion() &&
            result.has_query_failure() &&
            result.status ==
                SystemAurUpdateOperationStatus::StoppedBeforeAurExecution &&
            result.stopped_phase ==
                SystemAurUpdateOperationPhase::AurQuery,
        "AUR query failure aggregate semantics differ");
    expect_no_mutation("system+AUR query failure");
    query_stub::require_script_consumed();
}

void test_system_aur_fatal_query_failure_stops_before_preflight() {
    reset_stubs();
    query_stub::set_foreign_inventory({{"fatal-query",
                                        "1.0-1",
                                        InstalledPackageReason::Explicit}});
    query_stub::enqueue_info_many_result({{"fatal-query",
                                           package_info("fatal-query", "fatal-query-base")}});
    query_stub::enqueue_vercmp_failure(
        "fixture fatal version comparison failure");

    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(
        result.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::Completed &&
            result.foreign_inventory.status ==
                SystemAurUpdateForeignInventoryPhaseStatus::Completed &&
            result.query.status ==
                SystemAurUpdateQueryPhaseStatus::Failed &&
            !result.query.query_result.has_value() &&
            result.query.diagnostic ==
                std::optional<std::string>{
                    "fixture fatal version comparison failure"} &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::NotAttempted &&
            result.aur.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::AurQueryFailure},
        "Fatal AUR query failure lost typed phase/NotAttempted evidence");
    expect(
        result.status ==
                SystemAurUpdateOperationStatus::StoppedBeforeAurExecution &&
            result.stopped_phase ==
                SystemAurUpdateOperationPhase::AurQuery &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            result.has_query_failure() &&
            !result.is_success() &&
            preflight_stub::resolver_call_count() == 0 &&
            preparation_stub::database_call_count() == 0 &&
            execution_stub::call_history().empty(),
        "Fatal AUR query failure reached later authority or failed open");
    query_stub::require_script_consumed();
}

void test_system_aur_independent_requires_check_completes_with_attention() {
    reset_stubs();
    query_stub::set_foreign_inventory({{"manual-check-git",
                                        "1.0-1",
                                        InstalledPackageReason::Explicit}});
    query_stub::enqueue_info_many_result({{"manual-check-git",
                                           package_info(
                                               "manual-check-git", "manual-check-base", "1.0-1")}});
    query_stub::enqueue_vercmp_result("0");

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR RequiresCheck");

    expect(
        result.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::Completed &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::NoUpdates &&
            child.has_consistent_devel_requires_check_policy_snapshot() &&
            child.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::
                        SkipIndependentTarget} &&
            child.reduced_operation_result.status ==
                AurUpdateOperationStatus::NoUpdates &&
            child.preflight.targets.size() == 1 &&
            child.preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            child.preflight.targets.front().skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            has_preflight_issue(
                child.preflight.targets.front(),
                AurUpdateExecutionReason::DevelRequiresCheck),
        "Independent RequiresCheck was blocked, flattened, or approved after repository success");
    expect(
        result.is_success() && !result.has_partial_completion() &&
            result.status == SystemAurUpdateOperationStatus::Completed &&
            result.package_state_change() ==
                PackageStateChange::Unknown &&
            child.package_state_change() == PackageStateChange::Unknown &&
            !child.changed_package_state() &&
            execution_stub::call_history().empty() &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0,
        "Independent RequiresCheck became partial failure, mutation, or saved preference IO");
    query_stub::require_script_consumed();
}

void test_system_aur_update_and_independent_requires_check_succeed() {
    reset_stubs();
    const RootSpec update_root{
        "ordinary-update", "ordinary-update-base"};
    const RootSpec requires_check{
        "ordinary-independent-git", "ordinary-independent-base"};
    const RootSpec current{"ordinary-current", "ordinary-current"};
    query_stub::set_foreign_inventory(ForeignPackageInventory{
        {update_root.package_name, "1.0-1",
         InstalledPackageReason::Explicit},
        {requires_check.package_name, "1.0-1",
         InstalledPackageReason::Explicit},
        {current.package_name, "1.0-1",
         InstalledPackageReason::Explicit}});
    query_stub::enqueue_info_many_result({{update_root.package_name,
                                           package_info(
                                               update_root.package_name, update_root.package_base,
                                               "2.0-1")},
                                          {requires_check.package_name,
                                           package_info(
                                               requires_check.package_name,
                                               requires_check.package_base, "1.0-1")},
                                          {current.package_name,
                                           package_info(
                                               current.package_name, current.package_base,
                                               "1.0-1")}});
    query_stub::enqueue_vercmp_result("1");
    query_stub::enqueue_vercmp_result("0");
    query_stub::enqueue_vercmp_result("0");
    return_build_plan(
        root_plan({update_root}), {update_root.package_name});

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    execution_stub::enqueue_success(
        expected_system_aur_execution(
            0, update_root.package_name, update_root.package_base,
            InstalledPackageReason::Explicit, config),
        update_root.package_base,
        selected_system_aur_child(
            update_root.package_name,
            InstalledPackageReason::Explicit));

    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            prepare_system_aur_update_fixture(), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(
            result,
            "system+AUR update plus independent RequiresCheck");

    expect(
        result.is_success() &&
            result.status == SystemAurUpdateOperationStatus::Completed &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::Completed &&
            result.package_state_change() == PackageStateChange::Changed &&
            child.reduced_operation_result.status ==
                AurUpdateOperationStatus::Completed &&
            child.reduced_operation_result.targets.size() == 3 &&
            child.reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::Updated &&
            child.reduced_operation_result.targets[1].status ==
                AurUpdateOperationTargetStatus::Skipped &&
            child.reduced_operation_result.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            child.reduced_operation_result.targets[2].status ==
                AurUpdateOperationTargetStatus::Skipped &&
            child.reduced_operation_result.targets[2].skip_kind ==
                AurUpdateExecutionSkipKind::UpToDate,
        "Normal update and independent RequiresCheck did not complete with Changed authority");
    expect(
        preflight_stub::resolver_calls() ==
                std::vector<std::vector<std::string>>{
                    {update_root.package_name}} &&
            execution_stub::call_history().size() == 1 &&
            execution_stub::call_history().front().package_name ==
                update_root.package_name &&
            preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0,
        "Independent RequiresCheck entered mutation or saved preference authority");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_required_requires_check_blocks_actual_and_dry_run() {
    const RootSpec update_root{
        "required-owner", "required-owner-base"};
    const RootSpec requires_check{
        "required-child-git", "required-child-base"};
    const auto arrange = [&] {
        query_stub::set_foreign_inventory(ForeignPackageInventory{
            {update_root.package_name, "1.0-1",
             InstalledPackageReason::Explicit},
            {requires_check.package_name, "1.0-1",
             InstalledPackageReason::Dependency}});
        query_stub::enqueue_info_many_result({{update_root.package_name,
                                               package_info(
                                                   update_root.package_name, update_root.package_base,
                                                   "2.0-1")},
                                              {requires_check.package_name,
                                               package_info(
                                                   requires_check.package_name,
                                                   requires_check.package_base, "1.0-1")}});
        query_stub::enqueue_vercmp_result("1");
        query_stub::enqueue_vercmp_result("0");
        BuildPlan plan = root_plan({update_root});
        add_aur_dependency(
            plan, update_root.package_name,
            requires_check.package_name,
            requires_check.package_base);
        return_build_plan(
            std::move(plan), {update_root.package_name});
    };

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);

    reset_stubs();
    arrange();
    SystemAurUpdateDryRunObservation dry =
        observe_system_aur_update_dry_run(
            make_auto_system_aur_dry_run_request(), config);
    const FilteredAurUpdateObservation& dry_child =
        require_system_aur_dry_run_child(
            dry, "system+AUR dry required RequiresCheck");
    expect(
        dry.is_blocked() && !dry.is_ready() &&
            dry.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::
                        SkipIndependentTarget} &&
            dry_child.execution_preflight().targets.size() == 2 &&
            dry_child.execution_preflight().targets[0].status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            has_preflight_issue(
                dry_child.execution_preflight().targets[0],
                AurUpdateExecutionReason::
                    RequiredDevelTargetRequiresCheck) &&
            dry_child.execution_preflight().targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            dry_child.execution_preflight().targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck,
        "Dry-run required RequiresCheck was downgraded to attention or Ready");
    expect_no_system_aur_dry_run_mutation(
        "system+AUR dry required RequiresCheck");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();

    reset_stubs();
    arrange();
    SystemAurUpdateOperationResult actual =
        execute_prepared_system_aur_update_operation(
            prepare_system_aur_update_fixture(), config);
    const FilteredAurUpdateExecutionResult& actual_child =
        require_system_aur_child(
            actual, "system+AUR actual required RequiresCheck");
    expect(
        !actual.is_success() && actual.has_partial_completion() &&
            actual.status ==
                SystemAurUpdateOperationStatus::
                    StoppedBeforeAurExecution &&
            actual.aur.status ==
                SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution &&
            actual_child.reduced_operation_result.status ==
                AurUpdateOperationStatus::BlockedBeforeExecution &&
            actual_child.reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::Incomplete &&
            actual_child.reduced_operation_result.targets[1].status ==
                AurUpdateOperationTargetStatus::Skipped &&
            actual_child.reduced_operation_result.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck &&
            execution_stub::call_history().empty(),
        "Actual required RequiresCheck was downgraded or reached mutation");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_ambiguous_provider_is_not_auto_selected() {
    reset_stubs();
    const RootSpec root{"ambiguous-root", "ambiguous-root"};
    const RootSpec requires_check{
        "ambiguous-independent-git",
        "ambiguous-independent-base"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root, requires_check}));
    query_stub::enqueue_info_many_result({{root.package_name,
                                           package_info(root.package_name, root.package_base)},
                                          {requires_check.package_name,
                                           package_info(
                                               requires_check.package_name,
                                               requires_check.package_base, "1.0-1")}});
    query_stub::enqueue_vercmp_result("1");
    query_stub::enqueue_vercmp_result("0");
    BuildPlan plan = root_plan({root});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        root.package_name,
        root.package_base,
        "virtual-dependency",
        PackageRole::RuntimeDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt});
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "virtual-dependency",
        {ProvidedDependency::from_repository(
             "extra", "provider-a"),
         ProvidedDependency::from_aur("provider-b")}});
    return_build_plan(std::move(plan), {root.package_name});

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR ambiguous provider");

    expect(
        result.aur.status ==
                SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution &&
            child.preflight.targets.size() == 2 &&
            has_preflight_issue(
                child.preflight.targets.front(),
                AurUpdateExecutionReason::AmbiguousProvider) &&
            child.preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            child.preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck,
        "Ambiguous provider was downgraded by independent RequiresCheck attention");
    expect(
        preflight_stub::resolver_selection_callback_presence() ==
                std::vector<bool>{true} &&
            execution_stub::call_history().empty() &&
            result.has_partial_completion(),
        "--noconfirm bypassed ambiguity or lost repository partial completion");
    query_stub::require_script_consumed();
}

void test_system_aur_preparation_failure_is_partial() {
    reset_stubs();
    const RootSpec root{"preparation-failed", "preparation-failed"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    enqueue_exact_update_query({root});
    return_build_plan(root_plan({root}), {root.package_name});
    preparation_stub::fail_supported_options_guard(
        "fixture coordinator preparation failure");

    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR preparation failure");

    expect(
        result.aur.status ==
                SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution &&
            child.reduced_operation_result.status ==
                AurUpdateOperationStatus::BlockedBeforeExecution &&
            !child.reduced_operation_result.preparation_issues.empty() &&
            !result.is_success() &&
            result.has_partial_completion(),
        "AUR preparation failure was flattened after repository success");
    expect(
        preparation_stub::supported_options_guard_history() ==
                std::vector<bool>{false} &&
            preparation_stub::strict_preference_read_history().empty() &&
            execution_stub::call_history().empty(),
        "Preparation failure crossed Ignore or mutation boundaries");
    query_stub::require_script_consumed();
}

void test_system_aur_unexpected_preparation_exception_keeps_phase() {
    reset_stubs();
    const RootSpec root{"preparation-exception", "preparation-exception"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    enqueue_exact_update_query({root});
    preflight_stub::set_resolver_handler(
        [](const std::vector<std::string>&) -> BuildPlan {
            throw std::logic_error(
                "fixture unexpected filtered preparation exception");
        });

    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            result.stopped_phase ==
                SystemAurUpdateOperationPhase::AurPreparation &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            result.aur.diagnostic ==
                std::optional<std::string>{
                    "fixture unexpected filtered preparation exception"} &&
            !result.aur.operation_result.has_value() &&
            result.has_inconsistency() &&
            result.has_partial_completion() &&
            !result.is_success() &&
            execution_stub::call_history().empty(),
        "Unexpected filtered preparation exception lost its typed phase or failed open");
    query_stub::require_script_consumed();
}

void test_system_aur_unexpected_execution_exception_keeps_phase() {
    reset_stubs();
    const RootSpec root{"execution-exception", "execution-exception"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    enqueue_exact_update_query({root});
    return_build_plan(root_plan({root}), {root.package_name});
    execution_stub::fail_cache_activation(TrustedCacheFailure{
        TrustedCacheStage::RootRevalidation,
        TrustedCacheErrorCode::ConcurrentReplacement,
        std::nullopt});

    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            result.stopped_phase ==
                SystemAurUpdateOperationPhase::AurExecution &&
            result.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            result.aur.diagnostic.has_value() &&
            !result.aur.diagnostic->empty() &&
            !result.aur.operation_result.has_value() &&
            result.has_inconsistency() &&
            result.has_partial_completion() &&
            !result.is_success() &&
            execution_stub::call_history().empty() &&
            execution_stub::invocation_event_history() ==
                std::vector<execution_stub::InvocationEventKind>{
                    execution_stub::InvocationEventKind::CacheActivation},
        "Unexpected filtered execution exception lost its typed phase or failed open");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_success_uses_only_post_repository_inventory_and_ignore() {
    reset_stubs();
    const RootSpec post_repository_root{
        "foreign-b-child", "foreign-b-base"};
    query_stub::set_repository_configuration(
        PacmanRepositoryConfiguration{
            PacmanDatabasePaths{"/before/root", "/before/database"},
            {"before-repository"}});
    query_stub::set_foreign_inventory({{"foreign-a",
                                        "9.0-1",
                                        InstalledPackageReason::Explicit}});

    AppConfig config;
    config.no_confirm = true;
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);
    repository_stub::set_after_success([post_repository_root, config] {
        require_no_pre_repository_authority();
        query_stub::set_repository_configuration(
            PacmanRepositoryConfiguration{
                PacmanDatabasePaths{
                    "/after/root", "/after/database"},
                {"after-repository"}});
        query_stub::set_foreign_inventory({{post_repository_root.package_name,
                                            "1.0-7",
                                            InstalledPackageReason::Dependency}});
        preparation_stub::set_database_paths(
            PacmanDatabasePaths{"/stub/root", "/stub/database"});
        enqueue_exact_update_query({post_repository_root});
        return_build_plan(
            root_plan({post_repository_root}),
            {post_repository_root.package_name});
        preparation_stub::enqueue_source_preference_result(
            post_repository_root.package_name,
            SourcePreferenceLoaded{
                .entry_path = "/stub/preferences/foreign-b-child",
                .environment = SourceBuildEnvironment{
                    std::vector<SourceEnvironmentAssignment>{
                        {"CFLAGS", "-O3"}}},
                .warnings = {"must remain unread"},
                .raw_contents = {},
                .identity = std::nullopt,
            });
        preparation_stub::enqueue_source_preference_result(
            post_repository_root.package_base,
            SourcePreferenceFailure{
                SourcePreferenceFailureKind::ReadFailed,
                "/stub/preferences/foreign-b-base",
                std::make_error_code(std::errc::io_error),
                std::nullopt,
                "must remain unread"});
        execution_stub::enqueue_success(
            expected_system_aur_execution(
                0,
                post_repository_root.package_name,
                post_repository_root.package_base,
                InstalledPackageReason::Dependency,
                config),
            post_repository_root.package_base,
            selected_system_aur_child(
                post_repository_root.package_name,
                InstalledPackageReason::Dependency));
    });

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture(
            {"-Syu", "--needed"});
    expect_no_system_aur_later_authority(
        "system+AUR fresh inventory preparation");
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR fresh inventory");

    expect(
        repository_stub::ordered_argument_calls() ==
                std::vector<std::vector<std::string>>{
                    {"-Syu", "--needed"}} &&
            repository_stub::no_confirm_calls() ==
                std::vector<bool>{true},
        "Repository phase lost exact ordered args or noconfirm policy");
    expect(
        query_stub::repository_configuration_calls() == 1 &&
            query_stub::inventory_calls() == 1 &&
            query_stub::inventory_configuration_history().size() == 1 &&
            query_stub::inventory_configuration_history()
                    .front()
                    .repository_names ==
                std::vector<std::string>{"after-repository"} &&
            query_stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {post_repository_root.package_name}},
        "Fresh inventory/query did not use post-repository configuration and identity");
    expect(
        result.foreign_inventory.inventory.size() == 1 &&
            result.foreign_inventory.inventory.front().name ==
                post_repository_root.package_name &&
            result.foreign_inventory.inventory.front().version ==
                "1.0-7" &&
            result.foreign_inventory.inventory.front().reason ==
                InstalledPackageReason::Dependency &&
            child.query_result.plan.entries.size() == 1 &&
            child.query_result.plan.entries.front().installed_name ==
                post_repository_root.package_name &&
            child.query_result.plan.entries.front().installed_version ==
                "1.0-7" &&
            child.query_result.plan.entries.front().install_reason ==
                InstalledPackageReason::Dependency &&
            child.query_result.plan.entries.front().aur_package.has_value() &&
            child.query_result.plan.entries.front()
                    .aur_package->package_base ==
                post_repository_root.package_base,
        "Fresh inventory lost installed version/reason or exact AUR PackageBase identity");
    expect(
        preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::database_call_count() == 1 &&
            preparation_stub::reviewed_state_preflight_call_count() == 1 &&
            preparation_stub::pkgdest_guard_history().size() == 2 &&
            std::all_of(
                preparation_stub::pkgdest_guard_history().begin(),
                preparation_stub::pkgdest_guard_history().end(),
                [](const SourceBuildEnvironment& environment) {
                    return environment.ordered_assignments.empty();
                }),
        "Coordinator did not preserve Ignore zero-I/O and downstream safety");
    expect(
        child.upgrade_all_plan.explicit_sources.empty() &&
            child.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::
                        SkipIndependentTarget} &&
            execution_stub::call_history().size() == 1 &&
            !execution_stub::call_history().front().needed &&
            execution_stub::call_history()
                    .front()
                    .ordered_required_targets.front()
                    .desired_reason ==
                DesiredInstallReason::Dependency,
        "NoExplicit/--needed/install-reason policy changed in AUR runner");
    expect(
        result.is_success() &&
            result.package_state_change() == PackageStateChange::Changed &&
            child.reduced_operation_result.targets.front().status ==
                AurUpdateOperationTargetStatus::Updated,
        "Successful system+AUR update did not preserve child success");

    const StrictSourcePreferenceResult unread_failure =
        read_source_preference_strict(
            post_repository_root.package_base);
    expect(
        std::holds_alternative<SourcePreferenceFailure>(
            unread_failure) &&
            preparation_stub::strict_preference_read_history() ==
                std::vector<std::string>{
                    post_repository_root.package_base},
        "Ignore consumed the injected PackageBase preference failure");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_work_item_failure_preserves_inner_partial() {
    reset_stubs();
    const std::vector<RootSpec> roots{
        {"first-updated", "first-updated"},
        {"middle-failed", "middle-failed"},
        {"last-not-attempted", "last-not-attempted"}};
    query_stub::set_foreign_inventory(
        inventory_for_roots(roots));
    enqueue_exact_update_query(roots);
    return_build_plan(root_plan(roots),
                      {roots[0].package_name,
                       roots[1].package_name,
                       roots[2].package_name});
    const AppConfig config;
    execution_stub::enqueue_success(
        expected_system_aur_execution(
            0, roots[0].package_name, roots[0].package_base,
            InstalledPackageReason::Explicit, config),
        roots[0].package_base,
        selected_system_aur_child(
            roots[0].package_name,
            InstalledPackageReason::Explicit));
    execution_stub::enqueue_phase_failure(
        expected_system_aur_execution(
            1, roots[1].package_name, roots[1].package_base,
            InstalledPackageReason::Explicit, config),
        SeparatedPackageBaseSourceBuildFailurePhase::Build,
        "fixture coordinator work-item failure");

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR work-item failure");

    expect(
        result.aur.status ==
                SystemAurUpdateAurPhaseStatus::StoppedOnWorkItemFailure &&
            result.status ==
                SystemAurUpdateOperationStatus::StoppedOnAurFailure &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            !result.is_success(),
        "AUR work-item failure aggregate semantics differ");
    expect(
        child.reduced_operation_result.targets.size() == 3 &&
            child.reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::Updated &&
            child.reduced_operation_result.targets[1].status ==
                AurUpdateOperationTargetStatus::Failed &&
            child.reduced_operation_result.targets[2].status ==
                AurUpdateOperationTargetStatus::NotAttempted &&
            child.reduced_operation_result.execution_work_items.size() ==
                3 &&
            child.reduced_operation_result.execution_work_items[0].status ==
                AurUpdateWorkItemExecutionStatus::Updated &&
            child.reduced_operation_result.execution_work_items[1].status ==
                AurUpdateWorkItemExecutionStatus::Failed &&
            child.reduced_operation_result.execution_work_items[2].status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted,
        "Updated/Failed/NotAttempted inner partial was flattened");
    expect(
        execution_stub::call_history().size() == 2,
        "AUR work-item failure did not fail fast");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_cancellation_preserves_inner_partial() {
    reset_stubs();
    const std::vector<RootSpec> roots{
        {"first-updated", "first-updated"},
        {"middle-failed", "middle-failed"},
        {"last-not-attempted", "last-not-attempted"}};
    query_stub::set_foreign_inventory(
        inventory_for_roots(roots));
    enqueue_exact_update_query(roots);
    return_build_plan(root_plan(roots),
                      {roots[0].package_name,
                       roots[1].package_name,
                       roots[2].package_name});
    const AppConfig config;
    execution_stub::enqueue_success(
        expected_system_aur_execution(
            0, roots[0].package_name, roots[0].package_base,
            InstalledPackageReason::Explicit, config),
        roots[0].package_base,
        selected_system_aur_child(
            roots[0].package_name,
            InstalledPackageReason::Explicit));
    execution_stub::enqueue_confirmation_stop(
        expected_system_aur_execution(
            1, roots[1].package_name, roots[1].package_base,
            InstalledPackageReason::Explicit, config),
        ConfirmationCancelled{ConfirmationCancellationReason::ExplicitToken});

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR work-item failure");

    expect(
        result.aur.status ==
                SystemAurUpdateAurPhaseStatus::StoppedOnWorkItemCancellation &&
            result.status ==
                SystemAurUpdateOperationStatus::StoppedOnAurCancellation &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            !result.is_success(),
        "AUR work-item failure aggregate semantics differ");
    expect(
        child.reduced_operation_result.targets.size() == 3 &&
            child.reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::Updated &&
            child.reduced_operation_result.targets[1].status ==
                AurUpdateOperationTargetStatus::Cancelled &&
            child.reduced_operation_result.targets[2].status ==
                AurUpdateOperationTargetStatus::NotAttempted &&
            child.reduced_operation_result.execution_work_items.size() ==
                3 &&
            child.reduced_operation_result.execution_work_items[0].status ==
                AurUpdateWorkItemExecutionStatus::Updated &&
            child.reduced_operation_result.execution_work_items[1].status ==
                AurUpdateWorkItemExecutionStatus::Cancelled &&
            child.reduced_operation_result.execution_work_items[2].status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted,
        "Updated/Failed/NotAttempted inner partial was flattened");
    expect(
        execution_stub::call_history().size() == 2,
        "AUR work-item failure did not fail fast");
    expect(!result.has_inconsistency() && !result.aur.diagnostic &&
               result.stopped_phase == SystemAurUpdateOperationPhase::AurExecution &&
               result.repository.status == SystemAurUpdateRepositoryPhaseStatus::Completed,
           "Known cancellation lost repository prefix or became internal inconsistency");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_cleanup_failure_preserves_child_phase() {
    reset_stubs();
    const std::vector<RootSpec> roots{
        {"cleanup-failed", "cleanup-failed"},
        {"cleanup-pending", "cleanup-pending"}};
    query_stub::set_foreign_inventory(
        inventory_for_roots(roots));
    enqueue_exact_update_query(roots);
    return_build_plan(
        root_plan(roots),
        {roots[0].package_name, roots[1].package_name});
    const AppConfig config;
    execution_stub::enqueue_cleanup_failure(
        expected_system_aur_execution(
            0, roots[0].package_name, roots[0].package_base,
            InstalledPackageReason::Explicit, config),
        roots[0].package_base,
        selected_system_aur_child(
            roots[0].package_name,
            InstalledPackageReason::Explicit),
        {},
        "fixture coordinator cleanup failure");

    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const FilteredAurUpdateExecutionResult& child =
        require_system_aur_child(result, "system+AUR cleanup failure");

    expect(
        result.aur.status ==
                SystemAurUpdateAurPhaseStatus::
                    StoppedAfterCleanupFailure &&
            result.status ==
                SystemAurUpdateOperationStatus::
                    StoppedAfterAurCleanupFailure &&
            result.has_cleanup_failure() &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase(),
        "Cleanup child failure aggregate semantics differ");
    expect(
        child.reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::UpdatedCleanupFailed &&
            child.reduced_operation_result.targets[1].status ==
                AurUpdateOperationTargetStatus::NotAttempted &&
            child.reduced_operation_result.execution_work_items[0].status ==
                AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed &&
            child.reduced_operation_result.execution_work_items[1].status ==
                AurUpdateWorkItemExecutionStatus::NotAttempted,
        "Cleanup/NotAttempted child phase information was flattened");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_reducer_fails_closed_on_malformed_child() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "Malformed-child fixture did not start from success");

    const std::size_t repository_call_count =
        repository_stub::ordered_argument_calls().size();
    SystemAurUpdateOperationResult replay =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(
        replay.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            replay.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::NotAttempted &&
            replay.repository.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::
                        PriorAggregateInconsistency} &&
            replay.foreign_inventory.not_attempted_reason ==
                replay.repository.not_attempted_reason &&
            replay.query.not_attempted_reason ==
                replay.repository.not_attempted_reason &&
            replay.aur.not_attempted_reason ==
                replay.repository.not_attempted_reason &&
            repository_stub::ordered_argument_calls().size() ==
                repository_call_count,
        "Consumed coordinator capability did not retain PriorInconsistency without replaying repository work");

    result.aur.operation_result->reduced_operation_result.status =
        AurUpdateOperationStatus::InconsistentResult;
    SystemAurUpdateOperationResult malformed =
        reduce_system_aur_update_result(std::move(result));
    expect(
        malformed.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            malformed.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            malformed.has_inconsistency() &&
            malformed.has_partial_completion() &&
            !malformed.is_success() &&
            malformed.aur.operation_result->reduced_operation_result.status ==
                AurUpdateOperationStatus::InconsistentResult,
        "Malformed child result failed open or was flattened");
}

void test_system_aur_reducer_rejects_policy_mismatch() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(
        result.is_success(),
        "Policy-mismatch fixture did not start from success");

    result.aur.operation_result->devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    SystemAurUpdateOperationResult malformed =
        reduce_system_aur_update_result(std::move(result));
    expect(
        malformed.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            malformed.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            malformed.has_inconsistency() &&
            malformed.has_partial_completion() &&
            !malformed.is_success() &&
            malformed.aur.operation_result
                    ->devel_requires_check_policy ==
                DevelRequiresCheckPolicy::BlockOperation,
        "System/AUR RequiresCheck policy mismatch failed open");
}

void test_system_aur_reducer_correlates_exact_query_snapshot() {
    reset_stubs();
    const RootSpec root{"query-correlated", "query-correlated-base"};
    query_stub::set_foreign_inventory(
        inventory_for_roots({root}));
    enqueue_exact_update_query({root});
    return_build_plan(root_plan({root}), {root.package_name});
    const AppConfig config;
    execution_stub::enqueue_success(
        expected_system_aur_execution(
            0, root.package_name, root.package_base,
            InstalledPackageReason::Explicit, config),
        root.package_base,
        selected_system_aur_child(
            root.package_name,
            InstalledPackageReason::Explicit));
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "Query-correlation fixture did not start from success");

    result.query.query_result->plan.entries.front()
        .aur_package->package_base = "forged-outer-base";
    SystemAurUpdateOperationResult inconsistent =
        reduce_system_aur_update_result(std::move(result));
    expect(
        inconsistent.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            inconsistent.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            inconsistent.has_inconsistency() &&
            inconsistent.has_partial_completion() &&
            !inconsistent.is_success() &&
            inconsistent.query.query_result->plan.entries.front()
                    .aur_package->package_base ==
                "forged-outer-base" &&
            inconsistent.aur.operation_result->query_result.plan.entries
                    .front()
                    .aur_package->package_base ==
                root.package_base,
        "Outer/child query snapshot mismatch failed open or lost evidence");
    query_stub::require_script_consumed();
    execution_stub::require_script_consumed();
}

void test_system_aur_reducer_correlates_inventory_and_query() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "Inventory-correlation fixture did not start from success");

    result.foreign_inventory.inventory.push_back(
        InstalledPackageMetadata{
            "stale-pre-repository-foreign",
            "9.0-1",
            InstalledPackageReason::Dependency});
    SystemAurUpdateOperationResult inconsistent =
        reduce_system_aur_update_result(std::move(result));
    expect(
        inconsistent.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            inconsistent.has_inconsistency() &&
            inconsistent.has_partial_completion() &&
            !inconsistent.is_success() &&
            inconsistent.foreign_inventory.inventory.front().name ==
                "stale-pre-repository-foreign" &&
            inconsistent.query.query_result->plan.entries.empty(),
        "Fresh inventory/query mismatch failed open or lost evidence");
}

void test_system_aur_reducer_correlates_compatible_request() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "Repository-request correlation fixture did not start from success");

    result.repository.ordered_pacman_args.clear();
    SystemAurUpdateOperationResult inconsistent =
        reduce_system_aur_update_result(std::move(result));
    expect(
        inconsistent.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            inconsistent.has_inconsistency() &&
            inconsistent.has_partial_completion() &&
            !inconsistent.is_success() &&
            inconsistent.repository.ordered_pacman_args.empty() &&
            inconsistent.repository.compatible_request
                    ->ordered_pacman_args() ==
                std::vector<std::string>{"-Syu"},
        "Compatible request/executed argv mismatch failed open or lost evidence");
}

void test_system_aur_reducer_preserves_contradictory_not_attempted_reason() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "NotAttempted-reason fixture did not start from success");

    result.aur.not_attempted_reason =
        SystemAurUpdateNotAttemptedReason::RepositoryFailure;
    SystemAurUpdateOperationResult inconsistent =
        reduce_system_aur_update_result(std::move(result));
    expect(
        inconsistent.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            inconsistent.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            inconsistent.has_inconsistency() &&
            inconsistent.aur.not_attempted_reason ==
                std::optional<SystemAurUpdateNotAttemptedReason>{
                    SystemAurUpdateNotAttemptedReason::RepositoryFailure} &&
            !inconsistent.is_success(),
        "Contradictory AUR NotAttempted reason was erased or failed open");
}

void test_system_aur_reducer_rejects_contradictory_child_phase_status() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "Contradictory child-status fixture did not start from success");

    result.aur.status =
        SystemAurUpdateAurPhaseStatus::StoppedOnWorkItemFailure;
    SystemAurUpdateOperationResult inconsistent =
        reduce_system_aur_update_result(std::move(result));
    expect(
        inconsistent.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            inconsistent.aur.status ==
                SystemAurUpdateAurPhaseStatus::InconsistentResult &&
            inconsistent.has_inconsistency() &&
            inconsistent.has_partial_completion() &&
            !inconsistent.is_success() &&
            inconsistent.aur.operation_result->reduced_operation_result.status ==
                AurUpdateOperationStatus::NoUpdates,
        "Contradictory child-derived AUR phase status failed open or lost evidence");
}

void test_system_aur_reducer_rejects_unknown_query_phase_status() {
    reset_stubs();
    repository_stub::set_after_success([] {
        require_no_pre_repository_authority();
        query_stub::set_foreign_inventory({});
    });
    const AppConfig config;
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_fixture();
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(),
           "Unknown query-status fixture did not start from success");

    result.query.status =
        static_cast<SystemAurUpdateQueryPhaseStatus>(99);
    SystemAurUpdateOperationResult inconsistent =
        reduce_system_aur_update_result(std::move(result));
    expect(
        inconsistent.status ==
                SystemAurUpdateOperationStatus::InconsistentResult &&
            inconsistent.has_inconsistency() &&
            inconsistent.has_partial_completion() &&
            !inconsistent.is_success() &&
            inconsistent.query.status ==
                static_cast<SystemAurUpdateQueryPhaseStatus>(99),
        "Unknown query phase status failed open or lost evidence");
}

void test_empty_query_plan_is_normal_noop() {
    reset_stubs();
    const AppConfig config;

    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            {}, NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_noop(), "Empty query was not prepared as a no-op");
    expect(prepared.filtered_plan().entries.empty(),
           "Empty query produced filtered targets");
    expect(preflight_stub::resolver_call_count() == 0,
           "Empty query called BuildPlan resolver");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success(), "Empty query operation was not successful");
    expect(result.reduced_operation_result.status ==
               AurUpdateOperationStatus::NoUpdates,
           "Empty query did not reduce to NoUpdates");
    expect(!result.execution.has_value(),
           "Empty query produced an execution result");
    expect_no_mutation("empty query");
}

void test_real_query_wrapper_and_no_explicit_satisfaction_match_legacy_path() {
    reset_stubs();
    const ForeignPackageInventory inventory{{"legacy-root", "1.0-1", InstalledPackageReason::Explicit}};
    query_stub::set_foreign_inventory(inventory);
    query_stub::enqueue_info_many_result({{"legacy-root", package_info("legacy-root")}});
    query_stub::enqueue_vercmp_result("1");

    AurUpdateQueryResult query = query_installed_aur_updates();
    query_stub::require_script_consumed();
    expect(query_stub::repository_configuration_calls() == 1 &&
               query_stub::inventory_calls() == 1,
           "Legacy query wrapper did not obtain one foreign inventory");
    expect(query.plan.entries.size() == 1 &&
               query.plan.entries[0].classification ==
                   AurUpdateClassification::UpdateAvailable,
           "Real query wrapper did not create an update candidate");

    return_build_plan(
        root_plan({{"legacy-root", "legacy-root"}}),
        {"legacy-root"});
    AppConfig config;
    config.user_config.review.pkgbuild = ReviewPolicy::Skip;
    config.user_config.review.diff = ReviewPolicy::Skip;
    config.user_config.build.mode = BuildMode::Clean;
    config.no_confirm = true;
    config.rm_deps = true;
    config.editor = "fixture-editor";
    config.provider_selection =
        make_provider_selection_session(config.no_confirm);

    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            std::move(query), NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_prepared(),
           "No-explicit satisfaction did not prepare the legacy AUR update");
    expect(
        preflight_stub::resolver_selection_callback_presence() ==
            std::vector<bool>{true},
        "Filtered operation did not forward the AppConfig provider-selection session");
    expect(prepared.filtered_plan().entries.size() == 1 &&
               prepared.target_and_build_unit_plan()
                       .selected_targets.size() == 1 &&
               prepared.target_and_build_unit_plan()
                       .selected_build_units.size() == 1 &&
               prepared.source_build_preparation()
                   ->externally_satisfied_build_units.empty(),
           "No-explicit satisfaction changed legacy selection semantics");
    expect(preparation_stub::supported_options_guard_history() ==
               std::vector<bool>{true},
           "rm_deps option did not reach preparation");
    expect(
        preparation_stub::strict_preference_read_history() ==
                std::vector<std::string>{"legacy-root"} &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0,
        "Strict normal-AUR preparation changed preference IO");

    enqueue_installed(prepared, config, 1);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success() && result.changed_package_state(),
           "Legacy-equivalent operation did not update successfully");
    expect_statuses(
        result, {AurUpdateOperationTargetStatus::Updated},
        "legacy-equivalent operation");
    expect_success_lifecycle(
        execution_stub::call_history(), {"legacy-root"}, config,
        "legacy-equivalent operation");
    execution_stub::require_script_consumed();
}

void test_ignore_policy_reaches_filtered_preparation_without_preference_io() {
    reset_stubs();
    return_build_plan(
        root_plan(
            {{"ignore-filtered-child", "ignore-filtered-base"}}),
        {"ignore-filtered-child"});
    preparation_stub::enqueue_source_preference_result(
        "ignore-filtered-child",
        SourcePreferenceLoaded{
            .entry_path =
                "/stub/preferences/ignore-filtered-child",
            .environment = SourceBuildEnvironment{
                std::vector<SourceEnvironmentAssignment>{
                    {"CFLAGS", "-O3"}}},
            .warnings = {"must remain unread"},
            .raw_contents = {},
            .identity = std::nullopt,
        });
    preparation_stub::enqueue_source_preference_result(
        "ignore-filtered-base",
        SourcePreferenceFailure{
            SourcePreferenceFailureKind::ReadFailed,
            "/stub/preferences/ignore-filtered-base",
            std::make_error_code(std::errc::io_error),
            std::nullopt,
            "must remain unread"});

    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        ::prepare_filtered_aur_update_operation(
            query_result({update_entry(
                "ignore-filtered-child", "ignore-filtered-base")}),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Ignore, config);
    expect(
        prepared.is_prepared() &&
            prepared.source_build_preparation()->warnings.empty(),
        "Filtered Ignore did not prepare a normal AUR work item");
    const ProductionSourceBuildWorkItem& work_item =
        require_production_invocation(prepared).work_items.front();
    expect(
        work_item.request.package_name == "ignore-filtered-child" &&
            work_item.request.checkout_name ==
                "ignore-filtered-base" &&
            work_item.request.custom_environment
                .ordered_assignments.empty() &&
            work_item.required_targets.size() == 1 &&
            work_item.required_targets.front().package_name ==
                "ignore-filtered-child" &&
            work_item.required_targets.front().package_base ==
                "ignore-filtered-base" &&
            work_item.required_targets.front().desired_reason ==
                DesiredInstallReason::Explicit,
        "Filtered Ignore changed environment, identity, or install reason");
    expect(
        preparation_stub::strict_preference_read_history().empty() &&
            preparation_stub::
                    source_preference_directory_snapshot_call_count() ==
                0 &&
            preparation_stub::database_call_count() == 1 &&
            preparation_stub::reviewed_state_preflight_call_count() == 1,
        "Filtered Ignore crossed preference IO or skipped generic safety");
}

void test_explicit_inventory_query_core_bypasses_inventory_wrapper() {
    reset_stubs();
    query_stub::enqueue_info_many_result({{"latest-root", package_info("latest-root")}});
    query_stub::enqueue_vercmp_result("1");
    ForeignPackageInventory latest_inventory{{"latest-root", "1.0-1", InstalledPackageReason::Explicit}};

    AurUpdateQueryResult query = query_aur_updates_for_foreign_inventory(
        std::move(latest_inventory));
    query_stub::require_script_consumed();
    expect(query_stub::repository_configuration_calls() == 0 &&
               query_stub::inventory_calls() == 0,
           "Explicit inventory query unexpectedly reopened system inventory");
    expect(query_stub::info_many_call_history() ==
               std::vector<std::vector<std::string>>{{"latest-root"}},
           "Explicit inventory query lost inventory order");

    return_build_plan(
        root_plan({{"latest-root", "latest-root"}}),
        {"latest-root"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            std::move(query), NoExplicitSourceSatisfaction{}, config);
    enqueue_no_change(prepared, config, 1);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success() && !result.changed_package_state(),
           "Explicit inventory query result did not complete");
    execution_stub::require_script_consumed();
}

void test_package_name_target_exclusion() {
    reset_stubs();
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("name-match", "aur-base")}),
            explicit_satisfaction(
                {explicit_source("name-match", "source-base")}),
            config);

    expect(prepared.is_noop(), "Package-name exclusion was not a no-op");
    expect(prepared.target_and_build_unit_plan()
                       .target_dispositions.size() == 1 &&
               prepared.target_and_build_unit_plan()
                       .target_dispositions[0]
                       .disposition ==
                   UpgradeAllTargetDisposition::
                       ExcludedByExplicitPackageName,
           "Package-name duplicate was not excluded");
    expect(prepared.target_and_build_unit_plan()
                   .excluded_duplicate_target_indexes ==
               std::vector<std::size_t>{0},
           "Package-name exclusion lost duplicate attribution");
    expect(prepared.original_query_result()
                   .plan.entries[0]
                   .installed_name == "name-match",
           "Package-name exclusion lost original payload");
    expect(preflight_stub::resolver_call_count() == 0,
           "Excluded package-name target reached preflight resolver");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success() && result.has_duplicate_exclusions(),
           "Package-name exclusion result helpers differ");
    expect_no_mutation("package-name exclusion");
}

void test_package_base_target_exclusion() {
    reset_stubs();
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("suite-cli", "suite")}),
            explicit_satisfaction(
                {explicit_source("source-suite", "suite")}),
            config);

    expect(prepared.is_noop(), "PackageBase exclusion was not a no-op");
    const UpgradeAllTargetPlanEntry& target =
        prepared.target_and_build_unit_plan()
            .target_dispositions.front();
    expect(target.disposition ==
                   UpgradeAllTargetDisposition::
                       ExcludedByExplicitPackageBase &&
               target.explicit_source.has_value() &&
               target.explicit_source->matched_package_base ==
                   std::optional<std::string>{"suite"},
           "PackageBase exclusion lost typed source attribution");
    expect(preflight_stub::resolver_call_count() == 0,
           "Excluded PackageBase reached preflight resolver");
    static_cast<void>(execute_prepared_filtered_aur_update_operation(
        std::move(prepared), config));
    expect_no_mutation("PackageBase exclusion");
}

void test_split_package_targets_share_exclusion_attribution() {
    reset_stubs();
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("suite-cli", "suite"),
                          update_entry("suite-lib", "suite")}),
            explicit_satisfaction(
                {explicit_source("suite-source", "suite")}),
            config);

    expect(prepared.is_noop(), "Split-package exclusion was not a no-op");
    expect(prepared.target_and_build_unit_plan()
                   .excluded_duplicate_target_indexes ==
               std::vector<std::size_t>({0, 1}),
           "Split-package exclusion indexes differ");
    for(const UpgradeAllTargetPlanEntry& target :
        prepared.target_and_build_unit_plan().target_dispositions) {
        expect(target.disposition ==
                       UpgradeAllTargetDisposition::
                           ExcludedByExplicitPackageBase &&
                   target.explicit_source.has_value() &&
                   target.explicit_source->explicit_source_indexes ==
                       std::vector<std::size_t>{0},
               "Split-package exclusion attribution differs");
    }
    static_cast<void>(execute_prepared_filtered_aur_update_operation(
        std::move(prepared), config));
    expect_no_mutation("split-package exclusion");
}

void test_original_filtered_and_preflight_index_mapping() {
    reset_stubs();
    const AppConfig config;
    return_build_plan(
        root_plan({{"beta", "beta"}}), {"beta"});

    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({current_entry("current"),
                          update_entry("alpha", "alpha-base"),
                          non_aur_entry("foreign"),
                          update_entry("beta")}),
            explicit_satisfaction(
                {explicit_source("alpha", "source-alpha")}),
            config);

    expect(
        prepared.is_prepared(),
        "Mapped fixture did not prepare:" +
            prepared_diagnostic(prepared));
    expect(prepared.filtered_to_original_indexes() ==
               std::vector<std::size_t>({0, 2, 3}),
           "Filtered-to-original mapping differs");
    expect(prepared.original_to_filtered_indexes() ==
               std::vector<std::optional<std::size_t>>({0, std::nullopt, 1, 2}),
           "Original-to-filtered mapping differs");
    expect(prepared.filtered_plan().entries.size() == 3 &&
               prepared.filtered_plan().entries[0].installed_name ==
                   "current" &&
               prepared.filtered_plan().entries[1].installed_name ==
                   "foreign" &&
               prepared.filtered_plan().entries[2].installed_name ==
                   "beta",
           "Filtered target order differs");
    expect(prepared.selected_target_correlations().size() == 1,
           "Selected target correlation count differs");
    const FilteredAurUpdateTargetCorrelation& mapping =
        prepared.selected_target_correlations().front();
    expect(mapping.planner_target_index == 1 &&
               mapping.original_query_plan_index == 3 &&
               mapping.selected_target_index == 0 &&
               mapping.filtered_update_plan_index == 2 &&
               mapping.preflight_invocation_index ==
                   std::optional<std::size_t>{0} &&
               mapping.build_plan_root_index ==
                   std::optional<std::size_t>{0},
           "Original/selected/filtered/preflight mapping was conflated");
    expect(preflight_stub::resolver_calls() ==
               std::vector<std::vector<std::string>>{{"beta"}},
           "Excluded root leaked into BuildPlan resolution");

    enqueue_installed(prepared, config, 1);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.selected_target_results.size() == 1 &&
               result.selected_target_results[0]
                       .original_query_plan_index == 3 &&
               result.selected_target_results[0]
                       .filtered_update_plan_index == 2 &&
               result.selected_target_results[0].operation_result.status ==
                   AurUpdateOperationTargetStatus::Updated,
           "Reduced selected result lost original index attribution");
    expect(result.reduced_operation_result.targets.size() == 3 &&
               result.reduced_operation_result.targets[0].status ==
                   AurUpdateOperationTargetStatus::Skipped &&
               result.reduced_operation_result.targets[1].status ==
                   AurUpdateOperationTargetStatus::Skipped &&
               result.reduced_operation_result.targets[2].status ==
                   AurUpdateOperationTargetStatus::Updated,
           "Filtered result order/status differs");
    expect_success_lifecycle(
        execution_stub::call_history(), {"beta"}, config,
        "mapped selected target");
    execution_stub::require_script_consumed();
}

void test_requires_check_policy_skip_adapter_and_full_retention() {
    reset_stubs();
    const AurUpdatePlan original_plan{{
        classified_update_entry("normal-update-git"),
        requires_check_entry(
            "independent-devel-git", "independent-devel-base"),
        current_entry("current-package"),
    }};

    std::vector<FilteredAurUpdateOperationIssue> adapter_issues;
    const FilteredAurUpdateTargetAdapter adapter =
        adapt_aur_update_plan_for_upgrade_all(
            original_plan,
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            adapter_issues);
    expect(
        adapter_issues.empty() && adapter.entries.size() == 3 &&
            adapter.entries[0].original_query_plan_index == 0 &&
            adapter.entries[0].update == original_plan.entries[0] &&
            adapter.entries[0].disposition ==
                FilteredAurUpdateTargetAdapterDisposition::PlannerTarget &&
            adapter.entries[0].planner_target_index ==
                std::optional<std::size_t>{0} &&
            adapter.entries[1].original_query_plan_index == 1 &&
            adapter.entries[1].update == original_plan.entries[1] &&
            adapter.entries[1].disposition ==
                FilteredAurUpdateTargetAdapterDisposition::
                    RequiresCheckPolicySkip &&
            adapter.entries[1].devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::SkipIndependentTarget} &&
            adapter.entries[1].devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly} &&
            !adapter.entries[1].planner_target_index.has_value() &&
            adapter.entries[2].original_query_plan_index == 2 &&
            adapter.entries[2].update == original_plan.entries[2] &&
            adapter.entries[2].disposition ==
                FilteredAurUpdateTargetAdapterDisposition::NormalSkip &&
            !adapter.entries[2].planner_target_index.has_value(),
        "RequiresCheck adapter disposition lost full target identity");
    expect(
        adapter.planner_targets.size() == 1 &&
            adapter.planner_targets.front().package_name ==
                "normal-update-git" &&
            adapter.planner_target_to_original_query_plan_index ==
                std::vector<std::size_t>{0} &&
            adapter.original_query_plan_to_planner_target_index ==
                std::vector<std::optional<std::size_t>>{
                    std::size_t{0}, std::nullopt, std::nullopt},
        "RequiresCheck adapter leaked a policy skip into planner roots");

    std::vector<FilteredAurUpdateOperationIssue> block_issues;
    const FilteredAurUpdateTargetAdapter block_adapter =
        adapt_aur_update_plan_for_upgrade_all(
            AurUpdatePlan{{original_plan.entries[1]}},
            DevelRequiresCheckPolicy::BlockOperation,
            block_issues);
    expect(
        block_issues.empty() && block_adapter.entries.size() == 1 &&
            block_adapter.entries.front().disposition ==
                FilteredAurUpdateTargetAdapterDisposition::PlannerTarget &&
            !block_adapter.entries.front()
                 .devel_requires_check_policy.has_value() &&
            !block_adapter.entries.front()
                 .devel_requires_check_reason.has_value() &&
            block_adapter.entries.front().planner_target_index ==
                std::optional<std::size_t>{0} &&
            block_adapter.planner_targets.size() == 1 &&
            block_adapter.planner_targets.front().status ==
                UpgradeAllAurTargetStatus::Incomplete,
        "BlockOperation adapter behavior changed for RequiresCheck");

    return_build_plan(
        root_plan({{"normal-update-git", "normal-update-git"}}),
        {"normal-update-git"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        ::prepare_filtered_aur_update_operation(
            query_result(original_plan.entries),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, config);

    expect(
        prepared.is_prepared() &&
            prepared.original_query_result().plan == original_plan &&
            prepared.filtered_plan() == original_plan &&
            prepared.filtered_to_original_indexes() ==
                std::vector<std::size_t>{0, 1, 2} &&
            prepared.original_to_filtered_indexes() ==
                std::vector<std::optional<std::size_t>>{
                    std::size_t{0}, std::size_t{1}, std::size_t{2}},
        "RequiresCheck policy skip disappeared from the full filtered plan");
    expect(
        prepared.target_adapter_result().entries[1].disposition ==
                FilteredAurUpdateTargetAdapterDisposition::
                    RequiresCheckPolicySkip &&
            prepared.target_adapter_result()
                    .entries[1]
                    .devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::SkipIndependentTarget} &&
            prepared.target_adapter_result()
                    .entries[1]
                    .devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly} &&
            prepared.target_adapter_result()
                    .entries[1]
                    .filtered_update_plan_index ==
                std::optional<std::size_t>{1} &&
            prepared.selected_target_correlations().size() == 1 &&
            prepared.selected_target_correlations()
                    .front()
                    .original_query_plan_index == 0 &&
            prepared.selected_target_correlations()
                    .front()
                    .filtered_update_plan_index == 0 &&
            preflight_stub::resolver_calls() ==
                std::vector<std::vector<std::string>>{
                    {"normal-update-git"}},
        "RequiresCheck planner subset or correlation mapping differs");
    const AurUpdateExecutionPreflight& preflight =
        prepared.execution_preflight();
    expect(
        preflight.targets.size() == 3 &&
            preflight.targets[0].status ==
                AurUpdateExecutionTargetStatus::Executable &&
            preflight.targets[0].update.devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck &&
            project_aur_update_effective_state(
                preflight.targets[0].update) ==
                AurUpdateEffectiveState::UpdateAvailable &&
            preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            preflight.targets[1].issues.size() == 1 &&
            preflight.targets[1]
                    .issues.front()
                    .devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly} &&
            preflight.targets[2].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            preflight.targets[2].skip_kind ==
                AurUpdateExecutionSkipKind::UpToDate,
        "Filtered preflight changed update precedence or skip kinds");

    reset_stubs();
    PreparedFilteredAurUpdateOperation skip_only =
        ::prepare_filtered_aur_update_operation(
            query_result({original_plan.entries[1]}),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, config);
    expect(
        skip_only.is_noop() && preflight_stub::resolver_call_count() == 0 &&
            skip_only.original_query_result().plan.entries.size() == 1 &&
            skip_only.filtered_plan().entries ==
                std::vector<AurUpdatePlanEntry>{
                    original_plan.entries[1]} &&
            skip_only.target_adapter_result()
                    .entries.front()
                    .disposition ==
                FilteredAurUpdateTargetAdapterDisposition::
                    RequiresCheckPolicySkip &&
            skip_only.target_adapter_result()
                    .entries.front()
                    .devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::SkipIndependentTarget} &&
            skip_only.target_adapter_result()
                    .entries.front()
                    .devel_requires_check_reason ==
                std::optional<DevelRequiresCheckReason>{
                    DevelRequiresCheckReason::SuffixCandidateOnly} &&
            skip_only.execution_preflight().targets.front().status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            skip_only.execution_preflight().targets.front().skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            project_aur_update_effective_state(
                skip_only.execution_preflight().targets.front().update) ==
                AurUpdateEffectiveState::RequiresCheck,
        "RequiresCheck-only filtered capability was dropped, resolved, or blocked");
}

void test_requires_check_skip_observation_foundation() {
    const AppConfig config;
    const AurUpdatePlanEntry requires_check = requires_check_entry(
        "observed-independent-git", "observed-independent-base");

    reset_stubs();
    const FilteredAurUpdateObservation independent =
        ::observe_filtered_aur_update_operation(
            query_result({requires_check}),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, config);
    expect(
        independent.is_noop() && !independent.is_blocked() &&
            preflight_stub::resolver_call_count() == 0 &&
            independent.preflight.targets.size() == 1 &&
            independent.preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            independent.preflight.targets.front().skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            independent.source_build_observation.has_value() &&
            independent.source_build_observation->is_noop(),
        "Independent RequiresCheck observation was blocked or flattened");

    reset_stubs();
    BuildPlan required_plan =
        root_plan({{"observed-root", "observed-root"}});
    add_aur_dependency(
        required_plan, "observed-root", "observed-required-git",
        "observed-required-base");
    return_build_plan(std::move(required_plan), {"observed-root"});
    const FilteredAurUpdateObservation required =
        ::observe_filtered_aur_update_operation(
            query_result({
                update_entry("observed-root"),
                requires_check_entry(
                    "observed-required-git",
                    "observed-required-base"),
            }),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, config);
    expect(
        required.is_blocked() && !required.is_noop() &&
            required.preflight.targets.size() == 2 &&
            required.preflight.targets[0].status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            required.preflight.targets[1].status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            required.preflight.targets[1].skip_kind ==
                AurUpdateExecutionSkipKind::RequiredDevelRequiresCheck &&
            has_preflight_issue(
                required.preflight.targets[0],
                AurUpdateExecutionReason::
                    RequiredDevelTargetRequiresCheck) &&
            required.source_build_observation.has_value() &&
            required.source_build_observation->is_blocked(),
        "Required RequiresCheck observation lost its typed blocker");

    reset_stubs();
    BuildPlan execution_plan =
        root_plan({{"execution-root", "execution-root"}});
    add_aur_dependency(
        execution_plan, "execution-root", "execution-required-git",
        "execution-required-base");
    return_build_plan(std::move(execution_plan), {"execution-root"});
    PreparedFilteredAurUpdateOperation blocked_preparation =
        ::prepare_filtered_aur_update_operation(
            query_result({
                update_entry("execution-root"),
                requires_check_entry(
                    "execution-required-git",
                    "execution-required-base"),
            }),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, config);
    expect(
        blocked_preparation.is_blocked() &&
            blocked_preparation.source_build_preparation().has_value() &&
            blocked_preparation.source_build_preparation()->is_blocked() &&
            !blocked_preparation.source_build_preparation()
                 ->invocation.has_value(),
        "Required RequiresCheck relation created an execution capability");
    const FilteredAurUpdateExecutionResult blocked_execution =
        ::execute_prepared_filtered_aur_update_operation(
            std::move(blocked_preparation), config);
    expect(
        !blocked_execution.execution.has_value() &&
            blocked_execution.reduced_operation_result.status ==
                AurUpdateOperationStatus::BlockedBeforeExecution,
        "Required RequiresCheck relation reached the runner");
    expect_no_mutation("required RequiresCheck completeness firewall");
}

void test_transitive_external_satisfaction_keeps_selected_root_executable() {
    reset_stubs();
    const AppConfig config;
    BuildPlan plan = root_plan({{"application", "application"}});
    add_aur_dependency(
        plan, "application", "middle-library", "middle-library");
    add_aur_dependency(
        plan, "middle-library", "external-library", "external-library",
        PackageRole::BuildDependency);
    return_build_plan(std::move(plan), {"application"});

    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("application")}),
            explicit_satisfaction({explicit_source(
                "external-library", "external-library")}),
            config);
    expect(prepared.is_prepared(),
           "Selected root with external dependency was blocked");
    expect(prepared.target_and_build_unit_plan()
                       .externally_satisfied_build_unit_indexes ==
                   std::vector<std::size_t>{0} &&
               prepared.target_and_build_unit_plan()
                       .selected_build_units.size() == 2,
           "Transitive external selection did not compact execution order");
    expect(prepared.source_build_preparation()
                   ->externally_satisfied_build_units.size() == 1,
           "External satisfaction was not carried into preparation");
    const AurUpdateExternallySatisfiedBuildUnit& external =
        prepared.source_build_preparation()
            ->externally_satisfied_build_units.front();
    expect(external.package_name == "external-library" &&
               external.package_base == "external-library" &&
               external.required_target_attributions.size() == 1 &&
               external.required_target_attributions.front()
                       .required_target.package_name ==
                   "external-library" &&
               external.affected_update_plan_indices ==
                   std::vector<std::size_t>{0} &&
               external.affected_roots ==
                   std::vector<RootTargetIdentity>{{0, "application"}} &&
               external.roles ==
                   std::vector<PackageRole>{
                       PackageRole::BuildDependency} &&
               external.external_satisfaction.matched_package_base ==
                   std::optional<std::string>{"external-library"},
           "External satisfaction lost identity/root/role attribution");

    enqueue_installed(prepared, config, 2);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success() && result.changed_package_state(),
           "Selected root with external dependency did not execute");
    expect_success_lifecycle(
        execution_stub::call_history(),
        {"middle-library", "application"}, config,
        "transitive external satisfaction");
    expect(std::none_of(
               execution_stub::call_history().begin(),
               execution_stub::call_history().end(),
               [](const execution_stub::ExecutionCall& call) {
                   return call.package_name == "external-library";
               }),
           "External unit reached checkout/build/install/cleanup boundary");
    execution_stub::require_script_consumed();
}

void test_multiple_child_external_satisfaction_keeps_set_snapshot() {
    reset_stubs();
    const AppConfig config;
    BuildPlan plan = root_plan({{"application", "application"}});
    add_aur_dependency(
        plan, "application", "split-runtime", "split-suite");
    add_aur_dependency(
        plan, "application", "split-tools", "split-suite",
        PackageRole::BuildDependency);
    return_build_plan(std::move(plan), {"application"});

    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("application")}),
            explicit_satisfaction(
                {explicit_source("split-source", "split-suite")}),
            config);
    expect(
        prepared.is_prepared(),
        "Selected root with external multiple-child unit was blocked" +
            prepared_diagnostic(prepared));
    expect(
        prepared.target_and_build_unit_plan()
                    .externally_satisfied_build_unit_indexes ==
                std::vector<std::size_t>{0} &&
            prepared.target_and_build_unit_plan()
                    .selected_build_units.size() == 1 &&
            prepared.source_build_preparation()
                    ->externally_satisfied_build_units.size() ==
                1,
        "External multiple-child selection or dense execution order differs");

    const AurUpdateExternallySatisfiedBuildUnit& external =
        prepared.source_build_preparation()
            ->externally_satisfied_build_units.front();
    expect(
        external.package_name.empty() &&
            external.package_base == "split-suite" &&
            external.plan_package_names ==
                std::vector<std::string>{
                    "split-runtime", "split-tools"} &&
            external.required_target_attributions.size() == 2 &&
            external.required_target_attributions[0]
                    .required_target.package_name ==
                "split-runtime" &&
            external.required_target_attributions[0].roles ==
                std::vector<PackageRole>{
                    PackageRole::RuntimeDependency} &&
            external.required_target_attributions[1]
                    .required_target.package_name ==
                "split-tools" &&
            external.required_target_attributions[1].roles ==
                std::vector<PackageRole>{
                    PackageRole::BuildDependency} &&
            !external.desired_install_reason.has_value() &&
            external.affected_update_plan_indices ==
                std::vector<std::size_t>{0} &&
            external.affected_roots ==
                std::vector<RootTargetIdentity>{{0, "application"}},
        "External multiple-child snapshot lost ordered child attribution");
    expect(
        preparation_stub::strict_preference_read_history() ==
            std::vector<std::string>{"application"},
        "External multiple-child unit reached source preference IO");

    enqueue_installed(prepared, config, 1);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated},
        "external multiple-child selected root");
    expect_success_lifecycle(
        execution_stub::call_history(), {"application"}, config,
        "external multiple-child selected root");
    execution_stub::require_script_consumed();
}

void test_one_external_unit_shared_by_multiple_roots() {
    reset_stubs();
    const AppConfig config;
    BuildPlan plan = root_plan({{"first-root", "first-root"},
                                {"second-root", "second-root"}});
    add_aur_dependency(
        plan, "first-root", "shared-library", "shared-library");
    add_aur_dependency(
        plan, "second-root", "shared-library", "shared-library");
    return_build_plan(std::move(plan), {"first-root", "second-root"});

    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("first-root"),
                          update_entry("second-root")}),
            explicit_satisfaction({explicit_source(
                "shared-library", "shared-library")}),
            config);
    expect(prepared.is_prepared(), "Shared external fixture did not prepare");
    expect(prepared.source_build_preparation()
                   ->externally_satisfied_build_units.size() == 1,
           "Shared external unit was duplicated");
    const AurUpdateExternallySatisfiedBuildUnit& external =
        prepared.source_build_preparation()
            ->externally_satisfied_build_units.front();
    expect(external.required_target_attributions.size() == 1 &&
               external.required_target_attributions.front()
                       .required_target.package_name ==
                   "shared-library" &&
               external.affected_update_plan_indices ==
                   std::vector<std::size_t>({0, 1}) &&
               external.affected_roots ==
                   std::vector<RootTargetIdentity>({{0, "first-root"},
                                                    {1, "second-root"}}),
           "Shared external unit lost multi-root attribution");

    enqueue_installed(prepared, config, 2);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::Updated},
        "shared external roots");
    expect_success_lifecycle(
        execution_stub::call_history(),
        {"first-root", "second-root"}, config,
        "shared external roots");
    execution_stub::require_script_consumed();
}

void test_query_recoverable_failure_is_retained_and_blocks_mutation() {
    reset_stubs();
    query_stub::enqueue_info_many_failure("fixture AUR transport failure");
    AurUpdateQueryResult query = query_aur_updates_for_foreign_inventory({{"query-failed", "1.0-1", InstalledPackageReason::Explicit}});
    query_stub::require_script_consumed();
    expect(query.recoverable_failures.size() == 1 &&
               query.recoverable_failures[0].package_names ==
                   std::vector<std::string>{"query-failed"} &&
               query.plan.entries[0].classification ==
                   AurUpdateClassification::MetadataUnavailable,
           "Query recoverable failure was not typed into the query result");

    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            std::move(query), NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked(),
           "Recoverable query failure did not block incomplete planning");
    expect(has_planner_issue(
               prepared.target_and_build_unit_plan(),
               UpgradeAllPlanningIssueKind::IncompleteAurTarget),
           "Query failure did not retain its planning issue");
    expect(preflight_stub::resolver_call_count() == 0,
           "Incomplete query target reached BuildPlan resolver");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.has_query_failure() && result.has_planning_issue() &&
               !result.is_success(),
           "Query/planning failure helpers differ");
    expect(result.query_result.recoverable_failures[0].diagnostic ==
               "fixture AUR transport failure",
           "Query failure diagnostic was lost");
    expect_no_mutation("query recoverable failure");
}

void test_requires_check_blocks_mixed_operation_before_mutation() {
    const AppConfig config;
    for(const SavedSourcePreferencePolicy saved_policy : {
            SavedSourcePreferencePolicy::Ignore,
            SavedSourcePreferencePolicy::Strict}) {
        reset_stubs();
        return_build_plan(
            root_plan({{"normal-update-git", "normal-update-git"}}),
            {"normal-update-git"});
        const std::string context =
            saved_policy == SavedSourcePreferencePolicy::Ignore
                ? "Ignore + BlockOperation"
                : "Strict + BlockOperation";

        PreparedFilteredAurUpdateOperation prepared =
            ::prepare_filtered_aur_update_operation(
                query_result({
                    classified_update_entry("normal-update-git"),
                    requires_check_entry("manual-check-git"),
                }),
                NoExplicitSourceSatisfaction{},
                DevelRequiresCheckPolicy::BlockOperation,
                saved_policy, config);

        expect(
            prepared.is_blocked(),
            context + ": RequiresCheck did not block the mixed operation");
        expect(
            prepared.devel_requires_check_policy_snapshot() ==
                    std::optional<DevelRequiresCheckPolicy>{
                        DevelRequiresCheckPolicy::BlockOperation} &&
                prepared.execution_preflight()
                        .devel_requires_check_policy ==
                    prepared.devel_requires_check_policy_snapshot() &&
                prepared.source_build_preparation().has_value() &&
                prepared.source_build_preparation()
                        ->devel_requires_check_policy ==
                    prepared.devel_requires_check_policy_snapshot(),
            context + ": policy snapshots diverged before execution");
        expect(
            prepared.filtered_plan().entries.size() == 2 &&
                prepared.execution_preflight().targets.size() == 2 &&
                prepared.execution_preflight().targets[0].status ==
                    AurUpdateExecutionTargetStatus::Executable &&
                prepared.execution_preflight()
                        .targets[0]
                        .update.devel_assessment.state() ==
                    DevelUpdateAssessmentState::RequiresCheck &&
                prepared.execution_preflight().targets[1].status ==
                    AurUpdateExecutionTargetStatus::Incomplete &&
                !prepared.execution_preflight()
                     .targets[1]
                     .skip_kind.has_value(),
            context + ": mixed operation lost order or block shape");
        expect(
            prepared.execution_preflight()
                        .targets[1]
                        .issues.front()
                        .reason ==
                    AurUpdateExecutionReason::DevelRequiresCheck &&
                prepared.execution_preflight()
                        .targets[1]
                        .issues.front()
                        .devel_requires_check_reason ==
                    DevelRequiresCheckReason::SuffixCandidateOnly,
            context + ": RequiresCheck preflight reason was flattened");
        expect(
            has_planner_issue(
                prepared.target_and_build_unit_plan(),
                UpgradeAllPlanningIssueKind::IncompleteAurTarget),
            context + ": RequiresCheck lost its planning blocker");
        expect(
            preparation_stub::strict_preference_read_history().empty() &&
                preparation_stub::
                        source_preference_directory_snapshot_call_count() ==
                    0 &&
                preparation_stub::database_call_count() == 0,
            context + ": RequiresCheck crossed preparation authority");

        FilteredAurUpdateExecutionResult result =
            execute_prepared_filtered_aur_update_operation(
                std::move(prepared), config);
        expect(
            !result.is_success() && !result.execution.has_value() &&
                result.has_consistent_devel_requires_check_policy_snapshot() &&
                result.devel_requires_check_policy ==
                    std::optional<DevelRequiresCheckPolicy>{
                        DevelRequiresCheckPolicy::BlockOperation} &&
                result.reduced_operation_result.status ==
                    AurUpdateOperationStatus::BlockedBeforeExecution,
            context + ": mixed operation reached execution or lost policy");
        expect(
            result.selected_target_results.size() == 1 &&
                result.selected_target_results.front()
                        .operation_result.status ==
                    AurUpdateOperationTargetStatus::NotAttempted,
            context + ": RequiresCheck entered the selected subset");
        expect(
            result.reduced_operation_result.targets.size() == 2 &&
                result.reduced_operation_result.targets[0].status ==
                    AurUpdateOperationTargetStatus::NotAttempted &&
                result.reduced_operation_result.targets[1].status ==
                    AurUpdateOperationTargetStatus::Incomplete,
            context + ": mixed result lost original filtered order");
        expect_no_mutation(context);
    }
}

void test_planner_issue_blocks_mutation_but_keeps_disposition() {
    reset_stubs();
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({unavailable_entry("incomplete-target")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked(), "Incomplete planner target was not blocked");
    expect(prepared.target_and_build_unit_plan()
                       .target_dispositions.size() == 1 &&
               prepared.target_and_build_unit_plan()
                       .target_dispositions[0]
                       .disposition ==
                   UpgradeAllTargetDisposition::IdentityIncomplete &&
               has_planner_issue(
                   prepared.target_and_build_unit_plan(),
                   UpgradeAllPlanningIssueKind::IncompleteAurTarget),
           "Planner issue lost known target disposition");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.has_planning_issue() && !result.is_success(),
           "Planner issue was rounded to success");
    expect_no_mutation("planner issue");
}

void test_initial_preflight_identity_blocker_stops_before_mutation() {
    reset_stubs();
    BuildPlan plan = root_plan({{"identity-root", "identity-root"}});
    plan.root_targets[0].requested_name = "wrong-root";
    plan.package_targets[0].roots[0] = plan.root_targets[0];
    return_build_plan(std::move(plan), {"identity-root"});

    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        ::prepare_filtered_aur_update_operation(
            query_result({update_entry("identity-root")}),
            NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Ignore, config);
    expect(prepared.is_blocked(),
           "Initial preflight identity mismatch was not blocked");
    expect(has_operation_issue(
               prepared.operation_issues(),
               FilteredAurUpdateOperationIssueKind::
                   BuildPlanRootIndexMissing),
           "Initial preflight mismatch did not retain missing root mapping");
    expect(has_operation_issue(
               prepared.operation_issues(),
               FilteredAurUpdateOperationIssueKind::
                   PreflightInvocationIdentityMismatch),
           "Initial preflight mismatch lost typed invocation identity issue");
    expect(prepared.execution_preflight().targets[0].status ==
               AurUpdateExecutionTargetStatus::Incomplete,
           "Initial preflight mismatch did not remain typed incomplete");
    expect(preparation_stub::strict_preference_read_history().empty() &&
               preparation_stub::
                       source_preference_directory_snapshot_call_count() ==
                   0 &&
               preparation_stub::database_call_count() == 0,
           "Preflight blocker crossed preparation external boundaries");

    static_cast<void>(execute_prepared_filtered_aur_update_operation(
        std::move(prepared), config));
    expect_no_mutation("initial preflight identity blocker");
}

void test_preflight_blocker_stops_before_mutation() {
    reset_stubs();
    BuildPlan plan = root_plan({{"blocked-root", "blocked-root"}});
    plan.unresolved.push_back("missing-dependency>=2");
    return_build_plan(std::move(plan), {"blocked-root"});

    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("blocked-root")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked() &&
               prepared.source_build_preparation()->is_blocked(),
           "Preflight blocker did not block preparation");
    expect(preparation_stub::strict_preference_read_history().empty() &&
               preparation_stub::database_call_count() == 0,
           "Preflight blocker reached strict reader or Pacman DB");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(!result.is_success() &&
               result.reduced_operation_result.has_blocking_targets(),
           "Preflight blocker was rounded to success");
    expect_no_mutation("preflight blocker");
}

void test_preparation_blocker_stops_before_mutation() {
    reset_stubs();
    return_build_plan(
        root_plan({{"preparation-root", "preparation-root"}}),
        {"preparation-root"});
    preparation_stub::fail_supported_options_guard(
        "fixture option guard failure");

    AppConfig config;
    config.rm_deps = true;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("preparation-root")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked() &&
               prepared.source_build_preparation()->is_blocked() &&
               !prepared.source_build_preparation()->issues.empty(),
           "Preparation option blocker did not fail closed");
    expect(preparation_stub::supported_options_guard_history() ==
               std::vector<bool>{true},
           "Preparation blocker lost option propagation");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(!result.is_success() &&
               !result.reduced_operation_result.preparation_issues.empty(),
           "Preparation blocker was rounded to success");
    expect_no_mutation("preparation blocker");
}

void test_all_updated() {
    reset_stubs();
    return_build_plan(
        root_plan({{"updated-a", "updated-a"},
                   {"updated-b", "updated-b"}}),
        {"updated-a", "updated-b"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("updated-a"),
                          update_entry("updated-b")}),
            NoExplicitSourceSatisfaction{}, config);
    enqueue_installed(prepared, config, 2);

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success() && result.changed_package_state() &&
               !result.has_partial_completion(),
           "All-updated helper semantics differ");
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::Updated},
        "all updated");
    expect_success_lifecycle(
        execution_stub::call_history(),
        {"updated-a", "updated-b"}, config, "all updated");
    execution_stub::require_script_consumed();
}

void test_all_no_change() {
    reset_stubs();
    return_build_plan(
        root_plan({{"same-a", "same-a"}, {"same-b", "same-b"}}),
        {"same-a", "same-b"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("same-a"),
                          update_entry("same-b")}),
            NoExplicitSourceSatisfaction{}, config);
    enqueue_no_change(prepared, config, 2);

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(result.is_success() && !result.changed_package_state(),
           "All-NoChange helper semantics differ");
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::NoChange,
         AurUpdateOperationTargetStatus::NoChange},
        "all no-change");
    execution_stub::require_script_consumed();
}

void test_same_package_base_children_execute_once_with_mixed_outcomes() {
    reset_stubs();
    return_build_plan(
        root_plan({{"split-runtime", "split-suite"},
                   {"split-cli", "split-suite"}}),
        {"split-runtime", "split-cli"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry(
                              "split-runtime", "split-suite",
                              InstalledPackageReason::Dependency),
                          update_entry("split-cli", "split-suite")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(
        prepared.is_prepared(),
        "Same-PackageBase targets did not prepare" +
            prepared_diagnostic(prepared));
    expect(
        prepared.target_and_build_unit_plan().selected_targets.size() ==
                2 &&
            prepared.target_and_build_unit_plan()
                    .selected_build_units.size() == 1 &&
            require_production_invocation(prepared)
                    .work_items.size() == 1,
        "Same-PackageBase targets did not compact to one work item");

    const ProductionSourceBuildWorkItem& work_item =
        require_production_invocation(prepared).work_items.front();
    expect(
        work_item.request.package_name.empty() &&
            work_item.request.checkout_name == "split-suite" &&
            work_item.required_targets.size() == 2 &&
            work_item.required_targets[0].package_name ==
                "split-runtime" &&
            work_item.required_targets[0].desired_reason ==
                DesiredInstallReason::Dependency &&
            work_item.required_targets[1].package_name ==
                "split-cli" &&
            work_item.required_targets[1].desired_reason ==
                DesiredInstallReason::Explicit &&
            preparation_stub::strict_preference_read_history() ==
                std::vector<std::string>{"split-suite"},
        "Same-PackageBase work-item identity, reason, or preference differs");

    execution_stub::enqueue_success(
        expected_execution_at(prepared, 0, config),
        "split-suite",
        {
            PackageBaseSourceBuildSelectedResult{
                ArtifactPackageIdentity{
                    "split-runtime", "2.0-1"},
                DesiredInstallReason::Dependency,
                ArtifactInstallExecutionOutcome::Installed},
            PackageBaseSourceBuildSelectedResult{
                ArtifactPackageIdentity{
                    "split-cli", "2.0-1"},
                DesiredInstallReason::Explicit,
                ArtifactInstallExecutionOutcome::SkippedAsNeeded},
        },
        {ArtifactPackageIdentity{"split-debug", "2.0-1"}});

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(
        result.is_success() && result.changed_package_state() &&
            !result.has_partial_completion(),
        "Same-PackageBase mixed child outcome aggregate differs");
    expect_statuses(
        result,
        {
            AurUpdateOperationTargetStatus::Updated,
            AurUpdateOperationTargetStatus::NoChange,
        },
        "same-PackageBase mixed child outcomes");
    expect(
        result.execution.has_value() &&
            result.execution->work_item_results.size() == 1 &&
            result.execution->work_item_results.front()
                    .child_results.size() == 2 &&
            result.execution->work_item_results.front()
                    .unselected_artifacts.size() == 1 &&
            result.execution->work_item_results.front()
                    .unselected_artifacts.front()
                    .package_name == "split-debug" &&
            result.execution->work_item_results.front()
                    .unselected_artifacts.front()
                    .full_version == "2.0-1" &&
            result.selected_target_results[0]
                    .operation_result
                    .execution_contributions.size() == 1 &&
            result.selected_target_results[1]
                    .operation_result
                    .execution_contributions.size() == 1,
        "Same-PackageBase child or unselected snapshot was not retained exactly");

    const std::vector<execution_stub::ExecutionCall>& calls =
        execution_stub::call_history();
    expect(
        calls.size() == 1 && calls.front().package_name.empty() &&
            calls.front().package_base == "split-suite" &&
            calls.front().plan_package_names ==
                std::vector<std::string>{
                    "split-runtime", "split-cli"} &&
            calls.front().events ==
                std::vector<execution_stub::EventKind>{
                    execution_stub::EventKind::Checkout,
                    execution_stub::EventKind::Build,
                    execution_stub::EventKind::Install,
                    execution_stub::EventKind::Cleanup},
        "Same-PackageBase set owner call count or lifecycle differs");
    execution_stub::require_script_consumed();
}

void test_ordinary_failure_partial_completion_and_not_attempted() {
    reset_stubs();
    return_build_plan(
        root_plan({{"first-success", "first-success"},
                   {"middle-failure", "middle-failure"},
                   {"last-pending", "last-pending"}}),
        {"first-success", "middle-failure", "last-pending"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("first-success"),
                          update_entry("middle-failure"),
                          update_entry("last-pending")}),
            NoExplicitSourceSatisfaction{}, config);
    enqueue_installed(prepared, config, 1);
    execution_stub::enqueue_phase_failure(
        expected_execution_at(prepared, 1, config),
        SeparatedPackageBaseSourceBuildFailurePhase::Build,
        "fixture build failure");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(!result.is_success() && result.changed_package_state() &&
               result.has_partial_completion() &&
               result.has_not_attempted_targets(),
           "Ordinary failure helper semantics differ");
    expect(result.reduced_operation_result.status ==
               AurUpdateOperationStatus::StoppedOnWorkItemFailure,
           "Ordinary failure operation status differs");
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::Failed,
         AurUpdateOperationTargetStatus::NotAttempted},
        "ordinary failure");
    expect(execution_stub::call_history().size() == 2,
           "Ordinary failure did not fail fast");
    execution_stub::require_script_consumed();
}

void test_cancellation_partial_completion_and_not_attempted() {
    reset_stubs();
    return_build_plan(
        root_plan({{"first-success", "first-success"},
                   {"middle-failure", "middle-failure"},
                   {"last-pending", "last-pending"}}),
        {"first-success", "middle-failure", "last-pending"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("first-success"),
                          update_entry("middle-failure"),
                          update_entry("last-pending")}),
            NoExplicitSourceSatisfaction{}, config);
    enqueue_installed(prepared, config, 1);
    execution_stub::enqueue_confirmation_stop(
        expected_execution_at(prepared, 1, config),
        ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput});

    std::optional<FilteredAurUpdateExecutionResult> partial;
    try {
        static_cast<void>(execute_prepared_filtered_aur_update_operation(std::move(prepared), config));
        throw std::runtime_error("Filtered cancellation returned ordinary result");
    } catch(FilteredAurUpdateCancelled& stop) {
        partial.emplace(std::move(stop).release_result());
    }
    const auto& result = *partial;
    expect(result.issues.empty() && result.reduced_operation_result.reduction_issues.empty() && result.selected_target_results.size() == 3,
           "Partial cancellation lost filtered correlations");
    expect(result.selected_target_results[1].operation_result.cancellation == ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput},
           "Filtered target lost EOF reason");
    expect(!result.is_success() && result.changed_package_state() &&
               result.has_partial_completion() &&
               result.has_not_attempted_targets(),
           "Cancellation helper semantics differ");
    expect(result.reduced_operation_result.status ==
               AurUpdateOperationStatus::StoppedOnWorkItemCancellation,
           "Cancellation operation status differs");
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::Updated,
         AurUpdateOperationTargetStatus::Cancelled,
         AurUpdateOperationTargetStatus::NotAttempted},
        "cancellation");
    expect(execution_stub::call_history().size() == 2,
           "Cancellation did not fail fast");
    execution_stub::require_script_consumed();
}

void test_cleanup_failure_partial_completion_and_not_attempted() {
    reset_stubs();
    return_build_plan(
        root_plan({{"cleanup-failure", "cleanup-failure"},
                   {"cleanup-pending", "cleanup-pending"}}),
        {"cleanup-failure", "cleanup-pending"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("cleanup-failure"),
                          update_entry("cleanup-pending")}),
            NoExplicitSourceSatisfaction{}, config);
    const std::string package_base = require_production_invocation(prepared)
                                         .work_items.front()
                                         .request.checkout_name;
    execution_stub::enqueue_cleanup_failure(
        expected_execution_at(prepared, 0, config),
        package_base,
        selected_children_at(
            prepared,
            0,
            ArtifactInstallExecutionOutcome::Installed),
        {},
        "fixture cleanup failure");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(!result.is_success() && result.changed_package_state() &&
               result.has_partial_completion() &&
               result.has_cleanup_failure() &&
               result.has_not_attempted_targets(),
           "Cleanup failure helper semantics differ");
    expect(result.reduced_operation_result.status ==
               AurUpdateOperationStatus::
                   StoppedAfterPackageCleanupFailure,
           "Cleanup failure operation status differs");
    expect_statuses(
        result,
        {AurUpdateOperationTargetStatus::UpdatedCleanupFailed,
         AurUpdateOperationTargetStatus::NotAttempted},
        "cleanup failure");
    expect(execution_stub::call_history().size() == 1,
           "Cleanup failure did not stop subsequent work items");
    execution_stub::require_script_consumed();
}

void test_preflight_invocation_index_out_of_range() {
    reset_stubs();
    BuildPlan plan = root_plan({{"range-root", "range-root"}});
    plan.root_targets[0].invocation_index = 9;
    plan.package_targets[0].roots[0] = plan.root_targets[0];
    return_build_plan(std::move(plan), {"range-root"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("range-root")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked(),
           "Out-of-range preflight invocation unexpectedly prepared");
    expect(has_operation_issue(
               prepared.operation_issues(),
               FilteredAurUpdateOperationIssueKind::
                   PreflightInvocationIndexOutOfRange),
           "Out-of-range invocation index was not typed during preparation");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(has_operation_issue(
               result.issues,
               FilteredAurUpdateOperationIssueKind::
                   PreflightInvocationIndexOutOfRange),
           "Out-of-range invocation index was not typed");
    expect(result.has_planning_issue() && !result.execution.has_value(),
           "Out-of-range invocation index did not block execution");
    expect_no_mutation("out-of-range invocation index");
}

void test_preflight_invocation_identity_mismatch() {
    reset_stubs();
    BuildPlan plan = root_plan({{"identity-a", "identity-a"},
                                {"identity-b", "identity-b"}});
    plan.root_targets[0].invocation_index = 1;
    plan.package_targets[0].roots[0] = plan.root_targets[0];
    return_build_plan(
        std::move(plan), {"identity-a", "identity-b"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("identity-a"),
                          update_entry("identity-b")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked(),
           "Mismatched preflight invocation unexpectedly prepared");
    expect(has_operation_issue(
               prepared.operation_issues(),
               FilteredAurUpdateOperationIssueKind::
                   PreflightInvocationIdentityMismatch),
           "Invocation identity mismatch was not typed during preparation");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(has_operation_issue(
               result.issues,
               FilteredAurUpdateOperationIssueKind::
                   PreflightInvocationIdentityMismatch),
           "Invocation identity mismatch was not typed");
    expect(result.has_planning_issue() && !result.execution.has_value(),
           "Invocation identity mismatch did not block execution");
    expect_no_mutation("invocation identity mismatch");
}

void test_build_unit_order_identity_mismatch_blocks_mutation() {
    reset_stubs();
    BuildPlan plan = root_plan({{"correlation-root", "correlation-root"}});
    plan.order[0].package_names = {"wrong-package"};
    return_build_plan(std::move(plan), {"correlation-root"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("correlation-root")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_blocked(),
           "Build-unit order identity mismatch unexpectedly prepared");
    expect(has_operation_issue(
               prepared.operation_issues(),
               FilteredAurUpdateOperationIssueKind::
                   BuildUnitOrderIdentityMismatch),
           "Build-unit order identity mismatch was not typed");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(has_operation_issue(
               result.issues,
               FilteredAurUpdateOperationIssueKind::
                   BuildUnitOrderIdentityMismatch),
           "Build-unit identity mismatch was not typed");
    expect(!result.execution.has_value() && !result.is_success(),
           "Build-unit identity mismatch did not block execution");
    expect_no_mutation("build-unit correlation mismatch");
}

void test_projection_payload_private_snapshot_drift_blocks_mutation() {
    reset_stubs();
    return_build_plan(
        root_plan({{"payload-root", "payload-root"}}),
        {"payload-root"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("payload-root")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_prepared(), "Projection drift fixture did not prepare");

    BuildPlanArtifactTargetProjectionIssue projection_issue{
        BuildPlanArtifactTargetProjectionIssueKind::
            MissingPlannedPackageTarget,
        std::size_t{0},
        std::size_t{0},
        {0},
        std::string{"payload-root"},
        std::string{"payload-root"},
        {RootTargetIdentity{0, "payload-root"}},
        "Typed projection payload."};
    AurUpdateExecutionIssue issue{
        AurUpdateExecutionReason::BuildPlanInconsistent,
        std::string{"payload-root"},
        std::string{"payload-root"},
        std::nullopt,
        "Projection snapshot issue.",
        projection_issue};

    // Public observerはconstのまま保ち、testだけがowned private snapshotを
    // 意図的に破損してpre-execution firewallを確認する。
    AurUpdateExecutionPreflight& owned_preflight =
        const_cast<AurUpdateExecutionPreflight&>(
            prepared.execution_preflight());
    AurUpdateSourceBuildPreparation& owned_preparation =
        const_cast<AurUpdateSourceBuildPreparation&>(
            *prepared.source_build_preparation());
    owned_preflight.targets.front().issues.push_back(issue);
    owned_preparation.affected_update_targets.front().issues.push_back(
        std::move(issue));
    owned_preparation.affected_update_targets.front()
        .issues.back()
        .build_plan_projection_issue->package_target_indices = {1};

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);

    expect(
        has_operation_issue(
            result.issues,
            FilteredAurUpdateOperationIssueKind::
                PreflightTargetMappingInconsistent),
        "Projection-only private snapshot drift was not typed");
    expect(
        !result.execution.has_value() && result.has_planning_issue(),
        "Projection-only private snapshot drift reached execution");
    expect_no_mutation("projection payload private snapshot drift");
}

void test_skip_kind_snapshot_drift_blocks_before_execution() {
    reset_stubs();
    return_build_plan(
        root_plan({{"skip-drift-update", "skip-drift-update"}}),
        {"skip-drift-update"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({
                update_entry("skip-drift-update"),
                current_entry("skip-drift-current"),
            }),
            NoExplicitSourceSatisfaction{}, config);
    expect(
        prepared.is_prepared(),
        "Skip-kind drift fixture did not prepare");

    AurUpdateExecutionPreflight& owned_preflight =
        const_cast<AurUpdateExecutionPreflight&>(
            prepared.execution_preflight());
    owned_preflight.targets[1].skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(
        !result.execution.has_value() && !result.is_success() &&
            result.reduced_operation_result.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            !result.reduced_operation_result.reduction_issues.empty(),
        "Drifted skip kind reached execution or became success");
    expect_no_mutation("skip-kind snapshot drift");
}

void test_hidden_required_devel_payload_drift_blocks_before_execution() {
    reset_stubs();
    return_build_plan(
        root_plan({{"hidden-required-update", "hidden-required-update"}}),
        {"hidden-required-update"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("hidden-required-update")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(
        prepared.is_prepared(),
        "Hidden required-devel payload drift fixture did not prepare");

    AurUpdateExecutionPreflight& owned_preflight =
        const_cast<AurUpdateExecutionPreflight&>(
            prepared.execution_preflight());
    AurUpdateExecutionIssue hidden_issue;
    hidden_issue.required_devel_target_blocker =
        AurUpdateRequiredDevelTargetBlocker{
            AurUpdateRequiredDevelTargetRelation::AurExactDependency,
            1,
            "required-devel-git",
            DevelRequiresCheckReason::SuffixCandidateOnly};
    owned_preflight.targets.front().issues.push_back(
        std::move(hidden_issue));
    expect(
        !prepared.is_prepared() && prepared.is_blocked(),
        "Hidden required-devel payload drift remained executable");

    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(
        !result.execution.has_value() && !result.is_success() &&
            result.reduced_operation_result.status ==
                AurUpdateOperationStatus::InconsistentResult &&
            !result.reduced_operation_result.reduction_issues.empty(),
        "Hidden required-devel payload drift reached execution or became success");
    expect_no_mutation("hidden required-devel payload drift");
}

void test_prepared_operation_replay_is_rejected() {
    reset_stubs();
    return_build_plan(
        root_plan({{"one-shot-root", "one-shot-root"}}),
        {"one-shot-root"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("one-shot-root")}),
            NoExplicitSourceSatisfaction{}, config);
    enqueue_installed(prepared, config, 1);
    FilteredAurUpdateExecutionResult first =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);
    expect(first.is_success(), "First prepared operation execution failed");
    execution_stub::require_script_consumed();

    execution_stub::reset();
    expect_exception<std::logic_error>(
        [&prepared, &config] {
            static_cast<void>(
                execute_prepared_filtered_aur_update_operation(
                    std::move(prepared), config));
        },
        "prepared filtered operation replay");
    expect_no_mutation("prepared operation replay");
}

void test_reducer_inconsistency_is_retained() {
    reset_stubs();
    return_build_plan(
        root_plan({{"reducer-root", "reducer-root"}}),
        {"reducer-root"});
    const AppConfig config;
    PreparedFilteredAurUpdateOperation prepared =
        prepare_strict_filtered_aur_update_operation(
            query_result({update_entry("reducer-root")}),
            NoExplicitSourceSatisfaction{}, config);
    expect(prepared.is_prepared(), "Reducer fixture did not prepare");

    enqueue_installed(prepared, config, 1);
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(
            std::move(prepared), config);

    // reducer入力用のpublic owned result snapshotだけを壊し、actual runnerの
    // known outcomeとreduction issueがどちらも残ることを固定する。
    result.preflight.targets[0].update_plan_index = 8;
    AurUpdateOperationResult inconsistent =
        reduce_aur_update_operation_result(
            result.preflight, result.preparation,
            DevelRequiresCheckPolicy::BlockOperation,
            result.execution);
    expect(result.execution.has_value() &&
               !inconsistent.reduction_issues.empty() &&
               inconsistent.status ==
                   AurUpdateOperationStatus::InconsistentResult &&
               !inconsistent.is_success(),
           "Reducer inconsistency was not retained with known execution");
    expect(inconsistent.execution_work_items.size() == 1 &&
               inconsistent.execution_work_items[0]
                       .status ==
                   AurUpdateWorkItemExecutionStatus::Updated,
           "Reducer inconsistency lost known execution outcome");
    execution_stub::require_script_consumed();
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        test_cancellation_partial_completion_and_not_attempted();
        test_system_aur_cancellation_preserves_inner_partial();
        run_case(
            "system+AUR dry-run Auto current update observation",
            test_system_aur_dry_run_auto_observes_current_update_without_capability);
        run_case(
            "system+AUR dry-run Auto current no updates",
            test_system_aur_dry_run_auto_current_no_updates_is_not_blocked);
        run_case(
            "system+AUR dry-run RepoOnly zero AUR authority",
            test_system_aur_dry_run_repo_only_observes_repository_intent_only);
        run_case(
            "system+AUR dry-run independent RequiresCheck attention",
            test_system_aur_dry_run_independent_requires_check_is_attention);
        run_case(
            "system+AUR dry-run ambiguous provider",
            test_system_aur_dry_run_ambiguous_provider_is_not_auto_selected);
        run_case(
            "system+AUR dry-run fatal query failure",
            test_system_aur_dry_run_fatal_query_failure_is_typed_and_blocks);
        run_case(
            "system+AUR dry-run identity mismatch",
            test_system_aur_dry_run_identity_mismatch_blocks_before_mutation);
        run_case(
            "system+AUR dry-run preparation blocker",
            test_system_aur_dry_run_preparation_blocker_stops_before_mutation);
        run_case(
            "system+AUR dry world is not reused by actual world",
            test_system_aur_dry_run_world_is_not_reused_by_actual_world);
        run_case(
            "system+AUR repository failure stops later authority",
            test_system_aur_repository_failure_stops_all_later_authority);
        run_case(
            "system+AUR no updates is successful but not a false no-op",
            test_system_aur_success_without_aur_updates_is_not_false_noop);
        run_case(
            "system+AUR inventory failure is partial",
            test_system_aur_inventory_failure_is_partial);
        run_case(
            "system+AUR query failure is retained",
            test_system_aur_query_failure_is_retained);
        run_case(
            "system+AUR fatal query failure",
            test_system_aur_fatal_query_failure_stops_before_preflight);
        run_case(
            "system+AUR independent RequiresCheck completes with attention",
            test_system_aur_independent_requires_check_completes_with_attention);
        run_case(
            "system+AUR update plus independent RequiresCheck",
            test_system_aur_update_and_independent_requires_check_succeed);
        run_case(
            "system+AUR required RequiresCheck blocks actual and dry-run",
            test_system_aur_required_requires_check_blocks_actual_and_dry_run);
        run_case(
            "system+AUR ambiguous provider is not auto-selected",
            test_system_aur_ambiguous_provider_is_not_auto_selected);
        run_case(
            "system+AUR preparation failure is partial",
            test_system_aur_preparation_failure_is_partial);
        run_case(
            "system+AUR unexpected preparation exception phase",
            test_system_aur_unexpected_preparation_exception_keeps_phase);
        run_case(
            "system+AUR unexpected execution exception phase",
            test_system_aur_unexpected_execution_exception_keeps_phase);
        run_case(
            "system+AUR post-repository freshness and Ignore",
            test_system_aur_success_uses_only_post_repository_inventory_and_ignore);
        run_case(
            "system+AUR inner work-item partial is retained",
            test_system_aur_work_item_failure_preserves_inner_partial);
        run_case(
            "system+AUR cleanup phase is retained",
            test_system_aur_cleanup_failure_preserves_child_phase);
        run_case(
            "system+AUR malformed child fails closed",
            test_system_aur_reducer_fails_closed_on_malformed_child);
        run_case(
            "system+AUR RequiresCheck policy mismatch fails closed",
            test_system_aur_reducer_rejects_policy_mismatch);
        run_case(
            "system+AUR exact query snapshot correlation",
            test_system_aur_reducer_correlates_exact_query_snapshot);
        run_case(
            "system+AUR inventory/query correlation",
            test_system_aur_reducer_correlates_inventory_and_query);
        run_case(
            "system+AUR compatible request correlation",
            test_system_aur_reducer_correlates_compatible_request);
        run_case(
            "system+AUR contradictory NotAttempted reason",
            test_system_aur_reducer_preserves_contradictory_not_attempted_reason);
        run_case(
            "system+AUR contradictory child phase status",
            test_system_aur_reducer_rejects_contradictory_child_phase_status);
        run_case(
            "system+AUR unknown query phase status",
            test_system_aur_reducer_rejects_unknown_query_phase_status);
        run_case("empty query plan", test_empty_query_plan_is_normal_noop);
        run_case(
            "real query wrapper and no-explicit legacy equivalence",
            test_real_query_wrapper_and_no_explicit_satisfaction_match_legacy_path);
        run_case(
            "Ignore filtered preparation has zero preference IO",
            test_ignore_policy_reaches_filtered_preparation_without_preference_io);
        run_case(
            "explicit inventory query core",
            test_explicit_inventory_query_core_bypasses_inventory_wrapper);
        run_case(
            "package-name target exclusion",
            test_package_name_target_exclusion);
        run_case(
            "PackageBase target exclusion",
            test_package_base_target_exclusion);
        run_case(
            "split-package target exclusion",
            test_split_package_targets_share_exclusion_attribution);
        run_case(
            "original, filtered, and preflight index mapping",
            test_original_filtered_and_preflight_index_mapping);
        run_case(
            "RequiresCheck policy-skip adapter and full retention",
            test_requires_check_policy_skip_adapter_and_full_retention);
        run_case(
            "RequiresCheck skip observation foundation",
            test_requires_check_skip_observation_foundation);
        run_case(
            "transitive external satisfaction",
            test_transitive_external_satisfaction_keeps_selected_root_executable);
        run_case(
            "multiple-child external satisfaction",
            test_multiple_child_external_satisfaction_keeps_set_snapshot);
        run_case(
            "shared external unit",
            test_one_external_unit_shared_by_multiple_roots);
        run_case(
            "query recoverable failure",
            test_query_recoverable_failure_is_retained_and_blocks_mutation);
        run_case(
            "RequiresCheck mixed operation no mutation",
            test_requires_check_blocks_mixed_operation_before_mutation);
        run_case(
            "planner issue no mutation",
            test_planner_issue_blocks_mutation_but_keeps_disposition);
        run_case(
            "initial preflight identity blocker",
            test_initial_preflight_identity_blocker_stops_before_mutation);
        run_case(
            "preflight blocker no mutation",
            test_preflight_blocker_stops_before_mutation);
        run_case(
            "preparation blocker no mutation",
            test_preparation_blocker_stops_before_mutation);
        run_case("all Updated", test_all_updated);
        run_case("all NoChange", test_all_no_change);
        run_case(
            "same PackageBase mixed child outcomes",
            test_same_package_base_children_execute_once_with_mixed_outcomes);
        run_case(
            "ordinary failure, partial completion, and NotAttempted",
            test_ordinary_failure_partial_completion_and_not_attempted);
        run_case(
            "cleanup failure, partial completion, and NotAttempted",
            test_cleanup_failure_partial_completion_and_not_attempted);
        run_case(
            "preflight invocation index out of range",
            test_preflight_invocation_index_out_of_range);
        run_case(
            "preflight invocation identity mismatch",
            test_preflight_invocation_identity_mismatch);
        run_case(
            "build-unit order identity mismatch",
            test_build_unit_order_identity_mismatch_blocks_mutation);
        run_case(
            "projection payload private snapshot drift",
            test_projection_payload_private_snapshot_drift_blocks_mutation);
        run_case(
            "skip-kind snapshot drift blocks before execution",
            test_skip_kind_snapshot_drift_blocks_before_execution);
        run_case(
            "hidden required-devel payload drift blocks before execution",
            test_hidden_required_devel_payload_drift_blocks_before_execution);
        run_case(
            "prepared operation replay",
            test_prepared_operation_replay_is_rejected);
        run_case(
            "reducer inconsistency",
            test_reducer_inconsistency_is_retained);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "Filtered AUR update operation tests: all checks passed\n";
    return 0;
}
