#include "separated_package_base_source_build.hpp"
#include "invocation_owned_cleanup_adapter.hpp"

#include "stubs/artifact-install-executor/process_stub.hpp"
#include "stubs/package-metadata/alpm_stub.hpp"
#include "trusted_cache.hpp"
#include "trusted_cache_test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

PackageBaseArtifactInstallExecutionResult
RemoteAurCleanupCandidateCollector::
    execute_source_artifact_install_transaction(
        PreparedPackageBaseArtifactInstall&,
        std::size_t,
        const ArtifactInstallExecutionOptions&) {
    throw std::logic_error(
        "separated PackageBase test unexpectedly used cleanup execution");
}

#include <unistd.h>

using SeparatedPackageBaseSourceBuildExecutor =
    PackageBaseSourceBuildExecutionResult (*)(
        SeparatedPackageBaseSourceBuildRequest,
        const SeparatedSourceBuildUnitOptions&);

template <typename Value>
concept HasPathDataMember = requires(Value value) {
    value.path;
};

template <typename Value>
concept HasArtifactPathDataMember = requires(Value value) {
    value.artifact_path;
};

template <typename Value>
concept HasWorkspacePathDataMember = requires(Value value) {
    value.workspace_path;
};

template <typename Value>
concept HasArtifactIndexDataMember = requires(Value value) {
    value.artifact_index;
};

template <typename Value>
concept HasDirectiveDataMember = requires(Value value) {
    value.directive;
};

template <typename Value>
concept HasOutcomeDataMember = requires(Value value) {
    value.outcome;
};

template <typename Value>
concept HasWorkspacePathMethod = requires(const Value& value) {
    value.workspace_path();
};

template <typename Value>
concept HasArtifactPathsMethod = requires(const Value& value) {
    value.artifact_paths();
};

template <typename Value>
concept CanCleanupWorkspace = requires(Value& value) {
    value.cleanup_workspace();
};

static_assert(
    std::is_same_v<
        decltype(&execute_separated_package_base_source_build),
        SeparatedPackageBaseSourceBuildExecutor>);
static_assert(!std::is_default_constructible_v<
              PackageBaseSourceBuildExecutionResult>);
static_assert(std::is_copy_constructible_v<
              PackageBaseSourceBuildExecutionResult>);
static_assert(std::is_base_of_v<
              std::runtime_error,
              SeparatedPackageBaseSourceBuildPhaseError>);
static_assert(std::is_base_of_v<
              std::runtime_error,
              SeparatedPackageBaseSourceBuildPreparationError>);
static_assert(std::is_base_of_v<
              std::runtime_error,
              SeparatedPackageBaseSourceBuildCleanupError>);
static_assert(std::is_base_of_v<
              std::runtime_error,
              PackageBaseArtifactInstallTransactionError>);
static_assert(!HasPathDataMember<PackageBaseSourceBuildSelectedResult>);
static_assert(!HasArtifactPathDataMember<PackageBaseSourceBuildSelectedResult>);
static_assert(!HasWorkspacePathDataMember<PackageBaseSourceBuildSelectedResult>);
static_assert(!HasArtifactIndexDataMember<PackageBaseSourceBuildSelectedResult>);
static_assert(!HasDirectiveDataMember<PackageBaseSourceBuildSelectedResult>);
static_assert(!HasWorkspacePathMethod<PackageBaseSourceBuildExecutionResult>);
static_assert(!HasArtifactPathsMethod<PackageBaseSourceBuildExecutionResult>);
static_assert(!CanCleanupWorkspace<PackageBaseSourceBuildExecutionResult>);
static_assert(!HasWorkspacePathMethod<
              SeparatedPackageBaseSourceBuildCleanupError>);
static_assert(!CanCleanupWorkspace<
              SeparatedPackageBaseSourceBuildCleanupError>);
static_assert(!HasPathDataMember<PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasArtifactPathDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasWorkspacePathDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasArtifactIndexDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasDirectiveDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasOutcomeDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!CanCleanupWorkspace<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(noexcept(
    std::declval<PackageBaseSourceBuildExecutionResult&&>()
        .release_package_base()));
static_assert(noexcept(
    std::declval<PackageBaseSourceBuildExecutionResult&&>()
        .release_selected_children()));
static_assert(noexcept(
    std::declval<PackageBaseSourceBuildExecutionResult&&>()
        .release_unselected_artifacts()));
static_assert(noexcept(
    std::declval<SeparatedPackageBaseSourceBuildCleanupError&&>()
        .release_result()));

namespace {

namespace fs = std::filesystem;
namespace metadata_stub = package_metadata_test_stub;
namespace process_stub = artifact_install_executor_test_stub;

constexpr const char* PACKAGE_BASE = "sample-base";
constexpr const char* DEFAULT_VERSION = "2:1.4.0-3";

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

void write_file(
    const fs::path& path,
    const std::string& contents = "package artifact\n") {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error(
            "Failed to create set lifecycle fixture: " +
            path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error(
            "Failed to finish set lifecycle fixture: " +
            path.string());
    }
}

class ScopedWorkingDirectoryRestore final {
    fs::path working_directory_;

public:
    explicit ScopedWorkingDirectoryRestore(fs::path working_directory)
        : working_directory_(std::move(working_directory)) {
    }

    ~ScopedWorkingDirectoryRestore() noexcept {
        std::error_code error;
        fs::current_path(working_directory_, error);
    }

    ScopedWorkingDirectoryRestore(const ScopedWorkingDirectoryRestore&) = delete;
    ScopedWorkingDirectoryRestore& operator=(
        const ScopedWorkingDirectoryRestore&) = delete;
};

class TemporaryTestEnvironment final {
    fs::path original_working_directory_;
    fs::path root_path_;
    fs::path checkout_path_;
    std::optional<std::string> previous_xdg_cache_home_;
    std::optional<std::string> previous_pkgdest_;

public:
    TemporaryTestEnvironment()
        : original_working_directory_(fs::current_path()) {
        const std::string template_text =
            (fs::temp_directory_path() /
             "moguet-package-base-source-build-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create set lifecycle test directory.");
        }
        root_path_ = created_path;

        const char* previous_xdg = std::getenv("XDG_CACHE_HOME");
        if(previous_xdg != nullptr) previous_xdg_cache_home_ = previous_xdg;
        const char* previous_pkgdest = std::getenv("PKGDEST");
        if(previous_pkgdest != nullptr) previous_pkgdest_ = previous_pkgdest;

        if(setenv("XDG_CACHE_HOME", root_path_.c_str(), 1) != 0 ||
           unsetenv("PKGDEST") != 0) {
            throw std::runtime_error(
                "Failed to prepare set lifecycle test environment.");
        }

        try {
            ValidatedCacheRoot root = prepare_test_trusted_cache_root();
            checkout_path_ = root.path() / "source-checkout";
            fs::create_directory(checkout_path_);
            fs::permissions(
                checkout_path_, fs::perms::owner_all,
                fs::perm_options::replace);
            static_cast<void>(require_trusted_cache_path(
                root, checkout_path_,
                CachePathRequirement::ExistingDirectory));
        } catch(...) {
            restore_environment();
            std::error_code error;
            fs::remove_all(root_path_, error);
            throw;
        }
    }

    ~TemporaryTestEnvironment() noexcept {
        set_separated_package_base_source_build_workspace_observer_for_test(
            nullptr);
        process_stub::set_capture_hook(nullptr);
        process_stub::set_run_hook(nullptr);
        restore_environment();
        std::error_code working_directory_error;
        fs::current_path(
            original_working_directory_, working_directory_error);
        std::error_code error;
        fs::remove_all(root_path_, error);
    }

    TemporaryTestEnvironment(const TemporaryTestEnvironment&) = delete;
    TemporaryTestEnvironment& operator=(
        const TemporaryTestEnvironment&) = delete;

    SeparatedPackageBaseSourceBuildRequest request(
        const std::string& package_base,
        const std::vector<RequiredPackageArtifactTarget>& required_targets,
        const SourceBuildEnvironment& source_environment = {},
        SourceEnvironmentEmptyValuePolicy empty_value_policy =
            SourceEnvironmentEmptyValuePolicy::Omit) const {
        ValidatedCacheRoot root = prepare_test_trusted_cache_root();
        ValidatedCachePath checkout = require_trusted_cache_path(
            root, checkout_path_,
            CachePathRequirement::ExistingDirectory);
        return SeparatedPackageBaseSourceBuildRequest{
            std::move(checkout),
            prepare_private_trusted_cache_root(root),
            package_base,
            required_targets,
            source_environment,
            empty_value_policy,
            PacmanDatabasePaths{"/", "/var/lib/pacman"}};
    }

    fs::path checkout_path() const {
        return fs::canonical(checkout_path_);
    }

    const fs::path& original_working_directory() const noexcept {
        return original_working_directory_;
    }

private:
    void restore_environment() noexcept {
        if(previous_xdg_cache_home_.has_value()) {
            static_cast<void>(setenv(
                "XDG_CACHE_HOME", previous_xdg_cache_home_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("XDG_CACHE_HOME"));
        }
        if(previous_pkgdest_.has_value()) {
            static_cast<void>(setenv(
                "PKGDEST", previous_pkgdest_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("PKGDEST"));
        }
    }
};

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for(char character : value) {
        if(character == '\'')
            quoted += "'\\''";
        else
            quoted += character;
    }
    quoted += "'";
    return quoted;
}

std::string shell_join(const std::vector<std::string>& arguments) {
    std::string command;
    for(std::size_t index = 0; index < arguments.size(); ++index) {
        if(index != 0) command += " ";
        command += shell_quote(arguments[index]);
    }
    return command;
}

struct ProducedArtifactPlan {
    std::string leaf_name;
    ArtifactPackageIdentity identity;
    int identity_exit_code = 0;
    bool create_after_build = true;
};

enum class BuildHookBehavior {
    Return,
    ThrowAfterOutput,
    AddUnexpectedEntry,
};

class BuildRunnerTestError final : public std::runtime_error {
public:
    BuildRunnerTestError()
        : std::runtime_error("Fixture build runner failed.") {
    }
};

class InstallRunnerTestError final : public std::runtime_error {
public:
    InstallRunnerTestError()
        : std::runtime_error("Fixture pacman runner failed.") {
    }
};

struct LifecycleScenario {
    std::string package_base = PACKAGE_BASE;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    std::vector<ProducedArtifactPlan> produced_artifacts;
    SeparatedSourceBuildUnitOptions options;
    SourceBuildEnvironment source_environment;
    SourceEnvironmentEmptyValuePolicy empty_value_policy =
        SourceEnvironmentEmptyValuePolicy::Omit;

    int build_exit_code = 0;
    BuildHookBehavior build_hook_behavior = BuildHookBehavior::Return;
    std::optional<std::size_t> identity_query_count;
    bool expect_install = true;
    int install_exit_code = 0;
    const char* expected_reason_option = nullptr;
    bool throw_during_install = false;
    bool replace_workspace_after_build = false;
    bool replace_workspace_after_install = false;

    fs::path expected_checkout_path;
    fs::path caller_working_directory;
    fs::path workspace_path;
    fs::path displaced_workspace_path;
    std::vector<fs::path> artifact_paths;
    std::size_t packagelist_calls = 0;
    std::size_t build_calls = 0;
    std::size_t identity_calls = 0;
    std::size_t install_calls = 0;
};

LifecycleScenario* g_scenario = nullptr;

RequiredPackageArtifactTarget target(
    const std::string& package_name,
    DesiredInstallReason desired_reason = DesiredInstallReason::Explicit,
    const std::string& package_base = PACKAGE_BASE) {
    return RequiredPackageArtifactTarget{
        package_base, package_name, desired_reason};
}

ProducedArtifactPlan artifact(
    const std::string& package_name,
    const std::string& version = DEFAULT_VERSION,
    const std::string& leaf_name = {}) {
    return ProducedArtifactPlan{
        leaf_name.empty()
            ? package_name + "-1.0-1-x86_64.pkg.tar.zst"
            : leaf_name,
        ArtifactPackageIdentity{package_name, version}};
}

std::string source_environment_prefix(
    const LifecycleScenario& scenario,
    const fs::path& workspace_path) {
    std::string prefix;
    for(const SourceEnvironmentAssignment& assignment :
        scenario.source_environment.ordered_assignments) {
        if(assignment.value.empty() &&
           scenario.empty_value_policy ==
               SourceEnvironmentEmptyValuePolicy::Omit) {
            continue;
        }
        prefix += assignment.key + "=" + shell_quote(assignment.value) + " ";
    }
    prefix += "PKGDEST=" + shell_quote(workspace_path.string()) + " ";
    return prefix;
}

std::vector<std::string> source_environment_assignment_words(
    const LifecycleScenario& scenario,
    const fs::path& workspace_path) {
    std::vector<std::string> words;
    for(const SourceEnvironmentAssignment& assignment :
        scenario.source_environment.ordered_assignments) {
        if(assignment.value.empty() &&
           scenario.empty_value_policy ==
               SourceEnvironmentEmptyValuePolicy::Omit) {
            continue;
        }
        words.push_back(assignment.key + "=" + assignment.value);
    }
    words.push_back("PKGDEST=" + workspace_path.string());
    return words;
}

std::string expected_packagelist_command(
    const LifecycleScenario& scenario,
    const fs::path& workspace_path) {
    std::vector<std::string> arguments{"makepkg", "--packagelist"};
    const std::vector<std::string> assignment_words =
        source_environment_assignment_words(scenario, workspace_path);
    arguments.insert(
        arguments.end(), assignment_words.begin(),
        assignment_words.end());
    return source_environment_prefix(scenario, workspace_path) +
           shell_join(arguments);
}

std::string expected_build_command(
    const LifecycleScenario& scenario,
    const fs::path& workspace_path) {
    std::vector<std::string> arguments{"makepkg", "-sc"};
    if(scenario.options.no_confirm) arguments.emplace_back("--noconfirm");
    if(scenario.options.rebuild) arguments.emplace_back("-f");
    if(scenario.options.clean_build) arguments.emplace_back("-C");
    const std::vector<std::string> assignment_words =
        source_environment_assignment_words(scenario, workspace_path);
    arguments.insert(
        arguments.end(), assignment_words.begin(),
        assignment_words.end());
    return source_environment_prefix(scenario, workspace_path) +
           shell_join(arguments);
}

std::string expected_identity_command(const fs::path& artifact_path) {
    return "LC_ALL=C " + shell_join(
                             {"pacman", "-Qp", "--color", "never", "--",
                              artifact_path.string()});
}

std::size_t produced_index_for_required_target(
    const LifecycleScenario& scenario,
    const RequiredPackageArtifactTarget& required_target) {
    const auto found = std::find_if(
        scenario.produced_artifacts.begin(),
        scenario.produced_artifacts.end(),
        [&required_target](const ProducedArtifactPlan& produced) {
            return produced.identity.package_name ==
                   required_target.package_name;
        });
    if(found == scenario.produced_artifacts.end()) {
        throw std::logic_error(
            "Install expectation has no matching produced artifact.");
    }
    return static_cast<std::size_t>(
        std::distance(scenario.produced_artifacts.begin(), found));
}

std::string expected_install_command(const LifecycleScenario& scenario) {
    std::vector<std::string> arguments{"sudo", "pacman", "-U"};
    if(scenario.options.no_confirm) arguments.emplace_back("--noconfirm");
    if(scenario.options.needed) arguments.emplace_back("--needed");
    if(scenario.expected_reason_option != nullptr) {
        arguments.emplace_back(scenario.expected_reason_option);
    }
    arguments.emplace_back("--");
    for(const RequiredPackageArtifactTarget& required_target :
        scenario.required_targets) {
        arguments.push_back(
            scenario.artifact_paths
                .at(produced_index_for_required_target(
                    scenario, required_target))
                .string());
    }
    return shell_join(arguments);
}

void observe_workspace(const fs::path& workspace_path) {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Set lifecycle created a workspace outside an active scenario.");
    }
    LifecycleScenario& scenario = *g_scenario;
    if(!scenario.workspace_path.empty()) {
        throw std::logic_error(
            "Set lifecycle created more than one workspace.");
    }
    scenario.workspace_path = workspace_path;
    scenario.artifact_paths.clear();
    std::string packagelist_output;
    for(const ProducedArtifactPlan& produced : scenario.produced_artifacts) {
        scenario.artifact_paths.push_back(
            workspace_path / produced.leaf_name);
        packagelist_output += scenario.artifact_paths.back().string() + "\n";
    }

    process_stub::expect_capture_command(
        expected_packagelist_command(scenario, workspace_path),
        CapturedCommandResult{std::move(packagelist_output), 0});
    process_stub::expect_run_command(
        expected_build_command(scenario, workspace_path),
        scenario.build_exit_code);

    const std::size_t identity_query_count =
        scenario.identity_query_count.value_or(
            scenario.produced_artifacts.size());
    if(identity_query_count > scenario.produced_artifacts.size()) {
        throw std::logic_error(
            "Identity query expectation exceeds produced artifacts.");
    }
    for(std::size_t index = 0; index < identity_query_count; ++index) {
        const ProducedArtifactPlan& produced =
            scenario.produced_artifacts[index];
        process_stub::expect_capture_command(
            expected_identity_command(scenario.artifact_paths[index]),
            CapturedCommandResult{
                produced.identity.package_name + " " +
                    produced.identity.full_version + "\n",
                produced.identity_exit_code});
    }
    if(scenario.expect_install) {
        process_stub::expect_run_command(
            expected_install_command(scenario),
            scenario.install_exit_code);
    }
}

void observe_capture_command() {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Process capture occurred outside an active set scenario.");
    }
    LifecycleScenario& scenario = *g_scenario;
    const std::string command = process_stub::last_captured_command();
    if(command.starts_with("LC_ALL=C ")) {
        ++scenario.identity_calls;
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Identity query ran before restoring the caller directory");
        for(const fs::path& artifact_path : scenario.artifact_paths) {
            if(fs::exists(artifact_path)) {
                expect(
                    fs::is_regular_file(artifact_path),
                    "Identity query observed a non-regular artifact");
            }
        }
        return;
    }

    ++scenario.packagelist_calls;
    expect(
        fs::current_path() == scenario.expected_checkout_path,
        "Packagelist query did not run from the checkout");
    expect(
        fs::is_directory(scenario.workspace_path),
        "Packagelist query ran without its workspace");
    for(const fs::path& artifact_path : scenario.artifact_paths) {
        expect(
            !fs::exists(artifact_path),
            "Fresh workspace contained an artifact before build");
    }
}

void require_metadata_released_before_install() {
    expect(
        metadata_stub::created_handle_count() == 1,
        "PackageBase transaction did not use one metadata session");
    expect(
        metadata_stub::release_call_count() == 1 &&
            metadata_stub::release_count_for_handle(0) == 1,
        "PackageBase transaction started before metadata release");
}

void create_build_outputs(LifecycleScenario& scenario) {
    for(std::size_t index = 0;
        index < scenario.produced_artifacts.size(); ++index) {
        if(scenario.produced_artifacts[index].create_after_build) {
            write_file(scenario.artifact_paths[index]);
        }
    }
    if(scenario.build_hook_behavior ==
       BuildHookBehavior::AddUnexpectedEntry) {
        write_file(
            scenario.workspace_path / "unexpected-build-output",
            "unexpected\n");
    }
}

void observe_run_command() {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Process run occurred outside an active set scenario.");
    }
    LifecycleScenario& scenario = *g_scenario;
    const std::string command = process_stub::last_run_command();
    if(command.find("'makepkg' '-sc'") != std::string::npos) {
        ++scenario.build_calls;
        expect(
            scenario.packagelist_calls == 1,
            "Build started before its packagelist query");
        expect(
            metadata_stub::initialize_call_count() == 0,
            "Build started after metadata was opened");
        expect(
            fs::current_path() == scenario.expected_checkout_path,
            "Build-only makepkg did not run from the checkout");
        create_build_outputs(scenario);
        if(scenario.build_hook_behavior ==
           BuildHookBehavior::ThrowAfterOutput) {
            throw BuildRunnerTestError();
        }
        if(scenario.replace_workspace_after_build) {
            scenario.displaced_workspace_path = fs::path(
                scenario.workspace_path.string() +
                ".build-succeeded-before-revalidation");
            fs::rename(
                scenario.workspace_path,
                scenario.displaced_workspace_path);
            fs::create_directory(scenario.workspace_path);
            fs::permissions(
                scenario.workspace_path, fs::perms::owner_all,
                fs::perm_options::replace);
        }
        return;
    }

    ++scenario.install_calls;
    expect(
        fs::current_path() == scenario.caller_working_directory,
        "pacman transaction ran before restoring the caller directory");
    require_metadata_released_before_install();
    if(scenario.throw_during_install) throw InstallRunnerTestError();

    if(scenario.replace_workspace_after_install) {
        scenario.displaced_workspace_path = fs::path(
            scenario.workspace_path.string() +
            ".transaction-succeeded-before-cleanup");
        fs::rename(
            scenario.workspace_path,
            scenario.displaced_workspace_path);
        fs::create_directory(scenario.workspace_path);
        fs::permissions(
            scenario.workspace_path, fs::perms::owner_all,
            fs::perm_options::replace);
    }
}

void activate_scenario(
    LifecycleScenario& scenario,
    const TemporaryTestEnvironment& environment) {
    process_stub::reset_process_stub();
    metadata_stub::reset_alpm_stub();
    scenario.expected_checkout_path = environment.checkout_path();
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.workspace_path.clear();
    scenario.displaced_workspace_path.clear();
    scenario.artifact_paths.clear();
    scenario.packagelist_calls = 0;
    scenario.build_calls = 0;
    scenario.identity_calls = 0;
    scenario.install_calls = 0;
    g_scenario = &scenario;
    set_separated_package_base_source_build_workspace_observer_for_test(
        observe_workspace);
    process_stub::set_capture_hook(observe_capture_command);
    process_stub::set_run_hook(observe_run_command);
}

void finish_scenario(
    const LifecycleScenario& scenario,
    std::size_t expected_workspace_count = 1) {
    process_stub::require_process_expectations_consumed();
    metadata_stub::require_local_package_query_expectations_consumed();
    expect(
        (scenario.workspace_path.empty() ? 0U : 1U) ==
            expected_workspace_count,
        "Set lifecycle workspace count differs");
    expect(
        scenario.packagelist_calls == expected_workspace_count,
        "Set lifecycle packagelist count differs");
    expect(
        scenario.build_calls == expected_workspace_count,
        "Set lifecycle build count differs");
    set_separated_package_base_source_build_workspace_observer_for_test(
        nullptr);
    process_stub::set_capture_hook(nullptr);
    process_stub::set_run_hook(nullptr);
    g_scenario = nullptr;
}

PackageBaseSourceBuildExecutionResult execute_scenario(
    const TemporaryTestEnvironment& environment,
    LifecycleScenario& scenario) {
    ScopedWorkingDirectoryRestore working_directory_restore(
        environment.original_working_directory());
    expect(
        fs::current_path() == scenario.caller_working_directory,
        "Set lifecycle scenario started from a drifted directory");
    try {
        PackageBaseSourceBuildExecutionResult result =
            execute_separated_package_base_source_build(
                environment.request(
                    scenario.package_base,
                    scenario.required_targets,
                    scenario.source_environment,
                    scenario.empty_value_policy),
                scenario.options);
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Set lifecycle success leaked its working directory");
        return result;
    } catch(...) {
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Set lifecycle failure leaked its working directory");
        throw;
    }
}

template <typename Callable>
std::string expect_runtime_error(
    Callable&& callable,
    const std::string& context,
    const std::string& expected_fragment) {
    try {
        std::forward<Callable>(callable)();
    } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
        throw std::runtime_error(
            context + ": unexpected typed preparation error: " +
            error.what());
    } catch(const SeparatedPackageBaseSourceBuildCleanupError& error) {
        throw std::runtime_error(
            context + ": unexpected cleanup partial success: " +
            error.what());
    } catch(const std::logic_error& error) {
        throw std::runtime_error(
            context + ": unexpected logic_error: " + error.what());
    } catch(const std::runtime_error& error) {
        if(std::string(error.what()).find(expected_fragment) ==
           std::string::npos) {
            throw std::runtime_error(
                context + ": unexpected diagnostic [" + error.what() +
                "]");
        }
        return error.what();
    }
    throw std::runtime_error(context + ": expected runtime_error");
}

template <typename Callable>
void expect_logic_error(
    Callable&& callable,
    const std::string& context,
    const std::string& expected_fragment) {
    try {
        std::forward<Callable>(callable)();
    } catch(const std::logic_error& error) {
        expect(
            std::string(error.what()).find(expected_fragment) !=
                std::string::npos,
            context + ": logic_error diagnostic differs");
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " +
            error.what());
    }
    throw std::runtime_error(context + ": expected logic_error");
}

void expect_selected_child(
    const PackageBaseSourceBuildExecutionResult& result,
    std::size_t index,
    const ArtifactPackageIdentity& identity,
    DesiredInstallReason desired_reason,
    ArtifactInstallExecutionOutcome outcome,
    const std::string& context) {
    expect(
        index < result.selected_children().size(),
        context + ": selected child index is missing");
    const PackageBaseSourceBuildSelectedResult& child =
        result.selected_children()[index];
    expect(
        child.identity.package_name == identity.package_name &&
            child.identity.full_version == identity.full_version &&
            child.desired_reason == desired_reason &&
            child.outcome == outcome,
        context + ": selected child payload differs");
}

void expect_unselected(
    const PackageBaseSourceBuildExecutionResult& result,
    const std::vector<ArtifactPackageIdentity>& expected,
    const std::string& context) {
    expect(
        result.unselected_artifacts().size() == expected.size(),
        context + ": unselected identity count differs");
    for(std::size_t index = 0; index < expected.size(); ++index) {
        expect(
            result.unselected_artifacts()[index].package_name ==
                    expected[index].package_name &&
                result.unselected_artifacts()[index].full_version ==
                    expected[index].full_version,
            context + ": unselected identity/order differs");
    }
}

void expect_retained_workspace(
    const LifecycleScenario& scenario,
    const std::string& context) {
    expect(
        !scenario.workspace_path.empty() &&
            fs::is_directory(scenario.workspace_path),
        context + ": diagnostic workspace was not retained");
}

void test_ordinary_size_one(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.required_targets = {target("ordinary")};
    scenario.produced_artifacts = {artifact("ordinary")};
    activate_scenario(scenario, environment);
    metadata_stub::enqueue_local_package_query_absent("ordinary");

    const PackageBaseSourceBuildExecutionResult result =
        execute_scenario(environment, scenario);

    expect(result.package_base() == PACKAGE_BASE, "Ordinary PackageBase differs");
    expect(result.selected_children().size() == 1,
           "Ordinary selected child count differs");
    expect_selected_child(
        result, 0, scenario.produced_artifacts[0].identity,
        DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "ordinary set lifecycle");
    expect_unselected(result, {}, "ordinary set lifecycle");
    expect(result.installed_any() && !result.all_skipped_as_needed(),
           "Ordinary aggregate outcome differs");
    expect(!fs::exists(scenario.workspace_path),
           "Ordinary success did not clean its workspace");
    finish_scenario(scenario);
}

void test_split_child_and_unselected_outputs(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.package_base = "split-suite";
    scenario.required_targets = {
        target(
            "split-child", DesiredInstallReason::Explicit,
            "split-suite")};
    scenario.produced_artifacts = {
        artifact("split-suite"),
        artifact("split-child"),
        artifact("split-debug")};
    activate_scenario(scenario, environment);
    metadata_stub::enqueue_local_package_query_absent("split-child");

    const PackageBaseSourceBuildExecutionResult result =
        execute_scenario(environment, scenario);

    expect_selected_child(
        result, 0, scenario.produced_artifacts[1].identity,
        DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "requested split child");
    expect_unselected(
        result,
        {scenario.produced_artifacts[0].identity,
         scenario.produced_artifacts[2].identity},
        "requested split child");
    expect(
        metadata_stub::local_package_query_history() ==
            std::vector<std::string>{"split-child"},
        "Unselected split output reached metadata query");
    expect(
        process_stub::last_run_command().find(
            scenario.artifact_paths[0].string()) ==
                std::string::npos &&
            process_stub::last_run_command().find(
                scenario.artifact_paths[2].string()) ==
                std::string::npos,
        "Unselected split output reached pacman transaction");
    finish_scenario(scenario);
}

void test_multiple_required_order_and_mixed_needed_outcomes(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.options.no_confirm = true;
    scenario.options.needed = true;
    scenario.required_targets = {
        target("child-a"), target("child-b")};
    scenario.produced_artifacts = {
        artifact("sibling", "9-1"),
        artifact("child-b", "2-1"),
        artifact("debug-output", "8-1"),
        artifact("child-a", "1-1")};
    activate_scenario(scenario, environment);
    metadata_stub::enqueue_local_package_query_absent("child-a");
    metadata_stub::enqueue_local_package_query_present(
        "child-b", "child-b", "2-1",
        ALPM_PKG_REASON_EXPLICIT);

    const PackageBaseSourceBuildExecutionResult result =
        execute_scenario(environment, scenario);

    expect(result.selected_children().size() == 2,
           "Multiple selected child count differs");
    expect_selected_child(
        result, 0, scenario.produced_artifacts[3].identity,
        DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "multiple first required child");
    expect_selected_child(
        result, 1, scenario.produced_artifacts[1].identity,
        DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::SkippedAsNeeded,
        "multiple second required child");
    expect_unselected(
        result,
        {scenario.produced_artifacts[0].identity,
         scenario.produced_artifacts[2].identity},
        "multiple produced order");
    expect(result.installed_any() && !result.all_skipped_as_needed(),
           "Mixed needed aggregate outcome was flattened");
    expect(scenario.install_calls == 1,
           "Multiple required children used more than one transaction");
    expect(
        metadata_stub::local_package_query_history() ==
            std::vector<std::string>{"child-a", "child-b"},
        "Multiple metadata queries do not preserve required order");
    finish_scenario(scenario);
}

void test_dependency_reason(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.required_targets = {
        target("dependency-child", DesiredInstallReason::Dependency)};
    scenario.produced_artifacts = {artifact("dependency-child")};
    scenario.expected_reason_option = "--asdeps";
    activate_scenario(scenario, environment);
    metadata_stub::enqueue_local_package_query_absent("dependency-child");

    const PackageBaseSourceBuildExecutionResult result =
        execute_scenario(environment, scenario);

    expect_selected_child(
        result, 0, scenario.produced_artifacts[0].identity,
        DesiredInstallReason::Dependency,
        ArtifactInstallExecutionOutcome::Installed,
        "dependency reason");
    expect(
        process_stub::last_run_command().find("'--asdeps'") !=
            std::string::npos,
        "Dependency reason did not reach the transaction");
    finish_scenario(scenario);
}

void test_typed_preparation_failures(
    const TemporaryTestEnvironment& environment) {
    {
        LifecycleScenario scenario;
        scenario.required_targets = {
            target("root-child"),
            target("dependency-child", DesiredInstallReason::Dependency)};
        scenario.produced_artifacts = {
            artifact("root-child"), artifact("dependency-child")};
        scenario.expect_install = false;
        activate_scenario(scenario, environment);
        metadata_stub::enqueue_local_package_query_absent("root-child");
        metadata_stub::enqueue_local_package_query_absent("dependency-child");

        try {
            static_cast<void>(execute_scenario(environment, scenario));
            throw std::runtime_error("Mixed reason failure was not reported.");
        } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
            expect(error.selection_failure() == nullptr,
                   "Mixed reason error has a selection arm");
            const MixedPackageBaseInstallReasonUnsupported* mixed =
                error.mixed_reason_failure();
            expect(
                mixed != nullptr && mixed->package_base == PACKAGE_BASE &&
                    mixed->selected_artifacts.size() == 2,
                "Mixed reason typed detail differs");
        }
        expect_retained_workspace(scenario, "mixed reason failure");
        finish_scenario(scenario);
    }

    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("missing-child")};
        scenario.produced_artifacts = {artifact("unselected-sibling")};
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        try {
            static_cast<void>(execute_scenario(environment, scenario));
            throw std::runtime_error("Missing child failure was not reported.");
        } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
            const PackageBaseArtifactIdentitySelectionFailure* failure =
                error.selection_failure();
            expect(
                failure != nullptr &&
                    failure->missing_required_artifacts.size() == 1 &&
                    failure->missing_required_artifacts[0]
                            .target.package_name ==
                        "missing-child",
                "Missing child typed detail differs");
        }
        expect_retained_workspace(scenario, "missing child failure");
        finish_scenario(scenario);
    }

    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("duplicate-child")};
        scenario.produced_artifacts = {
            artifact("duplicate-child", "1-1", "duplicate-a.pkg.tar.zst"),
            artifact("duplicate-child", "2-1", "duplicate-b.pkg.tar.zst")};
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        try {
            static_cast<void>(execute_scenario(environment, scenario));
            throw std::runtime_error(
                "Duplicate produced identity failure was not reported.");
        } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
            const PackageBaseArtifactIdentitySelectionFailure* failure =
                error.selection_failure();
            expect(
                failure != nullptr &&
                    failure->duplicate_produced_identities.size() == 1 &&
                    failure->duplicate_produced_identities[0]
                            .package_name ==
                        "duplicate-child",
                "Duplicate produced identity typed detail differs");
        }
        expect_retained_workspace(
            scenario, "duplicate produced identity failure");
        finish_scenario(scenario);
    }
}

void test_static_request_failures_before_workspace(
    const TemporaryTestEnvironment& environment) {
    struct StaticFailureCase {
        const char* context;
        const char* expected_fragment;
        std::vector<RequiredPackageArtifactTarget> required_targets;
    };
    const std::vector<StaticFailureCase> cases = {
        {
            "duplicate required target",
            "duplicate required package target",
            {target("duplicate"), target("duplicate")},
        },
        {
            "PackageBase attribution mismatch",
            "mismatched PackageBase",
            {target(
                "mismatched", DesiredInstallReason::Explicit,
                "different-base")},
        },
        {
            "unknown desired reason",
            "unknown install reason",
            {target(
                "unknown-reason",
                static_cast<DesiredInstallReason>(255))},
        },
    };

    for(const StaticFailureCase& test_case : cases) {
        LifecycleScenario scenario;
        scenario.required_targets = test_case.required_targets;
        scenario.produced_artifacts = {artifact("unused")};
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        expect_logic_error(
            [&]() {
                static_cast<void>(execute_scenario(environment, scenario));
            },
            test_case.context,
            test_case.expected_fragment);
        expect(
            scenario.workspace_path.empty() &&
                process_stub::capture_command_call_count() == 0 &&
                process_stub::run_command_call_count() == 0 &&
                metadata_stub::initialize_call_count() == 0,
            std::string(test_case.context) +
                ": static rejection crossed a mutation boundary");
        finish_scenario(scenario, 0);
    }
}

void test_rmdeps_rejected_before_workspace(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.options.no_confirm = true;
    scenario.options.rm_deps = true;
    scenario.required_targets = {target("rmdeps-child")};
    scenario.produced_artifacts = {artifact("rmdeps-child")};
    scenario.expect_install = false;
    activate_scenario(scenario, environment);

    static_cast<void>(expect_runtime_error(
        [&]() {
            static_cast<void>(execute_scenario(environment, scenario));
        },
        "--rmdeps set preflight",
        "Separated build/install does not support --rmdeps."));
    expect(
        scenario.workspace_path.empty() &&
            process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0 &&
            metadata_stub::initialize_call_count() == 0,
        "--rmdeps set preflight crossed an external boundary");
    finish_scenario(scenario, 0);
}

void test_identity_and_metadata_failures(
    const TemporaryTestEnvironment& environment) {
    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("identity-failure")};
        scenario.produced_artifacts = {artifact("identity-failure")};
        scenario.produced_artifacts[0].identity_exit_code = 29;
        scenario.identity_query_count = 1;
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        bool identity_failure_reported = false;
        try {
            static_cast<void>(execute_scenario(environment, scenario));
        } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
            identity_failure_reported = true;
            expect(
                error.phase() ==
                    SeparatedPackageBaseSourceBuildFailurePhase::
                        ArtifactIdentity,
                "Identity failure phase differs");
            expect(
                std::string(error.what()) ==
                    "PackageBase artifact identity or validation failed before install preparation.",
                "Identity failure diagnostic differs");
        }
        expect(
            identity_failure_reported,
            "Identity failure did not report a typed phase");
        expect(metadata_stub::initialize_call_count() == 0,
               "Identity failure opened metadata");
        expect_retained_workspace(scenario, "identity query failure");
        finish_scenario(scenario);
    }

    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("metadata-failure")};
        scenario.produced_artifacts = {artifact("metadata-failure")};
        scenario.expect_install = false;
        activate_scenario(scenario, environment);
        metadata_stub::enqueue_local_package_query_failure(
            "metadata-failure", ALPM_ERR_DB_OPEN);

        try {
            static_cast<void>(execute_scenario(environment, scenario));
            throw std::runtime_error("Metadata failure was not reported.");
        } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
            expect(
                error.phase() ==
                        SeparatedPackageBaseSourceBuildFailurePhase::
                            InstallPreparation &&
                    error.package_metadata_failure().has_value() &&
                    error.package_metadata_failure()->code ==
                        PackageMetadataErrorCode::QueryFailed &&
                    error.production_outcome().has_value() &&
                    error.production_outcome()->build_outcome ==
                        ProductionSourceBuildCommandOutcome::
                            Succeeded &&
                    error.production_outcome()->install_outcome ==
                        ProductionSourceInstallOutcome::Failed,
                "Metadata failure lost typed staged outcome");
        }
        expect(
            metadata_stub::release_call_count() == 1 &&
                metadata_stub::release_count_for_handle(0) == 1,
            "Metadata failure leaked its session");
        expect_retained_workspace(scenario, "metadata failure");
        finish_scenario(scenario);
    }
}

void test_transaction_failures_have_no_public_result(
    const TemporaryTestEnvironment& environment) {
    {
        LifecycleScenario scenario;
        scenario.required_targets = {
            target("pacman-nonzero-a"),
            target("pacman-nonzero-b")};
        scenario.produced_artifacts = {
            artifact("unselected-sibling", "9-1"),
            artifact("pacman-nonzero-b", "2-1"),
            artifact("pacman-nonzero-a", "1-1")};
        scenario.install_exit_code = 73;
        activate_scenario(scenario, environment);
        metadata_stub::enqueue_local_package_query_absent("pacman-nonzero-a");
        metadata_stub::enqueue_local_package_query_absent("pacman-nonzero-b");
        std::optional<PackageBaseSourceBuildExecutionResult> public_result;

        bool transaction_failure_reported = false;
        try {
            public_result.emplace(execute_scenario(environment, scenario));
        } catch(const PackageBaseArtifactInstallTransactionError& error) {
            transaction_failure_reported = true;
            expect(
                error.failure_kind() ==
                        PackageBaseArtifactInstallTransactionFailureKind::
                            NonzeroExit &&
                    error.package_base() == PACKAGE_BASE &&
                    error.exit_code() == std::optional<int>{73},
                "pacman nonzero typed failure differs");
            expect(
                error.production_outcome().has_value() &&
                    error.production_outcome()->build_outcome ==
                        ProductionSourceBuildCommandOutcome::
                            Succeeded &&
                    error.production_outcome()->install_outcome ==
                        ProductionSourceInstallOutcome::Failed,
                "pacman nonzero lost successful build/failed install outcome");
            expect(
                error.attempts().size() == 2 &&
                    error.attempts()[0].identity.package_name ==
                        scenario.produced_artifacts[2]
                            .identity.package_name &&
                    error.attempts()[0].identity.full_version ==
                        scenario.produced_artifacts[2]
                            .identity.full_version &&
                    error.attempts()[0].desired_reason ==
                        DesiredInstallReason::Explicit &&
                    error.attempts()[1].identity.package_name ==
                        scenario.produced_artifacts[1]
                            .identity.package_name &&
                    error.attempts()[1].identity.full_version ==
                        scenario.produced_artifacts[1]
                            .identity.full_version &&
                    error.attempts()[1].desired_reason ==
                        DesiredInstallReason::Explicit,
                "pacman nonzero attempt snapshot lost required order");
            expect(
                std::string(error.what()) ==
                    "pacman -U failed with exit code 73.",
                "pacman nonzero diagnostic differs");
        }
        expect(
            transaction_failure_reported,
            "pacman nonzero did not report typed transaction failure");
        expect(!public_result.has_value(),
               "pacman nonzero fabricated child success");
        expect_retained_workspace(scenario, "pacman nonzero");
        finish_scenario(scenario);
    }

    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("pacman-exception")};
        scenario.produced_artifacts = {artifact("pacman-exception")};
        scenario.throw_during_install = true;
        activate_scenario(scenario, environment);
        metadata_stub::enqueue_local_package_query_absent("pacman-exception");
        std::optional<PackageBaseSourceBuildExecutionResult> public_result;

        bool process_failure_reported = false;
        try {
            public_result.emplace(execute_scenario(environment, scenario));
            throw std::runtime_error("pacman exception was not propagated.");
        } catch(const PackageBaseArtifactInstallTransactionError& error) {
            process_failure_reported = true;
            expect(
                error.failure_kind() ==
                        PackageBaseArtifactInstallTransactionFailureKind::
                            ProcessException &&
                    error.package_base() == PACKAGE_BASE &&
                    !error.exit_code().has_value() &&
                    error.attempts().size() == 1 &&
                    error.attempts()[0].identity.package_name ==
                        scenario.produced_artifacts[0]
                            .identity.package_name &&
                    error.attempts()[0].identity.full_version ==
                        scenario.produced_artifacts[0]
                            .identity.full_version &&
                    error.attempts()[0].desired_reason ==
                        DesiredInstallReason::Explicit,
                "pacman exception typed detail differs");
            expect(
                std::string(error.what()) ==
                    "The pacman -U transaction execution threw an exception.",
                "pacman exception safe diagnostic differs");
        }
        expect(
            process_failure_reported,
            "pacman exception did not report typed transaction failure");
        expect(!public_result.has_value(),
               "pacman exception fabricated child success");
        expect_retained_workspace(scenario, "pacman exception");
        finish_scenario(scenario);
    }
}

void test_cleanup_failure_retains_full_result(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.options.needed = true;
    scenario.required_targets = {target("install-child"), target("skip-child")};
    scenario.produced_artifacts = {
        artifact("unselected-sibling", "9-1"),
        artifact("skip-child", "2-1"),
        artifact("unselected-debug", "8-1"),
        artifact("install-child", "1-1")};
    scenario.replace_workspace_after_install = true;
    activate_scenario(scenario, environment);
    metadata_stub::enqueue_local_package_query_absent("install-child");
    metadata_stub::enqueue_local_package_query_present(
        "skip-child", "skip-child", "2-1",
        ALPM_PKG_REASON_EXPLICIT);

    try {
        static_cast<void>(execute_scenario(environment, scenario));
        throw std::runtime_error("Cleanup failure was not reported.");
    } catch(const SeparatedPackageBaseSourceBuildCleanupError& error) {
        const PackageBaseSourceBuildExecutionResult& result = error.result();
        expect(result.package_base() == PACKAGE_BASE,
               "Cleanup error lost PackageBase");
        expect_selected_child(
            result, 0, scenario.produced_artifacts[3].identity,
            DesiredInstallReason::Explicit,
            ArtifactInstallExecutionOutcome::Installed,
            "cleanup error installed child");
        expect_selected_child(
            result, 1, scenario.produced_artifacts[1].identity,
            DesiredInstallReason::Explicit,
            ArtifactInstallExecutionOutcome::SkippedAsNeeded,
            "cleanup error skipped child");
        expect_unselected(
            result,
            {scenario.produced_artifacts[0].identity,
             scenario.produced_artifacts[2].identity},
            "cleanup error unselected snapshot");
        expect(result.installed_any() && !result.all_skipped_as_needed(),
               "Cleanup error flattened mixed child outcomes");
        expect(
            std::string(error.what()).find("Package installation succeeded") !=
                    std::string::npos &&
                std::string(error.what()).find("pacman -U failed") ==
                    std::string::npos,
            "Cleanup failure was flattened to transaction failure");
    }
    expect(
        fs::is_directory(scenario.workspace_path) &&
            fs::is_regular_file(
                scenario.displaced_workspace_path /
                scenario.produced_artifacts[3].leaf_name),
        "Cleanup failure lost diagnostic workspace contents");
    finish_scenario(scenario);
}

void test_build_and_validation_failures_retain_diagnostics(
    const TemporaryTestEnvironment& environment) {
    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("build-failure")};
        scenario.produced_artifacts = {artifact("build-failure")};
        scenario.produced_artifacts[0].create_after_build = false;
        scenario.build_exit_code = 47;
        scenario.identity_query_count = 0;
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        bool build_failure_reported = false;
        try {
            static_cast<void>(execute_scenario(environment, scenario));
        } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
            build_failure_reported = true;
            expect(
                error.phase() ==
                        SeparatedPackageBaseSourceBuildFailurePhase::
                            Build &&
                    std::string(error.what()) ==
                        "The build-only makepkg command failed with exit code 47.",
                "Build nonzero typed failure differs");
        }
        expect(
            build_failure_reported,
            "Build nonzero did not report a typed phase");
        expect_retained_workspace(scenario, "build nonzero");
        finish_scenario(scenario);
    }

    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("build-exception")};
        scenario.produced_artifacts = {artifact("build-exception")};
        scenario.build_hook_behavior = BuildHookBehavior::ThrowAfterOutput;
        scenario.identity_query_count = 0;
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        bool build_exception_reported = false;
        try {
            static_cast<void>(execute_scenario(environment, scenario));
            throw std::runtime_error("Build exception was not propagated.");
        } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
            build_exception_reported = true;
            expect(
                error.phase() ==
                        SeparatedPackageBaseSourceBuildFailurePhase::
                            Build &&
                    std::string(error.what()) ==
                        "The PackageBase build-only makepkg execution failed.",
                "Build exception typed failure differs");
        }
        expect(
            build_exception_reported,
            "Build exception did not report a typed phase");
        expect_retained_workspace(scenario, "build exception");
        expect(fs::is_regular_file(scenario.artifact_paths[0]),
               "Build exception lost produced artifact");
        finish_scenario(scenario);
    }

    {
        LifecycleScenario scenario;
        scenario.required_targets = {target("validation-a")};
        scenario.produced_artifacts = {
            artifact("validation-a"), artifact("validation-b")};
        scenario.produced_artifacts[1].create_after_build = false;
        scenario.identity_query_count = 0;
        scenario.expect_install = false;
        activate_scenario(scenario, environment);

        bool validation_failure_reported = false;
        try {
            static_cast<void>(execute_scenario(environment, scenario));
        } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
            validation_failure_reported = true;
            expect(
                error.phase() ==
                        SeparatedPackageBaseSourceBuildFailurePhase::
                            ArtifactValidation &&
                    std::string(error.what()) ==
                        "PackageBase artifact aggregate validation failed.",
                "Aggregate validation typed failure differs");
        }
        expect(
            validation_failure_reported,
            "Aggregate validation did not report a typed phase");
        expect_retained_workspace(scenario, "aggregate validation failure");
        expect(fs::is_regular_file(scenario.artifact_paths[0]),
               "Validation failure lost the produced sibling");
        finish_scenario(scenario);
    }
}

void test_status_zero_post_build_revalidation_retains_succeeded_outcome(
    const TemporaryTestEnvironment& environment) {
    LifecycleScenario scenario;
    scenario.required_targets = {target("postcheck-child")};
    scenario.produced_artifacts = {artifact("postcheck-child")};
    scenario.identity_query_count = 0;
    scenario.expect_install = false;
    scenario.replace_workspace_after_build = true;
    activate_scenario(scenario, environment);

    bool failure_reported = false;
    try {
        static_cast<void>(execute_scenario(environment, scenario));
    } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
        failure_reported = true;
        expect(
            error.phase() ==
                    SeparatedPackageBaseSourceBuildFailurePhase::
                        Build &&
                error.production_outcome().has_value() &&
                error.production_outcome()->build_outcome ==
                    ProductionSourceBuildCommandOutcome::
                        Succeeded &&
                error.production_outcome()->install_outcome ==
                    ProductionSourceInstallOutcome::
                        NotAttempted,
            "PackageBase status-zero post-build revalidation lost staged success");
        try {
            error.rethrow_failure();
            throw std::runtime_error(
                "PackageBase post-build revalidation lost its typed cause");
        } catch(const ProductionSourceBuildPostCommandRevalidationError&
                    post_command_error) {
            expect(
                post_command_error.command_exit_status() == 0,
                "PackageBase post-build revalidation changed makepkg status");
            bool nested_failure_retained = false;
            try {
                post_command_error.rethrow_failure();
            } catch(const std::exception&) {
                nested_failure_retained = true;
            }
            expect(
                nested_failure_retained,
                "PackageBase post-build revalidation lost identity failure");
        }
    }
    expect(
        failure_reported,
        "PackageBase status-zero post-build revalidation returned success");
    expect(
        scenario.identity_calls == 0 && scenario.install_calls == 0 &&
            metadata_stub::initialize_call_count() == 0,
        "PackageBase post-build revalidation crossed a later boundary");
    expect(
        fs::is_regular_file(
            scenario.displaced_workspace_path /
            scenario.produced_artifacts.front().leaf_name),
        "PackageBase post-build revalidation lost its built artifact");
    finish_scenario(scenario);
}

template <typename Callable>
void run_case(const std::string& name, Callable&& callable) {
    std::forward<Callable>(callable)();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        TemporaryTestEnvironment environment;
        run_case("ordinary size-one set", [&]() {
            test_ordinary_size_one(environment);
        });
        run_case("requested split child and unselected outputs", [&]() {
            test_split_child_and_unselected_outputs(environment);
        });
        run_case("multiple required order and mixed needed outcomes", [&]() {
            test_multiple_required_order_and_mixed_needed_outcomes(environment);
        });
        run_case("dependency install reason", [&]() {
            test_dependency_reason(environment);
        });
        run_case("typed preparation failures", [&]() {
            test_typed_preparation_failures(environment);
        });
        run_case("static request failures before workspace", [&]() {
            test_static_request_failures_before_workspace(environment);
        });
        run_case("rmdeps rejection before workspace", [&]() {
            test_rmdeps_rejected_before_workspace(environment);
        });
        run_case("identity and metadata failures", [&]() {
            test_identity_and_metadata_failures(environment);
        });
        run_case("transaction failures expose no result", [&]() {
            test_transaction_failures_have_no_public_result(environment);
        });
        run_case("cleanup failure retains full result", [&]() {
            test_cleanup_failure_retains_full_result(environment);
        });
        run_case("build and validation diagnostic retention", [&]() {
            test_build_and_validation_failures_retain_diagnostics(environment);
        });
        run_case("status-zero post-build revalidation", [&]() {
            test_status_zero_post_build_revalidation_retains_succeeded_outcome(
                environment);
        });
    } catch(const std::exception& error) {
        std::cerr << "separated PackageBase source-build test failed: "
                  << error.what() << '\n';
        return 1;
    }

    std::cout << "separated PackageBase source-build tests: all checks passed\n";
    return 0;
}
