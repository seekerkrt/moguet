#include "aur_update_execution_preparation.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_query.hpp"
#include "commands_sync.hpp"
#include "local_dependency_plan_projection.hpp"
#include "package_relation_assessment_fixture.hpp"
#include "source_install.hpp"
#include "stubs/local-dependency-plan/query_stub.hpp"
#include "system_aur_update_operation.hpp"
#include "system_source_upgrade.hpp"
#include "terminal_safe_text_cases.hpp"
#include "unified_plan_projection.hpp"
#include "unified_plan_renderer.hpp"
#include "upgrade_all_operation.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

struct SystemAurUpdateUnifiedPlanProjectionTestAccess {
    static std::unique_ptr<UnifiedPlanProjection> make_child_projection(
        UnifiedPlanObservationInput input) {
        UnifiedPlanObservationResult observation_result =
            make_unified_plan_observation(std::move(input));
        if(!observation_result.is_valid() ||
           observation_result.observation() == nullptr) {
            throw std::logic_error(
                "System/AUR renderer child fixture is invalid.");
        }

        auto projection = std::unique_ptr<UnifiedPlanProjection>(
            new UnifiedPlanProjection(
                std::vector<BuildPlanArtifactTargetProjectionResult>{},
                std::vector<ProjectedBuildPlanArtifactTargets>{}));
        projection->observation_result_.emplace(
            std::move(observation_result));
        return projection;
    }

    static std::unique_ptr<SystemAurUpdateUnifiedPlanProjection> make_auto(
        SystemAurUpdateUnifiedPlanStatus status,
        std::unique_ptr<UnifiedPlanProjection> repository_projection,
        std::unique_ptr<UnifiedPlanProjection> aur_projection,
        std::vector<SystemAurUpdateRequiresCheckAttention>
            requires_check_attentions = {}) {
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
                SystemAurUpdateUnifiedPlanFreshness::
                    CurrentInstalledState,
                SystemAurUpdateUnifiedPlanActualRefresh::
                    AfterRepositorySuccess,
                SystemAurUpdateUnifiedPlanTransactionRelationship::
                    SeparateSequentialTransactions));
    }

    static std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
    make_repo_only(
        std::unique_ptr<UnifiedPlanProjection> repository_projection) {
        return std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>(
            new SystemAurUpdateUnifiedPlanProjection(
                SystemAurUpdateUnifiedPlanStatus::Ready,
                SystemAurUpdateUnifiedPlanMode::RepoOnly,
                {SystemAurUpdateUnifiedPlanPhase::
                     RepositorySystemTransactionIntent},
                std::move(repository_projection), nullptr, {}, std::nullopt,
                std::nullopt, std::nullopt));
    }
};

namespace {

namespace query_stub = local_dependency_plan_query_stub;

class TerminalUnsafeErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override {
        return "terminal-unsafe-fixture";
    }

    std::string message(int) const override {
        return std::string{"system-error\n"} +
               terminal_safe_text_test_cases::bytes({0x1b, 0xe2, 0x80, 0xae,
                                                     0xef, 0xbb, 0xbf});
    }
};

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void expect_contains(
    const std::string& output, std::string_view expected,
    const std::string& context) {
    expect(
        output.find(expected) != std::string::npos,
        context + " is missing: " + std::string(expected));
}

void expect_not_contains(
    const std::string& output, std::string_view unexpected,
    const std::string& context) {
    expect(
        output.find(unexpected) == std::string::npos,
        context + " unexpectedly contains: " + std::string(unexpected));
}

void expect_no_reflected_terminal_controls(
    const std::string& output, std::string_view context) {
    const std::string context_text(context);
    expect(
        output.find('\x1b') == std::string::npos,
        context_text + " contains an actual ESC byte");
    expect(
        output.find('\r') == std::string::npos,
        context_text + " contains an actual carriage return");
    expect(
        output.find('\t') == std::string::npos,
        context_text + " contains an actual tab");
    expect(
        output.find('\x07') == std::string::npos,
        context_text + " contains an actual BEL byte");
    expect(
        output.find('\x7f') == std::string::npos,
        context_text + " contains an actual DEL byte");
}

void expect_before(
    const std::string& output, std::string_view first,
    std::string_view second, const std::string& context) {
    const std::size_t first_position = output.find(first);
    const std::size_t second_position = output.find(second);
    expect(
        first_position != std::string::npos,
        context + " is missing first value: " + std::string(first));
    expect(
        second_position != std::string::npos,
        context + " is missing second value: " + std::string(second));
    expect(
        first_position < second_position,
        context + " changed the observed order");
}

const UnifiedPlanObservation& expect_valid(
    const UnifiedPlanObservationResult& result,
    std::string_view context) {
    expect(
        result.is_valid(),
        std::string(context) + " failed observation validation");
    expect(
        result.observation() != nullptr,
        std::string(context) + " has no observation");
    return *result.observation();
}

std::unique_ptr<UnifiedPlanProjection>
make_repository_system_transaction_child() {
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    RepositoryPackageTransactionIntent transaction;
    transaction.policy.needed = true;
    transaction.stage =
        UnifiedPlanTransactionIntentStage::RepositorySystemUpgrade;
    transaction.targets.push_back(RepositorySystemUpgradeIntent{});
    input.transaction_intents.push_back(std::move(transaction));
    return SystemAurUpdateUnifiedPlanProjectionTestAccess::
        make_child_projection(std::move(input));
}

std::unique_ptr<UnifiedPlanProjection> make_empty_aur_child(
    UnifiedPlanObservationStatus status) {
    UnifiedPlanObservationInput input;
    input.status = status;
    return SystemAurUpdateUnifiedPlanProjectionTestAccess::
        make_child_projection(std::move(input));
}

std::unique_ptr<UnifiedPlanProjection> make_ready_aur_child(
    const BuildPlan& plan,
    const RequiredPackageArtifactTarget& required_artifact,
    const ProvidedDependency& repository_provider) {
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.roots.emplace_back(
        RootTargetIdentity{0, "normal-aur-child"},
        AurRootPackageIdentity{"normal-aur-child", "normal-aur-base"},
        UnifiedPlanRootRouteKind::AurSourceBuild);
    input.build_units.push_back(
        AurPackageBaseBuildUnitReference(std::cref(plan), 0));
    input.required_artifacts.emplace_back(
        AurPackageBaseBuildUnitReference(std::cref(plan), 0),
        std::cref(required_artifact));

    RepositoryPackageTransactionIntent provider_transaction;
    provider_transaction.stage =
        UnifiedPlanTransactionIntentStage::LaterNormalAur;
    provider_transaction.targets.push_back(
        RepositoryProviderInstallIntent{
            UnifiedPlanBorrowedAuthorityReference<ProvidedDependency>(
                repository_provider)});
    input.transaction_intents.push_back(
        std::move(provider_transaction));

    SourceBuiltArtifactInstallBoundaryIntent artifact_transaction;
    artifact_transaction.stage =
        UnifiedPlanTransactionIntentStage::LaterNormalAur;
    artifact_transaction.targets.push_back(
        SourceRootArtifactInstallIntent{0});
    input.transaction_intents.push_back(
        std::move(artifact_transaction));
    return SystemAurUpdateUnifiedPlanProjectionTestAccess::
        make_child_projection(std::move(input));
}

LocalSourceRootObservationIdentity local_source_identity() {
    return LocalSourceRootObservationIdentity{
        "/work/local-suite",
        LocalSourceDirectoryIdentity{
            LocalSourceNodeType::Directory, 41, 73, 1000, 0755}};
}

LocalPackageMetadata local_metadata_fixture() {
    return LocalPackageMetadata{
        "local-base",
        std::nullopt,
        "2.0",
        "1",
        {"x86_64"},
        {LocalPackageMetadataChild{
            "local-child", false, false, {}}},
        {}};
}

BuildPlan build_plan_fixture() {
    BuildPlan plan;
    plan.order.push_back(BuildPlanEntry{
        "aur-base", {"aur-child", "aur-tools"}});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "aur-child",
        "aur-base",
        "repo-dependency>=1",
        PackageRole::RuntimeDependency,
        DependencyKind::Repo,
        "repo-dependency",
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        std::nullopt,
        ResolvedDependencyCandidate{RepositoryExactPackage{
            ConfiguredRepositoryIdentity{"zeta", 0},
            "repo-dependency",
            "repo-dependency",
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                "1.4"),
            {}}},
        ConstraintEvaluation::satisfied()});
    return plan;
}

BuildPlanDependencyEdge unknown_dependency_fixture() {
    return BuildPlanDependencyEdge{
        "blocked-child",
        "blocked-base",
        "missing-runtime>=3",
        PackageRole::RuntimeDependency,
        DependencyKind::Unknown,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        std::nullopt,
        std::nullopt,
        ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::MetadataQueryFailure)};
}

UnifiedPlanRenderingResult render_blocked(UnifiedPlanBlocker blocker) {
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.blockers.push_back(std::move(blocker));
    const UnifiedPlanObservationResult result =
        make_unified_plan_observation(std::move(input));
    return render_unified_plan_observation(
        expect_valid(result, "single blocker fixture"));
}

LocalPackageMetadata local_build_plan_metadata_fixture() {
    using Comparison = LocalPackageMetadataComparison;
    using RelationKind = LocalPackageMetadataRelationKind;
    using ScopeKind = LocalPackageMetadataScopeKind;
    using TargetKind = LocalPackageMetadataRelationTargetKind;

    return LocalPackageMetadata{
        "local-authority-base",
        std::nullopt,
        "1.0",
        "1",
        {"x86_64"},
        {
            LocalPackageMetadataChild{
                "local-provider", false, false, {}},
            LocalPackageMetadataChild{
                "local-consumer", false, false, {}},
        },
        {
            LocalPackageMetadataRelation{
                RelationKind::Provides,
                "virtual-api=1",
                LocalPackageMetadataRelationTarget{
                    TargetKind::Package, "virtual-api",
                    Comparison::Equal, "1"},
                LocalPackageMetadataScope{
                    ScopeKind::ChildPackage,
                    "local-provider"},
                std::nullopt,
                std::nullopt,
                false},
            LocalPackageMetadataRelation{
                RelationKind::Depends,
                "virtual-api>=2",
                LocalPackageMetadataRelationTarget{
                    TargetKind::Package, "virtual-api",
                    Comparison::GreaterThanOrEqual, "2"},
                LocalPackageMetadataScope{
                    ScopeKind::ChildPackage,
                    "local-consumer"},
                std::nullopt,
                std::nullopt,
                false},
        }};
}

void test_ready_rendering_and_identity_boundaries() {
    BuildPlan plan = build_plan_fixture();
    PackageRelationAssessment no_match =
        package_relation_assessment_fixture::
            confirmed_installed_conflict_reason(
                "aur-child", "aur-base", "absent-component")
                .assessment;
    no_match.kind = PackageRelationAssessmentKind::
        ConfirmedNoMatchingCurrentOrPlannedTarget;
    no_match.attributed_package_evidence.reset();
    no_match.attributed_observation_failure.reset();
    no_match.active_evidence.observation_completeness =
        PackageRelationObservationCompleteness::Complete;
    plan.relation_assessments.push_back(std::move(no_match));
    LocalPackageMetadata local_metadata = local_metadata_fixture();
    const LocalSourceRootObservationIdentity local_identity =
        local_source_identity();
    const std::vector<std::string> configured_repositories = {
        "zeta", "core"};
    const std::vector<RequiredPackageArtifactTarget> artifacts = {
        RequiredPackageArtifactTarget{
            "aur-base", "aur-child",
            DesiredInstallReason::Dependency},
        RequiredPackageArtifactTarget{
            "local-base", "local-child",
            DesiredInstallReason::Explicit}};
    const RepositoryExactPackage repository_dependency{
        ConfiguredRepositoryIdentity{"zeta", 0},
        "repo-dependency",
        "repo-dependency",
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage, "1.4"),
        {}};
    const ProvidedDependency repository_provider =
        ProvidedDependency::from_repository(
            "core", 1, "provider-package");

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.roots.emplace_back(
        RootTargetIdentity{0, "same-name"},
        RepositoryRootPackageIdentity{"zeta", "same-name"},
        UnifiedPlanRootRouteKind::RepositoryTransaction);
    input.roots.emplace_back(
        RootTargetIdentity{1, "same-name"},
        AurRootPackageIdentity{"same-name", "aur-base"},
        UnifiedPlanRootRouteKind::AurSourceBuild);
    input.roots.emplace_back(
        RootTargetIdentity{2, "local-child"}, local_identity,
        UnifiedPlanRootRouteKind::LocalSourceBuild);
    input.configured_repository_order.emplace(
        std::cref(configured_repositories));
    input.dependency_authorities.push_back(
        UnifiedPlanDependencyAuthorityReference::from_build_plan(plan));
    input.build_units.push_back(AurPackageBaseBuildUnitReference(
        std::cref(plan), 0));
    input.build_units.push_back(LocalSourceBuildUnitReference(
        local_identity, std::cref(local_metadata)));
    input.required_artifacts.emplace_back(
        AurPackageBaseBuildUnitReference(std::cref(plan), 0),
        std::cref(artifacts[0]));
    input.required_artifacts.emplace_back(
        LocalSourceBuildUnitReference(
            local_identity, std::cref(local_metadata)),
        std::cref(artifacts[1]));

    RepositoryPackageTransactionIntent repository_transaction;
    repository_transaction.policy.needed = true;
    repository_transaction.targets.push_back(RepositoryRootInstallIntent{
        UnifiedPlanRootReference(
            RootTargetIdentity{0, "same-name"},
            RepositoryRootPackageIdentity{"zeta", "same-name"},
            UnifiedPlanRootRouteKind::RepositoryTransaction)});
    repository_transaction.targets.push_back(
        RepositoryDependencyInstallIntent{
            UnifiedPlanBorrowedAuthorityReference<
                RepositoryExactPackage>(repository_dependency)});
    repository_transaction.targets.push_back(
        RepositoryProviderInstallIntent{
            UnifiedPlanBorrowedAuthorityReference<ProvidedDependency>(
                repository_provider)});
    repository_transaction.targets.push_back(
        RepositorySystemUpgradeIntent{});
    input.transaction_intents.push_back(std::move(repository_transaction));

    SourceBuiltArtifactInstallBoundaryIntent source_transaction;
    source_transaction.needed = false;
    source_transaction.targets.push_back(
        SourceDependencyArtifactInstallIntent{0});
    source_transaction.targets.push_back(SourceRootArtifactInstallIntent{1});
    input.transaction_intents.push_back(std::move(source_transaction));

    input.phases = {
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::RequestDiscovery,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                SystemSourceUpgradePhase::Preparation}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Libalpm,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::ForeignInventory}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::ProviderDecision,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurPreparation}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::ExecutionProjection,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                SystemSourceUpgradePhase::Preparation}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::RepositoryTransaction,
            UnifiedPlanAuthorityOwner::Pacman,
            ExistingRoutePhaseReference{
                SystemSourceUpgradePhase::System}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::SourceRetrieval,
            UnifiedPlanAuthorityOwner::Git,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurExecution}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::SourceBuild,
            UnifiedPlanAuthorityOwner::Makepkg,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurExecution}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::ArtifactValidation,
            UnifiedPlanAuthorityOwner::Moguet,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurExecution}},
        UnifiedPlanPhaseReference{
            UnifiedPlanObservationPhase::SourceArtifactInstall,
            UnifiedPlanAuthorityOwner::Pacman,
            ExistingRoutePhaseReference{
                UpgradeAllOperationPhase::AurExecution}}};

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation =
        expect_valid(observation_result, "Ready fixture");
    const UnifiedPlanObservationStatus status_before = observation.status();
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);
    const UnifiedPlanRenderingResult rendered_again =
        render_unified_plan_observation(observation);

    expect(rendered.is_complete(), "Ready rendering is incomplete");
    expect(
        observation.status() == status_before &&
            status_before == UnifiedPlanObservationStatus::Ready,
        "renderer changed Ready status");
    expect(
        rendered.text == rendered_again.text &&
            rendered.issues == rendered_again.issues,
        "renderer output is not deterministic");
    expect_contains(rendered.text, "  Status: Ready", "Ready status");
    expect_before(
        rendered.text, "Identity: zeta/same-name",
        "Identity: AUR/same-name (PackageBase: aur-base)",
        "repository/AUR root order");
    expect_before(
        rendered.text,
        "Identity: AUR/same-name (PackageBase: aur-base)",
        "Identity: /work/local-suite (node type: directory; device: 41; inode: 73)",
        "AUR/local root order");
    expect_contains(
        rendered.text, "Route: repository transaction",
        "repository root route");
    expect_contains(
        rendered.text, "Route: AUR source build", "AUR root route");
    expect_contains(
        rendered.text, "Route: local source build", "local root route");
    expect_contains(
        rendered.text, "External owner: libalpm", "external owner");
    expect_before(
        rendered.text, "  1. request / root discovery",
        "  2. metadata / dependency candidate discovery",
        "early phase order");
    expect_before(
        rendered.text, "  5. repository transaction",
        "  6. source retrieval", "mutation phase order");
    expect_contains(
        rendered.text, "Route phase: system/source: preparation",
        "system/source route phase");
    expect_contains(
        rendered.text, "Route phase: upgrade-all: AUR execution",
        "upgrade-all route phase");
    expect_contains(
        rendered.text,
        "Configured repository order:\n  1. zeta\n  2. core\n",
        "configured repository order");
    expect_contains(
        rendered.text,
        "aur-child (PackageBase: aur-base) -> repo-dependency>=1 [repository; role: runtime dependency]",
        "dependency authority");
    expect_contains(
        rendered.text, "Selected identity: zeta/repo-dependency",
        "repository dependency identity");
    expect_contains(
        rendered.text, "Stored constraint result: Satisfied",
        "stored constraint display");
    expect_contains(
        rendered.text,
        "Package relation assessments:\n       1. Confirmed no matching current or planned target",
        "complete no-match relation authority");
    expect_contains(
        rendered.text, "this relation does not block build/install",
        "complete no-match readiness wording");
    expect_not_contains(
        rendered.text,
        "ConfirmedNoMatchingCurrentOrPlannedTarget",
        "internal no-match enum token");
    expect_contains(
        rendered.text,
        "AUR BuildPlan unit #1 (PackageBase: aur-base)",
        "AUR build unit");
    expect_contains(
        rendered.text, "Child packages: aur-child, aur-tools",
        "PackageBase child identities");
    expect_contains(
        rendered.text,
        "local source /work/local-suite (node type: directory; device: 41; inode: 73; PackageBase: local-base)",
        "local build unit");
    expect_contains(
        rendered.text,
        "Required target: PackageBase: aur-base; child package: aur-child; install reason: dependency",
        "required artifact target");
    expect_contains(
        rendered.text,
        "Repository package transaction (pacman --needed policy: yes)",
        "repository transaction intent");
    expect_contains(
        rendered.text, "dependency: zeta/repo-dependency",
        "repository dependency intent");
    expect_contains(
        rendered.text, "selected provider: core/provider-package",
        "repository provider intent");
    expect_contains(rendered.text, "system upgrade", "system intent");
    expect_contains(
        rendered.text,
        "Source-built artifact install boundary (pacman --needed policy: no)",
        "source artifact boundary");
    expect_contains(
        rendered.text,
        "dependency required artifact #1: AUR BuildPlan unit #1 (PackageBase: aur-base); target: aur-base/aur-child",
        "dependency artifact install intent");
    expect_contains(
        rendered.text,
        "root required artifact #2: local source /work/local-suite (node type: directory; device: 41; inode: 73; PackageBase: local-base); target: local-base/local-child",
        "root artifact install intent");
}

void test_no_op_and_blocked_rendering() {
    UnifiedPlanObservationInput no_op_input;
    no_op_input.status = UnifiedPlanObservationStatus::NoOp;
    const UnifiedPlanObservationResult no_op_result =
        make_unified_plan_observation(std::move(no_op_input));
    const UnifiedPlanObservation& no_op =
        expect_valid(no_op_result, "NoOp fixture");
    const UnifiedPlanRenderingResult no_op_rendered =
        render_unified_plan_observation(no_op);
    expect(no_op_rendered.is_complete(), "NoOp rendering is incomplete");
    expect_contains(
        no_op_rendered.text, "  Status: NoOp", "NoOp status");
    expect_contains(
        no_op_rendered.text, "Transaction intents:\n  None",
        "NoOp transaction boundary");

    const BuildPlanDependencyEdge unknown = unknown_dependency_fixture();
    UnifiedPlanObservationInput blocked_input;
    blocked_input.status = UnifiedPlanObservationStatus::Blocked;
    blocked_input.blockers.push_back(UnknownUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<BuildPlanDependencyEdge>(
            unknown)});
    const UnifiedPlanObservationResult blocked_result =
        make_unified_plan_observation(std::move(blocked_input));
    const UnifiedPlanObservation& blocked =
        expect_valid(blocked_result, "Blocked fixture");
    const UnifiedPlanObservationStatus status_before = blocked.status();
    const UnifiedPlanRenderingResult blocked_rendered =
        render_unified_plan_observation(blocked);
    expect(blocked_rendered.is_complete(), "Blocked rendering is incomplete");
    expect(
        blocked.status() == status_before &&
            status_before == UnifiedPlanObservationStatus::Blocked,
        "renderer changed Blocked status");
    expect_contains(
        blocked_rendered.text, "  Status: Blocked", "Blocked status");
    expect_contains(
        blocked_rendered.text,
        "unknown dependency blocker (UnknownUnifiedPlanBlocker); dependency: missing-runtime>=3; parent: blocked-child (PackageBase: blocked-base)",
        "typed blocker category");
}

void test_local_build_plan_dependency_authority() {
    query_stub::reset_repository_stub();
    query_stub::reset_aur_stub();
    const LocalPackageMetadata metadata =
        local_build_plan_metadata_fixture();
    const LocalBuildPlan local_plan =
        resolve_local_build_plan(metadata, "x86_64");

    expect(
        local_plan.build_plan().dependency_edges.size() == 1 &&
            local_plan.internal_edges().size() == 1 &&
            local_plan.failures().size() == 1,
        "production LocalBuildPlan fixture lost typed dependency state");

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.dependency_authorities.push_back(
        UnifiedPlanDependencyAuthorityReference::from_local_build_plan(
            local_plan));
    input.blockers.push_back(LocalDependencyPlanUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<
            LocalDependencyPlanFailure>(
            local_plan.failures().front())});
    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(expect_valid(
            observation_result, "LocalBuildPlan authority fixture"));

    expect(rendered.is_complete(), "LocalBuildPlan rendering is incomplete");
    expect_contains(
        rendered.text, "  1. LocalBuildPlan",
        "LocalBuildPlan technical authority label");
    expect_contains(
        rendered.text,
        "local-consumer (PackageBase: local-authority-base) -> virtual-api>=2 [local; role: runtime dependency]",
        "LocalBuildPlan dependency edge");
    expect_contains(
        rendered.text,
        "Selected identity: local/local-provider (PackageBase: local-authority-base)",
        "LocalBuildPlan selected candidate");
    expect_contains(
        rendered.text, "Stored constraint result: Unsatisfied",
        "LocalBuildPlan stored constraint result");
    expect_contains(
        rendered.text, "Local internal dependencies:",
        "LocalBuildPlan internal edge section");
    expect_contains(
        rendered.text,
        "LocalDependencyPlanFailureKind::ConstraintMismatch",
        "LocalBuildPlan typed failure detail");
    expect(
        query_stub::repository_query_history().empty() &&
            query_stub::aur_query_history().empty(),
        "local authority fixture unexpectedly queried a remote source");
}

void test_constraint_second_authority_canary() {
    const ConsumerDependencyRequirement raw_requirement(
        "constraint-canary>=9", "constraint-canary",
        DependencyVersionConstraint(
            DependencyVersionRelation::GreaterThanOrEqual, "9"));
    const ObservedVersion observed_version = ObservedVersion::available(
        ObservedVersionSource::RepositoryExactPackage, "1");
    const ConstraintEvaluation reevaluated =
        evaluate_consumer_dependency_requirement(
            raw_requirement, observed_version);
    expect(
        reevaluated.satisfaction() ==
            ConstraintSatisfaction::Unsatisfied,
        "constraint canary does not differ from stored authority");

    BuildPlan plan;
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "constraint-parent",
        "constraint-base",
        "constraint-canary>=9",
        PackageRole::RuntimeDependency,
        DependencyKind::Repo,
        "constraint-canary",
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{raw_requirement},
        ResolvedDependencyCandidate{RepositoryExactPackage{
            ConfiguredRepositoryIdentity{"core", 0},
            "constraint-canary",
            "constraint-canary",
            observed_version,
            {}}},
        ConstraintEvaluation::satisfied()});

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.dependency_authorities.push_back(
        UnifiedPlanDependencyAuthorityReference::from_build_plan(plan));
    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(expect_valid(
            observation_result, "constraint authority canary"));

    expect(rendered.is_complete(), "constraint canary rendering is incomplete");
    expect_contains(
        rendered.text, "Stored constraint result: Satisfied",
        "renderer did not preserve stored constraint authority");
    expect(
        rendered.text.find("Stored constraint result: Unsatisfied") ==
            std::string::npos,
        "renderer exposed a second constraint authority");
}

void test_cross_source_required_artifact_identity() {
    const std::string package_base = "shared-base";
    const std::string package_name = "shared-child";

    BuildPlan aur_plan;
    aur_plan.order.push_back(
        BuildPlanEntry{package_base, {package_name}});
    const LocalPackageMetadata local_metadata{
        package_base,
        std::nullopt,
        "1",
        "1",
        {"x86_64"},
        {LocalPackageMetadataChild{
            package_name, false, false, {}}},
        {}};
    const LocalSourceRootObservationIdentity local_identity{
        "/work/cross-source-local",
        LocalSourceDirectoryIdentity{
            LocalSourceNodeType::Directory, 701, 902, 1000, 0755}};
    RegisteredSourcePreferenceSnapshot prepared_source;
    prepared_source.preference_package_name = package_name;
    prepared_source.canonical_source_identity_key =
        "repository-source-key";
    prepared_source.resolved_package_base = package_base;
    prepared_source.source_kind = SourceBuildSourceKind::Repository;
    prepared_source.required_target_provenance =
        RequiredTargetProvenance::RepositoryExactPackageProjection;
    prepared_source.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    prepared_source.repository_identity =
        ResolvedRepositorySourceBuildIdentity{
            RepositoryPackagePresent{
                "extra", 0, package_name, package_base}};
    const std::string prepared_requested_package = package_name;
    const std::string prepared_checkout_package_base = package_base;
    const std::vector<RequiredPackageArtifactTarget> prepared_targets = {
        RequiredPackageArtifactTarget{
            package_base, package_name,
            DesiredInstallReason::Explicit}};
    const std::vector<RequiredPackageArtifactTarget> other_targets = {
        RequiredPackageArtifactTarget{
            package_base, package_name,
            DesiredInstallReason::Explicit},
        RequiredPackageArtifactTarget{
            package_base, package_name,
            DesiredInstallReason::Explicit}};

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.build_units.push_back(AurPackageBaseBuildUnitReference(
        std::cref(aur_plan), 0));
    input.build_units.push_back(LocalSourceBuildUnitReference(
        local_identity, std::cref(local_metadata)));
    input.build_units.push_back(PreparedSystemSourceBuildUnitReference(
        std::cref(prepared_source),
        std::cref(prepared_requested_package),
        std::cref(prepared_checkout_package_base),
        RequiredTargetProvenance::RepositoryExactPackageProjection,
        ArtifactLifecycleIntent::PackageBaseSet, true,
        std::cref(prepared_targets)));
    input.required_artifacts.emplace_back(
        AurPackageBaseBuildUnitReference(std::cref(aur_plan), 0),
        std::cref(other_targets[0]));
    input.required_artifacts.emplace_back(
        LocalSourceBuildUnitReference(
            local_identity, std::cref(local_metadata)),
        std::cref(other_targets[1]));
    input.required_artifacts.emplace_back(
        PreparedSystemSourceBuildUnitReference(
            std::cref(prepared_source),
            std::cref(prepared_requested_package),
            std::cref(prepared_checkout_package_base),
            RequiredTargetProvenance::
                RepositoryExactPackageProjection,
            ArtifactLifecycleIntent::PackageBaseSet, true,
            std::cref(prepared_targets)),
        std::cref(prepared_targets[0]));

    SourceBuiltArtifactInstallBoundaryIntent install_boundary;
    install_boundary.targets.push_back(
        SourceRootArtifactInstallIntent{0});
    install_boundary.targets.push_back(
        SourceRootArtifactInstallIntent{1});
    install_boundary.targets.push_back(
        SourceRootArtifactInstallIntent{2});
    input.transaction_intents.push_back(std::move(install_boundary));

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(expect_valid(
            observation_result, "cross-source artifact fixture"));

    expect(rendered.is_complete(), "cross-source rendering is incomplete");
    expect_contains(
        rendered.text,
        "Source/build unit: AUR BuildPlan unit #1 (PackageBase: shared-base)",
        "AUR artifact source identity");
    expect_contains(
        rendered.text,
        "Source/build unit: local source /work/cross-source-local (node type: directory; device: 701; inode: 902; PackageBase: shared-base)",
        "local artifact source identity");
    expect_contains(
        rendered.text,
        "Source/build unit: repository source key repository-source-key (requested package: shared-child; checkout PackageBase: shared-base)",
        "prepared artifact source identity");
    expect_contains(
        rendered.text,
        "root required artifact #1: AUR BuildPlan unit #1",
        "AUR source install boundary identity");
    expect_contains(
        rendered.text,
        "root required artifact #2: local source /work/cross-source-local",
        "local source install boundary identity");
    expect_contains(
        rendered.text,
        "root required artifact #3: repository source key repository-source-key",
        "prepared source install boundary identity");
}

void test_build_plan_order_is_preserved() {
    BuildPlan plan;
    plan.order = {
        BuildPlanEntry{"zeta-unit", {"zeta-child"}},
        BuildPlanEntry{"alpha-unit", {"alpha-child"}},
    };

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.build_units.push_back(AurPackageBaseBuildUnitReference(
        std::cref(plan), 0));
    input.build_units.push_back(AurPackageBaseBuildUnitReference(
        std::cref(plan), 1));
    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(expect_valid(
            observation_result, "BuildPlan order fixture"));

    expect(rendered.is_complete(), "BuildPlan order rendering is incomplete");
    expect_before(
        rendered.text, "PackageBase: zeta-unit",
        "PackageBase: alpha-unit", "BuildPlan authority order");
}

void test_blocker_variant_details() {
    const BuildPlanDependencyEdge unknown = unknown_dependency_fixture();
    const AmbiguousProvidedDependency ambiguous{
        "virtual-blocker",
        {ProvidedDependency::from_aur(
            "aur-provider", "aur-provider-base",
            "virtual-blocker", "virtual-blocker=1", "1")}};
    const MixedPackageBaseInstallReasonUnsupported unsupported{
        "mixed-base",
        {MixedPackageBaseInstallReasonArtifact{
            ArtifactPackageIdentity{"mixed-child", "1-1"},
            DesiredInstallReason::Dependency,
            InstalledVersionState::SameVersion,
            ExistingInstallReason::Explicit,
            InstallReasonDirective::AsDependency}}};
    const LocalSourceRootFailure local_source_failure{
        LocalSourceRootStage::PkgbuildRead,
        LocalSourceRootErrorCode::PermissionDenied,
        "/work/blocked-local/PKGBUILD",
        std::make_error_code(std::errc::permission_denied)};
    const ProvidedDependency decoy_provider =
        ProvidedDependency::from_repository(
            "alpha", "constraint-decoy-provider",
            "constraint-virtual", "constraint-virtual=1", "1");
    const ProvidedDependency constraint_provider =
        ProvidedDependency::from_repository(
            "zeta", "constraint-provider",
            "constraint-virtual", "constraint-virtual=3", "3");
    BuildPlan constraint_plan;
    constraint_plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "constraint-decoy-parent",
        "constraint-decoy-base",
        "constraint-virtual>=4",
        PackageRole::BuildDependency,
        DependencyKind::Provided,
        "constraint-decoy-provider",
        std::nullopt,
        decoy_provider,
        ProviderResolutionKind::Unique,
        std::nullopt,
        ResolvedDependencyCandidate{ProviderResolvedDependencyCandidate{
            decoy_provider,
            ObservedVersion::available(
                ObservedVersionSource::
                    RepositoryProviderCapability,
                "1")}},
        ConstraintEvaluation::unsatisfied()});
    constraint_plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "constraint-parent",
        "constraint-parent-base",
        "constraint-virtual>=4",
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        "constraint-provider",
        "constraint-provider-base",
        constraint_provider,
        ProviderResolutionKind::UserSelected,
        std::nullopt,
        ResolvedDependencyCandidate{ProviderResolvedDependencyCandidate{
            constraint_provider,
            ObservedVersion::available(
                ObservedVersionSource::
                    RepositoryProviderCapability,
                "3")}},
        ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::
                CandidateVersionUnavailable)});
    const BuildPlanDependencyEdge& constraint_edge =
        constraint_plan.dependency_edges[1];
    const PlanDeclaredRelationReason relation_reason =
        package_relation_assessment_fixture::
            confirmed_installed_conflict_reason(
                "risk-child", "risk-base", "conflict-a");
    const LocalDependencyPlanFailure local_dependency_failure{
        LocalDependencyPlanFailureKind::ConstraintMismatch,
        "local-parent",
        "virtual-local>=2",
        std::nullopt,
        {LocalDependencyPlanCandidate{
            "local-provider", "virtual-local=1", "1",
            std::nullopt, ConstraintEvaluation::unsatisfied()}}};
    RootPackageInstallPreparationFailure root_preparation_failure;
    root_preparation_failure.details.push_back(
        RootPackageInstallPreparationIssue{
            RootPackageInstallPreparationIssueKind::
                SourceWorkPreparationFailed,
            std::nullopt, std::nullopt, std::nullopt,
            "root work preparation diagnostic"});
    const BuildPlanArtifactTargetProjectionIssue artifact_projection_issue{
        BuildPlanArtifactTargetProjectionIssueKind::PackageBaseMismatch,
        3,
        1,
        {2},
        "expected-base",
        "artifact-child",
        {RootTargetIdentity{4, "artifact-root"}},
        "artifact projection diagnostic"};
    BuildPlan state_plan;
    state_plan.unresolved.push_back("state-missing");
    const AurUpdateExecutionIssue route_issue{
        AurUpdateExecutionReason::ProviderMetadataUnavailable,
        "route-child",
        "route-base",
        "route-dependency>=1",
        "route preflight diagnostic"};
    const AurUpdateExecutionIssue relation_route_issue{
        AurUpdateExecutionReason::ConflictsOrReplacesUnresolved,
        "risk-child",
        "risk-base",
        std::nullopt,
        "DO-NOT-DUPLICATE-RELATION-DIAGNOSTIC",
        std::nullopt,
        relation_reason};

    const auto expect_blocker = [](
                                    UnifiedPlanBlocker blocker,
                                    const std::vector<std::string_view>&
                                        expected,
                                    std::string_view context) {
        const UnifiedPlanRenderingResult rendered =
            render_blocked(std::move(blocker));
        expect(
            rendered.is_complete(),
            std::string(context) + " rendering is incomplete");
        for(const std::string_view fragment : expected) {
            expect_contains(
                rendered.text, fragment, std::string(context));
        }
        return rendered.text;
    };

    expect_blocker(
        UnknownUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                BuildPlanDependencyEdge>(unknown)},
        {"UnknownUnifiedPlanBlocker", "missing-runtime>=3",
         "blocked-child (PackageBase: blocked-base)",
         "dependency kind: unknown", "role: runtime dependency",
         "Unknown (metadata query failed)"},
        "unknown blocker");
    expect_blocker(
        AmbiguousUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AmbiguousProvidedDependency>(ambiguous)},
        {"AmbiguousUnifiedPlanBlocker", "virtual-blocker",
         "AUR/aur-provider (PackageBase: aur-provider-base)",
         "provided dependency: virtual-blocker",
         "capability: virtual-blocker=1", "package version: 1"},
        "ambiguous blocker");
    expect_blocker(
        UnsupportedUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                MixedPackageBaseInstallReasonUnsupported>(
                unsupported)},
        {"MixedPackageBaseInstallReasonUnsupported", "mixed-base",
         "mixed-child 1-1", "desired reason: dependency",
         "InstalledVersionState::SameVersion",
         "InstallReasonDirective::AsDependency"},
        "unsupported blocker");
    expect_blocker(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                LocalSourceRootFailure>(local_source_failure)},
        {"LocalSourceRootFailure", "/work/blocked-local/PKGBUILD",
         "LocalSourceRootStage::PkgbuildRead",
         "LocalSourceRootErrorCode::PermissionDenied"},
        "source failure blocker");
    const std::string constraint_text = expect_blocker(
        ConstraintFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                BuildPlanDependencyEdge>(constraint_edge)},
        {"ConstraintFailureUnifiedPlanBlocker", "constraint-parent",
         "constraint-parent-base", "constraint-virtual>=4",
         "dependency kind: provider", "role: runtime dependency",
         "resolved package: constraint-provider (PackageBase: constraint-provider-base)",
         "selected candidate: zeta/constraint-provider",
         "selected provider: zeta/constraint-provider",
         "Unknown (candidate version cannot be proven)"},
        "constraint blocker");
    expect_not_contains(
        constraint_text, "constraint-decoy-provider",
        "constraint blocker edge correlation");
    expect_blocker(
        MetadataRiskUnifiedPlanBlocker{relation_reason},
        {"package relation blocker: Installed conflict confirmed",
         "declaring package risk-child",
         "PackageBase: risk-base", "declares conflict conflict-a",
         "matched installed package conflict-a",
         "build/install is blocked before mutation"},
        "metadata blocker");
    expect_blocker(
        LocalDependencyPlanUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                LocalDependencyPlanFailure>(
                local_dependency_failure)},
        {"LocalDependencyPlanFailureKind::ConstraintMismatch",
         "parent: local-parent", "dependency: virtual-local>=2",
         "local-provider", "provided capability: virtual-local=1",
         "version: 1", "stored constraint result: Unsatisfied"},
        "local dependency blocker");
    expect_blocker(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(
                root_preparation_failure)},
        {"RootPackageInstallPreparationIssueKind::SourceWorkPreparationFailed",
         "input gate: not observed",
         "root work preparation diagnostic"},
        "root preparation blocker");
    expect_blocker(
        BuildPlanArtifactProjectionUnifiedPlanBlocker{
            artifact_projection_issue},
        {"BuildPlanArtifactTargetProjectionIssueKind::PackageBaseMismatch",
         "BuildPlan unit index: 3", "entry package index: 1",
         "package target indices: 2", "PackageBase: expected-base",
         "package: artifact-child", "artifact-root (invocation index: 4)",
         "artifact projection diagnostic"},
        "artifact projection blocker");
    expect_blocker(
        BuildPlanStateUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<BuildPlan>(
                state_plan),
            BuildPlanStateUnifiedPlanBlockerKind::
                UnresolvedDependency,
            0},
        {"BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency",
         "state-missing"},
        "BuildPlan state blocker");
    expect_blocker(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdateExecutionIssue>(route_issue)},
        {"AurUpdateExecutionReason::ProviderMetadataUnavailable",
         "package: route-child", "PackageBase: route-base",
         "dependency: route-dependency>=1",
         "route preflight diagnostic"},
        "route preflight blocker");
    const std::string relation_route_text = expect_blocker(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdateExecutionIssue>(relation_route_issue)},
        {"AurUpdateExecutionReason::ConflictsOrReplacesUnresolved",
         "package: risk-child", "PackageBase: risk-base",
         "relation assessment: Installed conflict confirmed",
         "matched installed package conflict-a",
         "build/install is blocked before mutation"},
        "relation route preflight blocker");
    expect_not_contains(
        relation_route_text,
        "DO-NOT-DUPLICATE-RELATION-DIAGNOSTIC",
        "relation route preflight diagnostic duplication");
}

void test_invalid_root_search_snapshot_typed_details() {
    const std::string c1_group_repository =
        std::string("zeta-") + "\xC2\x85" + "-group-repo";
    const std::string line_separator_duplicate =
        std::string("zeta-") + "\xE2\x80\xA8" +
        "-duplicate-repo";
    const std::string malformed_unranked_package =
        std::string("zeta-") + static_cast<char>(0xff) +
        "-unranked-child";
    InvalidRootPackageSearchSnapshot validation_snapshot;
    validation_snapshot.validation_failures.push_back(
        RootPackageCandidateValidationFailure{
            RootPackageSourceKind::Aur,
            {
                RootPackageCandidateValidationIssue{
                    RootPackageCandidateValidationIssueKind::
                        InvalidPackageBase,
                    "DO-NOT-ECHO-AUR-BASE"},
                RootPackageCandidateValidationIssue{
                    RootPackageCandidateValidationIssueKind::
                        InvalidVersion,
                    "DO-NOT-ECHO-AUR-VERSION"},
            }});
    validation_snapshot.validation_failures.push_back(
        RootPackageCandidateValidationFailure{
            RootPackageSourceKind::Repository,
            {RootPackageCandidateValidationIssue{
                RootPackageCandidateValidationIssueKind::
                    InvalidRepositoryName,
                "DO-NOT-ECHO-REPOSITORY"}}});
    validation_snapshot.candidate_pair_issues.push_back(
        InconsistentAurRootPackageBase{
            "pair-child", "zeta-pair-base", "alpha-pair-base"});
    validation_snapshot.candidate_pair_issues.push_back(
        ConflictingRootPackageCandidateMetadata{
            RootPackageIdentity{RepositoryRootPackageIdentity{
                "metadata-repo", "metadata-child"}},
            RootPackageCandidateMetadataField::Description,
            "first-description", "second-description"});
    validation_snapshot.invalid_group_matches = {
        InvalidRepositoryRootPackageGroupMatch{
            RepositoryRootPackageIdentity{
                c1_group_repository, "zeta-group-child"},
            "zeta\\group"},
        InvalidRepositoryRootPackageGroupMatch{
            RepositoryRootPackageIdentity{
                "alpha/group-repo", "alpha-group-child"},
            std::nullopt},
    };
    validation_snapshot.duplicate_repository_order_entries = {
        line_separator_duplicate, "alpha-duplicate-repo"};
    validation_snapshot.unranked_repository_candidates = {
        RepositoryRootPackageIdentity{
            "zeta-unranked-repo", malformed_unranked_package},
        RepositoryRootPackageIdentity{
            "alpha-unranked-repo", "alpha-unranked-child"},
    };

    RootPackageInstallPreparationFailure failure;
    failure.details.push_back(std::move(validation_snapshot));
    const UnifiedPlanRenderingResult rendered = render_blocked(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(failure)});

    expect(rendered.is_complete(), "invalid snapshot details are incomplete");
    const std::vector<std::string_view> expected{
        "InvalidRootPackageSearchSnapshot",
        "RootPackageSourceKind::Aur",
        "RootPackageCandidateValidationIssueKind::InvalidPackageBase",
        "RootPackageCandidateValidationIssueKind::InvalidVersion",
        "RootPackageSourceKind::Repository",
        "RootPackageCandidateValidationIssueKind::InvalidRepositoryName",
        "InconsistentAurRootPackageBase",
        "package: pair-child",
        "first PackageBase: zeta-pair-base",
        "second PackageBase: alpha-pair-base",
        "ConflictingRootPackageCandidateMetadata",
        "identity: metadata-repo/metadata-child",
        "RootPackageCandidateMetadataField::Description",
        "first value: first-description",
        "second value: second-description",
        "zeta-\\xC2\\x85-group-repo/zeta-group-child",
        "group: zeta\\x5Cgroup",
        "alpha\\x2Fgroup-repo/alpha-group-child",
        "group: not observed",
        "zeta-\\xE2\\x80\\xA8-duplicate-repo",
        "alpha-duplicate-repo",
        "zeta-unranked-repo/zeta-\\xFF-unranked-child",
        "alpha-unranked-repo/alpha-unranked-child",
    };
    for(const std::string_view fragment : expected) {
        expect_contains(rendered.text, fragment, "invalid snapshot detail");
    }
    expect_not_contains(
        rendered.text, "DO-NOT-ECHO",
        "invalid snapshot raw candidate safety");
    expect_not_contains(
        rendered.text, c1_group_repository,
        "invalid snapshot raw C1 safety");
    expect_not_contains(
        rendered.text, line_separator_duplicate,
        "invalid snapshot raw line-separator safety");
    expect_not_contains(
        rendered.text, malformed_unranked_package,
        "invalid snapshot malformed UTF-8 safety");
    expect_not_contains(
        rendered.text, "zeta\\group",
        "invalid snapshot raw delimiter safety");
    expect_not_contains(
        rendered.text, "alpha/group-repo/alpha-group-child",
        "invalid snapshot raw identity-boundary safety");
    expect_before(
        rendered.text,
        "RootPackageCandidateValidationIssueKind::InvalidPackageBase",
        "RootPackageCandidateValidationIssueKind::InvalidVersion",
        "validation issue authority order");
    expect_before(
        rendered.text, "zeta-\\xE2\\x80\\xA8-duplicate-repo",
        "alpha-duplicate-repo",
        "duplicate repository authority order");
    expect_before(
        rendered.text,
        "zeta-\\xC2\\x85-group-repo/zeta-group-child",
        "alpha\\x2Fgroup-repo/alpha-group-child",
        "invalid group authority order");
    expect_before(
        rendered.text,
        "zeta-unranked-repo/zeta-\\xFF-unranked-child",
        "alpha-unranked-repo/alpha-unranked-child",
        "unranked repository authority order");

    InvalidRootPackageSearchSnapshot incomplete_snapshot;
    incomplete_snapshot.invalid_group_matches.push_back(
        InvalidRepositoryRootPackageGroupMatch{
            RepositoryRootPackageIdentity{
                "present-repository", ""},
            std::nullopt});
    incomplete_snapshot.duplicate_repository_order_entries.push_back("");
    incomplete_snapshot.unranked_repository_candidates.push_back(
        RepositoryRootPackageIdentity{"", "present-package"});
    RootPackageInstallPreparationFailure incomplete_failure;
    incomplete_failure.details.push_back(std::move(incomplete_snapshot));
    const UnifiedPlanRenderingResult incomplete_rendered = render_blocked(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(
                incomplete_failure)});
    expect(
        !incomplete_rendered.is_complete() &&
            incomplete_rendered.issues.size() == 3,
        "empty snapshot identity was rendered complete or duplicated");
    std::string issue_diagnostics;
    for(const UnifiedPlanRenderingIssue& issue :
        incomplete_rendered.issues) {
        expect(
            issue.kind ==
                    UnifiedPlanRenderingIssueKind::
                        MissingReferencedValue &&
                issue.section ==
                    UnifiedPlanRenderingSection::Blockers &&
                issue.item_index == 0 && issue.detail_index == 0,
            "empty snapshot identity issue lost its typed location");
        issue_diagnostics += issue.diagnostic;
        issue_diagnostics += '\n';
    }
    expect_contains(
        issue_diagnostics,
        "invalid group match is missing its package identity",
        "empty group-match identity diagnostic");
    expect_contains(
        issue_diagnostics,
        "duplicate repository-order entry is missing its repository identity",
        "empty duplicate repository identity diagnostic");
    expect_contains(
        issue_diagnostics,
        "unranked repository candidate is missing its repository identity",
        "empty unranked identity diagnostic");
    expect_contains(
        incomplete_rendered.text,
        "InvalidRepositoryRootPackageGroupMatch; identity: present-repository/unavailable; group: not observed",
        "empty group-match identity fallback");
    expect_contains(
        incomplete_rendered.text,
        "duplicate repository-order entry: unavailable",
        "empty duplicate repository fallback");
    expect_contains(
        incomplete_rendered.text,
        "unranked repository candidate: unavailable/present-package",
        "empty snapshot identity fallback");
}

void test_invalid_root_routing_identity_fields() {
    InvalidRootPackageRoutingProjection projection;
    projection.unrepresentable_repository_targets = {
        UnrepresentableRepositoryRootPackageRouteTarget{
            0, RepositoryRootPackageIdentity{".", "dot-package"}},
        UnrepresentableRepositoryRootPackageRouteTarget{
            2,
            RepositoryRootPackageIdentity{
                "..", "parent-package"}},
        UnrepresentableRepositoryRootPackageRouteTarget{
            4,
            RepositoryRootPackageIdentity{
                "nested/repository", "nested-package"}},
    };
    RootPackageInstallPreparationFailure failure;
    failure.details.push_back(std::move(projection));

    const UnifiedPlanRenderingResult rendered = render_blocked(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(failure)});

    expect(rendered.is_complete(), "invalid routing rendering is incomplete");
    expect_contains(
        rendered.text, "InvalidRootPackageRoutingProjection",
        "invalid routing failure kind");
    expect_contains(
        rendered.text,
        "selection index 0; repository: .; package: dot-package",
        "dot repository identity fields");
    expect_contains(
        rendered.text,
        "selection index 2; repository: ..; package: parent-package",
        "parent repository identity fields");
    expect_contains(
        rendered.text,
        "selection index 4; repository: nested/repository; package: nested-package",
        "nested repository identity fields");
    expect_not_contains(
        rendered.text, "nested/repository/nested-package",
        "invalid routing flattened identity");
}

void test_aur_root_preparation_diagnostics_are_terminal_safe() {
    const std::string c1_csi = "\xC2\x9B";
    const std::string line_separator = "\xE2\x80\xA8";
    const std::string root_search_diagnostic =
        std::string(
            "AUR RPC response validation failed for "
            "search[query=\"fixture-root\"]: field error reported "
            "\"root-search-before") +
        c1_csi + "31mroot-search-after" + line_separator + "next\"";
    const std::string escaped_root_search_diagnostic =
        "AUR RPC response validation failed for "
        "search[query=\"fixture-root\"]: field error reported "
        "\"root-search-before"
        "\\xC2\\x9B31mroot-search-after\\xE2\\x80\\xA8next\"";

    RootPackageInstallPreparationFailure root_search_failure;
    root_search_failure.details.push_back(
        AurRootPackageSearchFailure{root_search_diagnostic});
    const UnifiedPlanRenderingResult root_search_rendered = render_blocked(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(
                root_search_failure)});

    expect(
        root_search_rendered.is_complete(),
        "AUR root search diagnostic rendering is incomplete");
    expect_contains(
        root_search_rendered.text, "AurRootPackageSearchFailure",
        "AUR root search failure kind");
    expect_contains(
        root_search_rendered.text, escaped_root_search_diagnostic,
        "AUR root search escaped diagnostic");
    expect_not_contains(
        root_search_rendered.text, c1_csi,
        "AUR root search raw C1 diagnostic");
    expect_not_contains(
        root_search_rendered.text, line_separator,
        "AUR root search raw line separator diagnostic");

    const std::string preparation_diagnostic =
        std::string(
            "AUR RPC response validation failed for "
            "info[package=\"aur-root\"]: field error reported "
            "\"build-plan-before") +
        c1_csi + "31mbuild-plan-after" + line_separator + "next\"";
    const std::string escaped_preparation_diagnostic =
        "AUR RPC response validation failed for "
        "info[package=\"aur-root\"]: field error reported "
        "\"build-plan-before"
        "\\xC2\\x9B31mbuild-plan-after\\xE2\\x80\\xA8next\"";
    RootPackageInstallPreparationFailure preparation_failure;
    preparation_failure.details.push_back(
        RootPackageInstallPreparationIssue{
            RootPackageInstallPreparationIssueKind::
                BuildPlanPreparationFailed,
            std::nullopt, std::nullopt, std::nullopt,
            preparation_diagnostic});
    const UnifiedPlanRenderingResult preparation_rendered = render_blocked(
        RootPackagePreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RootPackageInstallPreparationFailure>(
                preparation_failure)});

    expect(
        preparation_rendered.is_complete(),
        "AUR build-plan preparation diagnostic rendering is incomplete");
    expect_contains(
        preparation_rendered.text,
        "RootPackageInstallPreparationIssueKind::BuildPlanPreparationFailed",
        "AUR build-plan preparation failure kind");
    expect_contains(
        preparation_rendered.text, escaped_preparation_diagnostic,
        "AUR build-plan preparation escaped diagnostic");
    expect_not_contains(
        preparation_rendered.text, c1_csi,
        "AUR build-plan preparation raw C1 diagnostic");
    expect_not_contains(
        preparation_rendered.text, line_separator,
        "AUR build-plan preparation raw line separator diagnostic");
}

void test_untrusted_failure_text_is_terminal_safe() {
    const std::string unsafe_entry_name =
        std::string("entry-path-before\nentry-path-after\t") +
        "\x1b]0;ENTRY-OSC\x07" + "\x7f";
    const std::filesystem::path unsafe_entry_path =
        std::filesystem::path("/preferences") / unsafe_entry_name;
    const std::string unsafe_preference_diagnostic =
        std::string("preference-日本語-before\rpreference-after") +
        "\xC2\x85" + "-C1" + "\x1b[31m-COLOR" +
        "\x1b]0;DIAGNOSTIC-OSC\x07" + "\xE2\x80\xA8" +
        "-LINE-SEPARATOR";

    SystemSourceUpgradeIssue preference_issue;
    preference_issue.kind =
        SystemSourceUpgradeIssueKind::PreferenceEnumerationUnavailable;
    preference_issue.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    preference_issue.phase = SystemSourceUpgradePhase::Preparation;
    preference_issue.source_preference_failure = SourcePreferenceFailure{
        SourcePreferenceFailureKind::InvalidEntryName,
        unsafe_entry_path, std::nullopt, std::nullopt,
        unsafe_preference_diagnostic};
    preference_issue.diagnostic = unsafe_preference_diagnostic;

    const UnifiedPlanRenderingResult preference_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(preference_issue)});

    expect(
        preference_rendered.is_complete(),
        "source preference terminal-safe rendering is incomplete");
    expect_contains(
        preference_rendered.text,
        "SystemSourceUpgradeIssueKind::PreferenceEnumerationUnavailable",
        "source preference issue kind");
    expect_contains(
        preference_rendered.text, "phase: preparation",
        "source preference issue phase");
    expect_contains(
        preference_rendered.text,
        "/preferences/entry-path-before\\x0Aentry-path-after\\x09\\x1B]0;ENTRY-OSC\\x07\\x7F",
        "source preference escaped path");
    const std::string escaped_preference_diagnostic =
        "preference-日本語-before\\x0Dpreference-after"
        "\\xC2\\x85-C1\\x1B[31m-COLOR"
        "\\x1B]0;DIAGNOSTIC-OSC\\x07"
        "\\xE2\\x80\\xA8-LINE-SEPARATOR";
    const std::size_t nested_diagnostic_position =
        preference_rendered.text.find(escaped_preference_diagnostic);
    expect(
        nested_diagnostic_position != std::string::npos,
        "source preference nested diagnostic is not escaped");
    expect(
        preference_rendered.text.find(
            escaped_preference_diagnostic,
            nested_diagnostic_position +
                escaped_preference_diagnostic.size()) !=
            std::string::npos,
        "source preference outer diagnostic is not escaped");
    expect(
        preference_rendered.text.find(
            "entry-path-before\nentry-path-after") ==
            std::string::npos,
        "source preference path injected a rendered line");
    expect(
        preference_rendered.text.find(
            "preference-日本語-before\rpreference-after") ==
            std::string::npos,
        "source preference diagnostic retained a carriage return");
    expect(
        preference_rendered.text.find(
            std::string("\xC2\x85", 2)) == std::string::npos,
        "source preference diagnostic retained a C1 control");
    expect(
        preference_rendered.text.find(
            std::string("\xE2\x80\xA8", 3)) ==
            std::string::npos,
        "source preference diagnostic retained a line separator");
    expect_no_reflected_terminal_controls(
        preference_rendered.text, "source preference output");

    const std::string unsafe_preference_name =
        std::string("invalid-name-before\ninvalid-name-after") +
        "\x1b]0;NAME-OSC\x07";
    SystemSourceUpgradeIssue invalid_name_issue;
    invalid_name_issue.kind =
        SystemSourceUpgradeIssueKind::InvalidPreferenceName;
    invalid_name_issue.impact =
        SystemSourceUpgradeIssueImpact::AffectsSuccess;
    invalid_name_issue.phase = SystemSourceUpgradePhase::RegisteredSource;
    invalid_name_issue.original_preference_index = 3;
    invalid_name_issue.preference_package_name = unsafe_preference_name;
    invalid_name_issue.diagnostic =
        "Ignoring invalid source preference: " +
        unsafe_preference_name;
    const UnifiedPlanRenderingResult invalid_name_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(invalid_name_issue)});
    expect(
        invalid_name_rendered.is_complete(),
        "invalid source preference name rendering is incomplete");
    expect_contains(
        invalid_name_rendered.text,
        "package: invalid-name-before\\x0Ainvalid-name-after\\x1B]0;NAME-OSC\\x07",
        "invalid source preference package field");
    expect(
        invalid_name_rendered.text.find(
            "invalid-name-before\ninvalid-name-after") ==
            std::string::npos,
        "invalid source preference name injected a rendered line");
    expect_no_reflected_terminal_controls(
        invalid_name_rendered.text,
        "invalid source preference name output");

    const std::string unsafe_raw_specification =
        std::string("virtual-provider=1\nCONSTRAINT-LINE\t") +
        "\x1b[31m-CONSTRAINT-COLOR" +
        "\x1b]0;CONSTRAINT-OSC\x07" +
        static_cast<char>(0xff);
    const ProviderCapabilityParseResult parse_result =
        parse_provider_capability(unsafe_raw_specification);
    const DependencyConstraintParseFailure* parse_failure =
        parse_result.failure();
    expect(
        parse_failure != nullptr &&
            parse_failure->kind ==
                DependencyConstraintParseFailureKind::
                    InvalidVersion &&
            parse_failure->raw_specification ==
                unsafe_raw_specification,
        "constraint parser fixture did not retain invalid raw input");
    const RepositoryProviderSourceFailure source_failure{
        ConfiguredRepositoryIdentity{"extra", 1}, *parse_failure};
    const UnifiedPlanRenderingResult constraint_rendered = render_blocked(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RepositoryProviderSourceFailure>(
                source_failure)});

    expect(
        constraint_rendered.is_complete(),
        "constraint terminal-safe rendering is incomplete");
    expect_contains(
        constraint_rendered.text,
        "DependencyConstraintParseFailureKind::InvalidVersion",
        "constraint parse failure kind");
    expect_contains(
        constraint_rendered.text,
        "virtual-provider=1\\x0ACONSTRAINT-LINE\\x09"
        "\\x1B[31m-CONSTRAINT-COLOR"
        "\\x1B]0;CONSTRAINT-OSC\\x07\\xFF",
        "escaped constraint specification");
    expect(
        constraint_rendered.text.find(
            "virtual-provider=1\nCONSTRAINT-LINE") ==
            std::string::npos,
        "constraint specification injected a rendered line");
    expect(
        constraint_rendered.text.find(static_cast<char>(0xff)) ==
            std::string::npos,
        "constraint specification retained invalid UTF-8");
    expect_no_reflected_terminal_controls(
        constraint_rendered.text, "constraint output");
}

void test_slice_five_failure_text_is_terminal_safe() {
    std::string unsafe_diagnostic =
        std::string("slice-five-before\nslice-five-after\\literal") +
        "\x1b]0;SLICE-FIVE-OSC\x07" +
        std::string("\xC2\x85", 2) +
        std::string("\xE2\x80\xA8", 3);
    unsafe_diagnostic.push_back(static_cast<char>(0xff));
    const std::string escaped_diagnostic =
        "slice-five-before\\x0Aslice-five-after\\x5Cliteral"
        "\\x1B]0;SLICE-FIVE-OSC\\x07"
        "\\xC2\\x85\\xE2\\x80\\xA8\\xFF";

    const auto expect_escaped = [&unsafe_diagnostic, &escaped_diagnostic](
                                    const UnifiedPlanRenderingResult& rendered,
                                    std::string_view context) {
        const std::string context_text(context);
        expect(
            rendered.is_complete(),
            context_text + " rendering is incomplete");
        expect_contains(
            rendered.text, escaped_diagnostic,
            context_text + " escaped diagnostic");
        expect_not_contains(
            rendered.text,
            "slice-five-before\nslice-five-after",
            context_text + " raw newline diagnostic");
        expect_not_contains(
            rendered.text, std::string("\xC2\x85", 2),
            context_text + " raw C1 diagnostic");
        expect_not_contains(
            rendered.text, std::string("\xE2\x80\xA8", 3),
            context_text + " raw line-separator diagnostic");
        expect(
            rendered.text.find(static_cast<char>(0xff)) ==
                std::string::npos,
            context_text + " retained invalid UTF-8");
        expect_not_contains(
            rendered.text, unsafe_diagnostic,
            context_text + " raw diagnostic");
        expect_no_reflected_terminal_controls(rendered.text, context);
    };

    SystemSourceUpgradeIssue package_metadata_issue;
    package_metadata_issue.kind =
        SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable;
    package_metadata_issue.impact =
        SystemSourceUpgradeIssueImpact::ObservabilityOnly;
    package_metadata_issue.phase = SystemSourceUpgradePhase::System;
    package_metadata_issue.package_metadata_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::MalformedMetadata,
        unsafe_diagnostic};
    package_metadata_issue.diagnostic =
        "outer package metadata diagnostic";
    const UnifiedPlanRenderingResult package_metadata_rendered =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(
                package_metadata_issue)});
    expect_escaped(package_metadata_rendered, "package metadata failure");

    AurUpdatePreparationIssue aur_preparation_issue;
    aur_preparation_issue.reason =
        AurUpdatePreparationReason::BuildPlanMissing;
    aur_preparation_issue.affected_update_plan_indices = {3};
    aur_preparation_issue.affected_roots = {
        RootTargetIdentity{2, "terminal-safe-root"}};
    aur_preparation_issue.package_name = "terminal-safe-child";
    aur_preparation_issue.package_base = "terminal-safe-base";
    aur_preparation_issue.diagnostic = unsafe_diagnostic;
    const UnifiedPlanRenderingResult aur_preparation_rendered =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdatePreparationIssue>(
                aur_preparation_issue)});
    expect_escaped(aur_preparation_rendered, "AUR preparation failure");

    package_metadata_issue.package_metadata_failure->diagnostic.clear();
    const UnifiedPlanRenderingResult missing_package_diagnostic =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(
                package_metadata_issue)});
    expect(
        !missing_package_diagnostic.is_complete() &&
            missing_package_diagnostic.issues.size() == 1 &&
            missing_package_diagnostic.issues.front().kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue,
        "package metadata missing diagnostic accounting changed");

    aur_preparation_issue.diagnostic.clear();
    const UnifiedPlanRenderingResult missing_aur_diagnostic =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdatePreparationIssue>(
                aur_preparation_issue)});
    expect(
        !missing_aur_diagnostic.is_complete() &&
            missing_aur_diagnostic.issues.size() == 1 &&
            missing_aur_diagnostic.issues.front().kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue,
        "AUR preparation missing diagnostic accounting changed");
}

void test_direct_aur_update_execution_diagnostic_is_terminal_safe() {
    std::string unsafe_diagnostic =
        std::string(
            "direct-before\ndirect-after\rcr-after\ttab-after\\literal") +
        std::string("\x1b", 1) + "esc-after" +
        std::string("\x07", 1) + "bel-after" +
        std::string("\xC2\x85", 2) + "c1-after" +
        std::string("\x7f", 1) + "del-after" +
        std::string("\xE2\x80\xA8", 3) + "line-after" +
        std::string("\xE2\x80\xA9", 3) + "paragraph-after";
    unsafe_diagnostic.push_back(static_cast<char>(0xff));
    const std::string escaped_diagnostic =
        "direct-before\\x0Adirect-after\\x0Dcr-after\\x09tab-after"
        "\\x5Cliteral\\x1Besc-after\\x07bel-after"
        "\\xC2\\x85c1-after\\x7Fdel-after"
        "\\xE2\\x80\\xA8line-after"
        "\\xE2\\x80\\xA9paragraph-after\\xFF";

    const AurUpdateExecutionIssue direct_issue{
        AurUpdateExecutionReason::ProviderMetadataUnavailable,
        "direct-terminal-child", "direct-terminal-base",
        "direct-terminal-dependency>=1", unsafe_diagnostic};
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.blockers.push_back(RoutePreflightUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<AurUpdateExecutionIssue>(
            direct_issue)});
    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation =
        expect_valid(observation_result, "direct AUR execution issue");
    const UnifiedPlanObservationStatus status_before = observation.status();

    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        rendered.is_complete(),
        "direct AUR execution diagnostic rendering is incomplete");
    expect(
        status_before == UnifiedPlanObservationStatus::Blocked &&
            observation.status() == status_before,
        "renderer changed direct AUR execution issue status");
    expect_contains(
        rendered.text, escaped_diagnostic,
        "direct AUR execution escaped diagnostic");
    expect_not_contains(
        rendered.text, "direct-before\ndirect-after",
        "direct AUR execution raw newline diagnostic");
    expect_not_contains(
        rendered.text, std::string("\xC2\x85", 2),
        "direct AUR execution raw C1 diagnostic");
    expect_not_contains(
        rendered.text, std::string("\xE2\x80\xA8", 3),
        "direct AUR execution raw line-separator diagnostic");
    expect_not_contains(
        rendered.text, std::string("\xE2\x80\xA9", 3),
        "direct AUR execution raw paragraph-separator diagnostic");
    expect(
        rendered.text.find(static_cast<char>(0xff)) == std::string::npos,
        "direct AUR execution diagnostic retained invalid UTF-8");
    expect_not_contains(
        rendered.text, "tab-after\\literal",
        "direct AUR execution diagnostic retained literal backslash");
    expect_no_reflected_terminal_controls(
        rendered.text, "direct AUR execution diagnostic");

    AurUpdateExecutionIssue missing_diagnostic_issue;
    missing_diagnostic_issue.reason =
        AurUpdateExecutionReason::ProviderMetadataUnavailable;
    const UnifiedPlanRenderingResult missing_diagnostic = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdateExecutionIssue>(
                missing_diagnostic_issue)});
    expect(
        !missing_diagnostic.is_complete() &&
            missing_diagnostic.issues.size() == 1,
        "direct AUR execution missing diagnostic accounting changed");
    const UnifiedPlanRenderingIssue& missing_issue =
        missing_diagnostic.issues.front();
    expect(
        missing_issue.kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue &&
            missing_issue.section ==
                UnifiedPlanRenderingSection::Blockers &&
            missing_issue.item_index == 0 &&
            !missing_issue.detail_index.has_value(),
        "direct AUR execution missing diagnostic location changed");
    expect_contains(
        missing_diagnostic.text, "diagnostic: not observed",
        "direct AUR execution missing diagnostic display");
}

void test_shared_terminal_safe_corpus_and_owner_bypasses() {
    for(const auto& test_case : terminal_safe_text_test_cases::cases()) {
        const AurUpdateExecutionIssue issue{
            AurUpdateExecutionReason::ProviderMetadataUnavailable,
            "corpus-child", "corpus-base", "corpus-dependency>=1",
            test_case.input};
        const UnifiedPlanRenderingResult rendered = render_blocked(
            RoutePreflightUnifiedPlanBlocker{
                UnifiedPlanBorrowedAuthorityReference<
                    AurUpdateExecutionIssue>(issue)});
        expect(
            rendered.is_complete(),
            std::string("Unified corpus rendering is incomplete for ") +
                std::string(test_case.label));
        expect_contains(
            rendered.text, "diagnostic: " + test_case.expected,
            std::string("Unified corpus differs for ") +
                std::string(test_case.label));
    }

    using terminal_safe_text_test_cases::bytes;
    const std::string bidi = bytes({0xe2, 0x80, 0xae});
    const std::string bom = bytes({0xef, 0xbb, 0xbf});
    const std::string unsafe_path =
        std::string{"/work/local"} + bytes({0x1b}) +
        "escape\nnext" + bidi + bom;
    const LocalSourceRootObservationIdentity local_identity{
        unsafe_path,
        LocalSourceDirectoryIdentity{
            LocalSourceNodeType::Directory, 11, 29, 1000, 0755}};
    const std::vector<std::string> configured_repositories = {
        std::string{"repo-日本語"} + bidi + bom};
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.roots.emplace_back(
        RootTargetIdentity{0, "local-child"}, local_identity,
        UnifiedPlanRootRouteKind::LocalSourceBuild);
    input.configured_repository_order.emplace(
        std::cref(configured_repositories));
    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation = expect_valid(
        observation_result, "terminal-safe local/repository fixture");
    const UnifiedPlanObservationStatus status_before = observation.status();
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        rendered.is_complete() &&
            status_before == UnifiedPlanObservationStatus::Ready &&
            observation.status() == status_before,
        "Terminal-safe rendering changed successful plan state");
    expect_contains(
        rendered.text,
        "/work/local\\x1Bescape\\x0Anext"
        "\\xE2\\x80\\xAE\\xEF\\xBB\\xBF",
        "Successful local path bypass");
    expect_contains(
        rendered.text,
        "repo-日本語\\xE2\\x80\\xAE\\xEF\\xBB\\xBF",
        "Configured repository order bypass");
    expect_not_contains(
        rendered.text, bidi,
        "Successful plan retained a raw bidi control");
    expect_not_contains(
        rendered.text, bom,
        "Successful plan retained a raw BOM");
    expect_no_reflected_terminal_controls(
        rendered.text, "successful local/repository output");

    UpgradeAllOperationIssue nested_diagnostic_issue;
    nested_diagnostic_issue.kind =
        UpgradeAllOperationIssueKind::UnknownFailure;
    nested_diagnostic_issue.phase = UpgradeAllOperationPhase::Preparation;
    nested_diagnostic_issue.diagnostic =
        std::string{"nested"} + bidi + bom;
    const UnifiedPlanRenderingResult nested_diagnostic_rendered =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(nested_diagnostic_issue)});
    expect_contains(
        nested_diagnostic_rendered.text,
        "nested\\xE2\\x80\\xAE\\xEF\\xBB\\xBF",
        "Nested diagnostic bypass");
    expect_not_contains(
        nested_diagnostic_rendered.text, bidi,
        "Nested diagnostic retained a raw bidi control");
    expect_not_contains(
        nested_diagnostic_rendered.text, bom,
        "Nested diagnostic retained a raw BOM");

    PlanDeclaredRelationReason relation_reason =
        package_relation_assessment_fixture::
            confirmed_installed_conflict_reason(
                "relation-child", "relation-base", "relation-target");
    relation_reason.assessment.declaration = DeclaredPackageRelation(
        "relation-child", "relation-base",
        PackageRelationKind::Conflict,
        std::string{"relation"} + bidi + bom,
        std::string{"target"} + bidi, std::nullopt);
    const AurUpdateExecutionIssue relation_issue{
        AurUpdateExecutionReason::ConflictsOrReplacesUnresolved,
        "relation-child", "relation-base", std::nullopt,
        "unselected diagnostic", std::nullopt, relation_reason};
    const UnifiedPlanRenderingResult relation_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdateExecutionIssue>(relation_issue)});
    expect_contains(
        relation_rendered.text,
        "relation\\xE2\\x80\\xAE\\xEF\\xBB\\xBF",
        "Nested relation diagnostic bypass");
    expect_not_contains(
        relation_rendered.text, bidi,
        "Nested relation diagnostic retained a raw bidi control");
    expect_not_contains(
        relation_rendered.text, bom,
        "Nested relation diagnostic retained a raw BOM");

    static const TerminalUnsafeErrorCategory error_category;
    const LocalSourceRootFailure local_failure{
        LocalSourceRootStage::RootInspection,
        LocalSourceRootErrorCode::ReadFailure,
        "/work/error", std::error_code(7, error_category)};
    const UnifiedPlanRenderingResult error_rendered = render_blocked(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                LocalSourceRootFailure>(local_failure)});
    expect_contains(
        error_rendered.text,
        "system-error\\x0A\\x1B\\xE2\\x80\\xAE\\xEF\\xBB\\xBF",
        "System error diagnostic bypass");
    expect_not_contains(
        error_rendered.text, bidi,
        "System error retained a raw bidi control");
    expect_not_contains(
        error_rendered.text, bom,
        "System error retained a raw BOM");
    expect_no_reflected_terminal_controls(
        error_rendered.text, "system error output");
}

void test_source_failure_and_route_preflight_subtypes() {
    const BuildPlanResolutionFailure build_plan_failure{
        BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
        "source-parent",
        "source-base",
        "repository-subject",
        "source-dependency>=1",
        {RootTargetIdentity{2, "source-root"}},
        "BuildPlan source diagnostic"};
    const IncompleteProviderCandidateSet incomplete_providers{
        "partial-provider",
        {ProvidedDependency::from_aur(
            "partial-package", "partial-base",
            "partial-provider", "partial-provider=1", "1")},
        ObservedVersionUnknownReason::PartialSourceFailure};
    const RepositoryExactPackageSourceFailure repository_package_failure{
        ConfiguredRepositoryIdentity{"core", 0},
        "repository-child",
        PackageMetadataFailure{
            PackageMetadataErrorCode::QueryFailed,
            "repository package diagnostic"}};
    const RepositoryProviderSourceFailure repository_provider_failure{
        ConfiguredRepositoryIdentity{"extra", 1},
        DependencyConstraintParseFailure{
            DependencyConstraintParseFailureKind::InvalidVersion,
            "virtual-provider=>"}};
    const LocalSourceRootFailure local_failure{
        LocalSourceRootStage::SrcinfoRead,
        LocalSourceRootErrorCode::ReadFailure,
        "/work/local/.SRCINFO",
        std::make_error_code(std::errc::io_error)};
    const AurUpdateQueryFailure aur_failure{
        {"aur-one", "aur-two"}, "AUR query diagnostic"};
    const SystemSourceUpgradeIssue system_issue{
        SystemSourceUpgradeIssueKind::SourceIdentityResolutionFailed,
        SystemSourceUpgradeIssueImpact::BlocksExecution,
        SystemSourceUpgradePhase::Preparation,
        5,
        "registered-source",
        "registered-base",
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        "system/source diagnostic",
        std::nullopt};
    const UpgradeAllOperationIssue upgrade_issue{
        UpgradeAllOperationIssueKind::AurQueryFailed,
        UpgradeAllOperationPhase::AurQuery,
        1,
        5,
        3,
        2,
        "upgrade-child",
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        "upgrade-all diagnostic"};

    const auto expect_source = [](
                                   SourceFailureUnifiedPlanBlocker blocker,
                                   std::string_view expected) {
        const UnifiedPlanRenderingResult rendered = render_blocked(
            UnifiedPlanBlocker{std::move(blocker)});
        expect(rendered.is_complete(), "source subtype rendering incomplete");
        expect_contains(rendered.text, expected, "source subtype");
    };
    expect_source(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                BuildPlanResolutionFailure>(build_plan_failure)},
        "BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable");
    expect_source(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                IncompleteProviderCandidateSet>(
                incomplete_providers)},
        "ObservedVersionUnknownReason::PartialSourceFailure");
    expect_source(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RepositoryExactPackageSourceFailure>(
                repository_package_failure)},
        "RepositoryExactPackageSourceFailure");
    expect_source(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                RepositoryProviderSourceFailure>(
                repository_provider_failure)},
        "DependencyConstraintParseFailureKind::InvalidVersion");
    expect_source(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                LocalSourceRootFailure>(local_failure)},
        "LocalSourceRootStage::SrcinfoRead");
    expect_source(
        SourceFailureUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdateQueryFailure>(aur_failure)},
        "AurUpdateQueryFailure");

    const UnifiedPlanRenderingResult system_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(system_issue)});
    expect(
        system_rendered.is_complete(),
        "system/source route issue rendering incomplete");
    expect_contains(
        system_rendered.text,
        "SystemSourceUpgradeIssueKind::SourceIdentityResolutionFailed",
        "system/source route issue kind");
    expect_contains(
        system_rendered.text, "registered-source",
        "system/source route package identity");

    const UnifiedPlanRenderingResult upgrade_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(upgrade_issue)});
    expect(
        upgrade_rendered.is_complete(),
        "upgrade-all route issue rendering incomplete");
    expect_contains(
        upgrade_rendered.text,
        "UpgradeAllOperationIssueKind::AurQueryFailed",
        "upgrade-all route issue kind");
    expect_contains(
        upgrade_rendered.text, "BuildPlan unit index: 2",
        "upgrade-all BuildPlan identity");
}

void test_route_preflight_nested_typed_details() {
    const auto expect_route = [](
                                  UnifiedPlanBlocker blocker,
                                  const std::vector<std::string_view>&
                                      expected,
                                  std::string_view context) {
        const UnifiedPlanRenderingResult rendered =
            render_blocked(std::move(blocker));
        expect(
            rendered.is_complete(),
            std::string(context) + " rendering is incomplete");
        for(const std::string_view fragment : expected) {
            expect_contains(rendered.text, fragment, std::string(context));
        }
    };

    SystemSourceUpgradeIssue preference_issue;
    preference_issue.kind =
        SystemSourceUpgradeIssueKind::PreferenceUnavailable;
    preference_issue.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    preference_issue.phase = SystemSourceUpgradePhase::Preparation;
    preference_issue.original_preference_index = 9;
    preference_issue.preference_package_name = "nested-source-package";
    preference_issue.source_preference_failure = SourcePreferenceFailure{
        SourcePreferenceFailureKind::ReadFailed,
        "/preferences/nested-source-package",
        std::make_error_code(std::errc::io_error), std::nullopt,
        "nested source preference diagnostic"};
    preference_issue.diagnostic = "outer source preference diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(preference_issue)},
        {"SystemSourceUpgradeIssueKind::PreferenceUnavailable",
         "preference index: 9", "package: nested-source-package",
         "SourcePreferenceFailure",
         "SourcePreferenceFailureKind::ReadFailed",
         "/preferences/nested-source-package", "system error: generic:",
         "nested source preference diagnostic",
         "outer source preference diagnostic"},
        "system/source preference failure");

    SystemSourceUpgradeIssue metadata_issue;
    metadata_issue.kind =
        SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable;
    metadata_issue.impact =
        SystemSourceUpgradeIssueImpact::ObservabilityOnly;
    metadata_issue.phase = SystemSourceUpgradePhase::System;
    metadata_issue.package_metadata_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::MalformedMetadata,
        "nested system metadata diagnostic"};
    metadata_issue.diagnostic = "outer system metadata diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(metadata_issue)},
        {"SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable",
         "SystemSourceUpgradeIssueImpact::ObservabilityOnly",
         "PackageMetadataFailure",
         "PackageMetadataErrorCode::MalformedMetadata",
         "nested system metadata diagnostic",
         "outer system metadata diagnostic"},
        "system/source metadata failure");

    SystemSourceUpgradeIssue resolution_issue;
    resolution_issue.kind =
        SystemSourceUpgradeIssueKind::CacheAuthorityInvalid;
    resolution_issue.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    resolution_issue.phase = SystemSourceUpgradePhase::Preparation;
    resolution_issue.cache_resolution_failure = xdg_paths::ResolutionFailure{
        xdg_paths::DirectoryKind::Cache,
        xdg_paths::EnvironmentVariable::XdgCacheHome,
        xdg_paths::ResolutionErrorCode::RelativePath};
    resolution_issue.diagnostic = "outer system resolution diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(resolution_issue)},
        {"SystemSourceUpgradeIssueKind::CacheAuthorityInvalid",
         "xdg_paths::ResolutionFailure",
         "xdg_paths::DirectoryKind::Cache", "XDG_CACHE_HOME",
         "xdg_paths::ResolutionErrorCode::RelativePath",
         "outer system resolution diagnostic"},
        "system/source XDG resolution failure");

    SystemSourceUpgradeIssue preparation_issue;
    preparation_issue.kind =
        SystemSourceUpgradeIssueKind::CacheAuthorityInvalid;
    preparation_issue.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    preparation_issue.phase = SystemSourceUpgradePhase::Preparation;
    preparation_issue.cache_preparation_failure =
        xdg_directory_safety::PreparationFailure{
            xdg_paths::DirectoryKind::Cache,
            xdg_directory_safety::PreparationStage::
                ComponentValidation,
            xdg_directory_safety::PreparationErrorCode::
                OwnershipMismatch,
            std::make_error_code(std::errc::permission_denied), 7};
    preparation_issue.diagnostic = "outer system preparation diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(preparation_issue)},
        {"xdg_directory_safety::PreparationFailure",
         "xdg_paths::DirectoryKind::Cache",
         "xdg_directory_safety::PreparationStage::ComponentValidation",
         "xdg_directory_safety::PreparationErrorCode::OwnershipMismatch",
         "system error: generic:", "component index: 7",
         "outer system preparation diagnostic"},
        "system/source directory preparation failure");

    SystemSourceUpgradeIssue trusted_issue;
    trusted_issue.kind =
        SystemSourceUpgradeIssueKind::CacheAuthorityInvalid;
    trusted_issue.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    trusted_issue.phase = SystemSourceUpgradePhase::RegisteredSource;
    trusted_issue.trusted_cache_failure = TrustedCacheFailure{
        TrustedCacheStage::ChildOpen,
        TrustedCacheErrorCode::PermissionDenied,
        std::make_error_code(std::errc::permission_denied)};
    trusted_issue.diagnostic = "outer system trusted-cache diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(trusted_issue)},
        {"TrustedCacheFailure", "TrustedCacheStage::ChildOpen",
         "TrustedCacheErrorCode::PermissionDenied",
         "system error: generic:",
         "outer system trusted-cache diagnostic"},
        "system/source trusted cache failure");

    UpgradeAllOperationIssue upgrade_metadata;
    upgrade_metadata.kind =
        UpgradeAllOperationIssueKind::ForeignInventoryReadFailed;
    upgrade_metadata.phase = UpgradeAllOperationPhase::ForeignInventory;
    upgrade_metadata.package_metadata_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "nested upgrade metadata diagnostic"};
    upgrade_metadata.diagnostic = "outer upgrade metadata diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(upgrade_metadata)},
        {"UpgradeAllOperationIssueKind::ForeignInventoryReadFailed",
         "phase: foreign package inventory",
         "PackageMetadataFailure",
         "PackageMetadataErrorCode::LocalDatabaseUnavailable",
         "nested upgrade metadata diagnostic",
         "outer upgrade metadata diagnostic"},
        "upgrade-all metadata failure");

    UpgradeAllOperationIssue upgrade_resolution;
    upgrade_resolution.kind =
        UpgradeAllOperationIssueKind::CacheAuthorityInvalid;
    upgrade_resolution.phase = UpgradeAllOperationPhase::Preparation;
    upgrade_resolution.cache_resolution_failure =
        xdg_paths::ResolutionFailure{
            xdg_paths::DirectoryKind::State,
            xdg_paths::EnvironmentVariable::XdgStateHome,
            xdg_paths::ResolutionErrorCode::DotComponent};
    upgrade_resolution.diagnostic = "outer upgrade resolution diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(upgrade_resolution)},
        {"UpgradeAllOperationIssueKind::CacheAuthorityInvalid",
         "xdg_paths::ResolutionFailure",
         "xdg_paths::DirectoryKind::State", "XDG_STATE_HOME",
         "xdg_paths::ResolutionErrorCode::DotComponent",
         "outer upgrade resolution diagnostic"},
        "upgrade-all XDG resolution failure");

    UpgradeAllOperationIssue upgrade_preparation;
    upgrade_preparation.kind =
        UpgradeAllOperationIssueKind::CacheAuthorityInvalid;
    upgrade_preparation.phase = UpgradeAllOperationPhase::Preparation;
    upgrade_preparation.cache_preparation_failure =
        xdg_directory_safety::PreparationFailure{
            xdg_paths::DirectoryKind::Cache,
            xdg_directory_safety::PreparationStage::ComponentCreation,
            xdg_directory_safety::PreparationErrorCode::CreationFailed,
            std::make_error_code(std::errc::permission_denied), 3};
    upgrade_preparation.diagnostic =
        "outer upgrade preparation diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(upgrade_preparation)},
        {"xdg_directory_safety::PreparationFailure",
         "xdg_directory_safety::PreparationStage::ComponentCreation",
         "xdg_directory_safety::PreparationErrorCode::CreationFailed",
         "component index: 3", "outer upgrade preparation diagnostic"},
        "upgrade-all directory preparation failure");

    UpgradeAllOperationIssue upgrade_trusted;
    upgrade_trusted.kind =
        UpgradeAllOperationIssueKind::CacheAuthorityInvalid;
    upgrade_trusted.phase = UpgradeAllOperationPhase::AurPreparation;
    upgrade_trusted.trusted_cache_failure = TrustedCacheFailure{
        TrustedCacheStage::Rollback,
        TrustedCacheErrorCode::RollbackRefusal, std::nullopt};
    upgrade_trusted.diagnostic = "outer upgrade trusted-cache diagnostic";
    expect_route(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(upgrade_trusted)},
        {"TrustedCacheFailure", "TrustedCacheStage::Rollback",
         "TrustedCacheErrorCode::RollbackRefusal",
         "system error: not observed",
         "outer upgrade trusted-cache diagnostic"},
        "upgrade-all trusted cache failure");
}

void test_route_preflight_nested_required_field_canaries() {
    const auto expect_single_missing = [](
                                           const UnifiedPlanRenderingResult& rendered,
                                           std::string_view diagnostic,
                                           std::string_view context) {
        expect(
            !rendered.is_complete(),
            std::string(context) + " was rendered complete");
        expect(
            rendered.issues.size() == 1,
            std::string(context) + " duplicated its missing issue");
        const UnifiedPlanRenderingIssue& issue = rendered.issues.front();
        expect(
            issue.kind ==
                    UnifiedPlanRenderingIssueKind::
                        MissingReferencedValue &&
                issue.section ==
                    UnifiedPlanRenderingSection::Blockers &&
                issue.item_index == 0 &&
                !issue.detail_index.has_value(),
            std::string(context) + " lost its typed issue location");
        expect_contains(
            issue.diagnostic, diagnostic,
            std::string(context) + " diagnostic");
    };

    SystemSourceUpgradeIssue unsupported_file_type;
    unsupported_file_type.kind =
        SystemSourceUpgradeIssueKind::PreferenceUnavailable;
    unsupported_file_type.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    unsupported_file_type.phase = SystemSourceUpgradePhase::Preparation;
    unsupported_file_type.original_preference_index = 4;
    unsupported_file_type.preference_package_name =
        "unsupported-file-type-package";
    unsupported_file_type.source_preference_failure =
        SourcePreferenceFailure{
            SourcePreferenceFailureKind::UnsupportedFileType,
            "/preferences/unsupported-file-type-package",
            std::nullopt, std::filesystem::file_type::symlink,
            "unsupported file type diagnostic"};
    unsupported_file_type.diagnostic =
        "outer unsupported file type diagnostic";
    const UnifiedPlanRenderingResult unsupported_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(unsupported_file_type)});
    expect(
        unsupported_rendered.is_complete(),
        "typed unsupported source preference file type was incomplete");
    expect_contains(
        unsupported_rendered.text,
        "SourcePreferenceFailureKind::UnsupportedFileType",
        "unsupported source preference kind");
    expect_contains(
        unsupported_rendered.text,
        "observed file type: std::filesystem::file_type::symlink",
        "unsupported source preference observed file type");

    SystemSourceUpgradeIssue missing_file_type = unsupported_file_type;
    missing_file_type.source_preference_failure->observed_file_type =
        std::nullopt;
    missing_file_type.source_preference_failure->diagnostic =
        "missing observed file type diagnostic";
    missing_file_type.diagnostic =
        "outer missing observed file type diagnostic";
    const UnifiedPlanRenderingResult missing_file_type_rendered =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(missing_file_type)});
    expect_single_missing(
        missing_file_type_rendered,
        "unsupported source preference entry is missing its observed "
        "file type",
        "missing source preference observed file type");
    expect_contains(
        missing_file_type_rendered.text,
        "observed file type: unavailable",
        "missing source preference file type fallback");

    SystemSourceUpgradeIssue missing_package_metadata;
    missing_package_metadata.kind =
        SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable;
    missing_package_metadata.impact =
        SystemSourceUpgradeIssueImpact::ObservabilityOnly;
    missing_package_metadata.phase = SystemSourceUpgradePhase::System;
    missing_package_metadata.diagnostic =
        "missing system package metadata diagnostic";
    const UnifiedPlanRenderingResult missing_package_metadata_rendered =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(
                missing_package_metadata)});
    expect_single_missing(
        missing_package_metadata_rendered,
        "system package snapshot issue is missing its package metadata "
        "failure",
        "missing system package metadata failure");
    expect_contains(
        missing_package_metadata_rendered.text,
        "SystemSourceUpgradeIssueKind::SystemPackageSnapshotUnavailable",
        "missing system package metadata kind");
    expect_contains(
        missing_package_metadata_rendered.text,
        "typed nested details: unavailable",
        "missing system package metadata fallback");

    SystemSourceUpgradeIssue missing_cache_failure;
    missing_cache_failure.kind =
        SystemSourceUpgradeIssueKind::CacheAuthorityInvalid;
    missing_cache_failure.impact =
        SystemSourceUpgradeIssueImpact::BlocksExecution;
    missing_cache_failure.phase = SystemSourceUpgradePhase::Preparation;
    missing_cache_failure.diagnostic =
        "missing typed cache failure diagnostic";
    const UnifiedPlanRenderingResult missing_cache_failure_rendered =
        render_blocked(RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemSourceUpgradeIssue>(missing_cache_failure)});
    expect_single_missing(
        missing_cache_failure_rendered,
        "system/source cache authority issue is missing its typed cache "
        "failure",
        "missing system/source cache failure");
    expect_contains(
        missing_cache_failure_rendered.text,
        "SystemSourceUpgradeIssueKind::CacheAuthorityInvalid",
        "missing system/source cache failure kind");
    expect_contains(
        missing_cache_failure_rendered.text,
        "typed nested details: unavailable",
        "missing system/source cache failure fallback");
}

void test_optional_constraint_evaluation_absence_is_not_missing() {
    BuildPlan plan;
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "unknown-optional-parent",
        "unknown-optional-base",
        "unknown-optional-dependency>=1",
        PackageRole::RuntimeDependency,
        DependencyKind::Unknown,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{ConsumerDependencyRequirement(
            "unknown-optional-dependency>=1",
            "unknown-optional-dependency",
            DependencyVersionConstraint(
                DependencyVersionRelation::GreaterThanOrEqual,
                "1"))},
        std::nullopt,
        std::nullopt});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "ambiguous-optional-parent",
        "ambiguous-optional-base",
        "ambiguous-optional-dependency>=2",
        PackageRole::BuildDependency,
        DependencyKind::AmbiguousProvider,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{ConsumerDependencyRequirement(
            "ambiguous-optional-dependency>=2",
            "ambiguous-optional-dependency",
            DependencyVersionConstraint(
                DependencyVersionRelation::GreaterThanOrEqual,
                "2"))},
        std::nullopt,
        std::nullopt});
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "ambiguous-optional-dependency>=2",
        {ProvidedDependency::from_aur(
            "ambiguous-optional-provider",
            "ambiguous-optional-provider-base",
            "ambiguous-optional-dependency",
            "ambiguous-optional-dependency=2", "2")}});

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.dependency_authorities.push_back(
        UnifiedPlanDependencyAuthorityReference::from_build_plan(plan));
    input.blockers.push_back(UnknownUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<BuildPlanDependencyEdge>(
            plan.dependency_edges[0])});
    input.blockers.push_back(AmbiguousUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<
            AmbiguousProvidedDependency>(
            plan.ambiguous_providers.front())});

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation = expect_valid(
        observation_result,
        "optional constraint evaluation absence fixture");
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked,
        "renderer changed optional constraint fixture status");
    expect(
        rendered.is_complete() && rendered.issues.empty(),
        "optional constraint evaluation absence was reported missing");
    expect_contains(
        rendered.text,
        "unknown-optional-parent (PackageBase: unknown-optional-base) -> "
        "unknown-optional-dependency>=1 [unknown; role: runtime dependency]",
        "optional Unknown dependency edge");
    expect_contains(
        rendered.text,
        "ambiguous-optional-parent (PackageBase: ambiguous-optional-base) "
        "-> ambiguous-optional-dependency>=2 [ambiguous provider; role: "
        "build dependency]",
        "optional Ambiguous dependency edge");
    expect_contains(
        rendered.text, "Stored constraint result: not observed",
        "optional constraint fallback display");

    UpgradeAllOperationIssue cache_exception_issue;
    cache_exception_issue.kind =
        UpgradeAllOperationIssueKind::CacheAuthorityInvalid;
    cache_exception_issue.phase = UpgradeAllOperationPhase::Preparation;
    cache_exception_issue.diagnostic =
        "generic cache authority exception diagnostic";
    const UnifiedPlanRenderingResult cache_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                UpgradeAllOperationIssue>(cache_exception_issue)});
    expect(
        cache_rendered.is_complete() && cache_rendered.issues.empty(),
        "generic upgrade-all cache failure was over-reported missing");
    expect_contains(
        cache_rendered.text,
        "typed nested details: None; diagnostic: generic cache authority exception diagnostic",
        "generic upgrade-all cache failure fallback");
}

void test_blocked_partial_root_identity_is_incomplete_rendering() {
    BuildPlan plan;
    plan.unresolved.push_back("partial-missing-dependency");

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.roots.emplace_back(
        RootTargetIdentity{0, "partial-request"},
        AurRootPackageIdentity{"partial-child", "partial-base"},
        UnifiedPlanRootRouteKind::RepositoryTransaction);
    input.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
        BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency,
        0});

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation = expect_valid(
        observation_result, "Blocked partial root identity fixture");
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked,
        "renderer changed partial root Blocked status");
    expect(
        !rendered.is_complete(),
        "partial root rendering was reported complete");
    expect(
        rendered.issues.size() == 1,
        "partial root rendering did not isolate its missing value");
    const UnifiedPlanRenderingIssue& issue = rendered.issues.front();
    expect(
        issue.kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue &&
            issue.section == UnifiedPlanRenderingSection::Roots &&
            issue.item_index == 0 && !issue.detail_index.has_value(),
        "partial root rendering issue lost its typed location");
    expect_contains(
        issue.diagnostic,
        "root route does not match its typed source identity",
        "partial root rendering diagnostic");
    expect_contains(
        rendered.text,
        "Identity: AUR/partial-child (PackageBase: partial-base)",
        "partial root identity display");
    expect_contains(
        rendered.text, "Typed identity completeness: unavailable",
        "partial root fallback display");
    expect_contains(
        rendered.text, "  Status: Blocked",
        "partial root Blocked status display");
}

void test_blocked_partial_root_request_has_one_specific_issue() {
    BuildPlan plan;
    plan.unresolved.push_back("partial-request-missing-dependency");

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.roots.emplace_back(
        RootTargetIdentity{0, ""},
        RepositoryRootPackageIdentity{
            "partial-request-repository", "partial-request-package"},
        UnifiedPlanRootRouteKind::RepositoryTransaction);
    input.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
        BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency, 0});

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation = expect_valid(
        observation_result, "Blocked partial root request fixture");
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked,
        "renderer changed partial root request Blocked status");
    expect(
        !rendered.is_complete() && rendered.issues.size() == 1,
        "partial root request was complete or duplicated its issue");
    const UnifiedPlanRenderingIssue& issue = rendered.issues.front();
    expect(
        issue.kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue &&
            issue.section == UnifiedPlanRenderingSection::Roots &&
            issue.item_index == 0 && !issue.detail_index.has_value(),
        "partial root request issue lost its typed location");
    expect_contains(
        issue.diagnostic,
        "root is missing its invocation request identity",
        "partial root request diagnostic");
    expect_contains(
        rendered.text,
        "Identity: partial-request-repository/partial-request-package",
        "partial root request identity display");
    expect_contains(
        rendered.text, "Request: unavailable (invocation index: 0)",
        "partial root request fallback");
    expect_not_contains(
        rendered.text, "Typed identity completeness: unavailable",
        "partial root request duplicate generic fallback");
}

void test_blocked_partial_build_unit_is_incomplete_rendering() {
    BuildPlan plan;
    plan.unresolved.push_back("partial-build-unit-dependency");

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.build_units.push_back(AurPackageBaseBuildUnitReference(
        std::cref(plan), 7));
    input.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
        BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency,
        0});

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation = expect_valid(
        observation_result, "Blocked partial build unit fixture");
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked,
        "renderer changed partial build unit Blocked status");
    expect(
        !rendered.is_complete(),
        "partial build unit rendering was reported complete");
    expect(
        rendered.issues.size() == 1,
        "partial build unit rendering duplicated its missing value");
    const UnifiedPlanRenderingIssue& issue = rendered.issues.front();
    expect(
        issue.kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue &&
            issue.section ==
                UnifiedPlanRenderingSection::BuildUnits &&
            issue.item_index == 0 && !issue.detail_index.has_value(),
        "partial build unit rendering issue lost its typed location");
    expect_contains(
        issue.diagnostic,
        "build unit no longer references a BuildPlan entry",
        "partial build unit rendering diagnostic");
    expect_contains(
        rendered.text,
        "AUR BuildPlan unit #8 (PackageBase: unavailable)",
        "partial build unit fallback display");
    expect_contains(
        rendered.text, "  Status: Blocked",
        "partial build unit Blocked status display");
}

void test_blocked_prepared_build_unit_missing_preference_is_not_duplicated() {
    RegisteredSourcePreferenceSnapshot source;
    source.preference_package_name = "";
    source.canonical_source_identity_key = "partial-prepared-source-key";
    source.resolved_package_base = "partial-prepared-base";
    source.source_kind = SourceBuildSourceKind::Repository;
    source.required_target_provenance =
        RequiredTargetProvenance::RepositoryExactPackageProjection;
    source.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility;
    source.repository_identity = ResolvedRepositorySourceBuildIdentity{
        RepositoryPackagePresent{
            "extra", 0, "partial-prepared-package",
            "partial-prepared-base"}};
    const std::string requested_package = "partial-prepared-package";
    const std::string checkout_package_base = "partial-prepared-base";
    const std::vector<RequiredPackageArtifactTarget> targets{
        RequiredPackageArtifactTarget{
            checkout_package_base, requested_package,
            DesiredInstallReason::Explicit}};
    BuildPlan plan;
    plan.unresolved.push_back("partial-prepared-missing-dependency");

    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Blocked;
    input.build_units.push_back(PreparedSystemSourceBuildUnitReference(
        std::cref(source), std::cref(requested_package),
        std::cref(checkout_package_base),
        RequiredTargetProvenance::RepositoryExactPackageProjection,
        ArtifactLifecycleIntent::SingularCompatibility, true,
        std::cref(targets)));
    input.blockers.push_back(BuildPlanStateUnifiedPlanBlocker{
        UnifiedPlanBorrowedAuthorityReference<BuildPlan>(plan),
        BuildPlanStateUnifiedPlanBlockerKind::UnresolvedDependency, 0});

    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation = expect_valid(
        observation_result,
        "Blocked prepared build unit preference fixture");
    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);

    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked,
        "renderer changed prepared build unit Blocked status");
    expect(
        !rendered.is_complete() && rendered.issues.size() == 1,
        "prepared build unit preference was complete or duplicated its issue");
    const UnifiedPlanRenderingIssue& issue = rendered.issues.front();
    expect(
        issue.kind ==
                UnifiedPlanRenderingIssueKind::
                    MissingReferencedValue &&
            issue.section ==
                UnifiedPlanRenderingSection::BuildUnits &&
            issue.item_index == 0 && !issue.detail_index.has_value(),
        "prepared build unit preference issue lost its typed location");
    expect_contains(
        issue.diagnostic,
        "prepared source build unit is missing its source preference identity",
        "prepared build unit preference diagnostic");
    expect_contains(
        rendered.text, "Source preference: unavailable",
        "prepared build unit preference fallback");
}

void test_slice_five_route_authority_rendering() {
    SyncInstallPreparationFailure sync_failure;
    sync_failure.details.push_back(SyncInstallPreparationIssue{
        SyncInstallPreparationIssueKind::InvalidTarget,
        RootTargetIdentity{2, "invalid-sync-root"}, "--invalid-option",
        "invalid sync fixture"});
    sync_failure.details.push_back(SyncRepositoryMetadataReadFailure{
        RootTargetIdentity{3, "metadata-root"},
        RepositoryMetadataFailure{
            RepositoryMetadataFailureKind::SyncDatabaseUnavailable,
            "extra", "repository metadata fixture",
            std::vector<std::string>{"core", "extra"}}});
    const UnifiedPlanRenderingResult sync_rendered = render_blocked(
        SyncInstallPreparationUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SyncInstallPreparationFailure>(sync_failure)});
    expect(sync_rendered.is_complete(), "sync blocker rendering incomplete");
    expect_contains(
        sync_rendered.text,
        "SyncInstallPreparationIssueKind::InvalidTarget",
        "sync route issue kind");
    expect_contains(
        sync_rendered.text,
        "RepositoryMetadataFailureKind::SyncDatabaseUnavailable",
        "sync repository metadata issue kind");

    AurUpdatePreparationIssue aur_issue;
    aur_issue.reason = AurUpdatePreparationReason::BuildPlanMissing;
    aur_issue.affected_update_plan_indices = {4};
    aur_issue.affected_roots = {
        RootTargetIdentity{1, "aur-update-root"}};
    aur_issue.package_name = "aur-update-child";
    aur_issue.package_base = "aur-update-base";
    aur_issue.diagnostic = "AUR source preparation fixture";
    const UnifiedPlanRenderingResult aur_rendered = render_blocked(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdatePreparationIssue>(aur_issue)});
    expect(
        aur_rendered.is_complete(),
        "AUR source preparation blocker rendering incomplete");
    expect_contains(
        aur_rendered.text,
        "AurUpdatePreparationReason::BuildPlanMissing",
        "AUR source preparation reason");
    expect_contains(
        aur_rendered.text, "aur-update-root",
        "AUR source preparation root identity");

    ResolvedSourceBuildIdentity source{
        ResolvedRepositorySourceBuildIdentity{
            RepositoryPackagePresent{
                "extra", 0, "repository-child",
                "repository-base"}}};
    ProductionSourceBuildWorkItem work;
    work.request.package_name = source.requested_name();
    work.request.checkout_name = source.package_base();
    work.request.git_url = source.git_url();
    work.required_targets.push_back(RequiredPackageArtifactTarget{
        source.package_base(), source.requested_name(),
        DesiredInstallReason::Explicit});
    work.required_target_provenance =
        RequiredTargetProvenance::RepositoryExactPackageProjection;
    work.artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    work.repository_identity = *source.repository_identity();
    work.uses_system_update_baseline = true;
    UnifiedPlanObservationInput remote_input;
    remote_input.status = UnifiedPlanObservationStatus::Ready;
    remote_input.build_units.push_back(
        PreparedRemoteSourceBuildUnitReference(
            std::cref(source), std::cref(work)));
    remote_input.required_artifacts.emplace_back(
        PreparedRemoteSourceBuildUnitReference(
            std::cref(source), std::cref(work)),
        std::cref(work.required_targets.front()));
    const UnifiedPlanObservationResult remote_observation =
        make_unified_plan_observation(std::move(remote_input));
    const UnifiedPlanRenderingResult remote_rendered =
        render_unified_plan_observation(expect_valid(
            remote_observation,
            "standalone remote source build rendering fixture"));
    expect(
        remote_rendered.is_complete(),
        "standalone remote source build rendering incomplete");
    expect_contains(
        remote_rendered.text,
        "repository source key repository:repository-base",
        "standalone remote source identity");
    expect_contains(
        remote_rendered.text,
        "requested package: repository-child",
        "standalone remote source requested package");
}

void test_system_aur_update_route_rendering() {
    BuildPlan aur_plan;
    aur_plan.order.push_back(BuildPlanEntry{
        "normal-aur-base", {"normal-aur-child"}});
    const RequiredPackageArtifactTarget required_artifact{
        "normal-aur-base", "normal-aur-child",
        DesiredInstallReason::Explicit};
    const ProvidedDependency repository_provider =
        ProvidedDependency::from_repository(
            "extra", 1, "normal-aur-provider");

    std::unique_ptr<SystemAurUpdateUnifiedPlanProjection> ready =
        SystemAurUpdateUnifiedPlanProjectionTestAccess::make_auto(
            SystemAurUpdateUnifiedPlanStatus::Ready,
            make_repository_system_transaction_child(),
            make_ready_aur_child(
                aur_plan, required_artifact, repository_provider));
    const UnifiedPlanRenderingResult ready_rendered =
        render_system_aur_update_unified_plan(*ready);

    expect(
        ready_rendered.is_complete(),
        "system/AUR Ready rendering is incomplete");
    expect(
        ready->status() == SystemAurUpdateUnifiedPlanStatus::Ready &&
            ready->aur_projection() != nullptr,
        "system/AUR Ready fixture lost its separate AUR child");
    expect_contains(
        ready_rendered.text,
        "System + normal AUR update plan:\n  Status: Ready",
        "system/AUR aggregate Ready status");
    expect_contains(
        ready_rendered.text,
        "Current-state normal AUR phase:\nUnified plan:\n  Status: Ready",
        "system/AUR current-state Ready child");
    expect_contains(
        ready_rendered.text,
        "AUR assessment is based on the current installed state.",
        "system/AUR current-state freshness");
    expect_contains(
        ready_rendered.text,
        "Actual execution re-evaluates AUR state after the repository upgrade succeeds.",
        "system/AUR post-repository refresh");
    expect_contains(
        ready_rendered.text,
        "The repository system transaction and later normal AUR transactions are separate intents.",
        "system/AUR separate transaction relationship");

    const std::string phase_one =
        "Phase 1: official repository system-upgrade intent";
    const std::string phase_two =
        "Phase 2: current installed foreign/AUR state observation";
    const std::string phase_three =
        "Phase 3: current-state normal AUR assessment";
    const std::string phase_four =
        "Phase 4: potential later normal AUR build/install intents";
    expect_before(
        ready_rendered.text, phase_one, phase_two,
        "system/AUR conceptual phase 1/2 order");
    expect_before(
        ready_rendered.text, phase_two, phase_three,
        "system/AUR conceptual phase 2/3 order");
    expect_before(
        ready_rendered.text, phase_three, phase_four,
        "system/AUR conceptual phase 3/4 order");

    expect_before(
        ready_rendered.text, "Repository system phase:",
        "Repository system transaction intent",
        "system/AUR repository heading/intent order");
    expect_before(
        ready_rendered.text, "Repository system transaction intent",
        "Current-state normal AUR phase:",
        "system/AUR repository and AUR section separation");
    expect_before(
        ready_rendered.text, "Current-state normal AUR phase:",
        "Later normal AUR dependency/provider transaction intent",
        "system/AUR later provider intent ownership");
    expect_before(
        ready_rendered.text,
        "Later normal AUR dependency/provider transaction intent",
        "Later normal AUR build/install transaction intent",
        "system/AUR later transaction intent order");

    std::unique_ptr<SystemAurUpdateUnifiedPlanProjection> no_op_child =
        SystemAurUpdateUnifiedPlanProjectionTestAccess::make_auto(
            SystemAurUpdateUnifiedPlanStatus::Ready,
            make_repository_system_transaction_child(),
            make_empty_aur_child(UnifiedPlanObservationStatus::NoOp),
            {SystemAurUpdateRequiresCheckAttention{
                0,
                "foo-git",
                "foo-git",
                DevelRequiresCheckReason::SuffixCandidateOnly,
                AurUpdateExecutionSkipKind::
                    IndependentDevelRequiresCheck,
                AurUpdateEffectiveState::RequiresCheck}});
    const UnifiedPlanRenderingResult no_op_rendered =
        render_system_aur_update_unified_plan(*no_op_child);
    expect(
        no_op_rendered.is_complete(),
        "system/AUR NoOp-child rendering is incomplete");
    expect(
        no_op_child->status() ==
                SystemAurUpdateUnifiedPlanStatus::Ready &&
            no_op_child->requires_check_attentions().size() == 1 &&
            no_op_child->requires_check_attentions().front() ==
                SystemAurUpdateRequiresCheckAttention{
                    0,
                    "foo-git",
                    "foo-git",
                    DevelRequiresCheckReason::SuffixCandidateOnly,
                    AurUpdateExecutionSkipKind::
                        IndependentDevelRequiresCheck,
                    AurUpdateEffectiveState::RequiresCheck},
        "current AUR NoOp flattened the combined route to NoOp");
    expect_contains(
        no_op_rendered.text,
        "System + normal AUR update plan:\n  Status: Ready",
        "system/AUR NoOp-child aggregate status");
    expect_contains(
        no_op_rendered.text,
        "Current-state normal AUR phase:\nUnified plan:\n  Status: NoOp",
        "system/AUR NoOp child status");
    expect_contains(
        no_op_rendered.text, "Repository system transaction intent",
        "system/AUR NoOp-child repository intent");
    expect_contains(
        no_op_rendered.text,
        "Attention-required details:\n  foo-git (PackageBase: foo-git): skipped: devel update requires check: suffix candidate only; not automatically updated because authoritative build provenance is unavailable",
        "system/AUR RequiresCheck attention");

    const std::string unsafe_diagnostic =
        std::string("query-before\nquery-after\rcr-after\ttab-after") +
        std::string("\x1b", 1) + "[31mred-after" +
        std::string("\x07", 1) + "bel-after" +
        std::string("\x7f", 1) + "del-after";
    const std::string escaped_diagnostic =
        "query-before\\x0Aquery-after\\x0Dcr-after\\x09tab-after"
        "\\x1B[31mred-after\\x07bel-after\\x7Fdel-after";
    const SystemAurUpdateDryRunIssue query_failure{
        SystemAurUpdateDryRunIssueKind::AurQueryFailure,
        std::nullopt, unsafe_diagnostic};
    UnifiedPlanObservationInput blocked_child_input;
    blocked_child_input.status = UnifiedPlanObservationStatus::Blocked;
    blocked_child_input.blockers.push_back(
        RoutePreflightUnifiedPlanBlocker{
            UnifiedPlanBorrowedAuthorityReference<
                SystemAurUpdateDryRunIssue>(query_failure)});
    std::unique_ptr<SystemAurUpdateUnifiedPlanProjection> blocked =
        SystemAurUpdateUnifiedPlanProjectionTestAccess::make_auto(
            SystemAurUpdateUnifiedPlanStatus::Blocked,
            make_repository_system_transaction_child(),
            SystemAurUpdateUnifiedPlanProjectionTestAccess::
                make_child_projection(std::move(blocked_child_input)));
    const UnifiedPlanRenderingResult blocked_rendered =
        render_system_aur_update_unified_plan(*blocked);
    expect(
        blocked_rendered.is_complete(),
        "system/AUR Blocked-child rendering is incomplete");
    expect_contains(
        blocked_rendered.text,
        "System + normal AUR update plan:\n  Status: Blocked",
        "system/AUR aggregate Blocked status");
    expect_before(
        blocked_rendered.text, "Repository system transaction intent",
        "Current-state normal AUR phase:",
        "system/AUR Blocked child retained repository intent");
    expect_contains(
        blocked_rendered.text,
        "System/AUR current-state observation failure",
        "system/AUR typed blocker");
    expect_contains(
        blocked_rendered.text, escaped_diagnostic,
        "system/AUR terminal-safe blocker diagnostic");
    expect_not_contains(
        blocked_rendered.text, "query-before\nquery-after",
        "system/AUR raw blocker newline");
    expect_no_reflected_terminal_controls(
        blocked_rendered.text,
        "system/AUR blocker diagnostic");

    std::unique_ptr<SystemAurUpdateUnifiedPlanProjection> repo_only =
        SystemAurUpdateUnifiedPlanProjectionTestAccess::make_repo_only(
            make_repository_system_transaction_child());
    const UnifiedPlanRenderingResult repo_only_rendered =
        render_system_aur_update_unified_plan(*repo_only);
    expect(
        repo_only_rendered.is_complete(),
        "system/AUR RepoOnly rendering is incomplete");
    expect(
        repo_only->aur_projection() == nullptr &&
            repo_only->requires_check_attentions().empty(),
        "system/AUR RepoOnly fixture retained an AUR child");
    expect_contains(
        repo_only_rendered.text,
        "Repository system update plan:\n  Status: Ready",
        "system/AUR RepoOnly aggregate status");
    expect_contains(
        repo_only_rendered.text,
        "Repository update phase:\n  Phase 1: official repository system-upgrade intent",
        "system/AUR RepoOnly phase heading");
    expect_contains(
        repo_only_rendered.text,
        "Phase 1: official repository system-upgrade intent",
        "system/AUR RepoOnly repository phase");
    expect_contains(
        repo_only_rendered.text, "Repository system transaction intent",
        "system/AUR RepoOnly repository intent");
    expect_not_contains(
        repo_only_rendered.text, "Current-state normal AUR phase:",
        "system/AUR RepoOnly AUR section");
    expect_not_contains(
        repo_only_rendered.text, "System + normal AUR update plan:",
        "system/AUR RepoOnly combined title");
    expect_not_contains(
        repo_only_rendered.text, "Combined update phases:",
        "system/AUR RepoOnly combined phase heading");
    expect_not_contains(
        repo_only_rendered.text, "Freshness:",
        "system/AUR RepoOnly freshness section");
    expect_not_contains(
        repo_only_rendered.text,
        "AUR assessment is based on the current installed state.",
        "system/AUR RepoOnly freshness wording");
    expect_not_contains(
        repo_only_rendered.text,
        "potential later normal AUR build/install intents",
        "system/AUR RepoOnly later AUR phase");
    expect_not_contains(
        repo_only_rendered.text, "Phase 2:",
        "system/AUR RepoOnly later phase");
    expect_not_contains(
        repo_only_rendered.text, "Attention-required details:",
        "system/AUR RepoOnly RequiresCheck attention");
}

void test_rendering_issue_is_isolated_from_execution_status() {
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::Ready;
    input.phases.push_back(UnifiedPlanPhaseReference{
        UnifiedPlanObservationPhase::RequestDiscovery,
        static_cast<UnifiedPlanAuthorityOwner>(999), std::nullopt});
    const UnifiedPlanObservationResult observation_result =
        make_unified_plan_observation(std::move(input));
    const UnifiedPlanObservation& observation =
        expect_valid(observation_result, "unsupported owner fixture");

    const UnifiedPlanRenderingResult rendered =
        render_unified_plan_observation(observation);
    expect(!rendered.is_complete(), "unsupported owner rendered as complete");
    expect(rendered.issues.size() == 1, "unexpected rendering issue count");
    expect(
        rendered.issues.front().kind ==
                UnifiedPlanRenderingIssueKind::UnsupportedValue &&
            rendered.issues.front().section ==
                UnifiedPlanRenderingSection::Phases,
        "unsupported owner issue lost typed presentation location");
    expect_contains(
        rendered.text, "  Status: Ready",
        "rendering issue status isolation");
    expect_contains(
        rendered.text, "External owner: unsupported",
        "unsupported owner fallback");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Ready,
        "rendering issue changed execution status");
}

void test_git_revision_basis_is_truthful() {
    AurUpdatePlanEntry update{"same-version-git", "1-1", InstalledPackageReason::Explicit,
                              AurUpdateRemotePackage{"same-version-git", "same-version", "1-1", AurVersionRelation::SameAsInstalled}, AurUpdateClassification::UpToDate};
    update.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
    update.devel_assessment = DevelUpdateAssessment::update_available();
    UnifiedPlanObservationInput input;
    input.status = UnifiedPlanObservationStatus::NoOp;
    input.root_metadata.push_back(UnifiedPlanBorrowedAuthorityReference<AurUpdatePlanEntry>(update));
    auto observed = make_unified_plan_observation(std::move(input));
    const auto rendered = render_unified_plan_observation(expect_valid(observed, "Git basis"));
    expect_contains(rendered.text, "Observed update basis: same-version-git", "Git basis name");
    expect_contains(rendered.text, "Git revision difference", "Git basis explanation");
    expect(rendered.text.find("1-1 -> 1-1") == std::string::npos, "Git update rendered a fake version arrow");
}

} // namespace

int main() {
    try {
        test_git_revision_basis_is_truthful();
        test_ready_rendering_and_identity_boundaries();
        test_no_op_and_blocked_rendering();
        test_local_build_plan_dependency_authority();
        test_constraint_second_authority_canary();
        test_cross_source_required_artifact_identity();
        test_build_plan_order_is_preserved();
        test_blocker_variant_details();
        test_invalid_root_search_snapshot_typed_details();
        test_invalid_root_routing_identity_fields();
        test_aur_root_preparation_diagnostics_are_terminal_safe();
        test_untrusted_failure_text_is_terminal_safe();
        test_slice_five_failure_text_is_terminal_safe();
        test_direct_aur_update_execution_diagnostic_is_terminal_safe();
        test_shared_terminal_safe_corpus_and_owner_bypasses();
        test_source_failure_and_route_preflight_subtypes();
        test_route_preflight_nested_typed_details();
        test_route_preflight_nested_required_field_canaries();
        test_optional_constraint_evaluation_absence_is_not_missing();
        test_blocked_partial_root_identity_is_incomplete_rendering();
        test_blocked_partial_root_request_has_one_specific_issue();
        test_blocked_partial_build_unit_is_incomplete_rendering();
        test_blocked_prepared_build_unit_missing_preference_is_not_duplicated();
        test_slice_five_route_authority_rendering();
        test_system_aur_update_route_rendering();
        test_rendering_issue_is_isolated_from_execution_status();
        std::cout << "unified plan renderer tests passed" << std::endl;
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "unified plan renderer test failed: " << error.what()
                  << std::endl;
        return 1;
    }
}
