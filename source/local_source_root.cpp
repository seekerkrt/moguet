#include "local_source_root.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

constexpr std::string_view PKGBUILD_LEAF = "PKGBUILD";
constexpr std::string_view SRCINFO_LEAF = ".SRCINFO";

class LocalSourceDescriptor final {
    int descriptor_ = -1;

public:
    explicit LocalSourceDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    LocalSourceDescriptor(const LocalSourceDescriptor&) = delete;
    LocalSourceDescriptor& operator=(const LocalSourceDescriptor&) = delete;

    LocalSourceDescriptor(LocalSourceDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    LocalSourceDescriptor& operator=(LocalSourceDescriptor&& other) noexcept {
        if(this == &other) return *this;
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ~LocalSourceDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
    }

    int get() const noexcept {
        return descriptor_;
    }

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }
};

struct LocalDirectoryCloser {
    void operator()(DIR* directory) const noexcept {
        static_cast<void>(::closedir(directory));
    }
};

enum class LocalSourceObject {
    Root,
    Pkgbuild,
    Srcinfo,
};

enum class LocalSourceInjectionPoint {
    RootInspection,
    RootOpen,
    CanonicalPathResolution,
    PkgbuildInspection,
    PkgbuildOpen,
    PkgbuildRead,
    SrcinfoInspection,
    SrcinfoOpen,
    SrcinfoRead,
};

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
using LocalSourceOpenOverrides = LocalSourceRootTestOverrides;
#else
struct LocalSourceOpenOverrides {};
#endif

struct LocalSourceOpenedFile {
    LocalSourceDescriptor descriptor;
    LocalSourceFileSnapshot snapshot;
};

struct LocalSourceMetadataParts {
    LocalSourceMetadataState state = LocalSourceMetadataState::Missing;
    std::optional<LocalSourceMetadataProvenance> provenance;
    LocalSourceDescriptor descriptor;
    std::optional<LocalSourceFileSnapshot> file;
    std::optional<LocalPackageMetadataParseResult> parse_result;
    std::optional<LocalSourceRootFailure> unsafe_failure;
    std::vector<LocalSourceMetadataStaleReason> stale_reasons;
};

struct LocalSourceRootOpenParts {
    fs::path input_path;
    fs::path lookup_path;
    fs::path canonical_path;
    LocalSourceDescriptor invocation_anchor;
    LocalSourceDescriptor directory;
    LocalSourceDescriptor pkgbuild_descriptor;
    LocalSourceDescriptor srcinfo_descriptor;
    std::uintmax_t expected_owner = 0;
    LocalSourceDirectoryIdentity directory_identity;
    LocalSourceFileSnapshot pkgbuild;
    LocalSourceMetadataParts metadata;
};

std::error_code local_source_system_error(int error_number) {
    return std::error_code(error_number, std::generic_category());
}

fs::path remove_trailing_path_separators(const fs::path& path) {
    fs::path::string_type native = path.native();
    const std::size_t last_component_byte =
        native.find_last_not_of(fs::path::preferred_separator);
    if(last_component_byte == fs::path::string_type::npos) return path;
    native.erase(last_component_byte + 1);
    return fs::path(std::move(native));
}

[[noreturn]] void throw_local_source_failure(
    LocalSourceRootStage stage, LocalSourceRootErrorCode code,
    const fs::path& path, std::optional<int> error_number = std::nullopt) {
    std::optional<std::error_code> system_error;
    if(error_number.has_value()) {
        system_error = local_source_system_error(*error_number);
    }
    throw LocalSourceRootError(
        LocalSourceRootFailure{stage, code, path, system_error});
}

LocalSourceRootErrorCode code_for_system_error(
    int error_number, LocalSourceRootErrorCode fallback,
    bool missing_is_missing) {
    if(error_number == ENOENT && missing_is_missing) {
        return LocalSourceRootErrorCode::Missing;
    }
    if(error_number == ELOOP) return LocalSourceRootErrorCode::Symlink;
    if(error_number == EACCES || error_number == EPERM) {
        return LocalSourceRootErrorCode::PermissionDenied;
    }
    return fallback;
}

[[noreturn]] void throw_local_source_system_failure(
    LocalSourceRootStage stage, LocalSourceRootErrorCode fallback,
    const fs::path& path, int error_number, bool missing_is_missing) {
    throw_local_source_failure(
        stage,
        code_for_system_error(
            error_number, fallback, missing_is_missing),
        path, error_number);
}

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
bool test_failure_point_matches(
    LocalSourceRootTestFailurePoint public_point,
    LocalSourceInjectionPoint internal_point) {
    switch(internal_point) {
        case LocalSourceInjectionPoint::RootInspection:
            return public_point == LocalSourceRootTestFailurePoint::RootInspection;
        case LocalSourceInjectionPoint::RootOpen:
            return public_point == LocalSourceRootTestFailurePoint::RootOpen;
        case LocalSourceInjectionPoint::CanonicalPathResolution:
            return public_point ==
                   LocalSourceRootTestFailurePoint::CanonicalPathResolution;
        case LocalSourceInjectionPoint::PkgbuildInspection:
            return public_point ==
                   LocalSourceRootTestFailurePoint::PkgbuildInspection;
        case LocalSourceInjectionPoint::PkgbuildOpen:
            return public_point == LocalSourceRootTestFailurePoint::PkgbuildOpen;
        case LocalSourceInjectionPoint::PkgbuildRead:
            return public_point == LocalSourceRootTestFailurePoint::PkgbuildRead;
        case LocalSourceInjectionPoint::SrcinfoInspection:
            return public_point ==
                   LocalSourceRootTestFailurePoint::SrcinfoInspection;
        case LocalSourceInjectionPoint::SrcinfoOpen:
            return public_point == LocalSourceRootTestFailurePoint::SrcinfoOpen;
        case LocalSourceInjectionPoint::SrcinfoRead:
            return public_point == LocalSourceRootTestFailurePoint::SrcinfoRead;
    }
    return false;
}
#endif

void maybe_inject_local_source_failure(
    const LocalSourceOpenOverrides* overrides,
    LocalSourceInjectionPoint failure_point, LocalSourceRootStage stage,
    LocalSourceRootErrorCode fallback, const fs::path& path) {
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
    if(overrides != nullptr && overrides->injected_failure.has_value() &&
       test_failure_point_matches(
           overrides->injected_failure->point, failure_point)) {
        const int error_number = overrides->injected_failure->error_number;
        throw_local_source_system_failure(
            stage, fallback, path, error_number, error_number == ENOENT);
    }
#else
    static_cast<void>(overrides);
    static_cast<void>(failure_point);
    static_cast<void>(stage);
    static_cast<void>(fallback);
    static_cast<void>(path);
#endif
}

std::uintmax_t effective_user(
    const LocalSourceOpenOverrides* overrides) noexcept {
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
    if(overrides != nullptr && overrides->effective_user.has_value()) {
        return *overrides->effective_user;
    }
#else
    static_cast<void>(overrides);
#endif
    return static_cast<std::uintmax_t>(::geteuid());
}

std::uintmax_t observed_owner(
    const struct stat& status, LocalSourceObject object,
    const LocalSourceOpenOverrides* overrides) noexcept {
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
    if(overrides != nullptr) {
        const std::optional<std::uintmax_t>* override_owner = nullptr;
        switch(object) {
            case LocalSourceObject::Root:
                override_owner = &overrides->root_observed_owner;
                break;
            case LocalSourceObject::Pkgbuild:
                override_owner = &overrides->pkgbuild_observed_owner;
                break;
            case LocalSourceObject::Srcinfo:
                override_owner = &overrides->srcinfo_observed_owner;
                break;
        }
        if(override_owner->has_value()) return override_owner->value();
    }
#else
    static_cast<void>(object);
    static_cast<void>(overrides);
#endif
    return static_cast<std::uintmax_t>(status.st_uid);
}

void validate_root_status(
    const struct stat& status, std::uintmax_t owner,
    std::uintmax_t expected_owner, LocalSourceRootStage stage,
    const fs::path& path) {
    if(S_ISLNK(status.st_mode)) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::Symlink, path);
    }
    if(!S_ISDIR(status.st_mode)) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::NotDirectory, path);
    }
    if(owner != expected_owner) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::OwnershipMismatch, path);
    }
    if((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::UnsafePermissions, path);
    }
}

void validate_file_status(
    const struct stat& status, std::uintmax_t owner,
    std::uintmax_t expected_owner, LocalSourceRootStage stage,
    const fs::path& path, bool require_single_link = false) {
    if(S_ISLNK(status.st_mode)) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::Symlink, path);
    }
    if(!S_ISREG(status.st_mode)) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::NotRegularFile, path);
    }
    if(owner != expected_owner) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::OwnershipMismatch, path);
    }
    if((status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::UnsafePermissions, path);
    }
    if(require_single_link && status.st_nlink != 1)
        throw_local_source_failure(stage, LocalSourceRootErrorCode::UnsafePermissions, path);
}

LocalSourceDirectoryIdentity make_directory_identity(
    const struct stat& status, std::uintmax_t owner) noexcept {
    return LocalSourceDirectoryIdentity{
        LocalSourceNodeType::Directory,
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        owner,
        static_cast<std::uintmax_t>(status.st_mode & 07777)};
}

LocalSourceFileIdentity make_file_identity(
    const struct stat& status, std::uintmax_t owner) noexcept {
    return LocalSourceFileIdentity{
        LocalSourceNodeType::RegularFile,
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        owner,
        static_cast<std::uintmax_t>(status.st_mode & 07777),
        static_cast<std::intmax_t>(status.st_size),
        static_cast<std::intmax_t>(status.st_mtim.tv_sec),
        static_cast<std::intmax_t>(status.st_mtim.tv_nsec),
        static_cast<std::intmax_t>(status.st_ctim.tv_sec),
        static_cast<std::intmax_t>(status.st_ctim.tv_nsec)};
}

void require_same_directory_observation(
    const LocalSourceDirectoryIdentity& expected,
    const LocalSourceDirectoryIdentity& observed,
    LocalSourceRootStage stage, const fs::path& path) {
    if(expected != observed) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement, path);
    }
}

void require_same_file_observation(
    const LocalSourceFileIdentity& expected,
    const LocalSourceFileIdentity& observed,
    LocalSourceRootStage stage, const fs::path& path) {
    if(expected.device != observed.device || expected.inode != observed.inode ||
       expected.type != observed.type) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement, path);
    }
    if(expected != observed) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::ContentChanged, path);
    }
}

std::string read_descriptor_contents(
    int descriptor, LocalSourceRootStage stage, const fs::path& path,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
    std::string contents;
    std::array<char, 8192> buffer{};
    off_t offset = 0;

    while(true) {
        const ssize_t read_size =
            ::pread(descriptor, buffer.data(), buffer.size(), offset);
        if(read_size > 0) {
            const auto chunk = static_cast<std::size_t>(read_size);
            if(chunk > max_bytes || contents.size() > max_bytes - chunk)
                throw_local_source_failure(stage, LocalSourceRootErrorCode::ReadFailure, path);
            contents.append(buffer.data(), chunk);
            offset += read_size;
            continue;
        }
        if(read_size == 0) break;
        const int read_error = errno;
        if(read_error == EINTR) continue;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::ReadFailure, path,
            read_error, false);
    }
    return contents;
}

fs::path canonical_path_from_descriptor(
    int descriptor, const fs::path& input_path,
    const LocalSourceOpenOverrides* overrides) {
    maybe_inject_local_source_failure(
        overrides, LocalSourceInjectionPoint::CanonicalPathResolution,
        LocalSourceRootStage::CanonicalPathResolution,
        LocalSourceRootErrorCode::MetadataFailure, input_path);

    const fs::path descriptor_path =
        fs::path("/proc/self/fd") / std::to_string(descriptor);
    std::error_code canonical_error;
    fs::path canonical_path = fs::canonical(descriptor_path, canonical_error);
    if(canonical_error) {
        throw LocalSourceRootError(LocalSourceRootFailure{
            LocalSourceRootStage::CanonicalPathResolution,
            LocalSourceRootErrorCode::MetadataFailure,
            input_path,
            canonical_error});
    }
    return canonical_path;
}

void validate_root_path_binding(
    int anchor_descriptor, int directory_descriptor,
    const fs::path& input_path, const fs::path& canonical_path,
    std::uintmax_t expected_owner,
    const LocalSourceDirectoryIdentity& expected_identity,
    LocalSourceRootStage stage,
    const LocalSourceOpenOverrides* overrides = nullptr) {
    struct stat retained_status{};
    if(::fstat(directory_descriptor, &retained_status) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::MetadataFailure,
            canonical_path, status_error, false);
    }
    const std::uintmax_t retained_owner = observed_owner(
        retained_status, LocalSourceObject::Root, overrides);
    validate_root_status(
        retained_status, retained_owner, expected_owner, stage,
        canonical_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(retained_status, retained_owner), stage,
        canonical_path);

    struct stat input_before{};
    if(::fstatat(
           anchor_descriptor, input_path.c_str(), &input_before,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement,
            input_path, status_error, false);
    }
    const std::uintmax_t input_before_owner = observed_owner(
        input_before, LocalSourceObject::Root, overrides);
    validate_root_status(
        input_before, input_before_owner, expected_owner, stage,
        input_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(input_before, input_before_owner), stage,
        input_path);

    LocalSourceDescriptor input_descriptor(::openat(
        anchor_descriptor, input_path.c_str(),
        O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if(input_descriptor.get() < 0) {
        const int open_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement,
            input_path, open_error, false);
    }
    struct stat input_opened{};
    if(::fstat(input_descriptor.get(), &input_opened) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::MetadataFailure,
            input_path, status_error, false);
    }
    const std::uintmax_t input_opened_owner = observed_owner(
        input_opened, LocalSourceObject::Root, overrides);
    validate_root_status(
        input_opened, input_opened_owner, expected_owner, stage,
        input_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(input_opened, input_opened_owner), stage,
        input_path);

    struct stat input_after{};
    if(::fstatat(
           anchor_descriptor, input_path.c_str(), &input_after,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement,
            input_path, status_error, false);
    }
    const std::uintmax_t input_after_owner = observed_owner(
        input_after, LocalSourceObject::Root, overrides);
    validate_root_status(
        input_after, input_after_owner, expected_owner, stage,
        input_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(input_after, input_after_owner), stage,
        input_path);

    struct stat canonical_status{};
    if(::fstatat(
           AT_FDCWD, canonical_path.c_str(), &canonical_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement,
            canonical_path, status_error, false);
    }
    const std::uintmax_t canonical_owner = observed_owner(
        canonical_status, LocalSourceObject::Root, overrides);
    validate_root_status(
        canonical_status, canonical_owner, expected_owner, stage,
        canonical_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(canonical_status, canonical_owner),
        stage, canonical_path);
}

std::optional<LocalSourceOpenedFile> inspect_local_source_file(
    int directory_descriptor, std::string_view leaf,
    const fs::path& display_path, std::uintmax_t expected_owner,
    LocalSourceObject object, LocalSourceRootStage inspection_stage,
    LocalSourceRootStage open_stage, LocalSourceRootStage read_stage,
    LocalSourceInjectionPoint inspection_injection,
    LocalSourceInjectionPoint open_injection,
    LocalSourceInjectionPoint read_injection, bool optional_file,
    bool revalidation, const LocalSourceOpenOverrides* overrides,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max(),
    bool require_single_link = false) {
    maybe_inject_local_source_failure(
        overrides, inspection_injection, inspection_stage,
        LocalSourceRootErrorCode::MetadataFailure, display_path);

    const std::string leaf_string(leaf);
    struct stat named_before{};
    if(::fstatat(
           directory_descriptor, leaf_string.c_str(), &named_before,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        if(optional_file && status_error == ENOENT) return std::nullopt;
        throw_local_source_system_failure(
            inspection_stage,
            revalidation
                ? LocalSourceRootErrorCode::ConcurrentReplacement
                : LocalSourceRootErrorCode::MetadataFailure,
            display_path, status_error,
            !revalidation && !optional_file);
    }
    const std::uintmax_t named_before_owner =
        observed_owner(named_before, object, overrides);
    validate_file_status(
        named_before, named_before_owner, expected_owner,
        inspection_stage, display_path, require_single_link);
    const LocalSourceFileIdentity before_identity =
        make_file_identity(named_before, named_before_owner);

    maybe_inject_local_source_failure(
        overrides, open_injection, open_stage,
        LocalSourceRootErrorCode::MetadataFailure, display_path);
    LocalSourceDescriptor descriptor(::openat(
        directory_descriptor, leaf_string.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if(descriptor.get() < 0) {
        const int open_error = errno;
        throw_local_source_system_failure(
            open_stage,
            revalidation
                ? LocalSourceRootErrorCode::ConcurrentReplacement
                : LocalSourceRootErrorCode::MetadataFailure,
            display_path, open_error, false);
    }

    struct stat opened_status{};
    if(::fstat(descriptor.get(), &opened_status) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            open_stage, LocalSourceRootErrorCode::MetadataFailure,
            display_path, status_error, false);
    }
    const std::uintmax_t opened_owner =
        observed_owner(opened_status, object, overrides);
    validate_file_status(
        opened_status, opened_owner, expected_owner, open_stage,
        display_path, require_single_link);
    const LocalSourceFileIdentity opened_identity =
        make_file_identity(opened_status, opened_owner);
    require_same_file_observation(
        before_identity, opened_identity, open_stage, display_path);

    maybe_inject_local_source_failure(
        overrides, read_injection, read_stage,
        LocalSourceRootErrorCode::ReadFailure, display_path);
    std::string contents = read_descriptor_contents(
        descriptor.get(), read_stage, display_path, max_bytes);

    struct stat descriptor_after{};
    if(::fstat(descriptor.get(), &descriptor_after) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            read_stage, LocalSourceRootErrorCode::MetadataFailure,
            display_path, status_error, false);
    }
    const std::uintmax_t descriptor_after_owner =
        observed_owner(descriptor_after, object, overrides);
    validate_file_status(
        descriptor_after, descriptor_after_owner, expected_owner,
        read_stage, display_path, require_single_link);
    const LocalSourceFileIdentity descriptor_after_identity =
        make_file_identity(descriptor_after, descriptor_after_owner);
    require_same_file_observation(
        opened_identity, descriptor_after_identity, read_stage,
        display_path);

    struct stat named_after{};
    if(::fstatat(
           directory_descriptor, leaf_string.c_str(), &named_after,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            read_stage, LocalSourceRootErrorCode::ConcurrentReplacement,
            display_path, status_error, false);
    }
    const std::uintmax_t named_after_owner =
        observed_owner(named_after, object, overrides);
    validate_file_status(
        named_after, named_after_owner, expected_owner, read_stage,
        display_path, require_single_link);
    require_same_file_observation(
        descriptor_after_identity,
        make_file_identity(named_after, named_after_owner), read_stage,
        display_path);

    return LocalSourceOpenedFile{
        std::move(descriptor),
        LocalSourceFileSnapshot{
            display_path,
            descriptor_after_identity,
            std::move(contents)}};
}

bool modification_time_is_newer(
    const LocalSourceFileIdentity& candidate,
    const LocalSourceFileIdentity& reference) noexcept {
    return std::tie(
               candidate.modification_time_seconds,
               candidate.modification_time_nanoseconds) >
           std::tie(
               reference.modification_time_seconds,
               reference.modification_time_nanoseconds);
}

LocalSourceMetadataParts inspect_local_source_metadata(
    int directory_descriptor, const fs::path& canonical_path,
    std::uintmax_t expected_owner,
    const LocalSourceFileSnapshot& pkgbuild,
    bool has_one_off_environment_assignment,
    const LocalSourceOpenOverrides* overrides) {
    LocalSourceMetadataParts metadata;
    const fs::path srcinfo_path = canonical_path / SRCINFO_LEAF;

    try {
        std::optional<LocalSourceOpenedFile> opened =
            inspect_local_source_file(
                directory_descriptor, SRCINFO_LEAF, srcinfo_path,
                expected_owner, LocalSourceObject::Srcinfo,
                LocalSourceRootStage::SrcinfoInspection,
                LocalSourceRootStage::SrcinfoOpen,
                LocalSourceRootStage::SrcinfoRead,
                LocalSourceInjectionPoint::SrcinfoInspection,
                LocalSourceInjectionPoint::SrcinfoOpen,
                LocalSourceInjectionPoint::SrcinfoRead, true, false,
                overrides);
        if(!opened.has_value()) return metadata;

        metadata.provenance =
            LocalSourceMetadataProvenance::ExistingSrcinfo;
        metadata.file = std::move(opened->snapshot);
        metadata.descriptor = std::move(opened->descriptor);
        metadata.parse_result.emplace(
            parse_local_package_metadata(metadata.file->contents));
        if(!metadata.parse_result->is_success()) {
            metadata.state = LocalSourceMetadataState::Invalid;
            return metadata;
        }

        if(modification_time_is_newer(
               pkgbuild.identity, metadata.file->identity)) {
            metadata.stale_reasons.push_back(
                LocalSourceMetadataStaleReason::PkgbuildNewer);
        }
        if(has_one_off_environment_assignment) {
            metadata.stale_reasons.push_back(
                LocalSourceMetadataStaleReason::
                    OneOffEnvironmentAssignment);
        }
        metadata.state = metadata.stale_reasons.empty()
                             ? LocalSourceMetadataState::UsableUnverified
                             : LocalSourceMetadataState::KnownStale;
        return metadata;
    } catch(const LocalSourceRootError& error) {
        metadata.state = LocalSourceMetadataState::Unsafe;
        metadata.provenance =
            LocalSourceMetadataProvenance::ExistingSrcinfo;
        metadata.unsafe_failure = error.failure();
        return metadata;
    }
}

void revalidate_retained_and_named_file(
    int directory_descriptor, int retained_descriptor,
    std::string_view leaf, const LocalSourceFileSnapshot& expected,
    std::uintmax_t expected_owner, LocalSourceObject object,
    LocalSourceRootStage stage);
void require_srcinfo_still_missing(
    int directory_descriptor, const fs::path& srcinfo_path);

LocalSourceRootOpenParts inspect_local_source_root(
    const fs::path& input_path,
    bool has_one_off_environment_assignment,
    const LocalSourceOpenOverrides* overrides) {
    if(input_path.empty() ||
       input_path.native().find('\0') != fs::path::string_type::npos) {
        throw_local_source_failure(
            LocalSourceRootStage::RootInspection,
            LocalSourceRootErrorCode::InvalidInputPath, input_path);
    }
    const fs::path lookup_path = remove_trailing_path_separators(input_path);

    LocalSourceDescriptor invocation_anchor(
        ::open(".", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if(invocation_anchor.get() < 0) {
        const int open_error = errno;
        throw_local_source_system_failure(
            LocalSourceRootStage::InvocationAnchorOpen,
            LocalSourceRootErrorCode::MetadataFailure, fs::path("."),
            open_error, false);
    }

    const std::uintmax_t expected_owner = effective_user(overrides);
    maybe_inject_local_source_failure(
        overrides, LocalSourceInjectionPoint::RootInspection,
        LocalSourceRootStage::RootInspection,
        LocalSourceRootErrorCode::MetadataFailure, input_path);
    struct stat named_before{};
    if(::fstatat(
           invocation_anchor.get(), lookup_path.c_str(), &named_before,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            LocalSourceRootStage::RootInspection,
            status_error == ENOTDIR
                ? LocalSourceRootErrorCode::NotDirectory
                : LocalSourceRootErrorCode::MetadataFailure,
            input_path, status_error, true);
    }
    const std::uintmax_t named_before_owner = observed_owner(
        named_before, LocalSourceObject::Root, overrides);
    validate_root_status(
        named_before, named_before_owner, expected_owner,
        LocalSourceRootStage::RootInspection, input_path);
    const LocalSourceDirectoryIdentity expected_identity =
        make_directory_identity(named_before, named_before_owner);

    maybe_inject_local_source_failure(
        overrides, LocalSourceInjectionPoint::RootOpen,
        LocalSourceRootStage::RootOpen,
        LocalSourceRootErrorCode::MetadataFailure, input_path);
    LocalSourceDescriptor directory(::openat(
        invocation_anchor.get(), lookup_path.c_str(),
        O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if(directory.get() < 0) {
        const int open_error = errno;
        throw_local_source_system_failure(
            LocalSourceRootStage::RootOpen,
            open_error == ENOTDIR
                ? LocalSourceRootErrorCode::NotDirectory
                : LocalSourceRootErrorCode::ConcurrentReplacement,
            input_path, open_error, false);
    }
    struct stat opened_status{};
    if(::fstat(directory.get(), &opened_status) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            LocalSourceRootStage::RootOpen,
            LocalSourceRootErrorCode::MetadataFailure, input_path,
            status_error, false);
    }
    const std::uintmax_t opened_owner = observed_owner(
        opened_status, LocalSourceObject::Root, overrides);
    validate_root_status(
        opened_status, opened_owner, expected_owner,
        LocalSourceRootStage::RootOpen, input_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(opened_status, opened_owner),
        LocalSourceRootStage::RootOpen, input_path);

    struct stat named_after{};
    if(::fstatat(
           invocation_anchor.get(), lookup_path.c_str(), &named_after,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            LocalSourceRootStage::RootOpen,
            LocalSourceRootErrorCode::ConcurrentReplacement, input_path,
            status_error, false);
    }
    const std::uintmax_t named_after_owner = observed_owner(
        named_after, LocalSourceObject::Root, overrides);
    validate_root_status(
        named_after, named_after_owner, expected_owner,
        LocalSourceRootStage::RootOpen, input_path);
    require_same_directory_observation(
        expected_identity,
        make_directory_identity(named_after, named_after_owner),
        LocalSourceRootStage::RootOpen, input_path);

    fs::path canonical_path = canonical_path_from_descriptor(
        directory.get(), input_path, overrides);
    validate_root_path_binding(
        invocation_anchor.get(), directory.get(), lookup_path,
        canonical_path, expected_owner, expected_identity,
        LocalSourceRootStage::RootRevalidation, overrides);

    const fs::path pkgbuild_path = canonical_path / PKGBUILD_LEAF;
    std::optional<LocalSourceOpenedFile> pkgbuild = inspect_local_source_file(
        directory.get(), PKGBUILD_LEAF, pkgbuild_path, expected_owner,
        LocalSourceObject::Pkgbuild,
        LocalSourceRootStage::PkgbuildInspection,
        LocalSourceRootStage::PkgbuildOpen,
        LocalSourceRootStage::PkgbuildRead,
        LocalSourceInjectionPoint::PkgbuildInspection,
        LocalSourceInjectionPoint::PkgbuildOpen,
        LocalSourceInjectionPoint::PkgbuildRead, false, false, overrides);

    LocalSourceMetadataParts metadata = inspect_local_source_metadata(
        directory.get(), canonical_path, expected_owner,
        pkgbuild->snapshot, has_one_off_environment_assignment,
        overrides);

    revalidate_retained_and_named_file(
        directory.get(), pkgbuild->descriptor.get(), PKGBUILD_LEAF,
        pkgbuild->snapshot, expected_owner, LocalSourceObject::Pkgbuild,
        LocalSourceRootStage::PkgbuildRevalidation);

    switch(metadata.state) {
        case LocalSourceMetadataState::Missing:
            require_srcinfo_still_missing(
                directory.get(), canonical_path / SRCINFO_LEAF);
            break;
        case LocalSourceMetadataState::Unsafe:
            // Unsafe metadata is retained only for typed inspection and can never
            // pass operation-time validation.
            break;
        case LocalSourceMetadataState::Invalid:
        case LocalSourceMetadataState::UsableUnverified:
        case LocalSourceMetadataState::KnownStale:
            if(!metadata.file.has_value() || metadata.descriptor.get() < 0) {
                throw_local_source_failure(
                    LocalSourceRootStage::SrcinfoRevalidation,
                    LocalSourceRootErrorCode::MetadataFailure,
                    canonical_path / SRCINFO_LEAF);
            }
            revalidate_retained_and_named_file(
                directory.get(), metadata.descriptor.get(), SRCINFO_LEAF,
                *metadata.file, expected_owner, LocalSourceObject::Srcinfo,
                LocalSourceRootStage::SrcinfoRevalidation);
            break;
    }

    validate_root_path_binding(
        invocation_anchor.get(), directory.get(), lookup_path,
        canonical_path, expected_owner, expected_identity,
        LocalSourceRootStage::RootRevalidation, overrides);

    return LocalSourceRootOpenParts{
        input_path,
        lookup_path,
        std::move(canonical_path),
        std::move(invocation_anchor),
        std::move(directory),
        std::move(pkgbuild->descriptor),
        std::move(metadata.descriptor),
        expected_owner,
        expected_identity,
        std::move(pkgbuild->snapshot),
        std::move(metadata)};
}

void revalidate_retained_and_named_file(
    int directory_descriptor, int retained_descriptor,
    std::string_view leaf, const LocalSourceFileSnapshot& expected,
    std::uintmax_t expected_owner, LocalSourceObject object,
    LocalSourceRootStage stage) {
    if(retained_descriptor < 0) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::MetadataFailure,
            expected.path);
    }

    // Check the current pathname first. A rename changes the retained file's
    // ctime too, but the controlling fact is that the constant leaf now names
    // a different inode.
    const std::string leaf_string(leaf);
    struct stat named_status{};
    if(::fstatat(
           directory_descriptor, leaf_string.c_str(), &named_status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::ConcurrentReplacement,
            expected.path, status_error, false);
    }
    const std::uintmax_t named_owner =
        observed_owner(named_status, object, nullptr);
    validate_file_status(
        named_status, named_owner, expected_owner, stage, expected.path);
    require_same_file_observation(
        expected.identity, make_file_identity(named_status, named_owner),
        stage, expected.path);

    struct stat retained_before{};
    if(::fstat(retained_descriptor, &retained_before) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::MetadataFailure,
            expected.path, status_error, false);
    }
    const std::uintmax_t retained_before_owner =
        observed_owner(retained_before, object, nullptr);
    validate_file_status(
        retained_before, retained_before_owner, expected_owner, stage,
        expected.path);
    require_same_file_observation(
        expected.identity,
        make_file_identity(retained_before, retained_before_owner), stage,
        expected.path);

    const std::string retained_contents = read_descriptor_contents(
        retained_descriptor, stage, expected.path);
    struct stat retained_after{};
    if(::fstat(retained_descriptor, &retained_after) != 0) {
        const int status_error = errno;
        throw_local_source_system_failure(
            stage, LocalSourceRootErrorCode::MetadataFailure,
            expected.path, status_error, false);
    }
    const std::uintmax_t retained_after_owner =
        observed_owner(retained_after, object, nullptr);
    validate_file_status(
        retained_after, retained_after_owner, expected_owner, stage,
        expected.path);
    require_same_file_observation(
        expected.identity,
        make_file_identity(retained_after, retained_after_owner), stage,
        expected.path);
    if(retained_contents != expected.contents) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::ContentChanged,
            expected.path);
    }

    const LocalSourceInjectionPoint inspection_injection =
        object == LocalSourceObject::Pkgbuild
            ? LocalSourceInjectionPoint::PkgbuildInspection
            : LocalSourceInjectionPoint::SrcinfoInspection;
    const LocalSourceInjectionPoint open_injection =
        object == LocalSourceObject::Pkgbuild
            ? LocalSourceInjectionPoint::PkgbuildOpen
            : LocalSourceInjectionPoint::SrcinfoOpen;
    const LocalSourceInjectionPoint read_injection =
        object == LocalSourceObject::Pkgbuild
            ? LocalSourceInjectionPoint::PkgbuildRead
            : LocalSourceInjectionPoint::SrcinfoRead;
    std::optional<LocalSourceOpenedFile> named = inspect_local_source_file(
        directory_descriptor, leaf, expected.path, expected_owner, object,
        stage, stage, stage, inspection_injection, open_injection,
        read_injection, false, true, nullptr);
    require_same_file_observation(
        expected.identity, named->snapshot.identity, stage,
        expected.path);
    if(named->snapshot.contents != expected.contents) {
        throw_local_source_failure(
            stage, LocalSourceRootErrorCode::ContentChanged,
            expected.path);
    }
}

void require_srcinfo_still_missing(
    int directory_descriptor, const fs::path& srcinfo_path) {
    struct stat status{};
    if(::fstatat(
           directory_descriptor, SRCINFO_LEAF.data(), &status,
           AT_SYMLINK_NOFOLLOW) != 0) {
        const int status_error = errno;
        if(status_error == ENOENT) return;
        throw_local_source_system_failure(
            LocalSourceRootStage::MetadataRevalidation,
            LocalSourceRootErrorCode::MetadataFailure, srcinfo_path,
            status_error, false);
    }
    throw_local_source_failure(
        LocalSourceRootStage::MetadataRevalidation,
        LocalSourceRootErrorCode::ConcurrentReplacement, srcinfo_path);
}

} // namespace

LocalSourceRootError::LocalSourceRootError(LocalSourceRootFailure failure)
    : std::runtime_error("local source root failure"),
      failure_(std::move(failure)) {
}

LocalSourceMetadataSnapshot::LocalSourceMetadataSnapshot(
    LocalSourceMetadataState state,
    std::optional<LocalSourceMetadataProvenance> provenance,
    std::optional<LocalSourceFileSnapshot> file,
    std::optional<LocalPackageMetadataParseResult> parse_result,
    std::optional<LocalSourceRootFailure> unsafe_failure,
    std::vector<LocalSourceMetadataStaleReason> stale_reasons) noexcept
    : state_(state),
      provenance_(std::move(provenance)),
      file_(std::move(file)),
      parse_result_(std::move(parse_result)),
      unsafe_failure_(std::move(unsafe_failure)),
      stale_reasons_(std::move(stale_reasons)) {
}

LocalSourceRoot::LocalSourceRoot(
    fs::path input_path, fs::path lookup_path, fs::path canonical_path,
    int invocation_anchor_descriptor, int directory_descriptor,
    int pkgbuild_descriptor, int srcinfo_descriptor,
    std::uintmax_t expected_owner,
    LocalSourceDirectoryIdentity directory_identity,
    LocalSourceFileSnapshot pkgbuild,
    LocalSourceMetadataSnapshot metadata) noexcept
    : input_path_(std::move(input_path)),
      lookup_path_(std::move(lookup_path)),
      canonical_path_(std::move(canonical_path)),
      invocation_anchor_descriptor_(invocation_anchor_descriptor),
      directory_descriptor_(directory_descriptor),
      pkgbuild_descriptor_(pkgbuild_descriptor),
      srcinfo_descriptor_(srcinfo_descriptor),
      expected_owner_(expected_owner),
      directory_identity_(directory_identity),
      pkgbuild_(std::move(pkgbuild)),
      metadata_(std::move(metadata)) {
}

LocalSourceRoot::LocalSourceRoot(LocalSourceRoot&& other) noexcept
    : input_path_(std::move(other.input_path_)),
      lookup_path_(std::move(other.lookup_path_)),
      canonical_path_(std::move(other.canonical_path_)),
      invocation_anchor_descriptor_(
          std::exchange(other.invocation_anchor_descriptor_, -1)),
      directory_descriptor_(std::exchange(other.directory_descriptor_, -1)),
      pkgbuild_descriptor_(std::exchange(other.pkgbuild_descriptor_, -1)),
      srcinfo_descriptor_(std::exchange(other.srcinfo_descriptor_, -1)),
      expected_owner_(other.expected_owner_),
      directory_identity_(other.directory_identity_),
      pkgbuild_(std::move(other.pkgbuild_)),
      metadata_(std::move(other.metadata_)) {
}

LocalSourceRoot::~LocalSourceRoot() noexcept {
    if(srcinfo_descriptor_ >= 0) {
        static_cast<void>(::close(srcinfo_descriptor_));
    }
    if(pkgbuild_descriptor_ >= 0) {
        static_cast<void>(::close(pkgbuild_descriptor_));
    }
    if(directory_descriptor_ >= 0) {
        static_cast<void>(::close(directory_descriptor_));
    }
    if(invocation_anchor_descriptor_ >= 0) {
        static_cast<void>(::close(invocation_anchor_descriptor_));
    }
}

void LocalSourceRoot::require_unchanged_identity() const {
    validate_root_path_binding(
        invocation_anchor_descriptor_, directory_descriptor_, lookup_path_,
        canonical_path_, expected_owner_, directory_identity_,
        LocalSourceRootStage::RootRevalidation);

    revalidate_retained_and_named_file(
        directory_descriptor_, pkgbuild_descriptor_, PKGBUILD_LEAF,
        pkgbuild_, expected_owner_, LocalSourceObject::Pkgbuild,
        LocalSourceRootStage::PkgbuildRevalidation);

    switch(metadata_.state_) {
        case LocalSourceMetadataState::Missing:
            require_srcinfo_still_missing(
                directory_descriptor_, canonical_path_ / SRCINFO_LEAF);
            break;
        case LocalSourceMetadataState::Unsafe:
            throw_local_source_failure(
                LocalSourceRootStage::MetadataRevalidation,
                LocalSourceRootErrorCode::UnsafeMetadata,
                metadata_.unsafe_failure_.has_value()
                    ? metadata_.unsafe_failure_->path
                    : canonical_path_ / SRCINFO_LEAF);
        case LocalSourceMetadataState::Invalid:
        case LocalSourceMetadataState::UsableUnverified:
        case LocalSourceMetadataState::KnownStale:
            if(!metadata_.file_.has_value()) {
                throw_local_source_failure(
                    LocalSourceRootStage::MetadataRevalidation,
                    LocalSourceRootErrorCode::MetadataFailure,
                    canonical_path_ / SRCINFO_LEAF);
            }
            revalidate_retained_and_named_file(
                directory_descriptor_, srcinfo_descriptor_, SRCINFO_LEAF,
                *metadata_.file_, expected_owner_, LocalSourceObject::Srcinfo,
                LocalSourceRootStage::SrcinfoRevalidation);
            break;
    }

    validate_root_path_binding(
        invocation_anchor_descriptor_, directory_descriptor_, lookup_path_,
        canonical_path_, expected_owner_, directory_identity_,
        LocalSourceRootStage::RootRevalidation);
}

bool same_supported_recipe_content_and_mode(
    const SupportedRecipeSnapshot& left, const SupportedRecipeSnapshot& right) {
    if(left.size() != right.size()) return false;
    for(std::size_t i = 0; i < left.size(); ++i) {
        if(left[i].relative_path != right[i].relative_path ||
           left[i].file.contents != right[i].file.contents ||
           left[i].file.identity.mode != right[i].file.identity.mode ||
           left[i].file.identity.owner != right[i].file.identity.owner)
            return false;
    }
    return true;
}

SupportedRecipeSnapshot snapshot_supported_recipe_files(const LocalSourceRoot& root) {
    constexpr std::size_t single_limit = 64U * 1024U * 1024U;
    constexpr std::size_t aggregate_limit = 256U * 1024U * 1024U;
    root.require_unchanged_identity();
    SupportedRecipeSnapshot result{{"PKGBUILD", root.pkgbuild_}};
    std::size_t aggregate_bytes = root.pkgbuild_.contents.size();
    if(aggregate_bytes > aggregate_limit)
        throw_local_source_failure(LocalSourceRootStage::PkgbuildRead,
                                   LocalSourceRootErrorCode::ReadFailure, root.pkgbuild_.path);
    const int scan_descriptor = ::openat(root.directory_descriptor_, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(scan_descriptor < 0)
        throw_local_source_system_failure(LocalSourceRootStage::RootRevalidation,
                                          LocalSourceRootErrorCode::MetadataFailure,
                                          root.canonical_path_, errno, false);
    std::unique_ptr<DIR, LocalDirectoryCloser> directory(::fdopendir(scan_descriptor));
    if(!directory) {
        const int open_error = errno;
        static_cast<void>(::close(scan_descriptor));
        throw_local_source_system_failure(LocalSourceRootStage::RootRevalidation,
                                          LocalSourceRootErrorCode::MetadataFailure,
                                          root.canonical_path_, open_error, false);
    }
    std::vector<std::string> names;
    errno = 0;
    while(const dirent* entry = ::readdir(directory.get())) {
        const std::string name(entry->d_name);
        if(name.size() > std::string_view(".install").size() && name.ends_with(".install")) {
            if(names.size() >= 1024)
                throw_local_source_failure(LocalSourceRootStage::RootRevalidation,
                                           LocalSourceRootErrorCode::MetadataFailure, root.canonical_path_);
            names.push_back(name);
        }
        errno = 0;
    }
    if(errno != 0)
        throw_local_source_system_failure(LocalSourceRootStage::RootRevalidation,
                                          LocalSourceRootErrorCode::MetadataFailure,
                                          root.canonical_path_, errno, false);
    std::sort(names.begin(), names.end());
    for(const std::string& name : names) {
        auto opened = inspect_local_source_file(
            root.directory_descriptor_, name, root.canonical_path_ / name,
            root.expected_owner_, LocalSourceObject::Pkgbuild,
            LocalSourceRootStage::PkgbuildInspection,
            LocalSourceRootStage::PkgbuildOpen,
            LocalSourceRootStage::PkgbuildRead,
            LocalSourceInjectionPoint::PkgbuildInspection,
            LocalSourceInjectionPoint::PkgbuildOpen,
            LocalSourceInjectionPoint::PkgbuildRead,
            false, true, nullptr,
            std::min(single_limit, aggregate_limit - aggregate_bytes), true);
        aggregate_bytes += opened->snapshot.contents.size();
        result.push_back({name, std::move(opened->snapshot)});
    }
    root.require_unchanged_identity();
    return result;
}

LocalSourceRoot open_local_source_root(
    const fs::path& input_path,
    bool has_one_off_environment_assignment) {
    LocalSourceRootOpenParts parts = inspect_local_source_root(
        input_path, has_one_off_environment_assignment, nullptr);
    LocalSourceMetadataSnapshot metadata(
        parts.metadata.state, parts.metadata.provenance,
        std::move(parts.metadata.file),
        std::move(parts.metadata.parse_result),
        std::move(parts.metadata.unsafe_failure),
        std::move(parts.metadata.stale_reasons));
    return LocalSourceRoot(
        std::move(parts.input_path), std::move(parts.lookup_path),
        std::move(parts.canonical_path),
        parts.invocation_anchor.release(), parts.directory.release(),
        parts.pkgbuild_descriptor.release(),
        parts.srcinfo_descriptor.release(), parts.expected_owner,
        parts.directory_identity, std::move(parts.pkgbuild),
        std::move(metadata));
}

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
struct LocalSourceRootTestAccess {
    static LocalSourceRoot open(
        const fs::path& input_path,
        bool has_one_off_environment_assignment,
        const LocalSourceRootTestOverrides& overrides) {
        LocalSourceRootOpenParts parts = inspect_local_source_root(
            input_path, has_one_off_environment_assignment, &overrides);
        LocalSourceMetadataSnapshot metadata(
            parts.metadata.state, parts.metadata.provenance,
            std::move(parts.metadata.file),
            std::move(parts.metadata.parse_result),
            std::move(parts.metadata.unsafe_failure),
            std::move(parts.metadata.stale_reasons));
        return LocalSourceRoot(
            std::move(parts.input_path), std::move(parts.lookup_path),
            std::move(parts.canonical_path),
            parts.invocation_anchor.release(), parts.directory.release(),
            parts.pkgbuild_descriptor.release(),
            parts.srcinfo_descriptor.release(), parts.expected_owner,
            parts.directory_identity, std::move(parts.pkgbuild),
            std::move(metadata));
    }
};

LocalSourceRoot open_local_source_root_for_test(
    const fs::path& input_path,
    bool has_one_off_environment_assignment,
    const LocalSourceRootTestOverrides& overrides) {
    return LocalSourceRootTestAccess::open(
        input_path, has_one_off_environment_assignment, overrides);
}
#endif
