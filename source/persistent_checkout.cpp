#include "persistent_checkout.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <filesystem>
#include <fcntl.h>
#include <linux/openat2.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

struct PersistentCheckoutDirectoryAccess {
    static int descriptor(
        const RetainedTrustedCacheDirectory& directory) noexcept {
        return directory.descriptor_;
    }
};

// Persistent trusted cache checkout固有のdescendant policyを所有する。
// POLICY(#197): containment / cwd / remove / rollbackはtrusted_cacheの責務として再実装しない。
namespace {

namespace fs = std::filesystem;

[[noreturn]] void throw_descendant_error(
    TrustedCacheErrorCode code,
    std::optional<int> error_number = std::nullopt) {
    TrustedCacheFailure failure{
        TrustedCacheStage::ChildValidation, code, std::nullopt};
    if(error_number.has_value()) {
        failure.system_error = std::error_code(
            error_number.value(), std::generic_category());
    }
    throw TrustedCacheError(std::move(failure));
}

bool is_permission_error(int error_number) noexcept {
    return error_number == EACCES || error_number == EPERM;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

struct OwnedFileDescriptor {
    int value = -1;

    explicit OwnedFileDescriptor(int descriptor) noexcept
        : value(descriptor) {
    }

    OwnedFileDescriptor(const OwnedFileDescriptor&) = delete;
    OwnedFileDescriptor& operator=(const OwnedFileDescriptor&) = delete;

    OwnedFileDescriptor(OwnedFileDescriptor&& other) noexcept
        : value(std::exchange(other.value, -1)) {
    }

    ~OwnedFileDescriptor() noexcept {
        if(value >= 0) static_cast<void>(close(value));
    }

    int release() noexcept {
        return std::exchange(value, -1);
    }
};

struct DirectoryStreamCloser {
    void operator()(DIR* directory) const noexcept {
        if(directory != nullptr) static_cast<void>(closedir(directory));
    }
};

enum class CheckoutEntryRequirement {
    Directory,
    RegularFile,
    DirectoryOrRegularFile,
};

struct OpenedCheckoutEntry {
    OwnedFileDescriptor descriptor;
    struct stat status{};
};

bool same_identity_and_type(
    const struct stat& expected, const struct stat& actual) noexcept {
    return (expected.st_mode & S_IFMT) == (actual.st_mode & S_IFMT) &&
           expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino;
}

void require_safe_checkout_status(
    const struct stat& status,
    CheckoutEntryRequirement requirement) {
    if(S_ISLNK(status.st_mode)) {
        throw_descendant_error(TrustedCacheErrorCode::Symlink);
    }
    if((requirement == CheckoutEntryRequirement::Directory &&
        !S_ISDIR(status.st_mode)) ||
       (requirement == CheckoutEntryRequirement::RegularFile &&
        !S_ISREG(status.st_mode)) ||
       (requirement == CheckoutEntryRequirement::DirectoryOrRegularFile &&
        !S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode))) {
        throw_descendant_error(
            requirement == CheckoutEntryRequirement::Directory
                ? TrustedCacheErrorCode::NotDirectory
                : TrustedCacheErrorCode::NotRegularFile);
    }
    if(status.st_uid != geteuid()) {
        throw_descendant_error(
            TrustedCacheErrorCode::OwnershipMismatch);
    }
    if((status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
       (S_ISDIR(status.st_mode) &&
        (status.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
            (S_IRUSR | S_IWUSR | S_IXUSR))) {
        throw_descendant_error(
            TrustedCacheErrorCode::UnsafePermissions);
    }
    // POLICY(#305): Moguet creates a standalone network clone. A regular
    // metadata/artifact hardlink could let git/editor mutate a same-device
    // inode outside the checkout, so local-clone hardlink layouts are refused.
    if(S_ISREG(status.st_mode) && status.st_nlink != 1) {
        throw_descendant_error(TrustedCacheErrorCode::ChildEscape);
    }
}

OpenedCheckoutEntry open_safe_checkout_entry(
    int parent_descriptor, const fs::path& inspection_path,
    CheckoutEntryRequirement requirement,
    TrustedCacheErrorCode missing_code,
    bool require_regular_file_read_access = false) {
    struct stat named_status{};
    if(fstatat(
           parent_descriptor, inspection_path.c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        if(metadata_error == ENOENT || metadata_error == ENOTDIR) {
            throw_descendant_error(missing_code, metadata_error);
        }
        throw_descendant_error(
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    require_safe_checkout_status(named_status, requirement);

    struct open_how how{};
    how.flags = O_CLOEXEC | O_NOFOLLOW;
    if(S_ISDIR(named_status.st_mode)) {
        how.flags |= O_RDONLY | O_DIRECTORY;
    } else {
        // LANDMINE(#411): a regular file can be replaced by a FIFO between
        // metadata inspection and open. Nonblocking open lets the identity/type
        // revalidation reject that race without waiting for a writer.
        how.flags |= require_regular_file_read_access
                         ? O_RDONLY | O_NONBLOCK
                         : O_PATH;
    }
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    const int descriptor = static_cast<int>(syscall(
        SYS_openat2, parent_descriptor, inspection_path.c_str(), &how,
        sizeof(how)));
    if(descriptor < 0) {
        const int open_error = errno;
        if(open_error == ELOOP) {
            throw_descendant_error(
                TrustedCacheErrorCode::Symlink, open_error);
        }
        if(open_error == EXDEV) {
            throw_descendant_error(
                TrustedCacheErrorCode::ChildEscape, open_error);
        }
        if(is_permission_error(open_error)) {
            throw_descendant_error(
                TrustedCacheErrorCode::PermissionDenied, open_error);
        }
        if(open_error == ENOENT || open_error == ENOTDIR) {
            throw_descendant_error(
                TrustedCacheErrorCode::ConcurrentReplacement,
                open_error);
        }
        throw_descendant_error(
            TrustedCacheErrorCode::MetadataFailure, open_error);
    }
    OwnedFileDescriptor opened(descriptor);
    struct stat opened_status{};
    if(fstat(opened.value, &opened_status) != 0) {
        const int metadata_error = errno;
        throw_descendant_error(
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!same_identity_and_type(named_status, opened_status)) {
        throw_descendant_error(
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    require_safe_checkout_status(opened_status, requirement);
    return OpenedCheckoutEntry{std::move(opened), opened_status};
}

void revalidate_named_checkout_entry(
    int parent_descriptor, const fs::path& inspection_path,
    const struct stat& expected,
    CheckoutEntryRequirement requirement) {
    struct stat current_status{};
    if(fstatat(
           parent_descriptor, inspection_path.c_str(), &current_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_descendant_error(
            metadata_error == ENOENT || metadata_error == ENOTDIR
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : (is_permission_error(metadata_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::MetadataFailure),
            metadata_error);
    }
    if(!same_identity_and_type(expected, current_status)) {
        throw_descendant_error(
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    require_safe_checkout_status(current_status, requirement);
}

std::vector<std::string> directory_entry_names(
    int directory_descriptor, const struct stat& expected_status,
    const std::function<void()>* checkpoint = nullptr) {
    const int listing_descriptor = openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(listing_descriptor < 0) {
        const int open_error = errno;
        throw_descendant_error(
            is_permission_error(open_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            open_error);
    }
    OwnedFileDescriptor listing(listing_descriptor);
    struct stat listing_status{};
    if(fstat(listing.value, &listing_status) != 0) {
        const int metadata_error = errno;
        throw_descendant_error(
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!same_identity_and_type(expected_status, listing_status)) {
        throw_descendant_error(
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    require_safe_checkout_status(
        listing_status, CheckoutEntryRequirement::Directory);

    DIR* raw_directory = fdopendir(listing.value);
    if(raw_directory == nullptr) {
        const int stream_error = errno;
        throw_descendant_error(
            is_permission_error(stream_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            stream_error);
    }
    static_cast<void>(listing.release());
    std::unique_ptr<DIR, DirectoryStreamCloser> directory(raw_directory);

    std::vector<std::string> names;
    while(true) {
        if(checkpoint) (*checkpoint)();
        errno = 0;
        dirent* entry = readdir(directory.get());
        if(entry == nullptr) {
            const int read_error = errno;
            if(read_error != 0) {
                throw_descendant_error(
                    is_permission_error(read_error)
                        ? TrustedCacheErrorCode::PermissionDenied
                        : TrustedCacheErrorCode::MetadataFailure,
                    read_error);
            }
            break;
        }
        std::string name(entry->d_name);
        if(name != "." && name != "..") {
            names.push_back(std::move(name));
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

constexpr std::size_t MAX_GIT_METADATA_DEPTH = 128;

void require_safe_git_metadata_entry(
    int parent_descriptor, const fs::path& name,
    CheckoutEntryRequirement requirement, std::size_t depth,
    const fs::path& relative_path,
    bool require_regular_file_read_access,
    const std::function<void()>* checkpoint = nullptr) {
    if(checkpoint) (*checkpoint)();
    if(depth > MAX_GIT_METADATA_DEPTH) {
        throw_descendant_error(TrustedCacheErrorCode::ChildEscape);
    }
    // These regular files redirect Git to metadata/object storage outside a
    // standalone checkout without using a filesystem symlink.
    if(relative_path == "commondir" ||
       relative_path == fs::path("objects") / "info" / "alternates") {
        throw_descendant_error(TrustedCacheErrorCode::ChildEscape);
    }

    OpenedCheckoutEntry opened = open_safe_checkout_entry(
        parent_descriptor, name, requirement,
        TrustedCacheErrorCode::ConcurrentReplacement,
        require_regular_file_read_access);
    if(S_ISREG(opened.status.st_mode)) {
        revalidate_named_checkout_entry(
            parent_descriptor, name, opened.status, requirement);
        return;
    }

    const std::vector<std::string> names = directory_entry_names(
        opened.descriptor.value, opened.status, checkpoint);
    for(const std::string& child_name : names) {
        require_safe_git_metadata_entry(
            opened.descriptor.value, child_name,
            CheckoutEntryRequirement::DirectoryOrRegularFile,
            depth + 1, relative_path / child_name,
            require_regular_file_read_access, checkpoint);
    }
    if(directory_entry_names(opened.descriptor.value, opened.status, checkpoint) != names) {
        throw_descendant_error(
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    revalidate_named_checkout_entry(
        parent_descriptor, name, opened.status, requirement);
}

struct stat require_safe_checkout_descendant(
    int checkout_descriptor, const fs::path& inspection_path,
    bool require_directory) {
    OpenedCheckoutEntry opened = open_safe_checkout_entry(
        checkout_descriptor, inspection_path,
        require_directory ? CheckoutEntryRequirement::Directory
                          : CheckoutEntryRequirement::RegularFile,
        require_directory ? TrustedCacheErrorCode::NotDirectory
                          : TrustedCacheErrorCode::NotRegularFile);
    revalidate_named_checkout_entry(
        checkout_descriptor, inspection_path, opened.status,
        require_directory ? CheckoutEntryRequirement::Directory
                          : CheckoutEntryRequirement::RegularFile);
    return opened.status;
}

bool has_safe_git_directory(int checkout_descriptor) {
    struct stat status{};
    if(fstatat(
           checkout_descriptor, ".git", &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        if(metadata_error == ENOENT) return false;
        throw_descendant_error(
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(S_ISLNK(status.st_mode)) {
        throw_descendant_error(TrustedCacheErrorCode::Symlink);
    }
    // POLICY(#197): persistent checkoutではgitfile/worktree redirectを対応対象にしない。
    if(!S_ISDIR(status.st_mode)) {
        throw_descendant_error(TrustedCacheErrorCode::NotDirectory);
    }
    OpenedCheckoutEntry opened = open_safe_checkout_entry(
        checkout_descriptor, ".git",
        CheckoutEntryRequirement::Directory,
        TrustedCacheErrorCode::NotDirectory);
    revalidate_named_checkout_entry(
        checkout_descriptor, ".git", opened.status,
        CheckoutEntryRequirement::Directory);
    return true;
}

void require_safe_git_directory(
    int checkout_descriptor,
    bool require_regular_file_read_access = false,
    const std::function<void()>* checkpoint = nullptr) {
    if(!has_safe_git_directory(checkout_descriptor)) {
        throw_descendant_error(TrustedCacheErrorCode::NotDirectory);
    }
    require_safe_git_metadata_entry(
        checkout_descriptor, ".git",
        CheckoutEntryRequirement::Directory, 0, fs::path(),
        require_regular_file_read_access, checkpoint);
}

void require_safe_artifact(
    int checkout_descriptor, const fs::path& artifact_path) {
    if(artifact_path.empty() || artifact_path.is_absolute() ||
       artifact_path.parent_path() != fs::path() ||
       artifact_path == "." || artifact_path == "..") {
        throw_descendant_error(TrustedCacheErrorCode::ChildEscape);
    }
    struct stat status{};
    if(fstatat(
           checkout_descriptor, artifact_path.c_str(), &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        if(metadata_error == ENOENT || metadata_error == ENOTDIR) {
            throw_descendant_error(
                TrustedCacheErrorCode::NotRegularFile,
                metadata_error);
        }
        throw_descendant_error(
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(S_ISLNK(status.st_mode)) {
        throw_descendant_error(TrustedCacheErrorCode::Symlink);
    }
    if(!S_ISREG(status.st_mode)) {
        throw_descendant_error(TrustedCacheErrorCode::NotRegularFile);
    }
    static_cast<void>(require_safe_checkout_descendant(
        checkout_descriptor, artifact_path, false));
}

std::vector<fs::path> find_install_scripts(int checkout_descriptor) {
    std::vector<fs::path> scripts;
    const int listing_descriptor = openat(
        checkout_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(listing_descriptor < 0) {
        const int open_error = errno;
        throw_descendant_error(
            is_permission_error(open_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            open_error);
    }
    DIR* raw_directory = fdopendir(listing_descriptor);
    if(raw_directory == nullptr) {
        const int stream_error = errno;
        static_cast<void>(close(listing_descriptor));
        throw_descendant_error(
            is_permission_error(stream_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            stream_error);
    }
    std::unique_ptr<DIR, DirectoryStreamCloser> directory(raw_directory);

    while(true) {
        errno = 0;
        dirent* entry = readdir(directory.get());
        if(entry == nullptr) {
            const int read_error = errno;
            if(read_error != 0) {
                throw_descendant_error(
                    is_permission_error(read_error)
                        ? TrustedCacheErrorCode::PermissionDenied
                        : TrustedCacheErrorCode::MetadataFailure,
                    read_error);
            }
            break;
        }
        fs::path artifact_path(entry->d_name);
        if(artifact_path == "." || artifact_path == "..") continue;
        // POLICY(#197): 名前を先に対象化し、symlinkやspecial fileも検証対象から落とさない。
        if(artifact_path.extension() == ".install") {
            require_safe_artifact(checkout_descriptor, artifact_path);
            scripts.push_back(std::move(artifact_path));
        }
    }
    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

} // namespace

bool has_safe_persistent_checkout_git_directory(const ValidatedCachePath& checkout) {
    RetainedTrustedCacheDirectory directory =
        retain_trusted_cache_directory(checkout);
    const bool result = has_safe_git_directory(
        PersistentCheckoutDirectoryAccess::descriptor(directory));
    directory.require_unchanged_identity();
    return result;
}

void require_safe_persistent_checkout_git_metadata(
    const ValidatedCachePath& checkout) {
    RetainedTrustedCacheDirectory directory =
        retain_trusted_cache_directory(checkout);
    require_safe_git_directory(
        PersistentCheckoutDirectoryAccess::descriptor(directory));
    directory.require_unchanged_identity();
}

void require_safe_persistent_checkout_git_metadata(
    const ValidatedCachePath& checkout, const std::function<void()>& checkpoint) {
    RetainedTrustedCacheDirectory directory = retain_trusted_cache_directory(checkout);
    require_safe_git_directory(PersistentCheckoutDirectoryAccess::descriptor(directory), true, &checkpoint);
    directory.require_unchanged_identity();
}

void require_readable_persistent_checkout_git_metadata(
    const ValidatedCachePath& checkout) {
    RetainedTrustedCacheDirectory directory =
        retain_trusted_cache_directory(checkout);
    require_safe_git_directory(
        PersistentCheckoutDirectoryAccess::descriptor(directory), true);
    directory.require_unchanged_identity();
}

std::vector<std::filesystem::path> require_safe_persistent_checkout_descendants(
    const ValidatedCachePath& checkout) {
    RetainedTrustedCacheDirectory directory =
        retain_trusted_cache_directory(checkout);
    const int descriptor =
        PersistentCheckoutDirectoryAccess::descriptor(directory);
    require_safe_git_directory(descriptor);
    require_safe_artifact(descriptor, "PKGBUILD");
    std::vector<fs::path> scripts = find_install_scripts(descriptor);
    directory.require_unchanged_identity();
    return scripts;
}

PersistentCheckoutReviewOverrides observe_persistent_checkout_review_overrides(
    const ValidatedCachePath& checkout) {
    RetainedTrustedCacheDirectory directory =
        retain_trusted_cache_directory(checkout);
    const int descriptor =
        PersistentCheckoutDirectoryAccess::descriptor(directory);
    require_safe_git_directory(descriptor);

    const auto exists = [descriptor](const char* relative_path) {
        struct stat status{};
        if(fstatat(
               descriptor, relative_path, &status,
               AT_SYMLINK_NOFOLLOW) == 0) {
            return true;
        }
        const int metadata_error = errno;
        if(metadata_error == ENOENT || metadata_error == ENOTDIR) return false;
        throw_descendant_error(
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    };

    const bool attributes_before = exists(".git/info/attributes");
    const bool grafts_before = exists(".git/info/grafts");
    require_safe_git_directory(descriptor);
    PersistentCheckoutReviewOverrides overrides{
        attributes_before || exists(".git/info/attributes"),
        grafts_before || exists(".git/info/grafts")};
    directory.require_unchanged_identity();
    return overrides;
}

void require_safe_persistent_checkout_review_targets(
    const ValidatedCachePath& checkout,
    const std::vector<std::filesystem::path>& install_scripts) {
    // LANDMINE(#197): 再列挙だけでは、review開始後に消えた既存targetを見落とす。
    RetainedTrustedCacheDirectory directory =
        retain_trusted_cache_directory(checkout);
    const int descriptor =
        PersistentCheckoutDirectoryAccess::descriptor(directory);
    require_safe_git_directory(descriptor);
    require_safe_artifact(descriptor, "PKGBUILD");
    static_cast<void>(find_install_scripts(descriptor));
    for(const auto& install_script : install_scripts) {
        require_safe_artifact(descriptor, install_script);
    }
    directory.require_unchanged_identity();
}

bool remote_url_matches_expected(
    const std::string& current_url, const std::string& expected_url) {
    // LANDMINE: cache directoryの再利用可否を決めるguard。曖昧一致にすると別remoteを上書きし得る。
    return trim(current_url) == trim(expected_url);
}
