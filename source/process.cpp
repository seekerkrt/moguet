#include "process.hpp"

#include "logging.hpp"
#include "localization.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <poll.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace {

#ifdef MOGUET_ENABLE_PROCESS_TEST_HOOKS
ExplicitProcessPostWaitHookForTest g_explicit_process_post_wait_hook =
    nullptr;
#endif

struct ExplicitProcessExecutionObservation {
    bool child_started = false;
    bool outcome_known = false;
};

struct CommandSignalState {
    struct sigaction original_sigint{};
    struct sigaction original_sigquit{};
    sigset_t original_mask{};
    bool sigint_changed = false;
    bool sigquit_changed = false;
    bool mask_changed = false;
};

enum class SignalRestoreTarget {
    SigintDisposition,
    SigquitDisposition,
    SignalMask
};

enum class SignalRestoreContext {
    CommandSetupRollback,
    ExplicitProcessForkFailure,
    ExplicitProcessWait,
    StdinFdCommandForkFailure,
    StdinFdCommandWait
};

enum class CommandSignalContext {
    ExplicitProcess,
    StdinFdCommand
};

std::string signal_restore_failure_message(
    SignalRestoreTarget target, SignalRestoreContext context,
    int error_number) {
    const std::string_view error = std::strerror(error_number);
    switch(context) {
        case SignalRestoreContext::CommandSetupRollback:
            switch(target) {
                case SignalRestoreTarget::SigintDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition during command setup rollback: {}",
                        "SIGINT", error);
                case SignalRestoreTarget::SigquitDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition during command setup rollback: {}",
                        "SIGQUIT", error);
                case SignalRestoreTarget::SignalMask:
                    return localization::format_translated_message(
                        "Failed to restore the signal mask during command setup rollback: {}",
                        error);
            }
            break;
        case SignalRestoreContext::ExplicitProcessForkFailure:
            switch(target) {
                case SignalRestoreTarget::SigintDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after an explicit-process fork failure: {}",
                        "SIGINT", error);
                case SignalRestoreTarget::SigquitDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after an explicit-process fork failure: {}",
                        "SIGQUIT", error);
                case SignalRestoreTarget::SignalMask:
                    return localization::format_translated_message(
                        "Failed to restore the signal mask after an explicit-process fork failure: {}",
                        error);
            }
            break;
        case SignalRestoreContext::ExplicitProcessWait:
            switch(target) {
                case SignalRestoreTarget::SigintDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after waiting for an explicit process: {}",
                        "SIGINT", error);
                case SignalRestoreTarget::SigquitDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after waiting for an explicit process: {}",
                        "SIGQUIT", error);
                case SignalRestoreTarget::SignalMask:
                    return localization::format_translated_message(
                        "Failed to restore the signal mask after waiting for an explicit process: {}",
                        error);
            }
            break;
        case SignalRestoreContext::StdinFdCommandForkFailure:
            switch(target) {
                case SignalRestoreTarget::SigintDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after a {}-file-descriptor command fork failure: {}",
                        "SIGINT", "stdin", error);
                case SignalRestoreTarget::SigquitDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after a {}-file-descriptor command fork failure: {}",
                        "SIGQUIT", "stdin", error);
                case SignalRestoreTarget::SignalMask:
                    return localization::format_translated_message(
                        "Failed to restore the signal mask after a {}-file-descriptor command fork failure: {}",
                        "stdin", error);
            }
            break;
        case SignalRestoreContext::StdinFdCommandWait:
            switch(target) {
                case SignalRestoreTarget::SigintDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after waiting for a {}-file-descriptor command: {}",
                        "SIGINT", "stdin", error);
                case SignalRestoreTarget::SigquitDisposition:
                    return localization::format_translated_message(
                        "Failed to restore the {} disposition after waiting for a {}-file-descriptor command: {}",
                        "SIGQUIT", "stdin", error);
                case SignalRestoreTarget::SignalMask:
                    return localization::format_translated_message(
                        "Failed to restore the signal mask after waiting for a {}-file-descriptor command: {}",
                        "stdin", error);
            }
            break;
    }
    return localization::translate_message(
        "Failed to restore an unknown process signal state.");
}

void log_signal_setup_error(
    CommandSignalContext context, std::string_view action,
    int error_number) {
    const std::string_view error = std::strerror(error_number);
    if(context == CommandSignalContext::ExplicitProcess) {
        if(action == "SIGINT") {
            Logger::error(localization::format_translated_message(
                "Failed to ignore {} while waiting for an explicit process: {}",
                action, error));
        } else if(action == "SIGQUIT") {
            Logger::error(localization::format_translated_message(
                "Failed to ignore {} while waiting for an explicit process: {}",
                action, error));
        } else {
            Logger::error(localization::format_translated_message(
                "Failed to block {} while waiting for an explicit process: {}",
                action, error));
        }
        return;
    }
    if(action == "SIGINT") {
        Logger::error(localization::format_translated_message(
            "Failed to ignore {} while waiting for a {}-file-descriptor command: {}",
            action, "stdin", error));
    } else if(action == "SIGQUIT") {
        Logger::error(localization::format_translated_message(
            "Failed to ignore {} while waiting for a {}-file-descriptor command: {}",
            action, "stdin", error));
    } else {
        Logger::error(localization::format_translated_message(
            "Failed to block {} while waiting for a {}-file-descriptor command: {}",
            action, "stdin", error));
    }
}

bool restore_parent_signal_state(
    const CommandSignalState& state,
    SignalRestoreContext context) {
    int sigint_restore_error = 0;
    int sigquit_restore_error = 0;
    int mask_restore_error = 0;
    if(state.sigint_changed && sigaction(SIGINT, &state.original_sigint, nullptr) == -1) {
        sigint_restore_error = errno;
    }
    if(state.sigquit_changed && sigaction(SIGQUIT, &state.original_sigquit, nullptr) == -1) {
        sigquit_restore_error = errno;
    }
    if(state.mask_changed && sigprocmask(SIG_SETMASK, &state.original_mask, nullptr) == -1) {
        mask_restore_error = errno;
    }

    // 復元処理をすべて試した後でloggingし、先の失敗で後続の復元を飛ばさない。
    if(sigint_restore_error != 0) {
        Logger::error(signal_restore_failure_message(
            SignalRestoreTarget::SigintDisposition, context,
            sigint_restore_error));
    }
    if(sigquit_restore_error != 0) {
        Logger::error(signal_restore_failure_message(
            SignalRestoreTarget::SigquitDisposition, context,
            sigquit_restore_error));
    }
    if(mask_restore_error != 0) {
        Logger::error(signal_restore_failure_message(
            SignalRestoreTarget::SignalMask, context,
            mask_restore_error));
    }
    return sigint_restore_error == 0 && sigquit_restore_error == 0 && mask_restore_error == 0;
}

bool setup_command_signal_state(
    CommandSignalState& state,
    CommandSignalContext command_context) {
    struct sigaction ignore_action{};
    ignore_action.sa_handler = SIG_IGN;
    if(sigemptyset(&ignore_action.sa_mask) == -1) {
        int setup_error = errno;
        Logger::error(localization::format_translated_message(
            "Failed to initialize the ignored signal disposition: {}",
            std::string_view(std::strerror(setup_error))));
        return false;
    }

    sigset_t sigchld_mask;
    if(sigemptyset(&sigchld_mask) == -1 || sigaddset(&sigchld_mask, SIGCHLD) == -1) {
        int setup_error = errno;
        Logger::error(localization::format_translated_message(
            "Failed to initialize the {} mask: {}", "SIGCHLD",
            std::string_view(std::strerror(setup_error))));
        return false;
    }

    if(sigaction(SIGINT, &ignore_action, &state.original_sigint) == -1) {
        int setup_error = errno;
        log_signal_setup_error(command_context, "SIGINT", setup_error);
        return false;
    }
    state.sigint_changed = true;

    if(sigaction(SIGQUIT, &ignore_action, &state.original_sigquit) == -1) {
        int setup_error = errno;
        restore_parent_signal_state(
            state, SignalRestoreContext::CommandSetupRollback);
        log_signal_setup_error(command_context, "SIGQUIT", setup_error);
        return false;
    }
    state.sigquit_changed = true;

    if(sigprocmask(SIG_BLOCK, &sigchld_mask, &state.original_mask) == -1) {
        int setup_error = errno;
        restore_parent_signal_state(
            state, SignalRestoreContext::CommandSetupRollback);
        log_signal_setup_error(command_context, "SIGCHLD", setup_error);
        return false;
    }
    state.mask_changed = true;
    return true;
}

std::string trim_captured_output(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

CapturedCommandResult capture_command_output_impl(
    const char* cmd,
    bool trim_output) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd, "r"), pclose);
    if(!pipe) return CapturedCommandResult{};
    // POLICY(#242): strict machine-output parserへNULも含む全byteを渡し、
    // C-string appendによる途中切り捨てを起こさない。
    while(true) {
        std::size_t bytes_read =
            std::fread(buffer.data(), 1, buffer.size(), pipe.get());
        result.append(buffer.data(), bytes_read);
        if(bytes_read == 0) break;
    }
    bool read_failed = ferror(pipe.get()) != 0;
    int status = pclose(pipe.release());
    int exit_code = 127;
    if(status != -1) {
        if(WIFEXITED(status))
            exit_code = WEXITSTATUS(status);
        else if(WIFSIGNALED(status))
            exit_code = 128 + WTERMSIG(status);
        else
            exit_code = 1;
    }
    if(read_failed) exit_code = 1;
    return CapturedCommandResult{
        trim_output ? trim_captured_output(result) : std::move(result),
        exit_code};
}

int decode_process_status(int status) noexcept {
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

bool has_embedded_nul(const std::string& value) noexcept {
    return value.find('\0') != std::string::npos;
}

int duplicate_executable_descriptor_for_exec(
    const ExplicitProcessInvocation& invocation) noexcept {
    if(!invocation.executable_fd.has_value()) return -1;
    int descriptor;
    do {
        // LANDMINE: an execveat(AT_EMPTY_PATH) target may be a shebang
        // script. A CLOEXEC descriptor makes the interpreter hand-off fail
        // with ENOENT, so only the child receives this non-CLOEXEC duplicate.
        descriptor = fcntl(*invocation.executable_fd, F_DUPFD, 3);
    } while(descriptor == -1 && errno == EINTR);
    return descriptor;
}

[[noreturn]] void exec_explicit_process_child(
    const ExplicitProcessInvocation& invocation,
    std::vector<char*>& argument_vector,
    std::vector<char*>& environment_vector,
    const int output_pipe[2],
    bool capture_standard_output,
    bool suppress_standard_output,
    bool suppress_standard_error,
    const CommandSignalState& signal_state,
    std::optional<int> inherited_lifetime_guard_fd = std::nullopt) {
    if(sigaction(SIGINT, &signal_state.original_sigint, nullptr) == -1 ||
       sigaction(SIGQUIT, &signal_state.original_sigquit, nullptr) == -1 ||
       sigprocmask(
           SIG_SETMASK, &signal_state.original_mask, nullptr) == -1) {
        _exit(127);
    }

    if(invocation.working_directory_fd.has_value() &&
       fchdir(*invocation.working_directory_fd) == -1) {
        _exit(127);
    }

    // Preserve the retained executable before stdio can overwrite an alias
    // (or close a shared stdin FD). Only this child-local FD may be executed.
    const int executable_descriptor =
        duplicate_executable_descriptor_for_exec(invocation);
    if(invocation.executable_fd.has_value() && executable_descriptor < 0) {
        _exit(127);
    }

    if(invocation.standard_input_fd.has_value()) {
        const int source_fd = *invocation.standard_input_fd;
        if(source_fd == STDIN_FILENO) {
            // LANDMINE: dup2(0, 0) does not clear an inherited CLOEXEC bit.
            const int descriptor_flags = fcntl(STDIN_FILENO, F_GETFD);
            if(descriptor_flags == -1 ||
               fcntl(
                   STDIN_FILENO, F_SETFD,
                   descriptor_flags & ~FD_CLOEXEC) == -1) {
                _exit(127);
            }
        } else if(dup2(source_fd, STDIN_FILENO) == -1) {
            _exit(127);
        } else if(source_fd > STDERR_FILENO) {
            static_cast<void>(close(source_fd));
        }
    }

    if(capture_standard_output) {
        static_cast<void>(close(output_pipe[0]));
        if(dup2(output_pipe[1], STDOUT_FILENO) == -1) _exit(127);
        static_cast<void>(close(output_pipe[1]));
    }

    if(suppress_standard_output || suppress_standard_error) {
        const int null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if(null_descriptor < 0) _exit(127);
        const auto redirect_to_null = [null_descriptor](int target) {
            if(null_descriptor == target) {
                const int flags = fcntl(target, F_GETFD);
                return flags != -1 &&
                       fcntl(target, F_SETFD, flags & ~FD_CLOEXEC) != -1;
            }
            return dup2(null_descriptor, target) != -1;
        };
        if(suppress_standard_output &&
           !redirect_to_null(STDOUT_FILENO)) _exit(127);
        if(suppress_standard_error &&
           !redirect_to_null(STDERR_FILENO)) _exit(127);
        if(null_descriptor != STDOUT_FILENO &&
           null_descriptor != STDERR_FILENO) {
            static_cast<void>(close(null_descriptor));
        }
    }

    if(inherited_lifetime_guard_fd.has_value()) {
        const int guard_descriptor = *inherited_lifetime_guard_fd;
        const int descriptor_flags = fcntl(guard_descriptor, F_GETFD);
        if(descriptor_flags == -1 ||
           fcntl(
               guard_descriptor, F_SETFD,
               descriptor_flags & ~FD_CLOEXEC) == -1) {
            _exit(127);
        }
    }

    if(executable_descriptor >= 0) {
        static_cast<void>(syscall(
            SYS_execveat, executable_descriptor, "",
            argument_vector.data(), environment_vector.data(),
            AT_EMPTY_PATH));
    } else {
        execve(
            invocation.executable.c_str(), argument_vector.data(),
            environment_vector.data());
    }
    _exit(127);
}

bool close_descriptor_range(
    unsigned int first, unsigned int last) noexcept {
    if(first > last) return true;
#ifdef SYS_close_range
    long result;
    do {
        result = syscall(SYS_close_range, first, last, 0U);
    } while(result == -1 && errno == EINTR);
    return result == 0;
#else
    static_cast<void>(first);
    static_cast<void>(last);
    return false;
#endif
}

bool close_descriptors_except(int preserved_descriptor) noexcept {
    if(preserved_descriptor < 3) return false;
    if(preserved_descriptor > 3 &&
       !close_descriptor_range(
           3U, static_cast<unsigned int>(preserved_descriptor - 1))) {
        return false;
    }
    if(preserved_descriptor < INT_MAX &&
       !close_descriptor_range(
           static_cast<unsigned int>(preserved_descriptor + 1),
           UINT_MAX)) {
        return false;
    }
    return true;
}

bool close_descriptors_except(
    int first_preserved_descriptor,
    int second_preserved_descriptor) noexcept {
    if(first_preserved_descriptor == second_preserved_descriptor) {
        return close_descriptors_except(first_preserved_descriptor);
    }
    if(first_preserved_descriptor < 3 || second_preserved_descriptor < 3) {
        return false;
    }
    const int lower = std::min(
        first_preserved_descriptor, second_preserved_descriptor);
    const int upper = std::max(
        first_preserved_descriptor, second_preserved_descriptor);
    if(lower > 3 &&
       !close_descriptor_range(3U, static_cast<unsigned int>(lower - 1))) {
        return false;
    }
    if(lower < INT_MAX && lower + 1 < upper &&
       !close_descriptor_range(
           static_cast<unsigned int>(lower + 1),
           static_cast<unsigned int>(upper - 1))) {
        return false;
    }
    if(upper < INT_MAX &&
       !close_descriptor_range(
           static_cast<unsigned int>(upper + 1), UINT_MAX)) {
        return false;
    }
    return true;
}

[[noreturn]] void supervise_explicit_process_lifetime(
    const ExplicitProcessInvocation& invocation,
    std::vector<char*>& argument_vector,
    std::vector<char*>& environment_vector,
    bool suppress_standard_output,
    bool suppress_standard_error,
    const CommandSignalState& signal_state) {
    const int guard_descriptor =
        *invocation.parent_independent_lifetime_guard_fd;
    int supervisor_guard;
    do {
        supervisor_guard =
            fcntl(guard_descriptor, F_DUPFD_CLOEXEC, 3);
    } while(supervisor_guard == -1 && errno == EINTR);
    const bool descriptors_closed =
        invocation.executable_fd.has_value()
            ? close_descriptors_except(
                  supervisor_guard, *invocation.executable_fd)
            : close_descriptors_except(supervisor_guard);
    if(supervisor_guard == -1 || !descriptors_closed ||
       prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == -1) {
        _exit(127);
    }

    const pid_t mutator_pid = fork();
    if(mutator_pid == -1) _exit(127);
    if(mutator_pid == 0) {
        const int no_output_pipe[2] = {-1, -1};
        exec_explicit_process_child(
            invocation, argument_vector, environment_vector,
            no_output_pipe, false, suppress_standard_output,
            suppress_standard_error, signal_state, supervisor_guard);
    }

    // The supervisor and actual mutator tree retain the same open-file-
    // description. If either side dies independently, the other keeps the
    // guard. A descendant that outlives its expected Git parent intentionally
    // keeps the guard fail-closed until it exits; the subreaper prevents normal
    // completion from returning while such a descendant remains.
    int root_status = 0;
    bool root_reaped = false;
    while(true) {
        int descendant_status = 0;
        const pid_t waited = waitpid(-1, &descendant_status, 0);
        if(waited > 0) {
            if(waited == mutator_pid) {
                root_status = descendant_status;
                root_reaped = true;
            }
            continue;
        }
        if(waited == -1 && errno == EINTR) continue;
        if(waited == -1 && errno == ECHILD) break;
        _exit(127);
    }
    _exit(root_reaped ? decode_process_status(root_status) : 127);
}

constexpr std::uint32_t BOUNDED_CHILD_FAILURE_MAGIC = 0x4d475046U;
constexpr std::chrono::milliseconds BOUNDED_PROCESS_POLL_SLICE{20};

struct BoundedChildFailureRecord {
    std::uint32_t magic;
    std::uint32_t stage;
    std::int32_t error_number;
};

static_assert(sizeof(BoundedChildFailureRecord) <= PIPE_BUF);

class OwnedProcessDescriptor final {
public:
    OwnedProcessDescriptor() = default;
    explicit OwnedProcessDescriptor(int descriptor) noexcept
        : descriptor_(descriptor) {
    }

    OwnedProcessDescriptor(const OwnedProcessDescriptor&) = delete;
    OwnedProcessDescriptor& operator=(const OwnedProcessDescriptor&) =
        delete;

    OwnedProcessDescriptor(OwnedProcessDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    OwnedProcessDescriptor& operator=(
        OwnedProcessDescriptor&& other) noexcept {
        if(this == &other) return *this;
        reset(std::exchange(other.descriptor_, -1));
        return *this;
    }

    ~OwnedProcessDescriptor() noexcept {
        reset();
    }

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return descriptor_ >= 0;
    }

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

    void reset(int replacement = -1) noexcept {
        if(descriptor_ >= 0) {
            static_cast<void>(close(descriptor_));
        }
        descriptor_ = replacement;
    }

private:
    int descriptor_ = -1;
};

BoundedCapturedProcessResult bounded_launch_failure(
    BoundedProcessLaunchStage stage, int error_number) {
    return BoundedCapturedProcessResult{
        {}, BoundedProcessLaunchOrSetupFailure{stage, error_number}};
}

bool is_valid_bounded_invocation(
    const ExplicitProcessInvocation& invocation,
    const BoundedProcessPolicy& policy) noexcept {
    if(invocation.executable.empty() ||
       has_embedded_nul(invocation.executable) ||
       invocation.stdout_capture_limit.has_value() ||
       invocation.parent_independent_lifetime_guard_fd.has_value() ||
       !invocation.working_directory_fd.has_value() ||
       !invocation.standard_input_fd.has_value() ||
       policy.hard_timeout <= std::chrono::milliseconds::zero() ||
       policy.termination_grace < std::chrono::milliseconds::zero()) {
        return false;
    }
    for(const std::string& argument : invocation.arguments) {
        if(has_embedded_nul(argument)) return false;
    }
    for(const std::string& assignment : invocation.environment) {
        const std::size_t separator = assignment.find('=');
        if(separator == std::string::npos || separator == 0 ||
           has_embedded_nul(assignment)) {
            return false;
        }
    }
    return (!invocation.executable_fd.has_value() ||
            fcntl(*invocation.executable_fd, F_GETFD) != -1) &&
           fcntl(*invocation.working_directory_fd, F_GETFD) != -1 &&
           fcntl(*invocation.standard_input_fd, F_GETFD) != -1;
}

bool promote_process_descriptor(int& descriptor) noexcept {
    if(descriptor >= STDERR_FILENO + 1) return true;
    int promoted;
    do {
        promoted = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    } while(promoted == -1 && errno == EINTR);
    if(promoted == -1) return false;
    static_cast<void>(close(descriptor));
    descriptor = promoted;
    return true;
}

bool create_bounded_pipe(int descriptors[2]) noexcept {
    if(pipe2(descriptors, O_CLOEXEC) == -1) return false;
    if(promote_process_descriptor(descriptors[0]) &&
       promote_process_descriptor(descriptors[1])) {
        return true;
    }
    const int promotion_error = errno;
    if(descriptors[0] >= 0) static_cast<void>(close(descriptors[0]));
    if(descriptors[1] >= 0) static_cast<void>(close(descriptors[1]));
    descriptors[0] = -1;
    descriptors[1] = -1;
    errno = promotion_error;
    return false;
}

bool make_nonblocking(int descriptor) noexcept {
    int flags;
    do {
        flags = fcntl(descriptor, F_GETFL);
    } while(flags == -1 && errno == EINTR);
    if(flags == -1) return false;
    int result;
    do {
        result = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    } while(result == -1 && errno == EINTR);
    return result != -1;
}

[[noreturn]] void report_bounded_child_failure(
    int status_descriptor,
    BoundedProcessLaunchStage stage,
    int error_number) noexcept {
    const BoundedChildFailureRecord record{
        BOUNDED_CHILD_FAILURE_MAGIC,
        static_cast<std::uint32_t>(stage),
        static_cast<std::int32_t>(error_number)};
    ssize_t written;
    do {
        written = write(status_descriptor, &record, sizeof(record));
    } while(written == -1 && errno == EINTR);
    static_cast<void>(written);
    _exit(127);
}

void require_child_operation(
    bool succeeded,
    int status_descriptor,
    BoundedProcessLaunchStage stage) noexcept {
    if(!succeeded) {
        report_bounded_child_failure(status_descriptor, stage, errno);
    }
}

void bind_bounded_child_descriptor(
    int source_descriptor,
    int target_descriptor,
    int status_descriptor,
    BoundedProcessLaunchStage stage) noexcept {
    if(source_descriptor == target_descriptor) {
        const int descriptor_flags = fcntl(target_descriptor, F_GETFD);
        require_child_operation(
            descriptor_flags != -1 &&
                fcntl(
                    target_descriptor, F_SETFD,
                    descriptor_flags & ~FD_CLOEXEC) != -1,
            status_descriptor, stage);
        return;
    }
    require_child_operation(
        dup2(source_descriptor, target_descriptor) != -1,
        status_descriptor, stage);
}

[[noreturn]] void exec_bounded_explicit_process_child(
    const ExplicitProcessInvocation& invocation,
    std::vector<char*>& argument_vector,
    std::vector<char*>& environment_vector,
    int output_descriptor,
    int exec_status_descriptor,
    bool suppress_standard_error,
    const sigset_t& original_signal_mask,
    pid_t expected_parent_pid) noexcept {
    int group_result;
    do {
        group_result = setpgid(0, 0);
    } while(group_result == -1 && errno == EINTR);
    require_child_operation(
        group_result == 0, exec_status_descriptor,
        BoundedProcessLaunchStage::ChildProcessGroup);

    require_child_operation(
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) == 0,
        exec_status_descriptor,
        BoundedProcessLaunchStage::ParentDeathSignal);
    if(getppid() != expected_parent_pid) {
        static_cast<void>(kill(getpid(), SIGKILL));
        _exit(127);
    }

    require_child_operation(
        sigprocmask(SIG_SETMASK, &original_signal_mask, nullptr) == 0,
        exec_status_descriptor,
        BoundedProcessLaunchStage::ChildSignalMask);
    require_child_operation(
        fchdir(*invocation.working_directory_fd) == 0,
        exec_status_descriptor,
        BoundedProcessLaunchStage::WorkingDirectory);

    // Stdio binding must not change the object behind executable_fd.
    const int executable_descriptor =
        duplicate_executable_descriptor_for_exec(invocation);
    if(invocation.executable_fd.has_value() && executable_descriptor < 0) {
        report_bounded_child_failure(
            exec_status_descriptor,
            BoundedProcessLaunchStage::DescriptorHygiene, errno);
    }

    bind_bounded_child_descriptor(
        *invocation.standard_input_fd, STDIN_FILENO,
        exec_status_descriptor, BoundedProcessLaunchStage::StandardInput);
    bind_bounded_child_descriptor(
        output_descriptor, STDOUT_FILENO,
        exec_status_descriptor, BoundedProcessLaunchStage::StandardOutput);

    if(suppress_standard_error) {
        int null_descriptor;
        do {
            null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
        } while(null_descriptor == -1 && errno == EINTR);
        require_child_operation(
            null_descriptor != -1, exec_status_descriptor,
            BoundedProcessLaunchStage::StandardError);
        bind_bounded_child_descriptor(
            null_descriptor, STDERR_FILENO, exec_status_descriptor,
            BoundedProcessLaunchStage::StandardError);
    }

    const bool descriptors_closed =
        executable_descriptor >= 0
            ? close_descriptors_except(
                  exec_status_descriptor, executable_descriptor)
            : close_descriptors_except(exec_status_descriptor);
    if(!descriptors_closed) {
        report_bounded_child_failure(
            exec_status_descriptor,
            BoundedProcessLaunchStage::DescriptorHygiene, errno);
    }

    if(executable_descriptor >= 0) {
        static_cast<void>(syscall(
            SYS_execveat, executable_descriptor, "",
            argument_vector.data(), environment_vector.data(),
            AT_EMPTY_PATH));
    } else {
        execve(
            invocation.executable.c_str(), argument_vector.data(),
            environment_vector.data());
    }
    report_bounded_child_failure(
        exec_status_descriptor, BoundedProcessLaunchStage::Execve, errno);
}

int bounded_poll_timeout_milliseconds(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point next_deadline) noexcept {
    if(next_deadline <= now) return 0;
    auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        next_deadline - now);
    remaining = std::min(remaining, BOUNDED_PROCESS_POLL_SLICE);
    if(remaining.count() > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(remaining.count());
}

bool bounded_process_group_exists(
    pid_t process_group, int& observation_error) noexcept {
    if(kill(-process_group, 0) == 0) return true;
    if(errno == ESRCH) return false;
    if(errno == EPERM) return true;
    observation_error = errno;
    return true;
}

bool signal_bounded_process_group(
    pid_t process_group, int signal_number,
    int& signal_error) noexcept {
    if(process_group <= 0 || process_group == getpgrp()) {
        signal_error = EINVAL;
        return false;
    }
    if(kill(-process_group, signal_number) == 0 || errno == ESRCH) {
        return true;
    }
    signal_error = errno;
    return false;
}

BoundedCapturedProcessResult execute_bounded_explicit_process(
    const ExplicitProcessInvocation& invocation,
    const BoundedProcessPolicy& policy) {
    if(!is_valid_bounded_invocation(invocation, policy)) {
        return bounded_launch_failure(
            BoundedProcessLaunchStage::InvocationValidation, EINVAL);
    }

    std::vector<char*> argument_vector;
    argument_vector.reserve(invocation.arguments.size() + 2);
    argument_vector.push_back(
        const_cast<char*>(invocation.executable.c_str()));
    for(const std::string& argument : invocation.arguments) {
        argument_vector.push_back(const_cast<char*>(argument.c_str()));
    }
    argument_vector.push_back(nullptr);

    std::vector<char*> environment_vector;
    environment_vector.reserve(invocation.environment.size() + 1);
    for(const std::string& assignment : invocation.environment) {
        environment_vector.push_back(const_cast<char*>(assignment.c_str()));
    }
    environment_vector.push_back(nullptr);

    int output_pipe[2] = {-1, -1};
    if(!create_bounded_pipe(output_pipe)) {
        return bounded_launch_failure(
            BoundedProcessLaunchStage::StandardOutputPipe, errno);
    }
    OwnedProcessDescriptor output_read(output_pipe[0]);
    OwnedProcessDescriptor output_write(output_pipe[1]);
    if(!make_nonblocking(output_read.get())) {
        return BoundedCapturedProcessResult{
            {}, BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::StandardOutputNonblocking, errno}};
    }

    int exec_status_pipe[2] = {-1, -1};
    if(!create_bounded_pipe(exec_status_pipe)) {
        return bounded_launch_failure(
            BoundedProcessLaunchStage::ExecStatusPipe, errno);
    }
    OwnedProcessDescriptor exec_status_read(exec_status_pipe[0]);
    OwnedProcessDescriptor exec_status_write(exec_status_pipe[1]);
    if(!make_nonblocking(exec_status_read.get())) {
        return BoundedCapturedProcessResult{
            {}, BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::ExecStatusNonblocking, errno}};
    }

    sigset_t original_signal_mask;
    if(sigprocmask(SIG_SETMASK, nullptr, &original_signal_mask) == -1) {
        return bounded_launch_failure(
            BoundedProcessLaunchStage::SignalMask, errno);
    }
    sigset_t observed_signals;
    if(sigemptyset(&observed_signals) == -1) {
        return bounded_launch_failure(
            BoundedProcessLaunchStage::SignalMask, errno);
    }
    bool has_observed_signal = false;
    constexpr std::array<int, 5> OBSERVED_SIGNALS{
        SIGCHLD, SIGINT, SIGQUIT, SIGHUP, SIGTERM};
    for(int signal_number : OBSERVED_SIGNALS) {
        const int is_blocked =
            sigismember(&original_signal_mask, signal_number);
        if(is_blocked == -1) {
            return bounded_launch_failure(
                BoundedProcessLaunchStage::SignalMask, errno);
        }
        if(is_blocked == 0) {
            if(sigaddset(&observed_signals, signal_number) == -1) {
                return bounded_launch_failure(
                    BoundedProcessLaunchStage::SignalMask, errno);
            }
            has_observed_signal = true;
        }
    }

    bool signal_mask_changed = false;
    if(has_observed_signal) {
        if(sigprocmask(
               SIG_BLOCK, &observed_signals, nullptr) == -1) {
            return bounded_launch_failure(
                BoundedProcessLaunchStage::SignalMask, errno);
        }
        signal_mask_changed = true;
    }

    OwnedProcessDescriptor signal_descriptor;
    if(has_observed_signal) {
        int descriptor = signalfd(
            -1, &observed_signals, SFD_CLOEXEC | SFD_NONBLOCK);
        if(descriptor == -1 || !promote_process_descriptor(descriptor)) {
            const int setup_error = errno;
            if(descriptor >= 0) static_cast<void>(close(descriptor));
            if(signal_mask_changed &&
               sigprocmask(
                   SIG_SETMASK, &original_signal_mask, nullptr) == -1) {
                return BoundedCapturedProcessResult{
                    {}, BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::SignalMaskRestore, errno}};
            }
            return bounded_launch_failure(
                BoundedProcessLaunchStage::SignalDescriptor,
                setup_error);
        }
        signal_descriptor.reset(descriptor);
    }

    int original_subreaper = 0;
    bool subreaper_changed = false;
    if(prctl(
           PR_GET_CHILD_SUBREAPER, &original_subreaper, 0, 0, 0) == -1 ||
       (original_subreaper == 0 &&
        prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == -1)) {
        const int setup_error = errno;
        signal_descriptor.reset();
        if(signal_mask_changed &&
           sigprocmask(
               SIG_SETMASK, &original_signal_mask, nullptr) == -1) {
            return BoundedCapturedProcessResult{
                {}, BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::SignalMaskRestore, errno}};
        }
        return bounded_launch_failure(
            BoundedProcessLaunchStage::Subreaper, setup_error);
    }
    subreaper_changed = original_subreaper == 0;

    const pid_t expected_parent_pid = getpid();
    const pid_t child_pid = fork();
    if(child_pid == -1) {
        const int fork_error = errno;
        int restore_error = 0;
        BoundedProcessIoStage restore_stage =
            BoundedProcessIoStage::SubreaperRestore;
        if(subreaper_changed &&
           prctl(
               PR_SET_CHILD_SUBREAPER, original_subreaper,
               0, 0, 0) == -1) {
            restore_error = errno;
        }
        signal_descriptor.reset();
        if(signal_mask_changed &&
           sigprocmask(
               SIG_SETMASK, &original_signal_mask, nullptr) == -1 &&
           restore_error == 0) {
            restore_error = errno;
            restore_stage =
                BoundedProcessIoStage::SignalMaskRestore;
        }
        if(restore_error != 0) {
            return BoundedCapturedProcessResult{
                {}, BoundedProcessIoOrWaitFailure{restore_stage, restore_error}};
        }
        return bounded_launch_failure(
            BoundedProcessLaunchStage::Fork, fork_error);
    }
    if(child_pid == 0) {
        exec_bounded_explicit_process_child(
            invocation, argument_vector, environment_vector,
            output_write.get(), exec_status_write.get(),
            policy.suppress_standard_error, original_signal_mask,
            expected_parent_pid);
    }

    const auto hard_deadline =
        std::chrono::steady_clock::now() + policy.hard_timeout;
    output_write.reset();
    exec_status_write.reset();

    bool process_group_is_dedicated = false;
    int parent_group_result;
    do {
        parent_group_result = setpgid(child_pid, child_pid);
    } while(parent_group_result == -1 && errno == EINTR);
    if(parent_group_result == 0) {
        process_group_is_dedicated = true;
    } else {
        const int group_error = errno;
        const pid_t observed_group = getpgid(child_pid);
        if(observed_group == child_pid ||
           (observed_group == -1 && errno == ESRCH)) {
            process_group_is_dedicated = true;
        } else if(group_error != ESRCH) {
            static_cast<void>(kill(child_pid, SIGKILL));
            int child_status = 0;
            while(waitpid(child_pid, &child_status, 0) == -1 &&
                  errno == EINTR) {
            }
            int restore_error = 0;
            BoundedProcessIoStage restore_stage =
                BoundedProcessIoStage::SubreaperRestore;
            if(subreaper_changed &&
               prctl(
                   PR_SET_CHILD_SUBREAPER, original_subreaper,
                   0, 0, 0) == -1) {
                restore_error = errno;
            }
            signal_descriptor.reset();
            if(signal_mask_changed &&
               sigprocmask(
                   SIG_SETMASK, &original_signal_mask, nullptr) == -1 &&
               restore_error == 0) {
                restore_error = errno;
                restore_stage =
                    BoundedProcessIoStage::SignalMaskRestore;
            }
            if(restore_error != 0) {
                return BoundedCapturedProcessResult{
                    {}, BoundedProcessIoOrWaitFailure{restore_stage, restore_error}};
            }
            return bounded_launch_failure(
                BoundedProcessLaunchStage::ParentProcessGroup,
                group_error);
        }
    }

    std::string output;
    std::optional<BoundedProcessOutcome> forced_outcome;
    std::exception_ptr pending_exception;
    std::array<char, sizeof(BoundedChildFailureRecord)>
        exec_status_bytes{};
    std::size_t exec_status_size = 0;
    bool exec_succeeded = false;
    bool root_reaped = false;
    int root_status = 0;
    bool termination_started = false;
    bool kill_sent = false;
    auto termination_deadline = hard_deadline;

    const auto force_io_failure = [&](BoundedProcessIoStage stage,
                                      int error_number) {
        forced_outcome = BoundedProcessIoOrWaitFailure{
            stage, error_number};
    };

    const auto signal_tree = [&](int signal_number) {
        int signal_error = 0;
        bool signaled = false;
        if(process_group_is_dedicated) {
            signaled = signal_bounded_process_group(
                child_pid, signal_number, signal_error);
        } else if(kill(child_pid, signal_number) == 0 ||
                  errno == ESRCH) {
            signaled = true;
        } else {
            signal_error = errno;
        }
        if(!signaled) {
            force_io_failure(
                BoundedProcessIoStage::ProcessGroupSignal,
                signal_error);
        }
    };

    const auto begin_termination = [&](int first_signal) {
        if(termination_started) return;
        termination_started = true;
        termination_deadline =
            std::chrono::steady_clock::now() +
            policy.termination_grace;
        signal_tree(first_signal);
    };

    const auto close_output_after_terminal_cause = [&]() {
        output_read.reset();
    };

    const auto observe_process_group = [&]() {
        if(!process_group_is_dedicated) return !root_reaped;
        int observation_error = 0;
        const bool exists = bounded_process_group_exists(
            child_pid, observation_error);
        if(observation_error != 0) {
            force_io_failure(
                BoundedProcessIoStage::ProcessGroupObservation,
                observation_error);
        }
        return exists;
    };

    while(true) {
        auto now = std::chrono::steady_clock::now();
        // POLICY: deadline observation precedes pipe readiness from the same
        // poll wakeup. Once overflow is observed first, later deadline expiry
        // cannot replace CaptureLimitExceeded during TERM/KILL cleanup.
        if(!forced_outcome.has_value() && now >= hard_deadline) {
            forced_outcome = BoundedProcessTimedOut{};
            close_output_after_terminal_cause();
            begin_termination(SIGTERM);
        }

        if(output_read.valid()) {
            std::array<char, 4096> buffer;
            while(true) {
                const ssize_t bytes_read = read(
                    output_read.get(), buffer.data(), buffer.size());
                if(bytes_read > 0) {
                    const std::size_t chunk_size =
                        static_cast<std::size_t>(bytes_read);
                    const std::size_t remaining =
                        output.size() < policy.stdout_capture_limit
                            ? policy.stdout_capture_limit - output.size()
                            : 0;
                    const std::size_t retained =
                        std::min(chunk_size, remaining);
                    if(retained > 0) {
                        try {
                            output.append(buffer.data(), retained);
                        } catch(...) {
                            pending_exception = std::current_exception();
                            force_io_failure(
                                BoundedProcessIoStage::StandardOutputRead,
                                ENOMEM);
                            close_output_after_terminal_cause();
                            begin_termination(SIGTERM);
                            break;
                        }
                    }
                    if(retained < chunk_size) {
                        if(!forced_outcome.has_value()) {
                            forced_outcome =
                                BoundedProcessCaptureLimitExceeded{
                                    policy.stdout_capture_limit};
                        }
                        close_output_after_terminal_cause();
                        begin_termination(SIGTERM);
                        break;
                    }
                    continue;
                }
                if(bytes_read == 0) {
                    output_read.reset();
                    break;
                }
                if(errno == EINTR) continue;
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                force_io_failure(
                    BoundedProcessIoStage::StandardOutputRead, errno);
                close_output_after_terminal_cause();
                begin_termination(SIGTERM);
                break;
            }
        }

        if(exec_status_read.valid()) {
            while(true) {
                const ssize_t bytes_read = read(
                    exec_status_read.get(),
                    exec_status_bytes.data() + exec_status_size,
                    exec_status_bytes.size() - exec_status_size);
                if(bytes_read > 0) {
                    exec_status_size +=
                        static_cast<std::size_t>(bytes_read);
                    if(exec_status_size == exec_status_bytes.size()) {
                        BoundedChildFailureRecord record{};
                        std::memcpy(
                            &record, exec_status_bytes.data(),
                            sizeof(record));
                        if(record.magic != BOUNDED_CHILD_FAILURE_MAGIC ||
                           record.stage > static_cast<std::uint32_t>(
                                              BoundedProcessLaunchStage::
                                                  Execve)) {
                            force_io_failure(
                                BoundedProcessIoStage::ExecStatusRead,
                                EPROTO);
                        } else {
                            forced_outcome =
                                BoundedProcessLaunchOrSetupFailure{
                                    static_cast<BoundedProcessLaunchStage>(
                                        record.stage),
                                    record.error_number};
                        }
                        exec_status_read.reset();
                        close_output_after_terminal_cause();
                        begin_termination(SIGTERM);
                        break;
                    }
                    continue;
                }
                if(bytes_read == 0) {
                    if(exec_status_size == 0) {
                        exec_succeeded = true;
                    } else {
                        force_io_failure(
                            BoundedProcessIoStage::ExecStatusRead,
                            EPROTO);
                        close_output_after_terminal_cause();
                        begin_termination(SIGTERM);
                    }
                    exec_status_read.reset();
                    break;
                }
                if(errno == EINTR) continue;
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                force_io_failure(
                    BoundedProcessIoStage::ExecStatusRead, errno);
                exec_status_read.reset();
                close_output_after_terminal_cause();
                begin_termination(SIGTERM);
                break;
            }
        }

        if(signal_descriptor.valid()) {
            while(true) {
                signalfd_siginfo signal_information{};
                const ssize_t bytes_read = read(
                    signal_descriptor.get(), &signal_information,
                    sizeof(signal_information));
                if(bytes_read == static_cast<ssize_t>(
                                     sizeof(signal_information))) {
                    const int signal_number = static_cast<int>(
                        signal_information.ssi_signo);
                    if(signal_number != SIGCHLD) {
                        signal_tree(signal_number);
                        if(!termination_started) {
                            termination_started = true;
                            termination_deadline =
                                std::chrono::steady_clock::now() +
                                policy.termination_grace;
                        }
                    }
                    continue;
                }
                if(bytes_read == -1 && errno == EINTR) continue;
                if(bytes_read == -1 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                if(bytes_read == 0) break;
                force_io_failure(
                    BoundedProcessIoStage::SignalRead,
                    bytes_read == -1 ? errno : EPROTO);
                begin_termination(SIGTERM);
                break;
            }
        }

        if(!root_reaped) {
            int status = 0;
            const pid_t waited = waitpid(child_pid, &status, WNOHANG);
            if(waited == child_pid) {
                root_reaped = true;
                root_status = status;
            } else if(waited == -1 && errno != EINTR) {
                const int wait_error = errno;
                force_io_failure(
                    BoundedProcessIoStage::Wait, wait_error);
                if(wait_error == ECHILD) root_reaped = true;
                begin_termination(SIGTERM);
            }
        }

        if(root_reaped) {
            while(true) {
                int descendant_status = 0;
                const pid_t waited = waitpid(
                    -child_pid, &descendant_status, WNOHANG);
                if(waited > 0) continue;
                if(waited == 0 || (waited == -1 && errno == ECHILD)) {
                    break;
                }
                if(waited == -1 && errno == EINTR) continue;
                force_io_failure(
                    BoundedProcessIoStage::Wait, errno);
                begin_termination(SIGTERM);
                break;
            }
        }

        bool group_exists = observe_process_group();
        if(root_reaped && group_exists && !termination_started) {
            // A root that exits while a same-group descendant retains stdout
            // cannot turn the old blocking EOF drain into an unbounded wait.
            begin_termination(SIGTERM);
        }

        now = std::chrono::steady_clock::now();
        if(termination_started && group_exists && !kill_sent &&
           now >= termination_deadline) {
            kill_sent = true;
            signal_tree(SIGKILL);
        }

        group_exists = observe_process_group();
        const bool normal_streams_complete =
            !output_read.valid() && !exec_status_read.valid() &&
            exec_succeeded;
        if(root_reaped && !group_exists &&
           (forced_outcome.has_value() || normal_streams_complete)) {
            break;
        }

        std::array<pollfd, 3> poll_descriptors{};
        nfds_t descriptor_count = 0;
        const auto append_poll_descriptor = [&](int descriptor) {
            poll_descriptors[descriptor_count] =
                pollfd{descriptor, POLLIN | POLLHUP, 0};
            ++descriptor_count;
        };
        if(output_read.valid()) append_poll_descriptor(output_read.get());
        if(exec_status_read.valid()) {
            append_poll_descriptor(exec_status_read.get());
        }
        if(signal_descriptor.valid()) {
            append_poll_descriptor(signal_descriptor.get());
        }

        const auto poll_now = std::chrono::steady_clock::now();
        auto next_deadline =
            forced_outcome.has_value() || kill_sent
                ? poll_now + BOUNDED_PROCESS_POLL_SLICE
                : hard_deadline;
        if(termination_started && !kill_sent) {
            next_deadline = std::min(next_deadline, termination_deadline);
        }
        const int poll_timeout = bounded_poll_timeout_milliseconds(
            poll_now, next_deadline);
        int poll_result;
        do {
            poll_result = poll(
                poll_descriptors.data(), descriptor_count,
                poll_timeout);
        } while(poll_result == -1 && errno == EINTR);
        if(poll_result == -1) {
            force_io_failure(BoundedProcessIoStage::Poll, errno);
            close_output_after_terminal_cause();
            begin_termination(SIGTERM);
        } else if(poll_result > 0) {
            for(nfds_t index = 0; index < descriptor_count; ++index) {
                if((poll_descriptors[index].revents & POLLNVAL) != 0) {
                    force_io_failure(BoundedProcessIoStage::Poll, EBADF);
                    close_output_after_terminal_cause();
                    begin_termination(SIGTERM);
                    break;
                }
            }
        }
    }

    // One final group-targeted reap closes the subreaper adoption race before
    // restoring the caller's process-wide subreaper state.
    while(true) {
        int descendant_status = 0;
        const pid_t waited = waitpid(-child_pid, &descendant_status, WNOHANG);
        if(waited > 0) continue;
        if(waited == -1 && errno == EINTR) continue;
        break;
    }

    output_read.reset();
    exec_status_read.reset();
    signal_descriptor.reset();

    if(subreaper_changed &&
       prctl(
           PR_SET_CHILD_SUBREAPER, original_subreaper,
           0, 0, 0) == -1) {
        force_io_failure(
            BoundedProcessIoStage::SubreaperRestore, errno);
    }
    if(signal_mask_changed &&
       sigprocmask(
           SIG_SETMASK, &original_signal_mask, nullptr) == -1) {
        force_io_failure(
            BoundedProcessIoStage::SignalMaskRestore, errno);
    }

    if(pending_exception) std::rethrow_exception(pending_exception);
    if(forced_outcome.has_value()) {
        return BoundedCapturedProcessResult{
            std::move(output), std::move(*forced_outcome)};
    }
    if(WIFEXITED(root_status)) {
        return BoundedCapturedProcessResult{
            std::move(output),
            BoundedProcessExited{WEXITSTATUS(root_status)}};
    }
    if(WIFSIGNALED(root_status)) {
        return BoundedCapturedProcessResult{
            std::move(output),
            BoundedProcessSignaled{WTERMSIG(root_status)}};
    }
    return BoundedCapturedProcessResult{
        std::move(output),
        BoundedProcessIoOrWaitFailure{
            BoundedProcessIoStage::Wait, ECHILD}};
}

CapturedCommandResult execute_explicit_process(
    const ExplicitProcessInvocation& invocation,
    bool capture_standard_output,
    bool suppress_standard_output,
    bool suppress_standard_error,
    ExplicitProcessExecutionObservation* execution_observation = nullptr) {
    if(execution_observation != nullptr) {
        *execution_observation = ExplicitProcessExecutionObservation{};
    }
    if(invocation.executable.empty() ||
       has_embedded_nul(invocation.executable)) {
        return CapturedCommandResult{};
    }
    if(invocation.executable_fd.has_value() &&
       fcntl(*invocation.executable_fd, F_GETFD) == -1) {
        return CapturedCommandResult{};
    }
    for(const std::string& argument : invocation.arguments) {
        if(has_embedded_nul(argument)) return CapturedCommandResult{};
    }
    for(const std::string& assignment : invocation.environment) {
        const std::size_t separator = assignment.find('=');
        if(separator == std::string::npos || separator == 0 ||
           has_embedded_nul(assignment)) {
            return CapturedCommandResult{};
        }
    }
    if(invocation.parent_independent_lifetime_guard_fd.has_value()) {
        const int guard_descriptor =
            *invocation.parent_independent_lifetime_guard_fd;
        if(capture_standard_output ||
           invocation.working_directory_fd.has_value() ||
           invocation.standard_input_fd.has_value() ||
           guard_descriptor < 0 ||
           fcntl(guard_descriptor, F_GETFD) == -1) {
            return CapturedCommandResult{};
        }
    }

    std::vector<char*> argument_vector;
    argument_vector.reserve(invocation.arguments.size() + 2);
    argument_vector.push_back(
        const_cast<char*>(invocation.executable.c_str()));
    for(const std::string& argument : invocation.arguments) {
        argument_vector.push_back(const_cast<char*>(argument.c_str()));
    }
    argument_vector.push_back(nullptr);

    std::vector<char*> environment_vector;
    environment_vector.reserve(invocation.environment.size() + 1);
    for(const std::string& assignment : invocation.environment) {
        environment_vector.push_back(const_cast<char*>(assignment.c_str()));
    }
    environment_vector.push_back(nullptr);

    int output_pipe[2] = {-1, -1};
    if(capture_standard_output && pipe2(output_pipe, O_CLOEXEC) != 0) {
        return CapturedCommandResult{};
    }

    CommandSignalState signal_state;
    if(!setup_command_signal_state(
           signal_state, CommandSignalContext::ExplicitProcess)) {
        if(output_pipe[0] >= 0) static_cast<void>(close(output_pipe[0]));
        if(output_pipe[1] >= 0) static_cast<void>(close(output_pipe[1]));
        return CapturedCommandResult{};
    }

    const pid_t child_pid = fork();
    if(child_pid == -1) {
        if(output_pipe[0] >= 0) static_cast<void>(close(output_pipe[0]));
        if(output_pipe[1] >= 0) static_cast<void>(close(output_pipe[1]));
        restore_parent_signal_state(
            signal_state,
            SignalRestoreContext::ExplicitProcessForkFailure);
        return CapturedCommandResult{};
    }
    if(child_pid == 0) {
        if(invocation.parent_independent_lifetime_guard_fd.has_value()) {
            supervise_explicit_process_lifetime(
                invocation, argument_vector, environment_vector,
                suppress_standard_output, suppress_standard_error,
                signal_state);
        }
        exec_explicit_process_child(
            invocation, argument_vector, environment_vector,
            output_pipe, capture_standard_output,
            suppress_standard_output, suppress_standard_error,
            signal_state);
    }
    if(execution_observation != nullptr) {
        execution_observation->child_started = true;
    }

    if(output_pipe[1] >= 0) static_cast<void>(close(output_pipe[1]));
    std::string output;
    bool read_failed = false;
    bool stdout_capture_limit_exceeded = false;
    std::exception_ptr pending_exception;
    if(capture_standard_output) {
        std::array<char, 4096> buffer;
        while(true) {
            const ssize_t bytes_read =
                read(output_pipe[0], buffer.data(), buffer.size());
            if(bytes_read > 0) {
                const std::size_t chunk_size =
                    static_cast<std::size_t>(bytes_read);
                std::size_t stored_size = chunk_size;
                if(invocation.stdout_capture_limit.has_value()) {
                    const std::size_t limit =
                        *invocation.stdout_capture_limit;
                    const std::size_t remaining =
                        output.size() < limit ? limit - output.size() : 0;
                    stored_size = std::min(chunk_size, remaining);
                    if(stored_size < chunk_size) {
                        stdout_capture_limit_exceeded = true;
                    }
                }

                if(stored_size > 0 && !pending_exception) {
                    try {
                        output.append(buffer.data(), stored_size);
                    } catch(...) {
                        // append失敗後もpipeをEOFまでdrainし、childを確実にreapしてから再送出する。
                        pending_exception = std::current_exception();
                    }
                }
                continue;
            }
            if(bytes_read == 0) break;
            if(errno == EINTR) continue;
            read_failed = true;
            break;
        }
        static_cast<void>(close(output_pipe[0]));
    }

    int status = 0;
    pid_t wait_result;
    do {
        wait_result = waitpid(child_pid, &status, 0);
    } while(wait_result == -1 && errno == EINTR);
    const bool signal_state_restored = restore_parent_signal_state(
        signal_state, SignalRestoreContext::ExplicitProcessWait);
#ifdef MOGUET_ENABLE_PROCESS_TEST_HOOKS
    if(g_explicit_process_post_wait_hook != nullptr) {
        g_explicit_process_post_wait_hook();
    }
#endif
    if(pending_exception) std::rethrow_exception(pending_exception);
    if(wait_result == -1 || !signal_state_restored) {
        return CapturedCommandResult{
            std::move(output), 127, stdout_capture_limit_exceeded};
    }
    if(execution_observation != nullptr) {
        execution_observation->outcome_known = true;
    }
    return CapturedCommandResult{
        std::move(output),
        read_failed ? 1 : decode_process_status(status),
        stdout_capture_limit_exceeded};
}

} // namespace

CapturedCommandResult capture_command_output(const char* cmd) {
    return capture_command_output_impl(cmd, true);
}

CapturedCommandResult capture_command_output_raw(const char* cmd) {
    return capture_command_output_impl(cmd, false);
}

CapturedCommandResult capture_explicit_process_output_raw(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_error) {
    return execute_explicit_process(
        invocation, true, false, suppress_standard_error);
}

BoundedCapturedProcessResult capture_bounded_explicit_process_output_raw(
    const ExplicitProcessInvocation& invocation,
    const BoundedProcessPolicy& policy) {
    return execute_bounded_explicit_process(invocation, policy);
}

int run_explicit_process(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_output,
    bool suppress_standard_error) {
    return execute_explicit_process(
               invocation, false, suppress_standard_output,
               suppress_standard_error)
        .exit_code;
}

ExplicitProcessExecutionResult run_explicit_process_with_outcome(
    const ExplicitProcessInvocation& invocation,
    bool suppress_standard_output,
    bool suppress_standard_error) noexcept {
    ExplicitProcessExecutionObservation observation;
    try {
        const CapturedCommandResult result = execute_explicit_process(
            invocation, false, suppress_standard_output,
            suppress_standard_error, &observation);
        if(!observation.child_started) {
            return ExplicitProcessExecutionResult{
                ExplicitProcessExecutionStatus::NotStarted,
                std::nullopt};
        }
        if(!observation.outcome_known) {
            return ExplicitProcessExecutionResult{
                ExplicitProcessExecutionStatus::StartedOutcomeUnknown,
                std::nullopt};
        }
        return ExplicitProcessExecutionResult{
            ExplicitProcessExecutionStatus::StartedKnownOutcome,
            result.exit_code};
    } catch(...) {
        return ExplicitProcessExecutionResult{
            observation.child_started
                ? ExplicitProcessExecutionStatus::StartedOutcomeUnknown
                : ExplicitProcessExecutionStatus::NotStarted,
            std::nullopt};
    }
}

#ifdef MOGUET_ENABLE_PROCESS_TEST_HOOKS
void set_explicit_process_post_wait_hook_for_test(
    ExplicitProcessPostWaitHookForTest hook) noexcept {
    g_explicit_process_post_wait_hook = hook;
}
#endif

std::string exec_command(const char* cmd) {
    return capture_command_output(cmd).output;
}

int command_status(const std::string& cmd) {
    int status = std::system(cmd.c_str());
    if(status == -1) return 127;
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

int run_command(const std::string& cmd) {
    Logger::raw_cmd(cmd);
    return command_status(cmd);
}

int run_command_with_parent_independent_lifetime_guard(
    const std::string& command,
    int lifetime_guard_fd,
    const std::string& display_command) {
    Logger::raw_cmd(display_command.empty() ? command : display_command);
    std::vector<std::string> environment;
    for(char** current = ::environ;
        current != nullptr && *current != nullptr; ++current) {
        environment.emplace_back(*current);
    }
    ExplicitProcessInvocation invocation;
    invocation.executable = "/bin/sh";
    invocation.arguments = {"-c", command};
    invocation.environment = std::move(environment);
    invocation.parent_independent_lifetime_guard_fd =
        lifetime_guard_fd;
    return run_explicit_process(invocation);
}

int run_command_with_stdin_fd(const std::string& command, int source_fd) {
    Logger::raw_cmd(command);
    const char* command_text = command.c_str();

    // POLICY: std::system()と同様、親はSIGINT/SIGQUITを無視し、SIGCHLDをblockしたままchildをreapする。
    CommandSignalState signal_state;
    if(!setup_command_signal_state(
           signal_state, CommandSignalContext::StdinFdCommand)) {
        return 127;
    }

    pid_t child_pid = fork();
    if(child_pid == -1) {
        restore_parent_signal_state(
            signal_state,
            SignalRestoreContext::StdinFdCommandForkFailure);
        return 127;
    }
    if(child_pid == 0) {
        // fork後に親専用のignore/block状態を持ち込まず、exec先へcallerのsignal契約を引き渡す。
        if(sigaction(SIGINT, &signal_state.original_sigint, nullptr) == -1 ||
           sigaction(SIGQUIT, &signal_state.original_sigquit, nullptr) == -1 ||
           sigprocmask(SIG_SETMASK, &signal_state.original_mask, nullptr) == -1) {
            _exit(127);
        }

        if(source_fd == STDIN_FILENO) {
            // LANDMINE: open(O_CLOEXEC)がfd 0を返した場合、dup2(0, 0)ではCLOEXECが解除されない。
            int descriptor_flags = fcntl(STDIN_FILENO, F_GETFD);
            if(descriptor_flags == -1 ||
               fcntl(STDIN_FILENO, F_SETFD, descriptor_flags & ~FD_CLOEXEC) == -1) {
                _exit(127);
            }
        } else if(dup2(source_fd, STDIN_FILENO) == -1) {
            _exit(127);
        }

        // source_fdにはO_CLOEXECがあるため、dup2後の元descriptorはexec時に閉じる。
        execl("/bin/sh", "sh", "-c", command_text, static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    pid_t wait_result;
    do {
        wait_result = waitpid(child_pid, &status, 0);
    } while(wait_result == -1 && errno == EINTR);

    bool signal_state_restored = restore_parent_signal_state(
        signal_state, SignalRestoreContext::StdinFdCommandWait);
    if(wait_result == -1 || !signal_state_restored) return 127;
    if(WIFEXITED(status)) return WEXITSTATUS(status);
    if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}
