#include "dry_run.hpp"

#include "app_config.hpp"
#include "aur_update_query.hpp"
#include "aur_devel_update.hpp"
#include "cli_authority.hpp"
#include "cli_parser.hpp"
#include "cli_routing.hpp"
#include "cli_runtime_contract.hpp"
#include "commands_inspect.hpp"
#include "commands_source_maintenance.hpp"
#include "commands_sync.hpp"
#include "commands_upgrade_all.hpp"
#include "filtered_aur_update_operation.hpp"
#include "local_dependency_plan_projection.hpp"
#include "local_source_build.hpp"
#include "local_source_metadata_evaluation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "runtime_diagnostic.hpp"
#include "source_install.hpp"
#include "system_aur_update_operation.hpp"
#include "system_source_upgrade.hpp"
#include "unified_plan_projection.hpp"
#include "unified_plan_renderer.hpp"
#include "upgrade_all_operation.hpp"

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr int DRY_RUN_BLOCKED_STATUS = 1;

int render_dry_run_projection(
    const std::unique_ptr<UnifiedPlanProjection>& projection) {
    if(projection == nullptr) {
        throw std::logic_error(localization::translate_message(
            "Dry-run projection did not produce an observation."));
    }
    const UnifiedPlanObservationResult& result =
        projection->observation_result();
    if(!result.is_valid() || result.observation() == nullptr) {
        throw std::logic_error(localization::translate_message(
            "Dry-run projection produced an invalid observation."));
    }

    const UnifiedPlanObservation& observation = *result.observation();
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);
    std::cout << rendered.text;

    // Renderer-local completeness never changes execution authority.
    switch(observation.status()) {
        case UnifiedPlanObservationStatus::Ready:
        case UnifiedPlanObservationStatus::NoOp:
            return 0;
        case UnifiedPlanObservationStatus::Blocked:
            return DRY_RUN_BLOCKED_STATUS;
    }
    throw std::logic_error(localization::translate_message(
        "Dry-run observation has an unknown status."));
}

int render_system_aur_update_dry_run_projection(
    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>&
        projection) {
    if(projection == nullptr) {
        throw std::logic_error(localization::format_translated_message(
            "System and {} update dry-run projection did not produce an observation.",
            "AUR"));
    }
    const UnifiedPlanRenderingResult rendered =
        render_system_aur_update_unified_plan(*projection);
    std::cout << rendered.text;
    return projection->status() ==
                   SystemAurUpdateUnifiedPlanStatus::Ready
               ? 0
               : DRY_RUN_BLOCKED_STATUS;
}

int run_system_aur_update_dry_run(
    const SyncInvocationRouteClassification& route,
    const AppConfig& config) {
    std::optional<SystemAurUpdateDryRunRequest> request;
    if(const auto* auto_candidate =
           std::get_if<AutoSystemUpdateRouteCandidate>(&route)) {
        // Normal AUR execution options must be rejected before observing any
        // repository or AUR authority, matching the actual pre-mutation gate.
        require_supported_production_source_build_options(config);
        request = SystemAurUpdateDryRunRequest::from_auto_candidate(
            *auto_candidate);
    } else if(const auto* repo_only =
                  std::get_if<RepoOnlySystemUpdateRouteCandidate>(
                      &route)) {
        request.emplace(*repo_only);
    }
    if(!request.has_value()) {
        throw std::logic_error(localization::format_translated_message(
            "System and {} update dry-run route is not executable.",
            "AUR"));
    }

    const SystemAurUpdateDryRunObservation observation =
        observe_system_aur_update_dry_run(
            std::move(request.value()), config);
    return render_system_aur_update_dry_run_projection(
        project_system_aur_update_unified_plan(observation));
}

int run_root_selection_dry_run(
    const ParsedCliArguments& parsed,
    const AppConfig& config) {
    RootPackageSelectionInvocation invocation =
        require_root_package_selection_invocation(parsed);
    RootPackageInstallPreparation preparation =
        prepare_root_package_install(
            parsed, std::move(invocation), config);
    return std::visit(
        [](const auto& authority) {
            return render_dry_run_projection(
                project_root_package_unified_plan(
                    RootPackageUnifiedPlanProjectionInput{
                        std::cref(authority)}));
        },
        preparation);
}

int run_sync_dry_run(
    const ParsedCliArguments& parsed,
    bool system_update,
    const AppConfig& config) {
    if(parsed.root_package_selection_requested) {
        return run_root_selection_dry_run(parsed, config);
    }
    const SyncInvocationRouteClassification route =
        classify_sync_invocation_route(parsed);
    if(!std::holds_alternative<OtherSyncRoute>(route)) {
        return run_system_aur_update_dry_run(route, config);
    }
    SyncInstallPreparation preparation = prepare_sync_install(
        parsed, system_update, parsed.source_selection, config);
    return std::visit(
        [](const auto& authority) {
            return render_dry_run_projection(
                project_sync_install_unified_plan(
                    SyncInstallUnifiedPlanProjectionInput{
                        std::cref(authority)}));
        },
        preparation);
}

int run_fetch_dry_run(
    const ParsedCliArguments& parsed,
    const AppConfig& config) {
    FetchPreparation preparation =
        prepare_fetch_operation(parsed.targets, config);
    return render_dry_run_projection(project_fetch_unified_plan(
        FetchUnifiedPlanProjectionInput{std::cref(preparation)}));
}

int run_remote_build_dry_run(
    const ParsedCliArguments& parsed,
    const AppConfig& config) {
    RemoteSourceBuildInvocation invocation =
        require_remote_source_build_invocation(parsed.targets);
    RemoteSourceBuildPreparation preparation =
        prepare_remote_source_build(
            invocation.package_name,
            std::move(invocation.source_environment), config);
    return std::visit(
        [](const auto& authority) {
            return render_dry_run_projection(
                project_remote_source_build_unified_plan(
                    RemoteSourceBuildUnifiedPlanProjectionInput{
                        std::cref(authority)}));
        },
        preparation);
}

int run_local_build_dry_run(
    const ParsedCliArguments& parsed,
    const AppConfig& config) {
    LocalSourceBuildInvocation invocation =
        require_local_source_build_invocation(parsed);
    PreparedLocalSourceBuildRoute route =
        prepare_local_source_build_route(
            std::move(invocation), config);
    if(route.source_root.metadata().state() !=
       LocalSourceMetadataState::UsableUnverified) {
        return render_dry_run_projection(project_local_source_unified_plan(
            LocalSourceUnifiedPlanProjectionInput{
                LocalSourceMetadataEvaluationProjectionInput{
                    std::cref(route.source_root)}}));
    }

    const std::string effective_architecture =
        resolve_local_source_effective_architecture(
            route.invocation.source_environment);
    LocalSourceBuildMetadata metadata = bind_existing_local_source_metadata(
        route.source_root, effective_architecture);
    LocalBuildPlan plan = resolve_local_build_plan(
        metadata.metadata(), effective_architecture,
        PackageRelationLocalSourceIdentity{
            route.source_root.canonical_path(),
            route.source_root.directory_identity().device,
            route.source_root.directory_identity().inode},
        provider_selection_callback(config));
    if(!plan.failures().empty()) {
        return render_dry_run_projection(project_local_source_unified_plan(
            LocalSourceUnifiedPlanProjectionInput{
                LocalSourceBuildPlanFailureProjectionInput{
                    std::cref(route.source_root),
                    std::cref(plan)}}));
    }

    LocalSourceBuildProjectionAuthority authority =
        make_local_source_build_projection_authority(
            route.source_root, plan, metadata);
    return render_dry_run_projection(project_local_source_unified_plan(
        LocalSourceUnifiedPlanProjectionInput{
            std::cref(authority)}));
}

int run_upgrade_dry_run(const AppConfig& config) {
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(config);
    return std::visit(
        [](const auto& authority) -> int {
            using Authority = std::decay_t<decltype(authority)>;
            if constexpr(std::is_same_v<
                             Authority,
                             PreparedSystemSourceUpgrade>) {
                const SystemSourceUpgradeProjectionAuthority* view =
                    authority.projection_authority();
                if(view == nullptr) {
                    throw std::logic_error(
                        localization::translate_message(
                            "System/source upgrade preflight has no projection authority."));
                }
                const auto registered = observe_registered_aur_devel_updates(*view);
                return render_dry_run_projection(
                    project_system_source_upgrade_unified_plan(
                        SystemSourceUpgradeUnifiedPlanProjectionInput{
                            std::cref(*view), &registered}));
            } else {
                return render_dry_run_projection(
                    project_system_source_upgrade_unified_plan(
                        SystemSourceUpgradeUnifiedPlanProjectionInput{
                            std::cref(authority)}));
            }
        },
        preparation);
}

int run_upgrade_aur_dry_run(const AppConfig& config) {
    require_supported_production_source_build_options(config);
    AurUpdateQueryResult query_result = query_installed_aur_updates();
    PreparedFilteredAurUpdateOperation preparation =
        prepare_filtered_aur_update_operation(
            std::move(query_result), NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::BlockOperation,
            SavedSourcePreferencePolicy::Strict, config, std::nullopt);
    return render_dry_run_projection(
        project_filtered_aur_update_unified_plan(preparation));
}

int run_upgrade_all_dry_run(
    const ParsedCliArguments& parsed,
    const AppConfig& config) {
    const std::vector<std::string> validation_errors =
        validate_upgrade_all_invocation(parsed);
    if(!validation_errors.empty()) {
        for(const std::string& diagnostic : validation_errors) {
            Logger::error(diagnostic);
        }
        return DRY_RUN_BLOCKED_STATUS;
    }
    require_supported_production_source_build_options(config);

    UpgradeAllOperationPreparation preparation =
        prepare_upgrade_all_operation(config);
    if(const auto* failure =
           std::get_if<UpgradeAllOperationResult>(&preparation);
       failure != nullptr) {
        return render_dry_run_projection(project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(*failure)}));
    }

    const PreparedUpgradeAllOperation& prepared =
        std::get<PreparedUpgradeAllOperation>(preparation);
    const UpgradeAllOperationPreparedSnapshot* snapshot =
        prepared.snapshot();
    const UpgradeAllOperationProjectionAuthority* authority =
        prepared.projection_authority();
    if(snapshot == nullptr || authority == nullptr) {
        throw std::logic_error(localization::translate_message(
            "Upgrade-all preflight has no projection authority."));
    }
    PreparedUpgradeAllAurPreflight aur_preflight =
        prepare_upgrade_all_aur_preflight(*snapshot, config);
    const auto registered = observe_registered_aur_devel_updates(authority->system_source());
    return render_dry_run_projection(project_upgrade_all_unified_plan(
        *authority, aur_preflight, &registered));
}

} // namespace

int run_dry_run(
    const ParsedCliArguments& parsed,
    const AppConfig& config) {
    try {
        const CliInvocationValidation invocation_validation =
            validate_cli_invocation_contract(parsed);
        if(!invocation_validation.is_valid()) {
            const auto& diagnostic =
                invocation_validation.diagnostic.value();
            report_runtime_diagnostic(
                diagnostic,
                cli_invocation_issue_message(diagnostic.reason));
            return DRY_RUN_BLOCKED_STATUS;
        }
        if(const std::optional<std::string> selection_error =
               validate_source_selection_operation(parsed);
           selection_error.has_value()) {
            Logger::error(selection_error.value());
            return DRY_RUN_BLOCKED_STATUS;
        }
        const DryRunOperation route = classify_dry_run_operation(parsed);
        switch(route) {
            case DryRunOperation::SyncInstall:
                return run_sync_dry_run(parsed, false, config);
            case DryRunOperation::SyncSystemUpdate:
                return run_sync_dry_run(parsed, true, config);
            case DryRunOperation::Fetch:
                return run_fetch_dry_run(parsed, config);
            case DryRunOperation::RemoteBuild:
                return run_remote_build_dry_run(parsed, config);
            case DryRunOperation::LocalBuild:
                return run_local_build_dry_run(parsed, config);
            case DryRunOperation::Upgrade:
                return run_upgrade_dry_run(config);
            case DryRunOperation::UpgradeAur:
                return run_upgrade_aur_dry_run(config);
            case DryRunOperation::UpgradeAll:
                return run_upgrade_all_dry_run(parsed, config);
            case DryRunOperation::Unsupported:
                Logger::error(localization::format_translated_message(
                    "Option {} is not supported for operation {}.",
                    "--dry-run", parsed.operation));
                return DRY_RUN_BLOCKED_STATUS;
        }
        throw std::logic_error(localization::translate_message(
            "Dry-run route classification returned an unknown operation."));
    } catch(const std::exception& error) {
        Logger::error(error.what());
        return DRY_RUN_BLOCKED_STATUS;
    } catch(...) {
        Logger::error(localization::translate_message(
            "Dry-run preflight failed because of an unknown error."));
        return DRY_RUN_BLOCKED_STATUS;
    }
}
