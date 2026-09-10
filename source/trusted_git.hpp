#pragma once

#include "reviewed_source_review.hpp"
#include "reviewed_source_projection.hpp"
#include "reviewed_source_trusted_review.hpp"
#include "trusted_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

class ReviewedSourcePackageBaseLease;
class InvocationOwnedSourceBuildContextAuthority;

// Moguet-owned persistent checkoutで許可するGit operationだけを公開する。
// Filesystem mutation authorityはValidatedCachePath側に残し、Gitへ渡すpathは
// explicit repository/worktree binding用のlogical viewとしてのみ使用する。
std::string trusted_git_remote_origin_url(
    const ValidatedCachePath& checkout);
int trusted_git_fetch_origin(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url);
int trusted_git_fetch_origin(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const ReviewedSourcePackageBaseLease& lease);
std::string trusted_git_detect_remote_branch(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url);
int trusted_git_diff_quiet(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch);
std::string trusted_git_diff_name_only(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch);
int trusted_git_show_diff(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch);
int trusted_git_reset_hard(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch);
int trusted_git_reset_hard(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch,
    const ReviewedSourcePackageBaseLease& lease);
int trusted_git_clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url);
int trusted_git_clone_persistent_checkout(
    const ValidatedCachePath& destination,
    const std::string& remote_url,
    const ReviewedSourcePackageBaseLease& lease);

enum class TrustedGitReviewStage {
    TargetResolution,
    TargetValidation,
    BaselineValidation,
    ShallowRepositoryCheck,
    AncestryCheck,
    BaselineTree,
    TargetTree,
    NameStatus,
    Numstat,
    AttributeGuard,
    HistoryGuard,
    ResourcePreflight,
    CrossStream,
    BlobRead,
    PatchGeneration,
};

enum class TrustedGitReviewFailureReason {
    CommandFailed,
    CaptureLimitExceeded,
    MalformedMachineOutput,
    InconsistentMachineOutput,
    RenameCandidateLimitExceeded,
    SingleBlobSizeLimitExceeded,
    AggregateBlobSizeLimitExceeded,
    LocalAttributeOverride,
    LocalHistoryOverride,
    ShallowRepositoryUnsupported,
    ObjectFormatMismatch,
    ReviewIdentityMismatch,
};

struct TrustedGitReviewFailure {
    TrustedGitReviewFailureReason reason;
    TrustedGitReviewStage stage;
    std::optional<int> exit_code;
    std::size_t record_index = 0;
    std::uintmax_t observed = 0;
    std::uintmax_t limit = 0;

    bool operator==(const TrustedGitReviewFailure&) const = default;
};

using TrustedGitCommitResolutionResult = std::variant<
    SourceRevisionIdentity,
    TrustedGitReviewFailure>;

using TrustedGitReviewedSourceProjectionResult = std::variant<
    ReviewedSourceInitialFullReview,
    ReviewedSourceAlreadyReviewed,
    ReviewedSourceUpdateReview,
    ReviewedSourceRebaselineFullReview,
    TrustedGitReviewFailure>;

using TrustedGitAurReviewedSourceProjectionResult = std::variant<
    TrustedAurReviewedSourceProjection,
    TrustedGitReviewFailure>;

using TrustedGitReviewedSourceMaterializationResult = std::variant<
    ReviewedSourceVerifiedMaterializedReview,
    ReviewedSourceReviewFailure,
    TrustedGitReviewFailure>;

using TrustedGitAurReviewedSourceMaterializationResult = std::variant<
    TrustedAurReviewedSourceReview,
    ReviewedSourceReviewFailure,
    TrustedGitReviewFailure>;

enum class TrustedGitPinnedCheckoutStage {
    TargetValidation,
    CheckoutMaterialization,
    ResidueCleanup,
    HeadVerification,
    IndexVerification,
    WorktreeVerification,
    OverlayObservation,
    OverlayRevalidation,
    BoundaryRevalidation,
};

enum class TrustedGitPinnedCheckoutFailureReason {
    InvalidCapability,
    CheckoutBoundaryInvalid,
    RemoteMismatch,
    ObjectFormatMismatch,
    TargetRevisionMismatch,
    LocalAttributeOverride,
    LocalHistoryOverride,
    CommandFailed,
    CaptureLimitExceeded,
    MalformedOutput,
    AttachedHead,
    DirtyIndex,
    DirtyWorktree,
    UnsafeIndexFlags,
    OverlayMismatch,
    UnsupportedOverlayEntry,
};

struct TrustedGitPinnedCheckoutFailure {
    TrustedGitPinnedCheckoutFailureReason reason;
    TrustedGitPinnedCheckoutStage stage;
    std::optional<int> exit_code;
    std::size_t observed = 0;
    std::size_t limit = 0;
    std::optional<TrustedCacheFailure> boundary_failure;

    bool operator==(const TrustedGitPinnedCheckoutFailure&) const = default;
};

struct TrustedGitPinnedCheckoutRevalidated {
    bool operator==(const TrustedGitPinnedCheckoutRevalidated&) const =
        default;
};

// Sealed proof that one managed checkout was detached and materialized from
// the exact reviewed target OID. The proof is useful only while its caller also
// retains the PackageBase lease; raw path/OID sidecars cannot construct it.
class TrustedGitPinnedCheckout final {
public:
    TrustedGitPinnedCheckout() = delete;
    TrustedGitPinnedCheckout(const TrustedGitPinnedCheckout&) = delete;
    TrustedGitPinnedCheckout(TrustedGitPinnedCheckout&& other) noexcept;
    TrustedGitPinnedCheckout& operator=(
        const TrustedGitPinnedCheckout&) = delete;
    TrustedGitPinnedCheckout& operator=(
        TrustedGitPinnedCheckout&& other) noexcept;
    ~TrustedGitPinnedCheckout();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity() const;
    [[nodiscard]] const std::filesystem::path& checkout_path() const;
    [[nodiscard]] std::uintmax_t checkout_device() const;
    [[nodiscard]] std::uintmax_t checkout_inode() const;

private:
    friend std::variant<
        TrustedGitPinnedCheckout,
        TrustedGitPinnedCheckoutFailure>
    trusted_git_materialize_pinned_checkout(
        const ValidatedCachePath& checkout,
        AurReviewedSourceReviewIdentity identity,
        const ReviewedSourcePackageBaseLease& lease);
    friend std::variant<
        TrustedGitPinnedCheckoutRevalidated,
        TrustedGitPinnedCheckoutFailure>
    revalidate_trusted_git_pinned_checkout(
        const TrustedGitPinnedCheckout& checkout);
    friend std::variant<
        class TrustedGitPinnedCheckoutOverlayObservation,
        TrustedGitPinnedCheckoutFailure>
    observe_clean_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease);
    friend std::variant<
        class TrustedGitPinnedCheckoutOverlayObservation,
        TrustedGitPinnedCheckoutFailure>
    observe_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease);
    friend std::variant<
        TrustedGitPinnedCheckoutRevalidated,
        TrustedGitPinnedCheckoutFailure>
    revalidate_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease,
        const class TrustedGitPinnedCheckoutOverlayObservation&
            expected);
    friend std::variant<
        class TrustedGitReviewedRecipeSnapshot,
        TrustedGitReviewFailure,
        ReviewedSourceReviewFailure,
        TrustedGitPinnedCheckoutFailure>
    trusted_git_project_reviewed_recipe_snapshot(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease,
        const class TrustedGitPinnedCheckoutOverlayObservation& expected);

    struct State;

    TrustedGitPinnedCheckout(
        ValidatedCachePath checkout,
        AurReviewedSourceReviewIdentity identity);

    [[nodiscard]] const State& require_state() const;

    std::unique_ptr<State> state_;
};

// Exact Git-backed projection plus a descriptor-observed filesystem manifest
// of all worktree entries except the repository's own top-level .git metadata.
// The Git projection uses an invocation-private alternate index and includes
// ignored/untracked entries; the manifest additionally seals directories,
// full modes, and nested .git inputs that Git trees cannot represent.
// Only the three exact observation/revalidation functions below can construct
// or consume this value as proof.
class TrustedGitPinnedCheckoutOverlayObservation final {
public:
    TrustedGitPinnedCheckoutOverlayObservation() = delete;
    TrustedGitPinnedCheckoutOverlayObservation(
        const TrustedGitPinnedCheckoutOverlayObservation&) = default;
    TrustedGitPinnedCheckoutOverlayObservation(
        TrustedGitPinnedCheckoutOverlayObservation&&) noexcept = default;
    TrustedGitPinnedCheckoutOverlayObservation& operator=(
        const TrustedGitPinnedCheckoutOverlayObservation&) = default;
    TrustedGitPinnedCheckoutOverlayObservation& operator=(
        TrustedGitPinnedCheckoutOverlayObservation&&) noexcept = default;
    ~TrustedGitPinnedCheckoutOverlayObservation() = default;

    bool operator==(
        const TrustedGitPinnedCheckoutOverlayObservation&) const =
        default;

private:
    friend std::variant<
        TrustedGitPinnedCheckoutOverlayObservation,
        TrustedGitPinnedCheckoutFailure>
    observe_clean_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease);
    friend std::variant<
        TrustedGitPinnedCheckoutOverlayObservation,
        TrustedGitPinnedCheckoutFailure>
    observe_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease);
    friend std::variant<
        TrustedGitPinnedCheckoutRevalidated,
        TrustedGitPinnedCheckoutFailure>
    revalidate_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease,
        const TrustedGitPinnedCheckoutOverlayObservation& expected);
    friend std::variant<
        class TrustedGitReviewedRecipeSnapshot,
        TrustedGitReviewFailure,
        ReviewedSourceReviewFailure,
        TrustedGitPinnedCheckoutFailure>
    trusted_git_project_reviewed_recipe_snapshot(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease,
        const TrustedGitPinnedCheckoutOverlayObservation& expected);

    TrustedGitPinnedCheckoutOverlayObservation(
        AurReviewedSourceReviewIdentity identity,
        std::uintmax_t checkout_device,
        std::uintmax_t checkout_inode,
        ReviewedSourceObjectId tree,
        std::string filesystem_manifest) noexcept;

    AurReviewedSourceReviewIdentity identity_;
    std::uintmax_t checkout_device_ = 0;
    std::uintmax_t checkout_inode_ = 0;
    ReviewedSourceObjectId tree_;
    std::string filesystem_manifest_;
};

using TrustedGitPinnedCheckoutResult = std::variant<
    TrustedGitPinnedCheckout,
    TrustedGitPinnedCheckoutFailure>;

using TrustedGitPinnedCheckoutRevalidationResult = std::variant<
    TrustedGitPinnedCheckoutRevalidated,
    TrustedGitPinnedCheckoutFailure>;

using TrustedGitPinnedCheckoutOverlayObservationResult = std::variant<
    TrustedGitPinnedCheckoutOverlayObservation,
    TrustedGitPinnedCheckoutFailure>;

// Hash-verified bytes for every entry in one exact reviewed commit tree.
// The capability is intentionally opaque outside the invocation-owned context
// mint: callers may inspect its identity, but cannot extract/recombine raw
// snapshot entries with another workspace.
class TrustedGitReviewedRecipeSnapshot final {
public:
    TrustedGitReviewedRecipeSnapshot() = delete;
    TrustedGitReviewedRecipeSnapshot(
        const TrustedGitReviewedRecipeSnapshot&) = delete;
    TrustedGitReviewedRecipeSnapshot(
        TrustedGitReviewedRecipeSnapshot&&) noexcept = default;
    TrustedGitReviewedRecipeSnapshot& operator=(
        const TrustedGitReviewedRecipeSnapshot&) = delete;
    TrustedGitReviewedRecipeSnapshot& operator=(
        TrustedGitReviewedRecipeSnapshot&&) noexcept = default;
    ~TrustedGitReviewedRecipeSnapshot() = default;

    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity()
        const noexcept;
    [[nodiscard]] const ReviewedSourceObjectId& git_tree_object_id()
        const noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept;

private:
    struct Entry {
        ReviewedSourceFileVersion version;
        std::string bytes;
    };

    friend class InvocationOwnedSourceBuildContextAuthority;
    friend std::variant<
        TrustedGitReviewedRecipeSnapshot,
        TrustedGitReviewFailure,
        ReviewedSourceReviewFailure,
        TrustedGitPinnedCheckoutFailure>
    trusted_git_project_reviewed_recipe_snapshot(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease,
        const TrustedGitPinnedCheckoutOverlayObservation& expected);

    TrustedGitReviewedRecipeSnapshot(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceObjectId git_tree_object_id,
        std::vector<Entry> entries) noexcept;

    AurReviewedSourceReviewIdentity identity_;
    ReviewedSourceObjectId git_tree_object_id_;
    std::vector<Entry> entries_;
};

using TrustedGitReviewedRecipeSnapshotResult = std::variant<
    TrustedGitReviewedRecipeSnapshot,
    TrustedGitReviewFailure,
    ReviewedSourceReviewFailure,
    TrustedGitPinnedCheckoutFailure>;

// This is a mutation boundary, unlike the read-only review projection above.
// It resolves no ref: the supplied identity already owns the complete target
// OID, which is checked out detached and then re-proven clean.
[[nodiscard]] TrustedGitPinnedCheckoutResult
trusted_git_materialize_pinned_checkout(
    const ValidatedCachePath& checkout,
    AurReviewedSourceReviewIdentity identity,
    const ReviewedSourcePackageBaseLease& lease);

// Re-proves remote/object format, detached HEAD, exact index and clean
// worktree. A caller must run this before converting a retained checkout into
// build authority after another side-effect boundary such as state CAS.
[[nodiscard]] TrustedGitPinnedCheckoutRevalidationResult
revalidate_trusted_git_pinned_checkout(
    const TrustedGitPinnedCheckout& checkout);

// The clean observation is the pre-editor boundary. The second observation
// permits a dirty worktree but still requires exact detached HEAD, exact real
// index, safe Git metadata, and the same retained checkout identity.
[[nodiscard]] TrustedGitPinnedCheckoutOverlayObservationResult
observe_clean_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease);

[[nodiscard]] TrustedGitPinnedCheckoutOverlayObservationResult
observe_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease);

[[nodiscard]] TrustedGitPinnedCheckoutRevalidationResult
revalidate_trusted_git_pinned_checkout_overlay(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease,
    const TrustedGitPinnedCheckoutOverlayObservation& expected);

// Reads only the exact commit/object database already sealed by #411. It does
// not inspect worktree bytes, copy .git metadata, run makepkg, or create a
// build workspace. The caller must retain the same lease/overlay capability.
[[nodiscard]] TrustedGitReviewedRecipeSnapshotResult
trusted_git_project_reviewed_recipe_snapshot(
    const TrustedGitPinnedCheckout& checkout,
    const ReviewedSourcePackageBaseLease& lease,
    const TrustedGitPinnedCheckoutOverlayObservation& expected);

// Resolve the mutable remote-tracking ref once. Callers must retain and use
// only the returned complete commit OID for later projection.
TrustedGitCommitResolutionResult trusted_git_resolve_remote_commit(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const std::string& branch);

// Read commit trees only. This API does not update refs, index, working tree,
// reviewed state, or any production source-build lifecycle.
TrustedGitReviewedSourceProjectionResult trusted_git_project_reviewed_source(
    const ValidatedCachePath& checkout,
    const std::string& expected_remote_url,
    const SourceRevisionIdentity& target,
    const std::optional<SourceRevisionIdentity>& baseline);

// Project and seal the complete Slice 3A inventory together with its AUR
// PackageBase/source/remote/format/target/baseline provenance.
TrustedGitAurReviewedSourceProjectionResult
trusted_git_project_aur_reviewed_source(
    const ValidatedCachePath& checkout,
    AurReviewedSourceReviewIdentity identity,
    std::optional<SourceRevisionIdentity> baseline);

// Materialize exact blobs named by a sealed Slice 3A projection capability.
// This API does not resolve refs, read worktree/index content, render human
// output, publish reviewed state, or connect a production lifecycle.
TrustedGitReviewedSourceMaterializationResult
trusted_git_materialize_reviewed_source_review(
    const ValidatedCachePath& checkout,
    TrustedAurReviewedSourceProjection projection);

// Seals PackageBase/source/remote/exact target and the verified 3B review in
// one trusted construction boundary. The returned capability is the only 4A
// input that can bind trusted Git review provenance to acceptance.
TrustedGitAurReviewedSourceMaterializationResult
trusted_git_materialize_aur_reviewed_source_review(
    const ValidatedCachePath& checkout,
    TrustedAurReviewedSourceProjection projection);

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_GIT_TEST_HOOKS
void set_trusted_git_review_machine_stream_limit_for_test(
    std::optional<std::size_t> limit);
#endif

// PKGBUILD exportはcache checkoutとは別のdescriptor-anchored temporary
// lifecycle。Git process isolationだけを共有し、cache capabilityへは昇格しない。
int trusted_git_clone_aur_export(
    const std::string& remote_url,
    const std::filesystem::path& anchored_destination);
std::string trusted_git_aur_export_remote_origin_url(
    const std::filesystem::path& anchored_checkout);
