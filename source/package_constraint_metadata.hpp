#pragma once

#include "dependency_constraint.hpp"
#include "package_metadata.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct InstalledExactPackage {
    std::string package_name;
    ObservedVersion observed_version;

    bool operator==(const InstalledExactPackage&) const = default;
};

struct InstalledExactPackageAbsent {
    std::string package_name;
};

struct InstalledExactPackageQueryFailure {
    std::string package_name;
    PackageMetadataFailure failure;
};

using InstalledExactPackageObservationResult = std::variant<
    InstalledExactPackage,
    InstalledExactPackageAbsent,
    InstalledExactPackageQueryFailure>;

struct ConfiguredRepositoryIdentity {
    std::string repository_name;
    std::size_t configured_order;

    bool operator==(const ConfiguredRepositoryIdentity&) const = default;
};

struct RepositoryProviderCapability {
    ProviderCapability capability;
    ObservedVersion provided_version;

    bool operator==(const RepositoryProviderCapability&) const = default;
};

using ProviderCapabilityMetadataProjectionResult = std::variant<
    std::vector<RepositoryProviderCapability>,
    DependencyConstraintParseFailure>;

// libalpm由来のowned provide metadataをrelation-awareにtyped capabilityへ
// 変換する。Installed / repository adapterは同じprovider-domain authorityを
// 共有し、typed fieldをsynthetic dependency stringとして再parseしない。
ProviderCapabilityMetadataProjectionResult
project_package_metadata_provides(
    const std::vector<RepositoryProvidedPackageMetadata>& metadata,
    ObservedVersionSource version_source);

struct RepositoryExactPackage {
    ConfiguredRepositoryIdentity repository;
    std::string package_name;
    std::string package_base;
    ObservedVersion package_version;
    std::vector<RepositoryProviderCapability> provides;
    // Strict libalpm observations set an exact single value. Legacy factual
    // fixtures remain explicitly unknown instead of inferring from package
    // name, PackageBase, or host architecture.
    std::optional<std::string> architecture = std::nullopt;
};

struct RepositoryExactPackageAbsent {
    ConfiguredRepositoryIdentity repository;
    std::string package_name;
};

using RepositoryExactPackageSourceFailureReason = std::variant<
    PackageMetadataFailure,
    DependencyConstraintParseFailure>;

struct RepositoryExactPackageSourceFailure {
    ConfiguredRepositoryIdentity repository;
    std::string package_name;
    RepositoryExactPackageSourceFailureReason reason;
};

using RepositoryExactPackageSourceResult = std::variant<
    RepositoryExactPackage,
    RepositoryExactPackageAbsent,
    RepositoryExactPackageSourceFailure>;

struct RepositoryExactPackageObservation {
    std::vector<std::string> configured_repository_order;
    std::vector<RepositoryExactPackageSourceResult> source_results;
};

struct RepositoryExactPackageObservationFailure {
    std::string package_name;
    PackageMetadataFailure failure;
};

using RepositoryExactPackageObservationResult = std::variant<
    RepositoryExactPackageObservation,
    RepositoryExactPackageObservationFailure>;

struct RepositoryProviderSourceObservation {
    ConfiguredRepositoryIdentity repository;
    std::vector<RepositoryExactPackage> packages;
};

struct RepositoryProviderSourceFailure {
    ConfiguredRepositoryIdentity repository;
    RepositoryExactPackageSourceFailureReason reason;
};

using RepositoryProviderSourceResult = std::variant<
    RepositoryProviderSourceObservation,
    RepositoryProviderSourceFailure>;

struct RepositoryProviderObservation {
    std::vector<std::string> configured_repository_order;
    std::vector<RepositoryProviderSourceResult> source_results;
};

struct RepositoryProviderObservationFailure {
    std::string dependency_name;
    PackageMetadataFailure failure;
};

using RepositoryProviderObservationResult = std::variant<
    RepositoryProviderObservation,
    RepositoryProviderObservationFailure>;

InstalledExactPackageObservationResult observe_installed_exact_package(
    const PackageMetadataSession& session,
    const std::string& package_name);

RepositoryExactPackageObservationResult observe_repository_exact_package(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& package_name);

RepositoryProviderObservationResult observe_repository_providers(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& dependency_name);
