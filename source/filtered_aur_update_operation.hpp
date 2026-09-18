#pragma once

#include "app_config.hpp"
#include "aur_update_operation_result.hpp"
#include "aur_update_query.hpp"
#include "upgrade_all_plan.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// explicit-source overlayの有無とsaved preference policyを直交させる。
// NoExplicitはnormal AUR target/build-unit selectionであり、Ignoreを意味しない。
struct NoExplicitSourceSatisfaction {
    bool operator==(const NoExplicitSourceSatisfaction&) const = default;
};

// upgrade-allのexplicit source overlay。identitiesがemptyでもNoExplicitとは異なる。
struct UpgradeAllExplicitSourceSatisfaction {
    std::vector<UpgradeAllExplicitSourceIdentity> identities;

    bool operator==(
        const UpgradeAllExplicitSourceSatisfaction&) const = default;
};

using FilteredAurUpdateExplicitSourceSatisfaction = std::variant<
    NoExplicitSourceSatisfaction,
    UpgradeAllExplicitSourceSatisfaction>;

enum class FilteredAurUpdateOperationIssueKind {
    UnknownUpdateClassification,
    DevelRequiresCheckPolicyInconsistent,
    TargetPlannerMappingInconsistent,
    FilteredTargetMappingInconsistent,
    PreflightTargetMappingInconsistent,
    PreflightInvocationIndexOutOfRange,
    PreflightInvocationIdentityMismatch,
    BuildPlanRootIndexMissing,
    BuildPlanRootIndexOutOfRange,
    BuildPlanRootIdentityMismatch,
    BuildPlanRootPackageIdentityMismatch,
    BuildUnitOrderIdentityMismatch,
    BuildUnitRootAttributionInconsistent,
    BuildUnitSelectionMappingInconsistent,
    ExecutionBuildUnitMappingInconsistent,
    ReducedTargetMappingInconsistent,
};

// planner-local index、filtered index、BuildPlan indexを混ぜずに診断へ残す。
struct FilteredAurUpdateOperationIssue {
    FilteredAurUpdateOperationIssueKind kind =
        FilteredAurUpdateOperationIssueKind::
            FilteredTargetMappingInconsistent;
    std::optional<std::size_t> original_query_plan_index;
    std::optional<std::size_t> planner_target_index;
    std::optional<std::size_t> selected_target_index;
    std::optional<std::size_t> filtered_update_plan_index;
    std::optional<std::size_t> preflight_invocation_index;
    std::optional<std::size_t> build_plan_root_index;
    std::optional<std::size_t> build_plan_order_index;
    std::optional<std::size_t> invocation_work_item_index;
    std::optional<std::string> package_name;
    std::optional<std::string> package_base;
    std::string diagnostic;
};

enum class FilteredAurUpdateTargetAdapterDisposition {
    NormalSkip,
    RequiresCheckPolicySkip,
    PlannerTarget,
};

// query plan全件のpayloadを正本として保持し、planner subsetへの写像だけを足す。
struct FilteredAurUpdateTargetAdapterEntry {
    std::size_t original_query_plan_index = 0;
    AurUpdatePlanEntry update;
    FilteredAurUpdateTargetAdapterDisposition disposition =
        FilteredAurUpdateTargetAdapterDisposition::NormalSkip;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;
    std::optional<DevelRequiresCheckReason>
        devel_requires_check_reason;
    std::optional<std::size_t> planner_target_index;
    std::optional<std::size_t> filtered_update_plan_index;
};

struct FilteredAurUpdateTargetAdapter {
    std::vector<FilteredAurUpdateTargetAdapterEntry> entries;
    std::vector<UpgradeAllAurTarget> planner_targets;
    std::vector<std::size_t> planner_target_to_original_query_plan_index;
    std::vector<std::optional<std::size_t>>
        original_query_plan_to_planner_target_index;
};

// selected targetの各index体系を、identity照合済みの1 recordへ固定する。
struct FilteredAurUpdateTargetCorrelation {
    std::size_t planner_target_index = 0;
    std::size_t original_query_plan_index = 0;
    std::size_t selected_target_index = 0;
    std::size_t filtered_update_plan_index = 0;
    std::optional<std::size_t> preflight_invocation_index;
    std::optional<std::size_t> build_plan_root_index;
    std::string package_name;
    std::string package_base;
};

struct FilteredAurUpdateBuildUnitRootCorrelation {
    RootTargetIdentity preflight_root;
    std::size_t planner_target_index = 0;
    std::size_t original_query_plan_index = 0;
    std::size_t selected_target_index = 0;
    UpgradeAllBuildUnitRole role = UpgradeAllBuildUnitRole::Root;
};

struct FilteredAurUpdateBuildUnitCorrelation {
    std::size_t original_build_plan_index = 0;
    std::string package_base;
    std::vector<std::string> package_names;
    std::vector<FilteredAurUpdateBuildUnitRootCorrelation> root_correlations;
    std::optional<std::size_t> selected_execution_index;
    std::optional<std::size_t> invocation_work_item_index;
};

struct FilteredAurUpdateSelectedTargetResult {
    std::size_t selected_target_index = 0;
    std::size_t original_query_plan_index = 0;
    std::size_t filtered_update_plan_index = 0;
    AurUpdateOperationTargetResult operation_result;
};

struct FilteredAurUpdateExecutionResult;
struct FilteredAurUpdateOperationMutableAccess;

class PreparedFilteredAurUpdateOperation final {
    PreparedFilteredAurUpdateOperation() = default;

    friend PreparedFilteredAurUpdateOperation
    prepare_filtered_aur_update_operation(
        AurUpdateQueryResult query_result,
        FilteredAurUpdateExplicitSourceSatisfaction
            explicit_source_satisfaction,
        DevelRequiresCheckPolicy devel_requires_check_policy,
        SavedSourcePreferencePolicy saved_source_preference_policy,
        const AppConfig& config,
        std::optional<ValidatedCacheRoot> cache_root);
    friend FilteredAurUpdateExecutionResult
    execute_prepared_filtered_aur_update_operation(
        PreparedFilteredAurUpdateOperation prepared,
        const AppConfig& config);
    friend void seed_filtered_aur_update_operation_cache(
        PreparedFilteredAurUpdateOperation& prepared,
        const ValidatedCacheRoot& cache_root);
    friend struct FilteredAurUpdateOperationMutableAccess;

    bool valid_ = true;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;

    AurUpdateQueryResult query_result;
    FilteredAurUpdateTargetAdapter target_adapter;
    UpgradeAllPlan upgrade_all_plan;
    AurUpdatePlan filtered_update_plan;
    std::vector<std::size_t> filtered_to_original_query_plan_index;
    std::vector<std::optional<std::size_t>>
        original_query_plan_to_filtered_index;
    std::vector<FilteredAurUpdateTargetCorrelation> target_correlations;
    AurUpdateExecutionPreflight preflight;
    std::vector<FilteredAurUpdateBuildUnitCorrelation>
        build_unit_correlations;
    std::optional<AurUpdateSourceBuildPreparation> preparation;
    std::vector<FilteredAurUpdateOperationIssue> issues;

public:
    PreparedFilteredAurUpdateOperation(
        const PreparedFilteredAurUpdateOperation&) = delete;
    PreparedFilteredAurUpdateOperation& operator=(
        const PreparedFilteredAurUpdateOperation&) = delete;
    PreparedFilteredAurUpdateOperation(
        PreparedFilteredAurUpdateOperation&& other) noexcept;
    PreparedFilteredAurUpdateOperation& operator=(
        PreparedFilteredAurUpdateOperation&&) = delete;
    ~PreparedFilteredAurUpdateOperation() noexcept = default;

    // prepared capabilityのowned snapshotは観測だけを許し、runnerへ渡す前に
    // mapping/preparationとprivate invocationが乖離しないようにする。
    const AurUpdateQueryResult& original_query_result() const noexcept {
        return query_result;
    }
    const FilteredAurUpdateTargetAdapter& target_adapter_result()
        const noexcept;
    const UpgradeAllPlan& target_and_build_unit_plan() const noexcept;
    const AurUpdatePlan& filtered_plan() const noexcept;
    const std::vector<std::size_t>& filtered_to_original_indexes()
        const noexcept;
    const std::vector<std::optional<std::size_t>>&
    original_to_filtered_indexes() const noexcept;
    const std::vector<FilteredAurUpdateTargetCorrelation>&
    selected_target_correlations() const noexcept;
    const AurUpdateExecutionPreflight& execution_preflight() const noexcept {
        return preflight;
    }
    const std::vector<FilteredAurUpdateBuildUnitCorrelation>&
    build_unit_mapping() const noexcept;
    const std::optional<AurUpdateSourceBuildPreparation>&
    source_build_preparation() const noexcept {
        return preparation;
    }
    const std::vector<FilteredAurUpdateOperationIssue>& operation_issues()
        const noexcept;
    const std::optional<DevelRequiresCheckPolicy>&
    devel_requires_check_policy_snapshot() const noexcept {
        return devel_requires_check_policy;
    }

    bool is_valid() const noexcept;
    bool is_prepared() const noexcept;
    bool is_noop() const noexcept;
    bool is_blocked() const noexcept;
};

// Capability-free counterpart for read-only observations. It retains the
// exact adapter/planner/preflight correlation firewall and source-build safety
// observation, but no executor or cache-seeding API accepts this type.
struct FilteredAurUpdateObservation {
    AurUpdateQueryResult query_result;
    FilteredAurUpdateTargetAdapter target_adapter;
    UpgradeAllPlan upgrade_all_plan;
    AurUpdatePlan filtered_update_plan;
    std::vector<std::size_t> filtered_to_original_query_plan_index;
    std::vector<std::optional<std::size_t>>
        original_query_plan_to_filtered_index;
    std::vector<FilteredAurUpdateTargetCorrelation> target_correlations;
    AurUpdateExecutionPreflight preflight;
    std::vector<FilteredAurUpdateBuildUnitCorrelation>
        build_unit_correlations;
    std::optional<AurUpdateSourceBuildObservation> source_build_observation;
    std::vector<FilteredAurUpdateOperationIssue> issues;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;

    const AurUpdateQueryResult& original_query_result() const noexcept {
        return query_result;
    }
    const AurUpdateExecutionPreflight& execution_preflight() const noexcept {
        return preflight;
    }
    const UpgradeAllPlan& target_and_build_unit_plan() const noexcept {
        return upgrade_all_plan;
    }
    const std::optional<AurUpdateSourceBuildObservation>&
    source_build_preflight() const noexcept {
        return source_build_observation;
    }
    const std::vector<FilteredAurUpdateOperationIssue>& operation_issues()
        const noexcept {
        return issues;
    }

    bool is_ready() const noexcept;
    bool is_noop() const noexcept;
    bool is_blocked() const noexcept;
    bool has_consistent_devel_requires_check_policy_snapshot()
        const noexcept;
};

struct FilteredAurUpdateExecutionResult {
    AurUpdateQueryResult query_result;
    FilteredAurUpdateTargetAdapter target_adapter;
    UpgradeAllPlan upgrade_all_plan;
    AurUpdatePlan filtered_update_plan;
    std::vector<std::size_t> filtered_to_original_query_plan_index;
    std::vector<std::optional<std::size_t>>
        original_query_plan_to_filtered_index;
    std::vector<FilteredAurUpdateTargetCorrelation> target_correlations;
    AurUpdateExecutionPreflight preflight;
    std::vector<FilteredAurUpdateBuildUnitCorrelation>
        build_unit_correlations;
    AurUpdateSourceBuildPreparation preparation;
    std::optional<AurUpdateSourceBuildExecutionResult> execution;
    AurUpdateOperationResult reduced_operation_result;
    std::vector<FilteredAurUpdateSelectedTargetResult> selected_target_results;
    std::vector<FilteredAurUpdateOperationIssue> issues;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;

    bool is_success() const noexcept;
    PackageStateChange package_state_change() const noexcept;
    bool changed_package_state() const noexcept;
    bool has_partial_completion() const noexcept;
    bool has_not_attempted_targets() const noexcept;
    bool has_cleanup_failure() const noexcept;
    bool has_query_failure() const noexcept;
    bool has_planning_issue() const noexcept;
    bool has_duplicate_exclusions() const noexcept;
    bool has_consistent_devel_requires_check_policy_snapshot()
        const noexcept;
};

// Owned partial facts travel with the stop signal; callers must not resume mutation.
class FilteredAurUpdateCancelled final : public std::exception {
public:
    explicit FilteredAurUpdateCancelled(FilteredAurUpdateExecutionResult result) noexcept;
    const FilteredAurUpdateExecutionResult& result() const noexcept;
    FilteredAurUpdateExecutionResult release_result() && noexcept;
    const char* what() const noexcept override;

private:
    FilteredAurUpdateExecutionResult result_;
};

FilteredAurUpdateTargetAdapter adapt_aur_update_plan_for_upgrade_all(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    std::vector<FilteredAurUpdateOperationIssue>& issues);

PreparedFilteredAurUpdateOperation prepare_filtered_aur_update_operation(
    AurUpdateQueryResult query_result,
    FilteredAurUpdateExplicitSourceSatisfaction
        explicit_source_satisfaction,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    const AppConfig& config,
    std::optional<ValidatedCacheRoot> cache_root = std::nullopt);

FilteredAurUpdateObservation observe_filtered_aur_update_operation(
    AurUpdateQueryResult query_result,
    FilteredAurUpdateExplicitSourceSatisfaction
        explicit_source_satisfaction,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    const AppConfig& config);

// A route aggregate that retained a cache authority can seed the already
// resolved filtered invocation immediately before actual execution.
void seed_filtered_aur_update_operation_cache(
    PreparedFilteredAurUpdateOperation& prepared,
    const ValidatedCacheRoot& cache_root);

// aggregateをby-valueでconsumeし、nested invocation capabilityだけをrunnerへmoveする。
FilteredAurUpdateExecutionResult execute_prepared_filtered_aur_update_operation(
    PreparedFilteredAurUpdateOperation prepared,
    const AppConfig& config);
