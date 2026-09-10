#include "invocation_owned_source_build_context.hpp"

#include "logging.hpp"
#include "reviewed_source_review.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <linux/openat2.h>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view CONTEXT_ROOT_PARENT = "/tmp";
constexpr std::string_view CONTEXT_ROOT_PREFIX =
    "moguet-source-build-context-";
constexpr std::string_view RECIPE_ROOT_LEAF = "recipe";
constexpr std::string_view PKGDEST_ROOT_LEAF = "pkgdest";
constexpr std::string_view BUILDDIR_ROOT_LEAF = "build";
constexpr std::string_view SRCDEST_ROOT_LEAF = "srcdest";
constexpr std::string_view MAKEPKG_EXECUTABLE = "/usr/bin/makepkg";
constexpr mode_t PRIVATE_DIRECTORY_MODE = 0700;
constexpr mode_t SEALED_RECIPE_DIRECTORY_MODE = 0500;
constexpr mode_t SEALED_RECIPE_FILE_MODE = 0400;
constexpr mode_t SEALED_RECIPE_EXECUTABLE_MODE = 0500;
constexpr std::size_t RANDOM_NAME_BYTES = 16;
constexpr std::size_t MAX_NAME_ATTEMPTS = 32;
constexpr std::size_t MAX_SNAPSHOT_PATH_BYTES = 4096;
constexpr std::size_t MAX_SNAPSHOT_PATH_DEPTH = 256;
constexpr std::size_t MAX_SNAPSHOT_COMPONENT_BYTES = 255;
// Includes generated trees, with headroom above the supported snapshot depth.
constexpr std::size_t MAX_CLEANUP_ENTRIES = 65536;
constexpr std::size_t MAX_CLEANUP_DEPTH = 512;

#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
InvocationOwnedSourceBuildContextTestHook g_context_test_hook;
std::optional<fs::path> g_makepkg_path_for_test;
std::optional<fs::path> g_context_root_parent_for_test;
std::optional<std::pair<std::size_t, std::size_t>> g_cleanup_limits_for_test;

void notify_test_event(
    InvocationOwnedSourceBuildContextTestEvent event,
    const fs::path& owned_root) {
    if(g_context_test_hook) g_context_test_hook(event, owned_root);
}
#endif

void notify_before_private_root_creation() {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::BeforePrivateRootCreation,
        {});
#endif
}

void notify_after_root_created(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterRootCreated,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_after_child_mkdir(const fs::path& child_path) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterChildMkdir,
        child_path);
#else
    static_cast<void>(child_path);
#endif
}

void notify_after_child_retained(const fs::path& child_path) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterChildRetained,
        child_path);
#else
    static_cast<void>(child_path);
#endif
}

void notify_after_child_mode_sealed(const fs::path& child_path) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterChildModeSealed,
        child_path);
#else
    static_cast<void>(child_path);
#endif
}

void notify_before_child_final_open(const fs::path& child_path) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::BeforeChildFinalOpen,
        child_path);
#else
    static_cast<void>(child_path);
#endif
}

void notify_before_child_final_reproof(const fs::path& child_path) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::BeforeChildFinalReproof,
        child_path);
#else
    static_cast<void>(child_path);
#endif
}

void notify_after_recipe_created(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterRecipeCreated,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_after_pkgdest_created(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterPkgdestCreated,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_after_builddir_created(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterBuilddirCreated,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_after_srcdest_created(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterSrcdestCreated,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_after_private_roots_created(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::AfterPrivateRootsCreated,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_before_final_reviewed_source_reproof(
    const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::
            BeforeFinalReviewedSourceReproof,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

void notify_before_cleanup(const fs::path& owned_root) {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    notify_test_event(
        InvocationOwnedSourceBuildContextTestEvent::BeforeCleanup,
        owned_root);
#else
    static_cast<void>(owned_root);
#endif
}

InvocationOwnedSourceBuildContextFailure context_failure(
    InvocationOwnedSourceBuildContextStage stage,
    InvocationOwnedSourceBuildContextFailureReason reason,
    fs::path relative_path = {},
    std::optional<int> error_number = std::nullopt) {
    InvocationOwnedSourceBuildContextFailure failure;
    failure.stage = stage;
    failure.reason = reason;
    failure.relative_path = std::move(relative_path);
    if(error_number.has_value()) {
        failure.system_error = std::error_code(
            *error_number, std::generic_category());
    }
    return failure;
}

class ContextFailureError final : public std::exception {
public:
    explicit ContextFailureError(
        InvocationOwnedSourceBuildContextFailure failure) noexcept
        : failure_(std::move(failure)) {
    }

    [[nodiscard]] const char* what() const noexcept override {
        return "invocation-owned-source-build-context-failure";
    }

    [[nodiscard]] InvocationOwnedSourceBuildContextFailure release() noexcept {
        return std::move(failure_);
    }

private:
    InvocationOwnedSourceBuildContextFailure failure_;
};

[[noreturn]] void throw_context_failure(
    InvocationOwnedSourceBuildContextStage stage,
    InvocationOwnedSourceBuildContextFailureReason reason,
    const fs::path& relative_path = {},
    std::optional<int> error_number = std::nullopt) {
    throw ContextFailureError(context_failure(
        stage, reason, relative_path, error_number));
}

[[noreturn]] void throw_unretained_created_root_failure(
    InvocationOwnedSourceBuildContextFailureReason primary_reason,
    const fs::path& owned_root,
    std::optional<int> error_number = std::nullopt) {
    InvocationOwnedSourceBuildContextFailure failure = context_failure(
        InvocationOwnedSourceBuildContextStage::RootCreation,
        primary_reason, {}, error_number);
    InvocationOwnedSourceBuildContextConstructionCleanupFailure cleanup;
    cleanup.reason =
        InvocationOwnedSourceBuildContextFailureReason::CleanupFailure;
    cleanup.owned_root = owned_root;
    if(error_number.has_value()) {
        cleanup.system_error = std::error_code(
            *error_number, std::generic_category());
    }
    cleanup.diagnostic =
        "Created context root could not be safely retained for abort cleanup.";
    failure.construction_cleanup_failure = std::move(cleanup);
    throw ContextFailureError(std::move(failure));
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

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int replacement = -1) noexcept {
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
        descriptor_ = replacement;
    }

private:
    int descriptor_ = -1;
};

struct CapturedMakepkgExecutable {
    fs::path path;
    OwnedDescriptor descriptor;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
};

struct NodeIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
    std::uintmax_t size = 0;
    std::uintmax_t links = 0;
    mode_t type = 0;
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
        static_cast<mode_t>(status.st_mode & S_IFMT)};
}

bool same_node(const NodeIdentity& expected, const NodeIdentity& actual) noexcept {
    return expected.device == actual.device &&
           expected.inode == actual.inode &&
           expected.type == actual.type;
}

// POLICY(#476 Slice 3): a private context may be created below a root-owned
// system parent or an effective-user-owned private parent. Shared writable
// parents are accepted only with sticky rename/unlink semantics.
std::optional<InvocationOwnedSourceBuildContextFailureReason>
parent_policy_failure(
    std::uintmax_t owner, std::uintmax_t mode,
    std::uintmax_t effective_user) noexcept {
    if(owner != 0 && owner != effective_user) {
        return InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch;
    }
    if((mode & 0022U) != 0 && (mode & S_ISVTX) == 0) {
        return InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions;
    }
    return std::nullopt;
}

fs::path selected_context_root_parent() {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    if(g_context_root_parent_for_test.has_value()) {
        return *g_context_root_parent_for_test;
    }
#endif
    return fs::path(CONTEXT_ROOT_PARENT);
}

void require_safe_parent_identity(
    const NodeIdentity& expected, const NodeIdentity& opened,
    const NodeIdentity& named,
    InvocationOwnedSourceBuildContextStage stage) {
    if(opened.type != S_IFDIR || !same_node(expected, opened) ||
       !same_node(opened, named)) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure);
    }
    if(opened.owner != expected.owner || named.owner != opened.owner) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch);
    }
    if(named.mode != opened.mode) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
    }
    if(const auto failure = parent_policy_failure(
           opened.owner, opened.mode,
           static_cast<std::uintmax_t>(::geteuid()));
       failure.has_value()) {
        throw_context_failure(stage, *failure);
    }
}

struct stat descriptor_status(
    int descriptor, InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path = {}) {
    struct stat status{};
    if(::fstat(descriptor, &status) != 0) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path, errno);
    }
    return status;
}

struct stat named_status(
    int parent_descriptor, const std::string& name,
    InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path = {}) {
    struct stat status{};
    if(::fstatat(
           parent_descriptor, name.c_str(), &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path, errno);
    }
    return status;
}

std::vector<std::string> directory_names(
    int directory_descriptor,
    InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path = {},
    std::size_t entry_limit = std::numeric_limits<std::size_t>::max()) {
    // LANDMINE: dup/fcntl would share the directory offset with the retained
    // authority descriptor. A fresh open-file-description is required for
    // every complete enumeration and revalidation pass.
    int scan_descriptor;
    do {
        scan_descriptor = ::openat(
            directory_descriptor, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while(scan_descriptor < 0 && errno == EINTR);
    if(scan_descriptor < 0) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path, errno);
    }
    OwnedDescriptor scan(scan_descriptor);
    DIR* raw_directory = ::fdopendir(scan.release());
    if(raw_directory == nullptr) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path, errno);
    }
    std::unique_ptr<DIR, int (*)(DIR*)> directory(raw_directory, ::closedir);
    std::vector<std::string> names;
    errno = 0;
    while(dirent* entry = ::readdir(directory.get())) {
        const std::string name(entry->d_name);
        if(name == "." || name == "..") continue;
        if(names.size() >= entry_limit) {
            throw_context_failure(stage,
                                  InvocationOwnedSourceBuildContextFailureReason::CleanupResourceLimitExceeded,
                                  relative_path);
        }
        names.push_back(name);
        errno = 0;
    }
    if(errno != 0) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path, errno);
    }
    std::sort(names.begin(), names.end());
    return names;
}

OwnedDescriptor open_child_directory(
    int parent_descriptor, const std::string& name,
    InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path,
    int access_flags = O_RDONLY | O_DIRECTORY) {
    struct open_how how{};
    how.flags = static_cast<std::uint64_t>(
        access_flags | O_CLOEXEC | O_NOFOLLOW);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    int descriptor;
    do {
        descriptor = static_cast<int>(::syscall(
            SYS_openat2, parent_descriptor, name.c_str(), &how,
            sizeof(how)));
    } while(descriptor < 0 && errno == EINTR);
    if(descriptor < 0) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path, errno);
    }
    return OwnedDescriptor(descriptor);
}

bool has_ambiguous_leading_double_slash(std::string_view path) noexcept {
    return path.size() >= 2 && path[0] == '/' && path[1] == '/' &&
           (path.size() == 2 || path[2] != '/');
}

std::optional<std::vector<std::string>> snapshot_path_components(
    const std::string& raw_path) {
    if(raw_path.empty() || raw_path.size() > MAX_SNAPSHOT_PATH_BYTES ||
       raw_path.front() == '/' || raw_path.back() == '/' ||
       raw_path.find('\0') != std::string::npos ||
       has_ambiguous_leading_double_slash(raw_path)) {
        return std::nullopt;
    }

    std::vector<std::string> components;
    std::size_t offset = 0;
    while(offset < raw_path.size()) {
        const std::size_t separator = raw_path.find('/', offset);
        const std::size_t length =
            separator == std::string::npos
                ? raw_path.size() - offset
                : separator - offset;
        std::string component = raw_path.substr(offset, length);
        if(component.empty() || component == "." || component == ".." ||
           component == ".git" ||
           component.size() > MAX_SNAPSHOT_COMPONENT_BYTES) {
            return std::nullopt;
        }
        components.push_back(std::move(component));
        if(components.size() > MAX_SNAPSHOT_PATH_DEPTH) {
            return std::nullopt;
        }
        if(separator == std::string::npos) break;
        offset = separator + 1;
    }
    return components.empty()
               ? std::nullopt
               : std::optional<std::vector<std::string>>(
                     std::move(components));
}

bool is_valid_environment_key(const std::string& key) noexcept {
    if(key.empty()) return false;
    const unsigned char first = static_cast<unsigned char>(key.front());
    if(std::isalpha(first) == 0 && key.front() != '_') return false;
    return std::all_of(
        key.begin() + 1, key.end(), [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '_';
        });
}

bool is_authority_owned_environment_key(std::string_view key) noexcept {
    return key == "PKGDEST" || key == "BUILDDIR" || key == "SRCDEST";
}

struct SnapshotFilePlan {
    std::string bytes;
    std::string sha256;
    std::uintmax_t size = 0;
    mode_t mode = SEALED_RECIPE_FILE_MODE;
};

struct SnapshotDirectoryPlan {
    std::map<std::string, SnapshotDirectoryPlan> directories;
    std::map<std::string, SnapshotFilePlan> files;
};

struct SnapshotPlan {
    SnapshotDirectoryPlan root;
    std::size_t tracked_entry_count = 0;
};

void insert_snapshot_file(
    SnapshotPlan& plan, const std::vector<std::string>& components,
    SnapshotFilePlan file, const fs::path& relative_path) {
    SnapshotDirectoryPlan* directory = &plan.root;
    for(std::size_t index = 0; index + 1 < components.size(); ++index) {
        const std::string& component = components[index];
        if(directory->files.contains(component)) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotProjection,
                InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
                relative_path);
        }
        directory = &directory->directories[component];
    }
    const std::string& leaf = components.back();
    if(directory->directories.contains(leaf) ||
       !directory->files.emplace(leaf, std::move(file)).second) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotProjection,
            InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
            relative_path);
    }
    ++plan.tracked_entry_count;
}

void write_all(
    int descriptor, std::string_view bytes,
    const fs::path& relative_path) {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const ssize_t written = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written < 0 && errno == EINTR) continue;
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path, written < 0 ? std::optional<int>(errno) : std::optional<int>(EIO));
    }
}

std::string read_exact_file(
    int descriptor, std::uintmax_t expected_size,
    InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path) {
    if(expected_size > std::numeric_limits<std::size_t>::max()) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path);
    }
    std::string contents;
    contents.resize(static_cast<std::size_t>(expected_size));
    std::size_t offset = 0;
    while(offset < contents.size()) {
        const ssize_t read_size = ::pread(
            descriptor, contents.data() + offset,
            contents.size() - offset, static_cast<off_t>(offset));
        if(read_size > 0) {
            offset += static_cast<std::size_t>(read_size);
            continue;
        }
        if(read_size < 0 && errno == EINTR) continue;
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            relative_path, read_size < 0 ? std::optional<int>(errno) : std::optional<int>(EIO));
    }
    std::array<char, 1> extra{};
    ssize_t extra_size;
    do {
        extra_size = ::pread(
            descriptor, extra.data(), extra.size(),
            static_cast<off_t>(contents.size()));
    } while(extra_size < 0 && errno == EINTR);
    if(extra_size != 0) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path,
            extra_size < 0 ? std::optional<int>(errno) : std::nullopt);
    }
    return contents;
}

void require_safe_directory_identity(
    const NodeIdentity& expected, const NodeIdentity& opened,
    const NodeIdentity& named, std::uintmax_t expected_device,
    mode_t expected_mode, InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path) {
    if(!same_node(expected, opened) || !same_node(opened, named) ||
       opened.type != S_IFDIR || opened.device != expected_device ||
       opened.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       named.owner != opened.owner) {
        throw_context_failure(
            stage,
            opened.owner != static_cast<std::uintmax_t>(::geteuid()) ||
                    named.owner != opened.owner
                ? InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch
                : InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
    if(opened.mode != static_cast<std::uintmax_t>(expected_mode) ||
       named.mode != opened.mode) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
            relative_path);
    }
}

void require_provisional_child_parent_identity(
    const NodeIdentity& expected, const NodeIdentity& opened,
    InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path) {
    if(!same_node(expected, opened) || opened.type != S_IFDIR ||
       opened.device != expected.device) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
            relative_path);
    }
    if(opened.owner != static_cast<std::uintmax_t>(::geteuid())) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch,
            relative_path);
    }
    if(opened.mode != expected.mode || opened.mode != PRIVATE_DIRECTORY_MODE) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
            relative_path);
    }
}

void require_provisional_child_identity(
    const NodeIdentity& expected, const NodeIdentity& opened,
    const NodeIdentity& named, std::uintmax_t expected_device,
    InvocationOwnedSourceBuildContextStage stage,
    const fs::path& relative_path) {
    if(!same_node(expected, opened) || !same_node(opened, named) ||
       opened.type != S_IFDIR) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
    if(opened.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       named.owner != opened.owner) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch,
            relative_path);
    }
    if(opened.device != expected_device) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
            relative_path);
    }
    if(named.mode != opened.mode) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
}

struct OwnedDirectory {
    std::string leaf;
    fs::path path;
    OwnedDescriptor descriptor;
    NodeIdentity identity;
    mode_t expected_mode = PRIVATE_DIRECTORY_MODE;
};

std::string random_context_leaf() {
    std::array<unsigned char, RANDOM_NAME_BYTES> random_bytes{};
    std::size_t offset = 0;
    while(offset < random_bytes.size()) {
        const ssize_t read_size = ::getrandom(
            random_bytes.data() + offset,
            random_bytes.size() - offset, 0);
        if(read_size > 0) {
            offset += static_cast<std::size_t>(read_size);
            continue;
        }
        if(read_size < 0 && errno == EINTR) continue;
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::RootCreation,
            InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
            {}, read_size < 0 ? std::optional<int>(errno) : std::optional<int>(EIO));
    }
    constexpr char HEX[] = "0123456789abcdef";
    std::string leaf(CONTEXT_ROOT_PREFIX);
    leaf.reserve(leaf.size() + random_bytes.size() * 2);
    for(const unsigned char byte : random_bytes) {
        leaf.push_back(HEX[byte >> 4]);
        leaf.push_back(HEX[byte & 0x0f]);
    }
    return leaf;
}

void set_created_directory_mode(
    int path_descriptor, mode_t mode,
    InvocationOwnedSourceBuildContextStage stage,
    InvocationOwnedSourceBuildContextFailureReason failure_reason,
    const fs::path& relative_path) {
    // LANDMINE: umask is process-global. mkdirat never requests group/other
    // access, then this retained no-follow descriptor establishes the exact
    // mode without changing another thread's creation policy.
    if(::syscall(
           SYS_fchmodat2, path_descriptor, "", mode,
           AT_EMPTY_PATH) != 0) {
        throw_context_failure(
            stage, failure_reason,
            relative_path, errno);
    }
    const NodeIdentity current = node_identity(descriptor_status(
        path_descriptor, stage, relative_path));
    if(current.type != S_IFDIR ||
       current.mode != static_cast<std::uintmax_t>(mode)) {
        throw_context_failure(
            stage,
            InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
            relative_path);
    }
}

// This owner is armed only after the newly named child and its retained O_PATH
// descriptor have been proven to be the same user-owned directory. Before
// that proof, pathname cleanup would be capable of deleting a replacement.
class ProvisionalOwnedChildDirectory final {
public:
    ProvisionalOwnedChildDirectory(
        int parent_descriptor, NodeIdentity parent_identity,
        std::string leaf, fs::path path, OwnedDescriptor descriptor,
        NodeIdentity identity) noexcept
        : parent_descriptor_(parent_descriptor),
          parent_identity_(parent_identity), leaf_(std::move(leaf)),
          path_(std::move(path)), descriptor_(std::move(descriptor)),
          identity_(identity) {
    }

    ProvisionalOwnedChildDirectory(
        const ProvisionalOwnedChildDirectory&) = delete;
    ProvisionalOwnedChildDirectory& operator=(
        const ProvisionalOwnedChildDirectory&) = delete;

    ProvisionalOwnedChildDirectory(
        ProvisionalOwnedChildDirectory&& other) noexcept
        : parent_descriptor_(other.parent_descriptor_),
          parent_identity_(other.parent_identity_),
          leaf_(std::move(other.leaf_)), path_(std::move(other.path_)),
          descriptor_(std::move(other.descriptor_)),
          identity_(other.identity_), active_(std::exchange(other.active_, false)) {
    }

    ProvisionalOwnedChildDirectory& operator=(
        ProvisionalOwnedChildDirectory&&) = delete;

    ~ProvisionalOwnedChildDirectory() noexcept {
        if(!active_) return;
        const auto cleanup_failure = cleanup();
        if(cleanup_failure.has_value()) {
            const fs::path& retained = path_;
            Logger::warn_noexcept([&retained]() {
                return "Provisional source-build child cleanup failed; retained path: " +
                       retained.string();
            });
        }
    }

    [[nodiscard]] const std::string& leaf() const noexcept {
        return leaf_;
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_.get();
    }

    [[nodiscard]] OwnedDirectory complete(
        OwnedDescriptor descriptor, NodeIdentity identity) noexcept {
        OwnedDirectory completed{
            std::move(leaf_), std::move(path_), std::move(descriptor),
            identity, PRIVATE_DIRECTORY_MODE};
        active_ = false;
        return completed;
    }

    [[nodiscard]] InvocationOwnedSourceBuildContextFailure abort_construction(
        InvocationOwnedSourceBuildContextFailure primary_failure) noexcept {
        const auto cleanup_failure = cleanup();
        if(cleanup_failure.has_value()) {
            try {
                primary_failure.construction_cleanup_failure =
                    InvocationOwnedSourceBuildContextConstructionCleanupFailure{
                        cleanup_failure->stage,
                        cleanup_failure->reason,
                        cleanup_failure->relative_path,
                        path_.parent_path(),
                        cleanup_failure->system_error,
                        cleanup_failure->diagnostic};
            } catch(...) {
                if(!primary_failure.diagnostic.has_value()) {
                    try {
                        primary_failure.diagnostic =
                            "Child creation failed and provisional cleanup also failed.";
                    } catch(...) {
                    }
                }
            }
        }
        return primary_failure;
    }

private:
    [[nodiscard]] std::optional<InvocationOwnedSourceBuildContextFailure>
    cleanup() noexcept {
        if(!active_) return std::nullopt;
        try {
            require_provisional_child_parent_identity(
                parent_identity_,
                node_identity(descriptor_status(
                    parent_descriptor_,
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    leaf_)),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            NodeIdentity retained = node_identity(descriptor_status(
                descriptor_.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            NodeIdentity named = node_identity(named_status(
                parent_descriptor_, leaf_,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            require_provisional_child_identity(
                identity_, retained, named, parent_identity_.device,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);

            // A restrictive caller umask may have created mode 0000. The
            // retained identity authorizes opening only this exact directory
            // for the empty-directory proof; no process-global policy changes.
            set_created_directory_mode(
                descriptor_.get(), PRIVATE_DIRECTORY_MODE,
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                leaf_);
            retained = node_identity(descriptor_status(
                descriptor_.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            named = node_identity(named_status(
                parent_descriptor_, leaf_,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            require_provisional_child_identity(
                identity_, retained, named, parent_identity_.device,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            if(retained.mode != PRIVATE_DIRECTORY_MODE) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                    leaf_);
            }

            OwnedDescriptor directory = open_child_directory(
                parent_descriptor_, leaf_,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            NodeIdentity opened = node_identity(descriptor_status(
                directory.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            require_provisional_child_identity(
                identity_, opened, named, parent_identity_.device,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            if(!directory_names(
                    directory.get(),
                    InvocationOwnedSourceBuildContextStage::Cleanup, leaf_)
                    .empty()) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                    leaf_);
            }

            require_provisional_child_parent_identity(
                parent_identity_,
                node_identity(descriptor_status(
                    parent_descriptor_,
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    leaf_)),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            retained = node_identity(descriptor_status(
                descriptor_.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            named = node_identity(named_status(
                parent_descriptor_, leaf_,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            opened = node_identity(descriptor_status(
                directory.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_));
            require_provisional_child_identity(
                identity_, retained, named, parent_identity_.device,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            require_provisional_child_identity(
                identity_, opened, named, parent_identity_.device,
                InvocationOwnedSourceBuildContextStage::Cleanup, leaf_);
            if(!directory_names(
                    directory.get(),
                    InvocationOwnedSourceBuildContextStage::Cleanup, leaf_)
                    .empty()) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                    leaf_);
            }
            if(::unlinkat(
                   parent_descriptor_, leaf_.c_str(), AT_REMOVEDIR) != 0) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                    leaf_, errno);
            }
            active_ = false;
            return std::nullopt;
        } catch(ContextFailureError& error) {
            active_ = false;
            InvocationOwnedSourceBuildContextFailure failure = error.release();
            failure.stage = InvocationOwnedSourceBuildContextStage::Cleanup;
            if(failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch &&
               failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure &&
               failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement &&
               failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions) {
                failure.reason =
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure;
            }
            return failure;
        } catch(const std::exception& error) {
            active_ = false;
            InvocationOwnedSourceBuildContextFailure failure = context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                leaf_);
            try {
                failure.diagnostic = error.what();
            } catch(...) {
            }
            return failure;
        } catch(...) {
            active_ = false;
            return context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                leaf_);
        }
    }

    int parent_descriptor_ = -1;
    NodeIdentity parent_identity_;
    std::string leaf_;
    fs::path path_;
    OwnedDescriptor descriptor_;
    NodeIdentity identity_;
    bool active_ = true;
};

OwnedDirectory create_owned_child_directory(
    int parent_descriptor, const fs::path& parent_path,
    std::string leaf, const NodeIdentity& parent_identity) {
    require_provisional_child_parent_identity(
        parent_identity,
        node_identity(descriptor_status(
            parent_descriptor,
            InvocationOwnedSourceBuildContextStage::RootCreation, leaf)),
        InvocationOwnedSourceBuildContextStage::RootCreation, leaf);
    if(::mkdirat(
           parent_descriptor, leaf.c_str(),
           PRIVATE_DIRECTORY_MODE) != 0) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::RootCreation,
            InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
            leaf, errno);
    }
    fs::path path = parent_path / leaf;
    OwnedDescriptor path_descriptor;
    NodeIdentity path_opened;
    try {
        notify_after_child_mkdir(path);
        path_descriptor = open_child_directory(
            parent_descriptor, leaf,
            InvocationOwnedSourceBuildContextStage::RootCreation,
            leaf, O_PATH);
        path_opened = node_identity(descriptor_status(
            path_descriptor.get(),
            InvocationOwnedSourceBuildContextStage::RootCreation, leaf));
        const NodeIdentity named = node_identity(named_status(
            parent_descriptor, leaf,
            InvocationOwnedSourceBuildContextStage::RootCreation, leaf));
        require_provisional_child_identity(
            path_opened, path_opened, named, parent_identity.device,
            InvocationOwnedSourceBuildContextStage::RootCreation, leaf);
    } catch(ContextFailureError&) {
        // No same-node retained authority was proven. The caller records the
        // root as potentially retained and must not unlink this pathname.
        throw;
    } catch(const std::exception& error) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::RootCreation,
            InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
            leaf);
        failure.diagnostic = error.what();
        throw ContextFailureError(std::move(failure));
    } catch(...) {
        throw ContextFailureError(context_failure(
            InvocationOwnedSourceBuildContextStage::RootCreation,
            InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
            leaf));
    }

    ProvisionalOwnedChildDirectory provisional(
        parent_descriptor, parent_identity, std::move(leaf), std::move(path),
        std::move(path_descriptor), path_opened);
    try {
        notify_after_child_retained(provisional.path());
        set_created_directory_mode(
            provisional.descriptor(), PRIVATE_DIRECTORY_MODE,
            InvocationOwnedSourceBuildContextStage::RootCreation,
            InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
            provisional.leaf());
        notify_after_child_mode_sealed(provisional.path());
        notify_before_child_final_open(provisional.path());
        OwnedDescriptor descriptor = open_child_directory(
            parent_descriptor, provisional.leaf(),
            InvocationOwnedSourceBuildContextStage::RootCreation,
            provisional.leaf());
        notify_before_child_final_reproof(provisional.path());
        const NodeIdentity opened = node_identity(descriptor_status(
            descriptor.get(),
            InvocationOwnedSourceBuildContextStage::RootCreation,
            provisional.leaf()));
        const NodeIdentity named = node_identity(named_status(
            parent_descriptor, provisional.leaf(),
            InvocationOwnedSourceBuildContextStage::RootCreation,
            provisional.leaf()));
        require_safe_directory_identity(
            path_opened, opened, named, parent_identity.device,
            PRIVATE_DIRECTORY_MODE,
            InvocationOwnedSourceBuildContextStage::RootCreation,
            provisional.leaf());
        return provisional.complete(std::move(descriptor), opened);
    } catch(ContextFailureError& error) {
        throw ContextFailureError(
            provisional.abort_construction(error.release()));
    } catch(const std::exception& error) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::RootCreation,
            InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
            provisional.leaf());
        try {
            failure.diagnostic = error.what();
        } catch(...) {
        }
        throw ContextFailureError(
            provisional.abort_construction(std::move(failure)));
    } catch(...) {
        throw ContextFailureError(provisional.abort_construction(
            context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                provisional.leaf())));
    }
}

struct CleanupBudget {
    std::size_t max_entries = MAX_CLEANUP_ENTRIES;
    std::size_t max_depth = MAX_CLEANUP_DEPTH;
    std::size_t entries = 0;

    CleanupBudget() {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
        if(g_cleanup_limits_for_test.has_value()) {
            max_entries = g_cleanup_limits_for_test->first;
            max_depth = g_cleanup_limits_for_test->second;
        }
#endif
    }

    void consume(std::size_t depth, const fs::path& relative_path) {
        if(entries >= max_entries || depth > max_depth) {
            throw_context_failure(InvocationOwnedSourceBuildContextStage::Cleanup,
                                  InvocationOwnedSourceBuildContextFailureReason::CleanupResourceLimitExceeded,
                                  relative_path);
        }
        ++entries;
    }
};

struct CleanupNode {
    std::string name;
    fs::path relative_path;
    NodeIdentity identity;
    bool is_directory = false;
    std::vector<CleanupNode> children;
};

void make_cleanup_directory_accessible(
    int path_descriptor, CleanupNode& node) {
    if(::syscall(
           SYS_fchmodat2, path_descriptor, "", PRIVATE_DIRECTORY_MODE,
           AT_EMPTY_PATH) != 0) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
            node.relative_path, errno);
    }
    NodeIdentity current = node_identity(descriptor_status(
        path_descriptor, InvocationOwnedSourceBuildContextStage::Cleanup,
        node.relative_path));
    if(!same_node(node.identity, current) || current.type != S_IFDIR ||
       current.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       current.mode != PRIVATE_DIRECTORY_MODE) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
            node.relative_path);
    }
    node.identity = current;
}

CleanupNode plan_cleanup_node(
    int parent_descriptor, const std::string& name,
    const fs::path& relative_path, std::uintmax_t root_device,
    CleanupBudget& budget, std::size_t depth) {
    budget.consume(depth, relative_path);
    NodeIdentity named = node_identity(named_status(
        parent_descriptor, name,
        InvocationOwnedSourceBuildContextStage::Cleanup, relative_path));
    if(named.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       named.device != root_device) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            named.owner != static_cast<std::uintmax_t>(::geteuid())
                ? InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch
                : InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
            relative_path);
    }

    CleanupNode node{name, relative_path, named, named.type == S_IFDIR, {}};
    OwnedDescriptor retained = open_child_directory(
        parent_descriptor, name,
        InvocationOwnedSourceBuildContextStage::Cleanup, relative_path,
        O_PATH);
    NodeIdentity opened = node_identity(descriptor_status(
        retained.get(), InvocationOwnedSourceBuildContextStage::Cleanup,
        relative_path));
    if(!same_node(named, opened) || opened.owner != named.owner ||
       opened.device != root_device) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
    node.identity = opened;
    if(!node.is_directory) return node;

    make_cleanup_directory_accessible(retained.get(), node);
    OwnedDescriptor directory = open_child_directory(
        parent_descriptor, name,
        InvocationOwnedSourceBuildContextStage::Cleanup, relative_path);
    NodeIdentity directory_identity = node_identity(descriptor_status(
        directory.get(), InvocationOwnedSourceBuildContextStage::Cleanup,
        relative_path));
    if(!same_node(node.identity, directory_identity)) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
    const std::vector<std::string> names = directory_names(
        directory.get(), InvocationOwnedSourceBuildContextStage::Cleanup,
        relative_path, budget.max_entries - budget.entries);
    node.children.reserve(names.size());
    for(const std::string& child : names) {
        node.children.push_back(plan_cleanup_node(
            directory.get(), child, relative_path / child,
            root_device, budget, depth + 1));
    }
    if(names != directory_names(
                    directory.get(),
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    relative_path, names.size())) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
    return node;
}

void require_cleanup_node_unchanged(
    int parent_descriptor, const CleanupNode& node,
    std::uintmax_t root_device) {
    const NodeIdentity current = node_identity(named_status(
        parent_descriptor, node.name,
        InvocationOwnedSourceBuildContextStage::Cleanup,
        node.relative_path));
    if(!same_node(node.identity, current) ||
       current.owner != static_cast<std::uintmax_t>(::geteuid()) ||
       current.device != root_device) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            node.relative_path);
    }
}

void validate_cleanup_plan(
    int parent_descriptor, const CleanupNode& node,
    std::uintmax_t root_device) {
    // The immutable plan already passed the shared entry/depth budget. Each
    // rescan is capped by its exact child inventory, so validation/removal cannot
    // expand that plan or begin an unbounded traversal of new recipe content.
    require_cleanup_node_unchanged(parent_descriptor, node, root_device);
    if(!node.is_directory) return;
    OwnedDescriptor directory = open_child_directory(
        parent_descriptor, node.name,
        InvocationOwnedSourceBuildContextStage::Cleanup,
        node.relative_path);
    const std::vector<std::string> names = directory_names(
        directory.get(), InvocationOwnedSourceBuildContextStage::Cleanup,
        node.relative_path, node.children.size());
    std::vector<std::string> expected_names;
    expected_names.reserve(node.children.size());
    for(const CleanupNode& child : node.children) {
        expected_names.push_back(child.name);
    }
    std::sort(expected_names.begin(), expected_names.end());
    if(names != expected_names) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            node.relative_path);
    }
    for(const CleanupNode& child : node.children) {
        validate_cleanup_plan(directory.get(), child, root_device);
    }
    require_cleanup_node_unchanged(parent_descriptor, node, root_device);
}

void remove_cleanup_plan(
    int parent_descriptor, const CleanupNode& node,
    std::uintmax_t root_device) {
    require_cleanup_node_unchanged(parent_descriptor, node, root_device);
    if(node.is_directory) {
        OwnedDescriptor directory = open_child_directory(
            parent_descriptor, node.name,
            InvocationOwnedSourceBuildContextStage::Cleanup,
            node.relative_path);
        for(const CleanupNode& child : node.children) {
            remove_cleanup_plan(directory.get(), child, root_device);
        }
        if(!directory_names(
                directory.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup,
                node.relative_path, 0)
                .empty()) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                node.relative_path);
        }
        require_cleanup_node_unchanged(
            parent_descriptor, node, root_device);
        if(::unlinkat(
               parent_descriptor, node.name.c_str(),
               AT_REMOVEDIR) != 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                node.relative_path, errno);
        }
        return;
    }
    if(::unlinkat(parent_descriptor, node.name.c_str(), 0) != 0) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
            node.relative_path, errno);
    }
}

class PrivateBuildRoot final {
public:
    PrivateBuildRoot(const PrivateBuildRoot&) = delete;
    PrivateBuildRoot& operator=(const PrivateBuildRoot&) = delete;

    PrivateBuildRoot(PrivateBuildRoot&& other) noexcept
        : parent_path_(std::move(other.parent_path_)),
          parent_descriptor_(std::move(other.parent_descriptor_)),
          parent_identity_(other.parent_identity_),
          root_(std::move(other.root_)), recipe_(std::move(other.recipe_)),
          pkgdest_(std::move(other.pkgdest_)),
          builddir_(std::move(other.builddir_)),
          srcdest_(std::move(other.srcdest_)),
          active_(std::exchange(other.active_, false)),
          cleanup_attempted_(other.cleanup_attempted_),
          cleanup_refusal_(other.cleanup_refusal_) {
    }

    PrivateBuildRoot& operator=(PrivateBuildRoot&&) = delete;

    ~PrivateBuildRoot() noexcept {
        if(!active_) return;
        const bool failed = cleanup_attempted_ || cleanup_refusal_.has_value() ||
                            std::holds_alternative<InvocationOwnedSourceBuildContextFailure>(cleanup());
        if(failed) {
            const fs::path retained = root_.path;
            Logger::warn_noexcept([&retained]() {
                return "Invocation-owned source-build context cleanup failed; retained root: " +
                       retained.string();
            });
        }
    }

    [[nodiscard]] static PrivateBuildRoot create() {
        fs::path parent_path = selected_context_root_parent();
        if(!parent_path.is_absolute() ||
           parent_path.root_path() != fs::path("/") ||
           parent_path.native().find('\0') != std::string::npos ||
           has_ambiguous_leading_double_slash(parent_path.native()) ||
           parent_path.lexically_normal() != parent_path) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure);
        }
        OwnedDescriptor parent(::open(
            parent_path.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if(!parent.valid()) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                {}, errno);
        }
        const struct stat parent_opened = descriptor_status(
            parent.get(),
            InvocationOwnedSourceBuildContextStage::RootCreation);
        struct stat parent_named{};
        if(::lstat(parent_path.c_str(), &parent_named) != 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                {}, errno);
        }
        const NodeIdentity parent_identity = node_identity(parent_opened);
        require_safe_parent_identity(
            parent_identity, parent_identity,
            node_identity(parent_named),
            InvocationOwnedSourceBuildContextStage::RootCreation);

        std::optional<std::string> root_leaf;
        for(std::size_t attempt = 0; attempt < MAX_NAME_ATTEMPTS; ++attempt) {
            std::string candidate = random_context_leaf();
            if(::mkdirat(
                   parent.get(), candidate.c_str(),
                   PRIVATE_DIRECTORY_MODE) == 0) {
                root_leaf = std::move(candidate);
                break;
            }
            if(errno == EEXIST) continue;
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                {}, errno);
        }
        if(!root_leaf.has_value()) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure);
        }

        fs::path root_path = parent_path / *root_leaf;
        int raw_root_descriptor;
        do {
            raw_root_descriptor = ::openat(
                parent.get(), root_leaf->c_str(),
                O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        } while(raw_root_descriptor < 0 && errno == EINTR);
        if(raw_root_descriptor < 0) {
            throw_unretained_created_root_failure(
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                root_path, errno);
        }
        OwnedDescriptor root_descriptor(raw_root_descriptor);
        struct stat root_opened_status{};
        if(::fstat(root_descriptor.get(), &root_opened_status) != 0) {
            throw_unretained_created_root_failure(
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                root_path, errno);
        }
        const NodeIdentity root_opened = node_identity(root_opened_status);
        struct stat root_named_status{};
        if(::fstatat(
               parent.get(), root_leaf->c_str(), &root_named_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw_unretained_created_root_failure(
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                root_path, errno);
        }
        const NodeIdentity root_named = node_identity(root_named_status);
        if(root_opened.type != S_IFDIR ||
           !same_node(root_named, root_opened)) {
            throw_unretained_created_root_failure(
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                root_path);
        }
        if(root_opened.owner !=
           static_cast<std::uintmax_t>(::geteuid())) {
            throw_unretained_created_root_failure(
                InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch,
                root_path);
        }
        if(root_opened.device != parent_identity.device) {
            throw_unretained_created_root_failure(
                InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
                root_path);
        }

        PrivateBuildRoot result(
            std::move(parent_path), std::move(parent),
            parent_identity,
            OwnedDirectory{
                std::move(*root_leaf), std::move(root_path),
                std::move(root_descriptor),
                root_opened, PRIVATE_DIRECTORY_MODE});
        try {
            set_created_directory_mode(
                result.root_.descriptor.get(), PRIVATE_DIRECTORY_MODE,
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure,
                {});
            const NodeIdentity exact_root = node_identity(descriptor_status(
                result.root_.descriptor.get(),
                InvocationOwnedSourceBuildContextStage::RootCreation));
            const NodeIdentity exact_named_root = node_identity(named_status(
                result.parent_descriptor_.get(), result.root_.leaf,
                InvocationOwnedSourceBuildContextStage::RootCreation));
            require_safe_directory_identity(
                result.root_.identity, exact_root, exact_named_root,
                result.parent_identity_.device, PRIVATE_DIRECTORY_MODE,
                InvocationOwnedSourceBuildContextStage::RootCreation, {});
            result.root_.identity = exact_root;
            notify_after_root_created(result.root_.path);
            result.recipe_.emplace(create_owned_child_directory(
                result.root_.descriptor.get(), result.root_.path,
                std::string(RECIPE_ROOT_LEAF),
                result.root_.identity));
            notify_after_recipe_created(result.root_.path);
            result.pkgdest_.emplace(create_owned_child_directory(
                result.root_.descriptor.get(), result.root_.path,
                std::string(PKGDEST_ROOT_LEAF),
                result.root_.identity));
            notify_after_pkgdest_created(result.root_.path);
            result.builddir_.emplace(create_owned_child_directory(
                result.root_.descriptor.get(), result.root_.path,
                std::string(BUILDDIR_ROOT_LEAF),
                result.root_.identity));
            notify_after_builddir_created(result.root_.path);
            result.srcdest_.emplace(create_owned_child_directory(
                result.root_.descriptor.get(), result.root_.path,
                std::string(SRCDEST_ROOT_LEAF),
                result.root_.identity));
            notify_after_srcdest_created(result.root_.path);
            notify_after_private_roots_created(result.root_.path);
            result.require_top_level_identity();
            return result;
        } catch(ContextFailureError& error) {
            throw ContextFailureError(
                result.abort_construction(error.release()));
        } catch(const std::exception& error) {
            InvocationOwnedSourceBuildContextFailure failure = context_failure(
                InvocationOwnedSourceBuildContextStage::RootCreation,
                InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure);
            failure.diagnostic = error.what();
            throw ContextFailureError(
                result.abort_construction(std::move(failure)));
        } catch(...) {
            throw ContextFailureError(result.abort_construction(
                context_failure(
                    InvocationOwnedSourceBuildContextStage::RootCreation,
                    InvocationOwnedSourceBuildContextFailureReason::RootCreationFailure)));
        }
    }

    [[nodiscard]] const fs::path& root_path() const noexcept {
        return root_.path;
    }

    [[nodiscard]] const fs::path& recipe_path() const noexcept {
        return recipe_->path;
    }

    [[nodiscard]] const fs::path& pkgdest_path() const noexcept {
        return pkgdest_->path;
    }

    [[nodiscard]] const fs::path& builddir_path() const noexcept {
        return builddir_->path;
    }

    [[nodiscard]] const fs::path& srcdest_path() const noexcept {
        return srcdest_->path;
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

    [[nodiscard]] int recipe_descriptor() const noexcept {
        return recipe_->descriptor.get();
    }

    [[nodiscard]] int pkgdest_descriptor() const noexcept {
        return pkgdest_->descriptor.get();
    }

    [[nodiscard]] int builddir_descriptor() const noexcept {
        return builddir_->descriptor.get();
    }

    [[nodiscard]] int srcdest_descriptor() const noexcept {
        return srcdest_->descriptor.get();
    }

    [[nodiscard]] std::uintmax_t device() const noexcept {
        return root_.identity.device;
    }

    void seal_recipe_root() {
        if(::fchmod(
               recipe_->descriptor.get(),
               SEALED_RECIPE_DIRECTORY_MODE) != 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
                RECIPE_ROOT_LEAF, errno);
        }
        recipe_->identity = node_identity(descriptor_status(
            recipe_->descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            RECIPE_ROOT_LEAF));
        recipe_->expected_mode = SEALED_RECIPE_DIRECTORY_MODE;
    }

    void require_top_level_identity() const {
        if(!active_) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                InvocationOwnedSourceBuildContextFailureReason::InvalidState);
        }
        if(!recipe_.has_value() || !pkgdest_.has_value() ||
           !builddir_.has_value() || !srcdest_.has_value()) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                InvocationOwnedSourceBuildContextFailureReason::InvalidState);
        }
        require_parent_identity(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation);
        require_owned_directory(parent_descriptor_.get(), root_);
        require_owned_directory(root_.descriptor.get(), *recipe_);
        require_owned_directory(root_.descriptor.get(), *pkgdest_);
        require_owned_directory(root_.descriptor.get(), *builddir_);
        require_owned_directory(root_.descriptor.get(), *srcdest_);
        std::vector<std::string> expected{
            std::string(BUILDDIR_ROOT_LEAF),
            std::string(PKGDEST_ROOT_LEAF),
            std::string(RECIPE_ROOT_LEAF),
            std::string(SRCDEST_ROOT_LEAF)};
        std::sort(expected.begin(), expected.end());
        if(directory_names(
               root_.descriptor.get(),
               InvocationOwnedSourceBuildContextStage::SnapshotRevalidation) !=
           expected) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
        }
    }

    void refuse_unproven_cleanup() noexcept {
        cleanup_refusal_ = InvocationOwnedSourceBuildContextFailureReason::UnprovenCleanupContent;
    }

    [[nodiscard]] InvocationOwnedSourceBuildContextCleanupResult cleanup() noexcept {
        if(!active_) return InvocationOwnedSourceBuildContextCleaned{};
        if(cleanup_refusal_.has_value()) {
            return context_failure(InvocationOwnedSourceBuildContextStage::Cleanup,
                                   *cleanup_refusal_);
        }
        cleanup_attempted_ = true;
        try {
            CleanupBudget budget;
            notify_before_cleanup(root_.path);
            require_parent_identity(
                InvocationOwnedSourceBuildContextStage::Cleanup);
            const NodeIdentity current_root = node_identity(named_status(
                parent_descriptor_.get(), root_.leaf,
                InvocationOwnedSourceBuildContextStage::Cleanup));
            const NodeIdentity opened_root = node_identity(descriptor_status(
                root_.descriptor.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup));
            if(!same_node(root_.identity, current_root) ||
               !same_node(root_.identity, opened_root) ||
               opened_root.type != S_IFDIR) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
            }
            if(opened_root.owner !=
                   static_cast<std::uintmax_t>(::geteuid()) ||
               current_root.owner != opened_root.owner) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch);
            }
            if(opened_root.device != parent_identity_.device) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure);
            }
            if(::syscall(
                   SYS_fchmodat2, root_.descriptor.get(), "",
                   PRIVATE_DIRECTORY_MODE, AT_EMPTY_PATH) != 0) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                    {}, errno);
            }
            const NodeIdentity accessible_root = node_identity(
                descriptor_status(
                    root_.descriptor.get(),
                    InvocationOwnedSourceBuildContextStage::Cleanup));
            if(!same_node(root_.identity, accessible_root) ||
               accessible_root.owner !=
                   static_cast<std::uintmax_t>(::geteuid()) ||
               accessible_root.mode != PRIVATE_DIRECTORY_MODE) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure);
            }
            std::vector<std::string> expected_names;
            for(const std::optional<OwnedDirectory>* directory : {
                    &recipe_, &pkgdest_, &builddir_, &srcdest_}) {
                if(!directory->has_value()) continue;
                require_owned_directory_for_cleanup(
                    root_.descriptor.get(), **directory);
                expected_names.push_back((*directory)->leaf);
            }
            std::sort(expected_names.begin(), expected_names.end());
            const std::vector<std::string> names = directory_names(
                root_.descriptor.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup, {}, budget.max_entries);
            if(names != expected_names) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
            }

            std::vector<CleanupNode> plans;
            plans.reserve(names.size());
            for(const std::string& name : names) {
                plans.push_back(plan_cleanup_node(
                    root_.descriptor.get(), name, fs::path(name),
                    root_.identity.device, budget, 1));
            }
            if(names != directory_names(
                            root_.descriptor.get(),
                            InvocationOwnedSourceBuildContextStage::Cleanup, {}, names.size())) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
            }
            for(const CleanupNode& plan : plans) {
                validate_cleanup_plan(
                    root_.descriptor.get(), plan, root_.identity.device);
            }
            require_parent_identity(
                InvocationOwnedSourceBuildContextStage::Cleanup);
            for(const CleanupNode& plan : plans) {
                remove_cleanup_plan(
                    root_.descriptor.get(), plan, root_.identity.device);
            }
            if(!directory_names(
                    root_.descriptor.get(),
                    InvocationOwnedSourceBuildContextStage::Cleanup, {}, 0)
                    .empty()) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
            }
            require_parent_identity(
                InvocationOwnedSourceBuildContextStage::Cleanup);
            const NodeIdentity named_root = node_identity(named_status(
                parent_descriptor_.get(), root_.leaf,
                InvocationOwnedSourceBuildContextStage::Cleanup));
            const NodeIdentity final_root = node_identity(descriptor_status(
                root_.descriptor.get(),
                InvocationOwnedSourceBuildContextStage::Cleanup));
            if(!same_node(root_.identity, named_root) ||
               !same_node(root_.identity, final_root)) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
            }
            if(final_root.owner !=
                   static_cast<std::uintmax_t>(::geteuid()) ||
               named_root.owner != final_root.owner) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch);
            }
            if(final_root.device != parent_identity_.device) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure);
            }
            if(::unlinkat(
                   parent_descriptor_.get(), root_.leaf.c_str(),
                   AT_REMOVEDIR) != 0) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::Cleanup,
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure,
                    {}, errno);
            }
            active_ = false;
            return InvocationOwnedSourceBuildContextCleaned{};
        } catch(ContextFailureError& error) {
            InvocationOwnedSourceBuildContextFailure failure =
                error.release();
            failure.stage = InvocationOwnedSourceBuildContextStage::Cleanup;
            if(failure.reason == InvocationOwnedSourceBuildContextFailureReason::CleanupResourceLimitExceeded) {
                cleanup_refusal_ = failure.reason;
                return failure;
            }
            if(failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch &&
               failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure &&
               failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement &&
               failure.reason !=
                   InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions) {
                failure.reason =
                    InvocationOwnedSourceBuildContextFailureReason::CleanupFailure;
            }
            return failure;
        } catch(const std::exception& error) {
            InvocationOwnedSourceBuildContextFailure failure = context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure);
            failure.diagnostic = error.what();
            return failure;
        } catch(...) {
            return context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::CleanupFailure);
        }
    }

    [[nodiscard]] InvocationOwnedSourceBuildContextFailure abort_construction(
        InvocationOwnedSourceBuildContextFailure primary_failure) noexcept {
        InvocationOwnedSourceBuildContextCleanupResult cleanup_result =
            cleanup();
        if(auto* cleanup_failure =
               std::get_if<InvocationOwnedSourceBuildContextFailure>(
                   &cleanup_result)) {
            try {
                primary_failure.construction_cleanup_failure =
                    InvocationOwnedSourceBuildContextConstructionCleanupFailure{
                        cleanup_failure->stage,
                        cleanup_failure->reason,
                        cleanup_failure->relative_path,
                        root_.path,
                        cleanup_failure->system_error,
                        cleanup_failure->diagnostic};
            } catch(...) {
                if(!primary_failure.diagnostic.has_value()) {
                    try {
                        primary_failure.diagnostic =
                            "Construction failed and abort cleanup also failed.";
                    } catch(...) {
                    }
                }
            }
            // There is no caller-visible owner after factory failure. Do not
            // run an unreported second destructor cleanup attempt after the
            // typed consequence has declared this root potentially retained.
            active_ = false;
        }
        return primary_failure;
    }

private:
    PrivateBuildRoot(
        fs::path parent_path, OwnedDescriptor parent_descriptor,
        NodeIdentity parent_identity, OwnedDirectory root) noexcept
        : parent_path_(std::move(parent_path)),
          parent_descriptor_(std::move(parent_descriptor)),
          parent_identity_(parent_identity), root_(std::move(root)) {
    }

    void require_parent_identity(
        InvocationOwnedSourceBuildContextStage stage) const {
        const NodeIdentity opened = node_identity(descriptor_status(
            parent_descriptor_.get(), stage));
        struct stat named_status_value{};
        if(::lstat(parent_path_.c_str(), &named_status_value) != 0) {
            throw_context_failure(
                stage,
                InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
                {}, errno);
        }
        require_safe_parent_identity(
            parent_identity_, opened, node_identity(named_status_value),
            stage);
    }

    void require_owned_directory(
        int parent_descriptor, const OwnedDirectory& directory) const {
        const NodeIdentity opened = node_identity(descriptor_status(
            directory.descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            directory.leaf));
        const NodeIdentity named = node_identity(named_status(
            parent_descriptor, directory.leaf,
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            directory.leaf));
        require_safe_directory_identity(
            directory.identity, opened, named, root_.identity.device,
            directory.expected_mode,
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            directory.leaf);
        if(directory.path.lexically_normal().parent_path() !=
           (directory.leaf == root_.leaf ? parent_path_ : root_.path)) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
                directory.leaf);
        }
    }

    void require_owned_directory_for_cleanup(
        int parent_descriptor, const OwnedDirectory& directory) const {
        const NodeIdentity opened = node_identity(descriptor_status(
            directory.descriptor.get(),
            InvocationOwnedSourceBuildContextStage::Cleanup,
            directory.leaf));
        const NodeIdentity named = node_identity(named_status(
            parent_descriptor, directory.leaf,
            InvocationOwnedSourceBuildContextStage::Cleanup,
            directory.leaf));
        if(!same_node(directory.identity, opened) ||
           !same_node(opened, named) || opened.type != S_IFDIR) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                directory.leaf);
        }
        if(opened.owner != static_cast<std::uintmax_t>(::geteuid()) ||
           named.owner != opened.owner) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch,
                directory.leaf);
        }
        if(opened.device != root_.identity.device) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::Cleanup,
                InvocationOwnedSourceBuildContextFailureReason::ContainmentFailure,
                directory.leaf);
        }
    }

    fs::path parent_path_;
    OwnedDescriptor parent_descriptor_;
    NodeIdentity parent_identity_;
    OwnedDirectory root_;
    std::optional<OwnedDirectory> recipe_;
    std::optional<OwnedDirectory> pkgdest_;
    std::optional<OwnedDirectory> builddir_;
    std::optional<OwnedDirectory> srcdest_;
    bool active_ = true;
    bool cleanup_attempted_ = false;
    std::optional<InvocationOwnedSourceBuildContextFailureReason> cleanup_refusal_;
};

void materialize_snapshot_directory(
    SnapshotDirectoryPlan& plan, int directory_descriptor,
    std::uintmax_t root_device, const fs::path& relative_path) {
    for(auto& [name, file] : plan.files) {
        const fs::path entry_path = relative_path / name;
        int raw_descriptor;
        do {
            raw_descriptor = ::openat(
                directory_descriptor, name.c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0600);
        } while(raw_descriptor < 0 && errno == EINTR);
        if(raw_descriptor < 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
                entry_path, errno);
        }
        OwnedDescriptor descriptor(raw_descriptor);
        write_all(descriptor.get(), file.bytes, entry_path);
        if(::fsync(descriptor.get()) != 0 ||
           ::fchmod(descriptor.get(), file.mode) != 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
                entry_path, errno);
        }
        const NodeIdentity opened = node_identity(descriptor_status(
            descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            entry_path));
        const NodeIdentity named = node_identity(named_status(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            entry_path));
        if(opened.type != S_IFREG || !same_node(opened, named) ||
           opened.device != root_device ||
           opened.owner != static_cast<std::uintmax_t>(::geteuid()) ||
           opened.mode != static_cast<std::uintmax_t>(file.mode) ||
           opened.links != 1 || opened.size != file.size ||
           read_exact_file(
               descriptor.get(), file.size,
               InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
               entry_path) != file.bytes) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                entry_path);
        }
        file.bytes.clear();
        file.bytes.shrink_to_fit();
    }

    for(auto& [name, child] : plan.directories) {
        const fs::path child_path = relative_path / name;
        if(::mkdirat(
               directory_descriptor, name.c_str(),
               PRIVATE_DIRECTORY_MODE) != 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
                child_path, errno);
        }
        const NodeIdentity created = node_identity(named_status(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path));
        OwnedDescriptor path_descriptor = open_child_directory(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path, O_PATH);
        const NodeIdentity path_opened = node_identity(descriptor_status(
            path_descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path));
        if(!same_node(created, path_opened)) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                child_path);
        }
        set_created_directory_mode(
            path_descriptor.get(), PRIVATE_DIRECTORY_MODE,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
            child_path);
        OwnedDescriptor child_descriptor = open_child_directory(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path);
        const NodeIdentity opened = node_identity(descriptor_status(
            child_descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path));
        const NodeIdentity named = node_identity(named_status(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path));
        require_safe_directory_identity(
            created, opened, named, root_device,
            PRIVATE_DIRECTORY_MODE,
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path);
        materialize_snapshot_directory(
            child, child_descriptor.get(), root_device, child_path);
        if(::fchmod(
               child_descriptor.get(),
               SEALED_RECIPE_DIRECTORY_MODE) != 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
                child_path, errno);
        }
        const NodeIdentity sealed = node_identity(descriptor_status(
            child_descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            child_path));
        if(!same_node(created, sealed) ||
           sealed.mode != SEALED_RECIPE_DIRECTORY_MODE) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                child_path);
        }
    }
}

void validate_snapshot_directory(
    const SnapshotDirectoryPlan& plan, int directory_descriptor,
    std::uintmax_t root_device, const fs::path& relative_path,
    mode_t expected_directory_mode) {
    const NodeIdentity directory_before = node_identity(descriptor_status(
        directory_descriptor,
        InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
        relative_path));
    if(directory_before.type != S_IFDIR ||
       directory_before.device != root_device ||
       directory_before.owner !=
           static_cast<std::uintmax_t>(::geteuid()) ||
       directory_before.mode !=
           static_cast<std::uintmax_t>(expected_directory_mode)) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            directory_before.owner !=
                    static_cast<std::uintmax_t>(::geteuid())
                ? InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch
                : InvocationOwnedSourceBuildContextFailureReason::UnsafePermissions,
            relative_path);
    }

    std::vector<std::string> expected_names;
    expected_names.reserve(plan.files.size() + plan.directories.size());
    for(const auto& [name, file] : plan.files) {
        static_cast<void>(file);
        expected_names.push_back(name);
    }
    for(const auto& [name, child] : plan.directories) {
        static_cast<void>(child);
        expected_names.push_back(name);
    }
    std::sort(expected_names.begin(), expected_names.end());
    if(directory_names(
           directory_descriptor,
           InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
           relative_path) != expected_names) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }

    for(const auto& [name, file] : plan.files) {
        const fs::path entry_path = relative_path / name;
        const NodeIdentity named = node_identity(named_status(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            entry_path));
        int raw_descriptor;
        do {
            raw_descriptor = ::openat(
                directory_descriptor, name.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        } while(raw_descriptor < 0 && errno == EINTR);
        if(raw_descriptor < 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                entry_path, errno);
        }
        OwnedDescriptor descriptor(raw_descriptor);
        const NodeIdentity opened = node_identity(descriptor_status(
            descriptor.get(),
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            entry_path));
        if(opened.type != S_IFREG || !same_node(opened, named) ||
           opened.device != root_device ||
           opened.owner != static_cast<std::uintmax_t>(::geteuid()) ||
           opened.mode != static_cast<std::uintmax_t>(file.mode) ||
           opened.links != 1 || opened.size != file.size) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                opened.owner != static_cast<std::uintmax_t>(::geteuid())
                    ? InvocationOwnedSourceBuildContextFailureReason::OwnershipMismatch
                    : InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                entry_path);
        }
        const std::string contents = read_exact_file(
            descriptor.get(), file.size,
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            entry_path);
        if(reviewed_source_sha256_content_identity(contents) != file.sha256) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
                entry_path);
        }
    }

    for(const auto& [name, child] : plan.directories) {
        const fs::path child_path = relative_path / name;
        OwnedDescriptor child_descriptor = open_child_directory(
            directory_descriptor, name,
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            child_path);
        validate_snapshot_directory(
            child, child_descriptor.get(), root_device, child_path,
            SEALED_RECIPE_DIRECTORY_MODE);
    }
    if(directory_names(
           directory_descriptor,
           InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
           relative_path) != expected_names ||
       !same_node(
           directory_before,
           node_identity(descriptor_status(
               directory_descriptor,
               InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
               relative_path)))) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
            relative_path);
    }
}

fs::path selected_makepkg_path() {
#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
    if(g_makepkg_path_for_test.has_value()) return *g_makepkg_path_for_test;
#endif
    return fs::path(MAKEPKG_EXECUTABLE);
}

CapturedMakepkgExecutable capture_makepkg_executable_identity() {
    const fs::path path = selected_makepkg_path();
    if(!path.is_absolute() || path.root_path() != fs::path("/") ||
       path.native().find('\0') != std::string::npos ||
       has_ambiguous_leading_double_slash(path.native()) ||
       path.lexically_normal() != path) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable);
    }

    OwnedDescriptor current(::open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if(!current.valid()) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable,
            {}, errno);
    }
    const NodeIdentity filesystem_root = node_identity(descriptor_status(
        current.get(),
        InvocationOwnedSourceBuildContextStage::MakepkgExecutable));
    if(filesystem_root.type != S_IFDIR ||
       (filesystem_root.mode & 0022U) != 0 ||
       filesystem_root.owner ==
           static_cast<std::uintmax_t>(::geteuid())) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable);
    }
    std::vector<std::string> components;
    for(const fs::path& component : path.relative_path()) {
        const std::string name = component.string();
        if(name.empty() || name == "." || name == "..") {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
                InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable);
        }
        components.push_back(name);
    }
    if(components.empty()) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable);
    }

    for(std::size_t index = 0; index < components.size(); ++index) {
        const bool is_final = index + 1 == components.size();
        const int flags = is_final
                              ? O_PATH | O_CLOEXEC | O_NOFOLLOW
                              : O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                    O_NOFOLLOW;
        int descriptor;
        do {
            descriptor = ::openat(
                current.get(), components[index].c_str(), flags);
        } while(descriptor < 0 && errno == EINTR);
        if(descriptor < 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
                InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable,
                {}, errno);
        }
        OwnedDescriptor next(descriptor);
        const NodeIdentity opened = node_identity(descriptor_status(
            next.get(),
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable));
        const NodeIdentity named = node_identity(named_status(
            current.get(), components[index],
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable));
        if(!same_node(opened, named) ||
           (!is_final && opened.type != S_IFDIR) ||
           (!is_final && (opened.mode & 0022U) != 0) ||
           (!is_final &&
            opened.owner == static_cast<std::uintmax_t>(::geteuid()))) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
                InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable);
        }
        current = std::move(next);
    }

    const NodeIdentity executable = node_identity(descriptor_status(
        current.get(),
        InvocationOwnedSourceBuildContextStage::MakepkgExecutable));
    if(executable.type != S_IFREG || (executable.mode & 0111U) == 0 ||
       (executable.mode & 0022U) != 0 ||
       executable.owner == static_cast<std::uintmax_t>(::geteuid())) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::MakepkgExecutableUnavailable);
    }
    return CapturedMakepkgExecutable{
        path, std::move(current), executable.device, executable.inode,
        executable.owner, executable.mode};
}

InvocationOwnedSourceBuildContextFailure checkout_projection_failure(
    const TrustedGitPinnedCheckoutFailure& checkout_failure) {
    InvocationOwnedSourceBuildContextFailure failure = context_failure(
        InvocationOwnedSourceBuildContextStage::SnapshotProjection,
        checkout_failure.reason ==
                TrustedGitPinnedCheckoutFailureReason::UnsupportedOverlayEntry
            ? InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry
            : InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
    failure.checkout_failure = checkout_failure;
    return failure;
}

} // namespace

struct InvocationOwnedSourceBuildContext::State {
    ReviewedSourceStateRecordBinding reviewed_binding;
    ReviewedRecipeSnapshotIdentity snapshot_identity;
    InvocationOwnedMakepkgExecutableIdentity makepkg_executable;
    PrivateBuildRoot roots;
    SnapshotPlan snapshot_plan;
    std::shared_ptr<const void> lineage;

    State(
        ReviewedSourceStateRecordBinding value_reviewed_binding,
        ReviewedRecipeSnapshotIdentity value_snapshot_identity,
        InvocationOwnedMakepkgExecutableIdentity value_makepkg_executable,
        PrivateBuildRoot value_roots, SnapshotPlan value_snapshot_plan,
        std::shared_ptr<const void> value_lineage) noexcept
        : reviewed_binding(std::move(value_reviewed_binding)),
          snapshot_identity(std::move(value_snapshot_identity)),
          makepkg_executable(std::move(value_makepkg_executable)),
          roots(std::move(value_roots)),
          snapshot_plan(std::move(value_snapshot_plan)),
          lineage(std::move(value_lineage)) {
    }
};

InvocationOwnedSourceBuildContextResult
InvocationOwnedSourceBuildContextAuthority::create(
    PinnedReviewedSourceBuild pinned_build) {
    if(!pinned_build.valid()) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::PinnedBuildValidation,
            InvocationOwnedSourceBuildContextFailureReason::InvalidPinnedBuild);
    }

    ReviewedSourceStateRecordBindingResult binding_result =
        derive_reviewed_source_state_record_binding(pinned_build);
    if(auto* binding_failure =
           std::get_if<ReviewedSourceStateRecordBindingFailure>(
               &binding_result)) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::ReviewedBinding,
            binding_failure->reason ==
                    ReviewedSourceStateRecordBindingFailureReason::InvalidPinnedBuild
                ? InvocationOwnedSourceBuildContextFailureReason::InvalidPinnedBuild
                : (binding_failure->reason ==
                           ReviewedSourceStateRecordBindingFailureReason::EditorOverlayPresent
                       ? InvocationOwnedSourceBuildContextFailureReason::EditorOverlayPresent
                       : InvocationOwnedSourceBuildContextFailureReason::ReviewedBindingFailure));
        failure.binding_failure = *binding_failure;
        return failure;
    }
    ReviewedSourceStateRecordBinding binding =
        std::get<ReviewedSourceStateRecordBinding>(binding_result);

    TrustedGitReviewedRecipeSnapshotResult projected =
        pinned_build.project_reviewed_recipe_snapshot();
    if(auto* git_failure =
           std::get_if<TrustedGitReviewFailure>(&projected)) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotProjection,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
        failure.git_failure = *git_failure;
        return failure;
    }
    if(auto* review_failure =
           std::get_if<ReviewedSourceReviewFailure>(&projected)) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotProjection,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
        failure.review_failure = *review_failure;
        return failure;
    }
    if(auto* checkout_failure =
           std::get_if<TrustedGitPinnedCheckoutFailure>(&projected)) {
        return checkout_projection_failure(*checkout_failure);
    }
    TrustedGitReviewedRecipeSnapshot snapshot =
        std::get<TrustedGitReviewedRecipeSnapshot>(
            std::move(projected));
    if(snapshot.identity().package_base() != binding.package_base() ||
       snapshot.identity().target_revision() !=
           binding.reviewed_recipe_revision().value()) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotProjection,
            InvocationOwnedSourceBuildContextFailureReason::ReviewedBindingFailure);
    }

    const auto make_snapshot_plan = [](const auto& trusted_snapshot) {
        SnapshotPlan plan;
        bool has_pkgbuild = false;
        bool has_srcinfo = false;
        for(const TrustedGitReviewedRecipeSnapshot::Entry& entry :
            trusted_snapshot.entries_) {
            const std::string& raw_path =
                entry.version.path().raw_bytes();
            const auto components = snapshot_path_components(raw_path);
            if(!components.has_value()) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::SnapshotProjection,
                    InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
                    fs::path(raw_path));
            }
            mode_t mode = SEALED_RECIPE_FILE_MODE;
            switch(entry.version.mode()) {
                case ReviewedSourceFileMode::Regular:
                    break;
                case ReviewedSourceFileMode::Executable:
                    mode = SEALED_RECIPE_EXECUTABLE_MODE;
                    break;
                case ReviewedSourceFileMode::SymbolicLink:
                case ReviewedSourceFileMode::Gitlink:
                    throw_context_failure(
                        InvocationOwnedSourceBuildContextStage::SnapshotProjection,
                        InvocationOwnedSourceBuildContextFailureReason::UnsafeSourceEntry,
                        fs::path(raw_path));
            }
            if(!entry.version.blob_size().has_value() ||
               *entry.version.blob_size() != entry.bytes.size()) {
                throw_context_failure(
                    InvocationOwnedSourceBuildContextStage::SnapshotProjection,
                    InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure,
                    fs::path(raw_path));
            }
            if(raw_path == "PKGBUILD") has_pkgbuild = true;
            if(raw_path == ".SRCINFO") has_srcinfo = true;
            SnapshotFilePlan file{
                entry.bytes,
                reviewed_source_sha256_content_identity(entry.bytes),
                static_cast<std::uintmax_t>(entry.bytes.size()), mode};
            insert_snapshot_file(
                plan, *components, std::move(file), fs::path(raw_path));
        }
        if(!has_pkgbuild || !has_srcinfo ||
           plan.tracked_entry_count == 0) {
            throw_context_failure(
                InvocationOwnedSourceBuildContextStage::SnapshotProjection,
                InvocationOwnedSourceBuildContextFailureReason::UnsupportedRecipeShape);
        }
        return plan;
    };

    SnapshotPlan snapshot_plan = make_snapshot_plan(snapshot);
    CapturedMakepkgExecutable captured_makepkg =
        capture_makepkg_executable_identity();
    InvocationOwnedMakepkgExecutableIdentity makepkg(
        std::move(captured_makepkg.path),
        captured_makepkg.descriptor.release(), captured_makepkg.device,
        captured_makepkg.inode, captured_makepkg.owner,
        captured_makepkg.mode);
    notify_before_private_root_creation();
    PrivateBuildRoot roots = PrivateBuildRoot::create();
    try {
        roots.require_top_level_identity();
        materialize_snapshot_directory(
            snapshot_plan.root, roots.recipe_descriptor(), roots.device(), {});
        roots.seal_recipe_root();
        roots.require_top_level_identity();
        validate_snapshot_directory(
            snapshot_plan.root, roots.recipe_descriptor(), roots.device(), {},
            SEALED_RECIPE_DIRECTORY_MODE);

        notify_before_final_reviewed_source_reproof(roots.root_path());
        TrustedGitPinnedCheckoutRevalidationResult final_source =
            pinned_build.revalidate_reviewed_recipe_source();
        if(auto* checkout_failure =
               std::get_if<TrustedGitPinnedCheckoutFailure>(&final_source)) {
            return roots.abort_construction(
                checkout_projection_failure(*checkout_failure));
        }
        roots.require_top_level_identity();
        validate_snapshot_directory(
            snapshot_plan.root, roots.recipe_descriptor(), roots.device(), {},
            SEALED_RECIPE_DIRECTORY_MODE);

        ReviewedRecipeSnapshotIdentity snapshot_identity(
            binding, snapshot.git_tree_object_id(), snapshot.entry_count());
        std::shared_ptr<const void> lineage =
            std::make_shared<const std::uint8_t>(0);
        return InvocationOwnedSourceBuildContext(
            std::make_unique<InvocationOwnedSourceBuildContext::State>(
                std::move(binding), std::move(snapshot_identity),
                std::move(makepkg), std::move(roots),
                std::move(snapshot_plan), std::move(lineage)));
    } catch(ContextFailureError& error) {
        return roots.abort_construction(error.release());
    } catch(const std::exception& error) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
        failure.diagnostic = error.what();
        return roots.abort_construction(std::move(failure));
    } catch(...) {
        return roots.abort_construction(context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotMaterialization,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure));
    }
}

ReviewedRecipeSnapshotIdentity::ReviewedRecipeSnapshotIdentity(
    ReviewedSourceStateRecordBinding reviewed_binding,
    ReviewedSourceObjectId git_tree_object_id,
    std::size_t tracked_entry_count) noexcept
    : reviewed_binding_(std::move(reviewed_binding)),
      git_tree_object_id_(std::move(git_tree_object_id)),
      tracked_entry_count_(tracked_entry_count) {
}

const ReviewedSourceStateRecordBinding&
ReviewedRecipeSnapshotIdentity::reviewed_binding() const noexcept {
    return reviewed_binding_;
}

const ReviewedSourceObjectId&
ReviewedRecipeSnapshotIdentity::git_tree_object_id() const noexcept {
    return git_tree_object_id_;
}

std::size_t ReviewedRecipeSnapshotIdentity::tracked_entry_count()
    const noexcept {
    return tracked_entry_count_;
}

InvocationOwnedMakepkgExecutableIdentity::
    InvocationOwnedMakepkgExecutableIdentity(
        fs::path path, int descriptor, std::uintmax_t device,
        std::uintmax_t inode, std::uintmax_t owner,
        std::uintmax_t mode) noexcept
    : path_(std::move(path)), descriptor_(descriptor), device_(device),
      inode_(inode), owner_(owner), mode_(mode) {
}

InvocationOwnedMakepkgExecutableIdentity::
    InvocationOwnedMakepkgExecutableIdentity(
        InvocationOwnedMakepkgExecutableIdentity&& other) noexcept
    : path_(std::move(other.path_)),
      descriptor_(std::exchange(other.descriptor_, -1)),
      device_(other.device_), inode_(other.inode_), owner_(other.owner_),
      mode_(other.mode_) {
}

InvocationOwnedMakepkgExecutableIdentity::~InvocationOwnedMakepkgExecutableIdentity() noexcept {
    if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
}

const fs::path& InvocationOwnedMakepkgExecutableIdentity::path()
    const noexcept {
    return path_;
}

std::uintmax_t InvocationOwnedMakepkgExecutableIdentity::device()
    const noexcept {
    return device_;
}

std::uintmax_t InvocationOwnedMakepkgExecutableIdentity::inode()
    const noexcept {
    return inode_;
}

std::uintmax_t InvocationOwnedMakepkgExecutableIdentity::owner()
    const noexcept {
    return owner_;
}

std::uintmax_t InvocationOwnedMakepkgExecutableIdentity::mode()
    const noexcept {
    return mode_;
}

void InvocationOwnedMakepkgExecutableIdentity::require_unchanged() const {
    if(descriptor_ < 0) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::InvalidState);
    }
    const NodeIdentity retained = node_identity(descriptor_status(
        descriptor_,
        InvocationOwnedSourceBuildContextStage::MakepkgExecutable));
    if(retained.device != device_ || retained.inode != inode_ ||
       retained.owner != owner_ || retained.mode != mode_ ||
       retained.type != S_IFREG) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
    }
    CapturedMakepkgExecutable current =
        capture_makepkg_executable_identity();
    if(current.path != path_ || current.device != device_ ||
       current.inode != inode_ || current.owner != owner_ ||
       current.mode != mode_) {
        throw_context_failure(
            InvocationOwnedSourceBuildContextStage::MakepkgExecutable,
            InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement);
    }
}

InvocationOwnedMakepkgEnvironment::InvocationOwnedMakepkgEnvironment(
    SourceBuildEnvironment source_environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy,
    std::shared_ptr<const void> lineage) noexcept
    : source_environment_(std::move(source_environment)),
      empty_value_policy_(empty_value_policy),
      lineage_(std::move(lineage)) {
}

const SourceBuildEnvironment&
InvocationOwnedMakepkgEnvironment::source_environment() const noexcept {
    return source_environment_;
}

SourceEnvironmentEmptyValuePolicy
InvocationOwnedMakepkgEnvironment::empty_value_policy() const noexcept {
    return empty_value_policy_;
}

InvocationOwnedSourceBuildContext::InvocationOwnedSourceBuildContext(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {
}

InvocationOwnedSourceBuildContext::InvocationOwnedSourceBuildContext(
    InvocationOwnedSourceBuildContext&& other) noexcept = default;

InvocationOwnedSourceBuildContext::~InvocationOwnedSourceBuildContext() noexcept = default;

bool InvocationOwnedSourceBuildContext::valid() const noexcept {
    return state_ != nullptr && state_->roots.active();
}

const InvocationOwnedSourceBuildContext::State&
InvocationOwnedSourceBuildContext::require_state() const {
    if(!valid()) {
        throw std::logic_error(
            "Invocation-owned source-build context is inactive.");
    }
    return *state_;
}

InvocationOwnedSourceBuildContext::State&
InvocationOwnedSourceBuildContext::require_state() {
    if(!valid()) {
        throw std::logic_error(
            "Invocation-owned source-build context is inactive.");
    }
    return *state_;
}

const PackageBaseIdentity&
InvocationOwnedSourceBuildContext::package_base() const {
    return require_state().reviewed_binding.package_base();
}

const ReviewedSourceStateRecordBinding&
InvocationOwnedSourceBuildContext::reviewed_binding() const {
    return require_state().reviewed_binding;
}

const ReviewedRecipeSnapshotIdentity&
InvocationOwnedSourceBuildContext::snapshot_identity() const {
    return require_state().snapshot_identity;
}

const fs::path& InvocationOwnedSourceBuildContext::recipe_root() const {
    return require_state().roots.recipe_path();
}

const fs::path& InvocationOwnedSourceBuildContext::pkgdest() const {
    return require_state().roots.pkgdest_path();
}

const fs::path& InvocationOwnedSourceBuildContext::builddir() const {
    return require_state().roots.builddir_path();
}

const fs::path& InvocationOwnedSourceBuildContext::srcdest() const {
    return require_state().roots.srcdest_path();
}

const fs::path& InvocationOwnedSourceBuildContext::owned_root() const {
    return require_state().roots.root_path();
}

const InvocationOwnedMakepkgExecutableIdentity&
InvocationOwnedSourceBuildContext::makepkg_executable() const {
    return require_state().makepkg_executable;
}

int InvocationOwnedSourceBuildContext::recipe_descriptor() const {
    return require_state().roots.recipe_descriptor();
}

int InvocationOwnedSourceBuildContext::pkgdest_descriptor() const {
    return require_state().roots.pkgdest_descriptor();
}

int InvocationOwnedSourceBuildContext::builddir_descriptor() const {
    return require_state().roots.builddir_descriptor();
}

int InvocationOwnedSourceBuildContext::srcdest_descriptor() const {
    return require_state().roots.srcdest_descriptor();
}

std::uintmax_t InvocationOwnedSourceBuildContext::root_device() const {
    return require_state().roots.device();
}

InvocationOwnedSourceBuildContextValidationResult
InvocationOwnedSourceBuildContext::revalidate() const {
    if(!valid()) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            InvocationOwnedSourceBuildContextFailureReason::InvalidState);
    }
    try {
        state_->roots.require_top_level_identity();
        validate_snapshot_directory(
            state_->snapshot_plan.root,
            state_->roots.recipe_descriptor(), state_->roots.device(), {},
            SEALED_RECIPE_DIRECTORY_MODE);
        state_->makepkg_executable.require_unchanged();
        return InvocationOwnedSourceBuildContextValidated{};
    } catch(ContextFailureError& error) {
        return error.release();
    } catch(const std::exception& error) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
        failure.diagnostic = error.what();
        return failure;
    } catch(...) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotRevalidation,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
    }
}

InvocationOwnedMakepkgEnvironmentResult
InvocationOwnedSourceBuildContext::make_makepkg_environment(
    const SourceBuildEnvironment& customization,
    SourceEnvironmentEmptyValuePolicy empty_value_policy) const {
    InvocationOwnedSourceBuildContextValidationResult validation =
        revalidate();
    if(auto* failure =
           std::get_if<InvocationOwnedSourceBuildContextFailure>(
               &validation)) {
        failure->stage =
            InvocationOwnedSourceBuildContextStage::EnvironmentProjection;
        return std::move(*failure);
    }
    try {
        SourceBuildEnvironment environment = customization;
        for(const SourceEnvironmentAssignment& assignment :
            environment.ordered_assignments) {
            if(!is_valid_environment_key(assignment.key) ||
               assignment.value.find('\0') != std::string::npos) {
                return context_failure(
                    InvocationOwnedSourceBuildContextStage::EnvironmentProjection,
                    InvocationOwnedSourceBuildContextFailureReason::InvalidEnvironmentAssignment);
            }
            if(is_authority_owned_environment_key(assignment.key)) {
                return context_failure(
                    InvocationOwnedSourceBuildContextStage::EnvironmentProjection,
                    InvocationOwnedSourceBuildContextFailureReason::EnvironmentAssignmentConflict);
            }
        }
        const State& state = require_state();
        environment.ordered_assignments.push_back(
            SourceEnvironmentAssignment{
                "PKGDEST", state.roots.pkgdest_path().string()});
        environment.ordered_assignments.push_back(
            SourceEnvironmentAssignment{
                "BUILDDIR", state.roots.builddir_path().string()});
        environment.ordered_assignments.push_back(
            SourceEnvironmentAssignment{
                "SRCDEST", state.roots.srcdest_path().string()});
        return InvocationOwnedMakepkgEnvironment(
            std::move(environment), empty_value_policy, state.lineage);
    } catch(const std::exception& error) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::EnvironmentProjection,
            InvocationOwnedSourceBuildContextFailureReason::InvalidState);
        failure.diagnostic = error.what();
        return failure;
    } catch(...) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::EnvironmentProjection,
            InvocationOwnedSourceBuildContextFailureReason::InvalidState);
    }
}

bool InvocationOwnedSourceBuildContext::owns_makepkg_environment(
    const InvocationOwnedMakepkgEnvironment& environment) const noexcept {
    return valid() && state_->lineage != nullptr &&
           state_->lineage.get() == environment.lineage_.get();
}

void InvocationOwnedSourceBuildContext::refuse_unproven_cleanup() noexcept {
    if(state_ != nullptr) state_->roots.refuse_unproven_cleanup();
}

InvocationOwnedSourceBuildContextCleanupResult
InvocationOwnedSourceBuildContext::cleanup() noexcept {
    if(state_ == nullptr) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::Cleanup,
            InvocationOwnedSourceBuildContextFailureReason::InvalidState);
    }
    return state_->roots.cleanup();
}

InvocationOwnedSourceBuildContextResult
create_invocation_owned_source_build_context(
    PinnedReviewedSourceBuild pinned_build) {
    try {
        return InvocationOwnedSourceBuildContextAuthority::create(
            std::move(pinned_build));
    } catch(ContextFailureError& error) {
        return error.release();
    } catch(const std::exception& error) {
        InvocationOwnedSourceBuildContextFailure failure = context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotProjection,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
        failure.diagnostic = error.what();
        return failure;
    } catch(...) {
        return context_failure(
            InvocationOwnedSourceBuildContextStage::SnapshotProjection,
            InvocationOwnedSourceBuildContextFailureReason::SnapshotFailure);
    }
}

#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
void set_invocation_owned_source_build_context_cleanup_limits_for_test(
    std::optional<std::pair<std::size_t, std::size_t>> limits) {
    if(limits.has_value() && (limits->first > MAX_CLEANUP_ENTRIES ||
                              limits->second > MAX_CLEANUP_DEPTH)) {
        throw std::invalid_argument("Cleanup test limits may only reduce production bounds.");
    }
    g_cleanup_limits_for_test = limits;
}

void set_invocation_owned_source_build_context_test_hook(
    InvocationOwnedSourceBuildContextTestHook hook) {
    g_context_test_hook = std::move(hook);
}

void set_invocation_owned_source_build_context_makepkg_path_for_test(
    std::optional<fs::path> makepkg_path) {
    g_makepkg_path_for_test = std::move(makepkg_path);
}

void set_invocation_owned_source_build_context_parent_path_for_test(
    std::optional<fs::path> parent_path) {
    g_context_root_parent_for_test = std::move(parent_path);
}

std::optional<InvocationOwnedSourceBuildContextFailureReason>
invocation_owned_source_build_context_parent_policy_failure_for_test(
    std::uintmax_t owner, std::uintmax_t mode,
    std::uintmax_t effective_user) {
    return parent_policy_failure(owner, mode, effective_user);
}

bool invocation_owned_source_build_context_snapshot_path_is_safe_for_test(
    const std::string& raw_path) {
    return snapshot_path_components(raw_path).has_value();
}
#endif
