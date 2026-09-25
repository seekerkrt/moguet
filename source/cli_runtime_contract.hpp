#pragma once

#include "cli_authority.hpp"
#include "cli_parser.hpp"
#include "cli_public_projection.hpp"
#include "diagnostic_model.hpp"

#include <optional>
#include <string>

enum class CliInvocationIssueKind {
    UnknownOperation,
    MisplacedLocalSourceOption,
    MisplacedSourcePreferenceOption,
    DuplicateSourcePreferenceOption,
    SourcePreferenceAssignmentConflict,
    MisplacedPkgbuildOutputDirectoryOption,
    SelectRequiresPlainSync,
    UnsupportedAutoSystemUpdateOption,
    UnsupportedAutoSystemUpdateArgumentForm,
    MissingOperand,
    ExtraOperand,
    InvalidOperandOrdering,
    InvalidEnvironmentAssignment,
    UnsupportedPresentationDetail,
};

struct CliInvocationIssue {
    CliInvocationIssueKind kind = CliInvocationIssueKind::UnknownOperation;
    std::string operation;
    std::optional<std::string> operand;
    cli_authority::TargetPolicy target_policy =
        cli_authority::TargetPolicy::None;
    cli_authority::OperandKind primary_operand_kind =
        cli_authority::OperandKind::None;

    bool operator==(const CliInvocationIssue&) const = default;
};

struct ResolvedCliRuntimeContract {
    const cli_authority::OperationMetadata* operation = nullptr;
    const cli_authority::OperationFormSpec* form = nullptr;
    const cli_authority::SpecialOperationSpec* special_operation = nullptr;
    cli_authority::GrammarOwnership owner =
        cli_authority::GrammarOwnership::MoguetOwned;

    bool is_known() const noexcept;
    bool is_delegated() const noexcept;
};

struct CliInvocationValidation {
    ResolvedCliRuntimeContract contract;
    std::optional<NormalizedDiagnostic<CliInvocationIssue>> diagnostic;

    bool is_valid() const noexcept;
};

// Slice 2のstructured metadataからruntimeで使用するformを選ぶ。filesystem、
// network、process stateには到達しない。
ResolvedCliRuntimeContract resolve_cli_runtime_contract(
    const ParsedCliArguments& parsed);

// Moguet-owned / intercepted grammarと--detailsのroute scopeを検証し、
// delegated pacman自身のopen grammarはそのまま返す。productionとdry-runの共通authority。
CliInvocationValidation validate_cli_invocation_contract(
    const ParsedCliArguments& parsed);

// Typed issueを既存のlocalized CLI messageへ投影する。classificationはmessage
// から推測せず、validate_cli_invocation_contractが明示した値を保持する。
std::string cli_invocation_issue_message(
    const CliInvocationIssue& issue);
