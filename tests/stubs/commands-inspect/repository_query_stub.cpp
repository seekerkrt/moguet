#include "repository_query.hpp"

#include "dependency_provider.hpp"
#include "package_identifier.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Explicit fixture membership is independent of subprocess exit status.
bool fixture_contains_package(const char* variable, const std::string& package_name) {
    const char* names = std::getenv(variable);
    if(names == nullptr) return false;
    std::istringstream stream(names);
    std::string name;
    while(stream >> name) {
        if(name == package_name) return true;
    }
    return false;
}

} // namespace

StrictRepositoryPackageQueryResult query_repository_package_strict(
    const std::string& package_name) {
    if(fixture_contains_package("MOGUET_TEST_INSPECTION_REPOSITORY_FAILURE_PACKAGES", package_name)) {
        return RepositoryMetadataFailure{
            RepositoryMetadataFailureKind::SyncDatabaseUnavailable,
            std::nullopt, "inspection repository metadata failure"};
    }
    if(fixture_contains_package("MOGUET_TEST_PACMAN_REPO_PACKAGES", package_name)) {
        return RepositoryPackagePresent{
            "test", 0, package_name, package_name,
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
    const char* scenario = std::getenv("MOGUET_TEST_INSPECTION_SCENARIO");
    if(scenario != nullptr &&
       std::string(scenario) == "deps-installed-query-failure" &&
       package_name == "installed-query-failure") {
        return InstalledExactPackageQueryFailure{
            package_name,
            PackageMetadataFailure{
                PackageMetadataErrorCode::QueryFailed,
                "installed database query failure"}};
    }
    if(fixture_contains_package("MOGUET_TEST_PACMAN_INSTALLED_PACKAGES", package_name)) {
        return InstalledExactPackage{
            package_name,
            ObservedVersion::unknown(
                ObservedVersionSource::InstalledExactPackage,
                ObservedVersionUnknownReason::MissingVersionMetadata)};
    }
    return InstalledExactPackageAbsent{package_name};
}

namespace {

std::vector<ProvidedDependency> repository_provider_candidates(
    const std::string& dependency_name) {
    if(!is_valid_package_name(dependency_name)) return {};

    if(dependency_name == "identity-same-virtual") {
        return {ProvidedDependency::from_repository(
            "extra", "same-package", dependency_name,
            dependency_name, std::nullopt)};
    }
    if(dependency_name == "identity-different-virtual") {
        return {ProvidedDependency::from_repository(
            "extra", "different-package", dependency_name,
            dependency_name, std::nullopt)};
    }
    if(dependency_name == "identity-stale-virtual") {
        return {ProvidedDependency::from_repository(
            "stale", "stale-package", dependency_name,
            dependency_name, std::nullopt)};
    }
    if(dependency_name == "identity-repository-aur-virtual") {
        return {ProvidedDependency::from_repository(
            "aur", "repository-aur-package", dependency_name,
            dependency_name, std::nullopt)};
    }
    if(dependency_name == "identity-ambiguous-virtual" ||
       dependency_name == "ambiguous-only-virtual") {
        return {
            ProvidedDependency::from_repository(
                "core", "ambiguous-provider-a", dependency_name,
                dependency_name, std::nullopt),
            ProvidedDependency::from_repository(
                "extra", "ambiguous-provider-b", dependency_name,
                dependency_name, std::nullopt)};
    }
    if(dependency_name == "public-conflict-virtual") {
        return {
            ProvidedDependency::from_repository(
                "core", "public-conflict-provider-a",
                "public-conflict-virtual",
                "public-conflict-virtual=2",
                std::optional<std::string>{"2.0-1"}),
            ProvidedDependency::from_repository(
                "extra", "public-conflict-provider-b",
                "public-conflict-virtual",
                "public-conflict-virtual=3",
                std::optional<std::string>{"3.0-1"}),
        };
    }

    // AUR provider/unknown fixturesと既存provider-order fixtureは、AUR seamへ委譲する。
    return {};
}

} // namespace

StrictRepositoryProvidersQueryResult query_repository_providers_strict(
    const std::string& dependency_name) {
    return RepositoryProviderQuerySnapshot{
        repository_provider_candidates(dependency_name), {}};
}
