#pragma once

#include "xdg_paths.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

// Mechanical immutable-generation/CAS storage for one resolver-owned XDG
// state namespace. This layer owns filesystem proof and raw-byte publication;
// it does not parse a semantic document or derive a PackageBase identity.

inline constexpr std::size_t xdg_generation_store_default_max_record_bytes =
    65536;

struct XdgGenerationRecordIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
    std::uintmax_t link_count = 0;
    std::intmax_t size = 0;
    std::intmax_t modification_time_seconds = 0;
    std::intmax_t modification_time_nanoseconds = 0;
    std::intmax_t status_change_time_seconds = 0;
    std::intmax_t status_change_time_nanoseconds = 0;

    bool operator==(const XdgGenerationRecordIdentity&) const = default;
};

// Raw observed token used by exact-predecessor CAS. Filesystem metadata stays
// runtime evidence; semantic stores choose separately what belongs in their
// persistent document.
struct XdgGenerationObservedRecord {
    std::uint64_t generation = 0;
    std::string leaf_name;
    XdgGenerationRecordIdentity identity;
    std::string raw_contents;

    bool operator==(const XdgGenerationObservedRecord&) const = default;
};

using XdgGenerationFutureDocumentPredicate = bool (*)(std::string_view);

struct XdgGenerationStoreConfiguration {
    xdg_paths::StateStorePaths paths;
    std::string unit_leaf;
    std::string temporary_leaf_prefix;
    std::size_t max_record_bytes =
        xdg_generation_store_default_max_record_bytes;
    // Schema ownership remains with the semantic codec. The low-level chain
    // asks only whether an observed raw document belongs to a future writer so
    // it can preserve the existing no-downgrade/unsafe-history contract.
    XdgGenerationFutureDocumentPredicate is_future_document = nullptr;
};

struct XdgGenerationStoreMissing {
    bool operator==(const XdgGenerationStoreMissing&) const = default;
};

struct XdgGenerationStoreLoaded {
    XdgGenerationObservedRecord observed;

    bool operator==(const XdgGenerationStoreLoaded&) const = default;
};

enum class XdgGenerationStoreFailureKind {
    AuthorityUnavailable,
    DirectoryPreparationFailed,
    DirectoryUnavailable,
    UnsupportedFileType,
    OwnershipMismatch,
    UnsafePermissions,
    MultipleHardLinks,
    OpenFailed,
    LockFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    CloseFailed,
    ConcurrentReplacement,
    FutureSchemaOverwriteRefused,
    RecordTooLarge,
    ResourceFailure,
    InternalFailure,
};

struct XdgGenerationStoreFailure {
    XdgGenerationStoreFailureKind kind;
    std::filesystem::path entry_path;
    std::optional<std::error_code> system_error;
    std::optional<std::filesystem::file_type> observed_file_type;
    std::optional<std::filesystem::path> leftover_artifact;

    bool operator==(const XdgGenerationStoreFailure&) const = default;
};

enum class XdgGenerationPostPublicationIssue {
    DirectorySyncUncertain,
    PublishedIdentityUncertain,
    DirectoryCloseFailed,
    LineageRevalidationFailed,
    PredecessorObservationUncertain,
    UnitDirectoryIdentityUncertain,
    AuthoritativeHistoryUncertain,
    ResourceFailure,
    InternalFailure,
};

struct XdgGenerationStorePublished {
    XdgGenerationObservedRecord observed;

    bool operator==(const XdgGenerationStorePublished&) const = default;
};

struct XdgGenerationStorePublishedUncertain {
    std::optional<XdgGenerationObservedRecord> observed;
    XdgGenerationPostPublicationIssue issue;
    XdgGenerationStoreFailureKind failure_kind;
    std::filesystem::path entry_path;
    std::optional<std::filesystem::path> leftover_artifact;
    std::optional<std::error_code> system_error;

    bool operator==(const XdgGenerationStorePublishedUncertain&) const =
        default;
};

enum class XdgGenerationStoreHistoryIssue {
    ForkDetected,
    OrphanSuccessor,
    ChainGap,
    MissingOriginWithArtifacts,
    UnrecognizedManagedEntry,
    FutureOwnedArtifact,
    HigherGenerationUnreachable,
};

struct XdgGenerationStoreUnsafeHistory {
    XdgGenerationStoreHistoryIssue issue;
    std::filesystem::path unit_path;
    std::vector<std::string> observed_leaves;
    std::optional<std::uint64_t> matching_generation;
    std::optional<std::uint64_t> highest_generation;

    bool operator==(const XdgGenerationStoreUnsafeHistory&) const = default;
};

using XdgGenerationStoreReadResult = std::variant<
    XdgGenerationStoreMissing,
    XdgGenerationStoreLoaded,
    XdgGenerationStoreUnsafeHistory,
    XdgGenerationStoreFailure>;

using XdgGenerationStorePublishResult = std::variant<
    XdgGenerationStorePublished,
    XdgGenerationStorePublishedUncertain,
    XdgGenerationStoreUnsafeHistory,
    XdgGenerationStoreFailure>;

[[nodiscard]] std::filesystem::path xdg_generation_store_entry_path(
    const XdgGenerationStoreConfiguration& configuration);

[[nodiscard]] std::string xdg_generation_store_origin_leaf();

[[nodiscard]] std::string xdg_generation_store_successor_leaf(
    std::uint64_t next_generation,
    const XdgGenerationRecordIdentity& predecessor,
    std::string_view predecessor_raw_contents);

[[nodiscard]] std::string xdg_generation_store_raw_contents_sha256(
    std::string_view raw_contents);

// Streams exactly expected_size bytes from one already-open descriptor with
// the same SHA-256 implementation used by generation records. The caller
// owns descriptor identity/replacement checks around this bounded read.
[[nodiscard]] std::string xdg_generation_store_file_descriptor_sha256(
    int descriptor,
    std::uintmax_t expected_size,
    std::uintmax_t maximum_size);

// Read-no-create. Missing is returned only for an absent managed namespace,
// absent unit directory, or a unit directory containing temp residue only.
[[nodiscard]] XdgGenerationStoreReadResult read_xdg_generation_store(
    const XdgGenerationStoreConfiguration& configuration);

// Publish raw current-schema bytes. expected_observed is the exact predecessor
// token from a prior read; nullopt means the caller observed Missing. Failed
// CAS is not retried. Every result after the no-replace link commit point is
// PublishedUncertain rather than an ordinary definite failure.
// Handled resource failures preserve that phase even when diagnostics cannot
// be allocated. Such an emergency result may have an empty entry_path and no
// observed record; it never authorizes rollback, retry, or adoption.
[[nodiscard]] XdgGenerationStorePublishResult publish_xdg_generation_store(
    const XdgGenerationStoreConfiguration& configuration,
    std::string_view publication,
    const std::optional<XdgGenerationObservedRecord>& expected_observed);

#if defined(MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_REVIEWED_SOURCE_STATE_STORE_TEST_HOOKS)
enum class XdgGenerationStoreTestFailurePoint {
    Status,
    RecordOwnership,
    Open,
    Read,
    Write,
    Sync,
    DirectorySync,
    Rename,
    Close,
    Lock,
    PostCommitVerify,
    PostCommitPredecessorStatus,
    PostCommitPredecessorRead,
};

enum class XdgGenerationStoreTestRacePoint {
    BeforePublication,
    AtPublicationBoundary,
    AfterAuthorityProof,
    AfterPublication,
    BeforePostCommitReproof,
    AfterVerifiedPublication,
    BeforeCleanup,
    AfterReadAuthorityProof,
    AfterRecordContentsRead,
};

struct XdgGenerationStoreTestRaceContext {
    std::filesystem::path unit_directory;
    std::filesystem::path publication_path;
    std::string publication_leaf;
    std::optional<std::filesystem::path> current_path;
    std::string temporary_leaf;
    std::uint64_t next_generation = 0;
    std::uintmax_t source_device = 0;
    std::uintmax_t source_inode = 0;
    std::optional<std::filesystem::path> record_path = std::nullopt;
};

using XdgGenerationStoreTestRaceHandler = void (*)(
    const XdgGenerationStoreTestRaceContext& context);

void fail_next_xdg_generation_store_operation_for_test(
    XdgGenerationStoreTestFailurePoint failure_point);
void run_xdg_generation_store_race_once_for_test(
    XdgGenerationStoreTestRacePoint race_point,
    XdgGenerationStoreTestRaceHandler handler);
void simulate_coarse_xdg_generation_store_timestamps_for_test();
void reset_xdg_generation_store_test_hooks();
#endif
