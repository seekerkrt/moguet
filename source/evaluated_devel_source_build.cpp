#include "evaluated_devel_source_build.hpp"

#include "artifact_archive_metadata.hpp"
#include "git_remote_revision_observer.hpp"
#include "trusted_git_process_policy.hpp"
#include "xdg_generation_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <linux/openat2.h>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

constexpr std::string_view WORKING_RECIPE_LEAF =
    ".moguet-evaluated-recipe";
constexpr mode_t WORKING_DIRECTORY_MODE = 0500;
constexpr mode_t WORKING_FILE_MODE = 0400;
constexpr mode_t WORKING_EXECUTABLE_MODE = 0500;
constexpr mode_t MUTABLE_PKGBUILD_MODE = 0600;
constexpr std::size_t MAX_RECIPE_ENTRIES = 4096;
constexpr std::size_t MAX_RECIPE_DEPTH = 256;
constexpr std::uintmax_t MAX_RECIPE_FILE_BYTES = 64U * 1024U * 1024U;
constexpr std::size_t MAX_SOURCE_ENTRIES = 64;
constexpr std::size_t MAX_FILESYSTEM_ENTRIES = 4096;
constexpr std::size_t MAX_FILESYSTEM_DEPTH = 128;
constexpr std::size_t MAX_GIT_METADATA_ENTRIES = 65536;
constexpr std::size_t MAX_SRCINFO_BYTES = 16U * 1024U * 1024U;
constexpr std::size_t MAX_PACKAGELIST_BYTES = 1U * 1024U * 1024U;
constexpr std::size_t MAX_GIT_OUTPUT_BYTES = 16U * 1024U;
constexpr std::size_t MAX_MAKEPKG_OUTPUT_BYTES = 64U * 1024U * 1024U;
constexpr std::size_t MAX_ARCHIVE_LISTING_BYTES = 8U * 1024U * 1024U;
constexpr std::size_t MAX_ARCHIVE_ENTRIES = 100000;
constexpr std::size_t MAX_ARCHIVE_MEMBER_PATH_BYTES = 4096;
constexpr std::size_t MAX_PKGINFO_BYTES = 1U * 1024U * 1024U;
constexpr std::size_t MAX_MTREE_BYTES = 16U * 1024U * 1024U;
constexpr std::uintmax_t MAX_ARTIFACT_BYTES =
    16ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::chrono::seconds METADATA_PROCESS_TIMEOUT{30};
constexpr std::chrono::minutes PREPARATION_PROCESS_TIMEOUT{30};
constexpr std::chrono::hours BUILD_PROCESS_TIMEOUT{2};
constexpr std::chrono::milliseconds PROCESS_TERMINATION_GRACE{500};

#ifdef MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
EvaluatedDevelSourceBuildTestHook g_build_test_hook;

void notify_test_event(
    EvaluatedDevelSourceBuildTestEvent event,
    const fs::path& owned_root,
    const fs::path& artifact_path = {}) {
    if(g_build_test_hook) {
        g_build_test_hook(event, owned_root, artifact_path);
    }
}
#else
void notify_test_event(
    EvaluatedDevelSourceBuildTestEvent,
    const fs::path&,
    const fs::path& = {}) {
}
#endif

EvaluatedDevelSourceBuildFailure build_failure(
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    EvaluatedDevelSourceBuildFailure failure;
    failure.stage = stage;
    failure.reason = reason;
    return failure;
}

class BuildFailureError final : public std::exception {
public:
    explicit BuildFailureError(
        EvaluatedDevelSourceBuildFailure failure) noexcept
        : failure_(std::move(failure)) {
    }

    [[nodiscard]] const char* what() const noexcept override {
        return "evaluated-devel-source-build-failure";
    }

    [[nodiscard]] EvaluatedDevelSourceBuildFailure release() noexcept {
        return std::move(failure_);
    }

private:
    EvaluatedDevelSourceBuildFailure failure_;
};

[[noreturn]] void throw_build_failure(
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason,
    std::optional<int> error_number = std::nullopt,
    std::optional<std::string> diagnostic = std::nullopt) {
    EvaluatedDevelSourceBuildFailure failure =
        build_failure(stage, reason);
    if(error_number.has_value()) {
        failure.system_error = std::error_code(
            *error_number, std::generic_category());
    }
    failure.diagnostic = std::move(diagnostic);
    throw BuildFailureError(std::move(failure));
}

[[noreturn]] void throw_process_failure(
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason,
    EvaluatedDevelSourceBuildProcess process,
    BoundedProcessOutcome outcome) {
    EvaluatedDevelSourceBuildFailure failure =
        build_failure(stage, reason);
    failure.process = process;
    failure.process_outcome = std::move(outcome);
    throw BuildFailureError(std::move(failure));
}

class OwnedDescriptor final {
public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

    OwnedDescriptor(OwnedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept {
        if(this == &other) return *this;
        reset(std::exchange(other.descriptor_, -1));
        return *this;
    }

    ~OwnedDescriptor() noexcept {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return descriptor_ >= 0;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int replacement = -1) noexcept {
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
        descriptor_ = replacement;
    }

private:
    int descriptor_ = -1;
};

struct NodeIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
    std::uintmax_t size = 0;
    std::uintmax_t links = 0;
    mode_t type = 0;
    timespec modification_time{};
    timespec change_time{};
};

NodeIdentity node_identity(const struct stat& status) noexcept {
    return NodeIdentity{
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_uid),
        static_cast<std::uintmax_t>(status.st_mode & 07777),
        status.st_size < 0 ? 0U
                           : static_cast<std::uintmax_t>(status.st_size),
        static_cast<std::uintmax_t>(status.st_nlink),
        static_cast<mode_t>(status.st_mode & S_IFMT),
        status.st_mtim,
        status.st_ctim};
}

bool same_node(
    const NodeIdentity& expected,
    const NodeIdentity& actual) noexcept {
    return expected.device == actual.device &&
           expected.inode == actual.inode &&
           expected.type == actual.type;
}

bool same_stable_file(
    const NodeIdentity& expected,
    const NodeIdentity& actual) noexcept {
    return same_node(expected, actual) &&
           expected.owner == actual.owner &&
           expected.mode == actual.mode &&
           expected.size == actual.size &&
           expected.links == actual.links &&
           expected.modification_time.tv_sec ==
               actual.modification_time.tv_sec &&
           expected.modification_time.tv_nsec ==
               actual.modification_time.tv_nsec &&
           expected.change_time.tv_sec == actual.change_time.tv_sec &&
           expected.change_time.tv_nsec == actual.change_time.tv_nsec;
}

NodeIdentity descriptor_identity(
    int descriptor,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    struct stat status{};
    if(descriptor < 0 || ::fstat(descriptor, &status) != 0) {
        throw_build_failure(stage, reason, errno);
    }
    return node_identity(status);
}

std::vector<std::string> directory_names(
    int directory_descriptor,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason,
    std::size_t limit = MAX_FILESYSTEM_ENTRIES) {
    int scan_descriptor;
    do {
        scan_descriptor = ::openat(
            directory_descriptor, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while(scan_descriptor < 0 && errno == EINTR);
    if(scan_descriptor < 0) {
        throw_build_failure(stage, reason, errno);
    }
    OwnedDescriptor scan(scan_descriptor);
    DIR* raw_directory = ::fdopendir(scan.release());
    if(raw_directory == nullptr) {
        throw_build_failure(stage, reason, errno);
    }
    std::unique_ptr<DIR, int (*)(DIR*)> directory(
        raw_directory, ::closedir);
    std::vector<std::string> names;
    errno = 0;
    while(dirent* entry = ::readdir(directory.get())) {
        const std::string name(entry->d_name);
        if(name == "." || name == "..") continue;
        if(names.size() >= limit) {
            throw_build_failure(
                stage,
                EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
        }
        names.push_back(name);
        errno = 0;
    }
    if(errno != 0) throw_build_failure(stage, reason, errno);
    std::sort(names.begin(), names.end());
    return names;
}

OwnedDescriptor open_beneath(
    int parent_descriptor,
    const std::string& relative_path,
    int flags,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    struct open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC | O_NOFOLLOW);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    int descriptor;
    do {
        descriptor = static_cast<int>(::syscall(
            SYS_openat2, parent_descriptor, relative_path.c_str(),
            &how, sizeof(how)));
    } while(descriptor < 0 && errno == EINTR);
    if(descriptor < 0) {
        throw_build_failure(stage, reason, errno);
    }
    return OwnedDescriptor(descriptor);
}

std::string read_descriptor_bytes(
    int descriptor,
    std::uintmax_t expected_size,
    std::uintmax_t limit,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    if(expected_size > limit ||
       expected_size > static_cast<std::uintmax_t>(
                           std::numeric_limits<std::size_t>::max())) {
        throw_build_failure(
            stage,
            EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
    }
    std::string bytes(static_cast<std::size_t>(expected_size), '\0');
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        ssize_t read_size;
        do {
            read_size = ::pread(
                descriptor, bytes.data() + offset,
                bytes.size() - offset,
                static_cast<off_t>(offset));
        } while(read_size < 0 && errno == EINTR);
        if(read_size <= 0) {
            throw_build_failure(
                stage, reason,
                read_size < 0 ? std::optional<int>(errno)
                              : std::nullopt);
        }
        offset += static_cast<std::size_t>(read_size);
    }
    return bytes;
}

struct OpenedRegularFile {
    OwnedDescriptor descriptor;
    NodeIdentity identity;
    std::string bytes;
};

OpenedRegularFile open_and_read_regular_file(
    int parent_descriptor,
    const std::string& relative_path,
    std::uintmax_t root_device,
    std::uintmax_t limit,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    OwnedDescriptor descriptor = open_beneath(
        parent_descriptor, relative_path,
        O_RDONLY | O_NONBLOCK, stage, reason);
    const NodeIdentity before = descriptor_identity(
        descriptor.get(), stage, reason);
    if(before.type != S_IFREG || before.device != root_device ||
       before.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       before.links != 1) {
        throw_build_failure(stage, reason);
    }
    std::string bytes = read_descriptor_bytes(
        descriptor.get(), before.size, limit, stage, reason);
    const NodeIdentity after = descriptor_identity(
        descriptor.get(), stage, reason);
    if(!same_stable_file(before, after)) {
        throw_build_failure(stage, reason);
    }
    return OpenedRegularFile{
        std::move(descriptor), before, std::move(bytes)};
}

bool is_valid_environment_key(std::string_view key) noexcept {
    if(key.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(key.front());
    if(std::isalpha(first) == 0 && key.front() != '_') return false;
    return std::all_of(
        key.begin() + 1, key.end(), [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '_';
        });
}

std::vector<std::string> make_explicit_environment(
    const InvocationOwnedMakepkgEnvironment& environment,
    const InvocationOwnedSourceBuildContext& context) {
    std::vector<std::pair<std::string, std::string>> values;
    std::map<std::string, std::size_t> indices;
    const auto apply = [&values, &indices](
                           std::string key, std::string value) {
        const auto found = indices.find(key);
        if(found == indices.end()) {
            const std::size_t index = values.size();
            indices.emplace(key, index);
            values.emplace_back(std::move(key), std::move(value));
        } else {
            values[found->second].second = std::move(value);
        }
    };

    if(environ != nullptr) {
        for(char** current = environ; *current != nullptr; ++current) {
            const std::string_view assignment(*current);
            const std::size_t separator = assignment.find('=');
            if(separator == std::string_view::npos || separator == 0 ||
               !is_valid_environment_key(
                   assignment.substr(0, separator))) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::ContextValidation,
                    EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
            }
            apply(
                std::string(assignment.substr(0, separator)),
                std::string(assignment.substr(separator + 1)));
        }
    }

    const SourceEnvironmentEmptyValuePolicy empty_policy =
        environment.empty_value_policy();
    for(const SourceEnvironmentAssignment& assignment :
        environment.source_environment().ordered_assignments) {
        if(assignment.value.empty() &&
           empty_policy == SourceEnvironmentEmptyValuePolicy::Omit) {
            continue;
        }
        apply(assignment.key, assignment.value);
    }

    const auto require_owned_value = [&values, &indices](
                                         std::string_view key,
                                         const fs::path& expected) {
        const auto found = indices.find(std::string(key));
        if(found == indices.end() ||
           values[found->second].second != expected.string()) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
        }
    };
    require_owned_value("PKGDEST", context.pkgdest());
    require_owned_value("BUILDDIR", context.builddir());
    require_owned_value("SRCDEST", context.srcdest());

    std::vector<std::string> result;
    result.reserve(values.size());
    for(auto& [key, value] : values) {
        result.push_back(std::move(key) + "=" + std::move(value));
    }
    return result;
}

class FixedExecutable final {
public:
    explicit FixedExecutable(fs::path path)
        : path_(std::move(path)) {
        if(!path_.is_absolute() || path_.lexically_normal() != path_) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
        }
        int descriptor;
        do {
            descriptor = ::open(
                path_.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
        } while(descriptor < 0 && errno == EINTR);
        if(descriptor < 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext,
                errno);
        }
        descriptor_.reset(descriptor);
        identity_ = descriptor_identity(
            descriptor_.get(),
            EvaluatedDevelSourceBuildStage::ContextValidation,
            EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
        require_validity();
    }

    FixedExecutable(const FixedExecutable&) = delete;
    FixedExecutable& operator=(const FixedExecutable&) = delete;
    FixedExecutable(FixedExecutable&&) noexcept = default;
    FixedExecutable& operator=(FixedExecutable&&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_.get();
    }

    void require_validity() const {
        const NodeIdentity retained = descriptor_identity(
            descriptor_.get(),
            EvaluatedDevelSourceBuildStage::ContextValidation,
            EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
        struct stat named_status{};
        if(::lstat(path_.c_str(), &named_status) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext,
                errno);
        }
        const NodeIdentity named = node_identity(named_status);
        if(identity_.type != S_IFREG || !same_node(identity_, retained) ||
           !same_node(identity_, named) || identity_.owner != 0 ||
           (identity_.mode & 0111U) == 0 ||
           (identity_.mode & 0022U) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
        }
    }

private:
    fs::path path_;
    OwnedDescriptor descriptor_;
    NodeIdentity identity_;
};

std::string require_successful_process_output(
    std::string executable,
    int executable_descriptor,
    std::vector<std::string> arguments,
    std::vector<std::string> environment,
    int working_directory_descriptor,
    int standard_input_descriptor,
    const BoundedProcessPolicy& policy,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason,
    EvaluatedDevelSourceBuildProcess process) {
    ExplicitProcessInvocation invocation{
        std::move(executable), std::move(arguments),
        std::move(environment)};
    invocation.working_directory_fd = working_directory_descriptor;
    invocation.standard_input_fd = standard_input_descriptor;
    invocation.executable_fd = executable_descriptor;
    BoundedCapturedProcessResult result =
        capture_bounded_explicit_process_output_raw(invocation, policy);
    const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
    if(exited == nullptr || exited->exit_code != 0) {
        throw_process_failure(
            stage, reason, process, std::move(result.outcome));
    }
    return std::move(result.output);
}

std::string require_single_output_line(
    std::string output,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    if(output.empty() || output.back() != '\n') {
        throw_build_failure(stage, reason);
    }
    output.pop_back();
    if(output.empty() || output.find('\n') != std::string::npos ||
       output.find('\0') != std::string::npos) {
        throw_build_failure(stage, reason);
    }
    return output;
}

struct RecipeEntryIdentity {
    fs::path relative_path;
    bool is_directory = false;
    mode_t mode = 0;
    std::uintmax_t size = 0;
    std::string digest;
};

struct WorkingRecipe {
    fs::path path;
    OwnedDescriptor descriptor;
    std::vector<RecipeEntryIdentity> entries;
};

void write_all(
    int descriptor,
    std::string_view bytes,
    EvaluatedDevelSourceBuildStage stage) {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        ssize_t written;
        do {
            written = ::write(
                descriptor, bytes.data() + offset,
                bytes.size() - offset);
        } while(written < 0 && errno == EINTR);
        if(written <= 0) {
            throw_build_failure(
                stage,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                written < 0 ? std::optional<int>(errno)
                            : std::nullopt);
        }
        offset += static_cast<std::size_t>(written);
    }
}

void copy_recipe_tree(
    int source_descriptor,
    int target_descriptor,
    const fs::path& relative_directory,
    std::uintmax_t root_device,
    std::vector<RecipeEntryIdentity>& entries,
    std::size_t depth) {
    if(depth > MAX_RECIPE_DEPTH) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
    }
    const std::vector<std::string> names = directory_names(
        source_descriptor,
        EvaluatedDevelSourceBuildStage::WorkingRecipe,
        EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
        MAX_RECIPE_ENTRIES);
    for(const std::string& name : names) {
        if(entries.size() >= MAX_RECIPE_ENTRIES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
        }
        struct stat named_status{};
        if(::fstatat(
               source_descriptor, name.c_str(), &named_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                errno);
        }
        const NodeIdentity named = node_identity(named_status);
        const fs::path relative_path = relative_directory / name;
        if(named.device != root_device ||
           named.owner != static_cast<std::uintmax_t>(::geteuid())) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
        }
        if(named.type == S_IFDIR) {
            if(::mkdirat(
                   target_descriptor, name.c_str(), 0700) != 0) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::WorkingRecipe,
                    EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                    errno);
            }
            OwnedDescriptor source_child = open_beneath(
                source_descriptor, name,
                O_RDONLY | O_DIRECTORY,
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
            OwnedDescriptor target_child = open_beneath(
                target_descriptor, name,
                O_RDONLY | O_DIRECTORY,
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
            copy_recipe_tree(
                source_child.get(), target_child.get(), relative_path,
                root_device, entries, depth + 1);
            if(::fchmod(target_child.get(), WORKING_DIRECTORY_MODE) != 0) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::WorkingRecipe,
                    EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                    errno);
            }
            entries.push_back(RecipeEntryIdentity{
                relative_path, true, WORKING_DIRECTORY_MODE, 0, {}});
            continue;
        }
        if(named.type != S_IFREG || named.links != 1 ||
           named.size > MAX_RECIPE_FILE_BYTES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        }
        OwnedDescriptor source_file = open_beneath(
            source_descriptor, name,
            O_RDONLY | O_NONBLOCK,
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
        const NodeIdentity source_opened = descriptor_identity(
            source_file.get(),
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        if(!same_node(named, source_opened) || source_opened.links != 1) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        }
        const std::string bytes = read_descriptor_bytes(
            source_file.get(), source_opened.size,
            MAX_RECIPE_FILE_BYTES,
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        const mode_t target_mode =
            relative_path == fs::path("PKGBUILD")
                ? MUTABLE_PKGBUILD_MODE
            : (named.mode & 0111U) != 0
                ? WORKING_EXECUTABLE_MODE
                : WORKING_FILE_MODE;
        int target_raw;
        do {
            target_raw = ::openat(
                target_descriptor, name.c_str(),
                O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                target_mode);
        } while(target_raw < 0 && errno == EINTR);
        if(target_raw < 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                errno);
        }
        OwnedDescriptor target_file(target_raw);
        write_all(
            target_file.get(), bytes,
            EvaluatedDevelSourceBuildStage::WorkingRecipe);
        if(::fchmod(target_file.get(), target_mode) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                errno);
        }
        const NodeIdentity source_after = descriptor_identity(
            source_file.get(),
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        if(!same_stable_file(source_opened, source_after)) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        }
        entries.push_back(RecipeEntryIdentity{
            relative_path, false, target_mode,
            static_cast<std::uintmax_t>(bytes.size()),
            xdg_generation_store_raw_contents_sha256(bytes)});
    }
}

WorkingRecipe create_working_recipe(
    int reviewed_recipe_descriptor,
    int builddir_descriptor,
    const fs::path& builddir_path,
    std::uintmax_t root_device) {
    if(!directory_names(
            builddir_descriptor,
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure)
            .empty()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    }
    if(::mkdirat(
           builddir_descriptor, std::string(WORKING_RECIPE_LEAF).c_str(),
           0700) != 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
            errno);
    }
    OwnedDescriptor working_descriptor = open_beneath(
        builddir_descriptor, std::string(WORKING_RECIPE_LEAF),
        O_RDONLY | O_DIRECTORY,
        EvaluatedDevelSourceBuildStage::WorkingRecipe,
        EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    std::vector<RecipeEntryIdentity> entries;
    entries.reserve(16);
    copy_recipe_tree(
        reviewed_recipe_descriptor, working_descriptor.get(), {},
        root_device, entries, 0);
    const bool has_pkgbuild = std::any_of(
        entries.begin(), entries.end(), [](const RecipeEntryIdentity& entry) {
            return !entry.is_directory &&
                   entry.relative_path == fs::path("PKGBUILD");
        });
    const bool has_srcinfo = std::any_of(
        entries.begin(), entries.end(), [](const RecipeEntryIdentity& entry) {
            return !entry.is_directory &&
                   entry.relative_path == fs::path(".SRCINFO");
        });
    if(!has_pkgbuild || !has_srcinfo ||
       ::fchmod(working_descriptor.get(), WORKING_DIRECTORY_MODE) != 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
            has_pkgbuild && has_srcinfo ? std::optional<int>(errno)
                                        : std::nullopt);
    }
    std::sort(
        entries.begin(), entries.end(),
        [](const RecipeEntryIdentity& lhs,
           const RecipeEntryIdentity& rhs) {
            return lhs.relative_path.native() < rhs.relative_path.native();
        });
    return WorkingRecipe{
        builddir_path / std::string(WORKING_RECIPE_LEAF),
        std::move(working_descriptor), std::move(entries)};
}

std::vector<RecipeEntryIdentity> observe_recipe_tree(
    int directory_descriptor,
    const fs::path& relative_directory,
    std::uintmax_t root_device,
    std::size_t depth,
    std::size_t& observed_count) {
    if(depth > MAX_RECIPE_DEPTH) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
    }
    std::vector<RecipeEntryIdentity> observed;
    const std::vector<std::string> names = directory_names(
        directory_descriptor,
        EvaluatedDevelSourceBuildStage::WorkingRecipe,
        EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
        MAX_RECIPE_ENTRIES);
    for(const std::string& name : names) {
        if(++observed_count > MAX_RECIPE_ENTRIES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
        }
        const fs::path relative_path = relative_directory / name;
        struct stat status{};
        if(::fstatat(
               directory_descriptor, name.c_str(), &status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                errno);
        }
        const NodeIdentity named = node_identity(status);
        if(named.device != root_device ||
           named.owner != static_cast<std::uintmax_t>(::geteuid())) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
        }
        if(named.type == S_IFDIR) {
            OwnedDescriptor child = open_beneath(
                directory_descriptor, name,
                O_RDONLY | O_DIRECTORY,
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
            std::vector<RecipeEntryIdentity> descendants =
                observe_recipe_tree(
                    child.get(), relative_path, root_device,
                    depth + 1, observed_count);
            observed.insert(
                observed.end(),
                std::make_move_iterator(descendants.begin()),
                std::make_move_iterator(descendants.end()));
            observed.push_back(RecipeEntryIdentity{
                relative_path, true, static_cast<mode_t>(named.mode), 0, {}});
            continue;
        }
        if(named.type != S_IFREG || named.links != 1 ||
           named.size > MAX_RECIPE_FILE_BYTES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        }
        OpenedRegularFile file = open_and_read_regular_file(
            directory_descriptor, name, root_device,
            MAX_RECIPE_FILE_BYTES,
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        observed.push_back(RecipeEntryIdentity{
            relative_path, false,
            static_cast<mode_t>(file.identity.mode), file.identity.size,
            xdg_generation_store_raw_contents_sha256(file.bytes)});
    }
    return observed;
}

std::vector<RecipeEntryIdentity> observe_recipe_tree(
    const WorkingRecipe& recipe,
    std::uintmax_t root_device) {
    const NodeIdentity root = descriptor_identity(
        recipe.descriptor.get(),
        EvaluatedDevelSourceBuildStage::WorkingRecipe,
        EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    if(root.type != S_IFDIR || root.device != root_device ||
       root.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       root.mode != WORKING_DIRECTORY_MODE) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    }
    std::size_t observed_count = 0;
    std::vector<RecipeEntryIdentity> result = observe_recipe_tree(
        recipe.descriptor.get(), {}, root_device, 0, observed_count);
    std::sort(
        result.begin(), result.end(),
        [](const RecipeEntryIdentity& lhs,
           const RecipeEntryIdentity& rhs) {
            return lhs.relative_path.native() < rhs.relative_path.native();
        });
    return result;
}

bool same_recipe_entry(
    const RecipeEntryIdentity& expected,
    const RecipeEntryIdentity& actual) {
    return expected.relative_path == actual.relative_path &&
           expected.is_directory == actual.is_directory &&
           expected.mode == actual.mode && expected.size == actual.size &&
           expected.digest == actual.digest;
}

void require_exact_working_recipe(
    const WorkingRecipe& recipe,
    std::uintmax_t root_device) {
    const std::vector<RecipeEntryIdentity> observed =
        observe_recipe_tree(recipe, root_device);
    if(observed.size() != recipe.entries.size() ||
       !std::equal(
           observed.begin(), observed.end(), recipe.entries.begin(),
           same_recipe_entry)) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    }
}

void seal_dynamic_pkgbuild(
    WorkingRecipe& recipe,
    std::uintmax_t root_device,
    bool allow_content_change) {
    std::vector<RecipeEntryIdentity> observed =
        observe_recipe_tree(recipe, root_device);
    if(observed.size() != recipe.entries.size()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    }
    bool updated_pkgbuild = false;
    for(std::size_t index = 0; index < observed.size(); ++index) {
        RecipeEntryIdentity& actual = observed[index];
        const RecipeEntryIdentity& expected = recipe.entries[index];
        if(actual.relative_path != expected.relative_path ||
           actual.is_directory != expected.is_directory) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        }
        if(actual.relative_path != fs::path("PKGBUILD")) {
            if(!same_recipe_entry(expected, actual)) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::WorkingRecipe,
                    EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
            }
            continue;
        }
        if(actual.is_directory || actual.mode != MUTABLE_PKGBUILD_MODE) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        }
        if(!allow_content_change &&
           (actual.size != expected.size ||
            actual.digest != expected.digest)) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::DynamicVersion,
                EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
        }
        OwnedDescriptor pkgbuild = open_beneath(
            recipe.descriptor.get(), "PKGBUILD",
            O_RDONLY | O_NONBLOCK,
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
        if(::fchmod(pkgbuild.get(), WORKING_FILE_MODE) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::WorkingRecipe,
                EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure,
                errno);
        }
        actual.mode = WORKING_FILE_MODE;
        updated_pkgbuild = true;
    }
    if(!updated_pkgbuild) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::WorkingRecipe,
            EvaluatedDevelSourceBuildFailureReason::WorkingRecipeFailure);
    }
    recipe.entries = std::move(observed);
    require_exact_working_recipe(recipe, root_device);
}

void make_pkgbuild_mutable_for_build(
    WorkingRecipe& recipe,
    std::uintmax_t root_device) {
    require_exact_working_recipe(recipe, root_device);
    auto pkgbuild = std::find_if(
        recipe.entries.begin(), recipe.entries.end(),
        [](const RecipeEntryIdentity& entry) {
            return !entry.is_directory &&
                   entry.relative_path == fs::path("PKGBUILD");
        });
    if(pkgbuild == recipe.entries.end() ||
       pkgbuild->mode != WORKING_FILE_MODE) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    }
    OwnedDescriptor descriptor = open_beneath(
        recipe.descriptor.get(), "PKGBUILD",
        O_RDONLY | O_NONBLOCK,
        EvaluatedDevelSourceBuildStage::DynamicVersion,
        EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    if(::fchmod(descriptor.get(), MUTABLE_PKGBUILD_MODE) != 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable,
            errno);
    }
    pkgbuild->mode = MUTABLE_PKGBUILD_MODE;
    require_exact_working_recipe(recipe, root_device);
}

struct SourceProjectionAnalysis {
    VcsSourceIdentity git_source;
    std::size_t source_count = 0;
    std::size_t tracked_local_source_count = 0;
    std::string package_name;
};

[[noreturn]] void throw_source_parse_failure(
    const SrcinfoSourceMetadataParseFailure& cause) {
    EvaluatedDevelSourceBuildFailure failure = build_failure(
        EvaluatedDevelSourceBuildStage::EvaluatedSource,
        EvaluatedDevelSourceBuildFailureReason::EvaluatedSourceFailure);
    failure.source_parse_failure = cause;
    throw BuildFailureError(std::move(failure));
}

[[noreturn]] void throw_package_parse_failure(
    EvaluatedDevelSourceBuildStage stage,
    const LocalPackageMetadataParseFailure& cause) {
    EvaluatedDevelSourceBuildFailure failure = build_failure(
        stage,
        stage == EvaluatedDevelSourceBuildStage::DynamicVersion
            ? EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable
            : EvaluatedDevelSourceBuildFailureReason::EvaluatedSourceFailure);
    failure.package_parse_failure = cause;
    throw BuildFailureError(std::move(failure));
}

bool safe_relative_source_path(const fs::path& path) {
    if(path.empty() || path.is_absolute() ||
       path.lexically_normal() != path) {
        return false;
    }
    return std::none_of(
        path.begin(), path.end(), [](const fs::path& component) {
            return component.empty() || component == "." ||
                   component == ".." || component == ".git";
        });
}

struct DeclaredArchitectureContract {
    std::vector<std::string> base_architectures;
    std::vector<std::string> package_architectures;

    bool operator==(const DeclaredArchitectureContract&) const = default;
};

DeclaredArchitectureContract declared_architecture_contract(
    const LocalPackageMetadata& metadata) {
    // The strict metadata parser owns token, duplicate and any/native
    // validation. Callers have also established exactly one child. Compare
    // sets, including the base declaration even when the child overrides it.
    const auto& child = metadata.children.front();
    DeclaredArchitectureContract contract{
        metadata.architectures,
        child.has_architecture_override ? child.architectures : metadata.architectures};
    if(contract.package_architectures.empty()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }
    std::sort(contract.base_architectures.begin(), contract.base_architectures.end());
    std::sort(contract.package_architectures.begin(), contract.package_architectures.end());
    return contract;
}

SourceProjectionAnalysis analyze_source_projection(
    std::string_view reviewed_srcinfo,
    std::string_view evaluated_srcinfo,
    int reviewed_recipe_descriptor,
    std::uintmax_t root_device,
    const PackageBaseIdentity& package_base) {
    SrcinfoSourceMetadataParseResult reviewed_source_result =
        parse_srcinfo_source_metadata(reviewed_srcinfo);
    if(!reviewed_source_result.is_success()) {
        throw_source_parse_failure(*reviewed_source_result.failure());
    }
    SrcinfoSourceMetadataParseResult evaluated_source_result =
        parse_srcinfo_source_metadata(evaluated_srcinfo);
    if(!evaluated_source_result.is_success()) {
        throw_source_parse_failure(*evaluated_source_result.failure());
    }
    const ParsedSrcinfoSourceMetadata& reviewed_source =
        *reviewed_source_result.metadata();
    const ParsedSrcinfoSourceMetadata& evaluated_source =
        *evaluated_source_result.metadata();
    if(reviewed_source != evaluated_source) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::RawEvaluatedSourceMismatch);
    }
    if(evaluated_source.package_base != package_base.package_base() ||
       evaluated_source.source_entries.empty() ||
       evaluated_source.source_entries.size() > MAX_SOURCE_ENTRIES) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }

    LocalPackageMetadataParseResult reviewed_package_result =
        parse_local_package_metadata(reviewed_srcinfo);
    if(!reviewed_package_result.is_success()) {
        throw_package_parse_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            *reviewed_package_result.failure());
    }
    LocalPackageMetadataParseResult evaluated_package_result =
        parse_local_package_metadata(evaluated_srcinfo);
    if(!evaluated_package_result.is_success()) {
        throw_package_parse_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            *evaluated_package_result.failure());
    }
    const LocalPackageMetadata& reviewed_package =
        *reviewed_package_result.metadata();
    const LocalPackageMetadata& evaluated_package =
        *evaluated_package_result.metadata();
    if(reviewed_package.package_base != package_base.package_base() ||
       evaluated_package.package_base != package_base.package_base() ||
       reviewed_package.children.size() != 1 ||
       evaluated_package.children.size() != 1 ||
       reviewed_package.children.front().name !=
           evaluated_package.children.front().name) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }

    if(declared_architecture_contract(reviewed_package) !=
       declared_architecture_contract(evaluated_package)) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::RawEvaluatedSourceMismatch);
    }

    std::optional<VcsSourceIdentity> git_source;
    std::size_t local_source_count = 0;
    for(const ParsedSrcinfoSourceEntry& source_entry :
        evaluated_source.source_entries) {
        if(source_entry.architecture_qualifier.has_value()) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::EvaluatedSource,
                EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
        }
        const ParsedSourceEntry& parsed = source_entry.parsed_source;
        if(parsed.kind == ParsedSourceEntryKind::Local) {
            const fs::path source_path(parsed.source_payload);
            if(!safe_relative_source_path(source_path)) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::EvaluatedSource,
                    EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
            }
            OpenedRegularFile local_source = open_and_read_regular_file(
                reviewed_recipe_descriptor, source_path.string(),
                root_device, MAX_RECIPE_FILE_BYTES,
                EvaluatedDevelSourceBuildStage::EvaluatedSource,
                EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
            static_cast<void>(local_source);
            ++local_source_count;
            continue;
        }
        if(parsed.kind != ParsedSourceEntryKind::Vcs ||
           !parsed.vcs.has_value() ||
           parsed.vcs->recognized_kind != ParsedSourceVcsKind::Git ||
           parsed.vcs->declaration_kind !=
               ParsedSourceVcsDeclarationKind::ExplicitPrefix ||
           parsed.transport_scheme != std::optional<std::string>("https") ||
           parsed.vcs->query.has_value() || git_source.has_value()) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::EvaluatedSource,
                EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
        }

        VcsSelector selector = VcsSelector::default_head();
        if(parsed.vcs->selector.has_value()) {
            const ParsedSourceSelector& parsed_selector =
                *parsed.vcs->selector;
            if(parsed.vcs->component_order !=
                   ParsedSourceVcsComponentOrder::FragmentOnly ||
               parsed_selector.recognized_role !=
                   ParsedSourceSelectorRole::Branch ||
               parsed_selector.key != "branch") {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::EvaluatedSource,
                    EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
            }
            ExactGitBranchValidationResult branch =
                validate_exact_git_branch(parsed_selector.value);
            if(const auto* validated =
                   std::get_if<ValidatedExactGitBranch>(&branch)) {
                selector = VcsSelector::branch(validated->name());
            } else if(std::holds_alternative<InvalidExactGitBranch>(branch)) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::EvaluatedSource,
                    EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
            } else {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::EvaluatedSource,
                    EvaluatedDevelSourceBuildFailureReason::EvaluatedSourceFailure);
            }
        } else if(parsed.vcs->component_order !=
                  ParsedSourceVcsComponentOrder::None) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::EvaluatedSource,
                EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
        }

        try {
            ValidatedHttpsGitRemote remote =
                ValidatedHttpsGitRemote::make(parsed.source_location);
            git_source = VcsSourceIdentity::make(
                VcsKind::Git, remote.canonical_url(),
                std::move(selector));
        } catch(const std::invalid_argument&) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::EvaluatedSource,
                EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
        }
    }
    if(!git_source.has_value()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }
    return SourceProjectionAnalysis{
        std::move(*git_source),
        evaluated_source.source_entries.size(), local_source_count,
        evaluated_package.children.front().name};
}

struct PreparedPackageExpectation {
    std::string package_name;
    std::string full_version;
    DeclaredArchitectureContract declared_architectures;
};

PreparedPackageExpectation prepared_package_expectation(
    std::string_view prepared_srcinfo,
    std::string_view reviewed_srcinfo,
    const SourceProjectionAnalysis& initial,
    int reviewed_recipe_descriptor,
    std::uintmax_t root_device,
    const PackageBaseIdentity& package_base) {
    SourceProjectionAnalysis prepared = analyze_source_projection(
        reviewed_srcinfo, prepared_srcinfo,
        reviewed_recipe_descriptor, root_device, package_base);
    if(prepared.git_source != initial.git_source ||
       prepared.source_count != initial.source_count ||
       prepared.tracked_local_source_count !=
           initial.tracked_local_source_count ||
       prepared.package_name != initial.package_name) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::RawEvaluatedSourceMismatch);
    }
    LocalPackageMetadataParseResult metadata_result =
        parse_local_package_metadata(prepared_srcinfo);
    if(!metadata_result.is_success()) {
        throw_package_parse_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            *metadata_result.failure());
    }
    const LocalPackageMetadata& metadata = *metadata_result.metadata();
    if(metadata.children.size() != 1 ||
       metadata.children.front().name != initial.package_name) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    }
    const LocalPackageMetadataChild& child = metadata.children.front();
    PackageVersionIdentity version = PackageVersionIdentity::pkgver_pkgrel(
        metadata.epoch, metadata.pkgver, metadata.pkgrel);
    if(version.full_version() == nullptr) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    }
    return PreparedPackageExpectation{
        child.name, *version.full_version(), declared_architecture_contract(metadata)};
}

struct SelectedPackageOutput {
    fs::path path;
    std::string architecture;
};

SelectedPackageOutput parse_expected_package_output(
    std::string output,
    const InvocationOwnedSourceBuildContext& context,
    const PreparedPackageExpectation& package) {
    const std::string line = require_single_output_line(
        std::move(output),
        EvaluatedDevelSourceBuildStage::DynamicVersion,
        EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    const fs::path path(line);
    if(!path.is_absolute() || path.lexically_normal() != path ||
       path.parent_path() != context.pkgdest() ||
       path.filename().empty()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    }
    // makepkg get_full_version includes a nonzero epoch. Its output grammar
    // uses the known child/version prefix and a PKGEXT beginning with .pkg.tar;
    // neither hyphens in identity nor the compression suffix select an arch.
    const std::string filename = path.filename().string();
    const std::string prefix = package.package_name + "-" + package.full_version + "-";
    const std::size_t extension = filename.find(".pkg.tar", prefix.size());
    if(!filename.starts_with(prefix) || extension == std::string::npos) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable);
    }
    const std::string architecture = filename.substr(prefix.size(), extension - prefix.size());
    const auto& declared = package.declared_architectures.package_architectures;
    // Membership also enforces the parser's architecture token grammar. The
    // valid singleton {any} can only accept any, never the native CARCH.
    if(std::find(declared.begin(), declared.end(), architecture) == declared.end()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }
    return SelectedPackageOutput{path, architecture};
}

struct RetainedDirectory {
    fs::path path;
    fs::path relative_path;
    OwnedDescriptor descriptor;
    NodeIdentity identity;
};

void require_safe_directory(
    const NodeIdentity& identity,
    std::uintmax_t root_device,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    if(identity.type != S_IFDIR || identity.device != root_device ||
       identity.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       (identity.mode & 0022U) != 0) {
        throw_build_failure(stage, reason);
    }
}

void locate_git_workspaces_recursive(
    int directory_descriptor,
    const fs::path& directory_path,
    const fs::path& relative_path,
    std::uintmax_t root_device,
    std::size_t depth,
    std::size_t& entry_count,
    std::vector<RetainedDirectory>& candidates) {
    if(depth > MAX_FILESYSTEM_DEPTH) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::SourceWorkspace,
            EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
    }
    const NodeIdentity current = descriptor_identity(
        directory_descriptor,
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    require_safe_directory(
        current, root_device,
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    const std::vector<std::string> names = directory_names(
        directory_descriptor,
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure,
        MAX_FILESYSTEM_ENTRIES);
    bool has_git_directory = false;
    for(const std::string& name : names) {
        if(++entry_count > MAX_FILESYSTEM_ENTRIES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::SourceWorkspace,
                EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
        }
        struct stat status{};
        if(::fstatat(
               directory_descriptor, name.c_str(), &status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::SourceWorkspace,
                EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure,
                errno);
        }
        const NodeIdentity child_identity = node_identity(status);
        if(name == ".git") {
            if(child_identity.type != S_IFDIR ||
               child_identity.device != root_device ||
               child_identity.owner !=
                   static_cast<std::uintmax_t>(::geteuid())) {
                throw_build_failure(
                    EvaluatedDevelSourceBuildStage::SourceWorkspace,
                    EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
            }
            has_git_directory = true;
            continue;
        }
        if(child_identity.type != S_IFDIR) continue;
        OwnedDescriptor child = open_beneath(
            directory_descriptor, name,
            O_RDONLY | O_DIRECTORY,
            EvaluatedDevelSourceBuildStage::SourceWorkspace,
            EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
        locate_git_workspaces_recursive(
            child.get(), directory_path / name,
            relative_path / name, root_device, depth + 1,
            entry_count, candidates);
    }
    if(has_git_directory) {
        int retained_raw;
        do {
            retained_raw = ::openat(
                directory_descriptor, ".",
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        } while(retained_raw < 0 && errno == EINTR);
        if(retained_raw < 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::SourceWorkspace,
                EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure,
                errno);
        }
        candidates.push_back(RetainedDirectory{
            directory_path, relative_path,
            OwnedDescriptor(retained_raw), current});
    }
}

RetainedDirectory locate_git_workspace(
    int builddir_descriptor,
    const fs::path& builddir_path,
    std::uintmax_t root_device) {
    std::size_t entry_count = 0;
    std::vector<RetainedDirectory> candidates;
    locate_git_workspaces_recursive(
        builddir_descriptor, builddir_path, {}, root_device, 0,
        entry_count, candidates);
    if(candidates.empty()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::SourceWorkspace,
            EvaluatedDevelSourceBuildFailureReason::SourceWorkspaceUnavailable);
    }
    if(candidates.size() != 1) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::SourceWorkspace,
            EvaluatedDevelSourceBuildFailureReason::SourceWorkspaceAmbiguous);
    }
    return std::move(candidates.front());
}

RetainedDirectory locate_git_mirror(
    int srcdest_descriptor,
    const fs::path& srcdest_path,
    std::uintmax_t root_device) {
    const std::vector<std::string> names = directory_names(
        srcdest_descriptor,
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure,
        MAX_SOURCE_ENTRIES);
    if(names.size() != 1) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::SourceWorkspace,
            names.empty()
                ? EvaluatedDevelSourceBuildFailureReason::SourceWorkspaceUnavailable
                : EvaluatedDevelSourceBuildFailureReason::SourceWorkspaceAmbiguous);
    }
    OwnedDescriptor descriptor = open_beneath(
        srcdest_descriptor, names.front(),
        O_RDONLY | O_DIRECTORY,
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    const NodeIdentity identity = descriptor_identity(
        descriptor.get(),
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    require_safe_directory(
        identity, root_device,
        EvaluatedDevelSourceBuildStage::SourceWorkspace,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    return RetainedDirectory{
        srcdest_path / names.front(), names.front(),
        std::move(descriptor), identity};
}

void require_named_directory_unchanged(
    int root_descriptor,
    const RetainedDirectory& directory,
    EvaluatedDevelSourceBuildStage stage) {
    OwnedDescriptor current = open_beneath(
        root_descriptor, directory.relative_path.string(),
        O_RDONLY | O_DIRECTORY, stage,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    const NodeIdentity retained = descriptor_identity(
        directory.descriptor.get(), stage,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    const NodeIdentity named = descriptor_identity(
        current.get(), stage,
        EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    if(!same_node(directory.identity, retained) ||
       !same_node(directory.identity, named)) {
        throw_build_failure(
            stage,
            EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure);
    }
}

std::string git_output(
    const FixedExecutable& git,
    const RetainedDirectory& repository,
    const OwnedDescriptor& null_input,
    std::vector<std::string> arguments,
    EvaluatedDevelSourceBuildProcess process) {
    git.require_validity();
    std::vector<std::string> complete =
        trusted_git_managed_process_arguments();
    // Actual commit evidence must use raw objects even if metadata drifts
    // between the explicit replacement/graft checks at a proof point.
    complete.push_back("--no-replace-objects");
    complete.insert(
        complete.end(),
        std::make_move_iterator(arguments.begin()),
        std::make_move_iterator(arguments.end()));
    std::vector<std::string> environment = trusted_git_process_environment(
        TrustedGitProcessEnvironmentMode::ReadOnlyObservation);
    const std::string output = require_successful_process_output(
        git.path().string(), git.descriptor(), std::move(complete),
        std::move(environment), repository.descriptor.get(),
        null_input.get(),
        BoundedProcessPolicy{
            METADATA_PROCESS_TIMEOUT, PROCESS_TERMINATION_GRACE,
            MAX_GIT_OUTPUT_BYTES, true},
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid,
        process);
    git.require_validity();
    return output;
}

std::string git_output_line(
    const FixedExecutable& git,
    const RetainedDirectory& repository,
    const OwnedDescriptor& null_input,
    std::vector<std::string> arguments,
    EvaluatedDevelSourceBuildProcess process) {
    return require_single_output_line(
        git_output(git, repository, null_input, std::move(arguments), process),
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRevisionUnavailable);
}

void require_entry_absent(
    int directory_descriptor,
    const char* relative_path) {
    struct open_how how{};
    how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    int descriptor;
    do {
        descriptor = static_cast<int>(::syscall(
            SYS_openat2, directory_descriptor, relative_path,
            &how, sizeof(how)));
    } while(descriptor < 0 && errno == EINTR);
    if(descriptor >= 0) {
        static_cast<void>(::close(descriptor));
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    }
    if(errno != ENOENT) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid,
            errno);
    }
}

void require_contained_git_metadata_tree(
    int directory_descriptor,
    std::uintmax_t root_device,
    std::size_t depth,
    std::size_t& entry_count) {
    if(depth > MAX_FILESYSTEM_DEPTH) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
    }
    const NodeIdentity directory = descriptor_identity(
        directory_descriptor,
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    require_safe_directory(
        directory, root_device,
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    const std::vector<std::string> names = directory_names(
        directory_descriptor,
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid,
        MAX_GIT_METADATA_ENTRIES);
    for(const std::string& name : names) {
        if(++entry_count > MAX_GIT_METADATA_ENTRIES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
        }
        struct stat status{};
        if(::fstatat(
               directory_descriptor, name.c_str(), &status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid,
                errno);
        }
        const NodeIdentity entry = node_identity(status);
        if(entry.device != root_device ||
           entry.owner != static_cast<std::uintmax_t>(::geteuid()) ||
           (entry.mode & 0022U) != 0) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        }
        if(entry.type == S_IFDIR) {
            OwnedDescriptor child = open_beneath(
                directory_descriptor, name,
                O_RDONLY | O_DIRECTORY,
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
            require_contained_git_metadata_tree(
                child.get(), root_device, depth + 1, entry_count);
            continue;
        }
        if(entry.type != S_IFREG || entry.links != 1) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        }
        OwnedDescriptor file = open_beneath(
            directory_descriptor, name,
            O_RDONLY | O_NONBLOCK,
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        const NodeIdentity opened = descriptor_identity(
            file.get(),
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        if(!same_stable_file(entry, opened)) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        }
    }
}

struct GitProofPoint {
    std::string object_id;
    GitObjectFormat object_format = GitObjectFormat::Sha1;
};

void require_unmodified_git_object_semantics(
    const FixedExecutable& git, const OwnedDescriptor& null_input,
    const RetainedDirectory& repository, int git_directory_descriptor,
    EvaluatedDevelSourceBuildProcess process) {
    require_entry_absent(git_directory_descriptor, "refs/replace");
    require_entry_absent(git_directory_descriptor, "info/grafts");
    // Keep the physical packed-metadata check too: Git may omit a broken ref
    // from its logical inventory. Such metadata is still unsupported here.
    struct stat status{};
    if(::fstatat(git_directory_descriptor, "packed-refs", &status,
                 AT_SYMLINK_NOFOLLOW) == 0) {
        OpenedRegularFile packed = open_and_read_regular_file(
            git_directory_descriptor, "packed-refs", repository.identity.device,
            MAX_SRCINFO_BYTES, EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        if(packed.bytes.find(" refs/replace/") != std::string::npos ||
           packed.bytes.find(" refs/replace\n") != std::string::npos) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        }
    } else if(errno != ENOENT) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid, errno);
    }
    // Ref storage is Git-owned (files, packed refs, or reftable). Inspect the
    // logical namespace with replacements disabled before any commit/ref proof,
    // and repeat at the end of both proof points. On-disk absence alone cannot
    // prove the absence of replacement authority in a different ref backend.
    if(!git_output(git, repository, null_input,
                   {"-c", "safe.bareRepository=all", "for-each-ref", "--format=%(refname)", "refs/replace"},
                   process)
            .empty()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    }
}

GitProofPoint observe_git_proof_point(
    const FixedExecutable& git,
    const OwnedDescriptor& null_input,
    const RetainedDirectory& mirror,
    const RetainedDirectory& workspace,
    const VcsSourceIdentity& source) {
    std::size_t mirror_entry_count = 0;
    require_contained_git_metadata_tree(
        mirror.descriptor.get(), mirror.identity.device, 0,
        mirror_entry_count);
    OwnedDescriptor workspace_git = open_beneath(
        workspace.descriptor.get(), ".git",
        O_RDONLY | O_DIRECTORY,
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    std::size_t workspace_git_entry_count = 0;
    require_contained_git_metadata_tree(
        workspace_git.get(), workspace.identity.device, 0,
        workspace_git_entry_count);
    require_unmodified_git_object_semantics(
        git, null_input, mirror, mirror.descriptor.get(),
        EvaluatedDevelSourceBuildProcess::GitMirrorObservation);
    require_unmodified_git_object_semantics(
        git, null_input, workspace, workspace_git.get(),
        EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
    if(git_output_line(
           git, mirror, null_input,
           {"-c", "safe.bareRepository=all", "rev-parse",
            "--is-bare-repository"},
           EvaluatedDevelSourceBuildProcess::GitMirrorObservation) !=
           "true" ||
       git_output_line(
           git, workspace, null_input,
           {"rev-parse", "--is-inside-work-tree"},
           EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation) !=
           "true" ||
       git_output_line(
           git, workspace, null_input,
           {"rev-parse", "--is-bare-repository"},
           EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation) !=
           "false") {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    }

    const std::string mirror_remote = git_output_line(
        git, mirror, null_input,
        {"-c", "safe.bareRepository=all", "config", "--get",
         "remote.origin.url"},
        EvaluatedDevelSourceBuildProcess::GitMirrorObservation);
    ValidatedHttpsGitRemote observed_remote =
        ValidatedHttpsGitRemote::make(mirror_remote);
    if(observed_remote.canonical_url() != source.source_location()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRevisionMismatch);
    }

    const std::string workspace_remote = git_output_line(
        git, workspace, null_input,
        {"remote", "get-url", "origin"},
        EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
    const fs::path workspace_remote_path(workspace_remote);
    if(!workspace_remote_path.is_absolute() ||
       workspace_remote_path.lexically_normal() !=
           mirror.path.lexically_normal()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRevisionMismatch);
    }

    const std::string workspace_root = git_output_line(
        git, workspace, null_input,
        {"rev-parse", "--show-toplevel"},
        EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
    if(fs::path(workspace_root).lexically_normal() !=
       workspace.path.lexically_normal()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    }

    std::string mirror_selector = "HEAD";
    std::string workspace_selector =
        "refs/remotes/origin/HEAD";
    if(source.selector().kind() == VcsSelectorKind::DefaultHead) {
        const std::string mirror_head = git_output_line(
            git, mirror, null_input,
            {"-c", "safe.bareRepository=all", "symbolic-ref", "HEAD"},
            EvaluatedDevelSourceBuildProcess::GitMirrorObservation);
        const std::string workspace_head = git_output_line(
            git, workspace, null_input,
            {"symbolic-ref", "refs/remotes/origin/HEAD"},
            EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
        constexpr std::string_view MIRROR_PREFIX = "refs/heads/";
        constexpr std::string_view WORKSPACE_PREFIX =
            "refs/remotes/origin/";
        if(!std::string_view(mirror_head).starts_with(MIRROR_PREFIX) ||
           std::string_view(mirror_head).size() == MIRROR_PREFIX.size() ||
           workspace_head !=
               std::string(WORKSPACE_PREFIX) +
                   mirror_head.substr(MIRROR_PREFIX.size())) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRevisionMismatch);
        }
    } else if(source.selector().kind() == VcsSelectorKind::Branch) {
        const std::string* branch = source.selector().value();
        if(branch == nullptr) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        }
        mirror_selector = "refs/heads/" + *branch;
        workspace_selector =
            "refs/remotes/origin/" + *branch;
    } else {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    }
    const std::string mirror_oid = git_output_line(
        git, mirror, null_input,
        {"-c", "safe.bareRepository=all", "rev-parse", "--verify",
         mirror_selector},
        EvaluatedDevelSourceBuildProcess::GitMirrorObservation);
    const std::string selected_workspace_oid = git_output_line(
        git, workspace, null_input,
        {"rev-parse", "--verify", workspace_selector},
        EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
    const std::string workspace_oid = git_output_line(
        git, workspace, null_input,
        {"rev-parse", "--verify", "HEAD"},
        EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
    SourceRevisionIdentity mirror_revision =
        SourceRevisionIdentity::git_commit(mirror_oid);
    SourceRevisionIdentity selected_revision =
        SourceRevisionIdentity::git_commit(selected_workspace_oid);
    SourceRevisionIdentity workspace_revision =
        SourceRevisionIdentity::git_commit(workspace_oid);
    if(mirror_revision != selected_revision ||
       mirror_revision != workspace_revision ||
       workspace_revision.git_object_format() == nullptr) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRevisionMismatch);
    }
    const std::string expected_format =
        *workspace_revision.git_object_format() == GitObjectFormat::Sha1
            ? "sha1"
            : "sha256";
    for(const RetainedDirectory* repository : {&mirror, &workspace}) {
        const auto process = repository == &mirror
                                 ? EvaluatedDevelSourceBuildProcess::GitMirrorObservation
                                 : EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation;
        if(git_output_line(git, *repository, null_input,
                           {"-c", "safe.bareRepository=all", "rev-parse", "--show-object-format=storage"}, process) != expected_format ||
           git_output_line(git, *repository, null_input,
                           {"-c", "safe.bareRepository=all", "cat-file", "-t", workspace_oid}, process) != "commit") {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
        }
    }

    require_entry_absent(workspace.descriptor.get(), ".git/commondir");
    require_entry_absent(workspace.descriptor.get(), ".git/gitdir");
    require_entry_absent(workspace.descriptor.get(), ".git/modules");
    require_entry_absent(workspace.descriptor.get(), ".gitmodules");
    require_entry_absent(mirror.descriptor.get(), "objects/info/alternates");
    OpenedRegularFile alternates = open_and_read_regular_file(
        workspace.descriptor.get(), ".git/objects/info/alternates",
        workspace.identity.device, MAX_SRCINFO_BYTES,
        EvaluatedDevelSourceBuildStage::GitRevision,
        EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    const std::string expected_alternate =
        (mirror.path / "objects").string() + "\n";
    if(alternates.bytes != expected_alternate) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::GitRevision,
            EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid);
    }
    require_unmodified_git_object_semantics(
        git, null_input, mirror, mirror.descriptor.get(),
        EvaluatedDevelSourceBuildProcess::GitMirrorObservation);
    require_unmodified_git_object_semantics(
        git, null_input, workspace, workspace_git.get(),
        EvaluatedDevelSourceBuildProcess::GitWorkspaceObservation);
    return GitProofPoint{
        workspace_oid, *workspace_revision.git_object_format()};
}

void require_empty_pkgdest(
    int pkgdest_descriptor,
    EvaluatedDevelSourceBuildStage stage) {
    if(!directory_names(
            pkgdest_descriptor, stage,
            EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch)
            .empty()) {
        throw_build_failure(
            stage,
            EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch);
    }
}

struct OpenedArtifact {
    fs::path path;
    std::string leaf_name;
    OwnedDescriptor descriptor;
    NodeIdentity identity;
};

PackageArchiveSha256Digest hash_artifact_archive(
    const OpenedArtifact& artifact) {
    if(artifact.identity.size == 0 ||
       artifact.identity.size > MAX_ARTIFACT_BYTES ||
       artifact.identity.size > static_cast<std::uintmax_t>(
                                    std::numeric_limits<off_t>::max())) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArchiveDigest,
            EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded);
    }
    const NodeIdentity before = descriptor_identity(
        artifact.descriptor.get(),
        EvaluatedDevelSourceBuildStage::ArchiveDigest,
        EvaluatedDevelSourceBuildFailureReason::ArtifactHashFailure);
    PackageArchiveSha256Digest digest = [&artifact]() {
        try {
            return PackageArchiveSha256Digest::make(
                xdg_generation_store_file_descriptor_sha256(
                    artifact.descriptor.get(), artifact.identity.size,
                    MAX_ARTIFACT_BYTES));
        } catch(const std::exception&) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ArchiveDigest,
                EvaluatedDevelSourceBuildFailureReason::ArtifactHashFailure);
        }
    }();
    const NodeIdentity after = descriptor_identity(
        artifact.descriptor.get(),
        EvaluatedDevelSourceBuildStage::ArchiveDigest,
        EvaluatedDevelSourceBuildFailureReason::ArtifactHashFailure);
    if(!same_stable_file(artifact.identity, before) ||
       !same_stable_file(artifact.identity, after)) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArchiveDigest,
            EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement);
    }
    return digest;
}

OpenedArtifact open_fresh_artifact(
    int pkgdest_descriptor,
    const fs::path& pkgdest_path,
    const fs::path& expected_path,
    std::uintmax_t root_device,
    const fs::path& owned_root) {
    const std::vector<std::string> names = directory_names(
        pkgdest_descriptor,
        EvaluatedDevelSourceBuildStage::ArtifactInventory,
        EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch,
        MAX_SOURCE_ENTRIES);
    if(names.size() != 1 ||
       names.front() != expected_path.filename().string()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch);
    }
    const fs::path artifact_path = pkgdest_path / names.front();
    struct stat named_status{};
    if(::fstatat(
           pkgdest_descriptor, names.front().c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch,
            errno);
    }
    const NodeIdentity named = node_identity(named_status);
    if(named.type != S_IFREG || named.device != root_device ||
       named.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       named.links != 1 || named.size == 0 ||
       named.size > MAX_ARTIFACT_BYTES || (named.mode & 0022U) != 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            named.size > MAX_ARTIFACT_BYTES
                ? EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded
                : EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch);
    }
    notify_test_event(
        EvaluatedDevelSourceBuildTestEvent::AfterArtifactInventory,
        owned_root, artifact_path);
    int descriptor;
    do {
        descriptor = ::openat(
            pkgdest_descriptor, names.front().c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    } while(descriptor < 0 && errno == EINTR);
    if(descriptor < 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement,
            errno);
    }
    OwnedDescriptor opened(descriptor);
    const NodeIdentity retained = descriptor_identity(
        opened.get(),
        EvaluatedDevelSourceBuildStage::ArtifactInventory,
        EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement);
    if(!same_stable_file(named, retained)) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement);
    }
    notify_test_event(
        EvaluatedDevelSourceBuildTestEvent::AfterArtifactOpen,
        owned_root, artifact_path);
    return OpenedArtifact{
        artifact_path, names.front(), std::move(opened), named};
}

void require_artifact_unchanged(
    int pkgdest_descriptor,
    const OpenedArtifact& artifact,
    const fs::path& owned_root) {
    notify_test_event(
        EvaluatedDevelSourceBuildTestEvent::BeforeFinalArtifactReproof,
        owned_root, artifact.path);
    const NodeIdentity retained = descriptor_identity(
        artifact.descriptor.get(),
        EvaluatedDevelSourceBuildStage::ArtifactInventory,
        EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement);
    struct stat named_status{};
    if(::fstatat(
           pkgdest_descriptor, artifact.leaf_name.c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement,
            errno);
    }
    const NodeIdentity named = node_identity(named_status);
    if(!same_stable_file(artifact.identity, retained) ||
       !same_stable_file(artifact.identity, named) ||
       directory_names(
           pkgdest_descriptor,
           EvaluatedDevelSourceBuildStage::ArtifactInventory,
           EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement,
           MAX_SOURCE_ENTRIES) !=
           std::vector<std::string>{artifact.leaf_name}) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactInventory,
            EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement);
    }
}

void rewind_artifact_descriptor(
    const OpenedArtifact& artifact,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason) {
    off_t result;
    do {
        result = ::lseek(artifact.descriptor.get(), 0, SEEK_SET);
    } while(result < 0 && errno == EINTR);
    if(result != 0) throw_build_failure(stage, reason, errno);
}

std::string capture_archive_command(
    const FixedExecutable& bsdtar,
    const OpenedArtifact& artifact,
    const OwnedDescriptor& root_directory,
    std::vector<std::string> arguments,
    std::size_t capture_limit,
    EvaluatedDevelSourceBuildStage stage,
    EvaluatedDevelSourceBuildFailureReason reason,
    EvaluatedDevelSourceBuildProcess process) {
    bsdtar.require_validity();
    rewind_artifact_descriptor(artifact, stage, reason);
    std::vector<std::string> environment{
        "PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C"};
    const std::string output = require_successful_process_output(
        bsdtar.path().string(), bsdtar.descriptor(),
        std::move(arguments), std::move(environment),
        root_directory.get(), artifact.descriptor.get(),
        BoundedProcessPolicy{
            METADATA_PROCESS_TIMEOUT, PROCESS_TERMINATION_GRACE,
            capture_limit, true},
        stage, reason, process);
    bsdtar.require_validity();
    return output;
}

void validate_archive_metadata_inventory(
    const FixedExecutable& bsdtar,
    const OpenedArtifact& artifact,
    const OwnedDescriptor& root_directory) {
    const std::string listing = capture_archive_command(
        bsdtar, artifact, root_directory,
        {"-tf", "-"}, MAX_ARCHIVE_LISTING_BYTES,
        EvaluatedDevelSourceBuildStage::ArtifactMetadata,
        EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch,
        EvaluatedDevelSourceBuildProcess::ArchiveInventory);
    if(listing.empty() || listing.back() != '\n') {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactMetadata,
            EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch);
    }
    std::size_t entry_count = 0;
    std::size_t pkginfo_count = 0;
    std::size_t mtree_count = 0;
    std::size_t offset = 0;
    while(offset < listing.size()) {
        const std::size_t end = listing.find('\n', offset);
        if(end == std::string::npos || end == offset ||
           end - offset > MAX_ARCHIVE_MEMBER_PATH_BYTES ||
           ++entry_count > MAX_ARCHIVE_ENTRIES) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ArtifactMetadata,
                entry_count > MAX_ARCHIVE_ENTRIES
                    ? EvaluatedDevelSourceBuildFailureReason::ResourceLimitExceeded
                    : EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch);
        }
        const std::string_view member(
            listing.data() + offset, end - offset);
        if(std::any_of(
               member.begin(), member.end(), [](unsigned char character) {
                   return character < 0x20U || character == 0x7fU;
               })) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ArtifactMetadata,
                EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch);
        }
        if(member == ".PKGINFO") ++pkginfo_count;
        if(member == ".MTREE") ++mtree_count;
        offset = end + 1;
    }
    if(pkginfo_count != 1 || mtree_count != 1) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactMetadata,
            EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch);
    }
    const std::string pkginfo = capture_archive_command(
        bsdtar, artifact, root_directory,
        {"-xOf", "-", ".PKGINFO"}, MAX_PKGINFO_BYTES,
        EvaluatedDevelSourceBuildStage::ArtifactMetadata,
        EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch,
        EvaluatedDevelSourceBuildProcess::PackageMetadataExtraction);
    if(pkginfo.empty() || pkginfo.back() != '\n') {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::ArtifactMetadata,
            EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch);
    }
}

std::string extract_mtree_bytes(
    const FixedExecutable& bsdtar,
    const OpenedArtifact& artifact,
    const OwnedDescriptor& root_directory) {
    const std::string output = capture_archive_command(
        bsdtar, artifact, root_directory,
        {"-xOf", "-", ".MTREE"}, MAX_MTREE_BYTES,
        EvaluatedDevelSourceBuildStage::MtreeDigest,
        EvaluatedDevelSourceBuildFailureReason::MtreeFailure,
        EvaluatedDevelSourceBuildProcess::MtreeExtraction);
    if(output.empty()) {
        throw_build_failure(
            EvaluatedDevelSourceBuildStage::MtreeDigest,
            EvaluatedDevelSourceBuildFailureReason::MtreeFailure);
    }
    return output;
}

void attach_cleanup_consequence(
    InvocationOwnedSourceBuildContext& context,
    EvaluatedDevelSourceBuildFailure& failure) noexcept {
    if(!context.valid()) return;
    InvocationOwnedSourceBuildContextCleanupResult cleanup =
        context.cleanup();
    if(auto* cleanup_failure =
           std::get_if<InvocationOwnedSourceBuildContextFailure>(&cleanup)) {
        failure.cleanup_consequence =
            EvaluatedDevelSourceBuildCleanupConsequence{
                std::move(*cleanup_failure), context.owned_root()};
    }
}

} // namespace

struct EvaluatedDevelSourceBuildProof::State {
    InvocationOwnedSourceBuildContext context;
    EvaluatedDevelSourceProjection evaluated_source;
    ActualBuiltGitRevision actual_revision;
    FreshDevelPackageArtifact artifact;

    State(
        InvocationOwnedSourceBuildContext value_context,
        EvaluatedDevelSourceProjection value_evaluated_source,
        ActualBuiltGitRevision value_actual_revision,
        FreshDevelPackageArtifact value_artifact) noexcept
        : context(std::move(value_context)),
          evaluated_source(std::move(value_evaluated_source)),
          actual_revision(std::move(value_actual_revision)),
          artifact(std::move(value_artifact)) {
    }
};

EvaluatedDevelSourceProjection::EvaluatedDevelSourceProjection(
    VcsSourceIdentity git_source,
    std::size_t source_count,
    std::size_t tracked_local_source_count) noexcept
    : git_source_(std::move(git_source)), source_count_(source_count),
      tracked_local_source_count_(tracked_local_source_count) {
}

const VcsSourceIdentity&
EvaluatedDevelSourceProjection::git_source() const noexcept {
    return git_source_;
}

std::size_t EvaluatedDevelSourceProjection::source_count() const noexcept {
    return source_count_;
}

std::size_t
EvaluatedDevelSourceProjection::tracked_local_source_count() const noexcept {
    return tracked_local_source_count_;
}

FreshDevelPackageArtifact::FreshDevelPackageArtifact(
    PackageChildIdentity package,
    BuiltPackageArtifactEvidence evidence,
    fs::path path,
    std::string leaf_name,
    int descriptor,
    std::uintmax_t device,
    std::uintmax_t inode,
    std::uintmax_t owner,
    std::uintmax_t size) noexcept
    : package_(std::move(package)), evidence_(std::move(evidence)),
      path_(std::move(path)), leaf_name_(std::move(leaf_name)),
      descriptor_(descriptor), device_(device), inode_(inode),
      owner_(owner), size_(size) {
}

FreshDevelPackageArtifact::FreshDevelPackageArtifact(
    FreshDevelPackageArtifact&& other) noexcept
    : package_(std::move(other.package_)),
      evidence_(std::move(other.evidence_)), path_(std::move(other.path_)),
      leaf_name_(std::move(other.leaf_name_)),
      descriptor_(std::exchange(other.descriptor_, -1)),
      device_(other.device_), inode_(other.inode_), owner_(other.owner_),
      size_(other.size_) {
}

FreshDevelPackageArtifact::~FreshDevelPackageArtifact() noexcept {
    if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
}

const PackageChildIdentity&
FreshDevelPackageArtifact::package() const noexcept {
    return package_;
}

const BuiltPackageArtifactEvidence&
FreshDevelPackageArtifact::evidence() const noexcept {
    return evidence_;
}

const fs::path& FreshDevelPackageArtifact::path() const noexcept {
    return path_;
}

std::uintmax_t FreshDevelPackageArtifact::size() const noexcept {
    return size_;
}

EvaluatedDevelSourceBuildProof::EvaluatedDevelSourceBuildProof(
    std::unique_ptr<State> state)
    : state_(std::move(state)) {
}

EvaluatedDevelSourceBuildProof::EvaluatedDevelSourceBuildProof(
    EvaluatedDevelSourceBuildProof&& other) noexcept = default;

EvaluatedDevelSourceBuildProof::~EvaluatedDevelSourceBuildProof() noexcept =
    default;

bool EvaluatedDevelSourceBuildProof::valid() const noexcept {
    return state_ != nullptr && state_->context.valid() &&
           state_->artifact.descriptor_ >= 0;
}

const EvaluatedDevelSourceBuildProof::State&
EvaluatedDevelSourceBuildProof::require_state() const {
    if(!valid()) {
        throw std::logic_error(
            "Evaluated devel source-build proof is inactive.");
    }
    return *state_;
}

EvaluatedDevelSourceBuildProof::State&
EvaluatedDevelSourceBuildProof::require_state() {
    if(!valid()) {
        throw std::logic_error(
            "Evaluated devel source-build proof is inactive.");
    }
    return *state_;
}

const PackageBaseIdentity&
EvaluatedDevelSourceBuildProof::package_base() const {
    return require_state().context.package_base();
}

const ReviewedSourceStateRecordBinding&
EvaluatedDevelSourceBuildProof::reviewed_binding() const {
    return require_state().context.reviewed_binding();
}

const ReviewedRecipeSnapshotIdentity&
EvaluatedDevelSourceBuildProof::snapshot_identity() const {
    return require_state().context.snapshot_identity();
}

const EvaluatedDevelSourceProjection&
EvaluatedDevelSourceBuildProof::evaluated_source() const {
    return require_state().evaluated_source;
}

const ActualBuiltGitRevision&
EvaluatedDevelSourceBuildProof::actual_built_revision() const {
    return require_state().actual_revision;
}

const FreshDevelPackageArtifact&
EvaluatedDevelSourceBuildProof::artifact() const {
    return require_state().artifact;
}

InvocationOwnedSourceBuildContextCleanupResult
EvaluatedDevelSourceBuildProof::cleanup() noexcept {
    if(state_ == nullptr) {
        InvocationOwnedSourceBuildContextFailure failure;
        failure.stage = InvocationOwnedSourceBuildContextStage::Cleanup;
        failure.reason =
            InvocationOwnedSourceBuildContextFailureReason::InvalidState;
        return failure;
    }
    InvocationOwnedSourceBuildContextCleanupResult result =
        state_->context.cleanup();
    if(std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(
           result) &&
       state_->artifact.descriptor_ >= 0) {
        static_cast<void>(::close(state_->artifact.descriptor_));
        state_->artifact.descriptor_ = -1;
    }
    return result;
}

EvaluatedDevelSourceBuildResult EvaluatedDevelSourceBuildAuthority::build(
    InvocationOwnedSourceBuildContext context,
    InvocationOwnedMakepkgEnvironment environment) {
    bool package_build_started = false;
    const auto cleanup_after_failure = [&](EvaluatedDevelSourceBuildFailure& failure) {
        if(context.valid()) {
            // A failed artifact proof cannot grant a fresh cleanup scan new
            // authority. Failure before artifact validation can also leave
            // partial recipe output, so prove PKGDEST empty before allowing it.
            bool has_unproven_content = package_build_started ||
                                        failure.reason == EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch ||
                                        failure.reason == EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement ||
                                        failure.reason == EvaluatedDevelSourceBuildFailureReason::SourceWorkspaceAmbiguous ||
                                        failure.reason == EvaluatedDevelSourceBuildFailureReason::SourceContainmentFailure;
            if(!has_unproven_content) {
                try {
                    require_empty_pkgdest(context.pkgdest_descriptor(),
                                          EvaluatedDevelSourceBuildStage::Cleanup);
                } catch(...) {
                    // Unavailable inventory is not permission to delete. Keep
                    // the primary error and report retention independently.
                    has_unproven_content = true;
                }
            }
            if(has_unproven_content) context.refuse_unproven_cleanup();
        }
        attach_cleanup_consequence(context, failure);
    };
    try {
        if(!context.valid()) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext);
        }
        if(!context.owns_makepkg_environment(environment)) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::EnvironmentLineageMismatch);
        }
        InvocationOwnedSourceBuildContextValidationResult validation =
            context.revalidate();
        if(auto* failure =
               std::get_if<InvocationOwnedSourceBuildContextFailure>(
                   &validation)) {
            EvaluatedDevelSourceBuildFailure projected = build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::ContextRevalidationFailure);
            projected.context_failure = std::move(*failure);
            throw BuildFailureError(std::move(projected));
        }

        std::vector<std::string> makepkg_environment =
            make_explicit_environment(environment, context);
        WorkingRecipe working_recipe = create_working_recipe(
            context.recipe_descriptor(), context.builddir_descriptor(),
            context.builddir(), context.root_device());
        require_exact_working_recipe(
            working_recipe, context.root_device());
        notify_test_event(
            EvaluatedDevelSourceBuildTestEvent::WorkingRecipeReady,
            context.owned_root());

        OpenedRegularFile reviewed_srcinfo = open_and_read_regular_file(
            context.recipe_descriptor(), ".SRCINFO", context.root_device(),
            MAX_SRCINFO_BYTES,
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildFailureReason::EvaluatedSourceFailure);
        OwnedDescriptor null_input(::open(
            "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        OwnedDescriptor root_directory(::open(
            "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if(!null_input.valid() || !root_directory.valid()) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ContextValidation,
                EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext,
                errno);
        }

        const auto run_makepkg = [&](std::vector<std::string> arguments,
                                     EvaluatedDevelSourceBuildStage stage,
                                     EvaluatedDevelSourceBuildProcess process,
                                     std::chrono::milliseconds timeout,
                                     std::size_t capture_limit) {
            InvocationOwnedSourceBuildContextValidationResult current =
                context.revalidate();
            if(auto* failure =
                   std::get_if<InvocationOwnedSourceBuildContextFailure>(
                       &current)) {
                EvaluatedDevelSourceBuildFailure projected = build_failure(
                    stage,
                    EvaluatedDevelSourceBuildFailureReason::ContextRevalidationFailure);
                projected.context_failure = std::move(*failure);
                throw BuildFailureError(std::move(projected));
            }
            require_exact_working_recipe(
                working_recipe, context.root_device());
            return require_successful_process_output(
                context.makepkg_executable().path().string(),
                context.makepkg_executable().descriptor_,
                std::move(arguments), makepkg_environment,
                working_recipe.descriptor.get(), null_input.get(),
                BoundedProcessPolicy{
                    timeout, PROCESS_TERMINATION_GRACE,
                    capture_limit, false},
                stage,
                EvaluatedDevelSourceBuildFailureReason::MakepkgPhaseFailure,
                process);
        };

        notify_test_event(
            EvaluatedDevelSourceBuildTestEvent::BeforeInitialPrintSrcinfo,
            context.owned_root());
        const std::string initial_srcinfo = run_makepkg(
            {"--printsrcinfo"},
            EvaluatedDevelSourceBuildStage::EvaluatedSource,
            EvaluatedDevelSourceBuildProcess::InitialPrintSrcinfo,
            METADATA_PROCESS_TIMEOUT, MAX_SRCINFO_BYTES);
        SourceProjectionAnalysis source_analysis =
            analyze_source_projection(
                reviewed_srcinfo.bytes, initial_srcinfo,
                context.recipe_descriptor(), context.root_device(),
                context.package_base());
        require_empty_pkgdest(
            context.pkgdest_descriptor(),
            EvaluatedDevelSourceBuildStage::SourcePreparation);

        // `makepkg --nobuild` must be allowed to update only the private
        // working PKGBUILD. The exact reviewed snapshot remains immutable in
        // context.recipe_root(); the updated copy is sealed immediately after
        // source preparation and is the sole input to later phases.
        static_cast<void>(run_makepkg(
            {"--nobuild", "--nodeps", "--noconfirm"},
            EvaluatedDevelSourceBuildStage::SourcePreparation,
            EvaluatedDevelSourceBuildProcess::SourcePreparation,
            PREPARATION_PROCESS_TIMEOUT, MAX_MAKEPKG_OUTPUT_BYTES));
        notify_test_event(
            EvaluatedDevelSourceBuildTestEvent::AfterSourcePreparation,
            context.owned_root());
        seal_dynamic_pkgbuild(
            working_recipe, context.root_device(), true);
        require_empty_pkgdest(
            context.pkgdest_descriptor(),
            EvaluatedDevelSourceBuildStage::SourcePreparation);

        const std::string prepared_srcinfo = run_makepkg(
            {"--printsrcinfo"},
            EvaluatedDevelSourceBuildStage::DynamicVersion,
            EvaluatedDevelSourceBuildProcess::PreparedPrintSrcinfo,
            METADATA_PROCESS_TIMEOUT, MAX_SRCINFO_BYTES);
        PreparedPackageExpectation package_expectation =
            prepared_package_expectation(
                prepared_srcinfo, reviewed_srcinfo.bytes,
                source_analysis, context.recipe_descriptor(),
                context.root_device(), context.package_base());
        const SelectedPackageOutput selected_output =
            parse_expected_package_output(
                run_makepkg(
                    {"--packagelist"},
                    EvaluatedDevelSourceBuildStage::DynamicVersion,
                    EvaluatedDevelSourceBuildProcess::PreparedPackagelist,
                    METADATA_PROCESS_TIMEOUT, MAX_PACKAGELIST_BYTES),
                context, package_expectation);
        require_empty_pkgdest(
            context.pkgdest_descriptor(),
            EvaluatedDevelSourceBuildStage::DynamicVersion);

        FixedExecutable git("/usr/bin/git");
        RetainedDirectory mirror = locate_git_mirror(
            context.srcdest_descriptor(), context.srcdest(),
            context.root_device());
        RetainedDirectory workspace = locate_git_workspace(
            context.builddir_descriptor(), context.builddir(),
            context.root_device());
        GitProofPoint prepared_git = observe_git_proof_point(
            git, null_input, mirror, workspace,
            source_analysis.git_source);

        require_empty_pkgdest(
            context.pkgdest_descriptor(),
            EvaluatedDevelSourceBuildStage::PackageBuild);
        make_pkgbuild_mutable_for_build(
            working_recipe, context.root_device());
        notify_test_event(
            EvaluatedDevelSourceBuildTestEvent::BeforePackageBuild,
            context.owned_root());
        package_build_started = true;
        static_cast<void>(run_makepkg(
            {"--noextract", "--nodeps", "--noconfirm"},
            EvaluatedDevelSourceBuildStage::PackageBuild,
            EvaluatedDevelSourceBuildProcess::PackageBuild,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                BUILD_PROCESS_TIMEOUT),
            MAX_MAKEPKG_OUTPUT_BYTES));
        notify_test_event(
            EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild,
            context.owned_root());

        seal_dynamic_pkgbuild(
            working_recipe, context.root_device(), false);

        InvocationOwnedSourceBuildContextValidationResult post_build_context =
            context.revalidate();
        if(auto* failure =
               std::get_if<InvocationOwnedSourceBuildContextFailure>(
                   &post_build_context)) {
            EvaluatedDevelSourceBuildFailure projected = build_failure(
                EvaluatedDevelSourceBuildStage::PackageBuild,
                EvaluatedDevelSourceBuildFailureReason::ContextRevalidationFailure);
            projected.context_failure = std::move(*failure);
            throw BuildFailureError(std::move(projected));
        }
        require_exact_working_recipe(
            working_recipe, context.root_device());
        require_named_directory_unchanged(
            context.srcdest_descriptor(), mirror,
            EvaluatedDevelSourceBuildStage::GitRevision);
        require_named_directory_unchanged(
            context.builddir_descriptor(), workspace,
            EvaluatedDevelSourceBuildStage::GitRevision);
        GitProofPoint post_build_git = observe_git_proof_point(
            git, null_input, mirror, workspace,
            source_analysis.git_source);
        if(prepared_git.object_id != post_build_git.object_id ||
           prepared_git.object_format != post_build_git.object_format) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRevisionMismatch);
        }

        MakepkgManagedGitWorkspaceRevisionObservation observation(
            UpstreamGitRevision::git_commit(
                source_analysis.git_source,
                prepared_git.object_id),
            UpstreamGitRevision::git_commit(
                source_analysis.git_source,
                post_build_git.object_id));
        ActualBuiltGitRevisionProofResult revision_result =
            prove_actual_built_git_revision(std::move(observation));
        if(auto* revision_failure =
               std::get_if<ActualBuiltGitRevisionProofFailure>(
                   &revision_result)) {
            EvaluatedDevelSourceBuildFailure failure = build_failure(
                EvaluatedDevelSourceBuildStage::GitRevision,
                EvaluatedDevelSourceBuildFailureReason::GitRevisionMismatch);
            failure.revision_failure = *revision_failure;
            throw BuildFailureError(std::move(failure));
        }
        ActualBuiltGitRevision actual_revision =
            std::get<ActualBuiltGitRevision>(
                std::move(revision_result));

        OpenedArtifact opened_artifact = open_fresh_artifact(
            context.pkgdest_descriptor(), context.pkgdest(),
            selected_output.path, context.root_device(),
            context.owned_root());
        PackageArchiveSha256Digest archive_digest =
            hash_artifact_archive(opened_artifact);
        FixedExecutable bsdtar("/usr/bin/bsdtar");
        validate_archive_metadata_inventory(
            bsdtar, opened_artifact, root_directory);

        ArtifactPackageIdentity artifact_identity = [&]() {
            try {
                artifact_archive_metadata::RetainedDescriptorQueryAuthority
                    metadata_authority(opened_artifact.descriptor.get());
                return artifact_archive_metadata::query_with_libalpm(metadata_authority);
            } catch(const std::runtime_error& error) {
                // Only the retained archive query boundary owns this translation.
                // Allocation/logic errors and unrelated phases keep their own layer.
                auto failure = build_failure(
                    EvaluatedDevelSourceBuildStage::ArtifactMetadata,
                    EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataQueryFailure);
                failure.diagnostic = error.what();
                throw BuildFailureError(std::move(failure));
            }
        }();
        const std::string* artifact_package_base =
            artifact_identity.package_base.value();
        const std::string* artifact_architecture =
            artifact_identity.architecture.value();
        if(artifact_identity.package_name !=
               package_expectation.package_name ||
           artifact_identity.full_version !=
               package_expectation.full_version ||
           artifact_identity.package_base.state() !=
               ArtifactMetadataValueState::Known ||
           artifact_package_base == nullptr ||
           *artifact_package_base != context.package_base().package_base() ||
           artifact_identity.architecture.state() !=
               ArtifactMetadataValueState::Known ||
           artifact_architecture == nullptr ||
           *artifact_architecture != selected_output.architecture) {
            throw_build_failure(
                EvaluatedDevelSourceBuildStage::ArtifactMetadata,
                EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch);
        }

        const std::string mtree_bytes = extract_mtree_bytes(
            bsdtar, opened_artifact, root_directory);
        AlpmMtreeSha256Digest mtree_digest =
            AlpmMtreeSha256Digest::make(
                xdg_generation_store_raw_contents_sha256(mtree_bytes));
        require_artifact_unchanged(
            context.pkgdest_descriptor(), opened_artifact,
            context.owned_root());

        PackageChildIdentity package = PackageChildIdentity::make(
            context.package_base(), package_expectation.package_name);
        BuiltPackageArtifactEvidence evidence{
            std::move(artifact_identity), std::move(archive_digest),
            std::move(mtree_digest)};
        FreshDevelPackageArtifact artifact(
            std::move(package), std::move(evidence),
            std::move(opened_artifact.path),
            std::move(opened_artifact.leaf_name),
            opened_artifact.descriptor.release(),
            opened_artifact.identity.device,
            opened_artifact.identity.inode,
            opened_artifact.identity.owner,
            opened_artifact.identity.size);
        EvaluatedDevelSourceProjection evaluated_projection(
            std::move(source_analysis.git_source),
            source_analysis.source_count,
            source_analysis.tracked_local_source_count);
        return EvaluatedDevelSourceBuildProof(
            std::make_unique<EvaluatedDevelSourceBuildProof::State>(
                std::move(context), std::move(evaluated_projection),
                std::move(actual_revision), std::move(artifact)));
    } catch(BuildFailureError& error) {
        EvaluatedDevelSourceBuildFailure failure = error.release();
        cleanup_after_failure(failure);
        return failure;
    } catch(const std::exception& error) {
        EvaluatedDevelSourceBuildFailure failure = build_failure(
            EvaluatedDevelSourceBuildStage::ContextValidation,
            EvaluatedDevelSourceBuildFailureReason::InternalFailure);
        failure.diagnostic = error.what();
        cleanup_after_failure(failure);
        return failure;
    } catch(...) {
        EvaluatedDevelSourceBuildFailure failure = build_failure(
            EvaluatedDevelSourceBuildStage::ContextValidation,
            EvaluatedDevelSourceBuildFailureReason::InternalFailure);
        cleanup_after_failure(failure);
        return failure;
    }
}

EvaluatedDevelSourceBuildResult build_evaluated_devel_source(
    InvocationOwnedSourceBuildContext context,
    InvocationOwnedMakepkgEnvironment environment) {
    return EvaluatedDevelSourceBuildAuthority::build(
        std::move(context), std::move(environment));
}

#ifdef MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
void set_evaluated_devel_source_build_test_hook(
    EvaluatedDevelSourceBuildTestHook hook) {
    g_build_test_hook = std::move(hook);
}
#endif
