#include "reviewed_source_lifecycle.hpp"

#include <deque>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

#if defined(MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS)
std::deque<ReviewedSourceStateStoreReadResult>
    g_reviewed_source_lifecycle_store_results;
bool g_reviewed_source_lifecycle_default_missing = false;
#endif

bool store_read_is_coherent(
    const ReviewedSourceStateStoreRead& store_read,
    const PackageBaseIdentity& expected_package_base) {
    if(std::holds_alternative<ReviewedSourceStateMissing>(
           store_read.observation)) {
        return !store_read.observed.has_value();
    }
    if(!store_read.observed.has_value()) return false;

    const ReviewedSourceStateInterpretation interpreted =
        interpret_reviewed_source_state(
            store_read.observed->raw_contents,
            expected_package_base);
    return std::visit(
        [&interpreted](const auto& observation) {
            using Observation = std::decay_t<decltype(observation)>;
            if constexpr(std::is_same_v<
                             Observation,
                             ReviewedSourceStateMissing>) {
                return false;
            } else {
                const auto* matching =
                    std::get_if<Observation>(&interpreted);
                return matching != nullptr &&
                       *matching == observation;
            }
        },
        store_read.observation);
}

ReviewedSourceOperationStopReason stop_reason(
    ReviewedSourceFatalStateReason reason) noexcept {
    switch(reason) {
        case ReviewedSourceFatalStateReason::UnsupportedFuture:
            return ReviewedSourceOperationStopReason::UnsupportedFuture;
        case ReviewedSourceFatalStateReason::UnsafeHistory:
            return ReviewedSourceOperationStopReason::UnsafeHistory;
        case ReviewedSourceFatalStateReason::StoreFailure:
            return ReviewedSourceOperationStopReason::StoreFailure;
        case ReviewedSourceFatalStateReason::InconsistentStoreObservation:
            return ReviewedSourceOperationStopReason::
                InconsistentStoreObservation;
    }
    return ReviewedSourceOperationStopReason::InconsistentStoreObservation;
}

} // namespace

AurReviewedSourceReviewIdentity::AurReviewedSourceReviewIdentity(
    PackageBaseIdentity package_base,
    SourceRevisionIdentity target_revision) noexcept
    : package_base_(std::move(package_base)),
      target_revision_(std::move(target_revision)) {
}

AurReviewedSourceReviewIdentity AurReviewedSourceReviewIdentity::make(
    PackageBaseIdentity package_base,
    SourceRevisionIdentity target_revision) {
    const PackageSourceIdentity& source = package_base.source();
    const SourceLocationIdentity& location = source.location();
    if(source.kind() != PackageSourceKind::Aur ||
       location.kind() != SourceLocationKind::GitRemote ||
       location.state() != SourceLocationState::Known ||
       location.value() == nullptr) {
        throw std::invalid_argument(
            "A reviewed source target requires a known canonical AUR Git remote.");
    }
    const std::string expected_remote =
        "https://aur.archlinux.org/" + package_base.package_base() +
        ".git";
    if(*location.value() != expected_remote) {
        throw std::invalid_argument(
            "A reviewed source target requires the canonical AUR Git remote for its PackageBase.");
    }
    if(target_revision.state() != SourceRevisionState::Known ||
       target_revision.git_commit() == nullptr ||
       target_revision.git_object_format() == nullptr) {
        throw std::invalid_argument(
            "A reviewed source target requires an exact Git commit identity.");
    }
    return AurReviewedSourceReviewIdentity(
        std::move(package_base), std::move(target_revision));
}

const PackageBaseIdentity&
AurReviewedSourceReviewIdentity::package_base() const noexcept {
    return package_base_;
}

const PackageSourceIdentity&
AurReviewedSourceReviewIdentity::source() const noexcept {
    return package_base_.source();
}

const std::string&
AurReviewedSourceReviewIdentity::canonical_git_remote() const noexcept {
    return *package_base_.source().location().value();
}

const SourceRevisionIdentity&
AurReviewedSourceReviewIdentity::target_revision() const noexcept {
    return target_revision_;
}

GitObjectFormat
AurReviewedSourceReviewIdentity::git_object_format() const noexcept {
    return *target_revision_.git_object_format();
}

ReviewedSourceExpectedStateObservation::
    ReviewedSourceExpectedStateObservation(
        ReviewedSourceStateStoreRead store_read) noexcept
    : store_read_(std::move(store_read)) {
}

const ReviewedSourceStateObservation&
ReviewedSourceExpectedStateObservation::observation() const noexcept {
    return store_read_.observation;
}

const std::optional<ReviewedSourceStateObservedRecord>&
ReviewedSourceExpectedStateObservation::observed_record() const noexcept {
    return store_read_.observed;
}

const ReviewedSourceStateStoreRead&
ReviewedSourceExpectedStateObservation::store_read() const noexcept {
    return store_read_;
}

ReviewedSourceOperationStop::ReviewedSourceOperationStop(
    ReviewedSourceOperationStopReason reason,
    std::optional<ReviewedSourceIntegrationLifecycle> lifecycle,
    std::optional<ReviewedSourceFatalStateFailure> fatal_failure) noexcept
    : reason_(reason), lifecycle_(std::move(lifecycle)),
      fatal_failure_(std::move(fatal_failure)) {
}

ReviewedSourceOperationStop ReviewedSourceOperationStop::make(
    ReviewedSourceOperationStopReason reason) noexcept {
    return ReviewedSourceOperationStop(
        reason, std::nullopt, std::nullopt);
}

ReviewedSourceOperationStop ReviewedSourceOperationStop::fatal(
    ReviewedSourceFatalStateFailure failure) noexcept {
    const ReviewedSourceFatalStateReason reason = failure.reason;
    return ReviewedSourceOperationStop(
        stop_reason(reason),
        ReviewedSourceIntegrationLifecycle(
            ReviewedSourceLifecycleFatalState{reason}),
        std::move(failure));
}

const std::optional<ReviewedSourceIntegrationLifecycle>&
ReviewedSourceOperationStop::lifecycle() const noexcept {
    return lifecycle_;
}

const std::optional<ReviewedSourceFatalStateFailure>&
ReviewedSourceOperationStop::fatal_state_failure() const noexcept {
    return fatal_failure_;
}

ReviewedSourceFatalStatePreflight::ReviewedSourceFatalStatePreflight(
    PackageBaseIdentity package_base,
    ReviewedSourceStateStoreRead store_read) noexcept
    : package_base_(std::move(package_base)),
      store_read_(std::move(store_read)) {
}

const PackageBaseIdentity&
ReviewedSourceFatalStatePreflight::package_base() const noexcept {
    return package_base_;
}

ReviewedSourceReviewRequirement::ReviewedSourceReviewRequirement(
    AurReviewedSourceReviewIdentity identity,
    ReviewedSourceReviewRequirementKind kind,
    std::optional<SourceRevisionIdentity> baseline,
    std::optional<ReviewedSourceAbnormalStateReason> abnormal_reason,
    ReviewedSourceExpectedStateObservation expected) noexcept
    : identity_(std::move(identity)), kind_(kind),
      baseline_(std::move(baseline)), abnormal_reason_(abnormal_reason),
      expected_(std::move(expected)) {
}

const AurReviewedSourceReviewIdentity&
ReviewedSourceReviewRequirement::identity() const noexcept {
    return identity_;
}

ReviewedSourceReviewRequirementKind
ReviewedSourceReviewRequirement::kind() const noexcept {
    return kind_;
}

const SourceRevisionIdentity*
ReviewedSourceReviewRequirement::baseline() const noexcept {
    return baseline_.has_value() ? &baseline_.value() : nullptr;
}

const ReviewedSourceAbnormalStateReason*
ReviewedSourceReviewRequirement::abnormal_reason() const noexcept {
    return abnormal_reason_.has_value() ? &abnormal_reason_.value() : nullptr;
}

const ReviewedSourceExpectedStateObservation&
ReviewedSourceReviewRequirement::expected_state_observation()
    const noexcept {
    return expected_;
}

ReviewedSourceAlreadyReviewedContinue::
    ReviewedSourceAlreadyReviewedContinue(
        AurReviewedSourceReviewIdentity identity,
        ReviewedSourceExpectedStateObservation expected) noexcept
    : identity_(std::move(identity)),
      lifecycle_(ReviewedSourceLifecycleAlreadyReviewed{}),
      expected_(std::move(expected)) {
}

ReviewedSourceAlreadyReviewedContinue::
    ReviewedSourceAlreadyReviewedContinue(
        ReviewedSourceAlreadyReviewedContinue&& other) noexcept
    : identity_(std::move(other.identity_)),
      lifecycle_(std::move(other.lifecycle_)),
      expected_(std::move(other.expected_)),
      valid_(std::exchange(other.valid_, false)) {
}

bool ReviewedSourceAlreadyReviewedContinue::valid() const noexcept {
    return valid_;
}

const AurReviewedSourceReviewIdentity&
ReviewedSourceAlreadyReviewedContinue::identity() const noexcept {
    return identity_;
}

const ReviewedSourceIntegrationLifecycle&
ReviewedSourceAlreadyReviewedContinue::lifecycle() const noexcept {
    return lifecycle_;
}

const ReviewedSourceExpectedStateObservation&
ReviewedSourceAlreadyReviewedContinue::expected_state_observation()
    const noexcept {
    return expected_;
}

ReviewedSourceLifecyclePlanResult plan_reviewed_source_lifecycle(
    AurReviewedSourceReviewIdentity identity) {
    ReviewedSourceFatalStatePreflightResult preflight =
        preflight_reviewed_source_fatal_state(
            identity.package_base());
    if(auto* stop = std::get_if<ReviewedSourceOperationStop>(&preflight)) {
        return std::move(*stop);
    }
    return plan_reviewed_source_lifecycle_from_preflight(
        std::move(identity),
        std::get<ReviewedSourceFatalStatePreflight>(
            std::move(preflight)));
}

ReviewedSourceFatalStatePreflightResult
preflight_reviewed_source_fatal_state(
    PackageBaseIdentity package_base) {
    ReviewedSourceStateStoreReadResult store_result = [&package_base] {
#if defined(MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS)
        if(!g_reviewed_source_lifecycle_store_results.empty()) {
            ReviewedSourceStateStoreReadResult injected = std::move(
                g_reviewed_source_lifecycle_store_results.front());
            g_reviewed_source_lifecycle_store_results.pop_front();
            return injected;
        }
#if defined(MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS)
        if(g_reviewed_source_lifecycle_default_missing) {
            return ReviewedSourceStateStoreReadResult(
                ReviewedSourceStateStoreRead{
                    ReviewedSourceStateMissing{}, std::nullopt});
        }
#endif
#endif
#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS
        throw std::logic_error(
            "A reviewed source lifecycle test observation is required.");
#else
        return read_reviewed_source_state(package_base);
#endif
    }();
    if(auto* unsafe = std::get_if<ReviewedSourceStateStoreUnsafeHistory>(
           &store_result)) {
        return ReviewedSourceOperationStop::fatal(
            ReviewedSourceFatalStateFailure{
                ReviewedSourceFatalStateReason::UnsafeHistory,
                std::nullopt, std::move(*unsafe), std::nullopt});
    }
    if(auto* failure = std::get_if<ReviewedSourceStateStoreFailure>(
           &store_result)) {
        return ReviewedSourceOperationStop::fatal(
            ReviewedSourceFatalStateFailure{
                ReviewedSourceFatalStateReason::StoreFailure,
                std::nullopt, std::nullopt, std::move(*failure)});
    }

    ReviewedSourceStateStoreRead store_read =
        std::get<ReviewedSourceStateStoreRead>(std::move(store_result));
    if(!store_read_is_coherent(store_read, package_base)) {
        return ReviewedSourceOperationStop::fatal(
            ReviewedSourceFatalStateFailure{
                ReviewedSourceFatalStateReason::
                    InconsistentStoreObservation,
                std::move(store_read), std::nullopt, std::nullopt});
    }

    if(std::holds_alternative<ReviewedSourceStateUnsupportedFuture>(
           store_read.observation)) {
        return ReviewedSourceOperationStop::fatal(
            ReviewedSourceFatalStateFailure{
                ReviewedSourceFatalStateReason::UnsupportedFuture,
                std::move(store_read), std::nullopt, std::nullopt});
    }

    return ReviewedSourceFatalStatePreflight(
        std::move(package_base), std::move(store_read));
}

ReviewedSourceLifecyclePlanResult
plan_reviewed_source_lifecycle_from_preflight(
    AurReviewedSourceReviewIdentity identity,
    ReviewedSourceFatalStatePreflight preflight,
    ReviewedSourceReviewPurpose purpose) {
    if(preflight.package_base_ != identity.package_base()) {
        return ReviewedSourceOperationStop::make(
            ReviewedSourceOperationStopReason::PackageBaseMismatch);
    }

    ReviewedSourceExpectedStateObservation expected(
        std::move(preflight.store_read_));
    const ReviewedSourceStateObservation& observation =
        expected.observation();

    if(purpose == ReviewedSourceReviewPurpose::DevelTrackingBootstrap) {
        // A valid prior review still needs full review for the new build intent.
        // Keep its exact record/digest as the publication CAS predecessor.
        if(!std::holds_alternative<ReviewedSourceStateMissing>(observation) &&
           !std::holds_alternative<ReviewedSourceStateLoaded>(observation)) {
            return ReviewedSourceOperationStop::make(
                ReviewedSourceOperationStopReason::InconsistentStoreObservation);
        }
        return ReviewedSourceReviewRequirement(
            std::move(identity), ReviewedSourceReviewRequirementKind::BootstrapFullReview,
            std::nullopt, std::nullopt, std::move(expected));
    }
    if(purpose != ReviewedSourceReviewPurpose::NormalUpdate) {
        return ReviewedSourceOperationStop::make(ReviewedSourceOperationStopReason::LifecycleMismatch);
    }
    if(std::holds_alternative<ReviewedSourceStateMissing>(observation)) {
        return ReviewedSourceReviewRequirement(
            std::move(identity),
            ReviewedSourceReviewRequirementKind::InitialFullReview,
            std::nullopt, std::nullopt, std::move(expected));
    }
    if(const auto* loaded =
           std::get_if<ReviewedSourceStateLoaded>(&observation)) {
        const SourceRevisionIdentity baseline =
            loaded->state.reviewed_revision();
        if(baseline == identity.target_revision()) {
            return ReviewedSourceAlreadyReviewedContinue(
                std::move(identity), std::move(expected));
        }
        return ReviewedSourceReviewRequirement(
            std::move(identity),
            ReviewedSourceReviewRequirementKind::UpdateReview,
            baseline, std::nullopt, std::move(expected));
    }
    if(std::holds_alternative<ReviewedSourceStateInvalid>(observation)) {
        return ReviewedSourceReviewRequirement(
            std::move(identity),
            ReviewedSourceReviewRequirementKind::
                AbnormalStateRebindFullReview,
            std::nullopt, ReviewedSourceAbnormalStateReason::Invalid,
            std::move(expected));
    }
    if(std::holds_alternative<ReviewedSourceStateCorrupted>(observation)) {
        return ReviewedSourceReviewRequirement(
            std::move(identity),
            ReviewedSourceReviewRequirementKind::
                AbnormalStateRebindFullReview,
            std::nullopt, ReviewedSourceAbnormalStateReason::Corrupted,
            std::move(expected));
    }
    if(std::holds_alternative<ReviewedSourceStateSourceMismatch>(
           observation)) {
        return ReviewedSourceReviewRequirement(
            std::move(identity),
            ReviewedSourceReviewRequirementKind::
                AbnormalStateRebindFullReview,
            std::nullopt,
            ReviewedSourceAbnormalStateReason::SourceMismatch,
            std::move(expected));
    }

    return ReviewedSourceOperationStop::fatal(
        ReviewedSourceFatalStateFailure{
            ReviewedSourceFatalStateReason::
                InconsistentStoreObservation,
            expected.store_read(), std::nullopt, std::nullopt});
}

#if defined(MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_REVIEWED_SOURCE_PRODUCTION_TEST_HOOKS)
void set_reviewed_source_lifecycle_store_result_for_test(
    ReviewedSourceStateStoreReadResult store_result) {
    g_reviewed_source_lifecycle_store_results.push_back(
        std::move(store_result));
}

void set_reviewed_source_lifecycle_default_missing_for_test(bool enabled) {
    g_reviewed_source_lifecycle_default_missing = enabled;
}
#endif

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_LIFECYCLE_TEST_HOOKS
ReviewedSourceLifecyclePlanResult plan_reviewed_source_lifecycle(
    AurReviewedSourceReviewIdentity identity,
    ReviewedSourceStateStoreReadResult store_result) {
    set_reviewed_source_lifecycle_store_result_for_test(
        std::move(store_result));
    return plan_reviewed_source_lifecycle(std::move(identity));
}
#endif
