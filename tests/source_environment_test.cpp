#include "source_environment.hpp"
#include "source_preference.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_warnings;
std::vector<fs::path> g_loaded_paths;
std::vector<std::string> g_callback_events;
std::string g_race_contents;
fs::path g_race_source_path;
fs::path g_race_internal_path;
int g_concurrent_writer_ready_descriptor = -1;
int g_concurrent_writer_release_descriptor = -1;

class ScopedEnvironmentVariable final {
    std::string name_;
    std::optional<std::string> previous_value_;

public:
    ScopedEnvironmentVariable(std::string name, const std::string& value)
        : name_(std::move(name)) {
        if(const char* previous = std::getenv(name_.c_str());
           previous != nullptr) {
            previous_value_ = previous;
        }
        if(::setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error(
                "Failed to set source preference test environment");
        }
    }

    ~ScopedEnvironmentVariable() noexcept {
        if(previous_value_.has_value()) {
            static_cast<void>(::setenv(
                name_.c_str(), previous_value_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }
};

void record_warning(const std::string& warning) {
    g_warnings.push_back(warning);
}

void record_load_event(const fs::path& entry_path) {
    g_loaded_paths.push_back(entry_path);
    g_callback_events.push_back("load");
}

void record_warning_event(const std::string& warning) {
    g_warnings.push_back(warning);
    g_callback_events.push_back("warning");
}

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void write_race_contents(const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error(
            "Failed to open source preference race fixture " +
            path.string());
    }
    output.write(
        g_race_contents.data(),
        static_cast<std::streamsize>(g_race_contents.size()));
    output.close();
    if(!output) {
        throw std::runtime_error(
            "Failed to write source preference race fixture " +
            path.string());
    }
    fs::permissions(
        path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

void mutate_preference_in_place_for_race(const fs::path& entry_path) {
    write_race_contents(entry_path);
}

void replace_preference_for_race(const fs::path& entry_path) {
    const fs::path replacement =
        entry_path.parent_path() / ".race-replacement-fixture";
    std::error_code remove_error;
    fs::remove(replacement, remove_error);
    if(remove_error) {
        throw std::runtime_error(
            "Failed to reset source preference race replacement: " +
            remove_error.message());
    }
    write_race_contents(replacement);
    fs::rename(replacement, entry_path);
}

void mutate_source_descriptor_for_race(const fs::path&) {
    write_race_contents(g_race_source_path);
}

std::vector<fs::path> internal_artifacts_for_package(
    const fs::path& entry_path) {
    const std::string expected_prefix =
        "-.moguet-source-preference-" +
        entry_path.filename().string() + "-";
    std::vector<fs::path> artifacts;
    for(const fs::directory_entry& candidate :
        fs::directory_iterator(entry_path.parent_path())) {
        if(candidate.path().filename().string().starts_with(expected_prefix)) {
            artifacts.push_back(candidate.path());
        }
    }
    return artifacts;
}

void replace_internal_temporary_for_race(const fs::path& entry_path) {
    const std::vector<fs::path> artifacts =
        internal_artifacts_for_package(entry_path);
    for(const fs::path& artifact : artifacts) {
        g_race_internal_path = artifact;
        fs::remove(g_race_internal_path);
        write_race_contents(g_race_internal_path);
        return;
    }
    throw std::runtime_error(
        "Atomic source preference temporary entry was not found");
}

void mutate_internal_temporary_in_place_for_race(
    const fs::path& entry_path) {
    const std::vector<fs::path> artifacts =
        internal_artifacts_for_package(entry_path);
    if(artifacts.size() != 1) {
        throw std::runtime_error(
            "Atomic source preference temporary entry was not unique");
    }
    g_race_internal_path = artifacts.front();
    write_race_contents(g_race_internal_path);
}

void write_pipe_byte(int descriptor, char value) {
    ssize_t write_size = -1;
    do {
        write_size = ::write(descriptor, &value, 1);
    } while(write_size < 0 && errno == EINTR);
    if(write_size != 1) {
        throw std::runtime_error("Failed to write concurrency test pipe");
    }
}

char read_pipe_byte(int descriptor) {
    char value = '\0';
    ssize_t read_size = -1;
    do {
        read_size = ::read(descriptor, &value, 1);
    } while(read_size < 0 && errno == EINTR);
    if(read_size != 1) {
        throw std::runtime_error("Failed to read concurrency test pipe");
    }
    return value;
}

void hold_writer_lock_for_concurrency_test(const fs::path&) {
    write_pipe_byte(g_concurrent_writer_ready_descriptor, 'R');
    if(read_pipe_byte(g_concurrent_writer_release_descriptor) != 'G') {
        throw std::runtime_error(
            "Unexpected concurrency test release signal");
    }
}

class TestPipe final {
    int read_descriptor_ = -1;
    int write_descriptor_ = -1;

public:
    TestPipe() {
        int descriptors[2] = {-1, -1};
        if(::pipe(descriptors) != 0) {
            throw std::runtime_error("Failed to create concurrency test pipe");
        }
        read_descriptor_ = descriptors[0];
        write_descriptor_ = descriptors[1];
    }

    TestPipe(const TestPipe&) = delete;
    TestPipe& operator=(const TestPipe&) = delete;

    ~TestPipe() noexcept {
        if(read_descriptor_ >= 0) {
            static_cast<void>(::close(read_descriptor_));
        }
        if(write_descriptor_ >= 0) {
            static_cast<void>(::close(write_descriptor_));
        }
    }

    int read_descriptor() const noexcept {
        return read_descriptor_;
    }

    int write_descriptor() const noexcept {
        return write_descriptor_;
    }

    void close_read_descriptor() noexcept {
        if(read_descriptor_ >= 0) {
            static_cast<void>(::close(read_descriptor_));
            read_descriptor_ = -1;
        }
    }

    void close_write_descriptor() noexcept {
        if(write_descriptor_ >= 0) {
            static_cast<void>(::close(write_descriptor_));
            write_descriptor_ = -1;
        }
    }
};

enum class ConcurrentProbeKind {
    StrictRead,
    DirectorySnapshot,
    ListingSnapshot,
    RemoveEntry,
};

bool run_concurrent_probe(
    ConcurrentProbeKind kind,
    const std::string& package_name) {
    switch(kind) {
        case ConcurrentProbeKind::StrictRead: {
            const StrictSourcePreferenceResult result =
                read_source_preference_strict(package_name);
            const SourcePreferenceLoaded* loaded =
                std::get_if<SourcePreferenceLoaded>(&result);
            return loaded != nullptr &&
                   loaded->raw_contents.find("SERIALIZED=writer\n") !=
                       std::string::npos;
        }
        case ConcurrentProbeKind::DirectorySnapshot: {
            const SourcePreferenceDirectorySnapshot snapshot =
                snapshot_source_preference_directory();
            return std::any_of(
                snapshot.entries.begin(), snapshot.entries.end(),
                [&package_name](const SourcePreferenceEntrySnapshot& entry) {
                    return entry.package_name == package_name;
                });
        }
        case ConcurrentProbeKind::ListingSnapshot: {
            const SourcePreferenceListSnapshot snapshot =
                snapshot_source_preferences_for_listing();
            const auto entry = std::find_if(
                snapshot.entries.begin(), snapshot.entries.end(),
                [&package_name](
                    const SourcePreferenceListEntrySnapshot& candidate) {
                    return candidate.package_name == package_name;
                });
            return entry != snapshot.entries.end() &&
                   std::find(
                       entry->display_lines.begin(),
                       entry->display_lines.end(),
                       "SERIALIZED=writer") != entry->display_lines.end();
        }
        case ConcurrentProbeKind::RemoveEntry:
            return remove_source_preference_entry(package_name);
    }
    return false;
}

[[noreturn]] void run_concurrent_probe_child(
    ConcurrentProbeKind kind,
    const std::string& package_name,
    TestPipe& ready,
    TestPipe& done) noexcept {
    ready.close_read_descriptor();
    done.close_read_descriptor();
    bool succeeded = false;
    try {
        write_pipe_byte(ready.write_descriptor(), 'R');
        succeeded = run_concurrent_probe(kind, package_name);
        write_pipe_byte(done.write_descriptor(), succeeded ? 'S' : 'F');
    } catch(...) {
        succeeded = false;
        const char failure = 'F';
        static_cast<void>(::write(done.write_descriptor(), &failure, 1));
    }
    ::_exit(succeeded ? 0 : 1);
}

pid_t start_concurrent_probe(
    ConcurrentProbeKind kind,
    const std::string& package_name,
    TestPipe& ready,
    TestPipe& done) {
    const pid_t child = ::fork();
    if(child < 0) {
        throw std::runtime_error("Failed to fork concurrency test probe");
    }
    if(child == 0) {
        run_concurrent_probe_child(
            kind, package_name, ready, done);
    }
    ready.close_write_descriptor();
    done.close_write_descriptor();
    return child;
}

bool any_probe_completed(
    const std::vector<int>& done_descriptors,
    int timeout_milliseconds) {
    std::vector<struct pollfd> poll_descriptors;
    poll_descriptors.reserve(done_descriptors.size());
    for(int descriptor : done_descriptors) {
        poll_descriptors.push_back({descriptor, POLLIN, 0});
    }
    int poll_result = -1;
    do {
        poll_result = ::poll(
            poll_descriptors.data(), poll_descriptors.size(),
            timeout_milliseconds);
    } while(poll_result < 0 && errno == EINTR);
    if(poll_result < 0) {
        throw std::runtime_error("Failed to poll concurrency test probes");
    }
    return poll_result != 0;
}

void expect_source_preference_directory_lock_blocked(
    int lock_operation,
    const std::string& context) {
    const int descriptor = ::open(
        source_preference_root().c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if(descriptor < 0) {
        throw std::runtime_error(
            context + ": failed to open source preference directory");
    }

    errno = 0;
    const int lock_result = ::flock(
        descriptor, lock_operation | LOCK_NB);
    const int lock_error = errno;
    if(lock_result == 0) {
        static_cast<void>(::flock(descriptor, LOCK_UN));
    }
    const int close_result = ::close(descriptor);
    expect(
        lock_result == -1 &&
            (lock_error == EWOULDBLOCK || lock_error == EAGAIN),
        context + ": writer did not hold the expected directory lock");
    expect(
        close_result == 0,
        context + ": failed to close directory lock probe");
}

void expect_child_success(pid_t child, const std::string& context) {
    int status = 0;
    pid_t wait_result = -1;
    do {
        wait_result = ::waitpid(child, &status, 0);
    } while(wait_result < 0 && errno == EINTR);
    if(wait_result != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error(context + ": child process failed");
    }
}

void expect_equal(
    const std::string& test_case,
    const std::string& actual,
    const std::string& expected) {
    if(actual == expected) return;
    throw std::runtime_error(
        test_case + ": expected [" + expected + "], actual [" + actual + "]");
}

void expect_words(
    const std::string& test_case,
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected) {
    if(actual == expected) return;
    throw std::runtime_error(test_case + ": assignment words differ");
}

template <typename Alternative>
Alternative expect_strict_alternative(
    const StrictSourcePreferenceResult& result,
    const std::string& test_case) {
    const Alternative* alternative = std::get_if<Alternative>(&result);
    expect(alternative != nullptr, test_case + ": unexpected strict result alternative");
    return *alternative;
}

SourcePreferenceFailure expect_strict_failure(
    const StrictSourcePreferenceResult& result,
    SourcePreferenceFailureKind expected_kind,
    const std::string& test_case) {
    SourcePreferenceFailure failure =
        expect_strict_alternative<SourcePreferenceFailure>(result, test_case);
    expect(failure.kind == expected_kind, test_case + ": unexpected failure kind");
    expect(!failure.diagnostic.empty(), test_case + ": missing failure diagnostic");
    return failure;
}

void expect_assignment(
    const SourceBuildEnvironment& environment,
    size_t index,
    const std::string& expected_key,
    const std::string& expected_value) {
    expect(
        index < environment.ordered_assignments.size(),
        "Missing source environment assignment at index " + std::to_string(index));
    const SourceEnvironmentAssignment& assignment =
        environment.ordered_assignments[index];
    expect_equal("assignment key", assignment.key, expected_key);
    expect_equal("assignment value for " + expected_key, assignment.value, expected_value);
}

void remove_preference_entry(const std::string& package_name) {
    const fs::path entry_path = source_preference_entry_path(package_name);
    std::error_code remove_error;
    fs::remove_all(entry_path, remove_error);
    if(remove_error) {
        throw std::runtime_error(
            "Failed to remove source preference fixture for " + package_name +
            ": " + remove_error.message());
    }
}

void require_mode(
    const fs::path& path, mode_t expected_mode,
    const std::string& context) {
    struct stat status{};
    if(::lstat(path.c_str(), &status) != 0) {
        throw std::runtime_error(
            context + ": failed to inspect " + path.string() + ": " +
            std::strerror(errno));
    }
    expect(
        (status.st_mode & 07777) == expected_mode,
        context + ": unexpected mode for " + path.string());
}

void prepare_preference_root() {
    const fs::path root = source_preference_root();
    fs::create_directories(root);
    fs::permissions(
        root.parent_path(), fs::perms::owner_all,
        fs::perm_options::replace);
    fs::permissions(
        root, fs::perms::owner_all,
        fs::perm_options::replace);
}

void write_preference(
    const std::string& package_name,
    const std::string& contents) {
    prepare_preference_root();
    remove_preference_entry(package_name);
    const fs::path entry_path = source_preference_entry_path(package_name);
    std::ofstream file(entry_path, std::ios::binary);
    if(!file) {
        throw std::runtime_error(
            "Failed to create source preference fixture for " + package_name);
    }
    file << contents;
    if(!file) {
        throw std::runtime_error(
            "Failed to write source preference fixture for " + package_name);
    }
    file.close();
    fs::permissions(
        entry_path,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

std::string read_preference_contents(const std::string& package_name) {
    std::ifstream file(
        source_preference_entry_path(package_name),
        std::ios::binary);
    if(!file) {
        throw std::runtime_error(
            "Failed to read source preference fixture for " + package_name);
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

SourceBuildEnvironment load_preference(const std::string& package_name) {
    g_warnings.clear();
    return get_package_env(package_name, nullptr, record_warning);
}

void test_absent_environment() {
    fs::remove(source_preference_entry_path("absent-target"));
    SourceBuildEnvironment environment = load_preference("absent-target");

    expect(environment.ordered_assignments.empty(), "Absent environment is not empty");
    expect(!environment.defines("PKGDEST"), "Absent environment defines PKGDEST");
    expect(
        !environment.has_forwarded_nonempty_assignment(),
        "Absent environment has a forwardable assignment");
    expect_equal(
        "absent preference serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        "");
    expect_equal(
        "absent CLI serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        "");
    expect_words(
        "absent preference assignment words",
        materialize_source_build_environment_assignment_words(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        {});
    expect_words(
        "absent CLI assignment words",
        materialize_source_build_environment_assignment_words(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        {});
    expect(
        !fs::exists(source_preference_root()),
        "Absent source preference read created the canonical store");
}

void test_native_mutation_creation_boundary_and_identity() {
    const fs::path root = source_preference_root();
    std::error_code remove_error;
    fs::remove_all(root.parent_path(), remove_error);
    if(remove_error) {
        throw std::runtime_error(
            "Failed to reset source preference creation fixture: " +
            remove_error.message());
    }

    const std::string package_name = "native-mutation-target";
    create_source_preference_entry(package_name);
    const fs::path entry_path = source_preference_entry_path(package_name);
    expect(fs::is_directory(root), "add mutation did not create the canonical store");
    expect(fs::is_regular_file(entry_path), "add mutation did not create an entry");
    require_mode(root.parent_path(), 0700, "source preference app directory");
    require_mode(root, 0700, "source preference store");
    require_mode(entry_path, 0600, "source preference entry");

    append_source_preference_assignment(package_name, "FIRST=alpha");
    append_source_preference_assignment(package_name, "SECOND=beta");
    expect_equal(
        "native append preserves byte identity",
        read_preference_contents(package_name),
        "FIRST=alpha\nSECOND=beta\n");

    const SourcePreferenceLoaded before_replace =
        expect_strict_alternative<SourcePreferenceLoaded>(
            read_source_preference_strict(package_name),
            "native mutation identity read");
    expect(before_replace.identity.has_value(), "Strict read omitted entry identity");

    const fs::path replacement = root.parent_path() / "replacement.fixture";
    {
        std::ofstream output(replacement, std::ios::binary);
        output << "REPLACED=yes\n";
    }
    fs::permissions(
        replacement,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
    const int descriptor = ::open(replacement.c_str(), O_RDONLY | O_CLOEXEC);
    if(descriptor < 0) {
        throw std::runtime_error(
            "Failed to open source preference replacement fixture: " +
            std::string(std::strerror(errno)));
    }
    try {
        replace_source_preference_entry_from_descriptor(
            package_name, descriptor, before_replace.identity);
    } catch(...) {
        static_cast<void>(::close(descriptor));
        throw;
    }
    if(::close(descriptor) != 0) {
        throw std::runtime_error("Failed to close replacement fixture");
    }
    expect_equal(
        "native atomic replacement contents",
        read_preference_contents(package_name), "REPLACED=yes\n");
    require_mode(entry_path, 0600, "atomically replaced source preference entry");

    const SourcePreferenceLoaded before_concurrent_change =
        expect_strict_alternative<SourcePreferenceLoaded>(
            read_source_preference_strict(package_name),
            "native mutation concurrent identity read");
    expect(
        before_concurrent_change.identity.has_value(),
        "Concurrent mutation fixture omitted entry identity");
    {
        std::ofstream concurrent(entry_path, std::ios::binary | std::ios::trunc);
        concurrent << "CONCURRENT=in-place-change\n";
    }
    const int stale_descriptor =
        ::open(replacement.c_str(), O_RDONLY | O_CLOEXEC);
    if(stale_descriptor < 0) {
        throw std::runtime_error(
            "Failed to reopen source preference replacement fixture: " +
            std::string(std::strerror(errno)));
    }
    try {
        replace_source_preference_entry_from_descriptor(
            package_name, stale_descriptor,
            before_concurrent_change.identity);
        static_cast<void>(::close(stale_descriptor));
        throw std::runtime_error(
            "Atomic replacement accepted stale same-inode entry state");
    } catch(const SourcePreferenceError& error) {
        if(::close(stale_descriptor) != 0) {
            throw std::runtime_error("Failed to close stale replacement fixture");
        }
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Stale same-inode replacement lost its typed error");
    }
    expect_equal(
        "stale replacement preserved concurrent contents",
        read_preference_contents(package_name),
        "CONCURRENT=in-place-change\n");
    expect(remove_source_preference_entry(package_name), "Native delete reported absent");
    expect(!remove_source_preference_entry(package_name), "Missing native delete reported present");
    expect(
        fs::is_directory(root),
        "Deleting the final entry removed the canonical store");
    fs::remove(replacement);
}

void test_preference_parsing_and_serialization() {
    write_preference(
        "structured-target",
        "FIRST = \"alpha value\" # stripped comment\n"
        "QUOTED = 'quoted # value' # stripped after quoted hash\n"
        "EMPTY=\n"
        "BRACED=${FIRST}/brace\n"
        "UNDEFINED=$MISSING\n"
        "DUP=first\n"
        "DUP=second\n"
        "SIMPLE=$DUP/simple\n"
        "9INVALID=value\n"
        "ignored without equals\n");

    SourceBuildEnvironment environment = load_preference("structured-target");
    expect(
        environment.ordered_assignments.size() == 8,
        "Unexpected structured assignment count");
    expect_assignment(environment, 0, "FIRST", "alpha value");
    expect_assignment(environment, 1, "QUOTED", "quoted # value");
    expect_assignment(environment, 2, "EMPTY", "");
    expect_assignment(environment, 3, "BRACED", "alpha value/brace");
    expect_assignment(environment, 4, "UNDEFINED", "");
    expect_assignment(environment, 5, "DUP", "first");
    expect_assignment(environment, 6, "DUP", "second");
    expect_assignment(environment, 7, "SIMPLE", "second/simple");
    expect(environment.defines("EMPTY"), "Empty assignment definition was lost");
    expect(environment.defines("UNDEFINED"), "Expanded-empty definition was lost");
    expect(
        environment.has_forwarded_nonempty_assignment(),
        "Nonempty preference assignment was not detected");
    expect(
        g_warnings.size() == 1,
        "Unexpected source preference warning count");
    expect_equal(
        "invalid preference warning",
        g_warnings.front(),
        "Ignoring invalid environment assignment: 9INVALID=value");

    expect_equal(
        "preference compatibility serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        "FIRST='alpha value' QUOTED='quoted # value' BRACED='alpha value/brace' "
        "DUP='first' DUP='second' SIMPLE='second/simple' ");
    expect_equal(
        "CLI compatibility serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        "FIRST='alpha value' QUOTED='quoted # value' EMPTY='' "
        "BRACED='alpha value/brace' UNDEFINED='' DUP='first' DUP='second' "
        "SIMPLE='second/simple' ");
    expect_words(
        "preference compatibility assignment words",
        materialize_source_build_environment_assignment_words(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        {"FIRST=alpha value", "QUOTED=quoted # value",
         "BRACED=alpha value/brace", "DUP=first", "DUP=second",
         "SIMPLE=second/simple"});
    expect_words(
        "CLI compatibility assignment words",
        materialize_source_build_environment_assignment_words(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        {"FIRST=alpha value", "QUOTED=quoted # value", "EMPTY=",
         "BRACED=alpha value/brace", "UNDEFINED=", "DUP=first",
         "DUP=second", "SIMPLE=second/simple"});
}

void test_raw_assignment_word_materialization() {
    const SourceBuildEnvironment environment{{
        {"PLAIN", "alpha"},
        {"DUP", "first"},
        {"EMPTY", ""},
        {"DUP", "second"},
        {"EQUALS", "left=middle=right"},
        {"SPACES", "two words"},
        {"SINGLE", "single'quote"},
        {"DOUBLE", "double\"quote"},
        {"DOLLAR", "literal $value"},
        {"BACKSLASH", "slash\\value"},
        {"NEWLINE", "line1\nline2"},
        {"UNICODE", "日本語"},
    }};

    expect_words(
        "Forward raw assignment words",
        materialize_source_build_environment_assignment_words(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        {"PLAIN=alpha", "DUP=first", "EMPTY=", "DUP=second",
         "EQUALS=left=middle=right", "SPACES=two words",
         "SINGLE=single'quote", "DOUBLE=double\"quote",
         "DOLLAR=literal $value", "BACKSLASH=slash\\value",
         "NEWLINE=line1\nline2", "UNICODE=日本語"});
    expect_equal(
        "Forward raw assignment prefix",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        "PLAIN='alpha' DUP='first' EMPTY='' DUP='second' "
        "EQUALS='left=middle=right' SPACES='two words' "
        "SINGLE='single'\\''quote' DOUBLE='double\"quote' "
        "DOLLAR='literal $value' BACKSLASH='slash\\value' "
        "NEWLINE='line1\nline2' UNICODE='日本語' ");

    expect_words(
        "Omit raw assignment words",
        materialize_source_build_environment_assignment_words(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        {"PLAIN=alpha", "DUP=first", "DUP=second",
         "EQUALS=left=middle=right", "SPACES=two words",
         "SINGLE=single'quote", "DOUBLE=double\"quote",
         "DOLLAR=literal $value", "BACKSLASH=slash\\value",
         "NEWLINE=line1\nline2", "UNICODE=日本語"});
    expect_equal(
        "Omit raw assignment prefix",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        "PLAIN='alpha' DUP='first' DUP='second' "
        "EQUALS='left=middle=right' SPACES='two words' "
        "SINGLE='single'\\''quote' DOUBLE='double\"quote' "
        "DOLLAR='literal $value' BACKSLASH='slash\\value' "
        "NEWLINE='line1\nline2' UNICODE='日本語' ");
}

void test_empty_duplicate_pkgdest_definitions() {
    write_preference(
        "requested-target",
        "PKGDEST=\n"
        "PKGDEST=\"\"\n"
        "PKGDEST=$UNDEFINED_PKGDEST\n");

    SourceBuildEnvironment environment = load_preference("requested-target");
    expect(
        environment.ordered_assignments.size() == 3,
        "Unexpected PKGDEST assignment count");
    expect_assignment(environment, 0, "PKGDEST", "");
    expect_assignment(environment, 1, "PKGDEST", "");
    expect_assignment(environment, 2, "PKGDEST", "");
    expect(environment.defines("PKGDEST"), "Empty PKGDEST definition was lost");
    expect(
        !environment.has_forwarded_nonempty_assignment(),
        "Empty-only PKGDEST changed fallback eligibility");
    expect_equal(
        "preference empty PKGDEST serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        "");
    expect_equal(
        "CLI empty PKGDEST serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        "PKGDEST='' PKGDEST='' PKGDEST='' ");
}

void test_mixed_duplicate_pkgdest_uses_last_value_for_expansion() {
    write_preference(
        "mixed-pkgdest-target",
        "PKGDEST=first-path\n"
        "PKGDEST=\n"
        "AFTER=$PKGDEST\n");

    SourceBuildEnvironment environment = load_preference("mixed-pkgdest-target");
    expect(
        environment.ordered_assignments.size() == 3,
        "Unexpected mixed PKGDEST assignment count");
    expect_assignment(environment, 0, "PKGDEST", "first-path");
    expect_assignment(environment, 1, "PKGDEST", "");
    expect_assignment(environment, 2, "AFTER", "");
    expect(environment.defines("PKGDEST"), "Mixed PKGDEST definition was lost");
    expect(
        environment.has_forwarded_nonempty_assignment(),
        "Mixed PKGDEST lost its forwardable nonempty assignment");
    expect_equal(
        "preference mixed PKGDEST serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Omit),
        "PKGDEST='first-path' ");
    expect_equal(
        "CLI mixed PKGDEST serialization",
        serialize_source_build_environment(
            environment, SourceEnvironmentEmptyValuePolicy::Forward),
        "PKGDEST='first-path' PKGDEST='' AFTER='' ");
}

void test_invalid_only_preference() {
    write_preference(
        "invalid-only-target",
        "9INVALID=value\n"
        "ignored without equals\n");

    SourceBuildEnvironment environment = load_preference("invalid-only-target");
    expect(
        environment.ordered_assignments.empty(),
        "Invalid preference assignment entered structured environment");
    expect(
        !environment.has_forwarded_nonempty_assignment(),
        "Invalid-only preference changed fallback eligibility");
    expect(
        g_warnings.size() == 1,
        "Unexpected invalid-only warning count");
    expect_equal(
        "invalid-only warning",
        g_warnings.front(),
        "Ignoring invalid environment assignment: 9INVALID=value");
}

void test_callback_timing_and_absent_behavior() {
    const std::string absent_package = "callback-absent";
    remove_preference_entry(absent_package);
    g_loaded_paths.clear();
    g_warnings.clear();
    g_callback_events.clear();

    SourceBuildEnvironment absent_environment = get_package_env(
        absent_package, record_load_event, record_warning_event);
    expect(
        absent_environment.ordered_assignments.empty(),
        "Absent callback environment is not empty");
    expect(g_loaded_paths.empty(), "Absent preference called on_load");
    expect(g_warnings.empty(), "Absent preference emitted a warning");
    expect(g_callback_events.empty(), "Absent preference emitted a callback event");

    const std::string package_name = "callback-target";
    write_preference(
        package_name,
        "GOOD=before\n"
        "9INVALID=value\n"
        "AFTER=still-parsed\n");
    g_loaded_paths.clear();
    g_warnings.clear();
    g_callback_events.clear();

    SourceBuildEnvironment environment = get_package_env(
        package_name, record_load_event, record_warning_event);
    expect(
        environment.ordered_assignments.size() == 2,
        "Invalid assignment stopped callback parsing");
    expect_assignment(environment, 0, "GOOD", "before");
    expect_assignment(environment, 1, "AFTER", "still-parsed");
    expect(g_loaded_paths.size() == 1, "Preference on_load count changed");
    expect(
        g_loaded_paths.front() == source_preference_entry_path(package_name),
        "Preference on_load path changed");
    expect(g_warnings.size() == 1, "Warning callback count changed");
    expect(
        g_callback_events == std::vector<std::string>{"load", "warning"},
        "on_load no longer precedes parser warnings");
}

void test_strict_absent_and_empty_file_are_distinct() {
    const std::string absent_package = "strict-absent-target";
    remove_preference_entry(absent_package);
    StrictSourcePreferenceResult absent =
        read_source_preference_strict(absent_package);
    static_cast<void>(expect_strict_alternative<SourcePreferenceAbsent>(
        absent, "strict absent preference"));

    const std::string empty_package = "strict-empty-target";
    write_preference(empty_package, "");
    StrictSourcePreferenceResult empty =
        read_source_preference_strict(empty_package);
    const SourcePreferenceLoaded loaded =
        expect_strict_alternative<SourcePreferenceLoaded>(
            empty, "strict empty preference");
    expect(
        loaded.entry_path == source_preference_entry_path(empty_package),
        "Strict empty preference returned an unexpected path");
    expect(
        loaded.environment.ordered_assignments.empty(),
        "Strict empty preference returned assignments");
    expect(loaded.warnings.empty(), "Strict empty preference returned warnings");
    expect(loaded.identity.has_value(), "Strict empty preference omitted identity");
    expect_equal("strict empty raw contents", loaded.raw_contents, "");
    expect_equal(
        "strict empty preference contents",
        read_preference_contents(empty_package), "");
}

void test_strict_parser_compatibility_and_file_preservation() {
    const std::string package_name = "strict-structured-target";
    const std::string contents =
        "9FIRST_INVALID=value\n"
        "FIRST = \"alpha value\" # stripped comment\n"
        "QUOTED = 'quoted # value' # stripped after quoted hash\n"
        "EMPTY=\n"
        "BRACED=${FIRST}/brace\n"
        "UNDEFINED=$MISSING\n"
        "DUP=first\n"
        "DUP=second\n"
        "SIMPLE=$DUP/simple\n"
        "=second-invalid\n"
        "ignored without equals\n";
    write_preference(package_name, contents);

    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    const SourcePreferenceLoaded loaded =
        expect_strict_alternative<SourcePreferenceLoaded>(
            result, "strict structured preference");
    const SourceBuildEnvironment& environment = loaded.environment;
    expect(
        environment.ordered_assignments.size() == 8,
        "Unexpected strict structured assignment count");
    expect_assignment(environment, 0, "FIRST", "alpha value");
    expect_assignment(environment, 1, "QUOTED", "quoted # value");
    expect_assignment(environment, 2, "EMPTY", "");
    expect_assignment(environment, 3, "BRACED", "alpha value/brace");
    expect_assignment(environment, 4, "UNDEFINED", "");
    expect_assignment(environment, 5, "DUP", "first");
    expect_assignment(environment, 6, "DUP", "second");
    expect_assignment(environment, 7, "SIMPLE", "second/simple");
    expect(
        environment.defines("EMPTY"),
        "Strict parser lost an explicit empty assignment");
    expect(
        loaded.warnings == std::vector<std::string>{
                               "Ignoring invalid environment assignment: 9FIRST_INVALID=value",
                               "Ignoring invalid environment assignment: =second-invalid"},
        "Strict parser warning order changed");
    expect_equal("strict raw contents", loaded.raw_contents, contents);
    expect(loaded.identity.has_value(), "Strict parser result omitted identity");
    expect_equal(
        "strict source preference remained unchanged",
        read_preference_contents(package_name), contents);
}

void test_strict_malformed_only_file_is_loaded_with_warnings() {
    const std::string package_name = "strict-malformed-only-target";
    write_preference(
        package_name,
        "9INVALID=value\n"
        "ignored without equals\n");

    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    const SourcePreferenceLoaded loaded =
        expect_strict_alternative<SourcePreferenceLoaded>(
            result, "strict malformed-only preference");
    expect(
        loaded.environment.ordered_assignments.empty(),
        "Strict malformed-only preference returned an assignment");
    expect(
        loaded.warnings == std::vector<std::string>{
                               "Ignoring invalid environment assignment: 9INVALID=value"},
        "Strict malformed-only warning changed");
}

void test_strict_status_failure_is_not_absent() {
    const std::string package_name = "strict-status-failure-target";
    write_preference(package_name, "VALUE=available\n");
    reset_source_preference_test_hooks();
    fail_next_source_preference_operation_for_test(
        package_name, SourcePreferenceTestFailurePoint::Status);

    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::StatusUnavailable,
        "strict status failure");
    expect(
        failure.entry_path == source_preference_entry_path(package_name),
        "Strict status failure returned an unexpected path");
    expect(
        failure.system_error == std::make_error_code(std::errc::permission_denied),
        "Strict status failure lost its system error");
    expect(
        !failure.observed_file_type.has_value(),
        "Strict status failure invented an observed file type");

    StrictSourcePreferenceResult retry =
        read_source_preference_strict(package_name);
    static_cast<void>(expect_strict_alternative<SourcePreferenceLoaded>(
        retry, "strict status failure one-shot retry"));
}

void test_strict_symlink_is_rejected_by_all_readers() {
    const std::string target_package = "strict-symlink-source";
    const std::string link_package = "strict-symlink-target";
    const std::string target_contents = "FROM_TARGET=followed\n";
    write_preference(target_package, target_contents);
    remove_preference_entry(link_package);
    fs::create_symlink(
        fs::path(target_package),
        source_preference_entry_path(link_package));

    StrictSourcePreferenceResult result =
        read_source_preference_strict(link_package);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::UnsupportedFileType,
        "strict symlink preference");
    expect(
        failure.observed_file_type == fs::file_type::symlink,
        "Strict symlink preference lost its observed file type");

    try {
        static_cast<void>(load_preference(link_package));
        throw std::runtime_error("Convenience reader followed a source preference symlink");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::UnsupportedFileType,
            "Convenience symlink failure lost its typed error");
    }
    expect_equal(
        "strict symlink target remained unread and unchanged",
        read_preference_contents(target_package), target_contents);
}

void test_strict_dangling_symlink_is_not_absent() {
    const std::string missing_package = "strict-dangling-missing";
    const std::string link_package = "strict-dangling-target";
    remove_preference_entry(missing_package);
    remove_preference_entry(link_package);
    fs::create_symlink(
        fs::path(missing_package),
        source_preference_entry_path(link_package));

    StrictSourcePreferenceResult result =
        read_source_preference_strict(link_package);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::UnsupportedFileType,
        "strict dangling symlink preference");
    expect(
        failure.observed_file_type == fs::file_type::symlink,
        "Strict dangling symlink lost its observed file type");
}

void test_strict_directory_entry_is_rejected() {
    const std::string package_name = "strict-directory-target";
    const fs::path entry_path = source_preference_entry_path(package_name);
    remove_preference_entry(package_name);
    fs::create_directory(entry_path);

    StrictSourcePreferenceResult result = read_source_preference_strict(package_name);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::UnsupportedFileType,
        "strict directory preference");
    expect(
        failure.observed_file_type == fs::file_type::directory,
        "Strict directory preference lost its observed file type");
}

void test_unsafe_mode_is_a_typed_hard_error() {
    const std::string package_name = "unsafe-mode-target";
    write_preference(package_name, "VALUE=unsafe\n");
    fs::permissions(
        source_preference_entry_path(package_name),
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::group_read | fs::perms::others_read,
        fs::perm_options::replace);

    const SourcePreferenceFailure failure = expect_strict_failure(
        read_source_preference_strict(package_name),
        SourcePreferenceFailureKind::UnsafePermissions,
        "unsafe source preference mode");
    expect(
        !failure.system_error.has_value(),
        "Unsafe mode failure invented a system error");
    fs::permissions(
        source_preference_entry_path(package_name),
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

void test_directory_and_listing_snapshots_are_all_or_nothing() {
    std::error_code remove_error;
    fs::remove_all(source_preference_root(), remove_error);
    if(remove_error) {
        throw std::runtime_error(
            "Failed to reset listing snapshot fixture: " +
            remove_error.message());
    }
    write_preference("snapshot-alpha", "ONE=1\n# ignored\nTWO=2\n");
    write_preference("snapshot-zeta", "ZED=yes\n");

    const SourcePreferenceDirectorySnapshot directory_snapshot =
        snapshot_source_preference_directory();
    expect(directory_snapshot.root_exists, "Directory snapshot reported the store absent");
    expect(!directory_snapshot.entries.empty(), "Directory snapshot omitted valid entries");
    for(const SourcePreferenceEntrySnapshot& entry : directory_snapshot.entries) {
        expect(entry.is_regular_file, "Validated directory snapshot lost regular identity");
        expect(
            entry.entry_path == source_preference_entry_path(entry.package_name),
            "Directory snapshot returned a noncanonical path");
    }

    const SourcePreferenceListSnapshot listing =
        snapshot_source_preferences_for_listing();
    const auto alpha = std::find_if(
        listing.entries.begin(), listing.entries.end(),
        [](const SourcePreferenceListEntrySnapshot& entry) {
            return entry.package_name == "snapshot-alpha";
        });
    expect(alpha != listing.entries.end(), "Listing snapshot omitted snapshot-alpha");
    expect(
        alpha->display_lines == std::vector<std::string>{"ONE=1", "TWO=2"},
        "Listing snapshot changed display-line filtering");

    const fs::path invalid_entry = source_preference_root() /
                                   "-.moguet-source-preference-interrupted-atomic-write";
    {
        std::ofstream output(invalid_entry, std::ios::binary);
        output << "VALUE=invalid-name\n";
    }
    fs::permissions(
        invalid_entry,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
    try {
        static_cast<void>(snapshot_source_preferences_for_listing());
        throw std::runtime_error("Listing snapshot accepted an invalid entry name");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind == SourcePreferenceFailureKind::InvalidEntryName,
            "Invalid listing entry name lost its typed error");
    }
    fs::remove(invalid_entry);

    fs::permissions(
        source_preference_entry_path("snapshot-zeta"),
        fs::perms::owner_read | fs::perms::owner_write |
            fs::perms::group_read,
        fs::perm_options::replace);
    try {
        static_cast<void>(snapshot_source_preferences_for_listing());
        throw std::runtime_error("Listing snapshot accepted an unsafe later entry");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind == SourcePreferenceFailureKind::UnsafePermissions,
            "Unsafe listing entry lost its typed error");
    }
    fs::permissions(
        source_preference_entry_path("snapshot-zeta"),
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

void test_cooperative_reader_writer_directory_locks() {
    const std::string read_package = "cooperative-lock-read-target";
    const std::string remove_package = "cooperative-lock-remove-target";
    write_preference(read_package, "ORIGINAL=reader\n");
    write_preference(remove_package, "ORIGINAL=remove\n");

    TestPipe writer_ready;
    TestPipe writer_release;
    g_concurrent_writer_ready_descriptor = writer_ready.write_descriptor();
    g_concurrent_writer_release_descriptor = writer_release.read_descriptor();
    run_source_preference_race_once_for_test(
        read_package,
        SourcePreferenceTestRacePoint::AtPublicationBoundary,
        hold_writer_lock_for_concurrency_test);

    const pid_t first_writer = ::fork();
    if(first_writer < 0) {
        throw std::runtime_error("Failed to fork first concurrency writer");
    }
    if(first_writer == 0) {
        writer_ready.close_read_descriptor();
        writer_release.close_write_descriptor();
        try {
            append_source_preference_assignment(
                read_package, "SERIALIZED=writer");
            ::_exit(0);
        } catch(...) {
            ::_exit(1);
        }
    }
    writer_ready.close_write_descriptor();
    writer_release.close_read_descriptor();
    reset_source_preference_test_hooks();
    expect(
        read_pipe_byte(writer_ready.read_descriptor()) == 'R',
        "First concurrency writer did not reach the publication boundary");
    expect(
        internal_artifacts_for_package(
            source_preference_entry_path(read_package))
                .size() == 1,
        "Blocked writer did not expose the expected live internal artifact");
    expect_source_preference_directory_lock_blocked(
        LOCK_SH,
        "Cooperative reader lock probe");
    expect_source_preference_directory_lock_blocked(
        LOCK_EX,
        "Cooperative writer lock probe");

    TestPipe strict_ready;
    TestPipe strict_done;
    const pid_t strict_reader = start_concurrent_probe(
        ConcurrentProbeKind::StrictRead, read_package,
        strict_ready, strict_done);
    expect(
        read_pipe_byte(strict_ready.read_descriptor()) == 'R',
        "Strict reader did not start");

    TestPipe directory_ready;
    TestPipe directory_done;
    const pid_t directory_reader = start_concurrent_probe(
        ConcurrentProbeKind::DirectorySnapshot, read_package,
        directory_ready, directory_done);
    expect(
        read_pipe_byte(directory_ready.read_descriptor()) == 'R',
        "Directory snapshot reader did not start");

    TestPipe listing_ready;
    TestPipe listing_done;
    const pid_t listing_reader = start_concurrent_probe(
        ConcurrentProbeKind::ListingSnapshot, read_package,
        listing_ready, listing_done);
    expect(
        read_pipe_byte(listing_ready.read_descriptor()) == 'R',
        "Listing snapshot reader did not start");

    TestPipe second_writer_ready;
    TestPipe second_writer_done;
    const pid_t second_writer = start_concurrent_probe(
        ConcurrentProbeKind::RemoveEntry, remove_package,
        second_writer_ready, second_writer_done);
    expect(
        read_pipe_byte(second_writer_ready.read_descriptor()) == 'R',
        "Second concurrency writer did not start");

    const bool completed_while_exclusive_lock_was_held = any_probe_completed(
        {
            strict_done.read_descriptor(),
            directory_done.read_descriptor(),
            listing_done.read_descriptor(),
            second_writer_done.read_descriptor(),
        },
        250);

    write_pipe_byte(writer_release.write_descriptor(), 'G');
    writer_release.close_write_descriptor();

    expect(
        read_pipe_byte(strict_done.read_descriptor()) == 'S',
        "Strict reader failed after writer serialization");
    expect(
        read_pipe_byte(directory_done.read_descriptor()) == 'S',
        "Directory snapshot failed after writer serialization");
    expect(
        read_pipe_byte(listing_done.read_descriptor()) == 'S',
        "Listing snapshot observed a live internal artifact");
    expect(
        read_pipe_byte(second_writer_done.read_descriptor()) == 'S',
        "Second writer failed after exclusive-lock serialization");

    expect_child_success(first_writer, "first source preference writer");
    expect_child_success(strict_reader, "strict source preference reader");
    expect_child_success(
        directory_reader, "source preference directory snapshot");
    expect_child_success(listing_reader, "source preference listing snapshot");
    expect_child_success(second_writer, "second source preference writer");

    expect(
        !completed_while_exclusive_lock_was_held,
        "A cooperative reader or writer bypassed the directory LOCK_EX");
    expect(
        !fs::exists(source_preference_entry_path(remove_package)),
        "Serialized removal did not remove its source preference entry");
    expect(
        internal_artifacts_for_package(
            source_preference_entry_path(read_package))
            .empty(),
        "Successful cooperative serialization left an internal artifact");
    g_concurrent_writer_ready_descriptor = -1;
    g_concurrent_writer_release_descriptor = -1;
}

void test_deterministic_descriptor_races_fail_closed() {
    const std::string read_package = "race-read-target";
    write_preference(read_package, "ORIGINAL=read\n");
    g_race_contents = "CONCURRENT=read-in-place\n";
    run_source_preference_race_once_for_test(
        read_package, SourcePreferenceTestRacePoint::AfterReadOpen,
        mutate_preference_in_place_for_race);
    expect_strict_failure(
        read_source_preference_strict(read_package),
        SourcePreferenceFailureKind::ConcurrentReplacement,
        "same-inode read mutation");
    expect_equal(
        "same-inode read race contents",
        read_preference_contents(read_package), g_race_contents);

    const std::string create_package = "race-create-target";
    remove_preference_entry(create_package);
    g_race_contents = "COMPETING=create\n";
    run_source_preference_race_once_for_test(
        create_package,
        SourcePreferenceTestRacePoint::AtPublicationBoundary,
        replace_preference_for_race);
    try {
        create_source_preference_entry(create_package);
        throw std::runtime_error(
            "Absent atomic create overwrote a competing entry");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Absent create race lost its typed error");
    }
    expect_equal(
        "absent create race preserved competitor",
        read_preference_contents(create_package), g_race_contents);

    const std::string post_create_package = "race-post-create-target";
    remove_preference_entry(post_create_package);
    g_race_contents = "COMPETING=post-create\n";
    run_source_preference_race_once_for_test(
        post_create_package,
        SourcePreferenceTestRacePoint::AfterPublication,
        replace_preference_for_race);
    try {
        create_source_preference_entry(post_create_package);
        throw std::runtime_error(
            "Post-publication create race returned success");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Post-publication create race lost its typed error");
    }
    expect_equal(
        "post-publication create race preserved canonical competitor",
        read_preference_contents(post_create_package), g_race_contents);

    const std::string replace_package = "race-replace-target";
    write_preference(replace_package, "ORIGINAL=replace\n");
    const SourcePreferenceLoaded replace_identity =
        expect_strict_alternative<SourcePreferenceLoaded>(
            read_source_preference_strict(replace_package),
            "replacement race identity");
    expect(
        replace_identity.identity.has_value(),
        "Replacement race fixture omitted entry identity");
    const fs::path replacement_source =
        source_preference_root().parent_path() /
        "race-replacement-source.fixture";
    {
        std::ofstream output(replacement_source, std::ios::binary);
        output << "DESIRED=replacement\n";
    }
    fs::permissions(
        replacement_source,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
    const int replacement_descriptor =
        ::open(replacement_source.c_str(), O_RDONLY | O_CLOEXEC);
    if(replacement_descriptor < 0) {
        throw std::runtime_error(
            "Failed to open replacement race source: " +
            std::string(std::strerror(errno)));
    }
    g_race_contents = "COMPETING=replace\n";
    run_source_preference_race_once_for_test(
        replace_package,
        SourcePreferenceTestRacePoint::AtPublicationBoundary,
        replace_preference_for_race);
    try {
        replace_source_preference_entry_from_descriptor(
            replace_package, replacement_descriptor,
            replace_identity.identity);
        static_cast<void>(::close(replacement_descriptor));
        throw std::runtime_error(
            "Atomic replacement overwrote a competing entry");
    } catch(const SourcePreferenceError& error) {
        if(::close(replacement_descriptor) != 0) {
            throw std::runtime_error(
                "Failed to close replacement race source");
        }
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Existing replacement race lost its typed error");
    }
    expect_equal(
        "replacement mismatch kept the published entry in place",
        read_preference_contents(replace_package),
        "DESIRED=replacement\n");
    const std::vector<fs::path> replacement_artifacts =
        internal_artifacts_for_package(
            source_preference_entry_path(replace_package));
    expect(
        replacement_artifacts.size() == 1,
        "Replacement mismatch did not preserve exactly one displaced artifact");
    {
        std::ifstream artifact(replacement_artifacts.front(), std::ios::binary);
        const std::string artifact_contents(
            (std::istreambuf_iterator<char>(artifact)),
            std::istreambuf_iterator<char>());
        expect_equal(
            "replacement mismatch preserved the untrusted artifact",
            artifact_contents, g_race_contents);
    }
    fs::remove(replacement_artifacts.front());
    fs::remove(replacement_source);

    const std::string source_race_package = "race-edit-source-target";
    write_preference(source_race_package, "ORIGINAL=source-race\n");
    const SourcePreferenceLoaded source_race_identity =
        expect_strict_alternative<SourcePreferenceLoaded>(
            read_source_preference_strict(source_race_package),
            "source descriptor race identity");
    g_race_source_path = source_preference_root().parent_path() /
                         "race-edit-source.fixture";
    g_race_contents = "DESIRED=before-race\n";
    write_race_contents(g_race_source_path);
    const int source_race_descriptor =
        ::open(g_race_source_path.c_str(), O_RDONLY | O_CLOEXEC);
    if(source_race_descriptor < 0) {
        throw std::runtime_error(
            "Failed to open source descriptor race fixture");
    }
    g_race_contents = "CONCURRENT=source-after-copy\n";
    run_source_preference_race_once_for_test(
        source_race_package,
        SourcePreferenceTestRacePoint::AtPublicationBoundary,
        mutate_source_descriptor_for_race);
    try {
        replace_source_preference_entry_from_descriptor(
            source_race_package, source_race_descriptor,
            source_race_identity.identity);
        static_cast<void>(::close(source_race_descriptor));
        throw std::runtime_error(
            "Atomic replacement accepted a changing source descriptor");
    } catch(const SourcePreferenceError& error) {
        if(::close(source_race_descriptor) != 0) {
            throw std::runtime_error(
                "Failed to close source descriptor race fixture");
        }
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Changing source descriptor lost its typed error");
    }
    expect_equal(
        "source descriptor race preserved destination",
        read_preference_contents(source_race_package),
        "ORIGINAL=source-race\n");
    fs::remove(g_race_source_path);

    const std::string temporary_race_package = "race-temporary-target";
    write_preference(temporary_race_package, "ORIGINAL=temporary-race\n");
    const SourcePreferenceLoaded temporary_race_identity =
        expect_strict_alternative<SourcePreferenceLoaded>(
            read_source_preference_strict(temporary_race_package),
            "internal temporary race identity");
    const fs::path temporary_race_source =
        source_preference_root().parent_path() /
        "race-temporary-source.fixture";
    g_race_contents = "DESIRED=temporary-race\n";
    write_race_contents(temporary_race_source);
    const int temporary_race_descriptor =
        ::open(temporary_race_source.c_str(), O_RDONLY | O_CLOEXEC);
    if(temporary_race_descriptor < 0) {
        throw std::runtime_error(
            "Failed to open internal temporary race source");
    }
    g_race_contents = "COMPETING=temporary-leaf\n";
    g_race_internal_path.clear();
    run_source_preference_race_once_for_test(
        temporary_race_package,
        SourcePreferenceTestRacePoint::BeforePublication,
        replace_internal_temporary_for_race);
    try {
        replace_source_preference_entry_from_descriptor(
            temporary_race_package, temporary_race_descriptor,
            temporary_race_identity.identity);
        static_cast<void>(::close(temporary_race_descriptor));
        throw std::runtime_error(
            "Atomic replacement published a substituted temporary leaf");
    } catch(const SourcePreferenceError& error) {
        if(::close(temporary_race_descriptor) != 0) {
            throw std::runtime_error(
                "Failed to close internal temporary race source");
        }
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Substituted temporary leaf lost its typed error");
    }
    expect_equal(
        "temporary mismatch preserved the original destination",
        read_preference_contents(temporary_race_package),
        "ORIGINAL=temporary-race\n");
    expect(
        !g_race_internal_path.empty() &&
            fs::is_regular_file(g_race_internal_path),
        "Temporary leaf race discarded the competing file");
    {
        std::ifstream competitor(g_race_internal_path, std::ios::binary);
        const std::string competitor_contents(
            (std::istreambuf_iterator<char>(competitor)),
            std::istreambuf_iterator<char>());
        expect_equal(
            "temporary mismatch preserved the untrusted artifact",
            competitor_contents, g_race_contents);
    }
    fs::remove(g_race_internal_path);
    fs::remove(temporary_race_source);

    const std::string in_place_temporary_race_package =
        "race-temporary-in-place-target";
    write_preference(
        in_place_temporary_race_package,
        "ORIGINAL=temporary-in-place-race\n");
    const SourcePreferenceLoaded in_place_temporary_race_identity =
        expect_strict_alternative<SourcePreferenceLoaded>(
            read_source_preference_strict(
                in_place_temporary_race_package),
            "same-inode internal temporary race identity");
    const fs::path in_place_temporary_race_source =
        source_preference_root().parent_path() /
        "race-temporary-in-place-source.fixture";
    g_race_contents = "DESIRED=temporary-in-place-race\n";
    write_race_contents(in_place_temporary_race_source);
    const int in_place_temporary_race_descriptor = ::open(
        in_place_temporary_race_source.c_str(), O_RDONLY | O_CLOEXEC);
    if(in_place_temporary_race_descriptor < 0) {
        throw std::runtime_error(
            "Failed to open same-inode temporary race source");
    }
    g_race_contents = "CONCURRENT=temporary-in-place\n";
    g_race_internal_path.clear();
    run_source_preference_race_once_for_test(
        in_place_temporary_race_package,
        SourcePreferenceTestRacePoint::BeforePublication,
        mutate_internal_temporary_in_place_for_race);
    try {
        replace_source_preference_entry_from_descriptor(
            in_place_temporary_race_package,
            in_place_temporary_race_descriptor,
            in_place_temporary_race_identity.identity);
        static_cast<void>(::close(in_place_temporary_race_descriptor));
        throw std::runtime_error(
            "Atomic replacement accepted a same-inode temporary mutation");
    } catch(const SourcePreferenceError& error) {
        if(::close(in_place_temporary_race_descriptor) != 0) {
            throw std::runtime_error(
                "Failed to close same-inode temporary race source");
        }
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Same-inode temporary mutation lost its typed error");
    }
    expect_equal(
        "same-inode temporary race preserved canonical entry",
        read_preference_contents(in_place_temporary_race_package),
        "ORIGINAL=temporary-in-place-race\n");
    expect(
        !g_race_internal_path.empty() &&
            fs::is_regular_file(g_race_internal_path),
        "Same-inode temporary race discarded the changed artifact");
    {
        std::ifstream artifact(g_race_internal_path, std::ios::binary);
        const std::string artifact_contents(
            (std::istreambuf_iterator<char>(artifact)),
            std::istreambuf_iterator<char>());
        expect_equal(
            "same-inode temporary race retained changed contents",
            artifact_contents, g_race_contents);
    }
    fs::remove(g_race_internal_path);
    fs::remove(in_place_temporary_race_source);

    const std::string remove_package = "race-remove-target";
    write_preference(remove_package, "ORIGINAL=remove\n");
    g_race_contents = "COMPETING=remove\n";
    run_source_preference_race_once_for_test(
        remove_package,
        SourcePreferenceTestRacePoint::AtRemovalBoundary,
        replace_preference_for_race);
    try {
        static_cast<void>(remove_source_preference_entry(remove_package));
        throw std::runtime_error(
            "Atomic removal deleted a competing entry");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Removal race lost its typed error");
    }
    expect(
        !fs::exists(source_preference_entry_path(remove_package)),
        "Removal mismatch restored an untrusted tombstone");
    const std::vector<fs::path> removal_artifacts =
        internal_artifacts_for_package(
            source_preference_entry_path(remove_package));
    expect(
        removal_artifacts.size() == 1,
        "Removal mismatch did not preserve exactly one tombstone artifact");
    {
        std::ifstream artifact(removal_artifacts.front(), std::ios::binary);
        const std::string artifact_contents(
            (std::istreambuf_iterator<char>(artifact)),
            std::istreambuf_iterator<char>());
        expect_equal(
            "removal mismatch preserved the untrusted tombstone",
            artifact_contents, g_race_contents);
    }
    fs::remove(removal_artifacts.front());

    const std::string post_remove_package = "race-post-remove-target";
    write_preference(post_remove_package, "ORIGINAL=post-remove\n");
    g_race_contents = "COMPETING=post-remove\n";
    run_source_preference_race_once_for_test(
        post_remove_package,
        SourcePreferenceTestRacePoint::AfterRemoval,
        replace_preference_for_race);
    try {
        static_cast<void>(remove_source_preference_entry(post_remove_package));
        throw std::runtime_error(
            "Post-removal recreation race returned success");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::ConcurrentReplacement,
            "Post-removal recreation race lost its typed error");
    }
    expect_equal(
        "post-removal race preserved canonical competitor",
        read_preference_contents(post_remove_package), g_race_contents);
    reset_source_preference_test_hooks();
}

void test_snapshot_authority_failure_is_typed() {
    ScopedEnvironmentVariable invalid_authority(
        "XDG_CONFIG_HOME", "relative-source-preference-config");
    try {
        static_cast<void>(snapshot_source_preference_directory());
        throw std::runtime_error(
            "Directory snapshot accepted a relative XDG authority");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::AuthorityUnavailable,
            "Directory snapshot authority failure lost its typed kind");
    }
    try {
        static_cast<void>(snapshot_source_preferences_for_listing());
        throw std::runtime_error(
            "Listing snapshot accepted a relative XDG authority");
    } catch(const SourcePreferenceError& error) {
        expect(
            error.failure().kind ==
                SourcePreferenceFailureKind::AuthorityUnavailable,
            "Listing snapshot authority failure lost its typed kind");
    }
}

void test_strict_fifo_entry_is_rejected_without_opening_it() {
    const std::string package_name = "strict-fifo-target";
    const fs::path entry_path = source_preference_entry_path(package_name);
    remove_preference_entry(package_name);
    if(::mkfifo(entry_path.c_str(), 0600) != 0) {
        const int fixture_errno = errno;
        throw std::runtime_error(
            "Failed to create strict FIFO fixture: " +
            std::string(std::strerror(fixture_errno)));
    }

    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::UnsupportedFileType,
        "strict FIFO preference");
    expect(
        failure.observed_file_type == fs::file_type::fifo,
        "Strict FIFO preference lost its observed file type");
}

void test_strict_open_failure_is_typed_and_one_shot() {
    const std::string package_name = "strict-open-failure-target";
    const std::string contents = "VALUE=available\n";
    write_preference(package_name, contents);
    reset_source_preference_test_hooks();
    fail_next_source_preference_operation_for_test(
        package_name, SourcePreferenceTestFailurePoint::Open);

    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::OpenFailed,
        "strict open failure");
    expect(
        failure.system_error == std::make_error_code(std::errc::permission_denied),
        "Strict open failure lost its system error");
    expect_equal(
        "strict open failure left the file unchanged",
        read_preference_contents(package_name), contents);

    StrictSourcePreferenceResult retry =
        read_source_preference_strict(package_name);
    const SourcePreferenceLoaded loaded =
        expect_strict_alternative<SourcePreferenceLoaded>(
            retry, "strict open failure one-shot retry");
    expect_assignment(loaded.environment, 0, "VALUE", "available");
}

void test_strict_read_failure_does_not_publish_partial_environment() {
    const std::string package_name = "strict-read-failure-target";
    const std::string contents =
        "FIRST=partial-byte-source\n"
        "SECOND=must-not-publish\n";
    write_preference(package_name, contents);
    reset_source_preference_test_hooks();
    fail_next_source_preference_operation_for_test(
        package_name, SourcePreferenceTestFailurePoint::Read);

    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    const SourcePreferenceFailure failure = expect_strict_failure(
        result, SourcePreferenceFailureKind::ReadFailed,
        "strict partial read failure");
    expect(
        failure.system_error == std::make_error_code(std::errc::io_error),
        "Strict read failure lost its system error");
    expect(
        std::get_if<SourcePreferenceLoaded>(&result) == nullptr,
        "Strict read failure published a partial environment");
    expect_equal(
        "strict read failure left the file unchanged",
        read_preference_contents(package_name), contents);

    StrictSourcePreferenceResult retry =
        read_source_preference_strict(package_name);
    const SourcePreferenceLoaded loaded =
        expect_strict_alternative<SourcePreferenceLoaded>(
            retry, "strict read failure one-shot retry");
    expect(
        loaded.environment.ordered_assignments.size() == 2,
        "Strict read failure retry did not load the full file");
    expect_assignment(loaded.environment, 0, "FIRST", "partial-byte-source");
    expect_assignment(loaded.environment, 1, "SECOND", "must-not-publish");
}

void test_promotion_round_trip_and_owned_bytes() {
    SourceBuildEnvironment environment{{{"CUSTOM", "double\"# tail"}, {"CUSTOM", "single'# tail"}, {"SPACES", "  two words  "}, {"ESCAPE", "backslash\\tail"}, {"EMPTY", ""}, {"PUNCT", "$-;`= #"}}};
    const auto prepared = prepare_source_preference_contents(environment);
    const std::string exact_bytes = prepared.serialized_contents();
    // The publication consumes the preflight's owned bytes, even if the
    // invocation's environment is subsequently changed by its caller.
    environment.ordered_assignments[0].value = "changed-after-preflight";
    create_source_preference_from_environment_if_absent("promotion-special-values", prepared);
    expect_equal("owned serialized bytes published unchanged", read_preference_contents("promotion-special-values"), exact_bytes);
    const auto loaded = expect_strict_alternative<SourcePreferenceLoaded>(
        read_source_preference_strict("promotion-special-values"), "special-value round-trip");
    expect(loaded.environment.ordered_assignments.size() == 6, "special-value round-trip changed environment length");
    expect_assignment(loaded.environment, 0, "CUSTOM", "double\"# tail");
    expect_assignment(loaded.environment, 1, "CUSTOM", "single'# tail");
    expect_assignment(loaded.environment, 2, "SPACES", "  two words  ");
    expect_assignment(loaded.environment, 3, "ESCAPE", "backslash\\tail");
    expect_assignment(loaded.environment, 4, "EMPTY", "");
    expect_assignment(loaded.environment, 5, "PUNCT", "$-;`= #");

    for(const std::string& value : {std::string("literal $HOME"), std::string("literal ${HOME}"),
                                    std::string("first\nSECOND=injected"), std::string("\"#'#\"#")}) {
        try {
            static_cast<void>(prepare_source_preference_contents(SourceBuildEnvironment{{{"FIRST", "keep"}, {"UNREPRESENTABLE", value}}}));
            throw std::runtime_error("nonrepresentable environment was accepted");
        } catch(const std::invalid_argument&) {
        }
    }
    try {
        static_cast<void>(prepare_source_preference_contents(SourceBuildEnvironment{{{"1INVALID", "value"}}}));
        throw std::runtime_error("invalid environment key was accepted");
    } catch(const std::invalid_argument&) {
    }
    // A later duplicate must not change into an expansion of the preceding
    // assignment, even when that key exists in the preference parser's map.
    try {
        static_cast<void>(prepare_source_preference_contents(SourceBuildEnvironment{{{"FIRST", "keep"}, {"FIRST", "$FIRST"}}}));
        throw std::runtime_error("ordered duplicate variable expansion was accepted");
    } catch(const std::invalid_argument&) {
    }
}

void test_environment_promotion_atomic_writer() {
    const SourceBuildEnvironment environment{{{"CFLAGS", "-O1"}, {"CFLAGS", "-O3"}, {"CUSTOM", "two words # literal"}, {"FOO", ""}}};
    const std::string package = "promotion-new-target";
    remove_preference_entry(package);
    create_source_preference_from_environment_if_absent(package, prepare_source_preference_contents(environment));
    const auto loaded = expect_strict_alternative<SourcePreferenceLoaded>(
        read_source_preference_strict(package), "promotion saved environment");
    expect(loaded.environment.ordered_assignments.size() == 4, "promotion lost ordered assignments");
    expect_assignment(loaded.environment, 0, "CFLAGS", "-O1");
    expect_assignment(loaded.environment, 1, "CFLAGS", "-O3");
    expect_assignment(loaded.environment, 2, "CUSTOM", "two words # literal");
    expect_assignment(loaded.environment, 3, "FOO", "");
    expect_words("promotion one-off empty forward", materialize_source_build_environment_assignment_words(loaded.environment, SourceEnvironmentEmptyValuePolicy::Forward),
                 {"CFLAGS=-O1", "CFLAGS=-O3", "CUSTOM=two words # literal", "FOO="});
    expect_words("promotion saved empty omit", materialize_source_build_environment_assignment_words(loaded.environment, SourceEnvironmentEmptyValuePolicy::Omit),
                 {"CFLAGS=-O1", "CFLAGS=-O3", "CUSTOM=two words # literal"});
    require_mode(source_preference_entry_path(package), 0600, "promotion entry");
    const std::string original = read_preference_contents(package);
    try {
        create_source_preference_from_environment_if_absent(package, prepare_source_preference_contents(SourceBuildEnvironment{{{"OTHER", "replacement"}}}));
        throw std::runtime_error("promotion replaced an existing preference");
    } catch(const SourcePreferenceError& error) {
        expect(error.failure().kind == SourcePreferenceFailureKind::ConcurrentReplacement, "promotion collision lost typed error");
    }
    expect_equal("existing preference bytes preserved", read_preference_contents(package), original);

    const std::string unsafe = "promotion-unsafe-target";
    write_preference(unsafe, "ORIGINAL=unsafe-mode\n");
    fs::permissions(source_preference_entry_path(unsafe), fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_write, fs::perm_options::replace);
    try {
        create_source_preference_from_environment_if_absent(unsafe, prepare_source_preference_contents(environment));
        throw std::runtime_error("promotion accepted unsafe entry permissions");
    } catch(const SourcePreferenceError& error) {
        expect(error.failure().kind == SourcePreferenceFailureKind::UnsafePermissions, "promotion unsafe mode lost typed error");
    }
    expect_equal("unsafe entry bytes preserved", read_preference_contents(unsafe), "ORIGINAL=unsafe-mode\n");
    remove_preference_entry(unsafe);
    fs::create_symlink(source_preference_entry_path(package), source_preference_entry_path(unsafe));
    try {
        create_source_preference_from_environment_if_absent(unsafe, prepare_source_preference_contents(environment));
        throw std::runtime_error("promotion followed an entry symlink");
    } catch(const SourcePreferenceError& error) {
        expect(error.failure().kind == SourcePreferenceFailureKind::UnsupportedFileType, "promotion symlink lost typed error");
    }
    expect_equal("symlink target bytes preserved", read_preference_contents(package), original);
    remove_preference_entry(unsafe);
    fs::create_directory(source_preference_entry_path(unsafe));
    try {
        create_source_preference_from_environment_if_absent(unsafe, prepare_source_preference_contents(environment));
        throw std::runtime_error("promotion accepted a directory entry");
    } catch(const SourcePreferenceError& error) {
        expect(error.failure().kind == SourcePreferenceFailureKind::UnsupportedFileType, "promotion directory lost typed error");
    }
    remove_preference_entry(unsafe);

    for(const auto point : {SourcePreferenceTestRacePoint::BeforePublication, SourcePreferenceTestRacePoint::AtPublicationBoundary}) {
        const std::string raced = point == SourcePreferenceTestRacePoint::BeforePublication
                                      ? "promotion-pre-publication-race"
                                      : "promotion-at-publication-race";
        remove_preference_entry(raced);
        g_race_contents = "COMPETING=keep-original\n";
        run_source_preference_race_once_for_test(raced, point, replace_preference_for_race);
        try {
            create_source_preference_from_environment_if_absent(raced, prepare_source_preference_contents(environment));
            throw std::runtime_error("promotion overwrote a concurrent preference");
        } catch(const SourcePreferenceError& error) {
            expect(error.failure().kind == SourcePreferenceFailureKind::ConcurrentReplacement, "promotion race lost typed error");
        }
        expect_equal("concurrent preference preserved", read_preference_contents(raced), g_race_contents);
        expect(internal_artifacts_for_package(source_preference_entry_path(raced)).empty(), "promotion race left a partial artifact");
    }
    for(const auto point : {SourcePreferenceTestFailurePoint::Write, SourcePreferenceTestFailurePoint::Sync,
                            SourcePreferenceTestFailurePoint::Publication}) {
        const std::string failed = "promotion-io-failure";
        remove_preference_entry(failed);
        fail_next_source_preference_operation_for_test(failed, point);
        try {
            create_source_preference_from_environment_if_absent(failed, prepare_source_preference_contents(environment));
            throw std::runtime_error("promotion writer failure returned success");
        } catch(const SourcePreferenceError& error) {
            const auto kind = point == SourcePreferenceTestFailurePoint::Write  ? SourcePreferenceFailureKind::WriteFailed
                              : point == SourcePreferenceTestFailurePoint::Sync ? SourcePreferenceFailureKind::SyncFailed
                                                                                : SourcePreferenceFailureKind::RenameFailed;
            expect(error.failure().kind == kind, "promotion IO failure lost typed kind");
        }
        expect(!fs::exists(source_preference_entry_path(failed)), "promotion IO failure published partial preference");
        expect(internal_artifacts_for_package(source_preference_entry_path(failed)).empty(), "promotion IO failure left partial artifact");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if(argc != 2) {
            throw std::runtime_error(
                "Usage: source-environment-test <fixture-root>");
        }

        const fs::path expected_root = fs::path(argv[1]).lexically_normal();
        expect(
            source_preference_root().lexically_normal() == expected_root,
            "Source preference XDG authority did not resolve to the fixture root");
        fs::create_directories(expected_root.parent_path().parent_path());
        fs::permissions(
            expected_root.parent_path().parent_path(),
            fs::perms::owner_all, fs::perm_options::replace);
        std::error_code remove_error;
        fs::remove_all(expected_root.parent_path(), remove_error);
        if(remove_error) {
            throw std::runtime_error(
                "Failed to reset source preference fixture: " +
                remove_error.message());
        }

        test_absent_environment();
        test_native_mutation_creation_boundary_and_identity();
        test_environment_promotion_atomic_writer();
        test_promotion_round_trip_and_owned_bytes();
        test_preference_parsing_and_serialization();
        test_raw_assignment_word_materialization();
        test_empty_duplicate_pkgdest_definitions();
        test_mixed_duplicate_pkgdest_uses_last_value_for_expansion();
        test_invalid_only_preference();
        test_callback_timing_and_absent_behavior();
        test_strict_absent_and_empty_file_are_distinct();
        test_strict_parser_compatibility_and_file_preservation();
        test_strict_malformed_only_file_is_loaded_with_warnings();
        test_strict_status_failure_is_not_absent();
        test_strict_symlink_is_rejected_by_all_readers();
        test_strict_dangling_symlink_is_not_absent();
        test_strict_directory_entry_is_rejected();
        test_strict_fifo_entry_is_rejected_without_opening_it();
        test_unsafe_mode_is_a_typed_hard_error();
        test_directory_and_listing_snapshots_are_all_or_nothing();
        test_cooperative_reader_writer_directory_locks();
        test_deterministic_descriptor_races_fail_closed();
        test_snapshot_authority_failure_is_typed();
        test_strict_open_failure_is_typed_and_one_shot();
        test_strict_read_failure_does_not_publish_partial_environment();
        reset_source_preference_test_hooks();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "source environment tests: all checks passed\n";
    return 0;
}
