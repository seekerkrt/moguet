#include "repository_query.hpp"

#include "stubs/package-metadata/alpm_stub.hpp"
#include "stubs/repository-query/process_stub.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace alpm_stub = package_metadata_test_stub;
namespace process_stub = repository_query_test_stub;

constexpr const char* DATABASE_PATH_COMMAND =
    "pacman-conf --verbose RootDir DBPath 2>/dev/null";
constexpr const char* REPOSITORY_LIST_COMMAND =
    "pacman-conf --repo-list 2>/dev/null";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Alternative, typename Result>
Alternative require_alternative(
    const Result& result, const std::string& context) {
    const Alternative* alternative = std::get_if<Alternative>(&result);
    if(alternative == nullptr) {
        throw std::runtime_error(
            context + ": unexpected result alternative");
    }
    return *alternative;
}

class TestDatabase final {
public:
    TestDatabase() {
        std::string template_text =
            (fs::temp_directory_path() /
             "moguet-repository-query-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create repository query test directory.");
        }
        root_ = created_path;
        database_path_ = root_ / "pacman";
        fs::create_directories(database_path_);
    }

    TestDatabase(const TestDatabase&) = delete;
    TestDatabase& operator=(const TestDatabase&) = delete;

    ~TestDatabase() noexcept {
        std::error_code error;
        fs::remove_all(root_, error);
    }

    const fs::path& database_path() const noexcept {
        return database_path_;
    }

private:
    fs::path root_;
    fs::path database_path_;
};

std::string repository_list_output(
    const std::vector<std::string>& repository_names) {
    std::string output;
    for(const auto& repository_name : repository_names) {
        output += repository_name + "\n";
    }
    return output;
}

void enqueue_database_paths(const TestDatabase& database) {
    process_stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{
            "RootDir = /\nDBPath = " +
                database.database_path().string() + "\n",
            0});
}

void enqueue_configuration(
    const TestDatabase& database,
    const std::vector<std::string>& repository_names =
        {"core", "extra"}) {
    enqueue_database_paths(database);
    process_stub::enqueue_captured_command_result(
        REPOSITORY_LIST_COMMAND,
        CapturedCommandResult{
            repository_list_output(repository_names), 0});
}

void set_repository_package(
    const std::string& repository_name,
    const std::string& package_name,
    const std::string& version) {
    alpm_stub::set_repository_package_metadata(
        repository_name, package_name, 10, 20);
    alpm_stub::set_repository_package_version(
        repository_name, package_name, version);
}

RepositoryMetadataFailure require_failure(
    const StrictRepositoryPackageQueryResult& result,
    RepositoryMetadataFailureKind expected_kind,
    const std::string& context) {
    const RepositoryMetadataFailure& failure =
        require_alternative<RepositoryMetadataFailure>(
            result, context);
    expect(failure.kind == expected_kind,
           context + ": failure kind differs");
    expect(!failure.diagnostic.empty(),
           context + ": empty failure diagnostic");
    return failure;
}

RepositoryProviderQuerySnapshot require_provider_snapshot(
    const StrictRepositoryProvidersQueryResult& result,
    const std::string& context) {
    return require_alternative<RepositoryProviderQuerySnapshot>(
        result, context);
}

const RepositoryProviderOrigin& require_repository_origin(
    const ProvidedDependency& provider,
    const std::string& context) {
    const auto* origin =
        std::get_if<RepositoryProviderOrigin>(&provider.origin);
    if(origin == nullptr) {
        throw std::runtime_error(
            context + ": provider origin is not a repository");
    }
    return *origin;
}

void test_candidate_value_contract() {
    const ProvidedDependency repository =
        ProvidedDependency::from_repository(
            "aur", "same-package", "virtual-api",
            "virtual-api=2", std::string("2.3-1"));
    const ProvidedDependency same_repository_identity =
        ProvidedDependency::from_repository(
            "aur", "same-package", "other-api",
            "other-api=1", std::string("2.4-1"));
    const ProvidedDependency aur = ProvidedDependency::from_aur(
        "same-package", "same-package", "virtual-api",
        "virtual-api=2", std::string("2.3-1"));

    expect(
        same_provider_identity(
            repository, same_repository_identity),
        "Repository provider identity depends on constraint metadata");
    expect(
        !same_provider_identity(repository, aur),
        "Repository named aur was conflated with AUR origin");
}

void test_configured_repository_order() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"core", "extra"});
    set_repository_package("core", "shared-package", "3.0-1");
    set_repository_package("extra", "shared-package", "2.0-1");

    const RepositoryPackagePresent& package =
        require_alternative<RepositoryPackagePresent>(
            query_repository_package_strict("shared-package"),
            "configured repository order");
    expect(
        package.repository_name == "core" &&
            package.configured_order == 0 &&
            package.configured_repository_order ==
                std::optional<std::vector<std::string>>{
                    {"core", "extra"}} &&
            package.package_name == "shared-package" &&
            package.package_base == "shared-package" &&
            package.package_version.has_value() &&
            package.package_version->version() != nullptr &&
            *package.package_version->version() == "3.0-1" &&
            package.architecture == std::optional<std::string>{"x86_64"},
        "Exact lookup lost configured repository precedence");

    const std::vector<std::string> commands_before_explicit_query =
        process_stub::captured_commands();
    const RepositoryPackagePresent& explicitly_configured =
        require_alternative<RepositoryPackagePresent>(
            query_repository_package_strict(
                PacmanRepositoryConfiguration{
                    PacmanDatabasePaths{
                        "/", database.database_path()},
                    {"core", "extra"}},
                "shared-package"),
            "explicitly configured repository query");
    expect(
        explicitly_configured.repository_name == "core" &&
            explicitly_configured.configured_order == 0 &&
            explicitly_configured.configured_repository_order ==
                std::optional<std::vector<std::string>>{
                    {"core", "extra"}},
        "Explicit repository configuration changed candidate authority");
    expect(
        process_stub::captured_commands() ==
            commands_before_explicit_query,
        "Explicit repository query re-resolved pacman configuration");
}

void test_split_child_preserves_package_base() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"core"});
    set_repository_package("core", "suite-child", "4.0-1");
    alpm_stub::set_repository_package_base(
        "core", "suite-child", "suite");

    const RepositoryPackagePresent& package =
        require_alternative<RepositoryPackagePresent>(
            query_repository_package_strict("suite-child"),
            "split child PackageBase");
    expect(
        package.repository_name == "core" &&
            package.package_name == "suite-child" &&
            package.package_base == "suite",
        "Strict repository query flattened split child and PackageBase");
}

void test_confirmed_not_found_is_distinct() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database);
    alpm_stub::set_repository_package_absent("core", "missing-package");
    alpm_stub::set_repository_package_absent("extra", "missing-package");

    const RepositoryPackageNotFound& missing =
        require_alternative<RepositoryPackageNotFound>(
            query_repository_package_strict("missing-package"),
            "confirmed repository absence");
    expect(
        missing.configured_repository_order ==
            std::optional<std::vector<std::string>>{
                {"core", "extra"}},
        "Confirmed repository absence lost configured order");
}

void test_malformed_package_base_is_failure() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"core"});
    set_repository_package("core", "malformed-base", "1.0-1");
    alpm_stub::set_repository_package_base(
        "core", "malformed-base", "invalid/base");

    RepositoryMetadataFailure failure = require_failure(
        query_repository_package_strict("malformed-base"),
        RepositoryMetadataFailureKind::SyncDatabaseMalformed,
        "malformed PackageBase");
    expect(
        failure.repository_name ==
            std::optional<std::string>{"core"},
        "Malformed PackageBase lost repository provenance");
}

void test_exact_returned_child_mismatch_is_failure() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"core"});
    set_repository_package("core", "requested-child", "1.0-1");
    alpm_stub::set_repository_package_returned_name(
        "core", "requested-child", "different-child");

    RepositoryMetadataFailure failure = require_failure(
        query_repository_package_strict("requested-child"),
        RepositoryMetadataFailureKind::SyncDatabaseMalformed,
        "exact returned child mismatch");
    expect(
        failure.repository_name ==
            std::optional<std::string>{"core"},
        "Exact child mismatch lost repository provenance");
}

void test_present_before_later_source_failure() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database);
    set_repository_package("core", "present-package", "1.0-1");
    alpm_stub::set_sync_database_register_failure("extra");

    const RepositoryPackagePresent& package =
        require_alternative<RepositoryPackagePresent>(
            query_repository_package_strict("present-package"),
            "present before later source failure");
    expect(
        package.repository_name == "core" &&
            package.configured_order == 0,
        "Later source failure erased an earlier exact observation");
}

void test_absent_before_later_source_failure() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database);
    alpm_stub::set_repository_package_absent(
        "core", "missing-package");
    alpm_stub::set_sync_database_register_failure("extra");

    RepositoryMetadataFailure failure = require_failure(
        query_repository_package_strict("missing-package"),
        RepositoryMetadataFailureKind::SyncDatabaseUnavailable,
        "absence before later source failure");
    expect(
        failure.repository_name ==
                std::optional<std::string>{"extra"} &&
            failure.configured_repository_order ==
                std::optional<std::vector<std::string>>{
                    {"core", "extra"}},
        "Partial exact failure lost source identity");
}

void test_unrelated_malformed_record_does_not_break_exact_lookup() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"core"});
    set_repository_package("core", "valid-package", "5.0-1");
    set_repository_package("core", "unrelated-package", "1.0-1");
    alpm_stub::set_repository_package_returned_name(
        "core", "unrelated-package", "invalid/name");

    const RepositoryPackagePresent& package =
        require_alternative<RepositoryPackagePresent>(
            query_repository_package_strict("valid-package"),
            "exact lookup with unrelated malformed record");
    expect(
        package.package_name == "valid-package" &&
            package.repository_name == "core",
        "Unrelated malformed metadata destroyed an exact candidate");
}

void test_repository_provider_capabilities() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"core"});
    set_repository_package("core", "provider-a", "9.0-1");
    alpm_stub::set_repository_package_provides(
        "core", "provider-a",
        {
            alpm_stub::RepositoryProvidedPackageMetadata{
                std::string("virtual-api"),
                std::string("2.1-3"),
                ALPM_DEP_MOD_EQ},
        });
    set_repository_package("core", "provider-b", "8.0-1");
    alpm_stub::set_repository_package_provides(
        "core", "provider-b",
        {
            alpm_stub::RepositoryProvidedPackageMetadata{
                std::string("virtual-api"),
                std::nullopt,
                ALPM_DEP_MOD_ANY},
        });

    const RepositoryProviderQuerySnapshot& snapshot =
        require_provider_snapshot(
            query_repository_providers_strict("virtual-api"),
            "repository provider capabilities");
    expect(snapshot.source_failures.empty(),
           "Complete provider snapshot has source failures");
    expect(
        snapshot.configured_repository_order ==
            std::optional<std::vector<std::string>>{{"core"}},
        "Provider snapshot lost configured repository authority");
    expect(snapshot.candidates.size() == 2,
           "Provider enumeration candidate count differs");

    const ProvidedDependency& equality = snapshot.candidates[0];
    expect(
        require_repository_origin(equality, "equality provider")
                    .repository_name == "core" &&
            require_repository_origin(equality, "equality provider")
                    .configured_order ==
                std::optional<std::size_t>{0} &&
            equality.package_name == "provider-a" &&
            equality.package_base == "provider-a" &&
            equality.package_architecture ==
                std::optional<std::string>{"x86_64"} &&
            equality.provided_dependency_specification ==
                "virtual-api=2.1-3" &&
            equality.package_version ==
                std::optional<std::string>{"9.0-1"} &&
            equality.constraint_metadata.has_value() &&
            equality.constraint_metadata->provided_version.version() !=
                nullptr &&
            *equality.constraint_metadata->provided_version.version() ==
                "2.1-3",
        "Equality provider capability lost typed metadata");
    expect(
        *equality.constraint_metadata->provided_version.version() !=
            equality.package_version.value(),
        "Provider package version replaced the capability version");
    ProvidedDependency different_architecture = equality;
    different_architecture.package_architecture = "aarch64";
    expect(
        different_architecture != equality,
        "Provider full equality ignored exact architecture");

    const ProvidedDependency& unversioned = snapshot.candidates[1];
    expect(
        unversioned.package_name == "provider-b" &&
            unversioned.package_base == "provider-b" &&
            unversioned.constraint_metadata.has_value() &&
            unversioned.constraint_metadata->provided_version
                    .unknown_reason() != nullptr &&
            *unversioned.constraint_metadata->provided_version
                    .unknown_reason() ==
                ObservedVersionUnknownReason::
                    UnversionedProviderCapability,
        "Unversioned provider capability was not retained as Unknown");

    set_repository_package("core", "rust", "1.90.0-1");
    alpm_stub::set_repository_package_base("core", "rust", "rust");
    alpm_stub::set_repository_package_architecture(
        "core", "rust", "any");
    alpm_stub::set_repository_package_provides(
        "core", "rust",
        {alpm_stub::RepositoryProvidedPackageMetadata{
            std::string("cargo"), std::string("1.90.0"),
            ALPM_DEP_MOD_EQ}});
    enqueue_configuration(database, {"core"});
    const RepositoryProviderQuerySnapshot& cargo_snapshot =
        require_provider_snapshot(
            query_repository_providers_strict("cargo"),
            "cargo provider PackageBase authority");
    expect(
        cargo_snapshot.candidates.size() == 1 &&
            cargo_snapshot.candidates.front().package_name == "rust" &&
            cargo_snapshot.candidates.front().package_base == "rust" &&
            cargo_snapshot.candidates.front().package_architecture ==
                std::optional<std::string>{"any"} &&
            cargo_snapshot.candidates.front().provided_dependency_name ==
                "cargo",
        "cargo requirement, rust provider package, and rust PackageBase were flattened");

    alpm_stub::reset_alpm_stub();
    set_repository_package("core", "missing-arch-provider", "1.0-1");
    alpm_stub::set_repository_package_provides(
        "core", "missing-arch-provider",
        {alpm_stub::RepositoryProvidedPackageMetadata{
            std::string("virtual-api"), std::nullopt,
            ALPM_DEP_MOD_ANY}});
    alpm_stub::set_repository_package_architecture_null(
        "core", "missing-arch-provider");
    enqueue_configuration(database, {"core"});
    const RepositoryProviderQuerySnapshot& missing_architecture =
        require_provider_snapshot(
            query_repository_providers_strict("virtual-api"),
            "missing provider architecture");
    expect(
        missing_architecture.candidates.empty() &&
            missing_architecture.source_failures.size() == 1,
        "Missing provider architecture became a complete candidate");
}

void test_provider_partial_source_failure() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database);
    set_repository_package("core", "provider-a", "1.0-1");
    alpm_stub::set_repository_package_provides(
        "core", "provider-a",
        {
            alpm_stub::RepositoryProvidedPackageMetadata{
                std::string("virtual-api"),
                std::string("1"),
                ALPM_DEP_MOD_EQ},
        });
    alpm_stub::set_sync_database_cache_failure("extra");

    const RepositoryProviderQuerySnapshot& snapshot =
        require_provider_snapshot(
            query_repository_providers_strict("virtual-api"),
            "partial provider source failure");
    expect(
        snapshot.candidates.size() == 1 &&
            snapshot.candidates.front().package_name ==
                "provider-a",
        "Partial provider failure discarded a valid observation");
    expect(
        snapshot.source_failures.size() == 1 &&
            snapshot.source_failures.front().repository_name ==
                std::optional<std::string>{"extra"} &&
            snapshot.source_failures.front().kind ==
                RepositoryMetadataFailureKind::
                    SyncDatabaseUnavailable,
        "Partial provider failure lost source-aware state");
}

void test_repository_named_aur_keeps_repository_origin() {
    alpm_stub::reset_alpm_stub();
    TestDatabase database;
    enqueue_configuration(database, {"aur"});
    set_repository_package("aur", "provider", "1.0-1");
    alpm_stub::set_repository_package_provides(
        "aur", "provider",
        {
            alpm_stub::RepositoryProvidedPackageMetadata{
                std::string("virtual-api"),
                std::nullopt,
                ALPM_DEP_MOD_ANY},
        });

    const RepositoryProviderQuerySnapshot& snapshot =
        require_provider_snapshot(
            query_repository_providers_strict("virtual-api"),
            "repository named aur");
    expect(
        snapshot.candidates.size() == 1 &&
            require_repository_origin(
                snapshot.candidates.front(),
                "repository named aur provider")
                    .repository_name == "aur" &&
            !std::holds_alternative<AurProviderOrigin>(
                snapshot.candidates.front().origin),
        "Repository named aur was classified as AUR");
}

void test_configuration_failure() {
    alpm_stub::reset_alpm_stub();
    process_stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{"RootDir = /\n", 0});

    RepositoryMetadataFailure failure = require_failure(
        query_repository_package_strict("package-name"),
        RepositoryMetadataFailureKind::ConfigurationMalformed,
        "configuration failure");
    expect(!failure.repository_name.has_value(),
           "Global configuration failure gained source identity");
}

void test_installed_exact_states() {
    {
        alpm_stub::reset_alpm_stub();
        TestDatabase database;
        enqueue_database_paths(database);
        alpm_stub::set_package_metadata(
            "foreign-package", "4.2-1", ALPM_PKG_REASON_EXPLICIT);

        const InstalledExactPackage& installed =
            require_alternative<InstalledExactPackage>(
                query_installed_exact_package_strict(
                    "foreign-package"),
                "installed exact present");
        expect(
            installed.package_name == "foreign-package" &&
                installed.observed_version.source() ==
                    ObservedVersionSource::
                        InstalledExactPackage &&
                installed.observed_version.version() != nullptr &&
                *installed.observed_version.version() == "4.2-1",
            "Installed exact observation lost source identity");
    }

    {
        alpm_stub::reset_alpm_stub();
        TestDatabase database;
        enqueue_database_paths(database);
        alpm_stub::set_package_absent();
        const InstalledExactPackageAbsent& absent =
            require_alternative<InstalledExactPackageAbsent>(
                query_installed_exact_package_strict(
                    "missing-package"),
                "installed exact absent");
        expect(absent.package_name == "missing-package",
               "Installed absence lost queried identity");
    }

    {
        alpm_stub::reset_alpm_stub();
        TestDatabase database;
        enqueue_database_paths(database);
        alpm_stub::set_package_query_failure(ALPM_ERR_DB_OPEN);
        const InstalledExactPackageQueryFailure& failure =
            require_alternative<InstalledExactPackageQueryFailure>(
                query_installed_exact_package_strict(
                    "failed-package"),
                "installed exact query failure");
        expect(
            failure.package_name == "failed-package" &&
                failure.failure.code ==
                    PackageMetadataErrorCode::QueryFailed,
            "Installed query failure was flattened into absence");
    }
}

void run_test_case(const std::string& test_case) {
    if(test_case == "candidate-value-contract") {
        test_candidate_value_contract();
    } else if(test_case == "configured-order") {
        test_configured_repository_order();
    } else if(test_case == "split-package-base") {
        test_split_child_preserves_package_base();
    } else if(test_case == "confirmed-not-found") {
        test_confirmed_not_found_is_distinct();
    } else if(test_case == "malformed-package-base") {
        test_malformed_package_base_is_failure();
    } else if(test_case == "returned-child-mismatch") {
        test_exact_returned_child_mismatch_is_failure();
    } else if(test_case == "present-later-failure") {
        test_present_before_later_source_failure();
    } else if(test_case == "absent-later-failure") {
        test_absent_before_later_source_failure();
    } else if(test_case == "unrelated-malformed-exact") {
        test_unrelated_malformed_record_does_not_break_exact_lookup();
    } else if(test_case == "provider-capabilities") {
        test_repository_provider_capabilities();
    } else if(test_case == "provider-partial-failure") {
        test_provider_partial_source_failure();
    } else if(test_case == "repository-named-aur") {
        test_repository_named_aur_keeps_repository_origin();
    } else if(test_case == "configuration-failure") {
        test_configuration_failure();
    } else if(test_case == "installed-exact-states") {
        test_installed_exact_states();
    } else {
        throw std::runtime_error(
            "Unknown repository query test case: " + test_case);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if(argc != 2) {
            throw std::runtime_error(
                "repository query test requires exactly one case name");
        }
        run_test_case(argv[1]);
        std::cout << "repository query test passed: " << argv[1] << "\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "repository query test failed: " << error.what()
                  << "\n";
        return 1;
    }
}
