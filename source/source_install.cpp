#include "source_install.hpp"

#include "invocation_owned_cleanup_adapter.hpp"

#include "interactive_confirmation.hpp"

#include "app_config.hpp"
#include "aur_rpc.hpp"
#include "build_plan_artifact_target_projection.hpp"
#include "cache_authority.hpp"
#include "dependency_plan.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "repository_query.hpp"
#include "reviewed_source_production_outcome.hpp"
#include "separated_source_build.hpp"
#include "source_build.hpp"
#include "source_install_internal.hpp"
#include "source_preference.hpp"
#include "shell_words.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {

// NO_TRANSLATE: These are protocol endpoint identities, not user-facing prose.
const std::string AUR_BASE_URL = "https://aur.archlinux.org/";

bool is_safe_repository_target_name(const std::string& repository_name) {
    if(repository_name.empty() || repository_name == "." ||
       repository_name == ".." ||
       repository_name.find('/') != std::string::npos) {
        return false;
    }
    return std::none_of(
        repository_name.begin(), repository_name.end(),
        [](unsigned char character) {
            return std::iscntrl(character) != 0;
        });
}

SourceBuildEnvironment load_source_preference_environment(
    const std::string& package_name) {
    return get_package_env(
        package_name,
        [](const fs::path& entry_path) {
            // TRANSLATORS: The placeholder is a source preference file path.
            Logger::info(localization::format_translated_message(
                "Loading custom build flags from {}.",
                entry_path.string()));
        },
        [](const std::string& warning) {
            Logger::warn(warning);
        });
}

void require_supported_registered_source_install_target(
    const ResolvedSourceBuildIdentity& source) {
    // POLICY(#98,#268): registered source upgradeのlegacy singular lifecycleは
    // requested split childを個別選択できないため安全側で停止する。
    if(source.source_kind() == SourceBuildSourceKind::Aur &&
       source.has_distinct_package_base()) {
        // TRANSLATORS: The placeholders are the AUR identity, a requested package name, the PackageBase field identity, and its value.
        throw std::runtime_error(localization::format_translated_message(
            "Registered source upgrade does not support split {} preference {} from {} {}; this route requires a singular package identity.",
            "AUR",
            source.requested_name(),
            "PackageBase",
            source.package_base()));
    }
}

void add_selected_repository_provider(
    std::vector<ProvidedDependency>& providers,
    const ProvidedDependency& provider);

DesiredInstallReason resolve_source_target_reason(
    const ResolvedSourceBuildIdentity& source,
    ArtifactLifecycleIntent lifecycle_intent,
    const ProviderSelectionCallback& select_provider,
    std::vector<ProvidedDependency>& selected_repository_providers,
    std::optional<std::vector<std::string>>&
        configured_repository_order) {
    if(source.source_kind() != SourceBuildSourceKind::Aur) {
        return DesiredInstallReason::Explicit;
    }

    // POLICY(#174,#268): dependency graph全体のRPC schemaを解決してから
    // route固有のexecutable guardへ進む。registered source upgradeのlegacy
    // singular ownerだけはsplit selection guardを維持する。
    BuildPlan plan = resolve_build_plan(
        source.requested_name(), select_provider);
    configured_repository_order = plan.configured_repository_order;
    if(lifecycle_intent == ArtifactLifecycleIntent::PackageBaseSet) {
        require_executable_build_plan(source.requested_name(), plan);
    } else {
        require_supported_registered_source_install_target(source);
        require_executable_install_plan(source.requested_name(), plan);
    }
    for(const BuildPlanProvidedDependency& dependency : plan.provided) {
        if(dependency.resolution !=
           ProviderResolutionKind::UserSelected) {
            continue;
        }
        if(std::holds_alternative<AurProviderOrigin>(
               dependency.provider.origin) &&
           lifecycle_intent != ArtifactLifecycleIntent::PackageBaseSet) {
            throw std::runtime_error(
                localization::format_translated_message(
                    "Registered source upgrade cannot enforce a selected {} dependency provider.",
                    "AUR"));
        }
        if(std::holds_alternative<RepositoryProviderOrigin>(
               dependency.provider.origin)) {
            add_selected_repository_provider(
                selected_repository_providers,
                dependency.provider);
        }
    }
    BuildPlanArtifactTargetProjectionResult projection =
        project_build_plan_required_artifact_targets(plan);
    if(!projection.is_success()) {
        // TRANSLATORS: The placeholders are the literal BuildPlan identity and a requested package name.
        throw std::logic_error(localization::format_translated_message(
            "{} required artifact target projection failed for {}.",
            "BuildPlan", source.requested_name()));
    }
    for(const auto& unit : projection.success()->build_units) {
        if(unit.package_base != source.package_base()) continue;
        for(const auto& target : unit.required_targets) {
            if(target.package_name == source.requested_name()) {
                return target.desired_reason;
            }
        }
    }
    // TRANSLATORS: The placeholders are the literal BuildPlan identity and a requested package name.
    throw std::logic_error(localization::format_translated_message(
        "{} required artifact target projection omitted {}.",
        "BuildPlan", source.requested_name()));
}

std::string join_required_package_names(
    const std::vector<RequiredPackageArtifactTarget>& targets) {
    std::stringstream ss;
    for(size_t i = 0; i < targets.size(); ++i) {
        if(i > 0) ss << ", ";
        ss << targets[i].package_name;
    }
    return ss.str();
}

std::string aur_git_url_for_package_base(const std::string& package_base) {
    require_valid_package_name(package_base);
    return AUR_BASE_URL + package_base + ".git";
}

PackageBaseIdentity aur_review_identity_for_package_base(
    const std::string& package_base) {
    const std::string remote = aur_git_url_for_package_base(package_base);
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(remote)),
        package_base);
}

void add_selected_repository_provider(
    std::vector<ProvidedDependency>& providers,
    const ProvidedDependency& provider) {
    auto same = [&provider](const ProvidedDependency& existing) {
        return same_provider_identity(existing, provider);
    };
    if(std::find_if(providers.begin(), providers.end(), same) !=
       providers.end()) {
        return;
    }
    providers.push_back(provider);
}

std::optional<std::pair<std::string, std::string>>
source_artifact_resolved_identity(
    const BuildPlanDependencyEdge& edge) {
    if(!edge.resolved_candidate.has_value()) return std::nullopt;
    return std::visit(
        [](const auto& candidate)
            -> std::optional<std::pair<std::string, std::string>> {
            using Candidate = std::decay_t<decltype(candidate)>;
            if constexpr(std::is_same_v<
                             Candidate,
                             AurResolvedDependencyCandidate>) {
                return std::pair{
                    candidate.package_name, candidate.package_base};
            } else if constexpr(std::is_same_v<
                                    Candidate,
                                    ProviderResolvedDependencyCandidate>) {
                if(!std::holds_alternative<AurProviderOrigin>(
                       candidate.provider.origin)) {
                    return std::nullopt;
                }
                return std::pair{
                    candidate.provider.package_name,
                    candidate.provider.package_base};
            }
            return std::nullopt;
        },
        edge.resolved_candidate.value());
}

void attach_build_plan_dependency_edge_indices(
    ProductionSourceBuildWorkItem& work_item,
    const ProjectedBuildPlanArtifactTargets& unit,
    const BuildPlan& plan) {
    for(std::size_t edge_index = 0;
        edge_index < plan.dependency_edges.size(); ++edge_index) {
        const auto resolved = source_artifact_resolved_identity(
            plan.dependency_edges[edge_index]);
        if(!resolved.has_value() || resolved->second != unit.package_base) {
            continue;
        }
        const bool is_required_target = std::any_of(
            unit.required_targets.begin(), unit.required_targets.end(),
            [&resolved](const RequiredPackageArtifactTarget& target) {
                return target.package_name == resolved->first &&
                       target.package_base == resolved->second;
            });
        if(is_required_target) {
            work_item.build_plan_dependency_edge_indices.push_back(
                edge_index);
        }
    }
}

void attach_selected_repository_providers(
    ProductionSourceBuildWorkItem& work_item,
    const BuildPlan& plan) {
    for(std::size_t edge_index = 0;
        edge_index < plan.dependency_edges.size(); ++edge_index) {
        const BuildPlanDependencyEdge& edge =
            plan.dependency_edges[edge_index];
        if(edge.parent_package_base != work_item.request.checkout_name ||
           edge.kind != DependencyKind::Provided ||
           edge.provider_resolution != ProviderResolutionKind::UserSelected ||
           !edge.resolved_provider.has_value() ||
           !std::holds_alternative<RepositoryProviderOrigin>(
               edge.resolved_provider->origin)) {
            continue;
        }
        add_selected_repository_provider(
            work_item.selected_repository_providers,
            edge.resolved_provider.value());
        work_item.selected_repository_provider_edge_indices.push_back(
            edge_index);
    }
}

ProductionSourceBuildWorkItem make_aur_source_build_work_item(
    const ProjectedBuildPlanArtifactTargets& unit,
    const BuildPlan& plan,
    bool use_source_build_preferences,
    bool needed) {
    const bool is_singular = unit.required_targets.size() == 1;
    const std::string preference_name = is_singular
                                            ? unit.required_targets.front().package_name
                                            : unit.package_base;
    SourceBuildEnvironment environment;
    if(use_source_build_preferences) {
        SourceBuildEnvironment requested_environment =
            load_source_preference_environment(preference_name);
        // POLICY(#242): empty definitionを保持したまま、fallback判定だけは従来の
        // forward可能なnonempty assignment基準にする。PKGDEST definitionは
        // fallbackで捨てず、all-target preflightまで保持する。
        if(!requested_environment.has_forwarded_nonempty_assignment() &&
           !requested_environment.defines("PKGDEST") && is_singular &&
           preference_name != unit.package_base) {
            environment =
                load_source_preference_environment(unit.package_base);
        } else {
            environment = requested_environment;
        }
    }

    ProductionSourceBuildWorkItem work_item;
    if(is_singular) {
        work_item.request.package_name =
            unit.required_targets.front().package_name;
    }
    work_item.request.checkout_name = unit.package_base;
    work_item.request.git_url = aur_git_url_for_package_base(unit.package_base);
    work_item.request.aur_review_identity =
        aur_review_identity_for_package_base(unit.package_base);
    work_item.request.custom_environment = std::move(environment);
    work_item.request.needed = needed;
    work_item.required_targets = unit.required_targets;
    work_item.required_target_provenance =
        RequiredTargetProvenance::AurBuildPlanProjection;
    work_item.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    work_item.configured_repository_order =
        plan.configured_repository_order;
    attach_build_plan_dependency_edge_indices(work_item, unit, plan);
    attach_selected_repository_providers(work_item, plan);
    require_static_production_source_build_work_item(work_item);
    return work_item;
}

enum class RepositoryProviderInstallDirective {
    Default,
    AsDependency,
};

[[noreturn]] void throw_malformed_repository_provider_metadata(
    const std::string& diagnostic) {
    throw PackageMetadataError(PackageMetadataFailure{
        PackageMetadataErrorCode::MalformedMetadata, diagnostic});
}

RepositoryProviderInstallDirective
resolve_repository_provider_install_directive(
    const std::vector<ProvidedDependency>& providers,
    const PacmanDatabasePaths& database_paths) {
    std::optional<RepositoryProviderInstallDirective> transaction_directive;
    PackageMetadataSession session =
        PackageMetadataSession::open(database_paths);
    for(const ProvidedDependency& provider : providers) {
        InstalledPackageQueryResult query_result =
            session.query_installed_package(provider.package_name);
        RepositoryProviderInstallDirective provider_directive =
            RepositoryProviderInstallDirective::AsDependency;
        if(const auto* failure =
               std::get_if<PackageMetadataFailure>(&query_result)) {
            // POLICY: metadata failureと未導入を区別し、reasonを推測してmutationしない。
            throw PackageMetadataError(*failure);
        }
        if(const auto* metadata =
               std::get_if<InstalledPackageMetadata>(&query_result)) {
            if(metadata->name != provider.package_name) {
                throw_malformed_repository_provider_metadata(
                    localization::translate_message(
                        "Installed package metadata name does not match the selected repository provider identity."));
            }
            if(metadata->version.empty()) {
                throw_malformed_repository_provider_metadata(
                    localization::translate_message(
                        "Installed package metadata contains an empty version."));
            }
            switch(metadata->reason) {
                case InstalledPackageReason::Explicit:
                    provider_directive =
                        RepositoryProviderInstallDirective::Default;
                    break;
                case InstalledPackageReason::Dependency:
                    break;
                case InstalledPackageReason::Unknown:
                    throw_malformed_repository_provider_metadata(
                        localization::translate_message(
                            "Installed package metadata contains an unknown install reason."));
                default:
                    throw_malformed_repository_provider_metadata(
                        localization::translate_message(
                            "Installed package metadata contains an invalid install reason."));
            }
        } else if(!std::holds_alternative<PackageNotFound>(query_result)) {
            throw std::logic_error(localization::translate_message(
                "Unknown installed package query result."));
        }

        if(transaction_directive.has_value() &&
           transaction_directive.value() != provider_directive) {
            throw std::runtime_error(localization::translate_message(
                "Selected repository provider install reasons cannot be represented by one package transaction."));
        }
        transaction_directive = provider_directive;
    }
    return transaction_directive.value_or(
        RepositoryProviderInstallDirective::AsDependency);
}

int install_selected_repository_providers(
    const std::vector<ProvidedDependency>& providers,
    RepositoryProviderInstallDirective directive,
    const AppConfig& config) {
    std::vector<std::string> targets;
    for(const ProvidedDependency& provider : providers) {
        const auto& repository =
            std::get<RepositoryProviderOrigin>(provider.origin);
        targets.push_back(
            repository.repository_name + "/" + provider.package_name);
    }
    std::vector<std::string> command{"sudo", "pacman", "-S"};
    switch(directive) {
        case RepositoryProviderInstallDirective::Default:
            break;
        case RepositoryProviderInstallDirective::AsDependency:
            command.push_back("--asdeps");
            break;
    }
    command.push_back("--needed");
    if(config.no_confirm) command.push_back("--noconfirm");
    command.push_back("--");
    command.insert(command.end(), targets.begin(), targets.end());
    Logger::info(localization::format_translated_message(
        "Installing selected repository providers: {}",
        shell_words::join(targets)));
    return run_command(shell_words::join(command));
}

ProductionSourceBuildWorkItem make_direct_source_build_work_item(
    const ResolvedSourceBuildIdentity& source,
    SourceBuildEnvironment environment,
    SourceEnvironmentEmptyValuePolicy empty_value_policy,
    bool only_if_updated,
    bool needed,
    ArtifactLifecycleIntent lifecycle_intent,
    const ProviderSelectionCallback& select_provider) {
    ProductionSourceBuildWorkItem work_item;
    work_item.request.package_name = source.requested_name();
    work_item.request.checkout_name = source.package_base();
    work_item.request.git_url = source.git_url();
    if(source.source_kind() == SourceBuildSourceKind::Aur) {
        work_item.request.aur_review_identity =
            aur_review_identity_for_package_base(
                source.package_base());
    }
    work_item.request.custom_environment = std::move(environment);
    work_item.request.empty_value_policy = empty_value_policy;
    work_item.request.only_if_updated = only_if_updated;
    work_item.request.needed = needed;
    DesiredInstallReason reason = resolve_source_target_reason(
        source, lifecycle_intent, select_provider,
        work_item.selected_repository_providers,
        work_item.configured_repository_order);
    work_item.required_targets.push_back(RequiredPackageArtifactTarget{
        source.package_base(), source.requested_name(), reason});
    work_item.required_target_provenance =
        source.source_kind() == SourceBuildSourceKind::Repository
            ? RequiredTargetProvenance::RepositoryExactPackageProjection
            : RequiredTargetProvenance::AurBuildPlanProjection;
    work_item.artifact_lifecycle_intent = lifecycle_intent;
    if(const auto* repository = source.repository_identity();
       repository != nullptr) {
        work_item.repository_identity = *repository;
        work_item.configured_repository_order =
            repository->exact_package().configured_repository_order;
    }
    work_item.uses_system_update_baseline =
        source.source_kind() == SourceBuildSourceKind::Repository;
    require_static_production_source_build_work_item(work_item);
    return work_item;
}

std::optional<ArtifactInstallExecutionOutcome> flatten_source_build_result(
    const SourceBuildExecutionResult& result) {
    switch(result.status) {
        case SourceBuildExecutionStatus::DevelRequiresCheckSkipped:
        case SourceBuildExecutionStatus::AuthoritativeIncomplete: return std::nullopt;
        case SourceBuildExecutionStatus::Installed:
            return ArtifactInstallExecutionOutcome::Installed;
        case SourceBuildExecutionStatus::SkippedAsNeeded:
            return ArtifactInstallExecutionOutcome::SkippedAsNeeded;
        case SourceBuildExecutionStatus::UpToDate:
        case SourceBuildExecutionStatus::UpdateStatusUnknownSkipped:
            return std::nullopt;
    }
    throw std::logic_error(localization::translate_message(
        "Unknown source-build execution status."));
}

bool should_present_package_base_result(
    const ProductionSourceBuildWorkItem& work_item,
    const PackageBaseSourceBuildExecutionResult& result) noexcept {
    return work_item.required_targets.size() != 1 ||
           work_item.required_targets.front().package_name !=
               work_item.request.checkout_name ||
           !result.unselected_artifacts().empty();
}

void present_package_base_result(
    const ProductionSourceBuildWorkItem& work_item,
    const PackageBaseSourceBuildExecutionResult& result) {
    if(!should_present_package_base_result(work_item, result)) return;
    if(result.package_base() != work_item.request.checkout_name ||
       result.selected_children().size() !=
           work_item.required_targets.size()) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} source-build result is incoherent for presentation.",
                "PackageBase"));
    }

    // TRANSLATORS: The placeholders are the PackageBase field identity and a PackageBase name.
    Logger::info(localization::format_translated_message(
        "{} result: {}", "PackageBase", result.package_base()));
    for(std::size_t index = 0;
        index < result.selected_children().size(); ++index) {
        const RequiredPackageArtifactTarget& required =
            work_item.required_targets[index];
        const PackageBaseSourceBuildSelectedResult& child =
            result.selected_children()[index];
        if(child.identity.package_name != required.package_name ||
           child.identity.full_version.empty() ||
           child.desired_reason != required.desired_reason) {
            throw std::logic_error(
                localization::format_translated_message(
                    "{} source-build child result is incoherent for presentation.",
                    "PackageBase"));
        }
        if(child.desired_reason == DesiredInstallReason::Explicit &&
           child.outcome == ArtifactInstallExecutionOutcome::Installed) {
            // TRANSLATORS: The placeholders are the requested package, produced package, and full version.
            Logger::info(localization::format_translated_message(
                "  required child: {} -> {} {} (explicit): installed",
                required.package_name,
                child.identity.package_name,
                child.identity.full_version));
        } else if(child.desired_reason == DesiredInstallReason::Explicit &&
                  child.outcome ==
                      ArtifactInstallExecutionOutcome::SkippedAsNeeded) {
            // TRANSLATORS: The placeholders are the requested package, produced package, full version, and literal --needed option.
            Logger::info(localization::format_translated_message(
                "  required child: {} -> {} {} (explicit): skipped as needed ({})",
                required.package_name,
                child.identity.package_name,
                child.identity.full_version,
                "--needed"));
        } else if(child.desired_reason == DesiredInstallReason::Dependency &&
                  child.outcome == ArtifactInstallExecutionOutcome::Installed) {
            // TRANSLATORS: The placeholders are the requested package, produced package, and full version.
            Logger::info(localization::format_translated_message(
                "  required child: {} -> {} {} (dependency): installed",
                required.package_name,
                child.identity.package_name,
                child.identity.full_version));
        } else if(child.desired_reason == DesiredInstallReason::Dependency &&
                  child.outcome ==
                      ArtifactInstallExecutionOutcome::SkippedAsNeeded) {
            // TRANSLATORS: The placeholders are the requested package, produced package, full version, and literal --needed option.
            Logger::info(localization::format_translated_message(
                "  required child: {} -> {} {} (dependency): skipped as needed ({})",
                required.package_name,
                child.identity.package_name,
                child.identity.full_version,
                "--needed"));
        } else {
            throw std::logic_error(localization::format_translated_message(
                "{} source-build child result has an unknown install reason or outcome.",
                "PackageBase"));
        }
    }
    for(const ArtifactPackageIdentity& unselected :
        result.unselected_artifacts()) {
        if(unselected.package_name.empty() ||
           unselected.full_version.empty()) {
            throw std::logic_error(
                localization::format_translated_message(
                    "{} unselected artifact identity is incoherent for presentation.",
                    "PackageBase"));
        }
        // TRANSLATORS: The placeholders are a produced package name and full version.
        Logger::info(localization::format_translated_message(
            "  produced artifact: {} {} (not selected; not installed)",
            unselected.package_name,
            unselected.full_version));
    }
}

void present_production_source_build_outcome(
    const std::string& package_base,
    const ProductionSourceBuildStagedOutcome& outcome) {
    ReviewedSourceProductionOutcomePresentation presentation =
        format_production_source_build_staged_outcome(
            package_base, outcome);
    for(const std::string& line : presentation.info_lines) {
        Logger::info(line);
    }
}

void present_production_source_build_outcome(
    const std::string& package_base,
    const std::optional<ProductionSourceBuildStagedOutcome>& outcome) {
    if(outcome.has_value()) {
        present_production_source_build_outcome(package_base, *outcome);
    }
}

} // namespace

bool ProductionSourceBuildInvocationResult::is_success() const noexcept {
    return std::all_of(
        work_items.begin(), work_items.end(),
        [](const ProductionSourceBuildWorkItemOutcome& work_item) {
            return work_item.status ==
                   ProductionSourceBuildWorkItemStatus::Succeeded;
        });
}

int ProductionSourceBuildInvocationResult::command_exit_status() const noexcept {
    return is_success() ? 0 : 1;
}

ProductionSourceBuildInvocationError::
    ProductionSourceBuildInvocationError(
        ProductionSourceBuildInvocationResult result,
        std::size_t failed_work_item_index,
        const std::string& diagnostic)
    : std::runtime_error(diagnostic), result_(std::move(result)),
      failed_work_item_index_(failed_work_item_index) {
    if(failed_work_item_index_ >= result_.work_items.size() ||
       result_.work_items[failed_work_item_index_].status !=
           ProductionSourceBuildWorkItemStatus::Failed ||
       !result_.work_items[failed_work_item_index_].failure_stage.has_value() ||
       result_.work_items[failed_work_item_index_].failure_exception ==
           nullptr) {
        throw std::logic_error(
            "Production source-build invocation failure is incoherent.");
    }
}

ProductionSourceBuildFailureStage
ProductionSourceBuildInvocationError::failure_stage() const {
    return *result_.work_items[failed_work_item_index_].failure_stage;
}

void ProductionSourceBuildInvocationError::rethrow_failure() const {
    std::rethrow_exception(
        result_.work_items[failed_work_item_index_].failure_exception);
}

std::string format_production_source_build_invocation_failure(
    const ProductionSourceBuildInvocationError& error) {
    switch(error.failure_stage()) {
        case ProductionSourceBuildFailureStage::Review:
            return error.what();
        case ProductionSourceBuildFailureStage::Build:
        case ProductionSourceBuildFailureStage::ArtifactValidation:
            return localization::format_translated_message(
                "Build Error: {}", error.what());
        case ProductionSourceBuildFailureStage::InstallPreparation:
        case ProductionSourceBuildFailureStage::InstallTransaction:
            return localization::format_translated_message(
                "Install Error: {}", error.what());
        case ProductionSourceBuildFailureStage::Cleanup:
            return error.what();
        case ProductionSourceBuildFailureStage::Other:
            return localization::format_translated_message(
                "Source-build Error: {}", error.what());
    }
    throw std::logic_error(
        "Production source-build failure stage is unknown.");
}

LocalSourceBuildDependencyPreparation::
    LocalSourceBuildDependencyPreparation(
        std::vector<ProductionSourceBuildWorkItem>
            remote_work_items,
        std::vector<ProvidedDependency>
            selected_repository_providers) noexcept
    : remote_work_items_(std::move(remote_work_items)),
      selected_repository_providers_(
          std::move(selected_repository_providers)) {
}

const std::vector<ProductionSourceBuildWorkItem>&
LocalSourceBuildDependencyPreparation::remote_work_items() const noexcept {
    return remote_work_items_;
}

const std::vector<ProvidedDependency>&
LocalSourceBuildDependencyPreparation::selected_repository_providers()
    const noexcept {
    return selected_repository_providers_;
}

#ifdef MOGUET_ENABLE_TEST_OVERRIDES
LocalSourceBuildDependencyPreparation
LocalSourceBuildDependencyPreparation::
    make_for_production_source_build_test(
        std::vector<ProductionSourceBuildWorkItem>
            remote_work_items,
        std::vector<ProvidedDependency>
            selected_repository_providers) {
    return LocalSourceBuildDependencyPreparation(
        std::move(remote_work_items),
        std::move(selected_repository_providers));
}
#endif

ProductionSourceBuildWorkItem prepare_aur_source_build_work_item_internal(
    const ProjectedBuildPlanArtifactTargets& unit,
    const BuildPlan& plan,
    bool use_source_build_preferences,
    bool needed) {
    return make_aur_source_build_work_item(
        unit, plan, use_source_build_preferences, needed);
}

void seed_production_source_build_cache(
    PreparedProductionSourceBuildInvocation& invocation,
    const ValidatedCacheRoot& cache_root) {
    if(invocation.work_items.empty()) {
        throw std::logic_error(
            localization::translate_message(
                "Cannot seed cache for an empty source-build invocation."));
    }

    cache_root.require_unchanged_identity();
    std::optional<ValidatedCacheRoot> existing_root = invocation.cache_root;
    for(const auto& work_item : invocation.work_items) {
        if(!work_item.cache_root.has_value()) continue;
        work_item.cache_root->require_unchanged_identity();
        if(!existing_root.has_value()) {
            existing_root = work_item.cache_root.value();
            continue;
        }
        existing_root->require_unchanged_identity();
        if(existing_root->device() != work_item.cache_root->device() ||
           existing_root->inode() != work_item.cache_root->inode() ||
           existing_root->owner() != work_item.cache_root->owner()) {
            throw std::logic_error(
                localization::translate_message(
                    "Production source-build work items use different cache authorities."));
        }
    }

    if(existing_root.has_value() &&
       (existing_root->device() != cache_root.device() ||
        existing_root->inode() != cache_root.inode() ||
        existing_root->owner() != cache_root.owner())) {
        throw std::logic_error(
            localization::translate_message(
                "Production source-build invocation cache authority changed."));
    }

    invocation.cache_root = cache_root;
    for(auto& work_item : invocation.work_items) {
        work_item.cache_root = cache_root;
    }
}

void activate_production_source_build_cache(
    PreparedProductionSourceBuildInvocation& invocation) {
    if(invocation.work_items.empty()) {
        if(!invocation.local_source_authority.has_value()) {
            throw std::logic_error(
                localization::translate_message(
                    "Cannot activate cache for an empty source-build invocation."));
        }
        if(!invocation.cache_root.has_value()) {
            throw std::logic_error(localization::translate_message(
                "Local source-build dependency invocation has no prepared cache authority."));
        }
        invocation.cache_root->require_unchanged_identity();
        return;
    }
    std::optional<ValidatedCacheRoot> shared_root = invocation.cache_root;
    for(const auto& work_item : invocation.work_items) {
        if(!work_item.cache_root.has_value()) continue;
        if(!shared_root.has_value()) shared_root = work_item.cache_root;
    }
    if(!shared_root.has_value()) shared_root = prepare_process_cache_root();
    seed_production_source_build_cache(invocation, shared_root.value());
}

namespace {

struct PreparedSelectedRepositoryProviderTransaction {
    SelectedRepositoryProviderTransactionResult result;
    std::optional<RepositoryProviderInstallDirective> directive;
};

PreparedSelectedRepositoryProviderTransaction
prepare_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation) {
    PreparedSelectedRepositoryProviderTransaction prepared;
    prepared.result.selected_providers =
        invocation.selected_repository_providers;
    if(prepared.result.selected_providers.empty()) return prepared;

    if(!invocation.cache_root.has_value()) {
        throw std::logic_error(
            localization::translate_message(
                "Production source-build invocation has no prepared cache authority."));
    }
    // POLICY(#272): retained cache authorityをexact pacman transaction直前に
    // 再検証する。system phase中のroot replacementを古いsnapshotで通さない。
    invocation.cache_root->require_unchanged_identity();

    try {
        prepared.directive = resolve_repository_provider_install_directive(
            prepared.result.selected_providers,
            invocation.database_paths);
    } catch(const std::exception& error) {
        prepared.result.status =
            SelectedRepositoryProviderTransactionStatus::
                BlockedBeforeExecution;
        prepared.result.diagnostic = error.what();
        return prepared;
    } catch(...) {
        prepared.result.status =
            SelectedRepositoryProviderTransactionStatus::
                BlockedBeforeExecution;
        prepared.result.diagnostic = localization::translate_message(
            "Failed to inspect selected repository provider install reasons with an unknown exception.");
        return prepared;
    }

    // Metadata sessionを閉じた後、actual pacman直前にもcache authorityを再証明する。
    invocation.cache_root->require_unchanged_identity();
    // Command success is independent from a package changed-set. Both the
    // legacy and receipt-capable path retain the aggregate as Unknown.
    prepared.result.package_state_change = PackageStateChange::Unknown;
    return prepared;
}

const std::string& requirement_raw_specification(
    const DependencyRequirement& requirement) {
    return std::visit(
        [](const auto& typed_requirement) -> const std::string& {
            return typed_requirement.raw_specification();
        },
        requirement);
}

const std::string& requirement_package_name(
    const DependencyRequirement& requirement) {
    return std::visit(
        [](const auto& typed_requirement) -> const std::string& {
            using Requirement = std::decay_t<decltype(typed_requirement)>;
            if constexpr(std::is_same_v<
                             Requirement,
                             ConsumerDependencyRequirement>) {
                return typed_requirement.package_name();
            } else {
                return typed_requirement.soname();
            }
        },
        requirement);
}

bool requirement_is_canonical(
    const DependencyRequirement& requirement,
    const std::string& raw_specification) {
    const DependencyRequirementParseResult parsed =
        parse_dependency_requirement(raw_specification);
    return parsed.requirement() != nullptr &&
           *parsed.requirement() == requirement;
}

bool provider_metadata_is_complete(
    const ProvidedDependency& provider,
    const BuildPlan& plan) {
    const auto* repository =
        std::get_if<RepositoryProviderOrigin>(&provider.origin);
    if(repository == nullptr ||
       !repository->configured_order.has_value() ||
       !plan.configured_repository_order.has_value() ||
       repository->configured_order.value() >=
           plan.configured_repository_order->size() ||
       (*plan.configured_repository_order)
               [repository->configured_order.value()] !=
           repository->repository_name ||
       !is_safe_repository_target_name(repository->repository_name) ||
       !is_valid_package_name(provider.package_name) ||
       !is_valid_package_name(provider.package_base) ||
       !provider.package_architecture.has_value() ||
       !is_valid_package_name(provider.provided_dependency_name) ||
       !provider.package_version.has_value() ||
       !provider.constraint_metadata.has_value()) {
        return false;
    }
    const std::string& package_architecture =
        provider.package_architecture.value();
    if(package_architecture.empty() ||
       std::any_of(
           package_architecture.begin(), package_architecture.end(),
           [](unsigned char character) {
               return character <= 0x20 || character == 0x7f;
           })) {
        return false;
    }

    const ProviderConstraintMetadata& metadata =
        provider.constraint_metadata.value();
    const ProviderCapability& capability = metadata.provided_capability;
    const std::string* package_version = metadata.package_version.version();
    if(metadata.package_version.source() !=
           ObservedVersionSource::RepositoryExactPackage ||
       metadata.provided_version.source() !=
           ObservedVersionSource::RepositoryProviderCapability ||
       package_version == nullptr ||
       *package_version != provider.package_version.value() ||
       capability.package_name() != provider.provided_dependency_name ||
       capability.raw_specification() !=
           provider.provided_dependency_specification) {
        return false;
    }
    return metadata.provided_version ==
           ObservedVersion::from_provider_capability(
               ObservedVersionSource::RepositoryProviderCapability,
               capability);
}

bool provider_edge_is_exact(
    const BuildPlanDependencyEdge& edge,
    const ProvidedDependency& provider,
    const BuildPlan& plan) {
    if(edge.kind != DependencyKind::Provided ||
       edge.provider_resolution != ProviderResolutionKind::UserSelected ||
       !edge.resolved_provider.has_value() ||
       edge.resolved_provider.value() != provider ||
       !edge.resolved_candidate.has_value() ||
       !edge.requirement.has_value() ||
       !edge.constraint_evaluation.has_value() ||
       !provider_metadata_is_complete(provider, plan) ||
       requirement_raw_specification(edge.requirement.value()) !=
           edge.dependency_spec ||
       !requirement_is_canonical(
           edge.requirement.value(), edge.dependency_spec) ||
       requirement_package_name(edge.requirement.value()) !=
           provider.provided_dependency_name) {
        return false;
    }

    const auto* resolved = std::get_if<ProviderResolvedDependencyCandidate>(
        &edge.resolved_candidate.value());
    if(resolved == nullptr || resolved->provider != provider ||
       resolved->provided_version !=
           provider.constraint_metadata->provided_version) {
        return false;
    }

    const auto* consumer = std::get_if<ConsumerDependencyRequirement>(
        &edge.requirement.value());
    if(consumer == nullptr) return false;
    const ConstraintEvaluation expected =
        evaluate_consumer_dependency_requirement(
            *consumer,
            provider.constraint_metadata->provided_version);
    return edge.constraint_evaluation.value() == expected &&
           (expected.satisfaction() == ConstraintSatisfaction::Satisfied ||
            expected.satisfaction() ==
                ConstraintSatisfaction::Unconstrained);
}

const BuildPlanProvidedDependency* exact_selected_decision(
    const BuildPlan& plan,
    const BuildPlanDependencyEdge& edge,
    const ProvidedDependency& provider) {
    const BuildPlanProvidedDependency* match = nullptr;
    for(const BuildPlanProvidedDependency& selected : plan.provided) {
        if(selected.dependency != edge.dependency_spec ||
           selected.resolution != edge.provider_resolution ||
           selected.provider != provider) {
            continue;
        }
        if(match != nullptr) return nullptr;
        match = &selected;
    }
    return match;
}

struct SelectedRepositoryProviderBindingProjection {
    std::vector<SelectedRepositoryProviderTrustedExecutionBinding> bindings;
    std::vector<std::size_t> work_item_indices;
};

std::optional<SelectedRepositoryProviderBindingProjection>
project_selected_repository_provider_bindings(
    const PreparedRemoteSourceBuild& prepared) {
    if(prepared.source.source_kind() != SourceBuildSourceKind::Aur ||
       !prepared.aur_build_plan.has_value()) {
        return std::nullopt;
    }
    const BuildPlan& plan = prepared.aur_build_plan.value();
    const PlanStateProjection plan_state = project_build_plan_state(plan);
    if(plan_state.construction != PlanConstruction::Constructed ||
       plan_state.completeness != PlanCompleteness::Complete ||
       plan_state.provider_decision != ProviderDecision::Selected) {
        return std::nullopt;
    }

    SelectedRepositoryProviderBindingProjection projection;
    std::set<std::size_t> attributed_edges;
    std::set<std::size_t> attributed_work_items;
    for(std::size_t work_item_index = 0;
        work_item_index < prepared.invocation.work_items.size();
        ++work_item_index) {
        const ProductionSourceBuildWorkItem& work_item =
            prepared.invocation.work_items[work_item_index];
        for(const std::size_t edge_index :
            work_item.selected_repository_provider_edge_indices) {
            if(edge_index >= plan.dependency_edges.size() ||
               !attributed_edges.insert(edge_index).second) {
                return std::nullopt;
            }
            const BuildPlanDependencyEdge& edge =
                plan.dependency_edges[edge_index];
            if(edge.parent_package_base !=
                   work_item.request.checkout_name ||
               !edge.resolved_provider.has_value()) {
                return std::nullopt;
            }
            const ProvidedDependency& provider =
                edge.resolved_provider.value();
            const BuildPlanProvidedDependency* selected =
                exact_selected_decision(plan, edge, provider);
            if(selected == nullptr ||
               !provider_edge_is_exact(edge, provider, plan) ||
               std::count(
                   work_item.selected_repository_providers.begin(),
                   work_item.selected_repository_providers.end(),
                   provider) != 1 ||
               std::count(
                   prepared.invocation.selected_repository_providers.begin(),
                   prepared.invocation.selected_repository_providers.end(),
                   provider) != 1) {
                return std::nullopt;
            }
            projection.bindings.push_back(
                SelectedRepositoryProviderTrustedExecutionBinding{
                    work_item_index,
                    edge_index,
                    edge.parent_package_base,
                    edge.requirement.value(),
                    *selected,
                    provider,
                    edge.provider_resolution});
            attributed_work_items.insert(work_item_index);
        }
    }
    if(projection.bindings.empty()) return std::nullopt;

    for(std::size_t edge_index = 0;
        edge_index < plan.dependency_edges.size(); ++edge_index) {
        const BuildPlanDependencyEdge& edge = plan.dependency_edges[edge_index];
        if(edge.kind != DependencyKind::Provided ||
           edge.provider_resolution !=
               ProviderResolutionKind::UserSelected ||
           !edge.resolved_provider.has_value() ||
           !std::holds_alternative<RepositoryProviderOrigin>(
               edge.resolved_provider->origin)) {
            continue;
        }
        if(attributed_edges.find(edge_index) == attributed_edges.end()) {
            return std::nullopt;
        }
    }
    for(const BuildPlanProvidedDependency& selected : plan.provided) {
        if(selected.resolution != ProviderResolutionKind::UserSelected ||
           !std::holds_alternative<RepositoryProviderOrigin>(
               selected.provider.origin)) {
            continue;
        }
        if(std::none_of(
               projection.bindings.begin(), projection.bindings.end(),
               [&selected](const auto& binding) {
                   return binding.selected_decision.dependency ==
                              selected.dependency &&
                          binding.selected_decision.resolution ==
                              selected.resolution &&
                          binding.selected_decision.provider ==
                              selected.provider;
               })) {
            return std::nullopt;
        }
    }
    for(const ProvidedDependency& provider :
        prepared.invocation.selected_repository_providers) {
        if(std::none_of(
               projection.bindings.begin(), projection.bindings.end(),
               [&provider](const auto& binding) {
                   return binding.provider == provider;
               })) {
            return std::nullopt;
        }
    }
    for(std::size_t work_item_index = 0;
        work_item_index < prepared.invocation.work_items.size();
        ++work_item_index) {
        for(const ProvidedDependency& provider :
            prepared.invocation.work_items[work_item_index]
                .selected_repository_providers) {
            if(std::none_of(
                   projection.bindings.begin(), projection.bindings.end(),
                   [work_item_index, &provider](const auto& binding) {
                       return binding.work_item_index == work_item_index &&
                              binding.provider == provider;
                   })) {
                return std::nullopt;
            }
        }
    }

    projection.work_item_indices.assign(
        attributed_work_items.begin(), attributed_work_items.end());
    return projection;
}

std::vector<std::string> selected_provider_package_names(
    const std::vector<ProvidedDependency>& providers) {
    std::vector<std::string> names;
    names.reserve(providers.size());
    for(const ProvidedDependency& provider : providers) {
        names.push_back(provider.package_name);
    }
    return names;
}

} // namespace

SelectedRepositoryProviderTrustedReceiptExecutionResult::
    SelectedRepositoryProviderTrustedReceiptExecutionResult(
        SelectedRepositoryProviderTransactionResult transaction_value,
        std::optional<TrustedAlpmReceiptCaptureResult>
            receipt_capture_value) noexcept
    : transaction(std::move(transaction_value)),
      receipt_capture(std::move(receipt_capture_value)) {
}

const std::optional<SelectedRepositoryProviderTrustedExecutionEvidence>&
SelectedRepositoryProviderTrustedReceiptExecutionResult::
    trusted_execution_evidence() const noexcept {
    return trusted_execution_evidence_;
}

class SelectedRepositoryProviderTrustedReceiptExecutor final {
public:
    [[nodiscard]] static bool session_is_ready(
        const SelectedRepositoryProviderTrustedReceiptRequest& request) noexcept {
        return !request.invocation_authority().has_value() ||
               (request.invocation_authority()->is_active() &&
                request.invocation_authority()->baseline_was_observed());
    }

    [[nodiscard]] static const PreparedProductionSourceBuildInvocation&
    select_invocation(
        const SelectedRepositoryProviderTrustedReceiptRequest& request,
        const PreparedProductionSourceBuildInvocation& compatibility_invocation) noexcept {
        // CONTRACT(#485): a session-bearing request executes its owned
        // invocation; the caller argument is only the Slice 3.6 raw fallback.
        if(request.invocation_authority().has_value() &&
           request.invocation_authority()->is_active()) {
            return request.invocation_authority()->prepared().invocation;
        }
        return compatibility_invocation;
    }

    static void close_execution_evidence(
        SelectedRepositoryProviderTrustedReceiptExecutionResult& execution,
        const SelectedRepositoryProviderTrustedReceiptRequest& request) {
        if(!request.invocation_authority().has_value() ||
           !request.invocation_authority()->is_active() ||
           !execution.receipt_capture.has_value()) {
            return;
        }
        const CleanupInvocationAuthority& authority =
            request.invocation_authority().value();
        const PreparedRemoteSourceBuild& prepared = authority.prepared();
        const auto projection =
            project_selected_repository_provider_bindings(prepared);
        if(!projection.has_value() ||
           execution.transaction.status !=
               SelectedRepositoryProviderTransactionStatus::Succeeded ||
           execution.transaction.package_state_change !=
               PackageStateChange::Unknown ||
           execution.transaction.command_exit_status !=
               std::optional<int>{0} ||
           execution.transaction.diagnostic.has_value() ||
           execution.transaction.selected_providers !=
               prepared.invocation.selected_repository_providers) {
            return;
        }

        const TrustedAlpmReceiptCaptureResult& capture =
            execution.receipt_capture.value();
        if(capture.status != TrustedAlpmReceiptCaptureStatus::Complete ||
           capture.pacman_exit_status != std::optional<int>{0} ||
           capture.diagnostic.has_value() ||
           capture.transaction_ledger.transactions.size() != 1) {
            return;
        }
        const InvocationDependencyTransaction& transaction =
            capture.transaction_ledger.transactions.front();
        const std::vector<std::string> requested_package_names =
            selected_provider_package_names(
                prepared.invocation.selected_repository_providers);
        if(transaction.owner !=
               InvocationDependencyTransactionOwner::
                   SelectedRepositoryProvider ||
           !is_valid_pacman_transaction_token(
               transaction.transaction_token) ||
           transaction.command_outcome !=
               InvocationDependencyTransactionCommandOutcome::Succeeded ||
           transaction.requested_package_names != requested_package_names ||
           !transaction.receipt.is_complete_for(
               transaction.transaction_token,
               InvocationDependencyTransactionOwner::
                   SelectedRepositoryProvider)) {
            return;
        }

        std::set<std::string> unique_requested_names;
        if(requested_package_names.empty() ||
           !std::all_of(
               requested_package_names.begin(), requested_package_names.end(),
               [&unique_requested_names](const std::string& package_name) {
                   return is_valid_package_name(package_name) &&
                          unique_requested_names.insert(package_name).second;
               })) {
            return;
        }
        std::vector<std::string> actual_install_set;
        for(const PacmanInstalledPackageReceipt& installed :
            transaction.receipt.newly_installed_packages()) {
            actual_install_set.push_back(installed.package_name);
        }
        if(actual_install_set.empty() ||
           std::any_of(
               requested_package_names.begin(), requested_package_names.end(),
               [&transaction](const std::string& package_name) {
                   return !transaction.receipt
                               .contains_newly_installed_package(package_name);
               })) {
            return;
        }

        // LANDMINE(#485): build the complete immutable snapshot before the
        // session inventory retires the token. Registration is the final
        // authority transition and must happen exactly once.
        SelectedRepositoryProviderTrustedExecutionEvidence evidence(
            authority,
            transaction.transaction_token,
            projection->bindings,
            execution.transaction,
            capture,
            std::move(actual_install_set));
        if(!authority.register_trusted_transaction_token(
               InvocationDependencyTransactionOwner::
                   SelectedRepositoryProvider,
               transaction.transaction_token,
               projection->work_item_indices)) {
            return;
        }
        if(!authority.mark_trusted_transaction_completed(
               InvocationDependencyTransactionOwner::
                   SelectedRepositoryProvider,
               transaction.transaction_token)) {
            return;
        }
        execution.trusted_execution_evidence_ = std::move(evidence);
    }
};

SelectedRepositoryProviderTransactionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig& config) {
    PreparedSelectedRepositoryProviderTransaction prepared =
        prepare_selected_repository_provider_transaction(invocation);
    SelectedRepositoryProviderTransactionResult result =
        std::move(prepared.result);
    if(result.selected_providers.empty() || !prepared.directive.has_value()) {
        return result;
    }
    try {
        const int exit_status = install_selected_repository_providers(
            result.selected_providers, prepared.directive.value(),
            config);
        result.command_exit_status = exit_status;
        if(exit_status == 0) {
            result.status =
                SelectedRepositoryProviderTransactionStatus::Succeeded;
            return result;
        }
        result.status = SelectedRepositoryProviderTransactionStatus::Failed;
        result.diagnostic = localization::translate_message(
            "Failed to install selected repository providers.");
        return result;
    } catch(const std::exception& error) {
        result.status = SelectedRepositoryProviderTransactionStatus::Failed;
        result.diagnostic = error.what();
        return result;
    } catch(...) {
        result.status = SelectedRepositoryProviderTransactionStatus::Failed;
        result.diagnostic = localization::translate_message(
            "Failed to install selected repository providers with an unknown exception.");
        return result;
    }
}

SelectedRepositoryProviderTrustedReceiptExecutionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig& config,
    SelectedRepositoryProviderTrustedReceiptRequest receipt_request) {
    const PreparedProductionSourceBuildInvocation& selected_invocation =
        SelectedRepositoryProviderTrustedReceiptExecutor::select_invocation(
            receipt_request, invocation);
    if(!SelectedRepositoryProviderTrustedReceiptExecutor::session_is_ready(
           receipt_request)) {
        SelectedRepositoryProviderTransactionResult blocked;
        blocked.status = SelectedRepositoryProviderTransactionStatus::
            BlockedBeforeExecution;
        blocked.selected_providers =
            selected_invocation.selected_repository_providers;
        blocked.package_state_change = PackageStateChange::Unknown;
        // NO_TRANSLATE: trusted cleanup transport has no production CLI caller.
        blocked.diagnostic =
            "Cleanup invocation baseline was not observed before the selected-provider transaction.";
        return SelectedRepositoryProviderTrustedReceiptExecutionResult{
            std::move(blocked), std::nullopt};
    }
    PreparedSelectedRepositoryProviderTransaction prepared =
        prepare_selected_repository_provider_transaction(selected_invocation);
    SelectedRepositoryProviderTrustedReceiptExecutionResult execution{
        std::move(prepared.result), std::nullopt};
    if(execution.transaction.selected_providers.empty() ||
       !prepared.directive.has_value()) {
        return execution;
    }

    TrustedAlpmReceiptSelectedProviderRequest transport_request;
    transport_request.install_directive =
        prepared.directive.value() ==
                RepositoryProviderInstallDirective::AsDependency
            ? TrustedAlpmReceiptRepositoryInstallDirective::AsDependency
            : TrustedAlpmReceiptRepositoryInstallDirective::
                  PreserveExistingReason;
    transport_request.no_confirm = config.no_confirm;
    transport_request.targets.reserve(
        execution.transaction.selected_providers.size());
    for(const ProvidedDependency& provider :
        execution.transaction.selected_providers) {
        const auto& repository =
            std::get<RepositoryProviderOrigin>(provider.origin);
        transport_request.targets.push_back(
            TrustedAlpmReceiptRepositoryTarget{
                repository.repository_name,
                provider.package_name});
    }

    execution.receipt_capture =
        execute_trusted_alpm_receipt_selected_provider_transaction(
            transport_request);
    const TrustedAlpmReceiptCaptureResult& capture =
        execution.receipt_capture.value();
    execution.transaction.command_exit_status = capture.pacman_exit_status;
    if(!capture.pacman_exit_status.has_value()) {
        const bool pre_transaction_transport_failure =
            capture.status ==
                TrustedAlpmReceiptCaptureStatus::InvalidRequest ||
            capture.status == TrustedAlpmReceiptCaptureStatus::
                                  TrustedExecutableUnavailable ||
            capture.status == TrustedAlpmReceiptCaptureStatus::
                                  TokenGenerationFailed;
        const bool transaction_pre_launch_failure =
            capture.status ==
                TrustedAlpmReceiptCaptureStatus::PrepareFailed ||
            capture.status ==
                TrustedAlpmReceiptCaptureStatus::AbortFailed;
        const bool transaction_definitely_not_started =
            transaction_pre_launch_failure &&
            capture.transaction_ledger.transactions.size() == 1 &&
            capture.transaction_ledger.transactions.front()
                    .command_outcome ==
                InvocationDependencyTransactionCommandOutcome::
                    NotAttempted;
        execution.transaction.status =
            pre_transaction_transport_failure ||
                    transaction_definitely_not_started
                ? SelectedRepositoryProviderTransactionStatus::
                      BlockedBeforeExecution
                : SelectedRepositoryProviderTransactionStatus::
                      OutcomeUnknown;
        execution.transaction.diagnostic = capture.diagnostic.value_or(
            localization::translate_message(
                "Failed to install selected repository providers."));
        return execution;
    }
    if(capture.pacman_exit_status.value() != 0) {
        execution.transaction.status =
            SelectedRepositoryProviderTransactionStatus::Failed;
        execution.transaction.diagnostic = localization::translate_message(
            "Failed to install selected repository providers.");
        return execution;
    }

    // Receipt availability is a separate evidence dimension. A successful
    // package transaction remains successful even when cleanup authority must
    // fail closed because consume/publication was unavailable.
    execution.transaction.status =
        SelectedRepositoryProviderTransactionStatus::Succeeded;
    SelectedRepositoryProviderTrustedReceiptExecutor::
        close_execution_evidence(execution, receipt_request);
    return execution;
}

namespace {

const ValidatedCacheRoot& require_prepared_cache_root(
    const ProductionSourceBuildWorkItem& work_item) {
    if(!work_item.cache_root.has_value()) {
        throw std::logic_error(
            localization::translate_message(
                "Production source-build work item has no prepared cache authority."));
    }
    work_item.cache_root->require_unchanged_identity();
    return work_item.cache_root.value();
}

void require_registered_repository_package_base_work_item(
    const ProductionSourceBuildWorkItem& work_item,
    const AppConfig& config) {
    require_static_production_source_build_work_item(work_item);
    if(work_item.required_target_provenance !=
           RequiredTargetProvenance::RepositoryExactPackageProjection ||
       work_item.artifact_lifecycle_intent !=
           ArtifactLifecycleIntent::PackageBaseSet ||
       !work_item.repository_identity.has_value()) {
        throw std::logic_error(
            "Registered repository prepared source-build requires exact repository PackageBase-set provenance.");
    }
    if(work_item.request.only_if_updated) {
        throw std::logic_error(
            "Registered repository PackageBase-set work item must keep only-if-updated out of the lower request.");
    }
    if(work_item.request.needed) {
        throw std::logic_error(
            "Registered repository PackageBase-set work item does not support needed execution.");
    }
    if(work_item.required_targets.size() != 1 ||
       work_item.required_targets.front().desired_reason !=
           DesiredInstallReason::Explicit) {
        throw std::logic_error(
            "Registered repository PackageBase-set work item requires exactly one explicit requested child.");
    }
    require_supported_separated_install_options(config.rm_deps);
    require_unclaimed_artifact_pkgdest(work_item.request.custom_environment);
}

} // namespace

ResolvedSourceBuildIdentity resolve_source_build_identity(
    const std::string& package_name) {
    require_valid_package_name(package_name);

    StrictRepositoryPackageQueryResult repository_result =
        query_repository_package_strict(package_name);
    if(const auto* present =
           std::get_if<RepositoryPackagePresent>(&repository_result);
       present != nullptr) {
        if(present->package_name != package_name) {
            throw std::runtime_error(localization::format_translated_message(
                "Repository exact package identity does not match the requested package {}.",
                package_name));
        }
        return make_repository_source_build_identity(*present);
    }
    if(const auto* failure =
           std::get_if<RepositoryMetadataFailure>(&repository_result);
       failure != nullptr) {
        throw std::runtime_error(localization::format_translated_message(
            "Failed to inspect repository metadata for {}: {}",
            package_name,
            failure->diagnostic));
    }

    std::optional<AurPackageInfo> info;
    try {
        info = AurClient::info(package_name);
    } catch(const AurRpcResponseError&) {
        throw;
    } catch(const std::exception& error) {
        // TRANSLATORS: The placeholders are the AUR identity, a package name, and an AUR diagnostic.
        throw std::runtime_error(localization::format_translated_message(
            "Failed to fetch {} info for {}: {}",
            "AUR",
            package_name,
            error.what()));
    }

    if(!info.has_value()) {
        // TRANSLATORS: The placeholders are the AUR identity and a package name.
        throw std::runtime_error(localization::format_translated_message(
            "Package not found in repos or {}: {}",
            "AUR",
            package_name));
    }
    if(info->PackageBase.empty()) {
        // TRANSLATORS: The placeholders are the AUR identity, package name, and PackageBase field identity.
        throw std::runtime_error(localization::format_translated_message(
            "{} info for {} does not include {}.",
            "AUR", package_name, "PackageBase"));
    }
    require_valid_package_name(info->PackageBase);

    return ResolvedSourceBuildIdentity{
        ResolvedAurSourceBuildIdentity{
            package_name,
            info->PackageBase}};
}

ResolvedSourceBuildIdentity make_repository_source_build_identity(
    const RepositoryPackagePresent& package) {
    require_valid_package_name(package.package_name);
    require_valid_package_name(package.package_base);
    if(package.repository_name.empty()) {
        throw std::invalid_argument(localization::translate_message(
            "Repository source-build identity has no repository name."));
    }
    return ResolvedSourceBuildIdentity{
        ResolvedRepositorySourceBuildIdentity{package}};
}

bool build_source_target(
    const std::string& package_name,
    const SourceBuildEnvironment& custom_environment,
    const AppConfig& config) {
    RemoteSourceBuildPreparation preparation = prepare_remote_source_build(
        package_name, custom_environment, config);
    if(const auto* blocked =
           std::get_if<RemoteSourceBuildPlanFailure>(&preparation);
       blocked != nullptr) {
        require_executable_build_plan(package_name, blocked->plan);
        throw std::logic_error(localization::translate_message(
            "Remote source-build plan was rejected without a blocking detail."));
    }
    PreparedRemoteSourceBuild prepared = std::move(
        std::get<PreparedRemoteSourceBuild>(preparation));
    if(prepared.source.source_kind() == SourceBuildSourceKind::Aur) {
        const auto collected = collect_remote_aur_cleanup_candidates(std::move(prepared), config);
        return collected.invocation_result().is_success();
    }
    return execute_prepared_source_build_invocation(
               std::move(prepared.invocation), config)
        .is_success();
}

RemoteSourceBuildPreparation prepare_remote_source_build(
    const std::string& package_name,
    const SourceBuildEnvironment& custom_environment,
    const AppConfig& config) {
    // --rmdepsはAUR/repository probeより前に、invocation optionとして拒否する。
    require_supported_production_source_build_options(config);
    require_valid_package_name(package_name);
    ResolvedSourceBuildIdentity source =
        resolve_source_build_identity(package_name);
    std::vector<ProductionSourceBuildWorkItem> work_items;
    std::optional<BuildPlan> aur_build_plan;
    ProviderSelectionCallback select_provider =
        provider_selection_callback(config);
    if(source.source_kind() == SourceBuildSourceKind::Aur) {
        BuildPlan plan = resolve_build_plan(package_name, select_provider);
        try {
            require_executable_build_plan(package_name, plan);
        } catch(const std::exception&) {
            return RemoteSourceBuildPlanFailure{
                std::move(source), std::move(plan)};
        }
        work_items = prepare_aur_source_build_work_items(
            plan, false, false);
        auto root_work_item = std::find_if(
            work_items.begin(), work_items.end(),
            [&source](const ProductionSourceBuildWorkItem& candidate) {
                return candidate.request.checkout_name ==
                       source.package_base();
            });
        if(root_work_item == work_items.end()) {
            throw std::logic_error(localization::format_translated_message(
                "{} required artifact target projection omitted {}.",
                "BuildPlan", source.requested_name()));
        }
        root_work_item->request.custom_environment = custom_environment;
        root_work_item->request.empty_value_policy =
            SourceEnvironmentEmptyValuePolicy::Forward;
        aur_build_plan.emplace(std::move(plan));
    } else {
        work_items.push_back(make_direct_source_build_work_item(
            source, custom_environment,
            SourceEnvironmentEmptyValuePolicy::Forward, false, false,
            ArtifactLifecycleIntent::PackageBaseSet,
            select_provider));
    }
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    return PreparedRemoteSourceBuild{
        std::move(source), std::move(aur_build_plan),
        std::move(invocation)};
}

ProductionSourceBuildWorkItem prepare_resolved_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    bool only_if_updated,
    bool needed) {
    return prepare_resolved_source_build_work_item(
        identity, std::move(environment), only_if_updated, needed,
        ProviderSelectionCallback{});
}

ProductionSourceBuildWorkItem prepare_resolved_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    bool only_if_updated,
    bool needed,
    const ProviderSelectionCallback& select_provider) {
    return make_direct_source_build_work_item(
        identity, std::move(environment),
        SourceEnvironmentEmptyValuePolicy::Omit, only_if_updated, needed,
        ArtifactLifecycleIntent::SingularCompatibility,
        select_provider);
}

ProductionSourceBuildWorkItem prepare_registered_source_build_work_item(
    const ResolvedSourceBuildIdentity& identity,
    SourceBuildEnvironment environment,
    const ProviderSelectionCallback& select_provider) {
    const bool is_repository =
        identity.source_kind() == SourceBuildSourceKind::Repository;
    return make_direct_source_build_work_item(
        identity, std::move(environment),
        SourceEnvironmentEmptyValuePolicy::Omit,
        !is_repository,
        false,
        is_repository ? ArtifactLifecycleIntent::PackageBaseSet
                      : ArtifactLifecycleIntent::SingularCompatibility,
        select_provider);
}

std::vector<ProductionSourceBuildWorkItem> prepare_aur_source_build_work_items(
    const BuildPlan& plan,
    bool use_source_build_preferences,
    bool needed) {
    require_compatible_selected_provider_package_identities(plan);
    BuildPlanArtifactTargetProjectionResult projection =
        project_build_plan_required_artifact_targets(plan);
    if(!projection.is_success()) {
        // TRANSLATORS: The placeholder is the literal BuildPlan identity.
        throw std::logic_error(localization::format_translated_message(
            "{} required artifact target projection failed before source-build work-item preparation.",
            "BuildPlan"));
    }

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.reserve(projection.success()->build_units.size());
    for(const auto& unit : projection.success()->build_units) {
        work_items.push_back(make_aur_source_build_work_item(
            unit, plan, use_source_build_preferences, needed));
    }
    return work_items;
}

ProductionSourceBuildWorkItem prepare_smart_source_build_work_item(
    const std::string& package_name,
    bool only_if_updated,
    bool needed) {
    return prepare_smart_source_build_work_item(
        package_name, only_if_updated, needed,
        ProviderSelectionCallback{});
}

ProductionSourceBuildWorkItem prepare_smart_source_build_work_item(
    const std::string& package_name,
    bool only_if_updated,
    bool needed,
    const ProviderSelectionCallback& select_provider) {
    SourceBuildEnvironment environment =
        load_source_preference_environment(package_name);
    ResolvedSourceBuildIdentity identity =
        resolve_source_build_identity(package_name);
    return prepare_resolved_source_build_work_item(
        identity, std::move(environment), only_if_updated, needed,
        select_provider);
}

SourceBuildPackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    // target provenanceではなく明示されたlifecycle intentだけでset ownerを選ぶ。
    require_static_production_source_build_work_item(work_item);
    if(work_item.artifact_lifecycle_intent !=
       ArtifactLifecycleIntent::PackageBaseSet) {
        throw std::logic_error(
            "PackageBase set source-build execution received a non-set lifecycle work item.");
    }
    if(work_item.request.only_if_updated) {
        throw std::logic_error(
            localization::format_translated_message(
                "{} set source-build execution does not support only-if-updated requests.",
                "PackageBase"));
    }

    if(work_item.required_target_provenance ==
       RequiredTargetProvenance::AurBuildPlanProjection) {
        // TRANSLATORS: The placeholders are AUR and PackageBase identities and an AUR PackageBase name.
        Logger::info(localization::format_translated_message(
            "Building {} {}: {}", "AUR", "PackageBase",
            work_item.request.checkout_name));
        // TRANSLATORS: The placeholder is a comma-separated list of package names.
        Logger::info(localization::format_translated_message(
            "Target package(s): {}",
            join_required_package_names(work_item.required_targets)));
    }
    return execute_source_build_package_base_typed(
        work_item.request, work_item.required_targets,
        require_prepared_cache_root(work_item),
        database_paths, config);
}

#ifndef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
namespace {

PackageBaseSourceBuildExecutionResult
execute_prepared_package_base_source_build_work_item_with_cleanup(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index) {
    require_static_production_source_build_work_item(work_item);
    if(work_item.artifact_lifecycle_intent !=
       ArtifactLifecycleIntent::PackageBaseSet) {
        throw std::logic_error(
            "PackageBase set source-build execution received a non-set lifecycle work item.");
    }
    if(work_item.request.only_if_updated) {
        throw std::logic_error(localization::format_translated_message(
            "{} set source-build execution does not support only-if-updated requests.",
            "PackageBase"));
    }
    if(work_item.required_target_provenance ==
       RequiredTargetProvenance::AurBuildPlanProjection) {
        Logger::info(localization::format_translated_message(
            "Building {} {}: {}", "AUR", "PackageBase",
            work_item.request.checkout_name));
        Logger::info(localization::format_translated_message(
            "Target package(s): {}",
            join_required_package_names(work_item.required_targets)));
    }
    return execute_source_build_package_base_with_cleanup_authority(
        work_item.request, work_item.required_targets,
        require_prepared_cache_root(work_item), database_paths,
        config, collector, work_item_index);
}

} // namespace
#endif

SourceBuildPreparationOutcome
prepare_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    SourceBuildUpdatePolicy update_policy,
    const AppConfig& config) {
    require_registered_repository_package_base_work_item(work_item, config);
    try {
        return prepare_source_build_for_execution(
            work_item.request, work_item.request.checkout_name,
            update_policy, require_prepared_cache_root(work_item),
            config);
    } catch(const ReviewedSourceProductionError&) {
        throw;
    } catch(const TrustedCacheError&) {
        throw;
    } catch(const ConfirmationOperationStopped&) {
        throw;
    } catch(const std::exception& error) {
        // registered CLIの既存prefix ownerへ原診断を返し、checkout failureを
        // PackageBase lower phaseの文言へ置換しない。
        throw std::runtime_error(error.what());
    } catch(...) {
        throw std::runtime_error(localization::translate_message(
            "Source checkout or build preparation failed."));
    }
}

RegisteredSourcePackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    PreparedSourceBuildNeedsBuild prepared,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    require_registered_repository_package_base_work_item(work_item, config);
    return std::get<PackageBaseSourceBuildExecutionResult>(execute_prepared_source_build_package_base_typed(
        work_item.request, work_item.required_targets,
        std::move(prepared), database_paths, config));
}

SourceBuildExecutionResult execute_prepared_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    require_static_production_source_build_work_item(work_item);
    const RequiredPackageArtifactTarget& target =
        require_singular_required_package_target(work_item);
    if(work_item.required_target_provenance ==
       RequiredTargetProvenance::AurBuildPlanProjection) {
        // TRANSLATORS: The placeholders are AUR and PackageBase identities and an AUR PackageBase name.
        Logger::info(localization::format_translated_message(
            "Building {} {}: {}", "AUR", "PackageBase",
            work_item.request.checkout_name));
        // TRANSLATORS: The placeholder is a comma-separated list of package names.
        Logger::info(localization::format_translated_message(
            "Target package(s): {}",
            join_required_package_names(work_item.required_targets)));
    }

    try {
        return execute_source_build_typed(
            work_item.request, require_prepared_cache_root(work_item),
            target.desired_reason,
            database_paths, config);
    } catch(const ReviewedSourceProductionError&) {
        throw;
    } catch(const SeparatedSourceBuildPhaseError&) {
        throw;
    } catch(const SeparatedSourceBuildCleanupError&) {
        // POLICY(#242): install成功後cleanup失敗の型とdiagnosticをgeneric
        // build/install failureへflattenしない。
        throw;
    } catch(const TrustedCacheError&) {
        // Cache authority failureはtyped callerがphase/codeを保持できるよう、
        // generic build/install diagnosticへwrapしない。
        throw;
    } catch(const ConfirmationOperationStopped&) {
        throw;
    } catch(const std::exception& error) {
        // TRANSLATORS: The placeholders are the PackageBase identity, an AUR PackageBase name, package name, and build/install diagnostic.
        throw std::runtime_error(localization::format_translated_message(
            "Failed while building/installing {} {} ({}): {}",
            "PackageBase",
            work_item.request.checkout_name,
            work_item.request.package_name,
            error.what()));
    }
}

std::optional<ArtifactInstallExecutionOutcome>
execute_prepared_source_build_work_item(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    return flatten_source_build_result(
        execute_prepared_source_build_work_item_typed(
            work_item, database_paths, config));
}

#ifdef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
namespace {
SourceInvocationExecutionTestHooks g_invocation_execution_hooks;
}
void set_source_invocation_execution_test_hooks(SourceInvocationExecutionTestHooks hooks) {
    g_invocation_execution_hooks = std::move(hooks);
}
#endif

namespace {

// Retain the terminal owner before any fallible display copy. A returned partial
// is not an exception: it must never enter the exception-only aggregate helper.
bool retain_authoritative_execution(ProductionSourceBuildWorkItemOutcome& outcome,
                                    ReviewedDevelExecutionSnapshot execution,
                                    bool partial) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<ReviewedDevelExecutionSnapshot>);
    outcome.devel_execution.emplace(std::move(execution));
    try {
        outcome.production_outcome = outcome.devel_execution->production_outcome;
    } catch(...) {
        outcome.devel_execution->projection_failed = true;
        outcome.devel_execution->complete = false;
        partial = true;
    }
    partial = partial || !outcome.devel_execution->complete;
    outcome.status = partial ? ProductionSourceBuildWorkItemStatus::AuthoritativePartial
                             : ProductionSourceBuildWorkItemStatus::Succeeded;
    return partial;
}

ProductionSourceBuildInvocationResult
execute_prepared_source_build_invocation_impl(
    PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig& config,
    RemoteAurCleanupCandidateCollector* collector) {
    ProductionSourceBuildInvocationResult aggregate;
    aggregate.work_items.reserve(invocation.work_items.size());
    for(const ProductionSourceBuildWorkItem& work_item :
        invocation.work_items) {
        ProductionSourceBuildWorkItemOutcome outcome;
        outcome.package_base = work_item.request.checkout_name;
        aggregate.work_items.push_back(std::move(outcome));
    }
#ifdef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
    // Isolated fixture owns dispatch; no host cache/provider transaction.
    SelectedRepositoryProviderTransactionResult provider_transaction;
#else
    activate_production_source_build_cache(invocation);
    SelectedRepositoryProviderTransactionResult provider_transaction =
        collector == nullptr
            ? execute_selected_repository_provider_transaction(
                  invocation, config)
            : collector->execute_selected_repository_provider_transaction(
                  config);
#endif
    if(!provider_transaction.is_success()) {
        const std::string diagnostic =
            provider_transaction.diagnostic.value_or(
                localization::translate_message(
                    "Failed to install selected repository providers."));
        if(collector != nullptr) {
            throw RemoteAurCleanupInvocationExecutionError(
                std::move(aggregate), std::move(provider_transaction),
                diagnostic);
        }
        throw std::runtime_error(diagnostic);
    }
    for(std::size_t index = 0; index < invocation.work_items.size();
        ++index) {
        const ProductionSourceBuildWorkItem& work_item =
            invocation.work_items[index];
        ProductionSourceBuildWorkItemOutcome& work_item_outcome =
            aggregate.work_items[index];
        auto stop_with_failure = [&aggregate, &work_item_outcome, index](
                                     ProductionSourceBuildFailureStage
                                         failure_stage,
                                     std::optional<
                                         ProductionSourceBuildStagedOutcome>
                                         production_outcome,
                                     const std::string& diagnostic) {
            work_item_outcome.status =
                ProductionSourceBuildWorkItemStatus::Failed;
            work_item_outcome.production_outcome =
                std::move(production_outcome);
            work_item_outcome.failure_stage = failure_stage;
            work_item_outcome.diagnostic = diagnostic;
            work_item_outcome.failure_exception =
                std::current_exception();
            throw ProductionSourceBuildInvocationError(
                std::move(aggregate), index, diagnostic);
        };
        if(work_item.artifact_lifecycle_intent ==
           ArtifactLifecycleIntent::PackageBaseSet) {
            try {
                SourceBuildPackageBaseExecutionResult execution =
#ifdef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
                    g_invocation_execution_hooks.package_base(work_item, invocation.database_paths, config);
#else
                    collector != nullptr && collector->should_use_trusted_source_artifact_install(index)
                        ? execute_prepared_package_base_source_build_work_item_with_cleanup(work_item, invocation.database_paths, config, *collector, index)
                        : execute_prepared_package_base_source_build_work_item_typed(work_item, invocation.database_paths, config);
#endif
                if(auto* devel = std::get_if<ReviewedDevelExecutionSnapshot>(&execution)) {
                    if(retain_authoritative_execution(work_item_outcome, std::move(*devel), false)) return aggregate;
                    continue;
                }
                auto result = std::get<PackageBaseSourceBuildExecutionResult>(std::move(execution));
                work_item_outcome.status =
                    ProductionSourceBuildWorkItemStatus::Succeeded;
                work_item_outcome.production_outcome =
                    result.production_outcome();
                present_production_source_build_outcome(
                    result.package_base(),
                    result.production_outcome());
                present_package_base_result(work_item, result);
            } catch(const ProductionSourceBuildInvocationError&) {
                throw;
            } catch(const SeparatedPackageBaseSourceBuildCleanupError& error) {
                // Transaction完了済みのchild outcomeを失わず表示し、
                // callerがcleanup failureを成功と扱わないようaggregateへ保持する。
                present_production_source_build_outcome(
                    error.result().package_base(),
                    error.result().production_outcome());
                present_package_base_result(work_item, error.result());
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Cleanup,
                    error.result().production_outcome(),
                    error.what());
            } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                ProductionSourceBuildFailureStage failure_stage =
                    ProductionSourceBuildFailureStage::Other;
                switch(error.phase()) {
                    case SeparatedPackageBaseSourceBuildFailurePhase::Build:
                        failure_stage =
                            error.reviewed_source_failure().has_value()
                                ? ProductionSourceBuildFailureStage::Review
                                : ProductionSourceBuildFailureStage::Build;
                        break;
                    case SeparatedPackageBaseSourceBuildFailurePhase::
                        ArtifactValidation:
                    case SeparatedPackageBaseSourceBuildFailurePhase::
                        ArtifactIdentity:
                        failure_stage = ProductionSourceBuildFailureStage::
                            ArtifactValidation;
                        break;
                    case SeparatedPackageBaseSourceBuildFailurePhase::
                        InstallPreparation:
                        failure_stage = ProductionSourceBuildFailureStage::
                            InstallPreparation;
                        break;
                    case SeparatedPackageBaseSourceBuildFailurePhase::
                        InstallTransaction:
                        failure_stage = ProductionSourceBuildFailureStage::
                            InstallTransaction;
                        break;
                }
                stop_with_failure(
                    failure_stage, error.production_outcome(),
                    error.what());
            } catch(const SeparatedPackageBaseSourceBuildPreparationError&
                        error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                stop_with_failure(
                    ProductionSourceBuildFailureStage::
                        InstallPreparation,
                    error.production_outcome(), error.what());
            } catch(const PackageBaseArtifactInstallTransactionError& error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                stop_with_failure(
                    ProductionSourceBuildFailureStage::
                        InstallTransaction,
                    error.production_outcome(), error.what());
            } catch(const ReviewedSourceProductionError& error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Review,
                    error.production_outcome(), error.what());
            } catch(const std::exception& error) {
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Other,
                    std::nullopt, error.what());
            } catch(...) {
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Other,
                    std::nullopt,
                    localization::translate_message(
                        "Source-build work item failed with an unknown error."));
            }
        } else {
            try {
                SourceBuildExecutionResult result =
#ifdef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
                    g_invocation_execution_hooks.singular(work_item, invocation.database_paths, config);
#else
                    execute_prepared_source_build_work_item_typed(work_item, invocation.database_paths, config);
#endif
                work_item_outcome.devel_update_query = std::move(result.devel_update_query);
                if(result.devel_execution) {
                    if(retain_authoritative_execution(work_item_outcome, std::move(*result.devel_execution), result.status == SourceBuildExecutionStatus::AuthoritativeIncomplete)) return aggregate;
                    continue;
                }
                work_item_outcome.status = ProductionSourceBuildWorkItemStatus::Succeeded;
                work_item_outcome.production_outcome = result.production_outcome;
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    result.production_outcome);
            } catch(const ProductionSourceBuildInvocationError&) {
                throw;
            } catch(const SeparatedSourceBuildPhaseError& error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                ProductionSourceBuildFailureStage failure_stage =
                    ProductionSourceBuildFailureStage::Other;
                switch(error.phase()) {
                    case SeparatedSourceBuildFailurePhase::Build:
                        failure_stage =
                            ProductionSourceBuildFailureStage::Build;
                        break;
                    case SeparatedSourceBuildFailurePhase::ArtifactValidation:
                        failure_stage = ProductionSourceBuildFailureStage::
                            ArtifactValidation;
                        break;
                    case SeparatedSourceBuildFailurePhase::InstallPreparation:
                        failure_stage = ProductionSourceBuildFailureStage::
                            InstallPreparation;
                        break;
                    case SeparatedSourceBuildFailurePhase::InstallTransaction:
                        failure_stage = ProductionSourceBuildFailureStage::
                            InstallTransaction;
                        break;
                }
                stop_with_failure(
                    failure_stage, error.production_outcome(),
                    error.what());
            } catch(const SeparatedSourceBuildCleanupError& error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Cleanup,
                    error.production_outcome(), error.what());
            } catch(const ReviewedSourceProductionError& error) {
                present_production_source_build_outcome(
                    work_item.request.checkout_name,
                    error.production_outcome());
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Review,
                    error.production_outcome(), error.what());
            } catch(const std::exception& error) {
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Other,
                    std::nullopt, error.what());
            } catch(...) {
                stop_with_failure(
                    ProductionSourceBuildFailureStage::Other,
                    std::nullopt,
                    localization::translate_message(
                        "Source-build work item failed with an unknown error."));
            }
        }
    }
    return aggregate;
}

} // namespace

ProductionSourceBuildInvocationResult execute_prepared_source_build_invocation(
    PreparedProductionSourceBuildInvocation invocation,
    const AppConfig& config) {
    return execute_prepared_source_build_invocation_impl(
        invocation, config, nullptr);
}

ProductionSourceBuildInvocationResult
execute_prepared_remote_aur_cleanup_invocation(
    RemoteAurCleanupCandidateCollector& collector,
    const AppConfig& config) {
    PreparedProductionSourceBuildInvocation& invocation =
        collector.prepared_invocation_for_execution();
    return execute_prepared_source_build_invocation_impl(
        invocation, config, &collector);
}
