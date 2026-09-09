#pragma once

#include "devel_build_provenance_decoder_authority.hpp"
#include "current_installed_artifact_binding_observer_authority.hpp"
#include "installed_artifact_binding_observer_authority.hpp"
#include "installed_package.hpp"
#include "source_package_identity.hpp"

#include <string>
#include <utility>
#include <variant>

class AlpmMtreeSha256Digest final {
public:
    AlpmMtreeSha256Digest() = delete;

    [[nodiscard]] static AlpmMtreeSha256Digest make(std::string digest);

    [[nodiscard]] const std::string& value() const noexcept;

    bool operator==(const AlpmMtreeSha256Digest&) const = default;

private:
    explicit AlpmMtreeSha256Digest(std::string digest) noexcept;

    std::string digest_;
};

// Digest of the exact local database desc/files byte projection. It can
// detect record-content drift, but it cannot prove that an identical record
// was reinstalled.
class InstalledDatabaseRecordSha256Digest final {
public:
    InstalledDatabaseRecordSha256Digest() = delete;

    [[nodiscard]] static InstalledDatabaseRecordSha256Digest make(
        std::string digest);

    [[nodiscard]] const std::string& value() const noexcept;

    bool operator==(
        const InstalledDatabaseRecordSha256Digest&) const = default;

private:
    explicit InstalledDatabaseRecordSha256Digest(
        std::string digest) noexcept;

    std::string digest_;
};

enum class InstalledPackageRecordGenerationScheme {
    LinuxNameToHandleAt,
};

// Opaque identity of one installed local-database record generation. The
// Linux producer must scope the filesystem identity together with the opaque
// file handle. Consumers must not parse it as an inode or timestamp.
class InstalledPackageRecordGeneration final {
public:
    InstalledPackageRecordGeneration() = delete;
    InstalledPackageRecordGeneration(
        const InstalledPackageRecordGeneration&) = default;
    InstalledPackageRecordGeneration(
        InstalledPackageRecordGeneration&&) noexcept = default;
    InstalledPackageRecordGeneration& operator=(
        const InstalledPackageRecordGeneration&) = default;
    InstalledPackageRecordGeneration& operator=(
        InstalledPackageRecordGeneration&&) noexcept = default;
    ~InstalledPackageRecordGeneration() = default;

    [[nodiscard]] InstalledPackageRecordGenerationScheme scheme()
        const noexcept;
    [[nodiscard]] const std::string& opaque_identity() const noexcept;

    bool operator==(
        const InstalledPackageRecordGeneration&) const = default;

private:
    friend class DevelBuildProvenancePersistentDecoderAccess;
    friend class InstalledArtifactBindingObserver;
    friend class CurrentInstalledArtifactBindingObserver;
#ifdef MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
    friend InstalledPackageRecordGeneration
    make_installed_package_record_generation_fixture_for_test(
        std::string opaque_identity);
#endif

    InstalledPackageRecordGeneration(
        InstalledPackageRecordGenerationScheme scheme,
        std::string opaque_identity) noexcept;

    InstalledPackageRecordGenerationScheme scheme_;
    std::string opaque_identity_;
};

// The PackageSourceIdentity in package() is correlation context established
// against the selected artifact/provenance. A pacman local DB record proves
// its child name and PackageBase text, not that source authority by itself.
class InstalledArtifactBinding final {
public:
    InstalledArtifactBinding() = delete;
    InstalledArtifactBinding(const InstalledArtifactBinding&) = default;
    InstalledArtifactBinding(InstalledArtifactBinding&&) noexcept = default;
    InstalledArtifactBinding& operator=(
        const InstalledArtifactBinding&) = default;
    InstalledArtifactBinding& operator=(
        InstalledArtifactBinding&&) noexcept = default;
    ~InstalledArtifactBinding() = default;

    [[nodiscard]] const PackageChildIdentity& package() const noexcept;
    [[nodiscard]] const PackageVersionIdentity& version() const noexcept;
    [[nodiscard]] const InstalledPackageArchitectureIdentity& architecture()
        const noexcept;
    [[nodiscard]] const AlpmMtreeSha256Digest& mtree_digest() const noexcept;
    [[nodiscard]] const InstalledDatabaseRecordSha256Digest&
    database_record_digest() const noexcept;
    [[nodiscard]] const InstalledPackageRecordGeneration& record_generation()
        const noexcept;

    bool operator==(const InstalledArtifactBinding&) const = default;

private:
    friend class DevelBuildProvenancePersistentDecoderAccess;
    friend class InstalledArtifactBindingObserver;
    friend class CurrentInstalledArtifactBindingObserver;
#ifdef MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
    friend InstalledArtifactBinding
    make_installed_artifact_binding_fixture_for_test(
        PackageChildIdentity package,
        PackageVersionIdentity version,
        InstalledPackageArchitectureIdentity architecture,
        AlpmMtreeSha256Digest mtree_digest,
        InstalledDatabaseRecordSha256Digest database_record_digest,
        InstalledPackageRecordGeneration record_generation);
#endif

    [[nodiscard]] static InstalledArtifactBinding make(
        PackageChildIdentity package,
        PackageVersionIdentity version,
        InstalledPackageArchitectureIdentity architecture,
        AlpmMtreeSha256Digest mtree_digest,
        InstalledDatabaseRecordSha256Digest database_record_digest,
        InstalledPackageRecordGeneration record_generation);

    InstalledArtifactBinding(
        PackageChildIdentity package,
        PackageVersionIdentity version,
        InstalledPackageArchitectureIdentity architecture,
        AlpmMtreeSha256Digest mtree_digest,
        InstalledDatabaseRecordSha256Digest database_record_digest,
        InstalledPackageRecordGeneration record_generation) noexcept;

    PackageChildIdentity package_;
    PackageVersionIdentity version_;
    InstalledPackageArchitectureIdentity architecture_;
    AlpmMtreeSha256Digest mtree_digest_;
    InstalledDatabaseRecordSha256Digest database_record_digest_;
    InstalledPackageRecordGeneration record_generation_;
};

enum class InstalledArtifactBindingMismatchReason {
    PackageIdentityMismatch,
    PackageBaseMismatch,
    VersionMismatch,
    ArchitectureMismatch,
    MtreeMismatch,
    DatabaseRecordMismatch,
    RecordGenerationMismatch,
};

struct InstalledArtifactBindingMatch {
    bool operator==(const InstalledArtifactBindingMatch&) const = default;
};

struct InstalledArtifactBindingMismatch {
    InstalledArtifactBindingMismatchReason reason;

    bool operator==(const InstalledArtifactBindingMismatch&) const = default;
};

using InstalledArtifactBindingComparisonResult = std::variant<
    InstalledArtifactBindingMatch,
    InstalledArtifactBindingMismatch>;

[[nodiscard]] InstalledArtifactBindingComparisonResult
compare_installed_artifact_binding(
    const InstalledArtifactBinding& expected,
    const InstalledArtifactBinding& current) noexcept;

enum class InstalledArtifactBindingObservationFailureReason {
    MalformedBinding,
    UnsupportedGeneration,
};

struct InstalledArtifactBindingObservationFailure {
    InstalledArtifactBindingObservationFailureReason reason;

    bool operator==(
        const InstalledArtifactBindingObservationFailure&) const = default;
};

using InstalledArtifactBindingObservationResult = std::variant<
    InstalledArtifactBinding,
    InstalledArtifactBindingObservationFailure>;

#ifdef MOGUET_ENABLE_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
// Test-only mint. Production uses the separate S5 transaction-bound or
// S7-A current installed-record observation owner.
[[nodiscard]] InstalledPackageRecordGeneration
make_installed_package_record_generation_fixture_for_test(
    std::string opaque_identity);

// Whole-binding raw construction is test-only. Persistent decode has a
// separate friend boundary; current observation has its own complete private
// producer rather than reopening InstalledArtifactBinding::make().
[[nodiscard]] InstalledArtifactBinding
make_installed_artifact_binding_fixture_for_test(
    PackageChildIdentity package,
    PackageVersionIdentity version,
    InstalledPackageArchitectureIdentity architecture,
    AlpmMtreeSha256Digest mtree_digest,
    InstalledDatabaseRecordSha256Digest database_record_digest,
    InstalledPackageRecordGeneration record_generation);
#endif
