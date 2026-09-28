#include "integration_stub.hpp"

#include "alpm_stub.hpp"
#include "aur_rpc.hpp"
#include "logging.hpp"

#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct IntegrationStubState {
    std::map<std::string, std::deque<CapturedCommandResult>> command_results;
    std::vector<std::string> captured_commands;
    std::vector<std::string> strict_info_calls;
    std::vector<std::string> strict_provider_search_calls;
    std::size_t forbidden_operation_count = 0;
};

IntegrationStubState g_state;

AurPackageInfo package_info(
    const std::string& name,
    const std::vector<std::string>& dependencies = {},
    const std::vector<std::string>& conflicts = {},
    const std::vector<std::string>& replaces = {},
    const std::string& version = "2.0-1") {
    AurPackageInfo info;
    info.Name = name;
    info.PackageBase = name;
    info.Version = version;
    info.Depends = dependencies;
    info.Conflicts = conflicts;
    info.Replaces = replaces;
    info.Maintainer = "moguet-test";
    return info;
}

[[noreturn]] void reject_forbidden_operation(const std::string& operation) {
    ++g_state.forbidden_operation_count;
    throw std::runtime_error(
        "AUR update preflight integration invoked forbidden operation: " +
        operation);
}

} // namespace

namespace aur_update_execution_preflight_integration_stub {

void reset() {
    g_state = IntegrationStubState{};
    package_metadata_test_stub::reset_alpm_stub();
}

void enqueue_captured_command_result(
    const std::string& command, CapturedCommandResult result) {
    g_state.command_results[command].push_back(std::move(result));
}

std::vector<std::string> captured_commands() {
    return g_state.captured_commands;
}

std::vector<std::string> strict_info_calls() {
    return g_state.strict_info_calls;
}

std::vector<std::string> strict_provider_search_calls() {
    return g_state.strict_provider_search_calls;
}

std::size_t forbidden_operation_count() {
    return g_state.forbidden_operation_count;
}

} // namespace aur_update_execution_preflight_integration_stub

CapturedCommandResult capture_explicit_process_output_raw(
    const ExplicitProcessInvocation&, bool) {
    // The shared metadata object also exposes the private prefix transport;
    // preflight fixtures must never enter it or spawn a real configuration tool.
    reject_forbidden_operation("capture_explicit_process_output_raw");
}

CapturedCommandResult capture_command_output_raw(const char* command) {
    const std::string command_text = command == nullptr ? "" : command;
    g_state.captured_commands.push_back(command_text);

    auto result = g_state.command_results.find(command_text);
    if(result == g_state.command_results.end() || result->second.empty()) {
        reject_forbidden_operation("capture_command_output_raw: " + command_text);
    }

    CapturedCommandResult command_result = std::move(result->second.front());
    result->second.pop_front();
    return command_result;
}

CapturedCommandResult capture_command_output(const char* command) {
    reject_forbidden_operation(
        "capture_command_output: " +
        std::string(command == nullptr ? "" : command));
}

std::string exec_command(const char* command) {
    reject_forbidden_operation(
        "exec_command: " + std::string(command == nullptr ? "" : command));
}

int command_status(const std::string& command) {
    reject_forbidden_operation("command_status: " + command);
}

int run_command(const std::string& command) {
    reject_forbidden_operation("run_command: " + command);
}

int run_command_with_stdin_fd(const std::string& command, int) {
    reject_forbidden_operation("run_command_with_stdin_fd: " + command);
}

std::vector<AurPackageInfo> AurClient::search(const std::string& query) {
    throw std::runtime_error("Unexpected AUR search in integration test: " + query);
}

std::vector<std::string> AurClient::search_names_by_provides(
    const std::string& provided_name) {
    throw std::runtime_error(
        "Unexpected legacy AUR provider search in integration test: " +
        provided_name);
}

std::vector<std::string> AurClient::search_names_by_provides_strict(
    const std::string& provided_name) {
    g_state.strict_provider_search_calls.push_back(provided_name);
    return {};
}

std::optional<AurPackageInfo> AurClient::info(const std::string& package_name) {
    throw std::runtime_error(
        "Unexpected legacy AUR info in integration test: " + package_name);
}

std::optional<AurPackageInfo> AurClient::info_strict(
    const std::string& package_name) {
    g_state.strict_info_calls.push_back(package_name);

    if(package_name == "explicit-root" || package_name == "dependency-root" ||
       package_name == "combined-root-a" || package_name == "combined-root-b") {
        return package_info(package_name);
    }
    if(package_name == "repository-failure-root") {
        return package_info(package_name, {"repository-failure-child"});
    }
    if(package_name == "repository-failure-child") {
        throw std::runtime_error(
            "Repository metadata failure incorrectly fell back to AUR exact lookup.");
    }
    if(package_name == "aur-failure-root") {
        return package_info(package_name, {"aur-failure-child"});
    }
    if(package_name == "aur-failure-child") {
        throw std::runtime_error("strict integration dependency metadata failure");
    }
    if(package_name == "relation-installed-root") {
        return package_info(
            package_name, {}, {"installed-conflict"});
    }
    if(package_name == "relation-no-match-root") {
        return package_info(
            package_name, {}, {"absent-conflict"});
    }
    if(package_name == "relation-query-failure-root") {
        return package_info(
            package_name, {}, {"unknown-conflict"});
    }
    if(package_name == "localization-required-root") {
        return package_info(
            package_name, {"localization-required-git"});
    }
    if(package_name == "localization-independent-root") {
        return package_info(package_name);
    }
    if(package_name == "localization-required-git" ||
       package_name == "localization-independent-git") {
        return package_info(
            package_name, {}, {}, {}, "1.0-1");
    }

    throw AurRpcResponseError(
        "Unexpected strict AUR info in integration test: " + package_name);
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    throw std::runtime_error(
        "Unexpected AUR multi-info in integration test for " +
        std::to_string(package_names.size()) + " packages.");
}

void Logger::set_diagnostics_to_stderr() {
}

void Logger::init(const std::filesystem::path&) {
}

void Logger::info(const std::string&) {
}

void Logger::warn(const std::string&) {
}

void Logger::error(const std::string&) {
}

void Logger::raw_cmd(const std::string& command) {
    reject_forbidden_operation("Logger::raw_cmd: " + command);
}
