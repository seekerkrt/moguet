#include "separated_package_base_source_build.hpp"

#include "localization.hpp"
#include "package_identifier.hpp"
#include "invocation_owned_cleanup_adapter.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#ifdef MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS
SeparatedPackageBaseSourceBuildWorkspaceObserverForTest
    g_workspace_observer = nullptr;

void notify_workspace_created_for_test(
    const std::filesystem::path& workspace_path) {
    if(g_workspace_observer != nullptr) g_workspace_observer(workspace_path);
}
#else
void notify_workspace_created_for_test(const std::filesystem::path&) {
}
#endif

void require_valid_set_request(
    const SeparatedPackageBaseSourceBuildRequest& request) {
    require_valid_package_name(request.package_base);
    if(request.required_targets.empty()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal Arch field name
            // "PackageBase".
            "Separated {} source-build requires at least one required package target.",
            "PackageBase"));
    }

    for(std::size_t index = 0; index < request.required_targets.size();
        ++index) {
        const RequiredPackageArtifactTarget& target =
            request.required_targets[index];
        require_valid_package_name(target.package_base);
        require_valid_package_name(target.package_name);
        if(target.package_base != request.package_base) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: Both placeholders are the literal Arch
                // field name "PackageBase".
                "Separated {} source-build required target has a mismatched {}.",
                "PackageBase", "PackageBase"));
        }
        if(std::any_of(
               request.required_targets.begin(),
               request.required_targets.begin() + index,
               [&target](
                   const RequiredPackageArtifactTarget& existing) {
                   return existing.package_name == target.package_name;
               })) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: The placeholder is the literal Arch field
                // name "PackageBase".
                "Separated {} source-build contains a duplicate required package target.",
                "PackageBase"));
        }
        switch(target.desired_reason) {
            case DesiredInstallReason::Explicit:
            case DesiredInstallReason::Dependency:
                break;
            default:
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch field
                    // name "PackageBase".
                    "Separated {} source-build has an unknown install reason.",
                    "PackageBase"));
        }
    }
}

ArtifactInstallExecutionOutcome execution_outcome(
    PackageBaseArtifactInstallExpectedOutcome outcome) noexcept {
    switch(outcome) {
        case PackageBaseArtifactInstallExpectedOutcome::Installed:
            return ArtifactInstallExecutionOutcome::Installed;
        case PackageBaseArtifactInstallExpectedOutcome::SkippedAsNeeded:
            return ArtifactInstallExecutionOutcome::SkippedAsNeeded;
    }
    // prepared/executor coherenceはunknown valueをtransaction前に拒否する。
    // 成功後promotionで到達するなら内部bugである。
    std::terminate();
}

struct PackageBaseSourceBuildResultDraft {
    std::vector<ArtifactPackageIdentity> unselected;
    // Transaction成功後のpromotionでallocationを起こさないための空capacity。
    std::vector<PackageBaseSourceBuildSelectedResult> promoted_selected;
    std::vector<ArtifactPackageIdentity> promoted_unselected;
};

PackageBaseSourceBuildResultDraft snapshot_result_draft(
    const PreparedPackageBaseArtifactInstall& prepared) {
    PackageBaseSourceBuildResultDraft draft;
    draft.promoted_selected.reserve(prepared.selected_artifacts().size());
    draft.unselected.reserve(prepared.unselected_artifacts().size());
    draft.promoted_unselected.reserve(
        prepared.unselected_artifacts().size());
    for(const auto& unselected : prepared.unselected_artifacts()) {
        draft.unselected.push_back(unselected.identity);
    }
    return draft;
}

void promote_successful_result(
    PackageBaseSourceBuildResultDraft& draft,
    std::vector<PackageBaseArtifactInstallExecutionArtifactResult>
        executed_selected) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<
                  ArtifactPackageIdentity>);
    static_assert(std::is_nothrow_move_constructible_v<
                  PackageBaseSourceBuildSelectedResult>);

    for(auto& selected : executed_selected) {
        draft.promoted_selected.push_back(
            PackageBaseSourceBuildSelectedResult{
                std::move(selected.identity),
                selected.desired_reason,
                execution_outcome(selected.outcome)});
    }
    for(auto& unselected : draft.unselected) {
        draft.promoted_unselected.push_back(std::move(unselected));
    }
}

std::string preparation_failure_diagnostic(
    const PackageBaseArtifactInstallPreparationFailure& failure) {
    if(failure.selection_failure() != nullptr) {
        return localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal Arch field name
            // "PackageBase".
            "{} artifact selection failed before package transaction.",
            "PackageBase");
    }
    if(failure.mixed_reason_failure() != nullptr) {
        return localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal Arch field name
            // "PackageBase".
            "{} artifact install reasons cannot be represented by one package transaction.",
            "PackageBase");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal Arch field name
        // "PackageBase".
        "Unknown {} artifact install preparation failure.",
        "PackageBase"));
}

} // namespace

PackageBaseSourceBuildExecutionResult::
    PackageBaseSourceBuildExecutionResult(
        std::string package_base,
        ProductionSourceBuildStagedOutcome production_outcome,
        std::vector<PackageBaseSourceBuildSelectedResult>
            selected_children,
        std::vector<ArtifactPackageIdentity> unselected_artifacts) noexcept
    : package_base_(std::move(package_base)),
      production_outcome_(std::move(production_outcome)),
      selected_children_(std::move(selected_children)),
      unselected_artifacts_(std::move(unselected_artifacts)) {
}

const std::string&
PackageBaseSourceBuildExecutionResult::package_base() const noexcept {
    return package_base_;
}

const ProductionSourceBuildStagedOutcome&
PackageBaseSourceBuildExecutionResult::production_outcome()
    const noexcept {
    return production_outcome_;
}

const ProductionSourceBuildProvenance&
PackageBaseSourceBuildExecutionResult::source_provenance() const noexcept {
    return production_outcome_.source_provenance;
}

ProductionSourceBuildCommandOutcome
PackageBaseSourceBuildExecutionResult::build_outcome() const noexcept {
    return production_outcome_.build_outcome;
}

ProductionSourceInstallOutcome
PackageBaseSourceBuildExecutionResult::install_outcome() const noexcept {
    return production_outcome_.install_outcome;
}

const std::vector<PackageBaseSourceBuildSelectedResult>&
PackageBaseSourceBuildExecutionResult::selected_children() const noexcept {
    return selected_children_;
}

const std::vector<ArtifactPackageIdentity>&
PackageBaseSourceBuildExecutionResult::unselected_artifacts() const noexcept {
    return unselected_artifacts_;
}

bool PackageBaseSourceBuildExecutionResult::installed_any() const noexcept {
    return std::any_of(
        selected_children_.begin(), selected_children_.end(),
        [](const PackageBaseSourceBuildSelectedResult& child) {
            return child.outcome ==
                   ArtifactInstallExecutionOutcome::Installed;
        });
}

bool PackageBaseSourceBuildExecutionResult::all_skipped_as_needed()
    const noexcept {
    return !selected_children_.empty() && std::all_of(
                                              selected_children_.begin(), selected_children_.end(),
                                              [](const PackageBaseSourceBuildSelectedResult& child) {
                                                  return child.outcome ==
                                                         ArtifactInstallExecutionOutcome::SkippedAsNeeded;
                                              });
}

std::string
PackageBaseSourceBuildExecutionResult::release_package_base() && noexcept {
    return std::move(package_base_);
}

std::vector<PackageBaseSourceBuildSelectedResult>
PackageBaseSourceBuildExecutionResult::release_selected_children() && noexcept {
    return std::move(selected_children_);
}

std::vector<ArtifactPackageIdentity>
PackageBaseSourceBuildExecutionResult::release_unselected_artifacts() && noexcept {
    return std::move(unselected_artifacts_);
}

SeparatedPackageBaseSourceBuildPhaseError::
    SeparatedPackageBaseSourceBuildPhaseError(
        SeparatedPackageBaseSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<ReviewedSourceProductionFailure>
            reviewed_source_failure,
        std::optional<PackageMetadataFailure>
            package_metadata_failure,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome,
        std::exception_ptr failure_exception)
    : std::runtime_error(diagnostic), phase_(phase),
      reviewed_source_failure_(std::move(reviewed_source_failure)),
      package_metadata_failure_(std::move(package_metadata_failure)),
      production_outcome_(std::move(production_outcome)),
      failure_exception_(std::move(failure_exception)) {
}

SeparatedPackageBaseSourceBuildFailurePhase
SeparatedPackageBaseSourceBuildPhaseError::phase() const noexcept {
    return phase_;
}

const std::optional<ReviewedSourceProductionFailure>&
SeparatedPackageBaseSourceBuildPhaseError::reviewed_source_failure()
    const noexcept {
    return reviewed_source_failure_;
}

const std::optional<PackageMetadataFailure>&
SeparatedPackageBaseSourceBuildPhaseError::package_metadata_failure()
    const noexcept {
    return package_metadata_failure_;
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPhaseError::production_outcome()
    const noexcept {
    return production_outcome_;
}

SeparatedPackageBaseSourceBuildPreparationError::
    SeparatedPackageBaseSourceBuildPreparationError(
        PackageBaseArtifactInstallPreparationFailure failure,
        const std::string& diagnostic,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome)
    : std::runtime_error(diagnostic), failure_(std::move(failure)),
      production_outcome_(std::move(production_outcome)) {
}

const PackageBaseArtifactInstallPreparationFailure&
SeparatedPackageBaseSourceBuildPreparationError::failure() const noexcept {
    return failure_;
}

const PackageBaseArtifactIdentitySelectionFailure*
SeparatedPackageBaseSourceBuildPreparationError::selection_failure()
    const noexcept {
    return failure_.selection_failure();
}

const MixedPackageBaseInstallReasonUnsupported*
SeparatedPackageBaseSourceBuildPreparationError::mixed_reason_failure()
    const noexcept {
    return failure_.mixed_reason_failure();
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPreparationError::production_outcome()
    const noexcept {
    return production_outcome_;
}

SeparatedPackageBaseSourceBuildCleanupError::
    SeparatedPackageBaseSourceBuildCleanupError(
        PackageBaseSourceBuildExecutionResult result,
        const std::string& diagnostic)
    : std::runtime_error(diagnostic), result_(std::move(result)) {
}

const PackageBaseSourceBuildExecutionResult&
SeparatedPackageBaseSourceBuildCleanupError::result() const noexcept {
    return result_;
}

PackageBaseSourceBuildExecutionResult
SeparatedPackageBaseSourceBuildCleanupError::release_result() && noexcept {
    return std::move(result_);
}

class SeparatedPackageBaseSourceBuildExecutionOwner final {
public:
    using InstallExecutor = PackageBaseArtifactInstallExecutionResult (*)(
        PreparedPackageBaseArtifactInstall& install,
        const ArtifactInstallExecutionOptions& options,
        void* context);

    static PackageBaseSourceBuildExecutionResult execute(
        SeparatedPackageBaseSourceBuildRequest request,
        const SeparatedSourceBuildUnitOptions& options,
        InstallExecutor install_executor,
        void* install_context);
};

namespace {

PackageBaseArtifactInstallExecutionResult execute_legacy_install(
    PreparedPackageBaseArtifactInstall& install,
    const ArtifactInstallExecutionOptions& options,
    void*) {
    return execute_prepared_package_base_artifact_install(install, options);
}

struct CleanupInstallContext {
    RemoteAurCleanupCandidateCollector* collector;
    std::size_t work_item_index;
};

PackageBaseArtifactInstallExecutionResult execute_cleanup_authority_install(
    PreparedPackageBaseArtifactInstall& install,
    const ArtifactInstallExecutionOptions& options,
    void* raw_context) {
    auto* context = static_cast<CleanupInstallContext*>(raw_context);
    if(context == nullptr || context->collector == nullptr) {
        throw std::logic_error(
            "Remote AUR cleanup source-artifact execution has no work-item attribution.");
    }
    return context->collector->execute_source_artifact_install_transaction(
        install, context->work_item_index, options);
}

} // namespace

PackageBaseSourceBuildExecutionResult
SeparatedPackageBaseSourceBuildExecutionOwner::execute(
    SeparatedPackageBaseSourceBuildRequest request,
    const SeparatedSourceBuildUnitOptions& options,
    InstallExecutor install_executor,
    void* install_context) {
    // POLICY(#268): caller-controlled policy/identityを最初のworkspace mutation前に
    // 全件確定し、duplicate/mismatch/unknown reasonをselectionまで遅らせない。
    require_supported_separated_install_options(options.rm_deps);
    require_unclaimed_artifact_pkgdest(request.source_environment);
    require_valid_set_request(request);

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
        } catch(const ReviewedSourceProductionError& error) {
            workspace.retain_for_diagnostics();
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                error.what(), error.failure(), std::nullopt,
                production_outcome);
        } catch(...) {
            workspace.retain_for_diagnostics();
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch
                    // field name "PackageBase".
                    "{} source-build context preparation failed.",
                    "PackageBase"),
                std::nullopt, std::nullopt, production_outcome);
        }
    }();
    ExpectedPackageArtifactSet expected = [&]() {
        try {
            return query_makepkg_packagelist_set(workspace, makepkg_context);
        } catch(const ReviewedSourceProductionError& error) {
            workspace.retain_for_diagnostics();
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                error.what(), error.failure(), std::nullopt,
                production_outcome);
        } catch(...) {
            workspace.retain_for_diagnostics();
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::Build,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // Arch field name "PackageBase" and command name
                    // "makepkg".
                    "{} {} artifact-list query failed.",
                    "PackageBase", "makepkg"),
                std::nullopt, std::nullopt, production_outcome);
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
        // Only an acquired zero status advances the staged build dimension;
        // pre-command and unknown-status failures remain Started.
        if(error.command_exit_status() == 0) {
            production_outcome.build_outcome =
                ProductionSourceBuildCommandOutcome::Succeeded;
        }
        throw SeparatedPackageBaseSourceBuildPhaseError(
            SeparatedPackageBaseSourceBuildFailurePhase::Build,
            error.what(), std::nullopt, std::nullopt,
            production_outcome, std::current_exception());
    } catch(const ReviewedSourceProductionError& error) {
        workspace.retain_for_diagnostics();
        throw SeparatedPackageBaseSourceBuildPhaseError(
            SeparatedPackageBaseSourceBuildFailurePhase::Build,
            error.what(), error.failure(), std::nullopt,
            production_outcome);
    } catch(...) {
        workspace.retain_for_diagnostics();
        throw SeparatedPackageBaseSourceBuildPhaseError(
            SeparatedPackageBaseSourceBuildFailurePhase::Build,
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are the literal Arch
                // field name "PackageBase" and command name "makepkg".
                "The {} build-only {} execution failed.",
                "PackageBase", "makepkg"),
            std::nullopt, std::nullopt, production_outcome);
    }

    // LANDMINE(#268): command statusやaggregate validationより先にretainする。
    // build phaseへ入った後の全failureはfresh workspaceを診断用に残す。
    workspace.retain_for_diagnostics();
    if(build_exit_code != 0) {
        production_outcome.build_outcome =
            ProductionSourceBuildCommandOutcome::Failed;
        throw SeparatedPackageBaseSourceBuildPhaseError(
            SeparatedPackageBaseSourceBuildFailurePhase::Build,
            localization::format_translated_message(
                // TRANSLATORS: The first placeholder is the literal
                // command name "makepkg"; the second is its exit code.
                "The build-only {} command failed with exit code {}.",
                "makepkg", build_exit_code),
            std::nullopt, std::nullopt, production_outcome);
    }
    production_outcome.build_outcome =
        ProductionSourceBuildCommandOutcome::Succeeded;

    ValidatedPackageArtifactSet artifacts = [&]() {
        try {
            return validate_post_build_package_artifacts(
                std::move(workspace), expected);
        } catch(...) {
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::
                    ArtifactValidation,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch
                    // field name "PackageBase".
                    "{} artifact aggregate validation failed.",
                    "PackageBase"),
                std::nullopt, std::nullopt, production_outcome);
        }
    }();
    production_outcome.install_outcome =
        ProductionSourceInstallOutcome::Started;
    PackageBaseArtifactInstallPreparationResult preparation = [&]() {
        try {
            return prepare_package_base_artifact_install(
                artifacts, request.package_base,
                request.required_targets,
                ArtifactInstallPreparationOptions{
                    options.needed, options.rm_deps},
                request.database_paths);
        } catch(const PackageMetadataError& error) {
            artifacts.retain_workspace_for_diagnostics();
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::
                    InstallPreparation,
                error.what(), std::nullopt, error.failure(),
                production_outcome);
        } catch(...) {
            artifacts.retain_workspace_for_diagnostics();
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::
                    ArtifactIdentity,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch
                    // field name "PackageBase".
                    "{} artifact identity or validation failed before install preparation.",
                    "PackageBase"),
                std::nullopt, std::nullopt, production_outcome);
        }
    }();
    if(!preparation.is_prepared()) {
        if(preparation.prepared() != nullptr ||
           preparation.failure() == nullptr) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::
                    InstallPreparation,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch field
                    // name "PackageBase".
                    "{} artifact install preparation result is incoherent.",
                    "PackageBase"),
                std::nullopt, std::nullopt, production_outcome);
        }
        artifacts.retain_workspace_for_diagnostics();
        PackageBaseArtifactInstallPreparationFailure failure =
            *preparation.failure();
        const std::string diagnostic =
            preparation_failure_diagnostic(failure);
        production_outcome.install_outcome =
            ProductionSourceInstallOutcome::Failed;
        throw SeparatedPackageBaseSourceBuildPreparationError(
            std::move(failure), diagnostic, production_outcome);
    }
    if(preparation.prepared() == nullptr ||
       preparation.failure() != nullptr) {
        production_outcome.install_outcome =
            ProductionSourceInstallOutcome::Failed;
        throw SeparatedPackageBaseSourceBuildPhaseError(
            SeparatedPackageBaseSourceBuildFailurePhase::
                InstallPreparation,
            localization::format_translated_message(
                // TRANSLATORS: The placeholder is the literal Arch field name
                // "PackageBase".
                "{} artifact install preparation result is incoherent.",
                "PackageBase"),
            std::nullopt, std::nullopt, production_outcome);
    }

    PreparedPackageBaseArtifactInstall& prepared =
        *preparation.prepared();
    // Transaction前にはprivate draftとpromotion用capacityだけを確定する。
    // ordinary failureではpublic child success resultを一件も生成しない。
    PackageBaseSourceBuildResultDraft result_draft = [&]() {
        try {
            return snapshot_result_draft(prepared);
        } catch(...) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::
                    InstallPreparation,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch field name
                    // "PackageBase".
                    "{} install-result preparation failed before package transaction.",
                    "PackageBase"),
                std::nullopt, std::nullopt, production_outcome);
        }
    }();
    PackageBaseArtifactInstallExecutionResult execution_result = [&]() {
        try {
            if(install_executor == nullptr) {
                throw std::logic_error(
                    "PackageBase source-build install executor is unavailable.");
            }
            return install_executor(
                prepared,
                ArtifactInstallExecutionOptions{options.no_confirm},
                install_context);
        } catch(PackageBaseArtifactInstallTransactionError& error) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            error.attach_production_outcome(production_outcome);
            throw;
        } catch(...) {
            production_outcome.install_outcome =
                ProductionSourceInstallOutcome::Failed;
            throw SeparatedPackageBaseSourceBuildPhaseError(
                SeparatedPackageBaseSourceBuildFailurePhase::
                    InstallTransaction,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal Arch
                    // field name "PackageBase".
                    "Prepared {} artifact install failed validation before transaction.",
                    "PackageBase"),
                std::nullopt, std::nullopt, production_outcome);
        }
    }();
    production_outcome.install_outcome =
        ProductionSourceInstallOutcome::Succeeded;

    // Transaction成功後はallocationなしのmoveだけでpublic resultへ昇格する。
    std::string executed_package_base =
        std::move(execution_result).release_package_base();
    std::vector<PackageBaseArtifactInstallExecutionArtifactResult>
        executed_selected =
            std::move(execution_result).release_selected_artifacts();
    promote_successful_result(
        result_draft, std::move(executed_selected));
    PackageBaseSourceBuildExecutionResult result(
        std::move(executed_package_base),
        production_outcome,
        std::move(result_draft.promoted_selected),
        std::move(result_draft.promoted_unselected));

    try {
        // POLICY(#268): transaction成功後だけaggregate cleanupを明示実行する。
        prepared.cleanup_workspace();
    } catch(const std::exception& error) {
        throw SeparatedPackageBaseSourceBuildCleanupError(
            std::move(result),
            localization::format_translated_message(
                "Package installation succeeded, but artifact workspace cleanup failed: {}",
                error.what()));
    } catch(...) {
        throw SeparatedPackageBaseSourceBuildCleanupError(
            std::move(result),
            localization::translate_message(
                "Package installation succeeded, but artifact workspace cleanup failed with an unknown error."));
    }
    return result;
}

PackageBaseSourceBuildExecutionResult
execute_separated_package_base_source_build(
    SeparatedPackageBaseSourceBuildRequest request,
    const SeparatedSourceBuildUnitOptions& options) {
    return SeparatedPackageBaseSourceBuildExecutionOwner::execute(
        std::move(request), options, execute_legacy_install, nullptr);
}

PackageBaseSourceBuildExecutionResult
execute_separated_package_base_source_build_with_cleanup_authority(
    SeparatedPackageBaseSourceBuildRequest request,
    const SeparatedSourceBuildUnitOptions& options,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index) {
    CleanupInstallContext context{&collector, work_item_index};
    return SeparatedPackageBaseSourceBuildExecutionOwner::execute(
        std::move(request), options, execute_cleanup_authority_install,
        &context);
}

#ifdef MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS
void set_separated_package_base_source_build_workspace_observer_for_test(
    SeparatedPackageBaseSourceBuildWorkspaceObserverForTest observer) {
    g_workspace_observer = observer;
}
#endif
