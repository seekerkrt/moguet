#include "artifact_install_executor.hpp"

#include "artifact_workspace.hpp"
#include "stubs/artifact-install-executor/process_stub.hpp"
#include "stubs/package-metadata/alpm_stub.hpp"
#include "trusted_cache.hpp"
#include "trusted_cache_test_support.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

using ArtifactInstallPreparationFactory = PreparedArtifactInstall (*)(
    ValidatedPackageArtifactPath&,
    const std::string&,
    const std::string&,
    DesiredInstallReason,
    const ArtifactInstallPreparationOptions&,
    const PacmanDatabasePaths&);
using PreparedArtifactInstallExecutor = ArtifactInstallExecutionOutcome (*)(
    PreparedArtifactInstall&,
    const ArtifactInstallExecutionOptions&);

template <typename Options>
concept HasNeededExecutionOption = requires(Options options) {
    options.needed;
};

static_assert(
    std::is_same_v<
        decltype(&prepare_artifact_install),
        ArtifactInstallPreparationFactory>);
static_assert(
    std::is_same_v<
        decltype(&execute_prepared_artifact_install),
        PreparedArtifactInstallExecutor>);
static_assert(!std::is_default_constructible_v<PreparedArtifactInstall>);
static_assert(!std::is_copy_constructible_v<PreparedArtifactInstall>);
static_assert(!std::is_copy_assignable_v<PreparedArtifactInstall>);
static_assert(std::is_nothrow_move_constructible_v<PreparedArtifactInstall>);
static_assert(!std::is_move_assignable_v<PreparedArtifactInstall>);
static_assert(std::is_nothrow_destructible_v<PreparedArtifactInstall>);
static_assert(!std::is_aggregate_v<PreparedArtifactInstall>);
static_assert(
    !std::is_constructible_v<
        PreparedArtifactInstall, const std::filesystem::path&>);
static_assert(
    !std::is_constructible_v<
        PreparedArtifactInstall,
        std::string,
        DesiredInstallReason,
        bool,
        ArtifactPackageIdentity&&,
        ValidatedArtifactInstallTarget&&,
        InstalledVersionState,
        std::optional<ExistingInstallReason>,
        InstallReasonDirective,
        ValidatedPackageArtifactPath&&>);
static_assert(
    !std::is_invocable_v<
        PreparedArtifactInstallExecutor,
        std::filesystem::path&,
        const ArtifactInstallExecutionOptions&>);
static_assert(
    !std::is_invocable_v<
        PreparedArtifactInstallExecutor,
        ValidatedPackageArtifactPath&,
        const ArtifactInstallExecutionOptions&>);
static_assert(!HasNeededExecutionOption<ArtifactInstallExecutionOptions>);

namespace {

namespace fs = std::filesystem;
namespace metadata_stub = package_metadata_test_stub;
namespace process_stub = artifact_install_executor_test_stub;

constexpr const char* PACKAGE_NAME = "sample-package";
constexpr const char* ARTIFACT_VERSION = "2:1.4.0-3";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Callable>
std::string expect_runtime_error(Callable&& callable, const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const std::runtime_error& error) {
        return error.what();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " + error.what());
    }

    throw std::runtime_error(context + ": expected runtime_error");
}

template <typename Callable>
std::string expect_logic_error(Callable&& callable, const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const std::logic_error& error) {
        return error.what();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " + error.what());
    }

    throw std::runtime_error(context + ": expected logic_error");
}

template <typename Callable>
PackageMetadataFailure expect_package_metadata_error(
    Callable&& callable,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const PackageMetadataError& error) {
        expect(
            error.failure().code == expected_code,
            context + ": unexpected metadata error code");
        expect(
            !error.failure().diagnostic.empty(),
            context + ": empty metadata diagnostic");
        expect(
            std::string(error.what()) == error.failure().diagnostic,
            context + ": exception and failure diagnostics differ");
        return error.failure();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " + error.what());
    }

    throw std::runtime_error(context + ": expected PackageMetadataError");
}

class TemporaryCacheHome final {
    fs::path path_;
    std::optional<std::string> previous_xdg_cache_home_;

public:
    TemporaryCacheHome() {
        const std::string template_text =
            (fs::temp_directory_path() /
             "moguet-artifact-install-executor-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');

        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create artifact install executor test directory.");
        }
        path_ = created_path;

        const char* previous = std::getenv("XDG_CACHE_HOME");
        if(previous != nullptr) previous_xdg_cache_home_ = previous;
        if(setenv("XDG_CACHE_HOME", path_.c_str(), 1) != 0) {
            std::error_code error;
            fs::remove_all(path_, error);
            throw std::runtime_error(
                "Failed to set artifact install executor test cache home.");
        }
    }

    TemporaryCacheHome(const TemporaryCacheHome&) = delete;
    TemporaryCacheHome& operator=(const TemporaryCacheHome&) = delete;

    ~TemporaryCacheHome() {
        if(previous_xdg_cache_home_.has_value())
            static_cast<void>(setenv(
                "XDG_CACHE_HOME",
                previous_xdg_cache_home_->c_str(), 1));
        else
            static_cast<void>(unsetenv("XDG_CACHE_HOME"));

        std::error_code error;
        fs::remove_all(path_, error);
    }
};

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    if(!output) {
        throw std::runtime_error("Failed to create test file: " + path.string());
    }
    output << contents;
    output.close();
    if(!output) {
        throw std::runtime_error("Failed to finish test file: " + path.string());
    }
}

class ArtifactFixture final {
    std::unique_ptr<ValidatedPackageArtifactPath> artifact_;
    fs::path artifact_path_;
    fs::path workspace_path_;

public:
    explicit ArtifactFixture(const std::string& artifact_leaf_name) {
        ArtifactWorkspace workspace = create_artifact_workspace(
            prepare_private_trusted_cache_root(
                prepare_test_trusted_cache_root()));
        workspace_path_ = workspace.path();
        artifact_path_ = workspace.path() / artifact_leaf_name;

        ExpectedPackageArtifactPath expected =
            validate_makepkg_packagelist_output(
                workspace, artifact_path_.string() + "\n");
        write_file(artifact_path_, "fixture");

        ValidatedPackageArtifactPath artifact =
            validate_post_build_package_artifact(
                std::move(workspace), expected);
        artifact_ = std::make_unique<ValidatedPackageArtifactPath>(
            std::move(artifact));
    }

    ArtifactFixture(const ArtifactFixture&) = delete;
    ArtifactFixture& operator=(const ArtifactFixture&) = delete;

    ValidatedPackageArtifactPath& artifact() {
        return *artifact_;
    }

    const fs::path& artifact_path() const noexcept {
        return artifact_path_;
    }

    const fs::path& workspace_path() const noexcept {
        return workspace_path_;
    }
};

PacmanDatabasePaths test_database_paths() {
    return PacmanDatabasePaths{"/", "/var/lib/pacman"};
}

std::string expected_identity_command(const fs::path& artifact_path);

void reset_stubs() {
    process_stub::reset_process_stub();
    metadata_stub::reset_alpm_stub();
}

void expect_artifact_identity_capture(
    const ArtifactFixture& fixture, CapturedCommandResult result) {
    process_stub::expect_capture_command(
        expected_identity_command(fixture.artifact_path()),
        std::move(result));
}

void reset_stubs_with_identity(
    const ArtifactFixture& fixture,
    const std::string& package_name = PACKAGE_NAME,
    const std::string& full_version = ARTIFACT_VERSION) {
    reset_stubs();
    expect_artifact_identity_capture(
        fixture,
        CapturedCommandResult{
            package_name + " " + full_version + "\n", 0});
}

PreparedArtifactInstall prepare_fixture(
    ArtifactFixture& fixture,
    DesiredInstallReason desired_reason,
    bool needed = false,
    bool rm_deps = false,
    const std::string& requested_name = PACKAGE_NAME,
    const std::string& package_base = PACKAGE_NAME) {
    return prepare_artifact_install(
        fixture.artifact(), requested_name, package_base,
        desired_reason,
        ArtifactInstallPreparationOptions{needed, rm_deps},
        test_database_paths());
}

void expect_caller_still_owns_valid_artifact(
    ArtifactFixture& fixture,
    const std::string& context) {
    expect(
        fs::is_regular_file(fixture.artifact_path()),
        context + ": artifact path disappeared");
    expect(
        fs::is_directory(fixture.workspace_path()),
        context + ": workspace disappeared");
    fixture.artifact().require_validity();
}

std::string expected_shell_quote(const std::string& value) {
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

std::string expected_shell_join(const std::vector<std::string>& arguments) {
    std::string command;
    for(std::size_t index = 0; index < arguments.size(); ++index) {
        if(index != 0) command += " ";
        command += expected_shell_quote(arguments[index]);
    }
    return command;
}

std::string expected_identity_command(const fs::path& artifact_path) {
    return "LC_ALL=C " + expected_shell_join(
                             {"pacman", "-Qp", "--color", "never", "--",
                              artifact_path.string()});
}

std::string expected_install_command(
    const fs::path& artifact_path,
    bool no_confirm,
    bool needed,
    const char* reason_option = nullptr) {
    std::vector<std::string> arguments = {"sudo", "pacman", "-U"};
    if(no_confirm) arguments.emplace_back("--noconfirm");
    if(needed) arguments.emplace_back("--needed");
    if(reason_option != nullptr) arguments.emplace_back(reason_option);
    arguments.emplace_back("--");
    arguments.push_back(artifact_path.string());
    return expected_shell_join(arguments);
}

void test_process_stub_fail_fast_contract() {
    process_stub::reset_process_stub();
    const std::string explicit_capture = expect_logic_error(
        []() {
            static_cast<void>(capture_explicit_process_output_raw(
                ExplicitProcessInvocation{
                    "/usr/bin/pacman-conf", {"--repo-list"}, {"LC_ALL=C"}, 16384},
                true));
        },
        "unexpected explicit capture transport");
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "sticky explicit capture transport") == explicit_capture,
        "Explicit capture failure was not retained by the process stub");

    process_stub::reset_process_stub();
    process_stub::expect_capture_command(
        "expected capture", CapturedCommandResult{"unused", 0});
    const std::string capture_mismatch = expect_logic_error(
        []() {
            static_cast<void>(capture_command_output_raw("unexpected capture"));
        },
        "capture command mismatch");
    expect(
        capture_mismatch ==
            "Artifact install capture command did not match the next expectation.",
        "Capture mismatch diagnostic differs");
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "sticky capture command mismatch") == capture_mismatch,
        "Capture mismatch was not retained by the process stub");

    process_stub::reset_process_stub();
    process_stub::expect_run_command("run only", 0);
    const std::string run_at_capture_boundary = expect_logic_error(
        []() {
            static_cast<void>(capture_command_output_raw("run only"));
        },
        "run command sent to capture boundary");
    expect(
        run_at_capture_boundary ==
            "Unexpected artifact install capture command with no pending expectation.",
        "Capture boundary accepted a run command expectation");
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "sticky capture boundary kind mismatch") ==
            run_at_capture_boundary,
        "Capture boundary kind mismatch was not retained by the process stub");

    process_stub::reset_process_stub();
    process_stub::expect_capture_command(
        "capture only", CapturedCommandResult{"unused", 0});
    const std::string capture_at_run_boundary = expect_logic_error(
        []() { static_cast<void>(run_command("capture only")); },
        "capture command sent to run boundary");
    expect(
        capture_at_run_boundary ==
            "Unexpected artifact install run command with no pending expectation.",
        "Run boundary accepted a capture command expectation");
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "sticky run boundary kind mismatch") ==
            capture_at_run_boundary,
        "Run boundary kind mismatch was not retained by the process stub");

    process_stub::reset_process_stub();
    process_stub::expect_run_command("expected run", 0);
    const std::string run_mismatch = expect_logic_error(
        []() { static_cast<void>(run_command("unexpected run")); },
        "run command mismatch");
    expect(
        run_mismatch ==
            "Artifact install run command did not match the next expectation.",
        "Run mismatch diagnostic differs");
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "sticky run command mismatch") == run_mismatch,
        "Run mismatch was not retained by the process stub");

    process_stub::reset_process_stub();
    process_stub::expect_capture_command(
        "unconsumed capture", CapturedCommandResult{"unused", 0});
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "unconsumed capture command") ==
            "Artifact install process stub has unconsumed capture command expectations.",
        "Unconsumed capture expectation was not rejected");

    process_stub::reset_process_stub();
    process_stub::expect_capture_command(
        "first capture", CapturedCommandResult{"first result", 11});
    process_stub::expect_capture_command(
        "second capture", CapturedCommandResult{"second result", 22});
    process_stub::expect_run_command("first run", 33);
    process_stub::expect_run_command("second run", 44);

    const CapturedCommandResult first_capture =
        capture_command_output_raw("first capture");
    const CapturedCommandResult second_capture =
        capture_command_output_raw("second capture");
    expect(
        first_capture.output == "first result" &&
            first_capture.exit_code == 11 &&
            second_capture.output == "second result" &&
            second_capture.exit_code == 22,
        "Capture expectations did not preserve FIFO results");
    expect(
        run_command("first run") == 33 &&
            run_command("second run") == 44,
        "Run expectations did not preserve FIFO statuses");
    process_stub::require_process_expectations_consumed();

    process_stub::reset_process_stub();
    expect(
        process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0 &&
            process_stub::last_captured_command().empty() &&
            process_stub::last_run_command().empty(),
        "Process stub reset did not clear counters or recorded commands");
    process_stub::require_process_expectations_consumed();
}

std::size_t count_occurrences(
    const std::string& text,
    const std::string& needle) {
    std::size_t count = 0;
    std::size_t position = 0;
    while((position = text.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

struct MetadataCallCounts {
    std::size_t initialize;
    std::size_t local_database;
    std::size_t database_valid;
    std::size_t package_cache;
    std::size_t package_query;
    std::size_t release;

    bool operator==(const MetadataCallCounts&) const = default;
};

MetadataCallCounts metadata_call_counts() {
    return MetadataCallCounts{
        metadata_stub::initialize_call_count(),
        metadata_stub::local_database_call_count(),
        metadata_stub::database_valid_call_count(),
        metadata_stub::package_cache_call_count(),
        metadata_stub::package_query_call_count(),
        metadata_stub::release_call_count()};
}

void require_all_metadata_sessions_released_before_run() {
    expect(
        metadata_stub::created_handle_count() > 0,
        "Executor command started without a metadata session");
    expect(
        metadata_stub::release_call_count() ==
            metadata_stub::created_handle_count(),
        "Executor command started before every metadata handle was released");
    for(std::size_t index = 0;
        index < metadata_stub::created_handle_count(); ++index) {
        expect(
            metadata_stub::release_count_for_handle(index) == 1,
            "Metadata handle was not released exactly once before executor command");
    }
}

void expect_policy_state(
    const InstalledArtifactPolicyState& state,
    InstalledVersionState expected_version_state,
    std::optional<ExistingInstallReason> expected_reason,
    const std::string& context) {
    expect(
        state.version_state == expected_version_state,
        context + ": installed version state differs");
    expect(
        state.existing_reason == expected_reason,
        context + ": existing install reason differs");
}

void test_pure_installed_metadata_mapping() {
    const ArtifactPackageIdentity identity{PACKAGE_NAME, ARTIFACT_VERSION};

    expect_policy_state(
        map_installed_artifact_policy_state(
            identity, PackageNotFound{}),
        InstalledVersionState::NotInstalled, std::nullopt,
        "package not found");
    expect_policy_state(
        map_installed_artifact_policy_state(
            identity,
            InstalledPackageMetadata{
                PACKAGE_NAME, ARTIFACT_VERSION,
                InstalledPackageReason::Explicit}),
        InstalledVersionState::SameVersion,
        ExistingInstallReason::Explicit,
        "same version explicit");
    expect_policy_state(
        map_installed_artifact_policy_state(
            identity,
            InstalledPackageMetadata{
                PACKAGE_NAME, ARTIFACT_VERSION,
                InstalledPackageReason::Dependency}),
        InstalledVersionState::SameVersion,
        ExistingInstallReason::Dependency,
        "same version dependency");
    expect_policy_state(
        map_installed_artifact_policy_state(
            identity,
            InstalledPackageMetadata{
                PACKAGE_NAME, "2:1.4.0-2",
                InstalledPackageReason::Explicit}),
        InstalledVersionState::DifferentVersion,
        ExistingInstallReason::Explicit,
        "different version explicit");
    expect_policy_state(
        map_installed_artifact_policy_state(
            identity,
            InstalledPackageMetadata{
                PACKAGE_NAME, "1.4.0-3",
                InstalledPackageReason::Dependency}),
        InstalledVersionState::DifferentVersion,
        ExistingInstallReason::Dependency,
        "different version dependency");

    static_cast<void>(expect_package_metadata_error(
        [&]() {
            static_cast<void>(map_installed_artifact_policy_state(
                identity,
                InstalledPackageMetadata{
                    "different-package", ARTIFACT_VERSION,
                    InstalledPackageReason::Explicit}));
        },
        PackageMetadataErrorCode::MalformedMetadata,
        "returned metadata name mismatch"));
    static_cast<void>(expect_package_metadata_error(
        [&]() {
            static_cast<void>(map_installed_artifact_policy_state(
                identity,
                InstalledPackageMetadata{
                    PACKAGE_NAME, "",
                    InstalledPackageReason::Explicit}));
        },
        PackageMetadataErrorCode::MalformedMetadata,
        "empty installed version"));
    static_cast<void>(expect_package_metadata_error(
        [&]() {
            static_cast<void>(map_installed_artifact_policy_state(
                identity,
                InstalledPackageMetadata{
                    PACKAGE_NAME, ARTIFACT_VERSION,
                    InstalledPackageReason::Unknown}));
        },
        PackageMetadataErrorCode::MalformedMetadata,
        "unknown installed reason"));
    static_cast<void>(expect_package_metadata_error(
        [&]() {
            static_cast<void>(map_installed_artifact_policy_state(
                identity,
                InstalledPackageMetadata{
                    PACKAGE_NAME, ARTIFACT_VERSION,
                    static_cast<InstalledPackageReason>(99)}));
        },
        PackageMetadataErrorCode::MalformedMetadata,
        "future installed reason"));

    const PackageMetadataFailure query_failure{
        PackageMetadataErrorCode::QueryFailed,
        "fixture metadata query diagnostic"};
    PackageMetadataFailure propagated = expect_package_metadata_error(
        [&]() {
            static_cast<void>(map_installed_artifact_policy_state(
                identity, query_failure));
        },
        PackageMetadataErrorCode::QueryFailed,
        "query failure propagation");
    expect(
        propagated.diagnostic == query_failure.diagnostic,
        "Query failure diagnostic was not preserved");
}

void test_prepared_aggregate_correlation_and_ownership() {
    ArtifactFixture fixture("aggregate-correlation.pkg.tar.zst");
    const fs::path artifact_path = fixture.artifact_path();
    const fs::path workspace_path = fixture.workspace_path();

    reset_stubs_with_identity(fixture);
    metadata_stub::set_package_metadata(
        PACKAGE_NAME, ARTIFACT_VERSION, ALPM_PKG_REASON_EXPLICIT);

    PreparedArtifactInstall install = prepare_fixture(
        fixture, DesiredInstallReason::Explicit);

    expect(install.artifact_path() == artifact_path, "Aggregate artifact path differs");
    expect(install.workspace_path() == workspace_path, "Aggregate workspace path differs");
    expect(
        install.identity().package_name == PACKAGE_NAME,
        "Aggregate identity package name differs");
    expect(
        install.identity().full_version == ARTIFACT_VERSION,
        "Aggregate identity full version differs");
    expect(
        install.target().package_name == PACKAGE_NAME,
        "Aggregate pure target package name differs");
    expect(
        install.requested_name() == PACKAGE_NAME,
        "Aggregate requested package name differs");
    expect(
        install.desired_reason() == DesiredInstallReason::Explicit,
        "Aggregate desired reason differs");
    expect(!install.needed(), "Aggregate unexpectedly enabled --needed");
    expect(
        install.installed_version_state() == InstalledVersionState::SameVersion,
        "Aggregate installed version state differs");
    expect(
        install.existing_reason() == ExistingInstallReason::Explicit,
        "Aggregate existing reason differs");
    expect(
        install.directive() == InstallReasonDirective::Default,
        "Aggregate reason directive differs");

    expect(
        process_stub::capture_command_call_count() == 1,
        "Preparation did not perform exactly one identity query");
    expect(
        process_stub::last_captured_command() ==
            expected_identity_command(artifact_path),
        "Preparation identity command differs");
    expect(
        metadata_stub::last_queried_package_name() == PACKAGE_NAME,
        "Preparation queried metadata for a different package");
    expect(
        metadata_stub::initialize_call_count() == 1 &&
            metadata_stub::release_call_count() == 1,
        "Preparation did not close its metadata session before returning");
    expect(
        process_stub::run_command_call_count() == 0,
        "Preparation unexpectedly executed an install command");

    static_cast<void>(expect_runtime_error(
        [&]() { fixture.artifact().require_validity(); },
        "successful preparation source ownership"));
    install.cleanup_workspace();
    expect(
        !fs::exists(workspace_path),
        "Aggregate cleanup did not remove its workspace");
    process_stub::require_process_expectations_consumed();
}

void test_preparation_failures_preserve_artifact() {
    {
        ArtifactFixture fixture("identity-command-failure.pkg.tar.zst");
        fixture.artifact().retain_workspace_for_diagnostics();
        reset_stubs();
        expect_artifact_identity_capture(
            fixture,
            CapturedCommandResult{"ignored\n", 37});

        static_cast<void>(expect_runtime_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit));
            },
            "artifact identity command failure"));
        expect_caller_still_owns_valid_artifact(
            fixture, "artifact identity command failure");
        expect(
            metadata_stub::initialize_call_count() == 0,
            "Identity command failure reached metadata session open");
        expect(
            process_stub::run_command_call_count() == 0,
            "Identity command failure reached install executor");
        fixture.artifact().cleanup_workspace();
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("malformed-identity.pkg.tar.zst");
        reset_stubs();
        expect_artifact_identity_capture(
            fixture,
            CapturedCommandResult{"sample-package  1-1\n", 0});

        static_cast<void>(expect_runtime_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit));
            },
            "malformed artifact identity"));
        expect_caller_still_owns_valid_artifact(
            fixture, "malformed artifact identity");
        expect(
            metadata_stub::initialize_call_count() == 0,
            "Malformed identity reached metadata session open");
        expect(
            process_stub::run_command_call_count() == 0,
            "Malformed identity reached install executor");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("identity-name-mismatch.pkg.tar.zst");
        reset_stubs_with_identity(
            fixture, "different-package", ARTIFACT_VERSION);

        static_cast<void>(expect_runtime_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit));
            },
            "identity package name mismatch"));
        expect_caller_still_owns_valid_artifact(
            fixture, "identity package name mismatch");
        expect(
            metadata_stub::initialize_call_count() == 0,
            "Identity package mismatch reached metadata session open");
        expect(
            process_stub::run_command_call_count() == 0,
            "Identity package mismatch reached install executor");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("package-base-mismatch.pkg.tar.zst");
        reset_stubs_with_identity(fixture);

        static_cast<void>(expect_runtime_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit,
                    false, false, PACKAGE_NAME,
                    "different-package-base"));
            },
            "PackageBase mismatch"));
        expect_caller_still_owns_valid_artifact(
            fixture, "PackageBase mismatch");
        expect(
            metadata_stub::initialize_call_count() == 0,
            "PackageBase mismatch reached metadata session open");
        expect(
            process_stub::run_command_call_count() == 0,
            "PackageBase mismatch reached install executor");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("metadata-open-failure.pkg.tar.zst");
        reset_stubs_with_identity(fixture);
        metadata_stub::set_initialize_failure(ALPM_ERR_SYSTEM);

        static_cast<void>(expect_package_metadata_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit));
            },
            PackageMetadataErrorCode::InitializationFailed,
            "metadata session open failure"));
        expect_caller_still_owns_valid_artifact(
            fixture, "metadata session open failure");
        expect(
            metadata_stub::initialize_call_count() == 1 &&
                metadata_stub::release_call_count() == 0,
            "Failed metadata initialization published or released a handle");
        expect(
            process_stub::run_command_call_count() == 0,
            "Metadata session open failure reached install executor");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("metadata-query-failure.pkg.tar.zst");
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_query_failure(ALPM_ERR_DB_OPEN);

        PackageMetadataFailure failure = expect_package_metadata_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit));
            },
            PackageMetadataErrorCode::QueryFailed,
            "metadata query failure");
        expect(
            failure.diagnostic.find("Installed package query failed") !=
                std::string::npos,
            "Metadata query diagnostic was lost");
        expect_caller_still_owns_valid_artifact(
            fixture, "metadata query failure");
        expect(
            metadata_stub::release_count_for_handle(0) == 1,
            "Metadata query failure leaked its session");
        expect(
            process_stub::run_command_call_count() == 0,
            "Metadata query failure reached install executor");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("unknown-installed-reason.pkg.tar.zst");
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_metadata(
            PACKAGE_NAME, ARTIFACT_VERSION, ALPM_PKG_REASON_UNKNOWN);

        static_cast<void>(expect_package_metadata_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit));
            },
            PackageMetadataErrorCode::MalformedMetadata,
            "unknown installed reason"));
        expect_caller_still_owns_valid_artifact(
            fixture, "unknown installed reason");
        expect(
            metadata_stub::release_count_for_handle(0) == 1,
            "Unknown installed reason leaked its session");
        expect(
            process_stub::run_command_call_count() == 0,
            "Unknown installed reason reached install executor");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("needed-reducer-failure.pkg.tar.zst");
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_metadata(
            PACKAGE_NAME, ARTIFACT_VERSION, ALPM_PKG_REASON_DEPEND);

        const std::string message = expect_runtime_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit, true));
            },
            "same-version needed reason change");
        expect(
            message ==
                "Cannot change the install reason because --needed may skip "
                "the same-version install.",
            "Same-version --needed reducer diagnostic differs");
        expect_caller_still_owns_valid_artifact(
            fixture, "same-version needed reason change");
        expect(
            metadata_stub::release_count_for_handle(0) == 1,
            "Reducer failure occurred before metadata session release");
        expect(
            process_stub::run_command_call_count() == 0,
            "Reducer failure reached install executor");
        process_stub::require_process_expectations_consumed();
    }
}

struct PreparationPolicyCase {
    const char* name;
    DesiredInstallReason desired_reason;
    bool installed;
    const char* installed_version;
    alpm_pkgreason_t installed_reason;
    bool needed;
    InstalledVersionState expected_version_state;
    std::optional<ExistingInstallReason> expected_existing_reason;
    InstallReasonDirective expected_directive;
};

void test_install_reason_reducer_integration() {
    const std::vector<PreparationPolicyCase> cases = {
        {"new root", DesiredInstallReason::Explicit, false, "",
         ALPM_PKG_REASON_EXPLICIT, false,
         InstalledVersionState::NotInstalled, std::nullopt,
         InstallReasonDirective::Default},
        {"existing explicit root", DesiredInstallReason::Explicit, true,
         "2:1.4.0-2", ALPM_PKG_REASON_EXPLICIT, false,
         InstalledVersionState::DifferentVersion,
         ExistingInstallReason::Explicit,
         InstallReasonDirective::Default},
        {"existing dependency root", DesiredInstallReason::Explicit, true,
         "2:1.4.0-2", ALPM_PKG_REASON_DEPEND, false,
         InstalledVersionState::DifferentVersion,
         ExistingInstallReason::Dependency,
         InstallReasonDirective::AsExplicit},
        {"new dependency", DesiredInstallReason::Dependency, false, "",
         ALPM_PKG_REASON_EXPLICIT, false,
         InstalledVersionState::NotInstalled, std::nullopt,
         InstallReasonDirective::AsDependency},
        {"existing dependency", DesiredInstallReason::Dependency, true,
         "2:1.4.0-2", ALPM_PKG_REASON_DEPEND, false,
         InstalledVersionState::DifferentVersion,
         ExistingInstallReason::Dependency,
         InstallReasonDirective::Default},
        {"existing explicit dependency", DesiredInstallReason::Dependency,
         true, "2:1.4.0-2", ALPM_PKG_REASON_EXPLICIT, false,
         InstalledVersionState::DifferentVersion,
         ExistingInstallReason::Explicit,
         InstallReasonDirective::Default},
        {"same version needed default", DesiredInstallReason::Explicit, true,
         ARTIFACT_VERSION, ALPM_PKG_REASON_EXPLICIT, true,
         InstalledVersionState::SameVersion,
         ExistingInstallReason::Explicit,
         InstallReasonDirective::Default},
        {"different version needed reason change",
         DesiredInstallReason::Explicit, true, "2:1.4.0-2",
         ALPM_PKG_REASON_DEPEND, true,
         InstalledVersionState::DifferentVersion,
         ExistingInstallReason::Dependency,
         InstallReasonDirective::AsExplicit},
    };

    for(std::size_t index = 0; index < cases.size(); ++index) {
        const PreparationPolicyCase& test_case = cases[index];
        ArtifactFixture fixture(
            "reason-policy-" + std::to_string(index) + ".pkg.tar.zst");
        reset_stubs_with_identity(fixture);
        if(test_case.installed) {
            metadata_stub::set_package_metadata(
                PACKAGE_NAME, test_case.installed_version,
                test_case.installed_reason);
        } else {
            metadata_stub::set_package_absent();
        }

        PreparedArtifactInstall install = prepare_fixture(
            fixture, test_case.desired_reason, test_case.needed);
        expect(
            install.installed_version_state() ==
                test_case.expected_version_state,
            std::string(test_case.name) + ": version state differs");
        expect(
            install.existing_reason() ==
                test_case.expected_existing_reason,
            std::string(test_case.name) + ": existing reason differs");
        expect(
            install.directive() == test_case.expected_directive,
            std::string(test_case.name) + ": directive differs");
        expect(
            install.needed() == test_case.needed,
            std::string(test_case.name) + ": needed state differs");
        expect(
            metadata_stub::release_count_for_handle(0) == 1,
            std::string(test_case.name) +
                ": preparation retained metadata session");
        expect(
            process_stub::run_command_call_count() == 0,
            std::string(test_case.name) +
                ": preparation executed pacman -U");
        process_stub::require_process_expectations_consumed();
    }
}

void test_fresh_session_per_preparation() {
    ArtifactFixture first_fixture("fresh-session-first.pkg.tar.zst");
    ArtifactFixture second_fixture("fresh-session-second.pkg.tar.zst");
    reset_stubs();
    expect_artifact_identity_capture(
        first_fixture,
        CapturedCommandResult{
            std::string(PACKAGE_NAME) + " " + ARTIFACT_VERSION + "\n", 0});
    expect_artifact_identity_capture(
        second_fixture,
        CapturedCommandResult{
            std::string(PACKAGE_NAME) + " " + ARTIFACT_VERSION + "\n", 0});
    metadata_stub::set_package_metadata(
        PACKAGE_NAME, ARTIFACT_VERSION, ALPM_PKG_REASON_EXPLICIT);

    PreparedArtifactInstall first = prepare_fixture(
        first_fixture, DesiredInstallReason::Explicit);
    PreparedArtifactInstall second = prepare_fixture(
        second_fixture, DesiredInstallReason::Explicit);

    expect(
        metadata_stub::initialize_call_count() == 2,
        "Two preparations did not open two fresh metadata sessions");
    expect(
        metadata_stub::created_handle_count() == 2,
        "Two preparations did not create two metadata handles");
    expect(
        metadata_stub::release_count_for_handle(0) == 1 &&
            metadata_stub::release_count_for_handle(1) == 1,
        "Fresh metadata sessions were not released independently");
    expect(
        process_stub::capture_command_call_count() == 2,
        "Two preparations did not query both artifact identities");
    expect(
        process_stub::run_command_call_count() == 0,
        "Fresh-session preparation unexpectedly ran an install command");
    process_stub::require_process_expectations_consumed();
}

void test_rmdeps_guard() {
    {
        ArtifactFixture fixture("rmdeps-false.pkg.tar.zst");
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_absent();

        PreparedArtifactInstall install = prepare_fixture(
            fixture, DesiredInstallReason::Explicit, false, false);
        expect(
            install.directive() == InstallReasonDirective::Default,
            "rmdeps=false changed preparation policy");
        expect(
            process_stub::capture_command_call_count() == 1 &&
                metadata_stub::initialize_call_count() == 1,
            "rmdeps=false did not reach normal preparation");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("rmdeps-true.pkg.tar.zst");
        reset_stubs();
        metadata_stub::set_package_absent();

        const std::string message = expect_runtime_error(
            [&]() {
                static_cast<void>(prepare_fixture(
                    fixture, DesiredInstallReason::Explicit,
                    false, true));
            },
            "rmdeps=true guard");
        expect(
            message ==
                "Separated build/install does not support --rmdeps.",
            "rmdeps rejection diagnostic differs");
        expect_caller_still_owns_valid_artifact(fixture, "rmdeps=true guard");
        expect(
            process_stub::capture_command_call_count() == 0,
            "rmdeps=true reached artifact identity query");
        expect(
            metadata_stub::initialize_call_count() == 0,
            "rmdeps=true reached metadata session open");
        expect(
            process_stub::run_command_call_count() == 0,
            "rmdeps=true reached sudo command");
        process_stub::require_process_expectations_consumed();
    }
}

struct ExecutionCase {
    const char* name;
    DesiredInstallReason desired_reason;
    bool installed;
    const char* installed_version;
    alpm_pkgreason_t installed_reason;
    bool needed;
    bool no_confirm;
    const char* expected_reason_option;
    ArtifactInstallExecutionOutcome expected_outcome;
    const char* artifact_leaf_name;
};

void test_exact_pacman_argv_and_session_boundary() {
    const std::vector<ExecutionCase> cases = {
        {"default", DesiredInstallReason::Explicit, false, "",
         ALPM_PKG_REASON_EXPLICIT, false, false, nullptr,
         ArtifactInstallExecutionOutcome::Installed,
         "default.pkg.tar.zst"},
        {"same installed version without needed",
         DesiredInstallReason::Explicit, true, ARTIFACT_VERSION,
         ALPM_PKG_REASON_EXPLICIT, false, false, nullptr,
         ArtifactInstallExecutionOutcome::Installed,
         "same-without-needed.pkg.tar.zst"},
        {"asexplicit and space", DesiredInstallReason::Explicit, true,
         "2:1.4.0-2", ALPM_PKG_REASON_DEPEND, false, false,
         "--asexplicit", ArtifactInstallExecutionOutcome::Installed,
         "artifact with space.pkg.tar.zst"},
        {"asdeps and apostrophe", DesiredInstallReason::Dependency, false,
         "", ALPM_PKG_REASON_EXPLICIT, false, false, "--asdeps",
         ArtifactInstallExecutionOutcome::Installed,
         "artifact'quote.pkg.tar.zst"},
        {"noconfirm and leading dash", DesiredInstallReason::Explicit,
         false, "", ALPM_PKG_REASON_EXPLICIT, false, true, nullptr,
         ArtifactInstallExecutionOutcome::Installed,
         "-leading-dash.pkg.tar.zst"},
        {"needed with different installed version",
         DesiredInstallReason::Explicit, true, "2:1.4.0-2",
         ALPM_PKG_REASON_EXPLICIT, true, false, nullptr,
         ArtifactInstallExecutionOutcome::Installed,
         "needed-different.pkg.tar.zst"},
        {"needed with same installed version",
         DesiredInstallReason::Explicit, true, ARTIFACT_VERSION,
         ALPM_PKG_REASON_EXPLICIT, true, false, nullptr,
         ArtifactInstallExecutionOutcome::SkippedAsNeeded,
         "needed-same.pkg.tar.zst"},
        {"noconfirm and needed", DesiredInstallReason::Explicit, false,
         "", ALPM_PKG_REASON_EXPLICIT, true, true, nullptr,
         ArtifactInstallExecutionOutcome::Installed,
         "both-options.pkg.tar.zst"},
    };

    for(const ExecutionCase& test_case : cases) {
        ArtifactFixture fixture(test_case.artifact_leaf_name);
        const fs::path artifact_path = fixture.artifact_path();
        reset_stubs_with_identity(fixture);
        if(test_case.installed) {
            metadata_stub::set_package_metadata(
                PACKAGE_NAME, test_case.installed_version,
                test_case.installed_reason);
        } else {
            metadata_stub::set_package_absent();
        }

        PreparedArtifactInstall install = prepare_fixture(
            fixture, test_case.desired_reason, test_case.needed);
        const MetadataCallCounts before_execute = metadata_call_counts();
        const std::string expected_command = expected_install_command(
            artifact_path, test_case.no_confirm, test_case.needed,
            test_case.expected_reason_option);
        process_stub::expect_run_command(expected_command, 0);
        process_stub::set_run_hook(require_all_metadata_sessions_released_before_run);

        const ArtifactInstallExecutionOutcome outcome =
            execute_prepared_artifact_install(
                install,
                ArtifactInstallExecutionOptions{
                    test_case.no_confirm});

        const std::string command = process_stub::last_run_command();
        expect(
            command == expected_command,
            std::string(test_case.name) + ": pacman argv differs");
        expect(
            process_stub::run_command_call_count() == 1,
            std::string(test_case.name) + ": sudo command count differs");
        expect(
            command.find("'--' " + expected_shell_quote(artifact_path.string())) !=
                std::string::npos,
            std::string(test_case.name) +
                ": semantic -- is not immediately before artifact path");
        expect(
            count_occurrences(command, "'--asexplicit'") +
                    count_occurrences(command, "'--asdeps'") <=
                1,
            std::string(test_case.name) + ": multiple reason options emitted");
        expect(
            command.find("'-D'") == std::string::npos,
            std::string(test_case.name) + ": pacman -D fallback emitted");
        expect(
            metadata_call_counts() == before_execute,
            std::string(test_case.name) +
                ": final executor called libalpm metadata functions");
        expect(
            outcome == test_case.expected_outcome,
            std::string(test_case.name) +
                ": typed install outcome differs");
        expect(
            fs::is_regular_file(artifact_path),
            std::string(test_case.name) +
                ": successful executor auto-cleaned artifact");

        install.cleanup_workspace();
        process_stub::require_process_expectations_consumed();
    }
}

void test_executor_failure_and_success_lifecycle() {
    {
        ArtifactFixture fixture("executor-failure.pkg.tar.zst");
        const fs::path artifact_path = fixture.artifact_path();
        const fs::path workspace_path = fixture.workspace_path();
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_absent();

        PreparedArtifactInstall install = prepare_fixture(
            fixture, DesiredInstallReason::Explicit);
        install.retain_workspace_for_diagnostics();
        process_stub::expect_run_command(
            expected_install_command(
                artifact_path, false, false),
            73);
        process_stub::set_run_hook(require_all_metadata_sessions_released_before_run);
        const MetadataCallCounts before_execute = metadata_call_counts();

        const std::string message = expect_runtime_error(
            [&]() {
                execute_prepared_artifact_install(
                    install, ArtifactInstallExecutionOptions{});
            },
            "nonzero pacman -U status");
        expect(
            message == "pacman -U failed with exit code 73.",
            "Nonzero pacman diagnostic differs");
        expect(
            message.find(artifact_path.string()) == std::string::npos,
            "Nonzero pacman diagnostic contains package-controlled path");
        expect(
            process_stub::run_command_call_count() == 1,
            "Nonzero pacman status changed command count");
        expect(
            metadata_call_counts() == before_execute,
            "Failed executor called libalpm metadata functions");
        expect(
            fs::is_regular_file(artifact_path) &&
                fs::is_directory(workspace_path),
            "Failed executor cleaned retained artifact workspace");
        expect(
            install.identity().package_name == PACKAGE_NAME,
            "Failed executor consumed prepared aggregate state");

        install.cleanup_workspace();
        expect(
            !fs::exists(workspace_path),
            "Explicit cleanup after executor failure did not remove workspace");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("executor-success.pkg.tar.zst");
        const fs::path artifact_path = fixture.artifact_path();
        const fs::path workspace_path = fixture.workspace_path();
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_absent();

        PreparedArtifactInstall install = prepare_fixture(
            fixture, DesiredInstallReason::Explicit);
        process_stub::expect_run_command(
            expected_install_command(
                artifact_path, false, false),
            0);
        process_stub::set_run_hook(require_all_metadata_sessions_released_before_run);
        execute_prepared_artifact_install(
            install, ArtifactInstallExecutionOptions{});

        expect(
            fs::is_regular_file(artifact_path) &&
                fs::is_directory(workspace_path),
            "Successful executor auto-cleaned artifact workspace");
        install.cleanup_workspace();
        expect(
            !fs::exists(artifact_path) && !fs::exists(workspace_path),
            "Explicit cleanup after executor success did not remove workspace");
        process_stub::require_process_expectations_consumed();
    }
}

void test_executor_revalidates_artifact_and_workspace() {
    {
        ArtifactFixture fixture("artifact-replaced.pkg.tar.zst");
        const fs::path artifact_path = fixture.artifact_path();
        const fs::path workspace_path = fixture.workspace_path();
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_absent();
        PreparedArtifactInstall install = prepare_fixture(
            fixture, DesiredInstallReason::Explicit);

        fs::path original_path = artifact_path;
        original_path += ".original";
        fs::rename(artifact_path, original_path);
        write_file(artifact_path, "replacement");

        static_cast<void>(expect_runtime_error(
            [&]() {
                execute_prepared_artifact_install(
                    install, ArtifactInstallExecutionOptions{});
            },
            "artifact replacement before executor"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Artifact replacement reached sudo command");
        install.cleanup_workspace();
        expect(
            !fs::exists(workspace_path),
            "Artifact replacement fixture cleanup failed");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("workspace-replaced.pkg.tar.zst");
        const fs::path workspace_path = fixture.workspace_path();
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_absent();

        fs::path displaced_workspace = workspace_path;
        displaced_workspace += ".original";
        {
            PreparedArtifactInstall install = prepare_fixture(
                fixture, DesiredInstallReason::Explicit);
            install.retain_workspace_for_diagnostics();
            fs::rename(workspace_path, displaced_workspace);
            fs::create_directory(workspace_path);
            fs::permissions(
                workspace_path, fs::perms::owner_all,
                fs::perm_options::replace);

            static_cast<void>(expect_runtime_error(
                [&]() {
                    execute_prepared_artifact_install(
                        install, ArtifactInstallExecutionOptions{});
                },
                "workspace identity replacement before executor"));
            expect(
                process_stub::run_command_call_count() == 0,
                "Workspace identity replacement reached sudo command");
        }

        std::error_code cleanup_error;
        fs::remove_all(workspace_path, cleanup_error);
        expect(!cleanup_error, "Failed to remove replacement workspace fixture");
        fs::remove_all(displaced_workspace, cleanup_error);
        expect(!cleanup_error, "Failed to remove displaced workspace fixture");
        process_stub::require_process_expectations_consumed();
    }

    {
        ArtifactFixture fixture("unexpected-entry.pkg.tar.zst");
        const fs::path workspace_path = fixture.workspace_path();
        reset_stubs_with_identity(fixture);
        metadata_stub::set_package_absent();
        PreparedArtifactInstall install = prepare_fixture(
            fixture, DesiredInstallReason::Explicit);

        write_file(workspace_path / "unexpected-entry", "unexpected");
        static_cast<void>(expect_runtime_error(
            [&]() {
                execute_prepared_artifact_install(
                    install, ArtifactInstallExecutionOptions{});
            },
            "unexpected workspace entry before executor"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Unexpected workspace entry reached sudo command");
        install.cleanup_workspace();
        expect(
            !fs::exists(workspace_path),
            "Unexpected-entry fixture cleanup failed");
        process_stub::require_process_expectations_consumed();
    }
}

template <typename Callable>
void run_case(const std::string& name, Callable&& callable) {
    std::forward<Callable>(callable)();
    process_stub::require_process_expectations_consumed();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        TemporaryCacheHome cache_home;
        static_cast<void>(cache_home);

        run_case(
            "process stub fail-fast expectations",
            test_process_stub_fail_fast_contract);
        run_case("pure installed metadata mapping", test_pure_installed_metadata_mapping);
        run_case(
            "prepared aggregate correlation and ownership",
            test_prepared_aggregate_correlation_and_ownership);
        run_case(
            "preparation failures preserve artifact",
            test_preparation_failures_preserve_artifact);
        run_case(
            "install reason reducer integration",
            test_install_reason_reducer_integration);
        run_case(
            "fresh session per preparation",
            test_fresh_session_per_preparation);
        run_case("rmdeps guard", test_rmdeps_guard);
        run_case(
            "exact pacman argv and session boundary",
            test_exact_pacman_argv_and_session_boundary);
        run_case(
            "executor failure and success lifecycle",
            test_executor_failure_and_success_lifecycle);
        run_case(
            "executor artifact and workspace revalidation",
            test_executor_revalidates_artifact_and_workspace);
    } catch(const std::exception& error) {
        std::cerr << "artifact install executor test failed: "
                  << error.what() << '\n';
        return 1;
    }

    std::cout << "artifact install executor tests: all checks passed\n";
    return 0;
}
