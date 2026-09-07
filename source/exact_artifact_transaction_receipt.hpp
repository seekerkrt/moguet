#pragma once

#include "evaluated_devel_source_artifact_transport.hpp"
#include "exact_artifact_transaction_protocol.hpp"

// Successful exact operation receipt, independent of DB observation success.
// No parser or raw-record factory grants this capability. The transport owner
// mints it only after known successful execution and complete actual coverage.
class ExactArtifactTransactionReceipt final {
public:
    ExactArtifactTransactionReceipt() = delete;
    ExactArtifactTransactionReceipt(const ExactArtifactTransactionReceipt&) = delete;
    ExactArtifactTransactionReceipt& operator=(const ExactArtifactTransactionReceipt&) = delete;
    ExactArtifactTransactionReceipt(ExactArtifactTransactionReceipt&&) noexcept = default;
    ExactArtifactTransactionReceipt& operator=(ExactArtifactTransactionReceipt&&) = delete;
    ~ExactArtifactTransactionReceipt() = default;

    [[nodiscard]] const SourceArtifactInstallRootPrepareRequest& manifest() const noexcept;
    [[nodiscard]] const std::string& staged_identity_sha256() const noexcept;
    [[nodiscard]] const ExactArtifactOperationRecords& operations() const noexcept;
    [[nodiscard]] const ExactArtifactRootEvidence& database_evidence() const noexcept;

private:
    friend class EvaluatedDevelSourceArtifactTransport;
    ExactArtifactTransactionReceipt(SourceArtifactInstallRootPrepareRequest manifest, std::string stage,
                                    ExactArtifactOperationRecords operations, ExactArtifactRootEvidence evidence) noexcept;
    SourceArtifactInstallRootPrepareRequest manifest_;
    std::string staged_identity_sha256_;
    ExactArtifactOperationRecords operations_;
    ExactArtifactRootEvidence evidence_;
};
