#include "commands_upgrade_all.hpp"

#include "app_config.hpp"
#include "application_identity.hpp"
#include "aur_update_cli_presentation.hpp"
#include "cli_authority.hpp"
#include "cli_parser.hpp"
#include "cli_runtime_contract.hpp"
#include "commands_aur_update.hpp"
#include "interactive_confirmation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "operation_state_model.hpp"
#include "presentation_projection.hpp"
#include "reviewed_source_production_outcome.hpp"
#include "runtime_diagnostic.hpp"
#include "upgrade_all_operation.hpp"

#include <algorithm>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::string_view PACKAGE_BASE_FIELD = "PackageBase";
constexpr std::string_view AUR_SERVICE = "AUR";
constexpr std::string_view COMMAND_NAME = "upgrade-all";

std::string aggregate_status_label(UpgradeAllOperationStatus status) {
    switch(status) {
        case UpgradeAllOperationStatus::Completed:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message("{} completed",
                                                           COMMAND_NAME);
        case UpgradeAllOperationStatus::NoUpdates:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message(
                "{} completed: no updates", COMMAND_NAME);
        case UpgradeAllOperationStatus::BlockedBeforeMutation:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message(
                "{} blocked before mutation", COMMAND_NAME);
        case UpgradeAllOperationStatus::StoppedOnSystemFailure:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message(
                "{} stopped on system failure", COMMAND_NAME);
        case UpgradeAllOperationStatus::StoppedOnSourceFailure:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message(
                "{} stopped on source failure", COMMAND_NAME);
        case UpgradeAllOperationStatus::StoppedAfterSourceCleanupFailure:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message(
                "{} stopped after source cleanup failure", COMMAND_NAME);
        case UpgradeAllOperationStatus::StoppedBeforeAurExecution:
            // TRANSLATORS: The placeholders are the literal command name
            // "upgrade-all" and service name "AUR".
            return localization::format_translated_message(
                "{} stopped before {} execution", COMMAND_NAME, AUR_SERVICE);
        case UpgradeAllOperationStatus::StoppedOnAurCancellation:
            return localization::translate_message("Cancelled");
        case UpgradeAllOperationStatus::StoppedOnAurFailure:
            // TRANSLATORS: The placeholders are the literal command name
            // "upgrade-all" and service name "AUR".
            return localization::format_translated_message(
                "{} stopped on {} failure", COMMAND_NAME, AUR_SERVICE);
        case UpgradeAllOperationStatus::StoppedAfterAurCleanupFailure:
            // TRANSLATORS: The placeholders are the literal command name
            // "upgrade-all" and service name "AUR".
            return localization::format_translated_message(
                "{} stopped after {} cleanup failure", COMMAND_NAME,
                AUR_SERVICE);
        case UpgradeAllOperationStatus::InconsistentResult:
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            return localization::format_translated_message(
                "{} result inconsistent", COMMAND_NAME);
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} operation status.", COMMAND_NAME));
}

std::string aggregate_phase_label(UpgradeAllOperationPhase phase) {
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
            return localization::translate_message("foreign inventory");
        case UpgradeAllOperationPhase::AurQuery:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message("{} query", AUR_SERVICE);
        case UpgradeAllOperationPhase::AurPreparation:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message("{} preparation",
                                                           AUR_SERVICE);
        case UpgradeAllOperationPhase::AurExecution:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message("{} execution",
                                                           AUR_SERVICE);
        case UpgradeAllOperationPhase::Reduction:
            return localization::translate_message("reduction");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} operation phase.", COMMAND_NAME));
}

void validate_aur_phase_presentation_boundary(
    const UpgradeAllAurPhaseResult& aur) {
    switch(aur.status) {
        case UpgradeAllAurPhaseStatus::NotAttempted:
        case UpgradeAllAurPhaseStatus::NoUpdates:
        case UpgradeAllAurPhaseStatus::Completed:
        case UpgradeAllAurPhaseStatus::BlockedBeforeExecution:
        case UpgradeAllAurPhaseStatus::StoppedOnProviderTransactionFailure:
        case UpgradeAllAurPhaseStatus::StoppedOnWorkItemCancellation:
        case UpgradeAllAurPhaseStatus::StoppedOnWorkItemFailure:
        case UpgradeAllAurPhaseStatus::StoppedAfterCleanupFailure:
        case UpgradeAllAurPhaseStatus::InconsistentResult:
            break;
        default:
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: The placeholders are the literal command name
                // "upgrade-all" and service name "AUR".
                "Unknown {} {} phase status.", COMMAND_NAME, AUR_SERVICE));
    }

    if(aur.not_attempted_reason.has_value()) {
        switch(aur.not_attempted_reason.value()) {
            case UpgradeAllNotAttemptedReason::PreparationBlocked:
            case UpgradeAllNotAttemptedReason::SystemFailure:
            case UpgradeAllNotAttemptedReason::SourceFailure:
            case UpgradeAllNotAttemptedReason::SourceCleanupFailure:
            case UpgradeAllNotAttemptedReason::SystemSourceIncomplete:
            case UpgradeAllNotAttemptedReason::ForeignInventoryFailure:
            case UpgradeAllNotAttemptedReason::CacheAuthorityFailure:
            case UpgradeAllNotAttemptedReason::PriorAggregateInconsistency:
                break;
            default:
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal command
                    // name "upgrade-all" and enum state "NotAttempted".
                    "Unknown {} {} reason.", COMMAND_NAME,
                    "NotAttempted"));
        }
    }

    if(!aur.operation_result.has_value()) return;
    switch(aur.operation_result->reduced_operation_result.status) {
        case AurUpdateOperationStatus::NoUpdates:
        case AurUpdateOperationStatus::Completed:
        case AurUpdateOperationStatus::BlockedBeforeExecution:
        case AurUpdateOperationStatus::StoppedOnProviderTransactionFailure:
        case AurUpdateOperationStatus::StoppedOnWorkItemCancellation:
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
        case AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure:
        case AurUpdateOperationStatus::InconsistentResult:
            return;
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown {} update operation status.", AUR_SERVICE));
}

std::string system_phase_status_label(SystemUpgradePhaseStatus status) {
    switch(status) {
        case SystemUpgradePhaseStatus::NotAttempted:
            return localization::translate_message("not attempted");
        case SystemUpgradePhaseStatus::Completed:
            return localization::translate_message("completed");
        case SystemUpgradePhaseStatus::Failed:
            return localization::translate_message("failed");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown system upgrade phase status."));
}

std::string source_failure_label(
    RegisteredSourceUpgradeFailureKind kind) {
    switch(kind) {
        case RegisteredSourceUpgradeFailureKind::None:
            return localization::translate_message("reason unavailable");
        case RegisteredSourceUpgradeFailureKind::InvalidPreferenceName:
            return localization::translate_message("invalid source preference name");
        case RegisteredSourceUpgradeFailureKind::PreferenceUnavailable:
            return localization::translate_message("source preference unavailable");
        case RegisteredSourceUpgradeFailureKind::PackageMetadataUnavailable:
            return localization::translate_message("package metadata unavailable");
        case RegisteredSourceUpgradeFailureKind::CacheAuthorityFailure:
            return localization::translate_message("cache authority failure");
        case RegisteredSourceUpgradeFailureKind::BuildOrInstallFailed:
            return localization::translate_message("build or install failed");
        case RegisteredSourceUpgradeFailureKind::
            CleanupFailedAfterPackageTransaction:
            return localization::translate_message("cleanup failed after package transaction");
        case RegisteredSourceUpgradeFailureKind::AuthoritativeExecutionIncomplete:
            return localization::translate_message("authoritative execution incomplete; installation and publication outcomes are separate");
        case RegisteredSourceUpgradeFailureKind::DevelRequiresCheckSkipped:
            return localization::translate_message("devel update requires check; explicit rebuild declined");
        case RegisteredSourceUpgradeFailureKind::UpdateStatusUnknownSkipped:
            return localization::translate_message("package update status unknown");
        case RegisteredSourceUpgradeFailureKind::PriorPhaseStopped:
            return localization::translate_message("prior phase stopped");
        case RegisteredSourceUpgradeFailureKind::UnknownException:
            return localization::translate_message("unknown exception");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown registered source failure kind."));
}

std::string system_source_phase_label(SystemSourceUpgradePhase phase) {
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
    throw std::logic_error(localization::translate_message(
        "Unknown system/source phase."));
}

std::string system_issue_kind_label(SystemSourceUpgradeIssueKind kind) {
    switch(kind) {
        case SystemSourceUpgradeIssueKind::PreferenceEnumerationUnavailable:
            return localization::translate_message("source preference enumeration unavailable");
        case SystemSourceUpgradeIssueKind::PreferenceUnavailable:
            return localization::translate_message("source preference unavailable");
        case SystemSourceUpgradeIssueKind::SourceIdentityResolutionFailed:
            return localization::translate_message("source identity resolution failed");
        case SystemSourceUpgradeIssueKind::SourceWorkItemPreparationFailed:
            return localization::translate_message("source work-item preparation failed");
        case SystemSourceUpgradeIssueKind::SourceInvocationPreparationFailed:
            return localization::translate_message("source invocation preparation failed");
        case SystemSourceUpgradeIssueKind::SourceBaselineSnapshotUnavailable:
            return localization::translate_message("source baseline snapshot unavailable");
        case SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable:
            return localization::translate_message("system package snapshot unavailable");
        case SystemSourceUpgradeIssueKind::PostSystemSourceSnapshotUnavailable:
            return localization::translate_message("post-system/source snapshot unavailable");
        case SystemSourceUpgradeIssueKind::CacheAuthorityInvalid:
            return localization::translate_message("cache authority invalid");
        case SystemSourceUpgradeIssueKind::InvalidPreferenceName:
            return localization::translate_message("invalid source preference name");
        case SystemSourceUpgradeIssueKind::OptionSnapshotMismatch:
            return localization::translate_message("option snapshot mismatch");
        case SystemSourceUpgradeIssueKind::PreparedCorrelationInconsistent:
            return localization::translate_message("prepared source correlation inconsistent");
        case SystemSourceUpgradeIssueKind::PreparedCapabilityConsumed:
            return localization::translate_message("prepared source capability consumed");
        case SystemSourceUpgradeIssueKind::UnknownPreparationFailure:
            return localization::translate_message("unknown source preparation failure");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown system/source issue kind."));
}

std::string system_issue_impact_label(
    SystemSourceUpgradeIssueImpact impact) {
    switch(impact) {
        case SystemSourceUpgradeIssueImpact::ObservabilityOnly:
            return localization::translate_message("observability only");
        case SystemSourceUpgradeIssueImpact::AffectsSuccess:
            return localization::translate_message("affects success");
        case SystemSourceUpgradeIssueImpact::BlocksExecution:
            return localization::translate_message("blocks execution");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown system/source issue impact."));
}

DiagnosticSeverity system_issue_severity(
    SystemSourceUpgradeIssueImpact impact) {
    switch(impact) {
        case SystemSourceUpgradeIssueImpact::ObservabilityOnly:
            return DiagnosticSeverity::Warning;
        case SystemSourceUpgradeIssueImpact::AffectsSuccess:
        case SystemSourceUpgradeIssueImpact::BlocksExecution:
            return DiagnosticSeverity::Error;
    }
    throw std::logic_error(localization::translate_message(
        "Unknown system/source issue impact."));
}

std::string system_warning_kind_label(
    SystemSourceUpgradeWarningKind kind) {
    switch(kind) {
        case SystemSourceUpgradeWarningKind::SourcePreference:
            return localization::translate_message("source preference");
        case SystemSourceUpgradeWarningKind::InvalidPreferenceName:
            return localization::translate_message("invalid source preference name");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown system/source warning kind."));
}

std::string package_metadata_error_label(PackageMetadataErrorCode code) {
    switch(code) {
        case PackageMetadataErrorCode::ConfigurationUnavailable:
            return localization::translate_message("configuration unavailable");
        case PackageMetadataErrorCode::ConfigurationMalformed:
            return localization::translate_message("configuration malformed");
        case PackageMetadataErrorCode::InitializationFailed:
            return localization::translate_message("database initialization failed");
        case PackageMetadataErrorCode::LocalDatabaseUnavailable:
            return localization::translate_message("local database unavailable");
        case PackageMetadataErrorCode::InvalidPackageName:
            return localization::translate_message("invalid package name");
        case PackageMetadataErrorCode::QueryFailed:
            return localization::translate_message("package query failed");
        case PackageMetadataErrorCode::MalformedMetadata:
            return localization::translate_message("malformed package metadata");
        case PackageMetadataErrorCode::SyncDatabaseUnavailable:
            return localization::translate_message("sync database unavailable");
        case PackageMetadataErrorCode::RepositoryNotConfigured:
            return localization::translate_message("repository not configured");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package metadata error code."));
}

std::string aggregate_warning_kind_label(
    UpgradeAllOperationWarningKind kind) {
    switch(kind) {
        case UpgradeAllOperationWarningKind::RegisteredSourcePreference:
            return localization::translate_message("registered source preference");
        case UpgradeAllOperationWarningKind::AurPreparation:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message("{} preparation",
                                                           AUR_SERVICE);
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} warning kind.", COMMAND_NAME));
}

std::string aggregate_issue_kind_label(
    UpgradeAllOperationIssueKind kind) {
    switch(kind) {
        case UpgradeAllOperationIssueKind::ExplicitSourceAdapterInvalid:
            return localization::translate_message("explicit source adapter invalid");
        case UpgradeAllOperationIssueKind::OptionSnapshotMismatch:
            return localization::translate_message("option snapshot mismatch");
        case UpgradeAllOperationIssueKind::SourceSnapshotMismatch:
            return localization::translate_message("source snapshot mismatch");
        case UpgradeAllOperationIssueKind::ExplicitSourceCorrelationInconsistent:
            return localization::translate_message("explicit source correlation inconsistent");
        case UpgradeAllOperationIssueKind::PreparedCapabilityConsumed:
            return localization::translate_message("prepared capability consumed");
        case UpgradeAllOperationIssueKind::SystemSourceExecutionFailedUnexpectedly:
            return localization::translate_message("system/source execution failed unexpectedly");
        case UpgradeAllOperationIssueKind::SystemSourcePhaseIncomplete:
            return localization::translate_message("system/source phase incomplete");
        case UpgradeAllOperationIssueKind::ForeignInventoryConfigurationFailed:
            return localization::translate_message("foreign inventory configuration failed");
        case UpgradeAllOperationIssueKind::ForeignInventoryReadFailed:
            return localization::translate_message("foreign inventory read failed");
        case UpgradeAllOperationIssueKind::CacheAuthorityInvalid:
            return localization::translate_message("cache authority invalid");
        case UpgradeAllOperationIssueKind::AurQueryFailed:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message("{} query failed",
                                                           AUR_SERVICE);
        case UpgradeAllOperationIssueKind::FilteredAurPreparationFailed:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message(
                "filtered {} preparation failed", AUR_SERVICE);
        case UpgradeAllOperationIssueKind::FilteredAurExecutionFailed:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message(
                "filtered {} execution failed", AUR_SERVICE);
        case UpgradeAllOperationIssueKind::
            DuplicateExclusionCorrelationInconsistent:
            return localization::translate_message("duplicate exclusion correlation inconsistent");
        case UpgradeAllOperationIssueKind::
            ExternalSatisfactionCorrelationInconsistent:
            return localization::translate_message("external satisfaction correlation inconsistent");
        case UpgradeAllOperationIssueKind::UnknownFailure:
            return localization::translate_message("unknown aggregate failure");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} issue kind.", COMMAND_NAME));
}

std::string adapter_issue_kind_label(
    UpgradeAllExplicitSourceAdapterIssueKind kind) {
    switch(kind) {
        case UpgradeAllExplicitSourceAdapterIssueKind::PreferencePackageNameMissing:
            return localization::translate_message("preference package name missing");
        case UpgradeAllExplicitSourceAdapterIssueKind::PackageBaseUnavailable:
            return localization::format_translated_message(
                "{} unavailable", PACKAGE_BASE_FIELD);
        case UpgradeAllExplicitSourceAdapterIssueKind::
            CanonicalSourceIdentityUnavailable:
            return localization::translate_message("canonical source identity unavailable");
        case UpgradeAllExplicitSourceAdapterIssueKind::
            DuplicateOriginalPreferenceIndex:
            return localization::translate_message("duplicate original preference index");
        case UpgradeAllExplicitSourceAdapterIssueKind::
            AdapterCorrelationInconsistent:
            return localization::translate_message("adapter correlation inconsistent");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown explicit source adapter issue kind."));
}

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
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown {} reduction stage.", AUR_SERVICE));
}

std::string reduction_reason_label(
    AurUpdateOperationReductionReason reason) {
    switch(reason) {
        case AurUpdateOperationReductionReason::
            DuplicatePreflightUpdatePlanIndex:
            return localization::translate_message("duplicate preflight update plan index");
        case AurUpdateOperationReductionReason::
            OutOfRangePreflightUpdatePlanIndex:
            return localization::translate_message("out-of-range preflight update plan index");
        case AurUpdateOperationReductionReason::
            PreflightTargetOrderInconsistent:
            return localization::translate_message("preflight target order inconsistent");
        case AurUpdateOperationReductionReason::DuplicatePreparationAttribution:
            return localization::translate_message("duplicate preparation attribution");
        case AurUpdateOperationReductionReason::UnknownPreparationUpdatePlanIndex:
            return localization::translate_message("unknown preparation update plan index");
        case AurUpdateOperationReductionReason::
            PreparationAttributionInconsistent:
            return localization::translate_message("preparation attribution inconsistent");
        case AurUpdateOperationReductionReason::
            PreparationTargetSnapshotInconsistent:
            return localization::translate_message("preparation target snapshot inconsistent");
        case AurUpdateOperationReductionReason::DuplicateExecutionWorkItemIndex:
            return localization::translate_message("duplicate execution work item index");
        case AurUpdateOperationReductionReason::
            ExecutionWorkItemOrderInconsistent:
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
        case AurUpdateOperationReductionReason::
            ExecutionResultWithPreparationIssues:
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
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown {} reduction reason.", AUR_SERVICE));
}

std::string filtered_issue_kind_label(
    FilteredAurUpdateOperationIssueKind kind) {
    switch(kind) {
        case FilteredAurUpdateOperationIssueKind::UnknownUpdateClassification:
            return localization::translate_message("unknown update classification");
        case FilteredAurUpdateOperationIssueKind::
            DevelRequiresCheckPolicyInconsistent:
            return localization::translate_message(
                "target/planner mapping inconsistent");
        case FilteredAurUpdateOperationIssueKind::TargetPlannerMappingInconsistent:
            return localization::translate_message("target/planner mapping inconsistent");
        case FilteredAurUpdateOperationIssueKind::FilteredTargetMappingInconsistent:
            return localization::translate_message("filtered target mapping inconsistent");
        case FilteredAurUpdateOperationIssueKind::
            PreflightTargetMappingInconsistent:
            return localization::translate_message("preflight target mapping inconsistent");
        case FilteredAurUpdateOperationIssueKind::
            PreflightInvocationIndexOutOfRange:
            return localization::translate_message("preflight invocation index out of range");
        case FilteredAurUpdateOperationIssueKind::
            PreflightInvocationIdentityMismatch:
            return localization::translate_message("preflight invocation identity mismatch");
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexMissing:
            return localization::translate_message("build-plan root index missing");
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootIndexOutOfRange:
            return localization::translate_message("build-plan root index out of range");
        case FilteredAurUpdateOperationIssueKind::BuildPlanRootIdentityMismatch:
            return localization::translate_message("build-plan root identity mismatch");
        case FilteredAurUpdateOperationIssueKind::
            BuildPlanRootPackageIdentityMismatch:
            return localization::translate_message("build-plan root package identity mismatch");
        case FilteredAurUpdateOperationIssueKind::BuildUnitOrderIdentityMismatch:
            return localization::translate_message("build-unit order identity mismatch");
        case FilteredAurUpdateOperationIssueKind::
            BuildUnitRootAttributionInconsistent:
            return localization::translate_message("build-unit root attribution inconsistent");
        case FilteredAurUpdateOperationIssueKind::
            BuildUnitSelectionMappingInconsistent:
            return localization::translate_message("build-unit selection mapping inconsistent");
        case FilteredAurUpdateOperationIssueKind::
            ExecutionBuildUnitMappingInconsistent:
            return localization::translate_message("execution build-unit mapping inconsistent");
        case FilteredAurUpdateOperationIssueKind::ReducedTargetMappingInconsistent:
            return localization::translate_message("reduced target mapping inconsistent");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown filtered {} operation issue kind.", AUR_SERVICE));
}

std::string planning_issue_kind_label(UpgradeAllPlanningIssueKind kind) {
    switch(kind) {
        case UpgradeAllPlanningIssueKind::ExplicitPreferencePackageNameMissing:
            return localization::translate_message("explicit preference package name missing");
        case UpgradeAllPlanningIssueKind::ExplicitProducedPackageNameMissing:
            return localization::translate_message("explicit produced package name missing");
        case UpgradeAllPlanningIssueKind::ExplicitPackageBaseAbsent:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "explicit {} absent", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::ExplicitPackageBaseResolutionFailed:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "explicit {} resolution failed", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::ExplicitPackageBaseEmpty:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "explicit {} empty", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::ExplicitSourceIdentityAbsent:
            return localization::translate_message("explicit source identity absent");
        case UpgradeAllPlanningIssueKind::ExplicitSourceIdentityResolutionFailed:
            return localization::translate_message("explicit source identity resolution failed");
        case UpgradeAllPlanningIssueKind::ExplicitSourceIdentityEmpty:
            return localization::translate_message("explicit source identity empty");
        case UpgradeAllPlanningIssueKind::
            ConflictingExplicitSourceIdentityDefinition:
            return localization::translate_message("conflicting explicit source identity definition");
        case UpgradeAllPlanningIssueKind::ConflictingExplicitPackageName:
            return localization::translate_message("conflicting explicit package name");
        case UpgradeAllPlanningIssueKind::ConflictingExplicitPackageBase:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "conflicting explicit {}", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::AurTargetPackageNameMissing:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message(
                "{} target package name missing", AUR_SERVICE);
        case UpgradeAllPlanningIssueKind::AurTargetPackageBaseAbsent:
            // TRANSLATORS: The placeholders are the literal service name "AUR"
            // and field name "PackageBase".
            return localization::format_translated_message(
                "{} target {} absent", AUR_SERVICE, PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::AurTargetPackageBaseResolutionFailed:
            // TRANSLATORS: The placeholders are the literal service name "AUR"
            // and field name "PackageBase".
            return localization::format_translated_message(
                "{} target {} resolution failed", AUR_SERVICE,
                PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::AurTargetPackageBaseEmpty:
            // TRANSLATORS: The placeholders are the literal service name "AUR"
            // and field name "PackageBase".
            return localization::format_translated_message(
                "{} target {} empty", AUR_SERVICE, PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::UnsupportedAurTarget:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message(
                "unsupported {} target", AUR_SERVICE);
        case UpgradeAllPlanningIssueKind::IncompleteAurTarget:
            // TRANSLATORS: The placeholder is the literal service name "AUR".
            return localization::format_translated_message(
                "incomplete {} target", AUR_SERVICE);
        case UpgradeAllPlanningIssueKind::BuildUnitPackageBaseAbsent:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "build-unit {} absent", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::BuildUnitPackageBaseResolutionFailed:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "build-unit {} resolution failed", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::BuildUnitPackageBaseEmpty:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "build-unit {} empty", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::BuildUnitHasNoRootAttribution:
            return localization::translate_message("build unit has no root attribution");
        case UpgradeAllPlanningIssueKind::BuildUnitTargetIndexOutOfRange:
            return localization::translate_message("build-unit target index out of range");
        case UpgradeAllPlanningIssueKind::DuplicateSelectedTargetPackageBase:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "duplicate selected target {}", PACKAGE_BASE_FIELD);
        case UpgradeAllPlanningIssueKind::DuplicateSelectedBuildUnitPackageBase:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "duplicate selected build-unit {}", PACKAGE_BASE_FIELD);
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} planning issue kind.", COMMAND_NAME));
}

std::string duplicate_reason_label(UpgradeAllTargetDisposition disposition) {
    switch(disposition) {
        case UpgradeAllTargetDisposition::ExcludedByExplicitPackageName:
            return localization::translate_message("package name handled by explicit source preference");
        case UpgradeAllTargetDisposition::ExcludedByExplicitPackageBase:
            // TRANSLATORS: The placeholder is the literal field name
            // "PackageBase".
            return localization::format_translated_message(
                "{} handled by explicit source preference",
                PACKAGE_BASE_FIELD);
        case UpgradeAllTargetDisposition::Selected:
        case UpgradeAllTargetDisposition::Unsupported:
        case UpgradeAllTargetDisposition::IdentityIncomplete:
        case UpgradeAllTargetDisposition::ConflictingExplicitSourceIdentity:
        case UpgradeAllTargetDisposition::ConflictingSelectedPackageBase:
            throw std::logic_error(localization::translate_message(
                "Non-exclusion target disposition reached duplicate presentation."));
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} target disposition.", COMMAND_NAME));
}

std::string build_unit_role_label(UpgradeAllBuildUnitRole role) {
    switch(role) {
        case UpgradeAllBuildUnitRole::Root:
            return localization::translate_message("root");
        case UpgradeAllBuildUnitRole::RuntimeDependency:
            return localization::translate_message("runtime dependency");
        case UpgradeAllBuildUnitRole::BuildDependency:
            return localization::translate_message("build dependency");
        case UpgradeAllBuildUnitRole::CheckDependency:
            return localization::translate_message("check dependency");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        "Unknown {} build-unit role.", COMMAND_NAME));
}

std::string join_strings(const std::vector<std::string>& values) {
    std::string joined;
    for(std::size_t index = 0; index < values.size(); ++index) {
        if(index != 0) joined += ", ";
        joined += values[index];
    }
    return joined;
}

std::string join_query_package_names(
    const std::vector<std::string>& package_names) {
    return package_names.empty() ? localization::translate_message("unknown packages")
                                 : join_strings(package_names);
}

std::string operation_outcome_label(OperationOutcome outcome) {
    switch(outcome) {
        case OperationOutcome::Succeeded:
            return localization::translate_message("Succeeded");
        case OperationOutcome::NoOp:
            return localization::translate_message("No operation needed");
        case OperationOutcome::Blocked:
            return localization::translate_message("Blocked");
        case OperationOutcome::PartialFailure:
            return localization::translate_message("Partial failure");
        case OperationOutcome::Failed:
            return localization::translate_message("Failed");
        case OperationOutcome::NotAttempted:
            return localization::translate_message("Not attempted");
        case OperationOutcome::Inconsistent:
            return localization::translate_message("Inconsistent");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown operation outcome."));
}

std::string package_state_observation_label(
    PackageStateObservation observation) {
    switch(observation) {
        case PackageStateObservation::Changed:
            return localization::translate_message("Changed");
        case PackageStateObservation::VerifiedUnchanged:
            return localization::translate_message("Verified unchanged");
        case PackageStateObservation::Unverified:
            return localization::translate_message("Unverified");
        case PackageStateObservation::NotObserved:
            return localization::translate_message("Not observed");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package-state observation."));
}

std::string observation_reason_label(ObservationReason reason) {
    switch(reason) {
        case ObservationReason::BeforeSnapshotUnavailable:
            return localization::translate_message(
                "Before snapshot unavailable");
        case ObservationReason::AfterSnapshotUnavailable:
            return localization::translate_message(
                "After snapshot unavailable");
        case ObservationReason::ObservationNotPrepared:
            return localization::translate_message("Observation not prepared");
        case ObservationReason::PhaseNotAttempted:
            return localization::translate_message("Phase not attempted");
        case ObservationReason::OperationFailed:
            return localization::translate_message("Operation failed");
        case ObservationReason::AuthorityFailure:
            return localization::translate_message("Authority failure");
        case ObservationReason::InconsistentEvidence:
            return localization::translate_message("Inconsistent evidence");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown observation reason."));
}

std::string no_op_basis_label(NoOpBasis basis) {
    switch(basis) {
        case NoOpBasis::NoRelevantWork:
            return localization::translate_message("No relevant work");
        case NoOpBasis::VerifiedUnchanged:
            return localization::translate_message("Verified unchanged");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown no-op basis."));
}

std::string diagnostic_required_action_label(
    DiagnosticRequiredAction action) {
    switch(action) {
        case DiagnosticRequiredAction::None:
            return localization::translate_message("None");
        case DiagnosticRequiredAction::CorrectInput:
            return localization::translate_message("Correct input");
        case DiagnosticRequiredAction::SelectCandidate:
            return localization::translate_message("Select a candidate");
        case DiagnosticRequiredAction::EnableInteraction:
            return localization::translate_message("Enable interaction");
        case DiagnosticRequiredAction::RetryQuery:
            return localization::translate_message("Retry the query");
        case DiagnosticRequiredAction::InspectMetadata:
            return localization::translate_message("Inspect metadata");
        case DiagnosticRequiredAction::ConfirmEvaluation:
            return localization::translate_message("Confirm evaluation");
        case DiagnosticRequiredAction::ResolveBlocker:
            return localization::translate_message("Resolve the blocker");
        case DiagnosticRequiredAction::InspectPartialResult:
            return localization::translate_message("Inspect the partial result");
        case DiagnosticRequiredAction::ReportInconsistency:
            return localization::translate_message("Report the inconsistency");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown diagnostic required action."));
}

std::string foreign_inventory_status_label(
    UpgradeAllForeignInventoryPhaseStatus status) {
    switch(status) {
        case UpgradeAllForeignInventoryPhaseStatus::NotAttempted:
            return localization::translate_message(
                "foreign inventory not attempted");
        case UpgradeAllForeignInventoryPhaseStatus::Completed:
            return localization::translate_message("foreign inventory completed");
        case UpgradeAllForeignInventoryPhaseStatus::Failed:
            return localization::translate_message("foreign inventory failed");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown foreign inventory status."));
}

std::string registered_source_status_reason_label(
    RegisteredSourceUpgradeStatus status) {
    switch(status) {
        case RegisteredSourceUpgradeStatus::Updated:
            return localization::translate_message("registered source updated");
        case RegisteredSourceUpgradeStatus::NoChange:
            return localization::translate_message("registered source unchanged");
        case RegisteredSourceUpgradeStatus::Failed:
            return localization::translate_message("registered source failed");
        case RegisteredSourceUpgradeStatus::UpdatedCleanupFailed:
            return localization::translate_message(
                "registered source updated; cleanup failed");
        case RegisteredSourceUpgradeStatus::NoChangeCleanupFailed:
            return localization::translate_message(
                "registered source unchanged; cleanup failed");
        case RegisteredSourceUpgradeStatus::NotAttempted:
            return localization::translate_message(
                "registered source not attempted");
        case RegisteredSourceUpgradeStatus::Unsupported:
            return localization::translate_message(
                "registered source unsupported");
        case RegisteredSourceUpgradeStatus::Incomplete:
            return localization::translate_message(
                "registered source incomplete");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown registered-source status."));
}

std::string aur_target_status_reason_label(
    AurUpdateOperationTargetStatus status) {
    switch(status) {
        case AurUpdateOperationTargetStatus::Updated:
            return localization::format_translated_message(
                "{} target updated", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::NoChange:
            return localization::format_translated_message(
                "{} target unchanged", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::Skipped:
            return localization::format_translated_message(
                "{} target skipped", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::Unsupported:
            return localization::format_translated_message(
                "{} target unsupported", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::Incomplete:
            return localization::format_translated_message(
                "{} target incomplete", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::Cancelled:
            return localization::translate_message("Cancelled");
        case AurUpdateOperationTargetStatus::Failed:
            return localization::format_translated_message(
                "{} target failed", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
            return localization::format_translated_message(
                "{} target updated; cleanup failed", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
            return localization::format_translated_message(
                "{} target unchanged; cleanup failed", AUR_SERVICE);
        case AurUpdateOperationTargetStatus::NotAttempted:
            return localization::format_translated_message(
                "{} target not attempted", AUR_SERVICE);
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        "Unknown {} update target status.", AUR_SERVICE));
}

std::string presentation_boundary_reason_label(
    UpgradeAllPresentationBoundaryReason reason) {
    switch(reason) {
        case UpgradeAllPresentationBoundaryReason::AggregateDiagnostic:
            return localization::translate_message("aggregate diagnostic");
        case UpgradeAllPresentationBoundaryReason::AurPhaseDiagnostic:
            return localization::format_translated_message(
                "{} phase diagnostic", AUR_SERVICE);
        case UpgradeAllPresentationBoundaryReason::AurQueryFailure:
            return localization::format_translated_message(
                "{} query failure", AUR_SERVICE);
    }
    throw std::logic_error(localization::translate_message(
        "Unknown presentation boundary reason."));
}

std::string upgrade_all_presentation_reason_label(
    const UpgradeAllPresentationReasonValue& reason) {
    return std::visit(
        [](const auto& typed_reason) -> std::string {
            using Reason = std::decay_t<decltype(typed_reason)>;
            if constexpr(std::is_same_v<
                             Reason,
                             UpgradeAllPresentationBoundaryReason>) {
                return presentation_boundary_reason_label(typed_reason);
            } else if constexpr(std::is_same_v<Reason, OperationOutcome>) {
                return operation_outcome_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    UpgradeAllOperationStatus>) {
                return aggregate_status_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    UpgradeAllOperationIssueKind>) {
                return aggregate_issue_kind_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    SystemUpgradePhaseStatus>) {
                return system_phase_status_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    UpgradeAllForeignInventoryPhaseStatus>) {
                return foreign_inventory_status_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PackageMetadataErrorCode>) {
                return package_metadata_error_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    RegisteredSourceUpgradeStatus>) {
                return registered_source_status_reason_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    RegisteredSourceUpgradeFailureKind>) {
                return source_failure_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    AurUpdatePreparationReason>) {
                return aur_update_preparation_reason_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    UpgradeAllPlanningIssueKind>) {
                return planning_issue_kind_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    AurUpdateOperationReductionReason>) {
                return reduction_reason_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    FilteredAurUpdateOperationIssueKind>) {
                return filtered_issue_kind_label(typed_reason);
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    AurUpdateOperationTargetStatus>) {
                return aur_target_status_reason_label(typed_reason);
            } else {
                // The carrier still retains the exact typed enum. Existing
                // route detail below prints its route-owned diagnostic.
                return localization::format_translated_message(
                    "{} work-item failure", AUR_SERVICE);
            }
        },
        reason);
}

void print_upgrade_all_summary(
    const OperationStateProjection& operation_state,
    const UpgradeAllPhasePackageStateObservations& phase_observations,
    const PresentationProjection& presentation) {
    std::cout << localization::format_translated_message(
                     "{} summary:", COMMAND_NAME)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "  operation outcome: {}",
                     operation_outcome_label(operation_state.outcome))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "  package state observation: {}",
                     package_state_observation_label(
                         operation_state.package_state.state))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "    {}: {}", "system/source",
                     package_state_observation_label(
                         phase_observations.system_source.state))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "    {}: {}", AUR_SERVICE,
                     package_state_observation_label(
                         phase_observations.aur.state))
              << std::endl;
    const bool reason_is_presented_as_attention =
        operation_state.outcome == OperationOutcome::Succeeded &&
        operation_state.package_state.state ==
            PackageStateObservation::Unverified;
    if(operation_state.package_state.reason.has_value() &&
       !reason_is_presented_as_attention) {
        std::cout << localization::format_translated_message(
                         "  observation reason: {}",
                         observation_reason_label(
                             operation_state.package_state.reason.value()))
                  << std::endl;
    }
    if(operation_state.no_op_basis.has_value()) {
        std::cout << localization::format_translated_message(
                         "  no-op basis: {}",
                         no_op_basis_label(
                             operation_state.no_op_basis.value()))
                  << std::endl;
    }
    std::cout << localization::format_translated_message(
                     "  items: {} total, {} normal, {} attention-required",
                     presentation.summary_counts.total,
                     presentation.summary_counts.normal,
                     presentation.summary_counts.attention_required)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "  update candidates: {}, blockers: {}, requires-check: {}, failures: {}",
                     presentation.summary_counts.update_candidates,
                     presentation.summary_counts.blockers,
                     presentation.summary_counts.requires_check,
                     presentation.summary_counts.failures)
              << std::endl;
}

void print_upgrade_all_attention(
    const PresentationProjection& presentation) {
    if(presentation.attention_items.empty()) return;

    std::cout << std::endl
              << localization::translate_message(
                     "Attention-required details:")
              << std::endl;
    for(const PresentationItem& item : presentation.attention_items) {
        std::cout << "  - ";
        if(item.requested_package.has_value()) {
            std::cout << localization::format_translated_message(
                "package: {}", item.requested_package.value());
        } else if(item.source_kind != DiagnosticSourceKind::Unspecified) {
            std::cout << localization::format_translated_message(
                "source: {}",
                diagnostic_source_label(item.source_kind));
        } else {
            std::cout << localization::translate_message("operation-wide");
        }
        std::cout << std::endl;
        if(item.package_base.has_value() &&
           item.package_base != item.requested_package) {
            std::cout << "    PackageBase: " << item.package_base.value()
                      << std::endl;
        }
        if(item.repository.has_value()) {
            std::cout << localization::format_translated_message(
                             "    repository: {}",
                             item.repository.value())
                      << std::endl;
        }
        if(item.canonical_source_identity.has_value()) {
            std::cout << localization::format_translated_message(
                             "    canonical source identity: {}",
                             item.canonical_source_identity.value())
                      << std::endl;
        }
        if(item.local_root.has_value()) {
            std::cout << localization::format_translated_message(
                             "    local root: {}",
                             item.local_root->string())
                      << std::endl;
        }
        if(item.package_state.has_value()) {
            std::cout << localization::format_translated_message(
                             "    package state: {}",
                             package_state_observation_label(
                                 item.package_state->state))
                      << std::endl;
            if(item.package_state->reason.has_value()) {
                std::cout << localization::format_translated_message(
                                 "    observation reason: {}",
                                 observation_reason_label(
                                     item.package_state->reason.value()))
                          << std::endl;
            }
        }
        if(item.diagnostic_class.has_value()) {
            std::cout << localization::format_translated_message(
                             "    diagnostic: {}",
                             diagnostic_class_label(
                                 item.diagnostic_class.value()))
                      << std::endl;
        }
        if(item.is_update_candidate) {
            std::cout << localization::translate_message(
                             "    update candidate")
                      << std::endl;
        }
        if(item.is_blocking) {
            std::cout << localization::translate_message("    blocking")
                      << std::endl;
        }
        if(item.requires_check) {
            std::cout << localization::translate_message(
                             "    requires check")
                      << std::endl;
        }
        if(item.requires_manual_action) {
            std::cout << localization::translate_message(
                             "    manual action required")
                      << std::endl;
        }
        for(const PresentationArtifactIdentity& artifact :
            item.selected_artifacts) {
            std::cout << localization::format_translated_message(
                             "    selected artifact: {} {}",
                             artifact.package_name,
                             artifact.full_version)
                      << std::endl;
        }
        for(const PresentationArtifactIdentity& artifact :
            item.unselected_artifacts) {
            std::cout << localization::format_translated_message(
                             "    unselected artifact: {} {}",
                             artifact.package_name,
                             artifact.full_version)
                      << std::endl;
        }
        for(const UpgradeAllPresentationReason& reason :
            item.upgrade_all_reasons) {
            std::cout << localization::format_translated_message(
                             "    reason [{}]: {}",
                             aggregate_phase_label(reason.phase),
                             upgrade_all_presentation_reason_label(
                                 reason.reason))
                      << std::endl;
            std::cout << localization::format_translated_message(
                             "      required action: {}",
                             diagnostic_required_action_label(
                                 reason.required_action))
                      << std::endl;
        }
    }
}

void append_cross_source_version_lock_line(
    std::string& output, std::string_view line) {
    output.append(line);
    output.push_back('\n');
}

bool append_cross_source_version_lock_replacement(
    std::string& output,
    const CrossSourceVersionLockAssessment& assessment) {
    switch(assessment.status) {
        case CrossSourceVersionLockStatus::CompatibleReplacement:
        case CrossSourceVersionLockStatus::IncompatibleReplacement: {
            const auto* query =
                std::get_if<AurReplacementCandidateQuerySuccess>(
                    &assessment.evidence.aur_replacement);
            if(query == nullptr || query->candidates.size() != 1U ||
               !assessment.replacement_requirement.has_value()) {
                return false;
            }
            const AurPackageConstraintMetadata& replacement =
                query->candidates.front();
            const std::string* replacement_version =
                replacement.package_version.version();
            if(replacement.package_name.empty() || replacement_version == nullptr) {
                return false;
            }
            // TRANSLATORS: The placeholders are the service name "AUR", an AUR
            // package name, and its version.
            append_cross_source_version_lock_line(
                output,
                localization::format_translated_message(
                    "    observed {} replacement candidate: {} {}",
                    AUR_SERVICE, replacement.package_name,
                    *replacement_version));
            // TRANSLATORS: The placeholder is one validated dependency expression.
            append_cross_source_version_lock_line(
                output,
                localization::format_translated_message(
                    "    replacement requirement: {}",
                    assessment.replacement_requirement->raw_specification()));
            if(assessment.status ==
               CrossSourceVersionLockStatus::CompatibleReplacement) {
                append_cross_source_version_lock_line(
                    output,
                    localization::translate_message(
                        "    replacement metadata: the direct runtime requirement matches the observed repository candidate"));
            } else {
                append_cross_source_version_lock_line(
                    output,
                    localization::translate_message(
                        "    replacement metadata: the direct runtime requirement does not match the observed repository candidate"));
            }
            return true;
        }
        case CrossSourceVersionLockStatus::MissingReplacement:
            if(!std::holds_alternative<AurReplacementCandidateNotFound>(
                   assessment.evidence.aur_replacement)) {
                return false;
            }
            append_cross_source_version_lock_line(
                output,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the service name
                    // "AUR".
                    "    observed {} replacement: a matching candidate was not found",
                    AUR_SERVICE));
            return true;
        case CrossSourceVersionLockStatus::Unknown:
            append_cross_source_version_lock_line(
                output,
                localization::translate_message(
                    "    replacement metadata: compatibility could not be determined"));
            return true;
        case CrossSourceVersionLockStatus::QueryFailure:
            if(!std::holds_alternative<AurReplacementCandidateQueryFailure>(
                   assessment.evidence.aur_replacement)) {
                return false;
            }
            append_cross_source_version_lock_line(
                output,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the service name
                    // "AUR".
                    "    observed {} replacement: metadata could not be queried",
                    AUR_SERVICE));
            return true;
        case CrossSourceVersionLockStatus::Ambiguous:
            append_cross_source_version_lock_line(
                output,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the service name
                    // "AUR".
                    "    observed {} replacement: evidence is ambiguous",
                    AUR_SERVICE));
            return true;
    }
    return false;
}

std::optional<std::string>
project_cross_source_version_lock_correlation_presentation(
    const UpgradeAllOperationResult& result) {
    if(result.status != UpgradeAllOperationStatus::StoppedOnSystemFailure ||
       !result.cross_source_version_lock_correlation.has_value()) {
        return std::nullopt;
    }

    const UpgradeAllCrossSourceVersionLockCorrelationResult& correlation =
        result.cross_source_version_lock_correlation.value();
    if(correlation.failure.has_value() || !correlation.observation.has_value() ||
       correlation.possible_blocker_assessment_indices.empty()) {
        return std::nullopt;
    }

    const CrossSourceVersionLockObservationStatus observation_status =
        correlation.observation->status;
    if(observation_status != CrossSourceVersionLockObservationStatus::Complete &&
       observation_status != CrossSourceVersionLockObservationStatus::Partial) {
        // Failed observation currently returns before candidate assessment. Do
        // not turn an incoherent synthetic index into public evidence.
        return std::nullopt;
    }

    const std::size_t candidate_count =
        correlation.possible_blocker_assessment_indices.size();
    const unsigned long plural_count =
        static_cast<unsigned long>(candidate_count);
    std::string output = "\n";
    // TRANSLATORS: The first placeholder is the service name "AUR"; the
    // second is the number of possible metadata correlations shown below.
    append_cross_source_version_lock_line(
        output,
        localization::format_translated_plural_message(
            "Possible repository/{} cross-source version-lock candidate: {}",
            "Possible repository/{} cross-source version-lock candidates: {}",
            plural_count, AUR_SERVICE, candidate_count));

    // POLICY(#460): Slice 4's index vector is the sole public-inclusion
    // authority. Presentation must not recompute or strengthen blocker status.
    std::set<std::size_t> rendered_indices;
    for(const std::size_t assessment_index :
        correlation.possible_blocker_assessment_indices) {
        if(!rendered_indices.insert(assessment_index).second) {
            return std::nullopt;
        }
        const CrossSourceVersionLockAssessment& assessment =
            correlation.assessments.at(assessment_index);
        const RepositoryUpgradeCandidate& repository_upgrade =
            assessment.evidence.repository_upgrade;
        const RepositoryPackagePresent& repository_candidate =
            repository_upgrade.repository_candidate;
        const InstalledCrossSourceVersionLockConsumer& installed_consumer =
            assessment.evidence.installed_consumer;
        const std::string* installed_repository_version =
            repository_upgrade.installed_package.observed_version.version();
        const std::string* repository_candidate_version =
            repository_candidate.package_version.has_value()
                ? repository_candidate.package_version->version()
                : nullptr;
        const std::string* installed_consumer_version =
            installed_consumer.package.package_version.version();
        if(repository_candidate.package_name.empty() ||
           repository_candidate.repository_name.empty() ||
           installed_consumer.package.package_name.empty() ||
           installed_repository_version == nullptr ||
           repository_candidate_version == nullptr ||
           installed_consumer_version == nullptr) {
            return std::nullopt;
        }

        append_cross_source_version_lock_line(output, "");
        // TRANSLATORS: The placeholder is a repository package name.
        append_cross_source_version_lock_line(
            output,
            localization::format_translated_message(
                "  - repository package: {}",
                repository_candidate.package_name));
        // TRANSLATORS: The placeholder is the installed package version.
        append_cross_source_version_lock_line(
            output,
            localization::format_translated_message(
                "    installed version: {}",
                *installed_repository_version));
        // TRANSLATORS: The placeholders are a repository package name, its
        // observed version, and the configured repository name.
        append_cross_source_version_lock_line(
            output,
            localization::format_translated_message(
                "    observed repository candidate: {} {} (repository: {})",
                repository_candidate.package_name,
                *repository_candidate_version,
                repository_candidate.repository_name));
        // TRANSLATORS: The placeholders are an installed foreign package name
        // and version. "foreign" does not assert historical AUR provenance.
        append_cross_source_version_lock_line(
            output,
            localization::format_translated_message(
                "    installed foreign package: {} {}",
                installed_consumer.package.package_name,
                *installed_consumer_version));
        // TRANSLATORS: The placeholder is one validated installed dependency
        // expression.
        append_cross_source_version_lock_line(
            output,
            localization::format_translated_message(
                "    installed requirement: {}",
                installed_consumer.requirement.raw_specification()));
        if(!append_cross_source_version_lock_replacement(output, assessment)) {
            return std::nullopt;
        }
    }

    if(observation_status ==
       CrossSourceVersionLockObservationStatus::Partial) {
        append_cross_source_version_lock_line(output, "");
        append_cross_source_version_lock_line(
            output,
            localization::translate_message(
                "  The supplemental candidate observation was incomplete."));
    }

    append_cross_source_version_lock_line(output, "");
    append_cross_source_version_lock_line(
        output,
        localization::translate_message(
            "The observed repository candidate is metadata evidence only; this correlation does not identify the cause of the system update failure."));
    append_cross_source_version_lock_line(
        output,
        localization::format_translated_message(
            // TRANSLATORS: The placeholders are the project name
            // "Moguet" and service name "AUR".
            "{} did not perform a coordinated repository/{} update; review the displayed versions and dependency constraints manually.",
            application_identity::PROJECT_NAME, AUR_SERVICE));
    return output;
}

void print_cross_source_version_lock_correlation(
    const UpgradeAllOperationResult& result) noexcept {
    try {
        const std::optional<std::string> presentation =
            project_cross_source_version_lock_correlation_presentation(
                result);
        if(presentation.has_value()) {
            std::cout << presentation.value() << std::flush;
        }
    } catch(...) {
        // POLICY(#460): This projection is secondary evidence after the primary
        // system failure. Formatting must never replace or suppress that error.
    }
}

void print_duplicate_exclusions(const UpgradeAllOperationResult& result) {
    for(const UpgradeAllDuplicateExcludedAurTarget& exclusion :
        result.duplicate_excluded_aur_targets) {
        const std::string reason = duplicate_reason_label(
            exclusion.planner_entry.disposition);
        // TRANSLATORS: The first placeholder is the literal service name
        // "AUR"; the second is a package name.
        std::cout << localization::format_translated_message(
                         "excluded from {} update: {}", AUR_SERVICE,
                         exclusion.query_entry.installed_name)
                  << std::endl;
        std::cout << localization::format_translated_message(
                         "reason: {}", reason)
                  << std::endl;

        if(!exclusion.planner_entry.explicit_source.has_value()) {
            throw std::logic_error(localization::translate_message(
                "The duplicate exclusion has no explicit source attribution."));
        }
        const UpgradeAllExplicitSourceAttribution& attribution =
            *exclusion.planner_entry.explicit_source;
        if(attribution.matched_package_name.has_value()) {
            std::cout << localization::format_translated_message(
                             "matched explicit source package: {}",
                             *attribution.matched_package_name)
                      << std::endl;
        }
        if(attribution.matched_package_base.has_value()) {
            // TRANSLATORS: The first placeholder is the literal field name
            // "PackageBase"; the second is its runtime value.
            std::cout << localization::format_translated_message(
                             "matched {}: {}", PACKAGE_BASE_FIELD,
                             *attribution.matched_package_base)
                      << std::endl;
        }
        for(const std::string& identity : attribution.source_identity_keys) {
            std::cout << localization::format_translated_message(
                             "canonical source identity: {}", identity)
                      << std::endl;
        }
    }
}

void print_external_satisfaction(const UpgradeAllOperationResult& result) {
    for(const UpgradeAllExternallySatisfiedAurBuildUnit& external :
        result.externally_satisfied_aur_build_units) {
        const AurUpdateExternallySatisfiedBuildUnit& unit =
            external.operation_unit;
        if(unit.external_satisfaction.source_identity_keys.empty()) {
            throw std::logic_error(localization::translate_message(
                "External satisfaction has no explicit source identity."));
        }
        std::vector<std::string> role_labels;
        role_labels.reserve(external.root_correlations.size());
        for(const FilteredAurUpdateBuildUnitRootCorrelation& root :
            external.root_correlations) {
            role_labels.push_back(build_unit_role_label(root.role));
        }

        // TRANSLATORS: The first placeholder is the literal service name
        // "AUR"; the second is a package-base identity.
        std::cout << localization::format_translated_message(
                         "{} build unit externally satisfied: {}",
                         AUR_SERVICE, unit.package_base)
                  << std::endl;
        std::cout << localization::format_translated_message(
                         "provided by explicit source preference: {}",
                         join_strings(
                             unit.external_satisfaction.source_identity_keys))
                  << std::endl;
        if(unit.external_satisfaction.matched_package_name.has_value()) {
            std::cout << localization::format_translated_message(
                             "matched explicit source package: {}",
                             *unit.external_satisfaction.matched_package_name)
                      << std::endl;
        }
        if(unit.external_satisfaction.matched_package_base.has_value()) {
            // TRANSLATORS: The first placeholder is the literal field name
            // "PackageBase"; the second is its runtime value.
            std::cout << localization::format_translated_message(
                             "matched {}: {}", PACKAGE_BASE_FIELD,
                             *unit.external_satisfaction.matched_package_base)
                      << std::endl;
        }
        for(std::size_t index = 0;
            index < external.root_correlations.size(); ++index) {
            const FilteredAurUpdateBuildUnitRootCorrelation& root =
                external.root_correlations[index];
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the others are a package name and localized role.
            std::cout << localization::format_translated_message(
                             "affected {} root: {} ({})", AUR_SERVICE,
                             root.preflight_root.requested_name,
                             role_labels[index])
                      << std::endl;
        }
    }
}

std::string diagnostic_key(
    std::string_view phase, const std::string& diagnostic) {
    return std::string(phase) + "\n" + diagnostic;
}

void print_system_warnings(
    const SystemSourceUpgradeResult& result,
    std::set<std::string>& printed_warning_keys) {
    for(const SystemSourceUpgradeWarning& warning : result.warnings) {
        const std::string package_name =
            warning.preference_package_name.value_or(
                localization::translate_message("unknown preference"));
        Logger::warn(localization::format_translated_message(
            "system/source warning: {}: {}: {}",
            system_warning_kind_label(warning.kind), package_name,
            warning.diagnostic));
        printed_warning_keys.insert(package_name + "\n" + warning.diagnostic);
    }
}

void print_system_reviewed_source_outcomes(
    const SystemSourceUpgradeResult& result) {
    for(const RegisteredSourceUpgradeResult& source :
        result.registered_source_results) {
        if(!source.production_outcome.has_value()) continue;
        const std::string& package_base =
            source.resolved_package_base.has_value()
                ? *source.resolved_package_base
                : source.preference_package_name;
        ReviewedSourceProductionOutcomePresentation presentation =
            format_production_source_build_staged_outcome(
                package_base, *source.production_outcome);
        for(const std::string& line : presentation.info_lines) {
            std::cout << line << std::endl;
        }
    }
}

void print_system_failures(const SystemSourceUpgradeResult& result) {
    if(result.system.diagnostic.has_value() &&
       !result.system.diagnostic->empty()) {
        Logger::error(localization::format_translated_message(
            "system failure: {}", *result.system.diagnostic));
    }
    for(const RegisteredSourceUpgradeResult& source :
        result.registered_source_results) {
        if((source.status == RegisteredSourceUpgradeStatus::Failed ||
            source.status == RegisteredSourceUpgradeStatus::Unsupported ||
            source.status == RegisteredSourceUpgradeStatus::Incomplete) &&
           source.diagnostic.has_value() && !source.diagnostic->empty()) {
            Logger::error(localization::format_translated_message(
                "registered source failure: {}: {}",
                source.preference_package_name, *source.diagnostic));
        }
        if((source.status ==
                RegisteredSourceUpgradeStatus::UpdatedCleanupFailed ||
            source.status ==
                RegisteredSourceUpgradeStatus::NoChangeCleanupFailed) &&
           source.cleanup_diagnostic.has_value() &&
           !source.cleanup_diagnostic->empty()) {
            Logger::error(localization::format_translated_message(
                "registered source cleanup failure: {}: {}",
                source.preference_package_name,
                *source.cleanup_diagnostic));
        }
    }
}

void print_system_issues_and_diagnostics(
    const SystemSourceUpgradeResult& result) {
    std::set<std::string> issue_diagnostics;
    for(const SystemSourceUpgradeIssue& issue : result.issues) {
        const std::string kind = system_issue_kind_label(issue.kind);
        const std::string impact = system_issue_impact_label(issue.impact);
        const std::string phase = system_source_phase_label(issue.phase);
        const bool has_package = issue.preference_package_name.has_value();
        const bool has_metadata = issue.package_metadata_failure.has_value();
        const bool has_diagnostic = !issue.diagnostic.empty();
        const std::string package_display =
            has_package
                ? terminal_safe_runtime_diagnostic_detail(
                      *issue.preference_package_name)
                : std::string{};
        const std::string diagnostic_display =
            has_diagnostic
                ? terminal_safe_runtime_diagnostic_detail(issue.diagnostic)
                : std::string{};
        std::string message;
        if(has_package && has_metadata && has_diagnostic) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}): {} [{}]: {}", kind,
                impact, phase, package_display,
                package_metadata_error_label(
                    issue.package_metadata_failure->code),
                diagnostic_display);
        } else if(has_package && has_metadata) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}): {} [{}]", kind,
                impact, phase, package_display,
                package_metadata_error_label(
                    issue.package_metadata_failure->code));
        } else if(has_package && has_diagnostic) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}): {}: {}", kind,
                impact, phase, package_display,
                diagnostic_display);
        } else if(has_metadata && has_diagnostic) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}) [{}]: {}", kind,
                impact, phase,
                package_metadata_error_label(
                    issue.package_metadata_failure->code),
                diagnostic_display);
        } else if(has_package) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}): {}", kind, impact,
                phase, package_display);
        } else if(has_metadata) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}) [{}]", kind, impact,
                phase,
                package_metadata_error_label(
                    issue.package_metadata_failure->code));
        } else if(has_diagnostic) {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {}): {}", kind, impact,
                phase, diagnostic_display);
        } else {
            message = localization::format_translated_message(
                "system/source issue: {} ({}, {})", kind, impact, phase);
        }
        report_runtime_diagnostic(RuntimeDiagnosticPresentation{
            system_issue_severity(issue.impact), std::move(message)});
        issue_diagnostics.insert(diagnostic_key(
            system_source_phase_label(issue.phase), issue.diagnostic));
    }

    for(const SystemSourceUpgradeDiagnostic& diagnostic :
        result.diagnostics) {
        if(diagnostic.diagnostic.empty() ||
           issue_diagnostics.contains(diagnostic_key(
               system_source_phase_label(diagnostic.phase),
               diagnostic.diagnostic))) {
            continue;
        }
        Logger::error(localization::format_translated_message(
            "system/source diagnostic: {}: {}",
            system_source_phase_label(diagnostic.phase),
            diagnostic.diagnostic));
    }
}

void print_aur_query_failures(const FilteredAurUpdateExecutionResult& result) {
    for(const AurUpdateQueryFailure& failure :
        result.query_result.recoverable_failures) {
        // TRANSLATORS: The first placeholder is the literal service name
        // "AUR"; the others are package names and a runtime diagnostic.
        Logger::error(localization::format_translated_message(
            "{} query failure for {}: {}", AUR_SERVICE,
            join_query_package_names(failure.package_names),
            failure.diagnostic));
    }
}

void print_planning_issues(const UpgradeAllPlan& plan) {
    for(const UpgradeAllPlanningIssue& issue : plan.issues) {
        const std::string kind = planning_issue_kind_label(issue.kind);
        std::string message;
        if(issue.package_name.has_value() && issue.package_base.has_value()) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the fourth is the literal field name "PackageBase".
            message = localization::format_translated_message(
                "{} planner issue: {}: package {}: {} {}", AUR_SERVICE,
                kind, *issue.package_name, PACKAGE_BASE_FIELD,
                *issue.package_base);
        } else if(issue.package_name.has_value()) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the others are a localized kind and package name.
            message = localization::format_translated_message(
                "{} planner issue: {}: package {}", AUR_SERVICE, kind,
                *issue.package_name);
        } else if(issue.package_base.has_value()) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the third is the literal field name "PackageBase".
            message = localization::format_translated_message(
                "{} planner issue: {}: {} {}", AUR_SERVICE, kind,
                PACKAGE_BASE_FIELD, *issue.package_base);
        } else {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the second is a localized issue kind.
            message = localization::format_translated_message(
                "{} planner issue: {}", AUR_SERVICE, kind);
        }
        Logger::error(message);
    }
}

void print_filtered_mapping_issues(
    const FilteredAurUpdateExecutionResult& result) {
    for(const FilteredAurUpdateOperationIssue& issue : result.issues) {
        const std::string kind = filtered_issue_kind_label(issue.kind);
        const bool has_package = issue.package_name.has_value();
        const bool has_base = issue.package_base.has_value();
        const bool has_diagnostic = !issue.diagnostic.empty();
        std::string message;
        if(has_package && has_base && has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the fourth is the literal field name "PackageBase".
            message = localization::format_translated_message(
                "{} mapping issue: {}: package {}: {} {}: {}", AUR_SERVICE,
                kind, *issue.package_name, PACKAGE_BASE_FIELD,
                *issue.package_base, issue.diagnostic);
        } else if(has_package && has_base) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the fourth is the literal field name "PackageBase".
            message = localization::format_translated_message(
                "{} mapping issue: {}: package {}: {} {}", AUR_SERVICE,
                kind, *issue.package_name, PACKAGE_BASE_FIELD,
                *issue.package_base);
        } else if(has_package && has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the others are a localized kind and runtime data.
            message = localization::format_translated_message(
                "{} mapping issue: {}: package {}: {}", AUR_SERVICE, kind,
                *issue.package_name, issue.diagnostic);
        } else if(has_base && has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the third is the literal field name "PackageBase".
            message = localization::format_translated_message(
                "{} mapping issue: {}: {} {}: {}", AUR_SERVICE, kind,
                PACKAGE_BASE_FIELD, *issue.package_base,
                issue.diagnostic);
        } else if(has_package) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the others are a localized kind and package name.
            message = localization::format_translated_message(
                "{} mapping issue: {}: package {}", AUR_SERVICE, kind,
                *issue.package_name);
        } else if(has_base) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the third is the literal field name "PackageBase".
            message = localization::format_translated_message(
                "{} mapping issue: {}: {} {}", AUR_SERVICE, kind,
                PACKAGE_BASE_FIELD, *issue.package_base);
        } else if(has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the others are a localized kind and runtime diagnostic.
            message = localization::format_translated_message(
                "{} mapping issue: {}: {}", AUR_SERVICE, kind,
                issue.diagnostic);
        } else {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the second is a localized issue kind.
            message = localization::format_translated_message(
                "{} mapping issue: {}", AUR_SERVICE, kind);
        }
        Logger::error(message);
    }
}

void print_aur_preflight_issues(const AurUpdateOperationResult& result) {
    for(const AurUpdateOperationTargetResult& target : result.targets) {
        for(const AurUpdateExecutionIssue& issue : target.preflight_issues) {
            if(is_routine_aur_update_skip(issue.reason)) continue;
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the others are a localized reason and diagnostic.
            Logger::error(localization::format_translated_message(
                "{} preflight issue: {}: {}", AUR_SERVICE,
                aur_update_preflight_reason_label(issue.reason),
                issue.diagnostic));
        }
    }
}

void print_aur_preparation_details(
    const AurUpdateOperationResult& result,
    std::set<std::string>& printed_warning_keys) {
    for(const AurUpdatePreparationWarning& warning :
        result.preparation_warnings) {
        const std::string package_name = warning.preference_name.empty()
                                             ? localization::translate_message("unknown preference")
                                             : warning.preference_name;
        // TRANSLATORS: The first placeholder is the literal service name
        // "AUR"; the others are a package name and runtime diagnostic.
        Logger::warn(localization::format_translated_message(
            "{} preparation warning: {}: {}", AUR_SERVICE, package_name,
            warning.diagnostic));
        printed_warning_keys.insert(package_name + "\n" + warning.diagnostic);
    }
    for(const AurUpdatePreparationIssue& issue : result.preparation_issues) {
        // TRANSLATORS: The first placeholder is the literal service name
        // "AUR"; the others are a localized reason and diagnostic.
        Logger::error(localization::format_translated_message(
            "{} preparation issue: {}: {}", AUR_SERVICE,
            aur_update_preparation_reason_label(issue.reason),
            issue.diagnostic));
    }
}

void print_aur_reduction_issues(const AurUpdateOperationResult& result) {
    for(const AurUpdateOperationReductionIssue& issue :
        result.reduction_issues) {
        // TRANSLATORS: The first placeholder is the literal service name
        // "AUR"; the others are localized details and a runtime diagnostic.
        Logger::error(localization::format_translated_message(
            "{} reduction issue: {}: {}: {}", AUR_SERVICE,
            reduction_stage_label(issue.stage),
            reduction_reason_label(issue.reason), issue.diagnostic));
    }
}

void print_adapter_issues(const UpgradeAllOperationResult& result) {
    for(const UpgradeAllExplicitSourceAdapterIssue& issue :
        result.prepared_snapshot.explicit_source_adapter.issues) {
        const std::string kind = adapter_issue_kind_label(issue.kind);
        std::string message;
        if(issue.preference_package_name.has_value() &&
           !issue.diagnostic.empty()) {
            message = localization::format_translated_message(
                "explicit source adapter issue: {}: {}: {}", kind,
                *issue.preference_package_name, issue.diagnostic);
        } else if(issue.preference_package_name.has_value()) {
            message = localization::format_translated_message(
                "explicit source adapter issue: {}: {}", kind,
                *issue.preference_package_name);
        } else if(!issue.diagnostic.empty()) {
            message = localization::format_translated_message(
                "explicit source adapter issue: {}: {}", kind,
                issue.diagnostic);
        } else {
            message = localization::format_translated_message(
                "explicit source adapter issue: {}", kind);
        }
        Logger::error(message);
    }
}

bool has_foreign_inventory_failure(
    const UpgradeAllForeignInventoryPhaseResult& inventory) noexcept {
    return inventory.status == UpgradeAllForeignInventoryPhaseStatus::Failed ||
           inventory.failure.has_value() || inventory.diagnostic.has_value();
}

std::set<std::string> print_foreign_inventory_failure(
    const UpgradeAllForeignInventoryPhaseResult& inventory) {
    std::set<std::string> printed_diagnostics;
    if(!has_foreign_inventory_failure(inventory)) {
        return printed_diagnostics;
    }

    if(inventory.failure.has_value()) {
        std::string message;
        if(!inventory.failure->diagnostic.empty()) {
            message = localization::format_translated_message(
                "foreign inventory failure [{}]: {}",
                package_metadata_error_label(inventory.failure->code),
                inventory.failure->diagnostic);
            printed_diagnostics.insert(inventory.failure->diagnostic);
        } else {
            message = localization::format_translated_message(
                "foreign inventory failure [{}]",
                package_metadata_error_label(inventory.failure->code));
        }
        Logger::error(message);
    }

    if(inventory.diagnostic.has_value() &&
       !inventory.diagnostic->empty() &&
       !printed_diagnostics.contains(*inventory.diagnostic)) {
        Logger::error(localization::format_translated_message(
            "foreign inventory diagnostic: {}", *inventory.diagnostic));
        printed_diagnostics.insert(*inventory.diagnostic);
    }

    if(!inventory.failure.has_value() && printed_diagnostics.empty()) {
        Logger::error(localization::translate_message(
            "foreign inventory failure: diagnostic unavailable"));
    }
    return printed_diagnostics;
}

void print_aggregate_warnings(
    const UpgradeAllOperationResult& result,
    std::set<std::string>& printed_warning_keys) {
    for(const UpgradeAllOperationWarning& warning : result.warnings) {
        const std::string package_name =
            warning.package_name.value_or(
                localization::translate_message("aggregate"));
        const std::string key = package_name + "\n" + warning.diagnostic;
        if(printed_warning_keys.contains(key)) continue;
        // TRANSLATORS: The first placeholder is the literal command name
        // "upgrade-all"; the others are localized or runtime details.
        Logger::warn(localization::format_translated_message(
            "{} warning: {} ({}): {}: {}", COMMAND_NAME,
            aggregate_warning_kind_label(warning.kind),
            aggregate_phase_label(warning.phase), package_name,
            warning.diagnostic));
        printed_warning_keys.insert(key);
    }
}

void print_aggregate_issues_and_diagnostics(
    const UpgradeAllOperationResult& result,
    const std::set<std::string>& already_reported_diagnostics) {
    const bool has_typed_aur_execution_snapshot =
        result.aur.operation_result.has_value();
    std::set<std::string> issue_diagnostics;
    for(const UpgradeAllOperationIssue& issue : result.issues) {
        const bool already_reported =
            issue.phase == UpgradeAllOperationPhase::ForeignInventory &&
            !issue.diagnostic.empty() &&
            already_reported_diagnostics.contains(issue.diagnostic);
        const bool suppress_raw_aur_execution_diagnostic =
            has_typed_aur_execution_snapshot &&
            issue.phase == UpgradeAllOperationPhase::AurExecution;
        const std::string kind = aggregate_issue_kind_label(issue.kind);
        const std::string phase = aggregate_phase_label(issue.phase);
        const bool has_package = issue.package_name.has_value();
        const bool has_metadata = issue.package_metadata_failure.has_value();
        const bool has_diagnostic = !issue.diagnostic.empty() &&
                                    !suppress_raw_aur_execution_diagnostic;
        std::string message;
        if(has_package && has_metadata && has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized or runtime details.
            message = localization::format_translated_message(
                "{} issue: {} ({}): {} [{}]: {}", COMMAND_NAME, kind,
                phase, *issue.package_name,
                package_metadata_error_label(
                    issue.package_metadata_failure->code),
                issue.diagnostic);
        } else if(has_package && has_metadata) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized or runtime details.
            message = localization::format_translated_message(
                "{} issue: {} ({}): {} [{}]", COMMAND_NAME, kind, phase,
                *issue.package_name,
                package_metadata_error_label(
                    issue.package_metadata_failure->code));
        } else if(has_package && has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized or runtime details.
            message = localization::format_translated_message(
                "{} issue: {} ({}): {}: {}", COMMAND_NAME, kind, phase,
                *issue.package_name, issue.diagnostic);
        } else if(has_metadata && has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized or runtime details.
            message = localization::format_translated_message(
                "{} issue: {} ({}) [{}]: {}", COMMAND_NAME, kind, phase,
                package_metadata_error_label(
                    issue.package_metadata_failure->code),
                issue.diagnostic);
        } else if(has_package) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized or runtime details.
            message = localization::format_translated_message(
                "{} issue: {} ({}): {}", COMMAND_NAME, kind, phase,
                *issue.package_name);
        } else if(has_metadata) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized details.
            message = localization::format_translated_message(
                "{} issue: {} ({}) [{}]", COMMAND_NAME, kind, phase,
                package_metadata_error_label(
                    issue.package_metadata_failure->code));
        } else if(has_diagnostic) {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized or runtime details.
            message = localization::format_translated_message(
                "{} issue: {} ({}): {}", COMMAND_NAME, kind, phase,
                issue.diagnostic);
        } else {
            // TRANSLATORS: The first placeholder is the literal command name
            // "upgrade-all"; the others are localized details.
            message = localization::format_translated_message(
                "{} issue: {} ({})", COMMAND_NAME, kind, phase);
        }
        if(!already_reported) Logger::error(message);
        issue_diagnostics.insert(diagnostic_key(
            aggregate_phase_label(issue.phase), issue.diagnostic));
    }

    if(!has_typed_aur_execution_snapshot &&
       result.aur.diagnostic.has_value() && !result.aur.diagnostic->empty()) {
        const bool already_reported = std::any_of(
            result.issues.begin(), result.issues.end(),
            [&](const UpgradeAllOperationIssue& issue) {
                return issue.diagnostic == *result.aur.diagnostic;
            });
        if(!already_reported) {
            // TRANSLATORS: The first placeholder is the literal service name
            // "AUR"; the second is a runtime diagnostic.
            Logger::error(localization::format_translated_message(
                "{} phase diagnostic: {}", AUR_SERVICE,
                *result.aur.diagnostic));
        }
    }

    for(const UpgradeAllOperationDiagnostic& diagnostic :
        result.diagnostics) {
        if(diagnostic.diagnostic.empty() ||
           (has_typed_aur_execution_snapshot &&
            diagnostic.phase == UpgradeAllOperationPhase::AurExecution) ||
           (diagnostic.phase == UpgradeAllOperationPhase::ForeignInventory &&
            already_reported_diagnostics.contains(diagnostic.diagnostic)) ||
           issue_diagnostics.contains(diagnostic_key(
               aggregate_phase_label(diagnostic.phase),
               diagnostic.diagnostic))) {
            continue;
        }
        // TRANSLATORS: The first placeholder is the literal command name
        // "upgrade-all"; the others are a localized phase and diagnostic.
        Logger::error(localization::format_translated_message(
            "{} diagnostic: {}: {}", COMMAND_NAME,
            aggregate_phase_label(diagnostic.phase),
            diagnostic.diagnostic));
    }
}

void print_details(
    const UpgradeAllOperationResult& result,
    const AurUpdateCliPresentation* presentation) {
    std::set<std::string> printed_warning_keys;
    print_system_warnings(result.system_source, printed_warning_keys);
    print_system_failures(result.system_source);
    print_system_issues_and_diagnostics(result.system_source);
    print_adapter_issues(result);

    if(result.aur.operation_result.has_value()) {
        const FilteredAurUpdateExecutionResult& filtered =
            *result.aur.operation_result;
        print_aur_query_failures(filtered);
        print_planning_issues(filtered.upgrade_all_plan);
        print_filtered_mapping_issues(filtered);
        print_aur_preflight_issues(filtered.reduced_operation_result);
        print_aur_preparation_details(
            filtered.reduced_operation_result,
            printed_warning_keys);
        if(presentation != nullptr) {
            for(const std::string& line : presentation->error_lines) {
                Logger::error(line);
            }
        }
        print_aur_reduction_issues(filtered.reduced_operation_result);
    }

    const std::set<std::string> inventory_diagnostics =
        print_foreign_inventory_failure(result.foreign_inventory);
    print_aggregate_warnings(result, printed_warning_keys);
    print_aggregate_issues_and_diagnostics(result, inventory_diagnostics);
}

void print_operation_result(const UpgradeAllOperationResult& result) {
    // AUR child snapshotを最初に検証し、unknown enumやincoherent identityを
    // 成功済みsummaryへ混ぜずfail-closedにする。
    validate_aur_phase_presentation_boundary(result.aur);
    std::optional<AurUpdateCliPresentation> aur_presentation;
    if(result.aur.operation_result.has_value()) {
        aur_presentation.emplace(format_aur_update_cli_presentation(
            result.aur.operation_result->reduced_operation_result));
    }
    const AurUpdateCliPresentation* presentation =
        aur_presentation.has_value() ? &*aur_presentation : nullptr;

    const OperationStateProjection operation_state =
        project_upgrade_all_operation_state(result);
    const UpgradeAllPhasePackageStateObservations phase_observations =
        project_upgrade_all_phase_package_state_observations(result);
    const PresentationProjection runtime_presentation =
        project_upgrade_all_presentation_with_operation_state(
            result, operation_state);

    // POLICY(#350): successful operation state and package-state observation
    // remain orthogonal. Normal items are aggregated before attention detail.
    print_upgrade_all_summary(
        operation_state, phase_observations, runtime_presentation);
    print_system_reviewed_source_outcomes(result.system_source);
    if(presentation != nullptr) {
        for(const std::string& line : presentation->summary_lines) {
            std::cout << line << std::endl;
        }
    }
    print_upgrade_all_attention(runtime_presentation);
    print_cross_source_version_lock_correlation(result);
    print_duplicate_exclusions(result);
    print_external_satisfaction(result);
    print_details(result, presentation);

    if((result.status == UpgradeAllOperationStatus::Completed ||
        result.status == UpgradeAllOperationStatus::NoUpdates) &&
       !result.is_success()) {
        Logger::error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            "The {} result contains failure details despite a successful aggregate status.",
            COMMAND_NAME));
    }
}

bool is_supported_upgrade_all_global_option(const std::string& option) {
    const cli_authority::GlobalOptionSpec* spec =
        cli_authority::find_moguet_global_option(option);
    if(spec == nullptr) return false;

    switch(spec->id) {
        case cli_authority::GlobalOptionId::Edit:
        case cli_authority::GlobalOptionId::NoEdit:
        case cli_authority::GlobalOptionId::Diff:
        case cli_authority::GlobalOptionId::NoDiff:
        case cli_authority::GlobalOptionId::NoConfirm:
        case cli_authority::GlobalOptionId::DryRun:
        case cli_authority::GlobalOptionId::BuildMode:
        case cli_authority::GlobalOptionId::Rebuild:
        case cli_authority::GlobalOptionId::CleanBuild:
            return true;
        case cli_authority::GlobalOptionId::RmDeps:
        case cli_authority::GlobalOptionId::Select:
        case cli_authority::GlobalOptionId::Aur:
        case cli_authority::GlobalOptionId::Repo:
        case cli_authority::GlobalOptionId::Count:
            return false;
    }
    return false;
}

} // namespace

std::vector<std::string> validate_upgrade_all_invocation(
    const ParsedCliArguments& parsed) {
    if(parsed.operation !=
       cli_authority::operation_spec(
           cli_authority::OperationId::UpgradeAll)
           .token) {
        return {};
    }

    std::vector<std::string> errors;
    const CliInvocationValidation operand_validation =
        validate_cli_invocation_contract(parsed);
    if(!operand_validation.is_valid()) {
        errors.push_back(cli_invocation_issue_message(
            operand_validation.diagnostic->reason));
    }
    for(const ParsedCliToken& token : parsed.tokens) {
        switch(token.role) {
            case CliTokenRole::Operation:
                break;
            case CliTokenRole::MoguetGlobalOption:
                if(!is_supported_upgrade_all_global_option(token.value)) {
                    errors.push_back(
                        localization::format_translated_message(
                            // TRANSLATORS: The first placeholder is the
                            // literal command name "upgrade-all"; the
                            // second is a runtime option token.
                            "Unsupported {} option: {}", COMMAND_NAME,
                            token.value));
                }
                break;
            case CliTokenRole::Target:
                // Operand cardinality belongs to the shared structured CLI
                // authority above. Keep this pass operation-option-only.
                break;
            case CliTokenRole::OpaqueOperand:
                // `--`-separated operands are targets in the same authority.
                break;
            case CliTokenRole::PacmanOption:
            case CliTokenRole::PacmanOptionValue:
                errors.push_back(localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // command name "upgrade-all"; the second is a runtime
                    // option or operand.
                    "Unsupported {} option or operand: {}", COMMAND_NAME,
                    token.value));
                break;
            case CliTokenRole::EndOfOptions:
                // `--` itself carries no target; any following opaque operand gets
                // its own diagnostic. A bare marker is still unsupported.
                if(parsed.targets.empty()) {
                    errors.push_back(localization::format_translated_message(
                        // TRANSLATORS: The placeholders are the literal
                        // command name "upgrade-all" and CLI marker "--".
                        "{} does not accept the {} operand marker.",
                        COMMAND_NAME, "--"));
                }
                break;
        }
    }
    return errors;
}

int cmd_upgrade_all(const AppConfig& config) {
    try {
        UpgradeAllOperationPreparation preparation =
            prepare_upgrade_all_operation(config);
        UpgradeAllOperationResult result =
            std::holds_alternative<PreparedUpgradeAllOperation>(
                preparation)
                ? execute_prepared_upgrade_all_operation(
                      std::move(std::get<PreparedUpgradeAllOperation>(
                          preparation)),
                      config)
                : std::move(std::get<UpgradeAllOperationResult>(preparation));

        print_operation_result(result);
        return result.is_success() ? 0 : 1;
    } catch(const ConfirmationOperationStopped&) {
        return 1;
    } catch(const std::exception& error) {
        Logger::error(localization::format_translated_message(
            // TRANSLATORS: The first placeholder is the literal command
            // name "upgrade-all"; the second is a runtime diagnostic.
            "Unexpected {} command failure: {}", COMMAND_NAME,
            error.what()));
        return 1;
    } catch(...) {
        Logger::error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal command name
            // "upgrade-all".
            "Unexpected {} command failure: unknown exception.",
            COMMAND_NAME));
        return 1;
    }
}
