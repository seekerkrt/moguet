#pragma once

#include "devel_source_artifact_install.hpp"
#include "evaluated_devel_source_build.hpp"
#include "exact_artifact_transaction_receipt.hpp"
#include "fresh_installed_artifact_binding.hpp"

// Internal storage shared by transport and finalizer, never returned to a
// consumer. One allocation before any privileged attempt owns every terminal
// result arm. Finalization transfers this storage instead of allocating a proof.
struct DevelSourceArtifactInstallState {
    EvaluatedDevelSourceBuildProof proof;
    bool consumed = false;
    bool exact_attempted = false;
    std::optional<std::string> transaction_token;
    std::optional<ExactArtifactTransactionReceipt> receipt;
    std::optional<ExactArtifactReceiptIssue> receipt_issue;
    std::optional<FreshInstalledArtifactBinding> fresh_binding;
    std::optional<InstalledRecordObservationIssue> binding_issue;
    std::optional<SourceArtifactInstallTrustedExecutionResult> execution;
    DevelSourceArtifactInstallOperation operation = DevelSourceArtifactInstallOperation::NotAttempted;
    std::optional<int> pacman_exit_status;
    DevelSourceArtifactInstallCleanup cleanup{DevelSourceArtifactInstallCleanupState::Complete, {}, {}};
    std::optional<InstalledDevelSourceBuildIssue> proof_issue = InstalledDevelSourceBuildIssue::NotExecuted;
    std::shared_ptr<const unsigned char> transaction_lineage = std::make_shared<const unsigned char>(0);
    std::size_t built_artifact_count = 1;
    std::size_t fresh_binding_count = 0;
#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
    bool fail_final_correlation = false;
#endif

    explicit DevelSourceArtifactInstallState(EvaluatedDevelSourceBuildProof value)
        : proof(std::move(value)) {
    }
};
