#pragma once

#include "local_package_metadata.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

enum class LocalSourceRootStage {
    InvocationAnchorOpen,
    RootInspection,
    RootOpen,
    CanonicalPathResolution,
    RootRevalidation,
    PkgbuildInspection,
    PkgbuildOpen,
    PkgbuildRead,
    PkgbuildRevalidation,
    SrcinfoInspection,
    SrcinfoOpen,
    SrcinfoRead,
    SrcinfoRevalidation,
    MetadataRevalidation
};

enum class LocalSourceRootErrorCode {
    InvalidInputPath,
    Missing,
    Symlink,
    NotDirectory,
    NotRegularFile,
    OwnershipMismatch,
    UnsafePermissions,
    PermissionDenied,
    MetadataFailure,
    ReadFailure,
    ConcurrentReplacement,
    ContentChanged,
    UnsafeMetadata
};

struct LocalSourceRootFailure {
    LocalSourceRootStage stage = LocalSourceRootStage::RootInspection;
    LocalSourceRootErrorCode code =
        LocalSourceRootErrorCode::MetadataFailure;
    std::filesystem::path path;
    std::optional<std::error_code> system_error;

    bool operator==(const LocalSourceRootFailure&) const = default;
};

// Presentation is owned by a later CLI slice. This exception carries only
// typed control-flow data and a stable internal what() token.
class LocalSourceRootError final : public std::runtime_error {
    LocalSourceRootFailure failure_;

public:
    explicit LocalSourceRootError(LocalSourceRootFailure failure);

    const LocalSourceRootFailure& failure() const noexcept {
        return failure_;
    }
};

enum class LocalSourceNodeType {
    Directory,
    RegularFile
};

struct LocalSourceDirectoryIdentity {
    LocalSourceNodeType type = LocalSourceNodeType::Directory;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;

    bool operator==(const LocalSourceDirectoryIdentity&) const = default;
};

struct LocalSourceFileIdentity {
    LocalSourceNodeType type = LocalSourceNodeType::RegularFile;
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;
    std::uintmax_t owner = 0;
    std::uintmax_t mode = 0;
    std::intmax_t size = 0;
    std::intmax_t modification_time_seconds = 0;
    std::intmax_t modification_time_nanoseconds = 0;
    std::intmax_t status_change_time_seconds = 0;
    std::intmax_t status_change_time_nanoseconds = 0;

    bool operator==(const LocalSourceFileIdentity&) const = default;
};

struct LocalSourceFileSnapshot {
    std::filesystem::path path;
    LocalSourceFileIdentity identity;
    std::string contents;

    bool operator==(const LocalSourceFileSnapshot&) const = default;
};

enum class LocalSourceMetadataState {
    Missing,
    Unsafe,
    Invalid,
    UsableUnverified,
    KnownStale
};

enum class LocalSourceMetadataProvenance {
    ExistingSrcinfo
};

enum class LocalSourceMetadataStaleReason {
    PkgbuildNewer,
    OneOffEnvironmentAssignment
};

class LocalSourceRoot;
class LocalSourceWorkspace;
class ValidatedCacheRoot;
struct LocalSourceMetadataEvaluationAccess;
LocalSourceRoot open_local_source_root(
    const std::filesystem::path& input_path,
    bool has_one_off_environment_assignment);
LocalSourceWorkspace materialize_local_source_workspace(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root);
void require_local_source_cache_separation(
    const LocalSourceRoot& source_root,
    const ValidatedCacheRoot& cache_root);
void require_directory_identity_outside_local_source_tree(
    const LocalSourceRoot& source_root,
    std::uintmax_t directory_device,
    std::uintmax_t directory_inode);
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
void require_cache_identity_outside_source_tree_for_test(
    const LocalSourceRoot& source_root,
    std::uintmax_t cache_device, std::uintmax_t cache_inode);
#endif

class LocalSourceMetadataSnapshot final {
    LocalSourceMetadataState state_ = LocalSourceMetadataState::Missing;
    std::optional<LocalSourceMetadataProvenance> provenance_;
    std::optional<LocalSourceFileSnapshot> file_;
    std::optional<LocalPackageMetadataParseResult> parse_result_;
    std::optional<LocalSourceRootFailure> unsafe_failure_;
    std::vector<LocalSourceMetadataStaleReason> stale_reasons_;

    LocalSourceMetadataSnapshot(
        LocalSourceMetadataState state,
        std::optional<LocalSourceMetadataProvenance> provenance,
        std::optional<LocalSourceFileSnapshot> file,
        std::optional<LocalPackageMetadataParseResult> parse_result,
        std::optional<LocalSourceRootFailure> unsafe_failure,
        std::vector<LocalSourceMetadataStaleReason> stale_reasons) noexcept;

    friend class LocalSourceRoot;
    friend LocalSourceRoot open_local_source_root(
        const std::filesystem::path& input_path,
        bool has_one_off_environment_assignment);
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
    friend struct LocalSourceRootTestAccess;
#endif

public:
    LocalSourceMetadataSnapshot(const LocalSourceMetadataSnapshot&) = default;
    LocalSourceMetadataSnapshot(
        LocalSourceMetadataSnapshot&&) noexcept = default;
    LocalSourceMetadataSnapshot& operator=(
        const LocalSourceMetadataSnapshot&) = delete;
    LocalSourceMetadataSnapshot& operator=(
        LocalSourceMetadataSnapshot&&) noexcept = delete;
    ~LocalSourceMetadataSnapshot() = default;

    LocalSourceMetadataState state() const noexcept {
        return state_;
    }

    std::optional<LocalSourceMetadataProvenance> provenance() const noexcept {
        return provenance_;
    }

    const LocalSourceFileSnapshot* file() const noexcept {
        return file_.has_value() ? &file_.value() : nullptr;
    }

    const LocalPackageMetadataParseResult* parse_result() const noexcept {
        return parse_result_.has_value() ? &parse_result_.value() : nullptr;
    }

    const LocalSourceRootFailure* unsafe_failure() const noexcept {
        return unsafe_failure_.has_value() ? &unsafe_failure_.value() : nullptr;
    }

    const std::vector<LocalSourceMetadataStaleReason>& stale_reasons()
        const noexcept {
        return stale_reasons_;
    }
};

class LocalSourceRoot final {
    std::filesystem::path input_path_;
    std::filesystem::path lookup_path_;
    std::filesystem::path canonical_path_;
    int invocation_anchor_descriptor_ = -1;
    int directory_descriptor_ = -1;
    int pkgbuild_descriptor_ = -1;
    int srcinfo_descriptor_ = -1;
    std::uintmax_t expected_owner_ = 0;
    LocalSourceDirectoryIdentity directory_identity_;
    LocalSourceFileSnapshot pkgbuild_;
    LocalSourceMetadataSnapshot metadata_;

    LocalSourceRoot(
        std::filesystem::path input_path,
        std::filesystem::path lookup_path,
        std::filesystem::path canonical_path,
        int invocation_anchor_descriptor, int directory_descriptor,
        int pkgbuild_descriptor, int srcinfo_descriptor,
        std::uintmax_t expected_owner,
        LocalSourceDirectoryIdentity directory_identity,
        LocalSourceFileSnapshot pkgbuild,
        LocalSourceMetadataSnapshot metadata) noexcept;

    friend LocalSourceRoot open_local_source_root(
        const std::filesystem::path& input_path,
        bool has_one_off_environment_assignment);
    friend LocalSourceWorkspace materialize_local_source_workspace(
        const LocalSourceRoot& source_root,
        const ValidatedCacheRoot& cache_root);
    friend void require_local_source_cache_separation(
        const LocalSourceRoot& source_root,
        const ValidatedCacheRoot& cache_root);
    friend void require_directory_identity_outside_local_source_tree(
        const LocalSourceRoot& source_root,
        std::uintmax_t directory_device,
        std::uintmax_t directory_inode);
    friend struct LocalSourceMetadataEvaluationAccess;
    friend struct LocalRecipeCandidateAccess;
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_WORKSPACE_TEST_HOOKS
    friend void require_cache_identity_outside_source_tree_for_test(
        const LocalSourceRoot& source_root,
        std::uintmax_t cache_device, std::uintmax_t cache_inode);
#endif
#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
    friend struct LocalSourceRootTestAccess;
#endif

public:
    LocalSourceRoot(const LocalSourceRoot&) = delete;
    LocalSourceRoot& operator=(const LocalSourceRoot&) = delete;
    LocalSourceRoot(LocalSourceRoot&& other) noexcept;
    LocalSourceRoot& operator=(LocalSourceRoot&&) = delete;
    ~LocalSourceRoot() noexcept;

    const std::filesystem::path& input_path() const noexcept {
        return input_path_;
    }

    const std::filesystem::path& canonical_path() const noexcept {
        return canonical_path_;
    }

    const LocalSourceDirectoryIdentity& directory_identity() const noexcept {
        return directory_identity_;
    }

    const LocalSourceFileSnapshot& pkgbuild() const noexcept {
        return pkgbuild_;
    }

    const LocalSourceMetadataSnapshot& metadata() const noexcept {
        return metadata_;
    }

    void require_unchanged_identity() const;
};

// The current directory descriptor is captured before resolving input_path and
// retained for the complete LocalSourceRoot lifetime. Relative paths therefore
// remain anchored even if a later owner changes the process working directory.
LocalSourceRoot open_local_source_root(
    const std::filesystem::path& input_path,
    bool has_one_off_environment_assignment = false);

#ifdef MOGUET_ENABLE_LOCAL_SOURCE_ROOT_TEST_HOOKS
enum class LocalSourceRootTestFailurePoint {
    RootInspection,
    RootOpen,
    CanonicalPathResolution,
    PkgbuildInspection,
    PkgbuildOpen,
    PkgbuildRead,
    SrcinfoInspection,
    SrcinfoOpen,
    SrcinfoRead
};

struct LocalSourceRootInjectedFailure {
    LocalSourceRootTestFailurePoint point;
    int error_number;
};

// Focused tests may override observed ownership or one syscall boundary. No
// production overload exposes these bypasses.
struct LocalSourceRootTestOverrides {
    std::optional<std::uintmax_t> effective_user;
    std::optional<std::uintmax_t> root_observed_owner;
    std::optional<std::uintmax_t> pkgbuild_observed_owner;
    std::optional<std::uintmax_t> srcinfo_observed_owner;
    std::optional<LocalSourceRootInjectedFailure> injected_failure;
};

LocalSourceRoot open_local_source_root_for_test(
    const std::filesystem::path& input_path,
    bool has_one_off_environment_assignment,
    const LocalSourceRootTestOverrides& overrides);
#endif
