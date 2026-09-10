#include "trusted_git.hpp"

#include "localization.hpp"
#include "logging.hpp"
#include "persistent_checkout.hpp"
#include "process.hpp"
#include "reviewed_source_pinned_build.hpp"
#include "reviewed_source_git_parser.hpp"
#include "trusted_git_process_policy.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::size_t MAX_LOCAL_CONFIG_OUTPUT = 1024 * 1024;
constexpr std::size_t MAX_COMMIT_OID_OUTPUT = 65;
constexpr std::size_t MAX_OBJECT_TYPE_OUTPUT = 7;
constexpr std::size_t MAX_EMPTY_COMMAND_OUTPUT = 1;
constexpr std::size_t MAX_SHALLOW_OUTPUT = 6;
constexpr std::size_t REVIEW_BLOB_BATCH_INPUT_LIMIT = 4096;
constexpr std::uintmax_t REVIEW_BLOB_BATCH_PAYLOAD_LIMIT =
    REVIEWED_SOURCE_SINGLE_BLOB_LIMIT;
constexpr std::size_t REVIEWED_SOURCE_OVERLAY_PATH_LIMIT = 4096;
constexpr std::size_t REVIEWED_SOURCE_OVERLAY_DEPTH_LIMIT = 256;
constexpr std::size_t REVIEWED_SOURCE_OVERLAY_SYMLINK_TARGET_LIMIT = 4096;

enum class PinnedCheckoutWorktreePolicy {
    RequireClean,
    AllowEditorOverlay,
};

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_GIT_TEST_HOOKS
std::optional<std::size_t> g_review_machine_stream_limit;
#endif

class OwnedFileDescriptor final {
public:
    explicit OwnedFileDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
    OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;

    OwnedFileDescriptor(OwnedFileDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    OwnedFileDescriptor& operator=(OwnedFileDescriptor&& other) noexcept {
        if(this == &other) return *this;
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ~OwnedFileDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] bool valid() const noexcept {
        return descriptor_ >= 0;
    }

private:
    int descriptor_ = -1;
};

struct DirectoryStreamCloser {
    void operator()(DIR* directory) const noexcept {
        if(directory != nullptr) static_cast<void>(::closedir(directory));
    }
};

using OwnedDirectoryStream =
    std::unique_ptr<DIR, DirectoryStreamCloser>;

class TemporaryOverlayIndex final {
public:
    TemporaryOverlayIndex() {
        std::string pattern =
            (fs::temp_directory_path() /
             "moguet-reviewed-overlay-index-XXXXXX")
                .string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::system_error(
                errno, std::generic_category(),
                "Failed to create reviewed source overlay index directory");
        }
        directory_ = created;
        index_ = directory_ / "index";
        std::error_code error;
        if(!fs::create_directory(directory_ / "objects", error) || error) {
            std::error_code cleanup_error;
            fs::remove_all(directory_, cleanup_error);
            throw std::system_error(
                error, "Failed to create reviewed source overlay object directory");
        }
        fs::permissions(
            directory_ / "objects", fs::perms::owner_all,
            fs::perm_options::replace, error);
        if(error) {
            std::error_code cleanup_error;
            fs::remove_all(directory_, cleanup_error);
            throw std::system_error(
                error, "Failed to secure reviewed source overlay object directory");
        }
    }

    TemporaryOverlayIndex(const TemporaryOverlayIndex&) = delete;
    TemporaryOverlayIndex& operator=(const TemporaryOverlayIndex&) = delete;

    ~TemporaryOverlayIndex() noexcept {
        std::error_code error;
        fs::remove_all(directory_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return index_;
    }

private:
    fs::path directory_;
    fs::path index_;
};

struct LocalGitConfiguration {
    std::string remote_origin_url;
    GitObjectFormat object_format = GitObjectFormat::Sha1;
};

struct BranchConfigurationState {
    bool has_remote = false;
    bool has_merge = false;
};

std::string trim(const std::string& value) {
    const std::string::size_type first =
        value.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    const std::string::size_type last =
        value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

std::string to_lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

[[noreturn]] void throw_unsafe_local_configuration() {
    // Do not include config values or the checkout/cache path in diagnostics.
    throw std::runtime_error(localization::format_translated_message(
        "Refusing unsafe local {} configuration in the managed checkout.",
        "Git"));
}

bool is_boolean_value(const std::string& value) {
    const std::string lowered = to_lower(trim(value));
    return lowered == "true" || lowered == "false" || lowered == "yes" ||
           lowered == "no" || lowered == "on" || lowered == "off" ||
           lowered == "1" || lowered == "0";
}

bool is_false_value(const std::string& value) {
    const std::string lowered = to_lower(trim(value));
    return lowered == "false" || lowered == "no" || lowered == "off" ||
           lowered == "0";
}

bool has_control_character(const std::string& value) {
    return std::any_of(
        value.begin(), value.end(), [](unsigned char character) {
            return character < 0x20 || character == 0x7f;
        });
}

bool is_safe_branch_component(const std::string& value) {
    if(value.empty() || value.front() == '-' || value.front() == '/' ||
       value.back() == '/' || value.back() == '.' ||
       value.find("..") != std::string::npos ||
       value.find("//") != std::string::npos ||
       value.find("@{") != std::string::npos) {
        return false;
    }
    return std::all_of(
        value.begin(), value.end(), [](unsigned char character) {
            return std::isalnum(character) || character == '-' ||
                   character == '_' || character == '.' ||
                   character == '/';
        });
}

void require_unique_key(
    std::map<std::string, std::size_t>& key_counts,
    const std::string& key) {
    if(++key_counts[key] != 1) throw_unsafe_local_configuration();
}

LocalGitConfiguration parse_local_configuration(
    const CapturedCommandResult& result) {
    if(result.exit_code != 0 || result.output.empty() ||
       result.stdout_capture_limit_exceeded ||
       result.output.size() > MAX_LOCAL_CONFIG_OUTPUT ||
       result.output.back() != '\0') {
        throw_unsafe_local_configuration();
    }

    std::map<std::string, std::size_t> key_counts;
    std::map<std::string, BranchConfigurationState> branch_states;
    LocalGitConfiguration configuration;
    bool has_repository_format = false;
    bool has_file_mode = false;
    bool has_bare = false;
    bool has_log_all_ref_updates = false;
    bool has_origin_fetch = false;
    std::optional<std::string> repository_format_version;
    std::optional<std::string> extension_object_format;

    std::size_t offset = 0;
    while(offset < result.output.size()) {
        const std::size_t end = result.output.find('\0', offset);
        if(end == std::string::npos || end == offset) {
            throw_unsafe_local_configuration();
        }
        const std::string record = result.output.substr(offset, end - offset);
        offset = end + 1;

        const std::size_t separator = record.find('\n');
        if(separator == std::string::npos || separator == 0) {
            throw_unsafe_local_configuration();
        }
        const std::string key = to_lower(record.substr(0, separator));
        const std::string value = record.substr(separator + 1);
        if(has_control_character(key) || has_control_character(value)) {
            throw_unsafe_local_configuration();
        }

        if(key == "core.repositoryformatversion") {
            require_unique_key(key_counts, key);
            repository_format_version = trim(value);
            has_repository_format = true;
            continue;
        }
        if(key == "core.filemode") {
            require_unique_key(key_counts, key);
            if(!is_boolean_value(value)) throw_unsafe_local_configuration();
            has_file_mode = true;
            continue;
        }
        if(key == "core.bare") {
            require_unique_key(key_counts, key);
            if(!is_false_value(value)) throw_unsafe_local_configuration();
            has_bare = true;
            continue;
        }
        if(key == "core.logallrefupdates") {
            require_unique_key(key_counts, key);
            if(!is_boolean_value(value)) throw_unsafe_local_configuration();
            has_log_all_ref_updates = true;
            continue;
        }
        if(key == "core.ignorecase" || key == "core.symlinks" ||
           key == "core.precomposeunicode") {
            require_unique_key(key_counts, key);
            if(!is_boolean_value(value)) throw_unsafe_local_configuration();
            continue;
        }
        if(key == "remote.origin.url") {
            require_unique_key(key_counts, key);
            if(trim(value).empty()) throw_unsafe_local_configuration();
            configuration.remote_origin_url = value;
            continue;
        }
        if(key == "remote.origin.fetch") {
            require_unique_key(key_counts, key);
            if(trim(value) != "+refs/heads/*:refs/remotes/origin/*") {
                throw_unsafe_local_configuration();
            }
            has_origin_fetch = true;
            continue;
        }
        if(key == "extensions.objectformat") {
            require_unique_key(key_counts, key);
            extension_object_format = trim(value);
            continue;
        }

        constexpr std::string_view branch_prefix = "branch.";
        constexpr std::string_view remote_suffix = ".remote";
        constexpr std::string_view merge_suffix = ".merge";
        if(key.starts_with(branch_prefix) &&
           (key.ends_with(remote_suffix) || key.ends_with(merge_suffix))) {
            const bool is_remote = key.ends_with(remote_suffix);
            const std::size_t suffix_size =
                is_remote ? remote_suffix.size() : merge_suffix.size();
            const std::string branch = key.substr(
                branch_prefix.size(),
                key.size() - branch_prefix.size() - suffix_size);
            if(!is_safe_branch_component(branch)) {
                throw_unsafe_local_configuration();
            }
            require_unique_key(key_counts, key);
            BranchConfigurationState& state = branch_states[branch];
            if(is_remote) {
                if(trim(value) != "origin") throw_unsafe_local_configuration();
                state.has_remote = true;
            } else {
                const std::string expected_merge = "refs/heads/" + branch;
                if(trim(value) != expected_merge) {
                    throw_unsafe_local_configuration();
                }
                state.has_merge = true;
            }
            continue;
        }

        // Moguet owns these clones. Unknown keys are refused rather than
        // attempting to maintain an incomplete Git command-injection denylist.
        throw_unsafe_local_configuration();
    }

    if(!has_repository_format || !has_file_mode || !has_bare ||
       !has_log_all_ref_updates || configuration.remote_origin_url.empty() ||
       !has_origin_fetch) {
        throw_unsafe_local_configuration();
    }
    for(const auto& [branch, state] : branch_states) {
        static_cast<void>(branch);
        if(!state.has_remote || !state.has_merge) {
            throw_unsafe_local_configuration();
        }
    }
    if(repository_format_version == std::optional<std::string>("0") &&
       !extension_object_format.has_value()) {
        configuration.object_format = GitObjectFormat::Sha1;
    } else if(
        repository_format_version == std::optional<std::string>("1") &&
        extension_object_format == std::optional<std::string>("sha256")) {
        configuration.object_format = GitObjectFormat::Sha256;
    } else {
        throw_unsafe_local_configuration();
    }
    return configuration;
}

std::vector<std::string> trusted_git_environment(
    const std::optional<std::string>& test_display_command,
    bool read_only_projection = false) {
    std::vector<std::string> environment =
        trusted_git_process_environment(
            read_only_projection
                ? TrustedGitProcessEnvironmentMode::ReadOnlyObservation
                : TrustedGitProcessEnvironmentMode::ManagedOperation);

#ifdef MOGUET_ENABLE_TEST_OVERRIDES
    for(char** current = ::environ;
        current != nullptr && *current != nullptr;
        ++current) {
        std::string assignment(*current);
        const std::size_t separator = assignment.find('=');
        if(separator == std::string::npos) continue;
        const std::string name = assignment.substr(0, separator);
        if(!name.starts_with("MOGUET_TEST_") ||
           name == "MOGUET_TEST_TRUSTED_GIT_DISPLAY_COMMAND" ||
           name == "MOGUET_TEST_TRUSTED_GIT_BOUNDARY") {
            continue;
        }
        environment.push_back(std::move(assignment));
    }
    environment.push_back("MOGUET_TEST_TRUSTED_GIT_BOUNDARY=1");
    if(test_display_command.has_value()) {
        environment.push_back(
            "MOGUET_TEST_TRUSTED_GIT_DISPLAY_COMMAND=" +
            test_display_command.value());
    }
#else
    static_cast<void>(test_display_command);
#endif
    return environment;
}

std::string trusted_git_executable() {
#ifdef MOGUET_ENABLE_TEST_OVERRIDES
    const char* explicit_executable =
        std::getenv("MOGUET_TEST_GIT_EXECUTABLE");
    if(explicit_executable != nullptr && explicit_executable[0] != '\0') {
        const fs::path candidate(explicit_executable);
        if(candidate.is_absolute() && access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
        // NO_TRANSLATE: Test-only override validation; unreachable in production builds.
        throw std::runtime_error("Invalid trusted Git test executable.");
    }

    const char* path_value = std::getenv("PATH");
    if(path_value != nullptr) {
        std::string path(path_value);
        std::size_t offset = 0;
        while(offset <= path.size()) {
            const std::size_t end = path.find(':', offset);
            const std::string component = path.substr(
                offset,
                end == std::string::npos ? std::string::npos
                                         : end - offset);
            const fs::path directory(component);
            if(directory.is_absolute()) {
                const fs::path candidate = directory / "git";
                if(access(candidate.c_str(), X_OK) == 0) {
                    return candidate.string();
                }
            }
            if(end == std::string::npos) break;
            offset = end + 1;
        }
    }
    // NO_TRANSLATE: Test-only harness setup failure; unreachable in production builds.
    throw std::runtime_error("Unable to locate trusted Git test executable.");
#else
    return "/usr/bin/git";
#endif
}

std::vector<std::string> common_git_arguments() {
    return trusted_git_managed_process_arguments();
}

ExplicitProcessInvocation isolated_invocation(
    std::vector<std::string> git_arguments,
    const std::optional<std::string>& test_display_command,
    bool read_only_projection = false) {
    return ExplicitProcessInvocation{
        trusted_git_executable(), std::move(git_arguments),
        trusted_git_environment(
            test_display_command, read_only_projection)};
}

std::vector<std::string> bound_git_arguments(
    const fs::path& checkout,
    std::vector<std::string> operation_arguments) {
    std::vector<std::string> arguments = common_git_arguments();
    arguments.push_back("--git-dir=" + (checkout / ".git").string());
    arguments.push_back("--work-tree=" + checkout.string());
    arguments.insert(
        arguments.end(),
        std::make_move_iterator(operation_arguments.begin()),
        std::make_move_iterator(operation_arguments.end()));
    return arguments;
}

std::vector<std::string> bound_review_git_arguments(
    const fs::path& checkout,
    std::vector<std::string> operation_arguments,
    const std::optional<std::string>& attribute_source = std::nullopt) {
    std::vector<std::string> arguments = common_git_arguments();
    arguments.push_back("--no-replace-objects");
    if(attribute_source.has_value()) {
        arguments.push_back("--attr-source=" + *attribute_source);
    }
    arguments.push_back("--git-dir=" + (checkout / ".git").string());
    arguments.push_back("--work-tree=" + checkout.string());
    arguments.insert(
        arguments.end(),
        std::make_move_iterator(operation_arguments.begin()),
        std::make_move_iterator(operation_arguments.end()));
    return arguments;
}

std::size_t review_machine_stream_limit() noexcept {
#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_GIT_TEST_HOOKS
    if(g_review_machine_stream_limit.has_value()) {
        return *g_review_machine_stream_limit;
    }
#endif
    return REVIEWED_SOURCE_MACHINE_STREAM_LIMIT;
}

CapturedCommandResult inspect_local_configuration_output(
    const fs::path& checkout,
    const std::optional<std::string>& test_display_command) {
    const std::vector<std::string> arguments = bound_git_arguments(
        checkout,
        {"config", "--local", "--no-includes", "--null", "--list"});
    ExplicitProcessInvocation invocation =
        isolated_invocation(arguments, test_display_command);
    invocation.stdout_capture_limit = MAX_LOCAL_CONFIG_OUTPUT + 1;
    return capture_explicit_process_output_raw(invocation, true);
}

LocalGitConfiguration inspect_managed_checkout_configuration(
    const ValidatedCachePath& checkout,
    const std::optional<std::string>& test_display_command =
        std::nullopt) {
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_descendants(current);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    LocalGitConfiguration configuration = parse_local_configuration(
        inspect_local_configuration_output(
            current.canonical_path(), test_display_command));
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_descendants(current);
    return configuration;
}

LocalGitConfiguration inspect_review_checkout_configuration(
    const ValidatedCachePath& checkout,
    const std::optional<std::string>& test_display_command =
        std::nullopt) {
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    LocalGitConfiguration configuration = parse_local_configuration(
        inspect_local_configuration_output(
            current.canonical_path(), test_display_command));
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    return configuration;
}

void require_expected_remote(
    const LocalGitConfiguration& configuration,
    const std::string& expected_remote_url) {
    if(!remote_url_matches_expected(
           configuration.remote_origin_url, expected_remote_url)) {
        throw std::runtime_error(localization::format_translated_message(
            "The remote URL changed before the managed {} operation.",
            "Git"));
    }
}

CapturedCommandResult capture_managed_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    std::vector<std::string> operation_arguments,
    const std::string& display_command,
    bool suppress_standard_error = false) {
    require_expected_remote(
        inspect_managed_checkout_configuration(checkout),
        expected_remote_url);
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    CapturedCommandResult result = capture_explicit_process_output_raw(
        isolated_invocation(
            bound_git_arguments(
                current.canonical_path(),
                std::move(operation_arguments)),
            display_command),
        suppress_standard_error);
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_descendants(current);
    return result;
}

CapturedCommandResult capture_review_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    std::vector<std::string> operation_arguments,
    const std::string& display_command,
    std::size_t stdout_limit,
    const std::optional<std::string>& attribute_source = std::nullopt,
    std::optional<int> standard_input_fd = std::nullopt) {
    require_expected_remote(
        inspect_review_checkout_configuration(checkout),
        expected_remote_url);
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    ExplicitProcessInvocation invocation = isolated_invocation(
        bound_review_git_arguments(
            current.canonical_path(),
            std::move(operation_arguments), attribute_source),
        display_command, true);
    invocation.stdout_capture_limit = stdout_limit;
    invocation.standard_input_fd = standard_input_fd;
    CapturedCommandResult result =
        capture_explicit_process_output_raw(invocation, true);
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    return result;
}

TrustedGitReviewFailure review_failure(
    TrustedGitReviewFailureReason reason,
    TrustedGitReviewStage stage) {
    return TrustedGitReviewFailure{reason, stage, std::nullopt};
}

TrustedGitReviewFailure command_failure(
    TrustedGitReviewStage stage, int exit_code) {
    TrustedGitReviewFailure failure = review_failure(
        TrustedGitReviewFailureReason::CommandFailed, stage);
    failure.exit_code = exit_code;
    return failure;
}

TrustedGitReviewFailure capture_limit_failure(
    TrustedGitReviewStage stage, std::size_t limit) {
    TrustedGitReviewFailure failure = review_failure(
        TrustedGitReviewFailureReason::CaptureLimitExceeded, stage);
    failure.limit = limit;
    return failure;
}

TrustedGitReviewStage review_stage_for_stream(
    ReviewedSourceMachineStream stream) noexcept {
    switch(stream) {
        case ReviewedSourceMachineStream::CommitResolution:
            return TrustedGitReviewStage::TargetValidation;
        case ReviewedSourceMachineStream::BaselineTree:
            return TrustedGitReviewStage::BaselineTree;
        case ReviewedSourceMachineStream::TargetTree:
            return TrustedGitReviewStage::TargetTree;
        case ReviewedSourceMachineStream::NameStatus:
            return TrustedGitReviewStage::NameStatus;
        case ReviewedSourceMachineStream::Numstat:
            return TrustedGitReviewStage::Numstat;
        case ReviewedSourceMachineStream::CrossStream:
            return TrustedGitReviewStage::CrossStream;
        case ReviewedSourceMachineStream::ResourcePreflight:
            return TrustedGitReviewStage::ResourcePreflight;
    }
    return TrustedGitReviewStage::CrossStream;
}

TrustedGitReviewFailure map_projection_failure(
    const ReviewedSourceProjectionFailure& source) {
    TrustedGitReviewFailureReason reason =
        TrustedGitReviewFailureReason::MalformedMachineOutput;
    switch(source.reason) {
        case ReviewedSourceProjectionFailureReason::MalformedMachineOutput:
            reason = TrustedGitReviewFailureReason::MalformedMachineOutput;
            break;
        case ReviewedSourceProjectionFailureReason::InconsistentMachineOutput:
            reason = TrustedGitReviewFailureReason::InconsistentMachineOutput;
            break;
        case ReviewedSourceProjectionFailureReason::RenameCandidateLimitExceeded:
            reason = TrustedGitReviewFailureReason::RenameCandidateLimitExceeded;
            break;
        case ReviewedSourceProjectionFailureReason::SingleBlobSizeLimitExceeded:
            reason = TrustedGitReviewFailureReason::SingleBlobSizeLimitExceeded;
            break;
        case ReviewedSourceProjectionFailureReason::AggregateBlobSizeLimitExceeded:
            reason = TrustedGitReviewFailureReason::AggregateBlobSizeLimitExceeded;
            break;
    }
    TrustedGitReviewFailure failure = review_failure(
        reason, review_stage_for_stream(source.stream));
    failure.record_index = source.record_index;
    failure.observed = source.observed;
    failure.limit = source.limit;
    return failure;
}

const std::string& require_known_commit(
    const SourceRevisionIdentity& revision) {
    if(revision.state() != SourceRevisionState::Known ||
       revision.git_commit() == nullptr ||
       revision.git_object_format() == nullptr) {
        throw std::invalid_argument(
            "Reviewed source Git operation requires a known complete commit.");
    }
    return *revision.git_commit();
}

struct ExactCommitUnavailable {};

using ExactTargetCommitValidationResult = std::variant<
    SourceRevisionIdentity,
    TrustedGitReviewFailure>;

using ExactBaselineCommitValidationResult = std::variant<
    SourceRevisionIdentity,
    ExactCommitUnavailable,
    TrustedGitReviewFailure>;

ExactTargetCommitValidationResult validate_exact_target_commit(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const SourceRevisionIdentity& expected,
    GitObjectFormat repository_format,
    TrustedGitReviewStage stage) {
    const std::string& object_id = require_known_commit(expected);
    CapturedCommandResult result = capture_review_git(
        checkout, expected_remote_url,
        {"rev-parse", "--verify", "--quiet",
         "--output-object-format=storage", "--end-of-options",
         object_id + "^{commit}"},
        "git rev-parse --verify <pinned-commit>^{commit}",
        MAX_COMMIT_OID_OUTPUT);
    if(result.stdout_capture_limit_exceeded) {
        return capture_limit_failure(stage, MAX_COMMIT_OID_OUTPUT);
    }
    if(result.exit_code != 0) {
        return command_failure(stage, result.exit_code);
    }
    ReviewedSourceCommitParseResult parsed =
        parse_reviewed_source_commit_output(result.output);
    if(std::holds_alternative<ReviewedSourceProjectionFailure>(parsed)) {
        TrustedGitReviewFailure failure = map_projection_failure(
            std::get<ReviewedSourceProjectionFailure>(parsed));
        failure.stage = stage;
        return failure;
    }
    SourceRevisionIdentity observed =
        std::get<SourceRevisionIdentity>(std::move(parsed));
    if(observed.git_object_format() == nullptr ||
       *observed.git_object_format() != repository_format) {
        return review_failure(
            TrustedGitReviewFailureReason::ObjectFormatMismatch, stage);
    }
    if(observed != expected) {
        return review_failure(
            TrustedGitReviewFailureReason::ObjectFormatMismatch, stage);
    }
    return observed;
}

ExactBaselineCommitValidationResult validate_exact_baseline_commit(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const SourceRevisionIdentity& expected,
    GitObjectFormat repository_format,
    const std::string& target_oid) {
    constexpr TrustedGitReviewStage STAGE =
        TrustedGitReviewStage::BaselineValidation;
    const std::string& object_id = require_known_commit(expected);
    if(expected.git_object_format() == nullptr ||
       *expected.git_object_format() != repository_format) {
        return ExactCommitUnavailable{};
    }

    CapturedCommandResult existence = capture_review_git(
        checkout, expected_remote_url,
        {"cat-file", "-e", object_id},
        "git cat-file -e <baseline-object>", MAX_EMPTY_COMMAND_OUTPUT);
    if(existence.stdout_capture_limit_exceeded) {
        return capture_limit_failure(STAGE, MAX_EMPTY_COMMAND_OUTPUT);
    }
    if(!existence.output.empty()) {
        return review_failure(
            TrustedGitReviewFailureReason::MalformedMachineOutput, STAGE);
    }
    if(existence.exit_code == 1) {
        // LANDMINE(#411): cat-file -e also returns 1 when a pack is present
        // but unreadable. Exact disambiguation still observes its index entry;
        // an unreadable index is caught by the descriptor-safe read proof.
        CapturedCommandResult indexed = capture_review_git(
            checkout, expected_remote_url,
            {"rev-parse", "--disambiguate=" + object_id},
            "git rev-parse --disambiguate=<baseline-object>",
            MAX_COMMIT_OID_OUTPUT);
        if(indexed.stdout_capture_limit_exceeded) {
            return capture_limit_failure(STAGE, MAX_COMMIT_OID_OUTPUT);
        }
        if(indexed.exit_code != 0) {
            return command_failure(STAGE, indexed.exit_code);
        }
        if(!indexed.output.empty()) {
            ReviewedSourceCommitParseResult parsed =
                parse_reviewed_source_commit_output(indexed.output);
            if(std::holds_alternative<ReviewedSourceProjectionFailure>(parsed)) {
                TrustedGitReviewFailure failure = map_projection_failure(
                    std::get<ReviewedSourceProjectionFailure>(parsed));
                failure.stage = STAGE;
                return failure;
            }
            const SourceRevisionIdentity& indexed_revision =
                std::get<SourceRevisionIdentity>(parsed);
            if(indexed_revision != expected) {
                return review_failure(
                    TrustedGitReviewFailureReason::MalformedMachineOutput,
                    STAGE);
            }
            return command_failure(STAGE, existence.exit_code);
        }

        try {
            require_readable_persistent_checkout_git_metadata(checkout);
        } catch(const TrustedCacheError& error) {
            if(error.failure().code == TrustedCacheErrorCode::PermissionDenied ||
               error.failure().code == TrustedCacheErrorCode::MetadataFailure) {
                return command_failure(STAGE, existence.exit_code);
            }
            throw;
        }

        const std::size_t integrity_limit = review_machine_stream_limit();
        CapturedCommandResult integrity = capture_review_git(
            checkout, expected_remote_url,
            {"fsck", "--full", "--no-dangling", "--no-progress",
             "--no-reflogs", "--no-cache", "--no-references",
             target_oid},
            "git fsck --full <pinned-target>",
            integrity_limit);
        if(integrity.stdout_capture_limit_exceeded) {
            return capture_limit_failure(STAGE, integrity_limit);
        }
        if(integrity.exit_code != 0) {
            return command_failure(STAGE, integrity.exit_code);
        }
        if(!integrity.output.empty()) {
            return review_failure(
                TrustedGitReviewFailureReason::MalformedMachineOutput,
                STAGE);
        }
        return ExactCommitUnavailable{};
    }
    if(existence.exit_code != 0) {
        return command_failure(STAGE, existence.exit_code);
    }

    CapturedCommandResult type = capture_review_git(
        checkout, expected_remote_url,
        {"cat-file", "-t", object_id},
        "git cat-file -t <baseline-object>", MAX_OBJECT_TYPE_OUTPUT);
    if(type.stdout_capture_limit_exceeded) {
        return capture_limit_failure(STAGE, MAX_OBJECT_TYPE_OUTPUT);
    }
    if(type.exit_code != 0) {
        return command_failure(STAGE, type.exit_code);
    }
    if(type.output == "commit\n") return expected;
    if(type.output == "blob\n" || type.output == "tree\n" ||
       type.output == "tag\n") {
        return ExactCommitUnavailable{};
    }
    return review_failure(
        TrustedGitReviewFailureReason::MalformedMachineOutput, STAGE);
}

std::optional<TrustedGitReviewFailure> require_non_shallow_repository(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url) {
    CapturedCommandResult result = capture_review_git(
        checkout, expected_remote_url,
        {"rev-parse", "--is-shallow-repository"},
        "git rev-parse --is-shallow-repository", MAX_SHALLOW_OUTPUT);
    if(result.stdout_capture_limit_exceeded) {
        return capture_limit_failure(
            TrustedGitReviewStage::ShallowRepositoryCheck,
            MAX_SHALLOW_OUTPUT);
    }
    if(result.exit_code != 0) {
        return command_failure(
            TrustedGitReviewStage::ShallowRepositoryCheck,
            result.exit_code);
    }
    if(result.output == "true\n") {
        return review_failure(
            TrustedGitReviewFailureReason::
                ShallowRepositoryUnsupported,
            TrustedGitReviewStage::ShallowRepositoryCheck);
    }
    if(result.output != "false\n") {
        return review_failure(
            TrustedGitReviewFailureReason::MalformedMachineOutput,
            TrustedGitReviewStage::ShallowRepositoryCheck);
    }
    return std::nullopt;
}

std::optional<TrustedGitReviewFailure> require_no_attribute_override(
    const ValidatedCachePath& checkout) {
    if(observe_persistent_checkout_review_overrides(checkout).has_attributes) {
        return review_failure(
            TrustedGitReviewFailureReason::LocalAttributeOverride,
            TrustedGitReviewStage::AttributeGuard);
    }
    return std::nullopt;
}

std::optional<TrustedGitReviewFailure> require_no_history_override(
    const ValidatedCachePath& checkout) {
    if(observe_persistent_checkout_review_overrides(checkout).has_grafts) {
        return review_failure(
            TrustedGitReviewFailureReason::LocalHistoryOverride,
            TrustedGitReviewStage::HistoryGuard);
    }
    return std::nullopt;
}

int run_managed_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    std::vector<std::string> operation_arguments,
    const std::string& display_command,
    std::optional<int> lifetime_guard_descriptor = std::nullopt) {
    require_expected_remote(
        inspect_managed_checkout_configuration(checkout),
        expected_remote_url);
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    Logger::raw_cmd(display_command);
    ExplicitProcessInvocation invocation = isolated_invocation(
        bound_git_arguments(
            current.canonical_path(),
            std::move(operation_arguments)),
        display_command);
    invocation.parent_independent_lifetime_guard_fd =
        lifetime_guard_descriptor;
    const int status = run_explicit_process(invocation);
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_descendants(current);
    return status;
}

std::string remote_ref_for_branch(const std::string& branch) {
    if(!is_safe_branch_component(branch)) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing an invalid managed {} branch name.", "Git"));
    }
    // NO_TRANSLATE: Literal Git remote-ref identity.
    return "origin/" + branch;
}

std::string full_remote_ref_for_branch(const std::string& branch) {
    return "refs/remotes/" + remote_ref_for_branch(branch);
}

std::vector<std::string> reviewed_source_diff_arguments(
    const std::string& output_option,
    const std::string& baseline,
    const std::string& target,
    bool detect_renames) {
    std::vector<std::string> arguments{
        "diff-tree", "--no-commit-id", "-r", "-z", output_option,
        "--no-relative", "--no-renames"};
    if(detect_renames) {
        arguments.push_back("--find-renames=50%");
        arguments.push_back("-l1000");
    }
    arguments.insert(
        arguments.end(),
        {"--diff-algorithm=myers", "--no-ext-diff", "--no-textconv",
         "--ignore-submodules=none", baseline, target, "--"});
    return arguments;
}

std::vector<std::string> reviewed_source_blob_patch_arguments(
    const std::string& old_object_id,
    const std::string& new_object_id) {
    return {
        "diff", "--patch", "--full-index", "--no-prefix",
        "--no-color", "--no-color-moved", "--no-ext-diff",
        "--no-textconv", "--no-renames", "--diff-algorithm=myers",
        "--no-indent-heuristic", "--inter-hunk-context=0",
        "--unified=3", "--text", old_object_id, new_object_id};
}

std::optional<OwnedFileDescriptor> make_standard_input_pipe(
    std::string_view input) {
    if(input.empty() || input.size() > REVIEW_BLOB_BATCH_INPUT_LIMIT) {
        return std::nullopt;
    }
    int descriptors[2] = {-1, -1};
    if(pipe2(descriptors, O_CLOEXEC) != 0) return std::nullopt;
    OwnedFileDescriptor reader(descriptors[0]);
    OwnedFileDescriptor writer(descriptors[1]);

    std::size_t offset = 0;
    while(offset < input.size()) {
        const ssize_t written = write(
            writer.get(), input.data() + offset, input.size() - offset);
        if(written == -1 && errno == EINTR) continue;
        if(written <= 0) return std::nullopt;
        offset += static_cast<std::size_t>(written);
    }
    writer = OwnedFileDescriptor{};
    return reader;
}

const SourceRevisionIdentity& materialization_target_revision(
    const ReviewedSourceProjection& projection) {
    return std::visit(
        [](const auto& value) -> const SourceRevisionIdentity& {
            using Value = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<Value, ReviewedSourceAlreadyReviewed>) {
                return value.revision;
            } else {
                return value.target;
            }
        },
        projection);
}

using TrustedGitReviewedSourceModelMaterializationResult = std::variant<
    ReviewedSourceMaterializedReview,
    ReviewedSourceReviewFailure,
    TrustedGitReviewFailure>;

using TrustedGitMaterializationFailure = std::variant<
    ReviewedSourceReviewFailure,
    TrustedGitReviewFailure>;

class TrustedGitProjectionStopped final {
public:
    explicit TrustedGitProjectionStopped(
        TrustedGitReviewFailure failure) noexcept
        : failure_(std::move(failure)) {
    }

    [[nodiscard]] const TrustedGitReviewFailure& failure() const noexcept {
        return failure_;
    }

private:
    TrustedGitReviewFailure failure_;
};

class TrustedGitMaterializationStopped final {
public:
    explicit TrustedGitMaterializationStopped(
        ReviewedSourceReviewFailure failure)
        : failure_(std::move(failure)) {
    }

    explicit TrustedGitMaterializationStopped(
        TrustedGitReviewFailure failure)
        : failure_(std::move(failure)) {
    }

    [[nodiscard]] const TrustedGitMaterializationFailure& failure()
        const noexcept {
        return failure_;
    }

private:
    TrustedGitMaterializationFailure failure_;
};

TrustedGitPinnedCheckoutFailure pinned_checkout_failure(
    TrustedGitPinnedCheckoutFailureReason reason,
    TrustedGitPinnedCheckoutStage stage) {
    return TrustedGitPinnedCheckoutFailure{
        reason, stage, std::nullopt, 0, 0, std::nullopt};
}

TrustedGitPinnedCheckoutFailure pinned_checkout_command_failure(
    TrustedGitPinnedCheckoutStage stage, int exit_code) {
    TrustedGitPinnedCheckoutFailure failure = pinned_checkout_failure(
        TrustedGitPinnedCheckoutFailureReason::CommandFailed, stage);
    failure.exit_code = exit_code;
    return failure;
}

TrustedGitPinnedCheckoutFailure pinned_checkout_capture_failure(
    TrustedGitPinnedCheckoutStage stage, std::size_t observed,
    std::size_t limit) {
    TrustedGitPinnedCheckoutFailure failure = pinned_checkout_failure(
        TrustedGitPinnedCheckoutFailureReason::CaptureLimitExceeded,
        stage);
    failure.observed = observed;
    failure.limit = limit;
    return failure;
}

TrustedGitPinnedCheckoutFailure pinned_checkout_boundary_failure(
    TrustedGitPinnedCheckoutStage stage,
    std::optional<TrustedCacheFailure> boundary = std::nullopt) {
    TrustedGitPinnedCheckoutFailure failure = pinned_checkout_failure(
        TrustedGitPinnedCheckoutFailureReason::CheckoutBoundaryInvalid,
        stage);
    failure.boundary_failure = std::move(boundary);
    return failure;
}

TrustedGitPinnedCheckoutFailure pinned_checkout_review_failure(
    const TrustedGitReviewFailure& review) {
    TrustedGitPinnedCheckoutFailureReason reason =
        TrustedGitPinnedCheckoutFailureReason::MalformedOutput;
    switch(review.reason) {
        case TrustedGitReviewFailureReason::CommandFailed:
            reason = TrustedGitPinnedCheckoutFailureReason::CommandFailed;
            break;
        case TrustedGitReviewFailureReason::CaptureLimitExceeded:
            reason = TrustedGitPinnedCheckoutFailureReason::CaptureLimitExceeded;
            break;
        case TrustedGitReviewFailureReason::LocalAttributeOverride:
            reason = TrustedGitPinnedCheckoutFailureReason::LocalAttributeOverride;
            break;
        case TrustedGitReviewFailureReason::LocalHistoryOverride:
            reason = TrustedGitPinnedCheckoutFailureReason::LocalHistoryOverride;
            break;
        case TrustedGitReviewFailureReason::ObjectFormatMismatch:
            reason = TrustedGitPinnedCheckoutFailureReason::ObjectFormatMismatch;
            break;
        case TrustedGitReviewFailureReason::MalformedMachineOutput:
        case TrustedGitReviewFailureReason::InconsistentMachineOutput:
        case TrustedGitReviewFailureReason::RenameCandidateLimitExceeded:
        case TrustedGitReviewFailureReason::SingleBlobSizeLimitExceeded:
        case TrustedGitReviewFailureReason::AggregateBlobSizeLimitExceeded:
        case TrustedGitReviewFailureReason::ShallowRepositoryUnsupported:
        case TrustedGitReviewFailureReason::ReviewIdentityMismatch:
            break;
    }
    TrustedGitPinnedCheckoutFailure failure = pinned_checkout_failure(
        reason, TrustedGitPinnedCheckoutStage::TargetValidation);
    failure.exit_code = review.exit_code;
    failure.observed = review.observed;
    failure.limit = review.limit;
    return failure;
}

CapturedCommandResult capture_pinned_checkout_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    std::vector<std::string> operation_arguments,
    const std::string& display_command,
    std::size_t stdout_limit) {
    require_expected_remote(
        inspect_review_checkout_configuration(checkout),
        expected_remote_url);
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    CapturedCommandResult result;
    {
        // Git has no directory-FD repository argument. Bind the child cwd to
        // the retained checkout inode and use only relative logical views so
        // a named-path replacement cannot receive this operation.
        WorkDirGuard workdir(current);
        ExplicitProcessInvocation invocation = isolated_invocation(
            bound_review_git_arguments(
                fs::path("."), std::move(operation_arguments)),
            display_command);
        invocation.stdout_capture_limit = stdout_limit;
        result = capture_explicit_process_output_raw(invocation, true);
    }
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    return result;
}

std::optional<TrustedGitPinnedCheckoutFailure>
validate_pinned_checkout_materialization(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity,
    PinnedCheckoutWorktreePolicy worktree_policy);

CapturedCommandResult capture_overlay_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    std::vector<std::string> operation_arguments,
    const std::string& display_command,
    std::size_t stdout_limit,
    const fs::path& alternate_index,
    int lifetime_guard_descriptor,
    const std::optional<std::string>& attribute_source = std::nullopt) {
    require_expected_remote(
        inspect_review_checkout_configuration(checkout),
        expected_remote_url);
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    CapturedCommandResult result;
    {
        WorkDirGuard workdir(current);
        ExplicitProcessInvocation invocation = isolated_invocation(
            bound_review_git_arguments(
                fs::path("."), std::move(operation_arguments),
                attribute_source),
            display_command);
        invocation.environment.push_back(
            "GIT_INDEX_FILE=" + alternate_index.string());
        invocation.environment.push_back(
            "GIT_OBJECT_DIRECTORY=" +
            (alternate_index.parent_path() / "objects").string());
        invocation.environment.push_back(
            "GIT_ALTERNATE_OBJECT_DIRECTORIES=" +
            (current.canonical_path() / ".git" / "objects").string());
        invocation.stdout_capture_limit = stdout_limit;
        // Captured explicit processes cannot use the mutator supervisor. This
        // projection never changes refs, the real index, or the worktree; if
        // the parent dies it cannot mint or return overlay/build authority.
        static_cast<void>(lifetime_guard_descriptor);
        result = capture_explicit_process_output_raw(invocation, true);
    }
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    return result;
}

std::variant<ReviewedSourceObjectId, TrustedGitPinnedCheckoutFailure>
parse_overlay_object_id(
    CapturedCommandResult result,
    GitObjectFormat expected_format,
    TrustedGitPinnedCheckoutStage stage) {
    if(result.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            stage, result.output.size(), MAX_COMMIT_OID_OUTPUT);
    }
    if(result.exit_code != 0) {
        return pinned_checkout_command_failure(stage, result.exit_code);
    }
    if(result.output.empty() || result.output.back() != '\n' ||
       result.output.find('\n') != result.output.size() - 1) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::MalformedOutput,
            stage);
    }
    result.output.pop_back();
    try {
        ReviewedSourceObjectId object_id =
            ReviewedSourceObjectId::make(std::move(result.output));
        if(object_id.format() != expected_format) {
            return pinned_checkout_failure(
                TrustedGitPinnedCheckoutFailureReason::
                    ObjectFormatMismatch,
                stage);
        }
        return object_id;
    } catch(const std::invalid_argument&) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::MalformedOutput,
            stage);
    }
}

enum class OverlayFilesystemEntryKind : unsigned char {
    Directory = 1,
    RegularFile = 2,
    Symlink = 3,
};

struct OverlayFilesystemEntry {
    std::string path;
    OverlayFilesystemEntryKind kind =
        OverlayFilesystemEntryKind::Directory;
    struct stat status{};
    std::string payload_identity;
};

struct OverlayFilesystemManifestState {
    TrustedGitPinnedCheckoutStage stage =
        TrustedGitPinnedCheckoutStage::OverlayObservation;
    std::vector<OverlayFilesystemEntry> entries;
    std::uintmax_t aggregate_regular_bytes = 0;
    std::size_t aggregate_path_bytes = 0;
};

using OverlayDirectoryNamesResult = std::variant<
    std::vector<std::string>,
    TrustedGitPinnedCheckoutFailure>;
using OverlayFilesystemManifestResult = std::variant<
    std::string,
    TrustedGitPinnedCheckoutFailure>;

bool stable_overlay_status_equal(
    const struct stat& left,
    const struct stat& right) noexcept {
    return left.st_dev == right.st_dev &&
           left.st_ino == right.st_ino &&
           left.st_mode == right.st_mode &&
           left.st_nlink == right.st_nlink &&
           left.st_uid == right.st_uid &&
           left.st_gid == right.st_gid &&
           left.st_rdev == right.st_rdev &&
           left.st_size == right.st_size &&
           left.st_blksize == right.st_blksize &&
           left.st_blocks == right.st_blocks &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
           left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec &&
           left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

bool overlay_path_less(
    const OverlayFilesystemEntry& left,
    const OverlayFilesystemEntry& right) {
    return std::lexicographical_compare(
        left.path.begin(), left.path.end(),
        right.path.begin(), right.path.end(),
        [](char left_byte, char right_byte) {
            return static_cast<unsigned char>(left_byte) <
                   static_cast<unsigned char>(right_byte);
        });
}

void append_overlay_manifest_integer(
    std::string& output,
    std::uint64_t value) {
    for(int shift = 56; shift >= 0; shift -= 8) {
        output.push_back(static_cast<char>(
            static_cast<unsigned char>(value >> shift)));
    }
}

void append_overlay_manifest_bytes(
    std::string& output,
    std::string_view bytes) {
    append_overlay_manifest_integer(output, bytes.size());
    output.append(bytes);
}

std::string serialize_overlay_filesystem_manifest(
    std::vector<OverlayFilesystemEntry> entries) {
    std::sort(entries.begin(), entries.end(), overlay_path_less);
    std::string manifest("moguet-reviewed-overlay-filesystem-v1");
    manifest.push_back('\0');
    append_overlay_manifest_integer(manifest, entries.size());
    for(const OverlayFilesystemEntry& entry : entries) {
        append_overlay_manifest_bytes(manifest, entry.path);
        manifest.push_back(static_cast<char>(entry.kind));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_mode));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_dev));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_ino));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_nlink));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_uid));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_gid));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_rdev));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_size));
        append_overlay_manifest_integer(
            manifest,
            static_cast<std::uint64_t>(entry.status.st_blksize));
        append_overlay_manifest_integer(
            manifest, static_cast<std::uint64_t>(entry.status.st_blocks));
        append_overlay_manifest_integer(
            manifest,
            static_cast<std::uint64_t>(entry.status.st_mtim.tv_sec));
        append_overlay_manifest_integer(
            manifest,
            static_cast<std::uint64_t>(entry.status.st_mtim.tv_nsec));
        append_overlay_manifest_integer(
            manifest,
            static_cast<std::uint64_t>(entry.status.st_ctim.tv_sec));
        append_overlay_manifest_integer(
            manifest,
            static_cast<std::uint64_t>(entry.status.st_ctim.tv_nsec));
        append_overlay_manifest_bytes(manifest, entry.payload_identity);
    }
    return manifest;
}

OverlayDirectoryNamesResult read_overlay_directory_names(
    int directory_descriptor,
    bool is_checkout_root,
    TrustedGitPinnedCheckoutStage stage) {
    const int scan_descriptor = ::openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) return pinned_checkout_boundary_failure(stage);
    DIR* raw_directory = ::fdopendir(scan_descriptor);
    if(raw_directory == nullptr) {
        static_cast<void>(::close(scan_descriptor));
        return pinned_checkout_boundary_failure(stage);
    }
    OwnedDirectoryStream directory(raw_directory);
    std::vector<std::string> names;
    errno = 0;
    while(dirent* entry = ::readdir(directory.get())) {
        const std::string_view name(entry->d_name);
        if(name == "." || name == ".." ||
           (is_checkout_root && name == ".git")) {
            continue;
        }
        names.emplace_back(name);
        if(names.size() > REVIEWED_SOURCE_REVIEW_ENTRY_LIMIT) {
            return pinned_checkout_capture_failure(
                stage, names.size(),
                REVIEWED_SOURCE_REVIEW_ENTRY_LIMIT);
        }
        errno = 0;
    }
    if(errno != 0) return pinned_checkout_boundary_failure(stage);
    std::sort(
        names.begin(), names.end(),
        [](const std::string& left, const std::string& right) {
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end(),
                [](char left_byte, char right_byte) {
                    return static_cast<unsigned char>(left_byte) <
                           static_cast<unsigned char>(right_byte);
                });
        });
    return names;
}

std::optional<TrustedGitPinnedCheckoutFailure>
add_overlay_manifest_entry(
    OverlayFilesystemManifestState& manifest,
    OverlayFilesystemEntry entry) {
    if(manifest.entries.size() >= REVIEWED_SOURCE_REVIEW_ENTRY_LIMIT) {
        return pinned_checkout_capture_failure(
            manifest.stage, manifest.entries.size() + 1,
            REVIEWED_SOURCE_REVIEW_ENTRY_LIMIT);
    }
    if(entry.path.size() > REVIEWED_SOURCE_OVERLAY_PATH_LIMIT) {
        return pinned_checkout_capture_failure(
            manifest.stage, entry.path.size(),
            REVIEWED_SOURCE_OVERLAY_PATH_LIMIT);
    }
    if(entry.path.size() >
       REVIEWED_SOURCE_MACHINE_STREAM_LIMIT -
           manifest.aggregate_path_bytes) {
        return pinned_checkout_capture_failure(
            manifest.stage,
            manifest.aggregate_path_bytes + entry.path.size(),
            REVIEWED_SOURCE_MACHINE_STREAM_LIMIT);
    }
    manifest.aggregate_path_bytes += entry.path.size();
    manifest.entries.push_back(std::move(entry));
    return std::nullopt;
}

std::variant<std::string, TrustedGitPinnedCheckoutFailure>
read_overlay_regular_file(
    int parent_descriptor,
    const std::string& name,
    const struct stat& named_status,
    OverlayFilesystemManifestState& manifest) {
    OwnedFileDescriptor file(::openat(
        parent_descriptor, name.c_str(),
        O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
    if(!file.valid()) return pinned_checkout_boundary_failure(manifest.stage);
    struct stat opened_status{};
    if(::fstat(file.get(), &opened_status) != 0 ||
       !S_ISREG(opened_status.st_mode) ||
       !stable_overlay_status_equal(named_status, opened_status) ||
       opened_status.st_size < 0) {
        return pinned_checkout_boundary_failure(manifest.stage);
    }
    const std::uintmax_t expected_size =
        static_cast<std::uintmax_t>(opened_status.st_size);
    if(expected_size > REVIEWED_SOURCE_SINGLE_BLOB_LIMIT) {
        return pinned_checkout_capture_failure(
            manifest.stage, static_cast<std::size_t>(expected_size),
            static_cast<std::size_t>(
                REVIEWED_SOURCE_SINGLE_BLOB_LIMIT));
    }
    if(expected_size > REVIEWED_SOURCE_AGGREGATE_BLOB_LIMIT -
                           manifest.aggregate_regular_bytes) {
        return pinned_checkout_capture_failure(
            manifest.stage,
            static_cast<std::size_t>(
                manifest.aggregate_regular_bytes + expected_size),
            static_cast<std::size_t>(
                REVIEWED_SOURCE_AGGREGATE_BLOB_LIMIT));
    }

    std::string content;
    content.reserve(static_cast<std::size_t>(expected_size));
    std::array<char, 64U * 1024U> buffer{};
    while(true) {
        ssize_t read_size;
        do {
            read_size = ::read(file.get(), buffer.data(), buffer.size());
        } while(read_size < 0 && errno == EINTR);
        if(read_size < 0) {
            return pinned_checkout_boundary_failure(manifest.stage);
        }
        if(read_size == 0) break;
        const std::size_t chunk_size =
            static_cast<std::size_t>(read_size);
        if(chunk_size > REVIEWED_SOURCE_SINGLE_BLOB_LIMIT - content.size()) {
            return pinned_checkout_capture_failure(
                manifest.stage, content.size() + chunk_size,
                static_cast<std::size_t>(
                    REVIEWED_SOURCE_SINGLE_BLOB_LIMIT));
        }
        content.append(buffer.data(), chunk_size);
    }

    struct stat final_status{};
    struct stat final_named_status{};
    if(::fstat(file.get(), &final_status) != 0 ||
       ::fstatat(
           parent_descriptor, name.c_str(), &final_named_status,
           AT_SYMLINK_NOFOLLOW) != 0 ||
       !stable_overlay_status_equal(opened_status, final_status) ||
       !stable_overlay_status_equal(opened_status, final_named_status) ||
       content.size() != expected_size) {
        return pinned_checkout_boundary_failure(manifest.stage);
    }
    manifest.aggregate_regular_bytes += expected_size;
    return reviewed_source_sha256_content_identity(content);
}

std::variant<std::string, TrustedGitPinnedCheckoutFailure>
read_overlay_symlink_target(
    int parent_descriptor,
    const std::string& name,
    const struct stat& named_status,
    TrustedGitPinnedCheckoutStage stage) {
    OwnedFileDescriptor link(::openat(
        parent_descriptor, name.c_str(),
        O_PATH | O_CLOEXEC | O_NOFOLLOW));
    if(!link.valid()) return pinned_checkout_boundary_failure(stage);
    struct stat opened_status{};
    if(::fstat(link.get(), &opened_status) != 0 ||
       !S_ISLNK(opened_status.st_mode) ||
       !stable_overlay_status_equal(named_status, opened_status)) {
        return pinned_checkout_boundary_failure(stage);
    }
    std::array<char, REVIEWED_SOURCE_OVERLAY_SYMLINK_TARGET_LIMIT + 1>
        target{};
    ssize_t target_size;
    do {
        target_size = ::readlinkat(
            link.get(), "", target.data(), target.size());
    } while(target_size < 0 && errno == EINTR);
    if(target_size < 0) return pinned_checkout_boundary_failure(stage);
    if(static_cast<std::size_t>(target_size) >
       REVIEWED_SOURCE_OVERLAY_SYMLINK_TARGET_LIMIT) {
        return pinned_checkout_capture_failure(
            stage, static_cast<std::size_t>(target_size),
            REVIEWED_SOURCE_OVERLAY_SYMLINK_TARGET_LIMIT);
    }
    struct stat final_status{};
    struct stat final_named_status{};
    if(::fstat(link.get(), &final_status) != 0 ||
       ::fstatat(
           parent_descriptor, name.c_str(), &final_named_status,
           AT_SYMLINK_NOFOLLOW) != 0 ||
       !stable_overlay_status_equal(opened_status, final_status) ||
       !stable_overlay_status_equal(opened_status, final_named_status)) {
        return pinned_checkout_boundary_failure(stage);
    }
    return std::string(
        target.data(), static_cast<std::size_t>(target_size));
}

std::optional<TrustedGitPinnedCheckoutFailure>
scan_overlay_filesystem_directory(
    int directory_descriptor,
    std::string relative_path,
    std::size_t depth,
    bool is_checkout_root,
    OverlayFilesystemManifestState& manifest) {
    if(depth > REVIEWED_SOURCE_OVERLAY_DEPTH_LIMIT) {
        return pinned_checkout_capture_failure(
            manifest.stage, depth,
            REVIEWED_SOURCE_OVERLAY_DEPTH_LIMIT);
    }
    struct stat initial_directory_status{};
    if(::fstat(directory_descriptor, &initial_directory_status) != 0 ||
       !S_ISDIR(initial_directory_status.st_mode)) {
        return pinned_checkout_boundary_failure(manifest.stage);
    }
    if(auto failure = add_overlay_manifest_entry(
           manifest,
           OverlayFilesystemEntry{
               std::move(relative_path),
               OverlayFilesystemEntryKind::Directory,
               initial_directory_status,
               {}})) {
        return failure;
    }
    const std::string current_path = manifest.entries.back().path;
    OverlayDirectoryNamesResult names_result = read_overlay_directory_names(
        directory_descriptor, is_checkout_root, manifest.stage);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &names_result)) {
        return std::move(*failure);
    }

    for(const std::string& name :
        std::get<std::vector<std::string>>(std::move(names_result))) {
        const std::string child_path = current_path.empty()
                                           ? name
                                           : current_path + "/" + name;
        if(child_path.size() > REVIEWED_SOURCE_OVERLAY_PATH_LIMIT) {
            return pinned_checkout_capture_failure(
                manifest.stage, child_path.size(),
                REVIEWED_SOURCE_OVERLAY_PATH_LIMIT);
        }
        struct stat named_status{};
        if(::fstatat(
               directory_descriptor, name.c_str(), &named_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            return pinned_checkout_boundary_failure(manifest.stage);
        }

        if(S_ISDIR(named_status.st_mode)) {
            OwnedFileDescriptor child(::openat(
                directory_descriptor, name.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            struct stat opened_status{};
            if(!child.valid() || ::fstat(child.get(), &opened_status) != 0 ||
               !stable_overlay_status_equal(named_status, opened_status)) {
                return pinned_checkout_boundary_failure(manifest.stage);
            }
            if(auto failure = scan_overlay_filesystem_directory(
                   child.get(), child_path, depth + 1, false,
                   manifest)) {
                return failure;
            }
            struct stat final_status{};
            struct stat final_named_status{};
            if(::fstat(child.get(), &final_status) != 0 ||
               ::fstatat(
                   directory_descriptor, name.c_str(),
                   &final_named_status, AT_SYMLINK_NOFOLLOW) != 0 ||
               !stable_overlay_status_equal(opened_status, final_status) ||
               !stable_overlay_status_equal(
                   opened_status, final_named_status)) {
                return pinned_checkout_boundary_failure(manifest.stage);
            }
            continue;
        }

        if(S_ISREG(named_status.st_mode)) {
            auto content = read_overlay_regular_file(
                directory_descriptor, name, named_status, manifest);
            if(auto* failure = std::get_if<
                   TrustedGitPinnedCheckoutFailure>(&content)) {
                return std::move(*failure);
            }
            if(auto failure = add_overlay_manifest_entry(
                   manifest,
                   OverlayFilesystemEntry{
                       child_path,
                       OverlayFilesystemEntryKind::RegularFile,
                       named_status,
                       std::get<std::string>(std::move(content))})) {
                return failure;
            }
            continue;
        }

        if(S_ISLNK(named_status.st_mode)) {
            auto target = read_overlay_symlink_target(
                directory_descriptor, name, named_status,
                manifest.stage);
            if(auto* failure = std::get_if<
                   TrustedGitPinnedCheckoutFailure>(&target)) {
                return std::move(*failure);
            }
            std::string target_bytes =
                std::get<std::string>(std::move(target));
            if(target_bytes.size() >
               REVIEWED_SOURCE_MACHINE_STREAM_LIMIT -
                   manifest.aggregate_path_bytes) {
                return pinned_checkout_capture_failure(
                    manifest.stage,
                    manifest.aggregate_path_bytes + target_bytes.size(),
                    REVIEWED_SOURCE_MACHINE_STREAM_LIMIT);
            }
            manifest.aggregate_path_bytes += target_bytes.size();
            if(auto failure = add_overlay_manifest_entry(
                   manifest,
                   OverlayFilesystemEntry{
                       child_path,
                       OverlayFilesystemEntryKind::Symlink,
                       named_status, std::move(target_bytes)})) {
                return failure;
            }
            continue;
        }

        // Never open or read FIFOs, sockets, devices, or unknown types.
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::
                UnsupportedOverlayEntry,
            manifest.stage);
    }

    struct stat final_directory_status{};
    if(::fstat(directory_descriptor, &final_directory_status) != 0 ||
       !stable_overlay_status_equal(
           initial_directory_status, final_directory_status)) {
        return pinned_checkout_boundary_failure(manifest.stage);
    }
    return std::nullopt;
}

OverlayFilesystemManifestResult project_overlay_filesystem_manifest(
    int checkout_descriptor,
    std::uintmax_t expected_device,
    std::uintmax_t expected_inode,
    TrustedGitPinnedCheckoutStage stage) {
    OwnedFileDescriptor root(::openat(
        checkout_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat root_status{};
    if(!root.valid() || ::fstat(root.get(), &root_status) != 0 ||
       !S_ISDIR(root_status.st_mode) ||
       static_cast<std::uintmax_t>(root_status.st_dev) != expected_device ||
       static_cast<std::uintmax_t>(root_status.st_ino) != expected_inode) {
        return pinned_checkout_boundary_failure(stage);
    }
    OverlayFilesystemManifestState manifest;
    manifest.stage = stage;
    if(auto failure = scan_overlay_filesystem_directory(
           root.get(), {}, 0, true, manifest)) {
        return std::move(*failure);
    }
    return serialize_overlay_filesystem_manifest(
        std::move(manifest.entries));
}

using OverlayTreeProjectionResult = std::variant<
    ReviewedSourceObjectId,
    TrustedGitPinnedCheckoutFailure>;

struct OverlayProjection {
    ReviewedSourceObjectId tree;
    std::string filesystem_manifest;

    bool operator==(const OverlayProjection&) const = default;
};

using OverlayProjectionResult = std::variant<
    OverlayProjection,
    TrustedGitPinnedCheckoutFailure>;

OverlayTreeProjectionResult project_pinned_checkout_overlay_tree(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity,
    int lifetime_guard_descriptor,
    TrustedGitPinnedCheckoutStage stage) {
    TemporaryOverlayIndex alternate_index;
    const std::string& expected_remote = identity.canonical_git_remote();

    CapturedCommandResult empty_tree_result = capture_overlay_git(
        checkout, expected_remote,
        {"hash-object", "-w", "-t", "tree", "/dev/null"},
        "git hash-object -w -t tree /dev/null",
        MAX_COMMIT_OID_OUTPUT, alternate_index.path(),
        lifetime_guard_descriptor);
    OverlayTreeProjectionResult empty_tree = parse_overlay_object_id(
        std::move(empty_tree_result), identity.git_object_format(),
        stage);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &empty_tree)) {
        return std::move(*failure);
    }
    const ReviewedSourceObjectId& empty_tree_id =
        std::get<ReviewedSourceObjectId>(empty_tree);
    if(empty_tree_id !=
       reviewed_source_empty_tree_object_id(identity.git_object_format())) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::MalformedOutput,
            stage);
    }

    CapturedCommandResult initialized = capture_overlay_git(
        checkout, expected_remote, {"read-tree", "--empty"},
        "git read-tree --empty", MAX_EMPTY_COMMAND_OUTPUT,
        alternate_index.path(), lifetime_guard_descriptor);
    if(initialized.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            stage, initialized.output.size(), MAX_EMPTY_COMMAND_OUTPUT);
    }
    if(initialized.exit_code != 0 || !initialized.output.empty()) {
        return pinned_checkout_command_failure(stage, initialized.exit_code);
    }

    // --force includes ignored entries. --attr-source=<empty-tree> and the
    // explicit autocrlf setting make the alternate index a projection of raw
    // worktree bytes/modes rather than caller- or worktree-owned filters.
    CapturedCommandResult added = capture_overlay_git(
        checkout, expected_remote,
        {"-c", "core.autocrlf=false", "-c", "core.filemode=true",
         "add", "--all", "--force", "--", "."},
        "git add --all --force -- .", MAX_EMPTY_COMMAND_OUTPUT,
        alternate_index.path(), lifetime_guard_descriptor,
        empty_tree_id.value());
    if(added.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            stage, added.output.size(), MAX_EMPTY_COMMAND_OUTPUT);
    }
    if(added.exit_code != 0 || !added.output.empty()) {
        return pinned_checkout_command_failure(stage, added.exit_code);
    }

    CapturedCommandResult entries = capture_overlay_git(
        checkout, expected_remote, {"ls-files", "--stage", "-z"},
        "git ls-files --stage -z", REVIEWED_SOURCE_MACHINE_STREAM_LIMIT,
        alternate_index.path(), lifetime_guard_descriptor);
    if(entries.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            stage, entries.output.size(),
            REVIEWED_SOURCE_MACHINE_STREAM_LIMIT);
    }
    if(entries.exit_code != 0) {
        return pinned_checkout_command_failure(stage, entries.exit_code);
    }
    std::size_t entry_offset = 0;
    while(entry_offset < entries.output.size()) {
        const std::size_t entry_end =
            entries.output.find('\0', entry_offset);
        if(entry_end == std::string::npos || entry_end == entry_offset) {
            return pinned_checkout_failure(
                TrustedGitPinnedCheckoutFailureReason::MalformedOutput,
                stage);
        }
        const std::string_view entry(
            entries.output.data() + entry_offset,
            entry_end - entry_offset);
        const bool representable =
            entry.starts_with("100644 ") ||
            entry.starts_with("100755 ") ||
            entry.starts_with("120000 ");
        if(!representable) {
            // A Gitlink hides the nested worktree behind one commit OID, so
            // later nested-file mutation would not change the sealed tree.
            return pinned_checkout_failure(
                TrustedGitPinnedCheckoutFailureReason::
                    UnsupportedOverlayEntry,
                stage);
        }
        entry_offset = entry_end + 1;
    }

    return parse_overlay_object_id(
        capture_overlay_git(
            checkout, expected_remote, {"write-tree"},
            "git write-tree", MAX_COMMIT_OID_OUTPUT,
            alternate_index.path(), lifetime_guard_descriptor),
        identity.git_object_format(), stage);
}

OverlayProjectionResult project_pinned_checkout_overlay(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity,
    int lifetime_guard_descriptor,
    TrustedGitPinnedCheckoutStage stage) {
    OverlayFilesystemManifestResult manifest_before =
        project_overlay_filesystem_manifest(
            lifetime_guard_descriptor, checkout.device(),
            checkout.inode(), stage);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &manifest_before)) {
        return std::move(*failure);
    }
    // The descriptor manifest runs first so a FIFO/socket/device is rejected
    // without allowing the alternate-index Git projection to open it.
    OverlayTreeProjectionResult tree = project_pinned_checkout_overlay_tree(
        checkout, identity, lifetime_guard_descriptor, stage);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(&tree)) {
        return std::move(*failure);
    }
    OverlayFilesystemManifestResult manifest_after =
        project_overlay_filesystem_manifest(
            lifetime_guard_descriptor, checkout.device(),
            checkout.inode(), stage);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &manifest_after)) {
        return std::move(*failure);
    }
    if(std::get<std::string>(manifest_before) !=
       std::get<std::string>(manifest_after)) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::OverlayMismatch,
            stage);
    }
    return OverlayProjection{
        std::get<ReviewedSourceObjectId>(std::move(tree)),
        std::get<std::string>(std::move(manifest_after))};
}

OverlayProjectionResult observe_stable_pinned_checkout_overlay(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity,
    int lifetime_guard_descriptor,
    PinnedCheckoutWorktreePolicy worktree_policy,
    TrustedGitPinnedCheckoutStage stage) {
    if(auto failure = validate_pinned_checkout_materialization(
           checkout, identity, worktree_policy)) {
        return *failure;
    }
    OverlayProjectionResult first = project_pinned_checkout_overlay(
        checkout, identity, lifetime_guard_descriptor, stage);
    if(std::holds_alternative<TrustedGitPinnedCheckoutFailure>(first)) {
        return std::get<TrustedGitPinnedCheckoutFailure>(std::move(first));
    }
    if(auto failure = validate_pinned_checkout_materialization(
           checkout, identity, worktree_policy)) {
        return *failure;
    }
    OverlayProjectionResult second = project_pinned_checkout_overlay(
        checkout, identity, lifetime_guard_descriptor, stage);
    if(std::holds_alternative<TrustedGitPinnedCheckoutFailure>(second)) {
        return std::get<TrustedGitPinnedCheckoutFailure>(std::move(second));
    }
    if(std::get<OverlayProjection>(first) !=
       std::get<OverlayProjection>(second)) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::OverlayMismatch,
            stage);
    }
    return std::get<OverlayProjection>(std::move(second));
}

int run_pinned_checkout_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    std::vector<std::string> operation_arguments,
    const std::string& display_command,
    int lifetime_guard_descriptor) {
    require_expected_remote(
        inspect_review_checkout_configuration(checkout),
        expected_remote_url);
    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    Logger::raw_cmd(display_command);
    int exit_code = 0;
    {
        WorkDirGuard workdir(current);
        ExplicitProcessInvocation invocation = isolated_invocation(
            bound_review_git_arguments(
                fs::path("."), std::move(operation_arguments)),
            display_command);
        invocation.parent_independent_lifetime_guard_fd =
            lifetime_guard_descriptor;
        exit_code = run_explicit_process(invocation);
    }
    retained.require_unchanged_identity();
    current = revalidate_trusted_cache_path(
        current, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    return exit_code;
}

std::optional<TrustedGitPinnedCheckoutFailure>
validate_pinned_checkout_materialization(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity,
    PinnedCheckoutWorktreePolicy worktree_policy =
        PinnedCheckoutWorktreePolicy::RequireClean) {
    constexpr std::size_t INDEX_OUTPUT_LIMIT =
        REVIEWED_SOURCE_MACHINE_STREAM_LIMIT;
    const std::string& expected_remote = identity.canonical_git_remote();
    const SourceRevisionIdentity& expected_revision =
        identity.target_revision();
    const std::string& expected_oid = require_known_commit(expected_revision);

    const LocalGitConfiguration configuration =
        inspect_review_checkout_configuration(checkout);
    if(!remote_url_matches_expected(
           configuration.remote_origin_url, expected_remote)) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::RemoteMismatch,
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }
    if(configuration.object_format != identity.git_object_format()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::ObjectFormatMismatch,
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }
    const PersistentCheckoutReviewOverrides overrides =
        observe_persistent_checkout_review_overrides(checkout);
    if(overrides.has_attributes) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::LocalAttributeOverride,
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }
    if(overrides.has_grafts) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::LocalHistoryOverride,
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }

    CapturedCommandResult head = capture_pinned_checkout_git(
        checkout, expected_remote,
        {"rev-parse", "--verify", "--output-object-format=storage",
         "--end-of-options", "HEAD^{commit}"},
        "git rev-parse --verify HEAD^{commit}",
        MAX_COMMIT_OID_OUTPUT);
    if(head.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            TrustedGitPinnedCheckoutStage::HeadVerification,
            head.output.size(), MAX_COMMIT_OID_OUTPUT);
    }
    if(head.exit_code != 0) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::HeadVerification,
            head.exit_code);
    }
    ReviewedSourceCommitParseResult parsed_head =
        parse_reviewed_source_commit_output(head.output);
    if(std::holds_alternative<ReviewedSourceProjectionFailure>(parsed_head)) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::MalformedOutput,
            TrustedGitPinnedCheckoutStage::HeadVerification);
    }
    if(std::get<SourceRevisionIdentity>(parsed_head) != expected_revision) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::TargetRevisionMismatch,
            TrustedGitPinnedCheckoutStage::HeadVerification);
    }

    CapturedCommandResult symbolic = capture_pinned_checkout_git(
        checkout, expected_remote,
        {"symbolic-ref", "--quiet", "HEAD"},
        "git symbolic-ref --quiet HEAD", MAX_LOCAL_CONFIG_OUTPUT);
    if(symbolic.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            TrustedGitPinnedCheckoutStage::HeadVerification,
            symbolic.output.size(), MAX_LOCAL_CONFIG_OUTPUT);
    }
    if(symbolic.exit_code == 0) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::AttachedHead,
            TrustedGitPinnedCheckoutStage::HeadVerification);
    }
    if(symbolic.exit_code != 1 || !symbolic.output.empty()) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::HeadVerification,
            symbolic.exit_code);
    }

    CapturedCommandResult index = capture_pinned_checkout_git(
        checkout, expected_remote,
        {"-c", "core.filemode=true", "diff", "--cached", "--quiet",
         "--no-ext-diff", "--no-textconv", "--no-renames",
         expected_oid, "--"},
        "git diff --cached --quiet <pinned-commit>",
        MAX_EMPTY_COMMAND_OUTPUT);
    if(index.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            TrustedGitPinnedCheckoutStage::IndexVerification,
            index.output.size(), MAX_EMPTY_COMMAND_OUTPUT);
    }
    if(index.exit_code == 1) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::DirtyIndex,
            TrustedGitPinnedCheckoutStage::IndexVerification);
    }
    if(index.exit_code != 0 || !index.output.empty()) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::IndexVerification,
            index.exit_code);
    }

    CapturedCommandResult index_flags = capture_pinned_checkout_git(
        checkout, expected_remote, {"ls-files", "-v", "-z"},
        "git ls-files -v -z", INDEX_OUTPUT_LIMIT);
    if(index_flags.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            TrustedGitPinnedCheckoutStage::IndexVerification,
            index_flags.output.size(), INDEX_OUTPUT_LIMIT);
    }
    if(index_flags.exit_code != 0) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::IndexVerification,
            index_flags.exit_code);
    }
    std::size_t offset = 0;
    while(offset < index_flags.output.size()) {
        const std::size_t end = index_flags.output.find('\0', offset);
        if(end == std::string::npos || end - offset < 3 ||
           index_flags.output[offset] != 'H' ||
           index_flags.output[offset + 1] != ' ') {
            return pinned_checkout_failure(
                TrustedGitPinnedCheckoutFailureReason::UnsafeIndexFlags,
                TrustedGitPinnedCheckoutStage::IndexVerification);
        }
        offset = end + 1;
    }

    CapturedCommandResult status = capture_pinned_checkout_git(
        checkout, expected_remote,
        {"-c", "core.filemode=true", "status", "--porcelain=v2", "-z",
         "--untracked-files=all", "--ignored=matching"},
        "git status --porcelain=v2 -z --untracked-files=all --ignored=matching",
        REVIEWED_SOURCE_MACHINE_STREAM_LIMIT);
    if(status.stdout_capture_limit_exceeded) {
        return pinned_checkout_capture_failure(
            TrustedGitPinnedCheckoutStage::WorktreeVerification,
            status.output.size(), REVIEWED_SOURCE_MACHINE_STREAM_LIMIT);
    }
    if(status.exit_code != 0) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::WorktreeVerification,
            status.exit_code);
    }
    if(!status.output.empty() &&
       worktree_policy ==
           PinnedCheckoutWorktreePolicy::RequireClean) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::DirtyWorktree,
            TrustedGitPinnedCheckoutStage::WorktreeVerification);
    }

    require_safe_persistent_checkout_descendants(checkout);
    return std::nullopt;
}

using PinnedCheckoutMaterialization = std::variant<
    TrustedGitPinnedCheckoutRevalidated,
    TrustedGitPinnedCheckoutFailure>;

PinnedCheckoutMaterialization materialize_pinned_checkout_tree(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity,
    int lifetime_guard_descriptor) {
    const LocalGitConfiguration configuration =
        inspect_review_checkout_configuration(checkout);
    if(!remote_url_matches_expected(
           configuration.remote_origin_url,
           identity.canonical_git_remote())) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::RemoteMismatch,
            TrustedGitPinnedCheckoutStage::TargetValidation);
    }
    if(configuration.object_format != identity.git_object_format()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::ObjectFormatMismatch,
            TrustedGitPinnedCheckoutStage::TargetValidation);
    }
    if(auto attributes = require_no_attribute_override(checkout)) {
        return pinned_checkout_review_failure(*attributes);
    }
    if(auto history = require_no_history_override(checkout)) {
        return pinned_checkout_review_failure(*history);
    }
    ExactTargetCommitValidationResult validated = validate_exact_target_commit(
        checkout, identity.canonical_git_remote(),
        identity.target_revision(), configuration.object_format,
        TrustedGitReviewStage::TargetValidation);
    if(const auto* failure =
           std::get_if<TrustedGitReviewFailure>(&validated)) {
        return pinned_checkout_review_failure(*failure);
    }

    const std::string& target_oid =
        *identity.target_revision().git_commit();
    const int checkout_exit = run_pinned_checkout_git(
        checkout, identity.canonical_git_remote(),
        {"-c", "core.filemode=true", "checkout", "--detach", "--force",
         "--no-recurse-submodules", target_oid},
        "git checkout --detach --force <pinned-commit>",
        lifetime_guard_descriptor);
    if(checkout_exit != 0) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::CheckoutMaterialization,
            checkout_exit);
    }

    const int clean_exit = run_pinned_checkout_git(
        checkout, identity.canonical_git_remote(),
        {"clean", "-ffdx", "--"}, "git clean -ffdx",
        lifetime_guard_descriptor);
    if(clean_exit != 0) {
        return pinned_checkout_command_failure(
            TrustedGitPinnedCheckoutStage::ResidueCleanup, clean_exit);
    }
    if(auto failure =
           validate_pinned_checkout_materialization(checkout, identity)) {
        return *failure;
    }
    return TrustedGitPinnedCheckoutRevalidated{};
}

std::string diff_range_for_branch(const std::string& branch) {
    // NO_TRANSLATE: Literal Git revision range.
    return "HEAD.." + remote_ref_for_branch(branch);
}

LocalGitConfiguration inspect_aur_export_configuration(
    const fs::path& anchored_checkout,
    const std::string& display_command) {
    return parse_local_configuration(inspect_local_configuration_output(
        anchored_checkout, display_command));
}

} // namespace

struct TrustedGitPinnedCheckout::State {
    const ValidatedCachePath checkout;
    const AurReviewedSourceReviewIdentity identity;

    State(
        ValidatedCachePath value_checkout,
        AurReviewedSourceReviewIdentity value_identity)
        : checkout(std::move(value_checkout)),
          identity(std::move(value_identity)) {
    }
};

TrustedGitPinnedCheckout::TrustedGitPinnedCheckout(
    ValidatedCachePath checkout,
    AurReviewedSourceReviewIdentity identity)
    : state_(std::make_unique<State>(
          std::move(checkout), std::move(identity))) {
}

TrustedGitPinnedCheckout::TrustedGitPinnedCheckout(
    TrustedGitPinnedCheckout&& other) noexcept = default;

TrustedGitPinnedCheckout& TrustedGitPinnedCheckout::operator=(
    TrustedGitPinnedCheckout&& other) noexcept = default;

TrustedGitPinnedCheckout::~TrustedGitPinnedCheckout() = default;

bool TrustedGitPinnedCheckout::valid() const noexcept {
    return state_ != nullptr;
}

const TrustedGitPinnedCheckout::State&
TrustedGitPinnedCheckout::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from pinned Git checkout has no authority.");
    }
    return *state_;
}

const AurReviewedSourceReviewIdentity&
TrustedGitPinnedCheckout::identity() const {
    return require_state().identity;
}

const std::filesystem::path&
TrustedGitPinnedCheckout::checkout_path() const {
    return require_state().checkout.canonical_path();
}

std::uintmax_t TrustedGitPinnedCheckout::checkout_device() const {
    return require_state().checkout.device();
}

std::uintmax_t TrustedGitPinnedCheckout::checkout_inode() const {
    return require_state().checkout.inode();
}

TrustedGitPinnedCheckoutOverlayObservation::
    TrustedGitPinnedCheckoutOverlayObservation(
        AurReviewedSourceReviewIdentity identity,
        std::uintmax_t checkout_device,
        std::uintmax_t checkout_inode,
        ReviewedSourceObjectId tree,
        std::string filesystem_manifest) noexcept
    : identity_(std::move(identity)), checkout_device_(checkout_device),
      checkout_inode_(checkout_inode), tree_(std::move(tree)),
      filesystem_manifest_(std::move(filesystem_manifest)) {
}

TrustedGitReviewedRecipeSnapshot::TrustedGitReviewedRecipeSnapshot(
    AurReviewedSourceReviewIdentity identity,
    ReviewedSourceObjectId git_tree_object_id,
    std::vector<Entry> entries) noexcept
    : identity_(std::move(identity)),
      git_tree_object_id_(std::move(git_tree_object_id)),
      entries_(std::move(entries)) {
}

const AurReviewedSourceReviewIdentity&
TrustedGitReviewedRecipeSnapshot::identity() const noexcept {
    return identity_;
}

const ReviewedSourceObjectId&
TrustedGitReviewedRecipeSnapshot::git_tree_object_id() const noexcept {
    return git_tree_object_id_;
}

std::size_t TrustedGitReviewedRecipeSnapshot::entry_count() const noexcept {
    return entries_.size();
}

TrustedGitPinnedCheckoutResult trusted_git_materialize_pinned_checkout(
    const ValidatedCachePath& checkout,
    AurReviewedSourceReviewIdentity identity,
    const ReviewedSourcePackageBaseLease& lease) {
    if(!lease.valid_ || lease.descriptor_ < 0 ||
       checkout.device() != lease.directory_.path().device() ||
       checkout.inode() != lease.directory_.path().inode()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
            TrustedGitPinnedCheckoutStage::TargetValidation);
    }
    try {
        lease.directory_.require_unchanged_identity();
        PinnedCheckoutMaterialization materialized =
            materialize_pinned_checkout_tree(
                checkout, identity, lease.descriptor_);
        if(const auto* failure =
               std::get_if<TrustedGitPinnedCheckoutFailure>(
                   &materialized)) {
            return *failure;
        }
        return TrustedGitPinnedCheckout(
            revalidate_trusted_cache_path(
                checkout,
                CachePathRequirement::ExistingDirectory),
            std::move(identity));
    } catch(const TrustedCacheError& error) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation,
            error.failure());
    } catch(const std::exception&) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }
}

TrustedGitPinnedCheckoutRevalidationResult
revalidate_trusted_git_pinned_checkout(
    const TrustedGitPinnedCheckout& checkout) {
    if(!checkout.valid()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }
    try {
        if(auto failure = validate_pinned_checkout_materialization(
               checkout.require_state().checkout,
               checkout.require_state().identity,
               PinnedCheckoutWorktreePolicy::RequireClean)) {
            return *failure;
        }
        return TrustedGitPinnedCheckoutRevalidated{};
    } catch(const TrustedCacheError& error) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation,
            error.failure());
    } catch(const std::exception&) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::BoundaryRevalidation);
    }
}

TrustedGitPinnedCheckoutOverlayObservationResult
observe_clean_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease) {
    if(!checkout.valid() || !lease.valid_ || lease.descriptor_ < 0 ||
       checkout.checkout_device() != lease.device() ||
       checkout.checkout_inode() != lease.inode()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
            TrustedGitPinnedCheckoutStage::OverlayObservation);
    }
    try {
        lease.require_unchanged_identity();
        const TrustedGitPinnedCheckout::State& state =
            checkout.require_state();
        OverlayProjectionResult projected =
            observe_stable_pinned_checkout_overlay(
                state.checkout, state.identity, lease.descriptor_,
                PinnedCheckoutWorktreePolicy::RequireClean,
                TrustedGitPinnedCheckoutStage::OverlayObservation);
        if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
               &projected)) {
            return std::move(*failure);
        }
        lease.require_unchanged_identity();
        OverlayProjection stable =
            std::get<OverlayProjection>(std::move(projected));
        return TrustedGitPinnedCheckoutOverlayObservation(
            state.identity, state.checkout.device(),
            state.checkout.inode(), std::move(stable.tree),
            std::move(stable.filesystem_manifest));
    } catch(const TrustedCacheError& error) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::OverlayObservation,
            error.failure());
    } catch(const std::exception&) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::OverlayObservation);
    }
}

TrustedGitPinnedCheckoutOverlayObservationResult
observe_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease) {
    if(!checkout.valid() || !lease.valid_ || lease.descriptor_ < 0 ||
       checkout.checkout_device() != lease.device() ||
       checkout.checkout_inode() != lease.inode()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
            TrustedGitPinnedCheckoutStage::OverlayObservation);
    }
    try {
        lease.require_unchanged_identity();
        const TrustedGitPinnedCheckout::State& state =
            checkout.require_state();
        OverlayProjectionResult projected =
            observe_stable_pinned_checkout_overlay(
                state.checkout, state.identity, lease.descriptor_,
                PinnedCheckoutWorktreePolicy::AllowEditorOverlay,
                TrustedGitPinnedCheckoutStage::OverlayObservation);
        if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
               &projected)) {
            return std::move(*failure);
        }
        lease.require_unchanged_identity();
        OverlayProjection stable =
            std::get<OverlayProjection>(std::move(projected));
        return TrustedGitPinnedCheckoutOverlayObservation(
            state.identity, state.checkout.device(),
            state.checkout.inode(), std::move(stable.tree),
            std::move(stable.filesystem_manifest));
    } catch(const TrustedCacheError& error) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::OverlayObservation,
            error.failure());
    } catch(const std::exception&) {
        return pinned_checkout_boundary_failure(
            TrustedGitPinnedCheckoutStage::OverlayObservation);
    }
}

TrustedGitPinnedCheckoutRevalidationResult
revalidate_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease,
    const TrustedGitPinnedCheckoutOverlayObservation& expected) {
    TrustedGitPinnedCheckoutOverlayObservationResult observed =
        observe_trusted_git_pinned_checkout_overlay(checkout, lease);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &observed)) {
        failure->stage = TrustedGitPinnedCheckoutStage::OverlayRevalidation;
        return std::move(*failure);
    }
    const auto& current =
        std::get<TrustedGitPinnedCheckoutOverlayObservation>(observed);
    if(current != expected) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::OverlayMismatch,
            TrustedGitPinnedCheckoutStage::OverlayRevalidation);
    }
    return TrustedGitPinnedCheckoutRevalidated{};
}

TrustedGitReviewedRecipeSnapshotResult
trusted_git_project_reviewed_recipe_snapshot(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease,
    const TrustedGitPinnedCheckoutOverlayObservation& expected) {
    if(!checkout.valid() || !lease.valid() ||
       checkout.identity() != expected.identity_ ||
       checkout.checkout_device() != expected.checkout_device_ ||
       checkout.checkout_inode() != expected.checkout_inode_ ||
       checkout.checkout_device() != lease.device() ||
       checkout.checkout_inode() != lease.inode()) {
        return pinned_checkout_failure(
            TrustedGitPinnedCheckoutFailureReason::InvalidCapability,
            TrustedGitPinnedCheckoutStage::OverlayRevalidation);
    }

    TrustedGitPinnedCheckoutRevalidationResult initial_revalidation =
        revalidate_trusted_git_pinned_checkout_overlay(
            checkout, lease, expected);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &initial_revalidation)) {
        return std::move(*failure);
    }

    const TrustedGitPinnedCheckout::State& state = checkout.require_state();
    const AurReviewedSourceReviewIdentity& identity = state.identity;
    const std::string& target_oid =
        require_known_commit(identity.target_revision());
    const LocalGitConfiguration configuration =
        inspect_review_checkout_configuration(state.checkout);
    require_expected_remote(configuration, identity.canonical_git_remote());
    if(configuration.object_format != identity.git_object_format() ||
       expected.tree_.format() != configuration.object_format) {
        return review_failure(
            TrustedGitReviewFailureReason::ObjectFormatMismatch,
            TrustedGitReviewStage::TargetValidation);
    }

    ExactTargetCommitValidationResult target_validation =
        validate_exact_target_commit(
            state.checkout, identity.canonical_git_remote(),
            identity.target_revision(), configuration.object_format,
            TrustedGitReviewStage::TargetValidation);
    if(auto* failure =
           std::get_if<TrustedGitReviewFailure>(&target_validation)) {
        return std::move(*failure);
    }

    const PersistentCheckoutReviewOverrides initial_overrides =
        observe_persistent_checkout_review_overrides(state.checkout);
    if(initial_overrides.has_attributes) {
        return review_failure(
            TrustedGitReviewFailureReason::LocalAttributeOverride,
            TrustedGitReviewStage::AttributeGuard);
    }
    if(initial_overrides.has_grafts) {
        return review_failure(
            TrustedGitReviewFailureReason::LocalHistoryOverride,
            TrustedGitReviewStage::HistoryGuard);
    }

    CapturedCommandResult captured_tree = capture_review_git(
        state.checkout, identity.canonical_git_remote(),
        {"rev-parse", "--verify", target_oid + "^{tree}"},
        "git rev-parse --verify <reviewed-commit>^{tree}",
        MAX_COMMIT_OID_OUTPUT);
    if(captured_tree.stdout_capture_limit_exceeded) {
        return capture_limit_failure(
            TrustedGitReviewStage::TargetTree,
            MAX_COMMIT_OID_OUTPUT);
    }
    if(captured_tree.exit_code != 0) {
        return command_failure(
            TrustedGitReviewStage::TargetTree,
            captured_tree.exit_code);
    }
    if(captured_tree.output.empty() || captured_tree.output.back() != '\n' ||
       captured_tree.output.find('\n') != captured_tree.output.size() - 1) {
        return review_failure(
            TrustedGitReviewFailureReason::MalformedMachineOutput,
            TrustedGitReviewStage::TargetTree);
    }
    ReviewedSourceObjectId exact_tree = [&captured_tree]() {
        std::string object_id = captured_tree.output;
        object_id.pop_back();
        return ReviewedSourceObjectId::make(std::move(object_id));
    }();
    if(exact_tree != expected.tree_) {
        return review_failure(
            TrustedGitReviewFailureReason::InconsistentMachineOutput,
            TrustedGitReviewStage::TargetTree);
    }

    const std::size_t stream_limit = review_machine_stream_limit();
    CapturedCommandResult captured_metadata = capture_review_git(
        state.checkout, identity.canonical_git_remote(),
        {"ls-tree", "-r", "-z", "--full-tree", "--no-abbrev",
         "--format=%(objectmode)%x00%(objecttype)%x00%(objectname)%x00%(objectsize)",
         target_oid, "--"},
        "git ls-tree -r -z --full-tree <reviewed-commit> metadata",
        stream_limit);
    if(captured_metadata.stdout_capture_limit_exceeded) {
        return capture_limit_failure(
            TrustedGitReviewStage::TargetTree, stream_limit);
    }
    if(captured_metadata.exit_code != 0) {
        return command_failure(
            TrustedGitReviewStage::TargetTree,
            captured_metadata.exit_code);
    }
    CapturedCommandResult captured_paths = capture_review_git(
        state.checkout, identity.canonical_git_remote(),
        {"ls-tree", "-r", "-z", "--full-tree", "--name-only",
         target_oid, "--"},
        "git ls-tree -r -z --full-tree <reviewed-commit> paths",
        stream_limit);
    if(captured_paths.stdout_capture_limit_exceeded) {
        return capture_limit_failure(
            TrustedGitReviewStage::TargetTree, stream_limit);
    }
    if(captured_paths.exit_code != 0) {
        return command_failure(
            TrustedGitReviewStage::TargetTree,
            captured_paths.exit_code);
    }
    ReviewedSourceTreeParseResult parsed_inventory =
        parse_reviewed_source_tree_output(
            captured_metadata.output, captured_paths.output,
            configuration.object_format,
            ReviewedSourceMachineStream::TargetTree);
    if(auto* failure = std::get_if<ReviewedSourceProjectionFailure>(
           &parsed_inventory)) {
        return map_projection_failure(*failure);
    }
    ReviewedSourceTreeInventory inventory =
        std::get<ReviewedSourceTreeInventory>(
            std::move(parsed_inventory));
    if(inventory.entries.size() > REVIEWED_SOURCE_REVIEW_ENTRY_LIMIT) {
        return ReviewedSourceReviewFailure{
            ReviewedSourceReviewFailureReason::ResourceLimitExceeded,
            ReviewedSourceReviewResourceKind::ReviewEntries,
            0, 0, 0, inventory.entries.size(),
            REVIEWED_SOURCE_REVIEW_ENTRY_LIMIT};
    }

    std::vector<ReviewedSourceBlobRequest> requests;
    requests.reserve(inventory.entries.size());
    std::uintmax_t aggregate_blob_size = 0;
    for(const ReviewedSourceFileVersion& entry : inventory.entries) {
        if(entry.mode() == ReviewedSourceFileMode::Gitlink) continue;
        if(!entry.blob_size().has_value()) {
            return review_failure(
                TrustedGitReviewFailureReason::InconsistentMachineOutput,
                TrustedGitReviewStage::TargetTree);
        }
        const std::uintmax_t blob_size = *entry.blob_size();
        if(blob_size > REVIEWED_SOURCE_SINGLE_BLOB_LIMIT) {
            TrustedGitReviewFailure failure = review_failure(
                TrustedGitReviewFailureReason::SingleBlobSizeLimitExceeded,
                TrustedGitReviewStage::ResourcePreflight);
            failure.observed = blob_size;
            failure.limit = REVIEWED_SOURCE_SINGLE_BLOB_LIMIT;
            return failure;
        }
        if(blob_size > REVIEWED_SOURCE_AGGREGATE_BLOB_LIMIT -
                           aggregate_blob_size) {
            TrustedGitReviewFailure failure = review_failure(
                TrustedGitReviewFailureReason::AggregateBlobSizeLimitExceeded,
                TrustedGitReviewStage::ResourcePreflight);
            failure.observed = aggregate_blob_size + blob_size;
            failure.limit = REVIEWED_SOURCE_AGGREGATE_BLOB_LIMIT;
            return failure;
        }
        aggregate_blob_size += blob_size;
        requests.push_back(
            ReviewedSourceBlobRequest{entry.object_id(), blob_size});
    }

    std::vector<ReviewedSourceRawBlob> blobs;
    blobs.reserve(requests.size());
    std::size_t request_offset = 0;
    while(request_offset < requests.size()) {
        std::vector<ReviewedSourceBlobRequest> batch;
        std::string input;
        std::uintmax_t payload_size = 0;
        while(request_offset < requests.size()) {
            const ReviewedSourceBlobRequest& request =
                requests[request_offset];
            const std::size_t input_record_size =
                request.object_id.value().size() + 1;
            const bool input_would_exceed =
                input_record_size > REVIEW_BLOB_BATCH_INPUT_LIMIT -
                                        input.size();
            const bool payload_would_exceed =
                request.expected_size > REVIEW_BLOB_BATCH_PAYLOAD_LIMIT -
                                            payload_size;
            if(!batch.empty() &&
               (input_would_exceed || payload_would_exceed)) {
                break;
            }
            if(input_would_exceed || payload_would_exceed) {
                TrustedGitReviewFailure failure = review_failure(
                    TrustedGitReviewFailureReason::SingleBlobSizeLimitExceeded,
                    TrustedGitReviewStage::BlobRead);
                failure.observed = request.expected_size;
                failure.limit = REVIEW_BLOB_BATCH_PAYLOAD_LIMIT;
                return failure;
            }
            batch.push_back(request);
            input += request.object_id.value();
            input.push_back('\0');
            payload_size += request.expected_size;
            ++request_offset;
        }

        ReviewedSourceBlobBatchSizeResult capture_size =
            reviewed_source_blob_batch_capture_size(batch);
        if(auto* failure = std::get_if<ReviewedSourceReviewFailure>(
               &capture_size)) {
            return std::move(*failure);
        }
        std::optional<OwnedFileDescriptor> input_pipe =
            make_standard_input_pipe(input);
        if(!input_pipe.has_value() || !input_pipe->valid()) {
            return command_failure(
                TrustedGitReviewStage::BlobRead, 127);
        }
        const std::size_t output_limit =
            std::get<std::size_t>(capture_size);
        CapturedCommandResult captured = capture_review_git(
            state.checkout, identity.canonical_git_remote(),
            {"cat-file",
             "--batch=%(objectname) %(objecttype) %(objectsize)",
             "-Z"},
            "git cat-file --batch=<reviewed-recipe-blobs> -Z",
            output_limit, std::nullopt, input_pipe->get());
        if(captured.stdout_capture_limit_exceeded) {
            return capture_limit_failure(
                TrustedGitReviewStage::BlobRead, output_limit);
        }
        if(captured.exit_code != 0) {
            return command_failure(
                TrustedGitReviewStage::BlobRead, captured.exit_code);
        }
        ReviewedSourceBlobBatchParseResult parsed =
            parse_reviewed_source_blob_batch_output(
                batch, captured.output);
        if(auto* failure = std::get_if<ReviewedSourceReviewFailure>(
               &parsed)) {
            return std::move(*failure);
        }
        std::vector<ReviewedSourceRawBlob> parsed_blobs =
            std::get<std::vector<ReviewedSourceRawBlob>>(
                std::move(parsed));
        blobs.insert(
            blobs.end(),
            std::make_move_iterator(parsed_blobs.begin()),
            std::make_move_iterator(parsed_blobs.end()));
    }

    std::vector<TrustedGitReviewedRecipeSnapshot::Entry> entries;
    entries.reserve(inventory.entries.size());
    std::size_t blob_index = 0;
    for(ReviewedSourceFileVersion& version : inventory.entries) {
        std::string bytes;
        if(version.mode() != ReviewedSourceFileMode::Gitlink) {
            if(blob_index >= blobs.size() ||
               blobs[blob_index].object_id != version.object_id()) {
                return review_failure(
                    TrustedGitReviewFailureReason::InconsistentMachineOutput,
                    TrustedGitReviewStage::CrossStream);
            }
            bytes = std::move(blobs[blob_index].bytes);
            ++blob_index;
        }
        entries.push_back(TrustedGitReviewedRecipeSnapshot::Entry{
            std::move(version), std::move(bytes)});
    }
    if(blob_index != blobs.size()) {
        return review_failure(
            TrustedGitReviewFailureReason::InconsistentMachineOutput,
            TrustedGitReviewStage::CrossStream);
    }

    TrustedGitPinnedCheckoutRevalidationResult final_revalidation =
        revalidate_trusted_git_pinned_checkout_overlay(
            checkout, lease, expected);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &final_revalidation)) {
        return std::move(*failure);
    }
    return TrustedGitReviewedRecipeSnapshot(
        identity, std::move(exact_tree), std::move(entries));
}

std::string trusted_git_remote_origin_url(
    const ValidatedCachePath& checkout) {
    return trim(inspect_managed_checkout_configuration(
                    checkout,
                    "git config --get remote.origin.url")
                    .remote_origin_url);
}

int trusted_git_fetch_origin(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url) {
    return run_managed_git(
        checkout, expected_remote_url,
        {"fetch", "--no-auto-maintenance", "--no-recurse-submodules",
         "origin"},
        "git fetch origin");
}

int trusted_git_fetch_origin(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const ReviewedSourcePackageBaseLease& lease) {
    lease.require_unchanged_identity();
    if(checkout.device() != lease.device() ||
       checkout.inode() != lease.inode()) {
        throw std::logic_error(
            "Managed Git fetch lease binding is inconsistent.");
    }
    const int status = run_managed_git(
        checkout, expected_remote_url,
        {"fetch", "--no-auto-maintenance", "--no-recurse-submodules",
         "origin"},
        "git fetch origin", lease.descriptor_);
    lease.require_unchanged_identity();
    return status;
}

std::string trusted_git_detect_remote_branch(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url) {
    CapturedCommandResult remote_head = capture_managed_git(
        checkout, expected_remote_url,
        {"symbolic-ref", "--quiet", "--short",
         "refs/remotes/origin/HEAD"},
        "git symbolic-ref --quiet --short refs/remotes/origin/HEAD 2>/dev/null",
        true);
    constexpr std::string_view prefix = "origin/";
    const std::string trimmed_head = trim(remote_head.output);
    if(remote_head.exit_code == 0 && trimmed_head.starts_with(prefix)) {
        const std::string branch = trimmed_head.substr(prefix.size());
        static_cast<void>(remote_ref_for_branch(branch));
        return branch;
    }

    for(const std::string& branch : {std::string("main"), std::string("master")}) {
        const std::string ref = "refs/remotes/origin/" + branch;
        const int status = run_managed_git(
            checkout, expected_remote_url,
            {"show-ref", "--verify", "--quiet", ref},
            "git show-ref --verify --quiet " + ref);
        if(status == 0) return branch;
    }
    return "master";
}

int trusted_git_diff_quiet(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    const std::string range = diff_range_for_branch(branch);
    return run_managed_git(
        checkout, expected_remote_url,
        {"diff", "--quiet", "--no-ext-diff", "--no-textconv", range,
         "--"},
        "git diff --quiet " + range);
}

std::string trusted_git_diff_name_only(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    const std::string range = diff_range_for_branch(branch);
    CapturedCommandResult result = capture_managed_git(
        checkout, expected_remote_url,
        {"diff", "--name-only", "--no-ext-diff", "--no-textconv",
         range, "--"},
        "git diff --name-only " + range +
            " 2>/dev/null",
        true);
    return result.exit_code == 0 ? result.output : "";
}

int trusted_git_show_diff(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    const std::string range = diff_range_for_branch(branch);
    return run_managed_git(
        checkout, expected_remote_url,
        {"diff", "--no-ext-diff", "--no-textconv", "--color=always",
         range, "--"},
        "git diff " + range + " --color=always");
}

int trusted_git_reset_hard(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    const std::string remote_ref = remote_ref_for_branch(branch);
    return run_managed_git(
        checkout, expected_remote_url,
        {"reset", "--hard", remote_ref, "--"},
        "git reset --hard " + remote_ref);
}

int trusted_git_reset_hard(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch,
    const ReviewedSourcePackageBaseLease& lease) {
    lease.require_unchanged_identity();
    if(checkout.device() != lease.device() ||
       checkout.inode() != lease.inode()) {
        throw std::logic_error(
            "Managed Git reset lease binding is inconsistent.");
    }
    const std::string remote_ref = remote_ref_for_branch(branch);
    const int status = run_managed_git(
        checkout, expected_remote_url,
        {"reset", "--hard", remote_ref, "--"},
        "git reset --hard " + remote_ref, lease.descriptor_);
    lease.require_unchanged_identity();
    return status;
}

TrustedGitCommitResolutionResult trusted_git_resolve_remote_commit(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch) {
    const LocalGitConfiguration configuration =
        inspect_review_checkout_configuration(checkout);
    require_expected_remote(configuration, expected_remote_url);
    const std::string ref = full_remote_ref_for_branch(branch);
    CapturedCommandResult result = capture_review_git(
        checkout, expected_remote_url,
        {"rev-parse", "--verify", "--output-object-format=storage",
         "--end-of-options", ref + "^{commit}"},
        "git rev-parse --verify " + ref + "^{commit}",
        MAX_COMMIT_OID_OUTPUT);
    if(result.stdout_capture_limit_exceeded) {
        return capture_limit_failure(
            TrustedGitReviewStage::TargetResolution,
            MAX_COMMIT_OID_OUTPUT);
    }
    if(result.exit_code != 0) {
        return command_failure(
            TrustedGitReviewStage::TargetResolution,
            result.exit_code);
    }
    ReviewedSourceCommitParseResult parsed =
        parse_reviewed_source_commit_output(result.output);
    if(std::holds_alternative<ReviewedSourceProjectionFailure>(parsed)) {
        TrustedGitReviewFailure failure = map_projection_failure(
            std::get<ReviewedSourceProjectionFailure>(parsed));
        failure.stage = TrustedGitReviewStage::TargetResolution;
        return failure;
    }
    SourceRevisionIdentity revision =
        std::get<SourceRevisionIdentity>(std::move(parsed));
    if(revision.git_object_format() == nullptr ||
       *revision.git_object_format() != configuration.object_format) {
        return review_failure(
            TrustedGitReviewFailureReason::ObjectFormatMismatch,
            TrustedGitReviewStage::TargetResolution);
    }
    return revision;
}

TrustedGitReviewedSourceProjectionResult trusted_git_project_reviewed_source(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const SourceRevisionIdentity& target,
    const std::optional<SourceRevisionIdentity>& baseline) {
    const std::string& target_oid = require_known_commit(target);
    const LocalGitConfiguration configuration =
        inspect_review_checkout_configuration(checkout);
    require_expected_remote(configuration, expected_remote_url);

    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    RetainedTrustedCacheDirectory outer =
        retain_trusted_cache_directory(current);
    outer.require_unchanged_identity();

    const auto finish = [&outer, &current](
                            TrustedGitReviewedSourceProjectionResult result) {
        outer.require_unchanged_identity();
        current = revalidate_trusted_cache_path(
            current, CachePathRequirement::ExistingDirectory);
        require_safe_persistent_checkout_git_metadata(current);
        return result;
    };

    const PersistentCheckoutReviewOverrides initial_overrides =
        observe_persistent_checkout_review_overrides(current);
    if(initial_overrides.has_attributes) {
        return finish(review_failure(
            TrustedGitReviewFailureReason::LocalAttributeOverride,
            TrustedGitReviewStage::AttributeGuard));
    }
    if(initial_overrides.has_grafts) {
        return finish(review_failure(
            TrustedGitReviewFailureReason::LocalHistoryOverride,
            TrustedGitReviewStage::HistoryGuard));
    }
    if(target.git_object_format() == nullptr ||
       *target.git_object_format() != configuration.object_format) {
        return finish(review_failure(
            TrustedGitReviewFailureReason::ObjectFormatMismatch,
            TrustedGitReviewStage::TargetValidation));
    }

    ExactTargetCommitValidationResult target_validation =
        validate_exact_target_commit(
            current, expected_remote_url, target,
            configuration.object_format,
            TrustedGitReviewStage::TargetValidation);
    if(std::holds_alternative<TrustedGitReviewFailure>(target_validation)) {
        return finish(std::get<TrustedGitReviewFailure>(target_validation));
    }
    if(const auto shallow_failure = require_non_shallow_repository(
           current, expected_remote_url)) {
        return finish(*shallow_failure);
    }

    if(baseline.has_value() && *baseline == target) {
        return finish(ReviewedSourceAlreadyReviewed{target});
    }

    bool is_rebaseline = false;
    bool detect_renames = false;
    std::optional<ReviewedSourceHistoryRelation> history_relation;
    if(baseline.has_value()) {
        ExactBaselineCommitValidationResult baseline_validation =
            validate_exact_baseline_commit(
                current, expected_remote_url, *baseline,
                configuration.object_format, target_oid);
        if(std::holds_alternative<TrustedGitReviewFailure>(
               baseline_validation)) {
            return finish(std::get<TrustedGitReviewFailure>(
                baseline_validation));
        }
        if(std::holds_alternative<ExactCommitUnavailable>(
               baseline_validation)) {
            is_rebaseline = true;
        } else {
            detect_renames = true;
            if(const auto history_override =
                   require_no_history_override(current)) {
                return finish(*history_override);
            }
            CapturedCommandResult ancestry = capture_review_git(
                current, expected_remote_url,
                {"merge-base", "--is-ancestor",
                 require_known_commit(*baseline), target_oid},
                "git merge-base --is-ancestor <baseline> <target>", 1);
            if(const auto history_override =
                   require_no_history_override(current)) {
                return finish(*history_override);
            }
            if(ancestry.stdout_capture_limit_exceeded) {
                return finish(capture_limit_failure(
                    TrustedGitReviewStage::AncestryCheck, 1));
            }
            if(!ancestry.output.empty()) {
                return finish(review_failure(
                    TrustedGitReviewFailureReason::MalformedMachineOutput,
                    TrustedGitReviewStage::AncestryCheck));
            }
            if(ancestry.exit_code == 0) {
                history_relation = ReviewedSourceHistoryRelation::Ancestor;
            } else if(ancestry.exit_code == 1) {
                history_relation = ReviewedSourceHistoryRelation::NonAncestor;
            } else {
                return finish(command_failure(
                    TrustedGitReviewStage::AncestryCheck,
                    ancestry.exit_code));
            }
        }
    }

    using InventoryReadResult = std::variant<
        ReviewedSourceTreeInventory,
        TrustedGitReviewFailure>;
    const std::size_t stream_limit = review_machine_stream_limit();
    const auto read_inventory = [&](
                                    const std::string& object_id,
                                    ReviewedSourceMachineStream stream,
                                    TrustedGitReviewStage stage)
        -> InventoryReadResult {
        CapturedCommandResult captured = capture_review_git(
            current, expected_remote_url,
            {"ls-tree", "-r", "-z", "--full-tree", "--no-abbrev",
             "--format=%(objectmode)%x00%(objecttype)%x00%(objectname)%x00%(objectsize)",
             object_id, "--"},
            "git ls-tree -r -z --full-tree <pinned-commit> metadata",
            stream_limit);
        if(captured.stdout_capture_limit_exceeded) {
            return capture_limit_failure(stage, stream_limit);
        }
        if(captured.exit_code != 0) {
            return command_failure(stage, captured.exit_code);
        }
        // LANDMINE(#411): Git 2.55 custom %(path) formatting C-quotes some
        // non-UTF-8/control-byte names even with -z. Keep metadata path-free
        // and bind it by record order to the separate raw --name-only -z stream.
        CapturedCommandResult captured_paths = capture_review_git(
            current, expected_remote_url,
            {"ls-tree", "-r", "-z", "--full-tree", "--name-only",
             object_id, "--"},
            "git ls-tree -r -z --full-tree <pinned-commit> paths",
            stream_limit);
        if(captured_paths.stdout_capture_limit_exceeded) {
            return capture_limit_failure(stage, stream_limit);
        }
        if(captured_paths.exit_code != 0) {
            return command_failure(stage, captured_paths.exit_code);
        }
        ReviewedSourceTreeParseResult parsed =
            parse_reviewed_source_tree_output(
                captured.output, captured_paths.output,
                configuration.object_format, stream);
        if(std::holds_alternative<ReviewedSourceProjectionFailure>(parsed)) {
            return map_projection_failure(
                std::get<ReviewedSourceProjectionFailure>(parsed));
        }
        return std::get<ReviewedSourceTreeInventory>(std::move(parsed));
    };

    InventoryReadResult target_read = read_inventory(
        target_oid, ReviewedSourceMachineStream::TargetTree,
        TrustedGitReviewStage::TargetTree);
    if(std::holds_alternative<TrustedGitReviewFailure>(target_read)) {
        return finish(std::get<TrustedGitReviewFailure>(target_read));
    }
    ReviewedSourceTreeInventory target_inventory =
        std::get<ReviewedSourceTreeInventory>(std::move(target_read));

    ReviewedSourceTreeInventory baseline_inventory;
    std::string diff_baseline = reviewed_source_empty_tree_object_id(
                                    configuration.object_format)
                                    .value();
    if(baseline.has_value() && !is_rebaseline) {
        diff_baseline = require_known_commit(*baseline);
        InventoryReadResult baseline_read = read_inventory(
            diff_baseline, ReviewedSourceMachineStream::BaselineTree,
            TrustedGitReviewStage::BaselineTree);
        if(std::holds_alternative<TrustedGitReviewFailure>(baseline_read)) {
            return finish(std::get<TrustedGitReviewFailure>(baseline_read));
        }
        baseline_inventory =
            std::get<ReviewedSourceTreeInventory>(
                std::move(baseline_read));
    }

    ReviewedSourceResourcePreflightResult resource_preflight =
        preflight_reviewed_source_projection_resources(
            baseline_inventory, target_inventory, detect_renames);
    if(std::holds_alternative<ReviewedSourceProjectionFailure>(
           resource_preflight)) {
        return finish(map_projection_failure(
            std::get<ReviewedSourceProjectionFailure>(
                resource_preflight)));
    }

    using DiffReadResult = std::variant<std::string, TrustedGitReviewFailure>;
    const auto read_diff = [&](
                               const std::string& output_option,
                               TrustedGitReviewStage stage)
        -> DiffReadResult {
        if(const auto attribute_override =
               require_no_attribute_override(current)) {
            return *attribute_override;
        }
        CapturedCommandResult captured = capture_review_git(
            current, expected_remote_url,
            reviewed_source_diff_arguments(
                output_option, diff_baseline, target_oid,
                detect_renames),
            output_option == "--name-status"
                ? "git diff-tree -z --name-status <baseline> <target>"
                : "git diff-tree -z --numstat <baseline> <target>",
            stream_limit, target_oid);
        if(const auto attribute_override =
               require_no_attribute_override(current)) {
            return *attribute_override;
        }
        if(captured.stdout_capture_limit_exceeded) {
            return capture_limit_failure(stage, stream_limit);
        }
        if(captured.exit_code != 0) {
            return command_failure(stage, captured.exit_code);
        }
        return std::move(captured.output);
    };

    DiffReadResult name_status = read_diff(
        "--name-status", TrustedGitReviewStage::NameStatus);
    if(std::holds_alternative<TrustedGitReviewFailure>(name_status)) {
        return finish(std::get<TrustedGitReviewFailure>(name_status));
    }
    DiffReadResult numstat = read_diff(
        "--numstat", TrustedGitReviewStage::Numstat);
    if(std::holds_alternative<TrustedGitReviewFailure>(numstat)) {
        return finish(std::get<TrustedGitReviewFailure>(numstat));
    }

    ReviewedSourceChangeAssemblyResult assembled =
        assemble_reviewed_source_changes(
            baseline_inventory, target_inventory,
            std::get<std::string>(name_status),
            std::get<std::string>(numstat), detect_renames);
    if(std::holds_alternative<ReviewedSourceProjectionFailure>(assembled)) {
        return finish(map_projection_failure(
            std::get<ReviewedSourceProjectionFailure>(assembled)));
    }
    std::vector<ReviewedSourceFileChange> changes =
        std::get<std::vector<ReviewedSourceFileChange>>(
            std::move(assembled));

    if(!baseline.has_value()) {
        return finish(ReviewedSourceInitialFullReview{
            target, std::move(changes)});
    }
    if(is_rebaseline) {
        return finish(ReviewedSourceRebaselineFullReview{
            *baseline, target,
            ReviewedSourceBaselineUnavailableReason::MissingOrNotCommit,
            std::move(changes)});
    }
    if(!history_relation.has_value()) {
        return finish(review_failure(
            TrustedGitReviewFailureReason::InconsistentMachineOutput,
            TrustedGitReviewStage::CrossStream));
    }
    return finish(ReviewedSourceUpdateReview{
        *baseline, target, *history_relation, std::move(changes)});
}

TrustedAurReviewedSourceProjection
project_aur_reviewed_source_from_trusted_git(
    const ValidatedCachePath& checkout,
    AurReviewedSourceReviewIdentity identity,
    std::optional<SourceRevisionIdentity> baseline) {
    TrustedGitReviewedSourceProjectionResult projected =
        trusted_git_project_reviewed_source(
            checkout, identity.canonical_git_remote(),
            identity.target_revision(), baseline);
    if(const auto* failure =
           std::get_if<TrustedGitReviewFailure>(&projected)) {
        throw TrustedGitProjectionStopped(*failure);
    }

    ReviewedSourceProjection projection = std::visit(
        [](auto&& value) -> ReviewedSourceProjection {
            using Value = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<Value, TrustedGitReviewFailure>) {
                throw std::logic_error(
                    "Trusted projection failure escaped its boundary.");
            } else {
                return ReviewedSourceProjection(std::move(value));
            }
        },
        std::move(projected));
    return TrustedAurReviewedSourceProjection(
        std::move(identity), std::move(baseline),
        std::move(projection));
}

TrustedGitAurReviewedSourceProjectionResult
trusted_git_project_aur_reviewed_source(
    const ValidatedCachePath& checkout,
    AurReviewedSourceReviewIdentity identity,
    std::optional<SourceRevisionIdentity> baseline) {
    try {
        return project_aur_reviewed_source_from_trusted_git(
            checkout, std::move(identity), std::move(baseline));
    } catch(const TrustedGitProjectionStopped& stopped) {
        return stopped.failure();
    }
}

namespace {

TrustedGitReviewedSourceModelMaterializationResult
materialize_reviewed_source_model_from_trusted_git(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const ReviewedSourceProjection& projection) {
    ReviewedSourceBlobRequestPlanResult planned =
        plan_reviewed_source_blob_requests(projection);
    if(std::holds_alternative<ReviewedSourceReviewFailure>(planned)) {
        return std::get<ReviewedSourceReviewFailure>(planned);
    }
    const auto& requests = std::get<std::vector<ReviewedSourceBlobRequest>>(
        planned);

    const LocalGitConfiguration configuration =
        inspect_review_checkout_configuration(checkout);
    require_expected_remote(configuration, expected_remote_url);
    const SourceRevisionIdentity& target =
        materialization_target_revision(projection);
    static_cast<void>(require_known_commit(target));
    if(target.git_object_format() == nullptr ||
       *target.git_object_format() != configuration.object_format) {
        return review_failure(
            TrustedGitReviewFailureReason::ObjectFormatMismatch,
            TrustedGitReviewStage::TargetValidation);
    }
    ExactTargetCommitValidationResult target_validation =
        validate_exact_target_commit(
            checkout, expected_remote_url, target,
            configuration.object_format,
            TrustedGitReviewStage::TargetValidation);
    if(std::holds_alternative<TrustedGitReviewFailure>(target_validation)) {
        return std::get<TrustedGitReviewFailure>(target_validation);
    }

    // AlreadyReviewed and a distinct same-tree update require no blob/patch
    // observation, but the trusted seal still proves remote/format/exact target.
    if(requests.empty()) {
        ReviewedSourceReviewPreparationResult prepared =
            prepare_reviewed_source_review(projection, {});
        if(std::holds_alternative<ReviewedSourceReviewFailure>(prepared)) {
            return std::get<ReviewedSourceReviewFailure>(prepared);
        }
        ReviewedSourceReviewFinalizationResult finalized =
            finalize_reviewed_source_review(
                std::get<ReviewedSourceReviewPreparation>(
                    std::move(prepared)),
                {});
        if(std::holds_alternative<ReviewedSourceReviewFailure>(finalized)) {
            return std::get<ReviewedSourceReviewFailure>(finalized);
        }
        return std::get<ReviewedSourceMaterializedReview>(
            std::move(finalized));
    }

    for(const ReviewedSourceBlobRequest& request : requests) {
        if(request.object_id.format() != configuration.object_format) {
            return review_failure(
                TrustedGitReviewFailureReason::ObjectFormatMismatch,
                TrustedGitReviewStage::BlobRead);
        }
    }

    ValidatedCachePath current = revalidate_trusted_cache_path(
        checkout, CachePathRequirement::ExistingDirectory);
    require_safe_persistent_checkout_git_metadata(current);
    RetainedTrustedCacheDirectory outer =
        retain_trusted_cache_directory(current);
    outer.require_unchanged_identity();

    const auto finish = [&outer, &current](
                            TrustedGitReviewedSourceModelMaterializationResult
                                result) {
        outer.require_unchanged_identity();
        current = revalidate_trusted_cache_path(
            current, CachePathRequirement::ExistingDirectory);
        require_safe_persistent_checkout_git_metadata(current);
        return result;
    };

    const PersistentCheckoutReviewOverrides initial_overrides =
        observe_persistent_checkout_review_overrides(current);
    if(initial_overrides.has_attributes) {
        return finish(review_failure(
            TrustedGitReviewFailureReason::LocalAttributeOverride,
            TrustedGitReviewStage::AttributeGuard));
    }
    if(initial_overrides.has_grafts) {
        return finish(review_failure(
            TrustedGitReviewFailureReason::LocalHistoryOverride,
            TrustedGitReviewStage::HistoryGuard));
    }

    std::vector<ReviewedSourceRawBlob> blobs;
    blobs.reserve(requests.size());
    std::size_t request_offset = 0;
    while(request_offset < requests.size()) {
        std::vector<ReviewedSourceBlobRequest> batch;
        std::string input;
        std::uintmax_t payload_size = 0;
        while(request_offset < requests.size()) {
            const ReviewedSourceBlobRequest& request = requests[request_offset];
            const std::size_t input_record_size =
                request.object_id.value().size() + 1;
            const bool input_would_exceed =
                input_record_size >
                REVIEW_BLOB_BATCH_INPUT_LIMIT - input.size();
            const bool payload_would_exceed =
                request.expected_size >
                REVIEW_BLOB_BATCH_PAYLOAD_LIMIT - payload_size;
            if(!batch.empty() &&
               (input_would_exceed || payload_would_exceed)) {
                break;
            }
            if(input_would_exceed || payload_would_exceed) {
                TrustedGitReviewFailure failure = review_failure(
                    TrustedGitReviewFailureReason::
                        SingleBlobSizeLimitExceeded,
                    TrustedGitReviewStage::BlobRead);
                failure.observed = request.expected_size;
                failure.limit = REVIEW_BLOB_BATCH_PAYLOAD_LIMIT;
                return finish(failure);
            }
            batch.push_back(request);
            input += request.object_id.value();
            input.push_back('\0');
            payload_size += request.expected_size;
            ++request_offset;
        }

        ReviewedSourceBlobBatchSizeResult capture_size =
            reviewed_source_blob_batch_capture_size(batch);
        if(std::holds_alternative<ReviewedSourceReviewFailure>(capture_size)) {
            return finish(std::get<ReviewedSourceReviewFailure>(capture_size));
        }
        std::optional<OwnedFileDescriptor> input_pipe =
            make_standard_input_pipe(input);
        if(!input_pipe.has_value() || !input_pipe->valid()) {
            return finish(command_failure(
                TrustedGitReviewStage::BlobRead, 127));
        }

        const std::size_t output_limit = std::get<std::size_t>(capture_size);
        CapturedCommandResult captured = capture_review_git(
            current, expected_remote_url,
            {"cat-file",
             "--batch=%(objectname) %(objecttype) %(objectsize)",
             "-Z"},
            "git cat-file --batch=<review-blobs> -Z",
            output_limit, std::nullopt, input_pipe->get());
        if(captured.stdout_capture_limit_exceeded) {
            return finish(capture_limit_failure(
                TrustedGitReviewStage::BlobRead, output_limit));
        }
        if(captured.exit_code != 0) {
            return finish(command_failure(
                TrustedGitReviewStage::BlobRead, captured.exit_code));
        }
        ReviewedSourceBlobBatchParseResult parsed =
            parse_reviewed_source_blob_batch_output(
                batch, captured.output);
        if(std::holds_alternative<ReviewedSourceReviewFailure>(parsed)) {
            return finish(std::get<ReviewedSourceReviewFailure>(parsed));
        }
        auto parsed_blobs = std::get<std::vector<ReviewedSourceRawBlob>>(
            std::move(parsed));
        blobs.insert(
            blobs.end(),
            std::make_move_iterator(parsed_blobs.begin()),
            std::make_move_iterator(parsed_blobs.end()));
    }

    ReviewedSourceReviewPreparationResult prepared =
        prepare_reviewed_source_review(projection, std::move(blobs));
    if(std::holds_alternative<ReviewedSourceReviewFailure>(prepared)) {
        return finish(std::get<ReviewedSourceReviewFailure>(prepared));
    }
    ReviewedSourceReviewPreparation preparation =
        std::get<ReviewedSourceReviewPreparation>(std::move(prepared));

    std::vector<ReviewedSourceRawPatch> patches;
    patches.reserve(preparation.patch_requests.size());
    std::uintmax_t aggregate_patch_size = 0;
    for(const ReviewedSourcePatchRequest& request :
        preparation.patch_requests) {
        CapturedCommandResult captured = capture_review_git(
            current, expected_remote_url,
            reviewed_source_blob_patch_arguments(
                request.old_object_id.value(),
                request.new_object_id.value()),
            "git diff <old-reviewed-blob> <new-reviewed-blob>",
            REVIEWED_SOURCE_SINGLE_RAW_PATCH_LIMIT);
        if(captured.stdout_capture_limit_exceeded) {
            return finish(capture_limit_failure(
                TrustedGitReviewStage::PatchGeneration,
                REVIEWED_SOURCE_SINGLE_RAW_PATCH_LIMIT));
        }
        if(captured.exit_code != 0) {
            return finish(command_failure(
                TrustedGitReviewStage::PatchGeneration,
                captured.exit_code));
        }
        if(captured.output.size() >
           REVIEWED_SOURCE_AGGREGATE_RAW_PATCH_LIMIT -
               aggregate_patch_size) {
            const std::uintmax_t observed = aggregate_patch_size +
                                            static_cast<std::uintmax_t>(captured.output.size());
            ReviewedSourceReviewResourceResult resource =
                preflight_reviewed_source_review_resource(
                    ReviewedSourceReviewResourceKind::
                        AggregateRawPatches,
                    observed);
            return finish(std::get<ReviewedSourceReviewFailure>(resource));
        }
        aggregate_patch_size += captured.output.size();
        patches.push_back(ReviewedSourceRawPatch{
            request.entry_index,
            request.old_object_id, request.new_object_id,
            std::move(captured.output)});
    }

    ReviewedSourceReviewFinalizationResult finalized =
        finalize_reviewed_source_review(
            std::move(preparation), std::move(patches));
    if(std::holds_alternative<ReviewedSourceReviewFailure>(finalized)) {
        return finish(std::get<ReviewedSourceReviewFailure>(finalized));
    }
    return finish(std::get<ReviewedSourceMaterializedReview>(
        std::move(finalized)));
}

} // namespace

ReviewedSourceVerifiedMaterializedReview
materialize_verified_review_from_trusted_git(
    const ValidatedCachePath& checkout,
    TrustedAurReviewedSourceProjection projection) {
    if(!projection.valid()) {
        throw TrustedGitMaterializationStopped(review_failure(
            TrustedGitReviewFailureReason::ReviewIdentityMismatch,
            TrustedGitReviewStage::TargetValidation));
    }
    TrustedGitReviewedSourceModelMaterializationResult materialized =
        materialize_reviewed_source_model_from_trusted_git(
            checkout,
            projection.identity().canonical_git_remote(),
            projection.projection());
    if(const auto* failure =
           std::get_if<ReviewedSourceReviewFailure>(&materialized)) {
        throw TrustedGitMaterializationStopped(*failure);
    }
    if(const auto* failure =
           std::get_if<TrustedGitReviewFailure>(&materialized)) {
        throw TrustedGitMaterializationStopped(*failure);
    }
    return ReviewedSourceVerifiedMaterializedReview(
        std::get<ReviewedSourceMaterializedReview>(
            std::move(materialized)));
}

TrustedGitReviewedSourceMaterializationResult
trusted_git_materialize_reviewed_source_review(
    const ValidatedCachePath& checkout,
    TrustedAurReviewedSourceProjection projection) {
    try {
        return materialize_verified_review_from_trusted_git(
            checkout, std::move(projection));
    } catch(const TrustedGitMaterializationStopped& stopped) {
        if(const auto* failure = std::get_if<ReviewedSourceReviewFailure>(
               &stopped.failure())) {
            return *failure;
        }
        return std::get<TrustedGitReviewFailure>(stopped.failure());
    }
}

TrustedAurReviewedSourceReview
materialize_aur_reviewed_source_from_trusted_git(
    const ValidatedCachePath& checkout,
    TrustedAurReviewedSourceProjection projection) {
    if(!projection.valid()) {
        throw TrustedGitMaterializationStopped(review_failure(
            TrustedGitReviewFailureReason::ReviewIdentityMismatch,
            TrustedGitReviewStage::TargetValidation));
    }

    AurReviewedSourceReviewIdentity identity = projection.identity();
    ReviewedSourceVerifiedMaterializedReview verified_review =
        materialize_verified_review_from_trusted_git(
            checkout, std::move(projection));
    return TrustedAurReviewedSourceReview(
        std::move(identity), std::move(verified_review));
}

TrustedGitAurReviewedSourceMaterializationResult
trusted_git_materialize_aur_reviewed_source_review(
    const ValidatedCachePath& checkout,
    TrustedAurReviewedSourceProjection projection) {
    try {
        return materialize_aur_reviewed_source_from_trusted_git(
            checkout, std::move(projection));
    } catch(const TrustedGitMaterializationStopped& stopped) {
        if(const auto* failure = std::get_if<ReviewedSourceReviewFailure>(
               &stopped.failure())) {
            return *failure;
        }
        return std::get<TrustedGitReviewFailure>(stopped.failure());
    }
}

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_GIT_TEST_HOOKS
void set_trusted_git_review_machine_stream_limit_for_test(
    std::optional<std::size_t> limit) {
    g_review_machine_stream_limit = limit;
}
#endif

namespace {

int clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url,
    std::optional<int> lifetime_guard_descriptor) {
    ValidatedCachePath current = revalidate_trusted_cache_path(
        destination, CachePathRequirement::ExistingDirectory);
    RetainedTrustedCacheDirectory retained =
        retain_trusted_cache_directory(current);
    retained.require_unchanged_identity();
    const std::string leaf = current.path().filename().string();
    const std::string display_command =
        "git clone " + remote_url + " " + leaf;
    Logger::raw_cmd(display_command);
    std::vector<std::string> arguments = common_git_arguments();
    arguments.insert(
        arguments.end(),
        {"clone", "--no-recurse-submodules", "--", remote_url,
         current.canonical_path().string()});
    ExplicitProcessInvocation invocation = isolated_invocation(
        std::move(arguments), display_command);
    invocation.parent_independent_lifetime_guard_fd =
        lifetime_guard_descriptor;
    const int status = run_explicit_process(invocation);
    retained.require_unchanged_identity();
    return status;
}

} // namespace

int trusted_git_clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url) {
    return clone_persistent_checkout(
        destination, remote_url, std::nullopt);
}

int trusted_git_clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url,
    const ReviewedSourcePackageBaseLease& lease) {
    lease.require_unchanged_identity();
    if(destination.device() != lease.device() ||
       destination.inode() != lease.inode()) {
        throw std::logic_error(
            "Managed Git clone lease binding is inconsistent.");
    }
    const int status = clone_persistent_checkout(
        destination, remote_url, lease.descriptor_);
    lease.require_unchanged_identity();
    return status;
}

int trusted_git_clone_aur_export(
    const std::string& remote_url,
    const std::filesystem::path& anchored_destination) {
    const std::string display_command =
        "git clone --quiet -- " + remote_url + " " +
        anchored_destination.string() +
        " > /dev/null";
    Logger::raw_cmd(display_command);
    std::vector<std::string> arguments = common_git_arguments();
    arguments.insert(
        arguments.end(),
        {"clone", "--quiet", "--no-recurse-submodules", "--",
         remote_url, anchored_destination.string()});
    return run_explicit_process(
        isolated_invocation(std::move(arguments), display_command),
        true);
}

std::string trusted_git_aur_export_remote_origin_url(
    const std::filesystem::path& anchored_checkout) {
    const std::string display_command =
        "git -C " + anchored_checkout.string() +
        " config --local --get remote.origin.url";
    return trim(inspect_aur_export_configuration(
                    anchored_checkout, display_command)
                    .remote_origin_url);
}
