#include "cli_runtime_contract.hpp"

#include "application_identity.hpp"
#include "cli_routing.hpp"
#include "localization.hpp"
#include "source_environment.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace {

using cli_authority::OperandKind;
using cli_authority::OperandOrderingRule;
using cli_authority::OperationFormSpec;
using cli_authority::OperationId;
using cli_authority::OperationMetadata;
using cli_authority::SpecialOperationId;
using cli_authority::TargetPolicy;

bool has_semantic_option(
    const ParsedCliArguments& parsed, std::string_view option) noexcept {
    for(const ParsedCliToken& token : parsed.tokens) {
        if(token.role == CliTokenRole::PacmanOption && token.value == option) {
            return true;
        }
    }
    return false;
}

bool is_pkgbuild_output_directory_option(
    std::string_view token) noexcept {
    const std::string_view option =
        cli_authority::PKGBUILD_OUTPUT_DIRECTORY_OPTION;
    return token == option ||
           (token.size() > option.size() && token.starts_with(option) &&
            token[option.size()] == '=');
}

bool has_semantic_pkgbuild_output_directory_option(
    const ParsedCliArguments& parsed) noexcept {
    for(const ParsedCliToken& token : parsed.tokens) {
        if(token.role == CliTokenRole::PacmanOption &&
           is_pkgbuild_output_directory_option(token.value)) {
            return true;
        }
    }
    return false;
}

const OperationFormSpec& selected_operation_form(
    const ParsedCliArguments& parsed,
    const OperationMetadata& metadata) noexcept {
    if(metadata.id == OperationId::Build &&
       has_semantic_option(parsed, cli_authority::LOCAL_SOURCE_OPTION)) {
        return cli_authority::operation_form(metadata, 1);
    }
    return cli_authority::operation_form(metadata, 0);
}

bool is_environment_assignment(const std::string& operand) {
    std::string key;
    std::string value;
    return split_env_assignment(operand, key, value);
}

OperandKind primary_operand_kind(const OperationFormSpec& form) noexcept {
    return form.operands.term_count == 0
               ? OperandKind::None
               : form.operands.terms.front().kind;
}

DiagnosticOperation diagnostic_operation(OperationId operation) noexcept {
    switch(operation) {
        case OperationId::Build:
            return DiagnosticOperation::Build;
        case OperationId::Upgrade:
            return DiagnosticOperation::Upgrade;
        case OperationId::UpgradeAur:
            return DiagnosticOperation::UpgradeAur;
        case OperationId::UpgradeAll:
            return DiagnosticOperation::UpgradeAll;
        case OperationId::Clean:
            return DiagnosticOperation::Clean;
        case OperationId::Deps:
            return DiagnosticOperation::Deps;
        case OperationId::Plan:
            return DiagnosticOperation::Plan;
        case OperationId::Fetch:
            return DiagnosticOperation::Fetch;
        case OperationId::AddSource:
            return DiagnosticOperation::AddSource;
        case OperationId::DeleteSource:
            return DiagnosticOperation::DeleteSource;
        case OperationId::Revert:
            return DiagnosticOperation::Revert;
        case OperationId::EditSource:
            return DiagnosticOperation::EditSource;
        case OperationId::ListSources:
            return DiagnosticOperation::ListSources;
        case OperationId::Count:
            return DiagnosticOperation::CliParsing;
    }
    return DiagnosticOperation::CliParsing;
}

DiagnosticOperation diagnostic_operation(
    SpecialOperationId operation) noexcept {
    switch(operation) {
        case SpecialOperationId::PkgbuildExport:
            return DiagnosticOperation::PkgbuildExport;
        case SpecialOperationId::PkgbuildPrint:
            return DiagnosticOperation::PkgbuildPrint;
        case SpecialOperationId::SyncSelect:
            return DiagnosticOperation::RootPackageSelection;
        case SpecialOperationId::SystemRepositoryUpdate:
        case SpecialOperationId::SystemAurUpdate:
        case SpecialOperationId::SystemRepositoryUpdateNoRefresh:
        case SpecialOperationId::SystemAurUpdateNoRefresh:
            return DiagnosticOperation::PacmanDelegation;
        case SpecialOperationId::DelegatedPacmanGrammar:
            return DiagnosticOperation::PacmanDelegation;
        case SpecialOperationId::Help:
        case SpecialOperationId::Version:
        case SpecialOperationId::Count:
            return DiagnosticOperation::CliParsing;
    }
    return DiagnosticOperation::CliParsing;
}

NormalizedDiagnostic<CliInvocationIssue> make_cli_diagnostic(
    CliInvocationIssue issue, DiagnosticClass classification,
    DiagnosticOperation operation) {
    return NormalizedDiagnostic<CliInvocationIssue>{
        classification,
        DiagnosticSeverity::Error,
        operation,
        DiagnosticPhase::Parsing,
        {},
        std::move(issue),
        DiagnosticRequiredAction::CorrectInput,
        DiagnosticBlockingDecision::BlocksCurrentOperation,
        DiagnosticExitStatusEffect::Failure,
        std::nullopt};
}

CliInvocationValidation invalid_invocation(
    ResolvedCliRuntimeContract contract, CliInvocationIssue issue,
    DiagnosticClass classification,
    DiagnosticOperation operation) {
    return CliInvocationValidation{
        contract,
        make_cli_diagnostic(
            std::move(issue), classification, operation)};
}

std::optional<CliInvocationIssue> validate_operand_contract(
    const ParsedCliArguments& parsed,
    const OperationFormSpec& form) {
    const std::size_t operand_count = parsed.targets.size();
    const OperandKind primary_kind = primary_operand_kind(form);
    const std::string operation =
        form.operation == OperationId::Build &&
                primary_kind == OperandKind::Directory
            ? "build --local"
            : parsed.operation;
    auto issue = [&](CliInvocationIssueKind kind,
                     std::optional<std::string> operand = std::nullopt) {
        return CliInvocationIssue{
            kind, operation, std::move(operand),
            form.target_policy, primary_kind};
    };

    switch(form.target_policy) {
        case TargetPolicy::None:
            if(operand_count != 0) {
                return issue(
                    CliInvocationIssueKind::ExtraOperand,
                    parsed.targets.front());
            }
            return std::nullopt;
        case TargetPolicy::OneOrMore:
            return operand_count == 0
                       ? std::optional<CliInvocationIssue>{
                             issue(CliInvocationIssueKind::MissingOperand)}
                       : std::nullopt;
        case TargetPolicy::OrderedItems:
            if(operand_count == 0) {
                return issue(CliInvocationIssueKind::MissingOperand);
            }
            if(is_environment_assignment(parsed.targets.front()) ||
               parsed.targets.front().find('=') != std::string::npos) {
                return issue(
                    CliInvocationIssueKind::InvalidOperandOrdering,
                    parsed.targets.front());
            }
            return std::nullopt;
        case TargetPolicy::ExactlyOne:
            break;
        case TargetPolicy::Delegated:
            return std::nullopt;
    }

    if(form.operands.ordering !=
       OperandOrderingRule::PrimaryThenEnvironmentAssignments) {
        if(operand_count == 0) {
            return issue(CliInvocationIssueKind::MissingOperand);
        }
        if(operand_count > 1) {
            return issue(
                CliInvocationIssueKind::ExtraOperand,
                parsed.targets[1]);
        }
        return std::nullopt;
    }

    if(operand_count == 0) {
        return issue(CliInvocationIssueKind::MissingOperand);
    }
    if(is_environment_assignment(parsed.targets.front())) {
        return issue(
            CliInvocationIssueKind::InvalidOperandOrdering,
            parsed.targets.front());
    }
    if(parsed.targets.front().find('=') != std::string::npos) {
        return issue(
            CliInvocationIssueKind::InvalidEnvironmentAssignment,
            parsed.targets.front());
    }
    for(std::size_t index = 1; index < operand_count; ++index) {
        if(is_environment_assignment(parsed.targets[index])) continue;
        if(parsed.targets[index].find('=') != std::string::npos) {
            return issue(
                CliInvocationIssueKind::InvalidEnvironmentAssignment,
                parsed.targets[index]);
        }
        return issue(
            CliInvocationIssueKind::ExtraOperand,
            parsed.targets[index]);
    }
    return std::nullopt;
}

std::optional<CliInvocationIssue> validate_operand_contract(
    const ParsedCliArguments& parsed,
    const cli_authority::SpecialOperationSpec& special) {
    const std::size_t operand_count = parsed.targets.size();
    const std::string operation =
        special.id == SpecialOperationId::SyncSelect
            ? "-S --select"
            : parsed.operation;
    const OperandKind primary_kind = special.operands.term_count == 0
                                         ? OperandKind::None
                                         : special.operands.terms.front().kind;
    if(special.target_policy == TargetPolicy::Delegated) return std::nullopt;
    if(special.target_policy == TargetPolicy::None && operand_count != 0) {
        return CliInvocationIssue{
            CliInvocationIssueKind::ExtraOperand,
            operation,
            parsed.targets.front(),
            special.target_policy,
            primary_kind};
    }
    if(special.target_policy == TargetPolicy::ExactlyOne &&
       operand_count != 1) {
        return CliInvocationIssue{
            operand_count == 0
                ? CliInvocationIssueKind::MissingOperand
                : CliInvocationIssueKind::ExtraOperand,
            operation,
            operand_count > 1
                ? std::optional<std::string>{parsed.targets[1]}
                : std::nullopt,
            special.target_policy,
            primary_kind};
    }
    return std::nullopt;
}

std::string operand_placeholder(OperandKind kind) {
    switch(kind) {
        case OperandKind::Package:
            return "<pkg>";
        case OperandKind::Directory:
            return "<directory>";
        case OperandKind::Query:
            return "<query>";
        case OperandKind::SourcePreferenceItem:
            return "<item>";
        case OperandKind::EnvironmentAssignment:
            return "V=K";
        case OperandKind::DelegatedPacmanArgument:
            return "<pacman-arg>";
        case OperandKind::None:
            return "<operand>";
    }
    return "<operand>";
}

} // namespace

bool ResolvedCliRuntimeContract::is_known() const noexcept {
    return operation != nullptr || special_operation != nullptr;
}

bool ResolvedCliRuntimeContract::is_delegated() const noexcept {
    return is_known() &&
           owner == cli_authority::GrammarOwnership::DelegatedPacman;
}

bool CliInvocationValidation::is_valid() const noexcept {
    return !diagnostic.has_value();
}

ResolvedCliRuntimeContract resolve_cli_runtime_contract(
    const ParsedCliArguments& parsed) {
    if(const cli_authority::OperationSpec* legacy =
           cli_authority::find_moguet_operation(parsed.operation);
       legacy != nullptr) {
        const OperationMetadata& metadata =
            cli_authority::operation_metadata(legacy->id);
        const OperationFormSpec& form =
            selected_operation_form(parsed, metadata);
        return ResolvedCliRuntimeContract{
            &metadata, &form, nullptr, metadata.owner};
    }

    const cli_authority::SpecialOperationSpec* special = nullptr;
    if(parsed.operation == cli_authority::PKGBUILD_EXPORT_OPERATION) {
        special = &cli_authority::special_operation_spec(
            SpecialOperationId::PkgbuildExport);
    } else if(parsed.operation == cli_authority::PKGBUILD_PRINT_OPERATION) {
        special = &cli_authority::special_operation_spec(
            SpecialOperationId::PkgbuildPrint);
    } else if(parsed.root_package_selection_requested &&
              parsed.operation == "-S") {
        special = &cli_authority::special_operation_spec(
            SpecialOperationId::SyncSelect);
    } else {
        const SyncInvocationRouteClassification sync_route =
            classify_sync_invocation_route(parsed);
        if(std::holds_alternative<RepoOnlySystemUpdateRouteCandidate>(
               sync_route)) {
            special = &cli_authority::special_operation_spec(
                parsed.operation == cli_authority::PACMAN_SYSTEM_UPGRADE_NO_REFRESH_SYNTAX
                    ? SpecialOperationId::SystemRepositoryUpdateNoRefresh
                    : SpecialOperationId::SystemRepositoryUpdate);
        } else if(!std::holds_alternative<OtherSyncRoute>(sync_route)) {
            special = &cli_authority::special_operation_spec(
                parsed.operation == cli_authority::PACMAN_SYSTEM_UPGRADE_NO_REFRESH_SYNTAX
                    ? SpecialOperationId::SystemAurUpdateNoRefresh
                    : SpecialOperationId::SystemAurUpdate);
        }
    }
    if(special != nullptr) {
        return ResolvedCliRuntimeContract{
            nullptr, nullptr, special, special->owner};
    }
    if(parsed.operation == cli_authority::HELP_SHORT_OPTION ||
       parsed.operation == cli_authority::HELP_LONG_OPTION) {
        special = &cli_authority::special_operation_spec(
            SpecialOperationId::Help);
    } else if(parsed.operation == cli_authority::VERSION_SHORT_OPTION ||
              parsed.operation == cli_authority::VERSION_LONG_OPTION) {
        special = &cli_authority::special_operation_spec(
            SpecialOperationId::Version);
    } else if(!parsed.operation.empty() && parsed.operation.front() == '-') {
        special = &cli_authority::special_operation_spec(
            SpecialOperationId::DelegatedPacmanGrammar);
    }

    if(special == nullptr) return {};
    return ResolvedCliRuntimeContract{
        nullptr, nullptr, special, special->owner};
}

CliInvocationValidation validate_cli_invocation_contract(
    const ParsedCliArguments& parsed) {
    ResolvedCliRuntimeContract contract =
        resolve_cli_runtime_contract(parsed);

    if(parsed.operation == cli_authority::LOCAL_SOURCE_OPTION) {
        return invalid_invocation(
            contract,
            CliInvocationIssue{
                CliInvocationIssueKind::MisplacedLocalSourceOption,
                parsed.operation,
                std::nullopt,
                TargetPolicy::None,
                OperandKind::None},
            DiagnosticClass::Invalid,
            DiagnosticOperation::CliParsing);
    }
    if(is_pkgbuild_output_directory_option(parsed.operation) ||
       (parsed.operation != cli_authority::PKGBUILD_EXPORT_OPERATION &&
        has_semantic_pkgbuild_output_directory_option(parsed))) {
        return invalid_invocation(
            contract,
            CliInvocationIssue{
                CliInvocationIssueKind::
                    MisplacedPkgbuildOutputDirectoryOption,
                parsed.operation,
                std::nullopt,
                TargetPolicy::None,
                OperandKind::None},
            DiagnosticClass::Unsupported,
            DiagnosticOperation::CliParsing);
    }
    if(!contract.is_known()) {
        return invalid_invocation(
            contract,
            CliInvocationIssue{
                CliInvocationIssueKind::UnknownOperation,
                parsed.operation,
                std::nullopt,
                TargetPolicy::None,
                OperandKind::None},
            DiagnosticClass::Unsupported,
            DiagnosticOperation::CliParsing);
    }
    // Presentation state can have other authorities; only an actual Moguet
    // option occurrence is subject to this CLI scope check, before delegation.
    for(const ParsedCliToken& token : parsed.tokens) {
        if(token.role != CliTokenRole::MoguetGlobalOption ||
           token.value != cli_authority::option_contract(
                              cli_authority::OptionId::Details)
                              .canonical_token) {
            continue;
        }
        const auto& relations = contract.form != nullptr
                                    ? contract.form->option_relations
                                    : contract.special_operation->option_relations;
        if(!relations.contains(cli_authority::OptionId::Details)) {
            return invalid_invocation(
                contract,
                CliInvocationIssue{
                    CliInvocationIssueKind::UnsupportedPresentationDetail,
                    parsed.operation, std::nullopt,
                    TargetPolicy::None, OperandKind::None},
                DiagnosticClass::Unsupported,
                contract.operation != nullptr
                    ? diagnostic_operation(contract.operation->id)
                    : diagnostic_operation(contract.special_operation->id));
        }
        break;
    }
    if(parsed.root_package_selection_requested &&
       parsed.operation != "-S") {
        const bool local_build_route =
            contract.operation != nullptr &&
            contract.operation->id == OperationId::Build &&
            primary_operand_kind(*contract.form) == OperandKind::Directory;
        if(!local_build_route) {
            return invalid_invocation(
                contract,
                CliInvocationIssue{
                    CliInvocationIssueKind::SelectRequiresPlainSync,
                    parsed.operation,
                    std::nullopt,
                    TargetPolicy::ExactlyOne,
                    OperandKind::Query},
                DiagnosticClass::Invalid,
                DiagnosticOperation::RootPackageSelection);
        }
    }
    const SyncInvocationRouteClassification sync_route =
        classify_sync_invocation_route(parsed);
    if(const auto* auto_candidate =
           std::get_if<AutoSystemUpdateRouteCandidate>(&sync_route)) {
        if(const auto* incompatible = std::get_if<
               IncompatibleAutoSystemUpdatePacmanArguments>(
               &auto_candidate->pacman_compatibility)) {
            const CliInvocationIssueKind kind =
                incompatible->kind ==
                        AutoSystemUpdatePacmanIncompatibilityKind::
                            UnsupportedOption
                    ? CliInvocationIssueKind::
                          UnsupportedAutoSystemUpdateOption
                    : CliInvocationIssueKind::
                          UnsupportedAutoSystemUpdateArgumentForm;
            return invalid_invocation(
                contract,
                CliInvocationIssue{
                    kind, parsed.operation, incompatible->token,
                    TargetPolicy::None, OperandKind::None},
                DiagnosticClass::Unsupported,
                DiagnosticOperation::PacmanDelegation);
        }
    }
    if(contract.is_delegated()) return CliInvocationValidation{contract, {}};

    std::optional<CliInvocationIssue> issue;
    DiagnosticOperation operation = DiagnosticOperation::CliParsing;
    if(contract.operation != nullptr) {
        operation = diagnostic_operation(contract.operation->id);
        issue = validate_operand_contract(parsed, *contract.form);
    } else {
        issue = validate_operand_contract(parsed, *contract.special_operation);
        operation = diagnostic_operation(contract.special_operation->id);
    }
    if(!issue.has_value()) return CliInvocationValidation{contract, {}};
    return invalid_invocation(
        contract, std::move(*issue), DiagnosticClass::Invalid,
        operation);
}

std::string cli_invocation_issue_message(
    const CliInvocationIssue& issue) {
    switch(issue.kind) {
        case CliInvocationIssueKind::UnknownOperation:
            return localization::format_translated_message(
                "Unknown operation: {}", issue.operation);
        case CliInvocationIssueKind::MisplacedLocalSourceOption:
            return localization::format_translated_message(
                "Option {} is supported only with operation {}.",
                cli_authority::LOCAL_SOURCE_OPTION, "build");
        case CliInvocationIssueKind::MisplacedPkgbuildOutputDirectoryOption:
            return localization::format_translated_message(
                "Option {} is supported only with operation {}.",
                cli_option_syntax(
                    cli_authority::OptionId::PkgbuildOutputDirectory),
                cli_authority::PKGBUILD_EXPORT_OPERATION);
        case CliInvocationIssueKind::SelectRequiresPlainSync:
            return localization::format_translated_message(
                "Option {} is supported only with plain {}.",
                "--select", "-S");
        case CliInvocationIssueKind::UnsupportedPresentationDetail:
            return localization::format_translated_message(
                "Option {} is not supported for operation {}.",
                cli_authority::option_contract(
                    cli_authority::OptionId::Details)
                    .canonical_token,
                issue.operation);
        case CliInvocationIssueKind::UnsupportedAutoSystemUpdateOption:
            return localization::format_translated_message(
                "A {} option is not supported for the combined {} route. Use {} for a repository-only system upgrade with full {} pass-through.",
                "pacman", issue.operation, "moguet " + issue.operation + " --repo", "pacman");
        case CliInvocationIssueKind::
            UnsupportedAutoSystemUpdateArgumentForm:
            return localization::format_translated_message(
                "A {} argument form is not supported for the combined {} route. Use {} for a repository-only system upgrade with full {} pass-through.",
                "pacman", issue.operation, "moguet " + issue.operation + " --repo", "pacman");
        case CliInvocationIssueKind::ExtraOperand:
            if(issue.target_policy == TargetPolicy::None) {
                return localization::format_translated_message(
                    "Operation {} does not accept target operands.",
                    issue.operation);
            }
            return localization::format_translated_message(
                "Operation {} requires exactly one {} operand.",
                issue.operation,
                operand_placeholder(issue.primary_operand_kind));
        case CliInvocationIssueKind::InvalidOperandOrdering:
            if(issue.primary_operand_kind == OperandKind::Directory) {
                return localization::format_translated_message(
                    "Environment assignment requires a preceding directory: {}",
                    issue.operand.value_or(""));
            }
            return localization::format_translated_message(
                "Environment assignment requires a preceding package: {}",
                issue.operand.value_or(""));
        case CliInvocationIssueKind::InvalidEnvironmentAssignment:
            return localization::format_translated_message(
                "Invalid environment assignment: {}",
                issue.operand.value_or(""));
        case CliInvocationIssueKind::MissingOperand:
            break;
    }

    if(const cli_authority::OperationSpec* operation =
           cli_authority::find_moguet_operation(issue.operation);
       operation != nullptr) {
        return localization::format_translated_message(
            "Usage: {} {}", application_identity::COMMAND_NAME,
            cli_operation_syntax(operation->id));
    }

    if(issue.target_policy == TargetPolicy::ExactlyOne) {
        return localization::format_translated_message(
            "Operation {} requires exactly one {} operand.",
            issue.operation,
            operand_placeholder(issue.primary_operand_kind));
    }
    return localization::format_translated_message(
        "Operation {} requires exactly one {} operand.",
        issue.operation,
        operand_placeholder(issue.primary_operand_kind));
}
