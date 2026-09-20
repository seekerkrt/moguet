#pragma once

#include "dependency_plan.hpp"
#include "installed_package.hpp"
#include "source_package_identity.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct PreparedRemoteSourceBuild;
class CleanupInvocationSession;
class CleanupInvocationSessionInspector;
class CleanupPhaseObservationProducer;
class RemoteAurCleanupCandidateCollector;
class SelectedRepositoryProviderTrustedReceiptExecutor;
class SourceArtifactInstallTrustedTransport;
struct CleanupInvocationSessionState;

// The owner identifies the mutation path, not the package selected by its
// solver. A complete receipt may therefore contain packages that were not
// requested targets.
enum class InvocationDependencyTransactionOwner {
    SelectedRepositoryProvider,
    SourceArtifactInstall,
    MakepkgSyncDependencies,
    Unknown,
};

struct CleanupTrustedTransactionTokenInventoryEntry {
    InvocationDependencyTransactionOwner owner;
    std::string transaction_token;
    std::vector<std::size_t> work_item_indices;
    bool completed_successfully = false;

    bool operator==(
        const CleanupTrustedTransactionTokenInventoryEntry&) const =
        default;
};

// Copyable evidence receives only this opaque authority. Equality is the
// identity of one live invocation-session state, never a caller-provided
// string, PID, timestamp, package name, or serialized value.
class CleanupInvocationAuthority final {
public:
    CleanupInvocationAuthority() = delete;
    CleanupInvocationAuthority(const CleanupInvocationAuthority&) = default;
    CleanupInvocationAuthority(CleanupInvocationAuthority&&) noexcept =
        default;
    CleanupInvocationAuthority& operator=(
        const CleanupInvocationAuthority&) = default;
    CleanupInvocationAuthority& operator=(
        CleanupInvocationAuthority&&) noexcept = default;
    ~CleanupInvocationAuthority() = default;

    bool operator==(const CleanupInvocationAuthority&) const noexcept =
        default;

private:
    explicit CleanupInvocationAuthority(
        std::shared_ptr<CleanupInvocationSessionState> state) noexcept;

    [[nodiscard]] bool register_trusted_transaction_token(
        InvocationDependencyTransactionOwner owner,
        const std::string& transaction_token,
        std::vector<std::size_t> work_item_indices) const;
    [[nodiscard]] bool mark_trusted_transaction_completed(
        InvocationDependencyTransactionOwner owner,
        const std::string& transaction_token) const;
    [[nodiscard]] const PreparedRemoteSourceBuild& prepared() const noexcept;
    [[nodiscard]] bool is_active() const noexcept;
    [[nodiscard]] bool baseline_was_observed() const noexcept;

    std::shared_ptr<CleanupInvocationSessionState> state_;

    friend class CleanupInvocationSession;
    friend class SelectedRepositoryProviderTrustedReceiptExecutor;
    friend class SourceArtifactInstallTrustedTransport;
#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
    friend bool register_cleanup_invocation_transaction_token_for_test(
        CleanupInvocationSession& session,
        InvocationDependencyTransactionOwner owner,
        const std::string& transaction_token,
        std::vector<std::size_t> work_item_indices,
        bool completed_successfully);
#endif
};

// One move-only owner binds the immutable remote-AUR preparation, phase
// observations, trusted transaction inventory, correlations, and aggregate.
// The collector-only production factory consumes the preparation so another
// session cannot be reconstructed from the same caller-selected identity.
class CleanupInvocationSession final {
public:
    CleanupInvocationSession() = delete;
    CleanupInvocationSession(const CleanupInvocationSession&) = delete;
    CleanupInvocationSession& operator=(const CleanupInvocationSession&) =
        delete;
    CleanupInvocationSession(CleanupInvocationSession&&) noexcept = default;
    CleanupInvocationSession& operator=(CleanupInvocationSession&&) noexcept =
        default;
    ~CleanupInvocationSession() = default;

#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
    [[nodiscard]] static CleanupInvocationSession begin(
        PreparedRemoteSourceBuild prepared);
#endif

    [[nodiscard]] const CleanupInvocationAuthority& authority()
        const noexcept;

private:
    [[nodiscard]] static CleanupInvocationSession begin_for_collector(
        PreparedRemoteSourceBuild prepared);
    explicit CleanupInvocationSession(
        std::shared_ptr<CleanupInvocationSessionState> state) noexcept;

    [[nodiscard]] const PreparedRemoteSourceBuild& prepared() const noexcept;
    [[nodiscard]] PreparedRemoteSourceBuild& prepared_for_execution() noexcept;
    [[nodiscard]] bool is_active() const noexcept;
    [[nodiscard]] bool record_baseline_observation() const;
    [[nodiscard]] std::optional<std::size_t>
    record_post_success_observation() const;
    [[nodiscard]] bool contains_trusted_transaction_token(
        InvocationDependencyTransactionOwner owner,
        const std::string& transaction_token,
        std::size_t work_item_index) const noexcept;
    [[nodiscard]] std::vector<CleanupTrustedTransactionTokenInventoryEntry>
    transaction_token_inventory() const;

    std::shared_ptr<CleanupInvocationSessionState> state_;
    CleanupInvocationAuthority authority_;

    friend class CleanupInvocationSessionInspector;
    friend class CleanupPhaseObservationProducer;
    friend class RemoteAurCleanupCandidateCollector;
#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
    friend void mark_cleanup_invocation_baseline_observed_for_test(
        CleanupInvocationSession& session);
    friend bool register_cleanup_invocation_transaction_token_for_test(
        CleanupInvocationSession& session,
        InvocationDependencyTransactionOwner owner,
        const std::string& transaction_token,
        std::vector<std::size_t> work_item_indices,
        bool completed_successfully);
#endif
};

#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
void mark_cleanup_invocation_baseline_observed_for_test(
    CleanupInvocationSession& session);
[[nodiscard]] bool register_cleanup_invocation_transaction_token_for_test(
    CleanupInvocationSession& session,
    InvocationDependencyTransactionOwner owner,
    const std::string& transaction_token,
    std::vector<std::size_t> work_item_indices,
    bool completed_successfully = true);
#endif

enum class InvocationDependencyTransactionCommandOutcome {
    NotAttempted,
    Succeeded,
    Failed,
    Unknown,
};

enum class PacmanTransactionPackageOperation {
    Install,
    Upgrade,
    Remove,
    Unknown,
};

enum class PacmanTransactionReceiptObservationState {
    Missing,
    Incomplete,
    Complete,
};

struct PacmanTransactionPackageObservation {
    PacmanTransactionPackageOperation operation;
    std::string package_name;
};

// This is machine protocol input, not pacman stdout/stderr or log text.
// A transport must set Complete only after its transaction-local protocol has
// reached an explicit final record.
struct PacmanTransactionReceiptObservation {
    PacmanTransactionReceiptObservationState state;
    std::optional<std::string> transaction_token;
    std::optional<InvocationDependencyTransactionOwner> owner;
    std::vector<PacmanTransactionPackageObservation> package_operations;
};

enum class PacmanTransactionReceiptState {
    Unavailable,
    Incomplete,
    Complete,
    Invalid,
};

// Declaration order is the canonical issue order.
enum class PacmanTransactionReceiptIssueKind {
    InvalidObservationState,
    InvalidExpectedTransactionToken,
    InvalidExpectedTransactionOwner,
    ReceiptMissing,
    ReceiptIncomplete,
    UnexpectedReceiptData,
    ObservedTransactionTokenMissing,
    InvalidObservedTransactionToken,
    TransactionTokenMismatch,
    ObservedTransactionOwnerMissing,
    InvalidObservedTransactionOwner,
    TransactionOwnerMismatch,
    InvalidPackageOperation,
    InvalidPackageName,
    DuplicatePackageName,
};

struct PacmanInstalledPackageReceipt {
    std::string package_name;

    bool operator==(const PacmanInstalledPackageReceipt&) const = default;
};

// The class is created only through the validator below. Complete means the
// machine receipt itself is structurally complete; command success remains a
// separate transaction dimension.
class PacmanTransactionReceipt final {
public:
    PacmanTransactionReceipt() = delete;
    PacmanTransactionReceipt(const PacmanTransactionReceipt&) = default;
    PacmanTransactionReceipt(PacmanTransactionReceipt&&) noexcept = default;
    PacmanTransactionReceipt& operator=(
        const PacmanTransactionReceipt&) = default;
    PacmanTransactionReceipt& operator=(
        PacmanTransactionReceipt&&) noexcept = default;
    ~PacmanTransactionReceipt() = default;

    [[nodiscard]] PacmanTransactionReceiptState state() const noexcept;
    [[nodiscard]] const std::optional<std::string>& transaction_token()
        const noexcept;
    [[nodiscard]] const std::optional<InvocationDependencyTransactionOwner>&
    owner() const noexcept;
    [[nodiscard]] const std::vector<PacmanTransactionPackageObservation>&
    package_operations() const noexcept;
    [[nodiscard]] const std::vector<PacmanInstalledPackageReceipt>&
    newly_installed_packages() const noexcept;
    [[nodiscard]] const std::vector<PacmanTransactionReceiptIssueKind>&
    issues() const noexcept;

    [[nodiscard]] bool is_complete_for(
        const std::string& expected_transaction_token,
        InvocationDependencyTransactionOwner expected_owner)
        const noexcept;
    [[nodiscard]] bool contains_newly_installed_package(
        const std::string& package_name) const noexcept;

private:
    PacmanTransactionReceipt(
        PacmanTransactionReceiptState state,
        std::optional<std::string> transaction_token,
        std::optional<InvocationDependencyTransactionOwner> owner,
        std::vector<PacmanTransactionPackageObservation>
            package_operations,
        std::vector<PacmanInstalledPackageReceipt>
            newly_installed_packages,
        std::vector<PacmanTransactionReceiptIssueKind> issues) noexcept;

    PacmanTransactionReceiptState state_;
    std::optional<std::string> transaction_token_;
    std::optional<InvocationDependencyTransactionOwner> owner_;
    std::vector<PacmanTransactionPackageObservation> package_operations_;
    std::vector<PacmanInstalledPackageReceipt> newly_installed_packages_;
    std::vector<PacmanTransactionReceiptIssueKind> issues_;

    friend PacmanTransactionReceipt validate_pacman_transaction_receipt(
        const std::string& expected_transaction_token,
        InvocationDependencyTransactionOwner expected_owner,
        const PacmanTransactionReceiptObservation& observation);
};

// The canonical token is a lowercase 256-bit nonce encoded as 64 hex digits.
// This validates protocol identity only; it does not generate a nonce.
[[nodiscard]] bool is_valid_pacman_transaction_token(
    const std::string& transaction_token) noexcept;

[[nodiscard]] PacmanTransactionReceipt validate_pacman_transaction_receipt(
    const std::string& expected_transaction_token,
    InvocationDependencyTransactionOwner expected_owner,
    const PacmanTransactionReceiptObservation& observation);

struct InvocationDependencyTransaction {
    std::string transaction_token;
    InvocationDependencyTransactionOwner owner;
    // These are exact planned package identities, not proof that the package
    // manager changed them. Solver-introduced Install records may be absent
    // from this vector and remain valid causal evidence.
    std::vector<std::string> requested_package_names;
    InvocationDependencyTransactionCommandOutcome command_outcome;
    PacmanTransactionReceipt receipt;
};

struct InvocationDependencyTransactionLedger {
    // Transaction order is evidence order. A later Upgrade or unavailable
    // receipt never erases an earlier authoritative Install receipt.
    std::vector<InvocationDependencyTransaction> transactions;
};

enum class CleanupBaselineObservation {
    PreExisting,
    NewlyObserved,
    Unknown,
};

enum class CleanupInstalledState {
    Present,
    Absent,
    Unknown,
};

// Causal ownership is deliberately independent from baseline observation.
// In particular, NewlyObserved cannot be passed as InvocationOwned.
// Unknown is missing causal proof, not affirmative evidence of another owner.
// Cleanup eligibility does not require positive proof, but NotInvocationOwned
// remains protective evidence.
enum class CleanupCausalOwnership {
    InvocationOwned,
    NotInvocationOwned,
    Unknown,
};

enum class CleanupSharedRequirementState {
    StillRequired,
    NoLongerRequired,
    Unknown,
};

enum class CleanupEvidenceVerification {
    Verified,
    Unverified,
};

// This is set-level authority and is independent from verification of any
// individual correlation. A verified edge never proves that no other root,
// PackageBase, role, or dependency edge exists.
enum class CleanupCorrelationCoverage {
    Complete,
    Incomplete,
    Unknown,
};

// Adapters may project group or other policy evidence without teaching this
// pure model package-name heuristics.
enum class CleanupPolicyProtection {
    NotProtected,
    Protected,
    Unknown,
};

enum class CleanupRouteKind {
    RemoteAurSourceBuild,
    LocalSourceBuild,
    Upgrade,
    UpgradeAur,
    UpgradeAll,
    StandaloneRepositorySourceBuild,
    MakepkgSyncDependencies,
    Unknown,
};

enum class CleanupRouteAuthority {
    Complete,
    Unsupported,
    Unknown,
};

enum class CleanupEvidenceCompleteness {
    Complete,
    Incomplete,
    Unknown,
};

struct CleanupCurrentPackageEvidence {
    CleanupInstalledState state;
    std::optional<InstalledPackageMetadata> metadata;
    CleanupEvidenceVerification verification;
};

struct CleanupProviderCorrelation {
    ProvidedDependency provider;
    ProviderResolutionKind resolution;

    bool operator==(const CleanupProviderCorrelation&) const = default;
};

// This is the minimal pure projection of a BuildPlan dependency edge. It
// retains the edge index, requiring source package, typed requirement, and
// provider identity without owning BuildPlan or an execution object.
struct CleanupDependencyEdgeCorrelation {
    std::size_t build_plan_edge_index;
    PackageChildIdentity requiring_package;
    DependencyRequirement requirement;
    std::optional<CleanupProviderCorrelation> provider;

    bool operator==(const CleanupDependencyEdgeCorrelation&) const =
        default;
};

// One package may retain several root, PackageBase, role, and edge
// correlations. Vector order is evidence order and never classification
// priority. Verified means the adapter checked this correlation's role, edge,
// typed requirement, and any provider association against one authoritative
// observation; it does not establish set-level completeness.
struct CleanupPackageCorrelation {
    RootTargetIdentity requested_root;
    PackageChildIdentity package;
    std::optional<PackageRole> role;
    std::optional<CleanupDependencyEdgeCorrelation> dependency_edge;
    CleanupEvidenceVerification verification;

    bool operator==(const CleanupPackageCorrelation&) const = default;
};

// The historical name is retained with the existing projection interfaces.
// Eligible describes complete cleanup safety evidence, not proven causality.
struct InvocationOwnedCleanupCandidate {
    SourceAwarePackageIdentity package;
    CleanupBaselineObservation baseline;
    CleanupCurrentPackageEvidence current_package;
    CleanupCausalOwnership causal_ownership;
    CleanupSharedRequirementState shared_requirement;
    CleanupPolicyProtection policy_protection;
    CleanupCorrelationCoverage correlation_coverage;
    std::vector<CleanupPackageCorrelation> correlations;
};

enum class CleanupClassification {
    Eligible,
    Protected,
    Unknown,
    Invalid,
};

// Declaration order is the canonical reason order. The classifier returns
// only reasons for the selected precedence arm.
enum class CleanupClassificationReason {
    InvalidTypedState,
    CurrentPackageIdentityMismatch,
    CurrentPackageVersionMismatch,
    MalformedRequestedRootIdentity,
    CorrelationPackageIdentityMismatch,
    CorrelationShapeMismatch,
    DependencyRequirementIdentityMismatch,
    ProviderIdentityMismatch,
    ProviderRequirementIdentityMismatch,

    PreExisting,
    CurrentPackageAbsent,
    ExplicitInstallReason,
    KnownNotInvocationOwned,
    RootTarget,
    RuntimeDependency,
    StillRequired,
    PolicyProtected,

    BaselineObservationUnknown,
    CurrentInstalledStateUnknown,
    InstallReasonUnknown,
    CurrentPackageVersionUnknown,
    CurrentPackageVersionUnavailable,
    CurrentPackageBaseUnknown,
    CurrentPackageArchitectureUnknown,
    // Historical reason; retained in canonical order, no longer emitted.
    CausalOwnershipUnknown,
    CurrentPackageEvidenceUnverified,
    CorrelationCoverageIncomplete,
    CorrelationCoverageUnknown,
    DependencyCorrelationMissing,
    DependencyRoleUnknown,
    DependencyEdgeCorrelationMissing,
    DependencyCorrelationUnverified,
    BuildOrCheckDependencyCorrelationMissing,
    SharedRequirementUnknown,
    PolicyProtectionUnknown,

    EligibleEvidenceComplete,
};

class CleanupClassificationResult final {
public:
    CleanupClassificationResult() = delete;
    CleanupClassificationResult(const CleanupClassificationResult&) = default;
    CleanupClassificationResult(CleanupClassificationResult&&) noexcept =
        default;
    CleanupClassificationResult& operator=(
        const CleanupClassificationResult&) = default;
    CleanupClassificationResult& operator=(
        CleanupClassificationResult&&) noexcept = default;
    ~CleanupClassificationResult() = default;

    [[nodiscard]] CleanupClassification classification() const noexcept;
    [[nodiscard]] const std::vector<CleanupClassificationReason>& reasons()
        const noexcept;

    bool operator==(const CleanupClassificationResult&) const = default;

private:
    CleanupClassificationResult(
        CleanupClassification classification,
        std::vector<CleanupClassificationReason> reasons) noexcept;

    CleanupClassification classification_;
    std::vector<CleanupClassificationReason> reasons_;

    friend CleanupClassificationResult classify_invocation_owned_cleanup(
        const InvocationOwnedCleanupCandidate& candidate);
};

// Precedence is Invalid, then Protected, then Unknown, and finally Eligible.
// The function reads no external state and grants no cleanup capability.
[[nodiscard]] CleanupClassificationResult classify_invocation_owned_cleanup(
    const InvocationOwnedCleanupCandidate& candidate);
