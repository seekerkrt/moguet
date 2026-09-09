#pragma once

#include "aur_update_execution_preflight.hpp"
#include "build_plan_artifact_target_projection.hpp"
#include "commands_inspect.hpp"
#include "commands_sync.hpp"
#include "local_source_build.hpp"
#include "source_install.hpp"
#include "system_source_upgrade.hpp"
#include "unified_plan_observation.hpp"
#include "upgrade_all_operation.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct AurUpdateQueryResult;
struct RegisteredAurDevelObservation;
struct AurUpdateSourceBuildPreparation;
class PreparedFilteredAurUpdateOperation;
class PreparedUpgradeAllAurPreflight;
struct SystemAurUpdateDryRunObservation;
class SystemAurUpdateUnifiedPlanProjection;
struct SystemAurUpdateUnifiedPlanProjectionTestAccess;

struct FetchUnifiedPlanProjectionInput {
    std::reference_wrapper<const FetchPreparation> source;
};

using SyncInstallUnifiedPlanProjectionSource = std::variant<
    std::reference_wrapper<const PreparedSyncInstall>,
    std::reference_wrapper<const SyncInstallPreparationFailure>>;

struct SyncInstallUnifiedPlanProjectionInput {
    SyncInstallUnifiedPlanProjectionSource source;
};

using RemoteSourceBuildUnifiedPlanProjectionSource = std::variant<
    std::reference_wrapper<const PreparedRemoteSourceBuild>,
    std::reference_wrapper<const RemoteSourceBuildPlanFailure>>;

struct RemoteSourceBuildUnifiedPlanProjectionInput {
    RemoteSourceBuildUnifiedPlanProjectionSource source;
};
// Ready prepared ownerとBlocked production resultだけを受けるread-only sum。
// reference_wrapperによりtemporary authorityは拒否し、fake work itemを
// Blocked armへ要求しない。すべてのownerはprojectionを破棄するまでmove、
// mutation、destroyしてはならない。
using RootPackageUnifiedPlanProjectionSource = std::variant<
    std::reference_wrapper<const PreparedRootPackageInstall>,
    std::reference_wrapper<const RootPackageInstallPreparationFailure>>;

struct RootPackageUnifiedPlanProjectionInput {
    RootPackageUnifiedPlanProjectionSource source;
};

struct LocalSourceBuildPlanFailureProjectionInput {
    std::reference_wrapper<const LocalSourceRoot> source_root;
    std::reference_wrapper<const LocalBuildPlan> local_build_plan;
};

struct LocalSourceMetadataEvaluationProjectionInput {
    std::reference_wrapper<const LocalSourceRoot> source_root;
};

using LocalSourceUnifiedPlanProjectionSource = std::variant<
    std::reference_wrapper<const LocalSourceBuildProjectionAuthority>,
    LocalSourceBuildPlanFailureProjectionInput,
    LocalSourceMetadataEvaluationProjectionInput>;

struct LocalSourceUnifiedPlanProjectionInput {
    LocalSourceUnifiedPlanProjectionSource source;
    bool needed = false;
};

struct AurUpdateUnifiedPlanProjectionInput {
    std::reference_wrapper<const AurUpdateQueryResult> query_result;
    std::reference_wrapper<const AurUpdateExecutionPreflight> preflight;
    bool needed = false;
    std::optional<std::reference_wrapper<
        const AurUpdateSourceBuildPreparation>>
        source_build_preparation = std::nullopt;
    std::optional<std::reference_wrapper<
        const PreparedFilteredAurUpdateOperation>>
        filtered_operation = std::nullopt;
};

using SystemSourceUpgradeUnifiedPlanProjectionSource = std::variant<
    std::reference_wrapper<const SystemSourceUpgradeProjectionAuthority>,
    std::reference_wrapper<const SystemSourceUpgradeResult>>;

struct SystemSourceUpgradeUnifiedPlanProjectionInput {
    SystemSourceUpgradeUnifiedPlanProjectionSource source;
    const std::vector<RegisteredAurDevelObservation>* registered_devel = nullptr;
};

using UpgradeAllUnifiedPlanProjectionSource = std::variant<
    std::reference_wrapper<const UpgradeAllOperationProjectionAuthority>,
    std::reference_wrapper<const UpgradeAllOperationResult>>;

struct UpgradeAllUnifiedPlanProjectionInput {
    UpgradeAllUnifiedPlanProjectionSource source;
    std::optional<std::reference_wrapper<const AurUpdateQueryResult>>
        aur_query_result = std::nullopt;
    std::optional<std::reference_wrapper<const AurUpdateExecutionPreflight>>
        aur_preflight = std::nullopt;
    std::optional<std::reference_wrapper<
        const std::vector<UpgradeAllOperationIssue>>>
        issues = std::nullopt;
    std::optional<std::reference_wrapper<
        const AurUpdateSourceBuildPreparation>>
        aur_source_build_preparation = std::nullopt;
    std::optional<std::reference_wrapper<
        const PreparedUpgradeAllAurPreflight>>
        aur_operation_preflight = std::nullopt;
    const std::vector<RegisteredAurDevelObservation>* registered_devel = nullptr;
};

// BuildPlan routeのRequiredPackageArtifactTargetはowned projection resultまたは
// route-specific reason viewへ残し、prepared system/source routeはproduction
// work itemを直接borrowする。
// result/observationはcopyできず、内部targetをborrowするviewをbundle外へ
// value escapeさせない。このbundleはexternal production authorityをcopyしない。
class UnifiedPlanProjection final {
public:
    UnifiedPlanProjection(const UnifiedPlanProjection&) = delete;
    UnifiedPlanProjection& operator=(const UnifiedPlanProjection&) = delete;
    UnifiedPlanProjection(UnifiedPlanProjection&&) = delete;
    UnifiedPlanProjection& operator=(UnifiedPlanProjection&&) = delete;
    ~UnifiedPlanProjection() = default;

    [[nodiscard]] const UnifiedPlanObservationResult& observation_result()
        const noexcept {
        return observation_result_.value();
    }

private:
    explicit UnifiedPlanProjection(
        std::vector<BuildPlanArtifactTargetProjectionResult>
            artifact_target_projections,
        std::vector<ProjectedBuildPlanArtifactTargets>
            route_artifact_targets)
        : artifact_target_projections_(
              std::move(artifact_target_projections)),
          route_artifact_targets_(std::move(route_artifact_targets)) {
    }

    std::vector<BuildPlanArtifactTargetProjectionResult>
        artifact_target_projections_;
    std::vector<ProjectedBuildPlanArtifactTargets> route_artifact_targets_;
    std::optional<UnifiedPlanObservationResult> observation_result_;

    static std::unique_ptr<UnifiedPlanProjection> make(
        std::vector<BuildPlanArtifactTargetProjectionResult>
            artifact_target_projections,
        UnifiedPlanObservationInput observation_input);
    static std::unique_ptr<UnifiedPlanProjection> make(
        std::vector<BuildPlanArtifactTargetProjectionResult>
            artifact_target_projections,
        std::vector<ProjectedBuildPlanArtifactTargets>
            route_artifact_targets,
        UnifiedPlanObservationInput observation_input);

    friend std::unique_ptr<UnifiedPlanProjection>
    project_root_package_unified_plan(
        RootPackageUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_fetch_unified_plan(FetchUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_sync_install_unified_plan(
        SyncInstallUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_remote_source_build_unified_plan(
        RemoteSourceBuildUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_local_source_unified_plan(
        LocalSourceUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_aur_update_unified_plan(
        AurUpdateUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_filtered_aur_update_unified_plan(
        const PreparedFilteredAurUpdateOperation& prepared);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_system_source_upgrade_unified_plan(
        SystemSourceUpgradeUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_upgrade_all_unified_plan(
        UpgradeAllUnifiedPlanProjectionInput input);
    friend std::unique_ptr<UnifiedPlanProjection>
    project_upgrade_all_unified_plan(
        const UpgradeAllOperationProjectionAuthority& prepared,
        const PreparedUpgradeAllAurPreflight& aur_preflight,
        const std::vector<RegisteredAurDevelObservation>* registered_devel);
    friend std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
    project_system_aur_update_unified_plan(
        const SystemAurUpdateDryRunObservation& observation);
    friend struct SystemAurUpdateUnifiedPlanProjectionTestAccess;
};

enum class SystemAurUpdateUnifiedPlanStatus {
    Ready,
    Blocked,
};

enum class SystemAurUpdateUnifiedPlanMode {
    Auto,
    RepoOnly,
};

enum class SystemAurUpdateUnifiedPlanPhase {
    RepositorySystemTransactionIntent,
    CurrentForeignInventoryObservation,
    CurrentNormalAurAssessment,
    PotentialLaterAurTransactions,
};

enum class SystemAurUpdateUnifiedPlanFreshness {
    CurrentInstalledState,
};

enum class SystemAurUpdateUnifiedPlanActualRefresh {
    AfterRepositorySuccess,
};

enum class SystemAurUpdateUnifiedPlanTransactionRelationship {
    SeparateSequentialTransactions,
};

// Ordinary system+AUR dry-run keeps non-blocking RequiresCheck observations
// as typed route data. Rendering is a one-way projection and never decides
// whether a target is independent from these copied fields.
struct SystemAurUpdateRequiresCheckAttention {
    std::size_t update_plan_index = 0;
    std::string package_name;
    std::string package_base;
    DevelRequiresCheckReason reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    AurUpdateExecutionSkipKind skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
    AurUpdateEffectiveState observation_state =
        AurUpdateEffectiveState::RequiresCheck;

    bool operator==(
        const SystemAurUpdateRequiresCheckAttention&) const = default;
};

// Route-specific aggregate keeps the repository system intent observable even
// when the current-state AUR child is Blocked. The child observations remain
// separate, so no atomic transaction is projected.
class SystemAurUpdateUnifiedPlanProjection final {
public:
    SystemAurUpdateUnifiedPlanProjection(
        const SystemAurUpdateUnifiedPlanProjection&) = delete;
    SystemAurUpdateUnifiedPlanProjection& operator=(
        const SystemAurUpdateUnifiedPlanProjection&) = delete;
    SystemAurUpdateUnifiedPlanProjection(
        SystemAurUpdateUnifiedPlanProjection&&) = delete;
    SystemAurUpdateUnifiedPlanProjection& operator=(
        SystemAurUpdateUnifiedPlanProjection&&) = delete;
    ~SystemAurUpdateUnifiedPlanProjection() = default;

    SystemAurUpdateUnifiedPlanStatus status() const noexcept {
        return status_;
    }
    SystemAurUpdateUnifiedPlanMode mode() const noexcept {
        return mode_;
    }
    const std::vector<SystemAurUpdateUnifiedPlanPhase>& phases()
        const noexcept {
        return phases_;
    }
    const UnifiedPlanProjection& repository_projection() const noexcept {
        return *repository_projection_;
    }
    const UnifiedPlanProjection* aur_projection() const noexcept {
        return aur_projection_.get();
    }
    const std::vector<SystemAurUpdateRequiresCheckAttention>&
    requires_check_attentions() const noexcept {
        return requires_check_attentions_;
    }
    std::optional<SystemAurUpdateUnifiedPlanFreshness> freshness()
        const noexcept {
        return freshness_;
    }
    std::optional<SystemAurUpdateUnifiedPlanActualRefresh> actual_refresh()
        const noexcept {
        return actual_refresh_;
    }
    std::optional<SystemAurUpdateUnifiedPlanTransactionRelationship>
    transaction_relationship() const noexcept {
        return transaction_relationship_;
    }

private:
    SystemAurUpdateUnifiedPlanProjection(
        SystemAurUpdateUnifiedPlanStatus status,
        SystemAurUpdateUnifiedPlanMode mode,
        std::vector<SystemAurUpdateUnifiedPlanPhase> phases,
        std::unique_ptr<UnifiedPlanProjection> repository_projection,
        std::unique_ptr<UnifiedPlanProjection> aur_projection,
        std::vector<SystemAurUpdateRequiresCheckAttention>
            requires_check_attentions,
        std::optional<SystemAurUpdateUnifiedPlanFreshness> freshness,
        std::optional<SystemAurUpdateUnifiedPlanActualRefresh>
            actual_refresh,
        std::optional<SystemAurUpdateUnifiedPlanTransactionRelationship>
            transaction_relationship)
        : status_(status), mode_(mode), phases_(std::move(phases)),
          repository_projection_(std::move(repository_projection)),
          aur_projection_(std::move(aur_projection)),
          requires_check_attentions_(
              std::move(requires_check_attentions)),
          freshness_(freshness), actual_refresh_(actual_refresh),
          transaction_relationship_(transaction_relationship) {
    }

    SystemAurUpdateUnifiedPlanStatus status_;
    SystemAurUpdateUnifiedPlanMode mode_;
    std::vector<SystemAurUpdateUnifiedPlanPhase> phases_;
    std::unique_ptr<UnifiedPlanProjection> repository_projection_;
    std::unique_ptr<UnifiedPlanProjection> aur_projection_;
    std::vector<SystemAurUpdateRequiresCheckAttention>
        requires_check_attentions_;
    std::optional<SystemAurUpdateUnifiedPlanFreshness> freshness_;
    std::optional<SystemAurUpdateUnifiedPlanActualRefresh> actual_refresh_;
    std::optional<SystemAurUpdateUnifiedPlanTransactionRelationship>
        transaction_relationship_;

    friend std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
    project_system_aur_update_unified_plan(
        const SystemAurUpdateDryRunObservation& observation);
    friend struct SystemAurUpdateUnifiedPlanProjectionTestAccess;
};

std::unique_ptr<UnifiedPlanProjection> project_root_package_unified_plan(
    RootPackageUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_fetch_unified_plan(
    FetchUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_sync_install_unified_plan(
    SyncInstallUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection>
project_remote_source_build_unified_plan(
    RemoteSourceBuildUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_local_source_unified_plan(
    LocalSourceUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_aur_update_unified_plan(
    AurUpdateUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_filtered_aur_update_unified_plan(
    const PreparedFilteredAurUpdateOperation& prepared);
std::unique_ptr<UnifiedPlanProjection> project_system_source_upgrade_unified_plan(
    SystemSourceUpgradeUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_upgrade_all_unified_plan(
    UpgradeAllUnifiedPlanProjectionInput input);
std::unique_ptr<UnifiedPlanProjection> project_upgrade_all_unified_plan(
    const UpgradeAllOperationProjectionAuthority& prepared,
    const PreparedUpgradeAllAurPreflight& aur_preflight,
    const std::vector<RegisteredAurDevelObservation>* registered_devel = nullptr);
std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
project_system_aur_update_unified_plan(
    const SystemAurUpdateDryRunObservation& observation);
std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
project_system_aur_update_unified_plan(
    SystemAurUpdateDryRunObservation&& observation) = delete;
std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
project_system_aur_update_unified_plan(
    const SystemAurUpdateDryRunObservation&& observation) = delete;
