#include "selection_expression.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

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

enum class IndexStatus { Valid,
                         Malformed,
                         OutOfRange };

struct IndexResult {
    IndexStatus status;
    std::size_t value = 0;
};

IndexResult parse_index(std::string_view text, std::size_t candidate_count) {
    if(text.empty()) return {IndexStatus::Malformed};

    std::size_t value = 0;
    const char* first = text.data();
    const auto [end, error] =
        std::from_chars(first, first + text.size(), value);
    if(error == std::errc::result_out_of_range) {
        return {IndexStatus::OutOfRange};
    }
    if(error != std::errc{} || end != first + text.size()) {
        return {IndexStatus::Malformed};
    }
    if(value == 0 || value > candidate_count) {
        return {IndexStatus::OutOfRange};
    }
    return {IndexStatus::Valid, value};
}

void parse_atom(
    std::string_view token,
    std::size_t offset,
    std::size_t candidate_count,
    SelectionExpressionParseResult& result) {
    const std::string_view original = token;
    const bool is_exclusion = token.front() == '^';
    if(is_exclusion) token.remove_prefix(1);

    const std::size_t separator = token.find('-');
    const bool is_range = separator != std::string_view::npos;
    const IndexResult first = parse_index(
        is_range ? token.substr(0, separator) : token, candidate_count);
    const IndexResult last = is_range
                                 ? parse_index(token.substr(separator + 1), candidate_count)
                                 : first;

    auto issue = [&](SelectionExpressionIssueKind kind) {
        result.issues.push_back({kind, std::string(original), offset});
    };
    if(first.status == IndexStatus::Malformed ||
       last.status == IndexStatus::Malformed) {
        issue(SelectionExpressionIssueKind::MalformedToken);
        return;
    }
    if(first.status == IndexStatus::OutOfRange ||
       last.status == IndexStatus::OutOfRange) {
        issue(SelectionExpressionIssueKind::IndexOutOfRange);
        return;
    }
    if(first.value > last.value) {
        issue(SelectionExpressionIssueKind::DescendingRange);
        return;
    }

    std::vector<std::size_t>& indices = is_exclusion
                                            ? result.expression.exclude_indices
                                            : result.expression.include_indices;
    for(std::size_t index = first.value; index < last.value; ++index) {
        indices.push_back(index);
    }
    indices.push_back(last.value);
}

} // namespace

SelectionExpressionParseResult parse_selection_expression(
    std::string_view input, std::size_t candidate_count) {
    SelectionExpressionParseResult result;
    std::size_t atom_begin = std::string_view::npos;
    std::size_t last_comma = std::string_view::npos;
    bool has_atom_since_comma = false;
    bool last_comma_was_empty = false;

    auto finish_atom = [&](std::size_t end) {
        if(atom_begin == std::string_view::npos) return;
        parse_atom(
            input.substr(atom_begin, end - atom_begin), atom_begin,
            candidate_count, result);
        atom_begin = std::string_view::npos;
        has_atom_since_comma = true;
    };

    for(std::size_t offset = 0; offset < input.size(); ++offset) {
        const char character = input[offset];
        if(character == ',' || is_ascii_whitespace(character)) {
            finish_atom(offset);
            if(character == ',') {
                last_comma_was_empty = !has_atom_since_comma;
                if(last_comma_was_empty) {
                    result.issues.push_back({SelectionExpressionIssueKind::EmptyCommaField,
                                             ",", offset});
                }
                has_atom_since_comma = false;
                last_comma = offset;
            }
        } else if(atom_begin == std::string_view::npos) {
            atom_begin = offset;
        }
    }
    finish_atom(input.size());
    if(last_comma != std::string_view::npos && !has_atom_since_comma &&
       !last_comma_was_empty) {
        result.issues.push_back({SelectionExpressionIssueKind::EmptyCommaField,
                                 ",", last_comma});
    }
    std::stable_sort(
        result.issues.begin(), result.issues.end(),
        [](const SelectionExpressionIssue& left,
           const SelectionExpressionIssue& right) {
            return left.offset < right.offset;
        });
    return result;
}

NormalizedSelectionExpressionResult normalize_selection_expression(
    const SelectionExpression& expression, std::size_t candidate_count) {
    if(expression.include_indices.empty() &&
       expression.exclude_indices.empty()) {
        return SelectionExpressionIssue{
            SelectionExpressionIssueKind::MalformedToken, "", 0};
    }
    const bool has_includes = !expression.include_indices.empty();
    std::vector<bool> included(candidate_count, !has_includes);
    std::vector<bool> excluded(candidate_count, false);
    for(std::size_t index : expression.include_indices) {
        if(index == 0 || index > candidate_count) {
            return SelectionExpressionIssue{
                SelectionExpressionIssueKind::IndexOutOfRange, "", 0};
        }
        included[index - 1] = true;
    }
    for(std::size_t index : expression.exclude_indices) {
        if(index == 0 || index > candidate_count) {
            return SelectionExpressionIssue{
                SelectionExpressionIssueKind::IndexOutOfRange, "", 0};
        }
        excluded[index - 1] = true;
    }

    std::vector<std::size_t> normalized;
    for(std::size_t index = 0; index < candidate_count; ++index) {
        if(included[index] && !excluded[index]) {
            normalized.push_back(index + 1);
        }
    }
    if(normalized.empty()) {
        return SelectionExpressionIssue{
            SelectionExpressionIssueKind::EmptyResultAfterExclusion, "", 0};
    }
    return normalized;
}
