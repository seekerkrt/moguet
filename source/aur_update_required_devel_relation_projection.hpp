#pragma once

#include "aur_update_execution_preflight.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Pure semantic record used to compare the BuildPlan-derived required
// RequiresCheck relation set with the producer snapshot. Presentation text is
// intentionally excluded from this authority.
struct AurUpdateExpectedRequiredDevelRelation {
    std::size_t owner_update_plan_index = 0;
    AurUpdateRequiredDevelTargetBlocker blocker;
    std::optional<std::string> dependency_specification;

    AurUpdateExpectedRequiredDevelRelation(
        std::size_t owner_update_plan_index_value,
        AurUpdateRequiredDevelTargetBlocker blocker_value,
        std::optional<std::string> dependency_specification_value)
        : owner_update_plan_index(owner_update_plan_index_value),
          blocker(std::move(blocker_value)),
          dependency_specification(
              std::move(dependency_specification_value)) {
    }

    bool operator==(
        const AurUpdateExpectedRequiredDevelRelation&) const = default;
};

struct AurUpdateRequiredDevelRelationProjection {
    std::vector<AurUpdateExpectedRequiredDevelRelation> relations;
    std::vector<std::size_t> required_update_plan_indices;
    bool requires_global_fail_closed = false;
    bool authority_is_complete = true;
};

// CONTRACT(#508): expected relations are derived only from the full update
// payload and typed BuildPlan. Target status, skip kind, and existing blocker
// presence are never inputs to this projection.
AurUpdateRequiredDevelRelationProjection
project_aur_update_required_devel_relations(
    const AurUpdateExecutionPreflight& preflight);

// Complete equality covers both directions: every expected relation must have
// one producer blocker, and every producer blocker must be expected. This is a
// pre-mutation firewall as well as a reducer verification boundary.
bool has_complete_aur_update_required_devel_relation_snapshot(
    const AurUpdateExecutionPreflight& preflight);

// LANDMINE(#508): This non-trivial implementation remains inline so the
// isolated preparation/result link firewalls can share exactly one semantic
// authority without importing the production resolver or changing the
// repository-wide test descriptor ledger. Keep detail symbols in this named
// namespace and keep every definition inline.
namespace aur_update_required_devel_relation_detail {

struct RequiresCheckTargetIdentity {
    std::size_t update_plan_index = 0;
    std::string installed_name;
    std::string installed_version;
    std::string aur_name;
    std::string package_base;
    std::string remote_version;
    DevelRequiresCheckReason reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    bool is_bootstrap_trial = false;
};

struct CandidateMapping {
    std::size_t candidate_index = 0;
    std::size_t update_plan_index = 0;
    std::string requested_name;
};

struct PendingRequiredDevelRelation {
    AurUpdateRequiredDevelTargetBlocker blocker;
    std::optional<std::string> dependency_specification;

    PendingRequiredDevelRelation(
        AurUpdateRequiredDevelTargetBlocker blocker_value,
        std::optional<std::string> dependency_specification_value)
        : blocker(std::move(blocker_value)),
          dependency_specification(
              std::move(dependency_specification_value)) {
    }
};

inline bool is_update_candidate(const AurUpdatePlanEntry& update) noexcept {
    // Execution roots may include a trial; effective update state is unchanged.
    return has_aur_update_execution_intent(update);
}

inline bool is_requires_check_candidate(
    const AurUpdatePlanEntry& update) noexcept {
    const auto* observed_reason = update.devel_assessment.requires_check_reason();
    if(!observed_reason) return false;
    const auto reason = *observed_reason;
    return update.classification == AurUpdateClassification::UpToDate &&
           update.devel_assessment ==
               DevelUpdateAssessment::requires_check(reason) &&
           has_valid_aur_update_requires_check_identity_snapshot(
               update, reason);
}

inline bool root_identity_less(
    const RootTargetIdentity& lhs,
    const RootTargetIdentity& rhs) {
    if(lhs.invocation_index != rhs.invocation_index) {
        return lhs.invocation_index < rhs.invocation_index;
    }
    return lhs.requested_name < rhs.requested_name;
}

inline std::vector<RootTargetIdentity> normalize_roots(
    const std::vector<RootTargetIdentity>& roots) {
    std::vector<RootTargetIdentity> normalized;
    for(const RootTargetIdentity& root : roots) {
        if(std::find(normalized.begin(), normalized.end(), root) ==
           normalized.end()) {
            normalized.push_back(root);
        }
    }
    std::sort(normalized.begin(), normalized.end(), root_identity_less);
    return normalized;
}

inline bool is_known_plan_root(
    const BuildPlan& plan,
    const RootTargetIdentity& root) {
    return std::count(
               plan.root_targets.begin(), plan.root_targets.end(), root) ==
           1;
}

inline const PlannedPackageTarget* find_unique_package_target(
    const BuildPlan& plan,
    const std::string& package_name,
    const std::optional<std::string>& package_base) {
    const PlannedPackageTarget* match = nullptr;
    for(const PlannedPackageTarget& target : plan.package_targets) {
        if(target.package_name != package_name ||
           (package_base.has_value() &&
            target.package_base != *package_base)) {
            continue;
        }
        if(match != nullptr) return nullptr;
        match = &target;
    }
    return match;
}

inline bool has_package_role(
    const PlannedPackageTarget& target,
    PackageRole role) {
    return std::find(target.roles.begin(), target.roles.end(), role) !=
           target.roles.end();
}

inline bool is_dependency_role(PackageRole role) noexcept {
    return role == PackageRole::RuntimeDependency ||
           role == PackageRole::BuildDependency ||
           role == PackageRole::CheckDependency;
}

inline bool contains_all_roots(
    const std::vector<RootTargetIdentity>& available,
    const std::vector<RootTargetIdentity>& required) {
    return std::all_of(
        required.begin(), required.end(),
        [&available](const RootTargetIdentity& root) {
            return std::find(available.begin(), available.end(), root) !=
                   available.end();
        });
}

inline std::vector<RootTargetIdentity> exact_parent_roots(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge) {
    const PlannedPackageTarget* parent = find_unique_package_target(
        plan, edge.parent_package_name,
        std::optional<std::string>{edge.parent_package_base});
    if(parent == nullptr || parent->roots.empty()) return {};

    std::vector<RootTargetIdentity> roots = normalize_roots(parent->roots);
    if(roots.size() != parent->roots.size() ||
       std::any_of(
           roots.begin(), roots.end(),
           [&plan](const RootTargetIdentity& root) {
               return !is_known_plan_root(plan, root);
           })) {
        return {};
    }
    return roots;
}

inline bool planned_aur_target_matches_edge(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge,
    const std::string& package_name,
    const std::string& package_base,
    const std::vector<RootTargetIdentity>& parent_roots,
    const PlannedPackageTarget** matched_target) {
    const PlannedPackageTarget* target = find_unique_package_target(
        plan, package_name, std::optional<std::string>{package_base});
    if(target == nullptr || !has_package_role(*target, edge.role)) {
        return false;
    }
    const std::vector<RootTargetIdentity> target_roots =
        normalize_roots(target->roots);
    if(target_roots.size() != target->roots.size() ||
       !contains_all_roots(target_roots, parent_roots)) {
        return false;
    }
    if(matched_target != nullptr) *matched_target = target;
    return true;
}

inline const ConsumerDependencyRequirement* consumer_requirement(
    const BuildPlanDependencyEdge& edge) {
    if(!edge.requirement.has_value()) return nullptr;
    return std::get_if<ConsumerDependencyRequirement>(
        &edge.requirement.value());
}

inline bool has_known_constraint_evaluation(
    const BuildPlanDependencyEdge& edge) noexcept {
    if(!edge.constraint_evaluation.has_value()) return false;
    switch(edge.constraint_evaluation->satisfaction()) {
        case ConstraintSatisfaction::Unconstrained:
        case ConstraintSatisfaction::Satisfied:
        case ConstraintSatisfaction::Unsatisfied:
        case ConstraintSatisfaction::Unknown:
        case ConstraintSatisfaction::Invalid:
        case ConstraintSatisfaction::Conflicting:
            return true;
    }
    return false;
}

inline bool observed_version_matches(
    const ObservedVersion& observed,
    ObservedVersionSource expected_source,
    const std::string& expected_version) {
    const std::string* version = observed.version();
    return observed.source() == expected_source && version != nullptr &&
           *version == expected_version;
}

inline bool observed_version_is_available_from(
    const ObservedVersion& observed,
    ObservedVersionSource expected_source) {
    const std::string* version = observed.version();
    return observed.source() == expected_source && version != nullptr &&
           !version->empty();
}

inline bool contains_control_character(const std::string& value) noexcept {
    return std::any_of(
        value.begin(), value.end(), [](const unsigned char character) {
            return std::iscntrl(character) != 0;
        });
}

inline bool repository_identity_matches_plan(
    const BuildPlan& plan,
    const ConfiguredRepositoryIdentity& repository) {
    return plan.configured_repository_order.has_value() &&
           repository.configured_order <
               plan.configured_repository_order->size() &&
           !repository.repository_name.empty() &&
           !contains_control_character(repository.repository_name) &&
           (*plan.configured_repository_order)
                   [repository.configured_order] ==
               repository.repository_name;
}

inline bool provider_capability_version_is_coherent(
    const ProvidedDependency& provider,
    ObservedVersionSource expected_source) {
    if(!provider.constraint_metadata.has_value()) return false;
    const ProviderConstraintMetadata& metadata =
        *provider.constraint_metadata;
    const ProviderCapability& capability =
        metadata.provided_capability;
    if(capability.package_name() !=
           provider.provided_dependency_name ||
       capability.raw_specification() !=
           provider.provided_dependency_specification ||
       metadata.provided_version.source() != expected_source) {
        return false;
    }
    if(capability.version().has_value()) {
        const std::string* provided_version =
            metadata.provided_version.version();
        return provided_version != nullptr &&
               *provided_version == *capability.version();
    }
    const ObservedVersionUnknownReason* unknown_reason =
        metadata.provided_version.unknown_reason();
    return unknown_reason != nullptr &&
           *unknown_reason == ObservedVersionUnknownReason::
                                  UnversionedProviderCapability;
}

inline bool edge_mentions_package_name(
    const BuildPlanDependencyEdge& edge,
    const std::string& package_name) {
    if(const ConsumerDependencyRequirement* requirement =
           consumer_requirement(edge);
       requirement != nullptr &&
       requirement->package_name() == package_name) {
        return true;
    }
    // POLICY(#508): raw parsing is discovery-only when typed evidence is
    // missing or drifted. It can widen fail-closed IdentityDrift detection,
    // but no accepted dependency/provider relation is constructed from it.
    const DependencyRequirementParseResult parsed =
        parse_dependency_requirement(edge.dependency_spec);
    if(parsed.requirement() != nullptr) {
        if(const auto* requirement =
               std::get_if<ConsumerDependencyRequirement>(
                   parsed.requirement());
           requirement != nullptr &&
           requirement->package_name() == package_name) {
            return true;
        }
    }
    if(edge.resolved_package_name ==
           std::optional<std::string>{package_name} ||
       (edge.resolved_provider.has_value() &&
        edge.resolved_provider->package_name == package_name)) {
        return true;
    }
    if(!edge.resolved_candidate.has_value()) return false;
    return std::visit(
        [&package_name](const auto& candidate) {
            using Candidate = std::decay_t<decltype(candidate)>;
            if constexpr(std::is_same_v<
                             Candidate,
                             ProviderResolvedDependencyCandidate>) {
                return candidate.provider.package_name == package_name;
            } else {
                return candidate.package_name == package_name;
            }
        },
        *edge.resolved_candidate);
}

inline bool exact_aur_dependency_matches(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge,
    const RequiresCheckTargetIdentity& requires_check,
    const std::vector<RootTargetIdentity>& roots,
    const PlannedPackageTarget** matched_target) {
    const ConsumerDependencyRequirement* requirement =
        consumer_requirement(edge);
    const auto* candidate =
        edge.resolved_candidate.has_value()
            ? std::get_if<AurResolvedDependencyCandidate>(
                  &edge.resolved_candidate.value())
            : nullptr;
    return edge.kind == DependencyKind::Aur && requirement != nullptr &&
           requirement->package_name() == requires_check.aur_name &&
           edge.resolved_package_name ==
               std::optional<std::string>{requires_check.aur_name} &&
           edge.resolved_package_base ==
               std::optional<std::string>{requires_check.package_base} &&
           !edge.resolved_provider.has_value() && candidate != nullptr &&
           candidate->package_name == requires_check.aur_name &&
           candidate->package_base == requires_check.package_base &&
           observed_version_matches(
               candidate->package_version,
               ObservedVersionSource::AurExactPackage,
               requires_check.remote_version) &&
           has_known_constraint_evaluation(edge) && !roots.empty() &&
           planned_aur_target_matches_edge(
               plan, edge, requires_check.aur_name,
               requires_check.package_base, roots, matched_target);
}

inline bool aur_provider_matches(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge,
    const RequiresCheckTargetIdentity& requires_check,
    const std::vector<RootTargetIdentity>& roots,
    const PlannedPackageTarget** matched_target) {
    if(edge.kind != DependencyKind::Provided ||
       !edge.resolved_provider.has_value() ||
       !std::holds_alternative<AurProviderOrigin>(
           edge.resolved_provider->origin) ||
       edge.resolved_package_name.has_value() ||
       edge.resolved_package_base.has_value()) {
        return false;
    }
    const ProvidedDependency& provider = *edge.resolved_provider;
    const ConsumerDependencyRequirement* requirement =
        consumer_requirement(edge);
    const auto* candidate =
        edge.resolved_candidate.has_value()
            ? std::get_if<ProviderResolvedDependencyCandidate>(
                  &edge.resolved_candidate.value())
            : nullptr;
    if(requirement == nullptr || candidate == nullptr ||
       provider.package_name != requires_check.aur_name ||
       provider.package_base != requires_check.package_base ||
       provider.package_version !=
           std::optional<std::string>{requires_check.remote_version} ||
       provider.provided_dependency_name !=
           requirement->package_name() ||
       !provider.constraint_metadata.has_value() ||
       provider.constraint_metadata->provided_capability.package_name() !=
           requirement->package_name() ||
       !provider_capability_version_is_coherent(
           provider, ObservedVersionSource::AurProviderCapability) ||
       !observed_version_matches(
           provider.constraint_metadata->package_version,
           ObservedVersionSource::AurExactPackage,
           requires_check.remote_version) ||
       candidate->provider != provider ||
       candidate->provided_version !=
           provider.constraint_metadata->provided_version ||
       !has_known_constraint_evaluation(edge) || roots.empty()) {
        return false;
    }
    switch(edge.provider_resolution) {
        case ProviderResolutionKind::Unique:
        case ProviderResolutionKind::UserSelected:
            break;
        default:
            return false;
    }
    return planned_aur_target_matches_edge(
        plan, edge, requires_check.aur_name,
        requires_check.package_base, roots, matched_target);
}

inline bool repository_exact_matches(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge,
    const RequiresCheckTargetIdentity& requires_check) {
    const ConsumerDependencyRequirement* requirement =
        consumer_requirement(edge);
    const auto* candidate =
        edge.resolved_candidate.has_value()
            ? std::get_if<RepositoryExactPackage>(
                  &edge.resolved_candidate.value())
            : nullptr;
    return edge.kind == DependencyKind::Repo && requirement != nullptr &&
           requirement->package_name() == requires_check.aur_name &&
           edge.resolved_package_name ==
               std::optional<std::string>{requires_check.aur_name} &&
           !edge.resolved_package_base.has_value() &&
           !edge.resolved_provider.has_value() && candidate != nullptr &&
           candidate->package_name == requires_check.aur_name &&
           has_valid_requires_check_package_identity_shape(
               candidate->package_base) &&
           repository_identity_matches_plan(plan, candidate->repository) &&
           observed_version_is_available_from(
               candidate->package_version,
               ObservedVersionSource::RepositoryExactPackage) &&
           has_known_constraint_evaluation(edge);
}

inline bool repository_provider_matches(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge,
    const RequiresCheckTargetIdentity& requires_check) {
    if(edge.kind != DependencyKind::Provided ||
       !edge.resolved_provider.has_value() ||
       edge.resolved_package_name.has_value() ||
       edge.resolved_package_base.has_value()) {
        return false;
    }
    const ProvidedDependency& provider = *edge.resolved_provider;
    const auto* origin =
        std::get_if<RepositoryProviderOrigin>(&provider.origin);
    const ConsumerDependencyRequirement* requirement =
        consumer_requirement(edge);
    const auto* candidate =
        edge.resolved_candidate.has_value()
            ? std::get_if<ProviderResolvedDependencyCandidate>(
                  &edge.resolved_candidate.value())
            : nullptr;
    if(origin == nullptr || !origin->configured_order.has_value() ||
       requirement == nullptr || candidate == nullptr ||
       provider.package_name != requires_check.aur_name ||
       !has_valid_requires_check_package_identity_shape(
           provider.package_base) ||
       provider.provided_dependency_name !=
           requirement->package_name() ||
       !provider.constraint_metadata.has_value() ||
       provider.constraint_metadata->provided_capability.package_name() !=
           requirement->package_name() ||
       !provider_capability_version_is_coherent(
           provider,
           ObservedVersionSource::RepositoryProviderCapability) ||
       !observed_version_is_available_from(
           provider.constraint_metadata->package_version,
           ObservedVersionSource::RepositoryExactPackage) ||
       provider.package_version !=
           std::optional<std::string>{
               *provider.constraint_metadata->package_version.version()} ||
       candidate->provider != provider ||
       candidate->provided_version !=
           provider.constraint_metadata->provided_version ||
       !has_known_constraint_evaluation(edge)) {
        return false;
    }
    switch(edge.provider_resolution) {
        case ProviderResolutionKind::Unique:
        case ProviderResolutionKind::UserSelected:
            break;
        default:
            return false;
    }
    return repository_identity_matches_plan(
        plan,
        ConfiguredRepositoryIdentity{
            origin->repository_name, *origin->configured_order});
}

inline AurUpdateRequiredDevelTargetBlocker make_blocker(
    AurUpdateRequiredDevelTargetRelation relation,
    const RequiresCheckTargetIdentity& requires_check,
    std::optional<std::size_t> dependency_edge_index,
    std::optional<std::size_t> build_plan_order_index,
    std::vector<PackageRole> roles,
    std::vector<RootTargetIdentity> roots) {
    if(requires_check.is_bootstrap_trial) {
        // The added trial root role is not itself a required dependency role.
        // Preserve the original BuildPlan roles, and project only required
        // roles into this existing blocker contract. Never invent a role.
        std::erase(roles, PackageRole::Root);
        if(roles.empty()) relation = AurUpdateRequiredDevelTargetRelation::IdentityDrift;
    }
    AurUpdateRequiredDevelTargetBlocker blocker{
        relation, requires_check.update_plan_index,
        requires_check.aur_name, requires_check.reason};
    blocker.dependency_edge_index = dependency_edge_index;
    blocker.build_plan_order_index = build_plan_order_index;
    blocker.package_base = requires_check.package_base;
    blocker.roles = std::move(roles);
    blocker.affected_roots = std::move(roots);
    return blocker;
}

inline std::vector<CandidateMapping> collect_candidates(
    const AurUpdateExecutionPreflight& preflight,
    bool& authority_is_complete) {
    std::vector<CandidateMapping> candidates;
    std::set<std::size_t> update_plan_indices;
    for(std::size_t position = 0; position < preflight.targets.size();
        ++position) {
        const AurUpdateExecutionTarget& target =
            preflight.targets[position];
        if(target.update_plan_index != position) {
            authority_is_complete = false;
        }
        if(!update_plan_indices.insert(target.update_plan_index).second) {
            authority_is_complete = false;
        }
        if(!is_update_candidate(target.update)) {
            continue;
        }
        candidates.push_back(CandidateMapping{
            candidates.size(), target.update_plan_index,
            target.update.installed_name});
    }
    return candidates;
}

inline std::vector<RequiresCheckTargetIdentity>
collect_requires_check_identities(
    const AurUpdateExecutionPreflight& preflight,
    bool& authority_is_complete) {
    std::map<std::string, std::size_t> positions_by_name;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        ++positions_by_name[target.update.installed_name];
        if(target.update.aur_package.has_value() &&
           target.update.aur_package->aur_name !=
               target.update.installed_name) {
            ++positions_by_name[target.update.aur_package->aur_name];
        }
    }

    std::vector<RequiresCheckTargetIdentity> identities;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(!is_requires_check_candidate(target.update)) {
            continue;
        }
        const auto reason = *target.update.devel_assessment.requires_check_reason();
        if(positions_by_name[target.update.installed_name] != 1) {
            authority_is_complete = false;
            continue;
        }
        identities.push_back(RequiresCheckTargetIdentity{
            target.update_plan_index,
            target.update.installed_name,
            target.update.installed_version,
            target.update.aur_package->aur_name,
            target.update.aur_package->package_base,
            target.update.aur_package->version,
            reason, has_aur_update_bootstrap_intent(target.update)});
    }
    return identities;
}

inline bool has_exact_root_mapping(
    const BuildPlan& plan,
    const std::vector<CandidateMapping>& candidates) {
    if(plan.root_targets.size() != candidates.size()) return false;
    for(const CandidateMapping& candidate : candidates) {
        if(candidate.candidate_index >= plan.root_targets.size()) {
            return false;
        }
        const RootTargetIdentity& root =
            plan.root_targets[candidate.candidate_index];
        if(root.invocation_index != candidate.candidate_index ||
           root.requested_name != candidate.requested_name) {
            return false;
        }
    }
    return true;
}

inline const CandidateMapping* find_candidate_for_root(
    const std::vector<CandidateMapping>& candidates,
    const RootTargetIdentity& root) {
    const auto found = std::find_if(
        candidates.begin(), candidates.end(),
        [&root](const CandidateMapping& candidate) {
            return candidate.candidate_index == root.invocation_index &&
                   candidate.requested_name == root.requested_name;
        });
    return found == candidates.end() ? nullptr : &(*found);
}

inline void add_required_update_plan_index(
    std::vector<std::size_t>& indices,
    std::size_t update_plan_index) {
    if(std::find(indices.begin(), indices.end(), update_plan_index) ==
       indices.end()) {
        indices.push_back(update_plan_index);
    }
}

inline void expand_pending_relation(
    const PendingRequiredDevelRelation& pending,
    const std::vector<CandidateMapping>& candidates,
    bool has_exact_mapping,
    AurUpdateRequiredDevelRelationProjection& projection) {
    add_required_update_plan_index(
        projection.required_update_plan_indices,
        pending.blocker.requires_check_update_plan_index);

    std::vector<const CandidateMapping*> owners;
    bool requires_global = !has_exact_mapping ||
                           pending.blocker.affected_roots.empty();
    if(!requires_global) {
        for(const RootTargetIdentity& root :
            pending.blocker.affected_roots) {
            const CandidateMapping* owner =
                find_candidate_for_root(candidates, root);
            if(owner == nullptr) {
                requires_global = true;
                break;
            }
            if(std::find(owners.begin(), owners.end(), owner) ==
               owners.end()) {
                owners.push_back(owner);
            }
        }
    }
    if(requires_global) {
        projection.requires_global_fail_closed = true;
        owners.clear();
        for(const CandidateMapping& candidate : candidates) {
            owners.push_back(&candidate);
        }
    }
    if(owners.empty()) {
        projection.authority_is_complete = false;
        return;
    }
    for(const CandidateMapping* owner : owners) {
        projection.relations.emplace_back(
            owner->update_plan_index, pending.blocker,
            pending.dependency_specification);
    }
}

inline bool contains_index(
    const std::vector<std::size_t>& indices,
    std::size_t index) {
    return std::find(indices.begin(), indices.end(), index) !=
           indices.end();
}

inline bool relation_sets_are_equal(
    const std::vector<AurUpdateExpectedRequiredDevelRelation>& expected,
    const std::vector<AurUpdateExpectedRequiredDevelRelation>& actual) {
    if(expected.size() != actual.size()) return false;
    std::vector<bool> matched(actual.size(), false);
    for(const AurUpdateExpectedRequiredDevelRelation& relation : expected) {
        bool found = false;
        for(std::size_t index = 0; index < actual.size(); ++index) {
            if(matched[index] || actual[index] != relation) continue;
            matched[index] = true;
            found = true;
            break;
        }
        if(!found) return false;
    }
    return true;
}

inline bool owner_has_build_plan_inconsistency(
    const AurUpdateExecutionTarget& target) {
    return std::any_of(
        target.issues.begin(), target.issues.end(),
        [](const AurUpdateExecutionIssue& issue) {
            return issue.reason ==
                   AurUpdateExecutionReason::BuildPlanInconsistent;
        });
}

} // namespace aur_update_required_devel_relation_detail

inline AurUpdateRequiredDevelRelationProjection
project_aur_update_required_devel_relations(
    const AurUpdateExecutionPreflight& preflight) {
    using namespace aur_update_required_devel_relation_detail;
    AurUpdateRequiredDevelRelationProjection projection;
    std::vector<CandidateMapping> candidates = collect_candidates(
        preflight, projection.authority_is_complete);
    const std::vector<RequiresCheckTargetIdentity> requires_check_targets =
        collect_requires_check_identities(
            preflight, projection.authority_is_complete);

    if(!preflight.build_plan.has_value()) {
        if(!candidates.empty()) projection.authority_is_complete = false;
        return projection;
    }
    const BuildPlan& plan = *preflight.build_plan;
    const bool exact_root_mapping =
        has_exact_root_mapping(plan, candidates);
    if(!exact_root_mapping) {
        projection.requires_global_fail_closed = true;
    }

    std::vector<PendingRequiredDevelRelation> pending_relations;
    for(const RequiresCheckTargetIdentity& requires_check :
        requires_check_targets) {
        for(std::size_t edge_index = 0;
            edge_index < plan.dependency_edges.size(); ++edge_index) {
            const BuildPlanDependencyEdge& edge =
                plan.dependency_edges[edge_index];
            if(!edge_mentions_package_name(
                   edge, requires_check.aur_name)) {
                continue;
            }

            const std::vector<RootTargetIdentity> roots =
                exact_parent_roots(plan, edge);
            if(!roots.empty() &&
               is_verified_installed_exact_dependency_satisfaction(
                   edge, requires_check.aur_name,
                   requires_check.installed_version)) {
                continue;
            }

            AurUpdateRequiredDevelTargetRelation relation =
                AurUpdateRequiredDevelTargetRelation::IdentityDrift;
            std::vector<PackageRole> roles;
            const PlannedPackageTarget* planned_target = nullptr;
            if(exact_aur_dependency_matches(
                   plan, edge, requires_check, roots,
                   &planned_target)) {
                relation = AurUpdateRequiredDevelTargetRelation::
                    AurExactDependency;
                roles = planned_target->roles;
            } else if(aur_provider_matches(
                          plan, edge, requires_check, roots,
                          &planned_target)) {
                relation =
                    AurUpdateRequiredDevelTargetRelation::AurProvider;
                roles = planned_target->roles;
            } else if(repository_exact_matches(
                          plan, edge, requires_check)) {
                relation = AurUpdateRequiredDevelTargetRelation::
                    RepositoryExactDependency;
                roles = {edge.role};
            } else if(repository_provider_matches(
                          plan, edge, requires_check)) {
                relation = AurUpdateRequiredDevelTargetRelation::
                    RepositoryProvider;
                roles = {edge.role};
            } else if(is_dependency_role(edge.role)) {
                roles = {edge.role};
            }

            pending_relations.emplace_back(
                make_blocker(
                    relation, requires_check, edge_index,
                    std::nullopt, std::move(roles), roots),
                edge.dependency_spec);
        }

        const BuildPlanArtifactTargetProjectionResult artifact_projection =
            project_build_plan_required_artifact_targets(plan);
        for(std::size_t order_index = 0;
            order_index < plan.order.size(); ++order_index) {
            const BuildPlanEntry& entry = plan.order[order_index];
            if(std::find(
                   entry.package_names.begin(), entry.package_names.end(),
                   requires_check.aur_name) ==
               entry.package_names.end()) {
                continue;
            }

            const PlannedPackageTarget* planned_target =
                find_unique_package_target(
                    plan, requires_check.aur_name,
                    std::optional<std::string>{entry.package_base});
            std::vector<RootTargetIdentity> roots =
                planned_target == nullptr
                    ? std::vector<RootTargetIdentity>{}
                    : normalize_roots(planned_target->roots);
            if(planned_target != nullptr &&
               (roots.size() != planned_target->roots.size() ||
                std::any_of(
                    roots.begin(), roots.end(),
                    [&plan](const RootTargetIdentity& root) {
                        return !is_known_plan_root(plan, root);
                    }))) {
                roots.clear();
            }
            std::vector<PackageRole> roles =
                planned_target == nullptr
                    ? std::vector<PackageRole>{}
                    : planned_target->roles;

            bool projection_contains_exact_child = false;
            if(artifact_projection.is_success() &&
               order_index <
                   artifact_projection.success()->build_units.size()) {
                const ProjectedBuildPlanArtifactTargets& unit =
                    artifact_projection.success()->build_units[order_index];
                projection_contains_exact_child =
                    unit.build_plan_order_index == order_index &&
                    unit.package_base == entry.package_base &&
                    std::any_of(
                        unit.required_targets.begin(),
                        unit.required_targets.end(),
                        [&requires_check](
                            const RequiredPackageArtifactTarget& target) {
                            return target.package_name ==
                                       requires_check.aur_name &&
                                   target.package_base ==
                                       requires_check.package_base;
                        });
            }
            const bool exact_identity =
                entry.package_base == requires_check.package_base &&
                planned_target != nullptr && !roots.empty() &&
                projection_contains_exact_child;
            // A bootstrap's own singular root output is its explicit trial
            // intent. Every cross-root/provider/dependency contribution still
            // produces the original strict RequiresCheck blocker.
            const auto& original = preflight.targets[requires_check.update_plan_index];
            if(exact_identity && has_aur_update_bootstrap_intent(original.update) &&
               roots.size() == 1 && roles == std::vector<PackageRole>{PackageRole::Root}) {
                const auto* self = find_candidate_for_root(candidates, roots.front());
                if(self && self->update_plan_index == requires_check.update_plan_index) continue;
            }
            pending_relations.emplace_back(
                make_blocker(
                    exact_identity
                        ? AurUpdateRequiredDevelTargetRelation::
                              RequiredArtifactChild
                        : AurUpdateRequiredDevelTargetRelation::
                              IdentityDrift,
                    requires_check, std::nullopt, order_index,
                    std::move(roles), roots),
                std::nullopt);
        }
    }

    for(const PendingRequiredDevelRelation& pending : pending_relations) {
        expand_pending_relation(
            pending, candidates, exact_root_mapping, projection);
    }
    std::sort(
        projection.required_update_plan_indices.begin(),
        projection.required_update_plan_indices.end());
    return projection;
}

inline bool has_complete_aur_update_required_devel_relation_snapshot(
    const AurUpdateExecutionPreflight& preflight) {
    using namespace aur_update_required_devel_relation_detail;
    if(preflight.devel_requires_check_policy !=
       std::optional<DevelRequiresCheckPolicy>{
           DevelRequiresCheckPolicy::SkipIndependentTarget}) {
        return preflight.devel_requires_check_policy ==
               std::optional<DevelRequiresCheckPolicy>{
                   DevelRequiresCheckPolicy::BlockOperation};
    }
    if(!has_valid_aur_update_execution_policy_snapshot(preflight)) {
        return false;
    }

    const AurUpdateRequiredDevelRelationProjection expected =
        project_aur_update_required_devel_relations(preflight);
    if(!expected.authority_is_complete) return false;
    bool candidate_authority_is_complete = true;
    const std::vector<CandidateMapping> candidates = collect_candidates(
        preflight, candidate_authority_is_complete);
    if(!candidate_authority_is_complete) return false;

    std::vector<AurUpdateExpectedRequiredDevelRelation> actual_relations;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(is_requires_check_candidate(target.update) && !has_aur_update_bootstrap_intent(target.update)) {
            const bool expected_required = contains_index(
                expected.required_update_plan_indices,
                target.update_plan_index);
            const std::optional<AurUpdateExecutionSkipKind> expected_kind =
                expected_required
                    ? std::optional<AurUpdateExecutionSkipKind>{
                          AurUpdateExecutionSkipKind::
                              RequiredDevelRequiresCheck}
                    : std::optional<AurUpdateExecutionSkipKind>{AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck};
            if(target.status != AurUpdateExecutionTargetStatus::Skipped ||
               target.skip_kind != expected_kind) {
                return false;
            }
        }

        if(has_aur_update_bootstrap_intent(target.update) &&
           (target.skip_kind || target.status == AurUpdateExecutionTargetStatus::Skipped ||
            (contains_index(expected.required_update_plan_indices, target.update_plan_index) &&
             target.status != AurUpdateExecutionTargetStatus::Incomplete))) return false;

        for(const AurUpdateExecutionIssue& issue : target.issues) {
            if(issue.reason != AurUpdateExecutionReason::
                                   RequiredDevelTargetRequiresCheck) {
                continue;
            }
            if(!is_valid_aur_update_execution_issue_devel_payload(issue) ||
               !issue.required_devel_target_blocker.has_value() ||
               issue.package_name !=
                   std::optional<std::string>{
                       issue.required_devel_target_blocker->package_name} ||
               issue.package_base !=
                   issue.required_devel_target_blocker->package_base ||
               issue.devel_requires_check_reason !=
                   std::optional<DevelRequiresCheckReason>{
                       issue.required_devel_target_blocker
                           ->devel_requires_check_reason} ||
               issue.diagnostic.empty() ||
               issue.build_plan_projection_issue.has_value() ||
               issue.relation_reason.has_value()) {
                return false;
            }
            const AurUpdateRequiredDevelTargetBlocker& blocker =
                *issue.required_devel_target_blocker;
            if(blocker.dependency_edge_index.has_value() ==
               blocker.build_plan_order_index.has_value()) {
                return false;
            }
            const auto owner = std::find_if(
                candidates.begin(), candidates.end(),
                [&target](const CandidateMapping& candidate) {
                    return candidate.update_plan_index ==
                           target.update_plan_index;
                });
            if(owner == candidates.end() ||
               !preflight.build_plan.has_value() ||
               !target.build_plan_root_index.has_value() ||
               *target.build_plan_root_index != owner->candidate_index ||
               owner->candidate_index >=
                   preflight.build_plan->root_targets.size() ||
               preflight.build_plan
                       ->root_targets[owner->candidate_index] !=
                   RootTargetIdentity{
                       owner->candidate_index,
                       owner->requested_name}) {
                return false;
            }
            actual_relations.emplace_back(
                target.update_plan_index, blocker,
                issue.dependency_specification);
        }
    }

    if(!relation_sets_are_equal(expected.relations, actual_relations)) {
        return false;
    }
    if(expected.requires_global_fail_closed) {
        bool has_candidate = false;
        for(const AurUpdateExecutionTarget& target : preflight.targets) {
            if(!is_update_candidate(target.update)) {
                continue;
            }
            has_candidate = true;
            if(!owner_has_build_plan_inconsistency(target)) return false;
        }
        if(!has_candidate) return false;
    }
    return true;
}
