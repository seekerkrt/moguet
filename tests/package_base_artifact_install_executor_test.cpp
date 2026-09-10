#include "package_base_artifact_install_executor.hpp"

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
#include <sys/stat.h>
#include <fcntl.h>

using PackageBaseArtifactInstallPreparationFactory =
    PackageBaseArtifactInstallPreparationResult (*)(
        ValidatedPackageArtifactSet&,
        const std::string&,
        const std::vector<RequiredPackageArtifactTarget>&,
        const ArtifactInstallPreparationOptions&,
        const PacmanDatabasePaths&);
using PreparedPackageBaseArtifactInstallExecutor =
    PackageBaseArtifactInstallExecutionResult (*)(
        PreparedPackageBaseArtifactInstall&,
        const ArtifactInstallExecutionOptions&);

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
concept HasArtifactPathMethod = requires(const Value& value) {
    value.artifact_path();
};

template <typename Value>
concept HasArtifactPathsMethod = requires(const Value& value) {
    value.artifact_paths();
};

template <typename Value>
concept HasArtifactIndex = requires(const Value& value) {
    value.artifact_index;
};

template <typename Value>
concept HasDirectiveDataMember = requires(const Value& value) {
    value.directive;
};

template <typename Value>
concept HasOutcomeDataMember = requires(const Value& value) {
    value.outcome;
};

template <typename Value>
concept HasSelectedArtifacts = requires(const Value& value) {
    value.selected_artifacts;
};

template <typename Value>
concept CanCleanupWorkspace = requires(Value& value) {
    value.cleanup_workspace();
};

static_assert(
    std::is_same_v<
        decltype(&prepare_package_base_artifact_install),
        PackageBaseArtifactInstallPreparationFactory>);
static_assert(
    std::is_same_v<
        decltype(&execute_prepared_package_base_artifact_install),
        PreparedPackageBaseArtifactInstallExecutor>);

static_assert(
    !std::is_default_constructible_v<
        PackageBaseArtifactInstallPreparationResult>);
static_assert(
    !std::is_copy_constructible_v<
        PackageBaseArtifactInstallPreparationResult>);
static_assert(
    std::is_nothrow_move_constructible_v<
        PackageBaseArtifactInstallPreparationResult>);
static_assert(
    !std::is_constructible_v<
        PackageBaseArtifactInstallPreparationResult,
        PreparedPackageBaseArtifactInstall&&>);
static_assert(
    !std::is_constructible_v<
        PackageBaseArtifactInstallPreparationResult,
        PackageBaseArtifactInstallPreparationFailure&&>);

static_assert(
    !std::is_default_constructible_v<
        PackageBaseArtifactInstallPreparationFailure>);
static_assert(
    !std::is_constructible_v<
        PackageBaseArtifactInstallPreparationFailure,
        PackageBaseArtifactIdentitySelectionFailure>);
static_assert(
    !std::is_constructible_v<
        PackageBaseArtifactInstallPreparationFailure,
        MixedPackageBaseInstallReasonUnsupported>);

static_assert(
    !std::is_default_constructible_v<
        PreparedPackageBaseArtifactInstall>);
static_assert(
    !std::is_default_constructible_v<ValidatedPackageArtifactSet>);
static_assert(
    !std::is_copy_constructible_v<
        PreparedPackageBaseArtifactInstall>);
static_assert(
    !std::is_copy_assignable_v<
        PreparedPackageBaseArtifactInstall>);
static_assert(
    std::is_nothrow_move_constructible_v<
        PreparedPackageBaseArtifactInstall>);
static_assert(
    !std::is_move_assignable_v<
        PreparedPackageBaseArtifactInstall>);
static_assert(
    std::is_nothrow_destructible_v<
        PreparedPackageBaseArtifactInstall>);
static_assert(!std::is_aggregate_v<PreparedPackageBaseArtifactInstall>);
static_assert(
    !std::is_constructible_v<
        PreparedPackageBaseArtifactInstall,
        std::string&&,
        ValidatedPackageArtifactSet&&,
        std::vector<ArtifactPackageIdentity>&&,
        std::vector<
            PreparedPackageBaseArtifactInstallSelectedArtifact>&&,
        std::vector<
            PreparedPackageBaseArtifactInstallUnselectedArtifact>&&,
        InstallReasonDirective,
        bool>);
static_assert(!HasArtifactPathMethod<PreparedPackageBaseArtifactInstall>);
static_assert(!HasArtifactPathsMethod<PreparedPackageBaseArtifactInstall>);

static_assert(
    !std::is_default_constructible_v<
        PackageBaseArtifactInstallExecutionResult>);

static_assert(
    !std::is_invocable_v<
        PreparedPackageBaseArtifactInstallExecutor,
        std::filesystem::path&,
        const ArtifactInstallExecutionOptions&>);
static_assert(
    !std::is_invocable_v<
        PreparedPackageBaseArtifactInstallExecutor,
        std::vector<std::filesystem::path>&,
        const ArtifactInstallExecutionOptions&>);
static_assert(
    !std::is_invocable_v<
        PackageBaseArtifactInstallPreparationFactory,
        ValidatedPackageArtifactSet&,
        const std::string&,
        const std::vector<ArtifactPackageIdentity>&,
        const ArtifactInstallPreparationOptions&,
        const PacmanDatabasePaths&>);

static_assert(
    !HasPathDataMember<
        PreparedPackageBaseArtifactInstallSelectedArtifact>);
static_assert(
    !HasArtifactPathDataMember<
        PreparedPackageBaseArtifactInstallSelectedArtifact>);
static_assert(
    !HasWorkspacePathDataMember<
        PreparedPackageBaseArtifactInstallSelectedArtifact>);
static_assert(
    !CanCleanupWorkspace<
        PreparedPackageBaseArtifactInstallSelectedArtifact>);
static_assert(
    !HasPathDataMember<
        PreparedPackageBaseArtifactInstallUnselectedArtifact>);
static_assert(
    !HasArtifactPathDataMember<
        PreparedPackageBaseArtifactInstallUnselectedArtifact>);
static_assert(
    !HasWorkspacePathDataMember<
        PreparedPackageBaseArtifactInstallUnselectedArtifact>);
static_assert(
    !CanCleanupWorkspace<
        PreparedPackageBaseArtifactInstallUnselectedArtifact>);

static_assert(
    !HasSelectedArtifacts<
        PackageBaseArtifactIdentitySelectionFailure>);
static_assert(
    !HasArtifactIndex<
        PackageBaseArtifactIdentitySelectionFailure>);
static_assert(
    !HasPathDataMember<
        PackageBaseArtifactIdentitySelectionFailure>);
static_assert(
    !HasArtifactIndex<DiagnosticPackageArtifactIdentityMatch>);
static_assert(
    !HasArtifactIndex<
        DuplicateProducedArtifactIdentityDiagnostic>);
static_assert(
    !HasArtifactIndex<
        ArtifactIdentitySelectionIdentityInconsistency>);
static_assert(
    !HasArtifactIndex<
        MixedPackageBaseInstallReasonArtifact>);
static_assert(
    !HasPathDataMember<
        MixedPackageBaseInstallReasonArtifact>);
static_assert(std::is_base_of_v<
              std::runtime_error,
              PackageBaseArtifactInstallTransactionError>);
static_assert(!HasPathDataMember<PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasArtifactPathDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasWorkspacePathDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasArtifactIndex<PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasDirectiveDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!HasOutcomeDataMember<
              PackageBaseArtifactInstallTransactionAttempt>);
static_assert(!CanCleanupWorkspace<
              PackageBaseArtifactInstallTransactionAttempt>);

namespace {

namespace fs = std::filesystem;
namespace metadata_stub = package_metadata_test_stub;
namespace process_stub = artifact_install_executor_test_stub;

constexpr const char* PACKAGE_BASE = "sample-base";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Callable>
std::string expect_runtime_error(
    Callable&& callable,
    const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const std::runtime_error& error) {
        return error.what();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " +
            error.what());
    }
    throw std::runtime_error(context + ": expected runtime_error");
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
            context + ": metadata failure code differs");
        expect(
            !error.failure().diagnostic.empty(),
            context + ": metadata diagnostic is empty");
        return error.failure();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " +
            error.what());
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
             "moguet-package-base-install-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');

        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create PackageBase install test directory.");
        }
        path_ = created_path;

        const char* previous = std::getenv("XDG_CACHE_HOME");
        if(previous != nullptr) previous_xdg_cache_home_ = previous;
        if(setenv("XDG_CACHE_HOME", path_.c_str(), 1) != 0) {
            std::error_code error;
            fs::remove_all(path_, error);
            throw std::runtime_error(
                "Failed to set PackageBase install test cache home.");
        }
    }

    TemporaryCacheHome(const TemporaryCacheHome&) = delete;
    TemporaryCacheHome& operator=(const TemporaryCacheHome&) = delete;

    ~TemporaryCacheHome() noexcept {
        if(previous_xdg_cache_home_.has_value()) {
            static_cast<void>(setenv(
                "XDG_CACHE_HOME",
                previous_xdg_cache_home_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("XDG_CACHE_HOME"));
        }

        std::error_code error;
        fs::remove_all(path_, error);
    }
};

void write_file(
    const fs::path& path,
    const std::string& contents = "fixture") {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error(
            "Failed to create PackageBase install fixture file: " +
            path.string());
    }
    output.write(
        contents.data(),
        static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error(
            "Failed to finish PackageBase install fixture file: " +
            path.string());
    }
}

fs::path signature_path(const fs::path& artifact_path) {
    return fs::path(artifact_path.string() + ".sig");
}

class ArtifactSetFixture final {
    std::unique_ptr<ValidatedPackageArtifactSet> artifacts_;
    std::vector<fs::path> artifact_paths_;
    fs::path workspace_path_;

public:
    explicit ArtifactSetFixture(
        const std::vector<std::string>& artifact_leaf_names,
        const std::vector<std::size_t>& signed_indices = {}) {
        ArtifactWorkspace workspace = create_artifact_workspace(
            prepare_private_trusted_cache_root(
                prepare_test_trusted_cache_root()));
        workspace_path_ = workspace.path();

        std::string packagelist_output;
        artifact_paths_.reserve(artifact_leaf_names.size());
        for(const std::string& leaf_name : artifact_leaf_names) {
            fs::path path = workspace.path() / leaf_name;
            artifact_paths_.push_back(path);
            packagelist_output += path.string() + "\n";
        }

        ExpectedPackageArtifactSet expected =
            validate_makepkg_packagelist_output_set(
                workspace, packagelist_output);
        for(std::size_t index = 0;
            index < artifact_paths_.size(); ++index) {
            write_file(
                artifact_paths_[index],
                "artifact-" + std::to_string(index));
        }
        for(std::size_t index : signed_indices) {
            if(index >= artifact_paths_.size()) {
                throw std::logic_error(
                    "Signed PackageBase fixture index is out of range.");
            }
            write_file(signature_path(artifact_paths_[index]), "signature");
        }

        ValidatedPackageArtifactSet artifacts =
            validate_post_build_package_artifacts(
                std::move(workspace), expected);
        artifacts_ = std::make_unique<ValidatedPackageArtifactSet>(
            std::move(artifacts));
    }

    ArtifactSetFixture(const ArtifactSetFixture&) = delete;
    ArtifactSetFixture& operator=(const ArtifactSetFixture&) = delete;

    ValidatedPackageArtifactSet& artifacts() {
        return *artifacts_;
    }

    const fs::path& path_at(std::size_t index) const {
        return artifact_paths_.at(index);
    }

    const fs::path& workspace_path() const noexcept {
        return workspace_path_;
    }

    std::size_t size() const noexcept {
        return artifact_paths_.size();
    }
};

PacmanDatabasePaths test_database_paths() {
    return PacmanDatabasePaths{"/", "/var/lib/pacman"};
}

RequiredPackageArtifactTarget target(
    const std::string& package_name,
    DesiredInstallReason desired_reason,
    const std::string& package_base = PACKAGE_BASE) {
    return RequiredPackageArtifactTarget{
        package_base, package_name, desired_reason};
}

std::string expected_shell_quote(const std::string& value) {
    std::string quoted = "'";
    for(char character : value) {
        if(character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

std::string expected_shell_join(
    const std::vector<std::string>& arguments) {
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
    const std::vector<fs::path>& artifact_paths,
    bool no_confirm,
    bool needed,
    const char* reason_option = nullptr) {
    std::vector<std::string> arguments = {"sudo", "pacman", "-U"};
    if(no_confirm) arguments.emplace_back("--noconfirm");
    if(needed) arguments.emplace_back("--needed");
    if(reason_option != nullptr) arguments.emplace_back(reason_option);
    arguments.emplace_back("--");
    for(const fs::path& path : artifact_paths) {
        arguments.push_back(path.string());
    }
    return expected_shell_join(arguments);
}

void reset_stubs() {
    static bool initialized = false;
    if(initialized) {
        process_stub::require_process_expectations_consumed();
        metadata_stub::require_local_package_query_expectations_consumed();
    }
    process_stub::reset_process_stub();
    metadata_stub::reset_alpm_stub();
    initialized = true;
}

void expect_identity_queries(
    const ArtifactSetFixture& fixture,
    const std::vector<ArtifactPackageIdentity>& identities) {
    expect(
        fixture.size() == identities.size(),
        "Identity fixture and expectation sizes differ");
    for(std::size_t index = 0; index < identities.size(); ++index) {
        process_stub::expect_capture_command(
            expected_identity_command(fixture.path_at(index)),
            CapturedCommandResult{
                identities[index].package_name + " " +
                    identities[index].full_version + "\n",
                0});
    }
}

PackageBaseArtifactInstallPreparationResult prepare_fixture(
    ArtifactSetFixture& fixture,
    const std::vector<RequiredPackageArtifactTarget>& targets,
    bool needed = false,
    bool rm_deps = false,
    const std::string& package_base = PACKAGE_BASE,
    PacmanDatabasePaths database_paths = test_database_paths()) {
    return prepare_package_base_artifact_install(
        fixture.artifacts(), package_base, targets,
        ArtifactInstallPreparationOptions{needed, rm_deps},
        database_paths);
}

PreparedPackageBaseArtifactInstall& expect_prepared(
    PackageBaseArtifactInstallPreparationResult& result,
    const std::string& context) {
    expect(result.is_prepared(), context + ": result is not prepared");
    expect(result.failure() == nullptr, context + ": result has failure arm");
    PreparedPackageBaseArtifactInstall* prepared = result.prepared();
    expect(prepared != nullptr, context + ": prepared payload is null");
    return *prepared;
}

const PackageBaseArtifactInstallPreparationFailure& expect_failure(
    const PackageBaseArtifactInstallPreparationResult& result,
    const std::string& context) {
    expect(!result.is_prepared(), context + ": result is unexpectedly prepared");
    expect(result.prepared() == nullptr, context + ": result has prepared arm");
    const PackageBaseArtifactInstallPreparationFailure* failure =
        result.failure();
    expect(failure != nullptr, context + ": failure payload is null");
    return *failure;
}

void expect_caller_ownership(
    ArtifactSetFixture& fixture,
    const std::string& context) {
    fixture.artifacts().require_validity();
    expect(
        fs::is_directory(fixture.workspace_path()),
        context + ": caller workspace is missing");
    for(std::size_t index = 0; index < fixture.size(); ++index) {
        expect(
            fs::is_regular_file(fixture.path_at(index)),
            context + ": caller artifact is missing");
    }
}

void expect_source_was_moved(
    ArtifactSetFixture& fixture,
    const std::string& context) {
    static_cast<void>(expect_runtime_error(
        [&fixture]() { fixture.artifacts().require_validity(); },
        context));
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

void require_metadata_released_before_run() {
    expect(
        metadata_stub::created_handle_count() == 1,
        "Executor did not observe exactly one preparation metadata session");
    expect(
        metadata_stub::release_call_count() == 1 &&
            metadata_stub::release_count_for_handle(0) == 1,
        "Executor started before the preparation metadata session was released");
}

void require_metadata_released_before_reason_plan() {
    expect(
        metadata_stub::created_handle_count() == 1,
        "Reason reducer did not observe exactly one preparation metadata session");
    expect(
        metadata_stub::release_call_count() == 1 &&
            metadata_stub::release_count_for_handle(0) == 1,
        "Reason reducer started before the preparation metadata session was released");
}

class ScopedReasonPlanObserver final {
public:
    ScopedReasonPlanObserver() {
        set_package_base_artifact_install_reason_plan_observer_for_test(
            require_metadata_released_before_reason_plan);
    }

    ~ScopedReasonPlanObserver() {
        set_package_base_artifact_install_reason_plan_observer_for_test(
            nullptr);
    }

    ScopedReasonPlanObserver(const ScopedReasonPlanObserver&) = delete;
    ScopedReasonPlanObserver& operator=(const ScopedReasonPlanObserver&) =
        delete;
};

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

void expect_exact_coverage(
    const PreparedPackageBaseArtifactInstall& prepared,
    std::size_t artifact_count,
    const std::string& context) {
    std::vector<bool> covered(artifact_count, false);
    for(const auto& selected : prepared.selected_artifacts()) {
        expect(
            selected.artifact_index < artifact_count,
            context + ": selected index is out of range");
        expect(
            !covered[selected.artifact_index],
            context + ": selected index is duplicated");
        covered[selected.artifact_index] = true;
    }
    for(const auto& unselected : prepared.unselected_artifacts()) {
        expect(
            unselected.artifact_index < artifact_count,
            context + ": unselected index is out of range");
        expect(
            !covered[unselected.artifact_index],
            context + ": selected/unselected index is duplicated");
        covered[unselected.artifact_index] = true;
    }
    for(bool is_covered : covered) {
        expect(is_covered, context + ": aggregate index is missing");
    }
}

void test_selection_order_and_success_ownership() {
    {
        ArtifactSetFixture fixture({"ordinary.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"ordinary", "2:1.0-3"}});
        metadata_stub::enqueue_local_package_query_absent("ordinary");

        PackageBaseArtifactInstallPreparationResult result =
            prepare_fixture(
                fixture,
                {target("ordinary", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "ordinary selected artifact");
        expect(
            prepared.selected_artifacts().size() == 1 &&
                prepared.selected_artifacts()[0].artifact_index == 0,
            "Ordinary artifact selection differs");
        expect(
            prepared.unselected_artifacts().empty(),
            "Ordinary artifact has an unselected sibling");
        expect_exact_coverage(prepared, 1, "ordinary selection");
        expect_source_was_moved(fixture, "ordinary success ownership");
        expect(
            process_stub::run_command_call_count() == 0,
            "Ordinary preparation ran pacman -U");
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"base.pkg.tar.zst", "child.pkg.tar.zst",
             "debug.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"sample-base", "1-1"},
             {"sample-child", "1-1"},
             {"sample-debug", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("sample-child");

        PackageBaseArtifactInstallPreparationResult result =
            prepare_fixture(
                fixture,
                {target(
                    "sample-child",
                    DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "requested split child");
        expect(
            prepared.selected_artifacts().size() == 1 &&
                prepared.selected_artifacts()[0].artifact_index == 1 &&
                prepared.selected_artifacts()[0]
                        .identity.package_name ==
                    "sample-child",
            "Requested split child was not selected");
        expect(
            prepared.unselected_artifacts().size() == 2 &&
                prepared.unselected_artifacts()[0].artifact_index == 0 &&
                prepared.unselected_artifacts()[1].artifact_index == 2,
            "Split siblings do not preserve aggregate order");
        expect_exact_coverage(prepared, 3, "split child selection");
        expect(
            metadata_stub::local_package_query_history() ==
                std::vector<std::string>{"sample-child"},
            "Unselected split sibling reached metadata query");
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"sibling.pkg.tar.zst", "child-b.pkg.tar.zst",
             "debug.pkg.tar.zst", "child-a.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"sibling", "1-1"},
             {"child-b", "2-1"},
             {"debug-output", "3-1"},
             {"child-a", "4-1"}});
        metadata_stub::enqueue_local_package_query_absent("child-a");
        metadata_stub::enqueue_local_package_query_absent("child-b");

        PackageBaseArtifactInstallPreparationResult result =
            prepare_fixture(
                fixture,
                {target("child-a", DesiredInstallReason::Explicit),
                 target("child-b", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "multiple selected children");
        const auto& selected = prepared.selected_artifacts();
        const auto& unselected = prepared.unselected_artifacts();
        expect(
            selected.size() == 2 &&
                selected[0].artifact_index == 3 &&
                selected[0].identity.package_name == "child-a" &&
                selected[1].artifact_index == 1 &&
                selected[1].identity.package_name == "child-b",
            "Selected artifacts do not preserve required target order");
        expect(
            unselected.size() == 2 &&
                unselected[0].artifact_index == 0 &&
                unselected[1].artifact_index == 2,
            "Unselected artifacts do not preserve aggregate order");
        expect_exact_coverage(prepared, 4, "multiple selection");
        expect(
            process_stub::capture_command_call_count() == 4,
            "Multiple preparation identity capture count differs");
        expect(
            metadata_stub::local_package_query_history() ==
                std::vector<std::string>{"child-a", "child-b"},
            "Selected metadata query order differs");
        expect(
            metadata_stub::initialize_call_count() == 1 &&
                metadata_stub::created_handle_count() == 1 &&
                metadata_stub::release_call_count() == 1 &&
                metadata_stub::release_count_for_handle(0) == 1,
            "Multiple preparation did not use one released metadata session");
        expect(
            process_stub::run_command_call_count() == 0,
            "Multiple preparation ran pacman -U");
        prepared.cleanup_workspace();
    }
}

void test_selection_failure_and_preparation_guards() {
    {
        ArtifactSetFixture fixture(
            {"selected.pkg.tar.zst", "sibling.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"selected", "1-1"}, {"sibling", "1-1"}});

        PackageBaseArtifactInstallPreparationResult result =
            prepare_fixture(
                fixture,
                {target("selected", DesiredInstallReason::Explicit),
                 target("missing", DesiredInstallReason::Explicit)});
        const std::string selection_failure_context = "selection failure";
        const PackageBaseArtifactInstallPreparationFailure& failure =
            expect_failure(result, selection_failure_context);
        const PackageBaseArtifactIdentitySelectionFailure* selection_failure =
            failure.selection_failure();
        expect(
            selection_failure != nullptr &&
                failure.mixed_reason_failure() == nullptr,
            "Selection failure arm differs");
        expect(
            selection_failure->missing_required_artifacts.size() == 1 &&
                selection_failure->missing_required_artifacts[0]
                        .target.package_name ==
                    "missing",
            "Selection diagnostic lost the missing target");
        expect(
            process_stub::capture_command_call_count() == fixture.size(),
            "Selection failure did not capture every artifact identity");
        expect(
            metadata_stub::initialize_call_count() == 0 &&
                metadata_stub::package_query_call_count() == 0,
            "Selection failure opened a metadata session");
        expect(
            process_stub::run_command_call_count() == 0,
            "Selection failure ran an install transaction");
        expect_caller_ownership(fixture, "selection failure ownership");
        fixture.artifacts().retain_workspace_for_diagnostics();
        fixture.artifacts().cleanup_workspace();
        expect(
            !fs::exists(fixture.workspace_path()),
            "Caller could not clean selection-failure workspace");
    }

    {
        ArtifactSetFixture fixture({"identity-failure.pkg.tar.zst"});
        reset_stubs();
        process_stub::expect_capture_command(
            expected_identity_command(fixture.path_at(0)),
            CapturedCommandResult{"ignored", 42});
        static_cast<void>(expect_runtime_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target(
                        "identity-failure",
                        DesiredInstallReason::Explicit)}));
            },
            "identity query failure"));
        expect_caller_ownership(fixture, "identity query failure ownership");
        expect(
            metadata_stub::initialize_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "Identity query failure crossed a later external boundary");
        fixture.artifacts().cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"rmdeps.pkg.tar.zst"});
        reset_stubs();
        static_cast<void>(expect_runtime_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target(
                        "rmdeps",
                        DesiredInstallReason::Explicit)},
                    false, true));
            },
            "rmdeps guard"));
        expect(
            process_stub::capture_command_call_count() == 0 &&
                metadata_stub::initialize_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "rmdeps guard reached an external boundary");
        expect_caller_ownership(fixture, "rmdeps guard ownership");
        fixture.artifacts().cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"moved.pkg.tar.zst"});
        ValidatedPackageArtifactSet owner = std::move(fixture.artifacts());
        reset_stubs();
        static_cast<void>(expect_runtime_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target(
                        "moved",
                        DesiredInstallReason::Explicit)}));
            },
            "moved-from artifact aggregate"));
        expect(
            process_stub::capture_command_call_count() == 0 &&
                metadata_stub::initialize_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "Moved-from artifact aggregate reached an external boundary");
        owner.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"cleaned.pkg.tar.zst"});
        fixture.artifacts().cleanup_workspace();
        reset_stubs();
        static_cast<void>(expect_runtime_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target(
                        "cleaned",
                        DesiredInstallReason::Explicit)}));
            },
            "cleaned artifact aggregate"));
        expect(
            process_stub::capture_command_call_count() == 0 &&
                metadata_stub::initialize_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "Cleaned artifact aggregate reached an external boundary");
    }
}

void test_selected_metadata_mapping_and_order() {
    ArtifactSetFixture fixture(
        {"unselected.pkg.tar.zst", "absent.pkg.tar.zst",
         "same.pkg.tar.zst", "different-explicit.pkg.tar.zst",
         "different-dependency.pkg.tar.zst"});
    reset_stubs();
    expect_identity_queries(
        fixture,
        {{"unselected", "9-1"},
         {"absent", "3-1"},
         {"same", "2:1.0-3"},
         {"different-explicit", "4-1"},
         {"different-dependency", "5-1"}});
    metadata_stub::enqueue_local_package_query_present(
        "same", "same", "2:1.0-3", ALPM_PKG_REASON_EXPLICIT);
    metadata_stub::enqueue_local_package_query_present(
        "different-dependency", "different-dependency", "5-0",
        ALPM_PKG_REASON_DEPEND);
    metadata_stub::enqueue_local_package_query_absent("absent");
    metadata_stub::enqueue_local_package_query_present(
        "different-explicit", "different-explicit", "3-9",
        ALPM_PKG_REASON_EXPLICIT);

    const PacmanDatabasePaths database_paths{
        "/pre-resolved-root", "/pre-resolved-database"};

    PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
        fixture,
        {target("same", DesiredInstallReason::Explicit),
         target("different-dependency", DesiredInstallReason::Explicit),
         target("absent", DesiredInstallReason::Explicit),
         target("different-explicit", DesiredInstallReason::Explicit)},
        false, false, PACKAGE_BASE, database_paths);
    PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
        result, "selected metadata mapping");
    const auto& selected = prepared.selected_artifacts();
    expect(selected.size() == 4, "Selected metadata result size differs");
    expect(
        selected[0].artifact_index == 2 &&
            selected[0].installed_version_state ==
                InstalledVersionState::SameVersion &&
            selected[0].existing_reason ==
                ExistingInstallReason::Explicit &&
            selected[0].directive == InstallReasonDirective::Default,
        "Same-version explicit metadata mapping differs");
    expect(
        selected[1].artifact_index == 4 &&
            selected[1].installed_version_state ==
                InstalledVersionState::DifferentVersion &&
            selected[1].existing_reason ==
                ExistingInstallReason::Dependency &&
            selected[1].directive ==
                InstallReasonDirective::AsExplicit,
        "Different-version dependency metadata mapping differs");
    expect(
        selected[2].artifact_index == 1 &&
            selected[2].installed_version_state ==
                InstalledVersionState::NotInstalled &&
            !selected[2].existing_reason.has_value() &&
            selected[2].directive == InstallReasonDirective::Default,
        "Absent package metadata mapping differs");
    expect(
        selected[3].artifact_index == 3 &&
            selected[3].installed_version_state ==
                InstalledVersionState::DifferentVersion &&
            selected[3].existing_reason ==
                ExistingInstallReason::Explicit &&
            selected[3].directive == InstallReasonDirective::Default,
        "Different-version explicit metadata mapping differs");
    expect(
        prepared.transaction_directive() ==
            InstallReasonDirective::AsExplicit,
        "Selected metadata transaction directive differs");
    expect(
        metadata_stub::local_package_query_history() ==
            std::vector<std::string>{
                "same", "different-dependency", "absent",
                "different-explicit"},
        "Selected metadata query order differs");
    expect(
        metadata_stub::initialize_call_count() == 1 &&
            metadata_stub::release_count_for_handle(0) == 1,
        "Selected metadata session lifetime differs");
    expect(
        metadata_stub::last_initialize_root() ==
                database_paths.root_dir.string() &&
            metadata_stub::last_initialize_database_path() ==
                database_paths.db_path.string(),
        "Preparation did not use the caller-resolved database path snapshot");
    expect(
        process_stub::run_command_call_count() == 0,
        "Metadata preparation ran pacman -U");
    prepared.cleanup_workspace();
}

void test_metadata_failures_preserve_ownership() {
    for(std::size_t failure_index = 0; failure_index < 3; ++failure_index) {
        ArtifactSetFixture fixture(
            {"query-a.pkg.tar.zst", "query-b.pkg.tar.zst",
             "query-c.pkg.tar.zst"});
        const std::vector<ArtifactPackageIdentity> identities = {
            {"query-a", "1-1"},
            {"query-b", "1-1"},
            {"query-c", "1-1"}};
        reset_stubs();
        expect_identity_queries(fixture, identities);
        for(std::size_t index = 0; index < failure_index; ++index) {
            metadata_stub::enqueue_local_package_query_present(
                identities[index].package_name,
                identities[index].package_name,
                "0-1", ALPM_PKG_REASON_EXPLICIT);
        }
        metadata_stub::enqueue_local_package_query_failure(
            identities[failure_index].package_name,
            ALPM_ERR_DB_OPEN);

        static_cast<void>(expect_package_metadata_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target("query-a", DesiredInstallReason::Explicit),
                     target("query-b", DesiredInstallReason::Explicit),
                     target("query-c", DesiredInstallReason::Explicit)}));
            },
            PackageMetadataErrorCode::QueryFailed,
            "selected metadata query failure " +
                std::to_string(failure_index)));
        expect(
            metadata_stub::package_query_call_count() ==
                failure_index + 1,
            "Metadata failure position query count differs");
        expect(
            metadata_stub::release_call_count() == 1 &&
                metadata_stub::release_count_for_handle(0) == 1,
            "Metadata query failure leaked its session");
        expect(
            process_stub::run_command_call_count() == 0,
            "Metadata query failure ran pacman -U");
        expect_caller_ownership(
            fixture,
            "metadata query failure " + std::to_string(failure_index));
        fixture.artifacts().cleanup_workspace();
        process_stub::require_process_expectations_consumed();
        metadata_stub::require_local_package_query_expectations_consumed();
    }

    enum class MalformedKind {
        NameMismatch,
        EmptyVersion,
        UnknownReason,
    };
    const std::vector<MalformedKind> malformed_cases = {
        MalformedKind::NameMismatch,
        MalformedKind::EmptyVersion,
        MalformedKind::UnknownReason};
    for(std::size_t index = 0; index < malformed_cases.size(); ++index) {
        ArtifactSetFixture fixture(
            {"malformed-" + std::to_string(index) + ".pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(fixture, {{"malformed", "1-1"}});
        switch(malformed_cases[index]) {
            case MalformedKind::NameMismatch:
                metadata_stub::enqueue_local_package_query_present(
                    "malformed", "different-name", "1-1",
                    ALPM_PKG_REASON_EXPLICIT);
                break;
            case MalformedKind::EmptyVersion:
                metadata_stub::enqueue_local_package_query_present(
                    "malformed", "malformed", "",
                    ALPM_PKG_REASON_EXPLICIT);
                break;
            case MalformedKind::UnknownReason:
                metadata_stub::enqueue_local_package_query_present(
                    "malformed", "malformed", "1-1",
                    ALPM_PKG_REASON_UNKNOWN);
                break;
        }

        static_cast<void>(expect_package_metadata_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target(
                        "malformed",
                        DesiredInstallReason::Explicit)}));
            },
            PackageMetadataErrorCode::MalformedMetadata,
            "malformed selected metadata " + std::to_string(index)));
        expect(
            metadata_stub::release_count_for_handle(0) == 1,
            "Malformed metadata leaked its session");
        expect(
            process_stub::run_command_call_count() == 0,
            "Malformed metadata ran pacman -U");
        expect_caller_ownership(
            fixture,
            "malformed metadata ownership " + std::to_string(index));
        fixture.artifacts().cleanup_workspace();
        process_stub::require_process_expectations_consumed();
        metadata_stub::require_local_package_query_expectations_consumed();
    }
}

void test_mixed_reason_typed_failure() {
    ArtifactSetFixture fixture(
        {"root.pkg.tar.zst", "dependency.pkg.tar.zst"});
    reset_stubs();
    expect_identity_queries(
        fixture,
        {{"root-package", "1-1"}, {"dependency-package", "1-1"}});
    metadata_stub::enqueue_local_package_query_absent("root-package");
    metadata_stub::enqueue_local_package_query_absent("dependency-package");

    PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
        fixture,
        {target("root-package", DesiredInstallReason::Explicit),
         target("dependency-package", DesiredInstallReason::Dependency)});
    const std::string mixed_reason_context = "mixed reason failure";
    const PackageBaseArtifactInstallPreparationFailure& failure =
        expect_failure(result, mixed_reason_context);
    const MixedPackageBaseInstallReasonUnsupported* mixed =
        failure.mixed_reason_failure();
    expect(
        mixed != nullptr && failure.selection_failure() == nullptr,
        "Mixed reason failure arm differs");
    expect(
        mixed->package_base == PACKAGE_BASE &&
            mixed->selected_artifacts.size() == 2,
        "Mixed reason diagnostic aggregate differs");
    expect(
        mixed->selected_artifacts[0].identity.package_name ==
                "root-package" &&
            mixed->selected_artifacts[0].desired_reason ==
                DesiredInstallReason::Explicit &&
            mixed->selected_artifacts[0].directive ==
                InstallReasonDirective::Default &&
            mixed->selected_artifacts[1].identity.package_name ==
                "dependency-package" &&
            mixed->selected_artifacts[1].desired_reason ==
                DesiredInstallReason::Dependency &&
            mixed->selected_artifacts[1].directive ==
                InstallReasonDirective::AsDependency,
        "Mixed reason diagnostic lost selected policy state");
    expect(
        metadata_stub::release_count_for_handle(0) == 1,
        "Mixed reason reduction occurred before metadata session release");
    expect(
        process_stub::run_command_call_count() == 0,
        "Mixed reason failure ran pacman -U");
    expect_caller_ownership(fixture, "mixed reason failure ownership");
    fixture.artifacts().cleanup_workspace();
}

void test_needed_outcomes_and_stronger_global_directives() {
    {
        ArtifactSetFixture fixture(
            {"skip-a.pkg.tar.zst", "skip-b.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"skip-a", "1-1"}, {"skip-b", "2-1"}});
        metadata_stub::enqueue_local_package_query_present(
            "skip-a", "skip-a", "1-1", ALPM_PKG_REASON_EXPLICIT);
        metadata_stub::enqueue_local_package_query_present(
            "skip-b", "skip-b", "2-1", ALPM_PKG_REASON_EXPLICIT);

        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("skip-a", DesiredInstallReason::Explicit),
             target("skip-b", DesiredInstallReason::Explicit)},
            true);
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "all same-version needed");
        expect(
            prepared.transaction_directive() ==
                InstallReasonDirective::Default,
            "All-skip transaction directive differs");
        for(const auto& selected : prepared.selected_artifacts()) {
            expect(
                selected.expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        SkippedAsNeeded,
                "All-skip preparation produced an install outcome");
        }
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"skip.pkg.tar.zst", "install.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"skip", "1-1"}, {"install", "2-1"}});
        metadata_stub::enqueue_local_package_query_present(
            "skip", "skip", "1-1", ALPM_PKG_REASON_EXPLICIT);
        metadata_stub::enqueue_local_package_query_present(
            "install", "install", "2-0", ALPM_PKG_REASON_EXPLICIT);

        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("skip", DesiredInstallReason::Explicit),
             target("install", DesiredInstallReason::Explicit)},
            true);
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "mixed needed outcomes");
        expect(
            prepared.selected_artifacts()[0].expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        SkippedAsNeeded &&
                prepared.selected_artifacts()[1].expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        Installed,
            "Mixed needed outcomes do not preserve selected order");
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"explicit-skip.pkg.tar.zst",
             "explicit-promote.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"explicit-skip", "1-1"},
             {"explicit-promote", "2-1"}});
        metadata_stub::enqueue_local_package_query_present(
            "explicit-skip", "explicit-skip", "1-1",
            ALPM_PKG_REASON_EXPLICIT);
        metadata_stub::enqueue_local_package_query_present(
            "explicit-promote", "explicit-promote", "2-0",
            ALPM_PKG_REASON_DEPEND);

        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("explicit-skip", DesiredInstallReason::Explicit),
             target("explicit-promote", DesiredInstallReason::Explicit)},
            true);
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "global AsExplicit safe skip");
        expect(
            prepared.transaction_directive() ==
                    InstallReasonDirective::AsExplicit &&
                prepared.selected_artifacts()[0].directive ==
                    InstallReasonDirective::Default &&
                prepared.selected_artifacts()[0].expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        SkippedAsNeeded &&
                prepared.selected_artifacts()[1].directive ==
                    InstallReasonDirective::AsExplicit &&
                prepared.selected_artifacts()[1].expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        Installed,
            "Global AsExplicit safe-skip proof differs");
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"dependency-skip.pkg.tar.zst",
             "dependency-new.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"dependency-skip", "1-1"},
             {"dependency-new", "2-1"}});
        metadata_stub::enqueue_local_package_query_present(
            "dependency-skip", "dependency-skip", "1-1",
            ALPM_PKG_REASON_DEPEND);
        metadata_stub::enqueue_local_package_query_absent("dependency-new");

        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("dependency-skip", DesiredInstallReason::Dependency),
             target("dependency-new", DesiredInstallReason::Dependency)},
            true);
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "global AsDependency safe skip");
        expect(
            prepared.transaction_directive() ==
                    InstallReasonDirective::AsDependency &&
                prepared.selected_artifacts()[0].directive ==
                    InstallReasonDirective::Default &&
                prepared.selected_artifacts()[0].expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        SkippedAsNeeded &&
                prepared.selected_artifacts()[1].directive ==
                    InstallReasonDirective::AsDependency &&
                prepared.selected_artifacts()[1].expected_outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        Installed,
            "Global AsDependency safe-skip proof differs");
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"same-reason-change.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"same-reason-change", "1-1"}});
        metadata_stub::enqueue_local_package_query_present(
            "same-reason-change", "same-reason-change", "1-1",
            ALPM_PKG_REASON_DEPEND);

        static_cast<void>(expect_runtime_error(
            [&fixture]() {
                static_cast<void>(prepare_fixture(
                    fixture,
                    {target(
                        "same-reason-change",
                        DesiredInstallReason::Explicit)},
                    true));
            },
            "same-version reason change with needed"));
        expect_caller_ownership(
            fixture, "same-version reason-change ownership");
        expect(
            metadata_stub::release_count_for_handle(0) == 1 &&
                process_stub::run_command_call_count() == 0,
            "Same-version reason-change failure crossed transaction boundary");
        fixture.artifacts().cleanup_workspace();
    }
}

void test_exact_executor_argv_and_results() {
    struct SingleExecutionCase {
        const char* name;
        DesiredInstallReason desired_reason;
        bool installed;
        const char* installed_version;
        alpm_pkgreason_t installed_reason;
        bool no_confirm;
        const char* reason_option;
    };
    const std::vector<SingleExecutionCase> cases = {
        {"default", DesiredInstallReason::Explicit, false, "",
         ALPM_PKG_REASON_EXPLICIT, false, nullptr},
        {"asexplicit", DesiredInstallReason::Explicit, true, "0-1",
         ALPM_PKG_REASON_DEPEND, true, "--asexplicit"},
        {"asdependency", DesiredInstallReason::Dependency, false, "",
         ALPM_PKG_REASON_EXPLICIT, false, "--asdeps"}};

    for(std::size_t index = 0; index < cases.size(); ++index) {
        const SingleExecutionCase& test_case = cases[index];
        const std::string package_name =
            "executor-" + std::to_string(index);
        ArtifactSetFixture fixture(
            {package_name + ".pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(fixture, {{package_name, "1-1"}});
        if(test_case.installed) {
            metadata_stub::enqueue_local_package_query_present(
                package_name, package_name,
                test_case.installed_version,
                test_case.installed_reason);
        } else {
            metadata_stub::enqueue_local_package_query_absent(package_name);
        }

        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target(package_name, test_case.desired_reason)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, std::string(test_case.name) + " executor preparation");
        const MetadataCallCounts before_execute = metadata_call_counts();
        const std::string expected_command = expected_install_command(
            {fixture.path_at(0)}, test_case.no_confirm, false,
            test_case.reason_option);
        process_stub::expect_run_command(expected_command, 0);
        process_stub::set_run_hook(require_metadata_released_before_run);

        PackageBaseArtifactInstallExecutionResult execution =
            execute_prepared_package_base_artifact_install(
                prepared,
                ArtifactInstallExecutionOptions{
                    test_case.no_confirm});
        expect(
            process_stub::run_command_call_count() == 1 &&
                process_stub::last_run_command() == expected_command,
            std::string(test_case.name) +
                ": exact one-artifact command differs");
        expect(
            count_occurrences(expected_command, "'--asexplicit'") +
                    count_occurrences(expected_command, "'--asdeps'") <=
                1,
            std::string(test_case.name) +
                ": command has multiple reason options");
        expect(
            expected_command.find("'-D'") == std::string::npos,
            std::string(test_case.name) +
                ": command uses a pacman -D fallback");
        expect(
            execution.is_success() &&
                execution.package_base() == PACKAGE_BASE &&
                execution.selected_artifacts().size() == 1 &&
                execution.selected_artifacts()[0].artifact_index == 0 &&
                execution.selected_artifacts()[0]
                        .identity.package_name ==
                    package_name &&
                execution.selected_artifacts()[0]
                        .identity.full_version ==
                    "1-1" &&
                execution.selected_artifacts()[0].desired_reason ==
                    test_case.desired_reason &&
                execution.selected_artifacts()[0].outcome ==
                    PackageBaseArtifactInstallExpectedOutcome::
                        Installed,
            std::string(test_case.name) +
                ": typed execution result differs");
        expect(
            metadata_call_counts() == before_execute,
            std::string(test_case.name) +
                ": executor opened a metadata session");
        expect(
            fs::exists(fixture.path_at(0)) &&
                fs::exists(fixture.workspace_path()),
            std::string(test_case.name) +
                ": executor auto-cleaned the workspace");

        if(index == 0) {
            static_cast<void>(expect_runtime_error(
                [&prepared]() {
                    static_cast<void>(
                        execute_prepared_package_base_artifact_install(
                            prepared,
                            ArtifactInstallExecutionOptions{}));
                },
                "successful transaction replay"));
            expect(
                process_stub::run_command_call_count() == 1,
                "Successful prepared transaction replayed pacman");
        }
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"unselected sibling.pkg.tar.zst",
             "artifact'quote.pkg.tar.zst",
             "artifact with space.pkg.tar.zst",
             "-leading-dash.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"unselected-executor", "9-1"},
             {"quote-executor", "1-1"},
             {"space-executor", "2-1"},
             {"dash-executor", "3-1"}});
        metadata_stub::enqueue_local_package_query_present(
            "dash-executor", "dash-executor", "3-1",
            ALPM_PKG_REASON_EXPLICIT);
        metadata_stub::enqueue_local_package_query_present(
            "space-executor", "space-executor", "2-0",
            ALPM_PKG_REASON_EXPLICIT);
        metadata_stub::enqueue_local_package_query_absent("quote-executor");

        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("dash-executor", DesiredInstallReason::Explicit),
             target("space-executor", DesiredInstallReason::Explicit),
             target("quote-executor", DesiredInstallReason::Explicit)},
            true);
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "multiple executor preparation");
        const std::vector<fs::path> selected_paths = {
            fixture.path_at(3), fixture.path_at(2), fixture.path_at(1)};
        const std::string expected_command = expected_install_command(
            selected_paths, true, true);
        process_stub::expect_run_command(expected_command, 0);
        process_stub::set_run_hook(require_metadata_released_before_run);

        PackageBaseArtifactInstallExecutionResult execution =
            execute_prepared_package_base_artifact_install(
                prepared, ArtifactInstallExecutionOptions{true});
        expect(
            process_stub::run_command_call_count() == 1 &&
                process_stub::last_run_command() == expected_command,
            "Exact multiple-artifact command differs");
        expect(
            expected_command.find(
                "'--' " +
                expected_shell_quote(selected_paths[0].string())) !=
                std::string::npos,
            "Semantic -- is not directly before selected artifact paths");
        expect(
            expected_command.find(
                expected_shell_quote(fixture.path_at(0).string())) ==
                std::string::npos,
            "Unselected sibling path reached pacman -U");
        expect(
            count_occurrences(expected_command, "'--asexplicit'") +
                    count_occurrences(expected_command, "'--asdeps'") ==
                0,
            "Default multi-artifact command emitted a reason option");
        expect(
            execution.selected_artifacts().size() == 3,
            "Multiple execution result size differs");
        const std::vector<std::size_t> expected_indices = {3, 2, 1};
        const std::vector<ArtifactPackageIdentity> expected_identities = {
            {"dash-executor", "3-1"},
            {"space-executor", "2-1"},
            {"quote-executor", "1-1"}};
        const std::vector<PackageBaseArtifactInstallExpectedOutcome>
            expected_outcomes = {
                PackageBaseArtifactInstallExpectedOutcome::
                    SkippedAsNeeded,
                PackageBaseArtifactInstallExpectedOutcome::Installed,
                PackageBaseArtifactInstallExpectedOutcome::Installed};
        for(std::size_t index = 0; index < expected_indices.size(); ++index) {
            expect(
                execution.selected_artifacts()[index].artifact_index ==
                        expected_indices[index] &&
                    execution.selected_artifacts()[index]
                            .identity.package_name ==
                        expected_identities[index].package_name &&
                    execution.selected_artifacts()[index]
                            .identity.full_version ==
                        expected_identities[index].full_version &&
                    execution.selected_artifacts()[index]
                            .desired_reason ==
                        DesiredInstallReason::Explicit &&
                    execution.selected_artifacts()[index].outcome ==
                        expected_outcomes[index],
                "Multiple execution result payload/order differs");
        }
        expect(
            fs::exists(fixture.workspace_path()),
            "Multiple executor auto-cleaned the workspace");
        prepared.cleanup_workspace();
        expect(
            !fs::exists(fixture.workspace_path()),
            "Explicit multiple workspace cleanup failed");
    }
}

void replace_regular_file(
    const fs::path& target_path,
    fs::path& original_path) {
    original_path = fs::path(target_path.string() + ".executor-original");
    fs::rename(target_path, original_path);
    write_file(target_path, "replacement");
}

void restore_regular_file(
    const fs::path& target_path,
    const fs::path& original_path) {
    expect(fs::remove(target_path), "Replacement file was not removed");
    fs::rename(original_path, target_path);
}

void test_executor_revalidation() {
    for(int mutated_part : {0, 1, 2}) {
        ArtifactSetFixture fixture({"selected.pkg.tar.zst", "unselected.pkg.tar.zst"}, {0});
        reset_stubs();
        expect_identity_queries(fixture, {{"selected", "1-1"}, {"unselected", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("selected");
        auto result = prepare_fixture(fixture, {target("selected", DesiredInstallReason::Explicit)});
        auto& prepared = expect_prepared(result, "same-inode preparation");
        const auto path = mutated_part == 2 ? signature_path(fixture.path_at(0)) : fixture.path_at(mutated_part);
        struct stat before{}, after{};
        expect(stat(path.c_str(), &before) == 0, "Cannot stat original fixture");
        write_file(path, std::string(before.st_size, 'x'));
        const timespec times[] = {before.st_atim, before.st_mtim};
        expect(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0, "Cannot restore fixture mtime");
        expect(stat(path.c_str(), &after) == 0 && before.st_ino == after.st_ino && before.st_size == after.st_size,
               "Mutation regression did not retain inode and size");
        static_cast<void>(expect_runtime_error([&] {
            static_cast<void>(execute_prepared_package_base_artifact_install(prepared, {}));
        },
                                               "same-inode content mutation"));
        expect(process_stub::run_command_call_count() == 0, "Mutated content produced a transaction or Installed result");
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"artifact-one.pkg.tar.zst",
             "artifact-two.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"artifact-one", "1-1"}, {"artifact-two", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("artifact-two");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("artifact-two", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "artifact replacement preparation");

        fs::path original_path;
        replace_regular_file(fixture.path_at(1), original_path);
        const std::string diagnostic = expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "artifact replacement before command");
        expect(
            process_stub::run_command_call_count() == 0,
            "Artifact replacement reached pacman -U");
        expect(
            diagnostic.find(fixture.path_at(1).string()) ==
                std::string::npos,
            "Artifact replacement diagnostic exposed a package path");
        restore_regular_file(fixture.path_at(1), original_path);
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture(
            {"signed-one.pkg.tar.zst",
             "signed-two.pkg.tar.zst"},
            {1});
        reset_stubs();
        expect_identity_queries(
            fixture,
            {{"signed-one", "1-1"}, {"signed-two", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("signed-two");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("signed-two", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "signature replacement preparation");

        const fs::path target_signature = signature_path(fixture.path_at(1));
        fs::path original_signature;
        replace_regular_file(target_signature, original_signature);
        static_cast<void>(expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "signature replacement before command"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Signature replacement reached pacman -U");
        restore_regular_file(target_signature, original_signature);
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"workspace.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(fixture, {{"workspace", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("workspace");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("workspace", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "workspace replacement preparation");
        prepared.retain_workspace_for_diagnostics();

        const fs::path workspace_path = fixture.workspace_path();
        const fs::path original_workspace =
            fs::path(workspace_path.string() + ".executor-original");
        fs::rename(workspace_path, original_workspace);
        fs::create_directory(workspace_path);
        fs::permissions(
            workspace_path, fs::perms::owner_all,
            fs::perm_options::replace);

        static_cast<void>(expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "workspace replacement before command"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Workspace replacement reached pacman -U");
        fs::remove_all(workspace_path);
        fs::rename(original_workspace, workspace_path);
        prepared.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"unexpected.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(fixture, {{"unexpected", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("unexpected");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("unexpected", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "unexpected entry preparation");

        const fs::path unexpected_path =
            fixture.workspace_path() / "unexpected-entry";
        write_file(unexpected_path, "unexpected");
        static_cast<void>(expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "unexpected workspace entry before command"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Unexpected workspace entry reached pacman -U");
        expect(fs::remove(unexpected_path), "Unexpected fixture entry remained");
        prepared.cleanup_workspace();
    }
}

void test_move_replay_failure_and_cleanup_lifecycle() {
    {
        ArtifactSetFixture fixture({"moved-prepared.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"moved-prepared", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("moved-prepared");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("moved-prepared", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& source = expect_prepared(
            result, "moved prepared source");
        PreparedPackageBaseArtifactInstall owner(std::move(source));

        static_cast<void>(expect_runtime_error(
            [&source]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        source,
                        ArtifactInstallExecutionOptions{}));
            },
            "moved-from prepared executor"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Moved-from prepared object ran pacman -U");
        owner.cleanup_workspace();
    }

    {
        ArtifactSetFixture fixture({"cleaned-prepared.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"cleaned-prepared", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("cleaned-prepared");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("cleaned-prepared", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "cleaned prepared object");
        prepared.cleanup_workspace();
        static_cast<void>(expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "cleaned prepared executor"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Cleaned prepared object ran pacman -U");
        static_cast<void>(expect_runtime_error(
            [&prepared]() { prepared.cleanup_workspace(); },
            "repeated aggregate cleanup"));
    }

    {
        ArtifactSetFixture fixture({"cleanup-retry.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(fixture, {{"cleanup-retry", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("cleanup-retry");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("cleanup-retry", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "cleanup retry preparation");

        const fs::path workspace_path = fixture.workspace_path();
        const fs::path original_workspace =
            fs::path(workspace_path.string() + ".cleanup-original");
        fs::rename(workspace_path, original_workspace);
        fs::create_directory(workspace_path);
        fs::permissions(
            workspace_path, fs::perms::owner_all,
            fs::perm_options::replace);

        static_cast<void>(expect_runtime_error(
            [&prepared]() { prepared.cleanup_workspace(); },
            "prepared cleanup workspace replacement"));
        expect(
            prepared.workspace_path() == workspace_path,
            "Failed prepared cleanup lost its diagnostic workspace path");
        static_cast<void>(expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "cleanup-pending prepared executor"));
        expect(
            process_stub::run_command_call_count() == 0,
            "Cleanup-pending prepared object ran pacman -U");

        expect(
            fs::remove(workspace_path),
            "Replacement cleanup workspace was not removed");
        fs::rename(original_workspace, workspace_path);
        prepared.cleanup_workspace();
        expect(
            !fs::exists(workspace_path),
            "Prepared cleanup retry left the workspace behind");
    }

    {
        ArtifactSetFixture fixture({"command-failure.pkg.tar.zst"});
        reset_stubs();
        expect_identity_queries(
            fixture, {{"command-failure", "1-1"}});
        metadata_stub::enqueue_local_package_query_absent("command-failure");
        PackageBaseArtifactInstallPreparationResult result = prepare_fixture(
            fixture,
            {target("command-failure", DesiredInstallReason::Explicit)});
        PreparedPackageBaseArtifactInstall& prepared = expect_prepared(
            result, "command failure preparation");
        prepared.retain_workspace_for_diagnostics();
        process_stub::expect_run_command(
            expected_install_command({fixture.path_at(0)}, false, false),
            73);
        process_stub::set_run_hook(require_metadata_released_before_run);

        bool transaction_failure_reported = false;
        try {
            static_cast<void>(
                execute_prepared_package_base_artifact_install(
                    prepared,
                    ArtifactInstallExecutionOptions{}));
        } catch(const PackageBaseArtifactInstallTransactionError& error) {
            transaction_failure_reported = true;
            const std::string diagnostic = error.what();
            expect(
                error.failure_kind() ==
                        PackageBaseArtifactInstallTransactionFailureKind::
                            NonzeroExit &&
                    error.package_base() == PACKAGE_BASE &&
                    error.exit_code() == std::optional<int>{73},
                "Nonzero PackageBase transaction typed detail differs");
            expect(
                error.attempts().size() == 1 &&
                    error.attempts()[0].identity.package_name ==
                        "command-failure" &&
                    error.attempts()[0].identity.full_version ==
                        "1-1" &&
                    error.attempts()[0].desired_reason ==
                        DesiredInstallReason::Explicit,
                "Nonzero PackageBase transaction attempt snapshot differs");
            expect(
                diagnostic == "pacman -U failed with exit code 73.",
                "Nonzero PackageBase transaction diagnostic differs");
            expect(
                diagnostic.find(fixture.path_at(0).string()) ==
                    std::string::npos,
                "Nonzero transaction diagnostic exposed an artifact path");
        }
        expect(
            transaction_failure_reported,
            "Nonzero PackageBase transaction did not report typed failure");
        expect(
            process_stub::run_command_call_count() == 1 &&
                prepared.selected_artifacts().size() == 1 &&
                prepared.selected_artifacts()[0]
                        .identity.package_name ==
                    "command-failure" &&
                fs::exists(fixture.workspace_path()),
            "Failed transaction lost diagnostic retention state");

        static_cast<void>(expect_runtime_error(
            [&prepared]() {
                static_cast<void>(
                    execute_prepared_package_base_artifact_install(
                        prepared,
                        ArtifactInstallExecutionOptions{}));
            },
            "failed transaction replay"));
        expect(
            process_stub::run_command_call_count() == 1,
            "Failed transaction replayed pacman -U");
        prepared.cleanup_workspace();
        expect(
            !fs::exists(fixture.workspace_path()),
            "Explicit cleanup after command failure failed");
    }
}

template <typename Callable>
void run_case(const std::string& name, Callable&& callable) {
    std::forward<Callable>(callable)();
    process_stub::require_process_expectations_consumed();
    metadata_stub::require_local_package_query_expectations_consumed();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        TemporaryCacheHome cache_home;
        ScopedReasonPlanObserver reason_plan_observer;
        static_cast<void>(cache_home);
        static_cast<void>(reason_plan_observer);

        run_case(
            "selection order and success ownership",
            test_selection_order_and_success_ownership);
        run_case(
            "selection failure and preparation guards",
            test_selection_failure_and_preparation_guards);
        run_case(
            "selected metadata mapping and order",
            test_selected_metadata_mapping_and_order);
        run_case(
            "metadata failures preserve ownership",
            test_metadata_failures_preserve_ownership);
        run_case(
            "mixed reason typed failure",
            test_mixed_reason_typed_failure);
        run_case(
            "needed outcomes and stronger global directives",
            test_needed_outcomes_and_stronger_global_directives);
        run_case(
            "exact executor argv and typed results",
            test_exact_executor_argv_and_results);
        run_case(
            "executor aggregate revalidation",
            test_executor_revalidation);
        run_case(
            "move replay failure and cleanup lifecycle",
            test_move_replay_failure_and_cleanup_lifecycle);
    } catch(const std::exception& error) {
        std::cerr << "PackageBase artifact install executor test failed: "
                  << error.what() << '\n';
        return 1;
    }

    std::cout << "PackageBase artifact install executor tests: all checks passed\n";
    return 0;
}
