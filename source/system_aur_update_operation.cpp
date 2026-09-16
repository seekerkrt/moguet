#include "system_aur_update_operation.hpp"
#include "aur_devel_update.hpp"

#include "app_config.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_query.hpp"
#include "commands_sync.hpp"
#include "aur_rpc.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "shell_words.hpp"
#include <type_traits>
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
    return (!observation.preflight_version_lock_correlation.has_value() ||
            observation.preflight_version_lock_correlation->basis ==
                CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation) &&
           observation.aur_observation_basis ==
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
           !observation.aur_observation.has_value() &&
           !observation.preflight_version_lock_correlation.has_value();
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

    observation.preflight_version_lock_correlation =
        observe_cross_source_version_lock_correlation(
            CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation);

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
    if(result.coordinated_transition) {
        const auto& transition = *result.coordinated_transition;
        result.status = transition.is_success() ? SystemAurUpdateOperationStatus::Completed : SystemAurUpdateOperationStatus::StoppedOnCoordinatedTransition;
        result.stopped_phase = transition.is_success() ? SystemAurUpdateOperationPhase::None : SystemAurUpdateOperationPhase::CoordinatedTransition;
        result.repository.command_exit_status = transition.repository_exit_status;
        result.repository.status = transition.repository == CrossSourceExecutionPhaseStatus::Completed ? SystemAurUpdateRepositoryPhaseStatus::Completed : transition.repository == CrossSourceExecutionPhaseStatus::Failed ? SystemAurUpdateRepositoryPhaseStatus::Failed
                                                                                                                                                                                                                            : SystemAurUpdateRepositoryPhaseStatus::NotAttempted;
        return result;
    }
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

namespace {

using TransitionStatus = CrossSourceExecutionPhaseStatus;
using TransitionPhase = CrossSourceExecutionPhase;

const CrossSourceCoordinatedTransitionPlan* ready_transition(
    const CrossSourceVersionLockCorrelationResult& correlation) {
    if(correlation.basis != CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation ||
       correlation.failure || correlation.transition_plans.size() != 1 ||
       correlation.transition_plans.front().status != CrossSourceTransitionPlanStatus::ReadOnlyReady)
        return nullptr;
    return &correlation.transition_plans.front();
}

InstalledPackageQueryResult installed_transition_package(
    const InstalledPackageStateSnapshotResult& snapshot, const std::string& name) {
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&snapshot)) return *failure;
    const auto& packages = std::get<InstalledPackageStateSnapshot>(snapshot);
    const auto found = packages.find(name);
    if(found == packages.end()) return PackageNotFound{};
    return found->second;
}

bool installed_version_matches(const InstalledPackageQueryResult& observed,
                               const std::string& name, const std::string& version) {
    const auto* package = std::get_if<InstalledPackageMetadata>(&observed);
    return package && package->name == name && package->version == version;
}

// BuildPlan owns a second RPC observation. It must not silently replace the
// fresh, confirmed candidate while preparing the existing safe AUR lifecycle.
bool build_plan_matches_replacement(const BuildPlan& plan,
                                    const AurPackageConstraintMetadata& expected) {
    if(plan.root_targets.size() != 1 || plan.root_targets.front().requested_name != expected.package_name)
        return false;
    const PlannedPackageRelationObservation* root = nullptr;
    for(const auto& observed : plan.planned_relation_observations) {
        if(observed.package.package_name != expected.package_name) continue;
        if(root) return false;
        root = &observed;
    }
    if(!root || root->package.package_base != expected.package_base ||
       root->package.package_version != expected.package_version || root->declarations != expected.relations ||
       root->package.provides.size() != expected.provides.size()) return false;
    for(std::size_t i = 0; i < expected.provides.size(); ++i) {
        if(root->package.provides[i].capability != expected.provides[i].capability ||
           root->package.provides[i].observed_version != expected.provides[i].provided_version) return false;
    }
    std::vector<DependencyRequirement> runtime, build, check;
    for(const auto& edge : plan.dependency_edges) {
        if(edge.parent_package_name != expected.package_name) continue;
        if(edge.parent_package_base != expected.package_base || !edge.requirement) return false;
        switch(edge.role) {
            case PackageRole::RuntimeDependency: runtime.push_back(*edge.requirement); break;
            case PackageRole::BuildDependency: build.push_back(*edge.requirement); break;
            case PackageRole::CheckDependency: check.push_back(*edge.requirement); break;
            default: return false;
        }
    }
    return runtime == expected.depends && build == expected.make_depends && check == expected.check_depends;
}

CrossSourceTransitionExecutionResult execute_coordinated_transition(
    const CrossSourceCoordinatedTransitionPlan& plan,
    const std::vector<std::string>& repository_args, const AppConfig& config) {
    CrossSourceTransitionExecutionResult result{plan};
    // Mark the active phase failed before entering its throwing boundary. A
    // completed prefix survives exceptions; the untouched tail is NotAttempted.
    TransitionStatus* active = &result.confirmation;
    *active = TransitionStatus::Failed;
    const auto start = [&](TransitionPhase phase, TransitionStatus& status) {
        result.stopped_phase = phase;
        active = &status;
        status = TransitionStatus::Failed;
    };
    try {
        auto confirmation = confirm_cross_source_transition(plan, config);
        const auto* acceptance = std::get_if<ExplicitConfirmationAcceptance>(&confirmation);
        if(!acceptance || !acceptance->valid() || config.no_confirm) {
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr(!std::is_same_v<T, ExplicitConfirmationAcceptance>)
                    result.confirmation_result = value;
            },
                       confirmation);
            if(!result.confirmation_result) result.confirmation_result = ConfirmationUnavailable{ConfirmationUnavailableReason::NoConfirm};
            return result;
        }
        result.confirmation_result = ConfirmationAccepted{ConfirmationDecisionOrigin::ExplicitToken};
        result.confirmation = TransitionStatus::Completed;
        start(TransitionPhase::Revalidation, result.validation);
        result.revalidation = observe_cross_source_version_lock_correlation(
            CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation);
        const auto* fresh = ready_transition(*result.revalidation);
        if(!fresh || *fresh != plan) {
            result.diagnostic = localization::translate_message("Cross-source state changed after confirmation; rerun required. No package mutation was attempted.");
            return result;
        }
        result.validation = TransitionStatus::Completed;
        const auto& expected_repository = plan.correlation.evidence.repository_upgrade.repository_candidate;
        const auto& replacement = std::get<AurReplacementCandidateQuerySuccess>(plan.correlation.evidence.aur_replacement).candidates.at(0);
        const auto& old = plan.installed_foreign.value();
        require_valid_package_name(old.name);
        // Only one validated operand, with pacman's ordinary dependency safety.
        // No recursive/cascade options, no bypass, and no fallback after approval.
        start(TransitionPhase::RemoveInstalledForeign, result.removal);
        result.removal_exit_status = run_command(shell_words::join({"sudo", "pacman", "-R", "--", old.name}));
        if(result.removal_exit_status != 0) return result;
        result.removal = TransitionStatus::Completed;
        start(TransitionPhase::RepositorySystemUpgrade, result.repository);
        result.repository_exit_status = execute_ordered_repository_sync_transaction(repository_args, config);
        if(result.repository_exit_status != 0) return result;
        result.repository = TransitionStatus::Completed;

        start(TransitionPhase::RepositoryPostState, result.repository_post_state);
        const auto paths = resolve_pacman_database_paths();
        const auto repository_snapshot = snapshot_installed_package_states(paths);
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&repository_snapshot)) {
            result.metadata_failure = *failure;
            return result;
        }
        result.observed_repository = installed_transition_package(repository_snapshot, expected_repository.package_name);
        result.observed_consumer = installed_transition_package(repository_snapshot, old.name);
        if(!installed_version_matches(*result.observed_repository, expected_repository.package_name,
                                      *expected_repository.package_version->version()) ||
           !std::holds_alternative<PackageNotFound>(*result.observed_consumer)) {
            result.diagnostic = localization::translate_message("Expected repository post-state was not observed; the replacement was not started. Rerun required.");
            return result;
        }
        result.repository_post_state = TransitionStatus::Completed;

        start(TransitionPhase::AurAuthority, result.aur_authority);
        std::optional<AurPackageInfo> current;
        try {
            current = AurClient::info_strict(old.name);
        } catch(const std::exception& error) {
            result.replacement_observation = AurReplacementCandidateQueryFailure{{old.name}, error.what()};
            throw;
        }
        if(!current)
            result.replacement_observation = AurReplacementCandidateNotFound{old.name};
        else if(!current->constraint_metadata)
            result.replacement_observation = AurReplacementCandidateMetadataUnavailable{
                old.name, current->PackageBase, ObservedVersionUnknownReason::MissingVersionMetadata};
        else
            result.replacement_observation = AurReplacementCandidateQuerySuccess{{*current->constraint_metadata}};
        if(!current || !current->constraint_metadata || *current->constraint_metadata != replacement ||
           current->Name != replacement.package_name || current->PackageBase != replacement.package_base ||
           current->Version != *replacement.package_version.version()) {
            result.diagnostic = localization::format_translated_message("The {} replacement changed or is unavailable; rerun required. Build and install were not started.", "AUR");
            return result;
        }
        result.aur_authority = TransitionStatus::Completed;

        start(TransitionPhase::AurReplacement, result.aur_replacement);
        // The old record is historical reason/transition attribution, not a
        // claim that B is still installed. All remote authority above is fresh.
        auto query = query_aur_updates_for_foreign_inventory({old});
        result.replacement_query = query;
        if(!query.recoverable_failures.empty() || query.plan.entries.size() != 1 ||
           query.plan.entries.front().classification != AurUpdateClassification::UpdateAvailable ||
           !query.plan.entries.front().aur_package ||
           query.plan.entries.front().aur_package->aur_name != current->Name ||
           query.plan.entries.front().aur_package->package_base != current->PackageBase ||
           query.plan.entries.front().aur_package->version != current->Version) {
            result.diagnostic = localization::format_translated_message("The {} replacement changed or is unavailable; rerun required. Build and install were not started.", "AUR");
            return result;
        }
        query.plan.entries.front().coordinated_replacement_version = current->Version;
        auto prepared = prepare_filtered_aur_update_operation(std::move(query), NoExplicitSourceSatisfaction{},
                                                              DevelRequiresCheckPolicy::SkipIndependentTarget, SavedSourcePreferencePolicy::Ignore, config);
        const bool has_executable_replacement = prepared.is_prepared();
        if(has_executable_replacement &&
           (!prepared.execution_preflight().build_plan ||
            !build_plan_matches_replacement(*prepared.execution_preflight().build_plan, replacement))) {
            result.diagnostic = localization::format_translated_message("The prepared {} replacement differs from the confirmed transition; rerun required. Build and install were not started.", "AUR");
            return result;
        }
        try {
            result.aur_result.emplace(execute_prepared_filtered_aur_update_operation(std::move(prepared), config));
        } catch(FilteredAurUpdateCancelled& stopped) {
            result.aur_result.emplace(std::move(stopped).release_result());
        }
        if(!result.aur_result->is_success()) return result;
        if(!has_executable_replacement) {
            result.diagnostic = localization::format_translated_message("The confirmed {} replacement was not executable; the coordinated transition is incomplete.", "AUR");
            return result;
        }
        result.aur_replacement = TransitionStatus::Completed;

        // The install owner applies DesiredInstallReason in its transaction.
        // Restoration is only complete once fresh installed state confirms it.
        start(TransitionPhase::InstallReasonRestoration, result.install_reason);
        const auto final_paths = resolve_pacman_database_paths();
        const auto final_snapshot = snapshot_installed_package_states(final_paths);
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&final_snapshot)) {
            result.metadata_failure = *failure;
            return result;
        }
        result.observed_repository = installed_transition_package(final_snapshot, expected_repository.package_name);
        result.observed_consumer = installed_transition_package(final_snapshot, old.name);
        const auto* consumer = std::get_if<InstalledPackageMetadata>(&*result.observed_consumer);
        if(!consumer || consumer->reason != plan.expected_install_reason) {
            result.diagnostic = localization::translate_message("The replacement install reason was not restored; the coordinated transition is incomplete.");
            return result;
        }
        result.install_reason = TransitionStatus::Completed;
        start(TransitionPhase::PostStateVerification, result.post_state);
        const auto runtime = query_installed_package_runtime_dependency_metadata(final_paths);
        const auto* inventory = std::get_if<InstalledPackageRuntimeDependencyMetadataInventory>(&runtime);
        if(const auto* failure = std::get_if<InstalledPackageRuntimeDependencyMetadataInventoryFailure>(&runtime)) result.metadata_failure = failure->failure;
        bool exact_requirement_observed = false;
        std::size_t consumer_count = 0;
        if(inventory)
            for(const auto& package : *inventory) {
                if(package.package_name != old.name) continue;
                ++consumer_count;
                if(package.installed_version != consumer->version) continue;
                for(const auto& specification : package.dependency_specifications) {
                    const auto parsed = parse_dependency_requirement(specification);
                    if(!parsed.requirement()) continue;
                    const auto* requirement = std::get_if<ConsumerDependencyRequirement>(parsed.requirement());
                    if(requirement && *requirement == *plan.correlation.replacement_requirement)
                        exact_requirement_observed = true;
                }
            }
        const bool versions_match = installed_version_matches(*result.observed_repository,
                                                              expected_repository.package_name, *expected_repository.package_version->version()) &&
                                    installed_version_matches(*result.observed_consumer, replacement.package_name, *replacement.package_version.version());
        if(!versions_match || !exact_requirement_observed || consumer_count != 1 ||
           evaluate_consumer_dependency_requirement(*plan.correlation.replacement_requirement,
                                                    ObservedVersion::available(ObservedVersionSource::InstalledExactPackage,
                                                                               std::get<InstalledPackageMetadata>(*result.observed_repository).version))
                   .satisfaction() != ConstraintSatisfaction::Satisfied) {
            result.diagnostic = localization::translate_message("Expected exact-version post-state was not verified; the coordinated transition is incomplete.");
            return result;
        }
        result.post_state = TransitionStatus::Completed;
        result.stopped_phase = TransitionPhase::Complete;
        return result;
    } catch(const PackageMetadataError& error) {
        result.metadata_failure = error.failure();
        result.diagnostic = error.what();
    } catch(const std::exception& error) {
        result.diagnostic = error.what();
    } catch(...) {
        result.diagnostic = localization::translate_message("The coordinated transition stopped on an unexpected failure.");
    }
    *active = TransitionStatus::Failed;
    return result;
}

} // namespace

bool CrossSourceTransitionExecutionResult::is_success() const noexcept {
    const auto* acceptance = confirmation_result ? std::get_if<ConfirmationAccepted>(&*confirmation_result) : nullptr;
    const auto* fresh = revalidation ? ready_transition(*revalidation) : nullptr;
    return stopped_phase == CrossSourceExecutionPhase::Complete &&
           acceptance && acceptance->origin == ConfirmationDecisionOrigin::ExplicitToken &&
           fresh && *fresh == confirmed_plan && !metadata_failure &&
           confirmation == TransitionStatus::Completed && validation == TransitionStatus::Completed &&
           removal == TransitionStatus::Completed && removal_exit_status == 0 &&
           repository == TransitionStatus::Completed && repository_exit_status == 0 &&
           repository_post_state == TransitionStatus::Completed && aur_authority == TransitionStatus::Completed &&
           aur_replacement == TransitionStatus::Completed && aur_result && aur_result->is_success() &&
           install_reason == TransitionStatus::Completed && post_state == TransitionStatus::Completed && diagnostic.empty();
}

bool CrossSourceTransitionExecutionResult::has_partial_completion() const noexcept {
    return !is_success() && removal == TransitionStatus::Completed;
}

SystemAurUpdateOperationResult
execute_prepared_system_aur_update_operation(
    PreparedSystemAurUpdateOperation prepared,
    const AppConfig& config,
    SystemAurUpdatePreflightReporter report_preflight) {
    SystemAurUpdateOperationResult result;
    if(!prepared.valid_) {
        mark_all_not_attempted_for_inconsistency(result);
        return reduce_system_aur_update_result(std::move(result));
    }
    prepared.valid_ = false;
    result.repository.ordered_pacman_args =
        prepared.request_.ordered_pacman_args();
    result.repository.compatible_request = prepared.request_;

    // Only a unique complete plan enters the explicitly confirmed path. All
    // other observations retain the ordinary repository transaction policy.
    result.preflight_version_lock_correlation =
        observe_cross_source_version_lock_correlation(
            CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation);
    if(report_preflight != nullptr) {
        report_preflight(*result.preflight_version_lock_correlation);
    }

    if(const auto* plan = ready_transition(*result.preflight_version_lock_correlation)) {
        result.coordinated_transition.emplace(execute_coordinated_transition(*plan, result.repository.ordered_pacman_args, config));
        auto& transition = *result.coordinated_transition;
        if(transition.has_partial_completion()) {
            try {
                const auto snapshot = snapshot_installed_package_states(resolve_pacman_database_paths());
                transition.observed_repository = installed_transition_package(snapshot, plan->correlation.evidence.repository_upgrade.repository_candidate.package_name);
                transition.observed_consumer = installed_transition_package(snapshot, plan->installed_foreign->name);
            } catch(const PackageMetadataError& error) {
                transition.observed_consumer = error.failure();
            } catch(...) {
                transition.observed_consumer = PackageMetadataFailure{PackageMetadataErrorCode::QueryFailed,
                                                                      localization::translate_message("Current replacement state could not be observed.")};
            }
        }
        return reduce_system_aur_update_result(std::move(result));
    }

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
        // Primary failure and the unattempted AUR tail are final.
        result.cross_source_version_lock_correlation =
            observe_cross_source_version_lock_correlation();
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
        // Primary failure and the unattempted AUR tail are final.
        result.cross_source_version_lock_correlation =
            observe_cross_source_version_lock_correlation();
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
        // Primary failure and the unattempted AUR tail are final.
        result.cross_source_version_lock_correlation =
            observe_cross_source_version_lock_correlation();
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
    if(coordinated_transition) return coordinated_transition->is_success();
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
    if(coordinated_transition) return coordinated_transition->removal == CrossSourceExecutionPhaseStatus::Completed ? PackageStateChange::Changed : PackageStateChange::Unknown;
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
    if(coordinated_transition) return coordinated_transition->has_partial_completion();
    return !is_success() &&
           repository.status ==
               SystemAurUpdateRepositoryPhaseStatus::Completed;
}

bool SystemAurUpdateOperationResult::has_not_attempted_phase()
    const noexcept {
    if(coordinated_transition) return coordinated_transition->post_state == CrossSourceExecutionPhaseStatus::NotAttempted;
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
    if(coordinated_transition) return coordinated_transition->aur_result && coordinated_transition->aur_result->has_cleanup_failure();
    return aur.operation_result.has_value() &&
           aur.operation_result->has_cleanup_failure();
}

bool SystemAurUpdateOperationResult::has_query_failure() const noexcept {
    if(coordinated_transition) {
        const auto& transition = *coordinated_transition;
        return transition.metadata_failure.has_value() ||
               (transition.replacement_observation && std::holds_alternative<AurReplacementCandidateQueryFailure>(*transition.replacement_observation)) ||
               (transition.replacement_query && !transition.replacement_query->recoverable_failures.empty()) ||
               (transition.aur_result && transition.aur_result->has_query_failure()) ||
               (transition.observed_consumer && std::holds_alternative<PackageMetadataFailure>(*transition.observed_consumer)) ||
               (transition.observed_repository && std::holds_alternative<PackageMetadataFailure>(*transition.observed_repository));
    }
    return foreign_inventory.status ==
               SystemAurUpdateForeignInventoryPhaseStatus::Failed ||
           query.status == SystemAurUpdateQueryPhaseStatus::Failed ||
           (aur.operation_result.has_value() &&
            aur.operation_result->has_query_failure());
}

bool SystemAurUpdateOperationResult::has_inconsistency() const noexcept {
    if(coordinated_transition) return coordinated_transition->aur_result && filtered_result_has_inconsistency(*coordinated_transition->aur_result);
    return status ==
               SystemAurUpdateOperationStatus::InconsistentResult ||
           aur.status ==
               SystemAurUpdateAurPhaseStatus::InconsistentResult ||
           (aur.operation_result.has_value() &&
            filtered_result_has_inconsistency(
                aur.operation_result.value()));
}
