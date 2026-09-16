#include "cross_source_version_lock.hpp"
#include "cross_source_version_lock_observation.hpp"

#include "package_identifier.hpp"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <variant>
#include <vector>

namespace {

CrossSourceVersionLockAssessment make_assessment(
    CrossSourceVersionLockStatus status,
    const CrossSourceVersionLockCandidateEvidence& evidence) {
    return CrossSourceVersionLockAssessment{
        status,
        evidence,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt};
}

CrossSourceVersionLockAssessment finish_assessment(
    CrossSourceVersionLockAssessment assessment,
    CrossSourceVersionLockStatus status) {
    assessment.status = status;
    return assessment;
}

bool has_available_version(
    const ObservedVersion& version,
    ObservedVersionSource expected_source) noexcept {
    return version.source() == expected_source &&
           version.invalid_reason() == nullptr && version.version() != nullptr;
}

bool is_valid_repository_candidate_identity(
    const RepositoryPackagePresent& candidate) noexcept {
    return !candidate.repository_name.empty() &&
           is_valid_package_name(candidate.package_name) &&
           is_valid_package_name(candidate.package_base);
}

std::vector<const ConsumerDependencyRequirement*>
matching_runtime_requirements(
    const AurPackageConstraintMetadata& candidate,
    const std::string& repository_package_name) {
    std::vector<const ConsumerDependencyRequirement*> matches;
    for(const DependencyRequirement& dependency : candidate.depends) {
        const auto* requirement =
            std::get_if<ConsumerDependencyRequirement>(&dependency);
        if(requirement != nullptr &&
           requirement->package_name() == repository_package_name) {
            matches.push_back(requirement);
        }
    }
    return matches;
}

} // namespace

CrossSourceVersionLockAssessment assess_cross_source_version_lock_candidate(
    const CrossSourceVersionLockCandidateEvidence& evidence) {
    CrossSourceVersionLockAssessment assessment = make_assessment(
        CrossSourceVersionLockStatus::Unknown, evidence);

    const InstalledExactPackage& installed_repository_package =
        evidence.repository_upgrade.installed_package;
    const RepositoryPackagePresent& repository_candidate =
        evidence.repository_upgrade.repository_candidate;
    const InstalledCrossSourceVersionLockConsumer& installed_consumer =
        evidence.installed_consumer;

    if(!is_valid_repository_candidate_identity(repository_candidate) ||
       !is_valid_package_name(installed_repository_package.package_name) ||
       installed_repository_package.package_name !=
           repository_candidate.package_name ||
       installed_consumer.requirement.package_name() !=
           repository_candidate.package_name ||
       installed_consumer.package.package_name ==
           repository_candidate.package_name) {
        return finish_assessment(
            std::move(assessment),
            CrossSourceVersionLockStatus::Ambiguous);
    }

    if(validate_package_relation_observation(installed_consumer.package)
           .has_value()) {
        return assessment;
    }
    if(installed_consumer.package.role !=
           PackageRelationObservationRole::Installed ||
       !has_available_version(
           installed_consumer.package.package_version,
           ObservedVersionSource::InstalledExactPackage)) {
        return assessment;
    }
    if(!installed_consumer.requirement.constraint().has_value()) {
        return assessment;
    }

    if(!has_available_version(
           installed_repository_package.observed_version,
           ObservedVersionSource::InstalledExactPackage)) {
        return assessment;
    }
    if(!repository_candidate.package_version.has_value()) {
        return assessment;
    }
    const ObservedVersion& candidate_version =
        repository_candidate.package_version.value();
    if(candidate_version.source() !=
       ObservedVersionSource::RepositoryExactPackage) {
        return finish_assessment(
            std::move(assessment),
            CrossSourceVersionLockStatus::Ambiguous);
    }
    if(!has_available_version(
           candidate_version,
           ObservedVersionSource::RepositoryExactPackage)) {
        return assessment;
    }

    const std::string& installed_version =
        *installed_repository_package.observed_version.version();
    const std::string& repository_version = *candidate_version.version();
    if(compare_arch_package_versions(repository_version, installed_version) !=
       ArchVersionOrdering::Greater) {
        return assessment;
    }

    assessment.installed_requirement_against_installed_version =
        evaluate_consumer_dependency_requirement(
            installed_consumer.requirement,
            installed_repository_package.observed_version);
    assessment.installed_requirement_against_repository_candidate =
        evaluate_consumer_dependency_requirement(
            installed_consumer.requirement, candidate_version);
    if(assessment.installed_requirement_against_installed_version
               ->satisfaction() !=
           ConstraintSatisfaction::Satisfied ||
       assessment.installed_requirement_against_repository_candidate
               ->satisfaction() !=
           ConstraintSatisfaction::Unsatisfied) {
        return assessment;
    }

    if(const auto* failure =
           std::get_if<AurReplacementCandidateQueryFailure>(
               &evidence.aur_replacement);
       failure != nullptr) {
        return finish_assessment(
            std::move(assessment),
            std::find(
                failure->package_names.begin(),
                failure->package_names.end(),
                installed_consumer.package.package_name) !=
                    failure->package_names.end()
                ? CrossSourceVersionLockStatus::QueryFailure
                : CrossSourceVersionLockStatus::Ambiguous);
    }
    if(const auto* missing =
           std::get_if<AurReplacementCandidateNotFound>(
               &evidence.aur_replacement);
       missing != nullptr) {
        return finish_assessment(
            std::move(assessment),
            is_valid_package_name(missing->package_name) &&
                    missing->package_name ==
                        installed_consumer.package.package_name
                ? CrossSourceVersionLockStatus::MissingReplacement
                : CrossSourceVersionLockStatus::Ambiguous);
    }
    if(const auto* unavailable =
           std::get_if<AurReplacementCandidateMetadataUnavailable>(
               &evidence.aur_replacement);
       unavailable != nullptr) {
        return finish_assessment(
            std::move(assessment),
            is_valid_package_name(unavailable->package_name) &&
                    unavailable->package_name ==
                        installed_consumer.package.package_name
                ? CrossSourceVersionLockStatus::Unknown
                : CrossSourceVersionLockStatus::Ambiguous);
    }

    const auto* success =
        std::get_if<AurReplacementCandidateQuerySuccess>(
            &evidence.aur_replacement);
    if(success == nullptr || success->candidates.empty()) {
        return assessment;
    }
    if(success->candidates.size() != 1U) {
        return finish_assessment(
            std::move(assessment),
            CrossSourceVersionLockStatus::Ambiguous);
    }

    const AurPackageConstraintMetadata& replacement =
        success->candidates.front();
    if(!is_valid_package_name(replacement.package_name) ||
       !is_valid_package_name(replacement.package_base)) {
        return assessment;
    }
    if(replacement.package_name != installed_consumer.package.package_name) {
        return finish_assessment(
            std::move(assessment),
            CrossSourceVersionLockStatus::Ambiguous);
    }
    if(replacement.package_version.source() !=
       ObservedVersionSource::AurExactPackage) {
        return finish_assessment(
            std::move(assessment),
            CrossSourceVersionLockStatus::Ambiguous);
    }
    if(!has_available_version(
           replacement.package_version,
           ObservedVersionSource::AurExactPackage)) {
        return assessment;
    }

    const std::vector<const ConsumerDependencyRequirement*> requirements =
        matching_runtime_requirements(
            replacement, repository_candidate.package_name);
    if(requirements.empty()) {
        // A provided/indirect relation needs provider resolution. Absence of a
        // direct typed requirement is not optimistic compatibility evidence.
        return assessment;
    }
    if(requirements.size() != 1U) {
        return finish_assessment(
            std::move(assessment),
            CrossSourceVersionLockStatus::Ambiguous);
    }

    assessment.replacement_requirement = *requirements.front();
    assessment.replacement_requirement_against_repository_candidate =
        evaluate_consumer_dependency_requirement(
            assessment.replacement_requirement.value(),
            candidate_version);
    switch(assessment.replacement_requirement_against_repository_candidate
               ->satisfaction()) {
        case ConstraintSatisfaction::Unconstrained:
        case ConstraintSatisfaction::Satisfied:
            return finish_assessment(
                std::move(assessment),
                CrossSourceVersionLockStatus::CompatibleReplacement);
        case ConstraintSatisfaction::Unsatisfied:
            return finish_assessment(
                std::move(assessment),
                CrossSourceVersionLockStatus::IncompatibleReplacement);
        case ConstraintSatisfaction::Unknown:
        case ConstraintSatisfaction::Invalid:
        case ConstraintSatisfaction::Conflicting:
            return assessment;
    }
    return assessment;
}

namespace {

bool is_exact_requirement(const ConsumerDependencyRequirement& requirement) {
    return requirement.constraint().has_value() &&
           requirement.constraint()->relation() == DependencyVersionRelation::Equal;
}

bool is_satisfied(const ConstraintEvaluation& evaluation) {
    return evaluation.satisfaction() == ConstraintSatisfaction::Satisfied ||
           evaluation.satisfaction() == ConstraintSatisfaction::Unconstrained;
}

bool has_component_identity(const PackageRelationObservedPackage& package,
                            const std::string& component) {
    return package.package_name == component ||
           std::any_of(package.provides.begin(), package.provides.end(),
                       [&](const auto& provided) { return provided.capability.package_name() == component; });
}

// Bounded matching against packages that are already installed. This does not
// choose a provider or add a dependency; version semantics stay with the common
// constraint owner. An unversioned provide never borrows its package version.
bool installed_package_satisfies(const PackageRelationObservedPackage& package,
                                 const ConsumerDependencyRequirement& requirement) {
    if(package.package_name == requirement.package_name() &&
       is_satisfied(evaluate_consumer_dependency_requirement(requirement, package.package_version))) return true;
    return std::any_of(package.provides.begin(), package.provides.end(), [&](const auto& provided) {
        return provided.capability.package_name() == requirement.package_name() &&
               is_satisfied(evaluate_consumer_dependency_requirement(requirement, provided.observed_version));
    });
}

CrossSourceRemovalSafetyEvidence assess_bounded_removal(
    const CrossSourceTransitionInstalledSnapshot& snapshot,
    const PackageRelationObservedPackage& removed) {
    CrossSourceRemovalSafetyEvidence result;
    result.status = CrossSourceRemovalSafetyStatus::PreservesRuntimeDependencies;
    for(const auto& runtime : snapshot.runtime_requirements) {
        if(runtime.package_name == removed.package_name) continue;
        for(const auto& dependency : runtime.requirements) {
            const auto* requirement = std::get_if<ConsumerDependencyRequirement>(&dependency);
            if(requirement == nullptr) {
                // Complete Provides identity coverage can rule out an unrelated
                // soname. Do not invent soname resolution when identity matches.
                const auto& soname = std::get<SonameDependencyRequirement>(dependency);
                if(has_component_identity(removed, soname.soname())) {
                    result.status = CrossSourceRemovalSafetyStatus::Unsupported;
                    return result;
                }
                continue;
            }
            if(!has_component_identity(removed, requirement->package_name())) continue;
            CrossSourceRemovalDependencyEvidence evidence{runtime.package_name, *requirement, {}};
            for(std::size_t index = 0; index < snapshot.packages.size(); ++index) {
                const auto& remaining = snapshot.packages[index];
                if(remaining.package_name != removed.package_name &&
                   installed_package_satisfies(remaining, *requirement)) {
                    evidence.remaining_satisfier_indices.push_back(index);
                }
            }
            // Even a pre-existing unsatisfied relation is not permission to
            // bypass dependency checking. Only positive remaining satisfaction
            // supports a removal intent; no cascade or alternative install.
            if(evidence.remaining_satisfier_indices.empty()) {
                result.status = CrossSourceRemovalSafetyStatus::Blocked;
            }
            result.affected_requirements.push_back(std::move(evidence));
        }
    }
    return result;
}

bool has_consistent_installed_snapshot(const CrossSourceTransitionInstalledSnapshot& snapshot,
                                       const CrossSourceVersionLockCandidateEvidence& candidate) {
    if(snapshot.completeness != PackageRelationObservationCompleteness::Complete) return false;
    std::set<std::string> package_names;
    bool found_consumer = false;
    bool found_repository = false;
    for(const auto& package : snapshot.packages) {
        if(!package_names.insert(package.package_name).second ||
           validate_package_relation_observation(package).has_value() ||
           package.role != PackageRelationObservationRole::Installed ||
           package.source != PackageRelationSourceIdentity{snapshot.source} ||
           package.package_version.version() == nullptr) return false;
        if(package.package_name == candidate.installed_consumer.package.package_name) {
            found_consumer = package == candidate.installed_consumer.package;
        }
        if(package.package_name == candidate.repository_upgrade.installed_package.package_name) {
            found_repository = package.package_version == candidate.repository_upgrade.installed_package.observed_version;
        }
    }
    std::set<std::string> runtime_names;
    bool found_requirement = false;
    for(const auto& runtime : snapshot.runtime_requirements) {
        if(!runtime_names.insert(runtime.package_name).second) return false;
        if(runtime.package_name == candidate.installed_consumer.package.package_name) {
            found_requirement = std::count(runtime.requirements.begin(), runtime.requirements.end(),
                                           DependencyRequirement{candidate.installed_consumer.requirement}) == 1;
        }
    }
    return package_names == runtime_names && found_consumer && found_repository && found_requirement;
}

void classify_transition(CrossSourceCoordinatedTransitionPlan& plan) {
    const auto reject = [&](CrossSourceTransitionPlanStatus status, CrossSourceTransitionPlanReason reason) {
        plan.status = status;
        plan.reason = reason;
    };
    const auto& evidence = plan.correlation.evidence;
    if(plan.basis != CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation) {
        reject(CrossSourceTransitionPlanStatus::Unsupported, CrossSourceTransitionPlanReason::UnsupportedObservationBasis);
        return;
    }
    // Preserve the precise replacement result even when its query also made
    // observation Partial. Neither failure nor uncertainty becomes absence.
    switch(plan.correlation.status) {
        case CrossSourceVersionLockStatus::MissingReplacement:
            reject(CrossSourceTransitionPlanStatus::Blocked, CrossSourceTransitionPlanReason::ReplacementMissing);
            return;
        case CrossSourceVersionLockStatus::QueryFailure:
            reject(CrossSourceTransitionPlanStatus::Incomplete, CrossSourceTransitionPlanReason::ReplacementQueryFailure);
            return;
        case CrossSourceVersionLockStatus::IncompatibleReplacement:
            reject(CrossSourceTransitionPlanStatus::Blocked, CrossSourceTransitionPlanReason::IncompatibleReplacement);
            return;
        case CrossSourceVersionLockStatus::Ambiguous:
            reject(CrossSourceTransitionPlanStatus::Ambiguous, CrossSourceTransitionPlanReason::AmbiguousIdentity);
            return;
        case CrossSourceVersionLockStatus::Unknown:
            reject(CrossSourceTransitionPlanStatus::Incomplete, CrossSourceTransitionPlanReason::ExactRequirementUnavailable);
            return;
        case CrossSourceVersionLockStatus::CompatibleReplacement: break;
    }
    if(plan.observation_completeness != CrossSourceVersionLockObservationStatus::Complete || !plan.observation_issues.empty()) return;
    if(!is_exact_requirement(evidence.installed_consumer.requirement) ||
       !plan.correlation.replacement_requirement.has_value() ||
       !is_exact_requirement(*plan.correlation.replacement_requirement)) {
        reject(CrossSourceTransitionPlanStatus::Unsupported, CrossSourceTransitionPlanReason::ExactRequirementUnavailable);
        return;
    }
    const auto& repository = evidence.repository_upgrade.repository_candidate;
    if(!repository.configured_repository_order.has_value() ||
       repository.configured_order >= repository.configured_repository_order->size() ||
       repository.configured_repository_order->at(repository.configured_order) != repository.repository_name) {
        reject(CrossSourceTransitionPlanStatus::Ambiguous, CrossSourceTransitionPlanReason::AmbiguousIdentity);
        return;
    }
    if(!plan.installed_snapshot.has_value() ||
       !has_consistent_installed_snapshot(*plan.installed_snapshot, evidence)) {
        plan.removal_safety.status = CrossSourceRemovalSafetyStatus::Incomplete;
        reject(CrossSourceTransitionPlanStatus::Incomplete, CrossSourceTransitionPlanReason::IncompleteInstalledInventory);
        return;
    }
    const auto& snapshot = *plan.installed_snapshot;
    for(const auto& foreign : snapshot.foreign_packages) {
        if(foreign.name != evidence.installed_consumer.package.package_name) continue;
        if(plan.installed_foreign.has_value() ||
           foreign.version != *evidence.installed_consumer.package.package_version.version()) {
            reject(CrossSourceTransitionPlanStatus::Ambiguous, CrossSourceTransitionPlanReason::AmbiguousIdentity);
            return;
        }
        plan.installed_foreign = foreign;
        plan.expected_install_reason = foreign.reason;
    }
    if(!plan.installed_foreign.has_value() ||
       (plan.expected_install_reason != InstalledPackageReason::Explicit &&
        plan.expected_install_reason != InstalledPackageReason::Dependency)) {
        reject(CrossSourceTransitionPlanStatus::Incomplete, CrossSourceTransitionPlanReason::InstalledReasonUnknown);
        return;
    }
    const auto& replacement = std::get<AurReplacementCandidateQuerySuccess>(evidence.aur_replacement).candidates.front();
    if(!replacement.relations.empty()) {
        reject(CrossSourceTransitionPlanStatus::Unsupported, CrossSourceTransitionPlanReason::ReplacementRelationsRequireAssessment);
        return;
    }
    plan.removal_safety = assess_bounded_removal(snapshot, evidence.installed_consumer.package);
    switch(plan.removal_safety.status) {
        case CrossSourceRemovalSafetyStatus::PreservesRuntimeDependencies: break;
        case CrossSourceRemovalSafetyStatus::Blocked:
            reject(CrossSourceTransitionPlanStatus::Blocked, CrossSourceTransitionPlanReason::ReverseDependency);
            return;
        case CrossSourceRemovalSafetyStatus::Unsupported:
            reject(CrossSourceTransitionPlanStatus::Unsupported, CrossSourceTransitionPlanReason::UnsupportedRuntimeDependency);
            return;
        case CrossSourceRemovalSafetyStatus::Incomplete:
        case CrossSourceRemovalSafetyStatus::NotAssessed:
            reject(CrossSourceTransitionPlanStatus::Incomplete, CrossSourceTransitionPlanReason::IncompleteInstalledInventory);
            return;
    }
    plan.status = CrossSourceTransitionPlanStatus::ReadOnlyReady;
    plan.reason = CrossSourceTransitionPlanReason::None;
    plan.phases = {CrossSourceTransitionPhase::RemoveInstalledForeign,
                   CrossSourceTransitionPhase::RepositorySystemUpgrade,
                   CrossSourceTransitionPhase::InstallAurReplacement,
                   CrossSourceTransitionPhase::VerifyPostState};
}

} // namespace

std::vector<CrossSourceCoordinatedTransitionPlan> plan_cross_source_coordinated_transitions(
    const CrossSourceVersionLockCorrelationResult& correlation) {
    std::vector<CrossSourceCoordinatedTransitionPlan> plans;
    if(!correlation.observation.has_value()) return plans;
    const auto& observation = *correlation.observation;
    std::set<std::size_t> indices;
    std::set<std::string> affected_packages;
    bool overlaps = false;
    for(const auto index : correlation.possible_blocker_assessment_indices) {
        if(index >= correlation.assessments.size()) return {};
        if(!indices.insert(index).second) overlaps = true;
        const auto& evidence = correlation.assessments[index].evidence;
        if(!affected_packages.insert(evidence.installed_consumer.package.package_name).second ||
           !affected_packages.insert(evidence.repository_upgrade.repository_candidate.package_name).second) overlaps = true;
        CrossSourceCoordinatedTransitionPlan plan{
            CrossSourceTransitionPlanStatus::Incomplete,
            CrossSourceTransitionPlanReason::IncompleteObservation,
            correlation.basis,
            observation.status,
            assess_cross_source_version_lock_candidate(evidence),
            observation.issues,
            observation.transition_installed_snapshot,
            std::nullopt,
            InstalledPackageReason::Unknown,
            {},
            {}};
        if(!correlation.failure.has_value()) classify_transition(plan);
        plans.push_back(std::move(plan));
    }
    // Every candidate shares the system-upgrade phase. This slice intentionally
    // supports one correlation only; even disjoint pairs need coordinated phase
    // ordering, and overlapping identities must never get independent removal.
    if(plans.size() > 1) {
        for(auto& plan : plans) {
            plan.status = CrossSourceTransitionPlanStatus::Unsupported;
            plan.reason = overlaps ? CrossSourceTransitionPlanReason::OverlappingCandidates
                                   : CrossSourceTransitionPlanReason::MultipleCandidatesRequireCoordination;
            plan.phases.clear();
        }
    }
    return plans;
}
