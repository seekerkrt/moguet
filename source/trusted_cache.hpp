#pragma once

#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
#include <functional>
#endif

class ArtifactWorkspace;
class ArtifactMakepkgContext;
class LocalSourceRoot;
class LocalSourceWorkspace;
class PreparedCacheCleanup;
class ValidatedCacheRoot;
class ValidatedPrivateCacheRoot;
class ReviewedSourcePackageBaseLease;
struct PersistentCheckoutDirectoryAccess;
struct LocalSourceWorkspaceCleanupAccess;
struct TrustedCacheAccess;

LocalSourceWorkspace materialize_local_source_workspace(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root);

enum class TrustedCacheStage {
    RootAdoption,
    RootRevalidation,
    ChildValidation,
    ChildCreation,
    ChildOpen,
    CleanupPreflight,
    RecursiveRemoval,
    Rollback,
};

enum class TrustedCacheErrorCode {
    InvalidBoundary,
    Symlink,
    NotDirectory,
    NotRegularFile,
    OwnershipMismatch,
    UnsafePermissions,
    PermissionDenied,
    MetadataFailure,
    ConcurrentReplacement,
    ChildEscape,
    CreationFailure,
    CleanupPreflightFailure,
    RemovalFailure,
    RollbackRefusal,
};

struct TrustedCacheFailure {
    TrustedCacheStage stage = TrustedCacheStage::RootAdoption;
    TrustedCacheErrorCode code = TrustedCacheErrorCode::InvalidBoundary;
    std::optional<std::error_code> system_error;
};

class TrustedCacheError final : public std::runtime_error {
    TrustedCacheFailure failure_;

public:
    explicit TrustedCacheError(TrustedCacheFailure failure);

    const TrustedCacheFailure& failure() const noexcept {
        return failure_;
    }
};

// trusted cache root / entry の検証条件。
enum class CachePathRequirement {
    Existing,
    ExistingDirectory,
    Missing,
    ExistingOrMissing,
};

// PreparedDirectoryからadoptしたroot identityとretained descriptorを共有する
// copyable capability。logical pathは表示用であり、mutation authorityではない。
class ValidatedCacheRoot final {
    struct State;
    std::shared_ptr<State> state_;

    explicit ValidatedCacheRoot(std::shared_ptr<State> state) noexcept
        : state_(std::move(state)) {
    }

    friend struct TrustedCacheDirectoryAccess;
    friend struct TrustedCacheAccess;
    friend ValidatedCacheRoot adopt_trusted_cache_root(
        const xdg_paths::CachePaths& paths,
        xdg_directory_safety::PreparedDirectory directory);
    friend ValidatedPrivateCacheRoot prepare_private_trusted_cache_root(
        const ValidatedCacheRoot& root);
    friend class ValidatedCachePath;
    friend class WorkDirGuard;
    friend PreparedCacheCleanup preflight_cache_cleanup(
        const ValidatedCacheRoot& root);

public:
    const std::filesystem::path& path() const noexcept;

    // Compatibility display view。authorizationにはretained descriptorを使う。
    const std::filesystem::path& canonical_path() const noexcept;

    std::uintmax_t device() const noexcept;
    std::uintmax_t inode() const noexcept;
    std::uintmax_t owner() const noexcept;

    void require_unchanged_identity() const;
};

// artifact workspace等、root descriptorを独立して所有するconsumer用capability。
// 元のValidatedCacheRootと同じadopted inodeからだけ派生できる。
class ValidatedPrivateCacheRoot final {
    ValidatedCacheRoot trusted_root_;
    int directory_descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;

    ValidatedPrivateCacheRoot(
        ValidatedCacheRoot trusted_root, int directory_descriptor,
        std::uintmax_t device, std::uintmax_t inode,
        std::uintmax_t owner) noexcept;

    void require_unchanged_identity_for_owner(
        std::uintmax_t expected_effective_user) const;

    int directory_descriptor() const noexcept {
        return directory_descriptor_;
    }

    friend ValidatedPrivateCacheRoot prepare_private_trusted_cache_root(
        const ValidatedCacheRoot& root);
    friend class ArtifactWorkspace;
    friend ArtifactWorkspace create_artifact_workspace(
        ValidatedPrivateCacheRoot root);
#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
    friend void require_private_cache_root_identity_for_test(
        const ValidatedPrivateCacheRoot& root,
        std::uintmax_t expected_effective_user);
#endif

public:
    ValidatedPrivateCacheRoot(const ValidatedPrivateCacheRoot&) = delete;
    ValidatedPrivateCacheRoot& operator=(
        const ValidatedPrivateCacheRoot&) = delete;
    ValidatedPrivateCacheRoot(ValidatedPrivateCacheRoot&& other) noexcept;
    ValidatedPrivateCacheRoot& operator=(ValidatedPrivateCacheRoot&&) = delete;
    ~ValidatedPrivateCacheRoot() noexcept;

    const std::filesystem::path& path() const noexcept {
        return trusted_root_.path();
    }

    const std::filesystem::path& canonical_path() const noexcept {
        return trusted_root_.canonical_path();
    }

    std::uintmax_t device() const noexcept {
        return device_;
    }

    std::uintmax_t inode() const noexcept {
        return inode_;
    }

    std::uintmax_t owner() const noexcept {
        return owner_;
    }

    void require_unchanged_identity() const;
};

// validation済みのroot direct childと、そのobserved inode identityを一体で
// 保持するcapability。Missing capabilityは作成許可そのものではない。
class ValidatedCachePath final {
    ValidatedCacheRoot root_;
    std::filesystem::path path_;
    std::filesystem::path canonical_path_;
    std::string leaf_name_;
    bool exists_ = false;
    bool is_directory_ = false;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;
    std::uintmax_t permissions_ = 0;

    ValidatedCachePath(
        ValidatedCacheRoot root, std::filesystem::path path,
        std::string leaf_name, bool exists, bool is_directory,
        std::uintmax_t device, std::uintmax_t inode,
        std::uintmax_t owner, std::uintmax_t permissions)
        : root_(std::move(root)), path_(std::move(path)),
          canonical_path_(path_), leaf_name_(std::move(leaf_name)),
          exists_(exists), is_directory_(is_directory), device_(device),
          inode_(inode), owner_(owner), permissions_(permissions) {
    }

    friend ValidatedCachePath require_trusted_cache_path(
        const ValidatedCacheRoot& root, const std::filesystem::path& path,
        CachePathRequirement requirement);
    friend ValidatedCachePath revalidate_trusted_cache_path(
        const ValidatedCachePath& path, CachePathRequirement requirement);
    friend ValidatedCachePath create_trusted_cache_directory(
        const ValidatedCacheRoot& root, const std::filesystem::path& path);
    friend void remove_trusted_cache_path(const ValidatedCachePath& path);
    friend class WorkDirGuard;
    friend class DirCleanupGuard;
    friend struct TrustedCacheAccess;

public:
    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    const std::filesystem::path& canonical_path() const noexcept {
        return canonical_path_;
    }

    bool exists() const noexcept {
        return exists_;
    }

    bool is_directory() const noexcept {
        return is_directory_;
    }

    std::uintmax_t device() const noexcept {
        return device_;
    }

    std::uintmax_t inode() const noexcept {
        return inode_;
    }
};

// ValidatedCachePathのexact directory inodeをroot-relative openで保持する
// typed bridge。descriptorはnarrow friendだけが利用し、raw getterは公開しない。
class RetainedTrustedCacheDirectory final {
    ValidatedCachePath path_;
    int descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;

    RetainedTrustedCacheDirectory(
        ValidatedCachePath path, int descriptor,
        std::uintmax_t device, std::uintmax_t inode) noexcept;

    friend RetainedTrustedCacheDirectory retain_trusted_cache_directory(
        const ValidatedCachePath& path);
    friend class ArtifactMakepkgContext;
    friend struct PersistentCheckoutDirectoryAccess;
    friend ReviewedSourcePackageBaseLease
    acquire_reviewed_source_package_base_lease(
        RetainedTrustedCacheDirectory directory);
    friend class LocalSourceWorkspace;
    friend struct LocalSourceWorkspaceCleanupAccess;
    friend LocalSourceWorkspace materialize_local_source_workspace(
        const LocalSourceRoot& source_root,
        const ValidatedCacheRoot& cache_root);

    // Invocation-owned build output may change the workspace root mode.
    // Re-establish the original named inode and private owner mode before the
    // source-workspace-specific recursive cleanup runs.
    void prepare_for_owned_cleanup();

public:
    RetainedTrustedCacheDirectory(
        const RetainedTrustedCacheDirectory&) = delete;
    RetainedTrustedCacheDirectory& operator=(
        const RetainedTrustedCacheDirectory&) = delete;
    RetainedTrustedCacheDirectory(
        RetainedTrustedCacheDirectory&& other) noexcept;
    RetainedTrustedCacheDirectory& operator=(
        RetainedTrustedCacheDirectory&&) = delete;
    ~RetainedTrustedCacheDirectory() noexcept;

    const ValidatedCachePath& path() const noexcept {
        return path_;
    }

    void require_unchanged_identity() const;
};

// Cache-only resolve/prepare後のtyped directory capabilityをconsumeして
// adoptする唯一のbridge。environment、cwd、absolute pathを再解決しない。
ValidatedCacheRoot adopt_trusted_cache_root(
    const xdg_paths::CachePaths& paths,
    xdg_directory_safety::PreparedDirectory directory);

ValidatedPrivateCacheRoot prepare_private_trusted_cache_root(
    const ValidatedCacheRoot& root);

#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
void require_private_cache_root_identity_for_test(
    const ValidatedPrivateCacheRoot& root,
    std::uintmax_t expected_effective_user);
#endif

ValidatedCachePath require_trusted_cache_path(
    const ValidatedCacheRoot& root, const std::filesystem::path& path,
    CachePathRequirement requirement);
ValidatedCachePath revalidate_trusted_cache_path(
    const ValidatedCachePath& path, CachePathRequirement requirement);

RetainedTrustedCacheDirectory retain_trusted_cache_directory(
    const ValidatedCachePath& path);

// Clone rollbackはDirCleanupGuard constructionでretained descriptorを取得し、
// そのguard lifetime中にidentityをpinできた0700 directoryだけを所有対象にする。
// name creationからdescriptor取得までや、別々のretained intervalを跨ぐatomicな
// ownership continuityはLinux syscall上保証しない。
ValidatedCachePath create_trusted_cache_directory(
    const ValidatedCacheRoot& root, const std::filesystem::path& path);

void remove_trusted_cache_path(const ValidatedCachePath& path);

// clean開始前にtrusted rootと全target/treeのmetadata・opaque generationを所有する
// move-only capability。consumerはpacman clean/promptを跨いで保持し、削除時に
// by-valueでconsumeする。node FDとPackageBase leaseは各検証・削除scopeだけで保持し、
// mutation authorityは常にroot-relative named lineageから再構築する。
class PreparedCacheCleanup final {
    struct State;
    std::unique_ptr<State> state_;

    explicit PreparedCacheCleanup(std::unique_ptr<State> state) noexcept;

    friend PreparedCacheCleanup preflight_cache_cleanup(
        const ValidatedCacheRoot& root);
    friend void remove_preflighted_cache_paths(
        PreparedCacheCleanup cleanup);

public:
    PreparedCacheCleanup(const PreparedCacheCleanup&) = delete;
    PreparedCacheCleanup& operator=(const PreparedCacheCleanup&) = delete;
    PreparedCacheCleanup(PreparedCacheCleanup&& other) noexcept;
    PreparedCacheCleanup& operator=(PreparedCacheCleanup&&) = delete;
    ~PreparedCacheCleanup() noexcept;

    bool empty() const noexcept;
    std::size_t size() const noexcept;
    const std::filesystem::path& cache_path() const;
};

PreparedCacheCleanup preflight_cache_cleanup(
    const ValidatedCacheRoot& root);

// Preflight時から保持したoriginal identityを再検証し、同じplanをconsumeする。
void remove_preflighted_cache_paths(PreparedCacheCleanup cleanup);

#ifdef MOGUET_ENABLE_TRUSTED_CACHE_TEST_HOOKS
using TrustedCacheRemovalTestHook = std::function<void(std::size_t depth)>;
void set_trusted_cache_removal_test_hook(
    TrustedCacheRemovalTestHook hook);
using TrustedCacheStagedDirectoryTestHook =
    std::function<void(const std::filesystem::path& path)>;
void set_trusted_cache_staged_directory_test_hook(
    TrustedCacheStagedDirectoryTestHook hook);
#endif

// retained directory descriptorへfchdirし、caller cwdもdescriptorで復元する。
class WorkDirGuard final {
    int original_descriptor_ = -1;
    int target_descriptor_ = -1;

public:
    explicit WorkDirGuard(const ValidatedCacheRoot& target);
    explicit WorkDirGuard(const ValidatedCachePath& target);
    WorkDirGuard(const WorkDirGuard&) = delete;
    WorkDirGuard& operator=(const WorkDirGuard&) = delete;
    WorkDirGuard(WorkDirGuard&&) = delete;
    WorkDirGuard& operator=(WorkDirGuard&&) = delete;
    ~WorkDirGuard() noexcept;
};

// failed cloneが所有するexact child inodeを、成功確定までrollback可能に保つ。
class DirCleanupGuard final {
    ValidatedCachePath path_;
    int retained_descriptor_ = -1;
    std::uintmax_t retained_device_ = 0;
    std::uintmax_t retained_inode_ = 0;
    std::uintmax_t retained_owner_ = 0;
    std::uintmax_t retained_permissions_ = 0;
    bool committed_ = false;

public:
    explicit DirCleanupGuard(const ValidatedCachePath& path);
    DirCleanupGuard(const DirCleanupGuard&) = delete;
    DirCleanupGuard& operator=(const DirCleanupGuard&) = delete;
    DirCleanupGuard(DirCleanupGuard&&) = delete;
    DirCleanupGuard& operator=(DirCleanupGuard&&) = delete;
    void commit() noexcept;
    ~DirCleanupGuard() noexcept;
};
