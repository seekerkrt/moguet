#include "stubs/local-dependency-plan/query_stub.hpp"

#include "repository_query.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using local_dependency_plan_query_stub::RepositoryQuery;
using local_dependency_plan_query_stub::RepositoryQueryKind;

struct RepositoryPackageResponse {
    std::optional<std::string> repository_name;
    std::string package_base;
};

std::map<std::string, RepositoryPackageResponse> g_package_responses;
std::map<std::string, std::string> g_package_failures;
std::map<std::string, std::vector<ProvidedDependency>> g_provider_responses;
std::vector<RepositoryQuery> g_query_history;

const RepositoryPackageResponse& require_package_response(
    const std::string& package_name) {
    const auto response = g_package_responses.find(package_name);
    if(response == g_package_responses.end()) {
        throw std::runtime_error(
            "Unexpected local dependency plan repository package query: " +
            package_name);
    }
    return response->second;
}

const std::vector<ProvidedDependency>& require_provider_response(
    const std::string& dependency_name) {
    const auto response = g_provider_responses.find(dependency_name);
    if(response == g_provider_responses.end()) {
        throw std::runtime_error(
            "Unexpected local dependency plan repository provider query: " +
            dependency_name);
    }
    return response->second;
}

} // namespace

namespace local_dependency_plan_query_stub {

void reset_repository_stub() {
    g_package_responses.clear();
    g_package_failures.clear();
    g_provider_responses.clear();
    g_query_history.clear();
}

void set_repository_package_response(
    std::string package_name,
    std::optional<std::string> repository_name) {
    const std::string package_base = package_name;
    set_repository_package_response(
        std::move(package_name), std::move(repository_name),
        package_base);
}

void set_repository_package_response(
    std::string package_name,
    std::optional<std::string> repository_name,
    std::string package_base) {
    g_package_failures.erase(package_name);
    g_package_responses.insert_or_assign(
        std::move(package_name),
        RepositoryPackageResponse{
            std::move(repository_name),
            std::move(package_base)});
}

void set_repository_package_failure(
    std::string package_name, std::string diagnostic) {
    g_package_responses.erase(package_name);
    g_package_failures.insert_or_assign(
        std::move(package_name), std::move(diagnostic));
}

void set_repository_provider_response(
    std::string dependency_name,
    std::vector<ProvidedDependency> providers) {
    g_provider_responses.insert_or_assign(
        std::move(dependency_name), std::move(providers));
}

const std::vector<RepositoryQuery>& repository_query_history() {
    return g_query_history;
}

std::size_t repository_query_count(
    RepositoryQueryKind kind, const std::string& subject) {
    return static_cast<std::size_t>(std::count(
        g_query_history.begin(), g_query_history.end(),
        RepositoryQuery{kind, subject}));
}

} // namespace local_dependency_plan_query_stub

StrictRepositoryPackageQueryResult query_repository_package_strict(
    const std::string& package_name) {
    g_query_history.push_back(
        RepositoryQuery{RepositoryQueryKind::StrictPackage, package_name});
    const auto failure = g_package_failures.find(package_name);
    if(failure != g_package_failures.end()) {
        return RepositoryMetadataFailure{
            RepositoryMetadataFailureKind::SyncDatabaseUnavailable,
            std::nullopt, failure->second};
    }
    const RepositoryPackageResponse& response =
        require_package_response(package_name);
    if(response.repository_name.has_value()) {
        return RepositoryPackagePresent{
            response.repository_name.value(), 0, package_name,
            response.package_base,
            ObservedVersion::available(
                ObservedVersionSource::RepositoryExactPackage,
                "1.0-1")};
    }
    return RepositoryPackageNotFound{};
}

StrictRepositoryPackageQueryResult query_repository_package_strict(
    const PacmanRepositoryConfiguration&,
    const std::string& package_name) {
    return query_repository_package_strict(package_name);
}

InstalledExactPackageObservationResult query_installed_exact_package_strict(
    const std::string& package_name) {
    return InstalledExactPackageAbsent{package_name};
}

StrictRepositoryProvidersQueryResult query_repository_providers_strict(
    const std::string& dependency_name) {
    g_query_history.push_back(RepositoryQuery{
        RepositoryQueryKind::StrictProviders, dependency_name});
    return RepositoryProviderQuerySnapshot{
        require_provider_response(dependency_name), {}};
}
