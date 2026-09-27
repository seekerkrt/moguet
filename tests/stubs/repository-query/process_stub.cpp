#include "process_stub.hpp"

#include <deque>
#include <map>
#include <stdexcept>
#include <utility>

namespace {

std::map<std::string, std::deque<CapturedCommandResult>> g_results_by_command;
std::vector<std::string> g_commands;

} // namespace

namespace repository_query_test_stub {

void enqueue_captured_command_result(
    const std::string& command,
    CapturedCommandResult result) {
    g_results_by_command[command].push_back(std::move(result));
}

std::vector<std::string> captured_commands() {
    return g_commands;
}

} // namespace repository_query_test_stub

CapturedCommandResult capture_explicit_process_output_raw(
    const ExplicitProcessInvocation&, bool) {
    // This fixture links the shared metadata object but never queries prefixes.
    // Reject that private transport rather than launching a real pacman-conf.
    throw std::logic_error("Unexpected explicit capture in repository query test.");
}

CapturedCommandResult capture_command_output_raw(const char* command) {
    std::string command_text = command == nullptr ? "" : command;
    g_commands.push_back(command_text);

    auto result_queue = g_results_by_command.find(command_text);
    if(result_queue == g_results_by_command.end() || result_queue->second.empty())
        return CapturedCommandResult{};

    CapturedCommandResult result = std::move(result_queue->second.front());
    result_queue->second.pop_front();
    return result;
}

std::string exec_command(const char*) {
    throw std::runtime_error(
        "Unexpected legacy exec_command call in repository query test.");
}

int command_status(const std::string&) {
    throw std::runtime_error(
        "Unexpected pacman command in repository query test.");
}
