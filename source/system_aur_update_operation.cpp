#include "system_aur_update_operation.hpp"
#include "aur_devel_update.hpp"

#include "app_config.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_query.hpp"
#include "commands_sync.hpp"
#include "localization.hpp"

#include <algorithm>
#include <exception>
#include <utility>
#include <variant>

namespace {

bool has_reason(
    const std::optional<SystemAurUpdateNotAttemptedReason>& actual,
    SystemAurUpdateNotAttemptedReason expected) noexcept {
    return actual ==
           std::optional<SystemAurUpdateNotAttemptedReason>{expected};
}

bool filtered_result_has_inconsistency(
    const FilteredAurUpdateExecutionResult& filtered) noexcept {
    return !filtered.has_consistent_devel_requires_check_policy_snapshot() ||
           filtered.devel_requires_check_policy !=
               std::optional<DevelRequiresCheckPolicy>{
                   DevelRequiresCheckPolicy::SkipIndependentTarget} ||
           !filtered.issues.empty() ||
           !filtered.reduced_operation_result.reduction_issues.empty() ||
           filtered.reduced_operation_result.status ==
               AurUpdateOperationStatus::InconsistentResult;
}

SystemAurUpdateAurPhaseStatus projected_aur_phase_status(
    const FilteredAurUpdateExecutionResult& filtered) noexcept {
    if(filtered.has_query_failure()) {
        return SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution;
    }
    switch(filtered.reduced_operation_result.status) {
        case AurUpdateOperationStatus::NoUpdates:
            return SystemAurUpdateAurPhaseStatus::NoUpdates;
        case AurUpdateOperationStatus::Completed:
            return SystemAurUpdateAurPhaseStatus::Completed;
        case AurUpdateOperationStatus::BlockedBeforeExecution:
            return SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution;
        case AurUpdateOperationStatus::
            StoppedOnProviderTransactionFailure:
            return SystemAurUpdateAurPhaseStatus::
                StoppedOnProviderTransactionFailure;
        case AurUpdateOperationStatus::StoppedOnWorkItemCancellation:
            return SystemAurUpdateAurPhaseStatus::StoppedOnWorkItemCancellation;
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
            return SystemAurUpdateAurPhaseStatus::
                StoppedOnWorkItemFailure;
        case AurUpdateOperationStatus::
            StoppedAfterPackageCleanupFailure:
            return SystemAurUpdateAurPhaseStatus::
                StoppedAfterCleanupFailure;
        case AurUpdateOperationStatus::InconsistentResult:
            return SystemAurUpdateAurPhaseStatus::InconsistentResult;
    }
    return SystemAurUpdateAurPhaseStatus::InconsistentResult;
}

bool repository_request_matches(
    const SystemAurUpdateRepositoryPhaseResult& repository) noexcept {
    return repository.compatible_request.has_value() &&
           !repository.ordered_pacman_args.empty() &&
           repository.compatible_request->ordered_pacman_args() ==
               repository.ordered_pacman_args;
}

bool inventory_matches_query(
    const ForeignPackageInventory& inventory,
    const AurUpdateQueryResult& query) noexcept {
    if(inventory.size() != query.plan.entries.size()) return false;
    for(std::size_t index = 0; index < inventory.size(); ++index) {
        const InstalledPackageMetadata& installed = inventory[index];
        const AurUpdatePlanEntry& entry = query.plan.entries[index];
        if(installed.name != entry.installed_name ||
           installed.version != entry.installed_version ||
           installed.reason != entry.install_reason) {
            return false;
        }
    }
    return true;
}

bool inventory_not_attempted_with_reason(
    const SystemAurUpdateForeignInventoryPhaseResult& inventory,
    SystemAurUpdateNotAttemptedReason reason) noexcept {
    return inventory.status ==
               SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted &&
           has_reason(inventory.not_attempted_reason, reason) &&
           !inventory.repository_configuration.has_value() &&
           inventory.inventory.empty() && !inventory.failure.has_value() &&
           !inventory.diagnostic.has_value();
}

bool query_not_attempted_with_reason(
    const SystemAurUpdateQueryPhaseResult& query,
    SystemAurUpdateNotAttemptedReason reason) noexcept {
    return query.status ==
               SystemAurUpdateQueryPhaseStatus::NotAttempted &&
           has_reason(query.not_attempted_reason, reason) &&
           !query.query_result.has_value() &&
           !query.diagnostic.has_value();
}

bool aur_not_attempted_with_reason(
    const SystemAurUpdateAurPhaseResult& aur,
    SystemAurUpdateNotAttemptedReason reason) noexcept {
    return aur.status == SystemAurUpdateAurPhaseStatus::NotAttempted &&
           has_reason(aur.not_attempted_reason, reason) &&
           !aur.operation_result.has_value() &&
           !aur.diagnostic.has_value();
}

void mark_later_not_attempted(
    SystemAurUpdateOperationResult& result,
    SystemAurUpdateNotAttemptedReason reason) {
    result.query.status = SystemAurUpdateQueryPhaseStatus::NotAttempted;
    result.query.not_attempted_reason = reason;
    result.aur.status = SystemAurUpdateAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason = reason;
}

void mark_all_not_attempted_for_inconsistency(
    SystemAurUpdateOperationResult& result) {
    result.repository.status =
        SystemAurUpdateRepositoryPhaseStatus::NotAttempted;
    result.repository.not_attempted_reason =
        SystemAurUpdateNotAttemptedReason::PriorAggregateInconsistency;
    result.foreign_inventory.status =
        SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason =
        SystemAurUpdateNotAttemptedReason::PriorAggregateInconsistency;
    mark_later_not_attempted(
        result,
        SystemAurUpdateNotAttemptedReason::PriorAggregateInconsistency);
}

SystemAurUpdateOperationResult inconsistent_result(
    SystemAurUpdateOperationResult result,
    SystemAurUpdateOperationPhase stopped_phase) noexcept {
    result.status = SystemAurUpdateOperationStatus::InconsistentResult;
    result.stopped_phase = stopped_phase;
    if(result.aur.operation_result.has_value() ||
       result.aur.status != SystemAurUpdateAurPhaseStatus::NotAttempted) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
    }
    return result;
}

bool repository_failure_tail_is_consistent(
    const SystemAurUpdateOperationResult& result) noexcept {
    return inventory_not_attempted_with_reason(
               result.foreign_inventory,
               SystemAurUpdateNotAttemptedReason::RepositoryFailure) &&
           query_not_attempted_with_reason(
               result.query,
               SystemAurUpdateNotAttemptedReason::RepositoryFailure) &&
           aur_not_attempted_with_reason(
               result.aur,
               SystemAurUpdateNotAttemptedReason::RepositoryFailure);
}

bool inventory_failure_tail_is_consistent(
    const SystemAurUpdateOperationResult& result) noexcept {
    return query_not_attempted_with_reason(
               result.query,
               SystemAurUpdateNotAttemptedReason::ForeignInventoryFailure) &&
           aur_not_attempted_with_reason(
               result.aur,
               SystemAurUpdateNotAttemptedReason::ForeignInventoryFailure);
}

bool auto_dry_run_authority_is_complete(
    const SystemAurUpdateDryRunObservation& observation) noexcept {
    return observation.aur_observation_basis ==
               std::optional<SystemAurUpdateDryRunAurObservationBasis>{
                   SystemAurUpdateDryRunAurObservationBasis::
                       CurrentInstalledState} &&
           observation.actual_authority_refresh ==
               std::optional<SystemAurUpdateDryRunActualAuthorityRefresh>{
                   SystemAurUpdateDryRunActualAuthorityRefresh::
                       AfterRepositorySuccess} &&
           observation.explicit_source_satisfaction.has_value() &&
           observation.saved_source_preference_policy ==
               std::optional<SavedSourcePreferencePolicy>{
                   SavedSourcePreferencePolicy::Ignore} &&
           observation.devel_requires_check_policy ==
               std::optional<DevelRequiresCheckPolicy>{
                   DevelRequiresCheckPolicy::SkipIndependentTarget} &&
           observation.repository_configuration.has_value() &&
           observation.aur_observation.has_value() &&
           observation.aur_observation
               ->has_consistent_devel_requires_check_policy_snapshot() &&
           observation.aur_observation
                   ->devel_requires_check_policy ==
               observation.devel_requires_check_policy &&
           inventory_matches_query(
               observation.foreign_inventory,
               observation.aur_observation->original_query_result());
}

bool repo_only_dry_run_has_no_aur_authority(
    const SystemAurUpdateDryRunObservation& observation) noexcept {
    return !observation.aur_observation_basis.has_value() &&
           !observation.actual_authority_refresh.has_value() &&
           !observation.explicit_source_satisfaction.has_value() &&
           !observation.saved_source_preference_policy.has_value() &&
           !observation.devel_requires_check_policy.has_value() &&
           !observation.repository_configuration.has_value() &&
           observation.foreign_inventory.empty() &&
           !observation.aur_observation.has_value();
}

} // namespace

bool SystemAurUpdateDryRunObservation::is_ready() const noexcept {
    if(request.ordered_pacman_args().empty() || !issues.empty()) {
        return false;
    }
    if(request.mode() == SystemAurUpdateDryRunMode::RepoOnly) {
        return repo_only_dry_run_has_no_aur_authority(*this);
    }
    if(!auto_dry_run_authority_is_complete(*this)) {
        return false;
    }
    return aur_observation->is_ready() || aur_observation->is_noop();
}

bool SystemAurUpdateDryRunObservation::is_blocked() const noexcept {
    return !is_ready();
}

bool SystemAurUpdateDryRunObservation::has_current_aur_update_intent()
    const noexcept {
    return request.mode() == SystemAurUpdateDryRunMode::Auto &&
           aur_observation.has_value() &&
           has_executable_targets(
               aur_observation->execution_preflight());
}

SystemAurUpdateDryRunObservation observe_system_aur_update_dry_run(
    SystemAurUpdateDryRunRequest request,
    const AppConfig& config) {
    SystemAurUpdateDryRunObservation observation{std::move(request)};
    if(observation.request.mode() ==
       SystemAurUpdateDryRunMode::RepoOnly) {
        return observation;
    }

    observation.aur_observation_basis =
        SystemAurUpdateDryRunAurObservationBasis::CurrentInstalledState;
    observation.actual_authority_refresh =
        SystemAurUpdateDryRunActualAuthorityRefresh::AfterRepositorySuccess;
    observation.explicit_source_satisfaction.emplace();
    observation.saved_source_preference_policy =
        SavedSourcePreferencePolicy::Ignore;
    observation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;

    try {
        observation.repository_configuration =
            resolve_pacman_repository_configuration();
    } catch(const PackageMetadataError& error) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::
                RepositoryConfigurationFailure,
            error.failure(), error.what()});
        return observation;
    } catch(const std::exception& error) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::
                RepositoryConfigurationFailure,
            std::nullopt, error.what()});
        return observation;
    } catch(...) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::
                RepositoryConfigurationFailure,
            std::nullopt,
            localization::translate_message(
                "Repository configuration observation failed with an unknown exception.")});
        return observation;
    }

    ForeignPackageInventoryResult inventory_result;
    try {
        inventory_result = query_foreign_package_inventory(
            *observation.repository_configuration);
    } catch(const PackageMetadataError& error) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::ForeignInventoryFailure,
            error.failure(), error.what()});
        return observation;
    } catch(const std::exception& error) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::ForeignInventoryFailure,
            std::nullopt, error.what()});
        return observation;
    } catch(...) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::ForeignInventoryFailure,
            std::nullopt,
            localization::translate_message(
                "Foreign package inventory observation failed with an unknown exception.")});
        return observation;
    }
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&inventory_result);
       failure != nullptr) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::ForeignInventoryFailure,
            *failure, failure->diagnostic});
        return observation;
    }
    observation.foreign_inventory =
        std::get<ForeignPackageInventory>(std::move(inventory_result));

    std::optional<AurUpdateQueryResult> query_result;
    try {
        query_result = query_aur_updates_for_foreign_inventory(
            observation.foreign_inventory);
    } catch(const std::exception& error) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::AurQueryFailure,
            std::nullopt, error.what()});
        return observation;
    } catch(...) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::AurQueryFailure,
            std::nullopt,
            localization::format_translated_message(
                "{} query failed with an unknown exception.", "AUR")});
        return observation;
    }

    try {
        observation.aur_observation =
            observe_filtered_aur_update_operation(
                std::move(query_result.value()),
                NoExplicitSourceSatisfaction{},
                DevelRequiresCheckPolicy::SkipIndependentTarget,
                SavedSourcePreferencePolicy::Ignore, config);
    } catch(const std::exception& error) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::AurAssessmentFailure,
            std::nullopt, error.what()});
        return observation;
    } catch(...) {
        observation.issues.push_back(SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::AurAssessmentFailure,
            std::nullopt,
            localization::format_translated_message(
                "{} assessment failed with an unknown exception.", "AUR")});
        return observation;
    }

    return observation;
}

std::optional<CompatibleSystemAurUpdateRequest>
make_compatible_system_aur_update_request(
    AutoSystemUpdateRouteCandidate candidate) {
    if(!std::holds_alternative<
           CompatibleAutoSystemUpdatePacmanArguments>(
           candidate.pacman_compatibility) ||
       candidate.ordered_pacman_args.empty()) {
        return std::nullopt;
    }
    return CompatibleSystemAurUpdateRequest{
        std::move(candidate.ordered_pacman_args)};
}

PreparedSystemAurUpdateOperation::PreparedSystemAurUpdateOperation(
    PreparedSystemAurUpdateOperation&& other) noexcept
    : valid_(std::exchange(other.valid_, false)),
      request_(std::move(other.request_)) {
}

PreparedSystemAurUpdateOperation prepare_system_aur_update_operation(
    CompatibleSystemAurUpdateRequest request) {
    return PreparedSystemAurUpdateOperation{std::move(request)};
}

SystemAurUpdateOperationResult reduce_system_aur_update_result(
    SystemAurUpdateOperationResult result) noexcept {
    if(result.repository.status ==
       SystemAurUpdateRepositoryPhaseStatus::Failed) {
        if(result.repository.command_exit_status ==
               std::optional<int>{0} ||
           result.repository.not_attempted_reason.has_value() ||
           !repository_request_matches(result.repository) ||
           !repository_failure_tail_is_consistent(result)) {
            return inconsistent_result(
                std::move(result),
                SystemAurUpdateOperationPhase::Reduction);
        }
        result.status =
            SystemAurUpdateOperationStatus::StoppedOnRepositoryFailure;
        result.stopped_phase = SystemAurUpdateOperationPhase::Repository;
        return result;
    }
    if(result.repository.status !=
           SystemAurUpdateRepositoryPhaseStatus::Completed ||
       result.repository.not_attempted_reason.has_value() ||
       result.repository.command_exit_status != std::optional<int>{0} ||
       result.repository.diagnostic.has_value() ||
       !repository_request_matches(result.repository)) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }

    if(result.foreign_inventory.status ==
       SystemAurUpdateForeignInventoryPhaseStatus::Failed) {
        if(result.foreign_inventory.not_attempted_reason.has_value() ||
           !result.foreign_inventory.inventory.empty() ||
           !inventory_failure_tail_is_consistent(result)) {
            return inconsistent_result(
                std::move(result),
                SystemAurUpdateOperationPhase::Reduction);
        }
        result.status =
            SystemAurUpdateOperationStatus::StoppedBeforeAurExecution;
        result.stopped_phase =
            SystemAurUpdateOperationPhase::ForeignInventory;
        return result;
    }
    if(result.foreign_inventory.status !=
           SystemAurUpdateForeignInventoryPhaseStatus::Completed ||
       result.foreign_inventory.not_attempted_reason.has_value() ||
       !result.foreign_inventory.repository_configuration.has_value() ||
       result.foreign_inventory.failure.has_value() ||
       result.foreign_inventory.diagnostic.has_value()) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }

    if(result.query.status == SystemAurUpdateQueryPhaseStatus::Failed &&
       !result.query.query_result.has_value()) {
        if(result.query.not_attempted_reason.has_value() ||
           !aur_not_attempted_with_reason(
               result.aur,
               SystemAurUpdateNotAttemptedReason::AurQueryFailure)) {
            return inconsistent_result(
                std::move(result),
                SystemAurUpdateOperationPhase::Reduction);
        }
        result.status =
            SystemAurUpdateOperationStatus::StoppedBeforeAurExecution;
        result.stopped_phase = SystemAurUpdateOperationPhase::AurQuery;
        return result;
    }
    if(result.query.status ==
           SystemAurUpdateQueryPhaseStatus::NotAttempted ||
       result.query.not_attempted_reason.has_value() ||
       !result.query.query_result.has_value()) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }
    if(result.query.status != SystemAurUpdateQueryPhaseStatus::Completed &&
       result.query.status != SystemAurUpdateQueryPhaseStatus::Failed) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }
    const bool retained_query_has_failure =
        !result.query.query_result->recoverable_failures.empty();
    if((result.query.status == SystemAurUpdateQueryPhaseStatus::Failed) !=
           retained_query_has_failure ||
       (result.query.status == SystemAurUpdateQueryPhaseStatus::Completed &&
        result.query.diagnostic.has_value())) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }
    if(!inventory_matches_query(
           result.foreign_inventory.inventory,
           result.query.query_result.value())) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }

    if(!result.aur.operation_result.has_value()) {
        if(result.aur.status ==
               SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution &&
           !result.aur.not_attempted_reason.has_value()) {
            result.status =
                SystemAurUpdateOperationStatus::StoppedBeforeAurExecution;
            result.stopped_phase =
                SystemAurUpdateOperationPhase::AurPreparation;
            return result;
        }
        if(result.aur.status ==
           SystemAurUpdateAurPhaseStatus::InconsistentResult) {
            const bool has_known_failure_phase =
                result.stopped_phase ==
                    SystemAurUpdateOperationPhase::AurPreparation ||
                result.stopped_phase ==
                    SystemAurUpdateOperationPhase::AurExecution;
            const SystemAurUpdateOperationPhase failure_phase =
                has_known_failure_phase
                    ? result.stopped_phase
                    : SystemAurUpdateOperationPhase::Reduction;
            return inconsistent_result(
                std::move(result), failure_phase);
        }
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }

    FilteredAurUpdateExecutionResult& filtered =
        result.aur.operation_result.value();
    if(result.aur.not_attempted_reason.has_value() ||
       result.query.query_result.value() != filtered.query_result ||
       result.aur.diagnostic.has_value() ||
       filtered_result_has_inconsistency(filtered)) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }
    const SystemAurUpdateAurPhaseStatus projected_aur_status =
        projected_aur_phase_status(filtered);
    if(result.aur.status != SystemAurUpdateAurPhaseStatus::NotAttempted &&
       result.aur.status != projected_aur_status) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }
    const bool query_phase_failed =
        result.query.status == SystemAurUpdateQueryPhaseStatus::Failed;
    if(query_phase_failed != filtered.has_query_failure()) {
        return inconsistent_result(
            std::move(result),
            SystemAurUpdateOperationPhase::Reduction);
    }
    if(query_phase_failed) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution;
        result.status =
            SystemAurUpdateOperationStatus::StoppedBeforeAurExecution;
        result.stopped_phase = SystemAurUpdateOperationPhase::AurQuery;
        return result;
    }

    switch(filtered.reduced_operation_result.status) {
        case AurUpdateOperationStatus::NoUpdates:
            if(!filtered.is_success()) {
                return inconsistent_result(
                    std::move(result),
                    SystemAurUpdateOperationPhase::Reduction);
            }
            result.aur.status =
                SystemAurUpdateAurPhaseStatus::NoUpdates;
            result.status = SystemAurUpdateOperationStatus::Completed;
            result.stopped_phase = SystemAurUpdateOperationPhase::None;
            return result;
        case AurUpdateOperationStatus::Completed:
            if(!filtered.is_success()) {
                return inconsistent_result(
                    std::move(result),
                    SystemAurUpdateOperationPhase::Reduction);
            }
            result.aur.status =
                SystemAurUpdateAurPhaseStatus::Completed;
            result.status = SystemAurUpdateOperationStatus::Completed;
            result.stopped_phase = SystemAurUpdateOperationPhase::None;
            return result;
        case AurUpdateOperationStatus::BlockedBeforeExecution:
            result.aur.status =
                SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution;
            result.status =
                SystemAurUpdateOperationStatus::StoppedBeforeAurExecution;
            result.stopped_phase =
                SystemAurUpdateOperationPhase::AurPreparation;
            return result;
        case AurUpdateOperationStatus::
            StoppedOnProviderTransactionFailure:
            result.aur.status = SystemAurUpdateAurPhaseStatus::
                StoppedOnProviderTransactionFailure;
            result.status =
                SystemAurUpdateOperationStatus::StoppedOnAurFailure;
            result.stopped_phase =
                SystemAurUpdateOperationPhase::AurExecution;
            return result;
        case AurUpdateOperationStatus::StoppedOnWorkItemCancellation:
            result.aur.status = SystemAurUpdateAurPhaseStatus::StoppedOnWorkItemCancellation;
            result.status = SystemAurUpdateOperationStatus::StoppedOnAurCancellation;
            result.stopped_phase = SystemAurUpdateOperationPhase::AurExecution;
            return result;
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
            result.aur.status = SystemAurUpdateAurPhaseStatus::
                StoppedOnWorkItemFailure;
            result.status =
                SystemAurUpdateOperationStatus::StoppedOnAurFailure;
            result.stopped_phase =
                SystemAurUpdateOperationPhase::AurExecution;
            return result;
        case AurUpdateOperationStatus::
            StoppedAfterPackageCleanupFailure:
            result.aur.status = SystemAurUpdateAurPhaseStatus::
                StoppedAfterCleanupFailure;
            result.status = SystemAurUpdateOperationStatus::
                StoppedAfterAurCleanupFailure;
            result.stopped_phase =
                SystemAurUpdateOperationPhase::AurExecution;
            return result;
        case AurUpdateOperationStatus::InconsistentResult:
            return inconsistent_result(
                std::move(result),
                SystemAurUpdateOperationPhase::Reduction);
    }
    return inconsistent_result(
        std::move(result), SystemAurUpdateOperationPhase::Reduction);
}

SystemAurUpdateOperationResult
execute_prepared_system_aur_update_operation(
    PreparedSystemAurUpdateOperation prepared,
    const AppConfig& config) {
    SystemAurUpdateOperationResult result;
    if(!prepared.valid_) {
        mark_all_not_attempted_for_inconsistency(result);
        return reduce_system_aur_update_result(std::move(result));
    }
    prepared.valid_ = false;
    result.repository.ordered_pacman_args =
        prepared.request_.ordered_pacman_args();
    result.repository.compatible_request = prepared.request_;

    try {
        result.repository.command_exit_status =
            execute_ordered_repository_sync_transaction(
                result.repository.ordered_pacman_args, config);
    } catch(const std::exception& error) {
        result.repository.status =
            SystemAurUpdateRepositoryPhaseStatus::Failed;
        result.repository.diagnostic = error.what();
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted;
        result.foreign_inventory.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::RepositoryFailure;
        mark_later_not_attempted(
            result,
            SystemAurUpdateNotAttemptedReason::RepositoryFailure);
        return reduce_system_aur_update_result(std::move(result));
    } catch(...) {
        result.repository.status =
            SystemAurUpdateRepositoryPhaseStatus::Failed;
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted;
        result.foreign_inventory.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::RepositoryFailure;
        mark_later_not_attempted(
            result,
            SystemAurUpdateNotAttemptedReason::RepositoryFailure);
        return reduce_system_aur_update_result(std::move(result));
    }

    if(result.repository.command_exit_status != std::optional<int>{0}) {
        result.repository.status =
            SystemAurUpdateRepositoryPhaseStatus::Failed;
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted;
        result.foreign_inventory.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::RepositoryFailure;
        mark_later_not_attempted(
            result,
            SystemAurUpdateNotAttemptedReason::RepositoryFailure);
        return reduce_system_aur_update_result(std::move(result));
    }
    result.repository.status =
        SystemAurUpdateRepositoryPhaseStatus::Completed;

    try {
        result.foreign_inventory.repository_configuration =
            resolve_pacman_repository_configuration();
    } catch(const PackageMetadataError& error) {
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::Failed;
        result.foreign_inventory.failure = error.failure();
        result.foreign_inventory.diagnostic = error.what();
    } catch(const std::exception& error) {
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::Failed;
        result.foreign_inventory.diagnostic = error.what();
    } catch(...) {
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::Failed;
    }
    if(result.foreign_inventory.status ==
       SystemAurUpdateForeignInventoryPhaseStatus::Failed) {
        mark_later_not_attempted(
            result,
            SystemAurUpdateNotAttemptedReason::ForeignInventoryFailure);
        return reduce_system_aur_update_result(std::move(result));
    }

    ForeignPackageInventoryResult inventory_result;
    try {
        inventory_result = query_foreign_package_inventory(
            result.foreign_inventory.repository_configuration.value());
    } catch(const PackageMetadataError& error) {
        inventory_result = error.failure();
    } catch(const std::exception& error) {
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::Failed;
        result.foreign_inventory.diagnostic = error.what();
    } catch(...) {
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::Failed;
    }
    if(result.foreign_inventory.status ==
       SystemAurUpdateForeignInventoryPhaseStatus::Failed) {
        mark_later_not_attempted(
            result,
            SystemAurUpdateNotAttemptedReason::ForeignInventoryFailure);
        return reduce_system_aur_update_result(std::move(result));
    }
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&inventory_result)) {
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::Failed;
        result.foreign_inventory.failure = *failure;
        result.foreign_inventory.diagnostic = failure->diagnostic;
        mark_later_not_attempted(
            result,
            SystemAurUpdateNotAttemptedReason::ForeignInventoryFailure);
        return reduce_system_aur_update_result(std::move(result));
    }
    result.foreign_inventory.status =
        SystemAurUpdateForeignInventoryPhaseStatus::Completed;
    result.foreign_inventory.inventory =
        std::get<ForeignPackageInventory>(std::move(inventory_result));

    AurUpdateQueryResult query_result;
    try {
        query_result = query_aur_updates_for_foreign_inventory(
            result.foreign_inventory.inventory);
        observe_aur_devel_bootstrap_candidates(query_result, config);
    } catch(const std::exception& error) {
        result.query.status = SystemAurUpdateQueryPhaseStatus::Failed;
        result.query.diagnostic = error.what();
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::NotAttempted;
        result.aur.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::AurQueryFailure;
        return reduce_system_aur_update_result(std::move(result));
    } catch(...) {
        result.query.status = SystemAurUpdateQueryPhaseStatus::Failed;
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::NotAttempted;
        result.aur.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::AurQueryFailure;
        return reduce_system_aur_update_result(std::move(result));
    }
    result.query.status = query_result.recoverable_failures.empty()
                              ? SystemAurUpdateQueryPhaseStatus::Completed
                              : SystemAurUpdateQueryPhaseStatus::Failed;
    // The phase retains fresh query evidence while the filtered child consumes
    // an owned copy of that same post-repository snapshot.
    result.query.query_result = query_result;

    std::optional<PreparedFilteredAurUpdateOperation> filtered;
    try {
        filtered.emplace(prepare_filtered_aur_update_operation(
            std::move(query_result), NoExplicitSourceSatisfaction{},
            DevelRequiresCheckPolicy::SkipIndependentTarget,
            SavedSourcePreferencePolicy::Ignore, config));
    } catch(const std::logic_error& error) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
        result.stopped_phase =
            SystemAurUpdateOperationPhase::AurPreparation;
        result.aur.diagnostic = error.what();
        return reduce_system_aur_update_result(std::move(result));
    } catch(const std::exception& error) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution;
        result.aur.diagnostic = error.what();
        return reduce_system_aur_update_result(std::move(result));
    } catch(...) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
        result.stopped_phase =
            SystemAurUpdateOperationPhase::AurPreparation;
        return reduce_system_aur_update_result(std::move(result));
    }

    try {
        result.aur.operation_result.emplace(
            execute_prepared_filtered_aur_update_operation(
                std::move(filtered.value()), config));
    } catch(FilteredAurUpdateCancelled& stop) {
        result.aur.operation_result.emplace(std::move(stop).release_result());
        // Nested cancellation is authoritative; a generic aur.diagnostic would
        // contradict this child and be rejected by the reducer.
        return reduce_system_aur_update_result(std::move(result));
    } catch(const std::exception& error) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
        result.stopped_phase =
            SystemAurUpdateOperationPhase::AurExecution;
        result.aur.diagnostic = error.what();
        return reduce_system_aur_update_result(std::move(result));
    } catch(...) {
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
        result.stopped_phase =
            SystemAurUpdateOperationPhase::AurExecution;
        return reduce_system_aur_update_result(std::move(result));
    }
    return reduce_system_aur_update_result(std::move(result));
}

bool SystemAurUpdateOperationResult::is_success() const noexcept {
    const bool aur_completed =
        aur.status == SystemAurUpdateAurPhaseStatus::NoUpdates ||
        aur.status == SystemAurUpdateAurPhaseStatus::Completed;
    return status == SystemAurUpdateOperationStatus::Completed &&
           stopped_phase == SystemAurUpdateOperationPhase::None &&
           repository.status ==
               SystemAurUpdateRepositoryPhaseStatus::Completed &&
           repository.command_exit_status == std::optional<int>{0} &&
           !repository.not_attempted_reason.has_value() &&
           !repository.diagnostic.has_value() &&
           repository_request_matches(repository) &&
           foreign_inventory.status ==
               SystemAurUpdateForeignInventoryPhaseStatus::Completed &&
           foreign_inventory.repository_configuration.has_value() &&
           !foreign_inventory.failure.has_value() &&
           !foreign_inventory.not_attempted_reason.has_value() &&
           !foreign_inventory.diagnostic.has_value() &&
           query.status == SystemAurUpdateQueryPhaseStatus::Completed &&
           query.query_result.has_value() &&
           !query.not_attempted_reason.has_value() &&
           !query.diagnostic.has_value() && aur_completed &&
           aur.operation_result.has_value() &&
           !aur.not_attempted_reason.has_value() &&
           !aur.diagnostic.has_value() &&
           aur.operation_result->is_success() &&
           !has_cleanup_failure() && !has_query_failure() &&
           !has_not_attempted_phase() && !has_inconsistency();
}

PackageStateChange
SystemAurUpdateOperationResult::package_state_change() const noexcept {
    if(aur.operation_result.has_value() &&
       aur.operation_result->package_state_change() ==
           PackageStateChange::Changed) {
        return PackageStateChange::Changed;
    }
    // Repository before/after snapshots are intentionally absent in Slice 3.
    // Even an AUR NoUpdates result cannot prove the aggregate was a no-op.
    return PackageStateChange::Unknown;
}

bool SystemAurUpdateOperationResult::has_partial_completion()
    const noexcept {
    return !is_success() &&
           repository.status ==
               SystemAurUpdateRepositoryPhaseStatus::Completed;
}

bool SystemAurUpdateOperationResult::has_not_attempted_phase()
    const noexcept {
    return repository.status ==
               SystemAurUpdateRepositoryPhaseStatus::NotAttempted ||
           repository.not_attempted_reason.has_value() ||
           foreign_inventory.status ==
               SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted ||
           foreign_inventory.not_attempted_reason.has_value() ||
           query.status ==
               SystemAurUpdateQueryPhaseStatus::NotAttempted ||
           query.not_attempted_reason.has_value() ||
           aur.status == SystemAurUpdateAurPhaseStatus::NotAttempted ||
           aur.not_attempted_reason.has_value() ||
           (aur.operation_result.has_value() &&
            aur.operation_result->has_not_attempted_targets());
}

bool SystemAurUpdateOperationResult::has_cleanup_failure()
    const noexcept {
    return aur.operation_result.has_value() &&
           aur.operation_result->has_cleanup_failure();
}

bool SystemAurUpdateOperationResult::has_query_failure() const noexcept {
    return foreign_inventory.status ==
               SystemAurUpdateForeignInventoryPhaseStatus::Failed ||
           query.status == SystemAurUpdateQueryPhaseStatus::Failed ||
           (aur.operation_result.has_value() &&
            aur.operation_result->has_query_failure());
}

bool SystemAurUpdateOperationResult::has_inconsistency() const noexcept {
    return status ==
               SystemAurUpdateOperationStatus::InconsistentResult ||
           aur.status ==
               SystemAurUpdateAurPhaseStatus::InconsistentResult ||
           (aur.operation_result.has_value() &&
            filtered_result_has_inconsistency(
                aur.operation_result.value()));
}
