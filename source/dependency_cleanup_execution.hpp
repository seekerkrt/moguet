#pragma once

#include "dependency_cleanup_interaction.hpp"
#include "package_metadata.hpp"

#include <optional>
#include <vector>

enum class DependencyCleanupCandidateRevalidationStatus {
    Ready,
    AlreadyAbsent,
    IdentityChanged,
    InstallReasonChanged,
    Protected,
    StillRequired,
    Unknown,
    Invalid,
};

enum class DependencyCleanupRevalidationState {
    NoCandidatesReady,
    Ready,
    Blocked,
};

struct DependencyCleanupCandidateRevalidation {
    // Exact approval binding, including source identity and correlations.
    DependencyCleanupCandidateSnapshot approved;
    DependencyCleanupCandidateRevalidationStatus status;
    std::optional<InstalledPackageMetadata> current;
    std::vector<std::string> runtime_consumers;
    std::optional<PackageMetadataFailure> failure;
};

// Only fresh revalidation can construct this result. Candidate order is the
// approved order; Ready is a subset, never a new projection from the DB.
class DependencyCleanupRevalidationResult final {
public:
    [[nodiscard]] DependencyCleanupRevalidationState state() const noexcept;
    [[nodiscard]] const std::vector<DependencyCleanupCandidateRevalidation>& candidates() const noexcept;
    [[nodiscard]] const std::optional<PackageMetadataFailure>& failure() const noexcept;

private:
    DependencyCleanupRevalidationResult() = default;
    DependencyCleanupRevalidationState state_ = DependencyCleanupRevalidationState::Blocked;
    std::vector<DependencyCleanupCandidateRevalidation> candidates_;
    std::optional<PackageMetadataFailure> failure_;

    friend DependencyCleanupRevalidationResult revalidate_dependency_cleanup(
        const DependencyCleanupApprovalSnapshot& approval);
};

// Opens fresh configuration/session/policy authority; accepts no cached read
// evidence or caller-selected package operands. No mutation is performed.
[[nodiscard]] DependencyCleanupRevalidationResult revalidate_dependency_cleanup(
    const DependencyCleanupApprovalSnapshot& approval);

enum class DependencyCleanupExecutionStatus {
    NoCandidatesReady,
    Removed,
    RemovalFailed,
    Blocked,
};

// Independent of build/install and interaction. Failed attempts retain the
// exact attempted set; they do not claim which packages actually disappeared.
struct DependencyCleanupExecutionResult {
    DependencyCleanupExecutionStatus status = DependencyCleanupExecutionStatus::Blocked;
    std::optional<DependencyCleanupRevalidationResult> revalidation;
    std::vector<DependencyCleanupCandidateSnapshot> attempted;
    std::optional<int> removal_exit_status;
};

// The public remote AUR owner calls this only after explicit approval.
// Every call checks Approved and obtains new read authority immediately before
// its one exact attempt. No API accepts a caller-retained Ready plan for mutation.
[[nodiscard]] DependencyCleanupExecutionResult execute_dependency_cleanup(
    const DependencyCleanupInteractionResult& interaction);

// Report cleanup independently; never describe the successful build as failed.
void report_dependency_cleanup_result(
    const DependencyCleanupInteractionResult& interaction,
    const std::optional<DependencyCleanupExecutionResult>& execution);
