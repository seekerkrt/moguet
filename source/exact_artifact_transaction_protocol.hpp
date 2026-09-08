#pragma once

#include "source_artifact_install_trusted_protocol.hpp"
#include "installed_package_record_observation.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

enum class ExactArtifactTransactionOperation { Install,
                                               Upgrade };

struct ExactArtifactOperationRecord {
    ExactArtifactTransactionOperation operation;
    SourceArtifactInstallRootArtifactExpectation artifact;
    bool operator==(const ExactArtifactOperationRecord&) const = default;
};

enum class ExactArtifactReceiptIssue { Missing,
                                       Invalid,
                                       ResourceFailure };

// Parsed protocol values are observations, never successful-transaction
// capabilities. Only the transport's known-success path can seal a receipt.
using ExactArtifactOperationRecords = std::vector<ExactArtifactOperationRecord>;
using ExactArtifactOperationRecordsResult = std::variant<ExactArtifactOperationRecords, ExactArtifactReceiptIssue>;

[[nodiscard]] std::string serialize_exact_artifact_operation_fragment(
    const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256,
    ExactArtifactTransactionOperation operation,
    const std::vector<std::string>& targets);

[[nodiscard]] ExactArtifactOperationRecordsResult parse_exact_artifact_operation_fragment(
    std::string_view fragment,
    const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256,
    ExactArtifactTransactionOperation fixed_operation);

// A missing kind is acceptable only when the other kind covers every selected
// artifact. Duplicate/cross-kind records are rejected before canonical ordering.
[[nodiscard]] ExactArtifactOperationRecordsResult join_exact_artifact_operation_fragments(
    const std::optional<std::string>& installs,
    const std::optional<std::string>& upgrades,
    const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256);

struct ExactArtifactRecordObservation {
    std::size_t artifact_index;
    std::string package_name;
    InstalledPackageRecordObservation observation;
};
using ExactArtifactRecordObservations = std::vector<ExactArtifactRecordObservation>;
using ExactArtifactRecordObservationsResult = std::variant<ExactArtifactRecordObservations, InstalledRecordObservationIssue>;

[[nodiscard]] std::string serialize_exact_artifact_record_observations(const ExactArtifactRecordObservations& observations);
[[nodiscard]] ExactArtifactRecordObservationsResult parse_exact_artifact_record_observations(
    std::string_view protocol, const SourceArtifactInstallRootPrepareRequest& manifest) noexcept;

// Cleanup is a consequence after the evidence has been captured. Numeric
// encoding is fixed-width so a helper can record failure without allocating.
enum class ExactArtifactRootCleanup { Complete = 0,
                                      RetirementFailed = 1,
                                      PrivateStageCleanupFailed = 2 };

// Core fragments and database observations retain independent failure states.
// This wire bundle is not the successful exact receipt capability.
struct ExactArtifactRootEvidence {
    ExactArtifactRootCleanup cleanup = ExactArtifactRootCleanup::Complete;
    InstalledDatabaseWorldResult world = InstalledRecordObservationIssue::UnsupportedDatabaseWorld;
    ExactArtifactRecordObservationsResult baseline = InstalledRecordObservationIssue::MissingBaseline;
    std::optional<std::string> installs;
    std::optional<std::string> upgrades;
    ExactArtifactRecordObservationsResult install_anchors = InstalledRecordObservationIssue::MissingAnchor;
    ExactArtifactRecordObservationsResult upgrade_anchors = InstalledRecordObservationIssue::MissingAnchor;
};
[[nodiscard]] std::string serialize_exact_artifact_root_evidence(
    const ExactArtifactRootEvidence& evidence, const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256);
[[nodiscard]] std::variant<ExactArtifactRootEvidence, ExactArtifactReceiptIssue> parse_exact_artifact_root_evidence(
    std::string_view protocol, const SourceArtifactInstallRootPrepareRequest& manifest,
    std::string_view staged_identity_sha256) noexcept;
