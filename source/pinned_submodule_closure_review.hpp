#pragma once

#include "interactive_confirmation.hpp"
#include "pinned_submodule_closure.hpp"
#include "user_config.hpp"

#include <iosfwd>
#include <optional>
#include <system_error>

enum class PinnedClosureReviewStage {
    Input,
    ContentRead,
    Classification,
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
    ReadFailure,
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
    std::optional<PinnedClosureFailure> read_failure;
    std::optional<ConfirmationCancellationReason> cancellation;
    std::optional<std::error_code> io_error;
    PinnedClosureCleanupResult cleanup;
};

class AcceptedPinnedSubmoduleClosure;
using PinnedClosureReviewResult = std::variant<AcceptedPinnedSubmoduleClosure, PinnedClosureReviewFailure>;

// A separate closure acceptance, not recipe acceptance or a source-ready/S4
// proof. The whole 4A owner (including selection and object backing) survives.
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
    AcceptedPinnedSubmoduleClosure(InvocationOwnedPinnedSubmoduleClosure closure,
                                   ExplicitConfirmationAcceptance confirmation) noexcept;
    friend PinnedClosureReviewResult review_pinned_submodule_closure(
        InvocationOwnedPinnedSubmoduleClosure closure, ReviewPolicy diff_policy, bool no_confirm);
    InvocationOwnedPinnedSubmoduleClosure closure_;
    ExplicitConfirmationAcceptance confirmation_;
};

// The only mint obtains its own no-default confirmation after complete output.
// A migration/recipe token, raw metadata or an external "accepted" flag cannot
// substitute for this session. No object reads occur after the human prompt.
[[nodiscard]] PinnedClosureReviewResult review_pinned_submodule_closure(
    InvocationOwnedPinnedSubmoduleClosure closure,
    ReviewPolicy diff_policy = ReviewPolicy::Prompt, bool no_confirm = false);

#ifdef MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
#include <functional>
struct PinnedClosureReviewTestHooks {
    std::istream* input = nullptr;
    std::ostream* output = nullptr;
    std::optional<bool> interactive;
    std::optional<std::size_t> entries, blob_bytes, aggregate_bytes, line_bytes, rendered_bytes;
    std::function<void()> before_render;
};
void set_pinned_closure_review_test_hooks(PinnedClosureReviewTestHooks hooks);
#endif
