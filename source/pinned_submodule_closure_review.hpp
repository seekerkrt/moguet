#pragma once

#include "interactive_confirmation.hpp"
#include "pinned_submodule_closure.hpp"
#include "user_config.hpp"

#include <iosfwd>
#include <optional>
#include <system_error>

enum class PinnedClosureReviewStage {
    Input,
    Presentation,
    Output,
    Confirmation,
};

enum class PinnedClosureReviewFailureReason {
    InvalidClosure,
    ReviewSkipped,
    NoConfirm,
    NonInteractiveInput,
    UnsupportedContent,
    ResourceLimitExceeded,
    RenderFailure,
    OutputFailure,
    Declined,
    Cancelled,
    InputFailure,
};

struct PinnedClosureReviewFailure {
    PinnedClosureReviewStage stage;
    PinnedClosureReviewFailureReason reason;
    std::optional<std::size_t> node;
    std::optional<std::size_t> entry;
    std::optional<ConfirmationCancellationReason> cancellation;
    std::optional<std::error_code> io_error;
    PinnedClosureCleanupResult cleanup;
};

class AcceptedPinnedSubmoduleClosure;
using PinnedClosureReviewResult = std::variant<AcceptedPinnedSubmoduleClosure, PinnedClosureReviewFailure>;

// Explicit authorization to build the exact upstream snapshot, not a claim
// that its contents were audited or are safe, nor recipe acceptance/S4 proof.
// The whole 4A owner survives. Only acquisition backing is released privately
// after the complete independent SourceReady transfer proof.
class AcceptedPinnedSubmoduleClosure final {
public:
    AcceptedPinnedSubmoduleClosure() = delete;
    AcceptedPinnedSubmoduleClosure(const AcceptedPinnedSubmoduleClosure&) = delete;
    AcceptedPinnedSubmoduleClosure& operator=(const AcceptedPinnedSubmoduleClosure&) = delete;
    AcceptedPinnedSubmoduleClosure(AcceptedPinnedSubmoduleClosure&&) noexcept;
    AcceptedPinnedSubmoduleClosure& operator=(AcceptedPinnedSubmoduleClosure&&) = delete;
    ~AcceptedPinnedSubmoduleClosure() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const InvocationOwnedPinnedSubmoduleClosure& closure() const;
    [[nodiscard]] PinnedClosureCleanupResult cleanup() noexcept;

private:
    friend class PinnedSubmoduleWorkspaceAuthority;
    AcceptedPinnedSubmoduleClosure(InvocationOwnedPinnedSubmoduleClosure closure,
                                   ExplicitConfirmationAcceptance confirmation) noexcept;
    friend PinnedClosureReviewResult review_pinned_submodule_closure(
        InvocationOwnedPinnedSubmoduleClosure closure, PresentationDetail presentation_detail,
        ReviewPolicy diff_policy, bool no_confirm);
    InvocationOwnedPinnedSubmoduleClosure closure_;
    ExplicitConfirmationAcceptance confirmation_;
};

// The only mint obtains its own no-default confirmation after complete output.
// A migration/recipe token, raw metadata or an external "accepted" flag cannot
// substitute for this session. No object reads occur after the human prompt.
[[nodiscard]] PinnedClosureReviewResult review_pinned_submodule_closure(
    InvocationOwnedPinnedSubmoduleClosure closure, PresentationDetail presentation_detail,
    ReviewPolicy diff_policy = ReviewPolicy::Prompt, bool no_confirm = false);

#ifdef MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
#include <functional>
struct PinnedClosureReviewTestHooks {
    std::istream* input = nullptr;
    std::ostream* output = nullptr;
    std::optional<bool> interactive;
    std::optional<std::size_t> entries, rendered_bytes;
    std::function<void()> before_render;
};
void set_pinned_closure_review_test_hooks(PinnedClosureReviewTestHooks hooks);
#endif
