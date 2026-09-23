#include "selection_expression.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void expect_indices(
    std::string_view input,
    std::size_t candidate_count,
    std::vector<std::size_t> expected) {
    const SelectionExpressionParseResult parsed =
        parse_selection_expression(input, candidate_count);
    expect(parsed.issues.empty(), std::string(input) + ": parse failed");
    const NormalizedSelectionExpressionResult normalized =
        normalize_selection_expression(parsed.expression, candidate_count);
    const auto* indices = std::get_if<std::vector<std::size_t>>(&normalized);
    expect(indices != nullptr, std::string(input) + ": normalize failed");
    expect(*indices == expected, std::string(input) + ": indices differ");
}

void expect_invalid(
    std::string_view input,
    std::size_t candidate_count,
    SelectionExpressionIssueKind expected) {
    const SelectionExpressionParseResult parsed =
        parse_selection_expression(input, candidate_count);
    expect(!parsed.issues.empty(), std::string(input) + ": expected invalid");
    expect(
        parsed.issues.front().kind == expected,
        std::string(input) + ": issue kind differs");
}

void expect_empty_result(
    std::string_view input, std::size_t candidate_count) {
    const SelectionExpressionParseResult parsed =
        parse_selection_expression(input, candidate_count);
    expect(parsed.issues.empty(), std::string(input) + ": parse failed");
    const NormalizedSelectionExpressionResult normalized =
        normalize_selection_expression(parsed.expression, candidate_count);
    const auto* issue = std::get_if<SelectionExpressionIssue>(&normalized);
    expect(
        issue != nullptr &&
            issue->kind ==
                SelectionExpressionIssueKind::EmptyResultAfterExclusion,
        std::string(input) + ": empty result kind differs");
}

void test_valid_syntax_and_canonicalization() {
    const std::vector<std::pair<std::string_view, std::vector<std::size_t>>>
        cases{
            {"1", {1}},
            {"2", {2}},
            {"1 3", {1, 3}},
            {"1,3", {1, 3}},
            {"1-3", {1, 2, 3}},
            {"1-2,4", {1, 2, 4}},
            {"1-3 5", {1, 2, 3, 5}},
            {"1-3,5", {1, 2, 3, 5}},
            {"1 3-5", {1, 3, 4, 5}},
            {"1 2,4-6", {1, 2, 4, 5, 6}},
            {"^4", {1, 2, 3, 5, 6}},
            {"^2-4", {1, 5, 6}},
            {"1-5,^3", {1, 2, 4, 5}},
            {"1-3 5 ^2", {1, 3, 5}},
            {"3,1,2", {1, 2, 3}},
            {"1,1,2", {1, 2}},
            {"1 2 1", {1, 2}},
            {"1-2,2", {1, 2}},
            {"1-5 ^3", {1, 2, 4, 5}},
            {"^3 1-5", {1, 2, 4, 5}},
            {"1-3 ^2", {1, 3}},
            {"^2 1-3", {1, 3}},
            {"1, 2", {1, 2}},
            {"1 , 2", {1, 2}},
            {"\t1\v2,4-6\f", {1, 2, 4, 5, 6}},
        };
    for(const auto& [input, expected] : cases) {
        expect_indices(input, 6, expected);
    }
    expect_indices("^4", 5, {1, 2, 3, 5});
    expect_indices("^2-4", 5, {1, 5});
}

void test_invalid_and_empty_result() {
    using Kind = SelectionExpressionIssueKind;
    for(std::string_view input :
        {"0", "7", "^7", "9999999999999999999999999999"}) {
        expect_invalid(input, 6, Kind::IndexOutOfRange);
    }
    for(std::string_view input : {"3-1", "^4-2"}) {
        expect_invalid(input, 6, Kind::DescendingRange);
    }
    for(std::string_view input :
        {"1-", "-3", "^", "^^3", "^ 3", "foo", "1 - 3",
         "1- 3", "1 -3", "1,@group", "^@group"}) {
        expect_invalid(input, 6, Kind::MalformedToken);
    }
    for(std::string_view input : {"1,,3", ",1", "1,", "1, ,2"}) {
        expect_invalid(input, 6, Kind::EmptyCommaField);
    }
    expect_invalid("^6", 5, Kind::IndexOutOfRange);
    expect_empty_result("^1-5", 5);
    expect_empty_result("1,^1", 5);

    const SelectionExpressionParseResult blank =
        parse_selection_expression(" \t", 5);
    expect(blank.issues.empty(), "empty text had numeric parse issues");
    const NormalizedSelectionExpressionResult normalized_blank =
        normalize_selection_expression(blank.expression, 5);
    const auto* blank_issue =
        std::get_if<SelectionExpressionIssue>(&normalized_blank);
    expect(
        blank_issue != nullptr &&
            blank_issue->kind == Kind::MalformedToken,
        "empty numeric expression selected every candidate");
}

} // namespace

int main() {
    try {
        test_valid_syntax_and_canonicalization();
        test_invalid_and_empty_result();
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
