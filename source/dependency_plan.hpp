#pragma once

#include "aur_rpc.hpp"
#include "dependency_constraint.hpp"
#include "dependency_provider.hpp"
#include "package_constraint_metadata.hpp"
#include "package_relation_assessment.hpp"
#include "provider_selection.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// 複数 provider がある依存。明示選択されない場合はここで止め、暗黙選択しない。
struct AmbiguousProvidedDependency {
    std::string dependency;
    std::vector<ProvidedDependency> candidates;
};

// provider候補のpresentation/inputはCLI ownerへ委ね、plan coreは
// source-aware candidateと選択結果だけを受け取る。
using ProviderSelectionCallback = std::function<std::optional<ProviderSelectionSet>(
    const std::string& dependency,
    const std::vector<ProvidedDependency>& candidates)>;

struct SelectedProvidedDependency {
    std::string dependency;
    ProvidedDependency provider;
};

// 依存を official repo / AUR / provider / unknown に分けた結果。
struct DependencyClassification {
    std::vector<std::string> installed;
    std::vector<std::string> repo;
    std::vector<std::string> aur;
    std::vector<std::string> provided;
    std::vector<SelectedProvidedDependency> selected_providers;
    std::vector<AmbiguousProvidedDependency> ambiguous_providers;
    std::vector<std::string> unknown;
};

enum class ProviderResolutionKind {
    Unique,
    UserSelected
};

enum class PackageRole {
    Root,
    RuntimeDependency,
    BuildDependency,
    CheckDependency
};

enum class DependencyKind {
    Installed,
    Repo,
    Aur,
    Local,
    Provided,
    AmbiguousProvider,
    Unknown
};

enum class DesiredInstallReason {
    Explicit,
    Dependency
};

// AUR metadata上のdependency categoryを、raw specificationと対応付けたもの。
struct TypedPackageDependency {
    std::string specification;
    PackageRole role;
    // Metadata adapters may retain a parsed requirement here. Legacy AUR
    // inputs intentionally remain raw until their Slice 4 trust boundary.
    std::optional<DependencyRequirement> requirement = std::nullopt;
};

// CLI invocation内のroot target。indexは入力順を失わないためのidentityの一部。
struct RootTargetIdentity {
    std::size_t invocation_index;
    std::string requested_name;

    bool operator==(const RootTargetIdentity&) const = default;
};

// ordinary metadata resolution failureを、confirmed absenceやschema failureと区別して保持する。
enum class BuildPlanResolutionFailureKind {
    InstalledPackageMetadataUnavailable,
    RepositoryMetadataUnavailable,
    AurPackageMetadataUnavailable,
    ProviderSearchUnavailable,
    ProviderCandidateMetadataUnavailable
};

struct BuildPlanResolutionFailure {
    BuildPlanResolutionFailureKind kind;
    std::optional<std::string> parent_package_name;
    std::optional<std::string> parent_package_base;
    std::string subject;
    std::optional<std::string> dependency_specification;
    std::vector<RootTargetIdentity> roots;
    std::string diagnostic;

    bool operator==(const BuildPlanResolutionFailure&) const = default;
};

// source package nameごとに、plan内で担う全roleと到達元rootを集約する。
struct PlannedPackageTarget {
    std::string package_name;
    std::string package_base;
    std::vector<PackageRole> roles;
    std::vector<RootTargetIdentity> roots;
};

// BuildPlan edgeが解決時に採用したsource authority。package nameだけへ
// flattenせず、preflightとactual routeのidentity照合に使う。
struct AurResolvedDependencyCandidate {
    std::string package_name;
    std::string package_base;
    ObservedVersion package_version;

    bool operator==(const AurResolvedDependencyCandidate&) const = default;
};

struct LocalResolvedDependencyCandidate {
    std::string package_name;
    std::string package_base;
    std::optional<ProviderCapability> provided_capability;
    ObservedVersion observed_version;

    bool operator==(const LocalResolvedDependencyCandidate&) const = default;
};

struct ProviderResolvedDependencyCandidate {
    ProvidedDependency provider;
    ObservedVersion provided_version;

    bool operator==(const ProviderResolvedDependencyCandidate&) const = default;
};

using ResolvedDependencyCandidate = std::variant<
    InstalledExactPackage,
    RepositoryExactPackage,
    AurResolvedDependencyCandidate,
    LocalResolvedDependencyCandidate,
    ProviderResolvedDependencyCandidate>;

// dependency宣言と、その最終的な解決結果を結び付ける。
struct BuildPlanDependencyEdge {
    std::string parent_package_name;
    std::string parent_package_base;
    std::string dependency_spec;
    PackageRole role;
    DependencyKind kind = DependencyKind::Unknown;
    std::optional<std::string> resolved_package_name;
    std::optional<std::string> resolved_package_base;
    std::optional<ProvidedDependency> resolved_provider;
    ProviderResolutionKind provider_resolution =
        ProviderResolutionKind::Unique;
    // AUR RPC / local metadata trust boundaryで一度だけ構成された値。
    std::optional<DependencyRequirement> requirement = std::nullopt;
    std::optional<ResolvedDependencyCandidate> resolved_candidate =
        std::nullopt;
    std::optional<ConstraintEvaluation> constraint_evaluation = std::nullopt;
};

// recursive dependency tree の 1 node。表示と循環検出結果を同じ単位で持つ。
struct RecursiveDependencyNode {
    std::string dependency;
    std::string package_name;
    std::string package_base;
    std::optional<ProvidedDependency> provided_by;
    std::vector<ProvidedDependency> provider_candidates;
    DependencyKind kind = DependencyKind::Unknown;
    ProviderResolutionKind provider_resolution =
        ProviderResolutionKind::Unique;
    bool already_visited = false;
    bool max_depth_reached = false;
    std::vector<RecursiveDependencyNode> children;
};

// build plan 内で、同じ PackageBase から生成される package 群を束ねる。
struct BuildPlanEntry {
    std::string package_base;
    std::vector<std::string> package_names;
};

// install 実行時に、PackageBase とは別の package name を明示選択する必要がある target。
struct BuildPlanSplitPackageTarget {
    std::string package_base;
    std::string package_name;
};

// build plan 上で provider により解決された依存を記録する。
struct BuildPlanProvidedDependency {
    std::string dependency;
    ProvidedDependency provider;
    ProviderResolutionKind resolution = ProviderResolutionKind::Unique;
};

// Candidate observations remain available for diagnostics even when a source
// failure means the complete provider set cannot be proven. Selection never
// consumes this partial set.
struct IncompleteProviderCandidateSet {
    std::string dependency;
    std::vector<ProvidedDependency> observed_candidates;
    ObservedVersionUnknownReason reason;

    bool operator==(const IncompleteProviderCandidateSet&) const = default;
};

// Raw relation strings remain for compatibility/presentation only. Production
// safety is exclusively projected from relation_assessments below.
struct BuildPlanMetadataRisk {
    std::string package_name;
    std::string package_base;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
};

// AUR build / fetch の順序、未解決依存、循環検出結果をまとめる計画。
struct BuildPlan {
    std::vector<BuildPlanEntry> order;
    std::vector<RootTargetIdentity> root_targets;
    std::vector<PlannedPackageTarget> package_targets;
    std::vector<PlannedPackageRelationObservation>
        planned_relation_observations;
    std::vector<PackageRelationAssessment> relation_assessments;
    std::vector<BuildPlanDependencyEdge> dependency_edges;
    std::vector<BuildPlanResolutionFailure> resolution_failures;
    std::vector<BuildPlanSplitPackageTarget> split_package_targets;
    std::vector<BuildPlanProvidedDependency> provided;
    std::vector<BuildPlanMetadataRisk> metadata_risks;
    std::vector<AmbiguousProvidedDependency> ambiguous_providers;
    // Interactive provider selection が明示的に取消された dependency。
    // candidate ambiguity は残しつつ、decision の typed evidence を失わない。
    std::vector<std::string> cancelled_provider_dependencies;
    std::vector<std::string> unresolved;
    std::vector<std::string> cycles;
    std::vector<IncompleteProviderCandidateSet>
        incomplete_provider_candidate_sets;
    // strict dependency/provider resolutionが実際に問い合わせたpacman
    // repository configuration。nulloptはauthority未問い合わせを表す。
    std::optional<std::vector<std::string>> configured_repository_order =
        std::nullopt;
};

enum class PlanConstruction {
    Constructed,
    Failed,
};

enum class PlanCompleteness {
    Complete,
    Incomplete,
    Unknown,
};

enum class ProviderDecision {
    Unique,
    Selected,
    Ambiguous,
    Cancelled,
    Unavailable,
};

enum class ExecutionCapability {
    Fetch,
    Build,
    Install,
};

enum class ExecutionReadinessState {
    NotAssessed,
    Ready,
    RequiresCheck,
    Blocked,
    Unknown,
};

enum class PlanRequiredAction {
    None,
    CorrectPlanAuthority,
    ResolveDependency,
    SelectProvider,
    ObtainMetadata,
    ReviewDeclaredRelations,
    UsePackageBaseSetLifecycle,
};

enum class PlanConstraintAuthorityIssueKind {
    MissingRequirement,
    RequirementIdentityChanged,
    SourceIdentityInconsistent,
    MissingEvaluation,
    InvalidEvaluation,
    ConflictingEvaluation,
};

struct PlanConstraintAuthorityReason {
    PlanConstraintAuthorityIssueKind kind;
    std::size_t edge_index;
    std::string dependency_specification;
    std::optional<ConstraintSatisfaction> satisfaction;
};

struct PlanSelectedProviderIdentityConflictReason {
    ProvidedDependency existing;
    ProvidedDependency selected;
};

struct PlanConstraintReadinessReason {
    std::size_t edge_index;
    std::string dependency_specification;
    ConstraintSatisfaction satisfaction;
};

struct PlanResolutionReason {
    BuildPlanResolutionFailure failure;
};

struct PlanUnresolvedDependencyReason {
    std::string dependency;
};

struct PlanAmbiguousProviderReason {
    AmbiguousProvidedDependency dependency;
};

struct PlanDependencyCycleReason {
    std::string dependency;
};

struct PlanDeclaredRelationReason {
    PackageRelationAssessment assessment;

    bool operator==(const PlanDeclaredRelationReason&) const = default;
};

struct PlanSplitPackageReason {
    BuildPlanSplitPackageTarget target;
};

struct PlanIncompleteProviderCandidateReason {
    IncompleteProviderCandidateSet candidate_set;
};

using PlanReason = std::variant<
    PlanConstraintAuthorityReason,
    PlanSelectedProviderIdentityConflictReason,
    PlanConstraintReadinessReason,
    PlanResolutionReason,
    PlanUnresolvedDependencyReason,
    PlanAmbiguousProviderReason,
    PlanDependencyCycleReason,
    PlanDeclaredRelationReason,
    PlanSplitPackageReason,
    PlanIncompleteProviderCandidateReason>;

struct ExecutionReadinessReason {
    PlanReason reason;
    ExecutionReadinessState state = ExecutionReadinessState::NotAssessed;
    bool blocks_production_guard = false;
    PlanRequiredAction required_action = PlanRequiredAction::None;
};

struct ExecutionReadiness {
    ExecutionCapability capability = ExecutionCapability::Fetch;
    ExecutionReadinessState state =
        ExecutionReadinessState::NotAssessed;
    bool is_blocked_by_production_guard = false;
    std::vector<ExecutionReadinessReason> reasons;
    std::vector<PlanRequiredAction> required_actions;
};

struct PlanStateProjection {
    PlanConstruction construction =
        PlanConstruction::Constructed;
    PlanCompleteness completeness =
        PlanCompleteness::Complete;
    ProviderDecision provider_decision =
        ProviderDecision::Unique;
    std::vector<PlanReason> completeness_reasons;
    std::array<ExecutionReadiness, 3> readiness;
};

PlanStateProjection project_build_plan_state(const BuildPlan& plan);
const ExecutionReadiness& execution_readiness(
    const PlanStateProjection& projection,
    ExecutionCapability capability) noexcept;

std::vector<std::string> collect_build_dependencies(const AurPackageInfo& pkg);
std::vector<TypedPackageDependency> collect_typed_build_dependencies(const AurPackageInfo& pkg);
DesiredInstallReason desired_install_reason(const PlannedPackageTarget& target);
std::vector<RecursiveDependencyNode> resolve_recursive_dependencies(const AurPackageInfo& pkg);
std::vector<RecursiveDependencyNode> resolve_recursive_dependencies(
    const AurPackageInfo& pkg,
    const ProviderSelectionCallback& select_provider);
std::vector<BuildPlanMetadataRisk> collect_build_plan_metadata_risks(const AurPackageInfo& pkg);
BuildPlan resolve_build_plan(const std::string& target);
BuildPlan resolve_build_plan(
    const std::string& target,
    const ProviderSelectionCallback& select_provider);
BuildPlan resolve_build_plan(const std::vector<std::string>& targets);
BuildPlan resolve_build_plan(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider);
BuildPlan resolve_build_plan_for_preflight(const std::vector<std::string>& targets);
BuildPlan resolve_build_plan_for_preflight(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider);
BuildPlan resolve_recipe_build_plan(
    const std::vector<std::string>& targets,
    const AurRecipeMetadataSet& recipes,
    const ProviderSelectionCallback& select_provider);
BuildPlan resolve_fetch_plan(const std::string& target);
BuildPlan resolve_fetch_plan(
    const std::string& target,
    const ProviderSelectionCallback& select_provider);
BuildPlan resolve_fetch_plan(const std::vector<std::string>& targets);
BuildPlan resolve_fetch_plan(
    const std::vector<std::string>& targets,
    const ProviderSelectionCallback& select_provider);
void require_compatible_selected_provider_package_identities(
    const BuildPlan& plan);
bool has_incomplete_constraint_evaluations(const BuildPlan& plan) noexcept;
// Installed exact is the only resolved dependency shape that can satisfy an
// update dependency without becoming mutation input. Re-evaluate the retained
// requirement instead of trusting the stored constraint classification.
bool is_verified_installed_exact_dependency_satisfaction(
    const BuildPlanDependencyEdge& edge,
    const std::string& expected_package_name,
    const std::string& expected_installed_version);
std::string constraint_satisfaction_display(
    ConstraintSatisfaction satisfaction);
std::string constraint_evaluation_reason_display(
    const ConstraintEvaluation& evaluation);
void require_constructible_build_plan_constraints(const BuildPlan& plan);
void finalize_build_plan_constraints(BuildPlan& plan);
void require_fetchable_build_plan(const std::string& target, const BuildPlan& plan);
void require_executable_build_plan(const std::string& target, const BuildPlan& plan);
void require_executable_install_plan(const std::string& target, const BuildPlan& plan);
