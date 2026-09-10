#pragma once

#include "source_artifact_install_trusted_protocol.hpp"
#include "exact_artifact_transaction_protocol.hpp"

#include <memory>
#include <stdexcept>
#include <string>

#include <sys/types.h>

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
#include <functional>
#endif

class SourceArtifactInstallTrustedStateError final
    : public std::runtime_error {
public:
    explicit SourceArtifactInstallTrustedStateError(const std::string& diagnostic);
    SourceArtifactInstallTrustedStateError(SourceArtifactInstallSealingFailure reason,
                                           const std::string& diagnostic, int error_number = 0);
    [[nodiscard]] const SourceArtifactInstallSealingRefusal& refusal() const noexcept;

private:
    SourceArtifactInstallSealingRefusal refusal_;
};

// The installed helper supplies a descriptor for fixed /run and uid 0.
// Tests may supply an isolated descriptor, but no helper CLI can select a
// state root. Source-artifact state never shares the selected-provider
// alpm-receipts subtree.
class SourceArtifactInstallTrustedStateStore final {
public:
    [[nodiscard]] static SourceArtifactInstallTrustedStateStore
    open_below_runtime_parent(
        int runtime_parent_fd, uid_t expected_owner);

    SourceArtifactInstallTrustedStateStore(
        const SourceArtifactInstallTrustedStateStore&) = delete;
    SourceArtifactInstallTrustedStateStore(
        SourceArtifactInstallTrustedStateStore&&) noexcept;
    SourceArtifactInstallTrustedStateStore& operator=(
        const SourceArtifactInstallTrustedStateStore&) = delete;
    SourceArtifactInstallTrustedStateStore& operator=(
        SourceArtifactInstallTrustedStateStore&&) noexcept;
    ~SourceArtifactInstallTrustedStateStore();

    [[nodiscard]] SourceArtifactInstallRootPrepareResponse prepare(
        const SourceArtifactInstallRootPrepareRequest& request,
        int sealed_artifact_input_fd);
    // Takes the prepared lifetime's shared lease and a separate one-shot claim,
    // retains the private namespace and stage FDs,
    // reproves their identities/digests, then execs fixed pacman with normal
    // archive + adjacent .sig paths. Production success never returns.
    [[nodiscard]] int execute(const std::string& transaction_token);
    [[nodiscard]] SourceArtifactInstallExecutionObservation execution_status(
        const std::string& transaction_token);
    // Fixed PreTransaction hook entry. Publishes phase evidence only, never an
    // Install receipt or installed provenance. No caller-selected phase/path.
    void observe_execution(const std::string& transaction_token);
    // Record shares the execution lifetime for PostTransaction. Only cleanup
    // takes an exclusive lease, held through reproof, retirement and unlink.
    void record(
        const std::string& transaction_token,
        int needs_targets_input_fd);
    void record_install(const std::string& transaction_token, int needs_targets_input_fd);
    void record_upgrade(const std::string& transaction_token, int needs_targets_input_fd);
    [[nodiscard]] std::string consume_exact(const std::string& transaction_token);
    [[nodiscard]] std::string consume(
        const std::string& transaction_token);
    void abort(const std::string& transaction_token);

private:
    void record_exact_operation(const std::string& transaction_token, int needs_targets_input_fd,
                                ExactArtifactTransactionOperation fixed_operation);
    struct Implementation;

    explicit SourceArtifactInstallTrustedStateStore(
        std::unique_ptr<Implementation> implementation) noexcept;

    std::unique_ptr<Implementation> implementation_;
};

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
enum class SourceArtifactInstallTrustedStateTestEvent {
    AfterArtifactMetadataValidation,
    BeforeFinalReproof,
    BeforeRefusalPublication,
    BeforeExactRetirement,
    BeforeExactCleanup,
};
using SourceArtifactInstallTrustedStateTestHook = std::function<void(
    SourceArtifactInstallTrustedStateTestEvent, int, const std::string&)>;
void set_source_artifact_install_trusted_state_test_hook(
    SourceArtifactInstallTrustedStateTestHook hook);
void set_source_artifact_install_trusted_exec_test_hook(
    std::function<int(const std::vector<std::string>&)> hook);
#endif
