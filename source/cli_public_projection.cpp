#include "cli_public_projection.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace {

using cli_authority::OperandKind;
using cli_authority::OperandTermSpec;
using cli_authority::OperationFormSpec;
using cli_authority::OperationId;
using cli_authority::OperationMetadata;
using cli_authority::OptionPublicSyntax;
using cli_authority::OptionRelationContract;
using cli_authority::SpecialOperationId;

std::string canonical_option_syntax(
    const cli_authority::OptionContract& option) {
    std::string syntax(option.canonical_token);
    if(option.value.kind == cli_authority::OptionValueKind::AttachedEnum) {
        syntax += "=";
        for(std::size_t index = 0;
            index < option.value.allowed_value_count; ++index) {
            if(index != 0) syntax += "|";
            syntax += option.value.allowed_values[index];
        }
    } else if(option.value.kind ==
              cli_authority::OptionValueKind::AttachedValue) {
        syntax += "=";
        syntax += option.value.allowed_values.front();
    }
    return syntax;
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

void append_public_options(
    std::string& syntax,
    const cli_authority::OperationOptionRelationSet& relations) {
    for(std::size_t index = 0; index < relations.count; ++index) {
        const OptionRelationContract& relation = relations.values[index];
        if(relation.public_syntax == OptionPublicSyntax::Hidden) continue;

        syntax += " ";
        const bool optional =
            relation.public_syntax == OptionPublicSyntax::Optional;
        if(optional) syntax += "[";
        syntax += canonical_option_syntax(
            cli_authority::option_contract(relation.option));
        if(optional) syntax += "]";
    }
}

void append_operands(
    std::string& syntax,
    const cli_authority::OperandContract& operands) {
    for(std::size_t index = 0; index < operands.term_count; ++index) {
        const OperandTermSpec& term = operands.terms[index];
        syntax += " ";
        const bool optional = term.min_count == 0;
        if(optional) syntax += "[";
        syntax += operand_placeholder(term.kind);
        if(term.max_count == cli_authority::UNBOUNDED_OPERAND_COUNT) {
            syntax += "...";
        }
        if(optional) syntax += "]";
    }
}

constexpr std::array PUBLIC_OPERATION_ORDER = {
    OperationId::Build,
    OperationId::Upgrade,
    OperationId::UpgradeAur,
    OperationId::UpgradeAll,
    OperationId::Clean,
    OperationId::Deps,
    OperationId::Plan,
    OperationId::Fetch,
    OperationId::AddSource,
    OperationId::EditSource,
    OperationId::ListSources,
    OperationId::DeleteSource,
    OperationId::Revert,
};

constexpr std::array PUBLIC_SPECIAL_OPERATION_ORDER = {
    SpecialOperationId::PkgbuildExport,
    SpecialOperationId::PkgbuildPrint,
    SpecialOperationId::SyncSelect,
    SpecialOperationId::SystemAurUpdate,
    SpecialOperationId::SystemRepositoryUpdate,
    SpecialOperationId::SystemAurUpdateNoRefresh,
    SpecialOperationId::SystemRepositoryUpdateNoRefresh,
};

} // namespace

std::string cli_operation_form_syntax(
    const OperationMetadata& metadata,
    const OperationFormSpec& form) {
    std::string syntax(metadata.canonical_token);
    append_public_options(syntax, form.option_relations);
    append_operands(syntax, form.operands);
    return syntax;
}

std::string cli_operation_syntax(OperationId operation) {
    const OperationMetadata& metadata =
        cli_authority::operation_metadata(operation);
    std::string syntax;
    for(std::size_t form_index = 0;
        form_index < metadata.form_count; ++form_index) {
        if(form_index != 0) syntax += " | ";
        syntax += cli_operation_form_syntax(
            metadata,
            cli_authority::operation_form(metadata, form_index));
    }
    return syntax;
}

std::string cli_special_operation_syntax(SpecialOperationId operation) {
    const cli_authority::SpecialOperationSpec& special =
        cli_authority::special_operation_spec(operation);
    std::string syntax(special.canonical_token);
    if(operation == SpecialOperationId::PkgbuildExport) {
        // Public canonical form keeps the target first even though the parser
        // also accepts the operation-local option before it.
        append_operands(syntax, special.operands);
        append_public_options(syntax, special.option_relations);
        return syntax;
    }
    append_public_options(syntax, special.option_relations);
    append_operands(syntax, special.operands);
    return syntax;
}

std::string cli_option_syntax(cli_authority::OptionId option_id) {
    const cli_authority::OptionContract& option =
        cli_authority::option_contract(option_id);
    std::string syntax;
    for(std::size_t index = 0; index < option.aliases.count; ++index) {
        if(!syntax.empty()) syntax += ", ";
        syntax += option.aliases.values[index];
    }
    if(!syntax.empty()) syntax += ", ";
    syntax += canonical_option_syntax(option);
    return syntax;
}

std::span<const OperationId> cli_public_operation_order() {
    return PUBLIC_OPERATION_ORDER;
}

std::span<const SpecialOperationId> cli_public_special_operation_order() {
    return PUBLIC_SPECIAL_OPERATION_ORDER;
}

std::vector<std::string> cli_canonical_grammar() {
    std::vector<std::string> grammar;
    grammar.reserve(
        cli_authority::MOGUET_OPERATION_FORMS.size() +
        PUBLIC_SPECIAL_OPERATION_ORDER.size());
    for(OperationId id : cli_public_operation_order()) {
        const OperationMetadata& metadata =
            cli_authority::operation_metadata(id);
        for(std::size_t form_index = 0;
            form_index < metadata.form_count; ++form_index) {
            grammar.push_back(cli_operation_form_syntax(
                metadata,
                cli_authority::operation_form(metadata, form_index)));
        }
    }
    for(SpecialOperationId id : cli_public_special_operation_order()) {
        grammar.push_back(cli_special_operation_syntax(id));
    }
    return grammar;
}
