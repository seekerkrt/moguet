#pragma once

#include "process.hpp"
#include "reviewed_source_pinned_build.hpp"
#include "source_environment.hpp"
#include "trusted_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class ExpectedPackageArtifactPath;
class ExpectedPackageArtifactSet;
class ValidatedPackageArtifactPath;
class ValidatedPackageArtifactSet;
class SourceArtifactInstallTrustedTransport;
class PreparedPackageBaseArtifactInstall;
class ArtifactWorkspace;
class ArtifactMakepkgContext;
struct ArtifactMakepkgContextProvenance;

enum class ProductionSourceReviewStatus {
    NotApplicable,
    CompatibilityWithoutReview,
    Reviewed,
};

enum class ProductionSourceBuildCommandOutcome {
    NotAttempted,
    Started,
    Failed,
    Succeeded,
};

enum class ProductionSourceInstallOutcome {
    NotAttempted,
    Started,
    Failed,
    Succeeded,
};

// Reviewed-source lifecycle authorityから一方向に写したproduction outcome。
// build/install authorityではなく、aggregate/CLIがaccepted lifecycleを
// AlreadyReviewedやcompatibilityへflattenしないためのsnapshotである。
enum class ProductionReviewedSourceOutcome {
    InitialFullReview,
    UpdateReview,
    RebaselineFullReview,
    AbnormalStateRebindFullReview,
    AlreadyReviewed,
};

struct ProductionSourceBuildProvenance {
    ProductionSourceReviewStatus review_status =
        ProductionSourceReviewStatus::NotApplicable;
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None;
    std::optional<ReviewedSourceCompatibilityBuildReason>
        compatibility_reason;
    std::optional<SourceRevisionIdentity> reviewed_upstream_base_revision;
    std::optional<ReviewedSourcePublicationStatus> publication_status;
    std::optional<ProductionReviewedSourceOutcome> reviewed_outcome;
    std::optional<ReviewedSourceAbnormalStateReason> abnormal_state_reason;
    std::optional<std::uint64_t> reviewed_state_generation;

    bool operator==(const ProductionSourceBuildProvenance&) const = default;
};

// Review/state publication, makepkg, and package transaction are independent
// production dimensions. A phase advances only at its own execution boundary;
// later validation, metadata, install, or cleanup failure must not erase an
// already completed dimension.
struct ProductionSourceBuildStagedOutcome {
    ProductionSourceBuildProvenance source_provenance;
    ProductionSourceBuildCommandOutcome build_outcome =
        ProductionSourceBuildCommandOutcome::NotAttempted;
    ProductionSourceInstallOutcome install_outcome =
        ProductionSourceInstallOutcome::NotAttempted;

    bool operator==(const ProductionSourceBuildStagedOutcome&) const =
        default;
};

// Remote production makepkg authority. Compatibility routes retain the same
// cooperative PackageBase lease; reviewed routes retain the complete 4B
// capability instead of reducing it to a path/OID sidecar.
class ProductionArtifactSourceTree final {
    ValidatedCachePath checkout_;
    std::shared_ptr<void> authority_lifetime_;
    std::function<int(const std::string&, const std::string&)>
        guarded_command_;
    ProductionSourceBuildProvenance provenance_;

    ProductionArtifactSourceTree(
        ValidatedCachePath checkout,
        std::shared_ptr<void> authority_lifetime,
        std::function<int(
            const std::string&, const std::string&)>
            guarded_command,
        ProductionSourceBuildProvenance provenance);

    const ValidatedCachePath& checkout() const noexcept;
    void require_unchanged_identity() const;
    int run_guarded_command(
        const std::string& command,
        const std::string& display_command = {}) const;

    friend class ArtifactMakepkgContext;
    friend ProductionArtifactSourceTree
    make_unreviewed_production_artifact_source_tree(
        ValidatedCachePath checkout,
        ReviewedSourcePackageBaseLease lease,
        ProductionSourceReviewStatus review_status,
        ReviewedSourceEditorOverlayStatus editor_overlay,
        std::optional<ReviewedSourceCompatibilityBuildReason>
            compatibility_reason);
    friend ProductionArtifactSourceTree
    make_reviewed_production_artifact_source_tree(
        ValidatedCachePath checkout,
        PinnedReviewedSourceBuild reviewed,
        ProductionReviewedSourceOutcome reviewed_outcome,
        std::optional<ReviewedSourceAbnormalStateReason>
            abnormal_state_reason);
    friend ArtifactMakepkgContext prepare_artifact_makepkg_context(
        ProductionArtifactSourceTree source_tree,
        const ArtifactWorkspace& workspace,
        const SourceBuildEnvironment& environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy);

public:
    ProductionArtifactSourceTree(
        const ProductionArtifactSourceTree&) = delete;
    ProductionArtifactSourceTree& operator=(
        const ProductionArtifactSourceTree&) = delete;
    ProductionArtifactSourceTree(
        ProductionArtifactSourceTree&&) noexcept = default;
    ProductionArtifactSourceTree& operator=(
        ProductionArtifactSourceTree&&) = delete;
    ~ProductionArtifactSourceTree() = default;

    [[nodiscard]] const ProductionSourceBuildProvenance& provenance()
        const noexcept;

#if defined(MOGUET_ENABLE_SEPARATED_SOURCE_BUILD_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS)
    ProductionArtifactSourceTree(ValidatedCachePath checkout)
        : ProductionArtifactSourceTree(
              std::move(checkout), std::make_shared<int>(0),
              [](const std::string& command,
                 const std::string& display_command) {
                  return run_command_with_parent_independent_lifetime_guard(
                      command, -1, display_command);
              },
              {}) {
    }

    [[nodiscard]] static ProductionArtifactSourceTree make_for_test(
        ValidatedCachePath checkout) {
        return ProductionArtifactSourceTree(
            std::move(checkout), std::make_shared<int>(0),
            [](const std::string& command,
               const std::string& display_command) {
                return run_command_with_parent_independent_lifetime_guard(
                    command, -1, display_command);
            },
            {});
    }
#endif
};

[[nodiscard]] ProductionArtifactSourceTree
make_unreviewed_production_artifact_source_tree(
    ValidatedCachePath checkout,
    ReviewedSourcePackageBaseLease lease,
    ProductionSourceReviewStatus review_status,
    ReviewedSourceEditorOverlayStatus editor_overlay,
    std::optional<ReviewedSourceCompatibilityBuildReason>
        compatibility_reason = std::nullopt);

[[nodiscard]] ProductionArtifactSourceTree
make_reviewed_production_artifact_source_tree(
    ValidatedCachePath checkout,
    PinnedReviewedSourceBuild reviewed,
    ProductionReviewedSourceOutcome reviewed_outcome,
    std::optional<ReviewedSourceAbnormalStateReason>
        abnormal_state_reason = std::nullopt);

// 一回のsource-build invocationだけが所有するfresh PKGDEST。
// POLICY(#242): named pathだけでなくroot/workspace FDと作成時identityを保持し、
// identityを再証明できた場合だけscope cleanupする。
class ArtifactWorkspace final {
    ValidatedPrivateCacheRoot root_;
    std::filesystem::path path_;
    std::filesystem::path canonical_path_;
    std::string leaf_name_;
    int directory_descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;
    bool owns_path_ = false;
    bool cleanup_on_destruction_ = true;

    ArtifactWorkspace(
        ValidatedPrivateCacheRoot root, std::filesystem::path path,
        std::filesystem::path canonical_path, std::string leaf_name,
        int directory_descriptor, std::uintmax_t device,
        std::uintmax_t inode, std::uintmax_t owner) noexcept;

    void require_unchanged_identity_for_owner(
        std::uintmax_t expected_effective_user) const;

    friend ArtifactWorkspace create_artifact_workspace(
        ValidatedPrivateCacheRoot root);
    friend ArtifactMakepkgContext prepare_artifact_makepkg_context(
        const ValidatedCachePath& checkout,
        const ArtifactWorkspace& workspace,
        const SourceBuildEnvironment& environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy);
    friend ArtifactMakepkgContext prepare_artifact_makepkg_context(
        ProductionArtifactSourceTree source_tree,
        const ArtifactWorkspace& workspace,
        const SourceBuildEnvironment& environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy);
    friend class ArtifactMakepkgContext;
    friend class ExpectedPackageArtifactPath;
    friend class ExpectedPackageArtifactSet;
    friend class ValidatedPackageArtifactPath;
    friend class ValidatedPackageArtifactSet;
    friend ExpectedPackageArtifactPath validate_makepkg_packagelist_output(
        const ArtifactWorkspace& workspace, const std::string& raw_output);
    friend ExpectedPackageArtifactSet
    validate_makepkg_packagelist_output_set(
        const ArtifactWorkspace& workspace,
        const std::string& raw_output);
    friend ExpectedPackageArtifactPath query_makepkg_packagelist(
        const ArtifactWorkspace& workspace,
        const ArtifactMakepkgContext& context);
    friend ExpectedPackageArtifactSet query_makepkg_packagelist_set(
        const ArtifactWorkspace& workspace,
        const ArtifactMakepkgContext& context);
    friend ValidatedPackageArtifactPath validate_post_build_package_artifact(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected);
    friend ValidatedPackageArtifactSet validate_post_build_package_artifacts(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected);
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
    friend void require_artifact_workspace_identity_for_test(
        const ArtifactWorkspace& workspace,
        std::uintmax_t expected_effective_user);
    friend ValidatedPackageArtifactPath
    validate_post_build_package_artifact_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected,
        std::uintmax_t expected_effective_user);
    friend ValidatedPackageArtifactSet
    validate_post_build_package_artifacts_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected,
        std::uintmax_t expected_effective_user);
    friend ValidatedPackageArtifactSet
    validate_post_build_package_artifacts_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected,
        std::uintmax_t expected_workspace_owner,
        std::uintmax_t expected_artifact_owner,
        std::uintmax_t expected_signature_owner);
#endif

public:
    ArtifactWorkspace(const ArtifactWorkspace&) = delete;
    ArtifactWorkspace& operator=(const ArtifactWorkspace&) = delete;
    ArtifactWorkspace(ArtifactWorkspace&& other) noexcept;
    ArtifactWorkspace& operator=(ArtifactWorkspace&&) = delete;
    ~ArtifactWorkspace() noexcept;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    const std::filesystem::path& canonical_path() const noexcept {
        return canonical_path_;
    }

    void require_unchanged_identity() const;

    // diagnostic retention後も明示cleanupは可能。次回invocationは常に別workspaceを作る。
    void retain_for_diagnostics() noexcept;
    void cleanup();
};

ArtifactWorkspace create_artifact_workspace(ValidatedPrivateCacheRoot root);

// source/ambient PKGDESTはvalueのempty/nonemptyに関係なくownership conflictとする。
void require_unclaimed_artifact_pkgdest(
    const SourceBuildEnvironment& environment);

// build-only makepkgへ渡せるoptionを、install/remove系optionから型で分離する。
struct ArtifactMakepkgBuildOptions {
    bool no_confirm = false;
    bool rebuild = false;
    bool clean_build = false;
};

// packagelistと後続build-only makepkgが共有するcheckout/environment境界。
// owned PKGDESTはsource assignmentの順序を保った末尾へ一度だけ追加する。
class ArtifactMakepkgContext final {
    RetainedTrustedCacheDirectory checkout_;
    std::unique_ptr<ProductionArtifactSourceTree> production_source_tree_;
    SourceBuildEnvironment command_environment_;
    SourceEnvironmentEmptyValuePolicy empty_value_policy_;
    std::shared_ptr<const ArtifactMakepkgContextProvenance> provenance_;
    std::filesystem::path workspace_path_;
    std::uintmax_t workspace_device_ = 0;
    std::uintmax_t workspace_inode_ = 0;

    ArtifactMakepkgContext(
        RetainedTrustedCacheDirectory checkout,
        SourceBuildEnvironment command_environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy,
        std::filesystem::path workspace_path,
        std::uintmax_t workspace_device,
        std::uintmax_t workspace_inode);
    ArtifactMakepkgContext(
        RetainedTrustedCacheDirectory checkout,
        ProductionArtifactSourceTree source_tree,
        SourceBuildEnvironment command_environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy,
        std::filesystem::path workspace_path,
        std::uintmax_t workspace_device,
        std::uintmax_t workspace_inode);

    void require_unchanged_checkout() const;
    void require_matching_workspace(const ArtifactWorkspace& workspace) const;
    CapturedCommandResult capture_makepkg_output(
        const ArtifactWorkspace& workspace,
        const std::vector<std::string>& makepkg_arguments) const;
    std::string makepkg_command(
        const std::vector<std::string>& makepkg_arguments) const;

    friend ArtifactMakepkgContext prepare_artifact_makepkg_context(
        const ValidatedCachePath& checkout,
        const ArtifactWorkspace& workspace,
        const SourceBuildEnvironment& environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy);
    friend ArtifactMakepkgContext prepare_artifact_makepkg_context(
        ProductionArtifactSourceTree source_tree,
        const ArtifactWorkspace& workspace,
        const SourceBuildEnvironment& environment,
        SourceEnvironmentEmptyValuePolicy empty_value_policy);
    friend ExpectedPackageArtifactPath query_makepkg_packagelist(
        const ArtifactWorkspace& workspace,
        const ArtifactMakepkgContext& context);
    friend ExpectedPackageArtifactSet query_makepkg_packagelist_set(
        const ArtifactWorkspace& workspace,
        const ArtifactMakepkgContext& context);
    friend class ExpectedPackageArtifactPath;
    friend class ExpectedPackageArtifactSet;

public:
    ArtifactMakepkgContext(const ArtifactMakepkgContext&) = delete;
    ArtifactMakepkgContext& operator=(const ArtifactMakepkgContext&) = delete;
    ArtifactMakepkgContext(ArtifactMakepkgContext&& other) noexcept;
    ArtifactMakepkgContext& operator=(ArtifactMakepkgContext&&) = delete;
    ~ArtifactMakepkgContext() noexcept;

    const std::filesystem::path& checkout_path() const noexcept {
        return checkout_.path().canonical_path();
    }

    const std::filesystem::path& workspace_path() const noexcept {
        return workspace_path_;
    }

    const SourceBuildEnvironment& command_environment() const noexcept {
        return command_environment_;
    }

    std::string command_environment_prefix() const;

    // query由来expectedとのprovenance照合を必須にし、exact build-only argvだけを
    // 公開する。--needed/-r/-iを受けるarbitrary argv境界にはしない。
    // LANDMINE(#404): build-onlyはartifactをinstallしないという意味であり、
    // current `-sc`の`-s`が行い得るdependency installをMoguet-ownedにはしない。
    // pre/post package snapshotだけからcausal ownershipへ昇格させてはならない。
    int run_makepkg_build_only(
        const ArtifactWorkspace& workspace,
        const ExpectedPackageArtifactPath& expected,
        const ArtifactMakepkgBuildOptions& options) const;

    int run_makepkg_build_only(
        const ArtifactWorkspace& workspace,
        const ExpectedPackageArtifactSet& expected,
        const ArtifactMakepkgBuildOptions& options) const;
};

ArtifactMakepkgContext prepare_artifact_makepkg_context(
    const ValidatedCachePath& checkout,
    const ArtifactWorkspace& workspace,
    const SourceBuildEnvironment& environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy);

ArtifactMakepkgContext prepare_artifact_makepkg_context(
    ProductionArtifactSourceTree source_tree,
    const ArtifactWorkspace& workspace,
    const SourceBuildEnvironment& environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy);

// makepkg --packagelistで宣言されたbuild前のexpected path。
// filesystem artifactの検証済みcapabilityではない。
class ExpectedPackageArtifactPath final {
    std::filesystem::path path_;
    std::string leaf_name_;
    std::uintmax_t workspace_device_ = 0;
    std::uintmax_t workspace_inode_ = 0;
    bool makepkg_context_bound_ = false;
    std::shared_ptr<const ArtifactMakepkgContextProvenance>
        makepkg_context_provenance_;

    ExpectedPackageArtifactPath(
        std::filesystem::path path, std::string leaf_name,
        std::uintmax_t workspace_device,
        std::uintmax_t workspace_inode);

    void bind_makepkg_context(const ArtifactMakepkgContext& context);
    void require_matching_makepkg_context(
        const ArtifactMakepkgContext& context) const;

    friend ExpectedPackageArtifactPath validate_makepkg_packagelist_output(
        const ArtifactWorkspace& workspace, const std::string& raw_output);
    friend ExpectedPackageArtifactPath query_makepkg_packagelist(
        const ArtifactWorkspace& workspace,
        const ArtifactMakepkgContext& context);
    friend class ValidatedPackageArtifactPath;
    friend class ArtifactMakepkgContext;
    friend ValidatedPackageArtifactPath validate_post_build_package_artifact(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected);
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
    friend ValidatedPackageArtifactPath
    validate_post_build_package_artifact_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected,
        std::uintmax_t expected_effective_user);
#endif

public:
    const std::filesystem::path& path() const noexcept {
        return path_;
    }
};

ExpectedPackageArtifactPath validate_makepkg_packagelist_output(
    const ArtifactWorkspace& workspace, const std::string& raw_output);

ExpectedPackageArtifactPath query_makepkg_packagelist(
    const ArtifactWorkspace& workspace,
    const ArtifactMakepkgContext& context);

// makepkg --packagelistのrecord順と、一つのworkspace/context lineageを
// set全体で保持するmultiple expected capability。
class ExpectedPackageArtifactSet final {
    struct Entry {
        std::filesystem::path path;
        std::string leaf_name;
    };

    std::vector<Entry> entries_;
    std::uintmax_t workspace_device_ = 0;
    std::uintmax_t workspace_inode_ = 0;
    bool makepkg_context_bound_ = false;
    std::shared_ptr<const ArtifactMakepkgContextProvenance>
        makepkg_context_provenance_;

    ExpectedPackageArtifactSet(
        std::vector<Entry> entries,
        std::uintmax_t workspace_device,
        std::uintmax_t workspace_inode) noexcept;

    void bind_makepkg_context(const ArtifactMakepkgContext& context);
    void require_matching_makepkg_context(
        const ArtifactMakepkgContext& context) const;
    void require_matching_workspace(
        const ArtifactWorkspace& workspace) const;

    friend ExpectedPackageArtifactSet
    validate_makepkg_packagelist_output_set(
        const ArtifactWorkspace& workspace,
        const std::string& raw_output);
    friend ExpectedPackageArtifactSet query_makepkg_packagelist_set(
        const ArtifactWorkspace& workspace,
        const ArtifactMakepkgContext& context);
    friend class ArtifactMakepkgContext;
    friend class ValidatedPackageArtifactSet;

public:
    ExpectedPackageArtifactSet(const ExpectedPackageArtifactSet&) = default;
    ExpectedPackageArtifactSet& operator=(
        const ExpectedPackageArtifactSet&) = default;
    ExpectedPackageArtifactSet(
        ExpectedPackageArtifactSet&&) noexcept = default;
    ExpectedPackageArtifactSet& operator=(
        ExpectedPackageArtifactSet&&) noexcept = default;
    ~ExpectedPackageArtifactSet() = default;

    std::size_t size() const noexcept {
        return entries_.size();
    }

    const std::filesystem::path& path_at(std::size_t index) const;
};

ExpectedPackageArtifactSet validate_makepkg_packagelist_output_set(
    const ArtifactWorkspace& workspace,
    const std::string& raw_output);

ExpectedPackageArtifactSet query_makepkg_packagelist_set(
    const ArtifactWorkspace& workspace,
    const ArtifactMakepkgContext& context);

// post-build filesystem validationを通過し、workspace ownershipも保持するcapability。
// PR3のidentity/pacman executorはraw path overloadを持たず、この型だけを受け取る。
class ValidatedPackageArtifactPath final {
    ArtifactWorkspace workspace_;
    std::filesystem::path path_;
    std::string leaf_name_;
    int artifact_descriptor_ = -1;
    std::uintmax_t device_ = 0;
    std::uintmax_t inode_ = 0;
    std::uintmax_t owner_ = 0;

    ValidatedPackageArtifactPath(
        ArtifactWorkspace&& workspace, std::filesystem::path path,
        std::string leaf_name, int artifact_descriptor,
        std::uintmax_t device, std::uintmax_t inode,
        std::uintmax_t owner) noexcept;

    void require_validity_for_owner(
        std::uintmax_t expected_effective_user) const;
    static ValidatedPackageArtifactPath validate_for_owners(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected,
        std::uintmax_t expected_workspace_owner,
        std::uintmax_t expected_artifact_owner);

    friend ValidatedPackageArtifactPath validate_post_build_package_artifact(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected);
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
    friend ValidatedPackageArtifactPath
    validate_post_build_package_artifact_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactPath& expected,
        std::uintmax_t expected_effective_user);
#endif

public:
    ValidatedPackageArtifactPath(const ValidatedPackageArtifactPath&) = delete;
    ValidatedPackageArtifactPath& operator=(
        const ValidatedPackageArtifactPath&) = delete;
    ValidatedPackageArtifactPath(
        ValidatedPackageArtifactPath&& other) noexcept;
    ValidatedPackageArtifactPath& operator=(
        ValidatedPackageArtifactPath&&) = delete;
    ~ValidatedPackageArtifactPath() noexcept;

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    const std::filesystem::path& workspace_path() const noexcept {
        return workspace_.path();
    }

    void require_validity() const;
    void retain_workspace_for_diagnostics() noexcept;
    void cleanup_workspace();
};

// validation failureではworkspaceを移動せず、callerがdiagnostic retentionへ遷移できる。
// success時だけreturned capabilityへcleanup ownershipを移す。
ValidatedPackageArtifactPath validate_post_build_package_artifact(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactPath& expected);

// 複数のvalidated artifact recordとexactly one workspaceを一括所有する。
// 個別recordへcleanup/retention ownershipを分配しない。
class ValidatedPackageArtifactSet final {
    enum class OwnershipState {
        Active,
        WorkspaceCleanupPending,
        Inactive,
    };

    struct Record {
        std::filesystem::path path;
        std::string leaf_name;
        int artifact_descriptor = -1;
        std::uintmax_t artifact_device = 0;
        std::uintmax_t artifact_inode = 0;
        std::uintmax_t artifact_owner = 0;
        bool has_signature = false;
        int signature_descriptor = -1;
        std::uintmax_t signature_device = 0;
        std::uintmax_t signature_inode = 0;
        std::uintmax_t signature_owner = 0;
        int sealed_artifact_descriptor = -1;
        int sealed_signature_descriptor = -1;
        std::filesystem::path metadata_path;

        Record(
            std::filesystem::path artifact_path,
            std::string artifact_leaf,
            int retained_artifact_descriptor,
            std::uintmax_t retained_artifact_device,
            std::uintmax_t retained_artifact_inode,
            std::uintmax_t retained_artifact_owner,
            bool retained_signature,
            int retained_signature_descriptor,
            std::uintmax_t retained_signature_device,
            std::uintmax_t retained_signature_inode,
            std::uintmax_t retained_signature_owner) noexcept;
        Record(const Record&) = delete;
        Record& operator=(const Record&) = delete;
        Record(Record&& other) noexcept;
        Record& operator=(Record&&) = delete;
        ~Record() noexcept;
    };

    // LANDMINE(#268): 通常のmember破棄でもrecord FDを閉じてからworkspace
    // cleanupへ進むため、records_をworkspace_より後ろへ保つ。
    ArtifactWorkspace workspace_;
    std::vector<Record> records_;
    int install_input_descriptor_ = -1;
    OwnershipState ownership_state_ = OwnershipState::Active;

    ValidatedPackageArtifactSet(
        ArtifactWorkspace&& workspace,
        std::vector<Record>&& records) noexcept;

    void require_active() const;
    void require_workspace_ownership() const;
    void require_validity_for_owner(
        std::uintmax_t expected_effective_user) const;
    static ValidatedPackageArtifactSet validate_for_owners(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected,
        std::uintmax_t expected_workspace_owner,
        std::uintmax_t expected_artifact_owner,
        std::uintmax_t expected_signature_owner);

    friend ValidatedPackageArtifactSet validate_post_build_package_artifacts(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected);
    // Dedicated SourceArtifactInstall transport may snapshot only retained
    // descriptors selected by the closed PackageBase install capability.
    friend class SourceArtifactInstallTrustedTransport;
    // Retained sealed concatenation, ordered archive then adjacent signature.
    int create_install_input(const std::vector<std::size_t>& indices);
    friend class PreparedPackageBaseArtifactInstall;
#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
    friend ValidatedPackageArtifactSet
    validate_post_build_package_artifacts_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected,
        std::uintmax_t expected_effective_user);
    friend ValidatedPackageArtifactSet
    validate_post_build_package_artifacts_for_test(
        ArtifactWorkspace&& workspace,
        const ExpectedPackageArtifactSet& expected,
        std::uintmax_t expected_workspace_owner,
        std::uintmax_t expected_artifact_owner,
        std::uintmax_t expected_signature_owner);
#endif

public:
    ValidatedPackageArtifactSet(
        const ValidatedPackageArtifactSet&) = delete;
    ValidatedPackageArtifactSet& operator=(
        const ValidatedPackageArtifactSet&) = delete;
    ValidatedPackageArtifactSet(
        ValidatedPackageArtifactSet&& other) noexcept;
    ValidatedPackageArtifactSet& operator=(
        ValidatedPackageArtifactSet&&) = delete;
    ~ValidatedPackageArtifactSet() noexcept;

    std::size_t size() const;
    const std::filesystem::path& path_at(std::size_t index) const;
    // Freeze before querying metadata. Original named/inode authority remains
    // independent and must still match these bytes immediately before install.
    void bind_install_content();
    void require_install_content() const;
    const std::filesystem::path& metadata_path_at(std::size_t index) const;
    const std::filesystem::path& workspace_path() const;

    void require_validity() const;
    void retain_workspace_for_diagnostics();
    void cleanup_workspace();
};

// validation failureではworkspaceをconsumeせず、全件成功後だけaggregateへmoveする。
ValidatedPackageArtifactSet validate_post_build_package_artifacts(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected);

#ifdef MOGUET_ENABLE_ARTIFACT_WORKSPACE_TEST_HOOKS
void require_artifact_workspace_identity_for_test(
    const ArtifactWorkspace& workspace,
    std::uintmax_t expected_effective_user);

ValidatedPackageArtifactPath validate_post_build_package_artifact_for_test(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactPath& expected,
    std::uintmax_t expected_artifact_owner);

using MultipleArtifactValidationObserverForTest =
    void (*)(const std::filesystem::path& workspace_path);

void set_multiple_artifact_validation_observer_for_test(
    MultipleArtifactValidationObserverForTest observer);

using MultipleArtifactCleanupObserverForTest = void (*)(
    const std::filesystem::path& workspace_path,
    const std::vector<int>& retained_descriptors);

void set_multiple_artifact_cleanup_observer_for_test(
    MultipleArtifactCleanupObserverForTest observer);

using ArtifactWorkspaceCreationObserverForTest =
    void (*)(const std::filesystem::path& workspace_path);

void set_artifact_workspace_creation_observer_for_test(
    ArtifactWorkspaceCreationObserverForTest observer);

using ArtifactWorkspaceCleanupPreDeleteObserverForTest =
    void (*)(const std::filesystem::path& workspace_path);

void set_artifact_workspace_cleanup_pre_delete_observer_for_test(
    ArtifactWorkspaceCleanupPreDeleteObserverForTest observer);

// openat2(RESOLVE_NO_XDEV)のrequestとmount-boundary refusalを、mount権限
// なしでcleanup deletion orderまで検証するtest seam。
using ArtifactWorkspaceCleanupChildOpenForTest = int (*)(
    int parent_descriptor, const std::string& leaf_name,
    std::uint64_t flags, std::uint64_t resolve);

void set_artifact_workspace_cleanup_child_open_for_test(
    ArtifactWorkspaceCleanupChildOpenForTest open_child) noexcept;

ValidatedPackageArtifactSet validate_post_build_package_artifacts_for_test(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected,
    std::uintmax_t expected_artifact_owner);

ValidatedPackageArtifactSet validate_post_build_package_artifacts_for_test(
    ArtifactWorkspace&& workspace,
    const ExpectedPackageArtifactSet& expected,
    std::uintmax_t expected_workspace_owner,
    std::uintmax_t expected_artifact_owner,
    std::uintmax_t expected_signature_owner);
#endif
