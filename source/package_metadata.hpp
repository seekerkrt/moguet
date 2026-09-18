#pragma once

#include "installed_package.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

struct PacmanDatabasePaths {
    std::filesystem::path root_dir;
    std::filesystem::path db_path;
};

struct PacmanRepositoryConfiguration {
    PacmanDatabasePaths database_paths;
    std::vector<std::string> repository_names;
};

enum class RepositoryPackageSearchMatchKind {
    Search,
    ExactGroup,
};

struct RepositoryPackageSearchMatch {
    std::string repository_name;
    std::string package_name;
    std::optional<std::string> version;
    std::optional<std::string> description;
    RepositoryPackageSearchMatchKind kind;
    std::optional<std::string> group_name;
};

struct RepositoryPackageSearchSnapshot {
    std::vector<std::string> repository_order;
    std::vector<RepositoryPackageSearchMatch> matches;
};

// Installed package stateについて断言できる範囲を保持する。
// UnknownをNoChangeへ丸めず、strict boolean helperとは別に扱う。
enum class PackageStateChange {
    NoChange,
    Changed,
    Unknown,
};

struct RepositoryPackageLookup {
    std::string package_name;
    std::optional<std::string> exact_repository_name;
};

struct RepositoryPackageMetadata {
    std::string repository_name;
    std::string package_name;
    std::uint64_t package_size_bytes;
    std::uint64_t installed_size_bytes;
};

struct PackageNotFound {};

enum class PackageMetadataErrorCode {
    ConfigurationUnavailable,
    ConfigurationMalformed,
    InitializationFailed,
    LocalDatabaseUnavailable,
    InvalidPackageName,
    QueryFailed,
    MalformedMetadata,
    SyncDatabaseUnavailable,
    RepositoryNotConfigured,
};

struct PackageMetadataFailure {
    PackageMetadataErrorCode code;
    std::string diagnostic;
};

// Cleanup policy metadata remains factual evidence until the pure policy
// reducer projects it to CleanupPolicyProtection.  Completeness is explicit
// so a failed/partial inventory cannot be represented as a negative match.
enum class CleanupPolicyMetadataCompleteness {
    Complete,
    Incomplete,
    Failed,
};

enum class CleanupPolicyAuthorityKind {
    InstalledBaseDevelMetaPackage,
    ConfiguredSyncBaseDevelMetaPackage,
    BaseDevelGroupCompatibility,
};

enum class CleanupPolicyAuthorityObservation {
    Present,
    Absent,
    NotObserved,
    Unavailable,
};

enum class CleanupPolicyCandidateEvaluation {
    Protected,
    NotProtected,
    NotEvaluated,
};

enum class CleanupPolicyEvidenceConsistency {
    Consistent,
    Contradictory,
};

struct CleanupPolicyCandidatePackageMetadata {
    std::string package_name;
    std::string version;
    std::vector<std::string> provides;
    std::vector<std::string> groups;
};

struct CleanupPolicyMetaPackageMetadata {
    CleanupPolicyAuthorityKind authority_kind;
    std::optional<std::size_t> configured_repository_order;
    std::optional<std::string> repository_name;
    std::string package_name;
    std::string version;
    std::vector<std::string> dependencies;
};

struct CleanupPolicyGroupMemberMetadata {
    std::size_t configured_repository_order;
    std::string repository_name;
    std::string package_name;
};

struct CleanupPolicyGroupMetadata {
    std::string group_name;
    std::vector<std::string> repository_order;
    std::vector<std::string> repositories_with_group;
    std::vector<CleanupPolicyGroupMemberMetadata> members;
};

struct CleanupPolicyAuthorityEvidence {
    CleanupPolicyAuthorityKind authority_kind;
    CleanupPolicyAuthorityObservation observation;
    CleanupPolicyMetadataCompleteness inventory_completeness;
    CleanupPolicyCandidateEvaluation candidate_evaluation;
    CleanupPolicyMetadataCompleteness evaluation_completeness;
    std::vector<CleanupPolicyMetaPackageMetadata> meta_packages;
    std::optional<CleanupPolicyGroupMetadata> group;
};

struct CleanupPolicyProtectionEvidence {
    CleanupPolicyMetadataCompleteness local_database_completeness;
    CleanupPolicyMetadataCompleteness candidate_metadata_completeness;
    std::optional<CleanupPolicyCandidatePackageMetadata> candidate;
    CleanupPolicyAuthorityEvidence installed_base_devel;
    CleanupPolicyAuthorityEvidence configured_sync_base_devel;
    CleanupPolicyAuthorityEvidence base_devel_group;
    CleanupPolicyEvidenceConsistency consistency;
    std::vector<PackageMetadataFailure> failures;
};

// Slice 3 adapterへ渡すlibalpm read-phaseのowned snapshot。
// libalpmのrelation enumやborrowed pointerはこの境界より外へ出さない。
enum class RepositoryProvidedPackageRelation {
    Unversioned,
    Equal,
    GreaterThanOrEqual,
    LessThanOrEqual,
    GreaterThan,
    LessThan,
    Unsupported,
};

struct RepositoryProvidedPackageMetadata {
    std::optional<std::string> package_name;
    std::optional<std::string> version;
    RepositoryProvidedPackageRelation relation;
};

struct InstalledExactPackageMetadata {
    std::string package_name;
    std::optional<std::string> version;
};

struct InstalledPackageRelationMetadata {
    std::string package_name;
    std::optional<std::string> version;
    std::vector<RepositoryProvidedPackageMetadata> provides;
};

using InstalledPackageRelationMetadataInventory =
    std::vector<InstalledPackageRelationMetadata>;

struct InstalledPackageRelationMetadataInventoryFailure {
    InstalledPackageRelationMetadataInventory observed_packages;
    std::optional<std::size_t> package_index;
    PackageMetadataFailure failure;
};

using InstalledPackageRelationMetadataInventoryResult = std::variant<
    InstalledPackageRelationMetadataInventory,
    InstalledPackageRelationMetadataInventoryFailure>;

// Issue #460のcandidate observation専用read phase。既存relation inventoryを
// 肥大化させず、libalpmが所有するdirect runtime dependencyだけをcanonicalな
// specificationとしてowned保持する。source/provenanceやresolutionは表さない。
struct InstalledPackageRuntimeDependencyMetadata {
    std::string package_name;
    std::vector<std::string> dependency_specifications;
    // Version read from the same libalpm package as its runtime requirements.
    // Legacy observations cannot prove identity continuity for removal plans.
    std::optional<std::string> installed_version = std::nullopt;
};

using InstalledPackageRuntimeDependencyMetadataInventory =
    std::vector<InstalledPackageRuntimeDependencyMetadata>;

struct InstalledPackageRuntimeDependencyMetadataInventoryFailure {
    InstalledPackageRuntimeDependencyMetadataInventory observed_packages;
    std::optional<std::size_t> package_index;
    PackageMetadataFailure failure;
};

using InstalledPackageRuntimeDependencyMetadataInventoryResult = std::variant<
    InstalledPackageRuntimeDependencyMetadataInventory,
    InstalledPackageRuntimeDependencyMetadataInventoryFailure>;

using InstalledExactPackageMetadataQueryResult = std::variant<
    InstalledExactPackageMetadata,
    PackageNotFound,
    PackageMetadataFailure>;

struct RepositoryExactPackageMetadata {
    std::size_t configured_repository_order;
    std::string repository_name;
    std::string package_name;
    std::string package_base;
    std::optional<std::string> version;
    std::vector<RepositoryProvidedPackageMetadata> provides;
    // A successful strict repository observation always carries the exact
    // libalpm value. Missing or malformed architecture is a source failure.
    std::string architecture;
};

struct RepositoryExactPackageMetadataNotFound {
    std::size_t configured_repository_order;
    std::string repository_name;
    std::string package_name;
};

struct RepositoryExactPackageMetadataSourceFailure {
    std::size_t configured_repository_order;
    std::string repository_name;
    std::string package_name;
    PackageMetadataFailure failure;
};

using RepositoryExactPackageMetadataSourceResult = std::variant<
    RepositoryExactPackageMetadata,
    RepositoryExactPackageMetadataNotFound,
    RepositoryExactPackageMetadataSourceFailure>;

struct RepositoryExactPackageMetadataSnapshot {
    std::vector<std::string> repository_order;
    std::vector<RepositoryExactPackageMetadataSourceResult> source_results;
};

using RepositoryExactPackageMetadataQueryResult = std::variant<
    RepositoryExactPackageMetadataSnapshot,
    PackageMetadataFailure>;

// Provider enumeration uses the same owned libalpm projection as exact
// lookup. Each configured source keeps its own success/failure state so a
// later unavailable repository cannot erase observations from an earlier one.
struct RepositoryProviderPackageMetadataSourceSnapshot {
    std::size_t configured_repository_order;
    std::string repository_name;
    std::vector<RepositoryExactPackageMetadata> packages;
};

struct RepositoryProviderPackageMetadataSourceFailure {
    std::size_t configured_repository_order;
    std::string repository_name;
    PackageMetadataFailure failure;
};

using RepositoryProviderPackageMetadataSourceResult = std::variant<
    RepositoryProviderPackageMetadataSourceSnapshot,
    RepositoryProviderPackageMetadataSourceFailure>;

struct RepositoryProviderPackageMetadataSnapshot {
    std::vector<std::string> repository_order;
    std::vector<RepositoryProviderPackageMetadataSourceResult> source_results;
};

using RepositoryProviderPackageMetadataQueryResult = std::variant<
    RepositoryProviderPackageMetadataSnapshot,
    PackageMetadataFailure>;

using InstalledPackageQueryResult = std::variant<
    InstalledPackageMetadata,
    PackageNotFound,
    PackageMetadataFailure>;

using RepositoryPackageQueryResult = std::variant<
    RepositoryPackageMetadata,
    PackageNotFound,
    PackageMetadataFailure>;

using RepositoryPackageSearchResult = std::variant<
    RepositoryPackageSearchSnapshot,
    PackageMetadataFailure>;

using ForeignPackageInventory = std::vector<InstalledPackageMetadata>;

using ForeignPackageInventoryResult = std::variant<
    ForeignPackageInventory,
    PackageMetadataFailure>;

// package state比較用に、libalpmのborrowを残さないname/version snapshotを保持する。
using LocalPackageVersionSnapshot = std::map<std::string, std::string>;

using LocalPackageVersionSnapshotResult = std::variant<
    LocalPackageVersionSnapshot,
    PackageMetadataFailure>;

// cleanup baseline/current observation向けのfull local DB snapshot。
// keyとvalueはともにownedで、1 read phaseのname/version/actual
// PackageBase/actual architecture/reasonをlosslessに保持する。
using InstalledPackageStateSnapshot =
    std::map<std::string, InstalledPackageMetadata>;

using InstalledPackageStateSnapshotResult = std::variant<
    InstalledPackageStateSnapshot,
    PackageMetadataFailure>;

// resolver/session openの失敗を、CLI境界でstd::exceptionとして扱える形で伝播する。
class PackageMetadataError : public std::runtime_error {
public:
    explicit PackageMetadataError(PackageMetadataFailure failure);

    const PackageMetadataFailure& failure() const noexcept;

private:
    PackageMetadataFailure failure_;
};

PacmanDatabasePaths resolve_pacman_database_paths();
PacmanRepositoryConfiguration resolve_pacman_repository_configuration();
PacmanRepositoryConfiguration
resolve_pacman_root_search_repository_configuration();

RepositoryPackageSearchResult query_repository_root_package_search(
    const std::string& query);

// Slice 3のconfigured repository exact observation用read phase。
// repository固有のopen/query failureをconfigured order付きowned resultへ保持する。
RepositoryExactPackageMetadataQueryResult
query_configured_repository_exact_package_metadata(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& package_name);

RepositoryProviderPackageMetadataQueryResult
query_configured_repository_provider_package_metadata(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& dependency_name);

// Read-only cleanup-policy authority.  The local database is observed first;
// configured sync databases are opened only when the exact installed
// base-devel meta package is authoritatively absent.  Expected metadata
// failures are retained in the returned evidence and never thrown as absence.
CleanupPolicyProtectionEvidence query_cleanup_policy_protection_evidence(
    const PacmanRepositoryConfiguration& configuration,
    const std::string& candidate_package_name);

// local DBと全sync DBを1 handleで照合し、borrowを残さないowned inventoryを返す。
ForeignPackageInventoryResult query_foreign_package_inventory(
    const PacmanRepositoryConfiguration& configuration);

// 1 read phaseのlibalpm handleを単独所有し、raw libalpm型をdomain側へ公開しない。
class PackageMetadataSession {
public:
    static PackageMetadataSession open(const PacmanDatabasePaths& paths);

    PackageMetadataSession(const PackageMetadataSession&) = delete;
    PackageMetadataSession& operator=(const PackageMetadataSession&) = delete;

    PackageMetadataSession(PackageMetadataSession&&) noexcept;
    PackageMetadataSession& operator=(PackageMetadataSession&&) noexcept;

    ~PackageMetadataSession() noexcept;

    InstalledPackageQueryResult query_installed_package(
        const std::string& package_name) const;

    InstalledExactPackageMetadataQueryResult
    query_installed_exact_package_metadata(
        const std::string& package_name) const;

    LocalPackageVersionSnapshotResult snapshot_local_package_versions() const;

    InstalledPackageStateSnapshotResult
    snapshot_installed_package_states() const;

    InstalledPackageRelationMetadataInventoryResult
    snapshot_installed_package_relation_metadata() const;

    InstalledPackageRuntimeDependencyMetadataInventoryResult
    snapshot_installed_package_runtime_dependency_metadata() const;

private:
    struct Impl;

    friend CleanupPolicyProtectionEvidence
    query_cleanup_policy_protection_evidence(
        const PacmanRepositoryConfiguration& configuration,
        const std::string& candidate_package_name);

    explicit PackageMetadataSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

// Session open failureもempty inventoryへ丸めず、同じtyped resultへ戻す。
InstalledPackageStateSnapshotResult snapshot_installed_package_states(
    const PacmanDatabasePaths& paths);

// Session open failureもsuccessful empty inventoryへ丸めない。
InstalledPackageRuntimeDependencyMetadataInventoryResult
query_installed_package_runtime_dependency_metadata(
    const PacmanDatabasePaths& paths);

// repository sync DB専用のread phaseを所有し、installed/local sessionとは分離する。
class RepositoryPackageMetadataSession {
public:
    static RepositoryPackageMetadataSession open(
        const PacmanRepositoryConfiguration& configuration);

    RepositoryPackageMetadataSession(const RepositoryPackageMetadataSession&) = delete;
    RepositoryPackageMetadataSession& operator=(
        const RepositoryPackageMetadataSession&) = delete;

    RepositoryPackageMetadataSession(RepositoryPackageMetadataSession&&) noexcept;
    RepositoryPackageMetadataSession& operator=(
        RepositoryPackageMetadataSession&&) noexcept;

    ~RepositoryPackageMetadataSession() noexcept;

    RepositoryPackageQueryResult query_repository_package(
        const RepositoryPackageLookup& lookup) const;

    RepositoryExactPackageMetadataQueryResult
    query_repository_exact_package_metadata(
        const std::string& package_name) const;

    RepositoryProviderPackageMetadataQueryResult
    query_repository_provider_package_metadata(
        const std::string& dependency_name) const;

    RepositoryPackageSearchResult query_root_package_search(
        const std::string& query) const;

private:
    struct Impl;

    friend CleanupPolicyProtectionEvidence
    query_cleanup_policy_protection_evidence(
        const PacmanRepositoryConfiguration& configuration,
        const std::string& candidate_package_name);

    explicit RepositoryPackageMetadataSession(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};
