#pragma once

#include "pinned_submodule_closure_review.hpp"
#include "pinned_submodule_workspace_authority.hpp"

enum class PinnedWorkspaceStage {
    Input,
    ObjectTransfer,
    RootMaterialization,
    ChildMaterialization,
    GitdirBinding,
    SourceReadyReproof,
    NativePreparation,
    PreparedReproof,
    PostBuildReproof,
    Cleanup,
};
enum class PinnedWorkspaceFailureReason {
    InvalidAcceptedClosure,
    MaterializationFailed,
    GitProcessFailed,
    GitfileMismatch,
    GitdirMismatch,
    RevisionDrift,
    DeclarationDrift,
    MissingModule,
    UnexpectedModule,
    UnsafeFilesystem,
    ResourceLimitExceeded,
    Cancelled,
    TagNamespaceDrift,
    TagObjectInvalid,
};
struct PinnedWorkspaceCleanupFailure {
    PinnedWorkspaceFailureReason reason;
    std::optional<int> error_number;
};
struct PinnedWorkspaceCleanupResult {
    // Workspace ownership refusal is distinct from object backing and the
    // enclosing selection/context cleanup. Context owns the bounded removal.
    std::optional<PinnedWorkspaceCleanupFailure> workspace;
    PinnedClosureCleanupResult closure;
    [[nodiscard]] bool succeeded() const noexcept {
        return !workspace && closure.succeeded();
    }
};
struct PinnedWorkspaceFailure {
    PinnedWorkspaceStage stage;
    PinnedWorkspaceFailureReason reason;
    std::optional<int> error_number = std::nullopt;
    std::optional<BoundedCapturedProcessResult> process = std::nullopt;
    std::optional<PinnedClosureFailure> acquisition = std::nullopt;
    std::optional<InvocationOwnedSourceBuildContextFailure> context = std::nullopt;
    PinnedWorkspaceCleanupResult cleanup{};
    std::optional<std::filesystem::path> abandoned_workspace = std::nullopt;
};

[[nodiscard]] PinnedSubmoduleWorkspaceResult materialize_pinned_submodule_workspace(AcceptedPinnedSubmoduleClosure accepted);

#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_WORKSPACE_TEST_HOOKS
#include <functional>
struct PinnedWorkspaceTestHooks {
    std::function<void(const std::filesystem::path&)> before_reproof;
    std::function<void(const std::filesystem::path&)> before_cleanup;
    std::function<BoundedCapturedProcessResult(const ExplicitProcessInvocation&, const BoundedProcessPolicy&)> process;
    bool fail_next_allocation = false;
};
void set_pinned_workspace_test_hooks(PinnedWorkspaceTestHooks hooks);
#endif
