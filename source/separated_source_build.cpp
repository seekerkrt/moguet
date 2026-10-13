#include "separated_source_build.hpp"

#include "localization.hpp"
#include "reviewed_source_production_failure.hpp"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

#ifdef MOGUET_ENABLE_SEPARATED_SOURCE_BUILD_TEST_HOOKS
SeparatedSourceBuildWorkspaceObserverForTest g_workspace_observer = nullptr;

void notify_workspace_created_for_test(
    const std::filesystem::path& workspace_path) {
    if(g_workspace_observer != nullptr) g_workspace_observer(workspace_path);
}
#else
void notify_workspace_created_for_test(const std::filesystem::path&) {
}
#endif

} // namespace

SeparatedSourceBuildPhaseError::SeparatedSourceBuildPhaseError(
    SeparatedSourceBuildFailurePhase phase,
    ProductionSourceBuildStagedOutcome production_outcome,
    const std::string& diagnostic,
    std::optional<PackageMetadataFailure> package_metadata_failure,
    std::exception_ptr failure_exception)
    : std::runtime_error(diagnostic), phase_(phase),
      production_outcome_(std::move(production_outcome)),
      package_metadata_failure_(std::move(package_metadata_failure)),
      failure_exception_(std::move(failure_exception)) {
}

SeparatedSourceBuildCleanupError::SeparatedSourceBuildCleanupError(
    ArtifactInstallExecutionOutcome install_outcome,
    ProductionSourceBuildStagedOutcome production_outcome,
    const std::string& diagnostic)
    : std::runtime_error(diagnostic),
      install_outcome_(install_outcome),
      production_outcome_(std::move(production_outcome)) {
}

SeparatedSourceBuildExecutionResult execute_separated_source_build_unit(
    SeparatedSourceBuildUnitRequest request,
    const SeparatedSourceBuildUnitOptions& options) {
    // POLICY(#242): build unit最初のmutationより前に、caller-controlled optionと
    // PKGDEST ownership conflictをempty valueも含めて確定する。
    require_supported_separated_install_options(options.rm_deps);
    require_unclaimed_artifact_pkgdest(request.source_environment);

    ProductionSourceBuildStagedOutcome production_outcome{
        .source_provenance = request.source_tree.provenance()};
    ArtifactWorkspace workspace = create_artifact_workspace(
        std::move(request.artifact_root));
    notify_workspace_created_for_test(workspace.path());

    ArtifactMakepkgContext makepkg_context = [&]() {
        try {
            return prepare_artifact_makepkg_context(
                std::move(request.source_tree), workspace,
                request.source_environment,
                request.empty_value_policy);
        } catch(ReviewedSourceProductionError& error) {
            if(!error.production_outcome().has_value()) {
                error.attach_production_outcome(production_outcome);
            }
            throw;
        } catch(const std::exception& error) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::Build,
                production_outcome, error.what(), std::nullopt,
                std::current_exception());
        } catch(...) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::Build,
                production_outcome,
                localization::translate_message(
                    "Source-build context preparation failed with an unknown error."),
                std::nullopt, std::current_exception());
        }
    }();
    ExpectedPackageArtifactPath expected = [&]() {
        try {
            return query_makepkg_packagelist(
                workspace, makepkg_context);
        } catch(ReviewedSourceProductionError& error) {
            if(!error.production_outcome().has_value()) {
                error.attach_production_outcome(production_outcome);
            }
            throw;
        } catch(const std::exception& error) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::Build,
                production_outcome, error.what(), std::nullopt,
                std::current_exception());
        } catch(...) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::Build,
                production_outcome,
                localization::translate_message(
                    "Source-build artifact-list query failed with an unknown error."),
                std::nullopt, std::current_exception());
        }
    }();

    const ArtifactMakepkgBuildOptions makepkg_options{
        .no_confirm = options.no_confirm,
        .rebuild = options.rebuild,
        .clean_build = options.clean_build,
    };
    int build_exit_code = 0;
    production_outcome.build_outcome =
        ProductionSourceBuildCommandOutcome::Started;
    try {
        build_exit_code = makepkg_context.run_makepkg_build_only(
            workspace, expected, makepkg_options);
    } catch(const ProductionSourceBuildPostCommandRevalidationError& error) {
        workspace.retain_for_diagnostics();
        // The typed transport exists only after the child status was
        // acquired. Do not infer success from an untyped/unknown failure.
        if(error.command_exit_status() == 0) {
            production_outcome.build_outcome =
                ProductionSourceBuildCommandOutcome::Succeeded;
        }
        throw SeparatedSourceBuildPhaseError(
            SeparatedSourceBuildFailurePhase::Build,
            production_outcome, error.what(), std::nullopt,
            std::current_exception());
    } catch(ReviewedSourceProductionError& error) {
        // run_makepkg_build_only()はcommand前後の再検証でもthrowし得る。
        // build phaseへ入った後のfailureはdiagnostic workspaceを残す。
        workspace.retain_for_diagnostics();
        if(!error.production_outcome().has_value()) {
            error.attach_production_outcome(production_outcome);
        }
        throw;
    } catch(const std::exception& error) {
        workspace.retain_for_diagnostics();
        throw SeparatedSourceBuildPhaseError(
            SeparatedSourceBuildFailurePhase::Build,
            production_outcome, error.what(), std::nullopt,
            std::current_exception());
    } catch(...) {
        workspace.retain_for_diagnostics();
        throw SeparatedSourceBuildPhaseError(
            SeparatedSourceBuildFailurePhase::Build,
            production_outcome,
            localization::format_translated_message(
                // TRANSLATORS: The placeholder is the literal command
                // name "makepkg".
                "The build-only {} execution failed with an unknown error.",
                "makepkg"),
            std::nullopt, std::current_exception());
    }

    // LANDMINE(#242): status判定やfilesystem validationより先に保持へ遷移する。
    // ここから先のfailureは、生成済みartifactを自動cleanupしてはならない。
    workspace.retain_for_diagnostics();
    if(build_exit_code != 0) {
        production_outcome.build_outcome =
            ProductionSourceBuildCommandOutcome::Failed;
        throw SeparatedSourceBuildPhaseError(
            SeparatedSourceBuildFailurePhase::Build,
            production_outcome,
            localization::format_translated_message(
                // TRANSLATORS: The first placeholder is the literal command
                // name "makepkg"; the second is its numeric exit code.
                "The build-only {} command failed with exit code {}.",
                "makepkg", build_exit_code));
    }
    production_outcome.build_outcome =
        ProductionSourceBuildCommandOutcome::Succeeded;

    ValidatedPackageArtifactPath artifact = [&]() {
        try {
            return validate_post_build_package_artifact(
                std::move(workspace), expected);
        } catch(const PackageMetadataError& error) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::ArtifactValidation,
                production_outcome, error.what(), error.failure(),
                std::current_exception());
        } catch(const std::exception& error) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::ArtifactValidation,
                production_outcome, error.what(), std::nullopt,
                std::current_exception());
        } catch(...) {
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::ArtifactValidation,
                production_outcome,
                localization::translate_message(
                    "Built package artifact validation failed with an unknown error."),
                std::nullopt, std::current_exception());
        }
    }();
    production_outcome.install_outcome =
        ProductionSourceInstallOutcome::Started;
    PreparedArtifactInstall prepared = [&]() {
        try {
            return prepare_artifact_install(
                artifact, request.requested_name,
                request.package_base, request.desired_reason,
                ArtifactInstallPreparationOptions{
                    options.needed, options.rm_deps},
                request.database_paths);
        } catch(const PackageMetadataError& error) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::InstallPreparation,
                production_outcome, error.what(), error.failure(),
                std::current_exception());
        } catch(const std::exception& error) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::InstallPreparation,
                production_outcome, error.what(), std::nullopt,
                std::current_exception());
        } catch(...) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::InstallPreparation,
                production_outcome,
                localization::translate_message(
                    "Package install preparation failed with an unknown error."),
                std::nullopt, std::current_exception());
        }
    }();
    const ArtifactInstallExecutionOutcome install_outcome = [&]() {
        try {
            return execute_prepared_artifact_install(
                prepared,
                ArtifactInstallExecutionOptions{options.no_confirm});
        } catch(const std::exception& error) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::InstallTransaction,
                production_outcome, error.what(), std::nullopt,
                std::current_exception());
        } catch(...) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedSourceBuildPhaseError(
                SeparatedSourceBuildFailurePhase::InstallTransaction,
                production_outcome,
                localization::translate_message(
                    "Package install transaction failed with an unknown error."),
                std::nullopt, std::current_exception());
        }
    }();
    production_outcome.install_outcome =
        ProductionSourceInstallOutcome::Succeeded;

    try {
        // POLICY(#242): transaction成功後だけcleanup ownershipを明示的に行使する。
        prepared.cleanup_workspace();
    } catch(const std::exception& error) {
        throw SeparatedSourceBuildCleanupError(
            install_outcome, production_outcome,
            localization::format_translated_message(
                "Package installation succeeded, but artifact workspace cleanup failed: {}",
                error.what()));
    } catch(...) {
        throw SeparatedSourceBuildCleanupError(
            install_outcome, production_outcome,
            localization::translate_message(
                "Package installation succeeded, but artifact workspace cleanup failed with an unknown error."));
    }
    return SeparatedSourceBuildExecutionResult{
        install_outcome, production_outcome};
}

#ifdef MOGUET_ENABLE_SEPARATED_SOURCE_BUILD_TEST_HOOKS
void set_separated_source_build_workspace_observer_for_test(
    SeparatedSourceBuildWorkspaceObserverForTest observer) {
    g_workspace_observer = observer;
}
#endif
