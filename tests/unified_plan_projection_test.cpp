#include "aur_update_execution_preparation.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_query.hpp"
#include "commands_sync.hpp"
#include "local_dependency_plan_projection.hpp"
#include "package_relation_assessment_fixture.hpp"
#include "root_package_route_projection.hpp"
#include "system_aur_update_operation.hpp"
#include "system_source_upgrade.hpp"
#include "unified_plan_projection.hpp"
#include "upgrade_all_operation.hpp"

#include <algorithm>
#include <concepts>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

struct UnifiedPlanProjectionTestAccess {
    static LocalSourceBuildProjectionAuthority make_local_source(
        const LocalSourceRoot& source_root,
        const LocalBuildPlan& local_plan,
        const SourceBuildEnvironment& source_environment) {
        return LocalSourceBuildProjectionAuthority(
            source_root, local_plan, local_plan.local_metadata(),
            source_environment, local_plan.effective_architecture(),
            LocalSourceBuildMetadataProvenance::ExistingSrcinfo,
            source_root.directory_identity(), source_root.pkgbuild());
    }

    static SystemSourceUpgradeProjectionAuthority make_system_source(
        const SystemSourceUpgradePreparedSnapshot& snapshot,
        const BuildPlan* aur_plan,
        const std::vector<SystemSourceUpgradeIssue>& issues,
        const std::vector<ProductionSourceBuildWorkItem>& work_items) {
        if(snapshot.registered_sources.size() != work_items.size()) {
            throw std::invalid_argument(
                "test source/work fixture size mismatch");
        }
        std::vector<PreparedSystemSourceWorkReference> references;
        references.reserve(work_items.size());
        for(std::size_t index = 0; index < work_items.size(); ++index) {
            references.push_back(PreparedSystemSourceWorkReference(
                snapshot.registered_sources[index],
                work_items[index]));
        }
        return SystemSourceUpgradeProjectionAuthority(
            snapshot, aur_plan, issues, std::move(references));
    }

    static UpgradeAllOperationProjectionAuthority make_upgrade_all(
        const UpgradeAllOperationPreparedSnapshot& snapshot,
        const SystemSourceUpgradeProjectionAuthority& system_source) {
        return UpgradeAllOperationProjectionAuthority(
            snapshot, system_source);
    }
};

namespace {

template <typename T>
concept HasExecuteMember = requires(T value) { value.execute(); };

template <typename T>
concept HasCommandMember = requires(T value) { value.command; };

template <typename T>
concept HasArgvMember = requires(T value) { value.argv; };

template <typename T>
concept HasArtifactProjectionAccessor = requires(const T& value) {
    value.artifact_target_projections();
};

template <typename T>
concept CanProjectSystemAurUpdate = requires(T&& value) {
    project_system_aur_update_unified_plan(std::forward<T>(value));
};

template <typename T>
concept HasRepositoryConfigurationMember = requires(T value) {
    value.repository_configuration;
};

template <typename T>
concept HasLocalSourceRootMember = requires(T value) {
    value.source_root;
};

template <typename T>
concept HasLocalBuildPlanMember = requires(T value) {
    value.local_build_plan;
};

template <typename T>
concept HasNeededMember = requires(T value) {
    value.needed;
};

using ProjectionResultAccess = decltype(std::declval<const UnifiedPlanProjection&>().observation_result());
using ObservationAccess = decltype(std::declval<const UnifiedPlanObservationResult&>().observation());
using ObservationBorrow = decltype(*std::declval<const UnifiedPlanObservationResult&>().observation());

static_assert(!HasExecuteMember<UnifiedPlanProjection>);
static_assert(!HasCommandMember<UnifiedPlanProjection>);
static_assert(!HasArgvMember<UnifiedPlanProjection>);
static_assert(!HasExecuteMember<SystemSourceUpgradeProjectionAuthority>);
static_assert(!HasCommandMember<SystemSourceUpgradeProjectionAuthority>);
static_assert(!HasArgvMember<SystemSourceUpgradeProjectionAuthority>);
static_assert(!HasExecuteMember<UpgradeAllOperationProjectionAuthority>);
static_assert(!HasCommandMember<UpgradeAllOperationProjectionAuthority>);
static_assert(!HasArgvMember<UpgradeAllOperationProjectionAuthority>);
static_assert(!std::is_copy_constructible_v<UnifiedPlanProjection>);
static_assert(!std::is_move_constructible_v<UnifiedPlanProjection>);
static_assert(!std::is_copy_constructible_v<UnifiedPlanObservationResult>);
static_assert(!std::is_copy_constructible_v<UnifiedPlanObservation>);
static_assert(!std::is_copy_constructible_v<RequiredArtifactTargetReference>);
static_assert(!std::is_constructible_v<
              RequiredArtifactTargetReference,
              const RequiredArtifactTargetReference&&>);
static_assert(!std::is_copy_constructible_v<UnifiedPlanBlocker>);
static_assert(!std::is_constructible_v<
              UnifiedPlanBlocker, const UnifiedPlanBlocker&&>);
static_assert(!std::is_copy_constructible_v<UnifiedPlanTransactionIntent>);
static_assert(!std::is_copy_constructible_v<
              UnifiedPlanBuildUnitReference>);
static_assert(!std::is_copy_constructible_v<
              UnifiedPlanRootMetadataAuthorityReference>);
static_assert(!std::is_copy_constructible_v<
              UnifiedPlanRoutePreflightAuthorityReference>);
static_assert(!std::is_copy_constructible_v<
              PreparedSystemSourceWorkReference>);
static_assert(!std::is_constructible_v<
              PreparedSystemSourceWorkReference,
              const PreparedSystemSourceWorkReference&&>);
static_assert(!HasArtifactProjectionAccessor<UnifiedPlanProjection>);
static_assert(!HasRepositoryConfigurationMember<
              LocalSourceUnifiedPlanProjectionInput>);
static_assert(!HasRepositoryConfigurationMember<
              AurUpdateUnifiedPlanProjectionInput>);
static_assert(!HasRepositoryConfigurationMember<
              SystemSourceUpgradeUnifiedPlanProjectionInput>);
static_assert(!HasRepositoryConfigurationMember<
              UpgradeAllUnifiedPlanProjectionInput>);
static_assert(!HasLocalSourceRootMember<
              LocalSourceUnifiedPlanProjectionInput>);
static_assert(!HasLocalBuildPlanMember<
              LocalSourceUnifiedPlanProjectionInput>);
static_assert(!HasNeededMember<UpgradeAllUnifiedPlanProjectionInput>);
using NestedProviderBorrow = decltype(std::declval<const RepositoryProviderInstallIntent&>().provider);
static_assert(!std::is_copy_constructible_v<NestedProviderBorrow>);
static_assert(!std::is_constructible_v<
              NestedProviderBorrow, const NestedProviderBorrow&&>);
static_assert(std::same_as<
              ProjectionResultAccess,
              const UnifiedPlanObservationResult&>);
static_assert(std::same_as<
              ObservationAccess,
              const UnifiedPlanObservation*>);
static_assert(!std::constructible_from<
              UnifiedPlanObservationResult,
              ProjectionResultAccess>);
static_assert(!std::constructible_from<
              UnifiedPlanObservation,
              ObservationBorrow>);
static_assert(!std::is_constructible_v<UnifiedPlanBlocker, std::string>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const PreparedRootPackageInstall>,
              PreparedRootPackageInstall&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<
                  const RootPackageInstallPreparationFailure>,
              RootPackageInstallPreparationFailure&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const PreparedSyncInstall>,
              PreparedSyncInstall&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const SyncInstallPreparationFailure>,
              SyncInstallPreparationFailure&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const BuildPlan>, BuildPlan&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const LocalBuildPlan>,
              LocalBuildPlan&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<
                  const LocalSourceBuildProjectionAuthority>,
              LocalSourceBuildProjectionAuthority&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<
                  const SystemSourceUpgradeProjectionAuthority>,
              SystemSourceUpgradeProjectionAuthority&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const SystemSourceUpgradeResult>,
              SystemSourceUpgradeResult&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<
                  const UpgradeAllOperationProjectionAuthority>,
              UpgradeAllOperationProjectionAuthority&&>);
static_assert(!std::constructible_from<
              std::reference_wrapper<const UpgradeAllOperationResult>,
              UpgradeAllOperationResult&&>);
static_assert(!HasExecuteMember<SystemAurUpdateDryRunObservation>);
static_assert(!HasCommandMember<SystemAurUpdateDryRunObservation>);
static_assert(!HasArgvMember<SystemAurUpdateDryRunObservation>);
static_assert(!HasExecuteMember<SystemAurUpdateUnifiedPlanProjection>);
static_assert(!HasCommandMember<SystemAurUpdateUnifiedPlanProjection>);
static_assert(!HasArgvMember<SystemAurUpdateUnifiedPlanProjection>);
static_assert(!std::is_copy_constructible_v<
              SystemAurUpdateUnifiedPlanProjection>);
static_assert(!std::is_move_constructible_v<
              SystemAurUpdateUnifiedPlanProjection>);
static_assert(!std::constructible_from<
              PreparedSystemAurUpdateOperation,
              SystemAurUpdateDryRunObservation>);
static_assert(!std::convertible_to<
              SystemAurUpdateDryRunObservation,
              PreparedSystemAurUpdateOperation>);
static_assert(!std::invocable<
              decltype(&prepare_system_aur_update_operation),
              SystemAurUpdateDryRunObservation>);
static_assert(!std::invocable<
              decltype(&execute_prepared_system_aur_update_operation),
              SystemAurUpdateDryRunObservation,
              const AppConfig&>);
static_assert(CanProjectSystemAurUpdate<
              const SystemAurUpdateDryRunObservation&>);
static_assert(!CanProjectSystemAurUpdate<
              SystemAurUpdateDryRunObservation>);

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

template <typename Callback>
void expect_invalid_argument(Callback&& callback, std::string_view context) {
    bool rejected = false;
    try {
        std::forward<Callback>(callback)();
    } catch(const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, std::string(context) + " was not rejected");
}

const UnifiedPlanObservation& require_observation(
    const UnifiedPlanProjection& projection,
    std::string_view context) {
    const UnifiedPlanObservationResult& result =
        projection.observation_result();
    expect(
        result.is_valid(),
        std::string(context) + " observation is invalid");
    expect(
        result.observation() != nullptr,
        std::string(context) + " observation is missing");
    return *result.observation();
}

template <typename Detail>
std::size_t count_route_preflight_blockers(
    const UnifiedPlanObservation& observation) {
    return static_cast<std::size_t>(std::count_if(
        observation.blockers().begin(), observation.blockers().end(),
        [](const UnifiedPlanBlocker& blocker) {
            const auto* route =
                std::get_if<RoutePreflightUnifiedPlanBlocker>(
                    &blocker);
            return route != nullptr &&
                   std::holds_alternative<
                       UnifiedPlanBorrowedAuthorityReference<Detail>>(
                       route->detail);
        }));
}

bool has_phase(
    const UnifiedPlanObservation& observation,
    UnifiedPlanObservationPhase phase,
    std::optional<UnifiedPlanAuthorityOwner> owner = std::nullopt) {
    for(const UnifiedPlanPhaseReference& candidate : observation.phases()) {
        if(candidate.observation_phase == phase &&
           (!owner.has_value() || candidate.owner == owner.value())) {
            return true;
        }
    }
    return false;
}

template <typename Phase>
bool has_existing_phase(
    const UnifiedPlanObservation& observation,
    UnifiedPlanObservationPhase phase,
    Phase existing) {
    for(const UnifiedPlanPhaseReference& candidate : observation.phases()) {
        if(candidate.observation_phase != phase ||
           !candidate.existing_route_phase.has_value()) {
            continue;
        }
        const auto* typed = std::get_if<Phase>(
            &candidate.existing_route_phase.value());
        if(typed != nullptr && *typed == existing) return true;
    }
    return false;
}

RootPackageSearchCandidate repository_candidate(
    const std::string& repository, const std::string& package_name) {
    RootPackageCandidateValidationResult result =
        make_repository_root_package_candidate(
            repository, package_name, "1.0", "repository root");
    expect(result.is_valid(), "repository candidate fixture is invalid");
    return RootPackageSearchCandidate{*result.candidate(), {}};
}

RootPackageSearchCandidate aur_candidate(
    const std::string& package_name, const std::string& package_base) {
    RootPackageCandidateValidationResult result =
        make_aur_root_package_candidate(
            package_name, package_base, "2.0", "AUR root");
    expect(result.is_valid(), "AUR candidate fixture is invalid");
    return RootPackageSearchCandidate{*result.candidate(), {}};
}

RootPackageSelection require_selection(
    RootPackageSelectionExpressionResult result) {
    auto* selection = std::get_if<RootPackageSelection>(&result);
    expect(selection != nullptr, "root selection fixture failed");
    return std::move(*selection);
}

BuildPlan build_plan_fixture(
    std::string root_name = "suite-child",
    std::string package_base = "suite-base") {
    BuildPlan plan;
    const RootTargetIdentity root{0, root_name};
    plan.root_targets.push_back(root);
    plan.order.push_back(BuildPlanEntry{package_base, {root_name}});
    plan.package_targets.push_back(PlannedPackageTarget{
        root_name, package_base, {PackageRole::Root}, {root}});

    const ConsumerDependencyRequirement requirement(
        "virtual-runtime>=2", "virtual-runtime",
        DependencyVersionConstraint{
            DependencyVersionRelation::GreaterThanOrEqual, "2"});
    const ProvidedDependency provider =
        ProvidedDependency::from_repository_constraint_metadata(
            "extra", 1, "runtime-provider", "runtime-provider-base",
            ProviderConstraintMetadata{
                ProviderCapability(
                    "virtual-runtime=2", "virtual-runtime",
                    "2"),
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryExactPackage,
                    "2.1"),
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryProviderCapability,
                    "2")});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        root_name,
        package_base,
        "virtual-runtime>=2",
        PackageRole::RuntimeDependency,
        DependencyKind::Provided,
        std::nullopt,
        std::nullopt,
        provider,
        ProviderResolutionKind::UserSelected,
        DependencyRequirement{requirement},
        ResolvedDependencyCandidate{ProviderResolvedDependencyCandidate{
            provider,
            ObservedVersion::available(
                ObservedVersionSource::
                    RepositoryProviderCapability,
                "2")}},
        ConstraintEvaluation::satisfied()});
    plan.provided.push_back(BuildPlanProvidedDependency{
        "virtual-runtime", provider,
        ProviderResolutionKind::UserSelected});
    plan.configured_repository_order =
        std::vector<std::string>{"core", "extra", "multilib"};
    return plan;
}

ProductionSourceBuildWorkItem source_work_item(
    std::string package_base,
    std::string package_name,
    DesiredInstallReason reason = DesiredInstallReason::Explicit,
    ArtifactLifecycleIntent lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility,
    SourceBuildSourceKind source_kind =
        SourceBuildSourceKind::Repository,
    bool needed = false) {
    ProductionSourceBuildWorkItem work;
    work.request.package_name = package_name;
    work.request.checkout_name = package_base;
    work.request.needed = needed;
    work.required_targets.push_back(RequiredPackageArtifactTarget{
        package_base, package_name, reason});
    work.required_target_provenance =
        source_kind == SourceBuildSourceKind::Repository
            ? RequiredTargetProvenance::RepositoryExactPackageProjection
            : RequiredTargetProvenance::AurBuildPlanProjection;
    work.artifact_lifecycle_intent = lifecycle_intent;
    if(source_kind == SourceBuildSourceKind::Repository) {
        work.repository_identity =
            ResolvedRepositorySourceBuildIdentity{
                RepositoryPackagePresent{
                    "core", 0, package_name, package_base}};
        work.request.git_url = work.repository_identity->checkout().git_url();
    } else {
        work.request.git_url =
            "https://aur.archlinux.org/" + package_base + ".git";
        work.request.aur_review_identity = PackageBaseIdentity::make(
            PackageSourceIdentity::aur(
                SourceLocationIdentity::known_git_remote(
                    work.request.git_url)),
            package_base);
    }
    work.uses_system_update_baseline =
        source_kind == SourceBuildSourceKind::Repository;
    return work;
}

PreparedRootPackageInstall make_root_prepared(
    bool include_repository,
    bool include_aur,
    std::optional<std::vector<std::string>> repository_order) {
    RootPackageSearchSnapshot discovery;
    discovery.repository_order = std::move(repository_order);
    if(include_repository) {
        discovery.candidates.push_back(
            repository_candidate("core", "repo-root"));
    }
    if(include_aur) {
        discovery.candidates.push_back(
            aur_candidate("suite-child", "suite-base"));
    }

    std::string expression;
    for(std::size_t index = 0; index < discovery.candidates.size(); ++index) {
        if(!expression.empty()) expression += ' ';
        expression += std::to_string(index + 1);
    }
    const RootPackageSelection selection = require_selection(
        parse_root_package_selection(expression, discovery));
    const RootPackageRoutingProjectionResult routing =
        project_root_package_routing(selection);
    expect(routing.is_valid(), "root routing fixture is invalid");

    PreparedRootPackageInstall prepared;
    prepared.needed = true;
    prepared.discovery_snapshot.emplace(std::move(discovery));
    prepared.routing_projection.emplace(*routing.projection());
    for(const RepositoryRootPackageRouteTarget& target :
        prepared.routing_projection->repository_targets()) {
        prepared.exact_repository_targets.push_back(
            target.exact_package_target());
    }
    if(include_aur) {
        prepared.aur_build_plan.emplace(build_plan_fixture());
        prepared.source_invocation.emplace();
        prepared.source_invocation->work_items.push_back(
            source_work_item(
                "suite-base", "suite-child",
                DesiredInstallReason::Explicit,
                ArtifactLifecycleIntent::PackageBaseSet,
                SourceBuildSourceKind::Aur, prepared.needed));
        prepared.source_invocation->work_items.front()
            .configured_repository_order =
            prepared.aur_build_plan->configured_repository_order;
        prepared.source_invocation->work_items.front()
            .selected_repository_providers.push_back(
                prepared.aur_build_plan->provided.front().provider);
    }
    return prepared;
}

AurUpdatePlanEntry update_entry(
    AurUpdateClassification classification =
        AurUpdateClassification::UpdateAvailable,
    InstalledPackageReason install_reason =
        InstalledPackageReason::Explicit) {
    const AurVersionRelation relation =
        classification == AurUpdateClassification::UpdateAvailable
            ? AurVersionRelation::NewerThanInstalled
            : AurVersionRelation::SameAsInstalled;
    return AurUpdatePlanEntry{
        "suite-child",
        "1.0",
        install_reason,
        AurUpdateRemotePackage{
            "suite-child", "suite-base", "2.0", relation},
        classification};
}

AurUpdateQueryResult update_query(
    AurUpdateClassification classification =
        AurUpdateClassification::UpdateAvailable,
    InstalledPackageReason install_reason =
        InstalledPackageReason::Explicit) {
    AurUpdateQueryResult query;
    query.plan.entries.push_back(
        update_entry(classification, install_reason));
    return query;
}

AurUpdateExecutionPreflight executable_update_preflight(
    BuildPlan plan,
    InstalledPackageReason install_reason =
        InstalledPackageReason::Explicit,
    DevelRequiresCheckPolicy devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = 0;
    target.build_plan_root_index = 0;
    target.update = update_entry(
        AurUpdateClassification::UpdateAvailable, install_reason);
    target.status = AurUpdateExecutionTargetStatus::Executable;
    target.desired_install_reason =
        install_reason == InstalledPackageReason::Dependency
            ? DesiredInstallReason::Dependency
            : DesiredInstallReason::Explicit;
    return AurUpdateExecutionPreflight{
        {std::move(target)}, std::move(plan), devel_requires_check_policy};
}

AurUpdateExecutionPreflight no_op_update_preflight() {
    AurUpdateExecutionTarget target;
    target.update_plan_index = 0;
    target.update = update_entry(AurUpdateClassification::UpToDate);
    target.status = AurUpdateExecutionTargetStatus::Skipped;
    target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
    target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::UpToDate,
        target.update.installed_name,
        target.update.aur_package->package_base,
        std::nullopt,
        "fixture target is already up to date"});
    return AurUpdateExecutionPreflight{
        {std::move(target)}, std::nullopt, DevelRequiresCheckPolicy::SkipIndependentTarget};
}

RegisteredSourcePreferenceSnapshot registered_source(
    std::size_t index,
    std::string package_name,
    std::string package_base,
    SourceBuildSourceKind kind) {
    const std::string key =
        (kind == SourceBuildSourceKind::Aur ? "aur:" : "repository:") +
        package_base;
    RegisteredSourcePreferenceSnapshot snapshot{
        index,
        package_name,
        "/tmp/source-preference",
        SourceBuildEnvironment{},
        key,
        package_base,
        {},
        kind};
    snapshot.required_target_provenance =
        kind == SourceBuildSourceKind::Repository
            ? RequiredTargetProvenance::RepositoryExactPackageProjection
            : RequiredTargetProvenance::AurBuildPlanProjection;
    snapshot.artifact_lifecycle_intent =
        kind == SourceBuildSourceKind::Repository
            ? ArtifactLifecycleIntent::PackageBaseSet
            : ArtifactLifecycleIntent::SingularCompatibility;
    if(kind == SourceBuildSourceKind::Repository) {
        snapshot.repository_identity =
            ResolvedRepositorySourceBuildIdentity{
                RepositoryPackagePresent{
                    "core", 0, package_name, package_base}};
    }
    return snapshot;
}

RegisteredSourceUpgradeResult not_attempted_source_result(
    const RegisteredSourcePreferenceSnapshot& source) {
    RegisteredSourceUpgradeResult result;
    result.original_preference_index = source.original_preference_index;
    result.preference_package_name = source.preference_package_name;
    result.canonical_source_identity_key =
        source.canonical_source_identity_key;
    result.resolved_package_base = source.resolved_package_base;
    result.status = RegisteredSourceUpgradeStatus::NotAttempted;
    return result;
}

SystemSourceUpgradeIssue system_issue(
    SystemSourceUpgradeIssueKind kind,
    SystemSourceUpgradeIssueImpact impact,
    SystemSourceUpgradePhase phase) {
    SystemSourceUpgradeIssue issue;
    issue.kind = kind;
    issue.impact = impact;
    issue.phase = phase;
    return issue;
}

SystemAurUpdateDryRunRequest system_aur_auto_request(
    bool repository_needed = false) {
    std::vector<std::string> ordered_pacman_args{"-Syu"};
    if(repository_needed) ordered_pacman_args.push_back("--needed");
    std::optional<SystemAurUpdateDryRunRequest> request =
        SystemAurUpdateDryRunRequest::from_auto_candidate(
            AutoSystemUpdateRouteCandidate{
                CompatibleAutoSystemUpdatePacmanArguments{},
                std::move(ordered_pacman_args), repository_needed});
    expect(request.has_value(), "system/AUR Auto fixture was rejected");
    return std::move(request.value());
}

SystemAurUpdateDryRunRequest system_aur_repo_only_request() {
    return SystemAurUpdateDryRunRequest(
        RepoOnlySystemUpdateRouteCandidate{{"-Syu"}, false});
}

AurUpdateSourceBuildObservation ready_source_build_observation(
    const AurUpdateExecutionPreflight& preflight) {
    expect(
        preflight.build_plan.has_value() &&
            preflight.targets.size() == 1 &&
            preflight.devel_requires_check_policy.has_value() &&
            preflight.build_plan->root_targets.size() == 1 &&
            preflight.build_plan->order.size() == 1,
        "system/AUR source observation fixture is malformed");
    const BuildPlan& plan = *preflight.build_plan;
    const AurUpdateExecutionTarget& update_target =
        preflight.targets.front();
    const RootTargetIdentity& root = plan.root_targets.front();
    const BuildPlanEntry& build_unit = plan.order.front();
    expect(
        update_target.desired_install_reason.has_value() &&
            build_unit.package_names.size() == 1,
        "system/AUR source observation fixture lacks authority");

    AurUpdateRequiredTargetAttribution required_target{
        RequiredPackageArtifactTarget{
            build_unit.package_base, build_unit.package_names.front(),
            *update_target.desired_install_reason},
        {update_target.update_plan_index},
        {root},
        {PackageRole::Root}};
    AurUpdateProjectedBuildUnit projected_unit{
        0,
        build_unit.package_base,
        {required_target},
        {update_target.update_plan_index},
        {root}};
    AurUpdatePreparedWorkItemAttribution work_attribution{
        0,
        0,
        build_unit.package_names.front(),
        build_unit.package_base,
        {required_target},
        {update_target.update_plan_index},
        {root}};

    ProductionSourceBuildPreparationObservation production_preflight;
    ProductionSourceBuildWorkItemObservation observed_work_item =
        make_production_source_build_work_item_observation(
            source_work_item(
                build_unit.package_base,
                build_unit.package_names.front(),
                *update_target.desired_install_reason,
                ArtifactLifecycleIntent::PackageBaseSet,
                SourceBuildSourceKind::Aur));
    observed_work_item.configured_repository_order =
        plan.configured_repository_order;
    for(const BuildPlanProvidedDependency& provided : plan.provided) {
        if(provided.resolution != ProviderResolutionKind::UserSelected ||
           !std::holds_alternative<RepositoryProviderOrigin>(
               provided.provider.origin)) {
            continue;
        }
        observed_work_item.selected_repository_providers.push_back(
            provided.provider);
        production_preflight.selected_repository_providers.push_back(
            provided.provider);
    }
    production_preflight.work_items.push_back(
        std::move(observed_work_item));

    AurUpdateSourceBuildObservation observation;
    observation.affected_update_targets = preflight.targets;
    observation.affected_roots.push_back(root);
    observation.build_unit_selection.entries.push_back(
        AurUpdateBuildUnitSelectionEntry{
            0,
            build_unit.package_base,
            build_unit.package_names,
            AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution,
            0,
            std::nullopt});
    observation.projected_build_units.push_back(
        std::move(projected_unit));
    observation.work_item_attributions.push_back(
        std::move(work_attribution));
    observation.production_preflight.emplace(
        std::move(production_preflight));
    observation.devel_requires_check_policy =
        *preflight.devel_requires_check_policy;
    expect(
        observation.issues.empty() &&
            observation.affected_update_targets.size() == 1 &&
            observation.affected_roots.size() == 1 &&
            observation.build_unit_selection.entries.size() == 1 &&
            observation.projected_build_units.size() == 1 &&
            observation.work_item_attributions.size() == 1 &&
            observation.production_preflight.has_value() &&
            observation.production_preflight->work_items.size() == 1,
        "system/AUR source observation fixture lost typed authority");
    return observation;
}

FilteredAurUpdateObservation ready_filtered_aur_observation() {
    FilteredAurUpdateObservation filtered;
    filtered.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    filtered.query_result = update_query();
    filtered.preflight = executable_update_preflight(
        build_plan_fixture(), InstalledPackageReason::Explicit,
        DevelRequiresCheckPolicy::SkipIndependentTarget);
    filtered.source_build_observation.emplace(
        ready_source_build_observation(filtered.preflight));
    expect(
        filtered.issues.empty() &&
            filtered.query_result.plan.entries.size() == 1 &&
            filtered.preflight.targets.size() == 1 &&
            filtered.preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Executable &&
            filtered.preflight.build_plan.has_value() &&
            filtered.source_build_observation.has_value() &&
            filtered.source_build_observation->issues.empty() &&
            filtered.source_build_observation->production_preflight
                .has_value(),
        "filtered AUR Ready fixture lost typed authority");
    return filtered;
}

FilteredAurUpdateObservation
multi_unit_partial_provider_filtered_aur_observation() {
    constexpr std::string_view provider_child = "provider-child";
    constexpr std::string_view provider_base = "provider-base";
    constexpr std::string_view plain_child = "plain-child";
    constexpr std::string_view plain_base = "plain-base";

    BuildPlan plan = build_plan_fixture(
        std::string(provider_child), std::string(provider_base));
    const RootTargetIdentity plain_root{1, std::string(plain_child)};
    plan.root_targets.push_back(plain_root);
    plan.order.push_back(BuildPlanEntry{
        std::string(plain_base), {std::string(plain_child)}});
    plan.package_targets.push_back(PlannedPackageTarget{
        std::string(plain_child), std::string(plain_base), {PackageRole::Root}, {plain_root}});

    const auto update = [](
                            std::string_view package_name,
                            std::string_view package_base) {
        return AurUpdatePlanEntry{
            std::string(package_name),
            "1.0",
            InstalledPackageReason::Explicit,
            AurUpdateRemotePackage{
                std::string(package_name), std::string(package_base),
                "2.0", AurVersionRelation::NewerThanInstalled},
            AurUpdateClassification::UpdateAvailable};
    };

    FilteredAurUpdateObservation filtered;
    filtered.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    filtered.query_result.plan.entries = {
        update(provider_child, provider_base),
        update(plain_child, plain_base)};
    for(std::size_t index = 0;
        index < filtered.query_result.plan.entries.size(); ++index) {
        AurUpdateExecutionTarget target;
        target.update_plan_index = index;
        target.build_plan_root_index = index;
        target.update = filtered.query_result.plan.entries[index];
        target.status = AurUpdateExecutionTargetStatus::Executable;
        target.desired_install_reason = DesiredInstallReason::Explicit;
        filtered.preflight.targets.push_back(std::move(target));
    }
    filtered.preflight.build_plan.emplace(std::move(plan));
    filtered.preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    const BuildPlan& retained_plan = *filtered.preflight.build_plan;
    const ProvidedDependency& repository_provider =
        retained_plan.provided.front().provider;

    AurUpdateSourceBuildObservation source;
    source.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    source.affected_update_targets = filtered.preflight.targets;
    source.affected_roots = retained_plan.root_targets;
    source.production_preflight.emplace();
    source.production_preflight->selected_repository_providers.push_back(
        repository_provider);
    for(std::size_t index = 0; index < retained_plan.order.size(); ++index) {
        const BuildPlanEntry& unit = retained_plan.order[index];
        const RootTargetIdentity& root = retained_plan.root_targets[index];
        const std::string& package_name = unit.package_names.front();
        const std::string git_url =
            "https://aur.archlinux.org/" + unit.package_base + ".git";
        const RequiredPackageArtifactTarget required_target{
            unit.package_base, package_name,
            DesiredInstallReason::Explicit};
        const AurUpdateRequiredTargetAttribution child_attribution{
            required_target,
            {index},
            {root},
            {PackageRole::Root}};

        source.build_unit_selection.entries.push_back(
            AurUpdateBuildUnitSelectionEntry{
                index,
                unit.package_base,
                unit.package_names,
                AurUpdateBuildUnitSelectionStatus::
                    SelectedForAurExecution,
                index,
                std::nullopt});
        source.projected_build_units.push_back(
            AurUpdateProjectedBuildUnit{
                index,
                unit.package_base,
                {child_attribution},
                {index},
                {root}});
        source.work_item_attributions.push_back(
            AurUpdatePreparedWorkItemAttribution{
                index,
                index,
                package_name,
                unit.package_base,
                {child_attribution},
                {index},
                {root}});

        ProductionSourceBuildWorkItemObservation work_item{
            SourceBuildRequestObservation{
                package_name,
                unit.package_base,
                git_url,
                SourceBuildEnvironment{},
                SourceEnvironmentEmptyValuePolicy::Omit,
                std::nullopt,
                std::nullopt,
                false,
                false,
                PackageBaseIdentity::make(
                    PackageSourceIdentity::aur(
                        SourceLocationIdentity::known_git_remote(
                            git_url)),
                    unit.package_base),
                ReviewedSourceFatalStateObservationStatus::Completed},
            {required_target},
            {},
            {},
            {},
            RequiredTargetProvenance::AurBuildPlanProjection,
            ArtifactLifecycleIntent::PackageBaseSet,
            std::nullopt,
            false,
            retained_plan.configured_repository_order};
        if(index == 0) {
            work_item.selected_repository_providers.push_back(
                repository_provider);
        }
        source.production_preflight->work_items.push_back(
            std::move(work_item));
    }
    expect(
        source.production_preflight->work_items.size() == 2 &&
            source.production_preflight->work_items[0]
                    .selected_repository_providers ==
                std::vector<ProvidedDependency>{repository_provider} &&
            source.production_preflight->work_items[1]
                .selected_repository_providers.empty(),
        "multi-unit source fixture lost PackageBase-local provider scope");
    filtered.source_build_observation.emplace(std::move(source));
    return filtered;
}

FilteredAurUpdateObservation no_op_filtered_aur_observation() {
    FilteredAurUpdateObservation filtered;
    filtered.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    filtered.query_result = update_query(AurUpdateClassification::UpToDate);
    filtered.preflight = no_op_update_preflight();
    filtered.source_build_observation.emplace();
    filtered.source_build_observation->devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    expect(
        filtered.issues.empty() &&
            filtered.preflight.targets.size() == 1 &&
            filtered.preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Skipped &&
            !filtered.preflight.build_plan.has_value() &&
            filtered.source_build_observation.has_value() &&
            filtered.source_build_observation->issues.empty() &&
            !filtered.source_build_observation->production_preflight
                 .has_value(),
        "filtered AUR NoOp fixture retained execution authority");
    return filtered;
}

FilteredAurUpdateObservation blocked_filtered_aur_observation() {
    FilteredAurUpdateObservation filtered;
    filtered.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    filtered.query_result = update_query();
    AurUpdateExecutionTarget target;
    target.update_plan_index = 0;
    target.update = filtered.query_result.plan.entries.front();
    target.status = AurUpdateExecutionTargetStatus::Incomplete;
    target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::AmbiguousProvider,
        "suite-child", "suite-base", "virtual-runtime>=2",
        "fixture current-state provider ambiguity"});
    filtered.preflight.targets.push_back(std::move(target));
    filtered.preflight.devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    filtered.source_build_observation.emplace();
    filtered.source_build_observation->devel_requires_check_policy =
        DevelRequiresCheckPolicy::SkipIndependentTarget;
    AurUpdatePreparationIssue issue;
    issue.reason = AurUpdatePreparationReason::BlockingPreflight;
    issue.diagnostic = "fixture current-state preparation blocker";
    filtered.source_build_observation->issues.push_back(std::move(issue));
    expect(
        filtered.preflight.targets.size() == 1 &&
            filtered.preflight.targets.front().status ==
                AurUpdateExecutionTargetStatus::Incomplete &&
            filtered.preflight.targets.front().issues.size() == 1 &&
            filtered.preflight.targets.front().issues.front().reason ==
                AurUpdateExecutionReason::AmbiguousProvider &&
            filtered.source_build_observation.has_value() &&
            filtered.source_build_observation->issues.size() == 1 &&
            !filtered.source_build_observation->production_preflight
                 .has_value(),
        "filtered AUR Blocked fixture lost typed blockers");
    return filtered;
}

SystemAurUpdateDryRunObservation system_aur_auto_observation(
    FilteredAurUpdateObservation filtered,
    bool repository_needed = false) {
    ForeignPackageInventory inventory;
    PacmanRepositoryConfiguration repository_configuration;
    if(filtered.execution_preflight().build_plan.has_value() &&
       filtered.execution_preflight()
           .build_plan->configured_repository_order.has_value()) {
        repository_configuration.repository_names =
            *filtered.execution_preflight()
                 .build_plan->configured_repository_order;
    }
    for(const AurUpdatePlanEntry& entry :
        filtered.original_query_result().plan.entries) {
        inventory.push_back(InstalledPackageMetadata{
            entry.installed_name, entry.installed_version,
            entry.install_reason});
    }
    SystemAurUpdateDryRunObservation observation{
        system_aur_auto_request(repository_needed),
        SystemAurUpdateDryRunAurObservationBasis::CurrentInstalledState,
        SystemAurUpdateDryRunActualAuthorityRefresh::AfterRepositorySuccess,
        NoExplicitSourceSatisfaction{},
        SavedSourcePreferencePolicy::Ignore,
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        std::move(repository_configuration),
        std::move(inventory),
        std::move(filtered),
        {}};
    expect(
        observation.request.mode() == SystemAurUpdateDryRunMode::Auto &&
            !observation.request.ordered_pacman_args().empty() &&
            observation.aur_observation_basis ==
                std::optional<SystemAurUpdateDryRunAurObservationBasis>{
                    SystemAurUpdateDryRunAurObservationBasis::
                        CurrentInstalledState} &&
            observation.actual_authority_refresh ==
                std::optional<
                    SystemAurUpdateDryRunActualAuthorityRefresh>{
                    SystemAurUpdateDryRunActualAuthorityRefresh::
                        AfterRepositorySuccess} &&
            observation.explicit_source_satisfaction.has_value() &&
            observation.saved_source_preference_policy ==
                std::optional<SavedSourcePreferencePolicy>{
                    SavedSourcePreferencePolicy::Ignore} &&
            observation.devel_requires_check_policy ==
                std::optional<DevelRequiresCheckPolicy>{
                    DevelRequiresCheckPolicy::
                        SkipIndependentTarget} &&
            observation.repository_configuration.has_value() &&
            observation.aur_observation.has_value() &&
            observation.issues.empty(),
        "system/AUR Auto fixture lost freshness or source policy");
    return observation;
}

const RepositoryPackageTransactionIntent*
repository_transaction_intent(
    const UnifiedPlanTransactionIntent& transaction) {
    return std::get_if<RepositoryPackageTransactionIntent>(&transaction);
}

bool has_repository_system_upgrade_intent(
    const UnifiedPlanObservation& observation) {
    return std::any_of(
        observation.transaction_intents().begin(),
        observation.transaction_intents().end(),
        [](const UnifiedPlanTransactionIntent& transaction) {
            const RepositoryPackageTransactionIntent* repository =
                repository_transaction_intent(transaction);
            return repository != nullptr &&
                   repository->stage ==
                       UnifiedPlanTransactionIntentStage::
                           RepositorySystemUpgrade &&
                   repository->targets.size() == 1 &&
                   std::holds_alternative<RepositorySystemUpgradeIntent>(
                       repository->targets.front());
        });
}

UnifiedPlanTransactionIntentStage transaction_stage(
    const UnifiedPlanTransactionIntent& transaction) {
    return std::visit(
        [](const auto& typed) { return typed.stage; }, transaction);
}

void test_projection_lifetime_and_root_authorities() {
    const PreparedRootPackageInstall prepared = make_root_prepared(
        true, true,
        std::vector<std::string>{"core", "extra", "multilib"});
    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_root_package_unified_plan(
            RootPackageUnifiedPlanProjectionInput{
                std::cref(prepared)});
    const UnifiedPlanObservation& observation =
        require_observation(*projection, "root");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Ready,
        "root projection was not Ready");
    expect(observation.roots().size() == 2, "root identities were lost");
    expect(
        std::holds_alternative<RepositoryRootPackageIdentity>(
            observation.roots()[0].source_identity()) &&
            std::get<AurRootPackageIdentity>(
                observation.roots()[1].source_identity()) ==
                AurRootPackageIdentity{
                    "suite-child", "suite-base"},
        "repository/AUR root identity was flattened");
    expect(
        observation.dependency_authorities().front().build_plan() ==
            &prepared.aur_build_plan.value(),
        "BuildPlan was copied instead of borrowed");
    expect(
        observation.configured_repository_order() != nullptr &&
            &observation.configured_repository_order()
                    ->configured_order() ==
                &prepared.discovery_snapshot->repository_order
                     .value() &&
            observation.configured_repository_order()
                    ->configured_order() ==
                std::vector<std::string>{
                    "core", "extra", "multilib"},
        "configured repository order was reordered or copied");
    expect(
        observation.route_preflight_authorities().size() == 1 &&
            &std::get<UnifiedPlanBorrowedAuthorityReference<
                    RootPackageRoutingProjection>>(
                 observation.route_preflight_authorities().front())
                    .get() ==
                &prepared.routing_projection.value(),
        "root route authority was copied or omitted");

    const RequiredPackageArtifactTarget artifact =
        observation.required_artifacts().front().target();
    expect(
        artifact.package_base == "suite-base" &&
            artifact.package_name == "suite-child" &&
            artifact.desired_reason ==
                DesiredInstallReason::Explicit,
        "bundle-owned artifact target identity was not retained");

    bool selected_provider_borrowed = false;
    for(const UnifiedPlanTransactionIntent& transaction :
        observation.transaction_intents()) {
        const auto* repository =
            std::get_if<RepositoryPackageTransactionIntent>(&transaction);
        if(repository == nullptr) continue;
        for(const RepositoryInstallIntentTarget& target :
            repository->targets) {
            const auto* provider =
                std::get_if<RepositoryProviderInstallIntent>(&target);
            if(provider != nullptr &&
               &provider->provider.get() ==
                   &prepared.aur_build_plan->provided.front().provider) {
                selected_provider_borrowed = true;
            }
        }
    }
    expect(
        selected_provider_borrowed,
        "typed selected provider identity was reconstructed or omitted");

    // observation_result() exposes only a const borrow. The compile-time
    // assertions above make both result and observation copy escape invalid;
    // the projection therefore remains the sole owner of this internal target.
}

void test_root_repository_order_known_and_unknown() {
    const PreparedRootPackageInstall repository_only = make_root_prepared(
        true, false, std::vector<std::string>{"core", "extra"});
    const std::unique_ptr<UnifiedPlanProjection> repository_projection =
        project_root_package_unified_plan(
            RootPackageUnifiedPlanProjectionInput{
                std::cref(repository_only)});
    const UnifiedPlanObservation& repository_observation =
        require_observation(
            *repository_projection, "repository-only root");
    expect(
        repository_observation.configured_repository_order() != nullptr &&
            repository_observation.configured_repository_order()
                    ->configured_order() ==
                std::vector<std::string>{"core", "extra"},
        "queried repository order was not retained");
    expect(
        has_phase(
            repository_observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Libalpm) &&
            has_phase(
                repository_observation,
                UnifiedPlanObservationPhase::
                    RepositoryTransaction) &&
            !has_phase(
                repository_observation,
                UnifiedPlanObservationPhase::SourceRetrieval) &&
            !has_phase(
                repository_observation,
                UnifiedPlanObservationPhase::SourceBuild),
        "repository-only route fabricated source phases");

    const PreparedRootPackageInstall aur_only =
        make_root_prepared(false, true, std::nullopt);
    const std::unique_ptr<UnifiedPlanProjection> aur_projection =
        project_root_package_unified_plan(
            RootPackageUnifiedPlanProjectionInput{
                std::cref(aur_only)});
    const UnifiedPlanObservation& aur_observation =
        require_observation(*aur_projection, "AUR-only root");
    expect(
        aur_observation.configured_repository_order() == nullptr,
        "unqueried repository authority was flattened to known empty");
    expect(
        has_phase(
            aur_observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc) &&
            has_phase(
                aur_observation,
                UnifiedPlanObservationPhase::SourceRetrieval) &&
            has_phase(
                aur_observation,
                UnifiedPlanObservationPhase::SourceBuild),
        "AUR route lost its actual source phases");
}

void test_root_and_update_correlation_fail_closed() {
    PreparedRootPackageInstall missing_plan =
        make_root_prepared(false, true, std::nullopt);
    missing_plan.aur_build_plan.reset();
    expect_invalid_argument(
        [&missing_plan] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(missing_plan)});
        },
        "AUR root without BuildPlan");

    PreparedRootPackageInstall mismatched_plan =
        make_root_prepared(false, true, std::nullopt);
    mismatched_plan.aur_build_plan->root_targets.front().requested_name =
        "other-invocation";
    expect_invalid_argument(
        [&mismatched_plan] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(mismatched_plan)});
        },
        "mismatched root invocation");

    PreparedRootPackageInstall mismatched_source_work =
        make_root_prepared(false, true, std::nullopt);
    mismatched_source_work.source_invocation->work_items.front()
        .required_targets.front()
        .package_name = "other-work-item";
    expect_invalid_argument(
        [&mismatched_source_work] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(mismatched_source_work)});
        },
        "mismatched root source work");

    PreparedRootPackageInstall mismatched_needed =
        make_root_prepared(false, true, std::nullopt);
    mismatched_needed.source_invocation->work_items.front().request.needed =
        false;
    expect_invalid_argument(
        [&mismatched_needed] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(mismatched_needed)});
        },
        "root source work with a different needed policy");

    AurUpdateQueryResult query = update_query();
    AurUpdateExecutionPreflight mismatched_preflight =
        executable_update_preflight(build_plan_fixture());
    mismatched_preflight.targets.front().update.installed_version = "0.9";
    expect_invalid_argument(
        [&query, &mismatched_preflight] {
            (void)project_aur_update_unified_plan(
                AurUpdateUnifiedPlanProjectionInput{
                    std::cref(query),
                    std::cref(mismatched_preflight),
                    false});
        },
        "mismatched AUR query/preflight");

    AurUpdateExecutionPreflight missing_preflight_plan;
    AurUpdateExecutionTarget executable;
    executable.update_plan_index = 0;
    executable.build_plan_root_index = 0;
    executable.update = update_entry();
    executable.status = AurUpdateExecutionTargetStatus::Executable;
    missing_preflight_plan.targets.push_back(std::move(executable));
    expect_invalid_argument(
        [&query, &missing_preflight_plan] {
            (void)project_aur_update_unified_plan(
                AurUpdateUnifiedPlanProjectionInput{
                    std::cref(query),
                    std::cref(missing_preflight_plan),
                    false});
        },
        "executable AUR update without BuildPlan");
}

void test_aur_update_install_reason_parity() {
    const AurUpdateQueryResult query = update_query(
        AurUpdateClassification::UpdateAvailable,
        InstalledPackageReason::Dependency);
    const AurUpdateExecutionPreflight preflight =
        executable_update_preflight(
            build_plan_fixture(),
            InstalledPackageReason::Dependency);
    const std::unique_ptr<UnifiedPlanProjection> standalone_projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(query), std::cref(preflight), false});
    const UnifiedPlanObservation& standalone = require_observation(
        *standalone_projection, "dependency-installed AUR update");
    expect(
        standalone.status() == UnifiedPlanObservationStatus::Ready &&
            standalone.required_artifacts().size() == 1 &&
            standalone.required_artifacts().front().target().desired_reason ==
                DesiredInstallReason::Dependency,
        "standalone AUR update promoted dependency install reason");
    const auto* standalone_install = std::get_if<
        SourceBuiltArtifactInstallBoundaryIntent>(
        &standalone.transaction_intents().back());
    expect(
        standalone_install != nullptr &&
            standalone_install->targets.size() == 1 &&
            std::holds_alternative<
                SourceDependencyArtifactInstallIntent>(
                standalone_install->targets.front()),
        "standalone AUR update promoted dependency install intent");

    SystemSourceUpgradePreparedSnapshot system_snapshot;
    const std::vector<SystemSourceUpgradeIssue> system_issues;
    const std::vector<ProductionSourceBuildWorkItem> system_work;
    const SystemSourceUpgradeProjectionAuthority system_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            system_snapshot, nullptr, system_issues, system_work);
    const UpgradeAllOperationPreparedSnapshot aggregate_snapshot{
        system_snapshot, {}, {}};
    const UpgradeAllOperationProjectionAuthority aggregate_authority =
        UnifiedPlanProjectionTestAccess::make_upgrade_all(
            aggregate_snapshot, system_authority);
    const std::vector<UpgradeAllOperationIssue> aggregate_issues;
    const std::unique_ptr<UnifiedPlanProjection> upgrade_projection =
        project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(aggregate_authority),
                std::cref(query), std::cref(preflight),
                std::cref(aggregate_issues)});
    const UnifiedPlanObservation& upgrade = require_observation(
        *upgrade_projection,
        "dependency-installed upgrade-all AUR update");
    expect(
        upgrade.status() == UnifiedPlanObservationStatus::Ready &&
            upgrade.required_artifacts().size() == 1 &&
            upgrade.required_artifacts().front().target().desired_reason ==
                DesiredInstallReason::Dependency,
        "upgrade-all AUR update promoted dependency install reason");
    const auto* upgrade_install =
        std::get_if<SourceBuiltArtifactInstallBoundaryIntent>(
            &upgrade.transaction_intents().back());
    expect(
        upgrade_install != nullptr &&
            upgrade_install->targets.size() == 1 &&
            std::holds_alternative<
                SourceDependencyArtifactInstallIntent>(
                upgrade_install->targets.front()),
        "upgrade-all AUR update promoted dependency install intent");
}

void test_build_plan_partial_failure_remains_typed() {
    PreparedRootPackageInstall prepared =
        make_root_prepared(false, true, std::nullopt);
    BuildPlan& plan = prepared.aur_build_plan.value();
    plan.incomplete_provider_candidate_sets.push_back(
        IncompleteProviderCandidateSet{
            "partial-provider",
            {ProvidedDependency::from_repository(
                "core", 0, "observed-provider")},
            ObservedVersionUnknownReason::PartialSourceFailure});
    plan.dependency_edges.front().constraint_evaluation =
        ConstraintEvaluation::unsatisfied();
    const std::string shared_dependency =
        plan.dependency_edges.front().dependency_spec;
    const std::optional<DependencyRequirement> shared_requirement =
        plan.dependency_edges.front().requirement;
    const ProvidedDependency similar_provider =
        ProvidedDependency::from_repository_constraint_metadata(
            "core", 0, "similar-runtime-provider",
            "similar-runtime-provider-base",
            ProviderConstraintMetadata{
                ProviderCapability(
                    "virtual-runtime=1", "virtual-runtime",
                    "1"),
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryExactPackage,
                    "1.1"),
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryProviderCapability,
                    "1")});
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        plan.root_targets.front().requested_name,
        plan.order.front().package_base,
        shared_dependency,
        PackageRole::BuildDependency,
        DependencyKind::Provided,
        "similar-runtime-provider",
        std::nullopt,
        similar_provider,
        ProviderResolutionKind::Unique,
        shared_requirement,
        ResolvedDependencyCandidate{ProviderResolvedDependencyCandidate{
            similar_provider,
            ObservedVersion::available(
                ObservedVersionSource::
                    RepositoryProviderCapability,
                "1")}},
        ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::
                CandidateVersionUnavailable)});

    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_root_package_unified_plan(
            RootPackageUnifiedPlanProjectionInput{
                std::cref(prepared)});
    const UnifiedPlanObservation& observation =
        require_observation(*projection, "partial BuildPlan failure");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked &&
            observation.transaction_intents().empty(),
        "partial BuildPlan failure exposed mutation intent");
    bool partial_failure_borrowed = false;
    std::vector<const BuildPlanDependencyEdge*> constraint_failure_edges;
    for(const UnifiedPlanBlocker& blocker : observation.blockers()) {
        if(const auto* constraint =
               std::get_if<ConstraintFailureUnifiedPlanBlocker>(
                   &blocker);
           constraint != nullptr) {
            constraint_failure_edges.push_back(&constraint->detail.get());
        }
        const auto* source =
            std::get_if<SourceFailureUnifiedPlanBlocker>(&blocker);
        if(source == nullptr) continue;
        const auto* partial = std::get_if<
            UnifiedPlanBorrowedAuthorityReference<
                IncompleteProviderCandidateSet>>(&source->detail);
        if(partial != nullptr &&
           &partial->get() ==
               &plan.incomplete_provider_candidate_sets.front() &&
           partial->get().reason ==
               ObservedVersionUnknownReason::PartialSourceFailure) {
            partial_failure_borrowed = true;
        }
    }
    expect(
        partial_failure_borrowed &&
            constraint_failure_edges.size() == 2 &&
            constraint_failure_edges[0] ==
                &plan.dependency_edges[0] &&
            constraint_failure_edges[1] ==
                &plan.dependency_edges[1] &&
            constraint_failure_edges[0]->dependency_spec ==
                constraint_failure_edges[1]->dependency_spec &&
            constraint_failure_edges[0]
                ->resolved_provider.has_value() &&
            constraint_failure_edges[0]
                    ->resolved_provider->package_name ==
                "runtime-provider" &&
            constraint_failure_edges[0]
                ->constraint_evaluation.has_value() &&
            &constraint_failure_edges[0]
                    ->constraint_evaluation.value() ==
                &plan.dependency_edges[0]
                     .constraint_evaluation.value() &&
            constraint_failure_edges[0]
                    ->constraint_evaluation->satisfaction() ==
                ConstraintSatisfaction::Unsatisfied &&
            constraint_failure_edges[1]
                ->resolved_provider.has_value() &&
            constraint_failure_edges[1]
                    ->resolved_provider->package_name ==
                "similar-runtime-provider" &&
            constraint_failure_edges[1]
                ->constraint_evaluation.has_value() &&
            &constraint_failure_edges[1]
                    ->constraint_evaluation.value() ==
                &plan.dependency_edges[1]
                     .constraint_evaluation.value() &&
            constraint_failure_edges[1]
                    ->constraint_evaluation->unknown_reason() !=
                nullptr &&
            *constraint_failure_edges[1]
                    ->constraint_evaluation->unknown_reason() ==
                ObservedVersionUnknownReason::
                    CandidateVersionUnavailable,
        "typed partial-source/constraint authority was flattened");
}

void test_local_and_no_op_phases() {
    const LocalSourceRoot source_root = open_local_source_root(
        "tests/fixtures/pkgbuild-export");
    const LocalPackageMetadataParseResult* parse_result =
        source_root.metadata().parse_result();
    expect(
        parse_result != nullptr && parse_result->is_success(),
        "local metadata fixture is unavailable");
    const LocalBuildPlan local_plan = resolve_local_build_plan(
        *parse_result->metadata(), "x86_64");
    const SourceBuildEnvironment source_environment;
    const LocalSourceBuildProjectionAuthority local_authority =
        UnifiedPlanProjectionTestAccess::make_local_source(
            source_root, local_plan, source_environment);
    const std::unique_ptr<UnifiedPlanProjection> local_projection =
        project_local_source_unified_plan(
            LocalSourceUnifiedPlanProjectionInput{
                std::cref(local_authority), false});
    const UnifiedPlanObservation& local_observation =
        require_observation(*local_projection, "local source");
    expect(
        local_observation.status() == UnifiedPlanObservationStatus::Ready &&
            local_observation.dependency_authorities()
                    .front()
                    .local_build_plan() == &local_plan,
        "LocalBuildPlan was copied or omitted");
    expect(
        local_observation.route_preflight_authorities().size() == 1 &&
            &std::get<UnifiedPlanBorrowedAuthorityReference<
                    LocalSourceBuildProjectionAuthority>>(
                 local_observation
                     .route_preflight_authorities()
                     .front())
                    .get() == &local_authority &&
            &local_authority.accepted_metadata() ==
                &std::get<UnifiedPlanBorrowedAuthorityReference<
                     LocalPackageMetadata>>(
                     local_observation.root_metadata()
                         .front())
                     .get(),
        "local prepared invocation authority was copied or omitted");
    expect(
        has_phase(
            local_observation,
            UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::Makepkg) &&
            !has_phase(
                local_observation,
                UnifiedPlanObservationPhase::MetadataDiscovery,
                UnifiedPlanAuthorityOwner::AurRpc),
        "local route fabricated AUR metadata discovery");

    const LocalSourceRoot stale_source_root = open_local_source_root(
        "tests/fixtures/pkgbuild-export", true);
    expect(
        stale_source_root.metadata().state() ==
            LocalSourceMetadataState::KnownStale,
        "local one-off environment fixture did not require evaluation");
    const std::unique_ptr<UnifiedPlanProjection> metadata_blocked_projection =
        project_local_source_unified_plan(
            LocalSourceUnifiedPlanProjectionInput{
                LocalSourceMetadataEvaluationProjectionInput{
                    std::cref(stale_source_root)},
                false});
    const UnifiedPlanObservation& metadata_blocked = require_observation(
        *metadata_blocked_projection,
        "local metadata evaluation-required");
    expect(
        metadata_blocked.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            metadata_blocked.transaction_intents().empty() &&
            metadata_blocked.blockers().size() == 1 &&
            std::holds_alternative<
                LocalSourceMetadataEvaluationUnifiedPlanBlocker>(
                metadata_blocked.blockers().front()) &&
            &std::get<
                 LocalSourceMetadataEvaluationUnifiedPlanBlocker>(
                 metadata_blocked.blockers().front())
                    .detail.get() ==
                &stale_source_root.metadata(),
        "local metadata evaluation requirement was inferred as Ready or copied");

    const AurUpdateQueryResult query = update_query(
        AurUpdateClassification::UpToDate);
    const AurUpdateExecutionPreflight preflight = no_op_update_preflight();
    const std::unique_ptr<UnifiedPlanProjection> no_op_projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(query), std::cref(preflight),
                false});
    const UnifiedPlanObservation& no_op =
        require_observation(*no_op_projection, "AUR NoOp");
    expect(
        no_op.status() == UnifiedPlanObservationStatus::NoOp &&
            no_op.transaction_intents().empty(),
        "NoOp AUR update exposed mutation intent");
    expect(
        !has_phase(
            no_op,
            UnifiedPlanObservationPhase::RepositoryTransaction) &&
            !has_phase(
                no_op,
                UnifiedPlanObservationPhase::SourceBuild) &&
            !has_phase(
                no_op,
                UnifiedPlanObservationPhase::
                    SourceArtifactInstall),
        "NoOp route fabricated mutation phases");
}

void test_aur_update_source_preparation_blocker() {
    const AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight preflight =
        executable_update_preflight(build_plan_fixture());
    AurUpdateSourceBuildPreparation source_preparation;
    source_preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    AurUpdatePreparationIssue issue;
    issue.reason = AurUpdatePreparationReason::SourcePreferenceUnavailable;
    issue.package_name = "suite-child";
    issue.package_base = "suite-base";
    issue.diagnostic = "fixture strict source preference failure";
    source_preparation.issues.push_back(std::move(issue));

    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(query), std::cref(preflight), false,
                std::cref(source_preparation)});
    const UnifiedPlanObservation& observation = require_observation(
        *projection, "AUR source preparation blocker");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked &&
            observation.transaction_intents().empty(),
        "AUR source preparation failure was flattened to Ready");
    const auto* blocker = std::get_if<RoutePreflightUnifiedPlanBlocker>(
        &observation.blockers().back());
    expect(
        blocker != nullptr &&
            std::holds_alternative<
                UnifiedPlanBorrowedAuthorityReference<
                    AurUpdatePreparationIssue>>(
                blocker->detail) &&
            &std::get<UnifiedPlanBorrowedAuthorityReference<
                    AurUpdatePreparationIssue>>(blocker->detail)
                    .get() == &source_preparation.issues.front(),
        "AUR source preparation blocker lost its production authority");
}

AurUpdateSourceBuildPreparation blocking_preflight_preparation_fixture(
    const AurUpdateExecutionPreflight& preflight) {
    AurUpdateSourceBuildPreparation preparation;
    preparation.devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    for(const AurUpdateExecutionTarget& target : preflight.targets) {
        for(const AurUpdateExecutionIssue& preflight_issue : target.issues) {
            // Mirror retain_preflight_blockers(): the wrapper keeps an owned
            // copy of the original typed issue and only target-index
            // attribution.
            AurUpdatePreparationIssue wrapper;
            wrapper.reason = AurUpdatePreparationReason::BlockingPreflight;
            wrapper.affected_update_plan_indices = {
                target.update_plan_index};
            wrapper.package_name = preflight_issue.package_name;
            wrapper.package_base = preflight_issue.package_base;
            wrapper.preflight_issue = preflight_issue;
            wrapper.diagnostic = preflight_issue.diagnostic;
            preparation.issues.push_back(std::move(wrapper));
        }
    }
    return preparation;
}

std::size_t count_preparation_blockers_with_reason(
    const UnifiedPlanObservation& observation,
    AurUpdatePreparationReason reason) {
    return static_cast<std::size_t>(std::count_if(
        observation.blockers().begin(), observation.blockers().end(),
        [reason](const UnifiedPlanBlocker& blocker) {
            const auto* route =
                std::get_if<RoutePreflightUnifiedPlanBlocker>(
                    &blocker);
            if(route == nullptr) return false;
            const auto* preparation = std::get_if<
                UnifiedPlanBorrowedAuthorityReference<
                    AurUpdatePreparationIssue>>(
                &route->detail);
            return preparation != nullptr &&
                   preparation->get().reason == reason;
        }));
}

void expect_blocking_preflight_projection(
    const AurUpdateQueryResult& query,
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateSourceBuildPreparation& preparation,
    std::size_t original_issue_count,
    const std::vector<AurUpdatePreparationReason>& preparation_reasons,
    std::string_view context) {
    auto verify = [&](const UnifiedPlanObservation& observation,
                      std::string_view route) {
        const std::string route_context =
            std::string(context) + " " + std::string(route);
        expect(
            observation.status() == UnifiedPlanObservationStatus::Blocked &&
                observation.transaction_intents().empty() &&
                count_route_preflight_blockers<
                    AurUpdateExecutionIssue>(observation) ==
                    original_issue_count &&
                count_route_preflight_blockers<
                    AurUpdatePreparationIssue>(observation) ==
                    preparation_reasons.size(),
            route_context + " projected unexpected blocker authorities");
        for(const AurUpdatePreparationReason reason : preparation_reasons) {
            const std::size_t expected_count =
                static_cast<std::size_t>(std::count(
                    preparation_reasons.begin(),
                    preparation_reasons.end(), reason));
            expect(
                count_preparation_blockers_with_reason(
                    observation, reason) == expected_count,
                route_context + " lost a typed preparation blocker");
        }
    };

    const std::unique_ptr<UnifiedPlanProjection> aur_projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(query), std::cref(preflight), false,
                std::cref(preparation)});
    verify(
        require_observation(*aur_projection, context), "upgrade-aur");

    SystemSourceUpgradePreparedSnapshot system_snapshot;
    const std::vector<SystemSourceUpgradeIssue> system_issues;
    const std::vector<ProductionSourceBuildWorkItem> system_work;
    const SystemSourceUpgradeProjectionAuthority system_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            system_snapshot, nullptr, system_issues, system_work);
    const UpgradeAllOperationPreparedSnapshot aggregate_snapshot{
        system_snapshot, {}, {}};
    const UpgradeAllOperationProjectionAuthority aggregate_authority =
        UnifiedPlanProjectionTestAccess::make_upgrade_all(
            aggregate_snapshot, system_authority);
    const std::vector<UpgradeAllOperationIssue> aggregate_issues;
    const std::unique_ptr<UnifiedPlanProjection> upgrade_projection =
        project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(aggregate_authority),
                std::cref(query), std::cref(preflight),
                std::cref(aggregate_issues),
                std::cref(preparation)});
    verify(
        require_observation(*upgrade_projection, context), "upgrade-all");
}

AurUpdateExecutionPreflight blocking_preflight_fixture(
    AurUpdateExecutionIssue issue) {
    AurUpdateExecutionTarget target;
    target.update_plan_index = 0;
    target.update = update_entry();
    target.status = AurUpdateExecutionTargetStatus::Incomplete;
    target.issues.push_back(std::move(issue));
    return AurUpdateExecutionPreflight{
        {std::move(target)}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
}

BuildPlanArtifactTargetProjectionIssue projection_issue_fixture(
    BuildPlanArtifactTargetProjectionIssueKind kind,
    std::vector<RootTargetIdentity> roots) {
    BuildPlanArtifactTargetProjectionIssue issue;
    issue.kind = kind;
    issue.build_plan_order_index = 0;
    issue.entry_package_name_index = 0;
    issue.package_target_indices = {0};
    issue.package_base = "suite-base";
    issue.package_name = "suite-child";
    issue.roots = std::move(roots);
    issue.diagnostic = "fixture build-plan projection issue";
    return issue;
}

AurUpdateExecutionPreflight projection_blocking_preflight_fixture(
    AurUpdateExecutionReason reason,
    BuildPlanArtifactTargetProjectionIssueKind kind,
    std::vector<RootTargetIdentity> roots) {
    BuildPlan plan = build_plan_fixture();
    // Keep root identity valid while making artifact projection take its
    // typed DesiredInstallReasonUnavailable failure path.
    plan.package_targets.front().roles.push_back(
        static_cast<PackageRole>(999));

    AurUpdateExecutionIssue issue{
        reason, "suite-child", "suite-base", std::nullopt,
        "fixture blocking projection issue",
        projection_issue_fixture(kind, std::move(roots))};
    AurUpdateExecutionTarget target;
    target.update_plan_index = 0;
    target.build_plan_root_index = 0;
    target.update = update_entry();
    target.status = AurUpdateExecutionTargetStatus::Incomplete;
    target.issues.push_back(std::move(issue));
    return AurUpdateExecutionPreflight{
        {std::move(target)}, std::move(plan), DevelRequiresCheckPolicy::BlockOperation};
}

void test_aur_update_blocking_preflight_wrapper_is_not_duplicated() {
    const AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight preflight = blocking_preflight_fixture(
        AurUpdateExecutionIssue{
            AurUpdateExecutionReason::BuildPlanInconsistent,
            "suite-child", "suite-base", std::nullopt,
            "fixture blocking preflight issue"});
    const AurUpdateSourceBuildPreparation preparation =
        blocking_preflight_preparation_fixture(preflight);
    expect_blocking_preflight_projection(
        query, preflight, preparation, 1, {},
        "producer-shaped blocking preflight");

    const RootTargetIdentity root{0, "suite-child"};
    const AurUpdateExecutionPreflight projection_preflight =
        projection_blocking_preflight_fixture(
            AurUpdateExecutionReason::BuildPlanInconsistent,
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable,
            {root});
    const AurUpdateSourceBuildPreparation projection_preparation =
        blocking_preflight_preparation_fixture(projection_preflight);
    expect_blocking_preflight_projection(
        query, projection_preflight, projection_preparation, 1, {},
        "producer-shaped projection preflight");

    AurUpdateExecutionTarget multiple_target;
    multiple_target.update_plan_index = 0;
    multiple_target.update = update_entry();
    multiple_target.status = AurUpdateExecutionTargetStatus::Incomplete;
    multiple_target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::BuildPlanInconsistent,
        "suite-child", "suite-base", std::nullopt,
        "fixture first blocking issue"});
    multiple_target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::UnresolvedDependency,
        "suite-child", "suite-base", "fixture-runtime",
        "fixture second blocking issue"});
    const AurUpdateExecutionPreflight multiple_preflight{
        {std::move(multiple_target)}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    const AurUpdateSourceBuildPreparation multiple_preparation =
        blocking_preflight_preparation_fixture(multiple_preflight);
    expect_blocking_preflight_projection(
        query, multiple_preflight, multiple_preparation, 2, {},
        "multiple unrelated blocking preflight issues");
}

AurUpdateExecutionIssue required_devel_blocker_issue_fixture() {
    AurUpdateRequiredDevelTargetBlocker blocker{
        AurUpdateRequiredDevelTargetRelation::AurExactDependency,
        7,
        "required-devel-git",
        DevelRequiresCheckReason::SuffixCandidateOnly};
    blocker.dependency_edge_index = 3;
    blocker.build_plan_order_index = 2;
    blocker.package_base = "required-devel-base";
    blocker.roles = {PackageRole::RuntimeDependency};
    blocker.affected_roots = {{0, "suite-child"}};

    AurUpdateExecutionIssue issue;
    issue.reason =
        AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck;
    issue.package_name = "suite-child";
    issue.package_base = "suite-base";
    issue.diagnostic = "fixture required-devel blocker";
    issue.devel_requires_check_reason =
        DevelRequiresCheckReason::SuffixCandidateOnly;
    issue.required_devel_target_blocker = std::move(blocker);
    return issue;
}

void test_required_devel_blocker_wrapper_identity_is_lossless() {
    const AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight preflight =
        blocking_preflight_fixture(
            required_devel_blocker_issue_fixture());
    const AurUpdateSourceBuildPreparation exact_preparation =
        blocking_preflight_preparation_fixture(preflight);
    expect_blocking_preflight_projection(
        query, preflight, exact_preparation, 1, {},
        "exact required-devel blocker wrapper");

    struct DriftCase {
        std::string_view name;
        std::function<void(AurUpdateRequiredDevelTargetBlocker&)>
            mutate;
    };
    const std::vector<DriftCase> drift_cases{
        {"relation", [](auto& blocker) {
             blocker.relation =
                 AurUpdateRequiredDevelTargetRelation::AurProvider;
         }},
        {"target index", [](auto& blocker) {
             blocker.requires_check_update_plan_index = 8;
         }},
        {"dependency edge", [](auto& blocker) {
             blocker.dependency_edge_index = 4;
         }},
        {"build order", [](auto& blocker) {
             blocker.build_plan_order_index = 3;
         }},
        {"package child", [](auto& blocker) {
             blocker.package_name = "other-required-devel-git";
         }},
        {"package base", [](auto& blocker) {
             blocker.package_base = "other-required-devel-base";
         }},
        {"roles", [](auto& blocker) {
             blocker.roles.push_back(PackageRole::BuildDependency);
         }},
        {"reason", [](auto& blocker) {
             blocker.devel_requires_check_reason =
                 DevelRequiresCheckReason::NoAuthoritativeBuildProvenance;
         }},
        {"affected roots", [](auto& blocker) {
             blocker.affected_roots = {{1, "other-root"}};
         }},
    };

    for(const DriftCase& drift_case : drift_cases) {
        AurUpdateSourceBuildPreparation drifted =
            blocking_preflight_preparation_fixture(preflight);
        AurUpdateExecutionIssue& wrapped_issue =
            *drifted.issues.front().preflight_issue;
        drift_case.mutate(
            *wrapped_issue.required_devel_target_blocker);
        expect_blocking_preflight_projection(
            query, preflight, drifted, 1,
            {AurUpdatePreparationReason::BlockingPreflight},
            std::string{"required-devel blocker "} +
                std::string{drift_case.name} + " drift");
    }
}

void test_malformed_blocking_preflight_wrappers_are_retained() {
    const AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight preflight = blocking_preflight_fixture(
        AurUpdateExecutionIssue{
            AurUpdateExecutionReason::BuildPlanInconsistent,
            "suite-child", "suite-base", std::nullopt,
            "fixture blocking preflight issue"});

    AurUpdateSourceBuildPreparation affected_root =
        blocking_preflight_preparation_fixture(preflight);
    affected_root.issues.front().affected_roots.push_back(
        RootTargetIdentity{0, "suite-child"});
    expect_blocking_preflight_projection(
        query, preflight, affected_root, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with affected root");

    AurUpdateSourceBuildPreparation source_preference =
        blocking_preflight_preparation_fixture(preflight);
    source_preference.issues.front().source_preference_failure =
        SourcePreferenceFailure{
            SourcePreferenceFailureKind::AuthorityUnavailable,
            "fixture-source-preference", std::nullopt, std::nullopt,
            "fixture source-preference failure"};
    expect_blocking_preflight_projection(
        query, preflight, source_preference, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with source-preference attribution");

    AurUpdateSourceBuildPreparation package_metadata =
        blocking_preflight_preparation_fixture(preflight);
    package_metadata.issues.front().package_metadata_failure =
        PackageMetadataFailure{
            PackageMetadataErrorCode::LocalDatabaseUnavailable,
            "fixture package metadata failure"};
    expect_blocking_preflight_projection(
        query, preflight, package_metadata, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with package-metadata attribution");

    AurUpdateSourceBuildPreparation outer_projection =
        blocking_preflight_preparation_fixture(preflight);
    outer_projection.issues.front().build_plan_projection_issue =
        projection_issue_fixture(
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable,
            {RootTargetIdentity{0, "suite-child"}});
    expect_blocking_preflight_projection(
        query, preflight, outer_projection, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with outer projection attribution");

    const AurUpdateExecutionPreflight unknown_reason_preflight =
        blocking_preflight_fixture(AurUpdateExecutionIssue{
            static_cast<AurUpdateExecutionReason>(999),
            "suite-child", "suite-base", std::nullopt,
            "fixture unknown preflight reason"});
    const AurUpdateSourceBuildPreparation unknown_reason =
        blocking_preflight_preparation_fixture(
            unknown_reason_preflight);
    expect_blocking_preflight_projection(
        query, unknown_reason_preflight, unknown_reason, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with unknown nested reason");

    const AurUpdateExecutionPreflight none_reason_preflight =
        blocking_preflight_fixture(AurUpdateExecutionIssue{
            AurUpdateExecutionReason::None,
            "suite-child", "suite-base", std::nullopt,
            "fixture absent preflight reason"});
    const AurUpdateSourceBuildPreparation none_reason =
        blocking_preflight_preparation_fixture(none_reason_preflight);
    expect_blocking_preflight_projection(
        query, none_reason_preflight, none_reason, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with None nested reason");

    const RootTargetIdentity root{0, "suite-child"};
    const AurUpdateExecutionPreflight unknown_kind_preflight =
        projection_blocking_preflight_fixture(
            AurUpdateExecutionReason::BuildPlanInconsistent,
            static_cast<
                BuildPlanArtifactTargetProjectionIssueKind>(999),
            {root});
    const AurUpdateSourceBuildPreparation unknown_kind =
        blocking_preflight_preparation_fixture(unknown_kind_preflight);
    expect_blocking_preflight_projection(
        query, unknown_kind_preflight, unknown_kind, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with unknown projection kind");

    const AurUpdateExecutionPreflight mismatched_reason_preflight =
        projection_blocking_preflight_fixture(
            AurUpdateExecutionReason::PackageBaseMismatch,
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable,
            {root});
    const AurUpdateSourceBuildPreparation mismatched_reason =
        blocking_preflight_preparation_fixture(
            mismatched_reason_preflight);
    expect_blocking_preflight_projection(
        query, mismatched_reason_preflight, mismatched_reason, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with mismatched reason and projection kind");

    const AurUpdateExecutionPreflight mismatched_root_preflight =
        projection_blocking_preflight_fixture(
            AurUpdateExecutionReason::BuildPlanInconsistent,
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable,
            {RootTargetIdentity{1, "other-root"}});
    const AurUpdateSourceBuildPreparation mismatched_root =
        blocking_preflight_preparation_fixture(
            mismatched_root_preflight);
    expect_blocking_preflight_projection(
        query, mismatched_root_preflight, mismatched_root, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with cross-target projection root");

    const AurUpdateExecutionPreflight extra_root_preflight =
        projection_blocking_preflight_fixture(
            AurUpdateExecutionReason::BuildPlanInconsistent,
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable,
            {root, RootTargetIdentity{1, "unknown-root"}});
    const AurUpdateSourceBuildPreparation extra_root =
        blocking_preflight_preparation_fixture(extra_root_preflight);
    expect_blocking_preflight_projection(
        query, extra_root_preflight, extra_root, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with additional unknown projection root");

    const AurUpdateExecutionPreflight duplicate_root_preflight =
        projection_blocking_preflight_fixture(
            AurUpdateExecutionReason::BuildPlanInconsistent,
            BuildPlanArtifactTargetProjectionIssueKind::
                DesiredInstallReasonUnavailable,
            {root, root});
    const AurUpdateSourceBuildPreparation duplicate_root =
        blocking_preflight_preparation_fixture(
            duplicate_root_preflight);
    expect_blocking_preflight_projection(
        query, duplicate_root_preflight, duplicate_root, 1,
        {AurUpdatePreparationReason::BlockingPreflight},
        "blocking wrapper with duplicate projection root");
}

void test_strict_preparation_blockers_are_not_suppressed() {
    const AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight preflight = blocking_preflight_fixture(
        AurUpdateExecutionIssue{
            AurUpdateExecutionReason::BuildPlanInconsistent,
            "suite-child", "suite-base", std::nullopt,
            "fixture blocking preflight issue"});
    AurUpdateSourceBuildPreparation preparation =
        blocking_preflight_preparation_fixture(preflight);

    AurUpdatePreparationIssue source_preference;
    source_preference.reason =
        AurUpdatePreparationReason::SourcePreferenceUnavailable;
    source_preference.affected_update_plan_indices = {0};
    source_preference.package_name = "suite-child";
    source_preference.package_base = "suite-base";
    source_preference.source_preference_failure = SourcePreferenceFailure{
        SourcePreferenceFailureKind::AuthorityUnavailable,
        "fixture-source-preference", std::nullopt, std::nullopt,
        "fixture source-preference failure"};
    source_preference.diagnostic = "fixture source-preference blocker";
    preparation.issues.push_back(std::move(source_preference));

    AurUpdatePreparationIssue pkgdest;
    pkgdest.reason =
        AurUpdatePreparationReason::SourcePreferencePkgdestConflict;
    pkgdest.affected_update_plan_indices = {0};
    pkgdest.package_name = "suite-child";
    pkgdest.package_base = "suite-base";
    pkgdest.diagnostic = "fixture PKGDEST blocker";
    preparation.issues.push_back(std::move(pkgdest));

    AurUpdatePreparationIssue static_work;
    static_work.reason = AurUpdatePreparationReason::StaticWorkItemInvalid;
    static_work.affected_update_plan_indices = {0};
    static_work.package_name = "suite-child";
    static_work.package_base = "suite-base";
    static_work.diagnostic = "fixture static work blocker";
    preparation.issues.push_back(std::move(static_work));

    AurUpdatePreparationIssue pacman_database;
    pacman_database.reason =
        AurUpdatePreparationReason::PacmanDatabaseUnavailable;
    pacman_database.affected_update_plan_indices = {0};
    pacman_database.package_metadata_failure = PackageMetadataFailure{
        PackageMetadataErrorCode::LocalDatabaseUnavailable,
        "fixture Pacman database failure"};
    pacman_database.diagnostic = "fixture Pacman database blocker";
    preparation.issues.push_back(std::move(pacman_database));

    expect_blocking_preflight_projection(
        query, preflight, preparation, 1,
        {AurUpdatePreparationReason::SourcePreferenceUnavailable,
         AurUpdatePreparationReason::SourcePreferencePkgdestConflict,
         AurUpdatePreparationReason::StaticWorkItemInvalid,
         AurUpdatePreparationReason::PacmanDatabaseUnavailable},
        "strict preparation blockers");
}

void test_fetch_and_remote_source_build_adapters() {
    FetchPreparation fetch{build_plan_fixture(), "suite-child"};
    fetch.plan.metadata_risks.push_back(BuildPlanMetadataRisk{
        "suite-child", "suite-base", {"legacy-suite"}, {}});
    fetch.plan.relation_assessments.push_back(
        package_relation_assessment_fixture::
            confirmed_installed_conflict_reason(
                "suite-child", "suite-base", "legacy-suite")
                .assessment);
    const std::unique_ptr<UnifiedPlanProjection> fetch_projection =
        project_fetch_unified_plan(
            FetchUnifiedPlanProjectionInput{std::cref(fetch)});
    const UnifiedPlanObservation& fetch_observation = require_observation(
        *fetch_projection, "fetch production plan");
    expect(
        fetch_observation.status() ==
                UnifiedPlanObservationStatus::Ready &&
            fetch_observation.transaction_intents().empty() &&
            fetch_observation.build_units().size() ==
                fetch.plan.order.size() &&
            has_phase(
                fetch_observation,
                UnifiedPlanObservationPhase::SourceRetrieval,
                UnifiedPlanAuthorityOwner::Git),
        "fetch adapter treated review-only metadata risk as an execution blocker");

    FetchPreparation blocked_fetch{build_plan_fixture(), "suite-child"};
    blocked_fetch.plan.unresolved.push_back("missing-runtime");
    const std::unique_ptr<UnifiedPlanProjection> blocked_fetch_projection =
        project_fetch_unified_plan(
            FetchUnifiedPlanProjectionInput{
                std::cref(blocked_fetch)});
    const UnifiedPlanObservation& blocked_fetch_observation =
        require_observation(
            *blocked_fetch_projection, "blocked fetch plan");
    expect(
        blocked_fetch_observation.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            !has_phase(
                blocked_fetch_observation,
                UnifiedPlanObservationPhase::SourceRetrieval),
        "fetch BuildPlan blocker exposed retrieval mutation");

    PreparedRemoteSourceBuild repository_build{
        ResolvedSourceBuildIdentity{
            ResolvedRepositorySourceBuildIdentity{
                RepositoryPackagePresent{
                    "core", 0, "repo-child",
                    "repo-child"}}},
        std::nullopt,
        PreparedProductionSourceBuildInvocation{}};
    ProductionSourceBuildWorkItem repository_work =
        source_work_item(
            "repo-child", "repo-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet,
            SourceBuildSourceKind::Repository, false);
    repository_work.request.git_url = repository_build.source.git_url();
    repository_build.invocation.work_items.push_back(
        std::move(repository_work));
    const std::unique_ptr<UnifiedPlanProjection> repository_projection =
        project_remote_source_build_unified_plan(
            RemoteSourceBuildUnifiedPlanProjectionInput{
                std::cref(repository_build)});
    const UnifiedPlanObservation& repository_observation =
        require_observation(
            *repository_projection,
            "repository remote source build");
    expect(
        repository_observation.status() ==
                UnifiedPlanObservationStatus::Ready &&
            repository_observation.required_artifacts().size() == 1 &&
            std::holds_alternative<
                PreparedRemoteSourceBuildUnitReference>(
                repository_observation.build_units().front()) &&
            repository_observation.transaction_intents().size() == 1,
        "repository remote build lost actual work or install boundary");

    repository_build.invocation.work_items.front()
        .artifact_lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility;
    expect_invalid_argument(
        [&repository_build]() {
            static_cast<void>(project_remote_source_build_unified_plan(
                RemoteSourceBuildUnifiedPlanProjectionInput{
                    std::cref(repository_build)}));
        },
        "standalone repository singular lifecycle drift");
    repository_build.invocation.work_items.front()
        .artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    repository_build.invocation.work_items.front().request.only_if_updated =
        true;
    expect_invalid_argument(
        [&repository_build]() {
            static_cast<void>(project_remote_source_build_unified_plan(
                RemoteSourceBuildUnifiedPlanProjectionInput{
                    std::cref(repository_build)}));
        },
        "standalone repository only-if-updated drift");
    repository_build.invocation.work_items.front().request.only_if_updated =
        false;
    repository_build.invocation.work_items.front().request.needed = true;
    expect_invalid_argument(
        [&repository_build]() {
            static_cast<void>(project_remote_source_build_unified_plan(
                RemoteSourceBuildUnifiedPlanProjectionInput{
                    std::cref(repository_build)}));
        },
        "standalone repository needed drift");
    repository_build.invocation.work_items.front().request.needed = false;

    RemoteSourceBuildPlanFailure remote_failure{
        ResolvedSourceBuildIdentity{
            ResolvedAurSourceBuildIdentity{
                "suite-child", "suite-base"}},
        build_plan_fixture()};
    remote_failure.plan.unresolved.push_back("missing-runtime");
    const std::unique_ptr<UnifiedPlanProjection> remote_failure_projection =
        project_remote_source_build_unified_plan(
            RemoteSourceBuildUnifiedPlanProjectionInput{
                std::cref(remote_failure)});
    const UnifiedPlanObservation& remote_failure_observation =
        require_observation(
            *remote_failure_projection,
            "remote AUR BuildPlan failure");
    expect(
        remote_failure_observation.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            remote_failure_observation.transaction_intents().empty() &&
            !has_phase(
                remote_failure_observation,
                UnifiedPlanObservationPhase::SourceBuild),
        "remote AUR BuildPlan failure fabricated executable work");
}

void test_sync_install_preparation_adapters() {
    const std::vector<std::string> repository_order{
        "core", "extra", "multilib"};
    PreparedSyncInstall prepared;
    prepared.ordered_roots.push_back(SyncRepositoryTransactionRoot{
        RootTargetIdentity{0, "repo-root"},
        RepositoryPackagePresent{
            "core", 0, "repo-root", "repo-root", std::nullopt,
            repository_order}});
    prepared.repository_pacman_args = {"repo-root"};
    prepared.repository_transaction_required = true;
    prepared.system_update = true;
    prepared.needed = true;

    const std::unique_ptr<UnifiedPlanProjection> ready_projection =
        project_sync_install_unified_plan(
            SyncInstallUnifiedPlanProjectionInput{
                std::cref(prepared)});
    const UnifiedPlanObservation& ready = require_observation(
        *ready_projection, "prepared sync install");
    expect(
        ready.status() == UnifiedPlanObservationStatus::Ready &&
            ready.roots().size() == 1 &&
            ready.root_metadata().size() == 1 &&
            ready.transaction_intents().size() == 1 &&
            std::get<RepositoryPackageTransactionIntent>(
                ready.transaction_intents().front())
                    .targets.size() == 2 &&
            has_phase(
                ready,
                UnifiedPlanObservationPhase::RepositoryTransaction,
                UnifiedPlanAuthorityOwner::Pacman),
        "prepared sync install lost repository/system-update authority");

    PreparedSyncInstall aur_prepared;
    aur_prepared.source_selection = PackageSourceSelection::AurOnly;
    aur_prepared.needed = true;
    aur_prepared.aur_build_plan.emplace(build_plan_fixture());
    aur_prepared.ordered_roots.push_back(SyncAurRoot{
        RootTargetIdentity{0, "suite-child"}, std::nullopt, 0});
    aur_prepared.source_invocation.emplace();
    aur_prepared.source_invocation->work_items.push_back(source_work_item(
        "suite-base", "suite-child",
        DesiredInstallReason::Explicit,
        ArtifactLifecycleIntent::PackageBaseSet,
        SourceBuildSourceKind::Aur, true));
    aur_prepared.source_invocation->work_items.front()
        .configured_repository_order =
        aur_prepared.aur_build_plan->configured_repository_order;
    aur_prepared.source_invocation->work_items.front()
        .selected_repository_providers.push_back(
            aur_prepared.aur_build_plan->provided.front().provider);
    aur_prepared.source_invocation->selected_repository_providers.push_back(
        aur_prepared.aur_build_plan->provided.front().provider);
    const std::unique_ptr<UnifiedPlanProjection> aur_projection =
        project_sync_install_unified_plan(
            SyncInstallUnifiedPlanProjectionInput{
                std::cref(aur_prepared)});
    const UnifiedPlanObservation& aur_observation = require_observation(
        *aur_projection, "prepared AUR-only sync install");
    const auto* aur_repository_transaction = std::get_if<
        RepositoryPackageTransactionIntent>(
        &aur_observation.transaction_intents().front());
    expect(
        aur_observation.status() ==
                UnifiedPlanObservationStatus::Ready &&
            aur_observation.transaction_intents().size() == 2 &&
            aur_repository_transaction != nullptr &&
            aur_repository_transaction->targets.size() == 1 &&
            std::holds_alternative<RepositoryProviderInstallIntent>(
                aur_repository_transaction->targets.front()),
        "AUR-only sync duplicated provider transaction authority or required an initial pacman route");

    SyncInstallPreparationFailure failure;
    failure.details.push_back(SyncInstallPreparationIssue{
        SyncInstallPreparationIssueKind::InvalidTarget,
        RootTargetIdentity{0, "invalid-root"}, std::nullopt,
        "fixture invalid sync target"});
    const std::unique_ptr<UnifiedPlanProjection> blocked_projection =
        project_sync_install_unified_plan(
            SyncInstallUnifiedPlanProjectionInput{
                std::cref(failure)});
    const UnifiedPlanObservation& blocked = require_observation(
        *blocked_projection, "blocked sync install");
    const auto* blocker = std::get_if<
        SyncInstallPreparationUnifiedPlanBlocker>(
        &blocked.blockers().front());
    expect(
        blocked.status() == UnifiedPlanObservationStatus::Blocked &&
            blocked.transaction_intents().empty() &&
            blocker != nullptr &&
            &blocker->detail.get() == &failure &&
            !has_phase(
                blocked,
                UnifiedPlanObservationPhase::RepositoryTransaction),
        "blocked sync preparation fabricated transaction authority");
}

void test_sync_duplicate_repository_source_correlation() {
    const std::vector<std::string> repository_order{
        "core", "extra", "multilib"};
    const RepositoryPackagePresent package{
        "core", 0, "duplicate-root", "duplicate-root", std::nullopt,
        repository_order};
    const ResolvedSourceBuildIdentity source{
        ResolvedRepositorySourceBuildIdentity{package}};

    PreparedSyncInstall prepared;
    prepared.source_invocation.emplace();
    for(std::size_t index = 0; index < 2; ++index) {
        prepared.ordered_roots.push_back(SyncRepositorySourceRoot{
            RootTargetIdentity{index, "duplicate-root"}, package,
            source, index});
        ProductionSourceBuildWorkItem work =
            source_work_item("duplicate-root", "duplicate-root");
        work.request.git_url = source.git_url();
        work.repository_identity = *source.repository_identity();
        work.configured_repository_order = repository_order;
        prepared.source_invocation->work_items.push_back(std::move(work));
    }

    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_sync_install_unified_plan(
            SyncInstallUnifiedPlanProjectionInput{
                std::cref(prepared)});
    const UnifiedPlanObservation& observation = require_observation(
        *projection, "duplicate repository source sync roots");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Ready &&
            observation.roots().size() == 2 &&
            observation.build_units().size() == 2 &&
            observation.required_artifacts().size() == 2,
        "duplicate repository source roots became ambiguous or were deduplicated");

    for(std::size_t index = 0; index < 2; ++index) {
        const auto* root = std::get_if<SyncRepositorySourceRoot>(
            &prepared.ordered_roots[index]);
        const auto* unit = std::get_if<PreparedRemoteSourceBuildUnitReference>(
            &observation.build_units()[index]);
        expect(
            root != nullptr &&
                root->invocation_correlation.invocation_index ==
                    index &&
                root->source_work_item_index == index &&
                observation.roots()[index]
                        .invocation_correlation()
                        .invocation_index == index &&
                unit != nullptr &&
                &unit->source() == &root->source &&
                &unit->work_item() ==
                    &prepared.source_invocation
                         ->work_items[index],
            "duplicate repository source root used a name-matched work item instead of its typed correlation");
    }
}

void test_actual_prepared_source_work_is_artifact_authority() {
    SystemSourceUpgradePreparedSnapshot snapshot;
    snapshot.preference_root_exists = true;
    snapshot.registered_sources.push_back(registered_source(
        3, "repo-child", "repo-base",
        SourceBuildSourceKind::Repository));
    snapshot.registered_sources.push_back(registered_source(
        7, "suite-child", "suite-base", SourceBuildSourceKind::Aur));

    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(source_work_item(
        "repo-base", "repo-child",
        DesiredInstallReason::Explicit,
        ArtifactLifecycleIntent::PackageBaseSet));
    work_items.push_back(source_work_item(
        "suite-base", "suite-child",
        DesiredInstallReason::Explicit,
        ArtifactLifecycleIntent::SingularCompatibility,
        SourceBuildSourceKind::Aur));
    BuildPlan aur_plan = build_plan_fixture();
    aur_plan.order.insert(
        aur_plan.order.begin(),
        BuildPlanEntry{"dependency-base", {"dependency-child"}});
    work_items[1].configured_repository_order =
        aur_plan.configured_repository_order;
    work_items[1].selected_repository_providers.push_back(
        aur_plan.provided.front().provider);
    const std::vector<SystemSourceUpgradeIssue> issues;
    const SystemSourceUpgradeProjectionAuthority authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            snapshot, &aur_plan, issues, work_items);

    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_system_source_upgrade_unified_plan(
            SystemSourceUpgradeUnifiedPlanProjectionInput{
                std::cref(authority)});
    const UnifiedPlanObservation& observation =
        require_observation(*projection, "prepared source work");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Ready,
        "prepared system/source projection was not Ready");
    expect(
        observation.required_artifacts().size() == 2 &&
            &std::get<PreparedSystemSourceBuildUnitReference>(
                 observation.required_artifacts()[0].build_unit())
                    .required_targets() ==
                &work_items[0].required_targets &&
            &std::get<PreparedSystemSourceBuildUnitReference>(
                 observation.required_artifacts()[1].build_unit())
                    .required_targets() ==
                &work_items[1].required_targets,
        "actual prepared required_targets authority was replaced");
    const bool projected_dependency_unit = std::any_of(
        observation.required_artifacts().begin(),
        observation.required_artifacts().end(),
        [](const RequiredArtifactTargetReference& artifact) {
            return artifact.target().package_name == "dependency-child";
        });
    expect(
        !projected_dependency_unit,
        "a non-work BuildPlan unit became source install intent");
    expect(
        std::holds_alternative<RepositorySourceBuildRootIdentity>(
            observation.roots()[0].source_identity()) &&
            std::holds_alternative<AurRootPackageIdentity>(
                observation.roots()[1].source_identity()),
        "registered repository/AUR source identity was flattened");
    expect(
        has_existing_phase(
            observation,
            UnifiedPlanObservationPhase::RepositoryTransaction,
            SystemSourceUpgradePhase::System) &&
            has_existing_phase(
                observation,
                UnifiedPlanObservationPhase::SourceBuild,
                SystemSourceUpgradePhase::RegisteredSource),
        "system/source typed route phases were not projected");
}

void test_system_issue_impact_and_blocked_phases() {
    SystemSourceUpgradePreparedSnapshot snapshot;
    snapshot.registered_sources.push_back(registered_source(
        0, "repo-child", "repo-base",
        SourceBuildSourceKind::Repository));
    const std::vector<ProductionSourceBuildWorkItem> work_items{
        source_work_item(
            "repo-base", "repo-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet)};
    const std::vector<SystemSourceUpgradeIssue> non_blocking_issues{
        system_issue(
            SystemSourceUpgradeIssueKind::
                SystemPackageSnapshotUnavailable,
            SystemSourceUpgradeIssueImpact::ObservabilityOnly,
            SystemSourceUpgradePhase::System),
        system_issue(
            SystemSourceUpgradeIssueKind::
                PostSystemSourceSnapshotUnavailable,
            SystemSourceUpgradeIssueImpact::AffectsSuccess,
            SystemSourceUpgradePhase::RegisteredSource)};
    const SystemSourceUpgradeProjectionAuthority non_blocking_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            snapshot, nullptr, non_blocking_issues, work_items);
    const std::unique_ptr<UnifiedPlanProjection> ready_projection =
        project_system_source_upgrade_unified_plan(
            SystemSourceUpgradeUnifiedPlanProjectionInput{
                std::cref(non_blocking_authority)});
    const UnifiedPlanObservation& ready =
        require_observation(*ready_projection, "non-blocking issues");
    expect(
        ready.status() == UnifiedPlanObservationStatus::Ready &&
            ready.blockers().empty() &&
            non_blocking_authority.issues().size() == 2,
        "non-blocking typed issues were promoted to blockers");
    expect(
        !has_phase(
            ready, UnifiedPlanObservationPhase::MetadataDiscovery,
            UnifiedPlanAuthorityOwner::AurRpc),
        "repository registered source fabricated an AUR phase");

    const std::vector<SystemSourceUpgradeIssue> blocking_issues{
        system_issue(
            SystemSourceUpgradeIssueKind::CacheAuthorityInvalid,
            SystemSourceUpgradeIssueImpact::BlocksExecution,
            SystemSourceUpgradePhase::Preparation)};
    SystemSourceUpgradeResult blocking_result;
    blocking_result.status =
        SystemSourceUpgradeStatus::BlockedBeforeMutation;
    blocking_result.stopped_phase = SystemSourceUpgradePhase::Preparation;
    blocking_result.prepared_snapshot = snapshot;
    blocking_result.registered_source_results.push_back(
        not_attempted_source_result(
            blocking_result.prepared_snapshot.registered_sources
                .front()));
    blocking_result.issues = blocking_issues;
    const std::unique_ptr<UnifiedPlanProjection> blocked_projection =
        project_system_source_upgrade_unified_plan(
            SystemSourceUpgradeUnifiedPlanProjectionInput{
                std::cref(blocking_result)});
    const UnifiedPlanObservation& blocked =
        require_observation(*blocked_projection, "blocking issue");
    expect(
        blocked.status() == UnifiedPlanObservationStatus::Blocked &&
            blocked.blockers().size() == 1 &&
            blocked.transaction_intents().empty() &&
            blocked.build_units().empty() &&
            blocked.required_artifacts().empty(),
        "BlocksExecution did not produce a mutation-free Blocked view");
    expect(
        !has_phase(
            blocked,
            UnifiedPlanObservationPhase::RepositoryTransaction) &&
            !has_phase(
                blocked,
                UnifiedPlanObservationPhase::SourceBuild),
        "Blocked route fabricated mutation phases");
}

void test_partial_failures_and_upgrade_all_authorities() {
    SystemSourceUpgradePreparedSnapshot system_snapshot;
    system_snapshot.registered_sources.push_back(registered_source(
        0, "repo-child", "repo-base",
        SourceBuildSourceKind::Repository));
    const std::vector<ProductionSourceBuildWorkItem> work_items{
        source_work_item(
            "repo-base", "repo-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet)};
    const std::vector<SystemSourceUpgradeIssue> system_issues;
    const SystemSourceUpgradeProjectionAuthority system_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            system_snapshot, nullptr, system_issues, work_items);
    const UpgradeAllOperationPreparedSnapshot aggregate_snapshot{
        system_snapshot, {}, {}};
    const UpgradeAllOperationProjectionAuthority aggregate_authority =
        UnifiedPlanProjectionTestAccess::make_upgrade_all(
            aggregate_snapshot, system_authority);

    AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight preflight =
        executable_update_preflight(build_plan_fixture());
    const std::vector<UpgradeAllOperationIssue> aggregate_issues;
    {
        const std::unique_ptr<UnifiedPlanProjection> ready_projection =
            project_upgrade_all_unified_plan(
                UpgradeAllUnifiedPlanProjectionInput{
                    std::cref(aggregate_authority),
                    std::cref(query), std::cref(preflight),
                    std::cref(aggregate_issues)});
        const UnifiedPlanObservation& ready = require_observation(
            *ready_projection, "upgrade-all ready routes");
        expect(
            ready.status() == UnifiedPlanObservationStatus::Ready &&
                ready.required_artifacts().size() == 2 &&
                &std::get<
                     PreparedSystemSourceBuildUnitReference>(
                     ready.required_artifacts()
                         .front()
                         .build_unit())
                        .required_targets() ==
                    &work_items.front().required_targets,
            "upgrade-all did not combine actual registered work with AUR intent");
        expect(
            has_existing_phase(
                ready, UnifiedPlanObservationPhase::SourceBuild,
                UpgradeAllOperationPhase::RegisteredSource) &&
                has_existing_phase(
                    ready,
                    UnifiedPlanObservationPhase::SourceBuild,
                    UpgradeAllOperationPhase::AurExecution),
            "upgrade-all source routes lost typed phase identity");
    }

    query.recoverable_failures.push_back(AurUpdateQueryFailure{
        {"failed-source"}, "fixture partial query failure"});
    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(aggregate_authority),
                std::cref(query), std::cref(preflight),
                std::cref(aggregate_issues)});
    const UnifiedPlanObservation& observation =
        require_observation(*projection, "upgrade-all partial failure");
    expect(
        observation.status() == UnifiedPlanObservationStatus::Blocked &&
            observation.transaction_intents().empty(),
        "enabled-source partial failure was flattened to Ready");
    bool query_failure_borrowed = false;
    for(const UnifiedPlanBlocker& blocker : observation.blockers()) {
        const auto* source =
            std::get_if<SourceFailureUnifiedPlanBlocker>(&blocker);
        if(source == nullptr) continue;
        const auto* failure = std::get_if<
            UnifiedPlanBorrowedAuthorityReference<
                AurUpdateQueryFailure>>(
            &source->detail);
        if(failure != nullptr &&
           &failure->get() == &query.recoverable_failures.front()) {
            query_failure_borrowed = true;
        }
    }
    expect(
        query_failure_borrowed,
        "typed AUR query failure was copied or omitted");
    expect(
        observation.required_artifacts().size() == 2 &&
            &std::get<PreparedSystemSourceBuildUnitReference>(
                 observation.required_artifacts()
                     .front()
                     .build_unit())
                    .required_targets() ==
                &work_items.front().required_targets,
        "upgrade-all lost actual registered-source work authority");
    expect(
        !has_phase(
            observation, UnifiedPlanObservationPhase::SourceBuild),
        "blocked upgrade-all fabricated source mutation phases");
    expect(
        observation.route_preflight_authorities().size() == 2 &&
            &std::get<UnifiedPlanBorrowedAuthorityReference<
                    UpgradeAllOperationProjectionAuthority>>(
                 observation.route_preflight_authorities()[0])
                    .get() == &aggregate_authority,
        "upgrade-all prepared authority was copied or omitted");
}

void test_full_identity_correlation_fail_closed() {
    const LocalSourceRoot local_source_a = open_local_source_root(
        "tests/fixtures/pkgbuild-export");
    const LocalSourceRoot local_source_b = open_local_source_root(
        "tests/fixtures/pkgbuild-export");
    const LocalPackageMetadata* local_metadata_b =
        local_source_b.metadata().parse_result()->metadata();
    const LocalBuildPlan local_plan_b = resolve_local_build_plan(
        *local_metadata_b, "x86_64");
    expect(
        *local_source_a.metadata().parse_result()->metadata() ==
            local_plan_b.local_metadata(),
        "local mixed-invocation fixture metadata values differ");
    expect_invalid_argument(
        [&local_source_a, &local_plan_b] {
            (void)project_local_source_unified_plan(
                LocalSourceUnifiedPlanProjectionInput{
                    LocalSourceBuildPlanFailureProjectionInput{
                        std::cref(local_source_a),
                        std::cref(local_plan_b)},
                    false});
        },
        "same-metadata local source/plan from independent invocations");

    PreparedRootPackageInstall different_package_base =
        make_root_prepared(false, true, std::nullopt);
    different_package_base.aur_build_plan->package_targets.front()
        .package_base = "other-package-base";
    expect_invalid_argument(
        [&different_package_base] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(different_package_base)});
        },
        "same root name with different PackageBase");

    AurUpdateQueryResult query = update_query();
    AurUpdateExecutionPreflight same_index_different_base =
        executable_update_preflight(build_plan_fixture());
    same_index_different_base.build_plan->package_targets.front()
        .package_base = "different-update-base";
    expect_invalid_argument(
        [&query, &same_index_different_base] {
            (void)project_aur_update_unified_plan(
                AurUpdateUnifiedPlanProjectionInput{
                    std::cref(query),
                    std::cref(same_index_different_base),
                    false});
        },
        "same invocation index with different PackageBase");

    PreparedRootPackageInstall same_child_different_base =
        make_root_prepared(false, true, std::nullopt);
    same_child_different_base.aur_build_plan->order.front().package_base =
        "different-order-base";
    expect_invalid_argument(
        [&same_child_different_base] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(same_child_different_base)});
        },
        "same child name with different BuildPlan PackageBase");

    PreparedRootPackageInstall mismatched_actual_work =
        make_root_prepared(false, true, std::nullopt);
    mismatched_actual_work.source_invocation->work_items.front()
        .request.checkout_name = "different-work-base";
    expect_invalid_argument(
        [&mismatched_actual_work] {
            (void)project_root_package_unified_plan(
                RootPackageUnifiedPlanProjectionInput{
                    std::cref(mismatched_actual_work)});
        },
        "actual work item with different plan identity");

    SystemSourceUpgradePreparedSnapshot source_kind_snapshot;
    source_kind_snapshot.registered_sources.push_back(registered_source(
        0, "repo-child", "repo-base",
        SourceBuildSourceKind::Repository));
    std::vector<ProductionSourceBuildWorkItem> source_kind_work{
        source_work_item(
            "repo-base", "repo-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet)};
    source_kind_work.front().uses_system_update_baseline = false;
    const std::vector<SystemSourceUpgradeIssue> no_issues;
    const SystemSourceUpgradeProjectionAuthority source_kind_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            source_kind_snapshot, nullptr, no_issues,
            source_kind_work);
    expect_invalid_argument(
        [&source_kind_authority] {
            (void)project_system_source_upgrade_unified_plan(
                SystemSourceUpgradeUnifiedPlanProjectionInput{
                    std::cref(source_kind_authority)});
        },
        "registered source kind mismatch");

    SystemSourceUpgradePreparedSnapshot repository_lifecycle_snapshot;
    repository_lifecycle_snapshot.registered_sources.push_back(
        registered_source(
            0, "repo-child", "repo-base",
            SourceBuildSourceKind::Repository));
    repository_lifecycle_snapshot.registered_sources.front()
        .artifact_lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility;
    const std::vector<ProductionSourceBuildWorkItem>
        repository_singular_work{
            source_work_item(
                "repo-base", "repo-child",
                DesiredInstallReason::Explicit,
                ArtifactLifecycleIntent::SingularCompatibility)};
    const SystemSourceUpgradeProjectionAuthority
        repository_singular_authority =
            UnifiedPlanProjectionTestAccess::make_system_source(
                repository_lifecycle_snapshot, nullptr,
                no_issues, repository_singular_work);
    expect_invalid_argument(
        [&repository_singular_authority] {
            (void)project_system_source_upgrade_unified_plan(
                SystemSourceUpgradeUnifiedPlanProjectionInput{
                    std::cref(
                        repository_singular_authority)});
        },
        "registered repository singular lifecycle");

    SystemSourceUpgradePreparedSnapshot aur_lifecycle_snapshot;
    aur_lifecycle_snapshot.registered_sources.push_back(
        registered_source(
            0, "suite-child", "suite-base",
            SourceBuildSourceKind::Aur));
    aur_lifecycle_snapshot.registered_sources.front()
        .artifact_lifecycle_intent =
        ArtifactLifecycleIntent::PackageBaseSet;
    BuildPlan aur_lifecycle_plan = build_plan_fixture();
    std::vector<ProductionSourceBuildWorkItem> aur_set_work{
        source_work_item(
            "suite-base", "suite-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet,
            SourceBuildSourceKind::Aur)};
    aur_set_work.front().configured_repository_order =
        aur_lifecycle_plan.configured_repository_order;
    aur_set_work.front().selected_repository_providers.push_back(
        aur_lifecycle_plan.provided.front().provider);
    const SystemSourceUpgradeProjectionAuthority aur_set_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            aur_lifecycle_snapshot, &aur_lifecycle_plan,
            no_issues, aur_set_work);
    expect_invalid_argument(
        [&aur_set_authority] {
            (void)project_system_source_upgrade_unified_plan(
                SystemSourceUpgradeUnifiedPlanProjectionInput{
                    std::cref(aur_set_authority)});
        },
        "registered AUR PackageBaseSet lifecycle");

    SystemSourceUpgradePreparedSnapshot artifact_identity_snapshot;
    artifact_identity_snapshot.registered_sources.push_back(
        registered_source(
            0, "suite-child", "suite-base",
            SourceBuildSourceKind::Aur));
    BuildPlan artifact_identity_plan = build_plan_fixture();
    std::vector<ProductionSourceBuildWorkItem> artifact_identity_work{
        source_work_item(
            "suite-base", "suite-child",
            DesiredInstallReason::Dependency,
            ArtifactLifecycleIntent::SingularCompatibility,
            SourceBuildSourceKind::Aur)};
    artifact_identity_work.front().configured_repository_order =
        artifact_identity_plan.configured_repository_order;
    artifact_identity_work.front().selected_repository_providers.push_back(
        artifact_identity_plan.provided.front().provider);
    const SystemSourceUpgradeProjectionAuthority artifact_identity_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            artifact_identity_snapshot, &artifact_identity_plan,
            no_issues, artifact_identity_work);
    expect_invalid_argument(
        [&artifact_identity_authority] {
            (void)project_system_source_upgrade_unified_plan(
                SystemSourceUpgradeUnifiedPlanProjectionInput{
                    std::cref(artifact_identity_authority)});
        },
        "actual required artifact target with different root identity");

    SystemSourceUpgradePreparedSnapshot nested_snapshot;
    nested_snapshot.registered_sources.push_back(registered_source(
        0, "repo-child", "repo-base",
        SourceBuildSourceKind::Repository));
    const std::vector<ProductionSourceBuildWorkItem> nested_work{
        source_work_item(
            "repo-base", "repo-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet)};
    const SystemSourceUpgradeProjectionAuthority nested_authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            nested_snapshot, nullptr, no_issues, nested_work);
    UpgradeAllOperationPreparedSnapshot mixed_outer{
        nested_snapshot, {}, {}};
    mixed_outer.system_source.registered_sources.front()
        .resolved_package_base = "other-invocation-base";
    const AurUpdateExecutionPreflight update_preflight =
        executable_update_preflight(build_plan_fixture());
    const std::vector<UpgradeAllOperationIssue> upgrade_issues;
    const auto expect_mismatched_system_source_snapshot =
        [&nested_authority, &query, &update_preflight,
         &upgrade_issues](
            const UpgradeAllOperationPreparedSnapshot& outer,
            std::string_view context) {
            const UpgradeAllOperationProjectionAuthority authority =
                UnifiedPlanProjectionTestAccess::make_upgrade_all(
                    outer, nested_authority);
            expect_invalid_argument(
                [&authority, &query, &update_preflight, &upgrade_issues] {
                    (void)project_upgrade_all_unified_plan(
                        UpgradeAllUnifiedPlanProjectionInput{
                            std::cref(authority),
                            std::cref(query),
                            std::cref(update_preflight),
                            std::cref(upgrade_issues)});
                },
                context);
        };
    expect_mismatched_system_source_snapshot(
        mixed_outer, "upgrade-all nested different invocation");

    UpgradeAllOperationPreparedSnapshot mixed_repository_identity{
        nested_snapshot, {}, {}};
    mixed_repository_identity.system_source.registered_sources.front()
        .repository_identity = ResolvedRepositorySourceBuildIdentity{
        RepositoryPackagePresent{
            "extra", 1, "repo-child", "repo-base"}};
    expect_mismatched_system_source_snapshot(
        mixed_repository_identity,
        "upgrade-all nested different repository identity");

    UpgradeAllOperationPreparedSnapshot mixed_provenance{
        nested_snapshot, {}, {}};
    mixed_provenance.system_source.registered_sources.front()
        .required_target_provenance =
        RequiredTargetProvenance::AurBuildPlanProjection;
    expect_mismatched_system_source_snapshot(
        mixed_provenance,
        "upgrade-all nested different required-target provenance");

    UpgradeAllOperationPreparedSnapshot mixed_lifecycle{
        nested_snapshot, {}, {}};
    mixed_lifecycle.system_source.registered_sources.front()
        .artifact_lifecycle_intent =
        ArtifactLifecycleIntent::SingularCompatibility;
    expect_mismatched_system_source_snapshot(
        mixed_lifecycle,
        "upgrade-all nested different artifact lifecycle intent");

    UpgradeAllOperationPreparedSnapshot mixed_options{
        nested_snapshot, {}, {}};
    mixed_options.system_source.options.no_confirm = true;
    const UpgradeAllOperationProjectionAuthority mixed_options_authority =
        UnifiedPlanProjectionTestAccess::make_upgrade_all(
            mixed_options, nested_authority);
    expect_invalid_argument(
        [&mixed_options_authority, &query, &update_preflight,
         &upgrade_issues] {
            (void)project_upgrade_all_unified_plan(
                UpgradeAllUnifiedPlanProjectionInput{
                    std::cref(mixed_options_authority),
                    std::cref(query),
                    std::cref(update_preflight),
                    std::cref(upgrade_issues)});
        },
        "upgrade-all nested different option snapshot");

    std::vector<ProductionSourceBuildWorkItem> mismatched_needed_work{
        source_work_item(
            "repo-base", "repo-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::PackageBaseSet)};
    mismatched_needed_work.front().request.needed = true;
    const SystemSourceUpgradeProjectionAuthority
        mismatched_needed_system_authority =
            UnifiedPlanProjectionTestAccess::make_system_source(
                nested_snapshot, nullptr, no_issues,
                mismatched_needed_work);
    const UpgradeAllOperationPreparedSnapshot matching_outer{
        nested_snapshot, {}, {}};
    const UpgradeAllOperationProjectionAuthority
        mismatched_needed_authority =
            UnifiedPlanProjectionTestAccess::make_upgrade_all(
                matching_outer,
                mismatched_needed_system_authority);
    expect_invalid_argument(
        [&mismatched_needed_authority, &query, &update_preflight,
         &upgrade_issues] {
            (void)project_upgrade_all_unified_plan(
                UpgradeAllUnifiedPlanProjectionInput{
                    std::cref(mismatched_needed_authority),
                    std::cref(query),
                    std::cref(update_preflight),
                    std::cref(upgrade_issues)});
        },
        "upgrade-all source work with a different needed policy");
}

void test_repository_configuration_correlation() {
    const AurUpdateQueryResult query = update_query();
    const AurUpdateExecutionPreflight matching =
        executable_update_preflight(build_plan_fixture());
    const std::unique_ptr<UnifiedPlanProjection> matching_projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(query), std::cref(matching), false});
    const UnifiedPlanObservation& matching_observation =
        require_observation(
            *matching_projection,
            "matching repository configuration");
    expect(
        matching_observation.configured_repository_order() != nullptr &&
            &matching_observation.configured_repository_order()
                    ->configured_order() ==
                &matching.build_plan
                     ->configured_repository_order.value(),
        "actual BuildPlan repository configuration was not borrowed");

    AurUpdateExecutionPreflight wrong_index =
        executable_update_preflight(build_plan_fixture());
    std::get<RepositoryProviderOrigin>(
        wrong_index.build_plan->provided.front().provider.origin)
        .configured_order = 0;
    expect_invalid_argument(
        [&query, &wrong_index] {
            (void)project_aur_update_unified_plan(
                AurUpdateUnifiedPlanProjectionInput{
                    std::cref(query), std::cref(wrong_index),
                    false});
        },
        "same repository name with different configured index");

    AurUpdateExecutionPreflight missing_repository =
        executable_update_preflight(build_plan_fixture());
    auto& missing_origin = std::get<RepositoryProviderOrigin>(
        missing_repository.build_plan->provided.front().provider.origin);
    missing_origin.repository_name = "unconfigured";
    missing_origin.configured_order = 1;
    expect_invalid_argument(
        [&query, &missing_repository] {
            (void)project_aur_update_unified_plan(
                AurUpdateUnifiedPlanProjectionInput{
                    std::cref(query),
                    std::cref(missing_repository), false});
        },
        "provider repository absent from resolution configuration");

    AurUpdateExecutionPreflight unknown_configuration =
        executable_update_preflight(build_plan_fixture());
    unknown_configuration.build_plan->configured_repository_order.reset();
    expect_invalid_argument(
        [&query, &unknown_configuration] {
            (void)project_aur_update_unified_plan(
                AurUpdateUnifiedPlanProjectionInput{
                    std::cref(query),
                    std::cref(unknown_configuration), false});
        },
        "repository provider with unknown configuration");
}

void test_actual_production_blocked_results() {
    PreparedRootPackageInstall root_ready =
        make_root_prepared(false, true, std::nullopt);
    root_ready.aur_build_plan->dependency_edges.front()
        .constraint_evaluation = ConstraintEvaluation::unsatisfied();
    RootPackageInstallPreparationFailure root_failure;
    root_failure.details.push_back(RootPackageInstallPreparationIssue{
        RootPackageInstallPreparationIssueKind::
            BuildPlanPreparationFailed,
        std::nullopt, std::nullopt, std::nullopt,
        "fixture production BuildPlan blocker"});
    root_failure.discovery_snapshot =
        std::move(root_ready.discovery_snapshot);
    root_failure.routing_projection =
        std::move(root_ready.routing_projection);
    root_failure.aur_build_plan =
        std::move(root_ready.aur_build_plan);
    const std::unique_ptr<UnifiedPlanProjection> root_projection =
        project_root_package_unified_plan(
            RootPackageUnifiedPlanProjectionInput{
                std::cref(root_failure)});
    const UnifiedPlanObservation& root_observation =
        require_observation(*root_projection, "blocked root result");
    expect(
        root_observation.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            root_observation.transaction_intents().empty() &&
            root_observation.required_artifacts().empty() &&
            std::any_of(
                root_observation.blockers().begin(),
                root_observation.blockers().end(),
                [](const UnifiedPlanBlocker& blocker) {
                    return std::holds_alternative<
                        RootPackagePreparationUnifiedPlanBlocker>(
                        blocker);
                }) &&
            std::any_of(
                root_observation.blockers().begin(),
                root_observation.blockers().end(),
                [](const UnifiedPlanBlocker& blocker) {
                    return std::holds_alternative<
                        ConstraintFailureUnifiedPlanBlocker>(
                        blocker);
                }),
        "actual root preparation failure lost typed blockers");
    expect(
        !has_phase(
            root_observation,
            UnifiedPlanObservationPhase::RepositoryTransaction) &&
            !has_phase(
                root_observation,
                UnifiedPlanObservationPhase::SourceBuild),
        "blocked root result exposed mutation phases");

    const LocalSourceRoot source_root = open_local_source_root(
        "tests/fixtures/unified-plan-local-blocked");
    const LocalPackageMetadata* metadata =
        source_root.metadata().parse_result()->metadata();
    const LocalBuildPlan blocked_local_plan =
        resolve_local_build_plan(*metadata, "unsupported-architecture");
    const std::unique_ptr<UnifiedPlanProjection> local_projection =
        project_local_source_unified_plan(
            LocalSourceUnifiedPlanProjectionInput{
                LocalSourceBuildPlanFailureProjectionInput{
                    std::cref(source_root),
                    std::cref(blocked_local_plan)},
                false});
    const UnifiedPlanObservation& local_observation =
        require_observation(*local_projection, "blocked local result");
    expect(
        local_observation.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            !blocked_local_plan.failures().empty() &&
            local_observation.transaction_intents().empty(),
        "actual LocalBuildPlan failure did not reach Blocked observation");

    AurUpdateQueryResult route_query = update_query();
    AurUpdateExecutionTarget route_target;
    route_target.update_plan_index = 0;
    route_target.update = update_entry();
    route_target.status = AurUpdateExecutionTargetStatus::Unsupported;
    route_target.issues.push_back(AurUpdateExecutionIssue{
        AurUpdateExecutionReason::BuildPlanInconsistent,
        "suite-child", "suite-base", std::nullopt,
        "fixture route preflight blocker"});
    const AurUpdateExecutionPreflight route_preflight{
        {route_target}, std::nullopt, DevelRequiresCheckPolicy::BlockOperation};
    const std::unique_ptr<UnifiedPlanProjection> route_projection =
        project_aur_update_unified_plan(
            AurUpdateUnifiedPlanProjectionInput{
                std::cref(route_query),
                std::cref(route_preflight), false});
    const UnifiedPlanObservation& route_observation =
        require_observation(*route_projection, "route preflight result");
    expect(
        route_observation.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            route_observation.transaction_intents().empty(),
        "actual route preflight issue was not projected as Blocked");

    SystemSourceUpgradePreparedSnapshot nested_snapshot;
    nested_snapshot.registered_sources.push_back(registered_source(
        0, "repo-child", "repo-base",
        SourceBuildSourceKind::Repository));
    SystemSourceUpgradeResult nested_failure;
    nested_failure.status =
        SystemSourceUpgradeStatus::BlockedBeforeMutation;
    nested_failure.stopped_phase = SystemSourceUpgradePhase::Preparation;
    nested_failure.prepared_snapshot = nested_snapshot;
    nested_failure.registered_source_results.push_back(
        not_attempted_source_result(
            nested_failure.prepared_snapshot.registered_sources
                .front()));
    nested_failure.issues.push_back(system_issue(
        SystemSourceUpgradeIssueKind::SourceWorkItemPreparationFailed,
        SystemSourceUpgradeIssueImpact::BlocksExecution,
        SystemSourceUpgradePhase::Preparation));

    UpgradeAllOperationResult upgrade_failure;
    upgrade_failure.status =
        UpgradeAllOperationStatus::BlockedBeforeMutation;
    upgrade_failure.stopped_phase = UpgradeAllOperationPhase::Preparation;
    upgrade_failure.prepared_snapshot =
        UpgradeAllOperationPreparedSnapshot{nested_snapshot, {}, {}};
    upgrade_failure.system_source = std::move(nested_failure);
    UpgradeAllOperationIssue nested_issue;
    nested_issue.kind =
        UpgradeAllOperationIssueKind::SystemSourcePhaseIncomplete;
    nested_issue.phase = UpgradeAllOperationPhase::Preparation;
    nested_issue.diagnostic = "fixture nested source failure";
    upgrade_failure.issues.push_back(std::move(nested_issue));

    upgrade_failure.prepared_snapshot.system_source
        .registered_sources.front()
        .resolved_package_base = "other-blocked-invocation-base";
    expect_invalid_argument(
        [&upgrade_failure] {
            (void)project_upgrade_all_unified_plan(
                UpgradeAllUnifiedPlanProjectionInput{
                    std::cref(upgrade_failure)});
        },
        "blocked upgrade-all nested different invocation");
    upgrade_failure.prepared_snapshot.system_source
        .registered_sources.front()
        .resolved_package_base = "repo-base";

    const std::unique_ptr<UnifiedPlanProjection> upgrade_projection =
        project_upgrade_all_unified_plan(
            UpgradeAllUnifiedPlanProjectionInput{
                std::cref(upgrade_failure)});
    const UnifiedPlanObservation& upgrade_observation =
        require_observation(
            *upgrade_projection, "blocked upgrade-all result");
    expect(
        upgrade_observation.status() ==
                UnifiedPlanObservationStatus::Blocked &&
            upgrade_observation.transaction_intents().empty() &&
            upgrade_observation.required_artifacts().empty() &&
            has_existing_phase(
                upgrade_observation,
                UnifiedPlanObservationPhase::ExecutionProjection,
                UpgradeAllOperationPhase::Preparation) &&
            !has_phase(
                upgrade_observation,
                UnifiedPlanObservationPhase::SourceBuild),
        "upgrade-all nested production failure exposed mutation intent");
}

void test_system_aur_auto_projection_separates_current_observation() {
    SystemAurUpdateDryRunObservation combined =
        system_aur_auto_observation(
            ready_filtered_aur_observation(), true);

    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
        projection = project_system_aur_update_unified_plan(combined);
    expect(
        projection->status() ==
                SystemAurUpdateUnifiedPlanStatus::Ready &&
            projection->mode() ==
                SystemAurUpdateUnifiedPlanMode::Auto &&
            projection->requires_check_attentions().empty(),
        "system/AUR Auto projection did not remain Ready");
    expect(
        projection->phases() ==
            std::vector<SystemAurUpdateUnifiedPlanPhase>{
                SystemAurUpdateUnifiedPlanPhase::
                    RepositorySystemTransactionIntent,
                SystemAurUpdateUnifiedPlanPhase::
                    CurrentForeignInventoryObservation,
                SystemAurUpdateUnifiedPlanPhase::
                    CurrentNormalAurAssessment,
                SystemAurUpdateUnifiedPlanPhase::
                    PotentialLaterAurTransactions},
        "system/AUR Auto projection lost its four typed phases");
    expect(
        projection->freshness() ==
                std::optional<SystemAurUpdateUnifiedPlanFreshness>{
                    SystemAurUpdateUnifiedPlanFreshness::
                        CurrentInstalledState} &&
            projection->actual_refresh() ==
                std::optional<SystemAurUpdateUnifiedPlanActualRefresh>{
                    SystemAurUpdateUnifiedPlanActualRefresh::
                        AfterRepositorySuccess} &&
            projection->transaction_relationship() ==
                std::optional<
                    SystemAurUpdateUnifiedPlanTransactionRelationship>{
                    SystemAurUpdateUnifiedPlanTransactionRelationship::
                        SeparateSequentialTransactions},
        "system/AUR Auto projection flattened freshness or sequencing");

    const UnifiedPlanObservation& repository = require_observation(
        projection->repository_projection(),
        "system/AUR repository child");
    expect(
        repository.status() == UnifiedPlanObservationStatus::Ready &&
            repository.transaction_intents().size() == 1 &&
            repository.required_artifacts().empty() &&
            has_repository_system_upgrade_intent(repository),
        "repository child lost its isolated system-upgrade intent");
    const RepositoryPackageTransactionIntent* repository_transaction =
        repository_transaction_intent(
            repository.transaction_intents().front());
    expect(
        repository_transaction != nullptr &&
            repository_transaction->policy.needed,
        "repository child lost the exact --needed policy");

    expect(
        projection->aur_projection() != nullptr,
        "system/AUR Auto projection lost its AUR child");
    const UnifiedPlanObservation& aur = require_observation(
        *projection->aur_projection(), "system/AUR current-state child");
    expect(
        aur.status() == UnifiedPlanObservationStatus::Ready &&
            !aur.transaction_intents().empty() &&
            !aur.required_artifacts().empty(),
        "current-state AUR child lost its later mutation intents");

    bool has_later_provider_transaction = false;
    bool has_later_artifact_transaction = false;
    for(const UnifiedPlanTransactionIntent& transaction :
        aur.transaction_intents()) {
        expect(
            transaction_stage(transaction) ==
                UnifiedPlanTransactionIntentStage::LaterNormalAur,
            "AUR child transaction was merged into the repository stage");
        if(const RepositoryPackageTransactionIntent* repository_intent =
               repository_transaction_intent(transaction);
           repository_intent != nullptr) {
            for(const RepositoryInstallIntentTarget& target :
                repository_intent->targets) {
                expect(
                    !std::holds_alternative<
                        RepositorySystemUpgradeIntent>(target),
                    "later provider transaction retained a system-upgrade target");
                has_later_provider_transaction =
                    has_later_provider_transaction ||
                    std::holds_alternative<
                        RepositoryProviderInstallIntent>(target);
            }
            continue;
        }
        if(std::holds_alternative<
               SourceBuiltArtifactInstallBoundaryIntent>(transaction)) {
            has_later_artifact_transaction = true;
        }
    }
    expect(
        has_later_provider_transaction &&
            has_later_artifact_transaction &&
            !has_repository_system_upgrade_intent(aur),
        "later provider/artifact intents were not kept outside the system transaction");
}

void test_system_aur_multi_unit_provider_scope_projection() {
    SystemAurUpdateDryRunObservation combined =
        system_aur_auto_observation(
            multi_unit_partial_provider_filtered_aur_observation());
    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
        projection = project_system_aur_update_unified_plan(combined);
    expect(
        projection->status() ==
                SystemAurUpdateUnifiedPlanStatus::Ready &&
            projection->aur_projection() != nullptr,
        "multi-unit partial-provider projection was not Ready");

    const UnifiedPlanObservation& aur = require_observation(
        *projection->aur_projection(),
        "multi-unit partial-provider AUR child");
    std::size_t provider_target_count = 0;
    std::size_t artifact_target_count = 0;
    for(const UnifiedPlanTransactionIntent& transaction :
        aur.transaction_intents()) {
        expect(
            transaction_stage(transaction) ==
                UnifiedPlanTransactionIntentStage::LaterNormalAur,
            "multi-unit transaction escaped the later AUR stage");
        if(const RepositoryPackageTransactionIntent* repository =
               repository_transaction_intent(transaction);
           repository != nullptr) {
            provider_target_count +=
                static_cast<std::size_t>(std::count_if(
                    repository->targets.begin(),
                    repository->targets.end(),
                    [](const RepositoryInstallIntentTarget& target) {
                        return std::holds_alternative<
                            RepositoryProviderInstallIntent>(target);
                    }));
            continue;
        }
        if(const auto* artifacts =
               std::get_if<SourceBuiltArtifactInstallBoundaryIntent>(
                   &transaction);
           artifacts != nullptr) {
            artifact_target_count += artifacts->targets.size();
        }
    }
    expect(
        aur.status() == UnifiedPlanObservationStatus::Ready &&
            aur.required_artifacts().size() == 2 &&
            provider_target_count == 1 && artifact_target_count == 2,
        "multi-unit provider scope was flattened across AUR build units");
}

void test_system_aur_noop_and_repo_only_do_not_false_noop() {
    SystemAurUpdateDryRunObservation no_updates =
        system_aur_auto_observation(no_op_filtered_aur_observation());
    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
        no_updates_projection =
            project_system_aur_update_unified_plan(no_updates);
    expect(
        no_updates_projection->status() ==
                SystemAurUpdateUnifiedPlanStatus::Ready &&
            no_updates_projection->requires_check_attentions().empty() &&
            has_repository_system_upgrade_intent(require_observation(
                no_updates_projection->repository_projection(),
                "system/AUR no-update repository child")),
        "current AUR NoOp falsely collapsed the combined operation");
    expect(
        no_updates_projection->aur_projection() != nullptr,
        "current AUR NoOp lost its explicit child observation");
    const UnifiedPlanObservation& no_update_aur = require_observation(
        *no_updates_projection->aur_projection(),
        "system/AUR no-update AUR child");
    expect(
        no_update_aur.status() == UnifiedPlanObservationStatus::NoOp &&
            no_update_aur.transaction_intents().empty(),
        "current AUR NoOp fabricated a later transaction");

    SystemAurUpdateDryRunObservation repo_only{
        system_aur_repo_only_request(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        {},
        std::nullopt,
        {}};
    expect(
        repo_only.request.mode() ==
                SystemAurUpdateDryRunMode::RepoOnly &&
            !repo_only.aur_observation_basis.has_value() &&
            !repo_only.actual_authority_refresh.has_value() &&
            !repo_only.explicit_source_satisfaction.has_value() &&
            !repo_only.saved_source_preference_policy.has_value() &&
            !repo_only.devel_requires_check_policy.has_value() &&
            !repo_only.repository_configuration.has_value() &&
            repo_only.foreign_inventory.empty() &&
            !repo_only.aur_observation.has_value() &&
            repo_only.issues.empty(),
        "RepoOnly fixture retained AUR authority");
    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
        repo_only_projection =
            project_system_aur_update_unified_plan(repo_only);
    expect(
        repo_only_projection->status() ==
                SystemAurUpdateUnifiedPlanStatus::Ready &&
            repo_only_projection->mode() ==
                SystemAurUpdateUnifiedPlanMode::RepoOnly &&
            repo_only_projection->phases() ==
                std::vector<SystemAurUpdateUnifiedPlanPhase>{
                    SystemAurUpdateUnifiedPlanPhase::
                        RepositorySystemTransactionIntent} &&
            repo_only_projection->aur_projection() == nullptr &&
            repo_only_projection->requires_check_attentions().empty() &&
            !repo_only_projection->freshness().has_value() &&
            !repo_only_projection->actual_refresh().has_value() &&
            !repo_only_projection->transaction_relationship().has_value(),
        "RepoOnly projection retained AUR observation authority");
    expect(
        has_repository_system_upgrade_intent(require_observation(
            repo_only_projection->repository_projection(),
            "RepoOnly repository child")),
        "RepoOnly projection lost its repository system intent");
}

void test_system_aur_blockers_preserve_repository_child() {
    SystemAurUpdateDryRunObservation blocked =
        system_aur_auto_observation(blocked_filtered_aur_observation());
    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
        blocked_projection =
            project_system_aur_update_unified_plan(blocked);
    expect(
        blocked_projection->status() ==
                SystemAurUpdateUnifiedPlanStatus::Blocked &&
            has_repository_system_upgrade_intent(require_observation(
                blocked_projection->repository_projection(),
                "blocked system/AUR repository child")),
        "AUR blocker erased the independent repository intent");
    expect(
        blocked_projection->aur_projection() != nullptr,
        "blocked system/AUR projection lost its AUR child");
    const UnifiedPlanObservation& blocked_aur = require_observation(
        *blocked_projection->aur_projection(),
        "blocked system/AUR AUR child");
    expect(
        blocked_aur.status() == UnifiedPlanObservationStatus::Blocked &&
            blocked_aur.transaction_intents().empty() &&
            count_route_preflight_blockers<AurUpdateExecutionIssue>(
                blocked_aur) == 1,
        "current-state provider ambiguity did not remain typed Blocked");

    SystemAurUpdateDryRunObservation query_failure{
        system_aur_auto_request(),
        SystemAurUpdateDryRunAurObservationBasis::CurrentInstalledState,
        SystemAurUpdateDryRunActualAuthorityRefresh::AfterRepositorySuccess,
        NoExplicitSourceSatisfaction{},
        SavedSourcePreferencePolicy::Ignore,
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        PacmanRepositoryConfiguration{},
        ForeignPackageInventory{InstalledPackageMetadata{
            "suite-child", "1.0", InstalledPackageReason::Explicit}},
        std::nullopt,
        {SystemAurUpdateDryRunIssue{
            SystemAurUpdateDryRunIssueKind::AurQueryFailure,
            std::nullopt, "fixture exact AUR query failure"}}};
    expect(
        query_failure.issues.size() == 1 &&
            query_failure.issues.front().kind ==
                SystemAurUpdateDryRunIssueKind::AurQueryFailure &&
            !query_failure.aur_observation.has_value(),
        "system/AUR query failure fixture lost typed failure authority");
    const std::unique_ptr<SystemAurUpdateUnifiedPlanProjection>
        failure_projection =
            project_system_aur_update_unified_plan(query_failure);
    const UnifiedPlanObservation& failure_aur = require_observation(
        *failure_projection->aur_projection(),
        "query-failed system/AUR AUR child");
    expect(
        failure_projection->status() ==
                SystemAurUpdateUnifiedPlanStatus::Blocked &&
            has_repository_system_upgrade_intent(require_observation(
                failure_projection->repository_projection(),
                "query-failed system/AUR repository child")) &&
            failure_aur.status() == UnifiedPlanObservationStatus::Blocked &&
            count_route_preflight_blockers<SystemAurUpdateDryRunIssue>(
                failure_aur) == 1,
        "typed AUR query failure erased or contaminated the repository child");
}

void test_system_aur_projection_rejects_malformed_authority() {
    auto preflight = system_aur_auto_observation(ready_filtered_aur_observation());
    preflight.preflight_version_lock_correlation.emplace();
    preflight.preflight_version_lock_correlation->basis = CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation;
    preflight.preflight_version_lock_correlation->observation.emplace();
    preflight.preflight_version_lock_correlation->observation->status = CrossSourceVersionLockObservationStatus::Partial;
    const auto projected = project_system_aur_update_unified_plan(preflight);
    expect(projected->status() == SystemAurUpdateUnifiedPlanStatus::Ready &&
               preflight.preflight_version_lock_correlation->observation->status == CrossSourceVersionLockObservationStatus::Partial,
           "Supplemental partial preflight changed primary planning status or lost its status");
    preflight.preflight_version_lock_correlation->basis = CrossSourceVersionLockObservationBasis::AfterRepositoryFailure;
    expect_invalid_argument([&preflight] {
        (void)project_system_aur_update_unified_plan(preflight);
    },
                            "dry-run accepted post-repository correlation");

    SystemAurUpdateDryRunObservation strict =
        system_aur_auto_observation(ready_filtered_aur_observation());
    strict.saved_source_preference_policy =
        SavedSourcePreferencePolicy::Strict;
    expect_invalid_argument(
        [&strict] {
            (void)project_system_aur_update_unified_plan(strict);
        },
        "system/AUR Strict saved-preference policy");

    SystemAurUpdateDryRunObservation missing_devel_policy =
        system_aur_auto_observation(ready_filtered_aur_observation());
    missing_devel_policy.devel_requires_check_policy.reset();
    expect_invalid_argument(
        [&missing_devel_policy] {
            (void)project_system_aur_update_unified_plan(
                missing_devel_policy);
        },
        "system/AUR missing RequiresCheck policy");

    SystemAurUpdateDryRunObservation unknown_devel_policy =
        system_aur_auto_observation(ready_filtered_aur_observation());
    unknown_devel_policy.devel_requires_check_policy =
        static_cast<DevelRequiresCheckPolicy>(-1);
    expect_invalid_argument(
        [&unknown_devel_policy] {
            (void)project_system_aur_update_unified_plan(
                unknown_devel_policy);
        },
        "system/AUR unknown RequiresCheck policy");

    SystemAurUpdateDryRunObservation mismatched_devel_policy =
        system_aur_auto_observation(ready_filtered_aur_observation());
    mismatched_devel_policy.aur_observation
        ->devel_requires_check_policy =
        DevelRequiresCheckPolicy::BlockOperation;
    expect_invalid_argument(
        [&mismatched_devel_policy] {
            (void)project_system_aur_update_unified_plan(
                mismatched_devel_policy);
        },
        "system/AUR mismatched RequiresCheck policy");

    FilteredAurUpdateObservation malformed_skip =
        no_op_filtered_aur_observation();
    malformed_skip.preflight.targets.front().skip_kind =
        AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck;
    SystemAurUpdateDryRunObservation malformed_skip_observation =
        system_aur_auto_observation(std::move(malformed_skip));
    expect_invalid_argument(
        [&malformed_skip_observation] {
            (void)project_system_aur_update_unified_plan(
                malformed_skip_observation);
        },
        "system/AUR independent RequiresCheck skip snapshot");

    FilteredAurUpdateObservation hidden_required_payload =
        ready_filtered_aur_observation();
    AurUpdateExecutionIssue hidden_required_issue;
    hidden_required_issue.required_devel_target_blocker =
        AurUpdateRequiredDevelTargetBlocker{
            AurUpdateRequiredDevelTargetRelation::AurExactDependency,
            1,
            "required-devel-git",
            DevelRequiresCheckReason::SuffixCandidateOnly};
    hidden_required_payload.source_build_observation
        ->affected_update_targets.front()
        .issues.push_back(std::move(hidden_required_issue));
    expect(
        has_valid_aur_update_execution_policy_snapshot(
            hidden_required_payload.preflight),
        "Hidden required-devel observation drift contaminated preflight authority");
    SystemAurUpdateDryRunObservation hidden_required_observation =
        system_aur_auto_observation(
            std::move(hidden_required_payload));
    expect_invalid_argument(
        [&hidden_required_observation] {
            (void)project_system_aur_update_unified_plan(
                hidden_required_observation);
        },
        "system/AUR executable hidden required-devel payload");

    SystemAurUpdateDryRunObservation identity_mismatch =
        system_aur_auto_observation(ready_filtered_aur_observation());
    identity_mismatch.foreign_inventory.front().name =
        "other-installed-identity";
    expect_invalid_argument(
        [&identity_mismatch] {
            (void)project_system_aur_update_unified_plan(
                identity_mismatch);
        },
        "system/AUR inventory/query identity mismatch");

    SystemAurUpdateDryRunObservation repo_only_with_aur_authority{
        system_aur_repo_only_request(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        SavedSourcePreferencePolicy::Ignore,
        std::nullopt,
        std::nullopt,
        {},
        std::nullopt,
        {}};
    expect_invalid_argument(
        [&repo_only_with_aur_authority] {
            (void)project_system_aur_update_unified_plan(
                repo_only_with_aur_authority);
        },
        "RepoOnly observation with retained AUR authority");

    SystemAurUpdateDryRunObservation missing_typed_failure{
        system_aur_auto_request(),
        SystemAurUpdateDryRunAurObservationBasis::CurrentInstalledState,
        SystemAurUpdateDryRunActualAuthorityRefresh::AfterRepositorySuccess,
        NoExplicitSourceSatisfaction{},
        SavedSourcePreferencePolicy::Ignore,
        DevelRequiresCheckPolicy::SkipIndependentTarget,
        PacmanRepositoryConfiguration{},
        {},
        std::nullopt,
        {}};
    expect_invalid_argument(
        [&missing_typed_failure] {
            (void)project_system_aur_update_unified_plan(
                missing_typed_failure);
        },
        "system/AUR missing AUR authority without typed failure");
}

void test_missing_system_source_plan_is_rejected() {
    SystemSourceUpgradePreparedSnapshot snapshot;
    snapshot.registered_sources.push_back(registered_source(
        0, "suite-child", "suite-base", SourceBuildSourceKind::Aur));
    const std::vector<ProductionSourceBuildWorkItem> work_items{
        source_work_item(
            "suite-base", "suite-child",
            DesiredInstallReason::Explicit,
            ArtifactLifecycleIntent::SingularCompatibility,
            SourceBuildSourceKind::Aur)};
    const std::vector<SystemSourceUpgradeIssue> issues;
    const SystemSourceUpgradeProjectionAuthority authority =
        UnifiedPlanProjectionTestAccess::make_system_source(
            snapshot, nullptr, issues, work_items);
    expect_invalid_argument(
        [&authority] {
            (void)project_system_source_upgrade_unified_plan(
                SystemSourceUpgradeUnifiedPlanProjectionInput{
                    std::cref(authority)});
        },
        "AUR registered source without BuildPlan");
}

} // namespace

int main() {
    try {
        test_projection_lifetime_and_root_authorities();
        test_root_repository_order_known_and_unknown();
        test_root_and_update_correlation_fail_closed();
        test_aur_update_install_reason_parity();
        test_build_plan_partial_failure_remains_typed();
        test_local_and_no_op_phases();
        test_aur_update_source_preparation_blocker();
        test_aur_update_blocking_preflight_wrapper_is_not_duplicated();
        test_required_devel_blocker_wrapper_identity_is_lossless();
        test_malformed_blocking_preflight_wrappers_are_retained();
        test_strict_preparation_blockers_are_not_suppressed();
        test_fetch_and_remote_source_build_adapters();
        test_sync_install_preparation_adapters();
        test_sync_duplicate_repository_source_correlation();
        test_actual_prepared_source_work_is_artifact_authority();
        test_system_issue_impact_and_blocked_phases();
        test_partial_failures_and_upgrade_all_authorities();
        test_full_identity_correlation_fail_closed();
        test_repository_configuration_correlation();
        test_actual_production_blocked_results();
        test_system_aur_auto_projection_separates_current_observation();
        test_system_aur_multi_unit_provider_scope_projection();
        test_system_aur_noop_and_repo_only_do_not_false_noop();
        test_system_aur_blockers_preserve_repository_child();
        test_system_aur_projection_rejects_malformed_authority();
        test_missing_system_source_plan_is_rejected();
        std::cout << "unified plan projection tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "unified plan projection test failed: " << error.what()
                  << '\n';
        return 1;
    }
}
