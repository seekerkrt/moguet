#pragma once

#include "reviewed_source_acceptance.hpp"
#include "reviewed_source_state_store.hpp"
#include "trusted_git.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

// POLICY(#411): Slice 4B owns the production-disconnected transition from an
// explicitly accepted upstream review to an exact leased checkout, reviewed
// state CAS, and pinned build authority. It does not run an editor, makepkg,
// pacman, or any production source-build route.

class ReviewedSourcePackageBaseLease;
class AcceptedReviewedSourceCheckout;
class AlreadyReviewedSourceCheckout;
class ReviewedSourceEditorBoundary;
class ReviewedSourceEditorOverlayProof;
class PinnedReviewedSourceBuild;
class InvocationOwnedSourceBuildContextAuthority;
struct ReviewedSourcePinnedCheckoutFailure;
struct ReviewedSourcePublicationConflict;
struct ReviewedSourcePublicationUncertain;
struct ReviewedSourcePublicationUnsafeHistory;
struct ReviewedSourcePublicationFailure;
struct ReviewedSourcePostPublicationCheckoutFailure;

using AcceptedReviewedSourceCheckoutResult = std::variant<
    AcceptedReviewedSourceCheckout,
    ReviewedSourcePinnedCheckoutFailure>;

using AlreadyReviewedSourceCheckoutResult = std::variant<
    AlreadyReviewedSourceCheckout,
    ReviewedSourcePinnedCheckoutFailure>;

using ReviewedSourcePublicationResult = std::variant<
    PinnedReviewedSourceBuild,
    ReviewedSourcePublicationConflict,
    ReviewedSourcePublicationUncertain,
    ReviewedSourcePublicationUnsafeHistory,
    ReviewedSourcePublicationFailure,
    ReviewedSourcePostPublicationCheckoutFailure>;

using ReviewedSourceEditorBoundaryResult = std::variant<
    ReviewedSourceEditorBoundary,
    ReviewedSourcePublicationFailure>;

using ReviewedSourceEditorOverlayProofResult = std::variant<
    ReviewedSourceEditorOverlayProof,
    ReviewedSourcePublicationFailure>;

// Path-based transitions acquire a sealed PackageBase lease. Production may
// instead pass the already-held shared lease through the with_lease forms.
// Both materialize the detached target, clean all residue, and prove the final
// checkout without resolving a mutable ref again.
[[nodiscard]] AcceptedReviewedSourceCheckoutResult
materialize_accepted_reviewed_source_checkout(
    AcceptedReviewedSourceTarget target,
    const ValidatedCachePath& checkout);

[[nodiscard]] AcceptedReviewedSourceCheckoutResult
materialize_accepted_reviewed_source_checkout_with_lease(
    AcceptedReviewedSourceTarget target,
    ReviewedSourcePackageBaseLease lease);

[[nodiscard]] AlreadyReviewedSourceCheckoutResult
materialize_already_reviewed_source_checkout(
    ReviewedSourceAlreadyReviewedContinue target,
    const ValidatedCachePath& checkout);

[[nodiscard]] AlreadyReviewedSourceCheckoutResult
materialize_already_reviewed_source_checkout_with_lease(
    ReviewedSourceAlreadyReviewedContinue target,
    ReviewedSourcePackageBaseLease lease);

enum class ReviewedSourceEditorOverlayStatus {
    None,
    InvocationLocal,
};

// Consumes exact checkout + accepted authority. A CAS conflict is re-read only
// to classify an exact same-target idempotent success; it is never retried as
// another write. PublishedUncertain is propagated without retry.
[[nodiscard]] ReviewedSourcePublicationResult
publish_accepted_reviewed_source_checkout(
    AcceptedReviewedSourceCheckout target);

// AlreadyReviewed never rewrites state. It re-reads the store after exact
// checkout materialization and produces build authority only while the current
// proven observation is still the same exact target.
[[nodiscard]] ReviewedSourcePublicationResult
confirm_already_reviewed_source_checkout(
    AlreadyReviewedSourceCheckout target);

enum class ReviewedSourcePinnedCheckoutFailureReason {
    InvalidAcceptedCapability,
    InvalidAlreadyReviewedCapability,
    PackageBaseMismatch,
    CheckoutBoundaryInvalid,
    LeaseContended,
    LeaseUnavailable,
    GitMaterializationFailed,
};

struct ReviewedSourcePinnedCheckoutFailure {
    ReviewedSourcePinnedCheckoutFailureReason reason;
    std::optional<std::error_code> system_error;
    std::optional<TrustedCacheFailure> boundary_failure;
    std::optional<TrustedGitPinnedCheckoutFailure> git_failure;

    bool operator==(const ReviewedSourcePinnedCheckoutFailure&) const =
        default;
};

// A successful non-blocking flock acquisition over the exact retained
// PackageBase directory. The descriptor number is not caller evidence: only
// acquire_reviewed_source_package_base_lease() derives and seals it.
class ReviewedSourcePackageBaseLease final {
public:
    ReviewedSourcePackageBaseLease() = delete;
    ReviewedSourcePackageBaseLease(
        const ReviewedSourcePackageBaseLease&) = delete;
    ReviewedSourcePackageBaseLease(
        ReviewedSourcePackageBaseLease&& other) noexcept;
    ReviewedSourcePackageBaseLease& operator=(
        const ReviewedSourcePackageBaseLease&) = delete;
    ReviewedSourcePackageBaseLease& operator=(
        ReviewedSourcePackageBaseLease&&) = delete;
    ~ReviewedSourcePackageBaseLease() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const ValidatedCachePath& path() const;
    [[nodiscard]] std::uintmax_t device() const;
    [[nodiscard]] std::uintmax_t inode() const;
    void require_unchanged_identity() const;
    int run_guarded_command(
        const std::string& command,
        const std::string& display_command = {}) const;

private:
    friend ReviewedSourcePackageBaseLease
    acquire_reviewed_source_package_base_lease(
        RetainedTrustedCacheDirectory directory);
    friend TrustedGitPinnedCheckoutResult
    trusted_git_materialize_pinned_checkout(
        const ValidatedCachePath& checkout,
        AurReviewedSourceReviewIdentity identity,
        const ReviewedSourcePackageBaseLease& lease);
    friend int trusted_git_fetch_origin(
        const ValidatedCachePath& checkout,
        const std::string& expected_remote_url,
        const ReviewedSourcePackageBaseLease& lease);
    friend int trusted_git_reset_hard(
        const ValidatedCachePath& checkout,
        const std::string& expected_remote_url,
        const std::string& branch,
        const ReviewedSourcePackageBaseLease& lease);
    friend int trusted_git_clone_persistent_checkout(
        const ValidatedCachePath& destination,
        const std::string& remote_url,
        const ReviewedSourcePackageBaseLease& lease);
    friend TrustedGitPinnedCheckoutOverlayObservationResult
    observe_clean_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease);
    friend TrustedGitPinnedCheckoutOverlayObservationResult
    observe_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease);
    friend TrustedGitPinnedCheckoutRevalidationResult
    revalidate_trusted_git_pinned_checkout_overlay(
        const TrustedGitPinnedCheckout& checkout,
        const ReviewedSourcePackageBaseLease& lease,
        const TrustedGitPinnedCheckoutOverlayObservation& expected);
    friend class PinnedReviewedSourceBuild;

    ReviewedSourcePackageBaseLease(
        RetainedTrustedCacheDirectory directory,
        int descriptor) noexcept;
    int run_guarded_production_command(
        const std::string& command,
        const std::string& display_command) const;

    RetainedTrustedCacheDirectory directory_;
    int descriptor_ = -1;
    bool valid_ = true;
};

// Throws std::system_error when flock cannot be acquired. The retained
// directory is consumed so every successful capability is one single owner.
[[nodiscard]] ReviewedSourcePackageBaseLease
acquire_reviewed_source_package_base_lease(
    RetainedTrustedCacheDirectory directory);

class AcceptedReviewedSourceCheckout final {
public:
    AcceptedReviewedSourceCheckout() = delete;
    AcceptedReviewedSourceCheckout(
        const AcceptedReviewedSourceCheckout&) = delete;
    AcceptedReviewedSourceCheckout(
        AcceptedReviewedSourceCheckout&& other) noexcept;
    AcceptedReviewedSourceCheckout& operator=(
        const AcceptedReviewedSourceCheckout&) = delete;
    AcceptedReviewedSourceCheckout& operator=(
        AcceptedReviewedSourceCheckout&& other) noexcept;
    ~AcceptedReviewedSourceCheckout();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity() const;
    [[nodiscard]] const ReviewedSourceExpectedStateObservation&
    expected_state_observation() const;
    [[nodiscard]] const ReviewedSourceIntegrationLifecycle& lifecycle()
        const;
    [[nodiscard]] const std::filesystem::path& checkout_path() const;
    [[nodiscard]] std::uintmax_t checkout_device() const;
    [[nodiscard]] std::uintmax_t checkout_inode() const;
    void require_unchanged_checkout_identity() const;
    int run_guarded_command(
        const std::string& command,
        const std::string& display_command = {}) const;

private:
    friend class PinnedReviewedSourceBuild;
    friend AcceptedReviewedSourceCheckoutResult
    materialize_accepted_reviewed_source_checkout(
        AcceptedReviewedSourceTarget target,
        const ValidatedCachePath& checkout);
    friend AcceptedReviewedSourceCheckoutResult
    materialize_accepted_reviewed_source_checkout_with_lease(
        AcceptedReviewedSourceTarget target,
        ReviewedSourcePackageBaseLease lease);
    friend ReviewedSourcePublicationResult
    publish_accepted_reviewed_source_checkout(
        AcceptedReviewedSourceCheckout target);
    friend ReviewedSourceEditorBoundaryResult
    begin_reviewed_source_editor_boundary(
        const AcceptedReviewedSourceCheckout& checkout);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_editor_overlay(
        const AcceptedReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_no_editor_overlay(
        const AcceptedReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourcePublicationResult
    publish_accepted_reviewed_source_checkout_with_editor_overlay(
        AcceptedReviewedSourceCheckout target,
        ReviewedSourceEditorOverlayProof editor_overlay);

    struct State;

    AcceptedReviewedSourceCheckout(
        AcceptedReviewedSourceTarget target,
        ReviewedSourcePackageBaseLease lease,
        TrustedGitPinnedCheckout checkout);

    [[nodiscard]] const State& require_state() const;
    [[nodiscard]] State& require_state();

    std::unique_ptr<State> state_;
};

class AlreadyReviewedSourceCheckout final {
public:
    AlreadyReviewedSourceCheckout() = delete;
    AlreadyReviewedSourceCheckout(
        const AlreadyReviewedSourceCheckout&) = delete;
    AlreadyReviewedSourceCheckout(
        AlreadyReviewedSourceCheckout&& other) noexcept;
    AlreadyReviewedSourceCheckout& operator=(
        const AlreadyReviewedSourceCheckout&) = delete;
    AlreadyReviewedSourceCheckout& operator=(
        AlreadyReviewedSourceCheckout&& other) noexcept;
    ~AlreadyReviewedSourceCheckout();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity() const;
    [[nodiscard]] const ReviewedSourceExpectedStateObservation&
    expected_state_observation() const;
    [[nodiscard]] const ReviewedSourceIntegrationLifecycle& lifecycle()
        const;
    [[nodiscard]] const std::filesystem::path& checkout_path() const;
    [[nodiscard]] std::uintmax_t checkout_device() const;
    [[nodiscard]] std::uintmax_t checkout_inode() const;
    void require_unchanged_checkout_identity() const;
    int run_guarded_command(
        const std::string& command,
        const std::string& display_command = {}) const;

private:
    friend class PinnedReviewedSourceBuild;
    friend AlreadyReviewedSourceCheckoutResult
    materialize_already_reviewed_source_checkout(
        ReviewedSourceAlreadyReviewedContinue target,
        const ValidatedCachePath& checkout);
    friend AlreadyReviewedSourceCheckoutResult
    materialize_already_reviewed_source_checkout_with_lease(
        ReviewedSourceAlreadyReviewedContinue target,
        ReviewedSourcePackageBaseLease lease);
    friend ReviewedSourcePublicationResult
    confirm_already_reviewed_source_checkout(
        AlreadyReviewedSourceCheckout target);
    friend ReviewedSourceEditorBoundaryResult
    begin_reviewed_source_editor_boundary(
        const AlreadyReviewedSourceCheckout& checkout);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_editor_overlay(
        const AlreadyReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_no_editor_overlay(
        const AlreadyReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourcePublicationResult
    confirm_already_reviewed_source_checkout_with_editor_overlay(
        AlreadyReviewedSourceCheckout target,
        ReviewedSourceEditorOverlayProof editor_overlay);

    struct State;

    AlreadyReviewedSourceCheckout(
        ReviewedSourceAlreadyReviewedContinue target,
        ReviewedSourcePackageBaseLease lease,
        TrustedGitPinnedCheckout checkout);

    [[nodiscard]] const State& require_state() const;

    std::unique_ptr<State> state_;
};

// Pre-editor proof. It is minted only from an exact leased checkout and owns
// the clean worktree projection observed immediately before the editor
// boundary. Raw paths, status output, booleans, and overlay enums cannot
// construct it.
class ReviewedSourceEditorBoundary final {
public:
    ReviewedSourceEditorBoundary() = delete;
    ReviewedSourceEditorBoundary(
        const ReviewedSourceEditorBoundary&) = delete;
    ReviewedSourceEditorBoundary(
        ReviewedSourceEditorBoundary&&) noexcept;
    ReviewedSourceEditorBoundary& operator=(
        const ReviewedSourceEditorBoundary&) = delete;
    ReviewedSourceEditorBoundary& operator=(
        ReviewedSourceEditorBoundary&&) noexcept = delete;
    ~ReviewedSourceEditorBoundary();

    [[nodiscard]] bool valid() const noexcept;

private:
    friend ReviewedSourceEditorBoundaryResult
    begin_reviewed_source_editor_boundary(
        const AcceptedReviewedSourceCheckout& checkout);
    friend ReviewedSourceEditorBoundaryResult
    begin_reviewed_source_editor_boundary(
        const AlreadyReviewedSourceCheckout& checkout);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_editor_overlay(
        const AcceptedReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_editor_overlay(
        const AlreadyReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_no_editor_overlay(
        const AcceptedReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_no_editor_overlay(
        const AlreadyReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);

    struct State;

    ReviewedSourceEditorBoundary(
        AurReviewedSourceReviewIdentity identity,
        std::uintmax_t checkout_device,
        std::uintmax_t checkout_inode,
        TrustedGitPinnedCheckoutOverlayObservation pre_editor);

    [[nodiscard]] const State& require_state() const;

    std::unique_ptr<State> state_;
};

// Sealed post-editor worktree authority. It binds PackageBase/source/exact
// reviewed base, checkout device/inode, and both exact Git worktree
// projections. Its status is derived from those projections, never supplied by
// a caller-owned bool or enum.
class ReviewedSourceEditorOverlayProof final {
public:
    ReviewedSourceEditorOverlayProof() = delete;
    ReviewedSourceEditorOverlayProof(
        const ReviewedSourceEditorOverlayProof&) = delete;
    ReviewedSourceEditorOverlayProof(
        ReviewedSourceEditorOverlayProof&&) noexcept;
    ReviewedSourceEditorOverlayProof& operator=(
        const ReviewedSourceEditorOverlayProof&) = delete;
    ReviewedSourceEditorOverlayProof& operator=(
        ReviewedSourceEditorOverlayProof&&) noexcept = delete;
    ~ReviewedSourceEditorOverlayProof();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ReviewedSourceEditorOverlayStatus status() const;

private:
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_editor_overlay(
        const AcceptedReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_editor_overlay(
        const AlreadyReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_no_editor_overlay(
        const AcceptedReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourceEditorOverlayProofResult
    seal_reviewed_source_no_editor_overlay(
        const AlreadyReviewedSourceCheckout& checkout,
        ReviewedSourceEditorBoundary boundary);
    friend ReviewedSourcePublicationResult
    publish_accepted_reviewed_source_checkout_with_editor_overlay(
        AcceptedReviewedSourceCheckout target,
        ReviewedSourceEditorOverlayProof editor_overlay);
    friend ReviewedSourcePublicationResult
    confirm_already_reviewed_source_checkout_with_editor_overlay(
        AlreadyReviewedSourceCheckout target,
        ReviewedSourceEditorOverlayProof editor_overlay);
    friend class PinnedReviewedSourceBuild;

    struct State;

    ReviewedSourceEditorOverlayProof(
        AurReviewedSourceReviewIdentity identity,
        std::uintmax_t checkout_device,
        std::uintmax_t checkout_inode,
        TrustedGitPinnedCheckoutOverlayObservation pre_editor,
        TrustedGitPinnedCheckoutOverlayObservation post_editor);

    [[nodiscard]] const State& require_state() const;

    std::unique_ptr<State> state_;
};

[[nodiscard]] ReviewedSourceEditorBoundaryResult
begin_reviewed_source_editor_boundary(
    const AcceptedReviewedSourceCheckout& checkout);

[[nodiscard]] ReviewedSourceEditorBoundaryResult
begin_reviewed_source_editor_boundary(
    const AlreadyReviewedSourceCheckout& checkout);

// The editor form permits a changed post-state and derives None for a no-op
// editor. The no-editor form requires the exact pre/post projection to match.
[[nodiscard]] ReviewedSourceEditorOverlayProofResult
seal_reviewed_source_editor_overlay(
    const AcceptedReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary);

[[nodiscard]] ReviewedSourceEditorOverlayProofResult
seal_reviewed_source_editor_overlay(
    const AlreadyReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary);

[[nodiscard]] ReviewedSourceEditorOverlayProofResult
seal_reviewed_source_no_editor_overlay(
    const AcceptedReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary);

[[nodiscard]] ReviewedSourceEditorOverlayProofResult
seal_reviewed_source_no_editor_overlay(
    const AlreadyReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary);

[[nodiscard]] ReviewedSourcePublicationResult
publish_accepted_reviewed_source_checkout_with_editor_overlay(
    AcceptedReviewedSourceCheckout target,
    ReviewedSourceEditorOverlayProof editor_overlay);

[[nodiscard]] ReviewedSourcePublicationResult
confirm_already_reviewed_source_checkout_with_editor_overlay(
    AlreadyReviewedSourceCheckout target,
    ReviewedSourceEditorOverlayProof editor_overlay);

enum class ReviewedSourcePublicationStatus {
    Published,
    AlreadyPublishedSameTarget,
};

class PinnedReviewedSourceBuild final {
public:
    PinnedReviewedSourceBuild() = delete;
    PinnedReviewedSourceBuild(const PinnedReviewedSourceBuild&) = delete;
    PinnedReviewedSourceBuild(PinnedReviewedSourceBuild&& other) noexcept;
    PinnedReviewedSourceBuild& operator=(
        const PinnedReviewedSourceBuild&) = delete;
    PinnedReviewedSourceBuild& operator=(
        PinnedReviewedSourceBuild&& other) noexcept;
    ~PinnedReviewedSourceBuild();

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ReviewedSourcePublicationStatus publication_status() const;
    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity() const;
    [[nodiscard]] const SourceRevisionIdentity& reviewed_upstream_base_revision()
        const;
    [[nodiscard]] const ReviewedSourceState& reviewed_state() const;
    [[nodiscard]] const ReviewedSourceStateObservedRecord& published_record()
        const;
    // The reviewed revision is always the exact upstream base. InvocationLocal
    // means the leased worktree differs between the clean pre-editor boundary
    // and the post-editor observation; it does not claim OS-level authorship.
    [[nodiscard]] ReviewedSourceEditorOverlayStatus editor_overlay_status()
        const;
    [[nodiscard]] const std::filesystem::path& checkout_path() const;
    [[nodiscard]] std::uintmax_t checkout_device() const;
    [[nodiscard]] std::uintmax_t checkout_inode() const;
    void require_unchanged_checkout_identity() const;
    int run_guarded_command(
        const std::string& command,
        const std::string& display_command = {}) const;

private:
    friend class InvocationOwnedSourceBuildContextAuthority;
    friend ReviewedSourcePublicationResult
    publish_accepted_reviewed_source_checkout(
        AcceptedReviewedSourceCheckout target);
    friend ReviewedSourcePublicationResult
    publish_accepted_reviewed_source_checkout_with_editor_overlay(
        AcceptedReviewedSourceCheckout target,
        ReviewedSourceEditorOverlayProof editor_overlay);
    friend ReviewedSourcePublicationResult
    confirm_already_reviewed_source_checkout(
        AlreadyReviewedSourceCheckout target);
    friend ReviewedSourcePublicationResult
    confirm_already_reviewed_source_checkout_with_editor_overlay(
        AlreadyReviewedSourceCheckout target,
        ReviewedSourceEditorOverlayProof editor_overlay);

    struct State;

    PinnedReviewedSourceBuild(
        AcceptedReviewedSourceCheckout checkout,
        ReviewedSourcePublicationStatus publication_status,
        ReviewedSourceState state,
        ReviewedSourceStateObservedRecord observed,
        ReviewedSourceEditorOverlayProof editor_overlay);
    PinnedReviewedSourceBuild(
        AlreadyReviewedSourceCheckout checkout,
        ReviewedSourceState state,
        ReviewedSourceStateObservedRecord observed,
        ReviewedSourceEditorOverlayProof editor_overlay);

    [[nodiscard]] const State& require_state() const;
    [[nodiscard]] TrustedGitReviewedRecipeSnapshotResult
    project_reviewed_recipe_snapshot() const;
    [[nodiscard]] TrustedGitPinnedCheckoutRevalidationResult
    revalidate_reviewed_recipe_source() const;

    std::unique_ptr<State> state_;
};

// Dynamic makepkg-boundary failure. The PinnedReviewedSourceBuild remains the
// lease owner, but no external command is invoked when its sealed overlay no
// longer matches.
class ReviewedSourceBuildCheckoutReproofError final
    : public std::runtime_error {
public:
    explicit ReviewedSourceBuildCheckoutReproofError(
        TrustedGitPinnedCheckoutFailure failure);

    [[nodiscard]] const TrustedGitPinnedCheckoutFailure& failure()
        const noexcept;

private:
    TrustedGitPinnedCheckoutFailure failure_;
};

// The external command has completed and its exact status is known, but a
// production source-build identity check performed before returning it failed.
// Pre-command failures never construct this transport.
class ProductionSourceBuildPostCommandRevalidationError final
    : public std::runtime_error {
public:
    ProductionSourceBuildPostCommandRevalidationError(
        int command_exit_status,
        std::exception_ptr failure_exception)
        : std::runtime_error(diagnostic(failure_exception)),
          command_exit_status_(command_exit_status),
          failure_exception_(std::move(failure_exception)) {
    }

    [[nodiscard]] int command_exit_status() const noexcept {
        return command_exit_status_;
    }

    void rethrow_failure() const {
        if(failure_exception_ == nullptr) {
            throw std::logic_error(
                "Post-command revalidation failure has no nested exception.");
        }
        std::rethrow_exception(failure_exception_);
    }

private:
    static std::string diagnostic(
        const std::exception_ptr& failure_exception) {
        if(failure_exception == nullptr) {
            return {};
        }
        try {
            std::rethrow_exception(failure_exception);
        } catch(const std::exception& error) {
            return error.what();
        } catch(...) {
            return {};
        }
    }

    int command_exit_status_ = 0;
    std::exception_ptr failure_exception_;
};

struct ReviewedSourcePublicationConflict {
    AurReviewedSourceReviewIdentity identity;
    ReviewedSourceStateStoreRead current;
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None;
};

struct ReviewedSourcePublicationUncertain {
    ReviewedSourceStateStorePublishedUncertain store_result;
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None;
};

struct ReviewedSourcePublicationUnsafeHistory {
    ReviewedSourceStateStoreUnsafeHistory store_result;
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None;
};

enum class ReviewedSourcePublicationFailureReason {
    InvalidCheckoutCapability,
    StoreFailure,
    ConflictObservationFailure,
    CheckoutRevalidationFailed,
    InconsistentStoreResult,
};

struct ReviewedSourcePublicationFailure {
    ReviewedSourcePublicationFailureReason reason;
    std::optional<ReviewedSourceStateStoreFailure> store_failure;
    std::optional<TrustedGitPinnedCheckoutFailure> checkout_failure;
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None;
};

// A successful/confirmed state result is never rolled back merely because the
// leased checkout lost its exact proof before pinned authority could be
// returned. This arm reports both facts and carries no build capability.
struct ReviewedSourcePostPublicationCheckoutFailure {
    ReviewedSourcePublicationStatus publication_status;
    ReviewedSourceState state;
    ReviewedSourceStateObservedRecord observed;
    TrustedGitPinnedCheckoutFailure checkout_failure;
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None;
};
