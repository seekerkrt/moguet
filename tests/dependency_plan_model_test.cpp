#include "aur_rpc.hpp"
#include "dependency_plan.hpp"
#include "dependency_provider.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dependency_plan_repository_query_stub {

void reset_query_counts();
std::size_t sync_database_package_query_count();
std::size_t strict_repo_provider_query_count();

} // namespace dependency_plan_repository_query_stub

namespace dependency_plan_aur_rpc_stub {

void reset_selected_provider_identity_queries();

} // namespace dependency_plan_aur_rpc_stub

namespace {

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

ProvidedDependency typed_aur_provider(
    std::string package_name, std::string package_base,
    std::string capability, std::optional<std::string> package_version) {
    ProviderCapability parsed(
        capability,
        capability.substr(0, capability.find('=')),
        capability.find('=') == std::string::npos
            ? std::nullopt
            : std::optional<std::string>(
                  capability.substr(capability.find('=') + 1)));
    const ObservedVersion provided_version = parsed.version().has_value()
                                                 ? ObservedVersion::available(
                                                       ObservedVersionSource::AurProviderCapability,
                                                       parsed.version().value())
                                                 : ObservedVersion::unknown(
                                                       ObservedVersionSource::AurProviderCapability,
                                                       ObservedVersionUnknownReason::UnversionedProviderCapability);
    return ProvidedDependency::from_aur_constraint_metadata(
        std::move(package_name), std::move(package_base),
        ProviderConstraintMetadata{
            std::move(parsed),
            package_version.has_value()
                ? ObservedVersion::available(
                      ObservedVersionSource::AurExactPackage,
                      package_version.value())
                : ObservedVersion::unknown(
                      ObservedVersionSource::AurExactPackage,
                      ObservedVersionUnknownReason::MissingVersionMetadata),
            provided_version});
}

ProvidedDependency typed_repository_provider(
    std::string repository_name, std::string package_name,
    std::string package_base, std::string capability,
    std::optional<std::string> package_version) {
    ProviderCapability parsed(
        capability,
        capability.substr(0, capability.find('=')),
        capability.find('=') == std::string::npos
            ? std::nullopt
            : std::optional<std::string>(
                  capability.substr(capability.find('=') + 1)));
    const ObservedVersion provided_version = parsed.version().has_value()
                                                 ? ObservedVersion::available(
                                                       ObservedVersionSource::RepositoryProviderCapability,
                                                       parsed.version().value())
                                                 : ObservedVersion::unknown(
                                                       ObservedVersionSource::RepositoryProviderCapability,
                                                       ObservedVersionUnknownReason::UnversionedProviderCapability);
    return ProvidedDependency::from_repository_constraint_metadata(
        std::move(repository_name), std::move(package_name),
        std::move(package_base), "x86_64",
        ProviderConstraintMetadata{
            std::move(parsed),
            package_version.has_value()
                ? ObservedVersion::available(
                      ObservedVersionSource::RepositoryExactPackage,
                      package_version.value())
                : ObservedVersion::unknown(
                      ObservedVersionSource::RepositoryExactPackage,
                      ObservedVersionUnknownReason::MissingVersionMetadata),
            provided_version});
}

ProvidedDependency case7_aur_provider() {
    return typed_aur_provider(
        "case7-provider-pkg", "case7-provider-pkg",
        "case7-virtual-api",
        std::optional<std::string>{"1.0-1"});
}

ProvidedDependency case8_repository_provider_a() {
    return typed_repository_provider(
        "extra", "case8-provider-a", "case8-provider-a-base",
        "case8-virtual=2",
        std::optional<std::string>{"2.0-1"});
}

ProvidedDependency case8_repository_provider_b() {
    return typed_repository_provider(
        "community", "case8-provider-b", "case8-provider-b-base",
        "case8-virtual=3",
        std::optional<std::string>{"3.0-1"});
}

ProvidedDependency case14_repository_provider() {
    return typed_repository_provider(
        "aur", "case14-provider", "case14-provider-base",
        "case14-virtual=1",
        std::optional<std::string>{"1.0-1"});
}

ProvidedDependency case21_aur_provider_a() {
    return typed_aur_provider(
        "case21-provider-a", "case21-provider-a", "case21-virtual=1",
        std::optional<std::string>{"1.0-1"});
}

ProvidedDependency case21_aur_provider_b() {
    return typed_aur_provider(
        "case21-provider-b", "case21-provider-suite", "case21-virtual=2",
        std::optional<std::string>{"1.0-1"});
}

ProvidedDependency case22_aur_provider() {
    return typed_aur_provider(
        "case22-provider", "case22-provider", "case22-virtual",
        std::optional<std::string>{"1.0-1"});
}

ProviderSelectionCallback select_case21_provider(
    std::size_t& invocation_count) {
    return [&invocation_count](
               const std::string& dependency,
               const std::vector<ProvidedDependency>& candidates)
               -> std::optional<ProvidedDependency> {
        ++invocation_count;
        expect(
            dependency == "case21-virtual",
            "Case 21 selector dependency differs");
        expect(
            candidates == std::vector<ProvidedDependency>{
                              case21_aur_provider_a(), case21_aur_provider_b()},
            "Case 21 selector candidates differ");

        // Selectorの返値はidentityだけをauthorityとし、補助metadataはresolver側を正とする。
        return ProvidedDependency::from_aur(
            "case21-provider-b", "case21-provider-suite",
            "selector-tampered", "selector-tampered=999",
            std::optional<std::string>{"999.0-1"});
    };
}

const PlannedPackageTarget& require_package_target(
    const BuildPlan& plan, std::string_view package_name) {
    const PlannedPackageTarget* found = nullptr;
    for(const auto& target : plan.package_targets) {
        if(target.package_name != package_name) continue;
        if(found != nullptr) {
            throw std::runtime_error(
                "Duplicate planned package target: " + std::string(package_name));
        }
        found = &target;
    }
    if(found == nullptr) {
        throw std::runtime_error(
            "Missing planned package target: " + std::string(package_name));
    }
    return *found;
}

const PlannedPackageRelationObservation& require_relation_observation(
    const BuildPlan& plan, std::string_view package_name) {
    const PlannedPackageRelationObservation* found = nullptr;
    for(const auto& observation : plan.planned_relation_observations) {
        if(observation.package.package_name != package_name) continue;
        if(found != nullptr) {
            throw std::runtime_error(
                "Duplicate planned relation observation: " +
                std::string(package_name));
        }
        found = &observation;
    }
    if(found == nullptr) {
        throw std::runtime_error(
            "Missing planned relation observation: " +
            std::string(package_name));
    }
    return *found;
}

PackageRelationAssessment relation_assessment_fixture(
    PackageRelationAssessmentKind expected_kind,
    std::vector<PackageRelationRootAttribution> roots = {{0, "root"}}) {
    const PackageRelationKind relation_kind =
        expected_kind ==
                PackageRelationAssessmentKind::PotentialReplacement
            ? PackageRelationKind::Replacement
            : PackageRelationKind::Conflict;
    const DeclaredPackageRelationParseResult parsed =
        parse_declared_package_relation(
            "relation-child", "relation-base", relation_kind,
            "relation-target");
    expect(parsed.relation() != nullptr, "Relation fixture did not parse");
    const PackageRelationAurSourceIdentity declaring_source{
        "relation-child", "relation-base"};
    PackageRelationObservedPackage declaring_package{
        "relation-child",
        "relation-base",
        ObservedVersion::available(
            ObservedVersionSource::AurExactPackage, "1"),
        {},
        declaring_source,
        PackageRelationObservationRole::PlannedTarget,
        std::move(roots)};

    const PackageRelationInstalledDatabaseIdentity installed_source{
        "/", "/var/lib/pacman"};
    PackageRelationMatchingEvidence active_evidence{
        PackageRelationObservationCompleteness::Complete,
        {PackageRelationSourceIdentity{installed_source}},
        {PackageRelationSourceIdentityCoverage{
            PackageRelationSourceIdentity{installed_source},
            true,
            true}},
        {},
        {}};
    std::optional<PackageRelationMatchEvidence> matched_evidence;
    std::optional<PackageRelationObservationFailure> observation_failure;
    if(expected_kind == PackageRelationAssessmentKind::
                            ConfirmedInstalledConflict ||
       expected_kind ==
           PackageRelationAssessmentKind::PotentialReplacement) {
        matched_evidence = PackageRelationMatchEvidence{
            PackageRelationObservedPackage{
                "relation-target",
                std::nullopt,
                ObservedVersion::available(
                    ObservedVersionSource::InstalledExactPackage,
                    "1"),
                {},
                installed_source,
                PackageRelationObservationRole::Installed,
                {}},
            PackageRelationIdentityMatchKind::ExactPackage,
            PackageRelationVersionMatchKind::Unconstrained,
            {},
            std::nullopt};
    } else if(expected_kind == PackageRelationAssessmentKind::
                                   ConfirmedPlannedTargetConflict) {
        matched_evidence = PackageRelationMatchEvidence{
            PackageRelationObservedPackage{
                "relation-target",
                "relation-target-base",
                ObservedVersion::available(
                    ObservedVersionSource::AurExactPackage, "1"),
                {},
                PackageRelationAurSourceIdentity{
                    "relation-target", "relation-target-base"},
                PackageRelationObservationRole::PlannedTarget,
                {{0, "root"}}},
            PackageRelationIdentityMatchKind::ExactPackage,
            PackageRelationVersionMatchKind::Unconstrained,
            {},
            std::nullopt};
    } else if(expected_kind == PackageRelationAssessmentKind::Unknown) {
        active_evidence.observation_completeness =
            PackageRelationObservationCompleteness::Unavailable;
        active_evidence.source_identity_coverage.front()
            .exact_package_identity = false;
        active_evidence.source_identity_coverage.front()
            .provided_component_identity = false;
        observation_failure = PackageRelationObservationFailure{
            PackageRelationObservationFailureKind::SourceUnavailable,
            PackageRelationObservationRole::Installed,
            PackageRelationSourceIdentity{installed_source},
            std::nullopt,
            "installed inventory unavailable"};
    } else if(expected_kind == PackageRelationAssessmentKind::Invalid) {
        active_evidence.observation_completeness =
            PackageRelationObservationCompleteness::Invalid;
        active_evidence.source_identity_coverage.front()
            .exact_package_identity = false;
        active_evidence.source_identity_coverage.front()
            .provided_component_identity = false;
        observation_failure = PackageRelationObservationFailure{
            PackageRelationObservationFailureKind::MalformedMetadata,
            PackageRelationObservationRole::Installed,
            PackageRelationSourceIdentity{installed_source},
            std::nullopt,
            "installed inventory malformed"};
    } else if(expected_kind ==
              PackageRelationAssessmentKind::DeclaredRelation) {
        active_evidence = PackageRelationMatchingEvidence{
            PackageRelationObservationCompleteness::Unavailable,
            {},
            {},
            {},
            {}};
    }

    if(matched_evidence.has_value()) {
        active_evidence.package_evidence.push_back(*matched_evidence);
    }
    if(observation_failure.has_value()) {
        active_evidence.observation_failures.push_back(*observation_failure);
    }

    // Model tests consume a completed assessment. Matching/classification is
    // owned and tested by package_relation_assessment_test.cpp.
    return PackageRelationAssessment{
        *parsed.relation(),
        expected_kind,
        std::move(declaring_package),
        std::move(active_evidence),
        std::nullopt,
        std::move(matched_evidence),
        std::move(observation_failure)};
}

std::size_t package_target_count(
    const BuildPlan& plan, std::string_view package_name) {
    return static_cast<std::size_t>(std::count_if(
        plan.package_targets.begin(), plan.package_targets.end(),
        [&package_name](const PlannedPackageTarget& target) {
            return target.package_name == package_name;
        }));
}

const BuildPlanDependencyEdge& require_edge(
    const BuildPlan& plan, std::string_view parent_package_name,
    std::string_view dependency_spec, PackageRole role) {
    const BuildPlanDependencyEdge* found = nullptr;
    for(const auto& edge : plan.dependency_edges) {
        if(edge.parent_package_name != parent_package_name ||
           edge.dependency_spec != dependency_spec || edge.role != role) {
            continue;
        }
        if(found != nullptr) {
            throw std::runtime_error(
                "Duplicate dependency edge: " + std::string(parent_package_name) +
                " -> " + std::string(dependency_spec));
        }
        found = &edge;
    }
    if(found == nullptr) {
        throw std::runtime_error(
            "Missing dependency edge: " + std::string(parent_package_name) + " -> " +
            std::string(dependency_spec));
    }
    return *found;
}

const BuildPlanResolutionFailure& require_resolution_failure(
    const BuildPlan& plan, BuildPlanResolutionFailureKind kind,
    std::string_view subject) {
    const BuildPlanResolutionFailure* found = nullptr;
    for(const auto& failure : plan.resolution_failures) {
        if(failure.kind != kind || failure.subject != subject) continue;
        if(found != nullptr) {
            throw std::runtime_error(
                "Duplicate resolution failure: " + std::string(subject));
        }
        found = &failure;
    }
    if(found == nullptr) {
        throw std::runtime_error(
            "Missing resolution failure: " + std::string(subject));
    }
    return *found;
}

void expect_roles(
    const PlannedPackageTarget& target, const std::vector<PackageRole>& expected) {
    expect(target.roles == expected, "Unexpected roles for " + target.package_name);
}

void expect_roots(
    const PlannedPackageTarget& target,
    const std::vector<RootTargetIdentity>& expected) {
    expect(target.roots == expected, "Unexpected roots for " + target.package_name);
}

void expect_resolution_failure_context(
    const BuildPlanResolutionFailure& failure,
    const std::optional<std::string>& parent_package_name,
    const std::optional<std::string>& parent_package_base,
    const std::optional<std::string>& dependency_specification,
    const std::vector<RootTargetIdentity>& roots,
    const std::string& diagnostic) {
    expect(
        failure.parent_package_name == parent_package_name,
        "Resolution failure parent name differs for " + failure.subject);
    expect(
        failure.parent_package_base == parent_package_base,
        "Resolution failure parent base differs for " + failure.subject);
    expect(
        failure.dependency_specification == dependency_specification,
        "Resolution failure dependency differs for " + failure.subject);
    expect(
        failure.roots == roots,
        "Resolution failure roots differ for " + failure.subject);
    expect(
        failure.diagnostic == diagnostic,
        "Resolution failure diagnostic differs for " + failure.subject);
}

void expect_legacy_order(
    const BuildPlan& plan, const std::vector<std::string>& expected_package_bases) {
    expect(plan.order.size() == expected_package_bases.size(), "Unexpected legacy order size");
    for(std::size_t i = 0; i < expected_package_bases.size(); ++i) {
        expect(
            plan.order[i].package_base == expected_package_bases[i],
            "Unexpected legacy order at index " + std::to_string(i));
        expect(
            plan.order[i].package_names ==
                std::vector<std::string>{expected_package_bases[i]},
            "Unexpected legacy package names at index " + std::to_string(i));
    }
}

const BuildPlanEntry& require_build_plan_entry(
    const BuildPlan& plan, std::string_view package_base) {
    const BuildPlanEntry* found = nullptr;
    for(const auto& entry : plan.order) {
        if(entry.package_base != package_base) continue;
        if(found != nullptr) {
            throw std::runtime_error(
                "Duplicate build plan entry: " + std::string(package_base));
        }
        found = &entry;
    }
    if(found == nullptr) {
        throw std::runtime_error(
            "Missing build plan entry: " + std::string(package_base));
    }
    return *found;
}

std::size_t require_build_plan_order_index(
    const BuildPlan& plan, std::string_view package_base) {
    static_cast<void>(require_build_plan_entry(plan, package_base));
    for(std::size_t i = 0; i < plan.order.size(); ++i) {
        if(plan.order[i].package_base == package_base) return i;
    }
    throw std::logic_error(
        "Validated build plan entry has no order index: " +
        std::string(package_base));
}

void expect_build_unit_before(
    const BuildPlan& plan, std::string_view dependency_package_base,
    std::string_view dependent_package_base) {
    expect(
        require_build_plan_order_index(plan, dependency_package_base) <
            require_build_plan_order_index(plan, dependent_package_base),
        "Build unit order differs: " + std::string(dependency_package_base) +
            " must precede " + std::string(dependent_package_base));
}

void expect_direct_aur_resolution(
    const BuildPlanDependencyEdge& edge, const std::string& expected_name,
    const std::string& expected_base) {
    expect(edge.kind == DependencyKind::Aur, "Direct AUR edge kind differs");
    expect(
        edge.resolved_package_name == std::optional<std::string>{expected_name},
        "Direct AUR resolved name differs");
    expect(
        edge.resolved_package_base == std::optional<std::string>{expected_base},
        "Direct AUR resolved base differs");
    expect(!edge.resolved_provider.has_value(), "Direct AUR edge has a provider");
}

template <typename Callable>
void expect_exception(Callable callable, const std::string& expected_message) {
    try {
        callable();
    } catch(const std::exception& error) {
        expect(
            std::string(error.what()) == expected_message,
            "Unexpected exception message: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected exception: " + expected_message);
}

template <typename Callable>
void expect_failure(Callable callable, const std::string& context) {
    try {
        callable();
    } catch(const std::exception&) {
        return;
    }
    throw std::runtime_error("Expected failure: " + context);
}

template <typename Callable>
void expect_aur_rpc_response_error(
    Callable callable, const std::string& expected_message) {
    try {
        callable();
    } catch(const AurRpcResponseError& error) {
        expect(
            std::string(error.what()) == expected_message,
            "Unexpected AUR RPC response error: " + std::string(error.what()));
        return;
    } catch(const std::exception& error) {
        throw std::runtime_error(
            "Expected AurRpcResponseError, got: " + std::string(error.what()));
    }
    throw std::runtime_error(
        "Expected AurRpcResponseError: " + expected_message);
}

void expect_equivalent_plans(const BuildPlan& lhs, const BuildPlan& rhs) {
    expect(lhs.order.size() == rhs.order.size(), "BuildPlan::order size differs");
    for(std::size_t i = 0; i < lhs.order.size(); ++i) {
        expect(lhs.order[i].package_base == rhs.order[i].package_base, "BuildPlan::order base differs");
        expect(lhs.order[i].package_names == rhs.order[i].package_names, "BuildPlan::order names differ");
    }

    expect(lhs.root_targets == rhs.root_targets, "BuildPlan::root_targets differs");
    expect(lhs.package_targets.size() == rhs.package_targets.size(), "BuildPlan::package_targets size differs");
    for(std::size_t i = 0; i < lhs.package_targets.size(); ++i) {
        const PlannedPackageTarget& left = lhs.package_targets[i];
        const PlannedPackageTarget& right = rhs.package_targets[i];
        expect(left.package_name == right.package_name, "Planned package name differs");
        expect(left.package_base == right.package_base, "Planned package base differs");
        expect(left.roles == right.roles, "Planned package roles differ");
        expect(left.roots == right.roots, "Planned package roots differ");
    }

    expect(
        lhs.dependency_edges.size() == rhs.dependency_edges.size(),
        "BuildPlan::dependency_edges size differs");
    for(std::size_t i = 0; i < lhs.dependency_edges.size(); ++i) {
        const BuildPlanDependencyEdge& left = lhs.dependency_edges[i];
        const BuildPlanDependencyEdge& right = rhs.dependency_edges[i];
        expect(left.parent_package_name == right.parent_package_name, "Edge parent name differs");
        expect(left.parent_package_base == right.parent_package_base, "Edge parent base differs");
        expect(left.dependency_spec == right.dependency_spec, "Edge specification differs");
        expect(left.role == right.role, "Edge role differs");
        expect(left.kind == right.kind, "Edge kind differs");
        expect(left.resolved_package_name == right.resolved_package_name, "Edge resolved name differs");
        expect(left.resolved_package_base == right.resolved_package_base, "Edge resolved base differs");
        expect(
            left.resolved_provider == right.resolved_provider,
            "Edge provider differs");
        expect(
            left.provider_resolution == right.provider_resolution,
            "Edge provider resolution differs");
    }

    expect(
        lhs.resolution_failures == rhs.resolution_failures,
        "BuildPlan::resolution_failures differs");
    expect(
        lhs.incomplete_provider_candidate_sets ==
            rhs.incomplete_provider_candidate_sets,
        "BuildPlan::incomplete_provider_candidate_sets differs");
    expect(
        lhs.relation_assessments == rhs.relation_assessments,
        "BuildPlan::relation_assessments differs");

    expect(
        lhs.split_package_targets.size() == rhs.split_package_targets.size(),
        "BuildPlan::split_package_targets size differs");
    for(std::size_t i = 0; i < lhs.split_package_targets.size(); ++i) {
        expect(
            lhs.split_package_targets[i].package_base == rhs.split_package_targets[i].package_base,
            "Split package base differs");
        expect(
            lhs.split_package_targets[i].package_name == rhs.split_package_targets[i].package_name,
            "Split package name differs");
    }

    expect(lhs.provided.size() == rhs.provided.size(), "BuildPlan::provided size differs");
    for(std::size_t i = 0; i < lhs.provided.size(); ++i) {
        expect(lhs.provided[i].dependency == rhs.provided[i].dependency, "Provided dependency differs");
        expect(lhs.provided[i].provider == rhs.provided[i].provider, "Provider differs");
        expect(
            lhs.provided[i].resolution == rhs.provided[i].resolution,
            "Provided dependency resolution differs");
    }

    expect(lhs.metadata_risks.size() == rhs.metadata_risks.size(), "BuildPlan::metadata_risks size differs");
    for(std::size_t i = 0; i < lhs.metadata_risks.size(); ++i) {
        const BuildPlanMetadataRisk& left = lhs.metadata_risks[i];
        const BuildPlanMetadataRisk& right = rhs.metadata_risks[i];
        expect(left.package_name == right.package_name, "Metadata risk package name differs");
        expect(left.package_base == right.package_base, "Metadata risk package base differs");
        expect(left.conflicts == right.conflicts, "Metadata risk conflicts differs");
        expect(left.replaces == right.replaces, "Metadata risk replaces differs");
    }

    expect(
        lhs.ambiguous_providers.size() == rhs.ambiguous_providers.size(),
        "BuildPlan::ambiguous_providers size differs");
    for(std::size_t i = 0; i < lhs.ambiguous_providers.size(); ++i) {
        const AmbiguousProvidedDependency& left = lhs.ambiguous_providers[i];
        const AmbiguousProvidedDependency& right = rhs.ambiguous_providers[i];
        expect(left.dependency == right.dependency, "Ambiguous dependency differs");
        expect(left.candidates.size() == right.candidates.size(), "Ambiguous candidates size differs");
        for(std::size_t j = 0; j < left.candidates.size(); ++j) {
            expect(left.candidates[j] == right.candidates[j], "Ambiguous provider differs");
        }
    }

    expect(
        lhs.cancelled_provider_dependencies ==
            rhs.cancelled_provider_dependencies,
        "BuildPlan::cancelled_provider_dependencies differs");

    expect(lhs.unresolved == rhs.unresolved, "BuildPlan::unresolved differs");
    expect(lhs.cycles == rhs.cycles, "BuildPlan::cycles differs");
}

void test_provider_origin_value_contract() {
    const ProvidedDependency repository =
        ProvidedDependency::from_repository("aur", "same-package");
    const ProvidedDependency same_repository =
        ProvidedDependency::from_repository("aur", "same-package");
    const ProvidedDependency aur =
        ProvidedDependency::from_aur("same-package");
    const ProvidedDependency same_aur =
        ProvidedDependency::from_aur("same-package");

    expect(repository == same_repository, "Repository provider equality differs");
    expect(aur == same_aur, "AUR provider equality differs");
    expect(repository != aur, "Repository and AUR provider origins were conflated");

    const auto* repository_origin =
        std::get_if<RepositoryProviderOrigin>(&repository.origin);
    expect(repository_origin != nullptr, "Repository provider origin kind differs");
    expect(
        repository_origin->repository_name == "aur",
        "Repository provider origin name differs");
    expect(
        std::holds_alternative<AurProviderOrigin>(aur.origin),
        "AUR provider origin kind differs");
    expect(
        provided_dependency_display(repository) == "aur/same-package" &&
            provided_dependency_display(aur) == "aur/same-package",
        "Typed provider display compatibility differs");
}

BuildPlan typed_constraint_plan(
    ConstraintEvaluation evaluation,
    std::string resolved_name = "typed-candidate") {
    BuildPlan plan;
    ConsumerDependencyRequirement requirement(
        "typed-candidate>=2", "typed-candidate",
        DependencyVersionConstraint(
            DependencyVersionRelation::GreaterThanOrEqual, "2"));
    RepositoryExactPackage candidate{
        ConfiguredRepositoryIdentity{"core", 0},
        resolved_name,
        resolved_name,
        ObservedVersion::available(
            ObservedVersionSource::RepositoryExactPackage, "1"),
        {}};
    plan.dependency_edges.push_back(BuildPlanDependencyEdge{
        "typed-root",
        "typed-root",
        "typed-candidate>=2",
        PackageRole::RuntimeDependency,
        DependencyKind::Repo,
        resolved_name,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{requirement},
        ResolvedDependencyCandidate{candidate},
        std::move(evaluation)});
    return plan;
}

void test_typed_constraint_edge_and_mutation_firewall() {
    BuildPlan unsatisfied =
        typed_constraint_plan(ConstraintEvaluation::unsatisfied());
    const BuildPlanDependencyEdge& edge = unsatisfied.dependency_edges.front();
    expect(
        edge.requirement.has_value() &&
            edge.resolved_candidate.has_value() &&
            edge.constraint_evaluation ==
                std::optional<ConstraintEvaluation>{
                    ConstraintEvaluation::unsatisfied()},
        "Typed dependency edge did not retain requirement/candidate/evaluation");
    expect(
        has_incomplete_constraint_evaluations(unsatisfied),
        "Unsatisfied constraint did not make the read-only plan incomplete");
    require_constructible_build_plan_constraints(unsatisfied);
    bool mutation_called = false;
    expect_exception(
        [&]() {
            require_fetchable_build_plan("typed-root", unsatisfied);
            mutation_called = true;
        },
        "Cannot execute build plan for typed-root; dependency "
        "typed-candidate>=2 is Unsatisfied: the observed version does not "
        "satisfy the requirement.");
    expect(!mutation_called, "Mutation sentinel ran after Unsatisfied preflight");

    BuildPlan unknown = typed_constraint_plan(ConstraintEvaluation::unknown(
        ObservedVersionUnknownReason::CandidateVersionUnavailable));
    expect(
        has_incomplete_constraint_evaluations(unknown),
        "Unknown constraint did not make the read-only plan incomplete");
    expect_exception(
        [&]() {
            require_fetchable_build_plan("typed-root", unknown);
            mutation_called = true;
        },
        "Cannot execute build plan for typed-root; dependency "
        "typed-candidate>=2 is Unknown: candidate version cannot be proven.");
    expect(!mutation_called, "Mutation sentinel ran after Unknown preflight");

    for(const ConstraintEvaluation evaluation : {
            ConstraintEvaluation::satisfied(),
            ConstraintEvaluation::unconstrained()}) {
        BuildPlan executable = typed_constraint_plan(evaluation);
        expect(
            !has_incomplete_constraint_evaluations(executable),
            "Satisfied/Unconstrained constraint made the plan incomplete");
        require_fetchable_build_plan("typed-root", executable);
    }
}

void test_invalid_conflicting_and_source_identity_fail_closed() {
    const std::optional<ConstraintEvaluation> conflicting =
        project_conflicting_constraint_invocation({ConsumerDependencyRequirement(
                                                       "typed-candidate<1", "typed-candidate",
                                                       DependencyVersionConstraint(
                                                           DependencyVersionRelation::LessThan,
                                                           "1")),
                                                   ConsumerDependencyRequirement(
                                                       "typed-candidate>=2", "typed-candidate",
                                                       DependencyVersionConstraint(
                                                           DependencyVersionRelation::
                                                               GreaterThanOrEqual,
                                                           "2"))});
    expect(conflicting.has_value(), "Conflict fixture was not conflicting");
    for(const ConstraintEvaluation evaluation : {
            ConstraintEvaluation::invalid(
                ConstraintInvalidReason::MalformedRequirement),
            conflicting.value()}) {
        BuildPlan plan = typed_constraint_plan(evaluation);
        bool failed = false;
        try {
            require_constructible_build_plan_constraints(plan);
        } catch(const std::exception&) {
            failed = true;
        }
        expect(failed, "Invalid/Conflicting plan construction did not fail closed");
    }

    BuildPlan mismatched = typed_constraint_plan(
        ConstraintEvaluation::satisfied(), "different-candidate");
    std::get<RepositoryExactPackage>(
        mismatched.dependency_edges.front().resolved_candidate.value())
        .package_name = "typed-candidate";
    expect_exception(
        [&]() {
            require_fetchable_build_plan("typed-root", mismatched);
        },
        "Build plan constraint source identity is inconsistent for "
        "typed-candidate>=2.");
}

BuildPlanDependencyEdge installed_exact_edge(
    ConsumerDependencyRequirement requirement,
    std::string installed_version,
    ConstraintEvaluation evaluation) {
    const std::string package_name = requirement.package_name();
    const std::string dependency_specification =
        requirement.raw_specification();
    return BuildPlanDependencyEdge{
        "installed-consumer",
        "installed-consumer",
        dependency_specification,
        PackageRole::RuntimeDependency,
        DependencyKind::Installed,
        package_name,
        std::nullopt,
        std::nullopt,
        ProviderResolutionKind::Unique,
        DependencyRequirement{std::move(requirement)},
        ResolvedDependencyCandidate{InstalledExactPackage{
            package_name,
            ObservedVersion::available(
                ObservedVersionSource::InstalledExactPackage,
                std::move(installed_version))}},
        std::move(evaluation)};
}

void test_verified_installed_exact_dependency_satisfaction_matrix() {
    const std::string package_name = "installed-exact";
    BuildPlanDependencyEdge unconstrained = installed_exact_edge(
        ConsumerDependencyRequirement(
            package_name, package_name, std::nullopt),
        "1.0-1", ConstraintEvaluation::unconstrained());
    expect(
        is_verified_installed_exact_dependency_satisfaction(
            unconstrained, package_name, "1.0-1"),
        "Unconstrained InstalledExact satisfaction was rejected");

    const auto satisfied_requirement = []() {
        return ConsumerDependencyRequirement(
            "installed-exact>=1.0-1", "installed-exact",
            DependencyVersionConstraint(
                DependencyVersionRelation::GreaterThanOrEqual,
                "1.0-1"));
    };
    BuildPlanDependencyEdge satisfied = installed_exact_edge(
        satisfied_requirement(), "1.0-1",
        ConstraintEvaluation::satisfied());
    expect(
        is_verified_installed_exact_dependency_satisfaction(
            satisfied, package_name, "1.0-1"),
        "Satisfied InstalledExact dependency was rejected");

    const std::optional<ConstraintEvaluation> conflicting =
        project_conflicting_constraint_invocation({
            ConsumerDependencyRequirement(
                "installed-exact<1", package_name,
                DependencyVersionConstraint(
                    DependencyVersionRelation::LessThan, "1")),
            ConsumerDependencyRequirement(
                "installed-exact>=2", package_name,
                DependencyVersionConstraint(
                    DependencyVersionRelation::GreaterThanOrEqual,
                    "2")),
        });
    expect(conflicting.has_value(), "InstalledExact conflict fixture is invalid");

    struct NegativeCase {
        std::string name;
        std::function<void(BuildPlanDependencyEdge&)> mutate;
        std::string expected_installed_version = "1.0-1";
    };
    const std::vector<NegativeCase> cases = {
        {"raw specification mismatch",
         [](BuildPlanDependencyEdge& edge) {
             edge.dependency_spec = "different-installed-exact";
         }},
        {"raw and typed constraint mismatch",
         [](BuildPlanDependencyEdge& edge) {
             edge.requirement = DependencyRequirement{
                 ConsumerDependencyRequirement(
                     "installed-exact>=2", "installed-exact",
                     std::nullopt)};
             edge.dependency_spec = "installed-exact>=2";
             edge.constraint_evaluation =
                 ConstraintEvaluation::unconstrained();
         }},
        {"forged Unconstrained",
         [](BuildPlanDependencyEdge& edge) {
             edge.requirement = DependencyRequirement{
                 ConsumerDependencyRequirement(
                     "installed-exact>=2", "installed-exact",
                     DependencyVersionConstraint(
                         DependencyVersionRelation::GreaterThanOrEqual,
                         "2"))};
             edge.dependency_spec = "installed-exact>=2";
             edge.constraint_evaluation =
                 ConstraintEvaluation::unconstrained();
         }},
        {"Unsatisfied",
         [](BuildPlanDependencyEdge& edge) {
             edge.requirement = DependencyRequirement{
                 ConsumerDependencyRequirement(
                     "installed-exact>=2", "installed-exact",
                     DependencyVersionConstraint(
                         DependencyVersionRelation::GreaterThanOrEqual,
                         "2"))};
             edge.dependency_spec = "installed-exact>=2";
             edge.constraint_evaluation =
                 ConstraintEvaluation::unsatisfied();
         }},
        {"Unknown",
         [](BuildPlanDependencyEdge& edge) {
             edge.constraint_evaluation = ConstraintEvaluation::unknown(
                 ObservedVersionUnknownReason::
                     CandidateVersionUnavailable);
         }},
        {"Invalid",
         [](BuildPlanDependencyEdge& edge) {
             edge.constraint_evaluation = ConstraintEvaluation::invalid(
                 ConstraintInvalidReason::MalformedRequirement);
         }},
        {"Conflicting",
         [&conflicting](BuildPlanDependencyEdge& edge) {
             edge.constraint_evaluation = *conflicting;
         }},
        {"stored evaluation drift",
         [](BuildPlanDependencyEdge& edge) {
             edge.constraint_evaluation =
                 ConstraintEvaluation::unconstrained();
         }},
        {"installed version drift", [](BuildPlanDependencyEdge&) {},
         "9.0-1"},
        {"wrong observed source",
         [](BuildPlanDependencyEdge& edge) {
             std::get<InstalledExactPackage>(
                 *edge.resolved_candidate)
                 .observed_version = ObservedVersion::available(
                 ObservedVersionSource::RepositoryExactPackage,
                 "1.0-1");
         }},
        {"wrong candidate variant",
         [](BuildPlanDependencyEdge& edge) {
             edge.resolved_candidate = AurResolvedDependencyCandidate{
                 "installed-exact", "installed-exact",
                 ObservedVersion::available(
                     ObservedVersionSource::AurExactPackage,
                     "1.0-1")};
         }},
        {"resolved name drift",
         [](BuildPlanDependencyEdge& edge) {
             edge.resolved_package_name = "different-installed-exact";
         }},
        {"resolved PackageBase present",
         [](BuildPlanDependencyEdge& edge) {
             edge.resolved_package_base = "installed-exact";
         }},
        {"resolved provider present",
         [](BuildPlanDependencyEdge& edge) {
             edge.resolved_provider =
                 ProvidedDependency::from_aur("installed-exact");
         }},
        {"root role",
         [](BuildPlanDependencyEdge& edge) {
             edge.role = PackageRole::Root;
         }},
        {"selected Installed edge",
         [](BuildPlanDependencyEdge& edge) {
             edge.provider_resolution =
                 ProviderResolutionKind::UserSelected;
         }},
    };

    for(const NegativeCase& test_case : cases) {
        BuildPlanDependencyEdge edge = satisfied;
        test_case.mutate(edge);
        expect(
            !is_verified_installed_exact_dependency_satisfaction(
                edge, package_name,
                test_case.expected_installed_version),
            "InstalledExact negative case passed: " + test_case.name);
    }
}

void test_execution_guard_category_order() {
    BuildPlan plan;
    plan.unresolved.push_back("missing-dependency");
    plan.ambiguous_providers.push_back(AmbiguousProvidedDependency{
        "virtual-api",
        {case8_repository_provider_a(),
         case8_repository_provider_b()}});
    plan.cycles.push_back("cycle-base");
    plan.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::ConfirmedInstalledConflict));
    plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{"split-base", "split-child"});

    const auto require_install_plan = [&plan]() {
        require_executable_install_plan("guard-root", plan);
    };
    expect_exception(
        require_install_plan,
        "Cannot execute build plan for guard-root; unresolved dependencies: "
        "missing-dependency");

    plan.unresolved.clear();
    expect_exception(
        require_install_plan,
        "Cannot execute build plan for guard-root; ambiguous providers: "
        "virtual-api (extra/case8-provider-a, "
        "community/case8-provider-b)");

    plan.ambiguous_providers.clear();
    expect_exception(
        require_install_plan,
        "Cannot execute build plan for guard-root; cyclic dependencies: "
        "cycle-base");

    plan.cycles.clear();
    expect_exception(
        require_install_plan,
        "Cannot execute build plan for guard-root; Installed conflict "
        "confirmed: declaring package relation-child (PackageBase: "
        "relation-base) [source: AUR source relation-child (PackageBase: "
        "relation-base); roots: input #1 requested root] declares conflict "
        "relation-target for target component relation-target; matched "
        "installed package relation-target [source: installed package "
        "database] through exact "
        "package component relation-target (version 1); build/install is "
        "blocked before mutation.");

    plan.relation_assessments.clear();
    expect_exception(
        require_install_plan,
        "Cannot execute singular install plan for guard-root; split "
        "package targets require the PackageBase set lifecycle: "
        "split-child (base: split-base)");
}

void test_plan_state_projection_keeps_capability_axes() {
    const ExecutionReadiness not_assessed;
    expect(
        not_assessed.state == ExecutionReadinessState::NotAssessed,
        "Default readiness did not preserve NotAssessed");

    BuildPlan relation_plan;
    relation_plan.relation_assessments.push_back(
        relation_assessment_fixture(
            PackageRelationAssessmentKind::
                ConfirmedInstalledConflict));
    relation_plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{"split-base", "split-child"});

    const PlanStateProjection relation_projection =
        project_build_plan_state(relation_plan);
    const ExecutionReadiness& fetch = execution_readiness(
        relation_projection, ExecutionCapability::Fetch);
    const ExecutionReadiness& build = execution_readiness(
        relation_projection, ExecutionCapability::Build);
    const ExecutionReadiness& install = execution_readiness(
        relation_projection, ExecutionCapability::Install);
    expect(
        fetch.state == ExecutionReadinessState::Ready &&
            !fetch.is_blocked_by_production_guard,
        "Fetch readiness imported build-only metadata policy");
    expect(
        build.state == ExecutionReadinessState::RequiresCheck &&
            build.is_blocked_by_production_guard &&
            std::find(
                build.required_actions.begin(),
                build.required_actions.end(),
                PlanRequiredAction::ReviewDeclaredRelations) !=
                build.required_actions.end(),
        "Build readiness lost declared metadata review");
    expect(
        install.state == ExecutionReadinessState::Blocked &&
            install.is_blocked_by_production_guard &&
            std::find(
                install.required_actions.begin(),
                install.required_actions.end(),
                PlanRequiredAction::UsePackageBaseSetLifecycle) !=
                install.required_actions.end(),
        "Install readiness lost the singular split-package gate");

    const auto relation_reason = std::find_if(
        build.reasons.begin(), build.reasons.end(),
        [](const ExecutionReadinessReason& reason) {
            return std::holds_alternative<PlanDeclaredRelationReason>(
                reason.reason);
        });
    expect(relation_reason != build.reasons.end(),
           "Build readiness lost declared relation metadata");
    const PlanDeclaredRelationReason& declared =
        std::get<PlanDeclaredRelationReason>(relation_reason->reason);
    expect(
        declared.assessment.kind ==
                PackageRelationAssessmentKind::
                    ConfirmedInstalledConflict &&
            declared.assessment.declaring_package.package_name ==
                "relation-child" &&
            declared.assessment.declaring_package.package_base ==
                std::optional<std::string>{"relation-base"} &&
            declared.assessment.declaration.target_component() ==
                "relation-target",
        "#353 relation assessment attribution was flattened");

    BuildPlan ambiguous_plan;
    ambiguous_plan.ambiguous_providers.push_back(
        AmbiguousProvidedDependency{
            "virtual-api",
            {case8_repository_provider_a(),
             case8_repository_provider_b()}});
    const PlanStateProjection ambiguous_projection =
        project_build_plan_state(ambiguous_plan);
    const ExecutionReadiness& ambiguous_fetch = execution_readiness(
        ambiguous_projection, ExecutionCapability::Fetch);
    expect(
        ambiguous_projection.provider_decision ==
                ProviderDecision::Ambiguous &&
            ambiguous_fetch.state ==
                ExecutionReadinessState::Blocked &&
            std::find(
                ambiguous_fetch.required_actions.begin(),
                ambiguous_fetch.required_actions.end(),
                PlanRequiredAction::SelectProvider) !=
                ambiguous_fetch.required_actions.end(),
        "Ambiguous provider decision was flattened");

    ambiguous_plan.cancelled_provider_dependencies.push_back(
        "virtual-api");
    const PlanStateProjection cancelled_projection =
        project_build_plan_state(ambiguous_plan);
    expect(
        cancelled_projection.provider_decision ==
                ProviderDecision::Cancelled &&
            execution_readiness(
                cancelled_projection,
                ExecutionCapability::Fetch)
                    .state ==
                ExecutionReadinessState::Blocked,
        "Cancelled provider decision was flattened into ambiguity");

    BuildPlan unknown_plan = typed_constraint_plan(
        ConstraintEvaluation::unknown(
            ObservedVersionUnknownReason::
                CandidateVersionUnavailable));
    const PlanStateProjection unknown_projection =
        project_build_plan_state(unknown_plan);
    const ExecutionReadiness& unknown_fetch = execution_readiness(
        unknown_projection, ExecutionCapability::Fetch);
    expect(
        unknown_projection.construction ==
                PlanConstruction::Constructed &&
            unknown_projection.completeness ==
                PlanCompleteness::Unknown &&
            unknown_fetch.state ==
                ExecutionReadinessState::Unknown &&
            unknown_fetch.is_blocked_by_production_guard &&
            std::find(
                unknown_fetch.required_actions.begin(),
                unknown_fetch.required_actions.end(),
                PlanRequiredAction::ObtainMetadata) !=
                unknown_fetch.required_actions.end(),
        "Unknown constraint was reduced to a readiness bool");

    BuildPlan invalid_plan = typed_constraint_plan(
        ConstraintEvaluation::invalid(
            ConstraintInvalidReason::MalformedRequirement));
    const PlanStateProjection invalid_projection =
        project_build_plan_state(invalid_plan);
    expect(
        invalid_projection.construction == PlanConstruction::Failed &&
            invalid_projection.completeness ==
                PlanCompleteness::Incomplete &&
            execution_readiness(
                invalid_projection,
                ExecutionCapability::Build)
                    .state ==
                ExecutionReadinessState::Blocked,
        "Invalid plan construction was flattened into completeness");
}

void test_relation_assessment_readiness_and_completeness_mapping() {
    for(const PackageRelationAssessmentKind kind : {
            PackageRelationAssessmentKind::
                ConfirmedInstalledConflict,
            PackageRelationAssessmentKind::
                ConfirmedPlannedTargetConflict,
            PackageRelationAssessmentKind::PotentialReplacement}) {
        BuildPlan plan;
        plan.relation_assessments.push_back(
            relation_assessment_fixture(kind));
        const PlanStateProjection projection =
            project_build_plan_state(plan);
        expect(
            projection.construction == PlanConstruction::Constructed &&
                projection.completeness ==
                    PlanCompleteness::Complete &&
                execution_readiness(
                    projection, ExecutionCapability::Fetch)
                        .state ==
                    ExecutionReadinessState::Ready &&
                !execution_readiness(
                     projection, ExecutionCapability::Fetch)
                     .is_blocked_by_production_guard &&
                execution_readiness(
                    projection, ExecutionCapability::Build)
                        .state ==
                    ExecutionReadinessState::RequiresCheck &&
                execution_readiness(
                    projection, ExecutionCapability::Install)
                    .is_blocked_by_production_guard,
            "Classified relation changed Fetch or completeness semantics");
    }

    for(const PackageRelationAssessmentKind kind : {
            PackageRelationAssessmentKind::DeclaredRelation,
            PackageRelationAssessmentKind::Unknown}) {
        BuildPlan plan;
        plan.relation_assessments.push_back(
            relation_assessment_fixture(kind));
        const PlanStateProjection projection =
            project_build_plan_state(plan);
        expect(
            projection.construction == PlanConstruction::Constructed &&
                projection.completeness ==
                    PlanCompleteness::Unknown &&
                execution_readiness(
                    projection, ExecutionCapability::Fetch)
                        .state ==
                    ExecutionReadinessState::Ready &&
                execution_readiness(
                    projection, ExecutionCapability::Build)
                        .state ==
                    ExecutionReadinessState::RequiresCheck &&
                execution_readiness(
                    projection, ExecutionCapability::Install)
                    .is_blocked_by_production_guard,
            "Unknown/fallback relation did not fail closed for Build/Install");
    }

    BuildPlan invalid;
    invalid.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::Invalid));
    const PlanStateProjection invalid_projection =
        project_build_plan_state(invalid);
    expect(
        invalid_projection.construction == PlanConstruction::Failed &&
            invalid_projection.completeness ==
                PlanCompleteness::Incomplete &&
            execution_readiness(
                invalid_projection, ExecutionCapability::Fetch)
                    .state ==
                ExecutionReadinessState::Ready &&
            execution_readiness(
                invalid_projection, ExecutionCapability::Build)
                    .state ==
                ExecutionReadinessState::Blocked,
        "Invalid relation did not use fail-closed construction policy");

    BuildPlan no_match;
    no_match.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::
            ConfirmedNoMatchingCurrentOrPlannedTarget));
    const PlanStateProjection no_match_projection =
        project_build_plan_state(no_match);
    expect(
        no_match_projection.completeness == PlanCompleteness::Complete &&
            execution_readiness(
                no_match_projection, ExecutionCapability::Build)
                    .state ==
                ExecutionReadinessState::Ready &&
            execution_readiness(
                no_match_projection, ExecutionCapability::Install)
                    .state ==
                ExecutionReadinessState::Ready,
        "Confirmed NoMatch retained a relation guard");
    require_executable_build_plan("no-match", no_match);
    require_executable_install_plan("no-match", no_match);

    BuildPlan legacy_raw_only;
    legacy_raw_only.metadata_risks.push_back(BuildPlanMetadataRisk{
        "legacy-child", "legacy-base", {"old"}, {"replacement"}});
    const PlanStateProjection legacy_projection =
        project_build_plan_state(legacy_raw_only);
    expect(
        execution_readiness(
            legacy_projection, ExecutionCapability::Build)
                    .state == ExecutionReadinessState::Ready &&
            !execution_readiness(
                 legacy_projection, ExecutionCapability::Install)
                 .is_blocked_by_production_guard,
        "Raw compatibility metadata remained a second safety authority");

    BuildPlan composed;
    composed.unresolved.push_back("missing-dependency");
    composed.relation_assessments.push_back(relation_assessment_fixture(
        PackageRelationAssessmentKind::Unknown,
        {{0, "root-a"}, {1, "root-b"}}));
    const PlanStateProjection composed_projection =
        project_build_plan_state(composed);
    expect(
        composed_projection.completeness == PlanCompleteness::Incomplete &&
            execution_readiness(
                composed_projection, ExecutionCapability::Fetch)
                    .state ==
                ExecutionReadinessState::Blocked &&
            composed.relation_assessments.front()
                    .declaring_package.roots ==
                std::vector<PackageRelationRootAttribution>{
                    {0, "root-a"}, {1, "root-b"}},
        "Existing blocker composition or shared roots were lost");
    expect_exception(
        [&composed]() {
            require_executable_build_plan("composed", composed);
        },
        "Cannot execute build plan for composed; unresolved dependencies: missing-dependency");
}

void test_split_package_install_readiness_is_orthogonal_to_completeness() {
    BuildPlan split_plan;
    split_plan.split_package_targets.push_back(
        BuildPlanSplitPackageTarget{"split-base", "split-child"});

    const PlanStateProjection projection =
        project_build_plan_state(split_plan);
    const ExecutionReadiness& fetch = execution_readiness(
        projection, ExecutionCapability::Fetch);
    const ExecutionReadiness& build = execution_readiness(
        projection, ExecutionCapability::Build);
    const ExecutionReadiness& install = execution_readiness(
        projection, ExecutionCapability::Install);

    expect(
        projection.construction == PlanConstruction::Constructed &&
            projection.completeness == PlanCompleteness::Complete,
        "Split-only install policy changed plan completeness");
    expect(
        fetch.state == ExecutionReadinessState::Ready &&
            build.state == ExecutionReadinessState::Ready,
        "Split-only install policy leaked into fetch/build readiness");
    expect(
        install.state == ExecutionReadinessState::Blocked &&
            install.is_blocked_by_production_guard,
        "Split-only singular install gate was not preserved");
    expect(
        std::none_of(
            projection.completeness_reasons.begin(),
            projection.completeness_reasons.end(),
            [](const PlanReason& reason) {
                return std::holds_alternative<PlanSplitPackageReason>(
                    reason);
            }),
        "Split-package install reason became completeness authority");
    const auto split_reason = std::find_if(
        install.reasons.begin(), install.reasons.end(),
        [](const ExecutionReadinessReason& reason) {
            return std::holds_alternative<PlanSplitPackageReason>(
                reason.reason);
        });
    expect(
        split_reason != install.reasons.end() &&
            split_reason->state == ExecutionReadinessState::Blocked &&
            split_reason->blocks_production_guard &&
            split_reason->required_action ==
                PlanRequiredAction::UsePackageBaseSetLifecycle,
        "Split-package install reason lost its independent readiness axes");
}

void test_case_1_root_only() {
    BuildPlan plan = resolve_build_plan("case1-app");
    expect(
        plan.root_targets == std::vector<RootTargetIdentity>{{0, "case1-app"}},
        "Case 1 root identity differs");

    const PlannedPackageTarget& app = require_package_target(plan, "case1-app");
    expect(plan.package_targets.size() == 1, "Case 1 package target count differs");
    expect(plan.dependency_edges.empty(), "Case 1 unexpectedly has dependency edges");
    expect(app.package_base == "case1-app", "Case 1 package base differs");
    expect_roles(app, {PackageRole::Root});
    expect_roots(app, {{0, "case1-app"}});
    expect(desired_install_reason(app) == DesiredInstallReason::Explicit, "Case 1 reason differs");
    expect_legacy_order(plan, {"case1-app"});
}

void test_case_2_dependency_roles() {
    std::optional<AurPackageInfo> app_info = AurClient::info("case2-app");
    expect(app_info.has_value(), "Case 2 fixture is missing");
    std::vector<TypedPackageDependency> typed =
        collect_typed_build_dependencies(app_info.value());
    expect(typed.size() == 3, "Case 2 typed dependency count differs");
    expect(
        typed[0].specification == "case2-runtime-dep>=2" &&
            typed[0].role == PackageRole::RuntimeDependency,
        "Case 2 runtime typed dependency differs");
    expect(
        typed[1].specification == "case2-build-dep" &&
            typed[1].role == PackageRole::BuildDependency,
        "Case 2 build typed dependency differs");
    expect(
        typed[2].specification == "case2-check-dep" &&
            typed[2].role == PackageRole::CheckDependency,
        "Case 2 check typed dependency differs");

    BuildPlan plan = resolve_build_plan("case2-app");
    expect_roles(
        require_package_target(plan, "case2-runtime-dep"),
        {PackageRole::RuntimeDependency});
    expect_roles(
        require_package_target(plan, "case2-build-dep"),
        {PackageRole::BuildDependency});
    expect_roles(
        require_package_target(plan, "case2-check-dep"),
        {PackageRole::CheckDependency});
    expect(
        package_target_count(plan, "case2-optional-dep") == 0,
        "Case 2 OptDepends leaked into the build plan");
    expect(plan.package_targets.size() == 4, "Case 2 package target count differs");
    expect(plan.dependency_edges.size() == 3, "Case 2 edge count differs");

    const BuildPlanDependencyEdge& runtime = require_edge(
        plan, "case2-app", "case2-runtime-dep>=2",
        PackageRole::RuntimeDependency);
    const BuildPlanDependencyEdge& build = require_edge(
        plan, "case2-app", "case2-build-dep", PackageRole::BuildDependency);
    const BuildPlanDependencyEdge& check = require_edge(
        plan, "case2-app", "case2-check-dep", PackageRole::CheckDependency);
    for(const BuildPlanDependencyEdge* edge : {&runtime, &build, &check}) {
        expect(edge->parent_package_base == "case2-app", "Case 2 edge parent base differs");
    }
    expect_direct_aur_resolution(runtime, "case2-runtime-dep", "case2-runtime-dep");
    expect_direct_aur_resolution(build, "case2-build-dep", "case2-build-dep");
    expect_direct_aur_resolution(check, "case2-check-dep", "case2-check-dep");
    expect(
        desired_install_reason(require_package_target(plan, "case2-build-dep")) ==
            DesiredInstallReason::Dependency,
        "Case 2 build dependency reason differs");
    expect(
        desired_install_reason(require_package_target(plan, "case2-check-dep")) ==
            DesiredInstallReason::Dependency,
        "Case 2 check dependency reason differs");
    expect_legacy_order(
        plan,
        {"case2-runtime-dep", "case2-build-dep", "case2-check-dep", "case2-app"});
}

void test_case_3_multiple_roles() {
    std::optional<AurPackageInfo> app_info = AurClient::info("case3-app");
    expect(app_info.has_value(), "Case 3 fixture is missing");
    expect(
        collect_typed_build_dependencies(app_info.value()).size() == 2,
        "Case 3 typed roles were deduplicated");
    expect(
        collect_build_dependencies(app_info.value()) ==
            std::vector<std::string>{"case3-shared"},
        "Case 3 legacy dependency dedup changed");

    BuildPlan plan = resolve_build_plan("case3-app");
    expect(package_target_count(plan, "case3-shared") == 1, "Case 3 target was not merged");
    const PlannedPackageTarget& shared = require_package_target(plan, "case3-shared");
    expect_roles(
        shared,
        {PackageRole::RuntimeDependency, PackageRole::BuildDependency});
    static_cast<void>(require_edge(
        plan, "case3-app", "case3-shared", PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case3-app", "case3-shared", PackageRole::BuildDependency));
    expect(plan.dependency_edges.size() == 2, "Case 3 edge count differs");
    expect_legacy_order(plan, {"case3-shared", "case3-app"});
    expect(
        desired_install_reason(shared) == DesiredInstallReason::Dependency,
        "Case 3 reason differs");
}

void test_case_4_root_dependency_overlap() {
    BuildPlan plan = resolve_build_plan(
        std::vector<std::string>{"case4-app", "case4-lib"});
    const PlannedPackageTarget& lib = require_package_target(plan, "case4-lib");
    expect_roles(lib, {PackageRole::Root, PackageRole::RuntimeDependency});
    expect_roots(lib, {{0, "case4-app"}, {1, "case4-lib"}});
    expect(
        desired_install_reason(lib) == DesiredInstallReason::Explicit,
        "Case 4 explicit-wins reducer differs");
    expect(package_target_count(plan, "case4-lib") == 1, "Case 4 lib target was duplicated");
    expect_legacy_order(plan, {"case4-lib", "case4-app"});
}

void test_case_5_shared_dependency() {
    BuildPlan plan = resolve_build_plan(
        std::vector<std::string>{"case5-app", "case5-tool"});
    const PlannedPackageTarget& common = require_package_target(plan, "case5-common");
    expect(package_target_count(plan, "case5-common") == 1, "Case 5 common target was duplicated");
    expect_roles(common, {PackageRole::RuntimeDependency});
    expect_roots(common, {{0, "case5-app"}, {1, "case5-tool"}});
    expect(
        desired_install_reason(common) == DesiredInstallReason::Dependency,
        "Case 5 reason differs");
    expect_legacy_order(plan, {"case5-common", "case5-app", "case5-tool"});
}

void test_case_6_repository_dependency() {
    BuildPlan plan = resolve_build_plan("case6-app");
    const BuildPlanDependencyEdge& edge = require_edge(
        plan, "case6-app", "case6-repo-lib", PackageRole::RuntimeDependency);
    expect(edge.kind == DependencyKind::Repo, "Case 6 edge kind differs");
    expect(
        edge.resolved_package_name == std::optional<std::string>{"case6-repo-lib"},
        "Case 6 resolved package differs");
    expect(!edge.resolved_package_base.has_value(), "Case 6 resolved base must be empty");
    expect(!edge.resolved_provider.has_value(), "Case 6 provider must be empty");
    expect(package_target_count(plan, "case6-repo-lib") == 0, "Case 6 repo target leaked into source plan");
    expect_legacy_order(plan, {"case6-app"});
}

void test_case_7_unique_aur_provider() {
    BuildPlan plan = resolve_build_plan("case7-app");
    const BuildPlanDependencyEdge& edge = require_edge(
        plan, "case7-app", "case7-virtual-api", PackageRole::RuntimeDependency);
    expect(edge.kind == DependencyKind::Provided, "Case 7 edge kind differs");
    expect(edge.resolved_provider.has_value(), "Case 7 provider is missing");
    expect(
        edge.resolved_provider.value() ==
            case7_aur_provider(),
        "Case 7 provider differs");
    expect(
        edge.provider_resolution == ProviderResolutionKind::Unique,
        "Case 7 provider resolution differs");
    const PlannedPackageTarget& provider =
        require_package_target(plan, "case7-provider-pkg");
    expect_roles(provider, {PackageRole::RuntimeDependency});
    expect_roots(provider, {{0, "case7-app"}});
    expect(plan.provided.size() == 1, "Case 7 legacy provider count differs");
    expect(plan.provided[0].dependency == "case7-virtual-api", "Case 7 legacy dependency differs");
    expect(
        plan.provided[0].provider ==
            case7_aur_provider(),
        "Case 7 legacy provider differs");
    expect(
        plan.provided[0].resolution == ProviderResolutionKind::Unique,
        "Case 7 legacy provider resolution differs");
    expect_legacy_order(plan, {"case7-provider-pkg", "case7-app"});
}

void test_case_8_ambiguous_provider() {
    BuildPlan plan = resolve_build_plan("case8-app");
    const BuildPlanDependencyEdge& edge = require_edge(
        plan, "case8-app", "case8-virtual", PackageRole::RuntimeDependency);
    expect(edge.kind == DependencyKind::AmbiguousProvider, "Case 8 edge kind differs");
    expect(!edge.resolved_package_name.has_value(), "Case 8 resolved name must be empty");
    expect(!edge.resolved_package_base.has_value(), "Case 8 resolved base must be empty");
    expect(!edge.resolved_provider.has_value(), "Case 8 provider must be empty");
    expect(plan.ambiguous_providers.size() == 1, "Case 8 ambiguous dependency count differs");
    expect(plan.ambiguous_providers[0].candidates.size() == 2, "Case 8 candidate count differs");
    expect(
        plan.ambiguous_providers[0].candidates ==
            std::vector<ProvidedDependency>{
                case8_repository_provider_a(),
                case8_repository_provider_b()},
        "Case 8 candidate metadata differs");
    expect(package_target_count(plan, "case8-provider-a") == 0, "Case 8 selected a provider");
    expect(package_target_count(plan, "case8-provider-b") == 0, "Case 8 selected a provider");
    expect_exception(
        [&plan]() { require_fetchable_build_plan("case8-app", plan); },
        "Cannot execute build plan for case8-app; ambiguous providers: "
        "case8-virtual (extra/case8-provider-a, community/case8-provider-b)");
}

void test_case_8_selected_repository_provider() {
    std::size_t invocation_count = 0;
    ProviderSelectionCallback select_provider =
        [&invocation_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++invocation_count;
        expect(
            dependency == "case8-virtual",
            "Case 8 selector dependency differs");
        expect(
            candidates == std::vector<ProvidedDependency>{
                              case8_repository_provider_a(),
                              case8_repository_provider_b()},
            "Case 8 selector candidates differ");
        ProvidedDependency selected = case8_repository_provider_b();
        selected.provided_dependency_name = "selector-tampered";
        selected.provided_dependency_specification =
            "selector-tampered=999";
        selected.package_version = "999.0-1";
        return selected;
    };

    BuildPlan plan = resolve_build_plan("case8-app", select_provider);
    expect(invocation_count == 1, "Case 8 selector invocation count differs");
    expect(plan.ambiguous_providers.empty(), "Case 8 selection stayed ambiguous");
    expect(plan.unresolved.empty(), "Case 8 selection became unresolved");

    const BuildPlanDependencyEdge& edge = require_edge(
        plan, "case8-app", "case8-virtual",
        PackageRole::RuntimeDependency);
    expect(edge.kind == DependencyKind::Provided, "Case 8 selected edge kind differs");
    expect(
        edge.resolved_provider ==
            std::optional<ProvidedDependency>{
                case8_repository_provider_b()},
        "Case 8 selected edge lost resolver-owned metadata");
    expect(
        edge.provider_resolution == ProviderResolutionKind::UserSelected,
        "Case 8 selected edge resolution differs");

    expect(plan.provided.size() == 1, "Case 8 selected provider count differs");
    expect(
        plan.provided[0].dependency == "case8-virtual" &&
            plan.provided[0].provider ==
                case8_repository_provider_b() &&
            plan.provided[0].resolution ==
                ProviderResolutionKind::UserSelected,
        "Case 8 selected provider record differs");
    expect(
        package_target_count(plan, "case8-provider-b") == 0,
        "Case 8 repository provider leaked into source targets");
    expect_legacy_order(plan, {"case8-app"});
    require_fetchable_build_plan("case8-app", plan);
}

void test_case_8_cancel_and_unoffered_selection() {
    BuildPlan without_selector = resolve_build_plan("case8-app");

    std::size_t cancel_count = 0;
    ProviderSelectionCallback cancel_selection =
        [&cancel_count](
            const std::string&,
            const std::vector<ProvidedDependency>&)
        -> std::optional<ProvidedDependency> {
        ++cancel_count;
        return std::nullopt;
    };
    BuildPlan cancelled = resolve_build_plan("case8-app", cancel_selection);
    expect(cancel_count == 1, "Case 8 cancel callback count differs");
    expect_equivalent_plans(without_selector, cancelled);

    ProviderSelectionCallback unoffered_selection = [](
                                                        const std::string&,
                                                        const std::vector<ProvidedDependency>&)
        -> std::optional<ProvidedDependency> {
        return ProvidedDependency::from_repository(
            "testing", "case8-provider-missing", "case8-virtual",
            "case8-virtual=999",
            std::optional<std::string>{"999.0-1"});
    };
    expect_exception(
        [&unoffered_selection]() {
            static_cast<void>(resolve_build_plan(
                "case8-app", unoffered_selection));
        },
        "Provider selection returned a candidate that was not offered for "
        "case8-virtual.");
}

void test_case_21_selected_aur_provider() {
    std::size_t invocation_count = 0;
    BuildPlan plan = resolve_build_plan(
        "case21-app", select_case21_provider(invocation_count));
    expect(invocation_count == 1, "Case 21 selector invocation count differs");
    expect(plan.ambiguous_providers.empty(), "Case 21 selection stayed ambiguous");
    expect(plan.unresolved.empty(), "Case 21 selection became unresolved");
    expect(plan.cycles.empty(), "Case 21 selection produced a cycle");

    const BuildPlanDependencyEdge& provider_edge = require_edge(
        plan, "case21-app", "case21-virtual",
        PackageRole::RuntimeDependency);
    expect(
        provider_edge.kind == DependencyKind::Provided,
        "Case 21 selected edge kind differs");
    expect(
        provider_edge.resolved_provider ==
            std::optional<ProvidedDependency>{case21_aur_provider_b()},
        "Case 21 selected edge lost resolver-owned metadata");
    expect(
        provider_edge.provider_resolution ==
            ProviderResolutionKind::UserSelected,
        "Case 21 selected edge resolution differs");

    expect(plan.provided.size() == 1, "Case 21 selected provider count differs");
    expect(
        plan.provided[0].dependency == "case21-virtual" &&
            plan.provided[0].provider == case21_aur_provider_b() &&
            plan.provided[0].resolution ==
                ProviderResolutionKind::UserSelected,
        "Case 21 selected provider record differs");

    const PlannedPackageTarget& provider =
        require_package_target(plan, "case21-provider-b");
    const PlannedPackageTarget& child =
        require_package_target(plan, "case21-provider-child");
    expect(
        provider.package_base == "case21-provider-suite",
        "Case 21 provider PackageBase differs");
    expect_roles(provider, {PackageRole::RuntimeDependency});
    expect_roles(child, {PackageRole::RuntimeDependency});
    expect_roots(provider, {{0, "case21-app"}});
    expect_roots(child, {{0, "case21-app"}});
    expect(
        require_build_plan_entry(plan, "case21-provider-suite").package_names ==
            std::vector<std::string>{"case21-provider-b"},
        "Case 21 provider build unit differs");

    const BuildPlanDependencyEdge& child_edge = require_edge(
        plan, "case21-provider-b", "case21-provider-child",
        PackageRole::RuntimeDependency);
    expect_direct_aur_resolution(
        child_edge, "case21-provider-child", "case21-provider-child");
    expect_build_unit_before(
        plan, "case21-provider-child", "case21-provider-suite");
    expect_build_unit_before(
        plan, "case21-provider-suite", "case21-app");
    require_fetchable_build_plan("case21-app", plan);
}

void test_provider_candidates_dedupe_by_source_identity() {
    std::size_t callback_count = 0;
    ProviderSelectionCallback observe_candidates =
        [&callback_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++callback_count;
        expect(
            dependency == "case22-virtual",
            "Identity dedupe selector dependency differs");
        expect(
            candidates == std::vector<ProvidedDependency>{
                              case22_aur_provider()},
            "Identity dedupe retained duplicate presentation metadata");
        return std::nullopt;
    };

    BuildPlan plan = resolve_build_plan(
        "case22-app", observe_candidates);
    expect(callback_count == 1, "Identity dedupe callback count differs");
    const BuildPlanDependencyEdge& edge = require_edge(
        plan, "case22-app", "case22-virtual",
        PackageRole::RuntimeDependency);
    expect(
        edge.resolved_provider ==
                std::optional<ProvidedDependency>{
                    typed_aur_provider(
                        "case22-provider",
                        "case22-provider",
                        "case22-virtual",
                        std::optional<std::string>{
                            "2.0-1"})} &&
            edge.provider_resolution == ProviderResolutionKind::Unique,
        "Identity dedupe did not retain the refreshed authoritative candidate");
}

BuildPlan selected_provider_identity_plan(
    ProvidedDependency first,
    ProvidedDependency second) {
    BuildPlan plan;
    plan.provided = {
        BuildPlanProvidedDependency{
            "first-virtual", std::move(first),
            ProviderResolutionKind::UserSelected},
        BuildPlanProvidedDependency{
            "second-virtual", std::move(second),
            ProviderResolutionKind::UserSelected},
    };
    return plan;
}

void test_selected_provider_package_identity_conflict_guard() {
    const auto repository_provider = [](const std::string& repository) {
        return ProvidedDependency::from_repository(
            repository, "shared-selected-provider", "virtual-api",
            "virtual-api=1", "1.0-1");
    };
    const auto aur_provider = [](const std::string& package_base) {
        return ProvidedDependency::from_aur(
            "shared-selected-provider", package_base, "virtual-api",
            "virtual-api=1", "1.0-1");
    };

    BuildPlan repository_conflict = selected_provider_identity_plan(
        repository_provider("extra"), repository_provider("core"));
    expect_exception(
        [&repository_conflict]() {
            require_compatible_selected_provider_package_identities(
                repository_conflict);
        },
        "Selected providers use incompatible identities for package "
        "shared-selected-provider: extra/shared-selected-provider and "
        "core/shared-selected-provider.");

    BuildPlan cross_source_conflict = selected_provider_identity_plan(
        repository_provider("extra"),
        aur_provider("shared-selected-provider-base"));
    expect_exception(
        [&cross_source_conflict]() {
            require_fetchable_build_plan(
                "identity-conflict-root", cross_source_conflict);
        },
        "Selected providers use incompatible identities for package "
        "shared-selected-provider: extra/shared-selected-provider and "
        "aur/shared-selected-provider (PackageBase: "
        "shared-selected-provider-base).");

    BuildPlan aur_base_conflict = selected_provider_identity_plan(
        aur_provider("first-selected-provider-base"),
        aur_provider("second-selected-provider-base"));
    expect_exception(
        [&aur_base_conflict]() {
            require_compatible_selected_provider_package_identities(
                aur_base_conflict);
        },
        "Selected providers use incompatible identities for package "
        "shared-selected-provider: aur/shared-selected-provider "
        "(PackageBase: first-selected-provider-base) and "
        "aur/shared-selected-provider (PackageBase: "
        "second-selected-provider-base).");

    BuildPlan compatible_aliases = selected_provider_identity_plan(
        repository_provider("extra"),
        ProvidedDependency::from_repository(
            "extra", "shared-selected-provider", "other-virtual",
            "other-virtual=2", "2.0-1"));
    require_compatible_selected_provider_package_identities(
        compatible_aliases);
    require_fetchable_build_plan(
        "compatible-provider-alias-root", compatible_aliases);
}

void test_selected_aur_provider_revalidation_boundary() {
    std::size_t callback_count = 0;
    ProviderSelectionCallback select_changed_provider =
        [&callback_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++callback_count;
        expect(
            dependency == "selected-provider-identity-virtual" &&
                candidates.size() == 2 &&
                candidates[1].package_name ==
                    "selected-provider-identity-b" &&
                candidates[1].package_base ==
                    "selected-provider-original-base",
            "Selected provider revalidation callback input differs");
        return candidates[1];
    };

    const std::string diagnostic =
        "AUR provider candidate changed during dependency resolution: "
        "selected-provider-identity-b";

    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        [&select_changed_provider]() {
            static_cast<void>(resolve_build_plan(
                "selected-provider-identity-root",
                select_changed_provider));
        },
        diagnostic);
    expect(callback_count == 1, "Build revalidation callback count differs");

    callback_count = 0;
    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        [&select_changed_provider]() {
            static_cast<void>(resolve_fetch_plan(
                "selected-provider-identity-root",
                select_changed_provider));
        },
        diagnostic);
    expect(callback_count == 1, "Fetch revalidation callback count differs");

    callback_count = 0;
    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        [&select_changed_provider]() {
            static_cast<void>(resolve_build_plan_for_preflight(
                {"selected-provider-identity-root"},
                select_changed_provider));
        },
        diagnostic);
    expect(callback_count == 1, "Preflight revalidation callback count differs");

    callback_count = 0;
    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    AurPackageInfo recursive_root;
    recursive_root.Name = "selected-provider-identity-recursive-root";
    recursive_root.PackageBase = recursive_root.Name;
    recursive_root.Depends = {"selected-provider-identity-virtual"};
    expect_exception(
        [&recursive_root, &select_changed_provider]() {
            static_cast<void>(resolve_recursive_dependencies(
                recursive_root, select_changed_provider));
        },
        diagnostic);
    expect(callback_count == 1, "Recursive revalidation callback count differs");

    std::size_t provides_callback_count = 0;
    ProviderSelectionCallback select_removed_provider =
        [&provides_callback_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++provides_callback_count;
        expect(
            dependency == "selected-provider-provides-virtual" &&
                candidates.size() == 2,
            "Removed Provides callback input differs");
        return candidates[1];
    };
    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        [&select_removed_provider]() {
            static_cast<void>(resolve_build_plan(
                "selected-provider-provides-root",
                select_removed_provider));
        },
        "AUR provider candidate changed during dependency resolution: "
        "selected-provider-provides-b");
    expect(
        provides_callback_count == 1,
        "Removed Provides callback count differs");

    std::size_t metadata_callback_count = 0;
    ProviderSelectionCallback select_updated_metadata =
        [&metadata_callback_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++metadata_callback_count;
        expect(
            dependency == "selected-provider-metadata-virtual" &&
                candidates.size() == 2,
            "Updated provider metadata callback input differs");
        return candidates[1];
    };
    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        [&select_updated_metadata]() {
            static_cast<void>(resolve_build_plan(
                "selected-provider-metadata-root",
                select_updated_metadata));
        },
        "AUR provider candidate changed during dependency resolution: "
        "selected-provider-metadata-b");
    expect(
        metadata_callback_count == 1,
        "Updated provider metadata callback count differs");

    std::size_t source_callback_count = 0;
    ProviderSelectionCallback select_changed_source =
        [&source_callback_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++source_callback_count;
        expect(
            dependency == "selected-source-change-virtual" &&
                candidates.size() == 1 &&
                std::holds_alternative<AurProviderOrigin>(
                    candidates.front().origin),
            "Source-change provider callback input differs");
        return candidates.front();
    };
    dependency_plan_repository_query_stub::reset_query_counts();
    expect_exception(
        [&select_changed_source]() {
            static_cast<void>(resolve_build_plan(
                "selected-source-change-root",
                select_changed_source));
        },
        "AUR provider candidate changed during dependency resolution: "
        "selected-source-change-provider");
    expect(
        source_callback_count == 1,
        "Provider source-change callback count differs");
}

void test_unique_aur_provider_refresh_firewall() {
    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        []() {
            static_cast<void>(resolve_build_plan_for_preflight(
                {"unique-refresh-removal-root"}));
        },
        "AUR provider candidate changed during dependency resolution: "
        "unique-refresh-removal-provider");

    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    expect_exception(
        []() {
            static_cast<void>(resolve_build_plan_for_preflight(
                {"unique-refresh-name-change-root"}));
        },
        "AUR provider candidate changed during dependency resolution: "
        "unique-refresh-name-change-provider");

    dependency_plan_aur_rpc_stub::reset_selected_provider_identity_queries();
    BuildPlan query_failure = resolve_build_plan_for_preflight(
        {"unique-refresh-failure-root"});
    const BuildPlanDependencyEdge& edge = require_edge(
        query_failure, "unique-refresh-failure-root",
        "unique-refresh-failure-virtual>=1",
        PackageRole::RuntimeDependency);
    expect(
        edge.kind == DependencyKind::Unknown &&
            !edge.resolved_provider.has_value() &&
            !edge.resolved_candidate.has_value() &&
            edge.constraint_evaluation.has_value() &&
            edge.constraint_evaluation->satisfaction() ==
                ConstraintSatisfaction::Unknown &&
            edge.constraint_evaluation->unknown_reason() != nullptr &&
            *edge.constraint_evaluation->unknown_reason() ==
                ObservedVersionUnknownReason::
                    MetadataQueryFailure,
        "Provider refresh failure reused a stale Satisfied edge");
    static_cast<void>(require_resolution_failure(
        query_failure,
        BuildPlanResolutionFailureKind::
            ProviderCandidateMetadataUnavailable,
        "unique-refresh-failure-provider"));

    bool mutation_called = false;
    try {
        require_executable_build_plan(
            "unique-refresh-failure-root", query_failure);
        mutation_called = true;
    } catch(const std::exception& error) {
        expect(
            std::string(error.what()).find("Unknown") !=
                std::string::npos,
            "Refresh failure guard did not report typed Unknown");
    }
    expect(
        !mutation_called,
        "Mutation sentinel ran after provider refresh failure");
}

void test_invocation_conflicts_before_provider_prompt() {
    const auto expect_conflict_before_prompt = [](
                                                   const std::function<void(const ProviderSelectionCallback&)>&
                                                       resolve) {
        std::size_t callback_count = 0;
        ProviderSelectionCallback selector =
            [&callback_count](
                const std::string&,
                const std::vector<ProvidedDependency>& candidates)
            -> std::optional<ProvidedDependency> {
            ++callback_count;
            return candidates.front();
        };
        try {
            resolve(selector);
        } catch(const std::exception& error) {
            expect(
                std::string(error.what()).find("Conflicting") !=
                    std::string::npos,
                "Invocation conflict did not remain typed Conflicting");
            expect(
                callback_count == 0,
                "Provider prompt ran before conflict projection");
            return;
        }
        throw std::runtime_error(
            "Conflicting invocation unexpectedly resolved");
    };

    expect_conflict_before_prompt(
        [](const ProviderSelectionCallback& selector) {
            static_cast<void>(resolve_build_plan(
                "conflict-single-root", selector));
        });
    expect_conflict_before_prompt(
        [](const ProviderSelectionCallback& selector) {
            static_cast<void>(resolve_build_plan(
                std::vector<std::string>{
                    "conflict-root-a", "conflict-root-b"},
                selector));
        });
    expect_conflict_before_prompt(
        [](const ProviderSelectionCallback& selector) {
            static_cast<void>(resolve_fetch_plan(
                std::vector<std::string>{
                    "conflict-root-a", "conflict-root-b"},
                selector));
        });

    dependency_plan_repository_query_stub::reset_query_counts();
    std::size_t metadata_change_callback_count = 0;
    ProviderSelectionCallback metadata_change_selector =
        [&metadata_change_callback_count](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++metadata_change_callback_count;
        return candidates.front();
    };
    expect_exception(
        [&metadata_change_selector]() {
            static_cast<void>(resolve_build_plan(
                std::vector<std::string>{
                    "target-metadata-root-a",
                    "target-metadata-root-b"},
                metadata_change_selector));
        },
        "Provider candidates changed during invocation resolution: "
        "target-metadata-change-virtual");
    expect(
        metadata_change_callback_count == 0,
        "Target-to-target metadata change reached provider prompt");
}

void test_installed_exact_state_projection() {
    BuildPlan present = resolve_build_plan_for_preflight(
        {"installed-present-root"});
    const BuildPlanDependencyEdge& present_edge = require_edge(
        present, "installed-present-root", "installed-present>=1",
        PackageRole::RuntimeDependency);
    expect(
        present_edge.kind == DependencyKind::Installed &&
            present_edge.resolved_candidate.has_value() &&
            std::holds_alternative<InstalledExactPackage>(
                present_edge.resolved_candidate.value()) &&
            present_edge.constraint_evaluation.has_value() &&
            present_edge.constraint_evaluation->satisfaction() ==
                ConstraintSatisfaction::Satisfied,
        "Installed exact candidate lost source-aware projection");

    BuildPlan absent = resolve_build_plan_for_preflight(
        {"installed-absent-root"});
    const BuildPlanDependencyEdge& absent_edge = require_edge(
        absent, "installed-absent-root", "installed-absent",
        PackageRole::RuntimeDependency);
    expect(
        absent_edge.kind == DependencyKind::Unknown &&
            !absent_edge.constraint_evaluation.has_value() &&
            absent.resolution_failures.empty() &&
            absent.unresolved ==
                std::vector<std::string>{"installed-absent"},
        "Installed absence was conflated with query failure");

    BuildPlan query_failure = resolve_build_plan_for_preflight(
        {"installed-query-failure-root"});
    const BuildPlanDependencyEdge& failure_edge = require_edge(
        query_failure, "installed-query-failure-root",
        "installed-query-failure>=1",
        PackageRole::RuntimeDependency);
    expect(
        failure_edge.kind == DependencyKind::Unknown &&
            failure_edge.constraint_evaluation.has_value() &&
            failure_edge.constraint_evaluation->unknown_reason() !=
                nullptr &&
            *failure_edge.constraint_evaluation->unknown_reason() ==
                ObservedVersionUnknownReason::
                    MetadataQueryFailure,
        "Installed DB query failure was flattened into absence");
    static_cast<void>(require_resolution_failure(
        query_failure,
        BuildPlanResolutionFailureKind::
            InstalledPackageMetadataUnavailable,
        "installed-query-failure"));
}

void test_selection_enabled_metadata_failure_boundary() {
    std::size_t callback_count = 0;
    ProviderSelectionCallback select_provider =
        [&callback_count](
            const std::string&,
            const std::vector<ProvidedDependency>&)
        -> std::optional<ProvidedDependency> {
        ++callback_count;
        return std::nullopt;
    };

    expect_exception(
        [&select_provider]() {
            static_cast<void>(resolve_build_plan(
                "preflight-dependency-failure-root",
                select_provider));
        },
        "strict dependency metadata failure");
    expect_exception(
        [&select_provider]() {
            static_cast<void>(resolve_fetch_plan(
                "preflight-dependency-failure-root",
                select_provider));
        },
        "strict dependency metadata failure");

    AurPackageInfo aur_failure_recursive_root;
    aur_failure_recursive_root.Name =
        "selection-enabled-aur-failure-recursive-root";
    aur_failure_recursive_root.PackageBase =
        aur_failure_recursive_root.Name;
    aur_failure_recursive_root.Depends = {
        "preflight-dependency-failure-child"};
    expect_exception(
        [&aur_failure_recursive_root, &select_provider]() {
            static_cast<void>(resolve_recursive_dependencies(
                aur_failure_recursive_root, select_provider));
        },
        "strict dependency metadata failure");

    BuildPlan provider_search = resolve_build_plan(
        "preflight-provider-search-root", select_provider);
    expect(
        require_edge(
            provider_search, "preflight-provider-search-root",
            "preflight-provider-search-virtual",
            PackageRole::RuntimeDependency)
                .constraint_evaluation->unknown_reason() != nullptr,
        "Provider search failure was not retained as typed Unknown");
    BuildPlan provider_candidate = resolve_build_plan(
        "preflight-provider-candidate-root", select_provider);
    expect(
        !provider_candidate.incomplete_provider_candidate_sets.empty(),
        "Partial provider candidates were not retained");
    BuildPlan fetch_search = resolve_fetch_plan(
        "preflight-provider-search-root", select_provider);
    expect(
        has_incomplete_constraint_evaluations(fetch_search),
        "Fetch provider search failure was not typed");
    BuildPlan fetch_candidate = resolve_fetch_plan(
        "preflight-provider-candidate-root", select_provider);
    expect(
        !fetch_candidate.incomplete_provider_candidate_sets.empty(),
        "Fetch partial provider candidates were not retained");

    expect_exception(
        [&select_provider]() {
            static_cast<void>(resolve_build_plan(
                "preflight-repository-exact-failure-root",
                select_provider));
        },
        "strict repository exact metadata failure");
    expect_exception(
        [&select_provider]() {
            static_cast<void>(resolve_fetch_plan(
                "preflight-repository-exact-failure-root",
                select_provider));
        },
        "strict repository exact metadata failure");

    AurPackageInfo recursive_root;
    recursive_root.Name = "selection-enabled-recursive-root";
    recursive_root.PackageBase = recursive_root.Name;
    recursive_root.Depends = {
        "preflight-repository-exact-failure-child"};
    expect_exception(
        [&recursive_root, &select_provider]() {
            static_cast<void>(resolve_recursive_dependencies(
                recursive_root, select_provider));
        },
        "strict repository exact metadata failure");

    // No selector must use the same strict repository boundary. The AUR
    // stub rejects an unexpected legacy info call for this dependency.
    expect_exception(
        [&recursive_root]() {
            static_cast<void>(resolve_recursive_dependencies(recursive_root));
        },
        "strict repository exact metadata failure");

    AurPackageInfo selected_provider_recursive_root;
    selected_provider_recursive_root.Name =
        "selection-enabled-selected-provider-recursive-root";
    selected_provider_recursive_root.PackageBase =
        selected_provider_recursive_root.Name;
    selected_provider_recursive_root.Depends = {
        "recursive-selected-provider-failure-virtual"};
    ProviderSelectionCallback select_recursive_aur_provider =
        [&callback_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++callback_count;
        expect(
            dependency ==
                    "recursive-selected-provider-failure-virtual" &&
                candidates.size() == 2,
            "Recursive selected provider callback input differs");
        return candidates[1];
    };
    expect_exception(
        [&selected_provider_recursive_root,
         &select_recursive_aur_provider]() {
            static_cast<void>(resolve_recursive_dependencies(
                selected_provider_recursive_root,
                select_recursive_aur_provider));
        },
        "strict selected provider traversal metadata failure");

    expect(
        callback_count == 1,
        "Selection callback ran outside the selected provider traversal case");
}

void test_single_provider_callback_contract() {
    std::size_t invocation_count = 0;
    ProviderSelectionCallback selected_unique =
        [&invocation_count](
            const std::string& dependency,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++invocation_count;
        expect(
            dependency == "case14-virtual",
            "Single provider callback dependency differs");
        expect(
            candidates == std::vector<ProvidedDependency>{
                              case14_repository_provider()},
            "Single provider callback candidates differ");
        return case14_repository_provider();
    };

    BuildPlan selected = resolve_build_plan("case14-app", selected_unique);
    expect(
        invocation_count == 1,
        "Single provider callback was not invoked");
    const BuildPlanDependencyEdge& selected_edge = require_edge(
        selected, "case14-app", "case14-virtual",
        PackageRole::RuntimeDependency);
    expect(
        selected_edge.resolved_provider ==
                std::optional<ProvidedDependency>{
                    case14_repository_provider()} &&
            selected_edge.provider_resolution ==
                ProviderResolutionKind::UserSelected,
        "Single provider callback result was not retained as user-selected");
    expect(
        selected.provided.size() == 1 &&
            selected.provided[0].provider ==
                case14_repository_provider() &&
            selected.provided[0].resolution ==
                ProviderResolutionKind::UserSelected,
        "Single provider plan record was not retained as user-selected");

    ProviderSelectionCallback conflict = [](
                                             const std::string& dependency,
                                             const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        expect(
            dependency == "case14-virtual" &&
                candidates == std::vector<ProvidedDependency>{
                                  case14_repository_provider()},
            "Single provider conflict callback input differs");
        throw std::runtime_error("simulated provider selection conflict");
    };
    expect_exception(
        [&conflict]() {
            static_cast<void>(resolve_build_plan("case14-app", conflict));
        },
        "simulated provider selection conflict");
}

void test_fetch_provider_traversal_boundary() {
    BuildPlan unique = resolve_fetch_plan("case7-app");
    expect_legacy_order(unique, {"case7-app"});
    expect(
        package_target_count(unique, "case7-provider-pkg") == 0,
        "Fetch traversed a unique AUR provider");
    expect(unique.provided.size() == 1, "Fetch unique provider record is missing");
    expect(
        unique.provided[0].provider == case7_aur_provider() &&
            unique.provided[0].resolution ==
                ProviderResolutionKind::Unique,
        "Fetch unique provider record differs");

    std::size_t invocation_count = 0;
    BuildPlan selected = resolve_fetch_plan(
        "case21-app", select_case21_provider(invocation_count));
    expect(
        invocation_count == 1,
        "Fetch selected provider callback count differs");
    expect(
        package_target_count(selected, "case21-provider-b") == 1,
        "Fetch did not traverse a user-selected AUR provider");
    expect(
        package_target_count(selected, "case21-provider-child") == 1,
        "Fetch did not recurse through a selected AUR provider");
    expect(
        package_target_count(selected, "case21-provider-a") == 0,
        "Fetch traversed an unselected AUR provider");
    expect_build_unit_before(
        selected, "case21-provider-child", "case21-provider-suite");
    expect_build_unit_before(
        selected, "case21-provider-suite", "case21-app");
    require_fetchable_build_plan("case21-app", selected);
}

void test_case_9_unknown_dependency() {
    BuildPlan plan = resolve_build_plan("case9-app");
    const BuildPlanDependencyEdge& edge = require_edge(
        plan, "case9-app", "case9-missing", PackageRole::RuntimeDependency);
    expect(edge.kind == DependencyKind::Unknown, "Case 9 edge kind differs");
    expect(!edge.resolved_package_name.has_value(), "Case 9 resolved name must be empty");
    expect(!edge.resolved_package_base.has_value(), "Case 9 resolved base must be empty");
    expect(!edge.resolved_provider.has_value(), "Case 9 provider must be empty");
    expect(
        plan.unresolved == std::vector<std::string>{"case9-missing"},
        "Case 9 unresolved contract differs");
}

void test_case_10_dependency_chain() {
    BuildPlan plan = resolve_build_plan("case10-app");
    expect_roots(require_package_target(plan, "case10-lib"), {{0, "case10-app"}});
    expect_roots(require_package_target(plan, "case10-common"), {{0, "case10-app"}});
    static_cast<void>(require_edge(
        plan, "case10-app", "case10-lib", PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case10-lib", "case10-common", PackageRole::RuntimeDependency));
    expect_legacy_order(plan, {"case10-common", "case10-lib", "case10-app"});
}

void test_case_11_single_overload_compatibility() {
    BuildPlan single = resolve_build_plan("case11-root");
    BuildPlan multiple = resolve_build_plan(
        std::vector<std::string>{"case11-root"});
    expect(!single.split_package_targets.empty(), "Case 11 split fixture is empty");
    expect(!single.provided.empty(), "Case 11 provider fixture is empty");
    expect(!single.metadata_risks.empty(), "Case 11 risk fixture is empty");
    expect(
        !single.relation_assessments.empty(),
        "Case 11 relation assessment fixture is empty");
    expect(!single.ambiguous_providers.empty(), "Case 11 ambiguous fixture is empty");
    expect(!single.unresolved.empty(), "Case 11 unresolved fixture is empty");
    expect(!single.cycles.empty(), "Case 11 cycle fixture is empty");
    expect(!single.dependency_edges.empty(), "Case 11 edge fixture is empty");
    const PlannedPackageTarget& root = require_package_target(single, "case11-root");
    expect_roles(root, {PackageRole::Root, PackageRole::RuntimeDependency});
    expect_roots(root, {{0, "case11-root"}});
    const PlannedPackageTarget& direct = require_package_target(single, "case11-direct");
    expect(
        direct.package_base == "case11-direct-base",
        "Case 11 planned package base differs");
    const BuildPlanDependencyEdge& direct_edge = require_edge(
        single, "case11-root", "case11-direct", PackageRole::RuntimeDependency);
    expect_direct_aur_resolution(direct_edge, "case11-direct", "case11-direct-base");
    expect(single.order.size() == 4, "Case 11 legacy order size differs");
    expect(
        single.order[0].package_base == "case11-direct-base" &&
            single.order[0].package_names ==
                std::vector<std::string>{"case11-direct"},
        "Case 11 split legacy entry differs");
    expect_equivalent_plans(single, multiple);
}

void test_case_12_invalid_reducer_state() {
    PlannedPackageTarget target{"case12-invalid", "case12-invalid", {}, {}};
    expect_failure(
        [&target]() { static_cast<void>(desired_install_reason(target)); },
        "role-less desired reason");
}

void test_supplemental_multi_root_contracts() {
    expect_failure(
        []() { static_cast<void>(resolve_build_plan(std::vector<std::string>{})); },
        "empty multi-root build plan");

    BuildPlan transitive = resolve_build_plan(
        std::vector<std::string>{"case13-app", "case13-lib"});
    expect_roots(
        require_package_target(transitive, "case13-common"),
        {{0, "case13-app"}, {1, "case13-lib"}});

    BuildPlan duplicate = resolve_build_plan(
        std::vector<std::string>{"case1-app", "case1-app"});
    expect(
        duplicate.root_targets ==
            std::vector<RootTargetIdentity>{{0, "case1-app"}, {1, "case1-app"}},
        "Duplicate root identities were not preserved");
    expect_roots(
        require_package_target(duplicate, "case1-app"),
        {{0, "case1-app"}, {1, "case1-app"}});
    expect_legacy_order(duplicate, {"case1-app"});

    BuildPlan repository_provider = resolve_build_plan("case14-app");
    const BuildPlanDependencyEdge& provider_edge = require_edge(
        repository_provider, "case14-app", "case14-virtual",
        PackageRole::RuntimeDependency);
    expect(provider_edge.kind == DependencyKind::Provided, "Repository provider kind differs");
    expect(provider_edge.resolved_provider.has_value(), "Repository provider is missing");
    expect(
        provider_edge.resolved_provider.value() ==
            case14_repository_provider(),
        "Repository provider result differs");
    const auto* repository_origin = std::get_if<RepositoryProviderOrigin>(
        &provider_edge.resolved_provider->origin);
    expect(
        repository_origin != nullptr &&
            repository_origin->repository_name == "aur",
        "Repository provider origin differs");
    expect(
        provided_dependency_display(
            provider_edge.resolved_provider.value()) ==
            "aur/case14-provider",
        "Repository provider display differs");
    expect(
        package_target_count(repository_provider, "case14-provider") == 0,
        "Repository provider leaked into source package targets");
    expect(
        repository_provider.package_targets.size() == 1,
        "Repository provider changed source target count");
    expect_roots(
        require_package_target(repository_provider, "case14-app"),
        {{0, "case14-app"}});
    expect_legacy_order(repository_provider, {"case14-app"});

    BuildPlan all_roles = resolve_build_plan(
        std::vector<std::string>{"case15-app", "case15-shared"});
    const PlannedPackageTarget& shared = require_package_target(all_roles, "case15-shared");
    expect_roles(
        shared,
        {PackageRole::Root, PackageRole::RuntimeDependency,
         PackageRole::BuildDependency, PackageRole::CheckDependency});
    expect(
        desired_install_reason(shared) == DesiredInstallReason::Explicit,
        "All-role explicit-wins reducer differs");
}

void test_same_package_base_root_coverage() {
    BuildPlan plan = resolve_build_plan(
        std::vector<std::string>{"case16-child-a", "case16-child-b"});
    expect(
        plan.root_targets == std::vector<RootTargetIdentity>{
                                 {0, "case16-child-a"},
                                 {1, "case16-child-b"},
                             },
        "Same-base root identities differ");

    const BuildPlanEntry& suite =
        require_build_plan_entry(plan, "case16-suite");
    expect(
        suite.package_names == std::vector<std::string>{
                                   "case16-child-a", "case16-child-b"},
        "Same-base required child order differs");
    expect(plan.order.size() == 4, "Same-base build unit count differs");

    const PlannedPackageTarget& child_a =
        require_package_target(plan, "case16-child-a");
    const PlannedPackageTarget& child_b =
        require_package_target(plan, "case16-child-b");
    const PlannedPackageTarget& shared =
        require_package_target(plan, "case16-shared");
    expect(plan.package_targets.size() == 5, "Same-base package target count differs");
    expect(package_target_count(plan, "case16-child-a") == 1, "First child target was duplicated");
    expect(package_target_count(plan, "case16-child-b") == 1, "Second child target was duplicated");
    expect(package_target_count(plan, "case16-shared") == 1, "Shared dependency target was duplicated");
    expect(child_a.package_base == "case16-suite", "First child PackageBase differs");
    expect(child_b.package_base == "case16-suite", "Second child PackageBase differs");
    expect_roles(child_a, {PackageRole::Root});
    expect_roles(child_b, {PackageRole::Root});
    expect_roles(shared, {PackageRole::RuntimeDependency});
    expect_roots(child_a, {{0, "case16-child-a"}});
    expect_roots(child_b, {{1, "case16-child-b"}});
    expect_roots(
        shared,
        {{0, "case16-child-a"}, {1, "case16-child-b"}});

    static_cast<void>(require_edge(
        plan, "case16-child-a", "case16-a-only",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case16-child-a", "case16-shared",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case16-child-b", "case16-b-only",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case16-child-b", "case16-shared",
        PackageRole::RuntimeDependency));
    expect(plan.dependency_edges.size() == 4, "Same-base dependency edge count differs");

    expect_build_unit_before(plan, "case16-a-only", "case16-suite");
    expect_build_unit_before(plan, "case16-b-only", "case16-suite");
    expect_build_unit_before(plan, "case16-shared", "case16-suite");
    expect(plan.cycles.empty(), "Independent same-base roots produced a cycle");
}

void test_same_package_base_sibling_attribution() {
    BuildPlan plan = resolve_build_plan(std::vector<std::string>{
        "case17-parent", "case17-parent", "case17-sibling"});
    expect(
        plan.root_targets == std::vector<RootTargetIdentity>{
                                 {0, "case17-parent"},
                                 {1, "case17-parent"},
                                 {2, "case17-sibling"},
                             },
        "Same-base duplicate root identities differ");

    const BuildPlanEntry& suite =
        require_build_plan_entry(plan, "case17-suite");
    expect(
        suite.package_names == std::vector<std::string>{
                                   "case17-parent", "case17-sibling"},
        "Same-base sibling order or deduplication differs");
    expect(plan.order.size() == 2, "Same-base sibling build unit count differs");
    expect(plan.package_targets.size() == 3, "Same-base sibling target count differs");
    expect(package_target_count(plan, "case17-parent") == 1, "Parent target was duplicated");
    expect(package_target_count(plan, "case17-sibling") == 1, "Sibling target was duplicated");

    const PlannedPackageTarget& parent =
        require_package_target(plan, "case17-parent");
    const PlannedPackageTarget& sibling =
        require_package_target(plan, "case17-sibling");
    const PlannedPackageTarget& leaf =
        require_package_target(plan, "case17-leaf");
    expect_roles(parent, {PackageRole::Root});
    expect_roles(
        sibling,
        {PackageRole::Root, PackageRole::RuntimeDependency});
    expect_roles(leaf, {PackageRole::RuntimeDependency});
    expect_roots(
        sibling,
        {
            {0, "case17-parent"},
            {1, "case17-parent"},
            {2, "case17-sibling"},
        });
    expect_roots(
        leaf,
        {
            {0, "case17-parent"},
            {1, "case17-parent"},
            {2, "case17-sibling"},
        });
    expect(
        desired_install_reason(sibling) == DesiredInstallReason::Explicit,
        "Same-base root/dependency reason did not prefer Explicit");

    static_cast<void>(require_edge(
        plan, "case17-parent", "case17-sibling",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case17-sibling", "case17-leaf",
        PackageRole::RuntimeDependency));
    expect_build_unit_before(plan, "case17-leaf", "case17-suite");
    expect(plan.cycles.empty(), "Same-base sibling dependency produced a false cycle");
}

void test_real_dependency_cycle_is_preserved() {
    BuildPlan plan = resolve_build_plan("case18-cycle-a");
    static_cast<void>(require_edge(
        plan, "case18-cycle-a", "case18-cycle-b",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case18-cycle-b", "case18-cycle-a",
        PackageRole::RuntimeDependency));
    expect(!plan.cycles.empty(), "Real dependency cycle was not preserved");
}

void test_same_package_base_real_cycle_is_preserved() {
    BuildPlan plan = resolve_build_plan("case20-cycle-a");
    static_cast<void>(require_edge(
        plan, "case20-cycle-a", "case20-cycle-b",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case20-cycle-b", "case20-cycle-a",
        PackageRole::RuntimeDependency));
    expect(
        plan.order.size() == 1 &&
            plan.order.front().package_base == "case20-suite" &&
            plan.order.front().package_names ==
                std::vector<std::string>{
                    "case20-cycle-a", "case20-cycle-b"},
        "Same-base real cycle lost its exact execution unit");
    expect(
        plan.cycles == std::vector<std::string>{"case20-suite"},
        "Same-base real cycle was not preserved exactly once");
}

void test_same_package_base_late_dependency_ordering() {
    BuildPlan plan = resolve_build_plan(std::vector<std::string>{
        "case19-consumer", "case19-suite-b"});
    const BuildPlanEntry& suite =
        require_build_plan_entry(plan, "case19-suite");
    expect(
        suite.package_names == std::vector<std::string>{
                                   "case19-suite-a", "case19-suite-b"},
        "Late same-base child aggregation differs");
    expect(plan.package_targets.size() == 5, "Late dependency target count differs");
    expect(package_target_count(plan, "case19-suite-a") == 1, "Early suite target was duplicated");
    expect(package_target_count(plan, "case19-suite-b") == 1, "Late suite target was duplicated");

    static_cast<void>(require_edge(
        plan, "case19-consumer", "case19-suite-a",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case19-suite-a", "case19-early-dep",
        PackageRole::RuntimeDependency));
    static_cast<void>(require_edge(
        plan, "case19-suite-b", "case19-late-dep",
        PackageRole::RuntimeDependency));

    // LANDMINE(#268): a late child may add dependencies after a consumer was first traversed.
    // Moving only the existing PackageBase entry would invert the consumer dependency.
    expect_build_unit_before(plan, "case19-early-dep", "case19-suite");
    expect_build_unit_before(plan, "case19-late-dep", "case19-suite");
    expect_build_unit_before(plan, "case19-suite", "case19-consumer");
    expect(plan.cycles.empty(), "Late same-base child produced a cycle");
}

void test_preflight_root_failure_and_continuation() {
    BuildPlan plan = resolve_build_plan_for_preflight(
        {"preflight-root-metadata-failure", "preflight-later-root"});

    expect(
        plan.root_targets == std::vector<RootTargetIdentity>{
                                 {0, "preflight-root-metadata-failure"},
                                 {1, "preflight-later-root"},
                             },
        "Preflight root identities differ after root failure");
    expect(plan.resolution_failures.size() == 1, "Unexpected root failure count");
    const BuildPlanResolutionFailure& failure = require_resolution_failure(
        plan, BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
        "preflight-root-metadata-failure");
    expect_resolution_failure_context(
        failure, std::nullopt, std::nullopt, std::nullopt,
        {{0, "preflight-root-metadata-failure"}},
        "strict root metadata failure");
    expect(
        plan.unresolved ==
            std::vector<std::string>{"preflight-root-metadata-failure"},
        "Root metadata failure did not remain unresolved");
    expect_roots(
        require_package_target(plan, "preflight-later-root"),
        {{1, "preflight-later-root"}});
    expect_legacy_order(plan, {"preflight-later-root"});

    BuildPlan duplicate_failure = resolve_build_plan_for_preflight({
        "preflight-root-metadata-failure",
        "preflight-root-metadata-failure",
    });
    expect(
        duplicate_failure.resolution_failures.size() == 1,
        "Duplicate ordinary failure was not deduplicated");
    expect(
        duplicate_failure.resolution_failures.front().roots ==
            std::vector<RootTargetIdentity>{
                {0, "preflight-root-metadata-failure"},
                {1, "preflight-root-metadata-failure"},
            },
        "Deduplicated failure roots differ");
}

void test_preflight_dependency_and_provider_failures() {
    BuildPlan dependency = resolve_build_plan_for_preflight(
        {"preflight-dependency-failure-root"});
    expect(
        dependency.resolution_failures.size() == 1,
        "Unexpected dependency metadata failure count");
    const BuildPlanResolutionFailure& dependency_failure =
        require_resolution_failure(
            dependency,
            BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
            "preflight-dependency-failure-child");
    expect_resolution_failure_context(
        dependency_failure,
        std::optional<std::string>{"preflight-dependency-failure-root"},
        std::optional<std::string>{"preflight-dependency-failure-root"},
        std::optional<std::string>{"preflight-dependency-failure-child"},
        {{0, "preflight-dependency-failure-root"}},
        "strict dependency metadata failure");
    expect(
        require_edge(
            dependency, "preflight-dependency-failure-root",
            "preflight-dependency-failure-child",
            PackageRole::RuntimeDependency)
                .kind == DependencyKind::Unknown,
        "Dependency metadata failure was treated as resolved");

    std::size_t fallback_callback_count = 0;
    ProviderSelectionCallback reject_provider_fallback =
        [&fallback_callback_count](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++fallback_callback_count;
        return candidates.front();
    };
    BuildPlan exact_failure = resolve_build_plan_for_preflight(
        {"preflight-exact-failure-no-provider-fallback-root"},
        reject_provider_fallback);
    expect(
        fallback_callback_count == 0,
        "Preflight prompted for a provider after exact metadata failure");
    const BuildPlanResolutionFailure& exact_failure_record =
        require_resolution_failure(
            exact_failure,
            BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
            "preflight-exact-failure-no-provider-fallback");
    expect_resolution_failure_context(
        exact_failure_record,
        std::optional<std::string>{
            "preflight-exact-failure-no-provider-fallback-root"},
        std::optional<std::string>{
            "preflight-exact-failure-no-provider-fallback-root"},
        std::optional<std::string>{
            "preflight-exact-failure-no-provider-fallback"},
        {{0, "preflight-exact-failure-no-provider-fallback-root"}},
        "strict exact metadata failure before provider fallback");
    expect(
        require_edge(
            exact_failure,
            "preflight-exact-failure-no-provider-fallback-root",
            "preflight-exact-failure-no-provider-fallback",
            PackageRole::RuntimeDependency)
                .kind == DependencyKind::Unknown,
        "Preflight exact metadata failure fell back to a provider");
    expect(
        exact_failure.provided.empty(),
        "Preflight retained a provider after exact metadata failure");

    BuildPlan provider_search = resolve_build_plan_for_preflight(
        {"preflight-provider-search-root"});
    expect(
        provider_search.resolution_failures.size() == 1,
        "Unexpected provider search failure count");
    const BuildPlanResolutionFailure& search_failure =
        require_resolution_failure(
            provider_search,
            BuildPlanResolutionFailureKind::ProviderSearchUnavailable,
            "preflight-provider-search-virtual");
    expect_resolution_failure_context(
        search_failure,
        std::optional<std::string>{"preflight-provider-search-root"},
        std::optional<std::string>{"preflight-provider-search-root"},
        std::optional<std::string>{"preflight-provider-search-virtual"},
        {{0, "preflight-provider-search-root"}},
        "strict provider search failure");

    std::size_t partial_candidate_callback_count = 0;
    ProviderSelectionCallback reject_partial_candidates =
        [&partial_candidate_callback_count](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++partial_candidate_callback_count;
        return candidates.front();
    };
    BuildPlan provider_candidate = resolve_build_plan_for_preflight(
        {"preflight-provider-candidate-root"},
        reject_partial_candidates);
    expect(
        provider_candidate.resolution_failures.size() == 1,
        "Unexpected provider candidate failure count");
    const BuildPlanResolutionFailure& candidate_failure =
        require_resolution_failure(
            provider_candidate,
            BuildPlanResolutionFailureKind::ProviderCandidateMetadataUnavailable,
            "preflight-provider-broken");
    expect_resolution_failure_context(
        candidate_failure,
        std::optional<std::string>{"preflight-provider-candidate-root"},
        std::optional<std::string>{"preflight-provider-candidate-root"},
        std::optional<std::string>{"preflight-provider-candidate-virtual"},
        {{0, "preflight-provider-candidate-root"}},
        "strict provider candidate failure");
    expect(
        partial_candidate_callback_count == 0,
        "Preflight exposed a partial provider candidate set");
    const BuildPlanDependencyEdge& provider_edge = require_edge(
        provider_candidate, "preflight-provider-candidate-root",
        "preflight-provider-candidate-virtual",
        PackageRole::RuntimeDependency);
    expect(
        provider_edge.kind == DependencyKind::Unknown &&
            !provider_edge.resolved_provider.has_value() &&
            provider_edge.constraint_evaluation.has_value() &&
            provider_edge.constraint_evaluation->unknown_reason() !=
                nullptr &&
            *provider_edge.constraint_evaluation->unknown_reason() ==
                ObservedVersionUnknownReason::
                    PartialSourceFailure,
        "Partial provider candidates were treated as authoritative");
    expect(
        provider_candidate.unresolved == std::vector<std::string>{
                                             "preflight-provider-candidate-virtual"},
        "Incomplete provider candidates did not remain unresolved");
    expect(
        provider_candidate.incomplete_provider_candidate_sets.size() ==
                1 &&
            provider_candidate.incomplete_provider_candidate_sets
                    .front()
                    .reason ==
                ObservedVersionUnknownReason::
                    PartialSourceFailure &&
            provider_candidate.incomplete_provider_candidate_sets
                    .front()
                    .observed_candidates.size() == 1 &&
            provider_candidate.incomplete_provider_candidate_sets
                    .front()
                    .observed_candidates.front()
                    .package_name ==
                "preflight-provider-good",
        "Partial AUR provider observations were discarded");

    std::size_t repository_partial_callback_count = 0;
    ProviderSelectionCallback reject_partial_repository_candidates =
        [&repository_partial_callback_count](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
        -> std::optional<ProvidedDependency> {
        ++repository_partial_callback_count;
        return candidates.front();
    };
    BuildPlan repository_partial = resolve_build_plan_for_preflight(
        {"preflight-repository-partial-provider-root"},
        reject_partial_repository_candidates);
    expect(
        repository_partial_callback_count == 0,
        "Partial repository provider set reached the prompt");
    const BuildPlanDependencyEdge& repository_partial_edge = require_edge(
        repository_partial,
        "preflight-repository-partial-provider-root",
        "preflight-repository-partial-provider-virtual>=1",
        PackageRole::RuntimeDependency);
    expect(
        repository_partial_edge.kind == DependencyKind::Unknown &&
            repository_partial_edge.constraint_evaluation.has_value() &&
            repository_partial_edge.constraint_evaluation
                    ->unknown_reason() != nullptr &&
            *repository_partial_edge.constraint_evaluation
                    ->unknown_reason() ==
                ObservedVersionUnknownReason::
                    PartialSourceFailure &&
            repository_partial.incomplete_provider_candidate_sets
                    .size() == 1 &&
            repository_partial.incomplete_provider_candidate_sets
                    .front()
                    .observed_candidates.front()
                    .package_name ==
                "partial-repository-provider",
        "Partial repository provider observation was flattened");

    bool mutation_called = false;
    try {
        require_executable_build_plan(
            "preflight-provider-candidate-root",
            provider_candidate);
        mutation_called = true;
    } catch(const std::exception&) {
    }
    expect(
        !mutation_called,
        "Mutation sentinel ran with an incomplete AUR provider set");

    try {
        require_executable_build_plan(
            "preflight-repository-partial-provider-root",
            repository_partial);
        mutation_called = true;
    } catch(const std::exception&) {
    }
    expect(
        !mutation_called,
        "Mutation sentinel ran with an incomplete repository provider set");
}

void test_preflight_shared_failure_root_attribution() {
    BuildPlan plan = resolve_build_plan_for_preflight(
        {"preflight-shared-root-a", "preflight-shared-root-b"});

    expect(plan.resolution_failures.size() == 1, "Unexpected shared failure count");
    const BuildPlanResolutionFailure& failure = require_resolution_failure(
        plan, BuildPlanResolutionFailureKind::AurPackageMetadataUnavailable,
        "preflight-shared-failure");
    expect_resolution_failure_context(
        failure,
        std::optional<std::string>{"preflight-shared-parent"},
        std::optional<std::string>{"preflight-shared-parent"},
        std::optional<std::string>{"preflight-shared-failure"},
        {
            {0, "preflight-shared-root-a"},
            {1, "preflight-shared-root-b"},
        },
        "strict shared dependency metadata failure");
    expect_roots(
        require_package_target(plan, "preflight-shared-parent"),
        {
            {0, "preflight-shared-root-a"},
            {1, "preflight-shared-root-b"},
        });
}

void test_preflight_response_error_propagation() {
    expect_aur_rpc_response_error(
        []() {
            static_cast<void>(resolve_build_plan_for_preflight(
                {"preflight-response-error-root"}));
        },
        "root response failure");
    expect_aur_rpc_response_error(
        []() {
            static_cast<void>(resolve_build_plan_for_preflight(
                {"preflight-provider-response-root"}));
        },
        "provider search response failure");
    expect_aur_rpc_response_error(
        []() {
            static_cast<void>(resolve_build_plan_for_preflight({
                "preflight-dependency-response-root",
                "preflight-response-must-stop-before-later-root",
            }));
        },
        "dependency info response failure");
    expect_aur_rpc_response_error(
        []() {
            static_cast<void>(resolve_build_plan_for_preflight({
                "preflight-provider-candidate-response-root",
                "preflight-response-must-stop-before-later-root",
            }));
        },
        "provider candidate info response failure");
}

void test_preflight_repository_query_boundary() {
    dependency_plan_repository_query_stub::reset_query_counts();
    BuildPlan repository_dependency =
        resolve_build_plan_for_preflight({"case6-app"});
    expect(
        require_edge(
            repository_dependency, "case6-app", "case6-repo-lib",
            PackageRole::RuntimeDependency)
                .kind == DependencyKind::Repo,
        "Strict resolver lost an exact repository dependency");
    expect(
        dependency_plan_repository_query_stub::sync_database_package_query_count() == 1,
        "Strict resolver did not use the sync database package query");

    BuildPlan repository_provider =
        resolve_build_plan_for_preflight({"case14-app"});
    expect(
        require_edge(
            repository_provider, "case14-app", "case14-virtual",
            PackageRole::RuntimeDependency)
                .kind == DependencyKind::Provided,
        "Strict resolver lost the pacman-first repository provider");
    expect(
        repository_provider.resolution_failures.empty(),
        "Repository provider lookup produced a strict AUR failure");
    expect(
        dependency_plan_repository_query_stub::strict_repo_provider_query_count() == 1,
        "Strict resolver did not use the typed repository provider query");
}

void test_preflight_repository_metadata_failures() {
    dependency_plan_repository_query_stub::reset_query_counts();
    BuildPlan exact_failure = resolve_build_plan_for_preflight({
        "preflight-repository-exact-failure-root",
        "preflight-later-root",
    });
    expect(
        exact_failure.resolution_failures.size() == 1,
        "Unexpected strict repository exact failure count");
    const BuildPlanResolutionFailure& exact = require_resolution_failure(
        exact_failure,
        BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
        "preflight-repository-exact-failure-child");
    expect_resolution_failure_context(
        exact,
        std::optional<std::string>{"preflight-repository-exact-failure-root"},
        std::optional<std::string>{"preflight-repository-exact-failure-root"},
        std::optional<std::string>{"preflight-repository-exact-failure-child"},
        {{0, "preflight-repository-exact-failure-root"}},
        "strict repository exact metadata failure");
    expect(
        require_edge(
            exact_failure,
            "preflight-repository-exact-failure-root",
            "preflight-repository-exact-failure-child",
            PackageRole::RuntimeDependency)
                .kind == DependencyKind::Unknown,
        "Unavailable repository exact metadata was treated as confirmed absence");
    expect_roots(
        require_package_target(exact_failure, "preflight-later-root"),
        {{1, "preflight-later-root"}});
    expect(
        dependency_plan_repository_query_stub::strict_repo_provider_query_count() == 0,
        "Repository exact failure continued into provider lookup");

    dependency_plan_repository_query_stub::reset_query_counts();
    BuildPlan provider_failure = resolve_build_plan_for_preflight(
        {"preflight-repository-provider-failure-root"});
    expect(
        provider_failure.resolution_failures.size() == 1,
        "Unexpected strict repository provider failure count");
    const BuildPlanResolutionFailure& provider = require_resolution_failure(
        provider_failure,
        BuildPlanResolutionFailureKind::RepositoryMetadataUnavailable,
        "preflight-repository-provider-failure-virtual");
    expect_resolution_failure_context(
        provider,
        std::optional<std::string>{"preflight-repository-provider-failure-root"},
        std::optional<std::string>{"preflight-repository-provider-failure-root"},
        std::optional<std::string>{"preflight-repository-provider-failure-virtual"},
        {{0, "preflight-repository-provider-failure-root"}},
        "strict repository provider metadata failure");
    expect(
        require_edge(
            provider_failure,
            "preflight-repository-provider-failure-root",
            "preflight-repository-provider-failure-virtual",
            PackageRole::RuntimeDependency)
                .kind == DependencyKind::Unknown,
        "Unavailable repository provider metadata was treated as an empty provider set");
    expect(
        dependency_plan_repository_query_stub::strict_repo_provider_query_count() == 1,
        "Strict repository provider query count differs after failure");
}

void test_legacy_resolution_failure_boundary() {
    BuildPlan normal = resolve_build_plan("case1-app");
    expect(
        normal.resolution_failures.empty(),
        "Legacy resolver added failures to a successful plan");

    BuildPlan dependency = resolve_build_plan(
        "preflight-dependency-failure-root");
    expect(
        dependency.resolution_failures.empty(),
        "Legacy resolver captured dependency metadata failure");
    expect(
        dependency.unresolved ==
            std::vector<std::string>{"preflight-dependency-failure-child"},
        "Legacy dependency failure behavior changed");

    BuildPlan provider_search = resolve_build_plan(
        "preflight-provider-search-root");
    expect(
        has_incomplete_constraint_evaluations(provider_search),
        "Legacy provider search failure was not retained as Unknown");

    BuildPlan provider_candidate = resolve_build_plan(
        "preflight-provider-candidate-root");
    expect(
        provider_candidate.incomplete_provider_candidate_sets.size() ==
                1 &&
            provider_candidate.incomplete_provider_candidate_sets
                    .front()
                    .observed_candidates.size() == 1,
        "Legacy partial provider observations were discarded");

    expect_exception(
        []() {
            static_cast<void>(resolve_build_plan(
                "preflight-root-metadata-failure"));
        },
        "legacy root metadata failure");
    expect_exception(
        []() {
            static_cast<void>(resolve_build_plan("preflight-root-not-found"));
        },
        "AUR package not found: preflight-root-not-found");
}

void test_planned_relation_observation_authorities() {
    const BuildPlan root_plan = resolve_build_plan("case11-root");
    const PlannedPackageRelationObservation& root_observation =
        require_relation_observation(root_plan, "case11-root");
    expect(
        root_observation.package.package_base ==
                std::optional<std::string>("case11-root") &&
            root_observation.package.package_version.version() !=
                nullptr &&
            *root_observation.package.package_version.version() ==
                "1.0-1" &&
            std::get<PackageRelationAurSourceIdentity>(
                root_observation.package.source) ==
                PackageRelationAurSourceIdentity{
                    "case11-root", "case11-root"} &&
            root_observation.package.roots ==
                std::vector<PackageRelationRootAttribution>{
                    {0, "case11-root"}} &&
            root_observation.declarations.size() == 1 &&
            root_observation.declarations.front().kind() ==
                PackageRelationKind::Conflict &&
            root_observation.declarations.front()
                    .target_component() == "case11-old",
        "AUR root observation did not retain direct metadata authority");

    const PlannedPackageRelationObservation& dependency_observation =
        require_relation_observation(root_plan, "case11-direct");
    expect(
        dependency_observation.package.package_base ==
                std::optional<std::string>(
                    "case11-direct-base") &&
            dependency_observation.package.package_version.version() !=
                nullptr &&
            *dependency_observation.package.package_version.version() ==
                "1.0-1" &&
            dependency_observation.package.roots ==
                std::vector<PackageRelationRootAttribution>{
                    {0, "case11-root"}} &&
            dependency_observation.declarations.size() == 1 &&
            dependency_observation.declarations.front().kind() ==
                PackageRelationKind::Replacement &&
            dependency_observation.declarations.front()
                    .target_component() ==
                "case11-replaced",
        "AUR dependency observation lost PackageBase/version/root/relation");

    const BuildPlan split_plan = resolve_build_plan(
        std::vector<std::string>{
            "case16-child-a", "case16-child-b"});
    const auto& split_a =
        require_relation_observation(split_plan, "case16-child-a");
    const auto& split_b =
        require_relation_observation(split_plan, "case16-child-b");
    const auto& split_shared =
        require_relation_observation(split_plan, "case16-shared");
    expect(
        split_a.package.package_base ==
                std::optional<std::string>("case16-suite") &&
            split_b.package.package_base ==
                std::optional<std::string>("case16-suite") &&
            split_a.package.package_name !=
                split_b.package.package_name &&
            std::get<PackageRelationAurSourceIdentity>(
                split_a.package.source)
                    .package_name == "case16-child-a" &&
            std::get<PackageRelationAurSourceIdentity>(
                split_b.package.source)
                    .package_name == "case16-child-b",
        "Split PackageBase siblings were flattened");
    expect(
        split_shared.package.roots ==
            std::vector<PackageRelationRootAttribution>{
                {0, "case16-child-a"},
                {1, "case16-child-b"}},
        "Repeated planned observation lost root attribution");

    const BuildPlan aur_provider_plan = resolve_build_plan("case7-app");
    const auto& aur_provider = require_relation_observation(
        aur_provider_plan, "case7-provider-pkg");
    expect(
        aur_provider.package.package_version.version() != nullptr &&
            *aur_provider.package.package_version.version() ==
                "1.0-1" &&
            aur_provider.package.provides.size() == 1 &&
            aur_provider.package.provides.front()
                    .capability.package_name() ==
                "case7-virtual-api" &&
            aur_provider.package.provides.front()
                    .observed_version.unknown_reason() !=
                nullptr &&
            *aur_provider.package.provides.front()
                    .observed_version.unknown_reason() ==
                ObservedVersionUnknownReason::
                    UnversionedProviderCapability,
        "AUR provider observation lost package/provided versions");

    const BuildPlan repository_plan = resolve_build_plan("case6-app");
    const auto& repository = require_relation_observation(
        repository_plan, "case6-repo-lib");
    expect(
        repository.package.package_base ==
                std::optional<std::string>("case6-repo-lib") &&
            repository.package.package_version.version() != nullptr &&
            *repository.package.package_version.version() ==
                "1.0-1" &&
            std::get<ConfiguredRepositoryIdentity>(
                repository.package.source) ==
                ConfiguredRepositoryIdentity{"core", 0} &&
            repository.package.roots ==
                std::vector<PackageRelationRootAttribution>{
                    {0, "case6-app"}},
        "Repository package observation lost source/version/root");

    const BuildPlan repository_provider_plan =
        resolve_build_plan("case23-app");
    const auto& repository_provider = require_relation_observation(
        repository_provider_plan, "case23-provider");
    expect(
        repository_provider.package.package_base ==
                std::optional<std::string>(
                    "case23-provider-base") &&
            repository_provider.package.package_version.version() !=
                nullptr &&
            *repository_provider.package.package_version.version() ==
                "5.0-1" &&
            repository_provider.package.provides.size() == 1 &&
            repository_provider.package.provides.front()
                    .capability.raw_specification() ==
                "case23-virtual=6" &&
            repository_provider.package.provides.front()
                    .observed_version.version() != nullptr &&
            *repository_provider.package.provides.front()
                    .observed_version.version() == "6" &&
            std::get<ConfiguredRepositoryIdentity>(
                repository_provider.package.source) ==
                ConfiguredRepositoryIdentity{"extra", 1} &&
            repository_provider.package.roots ==
                std::vector<PackageRelationRootAttribution>{
                    {0, "case23-app"}},
        "Repository provider observation lost source/provides/root");
}

template <typename Callable>
void run_case(const std::string& name, Callable callable) {
    callable();
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        run_case(
            "provider origin value contract",
            test_provider_origin_value_contract);
        run_case(
            "typed constraint edge and mutation firewall",
            test_typed_constraint_edge_and_mutation_firewall);
        run_case(
            "invalid/conflicting/source identity fail closed",
            test_invalid_conflicting_and_source_identity_fail_closed);
        run_case(
            "verified InstalledExact dependency satisfaction matrix",
            test_verified_installed_exact_dependency_satisfaction_matrix);
        run_case(
            "execution guard category order",
            test_execution_guard_category_order);
        run_case(
            "plan construction/completeness/readiness axes",
            test_plan_state_projection_keeps_capability_axes);
        run_case(
            "relation assessment readiness and completeness mapping",
            test_relation_assessment_readiness_and_completeness_mapping);
        run_case(
            "split package install readiness is orthogonal to completeness",
            test_split_package_install_readiness_is_orthogonal_to_completeness);
        run_case("Case 1 root only", test_case_1_root_only);
        run_case("Case 2 dependency roles", test_case_2_dependency_roles);
        run_case("Case 3 multiple roles", test_case_3_multiple_roles);
        run_case("Case 4 root/dependency overlap", test_case_4_root_dependency_overlap);
        run_case("Case 5 shared dependency", test_case_5_shared_dependency);
        run_case("Case 6 repository dependency", test_case_6_repository_dependency);
        run_case("Case 7 unique AUR provider", test_case_7_unique_aur_provider);
        run_case("Case 8 ambiguous provider", test_case_8_ambiguous_provider);
        run_case(
            "Case 8 selected repository provider",
            test_case_8_selected_repository_provider);
        run_case(
            "Case 8 cancel and unoffered selection",
            test_case_8_cancel_and_unoffered_selection);
        run_case(
            "Case 21 selected AUR provider",
            test_case_21_selected_aur_provider);
        run_case(
            "provider candidates dedupe by source identity",
            test_provider_candidates_dedupe_by_source_identity);
        run_case(
            "selected provider package identity conflict guard",
            test_selected_provider_package_identity_conflict_guard);
        run_case(
            "selected AUR provider revalidation boundary",
            test_selected_aur_provider_revalidation_boundary);
        run_case(
            "unique AUR provider refresh firewall",
            test_unique_aur_provider_refresh_firewall);
        run_case(
            "invocation conflicts precede provider prompt",
            test_invocation_conflicts_before_provider_prompt);
        run_case(
            "installed exact state projection",
            test_installed_exact_state_projection);
        run_case(
            "selection-enabled metadata failure boundary",
            test_selection_enabled_metadata_failure_boundary);
        run_case(
            "single provider callback contract",
            test_single_provider_callback_contract);
        run_case(
            "fetch provider traversal boundary",
            test_fetch_provider_traversal_boundary);
        run_case("Case 9 unknown dependency", test_case_9_unknown_dependency);
        run_case("Case 10 dependency chain", test_case_10_dependency_chain);
        run_case("Case 11 single overload compatibility", test_case_11_single_overload_compatibility);
        run_case("Case 12 invalid reducer state", test_case_12_invalid_reducer_state);
        run_case("supplemental multi-root contracts", test_supplemental_multi_root_contracts);
        run_case(
            "same PackageBase root coverage",
            test_same_package_base_root_coverage);
        run_case(
            "same PackageBase sibling attribution",
            test_same_package_base_sibling_attribution);
        run_case(
            "real dependency cycle is preserved",
            test_real_dependency_cycle_is_preserved);
        run_case(
            "same PackageBase real cycle is preserved",
            test_same_package_base_real_cycle_is_preserved);
        run_case(
            "same PackageBase late dependency ordering",
            test_same_package_base_late_dependency_ordering);
        run_case(
            "preflight root failure and continuation",
            test_preflight_root_failure_and_continuation);
        run_case(
            "preflight dependency and provider failures",
            test_preflight_dependency_and_provider_failures);
        run_case(
            "preflight shared failure root attribution",
            test_preflight_shared_failure_root_attribution);
        run_case(
            "preflight response error propagation",
            test_preflight_response_error_propagation);
        run_case(
            "preflight repository query boundary",
            test_preflight_repository_query_boundary);
        run_case(
            "preflight repository metadata failures",
            test_preflight_repository_metadata_failures);
        run_case(
            "legacy resolution failure boundary",
            test_legacy_resolution_failure_boundary);
        run_case(
            "planned relation observation authorities",
            test_planned_relation_observation_authorities);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "dependency plan model tests: all checks passed\n";
    return 0;
}
