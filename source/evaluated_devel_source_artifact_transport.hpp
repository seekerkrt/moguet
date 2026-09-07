#pragma once

#include <memory>
#include <optional>
#include <string>

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
#include <functional>
#endif

class EvaluatedDevelSourceBuildProof;
class SourceArtifactInstallTrustedExecutionResult;
struct ArtifactInstallExecutionOptions;

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

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_for_test(
        const ArtifactInstallExecutionOptions& options,
        const std::string& transaction_token);
#endif

private:
    friend EvaluatedDevelSourceArtifactTransport
    prepare_evaluated_devel_source_artifact_transport(EvaluatedDevelSourceBuildProof proof);

    struct State;
    explicit EvaluatedDevelSourceArtifactTransport(std::unique_ptr<State> state) noexcept;
    [[nodiscard]] SourceArtifactInstallTrustedExecutionResult execute_impl(
        const ArtifactInstallExecutionOptions& options,
        const std::string* test_token);
    std::unique_ptr<State> state_;
};

// Consumes the entire proof; throws on an inactive/moved-from input. Snapshot
// and its saved-digest recheck occur at execute, before any privileged call.
[[nodiscard]] EvaluatedDevelSourceArtifactTransport
prepare_evaluated_devel_source_artifact_transport(EvaluatedDevelSourceBuildProof proof);

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
struct ExplicitProcessInvocation;
struct ExplicitProcessExecutionResult;
struct CapturedCommandResult;

// Replaces only this transport's privileged process boundary. Slice 4's real
// Git/makepkg/metadata processes still run in the isolated build fixture.
struct EvaluatedDevelSourceArtifactTransportTestHooks {
    std::function<CapturedCommandResult(const ExplicitProcessInvocation&)> capture;
    std::function<ExplicitProcessExecutionResult(const ExplicitProcessInvocation&)> execute;
    std::function<void(int)> before_snapshot_copy;
};
void set_evaluated_devel_source_artifact_transport_test_hooks(
    EvaluatedDevelSourceArtifactTransportTestHooks hooks);
#endif
