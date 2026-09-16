#include "package_metadata.hpp"

#ifdef ALPM_H
#error "package_metadata.hpp must not expose or include raw libalpm types"
#endif

#include "stubs/package-metadata/alpm_stub.hpp"
#include "stubs/package-metadata/process_stub.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

static_assert(!std::is_default_constructible_v<PackageMetadataSession>);
static_assert(!std::is_copy_constructible_v<PackageMetadataSession>);
static_assert(!std::is_copy_assignable_v<PackageMetadataSession>);
static_assert(std::is_nothrow_move_constructible_v<PackageMetadataSession>);
static_assert(std::is_nothrow_move_assignable_v<PackageMetadataSession>);
static_assert(std::is_nothrow_destructible_v<PackageMetadataSession>);
static_assert(!std::is_default_constructible_v<RepositoryPackageMetadataSession>);
static_assert(!std::is_copy_constructible_v<RepositoryPackageMetadataSession>);
static_assert(!std::is_copy_assignable_v<RepositoryPackageMetadataSession>);
static_assert(std::is_nothrow_move_constructible_v<RepositoryPackageMetadataSession>);
static_assert(std::is_nothrow_move_assignable_v<RepositoryPackageMetadataSession>);
static_assert(std::is_nothrow_destructible_v<RepositoryPackageMetadataSession>);
static_assert(!std::is_pointer_v<
              decltype(RepositoryExactPackageMetadata::package_base)>);

namespace {

namespace fs = std::filesystem;
namespace stub = package_metadata_test_stub;

constexpr const char* DATABASE_PATH_COMMAND =
    "pacman-conf --verbose RootDir DBPath 2>/dev/null";
constexpr const char* REPOSITORY_LIST_COMMAND =
    "pacman-conf --repo-list 2>/dev/null";

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Identity>
void expect_installed_identity_value(
    const Identity& identity,
    InstalledPackageMetadataValueState expected_state,
    const std::optional<std::string>& expected_value,
    const std::string& context) {
    expect(identity.state() == expected_state,
           context + ": unexpected metadata state");
    const std::string* value = identity.value();
    expect(
        (value == nullptr) == !expected_value.has_value(),
        context + ": metadata value presence differs");
    if(value != nullptr) {
        expect(*value == expected_value.value(),
               context + ": metadata value differs");
    }
}

PacmanDatabasePaths valid_database_paths() {
    return PacmanDatabasePaths{"/", "/var/lib/pacman"};
}

template <typename Callable>
void expect_metadata_error(
    Callable callable, PackageMetadataErrorCode expected_code,
    const std::string& context, const std::string& forbidden_diagnostic_text = "") {
    try {
        callable();
    } catch(const PackageMetadataError& error) {
        const PackageMetadataFailure& failure = error.failure();
        expect(failure.code == expected_code, context + ": unexpected error code");
        expect(!failure.diagnostic.empty(), context + ": empty diagnostic");
        expect(
            std::string(error.what()) == failure.diagnostic,
            context + ": exception and failure diagnostics differ");
        if(!forbidden_diagnostic_text.empty()) {
            expect(
                failure.diagnostic.find(forbidden_diagnostic_text) == std::string::npos,
                context + ": diagnostic contains raw command output");
        }
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            context + ": unexpected exception category: " + error.what());
    }
    throw std::runtime_error(context + ": expected PackageMetadataError");
}

template <typename Expected, typename Result>
Expected require_result_alternative(
    const Result& result, const std::string& context) {
    const Expected* expected = std::get_if<Expected>(&result);
    if(expected == nullptr) {
        throw std::runtime_error(context + ": unexpected query result alternative");
    }
    return *expected;
}

PackageMetadataFailure require_query_failure(
    const InstalledPackageQueryResult& result, PackageMetadataErrorCode expected_code,
    const std::string& context) {
    PackageMetadataFailure failure =
        require_result_alternative<PackageMetadataFailure>(result, context);
    expect(failure.code == expected_code, context + ": unexpected failure code");
    expect(!failure.diagnostic.empty(), context + ": empty failure diagnostic");
    return failure;
}

PackageMetadataFailure require_repository_query_failure(
    const RepositoryPackageQueryResult& result,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    PackageMetadataFailure failure =
        require_result_alternative<PackageMetadataFailure>(result, context);
    expect(failure.code == expected_code, context + ": unexpected failure code");
    expect(!failure.diagnostic.empty(), context + ": empty failure diagnostic");
    return failure;
}

PackageMetadataFailure require_repository_search_failure(
    const RepositoryPackageSearchResult& result,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    PackageMetadataFailure failure =
        require_result_alternative<PackageMetadataFailure>(result, context);
    expect(failure.code == expected_code, context + ": unexpected failure code");
    expect(!failure.diagnostic.empty(), context + ": empty failure diagnostic");
    return failure;
}

RepositoryExactPackageMetadataSnapshot require_exact_repository_snapshot(
    const RepositoryExactPackageMetadataQueryResult& result,
    const std::string& context) {
    return require_result_alternative<
        RepositoryExactPackageMetadataSnapshot>(result, context);
}

RepositoryExactPackageMetadata require_exact_repository_metadata(
    const RepositoryExactPackageMetadataQueryResult& result,
    const std::string& context) {
    RepositoryExactPackageMetadataSnapshot snapshot =
        require_exact_repository_snapshot(result, context);
    expect(snapshot.source_results.size() == 1,
           context + ": source result count differs");
    return require_result_alternative<RepositoryExactPackageMetadata>(
        snapshot.source_results.front(), context);
}

PackageMetadataFailure require_exact_repository_source_failure(
    const RepositoryExactPackageMetadataQueryResult& result,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    RepositoryExactPackageMetadataSnapshot snapshot =
        require_exact_repository_snapshot(result, context);
    expect(snapshot.source_results.size() == 1,
           context + ": source result count differs");
    RepositoryExactPackageMetadataSourceFailure source_failure =
        require_result_alternative<
            RepositoryExactPackageMetadataSourceFailure>(
            snapshot.source_results.front(), context);
    expect(source_failure.failure.code == expected_code,
           context + ": unexpected failure code");
    return source_failure.failure;
}

PackageMetadataFailure require_inventory_failure(
    const ForeignPackageInventoryResult& result,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    PackageMetadataFailure failure =
        require_result_alternative<PackageMetadataFailure>(result, context);
    expect(failure.code == expected_code, context + ": unexpected failure code");
    expect(!failure.diagnostic.empty(), context + ": empty failure diagnostic");
    return failure;
}

PackageMetadataFailure require_snapshot_failure(
    const LocalPackageVersionSnapshotResult& result,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    PackageMetadataFailure failure =
        require_result_alternative<PackageMetadataFailure>(result, context);
    expect(failure.code == expected_code, context + ": unexpected failure code");
    expect(!failure.diagnostic.empty(), context + ": empty failure diagnostic");
    return failure;
}

PackageMetadataFailure require_state_snapshot_failure(
    const InstalledPackageStateSnapshotResult& result,
    PackageMetadataErrorCode expected_code,
    const std::string& context) {
    PackageMetadataFailure failure =
        require_result_alternative<PackageMetadataFailure>(
            result, context);
    expect(failure.code == expected_code,
           context + ": unexpected failure code");
    expect(!failure.diagnostic.empty(),
           context + ": empty failure diagnostic");
    return failure;
}

void set_pacman_conf_output(const std::string& output, int exit_code = 0) {
    stub::reset_process_stub();
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{output, exit_code});
}

void set_repository_configuration_outputs(
    const std::string& repository_output,
    int repository_exit_code = 0,
    const std::string& path_output =
        "RootDir = /\nDBPath = /var/lib/pacman/\n",
    int path_exit_code = 0) {
    stub::reset_process_stub();
    stub::enqueue_captured_command_result(
        DATABASE_PATH_COMMAND,
        CapturedCommandResult{path_output, path_exit_code});
    stub::enqueue_captured_command_result(
        REPOSITORY_LIST_COMMAND,
        CapturedCommandResult{repository_output, repository_exit_code});
}

std::string repository_usage_command(const std::string& quoted_repository_name) {
    return "pacman-conf --repo " + quoted_repository_name +
           " Usage 2>/dev/null";
}

void enqueue_repository_usage_output(
    const std::string& quoted_repository_name,
    const std::string& output,
    int exit_code = 0) {
    stub::enqueue_captured_command_result(
        repository_usage_command(quoted_repository_name),
        CapturedCommandResult{output, exit_code});
}

PacmanRepositoryConfiguration valid_repository_configuration(
    std::vector<std::string> repository_names = {"core", "extra"}) {
    return PacmanRepositoryConfiguration{
        valid_database_paths(),
        std::move(repository_names)};
}

void expect_repository_query_history(
    const std::vector<std::pair<std::string, std::string>>& expected_queries,
    const std::string& context) {
    std::vector<stub::RepositoryPackageQuery> actual_queries =
        stub::repository_package_query_history();
    expect(actual_queries.size() == expected_queries.size(), context + ": query count differs");
    for(std::size_t index = 0; index < expected_queries.size(); ++index) {
        expect(
            actual_queries[index].repository_name == expected_queries[index].first &&
                actual_queries[index].package_name == expected_queries[index].second,
            context + ": query identity differs at index " + std::to_string(index));
    }
}

void test_pacman_conf_path_parse_success() {
    set_pacman_conf_output("RootDir = /\nDBPath = /var/lib/pacman/\n");

    PacmanDatabasePaths paths = resolve_pacman_database_paths();

    expect(paths.root_dir == fs::path("/"), "RootDir parse result differs");
    expect(paths.db_path == fs::path("/var/lib/pacman"), "DBPath parse result differs");
    expect(stub::capture_command_call_count() == 1, "pacman-conf was not called exactly once");
    expect(
        stub::last_captured_command() ==
            "pacman-conf --verbose RootDir DBPath 2>/dev/null",
        "Unexpected pacman-conf command");
}

void test_pacman_conf_command_failure() {
    set_pacman_conf_output("RootDir = /\nDBPath = /var/lib/pacman/", 127);
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationUnavailable,
        "pacman-conf command failure");
}

void test_root_dir_missing() {
    set_pacman_conf_output("DBPath = /var/lib/pacman/");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "missing RootDir");
}

void test_database_path_missing() {
    set_pacman_conf_output("RootDir = /");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "missing DBPath");
}

void test_duplicate_root_dir() {
    set_pacman_conf_output("RootDir = /\nRootDir = /srv/root\nDBPath = /var/lib/pacman");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "duplicate RootDir");
}

void test_duplicate_database_path() {
    set_pacman_conf_output(
        "RootDir = /\nDBPath = /var/lib/pacman\nDBPath = /srv/pacman");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "duplicate DBPath");
}

void test_empty_root_dir() {
    set_pacman_conf_output("RootDir =    \nDBPath = /var/lib/pacman");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "empty RootDir");
}

void test_empty_database_path() {
    set_pacman_conf_output("RootDir = /\nDBPath =    ");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "empty DBPath");
}

void test_relative_root_dir() {
    set_pacman_conf_output("RootDir = relative/root\nDBPath = /var/lib/pacman");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "relative RootDir");
}

void test_relative_database_path() {
    set_pacman_conf_output("RootDir = /\nDBPath = relative/database");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "relative DBPath");
}

void test_trailing_slash_normalization() {
    set_pacman_conf_output("RootDir = /srv/root///\nDBPath = /var/lib/pacman///");

    PacmanDatabasePaths paths = resolve_pacman_database_paths();

    expect(paths.root_dir == fs::path("/srv/root"), "RootDir trailing slash remains");
    expect(paths.db_path == fs::path("/var/lib/pacman"), "DBPath trailing slash remains");
}

void test_malformed_pacman_conf_line() {
    set_pacman_conf_output("RootDir = /\nmalformed line\nDBPath = /var/lib/pacman");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "malformed pacman-conf line");
}

void test_leading_blank_pacman_conf_line() {
    set_pacman_conf_output("\nRootDir = /\nDBPath = /var/lib/pacman\n");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "leading blank pacman-conf line");
}

void test_trailing_blank_pacman_conf_line() {
    set_pacman_conf_output("RootDir = /\nDBPath = /var/lib/pacman\n\n");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "trailing blank pacman-conf line");
}

void test_unexpected_pacman_conf_key_does_not_leak_output() {
    const std::string sensitive_marker = "sensitive-marker";
    set_pacman_conf_output(
        "RootDir = /\nUnexpected = " + sensitive_marker +
        "\nDBPath = /var/lib/pacman");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_database_paths()); },
        PackageMetadataErrorCode::ConfigurationMalformed, "unexpected pacman-conf key",
        sensitive_marker);
}

void test_repository_configuration_preserves_configured_order() {
    set_repository_configuration_outputs("testing\ncore\nextra\n");

    PacmanRepositoryConfiguration configuration =
        resolve_pacman_repository_configuration();

    expect(
        configuration.database_paths.root_dir == fs::path("/") &&
            configuration.database_paths.db_path == fs::path("/var/lib/pacman"),
        "repository configuration paths differ");
    expect(
        configuration.repository_names ==
            std::vector<std::string>({"testing", "core", "extra"}),
        "configured repository order changed");
    expect(
        stub::captured_commands() ==
            std::vector<std::string>({DATABASE_PATH_COMMAND, REPOSITORY_LIST_COMMAND}),
        "repository resolver command order differs");
}

void test_empty_repository_configuration_is_valid() {
    set_repository_configuration_outputs("");

    PacmanRepositoryConfiguration configuration =
        resolve_pacman_repository_configuration();

    expect(configuration.repository_names.empty(), "empty repository list was not preserved");
}

void test_repository_configuration_rejects_duplicate_and_blank_lines() {
    set_repository_configuration_outputs("core\ncore\n");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "duplicate repository");

    set_repository_configuration_outputs("core\n\nextra\n");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "blank repository line");
}

void test_repository_configuration_rejects_control_characters() {
    set_repository_configuration_outputs("core\r\nextra\n");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "repository carriage return");

    std::string nul_output = "core\n";
    nul_output.append("sec\0ret", 7);
    nul_output.push_back('\n');
    set_repository_configuration_outputs(nul_output);
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "repository null byte");

    set_repository_configuration_outputs("core\nex\ttra\n");
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "repository tab");

    std::string delete_output = "core\nextra";
    delete_output.push_back(static_cast<char>(0x7f));
    delete_output.push_back('\n');
    set_repository_configuration_outputs(delete_output);
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "repository delete control");
}

void test_repository_configuration_command_failures_are_distinct() {
    set_repository_configuration_outputs(
        "core\n", 0,
        "raw-path-marker", 41);
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationUnavailable,
        "repository configuration path command failure",
        "raw-path-marker");
    expect(
        stub::captured_commands() == std::vector<std::string>({DATABASE_PATH_COMMAND}),
        "repo-list ran after path command failure");

    set_repository_configuration_outputs("raw-repository-marker", 42);
    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationUnavailable,
        "repository list command failure",
        "raw-repository-marker");
    expect(
        stub::captured_commands() ==
            std::vector<std::string>({DATABASE_PATH_COMMAND, REPOSITORY_LIST_COMMAND}),
        "repository list command failure history differs");
}

void test_malformed_repository_output_does_not_leak_raw_text() {
    const std::string sensitive_marker = "repository-sensitive-marker";
    set_repository_configuration_outputs("core\n" + sensitive_marker + "\tvalue\n");

    expect_metadata_error(
        []() { static_cast<void>(resolve_pacman_repository_configuration()); },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "malformed repository output diagnostic",
        sensitive_marker);
}

void test_root_search_repository_configuration_filters_usage_in_order() {
    set_repository_configuration_outputs("testing\ncore\nextra\narchive\n");
    enqueue_repository_usage_output("'testing'", "All\n");
    enqueue_repository_usage_output("'core'", "Search\nInstall\n");
    enqueue_repository_usage_output("'extra'", "Search\n");
    enqueue_repository_usage_output(
        "'archive'", "Sync\nInstall\nUpgrade\n");

    PacmanRepositoryConfiguration configuration =
        resolve_pacman_root_search_repository_configuration();

    expect(
        configuration.repository_names ==
            std::vector<std::string>({"testing", "core"}),
        "root search repository eligibility or order differs");
    expect(
        stub::captured_commands() == std::vector<std::string>({DATABASE_PATH_COMMAND,
                                                               REPOSITORY_LIST_COMMAND,
                                                               repository_usage_command("'testing'"),
                                                               repository_usage_command("'core'"),
                                                               repository_usage_command("'extra'"),
                                                               repository_usage_command("'archive'")}),
        "root search Usage query order differs");
}

void test_root_search_repository_usage_is_strict() {
    const std::vector<std::string> malformed_outputs = {
        "",
        "Search\nSearch\nInstall\n",
        "All\nSearch\n",
        "Search Install\n",
        "Search\nUnknown\nInstall\n",
        "Search\n\nInstall\n"};

    for(const auto& output : malformed_outputs) {
        set_repository_configuration_outputs("core\n");
        enqueue_repository_usage_output("'core'", output);
        expect_metadata_error(
            []() {
                static_cast<void>(
                    resolve_pacman_root_search_repository_configuration());
            },
            PackageMetadataErrorCode::ConfigurationMalformed,
            "strict repository Usage parse");
    }
}

void test_root_search_repository_usage_command_is_quoted() {
    set_repository_configuration_outputs("odd'repository\n");
    enqueue_repository_usage_output("'odd'\\''repository'", "All\n");

    PacmanRepositoryConfiguration configuration =
        resolve_pacman_root_search_repository_configuration();

    expect(
        configuration.repository_names ==
            std::vector<std::string>({"odd'repository"}),
        "quoted Usage repository was not retained");
    expect(
        stub::last_captured_command() ==
            repository_usage_command("'odd'\\''repository'"),
        "repository Usage command was not shell-quoted");
}

void test_root_search_usage_failure_precedes_alpm_open() {
    set_repository_configuration_outputs("core\nextra\n");
    enqueue_repository_usage_output("'core'", "All\n");
    enqueue_repository_usage_output("'extra'", "raw-sensitive-output", 23);
    stub::reset_alpm_stub();

    RepositoryPackageSearchResult result =
        query_repository_root_package_search("query");

    static_cast<void>(require_repository_search_failure(
        result,
        PackageMetadataErrorCode::ConfigurationUnavailable,
        "Usage failure before alpm open"));
    expect(
        stub::initialize_call_count() == 0,
        "alpm opened before every repository Usage was acquired");
    expect(
        stub::capture_command_call_count() == 4,
        "Usage resolver continued after command failure");
}

void test_session_open_success() {
    stub::reset_alpm_stub();
    {
        PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(session);
        expect(stub::initialize_call_count() == 1, "alpm_initialize call count differs");
        expect(stub::local_database_call_count() == 1, "alpm_get_localdb call count differs");
        expect(stub::database_valid_call_count() == 1, "alpm_db_get_valid call count differs");
        expect(stub::package_cache_call_count() == 1, "package cache was not loaded");
        expect(stub::last_initialize_root() == "/", "alpm RootDir differs");
        expect(
            stub::last_initialize_database_path() == "/var/lib/pacman",
            "alpm DBPath differs");
        expect(stub::release_call_count() == 0, "session released handle before destruction");
    }
    expect(stub::release_call_count() == 1, "session did not release handle exactly once");
}

void test_session_rejects_relative_paths_before_initialize() {
    stub::reset_alpm_stub();
    expect_metadata_error(
        []() {
            static_cast<void>(PackageMetadataSession::open(
                PacmanDatabasePaths{"relative/root", "/var/lib/pacman"}));
        },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "session relative RootDir validation");
    expect(stub::initialize_call_count() == 0, "relative RootDir reached libalpm");

    expect_metadata_error(
        []() {
            static_cast<void>(PackageMetadataSession::open(
                PacmanDatabasePaths{"/", "relative/database"}));
        },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "session relative DBPath validation");
    expect(stub::initialize_call_count() == 0, "relative DBPath reached libalpm");
}

void test_session_rejects_embedded_null_path_before_initialize() {
    stub::reset_alpm_stub();
    const std::string root_with_null("/srv/root\0suffix", 16);

    expect_metadata_error(
        [&root_with_null]() {
            static_cast<void>(PackageMetadataSession::open(
                PacmanDatabasePaths{fs::path(root_with_null), "/var/lib/pacman"}));
        },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "session embedded null path validation");
    expect(stub::initialize_call_count() == 0, "embedded null path reached libalpm");
}

void test_alpm_initialization_failure() {
    stub::reset_alpm_stub();
    stub::set_initialize_failure(ALPM_ERR_SYSTEM);

    expect_metadata_error(
        []() { static_cast<void>(PackageMetadataSession::open(valid_database_paths())); },
        PackageMetadataErrorCode::InitializationFailed, "alpm initialization failure");
    expect(stub::created_handle_count() == 0, "failed initialization published a handle");
    expect(stub::release_call_count() == 0, "failed initialization released an unowned handle");
}

void test_local_database_unavailable_releases_handle() {
    stub::reset_alpm_stub();
    stub::set_local_database_unavailable();

    expect_metadata_error(
        []() { static_cast<void>(PackageMetadataSession::open(valid_database_paths())); },
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "local database unavailable");
    expect(stub::created_handle_count() == 1, "local DB failure did not create a handle");
    expect(stub::release_count_for_handle(0) == 1, "local DB failure leaked its handle");
}

void test_invalid_local_database_releases_handle() {
    stub::reset_alpm_stub();
    stub::set_local_database_invalid();

    expect_metadata_error(
        []() { static_cast<void>(PackageMetadataSession::open(valid_database_paths())); },
        PackageMetadataErrorCode::LocalDatabaseUnavailable, "invalid local database");
    expect(stub::release_count_for_handle(0) == 1, "invalid local DB leaked its handle");
}

void test_package_cache_failure_releases_handle() {
    stub::reset_alpm_stub();
    stub::set_package_cache_failure();

    expect_metadata_error(
        []() { static_cast<void>(PackageMetadataSession::open(valid_database_paths())); },
        PackageMetadataErrorCode::LocalDatabaseUnavailable, "package cache failure");
    expect(stub::release_count_for_handle(0) == 1, "package cache failure leaked its handle");
}

void test_empty_package_cache_is_queryable_state() {
    stub::reset_alpm_stub();
    stub::set_empty_package_cache();
    stub::set_package_absent();
    {
        PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());
        InstalledPackageQueryResult result =
            session.query_installed_package("test-package");
        static_cast<void>(require_result_alternative<PackageNotFound>(
            result, "empty package cache query"));
    }
    expect(stub::release_count_for_handle(0) == 1, "empty package cache leaked its handle");
}

void test_local_package_version_snapshot_is_owned_and_order_independent() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"zeta-package", "3.0-1", ALPM_PKG_REASON_DEPEND},
                              {"alpha-package", "1.0-2", ALPM_PKG_REASON_EXPLICIT}});

    LocalPackageVersionSnapshot snapshot;
    {
        PackageMetadataSession session =
            PackageMetadataSession::open(valid_database_paths());
        LocalPackageVersionSnapshotResult result =
            session.snapshot_local_package_versions();
        snapshot = require_result_alternative<LocalPackageVersionSnapshot>(
            result, "local package version snapshot");

        expect(
            stub::package_cache_call_count() == 1,
            "snapshot reloaded the preloaded local package cache");
    }

    stub::reset_alpm_stub();
    expect(
        snapshot == LocalPackageVersionSnapshot({{"alpha-package", "1.0-2"},
                                                 {"zeta-package", "3.0-1"}}),
        "snapshot did not retain owned name/version values");
}

void test_empty_local_package_version_snapshot_is_success() {
    stub::reset_alpm_stub();
    stub::set_empty_package_cache();
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    LocalPackageVersionSnapshotResult result =
        session.snapshot_local_package_versions();
    LocalPackageVersionSnapshot snapshot =
        require_result_alternative<LocalPackageVersionSnapshot>(
            result, "empty local package version snapshot");

    expect(snapshot.empty(), "empty local database produced snapshot entries");
    expect(
        stub::package_cache_call_count() == 1,
        "empty snapshot reloaded the preloaded local package cache");
}

void test_local_package_version_snapshot_rejects_invalid_names() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"valid-package", "1.0-1"}});
    stub::set_local_package_name_null(0);
    {
        PackageMetadataSession null_name_session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_snapshot_failure(
            null_name_session.snapshot_local_package_versions(),
            PackageMetadataErrorCode::MalformedMetadata,
            "snapshot null package name"));
    }

    stub::reset_alpm_stub();
    stub::set_local_packages({{"", "1.0-1"}});
    {
        PackageMetadataSession empty_name_session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_snapshot_failure(
            empty_name_session.snapshot_local_package_versions(),
            PackageMetadataErrorCode::MalformedMetadata,
            "snapshot empty package name"));
    }

    stub::reset_alpm_stub();
    stub::set_local_packages({{"invalid/package", "1.0-1"}});
    {
        PackageMetadataSession invalid_name_session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_snapshot_failure(
            invalid_name_session.snapshot_local_package_versions(),
            PackageMetadataErrorCode::MalformedMetadata,
            "snapshot invalid package name"));
    }
}

void test_local_package_version_snapshot_rejects_invalid_versions() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"valid-package", "1.0-1"}});
    stub::set_local_package_version_null(0);
    {
        PackageMetadataSession null_version_session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_snapshot_failure(
            null_version_session.snapshot_local_package_versions(),
            PackageMetadataErrorCode::MalformedMetadata,
            "snapshot null package version"));
    }

    stub::reset_alpm_stub();
    stub::set_local_packages({{"valid-package", ""}});
    {
        PackageMetadataSession empty_version_session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_snapshot_failure(
            empty_version_session.snapshot_local_package_versions(),
            PackageMetadataErrorCode::MalformedMetadata,
            "snapshot empty package version"));
    }
}

void test_local_package_version_snapshot_rejects_duplicate_names() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"duplicate-package", "1.0-1"},
                              {"duplicate-package", "2.0-1"}});
    PackageMetadataSession session =
        PackageMetadataSession::open(valid_database_paths());

    static_cast<void>(require_snapshot_failure(
        session.snapshot_local_package_versions(),
        PackageMetadataErrorCode::MalformedMetadata,
        "snapshot duplicate package name"));
}

void test_local_package_version_snapshot_reports_invalid_cache_entry() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"valid-package", "1.0-1"}});
    PackageMetadataSession session =
        PackageMetadataSession::open(valid_database_paths());
    stub::set_local_package_cache_entry_null(0);

    static_cast<void>(require_snapshot_failure(
        session.snapshot_local_package_versions(),
        PackageMetadataErrorCode::QueryFailed,
        "snapshot invalid cache entry"));
}

void test_moved_from_session_snapshot_reports_query_failure() {
    stub::reset_alpm_stub();
    PackageMetadataSession source =
        PackageMetadataSession::open(valid_database_paths());
    PackageMetadataSession destination(std::move(source));
    static_cast<void>(destination);

    static_cast<void>(require_snapshot_failure(
        source.snapshot_local_package_versions(),
        PackageMetadataErrorCode::QueryFailed,
        "moved-from snapshot session"));
}

void test_installed_package_state_snapshot_is_owned_and_keeps_reasons() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"explicit-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT},
                              {"dependency-package", "2.0-3", ALPM_PKG_REASON_DEPEND},
                              {"unknown-package", "3.0-2", ALPM_PKG_REASON_UNKNOWN}});
    stub::set_local_package_base(0, "explicit-base");
    stub::set_local_package_architecture(0, "x86_64");
    stub::set_local_package_base(1, "dependency-base");
    stub::set_local_package_architecture(1, "any");
    stub::set_local_package_base(2, "unknown-base");
    stub::set_local_package_architecture(2, "aarch64");

    InstalledPackageStateSnapshot snapshot;
    {
        PackageMetadataSession session =
            PackageMetadataSession::open(valid_database_paths());
        InstalledPackageStateSnapshotResult result =
            session.snapshot_installed_package_states();
        snapshot = require_result_alternative<InstalledPackageStateSnapshot>(
            result, "installed package state snapshot");
        expect(
            stub::package_cache_call_count() == 1,
            "state snapshot reloaded the preloaded local package cache");
    }

    stub::reset_alpm_stub();
    expect(snapshot.size() == 3, "state snapshot package count differs");
    const InstalledPackageMetadata& explicit_package =
        snapshot.at("explicit-package");
    expect(
        explicit_package.name == "explicit-package" &&
            explicit_package.version == "1.0-1" &&
            explicit_package.reason ==
                InstalledPackageReason::Explicit,
        "state snapshot lost Explicit metadata");
    expect_installed_identity_value(
        explicit_package.package_base,
        InstalledPackageMetadataValueState::Known,
        std::string("explicit-base"),
        "explicit snapshot PackageBase");
    expect_installed_identity_value(
        explicit_package.architecture,
        InstalledPackageMetadataValueState::Known,
        std::string("x86_64"),
        "explicit snapshot architecture");
    const InstalledPackageMetadata& dependency_package =
        snapshot.at("dependency-package");
    expect(
        dependency_package.name == "dependency-package" &&
            dependency_package.version == "2.0-3" &&
            dependency_package.reason ==
                InstalledPackageReason::Dependency,
        "state snapshot lost Dependency metadata");
    expect_installed_identity_value(
        dependency_package.package_base,
        InstalledPackageMetadataValueState::Known,
        std::string("dependency-base"),
        "dependency snapshot PackageBase");
    expect_installed_identity_value(
        dependency_package.architecture,
        InstalledPackageMetadataValueState::Known,
        std::string("any"),
        "dependency snapshot architecture");
    const InstalledPackageMetadata& unknown_package =
        snapshot.at("unknown-package");
    expect(
        unknown_package.name == "unknown-package" &&
            unknown_package.version == "3.0-2" &&
            unknown_package.reason ==
                InstalledPackageReason::Unknown,
        "state snapshot rewrote Unknown install reason");
    expect_installed_identity_value(
        unknown_package.package_base,
        InstalledPackageMetadataValueState::Known,
        std::string("unknown-base"),
        "unknown-reason snapshot PackageBase");
    expect_installed_identity_value(
        unknown_package.architecture,
        InstalledPackageMetadataValueState::Known,
        std::string("aarch64"),
        "unknown-reason snapshot architecture");
}

void test_installed_package_state_snapshot_retains_incomplete_identity() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"missing-base", "1.0-1"},
                              {"malformed-base", "2.0-1"}});
    stub::set_local_package_base_null(0);
    stub::set_local_package_architecture(0, "invalid architecture");
    stub::set_local_package_base(1, "invalid/base");
    stub::set_local_package_architecture_null(1);

    PackageMetadataSession session =
        PackageMetadataSession::open(valid_database_paths());
    const InstalledPackageStateSnapshot snapshot =
        require_result_alternative<InstalledPackageStateSnapshot>(
            session.snapshot_installed_package_states(),
            "incomplete installed identity snapshot");

    expect_installed_identity_value(
        snapshot.at("missing-base").package_base,
        InstalledPackageMetadataValueState::Missing, std::nullopt,
        "missing snapshot PackageBase");
    expect_installed_identity_value(
        snapshot.at("missing-base").architecture,
        InstalledPackageMetadataValueState::Malformed, std::nullopt,
        "malformed snapshot architecture");
    expect_installed_identity_value(
        snapshot.at("malformed-base").package_base,
        InstalledPackageMetadataValueState::Malformed, std::nullopt,
        "malformed snapshot PackageBase");
    expect_installed_identity_value(
        snapshot.at("malformed-base").architecture,
        InstalledPackageMetadataValueState::Missing, std::nullopt,
        "missing snapshot architecture");
}

void test_installed_package_state_snapshot_rejects_malformed_metadata() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    {
        PackageMetadataSession session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_state_snapshot_failure(
            session.snapshot_installed_package_states(),
            PackageMetadataErrorCode::MalformedMetadata,
            "state snapshot empty package name"));
    }

    stub::reset_alpm_stub();
    stub::set_local_packages(
        {{"valid-package", "", ALPM_PKG_REASON_DEPEND}});
    {
        PackageMetadataSession session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_state_snapshot_failure(
            session.snapshot_installed_package_states(),
            PackageMetadataErrorCode::MalformedMetadata,
            "state snapshot empty package version"));
    }

    stub::reset_alpm_stub();
    stub::set_local_packages({{"duplicate-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT},
                              {"duplicate-package", "2.0-1", ALPM_PKG_REASON_DEPEND}});
    {
        PackageMetadataSession session =
            PackageMetadataSession::open(valid_database_paths());
        static_cast<void>(require_state_snapshot_failure(
            session.snapshot_installed_package_states(),
            PackageMetadataErrorCode::MalformedMetadata,
            "state snapshot duplicate package name"));
    }
}

void test_installed_package_state_snapshot_failure_is_not_empty_inventory() {
    stub::reset_alpm_stub();
    stub::set_initialize_failure(ALPM_ERR_SYSTEM);
    static_cast<void>(require_state_snapshot_failure(
        snapshot_installed_package_states(valid_database_paths()),
        PackageMetadataErrorCode::InitializationFailed,
        "state snapshot session initialization failure"));

    stub::reset_alpm_stub();
    PackageMetadataSession source =
        PackageMetadataSession::open(valid_database_paths());
    PackageMetadataSession destination(std::move(source));
    static_cast<void>(destination);
    static_cast<void>(require_state_snapshot_failure(
        source.snapshot_installed_package_states(),
        PackageMetadataErrorCode::QueryFailed,
        "state snapshot moved-from query failure"));
}

void test_empty_installed_package_state_snapshot_confirms_no_entries() {
    stub::reset_alpm_stub();
    stub::set_empty_package_cache();

    InstalledPackageStateSnapshotResult result =
        snapshot_installed_package_states(valid_database_paths());
    InstalledPackageStateSnapshot snapshot =
        require_result_alternative<InstalledPackageStateSnapshot>(
            result, "empty installed package state snapshot");
    expect(snapshot.empty(), "empty local DB produced state snapshot entries");
}

void test_installed_runtime_dependency_metadata_is_owned_and_canonical() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"virtualbox", "7.2.14-1", ALPM_PKG_REASON_EXPLICIT},
                              {"virtualbox-ext-oracle",
                               "7.2.14-1",
                               ALPM_PKG_REASON_EXPLICIT,
                               {},
                               {{std::string("virtualbox"),
                                 std::string("7.2.14"),
                                 ALPM_DEP_MOD_EQ},
                                {std::string("glibc"), std::nullopt, ALPM_DEP_MOD_ANY},
                                {std::string("linux"),
                                 std::string("6.0"),
                                 ALPM_DEP_MOD_GE}}}});

    InstalledPackageRuntimeDependencyMetadataInventoryResult result =
        query_installed_package_runtime_dependency_metadata(
            valid_database_paths());
    InstalledPackageRuntimeDependencyMetadataInventory inventory =
        require_result_alternative<
            InstalledPackageRuntimeDependencyMetadataInventory>(
            result, "installed runtime dependency inventory");
    expect(
        stub::created_handle_count() == 1 &&
            stub::release_count_for_handle(0) == 1,
        "Runtime dependency inventory did not release its read handle");

    stub::reset_alpm_stub();
    expect(inventory.size() == 2,
           "Runtime dependency inventory package count differs");
    expect(
        inventory[0].package_name == "virtualbox" &&
            inventory[0].dependency_specifications.empty(),
        "Dependency-free installed package acquired dependencies");
    expect(
        inventory[1].package_name == "virtualbox-ext-oracle" &&
            inventory[1].installed_version == std::optional<std::string>{"7.2.14-1"} &&
            inventory[1].dependency_specifications ==
                std::vector<std::string>{
                    "virtualbox=7.2.14",
                    "glibc",
                    "linux>=6.0"},
        "libalpm runtime dependencies were not retained canonically");
}

void test_installed_runtime_dependency_failure_is_not_empty_inventory() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"observed-package",
                               "1.0-1",
                               ALPM_PKG_REASON_EXPLICIT,
                               {},
                               {{std::string("glibc"), std::nullopt, ALPM_DEP_MOD_ANY}}},
                              {"malformed-package",
                               "2.0-1",
                               ALPM_PKG_REASON_EXPLICIT,
                               {},
                               {{std::nullopt, std::string("1"), ALPM_DEP_MOD_EQ}}}});

    InstalledPackageRuntimeDependencyMetadataInventoryResult result =
        query_installed_package_runtime_dependency_metadata(
            valid_database_paths());
    const auto& failure =
        require_result_alternative<
            InstalledPackageRuntimeDependencyMetadataInventoryFailure>(
            result, "installed runtime dependency failure");
    expect(
        failure.package_index == std::optional<std::size_t>(1) &&
            failure.failure.code ==
                PackageMetadataErrorCode::MalformedMetadata &&
            failure.observed_packages.size() == 1 &&
            failure.observed_packages.front().package_name ==
                "observed-package" &&
            failure.observed_packages.front()
                    .dependency_specifications ==
                std::vector<std::string>{"glibc"},
        "Runtime dependency failure was flattened or lost its prefix");
}

void test_package_absent() {
    stub::reset_alpm_stub();
    stub::set_package_absent();
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_result_alternative<PackageNotFound>(result, "package absence"));
}

void test_package_present() {
    stub::reset_alpm_stub();
    stub::set_package_metadata("test-package", "2.4.1-3", ALPM_PKG_REASON_EXPLICIT);
    stub::set_local_package_base(0, "actual-package-base");
    stub::set_local_package_architecture(0, "any");
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");
    InstalledPackageMetadata metadata =
        require_result_alternative<InstalledPackageMetadata>(result, "package present");

    expect(metadata.name == "test-package", "installed package name differs");
    expect(metadata.version == "2.4.1-3", "installed package version differs");
    expect(metadata.reason == InstalledPackageReason::Explicit, "installed package reason differs");
    expect_installed_identity_value(
        metadata.package_base,
        InstalledPackageMetadataValueState::Known,
        std::string("actual-package-base"),
        "installed package PackageBase");
    expect_installed_identity_value(
        metadata.architecture,
        InstalledPackageMetadataValueState::Known,
        std::string("any"),
        "installed package architecture");
}

void test_installed_package_identity_states_are_lossless() {
    const InstalledPackageMetadata legacy{
        "legacy-package", "1.0-1", InstalledPackageReason::Dependency};
    expect_installed_identity_value(
        legacy.package_base,
        InstalledPackageMetadataValueState::Unknown, std::nullopt,
        "legacy PackageBase authority");
    expect_installed_identity_value(
        legacy.architecture,
        InstalledPackageMetadataValueState::Unknown, std::nullopt,
        "legacy architecture authority");
    expect_installed_identity_value(
        InstalledPackageBaseIdentity::unavailable(),
        InstalledPackageMetadataValueState::Unavailable, std::nullopt,
        "unavailable PackageBase authority");
    expect_installed_identity_value(
        InstalledPackageArchitectureIdentity::unavailable(),
        InstalledPackageMetadataValueState::Unavailable, std::nullopt,
        "unavailable architecture authority");

    stub::reset_alpm_stub();
    stub::set_package_metadata(
        "test-package", "1.0-1", ALPM_PKG_REASON_DEPEND);
    stub::set_local_package_base_null(0);
    stub::set_local_package_architecture(0, "invalid architecture");
    {
        PackageMetadataSession first =
            PackageMetadataSession::open(valid_database_paths());
        const InstalledPackageMetadata missing_base =
            require_result_alternative<InstalledPackageMetadata>(
                first.query_installed_package("test-package"),
                "missing PackageBase query");
        expect_installed_identity_value(
            missing_base.package_base,
            InstalledPackageMetadataValueState::Missing, std::nullopt,
            "missing queried PackageBase");
        expect_installed_identity_value(
            missing_base.architecture,
            InstalledPackageMetadataValueState::Malformed, std::nullopt,
            "malformed queried architecture");
    }

    stub::reset_alpm_stub();
    stub::set_package_metadata(
        "test-package", "1.0-1", ALPM_PKG_REASON_DEPEND);
    stub::set_local_package_base(0, "invalid/base");
    stub::set_local_package_architecture_null(0);
    PackageMetadataSession second =
        PackageMetadataSession::open(valid_database_paths());
    const InstalledPackageMetadata malformed_base =
        require_result_alternative<InstalledPackageMetadata>(
            second.query_installed_package("test-package"),
            "malformed PackageBase query");
    expect_installed_identity_value(
        malformed_base.package_base,
        InstalledPackageMetadataValueState::Malformed, std::nullopt,
        "malformed queried PackageBase");
    expect_installed_identity_value(
        malformed_base.architecture,
        InstalledPackageMetadataValueState::Missing, std::nullopt,
        "missing queried architecture");
}

void test_returned_package_name_mismatch() {
    stub::reset_alpm_stub();
    stub::set_package_metadata("different-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT);
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "returned package name mismatch"));
}

void test_null_package_name() {
    stub::reset_alpm_stub();
    stub::set_package_metadata("test-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT);
    stub::set_null_package_name();
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::MalformedMetadata, "null package name"));
}

void test_null_package_version() {
    stub::reset_alpm_stub();
    stub::set_package_metadata("test-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT);
    stub::set_null_package_version();
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::MalformedMetadata, "null package version"));
}

void test_empty_package_version() {
    stub::reset_alpm_stub();
    stub::set_package_metadata("test-package", "", ALPM_PKG_REASON_EXPLICIT);
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::MalformedMetadata, "empty package version"));
}

void expect_reason_mapping(
    alpm_pkgreason_t alpm_reason, InstalledPackageReason expected_reason,
    const std::string& context) {
    stub::reset_alpm_stub();
    stub::set_package_metadata("test-package", "1.0-1", alpm_reason);
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");
    InstalledPackageMetadata metadata =
        require_result_alternative<InstalledPackageMetadata>(result, context);
    expect(metadata.reason == expected_reason, context + ": mapped reason differs");
}

void test_install_reason_mapping() {
    expect_reason_mapping(
        ALPM_PKG_REASON_EXPLICIT, InstalledPackageReason::Explicit,
        "explicit install reason");
    expect_reason_mapping(
        ALPM_PKG_REASON_DEPEND, InstalledPackageReason::Dependency,
        "dependency install reason");
    expect_reason_mapping(
        ALPM_PKG_REASON_UNKNOWN, InstalledPackageReason::Unknown,
        "unknown install reason");
    expect_reason_mapping(
        static_cast<alpm_pkgreason_t>(99), InstalledPackageReason::Unknown,
        "future install reason");
}

void test_invalid_package_name() {
    stub::reset_alpm_stub();
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("invalid/package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::InvalidPackageName, "invalid package name"));
    expect(stub::package_query_call_count() == 0, "invalid package name reached libalpm");
}

void test_query_failure_is_not_absence() {
    stub::reset_alpm_stub();
    stub::set_package_query_failure(ALPM_ERR_DB_OPEN);
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::QueryFailed,
        "libalpm package query failure"));
}

void test_null_query_without_error_is_not_absence() {
    stub::reset_alpm_stub();
    stub::set_package_query_null_without_error();
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult result = session.query_installed_package("test-package");

    static_cast<void>(require_query_failure(
        result, PackageMetadataErrorCode::QueryFailed,
        "package query null without libalpm error"));
}

void test_stale_query_error_does_not_override_found_package() {
    stub::reset_alpm_stub();
    stub::set_package_query_failure(ALPM_ERR_DB_OPEN);
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult failed_result =
        session.query_installed_package("test-package");
    static_cast<void>(require_query_failure(
        failed_result, PackageMetadataErrorCode::QueryFailed,
        "initial package query failure"));

    stub::set_package_metadata("test-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT);
    stub::preserve_error_on_next_package_query();
    InstalledPackageQueryResult found_result =
        session.query_installed_package("test-package");

    static_cast<void>(require_result_alternative<InstalledPackageMetadata>(
        found_result, "package found with stale libalpm error"));
}

void test_stale_query_error_does_not_override_package_absence() {
    stub::reset_alpm_stub();
    stub::set_package_query_failure(ALPM_ERR_DB_OPEN);
    PackageMetadataSession session = PackageMetadataSession::open(valid_database_paths());

    InstalledPackageQueryResult failed_result =
        session.query_installed_package("test-package");
    static_cast<void>(require_query_failure(
        failed_result, PackageMetadataErrorCode::QueryFailed,
        "initial package query failure"));

    stub::set_package_absent();
    InstalledPackageQueryResult absent_result =
        session.query_installed_package("test-package");

    static_cast<void>(require_result_alternative<PackageNotFound>(
        absent_result, "package absent after stale libalpm error"));
}

void test_move_construction_does_not_double_release() {
    stub::reset_alpm_stub();
    {
        PackageMetadataSession source = PackageMetadataSession::open(valid_database_paths());
        PackageMetadataSession destination(std::move(source));
        static_cast<void>(destination);
        expect(stub::release_call_count() == 0, "move construction released live handle");
    }
    expect(stub::created_handle_count() == 1, "move construction created extra handle");
    expect(stub::release_count_for_handle(0) == 1, "moved handle was not released exactly once");
}

void test_move_assignment_does_not_double_release() {
    stub::reset_alpm_stub();
    {
        PackageMetadataSession source = PackageMetadataSession::open(valid_database_paths());
        PackageMetadataSession destination = PackageMetadataSession::open(valid_database_paths());

        destination = std::move(source);

        expect(
            stub::release_count_for_handle(0) == 0,
            "move assignment released transferred handle early");
        expect(
            stub::release_count_for_handle(1) == 1,
            "move assignment did not release destination's old handle");
    }
    expect(stub::created_handle_count() == 2, "move assignment handle count differs");
    expect(stub::release_count_for_handle(0) == 1, "transferred handle release count differs");
    expect(stub::release_count_for_handle(1) == 1, "old destination handle was double released");
}

void test_foreign_inventory_returns_initialization_failure_as_value() {
    stub::reset_alpm_stub();
    stub::set_initialize_failure(ALPM_ERR_SYSTEM);

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::InitializationFailed,
        "foreign inventory initialization failure"));
    expect(
        stub::created_handle_count() == 0,
        "foreign inventory initialization failure created a handle");
    expect(
        stub::release_call_count() == 0,
        "foreign inventory initialization failure released a handle");
}

void test_foreign_inventory_requires_configured_repository() {
    stub::reset_alpm_stub();

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::RepositoryNotConfigured,
        "foreign inventory without repositories"));
    expect(stub::initialize_call_count() == 0, "empty repository list reached libalpm");
    expect(stub::release_call_count() == 0, "empty repository list released no handle");
}

void test_empty_local_cache_is_empty_foreign_inventory() {
    stub::reset_alpm_stub();
    stub::set_empty_package_cache();

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core", "extra"})),
            "empty local cache inventory");

    expect(inventory.empty(), "empty local cache produced foreign packages");
    expect(stub::package_cache_call_count() == 1, "local cache was not loaded once");
    expect(
        stub::sync_package_cache_call_count("core") == 1 &&
            stub::sync_package_cache_call_count("extra") == 1,
        "sync caches were not preloaded for empty inventory");
    expect(stub::release_count_for_handle(0) == 1, "empty inventory leaked its handle");
}

void test_foreign_inventory_local_database_failure() {
    stub::reset_alpm_stub();
    stub::set_local_database_unavailable();

    ForeignPackageInventoryResult unavailable_result =
        query_foreign_package_inventory(
            valid_repository_configuration({"core"}));
    static_cast<void>(require_inventory_failure(
        unavailable_result,
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "foreign inventory unavailable local database"));
    expect(
        stub::release_count_for_handle(0) == 1,
        "unavailable local database leaked handle");

    stub::reset_alpm_stub();
    stub::set_local_database_invalid();
    ForeignPackageInventoryResult invalid_result =
        query_foreign_package_inventory(
            valid_repository_configuration({"core"}));
    static_cast<void>(require_inventory_failure(
        invalid_result,
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "foreign inventory invalid local database"));
    expect(
        stub::release_count_for_handle(0) == 1,
        "invalid local database leaked handle");
}

void test_foreign_inventory_local_cache_failure() {
    stub::reset_alpm_stub();
    stub::set_package_cache_failure();

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "foreign inventory local cache failure"));
    expect(stub::release_count_for_handle(0) == 1, "local cache failure leaked handle");
    expect(
        stub::sync_package_cache_call_count("core") == 0,
        "sync cache loaded after local cache failure");
}

void test_foreign_inventory_sync_register_failure() {
    stub::reset_alpm_stub();
    stub::set_sync_database_register_failure("extra");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core", "extra", "testing"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::SyncDatabaseUnavailable,
        "foreign inventory sync register failure"));
    expect(
        stub::sync_database_operation_history() ==
            std::vector<std::string>({"register core", "register extra"}),
        "sync register failure did not stop immediately");
    expect(stub::release_count_for_handle(0) == 1, "sync register failure leaked handle");
}

void test_foreign_inventory_sync_validation_failure() {
    stub::reset_alpm_stub();
    stub::set_sync_database_validation_failure("extra");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core", "extra", "testing"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::SyncDatabaseUnavailable,
        "foreign inventory sync validation failure"));
    expect(
        stub::sync_database_operation_history() ==
            std::vector<std::string>({"register core", "register extra", "register testing",
                                      "valid core", "valid extra"}),
        "sync validation failure did not stop before cache load");
    expect(stub::release_count_for_handle(0) == 1, "sync validation failure leaked handle");
}

void test_foreign_inventory_sync_cache_failure() {
    stub::reset_alpm_stub();
    stub::set_sync_database_cache_failure("extra");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core", "extra", "testing"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::SyncDatabaseUnavailable,
        "foreign inventory sync cache failure"));
    expect(
        stub::sync_package_cache_call_count("core") == 1 &&
            stub::sync_package_cache_call_count("extra") == 1 &&
            stub::sync_package_cache_call_count("testing") == 0,
        "sync cache failure continued with a partial repository snapshot");
    expect(stub::release_count_for_handle(0) == 1, "sync cache failure leaked handle");
}

void test_foreign_inventory_accepts_empty_sync_cache() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_sync_database_empty_cache("core");

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core"})),
            "empty sync cache inventory");

    expect(
        inventory.size() == 1 && inventory[0].name == "foreign-package",
        "empty sync cache did not retain the foreign package");
    expect(
        stub::sync_package_cache_call_count("core") == 1,
        "empty sync cache was not preloaded exactly once");
    expect(
        stub::release_count_for_handle(0) == 1,
        "empty sync cache inventory leaked handle");
}

void test_foreign_inventory_excludes_first_repository_native_package() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_metadata("core", "native-package", 0, 0);
    stub::set_repository_package_metadata("extra", "native-package", 0, 0);

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core", "extra"})),
            "first repository native package");

    expect(inventory.empty(), "first repository native package was classified foreign");
    expect_repository_query_history(
        {{"core", "native-package"}},
        "first repository native package");
}

void test_foreign_inventory_excludes_later_repository_native_package() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_absent("core", "native-package");
    stub::set_repository_package_metadata("extra", "native-package", 0, 0);

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core", "extra"})),
            "later repository native package");

    expect(inventory.empty(), "later repository native package was classified foreign");
    expect_repository_query_history(
        {{"core", "native-package"}, {"extra", "native-package"}},
        "later repository native package");
}

void test_foreign_inventory_preserves_local_order_and_reasons() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"zeta-package", "3.0-1", ALPM_PKG_REASON_EXPLICIT},
                              {"alpha-package", "2.0-1", ALPM_PKG_REASON_DEPEND},
                              {"middle-package", "1.0-1", ALPM_PKG_REASON_UNKNOWN}});

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core", "extra"})),
            "ordered foreign inventory");

    expect(inventory.size() == 3, "foreign inventory size differs");
    expect(
        inventory[0].name == "zeta-package" &&
            inventory[0].version == "3.0-1" &&
            inventory[0].reason == InstalledPackageReason::Explicit,
        "explicit foreign package metadata differs");
    expect(
        inventory[1].name == "alpha-package" &&
            inventory[1].version == "2.0-1" &&
            inventory[1].reason == InstalledPackageReason::Dependency,
        "dependency foreign package metadata differs");
    expect(
        inventory[2].name == "middle-package" &&
            inventory[2].version == "1.0-1" &&
            inventory[2].reason == InstalledPackageReason::Unknown,
        "unknown-reason foreign package metadata differs");
    expect_repository_query_history(
        {
            {"core", "zeta-package"},
            {"extra", "zeta-package"},
            {"core", "alpha-package"},
            {"extra", "alpha-package"},
            {"core", "middle-package"},
            {"extra", "middle-package"},
        },
        "ordered foreign inventory");
}

void test_foreign_inventory_maps_future_reason_to_unknown() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"future-reason", "1.0-1", static_cast<alpm_pkgreason_t>(99)}});

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core"})),
            "future install reason");

    expect(
        inventory.size() == 1 &&
            inventory[0].reason == InstalledPackageReason::Unknown,
        "future install reason was guessed");
}

void test_foreign_inventory_rejects_null_local_name() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"test-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_local_package_name_null(0);

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "null local package name"));
}

void test_foreign_inventory_rejects_empty_local_name() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "empty local package name"));
}

void test_foreign_inventory_rejects_invalid_local_name() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"invalid/package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "invalid local package name"));
}

void test_foreign_inventory_rejects_null_foreign_version() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_local_package_version_null(0);

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "null foreign package version"));
}

void test_foreign_inventory_rejects_empty_foreign_version() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "", ALPM_PKG_REASON_EXPLICIT}});

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "empty foreign package version"));
}

void test_foreign_inventory_does_not_require_native_version() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_metadata("core", "native-package", 0, 0);

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core"})),
            "native package without version");

    expect(inventory.empty(), "native version was unnecessarily required");
}

void test_foreign_inventory_rejects_invalid_sync_returned_name() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_metadata("core", "native-package", 0, 0);
    stub::set_repository_package_returned_name(
        "core", "native-package", "invalid/package");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "invalid sync returned package name"));
}

void test_foreign_inventory_rejects_sync_returned_name_mismatch() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_metadata("core", "native-package", 0, 0);
    stub::set_repository_package_returned_name(
        "core", "native-package", "different-package");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "sync returned package name mismatch"));
}

void test_foreign_inventory_rejects_null_sync_returned_name() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_metadata("core", "native-package", 0, 0);
    stub::set_repository_package_name_null("core", "native-package");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::MalformedMetadata,
        "null sync returned package name"));
}

void test_foreign_inventory_distinguishes_absence_from_query_failure() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});

    ForeignPackageInventory absent_inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core"})),
            "explicit repository package absence");
    expect(
        absent_inventory.size() == 1 &&
            absent_inventory[0].name == "foreign-package",
        "repository absence was not classified foreign");

    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_query_failure("core", "foreign-package");
    ForeignPackageInventoryResult failure_result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));
    static_cast<void>(require_inventory_failure(
        failure_result, PackageMetadataErrorCode::QueryFailed,
        "repository membership query failure"));

    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_query_null_without_error(
        "core", "foreign-package");
    ForeignPackageInventoryResult null_without_error =
        query_foreign_package_inventory(
            valid_repository_configuration({"core"}));
    static_cast<void>(require_inventory_failure(
        null_without_error, PackageMetadataErrorCode::QueryFailed,
        "repository membership nullptr with OK"));
}

void test_foreign_inventory_ignores_stale_errno_on_successful_pointers() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"native-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::preserve_error_on_next_local_database();
    stub::preserve_error_on_next_sync_database_registration("core");
    stub::preserve_error_on_next_package_cache();
    stub::preserve_error_on_next_sync_database_cache("core");
    stub::set_repository_package_metadata("core", "native-package", 0, 0);
    stub::preserve_error_on_next_repository_package_query(
        "core", "native-package");

    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            query_foreign_package_inventory(
                valid_repository_configuration({"core"})),
            "successful pointers with stale errno");

    expect(inventory.empty(), "stale errno overrode a successful pointer");
}

void test_foreign_inventory_preloads_all_sync_caches_before_lookup() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});

    static_cast<void>(require_result_alternative<ForeignPackageInventory>(
        query_foreign_package_inventory(
            valid_repository_configuration({"core", "extra"})),
        "sync cache preload order"));

    expect(
        stub::sync_database_operation_history() ==
            std::vector<std::string>({"register core", "register extra",
                                      "valid core", "valid extra",
                                      "cache core", "cache extra",
                                      "query core/foreign-package",
                                      "query extra/foreign-package"}),
        "repository lookup began before all sync caches were loaded");
}

void test_foreign_inventory_releases_handle_once_on_query_failure() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"foreign-package", "1.0-1", ALPM_PKG_REASON_EXPLICIT}});
    stub::set_repository_package_query_failure("core", "foreign-package");

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_inventory_failure(
        result, PackageMetadataErrorCode::QueryFailed,
        "inventory failure handle release"));
    expect(stub::created_handle_count() == 1, "inventory failure created extra handles");
    expect(stub::release_count_for_handle(0) == 1, "inventory failure release count differs");
}

void test_foreign_inventory_returns_owned_values_after_release() {
    stub::reset_alpm_stub();
    stub::set_local_packages({{"owned-package", "4.2-1", ALPM_PKG_REASON_DEPEND}});

    ForeignPackageInventoryResult result = query_foreign_package_inventory(
        valid_repository_configuration({"core"}));
    expect(stub::created_handle_count() == 1, "owned inventory created extra handles");
    expect(stub::release_count_for_handle(0) == 1, "owned inventory handle was not released");

    stub::reset_alpm_stub();
    ForeignPackageInventory inventory =
        require_result_alternative<ForeignPackageInventory>(
            result, "owned foreign inventory");
    expect(
        inventory.size() == 1 && inventory[0].name == "owned-package" &&
            inventory[0].version == "4.2-1" &&
            inventory[0].reason == InstalledPackageReason::Dependency,
        "foreign inventory retained borrowed metadata");
}

void test_repository_session_open_registers_in_configured_order() {
    stub::reset_alpm_stub();
    {
        RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"testing", "core", "extra"}));
        static_cast<void>(session);

        std::vector<stub::SyncDatabaseRegistration> registrations =
            stub::sync_database_registration_history();
        expect(registrations.size() == 3, "sync registration count differs");
        expect(
            registrations[0].repository_name == "testing" &&
                registrations[1].repository_name == "core" &&
                registrations[2].repository_name == "extra",
            "sync registration order differs");
        for(const auto& registration : registrations) {
            expect(registration.signature_level == 0, "sync registration siglevel differs");
        }
        expect(
            stub::sync_database_operation_history() ==
                std::vector<std::string>({"register testing", "valid testing", "cache testing",
                                          "register core", "valid core", "cache core",
                                          "register extra", "valid extra", "cache extra"}),
            "sync register/valid/cache order differs");
        expect(stub::local_database_call_count() == 0, "sync session accessed local DB");
        expect(stub::release_call_count() == 0, "sync session released handle early");
    }
    expect(stub::release_count_for_handle(0) == 1, "sync session leaked its handle");
}

void test_repository_session_accepts_empty_repository_list() {
    stub::reset_alpm_stub();
    {
        RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
            valid_repository_configuration({}));
        RepositoryPackageQueryResult result = session.query_repository_package(
            RepositoryPackageLookup{"test-package", std::nullopt});
        static_cast<void>(require_result_alternative<PackageNotFound>(
            result, "empty repository session query"));
    }
    expect(
        stub::sync_database_registration_history().empty(),
        "empty repository session registered a database");
    expect(stub::release_count_for_handle(0) == 1, "empty repository session leaked handle");
}

void test_repository_session_revalidates_manual_configuration() {
    stub::reset_alpm_stub();
    PacmanRepositoryConfiguration duplicate_configuration =
        valid_repository_configuration({"core", "core"});
    expect_metadata_error(
        [&duplicate_configuration]() {
            static_cast<void>(RepositoryPackageMetadataSession::open(
                duplicate_configuration));
        },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "manual duplicate repository configuration");
    expect(stub::initialize_call_count() == 0, "duplicate repository reached libalpm");

    PacmanRepositoryConfiguration control_configuration =
        valid_repository_configuration({"core", "ex\ttra"});
    expect_metadata_error(
        [&control_configuration]() {
            static_cast<void>(RepositoryPackageMetadataSession::open(
                control_configuration));
        },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "manual control repository configuration");
    expect(stub::initialize_call_count() == 0, "control repository reached libalpm");

    PacmanRepositoryConfiguration invalid_path_configuration{
        PacmanDatabasePaths{"relative/root", "/var/lib/pacman"},
        {"core"}};
    expect_metadata_error(
        [&invalid_path_configuration]() {
            static_cast<void>(RepositoryPackageMetadataSession::open(
                invalid_path_configuration));
        },
        PackageMetadataErrorCode::ConfigurationMalformed,
        "manual repository path configuration");
    expect(stub::initialize_call_count() == 0, "invalid repository path reached libalpm");
}

void test_repository_register_failure_releases_all_state() {
    stub::reset_alpm_stub();
    stub::set_sync_database_register_failure("extra");

    expect_metadata_error(
        []() {
            static_cast<void>(RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core", "extra", "testing"})));
        },
        PackageMetadataErrorCode::SyncDatabaseUnavailable,
        "sync database registration failure");

    expect(
        stub::sync_database_operation_history() ==
            std::vector<std::string>({"register core", "valid core", "cache core", "register extra"}),
        "registration failure did not stop in configured order");
    expect(stub::release_count_for_handle(0) == 1, "registration failure leaked handle");
}

void test_repository_validation_failure_releases_all_state() {
    stub::reset_alpm_stub();
    stub::set_sync_database_validation_failure("extra");

    expect_metadata_error(
        []() {
            static_cast<void>(RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core", "extra", "testing"})));
        },
        PackageMetadataErrorCode::SyncDatabaseUnavailable,
        "sync database validation failure");

    expect(
        stub::sync_database_operation_history() ==
            std::vector<std::string>({"register core", "valid core", "cache core",
                                      "register extra", "valid extra"}),
        "validation failure did not stop before cache/later repository");
    expect(stub::release_count_for_handle(0) == 1, "validation failure leaked handle");
}

void test_repository_cache_failure_releases_all_state() {
    stub::reset_alpm_stub();
    stub::set_sync_database_cache_failure("extra");

    expect_metadata_error(
        []() {
            static_cast<void>(RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core", "extra", "testing"})));
        },
        PackageMetadataErrorCode::SyncDatabaseUnavailable,
        "sync database cache failure");

    expect(
        stub::sync_database_operation_history() ==
            std::vector<std::string>({"register core", "valid core", "cache core",
                                      "register extra", "valid extra", "cache extra"}),
        "cache failure continued to later repository");
    expect(stub::release_count_for_handle(0) == 1, "cache failure leaked handle");
}

void test_empty_repository_cache_is_valid() {
    stub::reset_alpm_stub();
    stub::set_sync_database_empty_cache("core");

    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));
    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"test-package", std::nullopt});

    static_cast<void>(require_result_alternative<PackageNotFound>(
        result, "empty sync cache query"));
}

void test_repository_precedence_uses_first_configured_hit() {
    stub::reset_alpm_stub();
    stub::set_repository_package_metadata("core", "same-package", 10, 20);
    stub::set_repository_package_metadata("extra", "same-package", 30, 40);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"same-package", std::nullopt});
    RepositoryPackageMetadata metadata =
        require_result_alternative<RepositoryPackageMetadata>(result, "first repo hit");

    expect(metadata.repository_name == "core", "first configured repository did not win");
    expect(metadata.package_name == "same-package", "repository package name differs");
    expect(metadata.package_size_bytes == 10, "repository package size differs");
    expect(metadata.installed_size_bytes == 20, "repository installed size differs");
    expect_repository_query_history({{"core", "same-package"}}, "first repo hit");
}

void test_repository_precedence_continues_after_explicit_miss() {
    stub::reset_alpm_stub();
    stub::set_repository_package_absent("core", "later-package");
    stub::set_repository_package_metadata("extra", "later-package", 50, 60);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"later-package", std::nullopt});
    RepositoryPackageMetadata metadata = require_result_alternative<RepositoryPackageMetadata>(
        result, "earlier miss later hit");

    expect(metadata.repository_name == "extra", "later repository hit differs");
    expect_repository_query_history(
        {{"core", "later-package"}, {"extra", "later-package"}},
        "earlier miss later hit");
}

void test_repository_precedence_stops_after_query_failure() {
    stub::reset_alpm_stub();
    stub::set_repository_package_query_failure("core", "broken-package");
    stub::set_repository_package_metadata("extra", "broken-package", 50, 60);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"broken-package", std::nullopt});

    static_cast<void>(require_repository_query_failure(
        result, PackageMetadataErrorCode::QueryFailed,
        "earlier repository query failure"));
    expect_repository_query_history(
        {{"core", "broken-package"}},
        "earlier repository query failure");
}

void test_all_repository_misses_return_not_found() {
    stub::reset_alpm_stub();
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"missing-package", std::nullopt});

    static_cast<void>(require_result_alternative<PackageNotFound>(
        result, "all repository misses"));
    expect_repository_query_history(
        {{"core", "missing-package"}, {"extra", "missing-package"}},
        "all repository misses");
}

void test_exact_repository_lookup_does_not_use_precedence_fallback() {
    stub::reset_alpm_stub();
    stub::set_repository_package_metadata("core", "provider-package", 10, 20);
    stub::set_repository_package_metadata("extra", "provider-package", 30, 40);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"provider-package", std::string("extra")});
    RepositoryPackageMetadata metadata = require_result_alternative<RepositoryPackageMetadata>(
        result, "exact repository lookup");

    expect(metadata.repository_name == "extra", "exact repository lookup selected wrong repo");
    expect(metadata.package_size_bytes == 30, "exact repository package size differs");
    expect_repository_query_history(
        {{"extra", "provider-package"}},
        "exact repository lookup");
}

void test_exact_repository_miss_is_not_found_without_fallback() {
    stub::reset_alpm_stub();
    stub::set_repository_package_metadata("core", "provider-package", 10, 20);
    stub::set_repository_package_absent("extra", "provider-package");
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"provider-package", std::string("extra")});

    static_cast<void>(require_result_alternative<PackageNotFound>(
        result, "exact repository miss"));
    expect_repository_query_history(
        {{"extra", "provider-package"}},
        "exact repository miss");
}

void test_unconfigured_exact_repository_is_failure() {
    stub::reset_alpm_stub();
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core", "extra"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"provider-package", std::string("testing")});

    static_cast<void>(require_repository_query_failure(
        result, PackageMetadataErrorCode::RepositoryNotConfigured,
        "unconfigured exact repository"));
    expect_repository_query_history({}, "unconfigured exact repository");
}

void test_repository_query_rejects_invalid_package_name() {
    stub::reset_alpm_stub();
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"invalid/package", std::nullopt});

    static_cast<void>(require_repository_query_failure(
        result, PackageMetadataErrorCode::InvalidPackageName,
        "invalid repository package name"));
    expect_repository_query_history({}, "invalid repository package name");
}

void test_repository_query_success_ignores_stale_errno() {
    stub::reset_alpm_stub();
    stub::set_repository_package_query_failure("core", "test-package");
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));
    RepositoryPackageQueryResult failed_result = session.query_repository_package(
        RepositoryPackageLookup{"test-package", std::nullopt});
    static_cast<void>(require_repository_query_failure(
        failed_result, PackageMetadataErrorCode::QueryFailed,
        "initial repository query failure"));

    stub::set_repository_package_metadata("core", "test-package", 10, 20);
    stub::preserve_error_on_next_repository_package_query("core", "test-package");
    RepositoryPackageQueryResult found_result = session.query_repository_package(
        RepositoryPackageLookup{"test-package", std::nullopt});

    static_cast<void>(require_result_alternative<RepositoryPackageMetadata>(
        found_result, "repository success with stale errno"));
}

void test_repository_query_null_without_error_is_failure() {
    stub::reset_alpm_stub();
    stub::set_repository_package_query_null_without_error("core", "test-package");
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));

    RepositoryPackageQueryResult result = session.query_repository_package(
        RepositoryPackageLookup{"test-package", std::nullopt});

    static_cast<void>(require_repository_query_failure(
        result, PackageMetadataErrorCode::QueryFailed,
        "repository nullptr with OK"));
}

void test_repository_metadata_validates_returned_name() {
    stub::reset_alpm_stub();
    stub::set_repository_package_metadata("core", "test-package", 10, 20);
    stub::set_repository_package_name_null("core", "test-package");
    RepositoryPackageMetadataSession null_name_session =
        RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core"}));
    RepositoryPackageQueryResult null_name_result =
        null_name_session.query_repository_package(
            RepositoryPackageLookup{"test-package", std::nullopt});
    static_cast<void>(require_repository_query_failure(
        null_name_result, PackageMetadataErrorCode::MalformedMetadata,
        "null repository package name"));

    stub::set_repository_package_metadata("core", "test-package", 10, 20);
    stub::set_repository_package_returned_name(
        "core", "test-package", "different-package");
    RepositoryPackageQueryResult mismatch_result =
        null_name_session.query_repository_package(
            RepositoryPackageLookup{"test-package", std::nullopt});
    static_cast<void>(require_repository_query_failure(
        mismatch_result, PackageMetadataErrorCode::MalformedMetadata,
        "mismatched repository package name"));
}

void test_repository_metadata_accepts_positive_and_zero_sizes() {
    stub::reset_alpm_stub();
    stub::set_repository_package_metadata("core", "positive-package", 991730, 5283285);
    stub::set_repository_package_metadata("core", "zero-package", 0, 0);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));

    RepositoryPackageMetadata positive = require_result_alternative<RepositoryPackageMetadata>(
        session.query_repository_package(
            RepositoryPackageLookup{"positive-package", std::nullopt}),
        "positive repository package sizes");
    expect(
        positive.package_size_bytes == 991730 &&
            positive.installed_size_bytes == 5283285,
        "positive repository package sizes differ");

    RepositoryPackageMetadata zero = require_result_alternative<RepositoryPackageMetadata>(
        session.query_repository_package(
            RepositoryPackageLookup{"zero-package", std::nullopt}),
        "zero repository package sizes");
    expect(
        zero.package_size_bytes == 0 && zero.installed_size_bytes == 0,
        "known zero repository package sizes differ");
}

void test_repository_metadata_rejects_negative_sizes() {
    stub::reset_alpm_stub();
    stub::set_repository_package_metadata("core", "negative-package", -1, 20);
    stub::set_repository_package_metadata("core", "negative-installed", 10, -1);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));

    static_cast<void>(require_repository_query_failure(
        session.query_repository_package(
            RepositoryPackageLookup{"negative-package", std::nullopt}),
        PackageMetadataErrorCode::MalformedMetadata,
        "negative repository package size"));
    static_cast<void>(require_repository_query_failure(
        session.query_repository_package(
            RepositoryPackageLookup{"negative-installed", std::nullopt}),
        PackageMetadataErrorCode::MalformedMetadata,
        "negative repository installed size"));
}

void test_repository_metadata_accepts_maximum_off_t() {
    stub::reset_alpm_stub();
    constexpr off_t MAXIMUM_SIZE = std::numeric_limits<off_t>::max();
    stub::set_repository_package_metadata(
        "core", "maximum-package", MAXIMUM_SIZE, MAXIMUM_SIZE);
    RepositoryPackageMetadataSession session = RepositoryPackageMetadataSession::open(
        valid_repository_configuration({"core"}));

    RepositoryPackageMetadata metadata = require_result_alternative<RepositoryPackageMetadata>(
        session.query_repository_package(
            RepositoryPackageLookup{"maximum-package", std::nullopt}),
        "maximum repository package sizes");

    expect(
        metadata.package_size_bytes == static_cast<std::uint64_t>(MAXIMUM_SIZE) &&
            metadata.installed_size_bytes == static_cast<std::uint64_t>(MAXIMUM_SIZE),
        "maximum off_t size conversion differs");
}

void test_repository_root_search_returns_owned_matches_in_repository_order() {
    stub::reset_alpm_stub();
    stub::set_repository_search_results(
        "core", "editor",
        {{"alpha-editor", "1.0-1", "Alpha editor"}});
    stub::set_repository_search_results(
        "extra", "editor",
        {{"beta-editor", "2.0-3", "Beta editor"}});

    RepositoryPackageSearchResult result;
    {
        RepositoryPackageMetadataSession session =
            RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core", "extra"}));
        result = session.query_root_package_search("editor");
    }

    RepositoryPackageSearchSnapshot snapshot =
        require_result_alternative<RepositoryPackageSearchSnapshot>(
            result, "owned repository root search");
    expect(
        snapshot.repository_order ==
            std::vector<std::string>({"core", "extra"}),
        "repository root search order differs");
    expect(snapshot.matches.size() == 2, "repository root search match count differs");
    expect(
        snapshot.matches[0].repository_name == "core" &&
            snapshot.matches[0].package_name == "alpha-editor" &&
            snapshot.matches[0].version ==
                std::optional<std::string>("1.0-1") &&
            snapshot.matches[0].description ==
                std::optional<std::string>("Alpha editor") &&
            snapshot.matches[0].kind ==
                RepositoryPackageSearchMatchKind::Search &&
            !snapshot.matches[0].group_name.has_value(),
        "first repository root search match differs");
    expect(
        snapshot.matches[1].repository_name == "extra" &&
            snapshot.matches[1].package_name == "beta-editor" &&
            snapshot.matches[1].version ==
                std::optional<std::string>("2.0-3") &&
            snapshot.matches[1].description ==
                std::optional<std::string>("Beta editor") &&
            snapshot.matches[1].kind ==
                RepositoryPackageSearchMatchKind::Search &&
            !snapshot.matches[1].group_name.has_value(),
        "second repository root search match differs");
    expect(
        stub::release_count_for_handle(0) == 1,
        "repository root search retained borrowed handle state");
    expect(
        stub::owned_list_free_call_count() == 2,
        "repository search result lists were not caller-freed");

    const auto search_history = stub::repository_search_query_history();
    expect(
        search_history.size() == 2 &&
            search_history[0].repository_name == "core" &&
            search_history[0].query == "editor" &&
            search_history[1].repository_name == "extra" &&
            search_history[1].query == "editor",
        "repository root search query history differs");
}

void test_repository_root_search_adds_exact_group_members_with_provenance() {
    stub::reset_alpm_stub();
    stub::set_repository_search_results(
        "core", "base-devel",
        {{"shared-tool", "1.0-1", "Shared search match"}});
    stub::set_repository_exact_group(
        "core", "base-devel",
        {{"shared-tool", "1.0-1", "Shared search match"},
         {"core-tool", "2.0-1", "Core group member"}});
    stub::set_repository_exact_group(
        "extra", "base-devel",
        {{"shared-tool", "3.0-1", "Lower precedence duplicate"},
         {"extra-tool", "4.0-1", "Extra group member"}});
    RepositoryPackageMetadataSession session =
        RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core", "extra"}));

    RepositoryPackageSearchSnapshot snapshot =
        require_result_alternative<RepositoryPackageSearchSnapshot>(
            session.query_root_package_search("base-devel"),
            "exact repository group search");

    expect(snapshot.matches.size() == 4, "exact group raw match count differs");
    expect(
        snapshot.matches[0].kind ==
                RepositoryPackageSearchMatchKind::Search &&
            snapshot.matches[0].package_name == "shared-tool",
        "search provenance was not retained before group matches");
    expect(
        snapshot.matches[1].repository_name == "core" &&
            snapshot.matches[1].package_name == "shared-tool" &&
            snapshot.matches[1].kind ==
                RepositoryPackageSearchMatchKind::ExactGroup &&
            snapshot.matches[1].group_name ==
                std::optional<std::string>("base-devel"),
        "first exact group match differs");
    expect(
        snapshot.matches[2].repository_name == "core" &&
            snapshot.matches[2].package_name == "core-tool" &&
            snapshot.matches[3].repository_name == "extra" &&
            snapshot.matches[3].package_name == "extra-tool",
        "group expansion did not preserve configured precedence");
    expect(
        stub::group_expansion_call_count() == 1,
        "exact group was not expanded exactly once");
    expect(
        stub::owned_list_free_call_count() == 2,
        "search/group caller-owned lists were not both freed");
}

void test_repository_root_search_failure_discards_partial_snapshot() {
    stub::reset_alpm_stub();
    stub::set_repository_search_results(
        "core", "broken-regex",
        {{"partial-match", "1.0-1", "Must not be published"}});
    stub::set_repository_search_failure(
        "extra", "broken-regex", ALPM_ERR_INVALID_REGEX);
    RepositoryPackageMetadataSession session =
        RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core", "extra"}));

    RepositoryPackageSearchResult result =
        session.query_root_package_search("broken-regex");

    static_cast<void>(require_repository_search_failure(
        result,
        PackageMetadataErrorCode::QueryFailed,
        "eligible repository search failure"));
    expect(
        stub::repository_search_query_history().size() == 2,
        "eligible repository search stopped before the failing database");
    expect(
        stub::owned_list_free_call_count() == 1,
        "partial repository search list was not freed on later failure");
    expect(
        stub::repository_group_query_history().empty() &&
            stub::group_expansion_call_count() == 0,
        "group query continued after repository search failure");
}

void test_repository_root_search_ignores_stale_errno_for_empty_group_result() {
    stub::reset_alpm_stub();
    stub::preserve_error_on_next_repository_search(
        "core", "not-a-group", ALPM_ERR_DB_OPEN);
    {
        RepositoryPackageMetadataSession missing_group_session =
            RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core"}));

        RepositoryPackageSearchSnapshot missing_group_snapshot =
            require_result_alternative<RepositoryPackageSearchSnapshot>(
                missing_group_session.query_root_package_search(
                    "not-a-group"),
                "exact group miss with stale errno");

        expect(
            missing_group_snapshot.matches.empty(),
            "exact group miss with stale errno produced a match");
        expect(
            stub::group_expansion_call_count() == 0,
            "exact group miss started group expansion");
    }

    stub::reset_alpm_stub();
    stub::set_repository_exact_group("core", "empty-group", {});
    stub::preserve_error_on_next_repository_search(
        "core", "empty-group", ALPM_ERR_DB_OPEN);
    RepositoryPackageMetadataSession session =
        RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core"}));

    RepositoryPackageSearchSnapshot snapshot =
        require_result_alternative<RepositoryPackageSearchSnapshot>(
            session.query_root_package_search("empty-group"),
            "empty exact group with stale errno");

    expect(snapshot.matches.empty(), "empty exact group produced a match");
    expect(
        stub::group_expansion_call_count() == 1,
        "empty exact group was not passed to libalpm expansion");
}

void test_repository_root_search_rejects_malformed_group_metadata() {
    stub::reset_alpm_stub();
    stub::set_repository_exact_group("core", "group-name", {});
    stub::set_repository_group_returned_name(
        "core", "group-name", "different-group");
    RepositoryPackageMetadataSession malformed_group_session =
        RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core"}));

    static_cast<void>(require_repository_search_failure(
        malformed_group_session.query_root_package_search("group-name"),
        PackageMetadataErrorCode::MalformedMetadata,
        "mismatched exact group name"));

    stub::reset_alpm_stub();
    stub::set_repository_exact_group("core", "group-name", {});
    stub::append_null_repository_group_member("core", "group-name");
    RepositoryPackageMetadataSession malformed_member_session =
        RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core"}));

    static_cast<void>(require_repository_search_failure(
        malformed_member_session.query_root_package_search("group-name"),
        PackageMetadataErrorCode::MalformedMetadata,
        "invalid exact group member source"));
    expect(
        stub::owned_list_free_call_count() == 1,
        "malformed group member result list was not freed");
}

void test_repository_root_search_free_function_uses_one_session() {
    set_repository_configuration_outputs("core\nextra\ndisabled\n");
    enqueue_repository_usage_output("'core'", "All\n");
    enqueue_repository_usage_output("'extra'", "Search\nInstall\n");
    enqueue_repository_usage_output("'disabled'", "Search\n");
    stub::reset_alpm_stub();
    stub::set_sync_database_register_failure("disabled");
    stub::set_repository_search_results(
        "extra", "terminal",
        {{"terminal-tool", "5.0-1", "Terminal utility"}});

    RepositoryPackageSearchResult result =
        query_repository_root_package_search("terminal");

    RepositoryPackageSearchSnapshot snapshot =
        require_result_alternative<RepositoryPackageSearchSnapshot>(
            result, "repository root search free function");
    expect(
        snapshot.repository_order ==
                std::vector<std::string>({"core", "extra"}) &&
            snapshot.matches.size() == 1 &&
            snapshot.matches[0].repository_name == "extra",
        "repository root search free function snapshot differs");
    expect(
        stub::initialize_call_count() == 1 &&
            stub::created_handle_count() == 1 &&
            stub::release_count_for_handle(0) == 1,
        "repository root search did not use one released session");
    const auto registrations = stub::sync_database_registration_history();
    expect(
        registrations.size() == 2 &&
            registrations[0].repository_name == "core" &&
            registrations[1].repository_name == "extra",
        "non-eligible repository reached the libalpm session");
}

void test_repository_exact_metadata_owns_authoritative_package_base() {
    {
        stub::reset_alpm_stub();
        stub::set_repository_package_metadata(
            "core", "ordinary-package", 10, 20);
        stub::set_repository_package_version(
            "core", "ordinary-package", "1.0-1");
        RepositoryPackageMetadataSession ordinary_session =
            RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core"}));
        RepositoryExactPackageMetadata ordinary =
            require_exact_repository_metadata(
                ordinary_session.query_repository_exact_package_metadata(
                    "ordinary-package"),
                "ordinary exact PackageBase");
        expect(
            ordinary.package_name == "ordinary-package" &&
                ordinary.package_base == "ordinary-package" &&
                ordinary.architecture == "x86_64",
            "Ordinary exact metadata lost its authoritative PackageBase or architecture");
    }

    {
        stub::reset_alpm_stub();
        stub::set_repository_package_metadata(
            "extra", "suite-child", 30, 40);
        stub::set_repository_package_version(
            "extra", "suite-child", "2.0-1");
        stub::set_repository_package_base(
            "extra", "suite-child", "suite");
        stub::set_repository_package_architecture(
            "extra", "suite-child", "any");
        RepositoryPackageMetadataSession split_session =
            RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"extra"}));
        RepositoryExactPackageMetadata split =
            require_exact_repository_metadata(
                split_session.query_repository_exact_package_metadata(
                    "suite-child"),
                "split exact PackageBase");
        expect(
            split.package_name == "suite-child" &&
                split.package_base == "suite" &&
                split.architecture == "any",
            "Split exact metadata flattened child, PackageBase, or architecture identity");
    }
}

void test_repository_exact_metadata_rejects_invalid_package_base() {
    const auto require_invalid_base = [](
                                          const std::string& package_name,
                                          const auto& configure_base,
                                          const std::string& context) {
        stub::reset_alpm_stub();
        stub::set_repository_package_metadata(
            "core", package_name, 10, 20);
        stub::set_repository_package_version(
            "core", package_name, "1.0-1");
        configure_base();
        RepositoryPackageMetadataSession session =
            RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core"}));
        PackageMetadataFailure failure =
            require_exact_repository_source_failure(
                session.query_repository_exact_package_metadata(
                    package_name),
                PackageMetadataErrorCode::MalformedMetadata,
                context);
        expect(!failure.diagnostic.empty(), context + ": empty diagnostic");
    };

    require_invalid_base(
        "null-base",
        []() {
            stub::set_repository_package_base_null(
                "core", "null-base");
        },
        "null PackageBase");
    require_invalid_base(
        "empty-base",
        []() {
            stub::set_repository_package_base(
                "core", "empty-base", "");
        },
        "empty PackageBase");
    require_invalid_base(
        "malformed-base",
        []() {
            stub::set_repository_package_base(
                "core", "malformed-base", "invalid/base");
        },
        "malformed PackageBase");
}

void test_repository_exact_metadata_rejects_invalid_architecture() {
    const auto require_invalid_architecture = [](
                                                  const std::string& package_name,
                                                  const auto& configure_architecture,
                                                  const std::string& context) {
        stub::reset_alpm_stub();
        stub::set_repository_package_metadata(
            "core", package_name, 10, 20);
        stub::set_repository_package_version(
            "core", package_name, "1.0-1");
        configure_architecture();
        RepositoryPackageMetadataSession session =
            RepositoryPackageMetadataSession::open(
                valid_repository_configuration({"core"}));
        const PackageMetadataFailure failure =
            require_exact_repository_source_failure(
                session.query_repository_exact_package_metadata(
                    package_name),
                PackageMetadataErrorCode::MalformedMetadata,
                context);
        expect(!failure.diagnostic.empty(), context + ": empty diagnostic");
    };

    require_invalid_architecture(
        "null-architecture",
        []() {
            stub::set_repository_package_architecture_null(
                "core", "null-architecture");
        },
        "null repository architecture");
    require_invalid_architecture(
        "empty-architecture",
        []() {
            stub::set_repository_package_architecture(
                "core", "empty-architecture", "");
        },
        "empty repository architecture");
    require_invalid_architecture(
        "malformed-architecture",
        []() {
            stub::set_repository_package_architecture(
                "core", "malformed-architecture",
                "invalid architecture");
        },
        "malformed repository architecture");
}

void test_repository_session_move_construction_does_not_double_release() {
    stub::reset_alpm_stub();
    {
        RepositoryPackageMetadataSession source = RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core"}));
        RepositoryPackageMetadataSession destination(std::move(source));
        static_cast<void>(destination);
        expect(stub::release_call_count() == 0, "sync move construction released handle early");
    }
    expect(stub::created_handle_count() == 1, "sync move construction created extra handle");
    expect(stub::release_count_for_handle(0) == 1, "sync moved handle release count differs");
}

void test_repository_session_move_assignment_does_not_double_release() {
    stub::reset_alpm_stub();
    {
        RepositoryPackageMetadataSession source = RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"core"}));
        RepositoryPackageMetadataSession destination = RepositoryPackageMetadataSession::open(
            valid_repository_configuration({"extra"}));

        destination = std::move(source);

        expect(stub::release_count_for_handle(0) == 0, "sync transferred handle released early");
        expect(stub::release_count_for_handle(1) == 1, "sync destination handle not released");
    }
    expect(stub::created_handle_count() == 2, "sync move assignment handle count differs");
    expect(stub::release_count_for_handle(0) == 1, "sync transferred handle release differs");
    expect(stub::release_count_for_handle(1) == 1, "sync old handle was double released");
}

} // namespace

int main() {
    try {
        test_pacman_conf_path_parse_success();
        test_pacman_conf_command_failure();
        test_root_dir_missing();
        test_database_path_missing();
        test_duplicate_root_dir();
        test_duplicate_database_path();
        test_empty_root_dir();
        test_empty_database_path();
        test_relative_root_dir();
        test_relative_database_path();
        test_trailing_slash_normalization();
        test_malformed_pacman_conf_line();
        test_leading_blank_pacman_conf_line();
        test_trailing_blank_pacman_conf_line();
        test_unexpected_pacman_conf_key_does_not_leak_output();
        test_repository_configuration_preserves_configured_order();
        test_empty_repository_configuration_is_valid();
        test_repository_configuration_rejects_duplicate_and_blank_lines();
        test_repository_configuration_rejects_control_characters();
        test_repository_configuration_command_failures_are_distinct();
        test_malformed_repository_output_does_not_leak_raw_text();
        test_root_search_repository_configuration_filters_usage_in_order();
        test_root_search_repository_usage_is_strict();
        test_root_search_repository_usage_command_is_quoted();
        test_root_search_usage_failure_precedes_alpm_open();

        test_session_open_success();
        test_session_rejects_relative_paths_before_initialize();
        test_session_rejects_embedded_null_path_before_initialize();
        test_alpm_initialization_failure();
        test_local_database_unavailable_releases_handle();
        test_invalid_local_database_releases_handle();
        test_package_cache_failure_releases_handle();
        test_empty_package_cache_is_queryable_state();
        test_local_package_version_snapshot_is_owned_and_order_independent();
        test_empty_local_package_version_snapshot_is_success();
        test_local_package_version_snapshot_rejects_invalid_names();
        test_local_package_version_snapshot_rejects_invalid_versions();
        test_local_package_version_snapshot_rejects_duplicate_names();
        test_local_package_version_snapshot_reports_invalid_cache_entry();
        test_moved_from_session_snapshot_reports_query_failure();
        test_installed_package_state_snapshot_is_owned_and_keeps_reasons();
        test_installed_package_state_snapshot_retains_incomplete_identity();
        test_installed_package_state_snapshot_rejects_malformed_metadata();
        test_installed_package_state_snapshot_failure_is_not_empty_inventory();
        test_empty_installed_package_state_snapshot_confirms_no_entries();
        test_installed_runtime_dependency_metadata_is_owned_and_canonical();
        test_installed_runtime_dependency_failure_is_not_empty_inventory();

        test_package_absent();
        test_package_present();
        test_installed_package_identity_states_are_lossless();
        test_returned_package_name_mismatch();
        test_null_package_name();
        test_null_package_version();
        test_empty_package_version();
        test_install_reason_mapping();
        test_invalid_package_name();
        test_query_failure_is_not_absence();
        test_null_query_without_error_is_not_absence();
        test_stale_query_error_does_not_override_found_package();
        test_stale_query_error_does_not_override_package_absence();

        test_move_construction_does_not_double_release();
        test_move_assignment_does_not_double_release();

        test_foreign_inventory_returns_initialization_failure_as_value();
        test_foreign_inventory_requires_configured_repository();
        test_empty_local_cache_is_empty_foreign_inventory();
        test_foreign_inventory_local_database_failure();
        test_foreign_inventory_local_cache_failure();
        test_foreign_inventory_sync_register_failure();
        test_foreign_inventory_sync_validation_failure();
        test_foreign_inventory_sync_cache_failure();
        test_foreign_inventory_accepts_empty_sync_cache();
        test_foreign_inventory_excludes_first_repository_native_package();
        test_foreign_inventory_excludes_later_repository_native_package();
        test_foreign_inventory_preserves_local_order_and_reasons();
        test_foreign_inventory_maps_future_reason_to_unknown();
        test_foreign_inventory_rejects_null_local_name();
        test_foreign_inventory_rejects_empty_local_name();
        test_foreign_inventory_rejects_invalid_local_name();
        test_foreign_inventory_rejects_null_foreign_version();
        test_foreign_inventory_rejects_empty_foreign_version();
        test_foreign_inventory_does_not_require_native_version();
        test_foreign_inventory_rejects_invalid_sync_returned_name();
        test_foreign_inventory_rejects_sync_returned_name_mismatch();
        test_foreign_inventory_rejects_null_sync_returned_name();
        test_foreign_inventory_distinguishes_absence_from_query_failure();
        test_foreign_inventory_ignores_stale_errno_on_successful_pointers();
        test_foreign_inventory_preloads_all_sync_caches_before_lookup();
        test_foreign_inventory_releases_handle_once_on_query_failure();
        test_foreign_inventory_returns_owned_values_after_release();

        test_repository_session_open_registers_in_configured_order();
        test_repository_session_accepts_empty_repository_list();
        test_repository_session_revalidates_manual_configuration();
        test_repository_register_failure_releases_all_state();
        test_repository_validation_failure_releases_all_state();
        test_repository_cache_failure_releases_all_state();
        test_empty_repository_cache_is_valid();
        test_repository_precedence_uses_first_configured_hit();
        test_repository_precedence_continues_after_explicit_miss();
        test_repository_precedence_stops_after_query_failure();
        test_all_repository_misses_return_not_found();
        test_exact_repository_lookup_does_not_use_precedence_fallback();
        test_exact_repository_miss_is_not_found_without_fallback();
        test_unconfigured_exact_repository_is_failure();
        test_repository_query_rejects_invalid_package_name();
        test_repository_query_success_ignores_stale_errno();
        test_repository_query_null_without_error_is_failure();
        test_repository_metadata_validates_returned_name();
        test_repository_metadata_accepts_positive_and_zero_sizes();
        test_repository_metadata_rejects_negative_sizes();
        test_repository_metadata_accepts_maximum_off_t();
        test_repository_root_search_returns_owned_matches_in_repository_order();
        test_repository_root_search_adds_exact_group_members_with_provenance();
        test_repository_root_search_failure_discards_partial_snapshot();
        test_repository_root_search_ignores_stale_errno_for_empty_group_result();
        test_repository_root_search_rejects_malformed_group_metadata();
        test_repository_root_search_free_function_uses_one_session();
        test_repository_exact_metadata_owns_authoritative_package_base();
        test_repository_exact_metadata_rejects_invalid_package_base();
        test_repository_exact_metadata_rejects_invalid_architecture();
        test_repository_session_move_construction_does_not_double_release();
        test_repository_session_move_assignment_does_not_double_release();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "package metadata tests: all checks passed\n";
    return 0;
}
