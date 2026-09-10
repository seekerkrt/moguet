#include "process.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;

constexpr std::string_view CHILD_MARKER = "--bounded-process-child";
constexpr std::size_t DEFAULT_CAPTURE_LIMIT = 4096;

void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

class OwnedDescriptor final {
public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

    ~OwnedDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

class TemporaryTree final {
public:
    TemporaryTree() {
        std::string path_template = "/tmp/moguet-bounded-process-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        require(created != nullptr, "Failed to create bounded-process fixture");
        path_ = created;
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    ~TemporaryTree() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

bool write_all(int descriptor, std::string_view bytes) {
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const ssize_t written = write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if(written == -1 && errno == EINTR) continue;
        if(written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

std::size_t parse_size(std::string_view value) {
    std::size_t parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    require(
        error == std::errc{} && end == value.data() + value.size(),
        "Invalid bounded-process size fixture");
    return parsed;
}

int parse_int(std::string_view value) {
    int parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    require(
        error == std::errc{} && end == value.data() + value.size(),
        "Invalid bounded-process integer fixture");
    return parsed;
}

void install_ignored_signal(int signal_number) {
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    require(sigemptyset(&action.sa_mask) == 0,
            "Failed to initialize ignored child signal");
    require(sigaction(signal_number, &action, nullptr) == 0,
            "Failed to install ignored child signal");
}

void write_pid_marker(const fs::path& marker) {
    const int descriptor = open(
        marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600);
    require(descriptor >= 0, "Failed to create child PID marker");
    OwnedDescriptor owned(descriptor);
    require(
        write_all(descriptor, std::to_string(getpid()) + "\n"),
        "Failed to write child PID marker");
}

[[noreturn]] void pause_forever() {
    while(true)
        pause();
}

int run_child(int argc, char* argv[]) {
    require(argc >= 3, "Missing bounded-process child mode");
    const std::string_view mode(argv[2]);
    if(mode == "exit") {
        require(argc == 4, "Missing bounded-process exit status");
        return parse_int(argv[3]);
    }
    if(mode == "signal-term") {
        raise(SIGTERM);
        return 125;
    }
    if(mode == "stdout") {
        require(argc == 4, "Missing bounded-process stdout size");
        const std::string bytes(parse_size(argv[3]), 'x');
        return write_all(STDOUT_FILENO, bytes) ? 0 : 125;
    }
    if(mode == "stdout-ignore-term") {
        require(argc == 4, "Missing bounded-process overflow size");
        install_ignored_signal(SIGTERM);
        const std::string bytes(parse_size(argv[3]), 'y');
        require(write_all(STDOUT_FILENO, bytes),
                "Failed to write overflow fixture");
        pause_forever();
    }
    if(mode == "stdin-eof") {
        char byte = '\0';
        ssize_t bytes_read;
        do {
            bytes_read = read(STDIN_FILENO, &byte, 1);
        } while(bytes_read == -1 && errno == EINTR);
        return bytes_read == 0 && write_all(STDOUT_FILENO, "eof\n")
                   ? 0
                   : 125;
    }
    if(mode == "slow-output") {
        for(int index = 0; index < 40; ++index) {
            require(write_all(STDOUT_FILENO, "z"),
                    "Failed to write slow stdout fixture");
            std::this_thread::sleep_for(50ms);
        }
        return 0;
    }
    if(mode == "hang" || mode == "hang-ignore-term") {
        if(mode == "hang-ignore-term") install_ignored_signal(SIGTERM);
        require(
            write_all(
                STDOUT_FILENO,
                "root=" + std::to_string(getpid()) + "\n"),
            "Failed to write hanging child PID");
        pause_forever();
    }
    if(mode == "grandchild-holds-stdout") {
        const pid_t grandchild = fork();
        require(grandchild >= 0, "Failed to fork held-pipe grandchild");
        if(grandchild == 0) {
            install_ignored_signal(SIGTERM);
            pause_forever();
        }
        require(
            write_all(
                STDOUT_FILENO,
                "root=" + std::to_string(getpid()) + "\n" +
                    "grandchild=" + std::to_string(grandchild) + "\n"),
            "Failed to write held-pipe process identities");
        return 0;
    }
    if(mode == "fd-closed") {
        require(argc == 4, "Missing inherited descriptor number");
        const int descriptor = parse_int(argv[3]);
        errno = 0;
        const int flags = fcntl(descriptor, F_GETFD);
        return flags == -1 && errno == EBADF &&
                       write_all(STDOUT_FILENO, "closed\n")
                   ? 0
                   : 125;
    }
    if(mode == "marker-hang") {
        require(argc == 4, "Missing child marker path");
        write_pid_marker(argv[3]);
        pause_forever();
    }
    throw std::runtime_error("Unknown bounded-process child mode");
}

BoundedProcessPolicy normal_policy(
    std::size_t capture_limit = DEFAULT_CAPTURE_LIMIT) {
    return BoundedProcessPolicy{2s, 150ms, capture_limit, true};
}

BoundedProcessPolicy timeout_policy() {
    return BoundedProcessPolicy{500ms, 200ms, DEFAULT_CAPTURE_LIMIT, true};
}

BoundedCapturedProcessResult run_bounded(
    const fs::path& executable,
    const std::vector<std::string>& child_arguments,
    const BoundedProcessPolicy& policy,
    std::optional<int> working_directory_override = std::nullopt) {
    const int directory_descriptor = working_directory_override.has_value()
                                         ? *working_directory_override
                                         : open(
                                               "/", O_RDONLY | O_DIRECTORY |
                                                        O_CLOEXEC |
                                                        O_NOFOLLOW);
    require(directory_descriptor >= 0,
            "Failed to open bounded-process cwd fixture");
    OwnedDescriptor directory_owner(
        working_directory_override.has_value() ? -1
                                               : directory_descriptor);
    const int input_descriptor = open(
        "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(input_descriptor >= 0,
            "Failed to open bounded-process stdin fixture");
    OwnedDescriptor input(input_descriptor);

    std::vector<std::string> arguments{std::string(CHILD_MARKER)};
    arguments.insert(
        arguments.end(), child_arguments.begin(), child_arguments.end());
    ExplicitProcessInvocation invocation{
        executable.string(), std::move(arguments), {"PATH=/usr/bin:/bin", "LC_ALL=C", "LANG=C"}};
    invocation.working_directory_fd = directory_descriptor;
    invocation.standard_input_fd = input.get();
    return capture_bounded_explicit_process_output_raw(
        invocation, policy);
}

template <typename Outcome>
const Outcome& require_outcome(
    const BoundedCapturedProcessResult& result,
    std::string_view test_case) {
    const Outcome* outcome = std::get_if<Outcome>(&result.outcome);
    require(
        outcome != nullptr,
        std::string(test_case) + ": unexpected process outcome");
    return *outcome;
}

pid_t parse_named_pid(
    const std::string& output, std::string_view name) {
    const std::string prefix = std::string(name) + "=";
    const std::size_t start = output.find(prefix);
    require(start != std::string::npos,
            "Missing process identity " + std::string(name));
    const std::size_t value_start = start + prefix.size();
    const std::size_t end = output.find('\n', value_start);
    require(end != std::string::npos,
            "Unterminated process identity " + std::string(name));
    return static_cast<pid_t>(parse_int(
        std::string_view(output).substr(value_start, end - value_start)));
}

void wait_for_path(const fs::path& path) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while(!fs::exists(path)) {
        require(
            std::chrono::steady_clock::now() < deadline,
            "Timed out waiting for child handshake");
        std::this_thread::sleep_for(10ms);
    }
}

pid_t read_pid_file(const fs::path& path) {
    std::ifstream input(path);
    std::string line;
    require(static_cast<bool>(std::getline(input, line)),
            "Failed to read child PID marker");
    return static_cast<pid_t>(parse_int(line));
}

void wait_for_process_absence(pid_t pid, bool process_group) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    const pid_t target = process_group ? -pid : pid;
    while(true) {
        errno = 0;
        if(kill(target, 0) == -1 && errno == ESRCH) return;
        require(
            std::chrono::steady_clock::now() < deadline,
            "Process fixture remained alive after bounded return");
        std::this_thread::sleep_for(10ms);
    }
}

void test_normal_outcomes(const fs::path& executable) {
    auto normal = run_bounded(executable, {"exit", "0"}, normal_policy());
    require(normal.output.empty(), "Normal exit produced unexpected output");
    require(
        require_outcome<BoundedProcessExited>(normal, "normal exit")
                .exit_code == 0,
        "Normal exit status changed");

    auto nonzero =
        run_bounded(executable, {"exit", "37"}, normal_policy());
    require(
        require_outcome<BoundedProcessExited>(nonzero, "nonzero exit")
                .exit_code == 37,
        "Nonzero exit status changed");

    auto exit_127 =
        run_bounded(executable, {"exit", "127"}, normal_policy());
    require(
        require_outcome<BoundedProcessExited>(exit_127, "exit 127")
                .exit_code == 127,
        "Executable exit 127 was not preserved");

    auto signaled =
        run_bounded(executable, {"signal-term"}, normal_policy());
    require(
        require_outcome<BoundedProcessSignaled>(signaled, "signal exit")
                .signal_number == SIGTERM,
        "Signal exit was not preserved");
}

void test_parent_process_state_restoration(const fs::path& executable) {
    sigset_t before_mask;
    require(sigprocmask(SIG_SETMASK, nullptr, &before_mask) == 0,
            "Failed to inspect parent signal mask");
    int before_subreaper = 0;
    require(prctl(PR_GET_CHILD_SUBREAPER, &before_subreaper, 0, 0, 0) == 0,
            "Failed to inspect parent subreaper state");

    auto result = run_bounded(
        executable, {"exit", "0"}, normal_policy());
    require_outcome<BoundedProcessExited>(
        result, "parent process state restoration");

    sigset_t after_mask;
    require(sigprocmask(SIG_SETMASK, nullptr, &after_mask) == 0,
            "Failed to re-inspect parent signal mask");
    for(int signal_number = 1; signal_number < NSIG; ++signal_number) {
        require(sigismember(&before_mask, signal_number) ==
                    sigismember(&after_mask, signal_number),
                "Bounded process changed the parent signal mask");
    }
    int after_subreaper = 0;
    require(prctl(PR_GET_CHILD_SUBREAPER, &after_subreaper, 0, 0, 0) == 0,
            "Failed to re-inspect parent subreaper state");
    require(after_subreaper == before_subreaper,
            "Bounded process changed the parent subreaper state");
}

void test_capture_and_stdin(const fs::path& executable) {
    auto exact = run_bounded(
        executable, {"stdout", "64"}, normal_policy(64));
    require(exact.output == std::string(64, 'x'),
            "Exact capture boundary changed bytes");
    require_outcome<BoundedProcessExited>(exact, "exact capture");

    auto empty =
        run_bounded(executable, {"exit", "0"}, normal_policy(0));
    require(empty.output.empty(), "Zero-limit empty output failed");
    require_outcome<BoundedProcessExited>(empty, "zero-limit empty output");

    auto stdin_eof =
        run_bounded(executable, {"stdin-eof"}, normal_policy());
    require(stdin_eof.output == "eof\n",
            "Bounded child did not receive /dev/null EOF");
    require_outcome<BoundedProcessExited>(stdin_eof, "stdin EOF");

    auto overflow = run_bounded(
        executable, {"stdout-ignore-term", "65"},
        normal_policy(64));
    require(overflow.output == std::string(64, 'y'),
            "Overflow retained bytes beyond the limit");
    require(
        require_outcome<BoundedProcessCaptureLimitExceeded>(
            overflow, "capture overflow")
                .capture_limit == 64,
        "Capture overflow did not retain its configured limit");

    // Overflow is observed before this short hard deadline. Its TERM grace
    // intentionally crosses that deadline; classification must stay fixed.
    auto overflow_race = run_bounded(
        executable, {"stdout-ignore-term", "65"},
        BoundedProcessPolicy{250ms, 400ms, 64, true});
    require_outcome<BoundedProcessCaptureLimitExceeded>(
        overflow_race, "overflow/timeout ordering");
}

void test_timeout_escalation(const fs::path& executable) {
    const pid_t parent_group = getpgrp();
    const auto slow_started = std::chrono::steady_clock::now();
    auto slow_output =
        run_bounded(executable, {"slow-output"}, timeout_policy());
    const auto slow_elapsed =
        std::chrono::steady_clock::now() - slow_started;
    require_outcome<BoundedProcessTimedOut>(
        slow_output, "monotonic stdout timeout");
    require(!slow_output.output.empty(),
            "Slow stdout fixture produced no captured bytes");
    require(slow_elapsed < 1500ms,
            "Stdout activity extended the absolute hard deadline");

    auto term_exit =
        run_bounded(executable, {"hang"}, timeout_policy());
    require_outcome<BoundedProcessTimedOut>(term_exit, "timeout TERM exit");
    const pid_t term_root = parse_named_pid(term_exit.output, "root");
    wait_for_process_absence(term_root, true);

    const auto started = std::chrono::steady_clock::now();
    auto kill_exit = run_bounded(
        executable, {"hang-ignore-term"}, timeout_policy());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require_outcome<BoundedProcessTimedOut>(kill_exit, "timeout KILL exit");
    require(elapsed < 3s, "TERM-ignore timeout was not bounded");
    const pid_t kill_root = parse_named_pid(kill_exit.output, "root");
    wait_for_process_absence(kill_root, true);
    require(getpgrp() == parent_group,
            "Bounded child process group captured the parent");
}

void test_descendant_cleanup(const fs::path& executable) {
    const auto started = std::chrono::steady_clock::now();
    auto result = run_bounded(
        executable, {"grandchild-holds-stdout"}, normal_policy());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(
        require_outcome<BoundedProcessExited>(
            result, "held-pipe descendant")
                .exit_code == 0,
        "Root outcome was lost during descendant cleanup");
    require(elapsed < 3s, "Held-open stdout pipe caused an unbounded wait");
    const pid_t root = parse_named_pid(result.output, "root");
    const pid_t grandchild = parse_named_pid(result.output, "grandchild");
    wait_for_process_absence(root, true);
    wait_for_process_absence(grandchild, false);
}

void test_launch_setup_and_fd_hygiene(
    const fs::path& executable,
    const fs::path& fixture_root) {
    const int input_descriptor = open(
        "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(input_descriptor >= 0, "Failed to open setup fixture input");
    OwnedDescriptor input(input_descriptor);
    const int directory_descriptor = open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(directory_descriptor >= 0, "Failed to open setup fixture cwd");
    OwnedDescriptor directory(directory_descriptor);

    ExplicitProcessInvocation missing_exec{
        "/definitely/not/a/moguet-executable", {}, {"LC_ALL=C"}};
    missing_exec.working_directory_fd = directory.get();
    missing_exec.standard_input_fd = input.get();
    auto exec_failure = capture_bounded_explicit_process_output_raw(
        missing_exec, normal_policy());
    const auto& exec_outcome =
        require_outcome<BoundedProcessLaunchOrSetupFailure>(
            exec_failure, "exec failure");
    require(
        exec_outcome.stage == BoundedProcessLaunchStage::Execve &&
            exec_outcome.error_number == ENOENT,
        "Exec failure was not separated from executable exit 127");

    const fs::path regular_path = fixture_root / "not-a-directory";
    {
        std::ofstream regular(regular_path);
        regular << "fixture";
    }
    const int regular_descriptor = open(
        regular_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(regular_descriptor >= 0, "Failed to open setup failure fixture");
    OwnedDescriptor regular(regular_descriptor);
    auto setup_failure = run_bounded(
        executable, {"exit", "0"}, normal_policy(), regular.get());
    const auto& setup_outcome =
        require_outcome<BoundedProcessLaunchOrSetupFailure>(
            setup_failure, "setup failure");
    require(
        setup_outcome.stage ==
                BoundedProcessLaunchStage::WorkingDirectory &&
            setup_outcome.error_number == ENOTDIR,
        "Child setup failure stage was not preserved");

    const int inherited_descriptor = open("/dev/null", O_RDONLY);
    require(inherited_descriptor > STDERR_FILENO,
            "Failed to allocate inherited descriptor fixture");
    OwnedDescriptor inherited(inherited_descriptor);
    auto fd_result = run_bounded(
        executable,
        {"fd-closed", std::to_string(inherited_descriptor)},
        normal_policy());
    require(fd_result.output == "closed\n",
            "Unexpected descriptor leaked across bounded exec");
    require_outcome<BoundedProcessExited>(fd_result, "descriptor hygiene");
}

void test_retained_executable_identity(const fs::path& executable) {
    const int executable_descriptor = open(
        executable.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    require(executable_descriptor >= 0,
            "Failed to retain executable identity fixture");
    OwnedDescriptor retained_executable(executable_descriptor);
    const int directory_descriptor = open(
        "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    require(directory_descriptor >= 0,
            "Failed to open retained-executable cwd");
    OwnedDescriptor directory(directory_descriptor);
    const int input_descriptor = open(
        "/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    require(input_descriptor >= 0,
            "Failed to open retained-executable stdin");
    OwnedDescriptor input(input_descriptor);

    ExplicitProcessInvocation invocation{
        "/path/replaced/after-retention",
        {std::string(CHILD_MARKER), "fd-closed",
         std::to_string(executable_descriptor)},
        {"PATH=/usr/bin:/bin", "LC_ALL=C", "LANG=C"}};
    invocation.working_directory_fd = directory.get();
    invocation.standard_input_fd = input.get();
    invocation.executable_fd = executable_descriptor;
    BoundedCapturedProcessResult result =
        capture_bounded_explicit_process_output_raw(
            invocation, normal_policy());
    require(result.output == "closed\n",
            "Retained executable descriptor leaked after execveat");
    require(
        require_outcome<BoundedProcessExited>(
            result, "retained executable identity")
                .exit_code == 0,
        "Bounded process resolved the replaced pathname instead of the retained executable");
}

struct WorkerResult {
    int outcome_kind;
    int detail;
};

void test_signal_forwarding(
    const fs::path& executable,
    const fs::path& fixture_root) {
    const fs::path marker = fixture_root / "signal-forwarding-ready";
    int result_pipe[2] = {-1, -1};
    require(pipe2(result_pipe, O_CLOEXEC) == 0,
            "Failed to create signal forwarding result pipe");
    OwnedDescriptor result_read(result_pipe[0]);

    const pid_t worker = fork();
    require(worker >= 0, "Failed to fork signal forwarding worker");
    if(worker == 0) {
        static_cast<void>(close(result_pipe[0]));
        auto result = run_bounded(
            executable, {"marker-hang", marker.string()},
            BoundedProcessPolicy{5s, 200ms, DEFAULT_CAPTURE_LIMIT, true});
        WorkerResult encoded{-1, 0};
        if(const auto* signaled =
               std::get_if<BoundedProcessSignaled>(&result.outcome)) {
            encoded = WorkerResult{1, signaled->signal_number};
        }
        const bool reported = write_all(
            result_pipe[1],
            std::string_view(
                static_cast<const char*>(static_cast<const void*>(&encoded)),
                sizeof(encoded)));
        static_cast<void>(close(result_pipe[1]));
        _exit(reported ? 0 : 125);
    }
    static_cast<void>(close(result_pipe[1]));
    wait_for_path(marker);
    const pid_t child = read_pid_file(marker);
    require(kill(worker, SIGINT) == 0,
            "Failed to send SIGINT to bounded-process worker");

    WorkerResult encoded{};
    std::size_t offset = 0;
    while(offset < sizeof(encoded)) {
        const ssize_t bytes_read = read(
            result_read.get(),
            static_cast<char*>(static_cast<void*>(&encoded)) + offset,
            sizeof(encoded) - offset);
        if(bytes_read == -1 && errno == EINTR) continue;
        require(bytes_read > 0,
                "Signal forwarding worker did not report an outcome");
        offset += static_cast<std::size_t>(bytes_read);
    }
    int worker_status = 0;
    require(waitpid(worker, &worker_status, 0) == worker,
            "Failed to reap signal forwarding worker");
    require(WIFEXITED(worker_status) && WEXITSTATUS(worker_status) == 0,
            "Signal forwarding worker failed");
    require(encoded.outcome_kind == 1 && encoded.detail == SIGINT,
            "SIGINT did not reach the dedicated child group");
    wait_for_process_absence(child, true);
}

void test_parent_death_root_ownership(
    const fs::path& executable,
    const fs::path& fixture_root) {
    const fs::path marker = fixture_root / "parent-death-ready";
    const pid_t worker = fork();
    require(worker >= 0, "Failed to fork parent-death worker");
    if(worker == 0) {
        static_cast<void>(run_bounded(
            executable, {"marker-hang", marker.string()},
            BoundedProcessPolicy{30s, 200ms, DEFAULT_CAPTURE_LIMIT, true}));
        _exit(125);
    }
    wait_for_path(marker);
    const pid_t child = read_pid_file(marker);
    require(kill(worker, SIGKILL) == 0,
            "Failed to terminate parent-death worker");
    int worker_status = 0;
    require(waitpid(worker, &worker_status, 0) == worker,
            "Failed to reap parent-death worker");
    require(WIFSIGNALED(worker_status) && WTERMSIG(worker_status) == SIGKILL,
            "Parent-death worker did not receive SIGKILL");
    wait_for_process_absence(child, true);
}

void require_no_waitable_children() {
    errno = 0;
    require(
        waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD,
        "Bounded process tests left a waitable child");
}

void run_tests(const fs::path& executable) {
    TemporaryTree fixture;
    test_normal_outcomes(executable);
    test_parent_process_state_restoration(executable);
    test_capture_and_stdin(executable);
    test_timeout_escalation(executable);
    test_descendant_cleanup(executable);
    test_launch_setup_and_fd_hygiene(executable, fixture.path());
    test_retained_executable_identity(executable);
    test_signal_forwarding(executable, fixture.path());
    test_parent_death_root_ownership(executable, fixture.path());
    require_no_waitable_children();
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
    std::cout << "bounded process tests: all checks passed\n";
    return 0;
}
