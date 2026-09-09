#include "filtered_aur_update_operation.hpp"

#include "dependency_spec.hpp"
#include "localization.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

// translation unit内のplanner/preflight adapterだけへmutable stateを貸す。
// Public APIにはconst getterしか出さず、prepared capabilityの改変面を作らない。
struct FilteredAurUpdateOperationMutableAccess {
    struct Snapshot {
        std::optional<DevelRequiresCheckPolicy>&
            devel_requires_check_policy;
        AurUpdateQueryResult& query_result;
        FilteredAurUpdateTargetAdapter& target_adapter;
        UpgradeAllPlan& upgrade_all_plan;
        AurUpdatePlan& filtered_update_plan;
        std::vector<std::size_t>& filtered_to_original_query_plan_index;
        std::vector<std::optional<std::size_t>>&
            original_query_plan_to_filtered_index;
        std::vector<FilteredAurUpdateTargetCorrelation>& target_correlations;
        AurUpdateExecutionPreflight& preflight;
        std::vector<FilteredAurUpdateBuildUnitCorrelation>&
            build_unit_correlations;
        std::optional<AurUpdateSourceBuildPreparation>& preparation;
        std::vector<FilteredAurUpdateOperationIssue>& issues;
    };

    struct ObservationSnapshot {
        std::optional<DevelRequiresCheckPolicy>&
            devel_requires_check_policy;
        AurUpdateQueryResult& query_result;
        FilteredAurUpdateTargetAdapter& target_adapter;
        UpgradeAllPlan& upgrade_all_plan;
        AurUpdatePlan& filtered_update_plan;
        std::vector<std::size_t>& filtered_to_original_query_plan_index;
        std::vector<std::optional<std::size_t>>&
            original_query_plan_to_filtered_index;
        std::vector<FilteredAurUpdateTargetCorrelation>& target_correlations;
        AurUpdateExecutionPreflight& preflight;
        std::vector<FilteredAurUpdateBuildUnitCorrelation>&
            build_unit_correlations;
        std::optional<AurUpdateSourceBuildObservation>& source_build_observation;
        std::vector<FilteredAurUpdateOperationIssue>& issues;
    };

    static Snapshot snapshot(PreparedFilteredAurUpdateOperation& operation) {
        return Snapshot{
            operation.devel_requires_check_policy,
            operation.query_result,
            operation.target_adapter,
            operation.upgrade_all_plan,
            operation.filtered_update_plan,
            operation.filtered_to_original_query_plan_index,
            operation.original_query_plan_to_filtered_index,
            operation.target_correlations,
            operation.preflight,
            operation.build_unit_correlations,
            operation.preparation,
            operation.issues};
    }

    static ObservationSnapshot snapshot(
        FilteredAurUpdateObservation& observation) {
        return ObservationSnapshot{
            observation.devel_requires_check_policy,
            observation.query_result,
            observation.target_adapter,
            observation.upgrade_all_plan,
            observation.filtered_update_plan,
            observation.filtered_to_original_query_plan_index,
            observation.original_query_plan_to_filtered_index,
            observation.target_correlations,
            observation.preflight,
            observation.build_unit_correlations,
            observation.source_build_observation,
            observation.issues};
    }
};

// query payload、pure planner、preflight、preparation/runner/reducerを接続する。
// POLICY(#281): 各段のdense indexはidentityの代用にせず、名前とPackageBaseを
// 照合したcorrelation recordを介してのみ次段へ渡す。
namespace {

constexpr std::string_view AUR_SERVICE_NAME = "AUR";
constexpr std::string_view AUR_UPDATE_PLAN_TYPE_NAME = "AurUpdatePlan";
constexpr std::string_view BUILD_PLAN_TYPE_NAME = "BuildPlan";
constexpr std::string_view ROOT_ROLE_NAME = "Root";
constexpr std::string_view UPGRADE_ALL_COMMAND_NAME = "upgrade-all";
constexpr std::string_view DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC =
    "Devel RequiresCheck policy is unknown.";
constexpr char PREFLIGHT_TARGET_COUNT_DIAGNOSTIC[] =
    "Preflight target count differs from the filtered update plan.";
constexpr char PREFLIGHT_TARGET_PAYLOAD_DIAGNOSTIC[] =
    "Preflight target position/index/payload differs from the filtered update plan.";

std::vector<UpgradeAllExplicitSourceIdentity>
materialize_explicit_source_satisfaction(
    FilteredAurUpdateExplicitSourceSatisfaction satisfaction) {
    if(std::holds_alternative<NoExplicitSourceSatisfaction>(satisfaction)) {
        return {};
    }
    return std::move(
        std::get<UpgradeAllExplicitSourceSatisfaction>(satisfaction)
            .identities);
}

bool same_remote_package(
    const std::optional<AurUpdateRemotePackage>& lhs,
    const std::optional<AurUpdateRemotePackage>& rhs) {
    if(lhs.has_value() != rhs.has_value()) return false;
    if(!lhs.has_value()) return true;
    return lhs->aur_name == rhs->aur_name &&
           lhs->package_base == rhs->package_base &&
           lhs->version == rhs->version &&
           lhs->version_relation == rhs->version_relation;
}

bool same_update_entry(
    const AurUpdatePlanEntry& lhs,
    const AurUpdatePlanEntry& rhs) {
    return lhs.installed_name == rhs.installed_name &&
           lhs.installed_version == rhs.installed_version &&
           lhs.install_reason == rhs.install_reason &&
           lhs.classification == rhs.classification &&
           lhs.devel_classification == rhs.devel_classification &&
           lhs.devel_assessment_origin == rhs.devel_assessment_origin &&
           lhs.devel_assessment == rhs.devel_assessment &&
           same_remote_package(lhs.aur_package, rhs.aur_package);
}

bool same_preflight_issue(
    const AurUpdateExecutionIssue& lhs,
    const AurUpdateExecutionIssue& rhs) {
    return lhs.reason == rhs.reason &&
           lhs.package_name == rhs.package_name &&
           lhs.package_base == rhs.package_base &&
           lhs.dependency_specification == rhs.dependency_specification &&
           lhs.diagnostic == rhs.diagnostic &&
           lhs.devel_requires_check_reason ==
               rhs.devel_requires_check_reason &&
           lhs.build_plan_projection_issue ==
               rhs.build_plan_projection_issue &&
           lhs.relation_reason == rhs.relation_reason &&
           lhs.required_devel_target_blocker ==
               rhs.required_devel_target_blocker;
}

bool same_preflight_target(
    const AurUpdateExecutionTarget& lhs,
    const AurUpdateExecutionTarget& rhs) {
    return lhs.update_plan_index == rhs.update_plan_index &&
           lhs.build_plan_root_index == rhs.build_plan_root_index &&
           same_update_entry(lhs.update, rhs.update) &&
           lhs.status == rhs.status &&
           lhs.desired_install_reason == rhs.desired_install_reason &&
           lhs.skip_kind == rhs.skip_kind &&
           lhs.issues.size() == rhs.issues.size() &&
           std::equal(
               lhs.issues.begin(), lhs.issues.end(),
               rhs.issues.begin(), same_preflight_issue);
}

bool has_compatible_singular_package_name(
    const std::string& package_name,
    const std::vector<std::string>& package_names) noexcept {
    if(package_names.size() == 1) {
        return package_name == package_names.front();
    }
    return package_name.empty();
}

bool has_exact_required_package_names(
    const std::vector<AurUpdateRequiredTargetAttribution>& attributions,
    const std::vector<std::string>& package_names) noexcept {
    if(attributions.size() != package_names.size()) return false;
    for(std::size_t index = 0; index < attributions.size(); ++index) {
        if(attributions[index].required_target.package_name !=
           package_names[index]) {
            return false;
        }
    }
    return true;
}

bool has_exact_execution_child_names(
    const AurUpdateWorkItemExecutionResult& work_item,
    const std::vector<std::string>& package_names) noexcept {
    if(work_item.child_results.size() != package_names.size()) return false;
    for(std::size_t index = 0; index < work_item.child_results.size(); ++index) {
        const AurUpdateChildExecutionResult& child =
            work_item.child_results[index];
        if(child.work_item_index != work_item.work_item_index ||
           child.build_plan_order_index != work_item.build_plan_order_index ||
           child.required_child_index != index ||
           child.package_base != work_item.package_base ||
           child.required_package_name != package_names[index]) {
            return false;
        }
    }
    return true;
}

FilteredAurUpdateOperationIssue& add_localized_operation_issue(
    std::vector<FilteredAurUpdateOperationIssue>& issues,
    FilteredAurUpdateOperationIssueKind kind,
    std::string diagnostic) {
    FilteredAurUpdateOperationIssue issue;
    issue.kind = kind;
    issue.diagnostic = std::move(diagnostic);
    issues.push_back(std::move(issue));
    return issues.back();
}

template <std::size_t Size>
FilteredAurUpdateOperationIssue& add_localized_operation_issue(
    std::vector<FilteredAurUpdateOperationIssue>& issues,
    FilteredAurUpdateOperationIssueKind kind,
    const char (&diagnostic)[Size]) {
    return add_localized_operation_issue(
        issues, kind, localization::translate_message(diagnostic));
}

UpgradeAllPackageBaseIdentity package_base_identity(
    const AurUpdatePlanEntry& entry) {
    if(!entry.aur_package.has_value()) return UpgradeAllPackageBaseAbsent{};
    return UpgradeAllResolvedPackageBase{entry.aur_package->package_base};
}

bool is_normal_skip(const AurUpdatePlanEntry& update) noexcept {
    const AurUpdateEffectiveState effective_state =
        project_aur_update_effective_state(update);
    return effective_state == AurUpdateEffectiveState::UpToDate ||
           effective_state == AurUpdateEffectiveState::NonAurForeign;
}

bool target_disposition_is_excluded(
    UpgradeAllTargetDisposition disposition) noexcept {
    return disposition ==
               UpgradeAllTargetDisposition::ExcludedByExplicitPackageName ||
           disposition ==
               UpgradeAllTargetDisposition::ExcludedByExplicitPackageBase;
}

bool has_role(
    const PlannedPackageTarget& target,
    PackageRole expected_role) {
    return std::find(
               target.roles.begin(), target.roles.end(), expected_role) !=
           target.roles.end();
}

bool is_dependency_role(PackageRole role) noexcept {
    return role == PackageRole::RuntimeDependency ||
           role == PackageRole::BuildDependency ||
           role == PackageRole::CheckDependency;
}

bool contains_root(
    const PlannedPackageTarget& target,
    const RootTargetIdentity& root) {
    return std::find(target.roots.begin(), target.roots.end(), root) !=
           target.roots.end();
}

bool contains_all_roots(
    const std::vector<RootTargetIdentity>& values,
    const std::vector<RootTargetIdentity>& expected) {
    return std::all_of(
        expected.begin(), expected.end(),
        [&values](const RootTargetIdentity& root) {
            return std::find(values.begin(), values.end(), root) !=
                   values.end();
        });
}

std::optional<std::size_t> find_unique_package_target_index(
    const BuildPlan& plan,
    const std::string& package_name,
    const std::optional<std::string>& package_base) {
    std::optional<std::size_t> match;
    for(std::size_t index = 0; index < plan.package_targets.size(); ++index) {
        const PlannedPackageTarget& target = plan.package_targets[index];
        if(target.package_name != package_name) continue;
        if(package_base.has_value() &&
           target.package_base != *package_base) {
            continue;
        }
        if(match.has_value()) return std::nullopt;
        match = index;
    }
    return match;
}

UpgradeAllBuildUnitRole map_build_unit_role(
    PackageRole role,
    std::vector<FilteredAurUpdateOperationIssue>& issues,
    std::optional<std::size_t> build_plan_order_index = std::nullopt) {
    switch(role) {
        case PackageRole::Root:
            return UpgradeAllBuildUnitRole::Root;
        case PackageRole::RuntimeDependency:
            return UpgradeAllBuildUnitRole::RuntimeDependency;
        case PackageRole::BuildDependency:
            return UpgradeAllBuildUnitRole::BuildDependency;
        case PackageRole::CheckDependency:
            return UpgradeAllBuildUnitRole::CheckDependency;
    }

    FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
        issues,
        FilteredAurUpdateOperationIssueKind::
            BuildUnitRootAttributionInconsistent,
        localization::format_translated_message(
            // TRANSLATORS: {} is the literal internal type name "BuildPlan".
            "{} package target contains an unknown package role.",
            BUILD_PLAN_TYPE_NAME));
    issue.build_plan_order_index = build_plan_order_index;
    return UpgradeAllBuildUnitRole::Root;
}

struct ExactDependencyEdge {
    std::size_t parent_target_index = 0;
    std::size_t target_index = 0;
    PackageRole role = PackageRole::RuntimeDependency;
};

struct DerivedRootRole {
    RootTargetIdentity root;
    PackageRole role = PackageRole::RuntimeDependency;
};

struct RootIdentityLess {
    bool operator()(
        const RootTargetIdentity& lhs,
        const RootTargetIdentity& rhs) const noexcept {
        if(lhs.invocation_index != rhs.invocation_index) {
            return lhs.invocation_index < rhs.invocation_index;
        }
        return lhs.requested_name < rhs.requested_name;
    }
};

void append_unique_root_role(
    std::vector<DerivedRootRole>& attributions,
    const RootTargetIdentity& root,
    PackageRole role) {
    auto same = [&root, role](const DerivedRootRole& attribution) {
        return attribution.root == root && attribution.role == role;
    };
    if(std::find_if(attributions.begin(), attributions.end(), same) ==
       attributions.end()) {
        attributions.push_back(DerivedRootRole{root, role});
    }
}

std::vector<ExactDependencyEdge> collect_exact_dependency_edges(
    const BuildPlan& plan,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    std::vector<ExactDependencyEdge> edges;
    for(const BuildPlanDependencyEdge& edge : plan.dependency_edges) {
        if(!is_dependency_role(edge.role)) continue;

        const bool is_aur_edge = edge.kind == DependencyKind::Aur;
        const bool is_aur_provider_edge =
            edge.kind == DependencyKind::Provided &&
            edge.resolved_provider.has_value() &&
            std::holds_alternative<AurProviderOrigin>(
                edge.resolved_provider->origin);
        if(!is_aur_edge && !is_aur_provider_edge) continue;

        const std::optional<std::size_t> parent_index =
            find_unique_package_target_index(
                plan, edge.parent_package_name,
                std::optional<std::string>{edge.parent_package_base});

        std::optional<std::size_t> target_index;
        std::optional<std::string> exact_requirement_name;
        if(edge.requirement.has_value() &&
           std::holds_alternative<ConsumerDependencyRequirement>(
               edge.requirement.value())) {
            exact_requirement_name =
                std::get<ConsumerDependencyRequirement>(
                    edge.requirement.value())
                    .package_name();
        } else if(!edge.requirement.has_value()) {
            // Compatibility for graph-only fixtures. Production edges own the
            // typed requirement and never enter this branch.
            ParsedDependency legacy =
                parse_dependency_string(edge.dependency_spec);
            if(!legacy.has_malformed_constraint()) {
                exact_requirement_name = legacy.name;
            }
        }
        if(is_aur_edge && edge.resolved_package_name.has_value() &&
           edge.resolved_package_base.has_value() &&
           !edge.resolved_provider.has_value() &&
           exact_requirement_name.has_value() &&
           *edge.resolved_package_name ==
               exact_requirement_name.value()) {
            target_index = find_unique_package_target_index(
                plan, *edge.resolved_package_name,
                edge.resolved_package_base);
        } else if(
            is_aur_provider_edge &&
            !edge.resolved_package_name.has_value() &&
            !edge.resolved_package_base.has_value()) {
            target_index = find_unique_package_target_index(
                plan, edge.resolved_provider->package_name,
                std::nullopt);
        }

        bool is_consistent = parent_index.has_value() &&
                             target_index.has_value();
        if(is_consistent) {
            const PlannedPackageTarget& parent =
                plan.package_targets[*parent_index];
            const PlannedPackageTarget& target =
                plan.package_targets[*target_index];
            is_consistent = has_role(target, edge.role) &&
                            !parent.roots.empty() &&
                            contains_all_roots(target.roots, parent.roots);
        }
        if(!is_consistent) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildUnitRootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // internal type name "BuildPlan" and service name
                    // "AUR", respectively.
                    "{} {} dependency edge cannot be correlated by exact package identity.",
                    BUILD_PLAN_TYPE_NAME,
                    AUR_SERVICE_NAME));
            issue.package_name = edge.resolved_package_name;
            issue.package_base = edge.resolved_package_base;
            continue;
        }

        edges.push_back(ExactDependencyEdge{
            *parent_index, *target_index, edge.role});
    }
    return edges;
}

std::vector<std::vector<DerivedRootRole>> derive_root_roles(
    const BuildPlan& plan,
    const std::vector<ExactDependencyEdge>& edges,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    std::vector<std::vector<DerivedRootRole>> attributions(
        plan.package_targets.size());

    for(const RootTargetIdentity& root : plan.root_targets) {
        const std::optional<std::size_t> root_target_index =
            find_unique_package_target_index(
                plan, root.requested_name, std::nullopt);
        if(!root_target_index.has_value() ||
           !has_role(
               plan.package_targets[*root_target_index],
               PackageRole::Root) ||
           !contains_root(plan.package_targets[*root_target_index], root)) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildUnitRootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // internal type name "BuildPlan" and enum value
                    // "Root", respectively.
                    "{} root does not have one exact {} package target.",
                    BUILD_PLAN_TYPE_NAME,
                    ROOT_ROLE_NAME));
            issue.preflight_invocation_index = root.invocation_index;
            issue.package_name = root.requested_name;
            continue;
        }

        append_unique_root_role(
            attributions[*root_target_index], root, PackageRole::Root);
        std::vector<std::size_t> pending{*root_target_index};
        std::set<std::size_t> reached{*root_target_index};
        while(!pending.empty()) {
            const std::size_t parent_index = pending.back();
            pending.pop_back();
            for(const ExactDependencyEdge& edge : edges) {
                if(edge.parent_target_index != parent_index) continue;
                append_unique_root_role(
                    attributions[edge.target_index], root, edge.role);
                if(reached.insert(edge.target_index).second) {
                    pending.push_back(edge.target_index);
                }
            }
        }
    }

    // LANDMINE(#281): PlannedPackageTarget::rolesとrootsの直積は、root兼dependency
    // packageに偽のRoot attributionを作る。root別graph traversalのunionだけを照合する。
    for(std::size_t index = 0; index < plan.package_targets.size(); ++index) {
        const PlannedPackageTarget& target = plan.package_targets[index];
        std::set<RootTargetIdentity, RootIdentityLess> derived_roots;
        std::set<PackageRole> derived_roles;
        for(const DerivedRootRole& attribution : attributions[index]) {
            derived_roots.insert(attribution.root);
            derived_roles.insert(attribution.role);
        }
        std::set<RootTargetIdentity, RootIdentityLess> declared_roots(
            target.roots.begin(), target.roots.end());
        std::set<PackageRole> declared_roles(
            target.roles.begin(), target.roles.end());
        if(derived_roots == declared_roots &&
           derived_roles == declared_roles) {
            continue;
        }

        FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
            issues,
            FilteredAurUpdateOperationIssueKind::
                BuildUnitRootAttributionInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                "{} package target roles/roots differ from rooted dependency graph attribution.",
                BUILD_PLAN_TYPE_NAME));
        issue.package_name = target.package_name;
        issue.package_base = target.package_base;
    }
    return attributions;
}

const FilteredAurUpdateTargetCorrelation* find_target_correlation(
    const std::vector<FilteredAurUpdateTargetCorrelation>& correlations,
    const RootTargetIdentity& root) {
    if(root.invocation_index >= correlations.size()) return nullptr;
    const FilteredAurUpdateTargetCorrelation& correlation =
        correlations[root.invocation_index];
    if(correlation.selected_target_index != root.invocation_index ||
       correlation.package_name != root.requested_name) {
        return nullptr;
    }
    return &correlation;
}

struct BuildUnitAdapterResult {
    std::vector<UpgradeAllAurBuildUnit> build_units;
    std::vector<FilteredAurUpdateBuildUnitCorrelation> correlations;
};

BuildUnitAdapterResult adapt_build_plan(
    const BuildPlan& plan,
    const std::vector<FilteredAurUpdateTargetCorrelation>&
        target_correlations,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    BuildUnitAdapterResult result;
    result.build_units.reserve(plan.order.size());
    result.correlations.reserve(plan.order.size());

    const std::vector<ExactDependencyEdge> graph =
        collect_exact_dependency_edges(plan, issues);
    const std::vector<std::vector<DerivedRootRole>> derived =
        derive_root_roles(plan, graph, issues);

    for(std::size_t order_index = 0; order_index < plan.order.size();
        ++order_index) {
        const BuildPlanEntry& order_entry = plan.order[order_index];
        UpgradeAllAurBuildUnit build_unit;
        build_unit.package_base = UpgradeAllResolvedPackageBase{
            order_entry.package_base};
        build_unit.package_names = order_entry.package_names;

        FilteredAurUpdateBuildUnitCorrelation correlation;
        correlation.original_build_plan_index = order_index;
        correlation.package_base = order_entry.package_base;
        correlation.package_names = order_entry.package_names;

        std::vector<std::size_t> package_target_indices;
        bool order_identity_is_consistent = !order_entry.package_names.empty();
        for(const std::string& package_name : order_entry.package_names) {
            const std::optional<std::size_t> target_index =
                find_unique_package_target_index(
                    plan, package_name,
                    std::optional<std::string>{
                        order_entry.package_base});
            if(!target_index.has_value()) {
                order_identity_is_consistent = false;
                continue;
            }
            if(std::find(
                   package_target_indices.begin(),
                   package_target_indices.end(), *target_index) ==
               package_target_indices.end()) {
                package_target_indices.push_back(*target_index);
            }
        }
        if(!order_identity_is_consistent) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildUnitOrderIdentityMismatch,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "{} order entry does not map exactly to planned package targets.",
                    BUILD_PLAN_TYPE_NAME));
            issue.build_plan_order_index = order_index;
            issue.package_base = order_entry.package_base;
        }

        for(const std::size_t package_target_index : package_target_indices) {
            for(const DerivedRootRole& attribution :
                derived[package_target_index]) {
                const FilteredAurUpdateTargetCorrelation* target =
                    find_target_correlation(
                        target_correlations, attribution.root);
                if(target == nullptr) {
                    FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                        issues,
                        FilteredAurUpdateOperationIssueKind::
                            BuildUnitRootAttributionInconsistent,
                        localization::format_translated_message(
                            // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                            "{} unit root cannot be mapped to a selected filtered target identity.",
                            BUILD_PLAN_TYPE_NAME));
                    issue.build_plan_order_index = order_index;
                    issue.preflight_invocation_index =
                        attribution.root.invocation_index;
                    issue.package_name = attribution.root.requested_name;
                    issue.package_base = order_entry.package_base;
                    continue;
                }

                const UpgradeAllBuildUnitRole role = map_build_unit_role(
                    attribution.role, issues, order_index);
                const UpgradeAllBuildUnitRootAttribution planner_attribution{
                    target->planner_target_index, role};
                if(std::find_if(
                       build_unit.root_attributions.begin(),
                       build_unit.root_attributions.end(),
                       [&planner_attribution](
                           const UpgradeAllBuildUnitRootAttribution&
                               existing) {
                           return existing.original_target_index ==
                                      planner_attribution
                                          .original_target_index &&
                                  existing.role ==
                                      planner_attribution.role;
                       }) == build_unit.root_attributions.end()) {
                    build_unit.root_attributions.push_back(
                        planner_attribution);
                }

                const FilteredAurUpdateBuildUnitRootCorrelation
                    root_correlation{
                        attribution.root,
                        target->planner_target_index,
                        target->original_query_plan_index,
                        target->selected_target_index,
                        role};
                auto same_root_correlation =
                    [&root_correlation](
                        const FilteredAurUpdateBuildUnitRootCorrelation&
                            existing) {
                        return existing.preflight_root ==
                                   root_correlation.preflight_root &&
                               existing.planner_target_index ==
                                   root_correlation
                                       .planner_target_index &&
                               existing.role == root_correlation.role;
                    };
                if(std::find_if(
                       correlation.root_correlations.begin(),
                       correlation.root_correlations.end(),
                       same_root_correlation) ==
                   correlation.root_correlations.end()) {
                    correlation.root_correlations.push_back(
                        root_correlation);
                }
            }
        }

        result.build_units.push_back(std::move(build_unit));
        result.correlations.push_back(std::move(correlation));
    }
    return result;
}

template <typename Operation>
void build_filtered_update_plan(Operation& operation) {
    auto state = FilteredAurUpdateOperationMutableAccess::snapshot(operation);
    const AurUpdatePlan& original_plan = state.query_result.plan;
    state.original_query_plan_to_filtered_index.assign(
        original_plan.entries.size(), std::nullopt);
    if(state.target_adapter.entries.size() !=
       original_plan.entries.size()) {
        add_localized_operation_issue(
            state.issues,
            FilteredAurUpdateOperationIssueKind::
                TargetPlannerMappingInconsistent,
            localization::translate_message(
                PREFLIGHT_TARGET_COUNT_DIAGNOSTIC));
    }

    for(std::size_t original_index = 0;
        original_index < state.target_adapter.entries.size();
        ++original_index) {
        FilteredAurUpdateTargetAdapterEntry& adapter_entry =
            state.target_adapter.entries[original_index];
        if(original_index >= original_plan.entries.size() ||
           adapter_entry.original_query_plan_index != original_index ||
           !same_update_entry(
               adapter_entry.update,
               original_plan.entries[original_index])) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    TargetPlannerMappingInconsistent,
                localization::translate_message(
                    PREFLIGHT_TARGET_PAYLOAD_DIAGNOSTIC));
            issue.original_query_plan_index = original_index;
            issue.package_name = adapter_entry.update.installed_name;
            continue;
        }

        const bool is_requires_check_policy_skip =
            adapter_entry.disposition ==
            FilteredAurUpdateTargetAdapterDisposition::
                RequiresCheckPolicySkip;
        const DevelRequiresCheckReason* adapter_requires_check_reason =
            adapter_entry.update.devel_assessment
                .requires_check_reason();
        if(is_requires_check_policy_skip &&
           (state.devel_requires_check_policy !=
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::SkipIndependentTarget} ||
            adapter_entry.devel_requires_check_policy !=
                state.devel_requires_check_policy ||
            project_aur_update_effective_state(adapter_entry.update) !=
                AurUpdateEffectiveState::RequiresCheck ||
            adapter_requires_check_reason == nullptr ||
            adapter_entry.devel_requires_check_reason !=
                std::optional<DevelRequiresCheckReason>{
                    *adapter_requires_check_reason} ||
            adapter_entry.planner_target_index.has_value())) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    DevelRequiresCheckPolicyInconsistent,
                std::string{DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
            issue.original_query_plan_index = original_index;
            issue.package_name = adapter_entry.update.installed_name;
        }
        if(!is_requires_check_policy_skip &&
           (adapter_entry.devel_requires_check_policy.has_value() ||
            adapter_entry.devel_requires_check_reason.has_value())) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    DevelRequiresCheckPolicyInconsistent,
                std::string{DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
            issue.original_query_plan_index = original_index;
            issue.package_name = adapter_entry.update.installed_name;
        }
        bool should_retain =
            adapter_entry.disposition ==
                FilteredAurUpdateTargetAdapterDisposition::NormalSkip ||
            adapter_entry.disposition ==
                FilteredAurUpdateTargetAdapterDisposition::
                    RequiresCheckPolicySkip;
        if(adapter_entry.planner_target_index.has_value()) {
            const std::size_t planner_index =
                *adapter_entry.planner_target_index;
            if(planner_index >=
               state.upgrade_all_plan.target_dispositions.size()) {
                FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                    state.issues,
                    FilteredAurUpdateOperationIssueKind::
                        TargetPlannerMappingInconsistent,
                    "Target adapter refers to an out-of-range planner target.");
                issue.original_query_plan_index = original_index;
                issue.planner_target_index = planner_index;
                continue;
            }
            should_retain = !target_disposition_is_excluded(
                state.upgrade_all_plan
                    .target_dispositions[planner_index]
                    .disposition);
        }
        if(!should_retain) continue;

        const std::size_t filtered_index =
            state.filtered_update_plan.entries.size();
        adapter_entry.filtered_update_plan_index = filtered_index;
        state.filtered_update_plan.entries.push_back(
            adapter_entry.update);
        state.filtered_to_original_query_plan_index.push_back(
            original_index);
        state.original_query_plan_to_filtered_index[original_index] =
            filtered_index;
    }

    state.target_correlations.reserve(
        state.upgrade_all_plan.selected_targets.size());
    for(std::size_t position = 0;
        position < state.upgrade_all_plan.selected_targets.size();
        ++position) {
        const UpgradeAllSelectedAurTarget& selected =
            state.upgrade_all_plan.selected_targets[position];
        if(selected.selected_index != position ||
           selected.original_target_index >=
               state.target_adapter
                   .planner_target_to_original_query_plan_index
                   .size() ||
           selected.original_target_index >=
               state.upgrade_all_plan
                   .original_to_selected_index.size() ||
           state.upgrade_all_plan.original_to_selected_index
                   [selected.original_target_index] != position) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    TargetPlannerMappingInconsistent,
                "Selected planner target dense/original mapping is inconsistent.");
            issue.planner_target_index = selected.original_target_index;
            issue.selected_target_index = position;
            continue;
        }

        const std::size_t original_index =
            state.target_adapter
                .planner_target_to_original_query_plan_index
                    [selected.original_target_index];
        if(original_index >= original_plan.entries.size() ||
           original_index >=
               state.original_query_plan_to_filtered_index.size() ||
           !state.original_query_plan_to_filtered_index[original_index]
                .has_value()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    FilteredTargetMappingInconsistent,
                "Selected planner target has no retained original query entry.");
            issue.original_query_plan_index = original_index;
            issue.planner_target_index = selected.original_target_index;
            issue.selected_target_index = position;
            continue;
        }

        const AurUpdatePlanEntry& original =
            original_plan.entries[original_index];
        const bool identity_matches = original.aur_package.has_value() &&
                                      original.installed_name == selected.package_name &&
                                      original.aur_package->aur_name == selected.package_name &&
                                      original.aur_package->package_base == selected.package_base;
        if(!identity_matches) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    FilteredTargetMappingInconsistent,
                "Selected planner target identity differs from its original query payload.");
            issue.original_query_plan_index = original_index;
            issue.planner_target_index = selected.original_target_index;
            issue.selected_target_index = position;
            issue.package_name = selected.package_name;
            issue.package_base = selected.package_base;
            continue;
        }

        state.target_correlations.push_back(
            FilteredAurUpdateTargetCorrelation{
                selected.original_target_index,
                original_index,
                position,
                *state.original_query_plan_to_filtered_index
                     [original_index],
                std::nullopt,
                std::nullopt,
                selected.package_name,
                selected.package_base});
    }
}

template <typename Operation>
bool correlate_root_invocation_identity(
    Operation& operation,
    const FilteredAurUpdateTargetCorrelation& correlation,
    const RootTargetIdentity& root,
    std::size_t root_index) {
    auto state = FilteredAurUpdateOperationMutableAccess::snapshot(operation);
    if(root.invocation_index >= state.target_correlations.size()) {
        FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
            state.issues,
            FilteredAurUpdateOperationIssueKind::
                PreflightInvocationIndexOutOfRange,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                "{} root invocation index maps outside the selected target snapshot.",
                BUILD_PLAN_TYPE_NAME));
        issue.original_query_plan_index =
            correlation.original_query_plan_index;
        issue.selected_target_index = correlation.selected_target_index;
        issue.preflight_invocation_index = root.invocation_index;
        issue.build_plan_root_index = root_index;
        issue.package_name = root.requested_name;
        return false;
    }
    if(root.invocation_index != correlation.selected_target_index ||
       root.requested_name != correlation.package_name) {
        FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
            state.issues,
            FilteredAurUpdateOperationIssueKind::
                PreflightInvocationIdentityMismatch,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                "{} root invocation identity differs from the selected target identity.",
                BUILD_PLAN_TYPE_NAME));
        issue.original_query_plan_index =
            correlation.original_query_plan_index;
        issue.selected_target_index = correlation.selected_target_index;
        issue.preflight_invocation_index = root.invocation_index;
        issue.build_plan_root_index = root_index;
        issue.package_name = root.requested_name;
        return false;
    }
    return true;
}

template <typename Operation>
void correlate_preflight(Operation& operation) {
    auto state = FilteredAurUpdateOperationMutableAccess::snapshot(operation);
    if(state.preflight.targets.size() !=
       state.filtered_update_plan.entries.size()) {
        add_localized_operation_issue(
            state.issues,
            FilteredAurUpdateOperationIssueKind::
                PreflightTargetMappingInconsistent,
            "Preflight target count differs from the filtered update plan.");
    }

    const std::size_t comparable_count = std::min(
        state.preflight.targets.size(),
        state.filtered_update_plan.entries.size());
    for(std::size_t position = 0; position < comparable_count; ++position) {
        const AurUpdateExecutionTarget& target =
            state.preflight.targets[position];
        if(target.update_plan_index == position &&
           same_update_entry(
               target.update,
               state.filtered_update_plan.entries[position])) {
            continue;
        }
        FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
            state.issues,
            FilteredAurUpdateOperationIssueKind::
                PreflightTargetMappingInconsistent,
            "Preflight target position/index/payload differs from the filtered update plan.");
        issue.filtered_update_plan_index = position;
        issue.original_query_plan_index =
            position <
                    state
                        .filtered_to_original_query_plan_index
                        .size()
                ? std::optional<std::size_t>{
                      state
                          .filtered_to_original_query_plan_index
                              [position]}
                : std::nullopt;
        issue.package_name = target.update.installed_name;
    }

    for(FilteredAurUpdateTargetCorrelation& correlation :
        state.target_correlations) {
        if(correlation.filtered_update_plan_index >=
           state.preflight.targets.size()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    PreflightTargetMappingInconsistent,
                "Selected target maps outside the preflight target snapshot.");
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.filtered_update_plan_index =
                correlation.filtered_update_plan_index;
            continue;
        }

        const AurUpdateExecutionTarget& target = state.preflight.targets
                                                     [correlation.filtered_update_plan_index];
        if(target.update.installed_name != correlation.package_name ||
           !target.update.aur_package.has_value() ||
           target.update.aur_package->aur_name != correlation.package_name ||
           target.update.aur_package->package_base !=
               correlation.package_base) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    PreflightTargetMappingInconsistent,
                "Selected target identity differs from its preflight target.");
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.filtered_update_plan_index =
                correlation.filtered_update_plan_index;
            issue.package_name = correlation.package_name;
            issue.package_base = correlation.package_base;
            continue;
        }

        if(!state.preflight.build_plan.has_value()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildPlanRootIndexOutOfRange,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Selected preflight target has no {} snapshot.",
                    BUILD_PLAN_TYPE_NAME));
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.filtered_update_plan_index =
                correlation.filtered_update_plan_index;
            issue.package_name = correlation.package_name;
            continue;
        }

        const BuildPlan& plan = *state.preflight.build_plan;
        if(!target.build_plan_root_index.has_value()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildPlanRootIndexMissing,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Selected preflight target has no {} root index.",
                    BUILD_PLAN_TYPE_NAME));
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.filtered_update_plan_index =
                correlation.filtered_update_plan_index;
            issue.package_name = correlation.package_name;

            // preflightはidentity不一致時にroot indexをpublishしない。owned
            // BuildPlan上のselected位置を補助照合し、原因をmissingへ丸めない。
            const std::size_t expected_root_index =
                correlation.selected_target_index;
            if(expected_root_index >= plan.root_targets.size()) {
                FilteredAurUpdateOperationIssue& range_issue = add_localized_operation_issue(
                    state.issues,
                    FilteredAurUpdateOperationIssueKind::
                        BuildPlanRootIndexOutOfRange,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "Selected target position maps outside the {} root snapshot.",
                        BUILD_PLAN_TYPE_NAME));
                range_issue.original_query_plan_index =
                    correlation.original_query_plan_index;
                range_issue.selected_target_index =
                    correlation.selected_target_index;
                range_issue.build_plan_root_index = expected_root_index;
            } else {
                correlate_root_invocation_identity(
                    operation, correlation,
                    plan.root_targets[expected_root_index],
                    expected_root_index);
            }
            continue;
        }

        const std::size_t root_index = *target.build_plan_root_index;
        if(root_index >= plan.root_targets.size()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildPlanRootIndexOutOfRange,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Selected preflight target has an out-of-range {} root index.",
                    BUILD_PLAN_TYPE_NAME));
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.build_plan_root_index = root_index;
            continue;
        }

        const RootTargetIdentity& root = plan.root_targets[root_index];
        if(root_index != correlation.selected_target_index) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildPlanRootIdentityMismatch,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "{} root position differs from the selected target mapping.",
                    BUILD_PLAN_TYPE_NAME));
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.preflight_invocation_index = root.invocation_index;
            issue.build_plan_root_index = root_index;
            issue.package_name = root.requested_name;
            continue;
        }
        if(!correlate_root_invocation_identity(
               operation, correlation, root, root_index)) continue;

        const std::optional<std::size_t> root_target_index =
            find_unique_package_target_index(
                plan, root.requested_name, std::nullopt);
        const bool root_package_matches = root_target_index.has_value() &&
                                          has_role(
                                              plan.package_targets[*root_target_index],
                                              PackageRole::Root) &&
                                          contains_root(
                                              plan.package_targets[*root_target_index], root) &&
                                          plan.package_targets[*root_target_index].package_base ==
                                              correlation.package_base;
        if(!root_package_matches) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                state.issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildPlanRootPackageIdentityMismatch,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // internal type name "BuildPlan" and service name
                    // "AUR", respectively.
                    "{} root package target differs from the selected {} target identity.",
                    BUILD_PLAN_TYPE_NAME,
                    AUR_SERVICE_NAME));
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.preflight_invocation_index = root.invocation_index;
            issue.build_plan_root_index = root_index;
            issue.package_name = correlation.package_name;
            issue.package_base = correlation.package_base;
            continue;
        }

        correlation.preflight_invocation_index = root.invocation_index;
        correlation.build_plan_root_index = root_index;
    }
}

template <typename Operation>
bool has_operation_planning_issue(const Operation& operation) noexcept {
    return !operation.operation_issues().empty() ||
           has_upgrade_all_planning_issues(
               operation.target_and_build_unit_plan());
}

bool has_consistent_policy_snapshot(
    const std::optional<DevelRequiresCheckPolicy>& route_policy,
    const AurUpdateExecutionPreflight& preflight,
    const std::optional<DevelRequiresCheckPolicy>& downstream_policy) noexcept {
    return route_policy.has_value() &&
           is_known_devel_requires_check_policy(*route_policy) &&
           preflight.devel_requires_check_policy == route_policy &&
           has_valid_aur_update_execution_policy_snapshot(preflight) &&
           downstream_policy == route_policy;
}

void validate_preparation_snapshot_before_execution(
    PreparedFilteredAurUpdateOperation& operation) {
    auto state = FilteredAurUpdateOperationMutableAccess::snapshot(operation);
    if(!state.preparation.has_value() ||
       !state.preparation->invocation.has_value() ||
       !state.preparation->invocation->is_valid()) {
        return;
    }

    std::vector<const AurUpdateExecutionTarget*> executable_targets;
    for(const AurUpdateExecutionTarget& target : state.preflight.targets) {
        if(target.status == AurUpdateExecutionTargetStatus::Executable) {
            executable_targets.push_back(&target);
        }
    }
    bool snapshot_matches = executable_targets.size() ==
                            state.preparation->affected_update_targets.size();
    const std::size_t comparable_count = std::min(
        executable_targets.size(),
        state.preparation->affected_update_targets.size());
    for(std::size_t index = 0; index < comparable_count; ++index) {
        snapshot_matches = snapshot_matches && same_preflight_target(
                                                   *executable_targets[index],
                                                   state.preparation->affected_update_targets[index]);
    }

    if(!state.preflight.build_plan.has_value()) {
        snapshot_matches = false;
    } else {
        snapshot_matches = snapshot_matches &&
                           state.preflight.build_plan->root_targets ==
                               state.preparation->affected_roots;
    }
    snapshot_matches =
        snapshot_matches &&
        has_consistent_policy_snapshot(
            state.devel_requires_check_policy, state.preflight,
            state.preparation->devel_requires_check_policy);
    if(snapshot_matches) return;

    add_localized_operation_issue(
        state.issues,
        FilteredAurUpdateOperationIssueKind::
            PreflightTargetMappingInconsistent,
        localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared filtered {} operation preflight snapshot changed after preparation.",
            AUR_SERVICE_NAME));
}

bool has_executable_target(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return target.status ==
                   AurUpdateExecutionTargetStatus::Executable;
        });
}

AurUpdateSourceBuildPreparation make_planning_blocker(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& selection) {
    AurUpdateSourceBuildPreparation preparation;
    preparation.devel_requires_check_policy =
        preflight.devel_requires_check_policy;
    preparation.build_unit_selection = selection;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(target.status != AurUpdateExecutionTargetStatus::Executable) continue;
        preparation.affected_update_targets.push_back(target);
    }
    if(preflight.build_plan.has_value()) {
        preparation.affected_roots = preflight.build_plan->root_targets;
    }
    if(preparation.affected_update_targets.empty()) return preparation;

    AurUpdatePreparationIssue issue;
    issue.reason = AurUpdatePreparationReason::BuildUnitSelectionInconsistent;
    issue.diagnostic = localization::format_translated_message(
        // TRANSLATORS: {} is the literal service name "AUR".
        "Filtered {} operation planning or correlation failed before mutation preparation.",
        AUR_SERVICE_NAME);
    for(const AurUpdateExecutionTarget& target :
        preparation.affected_update_targets) {
        issue.affected_update_plan_indices.push_back(
            target.update_plan_index);
    }
    issue.affected_roots = preparation.affected_roots;
    preparation.issues.push_back(std::move(issue));
    return preparation;
}

AurUpdateSourceBuildObservation make_planning_observation_blocker(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& selection) {
    AurUpdateSourceBuildPreparation preparation =
        make_planning_blocker(preflight, selection);
    return AurUpdateSourceBuildObservation{
        std::move(preparation.issues),
        std::move(preparation.warnings),
        std::move(preparation.affected_update_targets),
        std::move(preparation.affected_roots),
        std::move(preparation.build_unit_selection),
        std::move(preparation.projected_build_units),
        std::move(preparation.externally_satisfied_build_units),
        {},
        std::nullopt,
        preparation.devel_requires_check_policy};
}

AurUpdateBuildUnitSelection make_build_unit_selection(
    const UpgradeAllPlan& plan,
    const std::vector<FilteredAurUpdateBuildUnitCorrelation>& correlations,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    AurUpdateBuildUnitSelection selection;
    selection.entries.reserve(plan.build_unit_dispositions.size());
    for(std::size_t position = 0;
        position < plan.build_unit_dispositions.size(); ++position) {
        const UpgradeAllBuildUnitPlanEntry& planned =
            plan.build_unit_dispositions[position];
        if(planned.original_build_plan_index != position ||
           position >= correlations.size() ||
           correlations[position].original_build_plan_index != position) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildUnitSelectionMappingInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Build-unit planner result order differs from {} order.",
                    BUILD_PLAN_TYPE_NAME));
            issue.build_plan_order_index = position;
            continue;
        }

        AurUpdateBuildUnitSelectionEntry entry;
        entry.build_plan_order_index = position;
        entry.package_base = correlations[position].package_base;
        entry.package_names = correlations[position].package_names;
        switch(planned.disposition) {
            case UpgradeAllBuildUnitDisposition::SelectedForAurExecution:
                entry.status = AurUpdateBuildUnitSelectionStatus::
                    SelectedForAurExecution;
                entry.selected_execution_index =
                    planned.selected_execution_index;
                if(!entry.selected_execution_index.has_value()) {
                    FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                        issues,
                        FilteredAurUpdateOperationIssueKind::
                            BuildUnitSelectionMappingInconsistent,
                        "Selected build unit has no dense execution index.");
                    issue.build_plan_order_index = position;
                }
                break;
            case UpgradeAllBuildUnitDisposition::
                ExternallySatisfiedByExplicitSourcePackageBase: {
                entry.status = AurUpdateBuildUnitSelectionStatus::
                    ExternallySatisfiedByExplicitSourcePackageBase;
                if(!planned.explicit_source.has_value()) {
                    FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                        issues,
                        FilteredAurUpdateOperationIssueKind::
                            BuildUnitSelectionMappingInconsistent,
                        "Externally satisfied build unit has no explicit source attribution.");
                    issue.build_plan_order_index = position;
                    break;
                }
                entry.external_satisfaction =
                    AurUpdateExternalSatisfactionAttribution{
                        planned.explicit_source->explicit_source_indexes,
                        planned.explicit_source->source_identity_keys,
                        planned.explicit_source->matched_package_name,
                        planned.explicit_source->matched_package_base};
                break;
            }
            case UpgradeAllBuildUnitDisposition::NotRequiredBySelectedTarget:
            case UpgradeAllBuildUnitDisposition::IdentityIncomplete:
            case UpgradeAllBuildUnitDisposition::
                ConflictingExplicitSourceIdentity:
            case UpgradeAllBuildUnitDisposition::ConflictingSelectedPackageBase: {
                FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                    issues,
                    FilteredAurUpdateOperationIssueKind::
                        BuildUnitSelectionMappingInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "Filtered preflight {} contains a non-executable build-unit disposition.",
                        BUILD_PLAN_TYPE_NAME));
                issue.build_plan_order_index = position;
                issue.package_base = correlations[position].package_base;
                break;
            }
        }
        selection.entries.push_back(std::move(entry));
    }
    return selection;
}

void map_selected_execution_indices(
    const UpgradeAllPlan& plan,
    std::vector<FilteredAurUpdateBuildUnitCorrelation>& correlations,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    for(const UpgradeAllBuildUnitPlanEntry& entry :
        plan.build_unit_dispositions) {
        if(entry.original_build_plan_index >= correlations.size()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    BuildUnitSelectionMappingInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Build-unit planner result maps outside the {} correlation snapshot.",
                    BUILD_PLAN_TYPE_NAME));
            issue.build_plan_order_index = entry.original_build_plan_index;
            continue;
        }
        correlations[entry.original_build_plan_index]
            .selected_execution_index = entry.selected_execution_index;
    }
}

void correlate_prepared_work_items(
    const AurUpdateSourceBuildPreparation& preparation,
    const std::optional<AurUpdateSourceBuildExecutionResult>& execution,
    std::vector<FilteredAurUpdateBuildUnitCorrelation>& correlations,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    if(preparation.invocation.has_value()) {
        for(const AurUpdatePreparedWorkItemAttribution& attribution :
            preparation.invocation->work_item_attributions()) {
            if(attribution.build_plan_order_index >= correlations.size()) {
                FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                    issues,
                    FilteredAurUpdateOperationIssueKind::
                        ExecutionBuildUnitMappingInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "Prepared work item maps outside the {} correlation snapshot.",
                        BUILD_PLAN_TYPE_NAME));
                issue.build_plan_order_index =
                    attribution.build_plan_order_index;
                issue.invocation_work_item_index =
                    attribution.invocation_work_item_index;
                continue;
            }
            FilteredAurUpdateBuildUnitCorrelation& correlation =
                correlations[attribution.build_plan_order_index];
            const bool identity_matches =
                correlation.package_base == attribution.package_base &&
                has_compatible_singular_package_name(
                    attribution.package_name,
                    correlation.package_names) &&
                has_exact_required_package_names(
                    attribution.required_target_attributions,
                    correlation.package_names) &&
                correlation.selected_execution_index ==
                    attribution.invocation_work_item_index;
            if(!identity_matches ||
               (correlation.invocation_work_item_index.has_value() &&
                correlation.invocation_work_item_index !=
                    attribution.invocation_work_item_index)) {
                FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                    issues,
                    FilteredAurUpdateOperationIssueKind::
                        ExecutionBuildUnitMappingInconsistent,
                    "Prepared work-item identity differs from its selected build unit.");
                issue.build_plan_order_index =
                    attribution.build_plan_order_index;
                issue.invocation_work_item_index =
                    attribution.invocation_work_item_index;
                issue.package_name = attribution.package_name;
                issue.package_base = attribution.package_base;
                continue;
            }
            correlation.invocation_work_item_index =
                attribution.invocation_work_item_index;
        }
    }

    if(!execution.has_value()) return;
    for(const AurUpdateWorkItemExecutionResult& work_item :
        execution->work_item_results) {
        if(work_item.build_plan_order_index >= correlations.size()) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    ExecutionBuildUnitMappingInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Execution work item maps outside the {} correlation snapshot.",
                    BUILD_PLAN_TYPE_NAME));
            issue.build_plan_order_index = work_item.build_plan_order_index;
            issue.invocation_work_item_index = work_item.work_item_index;
            continue;
        }
        const FilteredAurUpdateBuildUnitCorrelation& correlation =
            correlations[work_item.build_plan_order_index];
        if(correlation.invocation_work_item_index != work_item.work_item_index ||
           correlation.package_base != work_item.package_base ||
           correlation.package_names != work_item.plan_package_names ||
           !has_compatible_singular_package_name(
               work_item.package_name, correlation.package_names) ||
           !has_exact_execution_child_names(
               work_item, correlation.package_names)) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    ExecutionBuildUnitMappingInconsistent,
                "Execution work-item identity differs from its prepared build unit.");
            issue.build_plan_order_index = work_item.build_plan_order_index;
            issue.invocation_work_item_index = work_item.work_item_index;
            issue.package_name = work_item.package_name;
            issue.package_base = work_item.package_base;
        }
    }
}

std::vector<FilteredAurUpdateSelectedTargetResult> map_selected_results(
    const std::vector<FilteredAurUpdateTargetCorrelation>& correlations,
    const AurUpdateOperationResult& reduced,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    std::vector<FilteredAurUpdateSelectedTargetResult> results;
    results.reserve(correlations.size());
    for(const FilteredAurUpdateTargetCorrelation& correlation : correlations) {
        auto match = std::find_if(
            reduced.targets.begin(), reduced.targets.end(),
            [&correlation](const AurUpdateOperationTargetResult& target) {
                return target.update_plan_index ==
                       correlation.filtered_update_plan_index;
            });
        const bool identity_matches = match != reduced.targets.end() &&
                                      match->update.installed_name == correlation.package_name &&
                                      match->package_base == correlation.package_base;
        if(!identity_matches) {
            FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                issues,
                FilteredAurUpdateOperationIssueKind::
                    ReducedTargetMappingInconsistent,
                "Reduced selected target cannot be mapped back to its original query identity.");
            issue.original_query_plan_index =
                correlation.original_query_plan_index;
            issue.selected_target_index = correlation.selected_target_index;
            issue.filtered_update_plan_index =
                correlation.filtered_update_plan_index;
            issue.package_name = correlation.package_name;
            issue.package_base = correlation.package_base;
            continue;
        }

        results.push_back(FilteredAurUpdateSelectedTargetResult{
            correlation.selected_target_index,
            correlation.original_query_plan_index,
            correlation.filtered_update_plan_index,
            *match});
    }
    return results;
}

bool target_status_is_success(
    AurUpdateOperationTargetStatus status) noexcept {
    return status == AurUpdateOperationTargetStatus::Updated ||
           status == AurUpdateOperationTargetStatus::NoChange ||
           status == AurUpdateOperationTargetStatus::Skipped;
}

bool work_item_status_is_success(
    AurUpdateWorkItemExecutionStatus status) noexcept {
    return status == AurUpdateWorkItemExecutionStatus::Updated ||
           status == AurUpdateWorkItemExecutionStatus::NoChange;
}

bool invocation_status_matches_operation(
    const AurUpdateOperationResult& result) noexcept {
    switch(result.status) {
        case AurUpdateOperationStatus::NoUpdates:
            return !result.execution_status.has_value();
        case AurUpdateOperationStatus::Completed:
            return result.execution_status ==
                   AurUpdateInvocationExecutionStatus::Completed;
        case AurUpdateOperationStatus::BlockedBeforeExecution:
        case AurUpdateOperationStatus::StoppedOnProviderTransactionFailure:
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
        case AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure:
        case AurUpdateOperationStatus::InconsistentResult:
            return false;
    }
    return false;
}

} // namespace

FilteredAurUpdateTargetAdapter adapt_aur_update_plan_for_upgrade_all(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    std::vector<FilteredAurUpdateOperationIssue>& issues) {
    FilteredAurUpdateTargetAdapter adapter;
    if(!is_known_devel_requires_check_policy(
           devel_requires_check_policy)) {
        add_localized_operation_issue(
            issues,
            FilteredAurUpdateOperationIssueKind::
                DevelRequiresCheckPolicyInconsistent,
            std::string{DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
    }
    adapter.entries.reserve(update_plan.entries.size());
    adapter.original_query_plan_to_planner_target_index.assign(
        update_plan.entries.size(), std::nullopt);

    for(std::size_t original_index = 0;
        original_index < update_plan.entries.size(); ++original_index) {
        const AurUpdatePlanEntry& update = update_plan.entries[original_index];
        FilteredAurUpdateTargetAdapterEntry adapter_entry;
        adapter_entry.original_query_plan_index = original_index;
        adapter_entry.update = update;

        if(is_normal_skip(update)) {
            adapter_entry.disposition =
                FilteredAurUpdateTargetAdapterDisposition::NormalSkip;
            adapter.entries.push_back(std::move(adapter_entry));
            continue;
        }

        UpgradeAllAurTargetStatus status =
            UpgradeAllAurTargetStatus::Incomplete;
        std::string status_detail;
        switch(project_aur_update_effective_state(update)) {
            case AurUpdateEffectiveState::UpdateAvailable:
                status = update.aur_package.has_value()
                             ? UpgradeAllAurTargetStatus::Candidate
                             : UpgradeAllAurTargetStatus::Incomplete;
                status_detail = update.aur_package.has_value()
                                    ? localization::format_translated_message(
                                          // TRANSLATORS: {} is the literal service name "AUR".
                                          "{} update candidate",
                                          AUR_SERVICE_NAME)
                                    : localization::format_translated_message(
                                          // TRANSLATORS: {} is the literal service name "AUR".
                                          "{} update candidate has no remote package identity",
                                          AUR_SERVICE_NAME);
                break;
            case AurUpdateEffectiveState::RequiresCheck:
                if(devel_requires_check_policy ==
                   DevelRequiresCheckPolicy::SkipIndependentTarget) {
                    // POLICY(#508): Keep the complete query/update identity,
                    // but do not let a check-required target become a planner
                    // root. BuildPlan re-entry is checked by preflight after
                    // the remaining candidate roots have been resolved.
                    adapter_entry.disposition =
                        FilteredAurUpdateTargetAdapterDisposition::
                            RequiresCheckPolicySkip;
                    adapter_entry.devel_requires_check_policy =
                        devel_requires_check_policy;
                    adapter_entry.devel_requires_check_reason =
                        *update.devel_assessment
                             .requires_check_reason();
                    adapter.entries.push_back(std::move(adapter_entry));
                    continue;
                }
                status = UpgradeAllAurTargetStatus::Incomplete;
                status_detail = localization::translate_message(
                    "Devel package update status requires check.");
                break;
            case AurUpdateEffectiveState::MetadataUnavailable:
                status = UpgradeAllAurTargetStatus::Incomplete;
                status_detail = localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "{} metadata unavailable", AUR_SERVICE_NAME);
                break;
            case AurUpdateEffectiveState::VersionComparisonUnavailable:
                status = UpgradeAllAurTargetStatus::Incomplete;
                status_detail = localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "{} version comparison unavailable", AUR_SERVICE_NAME);
                break;
            case AurUpdateEffectiveState::UpToDate:
            case AurUpdateEffectiveState::NonAurForeign:
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: {} is the literal command name "upgrade-all".
                    "Normal skip unexpectedly reached the {} target adapter.",
                    UPGRADE_ALL_COMMAND_NAME));
            case AurUpdateEffectiveState::Unknown:
                status = UpgradeAllAurTargetStatus::Incomplete;
                status_detail = localization::translate_message("Devel Git observation failed; no automatic build.");
                break;
            case AurUpdateEffectiveState::Unsupported:
                status = UpgradeAllAurTargetStatus::Incomplete;
                status_detail = localization::translate_message("Devel source is unsupported for automatic updates.");
                break;
            case AurUpdateEffectiveState::Inconsistent: {
                status = UpgradeAllAurTargetStatus::Incomplete;
                status_detail = localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "Unknown {} update classification", AUR_SERVICE_NAME);
                FilteredAurUpdateOperationIssue& issue = add_localized_operation_issue(
                    issues,
                    FilteredAurUpdateOperationIssueKind::
                        UnknownUpdateClassification,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "AurUpdatePlan".
                        "{} entry has an unknown classification.",
                        AUR_UPDATE_PLAN_TYPE_NAME));
                issue.original_query_plan_index = original_index;
                issue.package_name = update.installed_name;
                break;
            }
        }

        const std::size_t planner_index = adapter.planner_targets.size();
        adapter_entry.disposition =
            FilteredAurUpdateTargetAdapterDisposition::PlannerTarget;
        adapter_entry.planner_target_index = planner_index;
        adapter.planner_targets.push_back(UpgradeAllAurTarget{
            update.installed_name,
            package_base_identity(update),
            status,
            std::move(status_detail)});
        adapter.planner_target_to_original_query_plan_index.push_back(
            original_index);
        adapter.original_query_plan_to_planner_target_index[original_index] =
            planner_index;
        adapter.entries.push_back(std::move(adapter_entry));
    }
    return adapter;
}

PreparedFilteredAurUpdateOperation::PreparedFilteredAurUpdateOperation(
    PreparedFilteredAurUpdateOperation&& other) noexcept
    : valid_(std::exchange(other.valid_, false)),
      devel_requires_check_policy(
          std::move(other.devel_requires_check_policy)),
      query_result(std::move(other.query_result)),
      target_adapter(std::move(other.target_adapter)),
      upgrade_all_plan(std::move(other.upgrade_all_plan)),
      filtered_update_plan(std::move(other.filtered_update_plan)),
      filtered_to_original_query_plan_index(
          std::move(other.filtered_to_original_query_plan_index)),
      original_query_plan_to_filtered_index(
          std::move(other.original_query_plan_to_filtered_index)),
      target_correlations(std::move(other.target_correlations)),
      preflight(std::move(other.preflight)),
      build_unit_correlations(std::move(other.build_unit_correlations)),
      preparation(std::move(other.preparation)),
      issues(std::move(other.issues)) {
}

const FilteredAurUpdateTargetAdapter&
PreparedFilteredAurUpdateOperation::target_adapter_result() const noexcept {
    return target_adapter;
}

const UpgradeAllPlan&
PreparedFilteredAurUpdateOperation::target_and_build_unit_plan()
    const noexcept {
    return upgrade_all_plan;
}

const AurUpdatePlan&
PreparedFilteredAurUpdateOperation::filtered_plan() const noexcept {
    return filtered_update_plan;
}

const std::vector<std::size_t>&
PreparedFilteredAurUpdateOperation::filtered_to_original_indexes()
    const noexcept {
    return filtered_to_original_query_plan_index;
}

const std::vector<std::optional<std::size_t>>&
PreparedFilteredAurUpdateOperation::original_to_filtered_indexes()
    const noexcept {
    return original_query_plan_to_filtered_index;
}

const std::vector<FilteredAurUpdateTargetCorrelation>&
PreparedFilteredAurUpdateOperation::selected_target_correlations()
    const noexcept {
    return target_correlations;
}

const std::vector<FilteredAurUpdateBuildUnitCorrelation>&
PreparedFilteredAurUpdateOperation::build_unit_mapping() const noexcept {
    return build_unit_correlations;
}

const std::vector<FilteredAurUpdateOperationIssue>&
PreparedFilteredAurUpdateOperation::operation_issues() const noexcept {
    return issues;
}

bool PreparedFilteredAurUpdateOperation::is_valid() const noexcept {
    return valid_;
}

bool PreparedFilteredAurUpdateOperation::is_prepared() const noexcept {
    return valid_ && devel_requires_check_policy.has_value() &&
           is_known_devel_requires_check_policy(
               *devel_requires_check_policy) &&
           preflight.devel_requires_check_policy ==
               devel_requires_check_policy &&
           !has_blocking_targets(preflight) &&
           !has_operation_planning_issue(*this) &&
           preparation.has_value() &&
           preparation->devel_requires_check_policy ==
               devel_requires_check_policy &&
           preparation->is_prepared();
}

bool PreparedFilteredAurUpdateOperation::is_noop() const noexcept {
    return valid_ && devel_requires_check_policy.has_value() &&
           is_known_devel_requires_check_policy(
               *devel_requires_check_policy) &&
           preflight.devel_requires_check_policy ==
               devel_requires_check_policy &&
           !has_blocking_targets(preflight) &&
           !has_operation_planning_issue(*this) &&
           preparation.has_value() &&
           preparation->devel_requires_check_policy ==
               devel_requires_check_policy &&
           preparation->is_noop();
}

bool PreparedFilteredAurUpdateOperation::is_blocked() const noexcept {
    return valid_ && !is_prepared() && !is_noop();
}

PreparedFilteredAurUpdateOperation prepare_filtered_aur_update_operation(
    AurUpdateQueryResult query_result,
    FilteredAurUpdateExplicitSourceSatisfaction
        explicit_source_satisfaction,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    const AppConfig& config,
    std::optional<ValidatedCacheRoot> cache_root) {
    PreparedFilteredAurUpdateOperation operation;
    operation.devel_requires_check_policy =
        devel_requires_check_policy;
    if(cache_root.has_value()) {
        cache_root->require_unchanged_identity();
    }
    operation.query_result = std::move(query_result);
    operation.target_adapter = adapt_aur_update_plan_for_upgrade_all(
        operation.query_result.plan, devel_requires_check_policy,
        operation.issues);
    std::vector<UpgradeAllExplicitSourceIdentity> explicit_sources =
        materialize_explicit_source_satisfaction(
            std::move(explicit_source_satisfaction));
    operation.upgrade_all_plan = make_upgrade_all_target_plan(
        explicit_sources, operation.target_adapter.planner_targets);

    build_filtered_update_plan(operation);
    operation.preflight = resolve_aur_update_execution_preflight(
        operation.filtered_update_plan,
        devel_requires_check_policy,
        provider_selection_callback(config));
    correlate_preflight(operation);

    BuildUnitAdapterResult build_adapter;
    if(operation.preflight.build_plan.has_value()) {
        build_adapter = adapt_build_plan(
            *operation.preflight.build_plan,
            operation.target_correlations,
            operation.issues);
    }
    operation.build_unit_correlations =
        std::move(build_adapter.correlations);
    operation.upgrade_all_plan = complete_upgrade_all_build_unit_plan(
        operation.upgrade_all_plan, build_adapter.build_units);
    map_selected_execution_indices(
        operation.upgrade_all_plan,
        operation.build_unit_correlations,
        operation.issues);

    AurUpdateBuildUnitSelection selection = make_build_unit_selection(
        operation.upgrade_all_plan,
        operation.build_unit_correlations,
        operation.issues);
    if(cache_root.has_value()) {
        // Preflight/planning can be non-trivial. Revoke a moved cache root
        // before saved-preference policy and pacman database preparation begins.
        cache_root->require_unchanged_identity();
    }
    if(has_operation_planning_issue(operation) &&
       has_executable_target(operation.preflight) &&
       !has_blocking_targets(operation.preflight)) {
        operation.preparation.emplace(make_planning_blocker(
            operation.preflight, selection));
    } else if(has_operation_planning_issue(operation)) {
        // preflight blockerは既存typed issueを正本にし、preference authority/DBへ進まない。
        operation.preparation.emplace(
            prepare_aur_update_source_build_invocation(
                operation.preflight, devel_requires_check_policy,
                saved_source_preference_policy, false, config));
    } else {
        operation.preparation.emplace(
            prepare_aur_update_source_build_invocation(
                operation.preflight, selection,
                devel_requires_check_policy,
                saved_source_preference_policy, false, config));
    }
    if(cache_root.has_value()) {
        seed_aur_update_source_build_cache(
            operation.preparation.value(), cache_root.value());
    }
    return operation;
}

bool FilteredAurUpdateObservation::is_ready() const noexcept {
    return has_consistent_devel_requires_check_policy_snapshot() &&
           !has_blocking_targets(preflight) &&
           issues.empty() && source_build_observation.has_value() &&
           source_build_observation->is_ready();
}

bool FilteredAurUpdateObservation::is_noop() const noexcept {
    return has_consistent_devel_requires_check_policy_snapshot() &&
           !has_blocking_targets(preflight) &&
           issues.empty() && source_build_observation.has_value() &&
           source_build_observation->is_noop();
}

bool FilteredAurUpdateObservation::is_blocked() const noexcept {
    return !is_ready() && !is_noop();
}

bool FilteredAurUpdateObservation::
    has_consistent_devel_requires_check_policy_snapshot() const noexcept {
    return source_build_observation.has_value() &&
           has_consistent_policy_snapshot(
               devel_requires_check_policy, preflight,
               source_build_observation
                   ->devel_requires_check_policy);
}

FilteredAurUpdateObservation observe_filtered_aur_update_operation(
    AurUpdateQueryResult query_result,
    FilteredAurUpdateExplicitSourceSatisfaction
        explicit_source_satisfaction,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    const AppConfig& config) {
    FilteredAurUpdateObservation observation;
    observation.devel_requires_check_policy =
        devel_requires_check_policy;
    observation.query_result = std::move(query_result);
    observation.target_adapter = adapt_aur_update_plan_for_upgrade_all(
        observation.query_result.plan, devel_requires_check_policy,
        observation.issues);
    std::vector<UpgradeAllExplicitSourceIdentity> explicit_sources =
        materialize_explicit_source_satisfaction(
            std::move(explicit_source_satisfaction));
    observation.upgrade_all_plan = make_upgrade_all_target_plan(
        explicit_sources, observation.target_adapter.planner_targets);

    build_filtered_update_plan(observation);
    observation.preflight = resolve_aur_update_execution_preflight(
        observation.filtered_update_plan,
        devel_requires_check_policy,
        provider_selection_callback(config));
    correlate_preflight(observation);

    BuildUnitAdapterResult build_adapter;
    if(observation.preflight.build_plan.has_value()) {
        build_adapter = adapt_build_plan(
            *observation.preflight.build_plan,
            observation.target_correlations,
            observation.issues);
    }
    observation.build_unit_correlations =
        std::move(build_adapter.correlations);
    observation.upgrade_all_plan = complete_upgrade_all_build_unit_plan(
        observation.upgrade_all_plan, build_adapter.build_units);
    map_selected_execution_indices(
        observation.upgrade_all_plan,
        observation.build_unit_correlations,
        observation.issues);

    AurUpdateBuildUnitSelection selection = make_build_unit_selection(
        observation.upgrade_all_plan,
        observation.build_unit_correlations,
        observation.issues);
    if(has_operation_planning_issue(observation) &&
       has_executable_target(observation.preflight) &&
       !has_blocking_targets(observation.preflight)) {
        observation.source_build_observation.emplace(
            make_planning_observation_blocker(
                observation.preflight, selection));
    } else if(has_operation_planning_issue(observation)) {
        observation.source_build_observation.emplace(
            observe_aur_update_source_build_preparation(
                observation.preflight,
                devel_requires_check_policy,
                saved_source_preference_policy, false, config));
    } else {
        observation.source_build_observation.emplace(
            observe_aur_update_source_build_preparation(
                observation.preflight, selection,
                devel_requires_check_policy,
                saved_source_preference_policy, false, config));
    }
    return observation;
}

void seed_filtered_aur_update_operation_cache(
    PreparedFilteredAurUpdateOperation& prepared,
    const ValidatedCacheRoot& cache_root) {
    cache_root.require_unchanged_identity();
    seed_aur_update_source_build_cache(
        prepared.preparation.value(), cache_root);
}

FilteredAurUpdateExecutionResult execute_prepared_filtered_aur_update_operation(
    PreparedFilteredAurUpdateOperation prepared,
    const AppConfig& config) {
    if(!prepared.valid_) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared filtered {} update operation is invalid or has already been consumed.",
            AUR_SERVICE_NAME));
    }
    if(!prepared.preparation.has_value()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared filtered {} update operation lost its source-build preparation snapshot.",
            AUR_SERVICE_NAME));
    }

    const bool has_prepared_invocation_snapshot =
        prepared.preparation->invocation.has_value() &&
        prepared.preparation->invocation->is_valid();
    if(has_prepared_invocation_snapshot) {
        // POLICY(#281): private owned snapshotsも、最初のexternal mutationより
        // 前にpreflight/index/identity correlationを再検証する。
        correlate_preflight(prepared);
        validate_preparation_snapshot_before_execution(prepared);
        correlate_prepared_work_items(
            *prepared.preparation,
            std::nullopt,
            prepared.build_unit_correlations,
            prepared.issues);
    }
    const bool should_execute = prepared.is_prepared();
    prepared.valid_ = false;
    std::optional<AurUpdateSourceBuildExecutionResult> execution;
    if(should_execute) {
        // LANDMINE(#281): aggregate snapshotを保持し、one-shot invocationだけをconsumeする。
        execution.emplace(
            execute_prepared_aur_update_source_build_invocation(
                std::move(*prepared.preparation->invocation), config));
    }

    correlate_prepared_work_items(
        *prepared.preparation,
        execution,
        prepared.build_unit_correlations,
        prepared.issues);
    AurUpdateOperationResult reduced = reduce_aur_update_operation_result(
        prepared.preflight, *prepared.preparation,
        prepared.devel_requires_check_policy.value_or(
            static_cast<DevelRequiresCheckPolicy>(-1)),
        execution);
    std::vector<FilteredAurUpdateSelectedTargetResult> selected_results =
        map_selected_results(
            prepared.target_correlations,
            reduced,
            prepared.issues);

    return FilteredAurUpdateExecutionResult{
        std::move(prepared.query_result),
        std::move(prepared.target_adapter),
        std::move(prepared.upgrade_all_plan),
        std::move(prepared.filtered_update_plan),
        std::move(prepared.filtered_to_original_query_plan_index),
        std::move(prepared.original_query_plan_to_filtered_index),
        std::move(prepared.target_correlations),
        std::move(prepared.preflight),
        std::move(prepared.build_unit_correlations),
        std::move(*prepared.preparation),
        std::move(execution),
        std::move(reduced),
        std::move(selected_results),
        std::move(prepared.issues),
        std::move(prepared.devel_requires_check_policy)};
}

bool FilteredAurUpdateExecutionResult::is_success() const noexcept {
    if(!has_consistent_devel_requires_check_policy_snapshot() ||
       has_query_failure() || has_planning_issue() ||
       !reduced_operation_result.is_success() ||
       !invocation_status_matches_operation(reduced_operation_result) ||
       !reduced_operation_result.preparation_issues.empty() ||
       !reduced_operation_result.reduction_issues.empty() ||
       reduced_operation_result.has_blocking_targets() ||
       reduced_operation_result.has_cleanup_failure() ||
       reduced_operation_result.has_not_attempted_targets()) {
        return false;
    }
    if(!std::all_of(
           reduced_operation_result.targets.begin(),
           reduced_operation_result.targets.end(),
           [](const AurUpdateOperationTargetResult& target) {
               return target_status_is_success(target.status);
           })) {
        return false;
    }
    return std::all_of(
        reduced_operation_result.execution_work_items.begin(),
        reduced_operation_result.execution_work_items.end(),
        [](const AurUpdateWorkItemExecutionResult& work_item) {
            return work_item_status_is_success(work_item.status) &&
                   work_item.failure_kind ==
                       AurUpdateWorkItemFailureKind::None;
        });
}

PackageStateChange
FilteredAurUpdateExecutionResult::package_state_change() const noexcept {
    return reduced_operation_result.package_state_change();
}

bool FilteredAurUpdateExecutionResult::changed_package_state() const noexcept {
    return reduced_operation_result.changed_package_state();
}

bool FilteredAurUpdateExecutionResult::has_partial_completion() const noexcept {
    return reduced_operation_result.has_partial_completion();
}

bool FilteredAurUpdateExecutionResult::has_not_attempted_targets()
    const noexcept {
    return reduced_operation_result.has_not_attempted_targets();
}

bool FilteredAurUpdateExecutionResult::has_cleanup_failure() const noexcept {
    return reduced_operation_result.has_cleanup_failure();
}

bool FilteredAurUpdateExecutionResult::has_query_failure() const noexcept {
    return !query_result.recoverable_failures.empty();
}

bool FilteredAurUpdateExecutionResult::has_planning_issue() const noexcept {
    return !issues.empty() ||
           has_upgrade_all_planning_issues(upgrade_all_plan);
}

bool FilteredAurUpdateExecutionResult::has_duplicate_exclusions()
    const noexcept {
    return !upgrade_all_plan.excluded_duplicate_target_indexes.empty();
}

bool FilteredAurUpdateExecutionResult::
    has_consistent_devel_requires_check_policy_snapshot() const noexcept {
    return has_consistent_policy_snapshot(
               devel_requires_check_policy, preflight,
               preparation.devel_requires_check_policy) &&
           reduced_operation_result.devel_requires_check_policy ==
               devel_requires_check_policy;
}
