#pragma once

#include "source_environment.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

// source preferenceのread時messageはconsumerが表示し、moduleはLoggerへ依存しない。
using SourcePreferenceLoadHandler = void (*)(const std::filesystem::path& path);
using SourcePreferenceWarningHandler = void (*)(const std::string& warning);

// Rootはprocess environmentのconfig authorityだけから毎回pureに解決する。
std::filesystem::path source_preference_root();
std::filesystem::path source_preference_entry_path(const std::string& package_name);

// system/source upgrade preparationが最初のdirectory iteration順をowned化する。
// LOCK_SHを全validation期間保持し、不正entryが1件でもあれば失敗する。
struct SourcePreferenceEntrySnapshot {
    std::size_t original_index = 0;
    std::filesystem::path entry_path;
    std::string package_name;
    bool is_regular_file = false;
};

struct SourcePreferenceDirectorySnapshot {
    bool root_exists = false;
    std::vector<SourcePreferenceEntrySnapshot> entries;
};

SourcePreferenceDirectorySnapshot snapshot_source_preference_directory();

// list-srcはLOCK_SH中に全entryの検証とreadを完了してから初めて出力する。
struct SourcePreferenceListEntrySnapshot {
    std::filesystem::path entry_path;
    std::string package_name;
    std::vector<std::string> display_lines;
};

struct SourcePreferenceListSnapshot {
    bool root_exists = false;
    std::vector<SourcePreferenceListEntrySnapshot> entries;
};

SourcePreferenceListSnapshot snapshot_source_preferences_for_listing();

struct SourcePreferenceAbsent {
    bool operator==(const SourcePreferenceAbsent&) const = default;
};

struct SourcePreferenceEntryIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
    std::intmax_t size = 0;
    std::intmax_t modification_time_seconds = 0;
    std::intmax_t modification_time_nanoseconds = 0;
    std::intmax_t status_change_time_seconds = 0;
    std::intmax_t status_change_time_nanoseconds = 0;

    bool operator==(const SourcePreferenceEntryIdentity&) const = default;
};

struct SourcePreferenceLoaded {
    std::filesystem::path entry_path;
    SourceBuildEnvironment environment;
    std::vector<std::string> warnings;
    std::string raw_contents;
    std::optional<SourcePreferenceEntryIdentity> identity;
};

enum class SourcePreferenceFailureKind {
    AuthorityUnavailable,
    DirectoryEnumerationFailed,
    InvalidEntryName,
    StatusUnavailable,
    UnsupportedFileType,
    OwnershipMismatch,
    UnsafePermissions,
    OpenFailed,
    LockFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    RemoveFailed,
    ConcurrentReplacement,
    CloseFailed,
};

// syscall failureはsystem_error、unsupported typeはobserved_file_typeを正本にする。
struct SourcePreferenceFailure {
    SourcePreferenceFailureKind kind;
    std::filesystem::path entry_path;
    std::optional<std::error_code> system_error;
    std::optional<std::filesystem::file_type> observed_file_type;
    // diagnosticは表示用の補助情報であり、control flowはkindとtyped fieldで行う。
    std::string diagnostic;
};

using StrictSourcePreferenceResult = std::variant<
    SourcePreferenceAbsent,
    SourcePreferenceLoaded,
    SourcePreferenceFailure>;

class SourcePreferenceError final : public std::runtime_error {
    SourcePreferenceFailure failure_;

public:
    explicit SourcePreferenceError(SourcePreferenceFailure failure);

    const SourcePreferenceFailure& failure() const noexcept {
        return failure_;
    }
};

// Absentはentryが実在しない場合だけ。LOCK_SH中にregular fileを全byte readし、
// validation後のowned resultだけを返す。
StrictSourcePreferenceResult read_source_preference_strict(
    const std::string& package_name);

SourceBuildEnvironment get_package_env(
    const std::string& package_name, SourcePreferenceLoadHandler on_load,
    SourcePreferenceWarningHandler on_warning);

// add/editと明示promotionだけがdirectoryをprepareする。writerはstoreのLOCK_EXで直列化し、
// identity不明のpublication artifactは変更せずtyped errorにする。
// 既存entryはexact 0600でなければ失敗する。
void create_source_preference_entry(const std::string& package_name);
// Canonical parserでenvironment全体のexact round-tripを証明したowned bytes。
// Preflightからpublicationまで再serializeせず、検証した同一内容を保持する。
class PreparedSourcePreferenceContents {
    std::string contents_;
    explicit PreparedSourcePreferenceContents(std::string contents);
    friend PreparedSourcePreferenceContents prepare_source_preference_contents(const SourceBuildEnvironment& environment);

public:
    const std::string& serialized_contents() const noexcept;
};

// Pure preflight: current line format/parserでexactに再利用できない環境は拒否する。
// CLI one-off parserやsaved consumerのempty Omit契約は変更しない。
PreparedSourcePreferenceContents prepare_source_preference_contents(const SourceBuildEnvironment& environment);
// ordered assignments全体を一度に公開する。既存entryを変更せず、publicationもabsentを要求する。
void create_source_preference_from_environment_if_absent(
    const std::string& package_name, const PreparedSourcePreferenceContents& contents);
void append_source_preference_assignment(
    const std::string& package_name,
    const std::string& serialized_assignment);
void replace_source_preference_entry_from_descriptor(
    const std::string& package_name, int source_descriptor,
    std::optional<SourcePreferenceEntryIdentity> expected_identity);

// store/entry missingはfalse。それ以外のvalidation/IO failureは例外にする。
bool remove_source_preference_entry(const std::string& package_name);

#ifdef MOGUET_ENABLE_SOURCE_PREFERENCE_TEST_HOOKS
enum class SourcePreferenceTestFailurePoint {
    Status,
    Open,
    Read,
    Write,
    Sync,
    Publication,
};

enum class SourcePreferenceTestRacePoint {
    AfterReadOpen,
    BeforePublication,
    AtPublicationBoundary,
    AfterPublication,
    BeforeRemoval,
    AtRemovalBoundary,
    AfterRemoval,
};

using SourcePreferenceTestRaceHandler = void (*)(
    const std::filesystem::path& entry_path);

// 一回限りのreader/writer failureを注入し、production IO APIは差し替えない。
void fail_next_source_preference_operation_for_test(
    const std::string& package_name,
    SourcePreferenceTestFailurePoint failure_point);
void run_source_preference_race_once_for_test(
    const std::string& package_name,
    SourcePreferenceTestRacePoint race_point,
    SourcePreferenceTestRaceHandler handler);
void reset_source_preference_test_hooks();
#endif
