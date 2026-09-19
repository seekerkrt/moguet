#include "cli_parser.hpp"

#include "application_identity.hpp"
#include "cli_authority.hpp"
#include "localization.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

std::optional<cli_authority::GlobalOptionId> moguet_global_option_kind(
    const std::string& argument) {
    const cli_authority::GlobalOptionSpec* option =
        cli_authority::find_moguet_global_option(argument);
    if(option == nullptr) return std::nullopt;
    return option->id;
}

void report_parse_error(const std::string& message) {
    // POLICY: parse failureはLogger初期化前に出す。既存のpre-log stderr形式をここで維持する。
    // TRANSLATORS: The placeholder is a complete CLI parsing diagnostic.
    std::cerr << "\033[1;31m::\033[0m "
              << localization::format_translated_message(
                     "Error: {}", message)
              << std::endl;
}

std::string_view review_policy_name(ReviewPolicy policy) {
    switch(policy) {
        case ReviewPolicy::Prompt:
            return "prompt";
        case ReviewPolicy::Skip:
            return "skip";
    }
    throw std::logic_error(
        localization::translate_message("Unknown review policy."));
}

std::string_view build_mode_name(BuildMode mode) {
    switch(mode) {
        case BuildMode::Normal:
            return "normal";
        case BuildMode::Rebuild:
            return "rebuild";
        case BuildMode::Clean:
            return "clean";
    }
    throw std::logic_error(
        localization::translate_message("Unknown build mode."));
}

template <typename Value, typename ValueName>
bool apply_final_value_override(
    std::optional<Value>& current, Value requested,
    std::string_view setting, ValueName value_name) {
    if(!current.has_value()) {
        current = requested;
        return true;
    }
    if(current.value() == requested) return true;

    // TRANSLATORS: The placeholders are a literal configuration key and two
    // literal configuration values.
    report_parse_error(localization::format_translated_message(
        "Conflicting CLI overrides for {}: values '{}' and '{}' were both requested.",
        setting, value_name(current.value()), value_name(requested)));
    return false;
}

bool apply_build_mode_option(
    const std::string& arg, ParsedCliArguments& parsed) {
    const std::string_view option =
        cli_authority::global_option_spec(
            cli_authority::GlobalOptionId::BuildMode)
            .token;
    if(arg == option) {
        // TRANSLATORS: All placeholders are literal CLI tokens.
        report_parse_error(localization::format_translated_message(
            "Option {} requires an attached value: {}, {}, or {}.",
            option, cli_authority::BUILD_MODE_NORMAL,
            cli_authority::BUILD_MODE_REBUILD,
            cli_authority::BUILD_MODE_CLEAN));
        return false;
    }

    const std::string_view value =
        std::string_view(arg).substr(option.size() + 1);
    std::optional<BuildMode> mode;
    if(value == cli_authority::BUILD_MODE_NORMAL)
        mode = BuildMode::Normal;
    else if(value == cli_authority::BUILD_MODE_REBUILD)
        mode = BuildMode::Rebuild;
    else if(value == cli_authority::BUILD_MODE_CLEAN)
        mode = BuildMode::Clean;

    if(!mode.has_value()) {
        // TRANSLATORS: All placeholders are literal CLI values or tokens.
        report_parse_error(localization::format_translated_message(
            "Invalid value for {}: '{}'; expected {}, {}, or {}.",
            option, value, cli_authority::BUILD_MODE_NORMAL,
            cli_authority::BUILD_MODE_REBUILD,
            cli_authority::BUILD_MODE_CLEAN));
        return false;
    }
    return apply_final_value_override(
        parsed.cli_overrides.build_mode, mode.value(), "build.mode",
        build_mode_name);
}

bool apply_moguet_global_option(const std::string& arg, ParsedCliArguments& parsed) {
    std::optional<cli_authority::GlobalOptionId> option =
        moguet_global_option_kind(arg);
    if(!option.has_value()) {
        // TRANSLATORS: The placeholders are the project identity and a literal CLI token.
        throw std::logic_error(localization::format_translated_message(
            "Unknown {} global option: {}",
            application_identity::PROJECT_NAME,
            arg));
    }

    switch(option.value()) {
        case cli_authority::GlobalOptionId::Edit:
            return apply_final_value_override(
                parsed.cli_overrides.review_pkgbuild,
                ReviewPolicy::Prompt, "review.pkgbuild",
                review_policy_name);
        case cli_authority::GlobalOptionId::NoEdit:
            return apply_final_value_override(
                parsed.cli_overrides.review_pkgbuild,
                ReviewPolicy::Skip, "review.pkgbuild",
                review_policy_name);
        case cli_authority::GlobalOptionId::Diff:
            return apply_final_value_override(
                parsed.cli_overrides.review_diff,
                ReviewPolicy::Prompt, "review.diff", review_policy_name);
        case cli_authority::GlobalOptionId::NoDiff:
            return apply_final_value_override(
                parsed.cli_overrides.review_diff,
                ReviewPolicy::Skip, "review.diff", review_policy_name);
        case cli_authority::GlobalOptionId::NoConfirm:
            parsed.cli_overrides.no_confirm = true;
            break;
        case cli_authority::GlobalOptionId::DryRun:
            parsed.cli_overrides.dry_run = true;
            break;
        case cli_authority::GlobalOptionId::BuildMode:
            return apply_build_mode_option(arg, parsed);
        case cli_authority::GlobalOptionId::Rebuild:
            return apply_final_value_override(
                parsed.cli_overrides.build_mode, BuildMode::Rebuild,
                "build.mode", build_mode_name);
        case cli_authority::GlobalOptionId::CleanBuild:
            return apply_final_value_override(
                parsed.cli_overrides.build_mode, BuildMode::Clean,
                "build.mode", build_mode_name);
        case cli_authority::GlobalOptionId::RmDeps:
            parsed.cli_overrides.rm_deps = true;
            break;
        case cli_authority::GlobalOptionId::Details:
            parsed.cli_overrides.presentation_detail = PresentationDetail::Detailed;
            break;
        case cli_authority::GlobalOptionId::Select:
            parsed.root_package_selection_requested = true;
            break;
        case cli_authority::GlobalOptionId::Aur:
            if(parsed.source_selection == PackageSourceSelection::RepoOnly) {
                // TRANSLATORS: Both placeholders are literal CLI options.
                report_parse_error(localization::format_translated_message(
                    "Cannot combine {} and {}.", "--aur", "--repo"));
                return false;
            }
            parsed.source_selection = PackageSourceSelection::AurOnly;
            break;
        case cli_authority::GlobalOptionId::Repo:
            if(parsed.source_selection == PackageSourceSelection::AurOnly) {
                // TRANSLATORS: Both placeholders are literal CLI options.
                report_parse_error(localization::format_translated_message(
                    "Cannot combine {} and {}.", "--aur", "--repo"));
                return false;
            }
            parsed.source_selection = PackageSourceSelection::RepoOnly;
            break;
        case cli_authority::GlobalOptionId::Count:
            throw std::logic_error(localization::format_translated_message(
                "Invalid {} global option authority entry.",
                application_identity::PROJECT_NAME));
    }
    return true;
}

} // namespace

bool is_moguet_global_option(const std::string& arg) {
    return moguet_global_option_kind(arg).has_value();
}

UserConfig compose_user_config(
    UserConfig user_config, const CliOverrides& cli_overrides) {
    if(cli_overrides.review_pkgbuild.has_value()) {
        user_config.review.pkgbuild =
            cli_overrides.review_pkgbuild.value();
    }
    if(cli_overrides.review_diff.has_value()) {
        user_config.review.diff = cli_overrides.review_diff.value();
    }
    if(cli_overrides.build_mode.has_value()) {
        user_config.build.mode = cli_overrides.build_mode.value();
    }
    return user_config;
}

bool pacman_option_takes_value(const std::string& arg) {
    // POLICY: 固定の pacman option table。process lifetime の保持だが mutable な副作用は持たない。
    static const std::vector<std::string> s_long_opts = {
        "--arch", "--assume-installed", "--cachedir", "--color", "--config", "--dbpath",
        "--gpgdir", "--hookdir", "--ignore", "--ignoregroup", "--logfile", "--overwrite",
        "--print-format", "--root", "--sysroot"};
    static const std::vector<std::string> s_short_opts = {"-b", "-r"};

    if(arg.find('=') != std::string::npos) return false;
    if(std::find(s_long_opts.begin(), s_long_opts.end(), arg) != s_long_opts.end()) return true;
    return std::find(s_short_opts.begin(), s_short_opts.end(), arg) != s_short_opts.end();
}

std::optional<ParsedCliArguments> parse_cli_arguments(int argc, char* argv[]) {
    ParsedCliArguments parsed;
    bool has_operation = false;

    for(int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if(arg.empty()) {
            report_parse_error(localization::translate_message(
                "Empty arguments are not supported."));
            return std::nullopt;
        }

        if(!has_operation) {
            if(is_moguet_global_option(arg)) {
                if(!apply_moguet_global_option(arg, parsed)) return std::nullopt;
                parsed.tokens.push_back(
                    ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::MoguetGlobalOption});
                parsed.consumed_global_options.push_back(arg);
                continue;
            }

            has_operation = true;
            parsed.operation = arg;
            parsed.tokens.push_back(
                ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::Operation});
            parsed.ordered_pacman_args.push_back(arg);
            parsed.flags.push_back(arg);
            continue;
        }

        // POLICY(#173): pacmanの構文状態を確定してから、通常位置のMoguet optionだけを消費する。
        if(parsed.pending_option.has_value()) {
            parsed.tokens.push_back(
                ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::PacmanOptionValue});
            parsed.ordered_pacman_args.push_back(arg);
            parsed.flags.push_back(arg);
            parsed.pending_option.reset();
            continue;
        }
        if(parsed.end_of_options) {
            std::size_t token_index = parsed.tokens.size();
            parsed.tokens.push_back(
                ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::OpaqueOperand});
            parsed.ordered_pacman_args.push_back(arg);
            parsed.targets.push_back(arg);
            parsed.target_token_indices.push_back(token_index);
            continue;
        }
        if(arg == "--") {
            parsed.tokens.push_back(
                ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::EndOfOptions});
            parsed.ordered_pacman_args.push_back(arg);
            parsed.flags.push_back(arg);
            parsed.end_of_options = true;
            continue;
        }
        if(is_moguet_global_option(arg)) {
            if(!apply_moguet_global_option(arg, parsed)) return std::nullopt;
            parsed.tokens.push_back(
                ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::MoguetGlobalOption});
            parsed.consumed_global_options.push_back(arg);
            continue;
        }
        if(arg[0] == '-') {
            parsed.tokens.push_back(
                ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::PacmanOption});
            parsed.ordered_pacman_args.push_back(arg);
            parsed.flags.push_back(arg);
            if(pacman_option_takes_value(arg)) parsed.pending_option = arg;
            continue;
        }

        std::size_t token_index = parsed.tokens.size();
        parsed.tokens.push_back(
            ParsedCliToken{arg, static_cast<std::size_t>(i), CliTokenRole::Target});
        parsed.ordered_pacman_args.push_back(arg);
        parsed.targets.push_back(arg);
        parsed.target_token_indices.push_back(token_index);
    }

    if(parsed.pending_option.has_value()) {
        // TRANSLATORS: The placeholder is a literal CLI option.
        report_parse_error(localization::format_translated_message(
            "Missing value for option {}.",
            parsed.pending_option.value()));
        return std::nullopt;
    }
    return parsed;
}

std::vector<std::string> ordered_pacman_args_excluding_targets(
    const ParsedCliArguments& parsed,
    const std::set<std::size_t>& excluded_target_token_indices) {
    std::vector<std::string> args;
    for(std::size_t i = 0; i < parsed.tokens.size(); ++i) {
        const ParsedCliToken& token = parsed.tokens[i];
        if(token.role == CliTokenRole::MoguetGlobalOption) continue;
        if((token.role == CliTokenRole::Target || token.role == CliTokenRole::OpaqueOperand) &&
           excluded_target_token_indices.contains(i)) {
            continue;
        }
        args.push_back(token.value);
    }
    return args;
}
