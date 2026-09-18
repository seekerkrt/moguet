#pragma once

#include "xdg_paths.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace xdg_state_log {
struct StateLogDirectoryAccess;
}

struct TrustedCacheDirectoryAccess;
struct SourcePreferenceDirectoryAccess;
struct XdgGenerationStoreDirectoryAccess;

namespace xdg_directory_safety {

enum class PreparationStage {
    BoundaryValidation,
    FilesystemRootOpen,
    AnchorTraversal,
    AnchorValidation,
    ComponentInspection,
    ComponentCreation,
    ComponentOpen,
    ComponentValidation,
    DirectoryRevalidation,
};

enum class PreparationErrorCode {
    MissingAnchor,
    Symlink,
    NotDirectory,
    OwnershipMismatch,
    UnsafePermissions,
    PermissionDenied,
    CreationFailed,
    MetadataFailure,
    ConcurrentReplacement,
    InvalidCreationBoundary,
};

struct PreparationFailure {
    xdg_paths::DirectoryKind directory_kind =
        xdg_paths::DirectoryKind::Config;
    PreparationStage stage = PreparationStage::BoundaryValidation;
    PreparationErrorCode code =
        PreparationErrorCode::InvalidCreationBoundary;
    std::optional<std::error_code> system_error;
    std::optional<std::size_t> component_index;
};

class PreparationError final : public std::runtime_error {
    PreparationFailure failure_;

public:
    explicit PreparationError(PreparationFailure failure);

    const PreparationFailure& failure() const noexcept {
        return failure_;
    }
};

struct DirectoryIdentity {
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
};

// A throwing callback can reject a missing managed component before mkdirat.
// The supplied identity belongs to the descriptor-retained current parent;
// its named lineage is revalidated before and after the callback.
using DirectoryCreationPrecondition =
    std::function<void(const DirectoryIdentity&)>;

// Validation後のapplication directoryと、filesystem rootから各named linkを
// 再証明するprivate descriptor lineageを一体で保持する。作成済みcomponentは
// failure時にもrollbackせず、安全なpartial stateとして残す。
class PreparedDirectory final {
    struct RetainedDirectoryIdentity {
        int descriptor = -1;
        std::string leaf_name;
        std::uintmax_t device = 0;
        std::uintmax_t inode = 0;
        std::uintmax_t filesystem_owner = 0;
        bool requires_security_validation = false;
        bool requires_private_mode = false;
    };

    xdg_paths::DirectoryKind directory_kind_;
    std::filesystem::path path_;
    int parent_descriptor_ = -1;
    int directory_descriptor_ = -1;
    std::string leaf_name_;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;
    std::uintmax_t filesystem_owner_ = 0;
    std::uintmax_t permissions_ = 0;
    std::size_t created_component_count_ = 0;
    std::size_t managed_component_count_ = 0;
    std::vector<RetainedDirectoryIdentity> retained_lineage_;

    PreparedDirectory(
        xdg_paths::DirectoryKind directory_kind,
        std::filesystem::path path, int parent_descriptor,
        int directory_descriptor, std::string leaf_name,
        std::uintmax_t device, std::uintmax_t inode,
        std::uintmax_t owner, std::uintmax_t filesystem_owner,
        std::uintmax_t permissions,
        std::size_t created_component_count,
        std::size_t managed_component_count,
        std::vector<RetainedDirectoryIdentity> retained_lineage) noexcept;

    friend PreparedDirectory prepare_directory(
        const xdg_paths::ConfigPaths& paths);
    friend PreparedDirectory prepare_directory(
        const xdg_paths::StatePaths& paths);
    friend PreparedDirectory prepare_directory(
        const xdg_paths::CachePaths& paths);
    friend PreparedDirectory prepare_directory(
        const xdg_paths::SourcePreferencePaths& paths);
    friend PreparedDirectory prepare_directory(
        const xdg_paths::StateStorePaths& paths);
    friend std::optional<PreparedDirectory> open_existing_directory(
        const xdg_paths::StateStorePaths& paths);

    friend struct DirectorySafetyAccess;
    friend struct xdg_state_log::StateLogDirectoryAccess;
    friend struct ::TrustedCacheDirectoryAccess;
    friend struct ::SourcePreferenceDirectoryAccess;
    friend struct ::XdgGenerationStoreDirectoryAccess;

public:
    PreparedDirectory(const PreparedDirectory&) = delete;
    PreparedDirectory& operator=(const PreparedDirectory&) = delete;
    PreparedDirectory(PreparedDirectory&& other) noexcept;
    PreparedDirectory& operator=(PreparedDirectory&&) = delete;
    ~PreparedDirectory() noexcept;

    xdg_paths::DirectoryKind directory_kind() const noexcept {
        return directory_kind_;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    std::uintmax_t owner() const noexcept {
        return owner_;
    }

    std::uintmax_t permissions() const noexcept {
        return permissions_;
    }

    std::uintmax_t device() const noexcept {
        return device_;
    }

    std::uintmax_t inode() const noexcept {
        return inode_;
    }

    std::size_t created_component_count() const noexcept {
        return created_component_count_;
    }

    void require_unchanged_identity() const;

    // Publication-only durability of the resolver-owned entries: sync their
    // containing directories, from the final parent back to the stable anchor.
    // Existing residue is included. Opens only "." relative to retained FDs;
    // never traverses diagnostic paths or syncs ancestors above the anchor.
    // Identity failures throw PreparationError; synchronization I/O errors are
    // returned. Merely preparing/reading a directory does not call this method.
    [[nodiscard]] std::optional<std::error_code> synchronize_managed_parent_entries() const;
};

#if defined(MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS) || defined(MOGUET_ENABLE_XDG_GENERATION_STORE_TEST_HOOKS)
using ManagedParentSyncTestHook = std::optional<std::error_code> (*)(int descriptor);
void set_managed_parent_sync_hook_for_test(ManagedParentSyncTestHook hook);
#endif

// Explicit XDGではbase directoryをexisting anchorとして要求し、application
// componentだけを作成する。HOME fallbackではHOMEをexisting anchorとし、
// resolverが固定したXDG suffixとapplication componentだけを作成できる。
PreparedDirectory prepare_directory(const xdg_paths::ConfigPaths& paths);
PreparedDirectory prepare_directory(const xdg_paths::StatePaths& paths);
PreparedDirectory prepare_directory(
    const xdg_paths::StatePaths& paths,
    const DirectoryCreationPrecondition& creation_precondition);
PreparedDirectory prepare_directory(const xdg_paths::CachePaths& paths);
// Observation only; missing managed directories are not created.
std::optional<PreparedDirectory> open_existing_directory(const xdg_paths::CachePaths& paths);
PreparedDirectory prepare_directory(
    const xdg_paths::CachePaths& paths,
    const DirectoryCreationPrecondition& creation_precondition);

// Source preference directoryだけを対象とするcreation / read-only境界。
// open_existing_directory()のnulloptはmanaged componentのmissingだけを表す。
PreparedDirectory prepare_directory(
    const xdg_paths::SourcePreferencePaths& paths);
std::optional<PreparedDirectory> open_existing_directory(
    const xdg_paths::SourcePreferencePaths& paths);

// Resolver-owned XDG state-store directory. Lookup does not create it;
// missing is represented only by nullopt.
PreparedDirectory prepare_directory(
    const xdg_paths::StateStorePaths& paths);
std::optional<PreparedDirectory> open_existing_directory(
    const xdg_paths::StateStorePaths& paths);

#ifdef MOGUET_TEST_XDG_DIRECTORY_SAFETY_HOOKS
enum class DirectorySafetyTestEvent {
    AfterAnchorMetadata,
    AfterManagedMetadata,
    AfterComponentCreation,
    BeforeAbsentLineageRevalidation,
};

enum class DirectorySafetyTestFailurePoint {
    AnchorMetadata,
    ManagedMetadata,
    ComponentCreation,
    ComponentOpen,
    DescriptorMetadata,
};

struct DirectorySafetyInjectedFailure {
    DirectorySafetyTestFailurePoint failure_point;
    std::size_t component_index;
    int error_number;
};

using DirectorySafetyTestEventHook = std::function<void(
    DirectorySafetyTestEvent, xdg_paths::DirectoryKind, std::size_t,
    const std::filesystem::path&)>;

// Focused test binaryだけがeffective UID / observed owner / syscall timingを
// 差し替える。production path overrideやunsafe bypassは提供しない。
struct DirectorySafetyTestOverrides {
    std::optional<std::uintmax_t> effective_user;
    std::optional<std::uintmax_t> observed_owner;
    std::optional<DirectorySafetyInjectedFailure> injected_failure;
    DirectorySafetyTestEventHook event_hook;
};

PreparedDirectory prepare_directory_for_test(
    const xdg_paths::ConfigPaths& paths,
    const DirectorySafetyTestOverrides& overrides);
PreparedDirectory prepare_directory_for_test(
    const xdg_paths::StatePaths& paths,
    const DirectorySafetyTestOverrides& overrides);
PreparedDirectory prepare_directory_for_test(
    const xdg_paths::CachePaths& paths,
    const DirectorySafetyTestOverrides& overrides);
PreparedDirectory prepare_directory_for_test(
    const xdg_paths::SourcePreferencePaths& paths,
    const DirectorySafetyTestOverrides& overrides);
std::optional<PreparedDirectory> open_existing_directory_for_test(
    const xdg_paths::SourcePreferencePaths& paths,
    const DirectorySafetyTestOverrides& overrides);
PreparedDirectory prepare_directory_for_test(
    const xdg_paths::StateStorePaths& paths,
    const DirectorySafetyTestOverrides& overrides);
std::optional<PreparedDirectory> open_existing_directory_for_test(
    const xdg_paths::StateStorePaths& paths,
    const DirectorySafetyTestOverrides& overrides);
#endif

} // namespace xdg_directory_safety
