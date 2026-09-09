#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

inline constexpr std::size_t INSTALLED_RECORD_MAXIMUM_METADATA_BYTES = 4U * 1024U * 1024U;
inline constexpr std::size_t INSTALLED_RECORD_MAXIMUM_MTREE_BYTES = 64U * 1024U * 1024U;
inline constexpr std::size_t INSTALLED_RECORD_MAXIMUM_HANDLE_BYTES = 128;

enum class InstalledRecordObservationIssue {
    UnsupportedDatabaseWorld,
    DatabaseWorldMismatch,
    DatabaseLoadFailure,
    MissingPackage,
    MalformedMetadata,
    MetadataMismatch,
    UnsafeRecord,
    RecordChanged,
    ReadFailure,
    MetadataTooLarge,
    UnsupportedGeneration,
    GenerationMismatch,
    MtreeMismatch,
    RecordDigestMismatch,
    MissingBaseline,
    MissingAnchor,
    OperationMismatch,
    ResourceFailure,
};

// These are raw observation values, not mintable binding/proof capabilities.
// The root helper seals the resolved world and both observation phases in its
// private transaction state; the live observer accepts only that sealed route.
struct InstalledDatabaseWorld {
    std::string root_directory;
    std::string database_path;
    std::string descriptor_identity;
    bool operator==(const InstalledDatabaseWorld&) const = default;
};
using InstalledDatabaseWorldResult = std::variant<InstalledDatabaseWorld, InstalledRecordObservationIssue>;

struct InstalledPackageRecordSnapshot {
    std::string package_name;
    std::string package_base;
    std::string full_version;
    std::string architecture;
    std::string record_generation;
    std::string raw_mtree_sha256;
    std::string raw_database_sha256;
    std::string descriptor_identity;
    bool operator==(const InstalledPackageRecordSnapshot&) const = default;
};

struct InstalledPackageRecordAbsent {
    bool operator==(const InstalledPackageRecordAbsent&) const = default;
};
using InstalledPackageRecordObservation = std::variant<InstalledPackageRecordSnapshot,
                                                       InstalledPackageRecordAbsent, InstalledRecordObservationIssue>;

// Fixed /usr/bin/pacman-conf, fixed /etc/pacman.conf, no caller-selected
// RootDir/configuration/executable. Unsupported worlds remain proof failures.
[[nodiscard]] InstalledDatabaseWorldResult resolve_trusted_installed_database_world() noexcept;

// Always owns a newly opened ALPM handle and retained DB/record/child FDs in
// this call. Never mints an InstalledArtifactBinding from these raw parameters.
[[nodiscard]] InstalledPackageRecordObservation observe_installed_package_record(
    const InstalledDatabaseWorld& world, const std::string& package_name) noexcept;

[[nodiscard]] std::string serialize_installed_database_world(const InstalledDatabaseWorldResult& world);
[[nodiscard]] InstalledDatabaseWorldResult parse_installed_database_world(std::string_view protocol) noexcept;
[[nodiscard]] std::string serialize_installed_package_record_observation(const InstalledPackageRecordObservation& observation);
[[nodiscard]] InstalledPackageRecordObservation parse_installed_package_record_observation(std::string_view protocol) noexcept;

#ifdef MOGUET_ENABLE_INSTALLED_RECORD_OBSERVATION_TEST_HOOKS
#include <functional>
#include <sys/statfs.h>
#include <sys/types.h>

struct file_handle;
enum class InstalledRecordObservationTestEvent {
    BeforeSessionOpen,
    AfterSessionOpen,
    AfterRecordSelection,
    AfterRecordOpen,
    BeforeRawRead,
    AfterRawRead,
    BeforeFinalReproof,
    AfterRawDescAdmission,
    BeforeSemanticLoad,
    AfterSemanticLoad,
};
struct InstalledRecordObservationTestHooks {
    std::optional<std::string> database_path;
    uid_t expected_owner = 0;
    std::function<void(InstalledRecordObservationTestEvent)> event;
    std::function<void(std::string_view, int)> child_opened;
    std::function<int(int, const char*, struct file_handle*, int*, int)> name_to_handle;
    std::function<int(int, struct statfs*)> statfs;
    std::function<ssize_t(int, void*, std::size_t, off_t)> pread;
    bool fail_database_load = false;
    // Isolate required-child identity from directory content/stat drift.
    bool ignore_directory_content_changes = false;
};
void set_installed_record_observation_test_hooks(InstalledRecordObservationTestHooks hooks);
[[nodiscard]] std::variant<std::string, InstalledRecordObservationIssue>
observe_installed_record_generation_for_test(int retained_record_fd) noexcept;
#endif
