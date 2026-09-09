#include "commands_inspect.hpp"

#include "app_config.hpp"
#include "application_identity.hpp"
#include "aur_rpc.hpp"
#include "aur_update_query.hpp"
#include "cache_authority.hpp"
#include "checkout_fetch.hpp"
#include "cli_authority.hpp"
#include "cli_routing.hpp"
#include "cli_runtime_contract.hpp"
#include "dependency_plan.hpp"
#include "dependency_provider.hpp"
#include "dependency_spec.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "package_metadata.hpp"
#include "package_relation_presentation.hpp"
#include "presentation_projection.hpp"
#include "pkgbuild_export.hpp"
#include "repository_query.hpp"
#include "runtime_diagnostic.hpp"
#include "trusted_cache.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// inspection command固有のpresentationとtarget orchestrationを所有する。
// POLICY: CLI parsing、Curl/Logger lifetime、-G/-Gp special routeはrunner側に残す。
namespace {

const std::string AUR_BASE_URL = "https://aur.archlinux.org/";

std::string deps_usage() {
    return localization::format_translated_message(
        // TRANSLATORS: The placeholders are the command and complete operation syntax.
        "Usage: {} {}", application_identity::COMMAND_NAME,
        cli_operation_syntax(cli_authority::OperationId::Deps));
}

std::string plan_usage() {
    return localization::format_translated_message(
        // TRANSLATORS: The placeholders are the command and complete operation syntax.
        "Usage: {} {}", application_identity::COMMAND_NAME,
        cli_operation_syntax(cli_authority::OperationId::Plan));
}

std::string fetch_usage() {
    return localization::format_translated_message(
        // TRANSLATORS: The placeholders are the command and complete operation syntax.
        "Usage: {} {}", application_identity::COMMAND_NAME,
        cli_operation_syntax(cli_authority::OperationId::Fetch));
}

using RepositoryPackageLookupIdentity =
    std::pair<std::optional<std::string>, std::string>;
using RepositoryPackageDisplayIdentity = std::pair<std::string, std::string>;

// repository metadataはplan graphの正本ではなく、1回のplan invocationだけで有効な
// read-only presentation enrichmentとして所有する。
struct RepositoryMetadataPresentationContext {
    bool configuration_attempted = false;
    std::optional<PacmanRepositoryConfiguration> configuration;
    bool session_open_attempted = false;
    std::optional<RepositoryPackageMetadataSession> session;
    std::optional<PackageMetadataFailure> unavailable_failure;
    std::map<RepositoryPackageLookupIdentity, RepositoryPackageQueryResult>
        query_results;
};

RepositoryPackageLookupIdentity repository_package_lookup_identity(
    const RepositoryPackageLookup& lookup) {
    return {lookup.exact_repository_name, lookup.package_name};
}

void add_repository_package_lookup(
    std::vector<RepositoryPackageLookup>& lookups,
    std::set<RepositoryPackageLookupIdentity>& seen_lookups,
    RepositoryPackageLookup lookup) {
    RepositoryPackageLookupIdentity identity =
        repository_package_lookup_identity(lookup);
    if(!seen_lookups.insert(identity).second) return;
    lookups.push_back(std::move(lookup));
}

std::vector<RepositoryPackageLookup> collect_repository_package_lookups(
    const BuildPlan& plan) {
    std::vector<RepositoryPackageLookup> lookups;
    std::set<RepositoryPackageLookupIdentity> seen_lookups;

    // POLICY(#125): BuildPlan::orderはAUR build unit。official package sizeの正本は
    // dependency edgeの解決結果だけとし、edge first-seen orderを保持する。
    for(const auto& edge : plan.dependency_edges) {
        if(edge.kind == DependencyKind::Repo &&
           edge.resolved_package_name.has_value()) {
            add_repository_package_lookup(
                lookups, seen_lookups,
                RepositoryPackageLookup{
                    edge.resolved_package_name.value(), std::nullopt});
            continue;
        }

        if(edge.kind != DependencyKind::Provided ||
           !edge.resolved_provider.has_value()) {
            continue;
        }

        const auto* repository = std::get_if<RepositoryProviderOrigin>(
            &edge.resolved_provider->origin);
        if(repository == nullptr) continue;

        // Configured membershipはpacman-confの正本をresolveした後で確認する。
        // 現行provider resolverが返し得るunconfigured/stale repositoryはここではまだ保持する。
        add_repository_package_lookup(
            lookups, seen_lookups,
            RepositoryPackageLookup{
                edge.resolved_provider->package_name,
                repository->repository_name});
    }
    return lookups;
}

bool ensure_repository_configuration(
    RepositoryMetadataPresentationContext& context) {
    if(context.configuration.has_value()) return true;
    if(context.configuration_attempted) return false;

    context.configuration_attempted = true;
    try {
        context.configuration.emplace(
            resolve_pacman_repository_configuration());
        return true;
    } catch(const PackageMetadataError& error) {
        context.unavailable_failure = error.failure();
        return false;
    }
}

bool is_configured_repository(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& repository_name) {
    return std::find(
               configuration.repository_names.begin(),
               configuration.repository_names.end(), repository_name) !=
           configuration.repository_names.end();
}

std::vector<RepositoryPackageLookup> filter_configured_repository_lookups(
    const std::vector<RepositoryPackageLookup>& lookups,
    const PacmanRepositoryConfiguration& configuration) {
    std::vector<RepositoryPackageLookup> configured_lookups;
    configured_lookups.reserve(lookups.size());
    for(const auto& lookup : lookups) {
        if(lookup.exact_repository_name.has_value() &&
           !is_configured_repository(
               configuration, lookup.exact_repository_name.value())) {
            continue;
        }
        configured_lookups.push_back(lookup);
    }
    return configured_lookups;
}

bool ensure_repository_metadata_session(
    RepositoryMetadataPresentationContext& context) {
    if(context.session.has_value()) return true;
    if(context.session_open_attempted || context.unavailable_failure.has_value() ||
       !context.configuration.has_value()) {
        return false;
    }

    context.session_open_attempted = true;
    try {
        context.session.emplace(RepositoryPackageMetadataSession::open(
            context.configuration.value()));
        return true;
    } catch(const PackageMetadataError& error) {
        context.unavailable_failure = error.failure();
        return false;
    }
}

const RepositoryPackageQueryResult& query_repository_package_cached(
    RepositoryMetadataPresentationContext& context,
    const RepositoryPackageLookup& lookup) {
    RepositoryPackageLookupIdentity identity =
        repository_package_lookup_identity(lookup);
    auto cached_result = context.query_results.find(identity);
    if(cached_result != context.query_results.end()) return cached_result->second;

    RepositoryPackageQueryResult result =
        context.session->query_repository_package(lookup);
    auto inserted = context.query_results.emplace(
        std::move(identity), std::move(result));
    return inserted.first->second;
}

std::string repository_metadata_unavailable_display(
    PackageMetadataErrorCode code) {
    switch(code) {
        case PackageMetadataErrorCode::ConfigurationUnavailable:
            return localization::translate_message(
                "Metadata       : unavailable (configuration unavailable)");
        case PackageMetadataErrorCode::ConfigurationMalformed:
            return localization::translate_message(
                "Metadata       : unavailable (configuration malformed)");
        case PackageMetadataErrorCode::InitializationFailed:
            return localization::translate_message(
                "Metadata       : unavailable (initialization failed)");
        case PackageMetadataErrorCode::LocalDatabaseUnavailable:
            return localization::translate_message(
                "Metadata       : unavailable (local database unavailable)");
        case PackageMetadataErrorCode::InvalidPackageName:
            return localization::translate_message(
                "Metadata       : unavailable (invalid package name)");
        case PackageMetadataErrorCode::QueryFailed:
            return localization::translate_message(
                "Metadata       : unavailable (query failed)");
        case PackageMetadataErrorCode::MalformedMetadata:
            return localization::translate_message(
                "Metadata       : unavailable (invalid metadata)");
        case PackageMetadataErrorCode::SyncDatabaseUnavailable:
            return localization::translate_message(
                "Metadata       : unavailable (sync database unavailable)");
        case PackageMetadataErrorCode::RepositoryNotConfigured:
            return localization::translate_message(
                "Metadata       : unavailable (repository not configured)");
    }
    return localization::translate_message(
        "Metadata       : unavailable (metadata failure)");
}

std::string repository_package_lookup_display(
    const RepositoryPackageLookup& lookup) {
    if(!lookup.exact_repository_name.has_value()) return lookup.package_name;
    return lookup.exact_repository_name.value() + "/" + lookup.package_name;
}

std::string format_iec_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t IEC_UNIT_BASE = 1024;
    constexpr std::array<const char*, 7> IEC_UNITS = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};

    if(bytes < IEC_UNIT_BASE) return std::to_string(bytes) + " B";

    std::size_t unit_index = 0;
    std::uint64_t unit_divisor = 1;
    while(unit_index + 1 < IEC_UNITS.size() &&
          bytes / unit_divisor >= IEC_UNIT_BASE) {
        unit_divisor *= IEC_UNIT_BASE;
        ++unit_index;
    }

    std::uint64_t whole = bytes / unit_divisor;
    std::uint64_t remainder = bytes % unit_divisor;
    std::array<std::uint64_t, 3> decimal_digits{};
    for(auto& digit : decimal_digits) {
        // unit_divisorは最大2^60なので、remainder * 10はuint64_t内に収まる。
        remainder *= 10;
        digit = remainder / unit_divisor;
        remainder %= unit_divisor;
    }

    std::uint64_t hundredths = decimal_digits[0] * 10 + decimal_digits[1];
    if(decimal_digits[2] >= 5) ++hundredths;
    if(hundredths == 100) {
        hundredths = 0;
        ++whole;
    }

    // LANDMINE: 1023.995...は1024.00ではなく、次unitの1.00として表示する。
    if(whole == IEC_UNIT_BASE && unit_index + 1 < IEC_UNITS.size()) {
        whole = 1;
        hundredths = 0;
        ++unit_index;
    }

    std::string formatted = std::to_string(whole) + ".";
    if(hundredths < 10) formatted += "0";
    formatted += std::to_string(hundredths);
    formatted += " ";
    formatted += IEC_UNITS[unit_index];
    return formatted;
}

void print_repository_metadata_unavailable(
    const PackageMetadataFailure& failure) {
    std::cout << std::endl;
    std::cout << localization::translate_message(
                     "Repository package sizes:")
              << std::endl;
    std::cout << "  "
              << repository_metadata_unavailable_display(failure.code)
              << std::endl;
}

void print_repository_package_sizes(
    const BuildPlan& plan,
    RepositoryMetadataPresentationContext& context) {
    std::vector<RepositoryPackageLookup> provisional_lookups =
        collect_repository_package_lookups(plan);
    if(provisional_lookups.empty()) return;

    if(!ensure_repository_configuration(context)) {
        print_repository_metadata_unavailable(context.unavailable_failure.value());
        return;
    }

    std::vector<RepositoryPackageLookup> lookups =
        filter_configured_repository_lookups(
            provisional_lookups, context.configuration.value());
    if(lookups.empty()) return;

    if(!ensure_repository_metadata_session(context)) {
        print_repository_metadata_unavailable(context.unavailable_failure.value());
        return;
    }

    std::cout << std::endl;
    std::cout << localization::translate_message(
                     "Repository package sizes:")
              << std::endl;
    std::set<RepositoryPackageDisplayIdentity> displayed_packages;
    std::uint64_t package_size_total = 0;
    std::uint64_t installed_size_total = 0;
    bool totals_available = true;
    std::vector<std::string> unavailable_details;
    for(const auto& lookup : lookups) {
        const RepositoryPackageQueryResult& result =
            query_repository_package_cached(context, lookup);
        if(const auto* metadata =
               std::get_if<RepositoryPackageMetadata>(&result)) {
            RepositoryPackageDisplayIdentity display_identity{
                metadata->repository_name, metadata->package_name};
            if(!displayed_packages.insert(display_identity).second) continue;
            if(package_size_total >
                   std::numeric_limits<std::uint64_t>::max() -
                       metadata->package_size_bytes ||
               installed_size_total >
                   std::numeric_limits<std::uint64_t>::max() -
                       metadata->installed_size_bytes) {
                totals_available = false;
                continue;
            }
            package_size_total += metadata->package_size_bytes;
            installed_size_total += metadata->installed_size_bytes;
            continue;
        }

        if(std::holds_alternative<PackageNotFound>(result)) {
            unavailable_details.push_back(
                repository_package_lookup_display(lookup) +
                ": " + localization::translate_message("not found"));
            continue;
        }

        const PackageMetadataFailure& failure =
            std::get<PackageMetadataFailure>(result);
        unavailable_details.push_back(
            repository_package_lookup_display(lookup) + ": " +
            repository_metadata_unavailable_display(failure.code));
    }

    std::cout << localization::format_translated_message(
                     "  packages: {}", displayed_packages.size())
              << std::endl;
    if(totals_available) {
        std::cout << localization::format_translated_message(
                         "    Package size   : {}",
                         format_iec_bytes(package_size_total))
                  << std::endl;
        std::cout << localization::format_translated_message(
                         "    Installed size : {}",
                         format_iec_bytes(installed_size_total))
                  << std::endl;
    } else {
        std::cout << localization::translate_message(
                         "    totals: unavailable (overflow)")
                  << std::endl;
    }
    for(const std::string& detail : unavailable_details) {
        std::cout << localization::format_translated_message(
                         "  attention: {}", detail)
                  << std::endl;
    }
}

// inspection固有のtrimは、domain contractを汎用string utilityへ持ち上げず局所保持する。
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if(first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

std::string join_comma_display_values(const std::vector<std::string>& values) {
    std::stringstream ss;
    for(size_t i = 0; i < values.size(); ++i) {
        if(i > 0) ss << ", ";
        ss << values[i];
    }
    return ss.str();
}

std::string plan_construction_label(PlanConstruction construction) {
    switch(construction) {
        case PlanConstruction::Constructed:
            return localization::translate_message("Constructed");
        case PlanConstruction::Failed:
            return localization::translate_message("Failed");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown plan construction state."));
}

std::string plan_completeness_label(PlanCompleteness completeness) {
    switch(completeness) {
        case PlanCompleteness::Complete:
            return localization::translate_message("Complete");
        case PlanCompleteness::Incomplete:
            return localization::translate_message("Incomplete");
        case PlanCompleteness::Unknown:
            return localization::translate_message("Unknown");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown plan completeness state."));
}

std::string provider_decision_label(ProviderDecision decision) {
    switch(decision) {
        case ProviderDecision::Unique:
            return localization::translate_message("Unique");
        case ProviderDecision::Selected:
            return localization::translate_message("Selected");
        case ProviderDecision::Ambiguous:
            return localization::translate_message("Ambiguous");
        case ProviderDecision::Cancelled:
            return localization::translate_message("Cancelled");
        case ProviderDecision::Unavailable:
            return localization::translate_message("Unavailable");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown provider decision state."));
}

std::string execution_capability_label(ExecutionCapability capability) {
    switch(capability) {
        case ExecutionCapability::Fetch:
            return localization::translate_message("Fetch");
        case ExecutionCapability::Build:
            return localization::translate_message("Build");
        case ExecutionCapability::Install:
            return localization::translate_message("Install");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown execution capability."));
}

std::string execution_readiness_label(ExecutionReadinessState readiness) {
    switch(readiness) {
        case ExecutionReadinessState::NotAssessed:
            return localization::translate_message("Not assessed");
        case ExecutionReadinessState::Ready:
            return localization::translate_message("Ready");
        case ExecutionReadinessState::RequiresCheck:
            return localization::translate_message("Requires check");
        case ExecutionReadinessState::Blocked:
            return localization::translate_message("Blocked");
        case ExecutionReadinessState::Unknown:
            return localization::translate_message("Unknown");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown execution readiness state."));
}

std::string plan_required_action_label(PlanRequiredAction action) {
    switch(action) {
        case PlanRequiredAction::None:
            return localization::translate_message("None");
        case PlanRequiredAction::CorrectPlanAuthority:
            return localization::translate_message("Correct plan authority");
        case PlanRequiredAction::ResolveDependency:
            return localization::translate_message("Resolve dependency");
        case PlanRequiredAction::SelectProvider:
            return localization::translate_message("Select provider");
        case PlanRequiredAction::ObtainMetadata:
            return localization::translate_message("Obtain metadata");
        case PlanRequiredAction::ReviewDeclaredRelations:
            return localization::translate_message("Review declared relations");
        case PlanRequiredAction::UsePackageBaseSetLifecycle:
            return localization::translate_message(
                "Use the package-base set lifecycle");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown plan required action."));
}

std::string plan_presentation_reason_label(
    PlanPresentationReasonKind kind) {
    switch(kind) {
        case PlanPresentationReasonKind::ConstraintAuthority:
            return localization::translate_message("constraint authority");
        case PlanPresentationReasonKind::SelectedProviderIdentityConflict:
            return localization::translate_message(
                "selected provider identity conflict");
        case PlanPresentationReasonKind::ConstraintReadiness:
            return localization::translate_message("constraint readiness");
        case PlanPresentationReasonKind::ResolutionFailure:
            return localization::translate_message("resolution failure");
        case PlanPresentationReasonKind::UnresolvedDependency:
            return localization::translate_message("unresolved dependency");
        case PlanPresentationReasonKind::AmbiguousProvider:
            return localization::translate_message("ambiguous provider");
        case PlanPresentationReasonKind::DependencyCycle:
            return localization::translate_message("dependency cycle");
        case PlanPresentationReasonKind::DeclaredRelation:
            return localization::translate_message(
                "package relation assessment");
        case PlanPresentationReasonKind::SplitPackage:
            return localization::translate_message("split package");
        case PlanPresentationReasonKind::IncompleteProviderCandidate:
            return localization::translate_message(
                "incomplete provider candidate");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown plan presentation reason."));
}

std::size_t unconstrained_dependency_count(const BuildPlan& plan) {
    return static_cast<std::size_t>(std::count_if(
        plan.dependency_edges.begin(), plan.dependency_edges.end(),
        [](const BuildPlanDependencyEdge& edge) {
            return edge.constraint_evaluation.has_value() &&
                   edge.constraint_evaluation->satisfaction() ==
                       ConstraintSatisfaction::Unconstrained;
        }));
}

void print_plan_state_summary(
    const BuildPlan& plan,
    const PlanStateProjection& state,
    const PresentationProjection& presentation) {
    std::cout << localization::translate_message("Plan state:") << std::endl;
    std::cout << localization::format_translated_message(
                     "  construction: {}",
                     plan_construction_label(state.construction))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "  completeness: {}",
                     plan_completeness_label(state.completeness))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "  provider decision: {}",
                     provider_decision_label(state.provider_decision))
              << std::endl;
    for(ExecutionCapability capability : {
            ExecutionCapability::Fetch,
            ExecutionCapability::Build,
            ExecutionCapability::Install}) {
        const ExecutionReadiness& readiness =
            execution_readiness(state, capability);
        std::cout << localization::format_translated_message(
                         "  {} readiness: {}",
                         execution_capability_label(capability),
                         execution_readiness_label(readiness.state))
                  << std::endl;
    }
    std::cout << localization::format_translated_message(
                     "  items: {} total, {} normal, {} attention-required",
                     presentation.summary_counts.total,
                     presentation.summary_counts.normal,
                     presentation.summary_counts.attention_required)
              << std::endl;
    const std::size_t unconstrained = unconstrained_dependency_count(plan);
    if(unconstrained != 0) {
        std::cout << localization::format_translated_message(
                         "  normal unconstrained dependencies: {}",
                         unconstrained)
                  << std::endl;
    }
}

void print_plan_attention_details(
    const PresentationProjection& presentation) {
    if(presentation.attention_items.empty()) return;

    std::cout << std::endl
              << localization::translate_message(
                     "Attention-required details:")
              << std::endl;
    for(const PresentationItem& item : presentation.attention_items) {
        std::cout << localization::format_translated_message(
                         "  - package: {}",
                         item.requested_package.value_or(
                             localization::translate_message(
                                 "<plan-wide>")))
                  << std::endl;
        if(item.package_base.has_value() &&
           item.package_base != item.requested_package) {
            std::cout << "    PackageBase: " << item.package_base.value()
                      << std::endl;
        }
        if(item.diagnostic_class.has_value()) {
            std::cout << localization::format_translated_message(
                             "    diagnostic: {}",
                             diagnostic_class_label(
                                 item.diagnostic_class.value()))
                      << std::endl;
        }
        for(const PlanPresentationReason& reason : item.plan_reasons) {
            std::cout << localization::format_translated_message(
                             "    {}: {}",
                             execution_capability_label(reason.capability),
                             execution_readiness_label(reason.readiness))
                      << std::endl;
            std::cout << localization::format_translated_message(
                             "      reason: {}",
                             plan_presentation_reason_label(reason.kind))
                      << std::endl;
            std::cout << localization::format_translated_message(
                             "      required action: {}",
                             plan_required_action_label(
                                 reason.required_action))
                      << std::endl;
        }
    }
}

std::string aur_git_url_for_package_base(const std::string& package_base) {
    require_valid_package_name(package_base);
    return AUR_BASE_URL + package_base + ".git";
}

std::string dependency_display_name(const std::string& dependency, const std::string& package_name) {
    std::string display;
    if(package_name.empty() || dependency == package_name)
        display = dependency;
    else
        display = dependency + " (" + package_name + ")";
    return dependency_display_with_constraint_note(display, dependency);
}

std::string dependency_kind_display(DependencyKind kind) {
    // NO_TRANSLATE(Issue #308): These values are stable dependency-kind tokens
    // in the recursive inspection format, not human-readable prose labels.
    switch(kind) {
        case DependencyKind::Installed:
            return "installed";
        case DependencyKind::Repo:
            return "repo";
        case DependencyKind::Aur:
            return "aur";
        case DependencyKind::Local:
            return "local";
        case DependencyKind::Provided:
            return "provided";
        case DependencyKind::AmbiguousProvider:
            return "ambiguous-provider";
        case DependencyKind::Unknown:
            return "unknown";
    }
    return "unknown";
}

void print_recursive_dependency_node(const RecursiveDependencyNode& node, size_t indent) {
    std::cout << std::string(indent, ' ') << "- "
              << dependency_display_name(node.dependency, node.package_name) << " ["
              << dependency_kind_display(node.kind) << "]";
    if(node.kind == DependencyKind::Aur && !node.package_base.empty() && node.package_base != node.package_name) {
        std::cout << " " << localization::format_translated_message("base: {}", node.package_base);
    }
    if(node.provided_by.has_value()) {
        std::cout << " " << localization::format_translated_message("by {}", provided_dependency_display(node.provided_by.value()));
    }
    if(!node.provider_candidates.empty()) {
        std::vector<std::string> candidates;
        for(size_t i = 0; i < node.provider_candidates.size(); ++i) {
            candidates.push_back(provided_dependency_display(
                node.provider_candidates[i]));
        }
        std::cout << " " << localization::format_translated_message("candidates: {}", join_comma_display_values(candidates));
    }
    if(node.already_visited) {
        std::cout << " " << localization::translate_message("(already visited)");
    }
    if(node.max_depth_reached) {
        std::cout << " " << localization::translate_message("(max depth reached)");
    }
    std::cout << std::endl;

    for(const auto& child : node.children) {
        print_recursive_dependency_node(child, indent + 2);
    }
}

void print_recursive_dependency_tree(const std::vector<RecursiveDependencyNode>& nodes) {
    std::cout << localization::translate_message(
                     "Recursive dependency tree:")
              << std::endl;
    if(nodes.empty()) {
        std::cout << localization::translate_message("  None") << std::endl;
        return;
    }
    for(const auto& node : nodes) {
        print_recursive_dependency_node(node, 2);
    }
}

void add_unique_value(std::vector<std::string>& values, const std::string& value) {
    std::string trimmed = trim(value);
    if(trimmed.empty()) return;
    if(std::find(values.begin(), values.end(), trimmed) == values.end()) values.push_back(trimmed);
}

void print_dependency_group(const std::string& label, const std::vector<std::string>& dependencies) {
    std::cout << label << std::endl;
    if(dependencies.empty()) {
        std::cout << localization::translate_message("  None") << std::endl;
        return;
    }
    for(const auto& dep : dependencies) {
        std::cout << "  " << dep << std::endl;
    }
}

void print_ambiguous_provider_group(
    const std::string& label, const std::vector<AmbiguousProvidedDependency>& dependencies) {
    std::cout << label << std::endl;
    if(dependencies.empty()) {
        std::cout << localization::translate_message("  None") << std::endl;
        return;
    }

    for(const auto& dependency : dependencies) {
        std::cout << "  " << dependency.dependency << std::endl;
        std::cout << localization::translate_message("    candidates:")
                  << std::endl;
        for(size_t i = 0; i < dependency.candidates.size(); ++i) {
            std::cout << "      " << (i + 1) << ". "
                      << provided_dependency_display(dependency.candidates[i])
                      << std::endl;
        }
    }
}

void print_selected_provider_group(
    const std::string& label,
    const std::vector<SelectedProvidedDependency>& dependencies) {
    std::cout << label << std::endl;
    if(dependencies.empty()) {
        std::cout << localization::translate_message("  None") << std::endl;
        return;
    }
    for(const auto& dependency : dependencies) {
        std::cout << "  "
                  << dependency.dependency
                  << " -> "
                  << provided_dependency_display(dependency.provider)
                  << std::endl;
    }
}

void print_constraint_evaluations(
    const BuildPlan& plan,
    const std::optional<std::string>& parent_package_name =
        std::nullopt,
    bool suppress_unconstrained = false) {
    bool printed_header = false;
    for(const auto& edge : plan.dependency_edges) {
        if(parent_package_name.has_value() &&
           edge.parent_package_name != parent_package_name.value()) {
            continue;
        }
        if(!edge.constraint_evaluation.has_value()) continue;
        if(suppress_unconstrained &&
           edge.constraint_evaluation->satisfaction() ==
               ConstraintSatisfaction::Unconstrained) {
            continue;
        }
        if(!printed_header) {
            std::cout << std::endl
                      << localization::translate_message(
                             "Dependency constraint evaluations:")
                      << std::endl;
            printed_header = true;
        }
        const ConstraintEvaluation& evaluation =
            edge.constraint_evaluation.value();
        const std::string result = constraint_satisfaction_display(
            evaluation.satisfaction());
        const std::string reason =
            constraint_evaluation_reason_display(evaluation);
        std::cout << localization::format_translated_message(
                         "  - {}: result={}, reason={}",
                         edge.dependency_spec, result, reason)
                  << std::endl;
        if(evaluation.satisfaction() ==
               ConstraintSatisfaction::Unsatisfied ||
           evaluation.satisfaction() == ConstraintSatisfaction::Unknown) {
            Logger::warn(localization::format_translated_message(
                "Dependency {} is {}: {}",
                edge.dependency_spec, result, reason));
        }
    }
}

void print_resolution_failures(const BuildPlan& plan) {
    for(const auto& failure : plan.resolution_failures) {
        switch(failure.kind) {
            case BuildPlanResolutionFailureKind::
                InstalledPackageMetadataUnavailable:
                Logger::warn(localization::format_translated_message(
                    "Installed package metadata for {} is unavailable: {}",
                    failure.subject, failure.diagnostic));
                break;
            case BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable:
                Logger::warn(localization::format_translated_message(
                    "Repository package metadata for {} is unavailable: {}",
                    failure.subject, failure.diagnostic));
                break;
            case BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable:
                Logger::warn(localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // service name "AUR"; the remaining placeholders are a
                    // package identity and diagnostic, respectively.
                    "{} package metadata for {} is unavailable: {}",
                    "AUR", failure.subject, failure.diagnostic));
                break;
            case BuildPlanResolutionFailureKind::ProviderSearchUnavailable:
            case BuildPlanResolutionFailureKind::
                ProviderCandidateMetadataUnavailable:
                Logger::warn(localization::format_translated_message(
                    "Provider metadata for {} is unavailable: {}",
                    failure.subject, failure.diagnostic));
                break;
        }
    }
}

DependencyClassification classify_direct_build_plan_edges(
    const BuildPlan& plan, const std::string& parent_package_name) {
    DependencyClassification classified;
    for(const auto& edge : plan.dependency_edges) {
        if(edge.parent_package_name != parent_package_name) continue;
        switch(edge.kind) {
            case DependencyKind::Installed:
                add_unique_value(classified.installed, edge.dependency_spec);
                break;
            case DependencyKind::Repo:
                add_unique_value(classified.repo, edge.dependency_spec);
                break;
            case DependencyKind::Aur:
            case DependencyKind::Local:
                add_unique_value(classified.aur, edge.dependency_spec);
                break;
            case DependencyKind::Provided:
                if(!edge.resolved_provider.has_value()) {
                    add_unique_value(classified.unknown, edge.dependency_spec);
                    break;
                }
                if(edge.provider_resolution ==
                   ProviderResolutionKind::UserSelected) {
                    classified.selected_providers.push_back(
                        SelectedProvidedDependency{
                            edge.dependency_spec,
                            edge.resolved_provider.value()});
                } else {
                    classified.provided.push_back(
                        edge.dependency_spec + " -> " +
                        provided_dependency_display(
                            edge.resolved_provider.value()));
                }
                break;
            case DependencyKind::AmbiguousProvider:
            case DependencyKind::Unknown:
                break;
        }
    }
    for(const auto& ambiguous : plan.ambiguous_providers) {
        const bool is_direct = std::any_of(
            plan.dependency_edges.begin(), plan.dependency_edges.end(),
            [&parent_package_name, &ambiguous](
                const BuildPlanDependencyEdge& edge) {
                return edge.parent_package_name == parent_package_name &&
                       edge.kind == DependencyKind::AmbiguousProvider &&
                       edge.dependency_spec == ambiguous.dependency;
            });
        if(is_direct) {
            classified.ambiguous_providers.push_back(ambiguous);
        }
    }
    for(const auto& unresolved : plan.unresolved) {
        add_unique_value(classified.unknown, unresolved);
    }
    return classified;
}

void print_incomplete_provider_candidate_sets(const BuildPlan& plan) {
    if(plan.incomplete_provider_candidate_sets.empty()) return;

    std::cout << std::endl
              << localization::translate_message(
                     "Incomplete provider candidate observations:")
              << std::endl;
    for(const auto& candidate_set :
        plan.incomplete_provider_candidate_sets) {
        const std::string reason = constraint_evaluation_reason_display(
            ConstraintEvaluation::unknown(candidate_set.reason));
        std::cout << localization::format_translated_message(
                         "  - {}: {}", candidate_set.dependency,
                         reason)
                  << std::endl;
        if(!candidate_set.observed_candidates.empty()) {
            std::vector<std::string> candidates;
            candidates.reserve(
                candidate_set.observed_candidates.size());
            for(const auto& candidate :
                candidate_set.observed_candidates) {
                candidates.push_back(
                    provided_dependency_display(candidate));
            }
            std::cout << localization::format_translated_message(
                             "    observed candidates: {}",
                             join_comma_display_values(candidates))
                      << std::endl;
        }
        Logger::warn(localization::format_translated_message(
            "Provider candidates for {} are incomplete: {}",
            candidate_set.dependency, reason));
    }
}

void print_metadata_risk_group(const std::vector<BuildPlanMetadataRisk>& risks) {
    std::cout << localization::translate_message(
                     "Metadata conflicts/replaces:")
              << std::endl;
    for(const auto& risk : risks) {
        if(risk.package_base != risk.package_name) {
            std::cout << localization::format_translated_message(
                             "  {} (base: {})", risk.package_name,
                             risk.package_base)
                      << std::endl;
        } else {
            std::cout << "  " << risk.package_name << std::endl;
        }
        if(!risk.conflicts.empty())
            std::cout << localization::format_translated_message(
                             "    conflicts: {}",
                             join_comma_display_values(risk.conflicts))
                      << std::endl;
        if(!risk.replaces.empty())
            std::cout << localization::format_translated_message(
                             "    replaces: {}",
                             join_comma_display_values(risk.replaces))
                      << std::endl;
    }
}

void print_relation_assessment_group(
    const std::vector<PackageRelationAssessment>& assessments,
    const std::optional<std::string>& declaring_package = std::nullopt) {
    bool printed_header = false;
    for(const auto& assessment : assessments) {
        if(declaring_package.has_value() &&
           assessment.declaring_package.package_name !=
               declaring_package.value()) {
            continue;
        }
        if(!printed_header) {
            std::cout << localization::translate_message(
                             "Package relation assessments:")
                      << std::endl;
            printed_header = true;
        }
        std::cout << "  - "
                  << package_relation_assessment_diagnostic_display(
                         assessment)
                  << std::endl;
    }
}

void print_build_plan(const BuildPlan& plan) {
    const PlanStateProjection state = project_build_plan_state(plan);
    const PresentationProjection presentation =
        project_build_plan_presentation(plan);
    print_plan_state_summary(plan, state, presentation);
    print_plan_attention_details(presentation);

    // The generic projection keeps the typed reason/capability hierarchy.
    // Route-owned details below retain candidates, raw declared metadata, and
    // diagnostics that must not be discarded by aggregation.
    if(!plan.ambiguous_providers.empty()) {
        std::cout << std::endl;
        print_ambiguous_provider_group(
            localization::translate_message(
                "Ambiguous provided dependencies:"),
            plan.ambiguous_providers);
    }
    if(!plan.split_package_targets.empty()) {
        std::cout << std::endl;
        std::cout << localization::translate_message(
                         "Split package install targets:")
                  << std::endl;
        for(const auto& target : plan.split_package_targets) {
            std::cout << localization::format_translated_message(
                             "  - {} (base: {})", target.package_name,
                             target.package_base)
                      << std::endl;
        }
    }
    if(!plan.metadata_risks.empty()) {
        std::cout << std::endl;
        print_metadata_risk_group(plan.metadata_risks);
    }
    if(!plan.relation_assessments.empty()) {
        std::cout << std::endl;
        print_relation_assessment_group(plan.relation_assessments);
    }
    if(!plan.unresolved.empty()) {
        std::cout << std::endl;
        std::cout << localization::translate_message(
                         "Unresolved dependencies:")
                  << std::endl;
        for(const auto& dependency : plan.unresolved) {
            std::cout << "  - " << dependency << std::endl;
        }
    }
    print_incomplete_provider_candidate_sets(plan);
    if(!plan.cycles.empty()) {
        std::cout << std::endl;
        std::cout << localization::translate_message(
                         "Cyclic dependencies:")
                  << std::endl;
        for(const auto& dependency : plan.cycles) {
            std::cout << "  - " << dependency << std::endl;
        }
    }
    print_constraint_evaluations(plan, std::nullopt, true);
    print_resolution_failures(plan);

    std::cout << std::endl;
    std::cout << localization::translate_message("Build plan:") << std::endl;
    if(plan.order.empty()) {
        std::cout << localization::translate_message("  None") << std::endl;
    } else {
        for(size_t i = 0; i < plan.order.size(); ++i) {
            const BuildPlanEntry& entry = plan.order[i];
            std::cout << "  " << (i + 1) << ". " << entry.package_base;
            std::cout << std::endl;
            std::vector<std::string> distinct_targets;
            for(const auto& package_name : entry.package_names) {
                if(package_name != entry.package_base) add_unique_value(distinct_targets, package_name);
            }
            if(!distinct_targets.empty()) {
                std::cout << localization::format_translated_plural_message(
                                 "     target package: {}",
                                 "     target packages: {}",
                                 distinct_targets.size(),
                                 join_comma_display_values(
                                     distinct_targets))
                          << std::endl;
            }
        }
    }

    if(!plan.provided.empty()) {
        std::cout << std::endl;
        std::cout << localization::translate_message(
                         "Provided dependencies:")
                  << std::endl;
        for(const auto& dependency : plan.provided) {
            std::cout << "  - "
                      << dependency.dependency
                      << " -> "
                      << provided_dependency_display(dependency.provider)
                      << (dependency.resolution ==
                                  ProviderResolutionKind::UserSelected
                              ? localization::translate_message(
                                    " (selected)")
                              : "")
                      << std::endl;
        }
    }
}

void retain_cancelled_provider_decisions(
    BuildPlan& plan,
    const std::shared_ptr<ProviderSelectionSession>& selection) {
    if(!selection) return;
    for(const AmbiguousProvidedDependency& dependency :
        plan.ambiguous_providers) {
        if(selection->was_cancelled(dependency.dependency)) {
            add_unique_value(
                plan.cancelled_provider_dependencies,
                dependency.dependency);
        }
    }
}

void print_fetch_plan(const BuildPlan& plan) {
    std::cout << localization::translate_message("Fetch targets:")
              << std::endl;
    if(plan.order.empty()) {
        std::cout << localization::translate_message("  None") << std::endl;
    } else {
        for(size_t i = 0; i < plan.order.size(); ++i) {
            const BuildPlanEntry& entry = plan.order[i];
            std::cout << "  " << (i + 1) << ". " << entry.package_base << " -> "
                      << aur_git_url_for_package_base(entry.package_base) << std::endl;
        }
    }

    std::vector<SelectedProvidedDependency> selected_providers;
    for(const auto& dependency : plan.provided) {
        if(dependency.resolution !=
           ProviderResolutionKind::UserSelected) {
            continue;
        }
        selected_providers.push_back(SelectedProvidedDependency{
            dependency.dependency, dependency.provider});
    }
    if(!selected_providers.empty()) {
        std::cout << std::endl;
        print_selected_provider_group(
            localization::translate_message(
                "Selected provided dependencies:"),
            selected_providers);
    }

    if(!plan.unresolved.empty()) {
        std::cout << std::endl;
        std::cout << localization::translate_message(
                         "Unresolved dependencies:")
                  << std::endl;
        for(const auto& dependency : plan.unresolved) {
            Logger::warn(dependency);
        }
    }

    print_incomplete_provider_candidate_sets(plan);

    if(!plan.ambiguous_providers.empty()) {
        std::cout << std::endl;
        print_ambiguous_provider_group(
            localization::translate_message(
                "Ambiguous provided dependencies:"),
            plan.ambiguous_providers);
    }

    if(!plan.cycles.empty()) {
        std::cout << std::endl;
        std::cout << localization::translate_message(
                         "Cyclic dependencies:")
                  << std::endl;
        for(const auto& dependency : plan.cycles) {
            Logger::warn(dependency);
        }
    }

    if(!plan.metadata_risks.empty()) {
        std::cout << std::endl;
        print_metadata_risk_group(plan.metadata_risks);
    }
    if(!plan.relation_assessments.empty()) {
        std::cout << std::endl;
        print_relation_assessment_group(plan.relation_assessments);
    }
}

} // namespace

int cmd_deps(
    const std::vector<std::string>& targets,
    const std::vector<std::string>& flags,
    const AppConfig& config) {
    bool recursive = false;
    for(const auto& flag : flags) {
        if(flag ==
           cli_authority::operation_spec(
               cli_authority::OperationId::Deps)
               .token) {
            continue;
        }
        if(flag == "--recursive") {
            recursive = true;
            continue;
        }
        Logger::error(localization::format_translated_message(
            "Unsupported {} option: {}", "deps", flag));
        Logger::error(deps_usage());
        return 1;
    }

    if(targets.empty()) {
        Logger::error(deps_usage());
        return 1;
    }

    ProviderSelectionCallback select_provider =
        provider_selection_callback(config);
    BuildPlan invocation_plan;
    try {
        invocation_plan = resolve_build_plan_for_preflight(
            targets, select_provider);
    } catch(const std::exception& error) {
        Logger::error(localization::format_translated_message(
            "Failed to inspect dependencies for {}: {}",
            join_comma_display_values(targets), error.what()));
        return 1;
    }
    print_resolution_failures(invocation_plan);

    bool failed = false;
    for(size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        require_valid_package_name(target);

        try {
            std::optional<AurPackageInfo> info = AurClient::info(target);
            if(!info.has_value()) {
                Logger::error(localization::format_translated_message(
                    "{} package not found: {}", "AUR", target));
                failed = true;
                continue;
            }

            std::vector<std::string> dependencies =
                collect_build_dependencies(info.value());
            DependencyClassification classified =
                classify_direct_build_plan_edges(
                    invocation_plan, info->Name);

            if(i > 0) std::cout << std::endl;
            std::cout << localization::format_translated_message(
                             "Package         : {}", info->Name)
                      << std::endl;
            std::cout << localization::format_translated_message(
                             "Package Base    : {}", info->PackageBase)
                      << std::endl;
            std::cout << localization::format_translated_message(
                             "Dependencies    : {}", dependencies.size())
                      << std::endl;
            std::cout << std::endl;
            print_dependency_group(
                localization::translate_message(
                    "Installed dependencies:"),
                classified.installed);
            std::cout << std::endl;
            print_dependency_group(
                localization::format_translated_message(
                    "Official {} dependencies:", "repo"),
                classified.repo);
            std::cout << std::endl;
            print_dependency_group(
                localization::format_translated_message(
                    "{} dependencies:", "AUR"),
                classified.aur);
            std::cout << std::endl;
            print_dependency_group(
                localization::translate_message(
                    "Provided dependencies:"),
                classified.provided);
            if(!classified.selected_providers.empty()) {
                std::cout << std::endl;
                print_selected_provider_group(
                    localization::translate_message(
                        "Selected provided dependencies:"),
                    classified.selected_providers);
            }
            std::cout << std::endl;
            print_ambiguous_provider_group(
                localization::translate_message(
                    "Ambiguous provided dependencies:"),
                classified.ambiguous_providers);
            std::cout << std::endl;
            print_dependency_group(
                localization::translate_message(
                    "Unknown dependencies:"),
                classified.unknown);
            std::vector<BuildPlanMetadataRisk> metadata_risks =
                collect_build_plan_metadata_risks(info.value());
            if(!metadata_risks.empty()) {
                std::cout << std::endl;
                print_metadata_risk_group(metadata_risks);
            }
            if(std::any_of(
                   invocation_plan.relation_assessments.begin(),
                   invocation_plan.relation_assessments.end(),
                   [&info](const PackageRelationAssessment& assessment) {
                       return assessment.declaring_package.package_name ==
                              info->Name;
                   })) {
                std::cout << std::endl;
                print_relation_assessment_group(
                    invocation_plan.relation_assessments, info->Name);
            }
            print_incomplete_provider_candidate_sets(invocation_plan);
            print_constraint_evaluations(invocation_plan, info->Name);
            if(recursive) {
                std::vector<RecursiveDependencyNode> recursive_nodes =
                    resolve_recursive_dependencies(
                        info.value(), select_provider);
                std::cout << std::endl;
                print_recursive_dependency_tree(recursive_nodes);
            }
        } catch(const std::exception& e) {
            Logger::error(localization::format_translated_message(
                "Failed to inspect dependencies for {}: {}", target,
                e.what()));
            failed = true;
        }
    }

    return failed ? 1 : 0;
}

int cmd_plan(
    const std::vector<std::string>& targets,
    const std::vector<std::string>& flags,
    const AppConfig& config) {
    for(const auto& flag : flags) {
        if(flag ==
           cli_authority::operation_spec(
               cli_authority::OperationId::Plan)
               .token) {
            continue;
        }
        Logger::error(localization::format_translated_message(
            "Unsupported {} option: {}", "plan", flag));
        Logger::error(plan_usage());
        return 1;
    }

    if(targets.empty()) {
        Logger::error(plan_usage());
        return 1;
    }

    RepositoryMetadataPresentationContext metadata_context;
    ProviderSelectionCallback select_provider =
        provider_selection_callback(config);
    for(const auto& target : targets) {
        require_valid_package_name(target);
    }

    try {
        BuildPlan plan = resolve_build_plan_for_preflight(
            targets, select_provider);
        retain_cancelled_provider_decisions(
            plan, config.provider_selection);
        print_build_plan(plan);
        print_repository_package_sizes(plan, metadata_context);
    } catch(const std::exception& error) {
        Logger::error(localization::format_translated_message(
            "Failed to plan build order for {}: {}",
            join_comma_display_values(targets), error.what()));
        return 1;
    }
    return 0;
}

int cmd_fetch(
    const std::vector<std::string>& targets,
    const std::vector<std::string>& flags,
    const AppConfig& config) {
    for(const auto& flag : flags) {
        if(flag ==
           cli_authority::operation_spec(
               cli_authority::OperationId::Fetch)
               .token) {
            continue;
        }
        Logger::error(localization::format_translated_message(
            "Unsupported {} option: {}", "fetch", flag));
        Logger::error(fetch_usage());
        return 1;
    }

    if(targets.empty()) {
        Logger::error(fetch_usage());
        return 1;
    }

    FetchPreparation preparation;
    try {
        preparation = prepare_fetch_operation(targets, config);
        print_fetch_plan(preparation.plan);
        // POLICY(#150): fetch は read-only retrieval stage。metadata risk は表示するが取得を妨げない。
        require_fetchable_build_plan(
            preparation.invocation_targets, preparation.plan);
    } catch(const std::exception& error) {
        Logger::error(localization::format_translated_message(
            "Failed to fetch repositories for {}: {}",
            join_comma_display_values(targets), error.what()));
        return 1;
    }

    // POLICY(#174/#272/#351): invocation全体のprovider選択とconstraint
    // preflightが成功するまでcache preparationやclone/fetchへ進まない。
    ValidatedCacheRoot cache_root = prepare_process_cache_root();
    bool failed = false;
    for(const auto& entry : preparation.plan.order) {
        try {
            fetch_persistent_checkout(
                cache_root,
                entry.package_base,
                aur_git_url_for_package_base(entry.package_base));
        } catch(const std::exception& error) {
            Logger::error(localization::format_translated_message(
                "Failed to fetch repositories for {}: {}",
                preparation.invocation_targets, error.what()));
            failed = true;
        }
    }

    return failed ? 1 : 0;
}

FetchPreparation prepare_fetch_operation(
    const std::vector<std::string>& targets,
    const AppConfig& config) {
    if(targets.empty()) {
        throw std::invalid_argument(fetch_usage());
    }
    // Invalid targetはprovider resolutionやcache mutationより前に全件拒否する。
    for(const auto& target : targets) {
        require_valid_package_name(target);
    }

    ProviderSelectionCallback select_provider =
        provider_selection_callback(config);
    return FetchPreparation{
        resolve_fetch_plan(targets, select_provider),
        join_comma_display_values(targets)};
}

int cmd_export_pkgbuild_tree(const PkgbuildExportInvocation& invocation) {
    export_pkgbuild_tree(
        invocation.target, invocation.output_directory);
    return 0;
}

int cmd_print_pkgbuild(const PkgbuildExportInvocation& invocation) {
    std::string pkgbuild = load_pkgbuild_for_stdout(invocation.target);
    if(pkgbuild.size() >
       static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error(localization::format_translated_message(
            "{} is too large to write to {}.",
            "PKGBUILD", "stdout"));
    }

    // POLICY(#167/#196): moduleがtemporary checkoutをcleanupしてから返したbytesだけを出力する。
    std::cout.write(pkgbuild.data(), static_cast<std::streamsize>(pkgbuild.size()));
    std::cout.flush();
    if(!std::cout) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to write {} to {}.", "PKGBUILD", "stdout"));
    }
    return 0;
}

std::string devel_requires_check_query_message(
    const AurUpdatePlanEntry& entry) {
    if(entry.devel_assessment_origin == AurDevelAssessmentOrigin::CurrentObservation && entry.devel_assessment.requires_check_reason())
        return localization::format_translated_message("{}: devel update requires check; local authority is unavailable or has changed.", entry.installed_name);
    if(!entry.devel_classification.has_value() ||
       entry.devel_assessment.requires_check_reason() == nullptr ||
       *entry.devel_assessment.requires_check_reason() !=
           DevelRequiresCheckReason::SuffixCandidateOnly) {
        throw std::logic_error(localization::translate_message(
            "Devel check-required update entry is inconsistent."));
    }

    const DevelPackageSuffixEvidence& suffix =
        entry.devel_classification->suffix_evidence();
    const bool package_base_candidate =
        suffix.package_base_candidate_kind() != nullptr;
    const bool child_candidate = suffix.installed_children().size() == 1 &&
                                 suffix.installed_children().front().candidate_kind() != nullptr;
    if(package_base_candidate && child_candidate) {
        return localization::format_translated_message(
            // TRANSLATORS: The placeholders are an installed package,
            // the literal field name "PackageBase", and its identity.
            "{}: devel package update status requires check (suffix candidate only: {} {} and installed package suffix; authoritative build provenance is unavailable).",
            entry.installed_name, "PackageBase", suffix.package_base());
    }
    if(package_base_candidate) {
        return localization::format_translated_message(
            // TRANSLATORS: The placeholders are an installed package,
            // the literal field name "PackageBase", and its identity.
            "{}: devel package update status requires check (suffix candidate only: {} {}; authoritative build provenance is unavailable).",
            entry.installed_name, "PackageBase", suffix.package_base());
    }
    if(child_candidate) {
        return localization::format_translated_message(
            // TRANSLATORS: The placeholder is an installed package.
            "{}: devel package update status requires check (installed package suffix candidate only; authoritative build provenance is unavailable).",
            entry.installed_name);
    }
    throw std::logic_error(localization::translate_message(
        "Devel check-required update entry has no suffix evidence."));
}

int cmd_query_foreign_updates() {
    AurUpdateQueryResult query_result = query_installed_aur_updates();
    if(query_result.plan.entries.empty()) {
        Logger::info(localization::translate_message(
            "No foreign packages found."));
        return query_result.recoverable_failures.empty() ? 0 : 1;
    }

    for(size_t i = 0; i < query_result.plan.entries.size(); ++i) {
        const AurUpdatePlanEntry& entry = query_result.plan.entries[i];
        Logger::info(localization::format_translated_message(
            "Checking package {}/{}: {}", i + 1,
            query_result.plan.entries.size(), entry.installed_name));

        switch(project_aur_update_effective_state(entry)) {
            case AurUpdateEffectiveState::NonAurForeign:
            case AurUpdateEffectiveState::MetadataUnavailable:
                // POLICY(#266): failed batchも従来のnot-found warningを維持するが、
                // pure modelではconfirmed absenceとquery failureを同一視しない。
                Logger::warn(localization::format_translated_message(
                    "Foreign package not found in {}: {}",
                    "AUR", entry.installed_name));
                break;
            case AurUpdateEffectiveState::UpdateAvailable:
                if(aur_update_basis(entry) == AurUpdateBasis::GitRevision)
                    std::cout << entry.installed_name << " " << entry.installed_version << " [Git revision update available]" << std::endl;
                else
                    std::cout << entry.installed_name << " " << entry.installed_version << " -> " << entry.aur_package->version << std::endl;
                break;
            case AurUpdateEffectiveState::UpToDate:
                break;
            case AurUpdateEffectiveState::RequiresCheck:
                Logger::warn(devel_requires_check_query_message(entry));
                break;
            case AurUpdateEffectiveState::VersionComparisonUnavailable:
                Logger::warn(localization::format_translated_message(
                    "Failed to compare versions: {} -> {}",
                    entry.installed_version,
                    entry.aur_package->version));
                break;
            case AurUpdateEffectiveState::Unknown:
                Logger::error(localization::format_translated_message("Devel Git observation failed: {}", entry.installed_name));
                query_result.recoverable_failures.push_back({{entry.installed_name}, "Devel Git observation failed"});
                break;
            case AurUpdateEffectiveState::Unsupported:
                Logger::warn(localization::format_translated_message("Devel automatic update is unsupported: {}", entry.installed_name));
                break;
            case AurUpdateEffectiveState::Inconsistent:
                throw std::logic_error(localization::format_translated_message(
                    "Inconsistent {} update state.", "AUR"));
        }
    }

    return query_result.recoverable_failures.empty() ? 0 : 1;
}
