#include "devel_build_provenance.hpp"

#include "package_identifier.hpp"

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

DevelBuildProvenanceResult provenance_failure(
    DevelBuildProvenanceFailureReason reason) {
    return DevelBuildProvenanceFailure{reason};
}

bool is_nonempty_metadata_token(std::string_view value) noexcept {
    return !value.empty() &&
           std::none_of(value.begin(), value.end(), [](char character) {
               const unsigned char byte =
                   static_cast<unsigned char>(character);
               return byte <= 0x20 || byte == 0x7f;
           });
}

} // namespace

PackageArchiveSha256Digest::PackageArchiveSha256Digest(
    std::string digest) noexcept
    : digest_(std::move(digest)) {
}

PackageArchiveSha256Digest PackageArchiveSha256Digest::make(
    std::string digest) {
    require_sha256_digest(digest, "Package archive digest");
    return PackageArchiveSha256Digest(std::move(digest));
}

const std::string& PackageArchiveSha256Digest::value() const noexcept {
    return digest_;
}

ReviewedSourceStateDocumentSha256Digest::
    ReviewedSourceStateDocumentSha256Digest(std::string digest) noexcept
    : digest_(std::move(digest)) {
}

ReviewedSourceStateDocumentSha256Digest
ReviewedSourceStateDocumentSha256Digest::make(std::string digest) {
    require_sha256_digest(digest, "Reviewed source state document digest");
    return ReviewedSourceStateDocumentSha256Digest(std::move(digest));
}

const std::string& ReviewedSourceStateDocumentSha256Digest::value()
    const noexcept {
    return digest_;
}

ReviewedSourceStateRecordGeneration::
    ReviewedSourceStateRecordGeneration(std::uint64_t value) noexcept
    : value_(value) {
}

std::uint64_t ReviewedSourceStateRecordGeneration::value() const noexcept {
    return value_;
}

ReviewedSourceStateRecordBinding::ReviewedSourceStateRecordBinding(
    PackageBaseIdentity package_base,
    AurRecipeRevision reviewed_recipe_revision,
    ReviewedSourceStateRecordGeneration generation,
    ReviewedSourceStateDocumentSha256Digest document_digest) noexcept
    : package_base_(std::move(package_base)),
      reviewed_recipe_revision_(std::move(reviewed_recipe_revision)),
      generation_(generation), document_digest_(std::move(document_digest)) {
}

const PackageBaseIdentity& ReviewedSourceStateRecordBinding::package_base()
    const noexcept {
    return package_base_;
}

const AurRecipeRevision&
ReviewedSourceStateRecordBinding::reviewed_recipe_revision() const noexcept {
    return reviewed_recipe_revision_;
}

const ReviewedSourceStateRecordGeneration&
ReviewedSourceStateRecordBinding::generation() const noexcept {
    return generation_;
}

const ReviewedSourceStateDocumentSha256Digest&
ReviewedSourceStateRecordBinding::document_digest() const noexcept {
    return document_digest_;
}

MakepkgManagedGitWorkspaceRevisionObservation::
    MakepkgManagedGitWorkspaceRevisionObservation(
        UpstreamGitRevision prepared_revision,
        UpstreamGitRevision post_build_revision) noexcept
    : prepared_revision_(std::move(prepared_revision)),
      post_build_revision_(std::move(post_build_revision)) {
}

const UpstreamGitRevision&
MakepkgManagedGitWorkspaceRevisionObservation::prepared_revision()
    const noexcept {
    return prepared_revision_;
}

const UpstreamGitRevision&
MakepkgManagedGitWorkspaceRevisionObservation::post_build_revision()
    const noexcept {
    return post_build_revision_;
}

ActualBuiltGitRevision::ActualBuiltGitRevision(
    UpstreamGitRevision revision) noexcept
    : revision_(std::move(revision)) {
}

const UpstreamGitRevision& ActualBuiltGitRevision::revision()
    const noexcept {
    return revision_;
}

ActualBuiltGitRevisionProofResult prove_actual_built_git_revision(
    MakepkgManagedGitWorkspaceRevisionObservation observation) {
    if(observation.prepared_revision().source() !=
       observation.post_build_revision().source()) {
        return ActualBuiltGitRevisionProofFailure{
            ActualBuiltGitRevisionProofFailureReason::
                SourceIdentityMismatch};
    }
    if(observation.prepared_revision().value() !=
       observation.post_build_revision().value()) {
        return ActualBuiltGitRevisionProofFailure{
            ActualBuiltGitRevisionProofFailureReason::RevisionMismatch};
    }
    return ActualBuiltGitRevision(observation.post_build_revision());
}

DevelBuildProvenance::DevelBuildProvenance(
    PackageBaseIdentity package_base,
    ReviewedSourceStateRecordBinding reviewed_source_binding,
    VcsSourceIdentity evaluated_source,
    ActualBuiltGitRevision actual_built_revision,
    BuiltPackageArtifactEvidence artifact,
    InstalledArtifactBinding installed_binding) noexcept
    : package_base_(std::move(package_base)),
      reviewed_source_binding_(std::move(reviewed_source_binding)),
      evaluated_source_(std::move(evaluated_source)),
      actual_built_revision_(std::move(actual_built_revision)),
      artifact_(std::move(artifact)),
      installed_binding_(std::move(installed_binding)) {
}

const PackageBaseIdentity& DevelBuildProvenance::package_base()
    const noexcept {
    return package_base_;
}

const ReviewedSourceStateRecordBinding&
DevelBuildProvenance::reviewed_source_binding() const noexcept {
    return reviewed_source_binding_;
}

const AurRecipeRevision&
DevelBuildProvenance::reviewed_recipe_revision() const noexcept {
    return reviewed_source_binding_.reviewed_recipe_revision();
}

const VcsSourceIdentity& DevelBuildProvenance::evaluated_source()
    const noexcept {
    return evaluated_source_;
}

const ActualBuiltGitRevision&
DevelBuildProvenance::actual_built_revision() const noexcept {
    return actual_built_revision_;
}

const BuiltPackageArtifactEvidence& DevelBuildProvenance::artifact()
    const noexcept {
    return artifact_;
}

const InstalledArtifactBinding&
DevelBuildProvenance::installed_binding() const noexcept {
    return installed_binding_;
}

DevelBuildProvenanceResult make_devel_build_provenance(
    PackageBaseIdentity package_base,
    ReviewedSourceStateRecordBinding reviewed_source_binding,
    VcsSourceIdentity evaluated_source,
    ActualBuiltGitRevision actual_built_revision,
    BuiltPackageArtifactEvidence artifact,
    InstalledArtifactBinding installed_binding) {
    const PackageSourceIdentity& package_source = package_base.source();
    if(package_source.kind() != PackageSourceKind::Aur ||
       package_source.location().kind() != SourceLocationKind::GitRemote ||
       package_source.location().state() != SourceLocationState::Known) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::PackageSourceMismatch);
    }

    if(reviewed_source_binding.package_base() != package_base) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::
                ReviewedSourceBindingMismatch);
    }

    const VcsSelectorKind selector_kind = evaluated_source.selector().kind();
    if(evaluated_source.kind() != VcsKind::Git ||
       !std::string_view(evaluated_source.source_location())
            .starts_with("https://") ||
       evaluated_source.source_location().size() <=
           std::string_view("https://").size() ||
       (selector_kind != VcsSelectorKind::DefaultHead &&
        selector_kind != VcsSelectorKind::Branch) ||
       evaluated_source.selector().tracking_behavior() !=
           VcsSelectorTrackingBehavior::Floating ||
       evaluated_source.architecture() != nullptr) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::
                UnsupportedEvaluatedSource);
    }

    if(evaluated_source != actual_built_revision.revision().source()) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::EvaluatedSourceMismatch);
    }

    const std::string* artifact_package_base =
        artifact.identity.package_base.value();
    const std::string* artifact_architecture =
        artifact.identity.architecture.value();
    if(artifact.identity.package_base.state() !=
           ArtifactMetadataValueState::Known ||
       artifact_package_base == nullptr ||
       artifact.identity.architecture.state() !=
           ArtifactMetadataValueState::Known ||
       artifact_architecture == nullptr ||
       !is_valid_package_name(artifact.identity.package_name) ||
       !is_valid_package_name(*artifact_package_base) ||
       !is_nonempty_metadata_token(artifact.identity.full_version) ||
       !is_nonempty_metadata_token(*artifact_architecture)) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::
                MalformedArtifactIdentity);
    }

    const PackageChildIdentity& installed_package =
        installed_binding.package();
    if(installed_package.package_name() != artifact.identity.package_name ||
       installed_package.package_base().source() != package_source) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::PackageIdentityMismatch);
    }
    if(installed_package.package_base().package_base() !=
       package_base.package_base()) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::PackageBaseMismatch);
    }
    if(*artifact_package_base != package_base.package_base()) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::PackageBaseMismatch);
    }

    const std::string* installed_version =
        installed_binding.version().full_version();
    if(installed_version == nullptr ||
       artifact.identity.full_version != *installed_version) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::VersionMismatch);
    }

    const std::string* installed_architecture =
        installed_binding.architecture().value();
    if(installed_architecture == nullptr ||
       *artifact_architecture != *installed_architecture) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::ArchitectureMismatch);
    }
    if(artifact.mtree_digest != installed_binding.mtree_digest()) {
        return provenance_failure(
            DevelBuildProvenanceFailureReason::MtreeMismatch);
    }

    return DevelBuildProvenance(
        std::move(package_base), std::move(reviewed_source_binding),
        std::move(evaluated_source),
        std::move(actual_built_revision), std::move(artifact),
        std::move(installed_binding));
}

#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_TEST_HOOKS
ReviewedSourceStateRecordBinding
make_reviewed_source_state_record_binding_fixture_for_test(
    PackageBaseIdentity package_base,
    AurRecipeRevision reviewed_recipe_revision,
    std::uint64_t generation,
    ReviewedSourceStateDocumentSha256Digest document_digest) {
    if(generation == 0) {
        throw std::invalid_argument(
            "Reviewed source state record generation must be positive.");
    }
    return ReviewedSourceStateRecordBinding(
        std::move(package_base), std::move(reviewed_recipe_revision),
        ReviewedSourceStateRecordGeneration(generation),
        std::move(document_digest));
}

MakepkgManagedGitWorkspaceRevisionObservation
make_makepkg_git_workspace_revision_observation_fixture_for_test(
    UpstreamGitRevision prepared_revision,
    UpstreamGitRevision post_build_revision) {
    return MakepkgManagedGitWorkspaceRevisionObservation(
        std::move(prepared_revision), std::move(post_build_revision));
}
#endif
