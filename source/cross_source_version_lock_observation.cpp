#include "cross_source_version_lock_observation.hpp"

#include "aur_rpc.hpp"
#include "installed_package_relation_inventory.hpp"
#include "package_identifier.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct CandidateSeed {
    RepositoryUpgradeCandidate repository_upgrade;
    ConsumerDependencyRequirement requirement;
};

struct AurReplacementObservation {
    AurReplacementCandidateQueryResult result;
    std::optional<CrossSourceVersionLockObservationIssue> issue;
};

void add_issue(
    CrossSourceVersionLockObservationResult& result,
    CrossSourceVersionLockObservationIssueKind kind,
    std::optional<std::string> installed_consumer_package_name,
    std::optional<std::string> dependency_package_name,
    std::string diagnostic,
    CrossSourceVersionLockObservationStatus status =
        CrossSourceVersionLockObservationStatus::Partial) {
    result.issues.push_back(CrossSourceVersionLockObservationIssue{
        kind,
        std::move(installed_consumer_package_name),
        std::move(dependency_package_name),
        std::move(diagnostic)});
    if(status == CrossSourceVersionLockObservationStatus::Failed ||
       result.status == CrossSourceVersionLockObservationStatus::Complete) {
        result.status = status;
    }
}

std::string installed_relation_failure_diagnostic(
    const InstalledPackageRelationInventoryFailureReason& reason) {
    return std::visit(
        [](const auto& failure) -> std::string {
            using Failure = std::decay_t<decltype(failure)>;
            if constexpr(std::is_same_v<Failure, PackageMetadataFailure>) {
                return failure.diagnostic;
            } else {
                return "Installed package relation metadata is malformed: " +
                       failure.raw_specification;
            }
        },
        reason);
}

const PackageRelationObservedPackage* unique_installed_package(
    const std::map<
        std::string,
        std::vector<const PackageRelationObservedPackage*>>& packages,
    const std::string& package_name) noexcept {
    const auto found = packages.find(package_name);
    return found != packages.end() && found->second.size() == 1U
               ? found->second.front()
               : nullptr;
}

const InstalledPackageRuntimeDependencyMetadata* unique_dependency_metadata(
    const std::map<
        std::string,
        std::vector<
            const InstalledPackageRuntimeDependencyMetadata*>>&
        metadata,
    const std::string& package_name) noexcept {
    const auto found = metadata.find(package_name);
    return found != metadata.end() && found->second.size() == 1U
               ? found->second.front()
               : nullptr;
}

bool is_same_installed_database(
    const PackageRelationObservedPackage& package,
    const PacmanDatabasePaths& paths) noexcept {
    const auto* source =
        std::get_if<PackageRelationInstalledDatabaseIdentity>(
            &package.source);
    return source != nullptr &&
           source->root_dir == paths.root_dir.lexically_normal() &&
           source->database_path == paths.db_path.lexically_normal();
}

bool installed_consumer_matches_foreign_inventory(
    const PackageRelationObservedPackage& package,
    const InstalledPackageMetadata& foreign_package,
    const PacmanDatabasePaths& paths) noexcept {
    const std::string* installed_version = package.package_version.version();
    return package.role == PackageRelationObservationRole::Installed &&
           package.package_name == foreign_package.name &&
           installed_version != nullptr &&
           *installed_version == foreign_package.version &&
           is_same_installed_database(package, paths) &&
           !validate_package_relation_observation(package).has_value();
}

std::optional<std::vector<ConsumerDependencyRequirement>>
parse_installed_runtime_dependencies(
    const InstalledPackageRuntimeDependencyMetadata& metadata,
    CrossSourceVersionLockObservationResult& result) {
    std::vector<ConsumerDependencyRequirement> requirements;
    requirements.reserve(metadata.dependency_specifications.size());
    for(const std::string& specification :
        metadata.dependency_specifications) {
        const DependencyRequirementParseResult parsed =
            parse_dependency_requirement(specification);
        if(const auto* failure = parsed.failure(); failure != nullptr) {
            add_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    InstalledRuntimeDependencyInvalid,
                metadata.package_name,
                std::nullopt,
                "Installed runtime dependency metadata is invalid: " +
                    failure->raw_specification);
            return std::nullopt;
        }
        const DependencyRequirement* requirement = parsed.requirement();
        if(requirement == nullptr) {
            add_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    InstalledRuntimeDependencyInvalid,
                metadata.package_name,
                std::nullopt,
                "Installed runtime dependency metadata has no typed requirement.");
            return std::nullopt;
        }
        if(const auto* consumer =
               std::get_if<ConsumerDependencyRequirement>(requirement);
           consumer != nullptr) {
            requirements.push_back(*consumer);
        }
    }
    return requirements;
}

bool is_direct_exact_requirement(
    const ConsumerDependencyRequirement& requirement) noexcept {
    return requirement.constraint().has_value() &&
           requirement.constraint()->relation() ==
               DependencyVersionRelation::Equal;
}

CrossSourceTransitionInstalledSnapshot transition_installed_snapshot(
    const PacmanDatabasePaths& paths,
    const std::vector<PackageRelationObservedPackage>& packages,
    const InstalledPackageRuntimeDependencyMetadataInventory& dependencies,
    const ForeignPackageInventory& foreign_packages,
    bool inventories_complete) {
    CrossSourceTransitionInstalledSnapshot snapshot{
        {paths.root_dir.lexically_normal(), paths.db_path.lexically_normal()},
        inventories_complete ? PackageRelationObservationCompleteness::Complete
                             : PackageRelationObservationCompleteness::Partial,
        packages,
        {},
        foreign_packages};
    std::map<std::string, const PackageRelationObservedPackage*> identities;
    for(const auto& package : packages) {
        if(!identities.emplace(package.package_name, &package).second ||
           package.role != PackageRelationObservationRole::Installed ||
           !is_same_installed_database(package, paths) ||
           validate_package_relation_observation(package).has_value() ||
           package.package_version.version() == nullptr) {
            snapshot.completeness = PackageRelationObservationCompleteness::Invalid;
        }
    }
    std::set<std::string> dependency_names;
    for(const auto& metadata : dependencies) {
        const auto identity = identities.find(metadata.package_name);
        if(!dependency_names.insert(metadata.package_name).second ||
           identity == identities.end() || !metadata.installed_version.has_value() ||
           identity->second->package_version.version() == nullptr ||
           *identity->second->package_version.version() != *metadata.installed_version) {
            snapshot.completeness = PackageRelationObservationCompleteness::Invalid;
        }
        CrossSourceInstalledRuntimeRequirements runtime{metadata.package_name, {}};
        for(const auto& specification : metadata.dependency_specifications) {
            const auto parsed = parse_dependency_requirement(specification);
            if(parsed.failure() != nullptr || parsed.requirement() == nullptr) {
                snapshot.completeness = PackageRelationObservationCompleteness::Invalid;
            } else {
                runtime.requirements.push_back(*parsed.requirement());
            }
        }
        snapshot.runtime_requirements.push_back(std::move(runtime));
    }
    // Coverage gaps must not erase already observed invalidity.
    if(snapshot.completeness != PackageRelationObservationCompleteness::Invalid &&
       identities.size() != dependency_names.size()) {
        snapshot.completeness = PackageRelationObservationCompleteness::Partial;
    }
    return snapshot;
}

AurReplacementObservation observe_aur_replacement(
    const std::string& package_name) {
    std::optional<AurPackageInfo> package;
    try {
        package = AurClient::info_strict(package_name);
    } catch(const AurRpcResponseError& error) {
        // Schema/semantic failure means the exact response was obtained but its
        // metadata cannot be trusted. It is not confirmed package absence and
        // remains distinct from transport/query failure.
        return AurReplacementObservation{
            AurReplacementCandidateMetadataUnavailable{
                package_name,
                std::nullopt,
                ObservedVersionUnknownReason::PartialSourceFailure},
            CrossSourceVersionLockObservationIssue{
                CrossSourceVersionLockObservationIssueKind::
                    AurReplacementMetadataUnavailable,
                package_name,
                std::nullopt,
                error.what()}};
    } catch(const std::runtime_error& error) {
        // The strict AUR API currently uses std::runtime_error for request
        // setup and transport failures. Keep this catch at the query boundary
        // so unrelated local construction failures are not reclassified.
        return AurReplacementObservation{
            AurReplacementCandidateQueryFailure{
                {package_name}, error.what()},
            CrossSourceVersionLockObservationIssue{
                CrossSourceVersionLockObservationIssueKind::
                    AurReplacementQueryFailure,
                package_name,
                std::nullopt,
                error.what()}};
    }

    if(!package.has_value()) {
        return AurReplacementObservation{
            AurReplacementCandidateNotFound{package_name},
            std::nullopt};
    }
    if(!package->constraint_metadata.has_value()) {
        return AurReplacementObservation{
            AurReplacementCandidateMetadataUnavailable{
                package_name,
                package->PackageBase.empty()
                    ? std::nullopt
                    : std::optional<std::string>(
                          package->PackageBase),
                ObservedVersionUnknownReason::PartialSourceFailure},
            CrossSourceVersionLockObservationIssue{
                CrossSourceVersionLockObservationIssueKind::
                    AurReplacementMetadataUnavailable,
                package_name,
                std::nullopt,
                "Exact AUR replacement constraint metadata is unavailable."}};
    }
    return AurReplacementObservation{
        AurReplacementCandidateQuerySuccess{
            {package->constraint_metadata.value()}},
        std::nullopt};
}

} // namespace

CrossSourceVersionLockObservationResult
observe_cross_source_version_lock_candidates() {
    CrossSourceVersionLockObservationResult result;
    result.status = CrossSourceVersionLockObservationStatus::Complete;

    PacmanRepositoryConfiguration configuration;
    try {
        configuration = resolve_pacman_repository_configuration();
    } catch(const std::exception& error) {
        add_issue(
            result,
            CrossSourceVersionLockObservationIssueKind::
                RepositoryConfigurationUnavailable,
            std::nullopt,
            std::nullopt,
            error.what(),
            CrossSourceVersionLockObservationStatus::Failed);
        return result;
    } catch(...) {
        add_issue(
            result,
            CrossSourceVersionLockObservationIssueKind::
                RepositoryConfigurationUnavailable,
            std::nullopt,
            std::nullopt,
            "Repository configuration observation failed with an unknown exception.",
            CrossSourceVersionLockObservationStatus::Failed);
        return result;
    }

    ForeignPackageInventoryResult foreign_result =
        query_foreign_package_inventory(configuration);
    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&foreign_result);
       failure != nullptr) {
        add_issue(
            result,
            CrossSourceVersionLockObservationIssueKind::
                ForeignInventoryUnavailable,
            std::nullopt,
            std::nullopt,
            failure->diagnostic,
            CrossSourceVersionLockObservationStatus::Failed);
        return result;
    }
    ForeignPackageInventory foreign_packages =
        std::get<ForeignPackageInventory>(std::move(foreign_result));
    if(foreign_packages.empty()) return result;

    InstalledPackageRelationInventoryResult relation_result =
        query_installed_package_relations(
            configuration.database_paths);
    std::vector<PackageRelationObservedPackage> installed_packages;
    if(auto* failure =
           std::get_if<InstalledPackageRelationInventoryFailure>(
               &relation_result);
       failure != nullptr) {
        installed_packages = std::move(failure->observed_packages);
        add_issue(
            result,
            CrossSourceVersionLockObservationIssueKind::
                InstalledRelationInventoryUnavailable,
            failure->package_name,
            std::nullopt,
            installed_relation_failure_diagnostic(failure->reason),
            installed_packages.empty()
                ? CrossSourceVersionLockObservationStatus::Failed
                : CrossSourceVersionLockObservationStatus::Partial);
        if(installed_packages.empty()) return result;
    } else {
        installed_packages =
            std::get<InstalledPackageRelationInventory>(
                std::move(relation_result))
                .packages;
    }

    InstalledPackageRuntimeDependencyMetadataInventoryResult
        dependency_result =
            query_installed_package_runtime_dependency_metadata(
                configuration.database_paths);
    InstalledPackageRuntimeDependencyMetadataInventory dependency_metadata;
    if(auto* failure = std::get_if<
           InstalledPackageRuntimeDependencyMetadataInventoryFailure>(
           &dependency_result);
       failure != nullptr) {
        dependency_metadata = std::move(failure->observed_packages);
        add_issue(
            result,
            CrossSourceVersionLockObservationIssueKind::
                InstalledRuntimeDependencyInventoryUnavailable,
            std::nullopt,
            std::nullopt,
            failure->failure.diagnostic,
            dependency_metadata.empty()
                ? CrossSourceVersionLockObservationStatus::Failed
                : CrossSourceVersionLockObservationStatus::Partial);
        if(dependency_metadata.empty()) return result;
    } else {
        dependency_metadata =
            std::get<
                InstalledPackageRuntimeDependencyMetadataInventory>(
                std::move(dependency_result));
    }

    result.transition_installed_snapshot = transition_installed_snapshot(
        configuration.database_paths, installed_packages, dependency_metadata,
        foreign_packages, result.status == CrossSourceVersionLockObservationStatus::Complete);

    std::map<
        std::string,
        std::vector<const PackageRelationObservedPackage*>>
        installed_by_name;
    for(const PackageRelationObservedPackage& package : installed_packages) {
        installed_by_name[package.package_name].push_back(&package);
    }
    std::map<
        std::string,
        std::vector<const InstalledPackageRuntimeDependencyMetadata*>>
        dependencies_by_name;
    for(const InstalledPackageRuntimeDependencyMetadata& package :
        dependency_metadata) {
        dependencies_by_name[package.package_name].push_back(&package);
    }

    std::set<std::string> observed_foreign_names;
    for(const InstalledPackageMetadata& foreign_package : foreign_packages) {
        if(!is_valid_package_name(foreign_package.name) ||
           foreign_package.version.empty() ||
           !observed_foreign_names.insert(foreign_package.name).second) {
            add_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    InstalledConsumerIdentityAmbiguous,
                foreign_package.name,
                std::nullopt,
                "Foreign package identity is invalid or ambiguous.");
            continue;
        }

        const PackageRelationObservedPackage* installed_consumer =
            unique_installed_package(
                installed_by_name, foreign_package.name);
        if(installed_consumer == nullptr ||
           !installed_consumer_matches_foreign_inventory(
               *installed_consumer,
               foreign_package,
               configuration.database_paths)) {
            add_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    InstalledConsumerIdentityAmbiguous,
                foreign_package.name,
                std::nullopt,
                "Foreign inventory and installed package observation cannot be correlated uniquely.");
            continue;
        }

        const InstalledPackageRuntimeDependencyMetadata*
            installed_dependencies = unique_dependency_metadata(
                dependencies_by_name, foreign_package.name);
        if(installed_dependencies == nullptr) {
            add_issue(
                result,
                CrossSourceVersionLockObservationIssueKind::
                    InstalledRuntimeDependencyInventoryUnavailable,
                foreign_package.name,
                std::nullopt,
                "Installed runtime dependency observation is missing or ambiguous.");
            continue;
        }

        std::optional<std::vector<ConsumerDependencyRequirement>>
            parsed_requirements = parse_installed_runtime_dependencies(
                *installed_dependencies, result);
        if(!parsed_requirements.has_value()) continue;

        std::map<std::string, std::size_t> exact_requirement_counts;
        for(const ConsumerDependencyRequirement& requirement :
            parsed_requirements.value()) {
            if(is_direct_exact_requirement(requirement)) {
                ++exact_requirement_counts[requirement.package_name()];
            }
        }

        std::vector<CandidateSeed> seeds;
        std::set<std::string> processed_dependencies;
        for(const ConsumerDependencyRequirement& requirement :
            parsed_requirements.value()) {
            if(!is_direct_exact_requirement(requirement) ||
               !processed_dependencies.insert(requirement.package_name())
                    .second) {
                continue;
            }
            if(exact_requirement_counts[requirement.package_name()] != 1U) {
                add_issue(
                    result,
                    CrossSourceVersionLockObservationIssueKind::
                        DuplicateInstalledRuntimeDependency,
                    foreign_package.name,
                    requirement.package_name(),
                    "Installed package has multiple matching direct exact runtime dependencies.");
                continue;
            }

            const PackageRelationObservedPackage* installed_dependency =
                unique_installed_package(
                    installed_by_name, requirement.package_name());
            if(installed_dependency == nullptr ||
               installed_dependency->role !=
                   PackageRelationObservationRole::Installed ||
               !is_same_installed_database(
                   *installed_dependency,
                   configuration.database_paths)) {
                // Provider/indirect satisfaction is deliberately outside this
                // direct exact candidate adapter.
                continue;
            }

            StrictRepositoryPackageQueryResult repository_result;
            try {
                repository_result = query_repository_package_strict(
                    configuration, requirement.package_name());
            } catch(const std::exception& error) {
                add_issue(
                    result,
                    CrossSourceVersionLockObservationIssueKind::
                        RepositoryCandidateUnavailable,
                    foreign_package.name,
                    requirement.package_name(),
                    error.what());
                continue;
            }
            if(std::holds_alternative<RepositoryPackageNotFound>(
                   repository_result)) {
                continue;
            }
            if(const auto* failure =
                   std::get_if<RepositoryMetadataFailure>(
                       &repository_result);
               failure != nullptr) {
                add_issue(
                    result,
                    CrossSourceVersionLockObservationIssueKind::
                        RepositoryCandidateUnavailable,
                    foreign_package.name,
                    requirement.package_name(),
                    failure->diagnostic);
                continue;
            }

            seeds.push_back(CandidateSeed{
                RepositoryUpgradeCandidate{
                    InstalledExactPackage{
                        installed_dependency->package_name,
                        installed_dependency->package_version},
                    std::get<RepositoryPackagePresent>(
                        std::move(repository_result))},
                requirement});
        }
        if(seeds.empty()) continue;

        AurReplacementObservation aur_replacement =
            observe_aur_replacement(foreign_package.name);
        if(aur_replacement.issue.has_value()) {
            result.issues.push_back(
                std::move(aur_replacement.issue.value()));
            if(result.status ==
               CrossSourceVersionLockObservationStatus::Complete) {
                result.status = CrossSourceVersionLockObservationStatus::Partial;
            }
        }
        for(CandidateSeed& seed : seeds) {
            result.candidates.push_back(
                CrossSourceVersionLockCandidateEvidence{
                    std::move(seed.repository_upgrade),
                    InstalledCrossSourceVersionLockConsumer{
                        *installed_consumer,
                        std::move(seed.requirement)},
                    aur_replacement.result});
        }
    }

    return result;
}

namespace {

bool is_possible_cross_source_version_lock_blocker_candidate(
    const CrossSourceVersionLockAssessment& assessment) noexcept {
    return assessment.installed_requirement_against_installed_version
               .has_value() &&
           assessment.installed_requirement_against_repository_candidate
               .has_value() &&
           assessment.installed_requirement_against_installed_version
                   ->satisfaction() ==
               ConstraintSatisfaction::Satisfied &&
           assessment.installed_requirement_against_repository_candidate
                   ->satisfaction() ==
               ConstraintSatisfaction::Unsatisfied;
}

void record_cross_source_version_lock_correlation_failure(
    CrossSourceVersionLockCorrelationResult& correlation,
    CrossSourceVersionLockCorrelationFailureKind kind,
    const char* diagnostic = nullptr) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<
                  CrossSourceVersionLockCorrelationFailure>);
    correlation.failure.emplace();
    correlation.failure->kind = kind;
    if(diagnostic == nullptr) return;

    try {
        correlation.failure->diagnostic.emplace(diagnostic);
    } catch(...) {
        // The typed secondary failure remains available even when retaining
        // its optional diagnostic would require unavailable memory.
        correlation.failure->diagnostic.reset();
    }
}

} // namespace

CrossSourceVersionLockCorrelationResult
observe_cross_source_version_lock_correlation(
    CrossSourceVersionLockObservationBasis basis) noexcept {
    static_assert(std::is_nothrow_default_constructible_v<
                  CrossSourceVersionLockCorrelationResult>);
    CrossSourceVersionLockCorrelationResult correlation;
    correlation.basis = basis;

    try {
        correlation.observation.emplace(
            observe_cross_source_version_lock_candidates());

        std::vector<CrossSourceVersionLockAssessment> assessments;
        std::vector<std::size_t> possible_blocker_assessment_indices;
        const auto& candidates = correlation.observation->candidates;
        assessments.reserve(candidates.size());
        possible_blocker_assessment_indices.reserve(candidates.size());
        for(const CrossSourceVersionLockCandidateEvidence& candidate :
            candidates) {
            CrossSourceVersionLockAssessment assessment =
                assess_cross_source_version_lock_candidate(candidate);
            const bool is_possible_blocker_candidate =
                is_possible_cross_source_version_lock_blocker_candidate(
                    assessment);
            const std::size_t assessment_index = assessments.size();
            assessments.push_back(std::move(assessment));
            if(is_possible_blocker_candidate) {
                possible_blocker_assessment_indices.push_back(
                    assessment_index);
            }
        }

        // Publish assessment/index vectors together. An exception before this
        // point retains the observation plus a typed secondary failure, not a
        // misleading partially indexed assessment set.
        correlation.assessments = std::move(assessments);
        correlation.possible_blocker_assessment_indices =
            std::move(possible_blocker_assessment_indices);
        if(basis == CrossSourceVersionLockObservationBasis::BeforeRepositoryMutation) {
            correlation.transition_plans = plan_cross_source_coordinated_transitions(correlation);
        }
    } catch(const std::bad_alloc&) {
        record_cross_source_version_lock_correlation_failure(
            correlation,
            CrossSourceVersionLockCorrelationFailureKind::
                ResourceExhaustion);
    } catch(const std::exception& error) {
        record_cross_source_version_lock_correlation_failure(
            correlation,
            CrossSourceVersionLockCorrelationFailureKind::
                UnexpectedException,
            error.what());
    } catch(...) {
        record_cross_source_version_lock_correlation_failure(
            correlation,
            CrossSourceVersionLockCorrelationFailureKind::
                UnknownException);
    }
    return correlation;
}
