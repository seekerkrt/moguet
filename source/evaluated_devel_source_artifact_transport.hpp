#pragma once

#include "devel_source_artifact_install_authority.hpp"

#ifdef MOGUET_ENABLE_SOURCE_ARTIFACT_INSTALL_TRUSTED_TRANSPORT_TEST_HOOKS
#include <functional>
#endif

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
