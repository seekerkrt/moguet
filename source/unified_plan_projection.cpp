#include "unified_plan_projection.hpp"

#include "aur_update_execution_preparation.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_query.hpp"
#include "aur_devel_update.hpp"
#include "commands_sync.hpp"
#include "filtered_aur_update_operation.hpp"
#include "local_dependency_plan_projection.hpp"
#include "package_metadata.hpp"
#include "root_package_route_projection.hpp"
#include "root_package_search.hpp"
#include "system_aur_update_operation.hpp"
#include "system_source_upgrade.hpp"
#include "upgrade_all_operation.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

[[noreturn]] void reject_inconsistent_input(const char* diagnostic) {
    throw std::invalid_argument(diagnostic);
}

bool is_blocking_constraint(const ConstraintEvaluation& evaluation) noexcept {
    return evaluation.satisfaction() == ConstraintSatisfaction::Unsatisfied ||
           evaluation.satisfaction() == ConstraintSatisfaction::Unknown;
}

bool has_executable_update_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return target.status ==
                   AurUpdateExecutionTargetStatus::Executable;
        });
}

bool all_build_plan_roots_are_executable(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    if(!preflight.build_plan.has_value()) return false;
    std::size_t correlated_root_count = 0;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(!target.build_plan_root_index.has_value()) continue;
        ++correlated_root_count;
        if(target.status != AurUpdateExecutionTargetStatus::Executable ||
           !target.desired_install_reason.has_value()) {
            return false;
        }
    }
    return correlated_root_count != 0 &&
           correlated_root_count ==
               preflight.build_plan->root_targets.size();
}

std::vector<SystemAurUpdateRequiresCheckAttention>
project_system_aur_requires_check_attentions(
    const AurUpdateExecutionPreflight& preflight) {
    if(preflight.devel_requires_check_policy !=
       std::optional<DevelRequiresCheckPolicy>{
           DevelRequiresCheckPolicy::SkipIndependentTarget}) {
        reject_inconsistent_input(
            "System/AUR RequiresCheck attention projection has an invalid policy.");
    }

    std::vector<SystemAurUpdateRequiresCheckAttention> attentions;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(target.skip_kind !=
           std::optional<AurUpdateExecutionSkipKind>{
               AurUpdateExecutionSkipKind::
                   IndependentDevelRequiresCheck}) {
            continue;
        }
        if(target.status != AurUpdateExecutionTargetStatus::Skipped ||
           !is_valid_aur_update_independent_devel_skip_snapshot(
               target.update, target.issues, target.skip_kind,
               DevelRequiresCheckPolicy::SkipIndependentTarget) ||
           !target.update.aur_package.has_value() ||
           !target.issues.front().devel_requires_check_reason ||
           !is_known_devel_requires_check_reason(*target.issues.front().devel_requires_check_reason)) {
            reject_inconsistent_input(
                "System/AUR independent RequiresCheck attention is inconsistent.");
        }

        // The snapshot validator above owns effective-state correlation; the
        // public projection records that proven state without re-running the
        // producer classifier or interpreting its diagnostic text.
        attentions.push_back(SystemAurUpdateRequiresCheckAttention{
            target.update_plan_index,
            target.update.installed_name,
            target.update.aur_package->package_base,
            *target.issues.front().devel_requires_check_reason,
            *target.skip_kind,
            AurUpdateEffectiveState::RequiresCheck});
    }
    return attentions;
}

void append_registered_devel_observations(const SystemSourceUpgradeProjectionAuthority& prepared,
                                          const std::vector<RegisteredAurDevelObservation>* observations, UnifiedPlanObservationInput& out) {
    if(!observations) return;
    for(const auto& observed : *observations) {
        const auto matching = std::find_if(prepared.source_work_items().begin(), prepared.source_work_items().end(), [&](const auto& work) {
            return work.source().original_preference_index == observed.preference_index && work.checkout_package_base() == observed.package_base &&
                   work.required_targets().size() == 1 && work.required_targets().front().package_name == observed.package_name;
        });
        if(matching == prepared.source_work_items().end() || observed.query.plan.entries.size() != 1 || observed.query.plan.entries.front().installed_name != observed.package_name)
            reject_inconsistent_input("Registered devel observation/source attribution differs.");
        out.root_metadata.push_back(UnifiedPlanBorrowedAuthorityReference<AurUpdatePlanEntry>(observed.query.plan.entries.front()));
        for(const auto& issue : observed.issues)
            out.blockers.push_back(RoutePreflightUnifiedPlanBlocker{UnifiedPlanBorrowedAuthorityReference<AurUpdateExecutionIssue>(issue)});
    }
}

void append_build_plan_blockers(
    const BuildPlan& plan, UnifiedPlanObservationInput& observation) {
    for(const BuildPlanResolutionFailure& failure : plan.resolution_failures) {
        observation.blockers.push_back(SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                BuildPlanResolutionFailure>(failure)});
    }
    for(const IncompleteProviderCandidateSet& failure :
        plan.incomplete_provider_candidate_sets) {
        observation.blockers.push_back(SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                IncompleteProviderCandidateSet>(failure)});
    }
    for(const AmbiguousProvidedDependency& ambiguous :
        plan.ambiguous_providers) {
        observation.blockers.push_back(AmbiguousUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AmbiguousProvidedDependency>(ambiguous)});
    }
    const PlanStateProjection state = project_build_plan_state(plan);
    const ExecutionReadiness& install_readiness = execution_readiness(
        state, ExecutionCapability::Install);
    for(const auto& readiness_reason : install_readiness.reasons) {
        const auto* relation = std::get_if<PlanDeclaredRelationReason>(
            &readiness_reason.reason);
        if(relation == nullptr ||
           !readiness_reason.blocks_production_guard) {
            continue;
        }
        observation.blockers.push_back(
            MetadataRiskUnifiedPlanBlocker{*relation});
    }
    for(const BuildPlanDependencyEdge& edge : plan.dependency_edges) {
        if(edge.constraint_evaluation.has_value() &&
           is_blocking_constraint(edge.constraint_evaluation.value())) {
            observation.blockers.push_back(
                ConstraintFailureUnifiedPlanBlocker{
                    UnifiedPlanBorrowedAuthorityReference<
                        BuildPlanDependencyEdge>(edge)});
        } else if(edge.kind == DependencyKind::Unknown) {
            observation.blockers.push_back(UnknownUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    BuildPlanDependencyEdge>(edge)});
        }
    }
    for(std::size_t index = 0; index < plan.unresolved.size(); ++index) {
        observation.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
            BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency,
            index});
    }
    for(std::size_t index = 0; index < plan.cycles.size(); ++index) {
        observation.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
            BuildPlanStateUnifiedPlanBlockerKind::DependencyCycle,
            index});
    }
    for(std::size_t index = 0; index < plan.split_package_targets.size();
        ++index) {
        observation.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
            BuildPlanStateUnifiedPlanBlockerKind::
                SplitPackageSelectionRequired,
            index});
    }
}

void append_fetch_build_plan_blockers(
    const BuildPlan& plan, UnifiedPlanObservationInput& observation) {
    append_build_plan_blockers(plan, observation);
    std::erase_if(
        observation.blockers,
        [](const UnifiedPlanBlocker& blocker) {
            if(std::holds_alternative<
                   MetadataRiskUnifiedPlanBlocker>(blocker)) {
                return true;
            }
            const auto* state =
                std::get_if<BuildPlanStateUnifiedPlanBlocker>(
                    &blocker);
            return state != nullptr &&
                   state->kind ==
                       BuildPlanStateUnifiedPlanBlockerKind::
                           SplitPackageSelectionRequired;
        });
}

void append_repository_dependency_intents(
    const BuildPlan& plan,
    RepositoryPackageTransactionIntent& transaction) {
    for(const BuildPlanDependencyEdge& edge : plan.dependency_edges) {
        if(!edge.resolved_candidate.has_value()) continue;
        const auto* package = std::get_if<RepositoryExactPackage>(
            &edge.resolved_candidate.value());
        if(package == nullptr) continue;
        transaction.targets.push_back(RepositoryDependencyInstallIntent{
            UnifiedPlanBorrowedAuthorityReference<RepositoryExactPackage>(
                *package)});
    }
    for(const BuildPlanProvidedDependency& selected : plan.provided) {
        if(selected.resolution != ProviderResolutionKind::UserSelected ||
           !std::holds_alternative<RepositoryProviderOrigin>(
               selected.provider.origin)) {
            continue;
        }
        transaction.targets.push_back(RepositoryProviderInstallIntent{
            UnifiedPlanBorrowedAuthorityReference<ProvidedDependency>(
                selected.provider)});
    }
}

UnifiedPlanBuildUnitReference build_unit_reference(
    const BuildPlan& plan, std::size_t build_plan_order_index,
    const LocalSourceRootObservationIdentity* local_source,
    const LocalPackageMetadata* local_metadata) {
    if(local_source != nullptr && local_metadata != nullptr &&
       build_plan_order_index < plan.order.size() &&
       plan.order[build_plan_order_index].package_base ==
           local_metadata->package_base) {
        return LocalSourceBuildUnitReference(
            *local_source, std::cref(*local_metadata));
    }
    return AurPackageBaseBuildUnitReference(
        std::cref(plan), build_plan_order_index);
}

void append_artifact_units(
    const BuildPlan& plan,
    const std::vector<ProjectedBuildPlanArtifactTargets>& units,
    UnifiedPlanObservationInput& observation,
    const LocalSourceRootObservationIdentity* local_source = nullptr,
    const LocalPackageMetadata* local_metadata = nullptr) {
    for(const ProjectedBuildPlanArtifactTargets& unit : units) {
        UnifiedPlanBuildUnitReference build_unit = build_unit_reference(
            plan, unit.build_plan_order_index, local_source,
            local_metadata);
        observation.build_units.push_back(std::move(build_unit));
        for(const RequiredPackageArtifactTarget& target :
            unit.required_targets) {
            observation.required_artifacts.emplace_back(
                build_unit_reference(
                    plan, unit.build_plan_order_index, local_source,
                    local_metadata),
                std::cref(target));
        }
    }
}

void append_artifact_projection(
    const BuildPlan& plan,
    const BuildPlanArtifactTargetProjectionResult& projection,
    UnifiedPlanObservationInput& observation,
    const LocalSourceRootObservationIdentity* local_source = nullptr,
    const LocalPackageMetadata* local_metadata = nullptr) {
    if(const auto* failure = projection.failure(); failure != nullptr) {
        for(const BuildPlanArtifactTargetProjectionIssue& issue :
            failure->issues) {
            observation.blockers.push_back(
                BuildPlanArtifactProjectionUnifiedPlanBlocker{
                    issue});
        }
        return;
    }
    append_artifact_units(
        plan, projection.success()->build_units, observation,
        local_source, local_metadata);
}

void append_prepared_source_work(
    const SystemSourceUpgradeProjectionAuthority& prepared,
    UnifiedPlanObservationInput& observation) {
    for(const PreparedSystemSourceWorkReference& work :
        prepared.source_work_items()) {
        UnifiedPlanBuildUnitReference build_unit =
            PreparedSystemSourceBuildUnitReference(
                std::cref(work.source()),
                std::cref(work.requested_package_name()),
                std::cref(work.checkout_package_base()),
                work.required_target_provenance(),
                work.artifact_lifecycle_intent(),
                work.uses_system_update_baseline(),
                std::cref(work.required_targets()));
        observation.build_units.push_back(std::move(build_unit));
        for(const RequiredPackageArtifactTarget& target :
            work.required_targets()) {
            observation.required_artifacts.emplace_back(
                PreparedSystemSourceBuildUnitReference(
                    std::cref(work.source()),
                    std::cref(work.requested_package_name()),
                    std::cref(work.checkout_package_base()),
                    work.required_target_provenance(),
                    work.artifact_lifecycle_intent(),
                    work.uses_system_update_baseline(),
                    std::cref(work.required_targets())),
                std::cref(target));
        }
    }
}

void append_prepared_remote_source_work(
    const PreparedRemoteSourceBuild& prepared,
    UnifiedPlanObservationInput& observation) {
    if(prepared.invocation.work_items.size() != 1) {
        reject_inconsistent_input(
            "Prepared repository source build has an invalid work-item count.");
    }
    const ProductionSourceBuildWorkItem& work_item =
        prepared.invocation.work_items.front();
    if(work_item.artifact_lifecycle_intent !=
           ArtifactLifecycleIntent::PackageBaseSet ||
       work_item.request.only_if_updated || work_item.request.needed) {
        reject_inconsistent_input(
            "Prepared repository source build has an invalid standalone lifecycle policy.");
    }
    PreparedRemoteSourceBuildUnitReference build_unit(
        std::cref(prepared.source), std::cref(work_item));
    if(!build_unit.has_complete_identity()) {
        reject_inconsistent_input(
            "Prepared repository source build identity is inconsistent.");
    }
    observation.build_units.push_back(
        PreparedRemoteSourceBuildUnitReference(
            std::cref(prepared.source), std::cref(work_item)));
    for(const RequiredPackageArtifactTarget& target :
        work_item.required_targets) {
        observation.required_artifacts.emplace_back(
            PreparedRemoteSourceBuildUnitReference(
                std::cref(prepared.source), std::cref(work_item)),
            std::cref(target));
    }
}

void append_source_artifact_transaction(
    UnifiedPlanObservationInput& observation, bool needed,
    UnifiedPlanTransactionIntentStage stage =
        UnifiedPlanTransactionIntentStage::RouteOwned) {
    if(observation.required_artifacts.empty()) return;

    SourceBuiltArtifactInstallBoundaryIntent transaction;
    transaction.needed = needed;
    transaction.stage = stage;
    transaction.targets.reserve(observation.required_artifacts.size());
    for(std::size_t index = 0;
        index < observation.required_artifacts.size(); ++index) {
        if(observation.required_artifacts[index].target().desired_reason ==
           DesiredInstallReason::Explicit) {
            transaction.targets.push_back(SourceRootArtifactInstallIntent{
                index});
        } else {
            transaction.targets.push_back(
                SourceDependencyArtifactInstallIntent{index});
        }
    }
    observation.transaction_intents.push_back(std::move(transaction));
}

const RootPackageSearchCandidate* find_root_metadata(
    const RootPackageSearchSnapshot& discovery,
    const SelectedRootPackageTarget& selected) {
    auto match = [&selected](const RootPackageSearchCandidate& candidate) {
        return same_root_package_identity(
            candidate.candidate.identity(), selected.identity());
    };
    auto found = std::find_if(
        discovery.candidates.begin(), discovery.candidates.end(), match);
    return found == discovery.candidates.end() ? nullptr : &*found;
}

bool same_aur_remote_package(
    const std::optional<AurUpdateRemotePackage>& lhs,
    const std::optional<AurUpdateRemotePackage>& rhs) noexcept {
    if(lhs.has_value() != rhs.has_value()) return false;
    if(!lhs.has_value()) return true;
    return lhs->aur_name == rhs->aur_name &&
           lhs->package_base == rhs->package_base &&
           lhs->version == rhs->version &&
           lhs->version_relation == rhs->version_relation;
}

bool same_aur_update_entry(
    const AurUpdatePlanEntry& lhs,
    const AurUpdatePlanEntry& rhs) noexcept {
    return lhs.installed_name == rhs.installed_name &&
           lhs.installed_version == rhs.installed_version &&
           lhs.install_reason == rhs.install_reason &&
           same_aur_remote_package(lhs.aur_package, rhs.aur_package) &&
           lhs.classification == rhs.classification &&
           lhs.devel_classification == rhs.devel_classification &&
           lhs.devel_assessment_origin == rhs.devel_assessment_origin &&
           lhs.devel_assessment == rhs.devel_assessment;
}

bool source_observation_matches_ready_preflight(
    const AurUpdateSourceBuildObservation& source,
    const AurUpdateExecutionPreflight& preflight) noexcept {
    if(!source.issues.empty() || !source.warnings.empty() ||
       !source.production_preflight.has_value() ||
       !source.externally_satisfied_build_units.empty() ||
       !preflight.build_plan.has_value() ||
       source.affected_roots != preflight.build_plan->root_targets) {
        return false;
    }

    std::vector<const AurUpdateExecutionTarget*> executable_targets;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(target.status == AurUpdateExecutionTargetStatus::Executable) {
            executable_targets.push_back(&target);
        }
    }
    if(executable_targets.empty() ||
       executable_targets.size() != source.affected_update_targets.size() ||
       source.projected_build_units.size() !=
           preflight.build_plan->order.size() ||
       source.build_unit_selection.entries.size() !=
           preflight.build_plan->order.size() ||
       source.work_item_attributions.size() !=
           source.production_preflight->work_items.size() ||
       source.work_item_attributions.empty()) {
        return false;
    }
    for(std::size_t index = 0; index < executable_targets.size(); ++index) {
        const AurUpdateExecutionTarget& expected = *executable_targets[index];
        const AurUpdateExecutionTarget& actual =
            source.affected_update_targets[index];
        if(!is_valid_aur_update_execution_target_skip_snapshot(actual) ||
           actual.status != AurUpdateExecutionTargetStatus::Executable ||
           actual.update_plan_index != expected.update_plan_index ||
           actual.build_plan_root_index != expected.build_plan_root_index ||
           actual.desired_install_reason != expected.desired_install_reason ||
           !same_aur_update_entry(actual.update, expected.update)) {
            return false;
        }
    }
    const auto expected_providers_for_package_base =
        [&preflight](const std::string& package_base) {
            std::vector<ProvidedDependency> providers;
            for(const BuildPlanDependencyEdge& edge :
                preflight.build_plan->dependency_edges) {
                if(edge.parent_package_base != package_base ||
                   edge.kind != DependencyKind::Provided ||
                   edge.provider_resolution !=
                       ProviderResolutionKind::UserSelected ||
                   !edge.resolved_provider.has_value() ||
                   !std::holds_alternative<RepositoryProviderOrigin>(
                       edge.resolved_provider->origin)) {
                    continue;
                }
                const ProvidedDependency& provider =
                    *edge.resolved_provider;
                if(std::none_of(
                       providers.begin(), providers.end(),
                       [&provider](const ProvidedDependency& existing) {
                           return same_provider_identity(
                               existing, provider);
                       })) {
                    providers.push_back(provider);
                }
            }
            return providers;
        };
    std::vector<ProvidedDependency> expected_providers;
    for(const BuildPlanEntry& unit : preflight.build_plan->order) {
        for(const ProvidedDependency& provider :
            expected_providers_for_package_base(unit.package_base)) {
            if(std::none_of(
                   expected_providers.begin(), expected_providers.end(),
                   [&provider](const ProvidedDependency& existing) {
                       return same_provider_identity(existing, provider);
                   })) {
                expected_providers.push_back(provider);
            }
        }
    }
    if(source.production_preflight->selected_repository_providers !=
       expected_providers) {
        return false;
    }

    for(std::size_t index = 0;
        index < source.production_preflight->work_items.size(); ++index) {
        const ProductionSourceBuildWorkItemObservation& work_item =
            source.production_preflight->work_items[index];
        const AurUpdatePreparedWorkItemAttribution& attribution =
            source.work_item_attributions[index];
        if(attribution.invocation_work_item_index != index ||
           attribution.build_plan_order_index >=
               preflight.build_plan->order.size() ||
           attribution.build_plan_order_index >=
               source.projected_build_units.size()) {
            return false;
        }
        const BuildPlanEntry& plan_unit =
            preflight.build_plan
                ->order[attribution.build_plan_order_index];
        const AurUpdateProjectedBuildUnit& projected_unit =
            source.projected_build_units
                [attribution.build_plan_order_index];
        const std::vector<ProvidedDependency> expected_work_item_providers =
            expected_providers_for_package_base(plan_unit.package_base);
        const PackageBaseIdentity* reviewed_identity =
            work_item.request.aur_review_identity.has_value()
                ? &*work_item.request.aur_review_identity
                : nullptr;
        const SourceLocationIdentity* reviewed_location =
            reviewed_identity != nullptr
                ? &reviewed_identity->source().location()
                : nullptr;
        if(work_item.request.reviewed_state !=
               ReviewedSourceFatalStateObservationStatus::Completed ||
           reviewed_identity == nullptr || reviewed_location == nullptr ||
           reviewed_identity->source().kind() != PackageSourceKind::Aur ||
           reviewed_location->kind() != SourceLocationKind::GitRemote ||
           reviewed_location->state() != SourceLocationState::Known ||
           reviewed_location->value() == nullptr ||
           *reviewed_location->value() != work_item.request.git_url ||
           reviewed_identity->package_base() !=
               work_item.request.checkout_name ||
           work_item.request.checkout_name != plan_unit.package_base ||
           attribution.package_base != plan_unit.package_base ||
           work_item.request.package_name != attribution.package_name ||
           work_item.required_target_provenance !=
               RequiredTargetProvenance::AurBuildPlanProjection ||
           work_item.artifact_lifecycle_intent !=
               ArtifactLifecycleIntent::PackageBaseSet ||
           work_item.repository_identity.has_value() ||
           work_item.uses_system_update_baseline ||
           work_item.request.only_if_updated || work_item.request.needed ||
           !work_item.request.custom_environment
                .ordered_assignments.empty() ||
           work_item.configured_repository_order !=
               preflight.build_plan->configured_repository_order ||
           work_item.selected_repository_providers !=
               expected_work_item_providers ||
           work_item.required_targets.size() !=
               attribution.required_target_attributions.size() ||
           attribution.required_target_attributions.size() !=
               projected_unit.required_target_attributions.size()) {
            return false;
        }
        for(std::size_t child_index = 0;
            child_index < work_item.required_targets.size(); ++child_index) {
            const RequiredPackageArtifactTarget& observed =
                work_item.required_targets[child_index];
            const RequiredPackageArtifactTarget& attributed =
                attribution.required_target_attributions[child_index]
                    .required_target;
            const RequiredPackageArtifactTarget& projected =
                projected_unit.required_target_attributions[child_index]
                    .required_target;
            if(observed.package_base != attributed.package_base ||
               observed.package_name != attributed.package_name ||
               observed.desired_reason != attributed.desired_reason ||
               observed.package_base != projected.package_base ||
               observed.package_name != projected.package_name ||
               observed.desired_reason != projected.desired_reason) {
                return false;
            }
        }
    }
    return true;
}

bool source_observation_matches_noop(
    const AurUpdateSourceBuildObservation& source) noexcept {
    return source.issues.empty() && source.warnings.empty() &&
           source.affected_update_targets.empty() &&
           source.affected_roots.empty() &&
           source.build_unit_selection.entries.empty() &&
           source.projected_build_units.empty() &&
           source.externally_satisfied_build_units.empty() &&
           source.work_item_attributions.empty() &&
           !source.production_preflight.has_value();
}

bool has_package_role(
    const PlannedPackageTarget& target, PackageRole role) noexcept {
    return std::find(target.roles.begin(), target.roles.end(), role) !=
           target.roles.end();
}

const PlannedPackageTarget& validate_plan_root_identity(
    const BuildPlan& plan,
    std::size_t root_index,
    const std::string& requested_name,
    const std::string& package_base,
    const char* diagnostic) {
    if(root_index >= plan.root_targets.size()) {
        reject_inconsistent_input(diagnostic);
    }
    const RootTargetIdentity& root = plan.root_targets[root_index];
    if(root.invocation_index != root_index ||
       root.requested_name != requested_name || package_base.empty()) {
        reject_inconsistent_input(diagnostic);
    }

    const PlannedPackageTarget* matched_target = nullptr;
    for(const PlannedPackageTarget& target : plan.package_targets) {
        if(target.package_name != requested_name ||
           target.package_base != package_base ||
           !has_package_role(target, PackageRole::Root) ||
           std::count(target.roots.begin(), target.roots.end(), root) != 1) {
            continue;
        }
        if(matched_target != nullptr) reject_inconsistent_input(diagnostic);
        matched_target = &target;
    }
    if(matched_target == nullptr) reject_inconsistent_input(diagnostic);
    return *matched_target;
}

const PlannedPackageTarget* find_plan_root_target(
    const BuildPlan& plan, std::size_t root_index,
    const char* diagnostic) {
    if(root_index >= plan.root_targets.size()) {
        reject_inconsistent_input(diagnostic);
    }
    const RootTargetIdentity& root = plan.root_targets[root_index];
    const PlannedPackageTarget* matched = nullptr;
    for(const PlannedPackageTarget& target : plan.package_targets) {
        if(target.package_name != root.requested_name ||
           !has_package_role(target, PackageRole::Root) ||
           std::count(target.roots.begin(), target.roots.end(), root) != 1) {
            continue;
        }
        if(matched != nullptr) reject_inconsistent_input(diagnostic);
        matched = &target;
    }
    if(matched != nullptr && matched->package_base.empty()) {
        reject_inconsistent_input(diagnostic);
    }
    return matched;
}

const PlannedPackageTarget& require_plan_root_target(
    const BuildPlan& plan, std::size_t root_index,
    const char* diagnostic) {
    const PlannedPackageTarget* matched =
        find_plan_root_target(plan, root_index, diagnostic);
    if(matched == nullptr) reject_inconsistent_input(diagnostic);
    return *matched;
}

void validate_plan_roots(
    const BuildPlan& plan,
    const std::vector<AurRootPackageIdentity>& expected_roots,
    const char* diagnostic) {
    if(plan.root_targets.size() != expected_roots.size()) {
        reject_inconsistent_input(diagnostic);
    }
    for(std::size_t index = 0; index < expected_roots.size(); ++index) {
        validate_plan_root_identity(
            plan, index, expected_roots[index].package_name,
            expected_roots[index].package_base, diagnostic);
    }
}

void validate_blocked_plan_roots(
    const BuildPlan& plan,
    const std::vector<AurRootPackageIdentity>& expected_roots,
    const char* diagnostic) {
    if(plan.root_targets.size() != expected_roots.size()) {
        reject_inconsistent_input(diagnostic);
    }
    for(std::size_t index = 0; index < expected_roots.size(); ++index) {
        const RootTargetIdentity& root = plan.root_targets[index];
        if(root.invocation_index != index ||
           root.requested_name != expected_roots[index].package_name ||
           expected_roots[index].package_base.empty()) {
            reject_inconsistent_input(diagnostic);
        }

        const PlannedPackageTarget* planned_root = nullptr;
        for(const PlannedPackageTarget& target : plan.package_targets) {
            if(!has_package_role(target, PackageRole::Root) ||
               std::count(target.roots.begin(), target.roots.end(), root) ==
                   0) {
                continue;
            }
            if(planned_root != nullptr ||
               target.package_name != expected_roots[index].package_name ||
               target.package_base != expected_roots[index].package_base ||
               std::count(
                   target.roots.begin(), target.roots.end(), root) != 1) {
                reject_inconsistent_input(diagnostic);
            }
            planned_root = &target;
        }
        if(planned_root != nullptr) continue;

        const bool failed_before_root_target = std::any_of(
            plan.resolution_failures.begin(),
            plan.resolution_failures.end(),
            [&root](const BuildPlanResolutionFailure& failure) {
                return std::count(
                           failure.roots.begin(),
                           failure.roots.end(), root) == 1;
            });
        if(!failed_before_root_target) reject_inconsistent_input(diagnostic);
    }
}

void validate_root_package_prepared(
    const PreparedRootPackageInstall& prepared) {
    if(!prepared.discovery_snapshot.has_value() ||
       !prepared.routing_projection.has_value()) {
        reject_inconsistent_input(
            "Prepared root package projection authorities are incomplete.");
    }

    const RootPackageSearchSnapshot& discovery =
        prepared.discovery_snapshot.value();
    const RootPackageRoutingProjection& routing =
        prepared.routing_projection.value();
    if(routing.repository_targets().empty() && routing.aur_targets().empty()) {
        reject_inconsistent_input(
            "Prepared root package projection has no selected target.");
    }
    if(prepared.exact_repository_targets.size() !=
       routing.repository_targets().size()) {
        reject_inconsistent_input(
            "Prepared repository root targets do not match routing.");
    }
    for(std::size_t index = 0;
        index < routing.repository_targets().size(); ++index) {
        const RepositoryRootPackageRouteTarget& target =
            routing.repository_targets()[index];
        if(prepared.exact_repository_targets[index] !=
               target.exact_package_target() ||
           find_root_metadata(discovery, target.selected_target()) ==
               nullptr) {
            reject_inconsistent_input(
                "Prepared repository root correlation is inconsistent.");
        }
    }

    std::vector<AurRootPackageIdentity> aur_roots;
    aur_roots.reserve(routing.aur_targets().size());
    for(const AurRootPackageRouteTarget& target : routing.aur_targets()) {
        if(find_root_metadata(discovery, target.selected_target()) == nullptr) {
            reject_inconsistent_input(
                "Prepared AUR root discovery correlation is inconsistent.");
        }
        aur_roots.push_back(target.identity());
    }
    if(aur_roots.empty()) {
        if(prepared.aur_build_plan.has_value() ||
           prepared.source_invocation.has_value()) {
            reject_inconsistent_input(
                "Repository-only root preparation retained AUR authority.");
        }
        return;
    }
    if(!prepared.aur_build_plan.has_value() ||
       !prepared.source_invocation.has_value() ||
       prepared.source_invocation->work_items.empty()) {
        reject_inconsistent_input(
            "Prepared AUR root authorities are incomplete.");
    }
    validate_plan_roots(
        prepared.aur_build_plan.value(), aur_roots,
        "Prepared AUR root BuildPlan correlation is inconsistent.");
}

bool same_required_artifact_target(
    const RequiredPackageArtifactTarget& lhs,
    const RequiredPackageArtifactTarget& rhs) noexcept {
    return lhs.package_base == rhs.package_base &&
           lhs.package_name == rhs.package_name &&
           lhs.desired_reason == rhs.desired_reason;
}

void validate_configured_repository_identity(
    const ConfiguredRepositoryIdentity& identity,
    const std::vector<std::string>& configured_order,
    const char* diagnostic) {
    if(identity.repository_name.empty() ||
       identity.configured_order >= configured_order.size() ||
       configured_order[identity.configured_order] !=
           identity.repository_name) {
        reject_inconsistent_input(diagnostic);
    }
}

void validate_repository_provider_identity(
    const ProvidedDependency& provider,
    const std::vector<std::string>* configured_order,
    const char* diagnostic) {
    const auto* repository =
        std::get_if<RepositoryProviderOrigin>(&provider.origin);
    if(repository == nullptr) return;
    if(configured_order == nullptr ||
       !repository->configured_order.has_value()) {
        reject_inconsistent_input(diagnostic);
    }
    validate_configured_repository_identity(
        ConfiguredRepositoryIdentity{
            repository->repository_name,
            repository->configured_order.value()},
        *configured_order, diagnostic);
}

const std::vector<std::string>* validate_build_plan_repository_authority(
    const BuildPlan& plan) {
    const std::vector<std::string>* configured_order =
        plan.configured_repository_order.has_value()
            ? &plan.configured_repository_order.value()
            : nullptr;
    constexpr const char* diagnostic =
        "BuildPlan repository identity does not match its resolution configuration.";

    for(const BuildPlanDependencyEdge& edge : plan.dependency_edges) {
        if(edge.resolved_provider.has_value()) {
            validate_repository_provider_identity(
                edge.resolved_provider.value(), configured_order,
                diagnostic);
        }
        if(!edge.resolved_candidate.has_value()) continue;
        std::visit(
            [configured_order](const auto& candidate) {
                using Candidate = std::decay_t<decltype(candidate)>;
                if constexpr(std::is_same_v<
                                 Candidate,
                                 RepositoryExactPackage>) {
                    if(configured_order == nullptr) {
                        reject_inconsistent_input(
                            "BuildPlan exact repository package lacks resolution configuration authority.");
                    }
                    validate_configured_repository_identity(
                        candidate.repository, *configured_order,
                        "BuildPlan exact repository package does not match its resolution configuration.");
                } else if constexpr(std::is_same_v<
                                        Candidate,
                                        ProviderResolvedDependencyCandidate>) {
                    validate_repository_provider_identity(
                        candidate.provider, configured_order,
                        "BuildPlan resolved provider does not match its resolution configuration.");
                }
            },
            edge.resolved_candidate.value());
    }
    for(const BuildPlanProvidedDependency& selected : plan.provided) {
        validate_repository_provider_identity(
            selected.provider, configured_order, diagnostic);
    }
    for(const AmbiguousProvidedDependency& ambiguous :
        plan.ambiguous_providers) {
        for(const ProvidedDependency& candidate : ambiguous.candidates) {
            validate_repository_provider_identity(
                candidate, configured_order, diagnostic);
        }
    }
    for(const IncompleteProviderCandidateSet& incomplete :
        plan.incomplete_provider_candidate_sets) {
        for(const ProvidedDependency& candidate :
            incomplete.observed_candidates) {
            validate_repository_provider_identity(
                candidate, configured_order, diagnostic);
        }
    }
    return configured_order;
}

void merge_repository_order_authority(
    const std::vector<std::string>* candidate,
    const std::vector<std::string>*& authority,
    const char* diagnostic) {
    if(candidate == nullptr) return;
    if(authority == nullptr) {
        authority = candidate;
        return;
    }
    if(*authority != *candidate) reject_inconsistent_input(diagnostic);
}

const std::vector<std::string>* validate_prepared_work_repository_authority(
    const PreparedSystemSourceWorkReference& work) {
    const std::vector<std::string>* configured_order =
        work.configured_repository_order().has_value()
            ? &work.configured_repository_order().value()
            : nullptr;
    for(const ProvidedDependency& provider :
        work.selected_repository_providers()) {
        validate_repository_provider_identity(
            provider, configured_order,
            "Prepared source provider does not match its resolution configuration.");
    }
    return configured_order;
}

const std::vector<std::string>* validate_aur_source_work(
    const BuildPlan& plan,
    const PreparedProductionSourceBuildInvocation& invocation,
    bool needed,
    const BuildPlanArtifactTargetProjectionResult& projection) {
    const auto& work_items = invocation.work_items;
    if(work_items.size() != plan.order.size()) {
        reject_inconsistent_input(
            "Prepared AUR source work does not match BuildPlan execution units.");
    }
    for(std::size_t index = 0; index < work_items.size(); ++index) {
        const ProductionSourceBuildWorkItem& work_item = work_items[index];
        const BuildPlanEntry& plan_entry = plan.order[index];
        if(work_item.required_target_provenance !=
               RequiredTargetProvenance::AurBuildPlanProjection ||
           work_item.artifact_lifecycle_intent !=
               ArtifactLifecycleIntent::PackageBaseSet ||
           work_item.request.needed != needed ||
           work_item.request.checkout_name != plan_entry.package_base ||
           work_item.required_targets.empty() ||
           std::any_of(
               work_item.required_targets.begin(),
               work_item.required_targets.end(),
               [&plan_entry](
                   const RequiredPackageArtifactTarget& target) {
                   return target.package_base != plan_entry.package_base ||
                          std::find(
                              plan_entry.package_names.begin(),
                              plan_entry.package_names.end(),
                              target.package_name) ==
                              plan_entry.package_names.end();
               })) {
            reject_inconsistent_input(
                "Prepared AUR source work identity or needed policy differs from its owner.");
        }
    }
    const BuildPlanArtifactTargetProjectionSuccess* success =
        projection.success();
    if(success == nullptr) {
        return validate_build_plan_repository_authority(
            plan);
    }
    const std::vector<std::string>* plan_repository_order =
        validate_build_plan_repository_authority(plan);
    if(work_items.size() != success->build_units.size()) {
        reject_inconsistent_input(
            "Prepared AUR source work does not match its BuildPlan.");
    }
    for(std::size_t index = 0; index < work_items.size(); ++index) {
        const ProductionSourceBuildWorkItem& work_item = work_items[index];
        const auto& actual_targets = work_items[index].required_targets;
        const ProjectedBuildPlanArtifactTargets& projected_unit =
            success->build_units[index];
        const auto& projected_targets = projected_unit.required_targets;
        if(actual_targets.size() != projected_targets.size() ||
           work_item.required_target_provenance !=
               RequiredTargetProvenance::AurBuildPlanProjection ||
           work_item.artifact_lifecycle_intent !=
               ArtifactLifecycleIntent::PackageBaseSet ||
           work_item.request.checkout_name != projected_unit.package_base ||
           (actual_targets.size() == 1
                ? work_item.request.package_name !=
                      actual_targets.front().package_name
                : !work_item.request.package_name.empty()) ||
           !std::equal(
               actual_targets.begin(), actual_targets.end(),
               projected_targets.begin(),
               same_required_artifact_target)) {
            reject_inconsistent_input(
                "Prepared AUR source targets do not match its BuildPlan.");
        }
        const std::vector<std::string>* work_repository_order =
            work_item.configured_repository_order.has_value()
                ? &work_item.configured_repository_order.value()
                : nullptr;
        merge_repository_order_authority(
            work_repository_order, plan_repository_order,
            "Prepared AUR source work used a different repository configuration from its BuildPlan.");
        for(const ProvidedDependency& provider :
            work_item.selected_repository_providers) {
            validate_repository_provider_identity(
                provider, work_repository_order,
                "Prepared AUR source work provider does not match its resolution configuration.");
            const bool belongs_to_plan = std::any_of(
                plan.provided.begin(),
                plan.provided.end(),
                [&provider](
                    const BuildPlanProvidedDependency& selected) {
                    return selected.resolution ==
                               ProviderResolutionKind::UserSelected &&
                           same_provider_identity(
                               selected.provider, provider);
                });
            if(!belongs_to_plan) {
                reject_inconsistent_input(
                    "Prepared AUR source work provider does not belong to its BuildPlan.");
            }
        }
        std::vector<const ProvidedDependency*> expected_providers;
        for(const BuildPlanDependencyEdge& edge :
            plan.dependency_edges) {
            if(edge.parent_package_base != work_item.request.checkout_name ||
               edge.provider_resolution !=
                   ProviderResolutionKind::UserSelected ||
               !edge.resolved_provider.has_value() ||
               !std::holds_alternative<RepositoryProviderOrigin>(
                   edge.resolved_provider->origin)) {
                continue;
            }
            const ProvidedDependency* expected =
                &edge.resolved_provider.value();
            if(std::none_of(
                   expected_providers.begin(), expected_providers.end(),
                   [expected](const ProvidedDependency* existing) {
                       return same_provider_identity(*existing, *expected);
                   })) {
                expected_providers.push_back(expected);
            }
        }
        if(expected_providers.size() !=
               work_item.selected_repository_providers.size() ||
           std::any_of(
               expected_providers.begin(), expected_providers.end(),
               [&work_item](const ProvidedDependency* expected) {
                   return std::none_of(
                       work_item.selected_repository_providers.begin(),
                       work_item.selected_repository_providers.end(),
                       [expected](
                           const ProvidedDependency& actual) {
                           return same_provider_identity(
                               *expected, actual);
                       });
               })) {
            reject_inconsistent_input(
                "Prepared AUR source work lost selected provider authority.");
        }
    }
    return plan_repository_order;
}

const std::vector<std::string>* validate_root_source_work(
    const PreparedRootPackageInstall& prepared,
    const BuildPlanArtifactTargetProjectionResult& projection) {
    return validate_aur_source_work(
        prepared.aur_build_plan.value(),
        prepared.source_invocation.value(), prepared.needed,
        projection);
}

void validate_local_source_input(
    const LocalSourceRoot& source_root,
    const LocalBuildPlan& local_plan) {
    const LocalPackageMetadataParseResult* parse_result =
        source_root.metadata().parse_result();
    if(parse_result == nullptr || !parse_result->is_success() ||
       parse_result->metadata() == nullptr ||
       *parse_result->metadata() != local_plan.local_metadata()) {
        reject_inconsistent_input(
            "Local source root and LocalBuildPlan metadata do not match.");
    }
}

const std::vector<std::string>* validate_aur_update_input(
    const AurUpdateQueryResult& query,
    const AurUpdateExecutionPreflight& preflight) {
    if(!has_valid_aur_update_execution_policy_snapshot(preflight)) {
        reject_inconsistent_input(
            "AUR execution policy or typed skip snapshot is inconsistent.");
    }
    if(query.plan.entries.size() != preflight.targets.size()) {
        reject_inconsistent_input(
            "AUR query and execution preflight target counts do not match.");
    }

    const BuildPlan* plan = preflight.build_plan.has_value()
                                ? &preflight.build_plan.value()
                                : nullptr;
    const std::vector<std::string>* repository_order = plan == nullptr
                                                           ? nullptr
                                                           : validate_build_plan_repository_authority(*plan);
    std::vector<bool> observed_plan_roots(
        plan == nullptr ? 0 : plan->root_targets.size(), false);
    for(std::size_t index = 0; index < preflight.targets.size(); ++index) {
        const AurUpdateExecutionTarget& target = preflight.targets[index];
        if(target.update_plan_index != index ||
           !same_aur_update_entry(target.update, query.plan.entries[index])) {
            reject_inconsistent_input(
                "AUR query and execution preflight entries do not match.");
        }
        if(target.status == AurUpdateExecutionTargetStatus::Executable &&
           (plan == nullptr || !target.build_plan_root_index.has_value())) {
            reject_inconsistent_input(
                "Executable AUR update target lacks BuildPlan authority.");
        }
        if(!target.build_plan_root_index.has_value()) continue;
        if(plan == nullptr ||
           target.build_plan_root_index.value() >=
               plan->root_targets.size() ||
           observed_plan_roots[target.build_plan_root_index.value()]) {
            reject_inconsistent_input(
                "AUR update BuildPlan root correlation is inconsistent.");
        }
        const RootTargetIdentity& root = plan->root_targets[target.build_plan_root_index.value()];
        if(root.invocation_index != target.build_plan_root_index.value() ||
           root.requested_name != target.update.installed_name) {
            reject_inconsistent_input(
                "AUR update target and BuildPlan root do not match.");
        }
        if(!target.update.aur_package.has_value()) {
            reject_inconsistent_input(
                "AUR update BuildPlan root lacks PackageBase authority.");
        }
        validate_plan_root_identity(
            *plan, target.build_plan_root_index.value(),
            target.update.aur_package->aur_name,
            target.update.aur_package->package_base,
            "AUR update target and BuildPlan PackageBase do not match.");
        observed_plan_roots[target.build_plan_root_index.value()] = true;
    }
    if(std::any_of(
           observed_plan_roots.begin(), observed_plan_roots.end(),
           [](bool observed) { return !observed; })) {
        reject_inconsistent_input(
            "AUR update BuildPlan contains an uncorrelated root.");
    }
    return repository_order;
}

bool is_known_install_reason(DesiredInstallReason reason) noexcept {
    return reason == DesiredInstallReason::Explicit ||
           reason == DesiredInstallReason::Dependency;
}

std::vector<ProjectedBuildPlanArtifactTargets>
project_aur_update_artifact_targets(
    const AurUpdateExecutionPreflight& preflight,
    const BuildPlanArtifactTargetProjectionResult& projection) {
    if(projection.success() == nullptr) return {};
    const BuildPlan& plan = preflight.build_plan.value();
    std::vector<ProjectedBuildPlanArtifactTargets> units =
        projection.success()->build_units;

    for(const AurUpdateExecutionTarget& update_target : preflight.targets) {
        if(!update_target.build_plan_root_index.has_value()) continue;
        if(update_target.status !=
               AurUpdateExecutionTargetStatus::Executable ||
           !update_target.desired_install_reason.has_value() ||
           !is_known_install_reason(
               update_target.desired_install_reason.value()) ||
           !update_target.update.aur_package.has_value()) {
            reject_inconsistent_input(
                "AUR update BuildPlan root lacks executable install-reason authority.");
        }

        const std::size_t root_index =
            update_target.build_plan_root_index.value();
        const AurUpdateRemotePackage& remote =
            update_target.update.aur_package.value();
        const PlannedPackageTarget& planned_root =
            validate_plan_root_identity(
                plan, root_index,
                update_target.update.installed_name,
                remote.package_base,
                "AUR update root identity and required child do not match.");

        RequiredPackageArtifactTarget* matched_child = nullptr;
        for(ProjectedBuildPlanArtifactTargets& unit : units) {
            if(unit.build_plan_order_index >= plan.order.size() ||
               unit.package_base !=
                   plan.order[unit.build_plan_order_index].package_base) {
                reject_inconsistent_input(
                    "AUR update artifact unit lost its BuildPlan order correlation.");
            }
            for(RequiredPackageArtifactTarget& child :
                unit.required_targets) {
                if(child.package_base != remote.package_base ||
                   child.package_name !=
                       update_target.update.installed_name) {
                    continue;
                }
                if(matched_child != nullptr) {
                    reject_inconsistent_input(
                        "AUR update root matches multiple required children.");
                }
                matched_child = &child;
            }
        }
        if(matched_child == nullptr ||
           planned_root.package_base != matched_child->package_base ||
           planned_root.package_name != matched_child->package_name) {
            reject_inconsistent_input(
                "AUR update root does not match its required child.");
        }
        matched_child->desired_reason =
            update_target.desired_install_reason.value();
    }
    return units;
}

const std::vector<std::string>* validate_system_source_authority(
    const SystemSourceUpgradeProjectionAuthority& prepared) {
    const SystemSourceUpgradePreparedSnapshot& snapshot = prepared.snapshot();
    std::vector<bool> observed_sources(
        snapshot.registered_sources.size(), false);
    std::vector<AurRootPackageIdentity> aur_roots;
    const BuildPlan* plan = prepared.aur_invocation_plan();
    const std::vector<std::string>* repository_order = plan == nullptr
                                                           ? nullptr
                                                           : validate_build_plan_repository_authority(*plan);
    for(const PreparedSystemSourceWorkReference& work :
        prepared.source_work_items()) {
        if(work.needed() != snapshot.options.needed) {
            reject_inconsistent_input(
                "Prepared source work needed policy differs from its owner.");
        }
        merge_repository_order_authority(
            validate_prepared_work_repository_authority(work),
            repository_order,
            "Prepared source work items used different repository configurations.");
        const RegisteredSourcePreferenceSnapshot* source = &work.source();
        auto source_position = std::find_if(
            snapshot.registered_sources.begin(),
            snapshot.registered_sources.end(),
            [source](const RegisteredSourcePreferenceSnapshot& candidate) {
                return &candidate == source;
            });
        if(source_position == snapshot.registered_sources.end()) {
            reject_inconsistent_input(
                "Prepared source work does not belong to its snapshot.");
        }
        const std::size_t index = static_cast<std::size_t>(std::distance(
            snapshot.registered_sources.begin(), source_position));
        if(observed_sources[index] ||
           !PreparedSystemSourceBuildUnitReference(
                std::cref(work.source()),
                std::cref(work.requested_package_name()),
                std::cref(work.checkout_package_base()),
                work.required_target_provenance(),
                work.artifact_lifecycle_intent(),
                work.uses_system_update_baseline(),
                std::cref(work.required_targets()))
                .has_complete_identity()) {
            reject_inconsistent_input(
                "Prepared source work identity is inconsistent.");
        }
        observed_sources[index] = true;
        if(work.source().source_kind == SourceBuildSourceKind::Aur) {
            if(plan == nullptr) {
                reject_inconsistent_input(
                    "Prepared AUR source work lacks invocation-wide BuildPlan authority.");
            }
            const std::size_t plan_root_index = aur_roots.size();
            aur_roots.push_back(AurRootPackageIdentity{
                work.source().preference_package_name,
                work.source().resolved_package_base.value()});
            validate_plan_root_identity(
                *plan, plan_root_index,
                work.requested_package_name(),
                work.checkout_package_base(),
                "Prepared AUR source work and BuildPlan root identity do not match.");
            if(work.required_targets().front().desired_reason !=
               DesiredInstallReason::Explicit) {
                reject_inconsistent_input(
                    "Prepared AUR source required target does not belong to its BuildPlan root.");
            }
            if(std::any_of(
                   work.selected_repository_providers().begin(),
                   work.selected_repository_providers().end(),
                   [plan](const ProvidedDependency& provider) {
                       return std::none_of(
                           plan->provided.begin(),
                           plan->provided.end(),
                           [&provider](
                               const BuildPlanProvidedDependency&
                                   selected) {
                               return selected.resolution ==
                                          ProviderResolutionKind::
                                              UserSelected &&
                                      same_provider_identity(
                                          selected.provider,
                                          provider);
                           });
                   })) {
                reject_inconsistent_input(
                    "Prepared AUR source provider does not belong to its invocation BuildPlan.");
            }
            std::vector<const ProvidedDependency*> expected_providers;
            for(const BuildPlanDependencyEdge& edge :
                plan->dependency_edges) {
                if(edge.parent_package_base !=
                       work.checkout_package_base() ||
                   edge.provider_resolution !=
                       ProviderResolutionKind::UserSelected ||
                   !edge.resolved_provider.has_value() ||
                   !std::holds_alternative<RepositoryProviderOrigin>(
                       edge.resolved_provider->origin)) {
                    continue;
                }
                const ProvidedDependency* expected =
                    &edge.resolved_provider.value();
                if(std::none_of(
                       expected_providers.begin(),
                       expected_providers.end(),
                       [expected](const ProvidedDependency* existing) {
                           return same_provider_identity(
                               *existing, *expected);
                       })) {
                    expected_providers.push_back(expected);
                }
            }
            if(expected_providers.size() !=
                   work.selected_repository_providers().size() ||
               std::any_of(
                   expected_providers.begin(), expected_providers.end(),
                   [&work](const ProvidedDependency* expected) {
                       return std::none_of(
                           work.selected_repository_providers()
                               .begin(),
                           work.selected_repository_providers().end(),
                           [expected](
                               const ProvidedDependency& actual) {
                               return same_provider_identity(
                                   *expected, actual);
                           });
                   })) {
                reject_inconsistent_input(
                    "Prepared AUR source work lost selected provider authority.");
            }
        } else if(work.required_targets().front().desired_reason !=
                  DesiredInstallReason::Explicit) {
            reject_inconsistent_input(
                "Prepared repository source work has a non-root artifact target.");
        }
    }

    for(std::size_t index = 0; index < snapshot.registered_sources.size();
        ++index) {
        const RegisteredSourcePreferenceSnapshot& source =
            snapshot.registered_sources[index];
        const bool has_complete_source_identity =
            source.source_kind.has_value() &&
            source.resolved_package_base.has_value() &&
            source.canonical_source_identity_key.has_value();
        if(has_complete_source_identity != observed_sources[index]) {
            reject_inconsistent_input(
                "Prepared source snapshot and work items do not match.");
        }
    }

    if(aur_roots.empty()) {
        if(plan != nullptr) {
            reject_inconsistent_input(
                "Prepared repository source work retained an AUR BuildPlan.");
        }
        return repository_order;
    }
    if(plan == nullptr) {
        reject_inconsistent_input(
            "Prepared AUR source work lacks invocation-wide BuildPlan authority.");
    }
    validate_plan_roots(
        *plan, aur_roots,
        "Prepared AUR source BuildPlan correlation is inconsistent.");
    return repository_order;
}

void append_system_source_roots(
    const SystemSourceUpgradeProjectionAuthority& prepared,
    UnifiedPlanObservationInput& observation) {
    const BuildPlan* plan = prepared.aur_invocation_plan();
    std::size_t aur_root_index = 0;
    for(const PreparedSystemSourceWorkReference& work :
        prepared.source_work_items()) {
        const RegisteredSourcePreferenceSnapshot& source = work.source();
        RootTargetIdentity correlation{
            source.original_preference_index,
            source.preference_package_name};

        UnifiedPlanRootIdentity identity;
        UnifiedPlanRootRouteKind route;
        if(source.source_kind.value() == SourceBuildSourceKind::Aur) {
            correlation = plan->root_targets[aur_root_index++];
            identity = AurRootPackageIdentity{
                source.preference_package_name,
                source.resolved_package_base.value()};
            route = UnifiedPlanRootRouteKind::AurSourceBuild;
        } else {
            identity = RepositorySourceBuildRootIdentity{
                source.preference_package_name,
                source.resolved_package_base.value(),
                source.canonical_source_identity_key.value()};
            route = UnifiedPlanRootRouteKind::RepositorySourceBuild;
        }
        observation.roots.emplace_back(
            std::move(correlation), std::move(identity), route);
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RegisteredSourcePreferenceSnapshot>(source));
    }
}

bool same_source_build_environment(
    const std::optional<SourceBuildEnvironment>& lhs,
    const std::optional<SourceBuildEnvironment>& rhs) noexcept {
    if(lhs.has_value() != rhs.has_value()) return false;
    if(!lhs.has_value()) return true;
    if(lhs->ordered_assignments.size() !=
       rhs->ordered_assignments.size()) {
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

bool same_registered_source_identity(
    const RegisteredSourcePreferenceSnapshot& lhs,
    const RegisteredSourcePreferenceSnapshot& rhs) noexcept {
    return lhs.original_preference_index == rhs.original_preference_index &&
           lhs.preference_package_name == rhs.preference_package_name &&
           lhs.entry_path == rhs.entry_path &&
           same_source_build_environment(lhs.environment, rhs.environment) &&
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

bool same_system_source_options(
    const SystemSourceUpgradeOptionSnapshot& lhs,
    const SystemSourceUpgradeOptionSnapshot& rhs) noexcept {
    return lhs.no_edit == rhs.no_edit && lhs.no_diff == rhs.no_diff &&
           lhs.no_confirm == rhs.no_confirm &&
           lhs.rebuild == rhs.rebuild &&
           lhs.clean_build == rhs.clean_build &&
           lhs.rm_deps == rhs.rm_deps && lhs.editor == rhs.editor &&
           lhs.needed == rhs.needed;
}

void validate_upgrade_all_snapshot_identity(
    const SystemSourceUpgradePreparedSnapshot& outer,
    const SystemSourceUpgradePreparedSnapshot& nested) {
    if(outer.preference_root_exists != nested.preference_root_exists ||
       !same_system_source_options(outer.options, nested.options) ||
       outer.registered_sources.size() !=
           nested.registered_sources.size() ||
       !std::equal(
           outer.registered_sources.begin(),
           outer.registered_sources.end(),
           nested.registered_sources.begin(),
           same_registered_source_identity)) {
        reject_inconsistent_input(
            "upgrade-all snapshot does not belong to its nested system/source authority.");
    }
}

void validate_upgrade_all_authority(
    const UpgradeAllOperationProjectionAuthority& prepared) {
    validate_upgrade_all_snapshot_identity(
        prepared.snapshot().system_source,
        prepared.system_source().snapshot());
}

void append_system_source_issues(
    const std::vector<SystemSourceUpgradeIssue>& issues,
    UnifiedPlanObservationInput& observation) {
    for(const SystemSourceUpgradeIssue& issue : issues) {
        if(issue.impact != SystemSourceUpgradeIssueImpact::BlocksExecution) {
            continue;
        }
        observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(issue)});
    }
}

void append_aur_update_roots_and_blockers(
    const AurUpdateQueryResult& query_result,
    const AurUpdateExecutionPreflight& preflight,
    UnifiedPlanObservationInput& observation) {
    for(const AurUpdateQueryFailure& failure :
        query_result.recoverable_failures) {
        observation.blockers.push_back(SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<AurUpdateQueryFailure>(
                failure)});
    }
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(target.update.aur_package.has_value()) {
            const AurUpdateRemotePackage& package =
                target.update.aur_package.value();
            RootTargetIdentity correlation{
                target.update_plan_index, target.update.installed_name};
            if(target.build_plan_root_index.has_value()) {
                correlation = preflight.build_plan->root_targets[target.build_plan_root_index.value()];
            }
            observation.roots.emplace_back(
                std::move(correlation),
                AurRootPackageIdentity{
                    package.aur_name, package.package_base},
                UnifiedPlanRootRouteKind::AurSourceBuild);
            observation.root_metadata.push_back(
                UnifiedPlanBorrowedAuthorityReference<AurUpdatePlanEntry>(
                    target.update));
        }

        if(target.status != AurUpdateExecutionTargetStatus::Unsupported &&
           target.status != AurUpdateExecutionTargetStatus::Incomplete) {
            continue;
        }
        for(const AurUpdateExecutionIssue& issue : target.issues) {
            observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    AurUpdateExecutionIssue>(issue)});
        }
    }
}

bool same_build_plan_projection_issue_identity(
    const BuildPlanArtifactTargetProjectionIssue& lhs,
    const BuildPlanArtifactTargetProjectionIssue& rhs) noexcept {
    return lhs.kind == rhs.kind &&
           lhs.build_plan_order_index == rhs.build_plan_order_index &&
           lhs.entry_package_name_index == rhs.entry_package_name_index &&
           lhs.package_target_indices == rhs.package_target_indices &&
           lhs.package_base == rhs.package_base &&
           lhs.package_name == rhs.package_name && lhs.roots == rhs.roots;
}

bool is_known_wrapped_preflight_issue_reason(
    AurUpdateExecutionReason reason) noexcept {
    switch(reason) {
        case AurUpdateExecutionReason::None:
            return false;
        case AurUpdateExecutionReason::UpToDate:
        case AurUpdateExecutionReason::DevelRequiresCheck:
        case AurUpdateExecutionReason::DevelObservationUnknown:
        case AurUpdateExecutionReason::DevelUnsupported:
        case AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck:
        case AurUpdateExecutionReason::NonAurForeign:
        case AurUpdateExecutionReason::AurMetadataUnavailable:
        case AurUpdateExecutionReason::VersionComparisonUnavailable:
        case AurUpdateExecutionReason::InstalledReasonUnknown:
        case AurUpdateExecutionReason::UpdatePlanInconsistent:
        case AurUpdateExecutionReason::DuplicateUpdateTarget:
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
        case AurUpdateExecutionReason::InstalledPackageMetadataUnavailable:
            return true;
    }
    return false;
}

bool is_known_build_plan_projection_issue_kind(
    BuildPlanArtifactTargetProjectionIssueKind kind) noexcept {
    switch(kind) {
        case BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageBase:
        case BuildPlanArtifactTargetProjectionIssueKind::EmptyEntryPackageNames:
        case BuildPlanArtifactTargetProjectionIssueKind::InvalidPackageName:
        case BuildPlanArtifactTargetProjectionIssueKind::DuplicateEntryPackageName:
        case BuildPlanArtifactTargetProjectionIssueKind::
            MissingPlannedPackageTarget:
        case BuildPlanArtifactTargetProjectionIssueKind::
            DuplicatePlannedPackageTarget:
        case BuildPlanArtifactTargetProjectionIssueKind::PackageBaseMismatch:
        case BuildPlanArtifactTargetProjectionIssueKind::
            UncoveredPlannedPackageTarget:
        case BuildPlanArtifactTargetProjectionIssueKind::
            DesiredInstallReasonUnavailable:
        case BuildPlanArtifactTargetProjectionIssueKind::
            RootAttributionInconsistent:
        case BuildPlanArtifactTargetProjectionIssueKind::
            DuplicatePackageBaseEntry:
            return true;
    }
    return false;
}

bool has_known_build_plan_projection_issue_relation(
    const AurUpdateExecutionIssue& issue) noexcept {
    if(!issue.build_plan_projection_issue.has_value()) return true;

    const BuildPlanArtifactTargetProjectionIssueKind kind =
        issue.build_plan_projection_issue->kind;
    if(!is_known_build_plan_projection_issue_kind(kind)) return false;
    return issue.reason ==
           (kind == BuildPlanArtifactTargetProjectionIssueKind::
                        PackageBaseMismatch
                ? AurUpdateExecutionReason::PackageBaseMismatch
                : AurUpdateExecutionReason::BuildPlanInconsistent);
}

bool same_preflight_issue_identity(
    const AurUpdateExecutionIssue& lhs,
    const AurUpdateExecutionIssue& rhs) noexcept {
    if(!is_known_wrapped_preflight_issue_reason(lhs.reason) ||
       !is_known_wrapped_preflight_issue_reason(rhs.reason) ||
       !has_known_build_plan_projection_issue_relation(lhs) ||
       !has_known_build_plan_projection_issue_relation(rhs) ||
       lhs.reason != rhs.reason || lhs.package_name != rhs.package_name ||
       lhs.package_base != rhs.package_base ||
       lhs.dependency_specification != rhs.dependency_specification ||
       lhs.devel_requires_check_reason !=
           rhs.devel_requires_check_reason ||
       lhs.relation_reason != rhs.relation_reason ||
       lhs.required_devel_target_blocker !=
           rhs.required_devel_target_blocker ||
       lhs.build_plan_projection_issue.has_value() !=
           rhs.build_plan_projection_issue.has_value()) {
        return false;
    }
    return !lhs.build_plan_projection_issue.has_value() ||
           same_build_plan_projection_issue_identity(
               lhs.build_plan_projection_issue.value(),
               rhs.build_plan_projection_issue.value());
}

bool has_exact_preflight_issue_target_correlation(
    const AurUpdateExecutionIssue& issue,
    const AurUpdateExecutionTarget& target,
    const AurUpdateExecutionPreflight& preflight) noexcept {
    if(!issue.build_plan_projection_issue.has_value()) return true;
    if(!preflight.build_plan.has_value() ||
       !target.build_plan_root_index.has_value() ||
       target.build_plan_root_index.value() >=
           preflight.build_plan->root_targets.size()) {
        return false;
    }

    const BuildPlan& plan = preflight.build_plan.value();
    const std::vector<RootTargetIdentity>& roots =
        issue.build_plan_projection_issue->roots;
    const RootTargetIdentity& target_root = plan.root_targets[target.build_plan_root_index.value()];
    auto target_retains_same_issue =
        [&issue](const AurUpdateExecutionTarget& candidate) {
            return (candidate.status ==
                        AurUpdateExecutionTargetStatus::Unsupported ||
                    candidate.status ==
                        AurUpdateExecutionTargetStatus::Incomplete) &&
                   std::any_of(
                       candidate.issues.begin(), candidate.issues.end(),
                       [&issue](const AurUpdateExecutionIssue& original) {
                           return same_preflight_issue_identity(
                               original, issue);
                       });
        };

    if(roots.empty()) {
        return std::all_of(
            preflight.targets.begin(), preflight.targets.end(),
            [&target_retains_same_issue](
                const AurUpdateExecutionTarget& candidate) {
                return !candidate.build_plan_root_index.has_value() ||
                       target_retains_same_issue(candidate);
            });
    }
    if(std::count(roots.begin(), roots.end(), target_root) != 1) return false;

    for(const RootTargetIdentity& root : roots) {
        if(std::count(roots.begin(), roots.end(), root) != 1) return false;

        std::optional<std::size_t> root_index;
        for(std::size_t index = 0; index < plan.root_targets.size(); ++index) {
            if(plan.root_targets[index] != root) continue;
            if(root_index.has_value()) return false;
            root_index = index;
        }
        if(!root_index.has_value()) return false;

        const std::size_t correlated_target_count =
            static_cast<std::size_t>(std::count_if(
                preflight.targets.begin(), preflight.targets.end(),
                [root_index, &target_retains_same_issue](
                    const AurUpdateExecutionTarget& candidate) {
                    return candidate.build_plan_root_index ==
                               root_index &&
                           target_retains_same_issue(candidate);
                }));
        if(correlated_target_count != 1) return false;
    }
    return true;
}

bool is_already_projected_blocking_preflight_wrapper(
    const AurUpdatePreparationIssue& issue,
    const AurUpdateExecutionPreflight& preflight) noexcept {
    if(issue.reason != AurUpdatePreparationReason::BlockingPreflight ||
       !issue.preflight_issue.has_value() ||
       issue.affected_update_plan_indices.size() != 1 ||
       !issue.affected_roots.empty() ||
       issue.source_preference_failure.has_value() ||
       issue.package_metadata_failure.has_value() ||
       issue.build_plan_projection_issue.has_value() ||
       issue.package_name != issue.preflight_issue->package_name ||
       issue.package_base != issue.preflight_issue->package_base) {
        return false;
    }

    const std::size_t update_plan_index =
        issue.affected_update_plan_indices.front();
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        if(target.update_plan_index != update_plan_index ||
           (target.status != AurUpdateExecutionTargetStatus::Unsupported &&
            target.status != AurUpdateExecutionTargetStatus::Incomplete)) {
            continue;
        }
        if(!has_exact_preflight_issue_target_correlation(
               issue.preflight_issue.value(), target, preflight)) {
            return false;
        }
        if(std::any_of(
               target.issues.begin(), target.issues.end(),
               [&issue](const AurUpdateExecutionIssue& original) {
                   return same_preflight_issue_identity(
                       original, issue.preflight_issue.value());
               })) {
            return true;
        }
    }
    return false;
}

void append_aur_update_preparation_blockers(
    const AurUpdateSourceBuildPreparation& preparation,
    const AurUpdateExecutionPreflight& preflight,
    UnifiedPlanObservationInput& observation) {
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            AurUpdateSourceBuildPreparation>(preparation));
    for(const AurUpdatePreparationIssue& issue : preparation.issues) {
        if(is_already_projected_blocking_preflight_wrapper(
               issue, preflight)) {
            continue;
        }
        observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdatePreparationIssue>(issue)});
    }
}

void append_aur_update_observation_blockers(
    const AurUpdateSourceBuildObservation& preparation,
    const AurUpdateExecutionPreflight& preflight,
    UnifiedPlanObservationInput& observation) {
    for(const AurUpdatePreparationIssue& issue : preparation.issues) {
        if(is_already_projected_blocking_preflight_wrapper(
               issue, preflight)) {
            continue;
        }
        observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdatePreparationIssue>(issue)});
    }
}

void append_phase(
    UnifiedPlanObservationInput& observation,
    UnifiedPlanObservationPhase phase,
    UnifiedPlanAuthorityOwner owner,
    std::optional<ExistingRoutePhaseReference> existing = std::nullopt) {
    observation.phases.push_back(UnifiedPlanPhaseReference{
        phase, owner, std::move(existing)});
}

void append_local_metadata_evaluation_blocker(
    const LocalSourceRoot& source_root,
    UnifiedPlanObservationInput& observation) {
    const LocalSourceMetadataSnapshot& metadata = source_root.metadata();
    if(metadata.state() == LocalSourceMetadataState::UsableUnverified) {
        reject_inconsistent_input(
            "Usable local metadata was projected as evaluation-required.");
    }
    observation.status = UnifiedPlanObservationStatus::Blocked;
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<LocalSourceRoot>(
            source_root));
    if(metadata.state() == LocalSourceMetadataState::Unsafe) {
        if(metadata.unsafe_failure() == nullptr) {
            reject_inconsistent_input(
                "Unsafe local metadata has no typed source failure.");
        }
        observation.blockers.push_back(SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                LocalSourceRootFailure>(*metadata.unsafe_failure())});
    } else {
        observation.blockers.push_back(
            LocalSourceMetadataEvaluationUnifiedPlanBlocker{
                LocalSourceRootObservationIdentity{
                    source_root.canonical_path(),
                    source_root.directory_identity()},
                UnifiedPlanBorrowedAuthorityReference<
                    LocalSourceMetadataSnapshot>(metadata)});
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        observation, UnifiedPlanObservationPhase::MetadataDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
}

bool has_repository_transaction(
    const UnifiedPlanObservationInput& observation) noexcept {
    return std::any_of(
        observation.transaction_intents.begin(),
        observation.transaction_intents.end(),
        [](const UnifiedPlanTransactionIntent& transaction) {
            return std::holds_alternative<
                RepositoryPackageTransactionIntent>(transaction);
        });
}

bool has_source_transaction(
    const UnifiedPlanObservationInput& observation) noexcept {
    return std::any_of(
        observation.transaction_intents.begin(),
        observation.transaction_intents.end(),
        [](const UnifiedPlanTransactionIntent& transaction) {
            return std::holds_alternative<
                SourceBuiltArtifactInstallBoundaryIntent>(transaction);
        });
}

void append_source_mutation_phases(
    UnifiedPlanObservationInput& observation,
    std::optional<ExistingRoutePhaseReference> existing = std::nullopt,
    bool include_retrieval = true) {
    if(include_retrieval) {
        append_phase(
            observation, UnifiedPlanObservationPhase::SourceRetrieval,
            UnifiedPlanAuthorityOwner::Git, existing);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::SourceBuild,
        UnifiedPlanAuthorityOwner::Makepkg, existing);
    append_phase(
        observation, UnifiedPlanObservationPhase::ArtifactValidation,
        UnifiedPlanAuthorityOwner::Moguet, existing);
    append_phase(
        observation, UnifiedPlanObservationPhase::SourceArtifactInstall,
        UnifiedPlanAuthorityOwner::Pacman, existing);
}

void append_root_phases(
    const RootPackageRoutingProjection& routing,
    const BuildPlan* plan,
    UnifiedPlanObservationInput& observation) {
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    if(!routing.repository_targets().empty()) {
        append_phase(
            observation, UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Libalpm);
    }
    if(!routing.aur_targets().empty()) {
        append_phase(
            observation, UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc);
    }
    if(plan != nullptr) {
        append_phase(
            observation, UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    if(has_repository_transaction(observation)) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RepositoryTransaction,
            UnifiedPlanAuthorityOwner::Pacman);
    }
    if(has_source_transaction(observation)) {
        append_source_mutation_phases(observation);
    }
}

void append_local_phases(
    const BuildPlan& plan,
    UnifiedPlanObservationInput& observation) {
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        observation, UnifiedPlanObservationPhase::MetadataDiscovery,
        UnifiedPlanAuthorityOwner::Makepkg);
    if(!plan.dependency_edges.empty() || !plan.provided.empty()) {
        append_phase(
            observation, UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    if(has_repository_transaction(observation)) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RepositoryTransaction,
            UnifiedPlanAuthorityOwner::Pacman);
    }
    if(!has_source_transaction(observation)) return;
    const bool has_remote_source = std::any_of(
        observation.build_units.begin(),
        observation.build_units.end(),
        [](const UnifiedPlanBuildUnitReference& build_unit) {
            return std::holds_alternative<
                AurPackageBaseBuildUnitReference>(build_unit);
        });
    append_source_mutation_phases(observation, std::nullopt, has_remote_source);
}

void append_aur_update_phases(
    const AurUpdateQueryResult& query,
    const BuildPlan* plan,
    UnifiedPlanObservationInput& observation) {
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    if(!query.plan.entries.empty() || !query.recoverable_failures.empty()) {
        append_phase(
            observation, UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc);
    }
    if(plan != nullptr) {
        append_phase(
            observation, UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    if(has_repository_transaction(observation)) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RepositoryTransaction,
            UnifiedPlanAuthorityOwner::Pacman);
    }
    if(has_source_transaction(observation)) {
        append_source_mutation_phases(observation);
    }
}

void append_system_source_phases(
    const SystemSourceUpgradeProjectionAuthority& prepared,
    UnifiedPlanObservationInput& observation) {
    const ExistingRoutePhaseReference preparation{
        SystemSourceUpgradePhase::Preparation};
    const ExistingRoutePhaseReference system{
        SystemSourceUpgradePhase::System};
    const ExistingRoutePhaseReference source{
        SystemSourceUpgradePhase::RegisteredSource};
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
    append_phase(
        observation, UnifiedPlanObservationPhase::MetadataDiscovery,
        UnifiedPlanAuthorityOwner::Libalpm, preparation);
    if(prepared.aur_invocation_plan() != nullptr) {
        append_phase(
            observation, UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc, preparation);
        append_phase(
            observation, UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet, preparation);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
    if(has_repository_transaction(observation)) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RepositoryTransaction,
            UnifiedPlanAuthorityOwner::Pacman, system);
    }
    if(has_source_transaction(observation)) {
        append_source_mutation_phases(observation, source);
    }
}

void append_upgrade_all_phases(
    const UpgradeAllOperationProjectionAuthority& prepared,
    const AurUpdateQueryResult& query,
    const BuildPlan* aur_plan,
    bool has_registered_source_artifacts,
    bool has_aur_update_artifacts,
    UnifiedPlanObservationInput& observation) {
    const ExistingRoutePhaseReference preparation{
        UpgradeAllOperationPhase::Preparation};
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
    append_phase(
        observation, UnifiedPlanObservationPhase::MetadataDiscovery,
        UnifiedPlanAuthorityOwner::Libalpm,
        ExistingRoutePhaseReference{
            UpgradeAllOperationPhase::ForeignInventory});
    if(prepared.system_source().aur_invocation_plan() != nullptr ||
       aur_plan != nullptr || !query.plan.entries.empty() ||
       !query.recoverable_failures.empty()) {
        append_phase(
            observation, UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurQuery});
    }
    if(prepared.system_source().aur_invocation_plan() != nullptr ||
       aur_plan != nullptr) {
        append_phase(
            observation, UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurPreparation});
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
    if(has_repository_transaction(observation)) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RepositoryTransaction,
            UnifiedPlanAuthorityOwner::Pacman,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::System});
    }
    const ExistingRoutePhaseReference registered_source{
        UpgradeAllOperationPhase::RegisteredSource};
    const ExistingRoutePhaseReference aur_execution{
        UpgradeAllOperationPhase::AurExecution};
    auto append_source_phase =
        [&observation, has_registered_source_artifacts,
         has_aur_update_artifacts, &registered_source, &aur_execution](
            UnifiedPlanObservationPhase phase,
            UnifiedPlanAuthorityOwner owner) {
            if(has_registered_source_artifacts) {
                append_phase(
                    observation, phase, owner, registered_source);
            }
            if(has_aur_update_artifacts) {
                append_phase(observation, phase, owner, aur_execution);
            }
        };
    append_source_phase(
        UnifiedPlanObservationPhase::SourceRetrieval,
        UnifiedPlanAuthorityOwner::Git);
    append_source_phase(
        UnifiedPlanObservationPhase::SourceBuild,
        UnifiedPlanAuthorityOwner::Makepkg);
    append_source_phase(
        UnifiedPlanObservationPhase::ArtifactValidation,
        UnifiedPlanAuthorityOwner::Moguet);
    append_source_phase(
        UnifiedPlanObservationPhase::SourceArtifactInstall,
        UnifiedPlanAuthorityOwner::Pacman);
}

void append_blocked_root_preparation(
    const RootPackageInstallPreparationFailure& failure,
    UnifiedPlanObservationInput& observation) {
    if(failure.details.empty()) {
        reject_inconsistent_input(
            "Blocked root preparation has no typed failure detail.");
    }
    observation.status = UnifiedPlanObservationStatus::Blocked;
    observation.blockers.push_back(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(failure)});

    const BuildPlan* plan = failure.aur_build_plan.has_value()
                                ? &failure.aur_build_plan.value()
                                : nullptr;
    const std::vector<std::string>* plan_repository_order = plan == nullptr
                                                                ? nullptr
                                                                : validate_build_plan_repository_authority(*plan);
    if(plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *plan));
        append_build_plan_blockers(*plan, observation);
    }

    if(failure.discovery_snapshot.has_value() &&
       failure.discovery_snapshot->repository_order.has_value()) {
        const auto& discovery_order =
            failure.discovery_snapshot->repository_order.value();
        if(plan_repository_order != nullptr &&
           *plan_repository_order != discovery_order) {
            reject_inconsistent_input(
                "Blocked root discovery and BuildPlan used different repository configurations.");
        }
        observation.configured_repository_order.emplace(
            std::cref(discovery_order));
    }

    if(!failure.routing_projection.has_value()) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RequestDiscovery,
            UnifiedPlanAuthorityOwner::Moguet);
        return;
    }
    if(!failure.discovery_snapshot.has_value()) {
        reject_inconsistent_input(
            "Blocked root route lacks its discovery authority.");
    }

    const RootPackageSearchSnapshot& discovery =
        failure.discovery_snapshot.value();
    const RootPackageRoutingProjection& routing =
        failure.routing_projection.value();
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            RootPackageRoutingProjection>(routing));

    std::vector<AurRootPackageIdentity> aur_roots;
    for(const RepositoryRootPackageRouteTarget& target :
        routing.repository_targets()) {
        const RootPackageSearchCandidate* metadata =
            find_root_metadata(discovery, target.selected_target());
        if(metadata == nullptr) {
            reject_inconsistent_input(
                "Blocked repository root lost discovery correlation.");
        }
        observation.roots.emplace_back(
            RootTargetIdentity{
                target.selection_index(),
                target.identity().package_name},
            target.identity(),
            UnifiedPlanRootRouteKind::RepositoryTransaction);
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageSearchCandidate>(*metadata));
    }
    for(const AurRootPackageRouteTarget& target : routing.aur_targets()) {
        const RootPackageSearchCandidate* metadata =
            find_root_metadata(discovery, target.selected_target());
        if(metadata == nullptr) {
            reject_inconsistent_input(
                "Blocked AUR root lost discovery correlation.");
        }
        aur_roots.push_back(target.identity());
        const std::size_t aur_index = aur_roots.size() - 1;
        const RootTargetIdentity correlation =
            plan != nullptr && aur_index < plan->root_targets.size()
                ? plan->root_targets[aur_index]
                : RootTargetIdentity{
                      target.selection_index(),
                      target.identity().package_name};
        observation.roots.emplace_back(
            correlation, target.identity(),
            UnifiedPlanRootRouteKind::AurSourceBuild);
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageSearchCandidate>(*metadata));
    }
    if(plan != nullptr) {
        validate_blocked_plan_roots(
            *plan, aur_roots,
            "Blocked root BuildPlan identity is inconsistent.");
    }
    append_root_phases(routing, plan, observation);
}

bool is_preparation_only_source_result(
    const RegisteredSourceUpgradeResult& result) noexcept {
    return result.status == RegisteredSourceUpgradeStatus::NotAttempted ||
           result.status == RegisteredSourceUpgradeStatus::Incomplete ||
           result.status == RegisteredSourceUpgradeStatus::Unsupported;
}

const std::vector<std::string>* validate_blocked_system_source_result(
    const SystemSourceUpgradeResult& result,
    bool require_blocking_issue = true) {
    const bool has_blocking_issue = std::any_of(
        result.issues.begin(), result.issues.end(),
        [](const SystemSourceUpgradeIssue& issue) {
            return issue.impact ==
                   SystemSourceUpgradeIssueImpact::BlocksExecution;
        });
    if(result.status != SystemSourceUpgradeStatus::BlockedBeforeMutation ||
       result.stopped_phase != SystemSourceUpgradePhase::Preparation ||
       (require_blocking_issue && !has_blocking_issue) ||
       result.system.status != SystemUpgradePhaseStatus::NotAttempted ||
       result.system.package_state_change != PackageStateChange::NoChange ||
       result.system.command_exit_status.has_value() ||
       result.system.after_snapshot_failure.has_value() ||
       result.selected_repository_provider_transaction.status !=
           SelectedRepositoryProviderTransactionStatus::NotRequired ||
       !result.selected_repository_provider_transaction.selected_providers
            .empty() ||
       result.selected_repository_provider_transaction.package_state_change !=
           PackageStateChange::NoChange ||
       result.selected_repository_provider_transaction.command_exit_status
           .has_value() ||
       result.selected_repository_provider_transaction.diagnostic.has_value() ||
       result.registered_source_results.size() !=
           result.prepared_snapshot.registered_sources.size() ||
       std::any_of(
           result.registered_source_results.begin(),
           result.registered_source_results.end(),
           [](const RegisteredSourceUpgradeResult& source) {
               return !is_preparation_only_source_result(source) ||
                      source.package_state_change !=
                          PackageStateChange::NoChange ||
                      source.cleanup_diagnostic.has_value();
           })) {
        reject_inconsistent_input(
            "System/source Blocked result is not a pre-mutation production result.");
    }

    for(std::size_t index = 0;
        index < result.prepared_snapshot.registered_sources.size(); ++index) {
        const RegisteredSourcePreferenceSnapshot& prepared =
            result.prepared_snapshot.registered_sources[index];
        const RegisteredSourceUpgradeResult& observed =
            result.registered_source_results[index];
        if(observed.original_preference_index !=
               prepared.original_preference_index ||
           observed.preference_package_name !=
               prepared.preference_package_name ||
           observed.canonical_source_identity_key !=
               prepared.canonical_source_identity_key ||
           observed.resolved_package_base !=
               prepared.resolved_package_base) {
            reject_inconsistent_input(
                "System/source Blocked result mixed registered-source invocation identity.");
        }
    }

    for(const SystemSourceUpgradeIssue& issue : result.issues) {
        const bool has_source_attribution =
            issue.original_preference_index.has_value() ||
            issue.preference_package_name.has_value() ||
            issue.resolved_package_base.has_value();
        if(!has_source_attribution) {
            continue;
        }
        if(!issue.original_preference_index.has_value() ||
           !issue.preference_package_name.has_value()) {
            reject_inconsistent_input(
                "System/source Blocked issue has incomplete invocation attribution.");
        }
        const std::size_t matches = static_cast<std::size_t>(std::count_if(
            result.prepared_snapshot.registered_sources.begin(),
            result.prepared_snapshot.registered_sources.end(),
            [&issue](const RegisteredSourcePreferenceSnapshot& source) {
                return issue.original_preference_index.value() ==
                           source.original_preference_index &&
                       issue.preference_package_name.value() ==
                           source.preference_package_name &&
                       issue.resolved_package_base ==
                           source.resolved_package_base;
            }));
        if(matches != 1) {
            reject_inconsistent_input(
                "System/source Blocked issue does not belong to its prepared invocation.");
        }
    }
    return result.aur_invocation_plan.has_value()
               ? validate_build_plan_repository_authority(
                     result.aur_invocation_plan.value())
               : nullptr;
}

void append_blocked_system_source_roots(
    const SystemSourceUpgradeResult& result,
    UnifiedPlanObservationInput& observation) {
    const BuildPlan* plan = result.aur_invocation_plan.has_value()
                                ? &result.aur_invocation_plan.value()
                                : nullptr;
    std::vector<AurRootPackageIdentity> aur_roots;
    for(const RegisteredSourcePreferenceSnapshot& source :
        result.prepared_snapshot.registered_sources) {
        if(!source.source_kind.has_value() ||
           !source.resolved_package_base.has_value() ||
           source.resolved_package_base->empty() ||
           !source.canonical_source_identity_key.has_value() ||
           source.canonical_source_identity_key->empty() ||
           source.preference_package_name.empty()) {
            continue;
        }

        RootTargetIdentity correlation{
            source.original_preference_index,
            source.preference_package_name};
        UnifiedPlanRootIdentity identity;
        UnifiedPlanRootRouteKind route;
        if(source.source_kind.value() == SourceBuildSourceKind::Aur) {
            aur_roots.push_back(AurRootPackageIdentity{
                source.preference_package_name,
                source.resolved_package_base.value()});
            const std::size_t aur_index = aur_roots.size() - 1;
            if(plan != nullptr && aur_index < plan->root_targets.size()) {
                correlation = plan->root_targets[aur_index];
            }
            identity = aur_roots.back();
            route = UnifiedPlanRootRouteKind::AurSourceBuild;
        } else {
            identity = RepositorySourceBuildRootIdentity{
                source.preference_package_name,
                source.resolved_package_base.value(),
                source.canonical_source_identity_key.value()};
            route = UnifiedPlanRootRouteKind::RepositorySourceBuild;
        }
        observation.roots.emplace_back(
            std::move(correlation), std::move(identity), route);
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RegisteredSourcePreferenceSnapshot>(source));
    }
    if(plan != nullptr) {
        validate_blocked_plan_roots(
            *plan, aur_roots,
            "Blocked system/source BuildPlan identity is inconsistent.");
    }
}

void append_blocked_system_source(
    const SystemSourceUpgradeResult& result,
    UnifiedPlanObservationInput& observation,
    bool append_route_authority = true) {
    const std::vector<std::string>* repository_order =
        validate_blocked_system_source_result(result);
    observation.status = UnifiedPlanObservationStatus::Blocked;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    if(append_route_authority) {
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeResult>(result));
    }
    append_blocked_system_source_roots(result, observation);
    append_system_source_issues(result.issues, observation);
    if(result.aur_invocation_plan.has_value()) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                result.aur_invocation_plan.value()));
        append_build_plan_blockers(
            result.aur_invocation_plan.value(), observation);
    }

    const ExistingRoutePhaseReference preparation{
        SystemSourceUpgradePhase::Preparation};
    append_phase(
        observation,
        UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
    append_phase(
        observation,
        UnifiedPlanObservationPhase::MetadataDiscovery,
        UnifiedPlanAuthorityOwner::Libalpm, preparation);
    const bool has_aur_source = std::any_of(
        result.prepared_snapshot.registered_sources.begin(),
        result.prepared_snapshot.registered_sources.end(),
        [](const RegisteredSourcePreferenceSnapshot& source) {
            return source.source_kind == SourceBuildSourceKind::Aur;
        });
    if(has_aur_source || result.aur_invocation_plan.has_value()) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc, preparation);
    }
    if(result.aur_invocation_plan.has_value()) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet, preparation);
    }
    append_phase(
        observation,
        UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
}

void append_blocked_upgrade_all(
    const UpgradeAllOperationResult& result,
    UnifiedPlanObservationInput& observation) {
    if(result.status != UpgradeAllOperationStatus::BlockedBeforeMutation ||
       result.stopped_phase != UpgradeAllOperationPhase::Preparation ||
       result.foreign_inventory.status !=
           UpgradeAllForeignInventoryPhaseStatus::NotAttempted ||
       result.foreign_inventory.repository_configuration.has_value() ||
       result.aur.status != UpgradeAllAurPhaseStatus::NotAttempted ||
       result.aur.operation_result.has_value() ||
       !result.duplicate_excluded_aur_targets.empty() ||
       !result.externally_satisfied_aur_build_units.empty()) {
        reject_inconsistent_input(
            "upgrade-all Blocked result is not a preparation result.");
    }
    validate_upgrade_all_snapshot_identity(
        result.prepared_snapshot.system_source,
        result.system_source.prepared_snapshot);
    const std::vector<std::string>* repository_order =
        validate_blocked_system_source_result(
            result.system_source, false);

    observation.status = UnifiedPlanObservationStatus::Blocked;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            UpgradeAllOperationResult>(result));
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            SystemSourceUpgradeResult>(result.system_source));
    append_blocked_system_source_roots(result.system_source, observation);
    append_system_source_issues(result.system_source.issues, observation);
    for(const UpgradeAllOperationIssue& issue : result.issues) {
        observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(issue)});
    }
    if(result.system_source.aur_invocation_plan.has_value()) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                result.system_source.aur_invocation_plan.value()));
        append_build_plan_blockers(
            result.system_source.aur_invocation_plan.value(),
            observation);
    }
    if(observation.blockers.empty()) {
        reject_inconsistent_input(
            "Blocked upgrade-all result has no typed blocker.");
    }

    const ExistingRoutePhaseReference preparation{
        UpgradeAllOperationPhase::Preparation};
    append_phase(
        observation,
        UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
    append_phase(
        observation,
        UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet, preparation);
}

} // namespace

std::unique_ptr<UnifiedPlanProjection> UnifiedPlanProjection::make(
    std::vector<BuildPlanArtifactTargetProjectionResult>
        artifact_target_projections,
    UnifiedPlanObservationInput observation_input) {
    return make(
        std::move(artifact_target_projections), {},
        std::move(observation_input));
}

std::unique_ptr<UnifiedPlanProjection> UnifiedPlanProjection::make(
    std::vector<BuildPlanArtifactTargetProjectionResult>
        artifact_target_projections,
    std::vector<ProjectedBuildPlanArtifactTargets>
        route_artifact_targets,
    UnifiedPlanObservationInput observation_input) {
    // std::vector move transfers its storage; references created after the
    // source vectors reached final size continue to name the bundle elements.
    auto projection = std::unique_ptr<UnifiedPlanProjection>(
        new UnifiedPlanProjection(
            std::move(artifact_target_projections),
            std::move(route_artifact_targets)));
    projection->observation_result_.emplace(
        make_unified_plan_observation(std::move(observation_input)));
    return projection;
}

std::unique_ptr<UnifiedPlanProjection> project_root_package_unified_plan(
    RootPackageUnifiedPlanProjectionInput input) {
    if(const auto* blocked = std::get_if<std::reference_wrapper<
           const RootPackageInstallPreparationFailure>>(&input.source);
       blocked != nullptr) {
        UnifiedPlanObservationInput observation;
        append_blocked_root_preparation(blocked->get(), observation);
        return UnifiedPlanProjection::make({}, std::move(observation));
    }
    const PreparedRootPackageInstall& prepared =
        std::get<std::reference_wrapper<
            const PreparedRootPackageInstall>>(input.source)
            .get();
    validate_root_package_prepared(prepared);
    const RootPackageSearchSnapshot& discovery =
        prepared.discovery_snapshot.value();
    const RootPackageRoutingProjection& routing =
        prepared.routing_projection.value();
    const BuildPlan* plan = prepared.aur_build_plan.has_value()
                                ? &prepared.aur_build_plan.value()
                                : nullptr;

    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    const std::vector<std::string>* plan_repository_order = nullptr;
    if(plan != nullptr) {
        projections.reserve(1);
        projections.push_back(
            project_build_plan_required_artifact_targets(*plan));
        plan_repository_order =
            validate_root_source_work(prepared, projections.front());
    }

    UnifiedPlanObservationInput observation;
    if(discovery.repository_order.has_value()) {
        if(plan_repository_order != nullptr &&
           *plan_repository_order != discovery.repository_order.value()) {
            reject_inconsistent_input(
                "Root discovery and BuildPlan used different repository configurations.");
        }
        observation.configured_repository_order.emplace(
            std::cref(discovery.repository_order.value()));
    }
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            RootPackageRoutingProjection>(routing));
    if(plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *plan));
        append_build_plan_blockers(*plan, observation);
        append_artifact_projection(*plan, projections.front(), observation);
    }

    RepositoryPackageTransactionIntent repository_transaction;
    repository_transaction.policy.needed = prepared.needed;
    for(const RepositoryRootPackageRouteTarget& target :
        routing.repository_targets()) {
        UnifiedPlanRootReference root(
            RootTargetIdentity{
                target.selection_index(),
                target.identity().package_name},
            target.identity(),
            UnifiedPlanRootRouteKind::RepositoryTransaction);
        observation.roots.push_back(root);
        repository_transaction.targets.push_back(
            RepositoryRootInstallIntent{root});
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageSearchCandidate>(
                *find_root_metadata(
                    discovery, target.selected_target())));
    }
    for(std::size_t index = 0; index < routing.aur_targets().size();
        ++index) {
        const AurRootPackageRouteTarget& target =
            routing.aur_targets()[index];
        observation.roots.emplace_back(
            plan->root_targets[index], target.identity(),
            UnifiedPlanRootRouteKind::AurSourceBuild);
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageSearchCandidate>(
                *find_root_metadata(
                    discovery, target.selected_target())));
    }
    if(plan != nullptr) {
        append_repository_dependency_intents(*plan, repository_transaction);
    }

    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        if(!repository_transaction.targets.empty()) {
            observation.transaction_intents.push_back(
                std::move(repository_transaction));
        }
        append_source_artifact_transaction(observation, prepared.needed);
    }
    append_root_phases(routing, plan, observation);
    return UnifiedPlanProjection::make(
        std::move(projections), std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection> project_fetch_unified_plan(
    FetchUnifiedPlanProjectionInput input) {
    const FetchPreparation& preparation = input.source.get();
    const BuildPlan& plan = preparation.plan;
    if(plan.root_targets.empty() || preparation.invocation_targets.empty()) {
        reject_inconsistent_input(
            "Fetch projection lacks invocation root authority.");
    }
    const std::vector<std::string>* repository_order =
        validate_build_plan_repository_authority(plan);

    UnifiedPlanObservationInput observation;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<FetchPreparation>(
            preparation));
    observation.dependency_authorities.push_back(
        UnifiedPlanDependencyAuthorityReference::from_build_plan(plan));
    append_fetch_build_plan_blockers(plan, observation);
    for(std::size_t index = 0; index < plan.root_targets.size(); ++index) {
        const PlannedPackageTarget* target = find_plan_root_target(
            plan, index,
            "Fetch BuildPlan root identity is inconsistent.");
        if(target == nullptr) continue;
        observation.roots.emplace_back(
            plan.root_targets[index],
            AurRootPackageIdentity{
                target->package_name, target->package_base},
            UnifiedPlanRootRouteKind::AurSourceBuild);
    }
    for(std::size_t index = 0; index < plan.order.size(); ++index) {
        observation.build_units.emplace_back(
            AurPackageBaseBuildUnitReference(std::cref(plan), index));
    }
    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready &&
       observation.roots.size() != plan.root_targets.size()) {
        reject_inconsistent_input(
            "Fetch Ready projection lacks a complete root identity.");
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        observation, UnifiedPlanObservationPhase::MetadataDiscovery,
        UnifiedPlanAuthorityOwner::AurRpc);
    append_phase(
        observation, UnifiedPlanObservationPhase::ProviderDecision,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        observation, UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        append_phase(
            observation, UnifiedPlanObservationPhase::SourceRetrieval,
            UnifiedPlanAuthorityOwner::Git);
    }
    return UnifiedPlanProjection::make({}, std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection> project_sync_install_unified_plan(
    SyncInstallUnifiedPlanProjectionInput input) {
    if(const auto* blocked = std::get_if<std::reference_wrapper<
           const SyncInstallPreparationFailure>>(&input.source);
       blocked != nullptr) {
        const SyncInstallPreparationFailure& failure = blocked->get();
        if(failure.details.empty()) {
            reject_inconsistent_input(
                "Blocked sync preparation has no typed detail.");
        }
        UnifiedPlanObservationInput observation;
        observation.status = UnifiedPlanObservationStatus::Blocked;
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                SyncInstallPreparationFailure>(failure));
        observation.blockers.push_back(
            SyncInstallPreparationUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    SyncInstallPreparationFailure>(failure)});
        if(failure.aur_build_plan.has_value()) {
            const BuildPlan& plan = failure.aur_build_plan.value();
            observation.dependency_authorities.push_back(
                UnifiedPlanDependencyAuthorityReference::from_build_plan(
                    plan));
            append_build_plan_blockers(plan, observation);
            if(plan.configured_repository_order.has_value()) {
                observation.configured_repository_order.emplace(std::cref(
                    plan.configured_repository_order.value()));
            }
        }
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RequestDiscovery,
            UnifiedPlanAuthorityOwner::Moguet);
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ExecutionProjection,
            UnifiedPlanAuthorityOwner::Moguet);
        return UnifiedPlanProjection::make({}, std::move(observation));
    }

    const PreparedSyncInstall& prepared =
        std::get<std::reference_wrapper<const PreparedSyncInstall>>(
            input.source)
            .get();
    if(prepared.ordered_roots.empty() && !prepared.system_update) {
        reject_inconsistent_input("Prepared sync route has no operation.");
    }
    const BuildPlan* plan = prepared.aur_build_plan.has_value()
                                ? &prepared.aur_build_plan.value()
                                : nullptr;
    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    const std::vector<std::string>* repository_order = plan == nullptr
                                                           ? nullptr
                                                           : validate_build_plan_repository_authority(*plan);
    if(plan != nullptr) {
        projections.push_back(
            project_build_plan_required_artifact_targets(*plan));
    }

    UnifiedPlanObservationInput observation;
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<PreparedSyncInstall>(
            prepared));
    if(plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *plan));
        append_build_plan_blockers(*plan, observation);
        append_artifact_projection(
            *plan, projections.front(), observation);
    }

    RepositoryPackageTransactionIntent repository_transaction;
    repository_transaction.policy.needed = prepared.needed;
    std::vector<const SyncRepositorySourceRoot*> repository_source_roots;
    std::vector<bool> observed_aur_roots(
        plan == nullptr ? 0 : plan->root_targets.size(), false);
    bool has_repository_root = false;
    bool has_repository_transaction_root = false;
    bool has_aur_root = false;
    for(std::size_t ordered_root_index = 0;
        ordered_root_index < prepared.ordered_roots.size();
        ++ordered_root_index) {
        const SyncInstallRoot& sync_root =
            prepared.ordered_roots[ordered_root_index];
        std::visit(
            [&, ordered_root_index](const auto& root) {
                using Root = std::decay_t<decltype(root)>;
                if constexpr(std::is_same_v<
                                 Root,
                                 SyncRepositoryTransactionRoot> ||
                             std::is_same_v<
                                 Root,
                                 SyncRepositorySourceRoot>) {
                    const RepositoryPackagePresent& package =
                        root.package;
                    if(package.repository_name.empty() ||
                       package.package_name.empty() ||
                       package.package_name !=
                           root.invocation_correlation.requested_name ||
                       !package.configured_repository_order.has_value()) {
                        reject_inconsistent_input(
                            "Prepared sync repository root identity is incomplete.");
                    }
                    validate_configured_repository_identity(
                        ConfiguredRepositoryIdentity{
                            package.repository_name,
                            package.configured_order},
                        package.configured_repository_order.value(),
                        "Prepared sync repository root used an inconsistent repository configuration.");
                    merge_repository_order_authority(
                        &package.configured_repository_order.value(),
                        repository_order,
                        "Prepared sync roots used different repository configurations.");
                    has_repository_root = true;
                    if constexpr(std::is_same_v<
                                     Root,
                                     SyncRepositoryTransactionRoot>) {
                        has_repository_transaction_root = true;
                        UnifiedPlanRootReference observed_root(
                            root.invocation_correlation,
                            RepositoryRootPackageIdentity{
                                package.repository_name,
                                package.package_name},
                            UnifiedPlanRootRouteKind::
                                RepositoryTransaction);
                        observation.roots.push_back(observed_root);
                        repository_transaction.targets.push_back(
                            RepositoryRootInstallIntent{
                                observed_root});
                    } else {
                        if(root.invocation_correlation.invocation_index !=
                               ordered_root_index ||
                           !root.source_work_item_index.has_value()) {
                            reject_inconsistent_input(
                                "Prepared sync repository source root lost its typed work correlation.");
                        }
                        if(root.source.source_kind() !=
                               SourceBuildSourceKind::Repository ||
                           root.source.requested_name() !=
                               package.package_name ||
                           root.source.package_base().empty() ||
                           root.source.canonical_source_key().empty()) {
                            reject_inconsistent_input(
                                "Prepared sync repository source identity is inconsistent.");
                        }
                        observation.roots.emplace_back(
                            root.invocation_correlation,
                            RepositorySourceBuildRootIdentity{
                                root.source.requested_name(),
                                root.source.package_base(),
                                root.source.canonical_source_key()},
                            UnifiedPlanRootRouteKind::
                                RepositorySourceBuild);
                        repository_source_roots.push_back(&root);
                    }
                    observation.root_metadata.push_back(
                        UnifiedPlanBorrowedAuthorityReference<
                            RepositoryPackagePresent>(package));
                    if(repository_order == nullptr) {
                        repository_order =
                            &package.configured_repository_order.value();
                    }
                } else {
                    if(plan == nullptr ||
                       root.build_plan_root_index >=
                           plan->root_targets.size() ||
                       observed_aur_roots[root.build_plan_root_index]) {
                        reject_inconsistent_input(
                            "Prepared sync AUR root correlation is inconsistent.");
                    }
                    const PlannedPackageTarget& target =
                        require_plan_root_target(
                            *plan,
                            root.build_plan_root_index,
                            "Prepared sync AUR root lacks BuildPlan identity.");
                    if(target.package_name !=
                           root.invocation_correlation.requested_name ||
                       plan->root_targets[root.build_plan_root_index]
                               .requested_name !=
                           root.invocation_correlation.requested_name) {
                        reject_inconsistent_input(
                            "Prepared sync AUR root differs from its BuildPlan.");
                    }
                    observed_aur_roots[root.build_plan_root_index] = true;
                    has_aur_root = true;
                    observation.roots.emplace_back(
                        root.invocation_correlation,
                        AurRootPackageIdentity{
                            target.package_name,
                            target.package_base},
                        UnifiedPlanRootRouteKind::AurSourceBuild);
                    if(root.repository_absence.has_value()) {
                        const RepositoryPackageNotFound& absence =
                            root.repository_absence.value();
                        merge_repository_order_authority(
                            absence.configured_repository_order
                                    .has_value()
                                ? &absence
                                       .configured_repository_order
                                       .value()
                                : nullptr,
                            repository_order,
                            "Prepared sync AUR roots used different repository configurations.");
                        observation.root_metadata.push_back(
                            UnifiedPlanBorrowedAuthorityReference<
                                RepositoryPackageNotFound>(
                                absence));
                    }
                }
            },
            sync_root);
    }
    if(plan != nullptr &&
       std::any_of(
           observed_aur_roots.begin(), observed_aur_roots.end(),
           [](bool observed) { return !observed; })) {
        reject_inconsistent_input(
            "Prepared sync BuildPlan has an unobserved invocation root.");
    }

    const bool has_source_roots =
        !repository_source_roots.empty() || has_aur_root;
    if(has_source_roots != prepared.source_invocation.has_value()) {
        reject_inconsistent_input(
            "Prepared sync source roots and invocation do not match.");
    }
    if(prepared.source_invocation.has_value()) {
        const auto& work_items = prepared.source_invocation->work_items;
        std::vector<bool> observed_work(work_items.size(), false);
        for(const SyncRepositorySourceRoot* root :
            repository_source_roots) {
            if(!root->source_work_item_index.has_value() ||
               root->source_work_item_index.value() >= work_items.size()) {
                reject_inconsistent_input(
                    "Prepared sync repository source work is missing.");
            }
            const std::size_t matched_index =
                root->source_work_item_index.value();
            const ProductionSourceBuildWorkItem* matched =
                &work_items[matched_index];
            if(observed_work[matched_index] ||
               matched->required_target_provenance !=
                   RequiredTargetProvenance::
                       RepositoryExactPackageProjection ||
               matched->artifact_lifecycle_intent !=
                   ArtifactLifecycleIntent::SingularCompatibility) {
                reject_inconsistent_input(
                    "Prepared sync repository source work correlation is inconsistent.");
            }
            if(matched->request.needed != prepared.needed) {
                reject_inconsistent_input(
                    "Prepared sync repository source work used a different needed policy.");
            }
            merge_repository_order_authority(
                matched->configured_repository_order.has_value()
                    ? &matched->configured_repository_order.value()
                    : nullptr,
                repository_order,
                "Prepared sync repository source work used a different repository configuration.");
            observed_work[matched_index] = true;
            PreparedRemoteSourceBuildUnitReference unit(
                std::cref(root->source), std::cref(*matched));
            if(!unit.has_complete_identity()) {
                reject_inconsistent_input(
                    "Prepared sync repository source work is inconsistent.");
            }
            observation.build_units.push_back(
                PreparedRemoteSourceBuildUnitReference(
                    std::cref(root->source), std::cref(*matched)));
            for(const RequiredPackageArtifactTarget& target :
                matched->required_targets) {
                observation.required_artifacts.emplace_back(
                    PreparedRemoteSourceBuildUnitReference(
                        std::cref(root->source),
                        std::cref(*matched)),
                    std::cref(target));
            }
        }
        if(plan != nullptr) {
            const auto* projected = projections.front().success();
            if(projected == nullptr) {
                reject_inconsistent_input(
                    "Prepared sync source invocation has an invalid artifact projection.");
            }
            for(const ProjectedBuildPlanArtifactTargets& unit :
                projected->build_units) {
                const ProductionSourceBuildWorkItem* matched = nullptr;
                std::size_t matched_index = 0;
                for(std::size_t index = 0; index < work_items.size(); ++index) {
                    const ProductionSourceBuildWorkItem& work =
                        work_items[index];
                    if(work.required_target_provenance !=
                           RequiredTargetProvenance::
                               AurBuildPlanProjection ||
                       work.artifact_lifecycle_intent !=
                           ArtifactLifecycleIntent::PackageBaseSet ||
                       work.request.checkout_name != unit.package_base) {
                        continue;
                    }
                    if(matched != nullptr) {
                        reject_inconsistent_input(
                            "Prepared sync AUR work is ambiguous.");
                    }
                    matched = &work;
                    matched_index = index;
                }
                if(matched == nullptr || observed_work[matched_index] ||
                   matched->request.needed != prepared.needed ||
                   matched->required_targets.size() !=
                       unit.required_targets.size() ||
                   !std::equal(
                       matched->required_targets.begin(),
                       matched->required_targets.end(),
                       unit.required_targets.begin(),
                       same_required_artifact_target)) {
                    reject_inconsistent_input(
                        "Prepared sync AUR work differs from its BuildPlan.");
                }
                observed_work[matched_index] = true;
            }
        }
        if(std::any_of(
               observed_work.begin(), observed_work.end(),
               [](bool observed) { return !observed; })) {
            reject_inconsistent_input(
                "Prepared sync invocation has unowned source work.");
        }
        for(const ProvidedDependency& provider :
            prepared.source_invocation->selected_repository_providers) {
            const bool projected_by_plan =
                plan != nullptr &&
                std::any_of(
                    plan->provided.begin(), plan->provided.end(),
                    [&provider](
                        const BuildPlanProvidedDependency&
                            selected) {
                        return selected.resolution ==
                                   ProviderResolutionKind::
                                       UserSelected &&
                               std::holds_alternative<
                                   RepositoryProviderOrigin>(
                                   selected.provider.origin) &&
                               same_provider_identity(
                                   selected.provider, provider);
                    });
            if(projected_by_plan) continue;
            repository_transaction.targets.push_back(
                RepositoryProviderInstallIntent{
                    UnifiedPlanBorrowedAuthorityReference<
                        ProvidedDependency>(provider)});
        }
    }
    if(plan != nullptr) {
        append_repository_dependency_intents(*plan, repository_transaction);
    }
    if(prepared.system_update) {
        repository_transaction.targets.push_back(
            RepositorySystemUpgradeIntent{});
    }
    const bool initial_repository_transaction_required =
        prepared.source_selection == PackageSourceSelection::Auto &&
        (has_repository_transaction_root || prepared.system_update);
    if(initial_repository_transaction_required !=
       prepared.repository_transaction_required) {
        reject_inconsistent_input(
            "Prepared sync repository transaction policy is inconsistent.");
    }
    const bool has_repository_transaction =
        !repository_transaction.targets.empty();
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        if(has_repository_transaction) {
            observation.transaction_intents.push_back(
                std::move(repository_transaction));
        }
        append_source_artifact_transaction(observation, prepared.needed);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    if(has_repository_root || prepared.system_update) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Libalpm);
    }
    if(has_aur_root) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc);
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet);
    }
    append_phase(
        observation,
        UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        if(has_repository_transaction) {
            append_phase(
                observation,
                UnifiedPlanObservationPhase::RepositoryTransaction,
                UnifiedPlanAuthorityOwner::Pacman);
        }
        if(has_source_roots) append_source_mutation_phases(observation);
    }
    return UnifiedPlanProjection::make(
        std::move(projections), std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection>
project_remote_source_build_unified_plan(
    RemoteSourceBuildUnifiedPlanProjectionInput input) {
    if(const auto* blocked = std::get_if<std::reference_wrapper<
           const RemoteSourceBuildPlanFailure>>(&input.source);
       blocked != nullptr) {
        const RemoteSourceBuildPlanFailure& failure = blocked->get();
        if(failure.source.source_kind() != SourceBuildSourceKind::Aur ||
           failure.source.requested_name().empty() ||
           failure.source.package_base().empty()) {
            reject_inconsistent_input(
                "Remote source-build failure lacks AUR identity.");
        }
        UnifiedPlanObservationInput observation;
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                RemoteSourceBuildPlanFailure>(failure));
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                failure.plan));
        if(failure.plan.configured_repository_order.has_value()) {
            observation.configured_repository_order.emplace(std::cref(
                failure.plan.configured_repository_order.value()));
        }
        append_build_plan_blockers(failure.plan, observation);
        if(observation.blockers.empty()) {
            reject_inconsistent_input(
                "Remote source-build failure has no typed BuildPlan blocker.");
        }
        RootTargetIdentity correlation{0, failure.source.requested_name()};
        if(!failure.plan.root_targets.empty()) {
            correlation = failure.plan.root_targets.front();
        }
        observation.roots.emplace_back(
            std::move(correlation),
            AurRootPackageIdentity{
                failure.source.requested_name(),
                failure.source.package_base()},
            UnifiedPlanRootRouteKind::AurSourceBuild);
        observation.root_metadata.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                ResolvedSourceBuildIdentity>(failure.source));
        observation.status = UnifiedPlanObservationStatus::Blocked;
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RequestDiscovery,
            UnifiedPlanAuthorityOwner::Moguet);
        append_phase(
            observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc);
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet);
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ExecutionProjection,
            UnifiedPlanAuthorityOwner::Moguet);
        return UnifiedPlanProjection::make({}, std::move(observation));
    }

    const PreparedRemoteSourceBuild& prepared =
        std::get<std::reference_wrapper<
            const PreparedRemoteSourceBuild>>(input.source)
            .get();
    UnifiedPlanObservationInput observation;
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            PreparedRemoteSourceBuild>(prepared));
    observation.root_metadata.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            ResolvedSourceBuildIdentity>(prepared.source));

    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    const BuildPlan* plan = prepared.aur_build_plan.has_value()
                                ? &prepared.aur_build_plan.value()
                                : nullptr;
    RepositoryPackageTransactionIntent repository_transaction;
    bool has_repository_metadata = false;
    if(prepared.source.source_kind() == SourceBuildSourceKind::Aur) {
        if(plan == nullptr || plan->root_targets.size() != 1) {
            reject_inconsistent_input(
                "Prepared AUR source build lacks its invocation BuildPlan.");
        }
        const PlannedPackageTarget& root = require_plan_root_target(
            *plan, 0,
            "Prepared AUR source-build root identity is inconsistent.");
        if(root.package_name != prepared.source.requested_name() ||
           root.package_base != prepared.source.package_base()) {
            reject_inconsistent_input(
                "Prepared AUR source-build identity differs from its BuildPlan.");
        }
        projections.push_back(
            project_build_plan_required_artifact_targets(*plan));
        const std::vector<std::string>* repository_order =
            validate_aur_source_work(
                *plan, prepared.invocation, false,
                projections.front());
        if(repository_order != nullptr) {
            observation.configured_repository_order.emplace(
                std::cref(*repository_order));
        }
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *plan));
        append_build_plan_blockers(*plan, observation);
        append_artifact_projection(
            *plan, projections.front(), observation);
        append_repository_dependency_intents(
            *plan, repository_transaction);
        observation.roots.emplace_back(
            plan->root_targets.front(),
            AurRootPackageIdentity{
                prepared.source.requested_name(),
                prepared.source.package_base()},
            UnifiedPlanRootRouteKind::AurSourceBuild);
    } else {
        if(plan != nullptr) {
            reject_inconsistent_input(
                "Prepared repository source build retained an AUR BuildPlan.");
        }
        append_prepared_remote_source_work(prepared, observation);
        const ProductionSourceBuildWorkItem& work =
            prepared.invocation.work_items.front();
        if(work.configured_repository_order.has_value()) {
            observation.configured_repository_order.emplace(
                std::cref(work.configured_repository_order.value()));
        }
        for(const ProvidedDependency& provider :
            prepared.invocation.selected_repository_providers) {
            repository_transaction.targets.push_back(
                RepositoryProviderInstallIntent{
                    UnifiedPlanBorrowedAuthorityReference<
                        ProvidedDependency>(provider)});
        }
        observation.roots.emplace_back(
            RootTargetIdentity{0, prepared.source.requested_name()},
            RepositorySourceBuildRootIdentity{
                prepared.source.requested_name(),
                prepared.source.package_base(),
                prepared.source.canonical_source_key()},
            UnifiedPlanRootRouteKind::RepositorySourceBuild);
        has_repository_metadata = true;
    }

    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        if(!repository_transaction.targets.empty()) {
            observation.transaction_intents.push_back(
                std::move(repository_transaction));
        }
        append_source_artifact_transaction(observation, false);
    }
    append_phase(
        observation, UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        observation, UnifiedPlanObservationPhase::MetadataDiscovery,
        has_repository_metadata ? UnifiedPlanAuthorityOwner::Libalpm
                                : UnifiedPlanAuthorityOwner::AurRpc);
    if(plan != nullptr) {
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet);
    }
    append_phase(
        observation,
        UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        if(!observation.transaction_intents.empty() &&
           std::holds_alternative<RepositoryPackageTransactionIntent>(
               observation.transaction_intents.front())) {
            append_phase(
                observation,
                UnifiedPlanObservationPhase::RepositoryTransaction,
                UnifiedPlanAuthorityOwner::Pacman);
        }
        append_source_mutation_phases(observation);
    }
    return UnifiedPlanProjection::make(
        std::move(projections), std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection> project_local_source_unified_plan(
    LocalSourceUnifiedPlanProjectionInput input) {
    if(const auto* metadata_blocked =
           std::get_if<LocalSourceMetadataEvaluationProjectionInput>(
               &input.source);
       metadata_blocked != nullptr) {
        UnifiedPlanObservationInput observation;
        append_local_metadata_evaluation_blocker(
            metadata_blocked->source_root.get(), observation);
        return UnifiedPlanProjection::make({}, std::move(observation));
    }
    const auto* ready = std::get_if<std::reference_wrapper<
        const LocalSourceBuildProjectionAuthority>>(&input.source);
    const LocalSourceBuildPlanFailureProjectionInput* blocked =
        std::get_if<LocalSourceBuildPlanFailureProjectionInput>(
            &input.source);
    if(ready == nullptr && blocked == nullptr) {
        reject_inconsistent_input(
            "Local source projection has no supported authority.");
    }
    const LocalSourceRoot& source_root = ready != nullptr
                                             ? ready->get().source_root()
                                             : blocked->source_root.get();
    const LocalBuildPlan& local_plan = ready != nullptr
                                           ? ready->get().local_build_plan()
                                           : blocked->local_build_plan.get();
    const LocalPackageMetadata& metadata = ready != nullptr
                                               ? ready->get().accepted_metadata()
                                               : local_plan.local_metadata();
    if(ready != nullptr) {
        if(!ready->get().has_complete_identity() ||
           !local_plan.failures().empty()) {
            reject_inconsistent_input(
                "Prepared local source projection authority is inconsistent.");
        }
    } else {
        validate_local_source_input(source_root, local_plan);
        if(local_plan.failures().empty()) {
            reject_inconsistent_input(
                "Ready local source projection lacks prepared invocation authority.");
        }
    }
    const BuildPlan& plan = local_plan.build_plan();
    const std::vector<std::string>* repository_order =
        validate_build_plan_repository_authority(plan);

    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    projections.reserve(1);
    projections.push_back(project_build_plan_required_artifact_targets(plan));

    UnifiedPlanObservationInput observation;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.dependency_authorities.push_back(
        UnifiedPlanDependencyAuthorityReference::from_local_build_plan(
            local_plan));
    if(ready != nullptr) {
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                LocalSourceBuildProjectionAuthority>(
                ready->get()));
    } else {
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<LocalBuildPlan>(
                local_plan));
    }
    LocalSourceRootObservationIdentity local_identity{
        source_root.canonical_path(), source_root.directory_identity()};
    for(const RootTargetIdentity& root : plan.root_targets) {
        observation.roots.emplace_back(
            root, local_identity,
            UnifiedPlanRootRouteKind::LocalSourceBuild);
    }
    observation.root_metadata.push_back(
        UnifiedPlanBorrowedAuthorityReference<LocalPackageMetadata>(
            metadata));
    append_build_plan_blockers(plan, observation);
    for(const LocalDependencyPlanFailure& failure : local_plan.failures()) {
        observation.blockers.push_back(
            LocalDependencyPlanUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    LocalDependencyPlanFailure>(failure)});
    }
    append_artifact_projection(
        plan, projections.front(), observation, &local_identity,
        &metadata);

    RepositoryPackageTransactionIntent repository_transaction;
    repository_transaction.policy.needed = input.needed;
    append_repository_dependency_intents(plan, repository_transaction);
    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        if(!repository_transaction.targets.empty()) {
            observation.transaction_intents.push_back(
                std::move(repository_transaction));
        }
        append_source_artifact_transaction(observation, input.needed);
    }
    append_local_phases(plan, observation);
    return UnifiedPlanProjection::make(
        std::move(projections), std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection> project_aur_update_unified_plan(
    AurUpdateUnifiedPlanProjectionInput input) {
    const AurUpdateQueryResult& query = input.query_result.get();
    const AurUpdateExecutionPreflight& preflight = input.preflight.get();
    const std::vector<std::string>* repository_order =
        validate_aur_update_input(query, preflight);
    if(input.source_build_preparation.has_value() &&
       input.source_build_preparation->get()
               .devel_requires_check_policy !=
           preflight.devel_requires_check_policy) {
        reject_inconsistent_input(
            "AUR execution preflight and source preparation policies differ.");
    }
    const BuildPlan* plan = preflight.build_plan.has_value()
                                ? &preflight.build_plan.value()
                                : nullptr;
    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    std::vector<ProjectedBuildPlanArtifactTargets> route_artifact_targets;
    if(plan != nullptr) {
        projections.reserve(1);
        projections.push_back(
            project_build_plan_required_artifact_targets(*plan));
        route_artifact_targets = project_aur_update_artifact_targets(
            preflight, projections.front());
    }

    UnifiedPlanObservationInput observation;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            AurUpdateExecutionPreflight>(preflight));
    if(input.filtered_operation.has_value()) {
        const PreparedFilteredAurUpdateOperation& operation =
            input.filtered_operation->get();
        if(&operation.original_query_result() != &query ||
           &operation.execution_preflight() != &preflight ||
           operation.devel_requires_check_policy_snapshot() !=
               preflight.devel_requires_check_policy ||
           !input.source_build_preparation.has_value() ||
           !operation.source_build_preparation().has_value() ||
           &operation.source_build_preparation().value() !=
               &input.source_build_preparation->get()) {
            reject_inconsistent_input(
                "Filtered AUR operation projection authorities are inconsistent.");
        }
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                PreparedFilteredAurUpdateOperation>(operation));
    }
    append_aur_update_roots_and_blockers(query, preflight, observation);
    if(input.source_build_preparation.has_value()) {
        append_aur_update_preparation_blockers(
            input.source_build_preparation->get(), preflight,
            observation);
    }
    if(plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *plan));
        append_build_plan_blockers(*plan, observation);
        if(projections.front().failure() != nullptr) {
            append_artifact_projection(
                *plan, projections.front(), observation);
        } else {
            append_artifact_units(
                *plan, route_artifact_targets, observation);
        }
    }

    RepositoryPackageTransactionIntent repository_transaction;
    repository_transaction.policy.needed = input.needed;
    if(plan != nullptr) {
        append_repository_dependency_intents(*plan, repository_transaction);
    }
    if(!observation.blockers.empty()) {
        observation.status = UnifiedPlanObservationStatus::Blocked;
    } else if(!has_executable_update_targets(preflight)) {
        observation.status = UnifiedPlanObservationStatus::NoOp;
    } else {
        observation.status = UnifiedPlanObservationStatus::Ready;
        if(!repository_transaction.targets.empty()) {
            observation.transaction_intents.push_back(
                std::move(repository_transaction));
        }
        append_source_artifact_transaction(observation, input.needed);
    }
    append_aur_update_phases(query, plan, observation);
    return UnifiedPlanProjection::make(
        std::move(projections), std::move(route_artifact_targets),
        std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection>
project_filtered_aur_update_unified_plan(
    const PreparedFilteredAurUpdateOperation& prepared) {
    if(!prepared.source_build_preparation().has_value()) {
        reject_inconsistent_input(
            "Filtered AUR operation lacks a valid production preparation snapshot.");
    }
    return project_aur_update_unified_plan(
        AurUpdateUnifiedPlanProjectionInput{
            std::cref(prepared.original_query_result()),
            std::cref(prepared.execution_preflight()), false,
            std::cref(
                prepared.source_build_preparation().value()),
            std::cref(prepared)});
}

std::unique_ptr<UnifiedPlanProjection>
project_system_source_upgrade_unified_plan(
    SystemSourceUpgradeUnifiedPlanProjectionInput input) {
    if(const auto* blocked = std::get_if<std::reference_wrapper<
           const SystemSourceUpgradeResult>>(&input.source);
       blocked != nullptr) {
        UnifiedPlanObservationInput observation;
        append_blocked_system_source(blocked->get(), observation);
        return UnifiedPlanProjection::make({}, std::move(observation));
    }
    const SystemSourceUpgradeProjectionAuthority& prepared =
        std::get<std::reference_wrapper<
            const SystemSourceUpgradeProjectionAuthority>>(
            input.source)
            .get();
    const std::vector<std::string>* repository_order =
        validate_system_source_authority(prepared);
    const BuildPlan* plan = prepared.aur_invocation_plan();

    UnifiedPlanObservationInput observation;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            SystemSourceUpgradeProjectionAuthority>(prepared));
    append_system_source_roots(prepared, observation);
    append_system_source_issues(prepared.issues(), observation);
    append_prepared_source_work(prepared, observation);
    append_registered_devel_observations(prepared, input.registered_devel, observation);
    if(plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *plan));
        append_build_plan_blockers(*plan, observation);
    }

    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        RepositoryPackageTransactionIntent repository_transaction;
        repository_transaction.policy.needed =
            prepared.snapshot().options.needed;
        repository_transaction.targets.push_back(
            RepositorySystemUpgradeIntent{});
        if(plan != nullptr) {
            append_repository_dependency_intents(
                *plan, repository_transaction);
        }
        observation.transaction_intents.push_back(
            std::move(repository_transaction));
        append_source_artifact_transaction(
            observation, prepared.snapshot().options.needed);
    }
    append_system_source_phases(prepared, observation);
    return UnifiedPlanProjection::make({}, std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection> project_upgrade_all_unified_plan(
    UpgradeAllUnifiedPlanProjectionInput input) {
    if(const auto* blocked = std::get_if<std::reference_wrapper<
           const UpgradeAllOperationResult>>(&input.source);
       blocked != nullptr) {
        if(input.aur_query_result.has_value() ||
           input.aur_preflight.has_value() || input.issues.has_value() ||
           input.aur_source_build_preparation.has_value() ||
           input.aur_operation_preflight.has_value()) {
            reject_inconsistent_input(
                "Blocked upgrade-all projection received Ready-only authorities.");
        }
        UnifiedPlanObservationInput observation;
        append_blocked_upgrade_all(blocked->get(), observation);
        return UnifiedPlanProjection::make({}, std::move(observation));
    }
    const PreparedUpgradeAllAurPreflight* aggregate_aur_preflight =
        input.aur_operation_preflight.has_value()
            ? &input.aur_operation_preflight->get()
            : nullptr;
    if(aggregate_aur_preflight != nullptr &&
       !aggregate_aur_preflight->has_filtered_operation()) {
        if(input.aur_query_result.has_value() ||
           input.aur_preflight.has_value() ||
           input.aur_source_build_preparation.has_value() ||
           !input.issues.has_value() ||
           &input.issues->get() != &aggregate_aur_preflight->issues() ||
           aggregate_aur_preflight->issues().empty()) {
            reject_inconsistent_input(
                "Blocked upgrade-all AUR preflight authorities are inconsistent.");
        }
        const UpgradeAllOperationProjectionAuthority& prepared =
            std::get<std::reference_wrapper<
                const UpgradeAllOperationProjectionAuthority>>(
                input.source)
                .get();
        validate_upgrade_all_authority(prepared);
        const SystemSourceUpgradeProjectionAuthority& system_source =
            prepared.system_source();
        const std::vector<std::string>* repository_order =
            validate_system_source_authority(system_source);
        UnifiedPlanObservationInput observation;
        observation.status = UnifiedPlanObservationStatus::Blocked;
        if(repository_order != nullptr) {
            observation.configured_repository_order.emplace(
                std::cref(*repository_order));
        }
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationProjectionAuthority>(prepared));
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                PreparedUpgradeAllAurPreflight>(
                *aggregate_aur_preflight));
        append_system_source_roots(system_source, observation);
        append_registered_devel_observations(system_source, input.registered_devel, observation);
        append_system_source_issues(system_source.issues(), observation);
        append_prepared_source_work(system_source, observation);
        for(const UpgradeAllOperationIssue& issue :
            aggregate_aur_preflight->issues()) {
            observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    UpgradeAllOperationIssue>(issue)});
        }
        if(const BuildPlan* registered_plan =
               system_source.aur_invocation_plan();
           registered_plan != nullptr) {
            observation.dependency_authorities.push_back(
                UnifiedPlanDependencyAuthorityReference::from_build_plan(
                    *registered_plan));
            append_build_plan_blockers(*registered_plan, observation);
        }
        append_phase(
            observation,
            UnifiedPlanObservationPhase::RequestDiscovery,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::Preparation});
        append_phase(
            observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Libalpm,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::ForeignInventory});
        if(aggregate_aur_preflight->stopped_phase() ==
               UpgradeAllOperationPhase::AurQuery ||
           aggregate_aur_preflight->stopped_phase() ==
               UpgradeAllOperationPhase::AurPreparation) {
            append_phase(
                observation,
                UnifiedPlanObservationPhase::MetadataDiscovery,
                UnifiedPlanAuthorityOwner::AurRpc,
                ExistingRoutePhaseReference{
                    UpgradeAllOperationPhase::AurQuery});
        }
        append_phase(
            observation,
            UnifiedPlanObservationPhase::ExecutionProjection,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::Preparation});
        return UnifiedPlanProjection::make({}, std::move(observation));
    }
    if(!input.aur_query_result.has_value() ||
       !input.aur_preflight.has_value() || !input.issues.has_value()) {
        reject_inconsistent_input(
            "Ready upgrade-all projection lacks execution authorities.");
    }
    const UpgradeAllOperationProjectionAuthority& prepared =
        std::get<std::reference_wrapper<
            const UpgradeAllOperationProjectionAuthority>>(
            input.source)
            .get();
    validate_upgrade_all_authority(prepared);
    const SystemSourceUpgradeProjectionAuthority& system_source =
        prepared.system_source();
    const std::vector<std::string>* repository_order =
        validate_system_source_authority(system_source);
    const AurUpdateQueryResult& aur_query =
        input.aur_query_result->get();
    const AurUpdateExecutionPreflight& aur_preflight =
        input.aur_preflight->get();
    if(aggregate_aur_preflight != nullptr) {
        const PreparedFilteredAurUpdateOperation* filtered =
            aggregate_aur_preflight->filtered_operation();
        if(filtered == nullptr ||
           &aggregate_aur_preflight->issues() != &input.issues->get() ||
           &filtered->original_query_result() != &aur_query ||
           &filtered->execution_preflight() != &aur_preflight ||
           filtered->devel_requires_check_policy_snapshot() !=
               aur_preflight.devel_requires_check_policy ||
           !input.aur_source_build_preparation.has_value() ||
           !filtered->source_build_preparation().has_value() ||
           &filtered->source_build_preparation().value() !=
               &input.aur_source_build_preparation->get() ||
           input.aur_source_build_preparation->get()
                   .devel_requires_check_policy !=
               aur_preflight.devel_requires_check_policy) {
            reject_inconsistent_input(
                "Prepared upgrade-all AUR authorities are inconsistent.");
        }
    }
    merge_repository_order_authority(
        validate_aur_update_input(aur_query, aur_preflight),
        repository_order,
        "upgrade-all nested routes used different repository configurations.");
    const BuildPlan* registered_plan =
        system_source.aur_invocation_plan();
    const BuildPlan* aur_plan = aur_preflight.build_plan.has_value()
                                    ? &aur_preflight.build_plan.value()
                                    : nullptr;

    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    std::vector<ProjectedBuildPlanArtifactTargets> route_artifact_targets;
    if(aur_plan != nullptr) {
        projections.reserve(1);
        projections.push_back(
            project_build_plan_required_artifact_targets(*aur_plan));
        route_artifact_targets = project_aur_update_artifact_targets(
            aur_preflight, projections.front());
    }

    UnifiedPlanObservationInput observation;
    if(repository_order != nullptr) {
        observation.configured_repository_order.emplace(
            std::cref(*repository_order));
    }
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            UpgradeAllOperationProjectionAuthority>(prepared));
    observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            AurUpdateExecutionPreflight>(aur_preflight));
    if(aggregate_aur_preflight != nullptr) {
        observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                PreparedUpgradeAllAurPreflight>(
                *aggregate_aur_preflight));
    }
    append_system_source_roots(system_source, observation);
    append_registered_devel_observations(system_source, input.registered_devel, observation);
    append_system_source_issues(system_source.issues(), observation);
    append_prepared_source_work(system_source, observation);
    const std::size_t registered_artifact_count =
        observation.required_artifacts.size();
    append_aur_update_roots_and_blockers(
        aur_query, aur_preflight, observation);
    if(input.aur_source_build_preparation.has_value()) {
        append_aur_update_preparation_blockers(
            input.aur_source_build_preparation->get(), aur_preflight,
            observation);
    }
    for(const UpgradeAllOperationIssue& issue : input.issues->get()) {
        observation.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(issue)});
    }

    if(registered_plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *registered_plan));
        append_build_plan_blockers(*registered_plan, observation);
    }
    if(aur_plan != nullptr) {
        observation.dependency_authorities.push_back(
            UnifiedPlanDependencyAuthorityReference::from_build_plan(
                *aur_plan));
        append_build_plan_blockers(*aur_plan, observation);
        if(projections.front().failure() != nullptr) {
            append_artifact_projection(
                *aur_plan, projections.front(), observation);
        } else {
            append_artifact_units(
                *aur_plan, route_artifact_targets, observation);
        }
    }
    const bool has_aur_update_artifacts =
        observation.required_artifacts.size() >
        registered_artifact_count;

    observation.status = observation.blockers.empty()
                             ? UnifiedPlanObservationStatus::Ready
                             : UnifiedPlanObservationStatus::Blocked;
    if(observation.status == UnifiedPlanObservationStatus::Ready) {
        RepositoryPackageTransactionIntent repository_transaction;
        repository_transaction.policy.needed =
            system_source.snapshot().options.needed;
        repository_transaction.targets.push_back(
            RepositorySystemUpgradeIntent{});
        if(registered_plan != nullptr) {
            append_repository_dependency_intents(
                *registered_plan, repository_transaction);
        }
        if(aur_plan != nullptr) {
            append_repository_dependency_intents(
                *aur_plan, repository_transaction);
        }
        observation.transaction_intents.push_back(
            std::move(repository_transaction));
        append_source_artifact_transaction(
            observation, system_source.snapshot().options.needed);
    }
    append_upgrade_all_phases(
        prepared, aur_query, aur_plan,
        registered_artifact_count != 0 &&
            observation.status == UnifiedPlanObservationStatus::Ready,
        has_aur_update_artifacts &&
            observation.status == UnifiedPlanObservationStatus::Ready,
        observation);
    return UnifiedPlanProjection::make(
        std::move(projections), std::move(route_artifact_targets),
        std::move(observation));
}

std::unique_ptr<UnifiedPlanProjection> project_upgrade_all_unified_plan(
    const UpgradeAllOperationProjectionAuthority& prepared,
    const PreparedUpgradeAllAurPreflight& aur_preflight,
    const std::vector<RegisteredAurDevelObservation>* registered_devel) {
    if(const PreparedFilteredAurUpdateOperation* filtered =
           aur_preflight.filtered_operation();
       filtered != nullptr) {
        if(!filtered->source_build_preparation().has_value()) {
            reject_inconsistent_input(
                "Prepared upgrade-all filtered AUR operation lacks source preparation.");
        }
        return project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(prepared),
                std::cref(filtered->original_query_result()),
                std::cref(filtered->execution_preflight()),
                std::cref(aur_preflight.issues()),
                std::cref(
                    filtered->source_build_preparation().value()),
                std::cref(aur_preflight), registered_devel});
    }
    return project_upgrade_all_unified_plan(
        UpgradeAllUnifiedPlanProjectionInput{
            std::cref(prepared), std::nullopt, std::nullopt,
            std::cref(aur_preflight.issues()), std::nullopt,
            std::cref(aur_preflight), registered_devel});
}

std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
project_system_aur_update_unified_plan(
    const SystemAurUpdateDryRunObservation& combined) {
    if(combined.request.ordered_pacman_args().empty()) {
        reject_inconsistent_input(
            "System/AUR dry-run observation has no repository request.");
    }

    UnifiedPlanObservationInput repository_observation;
    repository_observation.status = UnifiedPlanObservationStatus::Ready;
    RepositoryPackageTransactionIntent repository_transaction;
    repository_transaction.policy.needed =
        combined.request.repository_needed();
    repository_transaction.stage =
        UnifiedPlanTransactionIntentStage::RepositorySystemUpgrade;
    repository_transaction.targets.push_back(
        RepositorySystemUpgradeIntent{});
    repository_observation.transaction_intents.push_back(
        std::move(repository_transaction));
    append_phase(
        repository_observation,
        UnifiedPlanObservationPhase::RequestDiscovery,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        repository_observation,
        UnifiedPlanObservationPhase::ExecutionProjection,
        UnifiedPlanAuthorityOwner::Moguet);
    append_phase(
        repository_observation,
        UnifiedPlanObservationPhase::RepositoryTransaction,
        UnifiedPlanAuthorityOwner::Pacman);
    std::unique_ptr<UnifiedPlanProjection> repository_projection =
        UnifiedPlanProjection::make(
            {}, std::move(repository_observation));

    if(combined.request.mode() ==
       SystemAurUpdateDryRunMode::RepoOnly) {
        if(combined.request.ordered_pacman_args().empty() ||
           !combined.issues.empty() ||
           combined.aur_observation_basis.has_value() ||
           combined.actual_authority_refresh.has_value() ||
           combined.explicit_source_satisfaction.has_value() ||
           combined.saved_source_preference_policy.has_value() ||
           combined.devel_requires_check_policy.has_value() ||
           combined.repository_configuration.has_value() ||
           !combined.foreign_inventory.empty() ||
           combined.aur_observation.has_value()) {
            reject_inconsistent_input(
                "Repo-only system update observation retained AUR authority.");
        }
        return std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>(
            new SystemAurUpdateUnifiedPlanProjection(
                SystemAurUpdateUnifiedPlanStatus::Ready,
                SystemAurUpdateUnifiedPlanMode::RepoOnly,
                {SystemAurUpdateUnifiedPlanPhase::
                     RepositorySystemTransactionIntent},
                std::move(repository_projection), nullptr,
                {},
                std::nullopt, std::nullopt, std::nullopt));
    }

    if(combined.aur_observation_basis !=
           std::optional<SystemAurUpdateDryRunAurObservationBasis>{
               SystemAurUpdateDryRunAurObservationBasis::
                   CurrentInstalledState} ||
       combined.actual_authority_refresh !=
           std::optional<SystemAurUpdateDryRunActualAuthorityRefresh>{
               SystemAurUpdateDryRunActualAuthorityRefresh::
                   AfterRepositorySuccess} ||
       !combined.explicit_source_satisfaction.has_value() ||
       combined.saved_source_preference_policy !=
           std::optional<SavedSourcePreferencePolicy>{
               SavedSourcePreferencePolicy::Ignore} ||
       combined.devel_requires_check_policy !=
           std::optional<DevelRequiresCheckPolicy>{
               DevelRequiresCheckPolicy::SkipIndependentTarget} ||
       (combined.aur_observation.has_value() &&
        (!combined.repository_configuration.has_value() ||
         !combined.aur_observation
              ->devel_requires_check_policy.has_value() ||
         !is_known_devel_requires_check_policy(
             *combined.aur_observation
                  ->devel_requires_check_policy) ||
         combined.aur_observation
                 ->devel_requires_check_policy !=
             combined.devel_requires_check_policy ||
         combined.aur_observation->preflight
                 .devel_requires_check_policy !=
             combined.devel_requires_check_policy ||
         !has_valid_aur_update_execution_policy_snapshot(
             combined.aur_observation->preflight) ||
         !combined.aur_observation
              ->source_build_observation.has_value() ||
         combined.aur_observation
                 ->source_build_observation
                 ->devel_requires_check_policy !=
             combined.devel_requires_check_policy))) {
        reject_inconsistent_input(
            "System/AUR dry-run observation has invalid freshness or source policy.");
    }

    UnifiedPlanObservationInput aur_observation;
    aur_observation.route_preflight_authorities.push_back(
        UnifiedPlanBorrowedAuthorityReference<
            SystemAurUpdateDryRunObservation>(combined));
    for(const SystemAurUpdateDryRunIssue& issue : combined.issues) {
        aur_observation.blockers.push_back(
            RoutePreflightUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    SystemAurUpdateDryRunIssue>(issue)});
    }

    std::vector<BuildPlanArtifactTargetProjectionResult> projections;
    std::vector<ProjectedBuildPlanArtifactTargets> route_artifact_targets;
    std::vector<SystemAurUpdateRequiresCheckAttention>
        requires_check_attentions;
    if(combined.aur_observation.has_value()) {
        const FilteredAurUpdateObservation& filtered =
            *combined.aur_observation;
        if(!filtered.source_build_preflight().has_value()) {
            reject_inconsistent_input(
                "System/AUR filtered observation lacks source-build safety authority.");
        }
        const AurUpdateQueryResult& query =
            filtered.original_query_result();
        if(combined.foreign_inventory.size() !=
           query.plan.entries.size()) {
            reject_inconsistent_input(
                "System/AUR foreign inventory and query counts differ.");
        }
        for(std::size_t index = 0;
            index < combined.foreign_inventory.size(); ++index) {
            const InstalledPackageMetadata& installed =
                combined.foreign_inventory[index];
            const AurUpdatePlanEntry& entry = query.plan.entries[index];
            if(installed.name != entry.installed_name ||
               installed.version != entry.installed_version ||
               installed.reason != entry.install_reason) {
                reject_inconsistent_input(
                    "System/AUR foreign inventory and query identities differ.");
            }
        }
        const AurUpdateExecutionPreflight& preflight =
            filtered.execution_preflight();
        requires_check_attentions =
            project_system_aur_requires_check_attentions(preflight);
        const std::vector<std::string>* repository_order =
            validate_aur_update_input(query, preflight);
        if(repository_order != nullptr &&
           *repository_order !=
               combined.repository_configuration->repository_names) {
            reject_inconsistent_input(
                "System/AUR current inventory and AUR plan used different repository configurations.");
        }
        if(repository_order != nullptr) {
            aur_observation.configured_repository_order.emplace(
                std::cref(*repository_order));
        }
        aur_observation.route_preflight_authorities.push_back(
            UnifiedPlanBorrowedAuthorityReference<
                FilteredAurUpdateObservation>(filtered));
        append_aur_update_roots_and_blockers(
            query, preflight, aur_observation);
        for(const FilteredAurUpdateOperationIssue& issue :
            filtered.operation_issues()) {
            aur_observation.blockers.push_back(
                RoutePreflightUnifiedPlanBlocker{
                    UnifiedPlanBorrowedAuthorityReference<
                        FilteredAurUpdateOperationIssue>(issue)});
        }
        append_aur_update_observation_blockers(
            *filtered.source_build_preflight(), preflight,
            aur_observation);

        const BuildPlan* plan = preflight.build_plan.has_value()
                                    ? &*preflight.build_plan
                                    : nullptr;
        if(plan != nullptr) {
            projections.push_back(
                project_build_plan_required_artifact_targets(*plan));
            aur_observation.dependency_authorities.push_back(
                UnifiedPlanDependencyAuthorityReference::from_build_plan(
                    *plan));
            append_build_plan_blockers(*plan, aur_observation);
            if(projections.front().failure() != nullptr) {
                append_artifact_projection(
                    *plan, projections.front(), aur_observation);
            } else if(all_build_plan_roots_are_executable(preflight)) {
                route_artifact_targets =
                    project_aur_update_artifact_targets(
                        preflight, projections.front());
                append_artifact_units(
                    *plan, route_artifact_targets, aur_observation);
            }
        }

        if(!aur_observation.blockers.empty()) {
            aur_observation.status = UnifiedPlanObservationStatus::Blocked;
        } else if(!has_executable_update_targets(preflight)) {
            if(!source_observation_matches_noop(
                   *filtered.source_build_preflight())) {
                reject_inconsistent_input(
                    "System/AUR no-update source observation is inconsistent.");
            }
            aur_observation.status = UnifiedPlanObservationStatus::NoOp;
        } else {
            if(!source_observation_matches_ready_preflight(
                   *filtered.source_build_preflight(), preflight)) {
                reject_inconsistent_input(
                    "System/AUR executable source observation is inconsistent.");
            }
            aur_observation.status = UnifiedPlanObservationStatus::Ready;
            RepositoryPackageTransactionIntent later_repository_transaction;
            later_repository_transaction.stage =
                UnifiedPlanTransactionIntentStage::LaterNormalAur;
            if(plan != nullptr) {
                append_repository_dependency_intents(
                    *plan, later_repository_transaction);
            }
            if(!later_repository_transaction.targets.empty()) {
                aur_observation.transaction_intents.push_back(
                    std::move(later_repository_transaction));
            }
            append_source_artifact_transaction(
                aur_observation, false,
                UnifiedPlanTransactionIntentStage::LaterNormalAur);
        }
        append_aur_update_phases(query, plan, aur_observation);
    } else {
        if(combined.issues.empty()) {
            reject_inconsistent_input(
                "System/AUR observation lost AUR authority without a typed failure.");
        }
        aur_observation.status = UnifiedPlanObservationStatus::Blocked;
        append_phase(
            aur_observation,
            UnifiedPlanObservationPhase::RequestDiscovery,
            UnifiedPlanAuthorityOwner::Moguet);
        append_phase(
            aur_observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Libalpm);
        append_phase(
            aur_observation,
            UnifiedPlanObservationPhase::ExecutionProjection,
            UnifiedPlanAuthorityOwner::Moguet);
    }

    if(aur_observation.status == UnifiedPlanObservationStatus::Blocked &&
       aur_observation.blockers.empty()) {
        reject_inconsistent_input(
            "System/AUR dry-run status differs from its AUR child authority.");
    }

    std::unique_ptr<UnifiedPlanProjection> aur_projection =
        UnifiedPlanProjection::make(
            std::move(projections),
            std::move(route_artifact_targets),
            std::move(aur_observation));
    if(aur_projection->observation_result().observation() == nullptr) {
        reject_inconsistent_input(
            "System/AUR projection produced an invalid AUR child.");
    }
    const SystemAurUpdateUnifiedPlanStatus status =
        aur_projection->observation_result().observation()->status() ==
                UnifiedPlanObservationStatus::Blocked
            ? SystemAurUpdateUnifiedPlanStatus::Blocked
            : SystemAurUpdateUnifiedPlanStatus::Ready;
    return std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>(
        new SystemAurUpdateUnifiedPlanProjection(
            status, SystemAurUpdateUnifiedPlanMode::Auto,
            {SystemAurUpdateUnifiedPlanPhase::
                 RepositorySystemTransactionIntent,
             SystemAurUpdateUnifiedPlanPhase::
                 CurrentForeignInventoryObservation,
             SystemAurUpdateUnifiedPlanPhase::
                 CurrentNormalAurAssessment,
             SystemAurUpdateUnifiedPlanPhase::
                 PotentialLaterAurTransactions},
            std::move(repository_projection),
            std::move(aur_projection),
            std::move(requires_check_attentions),
            SystemAurUpdateUnifiedPlanFreshness::CurrentInstalledState,
            SystemAurUpdateUnifiedPlanActualRefresh::
                AfterRepositorySuccess,
            SystemAurUpdateUnifiedPlanTransactionRelationship::
                SeparateSequentialTransactions));
}
