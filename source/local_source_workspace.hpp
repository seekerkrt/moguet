#pragma once

#include "local_source_root.hpp"
#include "trusted_cache.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <system_error>

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
#include <functional>
#endif

enum class LocalSourceWorkspaceStage {
    BoundaryValidation,
    NameGeneration,
    WorkspaceCreation,
    SourceEnumeration,
    SourceInspection,
    SourceOpen,
    SourceRead,
    DestinationCreation,
    DestinationWrite,
    MetadataPreservation,
    SourceRevalidation,
    WorkspaceRevalidation,
    Cleanup
};

enum class LocalSourceWorkspaceErrorCode {
    CacheInsideSource,
    RandomnessUnavailable,
    NameCollision,
    UnsafeName,
    UnsupportedFileType,
    OwnershipMismatch,
    UnsafePermissions,
    FilesystemBoundary,
    SymlinkEscape,
    ConcurrentMutation,
    ContentChanged,
    PermissionDenied,
    MetadataFailure,
    ReadFailure,
    WriteFailure,
    InvalidState,
    CleanupFailure
};

struct LocalSourceWorkspaceFailure {
    LocalSourceWorkspaceStage stage =
        LocalSourceWorkspaceStage::SourceInspection;
    LocalSourceWorkspaceErrorCode code =
        LocalSourceWorkspaceErrorCode::MetadataFailure;
    std::filesystem::path relative_path;
    std::optional<std::error_code> system_error;

    bool operator==(const LocalSourceWorkspaceFailure&) const = default;
};

// Presentation is owned by the CLI composition boundary. This exception
// carries typed control-flow data and a stable internal what() token only.
class LocalSourceWorkspaceError final : public std::runtime_error {
    LocalSourceWorkspaceFailure failure_;

public:
    explicit LocalSourceWorkspaceError(LocalSourceWorkspaceFailure failure);

    const LocalSourceWorkspaceFailure& failure() const noexcept {
        return failure_;
    }
};

struct LocalSourceBuildAccess;

class LocalSourceWorkspace final {
    enum class State {
        Active,
        CleanupAttempted,
        Cleaned,
        MovedFrom
    };

    RetainedTrustedCacheDirectory directory_;
    State state_ = State::Active;

    explicit LocalSourceWorkspace(
        RetainedTrustedCacheDirectory directory) noexcept;

    friend LocalSourceWorkspace materialize_local_source_workspace(
        const LocalSourceRoot& source_root,
        const ValidatedCacheRoot& cache_root);
    friend struct LocalSourceBuildAccess;
    friend LocalSourceWorkspace create_aur_patch_workspace(const ValidatedCacheRoot& cache_root);

public:
    LocalSourceWorkspace(const LocalSourceWorkspace&) = delete;
    LocalSourceWorkspace& operator=(const LocalSourceWorkspace&) = delete;
    LocalSourceWorkspace(LocalSourceWorkspace&& other) noexcept;
    LocalSourceWorkspace& operator=(LocalSourceWorkspace&&) = delete;
    ~LocalSourceWorkspace() noexcept;

    const std::filesystem::path& path() const noexcept;

    void require_unchanged_identity() const;
    // A failed attempt is terminal; it neither becomes success nor retries.
    void cleanup();
};

LocalSourceWorkspace materialize_local_source_workspace(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root);

// Empty invocation-owned parent for a fresh AUR recipe checkout. Uses the same
// retained-directory cleanup authority as local recipe candidates.
LocalSourceWorkspace create_aur_patch_workspace(const ValidatedCacheRoot& cache_root);

// workspace作成前にretained identityとsource tree全体をmutation-freeで検査する。
// materializationもrace対策として実行直前に同じ境界を再確認する。
void require_local_source_cache_separation(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root);

// XDG managed-directory creation callbacks use this retained-identity check
// before mkdir. It rejects the source root itself and any directory reachable
// inside the source tree, including a path alias with the same device/inode.
void require_directory_identity_outside_local_source_tree(
    const LocalSourceRoot& source_root,
    std::uintmax_t directory_device,
    std::uintmax_t directory_inode);

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
enum class LocalSourceWorkspaceTestEvent {
    AfterFileDataCopied,
    BeforeDirectoryRevalidation,
    BeforeCleanupRemoval
};

using LocalSourceWorkspaceTestHook = std::function<void(
    LocalSourceWorkspaceTestEvent event,
    const std::filesystem::path& relative_path)>;

// Simulates an inode-reuse ABA where the replacement presents the plan's
// original type/device/inode metadata. The opaque filesystem handle remains
// outside this override and must still reject the replacement.
using LocalSourceWorkspaceCleanupMetadataMatchForTest =
    std::function<bool(const std::filesystem::path& relative_path)>;

void set_local_source_workspace_test_hook(
    LocalSourceWorkspaceTestHook hook);

void set_local_source_workspace_cleanup_metadata_match_for_test(
    LocalSourceWorkspaceCleanupMetadataMatchForTest match);

void require_cache_identity_outside_source_tree_for_test(
    const LocalSourceRoot& source_root,
    std::uintmax_t cache_device, std::uintmax_t cache_inode);
#endif
