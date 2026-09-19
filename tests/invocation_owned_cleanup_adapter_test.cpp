#include "build_plan_artifact_target_projection.hpp"
#include "invocation_owned_cleanup_adapter.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

CleanupPolicyAuthorityEvidence unobserved_policy_authority(
    CleanupPolicyAuthorityKind kind) {
    return CleanupPolicyAuthorityEvidence{
        kind,
        CleanupPolicyAuthorityObservation::NotObserved,
        CleanupPolicyMetadataCompleteness::Incomplete,
        CleanupPolicyCandidateEvaluation::NotEvaluated,
        CleanupPolicyMetadataCompleteness::Incomplete,
        {},
        std::nullopt};
}

CleanupPolicyAuthorityEvidence absent_policy_authority(
    CleanupPolicyAuthorityKind kind) {
    return CleanupPolicyAuthorityEvidence{
        kind,
        CleanupPolicyAuthorityObservation::Absent,
        CleanupPolicyMetadataCompleteness::Complete,
        CleanupPolicyCandidateEvaluation::NotEvaluated,
        CleanupPolicyMetadataCompleteness::Incomplete,
        {},
        std::nullopt};
}

CleanupPolicyAuthorityEvidence meta_policy_authority(
    CleanupPolicyAuthorityKind kind,
    CleanupPolicyCandidateEvaluation evaluation,
    CleanupPolicyMetadataCompleteness inventory_completeness =
        CleanupPolicyMetadataCompleteness::Complete,
    CleanupPolicyMetadataCompleteness evaluation_completeness =
        CleanupPolicyMetadataCompleteness::Complete) {
    return CleanupPolicyAuthorityEvidence{
        kind,
        CleanupPolicyAuthorityObservation::Present,
        inventory_completeness,
        evaluation,
        evaluation_completeness,
        {CleanupPolicyMetaPackageMetadata{
            kind,
            kind == CleanupPolicyAuthorityKind::
                        ConfiguredSyncBaseDevelMetaPackage
                ? std::optional<std::size_t>(0)
                : std::nullopt,
            kind == CleanupPolicyAuthorityKind::
                        ConfiguredSyncBaseDevelMetaPackage
                ? std::optional<std::string>("core")
                : std::nullopt,
            "base-devel",
            "1-2",
            {"toolchain"}}},
        std::nullopt};
}

CleanupPolicyAuthorityEvidence group_policy_authority(
    CleanupPolicyCandidateEvaluation evaluation,
    CleanupPolicyMetadataCompleteness inventory_completeness =
        CleanupPolicyMetadataCompleteness::Complete,
    CleanupPolicyMetadataCompleteness evaluation_completeness =
        CleanupPolicyMetadataCompleteness::Complete) {
    return CleanupPolicyAuthorityEvidence{
        CleanupPolicyAuthorityKind::BaseDevelGroupCompatibility,
        CleanupPolicyAuthorityObservation::Present,
        inventory_completeness,
        evaluation,
        evaluation_completeness,
        {},
        CleanupPolicyGroupMetadata{
            "base-devel",
            {"core"},
            {"core"},
            {CleanupPolicyGroupMemberMetadata{0, "core", "toolchain"}}}};
}

CleanupPolicyProtectionEvidence base_policy_evidence() {
    return CleanupPolicyProtectionEvidence{
        CleanupPolicyMetadataCompleteness::Complete,
        CleanupPolicyMetadataCompleteness::Complete,
        CleanupPolicyCandidatePackageMetadata{
            "candidate", "1-1", {}, {}},
        absent_policy_authority(
            CleanupPolicyAuthorityKind::InstalledBaseDevelMetaPackage),
        absent_policy_authority(
            CleanupPolicyAuthorityKind::
                ConfiguredSyncBaseDevelMetaPackage),
        absent_policy_authority(
            CleanupPolicyAuthorityKind::BaseDevelGroupCompatibility),
        CleanupPolicyEvidenceConsistency::Consistent,
        {}};
}

bool has_reason(
    const CleanupClassificationResult& result,
    CleanupClassificationReason reason) {
    return std::find(
               result.reasons().begin(), result.reasons().end(), reason) !=
           result.reasons().end();
}

bool has_issue(
    const InvocationOwnedCleanupCandidateProjectionSuccess& projection,
    CleanupLifecycleProjectionIssueKind kind) {
    return std::any_of(
        projection.issues.begin(), projection.issues.end(),
        [kind](const CleanupLifecycleProjectionIssue& issue) {
            return issue.kind == kind;
        });
}

bool has_source_correlation_issue(
    const CleanupSourceArtifactCorrelationEvidence& evidence,
    CleanupSourceArtifactCorrelationIssueKind kind) {
    return std::find(
               evidence.issues().begin(), evidence.issues().end(), kind) !=
           evidence.issues().end();
}

bool has_provider_correlation_issue(
    const CleanupSelectedProviderCorrelationEvidence& evidence,
    CleanupSelectedProviderCorrelationIssueKind kind) {
    return std::find(
               evidence.issues().begin(), evidence.issues().end(), kind) !=
           evidence.issues().end();
}

bool has_invocation_issue(
    const CleanupInvocationEvidence& evidence,
    CleanupInvocationEvidenceIssueKind kind) {
    return std::find(
               evidence.issues().begin(), evidence.issues().end(), kind) !=
           evidence.issues().end();
}

RootTargetIdentity root(
    std::size_t invocation_index, std::string requested_name) {
    return RootTargetIdentity{
        invocation_index, std::move(requested_name)};
}

DependencyRequirement requirement(const std::string& package_name) {
    return ConsumerDependencyRequirement(
        package_name, package_name, std::nullopt);
}

PackageBaseIdentity aur_package_base(const std::string& package_base) {
    return PackageBaseIdentity::make(
        PackageSourceIdentity::aur(
            SourceLocationIdentity::known_git_remote(
                "https://aur.archlinux.org/" + package_base +
                ".git")),
        package_base);
}

BuildPlanDependencyEdge aur_edge(
    std::string parent_name,
    std::string parent_base,
    PackageRole role,
    std::string package_name = "build-tool",
    std::string package_base = "build-tools") {
    BuildPlanDependencyEdge edge;
    edge.parent_package_name = std::move(parent_name);
    edge.parent_package_base = std::move(parent_base);
    edge.dependency_spec = package_name;
    edge.role = role;
    edge.kind = DependencyKind::Aur;
    edge.resolved_package_name = package_name;
    edge.resolved_package_base = package_base;
    edge.requirement = requirement(package_name);
    edge.resolved_candidate = AurResolvedDependencyCandidate{
        std::move(package_name), std::move(package_base),
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage, "1.0-1")};
    edge.constraint_evaluation = ConstraintEvaluation::satisfied();
    return edge;
}

BuildPlan basic_plan() {
    BuildPlan plan;
    const RootTargetIdentity app_root = root(0, "application");
    plan.root_targets.push_back(app_root);
    plan.package_targets.push_back(PlannedPackageTarget{
        "build-tool", "build-tools", {PackageRole::BuildDependency}, {app_root}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "application", "application", {PackageRole::Root}, {app_root}});
    plan.order.push_back(BuildPlanEntry{
        "build-tools", {"build-tool"}});
    plan.order.push_back(BuildPlanEntry{
        "application", {"application"}});
    plan.dependency_edges.push_back(aur_edge(
        "application", "application",
        PackageRole::BuildDependency));
    return plan;
}

BuildPlan multiple_root_plan() {
    BuildPlan plan;
    const RootTargetIdentity first_root = root(0, "first-app");
    const RootTargetIdentity second_root = root(1, "second-app");
    plan.root_targets = {first_root, second_root};
    plan.package_targets.push_back(PlannedPackageTarget{
        "build-tool", "build-tools", {PackageRole::BuildDependency, PackageRole::CheckDependency}, {first_root, second_root}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "first-app", "first-base", {PackageRole::Root}, {first_root}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "second-app", "second-base", {PackageRole::Root}, {second_root}});
    plan.order.push_back(BuildPlanEntry{
        "build-tools", {"build-tool"}});
    plan.order.push_back(BuildPlanEntry{"first-base", {"first-app"}});
    plan.order.push_back(BuildPlanEntry{
        "second-base", {"second-app"}});
    plan.dependency_edges.push_back(aur_edge(
        "first-app", "first-base",
        PackageRole::BuildDependency));
    plan.dependency_edges.push_back(aur_edge(
        "second-app", "second-base",
        PackageRole::CheckDependency));
    return plan;
}

BuildPlan mixed_runtime_plan() {
    BuildPlan plan = basic_plan();
    plan.package_targets.front().roles = {
        PackageRole::RuntimeDependency,
        PackageRole::BuildDependency};
    plan.dependency_edges.insert(
        plan.dependency_edges.begin(),
        aur_edge(
            "application", "application",
            PackageRole::RuntimeDependency));
    return plan;
}

ProvidedDependency repository_provider() {
    ProviderCapability capability(
        "virtual-build-tool", "virtual-build-tool", std::nullopt);
    return ProvidedDependency::from_repository_constraint_metadata(
        "core", 0, "repository-tool", "repository-tools", "x86_64",
        ProviderConstraintMetadata{
            capability,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                "2.0-1"),
            ObservedVersion::unknown(
                ObservedVersionSource::
                    RepositoryProviderCapability,
                ObservedVersionUnknownReason::
                    UnversionedProviderCapability)});
}

BuildPlan repository_provider_plan() {
    BuildPlan plan;
    const RootTargetIdentity app_root = root(0, "application");
    plan.root_targets.push_back(app_root);
    plan.package_targets.push_back(PlannedPackageTarget{
        "application", "application", {PackageRole::Root}, {app_root}});
    plan.order.push_back(BuildPlanEntry{
        "application", {"application"}});
    plan.configured_repository_order =
        std::vector<std::string>{"core"};

    const ProvidedDependency provider = repository_provider();
    BuildPlanDependencyEdge edge;
    edge.parent_package_name = "application";
    edge.parent_package_base = "application";
    edge.dependency_spec = "virtual-build-tool";
    edge.role = PackageRole::BuildDependency;
    edge.kind = DependencyKind::Provided;
    edge.resolved_package_name = provider.package_name;
    edge.resolved_provider = provider;
    edge.provider_resolution = ProviderResolutionKind::UserSelected;
    edge.requirement = requirement("virtual-build-tool");
    edge.resolved_candidate = ProviderResolvedDependencyCandidate{
        provider,
        provider.constraint_metadata->provided_version};
    edge.constraint_evaluation = ConstraintEvaluation::satisfied();
    plan.dependency_edges.push_back(std::move(edge));
    plan.provided.push_back(BuildPlanProvidedDependency{
        "virtual-build-tool", provider,
        ProviderResolutionKind::UserSelected});
    return plan;
}

ProvidedDependency cargo_repository_provider() {
    ProviderCapability capability(
        "cargo=1.90.0", "cargo", std::string("1.90.0"));
    return ProvidedDependency::from_repository_constraint_metadata(
        "extra", 0, "rust", "rust", "x86_64",
        ProviderConstraintMetadata{
            capability,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                "1.90.0-1"),
            ObservedVersion::available(
                ObservedVersionSource::RepositoryProviderCapability,
                "1.90.0")});
}

BuildPlan cargo_provider_plan(std::size_t consumer_count = 1) {
    BuildPlan plan;
    plan.configured_repository_order =
        std::vector<std::string>{"extra"};
    const ProvidedDependency provider = cargo_repository_provider();
    for(std::size_t index = 0; index < consumer_count; ++index) {
        const std::string package_name =
            index == 0 ? "root-a" : "root-b";
        const std::string package_base =
            index == 0 ? "root-a-base" : "root-b-base";
        const RootTargetIdentity root_identity = root(index, package_name);
        plan.root_targets.push_back(root_identity);
        plan.package_targets.push_back(PlannedPackageTarget{
            package_name, package_base, {PackageRole::Root}, {root_identity}});
        plan.order.push_back(BuildPlanEntry{
            package_base, {package_name}});

        BuildPlanDependencyEdge edge;
        edge.parent_package_name = package_name;
        edge.parent_package_base = package_base;
        edge.dependency_spec = "cargo";
        edge.role = PackageRole::BuildDependency;
        edge.kind = DependencyKind::Provided;
        edge.resolved_provider = provider;
        edge.provider_resolution = ProviderResolutionKind::UserSelected;
        edge.requirement = requirement("cargo");
        edge.resolved_candidate = ProviderResolvedDependencyCandidate{
            provider,
            provider.constraint_metadata->provided_version};
        edge.constraint_evaluation = ConstraintEvaluation::satisfied();
        plan.dependency_edges.push_back(std::move(edge));
    }
    plan.provided.push_back(BuildPlanProvidedDependency{
        "cargo", provider, ProviderResolutionKind::UserSelected});
    return plan;
}

ResolvedDependencyCandidate repository_exact_candidate() {
    return RepositoryExactPackage{
        ConfiguredRepositoryIdentity{"core", 0},
        "repository-tool",
        "repository-tools",
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage,
            "2.0-1"),
        {}};
}

PreparedProductionSourceBuildInvocation prepared_invocation(
    const BuildPlan& plan) {
    BuildPlanArtifactTargetProjectionResult projected =
        project_build_plan_required_artifact_targets(plan);
    const BuildPlanArtifactTargetProjectionSuccess* success =
        projected.success();
    if(success == nullptr) {
        throw std::runtime_error(
            "test plan did not project artifact targets");
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
            if(!edge.resolved_candidate.has_value()) continue;
            std::string resolved_name;
            std::string resolved_base;
            if(const auto* aur =
                   std::get_if<AurResolvedDependencyCandidate>(
                       &edge.resolved_candidate.value());
               aur != nullptr) {
                resolved_name = aur->package_name;
                resolved_base = aur->package_base;
            } else if(const auto* provider =
                          std::get_if<
                              ProviderResolvedDependencyCandidate>(
                              &edge.resolved_candidate.value());
                      provider != nullptr &&
                      std::holds_alternative<AurProviderOrigin>(
                          provider->provider.origin)) {
                resolved_name = provider->provider.package_name;
                resolved_base = provider->provider.package_base;
            }
            const bool matches_target = std::any_of(
                unit.required_targets.begin(),
                unit.required_targets.end(),
                [&resolved_name, &resolved_base](
                    const RequiredPackageArtifactTarget& target) {
                    return target.package_name == resolved_name &&
                           target.package_base == resolved_base;
                });
            if(matches_target) {
                work_item.build_plan_dependency_edge_indices.push_back(
                    edge_index);
            }
        }
        work_items.push_back(std::move(work_item));
    }
    std::vector<ProvidedDependency> selected_providers;
    for(const ProductionSourceBuildWorkItem& work_item : work_items) {
        for(const ProvidedDependency& provider :
            work_item.selected_repository_providers) {
            if(std::find_if(
                   selected_providers.begin(), selected_providers.end(),
                   [&provider](const ProvidedDependency& existing) {
                       return same_provider_identity(existing, provider);
                   }) == selected_providers.end()) {
                selected_providers.push_back(provider);
            }
        }
    }
    return PreparedProductionSourceBuildInvocation{
        std::move(work_items), std::move(selected_providers),
        PacmanDatabasePaths{"/", "/var/lib/pacman"}, std::nullopt,
        std::nullopt};
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
        result.work_items.push_back(ProductionSourceBuildWorkItemOutcome{
            work_item.request.checkout_name,
            ProductionSourceBuildWorkItemStatus::Succeeded,
            successful_staged_outcome(), std::nullopt, std::nullopt,
            nullptr});
    }
    return result;
}

ProductionSourceBuildInvocationResult result_after_work_item(
    const PreparedProductionSourceBuildInvocation& invocation,
    std::size_t completed_index) {
    ProductionSourceBuildInvocationResult result;
    for(std::size_t index = 0; index < invocation.work_items.size();
        ++index) {
        ProductionSourceBuildWorkItemOutcome outcome;
        outcome.package_base =
            invocation.work_items[index].request.checkout_name;
        if(index <= completed_index) {
            outcome.status =
                ProductionSourceBuildWorkItemStatus::Succeeded;
            outcome.production_outcome = successful_staged_outcome();
        }
        result.work_items.push_back(std::move(outcome));
    }
    return result;
}

InstalledPackageStateSnapshotResult absent_snapshot() {
    return InstalledPackageStateSnapshot{};
}

InstalledPackageStateSnapshotResult present_snapshot(
    const std::string& package_name,
    const std::string& version,
    InstalledPackageReason reason,
    std::string package_base = {},
    InstalledPackageBaseIdentity base_identity =
        InstalledPackageBaseIdentity::unknown(),
    InstalledPackageArchitectureIdentity architecture_identity =
        InstalledPackageArchitectureIdentity::known("x86_64")) {
    if(base_identity.state() == InstalledPackageMetadataValueState::Unknown) {
        if(package_base.empty()) package_base = package_name;
        base_identity = InstalledPackageBaseIdentity::known(
            std::move(package_base));
    }
    InstalledPackageStateSnapshot snapshot;
    snapshot.emplace(
        package_name,
        InstalledPackageMetadata{
            package_name, version, reason, std::move(base_identity),
            std::move(architecture_identity)});
    return snapshot;
}

InstalledPackageStateSnapshotResult failed_snapshot() {
    return PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "typed metadata failure"};
}

std::string transaction_token(char digit = 'a') {
    return std::string(64, digit);
}

PacmanTransactionReceipt transaction_receipt(
    const std::string& token,
    InvocationDependencyTransactionOwner owner,
    std::vector<PacmanTransactionPackageObservation> operations,
    PacmanTransactionReceiptObservationState state =
        PacmanTransactionReceiptObservationState::Complete) {
    return validate_pacman_transaction_receipt(
        token, owner,
        PacmanTransactionReceiptObservation{
            state,
            state == PacmanTransactionReceiptObservationState::Missing
                ? std::nullopt
                : std::optional<std::string>{token},
            state == PacmanTransactionReceiptObservationState::Missing
                ? std::nullopt
                : std::optional<
                      InvocationDependencyTransactionOwner>{
                      owner},
            std::move(operations)});
}

InvocationDependencyTransaction dependency_transaction(
    std::string token,
    InvocationDependencyTransactionOwner owner,
    std::vector<std::string> requested_package_names,
    InvocationDependencyTransactionCommandOutcome command_outcome,
    PacmanTransactionReceipt receipt) {
    return InvocationDependencyTransaction{
        std::move(token), owner, std::move(requested_package_names),
        command_outcome, std::move(receipt)};
}

SourceAwarePackageIdentity source_artifact_identity(
    const std::string& package_name,
    const std::string& package_base,
    const std::string& version = "1.0-1") {
    return SourceAwarePackageIdentity::make(
        PackageChildIdentity::make(
            aur_package_base(package_base), package_name),
        SourceRevisionIdentity::unknown(),
        PackageVersionIdentity::composite(version),
        PackageArchitectureIdentity::known({"x86_64"}));
}

ArtifactPackageIdentity source_archive_identity(
    const std::string& package_name,
    const std::string& package_base,
    const std::string& version = "1.0-1") {
    return ArtifactPackageIdentity{
        package_name,
        version,
        ArtifactPackageBaseIdentity::known(package_base),
        ArtifactPackageArchitectureIdentity::known("x86_64")};
}

SourceArtifactInstallCausalEvidence source_artifact_causal_evidence(
    const CleanupInvocationSession& session,
    std::size_t work_item_index,
    const std::string& package_name,
    const std::string& package_base,
    std::vector<RootTargetIdentity> roots,
    std::vector<PackageRole> roles,
    std::vector<std::size_t> edge_indices,
    const std::string& version = "1.0-1") {
    SourceArtifactInstallWorkItemBinding binding{
        session.authority(), work_item_index, package_base, roots};
    const SourceArtifactInstallExpectedSelectedArtifact expected{
        0,
        source_artifact_identity(package_name, package_base, version),
        DesiredInstallReason::Dependency,
        roles,
        roots,
        edge_indices};
    const SourceArtifactInstallObservedSelectedArtifact observed{
        0,
        source_archive_identity(package_name, package_base, version),
        DesiredInstallReason::Dependency,
        roles,
        roots,
        edge_indices};
    const std::string token = transaction_token('d');
    InvocationDependencyTransactionLedger ledger{{dependency_transaction(
        token,
        InvocationDependencyTransactionOwner::SourceArtifactInstall,
        {package_name},
        InvocationDependencyTransactionCommandOutcome::Succeeded,
        transaction_receipt(
            token,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            {{PacmanTransactionPackageOperation::Install,
              package_name}}))}};
    const SourceArtifactInstallReceiptEvidence receipt_evidence =
        establish_source_artifact_install_receipt_evidence(
            SourceArtifactInstallReceiptExpectation{
                binding, {expected}, token},
            make_source_artifact_install_receipt_observation_for_test(
                binding, {observed}, std::move(ledger)));
    std::optional<SourceArtifactInstallCausalEvidence> causal =
        project_source_artifact_install_causal_evidence(
            receipt_evidence);
    if(!causal.has_value()) {
        throw std::runtime_error(
            "source artifact causal fixture was incomplete");
    }
    return std::move(causal.value());
}

PreparedRemoteSourceBuild prepared_remote_aur_build(BuildPlan plan) {
    if(plan.root_targets.empty()) {
        throw std::runtime_error("remote AUR fixture has no root");
    }
    const RootTargetIdentity& root_identity = plan.root_targets.front();
    const PlannedPackageTarget* root_target = nullptr;
    for(const PlannedPackageTarget& target : plan.package_targets) {
        if(std::find(
               target.roots.begin(), target.roots.end(), root_identity) !=
               target.roots.end() &&
           std::find(
               target.roles.begin(), target.roles.end(),
               PackageRole::Root) != target.roles.end()) {
            root_target = &target;
            break;
        }
    }
    if(root_target == nullptr) {
        throw std::runtime_error("remote AUR fixture root is incomplete");
    }
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    return PreparedRemoteSourceBuild{
        ResolvedSourceBuildIdentity{ResolvedAurSourceBuildIdentity{
            root_identity.requested_name, root_target->package_base}},
        std::move(plan), std::move(invocation)};
}

SelectedRepositoryProviderTrustedExecutionEvidence
selected_provider_execution(
    const CleanupInvocationSession& session,
    const BuildPlan& plan,
    const PreparedProductionSourceBuildInvocation& invocation,
    std::vector<std::string> extra_installs = {}) {
    const std::string token = transaction_token('e');
    std::vector<PacmanTransactionPackageObservation> operations;
    std::vector<std::string> requested_names;
    for(const ProvidedDependency& provider :
        invocation.selected_repository_providers) {
        requested_names.push_back(provider.package_name);
        operations.push_back(PacmanTransactionPackageObservation{
            PacmanTransactionPackageOperation::Install,
            provider.package_name});
    }
    for(std::string& package_name : extra_installs) {
        operations.push_back(PacmanTransactionPackageObservation{
            PacmanTransactionPackageOperation::Install,
            std::move(package_name)});
    }
    std::vector<std::string> actual_install_set;
    for(const PacmanTransactionPackageObservation& operation : operations) {
        if(operation.operation ==
           PacmanTransactionPackageOperation::Install) {
            actual_install_set.push_back(operation.package_name);
        }
    }
    InvocationDependencyTransactionLedger ledger{{dependency_transaction(
        token,
        InvocationDependencyTransactionOwner::SelectedRepositoryProvider,
        requested_names,
        InvocationDependencyTransactionCommandOutcome::Succeeded,
        transaction_receipt(
            token,
            InvocationDependencyTransactionOwner::
                SelectedRepositoryProvider,
            std::move(operations)))}};
    SelectedRepositoryProviderTransactionResult transaction;
    transaction.status =
        SelectedRepositoryProviderTransactionStatus::Succeeded;
    transaction.selected_providers =
        invocation.selected_repository_providers;
    transaction.package_state_change = PackageStateChange::Unknown;
    transaction.command_exit_status = 0;
    std::vector<SelectedRepositoryProviderTrustedExecutionBinding> bindings;
    for(std::size_t work_item_index = 0;
        work_item_index < invocation.work_items.size(); ++work_item_index) {
        const ProductionSourceBuildWorkItem& work_item =
            invocation.work_items[work_item_index];
        for(const std::size_t edge_index :
            work_item.selected_repository_provider_edge_indices) {
            const BuildPlanDependencyEdge& edge =
                plan.dependency_edges.at(edge_index);
            const auto decision = std::find_if(
                plan.provided.begin(), plan.provided.end(),
                [&edge](const BuildPlanProvidedDependency& candidate) {
                    return candidate.dependency == edge.dependency_spec &&
                           edge.resolved_provider.has_value() &&
                           candidate.provider == edge.resolved_provider.value() &&
                           candidate.resolution == edge.provider_resolution;
                });
            if(decision == plan.provided.end() ||
               !edge.requirement.has_value() ||
               !edge.resolved_provider.has_value()) {
                throw std::runtime_error(
                    "selected-provider fixture binding is incomplete");
            }
            bindings.push_back(
                SelectedRepositoryProviderTrustedExecutionBinding{
                    work_item_index,
                    edge_index,
                    edge.parent_package_base,
                    edge.requirement.value(),
                    *decision,
                    edge.resolved_provider.value(),
                    edge.provider_resolution});
        }
    }
    return SelectedRepositoryProviderTrustedExecutionEvidence::make_for_test(
        session.authority(), token, std::move(bindings),
        std::move(transaction),
        TrustedAlpmReceiptCaptureResult{
            TrustedAlpmReceiptCaptureStatus::Complete, 0,
            std::move(ledger), std::nullopt},
        std::move(actual_install_set));
}

SelectedRepositoryProviderTrustedExecutionEvidence
selected_provider_execution_variant(
    const CleanupInvocationSession& session,
    const SelectedRepositoryProviderTrustedExecutionEvidence& source,
    std::vector<SelectedRepositoryProviderTrustedExecutionBinding> bindings,
    std::optional<std::string> token = std::nullopt,
    std::optional<std::vector<std::string>> actual_install_set =
        std::nullopt) {
    return SelectedRepositoryProviderTrustedExecutionEvidence::make_for_test(
        session.authority(),
        token.value_or(source.transaction_token()), std::move(bindings),
        source.transaction(), source.receipt_capture(),
        actual_install_set.value_or(source.actual_install_set()));
}

CleanupInvocationEvidence aggregate_plan_without_correlations(
    BuildPlan plan,
    const std::function<void(ProductionSourceBuildInvocationResult&)>&
        mutate_result = {},
    InvocationDependencyTransactionOwner inventory_owner =
        InvocationDependencyTransactionOwner::SourceArtifactInstall) {
    PreparedRemoteSourceBuild prepared = prepared_remote_aur_build(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(prepared.invocation);
    if(mutate_result) mutate_result(result);
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        std::move(prepared));
    const CleanupBaselineSnapshotObservation baseline =
        make_cleanup_baseline_observation_for_test(
            session, absent_snapshot());
    const char token_digit =
        inventory_owner == InvocationDependencyTransactionOwner::
                               SelectedRepositoryProvider
            ? 'e'
            : 'd';
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session, inventory_owner, transaction_token(token_digit), {0}),
        "aggregate fixture token registration failed");
    const CleanupCurrentInstalledObservation current =
        make_cleanup_current_observation_for_test(
            session, absent_snapshot());
    const CleanupPolicyObservation policy =
        make_cleanup_policy_observation_for_test(
            session, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    return aggregate_remote_aur_cleanup_invocation_evidence(
        session, lifecycle, baseline, current, policy, {}, {});
}

InvocationOwnedCleanupCandidateProjectionSuccess require_projection(
    InvocationOwnedCleanupCandidateProjectionResult result,
    const std::string& context) {
    auto* success = std::get_if<
        InvocationOwnedCleanupCandidateProjectionSuccess>(&result);
    if(success == nullptr) {
        throw std::runtime_error(context + ": projection failed");
    }
    return std::move(*success);
}

InvocationOwnedCleanupCandidateProjectionSuccess project_basic_candidate(
    const BuildPlan& plan,
    const ResolvedDependencyCandidate& candidate_authority,
    const CleanupInvocationLifecycleEvidence& lifecycle,
    InstalledPackageStateSnapshotResult baseline = absent_snapshot(),
    InstalledPackageStateSnapshotResult current = present_snapshot(
        "build-tool", "1.0-1",
        InstalledPackageReason::Dependency, "build-tools"),
    const std::vector<SelectedRepositoryProviderTransactionResult>&
        provider_transactions = {}) {
    return require_projection(
        project_invocation_owned_cleanup_candidate(
            baseline, current, plan, candidate_authority, lifecycle,
            provider_transactions),
        "basic cleanup candidate");
}

void test_snapshot_observation_projection() {
    InstalledPackageStateSnapshotResult preexisting = present_snapshot(
        "build-tool", "1.0-1",
        InstalledPackageReason::Explicit, "build-tools");
    expect(
        project_cleanup_baseline_observation(
            preexisting, "build-tool") ==
            CleanupBaselineObservation::PreExisting,
        "pre-existing snapshot was not projected as PreExisting");
    expect(
        project_cleanup_baseline_observation(
            absent_snapshot(), "build-tool") ==
            CleanupBaselineObservation::NewlyObserved,
        "successful absence was not projected as NewlyObserved");
    expect(
        project_cleanup_baseline_observation(
            failed_snapshot(), "build-tool") ==
            CleanupBaselineObservation::Unknown,
        "failed baseline snapshot asserted absence");

    const CleanupCurrentPackageEvidence present =
        project_cleanup_current_package_evidence(
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"),
            "build-tool");
    expect(
        present.state == CleanupInstalledState::Present &&
            present.metadata.has_value() &&
            present.metadata->reason ==
                InstalledPackageReason::Dependency &&
            present.verification ==
                CleanupEvidenceVerification::Verified,
        "current present metadata projection differs");
    const CleanupCurrentPackageEvidence absent =
        project_cleanup_current_package_evidence(
            absent_snapshot(), "build-tool");
    expect(
        absent.state == CleanupInstalledState::Absent &&
            !absent.metadata.has_value(),
        "successful current absence was not confirmed");
    const CleanupCurrentPackageEvidence unknown =
        project_cleanup_current_package_evidence(
            failed_snapshot(), "build-tool");
    expect(
        unknown.state == CleanupInstalledState::Unknown &&
            !unknown.metadata.has_value() &&
            unknown.verification ==
                CleanupEvidenceVerification::Unverified,
        "failed current snapshot asserted absence or metadata");
}

void test_current_lifecycle_never_proves_causal_ownership() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(session, result);

    expect(
        project_cleanup_causal_ownership(lifecycle, {}) ==
            CleanupCausalOwnership::Unknown,
        "successful makepkg/install lifecycle proved package ownership");

    const ProvidedDependency provider = repository_provider();
    SelectedRepositoryProviderTransactionResult succeeded;
    succeeded.status =
        SelectedRepositoryProviderTransactionStatus::Succeeded;
    succeeded.selected_providers = {provider};
    succeeded.package_state_change = PackageStateChange::Unknown;
    succeeded.command_exit_status = 0;
    expect(
        project_cleanup_causal_ownership(lifecycle, {succeeded}) ==
            CleanupCausalOwnership::Unknown,
        "successful --needed provider transaction with Unknown change proved ownership");

    SelectedRepositoryProviderTransactionResult aggregate_changed =
        succeeded;
    aggregate_changed.selected_providers.push_back(
        ProvidedDependency::from_repository_constraint_metadata(
            "extra", 1, "second-provider", "second-provider-base",
            ProviderConstraintMetadata{
                ProviderCapability(
                    "other-virtual", "other-virtual",
                    std::nullopt),
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryExactPackage,
                    "1.0-1"),
                ObservedVersion::unknown(
                    ObservedVersionSource::
                        RepositoryProviderCapability,
                    ObservedVersionUnknownReason::
                        UnversionedProviderCapability)}));
    aggregate_changed.package_state_change = PackageStateChange::Changed;
    expect(
        project_cleanup_causal_ownership(
            lifecycle, {aggregate_changed}) ==
            CleanupCausalOwnership::Unknown,
        "multi-provider aggregate Changed proved package ownership");
}

void test_authoritative_install_receipt_projects_only_causal_dimension() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    const ResolvedDependencyCandidate& candidate =
        plan.dependency_edges.front().resolved_candidate.value();
    const std::string token = transaction_token();
    const auto owner = InvocationDependencyTransactionOwner::
        SourceArtifactInstall;
    InvocationDependencyTransactionLedger ledger{{dependency_transaction(
        token, owner, {"requested-parent"},
        InvocationDependencyTransactionCommandOutcome::Succeeded,
        transaction_receipt(
            token, owner,
            {{PacmanTransactionPackageOperation::Install,
              "build-tool"}}))}};

    expect(
        project_cleanup_causal_ownership_from_raw_ledger_for_test(
            "build-tool", CleanupBaselineObservation::NewlyObserved,
            project_cleanup_current_package_evidence(
                present_snapshot(
                    "build-tool", "1.0-1",
                    InstalledPackageReason::Dependency, "build-tools"),
                "build-tool"),
            ledger) == CleanupCausalOwnership::InvocationOwned,
        "test-only factual ledger regression lost its Install semantics");

    InvocationOwnedCleanupCandidateProjectionSuccess projection =
        project_basic_candidate(
            plan, candidate, lifecycle, absent_snapshot(),
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    expect(
        projection.candidate.causal_ownership ==
                CleanupCausalOwnership::Unknown &&
            has_issue(
                projection,
                CleanupLifecycleProjectionIssueKind::
                    CausalOwnershipUnavailable),
        "raw ledger remained reachable from production candidate projection");

    const CleanupClassificationResult classified =
        classify_invocation_owned_cleanup(projection.candidate);
    expect(
        projection.candidate.policy_protection ==
                CleanupPolicyProtection::Unknown &&
            classified.classification() ==
                CleanupClassification::Unknown &&
            !has_reason(
                classified,
                CleanupClassificationReason::
                    CausalOwnershipUnknown) &&
            has_reason(
                classified,
                CleanupClassificationReason::PolicyProtectionUnknown),
        "generic-ledger firewall bypassed Unknown candidate authority");
}

void test_upgrade_and_external_install_race_do_not_project_ownership() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    const ResolvedDependencyCandidate& candidate =
        plan.dependency_edges.front().resolved_candidate.value();
    const std::string token = transaction_token();
    const auto owner = InvocationDependencyTransactionOwner::
        SelectedRepositoryProvider;
    InvocationDependencyTransactionLedger ledger{{dependency_transaction(
        token, owner, {"build-tool"},
        InvocationDependencyTransactionCommandOutcome::Succeeded,
        transaction_receipt(
            token, owner,
            {{PacmanTransactionPackageOperation::Upgrade,
              "build-tool"},
             {PacmanTransactionPackageOperation::Install,
              "solver-introduced-tool"}}))}};

    expect(
        project_cleanup_causal_ownership_from_raw_ledger_for_test(
            "build-tool", CleanupBaselineObservation::NewlyObserved,
            project_cleanup_current_package_evidence(
                present_snapshot(
                    "build-tool", "1.0-1",
                    InstalledPackageReason::Dependency, "build-tools"),
                "build-tool"),
            ledger) == CleanupCausalOwnership::Unknown,
        "test-only Upgrade receipt became an Install");

    InvocationOwnedCleanupCandidateProjectionSuccess projection =
        project_basic_candidate(
            plan, candidate, lifecycle, absent_snapshot(),
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    expect(
        projection.candidate.causal_ownership ==
                CleanupCausalOwnership::Unknown &&
            has_issue(
                projection,
                CleanupLifecycleProjectionIssueKind::
                    CausalOwnershipUnavailable),
        "Upgrade after an external Install became invocation ownership");
}

void test_failed_missing_and_mismatched_receipts_remain_unknown() {
    const std::string token = transaction_token();
    const auto owner = InvocationDependencyTransactionOwner::
        SelectedRepositoryProvider;
    PacmanTransactionReceipt install_receipt = transaction_receipt(
        token, owner,
        {{PacmanTransactionPackageOperation::Install, "build-tool"}});

    const auto expect_unknown = [&](InvocationDependencyTransaction transaction,
                                    const std::string& context) {
        InvocationDependencyTransactionLedger ledger{
            {std::move(transaction)}};
        expect(
            project_cleanup_causal_ownership_from_raw_ledger_for_test(
                "build-tool", CleanupBaselineObservation::NewlyObserved,
                project_cleanup_current_package_evidence(
                    present_snapshot(
                        "build-tool", "1.0-1",
                        InstalledPackageReason::Dependency, "build-tools"),
                    "build-tool"),
                ledger) == CleanupCausalOwnership::Unknown,
            context);
    };

    expect_unknown(
        dependency_transaction(
            token, owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::Failed,
            install_receipt),
        "failed transaction command projected InvocationOwned");
    expect_unknown(
        dependency_transaction(
            token, owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::Unknown,
            install_receipt),
        "unknown transaction outcome projected InvocationOwned");
    expect_unknown(
        dependency_transaction(
            token, owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::
                NotAttempted,
            install_receipt),
        "not-attempted transaction projected InvocationOwned");
    expect_unknown(
        dependency_transaction(
            token, owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::Succeeded,
            transaction_receipt(
                token, owner, {},
                PacmanTransactionReceiptObservationState::Missing)),
        "missing receipt projected InvocationOwned");
    expect_unknown(
        dependency_transaction(
            transaction_token('b'), owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::Succeeded,
            install_receipt),
        "ledger/receipt transaction token mismatch projected InvocationOwned");
}

void test_multiple_transaction_attribution_is_not_flattened() {
    const auto owner = InvocationDependencyTransactionOwner::
        SourceArtifactInstall;
    const std::string install_token = transaction_token('a');
    const std::string other_token = transaction_token('b');
    const std::string upgrade_token = transaction_token('c');
    InvocationDependencyTransactionLedger ledger{{
        dependency_transaction(
            install_token, owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::Succeeded,
            transaction_receipt(
                install_token, owner,
                {{PacmanTransactionPackageOperation::Install,
                  "build-tool"}})),
        dependency_transaction(
            other_token, owner, {"other-tool"},
            InvocationDependencyTransactionCommandOutcome::Succeeded,
            transaction_receipt(
                other_token, owner,
                {{PacmanTransactionPackageOperation::Install,
                  "other-tool"}})),
        dependency_transaction(
            upgrade_token, owner, {"build-tool"},
            InvocationDependencyTransactionCommandOutcome::Succeeded,
            transaction_receipt(
                upgrade_token, owner,
                {{PacmanTransactionPackageOperation::Upgrade,
                  "build-tool"}})),
    }};

    const CleanupCausalOwnership raw_projection =
        project_cleanup_causal_ownership_from_raw_ledger_for_test(
            "build-tool", CleanupBaselineObservation::NewlyObserved,
            project_cleanup_current_package_evidence(
                present_snapshot(
                    "build-tool", "1.0-1",
                    InstalledPackageReason::Dependency, "build-tools"),
                "build-tool"),
            ledger);
    expect(
        ledger.transactions.size() == 3 &&
            ledger.transactions[0]
                    .receipt.package_operations()
                    .front()
                    .operation ==
                PacmanTransactionPackageOperation::Install &&
            ledger.transactions[2]
                    .receipt.package_operations()
                    .front()
                    .operation ==
                PacmanTransactionPackageOperation::Upgrade &&
            raw_projection ==
                CleanupCausalOwnership::InvocationOwned,
        "Install and later Upgrade evidence was flattened or misattributed");
}

void test_receipt_does_not_bypass_protection_precedence() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    const ResolvedDependencyCandidate& candidate =
        plan.dependency_edges.front().resolved_candidate.value();
    const std::string token = transaction_token();
    const auto owner = InvocationDependencyTransactionOwner::
        SourceArtifactInstall;
    InvocationDependencyTransactionLedger ledger{{dependency_transaction(
        token, owner, {"build-tool"},
        InvocationDependencyTransactionCommandOutcome::Succeeded,
        transaction_receipt(
            token, owner,
            {{PacmanTransactionPackageOperation::Install,
              "build-tool"}}))}};
    expect(
        project_cleanup_causal_ownership_from_raw_ledger_for_test(
            "build-tool", CleanupBaselineObservation::NewlyObserved,
            project_cleanup_current_package_evidence(
                present_snapshot(
                    "build-tool", "1.0-1",
                    InstalledPackageReason::Dependency, "build-tools"),
                "build-tool"),
            ledger) == CleanupCausalOwnership::InvocationOwned,
        "test-only receipt control was not authoritative");

    InvocationOwnedCleanupCandidateProjectionSuccess preexisting =
        project_basic_candidate(
            plan, candidate, lifecycle,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"),
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    expect(
        preexisting.candidate.causal_ownership ==
                CleanupCausalOwnership::Unknown &&
            classify_invocation_owned_cleanup(preexisting.candidate)
                    .classification() ==
                CleanupClassification::Protected,
        "pre-existing package receipt contradiction bypassed protection");

    InvocationOwnedCleanupCandidateProjectionSuccess explicit_package =
        project_basic_candidate(
            plan, candidate, lifecycle, absent_snapshot(),
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Explicit, "build-tools"));
    expect(
        explicit_package.candidate.causal_ownership ==
                CleanupCausalOwnership::Unknown &&
            classify_invocation_owned_cleanup(
                explicit_package.candidate)
                    .classification() ==
                CleanupClassification::Protected,
        "Explicit package with receipt was not Protected");

    BuildPlan runtime_plan = mixed_runtime_plan();
    CleanupInvocationSession runtime_session =
        CleanupInvocationSession::begin(
            prepared_remote_aur_build(runtime_plan));
    PreparedProductionSourceBuildInvocation runtime_invocation =
        prepared_invocation(runtime_plan);
    ProductionSourceBuildInvocationResult runtime_result =
        successful_result(runtime_invocation);
    CleanupInvocationLifecycleEvidence runtime_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            runtime_session, runtime_result);
    const ResolvedDependencyCandidate& runtime_candidate =
        runtime_plan.dependency_edges.front()
            .resolved_candidate.value();
    InvocationOwnedCleanupCandidateProjectionSuccess runtime =
        project_basic_candidate(
            runtime_plan, runtime_candidate, runtime_lifecycle,
            absent_snapshot(),
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    expect(
        runtime.candidate.causal_ownership ==
                CleanupCausalOwnership::Unknown &&
            classify_invocation_owned_cleanup(runtime.candidate)
                    .classification() ==
                CleanupClassification::Protected,
        "Runtime dependency with receipt was not Protected");
}

void test_complete_plan_projects_complete_correlations_and_lifetime() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(session, result);
    const ResolvedDependencyCandidate& candidate =
        plan.dependency_edges.front().resolved_candidate.value();

    InvocationOwnedCleanupCandidateProjectionSuccess projection =
        project_basic_candidate(plan, candidate, lifecycle);
    expect(
        projection.candidate.correlation_coverage ==
            CleanupCorrelationCoverage::Complete,
        "complete BuildPlan did not project Complete coverage");
    expect(
        projection.candidate.correlations.size() == 1 &&
            projection.candidate.correlations.front().role ==
                PackageRole::BuildDependency &&
            projection.candidate.correlations.front().verification ==
                CleanupEvidenceVerification::Verified,
        "complete BuildPlan correlation was flattened or unverified");
    expect(
        projection.candidate.shared_requirement ==
            CleanupSharedRequirementState::NoLongerRequired,
        "full successful invocation did not close build-only lifetime");
    expect(
        projection.candidate.package.package().package_base().source().location().state() == SourceLocationState::Known,
        "prepared AUR source identity was not retained");
}

void test_incomplete_plan_states_never_project_complete_coverage() {
    auto expect_not_complete = [](BuildPlan plan, const std::string& context) {
        CleanupInvocationSession session = CleanupInvocationSession::begin(
            prepared_remote_aur_build(plan));
        PreparedProductionSourceBuildInvocation invocation =
            prepared_invocation(plan);
        CleanupInvocationLifecycleEvidence lifecycle =
            CleanupInvocationLifecycleEvidence::
                before_build_completion(session);
        InvocationOwnedCleanupCandidateProjectionSuccess projection =
            project_basic_candidate(
                plan,
                plan.dependency_edges.front()
                    .resolved_candidate.value(),
                lifecycle);
        expect(
            projection.candidate.correlation_coverage !=
                CleanupCorrelationCoverage::Complete,
            context + " projected Complete coverage");
    };

    BuildPlan resolution_failure = basic_plan();
    resolution_failure.resolution_failures.push_back(
        BuildPlanResolutionFailure{
            BuildPlanResolutionFailureKind::
                AurPackageMetadataUnavailable,
            "application", "application", "missing-dependency",
            "missing-dependency", resolution_failure.root_targets,
            "typed resolution failure"});
    expect_not_complete(
        std::move(resolution_failure), "resolution failure");

    BuildPlan unresolved = basic_plan();
    unresolved.unresolved.push_back("missing-dependency");
    expect_not_complete(std::move(unresolved), "unresolved dependency");

    BuildPlan ambiguous = basic_plan();
    ambiguous.ambiguous_providers.push_back(
        AmbiguousProvidedDependency{
            "virtual-tool",
            {ProvidedDependency::from_aur("provider-one")}});
    expect_not_complete(std::move(ambiguous), "ambiguous provider");

    BuildPlan cancelled = basic_plan();
    cancelled.cancelled_provider_dependencies.push_back("virtual-tool");
    expect_not_complete(std::move(cancelled), "cancelled provider");

    BuildPlan incomplete_candidates = basic_plan();
    incomplete_candidates.incomplete_provider_candidate_sets.push_back(
        IncompleteProviderCandidateSet{
            "virtual-tool", {}, ObservedVersionUnknownReason::PartialSourceFailure});
    expect_not_complete(
        std::move(incomplete_candidates),
        "incomplete provider candidate set");
}

void test_repository_provider_package_base_is_verified() {
    BuildPlan plan = repository_provider_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::
            before_build_completion(session);
    ResolvedDependencyCandidate candidate = repository_exact_candidate();

    InvocationOwnedCleanupCandidateProjectionSuccess projection =
        project_basic_candidate(
            plan, candidate, lifecycle, absent_snapshot(),
            present_snapshot(
                "repository-tool", "2.0-1",
                InstalledPackageReason::Dependency, "repository-tools"));
    expect(
        projection.candidate.correlation_coverage ==
            CleanupCorrelationCoverage::Complete,
        "repository provider PackageBase did not keep Complete coverage");
    expect(
        !projection.candidate.correlations.empty() &&
            projection.candidate.correlations.front().verification ==
                CleanupEvidenceVerification::Verified,
        "repository provider PackageBase did not become Verified");
    expect(
        !has_issue(
            projection,
            CleanupLifecycleProjectionIssueKind::
                RepositoryProviderPackageBaseUnavailable),
        "authoritative repository provider PackageBase was discarded");
}

void test_mixed_roles_and_multiple_roots_are_preserved() {
    BuildPlan mixed = mixed_runtime_plan();
    CleanupInvocationSession mixed_session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(mixed));
    PreparedProductionSourceBuildInvocation mixed_invocation =
        prepared_invocation(mixed);
    ProductionSourceBuildInvocationResult mixed_result =
        successful_result(mixed_invocation);
    CleanupInvocationLifecycleEvidence mixed_lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(
                mixed_session, mixed_result);
    InvocationOwnedCleanupCandidateProjectionSuccess mixed_projection =
        project_basic_candidate(
            mixed,
            mixed.dependency_edges.front()
                .resolved_candidate.value(),
            mixed_lifecycle);
    bool saw_runtime = false;
    bool saw_build = false;
    for(const CleanupPackageCorrelation& correlation :
        mixed_projection.candidate.correlations) {
        saw_runtime |= correlation.role == PackageRole::RuntimeDependency;
        saw_build |= correlation.role == PackageRole::BuildDependency;
    }
    const CleanupClassificationResult mixed_classification =
        classify_invocation_owned_cleanup(mixed_projection.candidate);
    expect(
        saw_runtime && saw_build &&
            mixed_projection.candidate.shared_requirement ==
                CleanupSharedRequirementState::StillRequired &&
            mixed_classification.classification() ==
                CleanupClassification::Protected,
        "Build + Runtime roles were flattened or not protected");

    BuildPlan multiple = multiple_root_plan();
    CleanupInvocationSession multiple_session =
        CleanupInvocationSession::begin(
            prepared_remote_aur_build(multiple));
    PreparedProductionSourceBuildInvocation multiple_invocation =
        prepared_invocation(multiple);
    ProductionSourceBuildInvocationResult multiple_result =
        successful_result(multiple_invocation);
    CleanupInvocationLifecycleEvidence multiple_lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(
                multiple_session, multiple_result);
    InvocationOwnedCleanupCandidateProjectionSuccess multiple_projection =
        project_basic_candidate(
            multiple,
            multiple.dependency_edges.front()
                .resolved_candidate.value(),
            multiple_lifecycle);
    expect(
        multiple_projection.candidate.correlation_coverage ==
                CleanupCorrelationCoverage::Complete &&
            multiple_projection.candidate.correlations.size() == 2 &&
            multiple_projection.candidate.correlations[0]
                    .requested_root !=
                multiple_projection.candidate.correlations[1]
                    .requested_root &&
            multiple_projection.candidate.correlations[0]
                    .dependency_edge->requiring_package
                    .package_base()
                    .package_base() !=
                multiple_projection.candidate.correlations[1]
                    .dependency_edge->requiring_package
                    .package_base()
                    .package_base(),
        "multiple roots/PackageBases were not retained");
}

void test_later_work_item_and_unknown_lifecycle_fail_safe() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult partial =
        result_after_work_item(invocation, 0);
    CleanupInvocationLifecycleEvidence after_dependency =
        CleanupInvocationLifecycleEvidence::after_work_item(
            session, partial, 0);
    InvocationOwnedCleanupCandidateProjectionSuccess still_required =
        project_basic_candidate(
            plan,
            plan.dependency_edges.front()
                .resolved_candidate.value(),
            after_dependency);
    expect(
        still_required.candidate.shared_requirement ==
            CleanupSharedRequirementState::StillRequired,
        "later work item requirement was not StillRequired");

    CleanupInvocationLifecycleEvidence unknown_lifecycle =
        CleanupInvocationLifecycleEvidence::unknown();
    InvocationOwnedCleanupCandidateProjectionSuccess unknown =
        project_basic_candidate(
            plan,
            plan.dependency_edges.front()
                .resolved_candidate.value(),
            unknown_lifecycle);
    expect(
        unknown.candidate.shared_requirement ==
                CleanupSharedRequirementState::Unknown &&
            unknown.candidate.correlation_coverage !=
                CleanupCorrelationCoverage::Complete,
        "unknown lifecycle asserted completion or lifetime");
}

void test_local_remote_dependency_subset_is_not_complete_authority() {
    BuildPlan plan = basic_plan();
    PreparedProductionSourceBuildInvocation remote_dependencies =
        prepared_invocation(plan);
    // Local production preparation owns only remote dependency units; the
    // local root remains with the local lifecycle owner. A subset must not be
    // treated as the complete BuildPlan/work-item set.
    remote_dependencies.work_items.pop_back();
    PreparedRemoteSourceBuild prepared = prepared_remote_aur_build(plan);
    prepared.invocation = std::move(remote_dependencies);
    CleanupInvocationSession session =
        CleanupInvocationSession::begin(std::move(prepared));
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::before_build_completion(
            session);
    InvocationOwnedCleanupCandidateProjectionSuccess projection =
        project_basic_candidate(
            plan,
            plan.dependency_edges.front()
                .resolved_candidate.value(),
            lifecycle);
    expect(
        projection.candidate.correlation_coverage ==
                CleanupCorrelationCoverage::Incomplete &&
            projection.candidate.shared_requirement ==
                CleanupSharedRequirementState::Unknown,
        "local remote-dependency subset became complete lifecycle authority");
}

void test_metadata_and_source_failures_remain_typed_unknown_evidence() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(session, result);
    const ResolvedDependencyCandidate& candidate =
        plan.dependency_edges.front().resolved_candidate.value();

    InvocationOwnedCleanupCandidateProjectionSuccess metadata_failure =
        project_basic_candidate(
            plan, candidate, lifecycle, failed_snapshot(),
            failed_snapshot());
    const CleanupClassificationResult metadata_classification =
        classify_invocation_owned_cleanup(metadata_failure.candidate);
    expect(
        metadata_failure.candidate.baseline ==
                CleanupBaselineObservation::Unknown &&
            metadata_failure.candidate.current_package.state ==
                CleanupInstalledState::Unknown &&
            metadata_failure.candidate.correlations.front()
                    .verification ==
                CleanupEvidenceVerification::Unverified &&
            metadata_classification.classification() ==
                CleanupClassification::Unknown &&
            has_issue(
                metadata_failure,
                CleanupLifecycleProjectionIssueKind::
                    BaselineSnapshotUnavailable) &&
            has_issue(
                metadata_failure,
                CleanupLifecycleProjectionIssueKind::
                    CurrentSnapshotUnavailable),
        "metadata failure became absence, verified evidence, or Eligible");

    PreparedProductionSourceBuildInvocation source_incomplete =
        prepared_invocation(plan);
    source_incomplete.work_items.front()
        .request.aur_review_identity.reset();
    PreparedRemoteSourceBuild source_prepared =
        prepared_remote_aur_build(plan);
    source_prepared.invocation = std::move(source_incomplete);
    CleanupInvocationSession source_session =
        CleanupInvocationSession::begin(std::move(source_prepared));
    CleanupInvocationLifecycleEvidence source_lifecycle =
        CleanupInvocationLifecycleEvidence::before_build_completion(
            source_session);
    InvocationOwnedCleanupCandidateProjectionSuccess source_failure =
        project_basic_candidate(
            plan, candidate, source_lifecycle);
    expect(
        source_failure.candidate.correlation_coverage !=
                CleanupCorrelationCoverage::Complete &&
            source_failure.candidate.correlations.front()
                    .verification ==
                CleanupEvidenceVerification::Unverified,
        "source correlation failure became complete/verified");
}

void test_version_mismatch_and_unknown_policy_fail_closed() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(session, result);
    const ResolvedDependencyCandidate& candidate =
        plan.dependency_edges.front().resolved_candidate.value();

    InvocationOwnedCleanupCandidateProjectionSuccess mismatch =
        project_basic_candidate(
            plan, candidate, lifecycle, absent_snapshot(),
            present_snapshot(
                "build-tool", "2.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    expect(
        classify_invocation_owned_cleanup(mismatch.candidate)
                .classification() ==
            CleanupClassification::Invalid,
        "current installed version mismatch was not Invalid");

    InvocationOwnedCleanupCandidateProjectionSuccess policy_unknown =
        project_basic_candidate(plan, candidate, lifecycle);
    const CleanupClassificationResult policy_result =
        classify_invocation_owned_cleanup(policy_unknown.candidate);
    expect(
        policy_unknown.candidate.policy_protection ==
                CleanupPolicyProtection::Unknown &&
            policy_result.classification() ==
                CleanupClassification::Unknown &&
            has_reason(
                policy_result,
                CleanupClassificationReason::
                    PolicyProtectionUnknown),
        "unknown policy authority permitted Eligible");
}

// POLICY(#486): the legacy adapter still lacks policy authority. Relaxing
// strict causal proof must not hide that independent safety requirement.
void test_newly_observed_dependency_with_unknown_policy_is_not_eligible() {
    BuildPlan plan = basic_plan();
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    PreparedProductionSourceBuildInvocation invocation =
        prepared_invocation(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::
            after_successful_invocation(session, result);
    InvocationOwnedCleanupCandidateProjectionSuccess projection =
        project_basic_candidate(
            plan,
            plan.dependency_edges.front()
                .resolved_candidate.value(),
            lifecycle);

    expect(
        projection.candidate.baseline ==
                CleanupBaselineObservation::NewlyObserved &&
            projection.candidate.current_package.state ==
                CleanupInstalledState::Present &&
            projection.candidate.current_package.metadata->reason ==
                InstalledPackageReason::Dependency &&
            projection.candidate.correlation_coverage ==
                CleanupCorrelationCoverage::Complete &&
            projection.candidate.correlations.front().verification ==
                CleanupEvidenceVerification::Verified &&
            projection.candidate.shared_requirement ==
                CleanupSharedRequirementState::NoLongerRequired,
        "landmine did not assemble the intended observational evidence");
    const CleanupClassificationResult classified =
        classify_invocation_owned_cleanup(projection.candidate);
    expect(
        projection.candidate.causal_ownership ==
                CleanupCausalOwnership::Unknown &&
            projection.candidate.policy_protection ==
                CleanupPolicyProtection::Unknown &&
            classified.classification() ==
                CleanupClassification::Unknown &&
            !has_reason(
                classified,
                CleanupClassificationReason::
                    CausalOwnershipUnknown) &&
            has_reason(classified,
                       CleanupClassificationReason::PolicyProtectionUnknown),
        "pre absent + post Dependency + verified plan + success bypassed unknown policy");
}

void test_source_artifact_exact_build_plan_correlation_matrix() {
    BuildPlan plan = basic_plan();
    PreparedRemoteSourceBuild prepared = prepared_remote_aur_build(plan);
    ProductionSourceBuildInvocationResult result =
        successful_result(prepared.invocation);
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        std::move(prepared));
    const CleanupBaselineSnapshotObservation baseline =
        make_cleanup_baseline_observation_for_test(
            session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "source transaction token registration failed");
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    const std::vector<RootTargetIdentity> roots = plan.root_targets;

    const auto correlate = [&](SourceArtifactInstallCausalEvidence causal) {
        return correlate_source_artifact_install_to_build_plan(
            session, lifecycle, causal);
    };

    CleanupSourceArtifactCorrelationEvidence exact = correlate(
        source_artifact_causal_evidence(
            session, 0, "build-tool", "build-tools", roots,
            {PackageRole::BuildDependency}, {0}));
    expect(
        exact.completeness() == CleanupEvidenceCompleteness::Complete &&
            exact.selected_artifacts().size() == 1 &&
            exact.selected_artifacts().front().dependency_correlations.size() == 1 &&
            exact.selected_artifacts().front().dependency_correlations.front().dependency_edge->build_plan_edge_index == 0,
        "exact source-artifact work item did not close its BuildPlan edge");

    CleanupInvocationSession other_session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    SourceArtifactInstallCausalEvidence wrong_invocation_causal =
        source_artifact_causal_evidence(
            other_session, 0, "build-tool", "build-tools", roots,
            {PackageRole::BuildDependency}, {0});
    expect(
        wrong_invocation_causal.work_item().invocation_authority.value() !=
            session.authority(),
        "source causal fixture lost its distinct invocation");
    CleanupSourceArtifactCorrelationEvidence wrong_invocation = correlate(
        std::move(wrong_invocation_causal));
    expect(
        wrong_invocation.completeness() !=
            CleanupEvidenceCompleteness::Complete,
        "different source-artifact invocation correlated");
    expect(
        has_source_correlation_issue(
            wrong_invocation,
            CleanupSourceArtifactCorrelationIssueKind::
                InvocationMismatch),
        "different source-artifact invocation lost its typed issue");

    CleanupSourceArtifactCorrelationEvidence wrong_work_item = correlate(
        source_artifact_causal_evidence(
            session, 1, "build-tool", "build-tools", roots,
            {PackageRole::BuildDependency}, {0}));
    expect(
        wrong_work_item.completeness() !=
                CleanupEvidenceCompleteness::Complete &&
            has_source_correlation_issue(
                wrong_work_item,
                CleanupSourceArtifactCorrelationIssueKind::
                    WorkItemPackageBaseMismatch),
        "different source-artifact work item correlated");

    CleanupSourceArtifactCorrelationEvidence wrong_package_base = correlate(
        source_artifact_causal_evidence(
            session, 0, "build-tool", "other-build-tools",
            roots, {PackageRole::BuildDependency}, {0}));
    expect(
        wrong_package_base.completeness() !=
            CleanupEvidenceCompleteness::Complete,
        "different source-artifact PackageBase correlated");

    CleanupSourceArtifactCorrelationEvidence wrong_root = correlate(
        source_artifact_causal_evidence(
            session, 0, "build-tool", "build-tools",
            {root(9, "other-root")},
            {PackageRole::BuildDependency}, {0}));
    expect(
        wrong_root.completeness() != CleanupEvidenceCompleteness::Complete &&
            has_source_correlation_issue(
                wrong_root,
                CleanupSourceArtifactCorrelationIssueKind::
                    RequestedRootAttributionMismatch),
        "different source-artifact root attribution correlated");

    CleanupSourceArtifactCorrelationEvidence wrong_edge = correlate(
        source_artifact_causal_evidence(
            session, 0, "build-tool", "build-tools", roots,
            {PackageRole::BuildDependency}, {1}));
    expect(
        wrong_edge.completeness() != CleanupEvidenceCompleteness::Complete &&
            has_source_correlation_issue(
                wrong_edge,
                CleanupSourceArtifactCorrelationIssueKind::
                    DependencyEdgeAttributionMismatch),
        "different source-artifact dependency edge correlated");

    CleanupSourceArtifactCorrelationEvidence wrong_role = correlate(
        source_artifact_causal_evidence(
            session, 0, "build-tool", "build-tools", roots,
            {PackageRole::CheckDependency}, {0}));
    expect(
        wrong_role.completeness() != CleanupEvidenceCompleteness::Complete &&
            has_source_correlation_issue(
                wrong_role,
                CleanupSourceArtifactCorrelationIssueKind::
                    DependencyRoleMismatch),
        "different source-artifact dependency role correlated");

    BuildPlan shared_plan = multiple_root_plan();
    PreparedRemoteSourceBuild shared_prepared =
        prepared_remote_aur_build(shared_plan);
    ProductionSourceBuildInvocationResult shared_result =
        successful_result(shared_prepared.invocation);
    CleanupInvocationSession shared_session =
        CleanupInvocationSession::begin(std::move(shared_prepared));
    CleanupInvocationLifecycleEvidence shared_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            shared_session, shared_result);
    CleanupSourceArtifactCorrelationEvidence wrong_edge_subset =
        correlate_source_artifact_install_to_build_plan(
            shared_session, shared_lifecycle,
            source_artifact_causal_evidence(
                shared_session, 0, "build-tool", "build-tools",
                {shared_plan.root_targets.front()},
                {PackageRole::BuildDependency}, {0}));
    expect(
        wrong_edge_subset.completeness() !=
            CleanupEvidenceCompleteness::Complete,
        "one of two exact dependency edges was treated as complete");

    BuildPlan duplicate_edge_plan = basic_plan();
    duplicate_edge_plan.dependency_edges.push_back(
        duplicate_edge_plan.dependency_edges.front());
    PreparedRemoteSourceBuild duplicate_edge_prepared =
        prepared_remote_aur_build(duplicate_edge_plan);
    ProductionSourceBuildInvocationResult duplicate_edge_result =
        successful_result(duplicate_edge_prepared.invocation);
    CleanupInvocationSession duplicate_edge_session =
        CleanupInvocationSession::begin(std::move(duplicate_edge_prepared));
    CleanupInvocationLifecycleEvidence duplicate_edge_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            duplicate_edge_session, duplicate_edge_result);
    CleanupSourceArtifactCorrelationEvidence duplicate_edge_subset =
        correlate_source_artifact_install_to_build_plan(
            duplicate_edge_session, duplicate_edge_lifecycle,
            source_artifact_causal_evidence(
                duplicate_edge_session, 0, "build-tool", "build-tools",
                duplicate_edge_plan.root_targets,
                {PackageRole::BuildDependency}, {0}));
    expect(
        duplicate_edge_subset.completeness() !=
            CleanupEvidenceCompleteness::Complete,
        "same package/version/root/role from a different exact edge was accepted");
}

void test_selected_repository_provider_closed_correlation_matrix() {
    BuildPlan plan = cargo_provider_plan();
    PreparedRemoteSourceBuild prepared =
        prepared_remote_aur_build(plan);
    const PreparedProductionSourceBuildInvocation invocation =
        prepared.invocation;
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        std::move(prepared));
    const CleanupBaselineSnapshotObservation baseline =
        make_cleanup_baseline_observation_for_test(
            session, absent_snapshot());
    expect(
        baseline.phase() ==
                CleanupObservationPhase::BeforeFirstDependencyMutation &&
            register_cleanup_invocation_transaction_token_for_test(
                session,
                InvocationDependencyTransactionOwner::
                    SelectedRepositoryProvider,
                transaction_token('e'), {0}),
        "selected-provider baseline/token fixture failed");
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    const CleanupCurrentInstalledObservation current =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust"));
    const CleanupPolicyObservation policy =
        make_cleanup_policy_observation_for_test(
            session, base_policy_evidence());
    const SelectedRepositoryProviderTrustedExecutionEvidence execution =
        selected_provider_execution(
            session, plan, invocation);

    CleanupSelectedProviderCorrelationEvidence exact =
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, execution);
    expect(
        exact.completeness() == CleanupEvidenceCompleteness::Complete &&
            exact.edge_correlations().size() == 1 &&
            exact.edge_correlations().front().provider.package_name ==
                "rust" &&
            exact.edge_correlations().front().provider.package_base ==
                "rust" &&
            exact.edge_correlations()
                    .front()
                    .provider.provided_dependency_name == "cargo" &&
            exact.edge_correlations()
                    .front()
                    .package_identity.package()
                    .package_name() == "rust",
        "cargo requirement, rust provider, PackageBase, receipt, and current identity did not close");
    CleanupInvocationEvidence provider_route =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session, lifecycle, baseline, current, policy, {}, {exact});
    expect(
        provider_route.route_authority() ==
                CleanupRouteAuthority::Complete &&
            provider_route.completeness() ==
                CleanupEvidenceCompleteness::Complete &&
            provider_route.selected_provider_evidence().size() == 1 &&
            project_shared_requirement(
                provider_route,
                exact.edge_correlations().front().package_identity) ==
                CleanupSharedRequirementState::NoLongerRequired,
        "complete selected repository provider remote route was not authoritative");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_base_bindings = execution.bindings();
    wrong_base_bindings.front().provider.package_base = "wrong-rust-base";
    wrong_base_bindings.front().selected_decision.provider.package_base =
        "wrong-rust-base";
    const SelectedRepositoryProviderTrustedExecutionEvidence wrong_base =
        selected_provider_execution_variant(
            session, execution, std::move(wrong_base_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_base)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong repository provider PackageBase correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_package_bindings = execution.bindings();
    wrong_package_bindings.front().provider.package_name = "rust-alt";
    wrong_package_bindings.front().selected_decision.provider.package_name =
        "rust-alt";
    const auto wrong_package = selected_provider_execution_variant(
        session, execution, std::move(wrong_package_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_package)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong actual provider package correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_repository_bindings = execution.bindings();
    std::get<RepositoryProviderOrigin>(
        wrong_repository_bindings.front().provider.origin)
        .repository_name = "core";
    std::get<RepositoryProviderOrigin>(
        wrong_repository_bindings.front()
            .selected_decision.provider.origin)
        .repository_name = "core";
    const SelectedRepositoryProviderTrustedExecutionEvidence
        wrong_repository = selected_provider_execution_variant(
            session, execution, std::move(wrong_repository_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_repository)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong repository provenance correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_capability_bindings = execution.bindings();
    wrong_capability_bindings.front()
        .selected_decision.provider.provided_dependency_name = "rust";
    const SelectedRepositoryProviderTrustedExecutionEvidence
        wrong_capability = selected_provider_execution_variant(
            session, execution, std::move(wrong_capability_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_capability)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong provided capability correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_specification_bindings = execution.bindings();
    wrong_specification_bindings.front()
        .selected_decision.provider.provided_dependency_specification =
        "cargo=1.89.0";
    const auto wrong_specification = selected_provider_execution_variant(
        session, execution, std::move(wrong_specification_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_specification)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong provided specification correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_version_bindings = execution.bindings();
    wrong_version_bindings.front()
        .selected_decision.provider.constraint_metadata->provided_version =
        ObservedVersion::available(
            ObservedVersionSource::RepositoryProviderCapability,
            "1.89.0");
    const auto wrong_version = selected_provider_execution_variant(
        session, execution, std::move(wrong_version_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_version)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong provided version correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_architecture_bindings = execution.bindings();
    wrong_architecture_bindings.front().provider.package_architecture =
        "aarch64";
    wrong_architecture_bindings.front()
        .selected_decision.provider.package_architecture = "aarch64";
    const auto wrong_architecture = selected_provider_execution_variant(
        session, execution, std::move(wrong_architecture_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_architecture)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong provider architecture correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_requirement_bindings = execution.bindings();
    wrong_requirement_bindings.front().requirement = requirement("rust");
    const SelectedRepositoryProviderTrustedExecutionEvidence
        wrong_requirement = selected_provider_execution_variant(
            session, execution, std::move(wrong_requirement_bindings));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_requirement)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong typed requirement correlated");

    std::vector<SelectedRepositoryProviderTrustedExecutionBinding>
        wrong_decision_bindings = execution.bindings();
    wrong_decision_bindings.front().selected_decision.resolution =
        ProviderResolutionKind::Unique;
    const SelectedRepositoryProviderTrustedExecutionEvidence
        wrong_decision_execution = selected_provider_execution_variant(
            session, execution, std::move(wrong_decision_bindings));
    CleanupSelectedProviderCorrelationEvidence wrong_decision =
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_decision_execution);
    expect(
        wrong_decision.completeness() !=
                CleanupEvidenceCompleteness::Complete &&
            has_provider_correlation_issue(
                wrong_decision,
                CleanupSelectedProviderCorrelationIssueKind::
                    SelectedDecisionMismatch),
        "wrong selected-provider decision correlated");

    static_assert(!std::is_constructible_v<
                  SelectedRepositoryProviderTrustedExecutionEvidence,
                  SelectedRepositoryProviderTransactionResult,
                  TrustedAlpmReceiptCaptureResult>);
    static_assert(!std::is_constructible_v<
                  SelectedRepositoryProviderTrustedExecutionEvidence,
                  SourceArtifactInstallCausalEvidence>);
    const SelectedRepositoryProviderTrustedExecutionEvidence wrong_token =
        selected_provider_execution_variant(
            session, execution, execution.bindings(),
            transaction_token('f'));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, wrong_token)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong transaction token correlated");

    const CleanupCurrentInstalledObservation wrong_current =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.89.0-1",
                InstalledPackageReason::Dependency, "rust"));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, wrong_current, execution)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong current provider identity correlated");

    const CleanupCurrentInstalledObservation wrong_current_base =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "wrong-rust-base"));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, wrong_current_base, execution)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong current PackageBase correlated");

    const CleanupCurrentInstalledObservation missing_current_base =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, {},
                InstalledPackageBaseIdentity::missing()));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, missing_current_base, execution)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "missing current PackageBase correlated");

    const CleanupCurrentInstalledObservation missing_current_architecture =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust",
                InstalledPackageBaseIdentity::known("rust"),
                InstalledPackageArchitectureIdentity::missing()));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, missing_current_architecture, execution)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "missing current architecture correlated");

    const CleanupCurrentInstalledObservation wrong_current_architecture =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust",
                InstalledPackageBaseIdentity::known("rust"),
                InstalledPackageArchitectureIdentity::known("aarch64")));
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, wrong_current_architecture, execution)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "wrong current architecture correlated");

    CleanupInvocationSession other_session = CleanupInvocationSession::begin(
        prepared_remote_aur_build(plan));
    const CleanupBaselineSnapshotObservation other_baseline =
        make_cleanup_baseline_observation_for_test(
            other_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            other_session,
            InvocationDependencyTransactionOwner::
                SelectedRepositoryProvider,
            transaction_token('e'), {0}),
        "other-session token registration failed");
    const CleanupCurrentInstalledObservation other_current =
        make_cleanup_current_observation_for_test(
            other_session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust"));
    expect(
        other_baseline.authority() == other_session.authority() &&
            correlate_selected_repository_provider_to_build_plan(
                session, lifecycle, other_current, execution)
                    .completeness() !=
                CleanupEvidenceCompleteness::Complete,
        "cross-invocation current observation correlated");

    const CleanupCurrentInstalledObservation pre_success_current =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust"),
            CleanupObservationPhase::BeforeFirstDependencyMutation);
    expect(
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, pre_success_current, execution)
                .completeness() != CleanupEvidenceCompleteness::Complete,
        "pre-success current snapshot correlated as post-success");

    const SelectedRepositoryProviderTrustedExecutionEvidence
        solver_introduced = selected_provider_execution(
            session, plan, invocation,
            {"solver-introduced-package"});
    CleanupSelectedProviderCorrelationEvidence solver_correlation =
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, solver_introduced);
    expect(
        solver_correlation.completeness() !=
                CleanupEvidenceCompleteness::Complete &&
            has_provider_correlation_issue(
                solver_correlation,
                CleanupSelectedProviderCorrelationIssueKind::
                    UncorrelatedActualInstall),
        "solver-introduced Install became a selected-provider edge");
}

void test_remote_aur_invocation_route_and_evidence_completeness() {
    BuildPlan plan = basic_plan();
    PreparedRemoteSourceBuild prepared = prepared_remote_aur_build(plan);
    const PreparedProductionSourceBuildInvocation invocation =
        prepared.invocation;
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        std::move(prepared));
    const CleanupBaselineSnapshotObservation baseline =
        make_cleanup_baseline_observation_for_test(
            session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "remote route source token registration failed");
    const CleanupCurrentInstalledObservation current =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation policy =
        make_cleanup_policy_observation_for_test(
            session, base_policy_evidence());
    CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    SourceArtifactInstallCausalEvidence causal =
        source_artifact_causal_evidence(
            session, 0, "build-tool", "build-tools",
            plan.root_targets, {PackageRole::BuildDependency}, {0});
    CleanupSourceArtifactCorrelationEvidence source_correlation =
        correlate_source_artifact_install_to_build_plan(
            session, lifecycle, causal);
    const SourceAwarePackageIdentity candidate =
        source_correlation.selected_artifacts().front().artifact.expected_identity;
    CleanupInvocationEvidence complete =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session, lifecycle, baseline, current, policy,
            {source_correlation}, {});
    std::string complete_issue_codes;
    for(const CleanupInvocationEvidenceIssueKind issue : complete.issues()) {
        if(!complete_issue_codes.empty()) complete_issue_codes += ',';
        complete_issue_codes +=
            std::to_string(static_cast<int>(issue));
    }
    expect(
        complete.route_kind() ==
                CleanupRouteKind::RemoteAurSourceBuild &&
            complete.route_authority() ==
                CleanupRouteAuthority::Complete &&
            complete.completeness() ==
                CleanupEvidenceCompleteness::Complete &&
            complete.build_plan() != nullptr &&
            complete.roots() == plan.root_targets &&
            complete.work_items().size() == plan.order.size() &&
            complete.work_items().front().outcome.production_outcome.has_value() &&
            complete.package_bases().size() == plan.order.size() &&
            complete.source_artifact_evidence().size() == 1 &&
            project_shared_requirement(complete, candidate) ==
                CleanupSharedRequirementState::NoLongerRequired,
        "complete remote AUR invocation did not retain closed route evidence; issues=" +
            complete_issue_codes);

    CleanupInvocationEvidence incomplete =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session, lifecycle, baseline, current, policy, {}, {});
    expect(
        incomplete.route_authority() ==
                CleanupRouteAuthority::Unknown &&
            incomplete.completeness() !=
                CleanupEvidenceCompleteness::Complete &&
            has_invocation_issue(
                incomplete,
                CleanupInvocationEvidenceIssueKind::
                    SourceArtifactCorrelationMissing),
        "incomplete remote evidence became Complete");
}

void test_repository_provider_unknown_architecture_fails_closed() {
    BuildPlan plan = cargo_provider_plan();
    plan.dependency_edges.front()
        .resolved_provider->package_architecture.reset();
    std::get<ProviderResolvedDependencyCandidate>(
        plan.dependency_edges.front().resolved_candidate.value())
        .provider.package_architecture.reset();
    plan.provided.front().provider.package_architecture.reset();
    PreparedRemoteSourceBuild prepared = prepared_remote_aur_build(plan);
    const PreparedProductionSourceBuildInvocation invocation =
        prepared.invocation;
    ProductionSourceBuildInvocationResult result =
        successful_result(invocation);
    CleanupInvocationSession session = CleanupInvocationSession::begin(
        std::move(prepared));
    const CleanupBaselineSnapshotObservation baseline =
        make_cleanup_baseline_observation_for_test(
            session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session,
            InvocationDependencyTransactionOwner::
                SelectedRepositoryProvider,
            transaction_token('e'), {0}),
        "unknown-architecture provider token registration failed");
    const CleanupCurrentInstalledObservation current =
        make_cleanup_current_observation_for_test(
            session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust"));
    const CleanupPolicyObservation policy =
        make_cleanup_policy_observation_for_test(
            session, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session, result);
    const SelectedRepositoryProviderTrustedExecutionEvidence execution =
        selected_provider_execution(session, plan, invocation);
    const CleanupSelectedProviderCorrelationEvidence correlation =
        correlate_selected_repository_provider_to_build_plan(
            session, lifecycle, current, execution);
    const CleanupInvocationEvidence evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session, lifecycle, baseline, current, policy, {},
            {correlation});
    expect(
        correlation.completeness() !=
                CleanupEvidenceCompleteness::Complete &&
            evidence.route_authority() == CleanupRouteAuthority::Unknown,
        "repository provider with Unknown architecture became positive authority");
}

void test_invocation_wide_shared_lifetime_matrix() {
    BuildPlan multi_root = multiple_root_plan();
    PreparedRemoteSourceBuild multi_prepared =
        prepared_remote_aur_build(multi_root);
    const PreparedProductionSourceBuildInvocation multi_invocation =
        multi_prepared.invocation;
    ProductionSourceBuildInvocationResult after_first_root =
        result_after_work_item(multi_invocation, 1);
    CleanupInvocationSession multi_session =
        CleanupInvocationSession::begin(std::move(multi_prepared));
    const CleanupBaselineSnapshotObservation multi_baseline =
        make_cleanup_baseline_observation_for_test(
            multi_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            multi_session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "multi-root source token registration failed");
    const CleanupCurrentInstalledObservation multi_current =
        make_cleanup_current_observation_for_test(
            multi_session,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation multi_policy =
        make_cleanup_policy_observation_for_test(
            multi_session, base_policy_evidence());
    CleanupInvocationLifecycleEvidence after_first_lifecycle =
        CleanupInvocationLifecycleEvidence::after_work_item(
            multi_session, after_first_root, 1);
    SourceArtifactInstallCausalEvidence multi_causal =
        source_artifact_causal_evidence(
            multi_session, 0, "build-tool", "build-tools",
            multi_root.root_targets,
            {PackageRole::BuildDependency,
             PackageRole::CheckDependency},
            {0, 1});
    CleanupSourceArtifactCorrelationEvidence multi_source =
        correlate_source_artifact_install_to_build_plan(
            multi_session, after_first_lifecycle, multi_causal);
    SourceAwarePackageIdentity multi_candidate =
        multi_source.selected_artifacts().front().artifact.expected_identity;
    CleanupInvocationEvidence multi_evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            multi_session, after_first_lifecycle, multi_baseline,
            multi_current, multi_policy, {multi_source}, {});
    expect(
        project_shared_requirement(multi_evidence, multi_candidate) ==
            CleanupSharedRequirementState::StillRequired,
        "root A completion released a dependency still needed by root B");

    BuildPlan provider_plan = cargo_provider_plan(2);
    PreparedRemoteSourceBuild provider_prepared =
        prepared_remote_aur_build(provider_plan);
    const PreparedProductionSourceBuildInvocation provider_invocation =
        provider_prepared.invocation;
    ProductionSourceBuildInvocationResult provider_partial =
        result_after_work_item(provider_invocation, 0);
    CleanupInvocationSession provider_session =
        CleanupInvocationSession::begin(std::move(provider_prepared));
    const CleanupBaselineSnapshotObservation provider_baseline =
        make_cleanup_baseline_observation_for_test(
            provider_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            provider_session,
            InvocationDependencyTransactionOwner::
                SelectedRepositoryProvider,
            transaction_token('e'), {0, 1}),
        "shared provider token registration failed");
    const CleanupCurrentInstalledObservation provider_current =
        make_cleanup_current_observation_for_test(
            provider_session,
            present_snapshot(
                "rust", "1.90.0-1",
                InstalledPackageReason::Dependency, "rust"));
    const CleanupPolicyObservation provider_policy =
        make_cleanup_policy_observation_for_test(
            provider_session, base_policy_evidence());
    CleanupInvocationLifecycleEvidence provider_lifecycle =
        CleanupInvocationLifecycleEvidence::after_work_item(
            provider_session, provider_partial, 0);
    SelectedRepositoryProviderTrustedExecutionEvidence execution =
        selected_provider_execution(
            provider_session, provider_plan, provider_invocation);
    CleanupSelectedProviderCorrelationEvidence provider_correlation =
        correlate_selected_repository_provider_to_build_plan(
            provider_session, provider_lifecycle, provider_current,
            execution);
    SourceAwarePackageIdentity provider_identity =
        provider_correlation.edge_correlations().front().package_identity;
    CleanupInvocationEvidence provider_evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            provider_session, provider_lifecycle, provider_baseline,
            provider_current, provider_policy, {}, {provider_correlation});
    expect(
        project_shared_requirement(
            provider_evidence, provider_identity) ==
            CleanupSharedRequirementState::StillRequired,
        "PackageBase A completion released provider rust before PackageBase B");

    BuildPlan runtime_plan = mixed_runtime_plan();
    PreparedRemoteSourceBuild runtime_prepared =
        prepared_remote_aur_build(runtime_plan);
    const PreparedProductionSourceBuildInvocation runtime_invocation =
        runtime_prepared.invocation;
    ProductionSourceBuildInvocationResult runtime_result =
        successful_result(runtime_invocation);
    CleanupInvocationSession runtime_session =
        CleanupInvocationSession::begin(std::move(runtime_prepared));
    const CleanupBaselineSnapshotObservation runtime_baseline =
        make_cleanup_baseline_observation_for_test(
            runtime_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            runtime_session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "runtime fixture token registration failed");
    const CleanupCurrentInstalledObservation runtime_current =
        make_cleanup_current_observation_for_test(
            runtime_session,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation runtime_policy =
        make_cleanup_policy_observation_for_test(
            runtime_session, base_policy_evidence());
    CleanupInvocationLifecycleEvidence runtime_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            runtime_session, runtime_result);
    CleanupInvocationEvidence runtime_evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            runtime_session, runtime_lifecycle, runtime_baseline,
            runtime_current, runtime_policy, {}, {});
    expect(
        project_shared_requirement(
            runtime_evidence,
            source_artifact_identity("build-tool", "build-tools")) ==
            CleanupSharedRequirementState::StillRequired,
        "later Runtime consumer was not StillRequired");

    BuildPlan root_plan = basic_plan();
    const RootTargetIdentity build_tool_root = root(1, "build-tool");
    root_plan.root_targets.push_back(build_tool_root);
    root_plan.package_targets.front().roles.push_back(PackageRole::Root);
    root_plan.package_targets.front().roots.push_back(build_tool_root);
    PreparedRemoteSourceBuild root_prepared =
        prepared_remote_aur_build(root_plan);
    const PreparedProductionSourceBuildInvocation root_invocation =
        root_prepared.invocation;
    ProductionSourceBuildInvocationResult root_result =
        successful_result(root_invocation);
    CleanupInvocationSession root_session =
        CleanupInvocationSession::begin(std::move(root_prepared));
    const CleanupBaselineSnapshotObservation root_baseline =
        make_cleanup_baseline_observation_for_test(
            root_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            root_session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "root fixture token registration failed");
    const CleanupCurrentInstalledObservation root_current =
        make_cleanup_current_observation_for_test(
            root_session,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation root_policy =
        make_cleanup_policy_observation_for_test(
            root_session, base_policy_evidence());
    CleanupInvocationLifecycleEvidence root_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            root_session, root_result);
    CleanupInvocationEvidence root_evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            root_session, root_lifecycle, root_baseline, root_current,
            root_policy, {}, {});
    expect(
        project_shared_requirement(
            root_evidence,
            source_artifact_identity("build-tool", "build-tools")) ==
            CleanupSharedRequirementState::StillRequired,
        "later Root consumer was not StillRequired");

    BuildPlan failed_plan = basic_plan();
    PreparedRemoteSourceBuild failed_prepared =
        prepared_remote_aur_build(failed_plan);
    const PreparedProductionSourceBuildInvocation failed_invocation =
        failed_prepared.invocation;
    ProductionSourceBuildInvocationResult failed_result =
        result_after_work_item(failed_invocation, 0);
    failed_result.work_items[1].status =
        ProductionSourceBuildWorkItemStatus::Failed;
    failed_result.work_items[1].failure_stage =
        ProductionSourceBuildFailureStage::Build;
    CleanupInvocationSession failed_session =
        CleanupInvocationSession::begin(std::move(failed_prepared));
    const CleanupBaselineSnapshotObservation failed_baseline =
        make_cleanup_baseline_observation_for_test(
            failed_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            failed_session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "failed route source token registration failed");
    const CleanupCurrentInstalledObservation failed_current =
        make_cleanup_current_observation_for_test(
            failed_session,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation failed_policy =
        make_cleanup_policy_observation_for_test(
            failed_session, base_policy_evidence());
    CleanupInvocationLifecycleEvidence failed_lifecycle =
        CleanupInvocationLifecycleEvidence::after_invocation_completion(
            failed_session, failed_result);
    SourceArtifactInstallCausalEvidence failed_causal =
        source_artifact_causal_evidence(
            failed_session, 0, "build-tool", "build-tools",
            failed_plan.root_targets, {PackageRole::BuildDependency}, {0});
    CleanupSourceArtifactCorrelationEvidence failed_source =
        correlate_source_artifact_install_to_build_plan(
            failed_session, failed_lifecycle, failed_causal);
    SourceAwarePackageIdentity failed_candidate =
        failed_source.selected_artifacts().front().artifact.expected_identity;
    CleanupInvocationEvidence failed_evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            failed_session, failed_lifecycle, failed_baseline,
            failed_current, failed_policy, {failed_source}, {});
    expect(
        project_shared_requirement(
            failed_evidence, failed_candidate) ==
            CleanupSharedRequirementState::Unknown,
        "failed later work item made dependency NoLongerRequired");

    ProductionSourceBuildInvocationResult unattempted_result =
        result_after_work_item(failed_invocation, 0);
    CleanupInvocationLifecycleEvidence unattempted_lifecycle =
        CleanupInvocationLifecycleEvidence::after_invocation_completion(
            failed_session, unattempted_result);
    CleanupSourceArtifactCorrelationEvidence unattempted_source =
        correlate_source_artifact_install_to_build_plan(
            failed_session, unattempted_lifecycle, failed_causal);
    CleanupInvocationEvidence unattempted_evidence =
        aggregate_remote_aur_cleanup_invocation_evidence(
            failed_session, unattempted_lifecycle, failed_baseline,
            failed_current, failed_policy, {unattempted_source}, {});
    expect(
        project_shared_requirement(
            unattempted_evidence, failed_candidate) ==
            CleanupSharedRequirementState::Unknown,
        "unattempted later work item made dependency NoLongerRequired");
}

void test_session_replay_token_and_phase_firewalls() {
    BuildPlan plan = basic_plan();
    PreparedRemoteSourceBuild prepared_a = prepared_remote_aur_build(plan);
    const PreparedProductionSourceBuildInvocation invocation_a =
        prepared_a.invocation;
    ProductionSourceBuildInvocationResult result_a =
        successful_result(invocation_a);
    CleanupInvocationSession session_a = CleanupInvocationSession::begin(
        std::move(prepared_a));
    const CleanupBaselineSnapshotObservation baseline_a =
        make_cleanup_baseline_observation_for_test(
            session_a, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session_a,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}) &&
            !register_cleanup_invocation_transaction_token_for_test(
                session_a,
                InvocationDependencyTransactionOwner::
                    SourceArtifactInstall,
                transaction_token('d'), {0}),
        "session token inventory accepted duplicate registration");
    const CleanupCurrentInstalledObservation current_a =
        make_cleanup_current_observation_for_test(
            session_a,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation policy_a =
        make_cleanup_policy_observation_for_test(
            session_a, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence lifecycle_a =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session_a, result_a);
    const SourceArtifactInstallCausalEvidence causal_a =
        source_artifact_causal_evidence(
            session_a, 0, "build-tool", "build-tools",
            plan.root_targets, {PackageRole::BuildDependency}, {0});
    const CleanupSourceArtifactCorrelationEvidence correlation_a =
        correlate_source_artifact_install_to_build_plan(
            session_a, lifecycle_a, causal_a);
    expect(
        correlation_a.completeness() ==
            CleanupEvidenceCompleteness::Complete,
        "session A source correlation fixture was incomplete");

    PreparedRemoteSourceBuild prepared_b = prepared_remote_aur_build(plan);
    const PreparedProductionSourceBuildInvocation invocation_b =
        prepared_b.invocation;
    ProductionSourceBuildInvocationResult result_b =
        successful_result(invocation_b);
    CleanupInvocationSession session_b = CleanupInvocationSession::begin(
        std::move(prepared_b));
    const CleanupBaselineSnapshotObservation baseline_b =
        make_cleanup_baseline_observation_for_test(
            session_b, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session_b,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {0}),
        "session B same-value token registration failed");
    const CleanupCurrentInstalledObservation current_b =
        make_cleanup_current_observation_for_test(
            session_b,
            present_snapshot(
                "build-tool", "1.0-1",
                InstalledPackageReason::Dependency, "build-tools"));
    const CleanupPolicyObservation policy_b =
        make_cleanup_policy_observation_for_test(
            session_b, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence lifecycle_b =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            session_b, result_b);
    const SourceArtifactInstallCausalEvidence causal_b =
        source_artifact_causal_evidence(
            session_b, 0, "build-tool", "build-tools",
            plan.root_targets, {PackageRole::BuildDependency}, {0});
    const CleanupSourceArtifactCorrelationEvidence correlation_b =
        correlate_source_artifact_install_to_build_plan(
            session_b, lifecycle_b, causal_b);

    const CleanupSourceArtifactCorrelationEvidence replayed_causal =
        correlate_source_artifact_install_to_build_plan(
            session_b, lifecycle_b, causal_a);
    expect(
        replayed_causal.completeness() !=
                CleanupEvidenceCompleteness::Complete &&
            has_source_correlation_issue(
                replayed_causal,
                CleanupSourceArtifactCorrelationIssueKind::
                    InvocationMismatch),
        "same-value fresh session accepted copied causal evidence");

    const CleanupInvocationEvidence replayed_correlation =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session_b, lifecycle_b, baseline_b, current_b, policy_b,
            {correlation_a}, {});
    expect(
        replayed_correlation.route_authority() ==
                CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                replayed_correlation,
                CleanupInvocationEvidenceIssueKind::
                    CorrelationInvocationMismatch),
        "same-value fresh session accepted copied correlation evidence");

    const CleanupInvocationEvidence phase_mixed =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session_b, lifecycle_b, baseline_a, current_b, policy_a,
            {correlation_b}, {});
    expect(
        phase_mixed.route_authority() == CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                phase_mixed,
                CleanupInvocationEvidenceIssueKind::
                    PhaseObservationMismatch),
        "baseline/policy from invocation A mixed with invocation B receipt");

    const CleanupInvocationEvidence duplicated =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session_a, lifecycle_a, baseline_a, current_a, policy_a,
            {correlation_a, correlation_a}, {});
    expect(
        duplicated.route_authority() == CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                duplicated,
                CleanupInvocationEvidenceIssueKind::
                    TransactionTokenDuplicate),
        "duplicate correlation/token became Complete");

    const CleanupInvocationEvidence same_value_fresh_positive =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session_b, lifecycle_b, baseline_b, current_b, policy_b,
            {correlation_b}, {});
    expect(
        same_value_fresh_positive.route_authority() ==
            CleanupRouteAuthority::Complete,
        "fresh session could not use its own same-value evidence");

    expect(
        register_cleanup_invocation_transaction_token_for_test(
            session_a,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('e'), {0}),
        "later-transaction phase fixture registration failed");
    const CleanupInvocationEvidence stale_post_success_phase =
        aggregate_remote_aur_cleanup_invocation_evidence(
            session_a, lifecycle_a, baseline_a, current_a, policy_a,
            {correlation_a}, {});
    expect(
        stale_post_success_phase.route_authority() ==
                CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                stale_post_success_phase,
                CleanupInvocationEvidenceIssueKind::
                    PhaseObservationMismatch),
        "current/policy captured before a later trusted transaction remained post-success authority");

    PreparedRemoteSourceBuild reattributed_prepared =
        prepared_remote_aur_build(plan);
    reattributed_prepared.invocation.work_items.front()
        .build_plan_dependency_edge_indices.clear();
    reattributed_prepared.invocation.work_items.back()
        .build_plan_dependency_edge_indices.push_back(0);
    ProductionSourceBuildInvocationResult reattributed_result =
        successful_result(reattributed_prepared.invocation);
    CleanupInvocationSession reattributed_session =
        CleanupInvocationSession::begin(std::move(reattributed_prepared));
    const CleanupBaselineSnapshotObservation reattributed_baseline =
        make_cleanup_baseline_observation_for_test(
            reattributed_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            reattributed_session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('d'), {1}),
        "reattributed session token registration failed");
    const CleanupCurrentInstalledObservation reattributed_current =
        make_cleanup_current_observation_for_test(
            reattributed_session, absent_snapshot());
    const CleanupPolicyObservation reattributed_policy =
        make_cleanup_policy_observation_for_test(
            reattributed_session, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence reattributed_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            reattributed_session, reattributed_result);
    const CleanupInvocationEvidence reattributed =
        aggregate_remote_aur_cleanup_invocation_evidence(
            reattributed_session, reattributed_lifecycle,
            reattributed_baseline, reattributed_current,
            reattributed_policy, {correlation_a}, {});
    expect(
        reattributed.route_authority() == CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                reattributed,
                CleanupInvocationEvidenceIssueKind::
                    DependencyEdgeAttributionMismatch),
        "copied correlation survived current work-item reattribution");

    PreparedRemoteSourceBuild foreign_prepared =
        prepared_remote_aur_build(plan);
    ProductionSourceBuildInvocationResult foreign_result =
        successful_result(foreign_prepared.invocation);
    CleanupInvocationSession foreign_session =
        CleanupInvocationSession::begin(std::move(foreign_prepared));
    const CleanupBaselineSnapshotObservation foreign_baseline =
        make_cleanup_baseline_observation_for_test(
            foreign_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            foreign_session,
            InvocationDependencyTransactionOwner::SourceArtifactInstall,
            transaction_token('f'), {0}),
        "foreign-token fixture registration failed");
    const CleanupCurrentInstalledObservation foreign_current =
        make_cleanup_current_observation_for_test(
            foreign_session, absent_snapshot());
    const CleanupPolicyObservation foreign_policy =
        make_cleanup_policy_observation_for_test(
            foreign_session, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence foreign_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            foreign_session, foreign_result);
    const SourceArtifactInstallCausalEvidence foreign_causal =
        source_artifact_causal_evidence(
            foreign_session, 0, "build-tool", "build-tools",
            plan.root_targets, {PackageRole::BuildDependency}, {0});
    const CleanupSourceArtifactCorrelationEvidence foreign_correlation =
        correlate_source_artifact_install_to_build_plan(
            foreign_session, foreign_lifecycle, foreign_causal);
    const CleanupInvocationEvidence foreign_token =
        aggregate_remote_aur_cleanup_invocation_evidence(
            foreign_session, foreign_lifecycle, foreign_baseline,
            foreign_current, foreign_policy, {foreign_correlation}, {});
    expect(
        foreign_token.route_authority() == CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                foreign_token,
                CleanupInvocationEvidenceIssueKind::
                    TransactionTokenInventoryMissing),
        "foreign/missing inventory token became Complete");

    PreparedRemoteSourceBuild wrong_owner_prepared =
        prepared_remote_aur_build(plan);
    ProductionSourceBuildInvocationResult wrong_owner_result =
        successful_result(wrong_owner_prepared.invocation);
    CleanupInvocationSession wrong_owner_session =
        CleanupInvocationSession::begin(std::move(wrong_owner_prepared));
    const CleanupBaselineSnapshotObservation wrong_owner_baseline =
        make_cleanup_baseline_observation_for_test(
            wrong_owner_session, absent_snapshot());
    expect(
        register_cleanup_invocation_transaction_token_for_test(
            wrong_owner_session,
            InvocationDependencyTransactionOwner::
                SelectedRepositoryProvider,
            transaction_token('d'), {0}),
        "wrong-owner fixture token registration failed");
    const CleanupCurrentInstalledObservation wrong_owner_current =
        make_cleanup_current_observation_for_test(
            wrong_owner_session, absent_snapshot());
    const CleanupPolicyObservation wrong_owner_policy =
        make_cleanup_policy_observation_for_test(
            wrong_owner_session, base_policy_evidence());
    const CleanupInvocationLifecycleEvidence wrong_owner_lifecycle =
        CleanupInvocationLifecycleEvidence::after_successful_invocation(
            wrong_owner_session, wrong_owner_result);
    const SourceArtifactInstallCausalEvidence wrong_owner_causal =
        source_artifact_causal_evidence(
            wrong_owner_session, 0, "build-tool", "build-tools",
            plan.root_targets, {PackageRole::BuildDependency}, {0});
    const CleanupSourceArtifactCorrelationEvidence wrong_owner_correlation =
        correlate_source_artifact_install_to_build_plan(
            wrong_owner_session, wrong_owner_lifecycle,
            wrong_owner_causal);
    const CleanupInvocationEvidence wrong_owner =
        aggregate_remote_aur_cleanup_invocation_evidence(
            wrong_owner_session, wrong_owner_lifecycle,
            wrong_owner_baseline, wrong_owner_current, wrong_owner_policy,
            {wrong_owner_correlation}, {});
    expect(
        wrong_owner.route_authority() == CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                wrong_owner,
                CleanupInvocationEvidenceIssueKind::
                    TransactionTokenOwnerMismatch),
        "wrong-owner inventory token became Complete");
}

void test_exhaustive_edge_and_vacuous_completeness_matrix() {
    BuildPlan root_only = basic_plan();
    root_only.dependency_edges.clear();
    root_only.package_targets.erase(root_only.package_targets.begin());
    root_only.order.erase(root_only.order.begin());
    CleanupInvocationEvidence root_only_evidence =
        aggregate_plan_without_correlations(root_only);
    expect(
        root_only_evidence.route_authority() ==
                CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                root_only_evidence,
                CleanupInvocationEvidenceIssueKind::
                    DependencyEdgeInventoryEmpty) &&
            has_invocation_issue(
                root_only_evidence,
                CleanupInvocationEvidenceIssueKind::
                    CleanupRelevantEdgeInventoryEmpty),
        "root-only empty dependency set became Complete");

    BuildPlan build_role_without_edge = basic_plan();
    build_role_without_edge.dependency_edges.clear();
    CleanupInvocationEvidence build_role_without_edge_evidence =
        aggregate_plan_without_correlations(build_role_without_edge);
    expect(
        build_role_without_edge_evidence.route_authority() ==
                CleanupRouteAuthority::Unknown &&
            project_shared_requirement(
                build_role_without_edge_evidence,
                source_artifact_identity(
                    "build-tool", "build-tools")) ==
                CleanupSharedRequirementState::Unknown,
        "BuildDependency target without a relevant edge became NoLongerRequired");

    const auto expect_invalid_edge = [](
                                         BuildPlan plan,
                                         const std::string& context) {
        CleanupInvocationEvidence evidence =
            aggregate_plan_without_correlations(std::move(plan));
        expect(
            evidence.route_authority() == CleanupRouteAuthority::Unknown &&
                evidence.completeness() !=
                    CleanupEvidenceCompleteness::Complete &&
                project_shared_requirement(
                    evidence,
                    source_artifact_identity(
                        "build-tool", "build-tools")) ==
                    CleanupSharedRequirementState::Unknown,
            context + " became Complete or NoLongerRequired");
    };

    BuildPlan local_hybrid = basic_plan();
    local_hybrid.dependency_edges.push_back(
        local_hybrid.dependency_edges.front());
    local_hybrid.dependency_edges.back().kind = DependencyKind::Local;
    expect_invalid_edge(std::move(local_hybrid), "AUR + Local edge");

    BuildPlan unknown_hybrid = basic_plan();
    unknown_hybrid.dependency_edges.push_back(
        unknown_hybrid.dependency_edges.front());
    unknown_hybrid.dependency_edges.back().kind = DependencyKind::Unknown;
    expect_invalid_edge(std::move(unknown_hybrid), "AUR + Unknown edge");

    BuildPlan ambiguous = basic_plan();
    ambiguous.dependency_edges.front().kind =
        DependencyKind::AmbiguousProvider;
    expect_invalid_edge(std::move(ambiguous), "AmbiguousProvider edge");

    BuildPlan untyped = basic_plan();
    untyped.dependency_edges.front().requirement.reset();
    expect_invalid_edge(std::move(untyped), "raw untyped edge");

    BuildPlan invalid_kind = basic_plan();
    invalid_kind.dependency_edges.front().kind =
        static_cast<DependencyKind>(999);
    expect_invalid_edge(std::move(invalid_kind), "invalid edge kind");

    BuildPlan invalid_role = basic_plan();
    invalid_role.dependency_edges.front().role =
        static_cast<PackageRole>(999);
    expect_invalid_edge(std::move(invalid_role), "invalid edge role");

    BuildPlan root_role = basic_plan();
    root_role.dependency_edges.front().role = PackageRole::Root;
    expect_invalid_edge(
        std::move(root_role), "Root role on dependency edge");

    BuildPlan invalid_resolution = cargo_provider_plan();
    invalid_resolution.dependency_edges.front().provider_resolution =
        static_cast<ProviderResolutionKind>(999);
    expect_invalid_edge(
        std::move(invalid_resolution), "invalid provider resolution");

    BuildPlan direct_user_selected = basic_plan();
    direct_user_selected.dependency_edges.front().provider_resolution =
        ProviderResolutionKind::UserSelected;
    expect_invalid_edge(
        std::move(direct_user_selected),
        "direct AUR edge with UserSelected resolution");

    BuildPlan malformed_installed = basic_plan();
    BuildPlanDependencyEdge& installed_edge =
        malformed_installed.dependency_edges.front();
    installed_edge.kind = DependencyKind::Installed;
    installed_edge.resolved_package_base.reset();
    installed_edge.resolved_candidate = InstalledExactPackage{
        "different-installed-package",
        ObservedVersion::available(
            ObservedVersionSource::InstalledExactPackage, "1.0-1")};
    expect_invalid_edge(
        std::move(malformed_installed),
        "malformed installed edge identity");
}

void test_production_work_item_outcome_shape_matrix() {
    ProductionSourceBuildWorkItemOutcome invalid_status;
    invalid_status.package_base = "build-tools";
    invalid_status.status =
        static_cast<ProductionSourceBuildWorkItemStatus>(999);
    expect(
        validate_production_source_build_work_item_outcome(invalid_status) ==
            CleanupWorkItemOutcomeShape::Invalid,
        "invalid work-item status enum was accepted");

    ProductionSourceBuildWorkItemOutcome succeeded_with_failure{
        "build-tools",
        ProductionSourceBuildWorkItemStatus::Succeeded,
        successful_staged_outcome(),
        ProductionSourceBuildFailureStage::Build,
        std::string("contradictory failure"),
        std::make_exception_ptr(
            std::runtime_error("contradictory failure"))};
    expect(
        validate_production_source_build_work_item_outcome(
            succeeded_with_failure) == CleanupWorkItemOutcomeShape::Invalid,
        "Succeeded plus failure markers was accepted");

    ProductionSourceBuildWorkItemOutcome invalid_staged{
        "build-tools", ProductionSourceBuildWorkItemStatus::Succeeded,
        successful_staged_outcome(), std::nullopt, std::nullopt, nullptr};
    invalid_staged.production_outcome->build_outcome =
        static_cast<ProductionSourceBuildCommandOutcome>(999);
    expect(
        validate_production_source_build_work_item_outcome(invalid_staged) ==
            CleanupWorkItemOutcomeShape::Invalid,
        "invalid staged outcome was accepted");

    ProductionSourceBuildWorkItemOutcome unknown_review_revision{
        "build-tools", ProductionSourceBuildWorkItemStatus::Succeeded,
        successful_staged_outcome(), std::nullopt, std::nullopt, nullptr};
    ProductionSourceBuildProvenance& unknown_revision_provenance =
        unknown_review_revision.production_outcome->source_provenance;
    unknown_revision_provenance.review_status =
        ProductionSourceReviewStatus::Reviewed;
    unknown_revision_provenance.reviewed_upstream_base_revision =
        SourceRevisionIdentity::unknown();
    unknown_revision_provenance.publication_status =
        ReviewedSourcePublicationStatus::Published;
    unknown_revision_provenance.reviewed_outcome =
        ProductionReviewedSourceOutcome::InitialFullReview;
    unknown_revision_provenance.reviewed_state_generation = 1;
    expect(
        validate_production_source_build_work_item_outcome(
            unknown_review_revision) ==
            CleanupWorkItemOutcomeShape::Invalid,
        "Reviewed staged outcome with Unknown revision was accepted");

    ProductionSourceBuildWorkItemOutcome zero_generation =
        unknown_review_revision;
    zero_generation.production_outcome->source_provenance
        .reviewed_upstream_base_revision =
        SourceRevisionIdentity::git_commit(std::string(40, 'a'));
    zero_generation.production_outcome->source_provenance
        .reviewed_state_generation = 0;
    expect(
        validate_production_source_build_work_item_outcome(zero_generation) ==
            CleanupWorkItemOutcomeShape::Invalid,
        "Reviewed staged outcome with generation zero was accepted");

    ProductionSourceBuildWorkItemOutcome contradictory_publication =
        zero_generation;
    contradictory_publication.production_outcome->source_provenance
        .reviewed_state_generation = 1;
    contradictory_publication.production_outcome->source_provenance
        .reviewed_outcome = ProductionReviewedSourceOutcome::AlreadyReviewed;
    contradictory_publication.production_outcome->source_provenance
        .publication_status = ReviewedSourcePublicationStatus::Published;
    expect(
        validate_production_source_build_work_item_outcome(
            contradictory_publication) ==
            CleanupWorkItemOutcomeShape::Invalid,
        "AlreadyReviewed staged outcome with Published state was accepted");

    ProductionSourceBuildWorkItemOutcome failed_without_shape;
    failed_without_shape.package_base = "build-tools";
    failed_without_shape.status =
        ProductionSourceBuildWorkItemStatus::Failed;
    expect(
        validate_production_source_build_work_item_outcome(
            failed_without_shape) == CleanupWorkItemOutcomeShape::Invalid,
        "Failed without required failure shape was accepted");

    ProductionSourceBuildWorkItemOutcome not_attempted_with_success;
    not_attempted_with_success.package_base = "build-tools";
    not_attempted_with_success.production_outcome =
        successful_staged_outcome();
    expect(
        validate_production_source_build_work_item_outcome(
            not_attempted_with_success) ==
            CleanupWorkItemOutcomeShape::Invalid,
        "NotAttempted with success state was accepted");

    CleanupInvocationEvidence invalid_aggregate =
        aggregate_plan_without_correlations(
            basic_plan(), [](ProductionSourceBuildInvocationResult& result) {
                result.work_items.front().status =
                    static_cast<ProductionSourceBuildWorkItemStatus>(999);
            });
    expect(
        invalid_aggregate.route_authority() ==
                CleanupRouteAuthority::Unknown &&
            has_invocation_issue(
                invalid_aggregate,
                CleanupInvocationEvidenceIssueKind::
                    WorkItemOutcomeInvalid) &&
            project_shared_requirement(
                invalid_aggregate,
                source_artifact_identity(
                    "build-tool", "build-tools")) ==
                CleanupSharedRequirementState::Unknown,
        "invalid outcome aggregate became positive route/shared authority");
}

void test_cleanup_route_matrix_is_explicit_and_fail_closed() {
    expect(
        project_cleanup_route_evidence(
            CleanupRouteKind::LocalSourceBuild)
                .route_authority() ==
            CleanupRouteAuthority::Unsupported,
        "local cleanup route was not Unsupported");
    expect(
        project_cleanup_route_evidence(CleanupRouteKind::Upgrade)
                .route_authority() ==
            CleanupRouteAuthority::Unsupported,
        "upgrade cleanup route was not Unsupported");
    expect(
        project_cleanup_route_evidence(CleanupRouteKind::UpgradeAur)
                .route_authority() ==
            CleanupRouteAuthority::Unsupported,
        "upgrade-aur cleanup route was not Unsupported");
    expect(
        project_cleanup_route_evidence(CleanupRouteKind::UpgradeAll)
                .route_authority() ==
            CleanupRouteAuthority::Unsupported,
        "upgrade-all cleanup route was not Unsupported");
    expect(
        project_cleanup_route_evidence(
            CleanupRouteKind::MakepkgSyncDependencies)
                .route_authority() == CleanupRouteAuthority::Unknown,
        "makepkg syncdeps cleanup route was promoted");
    expect(
        project_cleanup_route_evidence(
            CleanupRouteKind::StandaloneRepositorySourceBuild)
                .route_authority() == CleanupRouteAuthority::Unknown,
        "standalone repository source route was promoted");
    expect(
        project_cleanup_route_authority(
            CleanupRouteKind::RemoteAurSourceBuild,
            CleanupEvidenceCompleteness::Incomplete) ==
            CleanupRouteAuthority::Unknown,
        "incomplete remote AUR evidence became Complete");
    expect(
        project_cleanup_route_authority(
            CleanupRouteKind::LocalSourceBuild,
            CleanupEvidenceCompleteness::Complete) ==
            CleanupRouteAuthority::Unsupported,
        "Unsupported route became a Complete empty set");
}

void test_cleanup_policy_reducer_authority_priority() {
    CleanupPolicyProtectionEvidence installed = base_policy_evidence();
    installed.installed_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::InstalledBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::Protected);
    installed.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotProtected);
    expect(
        project_cleanup_policy_protection(installed) ==
            CleanupPolicyProtection::Protected,
        "installed exact base-devel meta authority was not primary");

    installed.installed_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::InstalledBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotProtected);
    installed.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::Protected);
    installed.base_devel_group = group_policy_authority(
        CleanupPolicyCandidateEvaluation::Protected);
    expect(
        project_cleanup_policy_protection(installed) ==
            CleanupPolicyProtection::NotProtected,
        "lower-priority sync/group evidence overrode installed meta authority");

    CleanupPolicyProtectionEvidence sync = base_policy_evidence();
    sync.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::Protected);
    expect(
        project_cleanup_policy_protection(sync) ==
            CleanupPolicyProtection::Protected,
        "configured sync exact base-devel meta authority did not protect");

    sync.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotProtected);
    sync.base_devel_group = group_policy_authority(
        CleanupPolicyCandidateEvaluation::Protected);
    expect(
        project_cleanup_policy_protection(sync) ==
            CleanupPolicyProtection::NotProtected,
        "compatibility group overrode present sync meta authority");
}

void test_cleanup_policy_reducer_group_fallback() {
    CleanupPolicyProtectionEvidence protected_group =
        base_policy_evidence();
    protected_group.candidate->groups = {"other", "base-devel"};
    protected_group.base_devel_group = group_policy_authority(
        CleanupPolicyCandidateEvaluation::Protected);
    expect(
        project_cleanup_policy_protection(protected_group) ==
            CleanupPolicyProtection::Protected,
        "exact complete base-devel group fallback did not protect");

    CleanupPolicyProtectionEvidence not_protected_group =
        base_policy_evidence();
    not_protected_group.base_devel_group = group_policy_authority(
        CleanupPolicyCandidateEvaluation::NotProtected);
    expect(
        project_cleanup_policy_protection(not_protected_group) ==
            CleanupPolicyProtection::NotProtected,
        "complete exact group non-membership did not produce NotProtected");

    CleanupPolicyProtectionEvidence missing_group = base_policy_evidence();
    expect(
        project_cleanup_policy_protection(missing_group) ==
            CleanupPolicyProtection::Unknown,
        "missing exact compatibility group became NotProtected");
}

void test_cleanup_policy_reducer_failure_matrix() {
    CleanupPolicyProtectionEvidence local_failure = base_policy_evidence();
    local_failure.local_database_completeness =
        CleanupPolicyMetadataCompleteness::Failed;
    expect(
        project_cleanup_policy_protection(local_failure) ==
            CleanupPolicyProtection::Unknown,
        "local DB failure became a policy decision");

    CleanupPolicyProtectionEvidence candidate_incomplete =
        base_policy_evidence();
    candidate_incomplete.candidate_metadata_completeness =
        CleanupPolicyMetadataCompleteness::Incomplete;
    candidate_incomplete.candidate.reset();
    expect(
        project_cleanup_policy_protection(candidate_incomplete) ==
            CleanupPolicyProtection::Unknown,
        "incomplete candidate metadata became NotProtected");

    CleanupPolicyProtectionEvidence sync_unavailable =
        base_policy_evidence();
    sync_unavailable.configured_sync_base_devel =
        unobserved_policy_authority(
            CleanupPolicyAuthorityKind::
                ConfiguredSyncBaseDevelMetaPackage);
    sync_unavailable.configured_sync_base_devel.observation =
        CleanupPolicyAuthorityObservation::Unavailable;
    sync_unavailable.configured_sync_base_devel.inventory_completeness =
        CleanupPolicyMetadataCompleteness::Failed;
    sync_unavailable.configured_sync_base_devel.evaluation_completeness =
        CleanupPolicyMetadataCompleteness::Failed;
    expect(
        project_cleanup_policy_protection(sync_unavailable) ==
            CleanupPolicyProtection::Unknown,
        "required sync DB failure became NotProtected");

    CleanupPolicyProtectionEvidence partial_inventory =
        base_policy_evidence();
    partial_inventory.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotProtected,
        CleanupPolicyMetadataCompleteness::Incomplete);
    expect(
        project_cleanup_policy_protection(partial_inventory) ==
            CleanupPolicyProtection::Unknown,
        "partial protected inventory became NotProtected");

    CleanupPolicyProtectionEvidence evaluation_failure =
        base_policy_evidence();
    evaluation_failure.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotEvaluated,
        CleanupPolicyMetadataCompleteness::Complete,
        CleanupPolicyMetadataCompleteness::Failed);
    expect(
        project_cleanup_policy_protection(evaluation_failure) ==
            CleanupPolicyProtection::Unknown,
        "satisfier evaluation failure became NotProtected");

    CleanupPolicyProtectionEvidence contradictory = base_policy_evidence();
    contradictory.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::Protected);
    contradictory.consistency =
        CleanupPolicyEvidenceConsistency::Contradictory;
    expect(
        project_cleanup_policy_protection(contradictory) ==
            CleanupPolicyProtection::Unknown,
        "contradictory policy evidence became Protected");

    CleanupPolicyProtectionEvidence failed_negative = base_policy_evidence();
    failed_negative.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotProtected);
    failed_negative.failures.push_back(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed, "injected failure"});
    expect(
        project_cleanup_policy_protection(failed_negative) ==
            CleanupPolicyProtection::Unknown,
        "query failure plus negative evidence became NotProtected");

    CleanupPolicyProtectionEvidence invalid_typed_state =
        base_policy_evidence();
    invalid_typed_state.configured_sync_base_devel.observation =
        static_cast<CleanupPolicyAuthorityObservation>(999);
    expect(
        project_cleanup_policy_protection(invalid_typed_state) ==
            CleanupPolicyProtection::Unknown,
        "invalid policy evidence enum became a policy decision");

    CleanupPolicyProtectionEvidence malformed_authority =
        base_policy_evidence();
    malformed_authority.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::NotProtected);
    malformed_authority.configured_sync_base_devel
        .meta_packages.front()
        .dependencies.clear();
    expect(
        project_cleanup_policy_protection(malformed_authority) ==
            CleanupPolicyProtection::Unknown,
        "malformed complete meta evidence became NotProtected");
}

void test_cleanup_policy_reducer_complete_positive_survives_other_failure() {
    CleanupPolicyProtectionEvidence evidence = base_policy_evidence();
    evidence.configured_sync_base_devel = meta_policy_authority(
        CleanupPolicyAuthorityKind::ConfiguredSyncBaseDevelMetaPackage,
        CleanupPolicyCandidateEvaluation::Protected,
        CleanupPolicyMetadataCompleteness::Incomplete,
        CleanupPolicyMetadataCompleteness::Complete);
    evidence.failures.push_back(PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "separate configured source failed"});
    expect(
        project_cleanup_policy_protection(evidence) ==
            CleanupPolicyProtection::Protected,
        "complete positive protection was lost to a separate query failure");
}

} // namespace

void run_invocation_owned_cleanup_adapter_tests() {
    test_snapshot_observation_projection();
    test_current_lifecycle_never_proves_causal_ownership();
    test_authoritative_install_receipt_projects_only_causal_dimension();
    test_upgrade_and_external_install_race_do_not_project_ownership();
    test_failed_missing_and_mismatched_receipts_remain_unknown();
    test_multiple_transaction_attribution_is_not_flattened();
    test_receipt_does_not_bypass_protection_precedence();
    test_complete_plan_projects_complete_correlations_and_lifetime();
    test_incomplete_plan_states_never_project_complete_coverage();
    test_repository_provider_package_base_is_verified();
    test_mixed_roles_and_multiple_roots_are_preserved();
    test_later_work_item_and_unknown_lifecycle_fail_safe();
    test_local_remote_dependency_subset_is_not_complete_authority();
    test_metadata_and_source_failures_remain_typed_unknown_evidence();
    test_version_mismatch_and_unknown_policy_fail_closed();
    test_newly_observed_dependency_with_unknown_policy_is_not_eligible();
    test_source_artifact_exact_build_plan_correlation_matrix();
    test_selected_repository_provider_closed_correlation_matrix();
    test_remote_aur_invocation_route_and_evidence_completeness();
    test_repository_provider_unknown_architecture_fails_closed();
    test_invocation_wide_shared_lifetime_matrix();
    test_session_replay_token_and_phase_firewalls();
    test_exhaustive_edge_and_vacuous_completeness_matrix();
    test_production_work_item_outcome_shape_matrix();
    test_cleanup_route_matrix_is_explicit_and_fail_closed();
    test_cleanup_policy_reducer_authority_priority();
    test_cleanup_policy_reducer_group_fallback();
    test_cleanup_policy_reducer_failure_matrix();
    test_cleanup_policy_reducer_complete_positive_survives_other_failure();
}
