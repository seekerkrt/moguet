#pragma once

#include "evaluated_devel_source_build_authority.hpp"
#include "devel_build_provenance_reviewed_binding.hpp"
#include "reviewed_source_pinned_build.hpp"
#include "source_environment.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
#include <functional>
#endif

class InvocationOwnedSourceBuildContextAuthority;

enum class InvocationOwnedSourceBuildContextStage {
    PinnedBuildValidation,
    ReviewedBinding,
    SnapshotProjection,
    MakepkgExecutable,
    RootCreation,
    SnapshotMaterialization,
    SnapshotRevalidation,
    EnvironmentProjection,
    Cleanup,
};

enum class InvocationOwnedSourceBuildContextFailureReason {
    InvalidPinnedBuild,
    EditorOverlayPresent,
    ReviewedBindingFailure,
    UnsupportedRecipeShape,
    SnapshotFailure,
    UnsafeSourceEntry,
    RootCreationFailure,
    OwnershipMismatch,
    UnsafePermissions,
    ConcurrentReplacement,
    ContainmentFailure,
    MakepkgExecutableUnavailable,
    EnvironmentAssignmentConflict,
    InvalidEnvironmentAssignment,
    InvalidState,
    CleanupFailure,
    UnprovenCleanupContent,
    CleanupResourceLimitExceeded,
};

// A factory failure cannot return the partially constructed owner. If its
// descriptor-authorized abort cleanup also fails, this nested consequence
// keeps that second typed failure and the root that may remain.
struct InvocationOwnedSourceBuildContextConstructionCleanupFailure {
    InvocationOwnedSourceBuildContextStage stage =
        InvocationOwnedSourceBuildContextStage::Cleanup;
    InvocationOwnedSourceBuildContextFailureReason reason =
        InvocationOwnedSourceBuildContextFailureReason::CleanupFailure;
    std::filesystem::path relative_path;
    std::filesystem::path owned_root;
    std::optional<std::error_code> system_error;
    std::optional<std::string> diagnostic;

    bool operator==(
        const InvocationOwnedSourceBuildContextConstructionCleanupFailure&)
        const = default;
};

// One failure keeps the typed #411/Git cause where one exists. A filesystem
// failure may additionally retain errno and the exact snapshot-relative entry;
// callers never need to infer the failure class from diagnostic prose. A
// construction cleanup consequence is kept separately from the primary
// creation failure instead of replacing or flattening it.
struct InvocationOwnedSourceBuildContextFailure {
    InvocationOwnedSourceBuildContextStage stage =
        InvocationOwnedSourceBuildContextStage::PinnedBuildValidation;
    InvocationOwnedSourceBuildContextFailureReason reason =
        InvocationOwnedSourceBuildContextFailureReason::InvalidPinnedBuild;
    std::filesystem::path relative_path;
    std::optional<std::error_code> system_error;
    std::optional<ReviewedSourceStateRecordBindingFailure> binding_failure;
    std::optional<TrustedGitReviewFailure> git_failure;
    std::optional<ReviewedSourceReviewFailure> review_failure;
    std::optional<TrustedGitPinnedCheckoutFailure> checkout_failure;
    std::optional<std::string> diagnostic;
    std::optional<
        InvocationOwnedSourceBuildContextConstructionCleanupFailure>
        construction_cleanup_failure;

    bool operator==(
        const InvocationOwnedSourceBuildContextFailure&) const = default;
};

// This is not a second recipe authority. It records that the exact #411
// binding was materialized and reproven as the corresponding Git tree
// projection in the invocation-owned recipe root.
class ReviewedRecipeSnapshotIdentity final {
public:
    ReviewedRecipeSnapshotIdentity() = delete;
    ReviewedRecipeSnapshotIdentity(
        const ReviewedRecipeSnapshotIdentity&) = default;
    ReviewedRecipeSnapshotIdentity(
        ReviewedRecipeSnapshotIdentity&&) noexcept = default;
    ReviewedRecipeSnapshotIdentity& operator=(
        const ReviewedRecipeSnapshotIdentity&) = default;
    ReviewedRecipeSnapshotIdentity& operator=(
        ReviewedRecipeSnapshotIdentity&&) noexcept = default;
    ~ReviewedRecipeSnapshotIdentity() = default;

    [[nodiscard]] const ReviewedSourceStateRecordBinding& reviewed_binding()
        const noexcept;
    [[nodiscard]] const ReviewedSourceObjectId& git_tree_object_id()
        const noexcept;
    [[nodiscard]] std::size_t tracked_entry_count() const noexcept;

    bool operator==(const ReviewedRecipeSnapshotIdentity&) const = default;

private:
    friend class InvocationOwnedSourceBuildContextAuthority;

    ReviewedRecipeSnapshotIdentity(
        ReviewedSourceStateRecordBinding reviewed_binding,
        ReviewedSourceObjectId git_tree_object_id,
        std::size_t tracked_entry_count) noexcept;

    ReviewedSourceStateRecordBinding reviewed_binding_;
    ReviewedSourceObjectId git_tree_object_id_;
    std::size_t tracked_entry_count_ = 0;
};

// The fixed absolute makepkg path and the retained filesystem identity are
// captured once for the context. Later phases can reprove the same named
// executable instead of repeating PATH lookup.
class InvocationOwnedMakepkgExecutableIdentity final {
public:
    InvocationOwnedMakepkgExecutableIdentity() = delete;
    InvocationOwnedMakepkgExecutableIdentity(
        const InvocationOwnedMakepkgExecutableIdentity&) = delete;
    InvocationOwnedMakepkgExecutableIdentity(
        InvocationOwnedMakepkgExecutableIdentity&& other) noexcept;
    InvocationOwnedMakepkgExecutableIdentity& operator=(
        const InvocationOwnedMakepkgExecutableIdentity&) = delete;
    InvocationOwnedMakepkgExecutableIdentity& operator=(
        InvocationOwnedMakepkgExecutableIdentity&&) = delete;
    ~InvocationOwnedMakepkgExecutableIdentity() noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] std::uintmax_t device() const noexcept;
    [[nodiscard]] std::uintmax_t inode() const noexcept;
    [[nodiscard]] std::uintmax_t owner() const noexcept;
    [[nodiscard]] std::uintmax_t mode() const noexcept;

private:
    friend class InvocationOwnedSourceBuildContextAuthority;
    friend class InvocationOwnedSourceBuildContext;
    friend class EvaluatedDevelSourceBuildAuthority;

    InvocationOwnedMakepkgExecutableIdentity(
        std::filesystem::path path, int descriptor,
        std::uintmax_t device, std::uintmax_t inode,
        std::uintmax_t owner, std::uintmax_t mode) noexcept;
    void require_unchanged() const;

    std::filesystem::path path_;
    int descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;
    std::uintmax_t mode_ = 0;
};

// Pure future-process projection. The caller's ordered customization is
// retained, then the three authority-owned roots are appended exactly once.
// A private lineage token prevents an environment from being treated as one
// belonging to another context.
class InvocationOwnedMakepkgEnvironment final {
public:
    InvocationOwnedMakepkgEnvironment() = delete;
    InvocationOwnedMakepkgEnvironment(
        const InvocationOwnedMakepkgEnvironment&) = delete;
    InvocationOwnedMakepkgEnvironment(
        InvocationOwnedMakepkgEnvironment&&) noexcept = default;
    InvocationOwnedMakepkgEnvironment& operator=(
        const InvocationOwnedMakepkgEnvironment&) = delete;
    InvocationOwnedMakepkgEnvironment& operator=(
        InvocationOwnedMakepkgEnvironment&&) = delete;
    ~InvocationOwnedMakepkgEnvironment() = default;

    [[nodiscard]] const SourceBuildEnvironment& source_environment()
        const noexcept;
    [[nodiscard]] SourceEnvironmentEmptyValuePolicy empty_value_policy()
        const noexcept;

private:
    friend class InvocationOwnedSourceBuildContextAuthority;
    friend class InvocationOwnedSourceBuildContext;
    friend class EvaluatedDevelSourceBuildAuthority;

    InvocationOwnedMakepkgEnvironment(
        SourceBuildEnvironment source_environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy,
        std::shared_ptr<const void> lineage) noexcept;

    SourceBuildEnvironment source_environment_;
    SourceEnvironmentEmptyValuePolicy empty_value_policy_ =
        SourceEnvironmentEmptyValuePolicy::Omit;
    std::shared_ptr<const void> lineage_;
};

struct InvocationOwnedSourceBuildContextValidated {
    bool operator==(
        const InvocationOwnedSourceBuildContextValidated&) const = default;
};

struct InvocationOwnedSourceBuildContextCleaned {
    bool operator==(
        const InvocationOwnedSourceBuildContextCleaned&) const = default;
};

using InvocationOwnedSourceBuildContextValidationResult = std::variant<
    InvocationOwnedSourceBuildContextValidated,
    InvocationOwnedSourceBuildContextFailure>;

using InvocationOwnedSourceBuildContextCleanupResult = std::variant<
    InvocationOwnedSourceBuildContextCleaned,
    InvocationOwnedSourceBuildContextFailure>;

using InvocationOwnedMakepkgEnvironmentResult = std::variant<
    InvocationOwnedMakepkgEnvironment,
    InvocationOwnedSourceBuildContextFailure>;

// POLICY(#476 Slice 3): this move-only capability owns one exact reviewed
// recipe snapshot and every private makepkg root for one invocation. It does
// not run makepkg, install an artifact, observe an installed binding, or
// publish provenance.
class InvocationOwnedSourceBuildContext final {
public:
    InvocationOwnedSourceBuildContext() = delete;
    InvocationOwnedSourceBuildContext(
        const InvocationOwnedSourceBuildContext&) = delete;
    InvocationOwnedSourceBuildContext& operator=(
        const InvocationOwnedSourceBuildContext&) = delete;
    InvocationOwnedSourceBuildContext(
        InvocationOwnedSourceBuildContext&& other) noexcept;
    InvocationOwnedSourceBuildContext& operator=(
        InvocationOwnedSourceBuildContext&&) = delete;
    ~InvocationOwnedSourceBuildContext() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const PackageBaseIdentity& package_base() const;
    [[nodiscard]] const ReviewedSourceStateRecordBinding& reviewed_binding()
        const;
    [[nodiscard]] const ReviewedRecipeSnapshotIdentity& snapshot_identity()
        const;
    [[nodiscard]] const std::filesystem::path& recipe_root() const;
    [[nodiscard]] const std::filesystem::path& pkgdest() const;
    [[nodiscard]] const std::filesystem::path& builddir() const;
    [[nodiscard]] const std::filesystem::path& srcdest() const;
    [[nodiscard]] const std::filesystem::path& owned_root() const;
    [[nodiscard]] const InvocationOwnedMakepkgExecutableIdentity&
    makepkg_executable() const;

    [[nodiscard]] InvocationOwnedSourceBuildContextValidationResult
    revalidate() const;

    [[nodiscard]] InvocationOwnedMakepkgEnvironmentResult
    make_makepkg_environment(
        const SourceBuildEnvironment& customization,
        SourceEnvironmentEmptyValuePolicy empty_value_policy) const;

    [[nodiscard]] bool owns_makepkg_environment(
        const InvocationOwnedMakepkgEnvironment& environment) const noexcept;

    // Explicit retry is possible for transient failures. Unproven-content and
    // budget refusals are permanent for this context. Destruction never retries
    // an already attempted cleanup.
    [[nodiscard]] InvocationOwnedSourceBuildContextCleanupResult cleanup() noexcept;

private:
    friend class InvocationOwnedSourceBuildContextAuthority;
    friend class EvaluatedDevelSourceBuildAuthority;

    struct State;
    explicit InvocationOwnedSourceBuildContext(
        std::unique_ptr<State> state) noexcept;

    [[nodiscard]] const State& require_state() const;
    [[nodiscard]] State& require_state();
    [[nodiscard]] int recipe_descriptor() const;
    [[nodiscard]] int pkgdest_descriptor() const;
    [[nodiscard]] int builddir_descriptor() const;
    [[nodiscard]] int srcdest_descriptor() const;
    [[nodiscard]] std::uintmax_t root_device() const;
    // Slice 4 rejects the complete context when artifact/workspace proof has
    // rejected entries that a fresh generic cleanup scan must not adopt.
    void refuse_unproven_cleanup() noexcept;

    std::unique_ptr<State> state_;
};

using InvocationOwnedSourceBuildContextResult = std::variant<
    InvocationOwnedSourceBuildContext,
    InvocationOwnedSourceBuildContextFailure>;

// The only production mint consumes the complete #411 pinned capability.
// PackageBase/OID/path tuples and arbitrary checkout paths are not inputs.
[[nodiscard]] InvocationOwnedSourceBuildContextResult
create_invocation_owned_source_build_context(
    PinnedReviewedSourceBuild pinned_build);

// Complete, non-instantiable friend authority. Keeping the definition in this
// header prevents another translation unit from defining a same-named class
// to acquire the private #411/snapshot construction privileges.
class InvocationOwnedSourceBuildContextAuthority final {
    InvocationOwnedSourceBuildContextAuthority() = delete;

    friend InvocationOwnedSourceBuildContextResult
    create_invocation_owned_source_build_context(
        PinnedReviewedSourceBuild pinned_build);

    [[nodiscard]] static InvocationOwnedSourceBuildContextResult create(
        PinnedReviewedSourceBuild pinned_build);
};

#ifdef MOGUET_ENABLE_INVOCATION_OWNED_SOURCE_BUILD_CONTEXT_TEST_HOOKS
enum class InvocationOwnedSourceBuildContextTestEvent {
    BeforePrivateRootCreation,
    AfterRootCreated,
    AfterChildMkdir,
    AfterChildRetained,
    AfterChildModeSealed,
    BeforeChildFinalOpen,
    BeforeChildFinalReproof,
    AfterRecipeCreated,
    AfterPkgdestCreated,
    AfterBuilddirCreated,
    AfterSrcdestCreated,
    AfterPrivateRootsCreated,
    BeforeFinalReviewedSourceReproof,
    BeforeCleanup,
};

using InvocationOwnedSourceBuildContextTestHook = std::function<void(
    InvocationOwnedSourceBuildContextTestEvent event,
    const std::filesystem::path& owned_root)>;

void set_invocation_owned_source_build_context_test_hook(
    InvocationOwnedSourceBuildContextTestHook hook);

void set_invocation_owned_source_build_context_makepkg_path_for_test(
    std::optional<std::filesystem::path> makepkg_path);

void set_invocation_owned_source_build_context_parent_path_for_test(
    std::optional<std::filesystem::path> parent_path);

void set_invocation_owned_source_build_context_cleanup_limits_for_test(
    std::optional<std::pair<std::size_t, std::size_t>> entry_and_depth_limits);

[[nodiscard]] std::optional<
    InvocationOwnedSourceBuildContextFailureReason>
invocation_owned_source_build_context_parent_policy_failure_for_test(
    std::uintmax_t owner, std::uintmax_t mode,
    std::uintmax_t effective_user);

[[nodiscard]] bool
invocation_owned_source_build_context_snapshot_path_is_safe_for_test(
    const std::string& raw_path);
#endif
