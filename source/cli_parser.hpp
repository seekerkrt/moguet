#pragma once

#include "presentation_detail.hpp"
#include "user_config.hpp"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

// CLI parserが返すsource selector。semantic routeの解釈はconsumer側が所有する。
enum class PackageSourceSelection {
    Auto,
    AurOnly,
    RepoOnly,
};

enum class CliTokenRole {
    MoguetGlobalOption,
    Operation,
    PacmanOption,
    PacmanOptionValue,
    EndOfOptions,
    Target,
    OpaqueOperand,
};

struct ParsedCliToken {
    std::string value;
    std::size_t argv_index;
    CliTokenRole role;
};

// user config対象は最終値を保持し、invocation-only policyは永続化せず保持する。
struct CliOverrides {
    std::optional<ReviewPolicy> review_pkgbuild;
    std::optional<ReviewPolicy> review_diff;
    std::optional<BuildMode> build_mode;
    bool no_confirm = false;
    bool dry_run = false;
    bool rm_deps = false;
    PresentationDetail presentation_detail = PresentationDetail::Normal;
};

// CLI tokenの構文上の役割と、routing用view / pacman委譲用viewを同じparse結果に束ねる。
struct ParsedCliArguments {
    std::string operation;
    std::vector<ParsedCliToken> tokens;
    std::vector<std::string> ordered_pacman_args;
    std::vector<std::string> consumed_global_options;
    std::vector<std::string> flags;
    std::vector<std::string> targets;
    std::vector<std::size_t> target_token_indices;
    std::optional<std::string> pending_option;
    bool end_of_options = false;
    bool root_package_selection_requested = false;
    PackageSourceSelection source_selection = PackageSourceSelection::Auto;
    CliOverrides cli_overrides;
};

bool is_moguet_global_option(const std::string& arg);
bool pacman_option_takes_value(const std::string& arg);
// load_user_config()が解決したbuilt-in→userの結果へ、CLIの最終値だけを重ねるpure境界。
UserConfig compose_user_config(
    UserConfig user_config, const CliOverrides& cli_overrides);
// POLICY: global optionだけのargvもempty operationを持つparse成功として返す。
// usage表示とexit statusはrunnerが決める。
std::optional<ParsedCliArguments> parse_cli_arguments(int argc, char* argv[]);
std::vector<std::string> ordered_pacman_args_excluding_targets(
    const ParsedCliArguments& parsed,
    const std::set<std::size_t>& excluded_target_token_indices);
