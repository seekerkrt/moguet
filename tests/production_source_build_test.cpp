#include "app_config.hpp"
#include "cache_authority.hpp"
#include "dependency_plan.hpp"
#include "invocation_owned_cleanup_adapter.hpp"
#include "local_dependency_plan_projection.hpp"
#include "local_package_metadata.hpp"
#include "process.hpp"
#include "separated_package_base_source_build.hpp"
#include "separated_source_build.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"
#include "trusted_cache.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"

#include "stubs/artifact-install-executor/process_stub.hpp"
#include "stubs/local-dependency-plan/query_stub.hpp"
#include "stubs/package-metadata/alpm_stub.hpp"

#include <algorithm>
#include <cerrno>
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
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// process.cppをlinkしないfake-symbol binaryでも、production checkout ownerが使う
// trimmed capture APIだけは同じ意味でstrict raw-capture FIFOへ接続する。
CapturedCommandResult capture_command_output(const char* command) {
    CapturedCommandResult result = capture_command_output_raw(command);
    const std::size_t first = result.output.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) {
        result.output.clear();
        return result;
    }
    const std::size_t last = result.output.find_last_not_of(" \t\n\r");
    result.output = result.output.substr(first, last - first + 1);
    return result;
}

static_assert(std::variant_size_v<SourceBuildPreparationOutcome> == 3);
static_assert(std::is_same_v<
              std::variant_alternative_t<0, SourceBuildPreparationOutcome>,
              SourceBuildUpToDate>);
static_assert(std::is_same_v<
              std::variant_alternative_t<1, SourceBuildPreparationOutcome>,
              SourceBuildUpdateStatusUnknownSkipped>);
static_assert(std::is_same_v<
              std::variant_alternative_t<2, SourceBuildPreparationOutcome>,
              PreparedSourceBuildNeedsBuild>);
static_assert(!std::is_copy_constructible_v<PreparedSourceBuildNeedsBuild>);
static_assert(std::is_move_constructible_v<PreparedSourceBuildNeedsBuild>);

std::string exec_command(const char* command) {
    return capture_command_output(command).output;
}

int command_status(const std::string& command) {
    return run_command(command);
}

namespace trusted_receipt_transport_stub {

std::optional<TrustedAlpmReceiptCaptureResult> next_result;
std::optional<TrustedAlpmReceiptSelectedProviderRequest> last_request;
std::size_t call_count = 0;

void set_result(TrustedAlpmReceiptCaptureResult result) {
    next_result = std::move(result);
    last_request.reset();
    call_count = 0;
}

} // namespace trusted_receipt_transport_stub

TrustedAlpmReceiptCaptureResult
execute_trusted_alpm_receipt_selected_provider_transaction(
    const TrustedAlpmReceiptSelectedProviderRequest& request) {
    using namespace trusted_receipt_transport_stub;
    ++call_count;
    last_request = request;
    if(!next_result.has_value()) {
        throw std::logic_error(
            "trusted receipt transport stub has no result");
    }
    TrustedAlpmReceiptCaptureResult result = std::move(next_result.value());
    next_result.reset();
    return result;
}

namespace remote_cleanup_collector_stub {

std::size_t call_count = 0;

void reset() {
    call_count = 0;
}

} // namespace remote_cleanup_collector_stub

RemoteAurCleanupCollectionResult collect_remote_aur_cleanup_candidates(
    PreparedRemoteSourceBuild prepared,
    const AppConfig& config) {
    ++remote_cleanup_collector_stub::call_count;
    ProductionSourceBuildInvocationResult result =
        execute_prepared_source_build_invocation(
            std::move(prepared.invocation), config);
    return RemoteAurCleanupCollectionResult(
        std::move(result), CleanupEvidenceCompleteness::Incomplete,
        {}, {RemoteAurCleanupCollectionIssueKind::CandidateOriginMissing});
}

bool RemoteAurCleanupCandidateCollector::
    should_use_trusted_source_artifact_install(std::size_t) const noexcept {
    return false;
}

SelectedRepositoryProviderTransactionResult
RemoteAurCleanupCandidateCollector::
    execute_selected_repository_provider_transaction(const AppConfig&) {
    throw std::logic_error(
        "production-source-build collector stub was used as an executor");
}

PackageBaseArtifactInstallExecutionResult
RemoteAurCleanupCandidateCollector::
    execute_source_artifact_install_transaction(
        PreparedPackageBaseArtifactInstall&,
        std::size_t,
        const ArtifactInstallExecutionOptions&) {
    throw std::logic_error(
        "production-source-build collector stub was used as a source-artifact executor");
}

namespace {

namespace fs = std::filesystem;
namespace metadata_stub = package_metadata_test_stub;
namespace query_stub = local_dependency_plan_query_stub;
namespace process_stub = artifact_install_executor_test_stub;

constexpr const char* PACMAN_DATABASE_PATH_COMMAND =
    "pacman-conf --verbose RootDir DBPath 2>/dev/null";
constexpr const char* GIT_REMOTE_COMMAND =
    "git config --get remote.origin.url";
constexpr const char* GIT_BRANCH_COMMAND =
    "git symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null";
constexpr const char* ARTIFACT_VERSION = "1.0-1";
constexpr std::string_view LOCAL_REPOSITORY_PROVIDER_SRCINFO =
    "pkgbase = local-provider-suite\n"
    "\tpkgver = 1.0\n"
    "\tpkgrel = 1\n"
    "\tarch = x86_64\n"
    "pkgname = local-provider-root\n"
    "\tdepends = virtual-local-api\n"
    "\tdepends = virtual-local-api-alias\n";

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

template <typename Callable>
std::string expect_runtime_error(
    Callable&& callable, const std::string& context,
    const std::string& expected_fragment) {
    try {
        std::forward<Callable>(callable)();
    } catch(const SeparatedSourceBuildCleanupError& error) {
        throw std::runtime_error(
            context + ": cleanup partial-success was not expected: " +
            error.what());
    } catch(const std::runtime_error& error) {
        if(std::string(error.what()).find(expected_fragment) ==
           std::string::npos) {
            throw std::runtime_error(
                context + ": unexpected diagnostic [" + error.what() +
                "]");
        }
        return error.what();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " +
            error.what());
    }
    throw std::runtime_error(context + ": expected runtime_error");
}

template <typename Callable>
ProductionSourceBuildInvocationError expect_invocation_error(
    Callable&& callable, const std::string& context,
    const std::string& expected_fragment) {
    try {
        std::forward<Callable>(callable)();
    } catch(const ProductionSourceBuildInvocationError& error) {
        if(std::string(error.what()).find(expected_fragment) ==
           std::string::npos) {
            throw std::runtime_error(
                context + ": unexpected diagnostic [" + error.what() +
                "]");
        }
        return error;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": invocation aggregate was lost: " +
            error.what());
    }
    throw std::runtime_error(
        context + ": expected ProductionSourceBuildInvocationError");
}

struct CleanupErrorObservation {
    ArtifactInstallExecutionOutcome install_outcome;
    ProductionSourceBuildStagedOutcome production_outcome;
    std::string diagnostic;
};

template <typename Callable>
CleanupErrorObservation expect_cleanup_error(
    Callable&& callable, const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const ProductionSourceBuildInvocationError& error) {
        const ProductionSourceBuildWorkItemOutcome& failed =
            error.result().work_items.at(
                error.failed_work_item_index());
        if(!failed.production_outcome.has_value()) {
            throw std::runtime_error(
                context + ": cleanup aggregate lost staged outcome");
        }
        try {
            error.rethrow_failure();
        } catch(const SeparatedSourceBuildCleanupError& cleanup) {
            return CleanupErrorObservation{
                cleanup.install_outcome(),
                *failed.production_outcome, cleanup.what()};
        }
        throw std::runtime_error(
            context + ": cleanup aggregate lost typed cause");
    } catch(const SeparatedSourceBuildCleanupError& error) {
        return CleanupErrorObservation{
            error.install_outcome(), error.production_outcome(),
            error.what()};
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": cleanup partial-success was flattened: " +
            error.what());
    }
    throw std::runtime_error(
        context + ": expected SeparatedSourceBuildCleanupError");
}

template <typename Callable>
TrustedCacheFailure expect_trusted_cache_error(
    Callable&& callable, const std::string& context) {
    try {
        std::forward<Callable>(callable)();
    } catch(const TrustedCacheError& error) {
        return error.failure();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": trusted cache failure was flattened: " +
            error.what());
    }
    throw std::runtime_error(context + ": expected TrustedCacheError");
}

template <typename Callable>
std::string expect_logic_error(
    Callable&& callable, const std::string& context,
    const std::string& expected_fragment) {
    try {
        std::forward<Callable>(callable)();
    } catch(const std::logic_error& error) {
        if(std::string(error.what()).find(expected_fragment) ==
           std::string::npos) {
            throw std::runtime_error(
                context + ": unexpected diagnostic [" + error.what() +
                "]");
        }
        return error.what();
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " +
            error.what());
    }
    throw std::runtime_error(context + ": expected logic_error");
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error(
            "Failed to create production source-build fixture: " +
            path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error(
            "Failed to finish production source-build fixture: " +
            path.string());
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if(!input) {
        throw std::runtime_error(
            "Failed to open production source-build fixture: " +
            path.string());
    }
    std::string contents(
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{});
    if(input.bad()) {
        throw std::runtime_error(
            "Failed to read production source-build fixture: " +
            path.string());
    }
    return contents;
}

struct CacheTreeEntry {
    std::string relative_path;
    fs::file_type type = fs::file_type::unknown;
    std::string payload;

    bool operator==(const CacheTreeEntry&) const = default;
};

using CacheTreeSnapshot = std::vector<CacheTreeEntry>;

CacheTreeSnapshot snapshot_cache_tree(const fs::path& cache_root_path) {
    CacheTreeSnapshot snapshot;
    for(const fs::directory_entry& entry :
        fs::recursive_directory_iterator(cache_root_path)) {
        const fs::file_status status = entry.symlink_status();
        std::string payload;
        if(fs::is_regular_file(status)) {
            payload = read_file(entry.path());
        } else if(fs::is_symlink(status)) {
            payload = fs::read_symlink(entry.path()).generic_string();
        }
        snapshot.push_back(CacheTreeEntry{
            entry.path()
                .lexically_relative(cache_root_path)
                .generic_string(),
            status.type(),
            std::move(payload)});
    }
    std::sort(
        snapshot.begin(), snapshot.end(),
        [](const CacheTreeEntry& left, const CacheTreeEntry& right) {
            return left.relative_path < right.relative_path;
        });
    return snapshot;
}

class ScopedEnvironmentVariable final {
    std::string key_;
    std::optional<std::string> previous_value_;

public:
    ScopedEnvironmentVariable(
        std::string key, const std::optional<std::string>& value)
        : key_(std::move(key)) {
        const char* previous = std::getenv(key_.c_str());
        if(previous != nullptr) previous_value_ = previous;

        const int result = value.has_value()
                               ? setenv(
                                     key_.c_str(),
                                     value->c_str(), 1)
                               : unsetenv(key_.c_str());
        if(result != 0) {
            throw std::runtime_error(
                "Failed to set test environment variable: " + key_);
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() noexcept {
        if(previous_value_.has_value()) {
            static_cast<void>(setenv(
                key_.c_str(), previous_value_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv(key_.c_str()));
        }
    }
};

class ScopedUmask final {
    mode_t previous_mode_;

public:
    explicit ScopedUmask(mode_t mode) noexcept : previous_mode_(umask(mode)) {
    }

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

    ~ScopedUmask() noexcept {
        static_cast<void>(umask(previous_mode_));
    }
};

class ScopedSourcePreferenceEntry final {
    fs::path root_;
    fs::path entry_;

public:
    ScopedSourcePreferenceEntry(
        const std::string& package_name,
        const std::string& contents)
        : root_(source_preference_root()),
          entry_(root_ / package_name) {
        fs::create_directories(root_);
        fs::permissions(
            root_.parent_path(), fs::perms::owner_all,
            fs::perm_options::replace);
        fs::permissions(
            root_, fs::perms::owner_all,
            fs::perm_options::replace);
        write_file(entry_, contents);
        fs::permissions(
            entry_,
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);
    }

    ScopedSourcePreferenceEntry(const ScopedSourcePreferenceEntry&) = delete;
    ScopedSourcePreferenceEntry& operator=(
        const ScopedSourcePreferenceEntry&) = delete;

    ~ScopedSourcePreferenceEntry() noexcept {
        std::error_code error;
        fs::remove(entry_, error);
        error.clear();
        // rootそのものはemptyの場合だけ除去し、広いpathをrecursiveに消さない。
        fs::remove(root_, error);
    }
};

class ScopedStdinReplacement final {
    int saved_stdin_ = -1;
    int replacement_fd_ = -1;
    int peer_fd_ = -1;

    ScopedStdinReplacement(
        int replacement_fd, int peer_fd)
        : replacement_fd_(replacement_fd), peer_fd_(peer_fd) {
        saved_stdin_ =
            fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if(saved_stdin_ == -1 || dup2(replacement_fd_, STDIN_FILENO) == -1) {
            const int saved_error = errno;
            if(saved_stdin_ != -1) close(saved_stdin_);
            close(replacement_fd_);
            if(peer_fd_ != -1) close(peer_fd_);
            throw std::runtime_error(
                "Failed to replace stdin for production source-build test: " +
                std::to_string(saved_error));
        }
    }

public:
    static ScopedStdinReplacement noninteractive() {
        int descriptors[2];
        if(pipe2(descriptors, O_CLOEXEC) != 0) {
            throw std::runtime_error(
                "Failed to create non-interactive stdin fixture.");
        }
        close(descriptors[1]);
        return ScopedStdinReplacement(descriptors[0], -1);
    }

    static ScopedStdinReplacement terminal_with_input(
        const std::string& input) {
        const int master_fd = posix_openpt(O_RDWR | O_NOCTTY);
        if(master_fd == -1 || grantpt(master_fd) != 0 ||
           unlockpt(master_fd) != 0) {
            if(master_fd != -1) close(master_fd);
            throw std::runtime_error(
                "Failed to create terminal stdin fixture.");
        }
        const char* slave_name = ptsname(master_fd);
        if(slave_name == nullptr) {
            close(master_fd);
            throw std::runtime_error(
                "Failed to resolve terminal stdin fixture path.");
        }
        const int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
        if(slave_fd == -1) {
            close(master_fd);
            throw std::runtime_error(
                "Failed to open terminal stdin fixture.");
        }
        if(write(
               master_fd, input.data(),
               static_cast<size_t>(input.size())) !=
           static_cast<ssize_t>(input.size())) {
            close(slave_fd);
            close(master_fd);
            throw std::runtime_error(
                "Failed to seed terminal stdin fixture.");
        }
        return ScopedStdinReplacement(slave_fd, master_fd);
    }

    ScopedStdinReplacement(const ScopedStdinReplacement&) = delete;
    ScopedStdinReplacement& operator=(const ScopedStdinReplacement&) = delete;
    ScopedStdinReplacement(ScopedStdinReplacement&& other) noexcept
        : saved_stdin_(std::exchange(other.saved_stdin_, -1)),
          replacement_fd_(std::exchange(other.replacement_fd_, -1)),
          peer_fd_(std::exchange(other.peer_fd_, -1)) {
    }
    ScopedStdinReplacement& operator=(ScopedStdinReplacement&&) = delete;

    ~ScopedStdinReplacement() noexcept {
        if(saved_stdin_ != -1) {
            static_cast<void>(dup2(saved_stdin_, STDIN_FILENO));
            close(saved_stdin_);
        }
        if(replacement_fd_ != -1) close(replacement_fd_);
        if(peer_fd_ != -1) close(peer_fd_);
    }
};

class TemporaryProductionEnvironment final {
    fs::path original_working_directory_;
    fs::path path_;
    fs::path cache_root_path_;
    std::optional<std::string> previous_xdg_cache_home_;
    std::optional<std::string> previous_xdg_state_home_;
    std::optional<std::string> previous_pkgdest_;

public:
    explicit TemporaryProductionEnvironment(bool prepare_cache = true)
        : original_working_directory_(fs::current_path()) {
        const std::string template_text =
            (fs::temp_directory_path() /
             "moguet-production-source-build-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create production source-build test directory.");
        }
        path_ = created_path;

        const char* previous_xdg = std::getenv("XDG_CACHE_HOME");
        if(previous_xdg != nullptr) previous_xdg_cache_home_ = previous_xdg;
        const char* previous_xdg_state = std::getenv("XDG_STATE_HOME");
        if(previous_xdg_state != nullptr) {
            previous_xdg_state_home_ = previous_xdg_state;
        }
        const char* previous_pkgdest = std::getenv("PKGDEST");
        if(previous_pkgdest != nullptr) previous_pkgdest_ = previous_pkgdest;

        const fs::path state_home = path_ / "state";
        fs::create_directory(state_home);
        fs::permissions(
            state_home, fs::perms::owner_all,
            fs::perm_options::replace);
        if(setenv("XDG_CACHE_HOME", path_.c_str(), 1) != 0 ||
           setenv("XDG_STATE_HOME", state_home.c_str(), 1) != 0 ||
           unsetenv("PKGDEST") != 0) {
            restore_environment();
            throw std::runtime_error(
                "Failed to prepare production source-build test environment.");
        }

        try {
            xdg_paths::CachePaths cache_paths =
                xdg_paths::resolve_cache_process_environment();
            cache_root_path_ = cache_paths.directory;
            if(!prepare_cache) return;
            xdg_directory_safety::PreparedDirectory cache_directory =
                xdg_directory_safety::prepare_directory(cache_paths);
            ValidatedCacheRoot root = adopt_trusted_cache_root(
                cache_paths, std::move(cache_directory));
            cache_root_path_ = root.canonical_path();

            // POLICY: preflight snapshotはdirect entryだけでなく、file内容と
            // symlink targetの変化も検出できるfixtureを常に含める。
            const fs::path snapshot_fixture =
                cache_root_path_ / "preflight-snapshot-fixture";
            fs::create_directory(snapshot_fixture);
            fs::permissions(
                snapshot_fixture, fs::perms::owner_all,
                fs::perm_options::replace);
            write_file(
                snapshot_fixture / "state.txt",
                "stable preflight fixture\n");
            fs::create_symlink(
                "state.txt", snapshot_fixture / "state-link");
        } catch(...) {
            restore_environment();
            std::error_code error;
            fs::remove_all(path_, error);
            throw;
        }
    }

    TemporaryProductionEnvironment(
        const TemporaryProductionEnvironment&) = delete;
    TemporaryProductionEnvironment& operator=(
        const TemporaryProductionEnvironment&) = delete;

    ~TemporaryProductionEnvironment() noexcept {
        set_separated_source_build_workspace_observer_for_test(nullptr);
        set_separated_package_base_source_build_workspace_observer_for_test(
            nullptr);
        process_stub::set_capture_hook(nullptr);
        process_stub::set_run_hook(nullptr);
        std::error_code working_directory_error;
        fs::current_path(
            original_working_directory_, working_directory_error);
        restore_environment();
        std::error_code error;
        fs::remove_all(path_, error);
    }

    fs::path ensure_checkout(
        const std::string& package_name,
        const std::string& git_url) const {
        fs::path checkout_path = cache_root_path_ / package_name;
        if(!fs::exists(checkout_path)) {
            fs::create_directory(checkout_path);
            fs::permissions(
                checkout_path, fs::perms::owner_all,
                fs::perm_options::replace);
            fs::create_directory(checkout_path / ".git");
            fs::permissions(
                checkout_path / ".git", fs::perms::owner_all,
                fs::perm_options::replace);
            write_file(
                checkout_path / "PKGBUILD",
                "# production source-build fixture for " + package_name +
                    "\n# remote: " + git_url + "\n");
            fs::permissions(
                checkout_path / "PKGBUILD",
                fs::perms::owner_read | fs::perms::owner_write,
                fs::perm_options::replace);
        }
        return fs::canonical(checkout_path);
    }

    std::vector<fs::path> artifact_workspaces() const {
        std::vector<fs::path> workspaces;
        if(!fs::exists(cache_root_path_)) return workspaces;
        for(const fs::directory_entry& entry :
            fs::directory_iterator(cache_root_path_)) {
            if(entry.path().filename().string().starts_with(
                   ".artifact-workspace~-")) {
                workspaces.push_back(entry.path());
            }
        }
        std::sort(workspaces.begin(), workspaces.end());
        return workspaces;
    }

    CacheTreeSnapshot cache_tree_snapshot() const {
        return snapshot_cache_tree(cache_root_path_);
    }

    fs::path checkout_target_path(const std::string& package_name) const {
        return cache_root_path_ / package_name;
    }

    const fs::path& original_working_directory() const noexcept {
        return original_working_directory_;
    }

private:
    void restore_environment() noexcept {
        if(previous_xdg_cache_home_.has_value()) {
            static_cast<void>(setenv(
                "XDG_CACHE_HOME",
                previous_xdg_cache_home_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("XDG_CACHE_HOME"));
        }
        if(previous_xdg_state_home_.has_value()) {
            static_cast<void>(setenv(
                "XDG_STATE_HOME",
                previous_xdg_state_home_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv("XDG_STATE_HOME"));
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

ProductionSourceBuildWorkItem make_work_item(
    const std::string& package_name,
    DesiredInstallReason desired_reason =
        DesiredInstallReason::Explicit) {
    ProductionSourceBuildWorkItem work_item;
    work_item.request.package_name = package_name;
    work_item.request.checkout_name = package_name;
    work_item.request.git_url =
        "https://aur.archlinux.org/" + package_name + ".git";
    work_item.request.aur_review_identity = PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                work_item.request.git_url)),
        package_name);
    work_item.required_targets.push_back(RequiredPackageArtifactTarget{
        package_name, package_name, desired_reason});
    work_item.required_target_provenance =
        RequiredTargetProvenance::AurBuildPlanProjection;
    work_item.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility;
    return work_item;
}

ProductionSourceBuildWorkItem make_package_base_work_item(
    const std::string& package_base,
    std::vector<RequiredPackageArtifactTarget> required_targets) {
    ProductionSourceBuildWorkItem work_item;
    if(required_targets.size() == 1) {
        work_item.request.package_name =
            required_targets.front().package_name;
    }
    work_item.request.checkout_name = package_base;
    work_item.request.git_url =
        "https://aur.archlinux.org/" + package_base + ".git";
    work_item.request.aur_review_identity = PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                work_item.request.git_url)),
        package_base);
    work_item.required_targets = std::move(required_targets);
    work_item.required_target_provenance =
        RequiredTargetProvenance::AurBuildPlanProjection;
    work_item.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    return work_item;
}

ProductionSourceBuildWorkItem make_repository_package_base_work_item(
    const std::string& package_base,
    const std::string& package_name,
    SourceBuildEnvironment environment = {}) {
    const RepositoryPackagePresent exact{
        "extra", 0, package_name, package_base,
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage,
            ARTIFACT_VERSION),
        std::vector<std::string>{"extra"}};
    ProductionSourceBuildWorkItem work_item =
        prepare_resolved_source_build_work_item(
            make_repository_source_build_identity(exact),
            std::move(environment), false, false);
    work_item.request.empty_value_policy =
        SourceEnvironmentEmptyValuePolicy::Forward;
    work_item.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    require_static_production_source_build_work_item(work_item);
    return work_item;
}

ProductionSourceBuildWorkItem
make_registered_repository_package_base_work_item(
    const std::string& package_base,
    const std::string& package_name) {
    const RepositoryPackagePresent exact{
        "extra", 0, package_name, package_base,
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage,
            ARTIFACT_VERSION),
        std::vector<std::string>{"extra"}};
    return prepare_registered_source_build_work_item(
        make_repository_source_build_identity(exact),
        SourceBuildEnvironment{}, ProviderSelectionCallback{});
}

ProductionSourceBuildWorkItem make_update_check_work_item(
    const std::string& package_name,
    const std::string& installed_version) {
    ProductionSourceBuildWorkItem work_item = make_work_item(package_name);
    work_item.request.only_if_updated = true;
    work_item.request.installed_snapshot =
        SourceInstalledSnapshot{installed_version};
    return work_item;
}

BuildPlan two_entry_plan() {
    BuildPlan plan;
    const RootTargetIdentity root_identity{0, "root-package"};
    plan.root_targets.push_back(root_identity);
    plan.package_targets.push_back(PlannedPackageTarget{
        "dependency-package", "dependency-package", {PackageRole::RuntimeDependency}, {root_identity}});
    // Explicit優先はproduction側で再実装せず、BuildPlan helperの確定値を使う。
    plan.package_targets.push_back(PlannedPackageTarget{
        "root-package", "root-package", {PackageRole::RuntimeDependency, PackageRole::Root}, {root_identity}});
    plan.order.push_back(
        BuildPlanEntry{"dependency-package", {"dependency-package"}});
    plan.order.push_back(
        BuildPlanEntry{"root-package", {"root-package"}});
    return plan;
}

BuildPlan ordinary_single_entry_plan() {
    BuildPlan plan;
    const RootTargetIdentity root_identity{0, "ordinary-set-root"};
    plan.root_targets.push_back(root_identity);
    plan.package_targets.push_back(PlannedPackageTarget{
        "ordinary-set-root", "ordinary-set-root", {PackageRole::Root}, {root_identity}});
    plan.order.push_back(BuildPlanEntry{
        "ordinary-set-root", {"ordinary-set-root"}});
    return plan;
}

ProvidedDependency make_repository_provider(
    const std::string& repository_name,
    const std::string& package_name,
    const std::string& provided_dependency_name,
    const std::string& provided_dependency_specification,
    const std::optional<std::string>& package_version,
    const std::string& package_base = "repository-provider-base",
    const std::string& package_architecture = "x86_64",
    std::optional<std::size_t> configured_order = std::nullopt) {
    const ProviderCapabilityParseResult capability_parse =
        parse_provider_capability(provided_dependency_specification);
    const ProviderCapability* const parsed_capability =
        capability_parse.capability();
    expect(
        parsed_capability != nullptr &&
            parsed_capability->package_name() ==
                provided_dependency_name,
        "Failed to parse typed repository provider capability fixture: " +
            provided_dependency_specification);
    const ObservedVersion provided_version =
        parsed_capability->version().has_value()
            ? ObservedVersion::available(
                  ObservedVersionSource::RepositoryProviderCapability,
                  parsed_capability->version().value())
            : ObservedVersion::unknown(
                  ObservedVersionSource::RepositoryProviderCapability,
                  ObservedVersionUnknownReason::
                      UnversionedProviderCapability);
    ProviderConstraintMetadata metadata{
        *parsed_capability,
        package_version.has_value()
            ? ObservedVersion::available(
                  ObservedVersionSource::RepositoryExactPackage,
                  package_version.value())
            : ObservedVersion::unknown(
                  ObservedVersionSource::RepositoryExactPackage,
                  ObservedVersionUnknownReason::MissingVersionMetadata),
        provided_version};
    if(configured_order.has_value()) {
        return ProvidedDependency::from_repository_constraint_metadata(
            repository_name, configured_order.value(), package_name,
            package_base, package_architecture, std::move(metadata));
    }
    return ProvidedDependency::from_repository_constraint_metadata(
        repository_name, package_name, package_base, package_architecture,
        std::move(metadata));
}

BuildPlanDependencyEdge make_repository_provider_edge(
    const std::string& parent_package_name,
    const std::string& parent_package_base,
    const ProvidedDependency& provider,
    ProviderResolutionKind resolution) {
    BuildPlanDependencyEdge edge;
    edge.parent_package_name = parent_package_name;
    edge.parent_package_base = parent_package_base;
    edge.dependency_spec = provider.provided_dependency_specification;
    edge.role = PackageRole::RuntimeDependency;
    edge.kind = DependencyKind::Provided;
    edge.resolved_provider = provider;
    edge.provider_resolution = resolution;
    return edge;
}

BuildPlan single_repository_provider_plan(
    const ProvidedDependency& provider,
    ProviderResolutionKind resolution) {
    BuildPlan plan = ordinary_single_entry_plan();
    plan.dependency_edges.push_back(make_repository_provider_edge(
        "ordinary-set-root", "ordinary-set-root", provider,
        resolution));
    plan.provided.push_back(BuildPlanProvidedDependency{
        provider.provided_dependency_specification, provider,
        resolution});
    return plan;
}

BuildPlan same_package_base_plan() {
    BuildPlan plan;
    const RootTargetIdentity root_identity{0, "split-explicit"};
    plan.root_targets.push_back(root_identity);
    plan.package_targets.push_back(PlannedPackageTarget{
        "split-explicit", "split-suite", {PackageRole::RuntimeDependency, PackageRole::Root}, {root_identity}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "split-dependency", "split-suite", {PackageRole::RuntimeDependency}, {root_identity}});
    plan.order.push_back(BuildPlanEntry{
        "split-suite", {"split-explicit", "split-dependency"}});
    return plan;
}

void expect_required_target(
    const RequiredPackageArtifactTarget& target,
    const std::string& package_base,
    const std::string& package_name,
    DesiredInstallReason desired_reason,
    const std::string& context) {
    expect(target.package_base == package_base, context + ": PackageBase differs");
    expect(target.package_name == package_name, context + ": package name differs");
    expect(
        target.desired_reason == desired_reason,
        context + ": desired install reason differs");
}

AppConfig noninteractive_config() {
    AppConfig config;
    config.user_config.review.pkgbuild = ReviewPolicy::Skip;
    config.user_config.review.diff = ReviewPolicy::Skip;
    return config;
}

enum class MetadataMode {
    Absent,
    ExistingDependency,
    ExistingExplicitDifferentVersion,
    ExistingExplicitSameVersion,
    QueryFailure,
};

struct ProducedArtifactPlan {
    std::string package_name;
    std::string full_version = ARTIFACT_VERSION;
    std::optional<std::string> archive_package_name = std::nullopt;
};

const std::string& archive_package_name(
    const ProducedArtifactPlan& artifact) {
    return artifact.archive_package_name.has_value()
               ? artifact.archive_package_name.value()
               : artifact.package_name;
}

struct UnitPlan {
    std::string package_name;
    std::string package_base;
    std::string git_url;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    std::vector<ProducedArtifactPlan> produced_artifacts;
    SourceBuildEnvironment source_environment;
    SourceEnvironmentEmptyValuePolicy empty_value_policy =
        SourceEnvironmentEmptyValuePolicy::Omit;
    bool needed = false;
    fs::path checkout_path;

    int build_exit_code = 0;
    bool expect_identity = true;
    MetadataMode metadata_mode = MetadataMode::Absent;
    bool expect_install = true;
    int install_exit_code = 0;
    const char* install_reason_option = nullptr;
    bool replace_workspace_after_build = false;
    bool replace_workspace_after_install = false;
};

struct ProductionScenario {
    AppConfig config;
    fs::path caller_working_directory;
    std::vector<UnitPlan> units;

    std::vector<fs::path> workspace_paths;
    std::vector<fs::path> artifact_paths;
    std::vector<std::vector<fs::path>> produced_artifact_paths;
    std::vector<fs::path> displaced_workspace_paths;
    std::vector<std::string> install_attempt_order;

    std::size_t active_unit = 0;
    std::size_t resolver_calls = 0;
    std::size_t git_remote_calls = 0;
    std::size_t git_fetch_calls = 0;
    std::size_t git_branch_calls = 0;
    std::size_t git_reset_calls = 0;
    std::size_t packagelist_calls = 0;
    std::size_t build_calls = 0;
    std::size_t identity_calls = 0;
    std::size_t install_calls = 0;
    std::size_t repository_provider_query_calls = 0;
    std::size_t repository_provider_install_calls = 0;
};

ProductionScenario* g_scenario = nullptr;

std::string source_environment_prefix(
    const UnitPlan& unit, const fs::path& workspace_path) {
    std::string prefix;
    for(const SourceEnvironmentAssignment& assignment :
        unit.source_environment.ordered_assignments) {
        if(assignment.value.empty() &&
           unit.empty_value_policy == SourceEnvironmentEmptyValuePolicy::Omit) {
            continue;
        }
        prefix += assignment.key + "=" + shell_quote(assignment.value) + " ";
    }
    prefix += "PKGDEST=" + shell_quote(workspace_path.string()) + " ";
    return prefix;
}

std::vector<std::string> source_environment_assignment_words(
    const UnitPlan& unit, const fs::path& workspace_path) {
    std::vector<std::string> words;
    for(const SourceEnvironmentAssignment& assignment :
        unit.source_environment.ordered_assignments) {
        if(assignment.value.empty() &&
           unit.empty_value_policy == SourceEnvironmentEmptyValuePolicy::Omit) {
            continue;
        }
        words.push_back(assignment.key + "=" + assignment.value);
    }
    words.push_back("PKGDEST=" + workspace_path.string());
    return words;
}

std::string expected_packagelist_command(
    const UnitPlan& unit, const fs::path& workspace_path) {
    std::vector<std::string> arguments{"makepkg", "--packagelist"};
    const std::vector<std::string> assignment_words =
        source_environment_assignment_words(unit, workspace_path);
    arguments.insert(
        arguments.end(), assignment_words.begin(),
        assignment_words.end());
    return source_environment_prefix(unit, workspace_path) +
           shell_join(arguments);
}

std::string expected_build_command(
    const ProductionScenario& scenario, const UnitPlan& unit,
    const fs::path& workspace_path) {
    std::vector<std::string> arguments{"makepkg", "-sc"};
    if(scenario.config.no_confirm) arguments.emplace_back("--noconfirm");
    if(scenario.config.user_config.build.mode == BuildMode::Rebuild) {
        arguments.emplace_back("-f");
    }
    if(scenario.config.user_config.build.mode == BuildMode::Clean) {
        arguments.emplace_back("-C");
    }
    const std::vector<std::string> assignment_words =
        source_environment_assignment_words(unit, workspace_path);
    arguments.insert(
        arguments.end(), assignment_words.begin(),
        assignment_words.end());
    return source_environment_prefix(unit, workspace_path) +
           shell_join(arguments);
}

std::string expected_identity_command(const fs::path& artifact_path) {
    return "LC_ALL=C " + shell_join(
                             {"pacman", "-Qp", "--color", "never", "--",
                              artifact_path.string()});
}

std::vector<fs::path> produced_artifact_paths(
    const UnitPlan& unit, const fs::path& workspace_path) {
    std::vector<fs::path> paths;
    paths.reserve(unit.produced_artifacts.size());
    for(const ProducedArtifactPlan& artifact : unit.produced_artifacts) {
        paths.push_back(
            workspace_path /
            (artifact.package_name + "-" + artifact.full_version +
             "-x86_64.pkg.tar.zst"));
    }
    return paths;
}

std::vector<fs::path> selected_artifact_paths(
    const UnitPlan& unit,
    const std::vector<fs::path>& produced_paths) {
    expect(
        produced_paths.size() == unit.produced_artifacts.size(),
        "Produced artifact fixture path count differs");

    std::vector<fs::path> selected_paths;
    selected_paths.reserve(unit.required_targets.size());
    for(const RequiredPackageArtifactTarget& target : unit.required_targets) {
        std::optional<std::size_t> selected_index;
        for(std::size_t index = 0;
            index < unit.produced_artifacts.size(); ++index) {
            if(archive_package_name(unit.produced_artifacts[index]) !=
               target.package_name) {
                continue;
            }
            expect(
                !selected_index.has_value(),
                "Produced artifact fixture contains a duplicate selected identity");
            selected_index = index;
        }
        expect(
            selected_index.has_value(),
            "Produced artifact fixture omitted a required identity");
        selected_paths.push_back(produced_paths[*selected_index]);
    }
    return selected_paths;
}

std::string expected_install_command(
    const ProductionScenario& scenario, const UnitPlan& unit,
    const std::vector<fs::path>& artifact_paths) {
    std::vector<std::string> arguments{"sudo", "pacman", "-U"};
    if(scenario.config.no_confirm) arguments.emplace_back("--noconfirm");
    if(unit.needed) arguments.emplace_back("--needed");
    if(unit.install_reason_option != nullptr) {
        arguments.emplace_back(unit.install_reason_option);
    }
    arguments.emplace_back("--");
    for(const fs::path& artifact_path : artifact_paths) {
        arguments.push_back(artifact_path.string());
    }
    return shell_join(arguments);
}

std::string expected_repository_provider_install_command(
    const std::vector<ProvidedDependency>& providers,
    const AppConfig& config,
    bool as_dependencies = true) {
    std::vector<std::string> arguments{"sudo", "pacman", "-S"};
    if(as_dependencies) arguments.emplace_back("--asdeps");
    arguments.emplace_back("--needed");
    if(config.no_confirm) arguments.emplace_back("--noconfirm");
    arguments.emplace_back("--");
    for(const ProvidedDependency& provider : providers) {
        const auto& repository =
            std::get<RepositoryProviderOrigin>(provider.origin);
        arguments.push_back(
            repository.repository_name + "/" + provider.package_name);
    }
    return shell_join(arguments);
}

void expect_database_paths(int exit_code = 0) {
    process_stub::expect_capture_command(
        PACMAN_DATABASE_PATH_COMMAND,
        CapturedCommandResult{
            "RootDir = /\nDBPath = /var/lib/pacman\n", exit_code});
}

void expect_version_comparison(
    const std::string& candidate_version,
    const std::string& installed_version,
    const std::string& result) {
    process_stub::expect_capture_command(
        "vercmp " + shell_quote(candidate_version) + " " +
            shell_quote(installed_version) + " 2>/dev/null",
        CapturedCommandResult{result + "\n", 0});
}

void schedule_source_unit(std::size_t unit_index) {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Source unit was scheduled outside an active scenario.");
    }
    ProductionScenario& scenario = *g_scenario;
    if(unit_index >= scenario.units.size()) {
        throw std::logic_error("Scheduled source unit is out of range.");
    }
    scenario.active_unit = unit_index;
    const UnitPlan& unit = scenario.units[unit_index];
    process_stub::expect_capture_command(
        GIT_REMOTE_COMMAND,
        CapturedCommandResult{unit.git_url + "\n", 0});
    process_stub::expect_run_command("git fetch origin", 0);
    process_stub::expect_capture_command(
        GIT_BRANCH_COMMAND,
        CapturedCommandResult{"origin/main\n", 0});
    process_stub::expect_run_command(
        "git reset --hard 'origin/main'", 0);
}

void configure_metadata(const UnitPlan& unit) {
    switch(unit.metadata_mode) {
        case MetadataMode::Absent:
            metadata_stub::set_package_absent();
            return;
        case MetadataMode::ExistingDependency:
            metadata_stub::set_package_metadata(
                unit.package_name, "0.9-1", ALPM_PKG_REASON_DEPEND);
            return;
        case MetadataMode::ExistingExplicitDifferentVersion:
            metadata_stub::set_package_metadata(
                unit.package_name, "0.9-1", ALPM_PKG_REASON_EXPLICIT);
            return;
        case MetadataMode::ExistingExplicitSameVersion:
            metadata_stub::set_package_metadata(
                unit.package_name, ARTIFACT_VERSION,
                ALPM_PKG_REASON_EXPLICIT);
            return;
        case MetadataMode::QueryFailure:
            metadata_stub::set_package_query_failure(ALPM_ERR_DB_OPEN);
            return;
    }
    throw std::logic_error("Unknown production metadata fixture mode.");
}

void observe_workspace(const fs::path& workspace_path) {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Production lifecycle created a workspace outside an active scenario.");
    }
    ProductionScenario& scenario = *g_scenario;
    const std::size_t unit_index = scenario.workspace_paths.size();
    if(unit_index >= scenario.units.size() ||
       unit_index != scenario.active_unit) {
        throw std::logic_error(
            "Production lifecycle violated the scheduled BuildPlan order.");
    }
    expect(
        scenario.git_reset_calls == unit_index + 1,
        "Workspace was created before checkout preparation completed");

    const UnitPlan& unit = scenario.units[unit_index];
    const std::vector<fs::path> artifact_paths =
        produced_artifact_paths(unit, workspace_path);
    expect(
        !artifact_paths.empty(),
        "Production fixture has no produced artifact");
    scenario.workspace_paths.push_back(workspace_path);
    scenario.artifact_paths.push_back(artifact_paths.front());
    scenario.produced_artifact_paths.push_back(artifact_paths);
    scenario.displaced_workspace_paths.emplace_back();
    configure_metadata(unit);

    std::string packagelist_output;
    for(const fs::path& artifact_path : artifact_paths) {
        packagelist_output += artifact_path.string() + "\n";
    }
    process_stub::expect_capture_command(
        expected_packagelist_command(unit, workspace_path),
        CapturedCommandResult{std::move(packagelist_output), 0});
    process_stub::expect_run_command(
        expected_build_command(scenario, unit, workspace_path),
        unit.build_exit_code);
    if(unit.expect_identity) {
        for(std::size_t index = 0;
            index < unit.produced_artifacts.size(); ++index) {
            const ProducedArtifactPlan& artifact =
                unit.produced_artifacts[index];
            process_stub::expect_capture_command(
                expected_identity_command(artifact_paths[index]),
                CapturedCommandResult{
                    archive_package_name(artifact) + " " +
                        artifact.full_version + "\n",
                    0});
        }
    }
    if(unit.expect_install) {
        const std::vector<fs::path> selected_paths =
            selected_artifact_paths(unit, artifact_paths);
        process_stub::expect_run_command(
            expected_install_command(scenario, unit, selected_paths),
            unit.install_exit_code);
    }
}

void observe_capture_command() {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Process capture occurred outside an active production scenario.");
    }
    ProductionScenario& scenario = *g_scenario;
    const std::string command = process_stub::last_captured_command();
    if(command == PACMAN_DATABASE_PATH_COMMAND) {
        ++scenario.resolver_calls;
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Database path resolver changed the caller working directory");
        expect(
            scenario.workspace_paths.empty(),
            "Database paths were resolved after workspace creation");
        return;
    }

    const UnitPlan& unit = scenario.units.at(scenario.active_unit);
    if(command == GIT_REMOTE_COMMAND) {
        ++scenario.git_remote_calls;
        expect(
            fs::current_path() == unit.checkout_path,
            "Remote URL query did not run from the scheduled checkout");
        return;
    }
    if(command == GIT_BRANCH_COMMAND) {
        ++scenario.git_branch_calls;
        expect(
            scenario.git_fetch_calls == scenario.git_branch_calls,
            "Branch detection ran before git fetch");
        expect(
            fs::current_path() == unit.checkout_path,
            "Branch query did not run from the scheduled checkout");
        return;
    }
    if(command.starts_with("vercmp ")) {
        expect(
            fs::current_path() == unit.checkout_path,
            "Source update version comparison did not run from the scheduled checkout");
        return;
    }
    if(command.starts_with("LC_ALL=C ")) {
        ++scenario.identity_calls;
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Artifact identity query leaked the makepkg working directory");
        for(const fs::path& artifact_path :
            scenario.produced_artifact_paths.at(scenario.active_unit)) {
            expect(
                fs::is_regular_file(artifact_path),
                "Artifact identity query started before all build outputs existed");
        }
        return;
    }
    if(command.find("'makepkg' '--packagelist'") != std::string::npos) {
        ++scenario.packagelist_calls;
        expect(
            fs::current_path() == unit.checkout_path,
            "makepkg --packagelist did not run from the scheduled checkout");
        for(const fs::path& artifact_path :
            scenario.produced_artifact_paths.at(scenario.active_unit)) {
            expect(
                !fs::exists(artifact_path),
                "Fresh workspace contained an artifact before the build");
        }
        return;
    }
    throw std::logic_error("Unknown production capture command category.");
}

void require_metadata_released_before_install() {
    expect(
        metadata_stub::created_handle_count() > 0,
        "sudo pacman started without a metadata session");
    expect(
        metadata_stub::release_call_count() ==
            metadata_stub::created_handle_count(),
        "sudo pacman started before metadata session release");
    for(std::size_t index = 0;
        index < metadata_stub::created_handle_count(); ++index) {
        expect(
            metadata_stub::release_count_for_handle(index) == 1,
            "Metadata session was not released exactly once");
    }
}

void observe_run_command() {
    if(g_scenario == nullptr) {
        throw std::logic_error(
            "Process run occurred outside an active production scenario.");
    }
    ProductionScenario& scenario = *g_scenario;
    const std::string command = process_stub::last_run_command();
    if(command.starts_with("pacman -Q ")) {
        ++scenario.repository_provider_query_calls;
        expect(
            scenario.repository_provider_install_calls == 0,
            "Repository provider query ran after its install transaction");
        expect(
            scenario.git_remote_calls == 0 &&
                scenario.git_fetch_calls == 0 &&
                scenario.build_calls == 0 &&
                scenario.workspace_paths.empty(),
            "Source mutation started before repository provider query");
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Repository provider query changed the caller working directory");
        return;
    }
    if(command.starts_with("'sudo' 'pacman' '-S'")) {
        ++scenario.repository_provider_install_calls;
        require_metadata_released_before_install();
        expect(
            !metadata_stub::local_package_query_history().empty(),
            "Repository provider install started without installed metadata authority");
        expect(
            scenario.git_remote_calls == 0 &&
                scenario.git_fetch_calls == 0 &&
                scenario.build_calls == 0 &&
                scenario.workspace_paths.empty(),
            "Source mutation started before repository provider install");
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Repository provider install changed the caller working directory");
        return;
    }

    const UnitPlan& unit = scenario.units.at(scenario.active_unit);
    if(command == "git fetch origin") {
        ++scenario.git_fetch_calls;
        expect(
            scenario.git_remote_calls == scenario.git_fetch_calls,
            "git fetch ran before remote identity validation");
        expect(
            fs::current_path() == unit.checkout_path,
            "git fetch did not run from the scheduled checkout");
        return;
    }
    if(command == "git reset --hard 'origin/main'") {
        ++scenario.git_reset_calls;
        expect(
            scenario.git_branch_calls == scenario.git_reset_calls,
            "git reset ran before branch detection");
        expect(
            fs::current_path() == unit.checkout_path,
            "git reset did not run from the scheduled checkout");
        return;
    }
    if(command.find("'makepkg' '-sc'") != std::string::npos) {
        ++scenario.build_calls;
        expect(
            scenario.packagelist_calls == scenario.build_calls,
            "Build-only makepkg ran before makepkg --packagelist");
        expect(
            metadata_stub::initialize_call_count() + 1 ==
                scenario.build_calls +
                    scenario.repository_provider_install_calls,
            "Artifact metadata session opened before build-only makepkg");
        expect(
            fs::current_path() == unit.checkout_path,
            "Build-only makepkg did not run from the scheduled checkout");
        if(unit.build_exit_code == 0) {
            for(const fs::path& artifact_path :
                scenario.produced_artifact_paths.at(scenario.active_unit)) {
                write_file(artifact_path, "built package artifact\n");
            }
        }
        if(unit.replace_workspace_after_build) {
            fs::path displaced =
                scenario.workspace_paths.at(scenario.active_unit);
            displaced += ".build-succeeded-before-revalidation";
            fs::rename(
                scenario.workspace_paths.at(scenario.active_unit),
                displaced);
            fs::create_directory(
                scenario.workspace_paths.at(scenario.active_unit));
            fs::permissions(
                scenario.workspace_paths.at(scenario.active_unit),
                fs::perms::owner_all, fs::perm_options::replace);
            scenario.displaced_workspace_paths.at(scenario.active_unit) =
                std::move(displaced);
        }
        return;
    }
    if(command.starts_with("'sudo' 'pacman' '-U'")) {
        ++scenario.install_calls;
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "sudo pacman ran before restoring the caller directory");
        require_metadata_released_before_install();
        scenario.install_attempt_order.push_back(unit.package_base);

        if(unit.replace_workspace_after_install) {
            fs::path displaced =
                scenario.workspace_paths.at(scenario.active_unit);
            displaced += ".installed-before-cleanup";
            fs::rename(
                scenario.workspace_paths.at(scenario.active_unit),
                displaced);
            fs::create_directory(
                scenario.workspace_paths.at(scenario.active_unit));
            scenario.displaced_workspace_paths.at(scenario.active_unit) =
                std::move(displaced);
        }

        // Failure/cleanup partial-success must stop before a later PackageBase.
        if(unit.install_exit_code == 0 &&
           !unit.replace_workspace_after_install &&
           scenario.active_unit + 1 < scenario.units.size()) {
            schedule_source_unit(scenario.active_unit + 1);
        }
        return;
    }
    throw std::logic_error("Unknown production run command category.");
}

void activate_scenario(ProductionScenario& scenario) {
    process_stub::reset_process_stub();
    metadata_stub::reset_alpm_stub();
    metadata_stub::set_package_absent();
    scenario.workspace_paths.clear();
    scenario.artifact_paths.clear();
    scenario.produced_artifact_paths.clear();
    scenario.displaced_workspace_paths.clear();
    scenario.install_attempt_order.clear();
    scenario.active_unit = 0;
    scenario.resolver_calls = 0;
    scenario.git_remote_calls = 0;
    scenario.git_fetch_calls = 0;
    scenario.git_branch_calls = 0;
    scenario.git_reset_calls = 0;
    scenario.packagelist_calls = 0;
    scenario.build_calls = 0;
    scenario.identity_calls = 0;
    scenario.install_calls = 0;
    scenario.repository_provider_query_calls = 0;
    scenario.repository_provider_install_calls = 0;

    g_scenario = &scenario;
    set_separated_source_build_workspace_observer_for_test(observe_workspace);
    set_separated_package_base_source_build_workspace_observer_for_test(
        observe_workspace);
    process_stub::set_capture_hook(observe_capture_command);
    process_stub::set_run_hook(observe_run_command);
}

void deactivate_scenario() {
    set_separated_source_build_workspace_observer_for_test(nullptr);
    set_separated_package_base_source_build_workspace_observer_for_test(
        nullptr);
    process_stub::set_capture_hook(nullptr);
    process_stub::set_run_hook(nullptr);
    g_scenario = nullptr;
}

void require_scenario_complete(
    const ProductionScenario& scenario,
    std::size_t expected_workspace_count,
    const std::string& context) {
    process_stub::require_process_expectations_consumed();
    expect(
        scenario.workspace_paths.size() == expected_workspace_count,
        context + ": workspace count differs");
    expect(
        scenario.packagelist_calls == expected_workspace_count,
        context + ": packagelist count differs");
    expect(
        scenario.build_calls == expected_workspace_count,
        context + ": build count differs");
    deactivate_scenario();
}

ProductionScenario make_execution_scenario(
    const TemporaryProductionEnvironment& environment,
    const std::vector<ProductionSourceBuildWorkItem>& work_items,
    const AppConfig& config) {
    ProductionScenario scenario;
    scenario.config = config;
    scenario.caller_working_directory =
        environment.original_working_directory();
    for(const ProductionSourceBuildWorkItem& work_item : work_items) {
        UnitPlan unit;
        unit.package_name = work_item.request.package_name;
        unit.package_base = work_item.request.checkout_name;
        unit.git_url = work_item.request.git_url;
        unit.required_targets = work_item.required_targets;
        for(const RequiredPackageArtifactTarget& target :
            work_item.required_targets) {
            unit.produced_artifacts.push_back(ProducedArtifactPlan{
                target.package_name, ARTIFACT_VERSION});
        }
        unit.source_environment = work_item.request.custom_environment;
        unit.empty_value_policy = work_item.request.empty_value_policy;
        unit.needed = work_item.request.needed;
        unit.checkout_path = environment.ensure_checkout(
            work_item.request.checkout_name,
            work_item.request.git_url);
        scenario.units.push_back(std::move(unit));
    }
    return scenario;
}

PreparedProductionSourceBuildInvocation prepare_execution(
    std::vector<ProductionSourceBuildWorkItem> work_items,
    ProductionScenario& scenario) {
    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), scenario.config);
    activate_production_source_build_cache(invocation);
    expect(
        scenario.resolver_calls == 1,
        "Production resolver did not run exactly once during preflight");
    expect(
        scenario.workspace_paths.empty(),
        "Production preflight created an artifact workspace");
    schedule_source_unit(0);
    return invocation;
}

ProductionSourceBuildInvocationResult execute_invocation(
    const PreparedProductionSourceBuildInvocation& invocation,
    ProductionScenario& scenario) {
    expect(
        fs::current_path() == scenario.caller_working_directory,
        "Production execution started from a drifted working directory");
    try {
        ProductionSourceBuildInvocationResult result =
            execute_prepared_source_build_invocation(
                invocation, scenario.config);
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Production success leaked a changed working directory");
        return result;
    } catch(...) {
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Production failure leaked a changed working directory");
        throw;
    }
}

std::optional<ArtifactInstallExecutionOutcome> execute_work_item(
    const PreparedProductionSourceBuildInvocation& invocation,
    std::size_t work_item_index,
    ProductionScenario& scenario) {
    expect(
        fs::current_path() == scenario.caller_working_directory,
        "Production work-item execution started from a drifted working directory");
    try {
        std::optional<ArtifactInstallExecutionOutcome> outcome =
            execute_prepared_source_build_work_item(
                invocation.work_items.at(work_item_index),
                invocation.database_paths, scenario.config);
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Production work-item success leaked a changed working directory");
        return outcome;
    } catch(...) {
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Production work-item failure leaked a changed working directory");
        throw;
    }
}

SourceBuildExecutionResult execute_work_item_typed(
    const PreparedProductionSourceBuildInvocation& invocation,
    std::size_t work_item_index,
    ProductionScenario& scenario) {
    expect(
        fs::current_path() == scenario.caller_working_directory,
        "Typed production work-item execution started from a drifted working directory");
    try {
        SourceBuildExecutionResult result =
            execute_prepared_source_build_work_item_typed(
                invocation.work_items.at(work_item_index),
                invocation.database_paths, scenario.config);
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Typed production work-item success leaked a changed working directory");
        return result;
    } catch(...) {
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "Typed production work-item failure leaked a changed working directory");
        throw;
    }
}

PackageBaseSourceBuildExecutionResult execute_package_base_work_item_typed(
    const PreparedProductionSourceBuildInvocation& invocation,
    std::size_t work_item_index,
    ProductionScenario& scenario) {
    expect(
        fs::current_path() == scenario.caller_working_directory,
        "PackageBase production execution started from a drifted working directory");
    try {
        auto execution = execute_prepared_package_base_source_build_work_item_typed(
            invocation.work_items.at(work_item_index), invocation.database_paths, scenario.config);
        expect(std::holds_alternative<PackageBaseSourceBuildExecutionResult>(execution), "Legacy fixture selected authoritative execution");
        auto result = std::get<PackageBaseSourceBuildExecutionResult>(std::move(execution));
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "PackageBase production success leaked a changed working directory");
        return result;
    } catch(...) {
        expect(
            fs::current_path() == scenario.caller_working_directory,
            "PackageBase production failure leaked a changed working directory");
        throw;
    }
}

struct PreflightFilesystemSnapshot {
    CacheTreeSnapshot cache_tree;
    std::vector<fs::path> expected_missing_checkout_paths;
};

bool cache_entry_exists_without_following(const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if(error == std::errc::no_such_file_or_directory) return false;
    if(error) {
        throw std::runtime_error(
            "Failed to inspect production preflight fixture path: " +
            path.string() + ": " + error.message());
    }
    return fs::exists(status);
}

PreflightFilesystemSnapshot snapshot_preflight_filesystem(
    const TemporaryProductionEnvironment& environment,
    const std::vector<std::string>& expected_missing_checkout_names,
    const std::string& context) {
    PreflightFilesystemSnapshot snapshot;
    snapshot.cache_tree = environment.cache_tree_snapshot();

    // POLICY: relative path setがcache root直下entry一覧を表し、payloadが
    // regular file内容とsymlink targetのmutationを固定する。
    expect(
        std::find(
            snapshot.cache_tree.begin(), snapshot.cache_tree.end(),
            CacheTreeEntry{
                "preflight-snapshot-fixture",
                fs::file_type::directory,
                ""}) != snapshot.cache_tree.end(),
        context + ": snapshot omitted the direct cache fixture entry");
    expect(
        std::find(
            snapshot.cache_tree.begin(), snapshot.cache_tree.end(),
            CacheTreeEntry{
                "preflight-snapshot-fixture/state.txt",
                fs::file_type::regular,
                "stable preflight fixture\n"}) !=
            snapshot.cache_tree.end(),
        context + ": snapshot omitted fixture file contents");
    expect(
        std::find(
            snapshot.cache_tree.begin(), snapshot.cache_tree.end(),
            CacheTreeEntry{
                "preflight-snapshot-fixture/state-link",
                fs::file_type::symlink,
                "state.txt"}) != snapshot.cache_tree.end(),
        context + ": snapshot omitted fixture symlink target");
    expect(
        environment.artifact_workspaces().empty(),
        context + ": artifact workspace existed before preflight");

    for(const std::string& checkout_name :
        expected_missing_checkout_names) {
        fs::path checkout_path =
            environment.checkout_target_path(checkout_name);
        expect(
            !cache_entry_exists_without_following(checkout_path),
            context + ": checkout target existed before preflight: " +
                checkout_name);
        snapshot.expected_missing_checkout_paths.push_back(
            std::move(checkout_path));
    }
    return snapshot;
}

void expect_zero_mutation_state(
    const TemporaryProductionEnvironment& environment,
    const PreflightFilesystemSnapshot& before,
    const ProductionScenario& scenario,
    const std::string& context) {
    expect(
        scenario.workspace_paths.empty(),
        context + ": workspace observer unexpectedly ran");
    expect(
        environment.cache_tree_snapshot() == before.cache_tree,
        context + ": cache/source filesystem changed during preflight");
    expect(
        environment.artifact_workspaces().empty(),
        context + ": artifact workspace was created");
    for(const fs::path& checkout_path :
        before.expected_missing_checkout_paths) {
        expect(
            !cache_entry_exists_without_following(checkout_path),
            context + ": checkout target was created: " +
                checkout_path.filename().string());
    }
    expect(
        process_stub::run_command_call_count() == 0,
        context + ": mutation-capable process unexpectedly ran");
    expect(
        metadata_stub::initialize_call_count() == 0,
        context + ": metadata session unexpectedly opened");
}

void test_process_stub_rejects_cross_kind_reordering() {
    process_stub::reset_process_stub();
    process_stub::expect_capture_command(
        "capture-before-run", CapturedCommandResult{"unused", 0});
    process_stub::expect_run_command("run-after-capture", 0);

    const std::string diagnostic = expect_logic_error(
        []() {
            static_cast<void>(run_command("run-after-capture"));
        },
        "process global FIFO kind mismatch",
        "Unexpected artifact install run command");
    expect(
        process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 1,
        "Process global FIFO kind mismatch reached the later run expectation");
    expect(
        expect_logic_error(
            process_stub::require_process_expectations_consumed,
            "sticky process global FIFO kind mismatch",
            "Unexpected artifact install run command") == diagnostic,
        "Process global FIFO kind mismatch was not retained");

    process_stub::reset_process_stub();
    process_stub::require_process_expectations_consumed();
}

void test_build_plan_projection() {
    const BuildPlan plan = two_entry_plan();
    const std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(plan, false, true);
    expect(work_items.size() == 2, "BuildPlan projection changed unit count");
    expect(
        work_items[0].request.package_name == "dependency-package" &&
            work_items[1].request.package_name == "root-package",
        "BuildPlan::order was not preserved by production work-item preparation");
    expect(work_items[0].required_targets.size() == 1,
           "Ordinary dependency work item did not retain one required target");
    expect(work_items[1].required_targets.size() == 1,
           "Ordinary root work item did not retain one required target");
    expect_required_target(
        work_items[0].required_targets.front(),
        "dependency-package", "dependency-package",
        DesiredInstallReason::Dependency,
        "ordinary dependency BuildPlan projection");
    expect_required_target(
        work_items[1].required_targets.front(),
        "root-package", "root-package",
        DesiredInstallReason::Explicit,
        "ordinary root BuildPlan projection");
    expect(
        work_items[0].request.needed && work_items[1].request.needed &&
            work_items[0].required_target_provenance ==
                RequiredTargetProvenance::
                    AurBuildPlanProjection &&
            work_items[0].artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet &&
            work_items[1].required_target_provenance ==
                RequiredTargetProvenance::
                    AurBuildPlanProjection &&
            work_items[1].artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet,
        "--needed was not projected to every BuildPlan unit");

    BuildPlan attributed_plan = two_entry_plan();
    BuildPlanDependencyEdge attributed_edge;
    attributed_edge.parent_package_name = "root-package";
    attributed_edge.parent_package_base = "root-package";
    attributed_edge.dependency_spec = "dependency-package";
    attributed_edge.role = PackageRole::RuntimeDependency;
    attributed_edge.kind = DependencyKind::Aur;
    attributed_edge.resolved_package_name = "dependency-package";
    attributed_edge.resolved_package_base = "dependency-package";
    attributed_edge.resolved_candidate = AurResolvedDependencyCandidate{
        "dependency-package", "dependency-package",
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage, "1.0-1")};
    attributed_plan.dependency_edges.push_back(
        std::move(attributed_edge));
    const std::vector<ProductionSourceBuildWorkItem>
        attributed_work_items = prepare_aur_source_build_work_items(
            attributed_plan, false, false);
    expect(
        attributed_work_items[0].build_plan_dependency_edge_indices ==
                std::vector<std::size_t>{0} &&
            attributed_work_items[1]
                .build_plan_dependency_edge_indices.empty(),
        "AUR source artifact work item lost its exact BuildPlan edge index");
}

LocalPackageMetadata local_repository_provider_metadata_fixture() {
    const LocalPackageMetadataParseResult parsed =
        parse_local_package_metadata(
            LOCAL_REPOSITORY_PROVIDER_SRCINFO);
    expect(
        parsed.is_success() && parsed.metadata() != nullptr,
        "Local repository-provider metadata fixture is invalid");
    return *parsed.metadata();
}

void test_local_dependency_preparation_collects_selected_repository_providers() {
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();

    const std::string first_dependency = "virtual-local-api";
    const std::string duplicate_dependency =
        "virtual-local-api-alias";
    const ProvidedDependency first_provider = make_repository_provider(
        "extra", "local-root-provider", first_dependency,
        first_dependency + "=1", std::string("2.4-1"));
    const ProvidedDependency duplicate_provider = make_repository_provider(
        "extra", "local-root-provider", duplicate_dependency,
        duplicate_dependency + "=1", std::string("2.4-1"));
    expect(
        same_provider_identity(first_provider, duplicate_provider) &&
            first_provider != duplicate_provider,
        "Local repository-provider fixture does not isolate identity deduplication");

    query_stub::set_repository_package_response(
        first_dependency, std::nullopt);
    query_stub::set_aur_package_response(first_dependency, std::nullopt);
    query_stub::set_repository_provider_response(
        first_dependency, {first_provider});
    query_stub::set_repository_package_response(
        duplicate_dependency, std::nullopt);
    query_stub::set_aur_package_response(
        duplicate_dependency, std::nullopt);
    query_stub::set_repository_provider_response(
        duplicate_dependency, {duplicate_provider});

    std::size_t selection_count = 0;
    const LocalBuildPlan plan = resolve_local_build_plan(
        local_repository_provider_metadata_fixture(), "x86_64",
        [&](const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
            -> std::optional<ProvidedDependency> {
            ++selection_count;
            const ProvidedDependency& expected =
                dependency == first_dependency
                    ? first_provider
                    : duplicate_provider;
            expect(
                (dependency == first_dependency ||
                 dependency == duplicate_dependency) &&
                    candidates ==
                        std::vector<ProvidedDependency>{
                            expected},
                "Local repository-provider selection candidates differ");
            return candidates.front();
        });
    expect(
        selection_count == 2 && plan.failures().empty() &&
            plan.build_plan().provided.size() == 2 &&
            plan.build_plan().provided[0].resolution ==
                ProviderResolutionKind::UserSelected &&
            plan.build_plan().provided[1].resolution ==
                ProviderResolutionKind::UserSelected,
        "Actual LocalBuildPlan did not retain both selected provider edges");
    expect(
        plan.build_plan().order.size() == 1 &&
            plan.build_plan().order.front().package_base ==
                "local-provider-suite",
        "Local provider plan did not retain one local PackageBase unit");

    const LocalSourceBuildDependencyPreparation preparation =
        prepare_local_source_build_dependencies(plan, false, true);
    expect(
        preparation.remote_work_items().empty(),
        "Local PackageBase unit leaked into remote source-build work items");
    expect(
        preparation.selected_repository_providers() ==
                std::vector<ProvidedDependency>{first_provider} &&
            preparation.selected_repository_providers().front().provided_dependency_specification ==
                first_dependency + "=1",
        "Production local dependency preparation did not preserve "
        "first-seen repository-provider identity");

    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_local_dependency_invocation_accepts_zero_remote_units(
    const TemporaryProductionEnvironment&) {
    process_stub::reset_process_stub();
    const AppConfig config = noninteractive_config();
    LocalSourceBuildDependencyPreparation preparation =
        LocalSourceBuildDependencyPreparation::
            make_for_production_source_build_test({}, {});
    expect(
        preparation.remote_work_items().empty(),
        "Empty local dependency preparation unexpectedly contains an AUR work item");
    expect(
        preparation.selected_repository_providers().empty(),
        "Empty local dependency preparation unexpectedly contains a repository provider");

    preflight_local_source_build_dependencies(preparation, config);
    const ValidatedCacheRoot cache_root = prepare_process_cache_root();
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_local_source_build_dependency_invocation(
            std::move(preparation), cache_root, config);
    expect(
        invocation.work_items.empty() &&
            invocation.local_source_authority.has_value(),
        "Local-only dependency invocation did not retain its empty-invocation authority");
    expect(
        invocation.cache_root.has_value() &&
            invocation.cache_root->device() == cache_root.device() &&
            invocation.cache_root->inode() == cache_root.inode() &&
            invocation.cache_root->owner() == cache_root.owner(),
        "Local-only dependency invocation changed its cache authority");

    PreparedProductionSourceBuildInvocation unowned_empty_invocation =
        invocation;
    unowned_empty_invocation.local_source_authority.reset();
    static_cast<void>(expect_logic_error(
        [&unowned_empty_invocation]() {
            activate_production_source_build_cache(
                unowned_empty_invocation);
        },
        "unowned empty production invocation activation",
        "Cannot activate cache for an empty source-build invocation"));

    execute_prepared_source_build_invocation(
        std::move(invocation), config);
    expect(
        process_stub::run_command_call_count() == 0,
        "Completely empty local dependency invocation executed a process");
    process_stub::require_process_expectations_consumed();

    static_cast<void>(expect_logic_error(
        [&config]() {
            static_cast<void>(prepare_production_source_build_invocation(
                {}, config));
        },
        "generic empty production invocation",
        "must contain at least one work item"));
}

void test_local_dependency_invocation_executes_provider_without_aur_units(
    const TemporaryProductionEnvironment&) {
    process_stub::reset_process_stub();
    metadata_stub::reset_alpm_stub();
    metadata_stub::set_package_absent();
    const AppConfig config = noninteractive_config();
    const ProvidedDependency first_provider = make_repository_provider(
        "extra", "local-root-provider", "virtual-local-api",
        "virtual-local-api=1", std::string("1.0-1"));
    const ProvidedDependency duplicate_provider = make_repository_provider(
        "extra", "local-root-provider", "virtual-local-api-alias",
        "virtual-local-api-alias=1", std::string("1.1-1"));
    LocalSourceBuildDependencyPreparation preparation =
        LocalSourceBuildDependencyPreparation::
            make_for_production_source_build_test(
                {}, {first_provider, duplicate_provider});

    preflight_local_source_build_dependencies(preparation, config);
    const ValidatedCacheRoot cache_root = prepare_process_cache_root();
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_local_source_build_dependency_invocation(
            std::move(preparation), cache_root, config);
    expect(
        invocation.work_items.empty() &&
            invocation.local_source_authority.has_value(),
        "Provider-only local dependency invocation lost its local authority");
    expect(
        invocation.selected_repository_providers ==
            std::vector<ProvidedDependency>{first_provider},
        "Provider-only local dependency invocation did not preserve first-seen identity deduplication");

    process_stub::expect_run_command(
        expected_repository_provider_install_command(
            {first_provider}, config),
        0);
    execute_prepared_source_build_invocation(
        std::move(invocation), config);
    expect(
        process_stub::run_command_call_count() == 1,
        "Provider-only local dependency invocation did not execute exactly one transaction");
    expect(
        metadata_stub::local_package_query_history() ==
                std::vector<std::string>{"local-root-provider"} &&
            metadata_stub::release_call_count() == 1,
        "Provider-only local dependency invocation did not close its installed metadata authority");
    process_stub::require_process_expectations_consumed();
}

void test_selected_repository_provider_projection_and_invocation_deduplication() {
    const ProvidedDependency dependency_provider = make_repository_provider(
        "extra", "repository-provider", "virtual-api",
        "virtual-api=2", std::string("2.4-1"));
    const ProvidedDependency root_provider = make_repository_provider(
        "extra", "repository-provider", "virtual-api-alias",
        "virtual-api-alias=2", std::string("2.5-1"));
    expect(
        same_provider_identity(dependency_provider, root_provider) &&
            dependency_provider != root_provider,
        "Repository provider metadata fixture did not isolate identity dedupe");

    BuildPlan plan = two_entry_plan();
    plan.dependency_edges.push_back(make_repository_provider_edge(
        "dependency-package", "dependency-package",
        dependency_provider, ProviderResolutionKind::UserSelected));
    plan.dependency_edges.push_back(make_repository_provider_edge(
        "root-package", "root-package", root_provider,
        ProviderResolutionKind::UserSelected));

    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(plan, false, false);
    expect(
        work_items.size() == 2 &&
            work_items[0].selected_repository_providers ==
                std::vector<ProvidedDependency>{
                    dependency_provider} &&
            work_items[1].selected_repository_providers ==
                std::vector<ProvidedDependency>{root_provider},
        "BuildPlan projection did not preserve selected provider metadata per edge owner");

    process_stub::reset_process_stub();
    expect_database_paths();
    const PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), noninteractive_config());
    process_stub::require_process_expectations_consumed();
    expect(
        invocation.selected_repository_providers ==
            std::vector<ProvidedDependency>{dependency_provider},
        "Invocation did not deduplicate selected providers by source-aware identity in first-seen order");
    expect(
        invocation.selected_repository_providers.front()
                    .provided_dependency_specification ==
                "virtual-api=2" &&
            invocation.selected_repository_providers.front()
                    .package_version == std::string("2.4-1"),
        "Invocation discarded first-seen selected provider metadata");
}

void test_selected_repository_provider_static_validation() {
    ProductionSourceBuildWorkItem aur_origin =
        make_work_item("invalid-aur-provider-owner");
    aur_origin.selected_repository_providers.push_back(
        ProvidedDependency::from_aur(
            "aur-provider", "aur-provider-base", "virtual-api",
            "virtual-api=1", std::string("1.0-1")));
    expect_logic_error(
        [&aur_origin]() {
            require_static_production_source_build_work_item(aur_origin);
        },
        "AUR origin selected repository provider",
        "not repository-owned");

    ProductionSourceBuildWorkItem invalid_repository =
        make_work_item("invalid-repository-provider-identity");
    invalid_repository.selected_repository_providers.push_back(
        make_repository_provider(
            "invalid/repository", "repository-provider",
            "virtual-api", "virtual-api=1",
            std::string("1.0-1")));
    expect_logic_error(
        [&invalid_repository]() {
            require_static_production_source_build_work_item(
                invalid_repository);
        },
        "invalid selected repository provider identity",
        "invalid repository name");
}

void test_conflicting_selected_provider_identity_stops_work_item_preparation() {
    BuildPlan plan = ordinary_single_entry_plan();
    plan.provided = {
        BuildPlanProvidedDependency{
            "first-virtual",
            make_repository_provider(
                "extra", "conflicting-provider",
                "first-virtual", "first-virtual=1",
                std::string("1.0-1")),
            ProviderResolutionKind::UserSelected},
        BuildPlanProvidedDependency{
            "second-virtual",
            ProvidedDependency::from_aur(
                "conflicting-provider",
                "conflicting-provider-base", "second-virtual",
                "second-virtual=1", "1.0-1"),
            ProviderResolutionKind::UserSelected},
    };

    expect_runtime_error(
        [&plan]() {
            static_cast<void>(prepare_aur_source_build_work_items(
                plan, true, false));
        },
        "selected provider identity conflict before source preparation",
        "Selected providers use incompatible identities for package "
        "conflicting-provider: extra/conflicting-provider and "
        "aur/conflicting-provider (PackageBase: "
        "conflicting-provider-base).");
}

void test_selected_repository_provider_executes_before_source(
    const TemporaryProductionEnvironment& environment) {
    const ProvidedDependency provider = make_repository_provider(
        "extra", "repository-provider", "virtual-api",
        "virtual-api=2", std::string("2.4-1"));
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    AppConfig config = noninteractive_config();
    config.no_confirm = true;
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    expect(
        invocation.selected_repository_providers ==
                std::vector<ProvidedDependency>{provider} &&
            invocation.selected_repository_providers.front()
                    .package_base == "repository-provider-base" &&
            invocation.work_items.front()
                    .selected_repository_provider_edge_indices ==
                std::vector<std::size_t>{0},
        "Selected repository provider did not reach execution invocation");
    process_stub::expect_run_command(
        expected_repository_provider_install_command({provider}, config),
        0);
    schedule_source_unit(0);

    const ProductionSourceBuildInvocationResult aggregate =
        execute_invocation(invocation, scenario);

    expect(
        aggregate.work_items.size() == 1 &&
            aggregate.work_items.front().package_base ==
                invocation.work_items.front().request.checkout_name &&
            aggregate.work_items.front().status ==
                ProductionSourceBuildWorkItemStatus::Succeeded &&
            aggregate.work_items.front().production_outcome.has_value() &&
            aggregate.work_items.front().production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            aggregate.work_items.front().production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Succeeded &&
            aggregate.work_items.front().production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            aggregate.work_items.front().production_outcome->source_provenance.compatibility_reason ==
                std::optional<
                    ReviewedSourceCompatibilityBuildReason>{
                    ReviewedSourceCompatibilityBuildReason::
                        NoDiff},
        "Invocation aggregate flattened reviewed-source outcome");
    expect(
        scenario.repository_provider_query_calls == 0 &&
            scenario.repository_provider_install_calls == 1,
        "Selected repository provider transaction count differs");
    expect(
        scenario.git_remote_calls == 1 && scenario.build_calls == 1,
        "Source execution did not continue after provider installation");
    require_scenario_complete(
        scenario, 1,
        "selected repository provider execution ordering");
}

void test_new_repository_provider_uses_asdeps_needed_transaction(
    const TemporaryProductionEnvironment& environment) {
    const ProvidedDependency provider = make_repository_provider(
        "core", "installed-provider", "virtual-installed",
        "virtual-installed=1", std::string("1.0-1"));
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    process_stub::expect_run_command(
        expected_repository_provider_install_command({provider}, config),
        0);
    schedule_source_unit(0);

    execute_invocation(invocation, scenario);
    expect(
        scenario.repository_provider_query_calls == 0 &&
            scenario.repository_provider_install_calls == 1,
        "New selected repository provider bypassed the exact --asdeps --needed transaction");
    expect(
        !metadata_stub::local_package_query_history().empty() &&
            metadata_stub::local_package_query_history().front() ==
                provider.package_name,
        "New selected repository provider did not use installed metadata authority");
    require_scenario_complete(
        scenario, 1,
        "new selected repository provider exact --asdeps --needed transaction");
}

void test_selected_repository_provider_trusted_executor_closes_evidence(
    const TemporaryProductionEnvironment& environment) {
    constexpr std::size_t REPOSITORY_ORDER = 0;
    const std::string repository_name = "core";
    const std::string package_name = "receipt-provider";
    const std::string package_base = "receipt-provider-base";
    const std::string package_architecture = "x86_64";
    const std::string provided_name = "virtual-receipt-provider";
    const std::string provided_specification =
        "virtual-receipt-provider=1";
    const std::string package_version = "1.0-1";
    const std::string provided_version = "1";
    const ScopedEnvironmentVariable vercmp_lhs(
        "MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS", provided_version);
    const ScopedEnvironmentVariable vercmp_rhs(
        "MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS", provided_version);
    const ScopedEnvironmentVariable vercmp_result(
        "MOGUET_TEST_ALPM_VERCMP_RESULT", std::string("0"));
    const ProvidedDependency provider = make_repository_provider(
        repository_name, package_name, provided_name,
        provided_specification, package_version, package_base,
        package_architecture, REPOSITORY_ORDER);
    BuildPlan plan = single_repository_provider_plan(
        provider, ProviderResolutionKind::UserSelected);
    plan.configured_repository_order =
        std::vector<std::string>{repository_name};
    BuildPlanDependencyEdge& edge = plan.dependency_edges.front();
    const DependencyRequirementParseResult requirement_parse =
        parse_dependency_requirement(provided_specification);
    const DependencyRequirement* const requirement =
        requirement_parse.requirement();
    expect(
        requirement != nullptr &&
            std::holds_alternative<ConsumerDependencyRequirement>(
                *requirement),
        "Closed selected-provider fixture did not parse its typed requirement");
    edge.requirement = *requirement;
    edge.resolved_candidate = ProviderResolvedDependencyCandidate{
        provider, provider.constraint_metadata->provided_version};
    edge.constraint_evaluation = evaluate_consumer_dependency_requirement(
        std::get<ConsumerDependencyRequirement>(*requirement),
        provider.constraint_metadata->provided_version);

    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(plan, false, false);
    AppConfig config = noninteractive_config();
    config.no_confirm = true;
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), scenario.config);
    activate_production_source_build_cache(invocation);

    const auto owner = InvocationDependencyTransactionOwner::
        SelectedRepositoryProvider;
    const auto complete_capture =
        [&provider, owner](
            const std::string& token,
            const std::vector<std::string>& actual_install_set) {
            std::vector<PacmanTransactionPackageObservation> operations;
            operations.reserve(actual_install_set.size());
            for(const std::string& installed : actual_install_set) {
                operations.push_back(PacmanTransactionPackageObservation{
                    PacmanTransactionPackageOperation::Install, installed});
            }
            PacmanTransactionReceipt receipt =
                validate_pacman_transaction_receipt(
                    token, owner,
                    PacmanTransactionReceiptObservation{
                        PacmanTransactionReceiptObservationState::Complete,
                        token, owner, std::move(operations)});
            InvocationDependencyTransactionLedger ledger;
            ledger.transactions.push_back(
                InvocationDependencyTransaction{
                    token,
                    owner,
                    {provider.package_name},
                    InvocationDependencyTransactionCommandOutcome::Succeeded,
                    std::move(receipt)});
            return TrustedAlpmReceiptCaptureResult{
                TrustedAlpmReceiptCaptureStatus::Complete, 0,
                std::move(ledger), std::nullopt};
        };

    PreparedRemoteSourceBuild prepared{
        ResolvedSourceBuildIdentity{ResolvedAurSourceBuildIdentity{
            "ordinary-set-root", "ordinary-set-root"}},
        plan,
        invocation};
    CleanupInvocationSession session =
        CleanupInvocationSession::begin(std::move(prepared));
    mark_cleanup_invocation_baseline_observed_for_test(session);

    const std::string token(64, 'a');
    trusted_receipt_transport_stub::set_result(
        complete_capture(token, {package_name}));
    const SelectedRepositoryProviderTrustedReceiptExecutionResult execution =
        execute_selected_repository_provider_transaction(
            invocation, config,
            SelectedRepositoryProviderTrustedReceiptRequest::
                capture_actual_installs(session.authority()));
    expect(
        execution.transaction.status ==
                SelectedRepositoryProviderTransactionStatus::Succeeded &&
            execution.transaction.package_state_change ==
                PackageStateChange::Unknown &&
            execution.receipt_capture.has_value() &&
            execution.receipt_capture->status ==
                TrustedAlpmReceiptCaptureStatus::Complete &&
            execution.receipt_capture->transaction_ledger.transactions
                    .size() == 1 &&
            execution.receipt_capture->transaction_ledger
                .transactions[0]
                .receipt.contains_newly_installed_package(
                    package_name) &&
            execution.trusted_execution_evidence().has_value(),
        "trusted selected-provider executor did not close complete evidence");

    const SelectedRepositoryProviderTrustedExecutionEvidence& evidence =
        execution.trusted_execution_evidence().value();
    expect(
        evidence.selected_providers().size() == 1 &&
            evidence.selected_providers().front().constraint_metadata.has_value(),
        "trusted selected-provider producer did not retain one complete provider");
    const ProvidedDependency& observed_provider =
        evidence.selected_providers().front();
    const auto* observed_repository =
        std::get_if<RepositoryProviderOrigin>(&observed_provider.origin);
    const ProviderConstraintMetadata& observed_constraint =
        observed_provider.constraint_metadata.value();
    const ProviderCapability& observed_capability =
        observed_constraint.provided_capability;
    const std::string* observed_package_version =
        observed_constraint.package_version.version();
    const std::string* observed_provided_version =
        observed_constraint.provided_version.version();
    expect(
        evidence.owner() == owner &&
            evidence.invocation_authority() == session.authority() &&
            evidence.transaction_token() == token &&
            evidence.transaction().status ==
                SelectedRepositoryProviderTransactionStatus::Succeeded &&
            evidence.receipt_capture().status ==
                TrustedAlpmReceiptCaptureStatus::Complete &&
            observed_repository != nullptr &&
            observed_repository->repository_name == repository_name &&
            observed_repository->configured_order == REPOSITORY_ORDER &&
            observed_provider.package_name == package_name &&
            observed_provider.package_base == package_base &&
            observed_provider.package_architecture == package_architecture &&
            observed_provider.provided_dependency_name == provided_name &&
            observed_provider.provided_dependency_specification ==
                provided_specification &&
            observed_provider.package_version == package_version &&
            observed_capability.package_name() == provided_name &&
            observed_capability.raw_specification() ==
                provided_specification &&
            observed_capability.version() == provided_version &&
            observed_package_version != nullptr &&
            *observed_package_version == package_version &&
            observed_provided_version != nullptr &&
            *observed_provided_version == provided_version &&
            evidence.actual_install_set() ==
                std::vector<std::string>{package_name},
        "trusted selected-provider producer did not retain its independent fixture values");

    expect(
        evidence.bindings().size() == 1,
        "trusted selected-provider producer did not retain one edge binding");
    const SelectedRepositoryProviderTrustedExecutionBinding& binding =
        evidence.bindings().front();
    const auto* bound_requirement =
        std::get_if<ConsumerDependencyRequirement>(&binding.requirement);
    expect(
        binding.work_item_index == 0 &&
            binding.build_plan_edge_index == 0 &&
            binding.parent_package_base == "ordinary-set-root" &&
            binding.resolution == ProviderResolutionKind::UserSelected &&
            binding.provider == provider &&
            binding.selected_decision.dependency ==
                provided_specification &&
            binding.selected_decision.resolution ==
                ProviderResolutionKind::UserSelected &&
            binding.selected_decision.provider == provider &&
            bound_requirement != nullptr &&
            bound_requirement->raw_specification() ==
                provided_specification &&
            bound_requirement->package_name() == provided_name &&
            bound_requirement->constraint().has_value() &&
            bound_requirement->constraint()->relation() ==
                DependencyVersionRelation::Equal &&
            bound_requirement->constraint()->version() == provided_version,
        "trusted selected-provider producer did not retain its exact decision binding");

    ProductionSourceBuildInvocationResult successful_result;
    successful_result.work_items.push_back(
        ProductionSourceBuildWorkItemOutcome{
            "ordinary-set-root",
            ProductionSourceBuildWorkItemStatus::Succeeded,
            ProductionSourceBuildStagedOutcome{
                ProductionSourceBuildProvenance{},
                ProductionSourceBuildCommandOutcome::Succeeded,
                ProductionSourceInstallOutcome::Succeeded},
            std::nullopt,
            std::nullopt,
            nullptr});
    const CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, successful_result);
    InstalledPackageStateSnapshot current_snapshot;
    current_snapshot.emplace(
        package_name,
        InstalledPackageMetadata{
            package_name, package_version,
            InstalledPackageReason::Dependency,
            InstalledPackageBaseIdentity::known(package_base),
            InstalledPackageArchitectureIdentity::known(
                package_architecture)});
    const CleanupCurrentInstalledObservation current_observation =
        make_cleanup_current_observation_for_test(
            session, std::move(current_snapshot));
    const CleanupSelectedProviderCorrelationEvidence correlation =
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current_observation, evidence);
    expect(
        correlation.completeness() ==
                CleanupEvidenceCompleteness::Complete &&
            correlation.issues().empty() &&
            correlation.transaction_token() == token &&
            correlation.edge_correlations().size() == 1 &&
            correlation.edge_correlations().front().work_item_index == 0 &&
            correlation.edge_correlations()
                    .front()
                    .build_plan_edge_index == 0 &&
            correlation.edge_correlations().front().provider == provider &&
            correlation.edge_correlations()
                .front()
                .current_package.metadata.has_value() &&
            correlation.edge_correlations()
                    .front()
                    .current_package.metadata->package_base ==
                InstalledPackageBaseIdentity::known(package_base) &&
            correlation.edge_correlations()
                    .front()
                    .current_package.metadata->architecture ==
                InstalledPackageArchitectureIdentity::known(
                    package_architecture),
        "actual selected-provider trusted producer did not reach one complete correlation");

    const std::vector<TrustedAlpmReceiptRepositoryTarget> expected_targets{
        {repository_name, package_name}};
    expect(
        trusted_receipt_transport_stub::call_count == 1 &&
            trusted_receipt_transport_stub::last_request.has_value() &&
            trusted_receipt_transport_stub::last_request->targets ==
                expected_targets &&
            trusted_receipt_transport_stub::last_request
                    ->install_directive ==
                TrustedAlpmReceiptRepositoryInstallDirective::AsDependency &&
            trusted_receipt_transport_stub::last_request->no_confirm,
        "selected-provider receipt request changed target/reason/noconfirm authority");
    expect(
        process_stub::run_command_call_count() == 0,
        "receipt-capable API fell through to the legacy shell command");

    const std::string compatibility_token(64, 'b');
    trusted_receipt_transport_stub::set_result(
        complete_capture(compatibility_token, {package_name}));
    const SelectedRepositoryProviderTrustedReceiptExecutionResult
        compatibility_execution =
            execute_selected_repository_provider_transaction(
                invocation, config,
                SelectedRepositoryProviderTrustedReceiptRequest::
                    capture_actual_installs());
    expect(
        compatibility_execution.transaction.status ==
                SelectedRepositoryProviderTransactionStatus::Succeeded &&
            compatibility_execution.receipt_capture.has_value() &&
            !compatibility_execution.trusted_execution_evidence().has_value(),
        "legacy no-session selected-provider request gained closed evidence");

    const SelectedRepositoryProviderTrustedReceiptExecutionResult raw_rewrap{
        execution.transaction, execution.receipt_capture};
    expect(
        !raw_rewrap.trusted_execution_evidence().has_value(),
        "raw selected-provider execution rewrap gained closed evidence");

    const auto incomplete_capture =
        [&provider, owner](
            const std::string& capture_token,
            TrustedAlpmReceiptCaptureStatus capture_status,
            InvocationDependencyTransactionCommandOutcome command_outcome) {
            PacmanTransactionReceipt receipt =
                validate_pacman_transaction_receipt(
                    capture_token, owner,
                    PacmanTransactionReceiptObservation{
                        command_outcome ==
                                InvocationDependencyTransactionCommandOutcome::
                                    Unknown
                            ? PacmanTransactionReceiptObservationState::
                                  Incomplete
                            : PacmanTransactionReceiptObservationState::
                                  Missing,
                        command_outcome ==
                                InvocationDependencyTransactionCommandOutcome::
                                    Unknown
                            ? std::optional<std::string>{capture_token}
                            : std::nullopt,
                        command_outcome ==
                                InvocationDependencyTransactionCommandOutcome::
                                    Unknown
                            ? std::optional<
                                  InvocationDependencyTransactionOwner>{owner}
                            : std::nullopt,
                        {}});
            InvocationDependencyTransactionLedger ledger;
            ledger.transactions.push_back(
                InvocationDependencyTransaction{
                    capture_token, owner, {provider.package_name}, command_outcome, std::move(receipt)});
            return TrustedAlpmReceiptCaptureResult{
                capture_status, std::nullopt, std::move(ledger),
                "synthetic trusted transport failure"};
        };

    trusted_receipt_transport_stub::set_result(incomplete_capture(
        std::string(64, 'c'),
        TrustedAlpmReceiptCaptureStatus::OutcomeUnknown,
        InvocationDependencyTransactionCommandOutcome::Unknown));
    const SelectedRepositoryProviderTrustedReceiptExecutionResult
        post_launch_unknown_execution =
            execute_selected_repository_provider_transaction(
                invocation, config,
                SelectedRepositoryProviderTrustedReceiptRequest::
                    capture_actual_installs());
    expect(
        post_launch_unknown_execution.transaction.status ==
                SelectedRepositoryProviderTransactionStatus::
                    OutcomeUnknown &&
            !post_launch_unknown_execution.transaction.command_exit_status
                 .has_value() &&
            post_launch_unknown_execution.receipt_capture.has_value() &&
            post_launch_unknown_execution.receipt_capture
                    ->transaction_ledger.transactions.front()
                    .command_outcome ==
                InvocationDependencyTransactionCommandOutcome::Unknown &&
            !post_launch_unknown_execution.trusted_execution_evidence()
                 .has_value(),
        "post-launch trusted outcome became BlockedBeforeExecution");

    trusted_receipt_transport_stub::set_result(incomplete_capture(
        std::string(64, 'd'),
        TrustedAlpmReceiptCaptureStatus::PrepareFailed,
        InvocationDependencyTransactionCommandOutcome::NotAttempted));
    const SelectedRepositoryProviderTrustedReceiptExecutionResult
        pre_launch_failure_execution =
            execute_selected_repository_provider_transaction(
                invocation, config,
                SelectedRepositoryProviderTrustedReceiptRequest::
                    capture_actual_installs());
    expect(
        pre_launch_failure_execution.transaction.status ==
                SelectedRepositoryProviderTransactionStatus::
                    BlockedBeforeExecution &&
            !pre_launch_failure_execution.transaction.command_exit_status
                 .has_value() &&
            pre_launch_failure_execution.receipt_capture.has_value() &&
            pre_launch_failure_execution.receipt_capture
                    ->transaction_ledger.transactions.front()
                    .command_outcome ==
                InvocationDependencyTransactionCommandOutcome::
                    NotAttempted &&
            !pre_launch_failure_execution.trusted_execution_evidence()
                 .has_value(),
        "confirmed pre-launch failure lost BlockedBeforeExecution compatibility");

    trusted_receipt_transport_stub::set_result(
        TrustedAlpmReceiptCaptureResult{
            TrustedAlpmReceiptCaptureStatus::
                TrustedExecutableUnavailable,
            std::nullopt,
            {},
            "synthetic trusted executable preflight failure"});
    const SelectedRepositoryProviderTrustedReceiptExecutionResult
        transport_preflight_execution =
            execute_selected_repository_provider_transaction(
                invocation, config,
                SelectedRepositoryProviderTrustedReceiptRequest::
                    capture_actual_installs());
    expect(
        transport_preflight_execution.transaction.status ==
                SelectedRepositoryProviderTransactionStatus::
                    BlockedBeforeExecution &&
            !transport_preflight_execution.transaction.command_exit_status
                 .has_value() &&
            transport_preflight_execution.receipt_capture.has_value() &&
            transport_preflight_execution.receipt_capture
                ->transaction_ledger.transactions.empty(),
        "trusted executable preflight failure lost compatibility fallback eligibility");

    CleanupInvocationAuthority moved_from_authority = session.authority();
    [[maybe_unused]] CleanupInvocationAuthority retained_authority =
        std::move(moved_from_authority);
    const std::size_t calls_before_moved_from =
        trusted_receipt_transport_stub::call_count;
    const SelectedRepositoryProviderTrustedReceiptExecutionResult
        moved_from_execution =
            execute_selected_repository_provider_transaction(
                invocation, config,
                SelectedRepositoryProviderTrustedReceiptRequest::
                    capture_actual_installs(
                        std::move(moved_from_authority)));
    expect(
        moved_from_execution.transaction.status ==
                SelectedRepositoryProviderTransactionStatus::
                    BlockedBeforeExecution &&
            !moved_from_execution.receipt_capture.has_value() &&
            trusted_receipt_transport_stub::call_count ==
                calls_before_moved_from,
        "moved-from cleanup authority did not fail closed before transport");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void expect_existing_explicit_repository_provider_preserved(
    const TemporaryProductionEnvironment& environment,
    const std::string& provider_name,
    const std::string& installed_version,
    const std::string& available_version,
    const std::string& context) {
    const ProvidedDependency provider = make_repository_provider(
        "extra", provider_name, "virtual-explicit-provider",
        "virtual-explicit-provider=1", available_version);
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    activate_scenario(scenario);
    metadata_stub::set_package_metadata(
        provider.package_name, installed_version,
        ALPM_PKG_REASON_EXPLICIT);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    process_stub::expect_run_command(
        expected_repository_provider_install_command(
            {provider}, config, false),
        0);
    schedule_source_unit(0);

    execute_invocation(invocation, scenario);
    expect(
        scenario.repository_provider_install_calls == 1,
        context + " did not execute one reason-preserving provider transaction");
    expect(
        !metadata_stub::local_package_query_history().empty() &&
            metadata_stub::local_package_query_history().front() ==
                provider.package_name,
        context + " did not query the existing provider before pacman");
    expect(
        metadata_stub::created_handle_count() == 2 &&
            metadata_stub::release_call_count() == 2,
        context + " did not close provider and artifact metadata sessions");
    require_scenario_complete(scenario, 1, context);
}

void test_existing_explicit_repository_provider_same_version_stays_explicit(
    const TemporaryProductionEnvironment& environment) {
    expect_existing_explicit_repository_provider_preserved(
        environment, "same-version-explicit-provider", "2.4-1", "2.4-1",
        "same-version existing explicit repository provider");
}

void test_existing_explicit_repository_provider_update_stays_explicit(
    const TemporaryProductionEnvironment& environment) {
    expect_existing_explicit_repository_provider_preserved(
        environment, "update-explicit-provider", "1.0-1", "2.4-1",
        "update-required existing explicit repository provider");
}

void test_repository_provider_metadata_failure_stops_before_mutation(
    const TemporaryProductionEnvironment& environment) {
    const ProvidedDependency provider = make_repository_provider(
        "extra", "metadata-failure-provider", "virtual-metadata-failure",
        "virtual-metadata-failure=1", std::string("2.4-1"));
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    activate_scenario(scenario);
    metadata_stub::set_package_query_failure(ALPM_ERR_DB_OPEN);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);

    expect_runtime_error(
        [&]() { execute_invocation(invocation, scenario); },
        "selected repository provider metadata failure",
        "Installed package query failed");
    expect(
        scenario.repository_provider_install_calls == 0 &&
            scenario.git_remote_calls == 0 &&
            scenario.workspace_paths.empty() &&
            process_stub::run_command_call_count() == 0,
        "Repository provider metadata failure reached mutation");
    expect(
        metadata_stub::package_query_call_count() == 1 &&
            metadata_stub::release_call_count() == 1,
        "Repository provider metadata failure did not close its authority session");
    require_scenario_complete(
        scenario, 0, "selected repository provider metadata failure");
}

void test_mixed_repository_provider_reasons_stop_before_mutation(
    const TemporaryProductionEnvironment& environment) {
    const ProvidedDependency new_provider = make_repository_provider(
        "extra", "mixed-new-provider", "virtual-mixed-new",
        "virtual-mixed-new=1", std::string("2.4-1"));
    const ProvidedDependency explicit_provider = make_repository_provider(
        "extra", "mixed-explicit-provider", "virtual-mixed-explicit",
        "virtual-mixed-explicit=1", std::string("2.4-1"));
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                new_provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    work_items.front().selected_repository_providers.push_back(
        explicit_provider);
    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    activate_scenario(scenario);
    metadata_stub::enqueue_local_package_query_absent(
        new_provider.package_name);
    metadata_stub::enqueue_local_package_query_present(
        explicit_provider.package_name, explicit_provider.package_name,
        "1.0-1", ALPM_PKG_REASON_EXPLICIT);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);

    expect_runtime_error(
        [&]() { execute_invocation(invocation, scenario); },
        "mixed selected repository provider reasons",
        "Selected repository provider install reasons cannot be represented by one package transaction");
    expect(
        scenario.repository_provider_install_calls == 0 &&
            scenario.git_remote_calls == 0 &&
            scenario.workspace_paths.empty() &&
            process_stub::run_command_call_count() == 0,
        "Mixed repository provider reasons reached mutation");
    metadata_stub::require_local_package_query_expectations_consumed();
    expect(
        metadata_stub::release_call_count() == 1,
        "Mixed repository provider reason preflight did not close its authority session");
    require_scenario_complete(
        scenario, 0, "mixed selected repository provider reasons");
}

void test_repository_provider_failure_stops_before_source_mutation(
    const TemporaryProductionEnvironment& environment) {
    const ProvidedDependency provider = make_repository_provider(
        "extra", "failing-provider", "virtual-failure",
        "virtual-failure=1", std::string("1.0-1"));
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    const CacheTreeSnapshot cache_before = environment.cache_tree_snapshot();
    process_stub::expect_run_command(
        expected_repository_provider_install_command({provider}, config),
        37);

    expect_runtime_error(
        [&]() { execute_invocation(invocation, scenario); },
        "selected repository provider transaction failure",
        "Failed to install selected repository providers");
    expect(
        scenario.repository_provider_query_calls == 0 &&
            scenario.repository_provider_install_calls == 1,
        "Failing repository provider transaction count differs");
    expect(
        scenario.git_remote_calls == 0 &&
            scenario.git_fetch_calls == 0 &&
            scenario.build_calls == 0 &&
            scenario.workspace_paths.empty(),
        "Repository provider failure reached source execution");
    expect(
        environment.cache_tree_snapshot() == cache_before &&
            environment.artifact_workspaces().empty(),
        "Repository provider failure mutated source/cache state");
    expect(
        metadata_stub::initialize_call_count() == 1 &&
            metadata_stub::release_call_count() == 1,
        "Repository provider failure did not close provider metadata before pacman");
    require_scenario_complete(
        scenario, 0,
        "selected repository provider failure pre-mutation stop");
}

void test_unique_repository_provider_does_not_schedule_transaction(
    const TemporaryProductionEnvironment& environment) {
    const ProvidedDependency provider = make_repository_provider(
        "core", "unique-provider", "virtual-unique",
        "virtual-unique=1", std::string("1.0-1"));

    BuildPlan unselected_plan = ordinary_single_entry_plan();
    BuildPlanDependencyEdge unselected_edge;
    unselected_edge.parent_package_name = "ordinary-set-root";
    unselected_edge.parent_package_base = "ordinary-set-root";
    unselected_edge.dependency_spec = "virtual-unique";
    unselected_edge.role = PackageRole::RuntimeDependency;
    unselected_edge.kind = DependencyKind::AmbiguousProvider;
    unselected_plan.dependency_edges.push_back(unselected_edge);
    unselected_plan.ambiguous_providers.push_back(
        AmbiguousProvidedDependency{
            "virtual-unique",
            {provider,
             make_repository_provider(
                 "extra", "alternate-provider",
                 "virtual-unique", "virtual-unique=1",
                 std::string("1.0-1"))}});
    const std::vector<ProductionSourceBuildWorkItem> unselected_work_items =
        prepare_aur_source_build_work_items(
            unselected_plan, false, false);
    expect(
        unselected_work_items.size() == 1 &&
            unselected_work_items.front()
                .selected_repository_providers.empty(),
        "Unselected repository provider candidate reached preinstall state");

    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider, ProviderResolutionKind::Unique),
            false, false);
    expect(
        work_items.size() == 1 &&
            work_items.front().selected_repository_providers.empty(),
        "Unique repository provider was treated as a user selection");

    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation =
        prepare_execution(std::move(work_items), scenario);
    expect(
        invocation.selected_repository_providers.empty(),
        "Unique repository provider reached invocation preinstall state");

    execute_invocation(invocation, scenario);
    expect(
        scenario.repository_provider_query_calls == 0 &&
            scenario.repository_provider_install_calls == 0,
        "Unique repository provider scheduled a leading transaction");
    require_scenario_complete(
        scenario, 1,
        "unique repository provider execution");
}

void test_same_package_base_projection_preserves_required_children() {
    const std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            same_package_base_plan(), false, false);

    expect(
        work_items.size() == 1,
        "Same-PackageBase children were not aggregated into one work item");
    const ProductionSourceBuildWorkItem& work_item = work_items.front();
    expect(
        work_item.request.package_name.empty(),
        "Multiple required children were flattened into a singular request");
    expect(
        work_item.request.checkout_name == "split-suite",
        "Same-PackageBase work item lost its execution identity");
    expect(
        work_item.required_targets.size() == 2,
        "Same-PackageBase work item lost a required child");
    expect_required_target(
        work_item.required_targets[0], "split-suite", "split-explicit",
        DesiredInstallReason::Explicit,
        "first same-PackageBase required target");
    expect_required_target(
        work_item.required_targets[1], "split-suite", "split-dependency",
        DesiredInstallReason::Dependency,
        "second same-PackageBase required target");
    require_static_production_source_build_work_item(work_item);
    expect_logic_error(
        [&work_item]() {
            static_cast<void>(
                require_singular_required_package_target(work_item));
        },
        "multiple required target compatibility accessor",
        "singular compatibility lifecycle intent");
}

void test_same_package_base_source_preference_route() {
    const ScopedSourcePreferenceEntry preference(
        "split-suite", "MAKEFLAGS=-j7\n");
    const std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            same_package_base_plan(), true, false);

    expect(
        work_items.size() == 1 &&
            work_items.front().required_targets.size() == 2,
        "Source-preference route lost same-PackageBase required targets");
    const ProductionSourceBuildWorkItem& work_item = work_items.front();
    expect(
        work_item.request.package_name.empty() &&
            work_item.request.checkout_name == "split-suite",
        "Source-preference route flattened the multiple-target identity");
    expect(
        work_item.request.custom_environment.ordered_assignments.size() ==
                1 &&
            work_item.request.custom_environment
                    .ordered_assignments.front()
                    .key == "MAKEFLAGS" &&
            work_item.request.custom_environment
                    .ordered_assignments.front()
                    .value == "-j7",
        "Source-preference route did not use the PackageBase preference");
    expect_required_target(
        work_item.required_targets[0], "split-suite", "split-explicit",
        DesiredInstallReason::Explicit,
        "source-preference first required target");
    expect_required_target(
        work_item.required_targets[1], "split-suite", "split-dependency",
        DesiredInstallReason::Dependency,
        "source-preference second required target");
}

void test_resolved_repository_identity_and_owned_environment_preparation() {
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
    query_stub::set_repository_package_response(
        "identity-repository", "core");

    const ResolvedSourceBuildIdentity identity =
        resolve_source_build_identity("identity-repository");
    expect(
        identity.requested_name() == "identity-repository" &&
            identity.package_base() == "identity-repository",
        "Repository source identity lost requested/PackageBase correlation");
    expect(
        identity.canonical_source_key() ==
            "repository:identity-repository",
        "Repository source identity key differs");
    expect(
        identity.git_url() ==
            "https://gitlab.archlinux.org/archlinux/packaging/packages/identity-repository.git",
        "Repository source identity Git URL differs");
    expect(
        identity.source_kind() == SourceBuildSourceKind::Repository &&
            !identity.has_distinct_package_base(),
        "Repository source kind flags differ");

    SourceBuildEnvironment environment;
    environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"CFLAGS", "-O1"});
    const ProductionSourceBuildWorkItem work_item =
        prepare_resolved_source_build_work_item(
            identity, std::move(environment), true, true);
    expect(
        work_item.request.package_name == identity.requested_name() &&
            work_item.request.checkout_name == identity.package_base() &&
            work_item.request.git_url == identity.git_url(),
        "Resolved source identity was not projected to the work item");
    expect(
        work_item.request.custom_environment.ordered_assignments.size() ==
                1 &&
            work_item.request.custom_environment.ordered_assignments[0]
                    .key == "CFLAGS" &&
            work_item.request.custom_environment.ordered_assignments[0]
                    .value == "-O1",
        "Caller-owned strict source environment was not preserved");
    expect(
        work_item.request.only_if_updated && work_item.request.needed &&
            work_item.uses_system_update_baseline &&
            work_item.required_target_provenance ==
                RequiredTargetProvenance::
                    RepositoryExactPackageProjection &&
            work_item.artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::SingularCompatibility &&
            work_item.repository_identity.has_value(),
        "Resolved repository work-item policy differs");
    expect(
        work_item.required_targets.size() == 1,
        "Resolved source work item did not retain one required target");
    expect_required_target(
        require_singular_required_package_target(work_item),
        "identity-repository", "identity-repository",
        DesiredInstallReason::Explicit,
        "resolved source required target");
    expect(
        query_stub::repository_query_count(
            query_stub::RepositoryQueryKind::StrictPackage,
            "identity-repository") == 1,
        "Repository source identity did not use the strict package query once");
    process_stub::require_process_expectations_consumed();
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_resolved_repository_split_identity_and_required_target_projection() {
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
    query_stub::set_repository_package_response(
        "suite-child", "extra", "suite");

    const ResolvedSourceBuildIdentity identity =
        resolve_source_build_identity("suite-child");
    const ResolvedRepositorySourceBuildIdentity* repository =
        identity.repository_identity();
    expect(
        repository != nullptr &&
            repository->requested_child() == "suite-child" &&
            repository->package_base() == "suite" &&
            identity.requested_name() == "suite-child" &&
            identity.package_base() == "suite" &&
            identity.canonical_source_key() == "repository:suite" &&
            identity.git_url() ==
                "https://gitlab.archlinux.org/archlinux/packaging/packages/suite.git",
        "Repository split identity flattened requested child, PackageBase, or checkout");

    const ProductionSourceBuildWorkItem work_item =
        prepare_resolved_source_build_work_item(
            identity, SourceBuildEnvironment{}, true, false);
    expect(
        work_item.required_target_provenance ==
                RequiredTargetProvenance::
                    RepositoryExactPackageProjection &&
            work_item.artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::SingularCompatibility &&
            work_item.repository_identity.has_value() &&
            work_item.request.package_name == "suite-child" &&
            work_item.request.checkout_name == "suite" &&
            work_item.request.only_if_updated &&
            !work_item.request.needed &&
            work_item.uses_system_update_baseline,
        "Repository split work-item policy or identity differs");
    expect_required_target(
        require_singular_required_package_target(work_item),
        "suite", "suite-child", DesiredInstallReason::Explicit,
        "repository split required target");
    expect(
        query_stub::repository_query_count(
            query_stub::RepositoryQueryKind::StrictPackage,
            "suite-child") == 1 &&
            query_stub::aur_query_history().empty() &&
            process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0,
        "Repository split projection used a legacy/AUR query or started mutation");

    process_stub::require_process_expectations_consumed();
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_registered_source_factory_selects_route_owned_lifecycle() {
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();

    const RepositoryPackagePresent exact{
        "extra", 0, "registered-repository-child",
        "registered-repository-base",
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage,
            ARTIFACT_VERSION),
        std::vector<std::string>{"extra"}};
    SourceBuildEnvironment repository_environment;
    repository_environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"CXXFLAGS", "-O2"});
    const ProductionSourceBuildWorkItem repository_work_item =
        prepare_registered_source_build_work_item(
            make_repository_source_build_identity(exact),
            std::move(repository_environment),
            ProviderSelectionCallback{});
    expect(
        repository_work_item.required_target_provenance ==
                RequiredTargetProvenance::
                    RepositoryExactPackageProjection &&
            repository_work_item.artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet &&
            !repository_work_item.request.only_if_updated &&
            !repository_work_item.request.needed &&
            repository_work_item.uses_system_update_baseline &&
            repository_work_item.repository_identity.has_value() &&
            repository_work_item.required_targets.size() == 1,
        "Registered repository factory did not select Set/false/false policy");
    expect_required_target(
        repository_work_item.required_targets.front(),
        "registered-repository-base", "registered-repository-child",
        DesiredInstallReason::Explicit,
        "registered repository target");
    expect(
        repository_work_item.request.custom_environment
                    .ordered_assignments.size() == 1 &&
            repository_work_item.request.custom_environment
                    .ordered_assignments.front()
                    .key ==
                "CXXFLAGS" &&
            repository_work_item.request.custom_environment
                    .ordered_assignments.front()
                    .value ==
                "-O2",
        "Registered repository factory lost the owned environment");

    AurPackageInfo aur_package;
    aur_package.Name = "registered-aur";
    aur_package.PackageBase = "registered-aur";
    aur_package.Version = ARTIFACT_VERSION;
    query_stub::set_repository_package_response(
        "registered-aur", std::nullopt);
    query_stub::set_aur_package_response(
        "registered-aur", aur_package);
    const ProductionSourceBuildWorkItem aur_work_item =
        prepare_registered_source_build_work_item(
            ResolvedSourceBuildIdentity{
                ResolvedAurSourceBuildIdentity{
                    "registered-aur", "registered-aur"}},
            SourceBuildEnvironment{},
            ProviderSelectionCallback{});
    expect(
        aur_work_item.required_target_provenance ==
                RequiredTargetProvenance::AurBuildPlanProjection &&
            aur_work_item.artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::SingularCompatibility &&
            aur_work_item.request.only_if_updated &&
            !aur_work_item.request.needed &&
            !aur_work_item.uses_system_update_baseline &&
            !aur_work_item.repository_identity.has_value(),
        "Registered AUR factory changed singular/true/false policy");
    expect_required_target(
        require_singular_required_package_target(aur_work_item),
        "registered-aur", "registered-aur",
        DesiredInstallReason::Explicit,
        "registered AUR target");

    AurPackageInfo split_package;
    split_package.Name = "registered-aur-split-child";
    split_package.PackageBase = "registered-aur-split-base";
    split_package.Version = ARTIFACT_VERSION;
    query_stub::set_repository_package_response(
        "registered-aur-split-child", std::nullopt);
    query_stub::set_aur_package_response(
        "registered-aur-split-child", split_package);
    static_cast<void>(expect_runtime_error(
        [&]() {
            static_cast<void>(
                prepare_registered_source_build_work_item(
                    ResolvedSourceBuildIdentity{
                        ResolvedAurSourceBuildIdentity{
                            "registered-aur-split-child",
                            "registered-aur-split-base"}},
                    SourceBuildEnvironment{},
                    ProviderSelectionCallback{}));
        },
        "registered AUR split guard", "does not support split"));

    expect(
        process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0,
        "Registered factory route selection started source mutation");
    process_stub::require_process_expectations_consumed();
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_standalone_repository_preparation_uses_package_base_set() {
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
    query_stub::set_repository_package_response(
        "standalone-child", "extra", "standalone-base");
    expect_database_paths();

    SourceBuildEnvironment environment;
    environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"CFLAGS", "-O1"});
    environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"EMPTY_FLAG", ""});
    const AppConfig config = noninteractive_config();
    const RemoteSourceBuildPreparation preparation =
        prepare_remote_source_build(
            "standalone-child", environment, config);
    const PreparedRemoteSourceBuild* prepared =
        std::get_if<PreparedRemoteSourceBuild>(&preparation);
    expect(
        prepared != nullptr && !prepared->aur_build_plan.has_value() &&
            prepared->invocation.work_items.size() == 1,
        "Standalone repository preparation did not retain one exact repository work item");
    const ProductionSourceBuildWorkItem& work_item =
        prepared->invocation.work_items.front();
    expect(
        work_item.required_target_provenance ==
                RequiredTargetProvenance::
                    RepositoryExactPackageProjection &&
            work_item.artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet &&
            work_item.repository_identity.has_value() &&
            work_item.request.package_name == "standalone-child" &&
            work_item.request.checkout_name == "standalone-base" &&
            !work_item.request.only_if_updated &&
            !work_item.request.needed &&
            work_item.uses_system_update_baseline &&
            work_item.selected_repository_providers.empty(),
        "Standalone repository work-item lifecycle or route policy differs");
    expect(
        work_item.request.custom_environment.ordered_assignments.size() ==
                2 &&
            work_item.request.custom_environment
                    .ordered_assignments[0]
                    .key == "CFLAGS" &&
            work_item.request.custom_environment
                    .ordered_assignments[0]
                    .value == "-O1" &&
            work_item.request.custom_environment
                    .ordered_assignments[1]
                    .key == "EMPTY_FLAG" &&
            work_item.request.custom_environment
                .ordered_assignments[1]
                .value.empty() &&
            work_item.request.empty_value_policy ==
                SourceEnvironmentEmptyValuePolicy::Forward,
        "Standalone repository custom environment or empty-value policy differs");
    expect(
        work_item.required_targets.size() == 1,
        "Standalone repository work item did not retain one requested child");
    expect_required_target(
        work_item.required_targets.front(), "standalone-base",
        "standalone-child", DesiredInstallReason::Explicit,
        "standalone repository required target");
    expect(
        query_stub::repository_query_count(
            query_stub::RepositoryQueryKind::StrictPackage,
            "standalone-child") == 1 &&
            query_stub::aur_query_history().empty(),
        "Standalone repository preparation bypassed exact repository authority");

    process_stub::require_process_expectations_consumed();
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_repository_query_failure_stops_before_aur_or_mutation() {
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
    query_stub::set_repository_package_failure(
        "metadata-failure-child", "repository metadata unavailable");
    AurPackageInfo forbidden_aur_fallback;
    forbidden_aur_fallback.Name = "metadata-failure-child";
    forbidden_aur_fallback.PackageBase = "forbidden-aur-base";
    forbidden_aur_fallback.Version = "1.0-1";
    query_stub::set_aur_package_response(
        "metadata-failure-child", forbidden_aur_fallback);

    static_cast<void>(expect_runtime_error(
        []() {
            static_cast<void>(resolve_source_build_identity(
                "metadata-failure-child"));
        },
        "repository metadata failure source resolution",
        "Failed to inspect repository metadata"));
    expect(
        query_stub::repository_query_count(
            query_stub::RepositoryQueryKind::StrictPackage,
            "metadata-failure-child") == 1 &&
            query_stub::aur_query_history().empty() &&
            process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0,
        "Repository metadata failure fell back to AUR or started source mutation");

    process_stub::require_process_expectations_consumed();
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_confirmed_repository_not_found_allows_exact_aur_fallback() {
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
    query_stub::set_repository_package_response(
        "aur-split-child", std::nullopt);
    AurPackageInfo aur_package;
    aur_package.Name = "aur-split-child";
    aur_package.PackageBase = "aur-split-base";
    aur_package.Version = "2.0-1";
    query_stub::set_aur_package_response("aur-split-child", aur_package);

    const ResolvedSourceBuildIdentity identity =
        resolve_source_build_identity("aur-split-child");
    expect(
        identity.source_kind() == SourceBuildSourceKind::Aur &&
            identity.repository_identity() == nullptr &&
            identity.requested_name() == "aur-split-child" &&
            identity.package_base() == "aur-split-base" &&
            identity.canonical_source_key() ==
                "aur:aur-split-base" &&
            query_stub::repository_query_count(
                query_stub::RepositoryQueryKind::StrictPackage,
                "aur-split-child") == 1 &&
            query_stub::aur_query_count(
                query_stub::AurQueryKind::LegacyInfo,
                "aur-split-child") == 1,
        "Confirmed repository NotFound did not route to exact AUR identity");
    expect(
        process_stub::capture_command_call_count() == 0 &&
            process_stub::run_command_call_count() == 0,
        "Identity-only AUR fallback started checkout/build mutation");

    process_stub::require_process_expectations_consumed();
    process_stub::reset_process_stub();
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
}

void test_repository_work_item_static_identity_invariants() {
    const RepositoryPackagePresent exact{
        "extra", 0, "static-child", "static-base",
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage,
            "1.0-1"),
        std::vector<std::string>{"extra"}};
    const ResolvedSourceBuildIdentity identity =
        make_repository_source_build_identity(exact);
    const ProductionSourceBuildWorkItem valid =
        prepare_resolved_source_build_work_item(
            identity, SourceBuildEnvironment{}, true, false);

    ProductionSourceBuildWorkItem updated_only_set = valid;
    updated_only_set.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    static_cast<void>(expect_logic_error(
        [&updated_only_set]() {
            require_static_production_source_build_work_item(
                updated_only_set);
        },
        "repository PackageBase set only-if-updated rejection",
        "only-if-updated"));

    ProductionSourceBuildWorkItem standalone_set = updated_only_set;
    standalone_set.request.only_if_updated = false;
    require_static_production_source_build_work_item(standalone_set);

    ProductionSourceBuildWorkItem empty_child = valid;
    empty_child.request.package_name.clear();
    static_cast<void>(expect_runtime_error(
        [&empty_child]() {
            require_static_production_source_build_work_item(empty_child);
        },
        "empty repository requested child", "Invalid package name"));

    ProductionSourceBuildWorkItem empty_base = valid;
    empty_base.request.checkout_name.clear();
    static_cast<void>(expect_runtime_error(
        [&empty_base]() {
            require_static_production_source_build_work_item(empty_base);
        },
        "empty repository PackageBase", "Invalid package name"));

    ProductionSourceBuildWorkItem invalid_checkout = valid;
    invalid_checkout.request.git_url.clear();
    static_cast<void>(expect_logic_error(
        [&invalid_checkout]() {
            require_static_production_source_build_work_item(
                invalid_checkout);
        },
        "invalid repository checkout identity", "empty Git URL"));

    ProductionSourceBuildWorkItem mismatched_base = valid;
    mismatched_base.required_targets.front().package_base = "other-base";
    static_cast<void>(expect_logic_error(
        [&mismatched_base]() {
            require_static_production_source_build_work_item(
                mismatched_base);
        },
        "repository target PackageBase mismatch",
        "mismatched PackageBase"));

    ProductionSourceBuildWorkItem mismatched_child = valid;
    mismatched_child.required_targets.front().package_name = "other-child";
    static_cast<void>(expect_logic_error(
        [&mismatched_child]() {
            require_static_production_source_build_work_item(
                mismatched_child);
        },
        "repository target child mismatch",
        "does not match its required package target"));

    ProductionSourceBuildWorkItem aur_with_repository_identity = valid;
    aur_with_repository_identity.required_target_provenance =
        RequiredTargetProvenance::AurBuildPlanProjection;
    static_cast<void>(expect_logic_error(
        [&aur_with_repository_identity]() {
            require_static_production_source_build_work_item(
                aur_with_repository_identity);
        },
        "AUR provenance with repository identity",
        "contains a repository identity"));

    ProductionSourceBuildWorkItem missing_repository_identity = valid;
    missing_repository_identity.repository_identity.reset();
    static_cast<void>(expect_logic_error(
        [&missing_repository_identity]() {
            require_static_production_source_build_work_item(
                missing_repository_identity);
        },
        "repository provenance without identity",
        "has no exact repository identity"));

    ProductionSourceBuildWorkItem unsupported = valid;
    unsupported.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::Unspecified;
    static_cast<void>(expect_logic_error(
        [&unsupported]() {
            require_static_production_source_build_work_item(unsupported);
        },
        "unsupported lifecycle intent",
        "no supported artifact lifecycle intent"));
}

void test_aur_work_item_review_identity_invariants() {
    const ProductionSourceBuildWorkItem valid = make_work_item(
        "review-identity-root");
    require_static_production_source_build_work_item(valid);

    ProductionSourceBuildWorkItem missing = valid;
    missing.request.aur_review_identity.reset();
    static_cast<void>(expect_logic_error(
        [&missing]() {
            require_static_production_source_build_work_item(missing);
        },
        "AUR work item without reviewed identity",
        "has no reviewed PackageBase identity"));

    ProductionSourceBuildWorkItem mismatched_remote = valid;
    mismatched_remote.request.aur_review_identity =
        PackageBaseIdentity::make(
            PackageSourceIdentity::aur(
                SourceLocationIdentity::known_git_remote(
                    "https://aur.archlinux.org/other.git")),
            "review-identity-root");
    static_cast<void>(expect_logic_error(
        [&mismatched_remote]() {
            require_static_production_source_build_work_item(
                mismatched_remote);
        },
        "AUR reviewed identity remote mismatch",
        "does not match its reviewed PackageBase identity"));

    ProductionSourceBuildWorkItem mismatched_base = valid;
    mismatched_base.request.aur_review_identity =
        PackageBaseIdentity::make(
            PackageSourceIdentity::aur(
                SourceLocationIdentity::known_git_remote(
                    mismatched_base.request.git_url)),
            "other-base");
    static_cast<void>(expect_logic_error(
        [&mismatched_base]() {
            require_static_production_source_build_work_item(
                mismatched_base);
        },
        "AUR reviewed identity PackageBase mismatch",
        "does not match its reviewed PackageBase identity"));
}

void test_set_static_preparation_accepts_split_and_multiple(
    const TemporaryProductionEnvironment& environment) {
    ProductionScenario scenario;
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.config = noninteractive_config();
    activate_scenario(scenario);
    expect_database_paths();
    const PreflightFilesystemSnapshot before =
        snapshot_preflight_filesystem(
            environment,
            {"split-static-base", "multiple-static-base"},
            "set static preparation acceptance");

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_package_base_work_item(
        "split-static-base",
        {RequiredPackageArtifactTarget{
            "split-static-base", "split-static-child",
            DesiredInstallReason::Explicit}}));
    work_items.push_back(make_package_base_work_item(
        "multiple-static-base",
        {RequiredPackageArtifactTarget{
             "multiple-static-base", "multiple-static-first",
             DesiredInstallReason::Explicit},
         RequiredPackageArtifactTarget{
             "multiple-static-base", "multiple-static-second",
             DesiredInstallReason::Dependency}}));

    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), scenario.config);
    expect(
        invocation.work_items.size() == 2,
        "Set static preparation changed work-item count");
    expect(
        invocation.work_items[0].request.package_name ==
                "split-static-child" &&
            invocation.work_items[0].request.checkout_name ==
                "split-static-base",
        "Set static preparation rejected or rewrote a requested split child");
    expect(
        invocation.work_items[1].request.package_name.empty() &&
            invocation.work_items[1].required_targets.size() == 2,
        "Set static preparation exposed a singular name for multiple children");
    expect(
        scenario.resolver_calls == 1,
        "Set static preparation did not resolve Pacman DB after all validation");
    expect_zero_mutation_state(
        environment, before, scenario,
        "set static preparation acceptance");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void expect_set_static_preparation_rejection(
    const TemporaryProductionEnvironment& environment,
    ProductionSourceBuildWorkItem work_item,
    const std::string& expected_fragment,
    const std::string& context) {
    ProductionScenario scenario;
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.config = noninteractive_config();
    activate_scenario(scenario);
    const PreflightFilesystemSnapshot before =
        snapshot_preflight_filesystem(
            environment,
            {"valid-before-invalid-set",
             work_item.request.checkout_name},
            context);

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("valid-before-invalid-set"));
    work_items.push_back(std::move(work_item));
    static_cast<void>(expect_logic_error(
        [&]() {
            static_cast<void>(prepare_production_source_build_invocation(
                std::move(work_items), scenario.config));
        },
        context, expected_fragment));

    expect(
        scenario.resolver_calls == 0,
        context + ": invalid set reached Pacman DB resolution");
    expect_zero_mutation_state(environment, before, scenario, context);
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_set_static_preparation_rejects_invalid_sets_before_mutation(
    const TemporaryProductionEnvironment& environment) {
    ProductionSourceBuildWorkItem updated_only =
        make_package_base_work_item(
            "updated-only-static-base",
            {RequiredPackageArtifactTarget{
                "updated-only-static-base",
                "updated-only-static-child",
                DesiredInstallReason::Explicit}});
    updated_only.request.only_if_updated = true;
    expect_set_static_preparation_rejection(
        environment, std::move(updated_only), "only-if-updated",
        "PackageBase set only-if-updated static rejection");

    expect_set_static_preparation_rejection(
        environment,
        make_package_base_work_item(
            "duplicate-static-base",
            {RequiredPackageArtifactTarget{
                 "duplicate-static-base", "duplicate-static-child",
                 DesiredInstallReason::Explicit},
             RequiredPackageArtifactTarget{
                 "duplicate-static-base", "duplicate-static-child",
                 DesiredInstallReason::Explicit}}),
        "duplicate required package target",
        "duplicate set static rejection");

    expect_set_static_preparation_rejection(
        environment,
        make_package_base_work_item(
            "mismatch-static-base",
            {RequiredPackageArtifactTarget{
                "other-static-base", "mismatch-static-child",
                DesiredInstallReason::Explicit}}),
        "mismatched PackageBase",
        "PackageBase mismatch static rejection");

    expect_set_static_preparation_rejection(
        environment,
        make_package_base_work_item(
            "unknown-reason-static-base",
            {RequiredPackageArtifactTarget{
                "unknown-reason-static-base",
                "unknown-reason-static-child",
                static_cast<DesiredInstallReason>(999)}}),
        "unknown install reason",
        "unknown reason static rejection");
}

void test_rmdeps_global_rejection(
    const TemporaryProductionEnvironment& environment) {
    ProductionScenario scenario;
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.config = noninteractive_config();
    scenario.config.rm_deps = true;
    activate_scenario(scenario);
    const PreflightFilesystemSnapshot before =
        snapshot_preflight_filesystem(
            environment, {"rmdeps-first", "rmdeps-second"},
            "--rmdeps global preflight");

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("rmdeps-first"));
    work_items.push_back(make_work_item("rmdeps-second"));
    static_cast<void>(expect_runtime_error(
        [&]() {
            static_cast<void>(prepare_production_source_build_invocation(
                std::move(work_items), scenario.config));
        },
        "--rmdeps global preflight", "does not support --rmdeps"));

    expect(
        process_stub::capture_command_call_count() == 0,
        "--rmdeps rejection called the database resolver");
    expect_zero_mutation_state(
        environment, before, scenario, "--rmdeps global preflight");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_inherited_pkgdest_global_rejection(
    const TemporaryProductionEnvironment& environment) {
    ScopedEnvironmentVariable inherited_pkgdest(
        "PKGDEST", std::optional<std::string>(""));
    ProductionScenario scenario;
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.config = noninteractive_config();
    activate_scenario(scenario);
    const PreflightFilesystemSnapshot before =
        snapshot_preflight_filesystem(
            environment, {"inherited-pkgdest"},
            "inherited PKGDEST preflight");

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("inherited-pkgdest"));
    static_cast<void>(expect_runtime_error(
        [&]() {
            static_cast<void>(prepare_production_source_build_invocation(
                std::move(work_items), scenario.config));
        },
        "inherited PKGDEST preflight", "PKGDEST"));

    expect(
        process_stub::capture_command_call_count() == 0,
        "Inherited PKGDEST rejection called the database resolver");
    expect_zero_mutation_state(
        environment, before, scenario, "inherited PKGDEST preflight");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_later_target_pkgdest_global_rejection(
    const TemporaryProductionEnvironment& environment) {
    ProductionScenario scenario;
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.config = noninteractive_config();
    activate_scenario(scenario);
    const PreflightFilesystemSnapshot before =
        snapshot_preflight_filesystem(
            environment, {"valid-first", "invalid-later"},
            "later target PKGDEST preflight");

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("valid-first"));
    ProductionSourceBuildWorkItem invalid_later =
        make_work_item("invalid-later");
    invalid_later.request.custom_environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"PKGDEST", ""});
    work_items.push_back(std::move(invalid_later));

    static_cast<void>(expect_runtime_error(
        [&]() {
            static_cast<void>(prepare_production_source_build_invocation(
                std::move(work_items), scenario.config));
        },
        "later target PKGDEST preflight", "PKGDEST"));

    expect(
        process_stub::capture_command_call_count() == 0,
        "Later-target PKGDEST rejection called the database resolver");
    expect_zero_mutation_state(
        environment, before, scenario,
        "later target PKGDEST preflight");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_database_resolver_failure_stops_all_targets(
    const TemporaryProductionEnvironment& environment) {
    ProductionScenario scenario;
    scenario.caller_working_directory =
        environment.original_working_directory();
    scenario.config = noninteractive_config();
    activate_scenario(scenario);
    expect_database_paths(41);
    const PreflightFilesystemSnapshot before =
        snapshot_preflight_filesystem(
            environment, {"resolver-first", "resolver-second"},
            "database resolver failure");

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("resolver-first"));
    work_items.push_back(make_work_item("resolver-second"));
    static_cast<void>(expect_runtime_error(
        [&]() {
            static_cast<void>(prepare_production_source_build_invocation(
                std::move(work_items), scenario.config));
        },
        "database resolver failure", "pacman-conf failed with exit code 41"));

    expect(
        scenario.resolver_calls == 1 &&
            process_stub::capture_command_call_count() == 1,
        "Database resolver failure did not stop after exactly one call");
    expect_zero_mutation_state(
        environment, before, scenario, "database resolver failure");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_unsafe_existing_cache_root_stops_before_checkout_mutation() {
    TemporaryProductionEnvironment environment;
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_package_base_work_item(
        "unsafe-root-order",
        {RequiredPackageArtifactTarget{
            "unsafe-root-order", "unsafe-root-order",
            DesiredInstallReason::Explicit}}));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    const fs::path cache_root =
        scenario.units.front().checkout_path.parent_path();
    if(chmod(cache_root.c_str(), 0775) != 0) {
        throw std::runtime_error(
            "Failed to make the production cache root unsafe.");
    }

    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), scenario.config);
    static_cast<void>(expect_runtime_error(
        [&]() { execute_invocation(invocation, scenario); },
        "unsafe production cache root ordering",
        "directory permissions are unsafe"));

    expect(
        scenario.resolver_calls == 1,
        "Unsafe production cache root changed invocation preflight order");
    expect(
        scenario.git_remote_calls == 0 &&
            scenario.git_fetch_calls == 0 &&
            scenario.git_branch_calls == 0 &&
            scenario.git_reset_calls == 0,
        "Unsafe production cache root reached Git checkout inspection or mutation");
    expect(
        process_stub::capture_command_call_count() == 1 &&
            process_stub::run_command_call_count() == 0,
        "Unsafe production cache root reached an external checkout mutation");
    expect(
        scenario.workspace_paths.empty() &&
            metadata_stub::initialize_call_count() == 0,
        "Unsafe production cache root reached workspace or metadata work");
    struct stat root_status{};
    expect(
        stat(cache_root.c_str(), &root_status) == 0 &&
            (root_status.st_mode & 07777) == 0775,
        "Unsafe production cache root was silently repaired");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

PreparedProductionSourceBuildInvocation prepare_cache_failure_invocation(
    std::vector<ProductionSourceBuildWorkItem> work_items,
    ProductionScenario& scenario) {
    activate_scenario(scenario);
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), scenario.config);
    activate_production_source_build_cache(invocation);
    expect(
        scenario.resolver_calls == 1 &&
            scenario.workspace_paths.empty(),
        "Cache-failure fixture crossed its preparation boundary");
    process_stub::require_process_expectations_consumed();
    return invocation;
}

void test_singular_cache_failure_preserves_trusted_type() {
    TemporaryProductionEnvironment environment;
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("typed-singular-cache"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation =
        prepare_cache_failure_invocation(
            std::move(work_items), scenario);

    const fs::path cache_root =
        environment.checkout_target_path("typed-singular-cache")
            .parent_path();
    fs::path moved_root = cache_root;
    moved_root += ".typed-singular-replaced";
    fs::rename(cache_root, moved_root);
    fs::create_directory(cache_root);
    fs::permissions(
        cache_root, fs::perms::owner_all,
        fs::perm_options::replace);

    const TrustedCacheFailure failure = expect_trusted_cache_error(
        [&]() {
            static_cast<void>(execute_work_item_typed(
                invocation, 0, scenario));
        },
        "singular production cache replacement");
    expect(
        failure.stage == TrustedCacheStage::RootRevalidation &&
            failure.code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Singular production wrapper changed trusted cache failure detail");
    expect(
        scenario.git_remote_calls == 0 &&
            scenario.git_fetch_calls == 0 &&
            scenario.git_branch_calls == 0 &&
            scenario.git_reset_calls == 0 &&
            scenario.workspace_paths.empty() &&
            metadata_stub::initialize_call_count() == 0 &&
            process_stub::run_command_call_count() == 0,
        "Singular production cache replacement reached source mutation");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_selected_repository_provider_cache_failure_precedes_transaction() {
    TemporaryProductionEnvironment environment;
    const ProvidedDependency provider = make_repository_provider(
        "extra", "cache-guarded-provider", "virtual-cache-guarded",
        "virtual-cache-guarded=1", std::string("1.0-1"));
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            single_repository_provider_plan(
                provider,
                ProviderResolutionKind::UserSelected),
            false, false);
    AppConfig config = noninteractive_config();
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation =
        prepare_cache_failure_invocation(
            std::move(work_items), scenario);

    const ValidatedCacheRoot cache_root = invocation.cache_root.value();
    fs::path moved_root = cache_root.path();
    moved_root += ".provider-transaction-guard-replaced";
    fs::rename(cache_root.path(), moved_root);
    fs::create_directory(cache_root.path());
    fs::permissions(
        cache_root.path(), fs::perms::owner_all,
        fs::perm_options::replace);

    const TrustedCacheFailure failure = expect_trusted_cache_error(
        [&]() { execute_invocation(invocation, scenario); },
        "selected repository provider cache guard");
    expect(
        failure.stage == TrustedCacheStage::RootRevalidation &&
            failure.code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Selected provider cache guard changed trusted failure detail");
    expect(
        scenario.repository_provider_install_calls == 0 &&
            scenario.git_remote_calls == 0 &&
            scenario.workspace_paths.empty() &&
            process_stub::run_command_call_count() == 0,
        "Cache failure reached provider transaction or source mutation");
    process_stub::require_process_expectations_consumed();
    deactivate_scenario();
}

void test_package_base_cache_failures_preserve_trusted_type() {
    {
        TemporaryProductionEnvironment environment;
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(make_package_base_work_item(
            "typed-package-base-root",
            {RequiredPackageArtifactTarget{
                "typed-package-base-root",
                "typed-package-base-root",
                DesiredInstallReason::Explicit}}));
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        PreparedProductionSourceBuildInvocation invocation =
            prepare_cache_failure_invocation(
                std::move(work_items), scenario);
        const ValidatedCacheRoot cache_root =
            invocation.cache_root.value();
        fs::path moved_root = cache_root.path();
        moved_root += ".typed-package-base-replaced";
        fs::rename(cache_root.path(), moved_root);
        fs::create_directory(cache_root.path());
        fs::permissions(
            cache_root.path(), fs::perms::owner_all,
            fs::perm_options::replace);

        const ProductionSourceBuildWorkItem& work_item =
            invocation.work_items.front();
        const TrustedCacheFailure failure = expect_trusted_cache_error(
            [&]() {
                static_cast<void>(execute_source_build_package_base_typed(
                    work_item.request, work_item.required_targets,
                    cache_root, invocation.database_paths, config));
            },
            "PackageBase private-root cache replacement");
        expect(
            failure.stage == TrustedCacheStage::RootRevalidation &&
                failure.code ==
                    TrustedCacheErrorCode::ConcurrentReplacement,
            "PackageBase private-root wrapper changed trusted cache failure detail");
        expect(
            scenario.git_remote_calls == 0 &&
                scenario.git_fetch_calls == 0 &&
                scenario.git_branch_calls == 0 &&
                scenario.git_reset_calls == 0 &&
                scenario.workspace_paths.empty() &&
                metadata_stub::initialize_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "PackageBase private-root failure reached source mutation");
        process_stub::require_process_expectations_consumed();
        deactivate_scenario();
    }

    {
        TemporaryProductionEnvironment environment;
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(make_package_base_work_item(
            "typed-package-base-checkout",
            {RequiredPackageArtifactTarget{
                "typed-package-base-checkout",
                "typed-package-base-checkout",
                DesiredInstallReason::Explicit}}));
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        PreparedProductionSourceBuildInvocation invocation =
            prepare_cache_failure_invocation(
                std::move(work_items), scenario);

        const fs::path checkout = environment.checkout_target_path(
            "typed-package-base-checkout");
        fs::path moved_checkout = checkout;
        moved_checkout += ".typed-original";
        fs::rename(checkout, moved_checkout);
        fs::create_directory_symlink(moved_checkout, checkout);

        const TrustedCacheFailure failure = expect_trusted_cache_error(
            [&]() {
                static_cast<void>(execute_package_base_work_item_typed(
                    invocation, 0, scenario));
            },
            "PackageBase checkout symlink");
        expect(
            failure.stage == TrustedCacheStage::ChildValidation &&
                failure.code == TrustedCacheErrorCode::Symlink,
            "PackageBase checkout wrapper changed trusted cache failure detail");
        expect(
            scenario.git_remote_calls == 0 &&
                scenario.git_fetch_calls == 0 &&
                scenario.git_branch_calls == 0 &&
                scenario.git_reset_calls == 0 &&
                scenario.workspace_paths.empty() &&
                metadata_stub::initialize_call_count() == 0 &&
                process_stub::run_command_call_count() == 0,
            "PackageBase checkout cache failure reached source mutation");
        process_stub::require_process_expectations_consumed();
        deactivate_scenario();
    }
}

void expect_single_work_item_outcome(
    const TemporaryProductionEnvironment& environment,
    const std::string& package_name,
    bool needed,
    MetadataMode metadata_mode,
    ArtifactInstallExecutionOutcome expected_outcome,
    const std::string& context) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item(package_name));
    work_items[0].request.needed = needed;
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].metadata_mode = metadata_mode;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const std::optional<ArtifactInstallExecutionOutcome> outcome =
        execute_work_item(invocation, 0, scenario);

    expect(outcome.has_value(), context + ": typed outcome was omitted");
    expect(
        *outcome == expected_outcome,
        context + ": typed outcome differs");
    expect(
        scenario.install_attempt_order ==
            std::vector<std::string>{package_name},
        context + ": successful pacman -U was not observed");
    expect(
        !fs::exists(scenario.workspace_paths.at(0)),
        context + ": successful execution retained its workspace");
    require_scenario_complete(scenario, 1, context);
}

void test_work_item_typed_install_outcomes(
    const TemporaryProductionEnvironment& environment) {
    expect_single_work_item_outcome(
        environment, "outcome-needed-false", false,
        MetadataMode::Absent,
        ArtifactInstallExecutionOutcome::Installed,
        "needed=false install outcome");
    expect_single_work_item_outcome(
        environment, "outcome-needed-different", true,
        MetadataMode::ExistingExplicitDifferentVersion,
        ArtifactInstallExecutionOutcome::Installed,
        "different-version --needed install outcome");
    expect_single_work_item_outcome(
        environment, "outcome-needed-same", true,
        MetadataMode::ExistingExplicitSameVersion,
        ArtifactInstallExecutionOutcome::SkippedAsNeeded,
        "same-version --needed install outcome");
}

void expect_extended_work_item_outcome(
    const TemporaryProductionEnvironment& environment,
    const std::string& package_name,
    bool needed,
    MetadataMode metadata_mode,
    SourceBuildExecutionStatus expected_status,
    const std::string& context) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item(package_name));
    work_items[0].request.needed = needed;
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].metadata_mode = metadata_mode;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const SourceBuildExecutionResult result =
        execute_work_item_typed(invocation, 0, scenario);

    expect(result.status == expected_status, context + ": status differs");
    expect(
        !result.update_status_unknown_skip_reason.has_value(),
        context + ": unexpected update-status skip reason");
    expect(result.diagnostic.empty(), context + ": unexpected diagnostic");
    expect(
        result.production_outcome.has_value() &&
            result.production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            result.production_outcome->source_provenance.editor_overlay ==
                ReviewedSourceEditorOverlayStatus::None &&
            result.production_outcome->source_provenance.compatibility_reason ==
                std::optional<
                    ReviewedSourceCompatibilityBuildReason>{
                    ReviewedSourceCompatibilityBuildReason::
                        NoDiff} &&
            result.production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            result.production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Succeeded,
        context + ": review/build provenance was flattened");
    expect(
        scenario.install_attempt_order ==
            std::vector<std::string>{package_name},
        context + ": successful pacman -U was not observed");
    require_scenario_complete(scenario, 1, context);
}

void test_extended_work_item_install_outcomes(
    const TemporaryProductionEnvironment& environment) {
    expect_extended_work_item_outcome(
        environment, "extended-installed", false,
        MetadataMode::Absent,
        SourceBuildExecutionStatus::Installed,
        "extended installed outcome");
    expect_extended_work_item_outcome(
        environment, "extended-needed-skip", true,
        MetadataMode::ExistingExplicitSameVersion,
        SourceBuildExecutionStatus::SkippedAsNeeded,
        "extended --needed skip outcome");
}

void expect_review_bypass_provenance(
    const TemporaryProductionEnvironment& environment,
    const std::string& package_name,
    AppConfig config,
    ReviewedSourceCompatibilityBuildReason expected_reason,
    const std::string& context) {
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item(package_name));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const SourceBuildExecutionResult result =
        execute_work_item_typed(invocation, 0, scenario);
    expect(
        result.status == SourceBuildExecutionStatus::Installed &&
            result.production_outcome.has_value() &&
            result.production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            result.production_outcome->source_provenance.compatibility_reason ==
                std::optional<
                    ReviewedSourceCompatibilityBuildReason>{
                    expected_reason},
        context + ": review bypass provenance differs");
    require_scenario_complete(scenario, 1, context);
}

void test_review_bypass_provenance(
    const TemporaryProductionEnvironment& environment) {
    AppConfig no_confirm = noninteractive_config();
    no_confirm.user_config.review.diff = ReviewPolicy::Prompt;
    no_confirm.no_confirm = true;
    expect_review_bypass_provenance(
        environment, "review-bypass-no-confirm", no_confirm,
        ReviewedSourceCompatibilityBuildReason::NoConfirm,
        "--noconfirm review bypass");

    AppConfig non_tty = noninteractive_config();
    non_tty.user_config.review.diff = ReviewPolicy::Prompt;
    {
        const ScopedStdinReplacement stdin_fixture =
            ScopedStdinReplacement::noninteractive();
        static_cast<void>(stdin_fixture);
        expect(
            isatty(STDIN_FILENO) != 1,
            "Non-TTY review bypass fixture unexpectedly exposed a TTY");
        expect_review_bypass_provenance(
            environment, "review-bypass-non-tty", non_tty,
            ReviewedSourceCompatibilityBuildReason::NonInteractiveInput,
            "non-TTY review bypass");
    }
}

enum class FatalPreflightFixtureKind {
    UnsupportedFuture,
    UnsafeHistory,
    StoreFailure,
};

ReviewedSourceStateStoreReadResult fatal_preflight_fixture(
    FatalPreflightFixtureKind kind,
    const PackageBaseIdentity& package_base) {
    switch(kind) {
        case FatalPreflightFixtureKind::UnsupportedFuture: {
            const std::string document =
                "schema_version = 2\nfuture_field = true\n";
            ReviewedSourceStateObservation observation = std::visit(
                [](const auto& value) -> ReviewedSourceStateObservation {
                    return value;
                },
                interpret_reviewed_source_state(document, package_base));
            return ReviewedSourceStateStoreRead{
                std::move(observation),
                ReviewedSourceStateObservedRecord{
                    1, "1.toml",
                    ReviewedSourceStateRecordIdentity{
                        1, 2, 3, 0600, 1, 42, 4, 5, 6, 7},
                    document}};
        }
        case FatalPreflightFixtureKind::UnsafeHistory:
            return ReviewedSourceStateStoreUnsafeHistory{
                ReviewedSourceStateStoreHistoryIssue::ForkDetected,
                "/state/fatal-package",
                {"1.toml", "2-a.toml", "2-b.toml"},
                1,
                2};
        case FatalPreflightFixtureKind::StoreFailure:
            return ReviewedSourceStateStoreFailure{
                ReviewedSourceStateStoreFailureKind::ReadFailed,
                "/state/fatal-package/1.toml", std::nullopt,
                std::nullopt, std::nullopt};
    }
    throw std::logic_error("Unknown fatal preflight fixture kind");
}

ReviewedSourceProductionFailureReason fatal_preflight_expected_reason(
    FatalPreflightFixtureKind kind) {
    switch(kind) {
        case FatalPreflightFixtureKind::UnsupportedFuture:
            return ReviewedSourceProductionFailureReason::UnsupportedFuture;
        case FatalPreflightFixtureKind::UnsafeHistory:
            return ReviewedSourceProductionFailureReason::UnsafeHistory;
        case FatalPreflightFixtureKind::StoreFailure:
            return ReviewedSourceProductionFailureReason::StateStoreFailure;
    }
    throw std::logic_error("Unknown fatal preflight expected reason");
}

void test_invocation_preflight_snapshot_is_consumed_without_reread(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items{
        make_work_item("stable-preflight-snapshot")};
    const PackageBaseIdentity package_base =
        *work_items.front().request.aur_review_identity;
    ProductionScenario scenario = make_execution_scenario(
        environment, work_items, config);
    set_reviewed_source_lifecycle_store_result_for_test(
        ReviewedSourceStateStoreRead{
            ReviewedSourceStateMissing{}, std::nullopt});
    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);

    // This later fatal result must remain pending: execution consumes the
    // invocation-owned Missing observation instead of reading the store again.
    set_reviewed_source_lifecycle_store_result_for_test(
        fatal_preflight_fixture(
            FatalPreflightFixtureKind::UnsupportedFuture,
            package_base));
    const SourceBuildExecutionResult result = execute_work_item_typed(
        invocation, 0, scenario);
    expect(result.status == SourceBuildExecutionStatus::Installed,
           "Invocation preflight snapshot was replaced before execution");
    try {
        static_cast<void>(
            preflight_reviewed_source_fatal_state_for_production(
                invocation.work_items.front().request));
        throw std::runtime_error(
            "Later fatal observation was consumed by work-item execution");
    } catch(const ReviewedSourceProductionError& error) {
        expect(error.failure().reason ==
                   ReviewedSourceProductionFailureReason::
                       UnsupportedFuture,
               "Pending fatal observation lost its typed identity");
    }
    require_scenario_complete(
        scenario, 1, "invocation preflight snapshot propagation");
}

void test_local_dependency_preflight_snapshot_is_consumed_without_reread(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items{
        make_work_item("local-stable-preflight-snapshot")};
    const PackageBaseIdentity package_base =
        *work_items.front().request.aur_review_identity;
    ProductionScenario scenario = make_execution_scenario(
        environment, work_items, config);
    activate_scenario(scenario);

    LocalSourceBuildDependencyPreparation preparation =
        LocalSourceBuildDependencyPreparation::
            make_for_production_source_build_test(
                std::move(work_items), {});
    set_reviewed_source_lifecycle_store_result_for_test(
        ReviewedSourceStateStoreRead{
            ReviewedSourceStateMissing{}, std::nullopt});
    preflight_local_source_build_dependencies(preparation, config);

    // Cache-bound preparation and execution must retain the pre-cache Missing
    // observation. This later fatal result must remain pending.
    set_reviewed_source_lifecycle_store_result_for_test(
        fatal_preflight_fixture(
            FatalPreflightFixtureKind::UnsupportedFuture,
            package_base));
    const ValidatedCacheRoot cache_root = prepare_process_cache_root();
    expect_database_paths();
    PreparedProductionSourceBuildInvocation invocation =
        prepare_local_source_build_dependency_invocation(
            std::move(preparation), cache_root, config);
    expect(
        scenario.resolver_calls == 1,
        "Local dependency cache-bound preparation did not resolve the database snapshot once");
    schedule_source_unit(0);

    const SourceBuildExecutionResult result = execute_work_item_typed(
        invocation, 0, scenario);
    expect(
        result.status == SourceBuildExecutionStatus::Installed,
        "Local dependency preflight snapshot was replaced before execution");
    try {
        static_cast<void>(
            preflight_reviewed_source_fatal_state_for_production(
                invocation.work_items.front().request));
        throw std::runtime_error(
            "Later local dependency fatal observation was consumed by execution");
    } catch(const ReviewedSourceProductionError& error) {
        expect(
            error.failure().reason ==
                ReviewedSourceProductionFailureReason::
                    UnsupportedFuture,
            "Pending local dependency fatal observation lost its typed identity");
    }
    require_scenario_complete(
        scenario, 1,
        "local dependency preflight snapshot propagation");
}

void test_invocation_wide_fatal_preflight_stops_before_cache_provider_and_work_items() {
    TemporaryProductionEnvironment environment(false);
    const fs::path missing_cache_root =
        environment.checkout_target_path("safe-before-fatal").parent_path();
    expect(!fs::exists(missing_cache_root),
           "Invocation-wide fatal preflight fixture created its cache root");

    std::vector<ProductionSourceBuildWorkItem> work_items{
        make_work_item("safe-before-fatal"),
        make_work_item("fatal-later-target")};
    work_items.front().selected_repository_providers.push_back(
        make_repository_provider(
            "extra", "fatal-preflight-provider",
            "virtual-fatal-preflight",
            "virtual-fatal-preflight=1", std::string("1.0-1")));
    const PackageBaseIdentity safe_identity =
        *work_items.front().request.aur_review_identity;
    const PackageBaseIdentity fatal_identity =
        *work_items.back().request.aur_review_identity;
    set_reviewed_source_lifecycle_store_result_for_test(
        ReviewedSourceStateStoreRead{
            ReviewedSourceStateMissing{}, std::nullopt});
    set_reviewed_source_lifecycle_store_result_for_test(
        fatal_preflight_fixture(
            FatalPreflightFixtureKind::UnsupportedFuture,
            fatal_identity));
    process_stub::reset_process_stub();
    metadata_stub::reset_alpm_stub();

    try {
        static_cast<void>(prepare_production_source_build_invocation(
            std::move(work_items), noninteractive_config()));
        throw std::runtime_error(
            "Later fatal target produced a production invocation");
    } catch(const ReviewedSourceProductionError& error) {
        expect(error.failure().stage ==
                       ReviewedSourceProductionFailureStage::
                           FatalStatePreflight &&
                   error.failure().reason ==
                       ReviewedSourceProductionFailureReason::
                           UnsupportedFuture,
               "Invocation-wide fatal target lost typed classification");
    }

    expect(!fs::exists(missing_cache_root) &&
               !fs::exists(
                   reviewed_source_state_store_entry_path(
                       safe_identity)) &&
               !fs::exists(
                   reviewed_source_state_store_entry_path(
                       fatal_identity)) &&
               process_stub::run_command_call_count() == 0 &&
               process_stub::capture_command_call_count() == 0 &&
               metadata_stub::initialize_call_count() == 0,
           "Fatal preflight reached cache/provider/work-item mutation");
}

void test_all_compatibility_modes_observe_fatal_reviewed_state(
    const TemporaryProductionEnvironment& environment) {
    struct ModeCase {
        std::string name;
        AppConfig config;
    };

    AppConfig no_diff = noninteractive_config();
    AppConfig no_confirm = noninteractive_config();
    no_confirm.user_config.review.diff = ReviewPolicy::Prompt;
    no_confirm.no_confirm = true;
    AppConfig non_tty = noninteractive_config();
    non_tty.user_config.review.diff = ReviewPolicy::Prompt;

    const std::vector<ModeCase> modes{
        {"nodiff", no_diff},
        {"noconfirm", no_confirm},
        {"non-tty", non_tty},
    };
    const std::vector<FatalPreflightFixtureKind> fatal_kinds{
        FatalPreflightFixtureKind::UnsupportedFuture,
        FatalPreflightFixtureKind::UnsafeHistory,
        FatalPreflightFixtureKind::StoreFailure,
    };

    for(const ModeCase& mode : modes) {
        for(std::size_t fatal_index = 0;
            fatal_index < fatal_kinds.size(); ++fatal_index) {
            const FatalPreflightFixtureKind fatal_kind =
                fatal_kinds[fatal_index];
            const std::string package_name =
                "fatal-" + mode.name + "-" +
                std::to_string(fatal_index);
            std::vector<ProductionSourceBuildWorkItem> work_items{
                make_work_item(package_name)};
            const PackageBaseIdentity package_base =
                *work_items.front().request.aur_review_identity;
            ProductionScenario scenario = make_execution_scenario(
                environment, work_items, mode.config);
            const fs::path state_entry =
                reviewed_source_state_store_entry_path(package_base);
            expect(!fs::exists(state_entry),
                   "Fatal preflight fixture started with reviewed state");
            activate_scenario(scenario);
            set_reviewed_source_lifecycle_store_result_for_test(
                fatal_preflight_fixture(fatal_kind, package_base));

            try {
                static_cast<void>(prepare_production_source_build_invocation(
                    std::move(work_items), mode.config));
                throw std::runtime_error(
                    "Fatal reviewed state produced a compatibility invocation");
            } catch(const ReviewedSourceProductionError& error) {
                expect(
                    error.failure().stage ==
                            ReviewedSourceProductionFailureStage::
                                FatalStatePreflight &&
                        error.failure().reason ==
                            fatal_preflight_expected_reason(
                                fatal_kind),
                    "Fatal reviewed state lost typed production classification");
                const auto* detail = std::get_if<
                    ReviewedSourceFatalStateFailure>(
                    &error.failure().detail);
                expect(detail != nullptr,
                       "Fatal reviewed state lost its store payload");
            }
            expect(
                scenario.git_reset_calls == 0 &&
                    scenario.workspace_paths.empty() &&
                    scenario.packagelist_calls == 0 &&
                    scenario.build_calls == 0 &&
                    scenario.install_calls == 0 &&
                    !fs::exists(state_entry),
                "Fatal reviewed state reached reset/editor/makepkg/CAS");
            deactivate_scenario();
        }
    }
}

void test_reviewed_source_failure_payload_crosses_singular_and_set_routes(
    const TemporaryProductionEnvironment& environment) {
    {
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items{
            make_work_item("typed-lease-contention")};
        ProductionScenario scenario = make_execution_scenario(
            environment, work_items, config);
        PreparedProductionSourceBuildInvocation invocation =
            prepare_execution(std::move(work_items), scenario);
        ReviewedSourcePackageBaseLease held =
            acquire_reviewed_source_package_base_lease(
                retain_trusted_cache_directory(
                    require_trusted_cache_path(
                        invocation.work_items.front()
                            .cache_root.value(),
                        invocation.work_items.front()
                            .request.checkout_name,
                        CachePathRequirement::
                            ExistingDirectory)));
        static_cast<void>(held);
        try {
            static_cast<void>(execute_work_item_typed(
                invocation, 0, scenario));
            throw std::runtime_error(
                "Lease contention was flattened into source execution");
        } catch(const ReviewedSourceProductionError& error) {
            expect(error.failure().reason ==
                       ReviewedSourceProductionFailureReason::
                           LeaseContended,
                   "Singular route lost LeaseContended classification");
        }
        expect(scenario.git_reset_calls == 0 &&
                   scenario.workspace_paths.empty() &&
                   scenario.build_calls == 0,
               "Lease contention reached source mutation");
        deactivate_scenario();
    }

    {
        AppConfig config = noninteractive_config();
        const std::string package_base_name = "typed-unsafe-set";
        std::vector<ProductionSourceBuildWorkItem> work_items{
            make_package_base_work_item(
                package_base_name,
                {RequiredPackageArtifactTarget{
                    package_base_name, package_base_name,
                    DesiredInstallReason::Explicit}})};
        const PackageBaseIdentity package_base =
            *work_items.front().request.aur_review_identity;
        ProductionScenario scenario = make_execution_scenario(
            environment, work_items, config);
        activate_scenario(scenario);
        set_reviewed_source_lifecycle_store_result_for_test(
            fatal_preflight_fixture(
                FatalPreflightFixtureKind::UnsafeHistory,
                package_base));
        try {
            static_cast<void>(prepare_production_source_build_invocation(
                std::move(work_items), config));
            throw std::runtime_error(
                "PackageBase unsafe history was flattened");
        } catch(const ReviewedSourceProductionError& error) {
            expect(error.failure().reason ==
                           ReviewedSourceProductionFailureReason::
                               UnsafeHistory &&
                       std::holds_alternative<
                           ReviewedSourceFatalStateFailure>(
                           error.failure().detail),
                   "PackageBase route lost reviewed source failure payload");
        }
        expect(scenario.git_reset_calls == 0 &&
                   scenario.workspace_paths.empty() &&
                   scenario.build_calls == 0,
               "PackageBase unsafe history reached source mutation");
        deactivate_scenario();
    }
}

void test_registered_repository_closed_preparation_outcomes(
    const TemporaryProductionEnvironment& environment) {
    {
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(
            make_registered_repository_package_base_work_item(
                "registered-up-to-date-base",
                "registered-up-to-date-child"));
        work_items.front().request.installed_snapshot =
            SourceInstalledSnapshot{ARTIFACT_VERSION};
        work_items.front().request.update_baseline =
            SourceUpdateBaseline{ARTIFACT_VERSION};
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        write_file(
            scenario.units.front().checkout_path / ".SRCINFO",
            "pkgver = 1.0\npkgrel = 1\n");
        fs::permissions(
            scenario.units.front().checkout_path / ".SRCINFO",
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);
        PreparedProductionSourceBuildInvocation invocation =
            prepare_execution(std::move(work_items), scenario);
        expect_version_comparison(
            ARTIFACT_VERSION, ARTIFACT_VERSION, "0");

        SourceBuildPreparationOutcome outcome =
            prepare_package_base_source_build_work_item_typed(
                invocation.work_items.front(),
                SourceBuildUpdatePolicy::OnlyIfUpdated,
                config);
        const SourceBuildUpToDate* up_to_date =
            std::get_if<SourceBuildUpToDate>(&outcome);
        expect(
            up_to_date != nullptr &&
                up_to_date->diagnostic ==
                    "registered-up-to-date-child is up to date (1.0-1). Skipping.",
            "Registered preparation did not return the closed UpToDate outcome");
        expect(
            scenario.workspace_paths.empty() &&
                scenario.install_attempt_order.empty(),
            "Registered UpToDate preparation reached build/install");
        require_scenario_complete(
            scenario, 0, "registered closed UpToDate outcome");
    }

    {
        AppConfig config = noninteractive_config();
        config.no_confirm = true;
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(
            make_registered_repository_package_base_work_item(
                "registered-unknown-base",
                "registered-unknown-child"));
        work_items.front().request.installed_snapshot =
            SourceInstalledSnapshot{ARTIFACT_VERSION};
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        PreparedProductionSourceBuildInvocation invocation =
            prepare_execution(std::move(work_items), scenario);

        SourceBuildPreparationOutcome outcome =
            prepare_package_base_source_build_work_item_typed(
                invocation.work_items.front(),
                SourceBuildUpdatePolicy::OnlyIfUpdated,
                config);
        const auto* skipped =
            std::get_if<SourceBuildUpdateStatusUnknownSkipped>(&outcome);
        expect(
            skipped != nullptr &&
                skipped->reason ==
                    SourceBuildUpdateStatusUnknownSkipReason::
                        NoConfirm &&
                skipped->diagnostic ==
                    "Skipping registered-unknown-child: update status is unknown and --noconfirm is set.",
            "Registered preparation did not retain the closed unknown-status outcome");
        expect(
            scenario.workspace_paths.empty() &&
                scenario.install_attempt_order.empty(),
            "Registered unknown-status preparation reached build/install");
        require_scenario_complete(
            scenario, 0,
            "registered closed unknown-status outcome");
    }

    {
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(
            make_registered_repository_package_base_work_item(
                "registered-needs-build-base",
                "registered-needs-build-child"));
        work_items.front().request.installed_snapshot =
            SourceInstalledSnapshot{std::nullopt};
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        PreparedProductionSourceBuildInvocation invocation =
            prepare_execution(std::move(work_items), scenario);

        SourceBuildPreparationOutcome outcome =
            prepare_package_base_source_build_work_item_typed(
                invocation.work_items.front(),
                SourceBuildUpdatePolicy::OnlyIfUpdated,
                config);
        expect(
            std::holds_alternative<PreparedSourceBuildNeedsBuild>(outcome),
            "Registered preparation did not return the one-shot NeedsBuild capability");
        const std::size_t remote_calls = scenario.git_remote_calls;
        const std::size_t fetch_calls = scenario.git_fetch_calls;
        const std::size_t branch_calls = scenario.git_branch_calls;
        const std::size_t reset_calls = scenario.git_reset_calls;

        const PackageBaseSourceBuildExecutionResult result =
            execute_prepared_package_base_source_build_work_item_typed(
                invocation.work_items.front(),
                std::move(std::get<PreparedSourceBuildNeedsBuild>(
                    outcome)),
                invocation.database_paths,
                config);
        expect(
            result.package_base() == "registered-needs-build-base" &&
                result.source_provenance().review_status ==
                    ProductionSourceReviewStatus::NotApplicable &&
                result.build_outcome() ==
                    ProductionSourceBuildCommandOutcome::
                        Succeeded &&
                result.selected_children().size() == 1 &&
                result.selected_children().front().identity.package_name ==
                    "registered-needs-build-child" &&
                result.selected_children().front().outcome ==
                    ArtifactInstallExecutionOutcome::Installed,
            "Prepared registered PackageBase execution lost its aggregate");
        expect(
            scenario.git_remote_calls == remote_calls &&
                scenario.git_fetch_calls == fetch_calls &&
                scenario.git_branch_calls == branch_calls &&
                scenario.git_reset_calls == reset_calls &&
                remote_calls == 1 && fetch_calls == 1 &&
                branch_calls == 1 && reset_calls == 1,
            "Prepared PackageBase executor repeated checkout/update preparation");
        expect(
            scenario.install_attempt_order ==
                std::vector<std::string>{
                    "registered-needs-build-base"},
            "Prepared registered PackageBase execution did not install the requested child once");
        require_scenario_complete(
            scenario, 1,
            "registered closed NeedsBuild outcome");
    }
}

void test_up_to_date_outcome_and_legacy_flattening(
    const TemporaryProductionEnvironment& environment) {
    {
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(make_update_check_work_item(
            "typed-up-to-date", ARTIFACT_VERSION));
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        write_file(
            scenario.units[0].checkout_path / ".SRCINFO",
            "pkgver = 1.0\npkgrel = 1\n");
        fs::permissions(
            scenario.units[0].checkout_path / ".SRCINFO",
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);

        PreparedProductionSourceBuildInvocation invocation = prepare_execution(
            std::move(work_items), scenario);
        expect_version_comparison(
            ARTIFACT_VERSION, ARTIFACT_VERSION, "0");
        const SourceBuildExecutionResult result =
            execute_work_item_typed(invocation, 0, scenario);

        expect(
            result.status == SourceBuildExecutionStatus::UpToDate,
            "Up-to-date source result was flattened");
        expect(
            !result.update_status_unknown_skip_reason.has_value(),
            "Up-to-date source result acquired an unknown-status reason");
        expect(
            result.diagnostic ==
                "typed-up-to-date is up to date (1.0-1). Skipping.",
            "Up-to-date source diagnostic differs");
        expect(
            scenario.install_attempt_order.empty(),
            "Up-to-date source result reached package installation");
        require_scenario_complete(
            scenario, 0, "typed up-to-date source result");
    }

    {
        AppConfig config = noninteractive_config();
        std::vector<ProductionSourceBuildWorkItem> work_items;
        work_items.push_back(make_update_check_work_item(
            "legacy-up-to-date", ARTIFACT_VERSION));
        ProductionScenario scenario =
            make_execution_scenario(environment, work_items, config);
        write_file(
            scenario.units[0].checkout_path / ".SRCINFO",
            "pkgver = 1.0\npkgrel = 1\n");
        fs::permissions(
            scenario.units[0].checkout_path / ".SRCINFO",
            fs::perms::owner_read | fs::perms::owner_write,
            fs::perm_options::replace);

        PreparedProductionSourceBuildInvocation invocation = prepare_execution(
            std::move(work_items), scenario);
        expect_version_comparison(
            ARTIFACT_VERSION, ARTIFACT_VERSION, "0");
        const std::optional<ArtifactInstallExecutionOutcome> outcome =
            execute_work_item(invocation, 0, scenario);

        expect(
            !outcome.has_value(),
            "Legacy source-build API did not flatten up-to-date to nullopt");
        require_scenario_complete(
            scenario, 0, "legacy up-to-date flattening");
    }
}

void test_unknown_update_status_no_confirm_outcome(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    config.no_confirm = true;
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_update_check_work_item(
        "unknown-no-confirm", ARTIFACT_VERSION));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);

    const SourceBuildExecutionResult result =
        execute_work_item_typed(invocation, 0, scenario);
    expect(
        result.status ==
                SourceBuildExecutionStatus::UpdateStatusUnknownSkipped &&
            result.update_status_unknown_skip_reason ==
                SourceBuildUpdateStatusUnknownSkipReason::NoConfirm,
        "Unknown update status --noconfirm reason differs");
    expect(
        result.diagnostic ==
            "Skipping unknown-no-confirm: update status is unknown and --noconfirm is set.",
        "Unknown update status --noconfirm diagnostic differs");
    expect(
        scenario.install_attempt_order.empty(),
        "Unknown update status --noconfirm skip reached installation");
    require_scenario_complete(
        scenario, 0, "unknown update status --noconfirm skip");
}

void test_unknown_update_status_noninteractive_outcome(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_update_check_work_item(
        "unknown-noninteractive", ARTIFACT_VERSION));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);

    const ScopedStdinReplacement stdin_fixture =
        ScopedStdinReplacement::noninteractive();
    static_cast<void>(stdin_fixture);
    const SourceBuildExecutionResult result =
        execute_work_item_typed(invocation, 0, scenario);
    expect(
        result.status ==
                SourceBuildExecutionStatus::UpdateStatusUnknownSkipped &&
            result.update_status_unknown_skip_reason ==
                SourceBuildUpdateStatusUnknownSkipReason::
                    NonInteractiveStdin,
        "Unknown update status non-interactive reason differs");
    expect(
        result.diagnostic ==
            "Skipping unknown-noninteractive: update status is unknown and stdin is non-interactive.",
        "Unknown update status non-interactive diagnostic differs");
    require_scenario_complete(
        scenario, 0, "unknown update status non-interactive skip");
}

void test_unknown_update_status_user_decline_outcome(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_update_check_work_item(
        "unknown-declined", ARTIFACT_VERSION));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);

    const ScopedStdinReplacement stdin_fixture =
        ScopedStdinReplacement::terminal_with_input("n\n");
    static_cast<void>(stdin_fixture);
    const SourceBuildExecutionResult result =
        execute_work_item_typed(invocation, 0, scenario);
    expect(
        result.status ==
                SourceBuildExecutionStatus::UpdateStatusUnknownSkipped &&
            result.update_status_unknown_skip_reason ==
                SourceBuildUpdateStatusUnknownSkipReason::UserDeclined,
        "Unknown update status user-decline reason differs");
    expect(
        result.diagnostic ==
            "Skipping unknown-declined: update status is unknown and the user declined to continue.",
        "Unknown update status user-decline diagnostic differs");
    require_scenario_complete(
        scenario, 0, "unknown update status user-decline skip");
}

void expect_selected_child_result(
    const PackageBaseSourceBuildSelectedResult& child,
    const std::string& package_name,
    const std::string& full_version,
    DesiredInstallReason desired_reason,
    ArtifactInstallExecutionOutcome outcome,
    const std::string& context) {
    expect(
        child.identity.package_name == package_name,
        context + ": selected package name differs");
    expect(
        child.identity.full_version == full_version,
        context + ": selected package version differs");
    expect(
        child.desired_reason == desired_reason,
        context + ": selected desired reason differs");
    expect(child.outcome == outcome, context + ": selected outcome differs");
}

void expect_unselected_identity(
    const ArtifactPackageIdentity& artifact,
    const std::string& package_name,
    const std::string& full_version,
    const std::string& context) {
    expect(
        artifact.package_name == package_name,
        context + ": unselected package name differs");
    expect(
        artifact.full_version == full_version,
        context + ": unselected package version differs");
}

void test_repository_multi_output_selects_only_requested_child(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    SourceBuildEnvironment source_environment;
    source_environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"CXXFLAGS", "-O2"});
    source_environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{"EMPTY_FLAG", ""});
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_repository_package_base_work_item(
        "repository-multi-base", "repository-multi-child",
        std::move(source_environment)));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{"repository-multi-sibling", "2.0-1"},
        ProducedArtifactPlan{"repository-multi-child", "1.1-1"},
    };

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    PackageBaseSourceBuildExecutionResult result =
        execute_package_base_work_item_typed(invocation, 0, scenario);

    expect(
        result.package_base() == "repository-multi-base" &&
            result.selected_children().size() == 1 &&
            result.unselected_artifacts().size() == 1,
        "Repository multi-output result changed PackageBase or artifact cardinality");
    expect_selected_child_result(
        result.selected_children().front(), "repository-multi-child",
        "1.1-1", DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "repository multi-output requested child");
    expect_unselected_identity(
        result.unselected_artifacts().front(),
        "repository-multi-sibling", "2.0-1",
        "repository multi-output sibling");
    expect(
        scenario.identity_calls == 2 && scenario.install_calls == 1 &&
            metadata_stub::local_package_query_history() ==
                std::vector<std::string>{
                    "repository-multi-child"},
        "Repository multi-output did not query every archive and install only the requested child");
    require_scenario_complete(
        scenario, 1, "repository multi-output selected-only install");
}

void test_repository_debug_split_does_not_install_debug_artifact(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_repository_package_base_work_item(
        "repository-debug-base", "repository-debug-normal"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{"repository-debug-normal", "3.0-1"},
        ProducedArtifactPlan{"repository-debug-normal-debug", "3.0-1"},
    };

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    PackageBaseSourceBuildExecutionResult result =
        execute_package_base_work_item_typed(invocation, 0, scenario);

    expect(
        result.selected_children().size() == 1 &&
            result.unselected_artifacts().size() == 1,
        "Repository debug split changed selected/unselected cardinality");
    expect_selected_child_result(
        result.selected_children().front(), "repository-debug-normal",
        "3.0-1", DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "repository debug split normal child");
    expect_unselected_identity(
        result.unselected_artifacts().front(),
        "repository-debug-normal-debug", "3.0-1",
        "repository debug split artifact");
    expect(
        scenario.identity_calls == 2 && scenario.install_calls == 1,
        "Repository debug split bypassed full identity validation or selected-only install");
    require_scenario_complete(
        scenario, 1, "repository debug split selected-only install");
}

void test_repository_single_output_uses_package_base_set(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_repository_package_base_work_item(
        "repository-single-base", "repository-single-child"));
    expect(
        work_items.front().artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet &&
            !work_items.front().request.only_if_updated &&
            !work_items.front().request.needed,
        "Repository single-output fixture did not use standalone set policy");
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    PackageBaseSourceBuildExecutionResult result =
        execute_package_base_work_item_typed(invocation, 0, scenario);

    expect(
        result.selected_children().size() == 1 &&
            result.unselected_artifacts().empty(),
        "Repository single-output set result changed artifact cardinality");
    expect_selected_child_result(
        result.selected_children().front(), "repository-single-child",
        ARTIFACT_VERSION, DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "repository single-output set child");
    expect(
        scenario.identity_calls == 1 && scenario.install_calls == 1,
        "Repository single-output did not stay on the PackageBase set lifecycle");
    require_scenario_complete(
        scenario, 1, "repository single-output PackageBase set");
}

void test_repository_missing_requested_child_fails_before_install(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_repository_package_base_work_item(
        "repository-missing-base", "repository-missing-child"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{"repository-missing-sibling", "4.0-1"},
    };
    scenario.units[0].expect_install = false;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    bool missing_reported = false;
    try {
        static_cast<void>(execute_package_base_work_item_typed(
            invocation, 0, scenario));
    } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
        missing_reported = true;
        const PackageBaseArtifactIdentitySelectionFailure* failure =
            error.selection_failure();
        expect(
            failure != nullptr &&
                failure->missing_required_artifacts.size() == 1 &&
                failure->missing_required_artifacts.front()
                        .target.package_name ==
                    "repository-missing-child",
            "Repository missing-child failure lost typed selection detail");
    }
    expect(
        missing_reported && scenario.identity_calls == 1 &&
            scenario.install_calls == 0,
        "Repository missing child did not fail closed before install");
    require_scenario_complete(
        scenario, 1, "repository missing requested child");
}

void test_repository_duplicate_archive_identity_fails_before_install(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_repository_package_base_work_item(
        "repository-duplicate-base", "repository-duplicate-child"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{
            "repository-duplicate-first", "1.0-1",
            std::string{"repository-duplicate-child"}},
        ProducedArtifactPlan{
            "repository-duplicate-second", "2.0-1",
            std::string{"repository-duplicate-child"}},
    };
    scenario.units[0].expect_install = false;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    bool duplicate_reported = false;
    try {
        static_cast<void>(execute_package_base_work_item_typed(
            invocation, 0, scenario));
    } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
        duplicate_reported = true;
        const PackageBaseArtifactIdentitySelectionFailure* failure =
            error.selection_failure();
        expect(
            failure != nullptr &&
                failure->duplicate_produced_identities.size() == 1 &&
                failure->duplicate_produced_identities.front()
                        .package_name ==
                    "repository-duplicate-child",
            "Repository duplicate identity failure lost typed selection detail");
    }
    expect(
        duplicate_reported && scenario.identity_calls == 2 &&
            scenario.install_calls == 0,
        "Repository duplicate archive identity reached install");
    require_scenario_complete(
        scenario, 1, "repository duplicate archive identity");
}

void test_repository_requested_looking_path_uses_archive_identity(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_repository_package_base_work_item(
        "repository-wrong-base", "repository-wrong-child"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{
            "repository-wrong-child", "5.0-1",
            std::string{"metadata-other-child"}},
    };
    scenario.units[0].expect_install = false;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    bool missing_reported = false;
    try {
        static_cast<void>(execute_package_base_work_item_typed(
            invocation, 0, scenario));
    } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
        missing_reported = true;
        const PackageBaseArtifactIdentitySelectionFailure* failure =
            error.selection_failure();
        expect(
            failure != nullptr &&
                failure->missing_required_artifacts.size() == 1 &&
                failure->missing_required_artifacts.front()
                        .target.package_name ==
                    "repository-wrong-child",
            "Repository wrong archive identity lost typed missing-child detail");
    }
    expect(
        missing_reported && scenario.identity_calls == 1 &&
            scenario.install_calls == 0 &&
            scenario.produced_artifact_paths.front().front().filename().string().starts_with("repository-wrong-child-"),
        "Repository requested-looking artifact path overrode archive metadata identity");
    require_scenario_complete(
        scenario, 1, "repository wrong archive identity");
}

void test_ordinary_build_plan_unit_uses_set_owner(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            ordinary_single_entry_plan(), false, false);
    expect(
        work_items.size() == 1 &&
            work_items.front().required_target_provenance ==
                RequiredTargetProvenance::
                    AurBuildPlanProjection &&
            work_items.front().artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet,
        "Ordinary AUR BuildPlan did not produce one set-owned work item");
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    PackageBaseSourceBuildExecutionResult result =
        execute_package_base_work_item_typed(invocation, 0, scenario);

    expect(
        result.package_base() == "ordinary-set-root",
        "Ordinary AUR set result lost PackageBase identity");
    expect(
        result.selected_children().size() == 1 &&
            result.unselected_artifacts().empty(),
        "Ordinary AUR set result changed selected/unselected cardinality");
    expect_selected_child_result(
        result.selected_children().front(), "ordinary-set-root",
        ARTIFACT_VERSION, DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "ordinary AUR set owner");
    expect(
        result.installed_any() && !result.all_skipped_as_needed(),
        "Ordinary AUR set aggregate outcome differs");
    expect(
        scenario.install_attempt_order ==
            std::vector<std::string>{"ordinary-set-root"},
        "Ordinary AUR BuildPlan did not reach one PackageBase transaction");
    require_scenario_complete(
        scenario, 1, "ordinary AUR BuildPlan set owner");
}

void test_requested_split_child_uses_set_owner(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_package_base_work_item(
        "requested-split-base",
        {RequiredPackageArtifactTarget{
            "requested-split-base", "requested-split-child",
            DesiredInstallReason::Dependency}}));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].install_reason_option = "--asdeps";

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    PackageBaseSourceBuildExecutionResult result =
        execute_package_base_work_item_typed(invocation, 0, scenario);

    expect(
        result.package_base() == "requested-split-base" &&
            result.selected_children().size() == 1 &&
            result.unselected_artifacts().empty(),
        "Requested split child set result has inconsistent aggregate identity");
    expect_selected_child_result(
        result.selected_children().front(), "requested-split-child",
        ARTIFACT_VERSION, DesiredInstallReason::Dependency,
        ArtifactInstallExecutionOutcome::Installed,
        "requested split child set owner");
    expect(
        scenario.install_attempt_order ==
            std::vector<std::string>{"requested-split-base"},
        "Requested split child did not execute by checkout PackageBase");
    require_scenario_complete(
        scenario, 1, "requested split child set owner");
}

void test_multiple_required_children_return_typed_set_result(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_package_base_work_item(
        "multiple-result-base",
        {RequiredPackageArtifactTarget{
             "multiple-result-base", "multiple-result-second",
             DesiredInstallReason::Explicit},
         RequiredPackageArtifactTarget{
             "multiple-result-base", "multiple-result-first",
             DesiredInstallReason::Explicit}}));
    expect(
        work_items.front().request.package_name.empty(),
        "Multiple-result fixture unexpectedly has a singular package name");
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    // produced order intentionally differs from required order and includes
    // both an ordinary sibling and a debug artifact.
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{"multiple-result-sibling", "2.0-1"},
        ProducedArtifactPlan{"multiple-result-first", "1.1-1"},
        ProducedArtifactPlan{"multiple-result-debug", "1.1-1"},
        ProducedArtifactPlan{"multiple-result-second", "1.2-1"},
    };

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    PackageBaseSourceBuildExecutionResult result =
        execute_package_base_work_item_typed(invocation, 0, scenario);

    expect(
        result.package_base() == "multiple-result-base",
        "Multiple set result lost PackageBase identity");
    expect(
        result.selected_children().size() == 2,
        "Multiple set result lost a selected required child");
    expect_selected_child_result(
        result.selected_children()[0], "multiple-result-second",
        "1.2-1", DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "multiple required-order second child");
    expect_selected_child_result(
        result.selected_children()[1], "multiple-result-first",
        "1.1-1", DesiredInstallReason::Explicit,
        ArtifactInstallExecutionOutcome::Installed,
        "multiple required-order first child");
    expect(
        result.unselected_artifacts().size() == 2,
        "Multiple set result lost an unselected artifact identity");
    expect_unselected_identity(
        result.unselected_artifacts()[0], "multiple-result-sibling",
        "2.0-1", "multiple produced-order sibling");
    expect_unselected_identity(
        result.unselected_artifacts()[1], "multiple-result-debug",
        "1.1-1", "multiple produced-order debug artifact");
    expect(
        metadata_stub::local_package_query_history() ==
            std::vector<std::string>{
                "multiple-result-second",
                "multiple-result-first"},
        "Multiple set metadata queries did not preserve required child order");
    expect(
        scenario.identity_calls == 4 && scenario.install_calls == 1 &&
            scenario.install_attempt_order ==
                std::vector<std::string>{"multiple-result-base"},
        "Multiple set execution did not use one selected-only transaction");
    expect(
        result.installed_any() && !result.all_skipped_as_needed(),
        "Multiple set aggregate outcome differs");
    require_scenario_complete(
        scenario, 1, "multiple required child typed set result");
}

void test_package_base_transaction_failure_preserves_attempt_snapshot(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_package_base_work_item(
        "transaction-attempt-base",
        {RequiredPackageArtifactTarget{
             "transaction-attempt-base",
             "transaction-attempt-second",
             DesiredInstallReason::Explicit},
         RequiredPackageArtifactTarget{
             "transaction-attempt-base",
             "transaction-attempt-first",
             DesiredInstallReason::Explicit}}));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].produced_artifacts = {
        ProducedArtifactPlan{"transaction-attempt-sibling", "9-1"},
        ProducedArtifactPlan{"transaction-attempt-first", "1-1"},
        ProducedArtifactPlan{"transaction-attempt-second", "2-1"},
    };
    scenario.units[0].install_exit_code = 73;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    std::optional<PackageBaseSourceBuildExecutionResult> public_result;
    bool transaction_failure_reported = false;
    try {
        public_result.emplace(
            execute_package_base_work_item_typed(
                invocation, 0, scenario));
    } catch(const PackageBaseArtifactInstallTransactionError& error) {
        transaction_failure_reported = true;
        expect(
            error.failure_kind() ==
                    PackageBaseArtifactInstallTransactionFailureKind::
                        NonzeroExit &&
                error.package_base() == "transaction-attempt-base" &&
                error.exit_code() == std::optional<int>{73},
            "PackageBase production transaction failure detail differs");
        expect(
            error.production_outcome().has_value() &&
                error.production_outcome()->build_outcome ==
                    ProductionSourceBuildCommandOutcome::
                        Succeeded &&
                error.production_outcome()->install_outcome ==
                    ProductionSourceInstallOutcome::Failed &&
                error.production_outcome()->source_provenance.review_status ==
                    ProductionSourceReviewStatus::
                        CompatibilityWithoutReview,
            "PackageBase transaction failure lost staged production outcome");
        expect(
            error.attempts().size() == 2 &&
                error.attempts()[0].identity.package_name ==
                    "transaction-attempt-second" &&
                error.attempts()[0].identity.full_version == "2-1" &&
                error.attempts()[0].desired_reason ==
                    DesiredInstallReason::Explicit &&
                error.attempts()[1].identity.package_name ==
                    "transaction-attempt-first" &&
                error.attempts()[1].identity.full_version == "1-1" &&
                error.attempts()[1].desired_reason ==
                    DesiredInstallReason::Explicit,
            "PackageBase production transaction attempt order differs");
        expect(
            std::none_of(
                error.attempts().begin(), error.attempts().end(),
                [](const PackageBaseArtifactInstallTransactionAttempt&
                       attempt) {
                    return attempt.identity.package_name ==
                           "transaction-attempt-sibling";
                }),
            "Unselected sibling leaked into transaction attempts");
        expect(
            std::string(error.what()).find(scenario.produced_artifact_paths[0][1].string()) ==
                std::string::npos,
            "PackageBase transaction failure exposed an artifact path");
    }
    expect(
        transaction_failure_reported,
        "PackageBase production did not preserve typed transaction failure");
    expect(
        !public_result.has_value(),
        "PackageBase production transaction failure fabricated child success");
    expect(
        scenario.install_attempt_order ==
                std::vector<std::string>{
                    "transaction-attempt-base"} &&
            scenario.install_calls == 1 &&
            fs::is_regular_file(
                scenario.produced_artifact_paths[0][1]),
        "PackageBase production transaction failure lost retained state");
    require_scenario_complete(
        scenario, 1,
        "PackageBase production transaction attempt snapshot");
}

void test_single_aur_root_uses_shared_lifecycle(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("single-root"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);

    execute_invocation(invocation, scenario);

    expect(
        scenario.install_attempt_order ==
            std::vector<std::string>{"single-root"},
        "Single AUR root did not reach typed pacman -U");
    expect(
        metadata_stub::created_handle_count() == 1 &&
            metadata_stub::release_call_count() == 1,
        "Single AUR root did not use one fresh metadata session");
    expect(
        !fs::exists(scenario.workspace_paths.at(0)),
        "Single AUR root success did not clean its workspace");
    require_scenario_complete(scenario, 1, "single AUR root success");
}

void test_multi_unit_options_roles_and_order(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    config.no_confirm = true;
    config.user_config.build.mode = BuildMode::Clean;
    const BuildPlan plan = two_entry_plan();
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(plan, false, true);
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].install_reason_option = "--asdeps";
    scenario.units[1].metadata_mode = MetadataMode::ExistingDependency;
    scenario.units[1].install_reason_option = "--asexplicit";

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    execute_invocation(invocation, scenario);

    expect(
        scenario.resolver_calls == 1,
        "Multiple units re-resolved PacmanDatabasePaths");
    expect(
        metadata_stub::created_handle_count() == 2 &&
            metadata_stub::release_call_count() == 2,
        "Multiple units did not use fresh metadata sessions");
    expect(
        metadata_stub::release_count_for_handle(0) == 1 &&
            metadata_stub::release_count_for_handle(1) == 1,
        "Metadata sessions were shared or released incorrectly");
    expect(
        scenario.install_attempt_order == std::vector<std::string>{
                                              "dependency-package", "root-package"},
        "Production execution changed BuildPlan::order");
    expect(
        process_stub::run_command_call_count() == 8,
        "Combined option scenario changed its strict run boundary count");
    for(const fs::path& workspace_path : scenario.workspace_paths) {
        expect(
            !fs::exists(workspace_path),
            "Successful multi-unit execution retained a workspace");
    }
    require_scenario_complete(
        scenario, 2, "multi-unit role/option/order projection");
}

void test_invocation_aggregate_model_retains_reviewed_compatibility_failure_and_suffix() {
    ProductionSourceBuildProvenance reviewed;
    reviewed.review_status = ProductionSourceReviewStatus::Reviewed;
    reviewed.reviewed_upstream_base_revision =
        SourceRevisionIdentity::git_commit(
            "1111111111111111111111111111111111111111");
    reviewed.publication_status =
        ReviewedSourcePublicationStatus::Published;
    reviewed.reviewed_outcome =
        ProductionReviewedSourceOutcome::InitialFullReview;
    reviewed.reviewed_state_generation = 1;

    ProductionSourceBuildProvenance compatibility;
    compatibility.review_status =
        ProductionSourceReviewStatus::CompatibilityWithoutReview;
    compatibility.compatibility_reason =
        ReviewedSourceCompatibilityBuildReason::NoDiff;

    ProductionSourceBuildInvocationResult aggregate;
    aggregate.work_items.resize(4);
    aggregate.work_items[0].package_base = "aggregate-reviewed-a";
    aggregate.work_items[0].status =
        ProductionSourceBuildWorkItemStatus::Succeeded;
    aggregate.work_items[0].production_outcome =
        ProductionSourceBuildStagedOutcome{
            reviewed,
            ProductionSourceBuildCommandOutcome::Succeeded,
            ProductionSourceInstallOutcome::Succeeded};
    aggregate.work_items[1].package_base = "aggregate-compatibility-b";
    aggregate.work_items[1].status =
        ProductionSourceBuildWorkItemStatus::Succeeded;
    aggregate.work_items[1].production_outcome =
        ProductionSourceBuildStagedOutcome{
            compatibility,
            ProductionSourceBuildCommandOutcome::Succeeded,
            ProductionSourceInstallOutcome::Succeeded};
    aggregate.work_items[2].package_base = "aggregate-failed-c";
    aggregate.work_items[2].status =
        ProductionSourceBuildWorkItemStatus::Failed;
    aggregate.work_items[2].production_outcome =
        ProductionSourceBuildStagedOutcome{
            compatibility,
            ProductionSourceBuildCommandOutcome::Succeeded,
            ProductionSourceInstallOutcome::Failed};
    aggregate.work_items[2].failure_stage =
        ProductionSourceBuildFailureStage::InstallPreparation;
    aggregate.work_items[2].diagnostic = "typed metadata failure";
    aggregate.work_items[2].failure_exception = std::make_exception_ptr(
        PackageMetadataError(PackageMetadataFailure{
            PackageMetadataErrorCode::QueryFailed,
            "typed metadata failure"}));
    aggregate.work_items[3].package_base = "aggregate-not-attempted-d";

    const ProductionSourceBuildInvocationError failure(
        std::move(aggregate), 2, "typed metadata failure");
    const auto& outcomes = failure.result().work_items;
    expect(
        outcomes.size() == 4 &&
            outcomes[0].status ==
                ProductionSourceBuildWorkItemStatus::Succeeded &&
            outcomes[0].production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::Reviewed &&
            outcomes[1].status ==
                ProductionSourceBuildWorkItemStatus::Succeeded &&
            outcomes[1].production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            outcomes[2].status ==
                ProductionSourceBuildWorkItemStatus::Failed &&
            outcomes[2].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            outcomes[2].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Failed &&
            outcomes[3].status ==
                ProductionSourceBuildWorkItemStatus::NotAttempted &&
            !failure.result().is_success(),
        "Four-entry aggregate model flattened A/B/C/D outcome");
    try {
        failure.rethrow_failure();
        throw std::runtime_error(
            "Four-entry aggregate lost typed metadata cause");
    } catch(const PackageMetadataError& error) {
        expect(
            error.failure().code ==
                PackageMetadataErrorCode::QueryFailed,
            "Four-entry aggregate changed typed metadata cause");
    }
}

void test_normal_invocation_retains_completed_failure_and_suffix(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();

    std::vector<ProductionSourceBuildWorkItem> work_items{
        make_work_item("aggregate-reviewed-a"),
        make_work_item("aggregate-compatibility-b"),
        make_work_item("aggregate-failed-c"),
        make_work_item("aggregate-not-attempted-d")};
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[2].install_exit_code = 73;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const ProductionSourceBuildInvocationError failure =
        expect_invocation_error(
            [&]() { execute_invocation(invocation, scenario); },
            "normal four-entry invocation aggregate",
            "pacman -U failed with exit code 73");

    const auto& outcomes = failure.result().work_items;
    expect(
        outcomes.size() == 4 &&
            outcomes[0].status ==
                ProductionSourceBuildWorkItemStatus::Succeeded &&
            outcomes[0].production_outcome.has_value() &&
            outcomes[0].production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            outcomes[0].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            outcomes[0].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Succeeded,
        "Normal aggregate lost completed success A");
    expect(
        outcomes[1].status ==
                ProductionSourceBuildWorkItemStatus::Succeeded &&
            outcomes[1].production_outcome.has_value() &&
            outcomes[1].production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            outcomes[1].production_outcome->source_provenance.compatibility_reason ==
                std::optional<
                    ReviewedSourceCompatibilityBuildReason>{
                    ReviewedSourceCompatibilityBuildReason::
                        NoDiff} &&
            outcomes[1].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            outcomes[1].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Succeeded,
        "Normal aggregate lost compatibility success B");
    expect(
        outcomes[2].status ==
                ProductionSourceBuildWorkItemStatus::Failed &&
            outcomes[2].production_outcome.has_value() &&
            outcomes[2].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            outcomes[2].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Failed &&
            outcomes[2].failure_stage ==
                std::optional<
                    ProductionSourceBuildFailureStage>{
                    ProductionSourceBuildFailureStage::
                        InstallTransaction} &&
            outcomes[2].failure_exception != nullptr,
        "Normal aggregate lost typed failed outcome C");
    expect(
        outcomes[3].status ==
                ProductionSourceBuildWorkItemStatus::NotAttempted &&
            !outcomes[3].production_outcome.has_value() &&
            outcomes[3].failure_exception == nullptr &&
            !failure.result().is_success(),
        "Normal aggregate lost NotAttempted suffix D");
    expect(
        scenario.workspace_paths.size() == 3 &&
            scenario.install_attempt_order ==
                std::vector<std::string>{
                    "aggregate-reviewed-a",
                    "aggregate-compatibility-b",
                    "aggregate-failed-c"},
        "Normal aggregate executed suffix work after C");
    require_scenario_complete(
        scenario, 3, "normal four-entry invocation aggregate");
}

void test_normal_invocation_retains_status_zero_postcheck_and_suffix(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items{
        make_work_item("aggregate-postcheck-a"),
        make_work_item("aggregate-postcheck-b"),
        make_work_item("aggregate-postcheck-c")};
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[1].replace_workspace_after_build = true;
    scenario.units[1].expect_identity = false;
    scenario.units[1].expect_install = false;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const ProductionSourceBuildInvocationError failure =
        expect_invocation_error(
            [&]() { execute_invocation(invocation, scenario); },
            "status-zero postcheck invocation aggregate", "identity");

    const auto& outcomes = failure.result().work_items;
    expect(
        outcomes.size() == 3 &&
            failure.failed_work_item_index() == 1 &&
            outcomes[0].status ==
                ProductionSourceBuildWorkItemStatus::Succeeded &&
            outcomes[0].production_outcome.has_value() &&
            outcomes[0].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            outcomes[0].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Succeeded,
        "Status-zero postcheck aggregate lost completed prefix A");
    expect(
        outcomes[1].status ==
                ProductionSourceBuildWorkItemStatus::Failed &&
            outcomes[1].production_outcome.has_value() &&
            outcomes[1].production_outcome->source_provenance.review_status ==
                ProductionSourceReviewStatus::
                    CompatibilityWithoutReview &&
            outcomes[1].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            outcomes[1].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::NotAttempted &&
            outcomes[1].failure_stage ==
                std::optional<
                    ProductionSourceBuildFailureStage>{
                    ProductionSourceBuildFailureStage::Build} &&
            outcomes[1].failure_exception != nullptr,
        "Status-zero postcheck aggregate lost failed entry B");
    expect(
        outcomes[2].status ==
                ProductionSourceBuildWorkItemStatus::NotAttempted &&
            !outcomes[2].production_outcome.has_value() &&
            outcomes[2].failure_exception == nullptr,
        "Status-zero postcheck aggregate changed NotAttempted suffix C");

    bool typed_failure_retained = false;
    try {
        failure.rethrow_failure();
    } catch(const SeparatedSourceBuildPhaseError& phase_error) {
        expect(
            phase_error.production_outcome().build_outcome ==
                    ProductionSourceBuildCommandOutcome::
                        Succeeded &&
                phase_error.production_outcome().install_outcome ==
                    ProductionSourceInstallOutcome::
                        NotAttempted,
            "Aggregate nested phase lost status-zero staged outcome");
        try {
            phase_error.rethrow_failure();
        } catch(const ProductionSourceBuildPostCommandRevalidationError&
                    post_command_error) {
            typed_failure_retained = true;
            expect(
                post_command_error.command_exit_status() == 0,
                "Aggregate nested postcheck changed makepkg status");
        }
    }
    expect(
        typed_failure_retained,
        "Status-zero postcheck aggregate lost typed failure transport");
    expect(
        scenario.workspace_paths.size() == 2 &&
            scenario.install_attempt_order ==
                std::vector<std::string>{
                    "aggregate-postcheck-a"} &&
            fs::is_regular_file(
                scenario.displaced_workspace_paths.at(1) /
                scenario.artifact_paths.at(1).filename()),
        "Status-zero postcheck aggregate executed suffix or lost artifact");
    require_scenario_complete(
        scenario, 2, "status-zero postcheck invocation aggregate");
}

void test_build_failure_does_not_reach_sudo(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("build-failure"));
    work_items.push_back(make_work_item("build-not-started"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].build_exit_code = 37;
    scenario.units[0].expect_identity = false;
    scenario.units[0].expect_install = false;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const ProductionSourceBuildInvocationError failure =
        expect_invocation_error(
            [&]() { execute_invocation(invocation, scenario); },
            "production build failure",
            "The build-only makepkg command failed with exit code 37");

    expect(
        failure.result().work_items.size() == 2 &&
            failure.failed_work_item_index() == 0 &&
            failure.result().work_items[0].status ==
                ProductionSourceBuildWorkItemStatus::Failed &&
            failure.result().work_items[0].production_outcome.has_value() &&
            failure.result().work_items[0].production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Failed &&
            failure.result().work_items[0].production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::NotAttempted &&
            failure.result().work_items[1].status ==
                ProductionSourceBuildWorkItemStatus::NotAttempted,
        "Build failure aggregate lost failed/current or suffix outcome");

    expect(
        scenario.install_calls == 0 &&
            metadata_stub::initialize_call_count() == 0,
        "Build failure reached metadata or sudo pacman");
    expect(
        scenario.workspace_paths.size() == 1,
        "Build failure started a later PackageBase");
    require_scenario_complete(scenario, 1, "production build failure");
}

void test_metadata_failure_does_not_reach_sudo(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("metadata-failure"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].metadata_mode = MetadataMode::QueryFailure;
    scenario.units[0].expect_install = false;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const ProductionSourceBuildInvocationError failure =
        expect_invocation_error(
            [&]() { execute_invocation(invocation, scenario); },
            "production metadata failure", "Installed package query failed");

    const ProductionSourceBuildWorkItemOutcome& failed =
        failure.result().work_items.front();
    expect(
        failure.failure_stage() ==
                ProductionSourceBuildFailureStage::
                    InstallPreparation &&
            failed.production_outcome.has_value() &&
            failed.production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            failed.production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Failed,
        "Metadata failure lost successful build or failed install preparation");
    try {
        failure.rethrow_failure();
        throw std::runtime_error(
            "Metadata failure aggregate lost its typed cause");
    } catch(const SeparatedSourceBuildPhaseError& error) {
        expect(
            error.package_metadata_failure().has_value() &&
                error.package_metadata_failure()->code ==
                    PackageMetadataErrorCode::QueryFailed,
            "Metadata failure aggregate lost PackageMetadataFailure");
    }

    expect(
        scenario.install_calls == 0,
        "Metadata failure reached sudo pacman");
    expect(
        metadata_stub::initialize_call_count() == 1 &&
            metadata_stub::package_query_call_count() == 1 &&
            metadata_stub::release_call_count() == 1,
        "Metadata failure did not close its fresh session");
    require_scenario_complete(scenario, 1, "production metadata failure");
}

void test_pacman_failure_stops_later_unit(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("pacman-failure"));
    work_items.push_back(make_work_item("pacman-later"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].install_exit_code = 73;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const ProductionSourceBuildInvocationError failure =
        expect_invocation_error(
            [&]() { execute_invocation(invocation, scenario); },
            "production pacman failure",
            "pacman -U failed with exit code 73");

    const ProductionSourceBuildWorkItemOutcome& failed =
        failure.result().work_items.front();
    const std::string cli_diagnostic =
        format_production_source_build_invocation_failure(failure);
    expect(
        failure.result().work_items.size() == 2 &&
            failed.production_outcome.has_value() &&
            failed.production_outcome->build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            failed.production_outcome->install_outcome ==
                ProductionSourceInstallOutcome::Failed &&
            failure.result().work_items[1].status ==
                ProductionSourceBuildWorkItemStatus::NotAttempted &&
            cli_diagnostic.find("Install Error:") !=
                std::string::npos &&
            cli_diagnostic.find("Build Error:") ==
                std::string::npos,
        "pacman failure lost staged outcome, suffix, or install CLI classification");

    expect(
        scenario.workspace_paths.size() == 1 &&
            scenario.install_attempt_order ==
                std::vector<std::string>{"pacman-failure"},
        "pacman failure started a later PackageBase");
    expect(
        fs::is_regular_file(scenario.artifact_paths.at(0)),
        "pacman failure did not retain its diagnostic artifact");
    require_scenario_complete(scenario, 1, "production pacman failure");
}

void test_cleanup_partial_success_stays_distinct_and_stops(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("cleanup-partial"));
    work_items.push_back(make_work_item("cleanup-later"));
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].replace_workspace_after_install = true;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const CleanupErrorObservation cleanup_error = expect_cleanup_error(
        [&]() { execute_invocation(invocation, scenario); },
        "production cleanup partial-success");

    expect(
        cleanup_error.install_outcome ==
                ArtifactInstallExecutionOutcome::Installed &&
            cleanup_error.production_outcome.build_outcome ==
                ProductionSourceBuildCommandOutcome::Succeeded &&
            cleanup_error.production_outcome.install_outcome ==
                ProductionSourceInstallOutcome::Succeeded,
        "Cleanup error lost the completed install outcome");
    expect(
        cleanup_error.diagnostic.find("Package installation succeeded") !=
            std::string::npos,
        "Cleanup error omitted successful package installation");
    expect(
        cleanup_error.diagnostic.find("Failed while building/installing") ==
                std::string::npos &&
            cleanup_error.diagnostic.find("pacman -U failed") ==
                std::string::npos,
        "Cleanup partial-success was flattened to transaction failure");
    expect(
        scenario.workspace_paths.size() == 1 &&
            scenario.install_attempt_order ==
                std::vector<std::string>{"cleanup-partial"},
        "Cleanup partial-success started a later PackageBase");
    expect(
        fs::is_regular_file(
            scenario.displaced_workspace_paths.at(0) /
            "cleanup-partial-1.0-1-x86_64.pkg.tar.zst"),
        "Cleanup partial-success lost the installed artifact workspace");
    require_scenario_complete(
        scenario, 1, "production cleanup partial-success");
}

void test_needed_same_version_cleanup_failure_preserves_no_change(
    const TemporaryProductionEnvironment& environment) {
    AppConfig config = noninteractive_config();
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(make_work_item("cleanup-no-change"));
    work_items[0].request.needed = true;
    ProductionScenario scenario =
        make_execution_scenario(environment, work_items, config);
    scenario.units[0].metadata_mode =
        MetadataMode::ExistingExplicitSameVersion;
    scenario.units[0].replace_workspace_after_install = true;

    PreparedProductionSourceBuildInvocation invocation = prepare_execution(
        std::move(work_items), scenario);
    const CleanupErrorObservation cleanup_error = expect_cleanup_error(
        [&]() {
            static_cast<void>(execute_work_item(invocation, 0, scenario));
        },
        "same-version --needed cleanup failure");

    expect(
        cleanup_error.install_outcome ==
            ArtifactInstallExecutionOutcome::SkippedAsNeeded,
        "Cleanup error flattened same-version --needed skip to install");
    expect(
        cleanup_error.diagnostic.find("Package installation succeeded") !=
            std::string::npos,
        "No-change cleanup error changed the existing diagnostic");
    expect(
        scenario.install_attempt_order ==
            std::vector<std::string>{"cleanup-no-change"},
        "No-change cleanup failure did not reach successful pacman -U");
    expect(
        fs::is_regular_file(
            scenario.displaced_workspace_paths.at(0) /
            "cleanup-no-change-1.0-1-x86_64.pkg.tar.zst"),
        "No-change cleanup failure lost its diagnostic artifact workspace");
    require_scenario_complete(
        scenario, 1, "same-version --needed cleanup failure");
}

} // namespace

int main() {
    try {
        set_reviewed_source_lifecycle_default_missing_for_test(true);
        test_process_stub_rejects_cross_kind_reordering();
        test_build_plan_projection();
        test_local_dependency_preparation_collects_selected_repository_providers();
        test_selected_repository_provider_projection_and_invocation_deduplication();
        test_selected_repository_provider_static_validation();
        test_conflicting_selected_provider_identity_stops_work_item_preparation();
        test_same_package_base_projection_preserves_required_children();
        test_same_package_base_source_preference_route();
        test_invocation_aggregate_model_retains_reviewed_compatibility_failure_and_suffix();
        test_resolved_repository_identity_and_owned_environment_preparation();
        test_resolved_repository_split_identity_and_required_target_projection();
        test_registered_source_factory_selects_route_owned_lifecycle();
        test_standalone_repository_preparation_uses_package_base_set();
        test_repository_query_failure_stops_before_aur_or_mutation();
        test_confirmed_repository_not_found_allows_exact_aur_fallback();
        test_repository_work_item_static_identity_invariants();
        test_aur_work_item_review_identity_invariants();
        {
            // Production must remain private-first even when the caller's
            // ordinary collaborative umask would make a legacy mkdir 0775.
            ScopedUmask scoped_umask(0002);
            test_unsafe_existing_cache_root_stops_before_checkout_mutation();
            test_singular_cache_failure_preserves_trusted_type();
            test_selected_repository_provider_cache_failure_precedes_transaction();
            test_package_base_cache_failures_preserve_trusted_type();
            TemporaryProductionEnvironment environment;
            test_local_dependency_invocation_accepts_zero_remote_units(
                environment);
            test_local_dependency_invocation_executes_provider_without_aur_units(
                environment);
            test_set_static_preparation_accepts_split_and_multiple(environment);
            test_set_static_preparation_rejects_invalid_sets_before_mutation(
                environment);
            test_selected_repository_provider_executes_before_source(
                environment);
            test_new_repository_provider_uses_asdeps_needed_transaction(
                environment);
            test_selected_repository_provider_trusted_executor_closes_evidence(
                environment);
            test_existing_explicit_repository_provider_same_version_stays_explicit(
                environment);
            test_existing_explicit_repository_provider_update_stays_explicit(
                environment);
            test_repository_provider_metadata_failure_stops_before_mutation(
                environment);
            test_mixed_repository_provider_reasons_stop_before_mutation(
                environment);
            test_repository_provider_failure_stops_before_source_mutation(
                environment);
            test_unique_repository_provider_does_not_schedule_transaction(
                environment);
            test_rmdeps_global_rejection(environment);
            test_inherited_pkgdest_global_rejection(environment);
            test_later_target_pkgdest_global_rejection(environment);
            test_database_resolver_failure_stops_all_targets(environment);
            test_work_item_typed_install_outcomes(environment);
            test_extended_work_item_install_outcomes(environment);
            test_review_bypass_provenance(environment);
            test_invocation_preflight_snapshot_is_consumed_without_reread(
                environment);
            test_local_dependency_preflight_snapshot_is_consumed_without_reread(
                environment);
            test_invocation_wide_fatal_preflight_stops_before_cache_provider_and_work_items();
            test_all_compatibility_modes_observe_fatal_reviewed_state(
                environment);
            test_reviewed_source_failure_payload_crosses_singular_and_set_routes(
                environment);
            test_registered_repository_closed_preparation_outcomes(
                environment);
            test_up_to_date_outcome_and_legacy_flattening(environment);
            test_unknown_update_status_no_confirm_outcome(environment);
            test_unknown_update_status_noninteractive_outcome(environment);
            test_unknown_update_status_user_decline_outcome(environment);
            test_repository_multi_output_selects_only_requested_child(
                environment);
            test_repository_debug_split_does_not_install_debug_artifact(
                environment);
            test_repository_single_output_uses_package_base_set(environment);
            test_repository_missing_requested_child_fails_before_install(
                environment);
            test_repository_duplicate_archive_identity_fails_before_install(
                environment);
            test_repository_requested_looking_path_uses_archive_identity(
                environment);
            test_ordinary_build_plan_unit_uses_set_owner(environment);
            test_requested_split_child_uses_set_owner(environment);
            test_multiple_required_children_return_typed_set_result(environment);
            test_package_base_transaction_failure_preserves_attempt_snapshot(
                environment);
            test_single_aur_root_uses_shared_lifecycle(environment);
            test_multi_unit_options_roles_and_order(environment);
            test_normal_invocation_retains_completed_failure_and_suffix(
                environment);
            test_normal_invocation_retains_status_zero_postcheck_and_suffix(
                environment);
            test_build_failure_does_not_reach_sudo(environment);
            test_metadata_failure_does_not_reach_sudo(environment);
            test_pacman_failure_stops_later_unit(environment);
            test_cleanup_partial_success_stays_distinct_and_stops(environment);
            test_needed_same_version_cleanup_failure_preserves_no_change(
                environment);
        }
        std::cout << "production source-build tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "production source-build test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
