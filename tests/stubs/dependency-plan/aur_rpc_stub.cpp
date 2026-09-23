#include "aur_rpc.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::size_t g_case22_provider_info_queries = 0;
std::size_t g_recursive_selected_provider_info_queries = 0;
std::size_t g_selected_provider_identity_info_queries = 0;
std::size_t g_selected_provider_provides_info_queries = 0;
std::size_t g_selected_provider_metadata_info_queries = 0;
std::size_t g_unique_refresh_removal_info_queries = 0;
std::size_t g_unique_refresh_failure_info_queries = 0;
std::size_t g_unique_refresh_name_change_info_queries = 0;
std::size_t g_case33_b_info_queries = 0;

AurPackageInfo package_info(
    const std::string& name, const std::vector<std::string>& depends = {},
    const std::vector<std::string>& make_depends = {},
    const std::vector<std::string>& check_depends = {},
    const std::vector<std::string>& provides = {},
    const std::string& package_base = {}) {
    AurPackageInfo info;
    info.Name = name;
    info.PackageBase = package_base.empty() ? name : package_base;
    info.Version = "1.0-1";
    info.Description = "dependency plan model fixture";
    info.Depends = depends;
    info.MakeDepends = make_depends;
    info.CheckDepends = check_depends;
    info.Provides = provides;
    info.Maintainer = "moguet-test";
    return info;
}

bool is_leaf_package(const std::string& package_name) {
    const std::vector<std::string> leaves = {
        "case2-runtime-dep",
        "case2-build-dep",
        "case2-check-dep",
        "case3-shared",
        "case4-lib",
        "case5-common",
        "case7-provider-pkg",
        "case10-common",
        "case13-common",
        "case15-shared",
        "case16-a-only",
        "case16-b-only",
        "case16-shared",
        "case17-leaf",
        "case19-early-dep",
        "case19-late-dep",
        "case21-provider-child",
        "case34-child-a",
        "case34-child-b",
    };
    for(const auto& leaf : leaves) {
        if(package_name == leaf) return true;
    }
    return false;
}

} // namespace

namespace dependency_plan_aur_rpc_stub {

void reset_selected_provider_identity_queries() {
    g_selected_provider_identity_info_queries = 0;
    g_selected_provider_provides_info_queries = 0;
    g_selected_provider_metadata_info_queries = 0;
    g_unique_refresh_removal_info_queries = 0;
    g_unique_refresh_failure_info_queries = 0;
    g_unique_refresh_name_change_info_queries = 0;
}

} // namespace dependency_plan_aur_rpc_stub

std::vector<AurPackageInfo> AurClient::search(const std::string& query) {
    throw AurRpcResponseError("Unexpected dependency-plan AUR search: " + query);
}

std::vector<std::string> AurClient::search_names_by_provides(
    const std::string& provided_name) {
    if(provided_name == "case7-virtual-api") {
        return {"case7-provider-pkg", "case7-provider-pkg"};
    }
    if(provided_name == "case11-virtual") return {"case11-provider"};
    if(provided_name == "case21-virtual") {
        return {"case21-provider-a", "case21-provider-b"};
    }
    if(provided_name == "case33-virtual") {
        return {"case33-provider-a", "case33-provider-b"};
    }
    if(provided_name == "case34-virtual") {
        return {"case34-provider-a", "case34-provider-b"};
    }
    if(provided_name == "case22-virtual") {
        return {"case22-provider", "case22-provider"};
    }
    if(provided_name == "recursive-selected-provider-failure-virtual") {
        return {
            "recursive-selected-provider-failure-a",
            "recursive-selected-provider-failure-b"};
    }
    if(provided_name == "selected-provider-identity-virtual") {
        return {
            "selected-provider-identity-a",
            "selected-provider-identity-b"};
    }
    if(provided_name == "selected-provider-provides-virtual") {
        return {
            "selected-provider-provides-a",
            "selected-provider-provides-b"};
    }
    if(provided_name == "selected-provider-metadata-virtual") {
        return {
            "selected-provider-metadata-a",
            "selected-provider-metadata-b"};
    }
    if(provided_name == "selected-source-change-virtual") {
        return {"selected-source-change-provider"};
    }
    if(provided_name == "unique-refresh-removal-virtual") {
        return {"unique-refresh-removal-provider"};
    }
    if(provided_name == "unique-refresh-failure-virtual") {
        return {"unique-refresh-failure-provider"};
    }
    if(provided_name == "unique-refresh-name-change-virtual") {
        return {"unique-refresh-name-change-provider"};
    }
    if(provided_name ==
       "preflight-exact-failure-no-provider-fallback") {
        return {
            "preflight-exact-fallback-provider-a",
            "preflight-exact-fallback-provider-b"};
    }
    if(provided_name == "preflight-provider-candidate-virtual") {
        return {
            "preflight-provider-broken",
            "preflight-provider-good",
        };
    }
    if(provided_name == "preflight-provider-search-virtual") {
        throw std::runtime_error("legacy provider search failure");
    }
    if(provided_name == "preflight-provider-response-virtual") {
        throw AurRpcResponseError("provider search response failure");
    }
    if(provided_name == "preflight-provider-candidate-response-virtual") {
        return {"preflight-provider-candidate-response-broken"};
    }
    if(provided_name == "case9-missing" ||
       provided_name == "case11-missing" ||
       provided_name == "installed-present" ||
       provided_name == "installed-absent" ||
       provided_name == "installed-query-failure") {
        return {};
    }
    if(provided_name == "preflight-dependency-failure-child" ||
       provided_name == "preflight-shared-failure") {
        return {};
    }
    throw AurRpcResponseError(
        "Unexpected dependency-plan AUR provider search: " + provided_name);
}

std::vector<std::string> AurClient::search_names_by_provides_strict(
    const std::string& provided_name) {
    if(provided_name == "preflight-provider-search-virtual") {
        throw std::runtime_error("strict provider search failure");
    }
    if(provided_name == "preflight-provider-response-virtual") {
        throw AurRpcResponseError("provider search response failure");
    }
    return search_names_by_provides(provided_name);
}

std::optional<AurPackageInfo> AurClient::info(const std::string& package_name) {
    if(package_name == "case1-app") return package_info(package_name);

    if(package_name == "case2-app") {
        AurPackageInfo info = package_info(
            package_name, {"case2-runtime-dep>=2"}, {"case2-build-dep"},
            {"case2-check-dep"});
        info.OptDepends = {"case2-optional-dep"};
        return info;
    }
    if(is_leaf_package(package_name)) {
        if(package_name == "case7-provider-pkg") {
            return package_info(package_name, {}, {}, {}, {"case7-virtual-api"});
        }
        return package_info(package_name);
    }

    if(package_name == "case3-app") {
        return package_info(package_name, {"case3-shared"}, {"case3-shared"});
    }
    if(package_name == "case4-app") {
        return package_info(package_name, {"case4-lib"});
    }
    if(package_name == "case5-app" || package_name == "case5-tool") {
        return package_info(package_name, {"case5-common"});
    }
    if(package_name == "case6-app") {
        return package_info(package_name, {"case6-repo-lib"});
    }
    if(package_name == "case7-app") {
        return package_info(package_name, {"case7-virtual-api"});
    }
    if(package_name == "case8-app") {
        return package_info(package_name, {"case8-virtual"});
    }
    if(package_name == "case9-app") {
        return package_info(package_name, {"case9-missing"});
    }
    if(package_name == "case10-app") {
        return package_info(package_name, {"case10-lib"});
    }
    if(package_name == "case10-lib") {
        return package_info(package_name, {"case10-common"});
    }

    if(package_name == "case11-root") {
        AurPackageInfo info = package_info(
            package_name,
            {"case11-direct", "case11-virtual", "case11-ambiguous",
             "case11-missing", "case11-cycle-a"});
        info.Conflicts = {"case11-old"};
        return info;
    }
    if(package_name == "case11-direct") {
        AurPackageInfo info = package_info(
            package_name, {}, {}, {}, {}, "case11-direct-base");
        info.Replaces = {"case11-replaced"};
        return info;
    }
    if(package_name == "case11-provider") {
        return package_info(package_name, {}, {}, {}, {"case11-virtual"});
    }
    if(package_name == "case11-cycle-a") {
        return package_info(package_name, {"case11-root"});
    }

    if(package_name == "case13-app") {
        return package_info(package_name, {"case13-lib"});
    }
    if(package_name == "case13-lib") {
        return package_info(package_name, {"case13-common"});
    }
    if(package_name == "case14-app") {
        return package_info(package_name, {"case14-virtual"});
    }
    if(package_name == "case15-app") {
        return package_info(
            package_name, {"case15-shared"}, {"case15-shared"},
            {"case15-shared"});
    }

    if(package_name == "case16-child-a") {
        return package_info(
            package_name, {"case16-a-only", "case16-shared"}, {}, {}, {},
            "case16-suite");
    }
    if(package_name == "case16-child-b") {
        return package_info(
            package_name, {"case16-b-only", "case16-shared"}, {}, {}, {},
            "case16-suite");
    }

    if(package_name == "case17-parent") {
        return package_info(
            package_name, {"case17-sibling"}, {}, {}, {},
            "case17-suite");
    }
    if(package_name == "case17-sibling") {
        return package_info(
            package_name, {"case17-leaf"}, {}, {}, {},
            "case17-suite");
    }

    if(package_name == "case18-cycle-a") {
        return package_info(package_name, {"case18-cycle-b"});
    }
    if(package_name == "case18-cycle-b") {
        return package_info(package_name, {"case18-cycle-a"});
    }

    if(package_name == "case20-cycle-a") {
        return package_info(
            package_name, {"case20-cycle-b"}, {}, {}, {},
            "case20-suite");
    }
    if(package_name == "case20-cycle-b") {
        return package_info(
            package_name, {"case20-cycle-a"}, {}, {}, {},
            "case20-suite");
    }

    if(package_name == "case19-consumer") {
        return package_info(package_name, {"case19-suite-a"});
    }
    if(package_name == "case19-suite-a") {
        return package_info(
            package_name, {"case19-early-dep"}, {}, {}, {},
            "case19-suite");
    }
    if(package_name == "case19-suite-b") {
        return package_info(
            package_name, {"case19-late-dep"}, {}, {}, {},
            "case19-suite");
    }

    if(package_name == "case21-app") {
        return package_info(package_name, {"case21-virtual"});
    }
    if(package_name == "case21-provider-a") {
        return package_info(
            package_name, {}, {}, {}, {"case21-virtual=1"});
    }
    if(package_name == "case21-provider-b") {
        return package_info(
            package_name, {"case21-provider-child"}, {}, {},
            {"case21-virtual=2"}, "case21-provider-suite");
    }
    if(package_name == "case33-app") {
        return package_info(package_name, {"case33-virtual"});
    }
    if(package_name == "case33-provider-a" ||
       package_name == "case33-provider-b") {
        return package_info(package_name, {}, {}, {}, {"case33-virtual=1"});
    }
    if(package_name == "case34-app") {
        return package_info(package_name, {"case34-virtual"});
    }
    if(package_name == "case34-provider-a") {
        return package_info(package_name, {"case34-child-a"}, {}, {},
                            {"case34-virtual=1"}, "case34-suite");
    }
    if(package_name == "case34-provider-b") {
        return package_info(package_name, {"case34-child-b"}, {}, {},
                            {"case34-virtual=1"}, "case34-suite");
    }
    if(package_name == "case22-app") {
        return package_info(package_name, {"case22-virtual"});
    }
    if(package_name == "case23-app") {
        return package_info(package_name, {"case23-virtual"});
    }
    if(package_name == "case22-provider") {
        AurPackageInfo info = package_info(
            package_name, {}, {}, {}, {"case22-virtual"});
        if(g_case22_provider_info_queries++ > 0) info.Version = "2.0-1";
        return info;
    }
    if(package_name == "recursive-selected-provider-failure-a" ||
       package_name == "recursive-selected-provider-failure-b") {
        return package_info(
            package_name, {}, {}, {},
            {"recursive-selected-provider-failure-virtual"});
    }
    if(package_name == "selected-provider-identity-root") {
        return package_info(
            package_name, {"selected-provider-identity-virtual"});
    }
    if(package_name == "selected-provider-identity-a") {
        return package_info(
            package_name, {}, {}, {},
            {"selected-provider-identity-virtual"});
    }
    if(package_name == "selected-provider-identity-b") {
        return package_info(
            package_name, {}, {}, {},
            {"selected-provider-identity-virtual"},
            "selected-provider-original-base");
    }
    if(package_name == "selected-provider-provides-root") {
        return package_info(
            package_name, {"selected-provider-provides-virtual"});
    }
    if(package_name == "selected-provider-provides-a" ||
       package_name == "selected-provider-provides-b") {
        return package_info(
            package_name, {}, {}, {},
            {"selected-provider-provides-virtual=1"});
    }
    if(package_name == "selected-provider-metadata-root") {
        return package_info(
            package_name, {"selected-provider-metadata-virtual"});
    }
    if(package_name == "selected-provider-metadata-a" ||
       package_name == "selected-provider-metadata-b") {
        return package_info(
            package_name, {}, {}, {},
            {"selected-provider-metadata-virtual=1"});
    }
    if(package_name == "selected-source-change-root") {
        return package_info(
            package_name, {"selected-source-change-virtual"});
    }
    if(package_name == "selected-source-change-provider") {
        return package_info(
            package_name, {}, {}, {},
            {"selected-source-change-virtual=1"});
    }
    if(package_name == "unique-refresh-removal-root") {
        return package_info(
            package_name, {"unique-refresh-removal-virtual>=1"});
    }
    if(package_name == "unique-refresh-removal-provider") {
        return package_info(
            package_name, {}, {}, {},
            {"unique-refresh-removal-virtual=2"});
    }
    if(package_name == "unique-refresh-failure-root") {
        return package_info(
            package_name, {"unique-refresh-failure-virtual>=1"});
    }
    if(package_name == "unique-refresh-failure-provider") {
        return package_info(
            package_name, {}, {}, {},
            {"unique-refresh-failure-virtual=2"});
    }
    if(package_name == "unique-refresh-name-change-root") {
        return package_info(
            package_name, {"unique-refresh-name-change-virtual>=1"});
    }
    if(package_name == "unique-refresh-name-change-provider") {
        return package_info(
            package_name, {}, {}, {},
            {"unique-refresh-name-change-virtual=2"});
    }
    if(package_name == "conflict-single-root") {
        return package_info(
            package_name, {"conflict-virtual>=2"},
            {"conflict-virtual<2"});
    }
    if(package_name == "conflict-root-a") {
        return package_info(package_name, {"conflict-virtual>=2"});
    }
    if(package_name == "conflict-root-b") {
        return package_info(package_name, {"conflict-virtual<2"});
    }
    if(package_name == "target-metadata-root-a" ||
       package_name == "target-metadata-root-b") {
        return package_info(
            package_name, {"target-metadata-change-virtual"});
    }
    if(package_name == "installed-present-root") {
        return package_info(package_name, {"installed-present>=1"});
    }
    if(package_name == "installed-absent-root") {
        return package_info(package_name, {"installed-absent"});
    }
    if(package_name == "installed-query-failure-root") {
        return package_info(
            package_name, {"installed-query-failure>=1"});
    }
    if(package_name == "preflight-repository-partial-provider-root") {
        return package_info(
            package_name,
            {"preflight-repository-partial-provider-virtual>=1"});
    }
    if(package_name ==
       "preflight-exact-failure-no-provider-fallback-root") {
        return package_info(
            package_name,
            {"preflight-exact-failure-no-provider-fallback"});
    }
    if(package_name == "preflight-exact-fallback-provider-a" ||
       package_name == "preflight-exact-fallback-provider-b") {
        return package_info(
            package_name, {}, {}, {},
            {"preflight-exact-failure-no-provider-fallback"});
    }

    if(package_name == "preflight-root-metadata-failure") {
        throw std::runtime_error("legacy root metadata failure");
    }
    if(package_name == "preflight-root-not-found") return std::nullopt;
    if(package_name == "preflight-later-root") return package_info(package_name);
    if(package_name == "preflight-dependency-failure-root") {
        return package_info(package_name, {"preflight-dependency-failure-child"});
    }
    if(package_name == "preflight-dependency-failure-child") {
        throw std::runtime_error("legacy dependency metadata failure");
    }
    if(package_name == "preflight-provider-search-root") {
        return package_info(package_name, {"preflight-provider-search-virtual"});
    }
    if(package_name == "preflight-provider-candidate-root") {
        return package_info(package_name, {"preflight-provider-candidate-virtual"});
    }
    if(package_name == "preflight-provider-good") {
        return package_info(
            package_name, {}, {}, {},
            {"preflight-provider-candidate-virtual"});
    }
    if(package_name == "preflight-provider-broken") {
        throw std::runtime_error("legacy provider candidate failure");
    }
    if(package_name == "preflight-shared-root-a" ||
       package_name == "preflight-shared-root-b") {
        return package_info(package_name, {"preflight-shared-parent"});
    }
    if(package_name == "preflight-shared-parent") {
        return package_info(package_name, {"preflight-shared-failure"});
    }
    if(package_name == "preflight-shared-failure") {
        throw std::runtime_error("legacy shared dependency metadata failure");
    }
    if(package_name == "preflight-response-error-root") {
        throw AurRpcResponseError("root response failure");
    }
    if(package_name == "preflight-provider-response-root") {
        return package_info(package_name, {"preflight-provider-response-virtual"});
    }
    if(package_name == "preflight-dependency-response-root") {
        return package_info(package_name, {"preflight-dependency-response-child"});
    }
    if(package_name == "preflight-provider-candidate-response-root") {
        return package_info(
            package_name,
            {"preflight-provider-candidate-response-virtual"});
    }
    if(package_name == "preflight-repository-exact-failure-root") {
        return package_info(
            package_name,
            {"preflight-repository-exact-failure-child"});
    }
    if(package_name == "preflight-repository-provider-failure-root") {
        return package_info(
            package_name,
            {"preflight-repository-provider-failure-virtual"});
    }
    if(package_name == "preflight-response-must-stop-before-later-root") {
        throw std::runtime_error("response error did not stop later root resolution");
    }

    if(package_name == "case7-virtual-api" || package_name == "case8-virtual" ||
       package_name == "case9-missing" || package_name == "case11-virtual" ||
       package_name == "case11-ambiguous" || package_name == "case11-missing" ||
       package_name == "case14-virtual" || package_name == "case21-virtual" ||
       package_name == "case33-virtual" ||
       package_name == "case34-virtual" ||
       package_name == "case22-virtual" ||
       package_name == "case23-virtual" ||
       package_name == "recursive-selected-provider-failure-virtual" ||
       package_name == "selected-provider-identity-virtual" ||
       package_name == "selected-provider-provides-virtual" ||
       package_name == "selected-provider-metadata-virtual" ||
       package_name == "selected-source-change-virtual" ||
       package_name == "unique-refresh-removal-virtual" ||
       package_name == "unique-refresh-failure-virtual" ||
       package_name == "unique-refresh-name-change-virtual" ||
       package_name == "conflict-virtual" ||
       package_name == "target-metadata-change-virtual" ||
       package_name == "installed-present" ||
       package_name == "installed-absent" ||
       package_name == "installed-query-failure" ||
       package_name == "preflight-repository-partial-provider-virtual" ||
       package_name == "preflight-provider-search-virtual" ||
       package_name == "preflight-provider-candidate-virtual" ||
       package_name == "preflight-provider-response-virtual" ||
       package_name == "preflight-provider-candidate-response-virtual" ||
       package_name == "preflight-repository-provider-failure-virtual") {
        return std::nullopt;
    }

    throw AurRpcResponseError("Unexpected dependency-plan AUR info call: " + package_name);
}

std::optional<AurPackageInfo> AurClient::info_strict(const std::string& package_name) {
    if(package_name == "preflight-root-metadata-failure") {
        throw std::runtime_error("strict root metadata failure");
    }
    if(package_name == "preflight-dependency-failure-child") {
        throw std::runtime_error("strict dependency metadata failure");
    }
    if(package_name ==
       "preflight-exact-failure-no-provider-fallback") {
        throw std::runtime_error(
            "strict exact metadata failure before provider fallback");
    }
    if(package_name == "preflight-provider-broken") {
        throw std::runtime_error("strict provider candidate failure");
    }
    if(package_name == "preflight-shared-failure") {
        throw std::runtime_error("strict shared dependency metadata failure");
    }
    if(package_name == "preflight-dependency-response-child") {
        throw AurRpcResponseError("dependency info response failure");
    }
    if(package_name == "preflight-provider-candidate-response-broken") {
        throw AurRpcResponseError("provider candidate info response failure");
    }
    if(package_name == "recursive-selected-provider-failure-b" &&
       g_recursive_selected_provider_info_queries++ > 0) {
        throw std::runtime_error(
            "strict selected provider traversal metadata failure");
    }
    if(package_name == "case33-provider-b" &&
       g_case33_b_info_queries++ > 1) {
        throw std::runtime_error("selected set member refresh unavailable");
    }
    if(package_name == "selected-provider-identity-b") {
        std::optional<AurPackageInfo> result = info(package_name);
        if(g_selected_provider_identity_info_queries++ > 0) {
            result->PackageBase = "selected-provider-changed-base";
        }
        return result;
    }
    if(package_name == "selected-provider-provides-b") {
        std::optional<AurPackageInfo> result = info(package_name);
        if(g_selected_provider_provides_info_queries++ > 0) {
            result->Provides = {"different-virtual=1"};
        }
        return result;
    }
    if(package_name == "selected-provider-metadata-b") {
        std::optional<AurPackageInfo> result = info(package_name);
        if(g_selected_provider_metadata_info_queries++ > 0) {
            result->Version = "2.0-1";
            result->Provides = {"selected-provider-metadata-virtual=2"};
        }
        return result;
    }
    if(package_name == "unique-refresh-removal-provider") {
        std::optional<AurPackageInfo> result = info(package_name);
        if(g_unique_refresh_removal_info_queries++ > 0) {
            result->Provides = {"different-virtual=2"};
        }
        return result;
    }
    if(package_name == "unique-refresh-failure-provider") {
        if(g_unique_refresh_failure_info_queries++ > 0) {
            throw std::runtime_error(
                "transient selected provider metadata failure");
        }
        return info(package_name);
    }
    if(package_name == "unique-refresh-name-change-provider") {
        std::optional<AurPackageInfo> result = info(package_name);
        if(g_unique_refresh_name_change_info_queries++ > 0) {
            result->Name = "unique-refresh-changed-provider";
        }
        return result;
    }
    return info(package_name);
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    std::map<std::string, AurPackageInfo> package_by_name;
    for(const auto& package_name : package_names) {
        std::optional<AurPackageInfo> package = info(package_name);
        if(package.has_value()) package_by_name.emplace(package_name, package.value());
    }
    return package_by_name;
}
