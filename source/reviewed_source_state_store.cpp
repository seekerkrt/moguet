#include "reviewed_source_state_store.hpp"

#include <new>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <utility>

namespace {

constexpr std::string_view REVIEWED_SOURCE_TEMPORARY_PREFIX =
    "-.moguet-reviewed-source-";

void require_aur_known_package_base(const PackageBaseIdentity& package_base) {
    const PackageSourceIdentity& source = package_base.source();
    if(source.kind() != PackageSourceKind::Aur) {
        throw std::invalid_argument(
            "Reviewed source state store requires an AUR PackageBase identity.");
    }
    const SourceLocationIdentity& location = source.location();
    if(location.kind() != SourceLocationKind::GitRemote ||
       location.state() != SourceLocationState::Known ||
       location.value() == nullptr) {
        throw std::invalid_argument(
            "Reviewed source state store requires a known AUR Git remote.");
    }
}

bool is_future_reviewed_source_document(std::string_view document) {
    return std::holds_alternative<ReviewedSourceStateUnsupportedFuture>(
        decode_reviewed_source_state(document));
}

ReviewedSourceStateStoreFailure resolution_failure(
    const std::filesystem::path& entry_path) {
    return ReviewedSourceStateStoreFailure{
        ReviewedSourceStateStoreFailureKind::AuthorityUnavailable,
        entry_path, std::nullopt, std::nullopt, std::nullopt};
}

XdgGenerationStoreConfiguration configuration_for(
    const PackageBaseIdentity& package_base) {
    require_aur_known_package_base(package_base);
    return XdgGenerationStoreConfiguration{
        reviewed_source_state_store_paths(), package_base.package_base(),
        std::string(REVIEWED_SOURCE_TEMPORARY_PREFIX),
        reviewed_source_state_store_max_record_bytes,
        &is_future_reviewed_source_document};
}

} // namespace

xdg_paths::ReviewedSourceStatePaths reviewed_source_state_store_paths() {
    return xdg_paths::resolve_reviewed_source_state_process_environment();
}

std::filesystem::path reviewed_source_state_store_directory() {
    return reviewed_source_state_store_paths().directory;
}

std::filesystem::path reviewed_source_state_store_entry_path(
    const PackageBaseIdentity& package_base) {
    return xdg_generation_store_entry_path(configuration_for(package_base));
}

std::string reviewed_source_state_store_origin_leaf() {
    return xdg_generation_store_origin_leaf();
}

std::string reviewed_source_state_store_successor_leaf(
    std::uint64_t next_generation,
    const ReviewedSourceStateRecordIdentity& predecessor,
    std::string_view predecessor_raw_contents) {
    return xdg_generation_store_successor_leaf(
        next_generation, predecessor, predecessor_raw_contents);
}

ReviewedSourceStateStoreReadResult read_reviewed_source_state(
    const PackageBaseIdentity& expected_package_base) {
    XdgGenerationStoreConfiguration configuration{
        {}, "unresolved", std::string(REVIEWED_SOURCE_TEMPORARY_PREFIX), reviewed_source_state_store_max_record_bytes, &is_future_reviewed_source_document};
    try {
        configuration = configuration_for(expected_package_base);
    } catch(const xdg_paths::ResolutionError&) {
        return resolution_failure(expected_package_base.package_base());
    }

    XdgGenerationStoreReadResult result =
        read_xdg_generation_store(configuration);
    if(std::holds_alternative<XdgGenerationStoreMissing>(result)) {
        return ReviewedSourceStateStoreRead{
            ReviewedSourceStateMissing{}, std::nullopt};
    }
    if(auto* loaded = std::get_if<XdgGenerationStoreLoaded>(&result)) {
        const ReviewedSourceStateInterpretation interpreted =
            interpret_reviewed_source_state(
                loaded->observed.raw_contents, expected_package_base);
        ReviewedSourceStateObservation observation = std::visit(
            [](const auto& arm) -> ReviewedSourceStateObservation {
                return arm;
            },
            interpreted);
        return ReviewedSourceStateStoreRead{
            std::move(observation), std::move(loaded->observed)};
    }
    if(auto* unsafe =
           std::get_if<XdgGenerationStoreUnsafeHistory>(&result)) {
        return std::move(*unsafe);
    }
    return std::move(std::get<XdgGenerationStoreFailure>(result));
}

namespace {

ReviewedSourceStateStorePublishResult publish_state(
    const ReviewedSourceState& next_state,
    const std::optional<ReviewedSourceStateObservedRecord>&
        expected_observed) {
    if(next_state.schema_version() != reviewed_source_state_schema_version) {
        throw std::invalid_argument(
            "Reviewed source state store cannot publish a non-current schema.");
    }

    XdgGenerationStoreConfiguration configuration{
        {}, "unresolved", std::string(REVIEWED_SOURCE_TEMPORARY_PREFIX), reviewed_source_state_store_max_record_bytes, &is_future_reviewed_source_document};
    try {
        configuration = configuration_for(next_state.package_base());
    } catch(const xdg_paths::ResolutionError&) {
        return resolution_failure(next_state.package_base().package_base());
    }

    // The shared store may already have committed when it returns. Own both
    // semantic success arms before entering it, just like provenance storage.
    ReviewedSourceState success_state = next_state;
    XdgGenerationStorePublishResult result = publish_xdg_generation_store(
        configuration, encode_reviewed_source_state(next_state),
        expected_observed);
    if(auto* published =
           std::get_if<XdgGenerationStorePublished>(&result)) {
        return ReviewedSourceStateStorePublished{
            std::move(success_state), std::move(published->observed)};
    }
    if(auto* uncertain =
           std::get_if<XdgGenerationStorePublishedUncertain>(&result)) {
        return ReviewedSourceStateStorePublishedUncertain{
            std::move(success_state), std::move(uncertain->observed), uncertain->issue,
            uncertain->failure_kind, std::move(uncertain->entry_path),
            std::move(uncertain->leftover_artifact),
            std::move(uncertain->system_error)};
    }
    if(auto* unsafe =
           std::get_if<XdgGenerationStoreUnsafeHistory>(&result)) {
        return std::move(*unsafe);
    }
    return std::move(std::get<XdgGenerationStoreFailure>(result));
}

} // namespace

ReviewedSourceStateStorePublishResult publish_reviewed_source_state(
    const ReviewedSourceState& next_state,
    const std::optional<ReviewedSourceStateObservedRecord>& expected_observed) {
    static_assert(std::is_nothrow_move_constructible_v<ReviewedSourceStateStorePublishResult>);
    try {
        return publish_state(next_state, expected_observed);
    } catch(const std::bad_alloc&) {
        return ReviewedSourceStateStoreFailure{
            ReviewedSourceStateStoreFailureKind::ResourceFailure, {}, std::nullopt, std::nullopt, std::nullopt};
    } catch(const std::length_error&) {
        return ReviewedSourceStateStoreFailure{
            ReviewedSourceStateStoreFailureKind::ResourceFailure, {}, std::nullopt, std::nullopt, std::nullopt};
    }
}

#ifdef MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS
void fail_next_reviewed_source_state_store_operation_for_test(
    ReviewedSourceStateStoreTestFailurePoint failure_point) {
    fail_next_xdg_generation_store_operation_for_test(failure_point);
}

void run_reviewed_source_state_store_race_once_for_test(
    ReviewedSourceStateStoreTestRacePoint race_point,
    ReviewedSourceStateStoreTestRaceHandler handler) {
    run_xdg_generation_store_race_once_for_test(race_point, handler);
}

void simulate_coarse_reviewed_source_state_store_timestamps_for_test() {
    simulate_coarse_xdg_generation_store_timestamps_for_test();
}

void reset_reviewed_source_state_store_test_hooks() {
    reset_xdg_generation_store_test_hooks();
}
#endif
