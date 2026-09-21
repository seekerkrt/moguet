#include "local_source_build.hpp"

#include "local_source_workspace.hpp"
#include "stubs/local-dependency-plan/query_stub.hpp"
#include "stubs/local-source-build/process_stub.hpp"
#include "trusted_cache_test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

static_assert(!std::is_copy_constructible_v<LocalSourceBuildResult>);
static_assert(std::is_nothrow_move_constructible_v<LocalSourceBuildResult>);
static_assert(!std::is_copy_assignable_v<LocalSourceBuildResult>);
static_assert(!std::is_move_assignable_v<LocalSourceBuildResult>);

namespace {

namespace fs = std::filesystem;
namespace process_stub = local_source_build_test_stub;
namespace query_stub = local_dependency_plan_query_stub;

constexpr std::string_view PACKAGE_BASE = "local-build-suite";
constexpr std::string_view CLI_PACKAGE = "local-build-cli";
constexpr std::string_view LIBS_PACKAGE = "local-build-libs";
constexpr std::string_view DEBUG_PACKAGE = "local-build-debug";
constexpr std::string_view PKGBUILD_CONTENT =
    "pkgbase=local-build-suite\n"
    "pkgname=('local-build-cli' 'local-build-libs')\n"
    "pkgver=1.0\n"
    "pkgrel=1\n"
    "arch=('x86_64')\n";
constexpr std::string_view SRCINFO_CONTENT =
    "pkgbase = local-build-suite\n"
    "\tpkgver = 1.0\n"
    "\tpkgrel = 1\n"
    "\tarch = x86_64\n"
    "pkgname = local-build-cli\n"
    "pkgname = local-build-libs\n";
constexpr std::string_view UNSATISFIED_SRCINFO_CONTENT =
    "pkgbase = local-build-suite\n"
    "\tpkgver = 1.0\n"
    "\tpkgrel = 1\n"
    "\tarch = x86_64\n"
    "pkgname = local-build-cli\n"
    "\tdepends = local-build-libs>=2.0-1\n"
    "pkgname = local-build-libs\n";
constexpr std::string_view NESTED_CONTENT = "local source payload\n";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void write_file(const fs::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("Failed to open test file: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error(
            "Failed to finish test file: " + path.string());
    }
    if(::chmod(path.c_str(), 0644) != 0) {
        throw std::runtime_error(
            "Failed to set test file mode: " + path.string());
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) {
        throw std::runtime_error("Failed to read test file: " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void create_private_directory(const fs::path& path) {
    fs::create_directory(path);
    if(::chmod(path.c_str(), 0700) != 0) {
        throw std::runtime_error(
            "Failed to set test directory mode: " + path.string());
    }
}

class ScopedEnvironmentVariable final {
    std::string key_;
    std::optional<std::string> previous_;

public:
    ScopedEnvironmentVariable(std::string key, const std::string& value)
        : key_(std::move(key)) {
        const char* previous = std::getenv(key_.c_str());
        if(previous != nullptr) previous_ = previous;
        if(::setenv(key_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error(
                "Failed to set test environment variable: " + key_);
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() noexcept {
        if(previous_.has_value()) {
            static_cast<void>(
                ::setenv(key_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(key_.c_str()));
        }
    }
};

class LocalBuildFixture final {
    fs::path base_path_;
    fs::path source_path_;
    fs::path cache_home_path_;
    std::optional<std::string> previous_xdg_cache_home_;
    std::optional<std::string> previous_pkgdest_;

public:
    LocalBuildFixture() {
        const std::string template_text =
            (fs::temp_directory_path() /
             "moguet-local-source-build-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created_path = ::mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create local source build test directory.");
        }

        base_path_ = created_path;
        source_path_ = base_path_ / "source";
        cache_home_path_ = base_path_ / "cache-home";
        create_private_directory(source_path_);
        create_private_directory(cache_home_path_);
        create_private_directory(source_path_ / "nested");
        write_file(source_path_ / "PKGBUILD", PKGBUILD_CONTENT);
        write_file(source_path_ / ".SRCINFO", SRCINFO_CONTENT);
        write_file(source_path_ / "nested" / "payload", NESTED_CONTENT);

        const char* previous_cache_home = std::getenv("XDG_CACHE_HOME");
        if(previous_cache_home != nullptr) {
            previous_xdg_cache_home_ = previous_cache_home;
        }
        const char* previous_pkgdest = std::getenv("PKGDEST");
        if(previous_pkgdest != nullptr) previous_pkgdest_ = previous_pkgdest;

        if(::setenv("XDG_CACHE_HOME", cache_home_path_.c_str(), 1) != 0 ||
           ::unsetenv("PKGDEST") != 0) {
            throw std::runtime_error(
                "Failed to prepare local source build test environment.");
        }
    }

    LocalBuildFixture(const LocalBuildFixture&) = delete;
    LocalBuildFixture& operator=(const LocalBuildFixture&) = delete;

    ~LocalBuildFixture() noexcept {
        if(previous_xdg_cache_home_.has_value()) {
            static_cast<void>(::setenv(
                "XDG_CACHE_HOME", previous_xdg_cache_home_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv("XDG_CACHE_HOME"));
        }
        if(previous_pkgdest_.has_value()) {
            static_cast<void>(
                ::setenv("PKGDEST", previous_pkgdest_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv("PKGDEST"));
        }

        std::error_code error;
        fs::remove_all(base_path_, error);
    }

    const fs::path& source_path() const noexcept {
        return source_path_;
    }

    void write_srcinfo(std::string_view contents) const {
        write_file(source_path_ / ".SRCINFO", contents);
    }

    fs::path cache_root_path() const {
        return cache_home_path_ / "moguet";
    }

    LocalSourceBuildRequest make_request(
        bool mismatch_plan = false,
        SourceBuildEnvironment source_environment = {}) const {
        const bool has_one_off_environment_assignment =
            !source_environment.ordered_assignments.empty();
        LocalSourceRoot source_root =
            open_local_source_root(
                source_path_,
                has_one_off_environment_assignment);
        const LocalPackageMetadataParseResult* parsed =
            source_root.metadata().parse_result();
        expect(
            source_root.metadata().state() ==
                    (has_one_off_environment_assignment
                         ? LocalSourceMetadataState::KnownStale
                         : LocalSourceMetadataState::
                               UsableUnverified) &&
                parsed != nullptr && parsed->is_success() &&
                parsed->metadata() != nullptr,
            "Fixture .SRCINFO was not accepted as typed metadata");

        LocalSourceBuildMetadata bound_metadata =
            has_one_off_environment_assignment
                ? bind_evaluated_local_source_metadata(
                      source_root, std::move(source_environment),
                      "x86_64", SRCINFO_CONTENT)
                : bind_existing_local_source_metadata(
                      source_root, "x86_64");
        LocalPackageMetadata metadata = bound_metadata.metadata();
        if(mismatch_plan) metadata.pkgrel = "2";
        query_stub::reset_repository_stub();
        query_stub::reset_aur_stub();
        LocalBuildPlan plan =
            resolve_local_build_plan(metadata, "x86_64");
        expect(
            query_stub::repository_query_history().empty() &&
                query_stub::aur_query_history().empty(),
            "Dependency-free local build fixture performed a query");

        return LocalSourceBuildRequest{
            std::move(source_root),
            std::move(plan),
            prepare_test_trusted_cache_root(),
            std::move(bound_metadata),
            ArtifactMakepkgBuildOptions{
                .no_confirm = true,
                .rebuild = true,
                .clean_build = true}};
    }

    void expect_original_tree_unchanged(
        std::string_view expected_srcinfo = SRCINFO_CONTENT) const {
        expect(
            read_file(source_path_ / "PKGBUILD") == PKGBUILD_CONTENT,
            "Original PKGBUILD changed during local build");
        expect(
            read_file(source_path_ / ".SRCINFO") == expected_srcinfo,
            "Original .SRCINFO changed during local build");
        expect(
            read_file(source_path_ / "nested" / "payload") ==
                NESTED_CONTENT,
            "Original nested source changed during local build");
        expect(
            !fs::exists(source_path_ / "src") &&
                !fs::exists(source_path_ / "pkg"),
            "Build output escaped into the original local tree");
        expect(
            !fs::exists(source_path_ / ".git"),
            "Local build fixture unexpectedly became a Git checkout");
    }
};

std::vector<std::string> direct_child_names(const fs::path& directory) {
    std::vector<std::string> names;
    if(!fs::exists(directory)) return names;
    for(const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string expected_shell_quote(std::string_view value) {
    std::string quoted = "'";
    for(const char character : value) {
        if(character == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(character);
        }
    }
    quoted += "'";
    return quoted;
}

std::string expected_environment_prefix(
    const fs::path& artifact_workspace,
    const SourceBuildEnvironment& source_environment) {
    std::string prefix;
    for(const SourceEnvironmentAssignment& assignment :
        source_environment.ordered_assignments) {
        prefix += assignment.key + "=" +
                  expected_shell_quote(assignment.value) + " ";
    }
    return prefix + "PKGDEST=" +
           expected_shell_quote(artifact_workspace.string()) + " ";
}

std::string expected_makepkg_command(
    const fs::path& artifact_workspace,
    const std::vector<std::string>& arguments,
    const SourceBuildEnvironment& source_environment = {}) {
    std::vector<std::string> command_words{"makepkg"};
    command_words.insert(
        command_words.end(), arguments.begin(), arguments.end());
    for(const SourceEnvironmentAssignment& assignment :
        source_environment.ordered_assignments) {
        command_words.push_back(assignment.key + "=" + assignment.value);
    }
    command_words.push_back("PKGDEST=" + artifact_workspace.string());

    std::string command = expected_environment_prefix(
                              artifact_workspace,
                              source_environment) +
                          expected_shell_quote(command_words.front());
    for(std::size_t index = 1; index < command_words.size(); ++index) {
        command += " " + expected_shell_quote(command_words[index]);
    }
    return command;
}

std::string expected_identity_command(const fs::path& artifact_path) {
    const std::vector<std::string> arguments = {
        "pacman", "-Qp", "--color", "never", "--",
        artifact_path.string()};
    std::string command = "LC_ALL=C";
    for(const std::string& argument : arguments) {
        command += " " + expected_shell_quote(argument);
    }
    return command;
}

enum class ScenarioKind {
    Success,
    BuildFailure,
    BuildContextFailure,
    MissingArtifact,
    UnexpectedArtifact,
    IdentityMismatch,
    IdentityQueryFailure,
    UnsafeWorkspaceSuccess,
    UnsafeWorkspaceBuildFailure,
    CleanupFailureAfterSuccess,
    CleanupFailureDuringBuildFailure,
};

struct ScenarioObservation {
    ScenarioKind kind = ScenarioKind::Success;
    fs::path original_source_path;
    fs::path source_workspace_path;
    fs::path artifact_workspace_path;
    std::size_t source_workspace_events = 0;
    std::size_t cleanup_removal_attempts = 0;
    bool cleanup_failure_injected = false;
    std::string hook_failure;
    SourceBuildEnvironment source_environment;

    ScenarioObservation(
        ScenarioKind scenario_kind, fs::path original_source,
        SourceBuildEnvironment environment = {})
        : kind(scenario_kind),
          original_source_path(std::move(original_source)),
          source_environment(std::move(environment)) {
    }
};

ScenarioObservation* g_observation = nullptr;

bool is_build_failure_scenario(ScenarioKind kind) noexcept {
    return kind == ScenarioKind::BuildFailure ||
           kind == ScenarioKind::UnsafeWorkspaceBuildFailure ||
           kind == ScenarioKind::CleanupFailureDuringBuildFailure;
}

bool creates_unsafe_workspace_entries(ScenarioKind kind) noexcept {
    return kind == ScenarioKind::UnsafeWorkspaceSuccess ||
           kind == ScenarioKind::UnsafeWorkspaceBuildFailure;
}

const std::vector<std::string>& artifact_leaf_names() {
    static const std::vector<std::string> names = {
        "local-build-debug-1.0-1-x86_64.pkg.tar.zst",
        "local-build-libs-1.0-1-x86_64.pkg.tar.zst",
        "local-build-cli-1.0-1-x86_64.pkg.tar.zst",
    };
    return names;
}

void record_hook_failure(const std::exception& error) noexcept {
    if(g_observation == nullptr || !g_observation->hook_failure.empty()) return;
    try {
        g_observation->hook_failure = error.what();
    } catch(...) {
        g_observation->hook_failure = "process hook failed";
    }
}

void observe_source_workspace_event(
    LocalSourceWorkspaceTestEvent event,
    const fs::path&) {
    if(g_observation == nullptr) return;
    ++g_observation->source_workspace_events;
    if(event == LocalSourceWorkspaceTestEvent::BeforeCleanupRemoval) {
        ++g_observation->cleanup_removal_attempts;
    }
    if(event == LocalSourceWorkspaceTestEvent::BeforeCleanupRemoval &&
       (g_observation->kind == ScenarioKind::CleanupFailureAfterSuccess ||
        g_observation->kind ==
            ScenarioKind::CleanupFailureDuringBuildFailure) &&
       !g_observation->cleanup_failure_injected) {
        g_observation->cleanup_failure_injected = true;
        throw LocalSourceWorkspaceError(LocalSourceWorkspaceFailure{
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            "injected-cleanup", std::nullopt});
    }
}

void observe_packagelist_command() {
    if(g_observation == nullptr) return;
    try {
        g_observation->source_workspace_path = fs::current_path();
        expect(
            g_observation->source_workspace_path !=
                g_observation->original_source_path,
            "makepkg --packagelist ran in the original source tree");
        expect(
            read_file(
                g_observation->source_workspace_path / "PKGBUILD") ==
                PKGBUILD_CONTENT,
            "Source workspace did not contain the PKGBUILD snapshot");
        expect(
            read_file(
                g_observation->source_workspace_path / "nested" /
                "payload") == NESTED_CONTENT,
            "Source workspace did not contain the nested source snapshot");
    } catch(const std::exception& error) {
        record_hook_failure(error);
    }
}

void observe_build_command() {
    if(g_observation == nullptr) return;
    try {
        const fs::path source_workspace = fs::current_path();
        expect(
            source_workspace == g_observation->source_workspace_path,
            "packagelist and build used different source workspaces");
        create_private_directory(source_workspace / "src");
        create_private_directory(source_workspace / "pkg");
        write_file(source_workspace / "src" / "generated", "generated\n");
        write_file(source_workspace / "PKGBUILD", "mutated snapshot\n");
        if(creates_unsafe_workspace_entries(g_observation->kind)) {
            const fs::path writable_directory =
                source_workspace / "group-writable-output";
            fs::create_directory(writable_directory);
            if(::chmod(writable_directory.c_str(), 0777) != 0) {
                throw std::runtime_error(
                    "Failed to make fake build output group-writable");
            }
            const fs::path inaccessible_directory =
                writable_directory / "no-owner-access";
            fs::create_directory(inaccessible_directory);
            write_file(
                inaccessible_directory / "nested-output",
                "generated\n");
            if(::chmod(inaccessible_directory.c_str(), 0000) != 0) {
                throw std::runtime_error(
                    "Failed to remove fake build output owner access");
            }
            if(::mkfifo(
                   (source_workspace / "generated-fifo").c_str(),
                   0600) != 0) {
                throw std::runtime_error(
                    "Failed to create fake build FIFO output");
            }
        }
        expect(
            read_file(
                g_observation->original_source_path / "PKGBUILD") ==
                PKGBUILD_CONTENT,
            "Fake build mutated the original PKGBUILD");
        expect(
            !fs::exists(g_observation->original_source_path / "src") &&
                !fs::exists(g_observation->original_source_path / "pkg"),
            "Fake build created output in the original source tree");

        if(is_build_failure_scenario(g_observation->kind)) return;

        const auto& leaves = artifact_leaf_names();
        std::size_t artifact_count = leaves.size();
        if(g_observation->kind == ScenarioKind::MissingArtifact) {
            artifact_count = leaves.size() - 1;
        }
        for(std::size_t index = 0; index < artifact_count; ++index) {
            write_file(
                g_observation->artifact_workspace_path / leaves[index],
                "artifact-" + std::to_string(index) + "\n");
        }
        if(g_observation->kind == ScenarioKind::UnexpectedArtifact) {
            write_file(
                g_observation->artifact_workspace_path /
                    "unexpected-1.0-1-x86_64.pkg.tar.zst",
                "unexpected artifact\n");
        }
    } catch(const std::exception& error) {
        record_hook_failure(error);
    }
}

void observe_artifact_workspace_creation(const fs::path& workspace_path) {
    if(g_observation == nullptr) return;
    g_observation->artifact_workspace_path = workspace_path;

    if(g_observation->kind == ScenarioKind::BuildContextFailure) {
        if(::setenv("PKGDEST", "injected-after-preflight", 1) != 0) {
            record_hook_failure(std::runtime_error(
                "Failed to inject the BuildContext PKGDEST failure"));
        }
        return;
    }

    std::string packagelist_output;
    for(const std::string& leaf : artifact_leaf_names()) {
        packagelist_output += (workspace_path / leaf).string() + "\n";
    }
    process_stub::expect_capture_command(
        expected_makepkg_command(
            workspace_path, {"--packagelist"},
            g_observation->source_environment),
        CapturedCommandResult{std::move(packagelist_output), 0},
        observe_packagelist_command);

    const int build_exit_code =
        is_build_failure_scenario(g_observation->kind) ? 47 : 0;
    process_stub::expect_run_command(
        expected_makepkg_command(
            workspace_path,
            {"-sc", "--noconfirm", "-f", "-C"},
            g_observation->source_environment),
        build_exit_code, observe_build_command);
    if(build_exit_code != 0 ||
       g_observation->kind == ScenarioKind::MissingArtifact ||
       g_observation->kind == ScenarioKind::UnexpectedArtifact) {
        return;
    }

    const std::vector<std::string> identity_names = {
        std::string(DEBUG_PACKAGE),
        std::string(LIBS_PACKAGE),
        g_observation->kind == ScenarioKind::IdentityMismatch
            ? "wrong-local-build-cli"
            : std::string(CLI_PACKAGE),
    };
    for(std::size_t index = 0; index < identity_names.size(); ++index) {
        const bool query_failure =
            g_observation->kind == ScenarioKind::IdentityQueryFailure &&
            index == 1;
        process_stub::expect_capture_command(
            expected_identity_command(
                workspace_path / artifact_leaf_names()[index]),
            CapturedCommandResult{
                query_failure
                    ? std::string{}
                    : identity_names[index] + " 1.0-1\n",
                query_failure ? 31 : 0});
        if(query_failure) break;
    }
}

class ScenarioObservationGuard final {
public:
    explicit ScenarioObservationGuard(ScenarioObservation& observation) {
        if(g_observation != nullptr) {
            throw std::logic_error("Nested local source build observation.");
        }
        g_observation = &observation;
        set_local_source_workspace_test_hook(observe_source_workspace_event);
        set_artifact_workspace_creation_observer_for_test(
            observe_artifact_workspace_creation);
    }

    ScenarioObservationGuard(const ScenarioObservationGuard&) = delete;
    ScenarioObservationGuard& operator=(
        const ScenarioObservationGuard&) = delete;

    ~ScenarioObservationGuard() noexcept {
        set_artifact_workspace_creation_observer_for_test(nullptr);
        set_local_source_workspace_test_hook({});
        if(g_observation != nullptr &&
           g_observation->kind == ScenarioKind::BuildContextFailure) {
            static_cast<void>(::unsetenv("PKGDEST"));
        }
        g_observation = nullptr;
    }
};

struct ObservedFailure {
    LocalSourceBuildFailurePhase phase;
    std::optional<int> build_exit_code;
    std::optional<LocalSourceWorkspaceFailure> source_workspace_failure;
    std::optional<LocalSourceWorkspaceFailure> source_cleanup_failure;
    std::optional<PackageBaseArtifactIdentitySelectionFailure>
        selection_failure;
    std::optional<fs::path> retained_artifact_workspace;
    std::string diagnostic;
};

ObservedFailure execute_expect_failure(LocalSourceBuildRequest request) {
    try {
        LocalSourceBuildResult result =
            execute_local_source_build(std::move(request));
        result.cleanup_artifacts();
    } catch(const LocalSourceBuildPhaseError& error) {
        return ObservedFailure{
            error.phase(),
            error.build_exit_code(),
            error.source_workspace_failure() == nullptr
                ? std::nullopt
                : std::optional<LocalSourceWorkspaceFailure>(
                      *error.source_workspace_failure()),
            error.source_cleanup_failure() == nullptr
                ? std::nullopt
                : std::optional<LocalSourceWorkspaceFailure>(
                      *error.source_cleanup_failure()),
            error.selection_failure() == nullptr
                ? std::nullopt
                : std::optional<
                      PackageBaseArtifactIdentitySelectionFailure>(
                      *error.selection_failure()),
            error.retained_artifact_workspace() == nullptr
                ? std::nullopt
                : std::optional<fs::path>(
                      *error.retained_artifact_workspace()),
            error.what()};
    }
    throw std::runtime_error("Local source build unexpectedly succeeded");
}

void expect_observed_hooks_succeeded(
    const ScenarioObservation& observation) {
    expect(
        observation.hook_failure.empty(),
        "Process observation failed: " + observation.hook_failure);
    process_stub::require_process_expectations_consumed();
}

void expect_source_workspace_cleaned(
    const ScenarioObservation& observation) {
    expect(
        observation.source_workspace_events > 0,
        "Source workspace materialization was not observed");
    expect(
        !observation.source_workspace_path.empty(),
        "Source workspace command cwd was not observed");
    expect(
        !fs::exists(observation.source_workspace_path),
        "Source workspace was not cleaned");
}

void expect_artifact_diagnostic_retained(
    const ScenarioObservation& observation,
    const ObservedFailure& failure) {
    expect(
        !observation.artifact_workspace_path.empty() &&
            fs::exists(observation.artifact_workspace_path),
        "Failure did not retain its artifact workspace");
    expect(
        failure.retained_artifact_workspace.has_value() &&
            *failure.retained_artifact_workspace ==
                observation.artifact_workspace_path,
        "Failure lost its structured retained workspace display path");
    expect(
        failure.diagnostic.find("retained artifact workspace") !=
            std::string::npos,
        "Failure did not report artifact diagnostic retention");
    expect(
        failure.diagnostic.find(
            observation.artifact_workspace_path.string()) !=
            std::string::npos,
        "Failure diagnostic did not identify the retained workspace");
}

void test_success_uses_snapshot_and_returns_correlated_artifacts() {
    LocalBuildFixture fixture;
    LocalSourceBuildRequest request = fixture.make_request();
    ScenarioObservation observation(
        ScenarioKind::Success, fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    LocalSourceBuildResult result =
        execute_local_source_build(std::move(request));

    expect_observed_hooks_succeeded(observation);
    expect_source_workspace_cleaned(observation);
    fixture.expect_original_tree_unchanged();
    expect(
        result.package_base() == PACKAGE_BASE,
        "Local build result PackageBase differs");

    const auto& selected = result.selected_artifacts();
    expect(selected.size() == 2, "Selected artifact count differs");
    expect(
        selected[0].artifact_index == 2 &&
            selected[0].identity.package_name == CLI_PACKAGE &&
            selected[0].identity.full_version == "1.0-1" &&
            selected[0].desired_reason ==
                DesiredInstallReason::Explicit,
        "First selected artifact lost required-target order or stable index");
    expect(
        selected[1].artifact_index == 1 &&
            selected[1].identity.package_name == LIBS_PACKAGE &&
            selected[1].identity.full_version == "1.0-1" &&
            selected[1].desired_reason ==
                DesiredInstallReason::Explicit,
        "Second selected artifact lost required-target order or stable index");

    const auto& unselected = result.unselected_artifacts();
    expect(
        unselected.size() == 1 &&
            unselected[0].artifact_index == 0 &&
            unselected[0].identity.package_name == DEBUG_PACKAGE &&
            unselected[0].identity.full_version == "1.0-1",
        "Unselected artifact lost aggregate order or stable index");
    expect(
        process_stub::run_command_call_count() == 1 &&
            process_stub::capture_command_call_count() == 4,
        "Local build crossed an unexpected process boundary");
    expect(
        fs::exists(observation.artifact_workspace_path),
        "Success result did not retain artifact ownership");
    result.cleanup_artifacts();
    expect(
        !fs::exists(observation.artifact_workspace_path),
        "Explicit artifact cleanup did not remove the workspace");
}

void test_prepared_projection_authority_tracks_one_invocation() {
    LocalBuildFixture fixture;
    PreparedLocalSourceBuild prepared = prepare_local_source_build(
        fixture.make_request());
    const LocalSourceBuildProjectionAuthority& authority =
        prepared.projection_authority();
    expect(
        authority.has_complete_identity() &&
            authority.source_root().canonical_path() ==
                fs::canonical(fixture.source_path()) &&
            authority.accepted_metadata().package_base ==
                PACKAGE_BASE &&
            authority.effective_architecture() == "x86_64" &&
            authority.source_environment()
                .ordered_assignments.empty() &&
            authority.provenance() ==
                LocalSourceBuildMetadataProvenance::
                    ExistingSrcinfo &&
            authority.source_directory_identity() ==
                authority.source_root().directory_identity() &&
            authority.pkgbuild_snapshot() ==
                authority.source_root().pkgbuild(),
        "Prepared local projection authority mixed invocation state");

    PreparedLocalSourceBuild moved(std::move(prepared));
    const LocalSourceBuildProjectionAuthority& moved_authority =
        moved.projection_authority();
    expect(
        moved_authority.has_complete_identity() &&
            moved_authority.source_root().canonical_path() ==
                fs::canonical(fixture.source_path()),
        "Moved local projection authority retained moved-from borrows");
}

void test_cache_below_source_is_rejected_during_static_preflight() {
    LocalBuildFixture fixture;
    LocalSourceBuildRequest request = [&]() {
        ScopedEnvironmentVariable cache_home(
            "XDG_CACHE_HOME", fixture.source_path().string());
        return fixture.make_request();
    }();
    process_stub::reset_process_stub();

    try {
        static_cast<void>(prepare_local_source_build(std::move(request)));
    } catch(const LocalSourceBuildPhaseError& error) {
        expect(
            error.phase() == LocalSourceBuildFailurePhase::Preflight,
            "Source/cache containment was rejected after preflight");
        expect(
            error.source_workspace_failure() != nullptr &&
                error.source_workspace_failure()->stage ==
                    LocalSourceWorkspaceStage::BoundaryValidation &&
                error.source_workspace_failure()->code ==
                    LocalSourceWorkspaceErrorCode::CacheInsideSource,
            "Source/cache containment lost its typed boundary failure");
        expect(
            process_stub::run_command_call_count() == 0 &&
                process_stub::capture_command_call_count() == 0,
            "Source/cache containment crossed a process boundary");
        return;
    }
    throw std::runtime_error(
        "Source/cache containment passed static preflight");
}

void test_unsatisfied_constraint_stops_before_local_build_mutation() {
    LocalBuildFixture fixture;
    fixture.write_srcinfo(UNSATISFIED_SRCINFO_CONTENT);
    LocalSourceBuildRequest request = fixture.make_request();
    const auto initial_cache_entries =
        direct_child_names(fixture.cache_root_path());

    const auto typed_edge = std::find_if(
        request.build_plan.build_plan().dependency_edges.begin(),
        request.build_plan.build_plan().dependency_edges.end(),
        [](const BuildPlanDependencyEdge& edge) {
            return edge.dependency_spec ==
                   "local-build-libs>=2.0-1";
        });
    expect(
        typed_edge !=
                request.build_plan.build_plan()
                    .dependency_edges.end() &&
            typed_edge->kind == DependencyKind::Local &&
            typed_edge->requirement.has_value() &&
            typed_edge->resolved_candidate.has_value() &&
            typed_edge->constraint_evaluation.has_value() &&
            typed_edge->constraint_evaluation->satisfaction() ==
                ConstraintSatisfaction::Unsatisfied,
        "Local build fixture did not retain its typed unsatisfied edge");

    ScenarioObservation observation(
        ScenarioKind::Success, fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);
    const ObservedFailure failure =
        execute_expect_failure(std::move(request));

    expect(
        failure.phase == LocalSourceBuildFailurePhase::Preflight,
        "Unsatisfied local constraint was rejected after preflight");
    expect(
        process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0 &&
            observation.source_workspace_events == 0 &&
            observation.artifact_workspace_path.empty(),
        "Unsatisfied local constraint crossed a workspace or process boundary");
    expect(
        direct_child_names(fixture.cache_root_path()) ==
            initial_cache_entries,
        "Unsatisfied local constraint mutated the trusted cache");
    process_stub::require_process_expectations_consumed();
    fixture.expect_original_tree_unchanged(UNSATISFIED_SRCINFO_CONTENT);
}

void test_evaluated_metadata_forwards_bound_environment_in_order() {
    LocalBuildFixture fixture;
    SourceBuildEnvironment source_environment{{
        {"FIRST", "one"},
        {"EMPTY", ""},
        {"FIRST", "last"},
    }};
    LocalSourceBuildRequest request =
        fixture.make_request(false, source_environment);
    ScenarioObservation observation(
        ScenarioKind::Success, fixture.source_path(),
        std::move(source_environment));
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    LocalSourceBuildResult result =
        execute_local_source_build(std::move(request));

    expect_observed_hooks_succeeded(observation);
    expect_source_workspace_cleaned(observation);
    fixture.expect_original_tree_unchanged();
    expect(
        result.selected_artifacts().size() == 2,
        "Evaluated metadata build lost selected artifacts");
    result.cleanup_artifacts();
    expect(
        !fs::exists(observation.artifact_workspace_path),
        "Evaluated metadata build did not clean its artifact workspace");
}

void test_preflight_rejects_environment_pkgdest_and_plan_mismatch() {
    {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request();
        const auto initial_cache_entries =
            direct_child_names(fixture.cache_root_path());
        ScenarioObservation observation(
            ScenarioKind::Success, fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);
        ScopedEnvironmentVariable ambient_pkgdest("PKGDEST", "");

        const ObservedFailure failure =
            execute_expect_failure(std::move(request));
        expect(
            failure.phase == LocalSourceBuildFailurePhase::Preflight,
            "Ambient PKGDEST failure phase differs");
        expect(
            process_stub::capture_command_call_count() == 0 &&
                process_stub::run_command_call_count() == 0 &&
                observation.source_workspace_events == 0 &&
                observation.artifact_workspace_path.empty(),
            "Ambient PKGDEST crossed a workspace or process boundary");
        expect(
            direct_child_names(fixture.cache_root_path()) ==
                initial_cache_entries,
            "Ambient PKGDEST mutated the trusted cache");
        process_stub::require_process_expectations_consumed();
        fixture.expect_original_tree_unchanged();
    }

    {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request(true);
        const auto initial_cache_entries =
            direct_child_names(fixture.cache_root_path());
        ScenarioObservation observation(
            ScenarioKind::Success, fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);

        const ObservedFailure failure =
            execute_expect_failure(std::move(request));
        expect(
            failure.phase == LocalSourceBuildFailurePhase::Preflight,
            "Mismatched local metadata failure phase differs");
        expect(
            process_stub::capture_command_call_count() == 0 &&
                process_stub::run_command_call_count() == 0 &&
                observation.source_workspace_events == 0 &&
                observation.artifact_workspace_path.empty(),
            "Mismatched local metadata crossed a workspace or process boundary");
        expect(
            direct_child_names(fixture.cache_root_path()) ==
                initial_cache_entries,
            "Mismatched local metadata mutated the trusted cache");
        process_stub::require_process_expectations_consumed();
        fixture.expect_original_tree_unchanged();
    }
}

void test_workspace_failure_preserves_typed_cause() {
    LocalBuildFixture fixture;
    const fs::path unsafe_link = fixture.source_path() / "unsafe-link";
    fs::create_symlink("../outside", unsafe_link);
    LocalSourceBuildRequest request = fixture.make_request();
    const auto initial_cache_entries =
        direct_child_names(fixture.cache_root_path());
    ScenarioObservation observation(
        ScenarioKind::Success, fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    const ObservedFailure failure =
        execute_expect_failure(std::move(request));
    expect(
        failure.phase == LocalSourceBuildFailurePhase::SourceWorkspace &&
            failure.source_workspace_failure.has_value() &&
            failure.source_workspace_failure->stage ==
                LocalSourceWorkspaceStage::SourceInspection &&
            failure.source_workspace_failure->code ==
                LocalSourceWorkspaceErrorCode::SymlinkEscape &&
            failure.source_workspace_failure->relative_path ==
                fs::path("unsafe-link"),
        "Source workspace failure lost its typed symlink cause");
    expect(
        process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0 &&
            observation.artifact_workspace_path.empty(),
        "Unsafe source symlink crossed the process/artifact boundary");
    expect(
        direct_child_names(fixture.cache_root_path()) ==
            initial_cache_entries,
        "Unsafe source symlink left a partial workspace");
    process_stub::require_process_expectations_consumed();
    fixture.expect_original_tree_unchanged();
}

void test_execute_revalidates_source_before_workspace_creation() {
    LocalBuildFixture fixture;
    PreparedLocalSourceBuild prepared =
        prepare_local_source_build(fixture.make_request());
    const auto initial_cache_entries =
        direct_child_names(fixture.cache_root_path());
    const fs::path original_pkgbuild =
        fixture.source_path() / "PKGBUILD.original";
    fs::rename(fixture.source_path() / "PKGBUILD", original_pkgbuild);
    write_file(fixture.source_path() / "PKGBUILD", "pkgname=replaced\n");
    process_stub::reset_process_stub();

    try {
        static_cast<void>(
            execute_prepared_local_source_build(std::move(prepared)));
    } catch(const LocalSourceBuildPhaseError& error) {
        expect(
            error.phase() ==
                    LocalSourceBuildFailurePhase::SourceWorkspace &&
                error.source_root_failure() != nullptr &&
                error.source_root_failure()->code ==
                    LocalSourceRootErrorCode::ConcurrentReplacement,
            "Execution-time source replacement lost its typed phase/cause");
        expect(
            process_stub::capture_command_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "Execution-time source replacement crossed a process boundary");
        expect(
            direct_child_names(fixture.cache_root_path()) ==
                initial_cache_entries,
            "Execution-time source replacement created a workspace");
        process_stub::require_process_expectations_consumed();
        return;
    }
    throw std::runtime_error(
        "Execution-time source replacement was not rejected");
}

void test_build_context_failure_retains_artifact_diagnostics() {
    LocalBuildFixture fixture;
    LocalSourceBuildRequest request = fixture.make_request();
    ScenarioObservation observation(
        ScenarioKind::BuildContextFailure, fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    const ObservedFailure failure =
        execute_expect_failure(std::move(request));
    expect(
        failure.phase == LocalSourceBuildFailurePhase::BuildContext &&
            !failure.build_exit_code.has_value() &&
            !failure.source_workspace_failure.has_value() &&
            !failure.selection_failure.has_value(),
        "BuildContext failure did not preserve its typed phase");
    expect_observed_hooks_succeeded(observation);
    expect_artifact_diagnostic_retained(observation, failure);
    expect(
        direct_child_names(fixture.cache_root_path()) ==
            std::vector<std::string>{
                observation.artifact_workspace_path.filename()
                    .string()},
        "BuildContext failure retained a source workspace or lost its artifact workspace");
    fixture.expect_original_tree_unchanged();
}

void test_unsafe_makepkg_outputs_are_cleaned() {
    {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request();
        ScenarioObservation observation(
            ScenarioKind::UnsafeWorkspaceSuccess,
            fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);

        LocalSourceBuildResult result =
            execute_local_source_build(std::move(request));
        expect_observed_hooks_succeeded(observation);
        expect_source_workspace_cleaned(observation);
        fixture.expect_original_tree_unchanged();
        result.cleanup_artifacts();
        expect(
            !fs::exists(observation.artifact_workspace_path),
            "Unsafe-output success did not clean its artifact workspace");
    }

    {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request();
        ScenarioObservation observation(
            ScenarioKind::UnsafeWorkspaceBuildFailure,
            fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);

        const ObservedFailure failure =
            execute_expect_failure(std::move(request));
        expect(
            failure.phase == LocalSourceBuildFailurePhase::Build &&
                failure.build_exit_code == 47,
            "Unsafe-output nonzero build lost its typed failure");
        expect_observed_hooks_succeeded(observation);
        expect_source_workspace_cleaned(observation);
        expect_artifact_diagnostic_retained(observation, failure);
        fixture.expect_original_tree_unchanged();
    }
}

void test_build_failure_retains_only_artifact_diagnostics() {
    LocalBuildFixture fixture;
    LocalSourceBuildRequest request = fixture.make_request();
    ScenarioObservation observation(
        ScenarioKind::BuildFailure, fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    const ObservedFailure failure =
        execute_expect_failure(std::move(request));
    expect(
        failure.phase == LocalSourceBuildFailurePhase::Build &&
            failure.build_exit_code == 47 &&
            !failure.selection_failure.has_value(),
        "Nonzero build failure did not preserve typed phase/status");
    expect_observed_hooks_succeeded(observation);
    expect_source_workspace_cleaned(observation);
    expect_artifact_diagnostic_retained(observation, failure);
    fixture.expect_original_tree_unchanged();
}

void test_primary_failure_preserves_secondary_source_cleanup_failure() {
    LocalBuildFixture fixture;
    LocalSourceBuildRequest request = fixture.make_request();
    ScenarioObservation observation(
        ScenarioKind::CleanupFailureDuringBuildFailure,
        fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    const ObservedFailure failure =
        execute_expect_failure(std::move(request));
    expect(
        failure.phase == LocalSourceBuildFailurePhase::Build &&
            failure.build_exit_code == 47 &&
            failure.source_cleanup_failure.has_value() &&
            failure.source_cleanup_failure->stage ==
                LocalSourceWorkspaceStage::Cleanup &&
            failure.source_cleanup_failure->code ==
                LocalSourceWorkspaceErrorCode::CleanupFailure &&
            failure.source_cleanup_failure->relative_path ==
                "injected-cleanup",
        "Primary build failure lost its secondary cleanup failure");
    expect(
        observation.cleanup_failure_injected,
        "Secondary cleanup failure hook did not run");
    expect_observed_hooks_succeeded(observation);
    expect(
        fs::is_directory(observation.source_workspace_path),
        "Known cleanup failure did not preserve the source workspace");
    expect(
        read_file(observation.source_workspace_path / "PKGBUILD") ==
            "mutated snapshot\n",
        "Known cleanup failure deleted source workspace content");
    expect(
        observation.cleanup_removal_attempts == 1,
        "Known cleanup failure triggered another removal attempt");
    expect_artifact_diagnostic_retained(observation, failure);
    fixture.expect_original_tree_unchanged();
}

void test_successful_build_preserves_typed_source_cleanup_failure() {
    LocalBuildFixture fixture;
    LocalSourceBuildRequest request = fixture.make_request();
    ScenarioObservation observation(
        ScenarioKind::CleanupFailureAfterSuccess,
        fixture.source_path());
    process_stub::reset_process_stub();
    ScenarioObservationGuard guard(observation);

    const ObservedFailure failure =
        execute_expect_failure(std::move(request));
    expect(
        failure.phase == LocalSourceBuildFailurePhase::SourceCleanup &&
            !failure.build_exit_code.has_value() &&
            !failure.source_workspace_failure.has_value() &&
            failure.source_cleanup_failure.has_value() &&
            failure.source_cleanup_failure->stage ==
                LocalSourceWorkspaceStage::Cleanup &&
            failure.source_cleanup_failure->code ==
                LocalSourceWorkspaceErrorCode::CleanupFailure &&
            failure.source_cleanup_failure->relative_path ==
                "injected-cleanup",
        "Successful build lost its typed source cleanup failure");
    expect(
        observation.cleanup_failure_injected,
        "Standalone source cleanup failure hook did not run");
    expect_observed_hooks_succeeded(observation);
    expect(
        fs::is_directory(observation.source_workspace_path),
        "Known cleanup failure did not preserve the source workspace");
    expect(
        read_file(observation.source_workspace_path / "PKGBUILD") ==
            "mutated snapshot\n",
        "Known cleanup failure deleted source workspace content");
    expect(
        observation.cleanup_removal_attempts == 1,
        "Known cleanup failure triggered another removal attempt");
    expect_artifact_diagnostic_retained(observation, failure);
    fixture.expect_original_tree_unchanged();
}

void test_missing_and_unexpected_artifacts_fail_closed() {
    for(const ScenarioKind kind : {
            ScenarioKind::MissingArtifact,
            ScenarioKind::UnexpectedArtifact}) {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request();
        ScenarioObservation observation(kind, fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);

        const ObservedFailure failure =
            execute_expect_failure(std::move(request));
        expect(
            failure.phase ==
                    LocalSourceBuildFailurePhase::ArtifactValidation &&
                !failure.build_exit_code.has_value() &&
                !failure.selection_failure.has_value(),
            "Artifact set failure did not preserve validation phase");
        expect_observed_hooks_succeeded(observation);
        expect_source_workspace_cleaned(observation);
        expect_artifact_diagnostic_retained(observation, failure);
        fixture.expect_original_tree_unchanged();
    }
}

void test_identity_mismatch_and_query_failure_remain_distinct() {
    {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request();
        ScenarioObservation observation(
            ScenarioKind::IdentityMismatch, fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);

        const ObservedFailure failure =
            execute_expect_failure(std::move(request));
        expect(
            failure.phase ==
                    LocalSourceBuildFailurePhase::ArtifactSelection &&
                failure.selection_failure.has_value() &&
                failure.selection_failure->missing_required_artifacts
                        .size() == 1,
            "Identity mismatch lost closed selection diagnostics");
        expect_observed_hooks_succeeded(observation);
        expect_source_workspace_cleaned(observation);
        expect_artifact_diagnostic_retained(observation, failure);
        fixture.expect_original_tree_unchanged();
    }

    {
        LocalBuildFixture fixture;
        LocalSourceBuildRequest request = fixture.make_request();
        ScenarioObservation observation(
            ScenarioKind::IdentityQueryFailure, fixture.source_path());
        process_stub::reset_process_stub();
        ScenarioObservationGuard guard(observation);

        const ObservedFailure failure =
            execute_expect_failure(std::move(request));
        expect(
            failure.phase ==
                    LocalSourceBuildFailurePhase::ArtifactIdentity &&
                !failure.selection_failure.has_value(),
            "Identity query failure was confused with selection failure");
        expect_observed_hooks_succeeded(observation);
        expect_source_workspace_cleaned(observation);
        expect_artifact_diagnostic_retained(observation, failure);
        fixture.expect_original_tree_unchanged();
    }
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        run_case(
            "snapshot build and correlated artifacts",
            test_success_uses_snapshot_and_returns_correlated_artifacts);
        run_case(
            "prepared projection invocation authority",
            test_prepared_projection_authority_tracks_one_invocation);
        run_case(
            "source and cache separation preflight",
            test_cache_below_source_is_rejected_during_static_preflight);
        run_case(
            "unsatisfied constraint pre-mutation firewall",
            test_unsatisfied_constraint_stops_before_local_build_mutation);
        run_case(
            "evaluated metadata environment binding",
            test_evaluated_metadata_forwards_bound_environment_in_order);
        run_case(
            "environment, PKGDEST, and plan mismatch preflight",
            test_preflight_rejects_environment_pkgdest_and_plan_mismatch);
        run_case(
            "typed source workspace failure",
            test_workspace_failure_preserves_typed_cause);
        run_case(
            "execution-time source identity revalidation",
            test_execute_revalidates_source_before_workspace_creation);
        run_case(
            "BuildContext diagnostic retention",
            test_build_context_failure_retains_artifact_diagnostics);
        run_case(
            "unsafe makepkg output cleanup",
            test_unsafe_makepkg_outputs_are_cleaned);
        run_case(
            "nonzero build diagnostic retention",
            test_build_failure_retains_only_artifact_diagnostics);
        run_case(
            "primary and secondary cleanup failure",
            test_primary_failure_preserves_secondary_source_cleanup_failure);
        run_case(
            "standalone source cleanup failure",
            test_successful_build_preserves_typed_source_cleanup_failure);
        run_case(
            "missing and unexpected artifacts",
            test_missing_and_unexpected_artifacts_fail_closed);
        run_case(
            "identity mismatch and query failure",
            test_identity_mismatch_and_query_failure_remain_distinct);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "local source build tests: all checks passed\n";
    return 0;
}
