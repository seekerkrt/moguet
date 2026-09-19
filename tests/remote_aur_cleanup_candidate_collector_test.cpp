#include "app_config.hpp"
#include "build_plan_artifact_target_projection.hpp"
#include "invocation_owned_cleanup_adapter.hpp"
#include "stubs/package-metadata/alpm_stub.hpp"

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace metadata_stub = package_metadata_test_stub;

bool ProductionSourceBuildInvocationResult::is_success() const noexcept {
    return std::all_of(
        work_items.begin(), work_items.end(),
        [](const ProductionSourceBuildWorkItemOutcome& outcome) {
            return outcome.status ==
                   ProductionSourceBuildWorkItemStatus::Succeeded;
        });
}

SelectedRepositoryProviderTrustedReceiptExecutionResult::
    SelectedRepositoryProviderTrustedReceiptExecutionResult(
        SelectedRepositoryProviderTransactionResult transaction_value,
        std::optional<TrustedAlpmReceiptCaptureResult>
            receipt_capture_value) noexcept
    : transaction(std::move(transaction_value)),
      receipt_capture(std::move(receipt_capture_value)) {
}

const std::optional<SelectedRepositoryProviderTrustedExecutionEvidence>&
SelectedRepositoryProviderTrustedReceiptExecutionResult::
    trusted_execution_evidence() const noexcept {
    return trusted_execution_evidence_;
}

namespace selected_provider_fallback_stub {

std::optional<SelectedRepositoryProviderTrustedReceiptExecutionResult>
    trusted_result;
SelectedRepositoryProviderTransactionResult legacy_result;
std::optional<SelectedRepositoryProviderTransactionResult>
    observed_operation;
std::size_t trusted_call_count = 0;
std::size_t legacy_call_count = 0;

void reset(
    SelectedRepositoryProviderTrustedReceiptExecutionResult execution,
    SelectedRepositoryProviderTransactionResult fallback) {
    trusted_result = std::move(execution);
    legacy_result = std::move(fallback);
    observed_operation.reset();
    trusted_call_count = 0;
    legacy_call_count = 0;
}

} // namespace selected_provider_fallback_stub

SelectedRepositoryProviderTrustedReceiptExecutionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig&,
    SelectedRepositoryProviderTrustedReceiptRequest) {
    using namespace selected_provider_fallback_stub;
    ++trusted_call_count;
    if(!trusted_result.has_value()) {
        throw std::logic_error(
            "selected-provider trusted fallback stub has no result");
    }
    if(trusted_result->transaction.selected_providers !=
       invocation.selected_repository_providers) {
        throw std::logic_error(
            "selected-provider trusted fallback stub received an incoherent invocation");
    }
    SelectedRepositoryProviderTrustedReceiptExecutionResult result =
        std::move(trusted_result.value());
    trusted_result.reset();
    return result;
}

SelectedRepositoryProviderTransactionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig&) {
    using namespace selected_provider_fallback_stub;
    ++legacy_call_count;
    if(legacy_result.selected_providers !=
       invocation.selected_repository_providers) {
        throw std::logic_error(
            "selected-provider legacy fallback stub received an incoherent invocation");
    }
    return legacy_result;
}

namespace {

enum class CollectorScenario {
    Positive,
    PreExisting,
    CurrentExplicit,
    CurrentVersionMismatch,
    CurrentPackageBaseMismatch,
    CurrentArchitectureMismatch,
    CurrentQueryFailure,
    PolicyProtected,
    PolicyUnknown,
    RuntimeConsumer,
    MissingCorrelation,
    ForeignToken,
    CrossSessionEvidence,
    SolverIntroducedInstall,
    ReceiptMissing,
    FailedLaterWorkItem,
    UnattemptedLaterWorkItem,
    SelectedProviderPostLaunchUnknown,
    SelectedProviderPreLaunchFailure,
    CurrentReasonUnknown,
    CurrentIdentityUnknown,
    BaselineQueryFailure,
    UnrelatedNewPackage,
    NoNewDependency,
    ProviderMismatch,
    AmbiguousProvider,
    ProviderMetadataUnknown,
    IncompleteConsumer,
    IncompleteRegisteredTransaction,
    ContradictorySuccess,
    PreparedProviderMismatch,
};

enum class RepositoryCandidateKind {
    None,
    DirectBuild,
    DirectCheck,
    UniqueProvider,
    SelectedProvider,
};

RepositoryCandidateKind g_repository_candidate = RepositoryCandidateKind::None;

CollectorScenario g_scenario = CollectorScenario::Positive;

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

RootTargetIdentity root() {
    return RootTargetIdentity{0, "collector-root"};
}

DependencyRequirement requirement(const std::string& package_name) {
    return ConsumerDependencyRequirement(
        package_name, package_name, std::nullopt);
}

PackageBaseIdentity aur_package_base(const std::string& package_base) {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                "https://aur.archlinux.org/" + package_base + ".git")),
        package_base);
}

ProvidedDependency selected_repository_provider() {
    ProviderCapability capability(
        "collector-selected-api", "collector-selected-api",
        std::nullopt);
    return ProvidedDependency::from_repository_constraint_metadata(
        "core", 0, "collector-selected-provider",
        "collector-selected-provider", "x86_64",
        ProviderConstraintMetadata{
            capability,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                "1.0-1"),
            ObservedVersion::unknown(
                ObservedVersionSource::RepositoryProviderCapability,
                ObservedVersionUnknownReason::
                    UnversionedProviderCapability)});
}

BuildPlan collector_plan(bool with_later_work_item = false) {
    BuildPlan plan;
    plan.configured_repository_order = std::vector<std::string>{"core"};
    const RootTargetIdentity requested_root = root();
    plan.root_targets.push_back(requested_root);
    const PackageRole candidate_role =
        g_scenario == CollectorScenario::RuntimeConsumer
            ? PackageRole::RuntimeDependency
            : PackageRole::BuildDependency;
    plan.package_targets.push_back(PlannedPackageTarget{
        "collector-dependency",
        "collector-dependency-base",
        {candidate_role},
        {requested_root}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "collector-root",
        "collector-root-base",
        {PackageRole::Root},
        {requested_root}});
    plan.order.push_back(BuildPlanEntry{
        "collector-dependency-base", {"collector-dependency"}});
    plan.order.push_back(BuildPlanEntry{
        "collector-root-base", {"collector-root"}});
    if(with_later_work_item) {
        const RootTargetIdentity later_root{1, "collector-later"};
        plan.root_targets.push_back(later_root);
        plan.package_targets.push_back(PlannedPackageTarget{
            "collector-later",
            "collector-later-base",
            {PackageRole::Root},
            {later_root}});
        plan.order.push_back(BuildPlanEntry{
            "collector-later-base", {"collector-later"}});
    }

    BuildPlanDependencyEdge edge;
    edge.parent_package_name = "collector-root";
    edge.parent_package_base = "collector-root-base";
    edge.dependency_spec = "collector-dependency";
    edge.role = candidate_role;
    edge.kind = DependencyKind::Aur;
    edge.resolved_package_name = "collector-dependency";
    edge.resolved_package_base = "collector-dependency-base";
    edge.requirement = requirement("collector-dependency");
    edge.resolved_candidate = AurResolvedDependencyCandidate{
        "collector-dependency",
        "collector-dependency-base",
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage, "1.0-1")};
    edge.constraint_evaluation = ConstraintEvaluation::satisfied();
    if(g_repository_candidate != RepositoryCandidateKind::None) {
        plan.package_targets.erase(plan.package_targets.begin());
        plan.order.erase(plan.order.begin());
        if(g_repository_candidate == RepositoryCandidateKind::DirectCheck &&
           g_scenario != CollectorScenario::RuntimeConsumer) {
            edge.role = PackageRole::CheckDependency;
        }
        if(g_repository_candidate == RepositoryCandidateKind::UniqueProvider ||
           g_repository_candidate == RepositoryCandidateKind::SelectedProvider) {
            ProvidedDependency provider = ProvidedDependency::from_repository_constraint_metadata(
                "core", 0, "collector-dependency", "collector-dependency-base", "x86_64",
                ProviderConstraintMetadata{
                    ProviderCapability("collector-api", "collector-api", std::nullopt),
                    ObservedVersion::available(ObservedVersionSource::RepositoryExactPackage, "1.0-1"),
                    ObservedVersion::unknown(ObservedVersionSource::RepositoryProviderCapability,
                                             ObservedVersionUnknownReason::UnversionedProviderCapability)});
            edge.kind = DependencyKind::Provided;
            edge.dependency_spec = "collector-api";
            edge.requirement = requirement("collector-api");
            edge.provider_resolution = g_repository_candidate == RepositoryCandidateKind::SelectedProvider
                                           ? ProviderResolutionKind::UserSelected
                                           : ProviderResolutionKind::Unique;
            edge.resolved_provider = provider;
            edge.resolved_candidate = ProviderResolvedDependencyCandidate{provider, provider.constraint_metadata->provided_version};
            plan.provided.push_back(BuildPlanProvidedDependency{edge.dependency_spec, provider, edge.provider_resolution});
        } else {
            edge.kind = DependencyKind::Repo;
            edge.resolved_candidate = RepositoryExactPackage{
                ConfiguredRepositoryIdentity{"core", 0}, "collector-dependency", "collector-dependency-base", ObservedVersion::available(ObservedVersionSource::RepositoryExactPackage, "1.0-1"), {}, "x86_64"};
        }
    }
    plan.dependency_edges.push_back(std::move(edge));
    if(g_scenario ==
           CollectorScenario::SelectedProviderPostLaunchUnknown ||
       g_scenario ==
           CollectorScenario::SelectedProviderPreLaunchFailure) {
        const ProvidedDependency provider =
            selected_repository_provider();
        BuildPlanDependencyEdge provider_edge;
        provider_edge.parent_package_name = "collector-root";
        provider_edge.parent_package_base = "collector-root-base";
        provider_edge.dependency_spec = "collector-selected-api";
        provider_edge.role = PackageRole::BuildDependency;
        provider_edge.kind = DependencyKind::Provided;
        provider_edge.resolved_package_name = provider.package_name;
        provider_edge.resolved_provider = provider;
        provider_edge.provider_resolution =
            ProviderResolutionKind::UserSelected;
        provider_edge.requirement =
            requirement("collector-selected-api");
        provider_edge.resolved_candidate =
            ProviderResolvedDependencyCandidate{
                provider,
                provider.constraint_metadata->provided_version};
        provider_edge.constraint_evaluation =
            ConstraintEvaluation::satisfied();
        plan.dependency_edges.push_back(std::move(provider_edge));
        plan.provided.push_back(BuildPlanProvidedDependency{
            "collector-selected-api", provider,
            ProviderResolutionKind::UserSelected});
    }
    return plan;
}

PreparedProductionSourceBuildInvocation prepared_invocation(
    const BuildPlan& plan) {
    BuildPlanArtifactTargetProjectionResult projection =
        project_build_plan_required_artifact_targets(plan);
    const BuildPlanArtifactTargetProjectionSuccess* success =
        projection.success();
    if(success == nullptr) {
        throw std::runtime_error(
            "collector plan did not project artifact targets");
    }
    std::vector<ProductionSourceBuildWorkItem> work_items;
    for(const ProjectedBuildPlanArtifactTargets& unit :
        success->build_units) {
        ProductionSourceBuildWorkItem work_item;
        work_item.request.checkout_name = unit.package_base;
        work_item.request.git_url =
            "https://aur.archlinux.org/" + unit.package_base + ".git";
        work_item.request.aur_review_identity =
            aur_package_base(unit.package_base);
        if(unit.required_targets.size() == 1) {
            work_item.request.package_name =
                unit.required_targets.front().package_name;
        }
        work_item.required_targets = unit.required_targets;
        work_item.required_target_provenance =
            RequiredTargetProvenance::AurBuildPlanProjection;
        work_item.artifact_lifecycle_intent =
            ArtifactLifecycleIntent::PackageBaseSet;
        for(std::size_t edge_index = 0;
            edge_index < plan.dependency_edges.size(); ++edge_index) {
            const BuildPlanDependencyEdge& edge =
                plan.dependency_edges[edge_index];
            if(edge.kind == DependencyKind::Provided &&
               edge.provider_resolution ==
                   ProviderResolutionKind::UserSelected &&
               edge.parent_package_base == unit.package_base &&
               edge.resolved_provider.has_value() &&
               std::holds_alternative<RepositoryProviderOrigin>(
                   edge.resolved_provider->origin)) {
                work_item.selected_repository_providers.push_back(
                    edge.resolved_provider.value());
                work_item.selected_repository_provider_edge_indices
                    .push_back(edge_index);
            }
            if(edge.resolved_package_name ==
                   "collector-dependency" &&
               unit.package_base == "collector-dependency-base") {
                work_item.build_plan_dependency_edge_indices.push_back(
                    edge_index);
            }
        }
        work_items.push_back(std::move(work_item));
    }
    std::vector<ProvidedDependency> selected_providers;
    if(g_scenario ==
           CollectorScenario::SelectedProviderPostLaunchUnknown ||
       g_scenario ==
           CollectorScenario::SelectedProviderPreLaunchFailure) {
        selected_providers.push_back(
            selected_repository_provider());
    }
    if(g_repository_candidate == RepositoryCandidateKind::SelectedProvider) {
        selected_providers = work_items.front().selected_repository_providers;
    }
    return PreparedProductionSourceBuildInvocation{
        std::move(work_items), std::move(selected_providers),
        PacmanDatabasePaths{"/", "/var/lib/pacman"},
        std::nullopt, std::nullopt};
}

PreparedRemoteSourceBuild prepared_remote(bool with_later_work_item = false) {
    BuildPlan plan = collector_plan(with_later_work_item);
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    if(g_scenario == CollectorScenario::PreparedProviderMismatch) {
        invocation.selected_repository_providers.front().package_name = "wrong-provider";
    }
    // Corrupt the observed plan only after preparing the valid invocation, so
    // these tests exercise collector correlation rather than fixture setup.
    if(g_scenario == CollectorScenario::IncompleteConsumer) {
        plan.dependency_edges.front().parent_package_base = "missing-consumer";
    } else if(g_scenario == CollectorScenario::MissingCorrelation &&
              g_repository_candidate != RepositoryCandidateKind::None) {
        plan.dependency_edges.front().requirement.reset();
    } else if(g_scenario == CollectorScenario::ProviderMismatch) {
        plan.dependency_edges.front().resolved_provider->package_name = "wrong-provider";
    } else if(g_scenario == CollectorScenario::AmbiguousProvider) {
        plan.dependency_edges.front().kind = DependencyKind::AmbiguousProvider;
    } else if(g_scenario == CollectorScenario::ProviderMetadataUnknown) {
        auto& provider = plan.dependency_edges.front().resolved_provider.value();
        provider.package_architecture.reset();
        std::get<ProviderResolvedDependencyCandidate>(plan.dependency_edges.front().resolved_candidate.value()).provider = provider;
        plan.provided.front().provider = provider;
    }
    return PreparedRemoteSourceBuild{
        ResolvedSourceBuildIdentity{ResolvedAurSourceBuildIdentity{
            "collector-root", "collector-root-base"}},
        std::move(plan), std::move(invocation)};
}

ProductionSourceBuildStagedOutcome successful_staged_outcome() {
    ProductionSourceBuildStagedOutcome outcome;
    outcome.build_outcome =
        ProductionSourceBuildCommandOutcome::Succeeded;
    outcome.install_outcome = ProductionSourceInstallOutcome::Succeeded;
    return outcome;
}

ProductionSourceBuildInvocationResult successful_result(
    const PreparedProductionSourceBuildInvocation& invocation) {
    ProductionSourceBuildInvocationResult result;
    for(const ProductionSourceBuildWorkItem& work_item :
        invocation.work_items) {
        ProductionSourceBuildWorkItemOutcome outcome;
        outcome.package_base = work_item.request.checkout_name;
        outcome.status = ProductionSourceBuildWorkItemStatus::Succeeded;
        outcome.production_outcome = successful_staged_outcome();
        result.work_items.push_back(std::move(outcome));
    }
    return result;
}

SourceAwarePackageIdentity candidate_identity() {
    return SourceAwarePackageIdentity::make(
        PackageChildIdentity::make(
            aur_package_base("collector-dependency-base"),
            "collector-dependency"),
        SourceRevisionIdentity::unknown(),
        PackageVersionIdentity::composite("1.0-1"),
        PackageArchitectureIdentity::known({"x86_64"}));
}

ArtifactPackageIdentity archive_identity() {
    return ArtifactPackageIdentity{
        "collector-dependency",
        "1.0-1",
        ArtifactPackageBaseIdentity::known(
            "collector-dependency-base"),
        ArtifactPackageArchitectureIdentity::known("x86_64")};
}

std::string token(char value) {
    return std::string(64, value);
}

SourceArtifactInstallCausalEvidence make_causal_evidence(
    CleanupInvocationSession& session,
    const std::string& transaction_token,
    std::vector<std::size_t> edge_indices,
    std::vector<std::string> extra_installs = {}) {
    const SourceArtifactInstallWorkItemBinding work_item{
        session.authority(), 0, "collector-dependency-base", {root()}};
    const SourceArtifactInstallExpectedSelectedArtifact expected{
        0,
        candidate_identity(),
        DesiredInstallReason::Dependency,
        {PackageRole::BuildDependency},
        {root()},
        edge_indices};
    const SourceArtifactInstallObservedSelectedArtifact observed{
        0,
        archive_identity(),
        DesiredInstallReason::Dependency,
        {PackageRole::BuildDependency},
        {root()},
        edge_indices};
    std::vector<PacmanTransactionPackageObservation> operations = {
        {PacmanTransactionPackageOperation::Install,
         "collector-dependency"}};
    for(std::string& package_name : extra_installs) {
        operations.push_back(PacmanTransactionPackageObservation{
            PacmanTransactionPackageOperation::Install,
            std::move(package_name)});
    }
    PacmanTransactionReceipt receipt =
        validate_pacman_transaction_receipt(
            transaction_token,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            PacmanTransactionReceiptObservation{
                PacmanTransactionReceiptObservationState::Complete,
                transaction_token,
                InvocationDependencyTransactionOwner::
                    SourceArtifactInstall,
                std::move(operations)});
    InvocationDependencyTransactionLedger ledger{{InvocationDependencyTransaction{
        transaction_token,
        InvocationDependencyTransactionOwner::SourceArtifactInstall,
        {"collector-dependency"},
        InvocationDependencyTransactionCommandOutcome::Succeeded,
        std::move(receipt)}}};
    SourceArtifactInstallReceiptEvidence evidence =
        establish_source_artifact_install_receipt_evidence(
            SourceArtifactInstallReceiptExpectation{
                work_item, {expected}, transaction_token},
            make_source_artifact_install_receipt_observation_for_test(
                work_item, {observed}, std::move(ledger)));
    std::optional<SourceArtifactInstallCausalEvidence> causal =
        project_source_artifact_install_causal_evidence(evidence);
    if(!causal.has_value()) {
        throw std::runtime_error(
            "collector causal fixture did not close");
    }
    return std::move(causal.value());
}

metadata_stub::LocalPackageMetadata base_devel_metadata(bool protected_case) {
    metadata_stub::LocalPackageMetadata metadata;
    metadata.name = "base-devel";
    metadata.version = "1-2";
    metadata.reason = ALPM_PKG_REASON_EXPLICIT;
    metadata.dependencies.push_back(
        metadata_stub::PackageDependencyMetadata{
            protected_case
                ? std::optional<std::string>{"collector-dependency"}
                : std::optional<std::string>{"unrelated-tool"},
            std::nullopt,
            ALPM_DEP_MOD_ANY});
    return metadata;
}

void set_current_metadata() {
    if(g_scenario == CollectorScenario::BaselineQueryFailure) {
        metadata_stub::reset_alpm_stub();
    }
    if(g_scenario == CollectorScenario::CurrentQueryFailure) {
        metadata_stub::set_package_cache_failure();
        return;
    }
    if(g_scenario == CollectorScenario::PolicyUnknown) {
        metadata_stub::LocalPackageMetadata candidate{
            "collector-dependency", "1.0-1",
            ALPM_PKG_REASON_DEPEND};
        metadata_stub::set_local_packages({candidate});
        metadata_stub::set_local_package_base(
            0, "collector-dependency-base");
        metadata_stub::set_local_package_architecture(0, "x86_64");
        metadata_stub::set_sync_database_empty_cache("core");
        metadata_stub::enqueue_local_package_query_present_metadata(
            "collector-dependency", std::move(candidate));
        metadata_stub::enqueue_local_package_query_absent("base-devel");
        return;
    }
    metadata_stub::LocalPackageMetadata candidate{
        "collector-dependency",
        g_scenario == CollectorScenario::CurrentVersionMismatch
            ? "2.0-1"
            : "1.0-1",
        g_scenario == CollectorScenario::CurrentExplicit
            ? ALPM_PKG_REASON_EXPLICIT
            : ALPM_PKG_REASON_DEPEND};
    if(g_scenario == CollectorScenario::CurrentReasonUnknown) {
        candidate.reason = static_cast<alpm_pkgreason_t>(999);
    }
    metadata_stub::LocalPackageMetadata base_devel =
        base_devel_metadata(
            g_scenario == CollectorScenario::PolicyProtected);
    if(g_scenario == CollectorScenario::NoNewDependency) {
        metadata_stub::set_local_packages({base_devel});
        return;
    }
    std::vector<metadata_stub::LocalPackageMetadata> installed{candidate, base_devel};
    if(g_scenario == CollectorScenario::UnrelatedNewPackage) {
        installed.push_back(metadata_stub::LocalPackageMetadata{"unrelated-new-dependency", "1-1", ALPM_PKG_REASON_DEPEND});
    }
    metadata_stub::set_local_packages(std::move(installed));
    metadata_stub::set_local_package_base(
        0,
        g_scenario == CollectorScenario::CurrentPackageBaseMismatch
            ? "wrong-base"
            : "collector-dependency-base");
    metadata_stub::set_local_package_architecture(
        0,
        g_scenario == CollectorScenario::CurrentArchitectureMismatch
            ? "aarch64"
            : "x86_64");
    if(g_scenario == CollectorScenario::CurrentIdentityUnknown) {
        metadata_stub::set_local_package_architecture_null(0);
    }
    metadata_stub::enqueue_local_package_query_present_metadata(
        "collector-dependency", std::move(candidate));
    metadata_stub::enqueue_local_package_query_present_metadata(
        "base-devel", std::move(base_devel));
}

void set_baseline_metadata() {
    metadata_stub::reset_alpm_stub();
    if(g_scenario == CollectorScenario::BaselineQueryFailure) {
        metadata_stub::set_package_cache_failure();
        return;
    }
    if(g_scenario == CollectorScenario::PreExisting) {
        metadata_stub::set_local_packages({metadata_stub::LocalPackageMetadata{
                                               "collector-dependency", "1.0-1",
                                               ALPM_PKG_REASON_DEPEND},
                                           base_devel_metadata(false)});
        metadata_stub::set_local_package_base(
            0, "collector-dependency-base");
        metadata_stub::set_local_package_architecture(0, "x86_64");
        return;
    }
    metadata_stub::set_local_packages({base_devel_metadata(false)});
}

const RemoteAurCleanupCandidateAssessment* only_assessment(
    const RemoteAurCleanupCollectionResult& result) {
    return result.assessments().size() == 1
               ? &result.assessments().front()
               : nullptr;
}

std::string describe_result(
    const RemoteAurCleanupCollectionResult& result) {
    std::ostringstream output;
    output << " completeness="
           << static_cast<int>(result.completeness())
           << " assessments=" << result.assessments().size()
           << " issues=";
    for(const RemoteAurCleanupCollectionIssueKind issue : result.issues()) {
        output << static_cast<int>(issue) << ',';
    }
    for(const RemoteAurCleanupCandidateAssessment& assessment :
        result.assessments()) {
        output << " classification="
               << static_cast<int>(assessment.classification)
               << " reasons=";
        for(const CleanupClassificationReason reason : assessment.reasons) {
            output << static_cast<int>(reason) << ',';
        }
    }
    return output.str();
}

RemoteAurCleanupCollectionResult run_scenario(
    CollectorScenario scenario,
    RepositoryCandidateKind repository_candidate = RepositoryCandidateKind::None) {
    g_scenario = scenario;
    g_repository_candidate = repository_candidate;
    set_baseline_metadata();
    const bool later =
        scenario == CollectorScenario::FailedLaterWorkItem ||
        scenario == CollectorScenario::UnattemptedLaterWorkItem;
    AppConfig config;
    return collect_remote_aur_cleanup_candidates(
        prepared_remote(later), config);
}

void test_authoritative_projection_positive_and_uniqueness() {
    const RemoteAurCleanupCollectionResult positive =
        run_scenario(CollectorScenario::Positive);
    const RemoteAurCleanupCandidateAssessment* assessment =
        only_assessment(positive);
    expect(
        positive.invocation_result().is_success() &&
            positive.completeness() ==
                CleanupEvidenceCompleteness::Complete &&
            positive.has_eligible_candidate() && assessment != nullptr &&
            assessment->classification == CleanupClassification::Eligible,
        "closed collector positive fixture was not Eligible:" +
            describe_result(positive));

    const std::vector<CollectorScenario> negative_scenarios = {
        CollectorScenario::PreExisting,
        CollectorScenario::CurrentExplicit,
        CollectorScenario::CurrentVersionMismatch,
        CollectorScenario::CurrentPackageBaseMismatch,
        CollectorScenario::CurrentArchitectureMismatch,
        CollectorScenario::CurrentQueryFailure,
        CollectorScenario::PolicyProtected,
        CollectorScenario::PolicyUnknown,
        CollectorScenario::RuntimeConsumer,
        CollectorScenario::MissingCorrelation,
        CollectorScenario::ForeignToken,
        CollectorScenario::CrossSessionEvidence,
        CollectorScenario::SolverIntroducedInstall,
        CollectorScenario::ReceiptMissing,
        CollectorScenario::FailedLaterWorkItem,
        CollectorScenario::UnattemptedLaterWorkItem,
    };
    for(const CollectorScenario scenario : negative_scenarios) {
        const RemoteAurCleanupCollectionResult result =
            run_scenario(scenario);
        expect(
            !result.has_eligible_candidate(),
            "one-dimension collector negative became Eligible: scenario=" +
                std::to_string(static_cast<int>(scenario)) +
                describe_result(result));
    }
}

void test_receipt_independent_repository_candidate_matrix() {
    for(const RepositoryCandidateKind kind : {
            RepositoryCandidateKind::DirectBuild, RepositoryCandidateKind::DirectCheck,
            RepositoryCandidateKind::UniqueProvider, RepositoryCandidateKind::SelectedProvider}) {
        const auto positive = run_scenario(CollectorScenario::Positive, kind);
        expect(positive.invocation_result().is_success() &&
                   positive.completeness() == CleanupEvidenceCompleteness::Complete &&
                   positive.has_eligible_candidate() && only_assessment(positive) != nullptr &&
                   positive.issues().empty(),
               "receipt-independent repository candidate was not Eligible: kind=" +
                   std::to_string(static_cast<int>(kind)) + describe_result(positive));
        for(const CollectorScenario scenario : {
                CollectorScenario::PreExisting, CollectorScenario::CurrentExplicit,
                CollectorScenario::CurrentVersionMismatch, CollectorScenario::CurrentPackageBaseMismatch,
                CollectorScenario::CurrentArchitectureMismatch, CollectorScenario::CurrentQueryFailure,
                CollectorScenario::CurrentReasonUnknown, CollectorScenario::CurrentIdentityUnknown,
                CollectorScenario::BaselineQueryFailure, CollectorScenario::PolicyProtected,
                CollectorScenario::PolicyUnknown, CollectorScenario::RuntimeConsumer,
                CollectorScenario::MissingCorrelation, CollectorScenario::IncompleteConsumer,
                CollectorScenario::FailedLaterWorkItem, CollectorScenario::UnattemptedLaterWorkItem,
                CollectorScenario::IncompleteRegisteredTransaction, CollectorScenario::ContradictorySuccess,
                CollectorScenario::NoNewDependency}) {
            const auto result = run_scenario(scenario, kind);
            expect(!result.has_eligible_candidate(),
                   "receipt-independent unsafe candidate became Eligible: scenario=" +
                       std::to_string(static_cast<int>(scenario)) + describe_result(result));
            if(scenario == CollectorScenario::CurrentExplicit ||
               scenario == CollectorScenario::PolicyProtected ||
               scenario == CollectorScenario::RuntimeConsumer) {
                expect(only_assessment(result) != nullptr &&
                           only_assessment(result)->classification == CleanupClassification::Protected,
                       "ordinary protection was lost:" + describe_result(result));
            }
            if(scenario == CollectorScenario::CurrentQueryFailure ||
               scenario == CollectorScenario::CurrentReasonUnknown ||
               scenario == CollectorScenario::IncompleteConsumer ||
               scenario == CollectorScenario::PolicyUnknown) {
                expect(only_assessment(result) != nullptr &&
                           only_assessment(result)->classification == CleanupClassification::Unknown,
                       "unknown ordinary evidence was not retained:" + describe_result(result));
            }
        }
        const auto unrelated = run_scenario(CollectorScenario::UnrelatedNewPackage, kind);
        expect(unrelated.has_eligible_candidate() && only_assessment(unrelated) != nullptr &&
                   only_assessment(unrelated)->package.package().package_name() == "collector-dependency",
               "post-state difference admitted an unrelated package:" + describe_result(unrelated));
    }
    for(const CollectorScenario scenario : {CollectorScenario::ProviderMismatch,
                                            CollectorScenario::AmbiguousProvider, CollectorScenario::ProviderMetadataUnknown}) {
        const auto result = run_scenario(scenario, RepositoryCandidateKind::UniqueProvider);
        expect(!result.has_eligible_candidate() && only_assessment(result) != nullptr &&
                   (only_assessment(result)->classification == CleanupClassification::Unknown ||
                    only_assessment(result)->classification == CleanupClassification::Invalid),
               "invalid/unknown repository provider became eligible:" + describe_result(result));
    }
    const auto mismatched_preparation = run_scenario(
        CollectorScenario::PreparedProviderMismatch, RepositoryCandidateKind::SelectedProvider);
    expect(!mismatched_preparation.has_eligible_candidate(),
           "prepared selected-provider contradiction was ignored:" + describe_result(mismatched_preparation));
}

TrustedAlpmReceiptCaptureResult selected_provider_failure_capture(
    TrustedAlpmReceiptCaptureStatus status,
    InvocationDependencyTransactionCommandOutcome command_outcome,
    char token_character) {
    const std::string transaction_token = token(token_character);
    const auto owner = InvocationDependencyTransactionOwner::
        SelectedRepositoryProvider;
    const bool outcome_unknown =
        command_outcome ==
        InvocationDependencyTransactionCommandOutcome::Unknown;
    PacmanTransactionReceipt receipt =
        validate_pacman_transaction_receipt(
            transaction_token, owner,
            PacmanTransactionReceiptObservation{
                outcome_unknown
                    ? PacmanTransactionReceiptObservationState::Incomplete
                    : PacmanTransactionReceiptObservationState::Missing,
                outcome_unknown
                    ? std::optional<std::string>{transaction_token}
                    : std::nullopt,
                outcome_unknown
                    ? std::optional<InvocationDependencyTransactionOwner>{
                          owner}
                    : std::nullopt,
                {}});
    InvocationDependencyTransactionLedger ledger;
    ledger.transactions.push_back(
        InvocationDependencyTransaction{
            transaction_token, owner, {"collector-selected-provider"}, command_outcome, std::move(receipt)});
    return TrustedAlpmReceiptCaptureResult{
        status, std::nullopt, std::move(ledger),
        "synthetic selected-provider transport failure"};
}

void test_selected_provider_retry_boundary() {
    using namespace selected_provider_fallback_stub;

    SelectedRepositoryProviderTransactionResult unknown_operation;
    unknown_operation.status =
        SelectedRepositoryProviderTransactionStatus::OutcomeUnknown;
    unknown_operation.selected_providers.push_back(
        selected_repository_provider());
    unknown_operation.package_state_change = PackageStateChange::Unknown;
    unknown_operation.diagnostic =
        "selected-provider outcome unknown after launch";
    TrustedAlpmReceiptCaptureResult unknown_capture =
        selected_provider_failure_capture(
            TrustedAlpmReceiptCaptureStatus::OutcomeUnknown,
            InvocationDependencyTransactionCommandOutcome::Unknown, 'u');
    reset(
        SelectedRepositoryProviderTrustedReceiptExecutionResult{
            unknown_operation, unknown_capture},
        SelectedRepositoryProviderTransactionResult{});
    const RemoteAurCleanupCollectionResult unknown_result =
        run_scenario(
            CollectorScenario::SelectedProviderPostLaunchUnknown);
    expect(
        trusted_call_count == 1 && legacy_call_count == 0 &&
            observed_operation.has_value() &&
            observed_operation->status ==
                SelectedRepositoryProviderTransactionStatus::
                    OutcomeUnknown &&
            !observed_operation->command_exit_status.has_value() &&
            !unknown_result.has_eligible_candidate() &&
            unknown_result.completeness() !=
                CleanupEvidenceCompleteness::Complete,
        "post-launch selected-provider outcome retried or became positive");

    SelectedRepositoryProviderTransactionResult blocked_operation;
    blocked_operation.status =
        SelectedRepositoryProviderTransactionStatus::
            BlockedBeforeExecution;
    blocked_operation.selected_providers.push_back(
        selected_repository_provider());
    blocked_operation.package_state_change = PackageStateChange::Unknown;
    blocked_operation.diagnostic =
        "selected-provider process definitely did not start";
    TrustedAlpmReceiptCaptureResult blocked_capture =
        selected_provider_failure_capture(
            TrustedAlpmReceiptCaptureStatus::PrepareFailed,
            InvocationDependencyTransactionCommandOutcome::NotAttempted,
            'n');
    SelectedRepositoryProviderTransactionResult fallback_success;
    fallback_success.status =
        SelectedRepositoryProviderTransactionStatus::Succeeded;
    fallback_success.selected_providers =
        blocked_operation.selected_providers;
    fallback_success.package_state_change = PackageStateChange::Unknown;
    fallback_success.command_exit_status = 0;
    reset(
        SelectedRepositoryProviderTrustedReceiptExecutionResult{
            blocked_operation, blocked_capture},
        fallback_success);
    const RemoteAurCleanupCollectionResult pre_launch_result =
        run_scenario(
            CollectorScenario::SelectedProviderPreLaunchFailure);
    expect(
        trusted_call_count == 1 && legacy_call_count == 1 &&
            observed_operation.has_value() &&
            observed_operation->status ==
                SelectedRepositoryProviderTransactionStatus::Succeeded &&
            !pre_launch_result.has_eligible_candidate(),
        "confirmed pre-launch selected-provider failure lost compatibility fallback");
}

} // namespace

ProductionSourceBuildInvocationResult
execute_prepared_remote_aur_cleanup_invocation(
    RemoteAurCleanupCandidateCollector& collector,
    const AppConfig&) {
    if(g_scenario ==
           CollectorScenario::SelectedProviderPostLaunchUnknown ||
       g_scenario ==
           CollectorScenario::SelectedProviderPreLaunchFailure) {
        selected_provider_fallback_stub::observed_operation =
            collector.execute_selected_repository_provider_transaction(
                AppConfig{});
        set_current_metadata();
        return successful_result(
            collector.prepared_for_test().invocation);
    }
    CleanupInvocationSession& session = collector.session_for_test();
    ProductionSourceBuildInvocationResult result = successful_result(
        collector.prepared_for_test().invocation);
    if(g_repository_candidate != RepositoryCandidateKind::None) {
        if(g_scenario == CollectorScenario::IncompleteRegisteredTransaction) {
            expect(register_cleanup_invocation_transaction_token_for_test(
                       session, InvocationDependencyTransactionOwner::SelectedRepositoryProvider,
                       token('a'), {0}, false),
                   "incomplete transaction fixture registration failed");
        }
        if(g_scenario == CollectorScenario::FailedLaterWorkItem) {
            result.work_items.back().status = ProductionSourceBuildWorkItemStatus::Failed;
        } else if(g_scenario == CollectorScenario::UnattemptedLaterWorkItem) {
            result.work_items.back().status = ProductionSourceBuildWorkItemStatus::NotAttempted;
        } else if(g_scenario == CollectorScenario::ContradictorySuccess) {
            result.work_items.front().diagnostic = "success with failure evidence";
        }
        set_current_metadata();
        return result;
    }
    if(g_scenario == CollectorScenario::ReceiptMissing) {
        set_current_metadata();
        return result;
    }

    const std::string causal_token = token('c');
    const std::string inventory_token =
        g_scenario == CollectorScenario::ForeignToken
            ? token('f')
            : causal_token;
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            inventory_token,
            {0}),
        "collector transaction inventory fixture failed");

    CleanupInvocationSession* evidence_session = &session;
    std::optional<CleanupInvocationSession> other_session;
    if(g_scenario == CollectorScenario::CrossSessionEvidence) {
        other_session.emplace(CleanupInvocationSession::begin(
            prepared_remote()));
        evidence_session = &other_session.value();
    }
    std::vector<std::size_t> edge_indices =
        g_scenario == CollectorScenario::MissingCorrelation
            ? std::vector<std::size_t>{99}
            : std::vector<std::size_t>{0};
    std::vector<std::string> extra_installs =
        g_scenario == CollectorScenario::SolverIntroducedInstall
            ? std::vector<std::string>{"solver-extra"}
            : std::vector<std::string>{};
    collector.retain_source_artifact_causal_evidence_for_test(
        make_causal_evidence(
            *evidence_session, causal_token,
            std::move(edge_indices), std::move(extra_installs)));

    if(g_scenario == CollectorScenario::FailedLaterWorkItem) {
        ProductionSourceBuildWorkItemOutcome& later =
            result.work_items.back();
        later.status = ProductionSourceBuildWorkItemStatus::Failed;
        later.production_outcome.reset();
        later.failure_stage = ProductionSourceBuildFailureStage::Build;
        later.diagnostic = "synthetic later failure";
        later.failure_exception = std::make_exception_ptr(
            std::runtime_error("synthetic later failure"));
    } else if(g_scenario ==
              CollectorScenario::UnattemptedLaterWorkItem) {
        ProductionSourceBuildWorkItemOutcome unattempted;
        unattempted.package_base = collector.prepared_for_test()
                                       .invocation.work_items.back()
                                       .request.checkout_name;
        result.work_items.back() = std::move(unattempted);
    }
    set_current_metadata();
    return result;
}

void run_remote_aur_cleanup_candidate_collector_tests() {
    test_authoritative_projection_positive_and_uniqueness();
    test_selected_provider_retry_boundary();
    test_receipt_independent_repository_candidate_matrix();
}
