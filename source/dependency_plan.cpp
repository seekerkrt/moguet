#include "dependency_plan.hpp"

#include "build_plan_relation_assessment.hpp"
#include "dependency_plan_projection_support.hpp"
#include "dependency_provider.hpp"
#include "dependency_spec.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "package_relation_observation_adapter.hpp"
#include "repository_query.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

const int MAX_RECURSIVE_DEP_DEPTH = 16;

enum class BuildPlanResolutionMode {
    Legacy,
    CaptureOrdinaryFailures
};

struct BuildPlanResolutionFailureContext {
    BuildPlan& plan;
    RootTargetIdentity root;
    std::optional<std::string> parent_package_name;
    std::optional<std::string> parent_package_base;
    std::optional<std::string> dependency_specification;
};

struct ProviderCandidateDiscovery {
    std::vector<ProvidedDependency> candidates;
    bool is_complete = true;
    std::optional<ObservedVersionUnknownReason> incomplete_reason =
        std::nullopt;
    std::vector<RepositoryExactPackage> repository_observations;
};

class AurConstraintMetadataProjectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

AurPackageInfo require_typed_aur_package_info(AurPackageInfo package) {
    if(package.constraint_metadata.has_value()) return package;
    AurConstraintMetadataProjectionResult projection =
        project_aur_constraint_metadata(package);
    if(const auto* metadata =
           std::get_if<AurPackageConstraintMetadata>(&projection);
       metadata != nullptr) {
        package.constraint_metadata = *metadata;
        return package;
    }
    throw AurConstraintMetadataProjectionError(
        localization::format_translated_message(
            "{} package metadata constraint projection failed: {}",
            "AUR", package.Name));
}

std::optional<AurPackageInfo> query_aur_package_info(
    const std::string& package_name, BuildPlanResolutionMode mode,
    bool require_authoritative_metadata = false) {
    std::optional<AurPackageInfo> package =
        mode == BuildPlanResolutionMode::CaptureOrdinaryFailures ||
                require_authoritative_metadata
            ? AurClient::info_strict(package_name)
            : AurClient::info(package_name);
    if(package.has_value()) {
        package = require_typed_aur_package_info(std::move(package.value()));
    }
    return package;
}

// dependency planをmonolithへ逆依存させず、汎用utilityを公開しないためのlocal helper。
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

void add_unique_value(std::vector<std::string>& values, const std::string& value) {
    std::string trimmed = trim(value);
    if(trimmed.empty()) return;
    if(std::find(values.begin(), values.end(), trimmed) == values.end()) values.push_back(trimmed);
}

std::string package_base_name(const AurPackageInfo& info) {
    // POLICY(#174): PackageBase は strict AUR RPC parser の required identifier。
    return info.PackageBase;
}

bool matches_selected_aur_provider_contract(
    const AurPackageInfo& info,
    const ProvidedDependency& selected_provider) {
    if(!std::holds_alternative<AurProviderOrigin>(
           selected_provider.origin) ||
       info.Name != selected_provider.package_name ||
       info.PackageBase != selected_provider.package_base) {
        return false;
    }
    if(!info.constraint_metadata.has_value() ||
       !selected_provider.constraint_metadata.has_value()) {
        return false;
    }
    const auto matching = std::find_if(
        info.constraint_metadata->provides.begin(),
        info.constraint_metadata->provides.end(),
        [&selected_provider](
            const AurProviderCapabilityMetadata& provided) {
            return provided.capability.package_name() ==
                   selected_provider.provided_dependency_name;
        });
    if(matching == info.constraint_metadata->provides.end()) return false;
    return selected_provider.provided_dependency_specification ==
               matching->capability.raw_specification() &&
           selected_provider.constraint_metadata->provided_capability ==
               matching->capability &&
           selected_provider.constraint_metadata->provided_version ==
               matching->provided_version;
}

std::string selected_aur_provider_revalidation_failure_diagnostic(
    const ProvidedDependency& selected_provider) {
    return localization::format_translated_message(
        "{} provider candidate changed during dependency resolution: {}",
        "AUR", selected_provider.package_name);
}

void add_provider_candidate(std::vector<ProvidedDependency>& candidates, const ProvidedDependency& provider) {
    auto same = [&provider](const ProvidedDependency& existing) {
        return same_provider_identity(existing, provider);
    };
    if(std::find_if(candidates.begin(), candidates.end(), same) != candidates.end()) return;
    candidates.push_back(provider);
}

std::optional<ProvidedDependency> aur_provider_from_metadata(
    const AurPackageInfo& info, const std::string& dependency_name) {
    if(!info.constraint_metadata.has_value()) return std::nullopt;
    const AurPackageConstraintMetadata& metadata =
        info.constraint_metadata.value();
    const auto provided = std::find_if(
        metadata.provides.begin(), metadata.provides.end(),
        [&dependency_name](
            const AurProviderCapabilityMetadata& candidate) {
            return candidate.capability.package_name() == dependency_name;
        });
    if(provided == metadata.provides.end()) return std::nullopt;
    return ProvidedDependency::from_aur_constraint_metadata(
        metadata.package_name, metadata.package_base,
        ProviderConstraintMetadata{
            provided->capability, metadata.package_version,
            provided->provided_version});
}

bool same_resolution_failure(
    const BuildPlanResolutionFailure& lhs,
    const BuildPlanResolutionFailure& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.parent_package_name == rhs.parent_package_name &&
           lhs.parent_package_base == rhs.parent_package_base &&
           lhs.subject == rhs.subject &&
           lhs.dependency_specification == rhs.dependency_specification;
}

void add_resolution_failure(
    BuildPlanResolutionFailureContext* context,
    BuildPlanResolutionFailureKind kind, const std::string& subject,
    const std::string& diagnostic) {
    if(context == nullptr) return;

    BuildPlanResolutionFailure failure{
        kind,
        context->parent_package_name,
        context->parent_package_base,
        subject,
        context->dependency_specification,
        {context->root},
        diagnostic};
    auto same_failure = [&failure](const BuildPlanResolutionFailure& existing) {
        return same_resolution_failure(existing, failure);
    };
    auto it = std::find_if(
        context->plan.resolution_failures.begin(),
        context->plan.resolution_failures.end(), same_failure);
    if(it == context->plan.resolution_failures.end()) {
        context->plan.resolution_failures.push_back(std::move(failure));
        return;
    }
    if(std::find(it->roots.begin(), it->roots.end(), context->root) == it->roots.end()) {
        it->roots.push_back(context->root);
    }
}

enum class RepositoryPackageQueryStatus {
    Present,
    NotFound,
    Unavailable
};

void retain_configured_repository_order(
    BuildPlan* plan,
    const std::optional<std::vector<std::string>>& configured_order) {
    if(plan == nullptr || !configured_order.has_value()) return;
    if(!plan->configured_repository_order.has_value()) {
        plan->configured_repository_order = configured_order;
        return;
    }
    if(plan->configured_repository_order.value() != configured_order.value()) {
        throw std::runtime_error(
            "Repository configuration changed during BuildPlan resolution.");
    }
}

RepositoryPackageQueryStatus query_repository_package(
    const std::string& package_name,
    BuildPlanResolutionFailureContext* failure_context,
    std::optional<RepositoryPackagePresent>* present_package = nullptr,
    BuildPlan* repository_configuration_authority = nullptr) {
    StrictRepositoryPackageQueryResult result =
        query_repository_package_strict(package_name);
    if(const auto* present =
           std::get_if<RepositoryPackagePresent>(&result);
       present != nullptr) {
        retain_configured_repository_order(
            repository_configuration_authority,
            present->configured_repository_order);
        if(present_package != nullptr) *present_package = *present;
        return RepositoryPackageQueryStatus::Present;
    }
    if(const auto* not_found =
           std::get_if<RepositoryPackageNotFound>(&result);
       not_found != nullptr) {
        retain_configured_repository_order(
            repository_configuration_authority,
            not_found->configured_repository_order);
        return RepositoryPackageQueryStatus::NotFound;
    }

    const RepositoryMetadataFailure& failure =
        std::get<RepositoryMetadataFailure>(result);
    retain_configured_repository_order(
        repository_configuration_authority,
        failure.configured_repository_order);
    if(failure_context == nullptr) {
        throw std::runtime_error(failure.diagnostic);
    }
    add_resolution_failure(
        failure_context,
        BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
        package_name, failure.diagnostic);
    return RepositoryPackageQueryStatus::Unavailable;
}

ConstraintEvaluation evaluate_dependency_requirement(
    const DependencyRequirement& requirement,
    const ObservedVersion& observed_version) {
    if(const auto* consumer =
           std::get_if<ConsumerDependencyRequirement>(&requirement);
       consumer != nullptr) {
        return evaluate_consumer_dependency_requirement(
            *consumer, observed_version);
    }
    if(std::holds_alternative<SonameDependencyRequirement>(requirement)) {
        return ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::RelationKindNotComparable);
    }
    return ConstraintEvaluation::invalid(
        ConstraintInvalidReason::InternalInvariantViolation);
}

RepositoryExactPackage repository_candidate(
    const RepositoryPackagePresent& present,
    const std::string& package_name) {
    const ObservedVersion observed_version =
        present.package_version.value_or(ObservedVersion::unknown(
            ObservedVersionSource::RepositoryExactPackage,
            ObservedVersionUnknownReason::MetadataQueryFailure));
    return RepositoryExactPackage{
        ConfiguredRepositoryIdentity{
            present.repository_name, present.configured_order},
        present.package_name.empty() ? package_name
                                     : present.package_name,
        present.package_base,
        observed_version,
        present.provides};
}

AurResolvedDependencyCandidate aur_candidate(
    const AurPackageInfo& package) {
    if(!package.constraint_metadata.has_value()) {
        throw std::logic_error(localization::format_translated_message(
            "{} package metadata is missing its typed constraint projection: {}",
            "AUR", package.Name));
    }
    const AurPackageConstraintMetadata& metadata =
        package.constraint_metadata.value();
    if(metadata.package_name != package.Name ||
       metadata.package_base != package.PackageBase) {
        throw std::logic_error(localization::format_translated_message(
            "{} package metadata constraint identity is inconsistent: {}",
            "AUR", package.Name));
    }
    return AurResolvedDependencyCandidate{
        metadata.package_name, metadata.package_base,
        metadata.package_version};
}

ConstraintEvaluation evaluate_provider_requirement(
    const DependencyRequirement& requirement,
    const ProvidedDependency& provider) {
    const auto* consumer =
        std::get_if<ConsumerDependencyRequirement>(&requirement);
    if(consumer == nullptr) {
        return ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::RelationKindNotComparable);
    }
    if(!provider.constraint_metadata.has_value()) {
        return ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::CandidateVersionUnavailable);
    }
    const ProviderConstraintMetadata& metadata =
        provider.constraint_metadata.value();
    if(metadata.provided_capability.package_name() !=
       consumer->package_name()) {
        return ConstraintEvaluation::invalid(
            ConstraintInvalidReason::InternalInvariantViolation);
    }
    return evaluate_consumer_dependency_requirement(
        *consumer, metadata.provided_version);
}

ObservedVersion provider_observed_version(
    const ProvidedDependency& provider) {
    if(provider.constraint_metadata.has_value()) {
        return provider.constraint_metadata->provided_version;
    }
    const ObservedVersionSource source =
        std::holds_alternative<RepositoryProviderOrigin>(provider.origin)
            ? ObservedVersionSource::RepositoryProviderCapability
            : ObservedVersionSource::AurProviderCapability;
    return ObservedVersion::unknown(
        source,
        ObservedVersionUnknownReason::CandidateVersionUnavailable);
}

const RepositoryExactPackage* repository_provider_observation(
    const ProviderCandidateDiscovery& discovery,
    const ProvidedDependency& provider) {
    const auto* origin =
        std::get_if<RepositoryProviderOrigin>(&provider.origin);
    if(origin == nullptr || !origin->configured_order.has_value()) {
        return nullptr;
    }
    const auto matching = std::find_if(
        discovery.repository_observations.begin(),
        discovery.repository_observations.end(),
        [&provider, origin](const RepositoryExactPackage& package) {
            return package.package_name == provider.package_name &&
                   package.repository.repository_name ==
                       origin->repository_name &&
                   package.repository.configured_order ==
                       origin->configured_order.value();
        });
    return matching == discovery.repository_observations.end()
               ? nullptr
               : &(*matching);
}

ProviderCandidateDiscovery find_aur_providers(
    const std::string& dependency_name,
    BuildPlanResolutionFailureContext* failure_context = nullptr,
    bool require_complete_candidates = false) {
    ProviderCandidateDiscovery discovery;
    if(!is_valid_package_name(dependency_name)) return discovery;

    const bool use_strict_query =
        failure_context != nullptr || require_complete_candidates;
    std::vector<std::string> candidates;
    try {
        if(use_strict_query) {
            candidates = AurClient::search_names_by_provides_strict(dependency_name);
        } else {
            candidates = AurClient::search_names_by_provides(dependency_name);
        }
    } catch(const AurRpcResponseError&) {
        throw;
    } catch(const AurConstraintMetadataProjectionError&) {
        throw;
    } catch(const std::exception& e) {
        add_resolution_failure(
            failure_context,
            BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
            dependency_name, e.what());
        Logger::warn(localization::format_translated_message(
            "Failed to search {} providers for {}: {}",
            "AUR", dependency_name, e.what()));
        discovery.is_complete = false;
        discovery.incomplete_reason =
            ObservedVersionUnknownReason::MetadataQueryFailure;
        return discovery;
    }
    for(const auto& candidate : candidates) {
        std::optional<AurPackageInfo> info;
        try {
            info = use_strict_query
                       ? AurClient::info_strict(candidate)
                       : AurClient::info(candidate);
        } catch(const AurRpcResponseError&) {
            throw;
        } catch(const std::exception& e) {
            discovery.is_complete = false;
            discovery.incomplete_reason =
                ObservedVersionUnknownReason::PartialSourceFailure;
            add_resolution_failure(
                failure_context,
                BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable,
                candidate, e.what());
            Logger::warn(localization::format_translated_message(
                "Failed to check {} provider {}: {}",
                "AUR", candidate, e.what()));
            continue;
        }

        if(info.has_value()) {
            // Schema/constraint projection failure is malformed authoritative
            // metadata and remains strict rather than becoming a partial
            // query failure.
            info = require_typed_aur_package_info(
                std::move(info.value()));
        }
        std::optional<ProvidedDependency> provider;
        if(info.has_value()) {
            provider = aur_provider_from_metadata(
                info.value(), dependency_name);
        }
        if(provider.has_value()) {
            add_provider_candidate(
                discovery.candidates, provider.value());
            continue;
        }
        if(info.has_value()) {
            // Current authoritative metadata confirms that this package no
            // longer provides the requested capability. It is a candidate
            // disappearance, not a query failure.
            continue;
        }

        discovery.is_complete = false;
        discovery.incomplete_reason =
            ObservedVersionUnknownReason::PartialSourceFailure;
        const std::string diagnostic =
            localization::format_translated_message(
                "{} provider candidate metadata was not returned.",
                "AUR");
        add_resolution_failure(
            failure_context,
            BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable,
            candidate, diagnostic);
    }

    return discovery;
}

ProviderCandidateDiscovery find_dependency_providers(
    const std::string& dependency_name,
    BuildPlanResolutionFailureContext* failure_context = nullptr,
    bool require_authoritative_candidates = false,
    BuildPlan* repository_configuration_authority = nullptr) {
    static_cast<void>(require_authoritative_candidates);
    StrictRepositoryProvidersQueryResult result =
        query_repository_providers_strict(dependency_name);
    if(const auto* failure = std::get_if<RepositoryMetadataFailure>(&result);
       failure != nullptr) {
        retain_configured_repository_order(
            repository_configuration_authority,
            failure->configured_repository_order);
        add_resolution_failure(
            failure_context,
            BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
            dependency_name, failure->diagnostic);
        return ProviderCandidateDiscovery{
            {}, false, ObservedVersionUnknownReason::MetadataQueryFailure, {}};
    }
    RepositoryProviderQuerySnapshot repository =
        std::get<RepositoryProviderQuerySnapshot>(std::move(result));
    retain_configured_repository_order(
        repository_configuration_authority,
        repository.configured_repository_order);
    if(!repository.source_failures.empty()) {
        for(const auto& failure : repository.source_failures) {
            add_resolution_failure(
                failure_context,
                BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
                dependency_name, failure.diagnostic);
        }
        return ProviderCandidateDiscovery{
            std::move(repository.candidates), false,
            ObservedVersionUnknownReason::PartialSourceFailure,
            std::move(repository.observed_packages)};
    }

    // POLICY: pacman-first。official repo provider が見つかる場合は AUR provider を混ぜない。
    if(!repository.candidates.empty()) {
        return ProviderCandidateDiscovery{
            std::move(repository.candidates), true, std::nullopt,
            std::move(repository.observed_packages)};
    }
    return find_aur_providers(
        dependency_name, failure_context,
        require_authoritative_candidates);
}

void add_dependency(
    std::vector<std::string>& dependencies, std::set<std::string>& seen,
    const std::string& dependency) {
    std::string dep = trim(dependency);
    if(dep.empty()) return;
    if(seen.insert(dep).second) dependencies.push_back(dep);
}

void add_typed_dependency(
    std::vector<TypedPackageDependency>& dependencies,
    const std::string& dependency, PackageRole role,
    const DependencyRequirement& requirement) {
    if(trim(dependency).empty()) return;

    auto same_dependency = [&dependency, role](const TypedPackageDependency& existing) {
        return existing.specification == dependency && existing.role == role;
    };
    if(std::find_if(dependencies.begin(), dependencies.end(), same_dependency) != dependencies.end()) return;
    dependencies.push_back(
        TypedPackageDependency{dependency, role, requirement});
}

void add_ambiguous_provider_dependency(
    std::vector<AmbiguousProvidedDependency>& dependencies, const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates) {
    std::string trimmed = trim(dependency);
    if(trimmed.empty() || candidates.empty()) return;

    // POLICY(#97/#143): 複数 provider はここで集約し、暗黙選択しない。
    auto same_dependency = [&trimmed](const AmbiguousProvidedDependency& existing) {
        return existing.dependency == trimmed;
    };
    auto it = std::find_if(dependencies.begin(), dependencies.end(), same_dependency);
    if(it == dependencies.end()) {
        dependencies.push_back(AmbiguousProvidedDependency{trimmed, {}});
        it = std::prev(dependencies.end());
    }
    for(const auto& candidate : candidates) {
        add_provider_candidate(it->candidates, candidate);
    }
}

std::optional<ProvidedDependency> select_provider_candidate(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates,
    const ProviderSelectionCallback& select_provider) {
    if(candidates.empty() || !select_provider) return std::nullopt;

    std::optional<ProvidedDependency> selected =
        select_provider(dependency, candidates);
    if(!selected.has_value()) return std::nullopt;

    auto matching_candidate = std::find_if(
        candidates.begin(), candidates.end(),
        [&selected](const ProvidedDependency& candidate) {
            return same_provider_identity(candidate, selected.value());
        });
    if(matching_candidate == candidates.end()) {
        throw std::logic_error(localization::format_translated_message(
            "Provider selection returned a candidate that was not offered for {}.",
            dependency));
    }
    // Resolver-owned metadata is authoritative even when an injected selector
    // returns an identity-only value.
    return *matching_candidate;
}

} // namespace

std::vector<std::string> collect_build_dependencies(const AurPackageInfo& pkg) {
    std::vector<std::string> dependencies;
    std::set<std::string> seen;

    for(const auto& dep : pkg.Depends)
        add_dependency(dependencies, seen, dep);
    for(const auto& dep : pkg.MakeDepends)
        add_dependency(dependencies, seen, dep);
    for(const auto& dep : pkg.CheckDepends)
        add_dependency(dependencies, seen, dep);

    return dependencies;
}

std::vector<TypedPackageDependency> collect_typed_build_dependencies(const AurPackageInfo& pkg) {
    std::optional<AurPackageConstraintMetadata> projected_metadata;
    if(!pkg.constraint_metadata.has_value()) {
        AurConstraintMetadataProjectionResult projection =
            project_aur_constraint_metadata(pkg);
        if(const auto* metadata =
               std::get_if<AurPackageConstraintMetadata>(&projection);
           metadata != nullptr) {
            projected_metadata = *metadata;
        } else {
            throw std::logic_error(localization::format_translated_message(
                "{} package metadata constraint projection failed: {}",
                "AUR", pkg.Name));
        }
    }

    std::vector<TypedPackageDependency> dependencies;
    const AurPackageConstraintMetadata& metadata =
        pkg.constraint_metadata.has_value()
            ? pkg.constraint_metadata.value()
            : projected_metadata.value();
    if(pkg.Depends.size() != metadata.depends.size() ||
       pkg.MakeDepends.size() != metadata.make_depends.size() ||
       pkg.CheckDepends.size() != metadata.check_depends.size()) {
        throw std::logic_error(localization::format_translated_message(
            "{} package metadata constraint projection is inconsistent: {}",
            "AUR", pkg.Name));
    }

    for(std::size_t index = 0; index < pkg.Depends.size(); ++index) {
        add_typed_dependency(
            dependencies, pkg.Depends[index],
            PackageRole::RuntimeDependency, metadata.depends[index]);
    }
    for(std::size_t index = 0; index < pkg.MakeDepends.size(); ++index) {
        add_typed_dependency(
            dependencies, pkg.MakeDepends[index],
            PackageRole::BuildDependency,
            metadata.make_depends[index]);
    }
    for(std::size_t index = 0; index < pkg.CheckDepends.size(); ++index) {
        add_typed_dependency(
            dependencies, pkg.CheckDepends[index],
            PackageRole::CheckDependency,
            metadata.check_depends[index]);
    }

    return dependencies;
}

namespace {

RecursiveDependencyNode resolve_recursive_dependency(
    const std::string& dependency, std::set<std::string>& visited,
    int depth, int max_depth,
    const ProviderSelectionCallback& select_provider);

std::vector<RecursiveDependencyNode> resolve_recursive_dependencies(
    const AurPackageInfo& pkg, std::set<std::string>& visited,
    int depth, int max_depth,
    const ProviderSelectionCallback& select_provider) {
    std::vector<RecursiveDependencyNode> nodes;
    for(const auto& dependency : collect_build_dependencies(pkg)) {
        nodes.push_back(resolve_recursive_dependency(
            dependency, visited, depth, max_depth, select_provider));
    }
    return nodes;
}

void populate_recursive_aur_provider_children(
    RecursiveDependencyNode& node, const AurPackageInfo& info,
    std::set<std::string>& visited, int depth, int max_depth,
    const ProviderSelectionCallback& select_provider) {
    node.package_base = package_base_name(info);
    if(!visited.insert(node.package_base).second) {
        node.already_visited = true;
        return;
    }
    if(depth >= max_depth) {
        node.max_depth_reached = true;
        return;
    }
    node.children = resolve_recursive_dependencies(
        info, visited, depth + 1, max_depth, select_provider);
}

RecursiveDependencyNode resolve_recursive_dependency(
    const std::string& dependency, std::set<std::string>& visited,
    int depth, int max_depth,
    const ProviderSelectionCallback& select_provider) {
    RecursiveDependencyNode node;
    node.dependency = dependency;
    ParsedDependency parsed = parse_dependency_string(dependency);
    node.package_name = parsed.name;

    if(!is_valid_package_name(node.package_name) || parsed.has_malformed_constraint()) {
        node.kind = DependencyKind::Unknown;
        return node;
    }

    if(query_repository_package(
           node.package_name, nullptr) ==
       RepositoryPackageQueryStatus::Present) {
        node.kind = DependencyKind::Repo;
        return node;
    }

    std::optional<AurPackageInfo> info;
    try {
        info = select_provider
                   ? AurClient::info_strict(node.package_name)
                   : AurClient::info(node.package_name);
        if(info.has_value()) {
            info = require_typed_aur_package_info(std::move(info.value()));
        }
    } catch(const AurRpcResponseError&) {
        throw;
    } catch(const AurConstraintMetadataProjectionError&) {
        throw;
    } catch(const std::exception& e) {
        if(select_provider) throw;
        Logger::warn(localization::format_translated_message(
            "Failed to check {} dependency {}: {}",
            "AUR", node.package_name, e.what()));
        node.kind = DependencyKind::Unknown;
        return node;
    }

    if(!info.has_value()) {
        ProviderCandidateDiscovery discovery =
            find_dependency_providers(
                node.package_name, nullptr,
                static_cast<bool>(select_provider));
        if(!discovery.is_complete) {
            node.kind = DependencyKind::Unknown;
            node.provider_candidates = discovery.candidates;
            return node;
        }
        const std::vector<ProvidedDependency>& providers =
            discovery.candidates;
        std::optional<ProvidedDependency> resolved_provider =
            select_provider_candidate(
                dependency, providers, select_provider);
        if(resolved_provider.has_value()) {
            node.provider_resolution =
                ProviderResolutionKind::UserSelected;
        } else if(providers.size() == 1) {
            resolved_provider = providers.front();
        } else if(providers.size() > 1) {
            node.kind = DependencyKind::AmbiguousProvider;
            node.provider_candidates = providers;
        } else {
            node.kind = DependencyKind::Unknown;
        }

        if(!resolved_provider.has_value()) return node;
        node.kind = DependencyKind::Provided;
        node.provided_by = resolved_provider;
        if(std::holds_alternative<RepositoryProviderOrigin>(
               resolved_provider->origin)) {
            return node;
        }
        if(node.provider_resolution !=
           ProviderResolutionKind::UserSelected) {
            return node;
        }

        std::optional<AurPackageInfo> provider_info =
            AurClient::info_strict(resolved_provider->package_name);
        if(provider_info.has_value()) {
            provider_info = require_typed_aur_package_info(
                std::move(provider_info.value()));
        }
        if(!provider_info.has_value()) {
            node.kind = DependencyKind::Unknown;
            node.provided_by.reset();
            return node;
        }
        if(!matches_selected_aur_provider_contract(
               provider_info.value(), resolved_provider.value())) {
            throw std::runtime_error(
                selected_aur_provider_revalidation_failure_diagnostic(
                    resolved_provider.value()));
        }
        populate_recursive_aur_provider_children(
            node, provider_info.value(), visited, depth, max_depth,
            select_provider);
        return node;
    }

    node.kind = DependencyKind::Aur;
    populate_recursive_aur_provider_children(
        node, info.value(), visited, depth, max_depth,
        select_provider);
    return node;
}

} // namespace

std::vector<RecursiveDependencyNode> resolve_recursive_dependencies(
    const AurPackageInfo& pkg,
    const ProviderSelectionCallback& select_provider) {
    std::set<std::string> visited;
    visited.insert(package_base_name(pkg));
    return resolve_recursive_dependencies(
        pkg, visited, 1, MAX_RECURSIVE_DEP_DEPTH, select_provider);
}

std::vector<RecursiveDependencyNode> resolve_recursive_dependencies(
    const AurPackageInfo& pkg) {
    return resolve_recursive_dependencies(pkg, ProviderSelectionCallback{});
}

namespace {

int package_role_rank(PackageRole role) {
    switch(role) {
        case PackageRole::Root:
            return 0;
        case PackageRole::RuntimeDependency:
            return 1;
        case PackageRole::BuildDependency:
            return 2;
        case PackageRole::CheckDependency:
            return 3;
    }
    throw std::logic_error(
        localization::translate_message("Unknown package role."));
}

void add_package_role(std::vector<PackageRole>& roles, PackageRole role) {
    if(std::find(roles.begin(), roles.end(), role) != roles.end()) return;
    roles.push_back(role);
    std::sort(roles.begin(), roles.end(), [](PackageRole lhs, PackageRole rhs) {
        return package_role_rank(lhs) < package_role_rank(rhs);
    });
}

bool root_identity_less(const RootTargetIdentity& lhs, const RootTargetIdentity& rhs) {
    if(lhs.invocation_index != rhs.invocation_index) {
        return lhs.invocation_index < rhs.invocation_index;
    }
    return lhs.requested_name < rhs.requested_name;
}

void add_root_identity(
    std::vector<RootTargetIdentity>& roots, const RootTargetIdentity& root) {
    if(std::find(roots.begin(), roots.end(), root) != roots.end()) return;
    roots.push_back(root);
    std::sort(roots.begin(), roots.end(), root_identity_less);
}

PlannedPackageTarget* find_package_target(
    BuildPlan& plan, const std::string& package_name) {
    auto same_name = [&package_name](const PlannedPackageTarget& target) {
        return target.package_name == package_name;
    };
    auto it = std::find_if(plan.package_targets.begin(), plan.package_targets.end(), same_name);
    return it == plan.package_targets.end() ? nullptr : &(*it);
}

const PlannedPackageTarget* find_package_target(
    const BuildPlan& plan, const std::string& package_name) {
    auto same_name = [&package_name](const PlannedPackageTarget& target) {
        return target.package_name == package_name;
    };
    auto it = std::find_if(plan.package_targets.begin(), plan.package_targets.end(), same_name);
    return it == plan.package_targets.end() ? nullptr : &(*it);
}

void add_planned_package_target(
    BuildPlan& plan, const std::string& package_name,
    const std::string& package_base,
    const std::vector<PackageRole>& roles, const RootTargetIdentity& root) {
    PlannedPackageTarget* target = find_package_target(plan, package_name);
    if(target == nullptr) {
        plan.package_targets.push_back(
            PlannedPackageTarget{package_name, package_base, {}, {}});
        target = &plan.package_targets.back();
    }

    for(const auto role : roles)
        add_package_role(target->roles, role);
    add_root_identity(target->roots, root);
}

void add_planned_package_target(
    BuildPlan& plan, const AurPackageInfo& info,
    const std::vector<PackageRole>& roles, const RootTargetIdentity& root) {
    add_planned_package_target(
        plan, info.Name, package_base_name(info), roles, root);
}

PackageRelationRootAttribution relation_root_attribution(
    const RootTargetIdentity& root) {
    return PackageRelationRootAttribution{
        root.invocation_index, root.requested_name};
}

void add_planned_relation_observation(
    BuildPlan& plan,
    PlannedPackageRelationObservation observation,
    const RootTargetIdentity& root) {
    if(observation.package.role !=
       PackageRelationObservationRole::PlannedTarget) {
        throw std::logic_error(
            "BuildPlan relation observation is not a planned target.");
    }
    add_package_relation_root_attribution(
        observation.package, relation_root_attribution(root));

    const auto same_identity = [&observation](
                                   const PlannedPackageRelationObservation&
                                       existing) {
        return existing.package.package_name ==
                   observation.package.package_name &&
               existing.package.source == observation.package.source;
    };
    auto existing = std::find_if(
        plan.planned_relation_observations.begin(),
        plan.planned_relation_observations.end(), same_identity);
    if(existing == plan.planned_relation_observations.end()) {
        plan.planned_relation_observations.push_back(std::move(observation));
        return;
    }
    // POLICY(#353): Existing BuildPlan metadata uses the first package
    // snapshot as its invocation authority. Observation collection must not
    // add a new metadata-change guard before Slice 4 assessment.
    for(auto& observed_root : observation.package.roots) {
        add_package_relation_root_attribution(
            existing->package, std::move(observed_root));
    }
}

void add_relation_root_to_package(
    BuildPlan& plan, const std::string& package_name,
    const RootTargetIdentity& root) {
    for(auto& observation : plan.planned_relation_observations) {
        if(observation.package.package_name != package_name) continue;
        add_package_relation_root_attribution(
            observation.package, relation_root_attribution(root));
    }
}

std::vector<TypedPackageDependency> typed_dependencies_for_specification(
    const std::vector<TypedPackageDependency>& dependencies,
    const std::string& specification) {
    std::vector<TypedPackageDependency> matches;
    for(const auto& dependency : dependencies) {
        if(trim(dependency.specification) == specification) matches.push_back(dependency);
    }
    return matches;
}

std::vector<PackageRole> package_roles_for_dependencies(
    const std::vector<TypedPackageDependency>& dependencies) {
    std::vector<PackageRole> roles;
    for(const auto& dependency : dependencies)
        add_package_role(roles, dependency.role);
    return roles;
}

void add_build_plan_dependency_edges(
    BuildPlan& plan, const BuildPlanDependencyEdge& classified_edge,
    const std::vector<TypedPackageDependency>& dependencies) {
    for(const auto& dependency : dependencies) {
        BuildPlanDependencyEdge edge = classified_edge;
        edge.dependency_spec = dependency.specification;
        edge.role = dependency.role;
        edge.requirement = dependency.requirement;
        plan.dependency_edges.push_back(std::move(edge));
    }
}

std::optional<std::string> resolved_aur_dependency_name(
    const BuildPlanDependencyEdge& edge) {
    if(edge.kind == DependencyKind::Aur) return edge.resolved_package_name;
    if(edge.kind == DependencyKind::Provided && edge.resolved_provider.has_value() &&
       std::holds_alternative<AurProviderOrigin>(
           edge.resolved_provider->origin)) {
        return edge.resolved_provider->package_name;
    }
    return std::nullopt;
}

void propagate_root_identities(BuildPlan& plan) {
    // WHY(#218/#268): package visitedはmetadata traversalの重複を止めるためinvocation-wideで共有する。
    // 後からrootになったvisited nodeの既存subtreeへは、queryを増やさずedge上でidentityを伝播する。
    for(const auto& root : plan.root_targets) {
        std::vector<std::string> pending = {root.requested_name};
        std::set<std::string> visited_package_names;

        while(!pending.empty()) {
            std::string package_name = std::move(pending.back());
            pending.pop_back();
            if(!visited_package_names.insert(package_name).second) continue;

            PlannedPackageTarget* target = find_package_target(plan, package_name);
            if(target == nullptr) continue;
            add_root_identity(target->roots, root);
            add_relation_root_to_package(plan, package_name, root);

            for(const auto& edge : plan.dependency_edges) {
                if(edge.parent_package_name != package_name) continue;
                if(edge.resolved_package_name.has_value()) {
                    add_relation_root_to_package(
                        plan, edge.resolved_package_name.value(), root);
                } else if(edge.resolved_provider.has_value()) {
                    add_relation_root_to_package(
                        plan, edge.resolved_provider->package_name, root);
                }
                std::optional<std::string> dependency_name = resolved_aur_dependency_name(edge);
                if(!dependency_name.has_value() ||
                   find_package_target(plan, dependency_name.value()) == nullptr) {
                    continue;
                }
                pending.push_back(dependency_name.value());
            }
        }
    }
}

void propagate_resolution_failure_root_identities(BuildPlan& plan) {
    for(auto& failure : plan.resolution_failures) {
        if(!failure.parent_package_name.has_value()) continue;

        for(const auto& target : plan.package_targets) {
            if(target.package_name != failure.parent_package_name.value()) continue;
            if(failure.parent_package_base.has_value() &&
               target.package_base != failure.parent_package_base.value()) {
                continue;
            }
            for(const auto& root : target.roots) {
                add_root_identity(failure.roots, root);
            }
        }
    }
}

void add_build_plan_split_package_target(
    BuildPlan& plan, const std::string& package_name,
    const std::string& package_base) {
    if(package_name == package_base) return;

    auto same_target = [&](const BuildPlanSplitPackageTarget& existing) {
        return existing.package_base == package_base &&
               existing.package_name == package_name;
    };
    if(std::find_if(plan.split_package_targets.begin(), plan.split_package_targets.end(), same_target) !=
       plan.split_package_targets.end())
        return;

    plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{package_base, package_name});
}

void add_build_plan_split_package_target(
    BuildPlan& plan, const AurPackageInfo& info) {
    add_build_plan_split_package_target(
        plan, info.Name, package_base_name(info));
}

void add_build_plan_metadata_risk(
    BuildPlan& plan, const std::string& package_name,
    const std::string& package_base,
    const std::vector<std::string>& conflicts,
    const std::vector<std::string>& replaces) {
    if(conflicts.empty() && replaces.empty()) return;

    auto same_package = [&](const BuildPlanMetadataRisk& existing) {
        return existing.package_name == package_name &&
               existing.package_base == package_base;
    };
    if(std::find_if(plan.metadata_risks.begin(), plan.metadata_risks.end(), same_package) !=
       plan.metadata_risks.end())
        return;

    plan.metadata_risks.push_back(
        BuildPlanMetadataRisk{
            package_name, package_base, conflicts, replaces});
}

void add_build_plan_metadata_risk(BuildPlan& plan, const AurPackageInfo& info) {
    add_build_plan_metadata_risk(
        plan, info.Name, package_base_name(info), info.Conflicts,
        info.Replaces);
}

void add_build_plan_entry(
    BuildPlan& plan, const std::string& package_name,
    const std::string& package_base) {
    auto same_base = [&package_base](const BuildPlanEntry& existing) { return existing.package_base == package_base; };
    auto it = std::find_if(plan.order.begin(), plan.order.end(), same_base);
    if(it == plan.order.end()) {
        plan.order.push_back(BuildPlanEntry{package_base, {package_name}});
        return;
    }
    add_unique_value(it->package_names, package_name);
}

void add_build_plan_entry(BuildPlan& plan, const AurPackageInfo& info) {
    add_build_plan_entry(
        plan, info.Name, package_base_name(info));
}

const BuildPlanEntry* find_build_plan_entry(
    const std::vector<BuildPlanEntry>& entries,
    const std::string& package_base) {
    auto same_base = [&package_base](const BuildPlanEntry& entry) {
        return entry.package_base == package_base;
    };
    auto it = std::find_if(entries.begin(), entries.end(), same_base);
    return it == entries.end() ? nullptr : &(*it);
}

std::optional<std::string> resolved_aur_dependency_package_base(
    const BuildPlan& plan, const BuildPlanDependencyEdge& edge) {
    if(edge.kind == DependencyKind::Aur &&
       edge.resolved_package_base.has_value() &&
       !edge.resolved_package_base->empty()) {
        return edge.resolved_package_base;
    }

    std::optional<std::string> package_name =
        resolved_aur_dependency_name(edge);
    if(!package_name.has_value()) return std::nullopt;

    const PlannedPackageTarget* target =
        find_package_target(plan, package_name.value());
    if(target == nullptr) return std::nullopt;
    return target->package_base;
}

void append_build_plan_entry_postorder(
    const std::string& package_base, BuildPlan& plan,
    const std::vector<BuildPlanEntry>& aggregated_entries,
    std::set<std::string>& ordered_package_bases,
    std::set<std::string>& visiting_package_bases,
    std::vector<BuildPlanEntry>& ordered_entries) {
    if(ordered_package_bases.count(package_base) > 0) return;
    if(!visiting_package_bases.insert(package_base).second) {
        add_unique_value(plan.cycles, package_base);
        return;
    }

    for(const auto& edge : plan.dependency_edges) {
        if(edge.parent_package_base != package_base) continue;
        std::optional<std::string> dependency_package_base =
            resolved_aur_dependency_package_base(plan, edge);
        if(!dependency_package_base.has_value() ||
           dependency_package_base.value() == package_base ||
           find_build_plan_entry(
               aggregated_entries, dependency_package_base.value()) == nullptr) {
            continue;
        }
        append_build_plan_entry_postorder(
            dependency_package_base.value(), plan, aggregated_entries,
            ordered_package_bases, visiting_package_bases, ordered_entries);
    }

    visiting_package_bases.erase(package_base);
    if(!ordered_package_bases.insert(package_base).second) return;

    const BuildPlanEntry* entry =
        find_build_plan_entry(aggregated_entries, package_base);
    if(entry == nullptr) {
        throw std::logic_error(localization::format_translated_message(
            "{} aggregation is missing during build plan ordering: {}",
            "PackageBase", package_base));
    }
    ordered_entries.push_back(*entry);
}

void order_build_plan_entries(BuildPlan& plan) {
    std::vector<BuildPlanEntry> aggregated_entries = std::move(plan.order);
    std::vector<BuildPlanEntry> ordered_entries;
    ordered_entries.reserve(aggregated_entries.size());
    std::set<std::string> ordered_package_bases;
    std::set<std::string> visiting_package_bases;

    // POLICY(#268): first-seen PackageBase/edge orderを保ったpost-orderで、
    // 全required childのdependencyより後に各execution unitを一度だけ置く。
    for(const auto& entry : aggregated_entries) {
        append_build_plan_entry_postorder(
            entry.package_base, plan, aggregated_entries,
            ordered_package_bases, visiting_package_bases, ordered_entries);
    }

    plan.order = std::move(ordered_entries);
}

std::string resolved_candidate_constraint_key(
    const ResolvedDependencyCandidate& candidate) {
    return std::visit(
        [](const auto& value) -> std::string {
            using Candidate = std::decay_t<decltype(value)>;
            if constexpr(std::is_same_v<Candidate,
                                        InstalledExactPackage>) {
                return "installed/" + value.package_name;
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    RepositoryExactPackage>) {
                return "repository/" + value.repository.repository_name +
                       "/" + value.package_name;
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    AurResolvedDependencyCandidate>) {
                return "aur/" + value.package_base + "/" +
                       value.package_name;
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    LocalResolvedDependencyCandidate>) {
                return "local/" + value.package_base + "/" +
                       value.package_name;
            } else {
                return "provider/" +
                       provider_package_identity_display(value.provider) +
                       "/" +
                       value.provider.provided_dependency_name;
            }
        },
        candidate);
}

void project_build_plan_constraint_conflicts(BuildPlan& plan) {
    std::map<std::string, std::vector<std::size_t>> edge_indices;
    for(std::size_t index = 0; index < plan.dependency_edges.size(); ++index) {
        const BuildPlanDependencyEdge& edge = plan.dependency_edges[index];
        if(!edge.requirement.has_value() ||
           !edge.resolved_candidate.has_value() ||
           std::get_if<ConsumerDependencyRequirement>(
               &edge.requirement.value()) == nullptr) {
            continue;
        }
        const auto& consumer = std::get<ConsumerDependencyRequirement>(
            edge.requirement.value());
        edge_indices[resolved_candidate_constraint_key(
                         edge.resolved_candidate.value()) +
                     "/requirement/" + consumer.package_name()]
            .push_back(index);
    }

    // Ambiguous candidates are not selected yet, but choice reuse is keyed by
    // the canonical dependency identity. Project every offered source-aware
    // identity so contradictory requirements fail before the first prompt.
    for(std::size_t index = 0; index < plan.dependency_edges.size(); ++index) {
        const BuildPlanDependencyEdge& edge = plan.dependency_edges[index];
        if(edge.kind != DependencyKind::AmbiguousProvider ||
           !edge.requirement.has_value()) {
            continue;
        }
        const auto* consumer =
            std::get_if<ConsumerDependencyRequirement>(
                &edge.requirement.value());
        if(consumer == nullptr) continue;
        const auto candidate_set = std::find_if(
            plan.ambiguous_providers.begin(),
            plan.ambiguous_providers.end(),
            [&edge](const AmbiguousProvidedDependency& ambiguous) {
                return ambiguous.dependency == edge.dependency_spec;
            });
        if(candidate_set == plan.ambiguous_providers.end()) continue;
        for(const auto& provider : candidate_set->candidates) {
            edge_indices["provider/" +
                         provider_package_identity_display(provider) + "/" +
                         provider.provided_dependency_name + "/requirement/" +
                         consumer->package_name()]
                .push_back(index);
        }
    }

    for(const auto& [identity, indices] : edge_indices) {
        static_cast<void>(identity);
        std::vector<ConsumerDependencyRequirement> requirements;
        requirements.reserve(indices.size());
        for(const std::size_t index : indices) {
            requirements.push_back(
                std::get<ConsumerDependencyRequirement>(
                    plan.dependency_edges[index]
                        .requirement.value()));
        }
        const std::optional<ConstraintEvaluation> conflict =
            project_conflicting_constraint_invocation(requirements);
        if(!conflict.has_value()) continue;
        for(const std::size_t index : indices) {
            plan.dependency_edges[index].constraint_evaluation =
                conflict.value();
        }
    }
}

void add_build_plan_provided_dependency(
    BuildPlan& plan, const std::string& dependency,
    const ProvidedDependency& provider,
    ProviderResolutionKind resolution) {
    std::string trimmed = trim(dependency);
    if(trimmed.empty()) return;

    auto same_dependency = [&](const BuildPlanProvidedDependency& existing) {
        return existing.dependency == trimmed &&
               same_provider_identity(existing.provider, provider);
    };
    auto existing = std::find_if(
        plan.provided.begin(), plan.provided.end(), same_dependency);
    if(existing != plan.provided.end()) {
        if(existing->resolution != resolution) {
            existing->resolution = ProviderResolutionKind::UserSelected;
        }
        return;
    }
    plan.provided.push_back(
        BuildPlanProvidedDependency{trimmed, provider, resolution});
}

void add_build_plan_ambiguous_provider(
    BuildPlan& plan, const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates) {
    add_ambiguous_provider_dependency(plan.ambiguous_providers, dependency, candidates);
}

void add_incomplete_provider_candidate_set(
    BuildPlan& plan, const std::string& dependency,
    const ProviderCandidateDiscovery& discovery) {
    if(discovery.is_complete ||
       !discovery.incomplete_reason.has_value()) {
        return;
    }
    auto existing = std::find_if(
        plan.incomplete_provider_candidate_sets.begin(),
        plan.incomplete_provider_candidate_sets.end(),
        [&dependency](const IncompleteProviderCandidateSet& candidate_set) {
            return candidate_set.dependency == dependency;
        });
    if(existing == plan.incomplete_provider_candidate_sets.end()) {
        plan.incomplete_provider_candidate_sets.push_back(
            IncompleteProviderCandidateSet{
                dependency,
                discovery.candidates,
                discovery.incomplete_reason.value()});
        return;
    }
    for(const auto& candidate : discovery.candidates) {
        add_provider_candidate(existing->observed_candidates, candidate);
    }
}

void collect_aur_build_plan(
    const std::string& package_name, BuildPlan& plan,
    std::set<std::string>& visited_package_names,
    std::set<std::string>& visiting_package_names,
    const std::vector<PackageRole>& roles,
    const RootTargetIdentity& root, int depth, int max_depth,
    bool traverse_aur_providers, BuildPlanResolutionMode resolution_mode,
    const ProviderSelectionCallback& select_provider,
    const std::set<std::string>& local_package_bases,
    const std::set<std::string>& local_package_names,
    std::vector<dependency_plan_projection_support::
                    RemoteProviderIdentityConflict>*
        identity_conflicts,
    const dependency_plan_projection_support::LocalDependencyResolver&
        resolve_local_dependency,
    const std::optional<std::string>& parent_package_name,
    const std::optional<std::string>& parent_package_base,
    const std::optional<std::string>& dependency_specification,
    const std::optional<ProvidedDependency>& expected_selected_provider,
    std::optional<AurPackageInfo> preloaded_package_info);

void resolve_build_plan_dependency(
    const std::string& parent_package_name,
    const std::string& parent_package_base,
    const std::string& dependency,
    const std::vector<TypedPackageDependency>& matching_dependencies,
    BuildPlan& plan,
    std::set<std::string>& visited_package_names,
    std::set<std::string>& visiting_package_names,
    const RootTargetIdentity& root, int depth, int max_depth,
    bool traverse_aur_providers, BuildPlanResolutionMode resolution_mode,
    const ProviderSelectionCallback& select_provider,
    const std::set<std::string>& local_package_bases,
    const std::set<std::string>& local_package_names,
    std::vector<dependency_plan_projection_support::
                    RemoteProviderIdentityConflict>*
        identity_conflicts,
    const dependency_plan_projection_support::LocalDependencyResolver&
        resolve_local_dependency) {
    if(matching_dependencies.empty()) {
        throw std::logic_error(localization::format_translated_message(
            "Build dependency is missing its package role: {}",
            dependency));
    }

    if(resolve_local_dependency) {
        const dependency_plan_projection_support::DependencyDecision decision =
            resolve_local_dependency(
                dependency_plan_projection_support::DependencyContext{
                    parent_package_name,
                    parent_package_base,
                    matching_dependencies,
                    root});
        if(decision.handled_locally) {
            if(decision.cycle_package_base.has_value()) {
                add_unique_value(
                    plan.cycles, decision.cycle_package_base.value());
            }
            if(decision.unresolved_reason.has_value()) {
                add_unique_value(
                    plan.unresolved, decision.unresolved_reason.value());
            }
            return;
        }
    }

    const std::vector<PackageRole> dependency_roles =
        package_roles_for_dependencies(matching_dependencies);
    BuildPlanDependencyEdge edge{
        parent_package_name,
        parent_package_base,
        matching_dependencies.front().specification,
        matching_dependencies.front().role,
        DependencyKind::Unknown,
        std::nullopt,
        std::nullopt,
        std::nullopt};

    if(!matching_dependencies.front().requirement.has_value()) {
        edge.constraint_evaluation = ConstraintEvaluation::invalid(
            ConstraintInvalidReason::MalformedRequirement);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        require_constructible_build_plan_constraints(plan);
        return;
    }
    const DependencyRequirement& requirement =
        matching_dependencies.front().requirement.value();
    const auto* consumer =
        std::get_if<ConsumerDependencyRequirement>(&requirement);
    if(consumer == nullptr) {
        edge.constraint_evaluation = ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::RelationKindNotComparable);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        return;
    }
    const std::string& dep_name = consumer->package_name();

    BuildPlanResolutionFailureContext dependency_failure_context{
        plan, root, parent_package_name, parent_package_base, dependency};
    BuildPlanResolutionFailureContext* dependency_failure_sink =
        resolution_mode == BuildPlanResolutionMode::CaptureOrdinaryFailures
            ? &dependency_failure_context
            : nullptr;
    std::optional<RepositoryPackagePresent> present_repository_package;
    const RepositoryPackageQueryStatus repository_status =
        query_repository_package(
            dep_name, dependency_failure_sink,
            &present_repository_package,
            &plan);
    if(repository_status == RepositoryPackageQueryStatus::Present) {
        if(!present_repository_package.has_value()) {
            edge.constraint_evaluation = ConstraintEvaluation::invalid(
                ConstraintInvalidReason::InternalInvariantViolation);
            add_build_plan_dependency_edges(plan, edge, matching_dependencies);
            require_constructible_build_plan_constraints(plan);
            return;
        }
        RepositoryExactPackage candidate = repository_candidate(
            present_repository_package.value(), dep_name);
        add_planned_relation_observation(
            plan,
            project_repository_planned_relation_observation(
                candidate, {}),
            root);
        edge.kind = DependencyKind::Repo;
        edge.resolved_package_name = candidate.package_name;
        edge.resolved_candidate = candidate;
        edge.constraint_evaluation = evaluate_dependency_requirement(
            requirement, candidate.package_version);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        return;
    }
    if(repository_status == RepositoryPackageQueryStatus::Unavailable) {
        add_unique_value(plan.unresolved, dependency);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        return;
    }

    std::optional<AurPackageInfo> dependency_info;
    bool dependency_metadata_unavailable = false;
    try {
        dependency_info = query_aur_package_info(
            dep_name, resolution_mode,
            static_cast<bool>(select_provider));
    } catch(const AurRpcResponseError&) {
        throw;
    } catch(const AurConstraintMetadataProjectionError&) {
        throw;
    } catch(const std::exception& error) {
        if(resolution_mode == BuildPlanResolutionMode::Legacy &&
           select_provider) {
            throw;
        }
        add_resolution_failure(
            dependency_failure_sink,
            BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
            dep_name, error.what());
        dependency_metadata_unavailable = dependency_failure_sink != nullptr;
        Logger::warn(localization::format_translated_message(
            "Failed to check {} dependency {}: {}",
            "AUR", dep_name, error.what()));
    }

    if(dependency_metadata_unavailable) {
        add_unique_value(plan.unresolved, dependency);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        return;
    }

    if(dependency_info.has_value()) {
        AurResolvedDependencyCandidate candidate =
            aur_candidate(dependency_info.value());
        edge.kind = DependencyKind::Aur;
        edge.resolved_package_name = candidate.package_name;
        edge.resolved_package_base = candidate.package_base;
        edge.resolved_candidate = candidate;
        edge.constraint_evaluation = evaluate_dependency_requirement(
            requirement, candidate.package_version);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        if(local_package_bases.count(edge.resolved_package_base.value()) > 0) {
            add_unique_value(plan.cycles, edge.resolved_package_base.value());
            return;
        }
        collect_aur_build_plan(
            dep_name, plan, visited_package_names,
            visiting_package_names, dependency_roles, root,
            depth + 1, max_depth, traverse_aur_providers,
            resolution_mode, select_provider, local_package_bases,
            local_package_names, identity_conflicts,
            resolve_local_dependency, parent_package_name,
            parent_package_base, dependency, std::nullopt,
            std::move(dependency_info));
        return;
    }

    ProviderCandidateDiscovery provider_discovery =
        find_dependency_providers(
            dep_name, &dependency_failure_context,
            true, &plan);
    if(!provider_discovery.is_complete) {
        add_incomplete_provider_candidate_set(
            plan, dependency, provider_discovery);
        edge.kind = DependencyKind::Unknown;
        edge.constraint_evaluation = ConstraintEvaluation::unknown(
            provider_discovery.incomplete_reason.value_or(
                ObservedVersionUnknownReason::MetadataQueryFailure));
        add_unique_value(plan.unresolved, dependency);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        return;
    }
    const std::vector<ProvidedDependency>& providers =
        provider_discovery.candidates;
    for(const auto& provider : providers) {
        const ConstraintEvaluation evaluation =
            evaluate_provider_requirement(requirement, provider);
        const ConstraintSatisfaction satisfaction =
            evaluation.satisfaction();
        if(satisfaction == ConstraintSatisfaction::Invalid ||
           satisfaction == ConstraintSatisfaction::Conflicting) {
            throw std::runtime_error(
                localization::format_translated_message(
                    "Cannot select a provider for {}; candidate {} is {}: {}.",
                    dependency,
                    provider_package_identity_display(provider),
                    constraint_satisfaction_display(satisfaction),
                    constraint_evaluation_reason_display(evaluation)));
        }
        if(satisfaction == ConstraintSatisfaction::Unsatisfied ||
           satisfaction == ConstraintSatisfaction::Unknown) {
            Logger::warn(localization::format_translated_message(
                "Provider candidate {} for {} is {}: {}. Candidate order and explicit selection are unchanged.",
                provider_package_identity_display(provider), dependency,
                constraint_satisfaction_display(satisfaction),
                constraint_evaluation_reason_display(evaluation)));
        }
    }
    std::optional<ProvidedDependency> resolved_provider =
        select_provider_candidate(
            dependency, providers, select_provider);
    const ProviderResolutionKind provider_resolution =
        resolved_provider.has_value()
            ? ProviderResolutionKind::UserSelected
            : ProviderResolutionKind::Unique;
    if(!resolved_provider.has_value() && providers.size() == 1) {
        resolved_provider = providers.front();
    }

    if(resolved_provider.has_value()) {
        ProvidedDependency provider = resolved_provider.value();
        ConstraintEvaluation provider_evaluation =
            evaluate_provider_requirement(requirement, provider);
        std::optional<AurPackageInfo> refreshed_provider_info;
        if(std::holds_alternative<AurProviderOrigin>(provider.origin)) {
            AurProviderDependencyProjection selected_projection{
                *consumer, provider, provider_evaluation};
            std::optional<AurPackageInfo> current_info;
            try {
                current_info = AurClient::info_strict(provider.package_name);
            } catch(const AurRpcResponseError&) {
                throw;
            } catch(const std::exception& error) {
                add_resolution_failure(
                    &dependency_failure_context,
                    BuildPlanResolutionFailureKind::
                        ProviderCandidateMetadataUnavailable,
                    provider.package_name,
                    error.what());
                Logger::warn(localization::format_translated_message(
                    "Failed to refresh {} provider {}: {}",
                    "AUR", provider.package_name, error.what()));
            }

            AurProviderCandidateMetadata current_metadata =
                AurProviderMetadataUnavailable{
                    provider.package_name,
                    provider.package_base,
                    ObservedVersionUnknownReason::
                        MetadataQueryFailure};
            if(current_info.has_value()) {
                current_info = require_typed_aur_package_info(
                    std::move(current_info.value()));
                current_metadata =
                    current_info->constraint_metadata.value();
            } else {
                const std::string diagnostic =
                    localization::format_translated_message(
                        "{} provider candidate metadata was not returned.",
                        "AUR");
                add_resolution_failure(
                    &dependency_failure_context,
                    BuildPlanResolutionFailureKind::
                        ProviderCandidateMetadataUnavailable,
                    provider.package_name,
                    diagnostic);
            }

            AurProviderDependencyProjectionResult refreshed =
                refresh_aur_provider_dependency(
                    selected_projection,
                    current_metadata);
            if(const auto* projection =
                   std::get_if<AurProviderDependencyProjection>(
                       &refreshed);
               projection != nullptr) {
                provider = projection->provider;
                provider_evaluation = projection->evaluation;
                refreshed_provider_info = std::move(current_info);
            } else if(const auto* unknown =
                          std::get_if<AurProviderDependencyUnknown>(
                              &refreshed);
                      unknown != nullptr) {
                edge.kind = DependencyKind::Unknown;
                edge.constraint_evaluation =
                    ConstraintEvaluation::unknown(unknown->reason);
                add_unique_value(plan.unresolved, dependency);
                add_build_plan_dependency_edges(
                    plan, edge, matching_dependencies);
                return;
            } else {
                throw std::runtime_error(
                    selected_aur_provider_revalidation_failure_diagnostic(
                        provider));
            }
        }
        const bool returns_local_package_base =
            std::holds_alternative<AurProviderOrigin>(provider.origin) &&
            local_package_bases.count(provider.package_base) > 0;
        if(local_package_names.count(provider.package_name) > 0 &&
           !returns_local_package_base) {
            if(identity_conflicts != nullptr) {
                identity_conflicts->push_back(
                    dependency_plan_projection_support::
                        RemoteProviderIdentityConflict{
                            parent_package_name,
                            dependency,
                            provider});
            }
            add_unique_value(
                plan.unresolved,
                dependency +
                    " (remote provider conflicts with local package identity)");
            add_build_plan_dependency_edges(
                plan, edge, matching_dependencies);
            return;
        }
        edge.kind = DependencyKind::Provided;
        edge.resolved_provider = provider;
        edge.provider_resolution = provider_resolution;
        edge.resolved_candidate = ProviderResolvedDependencyCandidate{
            provider, provider_observed_version(provider)};
        edge.constraint_evaluation = provider_evaluation;
        if(const RepositoryExactPackage* repository_observation =
               repository_provider_observation(
                   provider_discovery, provider);
           repository_observation != nullptr) {
            add_planned_relation_observation(
                plan,
                project_repository_planned_relation_observation(
                    *repository_observation, {}),
                root);
        }
        add_build_plan_provided_dependency(
            plan, dependency, provider, provider_resolution);
        add_build_plan_dependency_edges(plan, edge, matching_dependencies);
        if((traverse_aur_providers ||
            provider_resolution == ProviderResolutionKind::UserSelected) &&
           std::holds_alternative<AurProviderOrigin>(provider.origin)) {
            if(returns_local_package_base) {
                add_unique_value(plan.cycles, provider.package_base);
                return;
            }
            const std::optional<ProvidedDependency>
                provider_revalidation_contract = provider;
            collect_aur_build_plan(
                provider.package_name, plan, visited_package_names,
                visiting_package_names, dependency_roles, root,
                depth + 1, max_depth, traverse_aur_providers,
                resolution_mode, select_provider, local_package_bases,
                local_package_names, identity_conflicts,
                resolve_local_dependency, parent_package_name,
                parent_package_base, dependency,
                provider_revalidation_contract,
                std::move(refreshed_provider_info));
        }
        return;
    }

    if(providers.size() > 1) {
        edge.kind = DependencyKind::AmbiguousProvider;
        add_build_plan_ambiguous_provider(plan, dependency, providers);
    } else {
        InstalledExactPackageObservationResult installed_result =
            query_installed_exact_package_strict(dep_name);
        if(const auto* installed =
               std::get_if<InstalledExactPackage>(&installed_result);
           installed != nullptr) {
            edge.kind = DependencyKind::Installed;
            edge.resolved_package_name = installed->package_name;
            edge.resolved_candidate = *installed;
            edge.constraint_evaluation = evaluate_dependency_requirement(
                requirement, installed->observed_version);
        } else if(std::holds_alternative<InstalledExactPackageAbsent>(
                      installed_result)) {
            add_unique_value(plan.unresolved, dependency);
        } else {
            const InstalledExactPackageQueryFailure& failure =
                std::get<InstalledExactPackageQueryFailure>(
                    installed_result);
            add_resolution_failure(
                &dependency_failure_context,
                BuildPlanResolutionFailureKind::
                    InstalledPackageMetadataUnavailable,
                dep_name,
                failure.failure.diagnostic);
            edge.constraint_evaluation = ConstraintEvaluation::unknown(
                ObservedVersionUnknownReason::MetadataQueryFailure);
            add_unique_value(plan.unresolved, dependency);
        }
    }
    add_build_plan_dependency_edges(plan, edge, matching_dependencies);
}

void collect_aur_build_plan(
    const std::string& package_name, BuildPlan& plan,
    std::set<std::string>& visited_package_names,
    std::set<std::string>& visiting_package_names,
    const std::vector<PackageRole>& roles,
    const RootTargetIdentity& root, int depth, int max_depth,
    bool traverse_aur_providers, BuildPlanResolutionMode resolution_mode,
    const ProviderSelectionCallback& select_provider,
    const std::set<std::string>& local_package_bases,
    const std::set<std::string>& local_package_names,
    std::vector<dependency_plan_projection_support::
                    RemoteProviderIdentityConflict>*
        identity_conflicts,
    const dependency_plan_projection_support::LocalDependencyResolver&
        resolve_local_dependency,
    const std::optional<std::string>& parent_package_name,
    const std::optional<std::string>& parent_package_base,
    const std::optional<std::string>& dependency_specification,
    const std::optional<ProvidedDependency>& expected_selected_provider,
    std::optional<AurPackageInfo> preloaded_package_info) {
    if(depth > max_depth) {
        add_unique_value(
            plan.unresolved,
            localization::format_translated_message(
                "{} (max depth reached)", package_name));
        return;
    }

    std::optional<AurPackageInfo> info;
    BuildPlanResolutionFailureContext package_failure_context{
        plan, root, parent_package_name, parent_package_base,
        dependency_specification};
    BuildPlanResolutionFailureContext* failure_context =
        resolution_mode == BuildPlanResolutionMode::CaptureOrdinaryFailures
            ? &package_failure_context
            : nullptr;
    try {
        if(preloaded_package_info.has_value()) {
            info = std::move(preloaded_package_info);
        } else {
            info = query_aur_package_info(
                package_name, resolution_mode,
                static_cast<bool>(select_provider));
        }
    } catch(const AurRpcResponseError&) {
        throw;
    } catch(const AurConstraintMetadataProjectionError&) {
        throw;
    } catch(const std::exception& e) {
        if(resolution_mode == BuildPlanResolutionMode::Legacy &&
           select_provider) {
            throw;
        }
        add_resolution_failure(
            failure_context,
            BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
            package_name, e.what());
        Logger::warn(localization::format_translated_message(
            "Failed to fetch {} info for {}: {}",
            "AUR", package_name, e.what()));
        add_unique_value(plan.unresolved, package_name);
        return;
    }

    if(!info.has_value()) {
        add_unique_value(plan.unresolved, package_name);
        return;
    }

    if(expected_selected_provider.has_value() &&
       !matches_selected_aur_provider_contract(
           info.value(), expected_selected_provider.value())) {
        std::string diagnostic =
            selected_aur_provider_revalidation_failure_diagnostic(
                expected_selected_provider.value());
        if(failure_context == nullptr) {
            throw std::runtime_error(diagnostic);
        }
        add_resolution_failure(
            failure_context,
            BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable,
            package_name, diagnostic);
        Logger::warn(diagnostic);
        add_unique_value(plan.unresolved, package_name);
        return;
    }

    // POLICY(#150): visited packageで再帰を打ち切る場合も、package単位のraw metadataは先に保持する。
    add_build_plan_metadata_risk(plan, info.value());
    // POLICY(#218/#268): role/rootとPackageBase child identityはpackage visitedより先にmergeする。
    add_planned_package_target(plan, info.value(), roles, root);
    add_planned_relation_observation(
        plan,
        project_aur_relation_observation(
            info->constraint_metadata.value(), {}),
        root);
    add_build_plan_entry(plan, info.value());

    std::string build_unit = package_base_name(info.value());
    std::string package_identity = info->Name;
    if(visited_package_names.count(package_identity) > 0) return;
    if(visiting_package_names.count(package_identity) > 0) {
        add_unique_value(plan.cycles, build_unit);
        return;
    }

    visiting_package_names.insert(package_identity);

    const std::vector<TypedPackageDependency> typed_dependencies =
        collect_typed_build_dependencies(info.value());
    for(const auto& dependency : collect_build_dependencies(info.value())) {
        resolve_build_plan_dependency(
            info->Name, build_unit, dependency,
            typed_dependencies_for_specification(
                typed_dependencies, dependency),
            plan, visited_package_names, visiting_package_names, root,
            depth, max_depth, traverse_aur_providers, resolution_mode,
            select_provider, local_package_bases, local_package_names,
            identity_conflicts,
            resolve_local_dependency);
    }

    visiting_package_names.erase(package_identity);
    visited_package_names.insert(package_identity);
    add_build_plan_split_package_target(plan, info.value());
}

BuildPlan resolve_build_plan_once(
    const std::vector<std::string>& targets,
    BuildPlanResolutionMode resolution_mode,
    const ProviderSelectionCallback& select_provider) {
    if(targets.empty()) {
        throw std::invalid_argument(localization::translate_message(
            "Build plan targets must not be empty."));
    }

    for(const auto& target : targets)
        require_valid_package_name(target);
    if(resolution_mode == BuildPlanResolutionMode::Legacy) {
        // POLICY: legacy resolverの事前存在確認と例外境界は通常-S consumerの契約。
        for(const auto& target : targets) {
            if(!query_aur_package_info(
                    target, resolution_mode,
                    static_cast<bool>(select_provider))
                    .has_value()) {
                throw std::runtime_error(
                    localization::format_translated_message(
                        "{} package not found: {}",
                        "AUR", target));
            }
        }
    }

    BuildPlan plan;
    std::set<std::string> visited_package_names;
    std::set<std::string> visiting_package_names;
    const std::vector<PackageRole> root_roles = {PackageRole::Root};
    for(std::size_t i = 0; i < targets.size(); ++i) {
        RootTargetIdentity root{i, targets[i]};
        plan.root_targets.push_back(root);
        collect_aur_build_plan(
            targets[i], plan, visited_package_names,
            visiting_package_names, root_roles, root, 0,
            MAX_RECURSIVE_DEP_DEPTH, true, resolution_mode,
            select_provider, {}, {}, nullptr, {},
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt);
    }
    order_build_plan_entries(plan);
    propagate_root_identities(plan);
    propagate_resolution_failure_root_identities(plan);
    project_build_plan_constraint_conflicts(plan);
    require_compatible_selected_provider_package_identities(plan);
    require_constructible_build_plan_constraints(plan);
    return plan;
}

struct ProviderSelectionRequest {
    std::string dependency;
    std::vector<ProvidedDependency> candidates;
};

std::string provider_selection_request_name(
    const ProviderSelectionRequest& request) {
    if(request.candidates.empty()) {
        throw std::logic_error(localization::translate_message(
            "Provider selection request has no candidates."));
    }
    const std::string& dependency_name =
        request.candidates.front().provided_dependency_name;
    if(dependency_name.empty() ||
       std::any_of(
           request.candidates.begin(), request.candidates.end(),
           [&dependency_name](const ProvidedDependency& candidate) {
               return candidate.provided_dependency_name !=
                      dependency_name;
           })) {
        throw std::logic_error(localization::translate_message(
            "Provider selection candidates have inconsistent dependency identities."));
    }
    return dependency_name;
}

bool same_selected_aur_provider_refresh_identity(
    const ProvidedDependency& selected,
    const ProvidedDependency& current) {
    if(!same_provider_identity(selected, current) ||
       !std::holds_alternative<AurProviderOrigin>(selected.origin) ||
       !std::holds_alternative<AurProviderOrigin>(current.origin) ||
       !selected.constraint_metadata.has_value() ||
       !current.constraint_metadata.has_value()) {
        return false;
    }
    return selected.provided_dependency_name ==
               current.provided_dependency_name &&
           selected.provided_dependency_specification ==
               current.provided_dependency_specification &&
           selected.constraint_metadata->provided_capability ==
               current.constraint_metadata->provided_capability &&
           selected.constraint_metadata->provided_version ==
               current.constraint_metadata->provided_version;
}

bool same_selected_provider_revalidation_contract(
    const ProvidedDependency& selected,
    const ProvidedDependency& current) {
    if(!same_provider_identity(selected, current) ||
       selected.provided_dependency_name !=
           current.provided_dependency_name ||
       selected.provided_dependency_specification !=
           current.provided_dependency_specification ||
       !selected.constraint_metadata.has_value() ||
       !current.constraint_metadata.has_value()) {
        return false;
    }
    return selected.constraint_metadata->provided_capability ==
               current.constraint_metadata->provided_capability &&
           selected.constraint_metadata->provided_version ==
               current.constraint_metadata->provided_version;
}

const BuildPlan& provider_interaction_plan(const BuildPlan& plan) {
    return plan;
}

const BuildPlan& provider_interaction_plan(
    const dependency_plan_projection_support::ResolutionResult& result) {
    return result.plan;
}

template <typename ResolveOnce>
auto resolve_with_provider_interaction(
    const ResolveOnce& resolve_once,
    const ProviderSelectionCallback& select_provider) {
    if(!select_provider) {
        return resolve_once(ProviderSelectionCallback{});
    }

    std::vector<ProviderSelectionRequest> requests;
    ProviderSelectionCallback discover_candidates =
        [&requests](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        requests.push_back(ProviderSelectionRequest{dependency, candidates});
        return std::nullopt;
    };
    auto discovered = resolve_once(discover_candidates);

    if(!provider_interaction_plan(discovered)
            .incomplete_provider_candidate_sets.empty()) {
        return discovered;
    }

    std::vector<ProviderSelectionRequest> unique_requests;
    std::map<std::string, std::size_t> request_by_dependency;
    for(const auto& request : requests) {
        const std::string dependency_name =
            provider_selection_request_name(request);
        const auto [existing, inserted] =
            request_by_dependency.emplace(
                dependency_name, unique_requests.size());
        if(inserted) {
            unique_requests.push_back(request);
            continue;
        }
        if(unique_requests[existing->second].candidates !=
           request.candidates) {
            throw std::runtime_error(
                localization::format_translated_message(
                    "Provider candidates changed during invocation resolution: {}",
                    dependency_name));
        }
    }

    std::map<std::string, ProvidedDependency> selections;
    for(const auto& request : unique_requests) {
        const std::string dependency_name =
            provider_selection_request_name(request);
        std::optional<ProvidedDependency> selected =
            select_provider_candidate(
                request.dependency,
                request.candidates,
                select_provider);
        if(!selected.has_value()) continue;

        auto existing = selections.find(dependency_name);
        if(existing != selections.end() &&
           !same_provider_identity(existing->second, selected.value())) {
            throw std::runtime_error(localization::format_translated_message(
                "Previously selected provider is no longer a candidate for dependency: {}",
                dependency_name));
        }
        selections.insert_or_assign(
            dependency_name,
            selected.value());
    }
    if(selections.empty()) return discovered;

    ProviderSelectionCallback reuse_selection =
        [&selections](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        if(candidates.empty()) return std::nullopt;
        const ProviderSelectionRequest request{"", candidates};
        const std::string dependency_name =
            provider_selection_request_name(request);
        const auto selected = selections.find(dependency_name);
        if(selected == selections.end()) return std::nullopt;

        if(std::holds_alternative<AurProviderOrigin>(
               selected->second.origin)) {
            const auto current = std::find_if(
                candidates.begin(), candidates.end(),
                [&selected](const ProvidedDependency& candidate) {
                    return candidate.package_name ==
                           selected->second.package_name;
                });
            if(current == candidates.end() ||
               !same_selected_aur_provider_refresh_identity(
                   selected->second, *current) ||
               !same_selected_provider_revalidation_contract(
                   selected->second, *current)) {
                throw std::runtime_error(
                    selected_aur_provider_revalidation_failure_diagnostic(
                        selected->second));
            }
            return *current;
        }

        const auto current = std::find_if(
            candidates.begin(), candidates.end(),
            [&selected](const ProvidedDependency& candidate) {
                return same_provider_identity(
                    selected->second, candidate);
            });
        if(current == candidates.end()) {
            throw std::runtime_error(localization::format_translated_message(
                "Previously selected provider is no longer a candidate for dependency: {}",
                dependency_name));
        }
        if(!same_selected_provider_revalidation_contract(
               selected->second, *current)) {
            throw std::runtime_error(
                localization::format_translated_message(
                    "Provider candidate changed during dependency resolution: {}",
                    provider_package_identity_display(
                        selected->second)));
        }
        return *current;
    };
    return resolve_once(reuse_selection);
}

BuildPlan resolve_build_plan_with_interaction(
    const std::vector<std::string>& targets,
    BuildPlanResolutionMode resolution_mode,
    const ProviderSelectionCallback& select_provider) {
    const auto resolve_once = [&targets, resolution_mode](
                                  const ProviderSelectionCallback& selector) {
        return resolve_build_plan_once(targets, resolution_mode, selector);
    };
    BuildPlan plan =
        resolve_with_provider_interaction(resolve_once, select_provider);
    finalize_build_plan_relation_assessments(plan);
    return plan;
}

BuildPlan resolve_fetch_plan_once(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    if(targets.empty()) {
        throw std::invalid_argument(localization::translate_message(
            "Build plan targets must not be empty."));
    }
    for(const auto& target : targets) {
        require_valid_package_name(target);
        if(!query_aur_package_info(
                target,
                BuildPlanResolutionMode::Legacy,
                static_cast<bool>(select_provider))
                .has_value()) {
            throw std::runtime_error(localization::format_translated_message(
                "{} package not found: {}", "AUR", target));
        }
    }

    BuildPlan plan;
    std::set<std::string> visited_package_names;
    std::set<std::string> visiting_package_names;
    const std::vector<PackageRole> root_roles = {PackageRole::Root};
    for(std::size_t index = 0; index < targets.size(); ++index) {
        const RootTargetIdentity root{index, targets[index]};
        plan.root_targets.push_back(root);
        // Fetch enumerates retrieval targets but does not traverse an
        // auto-resolved AUR provider. A user-selected AUR provider remains an
        // explicit retrieval target.
        collect_aur_build_plan(
            targets[index], plan, visited_package_names,
            visiting_package_names, root_roles, root, 0,
            MAX_RECURSIVE_DEP_DEPTH, false,
            BuildPlanResolutionMode::Legacy,
            select_provider, {}, {}, nullptr, {},
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            std::nullopt);
    }
    order_build_plan_entries(plan);
    propagate_root_identities(plan);
    propagate_resolution_failure_root_identities(plan);
    project_build_plan_constraint_conflicts(plan);
    require_compatible_selected_provider_package_identities(plan);
    require_constructible_build_plan_constraints(plan);
    return plan;
}

BuildPlan resolve_fetch_plan_with_interaction(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    const auto resolve_once = [&targets](
                                  const ProviderSelectionCallback& selector) {
        return resolve_fetch_plan_once(targets, selector);
    };
    BuildPlan plan =
        resolve_with_provider_interaction(resolve_once, select_provider);
    finalize_build_plan_relation_assessments(plan);
    return plan;
}

} // namespace

namespace dependency_plan_projection_support {
namespace {

ResolutionResult resolve_once(
    const std::vector<RootPackage>& roots,
    const std::set<std::string>& local_package_bases,
    const LocalDependencyResolver& resolve_local_dependency,
    const ProviderSelectionCallback& select_provider) {
    if(roots.empty()) {
        throw std::invalid_argument(localization::translate_message(
            "Build plan targets must not be empty."));
    }

    for(const auto& root : roots) {
        require_valid_package_name(root.package_name);
        require_valid_package_name(root.package_base);
    }

    BuildPlan plan;
    std::set<std::string> visited_package_names;
    std::set<std::string> visiting_package_names;
    std::set<std::string> local_package_names;
    std::vector<RemoteProviderIdentityConflict> identity_conflicts;
    const std::vector<PackageRole> root_roles = {PackageRole::Root};
    for(const auto& root : roots) {
        local_package_names.insert(root.package_name);
    }

    for(std::size_t index = 0; index < roots.size(); ++index) {
        const RootPackage& root_package = roots[index];
        const RootTargetIdentity root{index, root_package.package_name};
        plan.root_targets.push_back(root);
        add_planned_package_target(
            plan, root_package.package_name, root_package.package_base,
            root_roles, root);
        add_build_plan_entry(
            plan, root_package.package_name, root_package.package_base);
        add_build_plan_split_package_target(
            plan, root_package.package_name, root_package.package_base);
        add_build_plan_metadata_risk(
            plan, root_package.package_name, root_package.package_base,
            root_package.conflicts, root_package.replaces);

        std::vector<std::string> specifications;
        for(const auto& dependency : root_package.dependencies) {
            add_unique_value(specifications, dependency.specification);
        }
        for(const auto& specification : specifications) {
            resolve_build_plan_dependency(
                root_package.package_name, root_package.package_base,
                specification,
                typed_dependencies_for_specification(
                    root_package.dependencies, specification),
                plan, visited_package_names, visiting_package_names, root,
                0, MAX_RECURSIVE_DEP_DEPTH, true,
                BuildPlanResolutionMode::CaptureOrdinaryFailures,
                select_provider,
                local_package_bases, local_package_names,
                &identity_conflicts, resolve_local_dependency);
        }
    }

    order_build_plan_entries(plan);
    propagate_root_identities(plan);
    propagate_resolution_failure_root_identities(plan);
    project_build_plan_constraint_conflicts(plan);
    require_compatible_selected_provider_package_identities(plan);
    require_constructible_build_plan_constraints(plan);
    return ResolutionResult{
        std::move(plan), std::move(identity_conflicts)};
}

} // namespace

ResolutionResult resolve(
    const std::vector<RootPackage>& roots,
    const std::set<std::string>& local_package_bases,
    const LocalDependencyResolver& resolve_local_dependency,
    const ProviderSelectionCallback& select_provider) {
    const auto project_once =
        [&roots, &local_package_bases, &resolve_local_dependency](
            const ProviderSelectionCallback& selector) {
            return resolve_once(
                roots, local_package_bases, resolve_local_dependency,
                selector);
        };
    return resolve_with_provider_interaction(
        project_once, select_provider);
}

} // namespace dependency_plan_projection_support

std::vector<BuildPlanMetadataRisk> collect_build_plan_metadata_risks(const AurPackageInfo& pkg) {
    BuildPlan plan;
    add_build_plan_metadata_risk(plan, pkg);
    return std::move(plan.metadata_risks);
}

BuildPlan resolve_build_plan(const std::string& target) {
    return resolve_build_plan(target, ProviderSelectionCallback{});
}

BuildPlan resolve_build_plan(
    const std::string& target,
    const ProviderSelectionCallback& select_provider) {
    return resolve_build_plan(
        std::vector<std::string>{target}, select_provider);
}

BuildPlan resolve_build_plan(const std::vector<std::string>& targets) {
    return resolve_build_plan(targets, ProviderSelectionCallback{});
}

BuildPlan resolve_build_plan(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    return resolve_build_plan_with_interaction(
        targets, BuildPlanResolutionMode::Legacy, select_provider);
}

BuildPlan resolve_build_plan_for_preflight(
    const std::vector<std::string>& targets) {
    return resolve_build_plan_for_preflight(
        targets, ProviderSelectionCallback{});
}

BuildPlan resolve_build_plan_for_preflight(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    return resolve_build_plan_with_interaction(
        targets, BuildPlanResolutionMode::CaptureOrdinaryFailures,
        select_provider);
}

BuildPlan resolve_fetch_plan(const std::string& target) {
    return resolve_fetch_plan(target, ProviderSelectionCallback{});
}

BuildPlan resolve_fetch_plan(
    const std::string& target,
    const ProviderSelectionCallback& select_provider) {
    return resolve_fetch_plan(
        std::vector<std::string>{target}, select_provider);
}

BuildPlan resolve_fetch_plan(const std::vector<std::string>& targets) {
    return resolve_fetch_plan(targets, ProviderSelectionCallback{});
}

BuildPlan resolve_fetch_plan(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider) {
    return resolve_fetch_plan_with_interaction(targets, select_provider);
}

void finalize_build_plan_constraints(BuildPlan& plan) {
    project_build_plan_constraint_conflicts(plan);
    require_constructible_build_plan_constraints(plan);
}
