#include "source_artifact_install_trusted_helper_state.hpp"

#include "trusted_alpm_receipt_protocol.hpp"
#include "xdg_generation_store.hpp"

#include <alpm.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <linux/fs.h>

#ifndef MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH
#error "MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH is required"
#endif

namespace {

constexpr mode_t PRIVATE_DIRECTORY_MODE = 0700;
constexpr mode_t PRIVATE_FILE_MODE = 0600;
constexpr std::string_view PREPARED_FILE = "prepared";
constexpr std::string_view IDENTITY_FILE = "identity";
constexpr std::string_view LIFETIME_FILE = "lifetime";
constexpr std::string_view EXECUTION_FILE = "execution";
constexpr std::string_view AUTHORIZED_FILE = "authorized";
constexpr std::string_view REFUSAL_FILE = "refusal";
constexpr std::string_view OBSERVED_FILE = "execution-observed";
constexpr std::size_t MAX_IDENTITY_BYTES = 1024U * 1024U;
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
SourceArtifactInstallTrustedStateTestHook g_state_test_hook;
std::function<int(const std::vector<std::string>&)> g_exec_test_hook;
#endif
constexpr std::string_view HOOK_DIRECTORY = "hooks";
constexpr std::string_view ARTIFACT_DIRECTORY = "artifacts";
constexpr std::string_view RECEIPT_FILE = "receipt";
constexpr std::string_view PARTIAL_RECEIPT_FILE = "receipt.partial";
constexpr std::string_view EXACT_DIRECTORY = "exact";
constexpr std::string_view DATABASE_WORLD_FILE = "world";
constexpr std::array<std::string_view, 5> EXACT_RECORD_FILES = {
    "baseline", "install", "upgrade", "install-anchor", "upgrade-anchor"};

class OwnedDescriptor final {
public:
    OwnedDescriptor() noexcept = default;
    explicit OwnedDescriptor(int descriptor) noexcept
        : descriptor_(descriptor) {
    }
    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;
    OwnedDescriptor(OwnedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }
    OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept {
        if(this == &other) return *this;
        reset();
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }
    ~OwnedDescriptor() {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    void reset() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
        descriptor_ = -1;
    }

    int descriptor_ = -1;
};

struct AlpmHandleDeleter {
    void operator()(alpm_handle_t* handle) const noexcept {
        if(handle != nullptr) static_cast<void>(alpm_release(handle));
    }
};

struct AlpmPackageDeleter {
    void operator()(alpm_pkg_t* package) const noexcept {
        if(package != nullptr) static_cast<void>(alpm_pkg_free(package));
    }
};

using AlpmHandle = std::unique_ptr<alpm_handle_t, AlpmHandleDeleter>;
using AlpmPackage = std::unique_ptr<alpm_pkg_t, AlpmPackageDeleter>;

[[noreturn]] void throw_state_error(const std::string& action) {
    const int error_number = errno;
    throw SourceArtifactInstallTrustedStateError(
        SourceArtifactInstallSealingFailure::StagedArtifactRevalidationFailure,
        action + ": " + std::strerror(error_number), error_number);
}

[[noreturn]] void throw_state_error_message(const std::string& message) {
    throw SourceArtifactInstallTrustedStateError(message);
}

bool same_identity(const struct stat& lhs, const struct stat& rhs) noexcept {
    return lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
           lhs.st_mode == rhs.st_mode && lhs.st_uid == rhs.st_uid;
}

struct stat require_descriptor_metadata(
    int descriptor, uid_t expected_owner, mode_t expected_type,
    std::optional<mode_t> exact_permissions,
    const std::string& description) {
    struct stat metadata{};
    if(fstat(descriptor, &metadata) == -1) {
        throw_state_error("unable to inspect " + description);
    }
    if((metadata.st_mode & S_IFMT) != expected_type ||
       metadata.st_uid != expected_owner ||
       (expected_type == S_IFREG && metadata.st_nlink != 1)) {
        throw_state_error_message(
            description + " has an unexpected type or owner");
    }
    const mode_t permissions = metadata.st_mode & 07777;
    if(exact_permissions.has_value()) {
        if(permissions != *exact_permissions) {
            throw_state_error_message(
                description + " has unexpected permissions");
        }
    } else if((permissions & (S_IWGRP | S_IWOTH)) != 0) {
        throw_state_error_message(
            description + " is group- or world-writable");
    }
    return metadata;
}

void require_named_identity(
    int parent_fd, const std::string& name,
    const struct stat& expected_metadata,
    const std::string& description) {
    struct stat named_metadata{};
    if(fstatat(
           parent_fd, name.c_str(), &named_metadata,
           AT_SYMLINK_NOFOLLOW) == -1) {
        throw_state_error("unable to revalidate " + description);
    }
    if(!same_identity(expected_metadata, named_metadata)) {
        throw_state_error_message(description + " identity changed");
    }
}

OwnedDescriptor open_directory_at(
    int parent_fd, const std::string& name, uid_t expected_owner,
    mode_t exact_permissions, const std::string& description) {
    const int descriptor = openat(
        parent_fd, name.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor == -1) {
        throw_state_error("unable to open " + description);
    }
    OwnedDescriptor owned(descriptor);
    const struct stat metadata = require_descriptor_metadata(
        descriptor, expected_owner, S_IFDIR, exact_permissions,
        description);
    require_named_identity(parent_fd, name, metadata, description);
    return owned;
}

OwnedDescriptor ensure_private_directory(
    int parent_fd, const std::string& name, uid_t expected_owner,
    const std::string& description) {
    if(mkdirat(parent_fd, name.c_str(), PRIVATE_DIRECTORY_MODE) == -1 &&
       errno != EEXIST) {
        throw_state_error("unable to create " + description);
    }
    return open_directory_at(
        parent_fd, name, expected_owner, PRIVATE_DIRECTORY_MODE,
        description);
}

bool entry_exists(int parent_fd, const std::string& name) {
    struct stat metadata{};
    if(fstatat(
           parent_fd, name.c_str(), &metadata,
           AT_SYMLINK_NOFOLLOW) == 0) {
        return true;
    }
    if(errno == ENOENT) return false;
    throw_state_error("unable to inspect source-artifact state entry");
}

void require_entry_absent(
    int parent_fd, const std::string& name,
    const std::string& description) {
    if(entry_exists(parent_fd, name)) {
        throw_state_error_message(description + " already exists");
    }
}

std::vector<std::string> list_directory_entries(int directory_fd) {
    const int duplicate = openat(
        directory_fd, ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(duplicate == -1) {
        throw_state_error("unable to duplicate source-artifact directory");
    }
    DIR* directory = fdopendir(duplicate);
    if(directory == nullptr) {
        const int error_number = errno;
        static_cast<void>(close(duplicate));
        errno = error_number;
        throw_state_error("unable to enumerate source-artifact directory");
    }

    std::vector<std::string> entries;
    errno = 0;
    while(dirent* entry = readdir(directory)) {
        const std::string_view name(entry->d_name);
        if(name != "." && name != "..") entries.emplace_back(name);
        errno = 0;
    }
    const int read_error = errno;
    if(closedir(directory) == -1 && read_error == 0) {
        throw_state_error("unable to close source-artifact enumeration");
    }
    if(read_error != 0) {
        errno = read_error;
        throw_state_error("unable to enumerate source-artifact directory");
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

void require_exact_entries(
    int directory_fd, std::vector<std::string> expected,
    const std::string& description) {
    std::sort(expected.begin(), expected.end());
    if(list_directory_entries(directory_fd) != expected) {
        throw_state_error_message(
            description + " contains unexpected runtime state");
    }
}

OwnedDescriptor create_private_file(
    int parent_fd, const std::string& name, uid_t expected_owner,
    const std::string& description) {
    const int descriptor = openat(
        parent_fd, name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        PRIVATE_FILE_MODE);
    if(descriptor == -1) {
        throw_state_error("unable to create " + description);
    }
    OwnedDescriptor owned(descriptor);
    if(fchmod(descriptor, PRIVATE_FILE_MODE) == -1) {
        throw_state_error("unable to set permissions on " + description);
    }
    const struct stat metadata = require_descriptor_metadata(
        descriptor, expected_owner, S_IFREG, PRIVATE_FILE_MODE,
        description);
    require_named_identity(parent_fd, name, metadata, description);
    return owned;
}

OwnedDescriptor open_private_file(
    int parent_fd, const std::string& name, uid_t expected_owner,
    const std::string& description,
    std::optional<std::uint64_t> expected_size = std::nullopt) {
    const int descriptor = openat(
        parent_fd, name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor == -1) {
        throw_state_error("unable to open " + description);
    }
    OwnedDescriptor owned(descriptor);
    const struct stat metadata = require_descriptor_metadata(
        descriptor, expected_owner, S_IFREG, PRIVATE_FILE_MODE,
        description);
    if(expected_size.has_value() &&
       (metadata.st_size < 0 ||
        static_cast<std::uint64_t>(metadata.st_size) !=
            *expected_size)) {
        throw_state_error_message(description + " has an unexpected size");
    }
    require_named_identity(parent_fd, name, metadata, description);
    return owned;
}

void write_all(int descriptor, std::string_view bytes) {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const ssize_t written = write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written == -1 && errno == EINTR) continue;
        throw_state_error("unable to write source-artifact state");
    }
}

void copy_exact_range(
    int source_fd, std::uint64_t source_offset,
    int destination_fd, std::uint64_t byte_count) {
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t copied = 0;
    while(copied < byte_count) {
        const std::uint64_t remaining = byte_count - copied;
        const std::size_t request_size = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        const std::uint64_t absolute_offset = source_offset + copied;
        if(absolute_offset >
           static_cast<std::uint64_t>(
               std::numeric_limits<off_t>::max())) {
            throw_state_error_message(
                "sealed source-artifact input offset is too large");
        }
        const ssize_t count = pread(
            source_fd, buffer.data(), request_size,
            static_cast<off_t>(absolute_offset));
        if(count > 0) {
            std::size_t written_offset = 0;
            const std::size_t count_size = static_cast<std::size_t>(count);
            while(written_offset < count_size) {
                const ssize_t written = write(
                    destination_fd, buffer.data() + written_offset,
                    count_size - written_offset);
                if(written > 0) {
                    written_offset += static_cast<std::size_t>(written);
                    continue;
                }
                if(written == -1 && errno == EINTR) continue;
                throw_state_error("unable to stage source-artifact bytes");
            }
            copied += count_size;
            continue;
        }
        if(count == -1 && errno == EINTR) continue;
        if(count == 0) {
            throw_state_error_message(
                "sealed source-artifact input ended unexpectedly");
        }
        throw_state_error("unable to read sealed source-artifact input");
    }
}

std::string read_bounded(
    int descriptor, std::size_t maximum_bytes,
    const std::string& description) {
    std::string bytes;
    std::array<char, 4096> buffer{};
    while(true) {
        const ssize_t count = read(descriptor, buffer.data(), buffer.size());
        if(count > 0) {
            const std::size_t count_size = static_cast<std::size_t>(count);
            if(bytes.size() > maximum_bytes ||
               count_size > maximum_bytes - bytes.size()) {
                throw_state_error_message(description + " is too large");
            }
            bytes.append(buffer.data(), count_size);
            continue;
        }
        if(count == 0) return bytes;
        if(errno == EINTR) continue;
        throw_state_error("unable to read " + description);
    }
}

void synchronize_file(int descriptor, const std::string& description) {
    if(fsync(descriptor) == -1) {
        throw_state_error("unable to synchronize " + description);
    }
}

void rename_noreplace(
    int old_parent_fd, const std::string& old_name,
    int new_parent_fd, const std::string& new_name,
    const std::string& description) {
#ifdef SYS_renameat2
    long result;
    do {
        result = syscall(
            SYS_renameat2, old_parent_fd, old_name.c_str(),
            new_parent_fd, new_name.c_str(), RENAME_NOREPLACE);
    } while(result == -1 && errno == EINTR);
    if(result == 0) return;
    throw_state_error("unable to atomically publish " + description);
#else
    static_cast<void>(old_parent_fd);
    static_cast<void>(old_name);
    static_cast<void>(new_parent_fd);
    static_cast<void>(new_name);
    throw_state_error_message(
        "renameat2 is unavailable for source-artifact publication");
#endif
}

std::string preparing_name(const std::string& transaction_token) {
    return "." + transaction_token + ".preparing";
}

std::string staged_artifact_filename(std::size_t ordinal) {
    return "artifact-" + std::to_string(ordinal) + ".pkg.tar.zst";
}

std::string staged_signature_filename(std::size_t ordinal) {
    return staged_artifact_filename(ordinal) + ".sig";
}

std::string hook_contents(const std::string& transaction_token) {
    return "[Trigger]\n"
           "Operation = Install\n"
           "Type = Package\n"
           "Target = *\n"
           "\n"
           "[Action]\n"
           "Description = Record Moguet source-artifact installs\n"
           "When = PostTransaction\n"
           "Exec = " MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH
           " record " +
           transaction_token + "\nNeedsTargets\n";
}

std::string execution_hook_filename(const std::string& token) {
    return "moguet-source-artifact-execution-" + token + ".hook";
}

std::string execution_hook_contents(const std::string& token) {
    // A missing/failed observation must not change pacman's transaction policy:
    // there is deliberately no AbortOnFail. Lack of proof stays Unknown later.
    return "[Trigger]\nOperation = Install\nOperation = Upgrade\nType = Package\nTarget = *\n\n"
           "[Action]\nDescription = Check Moguet package inputs\nWhen = PreTransaction\nExec = " MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH " observe-execution " +
           token + "\n";
}

std::string exact_operation_hook_filename(const std::string& token, bool upgrade) {
    return "moguet-exact-" + std::string(upgrade ? "upgrade-" : "install-") + token + ".hook";
}

std::string exact_operation_hook_contents(const std::string& token, bool upgrade) {
    return "[Trigger]\nOperation = " + std::string(upgrade ? "Upgrade" : "Install") +
           "\nType = Package\nTarget = *\n\n[Action]\nDescription = Record exact Moguet package operation\n"
           "When = PostTransaction\nExec = " MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH +
           std::string(upgrade ? " record-upgrade " : " record-install ") + token + "\nNeedsTargets\n";
}

std::vector<std::string> expected_hook_entries(const SourceArtifactInstallRootPrepareRequest& request) {
    const auto& token = request.transaction_token;
    if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
        return {exact_operation_hook_filename(token, false), exact_operation_hook_filename(token, true), execution_hook_filename(token)};
    return {source_artifact_install_hook_filename(token), execution_hook_filename(token)};
}

std::string expected_hook_contents(const SourceArtifactInstallRootPrepareRequest& request, const std::string& name) {
    if(name == execution_hook_filename(request.transaction_token)) return execution_hook_contents(request.transaction_token);
    if(request.purpose == SourceArtifactInstallTrustedPurpose::CleanupInstallOnly) return hook_contents(request.transaction_token);
    return exact_operation_hook_contents(request.transaction_token, name == exact_operation_hook_filename(request.transaction_token, true));
}

bool unlink_if_present(int parent_fd, const std::string& name, int flags = 0) {
    if(unlinkat(parent_fd, name.c_str(), flags) == 0) return true;
    return errno == ENOENT;
}

std::vector<std::string> expected_artifact_entries(
    const SourceArtifactInstallRootPrepareRequest& request) {
    std::vector<std::string> entries;
    entries.reserve(request.artifacts.size() * 2);
    for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
        entries.push_back(staged_artifact_filename(index));
        if(request.artifacts[index].signature_size > 0) {
            entries.push_back(staged_signature_filename(index));
        }
    }
    return entries;
}

bool cleanup_preparing_directory(
    int active_fd, const std::string& staging_name,
    const SourceArtifactInstallRootPrepareRequest& request) noexcept {
    try {
        const int staging_descriptor = openat(
            active_fd, staging_name.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if(staging_descriptor == -1) return errno == ENOENT;
        OwnedDescriptor staging(staging_descriptor);

        const int hooks_descriptor = openat(
            staging.get(), std::string(HOOK_DIRECTORY).c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if(hooks_descriptor >= 0) {
            OwnedDescriptor hooks(hooks_descriptor);
            for(const auto& name : expected_hook_entries(request))
                static_cast<void>(unlink_if_present(hooks.get(), name));
        }
        static_cast<void>(unlink_if_present(
            staging.get(), std::string(HOOK_DIRECTORY), AT_REMOVEDIR));

        const int artifacts_descriptor = openat(
            staging.get(), std::string(ARTIFACT_DIRECTORY).c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if(artifacts_descriptor >= 0) {
            OwnedDescriptor artifacts(artifacts_descriptor);
            for(const std::string& name : expected_artifact_entries(request)) {
                static_cast<void>(unlink_if_present(artifacts.get(), name));
            }
        }
        static_cast<void>(unlink_if_present(
            staging.get(), std::string(ARTIFACT_DIRECTORY),
            AT_REMOVEDIR));
        static_cast<void>(unlink_if_present(
            staging.get(), std::string(PREPARED_FILE)));
        static_cast<void>(unlink_if_present(staging.get(), std::string(IDENTITY_FILE)));
        static_cast<void>(unlink_if_present(staging.get(), std::string(LIFETIME_FILE)));
        if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
            OwnedDescriptor exact(openat(staging.get(), std::string(EXACT_DIRECTORY).c_str(),
                                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if(exact.get() >= 0) static_cast<void>(unlink_if_present(exact.get(), std::string(DATABASE_WORLD_FILE)));
            static_cast<void>(unlink_if_present(staging.get(), std::string(EXACT_DIRECTORY), AT_REMOVEDIR));
        }
        return unlink_if_present(active_fd, staging_name, AT_REMOVEDIR);
    } catch(...) {
        return false;
    }
}

struct OpenTransaction {
    OwnedDescriptor descriptor;
    struct stat metadata{};
};

OpenTransaction open_transaction(
    int active_fd, uid_t expected_owner,
    const std::string& transaction_token) {
    OwnedDescriptor descriptor = open_directory_at(
        active_fd, transaction_token, expected_owner,
        PRIVATE_DIRECTORY_MODE,
        "active source-artifact transaction");
    const struct stat metadata = require_descriptor_metadata(
        descriptor.get(), expected_owner, S_IFDIR,
        PRIVATE_DIRECTORY_MODE,
        "active source-artifact transaction");
    return OpenTransaction{std::move(descriptor), metadata};
}

void require_archive_identity(
    int artifact_fd,
    const SourceArtifactInstallRootArtifactExpectation& expected) {
    alpm_errno_t initialization_error = ALPM_ERR_OK;
    AlpmHandle handle(
        alpm_initialize("/", "/var/lib/pacman", &initialization_error));
    if(handle == nullptr) {
        throw_state_error_message(
            "unable to initialize libalpm for staged artifact metadata");
    }
    const std::string descriptor_path =
        "/proc/self/fd/" + std::to_string(artifact_fd);
    alpm_pkg_t* loaded_package = nullptr;
    if(alpm_pkg_load(
           handle.get(), descriptor_path.c_str(), 0, 0,
           &loaded_package) != 0 ||
       loaded_package == nullptr) {
        throw_state_error_message(
            "unable to read staged artifact metadata");
    }
    AlpmPackage package(loaded_package);
    const char* package_name = alpm_pkg_get_name(package.get());
    const char* full_version = alpm_pkg_get_version(package.get());
    const char* package_base = alpm_pkg_get_base(package.get());
    const char* architecture = alpm_pkg_get_arch(package.get());
    if(package_name == nullptr || full_version == nullptr ||
       package_base == nullptr || architecture == nullptr ||
       expected.package_name != package_name ||
       expected.full_version != full_version ||
       expected.package_base != package_base ||
       expected.architecture != architecture) {
        throw_state_error_message(
            "staged artifact metadata does not match the prepared identity");
    }
}


// This is a live transaction identity, not an installed-record generation.
// Regular-file ctime/mtime and link count detect drift in addition to dev/ino.
// Directory contents change during other transactions and receipt publication;
// their identity uses dev/ino/type/owner/mode, not directory timestamps.
struct StagedFilesystemIdentity {
    std::uintmax_t device, inode, mode, owner, group, links, size;
    std::intmax_t mtime_seconds, mtime_nanoseconds, ctime_seconds, ctime_nanoseconds;

    static StagedFilesystemIdentity observe(int descriptor) {
        struct stat metadata{};
        if(fstat(descriptor, &metadata) != 0) throw_state_error("unable to observe staged generation");
        const bool file = S_ISREG(metadata.st_mode);
        return {static_cast<std::uintmax_t>(metadata.st_dev),
                static_cast<std::uintmax_t>(metadata.st_ino),
                static_cast<std::uintmax_t>(metadata.st_mode),
                static_cast<std::uintmax_t>(metadata.st_uid),
                static_cast<std::uintmax_t>(metadata.st_gid),
                file ? static_cast<std::uintmax_t>(metadata.st_nlink) : 0,
                file ? static_cast<std::uintmax_t>(metadata.st_size) : 0,
                file ? metadata.st_mtim.tv_sec : 0, file ? metadata.st_mtim.tv_nsec : 0,
                file ? metadata.st_ctim.tv_sec : 0, file ? metadata.st_ctim.tv_nsec : 0};
    }
    bool operator==(const StagedFilesystemIdentity&) const = default;
    std::string serialize(const std::string& name) const {
        return name + "\t" + std::to_string(device) + "\t" + std::to_string(inode) +
               "\t" + std::to_string(mode) + "\t" + std::to_string(owner) +
               "\t" + std::to_string(group) + "\t" + std::to_string(links) +
               "\t" + std::to_string(size) + "\t" + std::to_string(mtime_seconds) +
               "\t" + std::to_string(mtime_nanoseconds) + "\t" + std::to_string(ctime_seconds) +
               "\t" + std::to_string(ctime_nanoseconds) + "\n";
    }
};

std::string descriptor_digest(int descriptor, std::uint64_t size) {
    try {
        return xdg_generation_store_file_descriptor_sha256(
            descriptor, size, SOURCE_ARTIFACT_INSTALL_MAXIMUM_TRANSACTION_BYTES);
    } catch(const std::system_error& error) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::StagedArtifactRevalidationFailure,
            "unable to hash staged descriptor", error.code().value());
    } catch(const std::exception& error) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::StagedArtifactRevalidationFailure,
            error.what());
    }
}

void require_staged_identity(int parent, const std::string& name,
                             int descriptor, const StagedFilesystemIdentity& before) {
    struct stat named{};
    if(fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
       before.device != static_cast<std::uintmax_t>(named.st_dev) ||
       before.inode != static_cast<std::uintmax_t>(named.st_ino)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::StagedArtifactReplacement,
            "staged pathname no longer names its retained object");
    }
    if(before != StagedFilesystemIdentity::observe(descriptor)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::StagedArtifactGenerationMismatch,
            "staged descriptor changed during observation");
    }
}

std::string lifetime_lease_contents(const std::string& token) {
    return "MOGUET-TRANSACTION-LIFETIME\t2\nTOKEN\t" + token + "\n";
}

OwnedDescriptor acquire_lifetime_lease(int transaction_fd, uid_t owner,
                                       const std::string& token, int lock_mode) {
    // Never create here: every published transaction already owns exactly one
    // lease. The held descriptor is included in the subsequent generation proof,
    // so locking an unlinked/recreated object cannot authorize another lifetime.
    auto lease = open_private_file(transaction_fd, std::string(LIFETIME_FILE), owner, "transaction lifetime lease");
    const auto identity = StagedFilesystemIdentity::observe(lease.get());
    if(identity.group != StagedFilesystemIdentity::observe(transaction_fd).group ||
       read_bounded(lease.get(), 256, "transaction lifetime lease") != lifetime_lease_contents(token)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
            "transaction lifetime lease has an unexpected group or token");
    }
    int result;
    do {
        result = flock(lease.get(), lock_mode | LOCK_NB);
    } while(result != 0 && errno == EINTR);
    if(result != 0) {
        const int error_number = errno;
        if(error_number == EWOULDBLOCK || error_number == EAGAIN) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TransactionLifetimeBusy,
                "source-artifact transaction lifetime is busy", error_number);
        }
        throw_state_error("unable to acquire transaction lifetime lease");
    }
    require_staged_identity(transaction_fd, std::string(LIFETIME_FILE), lease.get(), identity);
    return lease;
}

void require_transaction_entries(int descriptor, std::vector<std::string> expected,
                                 uid_t owner, const std::string& description) {
    static_cast<void>(open_private_file(descriptor, std::string(LIFETIME_FILE), owner, "transaction lifetime lease"));
    expected.emplace_back(LIFETIME_FILE);
    if(entry_exists(descriptor, std::string(EXACT_DIRECTORY))) {
        auto prepared = open_private_file(descriptor, std::string(PREPARED_FILE), owner, "exact purpose");
        const auto parsed = parse_source_artifact_install_root_prepared_state(
            read_bounded(prepared.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "exact purpose"));
        const auto* request = std::get_if<SourceArtifactInstallRootPrepareRequest>(&parsed);
        if(!request || request->purpose != SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
            throw_state_error_message("cleanup state cannot adopt exact evidence");
        expected.emplace_back(EXACT_DIRECTORY);
    }
    for(const auto name : {EXECUTION_FILE, AUTHORIZED_FILE, REFUSAL_FILE, OBSERVED_FILE}) {
        if(entry_exists(descriptor, std::string(name))) {
            static_cast<void>(open_private_file(descriptor, std::string(name), owner, description));
            expected.emplace_back(name);
        }
    }
    require_exact_entries(descriptor, std::move(expected), description);
}

std::string collect_staged_projection(int transaction_fd, uid_t owner,
                                      const SourceArtifactInstallRootPrepareRequest& request,
                                      const std::string& namespace_identity,
                                      int lifetime_fd,
                                      std::vector<OwnedDescriptor>* retained = nullptr) {
    std::vector<OwnedDescriptor> descriptors;
    std::string projection = "MOGUET-STAGED-IDENTITY\t2\nTOKEN\t" +
                             request.transaction_token + "\n" + namespace_identity +
                             StagedFilesystemIdentity::observe(transaction_fd).serialize("transaction");
    const auto lifetime_identity = StagedFilesystemIdentity::observe(lifetime_fd);
    if(lifetime_identity.group != StagedFilesystemIdentity::observe(transaction_fd).group) {
        throw_state_error_message("transaction lifetime lease has an unexpected group");
    }
    require_staged_identity(transaction_fd, std::string(LIFETIME_FILE), lifetime_fd, lifetime_identity);
    projection += lifetime_identity.serialize("lifetime");
    OwnedDescriptor prepared = open_private_file(transaction_fd, std::string(PREPARED_FILE), owner, "prepared state");
    const auto prepared_identity = StagedFilesystemIdentity::observe(prepared.get());
    const std::string prepared_bytes = read_bounded(prepared.get(),
                                                    SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "prepared state");
    if(prepared_bytes != serialize_source_artifact_install_root_prepared_state(request)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
            "prepared state changed its exact request");
    }
    require_staged_identity(transaction_fd, std::string(PREPARED_FILE), prepared.get(), prepared_identity);
    projection += prepared_identity.serialize("prepared");
    projection += "request-sha256\t" + xdg_generation_store_raw_contents_sha256(prepared_bytes) + "\n";
    descriptors.push_back(std::move(prepared));

    if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
        auto exact = open_directory_at(transaction_fd, std::string(EXACT_DIRECTORY), owner, PRIVATE_DIRECTORY_MODE, "exact evidence directory");
        const auto exact_identity = StagedFilesystemIdentity::observe(exact.get());
        auto world = open_private_file(exact.get(), std::string(DATABASE_WORLD_FILE), owner, "sealed database world");
        const auto world_identity = StagedFilesystemIdentity::observe(world.get());
        const auto bytes = read_bounded(world.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "sealed database world");
        require_staged_identity(exact.get(), std::string(DATABASE_WORLD_FILE), world.get(), world_identity);
        require_staged_identity(transaction_fd, std::string(EXACT_DIRECTORY), exact.get(), exact_identity);
        projection += "PURPOSE\tExactInstalledBinding\n" + exact_identity.serialize("exact") + world_identity.serialize("world") +
                      "world-sha256\t" + xdg_generation_store_raw_contents_sha256(bytes) + "\n";
        descriptors.push_back(std::move(world));
        descriptors.push_back(std::move(exact));
    }

    OwnedDescriptor hooks = open_directory_at(transaction_fd, std::string(HOOK_DIRECTORY), owner,
                                              PRIVATE_DIRECTORY_MODE, "transaction hook directory");
    const auto hooks_identity = StagedFilesystemIdentity::observe(hooks.get());
    require_exact_entries(hooks.get(), expected_hook_entries(request), "transaction hook directory");
    projection += hooks_identity.serialize("hooks");
    for(const auto& hook_name : expected_hook_entries(request)) {
        OwnedDescriptor hook = open_private_file(hooks.get(), hook_name, owner, "transaction hook");
        const auto hook_identity = StagedFilesystemIdentity::observe(hook.get());
        const auto expected_contents = expected_hook_contents(request, hook_name);
        if(read_bounded(hook.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "transaction hook") != expected_contents) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "transaction hook changed");
        }
        require_staged_identity(hooks.get(), hook_name, hook.get(), hook_identity);
        projection += hook_identity.serialize(hook_name);
        descriptors.push_back(std::move(hook));
    }
    require_staged_identity(transaction_fd, std::string(HOOK_DIRECTORY), hooks.get(), hooks_identity);
    descriptors.push_back(std::move(hooks));

    OwnedDescriptor artifacts = open_directory_at(transaction_fd, std::string(ARTIFACT_DIRECTORY), owner,
                                                  PRIVATE_DIRECTORY_MODE, "staged artifact directory");
    const auto artifacts_identity = StagedFilesystemIdentity::observe(artifacts.get());
    require_exact_entries(artifacts.get(), expected_artifact_entries(request), "staged artifact directory");
    projection += artifacts_identity.serialize("artifacts");
    for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
        const auto& expected = request.artifacts[index];
        const auto inspect = [&](const std::string& leaf, std::uint64_t size,
                                 const std::string& digest, bool signature) {
            OwnedDescriptor file = open_private_file(artifacts.get(), leaf, owner, "staged input", size);
            const auto identity = StagedFilesystemIdentity::observe(file.get());
            if(!signature) {
                require_archive_identity(file.get(), expected);
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
                if(g_state_test_hook) g_state_test_hook(
                    SourceArtifactInstallTrustedStateTestEvent::AfterArtifactMetadataValidation,
                    artifacts.get(), leaf);
#endif
            }
            if(descriptor_digest(file.get(), size) != digest) {
                throw SourceArtifactInstallTrustedStateError(signature
                                                                 ? SourceArtifactInstallSealingFailure::SignatureDigestMismatch
                                                                 : SourceArtifactInstallSealingFailure::StagedArtifactDigestMismatch,
                                                             "staged input differs from its retained archive/signature SHA-256");
            }
            require_staged_identity(artifacts.get(), leaf, file.get(), identity);
            projection += identity.serialize(leaf);
            descriptors.push_back(std::move(file));
        };
        inspect(staged_artifact_filename(index), expected.artifact_size, expected.archive_sha256, false);
        if(expected.signature_size > 0) inspect(staged_signature_filename(index),
                                                expected.signature_size, expected.signature_sha256, true);
    }
    require_exact_entries(artifacts.get(), expected_artifact_entries(request), "staged artifact directory");
    require_staged_identity(transaction_fd, std::string(ARTIFACT_DIRECTORY), artifacts.get(), artifacts_identity);
    descriptors.push_back(std::move(artifacts));
    if(projection.size() > MAX_IDENTITY_BYTES) throw_state_error_message("staged identity exceeds its bound");
    if(retained) *retained = std::move(descriptors);
    return projection;
}

SourceArtifactInstallRootPrepareRequest validate_prepared_state(
    int transaction_fd, uid_t expected_owner, const std::string& transaction_token,
    const std::string& namespace_identity, int lifetime_fd, std::vector<OwnedDescriptor>* retained = nullptr) {
    OwnedDescriptor prepared = open_private_file(transaction_fd, std::string(PREPARED_FILE),
                                                 expected_owner, "prepared source-artifact state");
    const auto parsed = parse_source_artifact_install_root_prepared_state(
        read_bounded(prepared.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "prepared state"));
    const auto* request = std::get_if<SourceArtifactInstallRootPrepareRequest>(&parsed);
    if(!request || request->transaction_token != transaction_token) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
            "prepared source-artifact state is malformed or mismatched");
    }
    if(!entry_exists(transaction_fd, std::string(IDENTITY_FILE))) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
            "staged generation authority is missing");
    }
    OwnedDescriptor identity = open_private_file(transaction_fd, std::string(IDENTITY_FILE),
                                                 expected_owner, "staged generation authority");
    const auto identity_metadata = StagedFilesystemIdentity::observe(identity.get());
    const std::string expected = read_bounded(identity.get(), MAX_IDENTITY_BYTES, "staged generation authority");
    if(!expected.starts_with("MOGUET-STAGED-IDENTITY\t2\nTOKEN\t" + transaction_token + "\n")) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
            "staged generation schema or token is invalid");
    }
    if(collect_staged_projection(transaction_fd, expected_owner, *request, namespace_identity, lifetime_fd, retained) != expected) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::StagedArtifactGenerationMismatch,
            "staged filesystem generation differs from trusted prepare");
    }
    require_staged_identity(transaction_fd, std::string(IDENTITY_FILE), identity.get(), identity_metadata);
    if(retained) retained->push_back(std::move(identity));
    return *request;
}


// These records describe the helper before exec. A hook observation is a
// separate authority and cannot be smuggled into a claim/authorization/refusal.
SourceArtifactInstallExecutionObservation read_execution_authority(
    int transaction_fd, uid_t owner, const std::string& token) {
    SourceArtifactInstallExecutionObservation result{token, false, std::nullopt};
    const auto read_record = [&](std::string_view leaf) {
        auto file = open_private_file(transaction_fd, std::string(leaf), owner, "execution authority");
        const auto identity = StagedFilesystemIdentity::observe(file.get());
        auto parsed = parse_source_artifact_install_execution_observation(
            read_bounded(file.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "execution authority"));
        const auto* observation = std::get_if<SourceArtifactInstallExecutionObservation>(&parsed);
        if(!observation || observation->transaction_token != token ||
           observation->execution_evidence != SourceArtifactInstallExecutionEvidence::Unobserved) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid pre-exec authority");
        }
        require_staged_identity(transaction_fd, std::string(leaf), file.get(), identity);
        return *observation;
    };
    if(!entry_exists(transaction_fd, std::string(EXECUTION_FILE))) {
        for(const auto leaf : {AUTHORIZED_FILE, REFUSAL_FILE, OBSERVED_FILE, RECEIPT_FILE, PARTIAL_RECEIPT_FILE}) {
            if(entry_exists(transaction_fd, std::string(leaf))) {
                throw SourceArtifactInstallTrustedStateError(
                    SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "execution evidence has no claim");
            }
        }
        return result;
    }
    const auto claim = read_record(EXECUTION_FILE);
    if(claim.authorized || claim.refusal) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid execution claim");
    }
    if(entry_exists(transaction_fd, std::string(AUTHORIZED_FILE))) {
        result = read_record(AUTHORIZED_FILE);
        if(!result.authorized || result.refusal) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid launch authorization");
        }
    }
    if(entry_exists(transaction_fd, std::string(REFUSAL_FILE))) {
        const auto refusal = read_record(REFUSAL_FILE);
        if(!refusal.refusal || refusal.authorized != result.authorized) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid launch refusal");
        }
        result.refusal = refusal.refusal;
    }
    if(entry_exists(transaction_fd, std::string(OBSERVED_FILE)) && (!result.authorized || result.refusal)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "hook execution evidence contradicts helper state");
    }
    return result;
}

std::string observed_execution_contents(int transaction_fd, uid_t owner, const std::string& token,
                                        SourceArtifactInstallExecutionEvidence evidence, int marker_fd) {
    auto seal = open_private_file(transaction_fd, std::string(IDENTITY_FILE), owner, "staged generation authority");
    const auto seal_identity = StagedFilesystemIdentity::observe(seal.get());
    const auto seal_bytes = read_bounded(seal.get(), MAX_IDENTITY_BYTES, "staged generation authority");
    require_staged_identity(transaction_fd, std::string(IDENTITY_FILE), seal.get(), seal_identity);
    auto marker = StagedFilesystemIdentity::observe(marker_fd);
    if(marker.group != StagedFilesystemIdentity::observe(transaction_fd).group) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "execution marker group differs from transaction");
    }
    // Bind this immutable marker's own inode as well as the prepared generation.
    // Size/timestamps cannot be embedded in a file whose write changes them;
    // readers still reprove the full metadata across their bounded read.
    marker.size = 0;
    marker.mtime_seconds = marker.mtime_nanoseconds = marker.ctime_seconds = marker.ctime_nanoseconds = 0;
    return "MOGUET-PACKAGE-EXECUTION\t1\nTOKEN\t" + token + "\nPHASE\t" +
           (evidence == SourceArtifactInstallExecutionEvidence::PreTransaction ? "PreTransaction" : "PostTransaction") +
           "\nSEAL\t" + xdg_generation_store_raw_contents_sha256(seal_bytes) + "\n" +
           marker.serialize("marker") + "END\n";
}

SourceArtifactInstallExecutionEvidence read_observed_execution(int transaction_fd, uid_t owner, const std::string& token) {
    if(!entry_exists(transaction_fd, std::string(OBSERVED_FILE))) return SourceArtifactInstallExecutionEvidence::Unobserved;
    auto file = open_private_file(transaction_fd, std::string(OBSERVED_FILE), owner, "package execution observation");
    const auto identity = StagedFilesystemIdentity::observe(file.get());
    const auto bytes = read_bounded(file.get(), 4096, "package execution observation");
    require_staged_identity(transaction_fd, std::string(OBSERVED_FILE), file.get(), identity);
    for(const auto evidence : {SourceArtifactInstallExecutionEvidence::PreTransaction, SourceArtifactInstallExecutionEvidence::PostTransaction}) {
        if(bytes == observed_execution_contents(transaction_fd, owner, token, evidence, file.get())) return evidence;
    }
    throw SourceArtifactInstallTrustedStateError(
        SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "package execution observation is malformed or stale");
}

void publish_observed_execution(int transaction_fd, uid_t owner, const std::string& token,
                                SourceArtifactInstallExecutionEvidence evidence) {
    auto file = create_private_file(transaction_fd, std::string(OBSERVED_FILE), owner, "package execution observation");
    write_all(file.get(), observed_execution_contents(transaction_fd, owner, token, evidence, file.get()));
    synchronize_file(file.get(), "package execution observation");
    synchronize_file(transaction_fd, "observed execution transaction");
}

std::string staged_identity_digest(int transaction_fd, uid_t owner) {
    auto identity = open_private_file(transaction_fd, std::string(IDENTITY_FILE), owner, "exact staged identity");
    const auto before = StagedFilesystemIdentity::observe(identity.get());
    const auto bytes = read_bounded(identity.get(), MAX_IDENTITY_BYTES, "exact staged identity");
    require_staged_identity(transaction_fd, std::string(IDENTITY_FILE), identity.get(), before);
    return xdg_generation_store_raw_contents_sha256(bytes);
}

std::string exact_private_prefix(int descriptor, const std::string& leaf,
                                 const SourceArtifactInstallRootPrepareRequest& request, const std::string& stage) {
    auto identity = StagedFilesystemIdentity::observe(descriptor);
    identity.size = 0;
    identity.mtime_seconds = identity.mtime_nanoseconds = identity.ctime_seconds = identity.ctime_nanoseconds = 0;
    return "MOGUET-EXACT-PRIVATE-RECORD\t1\nTOKEN\t" + request.transaction_token +
           "\nPURPOSE\tExactInstalledBinding\nMANIFEST\t" +
           xdg_generation_store_raw_contents_sha256(serialize_source_artifact_install_root_prepared_state(request)) +
           "\nSTAGE\t" + stage + "\nLEAF\t" + leaf + "\n" + identity.serialize("self") + "PAYLOAD\n";
}

OwnedDescriptor claim_exact_record(int exact_fd, uid_t owner, const std::string& leaf) {
    // Creating the partial first leaves durable invalid evidence on duplicate
    // invocation, including complete+partial coexistence. Never repair either.
    auto partial = create_private_file(exact_fd, leaf + ".partial", owner, "exact record invocation");
    require_entry_absent(exact_fd, leaf, "complete exact record");
    return partial;
}

void publish_claimed_exact_record(int exact_fd, uid_t owner, const std::string& leaf, OwnedDescriptor partial,
                                  const SourceArtifactInstallRootPrepareRequest& request, const std::string& stage,
                                  const std::string& payload) {
    if(payload.size() > SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES - 4096)
        throw_state_error_message("exact record payload exceeds its bound");
    const auto prefix = exact_private_prefix(partial.get(), leaf, request, stage);
    write_all(partial.get(), prefix + payload);
    synchronize_file(partial.get(), "partial exact record");
    const auto identity = StagedFilesystemIdentity::observe(partial.get());
    require_staged_identity(exact_fd, leaf + ".partial", partial.get(), identity);
    static_cast<void>(require_descriptor_metadata(partial.get(), owner, S_IFREG, PRIVATE_FILE_MODE, "partial exact record"));
    rename_noreplace(exact_fd, leaf + ".partial", exact_fd, leaf, "complete exact record");
    // rename changes ctime on Linux, so compare the immutable object identity
    // and then retain the post-publication metadata for named/FD reproof.
    require_named_identity(exact_fd, leaf, require_descriptor_metadata(partial.get(), owner, S_IFREG, PRIVATE_FILE_MODE, "complete exact record"), "complete exact record");
    if(exact_private_prefix(partial.get(), leaf, request, stage) != prefix)
        throw_state_error_message("exact record identity changed during publication");
    synchronize_file(exact_fd, "exact record directory");
}

std::optional<std::string> read_exact_record(int exact_fd, uid_t owner, const std::string& leaf,
                                             const SourceArtifactInstallRootPrepareRequest& request, const std::string& stage) {
    if(entry_exists(exact_fd, leaf + ".partial")) throw_state_error_message("partial exact record cannot be adopted");
    if(!entry_exists(exact_fd, leaf)) return std::nullopt;
    auto record = open_private_file(exact_fd, leaf, owner, "complete exact record");
    const auto identity = StagedFilesystemIdentity::observe(record.get());
    const auto bytes = read_bounded(record.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "complete exact record");
    require_staged_identity(exact_fd, leaf, record.get(), identity);
    const auto prefix = exact_private_prefix(record.get(), leaf, request, stage);
    if(!bytes.starts_with(prefix)) throw_state_error_message("exact record has stale, recreated, or mismatched authority");
    return bytes.substr(prefix.size());
}

InstalledDatabaseWorldResult read_database_world(int exact_fd, uid_t owner) {
    auto world = open_private_file(exact_fd, std::string(DATABASE_WORLD_FILE), owner, "sealed database world");
    const auto before = StagedFilesystemIdentity::observe(world.get());
    auto parsed = parse_installed_database_world(read_bounded(world.get(), SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "sealed database world"));
    require_staged_identity(exact_fd, std::string(DATABASE_WORLD_FILE), world.get(), before);
    return parsed;
}

ExactArtifactRecordObservations observe_exact_records(const InstalledDatabaseWorldResult& sealed_world,
                                                      const SourceArtifactInstallRootPrepareRequest& request,
                                                      const std::vector<std::string>* targets = nullptr) {
    using Issue = InstalledRecordObservationIssue;
    auto current_world = resolve_trusted_installed_database_world();
    const auto* known = std::get_if<InstalledDatabaseWorld>(&sealed_world);
    const auto* current = std::get_if<InstalledDatabaseWorld>(&current_world);
    ExactArtifactRecordObservations result;
    for(const auto& artifact : request.artifacts) {
        if(targets && std::find(targets->begin(), targets->end(), artifact.package_name) == targets->end()) continue;
        InstalledPackageRecordObservation observation = Issue::DatabaseWorldMismatch;
        if(!known)
            observation = std::get<Issue>(sealed_world);
        else if(!current)
            observation = std::get<Issue>(current_world);
        else if(*known == *current)
            observation = observe_installed_package_record(*known, artifact.package_name);
        result.push_back({artifact.artifact_index, artifact.package_name, std::move(observation)});
    }
    return result;
}

std::vector<std::string> exact_directory_entries(int exact_fd) {
    auto entries = list_directory_entries(exact_fd);
    for(const auto& entry : entries) {
        if(entry == DATABASE_WORLD_FILE) continue;
        bool known = false;
        for(const auto leaf : EXACT_RECORD_FILES)
            if(entry == leaf || entry == std::string(leaf) + ".partial") known = true;
        if(!known) throw_state_error_message("unknown exact evidence entry");
    }
    return entries;
}

void validate_optional_private_file(
    int transaction_fd, uid_t expected_owner,
    const std::string& name, const std::string& description) {
    if(!entry_exists(transaction_fd, name)) return;
    static_cast<void>(open_private_file(
        transaction_fd, name, expected_owner, description));
}

OpenTransaction retire_transaction(
    int active_fd, int used_fd, uid_t expected_owner,
    const std::string& transaction_token,
    OpenTransaction transaction) {
    require_entry_absent(
        used_fd, transaction_token,
        "used source-artifact transaction token");
    rename_noreplace(
        active_fd, transaction_token, used_fd, transaction_token,
        "used source-artifact transaction tombstone");
    synchronize_file(active_fd, "active source-artifact directory");
    synchronize_file(used_fd, "used source-artifact directory");

    OwnedDescriptor retired = open_directory_at(
        used_fd, transaction_token, expected_owner,
        PRIVATE_DIRECTORY_MODE,
        "used source-artifact transaction tombstone");
    const struct stat retired_metadata = require_descriptor_metadata(
        retired.get(), expected_owner, S_IFDIR, PRIVATE_DIRECTORY_MODE,
        "used source-artifact transaction tombstone");
    if(!same_identity(transaction.metadata, retired_metadata)) {
        throw_state_error_message(
            "retired source-artifact transaction identity changed");
    }
    return OpenTransaction{std::move(retired), retired_metadata};
}

void cleanup_retired_transaction(
    OpenTransaction& retired, uid_t expected_owner,
    const SourceArtifactInstallRootPrepareRequest& request,
    bool has_receipt, bool has_partial_receipt) {
    std::vector<std::string> expected{
        std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
        std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)};
    if(has_receipt) expected.emplace_back(RECEIPT_FILE);
    if(has_partial_receipt) expected.emplace_back(PARTIAL_RECEIPT_FILE);
    require_transaction_entries(
        retired.descriptor.get(), expected, expected_owner,
        "retired source-artifact transaction");

    if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
        auto exact = open_directory_at(retired.descriptor.get(), std::string(EXACT_DIRECTORY), expected_owner,
                                       PRIVATE_DIRECTORY_MODE, "retired exact evidence");
        for(const auto& leaf : exact_directory_entries(exact.get())) {
            static_cast<void>(open_private_file(exact.get(), leaf, expected_owner, "retired exact evidence record"));
            if(unlinkat(exact.get(), leaf.c_str(), 0) != 0) throw_state_error("unable to remove retired exact record");
        }
        if(unlinkat(retired.descriptor.get(), std::string(EXACT_DIRECTORY).c_str(), AT_REMOVEDIR) != 0)
            throw_state_error("unable to remove retired exact evidence");
    }

    OwnedDescriptor hooks = open_directory_at(
        retired.descriptor.get(), std::string(HOOK_DIRECTORY),
        expected_owner, PRIVATE_DIRECTORY_MODE,
        "retired source-artifact hook directory");
    require_exact_entries(
        hooks.get(), expected_hook_entries(request),
        "retired source-artifact hook directory");
    for(const auto& hook_filename : expected_hook_entries(request)) {
        static_cast<void>(open_private_file(
            hooks.get(), hook_filename, expected_owner,
            "retired source-artifact hook"));
        if(unlinkat(hooks.get(), hook_filename.c_str(), 0) == -1) {
            throw_state_error("unable to remove retired source-artifact hook");
        }
    }
    if(unlinkat(
           retired.descriptor.get(),
           std::string(HOOK_DIRECTORY).c_str(), AT_REMOVEDIR) == -1) {
        throw_state_error(
            "unable to remove retired source-artifact hook directory");
    }

    OwnedDescriptor artifacts = open_directory_at(
        retired.descriptor.get(), std::string(ARTIFACT_DIRECTORY),
        expected_owner, PRIVATE_DIRECTORY_MODE,
        "retired source-artifact staging directory");
    require_exact_entries(
        artifacts.get(), expected_artifact_entries(request),
        "retired source-artifact staging directory");
    for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
        const auto& expected_artifact = request.artifacts[index];
        const std::string artifact_name = staged_artifact_filename(index);
        static_cast<void>(open_private_file(
            artifacts.get(), artifact_name, expected_owner,
            "retired staged artifact", expected_artifact.artifact_size));
        if(unlinkat(artifacts.get(), artifact_name.c_str(), 0) == -1) {
            throw_state_error("unable to remove retired staged artifact");
        }
        if(expected_artifact.signature_size > 0) {
            const std::string signature_name =
                staged_signature_filename(index);
            static_cast<void>(open_private_file(
                artifacts.get(), signature_name, expected_owner,
                "retired staged signature",
                expected_artifact.signature_size));
            if(unlinkat(
                   artifacts.get(), signature_name.c_str(), 0) == -1) {
                throw_state_error(
                    "unable to remove retired staged signature");
            }
        }
    }
    require_exact_entries(
        artifacts.get(), {},
        "retired source-artifact staging directory");
    if(unlinkat(
           retired.descriptor.get(),
           std::string(ARTIFACT_DIRECTORY).c_str(),
           AT_REMOVEDIR) == -1) {
        throw_state_error(
            "unable to remove retired source-artifact staging directory");
    }

    static_cast<void>(open_private_file(
        retired.descriptor.get(), std::string(PREPARED_FILE),
        expected_owner, "retired prepared source-artifact state"));
    if(unlinkat(
           retired.descriptor.get(),
           std::string(PREPARED_FILE).c_str(), 0) == -1) {
        throw_state_error(
            "unable to remove retired prepared source-artifact state");
    }
    for(const std::string_view optional_file :
        {RECEIPT_FILE, PARTIAL_RECEIPT_FILE}) {
        const bool should_remove = optional_file == RECEIPT_FILE
                                       ? has_receipt
                                       : has_partial_receipt;
        if(!should_remove) continue;
        static_cast<void>(open_private_file(
            retired.descriptor.get(), std::string(optional_file),
            expected_owner, "retired source-artifact receipt"));
        if(unlinkat(
               retired.descriptor.get(),
               std::string(optional_file).c_str(), 0) == -1) {
            throw_state_error(
                "unable to remove retired source-artifact receipt");
        }
    }
    for(const auto name : {IDENTITY_FILE, EXECUTION_FILE, AUTHORIZED_FILE, REFUSAL_FILE, OBSERVED_FILE, LIFETIME_FILE}) {
        if(!entry_exists(retired.descriptor.get(), std::string(name))) continue;
        static_cast<void>(open_private_file(retired.descriptor.get(), std::string(name),
                                            expected_owner, "retired sealing authority"));
        if(unlinkat(retired.descriptor.get(), std::string(name).c_str(), 0) != 0)
            throw_state_error("unable to remove retired sealing authority");
    }
    require_exact_entries(
        retired.descriptor.get(), {},
        "used source-artifact transaction tombstone");
    synchronize_file(
        retired.descriptor.get(),
        "used source-artifact transaction tombstone");
}

std::uint64_t require_sealed_input(
    int input_fd,
    const SourceArtifactInstallRootPrepareRequest& request) {
    if(input_fd < 0) {
        throw_state_error_message(
            "sealed source-artifact input descriptor is invalid");
    }
    struct stat metadata{};
    if(fstat(input_fd, &metadata) == -1 ||
       !S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        throw_state_error_message(
            "source-artifact input is not a regular sealed file");
    }
    const int seals = fcntl(input_fd, F_GET_SEALS);
    constexpr int REQUIRED_SEALS =
        F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if(seals == -1 || (seals & REQUIRED_SEALS) != REQUIRED_SEALS) {
        throw_state_error_message(
            "source-artifact input is not write-sealed");
    }
    std::uint64_t expected_size = 0;
    for(const auto& artifact : request.artifacts) {
        expected_size += artifact.artifact_size;
        expected_size += artifact.signature_size;
    }
    if(static_cast<std::uint64_t>(metadata.st_size) != expected_size) {
        throw_state_error_message(
            "sealed source-artifact input has an unexpected size");
    }
    return expected_size;
}

} // namespace

struct SourceArtifactInstallTrustedStateStore::Implementation {
    uid_t expected_owner;
    OwnedDescriptor runtime;
    OwnedDescriptor moguet;
    OwnedDescriptor state_root;
    OwnedDescriptor active;
    OwnedDescriptor used;

    std::string reprove_namespace() const {
        const auto runtime_metadata = require_descriptor_metadata(runtime.get(), expected_owner,
                                                                  S_IFDIR, std::nullopt, "runtime parent");
#ifndef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
        // The installed producer has only fixed /run. The test-only build
        // substitutes its isolated runtime descriptor, never a CLI override.
        require_named_identity(AT_FDCWD, "/run", runtime_metadata, "fixed runtime parent");
#endif
        const auto inspect = [&](int parent, const std::string& name, int descriptor) {
            const auto metadata = require_descriptor_metadata(descriptor, expected_owner,
                                                              S_IFDIR, PRIVATE_DIRECTORY_MODE, name);
            require_named_identity(parent, name, metadata, name);
            return StagedFilesystemIdentity::observe(descriptor).serialize(name);
        };
        static_cast<void>(runtime_metadata);
        return StagedFilesystemIdentity::observe(runtime.get()).serialize("runtime") +
               inspect(runtime.get(), "moguet", moguet.get()) +
               inspect(moguet.get(), "source-artifact-installs", state_root.get()) +
               inspect(state_root.get(), "active", active.get());
    }
};

SourceArtifactInstallTrustedStateStore::
    SourceArtifactInstallTrustedStateStore(
        std::unique_ptr<Implementation> implementation) noexcept
    : implementation_(std::move(implementation)) {
}

SourceArtifactInstallTrustedStateStore
SourceArtifactInstallTrustedStateStore::open_below_runtime_parent(
    int runtime_parent_fd, uid_t expected_owner) {
    if(runtime_parent_fd < 0) {
        throw_state_error_message("runtime parent descriptor is invalid");
    }
    static_cast<void>(require_descriptor_metadata(
        runtime_parent_fd, expected_owner, S_IFDIR, std::nullopt,
        "runtime parent"));

    OwnedDescriptor runtime(fcntl(runtime_parent_fd, F_DUPFD_CLOEXEC, 3));
    if(runtime.get() < 0) throw_state_error("unable to retain runtime parent");
    OwnedDescriptor moguet = ensure_private_directory(
        runtime_parent_fd, "moguet", expected_owner,
        "Moguet runtime directory");
    OwnedDescriptor state_root = ensure_private_directory(
        moguet.get(), "source-artifact-installs", expected_owner,
        "source-artifact install runtime directory");
    OwnedDescriptor active = ensure_private_directory(
        state_root.get(), "active", expected_owner,
        "active source-artifact directory");
    OwnedDescriptor used = ensure_private_directory(
        state_root.get(), "used", expected_owner,
        "used source-artifact directory");
    require_exact_entries(
        state_root.get(), {"active", "used"},
        "source-artifact install runtime directory");

    return SourceArtifactInstallTrustedStateStore(
        std::make_unique<Implementation>(Implementation{
            expected_owner, std::move(runtime), std::move(moguet),
            std::move(state_root), std::move(active), std::move(used)}));
}

SourceArtifactInstallTrustedStateStore::
    SourceArtifactInstallTrustedStateStore(
        SourceArtifactInstallTrustedStateStore&&) noexcept = default;

SourceArtifactInstallTrustedStateStore&
SourceArtifactInstallTrustedStateStore::operator=(
    SourceArtifactInstallTrustedStateStore&&) noexcept = default;

SourceArtifactInstallTrustedStateStore::~SourceArtifactInstallTrustedStateStore() =
    default;

SourceArtifactInstallRootPrepareResponse
SourceArtifactInstallTrustedStateStore::prepare(
    const SourceArtifactInstallRootPrepareRequest& request,
    int sealed_artifact_input_fd) {
    if(implementation_ == nullptr ||
       !is_valid_source_artifact_install_root_request(request)) {
        throw_state_error_message(
            "source-artifact prepare request is invalid");
    }
    static_cast<void>(require_sealed_input(
        sealed_artifact_input_fd, request));
    const std::string prepared_protocol =
        serialize_source_artifact_install_root_prepared_state(request);
    Implementation& state = *implementation_;
    require_entry_absent(
        state.used.get(), request.transaction_token,
        "used source-artifact transaction token");
    require_entry_absent(
        state.active.get(), request.transaction_token,
        "active source-artifact transaction token");
    const std::string staging_name =
        preparing_name(request.transaction_token);
    require_entry_absent(
        state.active.get(), staging_name,
        "preparing source-artifact transaction token");

    if(mkdirat(
           state.active.get(), staging_name.c_str(),
           PRIVATE_DIRECTORY_MODE) == -1) {
        throw_state_error(
            "unable to create preparing source-artifact state");
    }
    bool published = false;
    try {
        OwnedDescriptor staging = open_directory_at(
            state.active.get(), staging_name, state.expected_owner,
            PRIVATE_DIRECTORY_MODE,
            "preparing source-artifact state");
        OwnedDescriptor prepared = create_private_file(
            staging.get(), std::string(PREPARED_FILE),
            state.expected_owner,
            "prepared source-artifact state");
        write_all(prepared.get(), prepared_protocol);
        synchronize_file(prepared.get(), "prepared source-artifact state");

        OwnedDescriptor lifetime = create_private_file(
            staging.get(), std::string(LIFETIME_FILE), state.expected_owner,
            "transaction lifetime lease");
        write_all(lifetime.get(), lifetime_lease_contents(request.transaction_token));
        synchronize_file(lifetime.get(), "transaction lifetime lease");
        // Hold publication exclusive through the post-rename reproof. Later
        // execute/record readers and cleanup writers all open this same inode.
        if(flock(lifetime.get(), LOCK_EX | LOCK_NB) != 0)
            throw_state_error("unable to acquire preparing transaction lifetime");

        OwnedDescriptor hooks = ensure_private_directory(
            staging.get(), std::string(HOOK_DIRECTORY),
            state.expected_owner,
            "source-artifact transaction hook directory");
        for(const auto& hook_filename : expected_hook_entries(request)) {
            auto hook = create_private_file(hooks.get(), hook_filename, state.expected_owner, "transaction hook");
            write_all(hook.get(), expected_hook_contents(request, hook_filename));
            synchronize_file(hook.get(), "transaction hook");
        }
        synchronize_file(
            hooks.get(), "source-artifact transaction hook directory");

        OwnedDescriptor artifacts = ensure_private_directory(
            staging.get(), std::string(ARTIFACT_DIRECTORY),
            state.expected_owner,
            "source-artifact staging directory");
        std::uint64_t input_offset = 0;
        for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
            const auto& expected = request.artifacts[index];
            OwnedDescriptor artifact = create_private_file(
                artifacts.get(), staged_artifact_filename(index),
                state.expected_owner, "staged package artifact");
            copy_exact_range(
                sealed_artifact_input_fd, input_offset, artifact.get(),
                expected.artifact_size);
            input_offset += expected.artifact_size;
            synchronize_file(artifact.get(), "staged package artifact");

            if(expected.signature_size > 0) {
                OwnedDescriptor signature = create_private_file(
                    artifacts.get(), staged_signature_filename(index),
                    state.expected_owner, "staged package signature");
                copy_exact_range(
                    sealed_artifact_input_fd, input_offset,
                    signature.get(), expected.signature_size);
                input_offset += expected.signature_size;
                synchronize_file(
                    signature.get(), "staged package signature");
            }
        }
        require_exact_entries(
            artifacts.get(), expected_artifact_entries(request),
            "source-artifact staging directory");
        synchronize_file(artifacts.get(), "source-artifact staging directory");

        // Reopen every root-owned staged file and query the same descriptor
        // with libalpm before publication. Package names, versions,
        // PackageBase, and architecture are never filled from upper context.
        for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
            const auto& expected = request.artifacts[index];
            OwnedDescriptor artifact = open_private_file(
                artifacts.get(), staged_artifact_filename(index),
                state.expected_owner, "staged package artifact",
                expected.artifact_size);
            require_archive_identity(artifact.get(), expected);
        }

        if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
            auto exact = ensure_private_directory(staging.get(), std::string(EXACT_DIRECTORY), state.expected_owner, "exact evidence directory");
            auto world = create_private_file(exact.get(), std::string(DATABASE_WORLD_FILE), state.expected_owner, "sealed database world");
            write_all(world.get(), serialize_installed_database_world(resolve_trusted_installed_database_world()));
            synchronize_file(world.get(), "sealed database world");
            synchronize_file(exact.get(), "exact evidence directory");
        }
        {
            const std::string projection = collect_staged_projection(staging.get(),
                                                                     state.expected_owner, request, state.reprove_namespace(), lifetime.get());
            OwnedDescriptor identity = create_private_file(staging.get(), std::string(IDENTITY_FILE),
                                                           state.expected_owner, "staged generation authority");
            write_all(identity.get(), projection);
            synchronize_file(identity.get(), "staged generation authority");
        }

        std::vector<std::string> preparing_entries{std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
                                                   std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE), std::string(LIFETIME_FILE)};
        if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) preparing_entries.emplace_back(EXACT_DIRECTORY);
        require_exact_entries(staging.get(), preparing_entries, "preparing source-artifact state");
        synchronize_file(staging.get(), "preparing source-artifact state");

        rename_noreplace(
            state.active.get(), staging_name, state.active.get(),
            request.transaction_token,
            "active source-artifact transaction");
        published = true;
        synchronize_file(state.active.get(), "active source-artifact directory");

        OpenTransaction transaction = open_transaction(
            state.active.get(), state.expected_owner,
            request.transaction_token);
        const SourceArtifactInstallRootPrepareRequest validated =
            validate_prepared_state(
                transaction.descriptor.get(), state.expected_owner,
                request.transaction_token, state.reprove_namespace(), lifetime.get());
        if(validated != request) {
            throw_state_error_message(
                "published source-artifact state changed identity");
        }

        SourceArtifactInstallRootPrepareResponse response{
            request.transaction_token,
            source_artifact_install_hook_directory(
                request.transaction_token),
            {}};
        if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
            response.staged_identity_sha256 = staged_identity_digest(transaction.descriptor.get(), state.expected_owner);
        response.artifacts.reserve(request.artifacts.size());
        for(std::size_t index = 0; index < request.artifacts.size(); ++index) {
            response.artifacts.push_back(
                SourceArtifactInstallStagedArtifact{
                    request.artifacts[index].artifact_index,
                    source_artifact_install_staged_artifact_path(
                        request.transaction_token, index)});
        }
        return response;
    } catch(...) {
        if(published) {
            try {
                abort(request.transaction_token);
            } catch(...) {
                throw_state_error_message(
                    "source-artifact prepare failed and exact published-state cleanup failed");
            }
        } else if(!cleanup_preparing_directory(
                      state.active.get(), staging_name, request)) {
            throw_state_error_message(
                "source-artifact prepare failed and exact staging cleanup failed");
        }
        throw;
    }
}

void SourceArtifactInstallTrustedStateStore::record(
    const std::string& transaction_token,
    int needs_targets_input_fd) {
    if(implementation_ == nullptr ||
       !is_valid_trusted_alpm_receipt_token(transaction_token) ||
       needs_targets_input_fd < 0) {
        throw_state_error_message("source-artifact record request is invalid");
    }
    Implementation& state = *implementation_;
    OpenTransaction transaction = open_transaction(
        state.active.get(), state.expected_owner, transaction_token);
    auto lifetime = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner,
                                           transaction_token, LOCK_SH);
    const auto request = validate_prepared_state(
        transaction.descriptor.get(), state.expected_owner,
        transaction_token, state.reprove_namespace(), lifetime.get());
    if(request.purpose != SourceArtifactInstallTrustedPurpose::CleanupInstallOnly)
        throw_state_error_message("legacy Install recording requires cleanup purpose");
    require_transaction_entries(
        transaction.descriptor.get(),
        {std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
         std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)},
        state.expected_owner,
        "recordable source-artifact transaction");
    const auto execution = read_execution_authority(transaction.descriptor.get(), state.expected_owner, transaction_token);
    if(!execution.authorized || execution.refusal) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
            "Install receipt requires a successfully sealed execution handoff");
    }

    static_cast<void>(state.reprove_namespace());
    const std::string input = read_bounded(
        needs_targets_input_fd,
        SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES,
        "source-artifact ALPM NeedsTargets input");
    const TrustedAlpmReceiptNeedsTargetsResult parsed =
        parse_trusted_alpm_receipt_needs_targets(input);
    const auto* packages =
        std::get_if<std::vector<std::string>>(&parsed);
    if(packages == nullptr) {
        throw_state_error_message(
            "source-artifact ALPM NeedsTargets input is invalid");
    }
    const std::string receipt_protocol =
        serialize_source_artifact_install_root_receipt(
            SourceArtifactInstallRootReceipt{
                SourceArtifactInstallRootReceiptState::Complete,
                transaction_token, *packages});

    require_named_identity(state.active.get(), transaction_token, transaction.metadata, "record execution transaction");
    static_cast<void>(state.reprove_namespace());
    const auto evidence = read_observed_execution(transaction.descriptor.get(), state.expected_owner, transaction_token);
    if(evidence == SourceArtifactInstallExecutionEvidence::PostTransaction) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "PostTransaction recording was already attempted");
    }
    if(evidence == SourceArtifactInstallExecutionEvidence::Unobserved) {
        // A later trusted hook can prove its own phase if the earlier hook did
        // not publish anything. It never repairs a partial/stale marker.
        publish_observed_execution(transaction.descriptor.get(), state.expected_owner, transaction_token,
                                   SourceArtifactInstallExecutionEvidence::PostTransaction);
    }

    OwnedDescriptor partial = create_private_file(
        transaction.descriptor.get(),
        std::string(PARTIAL_RECEIPT_FILE), state.expected_owner,
        "partial source-artifact receipt");
    write_all(partial.get(), receipt_protocol);
    synchronize_file(partial.get(), "partial source-artifact receipt");
    require_named_identity(
        state.active.get(), transaction_token, transaction.metadata,
        "active source-artifact transaction");
    static_cast<void>(state.reprove_namespace());
    rename_noreplace(
        transaction.descriptor.get(),
        std::string(PARTIAL_RECEIPT_FILE),
        transaction.descriptor.get(), std::string(RECEIPT_FILE),
        "complete source-artifact receipt");
    synchronize_file(
        transaction.descriptor.get(),
        "active source-artifact transaction");
    require_transaction_entries(
        transaction.descriptor.get(),
        {std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
         std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE), std::string(RECEIPT_FILE)},
        state.expected_owner,
        "recorded source-artifact transaction");
}

void SourceArtifactInstallTrustedStateStore::record_install(const std::string& token, int input) {
    record_exact_operation(token, input, ExactArtifactTransactionOperation::Install);
}

void SourceArtifactInstallTrustedStateStore::record_upgrade(const std::string& token, int input) {
    record_exact_operation(token, input, ExactArtifactTransactionOperation::Upgrade);
}

void SourceArtifactInstallTrustedStateStore::record_exact_operation(
    const std::string& token, int input, ExactArtifactTransactionOperation operation) {
    if(!implementation_ || !is_valid_trusted_alpm_receipt_token(token) || input < 0)
        throw_state_error_message("invalid exact operation recording request");
    auto& state = *implementation_;
    auto transaction = open_transaction(state.active.get(), state.expected_owner, token);
    auto lifetime = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner, token, LOCK_SH);
    const auto request = validate_prepared_state(transaction.descriptor.get(), state.expected_owner, token,
                                                 state.reprove_namespace(), lifetime.get());
    if(request.purpose != SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
        throw_state_error_message("exact operation hook cannot adopt cleanup state");
    const auto execution = read_execution_authority(transaction.descriptor.get(), state.expected_owner, token);
    if(!execution.authorized || execution.refusal) throw_state_error_message("exact receipt requires sealed launch authorization");
    auto exact = open_directory_at(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), state.expected_owner,
                                   PRIVATE_DIRECTORY_MODE, "exact operation directory");
    const auto exact_identity = StagedFilesystemIdentity::observe(exact.get());
    const auto stage = staged_identity_digest(transaction.descriptor.get(), state.expected_owner);
    const bool upgrade = operation == ExactArtifactTransactionOperation::Upgrade;
    const std::string leaf = upgrade ? "upgrade" : "install";
    auto claim = claim_exact_record(exact.get(), state.expected_owner, leaf);
    const auto targets_result = parse_trusted_alpm_receipt_needs_targets(
        read_bounded(input, SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES, "exact NeedsTargets"));
    const auto* targets = std::get_if<std::vector<std::string>>(&targets_result);
    if(!targets) throw_state_error_message("invalid exact NeedsTargets");
    const auto payload = serialize_exact_artifact_operation_fragment(request, stage, operation, *targets);
    const auto other = read_exact_record(exact.get(), state.expected_owner, upgrade ? "install" : "upgrade", request, stage);
    const auto joined = join_exact_artifact_operation_fragments(upgrade ? other : std::optional<std::string>(payload),
                                                                upgrade ? std::optional<std::string>(payload) : other, request, stage);
    if(const auto* issue = std::get_if<ExactArtifactReceiptIssue>(&joined);
       issue && *issue != ExactArtifactReceiptIssue::Missing)
        throw_state_error_message("conflicting exact operation fragments");
    require_named_identity(state.active.get(), token, transaction.metadata, "exact recording transaction");
    require_staged_identity(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), exact.get(), exact_identity);
    static_cast<void>(state.reprove_namespace());
    if(read_observed_execution(transaction.descriptor.get(), state.expected_owner, token) == SourceArtifactInstallExecutionEvidence::Unobserved)
        publish_observed_execution(transaction.descriptor.get(), state.expected_owner, token, SourceArtifactInstallExecutionEvidence::PostTransaction);

    // Actual operation authority is durable before any local DB session,
    // metadata read, generation lookup, or anchor allocation can fail.
    publish_claimed_exact_record(exact.get(), state.expected_owner, leaf, std::move(claim), request, stage, payload);
    try {
        auto anchor_claim = claim_exact_record(exact.get(), state.expected_owner, leaf + "-anchor");
        const auto anchors = observe_exact_records(read_database_world(exact.get(), state.expected_owner), request, targets);
        require_staged_identity(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), exact.get(), exact_identity);
        publish_claimed_exact_record(exact.get(), state.expected_owner, leaf + "-anchor", std::move(anchor_claim), request, stage,
                                     serialize_exact_artifact_record_observations(anchors));
    } catch(...) {
        // The core receipt remains intact. Missing/partial anchors only prevent
        // binding proof and must not erase the observed Install/Upgrade.
    }
}

std::string SourceArtifactInstallTrustedStateStore::consume_exact(const std::string& token) {
    if(!implementation_ || !is_valid_trusted_alpm_receipt_token(token)) throw_state_error_message("invalid exact consume request");
    auto& state = *implementation_;
    auto transaction = open_transaction(state.active.get(), state.expected_owner, token);
    auto lifetime = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner, token, LOCK_EX);
    const auto request = validate_prepared_state(transaction.descriptor.get(), state.expected_owner, token,
                                                 state.reprove_namespace(), lifetime.get());
    if(request.purpose != SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
        throw_state_error_message("exact consumption cannot adopt cleanup state");
    const auto execution = read_execution_authority(transaction.descriptor.get(), state.expected_owner, token);
    if(!execution.authorized || execution.refusal ||
       read_observed_execution(transaction.descriptor.get(), state.expected_owner, token) == SourceArtifactInstallExecutionEvidence::Unobserved)
        throw_state_error_message("exact consumption lacks package-manager execution evidence");
    require_transaction_entries(transaction.descriptor.get(), {std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY), std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)},
                                state.expected_owner, "consumable exact transaction");
    auto exact = open_directory_at(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), state.expected_owner,
                                   PRIVATE_DIRECTORY_MODE, "exact evidence directory");
    const auto exact_identity = StagedFilesystemIdentity::observe(exact.get());
    static_cast<void>(exact_directory_entries(exact.get()));
    const auto stage = staged_identity_digest(transaction.descriptor.get(), state.expected_owner);
    ExactArtifactRootEvidence evidence;
    evidence.world = read_database_world(exact.get(), state.expected_owner);
    const auto read_core = [&](const std::string& leaf) -> std::optional<std::string> {
        try {
            return read_exact_record(exact.get(), state.expected_owner, leaf, request, stage);
        } catch(const std::exception&) {
            return "INVALID\n";
        }
    };
    evidence.installs = read_core("install");
    evidence.upgrades = read_core("upgrade");
    const auto read_observations = [&](const std::string& leaf, InstalledRecordObservationIssue missing) -> ExactArtifactRecordObservationsResult {
        try {
            auto payload = read_exact_record(exact.get(), state.expected_owner, leaf, request, stage);
            if(!payload) return missing;
            return parse_exact_artifact_record_observations(*payload, request);
        } catch(const std::bad_alloc&) {
            return InstalledRecordObservationIssue::ResourceFailure;
        } catch(...) {
            return InstalledRecordObservationIssue::MalformedMetadata;
        }
    };
    evidence.baseline = read_observations("baseline", InstalledRecordObservationIssue::MissingBaseline);
    evidence.install_anchors = read_observations("install-anchor", InstalledRecordObservationIssue::MissingAnchor);
    evidence.upgrade_anchors = read_observations("upgrade-anchor", InstalledRecordObservationIssue::MissingAnchor);
    const auto protocol = serialize_exact_artifact_root_evidence(evidence, request, stage);
    require_staged_identity(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), exact.get(), exact_identity);
    require_named_identity(state.active.get(), token, transaction.metadata, "exact consumption transaction");
    static_cast<void>(state.reprove_namespace());
    auto retired = retire_transaction(state.active.get(), state.used.get(), state.expected_owner, token, std::move(transaction));
    cleanup_retired_transaction(retired, state.expected_owner, request, false, false);
    return protocol;
}

std::string SourceArtifactInstallTrustedStateStore::consume(
    const std::string& transaction_token) {
    if(implementation_ == nullptr ||
       !is_valid_trusted_alpm_receipt_token(transaction_token)) {
        throw_state_error_message("source-artifact consume request is invalid");
    }
    Implementation& state = *implementation_;
    OpenTransaction transaction = open_transaction(
        state.active.get(), state.expected_owner, transaction_token);
    auto cleanup_lease = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner,
                                                transaction_token, LOCK_EX);
    const SourceArtifactInstallRootPrepareRequest request =
        validate_prepared_state(
            transaction.descriptor.get(), state.expected_owner,
            transaction_token, state.reprove_namespace(), cleanup_lease.get());
    if(request.purpose != SourceArtifactInstallTrustedPurpose::CleanupInstallOnly)
        throw_state_error_message("legacy receipt consumption requires cleanup purpose");

    static_cast<void>(state.reprove_namespace());
    const bool has_receipt = entry_exists(
        transaction.descriptor.get(), std::string(RECEIPT_FILE));
    if(entry_exists(
           transaction.descriptor.get(),
           std::string(PARTIAL_RECEIPT_FILE))) {
        throw_state_error_message(
            "partial source-artifact receipt cannot be consumed");
    }
    std::vector<std::string> expected{
        std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
        std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)};
    if(has_receipt) expected.emplace_back(RECEIPT_FILE);
    require_transaction_entries(
        transaction.descriptor.get(), expected, state.expected_owner,
        "consumable source-artifact transaction");

    SourceArtifactInstallRootReceipt receipt{
        SourceArtifactInstallRootReceiptState::Missing,
        transaction_token,
        {}};
    if(has_receipt) {
        OwnedDescriptor receipt_file = open_private_file(
            transaction.descriptor.get(), std::string(RECEIPT_FILE),
            state.expected_owner, "complete source-artifact receipt");
        const std::string protocol = read_bounded(
            receipt_file.get(),
            SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES,
            "complete source-artifact receipt");
        const SourceArtifactInstallRootReceiptResult parsed =
            parse_source_artifact_install_root_receipt(protocol);
        const auto* complete =
            std::get_if<SourceArtifactInstallRootReceipt>(&parsed);
        if(complete == nullptr ||
           complete->state !=
               SourceArtifactInstallRootReceiptState::Complete ||
           complete->transaction_token != transaction_token) {
            throw_state_error_message(
                "complete source-artifact receipt is malformed or mismatched");
        }
        const auto execution = read_execution_authority(transaction.descriptor.get(), state.expected_owner, transaction_token);
        if(!execution.authorized || execution.refusal ||
           read_observed_execution(transaction.descriptor.get(), state.expected_owner, transaction_token) ==
               SourceArtifactInstallExecutionEvidence::Unobserved) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
                "receipt lost its sealed execution authority");
        }
        receipt = *complete;
    }
    require_named_identity(
        state.active.get(), transaction_token, transaction.metadata,
        "active source-artifact transaction");
    OpenTransaction retired = retire_transaction(
        state.active.get(), state.used.get(), state.expected_owner,
        transaction_token, std::move(transaction));
    cleanup_retired_transaction(
        retired, state.expected_owner, request, has_receipt, false);
    return serialize_source_artifact_install_root_receipt(receipt);
}

void SourceArtifactInstallTrustedStateStore::abort(
    const std::string& transaction_token) {
    if(implementation_ == nullptr ||
       !is_valid_trusted_alpm_receipt_token(transaction_token)) {
        throw_state_error_message("source-artifact abort request is invalid");
    }
    Implementation& state = *implementation_;
    OpenTransaction transaction = open_transaction(
        state.active.get(), state.expected_owner, transaction_token);
    auto cleanup_lease = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner,
                                                transaction_token, LOCK_EX);
    const SourceArtifactInstallRootPrepareRequest request =
        validate_prepared_state(
            transaction.descriptor.get(), state.expected_owner,
            transaction_token, state.reprove_namespace(), cleanup_lease.get());
    static_cast<void>(state.reprove_namespace());
    const bool has_receipt = entry_exists(
        transaction.descriptor.get(), std::string(RECEIPT_FILE));
    const bool has_partial_receipt = entry_exists(
        transaction.descriptor.get(),
        std::string(PARTIAL_RECEIPT_FILE));
    if(has_receipt && has_partial_receipt) {
        throw_state_error_message(
            "complete and partial source-artifact receipts coexist");
    }
    validate_optional_private_file(
        transaction.descriptor.get(), state.expected_owner,
        std::string(RECEIPT_FILE),
        "abortable complete source-artifact receipt");
    validate_optional_private_file(
        transaction.descriptor.get(), state.expected_owner,
        std::string(PARTIAL_RECEIPT_FILE),
        "abortable partial source-artifact receipt");
    std::vector<std::string> expected{
        std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
        std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)};
    if(has_receipt) expected.emplace_back(RECEIPT_FILE);
    if(has_partial_receipt) expected.emplace_back(PARTIAL_RECEIPT_FILE);
    require_transaction_entries(
        transaction.descriptor.get(), expected, state.expected_owner,
        "abortable source-artifact transaction");
    require_named_identity(
        state.active.get(), transaction_token, transaction.metadata,
        "active source-artifact transaction");
    OpenTransaction retired = retire_transaction(
        state.active.get(), state.used.get(), state.expected_owner,
        transaction_token, std::move(transaction));
    cleanup_retired_transaction(
        retired, state.expected_owner, request, has_receipt,
        has_partial_receipt);
}

SourceArtifactInstallTrustedStateError::SourceArtifactInstallTrustedStateError(
    const std::string& diagnostic)
    : SourceArtifactInstallTrustedStateError(
          SourceArtifactInstallSealingFailure::StagedArtifactRevalidationFailure, diagnostic) {
}

SourceArtifactInstallTrustedStateError::SourceArtifactInstallTrustedStateError(
    SourceArtifactInstallSealingFailure reason, const std::string& diagnostic, int error_number)
    : std::runtime_error(diagnostic), refusal_{reason, error_number} {
}

const SourceArtifactInstallSealingRefusal&
SourceArtifactInstallTrustedStateError::refusal() const noexcept {
    return refusal_;
}

SourceArtifactInstallExecutionObservation
SourceArtifactInstallTrustedStateStore::execution_status(const std::string& token) {
    if(!implementation_ || !is_valid_trusted_alpm_receipt_token(token)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid execution status request");
    }
    auto& state = *implementation_;
    static_cast<void>(state.reprove_namespace());
    auto transaction = open_transaction(state.active.get(), state.expected_owner, token);
    auto lifetime = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner, token, LOCK_SH);
    auto result = read_execution_authority(transaction.descriptor.get(), state.expected_owner, token);
    if(entry_exists(transaction.descriptor.get(), std::string(OBSERVED_FILE))) {
        // Negative helper refusals remain readable when the archive itself
        // failed sealing. Positive evidence, however, requires the exact stage
        // and lease generation at every query; old or recreated state is not
        // promoted merely because a hook marker exists.
        static_cast<void>(validate_prepared_state(transaction.descriptor.get(), state.expected_owner,
                                                  token, state.reprove_namespace(), lifetime.get()));
        std::vector<std::string> expected{std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
                                          std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)};
        for(const auto name : {RECEIPT_FILE, PARTIAL_RECEIPT_FILE})
            if(entry_exists(transaction.descriptor.get(), std::string(name))) expected.emplace_back(name);
        require_transaction_entries(transaction.descriptor.get(), expected, state.expected_owner, "observed execution state");
        result.execution_evidence = read_observed_execution(transaction.descriptor.get(), state.expected_owner, token);
    }
    require_named_identity(state.active.get(), token, transaction.metadata, "execution transaction");
    return result;
}

void SourceArtifactInstallTrustedStateStore::observe_execution(const std::string& token) {
    if(!implementation_ || !is_valid_trusted_alpm_receipt_token(token)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid execution observation request");
    }
    auto& state = *implementation_;
    auto transaction = open_transaction(state.active.get(), state.expected_owner, token);
    auto lifetime = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner, token, LOCK_SH);
    const auto request = validate_prepared_state(transaction.descriptor.get(), state.expected_owner,
                                                 token, state.reprove_namespace(), lifetime.get());
    require_entry_absent(transaction.descriptor.get(), std::string(OBSERVED_FILE), "package execution observation");
    require_transaction_entries(transaction.descriptor.get(),
                                {std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
                                 std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE)},
                                state.expected_owner, "PreTransaction execution observation");
    const auto execution = read_execution_authority(transaction.descriptor.get(), state.expected_owner, token);
    if(!execution.authorized || execution.refusal) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "hook observation requires launch authorization");
    }
    require_named_identity(state.active.get(), token, transaction.metadata, "observed execution transaction");
    static_cast<void>(state.reprove_namespace());
    publish_observed_execution(transaction.descriptor.get(), state.expected_owner, token,
                               SourceArtifactInstallExecutionEvidence::PreTransaction);
    if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
        try {
            auto exact = open_directory_at(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), state.expected_owner,
                                           PRIVATE_DIRECTORY_MODE, "exact baseline directory");
            auto claim = claim_exact_record(exact.get(), state.expected_owner, "baseline");
            const auto baseline = observe_exact_records(read_database_world(exact.get(), state.expected_owner), request);
            publish_claimed_exact_record(exact.get(), state.expected_owner, "baseline", std::move(claim), request,
                                         staged_identity_digest(transaction.descriptor.get(), state.expected_owner),
                                         serialize_exact_artifact_record_observations(baseline));
        } catch(...) {
            // Baseline observation is not pacman transaction policy. Preserve
            // the independent execution witness and leave missing/partial proof.
        }
    }
}

int SourceArtifactInstallTrustedStateStore::execute(const std::string& token) {
    if(!implementation_ || !is_valid_trusted_alpm_receipt_token(token)) {
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "invalid execution request");
    }
    auto& state = *implementation_;
    auto transaction = open_transaction(state.active.get(), state.expected_owner, token);
    auto execution_lease = acquire_lifetime_lease(transaction.descriptor.get(), state.expected_owner,
                                                  token, LOCK_SH);
    for(const auto leaf : {EXECUTION_FILE, AUTHORIZED_FILE, REFUSAL_FILE, OBSERVED_FILE, RECEIPT_FILE, PARTIAL_RECEIPT_FILE}) {
        if(entry_exists(transaction.descriptor.get(), std::string(leaf))) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
                "source-artifact execution authority is already consumed");
        }
    }
    const auto publish = [&](std::string_view leaf, const SourceArtifactInstallExecutionObservation& observation) {
        auto file = create_private_file(transaction.descriptor.get(), std::string(leaf),
                                        state.expected_owner, "execution authority");
        write_all(file.get(), serialize_source_artifact_install_execution_observation(observation));
        synchronize_file(file.get(), "execution authority");
    };
    // O_EXCL is the one-shot claim, independent of the shared lifetime lock.
    // Its durable existence rejects replay even after the last lease FD closes.
    publish(EXECUTION_FILE, {token, false, std::nullopt});
    try {
        std::vector<OwnedDescriptor> retained;
        const auto request = validate_prepared_state(transaction.descriptor.get(), state.expected_owner,
                                                     token, state.reprove_namespace(), execution_lease.get(), &retained);
        auto seal = open_private_file(transaction.descriptor.get(), std::string(IDENTITY_FILE),
                                      state.expected_owner, "staged generation authority");
        const auto seal_identity = StagedFilesystemIdentity::observe(seal.get());
        const std::string initial_seal = read_bounded(seal.get(), MAX_IDENTITY_BYTES, "staged generation authority");
        std::vector<std::string> arguments{"/usr/bin/pacman", "-U"};
        if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
            auto exact = open_directory_at(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), state.expected_owner,
                                           PRIVATE_DIRECTORY_MODE, "exact database world");
            const auto world = read_database_world(exact.get(), state.expected_owner);
            if(const auto* known = std::get_if<InstalledDatabaseWorld>(&world)) {
                // Pin the already-resolved fixed pacman configuration world in
                // argv. Observer failures never invent a custom root or alter
                // transaction policy; unsupported worlds use existing defaults.
                arguments.insert(arguments.end(), {"--config", "/etc/pacman.conf", "--root", known->root_directory,
                                                   "--dbpath", known->database_path});
                write_all(STDERR_FILENO, "moguet: exact transaction RootDir=" + known->root_directory + " DBPath=" + known->database_path + "\n");
            }
        }
        if(request.needed) arguments.emplace_back("--needed");
        if(request.directive == SourceArtifactInstallTrustedDirective::AsDependency) arguments.emplace_back("--asdeps");
        if(request.no_confirm) arguments.emplace_back("--noconfirm");
        arguments.emplace_back("--hookdir");
        arguments.push_back(source_artifact_install_hook_directory(token));
        arguments.emplace_back("--");
        for(std::size_t index = 0; index < request.artifacts.size(); ++index)
            arguments.push_back(source_artifact_install_staged_artifact_path(token, index));
        std::vector<char*> argv;
        for(auto& argument : arguments)
            argv.push_back(argument.data());
        argv.push_back(nullptr);
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
        if(g_state_test_hook) g_state_test_hook(
            SourceArtifactInstallTrustedStateTestEvent::BeforeFinalReproof,
            transaction.descriptor.get(), token);
        if(!g_exec_test_hook) throw_state_error_message("test execution replacement is required");
#endif
        const int descriptor_flags = fcntl(execution_lease.get(), F_GETFD);
        if(descriptor_flags < 0 || fcntl(execution_lease.get(), F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0)
            throw_state_error("unable to retain execution lease across pacman");
        // Threat Model A: kernel/root/helper are trusted. Reproof and exec share
        // this owner and retained namespace/file descriptors. Only the separate
        // authorization record is written below; no stage path is mutated or
        // rediscovered by another Moguet process after this boundary.
        std::vector<OwnedDescriptor> final_retained;
        if(validate_prepared_state(transaction.descriptor.get(), state.expected_owner,
                                   token, state.reprove_namespace(), execution_lease.get(), &final_retained) != request) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "final request drift");
        }
        require_staged_identity(transaction.descriptor.get(), std::string(IDENTITY_FILE), seal.get(), seal_identity);
        auto final_seal = open_private_file(transaction.descriptor.get(), std::string(IDENTITY_FILE),
                                            state.expected_owner, "final staged generation authority");
        if(read_bounded(final_seal.get(), MAX_IDENTITY_BYTES, "final stage identity") != initial_seal) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch, "final identity authority drift");
        }
        require_named_identity(state.active.get(), token, transaction.metadata, "final transaction");
        static_cast<void>(state.reprove_namespace());
        try {
            std::vector<std::string> entries{std::string(PREPARED_FILE), std::string(HOOK_DIRECTORY),
                                             std::string(ARTIFACT_DIRECTORY), std::string(IDENTITY_FILE), std::string(EXECUTION_FILE),
                                             std::string(LIFETIME_FILE)};
            if(request.purpose == SourceArtifactInstallTrustedPurpose::ExactInstalledBinding) {
                entries.emplace_back(EXACT_DIRECTORY);
                auto exact = open_directory_at(transaction.descriptor.get(), std::string(EXACT_DIRECTORY), state.expected_owner,
                                               PRIVATE_DIRECTORY_MODE, "unexecuted exact evidence");
                require_exact_entries(exact.get(), {std::string(DATABASE_WORLD_FILE)}, "unexecuted exact evidence");
            }
            require_exact_entries(transaction.descriptor.get(), entries, "final transaction namespace");
        } catch(const SourceArtifactInstallTrustedStateError& error) {
            throw SourceArtifactInstallTrustedStateError(
                SourceArtifactInstallSealingFailure::TrustedTransportProtocolMismatch,
                error.what(), error.refusal().error_number);
        }
        publish(AUTHORIZED_FILE, {token, true, std::nullopt});
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
        return g_exec_test_hook(arguments);
#else
        // Namespace/artifact FDs remain held until atomic exec. The separate
        // execution lease survives exec, so even an ambiguous outer wait cannot
        // let Moguet consume/abort delete an in-flight package input. A surviving
        // descendant retains cleanup refusal instead of permitting early removal.
        // Pathname and adjacent .sig semantics stay owned by pacman.
        // Preserve the environment established by the same fixed sudo entry
        // used previously for pacman (including its HOME/USER defaults).
        execv("/usr/bin/pacman", argv.data());
        const int error_number = errno;
        throw SourceArtifactInstallTrustedStateError(
            SourceArtifactInstallSealingFailure::ExecutableLaunchFailure,
            "unable to execute fixed pacman", error_number);
#endif
    } catch(const SourceArtifactInstallTrustedStateError& error) {
        try {
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
            if(g_state_test_hook) g_state_test_hook(SourceArtifactInstallTrustedStateTestEvent::BeforeRefusalPublication,
                                                    transaction.descriptor.get(), token);
#endif
            publish(REFUSAL_FILE, {token,
                                   entry_exists(transaction.descriptor.get(), std::string(AUTHORIZED_FILE)), error.refusal()});
        } catch(...) {
            // The caller must classify an unavailable status as unknown; it
            // cannot turn this failed one-shot handoff into a successful one.
        }
        throw;
    } catch(const std::exception& error) {
        SourceArtifactInstallTrustedStateError failure(
            SourceArtifactInstallSealingFailure::StagedArtifactRevalidationFailure, error.what());
        try {
#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
            if(g_state_test_hook) g_state_test_hook(SourceArtifactInstallTrustedStateTestEvent::BeforeRefusalPublication,
                                                    transaction.descriptor.get(), token);
#endif
            publish(REFUSAL_FILE, {token,
                                   entry_exists(transaction.descriptor.get(), std::string(AUTHORIZED_FILE)), failure.refusal()});
        } catch(...) {
        }
        throw failure;
    }
}

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
void set_source_artifact_install_trusted_state_test_hook(SourceArtifactInstallTrustedStateTestHook hook) {
    g_state_test_hook = std::move(hook);
}
void set_source_artifact_install_trusted_exec_test_hook(std::function<int(const std::vector<std::string>&)> hook) {
    g_exec_test_hook = std::move(hook);
}
#endif
