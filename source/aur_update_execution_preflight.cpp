#include "aur_update_execution_preflight.hpp"

#include "aur_update_required_devel_relation_projection.hpp"

#include "dependency_provider.hpp"
#include "dependency_spec.hpp"
#include "localization.hpp"
#include "package_identifier.hpp"
#include "package_relation_presentation.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view AUR_SERVICE_NAME = "AUR";
constexpr std::string_view BUILD_PLAN_TYPE_NAME = "BuildPlan";
constexpr std::string_view PACKAGE_BASE_FIELD_NAME = "PackageBase";
constexpr char DUPLICATE_UPDATE_TARGET_DIAGNOSTIC[] =
    "Update candidate occurs more than once in the invocation.";

struct CandidateMapping {
    std::size_t candidate_index;
    std::size_t update_plan_index;
    std::string requested_name;
};

struct AttributedBuildPlanIssue {
    AurUpdateExecutionIssue issue;
    std::vector<RootTargetIdentity> roots;
};

bool same_issue(
    const AurUpdateExecutionIssue& lhs,
    const AurUpdateExecutionIssue& rhs) {
    return lhs.reason == rhs.reason && lhs.package_name == rhs.package_name &&
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

void add_issue(
    AurUpdateExecutionTarget& target,
    AurUpdateExecutionIssue issue) {
    auto same = [&issue](const AurUpdateExecutionIssue& existing) {
        return same_issue(existing, issue);
    };
    if(std::find_if(target.issues.begin(), target.issues.end(), same) !=
       target.issues.end()) {
        return;
    }
    target.issues.push_back(std::move(issue));
}

AurUpdateExecutionIssue make_localized_execution_issue(
    AurUpdateExecutionReason reason, std::string diagnostic,
    std::optional<std::string> package_name = std::nullopt,
    std::optional<std::string> package_base = std::nullopt,
    std::optional<std::string> dependency_specification = std::nullopt,
    std::optional<DevelRequiresCheckReason>
        devel_requires_check_reason = std::nullopt) {
    return AurUpdateExecutionIssue{
        reason, std::move(package_name), std::move(package_base),
        std::move(dependency_specification), std::move(diagnostic),
        std::nullopt, std::nullopt, devel_requires_check_reason};
}

template <std::size_t Size>
AurUpdateExecutionIssue make_localized_execution_issue(
    AurUpdateExecutionReason reason, const char (&diagnostic)[Size],
    std::optional<std::string> package_name = std::nullopt,
    std::optional<std::string> package_base = std::nullopt,
    std::optional<std::string> dependency_specification = std::nullopt,
    std::optional<DevelRequiresCheckReason>
        devel_requires_check_reason = std::nullopt) {
    return make_localized_execution_issue(
        reason, localization::translate_message(diagnostic),
        std::move(package_name), std::move(package_base),
        std::move(dependency_specification),
        devel_requires_check_reason);
}

bool is_unsupported_reason(AurUpdateExecutionReason reason) noexcept {
    switch(reason) {
        case AurUpdateExecutionReason::SplitPackageSelectionRequired:
        case AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase:
        case AurUpdateExecutionReason::AmbiguousProvider:
        case AurUpdateExecutionReason::ConflictsOrReplacesUnresolved:
            return true;
        default:
            return false;
    }
}

bool is_skip_reason(
    AurUpdateExecutionReason reason,
    DevelRequiresCheckPolicy policy,
    bool allow_devel_requires_check_skip) noexcept {
    return reason == AurUpdateExecutionReason::UpToDate ||
           reason == AurUpdateExecutionReason::NonAurForeign ||
           (reason == AurUpdateExecutionReason::DevelRequiresCheck &&
            policy ==
                DevelRequiresCheckPolicy::SkipIndependentTarget &&
            allow_devel_requires_check_skip);
}

std::optional<AurUpdateExecutionSkipKind> skip_kind_for_reason(
    AurUpdateExecutionReason reason,
    DevelRequiresCheckPolicy policy,
    bool allow_devel_requires_check_skip,
    bool is_required_devel_target) noexcept {
    switch(reason) {
        case AurUpdateExecutionReason::UpToDate:
            return AurUpdateExecutionSkipKind::UpToDate;
        case AurUpdateExecutionReason::NonAurForeign:
            return AurUpdateExecutionSkipKind::NonAurForeign;
        case AurUpdateExecutionReason::DevelRequiresCheck:
            if(policy ==
                   DevelRequiresCheckPolicy::SkipIndependentTarget &&
               allow_devel_requires_check_skip) {
                return is_required_devel_target
                           ? AurUpdateExecutionSkipKind::
                                 RequiredDevelRequiresCheck
                           : AurUpdateExecutionSkipKind::
                                 IndependentDevelRequiresCheck;
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

std::string devel_requires_check_diagnostic(
    DevelRequiresCheckReason reason) {
    switch(reason) {
        case DevelRequiresCheckReason::SuffixCandidateOnly:
            return localization::translate_message(
                "Devel package update status requires check: the suffix is candidate evidence only, and authoritative build provenance is unavailable.");
        case DevelRequiresCheckReason::NoAuthoritativeBuildProvenance:
        case DevelRequiresCheckReason::InstalledArtifactDrift:
        case DevelRequiresCheckReason::AurRecipeAdvanced:
        case DevelRequiresCheckReason::SourceMetadataMissing:
        case DevelRequiresCheckReason::SourceMetadataMalformed:
        case DevelRequiresCheckReason::SourceIdentityChanged:
        case DevelRequiresCheckReason::TransportRequiresCheck:
        case DevelRequiresCheckReason::SelectorRequiresCheck:
        case DevelRequiresCheckReason::MultipleFloatingSources:
        case DevelRequiresCheckReason::ArchitectureSpecificSourceUnresolved:
        case DevelRequiresCheckReason::ProvenanceMissing:
        case DevelRequiresCheckReason::ProvenanceInvalid:
        case DevelRequiresCheckReason::ProvenanceCorrupted:
        case DevelRequiresCheckReason::ProvenanceFutureSchema:
        case DevelRequiresCheckReason::BuildSourceProofUnavailable:
            return localization::translate_message(
                "Devel package update status requires check.");
    }
    return localization::translate_message(
        "Devel package update status requires check.");
}

bool contains_control_character(const std::string& value) noexcept {
    return std::any_of(
        value.begin(), value.end(), [](unsigned char character) {
            return std::iscntrl(character) != 0;
        });
}

void reduce_target_status(
    AurUpdateExecutionTarget& target,
    DevelRequiresCheckPolicy policy,
    bool allow_devel_requires_check_skip,
    bool is_required_devel_target) noexcept {
    bool has_incomplete = false;
    bool has_unsupported = false;
    bool has_skip = false;
    std::optional<AurUpdateExecutionSkipKind> skip_kind;
    target.skip_kind.reset();
    for(const auto& issue : target.issues) {
        if(issue.reason == AurUpdateExecutionReason::None) continue;
        if(is_unsupported_reason(issue.reason)) {
            has_unsupported = true;
        } else if(is_skip_reason(
                      issue.reason, policy,
                      allow_devel_requires_check_skip)) {
            has_skip = true;
            const auto current_skip_kind =
                skip_kind_for_reason(
                    issue.reason, policy,
                    allow_devel_requires_check_skip,
                    is_required_devel_target);
            if(skip_kind.has_value() &&
               skip_kind != current_skip_kind) {
                skip_kind.reset();
            } else if(!skip_kind.has_value()) {
                skip_kind = current_skip_kind;
            }
        } else {
            has_incomplete = true;
        }
    }

    // POLICY(#267): blocking stateはIncompleteを最優先し、次にUnsupportedとする。
    if(has_incomplete) {
        target.status = AurUpdateExecutionTargetStatus::Incomplete;
    } else if(has_unsupported) {
        target.status = AurUpdateExecutionTargetStatus::Unsupported;
    } else if(has_skip) {
        target.status = AurUpdateExecutionTargetStatus::Skipped;
        target.skip_kind = skip_kind;
    } else {
        target.status = AurUpdateExecutionTargetStatus::Executable;
    }
}

bool has_consistent_normal_remote_metadata(
    const AurUpdatePlanEntry& entry) {
    switch(entry.classification) {
        case AurUpdateClassification::UpdateAvailable:
            return entry.aur_package.has_value() &&
                   entry.aur_package->version_relation ==
                       AurVersionRelation::NewerThanInstalled;
        case AurUpdateClassification::UpToDate:
            return entry.aur_package.has_value() &&
                   (entry.aur_package->version_relation ==
                        AurVersionRelation::OlderThanInstalled ||
                    entry.aur_package->version_relation ==
                        AurVersionRelation::SameAsInstalled);
        case AurUpdateClassification::NonAurForeign:
        case AurUpdateClassification::MetadataUnavailable:
            return !entry.aur_package.has_value();
        case AurUpdateClassification::VersionComparisonUnavailable:
            return entry.aur_package.has_value() &&
                   entry.aur_package->version_relation ==
                       AurVersionRelation::Unavailable;
    }
    return false;
}

void add_initial_classification_issue(AurUpdateExecutionTarget& target) {
    switch(project_aur_update_effective_state(target.update)) {
        case AurUpdateEffectiveState::UpdateAvailable:
            break;
        case AurUpdateEffectiveState::UpToDate:
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::UpToDate,
                    "Installed package is already up to date.",
                    target.update.installed_name));
            break;
        case AurUpdateEffectiveState::RequiresCheck: {
            const DevelRequiresCheckReason* reason =
                target.update.devel_assessment.requires_check_reason();
            if(reason == nullptr) {
                add_issue(
                    target,
                    make_localized_execution_issue(
                        AurUpdateExecutionReason::UpdatePlanInconsistent,
                        "Devel update assessment has no check-required reason.",
                        target.update.installed_name));
                break;
            }
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::DevelRequiresCheck,
                    devel_requires_check_diagnostic(*reason),
                    target.update.installed_name,
                    target.update.aur_package.has_value()
                        ? std::optional<std::string>{
                              target.update.aur_package
                                  ->package_base}
                        : std::nullopt,
                    std::nullopt, *reason));
            break;
        }
        case AurUpdateEffectiveState::NonAurForeign:
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::NonAurForeign,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal service name "AUR".
                        "Installed foreign package is not present in {}.",
                        AUR_SERVICE_NAME),
                    target.update.installed_name));
            break;
        case AurUpdateEffectiveState::MetadataUnavailable:
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::AurMetadataUnavailable,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal service name "AUR".
                        "{} update metadata is unavailable.",
                        AUR_SERVICE_NAME),
                    target.update.installed_name));
            break;
        case AurUpdateEffectiveState::VersionComparisonUnavailable:
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::VersionComparisonUnavailable,
                    "Installed and remote versions could not be compared.",
                    target.update.installed_name));
            break;
        case AurUpdateEffectiveState::Unknown:
            add_issue(target, make_localized_execution_issue(AurUpdateExecutionReason::DevelObservationUnknown,
                                                             localization::format_translated_message(
                                                                 // TRANSLATORS: The placeholder is the literal tool name "Git".
                                                                 "Devel {} observation failed; no automatic build.", "Git"),
                                                             target.update.installed_name));
            break;
        case AurUpdateEffectiveState::Unsupported:
            add_issue(target, make_localized_execution_issue(AurUpdateExecutionReason::DevelUnsupported,
                                                             "Devel source is unsupported for automatic updates.", target.update.installed_name));
            break;
        case AurUpdateEffectiveState::Inconsistent:
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::UpdatePlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal service name "AUR".
                        "{} update classification is unknown.",
                        AUR_SERVICE_NAME),
                    target.update.installed_name));
            return;
    }

    if(!has_consistent_normal_remote_metadata(target.update)) {
        add_issue(
            target,
            make_localized_execution_issue(
                AurUpdateExecutionReason::UpdatePlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "{} update classification and remote metadata disagree.",
                    AUR_SERVICE_NAME),
                target.update.installed_name));
    }

    if(target.update.aur_package.has_value() &&
       (!is_valid_package_name(target.update.aur_package->aur_name) ||
        !is_valid_package_name(target.update.aur_package->package_base))) {
        add_issue(
            target,
            make_localized_execution_issue(
                AurUpdateExecutionReason::UpdatePlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "{} update metadata contains an invalid package identity.",
                    AUR_SERVICE_NAME),
                target.update.aur_package->aur_name,
                target.update.aur_package->package_base));
    }
    if(target.update.aur_package.has_value() &&
       target.update.aur_package->aur_name != target.update.installed_name) {
        add_issue(
            target,
            make_localized_execution_issue(
                AurUpdateExecutionReason::UpdatePlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "Installed and {} package names disagree.",
                    AUR_SERVICE_NAME),
                target.update.aur_package->aur_name,
                target.update.aur_package->package_base));
    }
}

std::optional<DesiredInstallReason> desired_install_reason_for_aur_update_root(
    InstalledPackageReason installed_reason) noexcept {
    switch(installed_reason) {
        case InstalledPackageReason::Explicit:
            return DesiredInstallReason::Explicit;
        case InstalledPackageReason::Dependency:
            return DesiredInstallReason::Dependency;
        case InstalledPackageReason::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}

bool has_package_role(
    const PlannedPackageTarget& target, PackageRole expected_role) {
    return std::find(
               target.roles.begin(), target.roles.end(), expected_role) !=
           target.roles.end();
}

bool is_dependency_role(PackageRole role) noexcept {
    return role == PackageRole::RuntimeDependency ||
           role == PackageRole::BuildDependency ||
           role == PackageRole::CheckDependency;
}

bool is_known_package_role(PackageRole role) noexcept {
    return role == PackageRole::Root || is_dependency_role(role);
}

bool contains_all_roots(
    const std::vector<RootTargetIdentity>& available_roots,
    const std::vector<RootTargetIdentity>& required_roots) {
    return std::all_of(
        required_roots.begin(), required_roots.end(),
        [&available_roots](const RootTargetIdentity& root) {
            return std::find(
                       available_roots.begin(),
                       available_roots.end(), root) !=
                   available_roots.end();
        });
}

bool root_identity_less(
    const RootTargetIdentity& lhs,
    const RootTargetIdentity& rhs) {
    if(lhs.invocation_index != rhs.invocation_index) {
        return lhs.invocation_index < rhs.invocation_index;
    }
    return lhs.requested_name < rhs.requested_name;
}

void add_root_identity(
    std::vector<RootTargetIdentity>& roots,
    const RootTargetIdentity& root) {
    if(std::find(roots.begin(), roots.end(), root) != roots.end()) return;
    roots.push_back(root);
    std::sort(roots.begin(), roots.end(), root_identity_less);
}

bool is_known_plan_root(
    const BuildPlan& plan, const RootTargetIdentity& root) {
    return std::find(
               plan.root_targets.begin(), plan.root_targets.end(), root) !=
           plan.root_targets.end();
}

std::vector<RootTargetIdentity> roots_for_package(
    const BuildPlan& plan, const std::optional<std::string>& package_name,
    const std::optional<std::string>& package_base) {
    std::vector<RootTargetIdentity> roots;
    for(const auto& target : plan.package_targets) {
        if(package_name.has_value() &&
           target.package_name != package_name.value()) {
            continue;
        }
        if(package_base.has_value() &&
           target.package_base != package_base.value()) {
            continue;
        }
        for(const auto& root : target.roots)
            add_root_identity(roots, root);
    }
    return roots;
}

const PlannedPackageTarget* find_unique_package_target(
    const BuildPlan& plan, const std::string& package_name,
    const std::optional<std::string>& package_base) {
    const PlannedPackageTarget* match = nullptr;
    for(const auto& target : plan.package_targets) {
        if(target.package_name != package_name) continue;
        if(package_base.has_value() &&
           target.package_base != package_base.value()) {
            continue;
        }
        if(match != nullptr) return nullptr;
        match = &target;
    }
    return match;
}

struct AurDependencyGraphEdge {
    const PlannedPackageTarget* parent;
    const PlannedPackageTarget* target;
    PackageRole role;
};

std::optional<std::string> edge_requirement_name(
    const BuildPlanDependencyEdge& edge) {
    if(!edge.requirement.has_value()) {
        // Compatibility for graph-only BuildPlan producers and focused test
        // fixtures. Production resolver edges always take the typed branch.
        const ParsedDependency legacy =
            parse_dependency_string(edge.dependency_spec);
        if(legacy.has_malformed_constraint()) return std::nullopt;
        return legacy.name;
    }
    if(const auto* consumer =
           std::get_if<ConsumerDependencyRequirement>(
               &edge.requirement.value());
       consumer != nullptr) {
        return consumer->package_name();
    }
    return std::nullopt;
}

std::vector<AurDependencyGraphEdge> collect_aur_dependency_graph(
    const BuildPlan& plan) {
    std::vector<AurDependencyGraphEdge> graph;
    for(const auto& edge : plan.dependency_edges) {
        if(!is_dependency_role(edge.role)) continue;

        const PlannedPackageTarget* parent = find_unique_package_target(
            plan, edge.parent_package_name,
            std::optional<std::string>{edge.parent_package_base});
        if(parent == nullptr) continue;

        const PlannedPackageTarget* target = nullptr;
        if(edge.kind == DependencyKind::Aur &&
           edge.resolved_package_name.has_value() &&
           edge.resolved_package_base.has_value() &&
           !edge.resolved_provider.has_value() &&
           edge_requirement_name(edge).has_value() &&
           edge.resolved_package_name.value() ==
               edge_requirement_name(edge).value()) {
            target = find_unique_package_target(
                plan, edge.resolved_package_name.value(),
                edge.resolved_package_base);
        } else if(
            edge.kind == DependencyKind::Provided &&
            edge.resolved_provider.has_value() &&
            std::holds_alternative<AurProviderOrigin>(
                edge.resolved_provider->origin) &&
            !edge.resolved_package_name.has_value() &&
            !edge.resolved_package_base.has_value()) {
            target = find_unique_package_target(
                plan, edge.resolved_provider->package_name, std::nullopt);
        }
        if(target == nullptr || !has_package_role(*target, edge.role) ||
           parent->roots.empty() ||
           !contains_all_roots(target->roots, parent->roots)) {
            continue;
        }
        graph.push_back(AurDependencyGraphEdge{parent, target, edge.role});
    }
    return graph;
}

struct RootGraphReachability {
    RootTargetIdentity root;
    std::vector<const PlannedPackageTarget*> targets;
};

bool contains_target(
    const std::vector<const PlannedPackageTarget*>& targets,
    const PlannedPackageTarget* target) {
    return std::find(targets.begin(), targets.end(), target) != targets.end();
}

std::vector<RootGraphReachability> collect_root_graph_reachability(
    const BuildPlan& plan,
    const std::vector<AurDependencyGraphEdge>& graph) {
    std::vector<RootGraphReachability> reachability;
    for(const auto& root : plan.root_targets) {
        RootGraphReachability reachable{root, {}};
        const PlannedPackageTarget* root_target = find_unique_package_target(
            plan, root.requested_name, std::nullopt);
        if(root_target == nullptr ||
           !has_package_role(*root_target, PackageRole::Root) ||
           std::find(
               root_target->roots.begin(), root_target->roots.end(), root) ==
               root_target->roots.end()) {
            reachability.push_back(std::move(reachable));
            continue;
        }

        std::vector<const PlannedPackageTarget*> pending{root_target};
        while(!pending.empty()) {
            const PlannedPackageTarget* current = pending.back();
            pending.pop_back();
            if(contains_target(reachable.targets, current)) continue;
            reachable.targets.push_back(current);

            for(const auto& edge : graph) {
                if(edge.parent != current ||
                   contains_target(reachable.targets, edge.target)) {
                    continue;
                }
                pending.push_back(edge.target);
            }
        }
        reachability.push_back(std::move(reachable));
    }
    return reachability;
}

const RootGraphReachability* find_root_reachability(
    const std::vector<RootGraphReachability>& reachability,
    const RootTargetIdentity& root) {
    auto match = [&root](const RootGraphReachability& current) {
        return current.root == root;
    };
    auto found = std::find_if(reachability.begin(), reachability.end(), match);
    return found == reachability.end() ? nullptr : &(*found);
}

bool dependency_target_is_grounded_by_rooted_graph(
    const PlannedPackageTarget& target,
    const std::vector<AurDependencyGraphEdge>& graph,
    const std::vector<RootGraphReachability>& reachability) {
    for(const auto& root : target.roots) {
        const RootGraphReachability* reachable =
            find_root_reachability(reachability, root);
        if(reachable == nullptr ||
           !contains_target(reachable->targets, &target)) {
            return false;
        }
    }

    for(const auto role : target.roles) {
        if(!is_dependency_role(role)) continue;
        bool role_is_grounded = std::any_of(
            graph.begin(), graph.end(),
            [&target, role, &reachability](
                const AurDependencyGraphEdge& edge) {
                if(edge.target != &target || edge.role != role) return false;
                return std::any_of(
                    reachability.begin(), reachability.end(),
                    [&edge](const RootGraphReachability& reachable) {
                        return contains_target(
                            reachable.targets, edge.parent);
                    });
            });
        if(!role_is_grounded) return false;
    }
    return true;
}

struct ReachableDependencyCycle {
    std::vector<std::string> package_bases;
    std::vector<RootTargetIdentity> roots;
};

using PackageBaseGraph = std::map<std::string, std::set<std::string>>;

bool package_target_reaches(
    const std::vector<AurDependencyGraphEdge>& graph,
    const PlannedPackageTarget* start,
    const PlannedPackageTarget* destination) {
    std::vector<const PlannedPackageTarget*> pending{start};
    std::set<const PlannedPackageTarget*> visited;
    while(!pending.empty()) {
        const PlannedPackageTarget* current = pending.back();
        pending.pop_back();
        if(current == destination) return true;
        if(!visited.insert(current).second) continue;

        for(const auto& edge : graph) {
            if(edge.parent == current && !visited.contains(edge.target)) {
                pending.push_back(edge.target);
            }
        }
    }
    return false;
}

bool package_base_reaches(
    const PackageBaseGraph& graph, const std::string& start,
    const std::string& destination) {
    std::vector<std::string> pending{start};
    std::set<std::string> visited;
    while(!pending.empty()) {
        std::string current = std::move(pending.back());
        pending.pop_back();
        if(current == destination) return true;
        if(!visited.insert(current).second) continue;

        auto neighbors = graph.find(current);
        if(neighbors == graph.end()) continue;
        for(const auto& neighbor : neighbors->second) {
            if(!visited.contains(neighbor)) pending.push_back(neighbor);
        }
    }
    return false;
}

bool cycle_contains_package_base(
    const ReachableDependencyCycle& cycle,
    const std::string& package_base) {
    return std::find(
               cycle.package_bases.begin(), cycle.package_bases.end(),
               package_base) != cycle.package_bases.end();
}

std::vector<ReachableDependencyCycle> collect_reachable_dependency_cycles(
    const std::vector<AurDependencyGraphEdge>& graph,
    const std::vector<RootGraphReachability>& reachability) {
    PackageBaseGraph package_base_graph;
    for(const auto& edge : graph) {
        // POLICY(#268): same-Base package dependency is real package-level
        // attribution。reverse pathがある場合だけexecution graphのreal
        // self-cycleへ縮約し、一方向のsibling dependencyはcycleにしない。
        if(edge.parent->package_base == edge.target->package_base &&
           edge.parent->package_name != edge.target->package_name) {
            if(package_target_reaches(graph, edge.target, edge.parent)) {
                package_base_graph[edge.parent->package_base].insert(
                    edge.parent->package_base);
            } else {
                package_base_graph.try_emplace(edge.parent->package_base);
            }
            continue;
        }
        package_base_graph[edge.parent->package_base].insert(
            edge.target->package_base);
        package_base_graph.try_emplace(edge.target->package_base);
    }

    std::vector<ReachableDependencyCycle> cycles;
    std::set<std::string> assigned_package_bases;
    for(const auto& graph_entry : package_base_graph) {
        const std::string& package_base = graph_entry.first;
        if(assigned_package_bases.contains(package_base)) continue;

        ReachableDependencyCycle cycle;
        for(const auto& candidate_entry : package_base_graph) {
            const std::string& candidate = candidate_entry.first;
            if(assigned_package_bases.contains(candidate)) continue;
            if(package_base_reaches(
                   package_base_graph, package_base, candidate) &&
               package_base_reaches(
                   package_base_graph, candidate, package_base)) {
                cycle.package_bases.push_back(candidate);
            }
        }
        for(const auto& member : cycle.package_bases) {
            assigned_package_bases.insert(member);
        }

        const bool has_self_edge = cycle.package_bases.size() == 1 &&
                                   package_base_graph.at(package_base).contains(package_base);
        if(cycle.package_bases.size() <= 1 && !has_self_edge) continue;

        for(const auto& reachable : reachability) {
            const bool reaches_cycle = std::any_of(
                reachable.targets.begin(), reachable.targets.end(),
                [&cycle](const PlannedPackageTarget* target) {
                    return cycle_contains_package_base(
                        cycle, target->package_base);
                });
            if(reaches_cycle) add_root_identity(cycle.roots, reachable.root);
        }
        if(!cycle.roots.empty()) cycles.push_back(std::move(cycle));
    }
    return cycles;
}

const ReachableDependencyCycle* find_reachable_cycle(
    const std::vector<ReachableDependencyCycle>& cycles,
    const std::string& package_base) {
    auto found = std::find_if(
        cycles.begin(), cycles.end(),
        [&package_base](const ReachableDependencyCycle& cycle) {
            return cycle_contains_package_base(cycle, package_base);
        });
    return found == cycles.end() ? nullptr : &(*found);
}

void add_attributed_issue(
    std::vector<AttributedBuildPlanIssue>& issues,
    AurUpdateExecutionIssue issue,
    std::vector<RootTargetIdentity> roots) {
    auto same = [&issue, &roots](const AttributedBuildPlanIssue& existing) {
        return same_issue(existing.issue, issue) && existing.roots == roots;
    };
    if(std::find_if(issues.begin(), issues.end(), same) != issues.end()) return;
    issues.push_back(
        AttributedBuildPlanIssue{std::move(issue), std::move(roots)});
}

std::vector<RootTargetIdentity> normalize_root_identities(
    const std::vector<RootTargetIdentity>& roots) {
    std::vector<RootTargetIdentity> normalized;
    for(const auto& root : roots)
        add_root_identity(normalized, root);
    return normalized;
}

bool resolution_failure_roots_are_consistent(
    const BuildPlan& plan, const BuildPlanResolutionFailure& failure,
    const std::vector<RootTargetIdentity>& normalized_roots) {
    if(normalized_roots.empty() ||
       normalized_roots.size() != failure.roots.size()) {
        return false;
    }
    if(std::any_of(
           normalized_roots.begin(), normalized_roots.end(),
           [&plan](const RootTargetIdentity& root) {
               return !is_known_plan_root(plan, root);
           })) {
        return false;
    }

    if(failure.parent_package_name.has_value() !=
       failure.parent_package_base.has_value()) {
        return false;
    }

    if(failure.parent_package_name.has_value()) {
        const std::vector<RootTargetIdentity> expected_roots =
            roots_for_package(
                plan, failure.parent_package_name,
                failure.parent_package_base);
        return !expected_roots.empty() && expected_roots == normalized_roots;
    }

    return std::all_of(
        normalized_roots.begin(), normalized_roots.end(),
        [&failure](const RootTargetIdentity& root) {
            return root.requested_name == failure.subject;
        });
}

void inspect_resolution_failures(
    const BuildPlan& plan,
    std::vector<AttributedBuildPlanIssue>& issues,
    std::set<std::string>& represented_unresolved_values) {
    for(const auto& failure : plan.resolution_failures) {
        AurUpdateExecutionReason reason;
        switch(failure.kind) {
            case BuildPlanResolutionFailureKind::
                InstalledPackageMetadataUnavailable:
                reason = AurUpdateExecutionReason::
                    InstalledPackageMetadataUnavailable;
                break;
            case BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable:
                reason = AurUpdateExecutionReason::RepositoryMetadataUnavailable;
                break;
            case BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable:
                reason = AurUpdateExecutionReason::AurDependencyMetadataUnavailable;
                break;
            case BuildPlanResolutionFailureKind::ProviderSearchUnavailable:
            case BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable:
                reason = AurUpdateExecutionReason::ProviderMetadataUnavailable;
                break;
            default:
                reason = AurUpdateExecutionReason::BuildPlanInconsistent;
                break;
        }

        std::vector<RootTargetIdentity> roots =
            normalize_root_identities(failure.roots);
        if(!resolution_failure_roots_are_consistent(
               plan, failure, roots)) {
            // POLICY(#267): 不正なownershipを別rootへ流さず、invocation全体を
            // fail-closedにするためattributionなしとして扱う。
            roots.clear();
        }
        add_attributed_issue(
            issues,
            make_localized_execution_issue(
                reason, failure.diagnostic, failure.subject,
                std::nullopt, failure.dependency_specification),
            std::move(roots));
        if(failure.kind ==
           BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable) {
            represented_unresolved_values.insert(failure.subject);
        }
    }
}

void inspect_dependency_edges(
    const BuildPlan& plan,
    std::vector<AttributedBuildPlanIssue>& issues,
    std::set<std::string>& represented_unresolved_values) {
    for(const auto& edge : plan.dependency_edges) {
        const std::optional<std::string> requirement_name =
            edge_requirement_name(edge);
        const std::optional<ParsedDependency> legacy_requirement =
            edge.requirement.has_value()
                ? std::nullopt
                : std::optional<ParsedDependency>{
                      parse_dependency_string(edge.dependency_spec)};
        std::vector<RootTargetIdentity> roots = roots_for_package(
            plan, edge.parent_package_name, edge.parent_package_base);

        auto has_consistent_resolved_target =
            [&plan, &edge, &roots](
                const std::string& package_name,
                const std::optional<std::string>& package_base) {
                std::vector<const PlannedPackageTarget*> matches;
                for(const auto& target : plan.package_targets) {
                    if(target.package_name != package_name) continue;
                    if(package_base.has_value() &&
                       target.package_base != package_base.value()) {
                        continue;
                    }
                    matches.push_back(&target);
                }
                return matches.size() == 1 &&
                       has_package_role(*matches.front(), edge.role) &&
                       contains_all_roots(matches.front()->roots, roots);
            };

        bool edge_is_consistent =
            !roots.empty() && is_dependency_role(edge.role);
        switch(edge.kind) {
            case DependencyKind::Installed:
            case DependencyKind::Repo:
                edge_is_consistent = edge_is_consistent &&
                                     edge.resolved_package_name.has_value() &&
                                     requirement_name.has_value() &&
                                     is_valid_package_name(
                                         edge.resolved_package_name.value()) &&
                                     edge.resolved_package_name.value() ==
                                         requirement_name.value() &&
                                     !edge.resolved_package_base.has_value() &&
                                     !edge.resolved_provider.has_value();
                break;
            case DependencyKind::Aur:
                edge_is_consistent = edge_is_consistent &&
                                     edge.resolved_package_name.has_value() &&
                                     edge.resolved_package_base.has_value() &&
                                     requirement_name.has_value() &&
                                     is_valid_package_name(
                                         edge.resolved_package_name.value()) &&
                                     is_valid_package_name(
                                         edge.resolved_package_base.value()) &&
                                     edge.resolved_package_name.value() ==
                                         requirement_name.value() &&
                                     !edge.resolved_provider.has_value();
                if(edge.resolved_package_name.has_value() &&
                   edge.resolved_package_base.has_value() &&
                   !has_consistent_resolved_target(
                       edge.resolved_package_name.value(),
                       edge.resolved_package_base)) {
                    edge_is_consistent = false;
                }
                break;
            case DependencyKind::Provided:
                edge_is_consistent = edge_is_consistent &&
                                     edge.resolved_provider.has_value() &&
                                     !edge.resolved_package_name.has_value() &&
                                     !edge.resolved_package_base.has_value();
                if(edge.resolved_provider.has_value()) {
                    edge_is_consistent = edge_is_consistent &&
                                         is_valid_package_name(
                                             edge.resolved_provider->package_name);
                    if(const auto* repository =
                           std::get_if<RepositoryProviderOrigin>(
                               &edge.resolved_provider->origin);
                       repository != nullptr) {
                        edge_is_consistent = edge_is_consistent &&
                                             !repository->repository_name.empty() &&
                                             !contains_control_character(
                                                 repository->repository_name);
                    } else if(!has_consistent_resolved_target(
                                  edge.resolved_provider->package_name,
                                  std::nullopt)) {
                        edge_is_consistent = false;
                    }
                }
                break;
            case DependencyKind::AmbiguousProvider:
                edge_is_consistent = edge_is_consistent &&
                                     !edge.resolved_package_name.has_value() &&
                                     !edge.resolved_package_base.has_value() &&
                                     !edge.resolved_provider.has_value();
                edge_is_consistent = edge_is_consistent && std::any_of(
                                                               plan.ambiguous_providers.begin(),
                                                               plan.ambiguous_providers.end(),
                                                               [&edge, &legacy_requirement](
                                                                   const AmbiguousProvidedDependency& ambiguous) {
                                                                   const std::string canonical_edge =
                                                                       legacy_requirement.has_value()
                                                                           ? legacy_requirement->raw
                                                                           : edge.dependency_spec;
                                                                   const std::string canonical_ambiguous =
                                                                       legacy_requirement.has_value()
                                                                           ? parse_dependency_string(
                                                                                 ambiguous.dependency)
                                                                                 .raw
                                                                           : ambiguous.dependency;
                                                                   return canonical_ambiguous == canonical_edge;
                                                               });
                add_attributed_issue(
                    issues,
                    make_localized_execution_issue(
                        AurUpdateExecutionReason::AmbiguousProvider,
                        localization::format_translated_message(
                            // TRANSLATORS: The placeholder is a literal dependency specification.
                            "Dependency has multiple provider candidates: {}",
                            edge.dependency_spec),
                        edge.parent_package_name,
                        edge.parent_package_base,
                        edge.dependency_spec),
                    roots);
                break;
            case DependencyKind::Unknown:
                edge_is_consistent = edge_is_consistent &&
                                     !edge.resolved_package_name.has_value() &&
                                     !edge.resolved_package_base.has_value() &&
                                     !edge.resolved_provider.has_value();
                break;
            default:
                edge_is_consistent = false;
                break;
        }
        if(!edge_is_consistent) {
            add_attributed_issue(
                issues,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    "Dependency edge fields or root attribution are inconsistent.",
                    edge.parent_package_name,
                    edge.parent_package_base,
                    edge.dependency_spec),
                roots);
        }
        if(edge.constraint_evaluation.has_value()) {
            const ConstraintSatisfaction satisfaction =
                edge.constraint_evaluation->satisfaction();
            if(satisfaction == ConstraintSatisfaction::Unsatisfied ||
               satisfaction == ConstraintSatisfaction::Unknown) {
                add_attributed_issue(
                    issues,
                    make_localized_execution_issue(
                        AurUpdateExecutionReason::
                            VersionConstraintUnverified,
                        localization::format_translated_message(
                            "Dependency {} is {}: {}",
                            edge.dependency_spec,
                            constraint_satisfaction_display(
                                satisfaction),
                            constraint_evaluation_reason_display(
                                edge.constraint_evaluation
                                    .value())),
                        edge.parent_package_name,
                        edge.parent_package_base,
                        edge.dependency_spec),
                    roots);
            }
        }

        const bool malformed_legacy_requirement =
            legacy_requirement.has_value() &&
            (legacy_requirement->has_malformed_constraint() ||
             !is_valid_package_name(legacy_requirement->name));
        const bool invalid_typed_requirement =
            edge.requirement.has_value() &&
            !requirement_name.has_value() &&
            !std::holds_alternative<SonameDependencyRequirement>(
                edge.requirement.value());
        if(malformed_legacy_requirement || invalid_typed_requirement ||
           edge.kind == DependencyKind::Unknown) {
            add_attributed_issue(
                issues,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::UnresolvedDependency,
                    localization::format_translated_message(
                        // TRANSLATORS: The placeholder is a literal dependency specification.
                        "Dependency could not be resolved: {}",
                        edge.dependency_spec),
                    edge.parent_package_name,
                    edge.parent_package_base,
                    edge.dependency_spec),
                roots);
            represented_unresolved_values.insert(edge.dependency_spec);
        }
        if(legacy_requirement.has_value() &&
           legacy_requirement->has_parseable_constraint()) {
            add_attributed_issue(
                issues,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::
                        VersionConstraintUnverified,
                    localization::format_translated_message(
                        "Dependency version constraint is not verified: {}",
                        legacy_requirement->raw),
                    edge.parent_package_name,
                    edge.parent_package_base,
                    edge.dependency_spec),
                roots);
            represented_unresolved_values.insert(
                dependency_constraint_unresolved_reason(
                    edge.dependency_spec));
        }
    }
}

void inspect_unresolved_cycles_and_risks(
    const BuildPlan& plan,
    const std::vector<ReachableDependencyCycle>& reachable_cycles,
    std::vector<AttributedBuildPlanIssue>& issues,
    const std::set<std::string>& represented_unresolved_values) {
    // POLICY(#267): legacy unresolved textは解析しない。typed edgeから対応できない
    // blockerだけをinvocation-wide issueとして残す。
    for(const auto& unresolved : plan.unresolved) {
        if(represented_unresolved_values.contains(unresolved)) continue;
        std::vector<RootTargetIdentity> roots;
        for(const auto& root : plan.root_targets) {
            if(root.requested_name == unresolved) add_root_identity(roots, root);
        }
        add_attributed_issue(
            issues,
            make_localized_execution_issue(
                AurUpdateExecutionReason::UnresolvedDependency,
                localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // internal type name "BuildPlan"; the second is a
                    // package or dependency identity.
                    "{} contains an unresolved dependency: {}",
                    BUILD_PLAN_TYPE_NAME, unresolved),
                std::nullopt, std::nullopt, unresolved),
            roots);
    }

    std::set<std::string> summarized_cycles;
    for(const auto& cycle : plan.cycles) {
        summarized_cycles.insert(cycle);
        const ReachableDependencyCycle* reachable_cycle =
            find_reachable_cycle(reachable_cycles, cycle);
        if(reachable_cycle != nullptr) continue;
        std::vector<RootTargetIdentity> roots =
            roots_for_package(plan, std::nullopt, cycle);
        add_attributed_issue(
            issues,
            make_localized_execution_issue(
                AurUpdateExecutionReason::DependencyCycle,
                localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // field name "PackageBase"; the second is that
                    // field's package identity.
                    "Dependency cycle contains {}: {}",
                    PACKAGE_BASE_FIELD_NAME, cycle),
                std::nullopt, cycle),
            roots);
        add_attributed_issue(
            issues,
            make_localized_execution_issue(
                AurUpdateExecutionReason::BuildPlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "{} cycle summary has no matching reachable dependency cycle.",
                    BUILD_PLAN_TYPE_NAME),
                std::nullopt, cycle),
            std::move(roots));
    }
    for(const auto& reachable_cycle : reachable_cycles) {
        auto summarized_member = std::find_if(
            reachable_cycle.package_bases.begin(),
            reachable_cycle.package_bases.end(),
            [&summarized_cycles](const std::string& package_base) {
                return summarized_cycles.contains(package_base);
            });
        const bool is_summarized =
            summarized_member != reachable_cycle.package_bases.end();
        const std::string& representative = is_summarized
                                                ? *summarized_member
                                                : reachable_cycle.package_bases.front();
        add_attributed_issue(
            issues,
            make_localized_execution_issue(
                AurUpdateExecutionReason::DependencyCycle,
                localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // field name "PackageBase"; the second is that
                    // field's package identity.
                    "Typed dependency graph contains a cycle at {}: {}",
                    PACKAGE_BASE_FIELD_NAME, representative),
                std::nullopt, representative),
            reachable_cycle.roots);
        if(is_summarized) continue;
        add_attributed_issue(
            issues,
            make_localized_execution_issue(
                AurUpdateExecutionReason::BuildPlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Reachable dependency cycle is missing from {} summary.",
                    BUILD_PLAN_TYPE_NAME),
                std::nullopt, representative),
            reachable_cycle.roots);
    }

    for(const auto& ambiguous : plan.ambiguous_providers) {
        std::vector<RootTargetIdentity> roots;
        bool has_matching_edge = false;
        const std::string canonical_dependency =
            parse_dependency_string(ambiguous.dependency).raw;
        for(const auto& edge : plan.dependency_edges) {
            if(edge.kind != DependencyKind::AmbiguousProvider ||
               (edge.requirement.has_value()
                    ? edge.dependency_spec
                    : parse_dependency_string(
                          edge.dependency_spec)
                          .raw) != canonical_dependency) {
                continue;
            }
            has_matching_edge = true;
            for(const auto& root : roots_for_package(
                    plan, edge.parent_package_name,
                    edge.parent_package_base)) {
                add_root_identity(roots, root);
            }
        }
        if(!has_matching_edge) {
            add_attributed_issue(
                issues,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::AmbiguousProvider,
                    localization::format_translated_message(
                        // TRANSLATORS: The placeholder is a literal dependency specification.
                        "Ambiguous provider summary has no matching dependency edge: {}",
                        canonical_dependency),
                    std::nullopt, std::nullopt,
                    ambiguous.dependency),
                roots);
        }
    }

    const PlanStateProjection state = project_build_plan_state(plan);
    const ExecutionReadiness& install_readiness = execution_readiness(
        state, ExecutionCapability::Install);
    for(const auto& readiness_reason : install_readiness.reasons) {
        const auto* relation = std::get_if<PlanDeclaredRelationReason>(
            &readiness_reason.reason);
        if(relation == nullptr ||
           !readiness_reason.blocks_production_guard) {
            continue;
        }
        AurUpdateExecutionIssue issue = make_localized_execution_issue(
            AurUpdateExecutionReason::ConflictsOrReplacesUnresolved,
            package_relation_assessment_diagnostic_display(
                relation->assessment),
            relation->assessment.declaring_package.package_name,
            relation->assessment.declaring_package.package_base,
            std::nullopt);
        issue.relation_reason = *relation;

        std::vector<RootTargetIdentity> roots;
        for(const auto& root :
            relation->assessment.declaring_package.roots) {
            add_root_identity(
                roots,
                RootTargetIdentity{
                    root.invocation_index, root.requested_name});
        }
        add_attributed_issue(
            issues, std::move(issue), std::move(roots));
    }
}

void inspect_package_targets_and_order(
    const BuildPlan& plan,
    const std::vector<AurDependencyGraphEdge>& dependency_graph,
    const std::vector<RootGraphReachability>& root_reachability,
    std::vector<AttributedBuildPlanIssue>& issues) {
    BuildPlanArtifactTargetProjectionResult projection =
        project_build_plan_required_artifact_targets(plan);
    if(!projection.is_success()) {
        for(const auto& projection_issue : projection.failure()->issues) {
            AurUpdateExecutionIssue issue = make_localized_execution_issue(
                projection_issue.kind ==
                        BuildPlanArtifactTargetProjectionIssueKind::
                            PackageBaseMismatch
                    ? AurUpdateExecutionReason::PackageBaseMismatch
                    : AurUpdateExecutionReason::BuildPlanInconsistent,
                projection_issue.diagnostic,
                projection_issue.package_name,
                projection_issue.package_base);
            issue.build_plan_projection_issue = projection_issue;
            add_attributed_issue(
                issues, std::move(issue), projection_issue.roots);
        }
    }

    for(const auto& target : plan.package_targets) {
        bool roots_are_consistent = !target.roots.empty();
        for(const auto& root : target.roots) {
            if(!is_known_plan_root(plan, root)) roots_are_consistent = false;
        }

        bool roles_are_consistent = !target.roles.empty() && std::all_of(
                                                                 target.roles.begin(), target.roles.end(),
                                                                 [](PackageRole role) { return is_known_package_role(role); });
        if(roles_are_consistent &&
           !has_package_role(target, PackageRole::Root)) {
            roles_are_consistent = std::any_of(
                target.roles.begin(), target.roles.end(),
                [](PackageRole role) { return is_dependency_role(role); });
        }

        bool has_self_root = !has_package_role(target, PackageRole::Root);
        if(!has_self_root) {
            has_self_root = std::any_of(
                target.roots.begin(), target.roots.end(),
                [&target](const RootTargetIdentity& root) {
                    return root.requested_name == target.package_name;
                });
        }
        if(!is_valid_package_name(target.package_name) ||
           !is_valid_package_name(target.package_base) ||
           !roots_are_consistent || !roles_are_consistent ||
           !has_self_root) {
            add_attributed_issue(
                issues,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "{} package target identity or root ownership is inconsistent.",
                        BUILD_PLAN_TYPE_NAME),
                    target.package_name, target.package_base),
                target.roots);
        }
        if(roles_are_consistent && roots_are_consistent &&
           !dependency_target_is_grounded_by_rooted_graph(
               target, dependency_graph, root_reachability)) {
            add_attributed_issue(
                issues,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: The placeholders are the literal
                        // internal type name "BuildPlan" and service
                        // name "AUR", respectively.
                        "{} {} dependency target is not grounded in the rooted dependency graph.",
                        BUILD_PLAN_TYPE_NAME,
                        AUR_SERVICE_NAME),
                    target.package_name, target.package_base),
                target.roots);
        }
    }
}

std::vector<AttributedBuildPlanIssue> inspect_build_plan(
    const BuildPlan& plan) {
    std::vector<AttributedBuildPlanIssue> issues;
    std::set<std::string> represented_unresolved_values;
    const std::vector<AurDependencyGraphEdge> dependency_graph =
        collect_aur_dependency_graph(plan);
    const std::vector<RootGraphReachability> root_reachability =
        collect_root_graph_reachability(plan, dependency_graph);
    const std::vector<ReachableDependencyCycle> reachable_cycles =
        collect_reachable_dependency_cycles(
            dependency_graph, root_reachability);

    inspect_resolution_failures(
        plan, issues, represented_unresolved_values);
    inspect_dependency_edges(
        plan, issues, represented_unresolved_values);
    inspect_unresolved_cycles_and_risks(
        plan, reachable_cycles, issues, represented_unresolved_values);
    inspect_package_targets_and_order(
        plan, dependency_graph, root_reachability, issues);
    return issues;
}

void reject_duplicate_requires_check_target_correlations(
    AurUpdateExecutionPreflight& preflight) {
    std::map<std::string, std::vector<std::size_t>> positions_by_name;
    std::set<std::string> requires_check_names;
    for(std::size_t position = 0; position < preflight.targets.size();
        ++position) {
        const AurUpdateExecutionTarget& target =
            preflight.targets[position];
        positions_by_name[target.update.installed_name].push_back(position);
        if(target.update.aur_package.has_value() &&
           target.update.aur_package->aur_name !=
               target.update.installed_name) {
            positions_by_name[target.update.aur_package->aur_name]
                .push_back(position);
        }
        if(project_aur_update_effective_state(target.update) ==
               AurUpdateEffectiveState::RequiresCheck &&
           target.update.aur_package.has_value()) {
            requires_check_names.insert(target.update.installed_name);
            requires_check_names.insert(
                target.update.aur_package->aur_name);
        }
    }

    std::set<std::size_t> rejected_positions;
    for(const auto& [package_name, positions] : positions_by_name) {
        if(!requires_check_names.contains(package_name) ||
           positions.size() <= 1) {
            continue;
        }
        for(const std::size_t position : positions) {
            if(!rejected_positions.insert(position).second) continue;
            add_issue(
                preflight.targets[position],
                make_localized_execution_issue(
                    AurUpdateExecutionReason::UpdatePlanInconsistent,
                    localization::translate_message(
                        DUPLICATE_UPDATE_TARGET_DIAGNOSTIC),
                    package_name));
        }
    }
}

AurUpdateExecutionIssue make_required_devel_target_issue(
    const AurUpdateExpectedRequiredDevelRelation& relation) {
    return AurUpdateExecutionIssue{
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck,
        relation.blocker.package_name,
        relation.blocker.package_base,
        relation.dependency_specification,
        devel_requires_check_diagnostic(
            relation.blocker.devel_requires_check_reason),
        std::nullopt,
        std::nullopt,
        relation.blocker.devel_requires_check_reason,
        relation.blocker};
}

AurUpdateExecutionTarget* find_unique_update_target(
    AurUpdateExecutionPreflight& preflight,
    std::size_t update_plan_index) {
    AurUpdateExecutionTarget* match = nullptr;
    for(AurUpdateExecutionTarget& target : preflight.targets) {
        if(target.update_plan_index != update_plan_index) continue;
        if(match != nullptr) return nullptr;
        match = &target;
    }
    return match;
}

void retain_required_devel_projection_failure(
    AurUpdateExecutionPreflight& preflight,
    const std::vector<CandidateMapping>& candidates) {
    const AurUpdateExecutionIssue issue = make_localized_execution_issue(
        AurUpdateExecutionReason::BuildPlanInconsistent,
        localization::format_translated_message(
            // TRANSLATORS: {} is the literal internal type name "BuildPlan".
            "A blocking {} issue could not be attributed to every affected root.",
            BUILD_PLAN_TYPE_NAME));
    for(const CandidateMapping& candidate : candidates) {
        if(AurUpdateExecutionTarget* target = find_unique_update_target(
               preflight, candidate.update_plan_index);
           target != nullptr) {
            add_issue(*target, issue);
        }
    }
}

void apply_required_devel_relation_projection(
    AurUpdateExecutionPreflight& preflight,
    const std::vector<CandidateMapping>& candidates,
    const AurUpdateRequiredDevelRelationProjection& projection) {
    const bool has_existing_blocker = std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return std::any_of(
                target.issues.begin(), target.issues.end(),
                [](const AurUpdateExecutionIssue& issue) {
                    return issue.reason != AurUpdateExecutionReason::None &&
                           issue.reason !=
                               AurUpdateExecutionReason::UpToDate &&
                           issue.reason != AurUpdateExecutionReason::
                                               NonAurForeign &&
                           issue.reason != AurUpdateExecutionReason::
                                               DevelRequiresCheck;
                });
        });
    bool has_mapping_failure =
        !projection.authority_is_complete && !has_existing_blocker;
    for(const AurUpdateExpectedRequiredDevelRelation& relation :
        projection.relations) {
        AurUpdateExecutionTarget* owner = find_unique_update_target(
            preflight, relation.owner_update_plan_index);
        if(owner == nullptr) {
            has_mapping_failure = true;
            continue;
        }
        add_issue(*owner, make_required_devel_target_issue(relation));
    }
    if(projection.requires_global_fail_closed || has_mapping_failure) {
        retain_required_devel_projection_failure(preflight, candidates);
    }
}
AurUpdateExecutionTarget& candidate_target(
    AurUpdateExecutionPreflight& preflight,
    const CandidateMapping& candidate) {
    return preflight.targets[candidate.update_plan_index];
}

const CandidateMapping* find_candidate_for_root(
    const std::vector<CandidateMapping>& candidates,
    const RootTargetIdentity& root) {
    auto same_root = [&root](const CandidateMapping& candidate) {
        return candidate.candidate_index == root.invocation_index &&
               candidate.requested_name == root.requested_name;
    };
    auto it = std::find_if(candidates.begin(), candidates.end(), same_root);
    return it == candidates.end() ? nullptr : &(*it);
}

void add_issue_to_all_candidates(
    AurUpdateExecutionPreflight& preflight,
    const std::vector<CandidateMapping>& candidates,
    const AurUpdateExecutionIssue& issue) {
    for(const auto& candidate : candidates) {
        add_issue(candidate_target(preflight, candidate), issue);
    }
}

void apply_attributed_build_plan_issues(
    AurUpdateExecutionPreflight& preflight,
    const std::vector<CandidateMapping>& candidates,
    const std::vector<AttributedBuildPlanIssue>& issues) {
    for(const auto& attributed : issues) {
        std::vector<const CandidateMapping*> affected_candidates;
        bool has_unattributed_root = attributed.roots.empty();
        for(const auto& root : attributed.roots) {
            const CandidateMapping* candidate =
                find_candidate_for_root(candidates, root);
            if(candidate == nullptr) {
                has_unattributed_root = true;
                continue;
            }
            if(std::find(
                   affected_candidates.begin(), affected_candidates.end(),
                   candidate) == affected_candidates.end()) {
                affected_candidates.push_back(candidate);
            }
        }

        if(has_unattributed_root) {
            add_issue_to_all_candidates(preflight, candidates, attributed.issue);
            add_issue_to_all_candidates(
                preflight, candidates,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "A blocking {} issue could not be attributed to every affected root.",
                        BUILD_PLAN_TYPE_NAME)));
            continue;
        }

        for(const auto* candidate : affected_candidates) {
            add_issue(candidate_target(preflight, *candidate), attributed.issue);
        }
    }
}

void inspect_combined_build_plan_consistency(
    AurUpdateExecutionPreflight& preflight,
    const std::vector<CandidateMapping>& candidates) {
    const BuildPlan& plan = preflight.build_plan.value();
    if(plan.root_targets.size() != candidates.size()) {
        add_issue_to_all_candidates(
            preflight, candidates,
            make_localized_execution_issue(
                AurUpdateExecutionReason::BuildPlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Combined {} root count does not match the update candidate count.",
                    BUILD_PLAN_TYPE_NAME)));
    }

    for(const auto& candidate : candidates) {
        AurUpdateExecutionTarget& target =
            candidate_target(preflight, candidate);
        if(candidate.candidate_index >= plan.root_targets.size()) {
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "Combined {} is missing the candidate root.",
                        BUILD_PLAN_TYPE_NAME),
                    candidate.requested_name));
            continue;
        }

        const RootTargetIdentity& root =
            plan.root_targets[candidate.candidate_index];
        if(root.invocation_index != candidate.candidate_index ||
           root.requested_name != candidate.requested_name) {
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "Combined {} root identity or order differs from the candidate mapping.",
                        BUILD_PLAN_TYPE_NAME),
                    candidate.requested_name));
            continue;
        }
        target.build_plan_root_index = candidate.candidate_index;

        std::vector<const PlannedPackageTarget*> matching_root_targets;
        for(const auto& package_target : plan.package_targets) {
            if(package_target.package_name != root.requested_name ||
               !has_package_role(package_target, PackageRole::Root) ||
               std::find(
                   package_target.roots.begin(),
                   package_target.roots.end(), root) ==
                   package_target.roots.end()) {
                continue;
            }
            matching_root_targets.push_back(&package_target);
        }
        if(matching_root_targets.size() != 1) {
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    "Candidate root does not have exactly one matching planned package target.",
                    candidate.requested_name));
            continue;
        }

        if(!target.update.aur_package.has_value()) continue;
        const PlannedPackageTarget& package_target =
            *matching_root_targets.front();
        if(package_target.package_name !=
               target.update.aur_package->aur_name ||
           package_target.package_base !=
               target.update.aur_package->package_base) {
            add_issue(
                target,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::PackageBaseMismatch,
                    localization::format_translated_message(
                        // TRANSLATORS: The placeholders are the literal
                        // service name "AUR" and internal type name
                        // "BuildPlan", respectively.
                        "{} update metadata and {} root identity differ.",
                        AUR_SERVICE_NAME,
                        BUILD_PLAN_TYPE_NAME),
                    package_target.package_name,
                    package_target.package_base));
        }
    }

    for(const auto& package_target : plan.package_targets) {
        if(!has_package_role(package_target, PackageRole::Root)) continue;
        if(package_target.roots.empty()) {
            add_issue_to_all_candidates(
                preflight, candidates,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "A {} root package target has no root identity.",
                        BUILD_PLAN_TYPE_NAME),
                    package_target.package_name,
                    package_target.package_base));
            continue;
        }
        for(const auto& root : package_target.roots) {
            if(find_candidate_for_root(candidates, root) != nullptr) continue;
            add_issue_to_all_candidates(
                preflight, candidates,
                make_localized_execution_issue(
                    AurUpdateExecutionReason::BuildPlanInconsistent,
                    localization::format_translated_message(
                        // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                        "A {} root package target references an unknown candidate root.",
                        BUILD_PLAN_TYPE_NAME),
                    package_target.package_name,
                    package_target.package_base));
        }
    }
}

} // namespace

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy) {
    return resolve_aur_update_execution_preflight(
        update_plan, devel_requires_check_policy,
        ProviderSelectionCallback{});
}

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    const ProviderSelectionCallback& select_provider) {
    AurUpdateExecutionPreflight preflight;
    preflight.devel_requires_check_policy =
        devel_requires_check_policy;
    preflight.targets.reserve(update_plan.entries.size());

    std::vector<CandidateMapping> candidates;
    std::map<std::string, std::vector<std::size_t>> candidate_indices_by_name;
    bool has_invalid_candidate_name = false;

    for(std::size_t plan_index = 0; plan_index < update_plan.entries.size();
        ++plan_index) {
        AurUpdateExecutionTarget target;
        target.update_plan_index = plan_index;
        target.update = update_plan.entries[plan_index];
        add_initial_classification_issue(target);

        if(project_aur_update_effective_state(target.update) ==
           AurUpdateEffectiveState::UpdateAvailable) {
            target.desired_install_reason =
                desired_install_reason_for_aur_update_root(
                    target.update.install_reason);
            if(!target.desired_install_reason.has_value()) {
                add_issue(
                    target,
                    make_localized_execution_issue(
                        AurUpdateExecutionReason::InstalledReasonUnknown,
                        "Installed package reason is unknown.",
                        target.update.installed_name));
            }

            const std::size_t candidate_index = candidates.size();
            candidates.push_back(CandidateMapping{
                candidate_index, plan_index,
                target.update.installed_name});
            candidate_indices_by_name[target.update.installed_name].push_back(
                plan_index);
            if(!is_valid_package_name(target.update.installed_name)) {
                has_invalid_candidate_name = true;
                add_issue(
                    target,
                    make_localized_execution_issue(
                        AurUpdateExecutionReason::UpdatePlanInconsistent,
                        "Update candidate has an invalid installed package name.",
                        target.update.installed_name));
            }
        }

        preflight.targets.push_back(std::move(target));
    }

    if(!is_known_devel_requires_check_policy(
           devel_requires_check_policy)) {
        for(auto& target : preflight.targets)
            reduce_target_status(
                target, devel_requires_check_policy, false, false);
        return preflight;
    }

    if(devel_requires_check_policy ==
       DevelRequiresCheckPolicy::SkipIndependentTarget) {
        reject_duplicate_requires_check_target_correlations(preflight);
    }

    bool has_duplicate_candidate = false;
    for(const auto& [package_name, plan_indices] :
        candidate_indices_by_name) {
        if(plan_indices.size() <= 1) continue;
        has_duplicate_candidate = true;
        for(const auto plan_index : plan_indices) {
            add_issue(
                preflight.targets[plan_index],
                make_localized_execution_issue(
                    AurUpdateExecutionReason::DuplicateUpdateTarget,
                    "Update candidate occurs more than once in the invocation.",
                    package_name));
        }
    }

    if(candidates.empty()) {
        for(auto& target : preflight.targets)
            reduce_target_status(
                target, devel_requires_check_policy, true, false);
        return preflight;
    }

    if(has_duplicate_candidate || has_invalid_candidate_name) {
        add_issue_to_all_candidates(
            preflight, candidates,
            make_localized_execution_issue(
                AurUpdateExecutionReason::BuildPlanInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Combined {} resolution was skipped because the candidate set is inconsistent.",
                    BUILD_PLAN_TYPE_NAME)));
        for(auto& target : preflight.targets)
            reduce_target_status(
                target, devel_requires_check_policy, false, false);
        return preflight;
    }

    std::vector<std::string> candidate_names;
    candidate_names.reserve(candidates.size());
    for(const auto& candidate : candidates) {
        candidate_names.push_back(candidate.requested_name);
    }

    // POLICY(#267): invocation全体で一度だけ解決し、candidate順とexecution順を混ぜない。
    preflight.build_plan = resolve_build_plan_for_preflight(
        candidate_names, select_provider);
    inspect_combined_build_plan_consistency(preflight, candidates);
    std::vector<AttributedBuildPlanIssue> attributed_issues =
        inspect_build_plan(preflight.build_plan.value());
    AurUpdateRequiredDevelRelationProjection required_devel_projection;
    if(devel_requires_check_policy ==
       DevelRequiresCheckPolicy::SkipIndependentTarget) {
        required_devel_projection =
            project_aur_update_required_devel_relations(preflight);
        apply_required_devel_relation_projection(
            preflight, candidates, required_devel_projection);
    }
    apply_attributed_build_plan_issues(
        preflight, candidates, attributed_issues);

    for(auto& target : preflight.targets) {
        const bool is_required =
            std::find(
                required_devel_projection.required_update_plan_indices
                    .begin(),
                required_devel_projection.required_update_plan_indices.end(),
                target.update_plan_index) !=
            required_devel_projection.required_update_plan_indices.end();
        reduce_target_status(
            target, devel_requires_check_policy, true,
            is_required);
    }
    if(devel_requires_check_policy ==
           DevelRequiresCheckPolicy::SkipIndependentTarget &&
       !has_complete_aur_update_required_devel_relation_snapshot(
           preflight) &&
       !std::any_of(
           preflight.targets.begin(), preflight.targets.end(),
           [](const AurUpdateExecutionTarget& target) {
               return target.status ==
                          AurUpdateExecutionTargetStatus::Incomplete ||
                      target.status ==
                          AurUpdateExecutionTargetStatus::Unsupported;
           })) {
        retain_required_devel_projection_failure(preflight, candidates);
        for(auto& target : preflight.targets) {
            const bool is_required =
                std::find(
                    required_devel_projection
                        .required_update_plan_indices.begin(),
                    required_devel_projection
                        .required_update_plan_indices.end(),
                    target.update_plan_index) !=
                required_devel_projection
                    .required_update_plan_indices.end();
            reduce_target_status(
                target, devel_requires_check_policy, true,
                is_required);
        }
    }
    return preflight;
}

bool has_executable_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return target.status ==
                   AurUpdateExecutionTargetStatus::Executable;
        });
}

bool has_blocking_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    if(!has_valid_aur_update_execution_policy_snapshot(preflight)) {
        return true;
    }
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return target.status ==
                       AurUpdateExecutionTargetStatus::Unsupported ||
                   target.status ==
                       AurUpdateExecutionTargetStatus::Incomplete;
        });
}

bool can_execute(const AurUpdateExecutionPreflight& preflight) noexcept {
    return has_executable_targets(preflight) &&
           !has_blocking_targets(preflight);
}
