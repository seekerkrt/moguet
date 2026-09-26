#pragma once

#include "aur_update_execution_preflight.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct AppConfig;
struct AurUpdateSourceBuildPreparation;
struct AurUpdateSourceBuildExecutionResult;

// AUR update work itemへsaved per-package customizationを適用するpolicy。
// Ignoreはpreference authorityをreadして結果を捨てるのではなく、authorityへ到達しない。
enum class SavedSourcePreferencePolicy {
    Strict,
    Ignore,
};

// upgrade-all固有型へ依存せず、明示sourceが満たすbuild unitの根拠をowned保持する。
struct AurUpdateExternalSatisfactionAttribution {
    std::vector<std::size_t> explicit_source_indexes;
    std::vector<std::string> source_identity_keys;
    std::optional<std::string> matched_package_name;
    std::optional<std::string> matched_package_base;

    bool operator==(
        const AurUpdateExternalSatisfactionAttribution&) const = default;
};

enum class AurUpdateBuildUnitSelectionStatus {
    SelectedForAurExecution,
    ExternallySatisfiedByExplicitSourcePackageBase,
};

// BuildPlan::order上のidentityと、denseなexecution selectionを分離して固定する。
struct AurUpdateBuildUnitSelectionEntry {
    std::size_t build_plan_order_index = 0;
    std::string package_base;
    std::vector<std::string> package_names;
    AurUpdateBuildUnitSelectionStatus status =
        AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution;
    std::optional<std::size_t> selected_execution_index;
    std::optional<AurUpdateExternalSatisfactionAttribution>
        external_satisfaction;

    bool operator==(const AurUpdateBuildUnitSelectionEntry&) const = default;
};

struct AurUpdateBuildUnitSelection {
    std::vector<AurUpdateBuildUnitSelectionEntry> entries;

    bool operator==(const AurUpdateBuildUnitSelection&) const = default;
};

enum class AurUpdatePreparationReason {
    None,
    BlockingPreflight,
    PreflightInconsistent,
    DevelRequiresCheckPolicyInconsistent,
    BuildPlanMissing,
    BuildPlanOrderEmpty,
    RootAttributionInconsistent,
    PackageTargetAttributionInconsistent,
    DesiredInstallReasonMissing,
    SourcePreferenceUnavailable,
    SourcePreferencePkgdestConflict,
    StaticWorkItemInvalid,
    PacmanDatabaseUnavailable,
    GenericPreparationInconsistent,
    BuildUnitSelectionInconsistent,
    ExternalSatisfactionInconsistent,
};

// preparation issueは表示文字列ではなくreasonと既存typed failureを正本にする。
struct AurUpdatePreparationIssue {
    AurUpdatePreparationReason reason = AurUpdatePreparationReason::None;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    std::optional<std::string> package_name;
    std::optional<std::string> package_base;
    std::optional<AurUpdateExecutionIssue> preflight_issue;
    std::optional<SourcePreferenceFailure> source_preference_failure;
    std::optional<PackageMetadataFailure> package_metadata_failure;
    std::optional<BuildPlanArtifactTargetProjectionIssue>
        build_plan_projection_issue;
    std::string diagnostic;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure;
};

// strict readerが返したwarningを、read順と対象attributionごとowned valueへ写す。
struct AurUpdatePreparationWarning {
    std::string preference_name;
    std::filesystem::path entry_path;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    std::string diagnostic;
};

// PackageBase aggregate前にchild単体でexact検証したupdate固有attribution。
struct AurUpdateRequiredTargetAttribution {
    RequiredPackageArtifactTarget required_target;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    std::vector<PackageRole> roles;
};

// BuildPlan::order上の1 work itemと、影響するupdate target/rootを固定する。
struct AurUpdatePreparedWorkItemAttribution {
    std::size_t invocation_work_item_index = 0;
    std::size_t build_plan_order_index = 0;
    std::string package_name;
    std::string package_base;
    std::vector<AurUpdateRequiredTargetAttribution>
        required_target_attributions;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
};

// BuildPlan unitごとのordered child attributionをexecution capabilityと独立に保持する。
struct AurUpdateProjectedBuildUnit {
    std::size_t build_plan_order_index = 0;
    std::string package_base;
    std::vector<AurUpdateRequiredTargetAttribution>
        required_target_attributions;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
};

// execution capabilityを持たないbuild unitも、元BuildPlan上の位置とroot/roleを失わない。
struct AurUpdateExternallySatisfiedBuildUnit {
    std::size_t build_plan_order_index = 0;
    // singular compatibility field。multipleではemptyにし、child vectorを正本にする。
    std::string package_name;
    std::string package_base;
    std::vector<std::string> plan_package_names;
    std::vector<AurUpdateRequiredTargetAttribution>
        required_target_attributions;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
    // singular compatibility fields。multipleのrole/reasonはchild vectorだけが正本。
    std::vector<PackageRole> roles;
    std::optional<DesiredInstallReason> desired_install_reason;
    AurUpdateExternalSatisfactionAttribution external_satisfaction;
};

// generic production invocationとupdate固有attributionを相関済みで所有する。
// POLICY(#267): execution-bearing generic invocationを外部へ公開せず、parallel
// vectorのcount/order/identityをpreparation後に書き換えられないようにする。
// moveはexecution capabilityを移し、move元を明示的にinvalid化する。
class AurUpgradePatchSet;
void attach_aur_upgrade_patch_candidates(AurUpdateSourceBuildPreparation&, const AurUpgradePatchSet&);

class PreparedAurUpdateSourceBuildInvocation final {
    PreparedProductionSourceBuildInvocation production_invocation_;
    std::vector<AurUpdatePreparedWorkItemAttribution>
        work_item_attributions_;
    bool valid_ = true;

    PreparedAurUpdateSourceBuildInvocation(
        PreparedProductionSourceBuildInvocation&& production_invocation,
        std::vector<AurUpdatePreparedWorkItemAttribution>&&
            work_item_attributions) noexcept;

    friend AurUpdateSourceBuildPreparation
    prepare_aur_update_source_build_invocation(
        const AurUpdateExecutionPreflight& preflight,
        const AurUpdateBuildUnitSelection& build_unit_selection,
        DevelRequiresCheckPolicy devel_requires_check_policy,
        SavedSourcePreferencePolicy saved_source_preference_policy,
        bool needed,
        const AppConfig& config);
    friend AurUpdateSourceBuildPreparation
    prepare_aur_update_source_build_invocation(
        const AurUpdateExecutionPreflight& preflight,
        DevelRequiresCheckPolicy devel_requires_check_policy,
        SavedSourcePreferencePolicy saved_source_preference_policy,
        bool needed,
        const AppConfig& config);
    friend struct AurUpdateSourceBuildPreparation;
    friend void attach_aur_upgrade_patch_candidates(AurUpdateSourceBuildPreparation&, const AurUpgradePatchSet&);
    friend AurUpdateSourceBuildExecutionResult
    execute_prepared_aur_update_source_build_invocation(
        PreparedAurUpdateSourceBuildInvocation invocation,
        const AppConfig& config);
    friend void seed_aur_update_source_build_cache(
        AurUpdateSourceBuildPreparation& preparation,
        const ValidatedCacheRoot& cache_root);

public:
    PreparedAurUpdateSourceBuildInvocation(
        const PreparedAurUpdateSourceBuildInvocation&) = delete;
    PreparedAurUpdateSourceBuildInvocation& operator=(
        const PreparedAurUpdateSourceBuildInvocation&) = delete;
    PreparedAurUpdateSourceBuildInvocation(
        PreparedAurUpdateSourceBuildInvocation&& other) noexcept;
    PreparedAurUpdateSourceBuildInvocation& operator=(
        PreparedAurUpdateSourceBuildInvocation&&) = delete;
    ~PreparedAurUpdateSourceBuildInvocation() noexcept = default;

#ifdef MOGUET_ENABLE_AUR_UPDATE_EXECUTION_PREPARATION_TEST_HOOKS
    // preparationのexact field assertion専用。production buildではgeneric
    // executorへ渡せる内部snapshotを公開しない。
    const PreparedProductionSourceBuildInvocation&
    production_invocation_for_test() const noexcept {
        return production_invocation_;
    }
#endif

    const std::vector<AurUpdatePreparedWorkItemAttribution>&
    work_item_attributions() const noexcept {
        return work_item_attributions_;
    }

    bool is_valid() const noexcept {
        return valid_;
    }
};

struct AurUpdateSourceBuildPreparation {
    std::vector<AurUpdatePreparationIssue> issues;
    std::vector<AurUpdatePreparationWarning> warnings;
    std::vector<AurUpdateExecutionTarget> affected_update_targets;
    std::vector<RootTargetIdentity> affected_roots;
    AurUpdateBuildUnitSelection build_unit_selection;
    std::vector<AurUpdateProjectedBuildUnit> projected_build_units;
    std::vector<AurUpdateExternallySatisfiedBuildUnit>
        externally_satisfied_build_units;
    std::optional<PreparedAurUpdateSourceBuildInvocation> invocation;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;

    bool is_prepared() const noexcept;
    bool is_noop() const noexcept;
    bool is_blocked() const noexcept;
};

// Dry-run/read-only assessment. It retains the exact production preflight
// observation and update attribution, but no executor accepts this type and it
// cannot carry PreparedAurUpdateSourceBuildInvocation.
struct AurUpdateSourceBuildObservation {
    std::vector<AurUpdatePreparationIssue> issues;
    std::vector<AurUpdatePreparationWarning> warnings;
    std::vector<AurUpdateExecutionTarget> affected_update_targets;
    std::vector<RootTargetIdentity> affected_roots;
    AurUpdateBuildUnitSelection build_unit_selection;
    std::vector<AurUpdateProjectedBuildUnit> projected_build_units;
    std::vector<AurUpdateExternallySatisfiedBuildUnit>
        externally_satisfied_build_units;
    std::vector<AurUpdatePreparedWorkItemAttribution>
        work_item_attributions;
    std::optional<ProductionSourceBuildPreparationObservation>
        production_preflight;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;

    bool is_ready() const noexcept;
    bool is_noop() const noexcept;
    bool is_blocked() const noexcept;
};

// Aggregate/command boundaryで先に準備したcache capabilityを、存在する
// execution invocationへ配る。blocked/no-op preparationでは再resolveしない。
void seed_aur_update_source_build_cache(
    AurUpdateSourceBuildPreparation& preparation,
    const ValidatedCacheRoot& cache_root);

// Update preflightを、executionへ接続しないowned invocation snapshotへ射影する。
// callerはsaved preference authorityへの到達可否をtyped policyで必ず指定する。
AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config);

// Capability-free counterpart used by read-only route observations. Saved
// preference policy and every downstream safety preflight remain explicit.
AurUpdateSourceBuildObservation observe_aur_update_source_build_preparation(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config);

// BuildPlan::order全件をexecution対象にするroute-neutral convenience overload。
AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config);

AurUpdateSourceBuildObservation observe_aur_update_source_build_preparation(
    const AurUpdateExecutionPreflight& preflight,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config);
