#pragma once

#include "evaluated_devel_source_build_authority.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <memory>
#include <variant>

class AcceptedPinnedSubmoduleClosure;
class InvocationOwnedPinnedSubmoduleClosure;
class EvaluatedDevelSourceSelection;
class InvocationOwnedSourceBuildContext;
class SourceReadyPinnedSubmoduleWorkspace;
enum class PinnedWorkspaceStage;
struct PinnedSubmoduleWorkspaceData;
struct PinnedWorkspaceFailure;
struct PinnedWorkspaceCleanupResult;
struct PinnedClosureFailure;
struct ExplicitProcessInvocation;
struct BoundedProcessPolicy;
struct BoundedCapturedProcessResult;
using PinnedSubmoduleWorkspaceResult = std::variant<SourceReadyPinnedSubmoduleWorkspace, PinnedWorkspaceFailure>;

// Accepted whole owner -> exact fresh local workspace. This capability records
// a clean pre-prepare phase point, not continuous attestation or completed S4.
class SourceReadyPinnedSubmoduleWorkspace final {
public:
    SourceReadyPinnedSubmoduleWorkspace() = delete;
    SourceReadyPinnedSubmoduleWorkspace(const SourceReadyPinnedSubmoduleWorkspace&) = delete;
    SourceReadyPinnedSubmoduleWorkspace& operator=(const SourceReadyPinnedSubmoduleWorkspace&) = delete;
    SourceReadyPinnedSubmoduleWorkspace(SourceReadyPinnedSubmoduleWorkspace&&) noexcept;
    SourceReadyPinnedSubmoduleWorkspace& operator=(SourceReadyPinnedSubmoduleWorkspace&&) = delete;
    ~SourceReadyPinnedSubmoduleWorkspace() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::filesystem::path& root() const;
    [[nodiscard]] const AcceptedPinnedSubmoduleClosure& accepted() const;
    // Failure consumes the workspace and preserves cleanup consequences.
    [[nodiscard]] std::optional<PinnedWorkspaceFailure> reprove();
    [[nodiscard]] PinnedWorkspaceCleanupResult cleanup() noexcept;

private:
    friend class PinnedSubmoduleWorkspaceAuthority;
    explicit SourceReadyPinnedSubmoduleWorkspace(std::unique_ptr<PinnedSubmoduleWorkspaceData> data) noexcept;
    std::unique_ptr<PinnedSubmoduleWorkspaceData> data_;
};

// Complete declaration at every friendship boundary: neither another TU nor
// the opaque backing can invent an authority with extra construction access.
class PinnedSubmoduleWorkspaceAuthority final {
    PinnedSubmoduleWorkspaceAuthority() = delete;
    friend class SourceReadyPinnedSubmoduleWorkspace;
    friend class EvaluatedDevelSourceBuildAuthority;
    friend PinnedSubmoduleWorkspaceResult materialize_pinned_submodule_workspace(AcceptedPinnedSubmoduleClosure accepted);

    static PinnedSubmoduleWorkspaceResult materialize(AcceptedPinnedSubmoduleClosure accepted);
    static PinnedWorkspaceCleanupResult cleanup(PinnedSubmoduleWorkspaceData& data) noexcept;
    // Internal borrow only: the consumer moves the whole SourceReady owner
    // through common S4 and into the result; selection is never released.
    static EvaluatedDevelSourceSelection& selection(SourceReadyPinnedSubmoduleWorkspace& workspace);
    static EvaluatedDevelSourceSelection& selection(InvocationOwnedPinnedSubmoduleClosure& closure);
    static std::optional<PinnedWorkspaceFailure> prepare_native(SourceReadyPinnedSubmoduleWorkspace& workspace);
    static std::optional<PinnedWorkspaceFailure> reprove_execution(SourceReadyPinnedSubmoduleWorkspace& workspace, PinnedWorkspaceStage stage);
    static int srcdest_descriptor(const EvaluatedDevelSourceSelection& selection);
    static const InvocationOwnedSourceBuildContext& context(const EvaluatedDevelSourceSelection& selection);
    static int builddir_descriptor(const EvaluatedDevelSourceSelection& selection);
    static void refuse_context_cleanup(const EvaluatedDevelSourceSelection& selection) noexcept;
    static BoundedCapturedProcessResult execute(const ExplicitProcessInvocation& invocation, const BoundedProcessPolicy& policy);
    // Local object copy only; failure preserves 4A detail but does not clean
    // its parent context before the derived workspace has checked ownership.
    static std::optional<PinnedClosureFailure> clone_objects(
        const InvocationOwnedPinnedSubmoduleClosure& closure, std::size_t node,
        const std::filesystem::path& target, int parent_descriptor,
        std::chrono::steady_clock::time_point deadline,
        BoundedCapturedProcessResult (*runner)(const ExplicitProcessInvocation&, const BoundedProcessPolicy&));
};
