#include "artifact_workspace.hpp"

#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "shell_words.hpp"
#include "source_artifact_install_trusted_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <linux/openat2.h>
#include <linux/memfd.h>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

// query/build-onlyのexact context lineageだけを共有するimmutable token。
// global registryやmutable stateを持たず、context move後もExpected側のproofを保つ。
struct ArtifactMakepkgContextProvenance final {
};

// invocation-owned artifact workspaceのcreation、identity、command environment、
// freshness validation、cleanupをこのmoduleへ閉じ込める。
namespace {

namespace fs = std::filesystem;

constexpr mode_t ARTIFACT_WORKSPACE_MODE = 0700;
constexpr char ARTIFACT_WORKSPACE_PREFIX[] = ".artifact-workspace~-";
constexpr std::size_t ARTIFACT_DIAGNOSTIC_VALUE_LIMIT = 96;
constexpr std::string_view MAKEPKG_PACKAGELIST_COMMAND =
    "makepkg --packagelist";
constexpr char PRODUCTION_PACKAGELIST_CAPTURE_LEAF[] =
    ".makepkg-packagelist~capture";

std::runtime_error production_packagelist_capture_error() {
    return std::runtime_error(localization::format_translated_message(
        "Internal {} output capture failed.", "makepkg"));
}

class ProductionPackagelistCapture final {
    int directory_descriptor_ = -1;
    fs::path path_;
    bool owns_name_ = false;

public:
    ProductionPackagelistCapture(
        int directory_descriptor,
        const fs::path& workspace_path)
        : directory_descriptor_(directory_descriptor),
          path_(workspace_path / PRODUCTION_PACKAGELIST_CAPTURE_LEAF) {
        const int descriptor = openat(
            directory_descriptor_,
            PRODUCTION_PACKAGELIST_CAPTURE_LEAF,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600);
        if(descriptor < 0) {
            throw production_packagelist_capture_error();
        }
        owns_name_ = true;
        const int mode_status = fchmod(descriptor, 0600);
        const int close_status = close(descriptor);
        if(mode_status != 0 || close_status != 0) {
            throw production_packagelist_capture_error();
        }
    }

    ProductionPackagelistCapture(
        const ProductionPackagelistCapture&) = delete;
    ProductionPackagelistCapture& operator=(
        const ProductionPackagelistCapture&) = delete;

    ~ProductionPackagelistCapture() noexcept {
        if(owns_name_) {
            static_cast<void>(unlinkat(
                directory_descriptor_,
                PRODUCTION_PACKAGELIST_CAPTURE_LEAF, 0));
        }
    }

    const fs::path& path() const noexcept {
        return path_;
    }

    std::string read_and_remove() {
        const int descriptor = openat(
            directory_descriptor_,
            PRODUCTION_PACKAGELIST_CAPTURE_LEAF,
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if(descriptor < 0) {
            throw production_packagelist_capture_error();
        }
        struct stat status{};
        if(fstat(descriptor, &status) != 0 ||
           !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
           status.st_nlink != 1 ||
           (status.st_mode & 0777) != 0600) {
            static_cast<void>(close(descriptor));
            throw production_packagelist_capture_error();
        }

        std::string output;
        std::array<char, 4096> buffer;
        while(true) {
            const ssize_t size = read(
                descriptor, buffer.data(), buffer.size());
            if(size > 0) {
                output.append(
                    buffer.data(), static_cast<std::size_t>(size));
                continue;
            }
            if(size == 0) break;
            if(errno == EINTR) continue;
            static_cast<void>(close(descriptor));
            throw production_packagelist_capture_error();
        }
        if(close(descriptor) != 0 ||
           unlinkat(
               directory_descriptor_,
               PRODUCTION_PACKAGELIST_CAPTURE_LEAF, 0) != 0) {
            throw production_packagelist_capture_error();
        }
        owns_name_ = false;
        return output;
    }
};

#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
MultipleArtifactValidationObserverForTest
    g_multiple_artifact_validation_observer = nullptr;
MultipleArtifactCleanupObserverForTest
    g_multiple_artifact_cleanup_observer = nullptr;
ArtifactWorkspaceCreationObserverForTest
    g_artifact_workspace_creation_observer = nullptr;
ArtifactWorkspaceCleanupPreDeleteObserverForTest
    g_artifact_workspace_cleanup_pre_delete_observer = nullptr;
ArtifactWorkspaceCleanupChildOpenForTest
    g_artifact_workspace_cleanup_child_open = nullptr;

void notify_multiple_artifact_validation_for_test(
    const fs::path& workspace_path) {
    if(g_multiple_artifact_validation_observer != nullptr)
        g_multiple_artifact_validation_observer(workspace_path);
}

void notify_multiple_artifact_cleanup_for_test(
    const fs::path& workspace_path,
    const std::vector<int>& retained_descriptors) {
    if(g_multiple_artifact_cleanup_observer != nullptr) {
        g_multiple_artifact_cleanup_observer(
            workspace_path, retained_descriptors);
    }
}

void notify_artifact_workspace_creation_for_test(
    const fs::path& workspace_path) {
    if(g_artifact_workspace_creation_observer != nullptr)
        g_artifact_workspace_creation_observer(workspace_path);
}

void notify_artifact_workspace_cleanup_pre_delete_for_test(
    const fs::path& workspace_path) {
    if(g_artifact_workspace_cleanup_pre_delete_observer != nullptr)
        g_artifact_workspace_cleanup_pre_delete_observer(workspace_path);
}
#else
void notify_multiple_artifact_validation_for_test(const fs::path&) {
}

void notify_artifact_workspace_creation_for_test(const fs::path&) {
}

void notify_artifact_workspace_cleanup_pre_delete_for_test(const fs::path&) {
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

    OwnedFileDescriptor& operator=(OwnedFileDescriptor&&) = delete;

    ~OwnedFileDescriptor() noexcept {
        if(descriptor_ >= 0) close(descriptor_);
    }

    int get() const noexcept {
        return descriptor_;
    }

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }
};

// A snapshot is queried only after sealing. Even an ABA write while copying
// cannot change the bytes libalpm and the privileged installer will consume.
constexpr int INSTALL_CONTENT_SEALS = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;

OwnedFileDescriptor create_install_memfd() {
    const int descriptor = static_cast<int>(syscall(SYS_memfd_create,
                                                    "moguet-install-content", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if(descriptor < 0) throw std::runtime_error("Unable to create immutable artifact input.");
    return OwnedFileDescriptor(descriptor);
}

std::uint64_t copy_install_bytes(int source, int destination, std::uint64_t maximum_bytes = SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES) {
    struct stat metadata{};
    if(fstat(source, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
       static_cast<std::uint64_t>(metadata.st_size) > maximum_bytes)
        throw std::runtime_error("Invalid artifact content descriptor.");
    std::array<char, 65536> buffer{};
    off_t offset = 0;
    while(offset < metadata.st_size) {
        const auto count = pread(source, buffer.data(),
                                 std::min<off_t>(buffer.size(), metadata.st_size - offset), offset);
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0) throw std::runtime_error("Unable to read artifact content.");
        ssize_t written = 0;
        while(written < count) {
            const auto amount = write(destination, buffer.data() + written, count - written);
            if(amount < 0 && errno == EINTR) continue;
            if(amount <= 0) throw std::runtime_error("Unable to copy artifact content.");
            written += amount;
        }
        offset += count;
    }
    return static_cast<std::uint64_t>(metadata.st_size);
}

void seal_install_content(int descriptor) {
    if(fcntl(descriptor, F_ADD_SEALS, INSTALL_CONTENT_SEALS) != 0 ||
       fcntl(descriptor, F_GET_SEALS) != INSTALL_CONTENT_SEALS)
        throw std::runtime_error("Unable to seal artifact content.");
}

void require_same_install_content(int source, int snapshot) {
    struct stat current{}, saved{};
    if(snapshot < 0 || fcntl(snapshot, F_GET_SEALS) != INSTALL_CONTENT_SEALS ||
       fstat(source, &current) != 0 || fstat(snapshot, &saved) != 0 || current.st_size != saved.st_size)
        throw std::runtime_error("Prepared artifact content changed.");
    std::array<char, 65536> original{}, frozen{};
    off_t offset = 0;
    while(offset < saved.st_size) {
        const auto count = std::min<off_t>(original.size(), saved.st_size - offset);
        ssize_t first;
        do {
            first = pread(source, original.data(), count, offset);
        } while(first < 0 && errno == EINTR);
        ssize_t second;
        do {
            second = pread(snapshot, frozen.data(), count, offset);
        } while(second < 0 && errno == EINTR);
        if(first != count || second != count || !std::equal(original.begin(), original.begin() + count, frozen.begin()))
            throw std::runtime_error("Prepared artifact content changed.");
        offset += count;
    }
    if(fstat(source, &current) != 0 || current.st_size != saved.st_size)
        throw std::runtime_error("Prepared artifact content changed.");
}

// command間でpathnameを再解決せず、prepare時に開いたcheckout inodeへchdirする。
// named pathとのidentity照合はcallerが前後で行う。
class FileDescriptorWorkDirGuard final {
    OwnedFileDescriptor original_directory_;

public:
    explicit FileDescriptorWorkDirGuard(int target_descriptor)
        : original_directory_(open(
              ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
        if(original_directory_.get() < 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Failed to retain the current working directory: {}",
                std::strerror(errno)));
        }
        if(fchdir(target_descriptor) != 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Failed to enter the retained source checkout: {}",
                std::strerror(errno)));
        }
    }

    FileDescriptorWorkDirGuard(const FileDescriptorWorkDirGuard&) = delete;
    FileDescriptorWorkDirGuard& operator=(
        const FileDescriptorWorkDirGuard&) = delete;

    ~FileDescriptorWorkDirGuard() noexcept {
        if(original_directory_.get() < 0 ||
           fchdir(original_directory_.get()) == 0) {
            return;
        }
        const int restore_error = errno;
        Logger::warn_noexcept([restore_error]() {
            return localization::format_translated_message(
                // TRANSLATORS: The first placeholder is the literal
                // command name "makepkg"; the second is an error detail.
                "Failed to restore the working directory after an artifact {} operation: {}",
                "makepkg", std::strerror(restore_error));
        });
    }
};

struct DirectoryIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
};

std::uintmax_t status_device(const struct stat& status) {
    return static_cast<std::uintmax_t>(status.st_dev);
}

std::uintmax_t status_inode(const struct stat& status) {
    return static_cast<std::uintmax_t>(status.st_ino);
}

std::uintmax_t status_owner(const struct stat& status) {
    return static_cast<std::uintmax_t>(status.st_uid);
}

bool same_filesystem_identity(
    const struct stat& expected, const struct stat& actual) {
    return expected.st_dev == actual.st_dev && expected.st_ino == actual.st_ino &&
           (expected.st_mode & S_IFMT) == (actual.st_mode & S_IFMT);
}

bool same_directory_identity(
    const DirectoryIdentity& expected, const struct stat& actual) {
    return S_ISDIR(actual.st_mode) && expected.device == status_device(actual) &&
           expected.inode == status_inode(actual);
}

struct stat require_descriptor_status(
    int descriptor, const fs::path& display_path) {
    struct stat status{};
    if(fstat(descriptor, &status) != 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to inspect the descriptor for {}: {}",
            display_path.string(), std::strerror(errno)));
    }
    return status;
}

fs::path proc_file_descriptor_path(int descriptor) {
    return fs::path("/proc") / std::to_string(getpid()) / "fd" /
           std::to_string(descriptor);
}

void require_no_inherited_pkgdest() {
    // getenv()はdefined-emptyでもnon-nullを返す。makepkg.confはこの境界では解析しない。
    if(std::getenv("PKGDEST") != nullptr) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal environment key "PKGDEST".
            "Inherited {} conflicts with the invocation-owned artifact workspace.",
            "PKGDEST"));
    }
}

std::string bounded_artifact_diagnostic_value(std::string_view value) {
    constexpr char HEX_DIGITS[] = "0123456789abcdef";

    std::string bounded;
    bounded.reserve(ARTIFACT_DIAGNOSTIC_VALUE_LIMIT * 4 + 3);
    const std::size_t copied_size =
        std::min(value.size(), ARTIFACT_DIAGNOSTIC_VALUE_LIMIT);
    for(std::size_t index = 0; index < copied_size; ++index) {
        const unsigned char character =
            static_cast<unsigned char>(value[index]);
        if(character >= 0x20 && character <= 0x7e && character != '\\') {
            bounded.push_back(static_cast<char>(character));
        } else if(character == '\\') {
            bounded += "\\\\";
        } else {
            bounded += "\\x";
            bounded.push_back(HEX_DIGITS[character >> 4]);
            bounded.push_back(HEX_DIGITS[character & 0x0f]);
        }
    }
    if(value.size() > copied_size) bounded += "...";
    return bounded;
}

std::string bounded_artifact_diagnostic_path(const fs::path& path) {
    return bounded_artifact_diagnostic_value(path.string());
}

std::optional<struct stat> entry_status_at(
    int directory_descriptor, const std::string& leaf_name,
    const fs::path& display_path) {
    struct stat status{};
    if(fstatat(
           directory_descriptor, leaf_name.c_str(), &status,
           AT_SYMLINK_NOFOLLOW) == 0) {
        return status;
    }
    if(errno == ENOENT) return std::nullopt;
    throw std::runtime_error(localization::format_translated_message(
        "Unable to inspect artifact path {}: {}",
        bounded_artifact_diagnostic_path(display_path),
        std::strerror(errno)));
}

struct CleanupEntryPlan {
    std::string leaf_name;
    fs::path display_path;
    struct stat expected_status{};
    OwnedFileDescriptor directory_descriptor;
    std::vector<CleanupEntryPlan> children;

    bool is_directory() const noexcept {
        return S_ISDIR(expected_status.st_mode);
    }
};

int open_cleanup_child_directory_without_mount_crossing(
    int parent_descriptor, const std::string& leaf_name) noexcept {
    struct open_how how{};
    how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
    if(g_artifact_workspace_cleanup_child_open != nullptr) {
        return g_artifact_workspace_cleanup_child_open(
            parent_descriptor, leaf_name, how.flags, how.resolve);
    }
#endif
    return static_cast<int>(syscall(
        SYS_openat2, parent_descriptor, leaf_name.c_str(), &how,
        sizeof(how)));
}

CleanupEntryPlan preflight_directory_contents_at(
    int directory_descriptor, const fs::path& display_path,
    const struct stat& expected_directory_status,
    std::uintmax_t workspace_device) {
    int retained_descriptor = openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(retained_descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to retain artifact workspace directory {}: {}",
            display_path.string(), std::strerror(errno)));
    }
    OwnedFileDescriptor retained(retained_descriptor);
    const struct stat retained_status =
        require_descriptor_status(retained.get(), display_path);
    if(!same_filesystem_identity(
           expected_directory_status, retained_status)) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed artifact workspace directory: {}",
            display_path.string()));
    }

    int scan_descriptor = openat(
        retained.get(), ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to scan artifact workspace {}: {}",
            display_path.string(), std::strerror(errno)));
    }

    DIR* raw_stream = fdopendir(scan_descriptor);
    if(!raw_stream) {
        int open_error = errno;
        close(scan_descriptor);
        throw std::runtime_error(localization::format_translated_message(
            "Failed to read artifact workspace {}: {}",
            display_path.string(), std::strerror(open_error)));
    }
    std::unique_ptr<DIR, int (*)(DIR*)> stream(raw_stream, closedir);
    CleanupEntryPlan plan{
        "", display_path, expected_directory_status, std::move(retained), {}};

    while(true) {
        errno = 0;
        dirent* entry = readdir(stream.get());
        if(!entry) {
            if(errno != 0) {
                throw std::runtime_error(localization::format_translated_message(
                    "Failed while reading artifact workspace {}: {}",
                    display_path.string(), std::strerror(errno)));
            }
            break;
        }

        std::string leaf_name = entry->d_name;
        if(leaf_name == "." || leaf_name == "..") continue;
        fs::path entry_path = display_path / leaf_name;

        struct stat observed_status{};
        if(fstatat(
               plan.directory_descriptor.get(), leaf_name.c_str(),
               &observed_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Refusing changed artifact workspace entry {}: {}",
                entry_path.string(), std::strerror(errno)));
        }

        if(S_ISDIR(observed_status.st_mode)) {
            // LANDMINE(#242): mount boundaryをcleanupで横断しない。
            if(status_device(observed_status) != workspace_device) {
                throw std::runtime_error(localization::format_translated_message(
                    "Refusing to cross a filesystem boundary while cleaning {}.",
                    entry_path.string()));
            }

            int child_descriptor =
                open_cleanup_child_directory_without_mount_crossing(
                    plan.directory_descriptor.get(), leaf_name);
            if(child_descriptor < 0) {
                const int open_error = errno;
                if(open_error == EXDEV) {
                    throw std::runtime_error(localization::format_translated_message(
                        "Refusing to cross a filesystem boundary while cleaning {}.",
                        entry_path.string()));
                }
                throw std::runtime_error(localization::format_translated_message(
                    "Refusing changed artifact workspace directory {}: {}",
                    entry_path.string(), std::strerror(open_error)));
            }
            OwnedFileDescriptor child(child_descriptor);

            struct stat opened_status =
                require_descriptor_status(child.get(), entry_path);
            if(!same_filesystem_identity(observed_status, opened_status)) {
                throw std::runtime_error(localization::format_translated_message(
                    "Refusing changed artifact workspace directory: {}",
                    entry_path.string()));
            }

            CleanupEntryPlan child_plan = preflight_directory_contents_at(
                child.get(), entry_path, opened_status,
                workspace_device);
            child_plan.leaf_name = std::move(leaf_name);
            plan.children.push_back(std::move(child_plan));
            continue;
        }

        plan.children.push_back(CleanupEntryPlan{
            std::move(leaf_name), std::move(entry_path), observed_status, OwnedFileDescriptor(), {}});
    }

    return plan;
}

void require_cleanup_directory_descriptor_unchanged(
    const CleanupEntryPlan& directory_plan) {
    if(!directory_plan.is_directory() ||
       directory_plan.directory_descriptor.get() < 0) {
        throw std::logic_error(localization::translate_message(
            "Artifact workspace cleanup plan lost a directory descriptor."));
    }
    const struct stat descriptor_status = require_descriptor_status(
        directory_plan.directory_descriptor.get(),
        directory_plan.display_path);
    if(!same_filesystem_identity(
           directory_plan.expected_status, descriptor_status)) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed artifact workspace directory: {}",
            directory_plan.display_path.string()));
    }
}

void require_cleanup_entry_unchanged(
    const CleanupEntryPlan& parent_plan,
    const CleanupEntryPlan& entry_plan) {
    require_cleanup_directory_descriptor_unchanged(parent_plan);

    struct stat current_status{};
    if(fstatat(
           parent_plan.directory_descriptor.get(),
           entry_plan.leaf_name.c_str(), &current_status,
           AT_SYMLINK_NOFOLLOW) != 0 ||
       !same_filesystem_identity(
           entry_plan.expected_status, current_status)) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed artifact workspace entry: {}",
            entry_plan.display_path.string()));
    }
    if(entry_plan.is_directory())
        require_cleanup_directory_descriptor_unchanged(entry_plan);
}

void require_cleanup_plan_unchanged(
    const CleanupEntryPlan& directory_plan) {
    require_cleanup_directory_descriptor_unchanged(directory_plan);

    std::map<std::string, const CleanupEntryPlan*> remaining_entries;
    for(const CleanupEntryPlan& entry : directory_plan.children) {
        if(!remaining_entries.emplace(entry.leaf_name, &entry).second) {
            throw std::logic_error(localization::translate_message(
                "Artifact workspace cleanup plan contains a duplicate entry."));
        }
    }

    int scan_descriptor = openat(
        directory_plan.directory_descriptor.get(), ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to rescan artifact workspace {}: {}",
            directory_plan.display_path.string(), std::strerror(errno)));
    }
    DIR* raw_stream = fdopendir(scan_descriptor);
    if(!raw_stream) {
        const int open_error = errno;
        close(scan_descriptor);
        throw std::runtime_error(localization::format_translated_message(
            "Failed to reread artifact workspace {}: {}",
            directory_plan.display_path.string(), std::strerror(open_error)));
    }
    std::unique_ptr<DIR, int (*)(DIR*)> stream(raw_stream, closedir);

    while(true) {
        errno = 0;
        dirent* entry = readdir(stream.get());
        if(!entry) {
            if(errno != 0) {
                throw std::runtime_error(localization::format_translated_message(
                    "Failed while rereading artifact workspace {}: {}",
                    directory_plan.display_path.string(),
                    std::strerror(errno)));
            }
            break;
        }

        const std::string leaf_name = entry->d_name;
        if(leaf_name == "." || leaf_name == "..") continue;
        auto planned_entry = remaining_entries.find(leaf_name);
        if(planned_entry == remaining_entries.end()) {
            throw std::runtime_error(localization::format_translated_message(
                "Refusing changed artifact workspace entry: {}",
                (directory_plan.display_path / leaf_name).string()));
        }
        require_cleanup_entry_unchanged(
            directory_plan, *planned_entry->second);
        if(planned_entry->second->is_directory())
            require_cleanup_plan_unchanged(*planned_entry->second);
        remaining_entries.erase(planned_entry);
    }

    if(!remaining_entries.empty()) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed artifact workspace entry: {}",
            remaining_entries.begin()->second->display_path.string()));
    }
}

using CleanupDirectoryLineage = std::vector<
    std::pair<const CleanupEntryPlan*, const CleanupEntryPlan*>>;

void require_cleanup_target_unchanged(
    const ArtifactWorkspace& workspace,
    const CleanupEntryPlan& root_plan,
    const CleanupDirectoryLineage& lineage,
    const CleanupEntryPlan& parent_plan,
    const CleanupEntryPlan& target_plan) {
    workspace.require_unchanged_identity();
    require_cleanup_directory_descriptor_unchanged(root_plan);
    for(const auto& [ancestor, descendant] : lineage)
        require_cleanup_entry_unchanged(*ancestor, *descendant);
    require_cleanup_entry_unchanged(parent_plan, target_plan);
}

void remove_preflighted_directory_contents(
    const ArtifactWorkspace& workspace,
    const CleanupEntryPlan& root_plan,
    const CleanupEntryPlan& directory_plan,
    CleanupDirectoryLineage& lineage) {
    for(const CleanupEntryPlan& entry_plan : directory_plan.children) {
        require_cleanup_target_unchanged(
            workspace, root_plan, lineage, directory_plan, entry_plan);
        if(entry_plan.is_directory()) {
            lineage.emplace_back(&directory_plan, &entry_plan);
            remove_preflighted_directory_contents(
                workspace, root_plan, entry_plan, lineage);
            lineage.pop_back();
            require_cleanup_target_unchanged(
                workspace, root_plan, lineage, directory_plan,
                entry_plan);
            if(unlinkat(
                   directory_plan.directory_descriptor.get(),
                   entry_plan.leaf_name.c_str(), AT_REMOVEDIR) != 0) {
                throw std::runtime_error(localization::format_translated_message(
                    "Failed to remove artifact workspace directory {}: {}",
                    entry_plan.display_path.string(), std::strerror(errno)));
            }
            continue;
        }

        // Symlinkやspecial fileもfollowせず、検証済みdirectory entryだけを外す。
        if(unlinkat(
               directory_plan.directory_descriptor.get(),
               entry_plan.leaf_name.c_str(), 0) != 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Failed to remove artifact workspace entry {}: {}",
                entry_plan.display_path.string(), std::strerror(errno)));
        }
    }
}

class CreatedWorkspaceRollback final {
    const ValidatedPrivateCacheRoot& root_;
    int root_descriptor_ = -1;
    std::string leaf_name_;
    struct stat created_status_{};
    bool active_ = true;

public:
    CreatedWorkspaceRollback(
        const ValidatedPrivateCacheRoot& root, int root_descriptor,
        std::string leaf_name,
        const struct stat& created_status)
        : root_(root), root_descriptor_(root_descriptor),
          leaf_name_(std::move(leaf_name)),
          created_status_(created_status) {
    }

    CreatedWorkspaceRollback(const CreatedWorkspaceRollback&) = delete;
    CreatedWorkspaceRollback& operator=(const CreatedWorkspaceRollback&) = delete;

    void release() noexcept {
        active_ = false;
    }

    ~CreatedWorkspaceRollback() noexcept {
        if(!active_) return;
        try {
            root_.require_unchanged_identity();
            struct stat current_status{};
            if(fstatat(
                   root_descriptor_, leaf_name_.c_str(),
                   &current_status, AT_SYMLINK_NOFOLLOW) != 0) {
                throw std::runtime_error(localization::format_translated_message(
                    "Unable to inspect the created directory: {}",
                    std::strerror(errno)));
            }
            if(!same_filesystem_identity(
                   created_status_, current_status) ||
               status_owner(created_status_) != status_owner(current_status) ||
               (created_status_.st_mode & 07777) !=
                   (current_status.st_mode & 07777)) {
                throw std::runtime_error(localization::translate_message(
                    "The created directory changed identity, owner, or mode."));
            }
            if(unlinkat(
                   root_descriptor_, leaf_name_.c_str(),
                   AT_REMOVEDIR) != 0) {
                throw std::runtime_error(localization::format_translated_message(
                    "Unable to remove the created directory: {}",
                    std::strerror(errno)));
            }
        } catch(const std::exception& error) {
            // LANDMINE: unwind中の元failureを保ち、pending state-log failureも
            // shutdown()で報告できるようwarn_noexceptへ委ねる。
            Logger::warn_noexcept([this, &error]() {
                return localization::format_translated_message(
                    "Refusing unsafe artifact workspace creation rollback for {}: {}",
                    leaf_name_, error.what());
            });
        } catch(...) {
            Logger::warn_noexcept([this]() {
                return localization::format_translated_message(
                    "Refusing unsafe artifact workspace creation rollback for {}: unknown error.",
                    leaf_name_);
            });
        }
    }
};

bool is_blank_packagelist_line(const std::string& line) {
    return std::all_of(line.begin(), line.end(), [](char character) {
        return character == ' ' || character == '\t';
    });
}

std::vector<std::string> parse_packagelist_records(
    const std::string& raw_output) {
    for(unsigned char character : raw_output) {
        if(character == '\r') {
            throw std::runtime_error(localization::format_translated_message(
                // TRANSLATORS: {} is the literal command
                // "makepkg --packagelist".
                "{} output contains a carriage return.",
                MAKEPKG_PACKAGELIST_COMMAND));
        }
        if((character < 0x20 && character != '\n') || character == 0x7f) {
            throw std::runtime_error(localization::format_translated_message(
                "{} output contains an invalid control character.",
                MAKEPKG_PACKAGELIST_COMMAND));
        }
    }

    std::vector<std::string> records;
    std::set<std::string> seen_paths;
    std::size_t line_start = 0;
    std::size_t line_number = 1;
    while(line_start <= raw_output.size()) {
        std::size_t line_end = raw_output.find('\n', line_start);
        std::string line = raw_output.substr(
            line_start,
            line_end == std::string::npos
                ? std::string::npos
                : line_end - line_start);
        if(!is_blank_packagelist_line(line)) {
            if(!seen_paths.insert(line).second) {
                throw std::runtime_error(
                    localization::format_translated_message(
                        "{} returned a duplicate artifact path at record {}.",
                        MAKEPKG_PACKAGELIST_COMMAND, line_number));
            }
            records.push_back(std::move(line));
        }
        if(line_end == std::string::npos) break;
        line_start = line_end + 1;
        ++line_number;
    }
    return records;
}

void require_direct_expected_path(
    const ArtifactWorkspace& workspace, const fs::path& candidate) {
    if(!candidate.is_absolute()) {
        throw std::runtime_error(localization::format_translated_message(
            "{} returned a relative artifact path.",
            MAKEPKG_PACKAGELIST_COMMAND));
    }
    for(const auto& component : candidate) {
        if(component == "." || component == "..") {
            throw std::runtime_error(localization::format_translated_message(
                "{} returned an artifact path with a dot component.",
                MAKEPKG_PACKAGELIST_COMMAND));
        }
    }
    if(candidate == workspace.canonical_path() || candidate.filename().empty() ||
       candidate.parent_path() != workspace.canonical_path()) {
        throw std::runtime_error(localization::format_translated_message(
            "The artifact returned by {} is not a direct child of the artifact workspace.",
            MAKEPKG_PACKAGELIST_COMMAND));
    }
}

class InspectedArtifact final {
    OwnedFileDescriptor descriptor_;

public:
    struct stat status{};

    InspectedArtifact(int descriptor, const struct stat& artifact_status)
        : descriptor_(descriptor), status(artifact_status) {
    }

    InspectedArtifact(const InspectedArtifact&) = delete;
    InspectedArtifact& operator=(const InspectedArtifact&) = delete;
    InspectedArtifact(InspectedArtifact&&) noexcept = default;
    InspectedArtifact& operator=(InspectedArtifact&&) = delete;

    int descriptor() const noexcept {
        return descriptor_.get();
    }

    int release_descriptor() noexcept {
        return descriptor_.release();
    }
};

void require_regular_owned_entry(
    const struct stat& status, std::uintmax_t expected_effective_user,
    const fs::path& entry_path) {
    if(S_ISLNK(status.st_mode)) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact entry must not be a symlink: {}",
            entry_path.string()));
    }
    if(!S_ISREG(status.st_mode)) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact entry must be a regular file: {}",
            entry_path.string()));
    }
    if(status_owner(status) != expected_effective_user) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact entry owner does not match the effective user: {}",
            entry_path.string()));
    }
}

OwnedFileDescriptor open_and_revalidate_regular_entry(
    int directory_descriptor, const std::string& leaf_name,
    const struct stat& named_status, const fs::path& entry_path) {
    int descriptor = openat(
        directory_descriptor, leaf_name.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if(descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to open artifact entry {}: {}", entry_path.string(),
            std::strerror(errno)));
    }
    OwnedFileDescriptor opened(descriptor);
    struct stat opened_status =
        require_descriptor_status(opened.get(), entry_path);
    if(!same_filesystem_identity(named_status, opened_status)) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed artifact entry: {}", entry_path.string()));
    }
    return opened;
}

void require_only_expected_workspace_entries(
    int directory_descriptor, const fs::path& workspace_path,
    const std::string& artifact_leaf, const std::string& signature_leaf) {
    int scan_descriptor = openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to enumerate artifact workspace {}: {}",
            workspace_path.string(), std::strerror(errno)));
    }
    DIR* raw_stream = fdopendir(scan_descriptor);
    if(!raw_stream) {
        int open_error = errno;
        close(scan_descriptor);
        throw std::runtime_error(localization::format_translated_message(
            "Failed to enumerate artifact workspace {}: {}",
            workspace_path.string(), std::strerror(open_error)));
    }
    std::unique_ptr<DIR, int (*)(DIR*)> stream(raw_stream, closedir);

    while(true) {
        errno = 0;
        dirent* entry = readdir(stream.get());
        if(!entry) {
            if(errno != 0) {
                throw std::runtime_error(
                    localization::format_translated_message(
                        "Failed while enumerating artifact workspace {}: {}",
                        workspace_path.string(), std::strerror(errno)));
            }
            break;
        }
        std::string leaf_name = entry->d_name;
        if(leaf_name == "." || leaf_name == ".." ||
           leaf_name == artifact_leaf || leaf_name == signature_leaf) {
            continue;
        }

        fs::path extra_path = workspace_path / leaf_name;
        struct stat extra_status{};
        if(fstatat(
               directory_descriptor, leaf_name.c_str(), &extra_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            if(errno == ENOENT) continue;
            throw std::runtime_error(localization::format_translated_message(
                "Failed to inspect unexpected artifact workspace entry {}: {}",
                extra_path.string(), std::strerror(errno)));
        }
        if(S_ISDIR(extra_status.st_mode)) {
            throw std::runtime_error(localization::format_translated_message(
                "Unexpected directory in artifact workspace: {}",
                extra_path.string()));
        }
        if(leaf_name.ends_with(".sig")) {
            throw std::runtime_error(localization::format_translated_message(
                "Unmatched signature in artifact workspace: {}",
                extra_path.string()));
        }
        throw std::runtime_error(localization::format_translated_message(
            "Unexpected entry in artifact workspace: {}",
            extra_path.string()));
    }
}

InspectedArtifact inspect_post_build_artifact(
    int directory_descriptor, const fs::path& workspace_path,
    const std::string& artifact_leaf,
    std::uintmax_t expected_effective_user) {
    fs::path artifact_path = workspace_path / artifact_leaf;
    std::optional<struct stat> artifact_status = entry_status_at(
        directory_descriptor, artifact_leaf, artifact_path);
    if(!artifact_status.has_value()) {
        throw std::runtime_error(localization::format_translated_message(
            "Expected package artifact is missing: {}",
            artifact_path.string()));
    }
    require_regular_owned_entry(
        artifact_status.value(), expected_effective_user, artifact_path);
    OwnedFileDescriptor artifact_descriptor = open_and_revalidate_regular_entry(
        directory_descriptor, artifact_leaf, artifact_status.value(),
        artifact_path);

    std::string signature_leaf = artifact_leaf + ".sig";
    fs::path signature_path = workspace_path / signature_leaf;
    std::optional<struct stat> signature_status = entry_status_at(
        directory_descriptor, signature_leaf, signature_path);
    if(signature_status.has_value()) {
        require_regular_owned_entry(
            signature_status.value(), expected_effective_user,
            signature_path);
        static_cast<void>(open_and_revalidate_regular_entry(
            directory_descriptor, signature_leaf,
            signature_status.value(), signature_path));
    }

    require_only_expected_workspace_entries(
        directory_descriptor, workspace_path, artifact_leaf,
        signature_leaf);

    std::optional<struct stat> final_artifact_status = entry_status_at(
        directory_descriptor, artifact_leaf, artifact_path);
    if(!final_artifact_status.has_value() ||
       !same_filesystem_identity(
           artifact_status.value(), final_artifact_status.value())) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed package artifact: {}",
            artifact_path.string()));
    }
    std::optional<struct stat> final_signature_status = entry_status_at(
        directory_descriptor, signature_leaf, signature_path);
    if(signature_status.has_value() != final_signature_status.has_value() ||
       (signature_status.has_value() &&
        !same_filesystem_identity(
            signature_status.value(), final_signature_status.value()))) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed package signature: {}",
            signature_path.string()));
    }
    return InspectedArtifact(
        artifact_descriptor.release(), artifact_status.value());
}

struct ArtifactInspectionTarget {
    fs::path path;
    std::string leaf_name;
};

class InspectedMultipleArtifact final {
    OwnedFileDescriptor artifact_descriptor_;
    std::optional<OwnedFileDescriptor> signature_descriptor_;

public:
    struct stat artifact_status{};
    std::optional<struct stat> signature_status;

    InspectedMultipleArtifact(
        OwnedFileDescriptor artifact_descriptor,
        const struct stat& retained_artifact_status,
        std::optional<OwnedFileDescriptor> signature_descriptor,
        std::optional<struct stat> retained_signature_status)
        : artifact_descriptor_(std::move(artifact_descriptor)),
          signature_descriptor_(std::move(signature_descriptor)),
          artifact_status(retained_artifact_status),
          signature_status(std::move(retained_signature_status)) {
    }

    InspectedMultipleArtifact(
        const InspectedMultipleArtifact&) = delete;
    InspectedMultipleArtifact& operator=(
        const InspectedMultipleArtifact&) = delete;
    InspectedMultipleArtifact(
        InspectedMultipleArtifact&&) noexcept = default;
    InspectedMultipleArtifact& operator=(
        InspectedMultipleArtifact&&) = delete;

    int artifact_descriptor() const noexcept {
        return artifact_descriptor_.get();
    }

    int signature_descriptor() const noexcept {
        return signature_descriptor_.has_value()
                   ? signature_descriptor_->get()
                   : -1;
    }

    int release_artifact_descriptor() noexcept {
        return artifact_descriptor_.release();
    }

    int release_signature_descriptor() noexcept {
        if(!signature_descriptor_.has_value()) return -1;
        return signature_descriptor_->release();
    }
};

InspectedMultipleArtifact inspect_expected_artifact(
    int directory_descriptor, const fs::path& workspace_path,
    const ArtifactInspectionTarget& target,
    std::uintmax_t expected_artifact_owner,
    std::uintmax_t expected_signature_owner) {
    std::optional<struct stat> artifact_status = entry_status_at(
        directory_descriptor, target.leaf_name, target.path);
    if(!artifact_status.has_value()) {
        throw std::runtime_error(localization::format_translated_message(
            "Expected package artifact is missing: {}",
            target.path.string()));
    }
    require_regular_owned_entry(
        artifact_status.value(), expected_artifact_owner, target.path);
    OwnedFileDescriptor artifact_descriptor = open_and_revalidate_regular_entry(
        directory_descriptor, target.leaf_name, artifact_status.value(),
        target.path);

    const std::string signature_leaf = target.leaf_name + ".sig";
    const fs::path signature_path = workspace_path / signature_leaf;
    std::optional<struct stat> signature_status = entry_status_at(
        directory_descriptor, signature_leaf, signature_path);
    std::optional<OwnedFileDescriptor> signature_descriptor;
    if(signature_status.has_value()) {
        require_regular_owned_entry(
            signature_status.value(), expected_signature_owner,
            signature_path);
        signature_descriptor.emplace(open_and_revalidate_regular_entry(
            directory_descriptor, signature_leaf,
            signature_status.value(), signature_path));
    }

    return InspectedMultipleArtifact(
        std::move(artifact_descriptor), artifact_status.value(),
        std::move(signature_descriptor), std::move(signature_status));
}

void require_retained_entry_unchanged(
    int descriptor, const struct stat& expected_status,
    std::uintmax_t expected_effective_user,
    const fs::path& entry_path) {
    if(descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Retained artifact entry descriptor is closed: {}",
            entry_path.string()));
    }
    const struct stat retained_status =
        require_descriptor_status(descriptor, entry_path);
    require_regular_owned_entry(
        retained_status, expected_effective_user, entry_path);
    if(!same_filesystem_identity(expected_status, retained_status)) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed retained artifact entry: {}",
            entry_path.string()));
    }
}

void require_named_entry_unchanged(
    int directory_descriptor, const std::string& leaf_name,
    const struct stat& expected_status,
    std::uintmax_t expected_effective_user,
    const fs::path& entry_path) {
    std::optional<struct stat> named_status = entry_status_at(
        directory_descriptor, leaf_name, entry_path);
    if(!named_status.has_value()) {
        throw std::runtime_error(localization::format_translated_message(
            "Expected artifact entry is missing: {}",
            entry_path.string()));
    }
    require_regular_owned_entry(
        named_status.value(), expected_effective_user, entry_path);
    if(!same_filesystem_identity(expected_status, named_status.value())) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed artifact entry: {}", entry_path.string()));
    }
}

void require_inspected_artifact_unchanged(
    int directory_descriptor, const fs::path& workspace_path,
    const ArtifactInspectionTarget& target,
    const InspectedMultipleArtifact& inspected,
    std::uintmax_t expected_artifact_owner,
    std::uintmax_t expected_signature_owner) {
    require_retained_entry_unchanged(
        inspected.artifact_descriptor(), inspected.artifact_status,
        expected_artifact_owner, target.path);
    require_named_entry_unchanged(
        directory_descriptor, target.leaf_name,
        inspected.artifact_status, expected_artifact_owner, target.path);

    const std::string signature_leaf = target.leaf_name + ".sig";
    const fs::path signature_path = workspace_path / signature_leaf;
    std::optional<struct stat> named_signature_status = entry_status_at(
        directory_descriptor, signature_leaf, signature_path);
    if(inspected.signature_status.has_value() !=
       named_signature_status.has_value()) {
        throw std::runtime_error(localization::format_translated_message(
            "Refusing changed package signature: {}",
            signature_path.string()));
    }
    if(!inspected.signature_status.has_value()) return;

    require_retained_entry_unchanged(
        inspected.signature_descriptor(),
        inspected.signature_status.value(), expected_signature_owner,
        signature_path);
    require_named_entry_unchanged(
        directory_descriptor, signature_leaf,
        inspected.signature_status.value(), expected_signature_owner,
        signature_path);
}

void require_only_expected_workspace_entries_for_set(
    int directory_descriptor, const fs::path& workspace_path,
    const std::set<std::string>& artifact_leaves,
    const std::set<std::string>& signature_leaves) {
    int scan_descriptor = openat(
        directory_descriptor, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to enumerate artifact workspace {}: {}",
            workspace_path.string(), std::strerror(errno)));
    }
    DIR* raw_stream = fdopendir(scan_descriptor);
    if(!raw_stream) {
        const int open_error = errno;
        close(scan_descriptor);
        throw std::runtime_error(localization::format_translated_message(
            "Failed to enumerate artifact workspace {}: {}",
            workspace_path.string(), std::strerror(open_error)));
    }
    std::unique_ptr<DIR, int (*)(DIR*)> stream(raw_stream, closedir);

    while(true) {
        errno = 0;
        dirent* entry = readdir(stream.get());
        if(!entry) {
            if(errno != 0) {
                throw std::runtime_error(
                    localization::format_translated_message(
                        "Failed while enumerating artifact workspace {}: {}",
                        workspace_path.string(), std::strerror(errno)));
            }
            break;
        }

        const std::string leaf_name = entry->d_name;
        if(leaf_name == "." || leaf_name == ".." ||
           artifact_leaves.contains(leaf_name) ||
           signature_leaves.contains(leaf_name)) {
            continue;
        }

        const fs::path extra_path = workspace_path / leaf_name;
        struct stat extra_status{};
        if(fstatat(
               directory_descriptor, leaf_name.c_str(), &extra_status,
               AT_SYMLINK_NOFOLLOW) != 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Unexpected artifact workspace entry changed during validation: {}",
                extra_path.string()));
        }
        if(S_ISDIR(extra_status.st_mode)) {
            throw std::runtime_error(localization::format_translated_message(
                "Unexpected directory in artifact workspace: {}",
                extra_path.string()));
        }
        if(leaf_name.ends_with(".sig")) {
            throw std::runtime_error(localization::format_translated_message(
                "Unmatched signature in artifact workspace: {}",
                extra_path.string()));
        }
        if(S_ISLNK(extra_status.st_mode)) {
            throw std::runtime_error(localization::format_translated_message(
                "Unexpected symlink in artifact workspace: {}",
                extra_path.string()));
        }
        if(!S_ISREG(extra_status.st_mode)) {
            throw std::runtime_error(localization::format_translated_message(
                "Unexpected special file in artifact workspace: {}",
                extra_path.string()));
        }
        throw std::runtime_error(localization::format_translated_message(
            "Unexpected entry in artifact workspace: {}",
            extra_path.string()));
    }
}

std::vector<InspectedMultipleArtifact> inspect_post_build_artifact_set(
    int directory_descriptor, const fs::path& workspace_path,
    const std::vector<ArtifactInspectionTarget>& targets,
    std::uintmax_t expected_artifact_owner,
    std::uintmax_t expected_signature_owner,
    bool should_notify_test_observer) {
    if(targets.empty()) {
        throw std::runtime_error(localization::translate_message(
            "Expected package artifact set must not be empty."));
    }

    std::set<std::string> artifact_leaves;
    std::set<std::string> signature_leaves;
    for(const ArtifactInspectionTarget& target : targets) {
        if(!artifact_leaves.insert(target.leaf_name).second) {
            throw std::runtime_error(localization::translate_message(
                "Expected package artifact set contains a duplicate path."));
        }
        signature_leaves.insert(target.leaf_name + ".sig");
    }
    for(const std::string& signature_leaf : signature_leaves) {
        if(artifact_leaves.contains(signature_leaf)) {
            throw std::runtime_error(localization::translate_message(
                "Expected package artifact and signature namespaces collide."));
        }
    }

    std::vector<InspectedMultipleArtifact> inspected_artifacts;
    inspected_artifacts.reserve(targets.size());
    for(const ArtifactInspectionTarget& target : targets) {
        inspected_artifacts.push_back(inspect_expected_artifact(
            directory_descriptor, workspace_path, target,
            expected_artifact_owner, expected_signature_owner));
    }

    require_only_expected_workspace_entries_for_set(
        directory_descriptor, workspace_path, artifact_leaves,
        signature_leaves);
    if(should_notify_test_observer)
        notify_multiple_artifact_validation_for_test(workspace_path);

    for(std::size_t index = 0; index < targets.size(); ++index) {
        require_inspected_artifact_unchanged(
            directory_descriptor, workspace_path, targets[index],
            inspected_artifacts[index], expected_artifact_owner,
            expected_signature_owner);
    }
    require_only_expected_workspace_entries_for_set(
        directory_descriptor, workspace_path, artifact_leaves,
        signature_leaves);
    for(std::size_t index = 0; index < targets.size(); ++index) {
        require_inspected_artifact_unchanged(
            directory_descriptor, workspace_path, targets[index],
            inspected_artifacts[index], expected_artifact_owner,
            expected_signature_owner);
    }
    return inspected_artifacts;
}

} // namespace

ArtifactWorkspace::ArtifactWorkspace(
    ValidatedPrivateCacheRoot root, fs::path path,
    fs::path canonical_path, std::string leaf_name,
    int directory_descriptor, std::uintmax_t device,
    std::uintmax_t inode, std::uintmax_t owner) noexcept
    : root_(std::move(root)), path_(std::move(path)),
      canonical_path_(std::move(canonical_path)),
      leaf_name_(std::move(leaf_name)),
      directory_descriptor_(directory_descriptor), device_(device),
      inode_(inode), owner_(owner), owns_path_(true) {
}

ArtifactWorkspace::ArtifactWorkspace(ArtifactWorkspace&& other) noexcept
    : root_(std::move(other.root_)), path_(std::move(other.path_)),
      canonical_path_(std::move(other.canonical_path_)),
      leaf_name_(std::move(other.leaf_name_)),
      directory_descriptor_(std::exchange(other.directory_descriptor_, -1)),
      device_(other.device_), inode_(other.inode_), owner_(other.owner_),
      owns_path_(std::exchange(other.owns_path_, false)),
      cleanup_on_destruction_(other.cleanup_on_destruction_) {
}

ArtifactWorkspace::~ArtifactWorkspace() noexcept {
    if(owns_path_ && cleanup_on_destruction_) {
        try {
            cleanup();
        } catch(const std::exception& error) {
            Logger::warn_noexcept([this, &error]() {
                return localization::format_translated_message(
                    "Refusing unsafe artifact workspace cleanup for {}: {}",
                    path_.string(), error.what());
            });
        } catch(...) {
            Logger::warn_noexcept([this]() {
                return localization::format_translated_message(
                    "Refusing unsafe artifact workspace cleanup for {}: unknown error.",
                    path_.string());
            });
        }
    }
    if(directory_descriptor_ >= 0) close(directory_descriptor_);
}

void ArtifactWorkspace::require_unchanged_identity_for_owner(
    std::uintmax_t expected_effective_user) const {
    if(!owns_path_ || directory_descriptor_ < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact workspace is no longer owned: {}", path_.string()));
    }

    root_.require_unchanged_identity();
    if(path_.parent_path() != root_.canonical_path() ||
       canonical_path_.parent_path() != root_.canonical_path() ||
       path_.filename().string() != leaf_name_) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact workspace is not a direct child of the trusted cache root: {}",
            path_.string()));
    }

    struct stat open_workspace_status =
        require_descriptor_status(directory_descriptor_, path_);
    DirectoryIdentity expected_workspace{device_, inode_};
    if(!same_directory_identity(expected_workspace, open_workspace_status) ||
       status_owner(open_workspace_status) != owner_ ||
       owner_ != expected_effective_user ||
       (open_workspace_status.st_mode & 07777) != ARTIFACT_WORKSPACE_MODE) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact workspace descriptor changed identity, owner, or mode: {}",
            path_.string()));
    }

    struct stat named_workspace_status{};
    if(fstatat(
           root_.directory_descriptor(), leaf_name_.c_str(),
           &named_workspace_status,
           AT_SYMLINK_NOFOLLOW) != 0 ||
       !same_directory_identity(expected_workspace, named_workspace_status) ||
       status_owner(named_workspace_status) != owner_ ||
       (named_workspace_status.st_mode & 07777) != ARTIFACT_WORKSPACE_MODE) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact workspace path changed identity, owner, or mode: {}",
            path_.string()));
    }

    std::error_code canonical_error;
    fs::path current_canonical = fs::canonical(path_, canonical_error);
    if(canonical_error || current_canonical != canonical_path_ ||
       current_canonical.parent_path() != root_.canonical_path()) {
        throw std::runtime_error(localization::format_translated_message(
            "Artifact workspace canonical containment changed: {}",
            path_.string()));
    }
}

void ArtifactWorkspace::require_unchanged_identity() const {
    require_unchanged_identity_for_owner(
        static_cast<std::uintmax_t>(geteuid()));
}

void ArtifactWorkspace::retain_for_diagnostics() noexcept {
    cleanup_on_destruction_ = false;
}

void ArtifactWorkspace::cleanup() {
    if(!owns_path_) return;
    // POLICY(#242): tree全体のdescriptor/identityをpreflightし、削除直前の
    // 再検証も全件成功しなければ一件も削除しない。
    // NOTE: same-euid processによる各final checkとunlinkatの間の同時mutationは
    // kernelにcompare-and-unlinkがないため脅威modelに含めない。
    require_unchanged_identity();
    const struct stat workspace_status =
        require_descriptor_status(directory_descriptor_, path_);
    CleanupEntryPlan cleanup_plan = preflight_directory_contents_at(
        directory_descriptor_, canonical_path_, workspace_status,
        device_);
    require_unchanged_identity();
    require_cleanup_plan_unchanged(cleanup_plan);
    notify_artifact_workspace_cleanup_pre_delete_for_test(canonical_path_);
    require_unchanged_identity();
    require_cleanup_plan_unchanged(cleanup_plan);

    CleanupDirectoryLineage lineage;
    remove_preflighted_directory_contents(
        *this, cleanup_plan, cleanup_plan, lineage);
    require_unchanged_identity();
    if(unlinkat(
           root_.directory_descriptor(), leaf_name_.c_str(),
           AT_REMOVEDIR) != 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to remove artifact workspace {}: {}", path_.string(),
            std::strerror(errno)));
    }
    owns_path_ = false;
    if(directory_descriptor_ >= 0) {
        close(directory_descriptor_);
        directory_descriptor_ = -1;
    }
}

ArtifactWorkspace create_artifact_workspace(
    ValidatedPrivateCacheRoot root) {
    root.require_unchanged_identity();
    const int root_descriptor = root.directory_descriptor();

    std::string template_path =
        (proc_file_descriptor_path(root_descriptor) /
         (std::string(ARTIFACT_WORKSPACE_PREFIX) + "XXXXXX"))
            .string();
    std::vector<char> path_buffer(template_path.begin(), template_path.end());
    path_buffer.push_back('\0');
    char* created_path = mkdtemp(path_buffer.data());
    if(!created_path) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to create artifact workspace under {}: {}",
            root.canonical_path().string(), std::strerror(errno)));
    }

    std::string leaf_name = fs::path(created_path).filename().string();
    fs::path display_path = root.canonical_path() / leaf_name;
    struct stat created_status{};
    if(fstatat(
           root_descriptor, leaf_name.c_str(), &created_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        int inspect_error = errno;
        // POLICY(#242): identityを一度も証明できていないnamed leafは削除しない。
        // replacementを消すより、fresh workspace候補をfail-safeに残す。
        throw std::runtime_error(localization::format_translated_message(
            "Failed to inspect created artifact workspace {}: {}",
            display_path.string(), std::strerror(inspect_error)));
    }
    if(!S_ISDIR(created_status.st_mode) || S_ISLNK(created_status.st_mode) ||
       status_owner(created_status) !=
           static_cast<std::uintmax_t>(geteuid()) ||
       (created_status.st_mode & 07777) != ARTIFACT_WORKSPACE_MODE) {
        throw std::runtime_error(localization::format_translated_message(
            "Created artifact workspace has an unsafe type, owner, or mode: {}",
            display_path.string()));
    }
    if(!leaf_name.starts_with(ARTIFACT_WORKSPACE_PREFIX) ||
       is_valid_package_name(leaf_name)) {
        throw std::logic_error(localization::format_translated_message(
            "Artifact workspace namespace collides with package identifiers: {}",
            leaf_name));
    }
    CreatedWorkspaceRollback rollback(
        root, root_descriptor, leaf_name, created_status);

    int directory_descriptor = openat(
        root_descriptor, leaf_name.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(directory_descriptor < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to open created artifact workspace {}: {}",
            display_path.string(), std::strerror(errno)));
    }
    OwnedFileDescriptor opened_directory(directory_descriptor);
    struct stat opened_status =
        require_descriptor_status(opened_directory.get(), display_path);
    if(!same_filesystem_identity(created_status, opened_status)) {
        throw std::runtime_error(localization::format_translated_message(
            "Created artifact workspace changed identity: {}",
            display_path.string()));
    }

    std::error_code canonical_error;
    fs::path canonical_path = fs::canonical(display_path, canonical_error);
    if(canonical_error || canonical_path.parent_path() != root.canonical_path() ||
       canonical_path != display_path) {
        throw std::runtime_error(localization::format_translated_message(
            "Created artifact workspace is outside the trusted cache root: {}",
            display_path.string()));
    }
    notify_artifact_workspace_creation_for_test(display_path);

    ArtifactWorkspace workspace(
        std::move(root), std::move(display_path),
        std::move(canonical_path), std::move(leaf_name),
        opened_directory.get(),
        status_device(created_status), status_inode(created_status),
        status_owner(created_status));
    static_cast<void>(opened_directory.release());
    rollback.release();
    return workspace;
}

void require_unclaimed_artifact_pkgdest(
    const SourceBuildEnvironment& environment) {
    if(environment.defines("PKGDEST")) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal environment key "PKGDEST".
            "Source environment {} conflicts with the invocation-owned artifact workspace.",
            "PKGDEST"));
    }
    require_no_inherited_pkgdest();
}

ProductionArtifactSourceTree::ProductionArtifactSourceTree(
    ValidatedCachePath checkout,
    std::shared_ptr<void> authority_lifetime,
    std::function<int(const std::string&, const std::string&)>
        guarded_command,
    ProductionSourceBuildProvenance provenance)
    : checkout_(std::move(checkout)),
      authority_lifetime_(std::move(authority_lifetime)),
      guarded_command_(std::move(guarded_command)),
      provenance_(std::move(provenance)) {
    if(authority_lifetime_ == nullptr || !guarded_command_ ||
       !checkout_.exists() ||
       !checkout_.is_directory()) {
        throw std::logic_error(
            "Production source tree lease binding is inconsistent.");
    }
}

const ProductionSourceBuildProvenance&
ProductionArtifactSourceTree::provenance() const noexcept {
    return provenance_;
}

const ValidatedCachePath& ProductionArtifactSourceTree::checkout()
    const noexcept {
    return checkout_;
}

void ProductionArtifactSourceTree::require_unchanged_identity() const {
    static_cast<void>(revalidate_trusted_cache_path(
        checkout_, CachePathRequirement::ExistingDirectory));
}

int ProductionArtifactSourceTree::run_guarded_command(
    const std::string& command,
    const std::string& display_command) const {
    require_unchanged_identity();
    const int status = guarded_command_(command, display_command);
    try {
        require_unchanged_identity();
    } catch(...) {
        throw ProductionSourceBuildPostCommandRevalidationError(
            status, std::current_exception());
    }
    return status;
}

ArtifactMakepkgContext::ArtifactMakepkgContext(
    RetainedTrustedCacheDirectory checkout,
    SourceBuildEnvironment command_environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy,
    fs::path workspace_path, std::uintmax_t workspace_device,
    std::uintmax_t workspace_inode)
    : checkout_(std::move(checkout)),
      command_environment_(std::move(command_environment)),
      empty_value_policy_(empty_value_policy),
      provenance_(std::make_shared<const ArtifactMakepkgContextProvenance>()),
      workspace_path_(std::move(workspace_path)),
      workspace_device_(workspace_device), workspace_inode_(workspace_inode) {
}

ArtifactMakepkgContext::ArtifactMakepkgContext(
    RetainedTrustedCacheDirectory checkout,
    ProductionArtifactSourceTree source_tree,
    SourceBuildEnvironment command_environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy,
    fs::path workspace_path, std::uintmax_t workspace_device,
    std::uintmax_t workspace_inode)
    : checkout_(std::move(checkout)),
      production_source_tree_(
          std::make_unique<ProductionArtifactSourceTree>(
              std::move(source_tree))),
      command_environment_(std::move(command_environment)),
      empty_value_policy_(empty_value_policy),
      provenance_(std::make_shared<const ArtifactMakepkgContextProvenance>()),
      workspace_path_(std::move(workspace_path)),
      workspace_device_(workspace_device), workspace_inode_(workspace_inode) {
}

ArtifactMakepkgContext::ArtifactMakepkgContext(
    ArtifactMakepkgContext&& other) noexcept
    : checkout_(std::move(other.checkout_)),
      production_source_tree_(std::move(other.production_source_tree_)),
      command_environment_(std::move(other.command_environment_)),
      empty_value_policy_(other.empty_value_policy_),
      provenance_(std::move(other.provenance_)),
      workspace_path_(std::move(other.workspace_path_)),
      workspace_device_(other.workspace_device_),
      workspace_inode_(other.workspace_inode_) {
}

ArtifactMakepkgContext::~ArtifactMakepkgContext() noexcept = default;

void ArtifactMakepkgContext::require_unchanged_checkout() const {
    checkout_.require_unchanged_identity();
    if(production_source_tree_ != nullptr) {
        production_source_tree_->require_unchanged_identity();
    }
}

void ArtifactMakepkgContext::require_matching_workspace(
    const ArtifactWorkspace& workspace) const {
    workspace.require_unchanged_identity();
    if(workspace.path_ != workspace_path_ ||
       workspace.device_ != workspace_device_ ||
       workspace.inode_ != workspace_inode_) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "makepkg".
            "Artifact {} context does not match the supplied workspace.",
            "makepkg"));
    }
}

std::string ArtifactMakepkgContext::command_environment_prefix() const {
    return serialize_source_build_environment(
        command_environment_, empty_value_policy_);
}

std::string ArtifactMakepkgContext::makepkg_command(
    const std::vector<std::string>& makepkg_arguments) const {
    const std::vector<std::string> assignment_words =
        materialize_source_build_environment_assignment_words(
            command_environment_, empty_value_policy_);
    std::vector<std::string> arguments;
    arguments.reserve(
        makepkg_arguments.size() + assignment_words.size() + 1);
    arguments.push_back("makepkg");
    arguments.insert(
        arguments.end(), makepkg_arguments.begin(),
        makepkg_arguments.end());
    arguments.insert(
        arguments.end(), assignment_words.begin(),
        assignment_words.end());
    return command_environment_prefix() + shell_words::join(arguments);
}

CapturedCommandResult ArtifactMakepkgContext::capture_makepkg_output(
    const ArtifactWorkspace& workspace,
    const std::vector<std::string>& makepkg_arguments) const {
    require_no_inherited_pkgdest();
    require_matching_workspace(workspace);
    require_unchanged_checkout();
    FileDescriptorWorkDirGuard working_directory(checkout_.descriptor_);
    std::string command = makepkg_command(makepkg_arguments);
    CapturedCommandResult result;
    if(production_source_tree_ != nullptr) {
        ProductionPackagelistCapture capture(
            workspace.directory_descriptor_,
            workspace.canonical_path());
        result.exit_code = production_source_tree_->run_guarded_command(
            command + " > " + shell_words::quote(capture.path().string()),
            command);
        result.output = capture.read_and_remove();
    } else {
        Logger::raw_cmd(command);
        result = capture_command_output_raw(command.c_str());
    }
    require_unchanged_checkout();
    require_matching_workspace(workspace);
    return result;
}

int ArtifactMakepkgContext::run_makepkg_build_only(
    const ArtifactWorkspace& workspace,
    const ExpectedPackageArtifactPath& expected,
    const ArtifactMakepkgBuildOptions& options) const {
    require_no_inherited_pkgdest();
    require_matching_workspace(workspace);
    expected.require_matching_makepkg_context(*this);
    require_unchanged_checkout();
    FileDescriptorWorkDirGuard working_directory(checkout_.descriptor_);
    std::vector<std::string> arguments = {"-sc"};
    if(options.no_confirm) arguments.emplace_back("--noconfirm");
    if(options.rebuild) arguments.emplace_back("-f");
    if(options.clean_build) arguments.emplace_back("-C");
    const std::string command = makepkg_command(arguments);
    if(production_source_tree_ != nullptr) {
        const int exit_code =
            production_source_tree_->run_guarded_command(command);
        try {
            require_unchanged_checkout();
            require_matching_workspace(workspace);
        } catch(...) {
            throw ProductionSourceBuildPostCommandRevalidationError(
                exit_code, std::current_exception());
        }
        return exit_code;
    }
    const int exit_code = run_command(command);
    require_unchanged_checkout();
    require_matching_workspace(workspace);
    return exit_code;
}

int ArtifactMakepkgContext::run_makepkg_build_only(
    const ArtifactWorkspace& workspace,
    const ExpectedPackageArtifactSet& expected,
    const ArtifactMakepkgBuildOptions& options) const {
    require_no_inherited_pkgdest();
    require_matching_workspace(workspace);
    expected.require_matching_workspace(workspace);
    expected.require_matching_makepkg_context(*this);
    require_unchanged_checkout();
    FileDescriptorWorkDirGuard working_directory(checkout_.descriptor_);
    std::vector<std::string> arguments = {"-sc"};
    if(options.no_confirm) arguments.emplace_back("--noconfirm");
    if(options.rebuild) arguments.emplace_back("-f");
    if(options.clean_build) arguments.emplace_back("-C");
    const std::string command = makepkg_command(arguments);
    if(production_source_tree_ != nullptr) {
        const int exit_code =
            production_source_tree_->run_guarded_command(command);
        try {
            require_unchanged_checkout();
            require_matching_workspace(workspace);
            expected.require_matching_workspace(workspace);
        } catch(...) {
            throw ProductionSourceBuildPostCommandRevalidationError(
                exit_code, std::current_exception());
        }
        return exit_code;
    }
    const int exit_code = run_command(command);
    require_unchanged_checkout();
    require_matching_workspace(workspace);
    expected.require_matching_workspace(workspace);
    return exit_code;
}

ArtifactMakepkgContext prepare_artifact_makepkg_context(
    const ValidatedCachePath& checkout,
    const ArtifactWorkspace& workspace,
    const SourceBuildEnvironment& environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy) {
    require_unclaimed_artifact_pkgdest(environment);
    workspace.require_unchanged_identity();
    if(!workspace.path().is_absolute()) {
        throw std::logic_error(localization::translate_message(
            "Artifact workspace path must be absolute."));
    }

    RetainedTrustedCacheDirectory retained_checkout =
        retain_trusted_cache_directory(checkout);
    SourceBuildEnvironment command_environment = environment;
    command_environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{
            "PKGDEST", workspace.canonical_path().string()});
    ArtifactMakepkgContext context(
        std::move(retained_checkout), std::move(command_environment),
        empty_value_policy,
        workspace.canonical_path(), workspace.device_, workspace.inode_);
    return context;
}

ArtifactMakepkgContext prepare_artifact_makepkg_context(
    ProductionArtifactSourceTree source_tree,
    const ArtifactWorkspace& workspace,
    const SourceBuildEnvironment& environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy) {
    require_unclaimed_artifact_pkgdest(environment);
    workspace.require_unchanged_identity();
    if(!workspace.path().is_absolute()) {
        throw std::logic_error(localization::translate_message(
            "Artifact workspace path must be absolute."));
    }

    source_tree.require_unchanged_identity();
    RetainedTrustedCacheDirectory retained_checkout =
        retain_trusted_cache_directory(source_tree.checkout());
    SourceBuildEnvironment command_environment = environment;
    command_environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{
            "PKGDEST", workspace.canonical_path().string()});
    return ArtifactMakepkgContext(
        std::move(retained_checkout), std::move(source_tree),
        std::move(command_environment), empty_value_policy,
        workspace.canonical_path(), workspace.device_,
        workspace.inode_);
}

ExpectedPackageArtifactPath::ExpectedPackageArtifactPath(
    fs::path path, std::string leaf_name,
    std::uintmax_t workspace_device, std::uintmax_t workspace_inode)
    : path_(std::move(path)), leaf_name_(std::move(leaf_name)),
      workspace_device_(workspace_device), workspace_inode_(workspace_inode) {
}

void ExpectedPackageArtifactPath::bind_makepkg_context(
    const ArtifactMakepkgContext& context) {
    if(makepkg_context_bound_) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "makepkg".
            "Expected artifact is already bound to a {} context.",
            "makepkg"));
    }
    makepkg_context_provenance_ = context.provenance_;
    makepkg_context_bound_ = true;
}

void ExpectedPackageArtifactPath::require_matching_makepkg_context(
    const ArtifactMakepkgContext& context) const {
    if(!makepkg_context_bound_ ||
       workspace_device_ != context.workspace_device_ ||
       workspace_inode_ != context.workspace_inode_ ||
       !makepkg_context_provenance_ ||
       makepkg_context_provenance_ != context.provenance_) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "makepkg".
            "Expected artifact does not belong to this {} context.",
            "makepkg"));
    }
}

ExpectedPackageArtifactPath validate_makepkg_packagelist_output(
    const ArtifactWorkspace& workspace, const std::string& raw_output) {
    workspace.require_unchanged_identity();
    std::vector<std::string> records = parse_packagelist_records(raw_output);
    if(records.size() != 1) {
        throw std::runtime_error(localization::format_translated_message(
            "Expected exactly one artifact path from {}, but got {}.",
            MAKEPKG_PACKAGELIST_COMMAND, records.size()));
    }

    fs::path candidate(records.front());
    require_direct_expected_path(workspace, candidate);
    std::string leaf_name = candidate.filename().string();
    if(entry_status_at(
           workspace.directory_descriptor_, leaf_name, candidate)
           .has_value()) {
        throw std::runtime_error(localization::translate_message(
            "Expected package artifact already exists before the build."));
    }

    std::string signature_leaf = leaf_name + ".sig";
    fs::path signature_path = workspace.canonical_path_ / signature_leaf;
    if(entry_status_at(
           workspace.directory_descriptor_, signature_leaf,
           signature_path)
           .has_value()) {
        throw std::runtime_error(localization::translate_message(
            "Expected package signature already exists before the build."));
    }
    workspace.require_unchanged_identity();
    return ExpectedPackageArtifactPath(
        std::move(candidate), std::move(leaf_name), workspace.device_,
        workspace.inode_);
}

ExpectedPackageArtifactPath query_makepkg_packagelist(
    const ArtifactWorkspace& workspace,
    const ArtifactMakepkgContext& context) {
    CapturedCommandResult result = context.capture_makepkg_output(
        workspace, {"--packagelist"});
    if(result.exit_code != 0) {
        throw std::runtime_error(localization::format_translated_message(
            "{} failed with exit status {}.",
            MAKEPKG_PACKAGELIST_COMMAND, result.exit_code));
    }
    ExpectedPackageArtifactPath expected =
        validate_makepkg_packagelist_output(workspace, result.output);
    expected.bind_makepkg_context(context);
    return expected;
}

ExpectedPackageArtifactSet::ExpectedPackageArtifactSet(
    std::vector<Entry> entries,
    std::uintmax_t workspace_device,
    std::uintmax_t workspace_inode) noexcept
    : entries_(std::move(entries)), workspace_device_(workspace_device),
      workspace_inode_(workspace_inode) {
}

void ExpectedPackageArtifactSet::bind_makepkg_context(
    const ArtifactMakepkgContext& context) {
    if(makepkg_context_bound_) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "makepkg".
            "Expected artifact set is already bound to a {} context.",
            "makepkg"));
    }
    makepkg_context_provenance_ = context.provenance_;
    makepkg_context_bound_ = true;
}

void ExpectedPackageArtifactSet::require_matching_makepkg_context(
    const ArtifactMakepkgContext& context) const {
    if(entries_.empty() || !makepkg_context_bound_ ||
       workspace_device_ != context.workspace_device_ ||
       workspace_inode_ != context.workspace_inode_ ||
       !makepkg_context_provenance_ ||
       makepkg_context_provenance_ != context.provenance_) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "makepkg".
            "Expected artifact set does not belong to this {} context.",
            "makepkg"));
    }
}

void ExpectedPackageArtifactSet::require_matching_workspace(
    const ArtifactWorkspace& workspace) const {
    workspace.require_unchanged_identity();
    if(entries_.empty() || workspace_device_ != workspace.device_ ||
       workspace_inode_ != workspace.inode_) {
        throw std::runtime_error(localization::translate_message(
            "Expected artifact set does not belong to this artifact workspace."));
    }

    std::set<std::string> artifact_leaves;
    std::set<std::string> signature_leaves;
    for(const Entry& entry : entries_) {
        if(entry.path.parent_path() != workspace.canonical_path_ ||
           entry.path.filename().string() != entry.leaf_name ||
           entry.leaf_name.empty() ||
           !artifact_leaves.insert(entry.leaf_name).second) {
            throw std::runtime_error(localization::translate_message(
                "Expected artifact set does not belong to this artifact workspace."));
        }
        signature_leaves.insert(entry.leaf_name + ".sig");
    }
    for(const std::string& signature_leaf : signature_leaves) {
        if(artifact_leaves.contains(signature_leaf)) {
            throw std::runtime_error(localization::translate_message(
                "Expected package artifact and signature namespaces collide."));
        }
    }
}

const fs::path& ExpectedPackageArtifactSet::path_at(
    std::size_t index) const {
    return entries_.at(index).path;
}

ExpectedPackageArtifactSet validate_makepkg_packagelist_output_set(
    const ArtifactWorkspace& workspace,
    const std::string& raw_output) {
    workspace.require_unchanged_identity();
    std::vector<std::string> records = parse_packagelist_records(raw_output);
    if(records.empty()) {
        throw std::runtime_error(localization::format_translated_message(
            "Expected one or more artifact paths from {}, but got 0.",
            MAKEPKG_PACKAGELIST_COMMAND));
    }

    std::vector<ExpectedPackageArtifactSet::Entry> entries;
    entries.reserve(records.size());
    std::set<std::string> artifact_leaves;
    std::set<std::string> signature_leaves;
    for(const std::string& record : records) {
        fs::path candidate(record);
        require_direct_expected_path(workspace, candidate);
        std::string leaf_name = candidate.filename().string();
        if(!artifact_leaves.insert(leaf_name).second) {
            throw std::runtime_error(localization::format_translated_message(
                "{} returned a duplicate artifact path.",
                MAKEPKG_PACKAGELIST_COMMAND));
        }
        signature_leaves.insert(leaf_name + ".sig");
        entries.push_back(ExpectedPackageArtifactSet::Entry{
            std::move(candidate), std::move(leaf_name)});
    }
    for(const std::string& signature_leaf : signature_leaves) {
        if(artifact_leaves.contains(signature_leaf)) {
            throw std::runtime_error(localization::format_translated_message(
                "The artifact and signature namespaces returned by {} collide.",
                MAKEPKG_PACKAGELIST_COMMAND));
        }
    }

    for(const ExpectedPackageArtifactSet::Entry& entry : entries) {
        if(entry_status_at(
               workspace.directory_descriptor_, entry.leaf_name,
               entry.path)
               .has_value()) {
            throw std::runtime_error(localization::translate_message(
                "Expected package artifact already exists before the build."));
        }

        const std::string signature_leaf = entry.leaf_name + ".sig";
        const fs::path signature_path =
            workspace.canonical_path_ / signature_leaf;
        if(entry_status_at(
               workspace.directory_descriptor_, signature_leaf,
               signature_path)
               .has_value()) {
            throw std::runtime_error(localization::translate_message(
                "Expected package signature already exists before the build."));
        }
    }
    workspace.require_unchanged_identity();
    return ExpectedPackageArtifactSet(
        std::move(entries), workspace.device_, workspace.inode_);
}

ExpectedPackageArtifactSet query_makepkg_packagelist_set(
    const ArtifactWorkspace& workspace,
    const ArtifactMakepkgContext& context) {
    CapturedCommandResult result = context.capture_makepkg_output(
        workspace, {"--packagelist"});
    if(result.exit_code != 0) {
        throw std::runtime_error(localization::format_translated_message(
            "{} failed with exit status {}.",
            MAKEPKG_PACKAGELIST_COMMAND, result.exit_code));
    }
    ExpectedPackageArtifactSet expected =
        validate_makepkg_packagelist_output_set(workspace, result.output);
    expected.bind_makepkg_context(context);
    return expected;
}

ValidatedPackageArtifactPath::ValidatedPackageArtifactPath(
    ArtifactWorkspace&& workspace, fs::path path, std::string leaf_name,
    int artifact_descriptor, std::uintmax_t device,
    std::uintmax_t inode, std::uintmax_t owner) noexcept
    : workspace_(std::move(workspace)), path_(std::move(path)),
      leaf_name_(std::move(leaf_name)),
      artifact_descriptor_(artifact_descriptor), device_(device), inode_(inode),
      owner_(owner) {
}

ValidatedPackageArtifactPath::ValidatedPackageArtifactPath(
    ValidatedPackageArtifactPath&& other) noexcept
    : workspace_(std::move(other.workspace_)), path_(std::move(other.path_)),
      leaf_name_(std::move(other.leaf_name_)),
      artifact_descriptor_(std::exchange(other.artifact_descriptor_, -1)),
      device_(other.device_), inode_(other.inode_), owner_(other.owner_) {
}

ValidatedPackageArtifactPath::~ValidatedPackageArtifactPath() noexcept {
    if(artifact_descriptor_ >= 0) close(artifact_descriptor_);
}

ValidatedPackageArtifactPath ValidatedPackageArtifactPath::validate_for_owners(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactPath& expected,
    std::uintmax_t expected_workspace_owner,
    std::uintmax_t expected_artifact_owner) {
    workspace.require_unchanged_identity_for_owner(expected_workspace_owner);
    if(expected.workspace_device_ != workspace.device_ ||
       expected.workspace_inode_ != workspace.inode_ ||
       expected.path_.parent_path() != workspace.canonical_path_ ||
       expected.path_.filename().string() != expected.leaf_name_) {
        throw std::runtime_error(localization::translate_message(
            "Expected artifact path does not belong to this artifact workspace."));
    }

    InspectedArtifact inspected = inspect_post_build_artifact(
        workspace.directory_descriptor_, workspace.canonical_path_,
        expected.leaf_name_, expected_artifact_owner);
    workspace.require_unchanged_identity_for_owner(expected_workspace_owner);

    fs::path artifact_path = expected.path_;
    std::string artifact_leaf = expected.leaf_name_;
    ValidatedPackageArtifactPath artifact(
        std::move(workspace), std::move(artifact_path),
        std::move(artifact_leaf), inspected.descriptor(),
        status_device(inspected.status), status_inode(inspected.status),
        status_owner(inspected.status));
    static_cast<void>(inspected.release_descriptor());
    return artifact;
}

void ValidatedPackageArtifactPath::require_validity_for_owner(
    std::uintmax_t expected_effective_user) const {
    workspace_.require_unchanged_identity_for_owner(expected_effective_user);
    if(artifact_descriptor_ < 0) {
        throw std::runtime_error(localization::format_translated_message(
            "Validated package artifact descriptor is closed: {}",
            path_.string()));
    }

    struct stat retained_status =
        require_descriptor_status(artifact_descriptor_, path_);
    if(!S_ISREG(retained_status.st_mode) ||
       status_device(retained_status) != device_ ||
       status_inode(retained_status) != inode_ ||
       status_owner(retained_status) != owner_ ||
       owner_ != expected_effective_user) {
        throw std::runtime_error(localization::format_translated_message(
            "Validated package artifact descriptor changed identity or owner: {}",
            path_.string()));
    }

    InspectedArtifact inspected = inspect_post_build_artifact(
        workspace_.directory_descriptor_, workspace_.canonical_path_,
        leaf_name_, expected_effective_user);
    if(status_device(inspected.status) != device_ ||
       status_inode(inspected.status) != inode_ ||
       status_owner(inspected.status) != owner_) {
        throw std::runtime_error(localization::format_translated_message(
            "Validated package artifact path changed identity or owner: {}",
            path_.string()));
    }
    workspace_.require_unchanged_identity_for_owner(expected_effective_user);
}

void ValidatedPackageArtifactPath::require_validity() const {
    require_validity_for_owner(static_cast<std::uintmax_t>(geteuid()));
}

void ValidatedPackageArtifactPath::retain_workspace_for_diagnostics() noexcept {
    workspace_.retain_for_diagnostics();
}

void ValidatedPackageArtifactPath::cleanup_workspace() {
    workspace_.cleanup();
    if(artifact_descriptor_ >= 0) {
        close(artifact_descriptor_);
        artifact_descriptor_ = -1;
    }
}

ValidatedPackageArtifactPath validate_post_build_package_artifact(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactPath& expected) {
    const std::uintmax_t effective_user =
        static_cast<std::uintmax_t>(geteuid());
    return ValidatedPackageArtifactPath::validate_for_owners(
        std::move(workspace), expected,
        effective_user, effective_user);
}

ValidatedPackageArtifactSet::Record::Record(
    fs::path artifact_path, std::string artifact_leaf,
    int retained_artifact_descriptor,
    std::uintmax_t retained_artifact_device,
    std::uintmax_t retained_artifact_inode,
    std::uintmax_t retained_artifact_owner,
    bool retained_signature,
    int retained_signature_descriptor,
    std::uintmax_t retained_signature_device,
    std::uintmax_t retained_signature_inode,
    std::uintmax_t retained_signature_owner) noexcept
    : path(std::move(artifact_path)), leaf_name(std::move(artifact_leaf)),
      artifact_descriptor(retained_artifact_descriptor),
      artifact_device(retained_artifact_device),
      artifact_inode(retained_artifact_inode),
      artifact_owner(retained_artifact_owner),
      has_signature(retained_signature),
      signature_descriptor(retained_signature_descriptor),
      signature_device(retained_signature_device),
      signature_inode(retained_signature_inode),
      signature_owner(retained_signature_owner) {
}

ValidatedPackageArtifactSet::Record::Record(Record&& other) noexcept
    : path(std::move(other.path)), leaf_name(std::move(other.leaf_name)),
      artifact_descriptor(std::exchange(other.artifact_descriptor, -1)),
      artifact_device(other.artifact_device),
      artifact_inode(other.artifact_inode),
      artifact_owner(other.artifact_owner),
      has_signature(std::exchange(other.has_signature, false)),
      signature_descriptor(std::exchange(other.signature_descriptor, -1)),
      signature_device(other.signature_device),
      signature_inode(other.signature_inode),
      signature_owner(other.signature_owner),
      sealed_artifact_descriptor(std::exchange(other.sealed_artifact_descriptor, -1)),
      sealed_signature_descriptor(std::exchange(other.sealed_signature_descriptor, -1)),
      metadata_path(std::move(other.metadata_path)) {
}

ValidatedPackageArtifactSet::Record::~Record() noexcept {
    if(artifact_descriptor >= 0) close(artifact_descriptor);
    if(signature_descriptor >= 0) close(signature_descriptor);
    if(sealed_artifact_descriptor >= 0) close(sealed_artifact_descriptor);
    if(sealed_signature_descriptor >= 0) close(sealed_signature_descriptor);
}

ValidatedPackageArtifactSet::ValidatedPackageArtifactSet(
    ArtifactWorkspace&& workspace,
    std::vector<Record>&& records) noexcept
    : workspace_(std::move(workspace)), records_(std::move(records)) {
}

ValidatedPackageArtifactSet::ValidatedPackageArtifactSet(
    ValidatedPackageArtifactSet&& other) noexcept
    : workspace_(std::move(other.workspace_)),
      records_(std::move(other.records_)),
      install_input_descriptor_(std::exchange(other.install_input_descriptor_, -1)),
      ownership_state_(std::exchange(
          other.ownership_state_, OwnershipState::Inactive)) {
}

ValidatedPackageArtifactSet::~ValidatedPackageArtifactSet() noexcept {
    if(install_input_descriptor_ >= 0) close(install_input_descriptor_);
}

void ValidatedPackageArtifactSet::bind_install_content() {
    require_validity();
    std::uint64_t total = 0;
    for(const auto& record : records_) {
        for(const auto descriptor : {record.artifact_descriptor, record.signature_descriptor}) {
            if(descriptor < 0) continue;
            struct stat metadata{};
            if(fstat(descriptor, &metadata) != 0 || metadata.st_size < 0 ||
               static_cast<std::uint64_t>(metadata.st_size) > SOURCE_ARTIFACT_INSTALL_MAXIMUM_TRANSACTION_BYTES - total)
                throw std::runtime_error("Artifact content snapshot exceeds the transport limit.");
            total += metadata.st_size;
        }
    }
    std::uint64_t remaining = SOURCE_ARTIFACT_INSTALL_MAXIMUM_TRANSACTION_BYTES;
    for(auto& record : records_) {
        if(record.sealed_artifact_descriptor >= 0) {
            for(const auto descriptor : {record.sealed_artifact_descriptor, record.sealed_signature_descriptor}) {
                if(descriptor < 0) continue;
                struct stat saved{};
                if(fstat(descriptor, &saved) != 0 || saved.st_size < 0 || static_cast<std::uint64_t>(saved.st_size) > remaining)
                    throw std::runtime_error("Artifact content snapshot exceeds the transport limit.");
                remaining -= saved.st_size;
            }
            continue;
        }
        auto archive = create_install_memfd();
        remaining -= copy_install_bytes(record.artifact_descriptor, archive.get(),
                                        std::min(remaining, SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES));
        seal_install_content(archive.get());
        auto signature = record.has_signature ? create_install_memfd() : OwnedFileDescriptor();
        if(record.has_signature) {
            remaining -= copy_install_bytes(record.signature_descriptor, signature.get(),
                                            std::min(remaining, SOURCE_ARTIFACT_INSTALL_MAXIMUM_SIGNATURE_BYTES));
            seal_install_content(signature.get());
        }
        record.metadata_path = "/proc/self/fd/" + std::to_string(archive.get());
        record.sealed_artifact_descriptor = archive.release();
        record.sealed_signature_descriptor = signature.release();
    }
    require_install_content();
}

void ValidatedPackageArtifactSet::require_install_content() const {
    require_validity();
    for(const auto& record : records_) {
        require_same_install_content(record.artifact_descriptor, record.sealed_artifact_descriptor);
        if(record.has_signature)
            require_same_install_content(record.signature_descriptor, record.sealed_signature_descriptor);
    }
    require_validity();
}

const fs::path& ValidatedPackageArtifactSet::metadata_path_at(std::size_t index) const {
    require_validity();
    const auto& record = records_.at(index);
    return record.sealed_artifact_descriptor >= 0 ? record.metadata_path : record.path;
}

int ValidatedPackageArtifactSet::create_install_input(const std::vector<std::size_t>& indices) {
    require_install_content();
    auto input = create_install_memfd();
    for(const auto index : indices) {
        const auto& record = records_.at(index);
        copy_install_bytes(record.sealed_artifact_descriptor, input.get());
        if(record.has_signature) copy_install_bytes(record.sealed_signature_descriptor, input.get());
    }
    seal_install_content(input.get());
    require_install_content();
    if(install_input_descriptor_ >= 0) close(install_input_descriptor_);
    install_input_descriptor_ = input.release();
    return install_input_descriptor_;
}

void ValidatedPackageArtifactSet::require_active() const {
    if(ownership_state_ != OwnershipState::Active || records_.empty()) {
        throw std::runtime_error(localization::translate_message(
            "Validated package artifact set is no longer owned."));
    }
}

void ValidatedPackageArtifactSet::require_workspace_ownership() const {
    if(ownership_state_ == OwnershipState::Inactive) {
        throw std::runtime_error(localization::translate_message(
            "Validated package artifact set workspace is no longer owned."));
    }
}

std::size_t ValidatedPackageArtifactSet::size() const {
    require_active();
    return records_.size();
}

const fs::path& ValidatedPackageArtifactSet::path_at(
    std::size_t index) const {
    require_active();
    return records_.at(index).path;
}

const fs::path& ValidatedPackageArtifactSet::workspace_path() const {
    require_workspace_ownership();
    return workspace_.path();
}

ValidatedPackageArtifactSet ValidatedPackageArtifactSet::validate_for_owners(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected,
    std::uintmax_t expected_workspace_owner,
    std::uintmax_t expected_artifact_owner,
    std::uintmax_t expected_signature_owner) {
    expected.require_matching_workspace(workspace);
    workspace.require_unchanged_identity_for_owner(expected_workspace_owner);

    std::vector<ArtifactInspectionTarget> targets;
    targets.reserve(expected.entries_.size());
    std::vector<Record> records;
    records.reserve(expected.entries_.size());
    for(const ExpectedPackageArtifactSet::Entry& entry : expected.entries_) {
        targets.push_back(ArtifactInspectionTarget{entry.path, entry.leaf_name});
        records.emplace_back(
            entry.path, entry.leaf_name,
            -1, 0, 0, 0,
            false, -1, 0, 0, 0);
    }
    std::vector<InspectedMultipleArtifact> inspected =
        inspect_post_build_artifact_set(
            workspace.directory_descriptor_, workspace.canonical_path_,
            targets, expected_artifact_owner,
            expected_signature_owner, true);
    workspace.require_unchanged_identity_for_owner(expected_workspace_owner);

    // LANDMINE(#268): filesystem最終検証後は、no-throwのidentity代入、
    // descriptor release、workspace/vector moveだけをownership commitに含める。
    for(std::size_t index = 0; index < inspected.size(); ++index) {
        const bool has_signature =
            inspected[index].signature_status.has_value();
        const struct stat* signature_status = has_signature
                                                  ? &inspected[index]
                                                         .signature_status
                                                         .value()
                                                  : nullptr;
        records[index].artifact_device =
            status_device(inspected[index].artifact_status);
        records[index].artifact_inode =
            status_inode(inspected[index].artifact_status);
        records[index].artifact_owner =
            status_owner(inspected[index].artifact_status);
        records[index].has_signature = has_signature;
        records[index].signature_device = signature_status != nullptr
                                              ? status_device(*signature_status)
                                              : 0;
        records[index].signature_inode = signature_status != nullptr
                                             ? status_inode(*signature_status)
                                             : 0;
        records[index].signature_owner = signature_status != nullptr
                                             ? status_owner(*signature_status)
                                             : 0;
        records[index].artifact_descriptor =
            inspected[index].release_artifact_descriptor();
        records[index].signature_descriptor =
            inspected[index].release_signature_descriptor();
    }
    return ValidatedPackageArtifactSet(
        std::move(workspace), std::move(records));
}

void ValidatedPackageArtifactSet::require_validity_for_owner(
    std::uintmax_t expected_effective_user) const {
    require_active();
    workspace_.require_unchanged_identity_for_owner(expected_effective_user);

    std::vector<ArtifactInspectionTarget> targets;
    targets.reserve(records_.size());
    for(const Record& record : records_) {
        if(record.path.parent_path() != workspace_.canonical_path_ ||
           record.path.filename().string() != record.leaf_name ||
           record.leaf_name.empty()) {
            throw std::runtime_error(localization::translate_message(
                "Validated package artifact set escaped its workspace."));
        }

        if(record.artifact_descriptor < 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Validated package artifact descriptor is closed: {}",
                record.path.string()));
        }
        const struct stat retained_artifact_status =
            require_descriptor_status(
                record.artifact_descriptor, record.path);
        require_regular_owned_entry(
            retained_artifact_status, expected_effective_user,
            record.path);
        if(status_device(retained_artifact_status) != record.artifact_device ||
           status_inode(retained_artifact_status) != record.artifact_inode ||
           status_owner(retained_artifact_status) != record.artifact_owner) {
            throw std::runtime_error(localization::format_translated_message(
                "Validated package artifact descriptor changed identity or owner: {}",
                record.path.string()));
        }

        const fs::path signature_path =
            workspace_.canonical_path_ / (record.leaf_name + ".sig");
        if(record.has_signature) {
            if(record.signature_descriptor < 0) {
                throw std::runtime_error(
                    localization::format_translated_message(
                        "Validated package signature descriptor is closed: {}",
                        signature_path.string()));
            }
            const struct stat retained_signature_status =
                require_descriptor_status(
                    record.signature_descriptor, signature_path);
            require_regular_owned_entry(
                retained_signature_status, expected_effective_user,
                signature_path);
            if(status_device(retained_signature_status) !=
                   record.signature_device ||
               status_inode(retained_signature_status) !=
                   record.signature_inode ||
               status_owner(retained_signature_status) !=
                   record.signature_owner) {
                throw std::runtime_error(
                    localization::format_translated_message(
                        "Validated package signature descriptor changed identity or owner: {}",
                        signature_path.string()));
            }
        } else if(record.signature_descriptor >= 0) {
            throw std::runtime_error(localization::format_translated_message(
                "Validated package signature state is inconsistent: {}",
                signature_path.string()));
        }
        targets.push_back(
            ArtifactInspectionTarget{record.path, record.leaf_name});
    }

    std::vector<InspectedMultipleArtifact> current =
        inspect_post_build_artifact_set(
            workspace_.directory_descriptor_,
            workspace_.canonical_path_, targets,
            expected_effective_user, expected_effective_user, false);
    for(std::size_t index = 0; index < records_.size(); ++index) {
        const Record& record = records_[index];
        if(status_device(current[index].artifact_status) !=
               record.artifact_device ||
           status_inode(current[index].artifact_status) !=
               record.artifact_inode ||
           status_owner(current[index].artifact_status) !=
               record.artifact_owner) {
            throw std::runtime_error(localization::format_translated_message(
                "Validated package artifact path changed identity or owner: {}",
                record.path.string()));
        }
        if(current[index].signature_status.has_value() !=
           record.has_signature) {
            throw std::runtime_error(localization::format_translated_message(
                "Validated package signature presence changed: {}",
                (workspace_.canonical_path_ /
                 (record.leaf_name + ".sig"))
                    .string()));
        }
        if(record.has_signature &&
           (status_device(current[index].signature_status.value()) !=
                record.signature_device ||
            status_inode(current[index].signature_status.value()) !=
                record.signature_inode ||
            status_owner(current[index].signature_status.value()) !=
                record.signature_owner)) {
            throw std::runtime_error(localization::format_translated_message(
                "Validated package signature path changed identity or owner: {}",
                (workspace_.canonical_path_ /
                 (record.leaf_name + ".sig"))
                    .string()));
        }
    }
    workspace_.require_unchanged_identity_for_owner(expected_effective_user);
}

void ValidatedPackageArtifactSet::require_validity() const {
    require_validity_for_owner(static_cast<std::uintmax_t>(geteuid()));
}

void ValidatedPackageArtifactSet::retain_workspace_for_diagnostics() {
    require_workspace_ownership();
    workspace_.retain_for_diagnostics();
}

void ValidatedPackageArtifactSet::cleanup_workspace() {
    require_workspace_ownership();
    if(ownership_state_ == OwnershipState::Active) {
        // POLICY(#268): cleanup entryでaggregate全体を再証明した後だけ、
        // retained artifact/signature descriptorを閉じる。
        require_validity();
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
        std::vector<int> retained_descriptors;
        retained_descriptors.reserve(records_.size() * 2);
        for(const Record& record : records_) {
            if(record.artifact_descriptor >= 0) {
                retained_descriptors.push_back(record.artifact_descriptor);
            }
            if(record.signature_descriptor >= 0) {
                retained_descriptors.push_back(record.signature_descriptor);
            }
        }
#endif
        // ここからvalidated artifact capabilityは失効させるが、workspace
        // ownershipはdiagnostic retentionとcleanup retryのため保持する。
        records_.clear();
        if(install_input_descriptor_ >= 0) close(std::exchange(install_input_descriptor_, -1));
        ownership_state_ = OwnershipState::WorkspaceCleanupPending;
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
        notify_multiple_artifact_cleanup_for_test(
            workspace_.path(), retained_descriptors);
#endif
    }
    workspace_.cleanup();
    ownership_state_ = OwnershipState::Inactive;
}

ValidatedPackageArtifactSet validate_post_build_package_artifacts(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected) {
    const std::uintmax_t effective_user =
        static_cast<std::uintmax_t>(geteuid());
    return ValidatedPackageArtifactSet::validate_for_owners(
        std::move(workspace), expected,
        effective_user, effective_user, effective_user);
}

#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
void require_artifact_workspace_identity_for_test(
    const ArtifactWorkspace& workspace,
    std::uintmax_t expected_effective_user) {
    workspace.require_unchanged_identity_for_owner(expected_effective_user);
}

ValidatedPackageArtifactPath validate_post_build_package_artifact_for_test(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactPath& expected,
    std::uintmax_t expected_artifact_owner) {
    return ValidatedPackageArtifactPath::validate_for_owners(
        std::move(workspace), expected,
        static_cast<std::uintmax_t>(geteuid()),
        expected_artifact_owner);
}

void set_multiple_artifact_validation_observer_for_test(
    MultipleArtifactValidationObserverForTest observer) {
    g_multiple_artifact_validation_observer = observer;
}

void set_multiple_artifact_cleanup_observer_for_test(
    MultipleArtifactCleanupObserverForTest observer) {
    g_multiple_artifact_cleanup_observer = observer;
}

void set_artifact_workspace_creation_observer_for_test(
    ArtifactWorkspaceCreationObserverForTest observer) {
    g_artifact_workspace_creation_observer = observer;
}

void set_artifact_workspace_cleanup_pre_delete_observer_for_test(
    ArtifactWorkspaceCleanupPreDeleteObserverForTest observer) {
    g_artifact_workspace_cleanup_pre_delete_observer = observer;
}

void set_artifact_workspace_cleanup_child_open_for_test(
    ArtifactWorkspaceCleanupChildOpenForTest open_child) noexcept {
    g_artifact_workspace_cleanup_child_open = open_child;
}

ValidatedPackageArtifactSet validate_post_build_package_artifacts_for_test(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected,
    std::uintmax_t expected_artifact_owner) {
    return ValidatedPackageArtifactSet::validate_for_owners(
        std::move(workspace), expected,
        static_cast<std::uintmax_t>(geteuid()),
        expected_artifact_owner, expected_artifact_owner);
}

ValidatedPackageArtifactSet validate_post_build_package_artifacts_for_test(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected,
    std::uintmax_t expected_workspace_owner,
    std::uintmax_t expected_artifact_owner,
    std::uintmax_t expected_signature_owner) {
    return ValidatedPackageArtifactSet::validate_for_owners(
        std::move(workspace), expected,
        expected_workspace_owner, expected_artifact_owner,
        expected_signature_owner);
}
#endif
