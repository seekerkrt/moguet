#include "source_preference.hpp"

#include "localization.hpp"
#include "package_identifier.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <istream>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

struct SourcePreferenceDirectoryAccess {
    static int descriptor(
        const xdg_directory_safety::PreparedDirectory& directory) {
        directory.require_unchanged_identity();
        return directory.directory_descriptor_;
    }
};

// source-build preferenceのXDG authority、descriptor-based IO、parseを所有する。
// POLICY: Loggerとcommand handler policyはconsumer側に残す。
namespace {

namespace fs = std::filesystem;

constexpr mode_t SOURCE_PREFERENCE_ENTRY_MODE = 0600;
// Leading '-' makes an interrupted temporary entry invalid as a package name.
// A stale artifact left after its cooperative lock owner exits, or one created
// by a non-cooperating process, therefore fails closed during a later snapshot.
constexpr std::string_view ATOMIC_TEMP_PREFIX = "-.moguet-source-preference-";

std::atomic<std::uint64_t> g_atomic_temp_sequence{0};

class SourcePreferenceDescriptor final {
    int descriptor_ = -1;

public:
    explicit SourcePreferenceDescriptor(int descriptor = -1) noexcept
        : descriptor_(descriptor) {
    }

    SourcePreferenceDescriptor(const SourcePreferenceDescriptor&) = delete;
    SourcePreferenceDescriptor& operator=(const SourcePreferenceDescriptor&) = delete;

    SourcePreferenceDescriptor(SourcePreferenceDescriptor&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {
    }

    SourcePreferenceDescriptor& operator=(
        SourcePreferenceDescriptor&& other) noexcept {
        if(this == &other) return *this;
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
        descriptor_ = std::exchange(other.descriptor_, -1);
        return *this;
    }

    ~SourcePreferenceDescriptor() noexcept {
        if(descriptor_ >= 0) static_cast<void>(::close(descriptor_));
    }

    int get() const noexcept {
        return descriptor_;
    }

    int release() noexcept {
        return std::exchange(descriptor_, -1);
    }
};

class SourcePreferenceDirectoryStream final {
    DIR* stream_ = nullptr;

public:
    explicit SourcePreferenceDirectoryStream(DIR* stream) noexcept
        : stream_(stream) {
    }

    SourcePreferenceDirectoryStream(const SourcePreferenceDirectoryStream&) = delete;
    SourcePreferenceDirectoryStream& operator=(
        const SourcePreferenceDirectoryStream&) = delete;

    ~SourcePreferenceDirectoryStream() noexcept {
        if(stream_ != nullptr) static_cast<void>(::closedir(stream_));
    }

    DIR* get() const noexcept {
        return stream_;
    }

    int close() noexcept {
        DIR* stream = std::exchange(stream_, nullptr);
        return stream == nullptr ? 0 : ::closedir(stream);
    }
};

#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
struct SourcePreferenceInjectedFailure {
    std::filesystem::path entry_path;
    SourcePreferenceTestFailurePoint failure_point;
    bool partial_read_completed = false;
};

struct SourcePreferenceInjectedRace {
    std::filesystem::path entry_path;
    SourcePreferenceTestRacePoint race_point;
    SourcePreferenceTestRaceHandler handler = nullptr;
};

std::optional<SourcePreferenceInjectedFailure> g_source_preference_injected_failure;
std::optional<SourcePreferenceInjectedRace> g_source_preference_injected_race;

void invoke_source_preference_race_for_test(
    const std::filesystem::path& entry_path,
    SourcePreferenceTestRacePoint race_point) {
    if(!g_source_preference_injected_race.has_value() ||
       g_source_preference_injected_race->entry_path != entry_path ||
       g_source_preference_injected_race->race_point != race_point) {
        return;
    }
    const SourcePreferenceTestRaceHandler handler =
        g_source_preference_injected_race->handler;
    g_source_preference_injected_race.reset();
    if(handler != nullptr) handler(entry_path);
}

bool has_source_preference_failure_for_test(
    const std::filesystem::path& entry_path,
    SourcePreferenceTestFailurePoint failure_point) {
    return g_source_preference_injected_failure.has_value() &&
           g_source_preference_injected_failure->entry_path == entry_path &&
           g_source_preference_injected_failure->failure_point == failure_point;
}

bool consume_source_preference_failure_for_test(
    const std::filesystem::path& entry_path,
    SourcePreferenceTestFailurePoint failure_point) {
    if(!has_source_preference_failure_for_test(entry_path, failure_point)) return false;
    g_source_preference_injected_failure.reset();
    return true;
}

ssize_t read_source_preference_bytes_for_test(
    const std::filesystem::path& entry_path,
    int descriptor,
    char* buffer,
    size_t buffer_size) {
    if(!has_source_preference_failure_for_test(
           entry_path, SourcePreferenceTestFailurePoint::Read)) {
        return ::read(descriptor, buffer, buffer_size);
    }

    if(g_source_preference_injected_failure->partial_read_completed) {
        g_source_preference_injected_failure.reset();
        errno = EIO;
        return -1;
    }

    // One real byteを先に返し、通常read error pathがpartial resultをpublishしないことを試す。
    const ssize_t read_size = ::read(descriptor, buffer, buffer_size == 0 ? 0 : 1);
    if(read_size > 0) {
        g_source_preference_injected_failure->partial_read_completed = true;
        return read_size;
    }

    const int read_errno = errno;
    if(read_size < 0 && read_errno == EINTR) return read_size;
    g_source_preference_injected_failure.reset();
    if(read_size == 0) {
        errno = EIO;
        return -1;
    }
    errno = read_errno;
    return read_size;
}
#endif

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, last - first + 1);
}

std::string strip_comment(const std::string& line) {
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;

    for(size_t i = 0; i < line.length(); ++i) {
        char ch = line[i];
        if(escaped) {
            escaped = false;
            continue;
        }
        if(ch == '\\' && in_double_quote) {
            escaped = true;
            continue;
        }
        if(ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            continue;
        }
        if(ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            continue;
        }
        if(ch == '#' && !in_single_quote && !in_double_quote) {
            return line.substr(0, i);
        }
    }
    return line;
}

std::string expand_config_vars(
    std::string value, const std::map<std::string, std::string>& variables) {
    std::regex brace_pattern(R"(\$\{([A-Za-z0-9_]+)\})");
    std::regex simple_pattern(R"(\$([A-Za-z0-9_]+))");
    std::smatch match;

    for(int i = 0; i < 32 && std::regex_search(value, match, brace_pattern); ++i) {
        std::string variable_name = match[1];
        std::string replacement =
            variables.count(variable_name) ? variables.at(variable_name) : "";
        std::string next = match.prefix().str() + replacement + match.suffix().str();
        if(next == value) break;
        value = next;
    }
    for(int i = 0; i < 32 && std::regex_search(value, match, simple_pattern); ++i) {
        std::string variable_name = match[1];
        std::string replacement =
            variables.count(variable_name) ? variables.at(variable_name) : "";
        std::string next = match.prefix().str() + replacement + match.suffix().str();
        if(next == value) break;
        value = next;
    }
    return value;
}

template <typename WarningHandler>
SourceBuildEnvironment parse_source_preference(
    std::istream& file, bool should_emit_warnings,
    WarningHandler&& on_warning) {
    // POLICY: warningはparse中に通知し、legacy callbackのtimingとthrow伝播を維持する。
    std::string line;
    SourceBuildEnvironment environment;
    std::map<std::string, std::string> variables;

    while(std::getline(file, line)) {
        line = strip_comment(line);
        if(trim(line).empty()) continue;

        std::string key, value;
        if(split_env_assignment(line, key, value)) {
            try {
                value = expand_config_vars(value, variables);
            } catch(const std::exception& error) {
                if(should_emit_warnings) {
                    // TRANSLATORS: The placeholders are an environment key and
                    // the expansion failure detail.
                    on_warning(localization::format_translated_message(
                        "Failed to expand variables for {}: {}", key,
                        error.what()));
                }
            }
            variables[key] = value;
            // POLICY(#242): expansion用last-value mapとは別に、valid assignmentを
            // emptyも含めてread順のまま保持する。
            environment.ordered_assignments.push_back({key, value});
        } else if(line.find('=') != std::string::npos && should_emit_warnings) {
            // TRANSLATORS: The placeholder is the literal invalid assignment.
            on_warning(localization::format_translated_message(
                "Ignoring invalid environment assignment: {}",
                trim(line)));
        }
    }
    return environment;
}

std::error_code current_system_error() {
    return std::error_code(errno, std::generic_category());
}

bool source_preference_contents_match_environment(const std::string& contents, const SourceBuildEnvironment& environment) {
    std::istringstream input(contents);
    const auto parsed = parse_source_preference(input, false, [](const std::string&) {});
    if(parsed.ordered_assignments.size() != environment.ordered_assignments.size()) return false;
    for(std::size_t index = 0; index < environment.ordered_assignments.size(); ++index) {
        const auto& expected = environment.ordered_assignments[index];
        const auto& actual = parsed.ordered_assignments[index];
        if(expected.key != actual.key || expected.value != actual.value) return false;
    }
    return true;
}

std::string serialize_source_preference_environment(const SourceBuildEnvironment& environment) {
    std::string contents;
    SourceBuildEnvironment prefix;
    for(const auto& assignment : environment.ordered_assignments) {
        std::string key, value;
        if(!split_env_assignment(assignment.key + "=", key, value) || key != assignment.key) {
            throw std::invalid_argument(localization::format_translated_message(
                "Invalid environment assignment: {}", assignment.key + "="));
        }
        prefix.ordered_assignments.push_back(assignment);
        bool is_representable = false;
        // Preference quoting is not shell quoting: its existing parser expands
        // variables and strips comments without decoding shell escapes. Use
        // only a spelling that the actual consumer reads back without change.
        for(const std::string& encoded_value : std::array{
                "\"" + assignment.value + "\"", "'" + assignment.value + "'", assignment.value}) {
            const std::string candidate = contents + assignment.key + "=" + encoded_value + "\n";
            if(source_preference_contents_match_environment(candidate, prefix)) {
                contents = candidate;
                is_representable = true;
                break;
            }
        }
        if(!is_representable) {
            throw std::invalid_argument(localization::format_translated_message(
                "Cannot save environment assignment {} exactly in the existing source preference format; use a one-off build.",
                assignment.key));
        }
    }
    // This final whole-environment check is the publication oracle, including
    // duplicate keys, empty values and any parser interaction between lines.
    if(!source_preference_contents_match_environment(contents, environment)) {
        throw std::logic_error(localization::translate_message(
            "Source preference serialization did not preserve the complete environment."));
    }
    return contents;
}

std::string source_preference_system_diagnostic(
    SourcePreferenceFailureKind kind,
    const fs::path& entry_path,
    const std::error_code& error) {
    switch(kind) {
        case SourcePreferenceFailureKind::DirectoryEnumerationFailed:
            return localization::format_translated_message(
                "Failed to enumerate source preference directory {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::StatusUnavailable:
            return localization::format_translated_message(
                "Failed to inspect source preference entry {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::OpenFailed:
            return localization::format_translated_message(
                "Failed to open source preference entry {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::LockFailed:
            return localization::format_translated_message(
                "Failed to lock source preference storage {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::ReadFailed:
            return localization::format_translated_message(
                "Failed to read source preference entry {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::WriteFailed:
            return localization::format_translated_message(
                "Failed to write source preference entry {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::SyncFailed:
            return localization::format_translated_message(
                "Failed to synchronize source preference storage {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::RenameFailed:
            return localization::format_translated_message(
                "Failed to publish source preference entry {} atomically: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::RemoveFailed:
            return localization::format_translated_message(
                "Failed to remove source preference entry {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::CloseFailed:
            return localization::format_translated_message(
                "Failed to close source preference storage {}: {}",
                entry_path.string(), error.message());
        case SourcePreferenceFailureKind::AuthorityUnavailable:
        case SourcePreferenceFailureKind::InvalidEntryName:
        case SourcePreferenceFailureKind::UnsupportedFileType:
        case SourcePreferenceFailureKind::OwnershipMismatch:
        case SourcePreferenceFailureKind::UnsafePermissions:
        case SourcePreferenceFailureKind::ConcurrentReplacement:
            break;
    }
    return localization::format_translated_message(
        "Source preference entry {} failed with an unknown error: {}",
        entry_path.string(), error.message());
}

SourcePreferenceFailure source_preference_system_failure(
    SourcePreferenceFailureKind kind,
    const fs::path& entry_path,
    const std::error_code& error) {
    return {
        .kind = kind,
        .entry_path = entry_path,
        .system_error = error,
        .observed_file_type = std::nullopt,
        .diagnostic = source_preference_system_diagnostic(
            kind, entry_path, error),
    };
}

SourcePreferenceFailure source_preference_plain_failure(
    SourcePreferenceFailureKind kind,
    const fs::path& entry_path,
    std::string diagnostic,
    std::optional<fs::file_type> observed_file_type = std::nullopt) {
    return {
        .kind = kind,
        .entry_path = entry_path,
        .system_error = std::nullopt,
        .observed_file_type = observed_file_type,
        .diagnostic = std::move(diagnostic),
    };
}

[[noreturn]] void throw_source_preference_failure(
    SourcePreferenceFailure failure) {
    throw SourcePreferenceError(std::move(failure));
}

std::filesystem::file_type file_type_from_mode(mode_t mode) {
    if(S_ISREG(mode)) return std::filesystem::file_type::regular;
    if(S_ISDIR(mode)) return std::filesystem::file_type::directory;
    if(S_ISLNK(mode)) return std::filesystem::file_type::symlink;
    if(S_ISBLK(mode)) return std::filesystem::file_type::block;
    if(S_ISCHR(mode)) return std::filesystem::file_type::character;
    if(S_ISFIFO(mode)) return std::filesystem::file_type::fifo;
    if(S_ISSOCK(mode)) return std::filesystem::file_type::socket;
    return std::filesystem::file_type::unknown;
}

SourcePreferenceFailure source_preference_file_type_failure(
    const fs::path& entry_path,
    fs::file_type observed_file_type) {
    return source_preference_plain_failure(
        SourcePreferenceFailureKind::UnsupportedFileType,
        entry_path,
        localization::format_translated_message(
            "Source preference entry is not a regular file: {}",
            entry_path.string()),
        observed_file_type);
}

bool same_filesystem_identity(
    const struct stat& expected, const struct stat& actual) {
    return expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino &&
           (expected.st_mode & S_IFMT) == (actual.st_mode & S_IFMT);
}

bool same_source_preference_state(
    const struct stat& expected, const struct stat& actual) {
    return same_filesystem_identity(expected, actual) &&
           expected.st_uid == actual.st_uid &&
           (expected.st_mode & 07777) == (actual.st_mode & 07777) &&
           expected.st_size == actual.st_size &&
           expected.st_mtim.tv_sec == actual.st_mtim.tv_sec &&
           expected.st_mtim.tv_nsec == actual.st_mtim.tv_nsec &&
           expected.st_ctim.tv_sec == actual.st_ctim.tv_sec &&
           expected.st_ctim.tv_nsec == actual.st_ctim.tv_nsec;
}

// rename(2) may update ctime. Post-publication checks therefore compare every
// stable security/content field while allowing only that relocation timestamp
// to differ from the descriptor-pinned pre-rename snapshot.
bool same_relocated_source_preference_state(
    const struct stat& expected, const struct stat& actual) {
    return same_filesystem_identity(expected, actual) &&
           expected.st_uid == actual.st_uid &&
           (expected.st_mode & 07777) == (actual.st_mode & 07777) &&
           expected.st_size == actual.st_size &&
           expected.st_mtim.tv_sec == actual.st_mtim.tv_sec &&
           expected.st_mtim.tv_nsec == actual.st_mtim.tv_nsec;
}

SourcePreferenceEntryIdentity source_preference_entry_identity(
    const struct stat& status) {
    return SourcePreferenceEntryIdentity{
        static_cast<std::uintmax_t>(status.st_dev),
        static_cast<std::uintmax_t>(status.st_ino),
        static_cast<std::uintmax_t>(status.st_uid),
        static_cast<std::uintmax_t>(status.st_mode & 07777),
        static_cast<std::intmax_t>(status.st_size),
        static_cast<std::intmax_t>(status.st_mtim.tv_sec),
        static_cast<std::intmax_t>(status.st_mtim.tv_nsec),
        static_cast<std::intmax_t>(status.st_ctim.tv_sec),
        static_cast<std::intmax_t>(status.st_ctim.tv_nsec)};
}

bool matches_source_preference_identity(
    const struct stat& status,
    const SourcePreferenceEntryIdentity& identity) {
    return static_cast<std::uintmax_t>(status.st_dev) == identity.device &&
           static_cast<std::uintmax_t>(status.st_ino) == identity.inode &&
           static_cast<std::uintmax_t>(status.st_uid) == identity.owner &&
           static_cast<std::uintmax_t>(status.st_mode & 07777) == identity.mode &&
           static_cast<std::intmax_t>(status.st_size) == identity.size &&
           static_cast<std::intmax_t>(status.st_mtim.tv_sec) ==
               identity.modification_time_seconds &&
           static_cast<std::intmax_t>(status.st_mtim.tv_nsec) ==
               identity.modification_time_nanoseconds &&
           static_cast<std::intmax_t>(status.st_ctim.tv_sec) ==
               identity.status_change_time_seconds &&
           static_cast<std::intmax_t>(status.st_ctim.tv_nsec) ==
               identity.status_change_time_nanoseconds;
}

void validate_source_preference_entry_status(
    const struct stat& status,
    const fs::path& entry_path,
    std::uintmax_t expected_owner) {
    const fs::file_type file_type = file_type_from_mode(status.st_mode);
    if(file_type != fs::file_type::regular) {
        throw_source_preference_failure(
            source_preference_file_type_failure(entry_path, file_type));
    }
    if(static_cast<std::uintmax_t>(status.st_uid) != expected_owner) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::OwnershipMismatch,
            entry_path,
            localization::format_translated_message(
                // TRANSLATORS: The first placeholder is the literal XDG identity; the second is a source preference path.
                "Source preference entry ownership does not match the {} config authority: {}",
                "XDG", entry_path.string())));
    }
    if((status.st_mode & 07777) != SOURCE_PREFERENCE_ENTRY_MODE) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::UnsafePermissions,
            entry_path,
            localization::format_translated_message(
                "Source preference entry permissions are not private mode {}: {}",
                "0600", entry_path.string())));
    }
}

std::optional<struct stat> inspect_source_preference_entry(
    const xdg_directory_safety::PreparedDirectory& directory,
    const std::string& leaf_name,
    const fs::path& entry_path,
    bool allow_test_injection) {
#ifndef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    static_cast<void>(allow_test_injection);
#endif
    directory.require_unchanged_identity();
    const int directory_descriptor =
        SourcePreferenceDirectoryAccess::descriptor(directory);

    struct stat observed_status{};
    int status_result = -1;
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    if(allow_test_injection && consume_source_preference_failure_for_test(
                                   entry_path, SourcePreferenceTestFailurePoint::Status)) {
        errno = EACCES;
    } else
#endif
    {
        do {
            status_result = ::fstatat(
                directory_descriptor, leaf_name.c_str(),
                &observed_status, AT_SYMLINK_NOFOLLOW);
        } while(status_result != 0 && errno == EINTR);
    }

    if(status_result != 0) {
        const int status_error = errno;
        if(status_error == ENOENT) {
            directory.require_unchanged_identity();
            return std::nullopt;
        }
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            entry_path,
            std::error_code(status_error, std::generic_category())));
    }
    validate_source_preference_entry_status(
        observed_status, entry_path, directory.owner());
    return observed_status;
}

SourcePreferenceDescriptor open_source_preference_entry(
    const xdg_directory_safety::PreparedDirectory& directory,
    const std::string& leaf_name,
    const fs::path& entry_path,
    const struct stat& observed_status,
    bool allow_test_injection) {
#ifndef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    static_cast<void>(allow_test_injection);
#endif
    const int directory_descriptor =
        SourcePreferenceDirectoryAccess::descriptor(directory);
    int raw_descriptor = -1;
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    if(allow_test_injection && consume_source_preference_failure_for_test(
                                   entry_path, SourcePreferenceTestFailurePoint::Open)) {
        errno = EACCES;
    } else
#endif
    {
        do {
            raw_descriptor = ::openat(
                directory_descriptor, leaf_name.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        } while(raw_descriptor < 0 && errno == EINTR);
    }
    if(raw_descriptor < 0) {
        const int open_error = errno;
        if(open_error == ENOENT || open_error == ELOOP ||
           open_error == ENOTDIR) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry changed during validation: {}",
                    entry_path.string())));
        }
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::OpenFailed,
            entry_path,
            std::error_code(open_error, std::generic_category())));
    }
    SourcePreferenceDescriptor descriptor(raw_descriptor);

    struct stat opened_status{};
    int opened_status_result = -1;
    do {
        opened_status_result = ::fstat(descriptor.get(), &opened_status);
    } while(opened_status_result != 0 && errno == EINTR);
    if(opened_status_result != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            entry_path, current_system_error()));
    }
    validate_source_preference_entry_status(
        opened_status, entry_path, directory.owner());
    if(!same_source_preference_state(observed_status, opened_status)) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed during validation: {}",
                entry_path.string())));
    }

    std::optional<struct stat> revalidated_status =
        inspect_source_preference_entry(
            directory, leaf_name, entry_path, false);
    if(!revalidated_status.has_value() ||
       !same_source_preference_state(
           opened_status, revalidated_status.value())) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed during validation: {}",
                entry_path.string())));
    }
    return descriptor;
}

std::string read_source_preference_contents(
    const xdg_directory_safety::PreparedDirectory& directory,
    const std::string& leaf_name,
    const fs::path& entry_path,
    const struct stat& observed_status,
    bool allow_test_injection) {
    SourcePreferenceDescriptor descriptor = open_source_preference_entry(
        directory, leaf_name, entry_path, observed_status,
        allow_test_injection);
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    if(allow_test_injection) {
        invoke_source_preference_race_for_test(
            entry_path,
            SourcePreferenceTestRacePoint::AfterReadOpen);
    }
#endif
    std::string contents;
    std::array<char, 4096> buffer{};
    while(true) {
        ssize_t read_size = 0;
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
        if(allow_test_injection) {
            read_size = read_source_preference_bytes_for_test(
                entry_path, descriptor.get(), buffer.data(), buffer.size());
        } else
#endif
        {
            read_size = ::read(descriptor.get(), buffer.data(), buffer.size());
        }
        if(read_size > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(read_size));
            continue;
        }
        if(read_size == 0) break;
        if(errno == EINTR) continue;
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::ReadFailed,
            entry_path, current_system_error()));
    }

    struct stat retained_status{};
    if(::fstat(descriptor.get(), &retained_status) != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            entry_path, current_system_error()));
    }
    validate_source_preference_entry_status(
        retained_status, entry_path, directory.owner());
    const std::optional<struct stat> named_status =
        inspect_source_preference_entry(
            directory, leaf_name, entry_path, false);
    if(!same_source_preference_state(observed_status, retained_status) ||
       !named_status.has_value() ||
       !same_source_preference_state(
           retained_status, named_status.value())) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed while it was being read: {}",
                entry_path.string())));
    }
    directory.require_unchanged_identity();
    return contents;
}

std::vector<std::string> source_preference_display_lines(
    const std::string& contents) {
    std::vector<std::string> lines;
    std::istringstream input(contents);
    std::string line;
    while(std::getline(input, line)) {
        std::string display_line = trim(strip_comment(line));
        if(!display_line.empty()) lines.push_back(std::move(display_line));
    }
    return lines;
}

std::vector<std::string> enumerate_source_preference_names(
    const xdg_directory_safety::PreparedDirectory& directory) {
    const int directory_descriptor =
        SourcePreferenceDirectoryAccess::descriptor(directory);
    int read_descriptor = -1;
    do {
        read_descriptor = ::openat(
            directory_descriptor, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while(read_descriptor < 0 && errno == EINTR);
    if(read_descriptor < 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::DirectoryEnumerationFailed,
            directory.path(), current_system_error()));
    }
    SourcePreferenceDescriptor descriptor(read_descriptor);
    const int stream_descriptor = descriptor.release();
    DIR* raw_stream = ::fdopendir(stream_descriptor);
    if(raw_stream == nullptr) {
        const std::error_code stream_error = current_system_error();
        static_cast<void>(::close(stream_descriptor));
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::DirectoryEnumerationFailed,
            directory.path(), stream_error));
    }
    SourcePreferenceDirectoryStream stream(raw_stream);

    std::vector<std::string> names;
    while(true) {
        errno = 0;
        const dirent* entry = ::readdir(stream.get());
        if(entry == nullptr) {
            if(errno != 0) {
                throw_source_preference_failure(source_preference_system_failure(
                    SourcePreferenceFailureKind::DirectoryEnumerationFailed,
                    directory.path(), current_system_error()));
            }
            break;
        }
        const std::string name(entry->d_name);
        if(name == "." || name == "..") continue;
        if(!is_valid_package_name(name)) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::InvalidEntryName,
                directory.path() / name,
                localization::format_translated_message(
                    "Invalid source preference entry name in {}: {}",
                    directory.path().string(), name)));
        }
        names.push_back(name);
    }
    if(stream.close() != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::CloseFailed,
            directory.path(), current_system_error()));
    }
    directory.require_unchanged_identity();
    return names;
}

std::optional<xdg_directory_safety::PreparedDirectory>
open_source_preference_directory(
    const xdg_paths::SourcePreferencePaths& paths) {
    return xdg_directory_safety::open_existing_directory(paths);
}

xdg_directory_safety::PreparedDirectory prepare_source_preference_directory(
    const xdg_paths::SourcePreferencePaths& paths) {
    return xdg_directory_safety::prepare_directory(paths);
}

std::string atomic_temp_leaf(const std::string& package_name) {
    const std::uint64_t sequence =
        g_atomic_temp_sequence.fetch_add(1, std::memory_order_relaxed);
    return std::string(ATOMIC_TEMP_PREFIX) + package_name + "-" +
           std::to_string(static_cast<std::uintmax_t>(::getpid())) + "-" +
           std::to_string(sequence);
}

void write_all(
    int descriptor,
    const char* bytes,
    std::size_t size,
    const fs::path& entry_path) {
    std::size_t offset = 0;
    while(offset < size) {
        const ssize_t write_size = ::write(
            descriptor, bytes + offset, size - offset);
        if(write_size > 0) {
            offset += static_cast<std::size_t>(write_size);
            continue;
        }
        if(write_size < 0 && errno == EINTR) continue;
        if(write_size == 0) errno = EIO;
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::WriteFailed,
            entry_path, current_system_error()));
    }
}

void require_descriptor_state_unchanged(
    int descriptor,
    const struct stat& expected_status,
    const fs::path& entry_path) {
    struct stat current_status{};
    int status_result = -1;
    do {
        status_result = ::fstat(descriptor, &current_status);
    } while(status_result != 0 && errno == EINTR);
    if(status_result != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            entry_path, current_system_error()));
    }
    if(!same_source_preference_state(expected_status, current_status)) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed while it was being read: {}",
                entry_path.string())));
    }
}

void copy_descriptor_contents(
    int source_descriptor,
    int destination_descriptor,
    const fs::path& entry_path,
    const struct stat& expected_source_status) {
    require_descriptor_state_unchanged(
        source_descriptor, expected_source_status, entry_path);
    if(::lseek(source_descriptor, 0, SEEK_SET) < 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::ReadFailed,
            entry_path, current_system_error()));
    }
    std::array<char, 4096> buffer{};
    while(true) {
        const ssize_t read_size = ::read(
            source_descriptor, buffer.data(), buffer.size());
        if(read_size > 0) {
            write_all(
                destination_descriptor, buffer.data(),
                static_cast<std::size_t>(read_size), entry_path);
            continue;
        }
        if(read_size == 0) break;
        if(errno == EINTR) continue;
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::ReadFailed,
            entry_path, current_system_error()));
    }

    require_descriptor_state_unchanged(
        source_descriptor, expected_source_status, entry_path);
}

void close_descriptor_checked(
    SourcePreferenceDescriptor& descriptor,
    const fs::path& entry_path) {
    const int raw_descriptor = descriptor.release();
    if(raw_descriptor >= 0 && ::close(raw_descriptor) != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::CloseFailed,
            entry_path, current_system_error()));
    }
}

SourcePreferenceDescriptor lock_source_preference_directory(
    const xdg_directory_safety::PreparedDirectory& directory,
    int lock_operation) {
    const int directory_descriptor =
        SourcePreferenceDirectoryAccess::descriptor(directory);
    int lock_descriptor = -1;
    do {
        lock_descriptor = ::openat(
            directory_descriptor, ".",
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    } while(lock_descriptor < 0 && errno == EINTR);
    if(lock_descriptor < 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::OpenFailed,
            directory.path(), current_system_error()));
    }
    SourcePreferenceDescriptor descriptor(lock_descriptor);

    int lock_result = -1;
    do {
        lock_result = ::flock(descriptor.get(), lock_operation);
    } while(lock_result != 0 && errno == EINTR);
    if(lock_result != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::LockFailed,
            directory.path(), current_system_error()));
    }
    directory.require_unchanged_identity();
    return descriptor;
}

int renameat2_checked(
    int old_directory_descriptor,
    const char* old_leaf,
    int new_directory_descriptor,
    const char* new_leaf,
    unsigned int flags) {
#ifdef SYS_renameat2
    int rename_result = -1;
    do {
        rename_result = static_cast<int>(::syscall(
            SYS_renameat2,
            old_directory_descriptor, old_leaf,
            new_directory_descriptor, new_leaf, flags));
    } while(rename_result != 0 && errno == EINTR);
    return rename_result;
#else
    static_cast<void>(old_directory_descriptor);
    static_cast<void>(old_leaf);
    static_cast<void>(new_directory_descriptor);
    static_cast<void>(new_leaf);
    static_cast<void>(flags);
    errno = ENOSYS;
    return -1;
#endif
}

void remove_internal_entry_checked(
    int directory_descriptor,
    const std::string& leaf_name,
    const fs::path& entry_path) {
    if(::unlinkat(directory_descriptor, leaf_name.c_str(), 0) == 0) return;
    const int remove_error = errno;
    if(remove_error == ENOENT || remove_error == ENOTDIR ||
       remove_error == ELOOP) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed during publication: {}",
                entry_path.string())));
    }
    throw_source_preference_failure(source_preference_system_failure(
        SourcePreferenceFailureKind::RemoveFailed,
        entry_path,
        std::error_code(remove_error, std::generic_category())));
}

struct stat descriptor_status_checked(
    const SourcePreferenceDescriptor& descriptor,
    const fs::path& entry_path) {
    struct stat status{};
    int status_result = -1;
    do {
        status_result = ::fstat(descriptor.get(), &status);
    } while(status_result != 0 && errno == EINTR);
    if(status_result != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            entry_path, current_system_error()));
    }
    return status;
}

bool remove_internal_entry_if_descriptor_matches(
    int directory_descriptor,
    const std::string& leaf_name,
    const SourcePreferenceDescriptor& descriptor,
    const fs::path& entry_path,
    const std::optional<struct stat>& expected_status = std::nullopt) {
    const struct stat descriptor_status =
        descriptor_status_checked(descriptor, entry_path);
    if(expected_status.has_value() &&
       !same_source_preference_state(
           expected_status.value(), descriptor_status)) {
        return false;
    }
    struct stat named_status{};
    int status_result = -1;
    do {
        status_result = ::fstatat(
            directory_descriptor, leaf_name.c_str(),
            &named_status, AT_SYMLINK_NOFOLLOW);
    } while(status_result != 0 && errno == EINTR);
    if(status_result != 0) {
        const int status_error = errno;
        if(status_error == ENOENT) return false;
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            entry_path,
            std::error_code(status_error, std::generic_category())));
    }
    if((expected_status.has_value() &&
        !same_source_preference_state(
            expected_status.value(), named_status)) ||
       !same_source_preference_state(descriptor_status, named_status)) {
        return false;
    }
    remove_internal_entry_checked(
        directory_descriptor, leaf_name, entry_path);
    return true;
}

void require_expected_destination(
    const xdg_directory_safety::PreparedDirectory& directory,
    const std::string& package_name,
    const fs::path& entry_path,
    const std::optional<struct stat>& expected_status) {
    const std::optional<struct stat> current_status =
        inspect_source_preference_entry(
            directory, package_name, entry_path, false);
    if(expected_status.has_value() != current_status.has_value() ||
       (expected_status.has_value() &&
        !same_source_preference_state(
            expected_status.value(), current_status.value()))) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed during validation: {}",
                entry_path.string())));
    }
}

template <typename ContentWriter, typename PrePublicationValidator>
void replace_source_preference_entry_atomically(
    const xdg_directory_safety::PreparedDirectory& directory,
    const std::string& package_name,
    const fs::path& entry_path,
    const std::optional<SourcePreferenceEntryIdentity>& expected_identity,
    ContentWriter&& content_writer,
    PrePublicationValidator&& pre_publication_validator) {
    const int directory_descriptor =
        SourcePreferenceDirectoryAccess::descriptor(directory);
    SourcePreferenceDescriptor directory_sync =
        lock_source_preference_directory(directory, LOCK_EX);

    std::optional<struct stat> expected_status =
        inspect_source_preference_entry(
            directory, package_name, entry_path, false);
    if(expected_identity.has_value() != expected_status.has_value() ||
       (expected_identity.has_value() &&
        !matches_source_preference_identity(
            expected_status.value(), expected_identity.value()))) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed before publication: {}",
                entry_path.string())));
    }
    SourcePreferenceDescriptor retained_entry;
    if(expected_status.has_value()) {
        retained_entry = open_source_preference_entry(
            directory, package_name, entry_path,
            expected_status.value(), false);
    }

    SourcePreferenceDescriptor temporary;
    std::string temporary_leaf;
    std::optional<struct stat> cleanup_expected_temporary_status;
    for(std::size_t attempt = 0; attempt < 128; ++attempt) {
        temporary_leaf = atomic_temp_leaf(package_name);
        const int temporary_descriptor = ::openat(
            directory_descriptor, temporary_leaf.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            SOURCE_PREFERENCE_ENTRY_MODE);
        if(temporary_descriptor >= 0) {
            temporary = SourcePreferenceDescriptor(temporary_descriptor);
            break;
        }
        if(errno != EEXIST) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::OpenFailed,
                entry_path, current_system_error()));
        }
    }
    if(temporary.get() < 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::OpenFailed,
            entry_path,
            std::make_error_code(std::errc::file_exists)));
    }

    bool was_published = false;
    try {
        if(::fchmod(temporary.get(), SOURCE_PREFERENCE_ENTRY_MODE) != 0) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::WriteFailed,
                entry_path, current_system_error()));
        }
        content_writer(temporary.get());
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
        if(consume_source_preference_failure_for_test(entry_path, SourcePreferenceTestFailurePoint::Write)) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::WriteFailed, entry_path,
                std::make_error_code(std::errc::io_error)));
        }
        if(consume_source_preference_failure_for_test(entry_path, SourcePreferenceTestFailurePoint::Sync)) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::SyncFailed, entry_path,
                std::make_error_code(std::errc::io_error)));
        }
#endif
        if(::fsync(temporary.get()) != 0) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::SyncFailed,
                entry_path, current_system_error()));
        }

        struct stat temporary_status{};
        if(::fstat(temporary.get(), &temporary_status) != 0) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::StatusUnavailable,
                entry_path, current_system_error()));
        }
        cleanup_expected_temporary_status = temporary_status;
        validate_source_preference_entry_status(
            temporary_status, entry_path, directory.owner());

        require_expected_destination(
            directory, package_name, entry_path, expected_status);
        directory.require_unchanged_identity();
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
        invoke_source_preference_race_for_test(
            entry_path,
            SourcePreferenceTestRacePoint::BeforePublication);
#endif
        require_expected_destination(
            directory, package_name, entry_path, expected_status);
        const struct stat final_temporary_status =
            descriptor_status_checked(temporary, entry_path);
        if(!same_source_preference_state(
               temporary_status, final_temporary_status)) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry changed during publication: {}",
                    entry_path.string())));
        }
        struct stat named_temporary_status{};
        int named_temporary_status_result = -1;
        do {
            named_temporary_status_result = ::fstatat(
                directory_descriptor, temporary_leaf.c_str(),
                &named_temporary_status, AT_SYMLINK_NOFOLLOW);
        } while(named_temporary_status_result != 0 && errno == EINTR);
        if(named_temporary_status_result != 0 ||
           !same_source_preference_state(
               final_temporary_status, named_temporary_status)) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry changed during publication: {}",
                    entry_path.string())));
        }
        if(expected_status.has_value()) {
            const struct stat final_expected_status =
                descriptor_status_checked(retained_entry, entry_path);
            if(!same_source_preference_state(
                   expected_status.value(), final_expected_status)) {
                throw_source_preference_failure(
                    source_preference_plain_failure(
                        SourcePreferenceFailureKind::
                            ConcurrentReplacement,
                        entry_path,
                        localization::format_translated_message(
                            "Source preference entry changed during publication: {}",
                            entry_path.string())));
            }
        }
        pre_publication_validator();
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
        invoke_source_preference_race_for_test(
            entry_path,
            SourcePreferenceTestRacePoint::AtPublicationBoundary);
        if(consume_source_preference_failure_for_test(entry_path, SourcePreferenceTestFailurePoint::Publication)) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::RenameFailed, entry_path,
                std::make_error_code(std::errc::io_error)));
        }
#endif
        pre_publication_validator();

        struct stat published_descriptor_status{};
        if(expected_status.has_value()) {
            if(renameat2_checked(
                   directory_descriptor, temporary_leaf.c_str(),
                   directory_descriptor, package_name.c_str(),
                   RENAME_EXCHANGE) != 0) {
                const int rename_error = errno;
                if(rename_error == ENOENT || rename_error == ENOTDIR ||
                   rename_error == ELOOP) {
                    throw_source_preference_failure(
                        source_preference_plain_failure(
                            SourcePreferenceFailureKind::
                                ConcurrentReplacement,
                            entry_path,
                            localization::format_translated_message(
                                "Source preference entry changed during validation: {}",
                                entry_path.string())));
                }
                throw_source_preference_failure(
                    source_preference_system_failure(
                        SourcePreferenceFailureKind::RenameFailed,
                        entry_path,
                        std::error_code(
                            rename_error,
                            std::generic_category())));
            }
            was_published = true;

            published_descriptor_status =
                descriptor_status_checked(temporary, entry_path);
            const struct stat displaced_descriptor_status =
                descriptor_status_checked(retained_entry, entry_path);

            struct stat named_published_status{};
            int named_published_status_result = -1;
            do {
                named_published_status_result = ::fstatat(
                    directory_descriptor, package_name.c_str(),
                    &named_published_status, AT_SYMLINK_NOFOLLOW);
            } while(named_published_status_result != 0 && errno == EINTR);
            const int named_published_status_error = errno;
            struct stat displaced_status{};
            int displaced_status_result = -1;
            do {
                displaced_status_result = ::fstatat(
                    directory_descriptor, temporary_leaf.c_str(),
                    &displaced_status, AT_SYMLINK_NOFOLLOW);
            } while(displaced_status_result != 0 && errno == EINTR);
            const int displaced_status_error = errno;
            const bool published_name_matches =
                named_published_status_result == 0 &&
                same_source_preference_state(
                    published_descriptor_status,
                    named_published_status);
            const bool displaced_name_matches =
                displaced_status_result == 0 &&
                same_source_preference_state(
                    displaced_descriptor_status,
                    displaced_status);
            const bool publication_matches =
                published_name_matches &&
                displaced_name_matches &&
                same_relocated_source_preference_state(
                    final_temporary_status,
                    published_descriptor_status) &&
                same_relocated_source_preference_state(
                    expected_status.value(),
                    displaced_descriptor_status);
            if(!publication_matches) {
                // POLICY: after an identity mismatch neither name is safe to
                // exchange or remove. Preserve both artifacts for recovery.
                if(named_published_status_result != 0 &&
                   named_published_status_error != ENOENT) {
                    throw_source_preference_failure(
                        source_preference_system_failure(
                            SourcePreferenceFailureKind::
                                StatusUnavailable,
                            entry_path,
                            std::error_code(
                                named_published_status_error,
                                std::generic_category())));
                }
                if(displaced_status_result != 0 &&
                   displaced_status_error != ENOENT) {
                    throw_source_preference_failure(
                        source_preference_system_failure(
                            SourcePreferenceFailureKind::
                                StatusUnavailable,
                            entry_path,
                            std::error_code(
                                displaced_status_error,
                                std::generic_category())));
                }
                throw_source_preference_failure(
                    source_preference_plain_failure(
                        SourcePreferenceFailureKind::
                            ConcurrentReplacement,
                        entry_path,
                        localization::format_translated_message(
                            "Source preference entry changed during publication: {}",
                            entry_path.string())));
            }
            if(!remove_internal_entry_if_descriptor_matches(
                   directory_descriptor, temporary_leaf,
                   retained_entry, entry_path,
                   displaced_descriptor_status)) {
                throw_source_preference_failure(
                    source_preference_plain_failure(
                        SourcePreferenceFailureKind::
                            ConcurrentReplacement,
                        entry_path,
                        localization::format_translated_message(
                            "Source preference entry changed during publication: {}",
                            entry_path.string())));
            }
            temporary_leaf.clear();
        } else if(renameat2_checked(
                      directory_descriptor, temporary_leaf.c_str(),
                      directory_descriptor, package_name.c_str(),
                      RENAME_NOREPLACE) != 0) {
            const int rename_error = errno;
            if(rename_error == EEXIST || rename_error == ENOTEMPTY ||
               rename_error == ENOENT || rename_error == ENOTDIR ||
               rename_error == ELOOP) {
                throw_source_preference_failure(source_preference_plain_failure(
                    SourcePreferenceFailureKind::ConcurrentReplacement,
                    entry_path,
                    localization::format_translated_message(
                        "Source preference entry changed during validation: {}",
                        entry_path.string())));
            }
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::RenameFailed,
                entry_path,
                std::error_code(rename_error, std::generic_category())));
        } else {
            was_published = true;
            published_descriptor_status =
                descriptor_status_checked(temporary, entry_path);
            struct stat named_published_status{};
            int named_published_status_result = -1;
            do {
                named_published_status_result = ::fstatat(
                    directory_descriptor, package_name.c_str(),
                    &named_published_status, AT_SYMLINK_NOFOLLOW);
            } while(named_published_status_result != 0 && errno == EINTR);
            const int named_published_status_error = errno;
            const bool publication_matches =
                named_published_status_result == 0 &&
                same_relocated_source_preference_state(
                    final_temporary_status,
                    published_descriptor_status) &&
                same_source_preference_state(
                    published_descriptor_status,
                    named_published_status);
            if(!publication_matches) {
                // The published name is untrusted once identity validation
                // fails. Leave it in place and report the typed failure.
                if(named_published_status_result != 0 &&
                   named_published_status_error != ENOENT) {
                    throw_source_preference_failure(
                        source_preference_system_failure(
                            SourcePreferenceFailureKind::
                                StatusUnavailable,
                            entry_path,
                            std::error_code(
                                named_published_status_error,
                                std::generic_category())));
                }
                throw_source_preference_failure(
                    source_preference_plain_failure(
                        SourcePreferenceFailureKind::
                            ConcurrentReplacement,
                        entry_path,
                        localization::format_translated_message(
                            "Source preference entry changed during publication: {}",
                            entry_path.string())));
            }
            temporary_leaf.clear();
        }

#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
        invoke_source_preference_race_for_test(
            entry_path,
            SourcePreferenceTestRacePoint::AfterPublication);
#endif
        const std::optional<struct stat> published_status =
            inspect_source_preference_entry(
                directory, package_name, entry_path, false);
        if(!published_status.has_value() ||
           !same_source_preference_state(
               published_descriptor_status,
               published_status.value())) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry changed during publication: {}",
                    entry_path.string())));
        }
        close_descriptor_checked(temporary, entry_path);
        if(::fsync(directory_sync.get()) != 0) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::SyncFailed,
                directory.path(), current_system_error()));
        }
        close_descriptor_checked(directory_sync, directory.path());
        directory.require_unchanged_identity();
    } catch(...) {
        const std::exception_ptr original_failure = std::current_exception();
        if(!was_published && !temporary_leaf.empty()) {
            const std::string cleanup_leaf =
                std::exchange(temporary_leaf, {});
            if(!remove_internal_entry_if_descriptor_matches(
                   directory_descriptor, cleanup_leaf,
                   temporary, entry_path,
                   cleanup_expected_temporary_status)) {
                throw_source_preference_failure(
                    source_preference_plain_failure(
                        SourcePreferenceFailureKind::
                            ConcurrentReplacement,
                        entry_path,
                        localization::format_translated_message(
                            "Source preference entry changed during publication: {}",
                            entry_path.string())));
            }
        }
        std::rethrow_exception(original_failure);
    }
}

} // namespace

SourcePreferenceError::SourcePreferenceError(SourcePreferenceFailure failure)
    : std::runtime_error(failure.diagnostic), failure_(std::move(failure)) {
}

fs::path source_preference_root() {
    return xdg_paths::resolve_source_preference_process_environment().directory;
}

fs::path source_preference_entry_path(const std::string& package_name) {
    require_valid_package_name(package_name);
    return source_preference_root() / package_name;
}

SourcePreferenceDirectorySnapshot snapshot_source_preference_directory() try {
    SourcePreferenceDirectorySnapshot snapshot;
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference_process_environment();
    std::optional<xdg_directory_safety::PreparedDirectory> directory =
        open_source_preference_directory(paths);
    if(!directory.has_value()) return snapshot;
    [[maybe_unused]] SourcePreferenceDescriptor directory_lock =
        lock_source_preference_directory(directory.value(), LOCK_SH);

    snapshot.root_exists = true;
    const std::vector<std::string> names =
        enumerate_source_preference_names(directory.value());
    std::size_t original_index = 0;
    for(const std::string& package_name : names) {
        const fs::path entry_path = paths.directory / package_name;
        const std::optional<struct stat> observed_status =
            inspect_source_preference_entry(
                directory.value(), package_name, entry_path, false);
        if(!observed_status.has_value()) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry disappeared during enumeration: {}",
                    entry_path.string())));
        }
        static_cast<void>(open_source_preference_entry(
            directory.value(), package_name, entry_path,
            observed_status.value(), false));
        snapshot.entries.push_back(SourcePreferenceEntrySnapshot{
            original_index,
            entry_path,
            package_name,
            true});
        ++original_index;
    }
    return snapshot;
} catch(const SourcePreferenceError&) {
    throw;
} catch(const xdg_paths::ResolutionError& error) {
    throw SourcePreferenceError(source_preference_plain_failure(
        SourcePreferenceFailureKind::AuthorityUnavailable,
        {}, error.what()));
} catch(const xdg_directory_safety::PreparationError& error) {
    throw SourcePreferenceError(source_preference_plain_failure(
        SourcePreferenceFailureKind::AuthorityUnavailable,
        {}, error.what()));
}

SourcePreferenceListSnapshot snapshot_source_preferences_for_listing() try {
    SourcePreferenceListSnapshot snapshot;
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference_process_environment();
    std::optional<xdg_directory_safety::PreparedDirectory> directory =
        open_source_preference_directory(paths);
    if(!directory.has_value()) return snapshot;
    [[maybe_unused]] SourcePreferenceDescriptor directory_lock =
        lock_source_preference_directory(directory.value(), LOCK_SH);

    snapshot.root_exists = true;
    const std::vector<std::string> names =
        enumerate_source_preference_names(directory.value());
    for(const std::string& package_name : names) {
        const fs::path entry_path = paths.directory / package_name;
        const std::optional<struct stat> observed_status =
            inspect_source_preference_entry(
                directory.value(), package_name, entry_path, false);
        if(!observed_status.has_value()) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry disappeared during enumeration: {}",
                    entry_path.string())));
        }
        const std::string contents = read_source_preference_contents(
            directory.value(), package_name, entry_path,
            observed_status.value(), false);
        snapshot.entries.push_back(SourcePreferenceListEntrySnapshot{
            entry_path,
            package_name,
            source_preference_display_lines(contents)});
    }
    directory->require_unchanged_identity();
    return snapshot;
} catch(const SourcePreferenceError&) {
    throw;
} catch(const xdg_paths::ResolutionError& error) {
    throw SourcePreferenceError(source_preference_plain_failure(
        SourcePreferenceFailureKind::AuthorityUnavailable,
        {}, error.what()));
} catch(const xdg_directory_safety::PreparationError& error) {
    throw SourcePreferenceError(source_preference_plain_failure(
        SourcePreferenceFailureKind::AuthorityUnavailable,
        {}, error.what()));
}

StrictSourcePreferenceResult read_source_preference_strict(
    const std::string& package_name) {
    require_valid_package_name(package_name);
    fs::path entry_path;
    try {
        const xdg_paths::SourcePreferencePaths paths =
            xdg_paths::resolve_source_preference_process_environment();
        entry_path = paths.directory / package_name;
        std::optional<xdg_directory_safety::PreparedDirectory> directory =
            open_source_preference_directory(paths);
        if(!directory.has_value()) return SourcePreferenceAbsent{};
        [[maybe_unused]] SourcePreferenceDescriptor directory_lock =
            lock_source_preference_directory(
                directory.value(), LOCK_SH);

        const std::optional<struct stat> observed_status =
            inspect_source_preference_entry(
                directory.value(), package_name, entry_path, true);
        if(!observed_status.has_value()) return SourcePreferenceAbsent{};

        std::string contents = read_source_preference_contents(
            directory.value(), package_name, entry_path,
            observed_status.value(), true);
        std::vector<std::string> warnings;
        std::istringstream input(contents);
        SourceBuildEnvironment environment = parse_source_preference(
            input, true,
            [&warnings](const std::string& warning) {
                warnings.push_back(warning);
            });
        return SourcePreferenceLoaded{
            .entry_path = entry_path,
            .environment = std::move(environment),
            .warnings = std::move(warnings),
            .raw_contents = std::move(contents),
            .identity = source_preference_entry_identity(
                observed_status.value()),
        };
    } catch(const SourcePreferenceError& error) {
        return error.failure();
    } catch(const xdg_paths::ResolutionError& error) {
        return source_preference_plain_failure(
            SourcePreferenceFailureKind::AuthorityUnavailable,
            entry_path, error.what());
    } catch(const xdg_directory_safety::PreparationError& error) {
        return source_preference_plain_failure(
            SourcePreferenceFailureKind::AuthorityUnavailable,
            entry_path, error.what());
    }
}

SourceBuildEnvironment get_package_env(
    const std::string& package_name, SourcePreferenceLoadHandler on_load,
    SourcePreferenceWarningHandler on_warning) {
    StrictSourcePreferenceResult result =
        read_source_preference_strict(package_name);
    if(std::get_if<SourcePreferenceAbsent>(&result) != nullptr) return {};
    if(auto* failure = std::get_if<SourcePreferenceFailure>(&result)) {
        throw SourcePreferenceError(*failure);
    }

    SourcePreferenceLoaded loaded =
        std::get<SourcePreferenceLoaded>(std::move(result));
    if(on_load) on_load(loaded.entry_path);
    if(on_warning) {
        for(const std::string& warning : loaded.warnings)
            on_warning(warning);
    }
    return std::move(loaded.environment);
}

void create_source_preference_entry(const std::string& package_name) {
    require_valid_package_name(package_name);
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference_process_environment();
    const fs::path entry_path = paths.directory / package_name;
    xdg_directory_safety::PreparedDirectory directory =
        prepare_source_preference_directory(paths);
    {
        [[maybe_unused]] SourcePreferenceDescriptor directory_lock =
            lock_source_preference_directory(directory, LOCK_SH);
        const std::optional<struct stat> observed_status =
            inspect_source_preference_entry(
                directory, package_name, entry_path, false);
        if(observed_status.has_value()) {
            static_cast<void>(open_source_preference_entry(
                directory, package_name, entry_path,
                observed_status.value(), false));
            return;
        }
    }

    replace_source_preference_entry_atomically(
        directory, package_name, entry_path, std::nullopt,
        [](int) {}, []() {});
}

PreparedSourcePreferenceContents::PreparedSourcePreferenceContents(std::string contents)
    : contents_(std::move(contents)) {
}

const std::string& PreparedSourcePreferenceContents::serialized_contents() const noexcept {
    return contents_;
}

PreparedSourcePreferenceContents prepare_source_preference_contents(const SourceBuildEnvironment& environment) {
    return PreparedSourcePreferenceContents(serialize_source_preference_environment(environment));
}

void create_source_preference_from_environment_if_absent(
    const std::string& package_name, const PreparedSourcePreferenceContents& prepared_contents) {
    require_valid_package_name(package_name);
    const std::string& contents = prepared_contents.serialized_contents();
    const auto paths = xdg_paths::resolve_source_preference_process_environment();
    const fs::path entry_path = paths.directory / package_name;
    auto directory = prepare_source_preference_directory(paths);
    replace_source_preference_entry_atomically(
        directory, package_name, entry_path, std::nullopt,
        [&contents, &entry_path](int descriptor) {
            write_all(descriptor, contents.data(), contents.size(), entry_path);
        },
        []() {});
}

void append_source_preference_assignment(
    const std::string& package_name,
    const std::string& serialized_assignment) {
    require_valid_package_name(package_name);
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference_process_environment();
    const fs::path entry_path = paths.directory / package_name;
    xdg_directory_safety::PreparedDirectory directory =
        prepare_source_preference_directory(paths);

    std::string contents;
    std::optional<SourcePreferenceEntryIdentity> expected_identity;
    {
        [[maybe_unused]] SourcePreferenceDescriptor directory_lock =
            lock_source_preference_directory(directory, LOCK_SH);
        const std::optional<struct stat> observed_status =
            inspect_source_preference_entry(
                directory, package_name, entry_path, false);
        if(observed_status.has_value()) {
            contents = read_source_preference_contents(
                directory, package_name, entry_path,
                observed_status.value(), false);
            expected_identity = source_preference_entry_identity(
                observed_status.value());
        }
    }
    contents += serialized_assignment;
    contents.push_back('\n');
    replace_source_preference_entry_atomically(
        directory, package_name, entry_path, expected_identity,
        [&contents, &entry_path](int descriptor) {
            write_all(
                descriptor, contents.data(), contents.size(),
                entry_path);
        },
        []() {});
}

void replace_source_preference_entry_from_descriptor(
    const std::string& package_name,
    int source_descriptor,
    std::optional<SourcePreferenceEntryIdentity> expected_identity) {
    require_valid_package_name(package_name);
    struct stat source_status{};
    if(::fstat(source_descriptor, &source_status) != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::StatusUnavailable,
            source_preference_entry_path(package_name),
            current_system_error()));
    }
    if(!S_ISREG(source_status.st_mode)) {
        throw_source_preference_failure(source_preference_file_type_failure(
            source_preference_entry_path(package_name),
            file_type_from_mode(source_status.st_mode)));
    }

    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference_process_environment();
    const fs::path entry_path = paths.directory / package_name;
    xdg_directory_safety::PreparedDirectory directory =
        prepare_source_preference_directory(paths);
    replace_source_preference_entry_atomically(
        directory, package_name, entry_path, expected_identity,
        [source_descriptor, source_status, &entry_path](
            int destination_descriptor) {
            copy_descriptor_contents(
                source_descriptor, destination_descriptor,
                entry_path, source_status);
        },
        [source_descriptor, source_status, &entry_path]() {
            require_descriptor_state_unchanged(
                source_descriptor, source_status, entry_path);
        });
    require_descriptor_state_unchanged(
        source_descriptor, source_status, entry_path);
}

bool remove_source_preference_entry(const std::string& package_name) {
    require_valid_package_name(package_name);
    const xdg_paths::SourcePreferencePaths paths =
        xdg_paths::resolve_source_preference_process_environment();
    const fs::path entry_path = paths.directory / package_name;
    std::optional<xdg_directory_safety::PreparedDirectory> directory =
        open_source_preference_directory(paths);
    if(!directory.has_value()) return false;

    const int directory_descriptor =
        SourcePreferenceDirectoryAccess::descriptor(directory.value());
    SourcePreferenceDescriptor directory_sync =
        lock_source_preference_directory(directory.value(), LOCK_EX);

    const std::optional<struct stat> observed_status =
        inspect_source_preference_entry(
            directory.value(), package_name, entry_path, false);
    if(!observed_status.has_value()) return false;
    SourcePreferenceDescriptor retained_entry = open_source_preference_entry(
        directory.value(), package_name, entry_path,
        observed_status.value(), false);
    static_cast<void>(retained_entry);
    require_expected_destination(
        directory.value(), package_name, entry_path, observed_status);
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    invoke_source_preference_race_for_test(
        entry_path,
        SourcePreferenceTestRacePoint::BeforeRemoval);
#endif
    require_expected_destination(
        directory.value(), package_name, entry_path, observed_status);
    const struct stat final_expected_status =
        descriptor_status_checked(retained_entry, entry_path);
    if(!same_source_preference_state(
           observed_status.value(), final_expected_status)) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed before removal: {}",
                entry_path.string())));
    }
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    invoke_source_preference_race_for_test(
        entry_path,
        SourcePreferenceTestRacePoint::AtRemovalBoundary);
#endif

    std::string tombstone_leaf;
    bool moved_to_tombstone = false;
    for(std::size_t attempt = 0; attempt < 128; ++attempt) {
        tombstone_leaf = atomic_temp_leaf(package_name);
        if(renameat2_checked(
               directory_descriptor, package_name.c_str(),
               directory_descriptor, tombstone_leaf.c_str(),
               RENAME_NOREPLACE) == 0) {
            moved_to_tombstone = true;
            break;
        }
        const int rename_error = errno;
        if(rename_error == EEXIST || rename_error == ENOTEMPTY) continue;
        if(rename_error == ENOENT || rename_error == ENOTDIR ||
           rename_error == ELOOP) {
            throw_source_preference_failure(source_preference_plain_failure(
                SourcePreferenceFailureKind::ConcurrentReplacement,
                entry_path,
                localization::format_translated_message(
                    "Source preference entry changed before removal: {}",
                    entry_path.string())));
        }
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::RenameFailed,
            entry_path,
            std::error_code(rename_error, std::generic_category())));
    }
    if(!moved_to_tombstone) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::RenameFailed,
            entry_path,
            std::make_error_code(std::errc::file_exists)));
    }

    struct stat displaced_status{};
    int displaced_status_result = -1;
    do {
        displaced_status_result = ::fstatat(
            directory_descriptor, tombstone_leaf.c_str(),
            &displaced_status, AT_SYMLINK_NOFOLLOW);
    } while(displaced_status_result != 0 && errno == EINTR);
    const int displaced_status_error = errno;
    const struct stat displaced_descriptor_status =
        descriptor_status_checked(retained_entry, entry_path);
    const bool tombstone_matches_retained =
        displaced_status_result == 0 &&
        same_source_preference_state(
            displaced_descriptor_status, displaced_status);
    const bool removal_matches =
        tombstone_matches_retained &&
        same_relocated_source_preference_state(
            final_expected_status,
            displaced_descriptor_status);
    if(!removal_matches) {
        // A mismatched tombstone is not ours to restore or remove. Keeping the
        // artifact is safer than mutating an identity we cannot prove.
        if(displaced_status_result != 0 &&
           displaced_status_error != ENOENT) {
            throw_source_preference_failure(source_preference_system_failure(
                SourcePreferenceFailureKind::StatusUnavailable,
                entry_path,
                std::error_code(
                    displaced_status_error,
                    std::generic_category())));
        }
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed before removal: {}",
                entry_path.string())));
    }

    if(!remove_internal_entry_if_descriptor_matches(
           directory_descriptor, tombstone_leaf,
           retained_entry, entry_path,
           displaced_descriptor_status)) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed before removal: {}",
                entry_path.string())));
    }
    tombstone_leaf.clear();
#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
    invoke_source_preference_race_for_test(
        entry_path,
        SourcePreferenceTestRacePoint::AfterRemoval);
#endif
    const std::optional<struct stat> post_removal_status =
        inspect_source_preference_entry(
            directory.value(), package_name, entry_path, false);
    if(post_removal_status.has_value()) {
        throw_source_preference_failure(source_preference_plain_failure(
            SourcePreferenceFailureKind::ConcurrentReplacement,
            entry_path,
            localization::format_translated_message(
                "Source preference entry changed before removal: {}",
                entry_path.string())));
    }
    if(::fsync(directory_sync.get()) != 0) {
        throw_source_preference_failure(source_preference_system_failure(
            SourcePreferenceFailureKind::SyncFailed,
            directory->path(), current_system_error()));
    }
    close_descriptor_checked(directory_sync, directory->path());
    directory->require_unchanged_identity();
    return true;
}

#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
void fail_next_source_preference_operation_for_test(
    const std::string& package_name,
    SourcePreferenceTestFailurePoint failure_point) {
    g_source_preference_injected_failure = SourcePreferenceInjectedFailure{
        .entry_path = source_preference_entry_path(package_name),
        .failure_point = failure_point,
    };
}

void run_source_preference_race_once_for_test(
    const std::string& package_name,
    SourcePreferenceTestRacePoint race_point,
    SourcePreferenceTestRaceHandler handler) {
    g_source_preference_injected_race = SourcePreferenceInjectedRace{
        .entry_path = source_preference_entry_path(package_name),
        .race_point = race_point,
        .handler = handler,
    };
}

void reset_source_preference_test_hooks() {
    g_source_preference_injected_failure.reset();
    g_source_preference_injected_race.reset();
}
#endif
