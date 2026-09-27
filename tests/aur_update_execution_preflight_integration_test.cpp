#include "aur_update_execution_preflight.hpp"
#include "alpm_stub.hpp"
#include "integration_stub.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace stub = aur_update_execution_preflight_integration_stub;

constexpr const char* DATABASE_PATH_COMMAND =
    "pacman-conf --verbose RootDir DBPath 2>/dev/null";
constexpr const char* REPOSITORY_LIST_COMMAND =
    "pacman-conf --repo-list 2>/dev/null";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan) {
    return ::resolve_aur_update_execution_preflight(
        update_plan, DevelRequiresCheckPolicy::BlockOperation);
}

AurUpdatePlanEntry update_entry(
    const std::string& package_name, InstalledPackageReason reason) {
    return AurUpdatePlanEntry{
        package_name,
        "1.0-1",
        reason,
        AurUpdateRemotePackage{
            package_name,
            package_name,
            "2.0-1",
            AurVersionRelation::NewerThanInstalled},
        AurUpdateClassification::UpdateAvailable};
}

AurUpdatePlanEntry requires_check_entry(
    const std::string& package_name) {
    return classify_aur_update(AurUpdatePlanInput{
        package_name,
        "1.0-1",
        InstalledPackageReason::Dependency,
        AurUpdateRemotePackage{
            package_name,
            package_name,
            "1.0-1",
            AurVersionRelation::SameAsInstalled}});
}

bool has_reason(
    const AurUpdateExecutionTarget& target,
    AurUpdateExecutionReason reason) {
    for(const auto& issue : target.issues) {
        if(issue.reason == reason) return true;
    }
    return false;
}

const AurUpdateExecutionIssue& relation_issue(
    const AurUpdateExecutionTarget& target) {
    const auto found = std::find_if(
        target.issues.begin(), target.issues.end(),
        [](const AurUpdateExecutionIssue& issue) {
            return issue.relation_reason.has_value();
        });
    if(found == target.issues.end()) {
        throw std::runtime_error("Typed relation preflight issue is missing");
    }
    return *found;
}

const AurUpdateExecutionTarget& target_by_name(
    const AurUpdateExecutionPreflight& preflight,
    std::string_view package_name) {
    const auto found = std::find_if(
        preflight.targets.begin(), preflight.targets.end(),
        [&package_name](const AurUpdateExecutionTarget& target) {
            return target.update.installed_name == package_name;
        });
    if(found == preflight.targets.end()) {
        throw std::runtime_error(
            "Integration preflight target is missing: " +
            std::string{package_name});
    }
    return *found;
}

class FixtureRoot {
public:
    explicit FixtureRoot(const std::string& case_name)
        : original_working_directory_(fs::current_path()),
          root_(fs::temp_directory_path() /
                ("moguet-aur-update-preflight-integration-" + case_name + "-" +
                 std::to_string(static_cast<long long>(getpid())))) {
        if(fs::exists(root_)) {
            throw std::runtime_error("Integration fixture root already exists: " + root_.string());
        }
        fs::create_directories(root_);
    }

    FixtureRoot(const FixtureRoot&) = delete;
    FixtureRoot& operator=(const FixtureRoot&) = delete;

    ~FixtureRoot() noexcept {
        std::error_code error;
        fs::current_path(original_working_directory_, error);
        fs::remove_all(root_, error);
    }

    const fs::path& root() const noexcept {
        return root_;
    }

    void enter() {
        fs::current_path(root_);
        setenv("HOME", root_.c_str(), 1);
        setenv("XDG_CACHE_HOME", root_.c_str(), 1);
        setenv("TMPDIR", root_.c_str(), 1);
    }

    std::vector<fs::path> relative_paths() const {
        std::vector<fs::path> paths;
        for(const auto& entry : fs::recursive_directory_iterator(root_)) {
            paths.push_back(fs::relative(entry.path(), root_));
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

private:
    fs::path original_working_directory_;
    fs::path root_;
};

void expect_no_forbidden_operations() {
    expect(
        stub::forbidden_operation_count() == 0,
        "Preflight integration crossed a forbidden process boundary");
}

void test_simple_roots_and_combined_resolution() {
    FixtureRoot fixture("simple");
    const std::vector<fs::path> paths_before = fixture.relative_paths();
    fixture.enter();

    stub::reset();
    AurUpdateExecutionPreflight explicit_preflight =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "explicit-root", InstalledPackageReason::Explicit)}});
    expect(
        explicit_preflight.targets.size() == 1 &&
            explicit_preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Executable,
        "Explicit integration root was not executable");
    expect(
        explicit_preflight.targets.front().desired_install_reason ==
            DesiredInstallReason::Explicit,
        "Explicit integration root lost its install reason");
    expect(
        stub::strict_info_calls() == std::vector<std::string>{"explicit-root"},
        "Explicit integration root used an unexpected AUR query sequence");
    expect(stub::captured_commands().empty(), "Dependency-free root queried pacman metadata");

    stub::reset();
    AurUpdateExecutionPreflight dependency_preflight =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "dependency-root", InstalledPackageReason::Dependency)}});
    expect(
        dependency_preflight.targets.front().desired_install_reason ==
            DesiredInstallReason::Dependency,
        "Dependency integration root was promoted to Explicit");

    stub::reset();
    AurUpdateExecutionPreflight combined =
        resolve_aur_update_execution_preflight(AurUpdatePlan{{
            update_entry("combined-root-a", InstalledPackageReason::Explicit),
            update_entry("combined-root-b", InstalledPackageReason::Dependency),
        }});
    expect(combined.build_plan.has_value(), "Combined integration plan is missing BuildPlan");
    expect(
        combined.build_plan->root_targets ==
            std::vector<RootTargetIdentity>{
                {0, "combined-root-a"},
                {1, "combined-root-b"},
            },
        "Combined integration root identity/order differs");
    expect(
        stub::strict_info_calls() ==
            std::vector<std::string>{"combined-root-a", "combined-root-b"},
        "Combined integration plan did not resolve candidate roots as one ordered set");
    expect(can_execute(combined), "Combined integration plan could not execute");

    stub::reset();
    AurUpdatePlanEntry skipped = update_entry(
        "skipped-root", InstalledPackageReason::Explicit);
    skipped.classification = AurUpdateClassification::UpToDate;
    AurUpdateExecutionPreflight skip_only =
        resolve_aur_update_execution_preflight(AurUpdatePlan{{skipped}});
    expect(!can_execute(skip_only), "Skip-only integration plan could execute");
    expect(stub::strict_info_calls().empty(), "Skip-only integration plan invoked resolver");
    expect(stub::captured_commands().empty(), "Skip-only integration plan invoked process command");
    expect(
        fixture.relative_paths() == paths_before,
        "Executable or skip-only preflight created a filesystem path");
    expect_no_forbidden_operations();
}

void test_skip_independent_target_uses_real_resolver_reentry() {
    FixtureRoot fixture("requires-check-localization");
    const fs::path database_path = fixture.root() / "database";
    const fs::path sync_directory = database_path / "sync";
    fs::create_directories(sync_directory);
    std::ofstream(sync_directory / "core.db").close();
    const std::vector<fs::path> paths_before = fixture.relative_paths();
    fixture.enter();

    const auto arrange_repository_observation = [&] {
        package_metadata_test_stub::set_empty_package_cache();
        stub::enqueue_captured_command_result(
            DATABASE_PATH_COMMAND,
            CapturedCommandResult{
                "RootDir = " + fixture.root().string() +
                    "\nDBPath = " + database_path.string() + "\n",
                0});
        stub::enqueue_captured_command_result(
            REPOSITORY_LIST_COMMAND,
            CapturedCommandResult{"core\n", 0});
    };

    stub::reset();
    arrange_repository_observation();
    const AurUpdateExecutionPreflight required =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                update_entry(
                    "localization-required-root",
                    InstalledPackageReason::Explicit),
                requires_check_entry(
                    "localization-required-git"),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    const AurUpdateExecutionTarget& required_root = target_by_name(
        required, "localization-required-root");
    const AurUpdateExecutionTarget& required_devel = target_by_name(
        required, "localization-required-git");
    const auto blocker = std::find_if(
        required_root.issues.begin(), required_root.issues.end(),
        [](const AurUpdateExecutionIssue& issue) {
            return issue.reason == AurUpdateExecutionReason::
                                       RequiredDevelTargetRequiresCheck;
        });
    expect(
        required.build_plan.has_value() &&
            required.build_plan->root_targets ==
                std::vector<RootTargetIdentity>{
                    {0, "localization-required-root"}} &&
            std::any_of(
                required.build_plan->dependency_edges.begin(),
                required.build_plan->dependency_edges.end(),
                [](const BuildPlanDependencyEdge& edge) {
                    return edge.parent_package_name ==
                               "localization-required-root" &&
                           edge.resolved_package_name ==
                               std::optional<std::string>{
                                   "localization-required-git"};
                }) &&
            required_root.status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            blocker != required_root.issues.end() &&
            blocker->required_devel_target_blocker.has_value() &&
            blocker->required_devel_target_blocker
                    ->requires_check_update_plan_index == 1 &&
            blocker->required_devel_target_blocker->relation ==
                AurUpdateRequiredDevelTargetRelation::
                    AurExactDependency &&
            required_devel.status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            required_devel.skip_kind ==
                AurUpdateExecutionSkipKind::
                    RequiredDevelRequiresCheck &&
            !required_devel.build_plan_root_index.has_value() &&
            !required_devel.desired_install_reason.has_value() &&
            !can_execute(required),
        "Real resolver did not re-enter or block a required RequiresCheck dependency");
    expect(
        stub::strict_info_calls() ==
            std::vector<std::string>{
                "localization-required-root",
                "localization-required-git"},
        "Real required RequiresCheck fixture used an unexpected AUR resolver sequence");
    expect_no_forbidden_operations();

    stub::reset();
    arrange_repository_observation();
    const AurUpdateExecutionPreflight independent =
        ::resolve_aur_update_execution_preflight(
            AurUpdatePlan{{
                update_entry(
                    "localization-independent-root",
                    InstalledPackageReason::Explicit),
                requires_check_entry(
                    "localization-independent-git"),
            }},
            DevelRequiresCheckPolicy::SkipIndependentTarget);
    const AurUpdateExecutionTarget& independent_root = target_by_name(
        independent, "localization-independent-root");
    const AurUpdateExecutionTarget& independent_devel = target_by_name(
        independent, "localization-independent-git");
    expect(
        independent.build_plan.has_value() &&
            independent.build_plan->root_targets ==
                std::vector<RootTargetIdentity>{
                    {0, "localization-independent-root"}} &&
            independent_root.status ==
                AurUpdateExecutionTargetStatus::Executable &&
            independent_devel.status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            independent_devel.skip_kind ==
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck &&
            !independent_devel.build_plan_root_index.has_value() &&
            !independent_devel.desired_install_reason.has_value() &&
            can_execute(independent),
        "Real resolver did not preserve an independent RequiresCheck skip");
    expect(
        stub::strict_info_calls() ==
            std::vector<std::string>{
                "localization-independent-root"},
        "Independent RequiresCheck leaked into real resolver root authority");
    expect(
        fixture.relative_paths() == paths_before,
        "RequiresCheck real-resolver preflight mutated the filesystem fixture");
    expect_no_forbidden_operations();
}

void test_repository_metadata_failure_is_fail_closed() {
    FixtureRoot fixture("repository-failure");
    const std::vector<fs::path> paths_before = fixture.relative_paths();
    fixture.enter();

    stub::reset();
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{"", 41});

    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "repository-failure-root",
                InstalledPackageReason::Explicit)}});

    expect(
        preflight.targets.front().status ==
            AurUpdateExecutionTargetStatus::Incomplete,
        "Repository metadata failure did not block integration preflight");
    expect(
        has_reason(
            preflight.targets.front(),
            AurUpdateExecutionReason::RepositoryMetadataUnavailable),
        "Repository metadata failure lost its typed preflight reason");
    expect(
        stub::strict_info_calls() ==
            std::vector<std::string>{"repository-failure-root"},
        "Repository metadata failure fell back to AUR exact metadata");
    expect(stub::strict_provider_search_calls().empty(), "Repository failure fell back to AUR provider search");
    expect(
        stub::captured_commands() == std::vector<std::string>{DATABASE_PATH_COMMAND},
        "Repository configuration failure did not stop at its first failed command");
    expect(
        fixture.relative_paths() == paths_before,
        "Repository failure preflight created a filesystem path");
    expect_no_forbidden_operations();
}

void test_ordinary_aur_dependency_failure_is_owned_and_read_only() {
    stub::reset();
    FixtureRoot fixture("aur-failure");
    const fs::path database_path = fixture.root() / "database";
    const fs::path sync_directory = database_path / "sync";
    const fs::path repository_database = sync_directory / "core.db";
    fs::create_directories(sync_directory);
    std::ofstream(repository_database).close();

    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{
            "RootDir = " + fixture.root().string() + "\nDBPath = " +
                database_path.string() + "\n",
            0});
    stub::enqueue_captured_command_result(
        REPOSITORY_LIST_COMMAND,
        CapturedCommandResult{"core\n", 0});
    const std::vector<fs::path> paths_before = fixture.relative_paths();
    fixture.enter();
    AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "aur-failure-root",
                InstalledPackageReason::Explicit)}});
    const std::vector<fs::path> paths_after = fixture.relative_paths();

    expect(
        preflight.targets.front().status ==
            AurUpdateExecutionTargetStatus::Incomplete,
        "Ordinary AUR dependency failure did not block integration preflight");
    expect(
        has_reason(
            preflight.targets.front(),
            AurUpdateExecutionReason::AurDependencyMetadataUnavailable),
        "Ordinary AUR dependency failure lost its typed reason");
    expect(paths_after == paths_before, "Preflight created a path outside the repository DB fixture");
    expect(
        stub::captured_commands() == std::vector<std::string>{
                                         DATABASE_PATH_COMMAND,
                                         REPOSITORY_LIST_COMMAND,
                                     },
        "Strict repository read used an unexpected command sequence");
    expect_no_forbidden_operations();
}

void test_relation_inventory_is_snapshotted_once_and_no_match_releases_guard() {
    FixtureRoot fixture("relation-assessment");
    fixture.enter();
    const fs::path database_path = fixture.root() / "database";

    stub::reset();
    package_metadata_test_stub::set_local_packages({{"installed-conflict", "1", ALPM_PKG_REASON_EXPLICIT, {}}});
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{
            "RootDir = " + fixture.root().string() +
                "\nDBPath = " + database_path.string() + "\n",
            0});
    const AurUpdateExecutionPreflight conflict =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "relation-installed-root",
                InstalledPackageReason::Explicit)}});
    expect(
        conflict.targets.front().status ==
            AurUpdateExecutionTargetStatus::Unsupported,
        "Installed relation conflict did not stop production preflight");
    const AurUpdateExecutionIssue& conflict_issue =
        relation_issue(conflict.targets.front());
    expect(
        conflict_issue.relation_reason->assessment.kind ==
                PackageRelationAssessmentKind::
                    ConfirmedInstalledConflict &&
            conflict_issue.relation_reason->assessment
                    .attributed_package_evidence
                    ->observed_package.package_name ==
                "installed-conflict" &&
            package_metadata_test_stub::initialize_call_count() == 1 &&
            package_metadata_test_stub::local_database_call_count() ==
                1 &&
            package_metadata_test_stub::package_cache_call_count() ==
                1 &&
            package_metadata_test_stub::release_call_count() == 1 &&
            stub::captured_commands() ==
                std::vector<std::string>{DATABASE_PATH_COMMAND},
        "Installed relation inventory was repeated or lost attribution");
    expect(
        conflict_issue.diagnostic.find(
            "Installed conflict confirmed") != std::string::npos &&
            conflict_issue.diagnostic.find(
                "matched installed package installed-conflict") !=
                std::string::npos &&
            conflict_issue.diagnostic.find(
                "build/install is blocked before mutation") !=
                std::string::npos,
        "Installed relation preflight diagnostic lost public attribution");
    expect_no_forbidden_operations();

    stub::reset();
    package_metadata_test_stub::set_empty_package_cache();
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{
            "RootDir = " + fixture.root().string() +
                "\nDBPath = " + database_path.string() + "\n",
            0});
    const AurUpdateExecutionPreflight no_match =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "relation-no-match-root",
                InstalledPackageReason::Explicit)}});
    expect(
        no_match.targets.front().status ==
                AurUpdateExecutionTargetStatus::Executable &&
            no_match.build_plan.has_value() &&
            no_match.build_plan->relation_assessments.size() == 1 &&
            no_match.build_plan->relation_assessments.front().kind ==
                PackageRelationAssessmentKind::
                    ConfirmedNoMatchingCurrentOrPlannedTarget &&
            !has_reason(
                no_match.targets.front(),
                AurUpdateExecutionReason::
                    ConflictsOrReplacesUnresolved),
        "Successful empty inventory did not release only the relation guard");
    expect_no_forbidden_operations();
}

void test_relation_inventory_failure_is_unknown_and_fail_closed() {
    FixtureRoot fixture("relation-query-failure");
    fixture.enter();
    stub::reset();
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{"", 73});

    const AurUpdateExecutionPreflight preflight =
        resolve_aur_update_execution_preflight(
            AurUpdatePlan{{update_entry(
                "relation-query-failure-root",
                InstalledPackageReason::Explicit)}});
    const AurUpdateExecutionIssue& issue =
        relation_issue(preflight.targets.front());
    expect(
        preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Unsupported &&
            issue.relation_reason->assessment.kind ==
                PackageRelationAssessmentKind::Unknown &&
            issue.relation_reason->assessment
                .attributed_observation_failure.has_value() &&
            package_metadata_test_stub::initialize_call_count() == 0 &&
            stub::captured_commands() ==
                std::vector<std::string>{DATABASE_PATH_COMMAND},
        "Installed inventory query failure became empty inventory or retried");
    expect_no_forbidden_operations();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if(argc != 2) throw std::runtime_error("Expected one integration case name.");
        stub::reset();
        bool rejected = false;
        try {
            static_cast<void>(capture_explicit_process_output_raw(
                ExplicitProcessInvocation{"/usr/bin/pacman-conf", {"--repo-list"}, {"LC_ALL=C"}}, true));
        } catch(const std::runtime_error&) {
            rejected = true;
        }
        expect(rejected && stub::forbidden_operation_count() == 1,
               "Explicit prefix capture bypassed preflight fixture isolation");
        stub::reset();
        const std::string case_name = argv[1];
        if(case_name == "simple") {
            test_skip_independent_target_uses_real_resolver_reentry();
            unsetenv("TMPDIR");
            test_simple_roots_and_combined_resolution();
        } else if(case_name == "repository-failure") {
            test_repository_metadata_failure_is_fail_closed();
        } else if(case_name == "aur-failure") {
            test_ordinary_aur_dependency_failure_is_owned_and_read_only();
        } else if(case_name == "relation-assessment") {
            test_relation_inventory_is_snapshotted_once_and_no_match_releases_guard();
        } else if(case_name == "relation-query-failure") {
            test_relation_inventory_failure_is_unknown_and_fail_closed();
        } else {
            throw std::runtime_error("Unknown integration case: " + case_name);
        }
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "aur update execution preflight integration " << argv[1]
              << ": all checks passed\n";
    return 0;
}
