#include "alpm_stub.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

enum class AlpmStubDatabaseKind {
    Local,
    Sync,
};

struct _alpm_handle_t {
    std::size_t creation_index;
    alpm_errno_t error;
};

struct _alpm_db_t {
    alpm_handle_t* handle;
    AlpmStubDatabaseKind kind;
    std::string repository_name;
};

struct _alpm_pkg_t {
    alpm_handle_t* handle;
    AlpmStubDatabaseKind kind;
    std::string repository_name;
    std::string lookup_name;
    std::size_t local_package_index;
    bool has_handle_owned_local_metadata = false;
};

namespace {

enum class PackageLookupMode {
    Present,
    Absent,
    Failure,
    NullWithoutError
};

struct SyncDatabaseBehavior {
    bool register_fails = false;
    alpm_errno_t register_error = ALPM_ERR_DB_OPEN;
    bool should_preserve_register_error = false;
    alpm_errno_t preserved_register_error = ALPM_ERR_DB_OPEN;
    bool validation_fails = false;
    alpm_errno_t validation_error = ALPM_ERR_DB_INVALID;
    bool cache_fails = false;
    alpm_errno_t cache_error = ALPM_ERR_DB_OPEN;
    bool cache_empty = false;
    bool should_preserve_cache_error = false;
    alpm_errno_t preserved_cache_error = ALPM_ERR_DB_OPEN;
    std::size_t cache_calls = 0;
};

struct LocalPackageState {
    std::string name = "test-package";
    std::string version = "1.0-1";
    alpm_pkgreason_t reason = ALPM_PKG_REASON_EXPLICIT;
    bool name_is_null = false;
    bool version_is_null = false;
    std::vector<package_metadata_test_stub::RepositoryProvidedPackageMetadata>
        provides;
    std::vector<alpm_depend_t> provide_dependencies;
    std::vector<alpm_list_t> provide_nodes;
    std::vector<package_metadata_test_stub::PackageDependencyMetadata>
        dependencies;
    std::vector<alpm_depend_t> dependency_records;
    std::vector<alpm_list_t> dependency_nodes;
    std::string package_base = "test-package";
    std::string architecture = "x86_64";
    bool package_base_is_null = false;
    bool architecture_is_null = false;
};

struct LocalPackageQueryExpectation {
    std::string expected_package_name;
    PackageLookupMode lookup_mode;
    LocalPackageState returned_package;
    alpm_errno_t query_error = ALPM_ERR_DB_OPEN;
};

struct HandleOwnedLocalPackageRecord {
    LocalPackageState state;
    alpm_pkg_t package;
};

struct RepositoryPackageState {
    PackageLookupMode lookup_mode = PackageLookupMode::Absent;
    alpm_errno_t query_error = ALPM_ERR_DB_OPEN;
    std::string returned_name;
    std::string package_base;
    bool package_base_is_null = false;
    std::string version;
    bool version_is_null = false;
    std::string description;
    off_t package_size = 0;
    off_t installed_size = 0;
    bool name_is_null = false;
    bool should_preserve_query_error = false;
    alpm_errno_t preserved_query_error = ALPM_ERR_DB_OPEN;
    std::vector<package_metadata_test_stub::RepositoryProvidedPackageMetadata>
        provides;
    std::vector<alpm_depend_t> provide_dependencies;
    std::vector<alpm_list_t> provide_nodes;
    std::string architecture = "x86_64";
    bool architecture_is_null = false;
};

struct RepositorySearchBehavior {
    bool fails = false;
    alpm_errno_t error = ALPM_ERR_INVALID_REGEX;
    bool should_preserve_error = false;
    alpm_errno_t preserved_error = ALPM_ERR_DB_OPEN;
    std::vector<std::string> package_names;
};

struct RepositoryGroupBehavior {
    PackageLookupMode lookup_mode = PackageLookupMode::Absent;
    alpm_errno_t query_error = ALPM_ERR_DB_OPEN;
    std::string returned_name;
    bool has_custom_returned_name = false;
    bool append_null_member = false;
    std::vector<std::string> package_names;
};

struct GroupRecord {
    explicit GroupRecord(std::string group_name)
        : name(std::move(group_name)), group{name.data(), nullptr} {
    }

    std::string name;
    alpm_group_t group;
};

struct SyncDatabaseRecord {
    SyncDatabaseRecord(alpm_handle_t* handle, const std::string& repository_name)
        : database{handle, AlpmStubDatabaseKind::Sync, repository_name} {
    }

    alpm_db_t database;
    std::map<std::string, std::unique_ptr<alpm_pkg_t>> packages;
    std::vector<std::unique_ptr<alpm_list_t>> cache_nodes;
    std::map<std::string, std::unique_ptr<GroupRecord>> groups;
};

struct HandleRecord {
    HandleRecord(std::size_t creation_index, std::size_t local_package_count)
        : handle{creation_index, ALPM_ERR_OK},
          database{&handle, AlpmStubDatabaseKind::Local, ""} {
        local_packages.reserve(local_package_count);
        local_cache_nodes.reserve(local_package_count);
        for(std::size_t index = 0; index < local_package_count; ++index) {
            local_packages.push_back(std::make_unique<alpm_pkg_t>(alpm_pkg_t{
                &handle,
                AlpmStubDatabaseKind::Local,
                "",
                "",
                index}));
            local_cache_nodes.push_back(std::make_unique<alpm_list_t>(alpm_list_t{
                local_packages.back().get(), nullptr, nullptr}));
        }
        for(std::size_t index = 0; index < local_cache_nodes.size(); ++index) {
            local_cache_nodes[index]->prev =
                index == 0 ? nullptr : local_cache_nodes[index - 1].get();
            local_cache_nodes[index]->next =
                index + 1 == local_cache_nodes.size()
                    ? nullptr
                    : local_cache_nodes[index + 1].get();
        }
    }

    alpm_handle_t handle;
    alpm_db_t database;
    std::size_t release_count = 0;
    std::vector<std::unique_ptr<alpm_pkg_t>> local_packages;
    std::vector<std::unique_ptr<alpm_list_t>> local_cache_nodes;
    std::vector<std::unique_ptr<HandleOwnedLocalPackageRecord>>
        strict_local_packages;
    std::vector<std::unique_ptr<SyncDatabaseRecord>> sync_databases;
};

struct AlpmStubState {
    bool initialize_fails = false;
    alpm_errno_t initialize_error = ALPM_ERR_SYSTEM;

    bool local_database_available = true;
    alpm_errno_t local_database_error = ALPM_ERR_DB_NULL;
    bool local_database_valid = true;
    alpm_errno_t local_database_valid_error = ALPM_ERR_DB_INVALID;

    bool package_cache_fails = false;
    bool package_cache_empty = false;
    alpm_errno_t package_cache_error = ALPM_ERR_DB_OPEN;
    bool should_preserve_package_cache_error = false;
    alpm_errno_t preserved_package_cache_error = ALPM_ERR_DB_OPEN;

    bool should_preserve_local_database_error = false;
    alpm_errno_t preserved_local_database_error = ALPM_ERR_DB_OPEN;

    PackageLookupMode package_lookup_mode = PackageLookupMode::Present;
    alpm_errno_t package_query_error = ALPM_ERR_SYSTEM;
    bool should_preserve_package_query_error = false;
    std::vector<LocalPackageState> local_packages = {LocalPackageState{}};

    bool local_package_query_strict_mode = false;
    std::deque<LocalPackageQueryExpectation> local_package_query_expectations;
    std::vector<std::string> local_package_queries;
    const char* local_package_query_expectation_failure = nullptr;

    std::size_t initialize_calls = 0;
    std::size_t local_database_calls = 0;
    std::size_t database_valid_calls = 0;
    std::size_t package_cache_calls = 0;
    std::size_t package_query_calls = 0;

    std::string initialize_root;
    std::string initialize_database_path;
    std::string queried_package_name;

    std::map<std::string, SyncDatabaseBehavior> sync_database_behaviors;
    std::map<std::pair<std::string, std::string>, RepositoryPackageState>
        repository_packages;
    std::map<std::pair<std::string, std::string>, RepositorySearchBehavior>
        repository_search_behaviors;
    std::map<std::pair<std::string, std::string>, RepositoryGroupBehavior>
        repository_group_behaviors;
    std::size_t owned_list_free_calls = 0;
    std::size_t group_expansion_calls = 0;
    std::vector<package_metadata_test_stub::SyncDatabaseRegistration>
        sync_database_registrations;
    std::vector<std::string> sync_database_operations;
    std::vector<package_metadata_test_stub::RepositoryPackageQuery>
        repository_package_queries;
    std::vector<package_metadata_test_stub::RepositorySearchQuery>
        repository_search_queries;
    std::vector<package_metadata_test_stub::RepositoryGroupQuery>
        repository_group_queries;

    std::vector<std::unique_ptr<HandleRecord>> handles;
};

AlpmStubState g_state;

HandleRecord* record_for_handle(alpm_handle_t* handle);
void set_handle_error(alpm_handle_t* handle, alpm_errno_t error);

LocalPackageState& primary_local_package_state() {
    if(g_state.local_packages.empty()) {
        g_state.local_packages.push_back(LocalPackageState{});
    }
    return g_state.local_packages.front();
}

LocalPackageState* local_package_state(alpm_pkg_t* package) {
    if(package == nullptr || package->kind != AlpmStubDatabaseKind::Local) return nullptr;
    if(package->has_handle_owned_local_metadata) {
        HandleRecord* record = record_for_handle(package->handle);
        if(record == nullptr ||
           package->local_package_index >=
               record->strict_local_packages.size()) {
            return nullptr;
        }
        return &record->strict_local_packages[package->local_package_index]->state;
    }
    if(package->local_package_index >= g_state.local_packages.size()) return nullptr;
    return &g_state.local_packages[package->local_package_index];
}

[[noreturn]] void fail_local_package_query_expectation(
    const char* diagnostic) {
    // POLICY: fixed diagnosticだけを保持し、package-controlled nameを埋め込まない。
    g_state.local_package_query_expectation_failure = diagnostic;
    throw std::logic_error(diagnostic);
}

void rebuild_local_provides(LocalPackageState& package_state);
void rebuild_local_dependencies(LocalPackageState& package_state);

alpm_pkg_t* consume_local_package_query_expectation(
    alpm_db_t* database,
    const std::string& package_name) {
    if(g_state.local_package_query_expectation_failure != nullptr) {
        fail_local_package_query_expectation(
            g_state.local_package_query_expectation_failure);
    }
    if(g_state.local_package_query_expectations.empty()) {
        fail_local_package_query_expectation(
            "Package metadata stub received an unexpected local package query "
            "with no pending expectation.");
    }

    const LocalPackageQueryExpectation& expectation =
        g_state.local_package_query_expectations.front();
    if(package_name != expectation.expected_package_name) {
        fail_local_package_query_expectation(
            "Package metadata stub local package query did not match the next "
            "expectation.");
    }

    switch(expectation.lookup_mode) {
        case PackageLookupMode::Present: {
            HandleRecord* record = record_for_handle(database->handle);
            if(record == nullptr) {
                set_handle_error(database->handle, ALPM_ERR_HANDLE_NULL);
                return nullptr;
            }

            const std::size_t package_index =
                record->strict_local_packages.size();
            auto package = std::make_unique<HandleOwnedLocalPackageRecord>(
                HandleOwnedLocalPackageRecord{
                    expectation.returned_package,
                    alpm_pkg_t{
                        database->handle,
                        AlpmStubDatabaseKind::Local,
                        "",
                        "",
                        package_index,
                        true}});
            record->strict_local_packages.push_back(std::move(package));
            rebuild_local_provides(
                record->strict_local_packages.back()->state);
            rebuild_local_dependencies(
                record->strict_local_packages.back()->state);
            g_state.local_package_query_expectations.pop_front();
            return &record->strict_local_packages.back()->package;
        }
        case PackageLookupMode::Absent:
            g_state.local_package_query_expectations.pop_front();
            set_handle_error(database->handle, ALPM_ERR_PKG_NOT_FOUND);
            return nullptr;
        case PackageLookupMode::Failure: {
            const alpm_errno_t query_error = expectation.query_error;
            g_state.local_package_query_expectations.pop_front();
            set_handle_error(database->handle, query_error);
            return nullptr;
        }
        case PackageLookupMode::NullWithoutError:
            g_state.local_package_query_expectations.pop_front();
            return nullptr;
    }
    fail_local_package_query_expectation(
        "Local package query expectation has an unknown lookup mode.");
}

void append_alpm_event(
    const char* event,
    const char* detail = nullptr) noexcept {
    const char* event_log_path =
        std::getenv("MOGUET_TEST_PACKAGE_METADATA_EVENT_LOG");
    if(event_log_path == nullptr || event_log_path[0] == '\0') return;

    // POLICY: alpm_release()はnoexcept destructorから呼ばれるため、test観測もC stdioで閉じる。
    std::FILE* event_log = std::fopen(event_log_path, "a");
    if(event_log == nullptr) return;
    if(detail == nullptr) {
        static_cast<void>(std::fprintf(event_log, "%s\n", event));
    } else {
        static_cast<void>(std::fprintf(event_log, "%s %s\n", event, detail));
    }
    static_cast<void>(std::fclose(event_log));
}

bool environment_flag_is_enabled(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

bool environment_ordinal_matches(
    const char* name, std::size_t actual_ordinal) noexcept {
    const char* expected_ordinal_text = std::getenv(name);
    if(expected_ordinal_text == nullptr || expected_ordinal_text[0] == '\0') return false;

    char* ordinal_end = nullptr;
    errno = 0;
    unsigned long long expected_ordinal =
        std::strtoull(expected_ordinal_text, &ordinal_end, 10);
    if(errno != 0 || ordinal_end == expected_ordinal_text || ordinal_end[0] != '\0') {
        return false;
    }
    return expected_ordinal == static_cast<unsigned long long>(actual_ordinal);
}

bool environment_requests_query_failure(const char* package_name) noexcept {
    const char* failure_package =
        std::getenv("MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_PACKAGE");
    if(failure_package != nullptr && package_name != nullptr &&
       std::strcmp(failure_package, package_name) == 0) {
        return true;
    }

    return environment_ordinal_matches(
        "MOGUET_TEST_PACKAGE_METADATA_QUERY_FAILURE_AT",
        g_state.package_query_calls);
}

bool environment_requests_unknown_reason(const char* package_name) noexcept {
    const char* unknown_reason_package =
        std::getenv("MOGUET_TEST_PACKAGE_METADATA_UNKNOWN_REASON_PACKAGE");
    return unknown_reason_package != nullptr && package_name != nullptr &&
           std::strcmp(unknown_reason_package, package_name) == 0;
}

void configure_foreign_inventory_from_environment() {
    const char* state_file_path =
        std::getenv("MOGUET_TEST_FOREIGN_PACKAGE_INVENTORY_STATE_FILE");
    if(state_file_path == nullptr) return;

    std::ifstream state_file(state_file_path);
    if(!state_file) {
        g_state.local_packages.clear();
        g_state.package_cache_fails = true;
        g_state.package_cache_error = ALPM_ERR_DB_OPEN;
        return;
    }

    std::vector<LocalPackageState> packages;
    std::string line;
    while(std::getline(state_file, line)) {
        if(line.empty()) continue;

        std::istringstream fields(line);
        std::string package_name;
        std::string package_version;
        std::string reason_text;
        std::string unexpected_field;
        if(!(fields >> package_name >> package_version) ||
           ((fields >> reason_text) && (fields >> unexpected_field))) {
            g_state.local_packages.clear();
            g_state.package_cache_fails = true;
            g_state.package_cache_error = ALPM_ERR_DB_OPEN;
            return;
        }

        alpm_pkgreason_t reason = ALPM_PKG_REASON_EXPLICIT;
        if(!reason_text.empty()) {
            if(reason_text == "explicit") {
                reason = ALPM_PKG_REASON_EXPLICIT;
            } else if(reason_text == "dependency") {
                reason = ALPM_PKG_REASON_DEPEND;
            } else if(reason_text == "unknown") {
                reason = ALPM_PKG_REASON_UNKNOWN;
            } else {
                g_state.local_packages.clear();
                g_state.package_cache_fails = true;
                g_state.package_cache_error = ALPM_ERR_DB_OPEN;
                return;
            }
        }

        packages.push_back(LocalPackageState{
            package_name,
            std::move(package_version),
            reason,
            false,
            false,
            {},
            {},
            {},
            {},
            {},
            {},
            std::move(package_name),
            "x86_64",
            false,
            false});
    }

    if(state_file.bad()) {
        g_state.local_packages.clear();
        g_state.package_cache_fails = true;
        g_state.package_cache_error = ALPM_ERR_DB_OPEN;
        return;
    }

    g_state.local_packages = std::move(packages);
    g_state.package_cache_empty = false;
    g_state.package_cache_fails = false;
}

void configure_package_lookup_from_environment(
    const char* queried_package_name,
    PackageLookupMode& lookup_mode,
    alpm_errno_t& query_error) {
    if(environment_requests_query_failure(queried_package_name)) {
        lookup_mode = PackageLookupMode::Failure;
        query_error = ALPM_ERR_DB_OPEN;
        return;
    }

    const char* state_file_path =
        std::getenv("MOGUET_TEST_PACKAGE_METADATA_STATE_FILE");
    if(state_file_path == nullptr) return;

    std::ifstream state_file(state_file_path);
    if(!state_file) {
        lookup_mode = PackageLookupMode::Failure;
        query_error = ALPM_ERR_DB_OPEN;
        return;
    }

    std::string package_name;
    std::string package_version;
    while(state_file >> package_name >> package_version) {
        if(package_name != queried_package_name) continue;

        lookup_mode = PackageLookupMode::Present;
        LocalPackageState& package = primary_local_package_state();
        package.name = std::move(package_name);
        package.package_base = package.name;
        package.version = std::move(package_version);
        package.reason =
            environment_requests_unknown_reason(queried_package_name)
                ? ALPM_PKG_REASON_UNKNOWN
                : ALPM_PKG_REASON_EXPLICIT;
        package.name_is_null = false;
        package.version_is_null = false;
        return;
    }

    if(state_file.bad()) {
        lookup_mode = PackageLookupMode::Failure;
        query_error = ALPM_ERR_DB_OPEN;
        return;
    }
    lookup_mode = PackageLookupMode::Absent;
}

void configure_repository_package_from_environment(
    const std::string& repository_name,
    const std::string& package_name,
    RepositoryPackageState& package_state) {
    const char* state_file_path =
        std::getenv("MOGUET_TEST_REPOSITORY_METADATA_STATE_FILE");
    if(state_file_path == nullptr) return;

    std::ifstream state_file(state_file_path);
    if(!state_file) {
        package_state.lookup_mode = PackageLookupMode::Failure;
        package_state.query_error = ALPM_ERR_DB_OPEN;
        return;
    }

    package_state.lookup_mode = PackageLookupMode::Absent;
    std::string line;
    while(std::getline(state_file, line)) {
        if(line.empty()) continue;

        std::istringstream fields(line);
        std::string fixture_repository;
        std::string fixture_package;
        off_t package_size = 0;
        off_t installed_size = 0;
        std::string fixture_package_base;
        std::string unexpected_field;
        if(!(fields >> fixture_repository >> fixture_package >> package_size >>
             installed_size) ||
           ((fields >> fixture_package_base) &&
            (fields >> unexpected_field))) {
            package_state.lookup_mode = PackageLookupMode::Failure;
            package_state.query_error = ALPM_ERR_DB_OPEN;
            return;
        }
        if(fixture_repository != repository_name || fixture_package != package_name) continue;

        package_state.lookup_mode = PackageLookupMode::Present;
        package_state.returned_name = fixture_package;
        package_state.package_base = fixture_package_base.empty()
                                         ? fixture_package
                                         : fixture_package_base;
        package_state.package_base_is_null = false;
        package_state.package_size = package_size;
        package_state.installed_size = installed_size;
        package_state.name_is_null = false;
        return;
    }

    if(state_file.bad()) {
        package_state.lookup_mode = PackageLookupMode::Failure;
        package_state.query_error = ALPM_ERR_DB_OPEN;
    }
}

HandleRecord* record_for_handle(alpm_handle_t* handle) {
    if(handle == nullptr || handle->creation_index >= g_state.handles.size()) return nullptr;
    HandleRecord* record = g_state.handles[handle->creation_index].get();
    if(&record->handle != handle) return nullptr;
    return record;
}

SyncDatabaseRecord* sync_database_record(alpm_db_t* database) {
    if(database == nullptr || database->kind != AlpmStubDatabaseKind::Sync) return nullptr;
    HandleRecord* handle_record = record_for_handle(database->handle);
    if(handle_record == nullptr) return nullptr;
    for(const auto& sync_database : handle_record->sync_databases) {
        if(&sync_database->database == database) return sync_database.get();
    }
    return nullptr;
}

RepositoryPackageState& repository_package_state(
    const std::string& repository_name,
    const std::string& package_name) {
    return g_state.repository_packages[{repository_name, package_name}];
}

void rebuild_repository_provides(RepositoryPackageState& package_state) {
    package_state.provide_dependencies.clear();
    package_state.provide_dependencies.reserve(package_state.provides.size());
    for(auto& provided : package_state.provides) {
        package_state.provide_dependencies.push_back(alpm_depend_t{
            provided.package_name.has_value()
                ? provided.package_name->data()
                : nullptr,
            provided.version.has_value()
                ? provided.version->data()
                : nullptr,
            nullptr,
            0,
            provided.relation});
    }

    package_state.provide_nodes.resize(
        package_state.provide_dependencies.size());
    for(std::size_t index = 0;
        index < package_state.provide_nodes.size();
        ++index) {
        package_state.provide_nodes[index] = alpm_list_t{
            &package_state.provide_dependencies[index],
            index == 0
                ? nullptr
                : &package_state.provide_nodes[index - 1],
            index + 1 == package_state.provide_nodes.size()
                ? nullptr
                : &package_state.provide_nodes[index + 1]};
    }
}

void rebuild_local_provides(LocalPackageState& package_state) {
    package_state.provide_dependencies.clear();
    package_state.provide_dependencies.reserve(package_state.provides.size());
    for(auto& provided : package_state.provides) {
        package_state.provide_dependencies.push_back(alpm_depend_t{
            provided.package_name.has_value()
                ? provided.package_name->data()
                : nullptr,
            provided.version.has_value()
                ? provided.version->data()
                : nullptr,
            nullptr,
            0,
            provided.relation});
    }

    package_state.provide_nodes.resize(
        package_state.provide_dependencies.size());
    for(std::size_t index = 0;
        index < package_state.provide_nodes.size();
        ++index) {
        package_state.provide_nodes[index] = alpm_list_t{
            &package_state.provide_dependencies[index],
            index == 0
                ? nullptr
                : &package_state.provide_nodes[index - 1],
            index + 1 == package_state.provide_nodes.size()
                ? nullptr
                : &package_state.provide_nodes[index + 1]};
    }
}

void rebuild_local_dependencies(LocalPackageState& package_state) {
    package_state.dependency_records.clear();
    package_state.dependency_records.reserve(
        package_state.dependencies.size());
    for(auto& dependency : package_state.dependencies) {
        package_state.dependency_records.push_back(alpm_depend_t{
            dependency.package_name.has_value()
                ? dependency.package_name->data()
                : nullptr,
            dependency.version.has_value()
                ? dependency.version->data()
                : nullptr,
            nullptr,
            0,
            dependency.relation});
    }

    package_state.dependency_nodes.resize(
        package_state.dependency_records.size());
    for(std::size_t index = 0;
        index < package_state.dependency_nodes.size();
        ++index) {
        package_state.dependency_nodes[index] = alpm_list_t{
            &package_state.dependency_records[index],
            index == 0
                ? nullptr
                : &package_state.dependency_nodes[index - 1],
            index + 1 == package_state.dependency_nodes.size()
                ? nullptr
                : &package_state.dependency_nodes[index + 1]};
    }
}

RepositoryPackageState* repository_package_state(alpm_pkg_t* package) {
    if(package == nullptr || package->kind != AlpmStubDatabaseKind::Sync) return nullptr;
    auto package_state = g_state.repository_packages.find(
        {package->repository_name, package->lookup_name});
    if(package_state == g_state.repository_packages.end()) return nullptr;
    return &package_state->second;
}

alpm_pkg_t* sync_package_for(
    alpm_db_t* database,
    const std::string& package_name) {
    SyncDatabaseRecord* database_record = sync_database_record(database);
    if(database_record == nullptr) return nullptr;

    auto& package = database_record->packages[package_name];
    if(package == nullptr) {
        package = std::make_unique<alpm_pkg_t>(alpm_pkg_t{
            database->handle,
            AlpmStubDatabaseKind::Sync,
            database->repository_name,
            package_name,
            0});
    }
    return package.get();
}

alpm_list_t* make_owned_list(const std::vector<void*>& values) {
    alpm_list_t* first = nullptr;
    alpm_list_t* previous = nullptr;
    try {
        for(void* value : values) {
            auto* node = new alpm_list_t{value, previous, nullptr};
            if(previous == nullptr) {
                first = node;
            } else {
                previous->next = node;
            }
            previous = node;
        }
    } catch(...) {
        while(first != nullptr) {
            alpm_list_t* next = first->next;
            delete first;
            first = next;
        }
        throw;
    }
    return first;
}

void set_handle_error(alpm_handle_t* handle, alpm_errno_t error) {
    if(handle != nullptr) handle->error = error;
}

} // namespace

namespace package_metadata_test_stub {

void reset_alpm_stub() {
    g_state = AlpmStubState{};
}

void set_initialize_failure(alpm_errno_t error) {
    g_state.initialize_fails = true;
    g_state.initialize_error = error;
}

void set_local_database_unavailable(alpm_errno_t error) {
    g_state.local_database_available = false;
    g_state.local_database_error = error;
}

void set_local_database_invalid(alpm_errno_t error) {
    g_state.local_database_valid = false;
    g_state.local_database_valid_error = error;
}

void set_package_cache_failure(alpm_errno_t error) {
    g_state.package_cache_fails = true;
    g_state.package_cache_error = error;
}

void set_empty_package_cache() {
    g_state.package_cache_empty = true;
}

void set_package_absent() {
    g_state.package_lookup_mode = PackageLookupMode::Absent;
}

void set_package_query_failure(alpm_errno_t error) {
    g_state.package_lookup_mode = PackageLookupMode::Failure;
    g_state.package_query_error = error;
}

void set_package_query_null_without_error() {
    g_state.package_lookup_mode = PackageLookupMode::NullWithoutError;
}

void preserve_error_on_next_package_query() {
    g_state.should_preserve_package_query_error = true;
}

void set_package_metadata(
    const std::string& name, const std::string& version,
    alpm_pkgreason_t reason) {
    g_state.package_lookup_mode = PackageLookupMode::Present;
    LocalPackageState& package = primary_local_package_state();
    package.name = name;
    package.package_base = name;
    package.version = version;
    package.reason = reason;
    package.name_is_null = false;
    package.version_is_null = false;
}

void enqueue_local_package_query_present(
    std::string expected_package_name,
    std::string returned_name,
    std::string version,
    alpm_pkgreason_t reason) {
    g_state.local_package_query_expectations.push_back(
        LocalPackageQueryExpectation{
            std::move(expected_package_name),
            PackageLookupMode::Present,
            LocalPackageState{
                returned_name,
                std::move(version),
                reason,
                false,
                false,
                {},
                {},
                {},
                {},
                {},
                {},
                std::move(returned_name),
                "x86_64",
                false,
                false},
            ALPM_ERR_OK});
    g_state.local_package_query_strict_mode = true;
}

void enqueue_local_package_query_present_metadata(
    std::string expected_package_name,
    LocalPackageMetadata returned_package) {
    g_state.local_package_query_expectations.push_back(
        LocalPackageQueryExpectation{
            std::move(expected_package_name),
            PackageLookupMode::Present,
            LocalPackageState{
                returned_package.name,
                returned_package.version,
                returned_package.reason,
                false,
                false,
                std::move(returned_package.provides),
                {},
                {},
                std::move(returned_package.dependencies),
                {},
                {},
                returned_package.name,
                "x86_64",
                false,
                false},
            ALPM_ERR_OK});
    g_state.local_package_query_strict_mode = true;
}

void enqueue_local_package_query_absent(std::string expected_package_name) {
    g_state.local_package_query_expectations.push_back(
        LocalPackageQueryExpectation{
            std::move(expected_package_name),
            PackageLookupMode::Absent,
            LocalPackageState{},
            ALPM_ERR_PKG_NOT_FOUND});
    g_state.local_package_query_strict_mode = true;
}

void enqueue_local_package_query_failure(
    std::string expected_package_name,
    alpm_errno_t error) {
    g_state.local_package_query_expectations.push_back(
        LocalPackageQueryExpectation{
            std::move(expected_package_name),
            PackageLookupMode::Failure,
            LocalPackageState{},
            error});
    g_state.local_package_query_strict_mode = true;
}

void require_local_package_query_expectations_consumed() {
    if(g_state.local_package_query_expectation_failure != nullptr) {
        throw std::logic_error(
            g_state.local_package_query_expectation_failure);
    }
    if(!g_state.local_package_query_expectations.empty()) {
        throw std::logic_error(
            "Package metadata stub has unconsumed local package query expectations.");
    }
}

void set_local_packages(const std::vector<LocalPackageMetadata>& packages) {
    g_state.package_lookup_mode = PackageLookupMode::Present;
    g_state.package_cache_empty = false;
    g_state.local_packages.clear();
    g_state.local_packages.reserve(packages.size());
    for(const LocalPackageMetadata& package : packages) {
        g_state.local_packages.push_back(LocalPackageState{
            package.name,
            package.version,
            package.reason,
            false,
            false,
            package.provides,
            {},
            {},
            package.dependencies,
            {},
            {},
            package.name,
            "x86_64",
            false,
            false});
        rebuild_local_provides(g_state.local_packages.back());
        rebuild_local_dependencies(g_state.local_packages.back());
    }
}

void set_local_package_provides(
    std::size_t package_index,
    const std::vector<RepositoryProvidedPackageMetadata>& provides) {
    if(package_index >= g_state.local_packages.size()) return;
    LocalPackageState& package = g_state.local_packages[package_index];
    package.provides = provides;
    rebuild_local_provides(package);
}

void set_local_package_dependencies(
    std::size_t package_index,
    const std::vector<PackageDependencyMetadata>& dependencies) {
    if(package_index >= g_state.local_packages.size()) return;
    LocalPackageState& package = g_state.local_packages[package_index];
    package.dependencies = dependencies;
    rebuild_local_dependencies(package);
}

void set_local_package_cache_entry_null(std::size_t package_index) {
    if(g_state.handles.empty()) return;
    auto& cache_nodes = g_state.handles.back()->local_cache_nodes;
    if(package_index >= cache_nodes.size()) return;
    cache_nodes[package_index]->data = nullptr;
}

void set_local_package_name_null(std::size_t package_index) {
    if(package_index >= g_state.local_packages.size()) return;
    g_state.local_packages[package_index].name_is_null = true;
}

void set_local_package_version_null(std::size_t package_index) {
    if(package_index >= g_state.local_packages.size()) return;
    g_state.local_packages[package_index].version_is_null = true;
}

void set_local_package_base(
    std::size_t package_index,
    const std::string& package_base) {
    if(package_index >= g_state.local_packages.size()) return;
    LocalPackageState& package = g_state.local_packages[package_index];
    package.package_base = package_base;
    package.package_base_is_null = false;
}

void set_local_package_base_null(std::size_t package_index) {
    if(package_index >= g_state.local_packages.size()) return;
    g_state.local_packages[package_index].package_base_is_null = true;
}

void set_local_package_architecture(
    std::size_t package_index,
    const std::string& architecture) {
    if(package_index >= g_state.local_packages.size()) return;
    LocalPackageState& package = g_state.local_packages[package_index];
    package.architecture = architecture;
    package.architecture_is_null = false;
}

void set_local_package_architecture_null(std::size_t package_index) {
    if(package_index >= g_state.local_packages.size()) return;
    g_state.local_packages[package_index].architecture_is_null = true;
}

void set_null_package_name() {
    g_state.package_lookup_mode = PackageLookupMode::Present;
    primary_local_package_state().name_is_null = true;
}

void set_null_package_version() {
    g_state.package_lookup_mode = PackageLookupMode::Present;
    primary_local_package_state().version_is_null = true;
}

void preserve_error_on_next_local_database(alpm_errno_t stale_error) {
    g_state.should_preserve_local_database_error = true;
    g_state.preserved_local_database_error = stale_error;
}

void preserve_error_on_next_package_cache(alpm_errno_t stale_error) {
    g_state.should_preserve_package_cache_error = true;
    g_state.preserved_package_cache_error = stale_error;
}

void set_sync_database_register_failure(
    const std::string& repository_name,
    alpm_errno_t error) {
    SyncDatabaseBehavior& behavior =
        g_state.sync_database_behaviors[repository_name];
    behavior.register_fails = true;
    behavior.register_error = error;
}

void set_sync_database_validation_failure(
    const std::string& repository_name,
    alpm_errno_t error) {
    SyncDatabaseBehavior& behavior =
        g_state.sync_database_behaviors[repository_name];
    behavior.validation_fails = true;
    behavior.validation_error = error;
}

void set_sync_database_cache_failure(
    const std::string& repository_name,
    alpm_errno_t error) {
    SyncDatabaseBehavior& behavior =
        g_state.sync_database_behaviors[repository_name];
    behavior.cache_fails = true;
    behavior.cache_error = error;
}

void set_sync_database_empty_cache(const std::string& repository_name) {
    g_state.sync_database_behaviors[repository_name].cache_empty = true;
}

void preserve_error_on_next_sync_database_registration(
    const std::string& repository_name,
    alpm_errno_t stale_error) {
    SyncDatabaseBehavior& behavior =
        g_state.sync_database_behaviors[repository_name];
    behavior.should_preserve_register_error = true;
    behavior.preserved_register_error = stale_error;
}

void preserve_error_on_next_sync_database_cache(
    const std::string& repository_name,
    alpm_errno_t stale_error) {
    SyncDatabaseBehavior& behavior =
        g_state.sync_database_behaviors[repository_name];
    behavior.should_preserve_cache_error = true;
    behavior.preserved_cache_error = stale_error;
}

void set_repository_package_absent(
    const std::string& repository_name,
    const std::string& package_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state = RepositoryPackageState{};
}

void set_repository_package_query_failure(
    const std::string& repository_name,
    const std::string& package_name,
    alpm_errno_t error) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state = RepositoryPackageState{};
    package_state.lookup_mode = PackageLookupMode::Failure;
    package_state.query_error = error;
}

void set_repository_package_query_null_without_error(
    const std::string& repository_name,
    const std::string& package_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state = RepositoryPackageState{};
    package_state.lookup_mode = PackageLookupMode::NullWithoutError;
}

void set_repository_package_metadata(
    const std::string& repository_name,
    const std::string& package_name,
    off_t package_size,
    off_t installed_size) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state = RepositoryPackageState{};
    package_state.lookup_mode = PackageLookupMode::Present;
    package_state.returned_name = package_name;
    package_state.package_base = package_name;
    package_state.package_size = package_size;
    package_state.installed_size = installed_size;
}

void set_repository_package_version(
    const std::string& repository_name,
    const std::string& package_name,
    const std::string& version) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.version = version;
    package_state.version_is_null = false;
}

void set_repository_package_version_null(
    const std::string& repository_name,
    const std::string& package_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.version.clear();
    package_state.version_is_null = true;
}

void set_repository_package_base(
    const std::string& repository_name,
    const std::string& package_name,
    const std::string& package_base) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.package_base = package_base;
    package_state.package_base_is_null = false;
}

void set_repository_package_base_null(
    const std::string& repository_name,
    const std::string& package_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.package_base.clear();
    package_state.package_base_is_null = true;
}

void set_repository_package_architecture(
    const std::string& repository_name,
    const std::string& package_name,
    const std::string& architecture) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.architecture = architecture;
    package_state.architecture_is_null = false;
}

void set_repository_package_architecture_null(
    const std::string& repository_name,
    const std::string& package_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.architecture.clear();
    package_state.architecture_is_null = true;
}

void set_repository_package_provides(
    const std::string& repository_name,
    const std::string& package_name,
    const std::vector<RepositoryProvidedPackageMetadata>& provides) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.provides = provides;
    rebuild_repository_provides(package_state);
}

void set_repository_package_returned_name(
    const std::string& repository_name,
    const std::string& package_name,
    const std::string& returned_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.lookup_mode = PackageLookupMode::Present;
    package_state.returned_name = returned_name;
    package_state.name_is_null = false;
}

void set_repository_package_name_null(
    const std::string& repository_name,
    const std::string& package_name) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.lookup_mode = PackageLookupMode::Present;
    package_state.name_is_null = true;
}

void preserve_error_on_next_repository_package_query(
    const std::string& repository_name,
    const std::string& package_name,
    alpm_errno_t stale_error) {
    RepositoryPackageState& package_state =
        repository_package_state(repository_name, package_name);
    package_state.should_preserve_query_error = true;
    package_state.preserved_query_error = stale_error;
}

void set_repository_search_results(
    const std::string& repository_name,
    const std::string& query,
    const std::vector<RepositorySearchPackageMetadata>& packages) {
    RepositorySearchBehavior& behavior =
        g_state.repository_search_behaviors[{repository_name, query}];
    behavior.fails = false;
    behavior.package_names.clear();
    for(const auto& package : packages) {
        behavior.package_names.push_back(package.name);
        RepositoryPackageState& package_state =
            repository_package_state(repository_name, package.name);
        package_state.lookup_mode = PackageLookupMode::Present;
        package_state.returned_name = package.name;
        package_state.version = package.version;
        package_state.description = package.description;
        package_state.name_is_null = false;
    }
}

void set_repository_search_failure(
    const std::string& repository_name,
    const std::string& query,
    alpm_errno_t error) {
    RepositorySearchBehavior& behavior =
        g_state.repository_search_behaviors[{repository_name, query}];
    behavior.fails = true;
    behavior.error = error;
}

void preserve_error_on_next_repository_search(
    const std::string& repository_name,
    const std::string& query,
    alpm_errno_t stale_error) {
    RepositorySearchBehavior& behavior =
        g_state.repository_search_behaviors[{repository_name, query}];
    behavior.should_preserve_error = true;
    behavior.preserved_error = stale_error;
}

void set_repository_exact_group(
    const std::string& repository_name,
    const std::string& group_name,
    const std::vector<RepositorySearchPackageMetadata>& packages) {
    RepositoryGroupBehavior& behavior =
        g_state.repository_group_behaviors[{repository_name, group_name}];
    behavior.lookup_mode = PackageLookupMode::Present;
    behavior.has_custom_returned_name = false;
    behavior.append_null_member = false;
    behavior.package_names.clear();
    for(const auto& package : packages) {
        behavior.package_names.push_back(package.name);
        RepositoryPackageState& package_state =
            repository_package_state(repository_name, package.name);
        package_state.lookup_mode = PackageLookupMode::Present;
        package_state.returned_name = package.name;
        package_state.version = package.version;
        package_state.description = package.description;
        package_state.name_is_null = false;
    }
}

void set_repository_group_returned_name(
    const std::string& repository_name,
    const std::string& group_name,
    const std::string& returned_name) {
    RepositoryGroupBehavior& behavior =
        g_state.repository_group_behaviors[{repository_name, group_name}];
    behavior.lookup_mode = PackageLookupMode::Present;
    behavior.returned_name = returned_name;
    behavior.has_custom_returned_name = true;
}

void append_null_repository_group_member(
    const std::string& repository_name,
    const std::string& group_name) {
    RepositoryGroupBehavior& behavior =
        g_state.repository_group_behaviors[{repository_name, group_name}];
    behavior.lookup_mode = PackageLookupMode::Present;
    behavior.append_null_member = true;
}

std::size_t initialize_call_count() {
    return g_state.initialize_calls;
}

std::size_t local_database_call_count() {
    return g_state.local_database_calls;
}

std::size_t database_valid_call_count() {
    return g_state.database_valid_calls;
}

std::size_t package_cache_call_count() {
    return g_state.package_cache_calls;
}

std::size_t package_query_call_count() {
    return g_state.package_query_calls;
}

std::size_t sync_package_cache_call_count(
    const std::string& repository_name) {
    auto behavior = g_state.sync_database_behaviors.find(repository_name);
    if(behavior == g_state.sync_database_behaviors.end()) return 0;
    return behavior->second.cache_calls;
}

std::size_t created_handle_count() {
    return g_state.handles.size();
}

std::size_t release_call_count() {
    std::size_t count = 0;
    for(const auto& record : g_state.handles)
        count += record->release_count;
    return count;
}

std::size_t release_count_for_handle(std::size_t creation_index) {
    if(creation_index >= g_state.handles.size()) return 0;
    return g_state.handles[creation_index]->release_count;
}

std::size_t owned_list_free_call_count() {
    return g_state.owned_list_free_calls;
}

std::size_t group_expansion_call_count() {
    return g_state.group_expansion_calls;
}

std::string last_initialize_root() {
    return g_state.initialize_root;
}

std::string last_initialize_database_path() {
    return g_state.initialize_database_path;
}

std::string last_queried_package_name() {
    return g_state.queried_package_name;
}

std::vector<std::string> local_package_query_history() {
    return g_state.local_package_queries;
}

std::vector<SyncDatabaseRegistration> sync_database_registration_history() {
    return g_state.sync_database_registrations;
}

std::vector<std::string> sync_database_operation_history() {
    return g_state.sync_database_operations;
}

std::vector<RepositoryPackageQuery> repository_package_query_history() {
    return g_state.repository_package_queries;
}

std::vector<RepositorySearchQuery> repository_search_query_history() {
    return g_state.repository_search_queries;
}

std::vector<RepositoryGroupQuery> repository_group_query_history() {
    return g_state.repository_group_queries;
}

} // namespace package_metadata_test_stub

extern "C" {

void alpm_list_free(alpm_list_t* list) {
    ++g_state.owned_list_free_calls;
    while(list != nullptr) {
        alpm_list_t* next = list->next;
        delete list;
        list = next;
    }
}

alpm_handle_t* alpm_initialize(
    const char* root, const char* database_path, alpm_errno_t* error) {
    append_alpm_event("alpm initialize");
    ++g_state.initialize_calls;
    g_state.initialize_root = root == nullptr ? "" : root;
    g_state.initialize_database_path = database_path == nullptr ? "" : database_path;

    bool environment_failure =
        environment_flag_is_enabled(
            "MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE") ||
        environment_ordinal_matches(
            "MOGUET_TEST_PACKAGE_METADATA_INITIALIZE_FAILURE_AT",
            g_state.initialize_calls);
    if(environment_failure || g_state.initialize_fails) {
        if(error != nullptr) {
            *error = environment_failure ? ALPM_ERR_SYSTEM : g_state.initialize_error;
        }
        return nullptr;
    }

    try {
        configure_foreign_inventory_from_environment();
    } catch(...) {
        g_state.local_packages.clear();
        g_state.package_cache_fails = true;
        g_state.package_cache_error = ALPM_ERR_DB_OPEN;
    }

    auto record = std::make_unique<HandleRecord>(
        g_state.handles.size(), g_state.local_packages.size());
    alpm_handle_t* handle = &record->handle;
    g_state.handles.push_back(std::move(record));
    if(error != nullptr) *error = ALPM_ERR_OK;
    return handle;
}

int alpm_release(alpm_handle_t* handle) {
    append_alpm_event("alpm release");
    HandleRecord* record = record_for_handle(handle);
    if(record == nullptr) return -1;
    ++record->release_count;
    return 0;
}

int alpm_option_set_logcb(alpm_handle_t* handle, alpm_cb_log, void*) {
    // The CLI metadata stub cannot attest a real installed-record lazy load.
    // Link the dormant observer without supplying a positive live-proof path.
    set_handle_error(handle, ALPM_ERR_WRONG_ARGS);
    return -1;
}

alpm_errno_t alpm_errno(alpm_handle_t* handle) {
    if(handle == nullptr) return ALPM_ERR_HANDLE_NULL;
    return handle->error;
}

const char* alpm_strerror(alpm_errno_t error) {
    switch(error) {
        case ALPM_ERR_OK:
            return "no error";
        case ALPM_ERR_SYSTEM:
            return "system error";
        case ALPM_ERR_WRONG_ARGS:
            return "wrong arguments";
        case ALPM_ERR_DB_NULL:
            return "database unavailable";
        case ALPM_ERR_DB_OPEN:
            return "database open failed";
        case ALPM_ERR_DB_NOT_FOUND:
            return "database not found";
        case ALPM_ERR_DB_INVALID:
            return "invalid database";
        case ALPM_ERR_PKG_NOT_FOUND:
            return "package not found";
        default:
            return "stub libalpm error";
    }
}

alpm_db_t* alpm_get_localdb(alpm_handle_t* handle) {
    ++g_state.local_database_calls;
    HandleRecord* record = record_for_handle(handle);
    if(record == nullptr) return nullptr;
    if(!g_state.local_database_available) {
        set_handle_error(handle, g_state.local_database_error);
        return nullptr;
    }
    if(g_state.should_preserve_local_database_error) {
        set_handle_error(handle, g_state.preserved_local_database_error);
        g_state.should_preserve_local_database_error = false;
    } else {
        set_handle_error(handle, ALPM_ERR_OK);
    }
    return &record->database;
}

alpm_db_t* alpm_register_syncdb(
    alpm_handle_t* handle,
    const char* repository_name,
    int signature_level) {
    const std::string repository = repository_name == nullptr ? "" : repository_name;
    append_alpm_event("alpm sync-register", repository.c_str());
    g_state.sync_database_registrations.push_back(
        package_metadata_test_stub::SyncDatabaseRegistration{
            repository, signature_level});
    g_state.sync_database_operations.push_back("register " + repository);

    HandleRecord* handle_record = record_for_handle(handle);
    if(handle_record == nullptr || repository.empty()) {
        set_handle_error(handle, ALPM_ERR_WRONG_ARGS);
        return nullptr;
    }

    SyncDatabaseBehavior& behavior = g_state.sync_database_behaviors[repository];
    if(behavior.register_fails) {
        set_handle_error(handle, behavior.register_error);
        return nullptr;
    }

    if(behavior.should_preserve_register_error) {
        set_handle_error(handle, behavior.preserved_register_error);
        behavior.should_preserve_register_error = false;
    } else {
        set_handle_error(handle, ALPM_ERR_OK);
    }
    auto sync_database = std::make_unique<SyncDatabaseRecord>(handle, repository);
    alpm_db_t* database = &sync_database->database;
    handle_record->sync_databases.push_back(std::move(sync_database));
    return database;
}

int alpm_db_get_valid(alpm_db_t* database) {
    if(database != nullptr && database->kind == AlpmStubDatabaseKind::Sync) {
        append_alpm_event("alpm sync-valid", database->repository_name.c_str());
        g_state.sync_database_operations.push_back(
            "valid " + database->repository_name);
        set_handle_error(database->handle, ALPM_ERR_OK);

        SyncDatabaseBehavior& behavior =
            g_state.sync_database_behaviors[database->repository_name];
        if(behavior.validation_fails) {
            set_handle_error(database->handle, behavior.validation_error);
            return -1;
        }
        return 0;
    }

    ++g_state.database_valid_calls;
    if(database == nullptr) return -1;
    set_handle_error(database->handle, ALPM_ERR_OK);
    if(!g_state.local_database_valid) {
        set_handle_error(database->handle, g_state.local_database_valid_error);
        return -1;
    }
    return 0;
}

alpm_list_t* alpm_db_get_pkgcache(alpm_db_t* database) {
    if(database != nullptr && database->kind == AlpmStubDatabaseKind::Sync) {
        append_alpm_event("alpm sync-cache", database->repository_name.c_str());
        g_state.sync_database_operations.push_back(
            "cache " + database->repository_name);

        SyncDatabaseBehavior& behavior =
            g_state.sync_database_behaviors[database->repository_name];
        ++behavior.cache_calls;
        if(behavior.cache_fails) {
            set_handle_error(database->handle, behavior.cache_error);
            return nullptr;
        }
        if(behavior.should_preserve_cache_error) {
            set_handle_error(database->handle, behavior.preserved_cache_error);
            behavior.should_preserve_cache_error = false;
        } else {
            set_handle_error(database->handle, ALPM_ERR_OK);
        }
        if(behavior.cache_empty) return nullptr;

        SyncDatabaseRecord* database_record = sync_database_record(database);
        if(database_record == nullptr) return nullptr;

        database_record->cache_nodes.clear();
        for(const auto& [identity, package_state] :
            g_state.repository_packages) {
            if(identity.first != database->repository_name ||
               package_state.lookup_mode != PackageLookupMode::Present) {
                continue;
            }
            database_record->cache_nodes.push_back(
                std::make_unique<alpm_list_t>(alpm_list_t{
                    sync_package_for(database, identity.second),
                    nullptr,
                    nullptr}));
        }
        for(std::size_t index = 0;
            index < database_record->cache_nodes.size();
            ++index) {
            database_record->cache_nodes[index]->prev = index == 0
                                                            ? nullptr
                                                            : database_record->cache_nodes[index - 1].get();
            database_record->cache_nodes[index]->next =
                index + 1 == database_record->cache_nodes.size()
                    ? nullptr
                    : database_record->cache_nodes[index + 1].get();
        }
        return database_record->cache_nodes.empty()
                   ? nullptr
                   : database_record->cache_nodes.front().get();
    }

    ++g_state.package_cache_calls;
    if(database == nullptr) return nullptr;
    if(g_state.package_cache_fails) {
        set_handle_error(database->handle, g_state.package_cache_error);
        return nullptr;
    }
    if(g_state.should_preserve_package_cache_error) {
        set_handle_error(database->handle, g_state.preserved_package_cache_error);
        g_state.should_preserve_package_cache_error = false;
    } else {
        set_handle_error(database->handle, ALPM_ERR_OK);
    }
    if(g_state.package_cache_empty) return nullptr;

    HandleRecord* record = record_for_handle(database->handle);
    if(record == nullptr || record->local_cache_nodes.empty()) return nullptr;
    return record->local_cache_nodes.front().get();
}

int alpm_db_search(
    alpm_db_t* database,
    const alpm_list_t* needles,
    alpm_list_t** results) {
    if(database == nullptr ||
       database->kind != AlpmStubDatabaseKind::Sync ||
       needles == nullptr || needles->data == nullptr ||
       needles->next != nullptr || results == nullptr || *results != nullptr) {
        if(database != nullptr) {
            set_handle_error(database->handle, ALPM_ERR_WRONG_ARGS);
        }
        return -1;
    }

    const std::string query(static_cast<const char*>(needles->data));
    g_state.repository_search_queries.push_back(
        package_metadata_test_stub::RepositorySearchQuery{
            database->repository_name, query});
    g_state.sync_database_operations.push_back(
        "search " + database->repository_name + "/" + query);

    RepositorySearchBehavior& behavior =
        g_state.repository_search_behaviors[{database->repository_name, query}];
    if(behavior.fails) {
        set_handle_error(database->handle, behavior.error);
        return -1;
    }

    std::vector<void*> package_values;
    package_values.reserve(behavior.package_names.size());
    for(const auto& package_name : behavior.package_names) {
        package_values.push_back(sync_package_for(database, package_name));
    }
    *results = make_owned_list(package_values);
    if(behavior.should_preserve_error) {
        set_handle_error(database->handle, behavior.preserved_error);
        behavior.should_preserve_error = false;
    } else {
        set_handle_error(database->handle, ALPM_ERR_OK);
    }
    return 0;
}

alpm_group_t* alpm_db_get_group(
    alpm_db_t* database,
    const char* group_name) {
    if(database == nullptr ||
       database->kind != AlpmStubDatabaseKind::Sync ||
       group_name == nullptr || group_name[0] == '\0') {
        if(database != nullptr) {
            set_handle_error(database->handle, ALPM_ERR_WRONG_ARGS);
        }
        return nullptr;
    }

    const std::string group(group_name);
    g_state.repository_group_queries.push_back(
        package_metadata_test_stub::RepositoryGroupQuery{
            database->repository_name, group});
    g_state.sync_database_operations.push_back(
        "group " + database->repository_name + "/" + group);

    RepositoryGroupBehavior& behavior =
        g_state.repository_group_behaviors[{database->repository_name, group}];
    switch(behavior.lookup_mode) {
        case PackageLookupMode::Present: {
            SyncDatabaseRecord* database_record =
                sync_database_record(database);
            if(database_record == nullptr) {
                set_handle_error(database->handle, ALPM_ERR_DB_NULL);
                return nullptr;
            }
            auto& group_record = database_record->groups[group];
            if(group_record == nullptr) {
                group_record = std::make_unique<GroupRecord>(
                    behavior.has_custom_returned_name
                        ? behavior.returned_name
                        : group);
            }
            return &group_record->group;
        }
        case PackageLookupMode::Absent:
            return nullptr;
        case PackageLookupMode::Failure:
            set_handle_error(database->handle, behavior.query_error);
            return nullptr;
        case PackageLookupMode::NullWithoutError:
            return nullptr;
    }
    return nullptr;
}

alpm_list_t* alpm_find_group_pkgs(
    alpm_list_t* databases,
    const char* group_name) {
    ++g_state.group_expansion_calls;
    if(databases == nullptr || group_name == nullptr || group_name[0] == '\0') {
        return nullptr;
    }

    auto* first_database = static_cast<alpm_db_t*>(databases->data);
    if(first_database == nullptr) return nullptr;
    std::set<std::string> seen_package_names;
    std::vector<void*> package_values;
    for(alpm_list_t* node = databases; node != nullptr; node = node->next) {
        auto* database = static_cast<alpm_db_t*>(node->data);
        if(database == nullptr ||
           database->kind != AlpmStubDatabaseKind::Sync) {
            set_handle_error(first_database->handle, ALPM_ERR_DB_NULL);
            return nullptr;
        }

        RepositoryGroupBehavior& behavior =
            g_state.repository_group_behaviors[{database->repository_name, group_name}];
        if(behavior.lookup_mode != PackageLookupMode::Present) continue;
        for(const auto& package_name : behavior.package_names) {
            if(!seen_package_names.insert(package_name).second) continue;
            package_values.push_back(
                sync_package_for(database, package_name));
        }
        if(behavior.append_null_member) package_values.push_back(nullptr);
    }

    return make_owned_list(package_values);
}

alpm_pkg_t* alpm_db_get_pkg(alpm_db_t* database, const char* name) {
    if(database != nullptr && database->kind == AlpmStubDatabaseKind::Sync) {
        const std::string package_name = name == nullptr ? "" : name;
        const std::string query_identity =
            database->repository_name + "/" + package_name;
        append_alpm_event("alpm sync-query", query_identity.c_str());
        g_state.sync_database_operations.push_back(
            "query " + query_identity);
        g_state.repository_package_queries.push_back(
            package_metadata_test_stub::RepositoryPackageQuery{
                database->repository_name, package_name});

        if(name == nullptr || name[0] == '\0') {
            set_handle_error(database->handle, ALPM_ERR_WRONG_ARGS);
            return nullptr;
        }

        RepositoryPackageState& package_state = repository_package_state(
            database->repository_name, package_name);
        try {
            configure_repository_package_from_environment(
                database->repository_name, package_name, package_state);
        } catch(...) {
            package_state.lookup_mode = PackageLookupMode::Failure;
            package_state.query_error = ALPM_ERR_DB_OPEN;
        }
        if(environment_requests_query_failure(name)) {
            package_state.lookup_mode = PackageLookupMode::Failure;
            package_state.query_error = ALPM_ERR_DB_OPEN;
        }

        if(package_state.should_preserve_query_error) {
            set_handle_error(database->handle, package_state.preserved_query_error);
            package_state.should_preserve_query_error = false;
        } else {
            set_handle_error(database->handle, ALPM_ERR_OK);
        }

        switch(package_state.lookup_mode) {
            case PackageLookupMode::Present: {
                SyncDatabaseRecord* database_record = sync_database_record(database);
                if(database_record == nullptr) return nullptr;
                auto& package = database_record->packages[package_name];
                if(package == nullptr) {
                    package = std::make_unique<alpm_pkg_t>(alpm_pkg_t{
                        database->handle,
                        AlpmStubDatabaseKind::Sync,
                        database->repository_name,
                        package_name,
                        0});
                }
                return package.get();
            }
            case PackageLookupMode::Absent:
                set_handle_error(database->handle, ALPM_ERR_PKG_NOT_FOUND);
                return nullptr;
            case PackageLookupMode::Failure:
                set_handle_error(database->handle, package_state.query_error);
                return nullptr;
            case PackageLookupMode::NullWithoutError:
                return nullptr;
        }
        return nullptr;
    }

    append_alpm_event("alpm query", name == nullptr ? "" : name);
    ++g_state.package_query_calls;
    if(database == nullptr) return nullptr;
    if(g_state.should_preserve_package_query_error) {
        g_state.should_preserve_package_query_error = false;
    } else {
        set_handle_error(database->handle, ALPM_ERR_OK);
    }
    g_state.queried_package_name = name == nullptr ? "" : name;
    g_state.local_package_queries.push_back(g_state.queried_package_name);

    if(g_state.local_package_query_strict_mode) {
        return consume_local_package_query_expectation(
            database, g_state.queried_package_name);
    }

    if(name == nullptr || name[0] == '\0') {
        set_handle_error(database->handle, ALPM_ERR_WRONG_ARGS);
        return nullptr;
    }

    PackageLookupMode lookup_mode = g_state.package_lookup_mode;
    alpm_errno_t query_error = g_state.package_query_error;
    try {
        configure_package_lookup_from_environment(name, lookup_mode, query_error);
    } catch(...) {
        // Test fixtureの読込自体が失敗しても、package absenceへflattenしない。
        lookup_mode = PackageLookupMode::Failure;
        query_error = ALPM_ERR_DB_OPEN;
    }

    switch(lookup_mode) {
        case PackageLookupMode::Present: {
            HandleRecord* record = record_for_handle(database->handle);
            if(record == nullptr || record->local_packages.empty()) return nullptr;
            return record->local_packages.front().get();
        }
        case PackageLookupMode::Absent:
            set_handle_error(database->handle, ALPM_ERR_PKG_NOT_FOUND);
            return nullptr;
        case PackageLookupMode::Failure:
            set_handle_error(database->handle, query_error);
            return nullptr;
        case PackageLookupMode::NullWithoutError:
            return nullptr;
    }
    return nullptr;
}

const char* alpm_pkg_get_name(alpm_pkg_t* package) {
    if(package == nullptr) return nullptr;
    if(package->kind == AlpmStubDatabaseKind::Sync) {
        RepositoryPackageState* package_state = repository_package_state(package);
        if(package_state == nullptr || package_state->name_is_null) return nullptr;
        return package_state->returned_name.c_str();
    }

    set_handle_error(package->handle, ALPM_ERR_OK);
    LocalPackageState* package_state = local_package_state(package);
    if(package_state == nullptr || package_state->name_is_null) return nullptr;
    return package_state->name.c_str();
}

const char* alpm_pkg_get_version(alpm_pkg_t* package) {
    if(package == nullptr) return nullptr;
    if(package->kind == AlpmStubDatabaseKind::Sync) {
        RepositoryPackageState* package_state = repository_package_state(package);
        if(package_state == nullptr || package_state->version_is_null) return nullptr;
        return package_state->version.c_str();
    }
    set_handle_error(package->handle, ALPM_ERR_OK);
    LocalPackageState* package_state = local_package_state(package);
    if(package_state == nullptr || package_state->version_is_null) return nullptr;
    return package_state->version.c_str();
}

const char* alpm_pkg_get_base(alpm_pkg_t* package) {
    if(package != nullptr && package->kind == AlpmStubDatabaseKind::Local) {
        LocalPackageState* package_state = local_package_state(package);
        if(package_state == nullptr || package_state->package_base_is_null) {
            return nullptr;
        }
        return package_state->package_base.c_str();
    }
    RepositoryPackageState* package_state = repository_package_state(package);
    if(package_state == nullptr || package_state->package_base_is_null) {
        return nullptr;
    }
    return package_state->package_base.c_str();
}

const char* alpm_pkg_get_arch(alpm_pkg_t* package) {
    if(package == nullptr) return nullptr;
    if(package->kind == AlpmStubDatabaseKind::Local) {
        LocalPackageState* package_state = local_package_state(package);
        if(package_state == nullptr || package_state->architecture_is_null) {
            return nullptr;
        }
        return package_state->architecture.c_str();
    }
    RepositoryPackageState* package_state = repository_package_state(package);
    if(package_state == nullptr || package_state->architecture_is_null) {
        return nullptr;
    }
    return package_state->architecture.c_str();
}

alpm_filelist_t* alpm_pkg_get_files(alpm_pkg_t* package) {
    if(package) set_handle_error(package->handle, ALPM_ERR_PKG_INVALID);
    return nullptr;
}

alpm_list_t* alpm_pkg_get_provides(alpm_pkg_t* package) {
    if(package != nullptr && package->kind == AlpmStubDatabaseKind::Local) {
        LocalPackageState* package_state = local_package_state(package);
        if(package_state == nullptr || package_state->provide_nodes.empty()) {
            return nullptr;
        }
        return package_state->provide_nodes.data();
    }
    RepositoryPackageState* package_state = repository_package_state(package);
    if(package_state == nullptr || package_state->provide_nodes.empty()) {
        return nullptr;
    }
    return package_state->provide_nodes.data();
}

alpm_list_t* alpm_pkg_get_groups(alpm_pkg_t*) {
    return nullptr;
}

alpm_list_t* alpm_pkg_get_depends(alpm_pkg_t* package) {
    if(package == nullptr || package->kind != AlpmStubDatabaseKind::Local) {
        return nullptr;
    }
    LocalPackageState* package_state = local_package_state(package);
    if(package_state == nullptr || package_state->dependency_nodes.empty()) {
        return nullptr;
    }
    return package_state->dependency_nodes.data();
}

alpm_pkg_t* alpm_find_satisfier(
    alpm_list_t* packages,
    const char* dependency_specification) {
    if(dependency_specification == nullptr ||
       dependency_specification[0] == '\0') {
        return nullptr;
    }
    const std::string specification(dependency_specification);
    const std::size_t relation = specification.find_first_of("<>=");
    const std::string required_name = specification.substr(0, relation);
    if(required_name.empty()) return nullptr;
    for(alpm_list_t* node = packages; node != nullptr; node = node->next) {
        auto* package = static_cast<alpm_pkg_t*>(node->data);
        const char* package_name = alpm_pkg_get_name(package);
        if(package_name != nullptr && required_name == package_name) {
            return package;
        }
    }
    return nullptr;
}

char* alpm_dep_compute_string(const alpm_depend_t* dependency) {
    if(dependency == nullptr || dependency->name == nullptr ||
       dependency->name[0] == '\0') {
        return nullptr;
    }

    const char* relation = nullptr;
    switch(dependency->mod) {
        case ALPM_DEP_MOD_ANY:
            relation = "";
            break;
        case ALPM_DEP_MOD_EQ:
            relation = "=";
            break;
        case ALPM_DEP_MOD_GE:
            relation = ">=";
            break;
        case ALPM_DEP_MOD_LE:
            relation = "<=";
            break;
        case ALPM_DEP_MOD_GT:
            relation = ">";
            break;
        case ALPM_DEP_MOD_LT:
            relation = "<";
            break;
        default:
            return nullptr;
    }

    const bool is_unversioned = dependency->mod == ALPM_DEP_MOD_ANY;
    const bool has_version = dependency->version != nullptr &&
                             dependency->version[0] != '\0';
    if((is_unversioned && has_version) ||
       (!is_unversioned && !has_version)) {
        return nullptr;
    }

    std::string specification = dependency->name;
    if(!is_unversioned) {
        specification += relation;
        specification += dependency->version;
    }
    char* result = static_cast<char*>(
        std::malloc(specification.size() + 1U));
    if(result == nullptr) return nullptr;
    std::memcpy(
        result, specification.c_str(), specification.size() + 1U);
    return result;
}

const char* alpm_pkg_get_desc(alpm_pkg_t* package) {
    RepositoryPackageState* package_state = repository_package_state(package);
    if(package_state == nullptr) return nullptr;
    return package_state->description.c_str();
}

alpm_db_t* alpm_pkg_get_db(alpm_pkg_t* package) {
    if(package == nullptr || package->kind != AlpmStubDatabaseKind::Sync) {
        return nullptr;
    }
    HandleRecord* handle_record = record_for_handle(package->handle);
    if(handle_record == nullptr) return nullptr;
    for(const auto& database : handle_record->sync_databases) {
        if(database->database.repository_name == package->repository_name) {
            return &database->database;
        }
    }
    return nullptr;
}

off_t alpm_pkg_get_size(alpm_pkg_t* package) {
    RepositoryPackageState* package_state = repository_package_state(package);
    return package_state == nullptr ? static_cast<off_t>(-1)
                                    : package_state->package_size;
}

off_t alpm_pkg_get_isize(alpm_pkg_t* package) {
    RepositoryPackageState* package_state = repository_package_state(package);
    return package_state == nullptr ? static_cast<off_t>(-1)
                                    : package_state->installed_size;
}

alpm_pkgreason_t alpm_pkg_get_reason(alpm_pkg_t* package) {
    if(package == nullptr) return static_cast<alpm_pkgreason_t>(-1);
    set_handle_error(package->handle, ALPM_ERR_OK);
    LocalPackageState* package_state = local_package_state(package);
    return package_state == nullptr
               ? static_cast<alpm_pkgreason_t>(-1)
               : package_state->reason;
}

int alpm_pkg_vercmp(const char* lhs, const char* rhs) {
    // This test binary intentionally does not link libalpm. Constraint cases
    // must declare the one expected comparison and its libalpm-style result;
    // the stub never implements an Arch version ordering algorithm.
    const char* expected_lhs =
        std::getenv("MOGUET_TEST_ALPM_VERCMP_EXPECTED_LHS");
    const char* expected_rhs =
        std::getenv("MOGUET_TEST_ALPM_VERCMP_EXPECTED_RHS");
    const char* expected_result =
        std::getenv("MOGUET_TEST_ALPM_VERCMP_RESULT");
    if(lhs != nullptr && rhs != nullptr && expected_lhs != nullptr &&
       expected_rhs != nullptr && expected_result != nullptr &&
       std::strcmp(lhs, expected_lhs) == 0 &&
       std::strcmp(rhs, expected_rhs) == 0) {
        if(std::strcmp(expected_result, "-1") == 0) return -1;
        if(std::strcmp(expected_result, "0") == 0) return 0;
        if(std::strcmp(expected_result, "1") == 0) return 1;
    }
    std::fputs(
        "Package metadata stub received an unexpected version comparison\n",
        stderr);
    std::abort();
}

} // extern "C"
