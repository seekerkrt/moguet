#include "logging.hpp"

#include "localization.hpp"

#include <cerrno>
#include <chrono>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace {

std::ofstream path_log_file;
std::unique_ptr<logging_detail::StateLogBackend> state_log_backend;
std::exception_ptr pending_state_log_failure;
bool initialized = false;
bool diagnostics_to_stderr = false;
thread_local ScopedLoggerDiagnosticCapture* active_diagnostic_capture =
    nullptr;

std::ostream& diagnostic_stream() {
    return diagnostics_to_stderr ? std::cerr : std::cout;
}

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string make_log_record(
    std::string_view level, const std::string& message) {
    return "[" + get_timestamp() + "] [" + std::string(level) + "] " +
           message + "\n";
}

void reset_log_backend() noexcept {
    state_log_backend.reset();
    if(path_log_file.is_open()) path_log_file.close();
    path_log_file.clear();
    initialized = false;
}

} // namespace

ScopedLoggerDiagnosticCapture::ScopedLoggerDiagnosticCapture() {
    if(active_diagnostic_capture != nullptr) {
        throw std::logic_error(
            "Logger diagnostic capture is already active.");
    }
    active_diagnostic_capture = this;
    active_ = true;
}

ScopedLoggerDiagnosticCapture::~ScopedLoggerDiagnosticCapture() noexcept {
    stop();
}

void ScopedLoggerDiagnosticCapture::stop() noexcept {
    if(!active_) return;
    if(active_diagnostic_capture == this) {
        active_diagnostic_capture = nullptr;
    }
    active_ = false;
}

void ScopedLoggerDiagnosticCapture::capture(
    LoggerDiagnosticLevel level, const std::string& message,
    const std::string& command_presentation) {
    events_.push_back(LoggerDiagnosticEvent{level, message, command_presentation});
}

void ScopedLoggerDiagnosticCapture::replay() {
    stop();
    if(replayed_) return;
    replayed_ = true;
    std::vector<LoggerDiagnosticEvent> events = std::move(events_);
    for(const auto& event : events) {
        switch(event.level) {
            case LoggerDiagnosticLevel::Info:
                Logger::info(event.message);
                break;
            case LoggerDiagnosticLevel::Warning:
                Logger::warn(event.message);
                break;
            case LoggerDiagnosticLevel::Error:
                Logger::error(event.message);
                break;
            case LoggerDiagnosticLevel::Command:
                Logger::command(event.message, event.command_presentation);
                break;
        }
    }
}

void Logger::flush_diagnostic_capture() {
    if(active_diagnostic_capture != nullptr) active_diagnostic_capture->replay();
}

bool Logger::capture_diagnostic(
    LoggerDiagnosticLevel level, const std::string& message,
    const std::string& command_presentation) {
    if(active_diagnostic_capture == nullptr) return false;
    active_diagnostic_capture->capture(level, message, command_presentation);
    return true;
}

void Logger::set_diagnostics_to_stderr() {
    diagnostics_to_stderr = true;
}

void Logger::init(const std::filesystem::path& path) {
    if(path.has_parent_path() && !std::filesystem::exists(path.parent_path())) {
        std::filesystem::create_directories(path.parent_path());
    }
    reset_log_backend();
    pending_state_log_failure = nullptr;
    path_log_file.open(path, std::ios::app);
    initialized = path_log_file.is_open();
}

void Logger::adopt_state_log_backend(
    std::unique_ptr<logging_detail::StateLogBackend> backend,
    const std::string& initial_info_message) {
    reset_log_backend();
    pending_state_log_failure = nullptr;
    state_log_backend = std::move(backend);
    initialized = true;

    diagnostic_stream() << "\033[1;32m::\033[0m " << initial_info_message
                        << std::endl;
    try {
        // NO_TRANSLATE: INFO is a stable state-log schema token.
        state_log_backend->append_record(
            make_log_record("INFO", initial_info_message));
    } catch(...) {
        reset_log_backend();
        throw;
    }
}

void Logger::shutdown() {
    std::exception_ptr failure =
        std::exchange(pending_state_log_failure, nullptr);

    if(state_log_backend) {
        std::unique_ptr<logging_detail::StateLogBackend> closing_backend =
            std::move(state_log_backend);
        initialized = false;
        try {
            closing_backend->close_checked();
        } catch(...) {
            if(failure == nullptr) failure = std::current_exception();
        }
    } else if(path_log_file.is_open()) {
        // Explicit legacy LOGFILE keeps its existing best-effort stream
        // contract. The new checked close contract applies only to the
        // descriptor-backed default state log.
        path_log_file.flush();
        path_log_file.close();
        path_log_file.clear();
        initialized = false;
    }

    if(failure != nullptr) std::rethrow_exception(failure);
}

void Logger::write_log_record(
    std::string_view level, const std::string& message) {
    if(pending_state_log_failure != nullptr)
        std::rethrow_exception(pending_state_log_failure);
    if(!initialized) return;
    std::string record = make_log_record(level, message);
    if(state_log_backend) {
        try {
            state_log_backend->append_record(record);
            return;
        } catch(...) {
            pending_state_log_failure = std::current_exception();
            reset_log_backend();
            throw;
        }
    }
    path_log_file << record << std::flush;
}

void Logger::info(const std::string& msg) {
    if(capture_diagnostic(LoggerDiagnosticLevel::Info, msg)) return;
    diagnostic_stream() << "\033[1;32m::\033[0m " << msg << std::endl;
    // NO_TRANSLATE: INFO is a stable state-log schema token.
    write_log_record("INFO", msg);
}

void Logger::warn(const std::string& msg) {
    if(capture_diagnostic(LoggerDiagnosticLevel::Warning, msg)) return;
    // TRANSLATORS: The placeholder is a complete warning diagnostic.
    diagnostic_stream() << "\033[1;33m::\033[0m "
                        << localization::format_translated_message(
                               "Warning: {}", msg)
                        << std::endl;
    // NO_TRANSLATE: WARN is a stable state-log schema token.
    write_log_record("WARN", msg);
}

void Logger::write_noexcept_warning_fallback() noexcept {
    // NO_TRANSLATE: This allocation-free raw-write fallback is used only when
    // message construction or logging itself has failed; gettext is not safe
    // inside this noexcept recovery boundary.
    static constexpr char FALLBACK_DIAGNOSTIC[] =
        "\033[1;31m:: Error:\033[0m Cleanup warning could not be "
        "constructed or logged safely.\n";
    const int saved_errno = errno;
    std::size_t offset = 0;
    while(offset < sizeof(FALLBACK_DIAGNOSTIC) - 1) {
        const ssize_t written = write(
            STDERR_FILENO, FALLBACK_DIAGNOSTIC + offset,
            sizeof(FALLBACK_DIAGNOSTIC) - 1 - offset);
        if(written > 0) {
            offset += static_cast<std::size_t>(written);
            continue;
        }
        if(written < 0 && errno == EINTR) continue;
        break;
    }
    errno = saved_errno;
}

void Logger::error(const std::string& msg) {
    if(capture_diagnostic(LoggerDiagnosticLevel::Error, msg)) return;
    // TRANSLATORS: The placeholder is a complete error diagnostic.
    std::cerr << "\033[1;31m::\033[0m "
              << localization::format_translated_message("Error: {}", msg)
              << std::endl;
    // NO_TRANSLATE: ERROR is a stable state-log schema token.
    write_log_record("ERROR", msg);
}

void Logger::raw_cmd(const std::string& cmd) {
    // TRANSLATORS: The placeholder is an exact shell command and must remain
    // byte-for-byte locale-neutral.
    command(cmd, localization::format_translated_message("Running: {}", cmd));
}

void Logger::command(const std::string& cmd, const std::string& terminal_message) {
    if(capture_diagnostic(LoggerDiagnosticLevel::Command, cmd, terminal_message)) return;
    diagnostic_stream() << "\033[1;33m::\033[0m " << terminal_message << std::endl;
    // NO_TRANSLATE: EXEC is a stable state-log schema token.
    write_log_record("EXEC", cmd);
}

#ifdef MOGUET_TEST_XDG_STATE_LOG_HOOKS
int Logger::state_log_descriptor_for_test() noexcept {
    if(!state_log_backend) return -1;
    return state_log_backend->descriptor_for_test();
}

void Logger::reset_for_test() noexcept {
    reset_log_backend();
    pending_state_log_failure = nullptr;
    diagnostics_to_stderr = false;
}
#endif
