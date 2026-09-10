#include "invocation_owned_cleanup_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef MOGUET_ENABLE_REMOTE_AUR_CLEANUP_COLLECTOR_STUB
namespace {

void add_issue(
    std::vector<RemoteAurCleanupCollectionIssueKind>& issues,
    RemoteAurCleanupCollectionIssueKind issue) {
    if(std::find(issues.begin(), issues.end(), issue) == issues.end()) {
        issues.push_back(issue);
    }
}

const PlannedPackageTarget* find_unique_target(
    const BuildPlan& plan,
    const std::string& package_name,
    const std::string& package_base) noexcept {
    const PlannedPackageTarget* match = nullptr;
    for(const PlannedPackageTarget& target : plan.package_targets) {
        if(target.package_name != package_name ||
           target.package_base != package_base) {
            continue;
        }
        if(match != nullptr) return nullptr;
        match = &target;
    }
    return match;
}

bool is_build_or_check(PackageRole role) noexcept {
    return role == PackageRole::BuildDependency ||
           role == PackageRole::CheckDependency;
}

bool same_correlation(
    const CleanupPackageCorrelation& lhs,
    const CleanupPackageCorrelation& rhs) noexcept {
    if(lhs.requested_root != rhs.requested_root ||
       lhs.package != rhs.package || lhs.role != rhs.role ||
       lhs.verification != rhs.verification ||
       lhs.dependency_edge.has_value() !=
           rhs.dependency_edge.has_value()) {
        return false;
    }
    if(!lhs.dependency_edge.has_value()) return true;
    const CleanupDependencyEdgeCorrelation& lhs_edge =
        lhs.dependency_edge.value();
    const CleanupDependencyEdgeCorrelation& rhs_edge =
        rhs.dependency_edge.value();
    return lhs_edge.build_plan_edge_index ==
               rhs_edge.build_plan_edge_index &&
           lhs_edge.requiring_package == rhs_edge.requiring_package &&
           lhs_edge.requirement == rhs_edge.requirement &&
           lhs_edge.provider == rhs_edge.provider;
}

struct CandidateOrigin {
    SourceAwarePackageIdentity package;
    std::vector<CleanupPackageCorrelation> correlations;
    bool conflicting_identity = false;
};

void merge_candidate_origin(
    std::vector<CandidateOrigin>& origins,
    SourceAwarePackageIdentity package,
    std::vector<CleanupPackageCorrelation> correlations,
    std::vector<RemoteAurCleanupCollectionIssueKind>& issues) {
    const std::string& package_name = package.package().package_name();
    auto existing = std::find_if(
        origins.begin(), origins.end(),
        [&package_name](const CandidateOrigin& candidate) {
            return candidate.package.package().package_name() ==
                   package_name;
        });
    if(existing == origins.end()) {
        origins.push_back(CandidateOrigin{
            std::move(package), std::move(correlations), false});
        return;
    }
    if(existing->package != package) {
        existing->conflicting_identity = true;
        add_issue(
            issues,
            RemoteAurCleanupCollectionIssueKind::
                CandidateIdentityConflict);
        return;
    }
    for(CleanupPackageCorrelation& correlation : correlations) {
        if(std::none_of(
               existing->correlations.begin(),
               existing->correlations.end(),
               [&correlation](const CleanupPackageCorrelation& current) {
                   return same_correlation(current, correlation);
               })) {
            existing->correlations.push_back(std::move(correlation));
        }
    }
}

std::optional<PackageChildIdentity> requiring_package_identity(
    const PreparedRemoteSourceBuild& prepared,
    std::size_t work_item_index,
    const BuildPlanDependencyEdge& edge) {
    if(work_item_index >= prepared.invocation.work_items.size()) {
        return std::nullopt;
    }
    const ProductionSourceBuildWorkItem& work_item =
        prepared.invocation.work_items[work_item_index];
    if(!work_item.request.aur_review_identity.has_value() ||
       work_item.request.checkout_name != edge.parent_package_base ||
       work_item.request.aur_review_identity->package_base() !=
           edge.parent_package_base) {
        return std::nullopt;
    }
    try {
        return PackageChildIdentity::make(
            work_item.request.aur_review_identity.value(),
            edge.parent_package_name);
    } catch(const std::invalid_argument&) {
        return std::nullopt;
    }
}

} // namespace
#endif

RemoteAurCleanupInvocationExecutionError::
    RemoteAurCleanupInvocationExecutionError(
        ProductionSourceBuildInvocationResult result,
        SelectedRepositoryProviderTransactionResult provider_transaction,
        const std::string& diagnostic)
    : std::runtime_error(diagnostic), result_(std::move(result)),
      provider_transaction_(std::move(provider_transaction)) {
}

const ProductionSourceBuildInvocationResult&
RemoteAurCleanupInvocationExecutionError::result() const noexcept {
    return result_;
}

const SelectedRepositoryProviderTransactionResult&
RemoteAurCleanupInvocationExecutionError::provider_transaction()
    const noexcept {
    return provider_transaction_;
}

RemoteAurCleanupCollectionResult::RemoteAurCleanupCollectionResult(
    ProductionSourceBuildInvocationResult invocation_result,
    CleanupEvidenceCompleteness completeness,
    std::vector<RemoteAurCleanupCandidateAssessment> assessments,
    std::vector<RemoteAurCleanupCollectionIssueKind> issues) noexcept
    : invocation_result_(std::move(invocation_result)),
      completeness_(completeness),
      assessments_(std::move(assessments)),
      issues_(std::move(issues)) {
}

const ProductionSourceBuildInvocationResult&
RemoteAurCleanupCollectionResult::invocation_result() const noexcept {
    return invocation_result_;
}

CleanupEvidenceCompleteness
RemoteAurCleanupCollectionResult::completeness() const noexcept {
    return completeness_;
}

const std::vector<RemoteAurCleanupCandidateAssessment>&
RemoteAurCleanupCollectionResult::assessments() const noexcept {
    return assessments_;
}

const std::vector<RemoteAurCleanupCollectionIssueKind>&
RemoteAurCleanupCollectionResult::issues() const noexcept {
    return issues_;
}

bool RemoteAurCleanupCollectionResult::has_eligible_candidate()
    const noexcept {
    return std::any_of(
        assessments_.begin(), assessments_.end(),
        [](const RemoteAurCleanupCandidateAssessment& assessment) {
            return assessment.classification ==
                   CleanupClassification::Eligible;
        });
}

#ifndef MOGUET_ENABLE_REMOTE_AUR_CLEANUP_COLLECTOR_STUB
RemoteAurCleanupCandidateCollector::RemoteAurCleanupCandidateCollector(
    PreparedRemoteSourceBuild prepared)
    : session_(CleanupInvocationSession::begin_for_collector(
          std::move(prepared))) {
}

PreparedProductionSourceBuildInvocation&
RemoteAurCleanupCandidateCollector::prepared_invocation_for_execution() noexcept {
    return session_.prepared_for_execution().invocation;
}

bool RemoteAurCleanupCandidateCollector::
    should_use_trusted_source_artifact_install(
        std::size_t work_item_index) const noexcept {
    try {
        const PreparedRemoteSourceBuild& prepared = session_.prepared();
        if(!prepared.aur_build_plan.has_value() ||
           work_item_index >= prepared.invocation.work_items.size()) {
            return false;
        }
        const BuildPlan& plan = prepared.aur_build_plan.value();
        const ProductionSourceBuildWorkItem& work_item =
            prepared.invocation.work_items[work_item_index];
        if(work_item.required_targets.empty() ||
           work_item.build_plan_dependency_edge_indices.empty() ||
           std::any_of(
               work_item.required_targets.begin(),
               work_item.required_targets.end(),
               [](const RequiredPackageArtifactTarget& target) {
                   return target.desired_reason !=
                          DesiredInstallReason::Dependency;
               })) {
            return false;
        }
        for(const RequiredPackageArtifactTarget& required :
            work_item.required_targets) {
            const PlannedPackageTarget* target = find_unique_target(
                plan, required.package_name, required.package_base);
            if(target == nullptr || target->roles.empty() ||
               std::any_of(
                   target->roles.begin(), target->roles.end(),
                   [](PackageRole role) {
                       return !is_build_or_check(role);
                   })) {
                return false;
            }
        }
        return true;
    } catch(...) {
        return false;
    }
}

SelectedRepositoryProviderTransactionResult
RemoteAurCleanupCandidateCollector::
    execute_selected_repository_provider_transaction(
        const AppConfig& config) {
    SelectedRepositoryProviderTrustedReceiptExecutionResult execution =
        ::execute_selected_repository_provider_transaction(
            session_.prepared().invocation, config,
            SelectedRepositoryProviderTrustedReceiptRequest::
                capture_actual_installs(session_.authority()));
    SelectedRepositoryProviderTransactionResult operation =
        execution.transaction;
    const bool trusted_transaction_not_started =
        operation.status ==
        SelectedRepositoryProviderTransactionStatus::
            BlockedBeforeExecution;
    selected_provider_execution_ = std::move(execution);
    if(trusted_transaction_not_started) {
        // Compatibility fallback owns only the package operation. It never
        // produces cleanup evidence, and any registered/incomplete trusted
        // token keeps the later authority fail closed.
        return ::execute_selected_repository_provider_transaction(
            session_.prepared().invocation, config);
    }
    return operation;
}

PackageBaseArtifactInstallExecutionResult
RemoteAurCleanupCandidateCollector::
    execute_source_artifact_install_transaction(
        PreparedPackageBaseArtifactInstall& install,
        std::size_t work_item_index,
        const ArtifactInstallExecutionOptions& options) {
    SourceArtifactInstallTrustedExecutionResult execution =
        execute_source_artifact_install_trusted_transaction(
            install, session_.authority(), work_item_index, options);
    const SourceArtifactInstallTrustedExecutionStatus execution_status =
        execution.status();
    std::optional<PackageBaseArtifactInstallExecutionResult>
        operation_result = execution.operation_result();
    if(execution.expectation().has_value() &&
       execution.observation().has_value()) {
        SourceArtifactInstallReceiptEvidence receipt =
            establish_source_artifact_install_receipt_evidence(
                execution.expectation().value(),
                execution.observation().value());
        std::optional<SourceArtifactInstallCausalEvidence> causal =
            project_source_artifact_install_causal_evidence(receipt);
        if(causal.has_value()) {
            source_artifact_causal_evidence_.push_back(
                std::move(causal.value()));
        }
    }
    const std::optional<std::string> diagnostic = execution.diagnostic();
    source_artifact_executions_.push_back(std::move(execution));
    if(!operation_result.has_value()) {
        switch(execution_status) {
            case SourceArtifactInstallTrustedExecutionStatus::InvalidRequest:
            case SourceArtifactInstallTrustedExecutionStatus::
                TrustedExecutableUnavailable:
            case SourceArtifactInstallTrustedExecutionStatus::
                TokenGenerationFailed:
            case SourceArtifactInstallTrustedExecutionStatus::
                ArtifactSnapshotFailed:
                // The trusted one-shot capability was not consumed. Preserve
                // the existing install semantics without granting causal
                // cleanup authority to this raw fallback transaction.
                return execute_prepared_package_base_artifact_install(
                    install, options);
            case SourceArtifactInstallTrustedExecutionStatus::ArtifactSealingFailed:
            case SourceArtifactInstallTrustedExecutionStatus::PrepareFailed:
            case SourceArtifactInstallTrustedExecutionStatus::PacmanFailed:
            case SourceArtifactInstallTrustedExecutionStatus::AbortFailed:
            case SourceArtifactInstallTrustedExecutionStatus::ConsumeFailed:
            case SourceArtifactInstallTrustedExecutionStatus::
                MalformedReceipt:
            case SourceArtifactInstallTrustedExecutionStatus::Missing:
            case SourceArtifactInstallTrustedExecutionStatus::Complete:
            case SourceArtifactInstallTrustedExecutionStatus::OutcomeUnknown:
                break;
        }
        throw std::runtime_error(diagnostic.value_or(
            "Trusted source-artifact package transaction did not complete."));
    }
    return std::move(operation_result.value());
}

RemoteAurCleanupCollectionResult
RemoteAurCleanupCandidateCollector::finish(
    ProductionSourceBuildInvocationResult result) {
    std::vector<RemoteAurCleanupCollectionIssueKind> issues;
    std::vector<RemoteAurCleanupCandidateAssessment> assessments;
    CleanupEvidenceCompleteness overall =
        CleanupEvidenceCompleteness::Complete;
    if(!result.is_success() || !baseline_.has_value()) {
        add_issue(
            issues,
            RemoteAurCleanupCollectionIssueKind::
                CurrentObservationUnavailable);
        return RemoteAurCleanupCollectionResult(
            std::move(result), CleanupEvidenceCompleteness::Incomplete,
            {}, std::move(issues));
    }

    CleanupCurrentInstalledObservation current = [&]() {
        try {
            return observe_cleanup_current_after_full_supported_invocation(
                session_, result);
        } catch(...) {
            add_issue(
                issues,
                RemoteAurCleanupCollectionIssueKind::
                    CurrentObservationUnavailable);
            throw;
        }
    }();
    const CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session_, result);

    std::vector<CleanupSourceArtifactCorrelationEvidence>
        source_correlations;
    source_correlations.reserve(source_artifact_causal_evidence_.size());
    for(const SourceArtifactInstallCausalEvidence& causal :
        source_artifact_causal_evidence_) {
        source_correlations.push_back(
            correlate_source_artifact_install_to_build_plan(
                session_, lifecycle, causal));
    }

    std::vector<CleanupSelectedProviderCorrelationEvidence>
        selected_correlations;
    if(selected_provider_execution_.has_value() &&
       selected_provider_execution_->trusted_execution_evidence()
           .has_value()) {
        selected_correlations.push_back(
            correlate_selected_repository_provider_to_build_plan(
                session_, lifecycle, current,
                selected_provider_execution_->trusted_execution_evidence()
                    .value()));
    }

    std::vector<CandidateOrigin> origins;
    for(const CleanupSourceArtifactCorrelationEvidence& evidence :
        source_correlations) {
        for(const CleanupSourceArtifactSelectedCorrelation& selected :
            evidence.selected_artifacts()) {
            merge_candidate_origin(
                origins, selected.artifact.expected_identity,
                selected.dependency_correlations, issues);
        }
    }

    const PreparedRemoteSourceBuild& prepared = session_.prepared();
    const BuildPlan& plan = prepared.aur_build_plan.value();
    for(const CleanupSelectedProviderCorrelationEvidence& evidence :
        selected_correlations) {
        for(const CleanupSelectedProviderEdgeCorrelation& selected :
            evidence.edge_correlations()) {
            if(selected.build_plan_edge_index >=
               plan.dependency_edges.size()) {
                add_issue(
                    issues,
                    RemoteAurCleanupCollectionIssueKind::
                        CandidateCorrelationIncomplete);
                continue;
            }
            const BuildPlanDependencyEdge& edge =
                plan.dependency_edges[selected.build_plan_edge_index];
            const PlannedPackageTarget* parent = find_unique_target(
                plan, edge.parent_package_name,
                edge.parent_package_base);
            const std::optional<PackageChildIdentity> requiring =
                requiring_package_identity(
                    prepared, selected.work_item_index, edge);
            if(parent == nullptr || parent->roots.empty() ||
               !requiring.has_value() || !edge.requirement.has_value()) {
                add_issue(
                    issues,
                    RemoteAurCleanupCollectionIssueKind::
                        CandidateCorrelationIncomplete);
                continue;
            }
            std::vector<CleanupPackageCorrelation> correlations;
            correlations.reserve(parent->roots.size());
            for(const RootTargetIdentity& root : parent->roots) {
                correlations.push_back(CleanupPackageCorrelation{
                    root,
                    selected.package_identity.package(),
                    edge.role,
                    CleanupDependencyEdgeCorrelation{
                        selected.build_plan_edge_index,
                        requiring.value(),
                        edge.requirement.value(),
                        CleanupProviderCorrelation{
                            selected.provider,
                            edge.provider_resolution}},
                    CleanupEvidenceVerification::Verified});
            }
            merge_candidate_origin(
                origins, selected.package_identity,
                std::move(correlations), issues);
        }
    }

    if(origins.empty()) {
        add_issue(
            issues,
            RemoteAurCleanupCollectionIssueKind::CandidateOriginMissing);
        return RemoteAurCleanupCollectionResult(
            std::move(result), CleanupEvidenceCompleteness::Incomplete,
            {}, std::move(issues));
    }

    for(CandidateOrigin& origin : origins) {
        if(origin.conflicting_identity) {
            assessments.push_back(RemoteAurCleanupCandidateAssessment{
                std::move(origin.package),
                CleanupClassification::Invalid,
                {CleanupClassificationReason::InvalidTypedState}});
            overall = CleanupEvidenceCompleteness::Incomplete;
            continue;
        }

        CleanupPolicyObservation policy = [&]() {
            try {
                return observe_cleanup_policy_after_full_supported_invocation(
                    session_, result,
                    origin.package.package().package_name());
            } catch(...) {
                add_issue(
                    issues,
                    RemoteAurCleanupCollectionIssueKind::
                        PolicyObservationUnavailable);
                throw;
            }
        }();
        CleanupInvocationEvidence aggregate =
            aggregate_remote_aur_cleanup_invocation_evidence(
                session_, lifecycle, baseline_.value(), current,
                policy, source_correlations, selected_correlations);
        const bool aggregate_complete =
            aggregate.route_authority() ==
                CleanupRouteAuthority::Complete &&
            aggregate.completeness() ==
                CleanupEvidenceCompleteness::Complete &&
            aggregate.issues().empty();
        if(!aggregate_complete) {
            add_issue(
                issues,
                RemoteAurCleanupCollectionIssueKind::
                    InvocationAggregateIncomplete);
            overall = CleanupEvidenceCompleteness::Incomplete;
        }

        InvocationOwnedCleanupCandidate candidate{
            origin.package,
            project_cleanup_baseline_observation(
                baseline_->snapshot(),
                origin.package.package().package_name()),
            project_cleanup_current_package_evidence(
                current.snapshot(),
                origin.package.package().package_name()),
            CleanupCausalOwnership::InvocationOwned,
            project_shared_requirement(aggregate, origin.package),
            project_cleanup_policy_protection(policy.evidence()),
            aggregate_complete && !origin.correlations.empty()
                ? CleanupCorrelationCoverage::Complete
                : CleanupCorrelationCoverage::Incomplete,
            std::move(origin.correlations)};
        CleanupClassificationResult classification =
            classify_invocation_owned_cleanup(candidate);
        if(classification.classification() ==
               CleanupClassification::Unknown ||
           classification.classification() ==
               CleanupClassification::Invalid) {
            overall = CleanupEvidenceCompleteness::Incomplete;
        }
        assessments.push_back(RemoteAurCleanupCandidateAssessment{
            std::move(origin.package),
            classification.classification(),
            classification.reasons()});
    }

    return RemoteAurCleanupCollectionResult(
        std::move(result), overall, std::move(assessments),
        std::move(issues));
}

#ifdef MOGUET_ENABLE_REMOTE_AUR_CLEANUP_COLLECTOR_TEST_HOOKS
CleanupInvocationSession&
RemoteAurCleanupCandidateCollector::session_for_test() noexcept {
    return session_;
}

const PreparedRemoteSourceBuild&
RemoteAurCleanupCandidateCollector::prepared_for_test() const noexcept {
    return session_.prepared();
}

void RemoteAurCleanupCandidateCollector::
    retain_source_artifact_causal_evidence_for_test(
        SourceArtifactInstallCausalEvidence evidence) {
    source_artifact_causal_evidence_.push_back(std::move(evidence));
}

void RemoteAurCleanupCandidateCollector::
    retain_selected_provider_execution_for_test(
        SelectedRepositoryProviderTrustedReceiptExecutionResult execution) {
    selected_provider_execution_ = std::move(execution);
}
#endif

RemoteAurCleanupCollectionResult collect_remote_aur_cleanup_candidates(
    PreparedRemoteSourceBuild prepared,
    const AppConfig& config) {
    RemoteAurCleanupCandidateCollector collector(std::move(prepared));
    collector.baseline_.emplace(
        observe_cleanup_baseline_before_first_dependency_mutation(
            collector.session_));
    ProductionSourceBuildInvocationResult result =
        execute_prepared_remote_aur_cleanup_invocation(collector, config);
    try {
        return collector.finish(result);
    } catch(...) {
        return RemoteAurCleanupCollectionResult(
            std::move(result), CleanupEvidenceCompleteness::Incomplete,
            {}, {RemoteAurCleanupCollectionIssueKind::CurrentObservationUnavailable});
    }
}
#endif
