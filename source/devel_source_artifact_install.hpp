#pragma once

#include "devel_source_artifact_install_authority.hpp"
#include "exact_artifact_transaction_protocol.hpp"
#include "source_artifact_install_trusted_transport.hpp"

#include <memory>
#include <optional>
#include <variant>

class EvaluatedDevelSourceBuildProof;
class ExactArtifactTransactionReceipt;
class InstalledArtifactBinding;

enum class DevelSourceArtifactInstallOperation { NotAttempted,
                                                 Succeeded,
                                                 Failed,
                                                 OutcomeUnknown };
enum class DevelSourceArtifactInstallReceipt { NotAttempted,
                                               Missing,
                                               Incomplete,
                                               Invalid,
                                               Complete };
enum class DevelSourceArtifactInstallProof { NotAttempted,
                                             Incomplete,
                                             Complete };
enum class DevelSourceArtifactInstallCleanupState { Pending,
                                                    Complete,
                                                    Failed,
                                                    Retained,
                                                    OutcomeUnknown };
enum class DevelSourceArtifactInstallCleanupIssue { PrepareUnconfirmed,
                                                    AbortFailed,
                                                    ConsumeFailed,
                                                    ConsumeUnconfirmed,
                                                    RetirementFailed,
                                                    PrivateStageCleanupFailed };

struct DevelSourceArtifactInstallCleanup {
    DevelSourceArtifactInstallCleanupState state = DevelSourceArtifactInstallCleanupState::Pending;
    std::optional<DevelSourceArtifactInstallCleanupIssue> issue;
    std::optional<int> helper_exit_status;
};

enum class InstalledDevelSourceBuildIssue {
    NotExecuted,
    OperationNotSuccessful,
    ReceiptUnavailable,
    BindingUnavailable,
    UnsupportedCardinality,
    InactiveComponent,
    BuiltLineageMismatch,
    TransactionLineageMismatch,
    PackageIdentityMismatch,
    ArtifactIndexMismatch,
    ArchiveDigestMismatch,
    MtreeMismatch,
    InstalledGenerationMismatch,
    DatabaseRecordMismatch,
    ResourceFailure,
    InternalFailure,
};

// Live in-memory capability. Owns the original Slice 4 context/artifact,
// exact receipt and fresh binding. All authority needed from privileged stage
// files was captured before their retirement; no path is reread for validity.
// Destruction only releases owned local build resources. It does not execute
// a transaction, consume a token, touch the installed DB, or publish provenance.
class InstalledDevelSourceBuildProof final {
public:
    InstalledDevelSourceBuildProof() = delete;
    InstalledDevelSourceBuildProof(const InstalledDevelSourceBuildProof&) = delete;
    InstalledDevelSourceBuildProof& operator=(const InstalledDevelSourceBuildProof&) = delete;
    InstalledDevelSourceBuildProof(InstalledDevelSourceBuildProof&&) noexcept;
    InstalledDevelSourceBuildProof& operator=(InstalledDevelSourceBuildProof&&) = delete;
    ~InstalledDevelSourceBuildProof() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const EvaluatedDevelSourceBuildProof& built_proof() const;
    [[nodiscard]] const ExactArtifactTransactionReceipt& receipt() const;
    [[nodiscard]] const InstalledArtifactBinding& installed_binding() const;
    [[nodiscard]] std::size_t artifact_index() const;
    [[nodiscard]] ExactArtifactTransactionOperation operation() const;

private:
    friend class DevelSourceArtifactInstallAuthority;
    // Result only borrows diagnostics from the complete proof it owns.
    friend class DevelSourceArtifactInstallResult;
    explicit InstalledDevelSourceBuildProof(std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept;
    const DevelSourceArtifactInstallState& require_state() const;
    std::unique_ptr<DevelSourceArtifactInstallState> state_;
};

// Closed live result, not a logging DTO. The complete arm contains the sealed
// proof; consumers cannot set contradictory operation/receipt/proof states.
// A preallocated transport state is transferred without allocation at finalize.
class DevelSourceArtifactInstallResult final {
public:
    DevelSourceArtifactInstallResult() = delete;
    DevelSourceArtifactInstallResult(const DevelSourceArtifactInstallResult&) = delete;
    DevelSourceArtifactInstallResult& operator=(const DevelSourceArtifactInstallResult&) = delete;
    DevelSourceArtifactInstallResult(DevelSourceArtifactInstallResult&&) noexcept;
    DevelSourceArtifactInstallResult& operator=(DevelSourceArtifactInstallResult&&) = delete;
    ~DevelSourceArtifactInstallResult() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] DevelSourceArtifactInstallOperation operation() const;
    [[nodiscard]] std::optional<int> pacman_exit_status() const;
    [[nodiscard]] const SourceArtifactInstallTrustedExecutionResult* transport_result() const;
    [[nodiscard]] DevelSourceArtifactInstallReceipt receipt_state() const;
    [[nodiscard]] const ExactArtifactTransactionReceipt* receipt() const;
    [[nodiscard]] std::optional<ExactArtifactReceiptIssue> receipt_issue() const;
    [[nodiscard]] DevelSourceArtifactInstallProof proof_state() const;
    [[nodiscard]] const InstalledDevelSourceBuildProof* proof() const noexcept;
    [[nodiscard]] std::optional<InstalledDevelSourceBuildIssue> proof_issue() const;
    [[nodiscard]] std::optional<InstalledRecordObservationIssue> binding_issue() const;
    [[nodiscard]] const DevelSourceArtifactInstallCleanup& privileged_cleanup() const;
    // Local build context remains owned, including on Unknown. This is a
    // lifetime observation, not a promise that destructor cleanup will succeed.
    [[nodiscard]] DevelSourceArtifactInstallCleanupState source_context_cleanup() const;

private:
    friend class DevelSourceArtifactInstallAuthority;
    explicit DevelSourceArtifactInstallResult(std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept;
    explicit DevelSourceArtifactInstallResult(InstalledDevelSourceBuildProof proof) noexcept;
    const DevelSourceArtifactInstallState& require_state() const;
    std::variant<std::unique_ptr<DevelSourceArtifactInstallState>, InstalledDevelSourceBuildProof> value_;
};
