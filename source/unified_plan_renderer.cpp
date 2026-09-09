#include "unified_plan_renderer.hpp"

#include "aur_update_execution_preparation.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_query.hpp"
#include "commands_sync.hpp"
#include "filtered_aur_update_operation.hpp"
#include "local_dependency_plan_projection.hpp"
#include "localization.hpp"
#include "package_relation_presentation.hpp"
#include "system_source_upgrade.hpp"
#include "system_aur_update_operation.hpp"
#include "terminal_safe_text.hpp"
#include "unified_plan_projection.hpp"
#include "upgrade_all_operation.hpp"

#include <exception>
#include <filesystem>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

std::string terminal_safe_text_display(std::string_view value);
std::string invalid_snapshot_raw_value_display(std::string_view value);

struct RenderState {
    std::ostringstream output;
    std::vector<UnifiedPlanRenderingIssue> issues;

    void add_issue(
        UnifiedPlanRenderingIssueKind kind,
        UnifiedPlanRenderingSection section, std::size_t item_index,
        std::optional<std::size_t> detail_index,
        std::string diagnostic) {
        issues.push_back(UnifiedPlanRenderingIssue{
            kind, section, item_index, detail_index,
            std::move(diagnostic)});
    }
};

std::string unsupported_display(
    RenderState& state, UnifiedPlanRenderingSection section,
    std::size_t item_index, std::optional<std::size_t> detail_index,
    std::string diagnostic) {
    state.add_issue(
        UnifiedPlanRenderingIssueKind::UnsupportedValue, section,
        item_index, detail_index, std::move(diagnostic));
    return localization::translate_message("unsupported");
}

std::string unavailable_display(
    RenderState& state, UnifiedPlanRenderingSection section,
    std::size_t item_index, std::optional<std::size_t> detail_index,
    std::string diagnostic) {
    state.add_issue(
        UnifiedPlanRenderingIssueKind::MissingReferencedValue, section,
        item_index, detail_index, std::move(diagnostic));
    return localization::translate_message("unavailable");
}

std::string required_string_display(
    const std::string& value, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index,
    std::string diagnostic) {
    if(!value.empty()) return terminal_safe_text_display(value);
    return unavailable_display(
        state, section, item_index, detail_index, std::move(diagnostic));
}

std::string optional_string_display(
    const std::optional<std::string>& value) {
    return value.has_value() && !value->empty()
               ? terminal_safe_text_display(value.value())
               : localization::translate_message("not observed");
}

std::string optional_index_display(
    const std::optional<std::size_t>& value) {
    return value.has_value()
               ? std::to_string(value.value())
               : localization::translate_message("not observed");
}

std::string yes_no_display(bool value) {
    return value ? localization::translate_message("yes")
                 : localization::translate_message("no");
}

std::string join_display_values(const std::vector<std::string>& values) {
    std::string display;
    for(std::size_t index = 0; index < values.size(); ++index) {
        if(index != 0) display += ", ";
        display += values[index];
    }
    return display;
}

std::string observation_status_display(
    UnifiedPlanObservationStatus status, RenderState& state) {
    switch(status) {
        case UnifiedPlanObservationStatus::Ready:
            return localization::translate_message("Ready");
        case UnifiedPlanObservationStatus::NoOp:
            return localization::translate_message("NoOp");
        case UnifiedPlanObservationStatus::Blocked:
            return localization::translate_message("Blocked");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Status, 0, std::nullopt,
        localization::translate_message(
            "Unified plan status is not supported by the renderer."));
}

std::string root_source_display(
    UnifiedPlanRootSourceKind source, RenderState& state,
    std::size_t root_index) {
    switch(source) {
        case UnifiedPlanRootSourceKind::Repository:
            return localization::translate_message("repository");
        case UnifiedPlanRootSourceKind::Aur:
            return "AUR";
        case UnifiedPlanRootSourceKind::Local:
            return localization::translate_message("local");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Roots, root_index,
        std::nullopt,
        localization::translate_message(
            "A root source is not supported by the renderer."));
}

std::string root_route_display(
    UnifiedPlanRootRouteKind route, RenderState& state,
    std::size_t root_index) {
    switch(route) {
        case UnifiedPlanRootRouteKind::RepositoryTransaction:
            return localization::translate_message("repository transaction");
        case UnifiedPlanRootRouteKind::RepositorySourceBuild:
            return localization::translate_message("repository source build");
        case UnifiedPlanRootRouteKind::AurSourceBuild:
            return localization::format_translated_message(
                "{} source build", "AUR");
        case UnifiedPlanRootRouteKind::LocalSourceBuild:
            return localization::translate_message("local source build");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Roots, root_index,
        std::nullopt,
        localization::translate_message(
            "A root route is not supported by the renderer."));
}

bool root_route_matches_typed_identity(
    const UnifiedPlanRootReference& root) noexcept {
    return std::visit(
        [&root](const auto& identity) {
            using Identity = std::decay_t<decltype(identity)>;
            if constexpr(std::is_same_v<
                             Identity,
                             RepositoryRootPackageIdentity>) {
                return root.route_kind() ==
                       UnifiedPlanRootRouteKind::RepositoryTransaction;
            } else if constexpr(std::is_same_v<
                                    Identity,
                                    RepositorySourceBuildRootIdentity>) {
                return root.route_kind() ==
                       UnifiedPlanRootRouteKind::RepositorySourceBuild;
            } else if constexpr(std::is_same_v<
                                    Identity,
                                    AurRootPackageIdentity>) {
                return root.route_kind() ==
                       UnifiedPlanRootRouteKind::AurSourceBuild;
            } else {
                return root.route_kind() ==
                       UnifiedPlanRootRouteKind::LocalSourceBuild;
            }
        },
        root.source_identity());
}

std::string local_directory_identity_type_display(
    LocalSourceNodeType type, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    switch(type) {
        case LocalSourceNodeType::Directory:
            return localization::translate_message("directory");
        case LocalSourceNodeType::RegularFile:
            return unavailable_display(
                state, section, item_index, detail_index,
                localization::translate_message(
                    "A local source filesystem identity references a regular file instead of a directory."));
    }
    return unsupported_display(
        state, section, item_index, detail_index,
        localization::translate_message(
            "A local source filesystem identity has an unsupported node type."));
}

std::string root_identity_display(
    const UnifiedPlanRootReference& root, RenderState& state,
    std::size_t root_index,
    UnifiedPlanRenderingSection section =
        UnifiedPlanRenderingSection::Roots,
    std::optional<std::size_t> detail_index = std::nullopt) {
    return std::visit(
        [&](const auto& identity) -> std::string {
            using Identity = std::decay_t<decltype(identity)>;
            if constexpr(std::is_same_v<
                             Identity,
                             RepositoryRootPackageIdentity>) {
                return required_string_display(
                           identity.repository_name, state, section,
                           root_index, detail_index,
                           localization::translate_message(
                               "A repository root is missing its repository identity.")) +
                       "/" +
                       required_string_display(
                           identity.package_name, state, section,
                           root_index, detail_index,
                           localization::translate_message(
                               "A repository root is missing its package identity."));
            } else if constexpr(std::is_same_v<
                                    Identity,
                                    RepositorySourceBuildRootIdentity>) {
                return localization::format_translated_message(
                    "{} ({}: {}; source key: {})",
                    required_string_display(
                        identity.package_name, state, section,
                        root_index, detail_index,
                        localization::translate_message(
                            "A repository source root is missing its package identity.")),
                    "PackageBase",
                    required_string_display(
                        identity.package_base, state, section,
                        root_index, detail_index,
                        localization::format_translated_message(
                            "A repository source root is missing its {} identity.",
                            "PackageBase")),
                    required_string_display(
                        identity.canonical_source_identity_key,
                        state, section, root_index, detail_index,
                        localization::translate_message(
                            "A repository source root is missing its source identity key.")));
            } else if constexpr(std::is_same_v<
                                    Identity,
                                    AurRootPackageIdentity>) {
                return localization::format_translated_message(
                    "{}/{} ({}: {})", "AUR",
                    required_string_display(
                        identity.package_name, state, section,
                        root_index, detail_index,
                        localization::format_translated_message(
                            "An {} root is missing its package identity.",
                            "AUR")),
                    "PackageBase",
                    required_string_display(
                        identity.package_base, state, section,
                        root_index, detail_index,
                        localization::format_translated_message(
                            "An {} root is missing its {} identity.",
                            "AUR", "PackageBase")));
            } else {
                return localization::format_translated_message(
                    "{} (node type: {}; device: {}; inode: {})",
                    required_string_display(
                        identity.canonical_path.generic_string(),
                        state, section, root_index, detail_index,
                        localization::translate_message(
                            "A local root is missing its canonical path identity.")),
                    local_directory_identity_type_display(
                        identity.directory_identity.type, state,
                        section, root_index, detail_index),
                    identity.directory_identity.device,
                    identity.directory_identity.inode);
            }
        },
        root.source_identity());
}

std::string observation_phase_display(
    UnifiedPlanObservationPhase phase, RenderState& state,
    std::size_t phase_index) {
    switch(phase) {
        case UnifiedPlanObservationPhase::RequestDiscovery:
            return localization::translate_message("request / root discovery");
        case UnifiedPlanObservationPhase::MetadataDiscovery:
            return localization::translate_message(
                "metadata / dependency candidate discovery");
        case UnifiedPlanObservationPhase::ProviderDecision:
            return localization::translate_message(
                "provider decision / refreshed resolution");
        case UnifiedPlanObservationPhase::ExecutionProjection:
            return localization::translate_message(
                "execution preflight / projection");
        case UnifiedPlanObservationPhase::RepositoryTransaction:
            return localization::translate_message("repository transaction");
        case UnifiedPlanObservationPhase::SourceRetrieval:
            return localization::translate_message("source retrieval");
        case UnifiedPlanObservationPhase::SourceBuild:
            return localization::translate_message("source build");
        case UnifiedPlanObservationPhase::ArtifactValidation:
            return localization::translate_message(
                "artifact validation / selection");
        case UnifiedPlanObservationPhase::SourceArtifactInstall:
            return localization::translate_message(
                "source-built artifact install");
        case UnifiedPlanObservationPhase::CleanupReduction:
            return localization::translate_message("cleanup / reduction");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Phases, phase_index,
        std::nullopt,
        localization::translate_message(
            "An observation phase is not supported by the renderer."));
}

std::string authority_owner_display(
    UnifiedPlanAuthorityOwner owner, RenderState& state,
    std::size_t phase_index) {
    switch(owner) {
        case UnifiedPlanAuthorityOwner::Moguet:
            return "Moguet";
        case UnifiedPlanAuthorityOwner::Libalpm:
            return "libalpm";
        case UnifiedPlanAuthorityOwner::Pacman:
            return "pacman";
        case UnifiedPlanAuthorityOwner::AurRpc:
            return "AUR RPC";
        case UnifiedPlanAuthorityOwner::Git:
            return "Git";
        case UnifiedPlanAuthorityOwner::Makepkg:
            return "makepkg";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Phases, phase_index,
        std::nullopt,
        localization::translate_message(
            "A phase owner is not supported by the renderer."));
}

std::string system_source_phase_display(
    SystemSourceUpgradePhase phase, RenderState& state,
    std::size_t phase_index) {
    switch(phase) {
        case SystemSourceUpgradePhase::None:
            return localization::translate_message("none");
        case SystemSourceUpgradePhase::Preparation:
            return localization::translate_message("preparation");
        case SystemSourceUpgradePhase::System:
            return localization::translate_message("system");
        case SystemSourceUpgradePhase::RegisteredSource:
            return localization::translate_message("registered source");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Phases, phase_index,
        std::nullopt,
        localization::translate_message(
            "A system/source route phase is not supported by the renderer."));
}

std::string upgrade_all_phase_display(
    UpgradeAllOperationPhase phase, RenderState& state,
    std::size_t phase_index) {
    switch(phase) {
        case UpgradeAllOperationPhase::None:
            return localization::translate_message("none");
        case UpgradeAllOperationPhase::Preparation:
            return localization::translate_message("preparation");
        case UpgradeAllOperationPhase::System:
            return localization::translate_message("system");
        case UpgradeAllOperationPhase::RegisteredSource:
            return localization::translate_message("registered source");
        case UpgradeAllOperationPhase::ForeignInventory:
            return localization::translate_message("foreign package inventory");
        case UpgradeAllOperationPhase::AurQuery:
            return localization::format_translated_message("{} query", "AUR");
        case UpgradeAllOperationPhase::AurPreparation:
            return localization::format_translated_message(
                "{} preparation", "AUR");
        case UpgradeAllOperationPhase::AurExecution:
            return localization::format_translated_message(
                "{} execution", "AUR");
        case UpgradeAllOperationPhase::Reduction:
            return localization::translate_message("reduction");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Phases, phase_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} route phase is not supported by the renderer.",
            "upgrade-all"));
}

std::string existing_route_phase_display(
    const ExistingRoutePhaseReference& phase, RenderState& state,
    std::size_t phase_index) {
    return std::visit(
        [&](const auto& existing_phase) {
            using Phase = std::decay_t<decltype(existing_phase)>;
            if constexpr(std::is_same_v<Phase, SystemSourceUpgradePhase>) {
                return localization::format_translated_message(
                    "system/source: {}",
                    system_source_phase_display(
                        existing_phase, state, phase_index));
            } else {
                return localization::format_translated_message(
                    "{}: {}", "upgrade-all",
                    upgrade_all_phase_display(
                        existing_phase, state, phase_index));
            }
        },
        phase);
}

std::string dependency_kind_display(
    DependencyKind kind, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    switch(kind) {
        case DependencyKind::Installed:
            return localization::translate_message("installed");
        case DependencyKind::Repo:
            return localization::translate_message("repository");
        case DependencyKind::Aur:
            return "AUR";
        case DependencyKind::Local:
            return localization::translate_message("local");
        case DependencyKind::Provided:
            return localization::translate_message("provider");
        case DependencyKind::AmbiguousProvider:
            return localization::translate_message("ambiguous provider");
        case DependencyKind::Unknown:
            return localization::translate_message("unknown");
    }
    return unsupported_display(
        state, section, item_index, detail_index,
        localization::translate_message(
            "A dependency kind is not supported by the renderer."));
}

std::string package_role_display(
    PackageRole role, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    switch(role) {
        case PackageRole::Root:
            return localization::translate_message("root");
        case PackageRole::RuntimeDependency:
            return localization::translate_message("runtime dependency");
        case PackageRole::BuildDependency:
            return localization::translate_message("build dependency");
        case PackageRole::CheckDependency:
            return localization::translate_message("check dependency");
    }
    return unsupported_display(
        state, section, item_index, detail_index,
        localization::translate_message(
            "A package role is not supported by the renderer."));
}

std::string provider_identity_display(
    const ProvidedDependency& provider, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    if(const auto* repository =
           std::get_if<RepositoryProviderOrigin>(&provider.origin);
       repository != nullptr) {
        return required_string_display(
                   repository->repository_name, state, section,
                   item_index, detail_index,
                   localization::translate_message(
                       "A repository provider is missing its repository identity.")) +
               "/" +
               required_string_display(
                   provider.package_name, state, section, item_index,
                   detail_index,
                   localization::translate_message(
                       "A repository provider is missing its package identity."));
    }
    return localization::format_translated_message(
        "{}/{} ({}: {})", "AUR",
        required_string_display(
            provider.package_name, state, section, item_index,
            detail_index,
            localization::format_translated_message(
                "An {} provider is missing its package identity.",
                "AUR")),
        "PackageBase",
        required_string_display(
            provider.package_base, state, section, item_index,
            detail_index,
            localization::format_translated_message(
                "An {} provider is missing its {} identity.",
                "AUR", "PackageBase")));
}

std::string resolved_candidate_display(
    const ResolvedDependencyCandidate& candidate, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    return std::visit(
        [&](const auto& resolved) -> std::string {
            using Candidate = std::decay_t<decltype(resolved)>;
            if constexpr(std::is_same_v<Candidate, InstalledExactPackage>) {
                return localization::format_translated_message(
                    "installed/{}",
                    required_string_display(
                        resolved.package_name, state,
                        section, item_index, detail_index,
                        localization::translate_message(
                            "An installed dependency candidate is missing its package identity.")));
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    RepositoryExactPackage>) {
                return required_string_display(
                           resolved.repository.repository_name, state,
                           section, item_index, detail_index,
                           localization::translate_message(
                               "A repository dependency candidate is missing its repository identity.")) +
                       "/" +
                       required_string_display(
                           resolved.package_name, state,
                           section, item_index, detail_index,
                           localization::translate_message(
                               "A repository dependency candidate is missing its package identity."));
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    AurResolvedDependencyCandidate>) {
                return localization::format_translated_message(
                    "{}/{} ({}: {})", "AUR",
                    required_string_display(
                        resolved.package_name, state,
                        section, item_index, detail_index,
                        localization::format_translated_message(
                            "An {} dependency candidate is missing its package identity.",
                            "AUR")),
                    "PackageBase",
                    required_string_display(
                        resolved.package_base, state,
                        section, item_index, detail_index,
                        localization::format_translated_message(
                            "An {} dependency candidate is missing its {} identity.",
                            "AUR", "PackageBase")));
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    LocalResolvedDependencyCandidate>) {
                return localization::format_translated_message(
                    "local/{} ({}: {})",
                    required_string_display(
                        resolved.package_name, state,
                        section, item_index, detail_index,
                        localization::translate_message(
                            "A local dependency candidate is missing its package identity.")),
                    "PackageBase",
                    required_string_display(
                        resolved.package_base, state,
                        section, item_index, detail_index,
                        localization::format_translated_message(
                            "A local dependency candidate is missing its {} identity.",
                            "PackageBase")));
            } else {
                return provider_identity_display(
                    resolved.provider, state, section, item_index,
                    detail_index);
            }
        },
        candidate);
}

std::string constraint_evaluation_display(
    const ConstraintEvaluation& evaluation, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    try {
        return localization::format_translated_message(
            "{} ({})",
            constraint_satisfaction_display(evaluation.satisfaction()),
            constraint_evaluation_reason_display(evaluation));
    } catch(const std::exception& error) {
        return unsupported_display(
            state, section, item_index, detail_index,
            localization::format_translated_message(
                "A dependency constraint could not be rendered: {}",
                error.what()));
    }
}

std::string install_reason_display(
    DesiredInstallReason reason, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index = std::nullopt) {
    switch(reason) {
        case DesiredInstallReason::Explicit:
            return localization::translate_message("explicit");
        case DesiredInstallReason::Dependency:
            return localization::translate_message("dependency");
    }
    return unsupported_display(
        state, section, item_index, detail_index,
        localization::translate_message(
            "An install reason is not supported by the renderer."));
}

std::string source_build_kind_display(
    SourceBuildSourceKind kind, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index = std::nullopt) {
    switch(kind) {
        case SourceBuildSourceKind::Repository:
            return localization::translate_message("repository");
        case SourceBuildSourceKind::Aur:
            return "AUR";
    }
    return unsupported_display(
        state, section, item_index, detail_index,
        localization::translate_message(
            "A source-build kind is not supported by the renderer."));
}

void render_roots(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message("Roots:") << '\n';
    if(observation.roots().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0; index < observation.roots().size(); ++index) {
        const UnifiedPlanRootReference& root = observation.roots()[index];
        const RootTargetIdentity& correlation = root.invocation_correlation();
        const std::size_t identity_issue_count_before = state.issues.size();
        const std::string identity = root_identity_display(root, state, index);
        const std::string request = required_string_display(
            correlation.requested_name, state,
            UnifiedPlanRenderingSection::Roots, index, std::nullopt,
            localization::translate_message(
                "A root is missing its invocation request identity."));
        const bool needs_defensive_identity_fallback =
            !root.has_complete_identity() &&
            state.issues.size() == identity_issue_count_before;
        state.output << localization::format_translated_message(
                            "  {}. Identity: {}", index + 1,
                            identity)
                     << '\n';
        state.output << localization::format_translated_message(
                            "     Source: {}",
                            root_source_display(
                                root.source_kind(), state, index))
                     << '\n';
        state.output << localization::format_translated_message(
                            "     Route: {}",
                            root_route_display(
                                root.route_kind(), state, index))
                     << '\n';
        if(!root_route_matches_typed_identity(root)) {
            state.output << localization::format_translated_message(
                                "     Typed identity completeness: {}",
                                unavailable_display(
                                    state,
                                    UnifiedPlanRenderingSection::Roots,
                                    index, std::nullopt,
                                    localization::translate_message(
                                        "A root route does not match its typed source identity.")))
                         << '\n';
        } else if(needs_defensive_identity_fallback) {
            state.output << localization::format_translated_message(
                                "     Typed identity completeness: {}",
                                unavailable_display(
                                    state,
                                    UnifiedPlanRenderingSection::Roots,
                                    index, std::nullopt,
                                    localization::translate_message(
                                        "A root has an incomplete typed source identity that cannot be attributed to one display field.")))
                         << '\n';
        }
        state.output << localization::format_translated_message(
                            "     Request: {} (invocation index: {})",
                            request, correlation.invocation_index)
                     << '\n';
    }
}

void render_phases(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message("Phases:") << '\n';
    if(observation.phases().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0; index < observation.phases().size(); ++index) {
        const UnifiedPlanPhaseReference& phase = observation.phases()[index];
        state.output << localization::format_translated_message(
                            "  {}. {}", index + 1,
                            observation_phase_display(
                                phase.observation_phase, state,
                                index))
                     << '\n';
        state.output << localization::format_translated_message(
                            "     External owner: {}",
                            authority_owner_display(
                                phase.owner, state, index))
                     << '\n';
        if(phase.existing_route_phase.has_value()) {
            state.output << localization::format_translated_message(
                                "     Route phase: {}",
                                existing_route_phase_display(
                                    phase.existing_route_phase.value(),
                                    state, index))
                         << '\n';
        }
    }
}

void render_configured_repositories(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message(
                        "Configured repository order:")
                 << '\n';
    const UnifiedPlanConfiguredRepositoryOrderReference* repositories =
        observation.configured_repository_order();
    if(repositories == nullptr) {
        state.output << localization::translate_message("  Not observed")
                     << '\n';
        return;
    }
    if(repositories->configured_order().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0;
        index < repositories->configured_order().size(); ++index) {
        state.output << "  " << index + 1 << ". "
                     << terminal_safe_text_display(
                            repositories->configured_order()[index])
                     << '\n';
    }
}

void render_build_plan_dependencies(
    const BuildPlan& plan, std::size_t authority_index,
    RenderState& state) {
    if(plan.dependency_edges.empty()) {
        state.output << localization::translate_message(
                            "     Dependencies: None")
                     << '\n';
    } else {
        for(std::size_t edge_index = 0;
            edge_index < plan.dependency_edges.size(); ++edge_index) {
            const BuildPlanDependencyEdge& edge =
                plan.dependency_edges[edge_index];
            state.output << localization::format_translated_message(
                                "     {}. {} ({}: {}) -> {} [{}; role: {}]",
                                edge_index + 1,
                                required_string_display(
                                    edge.parent_package_name, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index,
                                    localization::translate_message(
                                        "A dependency edge is missing its parent package identity.")),
                                "PackageBase",
                                required_string_display(
                                    edge.parent_package_base, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index,
                                    localization::format_translated_message(
                                        "A dependency edge is missing its parent {} identity.",
                                        "PackageBase")),
                                required_string_display(
                                    edge.dependency_spec, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index,
                                    localization::translate_message(
                                        "A dependency edge is missing its dependency specification.")),
                                dependency_kind_display(
                                    edge.kind, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index),
                                package_role_display(
                                    edge.role, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index))
                         << '\n';
            if(edge.resolved_candidate.has_value()) {
                state.output << localization::format_translated_message(
                                    "        Selected identity: {}",
                                    resolved_candidate_display(
                                        edge.resolved_candidate.value(),
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index))
                             << '\n';
            } else if(edge.resolved_provider.has_value()) {
                state.output << localization::format_translated_message(
                                    "        Selected provider: {}",
                                    provider_identity_display(
                                        edge.resolved_provider.value(),
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index))
                             << '\n';
            } else if(edge.kind != DependencyKind::Unknown &&
                      edge.kind != DependencyKind::AmbiguousProvider) {
                state.output << localization::format_translated_message(
                                    "        Selected identity: {}",
                                    unavailable_display(
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index,
                                        localization::translate_message(
                                            "A resolved dependency edge is missing its selected candidate or provider identity.")))
                             << '\n';
            }
            if(edge.constraint_evaluation.has_value()) {
                state.output << localization::format_translated_message(
                                    "        Stored constraint result: {}",
                                    constraint_evaluation_display(
                                        edge.constraint_evaluation.value(),
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index))
                             << '\n';
            } else {
                state.output << localization::format_translated_message(
                                    "        Stored constraint result: {}",
                                    edge.resolved_candidate.has_value()
                                        ? unavailable_display(
                                              state,
                                              UnifiedPlanRenderingSection::
                                                  Dependencies,
                                              authority_index,
                                              edge_index,
                                              localization::translate_message(
                                                  "A resolved dependency edge is missing its stored constraint result."))
                                        : localization::translate_message(
                                              "not observed"))
                             << '\n';
            }
        }
    }

    if(plan.relation_assessments.empty()) {
        state.output << localization::translate_message(
                            "     Package relation assessments: None")
                     << '\n';
        return;
    }
    state.output << localization::translate_message(
                        "     Package relation assessments:")
                 << '\n';
    for(std::size_t relation_index = 0;
        relation_index < plan.relation_assessments.size(); ++relation_index) {
        state.output << localization::format_translated_message(
                            "       {}. {}", relation_index + 1,
                            terminal_safe_text_display(
                                package_relation_assessment_diagnostic_display(
                                    plan.relation_assessments
                                        [relation_index])))
                     << '\n';
    }
}

std::string local_dependency_plan_failure_display(
    const LocalDependencyPlanFailure& failure, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index);

void render_local_dependency_details(
    const LocalBuildPlan& plan, std::size_t authority_index,
    RenderState& state) {
    state.output << localization::format_translated_message(
                        "     Effective architecture: {}",
                        required_string_display(
                            plan.effective_architecture(), state,
                            UnifiedPlanRenderingSection::Dependencies,
                            authority_index, std::nullopt,
                            localization::format_translated_message(
                                "A {} is missing its effective architecture.",
                                "LocalBuildPlan")))
                 << '\n';
    if(plan.internal_edges().empty()) {
        state.output << localization::translate_message(
                            "     Local internal dependencies: None")
                     << '\n';
    } else {
        state.output << localization::translate_message(
                            "     Local internal dependencies:")
                     << '\n';
        for(std::size_t edge_index = 0;
            edge_index < plan.internal_edges().size(); ++edge_index) {
            const LocalDependencyPlanInternalEdge& edge =
                plan.internal_edges()[edge_index];
            state.output << localization::format_translated_message(
                                "       {}. {} -> {} (resolved package: {}; role: {}; cycle: {})",
                                edge_index + 1,
                                required_string_display(
                                    edge.parent_package_name, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index,
                                    localization::translate_message(
                                        "A local dependency edge is missing its parent package identity.")),
                                required_string_display(
                                    edge.dependency_specification,
                                    state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index,
                                    localization::translate_message(
                                        "A local dependency edge is missing its dependency specification.")),
                                required_string_display(
                                    edge.resolved_package_name, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index,
                                    localization::translate_message(
                                        "A local dependency edge is missing its resolved package identity.")),
                                package_role_display(
                                    edge.role, state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, edge_index),
                                yes_no_display(edge.is_cycle))
                         << '\n';
            if(edge.provided_specification.has_value()) {
                state.output << localization::format_translated_message(
                                    "          Provided capability: {}",
                                    terminal_safe_text_display(
                                        edge.provided_specification.value()))
                             << '\n';
            }
            if(edge.resolved_candidate.has_value()) {
                state.output << localization::format_translated_message(
                                    "          Selected identity: {}",
                                    resolved_candidate_display(
                                        ResolvedDependencyCandidate{
                                            edge.resolved_candidate
                                                .value()},
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index))
                             << '\n';
            } else {
                state.output << localization::format_translated_message(
                                    "          Selected identity: {}",
                                    unavailable_display(
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index,
                                        localization::translate_message(
                                            "A local dependency edge is missing its selected candidate identity.")))
                             << '\n';
            }
            if(edge.constraint_evaluation.has_value()) {
                state.output << localization::format_translated_message(
                                    "          Stored constraint result: {}",
                                    constraint_evaluation_display(
                                        edge.constraint_evaluation
                                            .value(),
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index))
                             << '\n';
            } else {
                state.output << localization::format_translated_message(
                                    "          Stored constraint result: {}",
                                    unavailable_display(
                                        state,
                                        UnifiedPlanRenderingSection::
                                            Dependencies,
                                        authority_index, edge_index,
                                        localization::translate_message(
                                            "A local dependency edge is missing its stored constraint result.")))
                             << '\n';
            }
        }
    }
    if(plan.failures().empty()) {
        state.output << localization::translate_message(
                            "     Local dependency failures: None")
                     << '\n';
    } else {
        state.output << localization::translate_message(
                            "     Local dependency failures:")
                     << '\n';
        for(std::size_t failure_index = 0;
            failure_index < plan.failures().size(); ++failure_index) {
            state.output << localization::format_translated_message(
                                "       {}. {}", failure_index + 1,
                                local_dependency_plan_failure_display(
                                    plan.failures()[failure_index],
                                    state,
                                    UnifiedPlanRenderingSection::
                                        Dependencies,
                                    authority_index, failure_index))
                         << '\n';
        }
    }
}

void render_dependencies(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message(
                        "Dependency authorities:")
                 << '\n';
    if(observation.dependency_authorities().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0;
        index < observation.dependency_authorities().size(); ++index) {
        const UnifiedPlanDependencyAuthorityReference& authority =
            observation.dependency_authorities()[index];
        if(const BuildPlan* plan = authority.build_plan(); plan != nullptr) {
            state.output << localization::format_translated_message(
                                "  {}. {}", index + 1, "BuildPlan")
                         << '\n';
            render_build_plan_dependencies(*plan, index, state);
            continue;
        }
        if(const LocalBuildPlan* local_plan = authority.local_build_plan();
           local_plan != nullptr) {
            state.output << localization::format_translated_message(
                                "  {}. {}", index + 1,
                                "LocalBuildPlan")
                         << '\n';
            render_build_plan_dependencies(
                local_plan->build_plan(), index, state);
            render_local_dependency_details(*local_plan, index, state);
            continue;
        }
        state.output << localization::format_translated_message(
                            "  {}. {}", index + 1,
                            unavailable_display(
                                state,
                                UnifiedPlanRenderingSection::
                                    Dependencies,
                                index, std::nullopt,
                                localization::translate_message(
                                    "A dependency authority reference is unavailable.")))
                     << '\n';
    }
}

std::string build_unit_identity_display(
    const UnifiedPlanBuildUnitReference& build_unit, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index = std::nullopt,
    bool defer_defensive_completeness = false) {
    return std::visit(
        [&](const auto& unit) -> std::string {
            using Unit = std::decay_t<decltype(unit)>;
            const std::size_t issue_count_before = state.issues.size();
            std::string display;
            if constexpr(std::is_same_v<
                             Unit,
                             AurPackageBaseBuildUnitReference>) {
                const BuildPlanEntry* entry = unit.entry();
                const std::string package_base =
                    entry == nullptr
                        ? unavailable_display(
                              state, section, item_index, detail_index,
                              localization::format_translated_message(
                                  "An {} build unit no longer references a {} entry.",
                                  "AUR", "BuildPlan"))
                        : required_string_display(
                              entry->package_base, state, section,
                              item_index, detail_index,
                              localization::format_translated_message(
                                  "An {} build unit is missing its {} identity.",
                                  "AUR", "PackageBase"));
                display = localization::format_translated_message(
                    "{} {} unit #{} ({}: {})", "AUR", "BuildPlan",
                    unit.build_plan_order_index() + 1, "PackageBase",
                    package_base);
            } else if constexpr(std::is_same_v<
                                    Unit,
                                    LocalSourceBuildUnitReference>) {
                const LocalSourceRootObservationIdentity& source =
                    unit.source_root();
                display = localization::format_translated_message(
                    "local source {} (node type: {}; device: {}; inode: {}; {}: {})",
                    required_string_display(
                        source.canonical_path.generic_string(),
                        state, section, item_index, detail_index,
                        localization::translate_message(
                            "A local build unit is missing its canonical path identity.")),
                    local_directory_identity_type_display(
                        source.directory_identity.type, state,
                        section, item_index, detail_index),
                    source.directory_identity.device,
                    source.directory_identity.inode, "PackageBase",
                    required_string_display(
                        unit.metadata().package_base, state,
                        section, item_index, detail_index,
                        localization::format_translated_message(
                            "A local build unit is missing its {} identity.",
                            "PackageBase")));
            } else if constexpr(std::is_same_v<
                                    Unit,
                                    PreparedRemoteSourceBuildUnitReference>) {
                display = localization::format_translated_message(
                    "{} source key {} (requested package: {}; checkout {}: {})",
                    source_build_kind_display(
                        unit.source().source_kind(), state,
                        section, item_index, detail_index),
                    required_string_display(
                        unit.source().canonical_source_key(),
                        state, section, item_index,
                        detail_index,
                        localization::translate_message(
                            "A remote source build unit is missing its source identity key.")),
                    required_string_display(
                        unit.source().requested_name(), state,
                        section, item_index, detail_index,
                        localization::translate_message(
                            "A remote source build unit is missing its requested package identity.")),
                    "PackageBase",
                    required_string_display(
                        unit.source().package_base(), state,
                        section, item_index, detail_index,
                        localization::format_translated_message(
                            "A remote source build unit is missing its checkout {} identity.",
                            "PackageBase")));
            } else {
                const RegisteredSourcePreferenceSnapshot& source =
                    unit.source();
                const std::string source_kind =
                    source.source_kind.has_value()
                        ? source_build_kind_display(
                              source.source_kind.value(), state,
                              section, item_index, detail_index)
                        : unavailable_display(
                              state, section, item_index,
                              detail_index,
                              localization::translate_message(
                                  "A prepared source build unit is missing its source kind."));
                const std::string source_key =
                    source.canonical_source_identity_key.has_value() &&
                            !source.canonical_source_identity_key
                                 ->empty()
                        ? terminal_safe_text_display(
                              source.canonical_source_identity_key.value())
                        : unavailable_display(
                              state, section, item_index,
                              detail_index,
                              localization::translate_message(
                                  "A prepared source build unit is missing its source identity key."));
                display = localization::format_translated_message(
                    "{} source key {} (requested package: {}; checkout {}: {})",
                    source_kind, source_key,
                    required_string_display(
                        unit.requested_package_name(), state,
                        section, item_index, detail_index,
                        localization::translate_message(
                            "A prepared source build unit is missing its requested package identity.")),
                    "PackageBase",
                    required_string_display(
                        unit.checkout_package_base(), state,
                        section, item_index, detail_index,
                        localization::format_translated_message(
                            "A prepared source build unit is missing its checkout {} identity.",
                            "PackageBase")));
            }
            if(!defer_defensive_completeness &&
               !unit.has_complete_identity() &&
               state.issues.size() == issue_count_before) {
                state.add_issue(
                    UnifiedPlanRenderingIssueKind::
                        MissingReferencedValue,
                    section, item_index, detail_index,
                    localization::translate_message(
                        "A build unit has an incomplete typed source identity that cannot be attributed to one display field."));
            }
            return display;
        },
        build_unit);
}

void render_build_units(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message("Build units:") << '\n';
    if(observation.build_units().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0; index < observation.build_units().size();
        ++index) {
        const UnifiedPlanBuildUnitReference& build_unit =
            observation.build_units()[index];
        const std::size_t issue_count_before = state.issues.size();
        state.output << localization::format_translated_message(
                            "  {}. {}", index + 1,
                            build_unit_identity_display(
                                build_unit, state,
                                UnifiedPlanRenderingSection::
                                    BuildUnits,
                                index, std::nullopt, true))
                     << '\n';
        std::visit(
            [&](const auto& unit) {
                using Unit = std::decay_t<decltype(unit)>;
                if constexpr(std::is_same_v<
                                 Unit,
                                 AurPackageBaseBuildUnitReference>) {
                    const BuildPlanEntry* entry = unit.entry();
                    if(entry == nullptr) {
                        return;
                    }
                    const std::string children =
                        entry->package_names.empty()
                            ? localization::translate_message(
                                  "unavailable")
                            : join_display_values(entry->package_names);
                    state.output << localization::format_translated_message(
                                        "     Child packages: {}",
                                        children)
                                 << '\n';
                } else if constexpr(std::is_same_v<
                                        Unit,
                                        LocalSourceBuildUnitReference>) {
                    const LocalPackageMetadata& metadata = unit.metadata();
                    std::vector<std::string> children;
                    children.reserve(metadata.children.size());
                    for(const LocalPackageMetadataChild& child :
                        metadata.children) {
                        children.push_back(child.name);
                    }
                    state.output << localization::format_translated_message(
                                        "     Child packages: {}",
                                        children.empty()
                                            ? localization::
                                                  translate_message(
                                                      "unavailable")
                                            : join_display_values(
                                                  children))
                                 << '\n';
                } else if constexpr(std::is_same_v<
                                        Unit,
                                        PreparedRemoteSourceBuildUnitReference>) {
                    for(std::size_t target_index = 0;
                        target_index < unit.required_targets().size();
                        ++target_index) {
                        const RequiredPackageArtifactTarget& target =
                            unit.required_targets()[target_index];
                        state.output << localization::format_translated_message(
                                            "     Required target {}: {}/{} ({})",
                                            target_index + 1,
                                            target.package_base,
                                            target.package_name,
                                            install_reason_display(
                                                target.desired_reason,
                                                state,
                                                UnifiedPlanRenderingSection::
                                                    BuildUnits,
                                                index,
                                                target_index))
                                     << '\n';
                    }
                } else {
                    state.output << localization::format_translated_message(
                                        "     Source preference: {}",
                                        required_string_display(
                                            unit.source()
                                                .preference_package_name,
                                            state,
                                            UnifiedPlanRenderingSection::
                                                BuildUnits,
                                            index, std::nullopt,
                                            localization::
                                                translate_message(
                                                    "A prepared source build unit is missing its source preference identity.")))
                                 << '\n';
                    for(std::size_t target_index = 0;
                        target_index < unit.required_targets().size();
                        ++target_index) {
                        const RequiredPackageArtifactTarget& target =
                            unit.required_targets()[target_index];
                        state.output << localization::format_translated_message(
                                            "     Required target {}: {}/{} ({})",
                                            target_index + 1,
                                            target.package_base,
                                            target.package_name,
                                            install_reason_display(
                                                target.desired_reason,
                                                state,
                                                UnifiedPlanRenderingSection::
                                                    BuildUnits,
                                                index,
                                                target_index))
                                     << '\n';
                    }
                }
            },
            build_unit);
        if(!std::visit(
               [](const auto& unit) {
                   return unit.has_complete_identity();
               },
               build_unit) &&
           state.issues.size() == issue_count_before) {
            state.add_issue(
                UnifiedPlanRenderingIssueKind::MissingReferencedValue,
                UnifiedPlanRenderingSection::BuildUnits, index,
                std::nullopt,
                localization::translate_message(
                    "A build unit has an incomplete typed source identity that cannot be attributed to one display field."));
        }
    }
}

void render_required_artifacts(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message(
                        "Required artifact targets:")
                 << '\n';
    if(observation.required_artifacts().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0;
        index < observation.required_artifacts().size(); ++index) {
        const RequiredArtifactTargetReference& artifact =
            observation.required_artifacts()[index];
        const RequiredPackageArtifactTarget target =
            artifact.target();
        state.output << localization::format_translated_message(
                            "  {}. Source/build unit: {}",
                            index + 1,
                            build_unit_identity_display(
                                artifact.build_unit(), state,
                                UnifiedPlanRenderingSection::
                                    RequiredArtifacts,
                                index))
                     << '\n';
        state.output << localization::format_translated_message(
                            "     Required target: {}: {}; child package: {}; install reason: {}",
                            "PackageBase", target.package_base,
                            target.package_name,
                            install_reason_display(
                                target.desired_reason, state,
                                UnifiedPlanRenderingSection::
                                    RequiredArtifacts,
                                index))
                     << '\n';
    }
}

void render_repository_transaction_target(
    const RepositoryInstallIntentTarget& target,
    std::size_t intent_index, std::size_t target_index,
    RenderState& state) {
    state.output << "     - ";
    std::visit(
        [&](const auto& typed_target) {
            using Target = std::decay_t<decltype(typed_target)>;
            if constexpr(std::is_same_v<
                             Target,
                             RepositoryRootInstallIntent>) {
                state.output << localization::format_translated_message(
                    "root: {}",
                    root_identity_display(
                        typed_target.root, state, intent_index,
                        UnifiedPlanRenderingSection::
                            TransactionIntents,
                        target_index));
            } else if constexpr(std::is_same_v<
                                    Target,
                                    RepositoryDependencyInstallIntent>) {
                const RepositoryExactPackage& package =
                    typed_target.package.get();
                state.output << localization::format_translated_message(
                    "dependency: {}/{}",
                    required_string_display(
                        package.repository.repository_name, state,
                        UnifiedPlanRenderingSection::
                            TransactionIntents,
                        intent_index, target_index,
                        localization::translate_message(
                            "A repository transaction dependency is missing its repository identity.")),
                    required_string_display(
                        package.package_name, state,
                        UnifiedPlanRenderingSection::
                            TransactionIntents,
                        intent_index, target_index,
                        localization::translate_message(
                            "A repository transaction dependency is missing its package identity.")));
            } else if constexpr(std::is_same_v<
                                    Target,
                                    RepositoryProviderInstallIntent>) {
                state.output << localization::format_translated_message(
                    "selected provider: {}",
                    provider_identity_display(
                        typed_target.provider.get(), state,
                        UnifiedPlanRenderingSection::
                            TransactionIntents,
                        intent_index, target_index));
            } else {
                state.output << localization::translate_message(
                    "system upgrade");
            }
        },
        target);
    state.output << '\n';
}

void render_source_transaction_target(
    const SourceArtifactInstallIntentTarget& target,
    const UnifiedPlanObservation& observation,
    std::size_t intent_index, std::size_t target_index,
    RenderState& state) {
    const std::size_t artifact_index = std::visit(
        [](const auto& typed_target) {
            return typed_target.required_artifact_index;
        },
        target);
    state.output << "     - ";
    const bool is_root =
        std::holds_alternative<SourceRootArtifactInstallIntent>(target);
    if(artifact_index >= observation.required_artifacts().size()) {
        state.output << unavailable_display(
            state, UnifiedPlanRenderingSection::TransactionIntents,
            intent_index, target_index,
            localization::translate_message(
                "A source install intent references an unavailable required artifact."));
        state.output << '\n';
        return;
    }
    const RequiredArtifactTargetReference& artifact_reference =
        observation.required_artifacts()[artifact_index];
    const RequiredPackageArtifactTarget artifact =
        artifact_reference.target();
    state.output << localization::format_translated_message(
        "{} required artifact #{}: {}; target: {}/{}; install reason: {}",
        is_root ? localization::translate_message("root")
                : localization::translate_message("dependency"),
        artifact_index + 1,
        build_unit_identity_display(
            artifact_reference.build_unit(), state,
            UnifiedPlanRenderingSection::TransactionIntents,
            intent_index, target_index),
        artifact.package_base, artifact.package_name,
        install_reason_display(
            artifact.desired_reason, state,
            UnifiedPlanRenderingSection::TransactionIntents,
            intent_index, target_index));
    state.output << '\n';
}

std::string repository_transaction_intent_display(
    UnifiedPlanTransactionIntentStage stage, std::size_t intent_index,
    RenderState& state) {
    switch(stage) {
        case UnifiedPlanTransactionIntentStage::RouteOwned:
            return localization::translate_message(
                "Repository package transaction");
        case UnifiedPlanTransactionIntentStage::RepositorySystemUpgrade:
            return localization::translate_message(
                "Repository system transaction intent");
        case UnifiedPlanTransactionIntentStage::LaterNormalAur:
            return localization::format_translated_message(
                "Later normal {} dependency/provider transaction intent",
                "AUR");
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::TransactionIntents,
        intent_index, std::nullopt,
        localization::translate_message(
            "A repository transaction intent stage is not supported by the renderer."));
}

std::string source_transaction_intent_display(
    UnifiedPlanTransactionIntentStage stage, std::size_t intent_index,
    RenderState& state) {
    switch(stage) {
        case UnifiedPlanTransactionIntentStage::RouteOwned:
            return localization::translate_message(
                "Source-built artifact install boundary");
        case UnifiedPlanTransactionIntentStage::LaterNormalAur:
            return localization::format_translated_message(
                "Later normal {} build/install transaction intent", "AUR");
        case UnifiedPlanTransactionIntentStage::RepositorySystemUpgrade:
            break;
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::TransactionIntents,
        intent_index, std::nullopt,
        localization::translate_message(
            "A source transaction intent stage is not supported by the renderer."));
}

void render_transaction_intents(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message(
                        "Transaction intents:")
                 << '\n';
    if(observation.transaction_intents().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t intent_index = 0;
        intent_index < observation.transaction_intents().size();
        ++intent_index) {
        std::visit(
            [&](const auto& intent) {
                using Intent = std::decay_t<decltype(intent)>;
                if constexpr(std::is_same_v<
                                 Intent,
                                 RepositoryPackageTransactionIntent>) {
                    state.output << localization::format_translated_message(
                                        "  {}. {} ({} policy: {})",
                                        intent_index + 1,
                                        repository_transaction_intent_display(
                                            intent.stage, intent_index,
                                            state),
                                        "pacman --needed",
                                        yes_no_display(
                                            intent.policy.needed))
                                 << '\n';
                    for(std::size_t target_index = 0;
                        target_index < intent.targets.size();
                        ++target_index) {
                        render_repository_transaction_target(
                            intent.targets[target_index], intent_index,
                            target_index, state);
                    }
                } else {
                    state.output << localization::format_translated_message(
                                        "  {}. {} ({} policy: {})",
                                        intent_index + 1,
                                        source_transaction_intent_display(
                                            intent.stage, intent_index,
                                            state),
                                        "pacman --needed",
                                        yes_no_display(intent.needed))
                                 << '\n';
                    for(std::size_t target_index = 0;
                        target_index < intent.targets.size();
                        ++target_index) {
                        render_source_transaction_target(
                            intent.targets[target_index], observation,
                            intent_index, target_index, state);
                    }
                }
            },
            observation.transaction_intents()[intent_index]);
    }
}

std::string build_plan_resolution_failure_kind_display(
    BuildPlanResolutionFailureKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case BuildPlanResolutionFailureKind::InstalledPackageMetadataUnavailable:
            return "BuildPlanResolutionFailureKind::InstalledPackageMetadataUnavailable";
        case BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable:
            return "BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable";
        case BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable:
            return "BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable";
        case BuildPlanResolutionFailureKind::ProviderSearchUnavailable:
            return "BuildPlanResolutionFailureKind::ProviderSearchUnavailable";
        case BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable:
            return "BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "A {} resolution failure kind is not supported by the renderer.",
            "BuildPlan"));
}

std::string observed_version_unknown_reason_display(
    ObservedVersionUnknownReason reason, std::size_t blocker_index,
    RenderState& state) {
    switch(reason) {
        case ObservedVersionUnknownReason::MissingVersionMetadata:
            return "ObservedVersionUnknownReason::MissingVersionMetadata";
        case ObservedVersionUnknownReason::UnversionedProviderCapability:
            return "ObservedVersionUnknownReason::UnversionedProviderCapability";
        case ObservedVersionUnknownReason::MetadataQueryFailure:
            return "ObservedVersionUnknownReason::MetadataQueryFailure";
        case ObservedVersionUnknownReason::PartialSourceFailure:
            return "ObservedVersionUnknownReason::PartialSourceFailure";
        case ObservedVersionUnknownReason::ComparisonAuthorityUnavailable:
            return "ObservedVersionUnknownReason::ComparisonAuthorityUnavailable";
        case ObservedVersionUnknownReason::CandidateVersionUnavailable:
            return "ObservedVersionUnknownReason::CandidateVersionUnavailable";
        case ObservedVersionUnknownReason::RelationKindNotComparable:
            return "ObservedVersionUnknownReason::RelationKindNotComparable";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "An observed-version unknown reason is not supported by the renderer."));
}

std::string package_metadata_error_code_display(
    PackageMetadataErrorCode code, std::size_t blocker_index,
    RenderState& state) {
    switch(code) {
        case PackageMetadataErrorCode::ConfigurationUnavailable:
            return "PackageMetadataErrorCode::ConfigurationUnavailable";
        case PackageMetadataErrorCode::ConfigurationMalformed:
            return "PackageMetadataErrorCode::ConfigurationMalformed";
        case PackageMetadataErrorCode::InitializationFailed:
            return "PackageMetadataErrorCode::InitializationFailed";
        case PackageMetadataErrorCode::LocalDatabaseUnavailable:
            return "PackageMetadataErrorCode::LocalDatabaseUnavailable";
        case PackageMetadataErrorCode::InvalidPackageName:
            return "PackageMetadataErrorCode::InvalidPackageName";
        case PackageMetadataErrorCode::QueryFailed:
            return "PackageMetadataErrorCode::QueryFailed";
        case PackageMetadataErrorCode::MalformedMetadata:
            return "PackageMetadataErrorCode::MalformedMetadata";
        case PackageMetadataErrorCode::SyncDatabaseUnavailable:
            return "PackageMetadataErrorCode::SyncDatabaseUnavailable";
        case PackageMetadataErrorCode::RepositoryNotConfigured:
            return "PackageMetadataErrorCode::RepositoryNotConfigured";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A package metadata error code is not supported by the renderer."));
}

std::string dependency_constraint_parse_failure_kind_display(
    DependencyConstraintParseFailureKind kind,
    std::size_t blocker_index, RenderState& state) {
    switch(kind) {
        case DependencyConstraintParseFailureKind::EmptySpecification:
            return "DependencyConstraintParseFailureKind::EmptySpecification";
        case DependencyConstraintParseFailureKind::InvalidPackageIdentity:
            return "DependencyConstraintParseFailureKind::InvalidPackageIdentity";
        case DependencyConstraintParseFailureKind::InvalidSonameIdentity:
            return "DependencyConstraintParseFailureKind::InvalidSonameIdentity";
        case DependencyConstraintParseFailureKind::UnsupportedConsumerOperator:
            return "DependencyConstraintParseFailureKind::UnsupportedConsumerOperator";
        case DependencyConstraintParseFailureKind::UnsupportedProviderOperator:
            return "DependencyConstraintParseFailureKind::UnsupportedProviderOperator";
        case DependencyConstraintParseFailureKind::MissingVersion:
            return "DependencyConstraintParseFailureKind::MissingVersion";
        case DependencyConstraintParseFailureKind::InvalidVersion:
            return "DependencyConstraintParseFailureKind::InvalidVersion";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A dependency constraint parse failure kind is not supported by the renderer."));
}

std::string repository_source_failure_reason_display(
    const RepositoryExactPackageSourceFailureReason& reason,
    std::size_t blocker_index, RenderState& state) {
    return std::visit(
        [&](const auto& detail) {
            using Detail = std::decay_t<decltype(detail)>;
            if constexpr(std::is_same_v<Detail, PackageMetadataFailure>) {
                return localization::format_translated_message(
                    "{}; diagnostic: {}",
                    package_metadata_error_code_display(
                        detail.code, blocker_index, state),
                    required_string_display(
                        detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A repository source failure is missing its metadata diagnostic.")));
            } else {
                return localization::format_translated_message(
                    "{}; specification: {}",
                    dependency_constraint_parse_failure_kind_display(
                        detail.kind, blocker_index, state),
                    required_string_display(
                        detail.raw_specification, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A repository source failure is missing its constraint specification.")));
            }
        },
        reason);
}

std::string local_source_root_stage_display(
    LocalSourceRootStage stage, std::size_t blocker_index,
    RenderState& state) {
    switch(stage) {
        case LocalSourceRootStage::InvocationAnchorOpen:
            return "LocalSourceRootStage::InvocationAnchorOpen";
        case LocalSourceRootStage::RootInspection:
            return "LocalSourceRootStage::RootInspection";
        case LocalSourceRootStage::RootOpen:
            return "LocalSourceRootStage::RootOpen";
        case LocalSourceRootStage::CanonicalPathResolution:
            return "LocalSourceRootStage::CanonicalPathResolution";
        case LocalSourceRootStage::RootRevalidation:
            return "LocalSourceRootStage::RootRevalidation";
        case LocalSourceRootStage::PkgbuildInspection:
            return "LocalSourceRootStage::PkgbuildInspection";
        case LocalSourceRootStage::PkgbuildOpen:
            return "LocalSourceRootStage::PkgbuildOpen";
        case LocalSourceRootStage::PkgbuildRead:
            return "LocalSourceRootStage::PkgbuildRead";
        case LocalSourceRootStage::PkgbuildRevalidation:
            return "LocalSourceRootStage::PkgbuildRevalidation";
        case LocalSourceRootStage::SrcinfoInspection:
            return "LocalSourceRootStage::SrcinfoInspection";
        case LocalSourceRootStage::SrcinfoOpen:
            return "LocalSourceRootStage::SrcinfoOpen";
        case LocalSourceRootStage::SrcinfoRead:
            return "LocalSourceRootStage::SrcinfoRead";
        case LocalSourceRootStage::SrcinfoRevalidation:
            return "LocalSourceRootStage::SrcinfoRevalidation";
        case LocalSourceRootStage::MetadataRevalidation:
            return "LocalSourceRootStage::MetadataRevalidation";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A local source failure stage is not supported by the renderer."));
}

std::string local_source_root_error_code_display(
    LocalSourceRootErrorCode code, std::size_t blocker_index,
    RenderState& state) {
    switch(code) {
        case LocalSourceRootErrorCode::InvalidInputPath:
            return "LocalSourceRootErrorCode::InvalidInputPath";
        case LocalSourceRootErrorCode::Missing:
            return "LocalSourceRootErrorCode::Missing";
        case LocalSourceRootErrorCode::Symlink:
            return "LocalSourceRootErrorCode::Symlink";
        case LocalSourceRootErrorCode::NotDirectory:
            return "LocalSourceRootErrorCode::NotDirectory";
        case LocalSourceRootErrorCode::NotRegularFile:
            return "LocalSourceRootErrorCode::NotRegularFile";
        case LocalSourceRootErrorCode::OwnershipMismatch:
            return "LocalSourceRootErrorCode::OwnershipMismatch";
        case LocalSourceRootErrorCode::UnsafePermissions:
            return "LocalSourceRootErrorCode::UnsafePermissions";
        case LocalSourceRootErrorCode::PermissionDenied:
            return "LocalSourceRootErrorCode::PermissionDenied";
        case LocalSourceRootErrorCode::MetadataFailure:
            return "LocalSourceRootErrorCode::MetadataFailure";
        case LocalSourceRootErrorCode::ReadFailure:
            return "LocalSourceRootErrorCode::ReadFailure";
        case LocalSourceRootErrorCode::ConcurrentReplacement:
            return "LocalSourceRootErrorCode::ConcurrentReplacement";
        case LocalSourceRootErrorCode::ContentChanged:
            return "LocalSourceRootErrorCode::ContentChanged";
        case LocalSourceRootErrorCode::UnsafeMetadata:
            return "LocalSourceRootErrorCode::UnsafeMetadata";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A local source failure code is not supported by the renderer."));
}

std::string local_source_metadata_state_display(
    LocalSourceMetadataState metadata_state, std::size_t blocker_index,
    RenderState& state) {
    switch(metadata_state) {
        case LocalSourceMetadataState::Missing:
            return "LocalSourceMetadataState::Missing";
        case LocalSourceMetadataState::Unsafe:
            return "LocalSourceMetadataState::Unsafe";
        case LocalSourceMetadataState::Invalid:
            return "LocalSourceMetadataState::Invalid";
        case LocalSourceMetadataState::UsableUnverified:
            return "LocalSourceMetadataState::UsableUnverified";
        case LocalSourceMetadataState::KnownStale:
            return "LocalSourceMetadataState::KnownStale";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A local source metadata state is not supported by the renderer."));
}

std::string local_source_metadata_stale_reason_display(
    LocalSourceMetadataStaleReason reason, std::size_t blocker_index,
    RenderState& state) {
    switch(reason) {
        case LocalSourceMetadataStaleReason::PkgbuildNewer:
            return "LocalSourceMetadataStaleReason::PkgbuildNewer";
        case LocalSourceMetadataStaleReason::OneOffEnvironmentAssignment:
            return "LocalSourceMetadataStaleReason::OneOffEnvironmentAssignment";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A local source metadata stale reason is not supported by the renderer."));
}

std::string local_source_metadata_evaluation_blocker_display(
    const LocalSourceMetadataEvaluationUnifiedPlanBlocker& blocker,
    std::size_t blocker_index, RenderState& state) {
    std::vector<std::string> stale_reasons;
    for(const LocalSourceMetadataStaleReason reason :
        blocker.detail.get().stale_reasons()) {
        stale_reasons.push_back(local_source_metadata_stale_reason_display(
            reason, blocker_index, state));
    }
    return localization::format_translated_message(
        "local metadata evaluation required ({}); source: {}; stale reasons: {}",
        local_source_metadata_state_display(
            blocker.detail.get().state(), blocker_index, state),
        required_string_display(
            blocker.source_root.canonical_path.generic_string(),
            state, UnifiedPlanRenderingSection::Blockers,
            blocker_index, std::nullopt,
            localization::translate_message(
                "A local metadata blocker is missing its source identity.")),
        stale_reasons.empty()
            ? localization::translate_message("None")
            : join_display_values(stale_reasons));
}

std::string root_target_identity_display(const RootTargetIdentity& root) {
    return localization::format_translated_message(
        "{} (invocation index: {})", root.requested_name,
        root.invocation_index);
}

std::string source_failure_detail_display(
    const SourceFailureUnifiedPlanBlocker& blocker,
    std::size_t blocker_index, RenderState& state) {
    return std::visit(
        [&](const auto& reference) -> std::string {
            using Reference = std::decay_t<decltype(reference)>;
            const auto& detail = reference.get();
            if constexpr(std::is_same_v<
                             Reference,
                             UnifiedPlanBorrowedAuthorityReference<
                                 BuildPlanResolutionFailure>>) {
                return localization::format_translated_message(
                    "source authority failure ({}); subject: {}; parent: {}; {}: {}; dependency: {}; roots: {}; diagnostic: {}",
                    build_plan_resolution_failure_kind_display(
                        detail.kind, blocker_index, state),
                    required_string_display(
                        detail.subject, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "A {} resolution failure is missing its subject.",
                            "BuildPlan")),
                    optional_string_display(
                        detail.parent_package_name),
                    "PackageBase",
                    optional_string_display(
                        detail.parent_package_base),
                    optional_string_display(
                        detail.dependency_specification),
                    detail.roots.empty()
                        ? localization::translate_message("None")
                        : join_display_values([&detail] {
                              std::vector<std::string> roots;
                              roots.reserve(detail.roots.size());
                              for(const RootTargetIdentity& root :
                                  detail.roots) {
                                  roots.push_back(
                                      root_target_identity_display(
                                          root));
                              }
                              return roots;
                          }()),
                    required_string_display(
                        detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "A {} resolution failure is missing its diagnostic.",
                            "BuildPlan")));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        IncompleteProviderCandidateSet>>) {
                std::vector<std::string> candidates;
                candidates.reserve(detail.observed_candidates.size());
                for(const ProvidedDependency& candidate :
                    detail.observed_candidates) {
                    candidates.push_back(provider_identity_display(
                        candidate, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt));
                }
                return localization::format_translated_message(
                    "source authority failure ({}); dependency: {}; observed candidates: {}; reason: {}",
                    "IncompleteProviderCandidateSet",
                    required_string_display(
                        detail.dependency, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "An incomplete provider set is missing its dependency identity.")),
                    candidates.empty()
                        ? localization::translate_message("None")
                        : join_display_values(candidates),
                    observed_version_unknown_reason_display(
                        detail.reason, blocker_index, state));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        RepositoryExactPackageSourceFailure>>) {
                return localization::format_translated_message(
                    "source authority failure ({}); package: {}/{}; reason: {}",
                    "RepositoryExactPackageSourceFailure",
                    required_string_display(
                        detail.repository.repository_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A repository package source failure is missing its repository identity.")),
                    required_string_display(
                        detail.package_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A repository package source failure is missing its package identity.")),
                    repository_source_failure_reason_display(
                        detail.reason, blocker_index, state));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        RepositoryProviderSourceFailure>>) {
                return localization::format_translated_message(
                    "source authority failure ({}); repository: {}; reason: {}",
                    "RepositoryProviderSourceFailure",
                    required_string_display(
                        detail.repository.repository_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A repository provider source failure is missing its repository identity.")),
                    repository_source_failure_reason_display(
                        detail.reason, blocker_index, state));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        LocalSourceRootFailure>>) {
                return localization::format_translated_message(
                    "source authority failure ({}); path: {}; stage: {}; code: {}; system error: {}",
                    "LocalSourceRootFailure",
                    required_string_display(
                        detail.path.generic_string(), state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A local source failure is missing its path identity.")),
                    local_source_root_stage_display(
                        detail.stage, blocker_index, state),
                    local_source_root_error_code_display(
                        detail.code, blocker_index, state),
                    detail.system_error.has_value()
                        ? localization::format_translated_message(
                              "{} ({})",
                              detail.system_error->value(),
                              terminal_safe_text_display(
                                  detail.system_error->message()))
                        : localization::translate_message(
                              "not observed"));
            } else {
                return localization::format_translated_message(
                    "source authority failure ({}); packages: {}; diagnostic: {}",
                    "AurUpdateQueryFailure",
                    detail.package_names.empty()
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::format_translated_message(
                                  "An {} query failure is missing its package identities.",
                                  "AUR"))
                        : join_display_values(
                              detail.package_names),
                    required_string_display(
                        detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "An {} query failure is missing its diagnostic.",
                            "AUR")));
            }
        },
        blocker.detail);
}

std::string local_dependency_plan_failure_kind_display(
    LocalDependencyPlanFailureKind kind, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    switch(kind) {
        case LocalDependencyPlanFailureKind::UnsupportedArchitecture:
            return "LocalDependencyPlanFailureKind::UnsupportedArchitecture";
        case LocalDependencyPlanFailureKind::ConstraintMismatch:
            return "LocalDependencyPlanFailureKind::ConstraintMismatch";
        case LocalDependencyPlanFailureKind::AmbiguousLocalProvider:
            return "LocalDependencyPlanFailureKind::AmbiguousLocalProvider";
        case LocalDependencyPlanFailureKind::RemoteProviderIdentityConflict:
            return "LocalDependencyPlanFailureKind::RemoteProviderIdentityConflict";
    }
    return unsupported_display(
        state, section, item_index, detail_index,
        localization::translate_message(
            "A local dependency plan failure kind is not supported by the renderer."));
}

std::string local_dependency_plan_failure_display(
    const LocalDependencyPlanFailure& failure, RenderState& state,
    UnifiedPlanRenderingSection section, std::size_t item_index,
    std::optional<std::size_t> detail_index) {
    std::vector<std::string> candidates;
    candidates.reserve(failure.candidates.size());
    for(const LocalDependencyPlanCandidate& candidate : failure.candidates) {
        const std::string remote_provider =
            candidate.remote_provider.has_value()
                ? provider_identity_display(
                      candidate.remote_provider.value(), state, section,
                      item_index, detail_index)
                : localization::translate_message("not observed");
        const std::string constraint =
            candidate.constraint_evaluation.has_value()
                ? constraint_evaluation_display(
                      candidate.constraint_evaluation.value(), state,
                      section, item_index, detail_index)
                : localization::translate_message("not observed");
        candidates.push_back(localization::format_translated_message(
            "{} (provided capability: {}; version: {}; remote provider: {}; stored constraint result: {})",
            required_string_display(
                candidate.package_name, state, section, item_index,
                detail_index,
                localization::translate_message(
                    "A local dependency plan failure candidate is missing its package identity.")),
            optional_string_display(candidate.provided_specification),
            optional_string_display(candidate.version), remote_provider,
            constraint));
    }

    const bool requires_dependency =
        failure.kind !=
        LocalDependencyPlanFailureKind::UnsupportedArchitecture;
    const bool requires_architecture =
        failure.kind ==
        LocalDependencyPlanFailureKind::UnsupportedArchitecture;
    const bool requires_candidates =
        failure.kind !=
        LocalDependencyPlanFailureKind::UnsupportedArchitecture;
    std::string dependency = optional_string_display(
        failure.dependency_specification);
    if(requires_dependency &&
       (!failure.dependency_specification.has_value() ||
        failure.dependency_specification->empty())) {
        dependency = unavailable_display(
            state, section, item_index, detail_index,
            localization::translate_message(
                "A local dependency plan failure is missing its dependency specification."));
    }
    std::string architecture = optional_string_display(
        failure.effective_architecture);
    if(requires_architecture &&
       (!failure.effective_architecture.has_value() ||
        failure.effective_architecture->empty())) {
        architecture = unavailable_display(
            state, section, item_index, detail_index,
            localization::translate_message(
                "An unsupported-architecture local dependency failure is missing its architecture."));
    }
    std::string candidate_display =
        candidates.empty() ? localization::translate_message("None")
                           : join_display_values(candidates);
    if(requires_candidates && candidates.empty()) {
        candidate_display = unavailable_display(
            state, section, item_index, detail_index,
            localization::translate_message(
                "A local dependency plan failure is missing its candidate identities."));
    }
    return localization::format_translated_message(
        "local dependency plan failure ({}); parent: {}; dependency: {}; architecture: {}; candidates: {}",
        local_dependency_plan_failure_kind_display(
            failure.kind, state, section, item_index, detail_index),
        required_string_display(
            failure.parent_package_name, state, section, item_index,
            detail_index,
            localization::translate_message(
                "A local dependency plan failure is missing its parent package identity.")),
        dependency, architecture, candidate_display);
}

std::string build_plan_state_blocker_display(
    const BuildPlanStateUnifiedPlanBlocker& blocker,
    std::size_t blocker_index, RenderState& state) {
    const BuildPlan& plan = blocker.authority.get();
    switch(blocker.kind) {
        case BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency:
            if(blocker.authority_index < plan.unresolved.size()) {
                return localization::format_translated_message(
                    "{} state blocker ({}): unresolved dependency: {}",
                    "BuildPlan",
                    "BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency",
                    required_string_display(
                        plan.unresolved[blocker.authority_index], state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, blocker.authority_index,
                        localization::format_translated_message(
                            "A {} unresolved-dependency blocker is missing its dependency identity.",
                            "BuildPlan")));
            }
            break;
        case BuildPlanStateUnifiedPlanBlockerKind::DependencyCycle:
            if(blocker.authority_index < plan.cycles.size()) {
                return localization::format_translated_message(
                    "{} state blocker ({}): dependency cycle: {}",
                    "BuildPlan",
                    "BuildPlanStateUnifiedPlanBlockerKind::DependencyCycle",
                    required_string_display(
                        plan.cycles[blocker.authority_index], state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, blocker.authority_index,
                        localization::format_translated_message(
                            "A {} cycle blocker is missing its cycle diagnostic.",
                            "BuildPlan")));
            }
            break;
        case BuildPlanStateUnifiedPlanBlockerKind::SplitPackageSelectionRequired:
            if(blocker.authority_index < plan.split_package_targets.size()) {
                const BuildPlanSplitPackageTarget& target =
                    plan.split_package_targets[blocker.authority_index];
                return localization::format_translated_message(
                    "{} state blocker ({}): split package selection required: {} ({}: {})",
                    "BuildPlan",
                    "BuildPlanStateUnifiedPlanBlockerKind::SplitPackageSelectionRequired",
                    required_string_display(
                        target.package_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, blocker.authority_index,
                        localization::translate_message(
                            "A split-package selection blocker is missing its package identity.")),
                    "PackageBase",
                    required_string_display(
                        target.package_base, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, blocker.authority_index,
                        localization::format_translated_message(
                            "A split-package selection blocker is missing its {} identity.",
                            "PackageBase")));
            }
            break;
        default:
            return unsupported_display(
                state, UnifiedPlanRenderingSection::Blockers, blocker_index,
                std::nullopt,
                localization::format_translated_message(
                    "A {} blocker kind is not supported by the renderer.",
                    "BuildPlan"));
    }
    return unavailable_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        blocker.authority_index,
        localization::format_translated_message(
            "A {} blocker references an unavailable authority item.",
            "BuildPlan"));
}

std::string root_package_preparation_issue_kind_display(
    RootPackageInstallPreparationIssueKind kind,
    std::size_t blocker_index, std::optional<std::size_t> detail_index,
    RenderState& state) {
    switch(kind) {
        case RootPackageInstallPreparationIssueKind::EmptyQuery:
            return "RootPackageInstallPreparationIssueKind::EmptyQuery";
        case RootPackageInstallPreparationIssueKind::RemoveDependenciesUnsupported:
            return "RootPackageInstallPreparationIssueKind::RemoveDependenciesUnsupported";
        case RootPackageInstallPreparationIssueKind::InputGateUnavailable:
            return "RootPackageInstallPreparationIssueKind::InputGateUnavailable";
        case RootPackageInstallPreparationIssueKind::SelectionUnavailable:
            return "RootPackageInstallPreparationIssueKind::SelectionUnavailable";
        case RootPackageInstallPreparationIssueKind::SelectionCancelled:
            return "RootPackageInstallPreparationIssueKind::SelectionCancelled";
        case RootPackageInstallPreparationIssueKind::SourceOptionsWithoutAurTarget:
            return "RootPackageInstallPreparationIssueKind::SourceOptionsWithoutAurTarget";
        case RootPackageInstallPreparationIssueKind::BuildPlanPreparationFailed:
            return "RootPackageInstallPreparationIssueKind::BuildPlanPreparationFailed";
        case RootPackageInstallPreparationIssueKind::SourceWorkPreparationFailed:
            return "RootPackageInstallPreparationIssueKind::SourceWorkPreparationFailed";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package preparation issue kind is not supported by the renderer."));
}

std::string root_selection_input_gate_display(
    RootPackageSelectionInputGate gate, std::size_t blocker_index,
    std::optional<std::size_t> detail_index, RenderState& state) {
    switch(gate) {
        case RootPackageSelectionInputGate::Interactive:
            return "RootPackageSelectionInputGate::Interactive";
        case RootPackageSelectionInputGate::NonTty:
            return "RootPackageSelectionInputGate::NonTty";
        case RootPackageSelectionInputGate::NoConfirm:
            return "RootPackageSelectionInputGate::NoConfirm";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package selection input gate is not supported by the renderer."));
}

std::string root_selection_unavailable_reason_display(
    RootPackageSelectionUnavailableReason reason,
    std::size_t blocker_index, std::optional<std::size_t> detail_index,
    RenderState& state) {
    switch(reason) {
        case RootPackageSelectionUnavailableReason::NonInteractiveInput:
            return "RootPackageSelectionUnavailableReason::NonInteractiveInput";
        case RootPackageSelectionUnavailableReason::NoConfirm:
            return "RootPackageSelectionUnavailableReason::NoConfirm";
        case RootPackageSelectionUnavailableReason::NoCandidates:
            return "RootPackageSelectionUnavailableReason::NoCandidates";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package selection unavailable reason is not supported by the renderer."));
}

std::string root_selection_cancellation_reason_display(
    RootPackageSelectionCancellationReason reason,
    std::size_t blocker_index, std::optional<std::size_t> detail_index,
    RenderState& state) {
    switch(reason) {
        case RootPackageSelectionCancellationReason::EmptyInput:
            return "RootPackageSelectionCancellationReason::EmptyInput";
        case RootPackageSelectionCancellationReason::CancelToken:
            return "RootPackageSelectionCancellationReason::CancelToken";
        case RootPackageSelectionCancellationReason::EndOfInput:
            return "RootPackageSelectionCancellationReason::EndOfInput";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package selection cancellation reason is not supported by the renderer."));
}

std::string required_snapshot_opaque_value_display(
    std::string_view value, RenderState& state,
    std::size_t blocker_index, std::size_t detail_index,
    std::string diagnostic) {
    if(value.empty()) {
        return unavailable_display(
            state, UnifiedPlanRenderingSection::Blockers, blocker_index,
            detail_index, std::move(diagnostic));
    }
    return invalid_snapshot_raw_value_display(value);
}

std::string root_package_source_kind_display(
    RootPackageSourceKind kind, std::size_t blocker_index,
    std::size_t detail_index, RenderState& state) {
    switch(kind) {
        case RootPackageSourceKind::Repository:
            return "RootPackageSourceKind::Repository";
        case RootPackageSourceKind::Aur:
            return "RootPackageSourceKind::Aur";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package candidate source kind is not supported by the renderer."));
}

std::string root_package_validation_issue_kind_display(
    RootPackageCandidateValidationIssueKind kind,
    std::size_t blocker_index, std::size_t detail_index,
    RenderState& state) {
    switch(kind) {
        case RootPackageCandidateValidationIssueKind::InvalidRepositoryName:
            return "RootPackageCandidateValidationIssueKind::InvalidRepositoryName";
        case RootPackageCandidateValidationIssueKind::InvalidPackageName:
            return "RootPackageCandidateValidationIssueKind::InvalidPackageName";
        case RootPackageCandidateValidationIssueKind::InvalidPackageBase:
            return "RootPackageCandidateValidationIssueKind::InvalidPackageBase";
        case RootPackageCandidateValidationIssueKind::InvalidVersion:
            return "RootPackageCandidateValidationIssueKind::InvalidVersion";
        case RootPackageCandidateValidationIssueKind::InvalidDescription:
            return "RootPackageCandidateValidationIssueKind::InvalidDescription";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package candidate validation issue kind is not supported by the renderer."));
}

std::string root_package_metadata_field_display(
    RootPackageCandidateMetadataField field,
    std::size_t blocker_index, std::size_t detail_index,
    RenderState& state) {
    switch(field) {
        case RootPackageCandidateMetadataField::Version:
            return "RootPackageCandidateMetadataField::Version";
        case RootPackageCandidateMetadataField::Description:
            return "RootPackageCandidateMetadataField::Description";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        detail_index,
        localization::translate_message(
            "A root package candidate metadata field is not supported by the renderer."));
}

std::string root_package_identity_display(
    const RootPackageIdentity& identity, std::size_t blocker_index,
    std::size_t detail_index, RenderState& state) {
    return std::visit(
        [&](const auto& typed_identity) -> std::string {
            using Identity = std::decay_t<decltype(typed_identity)>;
            if constexpr(std::is_same_v<
                             Identity,
                             RepositoryRootPackageIdentity>) {
                return required_string_display(
                           typed_identity.repository_name, state,
                           UnifiedPlanRenderingSection::Blockers,
                           blocker_index, detail_index,
                           localization::translate_message(
                               "A candidate-pair issue is missing its repository identity.")) +
                       "/" +
                       required_string_display(
                           typed_identity.package_name, state,
                           UnifiedPlanRenderingSection::Blockers,
                           blocker_index, detail_index,
                           localization::translate_message(
                               "A candidate-pair issue is missing its package identity."));
            } else {
                return localization::format_translated_message(
                    "{}/{} ({}: {})", "AUR",
                    required_string_display(
                        typed_identity.package_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, detail_index,
                        localization::format_translated_message(
                            "An {} candidate-pair issue is missing its package identity.",
                            "AUR")),
                    "PackageBase",
                    required_string_display(
                        typed_identity.package_base, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, detail_index,
                        localization::format_translated_message(
                            "An {} candidate-pair issue is missing its {} identity.",
                            "AUR", "PackageBase")));
            }
        },
        identity);
}

std::string invalid_root_package_search_snapshot_display(
    const InvalidRootPackageSearchSnapshot& snapshot,
    std::size_t blocker_index, std::size_t detail_index,
    RenderState& state) {
    std::vector<std::string> validation_failures;
    validation_failures.reserve(snapshot.validation_failures.size());
    for(std::size_t failure_index = 0;
        failure_index < snapshot.validation_failures.size(); ++failure_index) {
        const RootPackageCandidateValidationFailure& failure =
            snapshot.validation_failures[failure_index];
        std::vector<std::string> issue_kinds;
        issue_kinds.reserve(failure.issues.size());
        for(const RootPackageCandidateValidationIssue& issue : failure.issues) {
            // POLICY: issue.value is raw candidate input and must not be
            // reflected to the terminal from an invalid snapshot.
            issue_kinds.push_back(root_package_validation_issue_kind_display(
                issue.kind, blocker_index, detail_index, state));
        }
        validation_failures.push_back(
            localization::format_translated_message(
                "#{} source: {}; validation kinds: {}",
                failure_index + 1,
                root_package_source_kind_display(
                    failure.source_kind, blocker_index,
                    detail_index, state),
                issue_kinds.empty()
                    ? unavailable_display(
                          state,
                          UnifiedPlanRenderingSection::Blockers,
                          blocker_index, detail_index,
                          localization::translate_message(
                              "A candidate validation failure has no typed issue kinds."))
                    : join_display_values(issue_kinds)));
    }

    std::vector<std::string> pair_issues;
    pair_issues.reserve(snapshot.candidate_pair_issues.size());
    for(std::size_t pair_index = 0;
        pair_index < snapshot.candidate_pair_issues.size(); ++pair_index) {
        pair_issues.push_back(std::visit(
            [&](const auto& issue) -> std::string {
                using Issue = std::decay_t<decltype(issue)>;
                if constexpr(std::is_same_v<
                                 Issue,
                                 InconsistentAurRootPackageBase>) {
                    return localization::format_translated_message(
                        "#{} {}; package: {}; first {}: {}; second {}: {}",
                        pair_index + 1,
                        "InconsistentAurRootPackageBase",
                        required_string_display(
                            issue.package_name, state,
                            UnifiedPlanRenderingSection::Blockers,
                            blocker_index, detail_index,
                            localization::format_translated_message(
                                "An inconsistent {} candidate pair is missing its package identity.",
                                "AUR")),
                        "PackageBase",
                        required_string_display(
                            issue.first_package_base, state,
                            UnifiedPlanRenderingSection::Blockers,
                            blocker_index, detail_index,
                            localization::format_translated_message(
                                "An inconsistent {} candidate pair is missing its first identity.",
                                "PackageBase")),
                        "PackageBase",
                        required_string_display(
                            issue.second_package_base, state,
                            UnifiedPlanRenderingSection::Blockers,
                            blocker_index, detail_index,
                            localization::format_translated_message(
                                "An inconsistent {} candidate pair is missing its second identity.",
                                "PackageBase")));
                } else {
                    return localization::format_translated_message(
                        "#{} {}; identity: {}; field: {}; first value: {}; second value: {}",
                        pair_index + 1,
                        "ConflictingRootPackageCandidateMetadata",
                        root_package_identity_display(
                            issue.identity, blocker_index,
                            detail_index, state),
                        root_package_metadata_field_display(
                            issue.field, blocker_index,
                            detail_index, state),
                        required_string_display(
                            issue.first_value, state,
                            UnifiedPlanRenderingSection::Blockers,
                            blocker_index, detail_index,
                            localization::translate_message(
                                "A candidate metadata conflict is missing its first value.")),
                        required_string_display(
                            issue.second_value, state,
                            UnifiedPlanRenderingSection::Blockers,
                            blocker_index, detail_index,
                            localization::translate_message(
                                "A candidate metadata conflict is missing its second value.")));
                }
            },
            snapshot.candidate_pair_issues[pair_index]));
    }

    std::vector<std::string> group_matches;
    group_matches.reserve(snapshot.invalid_group_matches.size());
    for(std::size_t group_index = 0;
        group_index < snapshot.invalid_group_matches.size(); ++group_index) {
        const InvalidRepositoryRootPackageGroupMatch& match =
            snapshot.invalid_group_matches[group_index];
        const std::string identity =
            required_snapshot_opaque_value_display(
                match.identity.repository_name, state, blocker_index,
                detail_index,
                localization::translate_message(
                    "An invalid group match is missing its repository identity.")) +
            "/" +
            required_snapshot_opaque_value_display(
                match.identity.package_name, state, blocker_index,
                detail_index,
                localization::translate_message(
                    "An invalid group match is missing its package identity."));
        const std::string group = match.group_name.has_value()
                                      ? required_snapshot_opaque_value_display(
                                            match.group_name.value(), state, blocker_index,
                                            detail_index,
                                            localization::translate_message(
                                                "An invalid group match has an empty group identity."))
                                      : localization::translate_message("not observed");
        group_matches.push_back(localization::format_translated_message(
            "#{} {}; identity: {}; group: {}", group_index + 1,
            "InvalidRepositoryRootPackageGroupMatch", identity, group));
    }

    std::vector<std::string> duplicate_repositories;
    duplicate_repositories.reserve(
        snapshot.duplicate_repository_order_entries.size());
    for(std::size_t repository_index = 0;
        repository_index <
        snapshot.duplicate_repository_order_entries.size();
        ++repository_index) {
        duplicate_repositories.push_back(
            localization::format_translated_message(
                "#{} duplicate repository-order entry: {}",
                repository_index + 1,
                required_snapshot_opaque_value_display(
                    snapshot.duplicate_repository_order_entries[repository_index],
                    state, blocker_index, detail_index,
                    localization::translate_message(
                        "A duplicate repository-order entry is missing its repository identity."))));
    }

    std::vector<std::string> unranked_candidates;
    unranked_candidates.reserve(
        snapshot.unranked_repository_candidates.size());
    for(std::size_t candidate_index = 0;
        candidate_index < snapshot.unranked_repository_candidates.size();
        ++candidate_index) {
        const RepositoryRootPackageIdentity& identity =
            snapshot.unranked_repository_candidates[candidate_index];
        const std::string identity_display =
            required_snapshot_opaque_value_display(
                identity.repository_name, state, blocker_index,
                detail_index,
                localization::translate_message(
                    "An unranked repository candidate is missing its repository identity.")) +
            "/" +
            required_snapshot_opaque_value_display(
                identity.package_name, state, blocker_index,
                detail_index,
                localization::translate_message(
                    "An unranked repository candidate is missing its package identity."));
        unranked_candidates.push_back(
            localization::format_translated_message(
                "#{} unranked repository candidate: {}",
                candidate_index + 1, identity_display));
    }

    if(validation_failures.empty() && pair_issues.empty() &&
       group_matches.empty() && duplicate_repositories.empty() &&
       unranked_candidates.empty()) {
        state.add_issue(
            UnifiedPlanRenderingIssueKind::MissingReferencedValue,
            UnifiedPlanRenderingSection::Blockers, blocker_index,
            detail_index,
            localization::translate_message(
                "An invalid root package search snapshot has no typed details."));
    }

    const auto display_collection = [](const std::vector<std::string>& values) {
        return values.empty() ? localization::translate_message("None")
                              : join_display_values(values);
    };
    return localization::format_translated_message(
        "{}; candidate validation failures: {}; candidate pair issues: {}; invalid group matches: {}; duplicate repository-order entries: {}; unranked repository candidates: {}",
        "InvalidRootPackageSearchSnapshot",
        display_collection(validation_failures),
        display_collection(pair_issues), display_collection(group_matches),
        display_collection(duplicate_repositories),
        display_collection(unranked_candidates));
}

std::string root_package_preparation_detail_display(
    const RootPackageInstallPreparationFailureDetail& detail,
    std::size_t blocker_index, std::size_t detail_index,
    RenderState& state) {
    return std::visit(
        [&](const auto& typed_detail) -> std::string {
            using Detail = std::decay_t<decltype(typed_detail)>;
            if constexpr(std::is_same_v<
                             Detail,
                             RootPackageInstallPreparationIssue>) {
                const bool requires_unavailable_reason =
                    typed_detail.kind ==
                    RootPackageInstallPreparationIssueKind::
                        SelectionUnavailable;
                const bool requires_cancellation_reason =
                    typed_detail.kind ==
                    RootPackageInstallPreparationIssueKind::
                        SelectionCancelled;
                const bool selection_requires_input_gate =
                    requires_unavailable_reason &&
                    typed_detail.selection_unavailable_reason
                        .has_value() &&
                    typed_detail.selection_unavailable_reason.value() !=
                        RootPackageSelectionUnavailableReason::
                            NoCandidates;
                const bool requires_input_gate =
                    typed_detail.kind ==
                        RootPackageInstallPreparationIssueKind::
                            InputGateUnavailable ||
                    selection_requires_input_gate;
                const bool requires_diagnostic =
                    typed_detail.kind !=
                        RootPackageInstallPreparationIssueKind::
                            InputGateUnavailable &&
                    (!requires_unavailable_reason ||
                     (typed_detail.selection_unavailable_reason
                          .has_value() &&
                      typed_detail.selection_unavailable_reason
                              .value() ==
                          RootPackageSelectionUnavailableReason::
                              NoCandidates));
                const std::string input_gate =
                    typed_detail.input_gate.has_value()
                        ? root_selection_input_gate_display(
                              typed_detail.input_gate.value(),
                              blocker_index, detail_index, state)
                    : requires_input_gate
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, detail_index,
                              localization::translate_message(
                                  "A root package preparation issue is missing its required input gate."))
                        : localization::translate_message(
                              "not observed");
                const std::string unavailable_reason =
                    typed_detail.selection_unavailable_reason
                            .has_value()
                        ? root_selection_unavailable_reason_display(
                              typed_detail
                                  .selection_unavailable_reason
                                  .value(),
                              blocker_index, detail_index, state)
                    : requires_unavailable_reason
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, detail_index,
                              localization::translate_message(
                                  "A root package selection-unavailable issue is missing its typed reason."))
                        : localization::translate_message(
                              "not observed");
                const std::string cancellation_reason =
                    typed_detail.selection_cancellation_reason
                            .has_value()
                        ? root_selection_cancellation_reason_display(
                              typed_detail
                                  .selection_cancellation_reason
                                  .value(),
                              blocker_index, detail_index, state)
                    : requires_cancellation_reason
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, detail_index,
                              localization::translate_message(
                                  "A root package selection-cancelled issue is missing its typed reason."))
                        : localization::translate_message(
                              "not observed");
                const std::string diagnostic =
                    typed_detail.diagnostic.empty()
                        ? requires_diagnostic
                              ? unavailable_display(
                                    state,
                                    UnifiedPlanRenderingSection::
                                        Blockers,
                                    blocker_index, detail_index,
                                    localization::translate_message(
                                        "A root package preparation issue is missing its required diagnostic."))
                              : localization::translate_message(
                                    "not observed")
                        : terminal_safe_text_display(
                              typed_detail.diagnostic);
                return localization::format_translated_message(
                    "{}; input gate: {}; unavailable reason: {}; cancellation reason: {}; diagnostic: {}",
                    root_package_preparation_issue_kind_display(
                        typed_detail.kind, blocker_index,
                        detail_index, state),
                    input_gate, unavailable_reason,
                    cancellation_reason, diagnostic);
            } else if constexpr(std::is_same_v<
                                    Detail,
                                    RepositoryRootPackageSearchFailure>) {
                return localization::format_translated_message(
                    "{}; code: {}; diagnostic: {}",
                    "RepositoryRootPackageSearchFailure",
                    package_metadata_error_code_display(
                        typed_detail.failure.code,
                        blocker_index, state),
                    required_string_display(
                        typed_detail.failure.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, detail_index,
                        localization::translate_message(
                            "A repository root search failure is missing its diagnostic.")));
            } else if constexpr(std::is_same_v<
                                    Detail,
                                    AurRootPackageSearchFailure>) {
                return localization::format_translated_message(
                    "{}; diagnostic: {}",
                    "AurRootPackageSearchFailure",
                    required_string_display(
                        typed_detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, detail_index,
                        localization::format_translated_message(
                            "An {} root search failure is missing its diagnostic.",
                            "AUR")));
            } else if constexpr(std::is_same_v<
                                    Detail,
                                    InvalidRootPackageSearchSnapshot>) {
                return invalid_root_package_search_snapshot_display(
                    typed_detail, blocker_index, detail_index, state);
            } else {
                std::vector<std::string> targets;
                targets.reserve(
                    typed_detail
                        .unrepresentable_repository_targets
                        .size());
                for(const auto& target :
                    typed_detail.unrepresentable_repository_targets) {
                    targets.push_back(
                        localization::format_translated_message(
                            "selection index {}; repository: {}; package: {}",
                            target.selection_index,
                            terminal_safe_text_display(
                                target.identity.repository_name),
                            terminal_safe_text_display(
                                target.identity.package_name)));
                }
                return localization::format_translated_message(
                    "{}; targets: {}",
                    "InvalidRootPackageRoutingProjection",
                    targets.empty()
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, detail_index,
                              localization::translate_message(
                                  "An invalid root routing projection is missing its affected target identities."))
                        : join_display_values(targets));
            }
        },
        detail);
}

std::string root_package_preparation_failure_display(
    const RootPackageInstallPreparationFailure& failure,
    std::size_t blocker_index, RenderState& state) {
    if(failure.details.empty()) {
        return localization::format_translated_message(
            "root package preparation failure: {}",
            unavailable_display(
                state, UnifiedPlanRenderingSection::Blockers,
                blocker_index, std::nullopt,
                localization::translate_message(
                    "A root package preparation failure has no typed details.")));
    }
    std::vector<std::string> details;
    details.reserve(failure.details.size());
    for(std::size_t detail_index = 0;
        detail_index < failure.details.size(); ++detail_index) {
        details.push_back(root_package_preparation_detail_display(
            failure.details[detail_index], blocker_index, detail_index,
            state));
    }
    return localization::format_translated_message(
        "root package preparation failure: {}",
        join_display_values(details));
}

std::string build_plan_artifact_projection_issue_kind_display(
    BuildPlanArtifactTargetProjectionIssueKind kind,
    std::size_t blocker_index, RenderState& state) {
    switch(kind) {
        case BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageBase:
            return "BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageBase";
        case BuildPlanArtifactTargetProjectionIssueKind::EmptyEntryPackageNames:
            return "BuildPlanArtifactTargetProjectionIssueKind::EmptyEntryPackageNames";
        case BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageName:
            return "BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageName";
        case BuildPlanArtifactTargetProjectionIssueKind::DuplicateEntryPackageName:
            return "BuildPlanArtifactTargetProjectionIssueKind::DuplicateEntryPackageName";
        case BuildPlanArtifactTargetProjectionIssueKind::MissingPlannedPackageTarget:
            return "BuildPlanArtifactTargetProjectionIssueKind::MissingPlannedPackageTarget";
        case BuildPlanArtifactTargetProjectionIssueKind::DuplicatePlannedPackageTarget:
            return "BuildPlanArtifactTargetProjectionIssueKind::DuplicatePlannedPackageTarget";
        case BuildPlanArtifactTargetProjectionIssueKind::PackageBaseMismatch:
            return "BuildPlanArtifactTargetProjectionIssueKind::PackageBaseMismatch";
        case BuildPlanArtifactTargetProjectionIssueKind::UncoveredPlannedPackageTarget:
            return "BuildPlanArtifactTargetProjectionIssueKind::UncoveredPlannedPackageTarget";
        case BuildPlanArtifactTargetProjectionIssueKind::DesiredInstallReasonUnavailable:
            return "BuildPlanArtifactTargetProjectionIssueKind::DesiredInstallReasonUnavailable";
        case BuildPlanArtifactTargetProjectionIssueKind::RootAttributionInconsistent:
            return "BuildPlanArtifactTargetProjectionIssueKind::RootAttributionInconsistent";
        case BuildPlanArtifactTargetProjectionIssueKind::DuplicatePackageBaseEntry:
            return "BuildPlanArtifactTargetProjectionIssueKind::DuplicatePackageBaseEntry";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A required artifact projection issue kind is not supported by the renderer."));
}

std::string build_plan_artifact_projection_issue_display(
    const BuildPlanArtifactTargetProjectionIssue& issue,
    std::size_t blocker_index, RenderState& state) {
    std::vector<std::string> roots;
    roots.reserve(issue.roots.size());
    for(const RootTargetIdentity& root : issue.roots) {
        const std::string root_identity = root.requested_name.empty()
                                              ? unavailable_display(
                                                    state,
                                                    UnifiedPlanRenderingSection::Blockers,
                                                    blocker_index, std::nullopt,
                                                    localization::translate_message(
                                                        "A required artifact projection issue is missing an affected root identity."))
                                              : invalid_snapshot_raw_value_display(root.requested_name);
        roots.push_back(localization::format_translated_message(
            "{} (invocation index: {})",
            root_identity, root.invocation_index));
    }
    std::vector<std::string> package_target_indices;
    package_target_indices.reserve(issue.package_target_indices.size());
    for(const std::size_t index : issue.package_target_indices) {
        package_target_indices.push_back(std::to_string(index));
    }

    bool has_required_location = false;
    switch(issue.kind) {
        case BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageBase:
            has_required_location = issue.build_plan_order_index.has_value() ||
                                    !issue.package_target_indices.empty();
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::EmptyEntryPackageNames:
        case BuildPlanArtifactTargetProjectionIssueKind::DuplicatePackageBaseEntry:
            has_required_location = issue.build_plan_order_index.has_value();
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageName:
        case BuildPlanArtifactTargetProjectionIssueKind::PackageBaseMismatch:
            has_required_location =
                (issue.build_plan_order_index.has_value() &&
                 issue.entry_package_name_index.has_value()) ||
                !issue.package_target_indices.empty();
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::DuplicateEntryPackageName:
        case BuildPlanArtifactTargetProjectionIssueKind::MissingPlannedPackageTarget:
            has_required_location = issue.build_plan_order_index.has_value() &&
                                    issue.entry_package_name_index.has_value();
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::
            DuplicatePlannedPackageTarget:
            has_required_location = issue.package_target_indices.size() >= 2;
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::
            UncoveredPlannedPackageTarget:
            has_required_location = !issue.package_target_indices.empty();
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::
            DesiredInstallReasonUnavailable:
            has_required_location = issue.build_plan_order_index.has_value() &&
                                    issue.entry_package_name_index.has_value() &&
                                    !issue.package_target_indices.empty();
            break;
        case BuildPlanArtifactTargetProjectionIssueKind::
            RootAttributionInconsistent:
            has_required_location = !issue.package_target_indices.empty() ||
                                    !issue.roots.empty();
            break;
    }
    if(!has_required_location) {
        state.add_issue(
            UnifiedPlanRenderingIssueKind::MissingReferencedValue,
            UnifiedPlanRenderingSection::Blockers, blocker_index,
            std::nullopt,
            localization::translate_message(
                "A required artifact projection issue is missing its kind-specific authority location."));
    }

    const bool requires_package_base =
        issue.kind != BuildPlanArtifactTargetProjectionIssueKind::
                          RootAttributionInconsistent;
    const bool requires_package_name =
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                InvalidPackageName ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                DuplicateEntryPackageName ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                MissingPlannedPackageTarget ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                DuplicatePlannedPackageTarget ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                PackageBaseMismatch ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                UncoveredPlannedPackageTarget ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable ||
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                RootAttributionInconsistent;
    const auto identity_display = [&](
                                      const std::optional<std::string>& value,
                                      bool required, bool unsafe_value,
                                      std::string diagnostic) {
        if(value.has_value()) {
            // Invalid identifier values are typed affected subjects, but are
            // deliberately not reflected to the terminal.
            if(unsafe_value) {
                return localization::translate_message(
                    "observed invalid value");
            }
            if(!value->empty()) {
                return invalid_snapshot_raw_value_display(value.value());
            }
            return unavailable_display(
                state, UnifiedPlanRenderingSection::Blockers,
                blocker_index, std::nullopt, std::move(diagnostic));
        }
        if(required) {
            return unavailable_display(
                state, UnifiedPlanRenderingSection::Blockers,
                blocker_index, std::nullopt, std::move(diagnostic));
        }
        return localization::translate_message("not observed");
    };
    const std::string package_base = identity_display(
        issue.package_base, requires_package_base,
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                InvalidPackageBase,
        localization::format_translated_message(
            "A required artifact projection issue is missing its {} subject.",
            "PackageBase"));
    const std::string package_name = identity_display(
        issue.package_name, requires_package_name,
        issue.kind ==
            BuildPlanArtifactTargetProjectionIssueKind::
                InvalidPackageName,
        localization::translate_message(
            "A required artifact projection issue is missing its package subject."));
    const std::string diagnostic = required_string_display(
        issue.diagnostic, state,
        UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A required artifact projection issue is missing its diagnostic."));
    return localization::format_translated_message(
        "required artifact projection failure ({}); {} unit index: {}; entry package index: {}; package target indices: {}; {}: {}; package: {}; roots: {}; diagnostic: {}",
        build_plan_artifact_projection_issue_kind_display(
            issue.kind, blocker_index, state),
        "BuildPlan",
        optional_index_display(issue.build_plan_order_index),
        optional_index_display(issue.entry_package_name_index),
        package_target_indices.empty()
            ? localization::translate_message("None")
            : join_display_values(package_target_indices),
        "PackageBase", package_base, package_name,
        roots.empty() ? localization::translate_message("None")
                      : join_display_values(roots),
        diagnostic);
}

std::string aur_update_execution_reason_display(
    AurUpdateExecutionReason reason, std::size_t blocker_index,
    RenderState& state) {
    switch(reason) {
        case AurUpdateExecutionReason::None:
            state.add_issue(
                UnifiedPlanRenderingIssueKind::MissingReferencedValue,
                UnifiedPlanRenderingSection::Blockers, blocker_index,
                std::nullopt,
                localization::format_translated_message(
                    "An {} route preflight blocker has no failure reason.",
                    "AUR"));
            return "AurUpdateExecutionReason::None";
        case AurUpdateExecutionReason::UpToDate:
            return "AurUpdateExecutionReason::UpToDate";
        case AurUpdateExecutionReason::DevelRequiresCheck:
            return "AurUpdateExecutionReason::DevelRequiresCheck";
        case AurUpdateExecutionReason::DevelObservationUnknown:
            return "AurUpdateExecutionReason::DevelObservationUnknown";
        case AurUpdateExecutionReason::DevelUnsupported:
            return "AurUpdateExecutionReason::DevelUnsupported";
        case AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck:
            return "AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck";
        case AurUpdateExecutionReason::NonAurForeign:
            return "AurUpdateExecutionReason::NonAurForeign";
        case AurUpdateExecutionReason::AurMetadataUnavailable:
            return "AurUpdateExecutionReason::AurMetadataUnavailable";
        case AurUpdateExecutionReason::VersionComparisonUnavailable:
            return "AurUpdateExecutionReason::VersionComparisonUnavailable";
        case AurUpdateExecutionReason::InstalledReasonUnknown:
            return "AurUpdateExecutionReason::InstalledReasonUnknown";
        case AurUpdateExecutionReason::UpdatePlanInconsistent:
            return "AurUpdateExecutionReason::UpdatePlanInconsistent";
        case AurUpdateExecutionReason::DuplicateUpdateTarget:
            return "AurUpdateExecutionReason::DuplicateUpdateTarget";
        case AurUpdateExecutionReason::RepositoryMetadataUnavailable:
            return "AurUpdateExecutionReason::RepositoryMetadataUnavailable";
        case AurUpdateExecutionReason::AurDependencyMetadataUnavailable:
            return "AurUpdateExecutionReason::AurDependencyMetadataUnavailable";
        case AurUpdateExecutionReason::ProviderMetadataUnavailable:
            return "AurUpdateExecutionReason::ProviderMetadataUnavailable";
        case AurUpdateExecutionReason::UnresolvedDependency:
            return "AurUpdateExecutionReason::UnresolvedDependency";
        case AurUpdateExecutionReason::VersionConstraintUnverified:
            return "AurUpdateExecutionReason::VersionConstraintUnverified";
        case AurUpdateExecutionReason::DependencyCycle:
            return "AurUpdateExecutionReason::DependencyCycle";
        case AurUpdateExecutionReason::BuildPlanInconsistent:
            return "AurUpdateExecutionReason::BuildPlanInconsistent";
        case AurUpdateExecutionReason::PackageBaseMismatch:
            return "AurUpdateExecutionReason::PackageBaseMismatch";
        case AurUpdateExecutionReason::SplitPackageSelectionRequired:
            return "AurUpdateExecutionReason::SplitPackageSelectionRequired";
        case AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase:
            return "AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase";
        case AurUpdateExecutionReason::AmbiguousProvider:
            return "AurUpdateExecutionReason::AmbiguousProvider";
        case AurUpdateExecutionReason::ConflictsOrReplacesUnresolved:
            return "AurUpdateExecutionReason::ConflictsOrReplacesUnresolved";
        case AurUpdateExecutionReason::InstalledPackageMetadataUnavailable:
            return "AurUpdateExecutionReason::InstalledPackageMetadataUnavailable";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} execution preflight reason is not supported by the renderer.",
            "AUR"));
}

std::string system_source_issue_kind_display(
    SystemSourceUpgradeIssueKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case SystemSourceUpgradeIssueKind::PreferenceEnumerationUnavailable:
            return "SystemSourceUpgradeIssueKind::PreferenceEnumerationUnavailable";
        case SystemSourceUpgradeIssueKind::PreferenceUnavailable:
            return "SystemSourceUpgradeIssueKind::PreferenceUnavailable";
        case SystemSourceUpgradeIssueKind::SourceIdentityResolutionFailed:
            return "SystemSourceUpgradeIssueKind::SourceIdentityResolutionFailed";
        case SystemSourceUpgradeIssueKind::SourceWorkItemPreparationFailed:
            return "SystemSourceUpgradeIssueKind::SourceWorkItemPreparationFailed";
        case SystemSourceUpgradeIssueKind::SourceInvocationPreparationFailed:
            return "SystemSourceUpgradeIssueKind::SourceInvocationPreparationFailed";
        case SystemSourceUpgradeIssueKind::SourceBaselineSnapshotUnavailable:
            return "SystemSourceUpgradeIssueKind::SourceBaselineSnapshotUnavailable";
        case SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable:
            return "SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable";
        case SystemSourceUpgradeIssueKind::PostSystemSourceSnapshotUnavailable:
            return "SystemSourceUpgradeIssueKind::PostSystemSourceSnapshotUnavailable";
        case SystemSourceUpgradeIssueKind::CacheAuthorityInvalid:
            return "SystemSourceUpgradeIssueKind::CacheAuthorityInvalid";
        case SystemSourceUpgradeIssueKind::InvalidPreferenceName:
            return "SystemSourceUpgradeIssueKind::InvalidPreferenceName";
        case SystemSourceUpgradeIssueKind::OptionSnapshotMismatch:
            return "SystemSourceUpgradeIssueKind::OptionSnapshotMismatch";
        case SystemSourceUpgradeIssueKind::PreparedCorrelationInconsistent:
            return "SystemSourceUpgradeIssueKind::PreparedCorrelationInconsistent";
        case SystemSourceUpgradeIssueKind::PreparedCapabilityConsumed:
            return "SystemSourceUpgradeIssueKind::PreparedCapabilityConsumed";
        case SystemSourceUpgradeIssueKind::UnknownPreparationFailure:
            return "SystemSourceUpgradeIssueKind::UnknownPreparationFailure";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A system/source upgrade issue kind is not supported by the renderer."));
}

std::string system_source_issue_impact_display(
    SystemSourceUpgradeIssueImpact impact, std::size_t blocker_index,
    RenderState& state) {
    switch(impact) {
        case SystemSourceUpgradeIssueImpact::ObservabilityOnly:
            return "SystemSourceUpgradeIssueImpact::ObservabilityOnly";
        case SystemSourceUpgradeIssueImpact::AffectsSuccess:
            return "SystemSourceUpgradeIssueImpact::AffectsSuccess";
        case SystemSourceUpgradeIssueImpact::BlocksExecution:
            return "SystemSourceUpgradeIssueImpact::BlocksExecution";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A system/source upgrade issue impact is not supported by the renderer."));
}

std::string route_system_error_display(
    const std::optional<std::error_code>& system_error) {
    if(!system_error.has_value()) {
        return localization::translate_message("not observed");
    }
    return localization::format_translated_message(
        "{}:{} ({})",
        terminal_safe_text_display(system_error->category().name()),
        system_error->value(),
        terminal_safe_text_display(system_error->message()));
}

std::string source_preference_failure_kind_display(
    SourcePreferenceFailureKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case SourcePreferenceFailureKind::AuthorityUnavailable:
            return "SourcePreferenceFailureKind::AuthorityUnavailable";
        case SourcePreferenceFailureKind::DirectoryEnumerationFailed:
            return "SourcePreferenceFailureKind::DirectoryEnumerationFailed";
        case SourcePreferenceFailureKind::InvalidEntryName:
            return "SourcePreferenceFailureKind::InvalidEntryName";
        case SourcePreferenceFailureKind::StatusUnavailable:
            return "SourcePreferenceFailureKind::StatusUnavailable";
        case SourcePreferenceFailureKind::UnsupportedFileType:
            return "SourcePreferenceFailureKind::UnsupportedFileType";
        case SourcePreferenceFailureKind::OwnershipMismatch:
            return "SourcePreferenceFailureKind::OwnershipMismatch";
        case SourcePreferenceFailureKind::UnsafePermissions:
            return "SourcePreferenceFailureKind::UnsafePermissions";
        case SourcePreferenceFailureKind::OpenFailed:
            return "SourcePreferenceFailureKind::OpenFailed";
        case SourcePreferenceFailureKind::LockFailed:
            return "SourcePreferenceFailureKind::LockFailed";
        case SourcePreferenceFailureKind::ReadFailed:
            return "SourcePreferenceFailureKind::ReadFailed";
        case SourcePreferenceFailureKind::WriteFailed:
            return "SourcePreferenceFailureKind::WriteFailed";
        case SourcePreferenceFailureKind::SyncFailed:
            return "SourcePreferenceFailureKind::SyncFailed";
        case SourcePreferenceFailureKind::RenameFailed:
            return "SourcePreferenceFailureKind::RenameFailed";
        case SourcePreferenceFailureKind::RemoveFailed:
            return "SourcePreferenceFailureKind::RemoveFailed";
        case SourcePreferenceFailureKind::ConcurrentReplacement:
            return "SourcePreferenceFailureKind::ConcurrentReplacement";
        case SourcePreferenceFailureKind::CloseFailed:
            return "SourcePreferenceFailureKind::CloseFailed";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A source preference failure kind is not supported by the renderer."));
}

std::string filesystem_file_type_display(
    std::filesystem::file_type type, std::size_t blocker_index,
    RenderState& state) {
    switch(type) {
        case std::filesystem::file_type::none:
            return "std::filesystem::file_type::none";
        case std::filesystem::file_type::not_found:
            return "std::filesystem::file_type::not_found";
        case std::filesystem::file_type::regular:
            return "std::filesystem::file_type::regular";
        case std::filesystem::file_type::directory:
            return "std::filesystem::file_type::directory";
        case std::filesystem::file_type::symlink:
            return "std::filesystem::file_type::symlink";
        case std::filesystem::file_type::block:
            return "std::filesystem::file_type::block";
        case std::filesystem::file_type::character:
            return "std::filesystem::file_type::character";
        case std::filesystem::file_type::fifo:
            return "std::filesystem::file_type::fifo";
        case std::filesystem::file_type::socket:
            return "std::filesystem::file_type::socket";
        case std::filesystem::file_type::unknown:
            return "std::filesystem::file_type::unknown";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A source preference file type is not supported by the renderer."));
}

std::string source_preference_failure_display(
    const SourcePreferenceFailure& failure, std::size_t blocker_index,
    RenderState& state) {
    const bool requires_path =
        failure.kind != SourcePreferenceFailureKind::AuthorityUnavailable;
    const bool requires_system_error =
        failure.kind ==
            SourcePreferenceFailureKind::DirectoryEnumerationFailed ||
        failure.kind == SourcePreferenceFailureKind::StatusUnavailable ||
        failure.kind == SourcePreferenceFailureKind::OpenFailed ||
        failure.kind == SourcePreferenceFailureKind::LockFailed ||
        failure.kind == SourcePreferenceFailureKind::ReadFailed ||
        failure.kind == SourcePreferenceFailureKind::WriteFailed ||
        failure.kind == SourcePreferenceFailureKind::SyncFailed ||
        failure.kind == SourcePreferenceFailureKind::RenameFailed ||
        failure.kind == SourcePreferenceFailureKind::RemoveFailed ||
        failure.kind == SourcePreferenceFailureKind::CloseFailed;
    const bool requires_file_type =
        failure.kind ==
        SourcePreferenceFailureKind::UnsupportedFileType;
    const std::string path = failure.entry_path.empty()
                                 ? requires_path
                                       ? unavailable_display(
                                             state,
                                             UnifiedPlanRenderingSection::Blockers,
                                             blocker_index, std::nullopt,
                                             localization::translate_message(
                                                 "A source preference failure is missing its affected path."))
                                       : localization::translate_message("not observed")
                                 : terminal_safe_text_display(failure.entry_path.generic_string());
    const std::string system_error = failure.system_error.has_value()
                                         ? route_system_error_display(failure.system_error)
                                     : requires_system_error
                                         ? unavailable_display(
                                               state,
                                               UnifiedPlanRenderingSection::Blockers,
                                               blocker_index, std::nullopt,
                                               localization::translate_message(
                                                   "A source preference syscall failure is missing its system error."))
                                         : localization::translate_message("not observed");
    const std::string file_type = failure.observed_file_type.has_value()
                                      ? filesystem_file_type_display(
                                            failure.observed_file_type.value(), blocker_index,
                                            state)
                                  : requires_file_type
                                      ? unavailable_display(
                                            state,
                                            UnifiedPlanRenderingSection::Blockers,
                                            blocker_index, std::nullopt,
                                            localization::translate_message(
                                                "An unsupported source preference entry is missing its observed file type."))
                                      : localization::translate_message("not observed");
    return localization::format_translated_message(
        "{}; kind: {}; path: {}; system error: {}; observed file type: {}; diagnostic: {}",
        "SourcePreferenceFailure",
        source_preference_failure_kind_display(
            failure.kind, blocker_index, state),
        path, system_error, file_type,
        required_string_display(
            failure.diagnostic, state,
            UnifiedPlanRenderingSection::Blockers, blocker_index,
            std::nullopt,
            localization::translate_message(
                "A source preference failure is missing its diagnostic.")));
}

std::string sync_install_preparation_issue_kind_display(
    SyncInstallPreparationIssueKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case SyncInstallPreparationIssueKind::UnsupportedSourceSelection:
            return "SyncInstallPreparationIssueKind::UnsupportedSourceSelection";
        case SyncInstallPreparationIssueKind::MissingAurTarget:
            return "SyncInstallPreparationIssueKind::MissingAurTarget";
        case SyncInstallPreparationIssueKind::InvalidTarget:
            return "SyncInstallPreparationIssueKind::InvalidTarget";
        case SyncInstallPreparationIssueKind::TargetCorrelationFailed:
            return "SyncInstallPreparationIssueKind::TargetCorrelationFailed";
        case SyncInstallPreparationIssueKind::UnsupportedSourceOption:
            return "SyncInstallPreparationIssueKind::UnsupportedSourceOption";
        case SyncInstallPreparationIssueKind::SourceBuildOptionsUnsupported:
            return "SyncInstallPreparationIssueKind::SourceBuildOptionsUnsupported";
        case SyncInstallPreparationIssueKind::RepositoryAuthorityChanged:
            return "SyncInstallPreparationIssueKind::RepositoryAuthorityChanged";
        case SyncInstallPreparationIssueKind::BuildPlanResolutionFailed:
            return "SyncInstallPreparationIssueKind::BuildPlanResolutionFailed";
        case SyncInstallPreparationIssueKind::BuildPlanBlocked:
            return "SyncInstallPreparationIssueKind::BuildPlanBlocked";
        case SyncInstallPreparationIssueKind::BuildPlanCorrelationFailed:
            return "SyncInstallPreparationIssueKind::BuildPlanCorrelationFailed";
        case SyncInstallPreparationIssueKind::SourceWorkPreparationFailed:
            return "SyncInstallPreparationIssueKind::SourceWorkPreparationFailed";
        case SyncInstallPreparationIssueKind::EmptyPreparedRoute:
            return "SyncInstallPreparationIssueKind::EmptyPreparedRoute";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A sync preparation issue kind is not supported by the renderer."));
}

std::string repository_metadata_failure_kind_display(
    RepositoryMetadataFailureKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case RepositoryMetadataFailureKind::ConfigurationUnavailable:
            return "RepositoryMetadataFailureKind::ConfigurationUnavailable";
        case RepositoryMetadataFailureKind::ConfigurationMalformed:
            return "RepositoryMetadataFailureKind::ConfigurationMalformed";
        case RepositoryMetadataFailureKind::SyncDatabaseUnavailable:
            return "RepositoryMetadataFailureKind::SyncDatabaseUnavailable";
        case RepositoryMetadataFailureKind::SyncDatabaseMalformed:
            return "RepositoryMetadataFailureKind::SyncDatabaseMalformed";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A repository metadata failure kind is not supported by the renderer."));
}

std::string sync_repository_metadata_failure_display(
    const SyncRepositoryMetadataReadFailure& failure,
    std::size_t blocker_index, RenderState& state) {
    const RepositoryMetadataFailure& metadata = failure.failure;
    std::vector<std::string> repository_order;
    if(metadata.configured_repository_order.has_value()) {
        repository_order.reserve(
            metadata.configured_repository_order->size());
        for(const std::string& repository :
            metadata.configured_repository_order.value()) {
            repository_order.push_back(
                terminal_safe_text_display(repository));
        }
    }
    return localization::format_translated_message(
        "{}; root: {}; kind: {}; repository: {}; configured repository order: {}; diagnostic: {}",
        "SyncRepositoryMetadataReadFailure",
        terminal_safe_text_display(
            root_target_identity_display(failure.root)),
        repository_metadata_failure_kind_display(
            metadata.kind, blocker_index, state),
        metadata.repository_name.has_value() &&
                !metadata.repository_name->empty()
            ? terminal_safe_text_display(
                  metadata.repository_name.value())
            : localization::translate_message("not observed"),
        repository_order.empty()
            ? localization::translate_message("not observed")
            : join_display_values(repository_order),
        metadata.diagnostic.empty()
            ? localization::translate_message("not observed")
            : terminal_safe_text_display(metadata.diagnostic));
}

std::string sync_install_preparation_failure_display(
    const SyncInstallPreparationFailure& failure,
    std::size_t blocker_index, RenderState& state) {
    if(failure.details.empty()) {
        return localization::format_translated_message(
            "sync preparation failure: {}",
            unavailable_display(
                state, UnifiedPlanRenderingSection::Blockers,
                blocker_index, std::nullopt,
                localization::translate_message(
                    "A sync preparation failure has no typed details.")));
    }
    std::vector<std::string> details;
    details.reserve(failure.details.size());
    for(const SyncInstallPreparationFailureDetail& detail : failure.details) {
        details.push_back(std::visit(
            [&](const auto& typed_detail) -> std::string {
                using Detail = std::decay_t<decltype(typed_detail)>;
                if constexpr(std::is_same_v<
                                 Detail,
                                 SyncInstallPreparationIssue>) {
                    return localization::format_translated_message(
                        "{}; kind: {}; root: {}; option: {}; diagnostic: {}",
                        "SyncInstallPreparationIssue",
                        sync_install_preparation_issue_kind_display(
                            typed_detail.kind, blocker_index,
                            state),
                        typed_detail.root.has_value()
                            ? terminal_safe_text_display(
                                  root_target_identity_display(
                                      typed_detail.root
                                          .value()))
                            : localization::translate_message(
                                  "not observed"),
                        typed_detail.option.has_value() &&
                                !typed_detail.option->empty()
                            ? terminal_safe_text_display(
                                  typed_detail.option.value())
                            : localization::translate_message(
                                  "not observed"),
                        typed_detail.diagnostic.empty()
                            ? localization::translate_message(
                                  "not observed")
                            : terminal_safe_text_display(
                                  typed_detail.diagnostic));
                } else if constexpr(std::is_same_v<
                                        Detail,
                                        SyncRepositoryMetadataReadFailure>) {
                    return sync_repository_metadata_failure_display(
                        typed_detail, blocker_index, state);
                } else {
                    return source_preference_failure_display(
                        typed_detail, blocker_index, state);
                }
            },
            detail));
    }
    return localization::format_translated_message(
        "sync preparation failure: {}", join_display_values(details));
}

std::string package_metadata_failure_display(
    const PackageMetadataFailure& failure, std::size_t blocker_index,
    RenderState& state) {
    return localization::format_translated_message(
        "{}; code: {}; diagnostic: {}", "PackageMetadataFailure",
        package_metadata_error_code_display(
            failure.code, blocker_index, state),
        required_string_display(
            failure.diagnostic, state,
            UnifiedPlanRenderingSection::Blockers, blocker_index,
            std::nullopt,
            localization::translate_message(
                "A package metadata failure is missing its diagnostic.")));
}

std::string xdg_directory_kind_display(
    xdg_paths::DirectoryKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case xdg_paths::DirectoryKind::Config:
            return "xdg_paths::DirectoryKind::Config";
        case xdg_paths::DirectoryKind::State:
            return "xdg_paths::DirectoryKind::State";
        case xdg_paths::DirectoryKind::Cache:
            return "xdg_paths::DirectoryKind::Cache";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} directory kind is not supported by the renderer.",
            "XDG"));
}

std::string xdg_environment_variable_display(
    xdg_paths::EnvironmentVariable variable, std::size_t blocker_index,
    RenderState& state) {
    switch(variable) {
        case xdg_paths::EnvironmentVariable::XdgConfigHome:
            return "XDG_CONFIG_HOME";
        case xdg_paths::EnvironmentVariable::XdgStateHome:
            return "XDG_STATE_HOME";
        case xdg_paths::EnvironmentVariable::XdgCacheHome:
            return "XDG_CACHE_HOME";
        case xdg_paths::EnvironmentVariable::Home:
            return "HOME";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} environment variable is not supported by the renderer.",
            "XDG"));
}

std::string xdg_resolution_error_code_display(
    xdg_paths::ResolutionErrorCode code, std::size_t blocker_index,
    RenderState& state) {
    switch(code) {
        case xdg_paths::ResolutionErrorCode::MissingHome:
            return "xdg_paths::ResolutionErrorCode::MissingHome";
        case xdg_paths::ResolutionErrorCode::EmptyHome:
            return "xdg_paths::ResolutionErrorCode::EmptyHome";
        case xdg_paths::ResolutionErrorCode::RelativePath:
            return "xdg_paths::ResolutionErrorCode::RelativePath";
        case xdg_paths::ResolutionErrorCode::DotComponent:
            return "xdg_paths::ResolutionErrorCode::DotComponent";
        case xdg_paths::ResolutionErrorCode::AmbiguousLeadingDoubleSlash:
            return "xdg_paths::ResolutionErrorCode::AmbiguousLeadingDoubleSlash";
        case xdg_paths::ResolutionErrorCode::EmbeddedNull:
            return "xdg_paths::ResolutionErrorCode::EmbeddedNull";
        case xdg_paths::ResolutionErrorCode::MalformedPath:
            return "xdg_paths::ResolutionErrorCode::MalformedPath";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} path resolution error code is not supported by the renderer.",
            "XDG"));
}

std::string xdg_resolution_failure_display(
    const xdg_paths::ResolutionFailure& failure,
    std::size_t blocker_index, RenderState& state) {
    return localization::format_translated_message(
        "{}; directory: {}; environment variable: {}; code: {}",
        "xdg_paths::ResolutionFailure",
        xdg_directory_kind_display(
            failure.directory_kind, blocker_index, state),
        xdg_environment_variable_display(
            failure.environment_variable, blocker_index, state),
        xdg_resolution_error_code_display(
            failure.code, blocker_index, state));
}

std::string xdg_preparation_stage_display(
    xdg_directory_safety::PreparationStage stage,
    std::size_t blocker_index, RenderState& state) {
    switch(stage) {
        case xdg_directory_safety::PreparationStage::BoundaryValidation:
            return "xdg_directory_safety::PreparationStage::BoundaryValidation";
        case xdg_directory_safety::PreparationStage::FilesystemRootOpen:
            return "xdg_directory_safety::PreparationStage::FilesystemRootOpen";
        case xdg_directory_safety::PreparationStage::AnchorTraversal:
            return "xdg_directory_safety::PreparationStage::AnchorTraversal";
        case xdg_directory_safety::PreparationStage::AnchorValidation:
            return "xdg_directory_safety::PreparationStage::AnchorValidation";
        case xdg_directory_safety::PreparationStage::ComponentInspection:
            return "xdg_directory_safety::PreparationStage::ComponentInspection";
        case xdg_directory_safety::PreparationStage::ComponentCreation:
            return "xdg_directory_safety::PreparationStage::ComponentCreation";
        case xdg_directory_safety::PreparationStage::ComponentOpen:
            return "xdg_directory_safety::PreparationStage::ComponentOpen";
        case xdg_directory_safety::PreparationStage::ComponentValidation:
            return "xdg_directory_safety::PreparationStage::ComponentValidation";
        case xdg_directory_safety::PreparationStage::DirectoryRevalidation:
            return "xdg_directory_safety::PreparationStage::DirectoryRevalidation";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} directory preparation stage is not supported by the renderer.",
            "XDG"));
}

std::string xdg_preparation_error_code_display(
    xdg_directory_safety::PreparationErrorCode code,
    std::size_t blocker_index, RenderState& state) {
    switch(code) {
        case xdg_directory_safety::PreparationErrorCode::MissingAnchor:
            return "xdg_directory_safety::PreparationErrorCode::MissingAnchor";
        case xdg_directory_safety::PreparationErrorCode::Symlink:
            return "xdg_directory_safety::PreparationErrorCode::Symlink";
        case xdg_directory_safety::PreparationErrorCode::NotDirectory:
            return "xdg_directory_safety::PreparationErrorCode::NotDirectory";
        case xdg_directory_safety::PreparationErrorCode::OwnershipMismatch:
            return "xdg_directory_safety::PreparationErrorCode::OwnershipMismatch";
        case xdg_directory_safety::PreparationErrorCode::UnsafePermissions:
            return "xdg_directory_safety::PreparationErrorCode::UnsafePermissions";
        case xdg_directory_safety::PreparationErrorCode::PermissionDenied:
            return "xdg_directory_safety::PreparationErrorCode::PermissionDenied";
        case xdg_directory_safety::PreparationErrorCode::CreationFailed:
            return "xdg_directory_safety::PreparationErrorCode::CreationFailed";
        case xdg_directory_safety::PreparationErrorCode::MetadataFailure:
            return "xdg_directory_safety::PreparationErrorCode::MetadataFailure";
        case xdg_directory_safety::PreparationErrorCode::ConcurrentReplacement:
            return "xdg_directory_safety::PreparationErrorCode::ConcurrentReplacement";
        case xdg_directory_safety::PreparationErrorCode::InvalidCreationBoundary:
            return "xdg_directory_safety::PreparationErrorCode::InvalidCreationBoundary";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} directory preparation error code is not supported by the renderer.",
            "XDG"));
}

std::string xdg_preparation_failure_display(
    const xdg_directory_safety::PreparationFailure& failure,
    std::size_t blocker_index, RenderState& state) {
    return localization::format_translated_message(
        "{}; directory: {}; stage: {}; code: {}; system error: {}; component index: {}",
        "xdg_directory_safety::PreparationFailure",
        xdg_directory_kind_display(
            failure.directory_kind, blocker_index, state),
        xdg_preparation_stage_display(
            failure.stage, blocker_index, state),
        xdg_preparation_error_code_display(
            failure.code, blocker_index, state),
        route_system_error_display(failure.system_error),
        optional_index_display(failure.component_index));
}

std::string trusted_cache_stage_display(
    TrustedCacheStage stage, std::size_t blocker_index,
    RenderState& state) {
    switch(stage) {
        case TrustedCacheStage::RootAdoption:
            return "TrustedCacheStage::RootAdoption";
        case TrustedCacheStage::RootRevalidation:
            return "TrustedCacheStage::RootRevalidation";
        case TrustedCacheStage::ChildValidation:
            return "TrustedCacheStage::ChildValidation";
        case TrustedCacheStage::ChildCreation:
            return "TrustedCacheStage::ChildCreation";
        case TrustedCacheStage::ChildOpen:
            return "TrustedCacheStage::ChildOpen";
        case TrustedCacheStage::CleanupPreflight:
            return "TrustedCacheStage::CleanupPreflight";
        case TrustedCacheStage::RecursiveRemoval:
            return "TrustedCacheStage::RecursiveRemoval";
        case TrustedCacheStage::Rollback:
            return "TrustedCacheStage::Rollback";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A trusted cache stage is not supported by the renderer."));
}

std::string trusted_cache_error_code_display(
    TrustedCacheErrorCode code, std::size_t blocker_index,
    RenderState& state) {
    switch(code) {
        case TrustedCacheErrorCode::InvalidBoundary:
            return "TrustedCacheErrorCode::InvalidBoundary";
        case TrustedCacheErrorCode::Symlink:
            return "TrustedCacheErrorCode::Symlink";
        case TrustedCacheErrorCode::NotDirectory:
            return "TrustedCacheErrorCode::NotDirectory";
        case TrustedCacheErrorCode::NotRegularFile:
            return "TrustedCacheErrorCode::NotRegularFile";
        case TrustedCacheErrorCode::OwnershipMismatch:
            return "TrustedCacheErrorCode::OwnershipMismatch";
        case TrustedCacheErrorCode::UnsafePermissions:
            return "TrustedCacheErrorCode::UnsafePermissions";
        case TrustedCacheErrorCode::PermissionDenied:
            return "TrustedCacheErrorCode::PermissionDenied";
        case TrustedCacheErrorCode::MetadataFailure:
            return "TrustedCacheErrorCode::MetadataFailure";
        case TrustedCacheErrorCode::ConcurrentReplacement:
            return "TrustedCacheErrorCode::ConcurrentReplacement";
        case TrustedCacheErrorCode::ChildEscape:
            return "TrustedCacheErrorCode::ChildEscape";
        case TrustedCacheErrorCode::CreationFailure:
            return "TrustedCacheErrorCode::CreationFailure";
        case TrustedCacheErrorCode::CleanupPreflightFailure:
            return "TrustedCacheErrorCode::CleanupPreflightFailure";
        case TrustedCacheErrorCode::RemovalFailure:
            return "TrustedCacheErrorCode::RemovalFailure";
        case TrustedCacheErrorCode::RollbackRefusal:
            return "TrustedCacheErrorCode::RollbackRefusal";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "A trusted cache error code is not supported by the renderer."));
}

std::string trusted_cache_failure_display(
    const TrustedCacheFailure& failure, std::size_t blocker_index,
    RenderState& state) {
    return localization::format_translated_message(
        "{}; stage: {}; code: {}; system error: {}",
        "TrustedCacheFailure",
        trusted_cache_stage_display(
            failure.stage, blocker_index, state),
        trusted_cache_error_code_display(
            failure.code, blocker_index, state),
        route_system_error_display(failure.system_error));
}

std::string upgrade_all_issue_kind_display(
    UpgradeAllOperationIssueKind kind, std::size_t blocker_index,
    RenderState& state) {
    switch(kind) {
        case UpgradeAllOperationIssueKind::ExplicitSourceAdapterInvalid:
            return "UpgradeAllOperationIssueKind::ExplicitSourceAdapterInvalid";
        case UpgradeAllOperationIssueKind::OptionSnapshotMismatch:
            return "UpgradeAllOperationIssueKind::OptionSnapshotMismatch";
        case UpgradeAllOperationIssueKind::SourceSnapshotMismatch:
            return "UpgradeAllOperationIssueKind::SourceSnapshotMismatch";
        case UpgradeAllOperationIssueKind::ExplicitSourceCorrelationInconsistent:
            return "UpgradeAllOperationIssueKind::ExplicitSourceCorrelationInconsistent";
        case UpgradeAllOperationIssueKind::PreparedCapabilityConsumed:
            return "UpgradeAllOperationIssueKind::PreparedCapabilityConsumed";
        case UpgradeAllOperationIssueKind::SystemSourceExecutionFailedUnexpectedly:
            return "UpgradeAllOperationIssueKind::SystemSourceExecutionFailedUnexpectedly";
        case UpgradeAllOperationIssueKind::SystemSourcePhaseIncomplete:
            return "UpgradeAllOperationIssueKind::SystemSourcePhaseIncomplete";
        case UpgradeAllOperationIssueKind::ForeignInventoryConfigurationFailed:
            return "UpgradeAllOperationIssueKind::ForeignInventoryConfigurationFailed";
        case UpgradeAllOperationIssueKind::ForeignInventoryReadFailed:
            return "UpgradeAllOperationIssueKind::ForeignInventoryReadFailed";
        case UpgradeAllOperationIssueKind::CacheAuthorityInvalid:
            return "UpgradeAllOperationIssueKind::CacheAuthorityInvalid";
        case UpgradeAllOperationIssueKind::AurQueryFailed:
            return "UpgradeAllOperationIssueKind::AurQueryFailed";
        case UpgradeAllOperationIssueKind::FilteredAurPreparationFailed:
            return "UpgradeAllOperationIssueKind::FilteredAurPreparationFailed";
        case UpgradeAllOperationIssueKind::FilteredAurExecutionFailed:
            return "UpgradeAllOperationIssueKind::FilteredAurExecutionFailed";
        case UpgradeAllOperationIssueKind::DuplicateExclusionCorrelationInconsistent:
            return "UpgradeAllOperationIssueKind::DuplicateExclusionCorrelationInconsistent";
        case UpgradeAllOperationIssueKind::ExternalSatisfactionCorrelationInconsistent:
            return "UpgradeAllOperationIssueKind::ExternalSatisfactionCorrelationInconsistent";
        case UpgradeAllOperationIssueKind::UnknownFailure:
            return "UpgradeAllOperationIssueKind::UnknownFailure";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} issue kind is not supported by the renderer.",
            "upgrade-all"));
}

std::string aur_update_preparation_reason_display(
    AurUpdatePreparationReason reason, std::size_t blocker_index,
    RenderState& state) {
    switch(reason) {
        case AurUpdatePreparationReason::None:
            return "AurUpdatePreparationReason::None";
        case AurUpdatePreparationReason::BlockingPreflight:
            return "AurUpdatePreparationReason::BlockingPreflight";
        case AurUpdatePreparationReason::PreflightInconsistent:
            return "AurUpdatePreparationReason::PreflightInconsistent";
        case AurUpdatePreparationReason::
            DevelRequiresCheckPolicyInconsistent:
            return "AurUpdatePreparationReason::DevelRequiresCheckPolicyInconsistent";
        case AurUpdatePreparationReason::BuildPlanMissing:
            return "AurUpdatePreparationReason::BuildPlanMissing";
        case AurUpdatePreparationReason::BuildPlanOrderEmpty:
            return "AurUpdatePreparationReason::BuildPlanOrderEmpty";
        case AurUpdatePreparationReason::RootAttributionInconsistent:
            return "AurUpdatePreparationReason::RootAttributionInconsistent";
        case AurUpdatePreparationReason::PackageTargetAttributionInconsistent:
            return "AurUpdatePreparationReason::PackageTargetAttributionInconsistent";
        case AurUpdatePreparationReason::DesiredInstallReasonMissing:
            return "AurUpdatePreparationReason::DesiredInstallReasonMissing";
        case AurUpdatePreparationReason::SourcePreferenceUnavailable:
            return "AurUpdatePreparationReason::SourcePreferenceUnavailable";
        case AurUpdatePreparationReason::SourcePreferencePkgdestConflict:
            return "AurUpdatePreparationReason::SourcePreferencePkgdestConflict";
        case AurUpdatePreparationReason::StaticWorkItemInvalid:
            return "AurUpdatePreparationReason::StaticWorkItemInvalid";
        case AurUpdatePreparationReason::PacmanDatabaseUnavailable:
            return "AurUpdatePreparationReason::PacmanDatabaseUnavailable";
        case AurUpdatePreparationReason::GenericPreparationInconsistent:
            return "AurUpdatePreparationReason::GenericPreparationInconsistent";
        case AurUpdatePreparationReason::BuildUnitSelectionInconsistent:
            return "AurUpdatePreparationReason::BuildUnitSelectionInconsistent";
        case AurUpdatePreparationReason::ExternalSatisfactionInconsistent:
            return "AurUpdatePreparationReason::ExternalSatisfactionInconsistent";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "An {} source preparation reason is not supported by the renderer.",
            "AUR"));
}

std::string aur_update_preparation_issue_display(
    const AurUpdatePreparationIssue& issue,
    std::size_t blocker_index, RenderState& state) {
    std::vector<std::string> update_indices;
    update_indices.reserve(issue.affected_update_plan_indices.size());
    for(const std::size_t index : issue.affected_update_plan_indices) {
        update_indices.push_back(std::to_string(index));
    }
    std::vector<std::string> roots;
    roots.reserve(issue.affected_roots.size());
    for(const RootTargetIdentity& root : issue.affected_roots) {
        roots.push_back(root_target_identity_display(root));
    }
    std::vector<std::string> nested_details;
    if(issue.preflight_issue.has_value()) {
        const AurUpdateExecutionIssue& preflight =
            issue.preflight_issue.value();
        nested_details.push_back(localization::format_translated_message(
            "{}; reason: {}; diagnostic: {}",
            "AurUpdateExecutionIssue",
            aur_update_execution_reason_display(
                preflight.reason, blocker_index, state),
            preflight.diagnostic.empty()
                ? localization::translate_message("not observed")
                : terminal_safe_text_display(preflight.diagnostic)));
    }
    if(issue.source_preference_failure.has_value()) {
        nested_details.push_back(source_preference_failure_display(
            issue.source_preference_failure.value(), blocker_index,
            state));
    }
    if(issue.package_metadata_failure.has_value()) {
        nested_details.push_back(package_metadata_failure_display(
            issue.package_metadata_failure.value(), blocker_index,
            state));
    }
    if(issue.build_plan_projection_issue.has_value()) {
        nested_details.push_back(
            build_plan_artifact_projection_issue_display(
                issue.build_plan_projection_issue.value(),
                blocker_index, state));
    }
    return localization::format_translated_message(
        "{} source preparation failure ({}); update plan indices: {}; roots: {}; package: {}; {}: {}; typed nested details: {}; diagnostic: {}",
        "AUR",
        aur_update_preparation_reason_display(
            issue.reason, blocker_index, state),
        update_indices.empty()
            ? localization::translate_message("None")
            : join_display_values(update_indices),
        roots.empty() ? localization::translate_message("None")
                      : join_display_values(roots),
        optional_string_display(issue.package_name), "PackageBase",
        optional_string_display(issue.package_base),
        nested_details.empty()
            ? localization::translate_message("None")
            : join_display_values(nested_details),
        required_string_display(
            issue.diagnostic, state,
            UnifiedPlanRenderingSection::Blockers, blocker_index,
            std::nullopt,
            localization::format_translated_message(
                "An {} source preparation issue is missing its diagnostic.",
                "AUR")));
}

std::string filtered_aur_observation_issue_kind_display(
    FilteredAurUpdateOperationIssueKind kind,
    std::size_t blocker_index, RenderState& state) {
    switch(kind) {
        case FilteredAurUpdateOperationIssueKind::UnknownUpdateClassification:
            return "FilteredAurUpdateOperationIssueKind::UnknownUpdateClassification";
        case FilteredAurUpdateOperationIssueKind::
            DevelRequiresCheckPolicyInconsistent:
            return "FilteredAurUpdateOperationIssueKind::DevelRequiresCheckPolicyInconsistent";
        case FilteredAurUpdateOperationIssueKind::TargetPlannerMappingInconsistent:
            return "FilteredAurUpdateOperationIssueKind::TargetPlannerMappingInconsistent";
        case FilteredAurUpdateOperationIssueKind::FilteredTargetMappingInconsistent:
            return "FilteredAurUpdateOperationIssueKind::FilteredTargetMappingInconsistent";
        case FilteredAurUpdateOperationIssueKind::PreflightTargetMappingInconsistent:
            return "FilteredAurUpdateOperationIssueKind::PreflightTargetMappingInconsistent";
        case FilteredAurUpdateOperationIssueKind::PreflightInvocationIndexOutOfRange:
            return "FilteredAurUpdateOperationIssueKind::PreflightInvocationIndexOutOfRange";
        case FilteredAurUpdateOperationIssueKind::PreflightInvocationIdentityMismatch:
            return "FilteredAurUpdateOperationIssueKind::PreflightInvocationIdentityMismatch";
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexMissing:
            return "FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexMissing";
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexOutOfRange:
            return "FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexOutOfRange";
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootIdentityMismatch:
            return "FilteredAurUpdateOperationIssueKind::BuildPlanRootIdentityMismatch";
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootPackageIdentityMismatch:
            return "FilteredAurUpdateOperationIssueKind::BuildPlanRootPackageIdentityMismatch";
        case FilteredAurUpdateOperationIssueKind::BuildUnitOrderIdentityMismatch:
            return "FilteredAurUpdateOperationIssueKind::BuildUnitOrderIdentityMismatch";
        case FilteredAurUpdateOperationIssueKind::BuildUnitRootAttributionInconsistent:
            return "FilteredAurUpdateOperationIssueKind::BuildUnitRootAttributionInconsistent";
        case FilteredAurUpdateOperationIssueKind::BuildUnitSelectionMappingInconsistent:
            return "FilteredAurUpdateOperationIssueKind::BuildUnitSelectionMappingInconsistent";
        case FilteredAurUpdateOperationIssueKind::ExecutionBuildUnitMappingInconsistent:
            return "FilteredAurUpdateOperationIssueKind::ExecutionBuildUnitMappingInconsistent";
        case FilteredAurUpdateOperationIssueKind::ReducedTargetMappingInconsistent:
            return "FilteredAurUpdateOperationIssueKind::ReducedTargetMappingInconsistent";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "A filtered {} observation issue kind is not supported by the renderer.",
            "AUR"));
}

std::string system_aur_dry_run_issue_kind_display(
    SystemAurUpdateDryRunIssueKind kind,
    std::size_t blocker_index, RenderState& state) {
    switch(kind) {
        case SystemAurUpdateDryRunIssueKind::RepositoryConfigurationFailure:
            return "SystemAurUpdateDryRunIssueKind::RepositoryConfigurationFailure";
        case SystemAurUpdateDryRunIssueKind::ForeignInventoryFailure:
            return "SystemAurUpdateDryRunIssueKind::ForeignInventoryFailure";
        case SystemAurUpdateDryRunIssueKind::AurQueryFailure:
            return "SystemAurUpdateDryRunIssueKind::AurQueryFailure";
        case SystemAurUpdateDryRunIssueKind::AurAssessmentFailure:
            return "SystemAurUpdateDryRunIssueKind::AurAssessmentFailure";
        case SystemAurUpdateDryRunIssueKind::InconsistentObservation:
            return "SystemAurUpdateDryRunIssueKind::InconsistentObservation";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::format_translated_message(
            "A system/{} dry-run issue kind is not supported by the renderer.",
            "AUR"));
}

std::string route_preflight_blocker_display(
    const RoutePreflightUnifiedPlanBlocker& blocker,
    std::size_t blocker_index, RenderState& state) {
    return std::visit(
        [&](const auto& reference) -> std::string {
            using Reference = std::decay_t<decltype(reference)>;
            const auto& detail = reference.get();
            if constexpr(std::is_same_v<
                             Reference,
                             UnifiedPlanBorrowedAuthorityReference<
                                 AurUpdateExecutionIssue>>) {
                if(!detail.package_name.has_value() &&
                   !detail.package_base.has_value() &&
                   !detail.dependency_specification.has_value() &&
                   detail.diagnostic.empty() &&
                   !detail.build_plan_projection_issue.has_value()) {
                    state.add_issue(
                        UnifiedPlanRenderingIssueKind::
                            MissingReferencedValue,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "An {} route preflight blocker has no affected identity or diagnostic detail.",
                            "AUR"));
                }
                const std::string artifact_projection =
                    detail.build_plan_projection_issue.has_value()
                        ? build_plan_artifact_projection_issue_display(
                              detail.build_plan_projection_issue
                                  .value(),
                              blocker_index, state)
                        : localization::translate_message(
                              "not observed");
                if(detail.relation_reason.has_value()) {
                    return localization::format_translated_message(
                        "{} route preflight failure ({}); package: {}; {}: {}; dependency: {}; artifact projection: {}; relation assessment: {}",
                        "AUR",
                        aur_update_execution_reason_display(
                            detail.reason, blocker_index, state),
                        optional_string_display(detail.package_name),
                        "PackageBase",
                        optional_string_display(detail.package_base),
                        optional_string_display(
                            detail.dependency_specification),
                        artifact_projection,
                        terminal_safe_text_display(
                            package_relation_assessment_diagnostic_display(
                                detail.relation_reason->assessment)));
                }
                return localization::format_translated_message(
                    "{} route preflight failure ({}); package: {}; {}: {}; dependency: {}; artifact projection: {}; diagnostic: {}",
                    "AUR",
                    aur_update_execution_reason_display(
                        detail.reason, blocker_index, state),
                    optional_string_display(detail.package_name),
                    "PackageBase",
                    optional_string_display(detail.package_base),
                    optional_string_display(
                        detail.dependency_specification),
                    artifact_projection,
                    detail.diagnostic.empty()
                        ? localization::translate_message(
                              "not observed")
                        : terminal_safe_text_display(detail.diagnostic));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        AurUpdatePreparationIssue>>) {
                return aur_update_preparation_issue_display(
                    detail, blocker_index, state);
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        FilteredAurUpdateOperationIssue>>) {
                return localization::format_translated_message(
                    "Filtered {} observation inconsistency ({}); package: {}; {}: {}; diagnostic: {}",
                    "AUR",
                    filtered_aur_observation_issue_kind_display(
                        detail.kind, blocker_index, state),
                    optional_string_display(detail.package_name),
                    "PackageBase",
                    optional_string_display(detail.package_base),
                    required_string_display(
                        detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "A filtered {} observation issue is missing its diagnostic.",
                            "AUR")));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        SystemAurUpdateDryRunIssue>>) {
                std::vector<std::string> nested_details;
                if(detail.package_metadata_failure.has_value()) {
                    nested_details.push_back(
                        package_metadata_failure_display(
                            *detail.package_metadata_failure,
                            blocker_index, state));
                }
                if(!detail.diagnostic.empty()) {
                    nested_details.push_back(
                        terminal_safe_text_display(detail.diagnostic));
                }
                return localization::format_translated_message(
                    "System/{} current-state observation failure ({}); details: {}",
                    "AUR",
                    system_aur_dry_run_issue_kind_display(
                        detail.kind, blocker_index, state),
                    nested_details.empty()
                        ? localization::translate_message(
                              "not observed")
                        : join_display_values(nested_details));
            } else if constexpr(std::is_same_v<
                                    Reference,
                                    UnifiedPlanBorrowedAuthorityReference<
                                        SystemSourceUpgradeIssue>>) {
                const bool requires_source_identity =
                    detail.kind ==
                        SystemSourceUpgradeIssueKind::
                            PreferenceUnavailable ||
                    detail.kind ==
                        SystemSourceUpgradeIssueKind::
                            SourceIdentityResolutionFailed ||
                    detail.kind ==
                        SystemSourceUpgradeIssueKind::
                            SourceWorkItemPreparationFailed ||
                    detail.kind ==
                        SystemSourceUpgradeIssueKind::
                            InvalidPreferenceName;
                std::vector<std::string> nested_details;
                if(detail.source_preference_failure.has_value()) {
                    nested_details.push_back(
                        source_preference_failure_display(
                            detail.source_preference_failure.value(),
                            blocker_index, state));
                }
                if(detail.package_metadata_failure.has_value()) {
                    nested_details.push_back(package_metadata_failure_display(
                        detail.package_metadata_failure.value(),
                        blocker_index, state));
                }
                if(detail.cache_resolution_failure.has_value()) {
                    nested_details.push_back(xdg_resolution_failure_display(
                        detail.cache_resolution_failure.value(),
                        blocker_index, state));
                }
                if(detail.cache_preparation_failure.has_value()) {
                    nested_details.push_back(xdg_preparation_failure_display(
                        detail.cache_preparation_failure.value(),
                        blocker_index, state));
                }
                if(detail.trusted_cache_failure.has_value()) {
                    nested_details.push_back(trusted_cache_failure_display(
                        detail.trusted_cache_failure.value(),
                        blocker_index, state));
                }
                if(detail.kind ==
                       SystemSourceUpgradeIssueKind::
                           SystemPackageSnapshotUnavailable &&
                   !detail.package_metadata_failure.has_value()) {
                    nested_details.push_back(unavailable_display(
                        state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A system package snapshot issue is missing its package metadata failure.")));
                }
                if(detail.kind ==
                       SystemSourceUpgradeIssueKind::
                           CacheAuthorityInvalid &&
                   !detail.cache_resolution_failure.has_value() &&
                   !detail.cache_preparation_failure.has_value() &&
                   !detail.trusted_cache_failure.has_value()) {
                    nested_details.push_back(unavailable_display(
                        state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A system/source cache authority issue is missing its typed cache failure.")));
                }
                const std::string preference_index =
                    detail.original_preference_index.has_value()
                        ? std::to_string(
                              detail.original_preference_index.value())
                    : requires_source_identity
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::translate_message(
                                  "A system/source issue is missing its source-preference index."))
                        : localization::translate_message(
                              "not observed");
                const std::string package =
                    detail.preference_package_name.has_value()
                        ? required_string_display(
                              detail.preference_package_name.value(),
                              state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt,
                              localization::translate_message(
                                  "A system/source issue has an empty affected package identity."))
                    : requires_source_identity
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::translate_message(
                                  "A system/source issue is missing its affected package identity."))
                        : localization::translate_message(
                              "not observed");
                const std::string package_base =
                    detail.resolved_package_base.has_value()
                        ? required_string_display(
                              detail.resolved_package_base.value(),
                              state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt,
                              localization::format_translated_message(
                                  "A system/source issue has an empty affected {} identity.",
                                  "PackageBase"))
                        : localization::translate_message("not observed");
                return localization::format_translated_message(
                    "system/source route preflight failure ({}); impact: {}; phase: {}; preference index: {}; package: {}; {}: {}; typed nested details: {}; diagnostic: {}",
                    system_source_issue_kind_display(
                        detail.kind, blocker_index, state),
                    system_source_issue_impact_display(
                        detail.impact, blocker_index, state),
                    system_source_phase_display(
                        detail.phase, state, blocker_index),
                    preference_index, package, "PackageBase",
                    package_base,
                    nested_details.empty()
                        ? localization::translate_message("None")
                        : join_display_values(nested_details),
                    required_string_display(
                        detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A system/source issue is missing its diagnostic.")));
            } else {
                std::vector<std::string> nested_details;
                if(detail.package_metadata_failure.has_value()) {
                    nested_details.push_back(package_metadata_failure_display(
                        detail.package_metadata_failure.value(),
                        blocker_index, state));
                }
                if(detail.cache_resolution_failure.has_value()) {
                    nested_details.push_back(xdg_resolution_failure_display(
                        detail.cache_resolution_failure.value(),
                        blocker_index, state));
                }
                if(detail.cache_preparation_failure.has_value()) {
                    nested_details.push_back(xdg_preparation_failure_display(
                        detail.cache_preparation_failure.value(),
                        blocker_index, state));
                }
                if(detail.trusted_cache_failure.has_value()) {
                    nested_details.push_back(trusted_cache_failure_display(
                        detail.trusted_cache_failure.value(),
                        blocker_index, state));
                }
                if((detail.kind ==
                        UpgradeAllOperationIssueKind::
                            ForeignInventoryConfigurationFailed ||
                    detail.kind ==
                        UpgradeAllOperationIssueKind::
                            ForeignInventoryReadFailed) &&
                   !detail.package_metadata_failure.has_value()) {
                    nested_details.push_back(unavailable_display(
                        state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "An {} foreign inventory issue is missing its package metadata failure.",
                            "upgrade-all")));
                }
                const bool requires_adapter_index =
                    detail.kind ==
                    UpgradeAllOperationIssueKind::
                        DuplicateExclusionCorrelationInconsistent;
                const bool requires_build_plan_index =
                    detail.kind ==
                    UpgradeAllOperationIssueKind::
                        ExternalSatisfactionCorrelationInconsistent;
                const std::string adapter_index =
                    detail.adapter_index.has_value()
                        ? std::to_string(detail.adapter_index.value())
                    : requires_adapter_index
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::format_translated_message(
                                  "An {} exclusion-correlation issue is missing its adapter index.",
                                  "upgrade-all"))
                        : localization::translate_message(
                              "not observed");
                const std::string build_plan_index =
                    detail.build_plan_order_index.has_value()
                        ? std::to_string(
                              detail.build_plan_order_index.value())
                    : requires_build_plan_index
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::format_translated_message(
                                  "An {} external-satisfaction issue is missing its {} unit index.",
                                  "upgrade-all",
                                  "BuildPlan"))
                        : localization::translate_message(
                              "not observed");
                const std::string package =
                    detail.package_name.has_value()
                        ? required_string_display(
                              detail.package_name.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt,
                              localization::format_translated_message(
                                  "An {} issue has an empty affected package identity.",
                                  "upgrade-all"))
                        : localization::translate_message("not observed");
                return localization::format_translated_message(
                    "{} route preflight failure ({}); phase: {}; adapter index: {}; preference index: {}; query plan index: {}; {} unit index: {}; package: {}; typed nested details: {}; diagnostic: {}",
                    "upgrade-all",
                    upgrade_all_issue_kind_display(
                        detail.kind, blocker_index, state),
                    upgrade_all_phase_display(
                        detail.phase, state, blocker_index),
                    adapter_index,
                    optional_index_display(
                        detail.original_preference_index),
                    optional_index_display(
                        detail.original_query_plan_index),
                    "BuildPlan", build_plan_index, package,
                    nested_details.empty()
                        ? localization::translate_message("None")
                        : join_display_values(nested_details),
                    required_string_display(
                        detail.diagnostic, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "An {} issue is missing its diagnostic.",
                            "upgrade-all")));
            }
        },
        blocker.detail);
}

std::string installed_version_state_display(
    InstalledVersionState status, std::size_t blocker_index,
    RenderState& state) {
    switch(status) {
        case InstalledVersionState::NotInstalled:
            return "InstalledVersionState::NotInstalled";
        case InstalledVersionState::SameVersion:
            return "InstalledVersionState::SameVersion";
        case InstalledVersionState::DifferentVersion:
            return "InstalledVersionState::DifferentVersion";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "An installed version state is not supported by the renderer."));
}

std::string existing_install_reason_display(
    ExistingInstallReason reason, std::size_t blocker_index,
    RenderState& state) {
    switch(reason) {
        case ExistingInstallReason::Explicit:
            return "ExistingInstallReason::Explicit";
        case ExistingInstallReason::Dependency:
            return "ExistingInstallReason::Dependency";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "An existing install reason is not supported by the renderer."));
}

std::string install_reason_directive_display(
    InstallReasonDirective directive, std::size_t blocker_index,
    RenderState& state) {
    switch(directive) {
        case InstallReasonDirective::Default:
            return "InstallReasonDirective::Default";
        case InstallReasonDirective::AsExplicit:
            return "InstallReasonDirective::AsExplicit";
        case InstallReasonDirective::AsDependency:
            return "InstallReasonDirective::AsDependency";
    }
    return unsupported_display(
        state, UnifiedPlanRenderingSection::Blockers, blocker_index,
        std::nullopt,
        localization::translate_message(
            "An install reason directive is not supported by the renderer."));
}

std::string blocker_display(
    const UnifiedPlanBlocker& blocker, std::size_t blocker_index,
    RenderState& state) {
    return std::visit(
        [&](const auto& typed_blocker) -> std::string {
            using Blocker = std::decay_t<decltype(typed_blocker)>;
            if constexpr(std::is_same_v<Blocker, UnknownUnifiedPlanBlocker>) {
                const BuildPlanDependencyEdge& edge =
                    typed_blocker.detail.get();
                const std::string selected_candidate =
                    edge.resolved_candidate.has_value()
                        ? resolved_candidate_display(
                              edge.resolved_candidate.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt)
                        : localization::translate_message("not observed");
                const std::string selected_provider =
                    edge.resolved_provider.has_value()
                        ? provider_identity_display(
                              edge.resolved_provider.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt)
                        : localization::translate_message("not observed");
                const std::string stored_constraint =
                    edge.constraint_evaluation.has_value()
                        ? constraint_evaluation_display(
                              edge.constraint_evaluation.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt)
                        : localization::translate_message("not observed");
                return localization::format_translated_message(
                    "unknown dependency blocker ({}); dependency: {}; parent: {} ({}: {}); dependency kind: {}; role: {}; resolved package: {} ({}: {}); selected candidate: {}; selected provider: {}; stored result: {}",
                    "UnknownUnifiedPlanBlocker",
                    required_string_display(
                        edge.dependency_spec, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "An unknown dependency blocker is missing its dependency specification.")),
                    required_string_display(
                        edge.parent_package_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "An unknown dependency blocker is missing its parent package identity.")),
                    "PackageBase",
                    required_string_display(
                        edge.parent_package_base, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "An unknown dependency blocker is missing its parent {} identity.",
                            "PackageBase")),
                    dependency_kind_display(
                        edge.kind, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt),
                    package_role_display(
                        edge.role, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt),
                    optional_string_display(
                        edge.resolved_package_name),
                    "PackageBase",
                    optional_string_display(
                        edge.resolved_package_base),
                    selected_candidate, selected_provider,
                    stored_constraint);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    AmbiguousUnifiedPlanBlocker>) {
                const AmbiguousProvidedDependency& ambiguous =
                    typed_blocker.detail.get();
                std::vector<std::string> candidates;
                candidates.reserve(ambiguous.candidates.size());
                for(const ProvidedDependency& candidate :
                    ambiguous.candidates) {
                    candidates.push_back(
                        localization::format_translated_message(
                            "{} (provided dependency: {}; capability: {}; package version: {})",
                            provider_identity_display(
                                candidate, state,
                                UnifiedPlanRenderingSection::
                                    Blockers,
                                blocker_index, std::nullopt),
                            candidate.provided_dependency_name.empty()
                                ? localization::translate_message(
                                      "not observed")
                                : candidate
                                      .provided_dependency_name,
                            candidate
                                    .provided_dependency_specification
                                    .empty()
                                ? localization::translate_message(
                                      "not observed")
                                : candidate
                                      .provided_dependency_specification,
                            optional_string_display(
                                candidate.package_version)));
                }
                return localization::format_translated_message(
                    "ambiguous provider blocker ({}); dependency: {}; candidates: {}",
                    "AmbiguousUnifiedPlanBlocker",
                    required_string_display(
                        ambiguous.dependency, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "An ambiguous provider blocker is missing its dependency identity.")),
                    candidates.empty()
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::translate_message(
                                  "An ambiguous provider blocker is missing its candidate identities."))
                        : join_display_values(candidates));
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    UnsupportedUnifiedPlanBlocker>) {
                const MixedPackageBaseInstallReasonUnsupported& detail =
                    typed_blocker.detail.get();
                std::vector<std::string> artifacts;
                artifacts.reserve(detail.selected_artifacts.size());
                for(const MixedPackageBaseInstallReasonArtifact& artifact :
                    detail.selected_artifacts) {
                    artifacts.push_back(
                        localization::format_translated_message(
                            "{} {} (desired reason: {}; installed version state: {}; existing reason: {}; directive: {})",
                            required_string_display(
                                artifact.identity.package_name,
                                state,
                                UnifiedPlanRenderingSection::
                                    Blockers,
                                blocker_index, std::nullopt,
                                localization::translate_message(
                                    "A mixed install-reason artifact is missing its package identity.")),
                            required_string_display(
                                artifact.identity.full_version,
                                state,
                                UnifiedPlanRenderingSection::
                                    Blockers,
                                blocker_index, std::nullopt,
                                localization::translate_message(
                                    "A mixed install-reason artifact is missing its version identity.")),
                            install_reason_display(
                                artifact.desired_reason,
                                state,
                                UnifiedPlanRenderingSection::
                                    Blockers,
                                blocker_index),
                            installed_version_state_display(
                                artifact.installed_version_state,
                                blocker_index, state),
                            artifact.existing_reason.has_value()
                                ? existing_install_reason_display(
                                      artifact
                                          .existing_reason
                                          .value(),
                                      blocker_index,
                                      state)
                                : localization::
                                      translate_message(
                                          "not observed"),
                            install_reason_directive_display(
                                artifact.directive,
                                blocker_index, state)));
                }
                return localization::format_translated_message(
                    "unsupported blocker ({}); mixed install reason for {} {}; artifacts: {}",
                    "MixedPackageBaseInstallReasonUnsupported",
                    "PackageBase",
                    required_string_display(
                        detail.package_base, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "A mixed install-reason blocker is missing its {} identity.",
                            "PackageBase")),
                    artifacts.empty()
                        ? unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::
                                  Blockers,
                              blocker_index, std::nullopt,
                              localization::translate_message(
                                  "A mixed install-reason blocker is missing its artifact details."))
                        : join_display_values(artifacts));
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    SourceFailureUnifiedPlanBlocker>) {
                return source_failure_detail_display(
                    typed_blocker, blocker_index, state);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    ConstraintFailureUnifiedPlanBlocker>) {
                const BuildPlanDependencyEdge& edge =
                    typed_blocker.detail.get();
                const std::string selected_candidate =
                    edge.resolved_candidate.has_value()
                        ? resolved_candidate_display(
                              edge.resolved_candidate.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt)
                        : localization::translate_message("not observed");
                const std::string selected_provider =
                    edge.resolved_provider.has_value()
                        ? provider_identity_display(
                              edge.resolved_provider.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt)
                        : localization::translate_message("not observed");
                const std::string stored_constraint =
                    edge.constraint_evaluation.has_value()
                        ? constraint_evaluation_display(
                              edge.constraint_evaluation.value(), state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt)
                        : unavailable_display(
                              state,
                              UnifiedPlanRenderingSection::Blockers,
                              blocker_index, std::nullopt,
                              localization::translate_message(
                                  "A constraint failure blocker is missing its stored constraint result."));
                return localization::format_translated_message(
                    "constraint failure blocker ({}); parent: {} ({}: {}); dependency: {}; dependency kind: {}; role: {}; resolved package: {} ({}: {}); selected candidate: {}; selected provider: {}; stored result: {}",
                    "ConstraintFailureUnifiedPlanBlocker",
                    required_string_display(
                        edge.parent_package_name, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A constraint failure blocker is missing its parent package identity.")),
                    "PackageBase",
                    required_string_display(
                        edge.parent_package_base, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::format_translated_message(
                            "A constraint failure blocker is missing its parent {} identity.",
                            "PackageBase")),
                    required_string_display(
                        edge.dependency_spec, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt,
                        localization::translate_message(
                            "A constraint failure blocker is missing its dependency specification.")),
                    dependency_kind_display(
                        edge.kind, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt),
                    package_role_display(
                        edge.role, state,
                        UnifiedPlanRenderingSection::Blockers,
                        blocker_index, std::nullopt),
                    optional_string_display(
                        edge.resolved_package_name),
                    "PackageBase",
                    optional_string_display(
                        edge.resolved_package_base),
                    selected_candidate, selected_provider,
                    stored_constraint);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    MetadataRiskUnifiedPlanBlocker>) {
                const PackageRelationAssessment& assessment =
                    typed_blocker.detail.assessment;
                return localization::format_translated_message(
                    "package relation blocker: {}",
                    terminal_safe_text_display(
                        package_relation_assessment_diagnostic_display(
                            assessment)));
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    LocalDependencyPlanUnifiedPlanBlocker>) {
                const LocalDependencyPlanFailure& failure =
                    typed_blocker.detail.get();
                return local_dependency_plan_failure_display(
                    failure, state,
                    UnifiedPlanRenderingSection::Blockers,
                    blocker_index, std::nullopt);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    LocalSourceMetadataEvaluationUnifiedPlanBlocker>) {
                return local_source_metadata_evaluation_blocker_display(
                    typed_blocker, blocker_index, state);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    RootPackagePreparationUnifiedPlanBlocker>) {
                const RootPackageInstallPreparationFailure& failure =
                    typed_blocker.detail.get();
                return root_package_preparation_failure_display(
                    failure, blocker_index, state);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    SyncInstallPreparationUnifiedPlanBlocker>) {
                const SyncInstallPreparationFailure& failure =
                    typed_blocker.detail.get();
                return sync_install_preparation_failure_display(
                    failure, blocker_index, state);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    BuildPlanArtifactProjectionUnifiedPlanBlocker>) {
                return build_plan_artifact_projection_issue_display(
                    typed_blocker.detail, blocker_index, state);
            } else if constexpr(std::is_same_v<
                                    Blocker,
                                    BuildPlanStateUnifiedPlanBlocker>) {
                return build_plan_state_blocker_display(
                    typed_blocker, blocker_index, state);
            } else {
                return route_preflight_blocker_display(
                    typed_blocker, blocker_index, state);
            }
        },
        blocker);
}

void render_blockers(
    const UnifiedPlanObservation& observation, RenderState& state) {
    state.output << localization::translate_message("Blockers:") << '\n';
    if(observation.blockers().empty()) {
        state.output << localization::translate_message("  None") << '\n';
        return;
    }
    for(std::size_t index = 0; index < observation.blockers().size();
        ++index) {
        state.output << localization::format_translated_message(
                            "  {}. {}", index + 1,
                            blocker_display(
                                observation.blockers()[index], index,
                                state))
                     << '\n';
    }
}

// Human-readable payloads retain valid printable UTF-8, while every terminal
// control, bidi control, BOM, invalid byte, and escape introducer stays visible.
std::string terminal_safe_text_display(std::string_view value) {
    return terminal_safe_text::escape_utf8(value);
}

// Legacy snapshot fields remain opaque because their surrounding display
// still uses delimiters to distinguish typed identities.
std::string invalid_snapshot_raw_value_display(std::string_view value) {
    std::string display;
    display.reserve(value.size());
    const auto append = [&display](std::string_view segment) {
        display.append(segment);
        return true;
    };
    for(const unsigned char byte : value) {
        const bool is_safe_identity_byte =
            (byte >= '0' && byte <= '9') ||
            (byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') || byte == '-' ||
            byte == '_' || byte == '.' || byte == '+' || byte == '@';
        if(is_safe_identity_byte) {
            display.push_back(static_cast<char>(byte));
            continue;
        }
        const bool completed =
            terminal_safe_text::append_escaped_byte(byte, append);
        (void)completed;
    }
    return display;
}

std::string independent_requires_check_attention_message(DevelRequiresCheckReason reason) {
    if(reason != DevelRequiresCheckReason::SuffixCandidateOnly)
        return localization::translate_message("skipped: devel update requires check; local authority is unavailable or has changed");
    return localization::translate_message(
        "skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable");
}

} // namespace

UnifiedPlanRenderingResult render_unified_plan_observation(
    const UnifiedPlanObservation& observation) {
    RenderState state;
    state.output << localization::translate_message("Unified plan:") << '\n';
    state.output << localization::format_translated_message(
                        "  Status: {}",
                        observation_status_display(
                            observation.status(), state))
                 << '\n';
    render_roots(observation, state);
    for(const auto& metadata : observation.root_metadata()) {
        if(const auto* update = std::get_if<UnifiedPlanBorrowedAuthorityReference<AurUpdatePlanEntry>>(&metadata);
           update && aur_update_basis(update->get()) == AurUpdateBasis::GitRevision) {
            // TRANSLATORS: The placeholder is a package name. This is a Git revision difference, not a newer package version.
            state.output << localization::format_translated_message("  Observed update basis: {} — Git revision difference", terminal_safe_text_display(update->get().installed_name)) << '\n';
        } else if(update && update->get().devel_assessment_origin == AurDevelAssessmentOrigin::CurrentObservation && update->get().devel_assessment.state() == DevelUpdateAssessmentState::UpToDate) {
            state.output << localization::format_translated_message("  Observed devel assessment: {} — same Git revision", terminal_safe_text_display(update->get().installed_name)) << '\n';
        }
    }
    render_phases(observation, state);
    render_configured_repositories(observation, state);
    render_dependencies(observation, state);
    render_build_units(observation, state);
    render_required_artifacts(observation, state);
    render_transaction_intents(observation, state);
    render_blockers(observation, state);
    return UnifiedPlanRenderingResult{
        state.output.str(), std::move(state.issues)};
}

UnifiedPlanRenderingResult render_system_aur_update_unified_plan(
    const SystemAurUpdateUnifiedPlanProjection& projection) {
    RenderState state;
    std::string status;
    switch(projection.status()) {
        case SystemAurUpdateUnifiedPlanStatus::Ready:
            status = localization::translate_message("Ready");
            break;
        case SystemAurUpdateUnifiedPlanStatus::Blocked:
            status = localization::translate_message("Blocked");
            break;
        default:
            status = unsupported_display(
                state, UnifiedPlanRenderingSection::RouteSemantics, 0,
                std::nullopt,
                localization::format_translated_message(
                    "A system/{} aggregate status is not supported by the renderer.",
                    "AUR"));
            break;
    }
    const bool is_repository_only =
        projection.mode() == SystemAurUpdateUnifiedPlanMode::RepoOnly;
    if(is_repository_only) {
        state.output << localization::translate_message(
                            "Repository system update plan:")
                     << '\n';
    } else {
        state.output << localization::format_translated_message(
                            "System + normal {} update plan:", "AUR")
                     << '\n';
    }
    state.output << localization::format_translated_message(
                        "  Status: {}", status)
                 << '\n';
    if(is_repository_only) {
        state.output << localization::translate_message(
                            "Repository update phase:")
                     << '\n';
    } else {
        state.output << localization::translate_message(
                            "Combined update phases:")
                     << '\n';
    }
    for(std::size_t index = 0; index < projection.phases().size(); ++index) {
        std::string phase;
        switch(projection.phases()[index]) {
            case SystemAurUpdateUnifiedPlanPhase::
                RepositorySystemTransactionIntent:
                phase = localization::translate_message(
                    "official repository system-upgrade intent");
                break;
            case SystemAurUpdateUnifiedPlanPhase::
                CurrentForeignInventoryObservation:
                phase = localization::format_translated_message(
                    "current installed foreign/{} state observation",
                    "AUR");
                break;
            case SystemAurUpdateUnifiedPlanPhase::
                CurrentNormalAurAssessment:
                phase = localization::format_translated_message(
                    "current-state normal {} assessment", "AUR");
                break;
            case SystemAurUpdateUnifiedPlanPhase::
                PotentialLaterAurTransactions:
                phase = localization::format_translated_message(
                    "potential later normal {} build/install intents",
                    "AUR");
                break;
            default:
                phase = unsupported_display(
                    state, UnifiedPlanRenderingSection::RouteSemantics,
                    index, std::nullopt,
                    localization::format_translated_message(
                        "A system/{} conceptual phase is not supported by the renderer.",
                        "AUR"));
                break;
        }
        state.output << localization::format_translated_message(
                            "  Phase {}: {}", index + 1, phase)
                     << '\n';
    }

    if(projection.mode() == SystemAurUpdateUnifiedPlanMode::Auto) {
        const bool has_supported_semantics =
            projection.freshness() ==
                std::optional<SystemAurUpdateUnifiedPlanFreshness>{
                    SystemAurUpdateUnifiedPlanFreshness::
                        CurrentInstalledState} &&
            projection.actual_refresh() ==
                std::optional<SystemAurUpdateUnifiedPlanActualRefresh>{
                    SystemAurUpdateUnifiedPlanActualRefresh::
                        AfterRepositorySuccess} &&
            projection.transaction_relationship() ==
                std::optional<
                    SystemAurUpdateUnifiedPlanTransactionRelationship>{
                    SystemAurUpdateUnifiedPlanTransactionRelationship::
                        SeparateSequentialTransactions};
        state.output << localization::translate_message(
                            "Freshness:")
                     << '\n';
        if(!has_supported_semantics) {
            state.add_issue(
                UnifiedPlanRenderingIssueKind::UnsupportedValue,
                UnifiedPlanRenderingSection::RouteSemantics, 0,
                std::nullopt,
                localization::format_translated_message(
                    "System/{} freshness or transaction semantics are incomplete.",
                    "AUR"));
            state.output << localization::translate_message(
                                "  unsupported")
                         << '\n';
        } else {
            state.output << localization::format_translated_message(
                                "  {} assessment is based on the current installed state.",
                                "AUR")
                         << '\n';
            state.output << localization::format_translated_message(
                                "  Actual execution re-evaluates {} state after the repository upgrade succeeds.",
                                "AUR")
                         << '\n';
            state.output << localization::format_translated_message(
                                "  The repository system transaction and later normal {} transactions are separate intents.",
                                "AUR")
                         << '\n';
        }
    } else if(projection.mode() !=
              SystemAurUpdateUnifiedPlanMode::RepoOnly) {
        state.add_issue(
            UnifiedPlanRenderingIssueKind::UnsupportedValue,
            UnifiedPlanRenderingSection::RouteSemantics, 0,
            std::nullopt,
            localization::format_translated_message(
                "A system/{} projection mode is not supported by the renderer.",
                "AUR"));
    }

    state.output << localization::translate_message(
                        "Repository system phase:")
                 << '\n';
    const UnifiedPlanObservation* repository_observation =
        projection.repository_projection().observation_result().observation();
    if(repository_observation == nullptr) {
        state.add_issue(
            UnifiedPlanRenderingIssueKind::MissingReferencedValue,
            UnifiedPlanRenderingSection::RouteSemantics, 0,
            std::nullopt,
            localization::format_translated_message(
                "System/{} projection has no repository child observation.",
                "AUR"));
        return UnifiedPlanRenderingResult{
            state.output.str(), std::move(state.issues)};
    }
    UnifiedPlanRenderingResult repository =
        render_unified_plan_observation(*repository_observation);
    state.output << repository.text;
    state.issues.insert(
        state.issues.end(),
        std::make_move_iterator(repository.issues.begin()),
        std::make_move_iterator(repository.issues.end()));

    if(const UnifiedPlanProjection* aur = projection.aur_projection();
       aur != nullptr) {
        state.output << localization::format_translated_message(
                            "Current-state normal {} phase:", "AUR")
                     << '\n';
        const UnifiedPlanObservation* aur_observation =
            aur->observation_result().observation();
        if(aur_observation == nullptr) {
            state.add_issue(
                UnifiedPlanRenderingIssueKind::MissingReferencedValue,
                UnifiedPlanRenderingSection::RouteSemantics, 0,
                std::nullopt,
                localization::format_translated_message(
                    "System/{} projection has no current-state {} child observation.",
                    "AUR", "AUR"));
            return UnifiedPlanRenderingResult{
                state.output.str(), std::move(state.issues)};
        }
        UnifiedPlanRenderingResult aur_rendering =
            render_unified_plan_observation(*aur_observation);
        state.output << aur_rendering.text;
        state.issues.insert(
            state.issues.end(),
            std::make_move_iterator(aur_rendering.issues.begin()),
            std::make_move_iterator(aur_rendering.issues.end()));
    }

    if(!projection.requires_check_attentions().empty()) {
        state.output << localization::translate_message(
                            "Attention-required details:")
                     << '\n';
        for(std::size_t index = 0;
            index < projection.requires_check_attentions().size();
            ++index) {
            const SystemAurUpdateRequiresCheckAttention& attention =
                projection.requires_check_attentions()[index];
            const bool is_supported =
                projection.mode() == SystemAurUpdateUnifiedPlanMode::Auto &&
                is_known_devel_requires_check_reason(attention.reason) &&
                attention.skip_kind ==
                    AurUpdateExecutionSkipKind::
                        IndependentDevelRequiresCheck &&
                attention.observation_state ==
                    AurUpdateEffectiveState::RequiresCheck &&
                has_valid_requires_check_package_identity_shape(
                    attention.package_name) &&
                has_valid_requires_check_package_identity_shape(
                    attention.package_base);
            if(!is_supported) {
                state.add_issue(
                    UnifiedPlanRenderingIssueKind::UnsupportedValue,
                    UnifiedPlanRenderingSection::RouteSemantics,
                    index, attention.update_plan_index,
                    localization::format_translated_message(
                        // TRANSLATORS: The placeholder is the AUR project identity.
                        "A system/{} RequiresCheck attention is not supported by the renderer.",
                        "AUR"));
                state.output << localization::translate_message(
                                    "  unsupported")
                             << '\n';
                continue;
            }
            state.output << "  "
                         << invalid_snapshot_raw_value_display(
                                attention.package_name)
                         << " (PackageBase: "
                         << invalid_snapshot_raw_value_display(
                                attention.package_base)
                         << "): "
                         << independent_requires_check_attention_message(attention.reason)
                         << '\n';
        }
    }
    return UnifiedPlanRenderingResult{
        state.output.str(), std::move(state.issues)};
}
