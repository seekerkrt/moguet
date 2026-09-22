#if __has_include("source_install.hpp")
#include "dependency_plan.hpp"
#include "source_install.hpp"
#define MOGUET_HAS_EXTRACTED_SOURCE_INSTALL 1
#else
#define MOGUET_HAS_EXTRACTED_SOURCE_INSTALL 0
#endif

// POLICY(#203,#242): production ownerのprepared invocationをdirect実行し、
// CLI wrapperを介さずall-target collectionとshared lifecycle接続をcharacterizeする。
#define main moguet_program_main
#include "../source/moguet.cpp"
#undef main

#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

AppConfig characterization_config() {
    AppConfig config;
    config.user_config.review.pkgbuild = ReviewPolicy::Skip;
    config.user_config.review.diff = ReviewPolicy::Skip;
    config.no_confirm = true;
    return config;
}

BuildPlan two_entry_plan() {
    BuildPlan plan;
    const RootTargetIdentity root_identity{0, "root-target"};

    plan.root_targets.push_back(root_identity);
    plan.package_targets.push_back(PlannedPackageTarget{
        "dep-target", "dep-target", {PackageRole::RuntimeDependency}, {root_identity}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "root-target", "root-target", {PackageRole::Root}, {root_identity}});

    plan.order.push_back(BuildPlanEntry{"dep-target", {"dep-target"}});
    plan.order.push_back(BuildPlanEntry{"root-target", {"root-target"}});
    return plan;
}

BuildPlan fallback_plan() {
    BuildPlan plan;
    const RootTargetIdentity root_identity{0, "base-target"};
    plan.root_targets.push_back(root_identity);
    plan.package_targets.push_back(PlannedPackageTarget{
        "base-target", "base-target", {PackageRole::Root}, {root_identity}});
    plan.order.push_back(BuildPlanEntry{"base-target", {"base-target"}});
    return plan;
}

void execute_characterized_plan(
    const BuildPlan& plan, bool use_source_build_preferences, bool needed,
    const AppConfig& config) {
#if MOGUET_HAS_EXTRACTED_SOURCE_INSTALL
    std::vector<ProductionSourceBuildWorkItem> work_items =
        prepare_aur_source_build_work_items(
            plan, use_source_build_preferences, needed);
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    execute_prepared_source_build_invocation(invocation, config);
#else
    static_cast<void>(config);
    SourceSyncOptions source_sync_options;
    source_sync_options.needed = needed;
    execute_aur_build_plan(plan, use_source_build_preferences, source_sync_options);
#endif
}

void install_characterized_smart_source(
    const std::string& package_name, bool only_if_updated, bool needed,
    const AppConfig& config) {
#if MOGUET_HAS_EXTRACTED_SOURCE_INSTALL
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.push_back(prepare_smart_source_build_work_item(
        package_name, only_if_updated, needed));
    PreparedProductionSourceBuildInvocation invocation =
        prepare_production_source_build_invocation(
            std::move(work_items), config);
    execute_prepared_source_build_invocation(invocation, config);
#else
    static_cast<void>(config);
    SourceSyncOptions source_sync_options;
    source_sync_options.needed = needed;
    install_smart_source(package_name, only_if_updated, source_sync_options);
#endif
}

int run_scenario(const std::string& scenario, const AppConfig& config) {
    if(scenario == "plan-success" || scenario == "plan-failure") {
        execute_characterized_plan(two_entry_plan(), false, true, config);
        return 0;
    }
    if(scenario == "fallback") {
        execute_characterized_plan(fallback_plan(), true, false, config);
        return 0;
    }
    if(scenario == "smart-source") {
        install_characterized_smart_source("clean-root", false, false, config);
        return 0;
    }
    if(scenario == "smart-source-missing-post-snapshot") {
        install_characterized_smart_source("clean-root", true, false, config);
        return 0;
    }

    std::cerr << "Unknown source-install characterization scenario: " << scenario << '\n';
    return 2;
}

} // namespace

int main(int argc, char* argv[]) {
    if(argc >= 2 && std::string(argv[1]) == "remote-rmdeps") {
        AppConfig config = characterization_config();
        config.no_confirm = argc > 2 && std::string(argv[2]) == "noconfirm";
        config.rm_deps = !(argc > 2 && std::string(argv[2]) == "unrequested");
        try {
            const auto result = build_source_target("cleanup-root", {}, config);
            std::cout << "typed-build-success=" << result.build_install.is_success() << '\n';
            if(result.cleanup_interaction) std::cout << "typed-interaction=" << static_cast<int>(result.cleanup_interaction->status()) << '\n';
            if(result.cleanup_execution) std::cout << "typed-execution=" << static_cast<int>(result.cleanup_execution->status) << '\n';
            return result.command_exit_status();
        } catch(const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if(argc != 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <plan-success|plan-failure|fallback|smart-source|"
                     "smart-source-missing-post-snapshot>\n";
        return 2;
    }

    AppConfig config = characterization_config();
#if !MOGUET_HAS_EXTRACTED_SOURCE_INSTALL
    // 抽出前実装はrunner-private configを参照するため、比較用processへ同じ設定をpublishする。
    g_config = config;
#endif

    try {
        return run_scenario(argv[1], config);
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
