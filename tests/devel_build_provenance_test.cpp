#include "devel_build_provenance.hpp"

#include "git_remote_revision_observer.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

constexpr const char* RECIPE_OID =
    "1111111111111111111111111111111111111111";
constexpr const char* BUILT_OID =
    "2222222222222222222222222222222222222222";
constexpr const char* OTHER_OID =
    "3333333333333333333333333333333333333333";
constexpr const char* ARCHIVE_DIGEST =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* OTHER_ARCHIVE_DIGEST =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* MTREE_DIGEST =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr const char* OTHER_MTREE_DIGEST =
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr const char* DATABASE_DIGEST =
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr const char* REVIEWED_DOCUMENT_DIGEST =
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
constexpr const char* AUR_REMOTE =
    "https://aur.archlinux.org/moguet-provenance-fixture.git";
constexpr const char* UPSTREAM_REMOTE =
    "https://example.invalid/moguet-provenance-upstream.git";

static_assert(!std::is_default_constructible_v<ActualBuiltGitRevision>);
static_assert(!std::is_constructible_v<
              ActualBuiltGitRevision,
              std::string>);
static_assert(!std::is_constructible_v<
              ActualBuiltGitRevision,
              SourceRevisionIdentity>);
static_assert(!std::is_constructible_v<
              ActualBuiltGitRevision,
              UpstreamGitRevision>);
static_assert(!std::is_constructible_v<
              ActualBuiltGitRevision,
              AurRecipeRevision>);
static_assert(!std::is_constructible_v<
              ActualBuiltGitRevision,
              ObservedGitRemoteRevision>);
static_assert(!std::is_convertible_v<
              ObservedGitRemoteRevision,
              ActualBuiltGitRevision>);
static_assert(!std::is_default_constructible_v<
              MakepkgManagedGitWorkspaceRevisionObservation>);
static_assert(!std::is_constructible_v<
              MakepkgManagedGitWorkspaceRevisionObservation,
              UpstreamGitRevision,
              UpstreamGitRevision>);
static_assert(std::variant_size_v<ActualBuiltGitRevisionProofResult> == 2);
static_assert(std::variant_size_v<DevelBuildProvenanceResult> == 2);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceStateRecordBinding>);
static_assert(!std::is_constructible_v<
              ReviewedSourceStateRecordBinding,
              PackageBaseIdentity,
              AurRecipeRevision,
              std::uint64_t,
              ReviewedSourceStateDocumentSha256Digest>);

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

VcsSourceIdentity upstream_source(
    std::string remote = UPSTREAM_REMOTE) {
    return VcsSourceIdentity::make(
        VcsKind::Git, std::move(remote), VcsSelector::default_head());
}

PackageBaseIdentity aur_package_base(
    std::string package_base = "moguet-provenance-fixture",
    std::string remote = AUR_REMOTE) {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                std::move(remote))),
        std::move(package_base));
}

ActualBuiltGitRevision actual_revision(
    const VcsSourceIdentity& source,
    std::string object_id = BUILT_OID) {
    const std::string prepared_object_id = object_id;
    const std::string post_build_object_id = object_id;
    ActualBuiltGitRevisionProofResult result =
        prove_actual_built_git_revision(
            make_makepkg_git_workspace_revision_observation_fixture_for_test(
                UpstreamGitRevision::git_commit(
                    source, prepared_object_id),
                UpstreamGitRevision::git_commit(
                    source, post_build_object_id)));
    auto* actual = std::get_if<ActualBuiltGitRevision>(&result);
    if(actual == nullptr) {
        throw std::runtime_error(
            "Matching makepkg workspace proof did not mint actual revision.");
    }
    return *actual;
}

InstalledArtifactBinding installed_binding(
    PackageBaseIdentity package_base = aur_package_base(),
    std::string package_name = "moguet-provenance-fixture",
    std::string version = "1.r2.g222222222222-1",
    std::string architecture = "any",
    std::string mtree_digest = MTREE_DIGEST) {
    return make_installed_artifact_binding_fixture_for_test(
        PackageChildIdentity::make(
            std::move(package_base), std::move(package_name)),
        PackageVersionIdentity::composite(std::move(version)),
        InstalledPackageArchitectureIdentity::known(
            std::move(architecture)),
        AlpmMtreeSha256Digest::make(std::move(mtree_digest)),
        InstalledDatabaseRecordSha256Digest::make(DATABASE_DIGEST),
        make_installed_package_record_generation_fixture_for_test(
            "ext4:opaque-record-generation"));
}

ReviewedSourceStateRecordBinding reviewed_binding(
    PackageBaseIdentity package_base,
    std::uint64_t generation = 7) {
    return make_reviewed_source_state_record_binding_fixture_for_test(
        std::move(package_base), AurRecipeRevision::git_commit(RECIPE_OID),
        generation,
        ReviewedSourceStateDocumentSha256Digest::make(
            REVIEWED_DOCUMENT_DIGEST));
}

BuiltPackageArtifactEvidence artifact(
    std::string package_name = "moguet-provenance-fixture",
    std::string package_base = "moguet-provenance-fixture",
    std::string version = "1.r2.g222222222222-1",
    std::string architecture = "any",
    std::string mtree_digest = MTREE_DIGEST,
    std::string archive_digest = ARCHIVE_DIGEST) {
    return BuiltPackageArtifactEvidence{
        ArtifactPackageIdentity{
            std::move(package_name), std::move(version),
            ArtifactPackageBaseIdentity::known(
                std::move(package_base)),
            ArtifactPackageArchitectureIdentity::known(
                std::move(architecture))},
        PackageArchiveSha256Digest::make(std::move(archive_digest)),
        AlpmMtreeSha256Digest::make(std::move(mtree_digest))};
}

DevelBuildProvenanceResult make_valid_provenance(
    PackageBaseIdentity package_base,
    VcsSourceIdentity evaluated_source,
    ActualBuiltGitRevision built_revision,
    BuiltPackageArtifactEvidence built_artifact,
    InstalledArtifactBinding current_binding) {
    ReviewedSourceStateRecordBinding exact_reviewed =
        reviewed_binding(package_base);
    return make_devel_build_provenance(
        std::move(package_base), std::move(exact_reviewed),
        std::move(evaluated_source), std::move(built_revision),
        std::move(built_artifact), std::move(current_binding));
}

const DevelBuildProvenance& require_provenance(
    const DevelBuildProvenanceResult& result,
    std::string_view message) {
    const auto* provenance = std::get_if<DevelBuildProvenance>(&result);
    if(provenance == nullptr) {
        throw std::runtime_error(std::string(message));
    }
    return *provenance;
}

void expect_failure(
    const DevelBuildProvenanceResult& result,
    DevelBuildProvenanceFailureReason expected,
    const std::string& message) {
    const auto* failure = std::get_if<DevelBuildProvenanceFailure>(&result);
    require(failure != nullptr && failure->reason == expected, message);
}

void test_actual_revision_firewall_and_bounded_proof() {
    const VcsSourceIdentity source = upstream_source();
    const ActualBuiltGitRevision actual = actual_revision(source);
    require(
        actual.revision().source() == source &&
            actual.revision().value().git_commit() != nullptr &&
            *actual.revision().value().git_commit() == BUILT_OID,
        "Actual built revision lost its workspace-bound source/OID.");

    const VcsSourceIdentity other_source = upstream_source(
        "https://example.invalid/other.git");
    const ActualBuiltGitRevisionProofResult source_mismatch =
        prove_actual_built_git_revision(
            make_makepkg_git_workspace_revision_observation_fixture_for_test(
                UpstreamGitRevision::git_commit(source, BUILT_OID),
                UpstreamGitRevision::git_commit(
                    other_source, BUILT_OID)));
    const auto* source_failure =
        std::get_if<ActualBuiltGitRevisionProofFailure>(&source_mismatch);
    require(
        source_failure != nullptr &&
            source_failure->reason ==
                ActualBuiltGitRevisionProofFailureReason::
                    SourceIdentityMismatch,
        "Prepared/post-build source mismatch was not typed.");

    const ActualBuiltGitRevisionProofResult revision_mismatch =
        prove_actual_built_git_revision(
            make_makepkg_git_workspace_revision_observation_fixture_for_test(
                UpstreamGitRevision::git_commit(source, BUILT_OID),
                UpstreamGitRevision::git_commit(source, OTHER_OID)));
    const auto* revision_failure =
        std::get_if<ActualBuiltGitRevisionProofFailure>(&revision_mismatch);
    require(
        revision_failure != nullptr &&
            revision_failure->reason ==
                ActualBuiltGitRevisionProofFailureReason::RevisionMismatch,
        "Prepared/post-build revision mismatch was not typed.");
}

void test_provenance_retains_separate_authorities() {
    const PackageBaseIdentity package_base = aur_package_base();
    const VcsSourceIdentity evaluated = upstream_source();
    const DevelBuildProvenanceResult result = make_valid_provenance(
        package_base, evaluated, actual_revision(evaluated), artifact(),
        installed_binding(package_base));
    const DevelBuildProvenance& provenance = require_provenance(
        result, "Valid pure provenance model was rejected.");

    require(
        provenance.package_base() == package_base &&
            provenance.reviewed_source_binding().generation().value() == 7 &&
            provenance.reviewed_source_binding()
                    .document_digest()
                    .value() == REVIEWED_DOCUMENT_DIGEST &&
            provenance.reviewed_recipe_revision().value().git_commit() !=
                nullptr &&
            *provenance.reviewed_recipe_revision()
                    .value()
                    .git_commit() == RECIPE_OID,
        "Reviewed recipe binding was flattened.");
    require(
        provenance.evaluated_source() == evaluated &&
            provenance.actual_built_revision()
                    .revision()
                    .value()
                    .git_commit() != nullptr &&
            *provenance.actual_built_revision()
                    .revision()
                    .value()
                    .git_commit() == BUILT_OID,
        "Evaluated source and actual built revision were flattened.");
    require(
        provenance.artifact().archive_digest.value() == ARCHIVE_DIGEST &&
            provenance.artifact().mtree_digest.value() == MTREE_DIGEST &&
            provenance.installed_binding().record_generation().opaque_identity() ==
                "ext4:opaque-record-generation",
        "Artifact history and installed binding were flattened.");

    // Archive bytes can differ while installed MTREE and binding remain the
    // same. The archive digest is retained as history, not used as the later
    // installed-record key.
    const DevelBuildProvenanceResult alternate_archive =
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            artifact(
                "moguet-provenance-fixture",
                "moguet-provenance-fixture",
                "1.r2.g222222222222-1", "any", MTREE_DIGEST,
                OTHER_ARCHIVE_DIGEST),
            installed_binding(package_base));
    require(
        require_provenance(
            alternate_archive,
            "Historical archive digest was treated as installed key.")
                .artifact()
                .archive_digest.value() == OTHER_ARCHIVE_DIGEST,
        "Alternate archive history was not retained.");
}

void test_provenance_mismatch_taxonomy() {
    const PackageBaseIdentity package_base = aur_package_base();
    const VcsSourceIdentity evaluated = upstream_source();

    expect_failure(
        make_devel_build_provenance(
            package_base,
            reviewed_binding(aur_package_base("different-base")),
            evaluated, actual_revision(evaluated), artifact(),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::ReviewedSourceBindingMismatch,
        "Different #411 record binding was accepted.");

    const VcsSourceIdentity fixed_source = VcsSourceIdentity::make(
        VcsKind::Git, UPSTREAM_REMOTE,
        VcsSelector::fixed_revision(BUILT_OID));
    expect_failure(
        make_devel_build_provenance(
            package_base, reviewed_binding(package_base), fixed_source,
            actual_revision(fixed_source), artifact(),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::UnsupportedEvaluatedSource,
        "Fixed source selector entered the initial provenance subset.");

    expect_failure(
        make_valid_provenance(
            PackageBaseIdentity::make(
                PackageSourceIdentity::local(
                    SourceLocationIdentity::known_local_path(
                        "/tmp/fixture")),
                "moguet-provenance-fixture"),
            evaluated, actual_revision(evaluated), artifact(),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::PackageSourceMismatch,
        "Non-AUR reviewed recipe binding was accepted.");

    const VcsSourceIdentity other_source = upstream_source(
        "https://example.invalid/other.git");
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(other_source),
            artifact(), installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::EvaluatedSourceMismatch,
        "Remote/evaluated source was flattened into actual revision.");
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            artifact("different-child"),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::PackageIdentityMismatch,
        "Artifact child mismatch was not typed.");
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            artifact(
                "moguet-provenance-fixture", "different-base"),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::PackageBaseMismatch,
        "Artifact PackageBase mismatch was not typed.");

    BuiltPackageArtifactEvidence missing_base = artifact();
    missing_base.identity.package_base =
        ArtifactPackageBaseIdentity::missing();
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            std::move(missing_base), installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::MalformedArtifactIdentity,
        "Missing archive PackageBase was reconstructed.");
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            artifact(
                "moguet-provenance-fixture",
                "moguet-provenance-fixture", "9-1"),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::VersionMismatch,
        "Artifact version mismatch was not typed.");
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            artifact(
                "moguet-provenance-fixture",
                "moguet-provenance-fixture",
                "1.r2.g222222222222-1", "x86_64"),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::ArchitectureMismatch,
        "Artifact architecture mismatch was not typed.");
    expect_failure(
        make_valid_provenance(
            package_base, evaluated, actual_revision(evaluated),
            artifact(
                "moguet-provenance-fixture",
                "moguet-provenance-fixture",
                "1.r2.g222222222222-1", "any",
                OTHER_MTREE_DIGEST),
            installed_binding(package_base)),
        DevelBuildProvenanceFailureReason::MtreeMismatch,
        "Archive/installed MTREE mismatch was not typed.");
}

} // namespace

int main() {
    try {
        test_actual_revision_firewall_and_bounded_proof();
        test_provenance_retains_separate_authorities();
        test_provenance_mismatch_taxonomy();
        std::cout << "devel build provenance tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
