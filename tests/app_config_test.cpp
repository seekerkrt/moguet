#include "../source/app_config.hpp"

#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

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

void print_config(const AppConfig& config) {
    std::cout << "SCHEMA_VERSION=" << config.user_config.schema_version << '\n'
              << "REVIEW_PKGBUILD="
              << review_policy_name(config.user_config.review.pkgbuild) << '\n'
              << "REVIEW_DIFF="
              << review_policy_name(config.user_config.review.diff) << '\n'
              << "BUILD_MODE="
              << build_mode_name(config.user_config.build.mode) << '\n'
              << "NOCONFIRM=" << (config.no_confirm ? "true" : "false") << '\n'
              << "RMDEPS=" << (config.rm_deps ? "true" : "false") << '\n'
              << "EDITOR=" << config.editor << '\n';
}

void expect(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

void test_provider_presentation_receives_invocation_detail() {
    for(const PresentationDetail detail : {PresentationDetail::Normal, PresentationDetail::Detailed}) {
        std::istringstream input("2\n");
        std::ostringstream output;
        AppConfig config;
        config.presentation_detail = detail;
        config.provider_selection = std::make_shared<ProviderSelectionSession>(input, output, true);
        std::optional<PresentationDetail> received_detail;
        config.provider_candidate_presenter_factory = [&](PresentationDetail received) {
            received_detail = received;
            return make_default_provider_candidate_presenter(received);
        };
        const std::vector<ProvidedDependency> candidates = {
            ProvidedDependency::from_repository("extra", "repo-provider", "virtual", "virtual=1", "1.0"),
            ProvidedDependency::from_aur("aur-provider", "aur-base", "virtual", "virtual=1", "1.0")};
        auto callback = provider_selection_callback(config);
        expect(received_detail == detail, "provider factory lost invocation presentation detail");
        const auto selected = callback("virtual", candidates);
        expect(selected.has_value() && selected.value() == candidates[1],
               "presentation detail changed callback selection identity");
        expect(output.str().find("2) source=AUR package=aur-provider PackageBase=aur-base") != std::string::npos,
               "callback lost rich provider metadata");
    }
}

int run_test_driver(int argc, char* argv[]) {
    if(argc == 2 && std::string(argv[1]) == "defaults") {
        AppConfig config;
        expect(config.presentation_detail == PresentationDetail::Normal,
               "default AppConfig detail is not Normal");
        expect(
            !provider_selection_callback(config),
            "default AppConfig unexpectedly exposed a provider callback");
        print_config(config);
        return 0;
    }

    if(argc == 2 && std::string(argv[1]) == "projection") {
        test_provider_presentation_receives_invocation_detail();
        UserConfig final_user_config;
        final_user_config.review.pkgbuild = ReviewPolicy::Skip;
        final_user_config.review.diff = ReviewPolicy::Skip;
        final_user_config.build.mode = BuildMode::Clean;
        AppConfig config = make_app_config(
            std::move(final_user_config), true, true);
        expect(
            config.provider_selection != nullptr,
            "composed AppConfig has no provider selection session");
        expect(
            !config.provider_selection->is_interactive(),
            "--noconfirm AppConfig has an interactive provider session");
        expect(config.presentation_detail == PresentationDetail::Normal,
               "default snapshot detail is not Normal");
        const AppConfig details_config = make_app_config(
            config.user_config, config.no_confirm, config.rm_deps, PresentationDetail::Detailed);
        expect(details_config.presentation_detail == PresentationDetail::Detailed,
               "AppConfig snapshot lost Detailed detail");
        expect(details_config.user_config.schema_version == config.user_config.schema_version &&
                   details_config.user_config.review.pkgbuild == config.user_config.review.pkgbuild &&
                   details_config.user_config.review.diff == config.user_config.review.diff &&
                   details_config.user_config.build.mode == config.user_config.build.mode &&
                   details_config.no_confirm == config.no_confirm &&
                   details_config.rm_deps == config.rm_deps &&
                   details_config.editor == config.editor &&
                   details_config.provider_selection->is_interactive() == config.provider_selection->is_interactive(),
               "Presentation detail changed AppConfig execution settings");
        expect(AppConfig(details_config).presentation_detail == PresentationDetail::Detailed,
               "AppConfig copy lost invocation presentation detail");
        AppConfig copied_config = config;
        expect(
            copied_config.provider_selection == config.provider_selection,
            "AppConfig copy did not share the invocation provider session");
        expect(
            static_cast<bool>(provider_selection_callback(copied_config)),
            "composed AppConfig did not expose a provider callback");
        print_config(config);
        return 0;
    }

    std::cerr << "usage: app-config-test defaults | projection" << std::endl;
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
