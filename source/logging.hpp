#pragma once

#include <concepts>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xdg_state_log {
class PreparedLogFile;
}

namespace logging_detail {

// logging.cppをXDG storage stackへlink依存させないための内部backend境界。
// Productionでこのinterfaceを実装するのはdefault state logだけ。
class StateLogBackend {
public:
    StateLogBackend() = default;
    StateLogBackend(const StateLogBackend&) = delete;
    StateLogBackend& operator=(const StateLogBackend&) = delete;
    virtual ~StateLogBackend() noexcept = default;

    virtual void append_record(std::string_view record) = 0;
    virtual void close_checked() = 0;

#ifdef MOGUET_TEST_XDG_STATE_LOG_HOOKS
    virtual int descriptor_for_test() const noexcept = 0;
#endif
};

} // namespace logging_detail

enum class LoggerDiagnosticLevel {
    Info,
    Warning,
    Error,
    Command,
};

struct LoggerDiagnosticEvent {
    LoggerDiagnosticLevel level = LoggerDiagnosticLevel::Info;
    std::string message;
    std::string command_presentation;
};

// Synchronous route preparation can retain diagnostics without touching the
// terminal or state log. The caller owns this scope and explicitly replays it
// once after deciding whether persistent logging is allowed.
class ScopedLoggerDiagnosticCapture final {
public:
    ScopedLoggerDiagnosticCapture();
    ScopedLoggerDiagnosticCapture(
        const ScopedLoggerDiagnosticCapture&) = delete;
    ScopedLoggerDiagnosticCapture& operator=(
        const ScopedLoggerDiagnosticCapture&) = delete;
    ScopedLoggerDiagnosticCapture(
        ScopedLoggerDiagnosticCapture&&) = delete;
    ScopedLoggerDiagnosticCapture& operator=(
        ScopedLoggerDiagnosticCapture&&) = delete;
    ~ScopedLoggerDiagnosticCapture() noexcept;

    void stop() noexcept;
    void replay();

private:
    friend class Logger;

    void capture(LoggerDiagnosticLevel level, const std::string& message,
                 const std::string& command_presentation);

    std::vector<LoggerDiagnosticEvent> events_;
    bool active_ = false;
    bool replayed_ = false;
};

// CLI 表示と log file 出力をまとめる薄い logger。
class Logger {
    static bool capture_diagnostic(
        LoggerDiagnosticLevel level, const std::string& message,
        const std::string& command_presentation = {});
    static void write_log_record(
        std::string_view level, const std::string& message);
    static void adopt_state_log_backend(
        std::unique_ptr<logging_detail::StateLogBackend> backend,
        const std::string& initial_info_message);
    static void write_noexcept_warning_fallback() noexcept;

public:
    static void set_diagnostics_to_stderr();
    static void init(const std::filesystem::path& path);
    static void init(
        xdg_state_log::PreparedLogFile&& log_file,
        const std::string& initial_info_message);
    static void shutdown();
    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    // Cleanup destructor専用。factoryはnoexcept boundary内で同期評価する。
    // Call siteはpath/errorを参照captureし、事前にmessageを構築しない。
    template <typename MessageFactory>
        requires requires(MessageFactory&& make_message) {
            {
                std::forward<MessageFactory>(make_message)()
            } -> std::convertible_to<std::string>;
        }
    static void warn_noexcept(MessageFactory&& make_message) noexcept {
        try {
            std::string message(
                std::forward<MessageFactory>(make_message)());
            warn(message);
        } catch(...) {
            write_noexcept_warning_fallback();
        }
    }
    static void error(const std::string& msg);
    static void raw_cmd(const std::string& cmd);
    // The terminal message is presentation only; EXEC always retains cmd.
    // Diagnostic capture/replay preserves both without emitting either early.
    static void command(const std::string& cmd, const std::string& terminal_message);

#ifdef MOGUET_TEST_XDG_STATE_LOG_HOOKS
    static int state_log_descriptor_for_test() noexcept;
    static void reset_for_test() noexcept;
#endif
};
