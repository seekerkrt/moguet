#pragma once

#include "cross_source_version_lock.hpp"

#include <optional>
#include <string>
#include <vector>

enum class CrossSourceVersionLockObservationStatus {
    Complete,
    Partial,
    Failed,
};

enum class CrossSourceVersionLockObservationIssueKind {
    RepositoryConfigurationUnavailable,
    ForeignInventoryUnavailable,
    InstalledRelationInventoryUnavailable,
    InstalledRuntimeDependencyInventoryUnavailable,
    InstalledConsumerIdentityAmbiguous,
    InstalledRuntimeDependencyInvalid,
    DuplicateInstalledRuntimeDependency,
    RepositoryCandidateUnavailable,
    AurReplacementMetadataUnavailable,
    AurReplacementQueryFailure,
};

struct CrossSourceVersionLockObservationIssue {
    CrossSourceVersionLockObservationIssueKind kind;
    std::optional<std::string> installed_consumer_package_name;
    std::optional<std::string> dependency_package_name;
    std::string diagnostic;
};

// Complete with an empty candidate set confirms only that this candidate scan
// found no direct exact correlation. Partial/Failed must never be interpreted
// as absence. Evidence remains repository/AUR metadata correlation only: it
// proves neither installed AUR provenance nor a pacman transaction blocker.
struct CrossSourceVersionLockObservationResult {
    CrossSourceVersionLockObservationStatus status =
        CrossSourceVersionLockObservationStatus::Failed;
    std::vector<CrossSourceVersionLockCandidateEvidence> candidates;
    std::vector<CrossSourceVersionLockObservationIssue> issues;
};

// Performs read-only local/sync database and exact AUR metadata observation.
// It does not run pacman, prepare a transaction, resolve providers, or authorize
// execution. Only direct equality-qualified installed runtime dependencies are
// candidates for correlation.
[[nodiscard]] CrossSourceVersionLockObservationResult
observe_cross_source_version_lock_candidates();

enum class CrossSourceVersionLockCorrelationFailureKind {
    ResourceExhaustion,
    UnexpectedException,
    UnknownException,
};

struct CrossSourceVersionLockCorrelationFailure {
    CrossSourceVersionLockCorrelationFailureKind kind =
        CrossSourceVersionLockCorrelationFailureKind::
            UnknownException;
    std::optional<std::string> diagnostic;
};

// Secondary evidence collected only after a system-upgrade failure. The
// observation status remains the Complete/Partial/Failed authority. Indices
// identify only possible candidate correlations; they neither identify the
// pacman transaction target nor confirm the cause of its failure.
struct CrossSourceVersionLockCorrelationResult {
    std::optional<CrossSourceVersionLockObservationResult> observation;
    std::vector<CrossSourceVersionLockAssessment> assessments;
    std::vector<std::size_t> possible_blocker_assessment_indices;
    std::optional<CrossSourceVersionLockCorrelationFailure> failure;
};

// Best-effort secondary evidence after a repository transaction failure.
// Never throws or grants mutation authority; collection failure is retained.
[[nodiscard]] CrossSourceVersionLockCorrelationResult
observe_cross_source_version_lock_correlation() noexcept;
