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

    bool operator==(const CrossSourceVersionLockObservationIssue&) const = default;
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
    std::optional<CrossSourceTransitionInstalledSnapshot> transition_installed_snapshot = std::nullopt;
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

enum class CrossSourceVersionLockObservationBasis {
    AfterRepositoryFailure,
    BeforeRepositoryMutation,
};

enum class CrossSourceTransitionPlanStatus {
    ReadOnlyReady,
    Blocked,
    Incomplete,
    Ambiguous,
    Unsupported,
};

enum class CrossSourceTransitionPlanReason {
    None,
    IncompleteObservation,
    ReplacementMissing,
    ReplacementQueryFailure,
    IncompatibleReplacement,
    AmbiguousIdentity,
    ExactRequirementUnavailable,
    IncompleteInstalledInventory,
    InstalledReasonUnknown,
    ReverseDependency,
    UnsupportedRuntimeDependency,
    ReplacementRelationsRequireAssessment,
    OverlappingCandidates,
    MultipleCandidatesRequireCoordination,
    UnsupportedObservationBasis,
};

enum class CrossSourceRemovalSafetyStatus {
    NotAssessed,
    PreservesRuntimeDependencies,
    Blocked,
    Incomplete,
    Unsupported,
};

struct CrossSourceRemovalDependencyEvidence {
    std::string dependent_package_name;
    ConsumerDependencyRequirement requirement;
    // Indices into the owned installed snapshot, never selected providers.
    std::vector<std::size_t> remaining_satisfier_indices;

    bool operator==(const CrossSourceRemovalDependencyEvidence&) const = default;
};

struct CrossSourceRemovalSafetyEvidence {
    CrossSourceRemovalSafetyStatus status = CrossSourceRemovalSafetyStatus::NotAssessed;
    std::vector<CrossSourceRemovalDependencyEvidence> affected_requirements;

    bool operator==(const CrossSourceRemovalSafetyEvidence&) const = default;
};

enum class CrossSourceTransitionPhase {
    RemoveInstalledForeign,
    RepositorySystemUpgrade,
    InstallAurReplacement,
    VerifyPostState,
};

// A read-only expected-state snapshot, NOT a capability or prepared operation.
// ReadOnlyReady proves only the direct exact-lock structure and bounded removal
// against complete observed installed metadata. The repository phase remains a
// system upgrade: no selected transaction or full future installed state is
// available here. Build/install feasibility is still execution-time authority.
struct CrossSourceCoordinatedTransitionPlan {
    CrossSourceTransitionPlanStatus status = CrossSourceTransitionPlanStatus::Incomplete;
    CrossSourceTransitionPlanReason reason = CrossSourceTransitionPlanReason::IncompleteObservation;
    CrossSourceVersionLockObservationBasis basis;
    CrossSourceVersionLockObservationStatus observation_completeness;
    CrossSourceVersionLockAssessment correlation;
    std::vector<CrossSourceVersionLockObservationIssue> observation_issues;
    std::optional<CrossSourceTransitionInstalledSnapshot> installed_snapshot;
    std::optional<InstalledPackageMetadata> installed_foreign;
    InstalledPackageReason expected_install_reason = InstalledPackageReason::Unknown;
    CrossSourceRemovalSafetyEvidence removal_safety;
    // Only structurally ready candidates carry phase intents. No commands.
    std::vector<CrossSourceTransitionPhase> phases;
    static constexpr bool requires_explicit_confirmation = true;
    static constexpr bool requires_mutation_time_revalidation = true;
    static constexpr bool is_atomic = false;
    static constexpr bool automatic_rollback = false;

    bool operator==(const CrossSourceCoordinatedTransitionPlan&) const = default;
};

// Read-only correlation at the stated observation point. Neither basis
// guarantees fresh sync databases or the targets selected by pacman. The
// observation status remains the Complete/Partial/Failed authority. Indices
// identify only possible candidate correlations; they neither identify the
// pacman transaction target nor confirm the cause of its failure.
struct CrossSourceVersionLockCorrelationResult {
    CrossSourceVersionLockObservationBasis basis =
        CrossSourceVersionLockObservationBasis::AfterRepositoryFailure;
    std::optional<CrossSourceVersionLockObservationResult> observation;
    std::vector<CrossSourceVersionLockAssessment> assessments;
    std::vector<std::size_t> possible_blocker_assessment_indices;
    std::optional<CrossSourceVersionLockCorrelationFailure> failure;
    std::vector<CrossSourceCoordinatedTransitionPlan> transition_plans;
};

// Pure bounded planning. No provider selection, transaction, prompt, or I/O.
// Multiple correlations require graph coordination and remain Unsupported.
[[nodiscard]] std::vector<CrossSourceCoordinatedTransitionPlan>
plan_cross_source_coordinated_transitions(
    const CrossSourceVersionLockCorrelationResult& correlation);

// Best-effort diagnostic evidence; preflight uses existing local/sync DBs
// without refreshing them. Actual execution must obtain its own authority.
// Never throws or grants mutation authority; collection failure is retained.
[[nodiscard]] CrossSourceVersionLockCorrelationResult
observe_cross_source_version_lock_correlation(
    CrossSourceVersionLockObservationBasis basis =
        CrossSourceVersionLockObservationBasis::AfterRepositoryFailure) noexcept;
