#include "trusted_cache.hpp"

#include "application_identity.hpp"
#include "localization.hpp"
#include "logging.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

struct ValidatedCacheRoot::State {
    struct RetainedDirectoryIdentity final {
        int descriptor = -1;
        std::string leaf_name;
        std::uintmax_t device = 0;
        std::uintmax_t inode = 0;
        std::uintmax_t owner = 0;
        bool requires_security_validation = false;

        RetainedDirectoryIdentity(
            int retained_descriptor, std::string retained_leaf_name,
            std::uintmax_t retained_device,
            std::uintmax_t retained_inode,
            std::uintmax_t retained_owner,
            bool retained_requires_security_validation) noexcept
            : descriptor(retained_descriptor),
              leaf_name(std::move(retained_leaf_name)),
              device(retained_device), inode(retained_inode),
              owner(retained_owner),
              requires_security_validation(
                  retained_requires_security_validation) {
        }

        RetainedDirectoryIdentity(const RetainedDirectoryIdentity&) = delete;
        RetainedDirectoryIdentity& operator=(
            const RetainedDirectoryIdentity&) = delete;

        RetainedDirectoryIdentity(
            RetainedDirectoryIdentity&& other) noexcept
            : descriptor(std::exchange(other.descriptor, -1)),
              leaf_name(std::move(other.leaf_name)),
              device(other.device), inode(other.inode), owner(other.owner),
              requires_security_validation(
                  other.requires_security_validation) {
        }

        RetainedDirectoryIdentity& operator=(
            RetainedDirectoryIdentity&&) = delete;

        ~RetainedDirectoryIdentity() noexcept {
            if(descriptor >= 0) static_cast<void>(close(descriptor));
        }
    };

    fs::path path;
    int parent_descriptor = -1;
    int directory_descriptor = -1;
    std::string leaf_name;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t permissions = 0;
    std::vector<RetainedDirectoryIdentity> retained_lineage;

    State(
        fs::path logical_path, int retained_parent_descriptor,
        int retained_directory_descriptor, std::string retained_leaf_name,
        std::uintmax_t retained_device, std::uintmax_t retained_inode,
        std::uintmax_t retained_owner,
        std::uintmax_t retained_permissions,
        std::vector<RetainedDirectoryIdentity>
            retained_directory_lineage) noexcept
        : path(std::move(logical_path)),
          parent_descriptor(retained_parent_descriptor),
          directory_descriptor(retained_directory_descriptor),
          leaf_name(std::move(retained_leaf_name)), device(retained_device),
          inode(retained_inode), owner(retained_owner),
          permissions(retained_permissions),
          retained_lineage(std::move(retained_directory_lineage)) {
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;

    ~State() noexcept {
        if(directory_descriptor >= 0) {
            static_cast<void>(close(directory_descriptor));
        }
        if(parent_descriptor >= 0) {
            static_cast<void>(close(parent_descriptor));
        }
    }
};

struct TrustedCacheAccess {
    static const std::shared_ptr<ValidatedCacheRoot::State>& state(
        const ValidatedCacheRoot& root) noexcept {
        return root.state_;
    }

    static ValidatedCacheRoot make_root(
        std::shared_ptr<ValidatedCacheRoot::State> state) {
        return ValidatedCacheRoot(std::move(state));
    }

    static ValidatedCachePath make_path(
        ValidatedCacheRoot root, fs::path path, std::string leaf,
        bool exists, bool is_directory, std::uintmax_t device,
        std::uintmax_t inode, std::uintmax_t owner,
        std::uintmax_t permissions) {
        return ValidatedCachePath(
            std::move(root), std::move(path), std::move(leaf), exists,
            is_directory, device, inode, owner, permissions);
    }

    static const ValidatedCacheRoot& root(
        const ValidatedCachePath& path) noexcept {
        return path.root_;
    }

    static const std::string& leaf(
        const ValidatedCachePath& path) noexcept {
        return path.leaf_name_;
    }

    static std::uintmax_t device(const ValidatedCachePath& path) noexcept {
        return path.device_;
    }

    static std::uintmax_t inode(const ValidatedCachePath& path) noexcept {
        return path.inode_;
    }

    static std::uintmax_t owner(const ValidatedCachePath& path) noexcept {
        return path.owner_;
    }

    static std::uintmax_t permissions(
        const ValidatedCachePath& path) noexcept {
        return path.permissions_;
    }
};

namespace {

constexpr mode_t REQUIRED_DIRECTORY_OWNER_PERMISSIONS =
    S_IRUSR | S_IWUSR | S_IXUSR;
constexpr mode_t FORBIDDEN_WRITE_PERMISSIONS = S_IWGRP | S_IWOTH;

#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
TrustedCacheRemovalTestHook removal_test_hook;
TrustedCacheStagedDirectoryTestHook staged_directory_test_hook;

void notify_removal_test_hook(std::size_t depth) {
    if(removal_test_hook) removal_test_hook(depth);
}

void notify_staged_directory_test_hook(const fs::path& path) {
    if(staged_directory_test_hook) staged_directory_test_hook(path);
}
#else
void notify_removal_test_hook(std::size_t) {
}
#endif

class OwnedFileDescriptor final {
    int descriptor_ = -1;

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

    int get() const noexcept {
        return descriptor_;
    }

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }
};

struct DirectoryStreamCloser {
    void operator()(DIR* directory) const noexcept {
        if(directory != nullptr) static_cast<void>(closedir(directory));
    }
};

std::string_view stage_name(TrustedCacheStage stage) {
    // NO_TRANSLATE: These values are stable internal stage tokens and are
    // passed as runtime data to complete diagnostic msgids.
    switch(stage) {
        case TrustedCacheStage::RootAdoption:
            return "root-adoption";
        case TrustedCacheStage::RootRevalidation:
            return "root-revalidation";
        case TrustedCacheStage::ChildValidation:
            return "child-validation";
        case TrustedCacheStage::ChildCreation:
            return "child-creation";
        case TrustedCacheStage::ChildOpen:
            return "child-open";
        case TrustedCacheStage::CleanupPreflight:
            return "cleanup-preflight";
        case TrustedCacheStage::RecursiveRemoval:
            return "recursive-removal";
        case TrustedCacheStage::Rollback:
            return "rollback";
    }
    throw std::logic_error(localization::translate_message(
        "Unknown trusted cache stage."));
}

std::string trusted_cache_diagnostic(const TrustedCacheFailure& failure) {
    if(failure.stage == TrustedCacheStage::CleanupPreflight &&
       failure.code == TrustedCacheErrorCode::Symlink &&
       !failure.system_error.has_value()) {
        return localization::format_translated_message(
            "Trusted {} cache cleanup preflight failed: symlink refused.",
            application_identity::PROJECT_NAME);
    }

    std::string diagnostic;
    switch(failure.code) {
        case TrustedCacheErrorCode::InvalidBoundary:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: invalid cache boundary.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::Symlink:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: symlink refused.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::NotDirectory:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: directory required.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::NotRegularFile:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: unsupported cache entry type.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::OwnershipMismatch:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: owner mismatch.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::UnsafePermissions:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: unsafe permissions.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::PermissionDenied:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: permission denied.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::MetadataFailure:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: metadata failure.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::ConcurrentReplacement:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: concurrent replacement.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::ChildEscape:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: child escape.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::CreationFailure:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: creation failure.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::CleanupPreflightFailure:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: cleanup preflight failure.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::RemovalFailure:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: removal failure.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
        case TrustedCacheErrorCode::RollbackRefusal:
            // TRANSLATORS: The placeholders are the project identity and a stable internal cache stage token.
            diagnostic = localization::format_translated_message(
                "The trusted {} cache operation failed during stage {}: rollback refusal.",
                application_identity::PROJECT_NAME,
                stage_name(failure.stage));
            break;
    }
    if(diagnostic.empty()) {
        throw std::logic_error(localization::translate_message(
            "Unknown trusted cache error code."));
    }
    if(failure.system_error.has_value()) {
        // TRANSLATORS: The placeholder is an operating-system error message.
        diagnostic += " " + localization::format_translated_message(
                                "System error: {}.", failure.system_error->message());
    }
    return diagnostic;
}

[[noreturn]] void throw_cache_error(
    TrustedCacheStage stage, TrustedCacheErrorCode code,
    std::optional<int> error_number = std::nullopt) {
    TrustedCacheFailure failure{stage, code, std::nullopt};
    if(error_number.has_value()) {
        failure.system_error = std::error_code(
            error_number.value(), std::generic_category());
    }
    throw TrustedCacheError(std::move(failure));
}

bool is_permission_error(int error_number) noexcept {
    return error_number == EACCES || error_number == EPERM;
}

int open_child_directory_without_mount_crossing(
    int parent_descriptor, const std::string& name) noexcept {
    struct open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    return static_cast<int>(syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
}

int open_removal_node_without_mount_crossing(
    int parent_descriptor, const std::string& name) noexcept {
    struct open_how how{};
    how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
    return static_cast<int>(syscall(
        SYS_openat2, parent_descriptor, name.c_str(), &how,
        sizeof(how)));
}

TrustedCacheErrorCode child_directory_open_error_code(
    int error_number) noexcept {
    if(error_number == EXDEV) return TrustedCacheErrorCode::ChildEscape;
    if(error_number == ELOOP) return TrustedCacheErrorCode::Symlink;
    if(error_number == ENOENT || error_number == ENOTDIR) {
        return TrustedCacheErrorCode::ConcurrentReplacement;
    }
    if(is_permission_error(error_number)) {
        return TrustedCacheErrorCode::PermissionDenied;
    }
    return TrustedCacheErrorCode::MetadataFailure;
}

std::uintmax_t status_device(const struct stat& status) noexcept {
    return static_cast<std::uintmax_t>(status.st_dev);
}

std::uintmax_t status_inode(const struct stat& status) noexcept {
    return static_cast<std::uintmax_t>(status.st_ino);
}

std::uintmax_t status_owner(const struct stat& status) noexcept {
    return static_cast<std::uintmax_t>(status.st_uid);
}

std::uintmax_t status_permissions(const struct stat& status) noexcept {
    return static_cast<std::uintmax_t>(status.st_mode & 07777);
}

bool same_identity_and_type(
    const struct stat& expected, const struct stat& actual) noexcept {
    return (expected.st_mode & S_IFMT) == (actual.st_mode & S_IFMT) &&
           expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino;
}

bool same_identity(
    const struct stat& status, std::uintmax_t expected_device,
    std::uintmax_t expected_inode) noexcept {
    return status_device(status) == expected_device &&
           status_inode(status) == expected_inode;
}

void require_safe_directory_permissions(
    const struct stat& status, TrustedCacheStage stage) {
    const mode_t permissions = status.st_mode & 07777;
    if((permissions & REQUIRED_DIRECTORY_OWNER_PERMISSIONS) !=
           REQUIRED_DIRECTORY_OWNER_PERMISSIONS ||
       (permissions & FORBIDDEN_WRITE_PERMISSIONS) != 0) {
        throw_cache_error(stage, TrustedCacheErrorCode::UnsafePermissions);
    }
}

void require_safe_direct_child_status(
    const struct stat& status, std::uintmax_t expected_owner,
    std::uintmax_t expected_device, TrustedCacheStage stage) {
    if(S_ISLNK(status.st_mode)) {
        throw_cache_error(stage, TrustedCacheErrorCode::Symlink);
    }
    if(!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) {
        throw_cache_error(stage, TrustedCacheErrorCode::NotRegularFile);
    }
    if(status_owner(status) != expected_owner) {
        throw_cache_error(stage, TrustedCacheErrorCode::OwnershipMismatch);
    }
    if(status_device(status) != expected_device) {
        throw_cache_error(stage, TrustedCacheErrorCode::ChildEscape);
    }
    if((status.st_mode & FORBIDDEN_WRITE_PERMISSIONS) != 0) {
        throw_cache_error(stage, TrustedCacheErrorCode::UnsafePermissions);
    }
    if(S_ISDIR(status.st_mode)) {
        require_safe_directory_permissions(status, stage);
    }
}

void require_safe_removal_node_status(
    const struct stat& status, std::uintmax_t expected_owner,
    std::uintmax_t expected_device, TrustedCacheStage stage) {
    if(status_device(status) != expected_device) {
        throw_cache_error(stage, TrustedCacheErrorCode::ChildEscape);
    }
    if(status_owner(status) != expected_owner) {
        throw_cache_error(stage, TrustedCacheErrorCode::OwnershipMismatch);
    }
    if(!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode) &&
       !S_ISLNK(status.st_mode)) {
        throw_cache_error(stage, TrustedCacheErrorCode::NotRegularFile);
    }
    if(!S_ISLNK(status.st_mode) &&
       (status.st_mode & FORBIDDEN_WRITE_PERMISSIONS) != 0) {
        throw_cache_error(stage, TrustedCacheErrorCode::UnsafePermissions);
    }
    if(S_ISDIR(status.st_mode)) {
        require_safe_directory_permissions(status, stage);
    }
}

OwnedFileDescriptor duplicate_descriptor(
    int descriptor, TrustedCacheStage stage) {
    const int duplicated = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if(duplicated < 0) {
        const int duplication_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(duplication_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            duplication_error);
    }
    return OwnedFileDescriptor(duplicated);
}

void require_root_unchanged(
    const ValidatedCacheRoot& root, TrustedCacheStage stage);

void rollback_staged_cache_directory_noexcept(
    const ValidatedCacheRoot& root, int root_descriptor,
    const std::string& leaf_name,
    const struct stat& expected_status) noexcept {
    try {
        require_root_unchanged(root, TrustedCacheStage::Rollback);
    } catch(...) {
        Logger::warn_noexcept([]() {
            return localization::translate_message(
                "Refusing staged cache rollback after root authority was revoked.");
        });
        return;
    }
    struct stat current_status{};
    if(fstatat(
           root_descriptor, leaf_name.c_str(), &current_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        if(errno == ENOENT) return;
        Logger::warn_noexcept([]() {
            return localization::translate_message(
                "Unable to inspect a staged trusted cache directory during rollback.");
        });
        return;
    }
    if(!same_identity_and_type(expected_status, current_status) ||
       !S_ISDIR(current_status.st_mode) ||
       status_device(expected_status) != root.device() ||
       status_device(current_status) != root.device() ||
       status_owner(expected_status) != root.owner() ||
       status_owner(current_status) != root.owner() ||
       status_owner(current_status) !=
           static_cast<std::uintmax_t>(geteuid()) ||
       status_permissions(expected_status) !=
           status_permissions(current_status) ||
       status_permissions(current_status) != 0700 ||
       (current_status.st_mode & FORBIDDEN_WRITE_PERMISSIONS) != 0) {
        Logger::warn_noexcept([]() {
            return localization::translate_message(
                "Refusing rollback of a replaced or unsafe staged trusted cache directory.");
        });
        return;
    }
    if(unlinkat(root_descriptor, leaf_name.c_str(), AT_REMOVEDIR) != 0) {
        Logger::warn_noexcept([]() {
            return localization::translate_message(
                "Unable to remove a staged trusted cache directory during rollback.");
        });
    }
}

void restore_working_directory_noexcept(int descriptor) noexcept {
    if(descriptor < 0 || fchdir(descriptor) == 0) return;
    const int restore_error = errno;
    Logger::warn_noexcept([restore_error]() {
        return localization::format_translated_message(
            "Failed to restore the working directory after a trusted cache operation: {}",
            std::string_view(std::strerror(restore_error)));
    });
}

const auto& require_root_state(
    const ValidatedCacheRoot& root, TrustedCacheStage stage) {
    const auto& state = TrustedCacheAccess::state(root);
    if(!state || state->parent_descriptor < 0 ||
       state->directory_descriptor < 0 || state->leaf_name.empty() ||
       state->retained_lineage.size() < 2) {
        throw_cache_error(stage, TrustedCacheErrorCode::InvalidBoundary);
    }
    return state;
}

void require_root_unchanged(
    const ValidatedCacheRoot& root, TrustedCacheStage stage) {
    const auto& state = require_root_state(root, stage);
    const std::uintmax_t current_effective_user =
        static_cast<std::uintmax_t>(geteuid());

    for(std::size_t index = 0; index < state->retained_lineage.size();
        ++index) {
        const auto& identity = state->retained_lineage[index];
        if(identity.descriptor < 0 ||
           (index == 0 ? !identity.leaf_name.empty()
                       : identity.leaf_name.empty())) {
            throw_cache_error(stage, TrustedCacheErrorCode::InvalidBoundary);
        }

        struct stat retained_lineage_status{};
        if(fstat(identity.descriptor, &retained_lineage_status) != 0) {
            const int metadata_error = errno;
            throw_cache_error(
                stage,
                is_permission_error(metadata_error)
                    ? TrustedCacheErrorCode::PermissionDenied
                    : TrustedCacheErrorCode::MetadataFailure,
                metadata_error);
        }
        if(!S_ISDIR(retained_lineage_status.st_mode) ||
           !same_identity(
               retained_lineage_status, identity.device,
               identity.inode)) {
            throw_cache_error(
                stage, TrustedCacheErrorCode::ConcurrentReplacement);
        }

        if(index == 0) continue;

        const auto& parent = state->retained_lineage[index - 1];
        struct stat named_lineage_status{};
        if(fstatat(
               parent.descriptor, identity.leaf_name.c_str(),
               &named_lineage_status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int metadata_error = errno;
            const bool was_replaced =
                metadata_error == ENOENT || metadata_error == ENOTDIR ||
                metadata_error == ELOOP;
            throw_cache_error(
                stage,
                was_replaced
                    ? TrustedCacheErrorCode::ConcurrentReplacement
                    : (is_permission_error(metadata_error)
                           ? TrustedCacheErrorCode::PermissionDenied
                           : TrustedCacheErrorCode::MetadataFailure),
                metadata_error);
        }
        if(!S_ISDIR(named_lineage_status.st_mode) ||
           !same_identity_and_type(
               retained_lineage_status, named_lineage_status)) {
            throw_cache_error(
                stage, TrustedCacheErrorCode::ConcurrentReplacement);
        }
        if(identity.requires_security_validation) {
            if(status_owner(retained_lineage_status) != identity.owner ||
               status_owner(named_lineage_status) != identity.owner ||
               identity.owner != current_effective_user) {
                throw_cache_error(
                    stage, TrustedCacheErrorCode::OwnershipMismatch);
            }
            require_safe_directory_permissions(
                retained_lineage_status, stage);
            require_safe_directory_permissions(named_lineage_status, stage);
        }
    }

    struct stat retained_status{};
    if(fstat(state->directory_descriptor, &retained_status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }

    struct stat retained_parent_status{};
    if(fstat(state->parent_descriptor, &retained_parent_status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }

    struct stat named_status{};
    if(fstatat(
           state->parent_descriptor, state->leaf_name.c_str(),
           &named_status, AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            metadata_error == ENOENT
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : (is_permission_error(metadata_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::MetadataFailure),
            metadata_error);
    }

    const auto& lineage_parent =
        state->retained_lineage[state->retained_lineage.size() - 2];
    const auto& lineage_directory = state->retained_lineage.back();
    if(lineage_directory.leaf_name != state->leaf_name ||
       !S_ISDIR(retained_status.st_mode) ||
       !S_ISDIR(retained_parent_status.st_mode) ||
       !same_identity_and_type(retained_status, named_status) ||
       !same_identity(retained_status, state->device, state->inode) ||
       !same_identity(
           retained_status, lineage_directory.device,
           lineage_directory.inode) ||
       !same_identity(
           retained_parent_status, lineage_parent.device,
           lineage_parent.inode)) {
        throw_cache_error(
            stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    if(status_owner(retained_status) != state->owner ||
       status_owner(named_status) != state->owner ||
       state->owner != static_cast<std::uintmax_t>(geteuid())) {
        throw_cache_error(stage, TrustedCacheErrorCode::OwnershipMismatch);
    }
    require_safe_directory_permissions(retained_status, stage);
    require_safe_directory_permissions(named_status, stage);
}

std::string validated_child_leaf(
    const ValidatedCacheRoot& root, const fs::path& raw_path) {
    if(raw_path.empty()) {
        throw_cache_error(
            TrustedCacheStage::ChildValidation,
            TrustedCacheErrorCode::ChildEscape);
    }
    const std::string native = raw_path.string();
    if(native.find('\0') != std::string::npos) {
        throw_cache_error(
            TrustedCacheStage::ChildValidation,
            TrustedCacheErrorCode::ChildEscape);
    }

    fs::path leaf_path;
    if(raw_path.is_absolute()) {
        if(raw_path.parent_path() != root.path()) {
            throw_cache_error(
                TrustedCacheStage::ChildValidation,
                TrustedCacheErrorCode::ChildEscape);
        }
        leaf_path = raw_path.filename();
    } else {
        auto component = raw_path.begin();
        if(component == raw_path.end()) {
            throw_cache_error(
                TrustedCacheStage::ChildValidation,
                TrustedCacheErrorCode::ChildEscape);
        }
        leaf_path = *component;
        ++component;
        if(component != raw_path.end()) {
            throw_cache_error(
                TrustedCacheStage::ChildValidation,
                TrustedCacheErrorCode::ChildEscape);
        }
    }

    const std::string leaf = leaf_path.string();
    if(leaf.empty() || leaf == "." || leaf == ".." ||
       leaf.find('/') != std::string::npos ||
       leaf.find('\0') != std::string::npos) {
        throw_cache_error(
            TrustedCacheStage::ChildValidation,
            TrustedCacheErrorCode::ChildEscape);
    }
    return leaf;
}

ValidatedCachePath inspect_cache_child(
    const ValidatedCacheRoot& root, const fs::path& raw_path,
    CachePathRequirement requirement) {
    require_root_unchanged(root, TrustedCacheStage::RootRevalidation);
    const auto& state = require_root_state(
        root, TrustedCacheStage::ChildValidation);
    const std::string leaf = validated_child_leaf(root, raw_path);

    struct stat status{};
    if(fstatat(
           state->directory_descriptor, leaf.c_str(), &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        if(metadata_error != ENOENT) {
            throw_cache_error(
                TrustedCacheStage::ChildValidation,
                is_permission_error(metadata_error)
                    ? TrustedCacheErrorCode::PermissionDenied
                    : TrustedCacheErrorCode::MetadataFailure,
                metadata_error);
        }
        if(requirement == CachePathRequirement::Existing ||
           requirement == CachePathRequirement::ExistingDirectory) {
            throw_cache_error(
                TrustedCacheStage::ChildValidation,
                TrustedCacheErrorCode::ConcurrentReplacement,
                metadata_error);
        }
        return TrustedCacheAccess::make_path(
            root, root.path() / leaf, leaf, false, false, 0, 0, 0, 0);
    }

    if(requirement == CachePathRequirement::Missing) {
        throw_cache_error(
            TrustedCacheStage::ChildValidation,
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    require_safe_direct_child_status(
        status, state->owner, state->device,
        TrustedCacheStage::ChildValidation);
    if(requirement == CachePathRequirement::ExistingDirectory &&
       !S_ISDIR(status.st_mode)) {
        throw_cache_error(
            TrustedCacheStage::ChildValidation,
            TrustedCacheErrorCode::NotDirectory);
    }

    return TrustedCacheAccess::make_path(
        root, root.path() / leaf, leaf, true, S_ISDIR(status.st_mode),
        status_device(status), status_inode(status), status_owner(status),
        status_permissions(status));
}

void require_same_cache_path_identity(
    const ValidatedCachePath& expected,
    const ValidatedCachePath& current,
    TrustedCacheStage stage) {
    if(expected.exists() != current.exists()) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    if(!expected.exists()) return;
    if(expected.is_directory() != current.is_directory() ||
       TrustedCacheAccess::device(expected) !=
           TrustedCacheAccess::device(current) ||
       TrustedCacheAccess::inode(expected) !=
           TrustedCacheAccess::inode(current) ||
       TrustedCacheAccess::owner(expected) !=
           TrustedCacheAccess::owner(current)) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
}

OwnedFileDescriptor open_validated_child_directory(
    const ValidatedCachePath& expected, TrustedCacheStage stage) {
    ValidatedCachePath current = inspect_cache_child(
        TrustedCacheAccess::root(expected),
        TrustedCacheAccess::leaf(expected),
        CachePathRequirement::ExistingDirectory);
    require_same_cache_path_identity(expected, current, stage);

    const auto& root_state = require_root_state(
        TrustedCacheAccess::root(expected), stage);
    const int descriptor = open_child_directory_without_mount_crossing(
        root_state->directory_descriptor,
        TrustedCacheAccess::leaf(expected));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_cache_error(
            stage,
            child_directory_open_error_code(open_error),
            open_error);
    }
    OwnedFileDescriptor opened(descriptor);

    struct stat opened_status{};
    struct stat named_status{};
    if(fstat(opened.get(), &opened_status) != 0 ||
       fstatat(
           root_state->directory_descriptor,
           TrustedCacheAccess::leaf(expected).c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!S_ISDIR(opened_status.st_mode) ||
       !same_identity_and_type(opened_status, named_status) ||
       !same_identity(
           opened_status, TrustedCacheAccess::device(expected),
           TrustedCacheAccess::inode(expected))) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    require_safe_direct_child_status(
        opened_status, root_state->owner, root_state->device, stage);
    require_safe_direct_child_status(
        named_status, root_state->owner, root_state->device, stage);
    return opened;
}

enum class RemovalHandleQueryMode {
    FileIdentity,
    Standard,
};

struct RemovalGeneration {
    RemovalHandleQueryMode query_mode = RemovalHandleQueryMode::FileIdentity;
    int mount_id = 0;
    int handle_type = 0;
    // The vector length is the exact returned handle length, without padding.
    std::vector<unsigned char> bytes;

    bool operator==(const RemovalGeneration&) const = default;
};

struct RemovalNode {
    std::string name;
    struct stat status{};
    // Unlike a stat tuple, the filesystem handle distinguishes inode reuse
    // after a preflight FD closes. It never authorizes handle-relative removal.
    RemovalGeneration generation;
    std::vector<RemovalNode> children;
};

struct RemovalPlan {
    ValidatedCachePath target;
    RemovalNode root;
};

std::vector<std::string> directory_entry_names(
    int directory_descriptor, TrustedCacheStage stage) {
    struct stat retained_status{};
    if(fstat(directory_descriptor, &retained_status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    const int scan_descriptor = openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        const int open_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(open_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            open_error);
    }
    OwnedFileDescriptor scan(scan_descriptor);
    struct stat scan_status{};
    if(fstat(scan.get(), &scan_status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage, TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!same_identity_and_type(retained_status, scan_status)) {
        throw_cache_error(
            stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }

    DIR* directory = fdopendir(scan.get());
    if(directory == nullptr) {
        const int open_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(open_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            open_error);
    }
    static_cast<void>(scan.release());
    std::unique_ptr<DIR, DirectoryStreamCloser> owned_directory(directory);

    std::vector<std::string> names;
    errno = 0;
    while(dirent* entry = readdir(owned_directory.get())) {
        const std::string name(entry->d_name);
        if(name != "." && name != "..") names.push_back(name);
        errno = 0;
    }
    const int read_error = errno;
    DIR* raw_directory = owned_directory.release();
    const int close_status = closedir(raw_directory);
    const int close_error = close_status == 0 ? 0 : errno;
    if(read_error == 0 && close_status != 0) {
        throw_cache_error(stage, TrustedCacheErrorCode::MetadataFailure,
                          close_error);
    }
    if(read_error != 0) {
        throw_cache_error(
            stage,
            is_permission_error(read_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            read_error);
    }
    std::sort(names.begin(), names.end());
    return names;
}

RemovalGeneration observe_removal_generation(
    int descriptor, TrustedCacheStage stage,
    std::optional<RemovalHandleQueryMode> expected_mode = std::nullopt) {
    RemovalHandleQueryMode mode =
        expected_mode.value_or(RemovalHandleQueryMode::FileIdentity);
    const auto query = [&](struct file_handle* handle, int* mount_id) {
        const int flags = AT_EMPTY_PATH |
                          (mode == RemovalHandleQueryMode::FileIdentity
                               ? AT_HANDLE_FID
                               : 0);
        return name_to_handle_at(descriptor, "", handle, mount_id, flags);
    };
    struct file_handle sizing{};
    int mount_id = 0;
    int probe_result = query(&sizing, &mount_id);
    int probe_error = probe_result < 0 ? errno : 0;
    // Only the initial zero-sized FID probe may negotiate the older API.
    // Revalidation uses the sealed mode, even if another mode would succeed.
    if(!expected_mode.has_value() && probe_result < 0 &&
       probe_error == EINVAL) {
        mode = RemovalHandleQueryMode::Standard;
        sizing = {};
        probe_result = query(&sizing, &mount_id);
        probe_error = probe_result < 0 ? errno : 0;
    }
    if(probe_result < 0 && probe_error != EOVERFLOW) {
        throw_cache_error(stage, TrustedCacheErrorCode::MetadataFailure,
                          probe_error);
    }
    if(probe_result != -1 || sizing.handle_bytes == 0 ||
       sizing.handle_bytes > MAX_HANDLE_SZ) {
        throw_cache_error(stage, TrustedCacheErrorCode::MetadataFailure);
    }
    const unsigned int requested = sizing.handle_bytes;
    // calloc provides aligned storage for the C flexible-array object.
    // Never truncate an unsupported handle or fall back to stat/full-tree pins.
    std::unique_ptr<struct file_handle, decltype(&std::free)> handle(
        static_cast<struct file_handle*>(
            std::calloc(1, sizeof(struct file_handle) + requested)),
        &std::free);
    if(!handle) throw std::bad_alloc();
    handle->handle_bytes = requested;
    int observed_mount_id = 0;
    if(query(handle.get(), &observed_mount_id) != 0) {
        const int handle_error = errno;
        throw_cache_error(stage, TrustedCacheErrorCode::MetadataFailure,
                          handle_error);
    }
    // FILEID_INVALID (255) cannot represent a successful filesystem handle.
    constexpr int INVALID_FILE_HANDLE_TYPE = 255;
    if(handle->handle_bytes != requested || handle->handle_type <= 0 ||
       handle->handle_type == INVALID_FILE_HANDLE_TYPE) {
        throw_cache_error(stage, TrustedCacheErrorCode::MetadataFailure);
    }
    if(observed_mount_id != mount_id) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    return RemovalGeneration{
        mode, mount_id, handle->handle_type,
        std::vector<unsigned char>(
            handle->f_handle, handle->f_handle + requested)};
}

void require_expected_removal_node_status(
    const RemovalNode& node, const struct stat& current_status,
    std::uintmax_t root_device, std::uintmax_t root_owner,
    TrustedCacheStage stage) {
    if(!same_identity_and_type(node.status, current_status)) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    if(status_device(current_status) != root_device) {
        throw_cache_error(stage, TrustedCacheErrorCode::ChildEscape);
    }
    if(status_owner(current_status) != status_owner(node.status) ||
       status_owner(current_status) != root_owner) {
        throw_cache_error(stage, TrustedCacheErrorCode::OwnershipMismatch);
    }
    require_safe_removal_node_status(
        current_status, root_owner, root_device, stage);
    if(status_permissions(current_status) !=
       status_permissions(node.status)) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
}

struct stat removal_descriptor_status(int descriptor, TrustedCacheStage stage) {
    struct stat status{};
    if(fstat(descriptor, &status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    return status;
}

void require_named_removal_node_identity(
    int parent_descriptor, const RemovalNode& node, int opened_descriptor,
    std::uintmax_t root_device, std::uintmax_t root_owner,
    TrustedCacheStage stage) {
    const struct stat opened_status =
        removal_descriptor_status(opened_descriptor, stage);
    require_expected_removal_node_status(
        node, opened_status, root_device, root_owner, stage);
    if(observe_removal_generation(
           opened_descriptor, stage, node.generation.query_mode) !=
       node.generation) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    struct stat named_status{};
    if(fstatat(
           parent_descriptor, node.name.c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            metadata_error == ENOENT
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : (is_permission_error(metadata_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::MetadataFailure),
            metadata_error);
    }
    require_expected_removal_node_status(
        node, named_status, root_device, root_owner, stage);
    if(!same_identity_and_type(opened_status, named_status) ||
       status_owner(opened_status) != status_owner(named_status) ||
       status_permissions(opened_status) != status_permissions(named_status)) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
}

OwnedFileDescriptor open_verified_removal_node(
    int parent_descriptor, const RemovalNode& node,
    std::uintmax_t root_device, std::uintmax_t root_owner,
    TrustedCacheStage stage, bool directory_reader = false) {
    const int descriptor =
        directory_reader
            ? open_child_directory_without_mount_crossing(
                  parent_descriptor, node.name)
            : open_removal_node_without_mount_crossing(
                  parent_descriptor, node.name);
    if(descriptor < 0) {
        const int open_error = errno;
        throw_cache_error(
            stage,
            open_error == ELOOP
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : child_directory_open_error_code(open_error),
            open_error);
    }
    OwnedFileDescriptor opened(descriptor);
    require_named_removal_node_identity(
        parent_descriptor, node, opened.get(), root_device, root_owner, stage);
    return opened;
}

void require_removal_directory_inventory(
    int descriptor, const RemovalNode& node, TrustedCacheStage stage) {
    std::vector<std::string> expected_names;
    expected_names.reserve(node.children.size());
    for(const RemovalNode& child : node.children) {
        expected_names.push_back(child.name);
    }
    if(directory_entry_names(descriptor, stage) != expected_names) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
}

RemovalNode preflight_removal_node(
    int parent_descriptor, const std::string& name,
    std::uintmax_t root_device, std::uintmax_t root_owner,
    TrustedCacheStage stage) {
    RemovalNode node;
    node.name = name;
    {
        if(fstatat(
               parent_descriptor, name.c_str(), &node.status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            const int metadata_error = errno;
            throw_cache_error(
                stage,
                metadata_error == ENOENT
                    ? TrustedCacheErrorCode::ConcurrentReplacement
                    : (is_permission_error(metadata_error)
                           ? TrustedCacheErrorCode::PermissionDenied
                           : TrustedCacheErrorCode::MetadataFailure),
                metadata_error);
        }
        require_safe_removal_node_status(
            node.status, root_owner, root_device, stage);
        const int descriptor = open_removal_node_without_mount_crossing(
            parent_descriptor, name);
        if(descriptor < 0) {
            const int open_error = errno;
            throw_cache_error(
                stage,
                open_error == ELOOP
                    ? TrustedCacheErrorCode::ConcurrentReplacement
                    : child_directory_open_error_code(open_error),
                open_error);
        }
        OwnedFileDescriptor pin(descriptor);
        require_expected_removal_node_status(
            node, removal_descriptor_status(pin.get(), stage),
            root_device, root_owner, stage);
        node.generation = observe_removal_generation(pin.get(), stage);
        require_named_removal_node_identity(
            parent_descriptor, node, pin.get(), root_device, root_owner, stage);
    }
    // The initial pin ends before descending. Only one directory reader per
    // recursive frame remains live; the returned tree contains no node FDs.
    if(!S_ISDIR(node.status.st_mode)) return node;
    OwnedFileDescriptor directory = open_verified_removal_node(
        parent_descriptor, node, root_device, root_owner, stage, true);
    for(const std::string& child_name :
        directory_entry_names(directory.get(), stage)) {
        node.children.push_back(preflight_removal_node(
            directory.get(), child_name, root_device, root_owner, stage));
    }
    require_named_removal_node_identity(
        parent_descriptor, node, directory.get(), root_device, root_owner, stage);
    require_removal_directory_inventory(directory.get(), node, stage);
    return node;
}

void revalidate_removal_node(
    int parent_descriptor, const RemovalNode& node,
    std::uintmax_t root_device, std::uintmax_t root_owner,
    TrustedCacheStage stage) {
    const bool is_directory = S_ISDIR(node.status.st_mode);
    OwnedFileDescriptor opened = open_verified_removal_node(
        parent_descriptor, node, root_device, root_owner, stage, is_directory);
    if(!is_directory) return;
    require_removal_directory_inventory(opened.get(), node, stage);
    for(const RemovalNode& child : node.children) {
        revalidate_removal_node(
            opened.get(), child, root_device, root_owner, stage);
    }
    require_named_removal_node_identity(
        parent_descriptor, node, opened.get(), root_device, root_owner, stage);
    require_removal_directory_inventory(opened.get(), node, stage);
}

using RemovalDirectoryLineage = std::vector<const RemovalNode*>;

OwnedFileDescriptor open_removal_parent_lineage(
    const ValidatedCacheRoot& root,
    const RemovalDirectoryLineage& lineage,
    TrustedCacheStage stage) {
    require_root_unchanged(root, stage);
    const auto& root_state = require_root_state(root, stage);
    OwnedFileDescriptor current = duplicate_descriptor(
        root_state->directory_descriptor, stage);
    for(const RemovalNode* ancestor : lineage) {
        if(ancestor == nullptr || !S_ISDIR(ancestor->status.st_mode)) {
            throw_cache_error(stage, TrustedCacheErrorCode::InvalidBoundary);
        }
        current = open_verified_removal_node(
            current.get(), *ancestor, root_state->device,
            root_state->owner, stage, true);
    }
    return current;
}

void remove_removal_node(
    const ValidatedCacheRoot& root, const RemovalNode& node,
    RemovalDirectoryLineage& lineage) {
    notify_removal_test_hook(lineage.size());
    const auto& root_state = require_root_state(
        root, TrustedCacheStage::RecursiveRemoval);
    {
        OwnedFileDescriptor parent = open_removal_parent_lineage(
            root, lineage, TrustedCacheStage::RecursiveRemoval);
        revalidate_removal_node(
            parent.get(), node, root_state->device, root_state->owner,
            TrustedCacheStage::RecursiveRemoval);
    }

    const bool is_directory = S_ISDIR(node.status.st_mode);
    if(is_directory) {
        lineage.push_back(&node);
        try {
            for(const RemovalNode& child : node.children) {
                remove_removal_node(root, child, lineage);
            }
        } catch(...) {
            lineage.pop_back();
            throw;
        }
        lineage.pop_back();
    }

    // Generation evidence identifies the original, but only the freshly
    // rebuilt named lineage authorizes deletion. Never find a moved original
    // via its opaque handle. Keep this local pin through unlinkat().
    OwnedFileDescriptor parent = open_removal_parent_lineage(
        root, lineage, TrustedCacheStage::RecursiveRemoval);
    OwnedFileDescriptor pin = open_verified_removal_node(
        parent.get(), node, root_state->device, root_state->owner,
        TrustedCacheStage::RecursiveRemoval);
    if(is_directory) {
        OwnedFileDescriptor directory = open_verified_removal_node(
            parent.get(), node, root_state->device, root_state->owner,
            TrustedCacheStage::RecursiveRemoval, true);
        if(!directory_entry_names(
                directory.get(), TrustedCacheStage::RecursiveRemoval)
                .empty()) {
            throw_cache_error(TrustedCacheStage::RecursiveRemoval,
                              TrustedCacheErrorCode::ConcurrentReplacement);
        }
    }
    require_named_removal_node_identity(
        parent.get(), node, pin.get(), root_state->device, root_state->owner,
        TrustedCacheStage::RecursiveRemoval);
    // Linux has no atomic compare-and-unlink operation. Observed replacement
    // fails closed; hostile same-euid mutation after this check remains possible.
    if(unlinkat(
           parent.get(), node.name.c_str(), is_directory ? AT_REMOVEDIR : 0) != 0) {
        const int removal_error = errno;
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            removal_error == ENOENT || (is_directory && removal_error == ENOTEMPTY)
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : (is_permission_error(removal_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::RemovalFailure),
            removal_error);
    }
}

OwnedFileDescriptor acquire_removal_lease(
    const ValidatedCachePath& target, TrustedCacheStage stage,
    const RemovalNode* expected = nullptr) {
    if(!target.is_directory()) return OwnedFileDescriptor{};
    const auto& root_state = require_root_state(
        TrustedCacheAccess::root(target), stage);
    require_root_unchanged(TrustedCacheAccess::root(target), stage);
    const int descriptor = open_child_directory_without_mount_crossing(
        root_state->directory_descriptor, TrustedCacheAccess::leaf(target));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_cache_error(
            stage,
            open_error == ELOOP
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : child_directory_open_error_code(open_error),
            open_error);
    }
    OwnedFileDescriptor lease(descriptor);
    int lock_result;
    do {
        lock_result = flock(lease.get(), LOCK_EX | LOCK_NB);
    } while(lock_result != 0 && errno == EINTR);
    if(lock_result != 0) {
        const int lock_error = errno;
        throw_cache_error(
            stage,
            lock_error == EWOULDBLOCK || lock_error == EAGAIN
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : (is_permission_error(lock_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::MetadataFailure),
            lock_error);
    }
    struct stat named_status{};
    if(fstatat(root_state->directory_descriptor,
               TrustedCacheAccess::leaf(target).c_str(), &named_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            stage,
            metadata_error == ENOENT
                ? TrustedCacheErrorCode::ConcurrentReplacement
                : (is_permission_error(metadata_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::MetadataFailure),
            metadata_error);
    }
    const struct stat status = removal_descriptor_status(lease.get(), stage);
    if(!same_identity(status, target.device(), target.inode()) ||
       !same_identity_and_type(status, named_status) ||
       status_owner(status) != status_owner(named_status) ||
       status_permissions(status) != status_permissions(named_status)) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    require_safe_direct_child_status(
        status, root_state->owner, root_state->device, stage);
    require_safe_direct_child_status(
        named_status, root_state->owner, root_state->device, stage);
    if(expected != nullptr) {
        require_named_removal_node_identity(
            root_state->directory_descriptor, *expected, lease.get(),
            root_state->device, root_state->owner, stage);
    }
    return lease;
}

RemovalPlan make_removal_plan(
    const ValidatedCachePath& expected, TrustedCacheStage stage) {
    if(!expected.exists()) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    ValidatedCachePath current = inspect_cache_child(
        TrustedCacheAccess::root(expected),
        TrustedCacheAccess::leaf(expected),
        expected.is_directory() ? CachePathRequirement::ExistingDirectory
                                : CachePathRequirement::Existing);
    require_same_cache_path_identity(expected, current, stage);
    const auto& root_state = require_root_state(
        TrustedCacheAccess::root(expected), stage);
    RemovalNode root_node = preflight_removal_node(
        root_state->directory_descriptor,
        TrustedCacheAccess::leaf(expected), root_state->device,
        root_state->owner, stage);
    if(!same_identity(root_node.status, expected.device(), expected.inode())) {
        throw_cache_error(stage, TrustedCacheErrorCode::ConcurrentReplacement);
    }
    return RemovalPlan{expected, std::move(root_node)};
}

void require_cleanup_root_lineage(
    const ValidatedCacheRoot& expected_root,
    const std::vector<RemovalPlan>& plans) {
    for(const RemovalPlan& plan : plans) {
        const ValidatedCacheRoot& root =
            TrustedCacheAccess::root(plan.target);
        if(root.device() != expected_root.device() ||
           root.inode() != expected_root.inode()) {
            throw_cache_error(
                TrustedCacheStage::CleanupPreflight,
                TrustedCacheErrorCode::InvalidBoundary);
        }
    }
}

[[noreturn]] void throw_cleanup_preflight_failure(
    const TrustedCacheError& error) {
    throw TrustedCacheError(TrustedCacheFailure{
        TrustedCacheStage::CleanupPreflight,
        error.failure().code,
        error.failure().system_error});
}

bool rollback_trusted_cache_path(
    const ValidatedCachePath& expected, int retained_descriptor,
    std::uintmax_t retained_device, std::uintmax_t retained_inode,
    std::uintmax_t retained_owner,
    std::uintmax_t retained_permissions) {
    try {
        const auto& root_state = require_root_state(
            TrustedCacheAccess::root(expected),
            TrustedCacheStage::Rollback);
        struct stat retained_status{};
        if(retained_descriptor < 0 ||
           fstat(retained_descriptor, &retained_status) != 0) {
            const int metadata_error = retained_descriptor < 0 ? EBADF : errno;
            throw_cache_error(
                TrustedCacheStage::Rollback,
                TrustedCacheErrorCode::RollbackRefusal,
                metadata_error);
        }
        if(!S_ISDIR(retained_status.st_mode) ||
           status_device(retained_status) != retained_device ||
           status_inode(retained_status) != retained_inode ||
           status_owner(retained_status) != retained_owner ||
           status_permissions(retained_status) != retained_permissions ||
           retained_device != root_state->device ||
           retained_owner != root_state->owner ||
           retained_owner != static_cast<std::uintmax_t>(geteuid()) ||
           retained_permissions != 0700) {
            throw_cache_error(
                TrustedCacheStage::Rollback,
                TrustedCacheErrorCode::RollbackRefusal);
        }

        struct stat named_status{};
        if(fstatat(
               root_state->directory_descriptor,
               TrustedCacheAccess::leaf(expected).c_str(), &named_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            if(errno == ENOENT && retained_status.st_nlink == 0) return false;
            const int metadata_error = errno;
            throw_cache_error(
                TrustedCacheStage::Rollback,
                TrustedCacheErrorCode::RollbackRefusal,
                metadata_error);
        }
        if(!same_identity_and_type(retained_status, named_status) ||
           !S_ISDIR(named_status.st_mode) ||
           status_owner(named_status) != retained_owner ||
           status_permissions(named_status) != retained_permissions ||
           !same_identity(
               named_status, TrustedCacheAccess::device(expected),
               TrustedCacheAccess::inode(expected))) {
            throw_cache_error(
                TrustedCacheStage::Rollback,
                TrustedCacheErrorCode::RollbackRefusal);
        }
        RemovalPlan plan = make_removal_plan(
            expected, TrustedCacheStage::Rollback);
        revalidate_removal_node(
            root_state->directory_descriptor, plan.root,
            root_state->device, root_state->owner,
            TrustedCacheStage::Rollback);
        RemovalDirectoryLineage lineage;
        remove_removal_node(
            TrustedCacheAccess::root(expected), plan.root, lineage);
        return true;
    } catch(const TrustedCacheError& error) {
        if(error.failure().code == TrustedCacheErrorCode::RollbackRefusal) {
            throw;
        }
        throw TrustedCacheError(TrustedCacheFailure{
            TrustedCacheStage::Rollback,
            TrustedCacheErrorCode::RollbackRefusal,
            error.failure().system_error});
    }
}

} // namespace

struct PreparedCacheCleanup::State {
    ValidatedCacheRoot root;
    std::vector<RemovalPlan> plans;

    State(
        ValidatedCacheRoot trusted_root,
        std::vector<RemovalPlan> removal_plans) noexcept
        : root(std::move(trusted_root)), plans(std::move(removal_plans)) {
    }
};

TrustedCacheError::TrustedCacheError(TrustedCacheFailure failure)
    : std::runtime_error(trusted_cache_diagnostic(failure)),
      failure_(std::move(failure)) {
}

struct TrustedCacheDirectoryAccess {
    static ValidatedCacheRoot adopt(
        const xdg_paths::CachePaths& paths,
        xdg_directory_safety::PreparedDirectory directory) {
        if(directory.directory_kind_ != xdg_paths::DirectoryKind::Cache ||
           directory.path_ != paths.directory ||
           directory.parent_descriptor_ < 0 ||
           directory.directory_descriptor_ < 0 ||
           directory.retained_lineage_.size() < 2 ||
           paths.creation_boundary.creatable_components.empty() ||
           directory.leaf_name_ !=
               paths.creation_boundary.creatable_components.back()) {
            throw_cache_error(
                TrustedCacheStage::RootAdoption,
                TrustedCacheErrorCode::InvalidBoundary);
        }
        directory.require_unchanged_identity();

        // Mutation用root descriptorだけではancestor renameを検出できない。
        // PreparedDirectoryのprivate lineageを複製し、capability lifetime全体で
        // filesystem-root-relativeのnamed linkを再証明する。
        std::vector<ValidatedCacheRoot::State::RetainedDirectoryIdentity>
            retained_lineage;
        retained_lineage.reserve(directory.retained_lineage_.size());
        for(const auto& identity : directory.retained_lineage_) {
            OwnedFileDescriptor duplicated = duplicate_descriptor(
                identity.descriptor, TrustedCacheStage::RootAdoption);
            retained_lineage.emplace_back(
                duplicated.get(), identity.leaf_name,
                identity.device, identity.inode,
                identity.filesystem_owner,
                identity.requires_security_validation);
            static_cast<void>(duplicated.release());
        }

        OwnedFileDescriptor parent = duplicate_descriptor(
            directory.parent_descriptor_,
            TrustedCacheStage::RootAdoption);
        const int root_descriptor = openat(
            directory.directory_descriptor_, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if(root_descriptor < 0) {
            const int open_error = errno;
            throw_cache_error(
                TrustedCacheStage::RootAdoption,
                is_permission_error(open_error)
                    ? TrustedCacheErrorCode::PermissionDenied
                    : TrustedCacheErrorCode::MetadataFailure,
                open_error);
        }
        OwnedFileDescriptor root(root_descriptor);

        struct stat status{};
        if(fstat(root.get(), &status) != 0) {
            const int metadata_error = errno;
            throw_cache_error(
                TrustedCacheStage::RootAdoption,
                TrustedCacheErrorCode::MetadataFailure,
                metadata_error);
        }
        if(!S_ISDIR(status.st_mode) ||
           status_device(status) != directory.device_ ||
           status_inode(status) != directory.inode_) {
            throw_cache_error(
                TrustedCacheStage::RootAdoption,
                TrustedCacheErrorCode::ConcurrentReplacement);
        }
        if(status_owner(status) != directory.filesystem_owner_) {
            throw_cache_error(
                TrustedCacheStage::RootAdoption,
                TrustedCacheErrorCode::OwnershipMismatch);
        }
        require_safe_directory_permissions(
            status, TrustedCacheStage::RootAdoption);

        auto state = std::make_shared<ValidatedCacheRoot::State>(
            paths.directory, parent.get(), root.get(),
            directory.leaf_name_, status_device(status),
            status_inode(status), status_owner(status),
            status_permissions(status),
            std::move(retained_lineage));
        static_cast<void>(parent.release());
        static_cast<void>(root.release());
        ValidatedCacheRoot adopted =
            TrustedCacheAccess::make_root(std::move(state));
        require_root_unchanged(
            adopted, TrustedCacheStage::RootAdoption);
        return adopted;
    }
};

const fs::path& ValidatedCacheRoot::path() const noexcept {
    static const fs::path EMPTY_PATH;
    return state_ ? state_->path : EMPTY_PATH;
}

const fs::path& ValidatedCacheRoot::canonical_path() const noexcept {
    return path();
}

std::uintmax_t ValidatedCacheRoot::device() const noexcept {
    return state_ ? state_->device : 0;
}

std::uintmax_t ValidatedCacheRoot::inode() const noexcept {
    return state_ ? state_->inode : 0;
}

std::uintmax_t ValidatedCacheRoot::owner() const noexcept {
    return state_ ? state_->owner : 0;
}

void ValidatedCacheRoot::require_unchanged_identity() const {
    require_root_unchanged(*this, TrustedCacheStage::RootRevalidation);
}

ValidatedPrivateCacheRoot::ValidatedPrivateCacheRoot(
    ValidatedCacheRoot trusted_root, int directory_descriptor,
    std::uintmax_t device, std::uintmax_t inode,
    std::uintmax_t owner) noexcept
    : trusted_root_(std::move(trusted_root)),
      directory_descriptor_(directory_descriptor), device_(device),
      inode_(inode), owner_(owner) {
}

ValidatedPrivateCacheRoot::ValidatedPrivateCacheRoot(
    ValidatedPrivateCacheRoot&& other) noexcept
    : trusted_root_(std::move(other.trusted_root_)),
      directory_descriptor_(std::exchange(other.directory_descriptor_, -1)),
      device_(other.device_), inode_(other.inode_), owner_(other.owner_) {
}

ValidatedPrivateCacheRoot::~ValidatedPrivateCacheRoot() noexcept {
    if(directory_descriptor_ >= 0) {
        static_cast<void>(close(directory_descriptor_));
    }
}

void ValidatedPrivateCacheRoot::require_unchanged_identity_for_owner(
    std::uintmax_t expected_effective_user) const {
    trusted_root_.require_unchanged_identity();
    if(directory_descriptor_ < 0) {
        throw_cache_error(
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::InvalidBoundary);
    }
    struct stat status{};
    if(fstat(directory_descriptor_, &status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!S_ISDIR(status.st_mode) || !same_identity(status, device_, inode_)) {
        throw_cache_error(
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    if(status_owner(status) != owner_ || owner_ != expected_effective_user) {
        throw_cache_error(
            TrustedCacheStage::RootRevalidation,
            TrustedCacheErrorCode::OwnershipMismatch);
    }
    require_safe_directory_permissions(
        status, TrustedCacheStage::RootRevalidation);
}

void ValidatedPrivateCacheRoot::require_unchanged_identity() const {
    require_unchanged_identity_for_owner(
        static_cast<std::uintmax_t>(geteuid()));
}

ValidatedCacheRoot adopt_trusted_cache_root(
    const xdg_paths::CachePaths& paths,
    xdg_directory_safety::PreparedDirectory directory) {
    return TrustedCacheDirectoryAccess::adopt(
        paths, std::move(directory));
}

ValidatedPrivateCacheRoot prepare_private_trusted_cache_root(
    const ValidatedCacheRoot& root) {
    root.require_unchanged_identity();
    const auto& state = require_root_state(
        root, TrustedCacheStage::RootAdoption);
    OwnedFileDescriptor duplicated = duplicate_descriptor(
        state->directory_descriptor, TrustedCacheStage::RootAdoption);
    struct stat status{};
    if(fstat(duplicated.get(), &status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            TrustedCacheStage::RootAdoption,
            TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!S_ISDIR(status.st_mode) ||
       !same_identity(status, state->device, state->inode)) {
        throw_cache_error(
            TrustedCacheStage::RootAdoption,
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    ValidatedPrivateCacheRoot private_root(
        root, duplicated.get(), status_device(status),
        status_inode(status), status_owner(status));
    static_cast<void>(duplicated.release());
    return private_root;
}

#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
void require_private_cache_root_identity_for_test(
    const ValidatedPrivateCacheRoot& root,
    std::uintmax_t expected_effective_user) {
    root.require_unchanged_identity_for_owner(expected_effective_user);
}

void set_trusted_cache_removal_test_hook(
    TrustedCacheRemovalTestHook hook) {
    removal_test_hook = std::move(hook);
}

void set_trusted_cache_staged_directory_test_hook(
    TrustedCacheStagedDirectoryTestHook hook) {
    staged_directory_test_hook = std::move(hook);
}
#endif

ValidatedCachePath require_trusted_cache_path(
    const ValidatedCacheRoot& root, const fs::path& path,
    CachePathRequirement requirement) {
    return inspect_cache_child(root, path, requirement);
}

ValidatedCachePath revalidate_trusted_cache_path(
    const ValidatedCachePath& expected,
    CachePathRequirement requirement) {
    ValidatedCachePath current = inspect_cache_child(
        TrustedCacheAccess::root(expected),
        TrustedCacheAccess::leaf(expected), requirement);
    require_same_cache_path_identity(
        expected, current, TrustedCacheStage::ChildValidation);
    return current;
}

RetainedTrustedCacheDirectory::RetainedTrustedCacheDirectory(
    ValidatedCachePath path, int descriptor,
    std::uintmax_t device, std::uintmax_t inode) noexcept
    : path_(std::move(path)), descriptor_(descriptor), device_(device),
      inode_(inode) {
}

RetainedTrustedCacheDirectory::RetainedTrustedCacheDirectory(
    RetainedTrustedCacheDirectory&& other) noexcept
    : path_(std::move(other.path_)),
      descriptor_(std::exchange(other.descriptor_, -1)),
      device_(other.device_), inode_(other.inode_) {
}

RetainedTrustedCacheDirectory::~RetainedTrustedCacheDirectory() noexcept {
    if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
}

void RetainedTrustedCacheDirectory::require_unchanged_identity() const {
    if(descriptor_ < 0) {
        throw_cache_error(
            TrustedCacheStage::ChildOpen,
            TrustedCacheErrorCode::InvalidBoundary);
    }
    ValidatedCachePath current = revalidate_trusted_cache_path(
        path_, CachePathRequirement::ExistingDirectory);
    struct stat retained_status{};
    if(fstat(descriptor_, &retained_status) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            TrustedCacheStage::ChildOpen,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!S_ISDIR(retained_status.st_mode) ||
       !same_identity(retained_status, device_, inode_) ||
       current.device() != device_ || current.inode() != inode_) {
        throw_cache_error(
            TrustedCacheStage::ChildOpen,
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
    const auto& root_state = require_root_state(
        TrustedCacheAccess::root(current),
        TrustedCacheStage::ChildOpen);
    require_safe_direct_child_status(
        retained_status, root_state->owner, root_state->device,
        TrustedCacheStage::ChildOpen);
}

void RetainedTrustedCacheDirectory::prepare_for_owned_cleanup() {
    if(descriptor_ < 0) {
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            TrustedCacheErrorCode::InvalidBoundary);
    }
    const ValidatedCacheRoot& root = TrustedCacheAccess::root(path_);
    require_root_unchanged(root, TrustedCacheStage::RecursiveRemoval);
    const auto& root_state = require_root_state(
        root, TrustedCacheStage::RecursiveRemoval);

    struct stat retained_before{};
    struct stat named_before{};
    if(fstat(descriptor_, &retained_before) != 0 ||
       fstatat(
           root_state->directory_descriptor,
           TrustedCacheAccess::leaf(path_).c_str(), &named_before,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!S_ISDIR(retained_before.st_mode) ||
       !same_identity_and_type(retained_before, named_before) ||
       !same_identity(retained_before, device_, inode_) ||
       status_device(retained_before) != root_state->device ||
       status_owner(retained_before) != root_state->owner ||
       status_owner(named_before) != root_state->owner ||
       status_owner(retained_before) !=
           static_cast<std::uintmax_t>(geteuid())) {
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            TrustedCacheErrorCode::ConcurrentReplacement);
    }

    if(fchmod(descriptor_, 0700) != 0) {
        const int mode_error = errno;
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            is_permission_error(mode_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            mode_error);
    }

    struct stat retained_after{};
    struct stat named_after{};
    if(fstat(descriptor_, &retained_after) != 0 ||
       fstatat(
           root_state->directory_descriptor,
           TrustedCacheAccess::leaf(path_).c_str(), &named_after,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }
    if(!same_identity_and_type(retained_before, retained_after) ||
       !same_identity_and_type(retained_after, named_after) ||
       status_owner(retained_after) != root_state->owner ||
       status_owner(named_after) != root_state->owner ||
       status_permissions(retained_after) != 0700 ||
       status_permissions(named_after) != 0700) {
        throw_cache_error(
            TrustedCacheStage::RecursiveRemoval,
            TrustedCacheErrorCode::ConcurrentReplacement);
    }
}

RetainedTrustedCacheDirectory retain_trusted_cache_directory(
    const ValidatedCachePath& path) {
    ValidatedCachePath current = revalidate_trusted_cache_path(
        path, CachePathRequirement::ExistingDirectory);
    OwnedFileDescriptor retained = open_validated_child_directory(
        current, TrustedCacheStage::ChildOpen);
    RetainedTrustedCacheDirectory directory(
        std::move(current), retained.get(), path.device(), path.inode());
    static_cast<void>(retained.release());
    directory.require_unchanged_identity();
    return directory;
}

ValidatedCachePath create_trusted_cache_directory(
    const ValidatedCacheRoot& root, const fs::path& path) {
    ValidatedCachePath missing = inspect_cache_child(
        root, path, CachePathRequirement::Missing);
    const auto& state = require_root_state(
        root, TrustedCacheStage::ChildCreation);
    std::string staging_template =
        "/proc/self/fd/" +
        std::to_string(state->directory_descriptor) +
        "/.moguet-cache-create-XXXXXX";
    std::vector<char> writable_template(
        staging_template.begin(), staging_template.end());
    writable_template.push_back('\0');
    // THREAT MODEL: Linux cannot atomically combine name creation with the
    // first retained descriptor acquisition. A hostile same-euid process can
    // race mkdtemp() -> openat2(); while the descriptor acquired below stays
    // open, nofollow checks and named identity comparisons pin and validate
    // the observed inode. This function returns a path capability, so a later
    // DirCleanupGuard starts a separate retained interval. We do not claim
    // atomic ownership from creation time or across those intervals.
    char* created_path = mkdtemp(writable_template.data());
    if(created_path == nullptr) {
        const int creation_error = errno;
        throw_cache_error(
            TrustedCacheStage::ChildCreation,
            is_permission_error(creation_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::CreationFailure,
            creation_error);
    }

    std::string staging_leaf =
        fs::path(created_path).filename().string();
    struct stat created_status{};
    if(fstatat(
           state->directory_descriptor, staging_leaf.c_str(),
           &created_status, AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        Logger::warn_noexcept([]() {
            return localization::translate_message(
                "Unable to prove ownership of a newly staged trusted cache directory; leaving it untouched.");
        });
        throw_cache_error(
            TrustedCacheStage::ChildCreation,
            is_permission_error(metadata_error)
                ? TrustedCacheErrorCode::PermissionDenied
                : TrustedCacheErrorCode::MetadataFailure,
            metadata_error);
    }

    bool rollback_required = true;
    try {
        require_safe_direct_child_status(
            created_status, state->owner, state->device,
            TrustedCacheStage::ChildCreation);
        if(status_permissions(created_status) != 0700) {
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                TrustedCacheErrorCode::UnsafePermissions);
        }

        const int staging_descriptor =
            open_child_directory_without_mount_crossing(
                state->directory_descriptor, staging_leaf);
        if(staging_descriptor < 0) {
            const int open_error = errno;
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                child_directory_open_error_code(open_error), open_error);
        }
        OwnedFileDescriptor opened_staging(staging_descriptor);
        struct stat opened_status{};
        if(fstat(opened_staging.get(), &opened_status) != 0) {
            const int metadata_error = errno;
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                TrustedCacheErrorCode::MetadataFailure,
                metadata_error);
        }
        if(!same_identity_and_type(created_status, opened_status)) {
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                TrustedCacheErrorCode::ConcurrentReplacement);
        }

#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
        notify_staged_directory_test_hook(root.path() / staging_leaf);
#endif
        require_root_unchanged(root, TrustedCacheStage::ChildCreation);
        struct stat named_staging_status{};
        if(fstatat(
               state->directory_descriptor, staging_leaf.c_str(),
               &named_staging_status, AT_SYMLINK_NOFOLLOW) != 0) {
            const int metadata_error = errno;
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                metadata_error == ENOENT
                    ? TrustedCacheErrorCode::ConcurrentReplacement
                    : (is_permission_error(metadata_error)
                           ? TrustedCacheErrorCode::PermissionDenied
                           : TrustedCacheErrorCode::MetadataFailure),
                metadata_error);
        }
        if(!same_identity_and_type(opened_status, named_staging_status)) {
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                TrustedCacheErrorCode::ConcurrentReplacement);
        }
        require_safe_direct_child_status(
            named_staging_status, state->owner, state->device,
            TrustedCacheStage::ChildCreation);
        if(status_permissions(named_staging_status) != 0700) {
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                TrustedCacheErrorCode::UnsafePermissions);
        }
        const std::string final_leaf = TrustedCacheAccess::leaf(missing);
        if(syscall(
               SYS_renameat2, state->directory_descriptor,
               staging_leaf.c_str(), state->directory_descriptor,
               final_leaf.c_str(), RENAME_NOREPLACE) != 0) {
            const int rename_error = errno;
            throw_cache_error(
                TrustedCacheStage::ChildCreation,
                rename_error == EEXIST
                    ? TrustedCacheErrorCode::ConcurrentReplacement
                    : (is_permission_error(rename_error)
                           ? TrustedCacheErrorCode::PermissionDenied
                           : TrustedCacheErrorCode::CreationFailure),
                rename_error);
        }
        staging_leaf = final_leaf;

        ValidatedCachePath created = TrustedCacheAccess::make_path(
            root, root.path() / final_leaf, final_leaf, true, true,
            status_device(created_status), status_inode(created_status),
            status_owner(created_status),
            status_permissions(created_status));
        created = revalidate_trusted_cache_path(
            created, CachePathRequirement::ExistingDirectory);
        rollback_required = false;
        return created;
    } catch(...) {
        if(rollback_required) {
            rollback_staged_cache_directory_noexcept(
                root, state->directory_descriptor, staging_leaf,
                created_status);
        }
        throw;
    }
}

void remove_trusted_cache_path(const ValidatedCachePath& expected) {
    RemovalPlan plan = make_removal_plan(
        expected, TrustedCacheStage::RecursiveRemoval);
    RemovalDirectoryLineage lineage;
    remove_removal_node(
        TrustedCacheAccess::root(expected), plan.root, lineage);
}

PreparedCacheCleanup::PreparedCacheCleanup(
    std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {
}

PreparedCacheCleanup::PreparedCacheCleanup(
    PreparedCacheCleanup&& other) noexcept = default;

PreparedCacheCleanup::~PreparedCacheCleanup() noexcept = default;

bool PreparedCacheCleanup::empty() const noexcept {
    return state_ == nullptr || state_->plans.empty();
}

std::size_t PreparedCacheCleanup::size() const noexcept {
    return state_ == nullptr ? 0 : state_->plans.size();
}

const fs::path& PreparedCacheCleanup::cache_path() const {
    if(state_ == nullptr) {
        throw_cache_error(
            TrustedCacheStage::CleanupPreflight,
            TrustedCacheErrorCode::InvalidBoundary);
    }
    return state_->root.path();
}

PreparedCacheCleanup preflight_cache_cleanup(
    const ValidatedCacheRoot& root) {
    try {
        root.require_unchanged_identity();
        const auto& state = require_root_state(
            root, TrustedCacheStage::CleanupPreflight);
        const std::vector<std::string> names = directory_entry_names(
            state->directory_descriptor,
            TrustedCacheStage::CleanupPreflight);
        std::vector<RemovalPlan> plans;
        plans.reserve(names.size());
        for(const std::string& name : names) {
            ValidatedCachePath target = inspect_cache_child(
                root, name, CachePathRequirement::Existing);
            // Cooperate with existing PackageBase writers for this subtree,
            // without retaining one lease per target across pacman/prompt.
            OwnedFileDescriptor lease = acquire_removal_lease(
                target, TrustedCacheStage::CleanupPreflight);
            RemovalPlan plan = make_removal_plan(
                target, TrustedCacheStage::CleanupPreflight);
            if(lease.get() >= 0) {
                require_named_removal_node_identity(
                    state->directory_descriptor, plan.root, lease.get(),
                    state->device, state->owner,
                    TrustedCacheStage::CleanupPreflight);
            }
            plans.push_back(std::move(plan));
        }
        root.require_unchanged_identity();
        return PreparedCacheCleanup(std::make_unique<PreparedCacheCleanup::State>(
            root, std::move(plans)));
    } catch(const TrustedCacheError& error) {
        throw_cleanup_preflight_failure(error);
    }
}

void remove_preflighted_cache_paths(PreparedCacheCleanup cleanup) {
    if(cleanup.state_ == nullptr) {
        throw_cache_error(
            TrustedCacheStage::CleanupPreflight,
            TrustedCacheErrorCode::InvalidBoundary);
    }
    if(cleanup.state_->plans.empty()) return;
    try {
        require_cleanup_root_lineage(
            cleanup.state_->root, cleanup.state_->plans);
        cleanup.state_->root.require_unchanged_identity();
        for(const RemovalPlan& plan : cleanup.state_->plans) {
            OwnedFileDescriptor lease = acquire_removal_lease(
                plan.target, TrustedCacheStage::CleanupPreflight, &plan.root);
            const auto& state = require_root_state(
                TrustedCacheAccess::root(plan.target),
                TrustedCacheStage::CleanupPreflight);
            revalidate_removal_node(
                state->directory_descriptor, plan.root, state->device,
                state->owner, TrustedCacheStage::CleanupPreflight);
        }
        cleanup.state_->root.require_unchanged_identity();
    } catch(const TrustedCacheError& error) {
        throw_cleanup_preflight_failure(error);
    }

    // All targets passed read-only revalidation before the first unlink.
    // A later busy target stops the operation as partial failure; its subtree
    // is never mutated without the same lease used by cooperative writers.
    for(const RemovalPlan& plan : cleanup.state_->plans) {
        OwnedFileDescriptor lease = acquire_removal_lease(
            plan.target, TrustedCacheStage::RecursiveRemoval, &plan.root);
        RemovalDirectoryLineage lineage;
        remove_removal_node(
            TrustedCacheAccess::root(plan.target), plan.root, lineage);
    }
}

WorkDirGuard::WorkDirGuard(const ValidatedCacheRoot& target) {
    target.require_unchanged_identity();
    const auto& state = require_root_state(
        target, TrustedCacheStage::ChildOpen);
    original_descriptor_ = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if(original_descriptor_ < 0) {
        const int open_error = errno;
        throw_cache_error(
            TrustedCacheStage::ChildOpen,
            TrustedCacheErrorCode::MetadataFailure, open_error);
    }
    try {
        target_descriptor_ = duplicate_descriptor(
                                 state->directory_descriptor,
                                 TrustedCacheStage::ChildOpen)
                                 .release();
        if(fchdir(target_descriptor_) != 0) {
            const int directory_error = errno;
            throw_cache_error(
                TrustedCacheStage::ChildOpen,
                is_permission_error(directory_error)
                    ? TrustedCacheErrorCode::PermissionDenied
                    : TrustedCacheErrorCode::MetadataFailure,
                directory_error);
        }
        target.require_unchanged_identity();
    } catch(...) {
        restore_working_directory_noexcept(original_descriptor_);
        if(target_descriptor_ >= 0) {
            static_cast<void>(close(target_descriptor_));
            target_descriptor_ = -1;
        }
        static_cast<void>(close(original_descriptor_));
        original_descriptor_ = -1;
        throw;
    }
}

WorkDirGuard::WorkDirGuard(const ValidatedCachePath& target) {
    original_descriptor_ = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
    if(original_descriptor_ < 0) {
        const int open_error = errno;
        throw_cache_error(
            TrustedCacheStage::ChildOpen,
            TrustedCacheErrorCode::MetadataFailure, open_error);
    }
    try {
        target_descriptor_ = open_validated_child_directory(
                                 target,
                                 TrustedCacheStage::ChildOpen)
                                 .release();
        if(fchdir(target_descriptor_) != 0) {
            const int directory_error = errno;
            throw_cache_error(
                TrustedCacheStage::ChildOpen,
                is_permission_error(directory_error)
                    ? TrustedCacheErrorCode::PermissionDenied
                    : TrustedCacheErrorCode::MetadataFailure,
                directory_error);
        }
        static_cast<void>(revalidate_trusted_cache_path(
            target, CachePathRequirement::ExistingDirectory));
    } catch(...) {
        restore_working_directory_noexcept(original_descriptor_);
        if(target_descriptor_ >= 0) {
            static_cast<void>(close(target_descriptor_));
            target_descriptor_ = -1;
        }
        static_cast<void>(close(original_descriptor_));
        original_descriptor_ = -1;
        throw;
    }
}

WorkDirGuard::~WorkDirGuard() noexcept {
    restore_working_directory_noexcept(original_descriptor_);
    if(target_descriptor_ >= 0) {
        static_cast<void>(close(target_descriptor_));
    }
    if(original_descriptor_ >= 0) {
        static_cast<void>(close(original_descriptor_));
    }
}

DirCleanupGuard::DirCleanupGuard(const ValidatedCachePath& path)
    : path_(path) {
    if(!path_.exists() || !path_.is_directory()) {
        throw std::invalid_argument(localization::translate_message(
            "Clone rollback requires an existing validated directory."));
    }

    ValidatedCachePath current = inspect_cache_child(
        TrustedCacheAccess::root(path_), TrustedCacheAccess::leaf(path_),
        CachePathRequirement::ExistingDirectory);
    require_same_cache_path_identity(
        path_, current, TrustedCacheStage::Rollback);
    const auto& root_state = require_root_state(
        TrustedCacheAccess::root(path_), TrustedCacheStage::Rollback);
    const int descriptor = open_removal_node_without_mount_crossing(
        root_state->directory_descriptor,
        TrustedCacheAccess::leaf(path_));
    if(descriptor < 0) {
        const int open_error = errno;
        throw_cache_error(
            TrustedCacheStage::Rollback,
            open_error == EXDEV
                ? TrustedCacheErrorCode::ChildEscape
                : (is_permission_error(open_error)
                       ? TrustedCacheErrorCode::PermissionDenied
                       : TrustedCacheErrorCode::RollbackRefusal),
            open_error);
    }
    OwnedFileDescriptor retained(descriptor);
    struct stat retained_status{};
    struct stat named_status{};
    if(fstat(retained.get(), &retained_status) != 0 ||
       fstatat(
           root_state->directory_descriptor,
           TrustedCacheAccess::leaf(path_).c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int metadata_error = errno;
        throw_cache_error(
            TrustedCacheStage::Rollback,
            TrustedCacheErrorCode::RollbackRefusal,
            metadata_error);
    }
    if(!S_ISDIR(retained_status.st_mode) ||
       !same_identity_and_type(retained_status, named_status) ||
       !same_identity(
           retained_status, path_.device_, path_.inode_) ||
       status_owner(retained_status) != path_.owner_ ||
       status_owner(named_status) != path_.owner_ ||
       status_permissions(retained_status) != path_.permissions_ ||
       status_permissions(named_status) != path_.permissions_ ||
       status_device(retained_status) != root_state->device ||
       status_owner(retained_status) != root_state->owner ||
       status_owner(retained_status) !=
           static_cast<std::uintmax_t>(geteuid()) ||
       status_permissions(retained_status) != 0700) {
        throw_cache_error(
            TrustedCacheStage::Rollback,
            TrustedCacheErrorCode::RollbackRefusal);
    }
    retained_descriptor_ = retained.release();
    retained_device_ = status_device(retained_status);
    retained_inode_ = status_inode(retained_status);
    retained_owner_ = status_owner(retained_status);
    retained_permissions_ = status_permissions(retained_status);
}

void DirCleanupGuard::commit() noexcept {
    committed_ = true;
}

DirCleanupGuard::~DirCleanupGuard() noexcept {
    if(!committed_) {
        try {
            if(rollback_trusted_cache_path(
                   path_, retained_descriptor_, retained_device_,
                   retained_inode_, retained_owner_,
                   retained_permissions_)) {
                Logger::warn_noexcept([]() {
                    return localization::translate_message(
                        "Rolled back a failed clone cache entry.");
                });
            }
        } catch(const std::exception& error) {
            Logger::warn_noexcept([&error]() {
                return localization::format_translated_message(
                    "Refusing unsafe clone rollback: {}", error.what());
            });
        } catch(...) {
            Logger::warn_noexcept([]() {
                return localization::translate_message(
                    "Refusing unsafe clone rollback: unknown error.");
            });
        }
    }
    if(retained_descriptor_ >= 0) {
        static_cast<void>(close(retained_descriptor_));
    }
}
