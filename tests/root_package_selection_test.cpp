#include "root_package_selection.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

static_assert(!std::is_default_constructible_v<RootPackageSelection>);
static_assert(!std::is_aggregate_v<RootPackageSelection>);
static_assert(std::is_copy_constructible_v<RootPackageSelection>);
static_assert(std::is_move_constructible_v<RootPackageSelection>);
static_assert(
    !std::is_constructible_v<
        RootPackageSelection,
        std::vector<SelectedRootPackageTarget>>);
static_assert(
    !std::is_copy_constructible_v<RootPackageSelectionSession>);
static_assert(std::is_move_constructible_v<RootPackageSelectionSession>);

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

RootPackageCandidate repository_candidate(
    std::string repository_name, std::string package_name) {
    RootPackageCandidateValidationResult result =
        make_repository_root_package_candidate(
            std::move(repository_name), std::move(package_name));
    expect(result.is_valid(), "repository test candidate was invalid");
    expect(result.candidate() != nullptr, "repository candidate is missing");
    return *result.candidate();
}

RootPackageCandidate aur_candidate(
    std::string package_name, std::string package_base) {
    RootPackageCandidateValidationResult result =
        make_aur_root_package_candidate(
            std::move(package_name), std::move(package_base));
    expect(result.is_valid(), "AUR test candidate was invalid");
    expect(result.candidate() != nullptr, "AUR candidate is missing");
    return *result.candidate();
}

RootPackageSearchCandidate repository_entry(
    std::string repository_name,
    std::string package_name,
    std::vector<std::string> selectable_group_names = {}) {
    return RootPackageSearchCandidate{
        repository_candidate(
            std::move(repository_name), std::move(package_name)),
        std::move(selectable_group_names)};
}

RootPackageSearchCandidate aur_entry(
    std::string package_name,
    std::string package_base,
    std::vector<std::string> selectable_group_names = {}) {
    return RootPackageSearchCandidate{
        aur_candidate(
            std::move(package_name), std::move(package_base)),
        std::move(selectable_group_names)};
}

RootPackageSearchSnapshot selection_snapshot() {
    return RootPackageSearchSnapshot{{
        repository_entry("core", "alpha", {"base-devel"}),
        repository_entry("extra", "beta"),
        aur_entry("gamma", "gamma-base"),
        repository_entry("core", "delta", {"base-devel"}),
        aur_entry("epsilon", "epsilon-base"),
    }};
}

const RootPackageSelection& require_expression_selection(
    const RootPackageSelectionExpressionResult& result,
    std::string_view context) {
    const auto* selection = std::get_if<RootPackageSelection>(&result);
    expect(
        selection != nullptr,
        std::string(context) + ": expected selection");
    return *selection;
}

const InvalidRootPackageSelection& require_invalid_expression(
    const RootPackageSelectionExpressionResult& result,
    std::string_view context) {
    const auto* invalid = std::get_if<InvalidRootPackageSelection>(&result);
    expect(
        invalid != nullptr,
        std::string(context) + ": expected invalid expression");
    return *invalid;
}

const CancelledRootPackageSelection& require_expression_cancellation(
    const RootPackageSelectionExpressionResult& result,
    std::string_view context) {
    const auto* cancellation =
        std::get_if<CancelledRootPackageSelection>(&result);
    expect(
        cancellation != nullptr,
        std::string(context) + ": expected cancellation");
    return *cancellation;
}

const RootPackageSelection& require_session_selection(
    const RootPackageSelectionSessionResult& result,
    std::string_view context) {
    const auto* selection = std::get_if<RootPackageSelection>(&result);
    expect(
        selection != nullptr,
        std::string(context) + ": expected session selection");
    return *selection;
}

const CancelledRootPackageSelection& require_session_cancellation(
    const RootPackageSelectionSessionResult& result,
    std::string_view context) {
    const auto* cancellation =
        std::get_if<CancelledRootPackageSelection>(&result);
    expect(
        cancellation != nullptr,
        std::string(context) + ": expected session cancellation");
    return *cancellation;
}

const UnavailableRootPackageSelection& require_unavailable_session(
    const RootPackageSelectionSessionResult& result,
    std::string_view context) {
    const auto* unavailable =
        std::get_if<UnavailableRootPackageSelection>(&result);
    expect(
        unavailable != nullptr,
        std::string(context) + ": expected unavailable session");
    return *unavailable;
}

void expect_selected_identities(
    const RootPackageSelection& selection,
    const std::vector<RootPackageIdentity>& expected,
    std::string_view context) {
    const std::string context_text(context);
    expect(
        selection.targets().size() == expected.size(),
        context_text + ": selected target count differs");
    for(std::size_t index = 0; index < expected.size(); ++index) {
        expect(
            selection.targets()[index].identity() == expected[index],
            context_text + ": selected identity differs at index " +
                std::to_string(index));
        expect(
            selection.targets()[index].target_role() ==
                RootPackageTargetRole::Root,
            context_text + ": selected target role differs at index " +
                std::to_string(index));
    }
}

void test_number_multiple_range_group_and_ascii_whitespace() {
    const RootPackageSearchSnapshot snapshot = selection_snapshot();

    const RootPackageSelectionExpressionResult number =
        parse_root_package_selection("2", snapshot);
    expect_selected_identities(
        require_expression_selection(number, "number selector"),
        {RepositoryRootPackageIdentity{"extra", "beta"}},
        "number selector");

    const RootPackageSelectionExpressionResult multiple =
        parse_root_package_selection("1 3 5", snapshot);
    expect_selected_identities(
        require_expression_selection(multiple, "multiple selector"),
        {
            RepositoryRootPackageIdentity{"core", "alpha"},
            AurRootPackageIdentity{"gamma", "gamma-base"},
            AurRootPackageIdentity{"epsilon", "epsilon-base"},
        },
        "multiple selector");

    const RootPackageSelectionExpressionResult range =
        parse_root_package_selection("2-4", snapshot);
    expect_selected_identities(
        require_expression_selection(range, "range selector"),
        {
            RepositoryRootPackageIdentity{"extra", "beta"},
            AurRootPackageIdentity{"gamma", "gamma-base"},
            RepositoryRootPackageIdentity{"core", "delta"},
        },
        "range selector");

    const RootPackageSelectionExpressionResult group =
        parse_root_package_selection("@base-devel", snapshot);
    expect_selected_identities(
        require_expression_selection(group, "group selector"),
        {
            RepositoryRootPackageIdentity{"core", "alpha"},
            RepositoryRootPackageIdentity{"core", "delta"},
        },
        "group selector");

    const RootPackageSelectionExpressionResult combined =
        parse_root_package_selection(
            " \t5\v3-4\f@base-devel\r2\n", snapshot);
    expect_selected_identities(
        require_expression_selection(
            combined, "combined ASCII-whitespace selector"),
        {
            RepositoryRootPackageIdentity{"core", "alpha"},
            RepositoryRootPackageIdentity{"extra", "beta"},
            AurRootPackageIdentity{"gamma", "gamma-base"},
            RepositoryRootPackageIdentity{"core", "delta"},
            AurRootPackageIdentity{"epsilon", "epsilon-base"},
        },
        "combined selector preserves display order");
}

void test_comma_and_exclusion_expressions() {
    const RootPackageSearchSnapshot snapshot = selection_snapshot();
    const auto expect_indices = [&](
                                    std::string_view input,
                                    const std::vector<std::size_t>& indices) {
        std::vector<RootPackageIdentity> expected;
        for(std::size_t index : indices) {
            expected.push_back(snapshot.candidates[index - 1]
                                   .candidate.identity());
        }
        const RootPackageSelectionExpressionResult result =
            parse_root_package_selection(std::string(input), snapshot);
        expect_selected_identities(
            require_expression_selection(result, input), expected, input);
    };

    expect_indices("1,3", {1, 3});
    expect_indices("1-2,4", {1, 2, 4});
    expect_indices("1-3 5", {1, 2, 3, 5});
    expect_indices("1-3,5", {1, 2, 3, 5});
    expect_indices("1 3-5", {1, 3, 4, 5});
    expect_indices("1 2,4-5", {1, 2, 4, 5});
    expect_indices("^4", {1, 2, 3, 5});
    expect_indices("^2-4", {1, 5});
    expect_indices("1-5,^3", {1, 2, 4, 5});
    expect_indices("@base-devel ^4", {1});
    expect_indices("^4 @base-devel", {1});
    expect_indices("1,1,2", {1, 2});
    expect_indices("3,1,2", {1, 2, 3});

    RootPackageSearchSnapshot six_candidates = selection_snapshot();
    six_candidates.candidates.push_back(
        repository_entry("extra", "zeta"));
    const RootPackageSelectionExpressionResult mixed =
        parse_root_package_selection("1 2,4-6", six_candidates);
    expect_selected_identities(
        require_expression_selection(mixed, "mixed six-candidate list"),
        {
            RepositoryRootPackageIdentity{"core", "alpha"},
            RepositoryRootPackageIdentity{"extra", "beta"},
            RepositoryRootPackageIdentity{"core", "delta"},
            AurRootPackageIdentity{"epsilon", "epsilon-base"},
            RepositoryRootPackageIdentity{"extra", "zeta"},
        },
        "mixed six-candidate list");

    for(std::string_view input : {"1,,3", ",1", "1,"}) {
        const RootPackageSelectionExpressionResult result =
            parse_root_package_selection(std::string(input), snapshot);
        const auto& invalid = require_invalid_expression(result, input);
        expect(
            std::holds_alternative<EmptyRootPackageSelectionCommaField>(
                invalid.issues.front()),
            std::string(input) + ": empty comma field issue differs");
    }
    for(std::string_view input : {"1,@base-devel", "^@base-devel"}) {
        const RootPackageSelectionExpressionResult result =
            parse_root_package_selection(std::string(input), snapshot);
        require_invalid_expression(result, input);
    }
    const RootPackageSelectionExpressionResult empty =
        parse_root_package_selection("1,^1", snapshot);
    expect(
        std::holds_alternative<EmptyRootPackageSelectionResult>(
            require_invalid_expression(empty, "empty result").issues.front()),
        "excluded empty selection was not typed invalid");
}

void test_overlapping_selectors_deduplicate_full_identity() {
    const RootPackageCandidate duplicate =
        repository_candidate("core", "duplicate");
    const RootPackageSearchSnapshot snapshot{{
        RootPackageSearchCandidate{duplicate, {"base-devel"}},
        RootPackageSearchCandidate{duplicate, {"base-devel"}},
        repository_entry("extra", "other", {"base-devel"}),
    }};

    const RootPackageSelectionExpressionResult result =
        parse_root_package_selection(
            "3 2 1 1-3 @base-devel 1", snapshot);
    expect_selected_identities(
        require_expression_selection(result, "overlapping selectors"),
        {
            RepositoryRootPackageIdentity{"core", "duplicate"},
            RepositoryRootPackageIdentity{"extra", "other"},
        },
        "overlapping selectors");
}

void test_group_selector_ignores_aur_group_metadata() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "official-member", {"base-devel"}),
        aur_entry("aur-package", "aur-base", {"base-devel"}),
    }};

    const RootPackageSelectionExpressionResult result =
        parse_root_package_selection("@base-devel", snapshot);
    expect_selected_identities(
        require_expression_selection(result, "repository-only group"),
        {RepositoryRootPackageIdentity{"core", "official-member"}},
        "repository-only group");
}

void test_alternative_source_conflict_is_atomic_and_ordered() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "shared"),
        repository_entry("extra", "shared"),
        aur_entry("shared", "shared-base"),
        repository_entry("core", "separate"),
    }};

    const RootPackageSelectionExpressionResult result =
        parse_root_package_selection("4 3 1 2", snapshot);
    const InvalidRootPackageSelection& invalid =
        require_invalid_expression(result, "alternative conflict");
    expect(invalid.issues.size() == 1, "alternative issue count differs");

    const auto* conflict =
        std::get_if<ConflictingRootPackageSelectionAlternatives>(
            &invalid.issues[0]);
    expect(conflict != nullptr, "alternative conflict kind differs");
    expect(conflict->package_name == "shared", "conflict package differs");
    expect(
        conflict->identities ==
            std::vector<RootPackageIdentity>{
                RepositoryRootPackageIdentity{"core", "shared"},
                RepositoryRootPackageIdentity{"extra", "shared"},
                AurRootPackageIdentity{"shared", "shared-base"},
            },
        "conflicting identities did not retain display order");
}

void test_exclusion_precedes_alternative_conflict() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "shared", {"both"}),
        aur_entry("shared", "shared-base"),
        repository_entry("core", "other", {"both"}),
    }};
    const RootPackageSelectionExpressionResult resolved =
        parse_root_package_selection("1-3 ^2", snapshot);
    expect_selected_identities(
        require_expression_selection(resolved, "excluded alternative"),
        {
            RepositoryRootPackageIdentity{"core", "shared"},
            RepositoryRootPackageIdentity{"core", "other"},
        },
        "excluded alternative");
    const RootPackageSelectionExpressionResult remains =
        parse_root_package_selection("1-3 ^3", snapshot);
    expect(
        std::holds_alternative<ConflictingRootPackageSelectionAlternatives>(
            require_invalid_expression(remains, "remaining alternative")
                .issues.front()),
        "remaining alternatives did not conflict");
    const RootPackageSelectionExpressionResult group =
        parse_root_package_selection("@both ^1", snapshot);
    expect_selected_identities(
        require_expression_selection(group, "group exclusion"),
        {RepositoryRootPackageIdentity{"core", "other"}},
        "group exclusion");
}

void test_invalid_tokens_are_typed_and_atomic() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "first"),
        repository_entry("core", "second"),
        repository_entry("core", "third"),
    }};

    const RootPackageSelectionExpressionResult result =
        parse_root_package_selection(
            "1 1,,2 1- @ 0 4 3-2 @missing CANCEL", snapshot);
    const InvalidRootPackageSelection& invalid =
        require_invalid_expression(result, "typed invalid selectors");
    const std::vector<RootPackageSelectionIssue> expected{
        EmptyRootPackageSelectionCommaField{},
        MalformedRootPackageSelectionToken{"1-"},
        MalformedRootPackageSelectionToken{"@"},
        RootPackageSelectionIndexOutOfRange{"0", 3},
        RootPackageSelectionIndexOutOfRange{"4", 3},
        DescendingRootPackageSelectionRange{"3-2"},
        UnknownRootPackageSelectionGroup{"missing"},
        MixedRootPackageSelectionCancellationToken{"CANCEL"},
    };
    expect(
        invalid.issues == expected,
        "typed invalid selector details or ordering differ");
}

void test_empty_and_cancel_tokens_are_distinct_cancellations() {
    const RootPackageSearchSnapshot snapshot = selection_snapshot();
    for(const std::string& input :
        std::vector<std::string>{"", " \t\n\r\f\v "}) {
        const RootPackageSelectionExpressionResult result =
            parse_root_package_selection(input, snapshot);
        expect(
            require_expression_cancellation(result, "empty input").reason ==
                RootPackageSelectionCancellationReason::EmptyInput,
            "ASCII-whitespace-only input was not empty cancellation");
    }

    for(const std::string& input :
        std::vector<std::string>{"q", " QUIT ", "\tCaNcEl\v"}) {
        const RootPackageSelectionExpressionResult result =
            parse_root_package_selection(input, snapshot);
        expect(
            require_expression_cancellation(result, "cancel token").reason ==
                RootPackageSelectionCancellationReason::CancelToken,
            "case-insensitive cancel token reason differs");
    }
}

void expect_gate_does_not_read_or_emit(
    RootPackageSelectionInputGate gate,
    const RootPackageSearchSnapshot& snapshot,
    RootPackageSelectionUnavailableReason expected_reason,
    std::string_view context) {
    std::istringstream input("1\n");
    std::size_t event_count = 0;
    RootPackageSelectionSession session(
        input,
        [&event_count](
            const RootPackageSelectionInteractionEvent&,
            const RootPackageSearchSnapshot&) { ++event_count; },
        gate);

    const RootPackageSelectionSessionResult result = session.select(snapshot);
    expect(
        require_unavailable_session(result, context).reason ==
            expected_reason,
        std::string(context) + ": unavailable reason differs");
    expect(event_count == 0, std::string(context) + ": emitted an event");

    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "1",
        std::string(context) + ": consumed selection input");
}

void test_input_gates_and_empty_snapshot_do_not_read_or_emit() {
    const RootPackageSearchSnapshot snapshot = selection_snapshot();
    expect_gate_does_not_read_or_emit(
        RootPackageSelectionInputGate::NonTty, snapshot,
        RootPackageSelectionUnavailableReason::NonInteractiveInput,
        "non-TTY gate");
    expect_gate_does_not_read_or_emit(
        RootPackageSelectionInputGate::NoConfirm, snapshot,
        RootPackageSelectionUnavailableReason::NoConfirm,
        "no-confirm gate");
    expect_gate_does_not_read_or_emit(
        RootPackageSelectionInputGate::Interactive,
        RootPackageSearchSnapshot{},
        RootPackageSelectionUnavailableReason::NoCandidates,
        "empty candidate gate");
}

void test_invalid_input_gate_is_rejected_before_read_or_emit() {
    std::istringstream input("1\n");
    std::size_t event_count = 0;
    bool rejected = false;
    try {
        RootPackageSelectionSession session(
            input,
            [&event_count](
                const RootPackageSelectionInteractionEvent&,
                const RootPackageSearchSnapshot&) {
                ++event_count;
            },
            static_cast<RootPackageSelectionInputGate>(42));
    } catch(const std::invalid_argument&) {
        rejected = true;
    }

    expect(rejected, "invalid input gate was accepted");
    expect(event_count == 0, "invalid input gate emitted an event");
    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "1",
        "invalid input gate consumed selection input");
}

void test_single_candidate_has_no_default() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "only-candidate"),
    }};
    std::istringstream input("\n1\n");
    std::vector<RootPackageSelectionInteractionEvent> events;
    RootPackageSelectionSession session(
        input,
        [&events](
            const RootPackageSelectionInteractionEvent& event,
            const RootPackageSearchSnapshot&) {
            events.push_back(event);
        },
        RootPackageSelectionInputGate::Interactive);

    const RootPackageSelectionSessionResult result = session.select(snapshot);
    expect(
        require_session_cancellation(result, "single candidate").reason ==
            RootPackageSelectionCancellationReason::EmptyInput,
        "empty input selected the single candidate by default");
    expect(
        events.size() == 2 &&
            std::holds_alternative<
                PresentRootPackageSelectionCandidates>(events[0]) &&
            std::holds_alternative<PromptForRootPackageSelection>(
                events[1]),
        "single candidate interaction event order differs");

    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "1",
        "single-candidate cancellation consumed a later choice");
}

void test_invalid_attempt_retries_with_ordered_events() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "first"),
        aur_entry("second", "second-base"),
    }};
    std::istringstream input("1,,2\n2\nremaining\n");
    std::vector<RootPackageSelectionInteractionEvent> events;
    bool callbacks_received_same_snapshot = true;
    RootPackageSelectionSession session(
        input,
        [&events, &snapshot, &callbacks_received_same_snapshot](
            const RootPackageSelectionInteractionEvent& event,
            const RootPackageSearchSnapshot& callback_snapshot) {
            callbacks_received_same_snapshot =
                callbacks_received_same_snapshot &&
                &callback_snapshot == &snapshot;
            events.push_back(event);
        },
        RootPackageSelectionInputGate::Interactive);

    const RootPackageSelectionSessionResult result = session.select(snapshot);
    expect_selected_identities(
        require_session_selection(result, "retry selection"),
        {AurRootPackageIdentity{"second", "second-base"}},
        "retry selection");
    expect(callbacks_received_same_snapshot, "retry changed candidate snapshot");
    expect(events.size() == 4, "retry interaction event count differs");
    expect(
        std::holds_alternative<PresentRootPackageSelectionCandidates>(
            events[0]) &&
            std::holds_alternative<PromptForRootPackageSelection>(
                events[1]) &&
            std::holds_alternative<InvalidRootPackageSelectionAttempt>(
                events[2]) &&
            std::holds_alternative<PromptForRootPackageSelection>(
                events[3]),
        "retry interaction event order differs");

    const auto& invalid =
        std::get<InvalidRootPackageSelectionAttempt>(events[2]).selection;
    expect(
        invalid.issues ==
            std::vector<RootPackageSelectionIssue>{
                EmptyRootPackageSelectionCommaField{}},
        "retry invalid event detail differs");

    std::string unread_input;
    expect(
        static_cast<bool>(std::getline(input, unread_input)) &&
            unread_input == "remaining",
        "successful retry consumed a later input line");
}

void test_eof_cancels_after_present_and_prompt() {
    const RootPackageSearchSnapshot snapshot{{
        repository_entry("core", "first"),
    }};
    std::istringstream input;
    std::vector<RootPackageSelectionInteractionEvent> events;
    RootPackageSelectionSession session(
        input,
        [&events](
            const RootPackageSelectionInteractionEvent& event,
            const RootPackageSearchSnapshot&) {
            events.push_back(event);
        },
        RootPackageSelectionInputGate::Interactive);

    const RootPackageSelectionSessionResult result = session.select(snapshot);
    expect(
        require_session_cancellation(result, "EOF").reason ==
            RootPackageSelectionCancellationReason::EndOfInput,
        "EOF cancellation reason differs");
    expect(
        events.size() == 2 &&
            std::holds_alternative<
                PresentRootPackageSelectionCandidates>(events[0]) &&
            std::holds_alternative<PromptForRootPackageSelection>(
                events[1]),
        "EOF interaction event order differs");
}

void test_empty_callback_and_selection_result_own_values() {
    const RootPackageSelectionSessionResult result = [] {
        std::istringstream input("1\n");
        RootPackageSelectionSession session(
            input, {}, RootPackageSelectionInputGate::Interactive);
        const RootPackageSearchSnapshot snapshot{{
            aur_entry("owned-child", "owned-base"),
        }};
        return session.select(snapshot);
    }();

    expect_selected_identities(
        require_session_selection(result, "owned session result"),
        {AurRootPackageIdentity{"owned-child", "owned-base"}},
        "owned session result");
}

class NonTtyStdinGuard final {
public:
    NonTtyStdinGuard() {
        int pipe_fds[2];
        if(pipe(pipe_fds) != 0) {
            throw std::runtime_error("failed to create non-TTY stdin pipe");
        }

        saved_stdin_ = dup(STDIN_FILENO);
        if(saved_stdin_ < 0) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            throw std::runtime_error("failed to duplicate stdin");
        }
        if(dup2(pipe_fds[0], STDIN_FILENO) < 0) {
            close(saved_stdin_);
            saved_stdin_ = -1;
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            throw std::runtime_error("failed to install non-TTY stdin");
        }

        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }

    NonTtyStdinGuard(const NonTtyStdinGuard&) = delete;
    NonTtyStdinGuard& operator=(const NonTtyStdinGuard&) = delete;

    ~NonTtyStdinGuard() {
        if(saved_stdin_ < 0) return;
        static_cast<void>(dup2(saved_stdin_, STDIN_FILENO));
        close(saved_stdin_);
    }

private:
    int saved_stdin_ = -1;
};

class TtyStdinGuard final {
public:
    TtyStdinGuard() {
        master_fd_ = posix_openpt(O_RDWR | O_NOCTTY);
        if(master_fd_ < 0 || grantpt(master_fd_) != 0 ||
           unlockpt(master_fd_) != 0) {
            if(master_fd_ >= 0) close(master_fd_);
            master_fd_ = -1;
            throw std::runtime_error("failed to create TTY stdin fixture");
        }

        const char* slave_name = ptsname(master_fd_);
        if(slave_name == nullptr) {
            close(master_fd_);
            master_fd_ = -1;
            throw std::runtime_error("failed to resolve TTY stdin fixture");
        }
        const int slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
        if(slave_fd < 0) {
            close(master_fd_);
            master_fd_ = -1;
            throw std::runtime_error("failed to open TTY stdin fixture");
        }

        saved_stdin_ = dup(STDIN_FILENO);
        if(saved_stdin_ < 0 || dup2(slave_fd, STDIN_FILENO) < 0) {
            if(saved_stdin_ >= 0) close(saved_stdin_);
            saved_stdin_ = -1;
            close(slave_fd);
            close(master_fd_);
            master_fd_ = -1;
            throw std::runtime_error("failed to install TTY stdin fixture");
        }
        close(slave_fd);
    }

    TtyStdinGuard(const TtyStdinGuard&) = delete;
    TtyStdinGuard& operator=(const TtyStdinGuard&) = delete;

    ~TtyStdinGuard() {
        if(saved_stdin_ >= 0) {
            static_cast<void>(dup2(saved_stdin_, STDIN_FILENO));
            close(saved_stdin_);
        }
        if(master_fd_ >= 0) close(master_fd_);
    }

private:
    int saved_stdin_ = -1;
    int master_fd_ = -1;
};

void test_production_factory_distinguishes_non_tty_and_no_confirm() {
    {
        const TtyStdinGuard guard;
        RootPackageSelectionSession session =
            make_root_package_selection_session({}, false);
        expect(
            session.input_gate() ==
                RootPackageSelectionInputGate::Interactive,
            "production factory did not detect TTY stdin");
        expect(
            session.is_interactive(),
            "TTY production session was not interactive");
    }

    {
        const NonTtyStdinGuard guard;
        RootPackageSelectionSession session =
            make_root_package_selection_session({}, false);
        expect(
            session.input_gate() == RootPackageSelectionInputGate::NonTty,
            "production factory did not detect non-TTY stdin");
        expect(
            !session.is_interactive(),
            "non-TTY production session remained interactive");
    }

    RootPackageSelectionSession session =
        make_root_package_selection_session({}, true);
    expect(
        session.input_gate() == RootPackageSelectionInputGate::NoConfirm,
        "production factory did not retain no-confirm gate");
    expect(
        !session.is_interactive(),
        "no-confirm production session remained interactive");
}

} // namespace

int main() {
    try {
        test_number_multiple_range_group_and_ascii_whitespace();
        test_comma_and_exclusion_expressions();
        test_overlapping_selectors_deduplicate_full_identity();
        test_group_selector_ignores_aur_group_metadata();
        test_alternative_source_conflict_is_atomic_and_ordered();
        test_exclusion_precedes_alternative_conflict();
        test_invalid_tokens_are_typed_and_atomic();
        test_empty_and_cancel_tokens_are_distinct_cancellations();
        test_input_gates_and_empty_snapshot_do_not_read_or_emit();
        test_invalid_input_gate_is_rejected_before_read_or_emit();
        test_single_candidate_has_no_default();
        test_invalid_attempt_retries_with_ordered_events();
        test_eof_cancels_after_present_and_prompt();
        test_empty_callback_and_selection_result_own_values();
        test_production_factory_distinguishes_non_tty_and_no_confirm();
        std::cout << "root package selection tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
