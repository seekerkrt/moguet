#pragma once

#include "dependency_constraint.hpp"
#include "dependency_provider.hpp"
#include "package_relation.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct AurPackageInfo;

struct AurProviderCapabilityMetadata {
    ProviderCapability capability;
    ObservedVersion provided_version;

    bool operator==(const AurProviderCapabilityMetadata&) const = default;
};

// AUR package metadata projected at the RPC response or explicitly evaluated
// recipe boundary. The owning AurPackageInfo preserves that distinct origin;
// package/version source identity remains AUR. Downstream consumers use these
// owned values without reparsing dependency or Provides expressions.
struct AurPackageConstraintMetadata {
    std::string package_name;
    std::string package_base;
    ObservedVersion package_version;
    std::vector<DependencyRequirement> depends;
    std::vector<DependencyRequirement> make_depends;
    std::vector<DependencyRequirement> check_depends;
    std::vector<AurProviderCapabilityMetadata> provides;
    std::vector<DeclaredPackageRelation> relations;

    bool operator==(const AurPackageConstraintMetadata&) const = default;
};

enum class AurConstraintMetadataField {
    Depends,
    MakeDepends,
    CheckDepends,
    Provides,
    Conflicts,
    Replaces
};

struct AurConstraintMetadataProjectionFailure {
    std::string package_name;
    std::string package_base;
    AurConstraintMetadataField field;
    std::size_t item_index;
    DependencyConstraintParseFailure reason;

    bool operator==(const AurConstraintMetadataProjectionFailure&) const =
        default;
};

using AurConstraintMetadataProjectionResult = std::variant<
    AurPackageConstraintMetadata,
    AurConstraintMetadataProjectionFailure>;

AurConstraintMetadataProjectionResult project_aur_constraint_metadata(
    const AurPackageInfo& package);

struct AurProviderMetadataUnavailable {
    std::string package_name;
    std::optional<std::string> package_base;
    ObservedVersionUnknownReason reason;

    bool operator==(const AurProviderMetadataUnavailable&) const = default;
};

using AurProviderCandidateMetadata = std::variant<
    AurPackageConstraintMetadata,
    AurConstraintMetadataProjectionFailure,
    AurProviderMetadataUnavailable>;

struct AurProviderDependencyProjection {
    ConsumerDependencyRequirement requirement;
    ProvidedDependency provider;
    ConstraintEvaluation evaluation;

    bool operator==(const AurProviderDependencyProjection&) const = default;
};

struct AurProviderDependencyUnknown {
    ConsumerDependencyRequirement requirement;
    std::string package_name;
    std::optional<std::string> package_base;
    ObservedVersionUnknownReason reason;

    bool operator==(const AurProviderDependencyUnknown&) const = default;
};

enum class AurProviderProjectionFailureKind {
    InvalidCandidateMetadata,
    MatchingCapabilityMissing,
    ProviderIdentityChanged
};

struct AurProviderDependencyProjectionFailure {
    ConsumerDependencyRequirement requirement;
    std::string package_name;
    std::optional<std::string> package_base;
    AurProviderProjectionFailureKind kind;
    std::optional<DependencyConstraintParseFailure> metadata_failure;

    bool operator==(const AurProviderDependencyProjectionFailure&) const =
        default;
};

using AurProviderDependencyProjectionResult = std::variant<
    AurProviderDependencyProjection,
    AurProviderDependencyUnknown,
    AurProviderDependencyProjectionFailure>;

AurProviderDependencyProjectionResult project_aur_provider_dependency(
    const ConsumerDependencyRequirement& requirement,
    const AurProviderCandidateMetadata& candidate_metadata);

std::vector<AurProviderDependencyProjectionResult>
project_aur_provider_dependencies(
    const ConsumerDependencyRequirement& requirement,
    const std::vector<AurProviderCandidateMetadata>& candidates);

AurProviderDependencyProjectionResult refresh_aur_provider_dependency(
    const AurProviderDependencyProjection& selected,
    const AurProviderCandidateMetadata& current_metadata);
