#include "artifact_archive_metadata.hpp"

#include "artifact_workspace.hpp"
#include "localization.hpp"
#include "package_identifier.hpp"

#include <alpm.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace artifact_archive_metadata {
namespace {

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

bool has_raw_whitespace_or_control(const std::string& value) noexcept {
    for(const unsigned char character : value) {
        if(character <= 0x20 || character == 0x7f) return true;
    }
    return false;
}

[[noreturn]] void throw_archive_query_failure() {
    throw std::runtime_error(localization::format_translated_message(
        // TRANSLATORS: {} is the literal library name "libalpm".
        "Failed to read package archive metadata with {}.", "libalpm"));
}

[[noreturn]] void throw_invalid_archive_core_identity() {
    throw std::runtime_error(localization::translate_message(
        "Package archive metadata contains an invalid name or version."));
}

ArtifactPackageBaseIdentity project_package_base(const char* raw_value) {
    if(raw_value == nullptr) return ArtifactPackageBaseIdentity::missing();

    const std::string value(raw_value);
    if(value.empty() || !is_valid_package_name(value)) {
        return ArtifactPackageBaseIdentity::malformed();
    }
    return ArtifactPackageBaseIdentity::known(value);
}

ArtifactPackageArchitectureIdentity project_architecture(
    const char* raw_value) {
    if(raw_value == nullptr) {
        return ArtifactPackageArchitectureIdentity::missing();
    }

    const std::string value(raw_value);
    if(value.empty() || has_raw_whitespace_or_control(value)) {
        return ArtifactPackageArchitectureIdentity::malformed();
    }
    return ArtifactPackageArchitectureIdentity::known(value);
}

ArtifactPackageIdentity query_path_with_libalpm(
    const std::filesystem::path& artifact_path) {
    alpm_errno_t initialization_error = ALPM_ERR_OK;
    // POLICY(#485): alpm_pkg_load() needs a handle but not a registered DB.
    // Reusing the normal absolute root/db path creates no transaction and
    // keeps this query independent from pacman output/localization parsing.
    AlpmHandle handle(
        alpm_initialize("/", "/var/lib/pacman", &initialization_error));
    if(handle == nullptr) throw_archive_query_failure();

    alpm_pkg_t* loaded_package = nullptr;
    if(alpm_pkg_load(
           handle.get(), artifact_path.c_str(), 0, 0,
           &loaded_package) != 0 ||
       loaded_package == nullptr) {
        throw_archive_query_failure();
    }
    AlpmPackage package(loaded_package);

    const char* raw_name = alpm_pkg_get_name(package.get());
    const char* raw_version = alpm_pkg_get_version(package.get());
    if(raw_name == nullptr || raw_version == nullptr) {
        throw_invalid_archive_core_identity();
    }

    std::string package_name(raw_name);
    std::string full_version(raw_version);
    if(!is_valid_package_name(package_name) || full_version.empty() ||
       has_raw_whitespace_or_control(full_version)) {
        throw_invalid_archive_core_identity();
    }

    return ArtifactPackageIdentity{
        std::move(package_name),
        std::move(full_version),
        project_package_base(alpm_pkg_get_base(package.get())),
        project_architecture(alpm_pkg_get_arch(package.get()))};
}

} // namespace

QueryAuthority::QueryAuthority(
    const ValidatedPackageArtifactPath& artifact)
    : artifact_(&artifact) {
    artifact.require_validity();
}

QueryAuthority::QueryAuthority(
    const ValidatedPackageArtifactSet& artifacts,
    std::size_t artifact_index)
    : artifacts_(&artifacts), artifact_index_(artifact_index) {
    artifacts.require_validity();
    static_cast<void>(artifacts.path_at(artifact_index));
}

void QueryAuthority::require_validity() const {
    if(artifact_ != nullptr && artifacts_ == nullptr) {
        artifact_->require_validity();
        return;
    }
    if(artifact_ == nullptr && artifacts_ != nullptr) {
        artifacts_->require_validity();
        static_cast<void>(artifacts_->path_at(artifact_index_));
        return;
    }
    throw std::logic_error(
        "Artifact archive metadata query authority is incoherent.");
}

const std::filesystem::path& QueryAuthority::path() const {
    require_validity();
    return artifact_ != nullptr ? artifact_->path()
                                : artifacts_->path_at(artifact_index_);
}

ArtifactPackageIdentity query_with_libalpm(
    const QueryAuthority& authority) {
    authority.require_validity();
    ArtifactPackageIdentity identity =
        query_path_with_libalpm(authority.path());
    authority.require_validity();
    return identity;
}

RetainedDescriptorQueryAuthority::RetainedDescriptorQueryAuthority(
    int descriptor)
    : descriptor_(descriptor) {
    struct stat status{};
    if(descriptor_ < 0 || ::fstat(descriptor_, &status) != 0 ||
       !S_ISREG(status.st_mode) || status.st_size < 0) {
        throw_archive_query_failure();
    }
    device_ = static_cast<std::uintmax_t>(status.st_dev);
    inode_ = static_cast<std::uintmax_t>(status.st_ino);
    owner_ = static_cast<std::uintmax_t>(status.st_uid);
    size_ = static_cast<std::uintmax_t>(status.st_size);
}

void RetainedDescriptorQueryAuthority::require_validity() const {
    struct stat status{};
    if(descriptor_ < 0 || ::fstat(descriptor_, &status) != 0 ||
       !S_ISREG(status.st_mode) || status.st_size < 0 ||
       static_cast<std::uintmax_t>(status.st_dev) != device_ ||
       static_cast<std::uintmax_t>(status.st_ino) != inode_ ||
       static_cast<std::uintmax_t>(status.st_uid) != owner_ ||
       static_cast<std::uintmax_t>(status.st_size) != size_) {
        throw_archive_query_failure();
    }
}

std::filesystem::path
RetainedDescriptorQueryAuthority::proc_descriptor_path() const {
    require_validity();
    return std::filesystem::path("/proc") / std::to_string(::getpid()) /
           "fd" / std::to_string(descriptor_);
}

ArtifactPackageIdentity query_with_libalpm(
    const RetainedDescriptorQueryAuthority& authority) {
    authority.require_validity();
    ArtifactPackageIdentity identity =
        query_path_with_libalpm(authority.proc_descriptor_path());
    authority.require_validity();
    return identity;
}

#ifdef MOGUET_ENABLE_ARTIFACT_IDENTITY_TEST_HOOKS
ArtifactPackageIdentity query_with_libalpm_for_test(
    const std::filesystem::path& artifact_path) {
    return query_path_with_libalpm(artifact_path);
}
#endif

} // namespace artifact_archive_metadata
