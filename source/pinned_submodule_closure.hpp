#pragma once

#include "evaluated_devel_source_build.hpp"
#include "reviewed_source_git_parser.hpp"

// Object-level acquisition limits, independent of recipe/build budgets.
struct PinnedClosureLimits {
    std::size_t depth = 16;
    std::size_t edges = 128;
    std::size_t tree_records = 262144;
    std::size_t declaration_bytes = 256 * 1024;
    std::size_t aggregate_declaration_bytes = 1024 * 1024;
    std::size_t metadata_bytes = 64 * 1024 * 1024;
    std::size_t processes = 4096;
};

enum class PinnedClosureStage {
    Input,
    WorkspaceCreation,
    RootObservation,
    RootAcquisition,
    Declaration,
    ChildAcquisition,
    ObjectProof,
    RecursiveTraversal,
    Cleanup,
};
enum class PinnedClosureFailureReason {
    InvalidSelection,
    UnsupportedTransport,
    MalformedObservation,
    DeclarationMismatch,
    MalformedDeclaration,
    UnsupportedUpdatePolicy,
    UnsafePath,
    PinnedObjectUnavailable,
    UnexpectedObjectType,
    ObjectFormatMismatch,
    MalformedTree,
    UnsafeFilesystem,
    IdentityChanged,
    MalformedRepository,
    ResourceLimitExceeded,
    Cancelled,
    GitProcessFailed,
    IoFailure,
    CreationFailed,
    RecursiveCycle,
};
struct PinnedClosureCleanupFailure {
    PinnedClosureFailureReason reason;
    std::optional<int> error_number;
};
struct PinnedClosureCleanupResult {
    std::optional<PinnedClosureCleanupFailure> objects;
    std::optional<InvocationOwnedSourceBuildContextFailure> selection;
    [[nodiscard]] bool succeeded() const noexcept {
        return !objects && !selection;
    }
};
struct PinnedClosureFailure {
    PinnedClosureStage stage;
    PinnedClosureFailureReason reason;
    std::optional<int> error_number = std::nullopt;
    std::optional<BoundedCapturedProcessResult> process = std::nullopt;
    PinnedClosureCleanupResult cleanup{};
    std::optional<std::filesystem::path> abandoned_root = std::nullopt;
};

// These are immutable observations when borrowed from the live owner, not
// constructors for acquisition authority. Each occurrence keeps its own edge.
struct PinnedSubmoduleNode {
    std::string locator;
    ReviewedSourceObjectId commit;
    ReviewedSourceObjectId tree;
    ReviewedSourceTreeInventory inventory;
    std::optional<std::size_t> parent_edge;
};
struct PinnedSubmoduleEdge {
    std::size_t parent;
    std::size_t child;
    std::string logical_name;
    std::string path;
    std::string locator;
    ReviewedSourceObjectId pin;
};

struct PinnedSubmoduleClosureData; // No friendship or minting privileges.
class InvocationOwnedPinnedSubmoduleClosure;
using PinnedSubmoduleClosureResult = std::variant<InvocationOwnedPinnedSubmoduleClosure, PinnedClosureFailure>;

// Consumes and retains the same-lineage evaluated selection. This is neither
// closure acceptance, a source-ready workspace, S4, nor persistent provenance.
class InvocationOwnedPinnedSubmoduleClosure final {
public:
    InvocationOwnedPinnedSubmoduleClosure() = delete;
    InvocationOwnedPinnedSubmoduleClosure(const InvocationOwnedPinnedSubmoduleClosure&) = delete;
    InvocationOwnedPinnedSubmoduleClosure& operator=(const InvocationOwnedPinnedSubmoduleClosure&) = delete;
    InvocationOwnedPinnedSubmoduleClosure(InvocationOwnedPinnedSubmoduleClosure&&) noexcept;
    InvocationOwnedPinnedSubmoduleClosure& operator=(InvocationOwnedPinnedSubmoduleClosure&&) = delete;
    ~InvocationOwnedPinnedSubmoduleClosure() noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const EvaluatedDevelSourceSelection& selection() const;
    [[nodiscard]] const std::vector<PinnedSubmoduleNode>& nodes() const;
    [[nodiscard]] const std::vector<PinnedSubmoduleEdge>& edges() const;
    // Retrieves only an inventoried non-gitlink entry from the owned backing.
    // Caller cannot substitute a path, repository, ref or arbitrary object.
    [[nodiscard]] std::variant<std::string, PinnedClosureFailure> read_blob(
        std::size_t node, std::size_t entry);
    [[nodiscard]] PinnedClosureCleanupResult cleanup() noexcept;

private:
    explicit InvocationOwnedPinnedSubmoduleClosure(std::unique_ptr<PinnedSubmoduleClosureData>) noexcept;
    friend PinnedSubmoduleClosureResult acquire_pinned_submodule_closure(EvaluatedDevelSourceSelection selection);
    std::unique_ptr<PinnedSubmoduleClosureData> data_;
};
[[nodiscard]] PinnedSubmoduleClosureResult acquire_pinned_submodule_closure(EvaluatedDevelSourceSelection selection);

#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
#include <functional>
struct PinnedClosureTestHooks {
    std::optional<PinnedClosureLimits> limits;
    std::function<void(PinnedClosureStage, const std::filesystem::path&)> event;
    std::function<BoundedCapturedProcessResult(const ExplicitProcessInvocation&, const BoundedProcessPolicy&)> process;
    std::function<void(const std::filesystem::path&)> before_remove;
    bool fail_next_backing_allocation = false;
};
void set_pinned_closure_test_hooks(PinnedClosureTestHooks hooks);
#endif
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
// Pure declaration observation seam: never constructs a live owner.
[[nodiscard]] std::optional<PinnedClosureFailure> check_pinned_declaration_fixture(std::string_view bytes);
#endif
