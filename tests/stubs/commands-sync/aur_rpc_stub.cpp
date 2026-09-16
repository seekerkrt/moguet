#include "aur_rpc.hpp"

#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// WHY: sync handler の外部呼び出し順と継続境界を、real AUR transportから
// 切り離したisolated integration binaryで決定的にcharacterizeする。
namespace {

const char* COMMAND_LOG_ENV = "MOGUET_TEST_COMMAND_LOG";

int g_search_deferred_call_count = 0;

std::string required_environment_value(const char* name) {
    const char* value = std::getenv(name);
    if(value == nullptr || value[0] == '\0') {
        throw std::runtime_error(std::string("Missing test environment variable: ") + name);
    }
    return value;
}

void append_command_log(const std::string& line) {
    std::ofstream log(required_environment_value(COMMAND_LOG_ENV), std::ios::app);
    if(!log) throw std::runtime_error("Failed to open commands-sync command log.");

    log << line << '\n';
    if(!log) throw std::runtime_error("Failed to write commands-sync command log.");
}

AurPackageInfo package_info(const std::string& name) {
    AurPackageInfo info;
    info.Name = name;
    info.PackageBase = name;
    info.Version = "1.0-1";
    info.Description = "commands sync fixture";
    info.Maintainer = "moguet-test";
    return info;
}

AurPackageInfo package_info_with_base(
    const std::string& name, const std::string& package_base) {
    AurPackageInfo info = package_info(name);
    info.PackageBase = package_base;
    return info;
}

AurPackageInfo search_presentation_info() {
    AurPackageInfo info = package_info("search-presented");
    info.Version = "2.0-1";
    info.Description = "search presentation fixture";
    info.Maintainer.clear();
    info.OutOfDate = 1;
    return info;
}

std::optional<AurPackageInfo> fixture_info(const std::string& package_name) {
    if(package_name == "virtualbox-ext-oracle" && std::getenv("MOGUET_TEST_CROSS_SOURCE_TRANSITION_CASE") != nullptr) {
        const std::string scenario = std::getenv("MOGUET_TEST_CROSS_SOURCE_TRANSITION_CASE");
        if(scenario == "missing") return std::nullopt;
        if(scenario == "query-failure") throw std::runtime_error("fixture replacement query failure");
        auto info = package_info(package_name);
        info.Version = "7.2.18-1";
        if(const char* file = std::getenv("MOGUET_TEST_CROSS_SOURCE_PHASE_FILE")) {
            std::ifstream input(file);
            std::string phase;
            input >> phase;
            const char* detail = std::getenv("MOGUET_TEST_CROSS_SOURCE_CASE");
            if(detail && std::string(detail) == "aur-changed" && phase == "repository") info.Version = "7.2.20-1";
        }
        info.Depends = {scenario == "incompatible" ? "virtualbox=7.2.16" : "virtualbox=7.2.18"};
        const auto parsed = parse_dependency_requirement(info.Depends.front());
        info.constraint_metadata = AurPackageConstraintMetadata{
            info.Name, info.PackageBase, ObservedVersion::available(ObservedVersionSource::AurExactPackage, info.Version), {*parsed.requirement()}, {}, {}, {}, {}};
        return info;
    }
    if(package_name == "info-error") {
        throw std::runtime_error("fixture info failure");
    }
    if(package_name == "info-missing" || package_name == "plan-missing") {
        return std::nullopt;
    }

    if(package_name == "info-a" || package_name == "info-b" ||
       package_name == "info-installed" || package_name == "info-uninstalled" ||
       package_name == "plan-a" || package_name == "plan-b" ||
       package_name == "source-a" || package_name == "source-b" ||
       package_name == "forced-official" || package_name == "mixed-aur" ||
       package_name == "aur-presented" || package_name == "scope-aur" ||
       package_name == "system-update-a" ||
       package_name == "system-devel-git" ||
       package_name == "system-current" ||
       package_name == "tree-sitter-cli-git" ||
       package_name == "wezterm-git" ||
       package_name == "xpadneo-dkms-git" ||
       package_name == "system-required-git") {
        return package_info(package_name);
    }
    if(package_name == "system-required-root") {
        AurPackageInfo info = package_info(package_name);
        info.Depends = {"system-required-git"};
        return info;
    }
    if(package_name == "constraint-block-root") {
        AurPackageInfo info = package_info(package_name);
        info.Depends = {"constraint-block-leaf>=2.0-1"};
        return info;
    }
    if(package_name == "constraint-block-leaf") {
        return package_info(package_name);
    }
    if(package_name == "install-partial-root") {
        AurPackageInfo info = package_info(package_name);
        info.Depends = {"install-partial-virtual>=1"};
        return info;
    }
    if(package_name == "install-partial-provider-a" ||
       package_name == "install-partial-provider-b") {
        AurPackageInfo info = package_info(package_name);
        info.Provides = {"install-partial-virtual=2"};
        return info;
    }
    if(package_name == "install-conflict-root-a") {
        AurPackageInfo info = package_info(package_name);
        info.Depends = {"install-conflict-virtual>=2"};
        return info;
    }
    if(package_name == "install-conflict-root-b") {
        AurPackageInfo info = package_info(package_name);
        info.Depends = {"install-conflict-virtual<2"};
        return info;
    }
    if(package_name == "install-conflict-provider-a" ||
       package_name == "install-conflict-provider-b") {
        AurPackageInfo info = package_info(package_name);
        info.Provides = {"install-conflict-virtual=2"};
        return info;
    }
    if(package_name == "mismatch-child") {
        return package_info_with_base(package_name, "resolved-base");
    }
    if(package_name == "split-one" || package_name == "split-two") {
        return package_info_with_base(package_name, "split-suite");
    }

    return std::nullopt;
}

} // namespace

CurlGlobal::CurlGlobal() = default;

CurlGlobal::~CurlGlobal() = default;

std::vector<AurPackageInfo> AurClient::search(const std::string& query) {
    append_command_log("aur search " + query);

    if(query == "search-schema") {
        throw AurRpcResponseError("fixture search schema failure");
    }
    if(query == "search-deferred") {
        ++g_search_deferred_call_count;
        if(g_search_deferred_call_count == 1) {
            throw std::runtime_error("fixture deferred search failure");
        }
        return {package_info(query)};
    }
    if(query == "search-presented") return {search_presentation_info()};
    if(query == "search-hit-a" || query == "search-hit-b") {
        return {package_info(query)};
    }
    return {};
}

std::vector<AurPackageInfo> AurClient::search_strict(
    const std::string& query) {
    append_command_log("aur search-strict " + query);
    throw std::runtime_error(
        "Unexpected commands-sync strict search call: " + query);
}

std::vector<std::string> AurClient::search_names_by_provides(
    const std::string& provided_name) {
    append_command_log("aur provides " + provided_name);
    if(provided_name == "install-partial-virtual") {
        return {"install-partial-provider-a", "install-partial-provider-b"};
    }
    if(provided_name == "install-conflict-virtual") {
        return {"install-conflict-provider-a", "install-conflict-provider-b"};
    }
    return {};
}

std::vector<std::string> AurClient::search_names_by_provides_strict(
    const std::string& provided_name) {
    return search_names_by_provides(provided_name);
}

std::optional<AurPackageInfo> AurClient::info(const std::string& package_name) {
    append_command_log("aur info " + package_name);
    return fixture_info(package_name);
}

std::optional<AurPackageInfo> AurClient::info_strict(const std::string& package_name) {
    append_command_log("aur info-strict " + package_name);
    if(package_name == "install-partial-provider-b") {
        throw std::runtime_error(
            "partial provider candidate metadata failure");
    }
    auto result = fixture_info(package_name);
    if(package_name == "virtualbox-ext-oracle") {
        if(const char* file = std::getenv("MOGUET_TEST_CROSS_SOURCE_PHASE_FILE")) {
            std::ifstream input(file);
            std::string phase;
            input >> phase;
            if(phase == "initial") {
                std::ofstream output(file);
                output << "observed\n";
            }
        }
    }
    return result;
}

std::map<std::string, AurPackageInfo> AurClient::info_many(
    const std::vector<std::string>& package_names) {
    std::string line = "aur info-many";
    for(const auto& package_name : package_names)
        line += " " + package_name;
    append_command_log(line);

    for(const std::string& package_name : package_names) {
        if(package_name == "system-query-fatal") {
            throw AurRpcResponseError(
                std::string{"fixture fatal AUR schema failure\n"} +
                std::string{"\x1b", 1} + "unsafe\\detail");
        }
    }

    std::map<std::string, AurPackageInfo> package_by_name;
    for(const auto& package_name : package_names) {
        std::optional<AurPackageInfo> info = fixture_info(package_name);
        if(info.has_value()) package_by_name.emplace(package_name, std::move(*info));
    }
    return package_by_name;
}
