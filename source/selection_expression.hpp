#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Candidate indices are one-origin. The parser owns only numeric syntax;
// callers add domain-specific includes before normalization.
struct SelectionExpression {
    std::vector<std::size_t> include_indices;
    std::vector<std::size_t> exclude_indices;
};

enum class SelectionExpressionIssueKind {
    MalformedToken,
    EmptyCommaField,
    IndexOutOfRange,
    DescendingRange,
    EmptyResultAfterExclusion
};

struct SelectionExpressionIssue {
    SelectionExpressionIssueKind kind;
    std::string token;
    std::size_t offset = 0;
};

struct SelectionExpressionParseResult {
    SelectionExpression expression;
    std::vector<SelectionExpressionIssue> issues;
};

// Empty text yields an empty expression; cancellation belongs to the session.
SelectionExpressionParseResult parse_selection_expression(
    std::string_view input, std::size_t candidate_count);

using NormalizedSelectionExpressionResult = std::variant<
    std::vector<std::size_t>, SelectionExpressionIssue>;

// Excludes always win. With no includes, excludes apply to all candidates.
NormalizedSelectionExpressionResult normalize_selection_expression(
    const SelectionExpression& expression, std::size_t candidate_count);
