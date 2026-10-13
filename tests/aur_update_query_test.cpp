#include "aur_update_query.hpp"

#include "aur_rpc.hpp"
#include "package_metadata.hpp"
#include "process.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using InfoManyHandler = std::function<std::map<std::string, AurPackageInfo>(
    const std::vector<std::string>&)>;
using InfoStrictHandler =
    std::function<std::optional<AurPackageInfo>(const std::string&)>;
using ExecHandler = std::function<CapturedCommandResult(const std::string&)>;

struct QueryFixture {
    ForeignPackageInventory inventory;
    std::vector<std::vector<std::string>> info_many_calls;
    std::vector<std::string> info_strict_calls;
    std::vector<std::string> exec_calls;
    std::vector<std::string> events;
    InfoManyHandler info_many_handler;
    InfoStrictHandler info_strict_handler;
    ExecHandler exec_handler;
    PacmanRepositoryConfiguration configuration;
    std::optional<PacmanRepositoryConfiguration> observed_configuration;
    std::optional<PackageMetadataFailure> configuration_failure;
    std::optional<PackageMetadataFailure> inventory_failure;
    std::size_t configuration_calls = 0;
    std::size_t inventory_calls = 0;
};

QueryFixture g_fixture;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename ExpectedException, typename Callable>
void expect_exception(
    Callable callable, const std::string& expected_message) {
    try {
        callable();
    } catch(const ExpectedException& error) {
        expect(
            std::string(error.what()) == expected_message,
            "Unexpected exception message: expected [" + expected_message +
                "], actual [" + error.what() + "]");
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Unexpected exception category: " + std::string(error.what()));
    }

    throw std::runtime_error("Expected exception: " + expected_message);
}

template <typename Callable>
void expect_package_metadata_error(
    Callable callable, PackageMetadataErrorCode expected_code,
    const std::string& expected_message) {
    try {
        callable();
    } catch(const PackageMetadataError& error) {
        expect(
            error.failure().code == expected_code,
            "Unexpected package metadata error code");
        expect(
            error.failure().diagnostic == expected_message,
            "Unexpected package metadata diagnostic: expected [" +
                expected_message + "], actual [" +
                error.failure().diagnostic + "]");
        expect(
            std::string(error.what()) == expected_message,
            "Package metadata exception message differs");
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Unexpected exception category: " + std::string(error.what()));
    }

    throw std::runtime_error(
        "Expected package metadata error: " + expected_message);
}

AurPackageInfo package_info(
    const std::string& name, const std::string& version = "1.0-1",
    const std::string& package_base = "") {
    AurPackageInfo info;
    info.Name = name;
    info.PackageBase = package_base.empty() ? name : package_base;
    info.Version = version;
    info.Description = "AUR update query fixture";
    info.Maintainer = "moguet-test";
    return info;
}

std::map<std::string, AurPackageInfo> metadata_for(
    const std::vector<std::string>& package_names) {
    std::map<std::string, AurPackageInfo> result;
    for(const auto& package_name : package_names) {
        result.emplace(package_name, package_info(package_name));
    }
    return result;
}

void reset_fixture() {
    g_fixture = QueryFixture{};
    g_fixture.configuration = PacmanRepositoryConfiguration{
        PacmanDatabasePaths{"/fixture-root", "/fixture-db"},
        {"fixture-core", "fixture-extra"}};
    g_fixture.info_many_handler = [](const std::vector<std::string>& package_names) {
        return metadata_for(package_names);
    };
    g_fixture.info_strict_handler = [](const std::string& package_name) {
        return std::optional<AurPackageInfo>(package_info(package_name));
    };
    g_fixture.exec_handler = [](const std::string& command) {
        if(!command.starts_with("vercmp ")) {
            throw std::runtime_error("Unexpected query command: " + command);
        }
        return CapturedCommandResult{"0", 0};
    };
}

ForeignPackageInventory make_inventory(
    std::size_t count, const std::string& prefix) {
    ForeignPackageInventory packages;
    packages.reserve(count);
    for(std::size_t i = 0; i < count; ++i) {
        packages.push_back(InstalledPackageMetadata{
            prefix + "-" + std::to_string(i + 1),
            "1.0-1",
            InstalledPackageReason::Unknown});
    }
    return packages;
}

std::vector<std::string> package_names(
    const ForeignPackageInventory& packages) {
    std::vector<std::string> names;
    names.reserve(packages.size());
    for(const auto& package : packages)
        names.push_back(package.name);
    return names;
}

void expect_plan_matches_inventory(
    const AurUpdatePlan& plan,
    const ForeignPackageInventory& inventory) {
    expect(plan.entries.size() == inventory.size(), "Plan entry count differs");
    for(std::size_t i = 0; i < inventory.size(); ++i) {
        expect(
            plan.entries[i].installed_name == inventory[i].name,
            "Plan installed-name order differs at index " + std::to_string(i));
        expect(
            plan.entries[i].installed_version == inventory[i].version,
            "Plan installed version differs at index " + std::to_string(i));
        expect(
            plan.entries[i].install_reason == inventory[i].reason,
            "Plan install reason differs at index " + std::to_string(i));
    }
}

void expect_classification(
    const AurUpdatePlan& plan, std::size_t index,
    AurUpdateClassification expected, const std::string& message) {
    expect(index < plan.entries.size(), "Classification index is outside the plan");
    expect(plan.entries[index].classification == expected, message);
}

std::size_t event_index(const std::string& expected_event) {
    const auto found =
        std::find(g_fixture.events.begin(), g_fixture.events.end(), expected_event);
    expect(found != g_fixture.events.end(), "Missing dependency event: " + expected_event);
    return static_cast<std::size_t>(found - g_fixture.events.begin());
}

void expect_aur_query_not_started(const std::string& context) {
    expect(
        g_fixture.info_many_calls.empty(),
        context + ": bulk AUR info was queried");
    expect(
        g_fixture.info_strict_calls.empty(),
        context + ": strict per-package AUR info was queried");
    expect(g_fixture.exec_calls.empty(), context + ": vercmp was invoked");
}

} // namespace

// WHY: query orchestrationだけをisolatedに固定するため、production transport、
// package metadata read phase、process runnerをlinkせず同じsymbolを差し込む。
PackageMetadataError::PackageMetadataError(PackageMetadataFailure failure)
    : std::runtime_error(failure.diagnostic), failure_(std::move(failure)) {
}

const PackageMetadataFailure& PackageMetadataError::failure() const noexcept {
    return failure_;
}

PacmanRepositoryConfiguration resolve_pacman_repository_configuration() {
    ++g_fixture.configuration_calls;
    g_fixture.events.push_back("configuration");
    if(g_fixture.configuration_failure.has_value()) {
        throw PackageMetadataError(g_fixture.configuration_failure.value());
    }
    return g_fixture.configuration;
}

ForeignPackageInventoryResult query_foreign_package_inventory(
    const PacmanRepositoryConfiguration& configuration) {
    ++g_fixture.inventory_calls;
    g_fixture.observed_configuration = configuration;
    g_fixture.events.push_back("inventory-open");
    // production APIはowned valueを返す前にlibalpm read phaseを破棄する。
    g_fixture.events.push_back("inventory-release");
    if(g_fixture.inventory_failure.has_value()) {
        return g_fixture.inventory_failure.value();
    }
    return g_fixture.inventory;
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    g_fixture.info_many_calls.push_back(package_names);
    g_fixture.events.push_back(
        "info-many:" +
        (package_names.empty() ? std::string() : package_names.front()));
    return g_fixture.info_many_handler(package_names);
}

std::optional<AurPackageInfo> AurClient::info_strict(
    const std::string& package_name) {
    g_fixture.info_strict_calls.push_back(package_name);
    g_fixture.events.push_back("info-strict:" + package_name);
    return g_fixture.info_strict_handler(package_name);
}

CapturedCommandResult capture_command_output(const char* command) {
    if(command == nullptr) throw std::runtime_error("Null query command");

    const std::string command_string(command);
    g_fixture.exec_calls.push_back(command_string);
    g_fixture.events.push_back("exec:" + command_string);
    return g_fixture.exec_handler(command_string);
}

namespace {

void test_empty_inventory_skips_aur_queries() {
    reset_fixture();

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect(result.plan.entries.empty(), "Empty inventory produced plan entries");
    expect(
        result.recoverable_failures.empty(),
        "Empty inventory produced recoverable failures");
    expect(
        g_fixture.configuration_calls == 1,
        "Configuration was not resolved exactly once");
    expect(g_fixture.inventory_calls == 1, "Inventory was not queried exactly once");
    expect(
        g_fixture.observed_configuration.has_value(),
        "Resolved configuration was not passed to inventory");
    expect(
        g_fixture.observed_configuration->database_paths.root_dir ==
                g_fixture.configuration.database_paths.root_dir &&
            g_fixture.observed_configuration->database_paths.db_path ==
                g_fixture.configuration.database_paths.db_path &&
            g_fixture.observed_configuration->repository_names ==
                g_fixture.configuration.repository_names,
        "Inventory received a different repository configuration");
    expect_aur_query_not_started("Empty inventory");
}

void test_explicit_inventory_core_skips_inventory_boundary_and_preserves_order() {
    reset_fixture();
    ForeignPackageInventory inventory = {
        {"core-zeta", "1.0-1", InstalledPackageReason::Explicit},
        {"core-alpha", "2.0-1", InstalledPackageReason::Dependency}};
    const ForeignPackageInventory expected_inventory = inventory;

    const AurUpdateQueryResult result =
        query_aur_updates_for_foreign_inventory(std::move(inventory));

    expect(
        g_fixture.configuration_calls == 0,
        "Explicit inventory core resolved repository configuration");
    expect(
        g_fixture.inventory_calls == 0,
        "Explicit inventory core queried installed packages");
    expect(
        !g_fixture.observed_configuration.has_value(),
        "Explicit inventory core passed configuration to inventory query");
    expect_plan_matches_inventory(result.plan, expected_inventory);
    expect(
        result.recoverable_failures.empty(),
        "Successful explicit inventory core produced a query failure");
    expect(
        g_fixture.info_many_calls ==
            std::vector<std::vector<std::string>>{
                {"core-zeta", "core-alpha"}},
        "Explicit inventory core changed AUR query order");
    expect(
        g_fixture.info_strict_calls.empty(),
        "Non-empty explicit inventory query used strict fallback");
    expect(
        g_fixture.exec_calls.size() == expected_inventory.size(),
        "Explicit inventory core did not compare every queried package");
}

void test_configuration_failure_stops_before_inventory_and_aur_queries() {
    reset_fixture();
    g_fixture.configuration_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::ConfigurationUnavailable,
        "configuration unavailable"};

    expect_package_metadata_error(
        []() { static_cast<void>(query_installed_aur_updates()); },
        PackageMetadataErrorCode::ConfigurationUnavailable,
        "configuration unavailable");

    expect(
        g_fixture.configuration_calls == 1,
        "Configuration failure did not resolve exactly once");
    expect(
        g_fixture.inventory_calls == 0,
        "Configuration failure reached inventory");
    expect_aur_query_not_started("Configuration failure");
}

void test_inventory_failure_propagates_without_aur_queries() {
    reset_fixture();
    g_fixture.inventory_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "foreign inventory failed"};

    expect_package_metadata_error(
        []() { static_cast<void>(query_installed_aur_updates()); },
        PackageMetadataErrorCode::QueryFailed,
        "foreign inventory failed");

    expect(
        g_fixture.inventory_calls == 1,
        "Inventory failure did not query inventory exactly once");
    expect(
        event_index("inventory-open") < event_index("inventory-release"),
        "Inventory failure did not release its read phase");
    expect_aur_query_not_started("Inventory failure");
}

void test_one_through_one_hundred_packages_use_one_batch() {
    for(const std::size_t count : {std::size_t(1), std::size_t(100)}) {
        reset_fixture();
        g_fixture.inventory = make_inventory(count, "bounded");
        const std::vector<std::string> expected_names =
            package_names(g_fixture.inventory);

        const AurUpdateQueryResult result = query_installed_aur_updates();

        expect_plan_matches_inventory(result.plan, g_fixture.inventory);
        expect(
            result.recoverable_failures.empty(),
            "Successful bounded batch produced a failure");
        expect(g_fixture.info_many_calls.size() == 1, "Bounded query did not use one batch");
        expect(
            g_fixture.info_many_calls.front() == expected_names,
            "Bounded batch did not preserve installed order");
        expect(
            g_fixture.info_strict_calls.empty(),
            "Non-empty bounded batch used fallback");
    }
}

void test_one_hundred_and_one_packages_use_two_batches() {
    reset_fixture();
    g_fixture.inventory = make_inventory(101, "split");
    const std::vector<std::string> expected_names = package_names(g_fixture.inventory);

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    expect(g_fixture.info_many_calls.size() == 2, "101 packages did not use two batches");
    expect(
        g_fixture.info_many_calls[0] == std::vector<std::string>(
                                            expected_names.begin(),
                                            expected_names.begin() + 100),
        "First 101-package batch was not the installed-order first 100");
    expect(
        g_fixture.info_many_calls[1] == std::vector<std::string>{expected_names[100]},
        "Second 101-package batch was not the installed-order remainder");
    expect(
        g_fixture.info_strict_calls.empty(),
        "Non-empty 101-package batches used fallback");
}

void test_empty_batch_uses_per_package_fallback_and_continues() {
    reset_fixture();
    g_fixture.inventory = make_inventory(101, "fallback");
    const std::vector<std::string> expected_names = package_names(g_fixture.inventory);
    g_fixture.info_many_handler = [](const std::vector<std::string>& requested) {
        if(requested.size() == 100) return std::map<std::string, AurPackageInfo>{};
        return metadata_for(requested);
    };
    g_fixture.info_strict_handler =
        [](const std::string& package_name) -> std::optional<AurPackageInfo> {
        if(package_name == "fallback-50") return std::nullopt;
        return package_info(package_name);
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    expect(
        result.recoverable_failures.empty(),
        "Successful per-package fallback produced a failure");
    expect(g_fixture.info_many_calls.size() == 2, "Fallback query skipped the later batch");
    expect(
        g_fixture.info_strict_calls == std::vector<std::string>(
                                           expected_names.begin(),
                                           expected_names.begin() + 100),
        "Strict fallback did not query exactly the empty batch in installed order");
    expect(
        event_index("info-strict:fallback-100") <
            event_index("info-many:fallback-101"),
        "Later batch ran before empty-batch fallback completed");
    expect_classification(
        result.plan, 49, AurUpdateClassification::NonAurForeign,
        "Confirmed fallback absence was not classified as non-AUR");
}

void test_partial_non_empty_batch_does_not_fallback() {
    reset_fixture();
    g_fixture.inventory = {
        {"partial-first", "1.0-1"},
        {"partial-missing", "1.0-1"},
        {"partial-last", "1.0-1"}};
    g_fixture.info_many_handler = [](const std::vector<std::string>&) {
        return std::map<std::string, AurPackageInfo>{
            {"partial-first", package_info("partial-first")},
            {"partial-last", package_info("partial-last")}};
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect(g_fixture.info_many_calls.size() == 1, "Partial batch query count differs");
    expect(
        g_fixture.info_strict_calls.empty(),
        "Partial non-empty batch used fallback");
    expect_classification(
        result.plan, 0, AurUpdateClassification::UpToDate,
        "First present package was not classified as up to date");
    expect_classification(
        result.plan, 1, AurUpdateClassification::NonAurForeign,
        "Missing partial result was not classified as confirmed non-AUR");
    expect_classification(
        result.plan, 2, AurUpdateClassification::UpToDate,
        "Last present package was not classified as up to date");
}

void test_ordinary_batch_failure_is_recoverable_and_later_batch_succeeds() {
    reset_fixture();
    g_fixture.inventory = make_inventory(101, "recoverable");
    const std::vector<std::string> expected_names = package_names(g_fixture.inventory);
    g_fixture.info_many_handler = [](const std::vector<std::string>& requested) {
        if(requested.size() == 100) throw std::runtime_error("ordinary batch failure");
        return metadata_for(requested);
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect(g_fixture.info_many_calls.size() == 2, "Ordinary failure stopped later batches");
    expect(result.recoverable_failures.size() == 1, "Ordinary failure was not aggregated");
    expect(
        result.recoverable_failures[0].package_names ==
            std::vector<std::string>(
                expected_names.begin(), expected_names.begin() + 100),
        "Ordinary failure lost failed batch identity or order");
    expect(
        result.recoverable_failures[0].diagnostic == "ordinary batch failure",
        "Ordinary failure diagnostic differs");
    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    for(std::size_t i = 0; i < 100; ++i) {
        expect_classification(
            result.plan, i, AurUpdateClassification::MetadataUnavailable,
            "Failed batch package was not metadata-unavailable");
    }
    expect_classification(
        result.plan, 100, AurUpdateClassification::UpToDate,
        "Later successful batch was not reflected in the plan");
    expect(g_fixture.exec_calls.size() == 1, "Failed batch unexpectedly invoked vercmp");
}

void test_ordinary_fallback_failure_marks_batch_unavailable_and_continues() {
    reset_fixture();
    g_fixture.inventory = make_inventory(101, "fallback-failure");
    const std::vector<std::string> expected_names = package_names(g_fixture.inventory);
    g_fixture.info_many_handler = [](const std::vector<std::string>& requested) {
        if(requested.size() == 100) return std::map<std::string, AurPackageInfo>{};
        return metadata_for(requested);
    };
    g_fixture.info_strict_handler =
        [](const std::string& package_name) -> std::optional<AurPackageInfo> {
        if(package_name == "fallback-failure-3") {
            throw std::runtime_error("ordinary fallback failure");
        }
        return package_info(package_name);
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect(
        g_fixture.info_many_calls.size() == 2,
        "Ordinary fallback failure stopped later batches");
    expect(
        g_fixture.info_strict_calls ==
            std::vector<std::string>{
                "fallback-failure-1", "fallback-failure-2",
                "fallback-failure-3"},
        "Ordinary fallback failure did not stop the failed batch immediately");
    expect(
        result.recoverable_failures.size() == 1,
        "Ordinary fallback failure was not aggregated");
    expect(
        result.recoverable_failures[0].package_names ==
            std::vector<std::string>(
                expected_names.begin(), expected_names.begin() + 100),
        "Ordinary fallback failure lost failed batch identity or order");
    expect(
        result.recoverable_failures[0].diagnostic ==
            "ordinary fallback failure",
        "Ordinary fallback failure diagnostic differs");
    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    for(std::size_t i = 0; i < 100; ++i) {
        expect_classification(
            result.plan, i, AurUpdateClassification::MetadataUnavailable,
            "Fallback-failed batch package was not metadata-unavailable");
    }
    expect_classification(
        result.plan, 100, AurUpdateClassification::UpToDate,
        "Batch after ordinary fallback failure was not reflected in the plan");
    expect(
        g_fixture.exec_calls.size() == 1,
        "Fallback-failed batch unexpectedly invoked vercmp");
}

void test_batch_schema_failure_propagates_immediately() {
    reset_fixture();
    g_fixture.inventory = make_inventory(101, "schema-batch");
    g_fixture.info_many_handler = [](const std::vector<std::string>&) -> std::map<std::string, AurPackageInfo> {
        throw AurRpcResponseError("schema batch failure");
    };

    expect_exception<AurRpcResponseError>(
        []() { static_cast<void>(query_installed_aur_updates()); },
        "schema batch failure");

    expect(g_fixture.info_many_calls.size() == 1, "Schema failure did not stop batching");
    expect(
        g_fixture.info_strict_calls.empty(),
        "Batch schema failure entered fallback");
    expect(g_fixture.exec_calls.empty(), "Batch schema failure reached version comparison");
}

void test_fallback_schema_failure_propagates_immediately() {
    reset_fixture();
    g_fixture.inventory = make_inventory(101, "schema-fallback");
    g_fixture.info_many_handler = [](const std::vector<std::string>&) {
        return std::map<std::string, AurPackageInfo>{};
    };
    g_fixture.info_strict_handler =
        [](const std::string&) -> std::optional<AurPackageInfo> {
        throw AurRpcResponseError("schema fallback failure");
    };

    expect_exception<AurRpcResponseError>(
        []() { static_cast<void>(query_installed_aur_updates()); },
        "schema fallback failure");

    expect(g_fixture.info_many_calls.size() == 1, "Fallback schema failure reached later batch");
    expect(
        g_fixture.info_strict_calls.size() == 1,
        "Fallback schema failure did not stop immediately");
    expect(
        g_fixture.info_strict_calls.front() == "schema-fallback-1",
        "Fallback schema failure occurred on an unexpected package");
    expect(g_fixture.exec_calls.empty(), "Fallback schema failure reached version comparison");
}

void test_version_comparison_classifies_and_fails_closed() {
    reset_fixture();
    g_fixture.inventory = {
        {"version-newer", "installed-newer"},
        {"version-same", "installed-same"},
        {"version-older", "installed-older"},
        {"version-invalid", "installed-invalid"},
        {"version-prefix-junk", "installed-prefix-junk"}};
    g_fixture.info_many_handler = [](const std::vector<std::string>&) {
        return std::map<std::string, AurPackageInfo>{
            {"version-newer", package_info("version-newer", "remote-newer")},
            {"version-same", package_info("version-same", "remote-same")},
            {"version-older", package_info("version-older", "remote-older")},
            {"version-invalid", package_info("version-invalid", "remote-invalid")},
            {"version-prefix-junk",
             package_info("version-prefix-junk", "remote-prefix-junk")}};
    };
    g_fixture.exec_handler = [](const std::string& command) {
        if(!command.starts_with("vercmp ")) {
            throw std::runtime_error("Unexpected query command: " + command);
        }
        if(command.find("remote-newer") != std::string::npos) return CapturedCommandResult{"1", 0};
        if(command.find("remote-same") != std::string::npos) return CapturedCommandResult{"0", 0};
        if(command.find("remote-older") != std::string::npos) return CapturedCommandResult{"-1", 0};
        if(command.find("remote-invalid") != std::string::npos) {
            return CapturedCommandResult{"invalid", 0};
        }
        if(command.find("remote-prefix-junk") != std::string::npos) {
            return CapturedCommandResult{"1junk", 0};
        }
        throw std::runtime_error("Unexpected vercmp fixture command: " + command);
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect_classification(
        result.plan, 0, AurUpdateClassification::UpdateAvailable,
        "Positive vercmp result was not update-available");
    expect_classification(
        result.plan, 1, AurUpdateClassification::UpToDate,
        "Zero vercmp result was not up to date");
    expect_classification(
        result.plan, 2, AurUpdateClassification::UpToDate,
        "Negative vercmp result was not up to date");
    expect_classification(
        result.plan, 3,
        AurUpdateClassification::VersionComparisonUnavailable,
        "Invalid vercmp output did not fail closed");
    expect_classification(
        result.plan, 4,
        AurUpdateClassification::VersionComparisonUnavailable,
        "Numeric-prefix junk vercmp output did not fail closed");
    expect(g_fixture.exec_calls.size() == 5, "Version fixture vercmp call count differs");

    // Fault injection hardens the result contract; no natural vercmp trigger is assumed.
    const std::vector<CapturedCommandResult> unavailable_results = {
        {"-1", 7}, {"0", 7}, {"1", 7}, {"-1garbage", 0}, {"0garbage", 0}, {"1garbage", 0}, {"1x", 0}, {"1 2", 0}, {"--1", 0}, {"garbage", 0}, {"", 0}, {"999999999999999999999999999999", 0}};
    for(const auto& comparison_result : unavailable_results) {
        reset_fixture();
        g_fixture.inventory = {{"version-failure", "1.0-1"}};
        g_fixture.exec_handler = [comparison_result](const std::string&) {
            return comparison_result;
        };
        const AurUpdateQueryResult unavailable = query_installed_aur_updates();
        expect_classification(
            unavailable.plan, 0,
            AurUpdateClassification::VersionComparisonUnavailable,
            "vercmp output [" + comparison_result.output + "] / exit " +
                std::to_string(comparison_result.exit_code) + " did not fail closed");
        expect(g_fixture.exec_calls.size() == 1, "Failure case did not invoke vercmp once");
    }
    expect(
        result.plan.entries[4].aur_package.has_value() &&
            result.plan.entries[4].aur_package->version == "remote-prefix-junk",
        "Failed comparison lost remote metadata");
}

void test_installed_and_aur_identity_are_preserved() {
    reset_fixture();
    g_fixture.inventory = {{"installed-query-name", "installed-version"}};
    g_fixture.info_many_handler = [](const std::vector<std::string>&) {
        return std::map<std::string, AurPackageInfo>{
            {"installed-query-name",
             package_info("aur-response-name", "aur-version", "split-package-base")}};
    };
    g_fixture.exec_handler = [](const std::string&) { return CapturedCommandResult{"1", 0}; };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect(result.plan.entries.size() == 1, "Identity fixture plan size differs");
    const AurUpdatePlanEntry& entry = result.plan.entries.front();
    expect(entry.installed_name == "installed-query-name", "Installed name was changed");
    expect(entry.installed_version == "installed-version", "Installed version was changed");
    expect(entry.aur_package.has_value(), "Identity fixture lost AUR metadata");
    expect(entry.aur_package->aur_name == "aur-response-name", "AUR Name differs");
    expect(entry.aur_package->package_base == "split-package-base", "PackageBase differs");
    expect(entry.aur_package->version == "aur-version", "AUR version differs");
}

void test_query_projects_exact_aur_suffix_candidates_conservatively() {
    reset_fixture();
    g_fixture.inventory = {
        {"normal-newer-git", "1.0-1", InstalledPackageReason::Explicit},
        {"split-cli", "1.0-1", InstalledPackageReason::Explicit},
        {"split-child-git", "1.0-1", InstalledPackageReason::Dependency},
        {"plain-package", "1.0-1", InstalledPackageReason::Explicit},
        {"non-aur-git", "1.0-1", InstalledPackageReason::Explicit},
        {"comparison-failed-git", "1.0-1", InstalledPackageReason::Explicit}};
    g_fixture.info_many_handler = [](const std::vector<std::string>&) {
        return std::map<std::string, AurPackageInfo>{
            {"normal-newer-git",
             package_info("normal-newer-git", "2.0-1")},
            {"split-cli",
             package_info("split-cli", "1.0-1", "split-suite-git")},
            {"split-child-git",
             package_info("split-child-git", "1.0-1", "split-suite")},
            {"plain-package", package_info("plain-package", "1.0-1")},
            {"comparison-failed-git",
             package_info("comparison-failed-git", "opaque-remote")}};
    };
    g_fixture.exec_handler = [comparison_index = std::size_t{0}](
                                 const std::string&) mutable {
        const std::size_t current_index = comparison_index++;
        if(current_index == 0) {
            return CapturedCommandResult{"1", 0};
        }
        if(current_index == 4) {
            return CapturedCommandResult{"invalid", 0};
        }
        return CapturedCommandResult{"0", 0};
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    expect(
        project_aur_update_effective_state(result.plan.entries[0]) ==
            AurUpdateEffectiveState::UpdateAvailable,
        "Query downgraded normal update precedence");
    expect(
        result.plan.entries[1].devel_classification.has_value() &&
            result.plan.entries[1]
                    .devel_classification->suffix_evidence()
                    .package_base_candidate_kind() != nullptr &&
            result.plan.entries[1]
                    .devel_classification->suffix_evidence()
                    .installed_children()
                    .front()
                    .candidate_kind() == nullptr &&
            project_aur_update_effective_state(
                result.plan.entries[1]) ==
                AurUpdateEffectiveState::RequiresCheck,
        "Query lost split PackageBase suffix evidence");
    expect(
        result.plan.entries[2].devel_classification.has_value() &&
            result.plan.entries[2]
                    .devel_classification->suffix_evidence()
                    .package_base_candidate_kind() == nullptr &&
            result.plan.entries[2]
                    .devel_classification->suffix_evidence()
                    .installed_children()
                    .front()
                    .candidate_kind() != nullptr &&
            project_aur_update_effective_state(
                result.plan.entries[2]) ==
                AurUpdateEffectiveState::RequiresCheck,
        "Query lost installed child suffix evidence");
    expect(
        project_aur_update_effective_state(result.plan.entries[3]) ==
            AurUpdateEffectiveState::UpToDate,
        "Query changed a normal no-suffix package");
    expect(
        project_aur_update_effective_state(result.plan.entries[4]) ==
                AurUpdateEffectiveState::NonAurForeign &&
            !result.plan.entries[4].devel_classification.has_value(),
        "Query promoted a non-AUR suffix package");
    expect(
        result.plan.entries[5].devel_assessment.state() ==
                DevelUpdateAssessmentState::RequiresCheck &&
            project_aur_update_effective_state(
                result.plan.entries[5]) ==
                AurUpdateEffectiveState::
                    VersionComparisonUnavailable,
        "Query changed version-comparison failure precedence");
}

void test_plan_preserves_inventory_order_instead_of_map_order() {
    reset_fixture();
    g_fixture.inventory = {
        {"zeta-package", "1.0-1"},
        {"middle-missing", "1.0-1"},
        {"alpha-package", "1.0-1"}};
    g_fixture.info_many_handler = [](const std::vector<std::string>&) {
        // std::map iteration is alpha then zeta; the plan must follow inventory instead.
        return std::map<std::string, AurPackageInfo>{
            {"zeta-package", package_info("zeta-package")},
            {"alpha-package", package_info("alpha-package")}};
    };

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    expect_classification(
        result.plan, 1, AurUpdateClassification::NonAurForeign,
        "Ordered missing package was not confirmed non-AUR");
    expect(
        g_fixture.info_strict_calls.empty(),
        "Ordered partial map unexpectedly used fallback");
}

void test_install_reason_is_forwarded_to_the_plan() {
    reset_fixture();
    g_fixture.inventory = {
        {"explicit-package", "1.0-1", InstalledPackageReason::Explicit},
        {"dependency-package", "1.0-1", InstalledPackageReason::Dependency},
        {"unknown-package", "1.0-1", InstalledPackageReason::Unknown}};

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect_plan_matches_inventory(result.plan, g_fixture.inventory);
    expect(
        result.plan.entries[0].install_reason ==
            InstalledPackageReason::Explicit,
        "Explicit install reason was not forwarded");
    expect(
        result.plan.entries[1].install_reason ==
            InstalledPackageReason::Dependency,
        "Dependency install reason was not forwarded");
    expect(
        result.plan.entries[2].install_reason ==
            InstalledPackageReason::Unknown,
        "Unknown install reason was not forwarded");
}

void test_query_dependency_surface_is_read_only() {
    reset_fixture();
    g_fixture.inventory = {{"read-only-package", "1.0-1"}};

    const AurUpdateQueryResult result = query_installed_aur_updates();

    expect(result.plan.entries.size() == 1, "Read-only fixture did not produce a plan");
    expect(g_fixture.inventory_calls == 1, "Read-only query skipped inventory");
    expect(g_fixture.info_many_calls.size() == 1, "Read-only query skipped AUR metadata");
    expect(g_fixture.exec_calls.size() == 1, "Read-only query did not invoke vercmp once");
    expect(
        event_index("configuration") < event_index("inventory-open"),
        "Inventory started before configuration was resolved");
    expect(
        event_index("inventory-release") <
            event_index("info-many:read-only-package"),
        "AUR RPC started before the inventory read phase was released");
    for(const auto& command : g_fixture.exec_calls) {
        expect(command.starts_with("vercmp "), "Query invoked a non-vercmp command");
        for(const char* forbidden : {
                "git clone", "git fetch", "makepkg", "sudo", "pacman -S",
                "pacman -U", "pacman -R"}) {
            expect(
                command.find(forbidden) == std::string::npos,
                "Query invoked a mutating command: " + command);
        }
    }
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        run_case("empty inventory skips AUR queries", test_empty_inventory_skips_aur_queries);
        run_case(
            "explicit inventory core skips inventory boundary and preserves order",
            test_explicit_inventory_core_skips_inventory_boundary_and_preserves_order);
        run_case(
            "configuration failure stops before inventory and AUR queries",
            test_configuration_failure_stops_before_inventory_and_aur_queries);
        run_case(
            "inventory failure propagates without AUR queries",
            test_inventory_failure_propagates_without_aur_queries);
        run_case(
            "1-100 packages use one installed-order batch",
            test_one_through_one_hundred_packages_use_one_batch);
        run_case(
            "101 packages use 100+1 batches",
            test_one_hundred_and_one_packages_use_two_batches);
        run_case(
            "empty batch fallback continues to later batch",
            test_empty_batch_uses_per_package_fallback_and_continues);
        run_case(
            "partial non-empty batch does not fallback",
            test_partial_non_empty_batch_does_not_fallback);
        run_case(
            "ordinary failure is recoverable and later batch succeeds",
            test_ordinary_batch_failure_is_recoverable_and_later_batch_succeeds);
        run_case(
            "ordinary fallback failure marks batch unavailable and continues",
            test_ordinary_fallback_failure_marks_batch_unavailable_and_continues);
        run_case(
            "batch schema failure propagates immediately",
            test_batch_schema_failure_propagates_immediately);
        run_case(
            "fallback schema failure propagates immediately",
            test_fallback_schema_failure_propagates_immediately);
        run_case(
            "version comparison classifies and fails closed",
            test_version_comparison_classifies_and_fails_closed);
        run_case(
            "installed and AUR identity are preserved",
            test_installed_and_aur_identity_are_preserved);
        run_case(
            "exact AUR suffix candidates are conservative",
            test_query_projects_exact_aur_suffix_candidates_conservatively);
        run_case(
            "plan preserves inventory order instead of map order",
            test_plan_preserves_inventory_order_instead_of_map_order);
        run_case(
            "install reason is forwarded to the plan",
            test_install_reason_is_forwarded_to_the_plan);
        run_case(
            "query dependency surface is read only",
            test_query_dependency_surface_is_read_only);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "AUR update query tests: all checks passed\n";
    return 0;
}
