#include "commands_aur_update.hpp"

#include "app_config.hpp"
#include "aur_update_cli_presentation.hpp"
#include "aur_update_execution_preflight.hpp"
#include "filtered_aur_update_operation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "runtime_diagnostic.hpp"
#include "source_install.hpp"

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string operation_status_label(AurUpdateOperationStatus status) {
    switch(status) {
        case AurUpdateOperationStatus::NoUpdates:
            return localization::translate_message("no updates");
        case AurUpdateOperationStatus::StoppedOnWorkItemCancellation:
            return localization::translate_message("Cancelled");
        case AurUpdateOperationStatus::Completed:
            return localization::translate_message("completed");
        case AurUpdateOperationStatus::BlockedBeforeExecution:
            return localization::translate_message("blocked before execution");
        case AurUpdateOperationStatus::StoppedOnProviderTransactionFailure:
            return localization::translate_message(
                "stopped after repository provider transaction failure");
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
            return localization::translate_message(
                "stopped after work-item failure");
        case AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure:
            return localization::translate_message(
                "stopped after cleanup failure");
        case AurUpdateOperationStatus::InconsistentResult:
            return localization::translate_message("inconsistent result");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} update operation status.", "AUR"));
}

} // namespace

std::string aur_update_preflight_reason_label(
    AurUpdateExecutionReason reason) {
    switch(reason) {
        case AurUpdateExecutionReason::None:
            return localization::translate_message("none");
        case AurUpdateExecutionReason::UpToDate:
            return localization::translate_message("up to date");
        case AurUpdateExecutionReason::DevelObservationUnknown:
            return localization::format_translated_message(
                // TRANSLATORS: The placeholder is the literal tool name "Git".
                "devel {} observation failed", "Git");
        case AurUpdateExecutionReason::DevelUnsupported:
            return localization::translate_message("devel automatic update unsupported");
        case AurUpdateExecutionReason::DevelRequiresCheck:
            return localization::translate_message(
                "devel update requires check");
        case AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck:
            return localization::translate_message(
                "devel update requires check");
        case AurUpdateExecutionReason::NonAurForeign:
            return localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                // TRANSLATORS: The placeholder is the literal service name "AUR".
                "non-{} foreign", "AUR");
        case AurUpdateExecutionReason::AurMetadataUnavailable:
            return localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "{} metadata unavailable", "AUR");
        case AurUpdateExecutionReason::VersionComparisonUnavailable:
            return localization::translate_message(
                "version comparison unavailable");
        case AurUpdateExecutionReason::InstalledReasonUnknown:
            return localization::translate_message("installed reason unknown");
        case AurUpdateExecutionReason::UpdatePlanInconsistent:
            return localization::translate_message("update plan inconsistent");
        case AurUpdateExecutionReason::DuplicateUpdateTarget:
            return localization::translate_message("duplicate update target");
        case AurUpdateExecutionReason::InstalledPackageMetadataUnavailable:
            return localization::translate_message(
                "installed package metadata unavailable");
        case AurUpdateExecutionReason::RepositoryMetadataUnavailable:
            return localization::translate_message(
                "repository metadata unavailable");
        case AurUpdateExecutionReason::AurDependencyMetadataUnavailable:
            return localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "{} dependency metadata unavailable", "AUR");
        case AurUpdateExecutionReason::ProviderMetadataUnavailable:
            return localization::translate_message(
                "provider metadata unavailable");
        case AurUpdateExecutionReason::UnresolvedDependency:
            return localization::translate_message("unresolved dependency");
        case AurUpdateExecutionReason::VersionConstraintUnverified:
            return localization::translate_message(
                "version constraint unverified");
        case AurUpdateExecutionReason::DependencyCycle:
            return localization::translate_message("dependency cycle");
        case AurUpdateExecutionReason::BuildPlanInconsistent:
            return localization::translate_message("build plan inconsistent");
        case AurUpdateExecutionReason::PackageBaseMismatch:
            return localization::translate_message("package base mismatch");
        case AurUpdateExecutionReason::SplitPackageSelectionRequired:
            return localization::translate_message(
                "split package selection required");
        case AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase:
            return localization::translate_message(
                "multiple package targets for package base");
        case AurUpdateExecutionReason::AmbiguousProvider:
            return localization::translate_message("ambiguous provider");
        case AurUpdateExecutionReason::ConflictsOrReplacesUnresolved:
            return localization::translate_message(
                "conflicts/replaces unresolved");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown {} update preflight reason.", "AUR"));
}

std::string aur_update_preparation_reason_label(
    AurUpdatePreparationReason reason) {
    switch(reason) {
        case AurUpdatePreparationReason::None:
            return localization::translate_message("none");
        case AurUpdatePreparationReason::BlockingPreflight:
            return localization::translate_message("blocking preflight");
        case AurUpdatePreparationReason::PreflightInconsistent:
            return localization::translate_message("preflight inconsistent");
        case AurUpdatePreparationReason::
            DevelRequiresCheckPolicyInconsistent:
            return localization::translate_message("preflight inconsistent");
        case AurUpdatePreparationReason::BuildPlanMissing:
            return localization::translate_message("build plan missing");
        case AurUpdatePreparationReason::BuildPlanOrderEmpty:
            return localization::translate_message("build plan order empty");
        case AurUpdatePreparationReason::RootAttributionInconsistent:
            return localization::translate_message(
                "root attribution inconsistent");
        case AurUpdatePreparationReason::PackageTargetAttributionInconsistent:
            return localization::translate_message(
                "package target attribution inconsistent");
        case AurUpdatePreparationReason::DesiredInstallReasonMissing:
            return localization::translate_message(
                "desired install reason missing");
        case AurUpdatePreparationReason::SourcePreferenceUnavailable:
            return localization::translate_message(
                "source preference unavailable");
        case AurUpdatePreparationReason::SourcePreferencePkgdestConflict:
            return localization::format_translated_message(
                // TRANSLATORS: PKGDEST is a runtime environment-key identity.
                "source preference {} conflict", "PKGDEST");
        case AurUpdatePreparationReason::StaticWorkItemInvalid:
            return localization::translate_message("static work item invalid");
        case AurUpdatePreparationReason::PacmanDatabaseUnavailable:
            return localization::format_translated_message(
                // TRANSLATORS: pacman is a runtime command identity.
                "{} database unavailable", "pacman");
        case AurUpdatePreparationReason::GenericPreparationInconsistent:
            return localization::translate_message(
                "generic preparation inconsistent");
        case AurUpdatePreparationReason::BuildUnitSelectionInconsistent:
            return localization::translate_message(
                "build unit selection inconsistent");
        case AurUpdatePreparationReason::ExternalSatisfactionInconsistent:
            return localization::translate_message(
                "external satisfaction inconsistent");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown {} update preparation reason.", "AUR"));
}

namespace {

std::string reduction_stage_label(AurUpdateOperationReductionStage stage) {
    switch(stage) {
        case AurUpdateOperationReductionStage::Preflight:
            return localization::translate_message("preflight");
        case AurUpdateOperationReductionStage::Preparation:
            return localization::translate_message("preparation");
        case AurUpdateOperationReductionStage::Execution:
            return localization::translate_message("execution");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} update reduction stage.", "AUR"));
}

std::string reduction_reason_label(AurUpdateOperationReductionReason reason) {
    switch(reason) {
        case AurUpdateOperationReductionReason::DuplicatePreflightUpdatePlanIndex:
            return localization::translate_message("duplicate preflight update plan index");
        case AurUpdateOperationReductionReason::OutOfRangePreflightUpdatePlanIndex:
            return localization::translate_message("out-of-range preflight update plan index");
        case AurUpdateOperationReductionReason::PreflightTargetOrderInconsistent:
            return localization::translate_message("preflight target order inconsistent");
        case AurUpdateOperationReductionReason::DuplicatePreparationAttribution:
            return localization::translate_message("duplicate preparation attribution");
        case AurUpdateOperationReductionReason::UnknownPreparationUpdatePlanIndex:
            return localization::translate_message("unknown preparation update plan index");
        case AurUpdateOperationReductionReason::PreparationAttributionInconsistent:
            return localization::translate_message("preparation attribution inconsistent");
        case AurUpdateOperationReductionReason::PreparationTargetSnapshotInconsistent:
            return localization::translate_message("preparation target snapshot inconsistent");
        case AurUpdateOperationReductionReason::DuplicateExecutionWorkItemIndex:
            return localization::translate_message("duplicate execution work item index");
        case AurUpdateOperationReductionReason::ExecutionWorkItemOrderInconsistent:
            return localization::translate_message("execution work item order inconsistent");
        case AurUpdateOperationReductionReason::DuplicateExecutionAttribution:
            return localization::translate_message("duplicate execution attribution");
        case AurUpdateOperationReductionReason::UnknownExecutionUpdatePlanIndex:
            return localization::translate_message("unknown execution update plan index");
        case AurUpdateOperationReductionReason::MissingExecutionAttribution:
            return localization::translate_message("missing execution attribution");
        case AurUpdateOperationReductionReason::
            DuplicateExecutionChildAttribution:
            return localization::translate_message("duplicate execution child attribution");
        case AurUpdateOperationReductionReason::
            MissingExecutionChildAttribution:
            return localization::translate_message("missing execution child attribution");
        case AurUpdateOperationReductionReason::
            UnexpectedExecutionChildAttribution:
            return localization::translate_message("unexpected execution child attribution");
        case AurUpdateOperationReductionReason::
            UnknownExecutionChildUpdatePlanIndex:
            return localization::translate_message("unknown execution child update plan index");
        case AurUpdateOperationReductionReason::
            ExecutionChildSnapshotInconsistent:
            return localization::translate_message("execution child snapshot inconsistent");
        case AurUpdateOperationReductionReason::UnexpectedSelectedArtifact:
            return localization::translate_message("unexpected selected artifact");
        case AurUpdateOperationReductionReason::
            UnexpectedUnselectedArtifactIdentity:
            return localization::translate_message("unexpected unselected artifact identity");
        case AurUpdateOperationReductionReason::ExecutionResultWithPreparationIssues:
            return localization::translate_message("execution result with preparation issues");
        case AurUpdateOperationReductionReason::MissingExecutionResult:
            return localization::translate_message("missing execution result");
        case AurUpdateOperationReductionReason::UnknownEnumValue:
            return localization::translate_message("unknown enum value");
        case AurUpdateOperationReductionReason::WorkItemResultInconsistent:
            return localization::translate_message("work item result inconsistent");
        case AurUpdateOperationReductionReason::InvocationResultInconsistent:
            return localization::translate_message("invocation result inconsistent");
        case AurUpdateOperationReductionReason::
            DevelRequiresCheckPolicyInconsistent:
        case AurUpdateOperationReductionReason::OtherCorrelationInconsistent:
            return localization::translate_message("other correlation inconsistency");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} update reduction reason.", "AUR"));
}

std::string target_reason_label(const AurUpdateOperationTargetResult& target) {
    if(!target.preflight_issues.empty()) {
        const AurUpdateExecutionIssue& issue =
            target.preflight_issues.front();
        if(issue.reason == AurUpdateExecutionReason::DevelRequiresCheck &&
           issue.devel_requires_check_reason ==
               DevelRequiresCheckReason::SuffixCandidateOnly) {
            return localization::translate_message(
                "devel update requires check: suffix candidate only");
        }
        return std::string(aur_update_preflight_reason_label(issue.reason));
    }
    if(!target.preparation_issues.empty()) {
        return std::string(aur_update_preparation_reason_label(
            target.preparation_issues.front().reason));
    }
    return localization::translate_message("reason unavailable");
}

std::string target_status_label(
    const AurUpdateOperationTargetResult& target,
    AurUpdateOperationStatus operation_status) {
    switch(target.status) {
        case AurUpdateOperationTargetStatus::Updated:
            if(has_aur_update_bootstrap_intent(target.update)) return localization::translate_message("devel tracking baseline established");
            return localization::translate_message("updated");
        case AurUpdateOperationTargetStatus::NoChange:
            return localization::translate_message("no change");
        case AurUpdateOperationTargetStatus::Skipped:
            return localization::translate_message("skipped") + ": " +
                   target_reason_label(target);
        case AurUpdateOperationTargetStatus::Unsupported:
            return localization::translate_message("unsupported") + ": " +
                   target_reason_label(target);
        case AurUpdateOperationTargetStatus::Incomplete:
            if(!target.preflight_issues.empty() &&
               target.preflight_issues.front().reason ==
                   AurUpdateExecutionReason::DevelRequiresCheck) {
                return target_reason_label(target);
            }
            return localization::translate_message("incomplete") + ": " +
                   target_reason_label(target);
        case AurUpdateOperationTargetStatus::Cancelled:
            return localization::translate_message("Cancelled");
        case AurUpdateOperationTargetStatus::Failed:
            return localization::translate_message("failed") + ": " +
                   aur_update_cli_target_failure_summary(target);
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
            return localization::translate_message("updated, but cleanup failed");
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
            return localization::translate_message("no package change, but cleanup failed");
        case AurUpdateOperationTargetStatus::NotAttempted:
            if(operation_status == AurUpdateOperationStatus::
                                       StoppedOnProviderTransactionFailure) {
                return localization::translate_message(
                    "not attempted: repository provider transaction failed");
            }
            if(target.execution_failure_kind ==
               AurUpdateWorkItemFailureKind::PriorWorkItemStopped) {
                return localization::translate_message("not attempted: prior work item stopped");
            }
            if(operation_status ==
               AurUpdateOperationStatus::BlockedBeforeExecution) {
                return localization::translate_message("not attempted: operation blocked before execution");
            }
            if(operation_status == AurUpdateOperationStatus::InconsistentResult) {
                return localization::translate_message("not attempted: result inconsistent");
            }
            return localization::translate_message("not attempted: prior work item stopped");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} update target status.", "AUR"));
}

const AurUpdateExecutionIssue*
independent_requires_check_attention_issue(
    const AurUpdateOperationResult& result,
    const AurUpdateOperationTargetResult& target) noexcept {
    if(result.devel_requires_check_policy !=
           std::optional<DevelRequiresCheckPolicy>{
               DevelRequiresCheckPolicy::SkipIndependentTarget} ||
       target.status != AurUpdateOperationTargetStatus::Skipped ||
       !is_valid_aur_update_independent_devel_skip_snapshot(
           target.update, target.preflight_issues,
           target.skip_kind,
           DevelRequiresCheckPolicy::SkipIndependentTarget)) {
        return nullptr;
    }
    return &target.preflight_issues.front();
}

std::string independent_requires_check_attention_message(
    DevelRequiresCheckReason reason) {
    if(reason == DevelRequiresCheckReason::SuffixCandidateOnly) {
        return localization::translate_message(
            "skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable");
    }
    if(is_known_devel_requires_check_reason(reason)) {
        return localization::translate_message(
            "skipped: devel update requires check; local authority is unavailable or has changed");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown devel RequiresCheck attention reason."));
}

void report_independent_requires_check_attention(
    const AurUpdateOperationTargetResult& target,
    const AurUpdateExecutionIssue& issue) {
    DiagnosticIdentity identity;
    identity.source_kind = DiagnosticSourceKind::Aur;
    identity.requested_package = target.update.installed_name;
    identity.package_base = target.update.aur_package->package_base;
    const NormalizedDiagnostic<AurUpdateExecutionSkipKind> diagnostic{
        DiagnosticClass::RequiresCheck,
        DiagnosticSeverity::Warning,
        DiagnosticOperation::PacmanDelegation,
        DiagnosticPhase::Preflight,
        std::move(identity),
        *target.skip_kind,
        DiagnosticRequiredAction::InspectMetadata,
        DiagnosticBlockingDecision::NonBlocking,
        DiagnosticExitStatusEffect::SuccessPermitted,
        std::nullopt};
    report_runtime_diagnostic(
        diagnostic,
        independent_requires_check_attention_message(
            *issue.devel_requires_check_reason));
}

} // namespace

bool is_routine_aur_update_skip(AurUpdateExecutionReason reason) {
    switch(reason) {
        case AurUpdateExecutionReason::UpToDate:
        case AurUpdateExecutionReason::NonAurForeign:
            return true;
        case AurUpdateExecutionReason::None:
        case AurUpdateExecutionReason::DevelObservationUnknown:
        case AurUpdateExecutionReason::DevelUnsupported:
        case AurUpdateExecutionReason::DevelRequiresCheck:
        case AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck:
        case AurUpdateExecutionReason::AurMetadataUnavailable:
        case AurUpdateExecutionReason::VersionComparisonUnavailable:
        case AurUpdateExecutionReason::InstalledReasonUnknown:
        case AurUpdateExecutionReason::UpdatePlanInconsistent:
        case AurUpdateExecutionReason::DuplicateUpdateTarget:
        case AurUpdateExecutionReason::InstalledPackageMetadataUnavailable:
        case AurUpdateExecutionReason::RepositoryMetadataUnavailable:
        case AurUpdateExecutionReason::AurDependencyMetadataUnavailable:
        case AurUpdateExecutionReason::ProviderMetadataUnavailable:
        case AurUpdateExecutionReason::UnresolvedDependency:
        case AurUpdateExecutionReason::VersionConstraintUnverified:
        case AurUpdateExecutionReason::DependencyCycle:
        case AurUpdateExecutionReason::BuildPlanInconsistent:
        case AurUpdateExecutionReason::PackageBaseMismatch:
        case AurUpdateExecutionReason::SplitPackageSelectionRequired:
        case AurUpdateExecutionReason::MultiplePackageTargetsForPackageBase:
        case AurUpdateExecutionReason::AmbiguousProvider:
        case AurUpdateExecutionReason::ConflictsOrReplacesUnresolved:
            return false;
    }
    throw std::logic_error(localization::format_translated_message(
        "Unknown {} update preflight reason.", "AUR"));
}

namespace {

std::string join_package_names(const std::vector<std::string>& package_names) {
    std::string joined;
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        if(index != 0) joined += ", ";
        joined += package_names[index];
    }
    return joined;
}

void print_preflight_issues(const AurUpdateOperationResult& result) {
    for(const auto& target : result.targets) {
        const AurUpdateExecutionIssue* attention =
            independent_requires_check_attention_issue(result, target);
        for(const auto& issue : target.preflight_issues) {
            if(&issue == attention) continue;
            if(is_routine_aur_update_skip(issue.reason)) continue;
            Logger::error("  " + localization::translate_message("preflight issue") +
                          ": " + aur_update_preflight_reason_label(issue.reason) + ": " +
                          issue.diagnostic);
        }
    }
}

void print_preparation_details(const AurUpdateOperationResult& result) {
    for(const auto& warning : result.preparation_warnings) {
        const std::string preference = warning.preference_name.empty()
                                           ? localization::translate_message("unknown preference")
                                           : warning.preference_name;
        Logger::warn("  " + localization::translate_message("preparation warning") +
                     ": " + preference + ": " + warning.diagnostic);
    }
    for(const auto& issue : result.preparation_issues) {
        Logger::error("  " + localization::translate_message("preparation issue") +
                      ": " + aur_update_preparation_reason_label(issue.reason) + ": " +
                      issue.diagnostic);
    }
}

void print_reduction_issues(const AurUpdateOperationResult& result) {
    for(const auto& issue : result.reduction_issues) {
        Logger::error("  " + localization::translate_message("reduction issue") +
                      ": " + reduction_stage_label(issue.stage) + ": " +
                      reduction_reason_label(issue.reason) + ": " +
                      issue.diagnostic);
    }
}

void print_query_failures(const AurUpdateQueryResult& query_result) {
    for(const auto& failure : query_result.recoverable_failures) {
        if(failure.package_names.empty()) {
            Logger::error(localization::format_translated_message(
                // TRANSLATORS: The placeholders are the AUR project identity
                // and an upstream diagnostic.
                "{} update query failure for unknown packages: {}", "AUR",
                failure.diagnostic));
            continue;
        }
        Logger::error(localization::format_translated_message(
            // TRANSLATORS: The placeholders are the AUR project identity,
            // package identities, and an upstream diagnostic.
            "{} update query failure for {}: {}", "AUR",
            join_package_names(failure.package_names),
            failure.diagnostic));
    }
}

void print_operation_result(
    const AurUpdateQueryResult& query_result,
    const AurUpdateOperationResult& result) {
    // Child/work-item enumやselected identityのincoherenceは、success summaryを
    // 一行でも出す前にfail-closedにする。
    const AurUpdateCliPresentation execution_presentation =
        format_aur_update_cli_presentation(result);

    std::cout << localization::format_translated_message(
                     // TRANSLATORS: AUR is a runtime project identity.
                     "{} update:", "AUR")
              << " " << operation_status_label(result.status)
              << std::endl;
    for(const auto& target : result.targets) {
        if(const AurUpdateExecutionIssue* attention =
               independent_requires_check_attention_issue(result, target);
           attention != nullptr) {
            report_independent_requires_check_attention(
                target, *attention);
            continue;
        }
        std::cout << target.update.installed_name << ": "
                  << target_status_label(target, result.status)
                  << std::endl;
    }
    for(const std::string& line : execution_presentation.summary_lines) {
        std::cout << line << std::endl;
    }

    print_preflight_issues(result);
    print_preparation_details(result);
    for(const std::string& line : execution_presentation.error_lines) {
        Logger::error(line);
    }
    print_reduction_issues(result);

    if(result.has_partial_completion() &&
       result.status != AurUpdateOperationStatus::StoppedOnWorkItemCancellation) {
        std::cout << localization::format_translated_message(
                         // TRANSLATORS: AUR is a runtime project identity.
                         "{} update partially completed before failure.",
                         "AUR")
                  << std::endl;
    }
    if(result.has_cleanup_failure()) {
        std::cout << localization::format_translated_message(
                         // TRANSLATORS: AUR is a runtime project identity.
                         "{} update cleanup failed after a package transaction.",
                         "AUR")
                  << std::endl;
    }
    if(result.has_not_attempted_targets()) {
        std::cout << localization::format_translated_message(
                         // TRANSLATORS: AUR is a runtime project identity.
                         "{} update has targets that were not attempted.",
                         "AUR")
                  << std::endl;
    }

    print_query_failures(query_result);
    if(!query_result.recoverable_failures.empty() && result.is_success()) {
        Logger::error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "{} update completed, but query failures were reported.",
            "AUR"));
    }
}

} // namespace

PreparedFilteredAurUpdateOperation prepare_upgrade_aur_operation(
    const AppConfig& config) {
    // POLICY(#267): NoUpdatesでもunsupported optionを成功へ落とさない。
    require_supported_production_source_build_options(config);

    AurUpdateQueryResult query_result = query_installed_aur_updates();
    return prepare_filtered_aur_update_operation(
        std::move(query_result), NoExplicitSourceSatisfaction{},
        DevelRequiresCheckPolicy::BlockOperation,
        SavedSourcePreferencePolicy::Strict, config);
}

int cmd_upgrade_aur(
    PreparedFilteredAurUpdateOperation prepared,
    const AppConfig& config) try {
    FilteredAurUpdateExecutionResult result =
        execute_prepared_filtered_aur_update_operation(std::move(prepared), config);

    // POLICY(#281): upgrade-aur presentationはlegacy reducer resultを正本にし、
    // filtered boundary固有のplanner/mapping detailをcommand outputへ追加しない。
    present_filtered_aur_update_execution_result(result);
    return result.is_success() ? 0 : 1;
} catch(const FilteredAurUpdateCancelled& stop) {
    present_filtered_aur_update_execution_result(stop.result());
    return 1;
}

void present_filtered_aur_update_execution_result(
    const FilteredAurUpdateExecutionResult& result) {
    print_operation_result(
        result.query_result, result.reduced_operation_result);
}
