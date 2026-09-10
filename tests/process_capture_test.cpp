#include "process.hpp"
#include "shell_words.hpp"

#include <charconv>
#include <csignal>
#include <cerrno>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view CHILD_MARKER = "--process-capture-child";

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

std::string repeated_output(std::size_t size) {
    std::string output(size, '\0');
    for(std::size_t i = 0; i < size; ++i) {
        output[i] = static_cast<char>('a' + (i % 26));
    }
    return output;
}

bool write_stdout(std::string_view output) {
    std::size_t offset = 0;
    while(offset < output.size()) {
        ssize_t written = write(
            STDOUT_FILENO,
            output.data() + offset,
            output.size() - offset);
        if(written == -1 && errno == EINTR) continue;
        if(written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::optional<std::string> read_stdin() {
    std::string input;
    char buffer[128];
    while(true) {
        const ssize_t bytes_read = read(STDIN_FILENO, buffer, sizeof(buffer));
        if(bytes_read > 0) {
            input.append(buffer, static_cast<std::size_t>(bytes_read));
            continue;
        }
        if(bytes_read == 0) return input;
        if(errno == EINTR) continue;
        return std::nullopt;
    }
}

#ifdef MOGUET_ENABLE_PROCESS_TEST_HOOKS
void throw_after_explicit_process_wait() {
    throw std::runtime_error("synthetic post-wait failure");
}
#endif

std::size_t parse_output_size(std::string_view value) {
    std::size_t output_size = 0;
    auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), output_size);
    if(error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid child output size");
    }
    return output_size;
}

int run_child(int argc, char* argv[]) {
    require(argc >= 3, "Missing child emitter mode");
    std::string_view mode = argv[2];

    if(mode == "empty") return 0;
    if(mode == "size") {
        require(argc == 4, "Missing child output size");
        return write_stdout(repeated_output(parse_output_size(argv[3]))) ? 0 : 125;
    }
    if(mode == "embedded-nul") {
        const std::string output{'b', 'e', 'f', 'o', 'r', 'e', '\0', 'a', 'f', 't', 'e', 'r'};
        return write_stdout(output) ? 0 : 125;
    }
    if(mode == "final-lf") return write_stdout("line\n") ? 0 : 125;
    if(mode == "no-final-lf") return write_stdout("line") ? 0 : 125;
    if(mode == "carriage-return") return write_stdout("left\rright") ? 0 : 125;
    if(mode == "trim-boundaries") return write_stdout(" \t\nbody\r\n ") ? 0 : 125;
    if(mode == "cwd") {
        const std::string directory = fs::current_path().string();
        return write_stdout(directory) ? 0 : 125;
    }
    if(mode == "stdin-echo") {
        const auto input = read_stdin();
        return input.has_value() && write_stdout(*input) ? 0 : 125;
    }
    if(mode == "exit-23") return 23;
    if(mode == "signal-term") {
        raise(SIGTERM);
        return 125;
    }

    throw std::runtime_error("Unknown child emitter mode");
}

std::string child_command(
    const fs::path& executable_path,
    const std::vector<std::string>& arguments) {
    std::vector<std::string> command_arguments{
        executable_path.string(), std::string(CHILD_MARKER)};
    command_arguments.insert(
        command_arguments.end(), arguments.begin(), arguments.end());

    // POLICY: shellをchild binary自身へ置換し、signal終了をshell由来のexit statusへ変換させない。
    return "exec " + shell_words::join(command_arguments);
}

void require_output_equal(
    const std::string& test_case,
    const std::string& actual,
    const std::string& expected) {
    if(actual == expected) return;

    std::size_t difference = 0;
    while(difference < actual.size() &&
          difference < expected.size() &&
          actual[difference] == expected[difference]) {
        ++difference;
    }
    throw std::runtime_error(
        test_case + ": output mismatch at byte " + std::to_string(difference) +
        " (expected size " + std::to_string(expected.size()) +
        ", actual size " + std::to_string(actual.size()) + ")");
}

void require_result(
    const std::string& test_case,
    const CapturedCommandResult& actual,
    const std::string& expected_output,
    int expected_exit_code) {
    require_output_equal(test_case, actual.output, expected_output);
    require(
        actual.exit_code == expected_exit_code,
        test_case + ": expected exit code " + std::to_string(expected_exit_code) +
            ", actual " + std::to_string(actual.exit_code));
}

CapturedCommandResult capture_raw(
    const fs::path& executable_path,
    const std::vector<std::string>& arguments) {
    std::string command = child_command(executable_path, arguments);
    return capture_command_output_raw(command.c_str());
}

CapturedCommandResult capture_trimmed(
    const fs::path& executable_path,
    const std::vector<std::string>& arguments) {
    std::string command = child_command(executable_path, arguments);
    return capture_command_output(command.c_str());
}

CapturedCommandResult capture_explicit(
    const fs::path& executable_path,
    const std::vector<std::string>& arguments,
    std::optional<std::size_t> stdout_capture_limit = std::nullopt) {
    std::vector<std::string> invocation_arguments{std::string(CHILD_MARKER)};
    invocation_arguments.insert(
        invocation_arguments.end(), arguments.begin(), arguments.end());
    return capture_explicit_process_output_raw(ExplicitProcessInvocation{
        executable_path.string(), std::move(invocation_arguments), {}, stdout_capture_limit});
}

void test_raw_chunk_boundaries(const fs::path& executable_path) {
    require_result("empty output", capture_raw(executable_path, {"empty"}), "", 0);

    for(std::size_t size : {127U, 128U, 129U, 256U}) {
        require_result(
            std::to_string(size) + " byte output",
            capture_raw(executable_path, {"size", std::to_string(size)}),
            repeated_output(size),
            0);
    }
}

void test_raw_byte_preservation(const fs::path& executable_path) {
    const std::string nul_output{
        'b', 'e', 'f', 'o', 'r', 'e', '\0', 'a', 'f', 't', 'e', 'r'};
    require_result(
        "embedded NUL tail",
        capture_raw(executable_path, {"embedded-nul"}),
        nul_output,
        0);
    require_result(
        "final LF",
        capture_raw(executable_path, {"final-lf"}),
        "line\n",
        0);
    require_result(
        "no final LF",
        capture_raw(executable_path, {"no-final-lf"}),
        "line",
        0);
    require_result(
        "carriage return",
        capture_raw(executable_path, {"carriage-return"}),
        "left\rright",
        0);
}

void test_trimmed_capture_compatibility(const fs::path& executable_path) {
    require_result(
        "trimmed capture",
        capture_trimmed(executable_path, {"trim-boundaries"}),
        "body",
        0);
}

void test_decoded_exit_status(const fs::path& executable_path) {
    require_result(
        "normal exit",
        capture_raw(executable_path, {"empty"}),
        "",
        0);
    require_result(
        "exit 23",
        capture_raw(executable_path, {"exit-23"}),
        "",
        23);
    require_result(
        "SIGTERM exit",
        capture_raw(executable_path, {"signal-term"}),
        "",
        128 + SIGTERM);
}

void test_bounded_explicit_capture(const fs::path& executable_path) {
    constexpr std::size_t LIMIT = 8192;

    CapturedCommandResult exact = capture_explicit(
        executable_path, {"size", std::to_string(LIMIT)}, LIMIT);
    require_result(
        "bounded exact limit", exact, repeated_output(LIMIT), 0);
    require(
        !exact.stdout_capture_limit_exceeded,
        "Exact-limit output was incorrectly reported as oversized");

    constexpr std::size_t OVERSIZED_OUTPUT_SIZE = LIMIT + 256 * 1024 + 1;
    CapturedCommandResult oversized = capture_explicit(
        executable_path,
        {"size", std::to_string(OVERSIZED_OUTPUT_SIZE)}, LIMIT);
    require_result(
        "bounded oversized output", oversized, repeated_output(LIMIT), 0);
    require(
        oversized.stdout_capture_limit_exceeded,
        "Oversized output did not set the capture limit flag");
    require(
        oversized.output.size() <= LIMIT,
        "Bounded capture retained bytes beyond its storage limit");

    CapturedCommandResult subsequent = capture_explicit(
        executable_path, {"no-final-lf"}, LIMIT);
    require_result("capture after overflow", subsequent, "line", 0);
    require(
        !subsequent.stdout_capture_limit_exceeded,
        "Capture after overflow inherited stale limit state");

    CapturedCommandResult signaled = capture_explicit(
        executable_path, {"signal-term"}, LIMIT);
    require_result(
        "bounded signal termination", signaled, "", 128 + SIGTERM);
    require(
        !signaled.stdout_capture_limit_exceeded,
        "Signal-terminated capture incorrectly reported overflow");

    errno = 0;
    require(
        waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD,
        "Bounded capture left a waitable child process");
}

void test_unbounded_explicit_capture_compatibility(
    const fs::path& executable_path) {
    constexpr std::size_t OUTPUT_SIZE = 12289;
    CapturedCommandResult result = capture_explicit(
        executable_path, {"size", std::to_string(OUTPUT_SIZE)});
    require_result(
        "unbounded explicit output", result,
        repeated_output(OUTPUT_SIZE), 0);
    require(
        !result.stdout_capture_limit_exceeded,
        "Unbounded capture reported a storage limit overflow");
}

void test_explicit_working_directory_descriptor(
    const fs::path& executable_path) {
    const int directory_descriptor =
        open("/tmp", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    require(
        directory_descriptor >= 0,
        "Failed to open explicit-process working directory");

    CapturedCommandResult result = capture_explicit_process_output_raw(
        ExplicitProcessInvocation{
            executable_path.string(),
            {std::string(CHILD_MARKER), "cwd"},
            {},
            std::nullopt,
            directory_descriptor});
    const int close_status = close(directory_descriptor);
    require(close_status == 0, "Failed to close working directory descriptor");
    require_result(
        "explicit working directory descriptor", result,
        fs::canonical("/tmp").string(), 0);
}

void test_explicit_standard_input_descriptor(
    const fs::path& executable_path) {
    int descriptors[2] = {-1, -1};
    require(pipe2(descriptors, O_CLOEXEC) == 0,
            "Failed to create explicit-process stdin pipe");
    const std::string input{'b', 'e', 'f', 'o', 'r', 'e', '\0', 'a', 'f', 't', 'e', 'r'};
    std::size_t input_offset = 0;
    while(input_offset < input.size()) {
        const ssize_t written = write(
            descriptors[1], input.data() + input_offset,
            input.size() - input_offset);
        if(written == -1 && errno == EINTR) continue;
        require(written > 0,
                "Failed to write explicit-process stdin fixture");
        input_offset += static_cast<std::size_t>(written);
    }
    require(close(descriptors[1]) == 0,
            "Failed to close explicit-process stdin writer");

    CapturedCommandResult result = capture_explicit_process_output_raw(
        ExplicitProcessInvocation{
            executable_path.string(),
            {std::string(CHILD_MARKER), "stdin-echo"},
            {},
            std::nullopt,
            std::nullopt,
            descriptors[0]});
    require(close(descriptors[0]) == 0,
            "Explicit process closed the caller-owned stdin descriptor");
    require_result(
        "explicit standard input descriptor", result, input, 0);
}

void test_explicit_process_launch_outcome(
    const fs::path& executable_path) {
    ExplicitProcessInvocation invocation;
    invocation.executable = executable_path.string();
    invocation.arguments = {
        std::string(CHILD_MARKER), "exit-23"};

    const ExplicitProcessExecutionResult known =
        run_explicit_process_with_outcome(invocation);
    require(
        known.status ==
                ExplicitProcessExecutionStatus::StartedKnownOutcome &&
            known.exit_code == std::optional<int>{23},
        "completed explicit process did not preserve its known outcome");

#ifdef MOGUET_ENABLE_PROCESS_TEST_HOOKS
    set_explicit_process_post_wait_hook_for_test(
        throw_after_explicit_process_wait);
    const ExplicitProcessExecutionResult post_wait_failure =
        run_explicit_process_with_outcome(invocation);
    set_explicit_process_post_wait_hook_for_test(nullptr);
    require(
        post_wait_failure.status ==
                ExplicitProcessExecutionStatus::StartedOutcomeUnknown &&
            !post_wait_failure.exit_code.has_value(),
        "post-wait exception was flattened into a pre-launch failure");
#endif

    const ExplicitProcessExecutionResult not_started =
        run_explicit_process_with_outcome(ExplicitProcessInvocation{});
    require(
        not_started.status ==
                ExplicitProcessExecutionStatus::NotStarted &&
            !not_started.exit_code.has_value(),
        "invalid pre-launch invocation did not remain NotStarted");
}

void test_explicit_retained_executable_identity(
    const fs::path& executable_path) {
    const int executable_descriptor = open(
        executable_path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    require(executable_descriptor >= 0,
            "Failed to retain explicit executable identity");
    ExplicitProcessInvocation invocation{
        "/path/replaced/after-retention",
        {std::string(CHILD_MARKER), "final-lf"},
        {}};
    invocation.executable_fd = executable_descriptor;
    CapturedCommandResult result =
        capture_explicit_process_output_raw(invocation);
    require(close(executable_descriptor) == 0,
            "Explicit process closed the caller-owned executable descriptor");
    require_result(
        "explicit retained executable identity", result, "line\n", 0);
}

void test_retained_executable_stdio_aliases() {
    for(const bool bounded : {false, true}) {
        for(const int executable_slot : {0, 1, 2, 3}) {
            for(const bool aliases_stdin : {false, true}) {
                // Isolate the fixture's stdio mutation from the test runner.
                const pid_t child = fork();
                require(child >= 0, "Failed to fork retained FD alias fixture");
                if(child == 0) {
                    const int retained = open("/usr/bin/false", O_RDONLY | O_CLOEXEC);
                    const int input = open("/usr/bin/true", O_RDONLY | O_CLOEXEC);
                    const int cwd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                    if(retained < 0 || input < 0 || cwd < 0) _exit(70);
                    const int slot = executable_slot == 3 ? retained : executable_slot;
                    if(dup2(retained, slot) < 0) _exit(71);
                    ExplicitProcessInvocation invocation{
                        "/usr/bin/true", {}, {"PATH=/usr/bin:/bin"}, 4096};
                    invocation.executable_fd = slot;
                    invocation.standard_input_fd = aliases_stdin ? slot : input;
                    invocation.working_directory_fd = cwd;
                    if(bounded) {
                        invocation.stdout_capture_limit.reset();
                        const auto result = capture_bounded_explicit_process_output_raw(
                            invocation, BoundedProcessPolicy{
                                            std::chrono::seconds(5), std::chrono::milliseconds(100), 4096, true});
                        const auto* exited = std::get_if<BoundedProcessExited>(&result.outcome);
                        _exit(exited != nullptr && exited->exit_code == 1 ? 0 : 72);
                    }
                    const auto result = capture_explicit_process_output_raw(invocation, true);
                    _exit(result.exit_code == 1 ? 0 : 73);
                }
                int status = 0;
                require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                            WEXITSTATUS(status) == 0,
                        "Retained /usr/bin/false was replaced by stdio binding: bounded=" +
                            std::to_string(bounded) + " fd=" + std::to_string(executable_slot) +
                            " stdin_alias=" + std::to_string(aliases_stdin));
            }
        }
    }
}

void run_tests(const fs::path& executable_path) {
    test_raw_chunk_boundaries(executable_path);
    test_raw_byte_preservation(executable_path);
    test_trimmed_capture_compatibility(executable_path);
    test_decoded_exit_status(executable_path);
    test_bounded_explicit_capture(executable_path);
    test_unbounded_explicit_capture_compatibility(executable_path);
    test_explicit_working_directory_descriptor(executable_path);
    test_explicit_standard_input_descriptor(executable_path);
    test_explicit_process_launch_outcome(executable_path);
    test_explicit_retained_executable_identity(executable_path);
    test_retained_executable_stdio_aliases();
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if(argc >= 2 && std::string_view(argv[1]) == CHILD_MARKER) {
            return run_child(argc, argv);
        }

        run_tests(fs::canonical("/proc/self/exe"));
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "process capture tests: all checks passed\n";
    return 0;
}
