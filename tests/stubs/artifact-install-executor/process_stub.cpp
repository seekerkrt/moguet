#include "process_stub.hpp"

#include <cstdlib>
#include <deque>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <map>
#include <sstream>
#include <vector>
#include <wordexp.h>
#include <fcntl.h>
#include <unistd.h>
#include "shell_words.hpp"

namespace {

enum class ExpectedProcessKind {
    Capture,
    Run,
};

struct ExpectedProcessCall {
    ExpectedProcessKind kind;
    std::string command;
    CapturedCommandResult capture_result;
    int run_exit_code = 0;
};

struct ProcessStubState {
    // POLICY: capture/runを同じFIFOへ積み、process API種別を跨ぐ順序も契約に含める。
    std::deque<ExpectedProcessCall> expected_calls;
    std::size_t capture_calls = 0;
    std::size_t run_calls = 0;
    std::string last_captured_command;
    std::string last_run_command;
    std::map<std::string, std::pair<std::string, std::string>> queried_archives;
    const char* expectation_failure = nullptr;
    void (*capture_hook)() = nullptr;
    void (*run_hook)() = nullptr;
};

ProcessStubState* g_state = nullptr;
bool g_cleanup_registered = false;

void destroy_process_stub_state() {
    delete g_state;
    g_state = nullptr;
}

ProcessStubState& process_stub_state() {
    if(g_state == nullptr) {
        g_state = new ProcessStubState;
        if(!g_cleanup_registered) {
            static_cast<void>(std::atexit(destroy_process_stub_state));
            g_cleanup_registered = true;
        }
    }
    return *g_state;
}

[[noreturn]] void fail_process_expectation(
    ProcessStubState& state, const char* diagnostic) {
    // POLICY: fixed diagnosticだけを保持し、package-controlled commandをerrorへ埋め込まない。
    state.expectation_failure = diagnostic;
    throw std::logic_error(diagnostic);
}

std::vector<std::string> words(const std::string& command) {
    wordexp_t expansion{};
    if(wordexp(command.c_str(), &expansion, WRDE_NOCMD) != 0)
        throw std::logic_error("Invalid quoted test command.");
    std::vector<std::string> result;
    for(std::size_t i = 0; i < expansion.we_wordc; ++i)
        result.emplace_back(expansion.we_wordv[i]);
    wordfree(&expansion);
    return result;
}

// Legacy expectations still describe the selected paths/options. Decode the
// real helper argv and its sealed input to compare that semantic intent, rather
// than teaching every source-build fixture a random token and FD number.
std::string legacy_transaction_projection(ProcessStubState& state, const std::string& command) {
    const auto prefix = shell_words::join({"/usr/bin/sudo", "--", MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH, "install-legacy"}) + " ";
    if(!command.starts_with(prefix)) return command;
    const auto args = words(command);
    if(args.size() < 21 || args[0] != "/usr/bin/sudo" || args[1] != "--" ||
       args[2] != MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH || args[11] != "--" || (args.size() - 12) % 9 != 0)
        throw std::logic_error("Invalid legacy helper argv.");
    const std::string input_path = "/proc/" + args[4] + "/fd/" + args[5];
    const int input = open(input_path.c_str(), O_RDONLY | O_CLOEXEC);
    if(input < 0) throw std::logic_error("Missing legacy sealed input.");
    const int seals = fcntl(input, F_GET_SEALS);
    close(input);
    if((seals & (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) !=
       (F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL))
        throw std::logic_error("Mutable legacy input.");
    std::ifstream bytes(input_path, std::ios::binary);
    std::vector<std::string> projected{"sudo", "pacman", "-U"};
    if(args[10] == "1") projected.emplace_back("--noconfirm");
    if(args[9] == "1") projected.emplace_back("--needed");
    if(args[8] == "AsExplicit")
        projected.emplace_back("--asexplicit");
    else if(args[8] == "AsDependency")
        projected.emplace_back("--asdeps");
    else if(args[8] != "PreserveExistingReason")
        throw std::logic_error("Unknown legacy reason.");
    projected.emplace_back("--");
    for(std::size_t i = 12; i < args.size(); i += 9) {
        const auto& [path, version] = state.queried_archives.at(args[i + 1]);
        if(version != args[i + 2]) throw std::logic_error("Legacy metadata drift.");
        projected.push_back(path);
        for(int part = 0; part < 2; ++part) {
            const auto size = std::stoull(args[i + 5 + part]);
            if(size == 0) continue;
            std::ifstream original(path + (part ? ".sig" : ""), std::ios::binary);
            const std::string expected{std::istreambuf_iterator<char>(original), {}};
            std::string actual(size, '\0');
            bytes.read(actual.data(), size);
            if(!bytes || actual != expected) throw std::logic_error("Legacy bytes differ from the selected fixture.");
        }
    }
    if(bytes.peek() != std::char_traits<char>::eof()) throw std::logic_error("Unselected legacy bytes transmitted.");
    return shell_words::join(projected);
}

} // namespace

namespace artifact_install_executor_test_stub {

void reset_process_stub() {
    process_stub_state() = ProcessStubState{};
}

void expect_capture_command(
    std::string command, CapturedCommandResult result) {
    process_stub_state().expected_calls.push_back(ExpectedProcessCall{
        ExpectedProcessKind::Capture,
        std::move(command),
        std::move(result),
        0});
}

void expect_run_command(std::string command, int exit_code) {
    process_stub_state().expected_calls.push_back(ExpectedProcessCall{
        ExpectedProcessKind::Run,
        std::move(command),
        CapturedCommandResult{},
        exit_code});
}

void require_process_expectations_consumed() {
    const ProcessStubState& state = process_stub_state();
    if(state.expectation_failure != nullptr) {
        throw std::logic_error(state.expectation_failure);
    }
    if(!state.expected_calls.empty() &&
       state.expected_calls.front().kind == ExpectedProcessKind::Capture) {
        throw std::logic_error(
            "Artifact install process stub has unconsumed capture command expectations.");
    }
    if(!state.expected_calls.empty()) {
        throw std::logic_error(
            "Artifact install process stub has unconsumed run command expectations.");
    }
}

void set_capture_hook(void (*hook)()) {
    process_stub_state().capture_hook = hook;
}

void set_run_hook(void (*hook)()) {
    process_stub_state().run_hook = hook;
}

std::size_t capture_command_call_count() {
    return process_stub_state().capture_calls;
}

std::size_t run_command_call_count() {
    return process_stub_state().run_calls;
}

std::string last_captured_command() {
    return process_stub_state().last_captured_command;
}

std::string last_run_command() {
    return process_stub_state().last_run_command;
}

} // namespace artifact_install_executor_test_stub

CapturedCommandResult capture_command_output_raw(const char* command) {
    ProcessStubState& state = process_stub_state();
    ++state.capture_calls;
    state.last_captured_command = command == nullptr ? "" : command;

    if(state.expected_calls.empty() ||
       state.expected_calls.front().kind != ExpectedProcessKind::Capture) {
        fail_process_expectation(
            state,
            "Unexpected artifact install capture command with no pending expectation.");
    }
    if(state.last_captured_command !=
       state.expected_calls.front().command) {
        fail_process_expectation(
            state,
            "Artifact install capture command did not match the next expectation.");
    }

    ExpectedProcessCall expectation = std::move(state.expected_calls.front());
    state.expected_calls.pop_front();
    if(state.last_captured_command.starts_with("LC_ALL=C " + shell_words::join({"pacman", "-Qp"}) + " ")) {
        const auto query = words(state.last_captured_command);
        std::istringstream identity(expectation.capture_result.output);
        std::string name, version, extra;
        if((identity >> name >> version) && !(identity >> extra)) state.queried_archives[name] = {query.back(), version};
    }
    if(state.capture_hook != nullptr) state.capture_hook();
    return std::move(expectation.capture_result);
}

int run_command(const std::string& command) {
    ProcessStubState& state = process_stub_state();
    ++state.run_calls;
    state.last_run_command = legacy_transaction_projection(state, command);

    if(state.expected_calls.empty() ||
       state.expected_calls.front().kind != ExpectedProcessKind::Run) {
        fail_process_expectation(
            state,
            "Unexpected artifact install run command with no pending expectation.");
    }
    if(state.last_run_command != state.expected_calls.front().command) {
        fail_process_expectation(
            state,
            "Artifact install run command did not match the next expectation.");
    }

    ExpectedProcessCall expectation = std::move(state.expected_calls.front());
    state.expected_calls.pop_front();
    if(state.run_hook != nullptr) state.run_hook();
    return expectation.run_exit_code;
}

int run_command_with_parent_independent_lifetime_guard(
    const std::string& command,
    int,
    const std::string& display_command) {
    if(display_command.empty()) return run_command(command);

    CapturedCommandResult result =
        capture_command_output_raw(display_command.c_str());
    const std::size_t redirect = command.rfind(" > ");
    if(redirect == std::string::npos) {
        throw std::logic_error(
            "Guarded capture command has no output redirection.");
    }
    std::string output_path = command.substr(redirect + 3);
    if(output_path.size() < 2 || output_path.front() != '\'' ||
       output_path.back() != '\'') {
        throw std::logic_error(
            "Guarded capture command has an unsupported output path.");
    }
    output_path = output_path.substr(1, output_path.size() - 2);
    std::ofstream output(
        output_path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::logic_error(
            "Guarded capture stub could not open its output path.");
    }
    output.write(
        result.output.data(),
        static_cast<std::streamsize>(result.output.size()));
    output.close();
    if(!output) {
        throw std::logic_error(
            "Guarded capture stub could not write its output.");
    }
    return result.exit_code;
}
