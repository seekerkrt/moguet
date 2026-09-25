#include "cli_authority.hpp"
#include "cli_public_projection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

using cli_authority::GrammarOwnership;
using cli_authority::OperandContract;
using cli_authority::OperandKind;
using cli_authority::OperandOrderingRule;
using cli_authority::OperationId;
using cli_authority::OperationOptionRelationSet;
using cli_authority::OptionConflictSet;
using cli_authority::OptionConflictRule;
using cli_authority::OptionCompletionVisibility;
using cli_authority::OptionForwardingOccurrence;
using cli_authority::OptionId;
using cli_authority::OptionLexicalPlacement;
using cli_authority::OptionOccurrence;
using cli_authority::OptionPublicDefinitionRole;
using cli_authority::OptionPublicSyntax;
using cli_authority::OptionRelationRequirement;
using cli_authority::OptionValueKind;
using cli_authority::SpecialOperationId;
using cli_authority::TargetPolicy;

template <typename Enum>
constexpr std::size_t enum_index(Enum value) noexcept {
    return static_cast<std::size_t>(value);
}

std::string_view occurrence_name(OptionOccurrence occurrence) noexcept {
    switch(occurrence) {
        case OptionOccurrence::Once:
            return "once";
        case OptionOccurrence::RepeatIdempotent:
            return "repeat-idempotent";
        case OptionOccurrence::RepeatSameValue:
            return "repeat-same-value";
        case OptionOccurrence::Delegated:
            return "delegated";
    }
    return "unknown";
}

std::string_view placement_name(
    OptionLexicalPlacement placement) noexcept {
    switch(placement) {
        case OptionLexicalPlacement::ParserGlobalNormalPosition:
            return "parser-global";
        case OptionLexicalPlacement::FirstNonGlobalToken:
            return "first-non-global";
        case OptionLexicalPlacement::OperationLocal:
            return "operation-local";
        case OptionLexicalPlacement::PacmanGrammar:
            return "pacman-grammar";
        case OptionLexicalPlacement::EndOfOptionsMarker:
            return "end-of-options";
    }
    return "unknown";
}

std::string_view value_kind_name(OptionValueKind kind) noexcept {
    switch(kind) {
        case OptionValueKind::None:
            return "none";
        case OptionValueKind::AttachedEnum:
            return "attached-enum";
        case OptionValueKind::AttachedValue:
            return "attached-value";
        case OptionValueKind::Marker:
            return "marker";
    }
    return "unknown";
}

std::string_view conflict_rule_name(OptionConflictRule rule) noexcept {
    switch(rule) {
        case OptionConflictRule::None:
            return "none";
        case OptionConflictRule::OperationLocalExclusion:
            return "operation-local-exclusion";
        case OptionConflictRule::MutuallyExclusive:
            return "mutually-exclusive";
        case OptionConflictRule::FinalValueMustAgree:
            return "final-value-must-agree";
    }
    return "unknown";
}

std::string_view ownership_name(GrammarOwnership ownership) noexcept {
    switch(ownership) {
        case GrammarOwnership::MoguetOwned:
            return "moguet-owned";
        case GrammarOwnership::InterceptedPacman:
            return "intercepted-pacman";
        case GrammarOwnership::DelegatedPacman:
            return "delegated-pacman";
    }
    return "unknown";
}

std::string_view requirement_name(
    OptionRelationRequirement requirement) noexcept {
    switch(requirement) {
        case OptionRelationRequirement::Optional:
            return "optional";
        case OptionRelationRequirement::Required:
            return "required";
    }
    return "unknown";
}

std::string_view public_syntax_name(
    OptionPublicSyntax syntax) noexcept {
    switch(syntax) {
        case OptionPublicSyntax::Hidden:
            return "hidden";
        case OptionPublicSyntax::Optional:
            return "optional";
        case OptionPublicSyntax::Required:
            return "required";
    }
    return "unknown";
}

std::string_view public_definition_role_name(
    OptionPublicDefinitionRole role) noexcept {
    switch(role) {
        case OptionPublicDefinitionRole::Definition:
            return "definition";
        case OptionPublicDefinitionRole::SyntaxOnly:
            return "syntax-only";
        case OptionPublicDefinitionRole::SchemaOnly:
            return "schema-only";
    }
    return "unknown";
}

std::string_view completion_visibility_name(
    OptionCompletionVisibility visibility) noexcept {
    switch(visibility) {
        case OptionCompletionVisibility::SuggestedAndDescribed:
            return "suggested-and-described";
        case OptionCompletionVisibility::Hidden:
            return "hidden";
    }
    return "unknown";
}

std::string_view forwarding_occurrence_name(
    OptionForwardingOccurrence occurrence) noexcept {
    switch(occurrence) {
        case OptionForwardingOccurrence::None:
            return "none";
        case OptionForwardingOccurrence::PreserveAll:
            return "preserve-all";
        case OptionForwardingOccurrence::ConsolidateSingle:
            return "consolidate-single";
    }
    return "unknown";
}

std::string_view target_policy_name(TargetPolicy policy) noexcept {
    switch(policy) {
        case TargetPolicy::None:
            return "none";
        case TargetPolicy::ExactlyOne:
            return "exactly-one";
        case TargetPolicy::OneOrMore:
            return "one-or-more";
        case TargetPolicy::OrderedItems:
            return "ordered-items";
        case TargetPolicy::FixedSequence:
            return "fixed-sequence";
        case TargetPolicy::Delegated:
            return "delegated";
    }
    return "unknown";
}

std::string_view operand_kind_name(OperandKind kind) noexcept {
    switch(kind) {
        case OperandKind::None:
            return "none";
        case OperandKind::Package:
            return "package";
        case OperandKind::Directory:
            return "directory";
        case OperandKind::PatchDirectory: return "patch-directory";
        case OperandKind::PatchFile: return "patch-file";
        case OperandKind::PackageBase: return "package-base";
        case OperandKind::Query:
            return "query";
        case OperandKind::SourcePreferenceItem:
            return "source-preference-item";
        case OperandKind::EnvironmentAssignment:
            return "environment-assignment";
        case OperandKind::DelegatedPacmanArgument:
            return "delegated-pacman-argument";
    }
    return "unknown";
}

std::string_view operand_ordering_name(
    OperandOrderingRule ordering) noexcept {
    switch(ordering) {
        case OperandOrderingRule::None:
            return "none";
        case OperandOrderingRule::PreserveInputOrder:
            return "preserve-input-order";
        case OperandOrderingRule::PrimaryThenEnvironmentAssignments:
            return "primary-then-environment-assignments";
        case OperandOrderingRule::PackageIntroducesFollowingAssignmentScope:
            return "package-introduces-following-assignment-scope";
        case OperandOrderingRule::Delegated:
            return "delegated";
    }
    return "unknown";
}

template <typename Enum, std::size_t Size>
void print_mask_names(
    std::uint32_t mask,
    const std::array<std::pair<Enum, std::string_view>, Size>& names) {
    if(mask == 0) {
        std::cout << "none";
        return;
    }

    bool first = true;
    std::uint32_t remaining = mask;
    for(const auto& [value, name] : names) {
        const std::uint32_t bit = static_cast<std::uint32_t>(value);
        if((remaining & bit) == 0) continue;
        if(!first) std::cout << '+';
        first = false;
        std::cout << name;
        remaining &= ~bit;
    }
    if(remaining == 0) return;
    if(!first) std::cout << '+';
    std::cout << "unknown";
}

void print_semantic_scopes(
    cli_authority::OptionSemanticScopeMask scopes) {
    constexpr std::array NAMES = {
        std::pair{cli_authority::OptionSemanticScope::Information,
                  std::string_view{"information"}},
        std::pair{cli_authority::OptionSemanticScope::SourceBuildReview,
                  std::string_view{"source-build-review"}},
        std::pair{
            cli_authority::OptionSemanticScope::SourceCheckoutReview,
            std::string_view{"source-checkout-review"}},
        std::pair{cli_authority::OptionSemanticScope::SourceBuild,
                  std::string_view{"source-build"}},
        std::pair{cli_authority::OptionSemanticScope::DryRunRouting,
                  std::string_view{"dry-run-routing"}},
        std::pair{cli_authority::OptionSemanticScope::RootPackageSelection,
                  std::string_view{"root-package-selection"}},
        std::pair{cli_authority::OptionSemanticScope::SourceSelection,
                  std::string_view{"source-selection"}},
        std::pair{cli_authority::OptionSemanticScope::LocalSourceBuild,
                  std::string_view{"local-source-build"}},
        std::pair{
            cli_authority::OptionSemanticScope::DependencyInspection,
            std::string_view{"dependency-inspection"}},
        std::pair{cli_authority::OptionSemanticScope::FinalPackageInstall,
                  std::string_view{"final-package-install"}},
        std::pair{cli_authority::OptionSemanticScope::PacmanDelegation,
                  std::string_view{"pacman-delegation"}},
        std::pair{cli_authority::OptionSemanticScope::ParserBoundary,
                  std::string_view{"parser-boundary"}},
        std::pair{cli_authority::OptionSemanticScope::DependencyCleanup,
                  std::string_view{"dependency-cleanup"}},
        std::pair{cli_authority::OptionSemanticScope::PackageExport,
                  std::string_view{"package-export"}},
        std::pair{cli_authority::OptionSemanticScope::PresentationDetail,
                  std::string_view{"presentation-detail"}},
    };
    print_mask_names(scopes, NAMES);
}

void print_semantic_effects(
    cli_authority::OptionSemanticEffectMask effects) {
    constexpr std::array NAMES = {
        std::pair{cli_authority::OptionSemanticEffect::MoguetControl,
                  std::string_view{"moguet-control"}},
        std::pair{cli_authority::OptionSemanticEffect::UpstreamArgument,
                  std::string_view{"upstream-argument"}},
        std::pair{
            cli_authority::OptionSemanticEffect::FinalInstallSemantic,
            std::string_view{"final-install-semantic"}},
        std::pair{cli_authority::OptionSemanticEffect::ParserBoundary,
                  std::string_view{"parser-boundary"}},
    };
    print_mask_names(effects, NAMES);
}

void print_forwarding_targets(
    cli_authority::OptionForwardingTargetMask targets) {
    constexpr std::array NAMES = {
        std::pair{cli_authority::OptionForwardingTarget::Pacman,
                  std::string_view{"pacman"}},
        std::pair{cli_authority::OptionForwardingTarget::Makepkg,
                  std::string_view{"makepkg"}},
        std::pair{
            cli_authority::OptionForwardingTarget::FinalInstallPacman,
            std::string_view{"final-install-pacman"}},
    };
    print_mask_names(targets, NAMES);
}

void print_operand_terms(const OperandContract& operands) {
    for(std::size_t index = 0; index < operands.term_count; ++index) {
        if(index != 0) std::cout << ',';
        const cli_authority::OperandTermSpec& term = operands.terms[index];
        std::cout << operand_kind_name(term.kind) << ':' << term.min_count
                  << ':';
        if(term.max_count == cli_authority::UNBOUNDED_OPERAND_COUNT) {
            std::cout << '*';
        } else {
            std::cout << term.max_count;
        }
    }
}

void print_ids(std::span<const OptionId> ids) {
    for(std::size_t index = 0; index < ids.size(); ++index) {
        if(index != 0) std::cout << ',';
        std::cout << enum_index(ids[index]);
    }
}

void print_conflicts(const OptionConflictSet& conflicts) {
    print_ids(std::span{conflicts.values}.first(conflicts.count));
}

void print_relations(const OperationOptionRelationSet& relations) {
    bool first = true;
    for(std::size_t index = 0; index < relations.count; ++index) {
        const cli_authority::OptionRelationContract& relation =
            relations.values[index];
        if(!first) std::cout << ',';
        first = false;
        std::cout << enum_index(relation.option) << ':'
                  << requirement_name(relation.requirement) << ':'
                  << occurrence_name(relation.occurrence) << ':'
                  << public_syntax_name(relation.public_syntax) << ':';
        print_semantic_effects(relation.semantic_effects);
        std::cout << ':';
        print_forwarding_targets(relation.forwarding_targets);
        std::cout << ':'
                  << forwarding_occurrence_name(
                         relation.forwarding_occurrence);
    }
}

void print_allowed_values(
    const cli_authority::OptionValueContract& value) {
    for(std::size_t index = 0; index < value.allowed_value_count; ++index) {
        if(index != 0) std::cout << ',';
        std::cout << value.allowed_values[index];
    }
}

void print_option_token(OptionId id, std::string_view token) {
    const cli_authority::OptionContract& option =
        cli_authority::option_contract(id);
    std::cout << "OPTION\t" << enum_index(id) << '\t' << token << '\t';
    if(option.value.kind == cli_authority::OptionValueKind::AttachedEnum ||
       option.value.kind == cli_authority::OptionValueKind::AttachedValue) {
        std::cout << token << '=';
    } else {
        std::cout << token;
    }
    std::cout << '\t' << occurrence_name(option.default_occurrence)
              << '\t' << placement_name(option.lexical_placement) << '\t'
              << value_kind_name(option.value.kind) << '\t';
    print_allowed_values(option.value);
    std::cout << '\t' << conflict_rule_name(option.conflicts.rule) << '\t';
    print_conflicts(option.conflicts);
    std::cout << '\t' << option.conflicts.value_identity << '\t';
    print_semantic_scopes(option.semantic_scopes);
    std::cout << '\t' << ownership_name(option.owner) << '\t'
              << public_definition_role_name(
                     option.public_definition_role)
              << '\t'
              << completion_visibility_name(
                     option.completion_visibility)
              << '\n';
}

void print_option(OptionId id) {
    const cli_authority::OptionContract& option =
        cli_authority::option_contract(id);
    for(std::size_t index = 0; index < option.aliases.count; ++index) {
        print_option_token(id, option.aliases.values[index]);
    }
    print_option_token(id, option.canonical_token);
}

void print_form(
    std::string_view token, std::string_view syntax,
    const OperandContract& operands, TargetPolicy target_policy,
    const OperationOptionRelationSet& relations,
    cli_authority::DelegatedPacmanTailPolicy delegated_tail_policy =
        cli_authority::DelegatedPacmanTailPolicy::None) {
    std::cout << "FORM\t" << token << '\t' << syntax << '\t'
              << target_policy_name(target_policy) << '\t'
              << operand_ordering_name(operands.ordering) << '\t';
    print_operand_terms(operands);
    std::cout << '\t';
    print_relations(relations);
    std::cout << '\t';
    switch(delegated_tail_policy) {
        case cli_authority::DelegatedPacmanTailPolicy::None:
            std::cout << "none";
            break;
        case cli_authority::DelegatedPacmanTailPolicy::RepositoryOnly:
            std::cout << "repository-only";
            break;
    }
    std::cout << '\n';
}

template <std::size_t Size>
constexpr bool is_complete_option_projection_order(
    const std::array<OptionId, Size>& order) noexcept {
    constexpr std::size_t option_count = enum_index(OptionId::Count);
    if constexpr(Size != option_count) return false;

    std::array<bool, option_count> seen{};
    for(OptionId id : order) {
        const std::size_t index = enum_index(id);
        if(index >= seen.size() || seen[index]) return false;
        seen[index] = true;
    }
    return true;
}

} // namespace

int main() {
    constexpr std::array OPTION_ORDER = {
        OptionId::Help,
        OptionId::Version,
        OptionId::Edit,
        OptionId::NoEdit,
        OptionId::Diff,
        OptionId::NoDiff,
        OptionId::NoConfirm,
        OptionId::DryRun,
        OptionId::BuildMode,
        OptionId::Rebuild,
        OptionId::CleanBuild,
        OptionId::RmDeps,
        OptionId::Select,
        OptionId::Aur,
        OptionId::Repo,
        OptionId::Details,
        OptionId::LocalSource,
        OptionId::UseSourcePreference,
        OptionId::UsePatches,
        OptionId::PkgbuildOutputDirectory,
        OptionId::Recursive,
        OptionId::Needed,
        OptionId::EndOfOptions,
    };
    static_assert(is_complete_option_projection_order(OPTION_ORDER));
    for(OptionId id : OPTION_ORDER)
        print_option(id);

    for(OperationId id : cli_public_operation_order()) {
        const cli_authority::OperationMetadata& metadata =
            cli_authority::operation_metadata(id);
        std::cout << "OPERATION\t" << metadata.canonical_token
                  << "\tclosed\n";
        for(std::size_t form_index = 0;
            form_index < metadata.form_count; ++form_index) {
            const cli_authority::OperationFormSpec& form =
                cli_authority::operation_form(metadata, form_index);
            print_form(
                metadata.canonical_token,
                cli_operation_form_syntax(metadata, form),
                form.operands, form.target_policy,
                form.option_relations);
        }
    }

    std::string_view previous_special_token;
    for(SpecialOperationId id : cli_public_special_operation_order()) {
        const cli_authority::SpecialOperationSpec& operation =
            cli_authority::special_operation_spec(id);
        if(operation.canonical_token != previous_special_token) {
            std::cout << "OPERATION\t" << operation.canonical_token
                      << "\tclosed\n";
            previous_special_token = operation.canonical_token;
        }
        print_form(
            operation.canonical_token, cli_special_operation_syntax(id),
            operation.operands, operation.target_policy,
            operation.option_relations,
            operation.delegated_pacman_tail_policy);
    }

    constexpr std::array DELEGATED_EXAMPLE_SYNTAX = {
        cli_authority::PACMAN_SYNC_INSTALL_SYNTAX,
        cli_authority::PACMAN_SYSTEM_UPGRADE_SYNTAX,
        cli_authority::PACMAN_SYSTEM_UPGRADE_NO_REFRESH_SYNTAX,
        cli_authority::PACMAN_SYNC_SEARCH_SYNTAX,
        cli_authority::PACMAN_SYNC_INFO_SYNTAX,
        cli_authority::PACMAN_FOREIGN_UPDATES_SYNTAX,
    };
    for(std::string_view syntax : DELEGATED_EXAMPLE_SYNTAX) {
        const std::size_t separator = syntax.find(' ');
        const std::string_view token = syntax.substr(0, separator);
        std::cout << "OPERATION\t" << token << "\topen\n";
    }

    for(const auto& scope : cli_authority::DELEGATED_PRESENTATION_DETAIL_SCOPES) {
        std::cout << "PRESENTATION\t" << scope.operation << '\t'
                  << enum_index(OptionId::Details) << '\t';
        if(scope.requires_dry_run) std::cout << enum_index(OptionId::DryRun);
        std::cout << '\n';
    }

    const cli_authority::SpecialOperationSpec& delegated =
        cli_authority::special_operation_spec(
            SpecialOperationId::DelegatedPacmanGrammar);
    std::cout << "DELEGATED\t"
              << target_policy_name(delegated.target_policy) << '\t'
              << operand_ordering_name(delegated.operands.ordering) << '\t';
    print_operand_terms(delegated.operands);
    std::cout << '\t';
    print_relations(delegated.option_relations);
    std::cout << '\n';

    for(OptionId id : {OptionId::Help, OptionId::Version}) {
        const cli_authority::OptionContract& option =
            cli_authority::option_contract(id);
        for(std::size_t index = 0; index < option.aliases.count; ++index) {
            std::cout << "TERMINAL\t" << option.aliases.values[index]
                      << '\n';
        }
        std::cout << "TERMINAL\t" << option.canonical_token << '\n';
    }

    for(const std::string& syntax : cli_canonical_grammar()) {
        std::cout << "CANONICAL\t" << syntax << '\n';
    }
    return 0;
}
