#pragma once

#include "evaluated_devel_source_artifact_transport.hpp"
#include "exact_artifact_transaction_protocol.hpp"
#include "installed_artifact_binding_observer_authority.hpp"

// Successful exact operation receipt, independent of DB observation success.
// No parser or raw-record factory grants this capability. The transport owner
// mints it only after known successful execution and complete actual coverage.
class ExactArtifactTransactionReceipt final {
public:
    ExactArtifactTransactionReceipt() = delete;
    ExactArtifactTransactionReceipt(const ExactArtifactTransactionReceipt&) = delete;
    ExactArtifactTransactionReceipt& operator=(const ExactArtifactTransactionReceipt&) = delete;
    ExactArtifactTransactionReceipt(ExactArtifactTransactionReceipt&& other) noexcept;
    ExactArtifactTransactionReceipt& operator=(ExactArtifactTransactionReceipt&&) = delete;
    ~ExactArtifactTransactionReceipt() = default;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const SourceArtifactInstallRootPrepareRequest& manifest() const noexcept;
    [[nodiscard]] const std::string& staged_identity_sha256() const noexcept;
    [[nodiscard]] const ExactArtifactOperationRecords& operations() const noexcept;
    [[nodiscard]] const ExactArtifactRootEvidence& database_evidence() const noexcept;

private:
    friend class EvaluatedDevelSourceArtifactTransport;
    friend class InstalledArtifactBindingObserver;
    friend class DevelSourceArtifactInstallAuthority;
#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
    friend class DevelSourceArtifactInstallFixture;
#endif
    ExactArtifactTransactionReceipt(SourceArtifactInstallRootPrepareRequest manifest, std::string stage,
                                    ExactArtifactOperationRecords operations, ExactArtifactRootEvidence evidence,
                                    std::shared_ptr<const unsigned char> built_lineage,
                                    std::shared_ptr<const unsigned char> transaction_lineage) noexcept;
    SourceArtifactInstallRootPrepareRequest manifest_;
    std::string staged_identity_sha256_;
    ExactArtifactOperationRecords operations_;
    ExactArtifactRootEvidence evidence_;
    std::shared_ptr<const unsigned char> built_lineage_;
    std::shared_ptr<const unsigned char> transaction_lineage_;
    bool active_ = true;
};
