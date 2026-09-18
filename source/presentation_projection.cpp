#include "presentation_projection.hpp"

#include "diagnostic_projection.hpp"

#include <algorithm>
#include <iterator>
#include <type_traits>
#include <utility>

namespace {

bool is_registered_source_failure(
    RegisteredSourceUpgradeStatus status) noexcept {
    return status == RegisteredSourceUpgradeStatus::Failed ||
           status == RegisteredSourceUpgradeStatus::UpdatedCleanupFailed ||
           status == RegisteredSourceUpgradeStatus::NoChangeCleanupFailed;
}

bool is_failure_class(DiagnosticClass classification) noexcept {
    switch(classification) {
        case DiagnosticClass::QueryFailure:
        case DiagnosticClass::MetadataFailure:
        case DiagnosticClass::PartialFailure:
        case DiagnosticClass::ExecutionFailure:
        case DiagnosticClass::InputFailure:
        case DiagnosticClass::InternalInconsistency:
            return true;
        case DiagnosticClass::Invalid:
        case DiagnosticClass::Unsupported:
        case DiagnosticClass::Ambiguous:
        case DiagnosticClass::Declined:
        case DiagnosticClass::Cancelled:
        case DiagnosticClass::Unavailable:
        case DiagnosticClass::RequiresCheck:
        case DiagnosticClass::Blocked:
            return false;
    }
    return true;
}

PackageStateObservationValue registered_source_observation(
    const RegisteredSourceUpgradeResult& result) noexcept {
    ObservationReason reason = ObservationReason::ObservationNotPrepared;
    if(result.status == RegisteredSourceUpgradeStatus::NotAttempted) {
        reason = ObservationReason::PhaseNotAttempted;
    } else if(is_registered_source_failure(result.status)) {
        reason = ObservationReason::OperationFailed;
    }
    return project_package_state_observation(
        result.package_state_change, reason);
}

PackageStateObservationValue aur_target_observation(
    const AurUpdateOperationTargetResult& result) noexcept {
    switch(result.status) {
        case AurUpdateOperationTargetStatus::Updated:
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
            return PackageStateObservationValue{
                PackageStateObservation::Changed, std::nullopt};
        case AurUpdateOperationTargetStatus::NoChange:
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
            return PackageStateObservationValue{
                PackageStateObservation::VerifiedUnchanged, std::nullopt};
        case AurUpdateOperationTargetStatus::NotAttempted:
            return PackageStateObservationValue{
                PackageStateObservation::NotObserved,
                ObservationReason::PhaseNotAttempted};
        case AurUpdateOperationTargetStatus::Skipped:
            if(std::any_of(
                   result.preflight_issues.begin(),
                   result.preflight_issues.end(),
                   [](const AurUpdateExecutionIssue& issue) {
                       return issue.reason ==
                              AurUpdateExecutionReason::UpToDate;
                   })) {
                return PackageStateObservationValue{
                    PackageStateObservation::VerifiedUnchanged,
                    std::nullopt};
            }
            return PackageStateObservationValue{
                PackageStateObservation::NotObserved,
                ObservationReason::ObservationNotPrepared};
        case AurUpdateOperationTargetStatus::Unsupported:
        case AurUpdateOperationTargetStatus::Incomplete:
        case AurUpdateOperationTargetStatus::Cancelled:
        case AurUpdateOperationTargetStatus::Failed:
            return PackageStateObservationValue{
                PackageStateObservation::Unverified,
                ObservationReason::OperationFailed};
    }
    return PackageStateObservationValue{
        PackageStateObservation::Unverified,
        ObservationReason::InconsistentEvidence};
}

std::optional<AurUpdateExecutionReason> aur_target_normal_skip_reason(
    const AurUpdateOperationTargetResult& result) noexcept {
    if(result.status != AurUpdateOperationTargetStatus::Skipped) {
        return std::nullopt;
    }

    std::optional<AurUpdateExecutionReason> reason;
    for(const AurUpdateExecutionIssue& issue : result.preflight_issues) {
        if(issue.reason == AurUpdateExecutionReason::None) continue;
        if(issue.reason != AurUpdateExecutionReason::UpToDate &&
           issue.reason != AurUpdateExecutionReason::NonAurForeign) {
            return std::nullopt;
        }
        if(reason.has_value() && reason.value() != issue.reason) {
            return std::nullopt;
        }
        reason = issue.reason;
    }
    return reason;
}

void apply_registered_source_status(
    PresentationItem& item,
    RegisteredSourceUpgradeStatus status) noexcept {
    switch(status) {
        case RegisteredSourceUpgradeStatus::Updated:
            item.is_update_candidate = true;
            return;
        case RegisteredSourceUpgradeStatus::NoChange:
            return;
        case RegisteredSourceUpgradeStatus::Failed:
            item.diagnostic_class = DiagnosticClass::ExecutionFailure;
            item.is_blocking = true;
            return;
        case RegisteredSourceUpgradeStatus::UpdatedCleanupFailed:
        case RegisteredSourceUpgradeStatus::NoChangeCleanupFailed:
            item.diagnostic_class = DiagnosticClass::PartialFailure;
            item.is_blocking = true;
            item.requires_manual_action = true;
            return;
        case RegisteredSourceUpgradeStatus::NotAttempted:
            item.diagnostic_class = DiagnosticClass::Blocked;
            item.is_blocking = true;
            return;
        case RegisteredSourceUpgradeStatus::Unsupported:
            item.diagnostic_class = DiagnosticClass::Unsupported;
            item.is_blocking = true;
            return;
        case RegisteredSourceUpgradeStatus::Incomplete:
            item.diagnostic_class = DiagnosticClass::RequiresCheck;
            item.requires_check = true;
            item.requires_manual_action = true;
            return;
    }
}

void apply_aur_target_status(
    PresentationItem& item,
    AurUpdateOperationTargetStatus status) noexcept {
    switch(status) {
        case AurUpdateOperationTargetStatus::Updated:
            item.is_update_candidate = true;
            return;
        case AurUpdateOperationTargetStatus::NoChange:
        case AurUpdateOperationTargetStatus::Skipped:
            return;
        case AurUpdateOperationTargetStatus::Unsupported:
            item.diagnostic_class = DiagnosticClass::Unsupported;
            item.is_blocking = true;
            return;
        case AurUpdateOperationTargetStatus::Incomplete:
            item.diagnostic_class = DiagnosticClass::RequiresCheck;
            item.requires_check = true;
            item.requires_manual_action = true;
            return;
        case AurUpdateOperationTargetStatus::Cancelled:
            item.diagnostic_class = DiagnosticClass::Cancelled;
            item.is_blocking = true;
            return;
        case AurUpdateOperationTargetStatus::Failed:
            item.diagnostic_class = DiagnosticClass::ExecutionFailure;
            item.is_blocking = true;
            return;
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
            item.diagnostic_class = DiagnosticClass::PartialFailure;
            item.is_blocking = true;
            item.requires_manual_action = true;
            return;
        case AurUpdateOperationTargetStatus::NotAttempted:
            item.diagnostic_class = DiagnosticClass::Blocked;
            item.is_blocking = true;
            return;
    }
}

template <typename>
inline constexpr bool always_false_v = false;

PlanPresentationReason project_plan_reason(
    const BuildPlan& plan, ExecutionCapability capability,
    const ExecutionReadinessReason& readiness_reason) {
    PlanPresentationReason projected;
    projected.capability = capability;
    projected.readiness = readiness_reason.state;
    projected.blocks_production_guard =
        readiness_reason.blocks_production_guard;
    projected.required_action = readiness_reason.required_action;

    std::visit(
        [&projected, &plan](const auto& reason) {
            using Reason = std::decay_t<decltype(reason)>;
            if constexpr(std::is_same_v<
                             Reason,
                             PlanConstraintAuthorityReason>) {
                projected.kind =
                    PlanPresentationReasonKind::ConstraintAuthority;
                projected.detail = reason.kind;
                projected.edge_index = reason.edge_index;
                projected.subject = reason.dependency_specification;
                if(reason.edge_index < plan.dependency_edges.size()) {
                    const BuildPlanDependencyEdge& edge =
                        plan.dependency_edges[reason.edge_index];
                    projected.subject = edge.parent_package_name;
                    projected.package_base = edge.parent_package_base;
                }
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanSelectedProviderIdentityConflictReason>) {
                projected.kind = PlanPresentationReasonKind::
                    SelectedProviderIdentityConflict;
                projected.subject = reason.selected.package_name;
                if(!reason.selected.package_base.empty()) {
                    projected.package_base =
                        reason.selected.package_base;
                }
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanConstraintReadinessReason>) {
                projected.kind =
                    PlanPresentationReasonKind::ConstraintReadiness;
                projected.detail = reason.satisfaction;
                projected.edge_index = reason.edge_index;
                projected.subject = reason.dependency_specification;
                if(reason.edge_index < plan.dependency_edges.size()) {
                    const BuildPlanDependencyEdge& edge =
                        plan.dependency_edges[reason.edge_index];
                    projected.subject = edge.parent_package_name;
                    projected.package_base = edge.parent_package_base;
                }
            } else if constexpr(std::is_same_v<
                                    Reason, PlanResolutionReason>) {
                projected.kind =
                    PlanPresentationReasonKind::ResolutionFailure;
                projected.detail = reason.failure.kind;
                projected.subject =
                    reason.failure.parent_package_name.has_value()
                        ? reason.failure.parent_package_name
                        : std::optional<std::string>{
                              reason.failure.subject};
                projected.package_base =
                    reason.failure.parent_package_base;
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanUnresolvedDependencyReason>) {
                projected.kind = PlanPresentationReasonKind::
                    UnresolvedDependency;
                projected.subject = reason.dependency;
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanAmbiguousProviderReason>) {
                projected.kind =
                    PlanPresentationReasonKind::AmbiguousProvider;
                projected.subject = reason.dependency.dependency;
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanDependencyCycleReason>) {
                projected.kind =
                    PlanPresentationReasonKind::DependencyCycle;
                projected.subject = reason.dependency;
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanDeclaredRelationReason>) {
                projected.kind =
                    PlanPresentationReasonKind::DeclaredRelation;
                projected.detail = reason.assessment.kind;
                projected.subject = reason.assessment.declaring_package
                                        .package_name;
                projected.package_base =
                    reason.assessment.declaring_package.package_base;
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanSplitPackageReason>) {
                projected.kind =
                    PlanPresentationReasonKind::SplitPackage;
                projected.subject = reason.target.package_name;
                projected.package_base = reason.target.package_base;
            } else if constexpr(std::is_same_v<
                                    Reason,
                                    PlanIncompleteProviderCandidateReason>) {
                projected.kind = PlanPresentationReasonKind::
                    IncompleteProviderCandidate;
                projected.detail = reason.candidate_set.reason;
                projected.subject = reason.candidate_set.dependency;
            } else {
                static_assert(always_false_v<Reason>);
            }
        },
        readiness_reason.reason);
    return projected;
}

void apply_plan_readiness_reason(
    PresentationItem& item, PlanPresentationReason reason) {
    item.is_blocking |= reason.blocks_production_guard;
    item.requires_check |=
        reason.readiness == ExecutionReadinessState::Unknown ||
        reason.readiness == ExecutionReadinessState::RequiresCheck;
    item.requires_manual_action |=
        reason.required_action != PlanRequiredAction::None;
    if(reason.readiness == ExecutionReadinessState::Blocked) {
        item.diagnostic_class = DiagnosticClass::Blocked;
    } else if((reason.readiness == ExecutionReadinessState::Unknown ||
               reason.readiness ==
                   ExecutionReadinessState::RequiresCheck) &&
              item.diagnostic_class !=
                  std::optional<DiagnosticClass>{
                      DiagnosticClass::Blocked}) {
        item.diagnostic_class = DiagnosticClass::RequiresCheck;
    }
    item.plan_reasons.push_back(std::move(reason));
}

PresentationItem* find_plan_item(
    std::vector<PresentationItem>& items,
    const PlanPresentationReason& reason) noexcept {
    if(!reason.subject.has_value()) return nullptr;
    auto found = std::find_if(
        items.begin(), items.end(),
        [&reason](const PresentationItem& item) {
            if(item.requested_package != reason.subject) return false;
            return !reason.package_base.has_value() ||
                   item.package_base == reason.package_base;
        });
    return found == items.end() ? nullptr : &*found;
}

PresentationArtifactIdentity project_artifact_identity(
    const ArtifactPackageIdentity& identity) {
    return PresentationArtifactIdentity{
        identity.package_name, identity.full_version};
}

const RegisteredSourcePreferenceSnapshot* find_registered_source(
    const UpgradeAllOperationResult& result,
    const RegisteredSourceUpgradeResult& source_result) noexcept {
    const auto& sources =
        result.prepared_snapshot.system_source.registered_sources;
    auto found = std::find_if(
        sources.begin(), sources.end(),
        [&source_result](const RegisteredSourcePreferenceSnapshot& source) {
            return source.original_preference_index ==
                       source_result.original_preference_index &&
                   source.preference_package_name ==
                       source_result.preference_package_name;
        });
    if(found == sources.end()) return nullptr;
    auto duplicate = std::find_if(
        found + 1, sources.end(),
        [&source_result](const RegisteredSourcePreferenceSnapshot& source) {
            return source.original_preference_index ==
                       source_result.original_preference_index &&
                   source.preference_package_name ==
                       source_result.preference_package_name;
        });
    return duplicate == sources.end() ? &*found : nullptr;
}

void append_upgrade_all_reason(
    PresentationItem& item, UpgradeAllPresentationReasonValue reason,
    UpgradeAllOperationPhase phase, DiagnosticSourceKind source_kind,
    DiagnosticRequiredAction required_action,
    PresentationCorrelationIdentity correlation = {},
    std::optional<std::string> supplemental_detail = std::nullopt) {
    item.upgrade_all_reasons.push_back(UpgradeAllPresentationReason{
        std::move(reason), phase, source_kind, required_action,
        std::move(correlation), std::move(supplemental_detail)});
}

PresentationItem make_upgrade_all_attention_item(
    std::optional<std::string> package_name,
    std::optional<std::string> package_base,
    UpgradeAllPresentationReasonValue reason,
    UpgradeAllOperationPhase phase, DiagnosticSourceKind source_kind,
    DiagnosticRequiredAction required_action,
    DiagnosticClass classification, bool is_blocking,
    bool requires_check,
    PresentationCorrelationIdentity correlation = {},
    std::optional<std::string> supplemental_detail = std::nullopt) {
    PresentationItem item;
    item.source_kind = source_kind;
    item.requested_package = std::move(package_name);
    item.package_base = std::move(package_base);
    item.diagnostic_class = classification;
    item.is_blocking = is_blocking;
    item.requires_check = requires_check;
    item.requires_manual_action =
        required_action != DiagnosticRequiredAction::None;
    append_upgrade_all_reason(
        item, std::move(reason), phase, source_kind, required_action,
        std::move(correlation), std::move(supplemental_detail));
    return item;
}

template <typename Value>
bool optional_correlation_conflicts(
    const std::optional<Value>& lhs,
    const std::optional<Value>& rhs) noexcept {
    return lhs.has_value() && rhs.has_value() && lhs != rhs;
}

template <typename Value>
bool optional_correlation_matches(
    const std::optional<Value>& lhs,
    const std::optional<Value>& rhs) noexcept {
    return lhs.has_value() && rhs.has_value() && lhs == rhs;
}

bool vector_correlation_conflicts(
    const std::vector<std::size_t>& lhs,
    const std::vector<std::size_t>& rhs) noexcept {
    return !lhs.empty() && !rhs.empty() && lhs != rhs;
}

bool vector_correlation_matches(
    const std::vector<std::size_t>& lhs,
    const std::vector<std::size_t>& rhs) noexcept {
    return !lhs.empty() && !rhs.empty() && lhs == rhs;
}

bool correlation_conflicts(
    const PresentationCorrelationIdentity& lhs,
    const PresentationCorrelationIdentity& rhs) noexcept {
    return optional_correlation_conflicts(
               lhs.adapter_index, rhs.adapter_index) ||
           optional_correlation_conflicts(
               lhs.original_preference_index,
               rhs.original_preference_index) ||
           optional_correlation_conflicts(
               lhs.original_query_plan_index,
               rhs.original_query_plan_index) ||
           optional_correlation_conflicts(
               lhs.planner_target_index, rhs.planner_target_index) ||
           optional_correlation_conflicts(
               lhs.selected_target_index, rhs.selected_target_index) ||
           optional_correlation_conflicts(
               lhs.filtered_update_plan_index,
               rhs.filtered_update_plan_index) ||
           optional_correlation_conflicts(
               lhs.preflight_invocation_index,
               rhs.preflight_invocation_index) ||
           optional_correlation_conflicts(
               lhs.build_plan_root_index,
               rhs.build_plan_root_index) ||
           optional_correlation_conflicts(
               lhs.build_plan_order_index,
               rhs.build_plan_order_index) ||
           optional_correlation_conflicts(
               lhs.invocation_work_item_index,
               rhs.invocation_work_item_index) ||
           vector_correlation_conflicts(
               lhs.affected_update_plan_indices,
               rhs.affected_update_plan_indices) ||
           vector_correlation_conflicts(
               lhs.original_target_indices,
               rhs.original_target_indices) ||
           vector_correlation_conflicts(
               lhs.build_unit_indices, rhs.build_unit_indices) ||
           optional_correlation_conflicts(
               lhs.package_name, rhs.package_name) ||
           optional_correlation_conflicts(
               lhs.package_base, rhs.package_base) ||
           optional_correlation_conflicts(
               lhs.canonical_source_identity,
               rhs.canonical_source_identity);
}

bool correlation_keys_match(
    const PresentationCorrelationIdentity& lhs,
    const PresentationCorrelationIdentity& rhs) noexcept {
    // Package and source identities constrain compatibility, but one source can
    // produce more than one logical failure. Only carrier-owned indices prove
    // correlation; localized supplemental text is not part of this identity.
    return optional_correlation_matches(
               lhs.adapter_index, rhs.adapter_index) ||
           optional_correlation_matches(
               lhs.original_preference_index,
               rhs.original_preference_index) ||
           optional_correlation_matches(
               lhs.original_query_plan_index,
               rhs.original_query_plan_index) ||
           optional_correlation_matches(
               lhs.planner_target_index, rhs.planner_target_index) ||
           optional_correlation_matches(
               lhs.selected_target_index, rhs.selected_target_index) ||
           optional_correlation_matches(
               lhs.filtered_update_plan_index,
               rhs.filtered_update_plan_index) ||
           optional_correlation_matches(
               lhs.preflight_invocation_index,
               rhs.preflight_invocation_index) ||
           optional_correlation_matches(
               lhs.build_plan_root_index,
               rhs.build_plan_root_index) ||
           optional_correlation_matches(
               lhs.build_plan_order_index,
               rhs.build_plan_order_index) ||
           optional_correlation_matches(
               lhs.invocation_work_item_index,
               rhs.invocation_work_item_index) ||
           vector_correlation_matches(
               lhs.affected_update_plan_indices,
               rhs.affected_update_plan_indices) ||
           vector_correlation_matches(
               lhs.original_target_indices,
               rhs.original_target_indices) ||
           vector_correlation_matches(
               lhs.build_unit_indices, rhs.build_unit_indices);
}

bool has_upgrade_all_phase(
    const PresentationItem& item,
    UpgradeAllOperationPhase phase) noexcept {
    return std::any_of(
        item.upgrade_all_reasons.begin(),
        item.upgrade_all_reasons.end(),
        [phase](const UpgradeAllPresentationReason& reason) {
            return reason.phase == phase;
        });
}

bool source_kinds_are_compatible(
    DiagnosticSourceKind lhs, DiagnosticSourceKind rhs) noexcept {
    return lhs == DiagnosticSourceKind::Unspecified ||
           rhs == DiagnosticSourceKind::Unspecified || lhs == rhs;
}

bool item_identity_conflicts_with_correlation(
    const PresentationItem& item,
    const PresentationCorrelationIdentity& correlation) noexcept {
    return optional_correlation_conflicts(
               item.requested_package, correlation.package_name) ||
           optional_correlation_conflicts(
               item.package_base, correlation.package_base) ||
           optional_correlation_conflicts(
               item.canonical_source_identity,
               correlation.canonical_source_identity);
}

bool item_correlations_are_internally_compatible(
    const PresentationItem& item) noexcept {
    for(std::size_t left_index = 0;
        left_index < item.upgrade_all_reasons.size(); ++left_index) {
        const UpgradeAllPresentationReason& left =
            item.upgrade_all_reasons[left_index];
        if(!source_kinds_are_compatible(
               item.source_kind, left.source_kind) ||
           item_identity_conflicts_with_correlation(
               item, left.correlation)) {
            return false;
        }
        for(std::size_t right_index = left_index + 1;
            right_index < item.upgrade_all_reasons.size(); ++right_index) {
            const UpgradeAllPresentationReason& right =
                item.upgrade_all_reasons[right_index];
            if(!source_kinds_are_compatible(
                   left.source_kind, right.source_kind) ||
               correlation_conflicts(
                   left.correlation, right.correlation)) {
                return false;
            }
        }
    }
    return true;
}

bool item_is_compatible_with_correlation(
    const PresentationItem& item,
    DiagnosticSourceKind source_kind,
    const PresentationCorrelationIdentity& correlation) noexcept {
    if(!source_kinds_are_compatible(item.source_kind, source_kind) ||
       !item_correlations_are_internally_compatible(item) ||
       item_identity_conflicts_with_correlation(item, correlation)) {
        return false;
    }
    return std::none_of(
        item.upgrade_all_reasons.begin(),
        item.upgrade_all_reasons.end(),
        [source_kind, &correlation](
            const UpgradeAllPresentationReason& reason) {
            return !source_kinds_are_compatible(
                       reason.source_kind, source_kind) ||
                   correlation_conflicts(
                       reason.correlation, correlation);
        });
}

DiagnosticSourceKind retain_item_source_kind(
    PresentationItem& item,
    UpgradeAllOperationPhase phase) noexcept {
    const DiagnosticSourceKind phase_source =
        upgrade_all_source_kind(phase);
    if(item.source_kind == DiagnosticSourceKind::Unspecified) {
        item.source_kind = phase_source;
    }
    return item.source_kind;
}

PresentationItem* find_correlated_failure_item(
    std::vector<PresentationItem>& items,
    UpgradeAllOperationPhase phase,
    DiagnosticSourceKind source_kind,
    const PresentationCorrelationIdentity& correlation) noexcept {
    PresentationItem* match = nullptr;
    for(PresentationItem& item : items) {
        if(!item_is_compatible_with_correlation(
               item, source_kind, correlation)) {
            continue;
        }
        const bool has_match = std::any_of(
            item.upgrade_all_reasons.begin(),
            item.upgrade_all_reasons.end(),
            [phase, &correlation](
                const UpgradeAllPresentationReason& reason) {
                return reason.phase == phase &&
                       correlation_keys_match(
                           reason.correlation, correlation);
            });
        if(!has_match) continue;
        if(match != nullptr && match != &item) return nullptr;
        match = &item;
    }
    return match;
}

template <typename Reason>
PresentationItem* find_unique_item_with_reason(
    std::vector<PresentationItem>& items,
    UpgradeAllOperationPhase phase, Reason expected,
    DiagnosticSourceKind source_kind,
    const PresentationCorrelationIdentity& correlation) noexcept {
    PresentationItem* match = nullptr;
    for(PresentationItem& item : items) {
        if(!item_is_compatible_with_correlation(
               item, source_kind, correlation)) {
            continue;
        }
        const bool has_match = std::any_of(
            item.upgrade_all_reasons.begin(),
            item.upgrade_all_reasons.end(),
            [phase, expected](
                const UpgradeAllPresentationReason& reason) {
                const Reason* value =
                    std::get_if<Reason>(&reason.reason);
                return reason.phase == phase && value != nullptr &&
                       *value == expected;
            });
        if(!has_match) continue;
        if(match != nullptr && match != &item) return nullptr;
        match = &item;
    }
    return match;
}

PresentationItem* find_unique_phase_item(
    std::vector<PresentationItem>& items,
    UpgradeAllOperationPhase phase,
    DiagnosticSourceKind source_kind,
    const PresentationCorrelationIdentity& correlation) noexcept {
    PresentationItem* match = nullptr;
    for(PresentationItem& item : items) {
        if(!item_is_compatible_with_correlation(
               item, source_kind, correlation)) {
            continue;
        }
        if(!has_upgrade_all_phase(item, phase)) continue;
        if(match != nullptr) return nullptr;
        match = &item;
    }
    return match;
}

void merge_logical_failure_item(
    PresentationItem& primary, PresentationItem secondary) {
    if(primary.source_kind == DiagnosticSourceKind::Unspecified) {
        primary.source_kind = secondary.source_kind;
    }
    if(!primary.repository.has_value()) {
        primary.repository = std::move(secondary.repository);
    }
    if(!primary.requested_package.has_value()) {
        primary.requested_package =
            std::move(secondary.requested_package);
    }
    if(!primary.package_base.has_value()) {
        primary.package_base = std::move(secondary.package_base);
    }
    if(!primary.canonical_source_identity.has_value()) {
        primary.canonical_source_identity =
            std::move(secondary.canonical_source_identity);
    }
    if(!primary.local_root.has_value()) {
        primary.local_root = std::move(secondary.local_root);
    }
    primary.selected_artifacts.insert(
        primary.selected_artifacts.end(),
        std::make_move_iterator(
            secondary.selected_artifacts.begin()),
        std::make_move_iterator(
            secondary.selected_artifacts.end()));
    primary.unselected_artifacts.insert(
        primary.unselected_artifacts.end(),
        std::make_move_iterator(
            secondary.unselected_artifacts.begin()),
        std::make_move_iterator(
            secondary.unselected_artifacts.end()));
    if(!primary.package_state.has_value()) {
        primary.package_state = std::move(secondary.package_state);
    }
    if(!primary.aur_normal_skip_reason.has_value()) {
        primary.aur_normal_skip_reason =
            secondary.aur_normal_skip_reason;
    }
    if(!primary.diagnostic_class.has_value()) {
        primary.diagnostic_class = secondary.diagnostic_class;
    }
    primary.plan_reasons.insert(
        primary.plan_reasons.end(),
        std::make_move_iterator(secondary.plan_reasons.begin()),
        std::make_move_iterator(secondary.plan_reasons.end()));
    primary.upgrade_all_reasons.insert(
        primary.upgrade_all_reasons.end(),
        std::make_move_iterator(
            secondary.upgrade_all_reasons.begin()),
        std::make_move_iterator(
            secondary.upgrade_all_reasons.end()));
    primary.is_update_candidate |= secondary.is_update_candidate;
    primary.is_blocking |= secondary.is_blocking;
    primary.requires_check |= secondary.requires_check;
    primary.requires_manual_action |= secondary.requires_manual_action;
}

void merge_correlated_failure_item(
    std::vector<PresentationItem>& items, PresentationItem item,
    UpgradeAllOperationPhase phase,
    const PresentationCorrelationIdentity& correlation) {
    if(!item_is_compatible_with_correlation(
           item, item.source_kind, correlation)) {
        items.push_back(std::move(item));
        return;
    }
    PresentationItem* primary = find_correlated_failure_item(
        items, phase, item.source_kind, correlation);
    if(primary == nullptr) {
        items.push_back(std::move(item));
        return;
    }
    merge_logical_failure_item(*primary, std::move(item));
}

bool is_stopping_outer_issue(
    UpgradeAllOperationIssueKind kind) noexcept {
    switch(kind) {
        case UpgradeAllOperationIssueKind::SystemSourceExecutionFailedUnexpectedly:
        case UpgradeAllOperationIssueKind::SystemSourcePhaseIncomplete:
        case UpgradeAllOperationIssueKind::ForeignInventoryConfigurationFailed:
        case UpgradeAllOperationIssueKind::ForeignInventoryReadFailed:
        case UpgradeAllOperationIssueKind::CacheAuthorityInvalid:
        case UpgradeAllOperationIssueKind::AurQueryFailed:
        case UpgradeAllOperationIssueKind::FilteredAurPreparationFailed:
        case UpgradeAllOperationIssueKind::FilteredAurExecutionFailed:
            return true;
        case UpgradeAllOperationIssueKind::ExplicitSourceAdapterInvalid:
        case UpgradeAllOperationIssueKind::OptionSnapshotMismatch:
        case UpgradeAllOperationIssueKind::SourceSnapshotMismatch:
        case UpgradeAllOperationIssueKind::ExplicitSourceCorrelationInconsistent:
        case UpgradeAllOperationIssueKind::PreparedCapabilityConsumed:
        case UpgradeAllOperationIssueKind::DuplicateExclusionCorrelationInconsistent:
        case UpgradeAllOperationIssueKind::ExternalSatisfactionCorrelationInconsistent:
        case UpgradeAllOperationIssueKind::UnknownFailure:
            return false;
    }
    return false;
}

PresentationItem* find_operation_issue_primary(
    std::vector<PresentationItem>& items,
    const UpgradeAllOperationResult& result,
    const UpgradeAllOperationIssue& issue,
    const PresentationCorrelationIdentity& correlation) noexcept {
    const DiagnosticSourceKind source_kind =
        upgrade_all_source_kind(issue.phase);
    if(PresentationItem* correlated = find_correlated_failure_item(
           items, issue.phase, source_kind, correlation)) {
        return correlated;
    }
    if(issue.kind == UpgradeAllOperationIssueKind::
                         ForeignInventoryConfigurationFailed ||
       issue.kind == UpgradeAllOperationIssueKind::
                         ForeignInventoryReadFailed) {
        if(PresentationItem* inventory = find_unique_item_with_reason(
               items, UpgradeAllOperationPhase::ForeignInventory,
               UpgradeAllForeignInventoryPhaseStatus::Failed,
               source_kind, correlation)) {
            return inventory;
        }
    }
    if(issue.phase == UpgradeAllOperationPhase::System) {
        if(PresentationItem* system = find_unique_item_with_reason(
               items, UpgradeAllOperationPhase::System,
               SystemUpgradePhaseStatus::Failed,
               source_kind, correlation)) {
            return system;
        }
    }
    if(is_stopping_outer_issue(issue.kind) &&
       issue.phase == result.stopped_phase) {
        return find_unique_phase_item(
            items, issue.phase, source_kind, correlation);
    }
    return nullptr;
}

void append_operation_issue_reason(
    PresentationItem& item,
    const UpgradeAllOperationIssue& issue,
    const NormalizedDiagnostic<UpgradeAllOperationIssue>& diagnostic,
    const PresentationCorrelationIdentity& correlation) {
    const DiagnosticSourceKind source_kind =
        retain_item_source_kind(item, issue.phase);
    if(!item.diagnostic_class.has_value()) {
        item.diagnostic_class = diagnostic.classification;
    }
    item.is_blocking |= diagnostic.blocking_decision !=
                        DiagnosticBlockingDecision::NonBlocking;
    item.requires_check |= diagnostic.classification ==
                           DiagnosticClass::RequiresCheck;
    item.requires_manual_action |= diagnostic.required_action !=
                                   DiagnosticRequiredAction::None;
    append_upgrade_all_reason(
        item, issue.kind, issue.phase, source_kind,
        diagnostic.required_action, correlation, issue.diagnostic);
    if(issue.package_metadata_failure.has_value()) {
        append_upgrade_all_reason(
            item, issue.package_metadata_failure->code, issue.phase,
            source_kind, DiagnosticRequiredAction::InspectMetadata,
            correlation,
            issue.package_metadata_failure->diagnostic);
    }
}

std::size_t append_phase_boundary_reason(
    std::vector<PresentationItem>& items,
    UpgradeAllPresentationBoundaryReason reason,
    UpgradeAllOperationPhase phase,
    DiagnosticRequiredAction required_action,
    DiagnosticClass classification, bool is_blocking,
    bool requires_check,
    const std::optional<std::string>& supplemental_detail) {
    std::size_t appended = 0;
    for(PresentationItem& item : items) {
        if(!has_upgrade_all_phase(item, phase)) continue;
        const DiagnosticSourceKind source_kind =
            retain_item_source_kind(item, phase);
        if(!item.diagnostic_class.has_value()) {
            item.diagnostic_class = classification;
        }
        item.is_blocking |= is_blocking;
        item.requires_check |= requires_check;
        item.requires_manual_action |= required_action !=
                                       DiagnosticRequiredAction::None;
        append_upgrade_all_reason(
            item, reason, phase, source_kind, required_action, {},
            supplemental_detail);
        ++appended;
    }
    return appended;
}

DiagnosticRequiredAction registered_source_required_action(
    RegisteredSourceUpgradeStatus status) noexcept {
    switch(status) {
        case RegisteredSourceUpgradeStatus::Updated:
        case RegisteredSourceUpgradeStatus::NoChange:
            return DiagnosticRequiredAction::None;
        case RegisteredSourceUpgradeStatus::Failed:
        case RegisteredSourceUpgradeStatus::UpdatedCleanupFailed:
        case RegisteredSourceUpgradeStatus::NoChangeCleanupFailed:
            return DiagnosticRequiredAction::InspectPartialResult;
        case RegisteredSourceUpgradeStatus::NotAttempted:
            return DiagnosticRequiredAction::ResolveBlocker;
        case RegisteredSourceUpgradeStatus::Unsupported:
        case RegisteredSourceUpgradeStatus::Incomplete:
            return DiagnosticRequiredAction::ConfirmEvaluation;
    }
    return DiagnosticRequiredAction::ReportInconsistency;
}

DiagnosticRequiredAction aur_target_required_action(
    AurUpdateOperationTargetStatus status) noexcept {
    switch(status) {
        case AurUpdateOperationTargetStatus::Updated:
        case AurUpdateOperationTargetStatus::NoChange:
        case AurUpdateOperationTargetStatus::Skipped:
            return DiagnosticRequiredAction::None;
        case AurUpdateOperationTargetStatus::Unsupported:
        case AurUpdateOperationTargetStatus::Incomplete:
            return DiagnosticRequiredAction::ConfirmEvaluation;
        case AurUpdateOperationTargetStatus::Cancelled:
            return DiagnosticRequiredAction::None;
        case AurUpdateOperationTargetStatus::Failed:
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
            return DiagnosticRequiredAction::InspectPartialResult;
        case AurUpdateOperationTargetStatus::NotAttempted:
            return DiagnosticRequiredAction::ResolveBlocker;
    }
    return DiagnosticRequiredAction::ReportInconsistency;
}

bool is_normal_registered_source_status(
    RegisteredSourceUpgradeStatus status) noexcept {
    return status == RegisteredSourceUpgradeStatus::Updated ||
           status == RegisteredSourceUpgradeStatus::NoChange;
}

bool is_normal_aur_target_status(
    AurUpdateOperationTargetStatus status) noexcept {
    return status == AurUpdateOperationTargetStatus::Updated ||
           status == AurUpdateOperationTargetStatus::NoChange ||
           status == AurUpdateOperationTargetStatus::Skipped;
}

bool is_authoritative_aur_up_to_date_normal_skip(
    const PresentationItem& item) noexcept {
    return item.source_kind == DiagnosticSourceKind::Aur &&
           item.aur_normal_skip_reason ==
               AurUpdateExecutionReason::UpToDate &&
           item.package_state.has_value() &&
           item.package_state->state ==
               PackageStateObservation::VerifiedUnchanged;
}

} // namespace

bool has_distinct_package_base_identity(
    const PresentationItem& item) noexcept {
    return item.package_base.has_value() &&
           (!item.requested_package.has_value() ||
            item.package_base.value() != item.requested_package.value());
}

bool has_distinct_artifact_identity(
    const PresentationItem& item) noexcept {
    if(!item.unselected_artifacts.empty() ||
       item.selected_artifacts.size() > 1) {
        return true;
    }
    return std::any_of(
        item.selected_artifacts.begin(), item.selected_artifacts.end(),
        [&item](const PresentationArtifactIdentity& artifact) {
            return !item.requested_package.has_value() ||
                   artifact.package_name !=
                       item.requested_package.value();
        });
}

bool is_attention_required(const PresentationItem& item) noexcept {
    const bool has_unverified_package_state =
        item.package_state.has_value() &&
        item.package_state->state ==
            PackageStateObservation::Unverified;
    if(item.is_update_candidate || item.is_blocking ||
       item.requires_check || item.requires_manual_action ||
       item.diagnostic_class.has_value() || !item.plan_reasons.empty() ||
       !item.upgrade_all_reasons.empty() ||
       has_distinct_artifact_identity(item) ||
       has_unverified_package_state) {
        return true;
    }
    return has_distinct_package_base_identity(item) &&
           !is_authoritative_aur_up_to_date_normal_skip(item);
}

bool should_suppress_repeated_package_base_identity(
    const PresentationItem& item) noexcept {
    return item.package_base.has_value() &&
           item.requested_package.has_value() &&
           item.package_base.value() == item.requested_package.value() &&
           !is_attention_required(item);
}

PresentationProjection partition_presentation_items(
    std::vector<PresentationItem> items) {
    PresentationProjection projection;
    projection.full_items = std::move(items);
    projection.summary_counts.total = projection.full_items.size();

    for(const PresentationItem& item : projection.full_items) {
        const bool needs_attention = is_attention_required(item);
        if(needs_attention) {
            projection.attention_items.push_back(item);
            ++projection.summary_counts.attention_required;
        } else {
            ++projection.summary_counts.normal;
        }
        if(item.is_update_candidate) {
            ++projection.summary_counts.update_candidates;
        }
        if(item.is_blocking) ++projection.summary_counts.blockers;
        if(item.requires_check) ++projection.summary_counts.requires_check;
        if(item.diagnostic_class.has_value() &&
           is_failure_class(item.diagnostic_class.value())) {
            ++projection.summary_counts.failures;
        }
        if(item.package_state.has_value()) {
            if(item.package_state->state ==
               PackageStateObservation::Unverified) {
                ++projection.summary_counts.unverified;
            } else if(item.package_state->state ==
                      PackageStateObservation::NotObserved) {
                ++projection.summary_counts.not_observed;
            }
        }
        if(has_distinct_package_base_identity(item)) {
            ++projection.summary_counts.split_identities;
        }
    }
    return projection;
}

PresentationItem project_registered_source_presentation_item(
    const RegisteredSourcePreferenceSnapshot* source,
    const RegisteredSourceUpgradeResult& result) {
    PresentationItem item;
    item.requested_package = result.preference_package_name;
    item.package_base = result.resolved_package_base;
    item.canonical_source_identity = result.canonical_source_identity_key;
    item.package_state = registered_source_observation(result);

    if(source != nullptr) {
        if(!item.package_base.has_value()) {
            item.package_base = source->resolved_package_base;
        }
        if(!item.canonical_source_identity.has_value()) {
            item.canonical_source_identity =
                source->canonical_source_identity_key;
        }
        if(source->source_kind == SourceBuildSourceKind::Repository) {
            item.source_kind = DiagnosticSourceKind::RepositorySource;
        } else if(source->source_kind == SourceBuildSourceKind::Aur) {
            item.source_kind = DiagnosticSourceKind::Aur;
        }
        if(source->repository_identity.has_value()) {
            item.repository = source->repository_identity->exact_package()
                                  .repository_name;
        }
    } else {
        item.diagnostic_class = DiagnosticClass::InternalInconsistency;
        item.is_blocking = true;
    }

    if(result.package_base_execution.has_value()) {
        item.selected_artifacts.push_back(
            project_artifact_identity(
                result.package_base_execution->selected_child
                    .identity));
        for(const ArtifactPackageIdentity& artifact :
            result.package_base_execution->unselected_artifacts) {
            item.unselected_artifacts.push_back(
                project_artifact_identity(artifact));
        }
    }

    apply_registered_source_status(item, result.status);
    if(!is_normal_registered_source_status(result.status)) {
        PresentationCorrelationIdentity correlation;
        correlation.original_preference_index =
            result.original_preference_index;
        correlation.package_name = result.preference_package_name;
        correlation.package_base = result.resolved_package_base;
        correlation.canonical_source_identity =
            result.canonical_source_identity_key;
        const DiagnosticRequiredAction action =
            registered_source_required_action(result.status);
        append_upgrade_all_reason(
            item, result.status,
            UpgradeAllOperationPhase::RegisteredSource,
            item.source_kind, action, correlation,
            result.cleanup_diagnostic.has_value()
                ? result.cleanup_diagnostic
                : result.diagnostic);
        if(result.failure_kind != RegisteredSourceUpgradeFailureKind::None) {
            append_upgrade_all_reason(
                item, result.failure_kind,
                UpgradeAllOperationPhase::RegisteredSource,
                item.source_kind, action, std::move(correlation),
                result.diagnostic);
        }
    }
    return item;
}

PresentationItem project_aur_update_presentation_item(
    const AurUpdateOperationTargetResult& result) {
    PresentationItem item;
    item.source_kind = DiagnosticSourceKind::Aur;
    item.requested_package = result.update.installed_name;
    item.package_base = result.package_base;
    if(!item.package_base.has_value() && result.update.aur_package.has_value()) {
        item.package_base = result.update.aur_package->package_base;
    }
    if(item.package_base.has_value()) {
        // NO_TRANSLATE(Issue #350): canonical source identity key.
        item.canonical_source_identity =
            "aur:" + item.package_base.value();
    }
    item.package_state = aur_target_observation(result);
    item.aur_normal_skip_reason = aur_target_normal_skip_reason(result);
    item.is_update_candidate =
        result.update.classification ==
        AurUpdateClassification::UpdateAvailable;

    for(const AurUpdateOperationExecutionContribution& contribution :
        result.execution_contributions) {
        if(contribution.selected_artifact.has_value()) {
            item.selected_artifacts.push_back(
                project_artifact_identity(
                    contribution.selected_artifact.value()));
        }
    }

    if(result.update.classification ==
       AurUpdateClassification::MetadataUnavailable) {
        item.diagnostic_class = DiagnosticClass::MetadataFailure;
        item.requires_check = true;
    } else if(result.update.classification ==
              AurUpdateClassification::VersionComparisonUnavailable) {
        item.diagnostic_class = DiagnosticClass::RequiresCheck;
        item.requires_check = true;
    }
    apply_aur_target_status(item, result.status);
    if(result.status == AurUpdateOperationTargetStatus::Cancelled && result.cancellation) {
        const auto diagnostic = project_confirmation_diagnostic(
            ConfirmationResult{*result.cancellation}, DiagnosticOperation::Build,
            DiagnosticPhase::Build, {});
        item.diagnostic_class = diagnostic.classification;
        item.is_blocking = diagnostic.blocking_decision != DiagnosticBlockingDecision::NonBlocking;
    }
    if(!is_normal_aur_target_status(result.status)) {
        PresentationCorrelationIdentity correlation;
        correlation.filtered_update_plan_index = result.update_plan_index;
        correlation.invocation_work_item_index =
            result.execution_work_item_index;
        correlation.package_name = result.update.installed_name;
        correlation.package_base = item.package_base;
        correlation.canonical_source_identity =
            item.canonical_source_identity;
        const DiagnosticRequiredAction action =
            aur_target_required_action(result.status);
        append_upgrade_all_reason(
            item, result.status, UpgradeAllOperationPhase::AurExecution,
            upgrade_all_source_kind(
                UpgradeAllOperationPhase::AurExecution),
            action, correlation,
            result.execution_diagnostic);
        if(result.execution_failure_kind.has_value()) {
            append_upgrade_all_reason(
                item, result.execution_failure_kind.value(),
                UpgradeAllOperationPhase::AurExecution,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::AurExecution),
                action,
                std::move(correlation), result.execution_diagnostic);
        }
    }
    return item;
}

PresentationProjection project_upgrade_all_presentation_with_operation_state(
    const UpgradeAllOperationResult& result,
    const OperationStateProjection& operation_state) {
    std::vector<PresentationItem> items;
    items.reserve(
        result.system_source.registered_source_results.size() +
        result.issues.size() + result.diagnostics.size() + 2);

    for(const RegisteredSourceUpgradeResult& source_result :
        result.system_source.registered_source_results) {
        items.push_back(project_registered_source_presentation_item(
            find_registered_source(result, source_result),
            source_result));
    }

    if(result.aur.operation_result.has_value()) {
        const FilteredAurUpdateExecutionResult& filtered =
            result.aur.operation_result.value();
        const AurUpdateOperationResult& aur_result =
            filtered.reduced_operation_result;
        items.reserve(items.size() + aur_result.targets.size());
        for(const AurUpdateOperationTargetResult& target :
            aur_result.targets) {
            items.push_back(project_aur_update_presentation_item(target));
        }

        for(const AurUpdateQueryFailure& failure :
            filtered.query_result.recoverable_failures) {
            const std::optional<std::string> package_name =
                failure.package_names.size() == 1
                    ? std::optional<std::string>{
                          failure.package_names.front()}
                    : std::nullopt;
            PresentationCorrelationIdentity correlation;
            correlation.package_name = package_name;
            PresentationItem item = make_upgrade_all_attention_item(
                package_name, std::nullopt,
                UpgradeAllPresentationBoundaryReason::AurQueryFailure,
                UpgradeAllOperationPhase::AurQuery,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::AurQuery),
                DiagnosticRequiredAction::RetryQuery,
                DiagnosticClass::QueryFailure, true, false,
                correlation, failure.diagnostic);
            merge_correlated_failure_item(
                items, std::move(item),
                UpgradeAllOperationPhase::AurQuery, correlation);
        }

        for(const UpgradeAllPlanningIssue& issue :
            filtered.upgrade_all_plan.issues) {
            PresentationCorrelationIdentity correlation;
            correlation.original_target_indices =
                issue.original_target_indexes;
            correlation.build_unit_indices = issue.build_unit_indexes;
            correlation.package_name = issue.package_name;
            correlation.package_base = issue.package_base;
            PresentationItem item = make_upgrade_all_attention_item(
                issue.package_name, issue.package_base, issue.kind,
                UpgradeAllOperationPhase::AurPreparation,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::AurPreparation),
                DiagnosticRequiredAction::ResolveBlocker,
                DiagnosticClass::Blocked, true, false,
                correlation);
            merge_correlated_failure_item(
                items, std::move(item),
                UpgradeAllOperationPhase::AurPreparation,
                correlation);
        }

        for(const AurUpdatePreparationIssue& issue :
            aur_result.preparation_issues) {
            PresentationCorrelationIdentity correlation;
            correlation.affected_update_plan_indices =
                issue.affected_update_plan_indices;
            correlation.package_name = issue.package_name;
            correlation.package_base = issue.package_base;
            PresentationItem item = make_upgrade_all_attention_item(
                issue.package_name, issue.package_base, issue.reason,
                UpgradeAllOperationPhase::AurPreparation,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::AurPreparation),
                DiagnosticRequiredAction::ResolveBlocker,
                DiagnosticClass::MetadataFailure, true, false,
                correlation, issue.diagnostic);
            if(issue.package_metadata_failure.has_value()) {
                append_upgrade_all_reason(
                    item, issue.package_metadata_failure->code,
                    UpgradeAllOperationPhase::AurPreparation,
                    upgrade_all_source_kind(
                        UpgradeAllOperationPhase::AurPreparation),
                    DiagnosticRequiredAction::InspectMetadata,
                    correlation,
                    issue.package_metadata_failure->diagnostic);
            }
            merge_correlated_failure_item(
                items, std::move(item),
                UpgradeAllOperationPhase::AurPreparation,
                correlation);
        }

        for(const AurUpdateOperationReductionIssue& issue :
            aur_result.reduction_issues) {
            PresentationCorrelationIdentity correlation;
            correlation.affected_update_plan_indices =
                issue.affected_update_plan_indices;
            correlation.invocation_work_item_index =
                issue.execution_work_item_index;
            PresentationItem item = make_upgrade_all_attention_item(
                std::nullopt, std::nullopt, issue.reason,
                UpgradeAllOperationPhase::Reduction,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::Reduction),
                DiagnosticRequiredAction::ReportInconsistency,
                DiagnosticClass::InternalInconsistency, true, false,
                correlation, issue.diagnostic);
            merge_correlated_failure_item(
                items, std::move(item),
                UpgradeAllOperationPhase::Reduction, correlation);
        }

        for(const FilteredAurUpdateOperationIssue& issue : filtered.issues) {
            PresentationCorrelationIdentity correlation;
            correlation.original_query_plan_index =
                issue.original_query_plan_index;
            correlation.planner_target_index = issue.planner_target_index;
            correlation.selected_target_index = issue.selected_target_index;
            correlation.filtered_update_plan_index =
                issue.filtered_update_plan_index;
            correlation.preflight_invocation_index =
                issue.preflight_invocation_index;
            correlation.build_plan_root_index = issue.build_plan_root_index;
            correlation.build_plan_order_index =
                issue.build_plan_order_index;
            correlation.invocation_work_item_index =
                issue.invocation_work_item_index;
            correlation.package_name = issue.package_name;
            correlation.package_base = issue.package_base;
            PresentationItem item = make_upgrade_all_attention_item(
                issue.package_name, issue.package_base, issue.kind,
                UpgradeAllOperationPhase::Reduction,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::Reduction),
                DiagnosticRequiredAction::ReportInconsistency,
                DiagnosticClass::InternalInconsistency, true, false,
                correlation, issue.diagnostic);
            merge_correlated_failure_item(
                items, std::move(item),
                UpgradeAllOperationPhase::Reduction, correlation);
        }
    }

    if(result.system_source.system.status ==
       SystemUpgradePhaseStatus::Failed) {
        PresentationItem item = make_upgrade_all_attention_item(
            std::nullopt, std::nullopt,
            result.system_source.system.status,
            UpgradeAllOperationPhase::System,
            upgrade_all_source_kind(
                UpgradeAllOperationPhase::System),
            DiagnosticRequiredAction::InspectPartialResult,
            DiagnosticClass::ExecutionFailure, true, false, {},
            result.system_source.system.diagnostic);
        if(result.system_source.system.before_snapshot_failure.has_value()) {
            append_upgrade_all_reason(
                item,
                result.system_source.system.before_snapshot_failure->code,
                UpgradeAllOperationPhase::System,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::System),
                DiagnosticRequiredAction::InspectMetadata);
        }
        if(result.system_source.system.after_snapshot_failure.has_value()) {
            append_upgrade_all_reason(
                item,
                result.system_source.system.after_snapshot_failure->code,
                UpgradeAllOperationPhase::System,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::System),
                DiagnosticRequiredAction::InspectMetadata);
        }
        items.push_back(std::move(item));
    }

    if(result.foreign_inventory.status ==
       UpgradeAllForeignInventoryPhaseStatus::Failed) {
        PresentationItem item = make_upgrade_all_attention_item(
            std::nullopt, std::nullopt,
            result.foreign_inventory.status,
            UpgradeAllOperationPhase::ForeignInventory,
            upgrade_all_source_kind(
                UpgradeAllOperationPhase::ForeignInventory),
            DiagnosticRequiredAction::RetryQuery,
            DiagnosticClass::QueryFailure, true, false, {},
            result.foreign_inventory.diagnostic);
        if(result.foreign_inventory.failure.has_value()) {
            append_upgrade_all_reason(
                item, result.foreign_inventory.failure->code,
                UpgradeAllOperationPhase::ForeignInventory,
                upgrade_all_source_kind(
                    UpgradeAllOperationPhase::ForeignInventory),
                DiagnosticRequiredAction::RetryQuery);
        }
        items.push_back(std::move(item));
    }

    for(const UpgradeAllOperationIssue& issue : result.issues) {
        const auto diagnostic = project_upgrade_all_diagnostic(issue);
        PresentationCorrelationIdentity correlation;
        correlation.adapter_index = issue.adapter_index;
        correlation.original_preference_index =
            issue.original_preference_index;
        correlation.original_query_plan_index =
            issue.original_query_plan_index;
        correlation.build_plan_order_index = issue.build_plan_order_index;
        correlation.package_name = issue.package_name;
        PresentationItem* primary = find_operation_issue_primary(
            items, result, issue, correlation);
        if(primary != nullptr) {
            append_operation_issue_reason(
                *primary, issue, diagnostic, correlation);
            continue;
        }

        PresentationItem item;
        item.source_kind = diagnostic.identity.source_kind;
        item.requested_package = issue.package_name;
        append_operation_issue_reason(
            item, issue, diagnostic, correlation);
        merge_correlated_failure_item(
            items, std::move(item), issue.phase, correlation);
    }

    for(const UpgradeAllOperationDiagnostic& diagnostic :
        result.diagnostics) {
        const DiagnosticRequiredAction required_action =
            diagnostic.stops_execution
                ? DiagnosticRequiredAction::ResolveBlocker
                : DiagnosticRequiredAction::ConfirmEvaluation;
        const DiagnosticClass classification =
            diagnostic.stops_execution
                ? DiagnosticClass::ExecutionFailure
                : DiagnosticClass::RequiresCheck;
        if(diagnostic.stops_execution &&
           diagnostic.phase == result.stopped_phase &&
           append_phase_boundary_reason(
               items,
               UpgradeAllPresentationBoundaryReason::
                   AggregateDiagnostic,
               diagnostic.phase, required_action, classification,
               true, false, diagnostic.diagnostic) > 0) {
            continue;
        }
        const DiagnosticSourceKind source_kind =
            upgrade_all_source_kind(diagnostic.phase);
        items.push_back(make_upgrade_all_attention_item(
            std::nullopt, std::nullopt,
            UpgradeAllPresentationBoundaryReason::AggregateDiagnostic,
            diagnostic.phase, source_kind, required_action,
            classification,
            diagnostic.stops_execution, !diagnostic.stops_execution,
            {}, diagnostic.diagnostic));
    }

    if(result.aur.diagnostic.has_value()) {
        const UpgradeAllOperationPhase phase =
            result.stopped_phase == UpgradeAllOperationPhase::None
                ? UpgradeAllOperationPhase::AurPreparation
                : result.stopped_phase;
        const bool joined_stopping_boundary =
            result.stopped_phase != UpgradeAllOperationPhase::None &&
            append_phase_boundary_reason(
                items,
                UpgradeAllPresentationBoundaryReason::
                    AurPhaseDiagnostic,
                phase,
                DiagnosticRequiredAction::ResolveBlocker,
                DiagnosticClass::ExecutionFailure, true, false,
                result.aur.diagnostic) > 0;
        if(!joined_stopping_boundary) {
            items.push_back(make_upgrade_all_attention_item(
                std::nullopt, std::nullopt,
                UpgradeAllPresentationBoundaryReason::
                    AurPhaseDiagnostic,
                phase, upgrade_all_source_kind(phase),
                DiagnosticRequiredAction::ResolveBlocker,
                DiagnosticClass::ExecutionFailure, true, false, {},
                result.aur.diagnostic));
        }
    }

    if(operation_state.outcome == OperationOutcome::Succeeded &&
       operation_state.package_state.state ==
           PackageStateObservation::Unverified) {
        constexpr UpgradeAllOperationPhase observation_phase =
            UpgradeAllOperationPhase::System;
        PresentationItem observation = make_upgrade_all_attention_item(
            std::nullopt, std::nullopt, operation_state.outcome,
            observation_phase,
            upgrade_all_source_kind(observation_phase),
            DiagnosticRequiredAction::InspectMetadata,
            DiagnosticClass::RequiresCheck, false, true);
        observation.package_state = operation_state.package_state;
        items.push_back(std::move(observation));
    }

    if(items.empty() &&
       result.status != UpgradeAllOperationStatus::Completed &&
       result.status != UpgradeAllOperationStatus::NoUpdates) {
        const DiagnosticClass classification =
            result.status ==
                    UpgradeAllOperationStatus::InconsistentResult
                ? DiagnosticClass::InternalInconsistency
                : DiagnosticClass::ExecutionFailure;
        const DiagnosticRequiredAction required_action =
            result.status ==
                    UpgradeAllOperationStatus::InconsistentResult
                ? DiagnosticRequiredAction::ReportInconsistency
                : DiagnosticRequiredAction::InspectPartialResult;
        items.push_back(make_upgrade_all_attention_item(
            std::nullopt, std::nullopt, result.status,
            result.stopped_phase,
            upgrade_all_source_kind(result.stopped_phase),
            required_action, classification, true, false));
    }

    return partition_presentation_items(std::move(items));
}

PresentationProjection project_build_plan_presentation(
    const BuildPlan& plan) {
    std::vector<PresentationItem> items;
    const PlanStateProjection state = project_build_plan_state(plan);
    const ExecutionReadiness& install = execution_readiness(
        state, ExecutionCapability::Install);
    items.reserve(plan.package_targets.size() + install.reasons.size());

    for(const PlannedPackageTarget& target : plan.package_targets) {
        PresentationItem item;
        item.requested_package = target.package_name;
        item.package_base = target.package_base;
        // BuildPlan does not own root source kind/canonical source identity.
        // Keep those fields unknown instead of inferring AUR from PackageBase.
        items.push_back(std::move(item));
    }

    for(const ExecutionReadinessReason& readiness_reason : install.reasons) {
        const auto* relation = std::get_if<PlanDeclaredRelationReason>(
            &readiness_reason.reason);
        if(relation != nullptr &&
           relation->assessment.kind ==
               PackageRelationAssessmentKind::
                   ConfirmedNoMatchingCurrentOrPlannedTarget) {
            continue;
        }
        PlanPresentationReason reason = project_plan_reason(
            plan, ExecutionCapability::Install, readiness_reason);
        PresentationItem* item = find_plan_item(items, reason);
        if(item == nullptr) {
            PresentationItem issue;
            issue.requested_package = reason.subject;
            issue.package_base = reason.package_base;
            items.push_back(std::move(issue));
            item = &items.back();
        }
        apply_plan_readiness_reason(*item, std::move(reason));
    }

    return partition_presentation_items(std::move(items));
}
