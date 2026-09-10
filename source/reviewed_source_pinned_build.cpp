#include "reviewed_source_pinned_build.hpp"

#include <cerrno>
#include <stdexcept>
#include <utility>

struct AcceptedReviewedSourceCheckout::State {
    AcceptedReviewedSourceTarget target;
    ReviewedSourcePackageBaseLease lease;
    TrustedGitPinnedCheckout checkout;

    State(
        AcceptedReviewedSourceTarget value_target,
        ReviewedSourcePackageBaseLease value_lease,
        TrustedGitPinnedCheckout value_checkout)
        : target(std::move(value_target)), lease(std::move(value_lease)),
          checkout(std::move(value_checkout)) {
    }
};

struct AlreadyReviewedSourceCheckout::State {
    ReviewedSourceAlreadyReviewedContinue target;
    ReviewedSourcePackageBaseLease lease;
    TrustedGitPinnedCheckout checkout;

    State(
        ReviewedSourceAlreadyReviewedContinue value_target,
        ReviewedSourcePackageBaseLease value_lease,
        TrustedGitPinnedCheckout value_checkout)
        : target(std::move(value_target)), lease(std::move(value_lease)),
          checkout(std::move(value_checkout)) {
    }
};

using ReviewedSourceCheckoutAuthority = std::variant<
    AcceptedReviewedSourceCheckout,
    AlreadyReviewedSourceCheckout>;

struct ReviewedSourceEditorBoundary::State {
    AurReviewedSourceReviewIdentity identity;
    std::uintmax_t checkout_device = 0;
    std::uintmax_t checkout_inode = 0;
    TrustedGitPinnedCheckoutOverlayObservation pre_editor;

    State(
        AurReviewedSourceReviewIdentity value_identity,
        std::uintmax_t value_checkout_device,
        std::uintmax_t value_checkout_inode,
        TrustedGitPinnedCheckoutOverlayObservation value_pre_editor)
        : identity(std::move(value_identity)),
          checkout_device(value_checkout_device),
          checkout_inode(value_checkout_inode),
          pre_editor(std::move(value_pre_editor)) {
    }
};

struct ReviewedSourceEditorOverlayProof::State {
    AurReviewedSourceReviewIdentity identity;
    std::uintmax_t checkout_device = 0;
    std::uintmax_t checkout_inode = 0;
    TrustedGitPinnedCheckoutOverlayObservation pre_editor;
    TrustedGitPinnedCheckoutOverlayObservation post_editor;
    ReviewedSourceEditorOverlayStatus status =
        ReviewedSourceEditorOverlayStatus::None;

    State(
        AurReviewedSourceReviewIdentity value_identity,
        std::uintmax_t value_checkout_device,
        std::uintmax_t value_checkout_inode,
        TrustedGitPinnedCheckoutOverlayObservation value_pre_editor,
        TrustedGitPinnedCheckoutOverlayObservation value_post_editor)
        : identity(std::move(value_identity)),
          checkout_device(value_checkout_device),
          checkout_inode(value_checkout_inode),
          pre_editor(std::move(value_pre_editor)),
          post_editor(std::move(value_post_editor)),
          status(pre_editor == post_editor
                     ? ReviewedSourceEditorOverlayStatus::None
                     : ReviewedSourceEditorOverlayStatus::
                           InvocationLocal) {
    }
};

struct PinnedReviewedSourceBuild::State {
    ReviewedSourceCheckoutAuthority checkout;
    ReviewedSourcePublicationStatus publication_status;
    ReviewedSourceState state;
    ReviewedSourceStateObservedRecord observed;
    ReviewedSourceEditorOverlayProof editor_overlay;

    State(
        ReviewedSourceCheckoutAuthority value_checkout,
        ReviewedSourcePublicationStatus value_publication_status,
        ReviewedSourceState value_state,
        ReviewedSourceStateObservedRecord value_observed,
        ReviewedSourceEditorOverlayProof value_editor_overlay)
        : checkout(std::move(value_checkout)),
          publication_status(value_publication_status),
          state(std::move(value_state)), observed(std::move(value_observed)),
          editor_overlay(std::move(value_editor_overlay)) {
    }
};

namespace {

ReviewedSourcePinnedCheckoutFailure checkout_failure(
    ReviewedSourcePinnedCheckoutFailureReason reason) {
    return ReviewedSourcePinnedCheckoutFailure{
        reason, std::nullopt, std::nullopt, std::nullopt};
}

ReviewedSourcePublicationFailure publication_failure(
    ReviewedSourcePublicationFailureReason reason) {
    return ReviewedSourcePublicationFailure{
        reason, std::nullopt, std::nullopt};
}

ReviewedSourcePublicationFailure store_publication_failure(
    ReviewedSourcePublicationFailureReason reason,
    ReviewedSourceStateStoreFailure failure) {
    ReviewedSourcePublicationFailure result = publication_failure(reason);
    result.store_failure = std::move(failure);
    return result;
}

ReviewedSourcePublicationFailure checkout_publication_failure(
    TrustedGitPinnedCheckoutFailure failure) {
    ReviewedSourcePublicationFailure result = publication_failure(
        ReviewedSourcePublicationFailureReason::
            CheckoutRevalidationFailed);
    result.checkout_failure = std::move(failure);
    return result;
}

ReviewedSourcePublicationFailure with_editor_overlay(
    ReviewedSourcePublicationFailure failure,
    ReviewedSourceEditorOverlayStatus editor_overlay) {
    failure.editor_overlay = editor_overlay;
    return failure;
}

bool checkout_name_matches(
    const ValidatedCachePath& checkout,
    const AurReviewedSourceReviewIdentity& identity) {
    return checkout.exists() && checkout.is_directory() &&
           checkout.canonical_path().filename() ==
               identity.package_base().package_base();
}

struct LeasedPinnedCheckout {
    ReviewedSourcePackageBaseLease lease;
    TrustedGitPinnedCheckout checkout;
};

using LeasedPinnedCheckoutResult = std::variant<
    LeasedPinnedCheckout,
    ReviewedSourcePinnedCheckoutFailure>;

LeasedPinnedCheckoutResult materialize_leased_pinned_checkout(
    const AurReviewedSourceReviewIdentity& identity,
    ReviewedSourcePackageBaseLease lease) {
    if(!lease.valid() || !checkout_name_matches(lease.path(), identity)) {
        return checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                PackageBaseMismatch);
    }

    try {
        lease.require_unchanged_identity();

        const ValidatedCachePath current = revalidate_trusted_cache_path(
            lease.path(), CachePathRequirement::ExistingDirectory);
        if(current.device() != lease.device() ||
           current.inode() != lease.inode()) {
            return checkout_failure(
                ReviewedSourcePinnedCheckoutFailureReason::
                    CheckoutBoundaryInvalid);
        }

        TrustedGitPinnedCheckoutResult materialized =
            trusted_git_materialize_pinned_checkout(
                current, identity, lease);
        if(auto* failure =
               std::get_if<TrustedGitPinnedCheckoutFailure>(
                   &materialized)) {
            ReviewedSourcePinnedCheckoutFailure result = checkout_failure(
                ReviewedSourcePinnedCheckoutFailureReason::
                    GitMaterializationFailed);
            result.git_failure = std::move(*failure);
            return result;
        }
        return LeasedPinnedCheckout{
            std::move(lease),
            std::move(std::get<TrustedGitPinnedCheckout>(materialized))};
    } catch(const std::system_error& error) {
        const int lock_error = error.code().value();
        ReviewedSourcePinnedCheckoutFailure failure = checkout_failure(
            lock_error == EWOULDBLOCK || lock_error == EAGAIN
                ? ReviewedSourcePinnedCheckoutFailureReason::
                      LeaseContended
                : ReviewedSourcePinnedCheckoutFailureReason::
                      LeaseUnavailable);
        failure.system_error = error.code();
        return failure;
    } catch(const TrustedCacheError& error) {
        ReviewedSourcePinnedCheckoutFailure failure = checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                CheckoutBoundaryInvalid);
        failure.boundary_failure = error.failure();
        return failure;
    }
}

LeasedPinnedCheckoutResult acquire_leased_pinned_checkout(
    const AurReviewedSourceReviewIdentity& identity,
    const ValidatedCachePath& checkout) {
    if(!checkout_name_matches(checkout, identity)) {
        return checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                PackageBaseMismatch);
    }

    try {
        RetainedTrustedCacheDirectory retained =
            retain_trusted_cache_directory(checkout);
        return materialize_leased_pinned_checkout(
            identity,
            acquire_reviewed_source_package_base_lease(
                std::move(retained)));
    } catch(const std::system_error& error) {
        const int lock_error = error.code().value();
        ReviewedSourcePinnedCheckoutFailure failure = checkout_failure(
            lock_error == EWOULDBLOCK || lock_error == EAGAIN
                ? ReviewedSourcePinnedCheckoutFailureReason::
                      LeaseContended
                : ReviewedSourcePinnedCheckoutFailureReason::
                      LeaseUnavailable);
        failure.system_error = error.code();
        return failure;
    } catch(const TrustedCacheError& error) {
        ReviewedSourcePinnedCheckoutFailure failure = checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                CheckoutBoundaryInvalid);
        failure.boundary_failure = error.failure();
        return failure;
    }
}

} // namespace

AcceptedReviewedSourceCheckout::AcceptedReviewedSourceCheckout(
    AcceptedReviewedSourceTarget target,
    ReviewedSourcePackageBaseLease lease,
    TrustedGitPinnedCheckout checkout) {
    if(!target.valid() || !checkout.valid() ||
       target.identity() != checkout.identity() ||
       lease.device() != checkout.checkout_device() ||
       lease.inode() != checkout.checkout_inode()) {
        throw std::logic_error(
            "Accepted reviewed source checkout binding is inconsistent.");
    }
    state_ = std::make_unique<State>(
        std::move(target), std::move(lease), std::move(checkout));
}

AcceptedReviewedSourceCheckout::AcceptedReviewedSourceCheckout(
    AcceptedReviewedSourceCheckout&& other) noexcept = default;

AcceptedReviewedSourceCheckout& AcceptedReviewedSourceCheckout::operator=(
    AcceptedReviewedSourceCheckout&& other) noexcept = default;

AcceptedReviewedSourceCheckout::~AcceptedReviewedSourceCheckout() = default;

bool AcceptedReviewedSourceCheckout::valid() const noexcept {
    return state_ != nullptr;
}

const AcceptedReviewedSourceCheckout::State&
AcceptedReviewedSourceCheckout::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from accepted reviewed source checkout has no authority.");
    }
    return *state_;
}

AcceptedReviewedSourceCheckout::State&
AcceptedReviewedSourceCheckout::require_state() {
    if(!state_) {
        throw std::logic_error(
            "A moved-from accepted reviewed source checkout has no authority.");
    }
    return *state_;
}

const AurReviewedSourceReviewIdentity&
AcceptedReviewedSourceCheckout::identity() const {
    return require_state().target.identity();
}

const ReviewedSourceExpectedStateObservation&
AcceptedReviewedSourceCheckout::expected_state_observation() const {
    return require_state().target.expected_state_observation();
}

const ReviewedSourceIntegrationLifecycle&
AcceptedReviewedSourceCheckout::lifecycle() const {
    return require_state().target.lifecycle();
}

const std::filesystem::path&
AcceptedReviewedSourceCheckout::checkout_path() const {
    return require_state().checkout.checkout_path();
}

std::uintmax_t AcceptedReviewedSourceCheckout::checkout_device() const {
    return require_state().checkout.checkout_device();
}

std::uintmax_t AcceptedReviewedSourceCheckout::checkout_inode() const {
    return require_state().checkout.checkout_inode();
}

void AcceptedReviewedSourceCheckout::require_unchanged_checkout_identity()
    const {
    require_state().lease.require_unchanged_identity();
}

int AcceptedReviewedSourceCheckout::run_guarded_command(
    const std::string& command,
    const std::string& display_command) const {
    return require_state().lease.run_guarded_command(
        command, display_command);
}

AcceptedReviewedSourceCheckoutResult
materialize_accepted_reviewed_source_checkout(
    AcceptedReviewedSourceTarget target,
    const ValidatedCachePath& checkout) {
    if(!target.valid()) {
        return checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                InvalidAcceptedCapability);
    }
    LeasedPinnedCheckoutResult acquired = acquire_leased_pinned_checkout(
        target.identity(), checkout);
    if(auto* failure = std::get_if<ReviewedSourcePinnedCheckoutFailure>(
           &acquired)) {
        return std::move(*failure);
    }
    LeasedPinnedCheckout authority =
        std::move(std::get<LeasedPinnedCheckout>(acquired));
    return AcceptedReviewedSourceCheckout(
        std::move(target), std::move(authority.lease),
        std::move(authority.checkout));
}

AcceptedReviewedSourceCheckoutResult
materialize_accepted_reviewed_source_checkout_with_lease(
    AcceptedReviewedSourceTarget target,
    ReviewedSourcePackageBaseLease lease) {
    if(!target.valid()) {
        return checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                InvalidAcceptedCapability);
    }
    LeasedPinnedCheckoutResult acquired = materialize_leased_pinned_checkout(
        target.identity(), std::move(lease));
    if(auto* failure = std::get_if<ReviewedSourcePinnedCheckoutFailure>(
           &acquired)) {
        return std::move(*failure);
    }
    LeasedPinnedCheckout authority =
        std::move(std::get<LeasedPinnedCheckout>(acquired));
    return AcceptedReviewedSourceCheckout(
        std::move(target), std::move(authority.lease),
        std::move(authority.checkout));
}

AlreadyReviewedSourceCheckout::AlreadyReviewedSourceCheckout(
    ReviewedSourceAlreadyReviewedContinue target,
    ReviewedSourcePackageBaseLease lease,
    TrustedGitPinnedCheckout checkout) {
    if(!target.valid() || !checkout.valid() ||
       target.identity() != checkout.identity() ||
       lease.device() != checkout.checkout_device() ||
       lease.inode() != checkout.checkout_inode()) {
        throw std::logic_error(
            "Already-reviewed source checkout binding is inconsistent.");
    }
    state_ = std::make_unique<State>(
        std::move(target), std::move(lease), std::move(checkout));
}

AlreadyReviewedSourceCheckout::AlreadyReviewedSourceCheckout(
    AlreadyReviewedSourceCheckout&& other) noexcept = default;

AlreadyReviewedSourceCheckout& AlreadyReviewedSourceCheckout::operator=(
    AlreadyReviewedSourceCheckout&& other) noexcept = default;

AlreadyReviewedSourceCheckout::~AlreadyReviewedSourceCheckout() = default;

bool AlreadyReviewedSourceCheckout::valid() const noexcept {
    return state_ != nullptr;
}

const AlreadyReviewedSourceCheckout::State&
AlreadyReviewedSourceCheckout::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from already-reviewed checkout has no authority.");
    }
    return *state_;
}

const AurReviewedSourceReviewIdentity&
AlreadyReviewedSourceCheckout::identity() const {
    return require_state().target.identity();
}

const ReviewedSourceExpectedStateObservation&
AlreadyReviewedSourceCheckout::expected_state_observation() const {
    return require_state().target.expected_state_observation();
}

const ReviewedSourceIntegrationLifecycle&
AlreadyReviewedSourceCheckout::lifecycle() const {
    return require_state().target.lifecycle();
}

const std::filesystem::path&
AlreadyReviewedSourceCheckout::checkout_path() const {
    return require_state().checkout.checkout_path();
}

std::uintmax_t AlreadyReviewedSourceCheckout::checkout_device() const {
    return require_state().checkout.checkout_device();
}

std::uintmax_t AlreadyReviewedSourceCheckout::checkout_inode() const {
    return require_state().checkout.checkout_inode();
}

void AlreadyReviewedSourceCheckout::require_unchanged_checkout_identity()
    const {
    require_state().lease.require_unchanged_identity();
}

int AlreadyReviewedSourceCheckout::run_guarded_command(
    const std::string& command,
    const std::string& display_command) const {
    return require_state().lease.run_guarded_command(
        command, display_command);
}

AlreadyReviewedSourceCheckoutResult
materialize_already_reviewed_source_checkout(
    ReviewedSourceAlreadyReviewedContinue target,
    const ValidatedCachePath& checkout) {
    if(!target.valid()) {
        return checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                InvalidAlreadyReviewedCapability);
    }
    LeasedPinnedCheckoutResult acquired = acquire_leased_pinned_checkout(
        target.identity(), checkout);
    if(auto* failure = std::get_if<ReviewedSourcePinnedCheckoutFailure>(
           &acquired)) {
        return std::move(*failure);
    }
    LeasedPinnedCheckout authority =
        std::move(std::get<LeasedPinnedCheckout>(acquired));
    return AlreadyReviewedSourceCheckout(
        std::move(target), std::move(authority.lease),
        std::move(authority.checkout));
}

AlreadyReviewedSourceCheckoutResult
materialize_already_reviewed_source_checkout_with_lease(
    ReviewedSourceAlreadyReviewedContinue target,
    ReviewedSourcePackageBaseLease lease) {
    if(!target.valid()) {
        return checkout_failure(
            ReviewedSourcePinnedCheckoutFailureReason::
                InvalidAlreadyReviewedCapability);
    }
    LeasedPinnedCheckoutResult acquired = materialize_leased_pinned_checkout(
        target.identity(), std::move(lease));
    if(auto* failure = std::get_if<ReviewedSourcePinnedCheckoutFailure>(
           &acquired)) {
        return std::move(*failure);
    }
    LeasedPinnedCheckout authority =
        std::move(std::get<LeasedPinnedCheckout>(acquired));
    return AlreadyReviewedSourceCheckout(
        std::move(target), std::move(authority.lease),
        std::move(authority.checkout));
}

ReviewedSourceEditorBoundary::ReviewedSourceEditorBoundary(
    AurReviewedSourceReviewIdentity identity,
    std::uintmax_t checkout_device,
    std::uintmax_t checkout_inode,
    TrustedGitPinnedCheckoutOverlayObservation pre_editor)
    : state_(std::make_unique<State>(
          std::move(identity), checkout_device, checkout_inode,
          std::move(pre_editor))) {
}

ReviewedSourceEditorBoundary::ReviewedSourceEditorBoundary(
    ReviewedSourceEditorBoundary&& other) noexcept = default;

ReviewedSourceEditorBoundary::~ReviewedSourceEditorBoundary() = default;

bool ReviewedSourceEditorBoundary::valid() const noexcept {
    return state_ != nullptr;
}

const ReviewedSourceEditorBoundary::State&
ReviewedSourceEditorBoundary::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from reviewed source editor boundary has no authority.");
    }
    return *state_;
}

ReviewedSourceEditorOverlayProof::ReviewedSourceEditorOverlayProof(
    AurReviewedSourceReviewIdentity identity,
    std::uintmax_t checkout_device,
    std::uintmax_t checkout_inode,
    TrustedGitPinnedCheckoutOverlayObservation pre_editor,
    TrustedGitPinnedCheckoutOverlayObservation post_editor)
    : state_(std::make_unique<State>(
          std::move(identity), checkout_device, checkout_inode,
          std::move(pre_editor), std::move(post_editor))) {
}

ReviewedSourceEditorOverlayProof::ReviewedSourceEditorOverlayProof(
    ReviewedSourceEditorOverlayProof&& other) noexcept = default;

ReviewedSourceEditorOverlayProof::~ReviewedSourceEditorOverlayProof() =
    default;

bool ReviewedSourceEditorOverlayProof::valid() const noexcept {
    return state_ != nullptr;
}

const ReviewedSourceEditorOverlayProof::State&
ReviewedSourceEditorOverlayProof::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from reviewed source editor overlay has no authority.");
    }
    return *state_;
}

ReviewedSourceEditorOverlayStatus
ReviewedSourceEditorOverlayProof::status() const {
    return require_state().status;
}

ReviewedSourceEditorBoundaryResult
begin_reviewed_source_editor_boundary(
    const AcceptedReviewedSourceCheckout& checkout) {
    if(!checkout.valid()) {
        return publication_failure(
            ReviewedSourcePublicationFailureReason::
                InvalidCheckoutCapability);
    }
    const AcceptedReviewedSourceCheckout::State& state =
        checkout.require_state();
    TrustedGitPinnedCheckoutOverlayObservationResult observed =
        observe_clean_trusted_git_pinned_checkout_overlay(
            state.checkout, state.lease);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &observed)) {
        return checkout_publication_failure(std::move(*failure));
    }
    return ReviewedSourceEditorBoundary(
        checkout.identity(), checkout.checkout_device(),
        checkout.checkout_inode(),
        std::get<TrustedGitPinnedCheckoutOverlayObservation>(
            std::move(observed)));
}

ReviewedSourceEditorBoundaryResult
begin_reviewed_source_editor_boundary(
    const AlreadyReviewedSourceCheckout& checkout) {
    if(!checkout.valid()) {
        return publication_failure(
            ReviewedSourcePublicationFailureReason::
                InvalidCheckoutCapability);
    }
    const AlreadyReviewedSourceCheckout::State& state =
        checkout.require_state();
    TrustedGitPinnedCheckoutOverlayObservationResult observed =
        observe_clean_trusted_git_pinned_checkout_overlay(
            state.checkout, state.lease);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &observed)) {
        return checkout_publication_failure(std::move(*failure));
    }
    return ReviewedSourceEditorBoundary(
        checkout.identity(), checkout.checkout_device(),
        checkout.checkout_inode(),
        std::get<TrustedGitPinnedCheckoutOverlayObservation>(
            std::move(observed)));
}

ReviewedSourceEditorOverlayProofResult seal_reviewed_source_editor_overlay(
    const AcceptedReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary) {
    if(!checkout.valid() || !boundary.valid()) {
        return publication_failure(
            ReviewedSourcePublicationFailureReason::
                InvalidCheckoutCapability);
    }
    const ReviewedSourceEditorBoundary::State& pre =
        boundary.require_state();
    if(pre.identity != checkout.identity() ||
       pre.checkout_device != checkout.checkout_device() ||
       pre.checkout_inode != checkout.checkout_inode()) {
        return publication_failure(
            ReviewedSourcePublicationFailureReason::
                InvalidCheckoutCapability);
    }
    const AcceptedReviewedSourceCheckout::State& state =
        checkout.require_state();
    TrustedGitPinnedCheckoutOverlayObservationResult observed =
        observe_trusted_git_pinned_checkout_overlay(
            state.checkout, state.lease);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &observed)) {
        return checkout_publication_failure(std::move(*failure));
    }
    return ReviewedSourceEditorOverlayProof(
        pre.identity, pre.checkout_device, pre.checkout_inode,
        pre.pre_editor,
        std::get<TrustedGitPinnedCheckoutOverlayObservation>(
            std::move(observed)));
}

ReviewedSourceEditorOverlayProofResult seal_reviewed_source_editor_overlay(
    const AlreadyReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary) {
    if(!checkout.valid() || !boundary.valid()) {
        return publication_failure(
            ReviewedSourcePublicationFailureReason::
                InvalidCheckoutCapability);
    }
    const ReviewedSourceEditorBoundary::State& pre =
        boundary.require_state();
    if(pre.identity != checkout.identity() ||
       pre.checkout_device != checkout.checkout_device() ||
       pre.checkout_inode != checkout.checkout_inode()) {
        return publication_failure(
            ReviewedSourcePublicationFailureReason::
                InvalidCheckoutCapability);
    }
    const AlreadyReviewedSourceCheckout::State& state =
        checkout.require_state();
    TrustedGitPinnedCheckoutOverlayObservationResult observed =
        observe_trusted_git_pinned_checkout_overlay(
            state.checkout, state.lease);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &observed)) {
        return checkout_publication_failure(std::move(*failure));
    }
    return ReviewedSourceEditorOverlayProof(
        pre.identity, pre.checkout_device, pre.checkout_inode,
        pre.pre_editor,
        std::get<TrustedGitPinnedCheckoutOverlayObservation>(
            std::move(observed)));
}

ReviewedSourceEditorOverlayProofResult seal_reviewed_source_no_editor_overlay(
    const AcceptedReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary) {
    ReviewedSourceEditorOverlayProofResult sealed =
        seal_reviewed_source_editor_overlay(
            checkout, std::move(boundary));
    if(auto* proof = std::get_if<ReviewedSourceEditorOverlayProof>(&sealed)) {
        if(proof->status() != ReviewedSourceEditorOverlayStatus::None) {
            return checkout_publication_failure(
                TrustedGitPinnedCheckoutFailure{
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch,
                    TrustedGitPinnedCheckoutStage::
                        OverlayObservation,
                    std::nullopt, 0, 0, std::nullopt});
        }
    }
    return sealed;
}

ReviewedSourceEditorOverlayProofResult seal_reviewed_source_no_editor_overlay(
    const AlreadyReviewedSourceCheckout& checkout,
    ReviewedSourceEditorBoundary boundary) {
    ReviewedSourceEditorOverlayProofResult sealed =
        seal_reviewed_source_editor_overlay(
            checkout, std::move(boundary));
    if(auto* proof = std::get_if<ReviewedSourceEditorOverlayProof>(&sealed)) {
        if(proof->status() != ReviewedSourceEditorOverlayStatus::None) {
            return checkout_publication_failure(
                TrustedGitPinnedCheckoutFailure{
                    TrustedGitPinnedCheckoutFailureReason::
                        OverlayMismatch,
                    TrustedGitPinnedCheckoutStage::
                        OverlayObservation,
                    std::nullopt, 0, 0, std::nullopt});
        }
    }
    return sealed;
}

PinnedReviewedSourceBuild::PinnedReviewedSourceBuild(
    AcceptedReviewedSourceCheckout checkout,
    ReviewedSourcePublicationStatus publication_status,
    ReviewedSourceState state,
    ReviewedSourceStateObservedRecord observed,
    ReviewedSourceEditorOverlayProof editor_overlay) {
    if(!checkout.valid() || state.package_base() != checkout.identity().package_base() ||
       state.reviewed_revision() != checkout.identity().target_revision() ||
       !editor_overlay.valid() ||
       editor_overlay.require_state().identity != checkout.identity() ||
       editor_overlay.require_state().checkout_device !=
           checkout.checkout_device() ||
       editor_overlay.require_state().checkout_inode !=
           checkout.checkout_inode()) {
        throw std::logic_error(
            "Pinned reviewed source build binding is inconsistent.");
    }
    state_ = std::make_unique<State>(
        std::move(checkout), publication_status, std::move(state),
        std::move(observed), std::move(editor_overlay));
}

PinnedReviewedSourceBuild::PinnedReviewedSourceBuild(
    AlreadyReviewedSourceCheckout checkout,
    ReviewedSourceState state,
    ReviewedSourceStateObservedRecord observed,
    ReviewedSourceEditorOverlayProof editor_overlay) {
    if(!checkout.valid() ||
       state.package_base() != checkout.identity().package_base() ||
       state.reviewed_revision() != checkout.identity().target_revision() ||
       !editor_overlay.valid() ||
       editor_overlay.require_state().identity != checkout.identity() ||
       editor_overlay.require_state().checkout_device !=
           checkout.checkout_device() ||
       editor_overlay.require_state().checkout_inode !=
           checkout.checkout_inode()) {
        throw std::logic_error(
            "Pinned already-reviewed source build binding is inconsistent.");
    }
    state_ = std::make_unique<State>(
        std::move(checkout),
        ReviewedSourcePublicationStatus::AlreadyPublishedSameTarget,
        std::move(state), std::move(observed),
        std::move(editor_overlay));
}

PinnedReviewedSourceBuild::PinnedReviewedSourceBuild(
    PinnedReviewedSourceBuild&& other) noexcept = default;

PinnedReviewedSourceBuild& PinnedReviewedSourceBuild::operator=(
    PinnedReviewedSourceBuild&& other) noexcept = default;

PinnedReviewedSourceBuild::~PinnedReviewedSourceBuild() = default;

bool PinnedReviewedSourceBuild::valid() const noexcept {
    return state_ != nullptr;
}

const PinnedReviewedSourceBuild::State&
PinnedReviewedSourceBuild::require_state() const {
    if(!state_) {
        throw std::logic_error(
            "A moved-from pinned reviewed source build has no authority.");
    }
    return *state_;
}

ReviewedSourcePublicationStatus
PinnedReviewedSourceBuild::publication_status() const {
    return require_state().publication_status;
}

const AurReviewedSourceReviewIdentity&
PinnedReviewedSourceBuild::identity() const {
    return std::visit(
        [](const auto& checkout)
            -> const AurReviewedSourceReviewIdentity& {
            return checkout.identity();
        },
        require_state().checkout);
}

const SourceRevisionIdentity&
PinnedReviewedSourceBuild::reviewed_upstream_base_revision() const {
    return identity().target_revision();
}

const ReviewedSourceState&
PinnedReviewedSourceBuild::reviewed_state() const {
    return require_state().state;
}

const ReviewedSourceStateObservedRecord&
PinnedReviewedSourceBuild::published_record() const {
    return require_state().observed;
}

ReviewedSourceEditorOverlayStatus
PinnedReviewedSourceBuild::editor_overlay_status() const {
    return require_state().editor_overlay.status();
}

const std::filesystem::path&
PinnedReviewedSourceBuild::checkout_path() const {
    return std::visit(
        [](const auto& checkout) -> const std::filesystem::path& {
            return checkout.checkout_path();
        },
        require_state().checkout);
}

std::uintmax_t PinnedReviewedSourceBuild::checkout_device() const {
    return std::visit(
        [](const auto& checkout) {
            return checkout.checkout_device();
        },
        require_state().checkout);
}

std::uintmax_t PinnedReviewedSourceBuild::checkout_inode() const {
    return std::visit(
        [](const auto& checkout) {
            return checkout.checkout_inode();
        },
        require_state().checkout);
}

void PinnedReviewedSourceBuild::require_unchanged_checkout_identity() const {
    std::visit(
        [](const auto& checkout) {
            checkout.require_unchanged_checkout_identity();
        },
        require_state().checkout);
}

TrustedGitReviewedRecipeSnapshotResult
PinnedReviewedSourceBuild::project_reviewed_recipe_snapshot() const {
    const ReviewedSourceEditorOverlayProof::State& overlay =
        require_state().editor_overlay.require_state();
    return std::visit(
        [&overlay](const auto& checkout) {
            const auto& checkout_state = checkout.require_state();
            return trusted_git_project_reviewed_recipe_snapshot(
                checkout_state.checkout, checkout_state.lease,
                overlay.post_editor);
        },
        require_state().checkout);
}

TrustedGitPinnedCheckoutRevalidationResult
PinnedReviewedSourceBuild::revalidate_reviewed_recipe_source() const {
    const ReviewedSourceEditorOverlayProof::State& overlay =
        require_state().editor_overlay.require_state();
    return std::visit(
        [&overlay](const auto& checkout) {
            const auto& checkout_state = checkout.require_state();
            return revalidate_trusted_git_pinned_checkout_overlay(
                checkout_state.checkout, checkout_state.lease,
                overlay.post_editor);
        },
        require_state().checkout);
}

int PinnedReviewedSourceBuild::run_guarded_command(
    const std::string& command,
    const std::string& display_command) const {
    const ReviewedSourceEditorOverlayProof::State& overlay =
        require_state().editor_overlay.require_state();
    return std::visit(
        [&command, &display_command, &overlay](const auto& checkout) {
            const auto& checkout_state = checkout.require_state();
            TrustedGitPinnedCheckoutRevalidationResult revalidated =
                revalidate_trusted_git_pinned_checkout_overlay(
                    checkout_state.checkout,
                    checkout_state.lease,
                    overlay.post_editor);
            if(auto* failure =
                   std::get_if<TrustedGitPinnedCheckoutFailure>(
                       &revalidated)) {
                throw ReviewedSourceBuildCheckoutReproofError(
                    std::move(*failure));
            }
            return checkout_state.lease.run_guarded_production_command(
                command, display_command);
        },
        require_state().checkout);
}

ReviewedSourceBuildCheckoutReproofError::
    ReviewedSourceBuildCheckoutReproofError(
        TrustedGitPinnedCheckoutFailure failure)
    : std::runtime_error(
          "Reviewed source checkout overlay reproof failed."),
      failure_(std::move(failure)) {
}

const TrustedGitPinnedCheckoutFailure&
ReviewedSourceBuildCheckoutReproofError::failure() const noexcept {
    return failure_;
}

ReviewedSourcePublicationResult
publish_accepted_reviewed_source_checkout(
    AcceptedReviewedSourceCheckout target) {
    ReviewedSourceEditorBoundaryResult boundary =
        begin_reviewed_source_editor_boundary(target);
    if(auto* failure = std::get_if<ReviewedSourcePublicationFailure>(
           &boundary)) {
        return std::move(*failure);
    }
    ReviewedSourceEditorOverlayProofResult overlay =
        seal_reviewed_source_no_editor_overlay(
            target,
            std::get<ReviewedSourceEditorBoundary>(
                std::move(boundary)));
    if(auto* failure = std::get_if<ReviewedSourcePublicationFailure>(
           &overlay)) {
        return std::move(*failure);
    }
    return publish_accepted_reviewed_source_checkout_with_editor_overlay(
        std::move(target),
        std::get<ReviewedSourceEditorOverlayProof>(
            std::move(overlay)));
}

ReviewedSourcePublicationResult
publish_accepted_reviewed_source_checkout_with_editor_overlay(
    AcceptedReviewedSourceCheckout target,
    ReviewedSourceEditorOverlayProof editor_overlay) {
    const ReviewedSourceEditorOverlayStatus editor_overlay_status =
        editor_overlay.valid()
            ? editor_overlay.status()
            : ReviewedSourceEditorOverlayStatus::None;
    if(!target.valid() || !editor_overlay.valid()) {
        return with_editor_overlay(
            publication_failure(
                ReviewedSourcePublicationFailureReason::
                    InvalidCheckoutCapability),
            editor_overlay_status);
    }

    const ReviewedSourceEditorOverlayProof::State& overlay =
        editor_overlay.require_state();
    if(overlay.identity != target.identity() ||
       overlay.checkout_device != target.checkout_device() ||
       overlay.checkout_inode != target.checkout_inode()) {
        return with_editor_overlay(
            publication_failure(
                ReviewedSourcePublicationFailureReason::
                    InvalidCheckoutCapability),
            editor_overlay_status);
    }
    const AcceptedReviewedSourceCheckout::State& checkout_state =
        target.require_state();
    TrustedGitPinnedCheckoutRevalidationResult prepublication =
        revalidate_trusted_git_pinned_checkout_overlay(
            checkout_state.checkout, checkout_state.lease,
            overlay.post_editor);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &prepublication)) {
        return with_editor_overlay(
            checkout_publication_failure(std::move(*failure)),
            editor_overlay_status);
    }

    const AurReviewedSourceReviewIdentity identity = target.identity();
    const ReviewedSourceState next_state = ReviewedSourceState::make(
        identity.package_base(), identity.target_revision());
    ReviewedSourceStateStorePublishResult published =
        publish_reviewed_source_state(
            next_state,
            target.expected_state_observation().observed_record());

    const auto finish_pinned =
        [&target, &editor_overlay, editor_overlay_status](
            ReviewedSourcePublicationStatus publication_status,
            ReviewedSourceState state,
            ReviewedSourceStateObservedRecord observed)
        -> ReviewedSourcePublicationResult {
        const AcceptedReviewedSourceCheckout::State& current_checkout =
            target.require_state();
        TrustedGitPinnedCheckoutRevalidationResult revalidated =
            revalidate_trusted_git_pinned_checkout_overlay(
                current_checkout.checkout, current_checkout.lease,
                editor_overlay.require_state().post_editor);
        if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
               &revalidated)) {
            return ReviewedSourcePostPublicationCheckoutFailure{
                publication_status, std::move(state),
                std::move(observed), std::move(*failure),
                editor_overlay_status};
        }
        return PinnedReviewedSourceBuild(
            std::move(target), publication_status, std::move(state),
            std::move(observed), std::move(editor_overlay));
    };

    if(auto* success =
           std::get_if<ReviewedSourceStateStorePublished>(&published)) {
        if(success->state != next_state) {
            return with_editor_overlay(
                publication_failure(
                    ReviewedSourcePublicationFailureReason::
                        InconsistentStoreResult),
                editor_overlay_status);
        }
        return finish_pinned(
            ReviewedSourcePublicationStatus::Published,
            std::move(success->state), std::move(success->observed));
    }
    if(auto* uncertain = std::get_if<
           ReviewedSourceStateStorePublishedUncertain>(&published)) {
        return ReviewedSourcePublicationUncertain{
            std::move(*uncertain), editor_overlay_status};
    }
    if(auto* unsafe =
           std::get_if<ReviewedSourceStateStoreUnsafeHistory>(
               &published)) {
        return ReviewedSourcePublicationUnsafeHistory{
            std::move(*unsafe), editor_overlay_status};
    }

    ReviewedSourceStateStoreFailure failure =
        std::move(std::get<ReviewedSourceStateStoreFailure>(published));
    if(failure.kind !=
       ReviewedSourceStateStoreFailureKind::ConcurrentReplacement) {
        return with_editor_overlay(
            store_publication_failure(
                ReviewedSourcePublicationFailureReason::StoreFailure,
                std::move(failure)),
            editor_overlay_status);
    }

    ReviewedSourceStateStoreReadResult current_result =
        read_reviewed_source_state(identity.package_base());
    if(auto* unsafe = std::get_if<ReviewedSourceStateStoreUnsafeHistory>(
           &current_result)) {
        return ReviewedSourcePublicationUnsafeHistory{
            std::move(*unsafe), editor_overlay_status};
    }
    if(auto* read_failure = std::get_if<ReviewedSourceStateStoreFailure>(
           &current_result)) {
        return with_editor_overlay(
            store_publication_failure(
                ReviewedSourcePublicationFailureReason::
                    ConflictObservationFailure,
                std::move(*read_failure)),
            editor_overlay_status);
    }

    ReviewedSourceStateStoreRead current =
        std::move(std::get<ReviewedSourceStateStoreRead>(current_result));
    const auto* loaded =
        std::get_if<ReviewedSourceStateLoaded>(&current.observation);
    if(loaded != nullptr && loaded->state == next_state) {
        if(!current.observed.has_value()) {
            return with_editor_overlay(
                publication_failure(
                    ReviewedSourcePublicationFailureReason::
                        InconsistentStoreResult),
                editor_overlay_status);
        }
        return finish_pinned(
            ReviewedSourcePublicationStatus::AlreadyPublishedSameTarget,
            loaded->state, *current.observed);
    }
    return ReviewedSourcePublicationConflict{
        std::move(identity), std::move(current), editor_overlay_status};
}

ReviewedSourcePublicationResult
confirm_already_reviewed_source_checkout(
    AlreadyReviewedSourceCheckout target) {
    ReviewedSourceEditorBoundaryResult boundary =
        begin_reviewed_source_editor_boundary(target);
    if(auto* failure = std::get_if<ReviewedSourcePublicationFailure>(
           &boundary)) {
        return std::move(*failure);
    }
    ReviewedSourceEditorOverlayProofResult overlay =
        seal_reviewed_source_no_editor_overlay(
            target,
            std::get<ReviewedSourceEditorBoundary>(
                std::move(boundary)));
    if(auto* failure = std::get_if<ReviewedSourcePublicationFailure>(
           &overlay)) {
        return std::move(*failure);
    }
    return confirm_already_reviewed_source_checkout_with_editor_overlay(
        std::move(target),
        std::get<ReviewedSourceEditorOverlayProof>(
            std::move(overlay)));
}

ReviewedSourcePublicationResult
confirm_already_reviewed_source_checkout_with_editor_overlay(
    AlreadyReviewedSourceCheckout target,
    ReviewedSourceEditorOverlayProof editor_overlay) {
    const ReviewedSourceEditorOverlayStatus editor_overlay_status =
        editor_overlay.valid()
            ? editor_overlay.status()
            : ReviewedSourceEditorOverlayStatus::None;
    if(!target.valid() || !editor_overlay.valid()) {
        return with_editor_overlay(
            publication_failure(
                ReviewedSourcePublicationFailureReason::
                    InvalidCheckoutCapability),
            editor_overlay_status);
    }

    const ReviewedSourceEditorOverlayProof::State& overlay =
        editor_overlay.require_state();
    if(overlay.identity != target.identity() ||
       overlay.checkout_device != target.checkout_device() ||
       overlay.checkout_inode != target.checkout_inode()) {
        return with_editor_overlay(
            publication_failure(
                ReviewedSourcePublicationFailureReason::
                    InvalidCheckoutCapability),
            editor_overlay_status);
    }

    const AurReviewedSourceReviewIdentity identity = target.identity();
    const ReviewedSourceState expected_state = ReviewedSourceState::make(
        identity.package_base(), identity.target_revision());
    const auto* expected_loaded = std::get_if<ReviewedSourceStateLoaded>(
        &target.expected_state_observation().observation());
    if(expected_loaded == nullptr ||
       expected_loaded->state != expected_state ||
       !target.expected_state_observation().observed_record().has_value()) {
        return with_editor_overlay(
            publication_failure(
                ReviewedSourcePublicationFailureReason::
                    InconsistentStoreResult),
            editor_overlay_status);
    }

    const AlreadyReviewedSourceCheckout::State& checkout_state =
        target.require_state();
    TrustedGitPinnedCheckoutRevalidationResult preconfirmation =
        revalidate_trusted_git_pinned_checkout_overlay(
            checkout_state.checkout, checkout_state.lease,
            overlay.post_editor);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &preconfirmation)) {
        return with_editor_overlay(
            checkout_publication_failure(std::move(*failure)),
            editor_overlay_status);
    }

    ReviewedSourceStateStoreReadResult current_result =
        read_reviewed_source_state(identity.package_base());
    if(auto* unsafe = std::get_if<ReviewedSourceStateStoreUnsafeHistory>(
           &current_result)) {
        return ReviewedSourcePublicationUnsafeHistory{
            std::move(*unsafe), editor_overlay_status};
    }
    if(auto* failure = std::get_if<ReviewedSourceStateStoreFailure>(
           &current_result)) {
        return with_editor_overlay(
            store_publication_failure(
                ReviewedSourcePublicationFailureReason::StoreFailure,
                std::move(*failure)),
            editor_overlay_status);
    }

    ReviewedSourceStateStoreRead current =
        std::move(std::get<ReviewedSourceStateStoreRead>(current_result));
    const auto* loaded =
        std::get_if<ReviewedSourceStateLoaded>(&current.observation);
    if(loaded == nullptr || loaded->state != expected_state) {
        return ReviewedSourcePublicationConflict{
            std::move(identity), std::move(current),
            editor_overlay_status};
    }
    if(!current.observed.has_value()) {
        return with_editor_overlay(
            publication_failure(
                ReviewedSourcePublicationFailureReason::
                    InconsistentStoreResult),
            editor_overlay_status);
    }

    const AlreadyReviewedSourceCheckout::State& current_checkout =
        target.require_state();
    TrustedGitPinnedCheckoutRevalidationResult revalidated =
        revalidate_trusted_git_pinned_checkout_overlay(
            current_checkout.checkout, current_checkout.lease,
            editor_overlay.require_state().post_editor);
    if(auto* failure = std::get_if<TrustedGitPinnedCheckoutFailure>(
           &revalidated)) {
        return ReviewedSourcePostPublicationCheckoutFailure{
            ReviewedSourcePublicationStatus::AlreadyPublishedSameTarget,
            loaded->state, *current.observed, std::move(*failure),
            editor_overlay_status};
    }
    return PinnedReviewedSourceBuild(
        std::move(target), loaded->state, *current.observed,
        std::move(editor_overlay));
}
