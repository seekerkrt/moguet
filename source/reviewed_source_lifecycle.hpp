#pragma once

#include "reviewed_source_projection.hpp"
#include "reviewed_source_state_store.hpp"

#include <optional>
#include <string>
#include <variant>

// POLICY(#411): Slice 4A owns only typed lifecycle observation. It does not
// publish reviewed state, materialize a checkout, authorize a build, or connect
// any production route.

class AurReviewedSourceReviewIdentity;
class ReviewedSourceFatalStatePreflight;
class ReviewedSourceReviewRequirement;
class ReviewedSourceAlreadyReviewedContinue;
class ReviewedSourceOperationStop;

// A bootstrap requests a full inventory without erasing the observed CAS input.
enum class ReviewedSourceReviewPurpose { NormalUpdate,
                                         DevelTrackingBootstrap };

using ReviewedSourceLifecyclePlanResult = std::variant<
    ReviewedSourceReviewRequirement,
    ReviewedSourceAlreadyReviewedContinue,
    ReviewedSourceOperationStop>;

// The exact production transition reads current persistent state at review
// start. Git history relation and baseline object availability are bound later
// from the verified 3B review rather than flattened into this observation.
[[nodiscard]] ReviewedSourceLifecyclePlanResult
plan_reviewed_source_lifecycle(
    AurReviewedSourceReviewIdentity identity);

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS
[[nodiscard]] ReviewedSourceLifecyclePlanResult
plan_reviewed_source_lifecycle(
    AurReviewedSourceReviewIdentity identity,
    ReviewedSourceStateStoreReadResult store_result);
#endif

// PackageBase is the review/state unit. The canonical AUR source and exact
// upstream base revision remain existing typed identities rather than being
// flattened into a derived string key.
class AurReviewedSourceReviewIdentity final {
public:
    AurReviewedSourceReviewIdentity() = delete;
    AurReviewedSourceReviewIdentity(
        const AurReviewedSourceReviewIdentity&) = default;
    AurReviewedSourceReviewIdentity(
        AurReviewedSourceReviewIdentity&&) noexcept = default;
    AurReviewedSourceReviewIdentity& operator=(
        const AurReviewedSourceReviewIdentity&) = default;
    AurReviewedSourceReviewIdentity& operator=(
        AurReviewedSourceReviewIdentity&&) noexcept = default;
    ~AurReviewedSourceReviewIdentity() = default;

    [[nodiscard]] static AurReviewedSourceReviewIdentity make(
        PackageBaseIdentity package_base,
        SourceRevisionIdentity target_revision);

    [[nodiscard]] const PackageBaseIdentity& package_base() const noexcept;
    [[nodiscard]] const PackageSourceIdentity& source() const noexcept;
    [[nodiscard]] const std::string& canonical_git_remote() const noexcept;
    [[nodiscard]] const SourceRevisionIdentity& target_revision()
        const noexcept;
    [[nodiscard]] GitObjectFormat git_object_format() const noexcept;

    bool operator==(const AurReviewedSourceReviewIdentity&) const = default;

private:
    AurReviewedSourceReviewIdentity(
        PackageBaseIdentity package_base,
        SourceRevisionIdentity target_revision) noexcept;

    PackageBaseIdentity package_base_;
    SourceRevisionIdentity target_revision_;
};

// Exact read-time CAS input. Missing alone has no observed record; every
// observed document retains the exact record identity and raw bytes returned by
// the store, including invalid/corrupted/source-mismatched records.
class ReviewedSourceExpectedStateObservation final {
public:
    ReviewedSourceExpectedStateObservation() = delete;
    ReviewedSourceExpectedStateObservation(
        const ReviewedSourceExpectedStateObservation&) = delete;
    ReviewedSourceExpectedStateObservation(
        ReviewedSourceExpectedStateObservation&&) noexcept = default;
    ReviewedSourceExpectedStateObservation& operator=(
        const ReviewedSourceExpectedStateObservation&) = delete;
    ReviewedSourceExpectedStateObservation& operator=(
        ReviewedSourceExpectedStateObservation&&) noexcept = default;
    ~ReviewedSourceExpectedStateObservation() = default;

    [[nodiscard]] const ReviewedSourceStateObservation& observation()
        const noexcept;
    [[nodiscard]] const std::optional<ReviewedSourceStateObservedRecord>&
    observed_record() const noexcept;
    [[nodiscard]] const ReviewedSourceStateStoreRead& store_read()
        const noexcept;

private:
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle(
        AurReviewedSourceReviewIdentity identity);
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle_from_preflight(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceFatalStatePreflight preflight,
        ReviewedSourceReviewPurpose purpose);

    explicit ReviewedSourceExpectedStateObservation(
        ReviewedSourceStateStoreRead store_read) noexcept;

    ReviewedSourceStateStoreRead store_read_;
};

enum class ReviewedSourceReviewRequirementKind {
    InitialFullReview,
    BootstrapFullReview,
    UpdateReview,
    AbnormalStateRebindFullReview,
};

enum class ReviewedSourceAbnormalStateReason {
    Invalid,
    Corrupted,
    SourceMismatch,
};

struct ReviewedSourceLifecycleInitialFullReview {
    bool operator==(
        const ReviewedSourceLifecycleInitialFullReview&) const = default;
};

struct ReviewedSourceLifecycleBootstrapFullReview {
    bool operator==(const ReviewedSourceLifecycleBootstrapFullReview&) const = default;
};

struct ReviewedSourceLifecycleAlreadyReviewed {
    bool operator==(
        const ReviewedSourceLifecycleAlreadyReviewed&) const = default;
};

struct ReviewedSourceLifecycleUpdateReview {
    SourceRevisionIdentity baseline;
    ReviewedSourceHistoryRelation relation;

    bool operator==(
        const ReviewedSourceLifecycleUpdateReview&) const = default;
};

struct ReviewedSourceLifecycleRebaselineFullReview {
    SourceRevisionIdentity unavailable_baseline;
    ReviewedSourceBaselineUnavailableReason reason;

    bool operator==(
        const ReviewedSourceLifecycleRebaselineFullReview&) const =
        default;
};

struct ReviewedSourceLifecycleAbnormalStateRebindFullReview {
    ReviewedSourceAbnormalStateReason reason;

    bool operator==(
        const ReviewedSourceLifecycleAbnormalStateRebindFullReview&)
        const = default;
};

enum class ReviewedSourceFatalStateReason {
    UnsupportedFuture,
    UnsafeHistory,
    StoreFailure,
    InconsistentStoreObservation,
};

struct ReviewedSourceLifecycleFatalState {
    ReviewedSourceFatalStateReason reason;

    bool operator==(
        const ReviewedSourceLifecycleFatalState&) const = default;
};

// Fatal state retains the exact store payload that made continuation unsafe.
// Unsupported/incoherent observations keep the complete read snapshot;
// filesystem/store failures and unsafe histories keep their existing typed
// values instead of reducing them to a diagnostic string.
struct ReviewedSourceFatalStateFailure {
    ReviewedSourceFatalStateReason reason;
    std::optional<ReviewedSourceStateStoreRead> store_read;
    std::optional<ReviewedSourceStateStoreUnsafeHistory> unsafe_history;
    std::optional<ReviewedSourceStateStoreFailure> store_failure;

    bool operator==(const ReviewedSourceFatalStateFailure&) const = default;
};

using ReviewedSourceIntegrationLifecycle = std::variant<
    ReviewedSourceLifecycleInitialFullReview,
    ReviewedSourceLifecycleBootstrapFullReview,
    ReviewedSourceLifecycleAlreadyReviewed,
    ReviewedSourceLifecycleUpdateReview,
    ReviewedSourceLifecycleRebaselineFullReview,
    ReviewedSourceLifecycleAbnormalStateRebindFullReview,
    ReviewedSourceLifecycleFatalState>;

enum class ReviewedSourceOperationStopReason {
    UnsupportedFuture,
    UnsafeHistory,
    StoreFailure,
    InconsistentStoreObservation,
    PackageBaseMismatch,
    SourceIdentityMismatch,
    GitObjectFormatMismatch,
    TargetRevisionMismatch,
    BaselineMismatch,
    LifecycleMismatch,
    MaterializationFailure,
    PresentationFailure,
    PresentationOutputFailure,
    ManualInspectionRequired,
    SensitiveSourceUnrenderable,
    NonExplicitAcceptance,
    InvalidCapability,
    ExplicitCancellation,
    EndOfInput,
    InputFailure,
};

// A stop result intentionally carries no review target, expected CAS record,
// acceptance token, or build continuation capability.
class ReviewedSourceOperationStop final {
public:
    ReviewedSourceOperationStop() = delete;

    [[nodiscard]] static ReviewedSourceOperationStop make(
        ReviewedSourceOperationStopReason reason) noexcept;
    [[nodiscard]] static ReviewedSourceOperationStop fatal(
        ReviewedSourceFatalStateFailure failure) noexcept;

    [[nodiscard]] ReviewedSourceOperationStopReason reason() const noexcept {
        return reason_;
    }
    [[nodiscard]] const std::optional<ReviewedSourceIntegrationLifecycle>&
    lifecycle() const noexcept;
    [[nodiscard]] const std::optional<ReviewedSourceFatalStateFailure>&
    fatal_state_failure() const noexcept;

    bool operator==(const ReviewedSourceOperationStop&) const = default;

private:
    ReviewedSourceOperationStop(
        ReviewedSourceOperationStopReason reason,
        std::optional<ReviewedSourceIntegrationLifecycle> lifecycle,
        std::optional<ReviewedSourceFatalStateFailure> fatal_failure) noexcept;

    ReviewedSourceOperationStopReason reason_;
    std::optional<ReviewedSourceIntegrationLifecycle> lifecycle_;
    std::optional<ReviewedSourceFatalStateFailure> fatal_failure_;
};

// A non-fatal read-time proof for one exact PackageBase. Compatibility routes
// may consume and discard it after observing fatal state; reviewed routes pass
// the same store observation into lifecycle planning without a second read.
class ReviewedSourceFatalStatePreflight final {
public:
    ReviewedSourceFatalStatePreflight() = delete;
    ReviewedSourceFatalStatePreflight(
        const ReviewedSourceFatalStatePreflight&) = delete;
    ReviewedSourceFatalStatePreflight(
        ReviewedSourceFatalStatePreflight&&) noexcept = default;
    ReviewedSourceFatalStatePreflight& operator=(
        const ReviewedSourceFatalStatePreflight&) = delete;
    ReviewedSourceFatalStatePreflight& operator=(
        ReviewedSourceFatalStatePreflight&&) noexcept = default;
    ~ReviewedSourceFatalStatePreflight() = default;

    [[nodiscard]] const PackageBaseIdentity& package_base() const noexcept;

private:
    friend std::variant<
        ReviewedSourceFatalStatePreflight,
        ReviewedSourceOperationStop>
    preflight_reviewed_source_fatal_state(
        PackageBaseIdentity package_base);
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle_from_preflight(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceFatalStatePreflight preflight,
        ReviewedSourceReviewPurpose purpose);

    ReviewedSourceFatalStatePreflight(
        PackageBaseIdentity package_base,
        ReviewedSourceStateStoreRead store_read) noexcept;

    PackageBaseIdentity package_base_;
    ReviewedSourceStateStoreRead store_read_;
};

using ReviewedSourceFatalStatePreflightResult = std::variant<
    ReviewedSourceFatalStatePreflight,
    ReviewedSourceOperationStop>;

// This boundary observes only persistent reviewed-state fatality. It never
// creates review, acceptance, publication, checkout, or build authority.
[[nodiscard]] ReviewedSourceFatalStatePreflightResult
preflight_reviewed_source_fatal_state(
    PackageBaseIdentity package_base);

// Review planning consumes the exact non-fatal observation returned above.
// The PackageBase must match the later exact target identity.
[[nodiscard]] ReviewedSourceLifecyclePlanResult
plan_reviewed_source_lifecycle_from_preflight(
    AurReviewedSourceReviewIdentity identity,
    ReviewedSourceFatalStatePreflight preflight,
    ReviewedSourceReviewPurpose purpose = ReviewedSourceReviewPurpose::NormalUpdate);

class ReviewedSourceReviewRequirement final {
public:
    ReviewedSourceReviewRequirement() = delete;
    ReviewedSourceReviewRequirement(
        const ReviewedSourceReviewRequirement&) = delete;
    ReviewedSourceReviewRequirement(
        ReviewedSourceReviewRequirement&&) noexcept = default;
    ReviewedSourceReviewRequirement& operator=(
        const ReviewedSourceReviewRequirement&) = delete;
    ReviewedSourceReviewRequirement& operator=(
        ReviewedSourceReviewRequirement&&) noexcept = default;
    ~ReviewedSourceReviewRequirement() = default;

    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity()
        const noexcept;
    [[nodiscard]] ReviewedSourceReviewRequirementKind kind() const noexcept;
    [[nodiscard]] const SourceRevisionIdentity* baseline() const noexcept;
    [[nodiscard]] const ReviewedSourceAbnormalStateReason* abnormal_reason()
        const noexcept;
    [[nodiscard]] const ReviewedSourceExpectedStateObservation&
    expected_state_observation() const noexcept;

private:
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle(
        AurReviewedSourceReviewIdentity identity);
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle_from_preflight(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceFatalStatePreflight preflight,
        ReviewedSourceReviewPurpose purpose);

    ReviewedSourceReviewRequirement(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceReviewRequirementKind kind,
        std::optional<SourceRevisionIdentity> baseline,
        std::optional<ReviewedSourceAbnormalStateReason> abnormal_reason,
        ReviewedSourceExpectedStateObservation expected) noexcept;

    AurReviewedSourceReviewIdentity identity_;
    ReviewedSourceReviewRequirementKind kind_;
    std::optional<SourceRevisionIdentity> baseline_;
    std::optional<ReviewedSourceAbnormalStateReason> abnormal_reason_;
    ReviewedSourceExpectedStateObservation expected_;
};

// AlreadyReviewed is a typed continue disposition. It deliberately has no
// presentation/acceptance step and is not reviewed-state rewrite authority.
class ReviewedSourceAlreadyReviewedContinue final {
public:
    ReviewedSourceAlreadyReviewedContinue() = delete;
    ReviewedSourceAlreadyReviewedContinue(
        const ReviewedSourceAlreadyReviewedContinue&) = delete;
    ReviewedSourceAlreadyReviewedContinue(
        ReviewedSourceAlreadyReviewedContinue&& other) noexcept;
    ReviewedSourceAlreadyReviewedContinue& operator=(
        const ReviewedSourceAlreadyReviewedContinue&) = delete;
    ReviewedSourceAlreadyReviewedContinue& operator=(
        ReviewedSourceAlreadyReviewedContinue&&) noexcept = delete;
    ~ReviewedSourceAlreadyReviewedContinue() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AurReviewedSourceReviewIdentity& identity()
        const noexcept;
    [[nodiscard]] const ReviewedSourceIntegrationLifecycle& lifecycle()
        const noexcept;
    [[nodiscard]] const ReviewedSourceExpectedStateObservation&
    expected_state_observation() const noexcept;

private:
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle(
        AurReviewedSourceReviewIdentity identity);
    friend ReviewedSourceLifecyclePlanResult
    plan_reviewed_source_lifecycle_from_preflight(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceFatalStatePreflight preflight,
        ReviewedSourceReviewPurpose purpose);

    ReviewedSourceAlreadyReviewedContinue(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceExpectedStateObservation expected) noexcept;

    AurReviewedSourceReviewIdentity identity_;
    ReviewedSourceIntegrationLifecycle lifecycle_;
    ReviewedSourceExpectedStateObservation expected_;
    bool valid_ = true;
};

#if defined(MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS)
void set_reviewed_source_lifecycle_store_result_for_test(
    ReviewedSourceStateStoreReadResult store_result);
void set_reviewed_source_lifecycle_default_missing_for_test(bool enabled);
#endif
