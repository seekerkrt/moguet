#include "query_stub.hpp"

#include "process.hpp"

#include <deque>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {

namespace stub = filtered_aur_update_operation_query_test_stub;

struct ScriptFailure {
    std::string diagnostic;
};

using RepositoryConfigurationScript = std::variant<
    PacmanRepositoryConfiguration,
    PackageMetadataFailure>;
using ForeignInventoryScript = std::variant<
    ForeignPackageInventory,
    PackageMetadataFailure>;
using InfoManyScript = std::variant<
    std::map<std::string, AurPackageInfo>,
    ScriptFailure>;
using InfoStrictScript = std::variant<
    std::optional<AurPackageInfo>,
    ScriptFailure>;
using VercmpScript = std::variant<std::string, ScriptFailure>;

struct QueryStubState {
    RepositoryConfigurationScript repository_configuration =
        PacmanRepositoryConfiguration{
            PacmanDatabasePaths{
                "/filtered-operation-stub/root",
                "/filtered-operation-stub/database"},
            {"filtered-operation-stub-repository"}};
    ForeignInventoryScript foreign_inventory = ForeignPackageInventory{};

    std::deque<InfoManyScript> info_many_scripts;
    std::deque<InfoStrictScript> info_strict_scripts;
    std::deque<VercmpScript> vercmp_scripts;

    std::size_t repository_configuration_call_count = 0;
    std::vector<PacmanRepositoryConfiguration>
        inventory_configurations;
    std::vector<std::vector<std::string>> info_many_calls;
    std::vector<std::string> info_strict_calls;
    std::vector<std::string> vercmp_calls;
    std::vector<stub::Event> events;
    std::optional<std::string> expectation_failure;
};

QueryStubState g_state;

[[noreturn]] void fail_unexpected_call(const std::string& diagnostic) {
    g_state.expectation_failure = diagnostic;
    throw std::logic_error(diagnostic);
}

template <typename Script>
Script take_script(
    std::deque<Script>& scripts,
    const std::string& missing_diagnostic) {
    if(scripts.empty()) fail_unexpected_call(missing_diagnostic);

    Script script = std::move(scripts.front());
    scripts.pop_front();
    return script;
}

} // namespace

namespace filtered_aur_update_operation_query_test_stub {

void reset() {
    g_state = QueryStubState{};
}

void set_repository_configuration(
    PacmanRepositoryConfiguration configuration) {
    g_state.repository_configuration = std::move(configuration);
}

void set_repository_configuration_failure(PackageMetadataFailure failure) {
    g_state.repository_configuration = std::move(failure);
}

void set_foreign_inventory(ForeignPackageInventory inventory) {
    g_state.foreign_inventory = std::move(inventory);
}

void set_foreign_inventory_failure(PackageMetadataFailure failure) {
    g_state.foreign_inventory = std::move(failure);
}

void enqueue_info_many_result(
    std::map<std::string, AurPackageInfo> result) {
    g_state.info_many_scripts.push_back(std::move(result));
}

void enqueue_info_many_failure(std::string diagnostic) {
    g_state.info_many_scripts.push_back(
        ScriptFailure{std::move(diagnostic)});
}

void enqueue_info_strict_result(std::optional<AurPackageInfo> result) {
    g_state.info_strict_scripts.push_back(std::move(result));
}

void enqueue_info_strict_failure(std::string diagnostic) {
    g_state.info_strict_scripts.push_back(
        ScriptFailure{std::move(diagnostic)});
}

void enqueue_vercmp_result(std::string output) {
    g_state.vercmp_scripts.push_back(std::move(output));
}

void enqueue_vercmp_failure(std::string diagnostic) {
    g_state.vercmp_scripts.push_back(
        ScriptFailure{std::move(diagnostic)});
}

std::size_t repository_configuration_calls() {
    return g_state.repository_configuration_call_count;
}

std::size_t inventory_calls() {
    return g_state.inventory_configurations.size();
}

const std::vector<PacmanRepositoryConfiguration>&
inventory_configuration_history() {
    return g_state.inventory_configurations;
}

const std::vector<std::vector<std::string>>& info_many_call_history() {
    return g_state.info_many_calls;
}

const std::vector<std::string>& info_strict_call_history() {
    return g_state.info_strict_calls;
}

const std::vector<std::string>& vercmp_call_history() {
    return g_state.vercmp_calls;
}

const std::vector<Event>& event_history() {
    return g_state.events;
}

void require_script_consumed() {
    if(g_state.expectation_failure.has_value()) {
        throw std::logic_error(*g_state.expectation_failure);
    }
    if(!g_state.info_many_scripts.empty()) {
        throw std::logic_error(
            "Filtered AUR query stub has unconsumed info_many scripts.");
    }
    if(!g_state.info_strict_scripts.empty()) {
        throw std::logic_error(
            "Filtered AUR query stub has unconsumed info_strict scripts.");
    }
    if(!g_state.vercmp_scripts.empty()) {
        throw std::logic_error(
            "Filtered AUR query stub has unconsumed vercmp scripts.");
    }
}

} // namespace filtered_aur_update_operation_query_test_stub

// WHY(#281): PackageMetadataErrorの実装は同じtest binaryにlinkするpreparation
// stubが所有する。ここではquery transport symbolだけを差し替え、重複定義しない。
PacmanRepositoryConfiguration resolve_pacman_repository_configuration() {
    ++g_state.repository_configuration_call_count;
    g_state.events.push_back(stub::Event{
        stub::EventKind::RepositoryConfigurationResolution,
        {},
        "repository-configuration"});

    if(const auto* failure = std::get_if<PackageMetadataFailure>(
           &g_state.repository_configuration)) {
        throw PackageMetadataError(*failure);
    }
    return std::get<PacmanRepositoryConfiguration>(
        g_state.repository_configuration);
}

ForeignPackageInventoryResult query_foreign_package_inventory(
    const PacmanRepositoryConfiguration& configuration) {
    g_state.inventory_configurations.push_back(configuration);
    g_state.events.push_back(stub::Event{
        stub::EventKind::ForeignInventoryQuery,
        {},
        "foreign-inventory"});

    if(const auto* failure = std::get_if<PackageMetadataFailure>(
           &g_state.foreign_inventory)) {
        return *failure;
    }
    return std::get<ForeignPackageInventory>(g_state.foreign_inventory);
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    g_state.info_many_calls.push_back(package_names);
    g_state.events.push_back(stub::Event{
        stub::EventKind::AurInfoMany,
        package_names,
        "aur-info-many"});

    InfoManyScript script = take_script(
        g_state.info_many_scripts,
        "Unexpected AurClient::info_many call with no pending script.");
    if(const auto* failure = std::get_if<ScriptFailure>(&script)) {
        throw std::runtime_error(failure->diagnostic);
    }
    return std::get<std::map<std::string, AurPackageInfo>>(
        std::move(script));
}

std::optional<AurPackageInfo> AurClient::info_strict(
    const std::string& package_name) {
    g_state.info_strict_calls.push_back(package_name);
    g_state.events.push_back(stub::Event{
        stub::EventKind::AurInfoStrict,
        {package_name},
        package_name});

    InfoStrictScript script = take_script(
        g_state.info_strict_scripts,
        "Unexpected AurClient::info_strict call with no pending script.");
    if(const auto* failure = std::get_if<ScriptFailure>(&script)) {
        throw std::runtime_error(failure->diagnostic);
    }
    return std::get<std::optional<AurPackageInfo>>(std::move(script));
}

CapturedCommandResult capture_command_output(const char* command) {
    if(command == nullptr) {
        fail_unexpected_call(
            "Filtered AUR query stub received a null command.");
    }

    const std::string command_string(command);
    g_state.vercmp_calls.push_back(command_string);
    g_state.events.push_back(stub::Event{
        stub::EventKind::VersionCompare,
        {},
        command_string});

    VercmpScript script = take_script(
        g_state.vercmp_scripts,
        "Unexpected vercmp call with no pending script.");
    if(const auto* failure = std::get_if<ScriptFailure>(&script)) {
        throw std::runtime_error(failure->diagnostic);
    }
    return CapturedCommandResult{std::get<std::string>(std::move(script)), 0};
}
