#pragma once

#include "artifact_identity_selection.hpp"
#include "artifact_workspace.hpp"
#include "local_dependency_plan_projection.hpp"
#include "local_source_root.hpp"
#include "local_source_workspace.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct LocalSourceBuildRequest;
class PreparedLocalSourceBuild;
class LocalSourceBuildResult;
class LocalSourceBuildProjectionAuthority;
class PreparedLocalRecipeBuild;

enum class LocalSourceBuildMetadataProvenance {
    ExistingSrcinfo,
    EvaluatedPkgbuild
};

// 採用metadataを、その評価に使ったone-off environmentとsource identityへ
// 束ねる。Slice 4のbuild-only ownerはこのproofだけを受け取り、callerが後から
// environmentを差し替える経路を持たない。
class LocalSourceBuildMetadata final {
    LocalPackageMetadata metadata_;
    SourceBuildEnvironment source_environment_;
    std::string effective_architecture_;
    LocalSourceBuildMetadataProvenance provenance_;
    LocalSourceDirectoryIdentity source_directory_identity_;
    LocalSourceFileSnapshot pkgbuild_snapshot_;

    LocalSourceBuildMetadata(
        LocalPackageMetadata metadata,
        SourceBuildEnvironment source_environment,
        std::string effective_architecture,
        LocalSourceBuildMetadataProvenance provenance,
        LocalSourceDirectoryIdentity source_directory_identity,
        LocalSourceFileSnapshot pkgbuild_snapshot) noexcept;

    friend LocalSourceBuildMetadata bind_existing_local_source_metadata(
        const LocalSourceRoot&, std::string);
    friend LocalSourceBuildMetadata bind_evaluated_local_source_metadata(
        const LocalSourceRoot&, SourceBuildEnvironment, std::string,
        std::string_view);
    friend LocalSourceBuildProjectionAuthority
    make_local_source_build_projection_authority(
        const LocalSourceRoot& source_root,
        const LocalBuildPlan& local_build_plan,
        const LocalSourceBuildMetadata& metadata);
    friend class PreparedLocalSourceBuild;
    friend class LocalSourceBuildProjectionAuthority;
    friend PreparedLocalSourceBuild prepare_local_source_build(
        struct LocalSourceBuildRequest request);

public:
    LocalSourceBuildMetadata(const LocalSourceBuildMetadata&) = default;
    LocalSourceBuildMetadata(LocalSourceBuildMetadata&&) noexcept = default;
    LocalSourceBuildMetadata& operator=(
        const LocalSourceBuildMetadata&) = delete;
    LocalSourceBuildMetadata& operator=(
        LocalSourceBuildMetadata&&) noexcept = delete;
    ~LocalSourceBuildMetadata() = default;

    const LocalPackageMetadata& metadata() const noexcept;
    const SourceBuildEnvironment& source_environment() const noexcept;
    const std::string& effective_architecture() const noexcept;
    LocalSourceBuildMetadataProvenance provenance() const noexcept;
    void require_matches(const LocalSourceRoot& source_root) const;
};

// Safeな既存.SRCINFOだけをenvironmentなしで採用する。
LocalSourceBuildMetadata bind_existing_local_source_metadata(
    const LocalSourceRoot& source_root,
    std::string effective_architecture);

// 明示許可済みmakepkg --printsrcinfo stdoutを、実行時environmentとsource
// identityへ束ねる。command実行自体はlocal_source_metadata_evaluationが所有する。
LocalSourceBuildMetadata bind_evaluated_local_source_metadata(
    const LocalSourceRoot& source_root,
    SourceBuildEnvironment source_environment,
    std::string effective_architecture,
    std::string_view evaluated_srcinfo);

// Local source、dependency plan、cache authorityを一つのbuild-only requestに
// 固定する。install policyやtransaction optionはこの境界へ持ち込まない。
struct LocalSourceBuildRequest {
    LocalSourceRoot source_root;
    LocalBuildPlan build_plan;
    ValidatedCacheRoot cache_root;
    LocalSourceBuildMetadata metadata;
    ArtifactMakepkgBuildOptions makepkg_options;
};

// PreparedLocalSourceBuild内でcoherence確認済みのrequestだけから生成する
// read-only seam。local source identity、採用metadata、environment/provenance、
// architecture、LocalBuildPlanを別invocationから組み合わせられない。
class LocalSourceBuildProjectionAuthority final {
public:
    LocalSourceBuildProjectionAuthority(
        const LocalSourceBuildProjectionAuthority&) = delete;
    LocalSourceBuildProjectionAuthority& operator=(
        const LocalSourceBuildProjectionAuthority&) = delete;
    LocalSourceBuildProjectionAuthority(
        LocalSourceBuildProjectionAuthority&&) noexcept = default;
    LocalSourceBuildProjectionAuthority& operator=(
        LocalSourceBuildProjectionAuthority&&) noexcept = default;
    ~LocalSourceBuildProjectionAuthority() = default;

    [[nodiscard]] const LocalSourceRoot& source_root() const noexcept {
        return source_root_.get();
    }
    [[nodiscard]] const LocalBuildPlan& local_build_plan() const noexcept {
        return local_build_plan_.get();
    }
    [[nodiscard]] const LocalPackageMetadata& accepted_metadata()
        const noexcept {
        return accepted_metadata_.get();
    }
    [[nodiscard]] const SourceBuildEnvironment& source_environment()
        const noexcept {
        return source_environment_.get();
    }
    [[nodiscard]] const std::string& effective_architecture()
        const noexcept {
        return effective_architecture_.get();
    }
    [[nodiscard]] LocalSourceBuildMetadataProvenance provenance()
        const noexcept {
        return provenance_;
    }
    [[nodiscard]] const LocalSourceDirectoryIdentity&
    source_directory_identity() const noexcept {
        return source_directory_identity_.get();
    }
    [[nodiscard]] const LocalSourceFileSnapshot& pkgbuild_snapshot()
        const noexcept {
        return pkgbuild_snapshot_.get();
    }
    [[nodiscard]] bool has_complete_identity() const noexcept {
        return !source_root().canonical_path().empty() &&
               !accepted_metadata().package_base.empty() &&
               !effective_architecture().empty() &&
               source_root().directory_identity() ==
                   source_directory_identity() &&
               source_root().pkgbuild() == pkgbuild_snapshot() &&
               accepted_metadata() ==
                   local_build_plan().local_metadata() &&
               effective_architecture() ==
                   local_build_plan().effective_architecture();
    }

private:
    explicit LocalSourceBuildProjectionAuthority(
        const LocalSourceBuildRequest& request) noexcept;
    LocalSourceBuildProjectionAuthority(
        const LocalSourceRoot& source_root,
        const LocalBuildPlan& local_build_plan,
        const LocalPackageMetadata& accepted_metadata,
        const SourceBuildEnvironment& source_environment,
        const std::string& effective_architecture,
        LocalSourceBuildMetadataProvenance provenance,
        const LocalSourceDirectoryIdentity& source_directory_identity,
        const LocalSourceFileSnapshot& pkgbuild_snapshot) noexcept
        : source_root_(source_root), local_build_plan_(local_build_plan),
          accepted_metadata_(accepted_metadata),
          source_environment_(source_environment),
          effective_architecture_(effective_architecture),
          provenance_(provenance),
          source_directory_identity_(source_directory_identity),
          pkgbuild_snapshot_(pkgbuild_snapshot) {
    }

    std::reference_wrapper<const LocalSourceRoot> source_root_;
    std::reference_wrapper<const LocalBuildPlan> local_build_plan_;
    std::reference_wrapper<const LocalPackageMetadata> accepted_metadata_;
    std::reference_wrapper<const SourceBuildEnvironment>
        source_environment_;
    std::reference_wrapper<const std::string> effective_architecture_;
    LocalSourceBuildMetadataProvenance provenance_;
    std::reference_wrapper<const LocalSourceDirectoryIdentity>
        source_directory_identity_;
    std::reference_wrapper<const LocalSourceFileSnapshot> pkgbuild_snapshot_;

    friend class PreparedLocalSourceBuild;
    friend LocalSourceBuildProjectionAuthority
    make_local_source_build_projection_authority(
        const LocalSourceRoot& source_root,
        const LocalBuildPlan& local_build_plan,
        const LocalSourceBuildMetadata& metadata);
    friend struct UnifiedPlanProjectionTestAccess;
};

// Existing .SRCINFOを採用したread-only dry-run route向け。cache/workspaceを
// 準備せず、production LocalSourceRoot/LocalBuildPlan/metadataの相関だけを
// projection authorityへ固定する。
LocalSourceBuildProjectionAuthority
make_local_source_build_projection_authority(
    const LocalSourceRoot& source_root,
    const LocalBuildPlan& local_build_plan,
    const LocalSourceBuildMetadata& metadata);

class PreparedLocalSourceBuild final {
    LocalSourceBuildRequest request_;
    // Only the recipe consumer can transfer an early snapshot. The ordinary
    // route still materializes its source after dependency preparation.
    std::optional<LocalSourceWorkspace> recipe_workspace_;
    std::string package_base_;
    std::vector<RequiredPackageArtifactTarget> required_targets_;
    LocalSourceBuildProjectionAuthority projection_authority_;

    PreparedLocalSourceBuild(
        LocalSourceBuildRequest request,
        std::string package_base,
        std::vector<RequiredPackageArtifactTarget>
            required_targets) noexcept;

    friend PreparedLocalSourceBuild prepare_local_source_build(
        LocalSourceBuildRequest request);
    friend class LocalSourceBuildResult;
    friend LocalSourceBuildResult execute_prepared_local_source_build(
        PreparedLocalSourceBuild prepared);
    friend class PreparedLocalRecipeBuild;

    static PreparedLocalSourceBuild from_recipe_candidate(
        LocalSourceBuildRequest request, LocalSourceWorkspace& workspace);

public:
    PreparedLocalSourceBuild(const PreparedLocalSourceBuild&) = delete;
    PreparedLocalSourceBuild& operator=(
        const PreparedLocalSourceBuild&) = delete;
    PreparedLocalSourceBuild(PreparedLocalSourceBuild&& other) noexcept;
    PreparedLocalSourceBuild& operator=(
        PreparedLocalSourceBuild&&) = delete;
    ~PreparedLocalSourceBuild() noexcept = default;

    [[nodiscard]] const LocalSourceBuildProjectionAuthority&
    projection_authority() const noexcept {
        return projection_authority_;
    }
};

enum class LocalSourceBuildFailurePhase {
    Preflight,
    SourceWorkspace,
    ArtifactWorkspace,
    BuildContext,
    Packagelist,
    Build,
    ArtifactValidation,
    ArtifactIdentity,
    ArtifactSelection,
    SourceCleanup,
};

// Stable phaseと、利用可能な場合だけbuild status / closed selection failure /
// local source failure / primary failure中のsecondary cleanup failureを保持する。
// retained workspace pathはdisplay専用であり、artifact pathやcleanup capabilityは
// errorへ公開しない。
class LocalSourceBuildPhaseError final : public std::runtime_error {
    LocalSourceBuildFailurePhase phase_;
    std::optional<int> build_exit_code_;
    std::optional<PackageBaseArtifactIdentitySelectionFailure>
        selection_failure_;
    std::optional<LocalSourceRootFailure> source_root_failure_;
    std::optional<LocalSourceWorkspaceFailure> source_workspace_failure_;
    std::optional<LocalSourceWorkspaceFailure> source_cleanup_failure_;
    std::optional<std::filesystem::path> retained_artifact_workspace_;

public:
    LocalSourceBuildPhaseError(
        LocalSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<int> build_exit_code = std::nullopt,
        std::optional<PackageBaseArtifactIdentitySelectionFailure>
            selection_failure = std::nullopt,
        std::optional<LocalSourceRootFailure> source_root_failure =
            std::nullopt,
        std::optional<LocalSourceWorkspaceFailure>
            source_workspace_failure = std::nullopt,
        std::optional<LocalSourceWorkspaceFailure>
            source_cleanup_failure = std::nullopt);
    LocalSourceBuildPhaseError(
        LocalSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::filesystem::path retained_artifact_workspace,
        std::optional<int> build_exit_code = std::nullopt,
        std::optional<PackageBaseArtifactIdentitySelectionFailure>
            selection_failure = std::nullopt,
        std::optional<LocalSourceRootFailure> source_root_failure =
            std::nullopt,
        std::optional<LocalSourceWorkspaceFailure>
            source_workspace_failure = std::nullopt,
        std::optional<LocalSourceWorkspaceFailure>
            source_cleanup_failure = std::nullopt);
    LocalSourceBuildPhaseError(
        const LocalSourceBuildPhaseError& primary,
        LocalSourceWorkspaceFailure source_cleanup_failure);

    LocalSourceBuildFailurePhase phase() const noexcept;
    std::optional<int> build_exit_code() const noexcept;
    const PackageBaseArtifactIdentitySelectionFailure* selection_failure()
        const noexcept;
    const LocalSourceRootFailure* source_root_failure() const noexcept;
    const LocalSourceWorkspaceFailure* source_workspace_failure()
        const noexcept;
    const LocalSourceWorkspaceFailure* source_cleanup_failure()
        const noexcept;
    // Display-only path; callers must not use it as filesystem authority.
    const std::filesystem::path* retained_artifact_workspace()
        const noexcept;
};

// Validated aggregateをprivateに所有し、publicにはstable artifact index付きの
// identity snapshotと明示cleanupだけを公開する。path/cleanup authorityやinstall
// directiveは外へ出さない。
class LocalSourceBuildResult final {
    PackageBaseArtifactIdentitySelectionSuccess selection_;
    ValidatedPackageArtifactSet artifacts_;

    LocalSourceBuildResult(
        PackageBaseArtifactIdentitySelectionSuccess selection,
        ValidatedPackageArtifactSet artifacts) noexcept;

    friend LocalSourceBuildResult execute_local_source_build(
        LocalSourceBuildRequest request);
    friend LocalSourceBuildResult execute_prepared_local_source_build(
        PreparedLocalSourceBuild prepared);
    friend class LocalSourceInstallAccess;

public:
    LocalSourceBuildResult(const LocalSourceBuildResult&) = delete;
    LocalSourceBuildResult& operator=(const LocalSourceBuildResult&) = delete;
    LocalSourceBuildResult(LocalSourceBuildResult&& other) noexcept = default;
    LocalSourceBuildResult& operator=(LocalSourceBuildResult&&) = delete;
    ~LocalSourceBuildResult() noexcept = default;

    const std::string& package_base() const noexcept;
    const std::vector<CorrelatedSelectedPackageArtifact>& selected_artifacts()
        const noexcept;
    const std::vector<CorrelatedUnselectedPackageArtifact>&
    unselected_artifacts() const noexcept;

    void cleanup_artifacts();
};

// 全static coherenceとsource identityをmutation-freeで固定する。remote
// dependency transactionより前にこのcapabilityを作る。
PreparedLocalSourceBuild prepare_local_source_build(
    LocalSourceBuildRequest request);

LocalSourceBuildResult execute_prepared_local_source_build(
    PreparedLocalSourceBuild prepared);

// local root unitだけをbuildし、remote dependency unitやinstall transactionは
// 実行しないcompatibility wrapper。success resultがvalidated aggregateを所有する。
LocalSourceBuildResult execute_local_source_build(
    LocalSourceBuildRequest request);
