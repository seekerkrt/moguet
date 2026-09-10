#include "artifact_workspace.hpp"
#include "devel_build_provenance_codec.hpp"
#include "devel_build_provenance_reviewed_binding.hpp"
#include "evaluated_devel_source_build.hpp"
#include "invocation_owned_source_build_context.hpp"
#include "reviewed_source_pinned_build.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

static_assert(std::is_invocable_v<
              decltype(plan_reviewed_source_lifecycle),
              AurReviewedSourceReviewIdentity>);
static_assert(!std::is_invocable_v<
              decltype(plan_reviewed_source_lifecycle),
              AurReviewedSourceReviewIdentity,
              ReviewedSourceStateStoreReadResult>);
static_assert(!std::is_default_constructible_v<
              ProductionArtifactSourceTree>);
static_assert(!std::is_copy_constructible_v<
              ProductionArtifactSourceTree>);
static_assert(!std::is_constructible_v<
              ProductionArtifactSourceTree,
              ValidatedCachePath>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceFatalStatePreflight>);
static_assert(!std::is_constructible_v<
              ReviewedSourceFatalStatePreflight,
              PackageBaseIdentity,
              ReviewedSourceStateStoreRead>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceEditorBoundary>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceEditorBoundary>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceEditorOverlayProof>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceEditorOverlayProof>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              ReviewedSourceEditorOverlayStatus>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              bool>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              ValidatedCachePath>);
static_assert(!std::is_constructible_v<
              ReviewedSourceEditorOverlayProof,
              std::filesystem::path>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceStateRecordBinding>);
static_assert(!std::is_constructible_v<
              ReviewedSourceStateRecordGeneration,
              std::uint64_t>);
static_assert(!std::is_default_constructible_v<
              DevelBuildProvenancePersistentDecoderAccess>);
static_assert(!std::is_default_constructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(!std::is_copy_constructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(std::is_move_constructible_v<
              InvocationOwnedSourceBuildContext>);
static_assert(!std::is_constructible_v<
              InvocationOwnedSourceBuildContext,
              PackageBaseIdentity,
              AurRecipeRevision,
              std::filesystem::path,
              std::filesystem::path,
              std::filesystem::path,
              std::filesystem::path>);
static_assert(std::is_invocable_v<
              decltype(create_invocation_owned_source_build_context),
              PinnedReviewedSourceBuild>);
static_assert(!std::is_invocable_v<
              decltype(create_invocation_owned_source_build_context),
              PackageBaseIdentity,
              AurRecipeRevision,
              std::filesystem::path>);
static_assert(!std::is_default_constructible_v<
              ReviewedRecipeSnapshotIdentity>);
static_assert(!std::is_default_constructible_v<
              InvocationOwnedMakepkgEnvironment>);
static_assert(!std::is_default_constructible_v<
              EvaluatedDevelSourceBuildProof>);
static_assert(!std::is_copy_constructible_v<
              EvaluatedDevelSourceBuildProof>);
static_assert(!std::is_constructible_v<
              EvaluatedDevelSourceBuildProof,
              PackageBaseIdentity,
              std::string,
              std::filesystem::path>);
static_assert(!std::is_default_constructible_v<
              EvaluatedDevelSourceProjection>);
static_assert(!std::is_constructible_v<
              EvaluatedDevelSourceProjection,
              VcsSourceIdentity,
              std::size_t,
              std::size_t>);
static_assert(!std::is_default_constructible_v<
              FreshDevelPackageArtifact>);
static_assert(!std::is_constructible_v<
              FreshDevelPackageArtifact,
              std::filesystem::path>);
static_assert(!std::is_constructible_v<
              EvaluatedDevelSourceBuildProof,
              InvocationOwnedSourceBuildContext,
              EvaluatedDevelSourceProjection,
              ActualBuiltGitRevision,
              FreshDevelPackageArtifact>);
static_assert(!std::is_constructible_v<
              InstalledArtifactBinding,
              PackageChildIdentity,
              PackageVersionIdentity,
              InstalledPackageArchitectureIdentity,
              AlpmMtreeSha256Digest,
              InstalledDatabaseRecordSha256Digest,
              InstalledPackageRecordGeneration>);

#if defined(MOGUET_FORGE_LIFECYCLE_EXPECTED)
class ReviewedSourceLifecycleAuthority final {
public:
    static ReviewedSourceExpectedStateObservation forge(
        ReviewedSourceStateStoreRead store_read) {
        return ReviewedSourceExpectedStateObservation(
            std::move(store_read));
    }
};
#elif defined(MOGUET_FORGE_FATAL_PREFLIGHT)
class ReviewedSourceLifecycleAuthority final {
public:
    static ReviewedSourceFatalStatePreflight forge(
        PackageBaseIdentity package_base,
        ReviewedSourceStateStoreRead store_read) {
        return ReviewedSourceFatalStatePreflight(
            std::move(package_base), std::move(store_read));
    }
};
#elif defined(MOGUET_FORGE_LIFECYCLE_ALREADY)
class ReviewedSourceLifecycleAuthority final {
public:
    static ReviewedSourceAlreadyReviewedContinue forge(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceExpectedStateObservation expected) {
        return ReviewedSourceAlreadyReviewedContinue(
            std::move(identity), std::move(expected));
    }
};
#elif defined(MOGUET_FORGE_RETAINED_DESCRIPTOR)
struct ReviewedSourcePackageBaseLeaseAccess {
    static int descriptor(
        const RetainedTrustedCacheDirectory& directory) {
        return directory.descriptor_;
    }
};
#elif defined(MOGUET_FORGE_ACCEPTED_CHECKOUT)
struct ReviewedSourcePinnedBuildAccess {
    static AcceptedReviewedSourceCheckout forge(
        AcceptedReviewedSourceTarget target,
        ReviewedSourcePackageBaseLease lease,
        TrustedGitPinnedCheckout checkout) {
        return AcceptedReviewedSourceCheckout(
            std::move(target), std::move(lease),
            std::move(checkout));
    }
};
#elif defined(MOGUET_FORGE_ALREADY_CHECKOUT)
struct ReviewedSourcePinnedBuildAccess {
    static AlreadyReviewedSourceCheckout forge(
        ReviewedSourceAlreadyReviewedContinue target,
        ReviewedSourcePackageBaseLease lease,
        TrustedGitPinnedCheckout checkout) {
        return AlreadyReviewedSourceCheckout(
            std::move(target), std::move(lease),
            std::move(checkout));
    }
};
#elif defined(MOGUET_FORGE_PINNED_ACCEPTED)
struct ReviewedSourcePinnedBuildAccess {
    static PinnedReviewedSourceBuild forge(
        AcceptedReviewedSourceCheckout checkout,
        ReviewedSourcePublicationStatus publication_status,
        ReviewedSourceState state,
        ReviewedSourceStateObservedRecord observed,
        ReviewedSourceEditorOverlayProof editor_overlay) {
        return PinnedReviewedSourceBuild(
            std::move(checkout), publication_status,
            std::move(state), std::move(observed),
            std::move(editor_overlay));
    }
};
#elif defined(MOGUET_FORGE_PINNED_ALREADY)
struct ReviewedSourcePinnedBuildAccess {
    static PinnedReviewedSourceBuild forge(
        AlreadyReviewedSourceCheckout checkout,
        ReviewedSourceState state,
        ReviewedSourceStateObservedRecord observed,
        ReviewedSourceEditorOverlayProof editor_overlay) {
        return PinnedReviewedSourceBuild(
            std::move(checkout), std::move(state),
            std::move(observed),
            std::move(editor_overlay));
    }
};
#elif defined(MOGUET_FORGE_EDITOR_BOUNDARY)
struct ReviewedSourceEditorOverlayAccess {
    static ReviewedSourceEditorBoundary forge(
        AurReviewedSourceReviewIdentity identity,
        TrustedGitPinnedCheckoutOverlayObservation observation) {
        return ReviewedSourceEditorBoundary(
            std::move(identity), 1, 2,
            std::move(observation));
    }
};
#elif defined(MOGUET_FORGE_EDITOR_OVERLAY)
struct ReviewedSourceEditorOverlayAccess {
    static ReviewedSourceEditorOverlayProof forge(
        AurReviewedSourceReviewIdentity identity,
        TrustedGitPinnedCheckoutOverlayObservation pre_editor,
        TrustedGitPinnedCheckoutOverlayObservation post_editor) {
        return ReviewedSourceEditorOverlayProof(
            std::move(identity), 1, 2,
            std::move(pre_editor), std::move(post_editor));
    }
};
#elif defined(MOGUET_FORGE_PROVENANCE_REVIEWED_GENERATION)
ReviewedSourceStateRecordGeneration forge_reviewed_generation() {
    return ReviewedSourceStateRecordGeneration(1);
}
#elif defined(MOGUET_FORGE_PROVENANCE_REVIEWED_BINDING)
ReviewedSourceStateRecordBinding forge_reviewed_binding(
    PackageBaseIdentity package_base,
    AurRecipeRevision revision,
    ReviewedSourceStateRecordGeneration generation,
    ReviewedSourceStateDocumentSha256Digest digest) {
    return ReviewedSourceStateRecordBinding(
        std::move(package_base), std::move(revision),
        std::move(generation), std::move(digest));
}
#elif defined(MOGUET_FORGE_PROVENANCE_REVIEWED_BINDING_AUTHORITY)
ReviewedSourceStateRecordBinding forge_reviewed_binding_authority(
    PackageBaseIdentity package_base,
    AurRecipeRevision revision,
    ReviewedSourceStateDocumentSha256Digest digest) {
    return ReviewedSourceStateRecordBindingAuthority::make(
        std::move(package_base), std::move(revision), 1,
        std::move(digest));
}
#elif defined(MOGUET_FORGE_INSTALLED_ARTIFACT_BINDING)
InstalledArtifactBinding forge_installed_binding(
    PackageChildIdentity package,
    PackageVersionIdentity version,
    InstalledPackageArchitectureIdentity architecture,
    AlpmMtreeSha256Digest mtree,
    InstalledDatabaseRecordSha256Digest database,
    InstalledPackageRecordGeneration generation) {
    return InstalledArtifactBinding::make(
        std::move(package), std::move(version), std::move(architecture),
        std::move(mtree), std::move(database), std::move(generation));
}
#elif defined(MOGUET_FORGE_PROVENANCE_PERSISTENT_DECODER)
DevelBuildProvenanceDocument forge_persistent_decoder(
    std::string_view document) {
    return DevelBuildProvenancePersistentDecoderAccess::decode_document(
        document);
}
#elif defined(MOGUET_FORGE_INVOCATION_SOURCE_BUILD_CONTEXT)
InvocationOwnedSourceBuildContext forge_invocation_source_build_context() {
    return InvocationOwnedSourceBuildContext(nullptr);
}
#elif defined(MOGUET_FORGE_REVIEWED_RECIPE_SNAPSHOT_IDENTITY)
ReviewedRecipeSnapshotIdentity forge_reviewed_recipe_snapshot_identity(
    ReviewedSourceStateRecordBinding binding,
    ReviewedSourceObjectId tree) {
    return ReviewedRecipeSnapshotIdentity(
        std::move(binding), std::move(tree), 1);
}
#elif defined(MOGUET_FORGE_INVOCATION_MAKEPKG_ENVIRONMENT)
InvocationOwnedMakepkgEnvironment forge_invocation_makepkg_environment(
    SourceBuildEnvironment environment) {
    return InvocationOwnedMakepkgEnvironment(
        std::move(environment), SourceEnvironmentEmptyValuePolicy::Forward,
        std::make_shared<const int>(0));
}
#elif defined(MOGUET_FORGE_EVALUATED_DEVEL_SOURCE_PROJECTION)
EvaluatedDevelSourceProjection forge_evaluated_source_projection(
    VcsSourceIdentity source) {
    return EvaluatedDevelSourceProjection(
        std::move(source), 1, 0);
}
#elif defined(MOGUET_FORGE_FRESH_DEVEL_PACKAGE_ARTIFACT)
FreshDevelPackageArtifact forge_fresh_devel_package_artifact(
    PackageChildIdentity package,
    BuiltPackageArtifactEvidence evidence,
    std::filesystem::path path) {
    return FreshDevelPackageArtifact(
        std::move(package), std::move(evidence), std::move(path),
        "artifact.pkg.tar.zst", -1, 1, 2, 3, 4);
}
#elif defined(MOGUET_FORGE_EVALUATED_DEVEL_SOURCE_BUILD_PROOF)
EvaluatedDevelSourceBuildProof forge_evaluated_source_build_proof() {
    return EvaluatedDevelSourceBuildProof(nullptr);
}
#else
int reviewed_source_authority_negative_fixture_baseline() {
    return 0;
}
#endif
