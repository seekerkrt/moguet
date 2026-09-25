#include "pinned_submodule_closure_review.hpp"

#include "localization.hpp"
#include "reviewed_source_presentation.hpp"
#include "terminal_safe_text.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <unistd.h>

namespace {
using Stage = PinnedClosureReviewStage;
using Reason = PinnedClosureReviewFailureReason;

#ifdef MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
PinnedClosureReviewTestHooks g_hooks;
#endif

struct Limits {
    std::size_t entries = PinnedClosureLimits{}.tree_records;
    std::size_t rendered = REVIEWED_SOURCE_RENDERED_OUTPUT_LIMIT;
};

struct ReviewStopped {
    PinnedClosureReviewFailure failure;
};

struct ReviewBody {
    Limits limits;
    Stage stage = Stage::Input;
    std::optional<std::size_t> node, entry;
    std::string text;

    [[noreturn]] void stop(Reason reason) const {
        throw ReviewStopped{{stage, reason, node, entry, {}, {}, {}}};
    }
    void append(std::string_view bytes) {
        if(bytes.size() > limits.rendered - text.size()) stop(Reason::ResourceLimitExceeded);
        text.append(bytes);
    }
    void escaped(std::string_view bytes) {
        const bool complete = terminal_safe_text::append_escaped_utf8(bytes, [&](std::string_view segment) {
            append(segment);
            return true;
        });
        if(!complete) stop(Reason::RenderFailure);
    }
    void field(std::string_view label, std::string_view value) {
        append(label);
        escaped(value);
        append("\n");
    }
};

// The live 4A owner already proves object hashes, connectivity and complete
// inventories. Acceptance selects that exact build input; it does not attest
// to source-code safety. Keep recipe content review in its separate owner.
void render_identity(const InvocationOwnedPinnedSubmoduleClosure& closure, PresentationDetail presentation_detail, ReviewBody& body) {
    body.stage = Stage::Presentation;
#ifdef MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
    if(g_hooks.before_render) g_hooks.before_render();
#endif
    body.append(localization::translate_message("Exact upstream source snapshot (separate from recipe review)\n"));
    body.append(localization::translate_message("Acceptance authorizes this snapshot as build input; it does not certify source-code safety. Upstream blob contents are not displayed.\n"));
    const auto& source = closure.selection().git_source();
    // NO_TRANSLATE: Stable technical inventory field labels; all values are escaped.
    body.field("remote: ", source.source_location());
    body.field("selector: ", source.selector().kind() == VcsSelectorKind::DefaultHead ? "HEAD" : "refs/heads/" + *source.selector().value());
    if(presentation_detail == PresentationDetail::Detailed)
        body.field("root X: ", closure.nodes().front().commit.value());
    else {
        body.field("root commit: ", closure.nodes().front().commit.value());
        body.field("root tree: ", closure.nodes().front().tree.value());
    }
    body.field("root tag count: ", std::to_string(closure.root_tags().size()));
    if(presentation_detail == PresentationDetail::Detailed) {
        for(const auto& tag : closure.root_tags()) {
            body.field("root tag: ", tag.ref_name());
            body.field("raw object: ", tag.raw().value());
            if(tag.peeled()) body.field("peeled object: ", tag.peeled()->value());
        }
    }
    std::size_t entries = 0, submodules = 0;
    std::uintmax_t total_bytes = 0;
    bool total_bytes_overflow = false;
    std::vector<std::string> paths(closure.nodes().size());
    for(std::size_t node = 0; node < closure.nodes().size(); ++node) {
        body.node = node;
        const auto& value = closure.nodes()[node];
        if(value.parent_edge) {
            const auto& edge = closure.edges().at(*value.parent_edge);
            paths[node] = paths.at(edge.parent).empty() ? edge.path : paths[edge.parent] + "/" + edge.path;
            if(presentation_detail == PresentationDetail::Detailed) {
                body.field("parent edge: ", std::to_string(*value.parent_edge));
                body.field("parent node: ", std::to_string(edge.parent));
                body.field("logical name: ", edge.logical_name);
                body.field("parent-relative path: ", edge.path);
                body.field("locator (transport only): ", edge.locator);
                body.field("parent gitlink pin: ", edge.pin.value());
            }
        }
        if(presentation_detail == PresentationDetail::Detailed) {
            body.field("node: ", std::to_string(node));
            body.field("closure path: ", node == 0 ? "." : paths[node]);
            body.field("commit: ", value.commit.value());
            body.field("object format: ", value.commit.format() == GitObjectFormat::Sha1 ? "sha1" : "sha256");
            body.field("tree: ", value.tree.value());
            body.field("locator: ", value.locator);
        }
        for(std::size_t entry = 0; entry < value.inventory.entries.size(); ++entry) {
            body.entry = entry;
            if(++entries > body.limits.entries) body.stop(Reason::ResourceLimitExceeded);
            const auto& file = value.inventory.entries[entry];
            // The existing workspace only supports regular/executable content
            // and proven Gitlinks. Binary regular blobs share that same mode.
            if(file.mode() != ReviewedSourceFileMode::Gitlink &&
               file.mode() != ReviewedSourceFileMode::Regular && file.mode() != ReviewedSourceFileMode::Executable)
                body.stop(Reason::UnsupportedContent);
            if(presentation_detail == PresentationDetail::Detailed) {
                body.field("file: ", file.path().raw_bytes());
                body.field("mode: ", file.mode() == ReviewedSourceFileMode::Gitlink ? "160000" : file.mode() == ReviewedSourceFileMode::Executable ? "100755"
                                                                                                                                                   : "100644");
                body.field("object: ", file.object_id().value());
            }
            if(file.mode() == ReviewedSourceFileMode::Gitlink) {
                ++submodules;
                const auto edge = std::find_if(closure.edges().begin(), closure.edges().end(), [&](const auto& candidate) {
                    return candidate.parent == node && candidate.path == file.path().raw_bytes();
                });
                if(edge == closure.edges().end() || edge->pin != file.object_id() ||
                   closure.nodes().at(edge->child).commit != edge->pin) body.stop(Reason::InvalidClosure);
                if(presentation_detail == PresentationDetail::Detailed) body.field("exact child node: ", std::to_string(edge->child));
            } else {
                if(!file.blob_size()) body.stop(Reason::InvalidClosure);
                if(*file.blob_size() > std::numeric_limits<std::uintmax_t>::max() - total_bytes)
                    total_bytes_overflow = true;
                else if(!total_bytes_overflow)
                    total_bytes += *file.blob_size();
                if(presentation_detail == PresentationDetail::Detailed) body.field("bytes: ", std::to_string(*file.blob_size()));
            }
        }
    }
    body.node.reset();
    body.entry.reset();
    if(presentation_detail == PresentationDetail::Normal) {
        body.field("closure nodes: ", std::to_string(closure.nodes().size()));
        body.field("files: ", std::to_string(entries));
        body.field("total bytes: ", total_bytes_overflow ? "> " + std::to_string(std::numeric_limits<std::uintmax_t>::max()) : std::to_string(total_bytes));
        body.field("submodules: ", std::to_string(submodules));
    }
}

ExplicitConfirmationResult present_and_confirm(ReviewBody& body, std::istream& input, std::ostream& output) {
    body.stage = Stage::Output;
    if(!output) body.stop(Reason::OutputFailure);
    // A checked facade leaves the caller's exception policy unchanged. Tying
    // input to it flushes the complete question before getline can consume Yes.
    std::ostream checked(output.rdbuf());
    checked.exceptions(std::ios::badbit | std::ios::failbit);
    struct InputTie {
        std::istream& input;
        std::ostream* previous;
        ~InputTie() {
            input.tie(previous);
        }
    } tie{input, input.tie(&checked)};
    try {
        checked.write(body.text.data(), static_cast<std::streamsize>(body.text.size()));
        checked.flush();
        // Complete, flushed presentation is the private Presented state.
        body.stage = Stage::Confirmation;
        auto confirmation = request_explicit_confirmation(
            localization::translate_message("Use this exact upstream source snapshot as build input?"),
            false, true, input, checked);
        checked.flush();
        return confirmation;
    } catch(const std::ios_base::failure& error) {
        if(checked && body.stage == Stage::Confirmation && input.eof() && !input.bad())
            return ConfirmationCancelled{ConfirmationCancellationReason::EndOfInput};
        PinnedClosureReviewFailure failure{body.stage,
                                           !checked || body.stage == Stage::Output ? Reason::OutputFailure : Reason::InputFailure,
                                           {},
                                           {},
                                           {},
                                           error.code(),
                                           {}};
        throw ReviewStopped{std::move(failure)};
    } catch(const std::bad_alloc&) {
        throw;
    } catch(...) {
        if(checked && body.stage == Stage::Confirmation && !input)
            return ConfirmationInputFailure{};
        body.stop(Reason::OutputFailure);
    }
}
} // namespace

AcceptedPinnedSubmoduleClosure::AcceptedPinnedSubmoduleClosure(
    InvocationOwnedPinnedSubmoduleClosure closure, ExplicitConfirmationAcceptance confirmation) noexcept
    : closure_(std::move(closure)), confirmation_(std::move(confirmation)) {
}
AcceptedPinnedSubmoduleClosure::AcceptedPinnedSubmoduleClosure(AcceptedPinnedSubmoduleClosure&&) noexcept = default;
AcceptedPinnedSubmoduleClosure::~AcceptedPinnedSubmoduleClosure() noexcept = default;
bool AcceptedPinnedSubmoduleClosure::valid() const noexcept {
    return closure_.valid() && confirmation_.valid();
}
const InvocationOwnedPinnedSubmoduleClosure& AcceptedPinnedSubmoduleClosure::closure() const {
    if(!valid()) throw std::logic_error("Accepted pinned closure is inactive");
    return closure_;
}
PinnedClosureCleanupResult AcceptedPinnedSubmoduleClosure::cleanup() noexcept {
    return closure_.cleanup();
}

PinnedClosureReviewResult review_pinned_submodule_closure(
    InvocationOwnedPinnedSubmoduleClosure closure, PresentationDetail presentation_detail,
    ReviewPolicy diff_policy, bool no_confirm) {
    ReviewBody body;
    std::istream* input = &std::cin;
    std::ostream* output = &std::cout;
    bool interactive = ::isatty(STDIN_FILENO) == 1;
#ifdef MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
    if(g_hooks.input) input = g_hooks.input;
    if(g_hooks.output) output = g_hooks.output;
    if(g_hooks.interactive) interactive = *g_hooks.interactive;
    const auto limit = [](std::size_t& value, const auto& override_value) {
        if(override_value) value = std::min(value, *override_value);
    };
    limit(body.limits.entries, g_hooks.entries);
    limit(body.limits.rendered, g_hooks.rendered_bytes);
#endif
    PinnedClosureReviewFailure failure{Stage::Input, Reason::InvalidClosure, {}, {}, {}, {}, {}};
    try {
        if(!closure.valid()) body.stop(Reason::InvalidClosure);
        if(diff_policy != ReviewPolicy::Prompt) body.stop(Reason::ReviewSkipped);
        if(no_confirm) body.stop(Reason::NoConfirm);
        if(!interactive) body.stop(Reason::NonInteractiveInput);
        render_identity(closure, presentation_detail, body);
        // The token is obtained here, never accepted from a caller. Complete
        // selected presentation precedes the prompt; no upstream blobs are read.
        auto confirmation = present_and_confirm(body, *input, *output);
        if(auto* accepted = std::get_if<ExplicitConfirmationAcceptance>(&confirmation)) {
            if(!accepted->valid()) body.stop(Reason::InvalidClosure);
            return AcceptedPinnedSubmoduleClosure(std::move(closure), std::move(*accepted));
        }
        if(const auto* cancelled = std::get_if<ConfirmationCancelled>(&confirmation)) {
            failure = {Stage::Confirmation, Reason::Cancelled, {}, {}, cancelled->reason, {}, {}};
        } else if(std::holds_alternative<ConfirmationDeclined>(confirmation)) {
            body.stop(Reason::Declined);
        } else {
            body.stop(Reason::InputFailure);
        }
    } catch(ReviewStopped& stopped) {
        failure = std::move(stopped.failure);
    } catch(const std::bad_alloc&) {
        failure = {body.stage, Reason::ResourceLimitExceeded, body.node, body.entry, {}, {}, {}};
    } catch(const std::ios_base::failure& error) {
        failure = {body.stage, body.stage == Stage::Confirmation && !*input && *output ? Reason::InputFailure : Reason::OutputFailure, body.node, body.entry, {}, error.code(), {}};
    } catch(...) {
        failure = {body.stage, body.stage == Stage::Output || body.stage == Stage::Confirmation ? Reason::OutputFailure : Reason::RenderFailure, body.node, body.entry, {}, {}, {}};
    }
    if(closure.valid()) failure.cleanup = closure.cleanup();
    return failure;
}

#ifdef MOGUET_ENABLE_PINNED_CLOSURE_REVIEW_TEST_HOOKS
void set_pinned_closure_review_test_hooks(PinnedClosureReviewTestHooks hooks) {
    g_hooks = std::move(hooks);
}
#endif
