#include "logging.hpp"
#include "presentation_detail.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"
#include "xdg_state_log.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include <fcntl.h>

namespace {

namespace fs = std::filesystem;
namespace directory_safety = xdg_directory_safety;
namespace state_log = xdg_state_log;

constexpr mode_t PRIVATE_DIRECTORY_MODE = 0700;
constexpr mode_t PRIVATE_LOG_MODE = 0600;

static_assert(!std::is_copy_constructible_v<state_log::PreparedLogFile>);
static_assert(!std::is_copy_assignable_v<state_log::PreparedLogFile>);
static_assert(std::is_nothrow_move_constructible_v<state_log::PreparedLogFile>);
static_assert(std::is_nothrow_move_assignable_v<state_log::PreparedLogFile>);
static_assert(noexcept(Logger::warn_noexcept([]() {
    return std::string();
})));

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void expect_path(
    const fs::path& actual, const fs::path& expected,
    const std::string& context) {
    if(actual != expected) {
        throw std::runtime_error(
            context + ": expected [" + expected.string() +
            "], actual [" + actual.string() + "]");
    }
}

class TemporaryDirectory final {
    fs::path path_;

public:
    TemporaryDirectory() {
        const std::string template_text =
            (fs::temp_directory_path() /
             "moguet-xdg-state-log-test-XXXXXX")
                .string();
        std::vector<char> path_template(
            template_text.begin(), template_text.end());
        path_template.push_back('\0');
        char* created_path = mkdtemp(path_template.data());
        if(created_path == nullptr) {
            throw std::runtime_error(
                "Failed to create XDG state log test root.");
        }
        path_ = created_path;
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept {
        return path_;
    }
};

class OwnedDescriptor final {
    int descriptor_ = -1;

public:
    explicit OwnedDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    OwnedDescriptor(const OwnedDescriptor&) = delete;
    OwnedDescriptor& operator=(const OwnedDescriptor&) = delete;

    OwnedDescriptor(OwnedDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    OwnedDescriptor& operator=(OwnedDescriptor&& other) noexcept {
        if(this == &other) return *this;
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ~OwnedDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(close(descriptor_));
    }

    int get() const noexcept {
        return descriptor_;
    }
};

class ScopedUmask final {
    mode_t previous_;

public:
    explicit ScopedUmask(mode_t mask) : previous_(umask(mask)) {
    }

    ScopedUmask(const ScopedUmask&) = delete;
    ScopedUmask& operator=(const ScopedUmask&) = delete;

    ~ScopedUmask() noexcept {
        static_cast<void>(umask(previous_));
    }
};

class ScopedCurrentDirectory final {
    fs::path previous_;

public:
    explicit ScopedCurrentDirectory(const fs::path& directory)
        : previous_(fs::current_path()) {
        fs::current_path(directory);
    }

    ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
    ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

    ~ScopedCurrentDirectory() noexcept {
        std::error_code error;
        fs::current_path(previous_, error);
    }
};

class ScopedLoggerReset final {
public:
    ScopedLoggerReset() {
        Logger::reset_for_test();
    }

    ScopedLoggerReset(const ScopedLoggerReset&) = delete;
    ScopedLoggerReset& operator=(const ScopedLoggerReset&) = delete;

    ~ScopedLoggerReset() noexcept {
        Logger::reset_for_test();
    }
};

class ScopedStreamCapture final {
    std::ostream& stream_;
    std::ostringstream capture_;
    std::streambuf* previous_;

public:
    explicit ScopedStreamCapture(std::ostream& stream)
        : stream_(stream), previous_(stream_.rdbuf(capture_.rdbuf())) {
    }

    ScopedStreamCapture(const ScopedStreamCapture&) = delete;
    ScopedStreamCapture& operator=(const ScopedStreamCapture&) = delete;

    ~ScopedStreamCapture() noexcept {
        stream_.rdbuf(previous_);
    }

    std::string str() const {
        return capture_.str();
    }
};

void set_mode(const fs::path& path, mode_t mode) {
    if(chmod(path.c_str(), mode) != 0) {
        throw std::runtime_error(
            "Failed to set test path mode: " + path.string());
    }
}

void create_test_directory(
    const fs::path& path, mode_t mode = PRIVATE_DIRECTORY_MODE) {
    if(!fs::create_directory(path)) {
        throw std::runtime_error(
            "Failed to create test directory: " + path.string());
    }
    set_mode(path, mode);
}

fs::path create_state_home(const fs::path& root, const std::string& name) {
    fs::path state_home = root / name;
    create_test_directory(state_home);
    return state_home;
}

void write_all(int descriptor, const std::string& contents) {
    std::size_t offset = 0;
    while(offset < contents.size()) {
        const ssize_t written = write(
            descriptor, contents.data() + offset,
            contents.size() - offset);
        if(written < 0) {
            if(errno == EINTR) continue;
            throw std::runtime_error("Failed to write test file contents.");
        }
        if(written == 0) {
            throw std::runtime_error("Test file write made no progress.");
        }
        offset += static_cast<std::size_t>(written);
    }
}

void create_file_exact(
    const fs::path& path, const std::string& contents = "",
    mode_t mode = PRIVATE_LOG_MODE) {
    const int descriptor = open(
        path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if(descriptor < 0) {
        throw std::runtime_error(
            "Failed to create test file: " + path.string());
    }
    OwnedDescriptor owned(descriptor);
    write_all(owned.get(), contents);
    if(fchmod(owned.get(), mode) != 0) {
        throw std::runtime_error(
            "Failed to set exact test file mode: " + path.string());
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if(!file) {
        throw std::runtime_error(
            "Failed to read test file: " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

class ScopedStderrDescriptorCapture final {
    fs::path path_;
    int saved_descriptor_ = -1;

    void restore() noexcept {
        if(saved_descriptor_ < 0) return;
        while(dup2(saved_descriptor_, STDERR_FILENO) < 0) {
            if(errno == EINTR) continue;
            break;
        }
        static_cast<void>(close(saved_descriptor_));
        saved_descriptor_ = -1;
    }

public:
    explicit ScopedStderrDescriptorCapture(fs::path path)
        : path_(std::move(path)) {
        std::cerr.flush();
        const int capture_descriptor = open(
            path_.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, PRIVATE_LOG_MODE);
        if(capture_descriptor < 0) {
            throw std::runtime_error(
                "Failed to create stderr capture file: " +
                path_.string());
        }
        OwnedDescriptor capture(capture_descriptor);
        const int saved_descriptor =
            fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
        if(saved_descriptor < 0) {
            throw std::runtime_error("Failed to duplicate stderr for capture.");
        }
        if(dup2(capture.get(), STDERR_FILENO) < 0) {
            const int redirect_error = errno;
            static_cast<void>(close(saved_descriptor));
            throw std::system_error(
                redirect_error, std::generic_category(),
                "Failed to redirect stderr for capture");
        }
        saved_descriptor_ = saved_descriptor;
    }

    ScopedStderrDescriptorCapture(
        const ScopedStderrDescriptorCapture&) = delete;
    ScopedStderrDescriptorCapture& operator=(
        const ScopedStderrDescriptorCapture&) = delete;

    ~ScopedStderrDescriptorCapture() noexcept {
        try {
            std::cerr.flush();
        } catch(...) {
        }
        restore();
    }

    std::string finish() {
        std::cerr.flush();
        restore();
        return read_file(path_);
    }
};

class ThrowingWarningFactoryCleanupProbe final {
    bool& factory_called_;
    bool& destructor_completed_;

public:
    ThrowingWarningFactoryCleanupProbe(
        bool& factory_called, bool& destructor_completed) noexcept
        : factory_called_(factory_called),
          destructor_completed_(destructor_completed) {
    }

    ~ThrowingWarningFactoryCleanupProbe() noexcept {
        Logger::warn_noexcept([this]() -> std::string {
            factory_called_ = true;
            throw std::bad_alloc();
        });
        destructor_completed_ = true;
    }
};

class CheckedWarningFailureCleanupProbe final {
    int& factory_calls_;

public:
    explicit CheckedWarningFailureCleanupProbe(int& factory_calls) noexcept
        : factory_calls_(factory_calls) {
    }

    ~CheckedWarningFailureCleanupProbe() noexcept {
        Logger::warn_noexcept([this]() {
            ++factory_calls_;
            return std::string("cleanup warning before checked shutdown");
        });
    }
};

std::vector<std::string> read_lines(const fs::path& path) {
    std::ifstream file(path);
    if(!file) {
        throw std::runtime_error(
            "Failed to read test log lines: " + path.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while(std::getline(file, line))
        lines.push_back(line);
    return lines;
}

struct PathMetadata {
    std::uintmax_t device;
    std::uintmax_t inode;
    std::uintmax_t owner;
    std::uintmax_t links;
    mode_t permissions;
    mode_t type;
};

PathMetadata path_metadata(const fs::path& path) {
    struct stat status{};
    if(lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error(
            "Failed to inspect test path: " + path.string());
    }
    return PathMetadata{
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_uid),
        static_cast<std::uintmax_t>(status.st_nlink),
        static_cast<mode_t>(status.st_mode & 07777),
        static_cast<mode_t>(status.st_mode & S_IFMT)};
}

void expect_metadata_unchanged(
    const PathMetadata& before, const PathMetadata& after,
    const std::string& context) {
    expect(before.device == after.device, context + ": device changed.");
    expect(before.inode == after.inode, context + ": inode changed.");
    expect(before.owner == after.owner, context + ": owner changed.");
    expect(before.links == after.links, context + ": link count changed.");
    expect(
        before.permissions == after.permissions,
        context + ": permissions changed.");
    expect(before.type == after.type, context + ": file type changed.");
}

void expect_descriptor_open(int descriptor, const std::string& context) {
    errno = 0;
    expect(
        descriptor >= 0 && fcntl(descriptor, F_GETFD) >= 0,
        context + ": descriptor is not open.");
}

void expect_descriptor_closed(int descriptor, const std::string& context) {
    errno = 0;
    const int result = fcntl(descriptor, F_GETFD);
    expect(
        result == -1 && errno == EBADF,
        context + ": descriptor is still open.");
}

xdg_paths::StatePaths resolve_explicit_state(const fs::path& state_home) {
    return xdg_paths::resolve_state(xdg_paths::EnvironmentSnapshot{
        .xdg_config_home = std::optional<std::string>{"ignored/relative/config"},
        .xdg_state_home =
            std::optional<std::string>{state_home.string()},
        .xdg_cache_home = std::optional<std::string>{"ignored/relative/cache"},
        .home = std::nullopt,
    });
}

class PreparedStateFixture final {
    TemporaryDirectory temporary_directory_;
    fs::path state_home_;
    xdg_paths::StatePaths paths_;
    directory_safety::PreparedDirectory directory_;

public:
    explicit PreparedStateFixture(const std::string& state_home_name = "state")
        : state_home_(create_state_home(
              temporary_directory_.path(), state_home_name)),
          paths_(resolve_explicit_state(state_home_)),
          directory_(directory_safety::prepare_directory(paths_)) {
    }

    PreparedStateFixture(const PreparedStateFixture&) = delete;
    PreparedStateFixture& operator=(const PreparedStateFixture&) = delete;

    const fs::path& root() const noexcept {
        return temporary_directory_.path();
    }

    const fs::path& state_home() const noexcept {
        return state_home_;
    }

    const xdg_paths::StatePaths& paths() const noexcept {
        return paths_;
    }

    const directory_safety::PreparedDirectory& directory() const noexcept {
        return directory_;
    }
};

template <typename Callable>
state_log::StateLogFailure expect_state_log_error(
    Callable callable, state_log::StateLogStage expected_stage,
    state_log::StateLogErrorCode expected_code,
    std::optional<int> expected_error_number = std::nullopt,
    const std::string& forbidden_diagnostic_fragment = "") {
    try {
        static_cast<void>(callable());
    } catch(const state_log::StateLogError& error) {
        const state_log::StateLogFailure failure = error.failure();
        expect(
            failure.directory_kind == xdg_paths::DirectoryKind::State,
            "Unexpected directory kind in state log failure.");
        expect(failure.stage == expected_stage, "Unexpected state log failure stage.");
        expect(failure.code == expected_code, "Unexpected state log error code.");
        expect(
            failure.system_error.has_value() ==
                expected_error_number.has_value(),
            "Unexpected state log system error presence.");
        if(expected_error_number.has_value()) {
            expect(
                failure.system_error->value() ==
                    expected_error_number.value(),
                "Unexpected state log system error number.");
        }
        if(!forbidden_diagnostic_fragment.empty()) {
            expect(
                std::string(error.what()).find(forbidden_diagnostic_fragment) ==
                    std::string::npos,
                "State log diagnostic exposed an authority path.");
        }
        return failure;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Unexpected state log exception category: " +
            std::string(error.what()));
    }
    throw std::runtime_error("Expected state log failure.");
}

state_log::StateLogTestOverrides injected_failure(
    state_log::StateLogTestFailurePoint failure_point,
    int error_number) {
    state_log::StateLogTestOverrides overrides;
    overrides.injected_failure =
        state_log::StateLogInjectedFailure{failure_point, error_number};
    return overrides;
}

void expect_log_record(
    const std::string& record, const std::string& level,
    const std::string& message) {
    const std::regex pattern(
        "^\\[[0-9]{4}-[0-9]{2}-[0-9]{2} "
        "[0-9]{2}:[0-9]{2}:[0-9]{2}\\] \\[" +
        level + "\\] " + message + "$");
    expect(
        std::regex_match(record, pattern),
        "Unexpected state log record: " + record);
}

void test_state_only_resolution_preparation_and_lazy_creation() {
    {
        TemporaryDirectory temporary_directory;
        const fs::path state_home = create_state_home(
            temporary_directory.path(), "explicit-state");
        const xdg_paths::StatePaths paths = resolve_explicit_state(state_home);

        expect(
            !fs::exists(paths.directory),
            "State-only resolver created the application directory.");
        expect(
            !fs::exists(temporary_directory.path() / "config") &&
                !fs::exists(temporary_directory.path() / "cache"),
            "State-only resolver created an unrelated consumer root.");

        directory_safety::PreparedDirectory directory =
            directory_safety::prepare_directory(paths);
        expect(
            fs::is_directory(paths.directory),
            "State preparation did not create the state directory.");
        expect(
            !fs::exists(paths.default_log_file),
            "State preparation eagerly created the log file.");
        expect(
            directory.created_component_count() == 1,
            "Explicit state preparation created an unexpected component count.");

        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log(paths, directory);
        expect(log_file.created(), "Missing explicit state log was not created.");
        expect(
            fs::is_regular_file(paths.default_log_file),
            "State log open did not create a regular file.");
        expect(
            !fs::exists(temporary_directory.path() / "config") &&
                !fs::exists(temporary_directory.path() / "cache"),
            "State log creation connected an unrelated consumer.");
    }

    {
        TemporaryDirectory temporary_directory;
        const fs::path home = temporary_directory.path() / "home";
        create_test_directory(home);
        const xdg_paths::StatePaths paths = xdg_paths::resolve_state(
            xdg_paths::EnvironmentSnapshot{
                .xdg_config_home =
                    std::optional<std::string>{"ignored/config"},
                .xdg_state_home = std::nullopt,
                .xdg_cache_home =
                    std::optional<std::string>{"ignored/cache"},
                .home = std::optional<std::string>{home.string()},
            });
        expect(
            !fs::exists(home / ".local"),
            "State fallback resolution mutated HOME.");

        directory_safety::PreparedDirectory directory =
            directory_safety::prepare_directory(paths);
        expect_path(
            paths.directory, home / ".local" / "state" / "moguet",
            "HOME fallback state directory");
        expect(
            directory.created_component_count() == 3,
            "HOME fallback state component count mismatch.");
        expect(
            !fs::exists(home / ".config") && !fs::exists(home / ".cache"),
            "HOME state preparation created config or cache directories.");
        expect(
            !fs::exists(paths.default_log_file),
            "HOME state preparation eagerly created the log file.");

        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log(paths, directory);
        expect(log_file.created(), "HOME fallback log was not created lazily.");
        expect_path(
            log_file.logical_path(),
            home / ".local" / "state" / "moguet" / "moguet.log",
            "HOME fallback logical log path");
        expect(
            !fs::exists(home / ".config") && !fs::exists(home / ".cache"),
            "HOME log creation connected config or cache consumers.");
    }
}

void test_boundary_mismatch_and_wrong_directory_kind_are_rejected() {
    PreparedStateFixture fixture;

    xdg_paths::StatePaths wrong_filename = fixture.paths();
    wrong_filename.default_log_file = wrong_filename.directory / "other.log";
    expect_state_log_error(
        [&]() {
            return state_log::open_default_state_log(
                wrong_filename, fixture.directory());
        },
        state_log::StateLogStage::BoundaryValidation,
        state_log::StateLogErrorCode::InvalidStateLogBoundary,
        std::nullopt, fixture.root().string());

    PreparedStateFixture other_fixture("other-state");
    expect_state_log_error(
        [&]() {
            return state_log::open_default_state_log(
                fixture.paths(), other_fixture.directory());
        },
        state_log::StateLogStage::BoundaryValidation,
        state_log::StateLogErrorCode::InvalidStateLogBoundary);

    TemporaryDirectory wrong_kind_root;
    create_test_directory(wrong_kind_root.path() / "config");
    create_test_directory(wrong_kind_root.path() / "state");
    create_test_directory(wrong_kind_root.path() / "cache");
    const xdg_paths::ResolvedPaths all_paths = xdg_paths::resolve(
        xdg_paths::EnvironmentSnapshot{
            .xdg_config_home =
                (wrong_kind_root.path() / "config").string(),
            .xdg_state_home =
                (wrong_kind_root.path() / "state").string(),
            .xdg_cache_home =
                (wrong_kind_root.path() / "cache").string(),
            .home = std::nullopt,
        });
    directory_safety::PreparedDirectory config_directory =
        directory_safety::prepare_directory(all_paths.config);
    expect_state_log_error(
        [&]() {
            return state_log::open_default_state_log(
                all_paths.state, config_directory);
        },
        state_log::StateLogStage::BoundaryValidation,
        state_log::StateLogErrorCode::InvalidStateLogBoundary);

    expect(
        !fs::exists(fixture.paths().default_log_file) &&
            !fs::exists(other_fixture.paths().default_log_file) &&
            !fs::exists(all_paths.state.default_log_file),
        "Boundary rejection created a default log file.");
}

void test_new_log_metadata_flags_and_umask_policy() {
    for(const mode_t mask : std::array<mode_t, 2>{0000, 0077}) {
        PreparedStateFixture fixture;
        ScopedUmask scoped_umask(mask);
        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log(
                fixture.paths(), fixture.directory());

        const PathMetadata metadata =
            path_metadata(fixture.paths().default_log_file);
        expect(metadata.type == S_IFREG, "New state log is not regular.");
        expect(
            metadata.permissions == PRIVATE_LOG_MODE,
            "New state log mode is not exactly 0600.");
        expect(
            metadata.owner == static_cast<std::uintmax_t>(geteuid()),
            "New state log owner does not match effective UID.");
        expect(metadata.links == 1, "New state log link count is not one.");
        expect(log_file.created(), "New state log did not report created=true.");
        expect(
            log_file.directory_kind() == xdg_paths::DirectoryKind::State,
            "Prepared log file has the wrong directory kind.");
        expect(log_file.owner() == metadata.owner, "Prepared log owner mismatch.");
        expect(
            log_file.permissions() == PRIVATE_LOG_MODE,
            "Prepared log permissions mismatch.");
        expect(log_file.device() == metadata.device, "Prepared log device mismatch.");
        expect(log_file.inode() == metadata.inode, "Prepared log inode mismatch.");

        const int file_descriptor =
            state_log::state_log_file_descriptor_for_test(log_file);
        const int directory_descriptor =
            state_log::state_log_directory_descriptor_for_test(log_file);
        const int file_descriptor_flags = fcntl(file_descriptor, F_GETFD);
        const int directory_descriptor_flags =
            fcntl(directory_descriptor, F_GETFD);
        const int status_flags = fcntl(file_descriptor, F_GETFL);
        expect(
            file_descriptor_flags >= 0 &&
                (file_descriptor_flags & FD_CLOEXEC) != 0,
            "State log descriptor is not close-on-exec.");
        expect(
            directory_descriptor_flags >= 0 &&
                (directory_descriptor_flags & FD_CLOEXEC) != 0,
            "Retained state directory descriptor is not close-on-exec.");
        expect(
            status_flags >= 0 &&
                (status_flags & O_ACCMODE) == O_WRONLY &&
                (status_flags & O_APPEND) != 0 &&
                (status_flags & O_NONBLOCK) != 0,
            "State log descriptor flags do not enforce append/nonblocking writes.");
        log_file.require_unchanged_identity();
    }
}

void test_existing_log_is_not_truncated_and_appends() {
    PreparedStateFixture fixture;
    create_file_exact(
        fixture.paths().default_log_file, "existing-prefix\n");
    const PathMetadata before =
        path_metadata(fixture.paths().default_log_file);

    {
        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log(
                fixture.paths(), fixture.directory());
        expect(!log_file.created(), "Existing state log reported created=true.");
        expect(
            read_file(fixture.paths().default_log_file) ==
                "existing-prefix\n",
            "Opening an existing state log changed its contents.");
        const int descriptor =
            state_log::state_log_file_descriptor_for_test(log_file);
        expect(
            lseek(descriptor, 0, SEEK_SET) == 0,
            "Failed to seek the test log descriptor.");
        write_all(descriptor, "appended-through-descriptor\n");
        log_file.require_unchanged_identity();
    }

    const PathMetadata after = path_metadata(fixture.paths().default_log_file);
    expect_metadata_unchanged(before, after, "Existing append target");
    expect(
        read_file(fixture.paths().default_log_file) ==
            "existing-prefix\nappended-through-descriptor\n",
        "O_APPEND did not preserve existing log content.");

    state_log::PreparedLogFile reopened =
        state_log::open_default_state_log(
            fixture.paths(), fixture.directory());
    expect(!reopened.created(), "Repeated state log open reported a new file.");
    expect(
        reopened.inode() == before.inode,
        "Repeated state log open changed the file identity.");
}

void test_symlink_and_non_regular_types_are_rejected() {
    {
        PreparedStateFixture fixture;
        const fs::path target = fixture.root() / "symlink-target";
        create_file_exact(target, "target-sentinel");
        fs::create_symlink(target, fixture.paths().default_log_file);
        const PathMetadata before =
            path_metadata(fixture.paths().default_log_file);
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log(
                    fixture.paths(), fixture.directory());
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::Symlink);
        expect_metadata_unchanged(
            before, path_metadata(fixture.paths().default_log_file),
            "Final symlink rejection");
        expect(
            read_file(target) == "target-sentinel",
            "Final symlink rejection changed its target.");
    }

    {
        PreparedStateFixture fixture;
        const fs::path missing_target = fixture.root() / "missing-target";
        fs::create_symlink(
            missing_target, fixture.paths().default_log_file);
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log(
                    fixture.paths(), fixture.directory());
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::Symlink);
        expect(
            fs::is_symlink(fixture.paths().default_log_file),
            "Dangling symlink was removed or replaced.");
        expect(
            !fs::exists(missing_target),
            "Dangling symlink rejection created its target.");
    }

    {
        PreparedStateFixture fixture;
        create_test_directory(fixture.paths().default_log_file);
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log(
                    fixture.paths(), fixture.directory());
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::NotRegularFile);
        expect(
            fs::is_directory(fixture.paths().default_log_file),
            "Directory rejection removed or replaced the path.");
    }

    {
        PreparedStateFixture fixture;
        if(mkfifo(fixture.paths().default_log_file.c_str(), PRIVATE_LOG_MODE) !=
           0) {
            throw std::runtime_error("Failed to create FIFO test fixture.");
        }
        set_mode(fixture.paths().default_log_file, PRIVATE_LOG_MODE);
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log(
                    fixture.paths(), fixture.directory());
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::NotRegularFile);
        expect(
            path_metadata(fixture.paths().default_log_file).type == S_IFIFO,
            "FIFO rejection removed or replaced the path.");
    }

    {
        PreparedStateFixture fixture;
        create_file_exact(fixture.paths().default_log_file, "socket-seam");
        state_log::StateLogTestOverrides overrides;
        overrides.observed_type =
            state_log::StateLogTestObservedType::Socket;
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log_for_test(
                    fixture.paths(), fixture.directory(), overrides);
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::NotRegularFile);
        expect(
            read_file(fixture.paths().default_log_file) == "socket-seam",
            "Socket seam rejection changed the backing fixture.");
    }

    {
        PreparedStateFixture fixture;
        create_file_exact(fixture.paths().default_log_file, "device-seam");
        state_log::StateLogTestOverrides overrides;
        overrides.observed_type =
            state_log::StateLogTestObservedType::CharacterDevice;
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log_for_test(
                    fixture.paths(), fixture.directory(), overrides);
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::NotRegularFile);
        expect(
            read_file(fixture.paths().default_log_file) == "device-seam",
            "Device seam rejection changed the backing fixture.");
    }
}

void test_existing_mode_policy_is_exact_and_does_not_repair() {
    struct ModeCase {
        const char* name;
        mode_t mode;
    };
    const std::array<ModeCase, 11> cases = {{
        {"owner-read-only", 0400},
        {"owner-write-only", 0200},
        {"group-read", 0640},
        {"other-read", 0604},
        {"group-execute", 0610},
        {"other-execute", 0601},
        {"group-write", 0620},
        {"other-write", 0602},
        {"setuid", 04600},
        {"setgid", 02600},
        {"sticky", 01600},
    }};

    for(const ModeCase& mode_case : cases) {
        PreparedStateFixture fixture;
        create_file_exact(
            fixture.paths().default_log_file, mode_case.name,
            mode_case.mode);
        const PathMetadata before =
            path_metadata(fixture.paths().default_log_file);
        expect(
            before.permissions == mode_case.mode,
            std::string("Test filesystem did not retain mode for ") +
                mode_case.name + ".");
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log(
                    fixture.paths(), fixture.directory());
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::UnsafePermissions);
        expect_metadata_unchanged(
            before, path_metadata(fixture.paths().default_log_file),
            std::string("Unsafe mode rejection ") + mode_case.name);
    }
}

void test_hard_links_and_owner_mismatch_are_rejected_without_repair() {
    {
        PreparedStateFixture fixture;
        const fs::path alias = fixture.root() / "hard-link-alias";
        create_file_exact(
            fixture.paths().default_log_file, "hard-link-sentinel");
        fs::create_hard_link(fixture.paths().default_log_file, alias);
        const PathMetadata before =
            path_metadata(fixture.paths().default_log_file);
        expect(before.links == 2, "Hard-link fixture link count is not two.");
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log(
                    fixture.paths(), fixture.directory());
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::MultipleHardLinks);
        expect_metadata_unchanged(
            before, path_metadata(fixture.paths().default_log_file),
            "Hard-link rejection");
        expect(
            path_metadata(alias).inode == before.inode &&
                read_file(alias) == "hard-link-sentinel",
            "Hard-link rejection changed either link.");
    }

    {
        PreparedStateFixture fixture;
        create_file_exact(
            fixture.paths().default_log_file, "owner-sentinel");
        const PathMetadata before =
            path_metadata(fixture.paths().default_log_file);
        const std::uintmax_t different_owner =
            before.owner == std::numeric_limits<std::uintmax_t>::max()
                ? before.owner - 1
                : before.owner + 1;
        state_log::StateLogTestOverrides overrides;
        overrides.observed_owner = different_owner;
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log_for_test(
                    fixture.paths(), fixture.directory(), overrides);
            },
            state_log::StateLogStage::FileInspection,
            state_log::StateLogErrorCode::OwnershipMismatch);
        expect_metadata_unchanged(
            before, path_metadata(fixture.paths().default_log_file),
            "Owner mismatch rejection");
        expect(
            read_file(fixture.paths().default_log_file) ==
                "owner-sentinel",
            "Owner mismatch rejection changed file contents.");
    }
}

void test_eexist_after_missing_is_reprocessed_as_existing() {
    PreparedStateFixture fixture;
    bool inserted = false;
    state_log::StateLogTestOverrides overrides;
    overrides.event_hook = [&](state_log::StateLogTestEvent event,
                               const fs::path& logical_path) {
        if(event != state_log::StateLogTestEvent::AfterMissingObservation ||
           inserted)
            return;
        expect_path(
            logical_path, fixture.paths().default_log_file,
            "EEXIST race logical path");
        create_file_exact(logical_path, "appeared-after-missing\n");
        inserted = true;
    };

    state_log::PreparedLogFile log_file =
        state_log::open_default_state_log_for_test(
            fixture.paths(), fixture.directory(), overrides);
    expect(inserted, "EEXIST race hook did not create the existing file.");
    expect(
        !log_file.created(),
        "EEXIST-after-missing was incorrectly classified as new creation.");
    expect(
        read_file(fixture.paths().default_log_file) ==
            "appeared-after-missing\n",
        "EEXIST reprocessing truncated or changed existing content.");
    log_file.require_unchanged_identity();
}

void test_metadata_to_fifo_race_is_nonblocking_and_fail_closed() {
    PreparedStateFixture fixture;
    const fs::path displaced = fixture.root() / "displaced-regular-log";
    create_file_exact(
        fixture.paths().default_log_file, "regular-before-race");
    bool swapped = false;
    state_log::StateLogTestOverrides overrides;
    overrides.event_hook = [&](state_log::StateLogTestEvent event,
                               const fs::path&) {
        if(event != state_log::StateLogTestEvent::AfterInitialMetadata ||
           swapped)
            return;
        fs::rename(fixture.paths().default_log_file, displaced);
        if(mkfifo(
               fixture.paths().default_log_file.c_str(),
               PRIVATE_LOG_MODE) != 0) {
            throw std::runtime_error("Failed to create raced FIFO fixture.");
        }
        set_mode(fixture.paths().default_log_file, PRIVATE_LOG_MODE);
        swapped = true;
    };

    const auto started = std::chrono::steady_clock::now();
    expect_state_log_error(
        [&]() {
            return state_log::open_default_state_log_for_test(
                fixture.paths(), fixture.directory(), overrides);
        },
        state_log::StateLogStage::FileOpen,
        state_log::StateLogErrorCode::ConcurrentReplacement, ENXIO);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(
        elapsed < std::chrono::seconds(2),
        "Raced FIFO open did not fail promptly.");
    expect(swapped, "FIFO race hook did not replace the initial file.");
    expect(
        path_metadata(fixture.paths().default_log_file).type == S_IFIFO,
        "FIFO race failure removed or replaced the raced FIFO.");
    expect(
        read_file(displaced) == "regular-before-race",
        "FIFO race failure changed the displaced regular file.");
}

void test_replacement_before_name_revalidation_is_detected() {
    PreparedStateFixture fixture;
    const fs::path displaced = fixture.root() / "opened-original-log";
    create_file_exact(
        fixture.paths().default_log_file, "opened-original");
    bool swapped = false;
    state_log::StateLogTestOverrides overrides;
    overrides.event_hook = [&](state_log::StateLogTestEvent event,
                               const fs::path&) {
        if(event != state_log::StateLogTestEvent::BeforeNameRevalidation ||
           swapped)
            return;
        fs::rename(fixture.paths().default_log_file, displaced);
        create_file_exact(
            fixture.paths().default_log_file, "replacement-sentinel");
        swapped = true;
    };

    expect_state_log_error(
        [&]() {
            return state_log::open_default_state_log_for_test(
                fixture.paths(), fixture.directory(), overrides);
        },
        state_log::StateLogStage::NameRevalidation,
        state_log::StateLogErrorCode::ConcurrentReplacement);
    expect(swapped, "Name revalidation race hook did not run.");
    expect(
        read_file(displaced) == "opened-original",
        "Name revalidation failure changed the opened inode.");
    expect(
        read_file(fixture.paths().default_log_file) ==
            "replacement-sentinel",
        "Name revalidation failure changed the replacement inode.");
}

void test_injected_syscall_errors_preserve_stage_code_and_errno() {
    struct FailureCase {
        const char* name;
        state_log::StateLogTestFailurePoint failure_point;
        bool needs_existing_file;
        int error_number;
        state_log::StateLogStage expected_stage;
        state_log::StateLogErrorCode expected_code;
    };
    const std::array<FailureCase, 11> cases = {{
        {"directory-duplication-metadata",
         state_log::StateLogTestFailurePoint::DirectoryDescriptorDuplication,
         false, EIO,
         state_log::StateLogStage::DirectoryDescriptorDuplication,
         state_log::StateLogErrorCode::MetadataFailure},
        {"directory-duplication-permission",
         state_log::StateLogTestFailurePoint::DirectoryDescriptorDuplication,
         false, EACCES,
         state_log::StateLogStage::DirectoryDescriptorDuplication,
         state_log::StateLogErrorCode::PermissionDenied},
        {"initial-metadata",
         state_log::StateLogTestFailurePoint::InitialMetadata, false, EIO,
         state_log::StateLogStage::FileInspection,
         state_log::StateLogErrorCode::MetadataFailure},
        {"initial-metadata-permission",
         state_log::StateLogTestFailurePoint::InitialMetadata, false,
         EACCES, state_log::StateLogStage::FileInspection,
         state_log::StateLogErrorCode::PermissionDenied},
        {"file-creation",
         state_log::StateLogTestFailurePoint::FileCreation, false, EIO,
         state_log::StateLogStage::FileCreation,
         state_log::StateLogErrorCode::OpenFailed},
        {"file-creation-permission",
         state_log::StateLogTestFailurePoint::FileCreation, false, EROFS,
         state_log::StateLogStage::FileCreation,
         state_log::StateLogErrorCode::PermissionDenied},
        {"file-open",
         state_log::StateLogTestFailurePoint::FileOpen, true, EIO,
         state_log::StateLogStage::FileOpen,
         state_log::StateLogErrorCode::OpenFailed},
        {"file-open-replacement",
         state_log::StateLogTestFailurePoint::FileOpen, true, ENOENT,
         state_log::StateLogStage::FileOpen,
         state_log::StateLogErrorCode::ConcurrentReplacement},
        {"descriptor-metadata",
         state_log::StateLogTestFailurePoint::DescriptorMetadata, true,
         EIO, state_log::StateLogStage::DescriptorValidation,
         state_log::StateLogErrorCode::MetadataFailure},
        {"name-revalidation-metadata",
         state_log::StateLogTestFailurePoint::NameRevalidation, true, EIO,
         state_log::StateLogStage::NameRevalidation,
         state_log::StateLogErrorCode::MetadataFailure},
        {"name-revalidation-replacement",
         state_log::StateLogTestFailurePoint::NameRevalidation, true,
         ENOENT, state_log::StateLogStage::NameRevalidation,
         state_log::StateLogErrorCode::ConcurrentReplacement},
    }};

    for(const FailureCase& failure_case : cases) {
        PreparedStateFixture fixture;
        std::optional<PathMetadata> before;
        if(failure_case.needs_existing_file) {
            create_file_exact(
                fixture.paths().default_log_file,
                std::string(failure_case.name) + "-sentinel");
            before = path_metadata(fixture.paths().default_log_file);
        }
        const state_log::StateLogTestOverrides overrides = injected_failure(
            failure_case.failure_point, failure_case.error_number);
        expect_state_log_error(
            [&]() {
                return state_log::open_default_state_log_for_test(
                    fixture.paths(), fixture.directory(), overrides);
            },
            failure_case.expected_stage, failure_case.expected_code,
            failure_case.error_number);
        if(before.has_value()) {
            expect_metadata_unchanged(
                before.value(),
                path_metadata(fixture.paths().default_log_file),
                std::string("Injected failure ") + failure_case.name);
        } else {
            expect(
                !fs::exists(fixture.paths().default_log_file),
                std::string("Injected failure created a log file: ") +
                    failure_case.name);
        }
    }
}

void test_open_is_independent_of_current_working_directory() {
    PreparedStateFixture fixture;
    const fs::path working_directory = fixture.root() / "working-directory";
    create_test_directory(working_directory);
    const fs::path cwd_log = working_directory / "moguet.log";
    create_file_exact(cwd_log, "cwd-sentinel");

    {
        ScopedCurrentDirectory changed_directory(working_directory);
        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log(
                fixture.paths(), fixture.directory());
        expect(log_file.created(), "CWD-independent log was not created.");
        expect_path(
            log_file.logical_path(), fixture.paths().default_log_file,
            "CWD-independent logical path");
    }

    expect(
        read_file(cwd_log) == "cwd-sentinel",
        "State log open used the current working directory.");
    expect(
        fs::is_regular_file(fixture.paths().default_log_file),
        "State log was not created at the resolved absolute path.");
}

void test_prepared_log_file_move_ownership_and_close_contract() {
    int moved_file_descriptor = -1;
    int moved_directory_descriptor = -1;
    {
        PreparedStateFixture fixture;
        state_log::PreparedLogFile source =
            state_log::open_default_state_log(
                fixture.paths(), fixture.directory());
        moved_file_descriptor =
            state_log::state_log_file_descriptor_for_test(source);
        moved_directory_descriptor =
            state_log::state_log_directory_descriptor_for_test(source);
        {
            state_log::PreparedLogFile destination(std::move(source));
            expect(
                state_log::state_log_file_descriptor_for_test(source) == -1 &&
                    state_log::state_log_directory_descriptor_for_test(
                        source) == -1,
                "Move construction did not empty the source log capability.");
            expect(
                state_log::state_log_file_descriptor_for_test(destination) ==
                        moved_file_descriptor &&
                    state_log::state_log_directory_descriptor_for_test(
                        destination) == moved_directory_descriptor,
                "Move construction changed owned descriptors.");
            expect_descriptor_open(
                moved_file_descriptor,
                "Move-constructed file descriptor");
            expect_descriptor_open(
                moved_directory_descriptor,
                "Move-constructed directory descriptor");
        }
        expect_descriptor_closed(
            moved_file_descriptor,
            "Destroyed move-constructed file descriptor");
        expect_descriptor_closed(
            moved_directory_descriptor,
            "Destroyed move-constructed directory descriptor");
    }

    int transferred_file_descriptor = -1;
    int transferred_directory_descriptor = -1;
    {
        TemporaryDirectory temporary_directory;
        const fs::path first_state = create_state_home(
            temporary_directory.path(), "first-state");
        const fs::path second_state = create_state_home(
            temporary_directory.path(), "second-state");
        const xdg_paths::StatePaths first_paths =
            resolve_explicit_state(first_state);
        const xdg_paths::StatePaths second_paths =
            resolve_explicit_state(second_state);
        directory_safety::PreparedDirectory first_directory =
            directory_safety::prepare_directory(first_paths);
        directory_safety::PreparedDirectory second_directory =
            directory_safety::prepare_directory(second_paths);
        state_log::PreparedLogFile source =
            state_log::open_default_state_log(
                first_paths, first_directory);
        state_log::PreparedLogFile destination =
            state_log::open_default_state_log(
                second_paths, second_directory);
        transferred_file_descriptor =
            state_log::state_log_file_descriptor_for_test(source);
        transferred_directory_descriptor =
            state_log::state_log_directory_descriptor_for_test(source);
        const int replaced_file_descriptor =
            state_log::state_log_file_descriptor_for_test(destination);
        const int replaced_directory_descriptor =
            state_log::state_log_directory_descriptor_for_test(destination);

        destination = std::move(source);
        expect_descriptor_closed(
            replaced_file_descriptor,
            "Move assignment replaced file descriptor");
        expect_descriptor_closed(
            replaced_directory_descriptor,
            "Move assignment replaced directory descriptor");
        expect(
            state_log::state_log_file_descriptor_for_test(source) == -1 &&
                state_log::state_log_directory_descriptor_for_test(
                    source) == -1,
            "Move assignment did not empty the source capability.");
        expect(
            state_log::state_log_file_descriptor_for_test(destination) ==
                    transferred_file_descriptor &&
                state_log::state_log_directory_descriptor_for_test(
                    destination) ==
                    transferred_directory_descriptor,
            "Move assignment did not transfer descriptor ownership.");
        expect_descriptor_open(
            transferred_file_descriptor,
            "Move-assigned file descriptor");
        expect_descriptor_open(
            transferred_directory_descriptor,
            "Move-assigned directory descriptor");
    }
    expect_descriptor_closed(
        transferred_file_descriptor,
        "Destroyed move-assigned file descriptor");
    expect_descriptor_closed(
        transferred_directory_descriptor,
        "Destroyed move-assigned directory descriptor");
}

void test_logger_adoption_format_visibility_append_and_reset_close() {
    PreparedStateFixture fixture;
    ScopedLoggerReset logger_reset;
    ScopedStreamCapture stdout_capture(std::cout);
    ScopedStreamCapture stderr_capture(std::cerr);

    state_log::PreparedLogFile log_file =
        state_log::open_default_state_log(
            fixture.paths(), fixture.directory());
    const int descriptor =
        state_log::state_log_file_descriptor_for_test(log_file);
    Logger::init(std::move(log_file), "state-log-initialization");
    expect(
        state_log::state_log_file_descriptor_for_test(log_file) == -1,
        "Logger adoption did not consume file descriptor ownership.");
    expect(
        Logger::state_log_descriptor_for_test() == descriptor,
        "Logger did not retain the validated descriptor.");

    std::vector<std::string> lines =
        read_lines(fixture.paths().default_log_file);
    expect(
        lines.size() == 1,
        "Initialization INFO record was not immediately visible.");
    expect_log_record(lines[0], "INFO", "state-log-initialization");

    Logger::info("state-log-info");
    Logger::warn("state-log-warn");
    Logger::error("state-log-error");
    Logger::raw_cmd("state-log-command");
    lines = read_lines(fixture.paths().default_log_file);
    expect(lines.size() == 5, "Logger did not append all record levels.");
    expect_log_record(lines[0], "INFO", "state-log-initialization");
    expect_log_record(lines[1], "INFO", "state-log-info");
    expect_log_record(lines[2], "WARN", "state-log-warn");
    expect_log_record(lines[3], "ERROR", "state-log-error");
    expect_log_record(lines[4], "EXEC", "state-log-command");
    expect(
        stdout_capture.str().find("state-log-initialization") !=
                std::string::npos &&
            stdout_capture.str().find("state-log-info") !=
                std::string::npos &&
            stdout_capture.str().find("state-log-warn") !=
                std::string::npos &&
            stdout_capture.str().find("Running: state-log-command") !=
                std::string::npos,
        "Logger terminal output behavior changed for state log adoption.");
    expect(
        stderr_capture.str().find("state-log-error") != std::string::npos,
        "Logger error output did not remain on stderr.");

    Logger::shutdown();
    expect(
        Logger::state_log_descriptor_for_test() == -1,
        "Logger shutdown retained a state log descriptor.");
    expect_descriptor_closed(descriptor, "Logger shutdown file descriptor");

    state_log::PreparedLogFile reopened =
        state_log::open_default_state_log(
            fixture.paths(), fixture.directory());
    expect(!reopened.created(), "Logger reinitialization recreated the log file.");
    const int reopened_descriptor =
        state_log::state_log_file_descriptor_for_test(reopened);
    Logger::init(std::move(reopened), "state-log-second-init");
    Logger::reset_for_test();
    expect_descriptor_closed(
        reopened_descriptor, "Logger test reset file descriptor");
    lines = read_lines(fixture.paths().default_log_file);
    expect(lines.size() == 6, "Logger reinitialization truncated prior records.");
    expect_log_record(lines[5], "INFO", "state-log-second-init");
}

std::size_t count_text_occurrences(
    const std::string& text, const std::string& fragment) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while((offset = text.find(fragment, offset)) != std::string::npos) {
        ++count;
        offset += fragment.size();
    }
    return count;
}

enum class OutputFixtureKind { Terminal,
                               Pipe,
                               File };

// A small real-descriptor fixture: Logger still writes through its production
// cout/cerr buffers. No terminal detection or presentation is mocked.
class ScopedOutputDescriptor final {
    int target_;
    OwnedDescriptor saved_, reader_, writer_;

public:
    ScopedOutputDescriptor(int target, OutputFixtureKind kind, const fs::path& path)
        : target_(target), saved_(fcntl(target, F_DUPFD_CLOEXEC, 0)) {
        expect(saved_.get() >= 0, "Failed to save output descriptor");
        if(kind == OutputFixtureKind::Terminal) {
            reader_ = OwnedDescriptor(posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK));
            expect(reader_.get() >= 0 && grantpt(reader_.get()) == 0 && unlockpt(reader_.get()) == 0,
                   "Failed to create output PTY");
            const char* name = ptsname(reader_.get());
            expect(name != nullptr, "Failed to resolve output PTY");
            writer_ = OwnedDescriptor(open(name, O_RDWR | O_NOCTTY | O_CLOEXEC));
        } else if(kind == OutputFixtureKind::Pipe) {
            int descriptors[2];
            expect(pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) == 0, "Failed to create output pipe");
            reader_ = OwnedDescriptor(descriptors[0]);
            writer_ = OwnedDescriptor(descriptors[1]);
        } else {
            writer_ = OwnedDescriptor(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, PRIVATE_LOG_MODE));
            reader_ = OwnedDescriptor(open(path.c_str(), O_RDONLY | O_CLOEXEC));
        }
        expect(writer_.get() >= 0 && reader_.get() >= 0, "Failed to open output fixture");
        std::cout.flush();
        std::cerr.flush();
        expect(dup2(writer_.get(), target_) >= 0, "Failed to redirect output descriptor");
        // A failure here unwinds only after installation, so restore explicitly.
        if((isatty(target_) != 0) != (kind == OutputFixtureKind::Terminal)) {
            static_cast<void>(dup2(saved_.get(), target_));
            throw std::runtime_error("Output fixture has wrong TTY state");
        }
    }

    ~ScopedOutputDescriptor() noexcept {
        static_cast<void>(dup2(saved_.get(), target_));
    }

    std::string drain() const {
        std::cout.flush();
        std::cerr.flush();
        std::string output;
        std::array<char, 4096> buffer;
        for(;;) {
            const auto count = read(reader_.get(), buffer.data(), buffer.size());
            if(count > 0)
                output.append(buffer.data(), static_cast<std::size_t>(count));
            else if(count == 0 || (count < 0 && errno == EAGAIN))
                return output;
            else if(errno != EINTR)
                throw std::runtime_error("Failed to read output fixture");
        }
    }
};

void test_logger_command_output_environment_matrix() {
    using Kind = OutputFixtureKind;
    const std::array<std::pair<Kind, Kind>, 5> environments{{{Kind::Terminal, Kind::Terminal}, {Kind::Pipe, Kind::Pipe}, {Kind::File, Kind::Terminal}, {Kind::Terminal, Kind::File}, {Kind::File, Kind::File}}};
    const std::string command = "'/usr/bin/git' 'fetch' '--' 'https://example.test/upstream.git' 'exact-object'";
    const std::string summary = "compact-root-tag-summary (1 object)";
    unsigned cells = 0;
    for(const auto& [stdout_kind, stderr_kind] : environments) {
        for(const bool stderr_route : {false, true}) {
            for(const bool captured : {false, true}) {
                for(const auto detail : {PresentationDetail::Normal, PresentationDetail::Detailed}) {
                    PreparedStateFixture fixture;
                    TemporaryDirectory output_root;
                    ScopedLoggerReset reset;
                    auto log_file = state_log::open_default_state_log(fixture.paths(), fixture.directory());
                    ScopedOutputDescriptor out(STDOUT_FILENO, stdout_kind, output_root.path() / "stdout");
                    ScopedOutputDescriptor err(STDERR_FILENO, stderr_kind, output_root.path() / "stderr");
                    if(stderr_route) Logger::set_diagnostics_to_stderr();
                    Logger::init(std::move(log_file), "matrix-started");
                    static_cast<void>(out.drain());
                    static_cast<void>(err.drain());
                    const auto initial_records = read_lines(fixture.paths().default_log_file);
                    std::optional<ScopedLoggerDiagnosticCapture> capture;
                    if(captured) capture.emplace();
                    if(detail == PresentationDetail::Detailed)
                        Logger::raw_cmd(command);
                    else
                        Logger::command(command, summary);
                    Logger::error("root-fetch-failure");
                    if(capture) {
                        expect(out.drain().empty() && err.drain().empty() &&
                                   read_lines(fixture.paths().default_log_file) == initial_records,
                               "Capture leaked command/failure before replay");
                        capture->replay();
                        capture->replay();
                    }
                    Logger::shutdown();
                    const auto stdout_text = out.drain();
                    const auto stderr_text = err.drain();
                    const auto& presentation = stderr_route ? stderr_text : stdout_text;
                    const auto& other = stderr_route ? stdout_text : stderr_text;
                    const std::string expected = detail == PresentationDetail::Detailed ? "Running: " + command : summary;
                    expect(count_text_occurrences(presentation, expected) == 1 && other.find(expected) == std::string::npos,
                           "Output environment changed command ownership/cardinality");
                    expect(stdout_text.find("root-fetch-failure") == std::string::npos &&
                               count_text_occurrences(stderr_text, "root-fetch-failure") == 1,
                           "Output environment changed error ownership/cardinality");
                    expect(presentation.find("\033[1;33m::\033[0m ") != std::string::npos &&
                               stderr_text.find("\033[1;31m::\033[0m ") != std::string::npos,
                           "Output environment changed existing ANSI contract");
                    if(detail == PresentationDetail::Normal) expect((stdout_text + stderr_text).find(command) == std::string::npos,
                                                                    "Normal output leaked exact command");
                    const auto lines = read_lines(fixture.paths().default_log_file);
                    expect(lines.size() == 3, "Output environment lost or duplicated state records");
                    expect_log_record(lines[1], "EXEC", command);
                    expect_log_record(lines[2], "ERROR", "root-fetch-failure");
                    ++cells;
                }
            }
        }
    }
    expect(cells == 40, "Output environment matrix lost coverage");
}

void test_logger_command_presentation_preserves_exec() {
    const std::string command = "'/usr/bin/git' 'fetch' '--' 'https://example.test/upstream.git' 'exact-object'";
    for(const std::string& presentation : {std::string("compact-summary"), "Running: " + command}) {
        PreparedStateFixture fixture;
        ScopedLoggerReset logger_reset;
        ScopedStreamCapture stdout_capture(std::cout);
        auto log_file = state_log::open_default_state_log(fixture.paths(), fixture.directory());
        Logger::init(std::move(log_file), "command-started");
        Logger::command(command, presentation);
        Logger::shutdown();
        const auto lines = read_lines(fixture.paths().default_log_file);
        expect(lines.size() == 2, "Command presentation changed persistent record cardinality.");
        expect_log_record(lines[1], "EXEC", command);
        expect(stdout_capture.str().find(presentation) != std::string::npos,
               "Command terminal presentation was lost.");
        if(presentation == "compact-summary")
            expect(stdout_capture.str().find(command) == std::string::npos,
                   "Compact terminal presentation leaked exact command.");
    }
}

void test_logger_diagnostic_capture_replays_once_and_releases_scope() {
    PreparedStateFixture fixture;
    ScopedLoggerReset logger_reset;
    ScopedStreamCapture stdout_capture(std::cout);
    ScopedStreamCapture stderr_capture(std::cerr);
    ScopedLoggerDiagnosticCapture capture;

    Logger::info("captured-info");
    Logger::warn("captured-warning");
    Logger::error("captured-error");
    Logger::raw_cmd("captured-command");
    Logger::command("exact-bulk-command", "compact-bulk-summary");
    expect(
        stdout_capture.str().empty() && stderr_capture.str().empty(),
        "Diagnostic capture emitted before replay.");

    capture.stop();
    state_log::PreparedLogFile log_file =
        state_log::open_default_state_log(
            fixture.paths(), fixture.directory());
    Logger::init(std::move(log_file), "capture-started");
    capture.replay();
    capture.replay();

    const std::vector<std::string> lines =
        read_lines(fixture.paths().default_log_file);
    expect(lines.size() == 6, "Captured diagnostics were lost or duplicated.");
    expect_log_record(lines[0], "INFO", "capture-started");
    expect_log_record(lines[1], "INFO", "captured-info");
    expect_log_record(lines[2], "WARN", "captured-warning");
    expect_log_record(lines[3], "ERROR", "captured-error");
    expect_log_record(lines[4], "EXEC", "captured-command");
    expect_log_record(lines[5], "EXEC", "exact-bulk-command");
    const std::string stdout_text = stdout_capture.str();
    expect(count_text_occurrences(stdout_text, "compact-bulk-summary") == 1 &&
               stdout_text.find("exact-bulk-command") == std::string::npos,
           "Captured command presentation leaked EXEC or lost its summary.");
    expect(
        stdout_text.find("capture-started") <
                stdout_text.find("captured-info") &&
            count_text_occurrences(stdout_text, "captured-info") ==
                1 &&
            count_text_occurrences(
                stdout_text, "captured-warning") == 1 &&
            count_text_occurrences(
                stdout_text, "captured-command") == 1 &&
            count_text_occurrences(
                stderr_capture.str(), "captured-error") == 1,
        "Captured diagnostics changed order, owner, or cardinality.");

    Logger::shutdown();
    {
        ScopedLoggerDiagnosticCapture abandoned_capture;
        Logger::info("abandoned-diagnostic");
    }
    Logger::info("after-abandoned-capture");
    expect(
        stdout_capture.str().find("abandoned-diagnostic") ==
                std::string::npos &&
            count_text_occurrences(
                stdout_capture.str(),
                "after-abandoned-capture") == 1,
        "Destroyed diagnostic capture leaked into a later route.");
}

void test_logger_rejects_invalid_descriptor_flags_without_consuming_owner() {
    PreparedStateFixture fixture;
    ScopedLoggerReset logger_reset;
    state_log::PreparedLogFile log_file =
        state_log::open_default_state_log(
            fixture.paths(), fixture.directory());
    const int descriptor =
        state_log::state_log_file_descriptor_for_test(log_file);
    const int descriptor_flags = fcntl(descriptor, F_GETFD);
    expect(descriptor_flags >= 0, "Failed to read logger adoption flags.");
    expect(
        fcntl(descriptor, F_SETFD, descriptor_flags & ~FD_CLOEXEC) == 0,
        "Failed to clear close-on-exec for adoption failure fixture.");

    expect_state_log_error(
        [&]() {
            Logger::init(
                std::move(log_file),
                "must-not-initialize-invalid-descriptor");
            return 0;
        },
        state_log::StateLogStage::DescriptorAdoption,
        state_log::StateLogErrorCode::DescriptorAdoptionFailure, EINVAL);
    expect(
        state_log::state_log_file_descriptor_for_test(log_file) == descriptor,
        "Failed Logger adoption consumed descriptor ownership.");
    expect_descriptor_open(descriptor, "Rejected Logger adoption descriptor");
    expect(
        Logger::state_log_descriptor_for_test() == -1,
        "Failed Logger adoption initialized the global backend.");
}

void test_logger_checked_initial_write_and_close_failures() {
    {
        PreparedStateFixture fixture;
        ScopedLoggerReset logger_reset;
        ScopedStreamCapture stdout_capture(std::cout);
        const state_log::StateLogTestOverrides overrides = injected_failure(
            state_log::StateLogTestFailurePoint::RecordWrite, EIO);
        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log_for_test(
                fixture.paths(), fixture.directory(), overrides);
        const int file_descriptor =
            state_log::state_log_file_descriptor_for_test(log_file);
        const int directory_descriptor =
            state_log::state_log_directory_descriptor_for_test(log_file);

        expect_state_log_error(
            [&]() {
                Logger::init(
                    std::move(log_file),
                    "state-log-initial-write-failure");
                return 0;
            },
            state_log::StateLogStage::RecordWrite,
            state_log::StateLogErrorCode::WriteFailure, EIO);
        expect(
            state_log::state_log_file_descriptor_for_test(log_file) == -1 &&
                state_log::state_log_directory_descriptor_for_test(
                    log_file) == -1,
            "Failed checked initialization did not consume moved ownership.");
        expect_descriptor_closed(
            file_descriptor,
            "Initial-write-failed Logger file descriptor");
        expect_descriptor_closed(
            directory_descriptor,
            "Initial-write-failed Logger directory descriptor");
        expect(
            Logger::state_log_descriptor_for_test() == -1,
            "Initial write failure retained the Logger backend.");
        expect(
            read_file(fixture.paths().default_log_file).empty(),
            "Initial write failure left a partial record.");
        expect(
            stdout_capture.str().find("state-log-initial-write-failure") !=
                std::string::npos,
            "Checked initialization changed terminal output behavior.");
    }

    struct CloseFailureCase {
        const char* name;
        state_log::StateLogTestFailurePoint failure_point;
        int error_number;
    };
    const std::array<CloseFailureCase, 2> cases = {{
        {"file-close", state_log::StateLogTestFailurePoint::FileClose,
         EIO},
        {"directory-close",
         state_log::StateLogTestFailurePoint::DirectoryClose, EBADF},
    }};

    for(const CloseFailureCase& failure_case : cases) {
        PreparedStateFixture fixture(failure_case.name);
        ScopedLoggerReset logger_reset;
        ScopedStreamCapture stdout_capture(std::cout);
        const state_log::StateLogTestOverrides overrides = injected_failure(
            failure_case.failure_point, failure_case.error_number);
        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log_for_test(
                fixture.paths(), fixture.directory(), overrides);
        const int file_descriptor =
            state_log::state_log_file_descriptor_for_test(log_file);
        const int directory_descriptor =
            state_log::state_log_directory_descriptor_for_test(log_file);
        const std::string initial_message =
            std::string("state-log-") + failure_case.name;
        Logger::init(std::move(log_file), initial_message);

        expect_state_log_error(
            []() {
                Logger::shutdown();
                return 0;
            },
            state_log::StateLogStage::DescriptorClose,
            state_log::StateLogErrorCode::CloseFailure,
            failure_case.error_number);
        expect(
            Logger::state_log_descriptor_for_test() == -1,
            std::string("Checked ") + failure_case.name +
                " failure retained the Logger backend.");
        expect_descriptor_closed(
            file_descriptor,
            std::string("Checked ") + failure_case.name +
                " file descriptor");
        expect_descriptor_closed(
            directory_descriptor,
            std::string("Checked ") + failure_case.name +
                " directory descriptor");
        const std::vector<std::string> lines =
            read_lines(fixture.paths().default_log_file);
        expect(
            lines.size() == 1,
            std::string("Checked ") + failure_case.name +
                " changed the initialized record count.");
        expect_log_record(lines[0], "INFO", initial_message);
        Logger::shutdown();
    }
}

void test_logger_noexcept_warning_factory_and_pending_failure() {
    {
        TemporaryDirectory temporary_directory;
        ScopedLoggerReset logger_reset;
        ScopedStreamCapture stdout_capture(std::cout);
        int factory_calls = 0;
        Logger::warn_noexcept([&factory_calls]() {
            ++factory_calls;
            return std::string(
                "Refusing unsafe cleanup for rich-path: rich error");
        });
        expect(factory_calls == 1, "Warning message factory was not called once.");
        expect(
            stdout_capture.str().find(
                "Refusing unsafe cleanup for rich-path: rich error") !=
                std::string::npos,
            "Successful warning factory changed the rich diagnostic.");

        ScopedStderrDescriptorCapture stderr_capture(
            temporary_directory.path() / "factory-fallback.stderr");
        bool factory_called = false;
        bool destructor_completed = false;
        {
            ThrowingWarningFactoryCleanupProbe cleanup_probe(
                factory_called, destructor_completed);
        }
        const std::string fallback = stderr_capture.finish();
        expect(factory_called, "Throwing warning factory was not evaluated.");
        expect(
            destructor_completed,
            "Throwing warning factory escaped the noexcept destructor.");
        expect(
            fallback.find(
                "Cleanup warning could not be constructed or logged "
                "safely.") != std::string::npos,
            "Throwing warning factory did not emit the fixed fallback.");
    }

    {
        PreparedStateFixture fixture("warning-pending-state");
        ScopedLoggerReset logger_reset;
        ScopedStreamCapture stdout_capture(std::cout);
        state_log::PreparedLogFile log_file =
            state_log::open_default_state_log(
                fixture.paths(), fixture.directory());
        Logger::init(std::move(log_file), "warning-pending-initialization");

        const fs::path displaced =
            fixture.root() / "warning-pending-retained-log";
        fs::rename(fixture.paths().default_log_file, displaced);
        create_file_exact(
            fixture.paths().default_log_file,
            "warning-pending-replacement");

        ScopedStderrDescriptorCapture stderr_capture(
            fixture.root() / "checked-fallback.stderr");
        int checked_factory_calls = 0;
        {
            CheckedWarningFailureCleanupProbe cleanup_probe(
                checked_factory_calls);
        }
        {
            bool factory_called = false;
            bool destructor_completed = false;
            {
                ThrowingWarningFactoryCleanupProbe cleanup_probe(
                    factory_called, destructor_completed);
            }
            expect(
                factory_called && destructor_completed,
                "Pending failure was lost through a throwing cleanup "
                "factory.");
        }
        const std::string fallback = stderr_capture.finish();
        expect(
            checked_factory_calls == 1,
            "Checked warning failure factory was not called once.");
        const std::string fallback_fragment =
            "Cleanup warning could not be constructed or logged safely.";
        const std::size_t first_fallback = fallback.find(fallback_fragment);
        expect(
            first_fallback != std::string::npos &&
                fallback.find(
                    fallback_fragment,
                    first_fallback + fallback_fragment.size()) !=
                    std::string::npos,
            "Checked logging and factory failures did not both use the "
            "fixed fallback.");
        expect(
            read_file(fixture.paths().default_log_file) ==
                "warning-pending-replacement",
            "Noexcept warning reopened or changed the replacement inode.");
        const std::vector<std::string> displaced_lines =
            read_lines(displaced);
        expect(
            displaced_lines.size() == 1,
            "Noexcept warning wrote to the displaced retained inode.");
        expect_log_record(
            displaced_lines[0], "INFO",
            "warning-pending-initialization");

        expect_state_log_error(
            []() {
                Logger::shutdown();
                return 0;
            },
            state_log::StateLogStage::NameRevalidation,
            state_log::StateLogErrorCode::ConcurrentReplacement);
        Logger::shutdown();
    }
}

void test_logger_rejects_name_replacement_before_write() {
    PreparedStateFixture fixture;
    ScopedLoggerReset logger_reset;
    ScopedStreamCapture stdout_capture(std::cout);
    state_log::PreparedLogFile log_file =
        state_log::open_default_state_log(
            fixture.paths(), fixture.directory());
    const int descriptor =
        state_log::state_log_file_descriptor_for_test(log_file);
    Logger::init(std::move(log_file), "state-log-before-replacement");

    const fs::path displaced = fixture.root() / "logger-retained-log";
    fs::rename(fixture.paths().default_log_file, displaced);
    create_file_exact(
        fixture.paths().default_log_file, "logger-replacement-sentinel");
    expect_state_log_error(
        []() {
            Logger::info("must-not-reach-either-inode");
            return 0;
        },
        state_log::StateLogStage::NameRevalidation,
        state_log::StateLogErrorCode::ConcurrentReplacement);
    const std::vector<std::string> displaced_lines = read_lines(displaced);
    expect(
        displaced_lines.size() == 1,
        "Logger wrote to the retained inode after name replacement.");
    expect_log_record(
        displaced_lines[0], "INFO", "state-log-before-replacement");
    expect(
        read_file(fixture.paths().default_log_file) ==
            "logger-replacement-sentinel",
        "Logger reopened or changed the replacement inode.");
    expect(
        stdout_capture.str().find("must-not-reach-either-inode") !=
            std::string::npos,
        "Logger terminal diagnostic disappeared on log write rejection.");
    expect(
        Logger::state_log_descriptor_for_test() == -1,
        "Logger retained a backend that failed identity revalidation.");
    expect_descriptor_closed(
        descriptor, "Replacement-rejected Logger descriptor");

    expect_state_log_error(
        []() {
            Logger::shutdown();
            return 0;
        },
        state_log::StateLogStage::NameRevalidation,
        state_log::StateLogErrorCode::ConcurrentReplacement);
    Logger::shutdown();
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        run_case(
            "state-only lazy creation",
            test_state_only_resolution_preparation_and_lazy_creation);
        run_case(
            "boundary mismatch and wrong kind",
            test_boundary_mismatch_and_wrong_directory_kind_are_rejected);
        run_case(
            "new log metadata flags and umask",
            test_new_log_metadata_flags_and_umask_policy);
        run_case(
            "existing log append without truncation",
            test_existing_log_is_not_truncated_and_appends);
        run_case(
            "symlink and non-regular rejection",
            test_symlink_and_non_regular_types_are_rejected);
        run_case(
            "exact existing mode policy",
            test_existing_mode_policy_is_exact_and_does_not_repair);
        run_case(
            "hard-link and owner rejection",
            test_hard_links_and_owner_mismatch_are_rejected_without_repair);
        run_case(
            "EEXIST after missing",
            test_eexist_after_missing_is_reprocessed_as_existing);
        run_case(
            "metadata-to-FIFO race is nonblocking",
            test_metadata_to_fifo_race_is_nonblocking_and_fail_closed);
        run_case(
            "replacement before name revalidation",
            test_replacement_before_name_revalidation_is_detected);
        run_case(
            "injected syscall error classification",
            test_injected_syscall_errors_preserve_stage_code_and_errno);
        run_case(
            "current working directory independence",
            test_open_is_independent_of_current_working_directory);
        run_case(
            "PreparedLogFile move and close ownership",
            test_prepared_log_file_move_ownership_and_close_contract);
        run_case(
            "Logger adoption format visibility and reset",
            test_logger_adoption_format_visibility_append_and_reset_close);
        run_case(
            "Logger diagnostic capture one-shot scope",
            test_logger_diagnostic_capture_replays_once_and_releases_scope);
        run_case(
            "Logger command presentation preserves EXEC",
            test_logger_command_presentation_preserves_exec);
        run_case(
            "Logger command TTY/pipe/redirect failure matrix (40 cells)",
            test_logger_command_output_environment_matrix);
        run_case(
            "Logger invalid descriptor adoption rejection",
            test_logger_rejects_invalid_descriptor_flags_without_consuming_owner);
        run_case(
            "Logger checked write and close failures",
            test_logger_checked_initial_write_and_close_failures);
        run_case(
            "Logger noexcept warning factory and pending failure",
            test_logger_noexcept_warning_factory_and_pending_failure);
        run_case(
            "Logger replacement before write rejection",
            test_logger_rejects_name_replacement_before_write);
    } catch(const std::exception& error) {
        Logger::reset_for_test();
        std::cerr << error.what() << '\n';
        return 1;
    }

    Logger::reset_for_test();
    std::cout << "XDG state log tests: all checks passed\n";
    return 0;
}
