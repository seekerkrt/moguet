#include "aur_rpc.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// WHY: inspection handler のloop / barrierだけをcharacterizeするため、real AUR transportを
// linkから外した専用binaryへ、この決定的なtest seamを差し込む。
namespace {

const char* INSPECTION_SCENARIO_ENV = "MOGUET_TEST_INSPECTION_SCENARIO";
const char* COMMAND_LOG_ENV = "MOGUET_TEST_COMMAND_LOG";

std::string required_environment_value(const char* name) {
    const char* value = std::getenv(name);
    if(value == nullptr || value[0] == '\0') {
        throw std::runtime_error(std::string("Missing test environment variable: ") + name);
    }
    return value;
}

std::string inspection_scenario() {
    return required_environment_value(INSPECTION_SCENARIO_ENV);
}

void append_command_log(const std::string& line) {
    std::ofstream log(required_environment_value(COMMAND_LOG_ENV), std::ios::app);
    if(!log) throw std::runtime_error("Failed to open inspection command log.");

    log << line << '\n';
    if(!log) throw std::runtime_error("Failed to write inspection command log.");
}

AurPackageInfo package_info(
    const std::string& name, const std::vector<std::string>& depends = {},
    const std::vector<std::string>& provides = {}) {
    AurPackageInfo info;
    info.Name = name;
    info.PackageBase = name;
    info.Version = "2.0-1";
    info.Description = "inspection characterization fixture";
    info.Depends = depends;
    info.Provides = provides;
    info.Maintainer = "moguet-test";
    return info;
}

bool is_graph_scenario(const std::string& scenario) {
    return !scenario.starts_with("foreign-authoritative-") && scenario != "foreign-fallback" &&
           scenario != "foreign-fallback-schema-failure" &&
           scenario != "foreign-ordinary-failure" &&
           scenario != "foreign-schema-failure" &&
           scenario != "foreign-order" &&
           scenario != "foreign-classification" &&
           scenario != "foreign-devel-requires-check";
}

bool is_numbered_foreign_package(const std::string& package_name) {
    if(package_name.size() != std::string("foreign-000").size()) return false;
    if(package_name.compare(0, std::string("foreign-").size(), "foreign-") != 0) return false;

    for(size_t i = std::string("foreign-").size(); i < package_name.size(); ++i) {
        if(package_name[i] < '0' || package_name[i] > '9') return false;
    }
    return package_name >= "foreign-001" && package_name <= "foreign-101";
}

bool is_expected_numbered_batch(
    const std::vector<std::string>& package_names, size_t expected_size,
    const std::string& expected_first, const std::string& expected_last) {
    return package_names.size() == expected_size && !package_names.empty() &&
           package_names.front() == expected_first && package_names.back() == expected_last;
}

std::optional<AurPackageInfo> graph_info(const std::string& package_name) {
    if(package_name == "deps-fail") throw std::runtime_error("fixture query failure");
    if(package_name == "plan-fail") throw std::runtime_error("fixture plan failure");

    if(package_name == "deps-first" || package_name == "deps-third" ||
       package_name == "plan-first" || package_name == "plan-third" ||
       package_name == "fetch-preflight-dep-one" ||
       package_name == "fetch-preflight-dep-two" ||
       package_name == "fetch-after-root" || package_name == "fetch-entry-fail" ||
       package_name == "fetch-entry-after" || package_name == "fetch-later-root") {
        return package_info(package_name);
    }

    if(package_name == "provider-root" || package_name == "deps-provider-root") {
        return package_info(package_name, {"moguet-inspect-203-virtual-provider"});
    }
    if(package_name == "provider-constraint-root") {
        return package_info(
            package_name,
            {"moguet-inspect-350-virtual-provider>=1"});
    }
    if(package_name == "moguet-inspect-203-virtual-provider") {
        return std::nullopt;
    }
    if(package_name == "moguet-inspect-350-virtual-provider") {
        return std::nullopt;
    }
    if(package_name == "provider-z" || package_name == "provider-a") {
        return package_info(
            package_name, {},
            {"moguet-inspect-203-virtual-provider",
             "moguet-inspect-350-virtual-provider=2"});
    }

    if(package_name == "fetch-preflight-root") {
        return package_info(
            package_name,
            {"fetch-preflight-dep-one", "fetch-preflight-dep-two"});
    }
    if(package_name == "fetch-guard-root") {
        return package_info(package_name, {"fetch-guard-root"});
    }
    if(package_name == "fetch-exec-root") {
        return package_info(package_name, {"fetch-entry-fail", "fetch-entry-after"});
    }

    if(package_name == "constraint-unsatisfied-root") {
        return package_info(package_name, {"constraint-leaf>=3"});
    }
    if(package_name == "constraint-satisfied-root") {
        return package_info(package_name, {"constraint-leaf>=2.0-1"});
    }
    if(package_name == "constraint-unconstrained-root") {
        return package_info(package_name, {"constraint-leaf"});
    }
    if(package_name == "constraint-invalid-root") {
        return package_info(package_name, {"constraint-leaf>="});
    }
    if(package_name == "constraint-leaf") {
        return package_info(package_name);
    }
    if(package_name == "plan-metadata-risk-root") {
        AurPackageInfo info = package_info(package_name);
        info.Conflicts = {"legacy-risk-package"};
        info.Replaces = {"replaced-risk-package"};
        return info;
    }
    if(package_name == "plan-split-child") {
        AurPackageInfo info = package_info(package_name);
        info.PackageBase = "plan-split-suite";
        return info;
    }
    if(package_name == "plan-density-root") {
        std::vector<std::string> dependencies;
        for(std::size_t index = 0; index < 32; ++index) {
            dependencies.push_back(
                "normal-dependency-" + std::to_string(index));
        }
        dependencies.push_back("attention-risk-child");
        return package_info(package_name, dependencies);
    }
    if(package_name.starts_with("normal-dependency-")) {
        return package_info(package_name);
    }
    if(package_name == "attention-risk-child") {
        AurPackageInfo info = package_info(package_name);
        info.Conflicts = {"attention-conflict"};
        return info;
    }
    if(package_name == "constraint-unknown-root") {
        return package_info(package_name, {"constraint-virtual>=3"});
    }
    if(package_name == "constraint-virtual") return std::nullopt;
    if(package_name == "constraint-provider") {
        return package_info(package_name, {}, {"constraint-virtual"});
    }

    if(package_name == "partial-provider-root") {
        return package_info(package_name, {"partial-provider-virtual>=1"});
    }
    if(package_name == "partial-provider-virtual") return std::nullopt;
    if(package_name == "partial-provider-a" ||
       package_name == "partial-provider-b") {
        return package_info(
            package_name, {}, {"partial-provider-virtual=2"});
    }

    if(package_name == "public-conflict-single-root") {
        AurPackageInfo info = package_info(
            package_name, {"public-conflict-virtual>=2"});
        info.MakeDepends = {"public-conflict-virtual<2"};
        return info;
    }
    if(package_name == "public-conflict-root-a") {
        return package_info(
            package_name, {"public-conflict-virtual>=2"});
    }
    if(package_name == "public-conflict-root-b") {
        return package_info(
            package_name, {"public-conflict-virtual<2"});
    }
    if(package_name == "public-conflict-virtual") return std::nullopt;

    if(package_name == "installed-display-root") {
        return package_info(package_name, {"foreign-installed"});
    }
    if(package_name == "installed-query-failure-root") {
        return package_info(
            package_name, {"installed-query-failure>=1"});
    }
    if(package_name == "foreign-installed" ||
       package_name == "installed-query-failure") {
        return std::nullopt;
    }

    if(package_name == "plan-formatter-root") {
        return package_info(
            package_name,
            {"format-0", "format-1023", "format-1024", "format-1152",
             "format-1536", "format-1048570", "format-1048571",
             "format-1048576", "format-991730", "format-5283285",
             "format-int64-max"});
    }

    if(package_name == "plan-result-root") {
        return package_info(
            package_name,
            {"result-zero", "result-missing", "result-query-failure",
             "result-malformed", "result-after-failure"});
    }
    if(package_name == "plan-result-later-root") {
        return package_info(package_name, {"result-later-target"});
    }

    if(package_name == "plan-identity-root") {
        AurPackageInfo info = package_info(
            package_name,
            {"same-package", "identity-same-virtual", "different-package",
             "identity-different-virtual", "same-semantic",
             "identity-stale-virtual",
             "identity-repository-aur-virtual", "identity-aur-virtual",
             "identity-ambiguous-virtual", "identity-unknown-virtual",
             "identity-aur-child"});
        info.MakeDepends = {"same-semantic"};
        return info;
    }

    if(package_name == "plan-no-metadata-root") {
        return package_info(
            package_name,
            {"no-metadata-aur-child", "no-metadata-aur-virtual",
             "ambiguous-only-virtual", "no-metadata-unknown-virtual"});
    }

    if(package_name == "plan-cache-first" ||
       package_name == "plan-cache-second") {
        return package_info(package_name, {"cache-shared"});
    }

    if(package_name == "plan-open-failure-first") {
        return package_info(package_name, {"open-first-package"});
    }
    if(package_name == "plan-open-failure-second") {
        return package_info(package_name, {"open-second-package"});
    }

    if(package_name == "identity-aur-child" ||
       package_name == "no-metadata-aur-child") {
        return package_info(package_name);
    }

    if(package_name == "identity-aur-provider") {
        return package_info(
            package_name, {}, {"identity-aur-virtual"});
    }
    if(package_name == "no-metadata-aur-provider") {
        return package_info(
            package_name, {}, {"no-metadata-aur-virtual"});
    }

    if(package_name == "identity-same-virtual" ||
       package_name == "identity-different-virtual" ||
       package_name == "identity-stale-virtual" ||
       package_name == "identity-repository-aur-virtual" ||
       package_name == "identity-aur-virtual" ||
       package_name == "identity-ambiguous-virtual" ||
       package_name == "identity-unknown-virtual" ||
       package_name == "no-metadata-aur-virtual" ||
       package_name == "ambiguous-only-virtual" ||
       package_name == "no-metadata-unknown-virtual") {
        return std::nullopt;
    }

    throw std::runtime_error("Unexpected inspection info call: " + package_name);
}

std::map<std::string, AurPackageInfo> foreign_info_many(
    const std::string& scenario, const std::vector<std::string>& package_names) {
    if(scenario == "foreign-fallback") {
        if(is_expected_numbered_batch(
               package_names, 100, "foreign-001", "foreign-100")) {
            return {};
        }
        if(is_expected_numbered_batch(
               package_names, 1, "foreign-101", "foreign-101")) {
            return {{"foreign-101", package_info("foreign-101")}};
        }
    } else if(scenario == "foreign-fallback-schema-failure") {
        if(is_expected_numbered_batch(
               package_names, 100, "foreign-001", "foreign-100")) {
            return {};
        }
    } else if(scenario == "foreign-ordinary-failure") {
        if(is_expected_numbered_batch(
               package_names, 100, "foreign-001", "foreign-100")) {
            throw std::runtime_error("ordinary batch failure");
        }
        if(is_expected_numbered_batch(
               package_names, 1, "foreign-101", "foreign-101")) {
            return {{"foreign-101", package_info("foreign-101")}};
        }
    } else if(scenario == "foreign-schema-failure") {
        if(is_expected_numbered_batch(
               package_names, 100, "foreign-001", "foreign-100")) {
            throw AurRpcResponseError("schema batch failure");
        }
    } else if(scenario == "foreign-order") {
        const std::vector<std::string> expected = {
            "foreign-order-z", "foreign-order-missing", "foreign-order-a"};
        if(package_names == expected) {
            return {
                {"foreign-order-z", package_info("foreign-order-z")},
                {"foreign-order-a", package_info("foreign-order-a")}};
        }
    } else if(scenario.starts_with("foreign-authoritative-")) {
        std::map<std::string, AurPackageInfo> result;
        for(const auto& name : package_names)
            result.emplace(name, package_info(name));
        return result;
    } else if(scenario == "foreign-classification") {
        const std::vector<std::string> expected = {
            "foreign-up-to-date", "foreign-non-aur"};
        if(package_names == expected) {
            return {{"foreign-up-to-date", package_info("foreign-up-to-date")}};
        }
    } else if(scenario == "foreign-devel-requires-check") {
        const std::vector<std::string> expected = {
            "split-cli", "split-child-git", "non-aur-git"};
        if(package_names == expected) {
            AurPackageInfo package_base_candidate = package_info("split-cli");
            package_base_candidate.PackageBase = "split-suite-git";
            AurPackageInfo child_candidate = package_info("split-child-git");
            child_candidate.PackageBase = "split-suite";
            return {
                {"split-cli", std::move(package_base_candidate)},
                {"split-child-git", std::move(child_candidate)},
            };
        }
    }

    throw std::runtime_error(
        "Unexpected inspection info_many call for scenario " + scenario);
}

} // namespace

CurlGlobal::CurlGlobal() = default;

CurlGlobal::~CurlGlobal() = default;

std::vector<AurPackageInfo> AurClient::search(const std::string& query) {
    append_command_log("aur search " + query);
    throw std::runtime_error("Unexpected inspection search call: " + query);
}

std::vector<AurPackageInfo> AurClient::search_strict(
    const std::string& query) {
    append_command_log("aur search-strict " + query);
    throw std::runtime_error(
        "Unexpected inspection strict search call: " + query);
}

std::vector<std::string> AurClient::search_names_by_provides(
    const std::string& provided_name) {
    append_command_log("aur search-provides " + provided_name);
    if(!is_graph_scenario(inspection_scenario())) {
        throw std::runtime_error(
            "Unexpected inspection search-provides call: " + provided_name);
    }
    if(provided_name == "moguet-inspect-203-virtual-provider") {
        // POLICY: AUR RPC order is significant to the provider presentation contract.
        return {"provider-z", "provider-a"};
    }
    if(provided_name == "moguet-inspect-350-virtual-provider") {
        return {"provider-z", "provider-a"};
    }
    if(provided_name == "identity-aur-virtual") {
        return {"identity-aur-provider"};
    }
    if(provided_name == "no-metadata-aur-virtual") {
        return {"no-metadata-aur-provider"};
    }
    if(provided_name == "constraint-virtual") {
        return {"constraint-provider"};
    }
    if(provided_name == "partial-provider-virtual") {
        return {"partial-provider-a", "partial-provider-b"};
    }
    if(provided_name == "foreign-installed" ||
       provided_name == "installed-query-failure") {
        return {};
    }
    if(provided_name == "identity-unknown-virtual" ||
       provided_name == "no-metadata-unknown-virtual") {
        return {};
    }
    throw std::runtime_error(
        "Unexpected inspection search-provides call: " + provided_name);
}

std::vector<std::string> AurClient::search_names_by_provides_strict(
    const std::string& provided_name) {
    return search_names_by_provides(provided_name);
}

std::optional<AurPackageInfo> AurClient::info(const std::string& package_name) {
    append_command_log("aur info " + package_name);
    const std::string scenario = inspection_scenario();

    if(is_graph_scenario(scenario)) return graph_info(package_name);

    throw std::runtime_error(
        "Unexpected inspection info call for scenario " + scenario + ": " +
        package_name);
}

std::optional<AurPackageInfo> AurClient::info_strict(const std::string& package_name) {
    append_command_log("aur info-strict " + package_name);
    const std::string scenario = inspection_scenario();

    if(is_graph_scenario(scenario)) {
        if(scenario.find("provider-partial-failure") !=
               std::string::npos &&
           package_name == "partial-provider-b") {
            throw std::runtime_error(
                "partial provider candidate metadata failure");
        }
        return graph_info(package_name);
    }

    if(scenario == "foreign-fallback" &&
       is_numbered_foreign_package(package_name) &&
       package_name != "foreign-101") {
        return package_info(package_name);
    }
    if(scenario == "foreign-fallback-schema-failure" &&
       package_name == "foreign-001") {
        throw AurRpcResponseError("schema fallback failure");
    }

    throw std::runtime_error(
        "Unexpected inspection info_strict call for scenario " + scenario +
        ": " + package_name);
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    std::string line = "aur info-many " + std::to_string(package_names.size());
    if(!package_names.empty()) {
        line += " " + package_names.front() + " " + package_names.back();
    }
    append_command_log(line);

    const std::string scenario = inspection_scenario();
    if(is_graph_scenario(scenario)) {
        throw std::runtime_error(
            "Unexpected inspection info_many call for scenario " + scenario);
    }
    return foreign_info_many(scenario, package_names);
}
