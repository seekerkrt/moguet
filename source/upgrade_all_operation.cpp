#include "upgrade_all_operation.hpp"

#include "app_config.hpp"
#include "cache_authority.hpp"
#include "interactive_confirmation.hpp"
#include "localization.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

constexpr const char* AUR_SERVICE = "AUR";
constexpr const char* COMMAND_NAME = "upgrade-all";
constexpr const char* COMMAND_SENTENCE_NAME = "Upgrade-all";
constexpr const char* PACKAGE_BASE_FIELD = "PackageBase";

std::string consumed_capability_diagnostic() {
    // TRANSLATORS: The placeholder is the literal command name "upgrade-all".
    return localization::format_translated_message(
        "Prepared {} operation is invalid or has already been consumed.",
        COMMAND_NAME);
}

std::string option_mismatch_diagnostic() {
    // TRANSLATORS: The placeholder is the literal command name "upgrade-all".
    return localization::format_translated_message(
        "Prepared {} options differ from execution options.", COMMAND_NAME);
}

std::string source_snapshot_mismatch_diagnostic() {
    // TRANSLATORS: The placeholder is the literal command name "upgrade-all".
    return localization::format_translated_message(
        "Prepared {} source snapshot differs from its nested system/source capability.",
        COMMAND_NAME);
}

std::string source_correlation_mismatch_diagnostic() {
    // TRANSLATORS: The placeholder is the literal command name "upgrade-all".
    return localization::format_translated_message(
        "Prepared {} explicit source correlation is inconsistent.",
        COMMAND_NAME);
}

bool options_match(
    const SystemSourceUpgradeOptionSnapshot& snapshot,
    const AppConfig& config) noexcept {
    return snapshot.no_edit ==
               (config.user_config.review.pkgbuild == ReviewPolicy::Skip) &&
           snapshot.no_diff ==
               (config.user_config.review.diff == ReviewPolicy::Skip) &&
           snapshot.no_confirm == config.no_confirm &&
           snapshot.rebuild ==
               (config.user_config.build.mode == BuildMode::Rebuild) &&
           snapshot.clean_build ==
               (config.user_config.build.mode == BuildMode::Clean) &&
           snapshot.rm_deps == config.rm_deps &&
           snapshot.editor == config.editor;
}

bool environments_match(
    const std::optional<SourceBuildEnvironment>& lhs,
    const std::optional<SourceBuildEnvironment>& rhs) noexcept {
    if(lhs.has_value() != rhs.has_value()) return false;
    if(!lhs.has_value()) return true;
    if(lhs->ordered_assignments.size() != rhs->ordered_assignments.size()) {
        return false;
    }
    for(std::size_t index = 0;
        index < lhs->ordered_assignments.size(); ++index) {
        const SourceEnvironmentAssignment& lhs_assignment =
            lhs->ordered_assignments[index];
        const SourceEnvironmentAssignment& rhs_assignment =
            rhs->ordered_assignments[index];
        if(lhs_assignment.key != rhs_assignment.key ||
           lhs_assignment.value != rhs_assignment.value) {
            return false;
        }
    }
    return true;
}

bool option_snapshots_match(
    const SystemSourceUpgradeOptionSnapshot& lhs,
    const SystemSourceUpgradeOptionSnapshot& rhs) noexcept {
    return lhs.no_edit == rhs.no_edit &&
           lhs.no_diff == rhs.no_diff &&
           lhs.no_confirm == rhs.no_confirm &&
           lhs.rebuild == rhs.rebuild &&
           lhs.clean_build == rhs.clean_build &&
           lhs.rm_deps == rhs.rm_deps &&
           lhs.editor == rhs.editor;
}

bool source_entries_match(
    const RegisteredSourcePreferenceSnapshot& lhs,
    const RegisteredSourcePreferenceSnapshot& rhs) noexcept {
    return lhs.original_preference_index == rhs.original_preference_index &&
           lhs.preference_package_name == rhs.preference_package_name &&
           lhs.entry_path == rhs.entry_path &&
           environments_match(lhs.environment, rhs.environment) &&
           lhs.canonical_source_identity_key ==
               rhs.canonical_source_identity_key &&
           lhs.resolved_package_base == rhs.resolved_package_base &&
           lhs.preference_load_warnings == rhs.preference_load_warnings &&
           lhs.source_kind == rhs.source_kind &&
           lhs.repository_identity == rhs.repository_identity &&
           lhs.required_target_provenance ==
               rhs.required_target_provenance &&
           lhs.artifact_lifecycle_intent == rhs.artifact_lifecycle_intent;
}

bool source_snapshots_match(
    const SystemSourceUpgradePreparedSnapshot& lhs,
    const SystemSourceUpgradePreparedSnapshot& rhs) noexcept {
    if(lhs.preference_root_exists != rhs.preference_root_exists ||
       !option_snapshots_match(lhs.options, rhs.options) ||
       lhs.registered_sources.size() != rhs.registered_sources.size()) {
        return false;
    }
    for(std::size_t index = 0;
        index < lhs.registered_sources.size(); ++index) {
        if(!source_entries_match(
               lhs.registered_sources[index],
               rhs.registered_sources[index])) {
            return false;
        }
    }
    return true;
}

UpgradeAllExplicitSourceAdapterIssue make_adapter_issue(
    UpgradeAllExplicitSourceAdapterIssueKind kind,
    std::size_t adapter_index,
    const RegisteredSourcePreferenceSnapshot& source,
    std::string diagnostic) {
    return UpgradeAllExplicitSourceAdapterIssue{
        kind,
        adapter_index,
        source.original_preference_index,
        source.preference_package_name,
        std::move(diagnostic)};
}

std::vector<UpgradeAllOperationWarning> collect_preparation_warnings(
    const SystemSourceUpgradePreparedSnapshot& snapshot) {
    std::vector<UpgradeAllOperationWarning> warnings;
    for(const RegisteredSourcePreferenceSnapshot& source :
        snapshot.registered_sources) {
        for(const std::string& diagnostic :
            source.preference_load_warnings) {
            warnings.push_back(UpgradeAllOperationWarning{
                UpgradeAllOperationWarningKind::
                    RegisteredSourcePreference,
                UpgradeAllOperationPhase::Preparation,
                source.original_preference_index,
                source.preference_package_name,
                diagnostic});
        }
    }
    return warnings;
}

bool adapter_matches_snapshot(
    const UpgradeAllExplicitSourceIdentityAdapter& adapter,
    const SystemSourceUpgradePreparedSnapshot& snapshot) noexcept {
    if(!adapter.issues.empty() ||
       adapter.entries.size() != snapshot.registered_sources.size()) {
        return false;
    }
    for(std::size_t index = 0; index < adapter.entries.size(); ++index) {
        const UpgradeAllExplicitSourceAdapterEntry& entry =
            adapter.entries[index];
        const RegisteredSourcePreferenceSnapshot& source =
            snapshot.registered_sources[index];
        if(entry.adapter_index != index ||
           entry.original_preference_index !=
               source.original_preference_index ||
           entry.preference_package_name != source.preference_package_name ||
           entry.canonical_source_identity_key !=
               source.canonical_source_identity_key ||
           entry.resolved_package_base != source.resolved_package_base ||
           entry.affected_package_names !=
               std::vector<std::string>{
                   source.preference_package_name} ||
           entry.planner_identity.preference_package_name !=
               source.preference_package_name ||
           entry.planner_identity.produced_package_names !=
               entry.affected_package_names) {
            return false;
        }

        const auto* package_base =
            std::get_if<UpgradeAllResolvedPackageBase>(
                &entry.planner_identity.package_base);
        const auto* source_identity =
            std::get_if<UpgradeAllResolvedSourceIdentity>(
                &entry.planner_identity.source_identity);
        if(package_base == nullptr || source_identity == nullptr ||
           !source.resolved_package_base.has_value() ||
           !source.canonical_source_identity_key.has_value() ||
           package_base->package_base != *source.resolved_package_base ||
           source_identity->key != *source.canonical_source_identity_key) {
            return false;
        }
    }
    return true;
}

RegisteredSourceUpgradeResult make_not_attempted_source_result(
    const RegisteredSourcePreferenceSnapshot& source) {
    return RegisteredSourceUpgradeResult{
        source.original_preference_index,
        source.preference_package_name,
        source.canonical_source_identity_key,
        source.resolved_package_base,
        RegisteredSourceUpgradeStatus::NotAttempted,
        RegisteredSourceUpgradeFailureKind::PriorPhaseStopped,
        PackageStateChange::NoChange,
        std::nullopt,
        std::nullopt};
}

SystemSourceUpgradeResult make_unattempted_system_source_result(
    const SystemSourceUpgradePreparedSnapshot& snapshot,
    SystemSourceUpgradeStatus status) {
    SystemSourceUpgradeResult result;
    result.status = status;
    result.stopped_phase = SystemSourceUpgradePhase::Preparation;
    result.prepared_snapshot = snapshot;
    result.registered_source_results.reserve(
        snapshot.registered_sources.size());
    for(const RegisteredSourcePreferenceSnapshot& source :
        snapshot.registered_sources) {
        result.registered_source_results.push_back(
            make_not_attempted_source_result(source));
    }
    return result;
}

SystemSourceUpgradeResult make_unavailable_system_source_result(
    const SystemSourceUpgradePreparedSnapshot& snapshot,
    SystemSourceUpgradePhase observed_phase,
    const std::string& diagnostic) {
    SystemSourceUpgradeResult result = make_unattempted_system_source_result(
        snapshot, SystemSourceUpgradeStatus::InconsistentResult);
    result.stopped_phase = observed_phase;

    if(observed_phase == SystemSourceUpgradePhase::System) {
        result.system.status = SystemUpgradePhaseStatus::Failed;
        result.system.package_state_change = PackageStateChange::Unknown;
        result.system.diagnostic = localization::format_translated_message(
            "The system result is unavailable because an unexpected exception occurred after the phase started: {}",
            diagnostic);
    } else if(observed_phase ==
              SystemSourceUpgradePhase::RegisteredSource) {
        result.system.status = SystemUpgradePhaseStatus::Completed;
        result.system.package_state_change = PackageStateChange::Unknown;
        for(RegisteredSourceUpgradeResult& source :
            result.registered_source_results) {
            source.status = RegisteredSourceUpgradeStatus::Incomplete;
            source.failure_kind =
                RegisteredSourceUpgradeFailureKind::UnknownException;
            source.package_state_change = PackageStateChange::Unknown;
            source.diagnostic = localization::format_translated_message(
                "The registered source result is unavailable because an unexpected exception occurred after the phase started: {}",
                diagnostic);
        }
    }
    return result;
}

UpgradeAllOperationPhase aggregate_phase_for_system_source(
    SystemSourceUpgradePhase phase) noexcept {
    switch(phase) {
        case SystemSourceUpgradePhase::None:
        case SystemSourceUpgradePhase::Preparation:
            return UpgradeAllOperationPhase::Preparation;
        case SystemSourceUpgradePhase::System:
            return UpgradeAllOperationPhase::System;
        case SystemSourceUpgradePhase::RegisteredSource:
            return UpgradeAllOperationPhase::RegisteredSource;
    }
    return UpgradeAllOperationPhase::Preparation;
}

UpgradeAllOperationIssue make_issue(
    UpgradeAllOperationIssueKind kind,
    UpgradeAllOperationPhase phase,
    std::string diagnostic) {
    UpgradeAllOperationIssue issue;
    issue.kind = kind;
    issue.phase = phase;
    issue.diagnostic = std::move(diagnostic);
    return issue;
}

void add_stopping_diagnostic(
    UpgradeAllOperationResult& result,
    UpgradeAllOperationPhase phase,
    const std::string& diagnostic) {
    result.diagnostics.push_back(UpgradeAllOperationDiagnostic{
        phase, true, diagnostic});
}

void set_not_attempted_after(
    UpgradeAllOperationResult& result,
    UpgradeAllNotAttemptedReason reason) {
    result.foreign_inventory.status =
        UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
    result.foreign_inventory.not_attempted_reason = reason;
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason = reason;
}

UpgradeAllOperationPreparedSnapshot make_prepared_snapshot(
    const SystemSourceUpgradePreparedSnapshot& source_snapshot) {
    UpgradeAllOperationPreparedSnapshot snapshot;
    snapshot.system_source = source_snapshot;
    snapshot.explicit_source_adapter =
        adapt_prepared_source_identities_for_upgrade_all(
            source_snapshot);
    snapshot.warnings = collect_preparation_warnings(source_snapshot);
    return snapshot;
}

void append_adapter_issues(
    UpgradeAllOperationResult& result,
    const UpgradeAllExplicitSourceIdentityAdapter& adapter) {
    for(const UpgradeAllExplicitSourceAdapterIssue& adapter_issue :
        adapter.issues) {
        UpgradeAllOperationIssue issue = make_issue(
            UpgradeAllOperationIssueKind::ExplicitSourceAdapterInvalid,
            UpgradeAllOperationPhase::Preparation,
            adapter_issue.diagnostic);
        issue.adapter_index = adapter_issue.adapter_index;
        issue.original_preference_index =
            adapter_issue.original_preference_index;
        issue.package_name = adapter_issue.preference_package_name;
        result.issues.push_back(std::move(issue));
    }
}

const SystemSourceUpgradeIssue* find_system_source_cache_issue(
    const SystemSourceUpgradeResult& result) {
    const auto issue = std::find_if(
        result.issues.begin(), result.issues.end(),
        [](const SystemSourceUpgradeIssue& candidate) {
            return candidate.kind ==
                   SystemSourceUpgradeIssueKind::CacheAuthorityInvalid;
        });
    return issue == result.issues.end() ? nullptr : &*issue;
}

void append_system_source_cache_issue(
    UpgradeAllOperationResult& result,
    const SystemSourceUpgradeIssue& source_issue,
    UpgradeAllOperationPhase phase) {
    UpgradeAllOperationIssue issue = make_issue(
        UpgradeAllOperationIssueKind::CacheAuthorityInvalid,
        phase, source_issue.diagnostic);
    issue.original_preference_index =
        source_issue.original_preference_index;
    issue.package_name = source_issue.preference_package_name;
    issue.cache_resolution_failure =
        source_issue.cache_resolution_failure;
    issue.cache_preparation_failure =
        source_issue.cache_preparation_failure;
    issue.trusted_cache_failure = source_issue.trusted_cache_failure;
    result.issues.push_back(std::move(issue));
}

UpgradeAllOperationResult make_blocked_preparation_result(
    SystemSourceUpgradeResult source_result) {
    UpgradeAllOperationResult result;
    result.status = UpgradeAllOperationStatus::BlockedBeforeMutation;
    result.stopped_phase = UpgradeAllOperationPhase::Preparation;
    result.prepared_snapshot = make_prepared_snapshot(
        source_result.prepared_snapshot);
    result.warnings = result.prepared_snapshot.warnings;
    result.system_source = std::move(source_result);
    append_adapter_issues(
        result, result.prepared_snapshot.explicit_source_adapter);
    const SystemSourceUpgradeIssue* cache_issue =
        find_system_source_cache_issue(result.system_source);
    if(cache_issue != nullptr) {
        append_system_source_cache_issue(
            result, *cache_issue,
            UpgradeAllOperationPhase::Preparation);
        add_stopping_diagnostic(
            result, UpgradeAllOperationPhase::Preparation,
            cache_issue->diagnostic);
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure);
    } else {
        set_not_attempted_after(
            result, UpgradeAllNotAttemptedReason::PreparationBlocked);
    }
    return result;
}

UpgradeAllOperationResult make_preexecution_rejection(
    UpgradeAllOperationPreparedSnapshot snapshot,
    UpgradeAllOperationIssueKind issue_kind,
    std::string diagnostic,
    UpgradeAllOperationStatus status =
        UpgradeAllOperationStatus::InconsistentResult) {
    UpgradeAllOperationResult result;
    result.status = status;
    result.stopped_phase = UpgradeAllOperationPhase::Preparation;
    result.prepared_snapshot = std::move(snapshot);
    result.warnings = result.prepared_snapshot.warnings;
    result.system_source = make_unattempted_system_source_result(
        result.prepared_snapshot.system_source,
        status == UpgradeAllOperationStatus::BlockedBeforeMutation
            ? SystemSourceUpgradeStatus::BlockedBeforeMutation
            : SystemSourceUpgradeStatus::InconsistentResult);
    result.issues.push_back(make_issue(
        issue_kind,
        UpgradeAllOperationPhase::Preparation,
        diagnostic));
    add_stopping_diagnostic(
        result, UpgradeAllOperationPhase::Preparation, diagnostic);
    set_not_attempted_after(
        result,
        status == UpgradeAllOperationStatus::BlockedBeforeMutation
            ? UpgradeAllNotAttemptedReason::PreparationBlocked
            : UpgradeAllNotAttemptedReason::
                  PriorAggregateInconsistency);
    return result;
}

bool is_successful_source_status(
    RegisteredSourceUpgradeStatus status) noexcept {
    return status == RegisteredSourceUpgradeStatus::Updated ||
           status == RegisteredSourceUpgradeStatus::NoChange;
}

bool has_non_successful_source(
    const SystemSourceUpgradeResult& result) noexcept {
    return std::any_of(
        result.registered_source_results.begin(),
        result.registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return !is_successful_source_status(source.status);
        });
}

bool is_possible_cross_source_version_lock_blocker_candidate(
    const CrossSourceVersionLockAssessment& assessment) noexcept {
    return assessment.installed_requirement_against_installed_version
               .has_value() &&
           assessment.installed_requirement_against_repository_candidate
               .has_value() &&
           assessment.installed_requirement_against_installed_version
                   ->satisfaction() ==
               ConstraintSatisfaction::Satisfied &&
           assessment.installed_requirement_against_repository_candidate
                   ->satisfaction() ==
               ConstraintSatisfaction::Unsatisfied;
}

void record_cross_source_version_lock_correlation_failure(
    UpgradeAllCrossSourceVersionLockCorrelationResult& correlation,
    UpgradeAllCrossSourceVersionLockCorrelationFailureKind kind,
    const char* diagnostic = nullptr) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<
                  UpgradeAllCrossSourceVersionLockCorrelationFailure>);
    correlation.failure.emplace();
    correlation.failure->kind = kind;
    if(diagnostic == nullptr) return;

    try {
        correlation.failure->diagnostic.emplace(diagnostic);
    } catch(...) {
        // The typed secondary failure remains available even when retaining
        // its optional diagnostic would require unavailable memory.
        correlation.failure->diagnostic.reset();
    }
}

void observe_post_system_failure_cross_source_version_lock_correlation(
    UpgradeAllOperationResult& result) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<
                  UpgradeAllCrossSourceVersionLockCorrelationResult>);
    UpgradeAllCrossSourceVersionLockCorrelationResult& correlation =
        result.cross_source_version_lock_correlation.emplace();

    try {
        correlation.observation.emplace(
            observe_cross_source_version_lock_candidates());

        std::vector<CrossSourceVersionLockAssessment> assessments;
        std::vector<std::size_t> possible_blocker_assessment_indices;
        const auto& candidates = correlation.observation->candidates;
        assessments.reserve(candidates.size());
        possible_blocker_assessment_indices.reserve(candidates.size());
        for(const CrossSourceVersionLockCandidateEvidence& candidate :
            candidates) {
            CrossSourceVersionLockAssessment assessment =
                assess_cross_source_version_lock_candidate(candidate);
            const bool is_possible_blocker_candidate =
                is_possible_cross_source_version_lock_blocker_candidate(
                    assessment);
            const std::size_t assessment_index = assessments.size();
            assessments.push_back(std::move(assessment));
            if(is_possible_blocker_candidate) {
                possible_blocker_assessment_indices.push_back(
                    assessment_index);
            }
        }

        // Publish assessment/index vectors together. An exception before this
        // point retains the observation plus a typed secondary failure, not a
        // misleading partially indexed assessment set.
        correlation.assessments = std::move(assessments);
        correlation.possible_blocker_assessment_indices =
            std::move(possible_blocker_assessment_indices);
    } catch(const std::bad_alloc&) {
        record_cross_source_version_lock_correlation_failure(
            correlation,
            UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
                ResourceExhaustion);
    } catch(const std::exception& error) {
        record_cross_source_version_lock_correlation_failure(
            correlation,
            UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
                UnexpectedException,
            error.what());
    } catch(...) {
        record_cross_source_version_lock_correlation_failure(
            correlation,
            UpgradeAllCrossSourceVersionLockCorrelationFailureKind::
                UnknownException);
    }
}

bool stop_after_system_source_failure(UpgradeAllOperationResult& result) {
    const SystemSourceUpgradeIssue* cache_issue =
        find_system_source_cache_issue(result.system_source);
    if(cache_issue != nullptr) {
        const UpgradeAllOperationPhase stopped_phase =
            aggregate_phase_for_system_source(cache_issue->phase);
        result.status = result.system_source.status ==
                                SystemSourceUpgradeStatus::BlockedBeforeMutation
                            ? UpgradeAllOperationStatus::BlockedBeforeMutation
                            : UpgradeAllOperationStatus::StoppedOnSourceFailure;
        result.stopped_phase = stopped_phase;
        append_system_source_cache_issue(
            result, *cache_issue, stopped_phase);
        add_stopping_diagnostic(
            result, stopped_phase, cache_issue->diagnostic);
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure);
        return true;
    }

    switch(result.system_source.status) {
        case SystemSourceUpgradeStatus::Completed:
            break;
        case SystemSourceUpgradeStatus::StoppedOnSystemFailure:
            result.status =
                UpgradeAllOperationStatus::StoppedOnSystemFailure;
            result.stopped_phase = UpgradeAllOperationPhase::System;
            set_not_attempted_after(
                result, UpgradeAllNotAttemptedReason::SystemFailure);
            return true;
        case SystemSourceUpgradeStatus::StoppedOnSourceFailure:
            result.status =
                UpgradeAllOperationStatus::StoppedOnSourceFailure;
            result.stopped_phase =
                UpgradeAllOperationPhase::RegisteredSource;
            set_not_attempted_after(
                result, UpgradeAllNotAttemptedReason::SourceFailure);
            return true;
        case SystemSourceUpgradeStatus::
            StoppedAfterSourceCleanupFailure:
            result.status = UpgradeAllOperationStatus::
                StoppedAfterSourceCleanupFailure;
            result.stopped_phase =
                UpgradeAllOperationPhase::RegisteredSource;
            set_not_attempted_after(
                result,
                UpgradeAllNotAttemptedReason::SourceCleanupFailure);
            return true;
        case SystemSourceUpgradeStatus::BlockedBeforeMutation:
            result.status = UpgradeAllOperationStatus::InconsistentResult;
            result.stopped_phase = UpgradeAllOperationPhase::Preparation;
            set_not_attempted_after(
                result,
                UpgradeAllNotAttemptedReason::
                    PriorAggregateInconsistency);
            return true;
        case SystemSourceUpgradeStatus::InconsistentResult:
            result.status = UpgradeAllOperationStatus::InconsistentResult;
            result.stopped_phase = aggregate_phase_for_system_source(
                result.system_source.stopped_phase);
            set_not_attempted_after(
                result,
                UpgradeAllNotAttemptedReason::
                    PriorAggregateInconsistency);
            return true;
    }

    if(result.system_source.is_success()) return false;

    if(has_non_successful_source(result.system_source)) {
        result.status = UpgradeAllOperationStatus::StoppedOnSourceFailure;
        result.stopped_phase =
            UpgradeAllOperationPhase::RegisteredSource;
        set_not_attempted_after(
            result, UpgradeAllNotAttemptedReason::SourceFailure);
        return true;
    }

    // TRANSLATORS: The placeholder is the literal service name "AUR".
    const std::string diagnostic = localization::format_translated_message(
        "The system/source phase completed without a fully successful typed result; {} processing did not start.",
        AUR_SERVICE);
    result.status =
        UpgradeAllOperationStatus::StoppedBeforeAurExecution;
    result.stopped_phase = UpgradeAllOperationPhase::System;
    result.issues.push_back(make_issue(
        UpgradeAllOperationIssueKind::SystemSourcePhaseIncomplete,
        UpgradeAllOperationPhase::System,
        diagnostic));
    add_stopping_diagnostic(
        result, UpgradeAllOperationPhase::System, diagnostic);
    set_not_attempted_after(
        result,
        UpgradeAllNotAttemptedReason::SystemSourceIncomplete);
    return true;
}

PackageMetadataFailure generic_metadata_failure(
    PackageMetadataErrorCode code,
    const std::string& diagnostic) {
    return PackageMetadataFailure{code, diagnostic};
}

void stop_for_cache_authority_failure(
    UpgradeAllOperationResult& result,
    UpgradeAllOperationPhase stopped_phase,
    const std::string& diagnostic,
    std::optional<TrustedCacheFailure> trusted_failure = std::nullopt) {
    result.status = UpgradeAllOperationStatus::StoppedBeforeAurExecution;
    result.stopped_phase = stopped_phase;
    UpgradeAllOperationIssue issue = make_issue(
        UpgradeAllOperationIssueKind::CacheAuthorityInvalid,
        stopped_phase, diagnostic);
    issue.trusted_cache_failure = std::move(trusted_failure);
    result.issues.push_back(std::move(issue));
    add_stopping_diagnostic(result, stopped_phase, diagnostic);
    if(stopped_phase == UpgradeAllOperationPhase::ForeignInventory) {
        result.foreign_inventory.status =
            UpgradeAllForeignInventoryPhaseStatus::NotAttempted;
        result.foreign_inventory.not_attempted_reason =
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure;
    }
    result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
    result.aur.not_attempted_reason =
        UpgradeAllNotAttemptedReason::CacheAuthorityFailure;
}

bool capture_duplicate_exclusions(
    UpgradeAllOperationResult& aggregate,
    const FilteredAurUpdateExecutionResult& filtered) {
    bool is_consistent = true;
    const UpgradeAllPlan& plan = filtered.upgrade_all_plan;
    for(const std::size_t planner_index :
        plan.excluded_duplicate_target_indexes) {
        const bool in_range =
            planner_index < plan.target_dispositions.size() &&
            planner_index < filtered.target_adapter.planner_target_to_original_query_plan_index.size();
        if(!in_range) {
            UpgradeAllOperationIssue issue = make_issue(
                UpgradeAllOperationIssueKind::
                    DuplicateExclusionCorrelationInconsistent,
                UpgradeAllOperationPhase::Reduction,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal
                    // service name "AUR".
                    "The duplicate-excluded {} target has no planner/query correlation.",
                    AUR_SERVICE));
            issue.adapter_index = planner_index;
            aggregate.issues.push_back(std::move(issue));
            is_consistent = false;
            continue;
        }

        const std::size_t original_query_index =
            filtered.target_adapter.planner_target_to_original_query_plan_index[planner_index];
        if(original_query_index >= filtered.query_result.plan.entries.size()) {
            UpgradeAllOperationIssue issue = make_issue(
                UpgradeAllOperationIssueKind::
                    DuplicateExclusionCorrelationInconsistent,
                UpgradeAllOperationPhase::Reduction,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal
                    // service name "AUR".
                    "The duplicate-excluded {} target maps outside the original query plan.",
                    AUR_SERVICE));
            issue.adapter_index = planner_index;
            issue.original_query_plan_index = original_query_index;
            aggregate.issues.push_back(std::move(issue));
            is_consistent = false;
            continue;
        }

        const UpgradeAllTargetPlanEntry& planner_entry =
            plan.target_dispositions[planner_index];
        const AurUpdatePlanEntry& query_entry =
            filtered.query_result.plan.entries[original_query_index];
        if(planner_entry.original_target_index != planner_index ||
           planner_entry.target.package_name != query_entry.installed_name) {
            UpgradeAllOperationIssue issue = make_issue(
                UpgradeAllOperationIssueKind::
                    DuplicateExclusionCorrelationInconsistent,
                UpgradeAllOperationPhase::Reduction,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal
                    // service name "AUR".
                    "The duplicate-excluded {} target identity differs from its original query entry.",
                    AUR_SERVICE));
            issue.adapter_index = planner_index;
            issue.original_query_plan_index = original_query_index;
            issue.package_name = query_entry.installed_name;
            aggregate.issues.push_back(std::move(issue));
            is_consistent = false;
            continue;
        }

        aggregate.duplicate_excluded_aur_targets.push_back(
            UpgradeAllDuplicateExcludedAurTarget{
                planner_index,
                original_query_index,
                planner_entry,
                query_entry});
    }
    return is_consistent;
}

bool has_exact_external_child_identity(
    const AurUpdateExternallySatisfiedBuildUnit& unit,
    const FilteredAurUpdateBuildUnitCorrelation& correlation) noexcept {
    if(correlation.package_names.empty() ||
       unit.plan_package_names != correlation.package_names ||
       unit.required_target_attributions.size() !=
           correlation.package_names.size()) {
        return false;
    }

    const bool is_singular = correlation.package_names.size() == 1;
    if((is_singular &&
        unit.package_name != correlation.package_names.front()) ||
       (!is_singular && !unit.package_name.empty())) {
        return false;
    }

    for(std::size_t child_index = 0;
        child_index < unit.required_target_attributions.size();
        ++child_index) {
        const RequiredPackageArtifactTarget& required_target =
            unit.required_target_attributions[child_index]
                .required_target;
        if(required_target.package_base != unit.package_base ||
           required_target.package_name !=
               correlation.package_names[child_index] ||
           std::find(
               correlation.package_names.begin(),
               correlation.package_names.begin() + child_index,
               required_target.package_name) !=
               correlation.package_names.begin() + child_index) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> external_singular_package_name(
    const AurUpdateExternallySatisfiedBuildUnit& unit) {
    if(unit.required_target_attributions.size() != 1) return std::nullopt;
    return unit.required_target_attributions.front()
        .required_target.package_name;
}

bool capture_external_satisfaction(
    UpgradeAllOperationResult& aggregate,
    const FilteredAurUpdateExecutionResult& filtered) {
    bool is_consistent =
        filtered.preparation.externally_satisfied_build_units.size() ==
        filtered.upgrade_all_plan.externally_satisfied_build_unit_indexes.size();
    for(const AurUpdateExternallySatisfiedBuildUnit& unit :
        filtered.preparation.externally_satisfied_build_units) {
        if(unit.build_plan_order_index >=
           filtered.build_unit_correlations.size()) {
            UpgradeAllOperationIssue issue = make_issue(
                UpgradeAllOperationIssueKind::
                    ExternalSatisfactionCorrelationInconsistent,
                UpgradeAllOperationPhase::Reduction,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // service name "AUR" and correlation label "PR3".
                    "The externally satisfied {} build unit has no {} root-role correlation.",
                    AUR_SERVICE, "PR3"));
            issue.build_plan_order_index = unit.build_plan_order_index;
            issue.package_name = external_singular_package_name(unit);
            aggregate.issues.push_back(std::move(issue));
            is_consistent = false;
            continue;
        }
        const FilteredAurUpdateBuildUnitCorrelation& correlation =
            filtered.build_unit_correlations[unit.build_plan_order_index];
        if(correlation.original_build_plan_index !=
               unit.build_plan_order_index ||
           correlation.package_base != unit.package_base ||
           !has_exact_external_child_identity(unit, correlation)) {
            UpgradeAllOperationIssue issue = make_issue(
                UpgradeAllOperationIssueKind::
                    ExternalSatisfactionCorrelationInconsistent,
                UpgradeAllOperationPhase::Reduction,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // service name "AUR" and correlation label "PR3".
                    "The externally satisfied {} build-unit child identity differs from its {} correlation.",
                    AUR_SERVICE, "PR3"));
            issue.build_plan_order_index = unit.build_plan_order_index;
            issue.package_name = external_singular_package_name(unit);
            aggregate.issues.push_back(std::move(issue));
            is_consistent = false;
            continue;
        }
        aggregate.externally_satisfied_aur_build_units.push_back(
            UpgradeAllExternallySatisfiedAurBuildUnit{
                unit,
                correlation.root_correlations});
    }

    if(!is_consistent &&
       aggregate.externally_satisfied_aur_build_units.empty() &&
       !filtered.preparation.externally_satisfied_build_units.empty()) {
        return false;
    }
    return is_consistent;
}

void append_aur_warnings(
    UpgradeAllOperationResult& aggregate,
    const FilteredAurUpdateExecutionResult& filtered) {
    for(const AurUpdatePreparationWarning& warning :
        filtered.reduced_operation_result.preparation_warnings) {
        aggregate.warnings.push_back(UpgradeAllOperationWarning{
            UpgradeAllOperationWarningKind::AurPreparation,
            UpgradeAllOperationPhase::AurPreparation,
            std::nullopt,
            warning.preference_name,
            warning.diagnostic});
    }
}

void map_filtered_result_status(UpgradeAllOperationResult& aggregate) {
    FilteredAurUpdateExecutionResult& filtered =
        *aggregate.aur.operation_result;
    append_aur_warnings(aggregate, filtered);
    const bool duplicate_correlation_valid =
        capture_duplicate_exclusions(aggregate, filtered);
    const bool external_correlation_valid =
        capture_external_satisfaction(aggregate, filtered);

    if(!duplicate_correlation_valid || !external_correlation_valid ||
       !filtered.issues.empty() ||
       !filtered.reduced_operation_result.reduction_issues.empty() ||
       filtered.reduced_operation_result.status ==
           AurUpdateOperationStatus::InconsistentResult) {
        aggregate.status = UpgradeAllOperationStatus::InconsistentResult;
        aggregate.stopped_phase = UpgradeAllOperationPhase::Reduction;
        aggregate.aur.status =
            UpgradeAllAurPhaseStatus::InconsistentResult;
        return;
    }

    if(filtered.has_query_failure()) {
        aggregate.status =
            UpgradeAllOperationStatus::StoppedBeforeAurExecution;
        aggregate.stopped_phase = UpgradeAllOperationPhase::AurQuery;
        aggregate.aur.status =
            UpgradeAllAurPhaseStatus::BlockedBeforeExecution;
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        aggregate.aur.diagnostic = localization::format_translated_message(
            "The {} update query completed with recoverable failures; filtered execution did not start.",
            AUR_SERVICE);
        return;
    }

    switch(filtered.reduced_operation_result.status) {
        case AurUpdateOperationStatus::NoUpdates:
            if(!filtered.is_success()) {
                aggregate.status =
                    UpgradeAllOperationStatus::InconsistentResult;
                aggregate.stopped_phase =
                    UpgradeAllOperationPhase::Reduction;
                aggregate.aur.status =
                    UpgradeAllAurPhaseStatus::InconsistentResult;
                return;
            }
            aggregate.aur.status = UpgradeAllAurPhaseStatus::NoUpdates;
            aggregate.status = UpgradeAllOperationStatus::Completed;
            aggregate.stopped_phase = UpgradeAllOperationPhase::None;
            return;
        case AurUpdateOperationStatus::Completed:
            if(!filtered.is_success()) {
                aggregate.status =
                    UpgradeAllOperationStatus::InconsistentResult;
                aggregate.stopped_phase =
                    UpgradeAllOperationPhase::Reduction;
                aggregate.aur.status =
                    UpgradeAllAurPhaseStatus::InconsistentResult;
                return;
            }
            aggregate.aur.status = UpgradeAllAurPhaseStatus::Completed;
            aggregate.status = UpgradeAllOperationStatus::Completed;
            aggregate.stopped_phase = UpgradeAllOperationPhase::None;
            return;
        case AurUpdateOperationStatus::BlockedBeforeExecution:
            aggregate.status =
                UpgradeAllOperationStatus::StoppedBeforeAurExecution;
            aggregate.stopped_phase =
                UpgradeAllOperationPhase::AurPreparation;
            aggregate.aur.status =
                UpgradeAllAurPhaseStatus::BlockedBeforeExecution;
            return;
        case AurUpdateOperationStatus::
            StoppedOnProviderTransactionFailure:
            aggregate.status =
                UpgradeAllOperationStatus::StoppedOnAurFailure;
            aggregate.stopped_phase =
                UpgradeAllOperationPhase::AurExecution;
            aggregate.aur.status = UpgradeAllAurPhaseStatus::
                StoppedOnProviderTransactionFailure;
            return;
        case AurUpdateOperationStatus::StoppedOnWorkItemCancellation:
            aggregate.status = UpgradeAllOperationStatus::StoppedOnAurCancellation;
            aggregate.stopped_phase = UpgradeAllOperationPhase::AurExecution;
            aggregate.aur.status = UpgradeAllAurPhaseStatus::StoppedOnWorkItemCancellation;
            return;
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
            aggregate.status = UpgradeAllOperationStatus::StoppedOnAurFailure;
            aggregate.stopped_phase = UpgradeAllOperationPhase::AurExecution;
            aggregate.aur.status =
                UpgradeAllAurPhaseStatus::StoppedOnWorkItemFailure;
            return;
        case AurUpdateOperationStatus::
            StoppedAfterPackageCleanupFailure:
            aggregate.status = UpgradeAllOperationStatus::
                StoppedAfterAurCleanupFailure;
            aggregate.stopped_phase = UpgradeAllOperationPhase::AurExecution;
            aggregate.aur.status =
                UpgradeAllAurPhaseStatus::StoppedAfterCleanupFailure;
            return;
        case AurUpdateOperationStatus::InconsistentResult:
            aggregate.status =
                UpgradeAllOperationStatus::InconsistentResult;
            aggregate.stopped_phase = UpgradeAllOperationPhase::Reduction;
            aggregate.aur.status =
                UpgradeAllAurPhaseStatus::InconsistentResult;
            return;
    }
}

bool all_registered_sources_no_change(
    const SystemSourceUpgradeResult& result) noexcept {
    return std::all_of(
        result.registered_source_results.begin(),
        result.registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return source.status ==
                       RegisteredSourceUpgradeStatus::NoChange &&
                   source.package_state_change ==
                       PackageStateChange::NoChange;
        });
}

bool qualifies_as_no_updates(
    const UpgradeAllOperationResult& result) noexcept {
    if(result.system_source.system.status !=
           SystemUpgradePhaseStatus::Completed ||
       result.system_source.system.package_state_change !=
           PackageStateChange::NoChange ||
       !all_registered_sources_no_change(result.system_source) ||
       result.foreign_inventory.status !=
           UpgradeAllForeignInventoryPhaseStatus::Completed ||
       !result.aur.operation_result.has_value() ||
       !result.aur.operation_result->is_success() ||
       (result.aur.status != UpgradeAllAurPhaseStatus::NoUpdates &&
        result.aur.status != UpgradeAllAurPhaseStatus::Completed) ||
       result.package_state_change() != PackageStateChange::NoChange ||
       result.has_cleanup_failure() || result.has_query_failure() ||
       result.has_planning_issue() || result.has_not_attempted_phase() ||
       result.has_inconsistency()) {
        return false;
    }
    return true;
}

bool stop_for_aur_preflight_failure(
    UpgradeAllOperationResult& result,
    const PreparedUpgradeAllAurPreflight& preflight) {
    if(preflight.stopped_phase() == UpgradeAllOperationPhase::None) {
        return false;
    }

    const bool is_inconsistent = std::any_of(
        preflight.issues().begin(), preflight.issues().end(),
        [](const UpgradeAllOperationIssue& issue) {
            return issue.kind == UpgradeAllOperationIssueKind::
                                     FilteredAurExecutionFailed;
        });
    result.status = is_inconsistent
                        ? UpgradeAllOperationStatus::InconsistentResult
                        : UpgradeAllOperationStatus::StoppedBeforeAurExecution;
    result.stopped_phase = preflight.stopped_phase();
    if(result.stopped_phase == UpgradeAllOperationPhase::ForeignInventory) {
        result.aur.status = UpgradeAllAurPhaseStatus::NotAttempted;
        result.aur.not_attempted_reason =
            UpgradeAllNotAttemptedReason::ForeignInventoryFailure;
    } else if(is_inconsistent) {
        result.aur.status = UpgradeAllAurPhaseStatus::InconsistentResult;
    } else {
        result.aur.status = UpgradeAllAurPhaseStatus::BlockedBeforeExecution;
    }
    result.aur.diagnostic = preflight.diagnostic();
    result.issues.insert(
        result.issues.end(),
        preflight.issues().begin(),
        preflight.issues().end());
    if(preflight.diagnostic().has_value()) {
        add_stopping_diagnostic(
            result,
            preflight.stopped_phase(),
            preflight.diagnostic().value());
    }
    return true;
}

} // namespace

struct PreparedUpgradeAllOperation::Impl {
    Impl(
        UpgradeAllOperationPreparedSnapshot prepared_snapshot,
        PreparedSystemSourceUpgrade prepared_system_source)
        : snapshot(std::move(prepared_snapshot)),
          system_source(std::move(prepared_system_source)) {
    }

    UpgradeAllOperationPreparedSnapshot snapshot;
    PreparedSystemSourceUpgrade system_source;
};

struct UpgradeAllOperationPreparationAccess {
    static PreparedUpgradeAllOperation make(
        UpgradeAllOperationPreparedSnapshot snapshot,
        PreparedSystemSourceUpgrade system_source) {
        return PreparedUpgradeAllOperation(
            std::make_unique<PreparedUpgradeAllOperation::Impl>(
                std::move(snapshot),
                std::move(system_source)));
    }
};

bool UpgradeAllExplicitSourceIdentityAdapter::is_valid() const noexcept {
    return issues.empty();
}

std::vector<UpgradeAllExplicitSourceIdentity>
UpgradeAllExplicitSourceIdentityAdapter::planner_identities() const {
    std::vector<UpgradeAllExplicitSourceIdentity> identities;
    identities.reserve(entries.size());
    for(const UpgradeAllExplicitSourceAdapterEntry& entry : entries) {
        identities.push_back(entry.planner_identity);
    }
    return identities;
}

UpgradeAllExplicitSourceIdentityAdapter
adapt_prepared_source_identities_for_upgrade_all(
    const SystemSourceUpgradePreparedSnapshot& source_snapshot) {
    UpgradeAllExplicitSourceIdentityAdapter adapter;
    adapter.entries.reserve(source_snapshot.registered_sources.size());
    std::set<std::size_t> original_preference_indexes;

    for(std::size_t adapter_index = 0;
        adapter_index < source_snapshot.registered_sources.size();
        ++adapter_index) {
        const RegisteredSourcePreferenceSnapshot& source =
            source_snapshot.registered_sources[adapter_index];
        std::vector<std::string> affected_package_names;
        if(!source.preference_package_name.empty()) {
            affected_package_names.push_back(
                source.preference_package_name);
        } else {
            adapter.issues.push_back(make_adapter_issue(
                UpgradeAllExplicitSourceAdapterIssueKind::
                    PreferencePackageNameMissing,
                adapter_index,
                source,
                localization::translate_message(
                    "The registered source preference has no package name.")));
        }

        UpgradeAllPackageBaseIdentity package_base =
            UpgradeAllPackageBaseAbsent{};
        if(source.resolved_package_base.has_value() &&
           !source.resolved_package_base->empty()) {
            package_base = UpgradeAllResolvedPackageBase{
                *source.resolved_package_base};
        } else {
            adapter.issues.push_back(make_adapter_issue(
                UpgradeAllExplicitSourceAdapterIssueKind::
                    PackageBaseUnavailable,
                adapter_index,
                source,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal
                    // field name "PackageBase".
                    "The registered source preference has no prepared {} identity.",
                    PACKAGE_BASE_FIELD)));
        }

        UpgradeAllSourceIdentity source_identity =
            UpgradeAllSourceIdentityAbsent{};
        if(source.canonical_source_identity_key.has_value() &&
           !source.canonical_source_identity_key->empty()) {
            source_identity = UpgradeAllResolvedSourceIdentity{
                *source.canonical_source_identity_key};
        } else {
            adapter.issues.push_back(make_adapter_issue(
                UpgradeAllExplicitSourceAdapterIssueKind::
                    CanonicalSourceIdentityUnavailable,
                adapter_index,
                source,
                localization::translate_message(
                    "The registered source preference has no prepared canonical source identity.")));
        }

        if(!original_preference_indexes.insert(
                                           source.original_preference_index)
                .second) {
            adapter.issues.push_back(make_adapter_issue(
                UpgradeAllExplicitSourceAdapterIssueKind::
                    DuplicateOriginalPreferenceIndex,
                adapter_index,
                source,
                localization::translate_message(
                    "The registered source preferences contain a duplicate original index.")));
        }

        UpgradeAllExplicitSourceIdentity planner_identity{
            source.preference_package_name,
            std::move(package_base),
            affected_package_names,
            std::move(source_identity)};
        adapter.entries.push_back(UpgradeAllExplicitSourceAdapterEntry{
            adapter_index,
            source.original_preference_index,
            source.preference_package_name,
            source.canonical_source_identity_key,
            source.resolved_package_base,
            std::move(affected_package_names),
            std::move(planner_identity)});
    }
    return adapter;
}

PreparedUpgradeAllOperation::PreparedUpgradeAllOperation(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
    if(impl_ == nullptr) return;
    const SystemSourceUpgradeProjectionAuthority* system_source =
        impl_->system_source.projection_authority();
    if(system_source == nullptr) return;
    UpgradeAllOperationProjectionAuthority authority(
        impl_->snapshot, *system_source);
    projection_authority_.emplace(std::move(authority));
}

PreparedUpgradeAllOperation::PreparedUpgradeAllOperation(
    PreparedUpgradeAllOperation&& other) noexcept
    : impl_(std::move(other.impl_)),
      projection_authority_(std::move(other.projection_authority_)) {
    other.projection_authority_.reset();
}

PreparedUpgradeAllOperation::~PreparedUpgradeAllOperation() noexcept = default;

bool PreparedUpgradeAllOperation::is_valid() const noexcept {
    return impl_ != nullptr;
}

const UpgradeAllOperationPreparedSnapshot*
PreparedUpgradeAllOperation::snapshot() const noexcept {
    return impl_ == nullptr ? nullptr : &impl_->snapshot;
}

const UpgradeAllOperationProjectionAuthority*
PreparedUpgradeAllOperation::projection_authority() const noexcept {
    return impl_ != nullptr && projection_authority_.has_value()
               ? &projection_authority_.value()
               : nullptr;
}

#ifdef MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS
void PreparedUpgradeAllOperation::
    make_source_snapshot_inconsistent_for_test() {
    if(impl_ == nullptr ||
       impl_->snapshot.system_source.registered_sources.empty()) {
        return;
    }
    impl_->snapshot.system_source.registered_sources.front().preference_package_name += "-outer-corruption";
}

void PreparedUpgradeAllOperation::
    make_explicit_source_correlation_inconsistent_for_test() {
    if(impl_ == nullptr ||
       impl_->snapshot.explicit_source_adapter.entries.empty()) {
        return;
    }
    ++impl_->snapshot.explicit_source_adapter.entries.front().adapter_index;
}

void PreparedUpgradeAllOperation::
    make_nested_system_source_correlation_inconsistent_for_test() {
#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
    if(impl_ != nullptr) {
        impl_->system_source.make_first_source_correlation_inconsistent_for_test();
    }
#endif
}

#ifdef MOGUET_ENABLE_SYSTEM_SOURCE_UPGRADE_TEST_HOOKS
void PreparedUpgradeAllOperation::
    set_nested_system_source_unexpected_exception_for_test(
        SystemSourceUpgradeUnexpectedExceptionPoint point,
        bool unknown_exception) {
    if(impl_ == nullptr) return;
    impl_->system_source.set_unexpected_exception_for_test(
        point, unknown_exception);
}
#endif
#endif

void PreparedUpgradeAllAurPreflight::prepare_foreign_inventory_stage() {
    if(stopped_phase_ != UpgradeAllOperationPhase::None) return;
    try {
        foreign_inventory_.repository_configuration =
            resolve_pacman_repository_configuration();
    } catch(const PackageMetadataError& error) {
        foreign_inventory_.status =
            UpgradeAllForeignInventoryPhaseStatus::Failed;
        foreign_inventory_.failure = error.failure();
        foreign_inventory_.diagnostic = error.what();
        UpgradeAllOperationIssue issue = make_issue(
            UpgradeAllOperationIssueKind::
                ForeignInventoryConfigurationFailed,
            UpgradeAllOperationPhase::ForeignInventory,
            error.what());
        issue.package_metadata_failure = error.failure();
        issues_.push_back(std::move(issue));
        stopped_phase_ = UpgradeAllOperationPhase::ForeignInventory;
        diagnostic_ = error.what();
        return;
    } catch(const std::exception& error) {
        PackageMetadataFailure failure = generic_metadata_failure(
            PackageMetadataErrorCode::ConfigurationUnavailable,
            error.what());
        foreign_inventory_.status =
            UpgradeAllForeignInventoryPhaseStatus::Failed;
        foreign_inventory_.failure = failure;
        foreign_inventory_.diagnostic = error.what();
        UpgradeAllOperationIssue issue = make_issue(
            UpgradeAllOperationIssueKind::
                ForeignInventoryConfigurationFailed,
            UpgradeAllOperationPhase::ForeignInventory,
            error.what());
        issue.package_metadata_failure = std::move(failure);
        issues_.push_back(std::move(issue));
        stopped_phase_ = UpgradeAllOperationPhase::ForeignInventory;
        diagnostic_ = error.what();
        return;
    } catch(...) {
        const std::string diagnostic = localization::translate_message(
            "Foreign inventory configuration resolution failed with an unknown exception.");
        PackageMetadataFailure failure = generic_metadata_failure(
            PackageMetadataErrorCode::ConfigurationUnavailable,
            diagnostic);
        foreign_inventory_.status =
            UpgradeAllForeignInventoryPhaseStatus::Failed;
        foreign_inventory_.failure = failure;
        foreign_inventory_.diagnostic = diagnostic;
        UpgradeAllOperationIssue issue = make_issue(
            UpgradeAllOperationIssueKind::
                ForeignInventoryConfigurationFailed,
            UpgradeAllOperationPhase::ForeignInventory,
            diagnostic);
        issue.package_metadata_failure = std::move(failure);
        issues_.push_back(std::move(issue));
        stopped_phase_ = UpgradeAllOperationPhase::ForeignInventory;
        diagnostic_ = diagnostic;
        return;
    }

    ForeignPackageInventoryResult inventory_result;
    try {
        inventory_result = query_foreign_package_inventory(
            foreign_inventory_.repository_configuration.value());
    } catch(const PackageMetadataError& error) {
        inventory_result = error.failure();
    } catch(const std::exception& error) {
        inventory_result = generic_metadata_failure(
            PackageMetadataErrorCode::QueryFailed, error.what());
    } catch(...) {
        inventory_result = generic_metadata_failure(
            PackageMetadataErrorCode::QueryFailed,
            localization::translate_message(
                "Foreign inventory read failed with an unknown exception."));
    }
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&inventory_result)) {
        foreign_inventory_.status =
            UpgradeAllForeignInventoryPhaseStatus::Failed;
        foreign_inventory_.failure = *failure;
        foreign_inventory_.diagnostic = failure->diagnostic;
        UpgradeAllOperationIssue issue = make_issue(
            UpgradeAllOperationIssueKind::ForeignInventoryReadFailed,
            UpgradeAllOperationPhase::ForeignInventory,
            failure->diagnostic);
        issue.package_metadata_failure = *failure;
        issues_.push_back(std::move(issue));
        stopped_phase_ = UpgradeAllOperationPhase::ForeignInventory;
        diagnostic_ = failure->diagnostic;
        return;
    }
    foreign_inventory_.status =
        UpgradeAllForeignInventoryPhaseStatus::Completed;
    foreign_inventory_.not_attempted_reason.reset();
    foreign_inventory_.inventory =
        std::get<ForeignPackageInventory>(std::move(inventory_result));
}

void PreparedUpgradeAllAurPreflight::prepare_aur_query_stage() {
    if(stopped_phase_ != UpgradeAllOperationPhase::None ||
       foreign_inventory_.status !=
           UpgradeAllForeignInventoryPhaseStatus::Completed) {
        return;
    }
    try {
        // Keep the fresh inventory as observable authority while the query
        // consumes its own owned copy.
        aur_query_result_.emplace(
            query_aur_updates_for_foreign_inventory(
                foreign_inventory_.inventory));
    } catch(const std::exception& error) {
        issues_.push_back(make_issue(
            UpgradeAllOperationIssueKind::AurQueryFailed,
            UpgradeAllOperationPhase::AurQuery,
            error.what()));
        stopped_phase_ = UpgradeAllOperationPhase::AurQuery;
        diagnostic_ = error.what();
        return;
    } catch(...) {
        const std::string diagnostic =
            localization::format_translated_message(
                "The {} update query failed with an unknown exception.",
                AUR_SERVICE);
        issues_.push_back(make_issue(
            UpgradeAllOperationIssueKind::AurQueryFailed,
            UpgradeAllOperationPhase::AurQuery,
            diagnostic));
        stopped_phase_ = UpgradeAllOperationPhase::AurQuery;
        diagnostic_ = diagnostic;
        return;
    }
}

void PreparedUpgradeAllAurPreflight::prepare_filtered_operation_stage(
    const UpgradeAllOperationPreparedSnapshot& prepared,
    const AppConfig& config,
    std::optional<ValidatedCacheRoot> cache_root) {
    if(stopped_phase_ != UpgradeAllOperationPhase::None ||
       !aur_query_result_.has_value()) {
        return;
    }
    std::vector<UpgradeAllExplicitSourceIdentity> explicit_sources =
        prepared.explicit_source_adapter.planner_identities();
    AurUpdateQueryResult query_result =
        std::move(aur_query_result_.value());
    aur_query_result_.reset();
    try {
        filtered_operation_.emplace(
            prepare_filtered_aur_update_operation(
                std::move(query_result),
                UpgradeAllExplicitSourceSatisfaction{
                    std::move(explicit_sources)},
                DevelRequiresCheckPolicy::BlockOperation,
                SavedSourcePreferencePolicy::Strict,
                config,
                std::move(cache_root)));
    } catch(const TrustedCacheError&) {
        throw;
    } catch(const std::logic_error& error) {
        issues_.push_back(make_issue(
            UpgradeAllOperationIssueKind::FilteredAurExecutionFailed,
            UpgradeAllOperationPhase::AurPreparation,
            error.what()));
        stopped_phase_ = UpgradeAllOperationPhase::AurPreparation;
        diagnostic_ = error.what();
    } catch(const std::exception& error) {
        issues_.push_back(make_issue(
            UpgradeAllOperationIssueKind::FilteredAurPreparationFailed,
            UpgradeAllOperationPhase::AurPreparation,
            error.what()));
        stopped_phase_ = UpgradeAllOperationPhase::AurPreparation;
        diagnostic_ = error.what();
    } catch(...) {
        const std::string diagnostic =
            localization::format_translated_message(
                "The filtered {} operation failed with an unknown exception.",
                AUR_SERVICE);
        issues_.push_back(make_issue(
            UpgradeAllOperationIssueKind::FilteredAurExecutionFailed,
            UpgradeAllOperationPhase::AurPreparation,
            diagnostic));
        stopped_phase_ = UpgradeAllOperationPhase::AurPreparation;
        diagnostic_ = diagnostic;
    }
}

PreparedUpgradeAllAurPreflight prepare_upgrade_all_aur_preflight(
    const UpgradeAllOperationPreparedSnapshot& prepared,
    const AppConfig& config) {
    PreparedUpgradeAllAurPreflight preflight;
    preflight.prepare_foreign_inventory_stage();
    preflight.prepare_aur_query_stage();
    preflight.prepare_filtered_operation_stage(
        prepared, config, std::nullopt);
    return preflight;
}

UpgradeAllOperationPreparation prepare_upgrade_all_operation(
    const AppConfig& config) {
    try {
        // Preparation is a read-only production preflight. Actual execution
        // acquires one cache authority for registered-source and AUR phases.
        require_supported_production_source_build_options(config);
        SystemSourceUpgradePreparation source_preparation =
            prepare_system_source_upgrade(config);
        if(auto* blocked =
               std::get_if<SystemSourceUpgradeResult>(
                   &source_preparation)) {
            return make_blocked_preparation_result(std::move(*blocked));
        }

        PreparedSystemSourceUpgrade prepared_source = std::move(
            std::get<PreparedSystemSourceUpgrade>(source_preparation));
        const SystemSourceUpgradePreparedSnapshot* source_snapshot =
            prepared_source.snapshot();
        if(source_snapshot == nullptr) {
            return make_preexecution_rejection(
                {},
                UpgradeAllOperationIssueKind::
                    PreparedCapabilityConsumed,
                consumed_capability_diagnostic());
        }

        UpgradeAllOperationPreparedSnapshot snapshot =
            make_prepared_snapshot(*source_snapshot);
        if(!snapshot.explicit_source_adapter.is_valid()) {
            UpgradeAllOperationResult result = make_preexecution_rejection(
                std::move(snapshot),
                UpgradeAllOperationIssueKind::
                    ExplicitSourceAdapterInvalid,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the literal
                    // command name "upgrade-all".
                    "The prepared registered source identity cannot be adapted safely for {} planning.",
                    COMMAND_NAME),
                UpgradeAllOperationStatus::BlockedBeforeMutation);
            append_adapter_issues(
                result,
                result.prepared_snapshot.explicit_source_adapter);
            return result;
        }

        return UpgradeAllOperationPreparationAccess::make(
            std::move(snapshot), std::move(prepared_source));
    } catch(const std::exception& error) {
        UpgradeAllOperationResult result = make_preexecution_rejection(
            {},
            UpgradeAllOperationIssueKind::UnknownFailure,
            error.what(),
            UpgradeAllOperationStatus::BlockedBeforeMutation);
        return result;
    } catch(...) {
        UpgradeAllOperationResult result = make_preexecution_rejection(
            {},
            UpgradeAllOperationIssueKind::UnknownFailure,
            localization::format_translated_message(
                // TRANSLATORS: The placeholder is the display spelling
                // of the literal command name "upgrade-all".
                "{} preparation failed with an unknown exception.",
                COMMAND_SENTENCE_NAME),
            UpgradeAllOperationStatus::BlockedBeforeMutation);
        return result;
    }
}

UpgradeAllOperationResult execute_prepared_upgrade_all_operation(
    PreparedUpgradeAllOperation prepared,
    const AppConfig& config) {
    if(prepared.impl_ == nullptr) {
        return make_preexecution_rejection(
            {},
            UpgradeAllOperationIssueKind::PreparedCapabilityConsumed,
            consumed_capability_diagnostic());
    }

    UpgradeAllOperationPreparedSnapshot snapshot =
        prepared.impl_->snapshot;
    const SystemSourceUpgradePreparedSnapshot* nested_snapshot =
        prepared.impl_->system_source.snapshot();
    if(!options_match(snapshot.system_source.options, config)) {
        return make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::OptionSnapshotMismatch,
            option_mismatch_diagnostic());
    }
    if(nested_snapshot == nullptr ||
       !source_snapshots_match(snapshot.system_source, *nested_snapshot)) {
        return make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::SourceSnapshotMismatch,
            source_snapshot_mismatch_diagnostic());
    }
    if(!adapter_matches_snapshot(
           snapshot.explicit_source_adapter,
           snapshot.system_source)) {
        return make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::
                ExplicitSourceCorrelationInconsistent,
            source_correlation_mismatch_diagnostic());
    }

    std::optional<ValidatedCacheRoot> execution_cache_root;
    try {
        // Preparation intentionally retained no filesystem capability. Adopt
        // current state only after every static correlation/option guard.
        execution_cache_root = prepare_process_cache_root();
    } catch(const xdg_paths::ResolutionError& error) {
        UpgradeAllOperationResult result = make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::CacheAuthorityInvalid,
            error.what(),
            UpgradeAllOperationStatus::BlockedBeforeMutation);
        result.issues.back().cache_resolution_failure = error.failure();
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure);
        return result;
    } catch(const xdg_directory_safety::PreparationError& error) {
        UpgradeAllOperationResult result = make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::CacheAuthorityInvalid,
            error.what(),
            UpgradeAllOperationStatus::BlockedBeforeMutation);
        result.issues.back().cache_preparation_failure = error.failure();
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure);
        return result;
    } catch(const TrustedCacheError& error) {
        UpgradeAllOperationResult result = make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::CacheAuthorityInvalid,
            error.what(),
            UpgradeAllOperationStatus::BlockedBeforeMutation);
        result.issues.back().trusted_cache_failure = error.failure();
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure);
        return result;
    } catch(const std::exception& error) {
        UpgradeAllOperationResult result = make_preexecution_rejection(
            std::move(snapshot),
            UpgradeAllOperationIssueKind::CacheAuthorityInvalid,
            error.what(),
            UpgradeAllOperationStatus::BlockedBeforeMutation);
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::CacheAuthorityFailure);
        return result;
    }

    UpgradeAllOperationResult result;
    result.prepared_snapshot = snapshot;
    result.warnings = snapshot.warnings;

    SystemSourceUpgradePhase observed_system_source_phase =
        SystemSourceUpgradePhase::Preparation;
    const SystemSourceUpgradeEventObserver progress_observer =
        [&observed_system_source_phase](
            const SystemSourceUpgradeEvent& event) noexcept {
            switch(event.kind) {
                case SystemSourceUpgradeEventKind::LoadingSourcePreference:
                case SystemSourceUpgradeEventKind::SourcePreferenceWarning:
                    return;
                case SystemSourceUpgradeEventKind::SystemUpgradeStarting:
                    observed_system_source_phase =
                        SystemSourceUpgradePhase::System;
                    return;
                case SystemSourceUpgradeEventKind::CheckingSourcePackages:
                case SystemSourceUpgradeEventKind::InvalidPreferenceWarning:
                    observed_system_source_phase =
                        SystemSourceUpgradePhase::RegisteredSource;
                    return;
            }
        };

    try {
        // POLICY(#281): noexcept observerはnested result自体も返せない例外時の
        // 最終防御専用。通常のpartial typed result保持はnested executorが担う。
        result.system_source = execute_prepared_system_source_upgrade(
            std::move(prepared.impl_->system_source),
            config,
            progress_observer,
            execution_cache_root.value());
    } catch(const ConfirmationOperationStopped&) {
        throw;
    } catch(const std::exception& error) {
        const UpgradeAllOperationPhase stopped_phase =
            aggregate_phase_for_system_source(
                observed_system_source_phase);
        result.status = UpgradeAllOperationStatus::InconsistentResult;
        result.stopped_phase = stopped_phase;
        result.system_source = make_unavailable_system_source_result(
            snapshot.system_source,
            observed_system_source_phase,
            error.what());
        result.issues.push_back(make_issue(
            UpgradeAllOperationIssueKind::
                SystemSourceExecutionFailedUnexpectedly,
            stopped_phase,
            error.what()));
        add_stopping_diagnostic(
            result, stopped_phase, error.what());
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::PriorAggregateInconsistency);
        return result;
    } catch(...) {
        const std::string diagnostic = localization::translate_message(
            "System/source execution failed with an unknown exception.");
        const UpgradeAllOperationPhase stopped_phase =
            aggregate_phase_for_system_source(
                observed_system_source_phase);
        result.status = UpgradeAllOperationStatus::InconsistentResult;
        result.stopped_phase = stopped_phase;
        result.system_source = make_unavailable_system_source_result(
            snapshot.system_source,
            observed_system_source_phase,
            diagnostic);
        result.issues.push_back(make_issue(
            UpgradeAllOperationIssueKind::
                SystemSourceExecutionFailedUnexpectedly,
            stopped_phase,
            diagnostic));
        add_stopping_diagnostic(
            result, stopped_phase, diagnostic);
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::PriorAggregateInconsistency);
        return result;
    }

    if(!source_snapshots_match(
           result.system_source.prepared_snapshot,
           snapshot.system_source)) {
        // TRANSLATORS: The placeholder is the literal command name
        // "upgrade-all".
        const std::string diagnostic =
            localization::format_translated_message(
                "The system/source result no longer matches the prepared {} source intent snapshot.",
                COMMAND_NAME);
        result.status = UpgradeAllOperationStatus::InconsistentResult;
        result.stopped_phase = UpgradeAllOperationPhase::RegisteredSource;
        result.issues.push_back(make_issue(
            UpgradeAllOperationIssueKind::SourceSnapshotMismatch,
            UpgradeAllOperationPhase::RegisteredSource,
            diagnostic));
        add_stopping_diagnostic(
            result,
            UpgradeAllOperationPhase::RegisteredSource,
            diagnostic);
        set_not_attempted_after(
            result,
            UpgradeAllNotAttemptedReason::PriorAggregateInconsistency);
        return result;
    }

    const bool should_stop = stop_after_system_source_failure(result);
    if(should_stop) {
        if(result.status ==
           UpgradeAllOperationStatus::StoppedOnSystemFailure) {
            // The pacman failure and every later NotAttempted result are
            // already final. Correlation is secondary, read-only evidence and
            // cannot rewrite that primary outcome.
            observe_post_system_failure_cross_source_version_lock_correlation(
                result);
        }
        return result;
    }

    try {
        // The system/source phase may be long-running. Revalidate before the
        // fresh read-only repository/AUR authority is collected.
        execution_cache_root->require_unchanged_identity();
    } catch(const TrustedCacheError& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::ForeignInventory,
            error.what(), error.failure());
        return result;
    } catch(const std::exception& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::ForeignInventory,
            error.what());
        return result;
    }

    PreparedUpgradeAllAurPreflight aur_preflight;
    aur_preflight.prepare_foreign_inventory_stage();
    result.foreign_inventory = aur_preflight.foreign_inventory_;
    if(stop_for_aur_preflight_failure(result, aur_preflight)) {
        return result;
    }
    try {
        // Inventory resolution can invoke pacman metadata queries. A cache
        // replacement observed there must stop before AUR network access.
        execution_cache_root->require_unchanged_identity();
    } catch(const TrustedCacheError& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::AurQuery, error.what(),
            error.failure());
        return result;
    } catch(const std::exception& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::AurQuery, error.what());
        return result;
    }
    aur_preflight.prepare_aur_query_stage();
    if(stop_for_aur_preflight_failure(result, aur_preflight)) {
        return result;
    }
    try {
        // AUR network access can be long-running. Revoke cache authority
        // before provider and package-database preparation starts.
        execution_cache_root->require_unchanged_identity();
    } catch(const TrustedCacheError& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::AurPreparation,
            error.what(), error.failure());
        return result;
    } catch(const std::exception& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::AurPreparation,
            error.what());
        return result;
    }
    try {
        aur_preflight.prepare_filtered_operation_stage(
            snapshot, config, execution_cache_root);
    } catch(const TrustedCacheError& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::AurPreparation,
            error.what(), error.failure());
        return result;
    }
    if(stop_for_aur_preflight_failure(result, aur_preflight)) {
        return result;
    }
    const PreparedFilteredAurUpdateOperation* filtered_operation =
        aur_preflight.filtered_operation();
    if(filtered_operation != nullptr && filtered_operation->is_prepared()) {
        try {
            // Blocked/no-op preparations are typed production results. Only
            // an executable capability needs this final mutation gate.
            execution_cache_root->require_unchanged_identity();
        } catch(const TrustedCacheError& error) {
            stop_for_cache_authority_failure(
                result, UpgradeAllOperationPhase::AurPreparation,
                error.what(), error.failure());
            return result;
        } catch(const std::exception& error) {
            stop_for_cache_authority_failure(
                result, UpgradeAllOperationPhase::AurPreparation,
                error.what());
            return result;
        }
    }
    try {
        // PR3 consume boundaryはblocked/no-op時にrunnerを呼ばず、正確な
        // correlation/reducer resultだけをmaterializeする。
        result.aur.operation_result.emplace(
            execute_prepared_filtered_aur_update_operation(
                std::move(
                    aur_preflight.filtered_operation_.value()),
                config));
    } catch(FilteredAurUpdateCancelled& stop) {
        result.aur.operation_result.emplace(std::move(stop).release_result());
        map_filtered_result_status(result);
        // Finalize only the known partial result, never the preparation-failure
        // fallback or a later mutation after cancellation.
        return result;
    } catch(const TrustedCacheError& error) {
        stop_for_cache_authority_failure(
            result, UpgradeAllOperationPhase::AurPreparation,
            error.what(), error.failure());
        return result;
    } catch(const std::logic_error& error) {
        result.status = UpgradeAllOperationStatus::InconsistentResult;
        result.stopped_phase = UpgradeAllOperationPhase::AurPreparation;
        result.aur.status = UpgradeAllAurPhaseStatus::InconsistentResult;
        result.aur.diagnostic = error.what();
        result.issues.push_back(make_issue(
            UpgradeAllOperationIssueKind::FilteredAurExecutionFailed,
            UpgradeAllOperationPhase::AurPreparation,
            error.what()));
        add_stopping_diagnostic(
            result,
            UpgradeAllOperationPhase::AurPreparation,
            error.what());
        return result;
    } catch(const std::exception& error) {
        result.status =
            UpgradeAllOperationStatus::StoppedBeforeAurExecution;
        result.stopped_phase = UpgradeAllOperationPhase::AurPreparation;
        result.aur.status =
            UpgradeAllAurPhaseStatus::BlockedBeforeExecution;
        result.aur.diagnostic = error.what();
        result.issues.push_back(make_issue(
            UpgradeAllOperationIssueKind::FilteredAurPreparationFailed,
            UpgradeAllOperationPhase::AurPreparation,
            error.what()));
        add_stopping_diagnostic(
            result,
            UpgradeAllOperationPhase::AurPreparation,
            error.what());
        return result;
    } catch(...) {
        // TRANSLATORS: The placeholder is the literal service name "AUR".
        const std::string diagnostic =
            localization::format_translated_message(
                "The filtered {} operation failed with an unknown exception.",
                AUR_SERVICE);
        result.status = UpgradeAllOperationStatus::InconsistentResult;
        result.stopped_phase = UpgradeAllOperationPhase::AurPreparation;
        result.aur.status = UpgradeAllAurPhaseStatus::InconsistentResult;
        result.aur.diagnostic = diagnostic;
        result.issues.push_back(make_issue(
            UpgradeAllOperationIssueKind::FilteredAurExecutionFailed,
            UpgradeAllOperationPhase::AurPreparation,
            diagnostic));
        add_stopping_diagnostic(
            result,
            UpgradeAllOperationPhase::AurPreparation,
            diagnostic);
        return result;
    }

    map_filtered_result_status(result);
    if(result.status == UpgradeAllOperationStatus::Completed &&
       qualifies_as_no_updates(result)) {
        result.status = UpgradeAllOperationStatus::NoUpdates;
    }
    return result;
}
