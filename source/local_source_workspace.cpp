#include "local_source_workspace.hpp"

#include "localization.hpp"
#include "logging.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// Narrow bridge used only by the source-workspace cleanup implementation.
// Every mutation revalidates the retained workspace as the same named child
// of the trusted cache root before rebuilding its relative lineage.
struct LocalSourceWorkspaceCleanupAccess final {
    static void prepare(
        RetainedTrustedCacheDirectory& directory) {
        directory.prepare_for_owned_cleanup();
    }

    static int descriptor(
        const RetainedTrustedCacheDirectory& directory) noexcept {
        return directory.descriptor_;
    }
};

namespace {

namespace fs = std::filesystem;

constexpr std::string_view WORKSPACE_PREFIX =
    ".local-source-workspace~-";
constexpr std::size_t RANDOM_NAME_BYTES = 16;
constexpr std::size_t MAX_NAME_ATTEMPTS = 16;
constexpr std::size_t COPY_BUFFER_SIZE = 64 * 1024;
constexpr std::size_t MAX_SYMLINK_TARGET_SIZE = 1024 * 1024;

class OwnedDescriptor final {
    int descriptor_ = -1;

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
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ~OwnedDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
    }

    int get() const noexcept {
        return descriptor_;
    }
};

struct DirectoryStreamCloser {
    void operator()(DIR* stream) const noexcept {
        if(stream != nullptr) static_cast<void>(::closedir(stream));
    }
};

using OwnedDirectoryStream = std::unique_ptr<DIR, DirectoryStreamCloser>;

enum class ObservedNodeType {
    Directory,
    RegularFile,
    Symlink,
    Unsupported,
};

struct ObservedNodeIdentity {
    ObservedNodeType type = ObservedNodeType::Unsupported;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
    std::uintmax_t link_count = 0;
    std::intmax_t size = 0;
    std::intmax_t modification_time_seconds = 0;
    std::intmax_t modification_time_nanoseconds = 0;
    std::intmax_t status_change_time_seconds = 0;
    std::intmax_t status_change_time_nanoseconds = 0;

    bool operator==(const ObservedNodeIdentity&) const = default;
};

struct SnapshotManifestEntry {
    ObservedNodeIdentity source_identity;
    ObservedNodeIdentity destination_identity;
    std::optional<std::string> symlink_target;
};

using SnapshotManifest = std::map<std::string, SnapshotManifestEntry>;

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
LocalSourceWorkspaceTestHook g_workspace_test_hook;
LocalSourceWorkspaceCleanupMetadataMatchForTest
    g_cleanup_metadata_match_for_test;

void notify_workspace_test_hook(
    LocalSourceWorkspaceTestEvent event,
    const fs::path& relative_path) {
    if(g_workspace_test_hook) g_workspace_test_hook(event, relative_path);
}
#else
void notify_after_file_data_copied(const fs::path&) {
}
void notify_before_directory_revalidation(const fs::path&) {
}
void notify_before_cleanup_removal() {
}
#endif

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
void notify_after_file_data_copied(const fs::path& relative_path) {
    notify_workspace_test_hook(
        LocalSourceWorkspaceTestEvent::AfterFileDataCopied,
        relative_path);
}

void notify_before_directory_revalidation(
    const fs::path& relative_path) {
    notify_workspace_test_hook(
        LocalSourceWorkspaceTestEvent::BeforeDirectoryRevalidation,
        relative_path);
}


void notify_before_cleanup_removal() {
    notify_workspace_test_hook(
        LocalSourceWorkspaceTestEvent::BeforeCleanupRemoval, {});
}
#endif

std::error_code workspace_system_error(int error_number) {
    return std::error_code(error_number, std::generic_category());
}

[[noreturn]] void throw_workspace_failure(
    LocalSourceWorkspaceStage stage,
    LocalSourceWorkspaceErrorCode code,
    const fs::path& relative_path = {},
    std::optional<int> error_number = std::nullopt) {
    std::optional<std::error_code> system_error;
    if(error_number.has_value()) {
        system_error = workspace_system_error(*error_number);
    }
    throw LocalSourceWorkspaceError(LocalSourceWorkspaceFailure{
        stage, code, relative_path, system_error});
}

LocalSourceWorkspaceErrorCode system_error_code(
    int error_number, LocalSourceWorkspaceErrorCode fallback) {
    if(error_number == EACCES || error_number == EPERM) {
        return LocalSourceWorkspaceErrorCode::PermissionDenied;
    }
    if(error_number == EXDEV) {
        return LocalSourceWorkspaceErrorCode::FilesystemBoundary;
    }
    return fallback;
}

[[noreturn]] void throw_workspace_system_failure(
    LocalSourceWorkspaceStage stage,
    LocalSourceWorkspaceErrorCode fallback,
    const fs::path& relative_path,
    int error_number) {
    throw_workspace_failure(
        stage, system_error_code(error_number, fallback), relative_path,
        error_number);
}

ObservedNodeType observed_node_type(mode_t mode) noexcept {
    if(S_ISDIR(mode)) return ObservedNodeType::Directory;
    if(S_ISREG(mode)) return ObservedNodeType::RegularFile;
    if(S_ISLNK(mode)) return ObservedNodeType::Symlink;
    return ObservedNodeType::Unsupported;
}

ObservedNodeIdentity observe_node_identity(const struct stat& status) noexcept {
    return ObservedNodeIdentity{
        observed_node_type(status.st_mode),
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_uid),
        static_cast<std::uintmax_t>(status.st_mode & 07777),
        static_cast<std::uintmax_t>(status.st_nlink),
        static_cast<std::intmax_t>(status.st_size),
        static_cast<std::intmax_t>(status.st_mtim.tv_sec),
        static_cast<std::intmax_t>(status.st_mtim.tv_nsec),
        static_cast<std::intmax_t>(status.st_ctim.tv_sec),
        static_cast<std::intmax_t>(status.st_ctim.tv_nsec)};
}

bool is_ascii_control(unsigned char character) noexcept {
    return character <= 0x1f || character == 0x7f;
}

void require_safe_entry_name(
    const std::string& name, const fs::path& relative_path) {
    if(name.empty() || name == "." || name == ".." ||
       name.find('/') != std::string::npos ||
       std::any_of(name.begin(), name.end(), [](unsigned char character) {
           return is_ascii_control(character);
       })) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceEnumeration,
            LocalSourceWorkspaceErrorCode::UnsafeName, relative_path);
    }
}

void record_manifest_entry(
    SnapshotManifest& manifest, const fs::path& relative_path,
    const ObservedNodeIdentity& source_identity,
    const ObservedNodeIdentity& destination_identity,
    std::optional<std::string> symlink_target = std::nullopt) {
    const std::string key = relative_path.generic_string();
    if(key.empty() ||
       !manifest.emplace(
                    key,
                    SnapshotManifestEntry{
                        source_identity,
                        destination_identity,
                        std::move(symlink_target)})
            .second) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
}

void require_admissible_source_node(
    const ObservedNodeIdentity& identity,
    ObservedNodeType expected_type,
    std::uintmax_t expected_device,
    std::uintmax_t expected_owner,
    const fs::path& relative_path) {
    if(identity.type != expected_type) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            identity.type == ObservedNodeType::Unsupported
                ? LocalSourceWorkspaceErrorCode::UnsupportedFileType
                : LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    if(identity.device != expected_device) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            LocalSourceWorkspaceErrorCode::FilesystemBoundary,
            relative_path);
    }
    if(identity.owner != expected_owner) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            LocalSourceWorkspaceErrorCode::OwnershipMismatch,
            relative_path);
    }
    if(expected_type != ObservedNodeType::Symlink &&
       (identity.mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            LocalSourceWorkspaceErrorCode::UnsafePermissions,
            relative_path);
    }
}

void require_unchanged_node(
    const ObservedNodeIdentity& expected,
    const struct stat& observed,
    const fs::path& relative_path) {
    const ObservedNodeIdentity current = observe_node_identity(observed);
    if(expected.type != current.type || expected.device != current.device ||
       expected.inode != current.inode) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    if(expected != current) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ContentChanged,
            relative_path);
    }
}

struct stat inspect_named_source_node(
    int parent_descriptor, const std::string& name,
    const fs::path& relative_path) {
    struct stat status{};
    if(::fstatat(
           parent_descriptor, name.c_str(), &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, status_error);
    }
    return status;
}

OwnedDescriptor open_source_node(
    int parent_descriptor, const std::string& name, bool directory,
    const fs::path& relative_path) {
    struct open_how how{};
    how.flags = static_cast<std::uint64_t>(
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
        (directory ? O_DIRECTORY : O_NONBLOCK));
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(::syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::SourceOpen,
            open_error == ELOOP
                ? LocalSourceWorkspaceErrorCode::ConcurrentMutation
                : LocalSourceWorkspaceErrorCode::MetadataFailure,
            relative_path, open_error);
    }
    return OwnedDescriptor(descriptor);
}

OwnedDescriptor open_destination_directory(
    int parent_descriptor, const std::string& name,
    const fs::path& relative_path) {
    struct open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(::syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::DestinationCreation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, open_error);
    }
    return OwnedDescriptor(descriptor);
}

OwnedDescriptor open_destination_file(
    int parent_descriptor, const std::string& name,
    const fs::path& relative_path) {
    struct open_how how{};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(::syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, open_error);
    }
    return OwnedDescriptor(descriptor);
}

struct stat descriptor_status(
    int descriptor, LocalSourceWorkspaceStage stage,
    const fs::path& relative_path) {
    struct stat status{};
    if(::fstat(descriptor, &status) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            stage, LocalSourceWorkspaceErrorCode::MetadataFailure,
            relative_path, status_error);
    }
    return status;
}

std::vector<std::string> enumerate_directory(
    int directory_descriptor, const fs::path& relative_path) {
    const int scan_descriptor = ::openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::SourceEnumeration,
            LocalSourceWorkspaceErrorCode::MetadataFailure,
            relative_path, open_error);
    }

    DIR* raw_stream = ::fdopendir(scan_descriptor);
    if(raw_stream == nullptr) {
        const int stream_error = errno;
        static_cast<void>(::close(scan_descriptor));
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::SourceEnumeration,
            LocalSourceWorkspaceErrorCode::MetadataFailure,
            relative_path, stream_error);
    }
    OwnedDirectoryStream stream(raw_stream);

    std::vector<std::string> names;
    errno = 0;
    while(dirent* entry = ::readdir(stream.get())) {
        const std::string name(entry->d_name);
        if(name == "." || name == "..") continue;
        const fs::path entry_path = relative_path / name;
        require_safe_entry_name(name, entry_path);
        names.push_back(name);
        errno = 0;
    }
    if(errno != 0) {
        const int read_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::SourceEnumeration,
            LocalSourceWorkspaceErrorCode::ReadFailure,
            relative_path, read_error);
    }
    std::sort(names.begin(), names.end());
    if(std::adjacent_find(names.begin(), names.end()) != names.end()) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceEnumeration,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    return names;
}

std::vector<std::string> enumerate_cleanup_directory(
    int directory_descriptor, const fs::path& relative_path) {
    const int scan_descriptor = ::openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, open_error);
    }

    DIR* raw_stream = ::fdopendir(scan_descriptor);
    if(raw_stream == nullptr) {
        const int stream_error = errno;
        static_cast<void>(::close(scan_descriptor));
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, stream_error);
    }
    OwnedDirectoryStream stream(raw_stream);

    std::vector<std::string> names;
    errno = 0;
    while(dirent* entry = ::readdir(stream.get())) {
        const std::string name(entry->d_name);
        if(name != "." && name != "..") names.push_back(name);
        errno = 0;
    }
    if(errno != 0) {
        const int read_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, read_error);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Unlike dev/inode and timestamps, the filesystem's opaque FID distinguishes
// an inode-number reuse and is stable across cleanup-owned fchmod/unlink
// metadata changes. It replaces the generation evidence formerly supplied by
// retaining one O_PATH descriptor per descendant.
struct CleanupFileHandleIdentity {
    int mount_id = 0;
    int handle_type = 0;
    std::vector<unsigned char> bytes;

    bool operator==(const CleanupFileHandleIdentity&) const = default;
};

struct OwnedCleanupNode {
    std::string name;
    struct stat status{};
    CleanupFileHandleIdentity file_handle;
    std::vector<OwnedCleanupNode> children;
};

struct OpenedOwnedCleanupNode {
    OwnedDescriptor descriptor;
    struct stat status{};
    CleanupFileHandleIdentity file_handle;
};

CleanupFileHandleIdentity observe_cleanup_file_handle(
    int descriptor, const fs::path& relative_path) {
    struct file_handle required{};
    int mount_id = 0;
    errno = 0;
    const int size_probe = ::name_to_handle_at(
        descriptor, "", &required, &mount_id,
        AT_EMPTY_PATH | AT_HANDLE_FID);
    if(size_probe == 0 || errno != EOVERFLOW ||
       required.handle_bytes == 0) {
        const int handle_error = errno;
        if(handle_error != 0) {
            throw_workspace_system_failure(
                LocalSourceWorkspaceStage::Cleanup,
                LocalSourceWorkspaceErrorCode::CleanupFailure,
                relative_path, handle_error);
        }
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path);
    }

    const std::size_t storage_bytes =
        sizeof(struct file_handle) + required.handle_bytes;
    std::vector<std::max_align_t> storage(
        (storage_bytes + sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t));
    // name_to_handle_at() owns this variable-sized C API boundary. The
    // resulting bytes are copied into a typed value before storage expires.
    auto* handle = reinterpret_cast<struct file_handle*>(storage.data());
    handle->handle_bytes = required.handle_bytes;
    if(::name_to_handle_at(
           descriptor, "", handle, &mount_id,
           AT_EMPTY_PATH | AT_HANDLE_FID) != 0) {
        const int handle_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, handle_error);
    }
    return CleanupFileHandleIdentity{
        mount_id, handle->handle_type,
        std::vector<unsigned char>(
            handle->f_handle,
            handle->f_handle + handle->handle_bytes)};
}

bool same_cleanup_identity_and_type(
    const struct stat& left, const struct stat& right) noexcept {
    return (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT) &&
           left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool cleanup_metadata_matches_plan(
    const struct stat& observed, const struct stat& expected,
    const fs::path& relative_path) {
    if(same_cleanup_identity_and_type(observed, expected)) return true;
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
    return g_cleanup_metadata_match_for_test &&
           g_cleanup_metadata_match_for_test(relative_path);
#else
    static_cast<void>(relative_path);
    return false;
#endif
}

void require_owned_cleanup_identity(
    const struct stat& named, const struct stat& opened,
    const CleanupFileHandleIdentity& opened_file_handle,
    const OwnedCleanupNode* expected,
    std::uintmax_t expected_device, std::uintmax_t expected_owner,
    const fs::path& relative_path) {
    if(!same_cleanup_identity_and_type(named, opened) ||
       (expected != nullptr &&
        (!cleanup_metadata_matches_plan(
             opened, expected->status, relative_path) ||
         opened_file_handle != expected->file_handle)) ||
       static_cast<std::uintmax_t>(opened.st_dev) != expected_device ||
       static_cast<std::uintmax_t>(opened.st_uid) != expected_owner ||
       static_cast<std::uintmax_t>(named.st_uid) != expected_owner) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path);
    }
}

OpenedOwnedCleanupNode open_owned_cleanup_node(
    int parent_descriptor, const std::string& name,
    const OwnedCleanupNode* expected,
    std::uintmax_t expected_device, std::uintmax_t expected_owner,
    const fs::path& relative_path) {
    struct stat named{};
    if(::fstatat(
           parent_descriptor, name.c_str(), &named,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, status_error);
    }

    struct open_how how{};
    how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(::syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, open_error);
    }
    OwnedDescriptor retained(descriptor);
    const struct stat opened = descriptor_status(
        retained.get(), LocalSourceWorkspaceStage::Cleanup,
        relative_path);
    CleanupFileHandleIdentity opened_file_handle =
        observe_cleanup_file_handle(retained.get(), relative_path);
    require_owned_cleanup_identity(
        named, opened, opened_file_handle, expected, expected_device,
        expected_owner, relative_path);
    return OpenedOwnedCleanupNode{
        std::move(retained), opened, std::move(opened_file_handle)};
}

void require_owned_cleanup_node_unchanged(
    int parent_descriptor, const std::string& name,
    const OwnedCleanupNode& expected,
    int opened_descriptor,
    std::uintmax_t expected_device, std::uintmax_t expected_owner,
    const fs::path& relative_path) {
    const struct stat opened = descriptor_status(
        opened_descriptor, LocalSourceWorkspaceStage::Cleanup,
        relative_path);
    struct stat named{};
    if(::fstatat(
           parent_descriptor, name.c_str(), &named,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, status_error);
    }
    require_owned_cleanup_identity(
        named, opened,
        observe_cleanup_file_handle(opened_descriptor, relative_path),
        &expected, expected_device, expected_owner, relative_path);
}

OwnedDescriptor open_owned_cleanup_directory(
    int parent_descriptor, const std::string& name,
    const OwnedCleanupNode& expected,
    std::uintmax_t expected_device, std::uintmax_t expected_owner,
    const fs::path& relative_path) {
    struct open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(::syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, open_error);
    }
    OwnedDescriptor opened(descriptor);
    const struct stat status = descriptor_status(
        opened.get(), LocalSourceWorkspaceStage::Cleanup,
        relative_path);
    if(!S_ISDIR(status.st_mode) ||
       !cleanup_metadata_matches_plan(
           status, expected.status, relative_path) ||
       observe_cleanup_file_handle(opened.get(), relative_path) !=
           expected.file_handle ||
       static_cast<std::uintmax_t>(status.st_dev) != expected_device ||
       static_cast<std::uintmax_t>(status.st_uid) != expected_owner) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path);
    }
    return opened;
}

void make_owned_cleanup_directory_accessible(
    int directory_descriptor, const OwnedCleanupNode& directory,
    const fs::path& relative_path) {
    if(::syscall(
           SYS_fchmodat2, directory_descriptor, "", 0700,
           AT_EMPTY_PATH) != 0) {
        const int mode_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, mode_error);
    }
    const struct stat status = descriptor_status(
        directory_descriptor, LocalSourceWorkspaceStage::Cleanup,
        relative_path);
    if(!S_ISDIR(status.st_mode) || (status.st_mode & 07777) != 0700 ||
       status.st_dev != directory.status.st_dev ||
       status.st_ino != directory.status.st_ino ||
       status.st_uid != directory.status.st_uid) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path);
    }
}

using OwnedCleanupDirectoryLineage =
    std::vector<const OwnedCleanupNode*>;

struct OwnedCleanupRootAuthority {
    RetainedTrustedCacheDirectory& directory;
    std::uintmax_t device = 0;
    std::uintmax_t owner = 0;
};

OwnedDescriptor duplicate_owned_cleanup_root(
    OwnedCleanupRootAuthority& authority) {
    LocalSourceWorkspaceCleanupAccess::prepare(authority.directory);
    const int descriptor = ::fcntl(
        LocalSourceWorkspaceCleanupAccess::descriptor(
            authority.directory),
        F_DUPFD_CLOEXEC, 0);
    if(descriptor < 0) {
        const int duplication_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure, {},
            duplication_error);
    }
    OwnedDescriptor duplicated(descriptor);
    const struct stat status = descriptor_status(
        duplicated.get(), LocalSourceWorkspaceStage::Cleanup, {});
    if(!S_ISDIR(status.st_mode) ||
       static_cast<std::uintmax_t>(status.st_dev) != authority.device ||
       static_cast<std::uintmax_t>(status.st_uid) != authority.owner) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure);
    }
    return duplicated;
}

OwnedDescriptor open_owned_cleanup_parent_lineage(
    OwnedCleanupRootAuthority& authority,
    const OwnedCleanupDirectoryLineage& lineage) {
    OwnedDescriptor current = duplicate_owned_cleanup_root(authority);
    fs::path relative_path;
    for(const OwnedCleanupNode* ancestor : lineage) {
        if(ancestor == nullptr || !S_ISDIR(ancestor->status.st_mode)) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::Cleanup,
                LocalSourceWorkspaceErrorCode::CleanupFailure,
                relative_path);
        }
        relative_path /= ancestor->name;
        OpenedOwnedCleanupNode opened = open_owned_cleanup_node(
            current.get(), ancestor->name, ancestor,
            authority.device, authority.owner, relative_path);
        make_owned_cleanup_directory_accessible(
            opened.descriptor.get(), *ancestor, relative_path);
        require_owned_cleanup_node_unchanged(
            current.get(), ancestor->name, *ancestor,
            opened.descriptor.get(),
            authority.device, authority.owner, relative_path);
        current = open_owned_cleanup_directory(
            current.get(), ancestor->name, *ancestor,
            authority.device, authority.owner, relative_path);
    }
    return current;
}

std::vector<std::string> owned_cleanup_child_names(
    const std::vector<OwnedCleanupNode>& children) {
    std::vector<std::string> names;
    names.reserve(children.size());
    for(const OwnedCleanupNode& child : children) {
        names.push_back(child.name);
    }
    return names;
}

OwnedCleanupNode preflight_owned_workspace_node(
    OwnedCleanupRootAuthority& authority,
    OwnedCleanupDirectoryLineage& lineage, const std::string& name,
    const fs::path& parent_relative_path) {
    const fs::path relative_path = parent_relative_path / name;
    OwnedCleanupNode node;
    node.name = name;
    std::vector<std::string> children;
    // Descriptor scopes end before recursive descent; the plan retains only
    // names and identity metadata, never one descriptor per descendant.
    {
        OwnedDescriptor parent = open_owned_cleanup_parent_lineage(
            authority, lineage);
        OpenedOwnedCleanupNode opened = open_owned_cleanup_node(
            parent.get(), name, nullptr, authority.device,
            authority.owner, relative_path);
        node.status = opened.status;
        node.file_handle = std::move(opened.file_handle);
        if(S_ISDIR(node.status.st_mode)) {
            make_owned_cleanup_directory_accessible(
                opened.descriptor.get(), node, relative_path);
            require_owned_cleanup_node_unchanged(
                parent.get(), name, node, opened.descriptor.get(),
                authority.device, authority.owner, relative_path);
            OwnedDescriptor directory = open_owned_cleanup_directory(
                parent.get(), name, node, authority.device,
                authority.owner, relative_path);
            children = enumerate_cleanup_directory(
                directory.get(), relative_path);
        }
    }

    if(S_ISDIR(node.status.st_mode)) {
        lineage.push_back(&node);
        try {
            node.children.reserve(children.size());
            for(const std::string& child : children) {
                node.children.push_back(preflight_owned_workspace_node(
                    authority, lineage, child, relative_path));
            }
        } catch(...) {
            lineage.pop_back();
            throw;
        }
        lineage.pop_back();

        OwnedDescriptor parent = open_owned_cleanup_parent_lineage(
            authority, lineage);
        OpenedOwnedCleanupNode opened = open_owned_cleanup_node(
            parent.get(), name, &node, authority.device,
            authority.owner, relative_path);
        require_owned_cleanup_node_unchanged(
            parent.get(), name, node, opened.descriptor.get(),
            authority.device, authority.owner, relative_path);
        OwnedDescriptor directory = open_owned_cleanup_directory(
            parent.get(), name, node, authority.device,
            authority.owner, relative_path);
        if(enumerate_cleanup_directory(directory.get(), relative_path) !=
           children) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::Cleanup,
                LocalSourceWorkspaceErrorCode::CleanupFailure,
                relative_path);
        }
    }

    return node;
}

void revalidate_owned_workspace_node(
    OwnedCleanupRootAuthority& authority,
    OwnedCleanupDirectoryLineage& lineage,
    const OwnedCleanupNode& node,
    const fs::path& parent_relative_path) {
    const fs::path relative_path = parent_relative_path / node.name;
    {
        OwnedDescriptor parent = open_owned_cleanup_parent_lineage(
            authority, lineage);
        OpenedOwnedCleanupNode opened = open_owned_cleanup_node(
            parent.get(), node.name, &node, authority.device,
            authority.owner, relative_path);
        if(!S_ISDIR(node.status.st_mode)) return;

        make_owned_cleanup_directory_accessible(
            opened.descriptor.get(), node, relative_path);
        require_owned_cleanup_node_unchanged(
            parent.get(), node.name, node,
            opened.descriptor.get(), authority.device,
            authority.owner, relative_path);
        OwnedDescriptor directory = open_owned_cleanup_directory(
            parent.get(), node.name, node, authority.device,
            authority.owner, relative_path);
        if(enumerate_cleanup_directory(
               directory.get(), relative_path) !=
           owned_cleanup_child_names(node.children)) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::Cleanup,
                LocalSourceWorkspaceErrorCode::CleanupFailure,
                relative_path);
        }
        require_owned_cleanup_node_unchanged(
            parent.get(), node.name, node,
            opened.descriptor.get(), authority.device,
            authority.owner, relative_path);
    }

    lineage.push_back(&node);
    try {
        for(const OwnedCleanupNode& child : node.children) {
            revalidate_owned_workspace_node(
                authority, lineage, child, relative_path);
        }
    } catch(...) {
        lineage.pop_back();
        throw;
    }
    lineage.pop_back();

    {
        OwnedDescriptor parent = open_owned_cleanup_parent_lineage(
            authority, lineage);
        OpenedOwnedCleanupNode opened = open_owned_cleanup_node(
            parent.get(), node.name, &node, authority.device,
            authority.owner, relative_path);
        OwnedDescriptor directory = open_owned_cleanup_directory(
            parent.get(), node.name, node, authority.device,
            authority.owner, relative_path);
        if(enumerate_cleanup_directory(
               directory.get(), relative_path) !=
           owned_cleanup_child_names(node.children)) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::Cleanup,
                LocalSourceWorkspaceErrorCode::CleanupFailure,
                relative_path);
        }
        require_owned_cleanup_node_unchanged(
            parent.get(), node.name, node,
            opened.descriptor.get(), authority.device,
            authority.owner, relative_path);
    }
}

void remove_preflighted_owned_workspace_node(
    OwnedCleanupRootAuthority& authority,
    OwnedCleanupDirectoryLineage& lineage,
    const OwnedCleanupNode& node,
    const fs::path& parent_relative_path) {
    const fs::path relative_path = parent_relative_path / node.name;
    const bool is_directory = S_ISDIR(node.status.st_mode);
    if(is_directory) {
        {
            OwnedDescriptor parent = open_owned_cleanup_parent_lineage(
                authority, lineage);
            static_cast<void>(open_owned_cleanup_node(
                parent.get(), node.name, &node, authority.device,
                authority.owner, relative_path));
        }
        lineage.push_back(&node);
        try {
            for(const OwnedCleanupNode& child : node.children) {
                remove_preflighted_owned_workspace_node(
                    authority, lineage, child, relative_path);
            }
        } catch(...) {
            lineage.pop_back();
            throw;
        }
        lineage.pop_back();
    }

    OwnedDescriptor parent = open_owned_cleanup_parent_lineage(
        authority, lineage);
    OpenedOwnedCleanupNode opened = open_owned_cleanup_node(
        parent.get(), node.name, &node, authority.device,
        authority.owner, relative_path);
    if(is_directory) {
        OwnedDescriptor directory = open_owned_cleanup_directory(
            parent.get(), node.name, node, authority.device,
            authority.owner, relative_path);
        if(!enumerate_cleanup_directory(directory.get(), relative_path)
                .empty()) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::Cleanup,
                LocalSourceWorkspaceErrorCode::CleanupFailure,
                relative_path);
        }
    }
    // Keep the freshly reacquired target pinned through the immediately
    // following name-relative mutation.
    require_owned_cleanup_node_unchanged(
        parent.get(), node.name, node, opened.descriptor.get(),
        authority.device, authority.owner, relative_path);
    if(::unlinkat(
           parent.get(), node.name.c_str(),
           is_directory ? AT_REMOVEDIR : 0) != 0) {
        const int removal_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            relative_path, removal_error);
    }
}

void remove_owned_workspace_contents(
    OwnedCleanupRootAuthority& authority) {
    OwnedCleanupDirectoryLineage lineage;
    OwnedDescriptor root = open_owned_cleanup_parent_lineage(
        authority, lineage);
    const std::vector<std::string> names =
        enumerate_cleanup_directory(root.get(), {});
    std::vector<OwnedCleanupNode> plans;
    plans.reserve(names.size());
    for(const std::string& name : names) {
        plans.push_back(preflight_owned_workspace_node(
            authority, lineage, name, {}));
    }
    root = open_owned_cleanup_parent_lineage(authority, lineage);
    if(enumerate_cleanup_directory(root.get(), {}) !=
       owned_cleanup_child_names(plans)) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure);
    }

    notify_before_cleanup_removal();
    for(const OwnedCleanupNode& plan : plans) {
        revalidate_owned_workspace_node(authority, lineage, plan, {});
    }
    root = open_owned_cleanup_parent_lineage(authority, lineage);
    if(enumerate_cleanup_directory(root.get(), {}) !=
       owned_cleanup_child_names(plans)) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure);
    }

    for(const OwnedCleanupNode& plan : plans) {
        remove_preflighted_owned_workspace_node(
            authority, lineage, plan, {});
    }
    root = open_owned_cleanup_parent_lineage(authority, lineage);
    if(!enumerate_cleanup_directory(root.get(), {}).empty()) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure);
    }
}

void write_all(
    int descriptor, const char* data, std::size_t size,
    const fs::path& relative_path) {
    std::size_t written = 0;
    while(written < size) {
        const ssize_t result = ::write(
            descriptor, data + written, size - written);
        if(result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if(result < 0 && errno == EINTR) continue;
        const int write_error = result < 0 ? errno : EIO;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::DestinationWrite,
            LocalSourceWorkspaceErrorCode::WriteFailure,
            relative_path, write_error);
    }
}

void preserve_descriptor_metadata(
    int descriptor, const struct stat& source_status,
    std::uintmax_t destination_mode,
    const fs::path& relative_path) {
    if(::fchmod(descriptor, static_cast<mode_t>(destination_mode)) != 0) {
        const int mode_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::MetadataPreservation,
            LocalSourceWorkspaceErrorCode::MetadataFailure,
            relative_path, mode_error);
    }
    const std::array<timespec, 2> times = {
        source_status.st_atim, source_status.st_mtim};
    if(::futimens(descriptor, times.data()) != 0) {
        const int time_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::MetadataPreservation,
            LocalSourceWorkspaceErrorCode::MetadataFailure,
            relative_path, time_error);
    }
}

void require_destination_file(
    int destination_parent_descriptor, const std::string& name,
    int destination_descriptor, std::uintmax_t expected_device,
    std::uintmax_t expected_owner, std::uintmax_t expected_size,
    std::uintmax_t expected_mode, const fs::path& relative_path) {
    const struct stat opened = descriptor_status(
        destination_descriptor,
        LocalSourceWorkspaceStage::WorkspaceRevalidation,
        relative_path);
    struct stat named{};
    if(::fstatat(
           destination_parent_descriptor, name.c_str(), &named,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, status_error);
    }
    if(!S_ISREG(opened.st_mode) || !S_ISREG(named.st_mode) ||
       opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
       static_cast<std::uintmax_t>(opened.st_dev) != expected_device ||
       static_cast<std::uintmax_t>(opened.st_uid) != expected_owner ||
       static_cast<std::uintmax_t>(named.st_uid) != expected_owner ||
       static_cast<std::uintmax_t>(opened.st_size) != expected_size ||
       static_cast<std::uintmax_t>(opened.st_mode & 07777) != expected_mode) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
}

void copy_directory_contents(
    int source_directory_descriptor,
    int destination_directory_descriptor,
    const ObservedNodeIdentity& expected_directory,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    SnapshotManifest& manifest,
    const fs::path& relative_path);

void copy_regular_file(
    int source_parent_descriptor, int destination_parent_descriptor,
    const std::string& name, const struct stat& named_before,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    SnapshotManifest& manifest,
    const fs::path& relative_path) {
    const ObservedNodeIdentity expected = observe_node_identity(named_before);
    require_admissible_source_node(
        expected, ObservedNodeType::RegularFile, source_device,
        source_owner, relative_path);
    OwnedDescriptor source = open_source_node(
        source_parent_descriptor, name, false, relative_path);
    const struct stat opened_before = descriptor_status(
        source.get(), LocalSourceWorkspaceStage::SourceOpen,
        relative_path);
    require_unchanged_node(expected, opened_before, relative_path);

    const int destination_descriptor = ::openat(
        destination_parent_descriptor, name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if(destination_descriptor < 0) {
        const int create_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::DestinationCreation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, create_error);
    }
    OwnedDescriptor destination(destination_descriptor);

    std::array<char, COPY_BUFFER_SIZE> buffer{};
    off_t offset = 0;
    std::uintmax_t copied_size = 0;
    while(true) {
        const ssize_t read_size = ::pread(
            source.get(), buffer.data(), buffer.size(), offset);
        if(read_size > 0) {
            write_all(
                destination.get(), buffer.data(),
                static_cast<std::size_t>(read_size), relative_path);
            offset += read_size;
            copied_size += static_cast<std::uintmax_t>(read_size);
            continue;
        }
        if(read_size == 0) break;
        if(errno == EINTR) continue;
        const int read_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::SourceRead,
            LocalSourceWorkspaceErrorCode::ReadFailure,
            relative_path, read_error);
    }

    notify_after_file_data_copied(relative_path);
    const struct stat opened_after = descriptor_status(
        source.get(), LocalSourceWorkspaceStage::SourceRevalidation,
        relative_path);
    require_unchanged_node(expected, opened_after, relative_path);
    const struct stat named_after = inspect_named_source_node(
        source_parent_descriptor, name, relative_path);
    require_unchanged_node(expected, named_after, relative_path);
    if(expected.size < 0 ||
       copied_size != static_cast<std::uintmax_t>(expected.size)) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ContentChanged,
            relative_path);
    }

    preserve_descriptor_metadata(
        destination.get(), named_before, expected.mode, relative_path);
    require_destination_file(
        destination_parent_descriptor, name, destination.get(),
        destination_device, destination_owner, copied_size,
        expected.mode, relative_path);
    const struct stat destination_after = descriptor_status(
        destination.get(),
        LocalSourceWorkspaceStage::WorkspaceRevalidation,
        relative_path);
    if(destination_after.st_nlink != 1) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    record_manifest_entry(
        manifest, relative_path, expected,
        observe_node_identity(destination_after));
}

std::string read_symlink_target(
    int source_parent_descriptor, const std::string& name,
    const struct stat& named_before, const fs::path& relative_path) {
    std::size_t buffer_size = std::max<std::size_t>(
        static_cast<std::size_t>(std::max<off_t>(named_before.st_size, 0)) +
            1,
        256);
    while(buffer_size <= MAX_SYMLINK_TARGET_SIZE) {
        std::vector<char> buffer(buffer_size);
        const ssize_t size = ::readlinkat(
            source_parent_descriptor, name.c_str(), buffer.data(),
            buffer.size());
        if(size < 0) {
            const int read_error = errno;
            throw_workspace_system_failure(
                LocalSourceWorkspaceStage::SourceRead,
                LocalSourceWorkspaceErrorCode::ReadFailure,
                relative_path, read_error);
        }
        if(static_cast<std::size_t>(size) < buffer.size()) {
            if(size == 0) {
                throw_workspace_failure(
                    LocalSourceWorkspaceStage::SourceRead,
                    LocalSourceWorkspaceErrorCode::SymlinkEscape,
                    relative_path);
            }
            return std::string(
                buffer.data(), static_cast<std::size_t>(size));
        }
        buffer_size *= 2;
    }
    throw_workspace_failure(
        LocalSourceWorkspaceStage::SourceRead,
        LocalSourceWorkspaceErrorCode::SymlinkEscape, relative_path);
}

void require_contained_symlink_target(
    const std::string& target, const fs::path& relative_path) {
    const fs::path target_path(target);
    if(target_path.is_absolute()) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            LocalSourceWorkspaceErrorCode::SymlinkEscape,
            relative_path);
    }
    const fs::path resolved =
        (relative_path.parent_path() / target_path).lexically_normal();
    if(resolved.is_absolute() || resolved.empty()) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceInspection,
            LocalSourceWorkspaceErrorCode::SymlinkEscape,
            relative_path);
    }
    for(const fs::path& component : resolved) {
        if(component == "..") {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::SourceInspection,
                LocalSourceWorkspaceErrorCode::SymlinkEscape,
                relative_path);
        }
    }
}

void copy_symlink(
    int source_parent_descriptor, int destination_parent_descriptor,
    const std::string& name, const struct stat& named_before,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    SnapshotManifest& manifest,
    const fs::path& relative_path) {
    const ObservedNodeIdentity expected = observe_node_identity(named_before);
    require_admissible_source_node(
        expected, ObservedNodeType::Symlink, source_device,
        source_owner, relative_path);
    const std::string target = read_symlink_target(
        source_parent_descriptor, name, named_before, relative_path);
    require_contained_symlink_target(target, relative_path);

    if(::symlinkat(
           target.c_str(), destination_parent_descriptor,
           name.c_str()) != 0) {
        const int create_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::DestinationCreation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, create_error);
    }

    const struct stat source_after = inspect_named_source_node(
        source_parent_descriptor, name, relative_path);
    require_unchanged_node(expected, source_after, relative_path);
    struct stat destination_status{};
    if(::fstatat(
           destination_parent_descriptor, name.c_str(),
           &destination_status, AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, status_error);
    }
    if(!S_ISLNK(destination_status.st_mode) ||
       static_cast<std::uintmax_t>(destination_status.st_dev) !=
           destination_device ||
       static_cast<std::uintmax_t>(destination_status.st_uid) !=
           destination_owner) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    record_manifest_entry(
        manifest, relative_path, expected,
        observe_node_identity(destination_status), target);
}

void copy_directory(
    int source_parent_descriptor, int destination_parent_descriptor,
    const std::string& name, const struct stat& named_before,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    SnapshotManifest& manifest,
    const fs::path& relative_path) {
    const ObservedNodeIdentity expected = observe_node_identity(named_before);
    require_admissible_source_node(
        expected, ObservedNodeType::Directory, source_device,
        source_owner, relative_path);
    OwnedDescriptor source = open_source_node(
        source_parent_descriptor, name, true, relative_path);
    const struct stat opened_before = descriptor_status(
        source.get(), LocalSourceWorkspaceStage::SourceOpen,
        relative_path);
    require_unchanged_node(expected, opened_before, relative_path);

    if(::mkdirat(destination_parent_descriptor, name.c_str(), 0700) != 0) {
        const int create_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::DestinationCreation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, create_error);
    }
    OwnedDescriptor destination = open_destination_directory(
        destination_parent_descriptor, name, relative_path);
    const struct stat destination_before = descriptor_status(
        destination.get(),
        LocalSourceWorkspaceStage::WorkspaceRevalidation,
        relative_path);
    if(!S_ISDIR(destination_before.st_mode) ||
       static_cast<std::uintmax_t>(destination_before.st_dev) !=
           destination_device ||
       static_cast<std::uintmax_t>(destination_before.st_uid) !=
           destination_owner ||
       (destination_before.st_mode & 07777) != 0700) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }

    copy_directory_contents(
        source.get(), destination.get(), expected, source_device,
        source_owner, destination_device, destination_owner,
        manifest, relative_path);
    const std::uintmax_t destination_mode = expected.mode | S_IRWXU;
    preserve_descriptor_metadata(
        destination.get(), named_before, destination_mode,
        relative_path);

    const struct stat named_after = inspect_named_source_node(
        source_parent_descriptor, name, relative_path);
    require_unchanged_node(expected, named_after, relative_path);
    const struct stat destination_after = descriptor_status(
        destination.get(),
        LocalSourceWorkspaceStage::WorkspaceRevalidation,
        relative_path);
    struct stat destination_named{};
    if(::fstatat(
           destination_parent_descriptor, name.c_str(),
           &destination_named, AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, status_error);
    }
    if(!S_ISDIR(destination_after.st_mode) ||
       !S_ISDIR(destination_named.st_mode) ||
       destination_after.st_dev != destination_named.st_dev ||
       destination_after.st_ino != destination_named.st_ino ||
       static_cast<std::uintmax_t>(destination_after.st_dev) !=
           destination_device ||
       static_cast<std::uintmax_t>(destination_after.st_uid) !=
           destination_owner ||
       static_cast<std::uintmax_t>(destination_after.st_mode & 07777) !=
           destination_mode) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    record_manifest_entry(
        manifest, relative_path, expected,
        observe_node_identity(destination_after));
}

void copy_entry(
    int source_parent_descriptor, int destination_parent_descriptor,
    const std::string& name, std::uintmax_t source_device,
    std::uintmax_t source_owner, std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    SnapshotManifest& manifest,
    const fs::path& parent_relative_path) {
    const fs::path relative_path = parent_relative_path / name;
    const struct stat named_before = inspect_named_source_node(
        source_parent_descriptor, name, relative_path);
    switch(observed_node_type(named_before.st_mode)) {
        case ObservedNodeType::RegularFile:
            copy_regular_file(
                source_parent_descriptor, destination_parent_descriptor,
                name, named_before, source_device, source_owner,
                destination_device, destination_owner, manifest,
                relative_path);
            return;
        case ObservedNodeType::Directory:
            copy_directory(
                source_parent_descriptor, destination_parent_descriptor,
                name, named_before, source_device, source_owner,
                destination_device, destination_owner, manifest,
                relative_path);
            return;
        case ObservedNodeType::Symlink:
            copy_symlink(
                source_parent_descriptor, destination_parent_descriptor,
                name, named_before, source_device, source_owner,
                destination_device, destination_owner, manifest,
                relative_path);
            return;
        case ObservedNodeType::Unsupported:
            throw_workspace_failure(
                LocalSourceWorkspaceStage::SourceInspection,
                LocalSourceWorkspaceErrorCode::UnsupportedFileType,
                relative_path);
    }
    throw_workspace_failure(
        LocalSourceWorkspaceStage::SourceInspection,
        LocalSourceWorkspaceErrorCode::UnsupportedFileType,
        relative_path);
}

void copy_directory_contents(
    int source_directory_descriptor,
    int destination_directory_descriptor,
    const ObservedNodeIdentity& expected_directory,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    SnapshotManifest& manifest,
    const fs::path& relative_path) {
    const struct stat opened_before = descriptor_status(
        source_directory_descriptor,
        LocalSourceWorkspaceStage::SourceInspection, relative_path);
    require_unchanged_node(
        expected_directory, opened_before, relative_path);
    require_admissible_source_node(
        expected_directory, ObservedNodeType::Directory, source_device,
        source_owner, relative_path);

    const std::vector<std::string> names_before =
        enumerate_directory(source_directory_descriptor, relative_path);
    for(const std::string& name : names_before) {
        copy_entry(
            source_directory_descriptor,
            destination_directory_descriptor, name, source_device,
            source_owner, destination_device, destination_owner,
            manifest, relative_path);
    }

    notify_before_directory_revalidation(relative_path);
    const std::vector<std::string> names_after =
        enumerate_directory(source_directory_descriptor, relative_path);
    if(names_before != names_after) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    const struct stat opened_after = descriptor_status(
        source_directory_descriptor,
        LocalSourceWorkspaceStage::SourceRevalidation, relative_path);
    require_unchanged_node(
        expected_directory, opened_after, relative_path);
}

const SnapshotManifestEntry& require_manifest_entry(
    const SnapshotManifest& manifest, const fs::path& relative_path) {
    const auto entry = manifest.find(relative_path.generic_string());
    if(entry == manifest.end()) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    return entry->second;
}

struct stat inspect_named_workspace_node(
    int parent_descriptor, const std::string& name,
    const fs::path& relative_path) {
    struct stat status{};
    if(::fstatat(
           parent_descriptor, name.c_str(), &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path, status_error);
    }
    return status;
}

void require_destination_named_identity(
    const struct stat& opened, const struct stat& named,
    const ObservedNodeIdentity& expected,
    const fs::path& relative_path) {
    if(observed_node_type(opened.st_mode) != expected.type ||
       observed_node_type(named.st_mode) != expected.type ||
       opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
       observe_node_identity(opened) != expected ||
       observe_node_identity(named) != expected) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
}

ssize_t pread_retry(
    int descriptor, char* buffer, std::size_t size, off_t offset,
    LocalSourceWorkspaceStage stage, const fs::path& relative_path) {
    while(true) {
        const ssize_t result = ::pread(descriptor, buffer, size, offset);
        if(result >= 0) return result;
        if(errno == EINTR) continue;
        const int read_error = errno;
        throw_workspace_system_failure(
            stage, LocalSourceWorkspaceErrorCode::ReadFailure,
            relative_path, read_error);
    }
}

void require_matching_file_contents(
    int source_descriptor, int destination_descriptor,
    const fs::path& relative_path) {
    std::array<char, COPY_BUFFER_SIZE> source_buffer{};
    std::array<char, COPY_BUFFER_SIZE> destination_buffer{};
    off_t offset = 0;
    while(true) {
        const ssize_t source_size = pread_retry(
            source_descriptor, source_buffer.data(),
            source_buffer.size(), offset,
            LocalSourceWorkspaceStage::SourceRevalidation,
            relative_path);
        const ssize_t destination_size = pread_retry(
            destination_descriptor, destination_buffer.data(),
            destination_buffer.size(), offset,
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            relative_path);
        if(source_size != destination_size ||
           (source_size > 0 &&
            !std::equal(
                source_buffer.begin(),
                source_buffer.begin() + source_size,
                destination_buffer.begin()))) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::SourceRevalidation,
                LocalSourceWorkspaceErrorCode::ContentChanged,
                relative_path);
        }
        if(source_size == 0) return;
        offset += source_size;
    }
}

void validate_snapshot_directory(
    int source_directory_descriptor,
    int destination_directory_descriptor,
    const ObservedNodeIdentity& expected_source_directory,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t destination_device,
    std::uintmax_t destination_owner,
    std::uintmax_t expected_destination_mode,
    const SnapshotManifest& manifest, std::size_t& validated_entries,
    const fs::path& relative_path) {
    const struct stat source_directory_before = descriptor_status(
        source_directory_descriptor,
        LocalSourceWorkspaceStage::SourceRevalidation, relative_path);
    require_unchanged_node(
        expected_source_directory, source_directory_before,
        relative_path);
    require_admissible_source_node(
        expected_source_directory, ObservedNodeType::Directory,
        source_device, source_owner, relative_path);

    const struct stat destination_directory_before = descriptor_status(
        destination_directory_descriptor,
        LocalSourceWorkspaceStage::WorkspaceRevalidation,
        relative_path);
    if(!S_ISDIR(destination_directory_before.st_mode) ||
       static_cast<std::uintmax_t>(destination_directory_before.st_dev) !=
           destination_device ||
       static_cast<std::uintmax_t>(destination_directory_before.st_uid) !=
           destination_owner ||
       static_cast<std::uintmax_t>(
           destination_directory_before.st_mode & 07777) !=
           expected_destination_mode) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    const ObservedNodeIdentity expected_destination_directory =
        observe_node_identity(destination_directory_before);

    const std::vector<std::string> source_names =
        enumerate_directory(source_directory_descriptor, relative_path);
    const std::vector<std::string> destination_names = enumerate_directory(
        destination_directory_descriptor, relative_path);
    if(source_names != destination_names) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }

    for(const std::string& name : source_names) {
        const fs::path entry_path = relative_path / name;
        const SnapshotManifestEntry& manifest_entry =
            require_manifest_entry(manifest, entry_path);
        const ObservedNodeIdentity& expected =
            manifest_entry.source_identity;
        const ObservedNodeIdentity& expected_destination =
            manifest_entry.destination_identity;
        const struct stat source_named = inspect_named_source_node(
            source_directory_descriptor, name, entry_path);
        require_unchanged_node(expected, source_named, entry_path);
        require_admissible_source_node(
            expected, expected.type, source_device, source_owner,
            entry_path);
        const struct stat destination_named = inspect_named_workspace_node(
            destination_directory_descriptor, name, entry_path);

        switch(expected.type) {
            case ObservedNodeType::RegularFile: {
                OwnedDescriptor source = open_source_node(
                    source_directory_descriptor, name, false, entry_path);
                OwnedDescriptor destination = open_destination_file(
                    destination_directory_descriptor, name, entry_path);
                const struct stat source_opened = descriptor_status(
                    source.get(),
                    LocalSourceWorkspaceStage::SourceRevalidation,
                    entry_path);
                require_unchanged_node(expected, source_opened, entry_path);
                const struct stat destination_opened = descriptor_status(
                    destination.get(),
                    LocalSourceWorkspaceStage::WorkspaceRevalidation,
                    entry_path);
                require_destination_named_identity(
                    destination_opened, destination_named,
                    expected_destination, entry_path);
                if(destination_opened.st_size != source_opened.st_size) {
                    throw_workspace_failure(
                        LocalSourceWorkspaceStage::WorkspaceRevalidation,
                        LocalSourceWorkspaceErrorCode::ContentChanged,
                        entry_path);
                }
                require_matching_file_contents(
                    source.get(), destination.get(), entry_path);
                const struct stat source_after = descriptor_status(
                    source.get(),
                    LocalSourceWorkspaceStage::SourceRevalidation,
                    entry_path);
                require_unchanged_node(expected, source_after, entry_path);
                const struct stat source_named_after =
                    inspect_named_source_node(
                        source_directory_descriptor, name, entry_path);
                require_unchanged_node(expected, source_named_after, entry_path);
                const struct stat destination_named_after =
                    inspect_named_workspace_node(
                        destination_directory_descriptor, name,
                        entry_path);
                if(observe_node_identity(destination_named_after) !=
                   observe_node_identity(destination_named)) {
                    throw_workspace_failure(
                        LocalSourceWorkspaceStage::WorkspaceRevalidation,
                        LocalSourceWorkspaceErrorCode::ConcurrentMutation,
                        entry_path);
                }
                break;
            }
            case ObservedNodeType::Directory: {
                OwnedDescriptor source = open_source_node(
                    source_directory_descriptor, name, true, entry_path);
                OwnedDescriptor destination = open_destination_directory(
                    destination_directory_descriptor, name, entry_path);
                const struct stat source_opened = descriptor_status(
                    source.get(),
                    LocalSourceWorkspaceStage::SourceRevalidation,
                    entry_path);
                require_unchanged_node(expected, source_opened, entry_path);
                const struct stat destination_opened = descriptor_status(
                    destination.get(),
                    LocalSourceWorkspaceStage::WorkspaceRevalidation,
                    entry_path);
                require_destination_named_identity(
                    destination_opened, destination_named,
                    expected_destination, entry_path);
                validate_snapshot_directory(
                    source.get(), destination.get(), expected, source_device,
                    source_owner, destination_device, destination_owner,
                    expected.mode | S_IRWXU, manifest, validated_entries,
                    entry_path);
                const struct stat source_named_after =
                    inspect_named_source_node(
                        source_directory_descriptor, name, entry_path);
                require_unchanged_node(expected, source_named_after, entry_path);
                const struct stat destination_named_after =
                    inspect_named_workspace_node(
                        destination_directory_descriptor, name,
                        entry_path);
                if(observe_node_identity(destination_named_after) !=
                   observe_node_identity(destination_named)) {
                    throw_workspace_failure(
                        LocalSourceWorkspaceStage::WorkspaceRevalidation,
                        LocalSourceWorkspaceErrorCode::ConcurrentMutation,
                        entry_path);
                }
                break;
            }
            case ObservedNodeType::Symlink: {
                if(!manifest_entry.symlink_target.has_value()) {
                    throw_workspace_failure(
                        LocalSourceWorkspaceStage::SourceRevalidation,
                        LocalSourceWorkspaceErrorCode::ContentChanged,
                        entry_path);
                }
                const std::string source_target = read_symlink_target(
                    source_directory_descriptor, name, source_named,
                    entry_path);
                const std::string destination_target = read_symlink_target(
                    destination_directory_descriptor, name,
                    destination_named, entry_path);
                if(source_target != *manifest_entry.symlink_target ||
                   destination_target != source_target ||
                   observe_node_identity(destination_named) !=
                       expected_destination) {
                    throw_workspace_failure(
                        LocalSourceWorkspaceStage::SourceRevalidation,
                        LocalSourceWorkspaceErrorCode::ContentChanged,
                        entry_path);
                }
                const struct stat source_named_after =
                    inspect_named_source_node(
                        source_directory_descriptor, name, entry_path);
                require_unchanged_node(expected, source_named_after, entry_path);
                const struct stat destination_named_after =
                    inspect_named_workspace_node(
                        destination_directory_descriptor, name,
                        entry_path);
                if(observe_node_identity(destination_named_after) !=
                       observe_node_identity(destination_named) ||
                   read_symlink_target(
                       destination_directory_descriptor, name,
                       destination_named_after, entry_path) !=
                       destination_target) {
                    throw_workspace_failure(
                        LocalSourceWorkspaceStage::WorkspaceRevalidation,
                        LocalSourceWorkspaceErrorCode::ConcurrentMutation,
                        entry_path);
                }
                break;
            }
            case ObservedNodeType::Unsupported:
                throw_workspace_failure(
                    LocalSourceWorkspaceStage::SourceRevalidation,
                    LocalSourceWorkspaceErrorCode::UnsupportedFileType,
                    entry_path);
        }
        ++validated_entries;
    }

    notify_before_directory_revalidation(relative_path);
    if(source_names !=
           enumerate_directory(
               source_directory_descriptor, relative_path) ||
       destination_names !=
           enumerate_directory(
               destination_directory_descriptor, relative_path)) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    const struct stat source_directory_after = descriptor_status(
        source_directory_descriptor,
        LocalSourceWorkspaceStage::SourceRevalidation, relative_path);
    require_unchanged_node(
        expected_source_directory, source_directory_after,
        relative_path);
    const struct stat destination_directory_after = descriptor_status(
        destination_directory_descriptor,
        LocalSourceWorkspaceStage::WorkspaceRevalidation,
        relative_path);
    if(observe_node_identity(destination_directory_after) !=
       expected_destination_directory) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
}

bool is_same_or_descendant(
    const fs::path& candidate, const fs::path& ancestor) {
    auto candidate_component = candidate.begin();
    auto ancestor_component = ancestor.begin();
    for(; ancestor_component != ancestor.end();
        ++ancestor_component, ++candidate_component) {
        if(candidate_component == candidate.end() ||
           *candidate_component != *ancestor_component) {
            return false;
        }
    }
    return true;
}

void require_directory_identity_outside_source_subtree(
    int source_directory_descriptor,
    const ObservedNodeIdentity& expected_directory,
    std::uintmax_t source_device, std::uintmax_t source_owner,
    std::uintmax_t directory_device, std::uintmax_t directory_inode,
    const fs::path& relative_path) {
    const struct stat opened_before = descriptor_status(
        source_directory_descriptor,
        LocalSourceWorkspaceStage::BoundaryValidation, relative_path);
    require_unchanged_node(
        expected_directory, opened_before, relative_path);
    require_admissible_source_node(
        expected_directory, ObservedNodeType::Directory, source_device,
        source_owner, relative_path);

    const std::vector<std::string> names_before =
        enumerate_directory(source_directory_descriptor, relative_path);
    for(const std::string& name : names_before) {
        const fs::path entry_path = relative_path / name;
        const struct stat named_before = inspect_named_source_node(
            source_directory_descriptor, name, entry_path);
        const ObservedNodeIdentity identity =
            observe_node_identity(named_before);
        if(identity.type != ObservedNodeType::Directory) continue;

        if(identity.device == directory_device &&
           identity.inode == directory_inode) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::BoundaryValidation,
                LocalSourceWorkspaceErrorCode::CacheInsideSource,
                entry_path);
        }
        require_admissible_source_node(
            identity, ObservedNodeType::Directory, source_device,
            source_owner, entry_path);

        OwnedDescriptor child = open_source_node(
            source_directory_descriptor, name, true, entry_path);
        const struct stat opened_child = descriptor_status(
            child.get(), LocalSourceWorkspaceStage::BoundaryValidation,
            entry_path);
        require_unchanged_node(identity, opened_child, entry_path);
        require_directory_identity_outside_source_subtree(
            child.get(), identity, source_device, source_owner,
            directory_device, directory_inode, entry_path);
        const struct stat named_after = inspect_named_source_node(
            source_directory_descriptor, name, entry_path);
        require_unchanged_node(identity, named_after, entry_path);
    }

    const std::vector<std::string> names_after =
        enumerate_directory(source_directory_descriptor, relative_path);
    if(names_before != names_after) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation,
            relative_path);
    }
    const struct stat opened_after = descriptor_status(
        source_directory_descriptor,
        LocalSourceWorkspaceStage::BoundaryValidation, relative_path);
    require_unchanged_node(expected_directory, opened_after, relative_path);
}

std::string random_workspace_leaf() {
    std::array<unsigned char, RANDOM_NAME_BYTES> random_bytes{};
    std::size_t filled = 0;
    while(filled < random_bytes.size()) {
        const ssize_t result = ::getrandom(
            random_bytes.data() + filled,
            random_bytes.size() - filled, 0);
        if(result > 0) {
            filled += static_cast<std::size_t>(result);
            continue;
        }
        if(result < 0 && errno == EINTR) continue;
        const int random_error = result < 0 ? errno : EIO;
        throw_workspace_system_failure(
            LocalSourceWorkspaceStage::NameGeneration,
            LocalSourceWorkspaceErrorCode::RandomnessUnavailable, {},
            random_error);
    }

    constexpr char HEX[] = "0123456789abcdef";
    std::string leaf(WORKSPACE_PREFIX);
    leaf.reserve(WORKSPACE_PREFIX.size() + random_bytes.size() * 2);
    for(const unsigned char byte : random_bytes) {
        leaf.push_back(HEX[byte >> 4]);
        leaf.push_back(HEX[byte & 0x0f]);
    }
    return leaf;
}

ValidatedCachePath create_source_workspace_path(
    const ValidatedCacheRoot& cache_root) {
    for(std::size_t attempt = 0; attempt < MAX_NAME_ATTEMPTS; ++attempt) {
        const std::string leaf = random_workspace_leaf();
        try {
            return create_trusted_cache_directory(
                cache_root, cache_root.canonical_path() / leaf);
        } catch(const TrustedCacheError& error) {
            if(error.failure().code ==
               TrustedCacheErrorCode::ConcurrentReplacement) {
                continue;
            }
            throw_workspace_failure(
                LocalSourceWorkspaceStage::WorkspaceCreation,
                system_error_code(
                    error.failure().system_error.has_value()
                        ? error.failure().system_error->value()
                        : 0,
                    LocalSourceWorkspaceErrorCode::MetadataFailure),
                {},
                error.failure().system_error.has_value()
                    ? std::optional<int>(
                          error.failure().system_error->value())
                    : std::nullopt);
        } catch(...) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::WorkspaceCreation,
                LocalSourceWorkspaceErrorCode::MetadataFailure);
        }
    }
    throw_workspace_failure(
        LocalSourceWorkspaceStage::NameGeneration,
        LocalSourceWorkspaceErrorCode::NameCollision);
}

void require_source_and_cache_boundary(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root) {
    try {
        source_root.require_unchanged_identity();
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation);
    }
    try {
        cache_root.require_unchanged_identity();
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::BoundaryValidation,
            LocalSourceWorkspaceErrorCode::MetadataFailure);
    }
    if((cache_root.device() == source_root.directory_identity().device &&
        cache_root.inode() == source_root.directory_identity().inode) ||
       is_same_or_descendant(
           cache_root.canonical_path(),
           source_root.canonical_path())) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::BoundaryValidation,
            LocalSourceWorkspaceErrorCode::CacheInsideSource);
    }
}

} // namespace

LocalSourceWorkspaceError::LocalSourceWorkspaceError(
    LocalSourceWorkspaceFailure failure)
    : std::runtime_error("local-source-workspace-error"),
      failure_(std::move(failure)) {
}

LocalSourceWorkspace::LocalSourceWorkspace(
    RetainedTrustedCacheDirectory directory) noexcept
    : directory_(std::move(directory)) {
}

LocalSourceWorkspace::LocalSourceWorkspace(
    LocalSourceWorkspace&& other) noexcept
    : directory_(std::move(other.directory_)),
      state_(std::exchange(other.state_, State::MovedFrom)) {
}

LocalSourceWorkspace::~LocalSourceWorkspace() noexcept {
    if(state_ == State::Active) {
        try {
            cleanup();
        } catch(...) {
            // Destruction reports residue but must not retry a failed attempt.
        }
    }
    if(state_ == State::CleanupAttempted) {
        Logger::warn_noexcept([this]() {
            // TRANSLATORS: The placeholder is a display-only source workspace path whose cleanup could not be completed.
            return localization::format_translated_message(
                "Local source workspace cleanup could not be completed; temporary source data may remain at {}.",
                path().string());
        });
    }
}

const fs::path& LocalSourceWorkspace::path() const noexcept {
    return directory_.path().canonical_path();
}

void LocalSourceWorkspace::require_unchanged_identity() const {
    if(state_ != State::Active) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::InvalidState);
    }
    try {
        directory_.require_unchanged_identity();
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation);
    }
}

void LocalSourceWorkspace::cleanup() {
    if(state_ == State::Cleaned) return;
    if(state_ != State::Active) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::InvalidState);
    }
    // Record the attempt before any validation or removal can fail.
    state_ = State::CleanupAttempted;
    try {
        OwnedCleanupRootAuthority cleanup_authority{
            directory_, directory_.path().device(),
            static_cast<std::uintmax_t>(geteuid())};
        remove_owned_workspace_contents(cleanup_authority);
        directory_.prepare_for_owned_cleanup();
        remove_trusted_cache_path(directory_.path());
    } catch(const LocalSourceWorkspaceError&) {
        throw;
    } catch(const TrustedCacheError& error) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure, {},
            error.failure().system_error.has_value()
                ? std::optional<int>(
                      error.failure().system_error->value())
                : std::nullopt);
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure);
    }
    state_ = State::Cleaned;
}

void require_local_source_cache_separation(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root) {
    require_source_and_cache_boundary(source_root, cache_root);

    require_directory_identity_outside_local_source_tree(
        source_root, cache_root.device(), cache_root.inode());
    cache_root.require_unchanged_identity();
}

void require_directory_identity_outside_local_source_tree(
    const LocalSourceRoot& source_root,
    std::uintmax_t directory_device,
    std::uintmax_t directory_inode) {
    try {
        source_root.require_unchanged_identity();
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation);
    }

    const struct stat boundary_source_status = descriptor_status(
        source_root.directory_descriptor_,
        LocalSourceWorkspaceStage::BoundaryValidation, {});
    const ObservedNodeIdentity boundary_source_identity =
        observe_node_identity(boundary_source_status);
    require_admissible_source_node(
        boundary_source_identity, ObservedNodeType::Directory,
        source_root.directory_identity_.device,
        source_root.expected_owner_, {});
    if(boundary_source_identity.device == directory_device &&
       boundary_source_identity.inode == directory_inode) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::BoundaryValidation,
            LocalSourceWorkspaceErrorCode::CacheInsideSource);
    }
    require_directory_identity_outside_source_subtree(
        source_root.directory_descriptor_, boundary_source_identity,
        boundary_source_identity.device, source_root.expected_owner_,
        directory_device, directory_inode, {});
    try {
        source_root.require_unchanged_identity();
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation);
    }
}

LocalSourceWorkspace materialize_local_source_workspace(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root) {
    require_local_source_cache_separation(source_root, cache_root);

    ValidatedCachePath workspace_path =
        create_source_workspace_path(cache_root);
    std::unique_ptr<DirCleanupGuard> cleanup_guard;
    try {
        cleanup_guard = std::make_unique<DirCleanupGuard>(workspace_path);
    } catch(...) {
        try {
            remove_trusted_cache_path(workspace_path);
        } catch(...) {
        }
        throw_workspace_failure(
            LocalSourceWorkspaceStage::WorkspaceCreation,
            LocalSourceWorkspaceErrorCode::MetadataFailure);
    }

    RetainedTrustedCacheDirectory destination = [&]() {
        try {
            return retain_trusted_cache_directory(workspace_path);
        } catch(...) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::WorkspaceCreation,
                LocalSourceWorkspaceErrorCode::MetadataFailure);
        }
    }();

    try {
        const struct stat source_status = descriptor_status(
            source_root.directory_descriptor_,
            LocalSourceWorkspaceStage::SourceInspection, {});
        const ObservedNodeIdentity source_identity =
            observe_node_identity(source_status);
        if(source_identity.type != ObservedNodeType::Directory ||
           source_identity.device != source_root.directory_identity_.device ||
           source_identity.inode != source_root.directory_identity_.inode ||
           source_identity.owner != source_root.expected_owner_ ||
           source_identity.mode != source_root.directory_identity_.mode) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::SourceRevalidation,
                LocalSourceWorkspaceErrorCode::ConcurrentMutation);
        }

        const struct stat destination_status = descriptor_status(
            destination.descriptor_,
            LocalSourceWorkspaceStage::WorkspaceRevalidation, {});
        SnapshotManifest manifest;
        copy_directory_contents(
            source_root.directory_descriptor_, destination.descriptor_,
            source_identity, source_identity.device,
            source_root.expected_owner_,
            static_cast<std::uintmax_t>(destination_status.st_dev),
            static_cast<std::uintmax_t>(destination_status.st_uid),
            manifest, {});
        std::size_t validated_entries = 0;
        validate_snapshot_directory(
            source_root.directory_descriptor_, destination.descriptor_,
            source_identity, source_identity.device,
            source_root.expected_owner_,
            static_cast<std::uintmax_t>(destination_status.st_dev),
            static_cast<std::uintmax_t>(destination_status.st_uid), 0700,
            manifest, validated_entries, {});
        if(validated_entries != manifest.size()) {
            throw_workspace_failure(
                LocalSourceWorkspaceStage::SourceRevalidation,
                LocalSourceWorkspaceErrorCode::ConcurrentMutation);
        }
        source_root.require_unchanged_identity();
        destination.require_unchanged_identity();
        cache_root.require_unchanged_identity();
    } catch(const LocalSourceWorkspaceError&) {
        throw;
    } catch(...) {
        throw_workspace_failure(
            LocalSourceWorkspaceStage::SourceRevalidation,
            LocalSourceWorkspaceErrorCode::ConcurrentMutation);
    }

    // Construction rollback ends here. The noexcept workspace construction
    // transfers cleanup ownership to the returned object alone.
    cleanup_guard->commit();
    return LocalSourceWorkspace(std::move(destination));
}

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
void set_local_source_workspace_test_hook(
    LocalSourceWorkspaceTestHook hook) {
    g_workspace_test_hook = std::move(hook);
}

void set_local_source_workspace_cleanup_metadata_match_for_test(
    LocalSourceWorkspaceCleanupMetadataMatchForTest match) {
    g_cleanup_metadata_match_for_test = std::move(match);
}

void require_cache_identity_outside_source_tree_for_test(
    const LocalSourceRoot& source_root,
    std::uintmax_t cache_device, std::uintmax_t cache_inode) {
    require_directory_identity_outside_local_source_tree(
        source_root, cache_device, cache_inode);
}
#endif
