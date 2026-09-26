#include "cli_routing.hpp"

#include "cli_authority.hpp"
#include "cli_runtime_contract.hpp"
#include "localization.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

// POLICY(#505,#554): composite candidateのsemantic authorityはexact tokenだけとする。
// cli_authority側の同綴りはpublic grammar identityであり、classifierの
// target/selector/option semanticsやparser allowlistを置き換えない。
constexpr std::string_view SYSTEM_UPDATE_WITH_REFRESH_OPERATION = "-Syu";
constexpr std::string_view SYSTEM_UPDATE_OPERATION = "-Su";

std::string package_source_selection_option(PackageSourceSelection selection) {
    switch(selection) {
        case PackageSourceSelection::Auto:
            throw std::logic_error(localization::translate_message(
                "Automatic source selection has no explicit option."));
        case PackageSourceSelection::AurOnly:
            return "--aur";
        case PackageSourceSelection::RepoOnly:
            return "--repo";
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package source selection."));
}

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

[[noreturn]] void reject_root_package_selection_invocation(
    const std::string& diagnostic) {
    throw std::invalid_argument(diagnostic);
}

[[noreturn]] void reject_local_source_build_invocation(
    const std::string& diagnostic) {
    throw std::invalid_argument(diagnostic);
}

[[noreturn]] void reject_pkgbuild_export_invocation(
    const std::string& diagnostic) {
    throw std::invalid_argument(diagnostic);
}

bool is_pkgbuild_output_directory_token(
    std::string_view token) noexcept {
    const std::string_view option =
        cli_authority::PKGBUILD_OUTPUT_DIRECTORY_OPTION;
    return token == option ||
           (token.size() > option.size() && token.starts_with(option) &&
            token[option.size()] == '=');
}

bool local_source_build_accepts_global_option(
    const std::string& option) {
    const cli_authority::GlobalOptionSpec* spec =
        cli_authority::find_moguet_global_option(option);
    if(spec == nullptr) return false;

    switch(spec->id) {
        case cli_authority::GlobalOptionId::Edit:
        case cli_authority::GlobalOptionId::NoEdit:
        case cli_authority::GlobalOptionId::NoConfirm:
        case cli_authority::GlobalOptionId::DryRun:
        case cli_authority::GlobalOptionId::BuildMode:
        case cli_authority::GlobalOptionId::Rebuild:
        case cli_authority::GlobalOptionId::CleanBuild:
            return true;
        case cli_authority::GlobalOptionId::Diff:
        case cli_authority::GlobalOptionId::NoDiff:
        case cli_authority::GlobalOptionId::RmDeps:
        case cli_authority::GlobalOptionId::Select:
        case cli_authority::GlobalOptionId::Aur:
        case cli_authority::GlobalOptionId::Repo:
        case cli_authority::GlobalOptionId::Details:
        case cli_authority::GlobalOptionId::Count:
            return false;
    }
    return false;
}

AutoSystemUpdatePacmanCompatibility
classify_auto_system_update_pacman_arguments(
    const ParsedCliArguments& parsed) {
    for(const ParsedCliToken& token : parsed.tokens) {
        switch(token.role) {
            case CliTokenRole::PacmanOption:
                if(token.value == "--needed") break;
                return IncompatibleAutoSystemUpdatePacmanArguments{
                    AutoSystemUpdatePacmanIncompatibilityKind::
                        UnsupportedOption,
                    token.value};
            case CliTokenRole::PacmanOptionValue:
            case CliTokenRole::EndOfOptions:
            case CliTokenRole::OpaqueOperand:
                return IncompatibleAutoSystemUpdatePacmanArguments{
                    AutoSystemUpdatePacmanIncompatibilityKind::
                        UnsupportedArgumentForm,
                    token.value};
            case CliTokenRole::MoguetGlobalOption:
            case CliTokenRole::Operation:
            case CliTokenRole::Target:
                break;
        }
    }
    return CompatibleAutoSystemUpdatePacmanArguments{};
}

[[noreturn]] void reject_local_source_build_operand_count() {
    // TRANSLATORS: Both placeholders are literal CLI syntax tokens.
    reject_local_source_build_invocation(
        localization::format_translated_message(
            "Operation {} requires exactly one {} operand.",
            "build --local", "<directory>"));
}

} // namespace

std::optional<PkgbuildExportMode> pkgbuild_export_mode(const ParsedCliArguments& parsed) {
    if(parsed.operation == cli_authority::PKGBUILD_EXPORT_OPERATION) return PkgbuildExportMode::Tree;
    if(parsed.operation == cli_authority::PKGBUILD_PRINT_OPERATION) return PkgbuildExportMode::PkgbuildStdout;
    return std::nullopt;
}

PkgbuildExportInvocation require_pkgbuild_export_invocation(
    const ParsedCliArguments& parsed) {
    const std::optional<PkgbuildExportMode> mode =
        pkgbuild_export_mode(parsed);
    if(!mode.has_value()) {
        // TRANSLATORS: The placeholder is the literal PKGBUILD artifact identity.
        throw std::logic_error(localization::format_translated_message(
            "{} export was not requested.", "PKGBUILD"));
    }

    if(parsed.targets.size() != 1) {
        // TRANSLATORS: The placeholders are literal CLI syntax tokens.
        reject_pkgbuild_export_invocation(
            localization::format_translated_message(
                "Operation {} requires exactly one {} operand.",
                parsed.operation, "<pkg>"));
    }

    PkgbuildExportInvocation invocation{
        mode.value(), parsed.targets.front(), std::nullopt};
    const std::string_view output_option =
        cli_authority::PKGBUILD_OUTPUT_DIRECTORY_OPTION;

    // POLICY(#173,#434): parserがsemantic PacmanOptionと確定した位置だけを
    // operation-local optionとして解釈する。option valueや`--`後の同名tokenは
    // 昇格させない。
    for(const ParsedCliToken& token : parsed.tokens) {
        if(token.role == CliTokenRole::Operation ||
           token.role == CliTokenRole::Target) {
            continue;
        }
        if(token.role == CliTokenRole::PacmanOption &&
           is_pkgbuild_output_directory_token(token.value)) {
            if(mode.value() != PkgbuildExportMode::Tree) {
                // TRANSLATORS: The placeholders are literal CLI tokens.
                reject_pkgbuild_export_invocation(
                    localization::format_translated_message(
                        "Unsupported option {} for operation {}.",
                        token.value, parsed.operation));
            }
            if(token.value == output_option) {
                // TRANSLATORS: Both placeholders are literal CLI option forms.
                reject_pkgbuild_export_invocation(
                    localization::format_translated_message(
                        "Option {} requires a non-empty attached value in the form {}.",
                        output_option, "--output-dir=DIR"));
            }
            if(invocation.output_directory.has_value()) {
                // TRANSLATORS: Both placeholders are literal CLI syntax tokens.
                reject_pkgbuild_export_invocation(
                    localization::format_translated_message(
                        "Option {} may be specified only once for operation {}.",
                        output_option, "-G"));
            }

            std::string value = token.value.substr(output_option.size() + 1);
            if(value.empty()) {
                // TRANSLATORS: Both placeholders are literal CLI option forms.
                reject_pkgbuild_export_invocation(
                    localization::format_translated_message(
                        "Option {} requires a non-empty attached value in the form {}.",
                        output_option, "--output-dir=DIR"));
            }
            invocation.output_directory = std::move(value);
            continue;
        }

        // TRANSLATORS: The placeholders are literal CLI tokens.
        reject_pkgbuild_export_invocation(
            localization::format_translated_message(
                "Unsupported option {} for operation {}.",
                token.value, parsed.operation));
    }

    return invocation;
}

bool parsed_has_semantic_pacman_option(
    const ParsedCliArguments& parsed, const std::string& option) {
    return std::any_of(parsed.tokens.begin(), parsed.tokens.end(), [&](const ParsedCliToken& token) {
        return token.role == CliTokenRole::PacmanOption && token.value == option;
    });
}

SourceSyncOptions parse_source_sync_options(const ParsedCliArguments& parsed) {
    SourceSyncOptions options;
    // POLICY(#173): option valueや`--`後のoperandをsemantic optionへ昇格させない。
    options.needed = parsed_has_semantic_pacman_option(parsed, "--needed");
    return options;
}

SourceSelectableSyncOperation source_selectable_sync_operation(const ParsedCliArguments& parsed) {
    if(!parsed.operation.starts_with("-S")) return SourceSelectableSyncOperation::Unsupported;

    bool has_search = false;
    bool has_info = false;
    bool has_unsupported_modifier = false;

    auto inspect_short_modifiers = [&](const std::string& option, size_t first_modifier) {
        if(option.size() <= first_modifier || option[0] != '-' || option.starts_with("--")) return;
        for(size_t i = first_modifier; i < option.size(); ++i) {
            switch(option[i]) {
                case 's':
                    has_search = true;
                    break;
                case 'i':
                    has_info = true;
                    break;
                case 'c':
                case 'g':
                case 'l':
                case 'u':
                    has_unsupported_modifier = true;
                    break;
                default:
                    break;
            }
        }
    };

    inspect_short_modifiers(parsed.operation, 2);
    for(const auto& token : parsed.tokens) {
        if(token.role != CliTokenRole::PacmanOption) continue;
        if(token.value == "--search")
            has_search = true;
        else if(token.value == "--info")
            has_info = true;
        else if(token.value == "--clean" || token.value == "--groups" ||
                token.value == "--list" || token.value == "--sysupgrade")
            has_unsupported_modifier = true;
        else
            inspect_short_modifiers(token.value, 1);
    }

    if(has_unsupported_modifier || (has_search && has_info)) {
        return SourceSelectableSyncOperation::Unsupported;
    }
    if(has_search) return SourceSelectableSyncOperation::Search;
    if(has_info) return SourceSelectableSyncOperation::Info;
    return SourceSelectableSyncOperation::Install;
}

SyncInvocationRouteClassification classify_sync_invocation_route(
    const ParsedCliArguments& parsed) {
    if((parsed.operation != SYSTEM_UPDATE_WITH_REFRESH_OPERATION &&
        parsed.operation != SYSTEM_UPDATE_OPERATION) ||
       !parsed.targets.empty()) {
        return OtherSyncRoute{};
    }

    switch(parsed.source_selection) {
        case PackageSourceSelection::Auto:
            return AutoSystemUpdateRouteCandidate{
                classify_auto_system_update_pacman_arguments(parsed),
                parsed.ordered_pacman_args,
                parsed_has_semantic_pacman_option(parsed, "--needed")};
        case PackageSourceSelection::RepoOnly:
            return RepoOnlySystemUpdateRouteCandidate{
                parsed.ordered_pacman_args,
                parsed_has_semantic_pacman_option(parsed, "--needed")};
        case PackageSourceSelection::AurOnly:
            return InvalidAurOnlySystemUpdateRoute{};
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package source selection."));
}

bool pacman_operation_requests_refresh(
    const std::string& operation, const std::vector<std::string>& flags) {
    auto short_option_requests_refresh = [](const std::string& option) {
        if(option.size() < 2 || option[0] != '-' || option[1] == '-') return false;
        return std::find(option.begin() + 1, option.end(), 'y') != option.end();
    };

    if(short_option_requests_refresh(operation)) return true;

    bool option_value_expected = false;
    // POLICY(#172): flags[0] は operation。残りは元の option 順で、値を取る option の次は判定対象外にする。
    for(size_t i = 1; i < flags.size(); ++i) {
        const std::string& flag = flags[i];
        if(option_value_expected) {
            option_value_expected = false;
            continue;
        }
        if(flag == "--") break;
        if(flag == "--refresh" || short_option_requests_refresh(flag)) return true;
        option_value_expected = pacman_option_takes_value(flag);
    }
    return false;
}

std::optional<std::string> validate_source_selection_operation(
    const ParsedCliArguments& parsed) {
    if(parsed.source_selection == PackageSourceSelection::Auto) return std::nullopt;

    // POLICY(#505): only the shared exact targetless classifier may promote
    // --repo into the repository-only system-update escape hatch. Other
    // refresh-bearing selector forms keep their previous rejection.
    if(parsed.source_selection == PackageSourceSelection::RepoOnly &&
       std::holds_alternative<RepoOnlySystemUpdateRouteCandidate>(
           classify_sync_invocation_route(parsed))) {
        return std::nullopt;
    }

    const std::string selector = package_source_selection_option(parsed.source_selection);
    const bool requests_refresh = pacman_operation_requests_refresh(parsed.operation, parsed.flags);
    SourceSelectableSyncOperation sync_operation = source_selectable_sync_operation(parsed);

    if(parsed.source_selection == PackageSourceSelection::AurOnly && requests_refresh) {
        // TRANSLATORS: The placeholders are literal CLI/program tokens.
        return localization::format_translated_message(
            "Cannot combine {} with {} refresh for operation {}.",
            "--aur", "pacman", parsed.operation);
    }
    if(sync_operation == SourceSelectableSyncOperation::Unsupported ||
       (sync_operation == SourceSelectableSyncOperation::Install && requests_refresh)) {
        // TRANSLATORS: The placeholders are a source selector description or
        // literal option and a literal CLI operation.
        return localization::format_translated_message(
            "{} is not supported for operation {}.", selector,
            parsed.operation);
    }
    return std::nullopt;
}

std::optional<std::string> unsupported_source_sync_option(
    const ParsedCliArguments& parsed) {
    // POLICY(#56): -S の y/u modifier は official repository update にだけ作用する。
    // AUR/source-build 側へ意味を移せない他の pacman option は、黙って無視せず build 前に止める。
    const std::string& operation = parsed.operation;
    if(operation.size() < 2 || operation[0] != '-' || operation[1] != 'S' ||
       !std::all_of(operation.begin() + 2, operation.end(), [](char modifier) {
           return modifier == 'y' || modifier == 'u';
       })) {
        return operation;
    }

    for(const auto& token : parsed.tokens) {
        if(token.role != CliTokenRole::PacmanOption) continue;
        if(token.value == "--needed") continue;
        return token.value;
    }
    return std::nullopt;
}

RootPackageSelectionInvocation require_root_package_selection_invocation(
    const ParsedCliArguments& parsed) {
    if(!parsed.root_package_selection_requested) {
        throw std::logic_error(localization::translate_message(
            "Root package selection was not requested."));
    }

    if(parsed.operation != "-S") {
        // TRANSLATORS: Both placeholders are literal CLI syntax tokens.
        reject_root_package_selection_invocation(
            localization::format_translated_message(
                "Option {} is supported only with plain {}.",
                "--select", "-S"));
    }
    if(parsed.cli_overrides.rm_deps) {
        // TRANSLATORS: Both placeholders are literal CLI option tokens.
        reject_root_package_selection_invocation(
            localization::format_translated_message(
                "Cannot combine {} and {}.",
                "--select", "--rmdeps"));
    }

    for(const ParsedCliToken& token : parsed.tokens) {
        switch(token.role) {
            case CliTokenRole::EndOfOptions:
                // TRANSLATORS: Both placeholders are literal CLI tokens.
                reject_root_package_selection_invocation(
                    localization::format_translated_message(
                        "Cannot use {} with {}.", "--", "--select"));
            case CliTokenRole::OpaqueOperand:
                // TRANSLATORS: The placeholders are a literal CLI option and an operand.
                reject_root_package_selection_invocation(
                    localization::format_translated_message(
                        "Opaque operand is not supported with {}: {}",
                        "--select", token.value));
            case CliTokenRole::PacmanOption:
                if(token.value != "--needed") {
                    // TRANSLATORS: The placeholders are literal CLI tokens.
                    reject_root_package_selection_invocation(
                        localization::format_translated_message(
                            "Unsupported option {} for {}.",
                            token.value, "-S --select"));
                }
                break;
            case CliTokenRole::PacmanOptionValue:
                // The owning pacman option is rejected before this token is reached.
                break;
            case CliTokenRole::MoguetGlobalOption:
            case CliTokenRole::Operation:
            case CliTokenRole::Target:
                break;
        }
    }

    if(parsed.targets.size() != 1 ||
       parsed.target_token_indices.size() != 1 ||
       parsed.tokens[parsed.target_token_indices.front()].role !=
           CliTokenRole::Target) {
        // TRANSLATORS: The placeholders are literal CLI syntax tokens.
        reject_root_package_selection_invocation(
            localization::format_translated_message(
                "Operation {} requires exactly one {} operand.",
                "-S --select", "<query>"));
    }

    const std::string_view query =
        trim_ascii_whitespace(parsed.targets.front());
    if(query.empty()) {
        reject_root_package_selection_invocation(
            localization::translate_message(
                "Root package search query must not be empty."));
    }
    if(std::any_of(
           query.begin(), query.end(), [](unsigned char character) {
               return std::iscntrl(character) != 0;
           })) {
        reject_root_package_selection_invocation(
            localization::translate_message(
                "Root package search query contains a control character."));
    }

    return RootPackageSelectionInvocation{
        std::string(query),
        parsed_has_semantic_pacman_option(parsed, "--needed")};
}

bool local_source_build_requested(const ParsedCliArguments& parsed) {
    // POLICY(#271): operation-local selectorはglobal optionへ昇格させず、
    // parserが確定したsemantic pacman-option位置のexact tokenだけを解釈する。
    return parsed_has_semantic_pacman_option(
        parsed, std::string(cli_authority::LOCAL_SOURCE_OPTION));
}

DryRunOperation classify_dry_run_operation(
    const ParsedCliArguments& parsed) {
    const SyncInvocationRouteClassification system_update_route =
        classify_sync_invocation_route(parsed);
    if(const auto* auto_candidate =
           std::get_if<AutoSystemUpdateRouteCandidate>(
               &system_update_route)) {
        return std::holds_alternative<
                   CompatibleAutoSystemUpdatePacmanArguments>(
                   auto_candidate->pacman_compatibility)
                   ? DryRunOperation::SyncSystemUpdate
                   : DryRunOperation::Unsupported;
    }
    if(std::holds_alternative<RepoOnlySystemUpdateRouteCandidate>(
           system_update_route)) {
        return DryRunOperation::SyncSystemUpdate;
    }
    if(std::holds_alternative<InvalidAurOnlySystemUpdateRoute>(
           system_update_route)) {
        return DryRunOperation::Unsupported;
    }

    if(parsed.operation.size() >= 2 &&
       parsed.operation[0] == '-' && parsed.operation[1] == 'S') {
        bool requests_system_update = false;
        for(std::size_t i = 2; i < parsed.operation.size(); ++i) {
            if(parsed.operation[i] != 'y' && parsed.operation[i] != 'u') {
                return DryRunOperation::Unsupported;
            }
            requests_system_update = true;
        }

        // `--needed` alone has an existing Moguet-owned install projection.
        // Separate pacman operation modifiers remain generic pass-through;
        // an owned system update is spelled in the operation token itself.
        for(const ParsedCliToken& token : parsed.tokens) {
            switch(token.role) {
                case CliTokenRole::PacmanOption:
                    if(token.value == "--needed") continue;
                    return DryRunOperation::Unsupported;
                case CliTokenRole::PacmanOptionValue:
                case CliTokenRole::EndOfOptions:
                case CliTokenRole::OpaqueOperand:
                    return DryRunOperation::Unsupported;
                case CliTokenRole::MoguetGlobalOption:
                case CliTokenRole::Operation:
                case CliTokenRole::Target:
                    break;
            }
        }

        if(parsed.source_selection == PackageSourceSelection::RepoOnly) {
            return DryRunOperation::Unsupported;
        }

        if(!requests_system_update && parsed.targets.empty() &&
           parsed.source_selection == PackageSourceSelection::Auto &&
           !parsed.root_package_selection_requested) {
            return DryRunOperation::Unsupported;
        }

        return requests_system_update
                   ? DryRunOperation::SyncSystemUpdate
                   : DryRunOperation::SyncInstall;
    }

    const ResolvedCliRuntimeContract runtime_contract =
        resolve_cli_runtime_contract(parsed);
    if(runtime_contract.operation == nullptr ||
       runtime_contract.operation->dry_run_support !=
           cli_authority::DryRunSupport::Supported) {
        return DryRunOperation::Unsupported;
    }
    if(parsed.root_package_selection_requested ||
       parsed.source_selection != PackageSourceSelection::Auto) {
        return DryRunOperation::Unsupported;
    }
    const bool has_unexpected_custom_token = std::any_of(
        parsed.tokens.begin(), parsed.tokens.end(),
        [](const ParsedCliToken& token) {
            return token.role != CliTokenRole::MoguetGlobalOption &&
                   token.role != CliTokenRole::Operation &&
                   token.role != CliTokenRole::Target;
        });

    switch(runtime_contract.operation->id) {
        case cli_authority::OperationId::Build: {
            const bool local_build = local_source_build_requested(parsed);
            std::size_t local_selector_count = 0;
            std::size_t preference_option_count = 0;
            for(const ParsedCliToken& token : parsed.tokens) {
                switch(token.role) {
                    case CliTokenRole::PacmanOption:
                        if(local_build &&
                           token.value == cli_authority::LOCAL_SOURCE_OPTION) {
                            ++local_selector_count;
                            break;
                        }
                        if(!local_build &&
                           token.value == cli_authority::USE_SOURCE_PREFERENCE_OPTION) {
                            ++preference_option_count;
                            break;
                        }
                        return DryRunOperation::Unsupported;
                    case CliTokenRole::PacmanOptionValue:
                    case CliTokenRole::EndOfOptions:
                    case CliTokenRole::OpaqueOperand:
                        return DryRunOperation::Unsupported;
                    case CliTokenRole::MoguetGlobalOption:
                    case CliTokenRole::Operation:
                    case CliTokenRole::Target:
                        break;
                }
            }
            if((local_build && local_selector_count != 1) ||
               preference_option_count > 1) {
                return DryRunOperation::Unsupported;
            }
            return local_build ? DryRunOperation::LocalBuild
                               : DryRunOperation::RemoteBuild;
        }
        case cli_authority::OperationId::Fetch:
            return has_unexpected_custom_token
                       ? DryRunOperation::Unsupported
                       : DryRunOperation::Fetch;
        case cli_authority::OperationId::Upgrade:
            return has_unexpected_custom_token
                       ? DryRunOperation::Unsupported
                       : DryRunOperation::Upgrade;
        case cli_authority::OperationId::UpgradeAur:
            return has_unexpected_custom_token
                       ? DryRunOperation::Unsupported
                       : DryRunOperation::UpgradeAur;
        case cli_authority::OperationId::UpgradeAll:
            return has_unexpected_custom_token
                       ? DryRunOperation::Unsupported
                       : DryRunOperation::UpgradeAll;
        case cli_authority::OperationId::Clean:
        case cli_authority::OperationId::Deps:
        case cli_authority::OperationId::Plan:
        case cli_authority::OperationId::AddSource:
        case cli_authority::OperationId::DeleteSource:
        case cli_authority::OperationId::Revert:
        case cli_authority::OperationId::EditSource:
        case cli_authority::OperationId::ListSources:
        case cli_authority::OperationId::AddPatch:
        case cli_authority::OperationId::UpdatePatch:
        case cli_authority::OperationId::DeletePatch:
        case cli_authority::OperationId::ListPatch:
        case cli_authority::OperationId::Count:
            return DryRunOperation::Unsupported;
    }
    return DryRunOperation::Unsupported;
}

LocalSourceBuildInvocation require_local_source_build_invocation(
    const ParsedCliArguments& parsed) {
    if(!local_source_build_requested(parsed)) {
        throw std::logic_error(localization::translate_message(
            "Local source build was not requested."));
    }

    const std::string build_operation(
        cli_authority::operation_spec(
            cli_authority::OperationId::Build)
            .token);
    const std::string local_source_option(
        cli_authority::LOCAL_SOURCE_OPTION);
    if(parsed.operation != build_operation) {
        // TRANSLATORS: Both placeholders are literal CLI tokens.
        reject_local_source_build_invocation(
            localization::format_translated_message(
                "Option {} is supported only with operation {}.",
                local_source_option, build_operation));
    }

    const std::size_t selector_count = static_cast<std::size_t>(
        std::count_if(
            parsed.tokens.begin(), parsed.tokens.end(),
            [&](const ParsedCliToken& token) {
                return token.role == CliTokenRole::PacmanOption &&
                       token.value == local_source_option;
            }));
    if(selector_count != 1) {
        // TRANSLATORS: Both placeholders are literal CLI syntax tokens.
        reject_local_source_build_invocation(
            localization::format_translated_message(
                "Option {} may be specified only once for operation {}.",
                local_source_option, build_operation));
    }

    LocalSourceBuildInvocation invocation;
    bool has_directory = false;

    for(const ParsedCliToken& token : parsed.tokens) {
        switch(token.role) {
            case CliTokenRole::MoguetGlobalOption:
                if(!local_source_build_accepts_global_option(token.value)) {
                    // TRANSLATORS: The placeholders are literal CLI syntax tokens.
                    reject_local_source_build_invocation(
                        localization::format_translated_message(
                            "Unsupported option {} for {}.",
                            token.value, "build --local"));
                }
                break;
            case CliTokenRole::PacmanOption:
                if(token.value == cli_authority::USE_PATCHES_OPTION) {
                    invocation.use_patches = true;
                    break;
                }
                if(token.value != local_source_option) {
                    // TRANSLATORS: The placeholders are literal CLI syntax tokens.
                    reject_local_source_build_invocation(
                        localization::format_translated_message(
                            "Unsupported option {} for {}.",
                            token.value, "build --local"));
                }
                break;
            case CliTokenRole::PacmanOptionValue:
                // The owning pacman option is rejected before this token is reached.
                break;
            case CliTokenRole::EndOfOptions:
                // TRANSLATORS: Both placeholders are literal CLI syntax tokens.
                reject_local_source_build_invocation(
                    localization::format_translated_message(
                        "Cannot use {} with {}.", "--",
                        "build --local"));
            case CliTokenRole::OpaqueOperand:
                // TRANSLATORS: The placeholders are literal CLI syntax and operand tokens.
                reject_local_source_build_invocation(
                    localization::format_translated_message(
                        "Opaque operand is not supported with {}: {}",
                        "build --local", token.value));
            case CliTokenRole::Target: {
                std::string key;
                std::string value;
                const bool is_assignment =
                    split_env_assignment(token.value, key, value);
                if(!has_directory) {
                    if(is_assignment) {
                        // TRANSLATORS: The placeholder is a literal environment assignment.
                        reject_local_source_build_invocation(
                            localization::format_translated_message(
                                "Environment assignment requires a preceding directory: {}",
                                token.value));
                    }
                    if(token.value.find('=') != std::string::npos) {
                        // TRANSLATORS: The placeholder is a literal environment assignment.
                        reject_local_source_build_invocation(
                            localization::format_translated_message(
                                "Invalid environment assignment: {}",
                                token.value));
                    }
                    invocation.directory = token.value;
                    has_directory = true;
                    break;
                }

                if(is_assignment) {
                    invocation.source_environment.ordered_assignments.push_back(
                        SourceEnvironmentAssignment{
                            std::move(key), std::move(value)});
                    break;
                }
                if(token.value.find('=') != std::string::npos) {
                    // TRANSLATORS: The placeholder is a literal environment assignment.
                    reject_local_source_build_invocation(
                        localization::format_translated_message(
                            "Invalid environment assignment: {}",
                            token.value));
                }
                reject_local_source_build_operand_count();
            }
            case CliTokenRole::Operation:
                break;
        }
    }

    if(!has_directory) reject_local_source_build_operand_count();
    return invocation;
}
