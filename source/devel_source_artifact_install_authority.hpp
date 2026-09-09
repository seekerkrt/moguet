#pragma once

#include <memory>
#include <optional>
#include <string>

class EvaluatedDevelSourceArtifactTransport;
class EvaluatedDevelSourceBuildProof;
class DevelSourceArtifactInstallResult;
struct DevelSourceArtifactInstallState;
enum class InstalledDevelSourceBuildIssue;

// The finalizer, transport and gated fixture share this cycle-free declaration
// header. Every granting narrow include sees their complete declarations,
// including the transport that can enter the private finalizer below.
class DevelSourceArtifactInstallAuthority final {
    DevelSourceArtifactInstallAuthority() = delete;
    friend class EvaluatedDevelSourceArtifactTransport;
    static DevelSourceArtifactInstallResult finalize(
        std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept;
    static std::optional<InstalledDevelSourceBuildIssue> correlate(
        const DevelSourceArtifactInstallState& state) noexcept;
};

#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
enum class DevelSourceArtifactInstallTestMismatch {
    BuiltLineage,
    ReceiptLineage,
    BindingLineage,
    PackageName,
    PackageBase,
    Version,
    Architecture,
    ArtifactIndex,
    ArchiveDigest,
    MtreeDigest,
    TransactionToken,
    Purpose,
    StagedIdentity,
    Generation,
    DatabaseDigest,
    DescriptorIdentity,
    ReceiptCardinality,
    BindingCardinality,
    BuiltCardinality,
    ResourceFailure,
};
class DevelSourceArtifactInstallFixture final {
public:
    static void mismatch(EvaluatedDevelSourceArtifactTransport& transport,
                         DevelSourceArtifactInstallTestMismatch mismatch);
    static void exchange_receipts(EvaluatedDevelSourceArtifactTransport& left,
                                  EvaluatedDevelSourceArtifactTransport& right,
                                  bool include_binding);
    static void exchange_bindings(EvaluatedDevelSourceArtifactTransport& left,
                                  EvaluatedDevelSourceArtifactTransport& right);
    static void replace_built(EvaluatedDevelSourceArtifactTransport& transport, EvaluatedDevelSourceBuildProof proof);
    static bool check_component_moves(EvaluatedDevelSourceArtifactTransport& transport);
};
#endif

class SourceArtifactInstallTrustedExecutionResult;
struct ArtifactInstallExecutionOptions;
class ExactArtifactTransactionReceipt;
class FreshInstalledArtifactBinding;
enum class ExactArtifactReceiptIssue;
enum class InstalledRecordObservationIssue;

// S5-A owns the original Slice 4 proof through the sealed transport attempt.
// This is an input capability, not a transaction receipt or installed proof.
// Its complete declaration seals friendship even from the narrow artifact
// header. No raw path, descriptor, metadata tuple, or decoded binding is input.
class EvaluatedDevelSourceArtifactTransport final {
public:
    EvaluatedDevelSourceArtifactTransport() = delete;
    EvaluatedDevelSourceArtifactTransport(const EvaluatedDevelSourceArtifactTransport&) = delete;
    EvaluatedDevelSourceArtifactTransport& operator=(const EvaluatedDevelSourceArtifactTransport&) = delete;
    EvaluatedDevelSourceArtifactTransport(EvaluatedDevelSourceArtifactTransport&&) noexcept;
    EvaluatedDevelSourceArtifactTransport& operator=(EvaluatedDevelSourceArtifactTransport&&) = delete;
    ~EvaluatedDevelSourceArtifactTransport() noexcept;

    [[nodiscard]] bool active() const noexcept;
    // Retained for diagnosis on OutcomeUnknown; never authorizes retry.
    [[nodiscard]] const std::optional<std::string>& transaction_token() const;

    // At most one attempt, including local rejection. Legacy cleanup
    // expectation/observation/operation_result remain absent on this route.
    // Numeric package-manager outcome still requires the shared execution
    // witness. Complete describes the existing transport protocol only.
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute(
        const ArtifactInstallExecutionOptions& options);

    // S5-B component producer; no normal CLI/source-route caller. Operation
    // outcome is returned independently of these receipt/observation outputs.
    // Finalize transfers this same owned state into the sealed Slice 5 result.
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_exact(
        const ArtifactInstallExecutionOptions& options);
    // One-shot even before execution (NotAttempted). A moved/finalized owner
    // or a legacy execute attempt returns nullopt; no execution is retried.
    [[nodiscard]] std::optional<DevelSourceArtifactInstallResult> finalize() noexcept;
    [[nodiscard]] const ExactArtifactTransactionReceipt* exact_receipt() const noexcept;
    [[nodiscard]] std::optional<ExactArtifactReceiptIssue> exact_receipt_issue() const noexcept;
    [[nodiscard]] const FreshInstalledArtifactBinding* fresh_binding() const noexcept;
    [[nodiscard]] std::optional<InstalledRecordObservationIssue> installed_binding_issue() const noexcept;

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_for_test(
        const ArtifactInstallExecutionOptions& options,
        const std::string& transaction_token);
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_exact_for_test(
        const ArtifactInstallExecutionOptions& options, const std::string& transaction_token);
#endif

private:
    friend EvaluatedDevelSourceArtifactTransport
    prepare_evaluated_devel_source_artifact_transport(EvaluatedDevelSourceBuildProof proof);

#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
    friend class DevelSourceArtifactInstallFixture;
#endif
    explicit EvaluatedDevelSourceArtifactTransport(std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept;
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_exact_impl(
        const ArtifactInstallExecutionOptions& options, const std::string* test_token);
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_impl(
        const ArtifactInstallExecutionOptions& options,
        const std::string* test_token, bool exact = false);
    std::unique_ptr<DevelSourceArtifactInstallState> state_;
};
