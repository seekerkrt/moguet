#pragma once

#include "interactive_confirmation.hpp"
#include "invocation_owned_cleanup_model.hpp"

#include <iosfwd>
#include <optional>
#include <vector>

struct AppConfig;
class RemoteAurCleanupCollectionResult;

// Owned facts only: no session, metadata reader, execution object, or live
// assessment. Installed metadata retains its exact representation separately
// from source identity for the later fresh-state comparison.
class DependencyCleanupCandidateSnapshot final {
public:
    [[nodiscard]] const SourceAwarePackageIdentity& package() const noexcept;
    [[nodiscard]] const InstalledPackageMetadata& expected_installed() const noexcept;
    [[nodiscard]] const std::vector<CleanupPackageCorrelation>& correlations() const noexcept;
    [[nodiscard]] CleanupClassification classification() const noexcept;
    [[nodiscard]] CleanupSharedRequirementState shared_requirement() const noexcept;
    [[nodiscard]] CleanupPolicyProtection policy_protection() const noexcept;

    bool operator==(const DependencyCleanupCandidateSnapshot&) const = default;

private:
    // Collector calls this only after the existing classifier returned Eligible,
    // which establishes complete correlation coverage and Dependency reason.
    explicit DependencyCleanupCandidateSnapshot(const InvocationOwnedCleanupCandidate& candidate);

    SourceAwarePackageIdentity package_;
    InstalledPackageMetadata expected_installed_;
    std::vector<CleanupPackageCorrelation> correlations_;
    CleanupClassification classification_;
    CleanupSharedRequirementState shared_requirement_;
    CleanupPolicyProtection policy_protection_;

    friend class RemoteAurCleanupCandidateCollector;
};

enum class DependencyCleanupPreviewState {
    Ready,
    NoCandidates,
    Blocked,
};

// This is the collector's assessed universe, not a claim to enumerate all
// dependencies installed by makepkg. Only Complete collections can be Ready.
class DependencyCleanupPreview final {
public:
    [[nodiscard]] DependencyCleanupPreviewState state() const noexcept;
    [[nodiscard]] const std::vector<DependencyCleanupCandidateSnapshot>& eligible_candidates() const noexcept;

private:
    DependencyCleanupPreview(DependencyCleanupPreviewState state,
                             std::vector<DependencyCleanupCandidateSnapshot> candidates);
    DependencyCleanupPreviewState state_;
    std::vector<DependencyCleanupCandidateSnapshot> eligible_candidates_;

    friend DependencyCleanupPreview make_dependency_cleanup_preview(
        const RemoteAurCleanupCollectionResult& collection);
};

[[nodiscard]] DependencyCleanupPreview make_dependency_cleanup_preview(
    const RemoteAurCleanupCollectionResult& collection);
void render_dependency_cleanup_preview(const DependencyCleanupPreview& preview, std::ostream& output);

enum class DependencyCleanupInteractionStatus {
    NoCandidates,
    Blocked,
    Declined,
    Cancelled,
    InteractionUnavailable,
    Approved,
};

enum class DependencyCleanupUnavailableReason {
    NoConfirm,
    NonInteractiveInput,
    InputFailure,
    OutputFailure,
};

class DependencyCleanupInteractionResult;

// Approval records exactly what was previewed. It is NOT a removal capability:
// Slice 4 must compare these expected facts with fresh mutation-time evidence.
// No conversion to package operands or executor input is provided.
class DependencyCleanupApprovalSnapshot final {
public:
    [[nodiscard]] const DependencyCleanupPreview& preview() const noexcept;

private:
    explicit DependencyCleanupApprovalSnapshot(DependencyCleanupPreview preview);
    DependencyCleanupPreview preview_;

    friend DependencyCleanupInteractionResult interact_dependency_cleanup(
        const DependencyCleanupPreview&, const AppConfig&, bool,
        std::istream&, std::ostream&);
};

// No build/install result reference is held or mutated, and no public exit
// status is assigned here. Only Approved can carry an approval snapshot.
class DependencyCleanupInteractionResult final {
public:
    [[nodiscard]] DependencyCleanupInteractionStatus status() const noexcept;
    [[nodiscard]] const std::optional<DependencyCleanupApprovalSnapshot>& approved_snapshot() const noexcept;
    [[nodiscard]] const std::optional<ConfirmationCancellationReason>& cancellation_reason() const noexcept;
    [[nodiscard]] const std::optional<DependencyCleanupUnavailableReason>& unavailable_reason() const noexcept;

private:
    explicit DependencyCleanupInteractionResult(
        DependencyCleanupInteractionStatus status,
        std::optional<DependencyCleanupApprovalSnapshot> approved = std::nullopt,
        std::optional<ConfirmationCancellationReason> cancelled = std::nullopt,
        std::optional<DependencyCleanupUnavailableReason> unavailable = std::nullopt);
    DependencyCleanupInteractionStatus status_;
    std::optional<DependencyCleanupApprovalSnapshot> approved_snapshot_;
    std::optional<ConfirmationCancellationReason> cancellation_reason_;
    std::optional<DependencyCleanupUnavailableReason> unavailable_reason_;

    friend DependencyCleanupInteractionResult interact_dependency_cleanup(
        const DependencyCleanupPreview&, const AppConfig&, bool,
        std::istream&, std::ostream&);
};

// Remote AUR --rmdeps interaction; ordinary builds do not call it.
[[nodiscard]] DependencyCleanupInteractionResult interact_dependency_cleanup(
    const DependencyCleanupPreview& preview, const AppConfig& config);

// The injected overload uses the existing confirmation stream/TTY convention.
[[nodiscard]] DependencyCleanupInteractionResult interact_dependency_cleanup(
    const DependencyCleanupPreview& preview, const AppConfig& config,
    bool is_interactive_input, std::istream& input, std::ostream& output);
