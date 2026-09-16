#pragma once

#include "aur_constraint_metadata.hpp"
#include "package_relation_observation.hpp"
#include "repository_query.hpp"

#include <optional>
#include <string>
#include <variant>
#include <vector>

// This is repository-metadata evidence only. It does not represent the target
// selected by pacman/libalpm for a system-upgrade transaction.
struct RepositoryUpgradeCandidate {
    InstalledExactPackage installed_package;
    RepositoryPackagePresent repository_candidate;

    bool operator==(const RepositoryUpgradeCandidate&) const = default;
};

// The installed database proves the current package identity and version, not
// AUR provenance. Cross-source correlation comes from the separately retained
// AUR replacement query evidence below.
struct InstalledCrossSourceVersionLockConsumer {
    PackageRelationObservedPackage package;
    ConsumerDependencyRequirement requirement;

    bool operator==(const InstalledCrossSourceVersionLockConsumer&) const = default;
};

struct AurReplacementCandidateQuerySuccess {
    std::vector<AurPackageConstraintMetadata> candidates;

    bool operator==(const AurReplacementCandidateQuerySuccess&) const = default;
};

struct AurReplacementCandidateNotFound {
    std::string package_name;

    bool operator==(const AurReplacementCandidateNotFound&) const = default;
};

struct AurReplacementCandidateMetadataUnavailable {
    std::string package_name;
    std::optional<std::string> package_base;
    ObservedVersionUnknownReason reason;

    bool operator==(const AurReplacementCandidateMetadataUnavailable&) const = default;
};

struct AurReplacementCandidateQueryFailure {
    std::vector<std::string> package_names;
    std::string diagnostic;

    bool operator==(const AurReplacementCandidateQueryFailure&) const = default;
};

using AurReplacementCandidateQueryResult = std::variant<
    AurReplacementCandidateQuerySuccess,
    AurReplacementCandidateNotFound,
    AurReplacementCandidateMetadataUnavailable,
    AurReplacementCandidateQueryFailure>;

struct CrossSourceVersionLockCandidateEvidence {
    RepositoryUpgradeCandidate repository_upgrade;
    InstalledCrossSourceVersionLockConsumer installed_consumer;
    AurReplacementCandidateQueryResult aur_replacement;

    bool operator==(const CrossSourceVersionLockCandidateEvidence&) const = default;
};

// Owned read-only inputs for the bounded removal check. The inventories must
// cover the same installed identities; this is not an atomic database snapshot.
struct CrossSourceInstalledRuntimeRequirements {
    std::string package_name;
    std::vector<DependencyRequirement> requirements;

    bool operator==(const CrossSourceInstalledRuntimeRequirements&) const = default;
};

struct CrossSourceTransitionInstalledSnapshot {
    PackageRelationInstalledDatabaseIdentity source;
    PackageRelationObservationCompleteness completeness =
        PackageRelationObservationCompleteness::Unavailable;
    std::vector<PackageRelationObservedPackage> packages;
    std::vector<CrossSourceInstalledRuntimeRequirements> runtime_requirements;
    ForeignPackageInventory foreign_packages;

    bool operator==(const CrossSourceTransitionInstalledSnapshot&) const = default;
};

enum class CrossSourceVersionLockStatus {
    CompatibleReplacement,
    IncompatibleReplacement,
    MissingReplacement,
    Unknown,
    QueryFailure,
    Ambiguous,
};

// CompatibleReplacement means only that one directly matching runtime
// dependency of one AUR candidate is satisfied by the repository candidate.
// It neither confirms a transaction blocker nor authorizes execution.
struct CrossSourceVersionLockAssessment {
    CrossSourceVersionLockStatus status;
    CrossSourceVersionLockCandidateEvidence evidence;
    std::optional<ConstraintEvaluation>
        installed_requirement_against_installed_version;
    std::optional<ConstraintEvaluation>
        installed_requirement_against_repository_candidate;
    std::optional<ConsumerDependencyRequirement> replacement_requirement;
    std::optional<ConstraintEvaluation>
        replacement_requirement_against_repository_candidate;

    bool operator==(const CrossSourceVersionLockAssessment&) const = default;
};

// Performs no filesystem, network, process, package-database, or transaction
// work. Provider resolution is deliberately outside this candidate-only
// assessment; evidence that cannot be correlated directly fails closed.
[[nodiscard]] CrossSourceVersionLockAssessment
assess_cross_source_version_lock_candidate(
    const CrossSourceVersionLockCandidateEvidence& evidence);
