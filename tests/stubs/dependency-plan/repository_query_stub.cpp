#include "repository_query.hpp"

#include "dependency_provider.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::size_t g_sync_database_package_queries = 0;
std::size_t g_strict_repo_provider_queries = 0;
std::size_t g_target_metadata_provider_queries = 0;
std::size_t g_source_change_provider_queries = 0;

std::vector<ProvidedDependency> repository_providers(
    const std::string& dependency_name) {
    const auto provider = [](
                              std::string repository,
                              std::string package,
                              std::string package_base,
                              std::string capability,
                              std::string package_version) {
        const std::size_t equals = capability.find('=');
        ProviderCapability parsed(
            capability, capability.substr(0, equals),
            equals == std::string::npos
                ? std::nullopt
                : std::optional<std::string>(
                      capability.substr(equals + 1)));
        const ObservedVersion provided_version = parsed.version().has_value()
                                                     ? ObservedVersion::available(
                                                           ObservedVersionSource::RepositoryProviderCapability,
                                                           parsed.version().value())
                                                     : ObservedVersion::unknown(
                                                           ObservedVersionSource::RepositoryProviderCapability,
                                                           ObservedVersionUnknownReason::UnversionedProviderCapability);
        return ProvidedDependency::from_repository_constraint_metadata(
            std::move(repository), std::move(package),
            std::move(package_base), "x86_64",
            ProviderConstraintMetadata{
                std::move(parsed),
                ObservedVersion::available(
                    ObservedVersionSource::RepositoryExactPackage,
                    std::move(package_version)),
                provided_version});
    };
    if(dependency_name == "case8-virtual") {
        return {
            provider("extra", "case8-provider-a", "case8-provider-a-base", "case8-virtual=2", "2.0-1"),
            provider("community", "case8-provider-b", "case8-provider-b-base", "case8-virtual=3", "3.0-1"),
        };
    }
    if(dependency_name == "case11-ambiguous") {
        return {
            provider("extra", "case11-provider-a", "case11-provider-a-base", "case11-ambiguous=1", "1.0-1"),
            provider("community", "case11-provider-b", "case11-provider-b-base", "case11-ambiguous=2", "2.0-1"),
        };
    }
    if(dependency_name == "case14-virtual") {
        return {provider(
            "aur", "case14-provider", "case14-provider-base", "case14-virtual=1", "1.0-1")};
    }
    if(dependency_name == "case23-virtual") {
        ProviderCapability capability(
            "case23-virtual=6", "case23-virtual",
            std::optional<std::string>{"6"});
        return {ProvidedDependency::from_repository_constraint_metadata(
            "extra", 1, "case23-provider", "case23-provider-base",
            "x86_64",
            ProviderConstraintMetadata{
                capability,
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryExactPackage,
                    "5.0-1"),
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryProviderCapability,
                    "6")})};
    }
    if(dependency_name == "conflict-virtual") {
        return {
            provider(
                "extra", "conflict-provider-a",
                "conflict-provider-a-base",
                "conflict-virtual=2", "2.0-1"),
            provider(
                "community", "conflict-provider-b",
                "conflict-provider-b-base",
                "conflict-virtual=3", "3.0-1"),
        };
    }
    if(dependency_name == "case7-virtual-api" || dependency_name == "case9-missing" ||
       dependency_name == "case11-virtual" || dependency_name == "case11-missing" ||
       dependency_name == "case21-virtual" ||
       dependency_name == "case22-virtual" ||
       dependency_name == "recursive-selected-provider-failure-virtual" ||
       dependency_name == "selected-provider-identity-virtual" ||
       dependency_name == "selected-provider-provides-virtual" ||
       dependency_name == "selected-provider-metadata-virtual" ||
       dependency_name == "selected-source-change-virtual" ||
       dependency_name == "unique-refresh-removal-virtual" ||
       dependency_name == "unique-refresh-failure-virtual" ||
       dependency_name == "unique-refresh-name-change-virtual" ||
       dependency_name == "installed-present" ||
       dependency_name == "installed-absent" ||
       dependency_name == "installed-query-failure" ||
       dependency_name ==
           "preflight-exact-failure-no-provider-fallback" ||
       dependency_name == "preflight-dependency-failure-child" ||
       dependency_name == "preflight-provider-search-virtual" ||
       dependency_name == "preflight-provider-candidate-virtual" ||
       dependency_name == "preflight-shared-failure" ||
       dependency_name == "preflight-provider-response-virtual" ||
       dependency_name == "preflight-dependency-response-child" ||
       dependency_name == "preflight-provider-candidate-response-virtual" ||
       dependency_name == "preflight-repository-provider-failure-virtual") {
        return {};
    }
    throw std::runtime_error(
        "Unexpected dependency-plan repository provider query: " + dependency_name);
}

} // namespace

namespace dependency_plan_repository_query_stub {

void reset_query_counts() {
    g_sync_database_package_queries = 0;
    g_strict_repo_provider_queries = 0;
    g_target_metadata_provider_queries = 0;
    g_source_change_provider_queries = 0;
}

std::size_t sync_database_package_query_count() {
    return g_sync_database_package_queries;
}

std::size_t strict_repo_provider_query_count() {
    return g_strict_repo_provider_queries;
}

} // namespace dependency_plan_repository_query_stub

StrictRepositoryPackageQueryResult query_repository_package_strict(
    const std::string& package_name) {
    ++g_sync_database_package_queries;
    if(package_name == "preflight-repository-exact-failure-child") {
        return RepositoryMetadataFailure{
            RepositoryMetadataFailureKind::ConfigurationUnavailable,
            std::nullopt,
            "strict repository exact metadata failure"};
    }
    if(package_name == "case6-repo-lib") {
        return RepositoryPackagePresent{
            "core", 0, package_name, package_name, ObservedVersion::available(ObservedVersionSource::RepositoryExactPackage, "1.0-1"), std::nullopt, {}};
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
    if(package_name == "installed-present") {
        return InstalledExactPackage{
            package_name,
            ObservedVersion::available(
                ObservedVersionSource::InstalledExactPackage,
                "2.0-1")};
    }
    if(package_name == "installed-query-failure") {
        return InstalledExactPackageQueryFailure{
            package_name,
            PackageMetadataFailure{
                PackageMetadataErrorCode::QueryFailed,
                "installed database query failure"}};
    }
    return InstalledExactPackageAbsent{package_name};
}

StrictRepositoryProvidersQueryResult query_repository_providers_strict(
    const std::string& dependency_name) {
    ++g_strict_repo_provider_queries;
    if(dependency_name == "preflight-repository-provider-failure-virtual") {
        return RepositoryMetadataFailure{
            RepositoryMetadataFailureKind::SyncDatabaseUnavailable,
            std::optional<std::string>{"core"},
            "strict repository provider metadata failure"};
    }
    if(dependency_name == "case23-virtual") {
        ProviderCapability capability(
            "case23-virtual=6", "case23-virtual",
            std::optional<std::string>{"6"});
        return RepositoryProviderQuerySnapshot{
            repository_providers(dependency_name),
            {},
            std::vector<std::string>{"core", "extra"},
            {RepositoryExactPackage{
                ConfiguredRepositoryIdentity{"extra", 1},
                "case23-provider",
                "case23-provider-base",
                ObservedVersion::available(
                    ObservedVersionSource::
                        RepositoryExactPackage,
                    "5.0-1"),
                {RepositoryProviderCapability{
                    capability,
                    ObservedVersion::available(
                        ObservedVersionSource::
                            RepositoryProviderCapability,
                        "6")}},
                std::string("x86_64")}}};
    }
    if(dependency_name ==
       "preflight-repository-partial-provider-virtual") {
        return RepositoryProviderQuerySnapshot{
            {ProvidedDependency::from_repository_constraint_metadata(
                "core", "partial-repository-provider",
                "partial-repository-provider-base",
                "x86_64",
                ProviderConstraintMetadata{
                    ProviderCapability(
                        "preflight-repository-partial-provider-virtual=1",
                        "preflight-repository-partial-provider-virtual",
                        std::optional<std::string>{"1"}),
                    ObservedVersion::available(
                        ObservedVersionSource::
                            RepositoryExactPackage,
                        "1.0-1"),
                    ObservedVersion::available(
                        ObservedVersionSource::
                            RepositoryProviderCapability,
                        "1")})},
            {RepositoryMetadataFailure{
                RepositoryMetadataFailureKind::
                    SyncDatabaseUnavailable,
                std::optional<std::string>{"extra"},
                "partial repository provider metadata failure"}},
            std::nullopt,
            {}};
    }
    if(dependency_name == "target-metadata-change-virtual") {
        const bool changed = g_target_metadata_provider_queries++ > 0;
        const std::string capability = changed
                                           ? "target-metadata-change-virtual=3"
                                           : "target-metadata-change-virtual=2";
        return RepositoryProviderQuerySnapshot{
            {
                ProvidedDependency::from_repository_constraint_metadata(
                    "extra", "target-metadata-provider-a",
                    "target-metadata-provider-a-base",
                    "x86_64",
                    ProviderConstraintMetadata{
                        ProviderCapability(
                            capability,
                            "target-metadata-change-virtual",
                            std::optional<std::string>{
                                changed ? "3" : "2"}),
                        ObservedVersion::available(
                            ObservedVersionSource::
                                RepositoryExactPackage,
                            "1.0-1"),
                        ObservedVersion::available(
                            ObservedVersionSource::
                                RepositoryProviderCapability,
                            changed ? "3" : "2")}),
                ProvidedDependency::from_repository_constraint_metadata(
                    "community", "target-metadata-provider-b",
                    "target-metadata-provider-b-base",
                    "x86_64",
                    ProviderConstraintMetadata{
                        ProviderCapability(
                            "target-metadata-change-virtual=4",
                            "target-metadata-change-virtual",
                            std::optional<std::string>{"4"}),
                        ObservedVersion::available(
                            ObservedVersionSource::
                                RepositoryExactPackage,
                            "1.0-1"),
                        ObservedVersion::available(
                            ObservedVersionSource::
                                RepositoryProviderCapability,
                            "4")}),
            },
            {},
            std::nullopt,
            {}};
    }
    if(dependency_name == "selected-source-change-virtual" &&
       g_source_change_provider_queries++ > 0) {
        return RepositoryProviderQuerySnapshot{
            {ProvidedDependency::from_repository_constraint_metadata(
                "extra", "selected-source-change-provider",
                "selected-source-change-provider-base",
                "x86_64",
                ProviderConstraintMetadata{
                    ProviderCapability(
                        "selected-source-change-virtual=1",
                        "selected-source-change-virtual",
                        std::optional<std::string>{"1"}),
                    ObservedVersion::available(
                        ObservedVersionSource::
                            RepositoryExactPackage,
                        "1.0-1"),
                    ObservedVersion::available(
                        ObservedVersionSource::
                            RepositoryProviderCapability,
                        "1")})},
            {},
            std::nullopt,
            {}};
    }
    return RepositoryProviderQuerySnapshot{
        repository_providers(dependency_name), {}, std::nullopt, {}};
}
