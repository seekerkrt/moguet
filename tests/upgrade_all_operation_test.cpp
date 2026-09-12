#include "app_config.hpp"
#include "artifact_install_executor.hpp"
#include "operation_state_model.hpp"
#include "presentation_projection.hpp"
#include "source_build.hpp"
#include "stubs/upgrade-all-operation/operation_stub.hpp"
#include "unified_plan_projection.hpp"
#include "upgrade_all_operation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using UpgradeAllOperationExecutor = UpgradeAllOperationResult (*)(
    PreparedUpgradeAllOperation,
    const AppConfig&);

static_assert(!std::is_default_constructible_v<PreparedUpgradeAllOperation>);
static_assert(!std::is_copy_constructible_v<PreparedUpgradeAllOperation>);
static_assert(std::is_nothrow_move_constructible_v<
              PreparedUpgradeAllOperation>);
static_assert(!std::is_move_assignable_v<PreparedUpgradeAllOperation>);
static_assert(!std::is_copy_constructible_v<
              UpgradeAllOperationProjectionAuthority>);
static_assert(!std::is_copy_constructible_v<
              PreparedUpgradeAllAurPreflight>);
static_assert(std::is_move_constructible_v<
              PreparedUpgradeAllAurPreflight>);
static_assert(std::is_same_v<
              decltype(&execute_prepared_upgrade_all_operation),
              UpgradeAllOperationExecutor>);
static_assert(std::is_invocable_v<
              UpgradeAllOperationExecutor,
              PreparedUpgradeAllOperation&&,
              const AppConfig&>);
static_assert(!std::is_invocable_v<
              UpgradeAllOperationExecutor,
              PreparedUpgradeAllOperation&,
              const AppConfig&>);

namespace {

namespace fs = std::filesystem;
namespace stub = upgrade_all_operation_test_stub;

constexpr int CORRELATION_SYSTEM_FAILURE_EXIT_STATUS = 37;

struct UnknownCorrelationObservationFailure {};

class TemporaryCacheEnvironment final {
    fs::path root_;

public:
    TemporaryCacheEnvironment() {
        std::string path_template =
            "/tmp/moguet-upgrade-all-cache-test-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::runtime_error(
                "Failed to create upgrade-all cache test directory.");
        }
        root_ = created;
        if(setenv("XDG_CACHE_HOME", root_.c_str(), 1) != 0) {
            throw std::runtime_error(
                "Failed to set upgrade-all cache test environment.");
        }
    }

    TemporaryCacheEnvironment(const TemporaryCacheEnvironment&) = delete;
    TemporaryCacheEnvironment& operator=(
        const TemporaryCacheEnvironment&) = delete;

    ~TemporaryCacheEnvironment() noexcept {
        std::error_code cleanup_error;
        fs::remove_all(root_, cleanup_error);
    }
};

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

fs::path move_active_cache_root(const std::string& fixture_name) {
    const char* raw_cache_home = std::getenv("XDG_CACHE_HOME");
    expect(raw_cache_home != nullptr, "XDG cache test environment is unset");
    const fs::path active = fs::path(raw_cache_home) / "moguet";
    const fs::path moved =
        fs::path(raw_cache_home) / ("moguet-revoked-" + fixture_name);
    expect(fs::is_directory(active), "Active Moguet cache root is missing");
    std::error_code rename_error;
    fs::rename(active, moved, rename_error);
    expect(!rename_error, "Failed to revoke Moguet cache root fixture");
    return moved;
}

void remove_cache_fixture(const fs::path& path) noexcept {
    std::error_code cleanup_error;
    fs::remove_all(path, cleanup_error);
}

AppConfig full_option_config() {
    AppConfig config;
    config.user_config.review.pkgbuild = ReviewPolicy::Skip;
    config.user_config.review.diff = ReviewPolicy::Skip;
    config.user_config.build.mode = BuildMode::Clean;
    config.no_confirm = true;
    config.rm_deps = true;
    config.editor = "upgrade-all-test-editor";
    return config;
}

SourcePreferenceDirectorySnapshot preference_directory(
    const std::vector<std::string>& package_names,
    bool root_exists = true) {
    SourcePreferenceDirectorySnapshot snapshot;
    snapshot.root_exists = root_exists;
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        snapshot.entries.push_back(SourcePreferenceEntrySnapshot{
            index,
            fs::path("/preferences") / package_names[index],
            package_names[index],
            true});
    }
    return snapshot;
}

SourcePreferenceLoaded loaded_preference(
    const std::string& package_name,
    std::vector<std::string> warnings = {}) {
    SourceBuildEnvironment environment;
    environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"PACKAGE_NAME", package_name});
    return SourcePreferenceLoaded{
        .entry_path = fs::path("/preferences") / package_name,
        .environment = std::move(environment),
        .warnings = std::move(warnings),
        .raw_contents = {},
        .identity = std::nullopt,
    };
}

SourcePreferenceFailure preference_failure(
    const std::string& package_name,
    const std::string& diagnostic) {
    return SourcePreferenceFailure{
        SourcePreferenceFailureKind::ReadFailed,
        fs::path("/preferences") / package_name,
        std::make_error_code(std::errc::io_error),
        std::nullopt,
        diagnostic};
}

InstalledPackageMetadata installed_package(
    const std::string& package_name,
    const std::string& version = "1.0-1",
    InstalledPackageReason reason = InstalledPackageReason::Explicit) {
    return InstalledPackageMetadata{package_name, version, reason};
}

stub::MetadataSessionScript metadata_session(
    const std::vector<std::string>& package_names,
    LocalPackageVersionSnapshotResult local_snapshot,
    const std::string& installed_version = "1.0-1") {
    stub::MetadataSessionScript script;
    script.local_package_snapshot = std::move(local_snapshot);
    for(const std::string& package_name : package_names) {
        script.installed_package_results.emplace(
            package_name,
            installed_package(package_name, installed_version));
    }
    return script;
}

SourceBuildExecutionResult source_execution(
    SourceBuildExecutionStatus status,
    std::string diagnostic = {}) {
    SourceBuildExecutionResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    if(status == SourceBuildExecutionStatus::UpdateStatusUnknownSkipped) {
        result.update_status_unknown_skip_reason =
            SourceBuildUpdateStatusUnknownSkipReason::NoConfirm;
    }
    return result;
}

ResolvedSourceBuildIdentity source_identity(
    const std::string& requested_name,
    const std::string& package_base) {
    return ResolvedSourceBuildIdentity{
        ResolvedRepositorySourceBuildIdentity{
            RepositoryPackagePresent{
                "core", 0, requested_name, package_base}}};
}

void configure_sources(
    const std::vector<std::string>& package_names,
    LocalPackageVersionSnapshot before_snapshot =
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}) {
    stub::set_preference_directory(preference_directory(package_names));
    for(const std::string& package_name : package_names) {
        stub::enqueue_preference_result(
            package_name, loaded_preference(package_name));
    }
    if(!package_names.empty()) {
        stub::enqueue_metadata_session(metadata_session(
            package_names, std::move(before_snapshot)));
    }
}

void enqueue_post_source_metadata(
    const std::vector<std::string>& package_names,
    LocalPackageVersionSnapshot after_snapshot =
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}) {
    stub::enqueue_metadata_session(metadata_session(
        package_names, std::move(after_snapshot)));
}

PreparedUpgradeAllOperation take_prepared(
    UpgradeAllOperationPreparation preparation,
    const std::string& context) {
    if(!std::holds_alternative<PreparedUpgradeAllOperation>(preparation)) {
        const UpgradeAllOperationResult& blocked =
            std::get<UpgradeAllOperationResult>(preparation);
        std::string detail;
        if(!blocked.system_source.issues.empty()) {
            detail = ": " +
                     blocked.system_source.issues.front().diagnostic;
        } else if(!blocked.issues.empty()) {
            detail = ": " + blocked.issues.front().diagnostic;
        }
        throw std::runtime_error(
            context + ": preparation unexpectedly blocked" + detail);
    }
    return std::move(std::get<PreparedUpgradeAllOperation>(preparation));
}

UpgradeAllOperationResult take_blocked(
    UpgradeAllOperationPreparation preparation,
    const std::string& context) {
    expect(
        std::holds_alternative<UpgradeAllOperationResult>(preparation),
        context + ": preparation unexpectedly returned a capability");
    return std::move(std::get<UpgradeAllOperationResult>(preparation));
}

PreparedUpgradeAllOperation prepare_sources(
    const std::vector<std::string>& package_names,
    const AppConfig& config,
    LocalPackageVersionSnapshot before_snapshot =
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}) {
    configure_sources(package_names, std::move(before_snapshot));
    return take_prepared(
        prepare_upgrade_all_operation(config), "registered sources");
}

AurPackageInfo package_info(
    const std::string& package_name,
    const std::string& package_base = {},
    const std::string& version = "2.0-1") {
    AurPackageInfo info;
    info.Name = package_name;
    info.PackageBase = package_base.empty() ? package_name : package_base;
    info.Version = version;
    info.Description = "upgrade-all operation fixture";
    info.Maintainer = "moguet-test";
    return info;
}

ForeignPackageInventory foreign_inventory(
    const std::vector<std::string>& package_names,
    const std::string& version = "1.0-1") {
    ForeignPackageInventory inventory;
    for(const std::string& package_name : package_names) {
        inventory.push_back(installed_package(package_name, version));
    }
    return inventory;
}

PackageRelationInstalledDatabaseIdentity
cross_source_observation_database_identity() {
    return PackageRelationInstalledDatabaseIdentity{
        fs::path("/upgrade-all-stub/root"),
        fs::path("/upgrade-all-stub/database")};
}

PackageRelationObservedPackage observed_installed_package(
    std::string package_name,
    std::string version) {
    return PackageRelationObservedPackage{
        std::move(package_name),
        std::nullopt,
        ObservedVersion::available(
            ObservedVersionSource::InstalledExactPackage,
            std::move(version)),
        {},
        cross_source_observation_database_identity(),
        PackageRelationObservationRole::Installed,
        {}};
}

DependencyRequirement parsed_dependency_requirement(
    const std::string& specification) {
    const DependencyRequirementParseResult parsed =
        parse_dependency_requirement(specification);
    expect(
        parsed.failure() == nullptr,
        "Cross-source fixture dependency parse failed: " +
            specification);
    const DependencyRequirement* requirement = parsed.requirement();
    expect(
        requirement != nullptr,
        "Cross-source fixture dependency requirement is missing");
    return *requirement;
}

AurPackageInfo virtualbox_replacement_package() {
    AurPackageInfo package = package_info(
        "virtualbox-ext-oracle",
        "virtualbox-ext-oracle",
        "7.2.16-1");
    package.constraint_metadata = AurPackageConstraintMetadata{
        "virtualbox-ext-oracle",
        "virtualbox-ext-oracle",
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage,
            "7.2.16-1"),
        {parsed_dependency_requirement("virtualbox=7.2.16")},
        {},
        {},
        {},
        {}};
    return package;
}

void arrange_virtualbox_cross_source_observation() {
    stub::set_foreign_inventory(foreign_inventory(
        {"virtualbox-ext-oracle"}, "7.2.14-1"));
    stub::set_installed_relation_inventory(
        InstalledPackageRelationInventory{
            cross_source_observation_database_identity(),
            {observed_installed_package(
                 "virtualbox", "7.2.14-1"),
             observed_installed_package(
                 "virtualbox-ext-oracle", "7.2.14-1")}});
    stub::set_installed_runtime_dependency_inventory(
        InstalledPackageRuntimeDependencyMetadataInventory{
            InstalledPackageRuntimeDependencyMetadata{
                "virtualbox-ext-oracle",
                {"virtualbox=7.2.14"}}});
    stub::set_repository_candidate_result(
        "virtualbox",
        RepositoryPackagePresent{
            "upgrade-all-stub-repository",
            0,
            "virtualbox",
            "virtualbox",
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                "7.2.16-1"),
            std::vector<std::string>{
                "upgrade-all-stub-repository"},
            {}});
}

void enqueue_aur_query(
    const std::vector<std::pair<std::string, std::string>>& packages,
    const std::string& comparison = "1") {
    std::map<std::string, AurPackageInfo> response;
    for(const auto& [package_name, package_base] : packages) {
        response.emplace(
            package_name,
            package_info(package_name, package_base));
    }
    stub::enqueue_info_many_result(std::move(response));
    for(std::size_t index = 0; index < packages.size(); ++index) {
        stub::enqueue_vercmp_result(comparison);
    }
}

struct RootSpec {
    std::string package_name;
    std::string package_base;
};

template <typename Value>
void append_unique(std::vector<Value>& values, const Value& value) {
    if(std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

PlannedPackageTarget* find_package_target(
    BuildPlan& plan,
    const std::string& package_name) {
    auto found = std::find_if(
        plan.package_targets.begin(), plan.package_targets.end(),
        [&package_name](const PlannedPackageTarget& target) {
            return target.package_name == package_name;
        });
    return found == plan.package_targets.end() ? nullptr : &*found;
}

const PlannedPackageTarget& require_package_target(
    const BuildPlan& plan,
    const std::string& package_name) {
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
            "BuildPlan dependency PackageBase differs");
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
    BuildPlan plan,
    std::vector<std::string> expected_targets) {
    stub::set_resolver_handler(
        [plan = std::move(plan),
         expected_targets = std::move(expected_targets)](
            const std::vector<std::string>& targets,
            const ProviderSelectionCallback&) {
            if(targets != expected_targets) {
                throw std::logic_error(
                    "Aggregate resolver target order differs");
            }
            return plan;
        });
}

bool has_issue(
    const UpgradeAllOperationResult& result,
    UpgradeAllOperationIssueKind kind) {
    return std::any_of(
        result.issues.begin(), result.issues.end(),
        [kind](const UpgradeAllOperationIssue& issue) {
            return issue.kind == kind;
        });
}

const AurUpdatePreparationIssue* find_aur_preparation_issue(
    const UpgradeAllOperationResult& result,
    AurUpdatePreparationReason reason) {
    if(!result.aur.operation_result.has_value()) return nullptr;
    const auto& issues = result.aur.operation_result->preparation.issues;
    auto found = std::find_if(
        issues.begin(), issues.end(),
        [reason](const AurUpdatePreparationIssue& issue) {
            return issue.reason == reason;
        });
    return found == issues.end() ? nullptr : &*found;
}

std::size_t event_position(
    stub::EventKind kind,
    const std::string& subject,
    std::size_t occurrence = 0) {
    std::size_t seen = 0;
    const auto& events = stub::event_history();
    for(std::size_t index = 0; index < events.size(); ++index) {
        if(events[index].kind != kind || events[index].subject != subject) {
            continue;
        }
        if(seen == occurrence) return index;
        ++seen;
    }
    throw std::runtime_error(
        "Missing event " + subject + " occurrence " +
        std::to_string(occurrence));
}

void expect_aur_not_attempted(
    const UpgradeAllOperationResult& result,
    UpgradeAllNotAttemptedReason reason,
    const std::string& context) {
    expect(
        result.aur.status == UpgradeAllAurPhaseStatus::NotAttempted &&
            result.aur.not_attempted_reason == reason,
        context + ": AUR NotAttempted reason differs");
    expect(
        !result.aur.operation_result.has_value(),
        context + ": unattempted AUR phase gained a normal result");
}

void expect_no_inventory_or_aur(const std::string& context) {
    expect(
        stub::repository_configuration_calls() == 0 &&
            stub::inventory_calls() == 0 &&
            stub::installed_relation_inventory_calls() == 0 &&
            stub::installed_runtime_dependency_inventory_calls() ==
                0 &&
            stub::repository_candidate_call_history().empty(),
        context + ": foreign inventory boundary was crossed");
    expect(
        stub::info_many_call_history().empty() &&
            stub::info_strict_call_history().empty() &&
            stub::resolver_call_count() == 0 &&
            stub::aur_execution_calls().empty(),
        context + ": AUR boundary was crossed");
}

const UpgradeAllCrossSourceVersionLockCorrelationResult*
require_cross_source_correlation(
    const UpgradeAllOperationResult& result,
    const char* context) {
    expect(
        result.cross_source_version_lock_correlation.has_value(),
        std::string(context) +
            ": secondary correlation was not attempted");
    return &result.cross_source_version_lock_correlation.value();
}

void expect_system_failure_primary_preserved(
    const UpgradeAllOperationResult& result,
    const std::string& context) {
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedOnSystemFailure &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::System &&
            result.system_source.status ==
                SystemSourceUpgradeStatus::
                    StoppedOnSystemFailure &&
            result.system_source.stopped_phase ==
                SystemSourceUpgradePhase::System &&
            result.system_source.system.status ==
                SystemUpgradePhaseStatus::Failed &&
            result.system_source.system.package_state_change ==
                PackageStateChange::Unknown &&
            result.system_source.system.command_exit_status ==
                std::optional<int>{
                    CORRELATION_SYSTEM_FAILURE_EXIT_STATUS} &&
            result.system_source.system.diagnostic ==
                std::optional<std::string>{
                    "The update failed."} &&
            result.system_source.issues.empty() &&
            result.system_source.diagnostics.size() == 1 &&
            result.system_source.diagnostics.front().diagnostic ==
                "The update failed." &&
            result.has_not_attempted_phase() &&
            !result.has_partial_completion(),
        context + ": primary system failure changed");
    expect(
        result.system_source.registered_source_results.size() == 1 &&
            result.system_source.registered_source_results.front()
                    .status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.system_source.registered_source_results.front()
                    .failure_kind ==
                RegisteredSourceUpgradeFailureKind::
                    PriorPhaseStopped,
        context + ": later registered source changed");
    expect(
        result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::
                    NotAttempted &&
            result.foreign_inventory.not_attempted_reason ==
                UpgradeAllNotAttemptedReason::SystemFailure,
        context + ": foreign inventory NotAttempted reason changed");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::SystemFailure,
        context);
    expect(
        stub::system_commands().size() == 1 &&
            stub::source_execution_calls().empty() &&
            stub::info_many_call_history().empty() &&
            stub::resolver_call_count() == 0 &&
            stub::aur_execution_calls().empty(),
        context + ": later production execution was reached");
}

UpgradeAllOperationResult execute_system_failure_for_correlation() {
    const AppConfig config;
    PreparedUpgradeAllOperation prepared =
        prepare_sources({"source-root"}, config);
    stub::set_system_command_exit_status(
        CORRELATION_SYSTEM_FAILURE_EXIT_STATUS);
    return execute_prepared_upgrade_all_operation(
        std::move(prepared), config);
}

void test_empty_source_preparation_snapshot() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const AppConfig config = full_option_config();

    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config), "empty source");
    const UpgradeAllOperationPreparedSnapshot* snapshot = prepared.snapshot();
    expect(snapshot != nullptr, "Prepared aggregate snapshot is unavailable");
    expect(
        snapshot->system_source.preference_root_exists &&
            snapshot->system_source.registered_sources.empty() &&
            snapshot->explicit_source_adapter.entries.empty() &&
            snapshot->explicit_source_adapter.is_valid(),
        "Empty registered-source intent changed during preparation");
    expect(
        snapshot->system_source.options.no_edit ==
                (config.user_config.review.pkgbuild ==
                 ReviewPolicy::Skip) &&
            snapshot->system_source.options.rm_deps ==
                config.rm_deps &&
            snapshot->system_source.options.editor == config.editor,
        "Aggregate option fingerprint was not frozen");
    stub::require_script_consumed();
}

void test_prepared_projection_authority_tracks_nested_source_work() {
    stub::reset();
    const std::string package_name = "projection-source";
    PreparedUpgradeAllOperation prepared = prepare_sources(
        {package_name}, full_option_config());
    const UpgradeAllOperationPreparedSnapshot* snapshot = prepared.snapshot();
    const UpgradeAllOperationProjectionAuthority* projection_authority =
        prepared.projection_authority();
    expect(
        snapshot != nullptr && projection_authority != nullptr &&
            &projection_authority->snapshot() == snapshot,
        "Prepared upgrade-all projection authority lost outer owner");
    const SystemSourceUpgradeProjectionAuthority& system_source =
        projection_authority->system_source();
    expect(
        system_source.source_work_items().size() == 1 &&
            &system_source.source_work_items().front().source() ==
                &system_source.snapshot()
                     .registered_sources.front(),
        "Prepared upgrade-all authority lost nested source correlation");
    const auto& targets = system_source.source_work_items()
                              .front()
                              .required_targets();
    expect(
        targets.size() == 1 &&
            targets.front().package_base == package_name &&
            targets.front().package_name == package_name &&
            targets.front().desired_reason ==
                DesiredInstallReason::Explicit,
        "Prepared upgrade-all authority did not borrow nested actual work targets");
    stub::require_script_consumed();
}

void test_preparation_and_aur_preflight_do_not_activate_cache() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory({});
    const AppConfig config = full_option_config();
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "read-only upgrade-all preparation");
    const char* raw_cache_home = std::getenv("XDG_CACHE_HOME");
    expect(raw_cache_home != nullptr, "XDG cache test environment is unset");
    const fs::path cache_root = fs::path(raw_cache_home) / "moguet";
    expect(
        !fs::exists(cache_root),
        "Static upgrade-all preparation activated the cache");

    const UpgradeAllOperationPreparedSnapshot* snapshot = prepared.snapshot();
    expect(snapshot != nullptr, "Prepared aggregate snapshot is unavailable");
    PreparedUpgradeAllAurPreflight aur_preflight =
        prepare_upgrade_all_aur_preflight(*snapshot, config);
    expect(
        aur_preflight.has_filtered_operation() &&
            aur_preflight.foreign_inventory().status ==
                UpgradeAllForeignInventoryPhaseStatus::Completed &&
            aur_preflight.aur_query_result() != nullptr &&
            aur_preflight.aur_preflight() != nullptr &&
            aur_preflight.filtered_operation() != nullptr &&
            aur_preflight.issues().empty(),
        "Fresh upgrade-all AUR preflight lost production authority");
    expect(
        !fs::exists(cache_root) && stub::system_commands().empty() &&
            stub::aur_execution_calls().empty(),
        "Read-only upgrade-all AUR preflight crossed a mutation boundary");
    stub::require_script_consumed();
}

void test_read_only_aur_preflight_requires_check_stays_blocked() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"dry-check-git"}));
    enqueue_aur_query(
        {{"dry-check-git", "dry-check-git"}}, "0");
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "read-only upgrade-all RequiresCheck preflight");
    const UpgradeAllOperationPreparedSnapshot* snapshot =
        prepared.snapshot();
    expect(snapshot != nullptr, "RequiresCheck dry snapshot is unavailable");

    PreparedUpgradeAllAurPreflight aur_preflight =
        prepare_upgrade_all_aur_preflight(*snapshot, config);
    const PreparedFilteredAurUpdateOperation* filtered =
        aur_preflight.filtered_operation();
    expect(
        filtered != nullptr && filtered->is_blocked() &&
            filtered->devel_requires_check_policy_snapshot() ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::BlockOperation} &&
            filtered->execution_preflight()
                    .devel_requires_check_policy ==
                filtered->devel_requires_check_policy_snapshot() &&
            filtered->execution_preflight().targets.size() == 1 &&
            filtered->execution_preflight().targets.front().status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            filtered->execution_preflight()
                    .targets.front()
                    .issues.front()
                    .reason ==
                AurUpdateExecutionReason::DevelRequiresCheck &&
            filtered->source_build_preparation().has_value() &&
            filtered->source_build_preparation()
                    ->devel_requires_check_policy ==
                filtered->devel_requires_check_policy_snapshot(),
        "upgrade-all read-only RequiresCheck preflight changed blocker semantics");
    expect(
        stub::system_commands().empty() &&
            stub::aur_execution_calls().empty(),
        "upgrade-all read-only RequiresCheck preflight crossed mutation");
    stub::require_script_consumed();
}

void test_cache_replacement_after_system_blocks_inventory_and_aur() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const AppConfig config = full_option_config();
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "cache replacement after system");

    fs::path moved;
    stub::set_after_system_command_hook([&moved]() {
        moved = move_active_cache_root("after-system");
    });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::ForeignInventory &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::CacheAuthorityInvalid),
        "Cache replacement after system did not stop before inventory");
    expect(
        result.system_source.system.status ==
                SystemUpgradePhaseStatus::Completed &&
            stub::system_commands().size() == 1,
        "Cache replacement fixture did not occur after system completion");
    expect(
        !result.issues.empty() &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Phase-boundary cache replacement lost typed failure detail");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "cache replacement after system");
    expect(
        result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::NotAttempted &&
            result.foreign_inventory.not_attempted_reason ==
                UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "Post-system cache replacement lost foreign-inventory reason");
    expect_no_inventory_or_aur("cache replacement after system");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_strict_preference_absence_blocks_without_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({"missing-source"}));
    stub::enqueue_preference_result(
        "missing-source", SourcePreferenceAbsent{});

    UpgradeAllOperationResult result = take_blocked(
        prepare_upgrade_all_operation(AppConfig{}),
        "strict preference absence");
    expect(
        result.status ==
                UpgradeAllOperationStatus::BlockedBeforeMutation &&
            result.system_source.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system_source.registered_source_results.size() == 1,
        "Strict preference absence did not retain blocked source result");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::PreparationBlocked,
        "strict preference absence");
    expect(stub::system_commands().empty(),
           "Strict preference absence executed system mutation");
    expect_no_inventory_or_aur("strict preference absence");
    stub::require_script_consumed();
}

void test_strict_preference_read_failure_blocks_without_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({"broken-source"}));
    stub::enqueue_preference_result(
        "broken-source",
        preference_failure(
            "broken-source", "fixture strict read failure"));

    UpgradeAllOperationResult result = take_blocked(
        prepare_upgrade_all_operation(AppConfig{}),
        "strict preference read failure");
    expect(
        result.status ==
                UpgradeAllOperationStatus::BlockedBeforeMutation &&
            !result.system_source.issues.empty() &&
            result.system_source.issues.front().source_preference_failure.has_value(),
        "Strict read failure lost typed preparation detail");
    expect_no_inventory_or_aur("strict preference read failure");
    stub::require_script_consumed();
}

void test_nonregular_preference_snapshot_blocks_aggregate() {
    stub::reset();
    SourcePreferenceDirectorySnapshot directory;
    directory.root_exists = true;
    directory.entries.push_back(SourcePreferenceEntrySnapshot{
        0, "/preferences/not-regular", "not-regular", false});
    stub::set_preference_directory(std::move(directory));

    UpgradeAllOperationResult result = take_blocked(
        prepare_upgrade_all_operation(AppConfig{}),
        "non-regular source preference snapshot");
    expect(
        result.status == UpgradeAllOperationStatus::BlockedBeforeMutation &&
            result.system_source.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system_source.failure_diagnostic().has_value() &&
            result.system_source.failure_diagnostic()->find(
                "non-regular") != std::string::npos,
        "Non-regular preference snapshot was not an aggregate hard error");
    expect(stub::system_commands().empty(),
           "Non-regular preference snapshot reached system mutation");
    expect_no_inventory_or_aur("non-regular source preference snapshot");
    stub::require_script_consumed();
}

void test_invalid_preference_snapshot_blocks_aggregate() {
    stub::reset();
    stub::set_preference_directory(preference_directory({"bad name", "valid"}));

    UpgradeAllOperationResult result = take_blocked(
        prepare_upgrade_all_operation(AppConfig{}),
        "invalid source preference snapshot");
    expect(
        result.status == UpgradeAllOperationStatus::BlockedBeforeMutation &&
            result.system_source.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system_source.failure_diagnostic().has_value() &&
            result.system_source.failure_diagnostic()->find(
                "invalid entry name") != std::string::npos,
        "Invalid preference snapshot was not an aggregate hard error");
    expect(stub::strict_preference_read_history().empty(),
           "Invalid preference snapshot reached a strict entry read");
    expect(stub::system_commands().empty(),
           "Invalid preference snapshot reached system mutation");
    expect_no_inventory_or_aur("invalid source preference snapshot");
    stub::require_script_consumed();
}

void test_system_source_preparation_blocker_is_aggregate_blocker() {
    stub::reset();
    stub::set_preference_directory(preference_directory({"source-root"}));
    stub::enqueue_preference_result(
        "source-root", loaded_preference("source-root"));
    stub::fail_source_identity(
        "source-root", "fixture identity resolution failure");

    UpgradeAllOperationResult result = take_blocked(
        prepare_upgrade_all_operation(AppConfig{}),
        "system/source preparation blocker");
    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(result)});
    const UnifiedPlanObservationResult& observation_result =
        projection->observation_result();
    const UnifiedPlanObservation* observation =
        observation_result.observation();
    expect(
        result.status ==
                UpgradeAllOperationStatus::BlockedBeforeMutation &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::Preparation &&
            result.system_source.has_blocking_issue() &&
            observation_result.is_valid() &&
            observation != nullptr &&
            observation->status() ==
                UnifiedPlanObservationStatus::Blocked &&
            observation->transaction_intents().empty() &&
            !result.cross_source_version_lock_correlation.has_value(),
        "Nested preparation blocker was not preserved");
    expect_no_inventory_or_aur("system/source preparation blocker");
    stub::require_script_consumed();
}

void test_option_mismatch_rejected_before_system_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const AppConfig prepared_config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(prepared_config),
        "option mismatch fixture");
    AppConfig execution_config = prepared_config;
    execution_config.no_confirm = true;

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), execution_config);
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    OptionSnapshotMismatch) &&
            result.has_inconsistency(),
        "Aggregate option mismatch was not typed");
    const UpgradeAllPhasePackageStateObservations phase_observations =
        project_upgrade_all_phase_package_state_observations(result);
    expect(
        phase_observations.system_source.state ==
                PackageStateObservation::Unverified &&
            phase_observations.system_source.reason ==
                ObservationReason::InconsistentEvidence &&
            phase_observations.aur.state ==
                PackageStateObservation::NotObserved &&
            phase_observations.aur.reason ==
                ObservationReason::PhaseNotAttempted,
        "Inconsistent system/source and unattempted AUR phases were conflated");
    expect(stub::system_commands().empty(),
           "Option mismatch crossed system mutation boundary");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency,
        "option mismatch");
    stub::require_script_consumed();
}

void test_capability_replay_rejected_before_second_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory({});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config), "replay fixture");

    UpgradeAllOperationResult first =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(first.is_success(), "First aggregate execution failed");
    const std::size_t command_count = stub::system_commands().size();

    UpgradeAllOperationResult replay =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        replay.status == UpgradeAllOperationStatus::InconsistentResult &&
            has_issue(
                replay,
                UpgradeAllOperationIssueKind::
                    PreparedCapabilityConsumed),
        "Aggregate capability replay was not typed");
    expect(
        stub::system_commands().size() == command_count,
        "Aggregate replay performed a second system mutation");
    stub::require_script_consumed();
}

void test_outer_source_snapshot_mismatch_is_rejected() {
    stub::reset();
    const AppConfig config;
    PreparedUpgradeAllOperation prepared =
        prepare_sources({"source-root"}, config);
    prepared.make_source_snapshot_inconsistent_for_test();

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        has_issue(
            result,
            UpgradeAllOperationIssueKind::SourceSnapshotMismatch) &&
            result.has_inconsistency(),
        "Outer/nested source snapshot mismatch was not rejected");
    expect(stub::system_commands().empty(),
           "Source snapshot mismatch executed system mutation");
    stub::require_script_consumed();
}

void test_explicit_source_adapter_correlation_mismatch_is_rejected() {
    stub::reset();
    const AppConfig config;
    PreparedUpgradeAllOperation prepared =
        prepare_sources({"source-root"}, config);
    prepared.make_explicit_source_correlation_inconsistent_for_test();

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        has_issue(
            result,
            UpgradeAllOperationIssueKind::
                ExplicitSourceCorrelationInconsistent) &&
            result.has_inconsistency(),
        "Explicit source adapter mismatch was not rejected");
    expect(stub::system_commands().empty(),
           "Explicit adapter mismatch executed system mutation");
    stub::require_script_consumed();
}

void test_nested_system_source_correlation_mismatch_is_rejected() {
    stub::reset();
    const AppConfig config;
    PreparedUpgradeAllOperation prepared =
        prepare_sources({"source-root"}, config);
    prepared.make_nested_system_source_correlation_inconsistent_for_test();

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            result.system_source.status ==
                SystemSourceUpgradeStatus::InconsistentResult &&
            result.has_inconsistency() &&
            !result.cross_source_version_lock_correlation.has_value(),
        "Nested PR2 correlation mismatch was not retained");
    expect(stub::system_commands().empty(),
           "Nested PR2 mismatch executed system mutation");
    expect_no_inventory_or_aur("nested PR2 mismatch");
    stub::require_script_consumed();
}

void test_unexpected_exception_after_system_start_is_not_unattempted() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "unexpected system-start exception fixture");
    prepared.set_nested_system_source_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint::SystemPhaseStarted);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::System &&
            result.system_source.system.status ==
                SystemUpgradePhaseStatus::Failed &&
            result.system_source.system.package_state_change ==
                PackageStateChange::Unknown &&
            result.system_source.system.diagnostic.has_value() &&
            result.system_source.system.diagnostic->find(
                "The system result is unavailable because an unexpected exception occurred after the phase started") !=
                std::string::npos,
        "System-start exception was falsely reported as NotAttempted");
    expect(
        stub::system_commands().empty(),
        "System-start test hook crossed the system mutation boundary");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::PriorAggregateInconsistency,
        "unexpected system-start exception");
    expect_no_inventory_or_aur("unexpected system-start exception");
    stub::require_script_consumed();
}

void test_unexpected_exception_preserves_system_completion() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "unexpected post-system exception fixture");
    prepared.set_nested_system_source_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint::SystemPhaseCompleted);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            result.system_source.system.status ==
                SystemUpgradePhaseStatus::Completed &&
            result.system_source.system.package_state_change ==
                PackageStateChange::Unknown &&
            result.has_partial_completion(),
        "Post-system exception discarded known system completion");
    expect(
        stub::system_commands().size() == 1,
        "Post-system exception fixture did not complete system command");
    expect_no_inventory_or_aur("unexpected post-system exception");
    stub::require_script_consumed();
}

void test_unexpected_exception_after_source_start_is_incomplete() {
    stub::reset();
    const std::vector<std::string> sources = {"source-started"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    prepared.set_nested_system_source_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint::SourceWorkItemStarted,
        true);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    const RegisteredSourceUpgradeResult& source =
        result.system_source.registered_source_results.front();
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::RegisteredSource &&
            result.system_source.system.status ==
                SystemUpgradePhaseStatus::Completed &&
            result.system_source.system.package_state_change ==
                PackageStateChange::NoChange &&
            source.status ==
                RegisteredSourceUpgradeStatus::Incomplete &&
            source.failure_kind ==
                RegisteredSourceUpgradeFailureKind::
                    UnknownException &&
            source.diagnostic.has_value() &&
            source.diagnostic->find(
                "The registered source result is unavailable because an unexpected exception occurred after the phase started") !=
                std::string::npos &&
            result.has_partial_completion(),
        "Source-start exception was falsely reported as NotAttempted");
    expect(
        stub::source_execution_calls().empty(),
        "Source-start test hook crossed the source mutation boundary");
    expect_no_inventory_or_aur("unexpected source-start exception");
    stub::require_script_consumed();
}

void test_unexpected_exception_preserves_recorded_source_result() {
    stub::reset();
    const std::vector<std::string> sources = {
        "source-recorded", "source-pending"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::Installed));
    prepared.set_nested_system_source_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint::SourceResultRecorded);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Updated &&
            result.system_source.registered_source_results[0].package_state_change ==
                PackageStateChange::Changed &&
            result.system_source.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.package_state_change() ==
                PackageStateChange::Changed &&
            result.has_partial_completion(),
        "Unexpected exception discarded a recorded source result");
    expect(
        stub::source_execution_calls().size() == 1,
        "Recorded-source exception executed a later source");
    expect_no_inventory_or_aur("unexpected recorded-source exception");
    stub::require_script_consumed();
}

void test_system_failure_is_fail_fast() {
    stub::reset();
    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "complete-zero system failure");

    const auto& correlation = *require_cross_source_correlation(
        result, "complete-zero system failure");
    expect(
        correlation.observation.has_value() &&
            correlation.observation->status ==
                CrossSourceVersionLockObservationStatus::
                    Complete &&
            correlation.observation->candidates.empty() &&
            correlation.observation->issues.empty() &&
            correlation.assessments.empty() &&
            correlation.possible_blocker_assessment_indices.empty() &&
            !correlation.failure.has_value(),
        "Complete-zero correlation was not retained");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1 &&
            stub::installed_relation_inventory_calls() == 0 &&
            stub::installed_runtime_dependency_inventory_calls() ==
                0 &&
            stub::repository_candidate_call_history().empty() &&
            stub::info_strict_call_history().empty(),
        "Complete-zero observer did not run exactly once");
    expect(
        event_position(
            stub::EventKind::SystemCommand,
            stub::system_commands().front()) <
            event_position(
                stub::EventKind::
                    RepositoryConfigurationResolution,
                "repository-configuration"),
        "Secondary observation ran before the system command failed");
    stub::require_script_consumed();
}

void test_virtualbox_system_failure_retains_possible_correlation() {
    stub::reset();
    arrange_virtualbox_cross_source_observation();
    stub::enqueue_info_strict_result(virtualbox_replacement_package());

    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "VirtualBox system failure");

    const auto& correlation = *require_cross_source_correlation(
        result, "VirtualBox system failure");
    expect(
        correlation.observation.has_value() &&
            correlation.observation->status ==
                CrossSourceVersionLockObservationStatus::
                    Complete &&
            correlation.observation->candidates.size() == 1 &&
            correlation.assessments.size() == 1 &&
            correlation.assessments.front().status ==
                CrossSourceVersionLockStatus::
                    CompatibleReplacement &&
            correlation.possible_blocker_assessment_indices ==
                std::vector<std::size_t>{0} &&
            !correlation.failure.has_value(),
        "VirtualBox possible candidate correlation differs");
    expect(
        correlation.assessments.front()
                    .installed_requirement_against_installed_version
                    ->satisfaction() ==
                ConstraintSatisfaction::Satisfied &&
            correlation.assessments.front()
                    .installed_requirement_against_repository_candidate
                    ->satisfaction() ==
                ConstraintSatisfaction::Unsatisfied,
        "VirtualBox possible-candidate typed evidence differs");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1 &&
            stub::installed_relation_inventory_calls() == 1 &&
            stub::installed_runtime_dependency_inventory_calls() ==
                1 &&
            stub::repository_candidate_call_history() ==
                std::vector<std::string>{"virtualbox"} &&
            stub::info_strict_call_history() ==
                std::vector<std::string>{
                    "virtualbox-ext-oracle"},
        "VirtualBox observer was retried or skipped a read-only stage");
    stub::require_script_consumed();
}

void test_partial_system_failure_observation_is_retained() {
    stub::reset();
    arrange_virtualbox_cross_source_observation();
    stub::enqueue_info_strict_response_failure(
        "fixture AUR replacement schema failure");

    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "partial system failure observation");

    const auto& correlation = *require_cross_source_correlation(
        result, "partial system failure observation");
    expect(
        correlation.observation.has_value() &&
            correlation.observation->status ==
                CrossSourceVersionLockObservationStatus::Partial &&
            correlation.observation->candidates.size() == 1 &&
            correlation.assessments.size() == 1 &&
            correlation.assessments.front().status ==
                CrossSourceVersionLockStatus::Unknown &&
            correlation.possible_blocker_assessment_indices ==
                std::vector<std::size_t>{0} &&
            !correlation.failure.has_value(),
        "Partial observation was flattened or discarded");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1 &&
            stub::info_strict_call_history().size() == 1,
        "Partial observer was not attempted exactly once");
    stub::require_script_consumed();
}

void test_failed_system_failure_observation_is_retained() {
    stub::reset();
    stub::set_foreign_inventory_failure(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture secondary foreign inventory failure"});

    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "failed system failure observation");

    const auto& correlation = *require_cross_source_correlation(
        result, "failed system failure observation");
    expect(
        correlation.observation.has_value() &&
            correlation.observation->status ==
                CrossSourceVersionLockObservationStatus::Failed &&
            correlation.observation->candidates.empty() &&
            correlation.assessments.empty() &&
            correlation.possible_blocker_assessment_indices.empty() &&
            !correlation.failure.has_value(),
        "Failed observation was flattened into no correlation");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1 &&
            stub::installed_relation_inventory_calls() == 0,
        "Failed observer was retried or crossed a later stage");
    stub::require_script_consumed();
}

void test_system_failure_observer_bad_alloc_is_secondary() {
    stub::reset();
    stub::set_after_foreign_inventory_hook([]() { throw std::bad_alloc(); });

    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "secondary allocation failure");

    const auto& correlation = *require_cross_source_correlation(
        result, "secondary allocation failure");
    expect(
        !correlation.observation.has_value() &&
            correlation.assessments.empty() &&
            correlation.possible_blocker_assessment_indices.empty() &&
            correlation.failure.has_value() &&
            correlation.failure->kind ==
                UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
                    ResourceExhaustion &&
            !correlation.failure->diagnostic.has_value(),
        "Allocation failure did not remain allocation-free secondary evidence");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1,
        "Allocation failure retried the observer");
    stub::require_script_consumed();
}

void test_system_failure_observer_runtime_error_is_secondary() {
    stub::reset();
    stub::set_after_foreign_inventory_hook([]() {
        throw std::runtime_error("fixture unexpected observer failure");
    });

    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "secondary std::exception");

    const auto& correlation = *require_cross_source_correlation(
        result, "secondary std::exception");
    expect(
        !correlation.observation.has_value() &&
            correlation.failure.has_value() &&
            correlation.failure->kind ==
                UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
                    UnexpectedException &&
            correlation.failure->diagnostic ==
                std::optional<std::string>{
                    "fixture unexpected observer failure"},
        "std::exception did not become typed secondary evidence");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1,
        "std::exception retried the observer");
    stub::require_script_consumed();
}

void test_system_failure_observer_unknown_exception_is_secondary() {
    stub::reset();
    stub::set_after_foreign_inventory_hook(
        []() { throw UnknownCorrelationObservationFailure{}; });

    UpgradeAllOperationResult result =
        execute_system_failure_for_correlation();
    expect_system_failure_primary_preserved(
        result, "secondary unknown exception");

    const auto& correlation = *require_cross_source_correlation(
        result, "secondary unknown exception");
    expect(
        !correlation.observation.has_value() &&
            correlation.failure.has_value() &&
            correlation.failure->kind ==
                UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
                    UnknownException &&
            !correlation.failure->diagnostic.has_value(),
        "Non-std exception did not become typed secondary evidence");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1,
        "Non-std exception retried the observer");
    stub::require_script_consumed();
}

void test_nested_cache_seed_failure_is_typed() {
    stub::reset();
    const AppConfig config;
    PreparedUpgradeAllOperation prepared =
        prepare_sources({"cache-seed"}, config);
    stub::fail_cache_seed();

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::BlockedBeforeMutation &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::Preparation &&
            result.system_source.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Nested cache seed failure was not typed by aggregate");
    expect(
        !result.system_source.issues.empty() &&
            result.system_source.issues.back().trusted_cache_failure.has_value() &&
            !result.issues.empty() &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Nested cache seed failure lost transferred typed detail");
    expect(
        result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::
                    NotAttempted &&
            result.foreign_inventory.not_attempted_reason ==
                UpgradeAllNotAttemptedReason::
                    CacheAuthorityFailure,
        "Nested cache seed failure lost inventory stop reason");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "nested cache seed failure");
    expect(stub::system_commands().empty(),
           "Nested cache seed failure reached system mutation");
    expect_no_inventory_or_aur("nested cache seed failure");
    stub::require_script_consumed();
}

void test_nested_cache_activation_failure_is_typed() {
    stub::reset();
    const AppConfig config;
    PreparedUpgradeAllOperation prepared =
        prepare_sources({"cache-activation"}, config);
    stub::fail_cache_activation();

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::BlockedBeforeMutation &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::Preparation &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Nested cache activation failure was not typed by aggregate");
    expect(
        !result.issues.empty() &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Nested cache activation failure lost TrustedCacheFailure");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "nested cache activation failure");
    expect(stub::system_commands().empty(),
           "Nested cache activation failure reached system mutation");
    expect_no_inventory_or_aur("nested cache activation failure");
    stub::require_script_consumed();
}

void test_registered_source_cache_failure_is_typed() {
    stub::reset();
    const std::vector<std::string> sources = {"cache-replaced"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_cache_failure();

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::StoppedOnSourceFailure &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::RegisteredSource &&
            result.system_source.registered_source_results.front().failure_kind ==
                RegisteredSourceUpgradeFailureKind::
                    CacheAuthorityFailure,
        "Registered-source cache failure was flattened");
    expect(
        !result.issues.empty() &&
            result.issues.back().kind ==
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Registered-source cache failure lost aggregate typed detail");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "registered-source cache failure");
    expect(stub::system_commands().size() == 1 &&
               stub::source_execution_calls().size() == 1,
           "Registered-source cache failure fixture crossed wrong phase");
    expect_no_inventory_or_aur("registered-source cache failure");
    stub::require_script_consumed();
}

void test_first_source_failure_stops_remaining_and_aur() {
    stub::reset();
    const std::vector<std::string> sources = {"first", "second", "third"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_failure("fixture first source failure");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedOnSourceFailure &&
            result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Failed &&
            result.system_source.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.system_source.registered_source_results[2].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            !result.cross_source_version_lock_correlation.has_value(),
        "First source failure did not preserve fail-fast results");
    expect(
        stub::source_execution_calls().size() == 1,
        "First source failure executed a later source");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::SourceFailure,
        "first source failure");
    expect_no_inventory_or_aur("first source failure");
    stub::require_script_consumed();
}

void test_later_source_failure_preserves_prior_update() {
    stub::reset();
    const std::vector<std::string> sources = {"updated-first", "failed-second",
                                              "pending-third"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::Installed));
    stub::enqueue_source_failure("fixture later source failure");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Updated &&
            result.system_source.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::Failed &&
            result.system_source.registered_source_results[2].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.package_state_change() ==
                PackageStateChange::Changed &&
            result.has_partial_completion(),
        "Later source failure lost prior source completion");
    expect_no_inventory_or_aur("later source failure");
    stub::require_script_consumed();
}

void test_source_cleanup_failure_stops_before_inventory() {
    stub::reset();
    const std::vector<std::string> sources = {"cleanup-source", "pending"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_cleanup_failure(
        ArtifactInstallExecutionOutcome::Installed,
        "fixture source cleanup failure");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::
                             StoppedAfterSourceCleanupFailure &&
            result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::
                    UpdatedCleanupFailed &&
            result.system_source.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.has_cleanup_failure() &&
            result.has_partial_completion() &&
            result.package_state_change() ==
                PackageStateChange::Changed &&
            !result.cross_source_version_lock_correlation.has_value(),
        "Source cleanup failure lost post-transaction state");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::SourceCleanupFailure,
        "source cleanup failure");
    expect_no_inventory_or_aur("source cleanup failure");
    stub::require_script_consumed();
}

void test_foreign_inventory_configuration_failure() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_repository_configuration_failure(PackageMetadataFailure{
        PackageMetadataErrorCode::ConfigurationMalformed,
        "fixture repository configuration failure"});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "inventory configuration failure fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::Failed &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    ForeignInventoryConfigurationFailed) &&
            result.has_query_failure(),
        "Repository configuration failure was not typed");
    expect(stub::inventory_calls() == 0,
           "Configuration failure called foreign inventory reader");
    expect(stub::info_many_call_history().empty(),
           "Configuration failure called AUR query");
    stub::require_script_consumed();
}

void test_foreign_inventory_read_failure() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory_failure(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture foreign inventory failure"});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "inventory read failure fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::Failed &&
            result.foreign_inventory.failure.has_value() &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    ForeignInventoryReadFailed) &&
            result.has_query_failure() &&
            result.has_partial_completion(),
        "Foreign inventory read failure lost typed detail");
    expect_aur_not_attempted(
        result,
        UpgradeAllNotAttemptedReason::ForeignInventoryFailure,
        "inventory read failure");
    expect(stub::info_many_call_history().empty(),
           "Inventory read failure called AUR query");
    stub::require_script_consumed();
}

void test_cache_replacement_during_inventory_blocks_aur_query() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"inventory-root"}));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "foreign inventory cache replacement fixture");

    fs::path moved;
    stub::set_after_foreign_inventory_hook([&moved]() {
        moved = move_active_cache_root("during-foreign-inventory");
    });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurQuery &&
            result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::Completed &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Foreign-inventory cache replacement was not stopped before AUR query");
    expect(
        !result.issues.empty() &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Foreign-inventory cache replacement lost typed detail");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "foreign-inventory cache replacement");
    expect(
        stub::repository_configuration_calls() == 1 &&
            stub::inventory_calls() == 1 &&
            stub::info_many_call_history().empty() &&
            stub::database_call_count() == 1 &&
            stub::resolver_call_count() == 0,
        "Foreign-inventory cache replacement crossed the next production stage");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_cache_replacement_during_aur_query_blocks_preparation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"query-root"}));
    enqueue_aur_query({{"query-root", "query-base"}});
    stub::enqueue_database_paths(PacmanDatabasePaths{
        "/upgrade-all-stub/root",
        "/upgrade-all-stub/database"});
    stub::set_database_failure(PackageMetadataFailure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "fixture later database failure"});
    stub::set_resolver_handler(
        [](const std::vector<std::string>&,
           const ProviderSelectionCallback&) -> BuildPlan {
            throw std::runtime_error(
                "fixture later provider failure");
        });
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "AUR query cache replacement fixture");

    fs::path moved;
    stub::set_after_info_many_hook([&moved]() {
        moved = move_active_cache_root("during-aur-query");
    });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::Completed &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "AUR-query cache replacement was not stopped before preparation");
    expect(
        !result.issues.empty() &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "AUR-query cache replacement lost typed detail");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "AUR-query cache replacement");
    expect(stub::database_call_count() == 1 &&
               stub::resolver_call_count() == 0 &&
               stub::aur_execution_calls().empty(),
           "AUR-query cache replacement reached provider/database preparation");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_cache_replacement_during_filtered_planning_blocks_database() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"planning-root"}));
    enqueue_aur_query({{"planning-root", "planning-base"}});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "filtered planning cache replacement fixture");

    fs::path moved;
    BuildPlan plan = root_plan({{"planning-root", "planning-base"}});
    stub::set_resolver_handler(
        [&moved, plan = std::move(plan)](
            const std::vector<std::string>&,
            const ProviderSelectionCallback&) mutable -> BuildPlan {
            moved = move_active_cache_root("during-filtered-planning");
            return std::move(plan);
        });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Filtered-planning cache replacement was not stopped before database preparation");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "filtered-planning cache replacement");
    expect(
        stub::resolver_call_count() == 1 &&
            stub::database_call_count() == 1 &&
            stub::aur_execution_calls().empty(),
        "Filtered-planning cache replacement reached package-database preparation");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_filtered_preparation_failure_keeps_stage_precedence() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"provider-root"}));
    enqueue_aur_query({{"provider-root", "provider-base"}});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "filtered preparation failure precedence fixture");

    fs::path moved;
    stub::set_resolver_handler(
        [&moved](const std::vector<std::string>&,
                 const ProviderSelectionCallback&) -> BuildPlan {
            moved = move_active_cache_root(
                "during-filtered-preparation");
            throw std::runtime_error(
                "fixture filtered provider preparation failure");
        });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    FilteredAurPreparationFailed) &&
            !has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Filtered preparation failure lost same-stage precedence");
    expect(
        stub::resolver_call_count() == 1 &&
            stub::database_call_count() == 1 &&
            stub::aur_execution_calls().empty(),
        "Filtered preparation failure crossed the expected stage boundary");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_strict_preference_failure_precedes_post_stage_cache_drift() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"strict-root"}));
    enqueue_aur_query({{"strict-root", "strict-root"}});
    return_build_plan(
        root_plan({{"strict-root", "strict-root"}}),
        {"strict-root"});
    stub::enqueue_source_preference_result(
        "strict-root",
        preference_failure(
            "strict-root", "fixture strict preference failure"));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "strict preference and cache drift fixture");

    fs::path moved;
    stub::set_after_next_strict_preference_read_hook([&moved]() {
        moved = move_active_cache_root("after-strict-preference-failure");
    });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    const AurUpdatePreparationIssue* issue = find_aur_preparation_issue(
        result,
        AurUpdatePreparationReason::SourcePreferenceUnavailable);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution &&
            issue != nullptr &&
            issue->source_preference_failure.has_value() &&
            issue->source_preference_failure->diagnostic ==
                "fixture strict preference failure" &&
            !has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Strict preference failure was masked by post-stage cache drift");
    expect(
        !moved.empty() &&
            stub::strict_preference_read_history() ==
                std::vector<std::string>{"strict-root"} &&
            stub::database_call_count() == 1 &&
            stub::aur_execution_calls().empty(),
        "Strict preference failure crossed the typed preparation boundary");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_pacman_database_failure_precedes_post_stage_cache_drift() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"database-root"}));
    enqueue_aur_query({{"database-root", "database-root"}});
    return_build_plan(
        root_plan({{"database-root", "database-root"}}),
        {"database-root"});
    stub::enqueue_source_preference_result(
        "database-root", loaded_preference("database-root"));
    const PackageMetadataFailure database_failure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "fixture pacman database failure"};
    stub::enqueue_database_paths(PacmanDatabasePaths{
        "/upgrade-all-stub/root",
        "/upgrade-all-stub/database"});
    stub::enqueue_database_failure(database_failure);
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "pacman database failure and cache drift fixture");

    fs::path moved;
    stub::set_after_next_strict_preference_read_hook([&moved]() {
        moved = move_active_cache_root("before-pacman-database-failure");
    });
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    const AurUpdatePreparationIssue* issue = find_aur_preparation_issue(
        result,
        AurUpdatePreparationReason::PacmanDatabaseUnavailable);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution &&
            issue != nullptr &&
            issue->package_metadata_failure.has_value() &&
            issue->package_metadata_failure->code ==
                database_failure.code &&
            issue->package_metadata_failure->diagnostic ==
                database_failure.diagnostic &&
            !has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Pacman database failure was masked by post-stage cache drift");
    expect(
        !moved.empty() &&
            stub::strict_preference_read_history() ==
                std::vector<std::string>{"database-root"} &&
            stub::database_call_count() == 2 &&
            stub::aur_execution_calls().empty(),
        "Pacman database failure crossed the typed preparation boundary");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_executable_preparation_cache_drift_blocks_aur_execution() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"executable-root"}));
    enqueue_aur_query({{"executable-root", "executable-root"}});
    BuildPlan plan = root_plan({{"executable-root", "executable-root"}});
    fs::path moved;
    stub::set_resolver_handler(
        [&moved, plan = std::move(plan)](
            const std::vector<std::string>& targets,
            const ProviderSelectionCallback&) mutable -> BuildPlan {
            if(targets != std::vector<std::string>{"executable-root"}) {
                throw std::logic_error(
                    "Executable drift resolver target differs");
            }
            stub::set_after_next_cache_seed_hook([&moved]() {
                moved = move_active_cache_root(
                    "after-executable-cache-seed");
            });
            return std::move(plan);
        });
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "executable preparation cache drift fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::
                    CacheAuthorityInvalid),
        "Executable preparation cache drift passed the mutation gate");
    expect_aur_not_attempted(
        result, UpgradeAllNotAttemptedReason::CacheAuthorityFailure,
        "executable preparation cache drift");
    expect(
        !moved.empty() && stub::resolver_call_count() == 1 &&
            stub::database_call_count() == 2 &&
            stub::aur_execution_calls().empty(),
        "Executable preparation cache drift reached AUR execution");
    remove_cache_fixture(moved);
    stub::require_script_consumed();
}

void test_recoverable_aur_query_failure_blocks_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"recoverable-root"}));
    stub::enqueue_info_many_failure("fixture recoverable AUR failure");
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "recoverable query fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution &&
            result.aur.operation_result.has_value() &&
            result.has_query_failure() &&
            result.has_planning_issue() &&
            result.has_partial_completion(),
        "Recoverable AUR query failure was not retained");
    expect(stub::aur_execution_calls().empty(),
           "Recoverable AUR failure reached mutation");

    const FilteredAurUpdateExecutionResult& aur_result =
        result.aur.operation_result.value();
    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(aur_result.query_result),
                std::cref(aur_result.preflight), false});
    const UnifiedPlanObservationResult& observation_result =
        projection->observation_result();
    const UnifiedPlanObservation* observation =
        observation_result.observation();
    expect(
        observation_result.is_valid() && observation != nullptr &&
            observation->status() ==
                UnifiedPlanObservationStatus::Blocked &&
            observation->transaction_intents().empty(),
        "Actual recoverable AUR query result did not reach a typed Blocked observation");
    const bool query_failure_borrowed = std::any_of(
        observation->blockers().begin(),
        observation->blockers().end(),
        [&aur_result](const UnifiedPlanBlocker& blocker) {
            const auto* source =
                std::get_if<SourceFailureUnifiedPlanBlocker>(
                    &blocker);
            if(source == nullptr) return false;
            const auto* failure = std::get_if<
                UnifiedPlanBorrowedAuthorityReference<
                    AurUpdateQueryFailure>>(
                &source->detail);
            return failure != nullptr &&
                   &failure->get() ==
                       &aur_result.query_result
                            .recoverable_failures.front();
        });
    expect(
        query_failure_borrowed,
        "Actual recoverable AUR query failure was copied or flattened");
    stub::require_script_consumed();
}

void test_devel_requires_check_preserves_prior_phases_and_blocks_aur() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"manual-check-git"}));
    enqueue_aur_query(
        {{"manual-check-git", "manual-check-git"}}, "0");
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "devel RequiresCheck fixture");

    const UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);

    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.stopped_phase ==
                UpgradeAllOperationPhase::AurPreparation &&
            result.system_source.system.status ==
                SystemUpgradePhaseStatus::Completed &&
            result.foreign_inventory.status ==
                UpgradeAllForeignInventoryPhaseStatus::Completed &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution &&
            result.aur.operation_result.has_value() &&
            result.has_partial_completion(),
        "Devel RequiresCheck rewrote or lost a prior phase outcome");
    const AurUpdateOperationResult& aur_result =
        result.aur.operation_result->reduced_operation_result;
    const FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    expect(
        filtered.has_consistent_devel_requires_check_policy_snapshot() &&
            filtered.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::BlockOperation} &&
            aur_result.status ==
                AurUpdateOperationStatus::BlockedBeforeExecution &&
            aur_result.targets.size() == 1 &&
            aur_result.targets.front().status ==
                AurUpdateOperationTargetStatus::Incomplete &&
            aur_result.targets.front().preflight_issues.size() == 1 &&
            aur_result.targets.front()
                    .preflight_issues.front()
                    .reason ==
                AurUpdateExecutionReason::DevelRequiresCheck &&
            aur_result.targets.front()
                    .preflight_issues.front()
                    .devel_requires_check_reason ==
                DevelRequiresCheckReason::SuffixCandidateOnly,
        "upgrade-all flattened the AUR RequiresCheck blocker");
    expect(
        stub::system_commands().size() == 1 &&
            stub::resolver_call_count() == 0 &&
            stub::aur_execution_calls().empty(),
        "RequiresCheck crossed the AUR mutation boundary");
    stub::require_script_consumed();
}

void test_total_aur_query_failure_blocks_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"fatal-root"}));
    stub::enqueue_info_many_response_failure(
        "fixture fatal AUR response failure");
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "total query failure fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.stopped_phase == UpgradeAllOperationPhase::AurQuery &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution &&
            !result.aur.operation_result.has_value() &&
            has_issue(
                result,
                UpgradeAllOperationIssueKind::AurQueryFailed) &&
            result.has_query_failure(),
        "Fatal AUR query failure was not typed by aggregate");
    expect(stub::resolver_call_count() == 0 &&
               stub::aur_execution_calls().empty(),
           "Fatal AUR query failure crossed planning/mutation boundary");
    stub::require_script_consumed();
}

void test_repository_source_key_is_derived_from_package_base() {
    const ResolvedSourceBuildIdentity first =
        source_identity("source-a", "source-base-a");
    const ResolvedSourceBuildIdentity second =
        source_identity("source-b", "source-base-b");
    const ResolvedSourceBuildIdentity sibling =
        source_identity("source-a-sibling", "source-base-a");

    expect(
        first.canonical_source_key() == "repository:source-base-a" &&
            second.canonical_source_key() ==
                "repository:source-base-b" &&
            first.canonical_source_key() !=
                second.canonical_source_key() &&
            sibling.canonical_source_key() ==
                first.canonical_source_key() &&
            sibling.package_base() == first.package_base() &&
            sibling.requested_name() != first.requested_name(),
        "Repository source key did not remain a PackageBase-derived identity");
}

void test_mapping_issue_blocks_aur_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"mapping-root"}));
    enqueue_aur_query({{"mapping-root", "mapping-root"}});
    BuildPlan plan = root_plan({{"mapping-root", "mapping-root"}});
    plan.root_targets[0].requested_name = "wrong-root";
    plan.package_targets[0].roots[0] = plan.root_targets[0];
    return_build_plan(std::move(plan), {"mapping-root"});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "mapping issue fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::InconsistentResult &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::InconsistentResult &&
            result.has_inconsistency(),
        "Filtered mapping issue was not retained as inconsistency");
    expect(stub::aur_execution_calls().empty(),
           "Mapping issue reached AUR mutation");
    stub::require_script_consumed();
}

void test_preflight_blocker_stops_before_preparation_io() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory(foreign_inventory({"blocked-root"}));
    enqueue_aur_query({{"blocked-root", "blocked-root"}});
    BuildPlan plan = root_plan({{"blocked-root", "blocked-root"}});
    plan.unresolved.push_back("missing-dependency>=2");
    return_build_plan(std::move(plan), {"blocked-root"});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "preflight blocker fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::
                    StoppedBeforeAurExecution &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution &&
            result.aur.operation_result.has_value() &&
            !result.is_success(),
        "Preflight blocker was rounded to success");
    expect(stub::database_call_count() == 1 &&
               stub::aur_execution_calls().empty(),
           "Preflight blocker crossed AUR preparation/mutation boundary");
    stub::require_script_consumed();
}

void test_preparation_blocker_stops_before_aur_mutation() {
    stub::reset();
    stub::fail_supported_options_guard("fixture AUR option guard failure");
    const AppConfig config;
    UpgradeAllOperationPreparation preparation =
        prepare_upgrade_all_operation(config);
    expect(
        std::holds_alternative<UpgradeAllOperationResult>(preparation),
        "Static source option failure produced an executable aggregate capability");
    UpgradeAllOperationResult result = std::move(
        std::get<UpgradeAllOperationResult>(preparation));
    expect(
        result.status ==
                UpgradeAllOperationStatus::BlockedBeforeMutation &&
            !result.is_success() &&
            !result.issues.empty(),
        "Static source option blocker was not rejected before mutation");
    expect(stub::events().size() == 1 &&
               stub::events().front().kind ==
                   stub::EventKind::SeparatedInstallOptionsGuard &&
               stub::aur_execution_calls().empty(),
           "Static source option blocker crossed the early guard boundary");
    stub::require_script_consumed();
}

void test_no_updates_success_contract() {
    stub::reset();
    const std::vector<std::string> sources = {"same-source"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::UpToDate));
    stub::set_foreign_inventory({});

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::NoUpdates &&
            result.is_success() &&
            result.system_source.system.package_state_change ==
                PackageStateChange::NoChange &&
            result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::NoChange &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::NoUpdates &&
            result.package_state_change() ==
                PackageStateChange::NoChange &&
            !result.has_partial_completion() &&
            !result.has_not_attempted_phase() &&
            !result.cross_source_version_lock_correlation.has_value(),
        "NoUpdates conjunction was not enforced");
    const OperationStateProjection projection =
        project_upgrade_all_operation_state(result);
    expect(
        projection.outcome == OperationOutcome::NoOp &&
            projection.no_op_basis ==
                std::optional<NoOpBasis>{
                    NoOpBasis::VerifiedUnchanged},
        "Registered-source UpToDate evidence lost its NoOp basis");
    expect(
        stub::source_preparation_calls().size() == 1 &&
            stub::source_preparation_calls().front().update_policy ==
                SourceBuildUpdatePolicy::OnlyIfUpdated &&
            stub::package_base_source_execution_calls().empty(),
        "Upgrade-all UpToDate source crossed the closed preparation boundary");
    stub::require_script_consumed();
}

void test_system_only_authoritative_unchanged_observation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory({});
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "system-only unchanged fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::NoUpdates &&
            result.is_success() &&
            result.system_source.system.package_state_change ==
                PackageStateChange::NoChange &&
            result.package_state_change() ==
                PackageStateChange::NoChange,
        "System-only authoritative no-change observation differs");
    const OperationStateProjection operation_state =
        project_upgrade_all_operation_state(result);
    expect(
        operation_state.outcome == OperationOutcome::NoOp &&
            operation_state.package_state.state ==
                PackageStateObservation::VerifiedUnchanged &&
            operation_state.no_op_basis ==
                std::optional<NoOpBasis>{
                    NoOpBasis::NoRelevantWork},
        "System-only no-change evidence lost its typed NoOp basis");
    const std::size_t metadata_open_count =
        static_cast<std::size_t>(std::count_if(
            stub::events().begin(), stub::events().end(),
            [](const stub::Event& event) {
                return event.kind ==
                       stub::EventKind::MetadataSessionOpen;
            }));
    const std::size_t snapshot_count =
        static_cast<std::size_t>(std::count_if(
            stub::events().begin(), stub::events().end(),
            [](const stub::Event& event) {
                return event.kind ==
                       stub::EventKind::LocalPackageSnapshot;
            }));
    expect(
        stub::database_call_count() == 1 &&
            metadata_open_count == 2 && snapshot_count == 2,
        "System-only pre/post observation did not use package metadata authority");
    stub::require_script_consumed();
}

void test_system_only_changed_observation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory({});
    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "system-only changed fixture");
    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "2.0-1"}}));

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    const OperationStateProjection operation_state =
        project_upgrade_all_operation_state(result);
    expect(
        result.status == UpgradeAllOperationStatus::Completed &&
            result.is_success() &&
            result.package_state_change() ==
                PackageStateChange::Changed &&
            operation_state.outcome == OperationOutcome::Succeeded &&
            operation_state.package_state.state ==
                PackageStateObservation::Changed &&
            !operation_state.no_op_basis.has_value(),
        "System-only changed observation was flattened");
    stub::require_script_consumed();
}

void test_system_only_observation_failure_remains_unverified() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::set_foreign_inventory({});
    stub::MetadataSessionScript failure;
    failure.open_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "fixture system-only observation failure"};
    stub::enqueue_metadata_session(std::move(failure));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "system-only unverified fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::Completed &&
            result.is_success() &&
            result.system_source.system.package_state_change ==
                PackageStateChange::Unknown &&
            result.package_state_change() ==
                PackageStateChange::Unknown,
        "Observation failure was rounded to failure or NoUpdates");
    const OperationStateProjection operation_state =
        project_upgrade_all_operation_state(result);
    const UpgradeAllPhasePackageStateObservations phase_observations =
        project_upgrade_all_phase_package_state_observations(result);
    const PresentationProjection presentation =
        project_upgrade_all_presentation_with_operation_state(
            result, operation_state);
    expect(
        operation_state.outcome == OperationOutcome::Succeeded &&
            operation_state.package_state.state ==
                PackageStateObservation::Unverified &&
            operation_state.package_state.reason ==
                ObservationReason::BeforeSnapshotUnavailable,
        "System-only observation failure lost its typed unverified state");
    expect(
        phase_observations.system_source ==
                PackageStateObservationValue{
                    PackageStateObservation::Unverified,
                    ObservationReason::
                        BeforeSnapshotUnavailable} &&
            phase_observations.aur ==
                PackageStateObservationValue{
                    PackageStateObservation::
                        VerifiedUnchanged,
                    std::nullopt},
        "Phase projection lost snapshot failure or authoritative AUR no-change evidence");
    expect(
        presentation.summary_counts.total == 1 &&
            presentation.summary_counts.attention_required == 1 &&
            presentation.summary_counts.unverified == 1 &&
            presentation.attention_items.size() == 1 &&
            presentation.full_items.size() == 1,
        "System-only successful-unverified result fell through presentation");
    const PresentationItem& observation =
        presentation.attention_items.front();
    const auto outcome_reason = std::find_if(
        observation.upgrade_all_reasons.begin(),
        observation.upgrade_all_reasons.end(),
        [](const UpgradeAllPresentationReason& reason) {
            const OperationOutcome* outcome =
                std::get_if<OperationOutcome>(&reason.reason);
            return outcome != nullptr &&
                   *outcome == OperationOutcome::Succeeded;
        });
    expect(
        observation.package_state ==
                std::optional<PackageStateObservationValue>{
                    operation_state.package_state} &&
            observation.source_kind ==
                DiagnosticSourceKind::Pacman &&
            !observation.requested_package.has_value() &&
            !observation.package_base.has_value() &&
            !observation.canonical_source_identity.has_value() &&
            observation.diagnostic_class ==
                DiagnosticClass::RequiresCheck &&
            observation.requires_check &&
            !observation.is_blocking &&
            outcome_reason !=
                observation.upgrade_all_reasons.end() &&
            outcome_reason->phase ==
                UpgradeAllOperationPhase::System &&
            outcome_reason->source_kind ==
                DiagnosticSourceKind::Pacman,
        "System-only observation failure lost typed reason or attribution");
    stub::require_script_consumed();
}

void test_system_changed_only() {
    stub::reset();
    const std::vector<std::string> sources = {"same-source"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(
        sources,
        config,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}});
    enqueue_post_source_metadata(
        sources,
        LocalPackageVersionSnapshot{{"core", "2.0-1"}});
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::UpToDate));
    stub::set_foreign_inventory({});

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::Completed &&
            result.system_source.system.package_state_change ==
                PackageStateChange::Changed &&
            result.system_source.registered_source_results[0].package_state_change ==
                PackageStateChange::NoChange &&
            result.package_state_change() ==
                PackageStateChange::Changed,
        "System-only package change was not aggregated");
    stub::require_script_consumed();
}

void test_source_updated_only() {
    stub::reset();
    const std::vector<std::string> sources = {"updated-source"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::Installed));
    stub::set_foreign_inventory({});

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.is_success() &&
            result.system_source.system.package_state_change ==
                PackageStateChange::NoChange &&
            result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Updated &&
            result.package_state_change() ==
                PackageStateChange::Changed,
        "Source-only package change was not aggregated");
    expect(
        stub::source_preparation_calls().size() == 1 &&
            stub::package_base_source_execution_calls().size() == 1 &&
            stub::source_preparation_calls().front().package_name ==
                "updated-source" &&
            stub::package_base_source_execution_calls().front().package_name ==
                "updated-source" &&
            stub::package_base_source_execution_calls().front().database_paths.root_dir ==
                stub::source_execution_calls().front().database_paths.root_dir &&
            stub::package_base_source_execution_calls().front().database_paths.db_path ==
                stub::source_execution_calls().front().database_paths.db_path,
        "Upgrade-all registered repository route did not preserve prepare/Set execution authority");
    stub::require_script_consumed();
}

void test_registered_package_base_result_and_composition() {
    stub::reset();
    const std::string requested_child = "registered-child";
    const std::string package_base = "registered-suite";
    const std::string aur_root = "aur-root";
    const AppConfig config;

    stub::set_preference_directory(
        preference_directory({requested_child}));
    SourcePreferenceLoaded preference = loaded_preference(requested_child);
    preference.environment.ordered_assignments = {
        {"FIRST_FLAG", "first value"},
        {"SECOND_FLAG", "second value"}};
    stub::enqueue_preference_result(
        requested_child, std::move(preference));
    stub::set_source_identity(
        requested_child,
        source_identity(requested_child, package_base));
    stub::enqueue_metadata_session(metadata_session(
        {requested_child},
        LocalPackageVersionSnapshot{{requested_child, "1.0-1"}}));

    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "registered PackageBase composition fixture");
    enqueue_post_source_metadata(
        {requested_child},
        LocalPackageVersionSnapshot{{requested_child, "1.0-1"}});
    stub::enqueue_package_base_source_success(
        package_base,
        ArtifactPackageIdentity{requested_child, "4.2-3"},
        {ArtifactPackageIdentity{"registered-sibling", "4.2-3"},
         ArtifactPackageIdentity{"registered-child-debug", "4.2-3"}});
    stub::set_foreign_inventory(
        foreign_inventory({requested_child, aur_root}));
    enqueue_aur_query({{requested_child, package_base},
                       {aur_root, aur_root}});
    return_build_plan(root_plan({{aur_root, aur_root}}), {aur_root});
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);

    const RegisteredSourceUpgradeResult& registered =
        result.system_source.registered_source_results.front();
    expect(
        result.is_success() &&
            registered.status ==
                RegisteredSourceUpgradeStatus::Updated &&
            registered.package_base_execution.has_value() &&
            registered.package_base_execution->package_base ==
                package_base &&
            registered.package_base_execution->selected_child.identity.package_name == requested_child &&
            registered.package_base_execution->selected_child.identity.full_version == "4.2-3" &&
            registered.package_base_execution->selected_child.desired_reason ==
                DesiredInstallReason::Explicit &&
            registered.package_base_execution->selected_child.outcome ==
                ArtifactInstallExecutionOutcome::Installed &&
            registered.package_base_execution->unselected_artifacts.size() == 2 &&
            registered.package_base_execution->unselected_artifacts[0].package_name == "registered-sibling" &&
            registered.package_base_execution->unselected_artifacts[0].full_version == "4.2-3" &&
            registered.package_base_execution->unselected_artifacts[1].package_name ==
                "registered-child-debug" &&
            registered.package_base_execution->unselected_artifacts[1].full_version == "4.2-3",
        "Registered PackageBase aggregate lost selected or unselected identities");

    const auto environment_matches = [](
                                         const SourceBuildRequest& request) {
        const auto& assignments =
            request.custom_environment.ordered_assignments;
        return request.empty_value_policy ==
                   SourceEnvironmentEmptyValuePolicy::Omit &&
               assignments.size() == 2 &&
               assignments[0].key == "FIRST_FLAG" &&
               assignments[0].value == "first value" &&
               assignments[1].key == "SECOND_FLAG" &&
               assignments[1].value == "second value";
    };
    expect(
        stub::source_preparation_calls().size() == 1 &&
            stub::package_base_source_execution_calls().size() == 1 &&
            stub::source_preparation_calls().front().package_name ==
                requested_child &&
            stub::source_preparation_calls().front().package_base ==
                package_base &&
            stub::source_preparation_calls().front().update_policy ==
                SourceBuildUpdatePolicy::OnlyIfUpdated &&
            environment_matches(
                stub::source_preparation_calls().front().request) &&
            stub::package_base_source_execution_calls().front().package_name == requested_child &&
            stub::package_base_source_execution_calls().front().package_base == package_base &&
            environment_matches(
                stub::package_base_source_execution_calls().front().request),
        "Registered custom environment or PackageBase execution count changed");

    expect(
        result.foreign_inventory.inventory.size() == 2 &&
            result.foreign_inventory.inventory[0].name ==
                requested_child &&
            result.foreign_inventory.inventory[1].name == aur_root &&
            result.has_duplicate_exclusions() &&
            result.duplicate_excluded_aur_targets.size() == 1 &&
            result.duplicate_excluded_aur_targets[0].query_entry.installed_name == requested_child &&
            stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {requested_child, aur_root}} &&
            stub::aur_execution_calls().size() == 1 &&
            stub::aur_execution_calls().front().package_base ==
                aur_root &&
            stub::aur_execution_calls().front().plan_package_names ==
                std::vector<std::string>{aur_root},
        "Fresh inventory filtering rebuilt an explicit registered source");

    expect(
        event_position(
            stub::EventKind::SourcePreparation,
            requested_child) <
                event_position(
                    stub::EventKind::PackageBaseSourceExecution,
                    requested_child) &&
            event_position(
                stub::EventKind::PackageBaseSourceExecution,
                requested_child) <
                event_position(
                    stub::EventKind::ForeignInventoryQuery,
                    "foreign-inventory") &&
            event_position(
                stub::EventKind::ForeignInventoryQuery,
                "foreign-inventory") <
                event_position(
                    stub::EventKind::AurInfoMany,
                    "aur-info-many") &&
            event_position(
                stub::EventKind::AurInfoMany,
                "aur-info-many") <
                event_position(
                    stub::EventKind::BuildPlanResolution,
                    "build-plan") &&
            event_position(
                stub::EventKind::BuildPlanResolution,
                "build-plan") <
                event_position(
                    stub::EventKind::AurCheckout,
                    aur_root),
        "Registered PackageBase and filtered AUR phase order changed");
    stub::require_script_consumed();
}

void test_aur_updated_only_and_fresh_inventory() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "fresh inventory fixture");

    // POLICY(#281): preparation後に差し替えたinventoryだけがAUR queryへ届く。
    stub::set_foreign_inventory(foreign_inventory({"latest-root"}));
    enqueue_aur_query({{"latest-root", "latest-root"}});
    return_build_plan(
        root_plan({{"latest-root", "latest-root"}}),
        {"latest-root"});
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.is_success() &&
            result.foreign_inventory.inventory.size() == 1 &&
            result.foreign_inventory.inventory[0].name ==
                "latest-root" &&
            result.foreign_inventory.inventory[0].version ==
                "1.0-1" &&
            result.foreign_inventory.inventory[0].reason ==
                InstalledPackageReason::Explicit &&
            stub::info_many_call_history() ==
                std::vector<std::vector<std::string>>{
                    {"latest-root"}} &&
            result.aur.status ==
                UpgradeAllAurPhaseStatus::Completed &&
            result.package_state_change() ==
                PackageStateChange::Changed,
        "Fresh foreign inventory did not drive AUR update");
    expect(stub::aur_execution_calls().size() == 1,
           "AUR-only update did not execute exactly once");
    stub::require_script_consumed();
}

void test_split_package_base_executes_once_with_child_results() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const std::vector<std::string> roots = {
        "split-runtime", "split-tools"};
    stub::set_foreign_inventory(foreign_inventory(roots));
    enqueue_aur_query({{roots[0], "split-suite"},
                       {roots[1], "split-suite"}});
    return_build_plan(
        root_plan({{roots[0], "split-suite"},
                   {roots[1], "split-suite"}}),
        roots);
    stub::enqueue_aur_successes(
        {ArtifactInstallExecutionOutcome::Installed,
         ArtifactInstallExecutionOutcome::SkippedAsNeeded},
        {ArtifactPackageIdentity{"split-debug", "2.0-1"}});

    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "split PackageBase fixture");
    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);

    expect(
        result.is_success() && result.aur.operation_result.has_value() &&
            result.aur.operation_result->execution.has_value() &&
            result.aur.operation_result->execution->work_item_results.size() == 1 &&
            result.aur.operation_result->reduced_operation_result.targets.size() == 2 &&
            result.aur.operation_result->reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.aur.operation_result->reduced_operation_result.targets[1].status ==
                AurUpdateOperationTargetStatus::NoChange,
        "Split PackageBase child outcomes were not reduced independently");

    const AurUpdateWorkItemExecutionResult& work_item =
        result.aur.operation_result->execution->work_item_results.front();
    expect(
        work_item.package_name.empty() &&
            work_item.package_base == "split-suite" &&
            work_item.child_results.size() == 2 &&
            work_item.child_results[0].required_package_name ==
                roots[0] &&
            work_item.child_results[0].status ==
                AurUpdateChildExecutionStatus::Installed &&
            work_item.child_results[1].required_package_name ==
                roots[1] &&
            work_item.child_results[1].status ==
                AurUpdateChildExecutionStatus::SkippedAsNeeded &&
            work_item.unselected_artifacts.size() == 1 &&
            work_item.unselected_artifacts[0].package_name ==
                "split-debug" &&
            work_item.unselected_artifacts[0].full_version ==
                "2.0-1",
        "Split PackageBase execution snapshot lost child or unselected identities");
    expect(
        stub::aur_execution_calls().size() == 1 &&
            stub::aur_execution_calls()[0].package_name.empty() &&
            stub::aur_execution_calls()[0].package_base ==
                "split-suite" &&
            stub::aur_execution_calls()[0].plan_package_names ==
                roots &&
            stub::aur_execution_calls()[0].required_targets.size() ==
                2,
        "Split PackageBase did not use one ordered set-owner call");
    stub::require_script_consumed();
}

void test_all_phases_changed_in_exact_order() {
    stub::reset();
    const std::vector<std::string> sources = {"source-root"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(
        sources,
        config,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}});
    enqueue_post_source_metadata(
        sources,
        LocalPackageVersionSnapshot{{"core", "2.0-1"}});
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::Installed));
    stub::set_foreign_inventory(foreign_inventory({"aur-root"}));
    enqueue_aur_query({{"aur-root", "aur-root"}});
    return_build_plan(root_plan({{"aur-root", "aur-root"}}), {"aur-root"});
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.is_success() &&
            result.package_state_change() ==
                PackageStateChange::Changed &&
            result.system_source.system.package_state_change ==
                PackageStateChange::Changed &&
            result.system_source.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Updated &&
            result.aur.status == UpgradeAllAurPhaseStatus::Completed,
        "All-changed aggregate result differs");

    const std::string system_command = stub::system_commands().front();
    expect(
        event_position(stub::EventKind::SystemCommand, system_command) <
                event_position(
                    stub::EventKind::SourceExecution,
                    "source-root") &&
            event_position(
                stub::EventKind::SourceExecution,
                "source-root") <
                event_position(
                    stub::EventKind::ForeignInventoryQuery,
                    "foreign-inventory") &&
            event_position(
                stub::EventKind::ForeignInventoryQuery,
                "foreign-inventory") <
                event_position(
                    stub::EventKind::AurInfoMany,
                    "aur-info-many") &&
            event_position(
                stub::EventKind::AurInfoMany,
                "aur-info-many") <
                event_position(
                    stub::EventKind::BuildPlanResolution,
                    "build-plan") &&
            event_position(
                stub::EventKind::BuildPlanResolution,
                "build-plan") <
                event_position(
                    stub::EventKind::AurCheckout,
                    "aur-root"),
        "Cross-phase mutation/query order changed");
    stub::require_script_consumed();
}

void test_source_no_change_and_aur_no_change() {
    stub::reset();
    const std::vector<std::string> sources = {"same-source"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::UpToDate));
    stub::set_foreign_inventory(foreign_inventory({"same-aur"}));
    enqueue_aur_query({{"same-aur", "same-aur"}});
    return_build_plan(root_plan({{"same-aur", "same-aur"}}), {"same-aur"});
    stub::enqueue_aur_success(
        ArtifactInstallExecutionOutcome::SkippedAsNeeded);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::NoUpdates &&
            result.aur.status == UpgradeAllAurPhaseStatus::Completed &&
            result.aur.operation_result->reduced_operation_result.targets[0].status ==
                AurUpdateOperationTargetStatus::NoChange &&
            result.package_state_change() ==
                PackageStateChange::NoChange,
        "All-NoChange execution did not satisfy NoUpdates contract");
    const OperationStateProjection projection =
        project_upgrade_all_operation_state(result);
    expect(
        projection.outcome == OperationOutcome::NoOp &&
            projection.no_op_basis ==
                std::optional<NoOpBasis>{
                    NoOpBasis::VerifiedUnchanged},
        "AUR SkippedAsNeeded/NoChange evidence lost its NoOp basis");
    stub::require_script_consumed();
}

void test_duplicate_exclusion_uses_prepared_source_intent() {
    stub::reset();
    const std::vector<std::string> sources = {"duplicate-root"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);

    // LANDMINE(#281): source intentのauthorityはprepared snapshotであり、
    // execution直前のdirectory/identity再観測ではない。
    stub::set_preference_directory(preference_directory({}));
    stub::set_source_identity(
        "duplicate-root",
        source_identity(
            "duplicate-root", "changed-base"));
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::UpToDate));
    stub::set_foreign_inventory(foreign_inventory({"duplicate-root"}));
    enqueue_aur_query({{"duplicate-root", "duplicate-root"}});

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.is_success() && result.has_duplicate_exclusions() &&
            result.duplicate_excluded_aur_targets.size() == 1 &&
            result.duplicate_excluded_aur_targets[0].original_query_plan_index == 0 &&
            result.duplicate_excluded_aur_targets[0].query_entry.installed_name == "duplicate-root" &&
            result.prepared_snapshot.explicit_source_adapter.entries[0].resolved_package_base ==
                std::optional<std::string>{"duplicate-root"},
        "Prepared source intent did not author duplicate exclusion");
    expect(stub::resolver_call_count() == 0 &&
               stub::aur_execution_calls().empty(),
           "Duplicate-excluded root reached AUR build/install");
    stub::require_script_consumed();
}

void test_transitive_external_satisfaction_attribution() {
    stub::reset();
    const std::vector<std::string> sources = {"external-library"};
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = prepare_sources(sources, config);
    enqueue_post_source_metadata(sources);
    stub::enqueue_source_success(
        source_execution(SourceBuildExecutionStatus::UpToDate));
    stub::set_foreign_inventory(foreign_inventory({"application"}));
    enqueue_aur_query({{"application", "application"}});

    BuildPlan plan = root_plan({{"application", "application"}});
    add_aur_dependency(
        plan, "application", "middle-library", "middle-library");
    add_aur_dependency(
        plan,
        "middle-library",
        "external-library",
        "external-library",
        PackageRole::BuildDependency);
    return_build_plan(std::move(plan), {"application"});
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.is_success() && result.has_external_satisfaction() &&
            result.externally_satisfied_aur_build_units.size() == 1,
        "Transitive external satisfaction was not aggregated");
    const UpgradeAllExternallySatisfiedAurBuildUnit& external =
        result.externally_satisfied_aur_build_units.front();
    expect(
        external.operation_unit.package_base == "external-library" &&
            external.operation_unit.package_name ==
                "external-library" &&
            external.operation_unit.plan_package_names ==
                std::vector<std::string>{"external-library"} &&
            external.operation_unit.required_target_attributions
                    .size() == 1 &&
            external.operation_unit.required_target_attributions
                    .front()
                    .required_target.package_name ==
                "external-library" &&
            external.operation_unit.required_target_attributions
                    .front()
                    .required_target.package_base ==
                "external-library" &&
            external.root_correlations.size() == 1 &&
            external.root_correlations[0].original_query_plan_index ==
                0 &&
            external.root_correlations[0].role ==
                UpgradeAllBuildUnitRole::BuildDependency,
        "External root-role correlation was reconstructed incorrectly");
    expect(
        stub::aur_execution_calls().size() == 2 &&
            stub::aur_execution_calls()[0].package_name ==
                "middle-library" &&
            stub::aur_execution_calls()[1].package_name ==
                "application" &&
            std::none_of(
                stub::aur_execution_calls().begin(),
                stub::aur_execution_calls().end(),
                [](const stub::AurExecutionCall& call) {
                    return call.package_name ==
                           "external-library";
                }),
        "Externally satisfied build unit reached AUR execution");
    stub::require_script_consumed();
}

void test_aur_ordinary_failure_partial_and_not_attempted() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const std::vector<std::string> roots = {
        "updated-first", "failed-second", "pending-third"};
    stub::set_foreign_inventory(foreign_inventory(roots));
    enqueue_aur_query({{roots[0], roots[0]},
                       {roots[1], roots[1]},
                       {roots[2], roots[2]}});
    return_build_plan(
        root_plan({{roots[0], roots[0]},
                   {roots[1], roots[1]},
                   {roots[2], roots[2]}}),
        roots);
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);
    stub::enqueue_aur_ordinary_failure("fixture AUR build failure");
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "ordinary AUR failure fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::StoppedOnAurFailure &&
            result.aur.status == UpgradeAllAurPhaseStatus::
                                     StoppedOnWorkItemFailure &&
            result.aur.operation_result.has_value() &&
            result.aur.operation_result->selected_target_results.size() == 3 &&
            result.aur.operation_result->selected_target_results[0].operation_result.status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.aur.operation_result->selected_target_results[1].operation_result.status ==
                AurUpdateOperationTargetStatus::Failed &&
            result.aur.operation_result->selected_target_results[2].operation_result.status ==
                AurUpdateOperationTargetStatus::NotAttempted &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            result.package_state_change() ==
                PackageStateChange::Changed,
        "Ordinary AUR failure lost partial/NotAttempted result");
    expect(stub::aur_execution_calls().size() == 2,
           "Ordinary AUR failure did not stop later work item");
    stub::require_script_consumed();
}

void test_aur_cancellation_partial_and_not_attempted() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const std::vector<std::string> roots = {
        "updated-first", "failed-second", "pending-third"};
    stub::set_foreign_inventory(foreign_inventory(roots));
    enqueue_aur_query({{roots[0], roots[0]},
                       {roots[1], roots[1]},
                       {roots[2], roots[2]}});
    return_build_plan(
        root_plan({{roots[0], roots[0]},
                   {roots[1], roots[1]},
                   {roots[2], roots[2]}}),
        roots);
    stub::enqueue_aur_success(ArtifactInstallExecutionOutcome::Installed);
    stub::enqueue_aur_cancellation();
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "ordinary AUR failure fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status ==
                UpgradeAllOperationStatus::StoppedOnAurCancellation &&
            result.aur.status == UpgradeAllAurPhaseStatus::
                                     StoppedOnWorkItemCancellation &&
            result.aur.operation_result.has_value() &&
            result.aur.operation_result->selected_target_results.size() == 3 &&
            result.aur.operation_result->selected_target_results[0].operation_result.status ==
                AurUpdateOperationTargetStatus::Updated &&
            result.aur.operation_result->selected_target_results[1].operation_result.status ==
                AurUpdateOperationTargetStatus::Cancelled &&
            result.aur.operation_result->selected_target_results[2].operation_result.status ==
                AurUpdateOperationTargetStatus::NotAttempted &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            result.package_state_change() ==
                PackageStateChange::Changed,
        "Ordinary AUR failure lost partial/NotAttempted result");
    expect(stub::aur_execution_calls().size() == 2,
           "Ordinary AUR failure did not stop later work item");
    expect(!result.is_success() && result.issues.empty() && result.diagnostics.empty() && !result.aur.diagnostic &&
               result.system_source.system.status == SystemUpgradePhaseStatus::Completed &&
               result.stopped_phase == UpgradeAllOperationPhase::AurExecution,
           "Cancellation became preparation failure or lost completed system phase");
    const auto presentation = project_upgrade_all_presentation_with_operation_state(result, project_upgrade_all_operation_state(result));
    expect(presentation.full_items.size() == 3, "Cancellation generated generic AUR fallback");
    expect(presentation.full_items[1].diagnostic_class == DiagnosticClass::Cancelled,
           "Cancellation projection became metadata/execution failure");
    stub::require_script_consumed();
}

void test_aur_cleanup_failure_partial_and_not_attempted() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    const std::vector<std::string> roots = {"cleanup-root", "pending-root"};
    stub::set_foreign_inventory(foreign_inventory(roots));
    enqueue_aur_query({{roots[0], roots[0]},
                       {roots[1], roots[1]}});
    return_build_plan(
        root_plan({{roots[0], roots[0]},
                   {roots[1], roots[1]}}),
        roots);
    stub::enqueue_aur_cleanup_failure(
        ArtifactInstallExecutionOutcome::Installed,
        "fixture AUR cleanup failure");
    const AppConfig config;
    PreparedUpgradeAllOperation prepared = take_prepared(
        prepare_upgrade_all_operation(config),
        "AUR cleanup failure fixture");

    UpgradeAllOperationResult result =
        execute_prepared_upgrade_all_operation(
            std::move(prepared), config);
    expect(
        result.status == UpgradeAllOperationStatus::
                             StoppedAfterAurCleanupFailure &&
            result.aur.status == UpgradeAllAurPhaseStatus::
                                     StoppedAfterCleanupFailure &&
            result.has_cleanup_failure() &&
            result.has_partial_completion() &&
            result.has_not_attempted_phase() &&
            result.package_state_change() ==
                PackageStateChange::Changed,
        "AUR cleanup failure lost known package transaction");
    expect(stub::aur_execution_calls().size() == 1,
           "AUR cleanup failure did not stop later work item");
    stub::require_script_consumed();
}

void test_constructed_no_source_no_updates_helper_fixture() {
    // Pure aggregate helper keeps NoRelevantWork distinct from the
    // production system-only authoritative snapshot fixture above.
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::NoUpdates;
    result.stopped_phase = UpgradeAllOperationPhase::None;
    result.system_source.status = SystemSourceUpgradeStatus::Completed;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::None;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change =
        PackageStateChange::NoChange;
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::Completed;
    result.aur.status = UpgradeAllAurPhaseStatus::NoUpdates;
    result.aur.operation_result.emplace();
    FilteredAurUpdateExecutionResult& filtered =
        *result.aur.operation_result;
    filtered.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.reduced_operation_result.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    filtered.reduced_operation_result.status =
        AurUpdateOperationStatus::NoUpdates;

    expect(
        result.is_success() &&
            result.package_state_change() ==
                PackageStateChange::NoChange &&
            !result.has_partial_completion() &&
            !result.has_not_attempted_phase() &&
            !result.has_cleanup_failure() &&
            !result.has_query_failure() &&
            !result.has_planning_issue() &&
            !result.has_duplicate_exclusions() &&
            !result.has_external_satisfaction() &&
            !result.has_inconsistency(),
        "Constructed no-source NoUpdates helper semantics differ");

    const OperationStateProjection projection =
        project_upgrade_all_operation_state(result);
    expect(
        projection.outcome == OperationOutcome::NoOp &&
            projection.no_op_basis ==
                std::optional<NoOpBasis>{
                    NoOpBasis::NoRelevantWork} &&
            projection.package_state.state ==
                PackageStateObservation::VerifiedUnchanged,
        "Verified NoUpdates did not project to NoOp + VerifiedUnchanged");
}

void test_constructed_aur_no_change_noop_basis_fixture() {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::NoUpdates;
    result.stopped_phase = UpgradeAllOperationPhase::None;
    result.system_source.status = SystemSourceUpgradeStatus::Completed;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::None;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change =
        PackageStateChange::NoChange;
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::Completed;
    result.aur.status = UpgradeAllAurPhaseStatus::NoUpdates;
    result.aur.operation_result.emplace();
    result.aur.operation_result->devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    result.aur.operation_result->preflight
        .devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    result.aur.operation_result->preparation
        .devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    AurUpdateOperationResult& aur =
        result.aur.operation_result->reduced_operation_result;
    aur.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    aur.status = AurUpdateOperationStatus::NoUpdates;
    AurUpdateOperationTargetResult target;
    target.update.installed_name = "unchanged-aur";
    target.status = AurUpdateOperationTargetStatus::NoChange;
    aur.targets.push_back(std::move(target));

    const OperationStateProjection projection =
        project_upgrade_all_operation_state(result);
    expect(
        projection.outcome == OperationOutcome::NoOp &&
            projection.no_op_basis ==
                std::optional<NoOpBasis>{
                    NoOpBasis::VerifiedUnchanged},
        "AUR-only NoChange evidence was treated as no relevant work");
}

UpgradeAllOperationResult make_constructed_completed_helper_fixture(
    PackageStateChange system_package_state) {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::Completed;
    result.stopped_phase = UpgradeAllOperationPhase::None;
    result.system_source.status = SystemSourceUpgradeStatus::Completed;
    result.system_source.stopped_phase = SystemSourceUpgradePhase::None;
    result.system_source.system.status = SystemUpgradePhaseStatus::Completed;
    result.system_source.system.package_state_change = system_package_state;
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::Completed;
    result.aur.status = UpgradeAllAurPhaseStatus::Completed;
    result.aur.operation_result.emplace();
    result.aur.operation_result->devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    result.aur.operation_result->preflight
        .devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    result.aur.operation_result->preparation
        .devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    result.aur.operation_result->reduced_operation_result
        .devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    result.aur.operation_result->reduced_operation_result.status =
        AurUpdateOperationStatus::Completed;
    result.aur.operation_result->reduced_operation_result.execution_status =
        AurUpdateInvocationExecutionStatus::Completed;
    return result;
}

enum class PhaseObservationFixture {
    Changed,
    VerifiedUnchanged,
    Unverified,
    NotObserved,
};

UpgradeAllOperationResult make_phase_observation_fixture(
    PhaseObservationFixture system_source_observation,
    PhaseObservationFixture aur_observation) {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);

    switch(system_source_observation) {
        case PhaseObservationFixture::Changed:
            result.system_source.system.package_state_change =
                PackageStateChange::Changed;
            break;
        case PhaseObservationFixture::VerifiedUnchanged:
            break;
        case PhaseObservationFixture::Unverified:
            result.system_source.system.package_state_change =
                PackageStateChange::Unknown;
            break;
        case PhaseObservationFixture::NotObserved:
            result.system_source.status =
                SystemSourceUpgradeStatus::BlockedBeforeMutation;
            result.system_source.stopped_phase =
                SystemSourceUpgradePhase::Preparation;
            result.system_source.system.status =
                SystemUpgradePhaseStatus::NotAttempted;
            break;
    }

    switch(aur_observation) {
        case PhaseObservationFixture::Changed:
        case PhaseObservationFixture::Unverified: {
            SelectedRepositoryProviderTransactionResult& provider_transaction =
                result.aur.operation_result->reduced_operation_result.selected_repository_provider_transaction;
            provider_transaction.status =
                SelectedRepositoryProviderTransactionStatus::Succeeded;
            provider_transaction.selected_providers = {
                ProvidedDependency::from_repository(
                    "extra", "phase-provider")};
            provider_transaction.package_state_change =
                aur_observation == PhaseObservationFixture::Changed
                    ? PackageStateChange::Changed
                    : PackageStateChange::Unknown;
            provider_transaction.command_exit_status = 0;
            break;
        }
        case PhaseObservationFixture::VerifiedUnchanged:
            break;
        case PhaseObservationFixture::NotObserved:
            result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
            result.aur.not_attempted_reason =
                UpgradeAllNotAttemptedReason::SystemFailure;
            result.aur.operation_result.reset();
            break;
    }

    return result;
}

void test_phase_package_state_observation_matrix() {
    struct PhaseObservationCase {
        const char* name;
        PhaseObservationFixture system_source_input;
        PhaseObservationFixture aur_input;
        PackageStateObservation expected_system_source;
        PackageStateObservation expected_aur;
        PackageStateObservation expected_aggregate;
    };

    const std::vector<PhaseObservationCase> cases = {
        {"system changed, AUR unchanged",
         PhaseObservationFixture::Changed,
         PhaseObservationFixture::VerifiedUnchanged,
         PackageStateObservation::Changed,
         PackageStateObservation::VerifiedUnchanged,
         PackageStateObservation::Changed},
        {"system unchanged, AUR changed",
         PhaseObservationFixture::VerifiedUnchanged,
         PhaseObservationFixture::Changed,
         PackageStateObservation::VerifiedUnchanged,
         PackageStateObservation::Changed,
         PackageStateObservation::Changed},
        {"both unchanged",
         PhaseObservationFixture::VerifiedUnchanged,
         PhaseObservationFixture::VerifiedUnchanged,
         PackageStateObservation::VerifiedUnchanged,
         PackageStateObservation::VerifiedUnchanged,
         PackageStateObservation::VerifiedUnchanged},
        {"both changed",
         PhaseObservationFixture::Changed,
         PhaseObservationFixture::Changed,
         PackageStateObservation::Changed,
         PackageStateObservation::Changed,
         PackageStateObservation::Changed},
        {"system unverified, AUR unchanged",
         PhaseObservationFixture::Unverified,
         PhaseObservationFixture::VerifiedUnchanged,
         PackageStateObservation::Unverified,
         PackageStateObservation::VerifiedUnchanged,
         PackageStateObservation::Unverified},
        {"system unchanged, AUR unverified",
         PhaseObservationFixture::VerifiedUnchanged,
         PhaseObservationFixture::Unverified,
         PackageStateObservation::VerifiedUnchanged,
         PackageStateObservation::Unverified,
         PackageStateObservation::Unverified},
        {"system changed, AUR unverified",
         PhaseObservationFixture::Changed,
         PhaseObservationFixture::Unverified,
         PackageStateObservation::Changed,
         PackageStateObservation::Unverified,
         PackageStateObservation::Changed},
        {"system unverified, AUR changed",
         PhaseObservationFixture::Unverified,
         PhaseObservationFixture::Changed,
         PackageStateObservation::Unverified,
         PackageStateObservation::Changed,
         PackageStateObservation::Changed},
        {"system changed, AUR not observed",
         PhaseObservationFixture::Changed,
         PhaseObservationFixture::NotObserved,
         PackageStateObservation::Changed,
         PackageStateObservation::NotObserved,
         PackageStateObservation::Changed},
        {"system not observed, AUR changed",
         PhaseObservationFixture::NotObserved,
         PhaseObservationFixture::Changed,
         PackageStateObservation::NotObserved,
         PackageStateObservation::Changed,
         PackageStateObservation::Changed},
    };

    for(const PhaseObservationCase& test_case : cases) {
        const UpgradeAllOperationResult result =
            make_phase_observation_fixture(
                test_case.system_source_input,
                test_case.aur_input);
        const UpgradeAllPhasePackageStateObservations phase_observations =
            project_upgrade_all_phase_package_state_observations(result);
        const OperationStateProjection aggregate_observation =
            project_upgrade_all_operation_state(result);

        expect(
            phase_observations.system_source.state ==
                    test_case.expected_system_source &&
                phase_observations.aur.state ==
                    test_case.expected_aur &&
                aggregate_observation.package_state.state ==
                    test_case.expected_aggregate,
            std::string("Phase package-state observation differs: ") +
                test_case.name);
        if(test_case.expected_system_source ==
           PackageStateObservation::NotObserved) {
            expect(
                result.system_source.package_state_change() ==
                        PackageStateChange::NoChange &&
                    phase_observations.system_source.reason ==
                        ObservationReason::PhaseNotAttempted,
                "Unattempted system/source default NoChange was presented as verified");
        }
        if(test_case.expected_aur ==
           PackageStateObservation::NotObserved) {
            expect(
                phase_observations.aur.reason ==
                    ObservationReason::PhaseNotAttempted,
                "Unattempted AUR phase lost its NotObserved reason");
        }
    }
}

void test_constructed_completed_unknown_success_fixture() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::Unknown);

    expect(
        result.is_success() &&
            result.package_state_change() ==
                PackageStateChange::Unknown,
        "Completed + Unknown package state must remain successful");

    const OperationStateProjection projection =
        project_upgrade_all_operation_state(result);
    expect(
        projection.outcome == OperationOutcome::Succeeded &&
            projection.outcome != OperationOutcome::NoOp &&
            projection.package_state.state ==
                PackageStateObservation::Unverified &&
            projection.package_state.reason ==
                ObservationReason::ObservationNotPrepared,
        "Completed + Unknown was flattened to NoOp or failure");
}

void test_constructed_provider_transaction_unknown_reaches_aggregate() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    SelectedRepositoryProviderTransactionResult& provider_transaction =
        result.aur.operation_result->reduced_operation_result.selected_repository_provider_transaction;
    provider_transaction.status =
        SelectedRepositoryProviderTransactionStatus::Succeeded;
    provider_transaction.selected_providers = {
        ProvidedDependency::from_repository("extra", "provider-pkg")};
    provider_transaction.package_state_change = PackageStateChange::Unknown;
    provider_transaction.command_exit_status = 0;

    expect(
        result.is_success() &&
            result.package_state_change() ==
                PackageStateChange::Unknown,
        "AUR provider transaction Unknown state was lost by the aggregate");
}

void test_constructed_success_metadata_fixture() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    result.duplicate_excluded_aur_targets.emplace_back();
    result.externally_satisfied_aur_build_units.emplace_back();

    expect(
        result.is_success() && result.has_duplicate_exclusions() &&
            result.has_external_satisfaction(),
        "Duplicate exclusion or external satisfaction changed success");
}

void test_constructed_completed_query_failure_is_not_success() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    result.aur.operation_result->query_result.recoverable_failures.push_back(
        AurUpdateQueryFailure{{"fixture-query"}, "fixture query failure"});

    expect(
        !result.is_success() && result.has_query_failure(),
        "Completed result hid a recoverable query failure");
}

void test_constructed_completed_direct_inventory_failure_is_not_success() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    result.foreign_inventory.failure = PackageMetadataFailure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "fixture direct foreign inventory failure"};
    result.foreign_inventory.diagnostic =
        "fixture direct foreign inventory failure";

    expect(
        !result.is_success() && result.issues.empty(),
        "Completed result depended on an aggregate issue copy to detect a direct inventory failure");
}

void test_constructed_completed_planning_issue_is_not_success() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    UpgradeAllPlanningIssue issue;
    issue.kind =
        UpgradeAllPlanningIssueKind::ConflictingExplicitPackageBase;
    issue.package_base = "fixture-conflict";
    result.aur.operation_result->upgrade_all_plan.issues.push_back(
        std::move(issue));

    expect(
        !result.is_success() && result.has_planning_issue(),
        "Completed result hid a planning issue");
}

void test_constructed_completed_inconsistency_is_not_success() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    UpgradeAllOperationIssue issue;
    issue.kind = UpgradeAllOperationIssueKind::
        ExternalSatisfactionCorrelationInconsistent;
    issue.phase = UpgradeAllOperationPhase::Reduction;
    issue.diagnostic = "fixture aggregate inconsistency";
    result.issues.push_back(std::move(issue));

    expect(
        !result.is_success() && result.has_inconsistency(),
        "Completed result hid an aggregate inconsistency");
}

void test_constructed_completed_cleanup_failure_is_not_success() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    AurUpdateOperationTargetResult target;
    target.status =
        AurUpdateOperationTargetStatus::NoChangeCleanupFailed;
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        std::move(target));

    expect(
        !result.is_success() && result.has_cleanup_failure(),
        "Completed result hid a cleanup failure");
}

void test_constructed_completed_not_attempted_is_not_success() {
    UpgradeAllOperationResult result =
        make_constructed_completed_helper_fixture(
            PackageStateChange::NoChange);
    AurUpdateOperationTargetResult target;
    target.status = AurUpdateOperationTargetStatus::NotAttempted;
    result.aur.operation_result->reduced_operation_result.targets.push_back(
        std::move(target));

    expect(
        !result.is_success() && result.has_not_attempted_phase(),
        "Completed result hid a NotAttempted target");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        test_aur_cancellation_partial_and_not_attempted();
        TemporaryCacheEnvironment cache_environment;
        run_case(
            "empty registered-source preparation snapshot",
            test_empty_source_preparation_snapshot);
        run_case(
            "prepared projection authority tracks nested source work",
            test_prepared_projection_authority_tracks_nested_source_work);
        run_case(
            "preparation and AUR preflight are cache-free",
            test_preparation_and_aur_preflight_do_not_activate_cache);
        run_case(
            "read-only AUR RequiresCheck preflight stays blocked",
            test_read_only_aur_preflight_requires_check_stays_blocked);
        run_case(
            "cache replacement after system",
            test_cache_replacement_after_system_blocks_inventory_and_aur);
        run_case(
            "strict preference absence",
            test_strict_preference_absence_blocks_without_mutation);
        run_case(
            "strict preference read failure",
            test_strict_preference_read_failure_blocks_without_mutation);
        run_case(
            "non-regular preference snapshot hard error",
            test_nonregular_preference_snapshot_blocks_aggregate);
        run_case(
            "invalid preference snapshot hard error",
            test_invalid_preference_snapshot_blocks_aggregate);
        run_case(
            "system/source preparation blocker",
            test_system_source_preparation_blocker_is_aggregate_blocker);
        run_case(
            "aggregate option mismatch",
            test_option_mismatch_rejected_before_system_mutation);
        run_case(
            "aggregate capability replay",
            test_capability_replay_rejected_before_second_mutation);
        run_case(
            "outer source snapshot mismatch",
            test_outer_source_snapshot_mismatch_is_rejected);
        run_case(
            "explicit source adapter correlation mismatch",
            test_explicit_source_adapter_correlation_mismatch_is_rejected);
        run_case(
            "nested system/source correlation mismatch",
            test_nested_system_source_correlation_mismatch_is_rejected);
        run_case(
            "unexpected exception after system start",
            test_unexpected_exception_after_system_start_is_not_unattempted);
        run_case(
            "unexpected exception preserves system completion",
            test_unexpected_exception_preserves_system_completion);
        run_case(
            "unexpected exception after source start",
            test_unexpected_exception_after_source_start_is_incomplete);
        run_case(
            "unexpected exception preserves source result",
            test_unexpected_exception_preserves_recorded_source_result);
        run_case("system failure fail-fast", test_system_failure_is_fail_fast);
        run_case(
            "VirtualBox post-system-failure correlation",
            test_virtualbox_system_failure_retains_possible_correlation);
        run_case(
            "partial post-system-failure observation",
            test_partial_system_failure_observation_is_retained);
        run_case(
            "failed post-system-failure observation",
            test_failed_system_failure_observation_is_retained);
        run_case(
            "post-system-failure observer bad_alloc",
            test_system_failure_observer_bad_alloc_is_secondary);
        run_case(
            "post-system-failure observer std::exception",
            test_system_failure_observer_runtime_error_is_secondary);
        run_case(
            "post-system-failure observer unknown exception",
            test_system_failure_observer_unknown_exception_is_secondary);
        run_case(
            "nested cache seed failure",
            test_nested_cache_seed_failure_is_typed);
        run_case(
            "nested cache activation failure",
            test_nested_cache_activation_failure_is_typed);
        run_case(
            "registered-source cache failure",
            test_registered_source_cache_failure_is_typed);
        run_case(
            "first source failure fail-fast",
            test_first_source_failure_stops_remaining_and_aur);
        run_case(
            "later source failure partial completion",
            test_later_source_failure_preserves_prior_update);
        run_case(
            "source cleanup failure",
            test_source_cleanup_failure_stops_before_inventory);
        run_case(
            "inventory configuration failure",
            test_foreign_inventory_configuration_failure);
        run_case(
            "foreign inventory read failure",
            test_foreign_inventory_read_failure);
        run_case(
            "cache replacement during foreign inventory",
            test_cache_replacement_during_inventory_blocks_aur_query);
        run_case(
            "cache replacement during AUR query",
            test_cache_replacement_during_aur_query_blocks_preparation);
        run_case(
            "cache replacement during filtered planning",
            test_cache_replacement_during_filtered_planning_blocks_database);
        run_case(
            "filtered preparation failure precedence",
            test_filtered_preparation_failure_keeps_stage_precedence);
        run_case(
            "strict preference failure precedes post-stage cache drift",
            test_strict_preference_failure_precedes_post_stage_cache_drift);
        run_case(
            "Pacman database failure precedes post-stage cache drift",
            test_pacman_database_failure_precedes_post_stage_cache_drift);
        run_case(
            "executable preparation cache drift",
            test_executable_preparation_cache_drift_blocks_aur_execution);
        run_case(
            "recoverable AUR query failure",
            test_recoverable_aur_query_failure_blocks_mutation);
        run_case(
            "devel RequiresCheck preserves prior phases",
            test_devel_requires_check_preserves_prior_phases_and_blocks_aur);
        run_case(
            "total AUR query failure",
            test_total_aur_query_failure_blocks_mutation);
        run_case(
            "repository source key derivation",
            test_repository_source_key_is_derived_from_package_base);
        run_case(
            "filtered mapping issue",
            test_mapping_issue_blocks_aur_mutation);
        run_case(
            "AUR preflight blocker",
            test_preflight_blocker_stops_before_preparation_io);
        run_case(
            "AUR preparation blocker",
            test_preparation_blocker_stops_before_aur_mutation);
        run_case("NoUpdates success", test_no_updates_success_contract);
        run_case(
            "system-only authoritative unchanged observation",
            test_system_only_authoritative_unchanged_observation);
        run_case(
            "system-only changed observation",
            test_system_only_changed_observation);
        run_case(
            "system-only observation failure remains Unverified",
            test_system_only_observation_failure_remains_unverified);
        run_case("system Changed only", test_system_changed_only);
        run_case("source Updated only", test_source_updated_only);
        run_case(
            "registered PackageBase result and composition",
            test_registered_package_base_result_and_composition);
        run_case(
            "AUR Updated only and fresh inventory",
            test_aur_updated_only_and_fresh_inventory);
        run_case(
            "split PackageBase child execution",
            test_split_package_base_executes_once_with_child_results);
        run_case(
            "all phases Changed and exact order",
            test_all_phases_changed_in_exact_order);
        run_case(
            "source and AUR NoChange",
            test_source_no_change_and_aur_no_change);
        run_case(
            "prepared source intent duplicate exclusion",
            test_duplicate_exclusion_uses_prepared_source_intent);
        run_case(
            "transitive external satisfaction",
            test_transitive_external_satisfaction_attribution);
        run_case(
            "ordinary AUR failure partial completion",
            test_aur_ordinary_failure_partial_and_not_attempted);
        run_case(
            "AUR cleanup failure partial completion",
            test_aur_cleanup_failure_partial_and_not_attempted);
        run_case(
            "constructed no-source NoUpdates helpers",
            test_constructed_no_source_no_updates_helper_fixture);
        run_case(
            "constructed AUR NoChange NoOp basis",
            test_constructed_aur_no_change_noop_basis_fixture);
        run_case(
            "phase package-state observation matrix",
            test_phase_package_state_observation_matrix);
        run_case(
            "constructed Completed Unknown success",
            test_constructed_completed_unknown_success_fixture);
        run_case(
            "constructed provider transaction Unknown aggregate",
            test_constructed_provider_transaction_unknown_reaches_aggregate);
        run_case(
            "constructed success metadata",
            test_constructed_success_metadata_fixture);
        run_case(
            "constructed Completed query failure",
            test_constructed_completed_query_failure_is_not_success);
        run_case(
            "constructed Completed direct inventory failure",
            test_constructed_completed_direct_inventory_failure_is_not_success);
        run_case(
            "constructed Completed planning issue",
            test_constructed_completed_planning_issue_is_not_success);
        run_case(
            "constructed Completed inconsistency",
            test_constructed_completed_inconsistency_is_not_success);
        run_case(
            "constructed Completed cleanup failure",
            test_constructed_completed_cleanup_failure_is_not_success);
        run_case(
            "constructed Completed NotAttempted",
            test_constructed_completed_not_attempted_is_not_success);
    } catch(const std::exception& error) {
        std::cerr << "upgrade_all_operation_test: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
