#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

inline constexpr std::size_t
    SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACTS = 1024;
inline constexpr std::uint64_t
    SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES =
        4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t
    SOURCE_ARTIFACT_INSTALL_MAXIMUM_TRANSACTION_BYTES =
        8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t
    SOURCE_ARTIFACT_INSTALL_MAXIMUM_SIGNATURE_BYTES =
        16ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t
    SOURCE_ARTIFACT_INSTALL_MAXIMUM_PROTOCOL_BYTES =
        16U * 1024U * 1024U;

enum class SourceArtifactInstallTrustedHelperCommand {
    Prepare,
    PrepareExact,
    InstallLegacy,
    Execute,
    ExecutionStatus,
    ObserveExecution,
    Record,
    RecordInstall,
    RecordUpgrade,
    ConsumeExact,
    Consume,
    Abort,
};

// The legacy route remains Install-only cleanup evidence. A distinct prepared
// protocol and fixed helper entry select the installed-binding purpose.
// LegacyArtifactInstall is an ordinary synchronous install with no receipt
// authority; its core name/version plus exact bytes preserve legacy selection.
enum class SourceArtifactInstallTrustedPurpose {
    CleanupInstallOnly,
    ExactInstalledBinding,
    LegacyArtifactInstall,
};

enum class SourceArtifactInstallTrustedDirective {
    PreserveExistingReason,
    AsDependency,
    AsExplicit,
};

enum class SourceArtifactInstallTrustedProtocolIssueKind {
    InvalidCommand,
    InvalidArgumentCount,
    InvalidTransactionToken,
    MissingArgumentSeparator,
    InvalidPackageBase,
    InvalidDirective,
    InvalidBoolean,
    EmptyArtifactSet,
    TooManyArtifacts,
    InvalidArtifactIndex,
    DuplicateArtifactIndex,
    InvalidPackageName,
    DuplicatePackageName,
    InvalidVersion,
    InvalidArchitecture,
    ArtifactPackageBaseMismatch,
    InvalidArtifactSize,
    InvalidSignatureSize,
    TransactionTooLarge,
    InputTooLarge,
    MissingFinalNewline,
    InvalidHeader,
    TruncatedProtocol,
    UnexpectedRecord,
    InvalidHookDirectory,
    InvalidStagedArtifactPath,
    InvalidReceiptState,
    InvalidDigest,
};

enum class SourceArtifactInstallSealingFailure {
    StagedArtifactDigestMismatch,
    StagedArtifactGenerationMismatch,
    StagedArtifactReplacement,
    StagedArtifactRevalidationFailure,
    SignatureDigestMismatch,
    TrustedTransportProtocolMismatch,
    ExecutableLaunchFailure,
    TransactionLifetimeBusy,
};

struct SourceArtifactInstallSealingRefusal {
    SourceArtifactInstallSealingFailure reason;
    int error_number = 0;
    bool operator==(const SourceArtifactInstallSealingRefusal&) const = default;
};

// A hook phase is positive package-manager-side evidence. Authorization is
// only permission to launch and cannot stand in for either observed phase.
enum class SourceArtifactInstallExecutionEvidence {
    Unobserved,
    PreTransaction,
    PostTransaction,
};

struct SourceArtifactInstallExecutionObservation {
    std::string transaction_token;
    bool authorized = false;
    std::optional<SourceArtifactInstallSealingRefusal> refusal;
    SourceArtifactInstallExecutionEvidence execution_evidence = SourceArtifactInstallExecutionEvidence::Unobserved;
};

struct SourceArtifactInstallTrustedProtocolFailure {
    SourceArtifactInstallTrustedProtocolIssueKind issue;
};

struct SourceArtifactInstallRootArtifactExpectation {
    std::size_t artifact_index;
    std::string package_name;
    std::string full_version;
    std::string package_base;
    std::string architecture;
    std::uint64_t artifact_size;
    std::uint64_t signature_size;
    // Exact bytes from the retained descriptor, verified again against the
    // sealed input and privileged stage. Signature identity is independent.
    std::string archive_sha256;
    // Legacy alone can retain an empty present .sig via SHA-256(empty);
    // size 0 with "-" remains absence. Receipt purposes keep the old contract.
    std::string signature_sha256;
    std::string raw_mtree_sha256 = "-";

    bool operator==(
        const SourceArtifactInstallRootArtifactExpectation&) const =
        default;
};

struct SourceArtifactInstallRootPrepareRequest {
    std::string transaction_token;
    std::string package_base;
    SourceArtifactInstallTrustedDirective directive;
    bool needed;
    bool no_confirm;
    std::vector<SourceArtifactInstallRootArtifactExpectation> artifacts;
    SourceArtifactInstallTrustedPurpose purpose = SourceArtifactInstallTrustedPurpose::CleanupInstallOnly;

    bool operator==(
        const SourceArtifactInstallRootPrepareRequest&) const = default;
};

struct SourceArtifactInstallTrustedHelperInvocation {
    SourceArtifactInstallTrustedHelperCommand command;
    std::string transaction_token;
    std::vector<SourceArtifactInstallRootArtifactExpectation> artifacts;
    std::string package_base;
    SourceArtifactInstallTrustedDirective directive =
        SourceArtifactInstallTrustedDirective::PreserveExistingReason;
    bool needed = false;
    bool no_confirm = false;
    std::string legacy_input_path = {};
};

using SourceArtifactInstallTrustedHelperInvocationResult = std::variant<
    SourceArtifactInstallTrustedHelperInvocation,
    SourceArtifactInstallTrustedProtocolFailure>;

using SourceArtifactInstallRootPreparedStateResult = std::variant<
    SourceArtifactInstallRootPrepareRequest,
    SourceArtifactInstallTrustedProtocolFailure>;

struct SourceArtifactInstallStagedArtifact {
    std::size_t artifact_index;
    std::string path;

    bool operator==(const SourceArtifactInstallStagedArtifact&) const =
        default;
};

struct SourceArtifactInstallRootPrepareResponse {
    std::string transaction_token;
    std::string hook_directory;
    std::vector<SourceArtifactInstallStagedArtifact> artifacts;
    std::string staged_identity_sha256 = {};
};

using SourceArtifactInstallRootPrepareResponseResult = std::variant<
    SourceArtifactInstallRootPrepareResponse,
    SourceArtifactInstallTrustedProtocolFailure>;

enum class SourceArtifactInstallRootReceiptState {
    Missing,
    Complete,
};

struct SourceArtifactInstallRootReceipt {
    SourceArtifactInstallRootReceiptState state;
    std::string transaction_token;
    std::vector<std::string> installed_package_names;
};

using SourceArtifactInstallRootReceiptResult = std::variant<
    SourceArtifactInstallRootReceipt,
    SourceArtifactInstallTrustedProtocolFailure>;

[[nodiscard]] std::string_view
source_artifact_install_trusted_owner() noexcept;

[[nodiscard]] bool is_valid_source_artifact_install_root_request(
    const SourceArtifactInstallRootPrepareRequest& request) noexcept;

[[nodiscard]] SourceArtifactInstallTrustedHelperInvocationResult
parse_source_artifact_install_trusted_helper_arguments(
    const std::vector<std::string>& arguments);

[[nodiscard]] std::string
serialize_source_artifact_install_root_prepared_state(
    const SourceArtifactInstallRootPrepareRequest& request);

[[nodiscard]] SourceArtifactInstallRootPreparedStateResult
parse_source_artifact_install_root_prepared_state(
    std::string_view protocol);

[[nodiscard]] std::string source_artifact_install_hook_directory(
    std::string_view transaction_token);

[[nodiscard]] std::string source_artifact_install_hook_filename(
    std::string_view transaction_token);

[[nodiscard]] std::string source_artifact_install_staged_artifact_path(
    std::string_view transaction_token,
    std::size_t ordinal);

[[nodiscard]] std::string source_artifact_install_staged_signature_path(
    std::string_view transaction_token,
    std::size_t ordinal);

[[nodiscard]] std::string
serialize_source_artifact_install_root_prepare_response(
    const SourceArtifactInstallRootPrepareResponse& response,
    const SourceArtifactInstallRootPrepareRequest& request);

[[nodiscard]] SourceArtifactInstallRootPrepareResponseResult
parse_source_artifact_install_root_prepare_response(
    std::string_view protocol,
    const SourceArtifactInstallRootPrepareRequest& expected_request);

[[nodiscard]] std::string serialize_source_artifact_install_root_receipt(
    const SourceArtifactInstallRootReceipt& receipt);

[[nodiscard]] SourceArtifactInstallRootReceiptResult
parse_source_artifact_install_root_receipt(std::string_view protocol);

[[nodiscard]] bool is_valid_source_artifact_install_sha256(std::string_view digest) noexcept;
[[nodiscard]] std::string serialize_source_artifact_install_execution_observation(
    const SourceArtifactInstallExecutionObservation& observation);
[[nodiscard]] std::variant<SourceArtifactInstallExecutionObservation,
                           SourceArtifactInstallTrustedProtocolFailure>
parse_source_artifact_install_execution_observation(std::string_view protocol);
