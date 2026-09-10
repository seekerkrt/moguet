#pragma once

#include "aur_rpc.hpp"
#include "dependency_provider.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace local_dependency_plan_query_stub {

enum class RepositoryQueryKind {
    StrictPackage,
    StrictProviders
};

enum class AurQueryKind {
    LegacyInfo,
    StrictInfo,
    LegacyProviderSearch,
    StrictProviderSearch
};

struct RepositoryQuery {
    RepositoryQueryKind kind;
    std::string subject;

    bool operator==(const RepositoryQuery&) const = default;
};

struct AurQuery {
    AurQueryKind kind;
    std::string subject;

    bool operator==(const AurQuery&) const = default;
};

void reset_repository_stub();
void set_repository_package_response(
    std::string package_name,
    std::optional<std::string> repository_name);
void set_repository_package_response(
    std::string package_name,
    std::optional<std::string> repository_name,
    std::string package_base);
void set_repository_package_failure(
    std::string package_name, std::string diagnostic);
void set_repository_provider_response(
    std::string dependency_name,
    std::vector<ProvidedDependency> providers);
const std::vector<RepositoryQuery>& repository_query_history();
std::size_t repository_query_count(
    RepositoryQueryKind kind, const std::string& subject);

void reset_aur_stub();
void set_aur_package_response(
    std::string package_name, std::optional<AurPackageInfo> package);
void set_aur_package_failure(
    std::string package_name, std::string diagnostic);
void set_aur_provider_response(
    std::string provided_name, std::vector<std::string> package_names);
void set_aur_provider_failure(
    std::string provided_name, std::string diagnostic);
const std::vector<AurQuery>& aur_query_history();
std::size_t aur_query_count(AurQueryKind kind, const std::string& subject);

} // namespace local_dependency_plan_query_stub
