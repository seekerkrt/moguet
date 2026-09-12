#include "reviewed_source_lifecycle.hpp"

#include <exception>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

static_assert(!std::is_default_constructible_v<
              AurReviewedSourceReviewIdentity>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceExpectedStateObservation>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceExpectedStateObservation>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceFatalStatePreflight>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceFatalStatePreflight>);
static_assert(std::is_move_constructible_v<
              ReviewedSourceFatalStatePreflight>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceReviewRequirement>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceReviewRequirement>);
static_assert(!std::is_default_constructible_v<
              ReviewedSourceAlreadyReviewedContinue>);
static_assert(!std::is_copy_constructible_v<
              ReviewedSourceAlreadyReviewedContinue>);

template <typename T>
concept HasExpectedStateObservation = requires(const T& value) {
    value.expected_state_observation();
};

static_assert(!HasExpectedStateObservation<ReviewedSourceOperationStop>);

constexpr std::string_view SHA1_A =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view SHA1_B =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view SHA256_C =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Arm, typename Variant>
const Arm& require_arm(const Variant& value, std::string_view message) {
    const Arm* arm = std::get_if<Arm>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return *arm;
}

template <typename Function>
void expect_invalid_argument(Function&& function) {
    try {
        std::forward<Function>(function)();
    } catch(const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected std::invalid_argument.");
}

PackageBaseIdentity aur_package_base(
    const std::string& package_base = "example-base",
    const std::string& remote =
        "https://aur.archlinux.org/example-base.git") {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(remote)),
        package_base);
}

AurReviewedSourceReviewIdentity review_identity(
    const std::string& commit = std::string(SHA1_A),
    const std::string& package_base = "example-base",
    const std::string& remote =
        "https://aur.archlinux.org/example-base.git") {
    return AurReviewedSourceReviewIdentity::make(
        aur_package_base(package_base, remote),
        SourceRevisionIdentity::git_commit(commit));
}

ReviewedSourceState state_for(
    const AurReviewedSourceReviewIdentity& identity,
    const std::string& commit) {
    return ReviewedSourceState::make(
        identity.package_base(),
        SourceRevisionIdentity::git_commit(commit));
}

ReviewedSourceStateObservedRecord observed_record(
    std::string raw_contents,
    std::uint64_t generation = 7) {
    return ReviewedSourceStateObservedRecord{
        generation,
        std::to_string(generation) + ".toml",
        ReviewedSourceStateRecordIdentity{
            11, 22, 33, 0600, 1, 44, 55, 66, 77, 88},
        std::move(raw_contents)};
}

ReviewedSourceStateObservation observation_from_document(
    const std::string& document,
    const PackageBaseIdentity& expected_package_base) {
    const ReviewedSourceStateInterpretation interpretation =
        interpret_reviewed_source_state(
            document, expected_package_base);
    return std::visit(
        [](const auto& value) -> ReviewedSourceStateObservation {
            return value;
        },
        interpretation);
}

ReviewedSourceStateStoreRead observed_read(
    const std::string& document,
    const PackageBaseIdentity& expected_package_base,
    std::uint64_t generation = 7) {
    return ReviewedSourceStateStoreRead{
        observation_from_document(document, expected_package_base),
        observed_record(document, generation)};
}

ReviewedSourceStateStoreRead missing_read() {
    return ReviewedSourceStateStoreRead{
        ReviewedSourceStateMissing{}, std::nullopt};
}

const ReviewedSourceLifecycleFatalState& require_fatal_lifecycle(
    const ReviewedSourceOperationStop& stop,
    std::string_view message) {
    require(stop.lifecycle().has_value(), std::string(message));
    return require_arm<ReviewedSourceLifecycleFatalState>(
        *stop.lifecycle(), message);
}

void test_typed_identity_reuses_aur_package_base_and_exact_revision() {
    const AurReviewedSourceReviewIdentity sha1 = review_identity();
    require(sha1.package_base() == aur_package_base(),
            "Typed review identity lost PackageBase authority.");
    require(sha1.source().kind() == PackageSourceKind::Aur,
            "Typed review identity lost AUR source kind.");
    require(sha1.canonical_git_remote() ==
                "https://aur.archlinux.org/example-base.git",
            "Typed review identity lost canonical Git remote.");
    require(sha1.target_revision().git_commit() != nullptr &&
                *sha1.target_revision().git_commit() == SHA1_A &&
                sha1.git_object_format() == GitObjectFormat::Sha1,
            "Typed review identity lost exact SHA-1 target.");

    const AurReviewedSourceReviewIdentity sha256 =
        review_identity(std::string(SHA256_C));
    require(sha256.target_revision().git_commit() != nullptr &&
                *sha256.target_revision().git_commit() == SHA256_C &&
                sha256.git_object_format() == GitObjectFormat::Sha256,
            "Typed review identity lost exact SHA-256 target.");

    expect_invalid_argument([] {
        static_cast<void>(AurReviewedSourceReviewIdentity::make(
            PackageBaseIdentity::make(
                PackageSourceIdentity::repository(
                    "core",
                    SourceLocationIdentity::known_git_remote(
                        "https://example.invalid/core.git")),
                "example-base"),
            SourceRevisionIdentity::git_commit(std::string(SHA1_A))));
    });
    expect_invalid_argument([] {
        static_cast<void>(AurReviewedSourceReviewIdentity::make(
            PackageBaseIdentity::make(
                PackageSourceIdentity::aur(
                    SourceLocationIdentity::unknown(
                        SourceLocationKind::GitRemote)),
                "example-base"),
            SourceRevisionIdentity::git_commit(std::string(SHA1_A))));
    });
    expect_invalid_argument([] {
        static_cast<void>(review_identity(
            std::string(SHA1_A), "example-base",
            "https://mirror.invalid/example-base.git"));
    });
    expect_invalid_argument([] {
        static_cast<void>(review_identity(
            std::string(SHA1_A), "other-base",
            "https://aur.archlinux.org/example-base.git"));
    });
    expect_invalid_argument([] {
        static_cast<void>(AurReviewedSourceReviewIdentity::make(
            aur_package_base(), SourceRevisionIdentity::unknown()));
    });
}

void test_split_children_share_one_package_base_review_identity() {
    const PackageBaseIdentity package_base = aur_package_base();
    const PackageChildIdentity first = PackageChildIdentity::make(
        package_base, "example-cli");
    const PackageChildIdentity second = PackageChildIdentity::make(
        package_base, "example-libs");

    const AurReviewedSourceReviewIdentity first_identity =
        AurReviewedSourceReviewIdentity::make(
            first.package_base(),
            SourceRevisionIdentity::git_commit(std::string(SHA1_A)));
    const AurReviewedSourceReviewIdentity second_identity =
        AurReviewedSourceReviewIdentity::make(
            second.package_base(),
            SourceRevisionIdentity::git_commit(std::string(SHA1_A)));
    require(first.package_name() != second.package_name(),
            "Split-package fixture did not use distinct children.");
    require(first_identity == second_identity,
            "Split-package children divided PackageBase review identity.");
}

void test_missing_maps_to_initial_full_review_with_expected_null() {
    ReviewedSourceLifecyclePlanResult result =
        plan_reviewed_source_lifecycle(
            review_identity(), missing_read());
    const auto& requirement = require_arm<ReviewedSourceReviewRequirement>(
        result, "Missing did not require InitialFullReview.");
    require(requirement.kind() ==
                ReviewedSourceReviewRequirementKind::InitialFullReview,
            "Missing mapped to the wrong review requirement.");
    require(requirement.baseline() == nullptr &&
                requirement.abnormal_reason() == nullptr,
            "InitialFullReview gained a baseline or rebind reason.");
    require(std::holds_alternative<ReviewedSourceStateMissing>(
                requirement.expected_state_observation().observation()) &&
                !requirement.expected_state_observation()
                     .observed_record()
                     .has_value(),
            "Only Missing may carry expected-null state.");
}

void test_loaded_same_is_already_reviewed_without_rewrite_authority() {
    const AurReviewedSourceReviewIdentity identity = review_identity();
    const ReviewedSourceState state = state_for(identity, std::string(SHA1_A));
    const std::string document = encode_reviewed_source_state(state);
    const ReviewedSourceStateObservedRecord expected_record =
        observed_record(document, 9);
    ReviewedSourceLifecyclePlanResult result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreRead{
                ReviewedSourceStateLoaded{state},
                expected_record});
    const auto& already =
        require_arm<ReviewedSourceAlreadyReviewedContinue>(
            result,
            "Loaded exact target did not map to AlreadyReviewed.");
    require(std::holds_alternative<
                ReviewedSourceLifecycleAlreadyReviewed>(
                already.lifecycle()),
            "AlreadyReviewed lifecycle was not retained.");
    require(already.identity() == identity,
            "AlreadyReviewed lost exact pinned target identity.");
    require(already.expected_state_observation().observed_record() ==
                expected_record,
            "AlreadyReviewed lost the exact loaded record observation.");
}

void test_loaded_different_maps_to_update_review() {
    const AurReviewedSourceReviewIdentity identity = review_identity();
    const ReviewedSourceState baseline_state =
        state_for(identity, std::string(SHA1_B));
    const std::string document = encode_reviewed_source_state(baseline_state);
    ReviewedSourceLifecyclePlanResult result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreRead{
                ReviewedSourceStateLoaded{baseline_state},
                observed_record(document)});
    const auto& requirement = require_arm<ReviewedSourceReviewRequirement>(
        result, "Loaded different target did not require UpdateReview.");
    require(requirement.kind() ==
                    ReviewedSourceReviewRequirementKind::UpdateReview &&
                requirement.baseline() != nullptr &&
                requirement.baseline()->git_commit() != nullptr &&
                *requirement.baseline()->git_commit() == SHA1_B,
            "UpdateReview lost its exact loaded baseline.");
    require(std::holds_alternative<ReviewedSourceStateLoaded>(
                requirement.expected_state_observation().observation()),
            "UpdateReview lost its Loaded observation kind.");
}

void test_abnormal_records_require_explicit_rebind_and_remain_exact() {
    const AurReviewedSourceReviewIdentity identity = review_identity();

    const std::string invalid_document = "schema_version = 1\n";
    const std::string corrupted_document;
    const ReviewedSourceState wrong_state = ReviewedSourceState::make(
        aur_package_base(
            "other-base",
            "https://aur.archlinux.org/other-base.git"),
        SourceRevisionIdentity::git_commit(std::string(SHA1_B)));
    const std::string mismatch_document =
        encode_reviewed_source_state(wrong_state);

    const struct {
        std::string document;
        ReviewedSourceAbnormalStateReason reason;
    } cases[]{
        {invalid_document, ReviewedSourceAbnormalStateReason::Invalid},
        {corrupted_document,
         ReviewedSourceAbnormalStateReason::Corrupted},
        {mismatch_document,
         ReviewedSourceAbnormalStateReason::SourceMismatch},
    };

    for(std::size_t index = 0; index < std::size(cases); ++index) {
        const ReviewedSourceStateStoreRead read = observed_read(
            cases[index].document, identity.package_base(), index + 20);
        const ReviewedSourceStateStoreRead exact_read = read;
        ReviewedSourceLifecyclePlanResult result =
            plan_reviewed_source_lifecycle(identity, read);
        const auto& requirement =
            require_arm<ReviewedSourceReviewRequirement>(
                result,
                "Abnormal state did not require explicit rebind.");
        require(requirement.kind() ==
                        ReviewedSourceReviewRequirementKind::
                            AbnormalStateRebindFullReview &&
                    requirement.abnormal_reason() != nullptr &&
                    *requirement.abnormal_reason() == cases[index].reason,
                "Abnormal state was flattened into ordinary Missing.");
        require(requirement.expected_state_observation().store_read() ==
                    exact_read,
                "Abnormal expected store observation was not retained exactly.");
        require(requirement.expected_state_observation()
                    .observed_record()
                    .has_value(),
                "Abnormal observed record was flattened to expected-null.");
    }
}

void test_future_unsafe_and_store_failure_fail_closed_without_authority() {
    const AurReviewedSourceReviewIdentity identity = review_identity();
    const std::string future_document =
        "schema_version = 2\nfuture_field = true\n";
    const ReviewedSourceLifecyclePlanResult future_result =
        plan_reviewed_source_lifecycle(
            identity,
            observed_read(
                future_document, identity.package_base()));
    const ReviewedSourceOperationStop& future =
        require_arm<ReviewedSourceOperationStop>(
            future_result,
            "Unsupported future state did not fail closed.");
    require(future.reason() ==
                    ReviewedSourceOperationStopReason::UnsupportedFuture &&
                require_fatal_lifecycle(
                    future, "Future state lost FatalState lifecycle.")
                        .reason ==
                    ReviewedSourceFatalStateReason::UnsupportedFuture,
            "Unsupported future state produced the wrong stop reason.");
    require(future.fatal_state_failure().has_value() &&
                future.fatal_state_failure()->store_read.has_value() &&
                std::holds_alternative<
                    ReviewedSourceStateUnsupportedFuture>(
                    future.fatal_state_failure()
                        ->store_read->observation),
            "Unsupported future state lost its exact read payload.");

    const ReviewedSourceLifecyclePlanResult unsafe_result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreUnsafeHistory{
                ReviewedSourceStateStoreHistoryIssue::ForkDetected,
                "/state/example-base",
                {"1.toml", "2-a.toml", "2-b.toml"},
                1,
                2});
    const ReviewedSourceOperationStop& unsafe =
        require_arm<ReviewedSourceOperationStop>(
            unsafe_result,
            "Unsafe history did not fail closed.");
    require(unsafe.reason() ==
                    ReviewedSourceOperationStopReason::UnsafeHistory &&
                require_fatal_lifecycle(
                    unsafe, "Unsafe history lost FatalState lifecycle.")
                        .reason ==
                    ReviewedSourceFatalStateReason::UnsafeHistory,
            "Unsafe history produced the wrong stop reason.");
    require(unsafe.fatal_state_failure().has_value() &&
                unsafe.fatal_state_failure()->unsafe_history.has_value() &&
                unsafe.fatal_state_failure()->unsafe_history->issue ==
                    ReviewedSourceStateStoreHistoryIssue::ForkDetected,
            "Unsafe history lost its store payload.");

    const ReviewedSourceLifecyclePlanResult failure_result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreFailure{
                ReviewedSourceStateStoreFailureKind::ReadFailed,
                "/state/example-base/1.toml",
                std::nullopt,
                std::nullopt,
                std::nullopt});
    const ReviewedSourceOperationStop& failure =
        require_arm<ReviewedSourceOperationStop>(
            failure_result,
            "Store failure did not fail closed.");
    require(failure.reason() ==
                    ReviewedSourceOperationStopReason::StoreFailure &&
                require_fatal_lifecycle(
                    failure, "Store failure lost FatalState lifecycle.")
                        .reason ==
                    ReviewedSourceFatalStateReason::StoreFailure,
            "Store failure produced the wrong stop reason.");
    require(failure.fatal_state_failure().has_value() &&
                failure.fatal_state_failure()->store_failure.has_value() &&
                failure.fatal_state_failure()->store_failure->kind ==
                    ReviewedSourceStateStoreFailureKind::ReadFailed,
            "Store failure lost its filesystem payload.");
}

void test_inconsistent_store_observation_fails_closed() {
    const AurReviewedSourceReviewIdentity identity = review_identity();
    const ReviewedSourceLifecyclePlanResult missing_with_record_result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreRead{
                ReviewedSourceStateMissing{},
                observed_record("unexpected")});
    const ReviewedSourceOperationStop& missing_with_record =
        require_arm<ReviewedSourceOperationStop>(
            missing_with_record_result,
            "Missing with an observed record was accepted.");
    require(missing_with_record.reason() ==
                ReviewedSourceOperationStopReason::
                    InconsistentStoreObservation,
            "Inconsistent Missing produced the wrong stop reason.");
    require(missing_with_record.fatal_state_failure().has_value() &&
                missing_with_record.fatal_state_failure()
                    ->store_read.has_value() &&
                missing_with_record.fatal_state_failure()
                    ->store_read->observed.has_value(),
            "Inconsistent observation lost its exact store snapshot.");

    const ReviewedSourceLifecyclePlanResult abnormal_without_record_result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreRead{
                ReviewedSourceStateCorrupted{
                    ReviewedSourceStateCorruptedReason::
                        EmptyDocument},
                std::nullopt});
    const ReviewedSourceOperationStop& abnormal_without_record =
        require_arm<ReviewedSourceOperationStop>(
            abnormal_without_record_result,
            "Observed corruption without its record was accepted.");
    require(abnormal_without_record.reason() ==
                ReviewedSourceOperationStopReason::
                    InconsistentStoreObservation,
            "Inconsistent abnormal observation produced wrong stop reason.");

    const ReviewedSourceState state = state_for(identity, std::string(SHA1_A));
    const ReviewedSourceLifecyclePlanResult mismatched_raw_result =
        plan_reviewed_source_lifecycle(
            identity,
            ReviewedSourceStateStoreRead{
                ReviewedSourceStateLoaded{state},
                observed_record("")});
    const ReviewedSourceOperationStop& mismatched_raw =
        require_arm<ReviewedSourceOperationStop>(
            mismatched_raw_result,
            "Typed observation/raw record mismatch was accepted.");
    require(mismatched_raw.reason() ==
                ReviewedSourceOperationStopReason::
                    InconsistentStoreObservation,
            "Raw record mismatch produced the wrong stop reason.");
}

void test_bootstrap_full_review_preserves_existing_cas_observation() {
    const auto identity = review_identity();
    for(const auto& revision : {std::string(), std::string(SHA1_A), std::string(SHA1_B)}) {
        const auto observed = revision.empty() ? missing_read() : observed_read(encode_reviewed_source_state(state_for(identity, revision)), identity.package_base());
        set_reviewed_source_lifecycle_store_result_for_test(observed);
        auto preflight = preflight_reviewed_source_fatal_state(identity.package_base());
        auto result = plan_reviewed_source_lifecycle_from_preflight(identity,
                                                                    std::move(std::get<ReviewedSourceFatalStatePreflight>(preflight)), ReviewedSourceReviewPurpose::DevelTrackingBootstrap);
        const auto& requirement = require_arm<ReviewedSourceReviewRequirement>(result, "Bootstrap skipped full review");
        require(requirement.kind() == ReviewedSourceReviewRequirementKind::BootstrapFullReview && !requirement.baseline(),
                "Bootstrap used incremental or already-reviewed continuation");
        require(requirement.expected_state_observation().store_read() == observed,
                "Bootstrap erased or changed the exact CAS observation");
    }
}

} // namespace

int main() {
    try {
        test_bootstrap_full_review_preserves_existing_cas_observation();
        test_typed_identity_reuses_aur_package_base_and_exact_revision();
        test_split_children_share_one_package_base_review_identity();
        test_missing_maps_to_initial_full_review_with_expected_null();
        test_loaded_same_is_already_reviewed_without_rewrite_authority();
        test_loaded_different_maps_to_update_review();
        test_abnormal_records_require_explicit_rebind_and_remain_exact();
        test_future_unsafe_and_store_failure_fail_closed_without_authority();
        test_inconsistent_store_observation_fails_closed();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "reviewed source lifecycle tests: all checks passed\n";
    return 0;
}
