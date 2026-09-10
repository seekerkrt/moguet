#pragma once

#include "devel_build_provenance_publication_authority.hpp"
#include "devel_build_provenance_store.hpp"
#include "devel_source_artifact_install.hpp"

#include <cstdint>
#include <optional>
#include <string>

enum class DevelBuildProvenancePublicationRequest { Publish,
                                                    NotRequested };
enum class DevelBuildProvenancePublicationState { NotAttempted,
                                                  Complete,
                                                  Failed,
                                                  OutcomeUnknown };
enum class DevelBuildProvenancePublicationStage { Eligibility,
                                                  Projection,
                                                  Serialization,
                                                  PredecessorRead,
                                                  StorePublication };
enum class DevelBuildProvenancePublicationIssue {
    NotExecuted,
    OperationUnknown,
    OperationFailed,
    ReceiptUnavailable,
    ProofIncomplete,
    NotRequested,
    ProjectionRejected,
    ResourceFailure,
    SerializationFailure,
    StoreReadRejected,
    StorePublicationFailed,
    StorePublicationUncertain,
    InternalFailure,
};

// Historical publication identity, not a mint/retry capability. Only the
// closed result can attest Complete; copying these diagnostics cannot do so.
struct DevelBuildProvenancePublicationIdentity {
    PackageBaseIdentity package_base;
    std::uint64_t generation;
    std::string document_sha256;
};

// Owns the original S5 product even on local/store failure. Public access is
// diagnostic/const only: no extraction, retry, rebase, or republish handle.
class DevelBuildProvenancePublicationResult final {
public:
    DevelBuildProvenancePublicationResult() = delete;
    DevelBuildProvenancePublicationResult(const DevelBuildProvenancePublicationResult&) = delete;
    DevelBuildProvenancePublicationResult& operator=(const DevelBuildProvenancePublicationResult&) = delete;
    DevelBuildProvenancePublicationResult(DevelBuildProvenancePublicationResult&&) noexcept;
    DevelBuildProvenancePublicationResult& operator=(DevelBuildProvenancePublicationResult&&) = delete;
    ~DevelBuildProvenancePublicationResult() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const DevelSourceArtifactInstallResult& installation() const;
    [[nodiscard]] DevelBuildProvenancePublicationState state() const;
    [[nodiscard]] DevelBuildProvenancePublicationStage stage() const;
    [[nodiscard]] std::optional<DevelBuildProvenancePublicationIssue> issue() const;
    [[nodiscard]] std::optional<DevelBuildProvenanceFailure> projection_issue() const;
    [[nodiscard]] const DevelBuildProvenance* projected_provenance() const;
    [[nodiscard]] const DevelBuildProvenanceStoreReadResult* store_read_result() const;
    [[nodiscard]] const DevelBuildProvenanceStorePublishResult* store_publish_result() const;
    [[nodiscard]] const DevelBuildProvenancePublicationIdentity* identity() const noexcept;

private:
    friend class DevelBuildProvenancePublicationAuthority;
    explicit DevelBuildProvenancePublicationResult(DevelSourceArtifactInstallResult result) noexcept;
    void require_valid() const;

    DevelSourceArtifactInstallResult installation_;
    bool active_ = true;
    DevelBuildProvenancePublicationState state_ = DevelBuildProvenancePublicationState::NotAttempted;
    DevelBuildProvenancePublicationStage stage_ = DevelBuildProvenancePublicationStage::Eligibility;
    std::optional<DevelBuildProvenancePublicationIssue> issue_;
    std::optional<DevelBuildProvenanceFailure> projection_issue_;
    std::optional<DevelBuildProvenance> projected_;
    std::string encoded_;
    std::optional<DevelBuildProvenancePublicationIdentity> identity_;
    std::optional<DevelBuildProvenanceStoreReadResult> read_;
    std::optional<DevelBuildProvenanceStorePublishResult> publication_;
};

// One-shot ownership transfer. Invalid/moved-from input returns nullopt with
// no store call. Eligible proof is a historical installed observation: this
// function never reopens live build/transaction/installed paths.
[[nodiscard]] std::optional<DevelBuildProvenancePublicationResult>
publish_installed_devel_source_build(
    DevelSourceArtifactInstallResult result,
    DevelBuildProvenancePublicationRequest request = DevelBuildProvenancePublicationRequest::Publish) noexcept;

#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
using DevelBuildProvenancePublicationTestHook = void (*)(DevelBuildProvenancePublicationStage);
void set_devel_build_provenance_publication_test_hook(DevelBuildProvenancePublicationTestHook hook);
#endif
