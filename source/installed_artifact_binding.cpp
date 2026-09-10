#include "installed_artifact_binding.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

void require_sha256_digest(
    std::string_view digest, std::string_view field_name) {
    const bool is_lowercase_hex =
        digest.size() == 64 &&
        std::all_of(digest.begin(), digest.end(), [](char character) {
            return (character >= '0' && character <= '9') ||
                   (character >= 'a' && character <= 'f');
        });
    if(!is_lowercase_hex) {
        throw std::invalid_argument(
            std::string(field_name) +
            " must be a canonical lowercase SHA-256 digest.");
    }
}

} // namespace

AlpmMtreeSha256Digest::AlpmMtreeSha256Digest(
    std::string digest) noexcept
    : digest_(std::move(digest)) {
}

AlpmMtreeSha256Digest AlpmMtreeSha256Digest::make(std::string digest) {
    require_sha256_digest(digest, "ALPM-MTREE digest");
    return AlpmMtreeSha256Digest(std::move(digest));
}

const std::string& AlpmMtreeSha256Digest::value() const noexcept {
    return digest_;
}

InstalledDatabaseRecordSha256Digest::
    InstalledDatabaseRecordSha256Digest(std::string digest) noexcept
    : digest_(std::move(digest)) {
}

InstalledDatabaseRecordSha256Digest
InstalledDatabaseRecordSha256Digest::make(std::string digest) {
    require_sha256_digest(digest, "Installed database record digest");
    return InstalledDatabaseRecordSha256Digest(std::move(digest));
}

const std::string&
InstalledDatabaseRecordSha256Digest::value() const noexcept {
    return digest_;
}

InstalledPackageRecordGeneration::InstalledPackageRecordGeneration(
    InstalledPackageRecordGenerationScheme scheme,
    std::string opaque_identity) noexcept
    : scheme_(scheme), opaque_identity_(std::move(opaque_identity)) {
}

InstalledPackageRecordGenerationScheme
InstalledPackageRecordGeneration::scheme() const noexcept {
    return scheme_;
}

const std::string&
InstalledPackageRecordGeneration::opaque_identity() const noexcept {
    return opaque_identity_;
}

InstalledArtifactBinding::InstalledArtifactBinding(
    PackageChildIdentity package,
    PackageVersionIdentity version,
    InstalledPackageArchitectureIdentity architecture,
    AlpmMtreeSha256Digest mtree_digest,
    InstalledDatabaseRecordSha256Digest database_record_digest,
    InstalledPackageRecordGeneration record_generation) noexcept
    : package_(std::move(package)), version_(std::move(version)),
      architecture_(std::move(architecture)),
      mtree_digest_(std::move(mtree_digest)),
      database_record_digest_(std::move(database_record_digest)),
      record_generation_(std::move(record_generation)) {
}

InstalledArtifactBinding InstalledArtifactBinding::make(
    PackageChildIdentity package,
    PackageVersionIdentity version,
    InstalledPackageArchitectureIdentity architecture,
    AlpmMtreeSha256Digest mtree_digest,
    InstalledDatabaseRecordSha256Digest database_record_digest,
    InstalledPackageRecordGeneration record_generation) {
    if(version.state() != PackageVersionState::Known ||
       version.full_version() == nullptr) {
        throw std::invalid_argument(
            "Installed artifact binding requires a known full version.");
    }
    if(architecture.state() != InstalledPackageMetadataValueState::Known ||
       architecture.value() == nullptr) {
        throw std::invalid_argument(
            "Installed artifact binding requires a known architecture.");
    }
    return InstalledArtifactBinding(
        std::move(package), std::move(version), std::move(architecture),
        std::move(mtree_digest), std::move(database_record_digest),
        std::move(record_generation));
}

const PackageChildIdentity& InstalledArtifactBinding::package()
    const noexcept {
    return package_;
}

const PackageVersionIdentity& InstalledArtifactBinding::version()
    const noexcept {
    return version_;
}

const InstalledPackageArchitectureIdentity&
InstalledArtifactBinding::architecture() const noexcept {
    return architecture_;
}

const AlpmMtreeSha256Digest& InstalledArtifactBinding::mtree_digest()
    const noexcept {
    return mtree_digest_;
}

const InstalledDatabaseRecordSha256Digest&
InstalledArtifactBinding::database_record_digest() const noexcept {
    return database_record_digest_;
}

const InstalledPackageRecordGeneration&
InstalledArtifactBinding::record_generation() const noexcept {
    return record_generation_;
}

InstalledArtifactBindingComparisonResult
compare_installed_artifact_binding(
    const InstalledArtifactBinding& expected,
    const InstalledArtifactBinding& current) noexcept {
    const PackageChildIdentity& expected_package = expected.package();
    const PackageChildIdentity& current_package = current.package();
    if(expected_package.package_name() != current_package.package_name()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::
                PackageIdentityMismatch};
    }

    const PackageBaseIdentity& expected_base =
        expected_package.package_base();
    const PackageBaseIdentity& current_base =
        current_package.package_base();
    if(expected_base.package_base() != current_base.package_base()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::PackageBaseMismatch};
    }
    if(expected_base.source() != current_base.source()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::
                PackageIdentityMismatch};
    }
    const std::string* expected_version = expected.version().full_version();
    const std::string* current_version = current.version().full_version();
    if(expected_version == nullptr || current_version == nullptr ||
       *expected_version != *current_version) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::VersionMismatch};
    }
    if(expected.architecture() != current.architecture()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::ArchitectureMismatch};
    }
    if(expected.mtree_digest() != current.mtree_digest()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::MtreeMismatch};
    }
    if(expected.database_record_digest() !=
       current.database_record_digest()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::
                DatabaseRecordMismatch};
    }
    if(expected.record_generation() != current.record_generation()) {
        return InstalledArtifactBindingMismatch{
            InstalledArtifactBindingMismatchReason::
                RecordGenerationMismatch};
    }
    return InstalledArtifactBindingMatch{};
}

#ifdef MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
InstalledPackageRecordGeneration
make_installed_package_record_generation_fixture_for_test(
    std::string opaque_identity) {
    if(opaque_identity.empty()) {
        throw std::invalid_argument(
            "Installed package record generation identity is empty.");
    }
    return InstalledPackageRecordGeneration(
        InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt,
        std::move(opaque_identity));
}

InstalledArtifactBinding make_installed_artifact_binding_fixture_for_test(
    PackageChildIdentity package,
    PackageVersionIdentity version,
    InstalledPackageArchitectureIdentity architecture,
    AlpmMtreeSha256Digest mtree_digest,
    InstalledDatabaseRecordSha256Digest database_record_digest,
    InstalledPackageRecordGeneration record_generation) {
    return InstalledArtifactBinding::make(
        std::move(package), std::move(version), std::move(architecture),
        std::move(mtree_digest), std::move(database_record_digest),
        std::move(record_generation));
}
#endif
