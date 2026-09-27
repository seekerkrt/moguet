#include "process_stub.hpp"

#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace {

std::map<std::string, std::deque<CapturedCommandResult>> g_results_by_command;
std::vector<std::string> g_commands;

} // namespace

namespace package_metadata_test_stub {

void reset_process_stub() {
    g_results_by_command.clear();
    g_commands.clear();
}

void enqueue_captured_command_result(
    const std::string& command,
    CapturedCommandResult result) {
    g_results_by_command[command].push_back(std::move(result));
}

std::size_t capture_command_call_count() {
    return g_commands.size();
}

std::string last_captured_command() {
    return g_commands.empty() ? "" : g_commands.back();
}

std::vector<std::string> captured_commands() {
    return g_commands;
}

} // namespace package_metadata_test_stub

CapturedCommandResult capture_command_output_raw(const char* command) {
    std::string command_text = command == nullptr ? "" : command;
    g_commands.push_back(command_text);

    auto result_queue = g_results_by_command.find(command_text);
    if(result_queue == g_results_by_command.end() || result_queue->second.empty()) {
        return CapturedCommandResult{};
    }

    CapturedCommandResult result = std::move(result_queue->second.front());
    result_queue->second.pop_front();
    return result;
}

CapturedCommandResult capture_explicit_process_output_raw(
    const ExplicitProcessInvocation& invocation, bool) {
    std::string command = "pacman-conf";
    for(const auto& argument : invocation.arguments)
        command += " " + argument;
    command += " 2>/dev/null";
    auto result = capture_command_output_raw(command.c_str());
    if(invocation.stdout_capture_limit && result.output.size() > *invocation.stdout_capture_limit) {
        result.output.resize(*invocation.stdout_capture_limit);
        result.stdout_capture_limit_exceeded = true;
    }
    return result;
}
