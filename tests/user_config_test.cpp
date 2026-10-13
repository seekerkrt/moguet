#include "cli_parser.hpp"
#include "user_config.hpp"

#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string_view review_policy_name(ReviewPolicy policy) {
    switch(policy) {
        case ReviewPolicy::Prompt:
            return "prompt";
        case ReviewPolicy::Skip:
            return "skip";
    }
    throw std::runtime_error("Unexpected ReviewPolicy value.");
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
    throw std::runtime_error("Unexpected BuildMode value.");
}

void print_config(const UserConfig& config) {
    std::cout << "SCHEMA_VERSION=" << config.schema_version << '\n'
              << "REVIEW_PKGBUILD="
              << review_policy_name(config.review.pkgbuild) << '\n'
              << "REVIEW_DIFF=" << review_policy_name(config.review.diff)
              << '\n'
              << "BUILD_MODE=" << build_mode_name(config.build.mode) << '\n';
}

bool same_config(const UserConfig& lhs, const UserConfig& rhs) {
    return lhs.schema_version == rhs.schema_version &&
           lhs.review.pkgbuild == rhs.review.pkgbuild &&
           lhs.review.diff == rhs.review.diff &&
           lhs.build.mode == rhs.build.mode;
}

void require_config(
    const UserConfig& config, ReviewPolicy pkgbuild,
    ReviewPolicy diff, BuildMode mode, std::string_view label) {
    if(config.schema_version != 1 || config.review.pkgbuild != pkgbuild ||
       config.review.diff != diff || config.build.mode != mode) {
        throw std::runtime_error(
            "Unexpected composed config for " + std::string(label) + ".");
    }
}

void verify_composition_contract() {
    const UserConfig defaults;
    require_config(
        compose_user_config(defaults, CliOverrides{}),
        ReviewPolicy::Prompt, ReviewPolicy::Prompt,
        BuildMode::Normal, "built-in defaults");

    UserConfig user_config;
    user_config.review.pkgbuild = ReviewPolicy::Skip;
    user_config.review.diff = ReviewPolicy::Skip;
    user_config.build.mode = BuildMode::Clean;
    const UserConfig original_user_config = user_config;

    require_config(
        compose_user_config(user_config, CliOverrides{}),
        ReviewPolicy::Skip, ReviewPolicy::Skip, BuildMode::Clean,
        "user values without CLI overrides");

    CliOverrides inverse_overrides;
    inverse_overrides.review_pkgbuild = ReviewPolicy::Prompt;
    inverse_overrides.review_diff = ReviewPolicy::Prompt;
    inverse_overrides.build_mode = BuildMode::Normal;
    require_config(
        compose_user_config(user_config, inverse_overrides),
        ReviewPolicy::Prompt, ReviewPolicy::Prompt,
        BuildMode::Normal, "inverse CLI overrides");

    CliOverrides enabled_overrides;
    enabled_overrides.review_pkgbuild = ReviewPolicy::Skip;
    enabled_overrides.review_diff = ReviewPolicy::Skip;
    enabled_overrides.build_mode = BuildMode::Rebuild;
    require_config(
        compose_user_config(defaults, enabled_overrides),
        ReviewPolicy::Skip, ReviewPolicy::Skip,
        BuildMode::Rebuild, "enabled CLI overrides");

    CliOverrides diff_only_override;
    diff_only_override.review_diff = ReviewPolicy::Prompt;
    require_config(
        compose_user_config(user_config, diff_only_override),
        ReviewPolicy::Skip, ReviewPolicy::Prompt, BuildMode::Clean,
        "single-field CLI override");

    CliOverrides invocation_only_overrides;
    invocation_only_overrides.no_confirm = true;
    invocation_only_overrides.rm_deps = true;
    require_config(
        compose_user_config(user_config, invocation_only_overrides),
        ReviewPolicy::Skip, ReviewPolicy::Skip, BuildMode::Clean,
        "invocation-only options");

    if(!same_config(user_config, original_user_config)) {
        throw std::runtime_error("Composition mutated its UserConfig input.");
    }
}

int run_test_driver(int argc, char* argv[]) {
    if(argc == 2 && std::string(argv[1]) == "defaults") {
        print_config(UserConfig{});
        return 0;
    }

    if(argc == 2 && std::string(argv[1]) == "composition") {
        verify_composition_contract();
        return 0;
    }

    if(argc == 3 && std::string(argv[1]) == "load") {
        print_config(load_user_config(std::filesystem::path(argv[2])));
        return 0;
    }

    std::cerr << "usage: user-config-test defaults | composition | load <path>"
              << std::endl;
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return run_test_driver(argc, argv);
    } catch(const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
