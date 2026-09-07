#pragma once

#include "devel_build_provenance_decoder_authority.hpp"
#include "evaluated_devel_source_build_authority.hpp"
#include "artifact_identity.hpp"
#include "installed_artifact_binding.hpp"
#include "vcs_source_identity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

class ReviewedSourceStateRecordBinding;

class PackageArchiveSha256Digest final {
public:
    PackageArchiveSha256Digest() = delete;

    [[nodiscard]] static PackageArchiveSha256Digest make(
        std::string digest);

    [[nodiscard]] const std::string& value() const noexcept;

    bool operator==(const PackageArchiveSha256Digest&) const = default;

private:
    explicit PackageArchiveSha256Digest(std::string digest) noexcept;

    std::string digest_;
};

class ReviewedSourceStateDocumentSha256Digest final {
public:
    ReviewedSourceStateDocumentSha256Digest() = delete;

    [[nodiscard]] static ReviewedSourceStateDocumentSha256Digest make(
        std::string digest);

    [[nodiscard]] const std::string& value() const noexcept;

    bool operator==(
        const ReviewedSourceStateDocumentSha256Digest&) const = default;

private:
    explicit ReviewedSourceStateDocumentSha256Digest(
        std::string digest) noexcept;

    std::string digest_;
};

// This generation belongs only to the #411 reviewed-source state lineage. It
// is neither the provenance-store generation nor an installed ALPM record
// generation.
class ReviewedSourceStateRecordGeneration final {
public:
    ReviewedSourceStateRecordGeneration() = delete;

    [[nodiscard]] std::uint64_t value() const noexcept;

    bool operator==(
        const ReviewedSourceStateRecordGeneration&) const = default;

private:
    friend class DevelBuildProvenancePersistentDecoderAccess;
    friend class ReviewedSourceStateRecordBindingAuthority;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
    friend ReviewedSourceStateRecordBinding
    make_reviewed_source_state_record_binding_fixture_for_test(
        PackageBaseIdentity package_base,
        AurRecipeRevision reviewed_recipe_revision,
        std::uint64_t generation,
        ReviewedSourceStateDocumentSha256Digest document_digest);
#endif

    explicit ReviewedSourceStateRecordGeneration(
        std::uint64_t value) noexcept;

    std::uint64_t value_;
};

// Stable persistent binding to the exact #411 authoritative record used by a
// build. Filesystem path, leaf, inode, timestamps, and mode stay in the
// runtime CAS token and are intentionally not business identity here.
class ReviewedSourceStateRecordBinding final {
public:
    ReviewedSourceStateRecordBinding() = delete;
    ReviewedSourceStateRecordBinding(
        const ReviewedSourceStateRecordBinding&) = default;
    ReviewedSourceStateRecordBinding(
        ReviewedSourceStateRecordBinding&&) noexcept = default;
    ReviewedSourceStateRecordBinding& operator=(
        const ReviewedSourceStateRecordBinding&) = default;
    ReviewedSourceStateRecordBinding& operator=(
        ReviewedSourceStateRecordBinding&&) noexcept = default;
    ~ReviewedSourceStateRecordBinding() = default;

    [[nodiscard]] const PackageBaseIdentity& package_base() const noexcept;
    [[nodiscard]] const AurRecipeRevision& reviewed_recipe_revision()
        const noexcept;
    [[nodiscard]] const ReviewedSourceStateRecordGeneration& generation()
        const noexcept;
    [[nodiscard]] const ReviewedSourceStateDocumentSha256Digest&
    document_digest() const noexcept;

    bool operator==(const ReviewedSourceStateRecordBinding&) const = default;

private:
    friend class DevelBuildProvenancePersistentDecoderAccess;
    friend class ReviewedSourceStateRecordBindingAuthority;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
    friend ReviewedSourceStateRecordBinding
    make_reviewed_source_state_record_binding_fixture_for_test(
        PackageBaseIdentity package_base,
        AurRecipeRevision reviewed_recipe_revision,
        std::uint64_t generation,
        ReviewedSourceStateDocumentSha256Digest document_digest);
#endif

    ReviewedSourceStateRecordBinding(
        PackageBaseIdentity package_base,
        AurRecipeRevision reviewed_recipe_revision,
        ReviewedSourceStateRecordGeneration generation,
        ReviewedSourceStateDocumentSha256Digest document_digest) noexcept;

    PackageBaseIdentity package_base_;
    AurRecipeRevision reviewed_recipe_revision_;
    ReviewedSourceStateRecordGeneration generation_;
    ReviewedSourceStateDocumentSha256Digest document_digest_;
};

// Observation of the same makepkg-managed Git workspace at the defined
// post-preparation and post-build boundaries. It does not claim binary
// reproducibility or detect arbitrary temporary PKGBUILD mutations that were
// perfectly restored before the second observation.
class MakepkgManagedGitWorkspaceRevisionObservation final {
public:
    MakepkgManagedGitWorkspaceRevisionObservation() = delete;
    MakepkgManagedGitWorkspaceRevisionObservation(
        const MakepkgManagedGitWorkspaceRevisionObservation&) = default;
    MakepkgManagedGitWorkspaceRevisionObservation(
        MakepkgManagedGitWorkspaceRevisionObservation&&) noexcept = default;
    MakepkgManagedGitWorkspaceRevisionObservation& operator=(
        const MakepkgManagedGitWorkspaceRevisionObservation&) = default;
    MakepkgManagedGitWorkspaceRevisionObservation& operator=(
        MakepkgManagedGitWorkspaceRevisionObservation&&) noexcept = default;
    ~MakepkgManagedGitWorkspaceRevisionObservation() = default;

    [[nodiscard]] const UpstreamGitRevision& prepared_revision()
        const noexcept;
    [[nodiscard]] const UpstreamGitRevision& post_build_revision()
        const noexcept;

    bool operator==(
        const MakepkgManagedGitWorkspaceRevisionObservation&) const =
        default;

private:
    friend class EvaluatedDevelSourceBuildAuthority;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
    friend MakepkgManagedGitWorkspaceRevisionObservation
    make_makepkg_git_workspace_revision_observation_fixture_for_test(
        UpstreamGitRevision prepared_revision,
        UpstreamGitRevision post_build_revision);
#endif

    MakepkgManagedGitWorkspaceRevisionObservation(
        UpstreamGitRevision prepared_revision,
        UpstreamGitRevision post_build_revision) noexcept;

    UpstreamGitRevision prepared_revision_;
    UpstreamGitRevision post_build_revision_;
};

enum class ActualBuiltGitRevisionProofFailureReason {
    SourceIdentityMismatch,
    RevisionMismatch,
};

struct ActualBuiltGitRevisionProofFailure {
    ActualBuiltGitRevisionProofFailureReason reason;

    bool operator==(
        const ActualBuiltGitRevisionProofFailure&) const = default;
};

// Capability for the exact Git revision observed at both bounded workspace
// boundaries. Live raw OIDs, reviewed recipe revisions, cache HEADs, and
// remote observations have no production mint path. Strict persistent decode
// and test fixtures use separate, explicitly scoped construction boundaries.
class ActualBuiltGitRevision final {
public:
    ActualBuiltGitRevision() = delete;
    ActualBuiltGitRevision(const ActualBuiltGitRevision&) = default;
    ActualBuiltGitRevision(ActualBuiltGitRevision&&) noexcept = default;
    ActualBuiltGitRevision& operator=(const ActualBuiltGitRevision&) = default;
    ActualBuiltGitRevision& operator=(
        ActualBuiltGitRevision&&) noexcept = default;
    ~ActualBuiltGitRevision() = default;

    [[nodiscard]] const UpstreamGitRevision& revision() const noexcept;

    bool operator==(const ActualBuiltGitRevision&) const = default;

private:
    friend std::variant<
        ActualBuiltGitRevision,
        ActualBuiltGitRevisionProofFailure>
    prove_actual_built_git_revision(
        MakepkgManagedGitWorkspaceRevisionObservation observation);
    friend class DevelBuildProvenancePersistentDecoderAccess;

    explicit ActualBuiltGitRevision(
        UpstreamGitRevision revision) noexcept;

    UpstreamGitRevision revision_;
};

using ActualBuiltGitRevisionProofResult = std::variant<
    ActualBuiltGitRevision,
    ActualBuiltGitRevisionProofFailure>;

[[nodiscard]] ActualBuiltGitRevisionProofResult
prove_actual_built_git_revision(
    MakepkgManagedGitWorkspaceRevisionObservation observation);

// Archive SHA-256 is historical evidence for the selected bytes. The MTREE
// digest is kept separately because it can be compared with the installed
// local-database MTREE after the archive itself has been cleaned up.
struct BuiltPackageArtifactEvidence {
    ArtifactPackageIdentity identity;
    PackageArchiveSha256Digest archive_digest;
    AlpmMtreeSha256Digest mtree_digest;

    bool operator==(
        const BuiltPackageArtifactEvidence&) const = default;
};

enum class DevelBuildProvenanceFailureReason {
    PackageSourceMismatch,
    ReviewedSourceBindingMismatch,
    UnsupportedEvaluatedSource,
    EvaluatedSourceMismatch,
    PackageIdentityMismatch,
    PackageBaseMismatch,
    MalformedArtifactIdentity,
    VersionMismatch,
    ArchitectureMismatch,
    MtreeMismatch,
};

struct DevelBuildProvenanceFailure {
    DevelBuildProvenanceFailureReason reason;

    bool operator==(const DevelBuildProvenanceFailure&) const = default;
};

class DevelBuildProvenance final {
public:
    DevelBuildProvenance() = delete;
    DevelBuildProvenance(const DevelBuildProvenance&) = default;
    DevelBuildProvenance(DevelBuildProvenance&&) noexcept = default;
    DevelBuildProvenance& operator=(const DevelBuildProvenance&) = default;
    DevelBuildProvenance& operator=(
        DevelBuildProvenance&&) noexcept = default;
    ~DevelBuildProvenance() = default;

    [[nodiscard]] const PackageBaseIdentity& package_base() const noexcept;
    [[nodiscard]] const ReviewedSourceStateRecordBinding&
    reviewed_source_binding() const noexcept;
    [[nodiscard]] const AurRecipeRevision& reviewed_recipe_revision()
        const noexcept;
    [[nodiscard]] const VcsSourceIdentity& evaluated_source() const noexcept;
    [[nodiscard]] const ActualBuiltGitRevision& actual_built_revision()
        const noexcept;
    [[nodiscard]] const BuiltPackageArtifactEvidence& artifact()
        const noexcept;
    [[nodiscard]] const InstalledArtifactBinding& installed_binding()
        const noexcept;

    bool operator==(const DevelBuildProvenance&) const = default;

private:
    friend std::variant<
        DevelBuildProvenance,
        DevelBuildProvenanceFailure>
    make_devel_build_provenance(
        PackageBaseIdentity package_base,
        ReviewedSourceStateRecordBinding reviewed_source_binding,
        VcsSourceIdentity evaluated_source,
        ActualBuiltGitRevision actual_built_revision,
        BuiltPackageArtifactEvidence artifact,
        InstalledArtifactBinding installed_binding);

    DevelBuildProvenance(
        PackageBaseIdentity package_base,
        ReviewedSourceStateRecordBinding reviewed_source_binding,
        VcsSourceIdentity evaluated_source,
        ActualBuiltGitRevision actual_built_revision,
        BuiltPackageArtifactEvidence artifact,
        InstalledArtifactBinding installed_binding) noexcept;

    PackageBaseIdentity package_base_;
    ReviewedSourceStateRecordBinding reviewed_source_binding_;
    VcsSourceIdentity evaluated_source_;
    ActualBuiltGitRevision actual_built_revision_;
    BuiltPackageArtifactEvidence artifact_;
    InstalledArtifactBinding installed_binding_;
};

using DevelBuildProvenanceResult = std::variant<
    DevelBuildProvenance,
    DevelBuildProvenanceFailure>;

[[nodiscard]] DevelBuildProvenanceResult make_devel_build_provenance(
    PackageBaseIdentity package_base,
    ReviewedSourceStateRecordBinding reviewed_source_binding,
    VcsSourceIdentity evaluated_source,
    ActualBuiltGitRevision actual_built_revision,
    BuiltPackageArtifactEvidence artifact,
    InstalledArtifactBinding installed_binding);

#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
[[nodiscard]] ReviewedSourceStateRecordBinding
make_reviewed_source_state_record_binding_fixture_for_test(
    PackageBaseIdentity package_base,
    AurRecipeRevision reviewed_recipe_revision,
    std::uint64_t generation,
    ReviewedSourceStateDocumentSha256Digest document_digest);

// Test-only workspace observation mint. Slice 4 has a separate closed
// production friend; ordinary callers still cannot supply raw observations.
[[nodiscard]] MakepkgManagedGitWorkspaceRevisionObservation
make_makepkg_git_workspace_revision_observation_fixture_for_test(
    UpstreamGitRevision prepared_revision,
    UpstreamGitRevision post_build_revision);
#endif
