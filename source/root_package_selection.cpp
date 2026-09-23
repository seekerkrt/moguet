#include "root_package_selection.hpp"

#include "package_identifier.hpp"
#include "selection_expression.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <istream>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <unistd.h>
#include <utility>

namespace {

bool is_ascii_whitespace(char character) noexcept {
    switch(character) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            return true;
        default:
            return false;
    }
}

std::string_view trim_ascii_whitespace(std::string_view value) noexcept {
    while(!value.empty() && is_ascii_whitespace(value.front())) {
        value.remove_prefix(1);
    }
    while(!value.empty() && is_ascii_whitespace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

std::string ascii_lower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for(unsigned char character : value) {
        if(character >= 'A' && character <= 'Z') {
            lowered.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
            lowered.push_back(static_cast<char>(character));
        }
    }
    return lowered;
}

bool is_cancel_token(std::string_view value) {
    const std::string lowered = ascii_lower(value);
    return lowered == "q" || lowered == "quit" || lowered == "cancel";
}

void select_group_members(
    std::string_view token,
    const RootPackageSearchSnapshot& snapshot,
    std::vector<std::size_t>& include_indices,
    std::vector<RootPackageSelectionIssue>& issues) {
    const std::string group_name(token.substr(1));
    if(!is_valid_package_name(group_name)) {
        issues.push_back(MalformedRootPackageSelectionToken{
            std::string(token)});
        return;
    }

    bool found = false;
    for(std::size_t index = 0; index < snapshot.candidates.size(); ++index) {
        if(snapshot.candidates[index].candidate.source_kind() !=
           RootPackageSourceKind::Repository) {
            continue;
        }
        const auto& group_names =
            snapshot.candidates[index].selectable_group_names;
        if(std::find(group_names.begin(), group_names.end(), group_name) !=
           group_names.end()) {
            include_indices.push_back(index + 1);
            found = true;
        }
    }
    if(!found) {
        issues.push_back(UnknownRootPackageSelectionGroup{
            group_name});
    }
}

RootPackageSelectionIssue project_numeric_issue(
    const SelectionExpressionIssue& issue,
    std::size_t candidate_count) {
    switch(issue.kind) {
        case SelectionExpressionIssueKind::MalformedToken:
            if(is_cancel_token(issue.token)) {
                return MixedRootPackageSelectionCancellationToken{issue.token};
            }
            return MalformedRootPackageSelectionToken{issue.token};
        case SelectionExpressionIssueKind::EmptyCommaField:
            return EmptyRootPackageSelectionCommaField{};
        case SelectionExpressionIssueKind::IndexOutOfRange:
            return RootPackageSelectionIndexOutOfRange{
                issue.token, candidate_count};
        case SelectionExpressionIssueKind::DescendingRange:
            return DescendingRootPackageSelectionRange{issue.token};
        case SelectionExpressionIssueKind::EmptyResultAfterExclusion:
            return EmptyRootPackageSelectionResult{};
    }
    throw std::logic_error("Unknown selection expression issue.");
}

struct SelectedPackageIdentitySet {
    std::string package_name;
    std::vector<RootPackageIdentity> identities;
};

} // namespace

RootPackageSelection::RootPackageSelection(
    std::vector<SelectedRootPackageTarget> targets) noexcept
    : targets_(std::move(targets)) {
}

const std::vector<SelectedRootPackageTarget>&
RootPackageSelection::targets() const noexcept {
    return targets_;
}

RootPackageSelectionExpressionResult parse_root_package_selection(
    std::string input,
    const RootPackageSearchSnapshot& snapshot) {
    const std::string_view trimmed = trim_ascii_whitespace(input);
    if(trimmed.empty()) {
        return CancelledRootPackageSelection{
            RootPackageSelectionCancellationReason::EmptyInput};
    }
    if(is_cancel_token(trimmed)) {
        return CancelledRootPackageSelection{
            RootPackageSelectionCancellationReason::CancelToken};
    }

    // Keep group names in the root adapter. Mask each standalone group token
    // at its original offset so numeric commas still validate on both sides.
    std::string numeric_input = input;
    std::vector<std::pair<std::size_t, RootPackageSelectionIssue>> ordered_issues;
    std::vector<std::size_t> group_indices;
    for(std::size_t offset = 0; offset < input.size();) {
        if(is_ascii_whitespace(input[offset])) {
            ++offset;
            continue;
        }
        const std::size_t begin = offset;
        while(offset < input.size() && !is_ascii_whitespace(input[offset])) {
            ++offset;
        }
        const std::string_view token(input.data() + begin, offset - begin);
        if(token.front() != '@') continue;

        std::vector<RootPackageSelectionIssue> group_issues;
        select_group_members(
            token, snapshot, group_indices, group_issues);
        for(RootPackageSelectionIssue& issue : group_issues) {
            ordered_issues.emplace_back(begin, std::move(issue));
        }
        std::fill(
            numeric_input.begin() + static_cast<std::ptrdiff_t>(begin),
            numeric_input.begin() + static_cast<std::ptrdiff_t>(offset), ' ');
    }

    SelectionExpressionParseResult parsed = parse_selection_expression(
        numeric_input, snapshot.candidates.size());
    for(const SelectionExpressionIssue& issue : parsed.issues) {
        ordered_issues.emplace_back(
            issue.offset,
            project_numeric_issue(issue, snapshot.candidates.size()));
    }
    std::stable_sort(
        ordered_issues.begin(), ordered_issues.end(),
        [](const auto& left, const auto& right) {
            return left.first < right.first;
        });

    std::vector<RootPackageSelectionIssue> issues;
    for(auto& [offset, issue] : ordered_issues) {
        (void)offset;
        issues.push_back(std::move(issue));
    }
    if(!issues.empty()) {
        return InvalidRootPackageSelection{std::move(issues)};
    }

    parsed.expression.include_indices.insert(
        parsed.expression.include_indices.end(),
        group_indices.begin(), group_indices.end());
    NormalizedSelectionExpressionResult normalized =
        normalize_selection_expression(
            parsed.expression, snapshot.candidates.size());
    if(const auto* issue = std::get_if<SelectionExpressionIssue>(&normalized)) {
        return InvalidRootPackageSelection{{project_numeric_issue(*issue, snapshot.candidates.size())}};
    }

    std::vector<SelectedRootPackageTarget> selected_targets;
    std::vector<SelectedPackageIdentitySet> identities_by_package;
    for(std::size_t selected_index :
        std::get<std::vector<std::size_t>>(normalized)) {
        const RootPackageCandidate& candidate =
            snapshot.candidates[selected_index - 1].candidate;

        auto duplicate = std::find_if(
            selected_targets.begin(), selected_targets.end(),
            [&candidate](const SelectedRootPackageTarget& target) {
                return same_root_package_identity(
                    candidate.identity(), target.identity());
            });
        if(duplicate != selected_targets.end()) continue;

        auto package = std::find_if(
            identities_by_package.begin(), identities_by_package.end(),
            [&candidate](const SelectedPackageIdentitySet& entry) {
                return entry.package_name == candidate.package_name();
            });
        if(package == identities_by_package.end()) {
            identities_by_package.push_back(SelectedPackageIdentitySet{
                candidate.package_name(), {candidate.identity()}});
        } else {
            package->identities.push_back(candidate.identity());
        }
        selected_targets.push_back(select_root_package_target(candidate));
    }

    for(auto& package : identities_by_package) {
        if(package.identities.size() > 1) {
            issues.push_back(ConflictingRootPackageSelectionAlternatives{
                std::move(package.package_name),
                std::move(package.identities)});
        }
    }
    if(!issues.empty()) {
        return InvalidRootPackageSelection{std::move(issues)};
    }

    // non-cancel expressionは少なくとも1 selectorを持ち、valid selectorは
    // 1件以上のdisplayed candidateへ解決される。
    return RootPackageSelection(std::move(selected_targets));
}

RootPackageSelectionSession::RootPackageSelectionSession(
    std::istream& input,
    RootPackageSelectionInteractionCallback interaction,
    RootPackageSelectionInputGate input_gate)
    : input_(&input),
      interaction_(std::move(interaction)),
      input_gate_(input_gate) {
    switch(input_gate_) {
        case RootPackageSelectionInputGate::Interactive:
        case RootPackageSelectionInputGate::NonTty:
        case RootPackageSelectionInputGate::NoConfirm:
            break;
        default:
            throw std::invalid_argument(
                "Unknown root package selection input gate.");
    }
}

RootPackageSelectionSessionResult RootPackageSelectionSession::select(
    const RootPackageSearchSnapshot& snapshot) {
    if(input_gate_ == RootPackageSelectionInputGate::NoConfirm) {
        return UnavailableRootPackageSelection{
            RootPackageSelectionUnavailableReason::NoConfirm};
    }
    if(input_gate_ == RootPackageSelectionInputGate::NonTty) {
        return UnavailableRootPackageSelection{
            RootPackageSelectionUnavailableReason::NonInteractiveInput};
    }
    if(snapshot.candidates.empty()) {
        return UnavailableRootPackageSelection{
            RootPackageSelectionUnavailableReason::NoCandidates};
    }

    if(interaction_) {
        interaction_(
            PresentRootPackageSelectionCandidates{}, snapshot);
    }
    for(;;) {
        if(interaction_) {
            interaction_(PromptForRootPackageSelection{}, snapshot);
        }

        std::string input;
        if(!std::getline(*input_, input)) {
            return CancelledRootPackageSelection{
                RootPackageSelectionCancellationReason::EndOfInput};
        }

        RootPackageSelectionExpressionResult result =
            parse_root_package_selection(
                std::move(input), snapshot);
        if(auto* selection = std::get_if<RootPackageSelection>(&result);
           selection != nullptr) {
            return std::move(*selection);
        }
        if(auto* cancelled =
               std::get_if<CancelledRootPackageSelection>(&result);
           cancelled != nullptr) {
            return *cancelled;
        }

        if(interaction_) {
            interaction_(
                InvalidRootPackageSelectionAttempt{
                    std::move(std::get<InvalidRootPackageSelection>(
                        result))},
                snapshot);
        }
    }
}

bool RootPackageSelectionSession::is_interactive() const noexcept {
    return input_gate_ == RootPackageSelectionInputGate::Interactive;
}

RootPackageSelectionInputGate
RootPackageSelectionSession::input_gate() const noexcept {
    return input_gate_;
}

RootPackageSelectionSession make_root_package_selection_session(
    RootPackageSelectionInteractionCallback interaction,
    bool no_confirm) {
    const RootPackageSelectionInputGate input_gate =
        no_confirm
            ? RootPackageSelectionInputGate::NoConfirm
        : isatty(STDIN_FILENO) != 0
            ? RootPackageSelectionInputGate::Interactive
            : RootPackageSelectionInputGate::NonTty;
    return RootPackageSelectionSession(
        std::cin, std::move(interaction), input_gate);
}
