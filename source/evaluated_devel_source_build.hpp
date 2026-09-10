#pragma once

#include "evaluated_devel_source_build_authority.hpp"
#include "evaluated_devel_source_artifact_transport.hpp"
#include "devel_build_provenance.hpp"
#include "invocation_owned_source_build_context.hpp"
#include "local_package_metadata.hpp"
#include "process.hpp"
#include "srcinfo_source_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <variant>

#ifdef MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
#include <functional>
#endif

enum class EvaluatedDevelSourceBuildStage {
    ContextValidation,
    WorkingRecipe,
    EvaluatedSource,
    SourcePreparation,
    SourceWorkspace,
    GitRevision,
    DynamicVersion,
    PackageBuild,
    ArtifactInventory,
    ArtifactMetadata,
    ArchiveDigest,
    MtreeDigest,
    Cleanup,
};

enum class EvaluatedDevelSourceBuildFailureReason {
    InvalidBuildContext,
    ContextRevalidationFailure,
    EnvironmentLineageMismatch,
    WorkingRecipeFailure,
    EvaluatedSourceFailure,
    RawEvaluatedSourceMismatch,
    UnsupportedSourceShape,
    MakepkgPhaseFailure,
    SourceWorkspaceUnavailable,
    SourceWorkspaceAmbiguous,
    SourceContainmentFailure,
    GitRepositoryInvalid,
    GitRevisionUnavailable,
    GitRevisionMismatch,
    DynamicVersionUnavailable,
    ArtifactInventoryMismatch,
    ArtifactMetadataMismatch,
    ArtifactReplacement,
    ArtifactHashFailure,
    MtreeFailure,
    ResourceLimitExceeded,
    CleanupFailure,
    InternalFailure,
    ArtifactMetadataQueryFailure,
};

enum class EvaluatedDevelSourceBuildProcess {
    InitialPrintSrcinfo,
    SourcePreparation,
    PreparedPrintSrcinfo,
    PreparedPackagelist,
    PackageBuild,
    GitWorkspaceObservation,
    GitMirrorObservation,
    ArchiveInventory,
    PackageMetadataExtraction,
    MtreeExtraction,
};

struct EvaluatedDevelSourceBuildCleanupConsequence {
    InvocationOwnedSourceBuildContextFailure failure;
    std::filesystem::path retained_root;

    bool operator==(
        const EvaluatedDevelSourceBuildCleanupConsequence&) const = default;
};

struct EvaluatedDevelSourceBuildFailure {
    EvaluatedDevelSourceBuildStage stage =
        EvaluatedDevelSourceBuildStage::ContextValidation;
    EvaluatedDevelSourceBuildFailureReason reason =
        EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext;
    std::optional<EvaluatedDevelSourceBuildProcess> process;
    std::optional<InvocationOwnedSourceBuildContextFailure> context_failure;
    std::optional<SrcinfoSourceMetadataParseFailure> source_parse_failure;
    std::optional<LocalPackageMetadataParseFailure> package_parse_failure;
    std::optional<BoundedProcessOutcome> process_outcome;
    std::optional<ActualBuiltGitRevisionProofFailure> revision_failure;
    std::optional<std::error_code> system_error;
    std::optional<std::string> diagnostic;
    std::optional<EvaluatedDevelSourceBuildCleanupConsequence>
        cleanup_consequence;

    bool operator==(const EvaluatedDevelSourceBuildFailure&) const = default;
};

// Effective source semantics accepted only after the exact reviewed
// `.SRCINFO` projection and same-context `makepkg --printsrcinfo` projection
// agree. The Git identity reuses the common source model; counts retain the
// supported-shape proof without exposing another raw-string identity.
class EvaluatedDevelSourceProjection final {
public:
    EvaluatedDevelSourceProjection() = delete;
    EvaluatedDevelSourceProjection(
        const EvaluatedDevelSourceProjection&) = default;
    EvaluatedDevelSourceProjection(
        EvaluatedDevelSourceProjection&&) noexcept = default;
    EvaluatedDevelSourceProjection& operator=(
        const EvaluatedDevelSourceProjection&) = default;
    EvaluatedDevelSourceProjection& operator=(
        EvaluatedDevelSourceProjection&&) noexcept = default;
    ~EvaluatedDevelSourceProjection() = default;

    [[nodiscard]] const VcsSourceIdentity& git_source() const noexcept;
    [[nodiscard]] std::size_t source_count() const noexcept;
    [[nodiscard]] std::size_t tracked_local_source_count() const noexcept;

    bool operator==(const EvaluatedDevelSourceProjection&) const = default;

private:
    friend class EvaluatedDevelSourceBuildAuthority;

    EvaluatedDevelSourceProjection(
        VcsSourceIdentity git_source, std::size_t source_count,
        std::size_t tracked_local_source_count) noexcept;

    VcsSourceIdentity git_source_;
    std::size_t source_count_ = 0;
    std::size_t tracked_local_source_count_ = 0;
};

// Retains the exact post-build archive descriptor and typed evidence. A path
// is exposed for diagnostics and future command presentation only; it cannot
// construct or substitute this capability.
class FreshDevelPackageArtifact final {
public:
    FreshDevelPackageArtifact() = delete;
    FreshDevelPackageArtifact(const FreshDevelPackageArtifact&) = delete;
    FreshDevelPackageArtifact& operator=(
        const FreshDevelPackageArtifact&) = delete;
    FreshDevelPackageArtifact(
        FreshDevelPackageArtifact&& other) noexcept;
    FreshDevelPackageArtifact& operator=(
        FreshDevelPackageArtifact&&) = delete;
    ~FreshDevelPackageArtifact() noexcept;

    [[nodiscard]] const PackageChildIdentity& package() const noexcept;
    [[nodiscard]] const BuiltPackageArtifactEvidence& evidence()
        const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::uintmax_t size() const noexcept;

private:
    friend class EvaluatedDevelSourceBuildAuthority;
    friend class EvaluatedDevelSourceBuildProof;
    friend class EvaluatedDevelSourceArtifactTransport;

    FreshDevelPackageArtifact(
        PackageChildIdentity package,
        BuiltPackageArtifactEvidence evidence,
        std::filesystem::path path, std::string leaf_name,
        int descriptor, std::uintmax_t device,
        std::uintmax_t inode, std::uintmax_t owner,
        std::uintmax_t size) noexcept;

    PackageChildIdentity package_;
    BuiltPackageArtifactEvidence evidence_;
    std::filesystem::path path_;
    std::string leaf_name_;
    int descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;
    std::uintmax_t size_ = 0;
};

// POLICY(#476 Slice 4): the move-only result owns the same Slice 3 context
// that evaluated, prepared, and built the source, plus the retained artifact
// authority. Slice 5 may later consume this object for installation; a raw
// artifact path, OID, or version tuple is never an equivalent input.
class EvaluatedDevelSourceBuildProof final {
public:
    EvaluatedDevelSourceBuildProof() = delete;
    EvaluatedDevelSourceBuildProof(
        const EvaluatedDevelSourceBuildProof&) = delete;
    EvaluatedDevelSourceBuildProof& operator=(
        const EvaluatedDevelSourceBuildProof&) = delete;
    EvaluatedDevelSourceBuildProof(
        EvaluatedDevelSourceBuildProof&& other) noexcept;
    EvaluatedDevelSourceBuildProof& operator=(
        EvaluatedDevelSourceBuildProof&&) = delete;
    ~EvaluatedDevelSourceBuildProof() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const PackageBaseIdentity& package_base() const;
    [[nodiscard]] const ReviewedSourceStateRecordBinding& reviewed_binding()
        const;
    [[nodiscard]] const ReviewedRecipeSnapshotIdentity& snapshot_identity()
        const;
    [[nodiscard]] const EvaluatedDevelSourceProjection& evaluated_source()
        const;
    [[nodiscard]] const ActualBuiltGitRevision& actual_built_revision()
        const;
    [[nodiscard]] const FreshDevelPackageArtifact& artifact() const;

    // Explicit cleanup is available to tests and abandoned future installs.
    // Refusal keeps the root; the context owns whether a safe retry is allowed.
    [[nodiscard]] InvocationOwnedSourceBuildContextCleanupResult cleanup() noexcept;

private:
    friend class EvaluatedDevelSourceBuildAuthority;

    friend class EvaluatedDevelSourceArtifactTransport;
    friend class InstalledArtifactBindingObserver;
    friend class DevelSourceArtifactInstallAuthority;
#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
    friend class DevelSourceArtifactInstallFixture;
#endif
    struct State;
    explicit EvaluatedDevelSourceBuildProof(
        std::unique_ptr<State> state);
    [[nodiscard]] const State& require_state() const;
    [[nodiscard]] State& require_state();

    std::unique_ptr<State> state_;
    // Per-build identity, unrelated to package/version/path/content equality.
    // Allocated while minting Slice 4, before any install side effect.
    std::shared_ptr<const unsigned char> lineage_ = std::make_shared<const unsigned char>(0);
};

// The only production mint consumes one context and an environment sealed by
// that same context. Normal source-build and install routes intentionally have
// no caller in Slice 4.
[[nodiscard]] EvaluatedDevelSourceBuildResult
build_evaluated_devel_source(
    InvocationOwnedSourceBuildContext context,
    InvocationOwnedMakepkgEnvironment environment);

enum class EvaluatedDevelSourceBuildTestEvent {
    WorkingRecipeReady,
    BeforeInitialPrintSrcinfo,
    AfterSourcePreparation,
    BeforePackageBuild,
    AfterPackageBuild,
    AfterArtifactInventory,
    AfterArtifactOpen,
    BeforeFinalArtifactReproof,
};

#ifdef MOGUET_ENABLE_EVALUATED_DEVEL_SOURCE_BUILD_TEST_HOOKS
using EvaluatedDevelSourceBuildTestHook = std::function<void(
    EvaluatedDevelSourceBuildTestEvent event,
    const std::filesystem::path& owned_root,
    const std::filesystem::path& artifact_path)>;

void set_evaluated_devel_source_build_test_hook(
    EvaluatedDevelSourceBuildTestHook hook);
#endif
