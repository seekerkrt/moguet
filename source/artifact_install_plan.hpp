#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

enum class DesiredInstallReason;

enum class ArtifactWorkspaceOwnership {
    InvocationOwnedFresh,
    ExternalOrShared
};

enum class SourcePkgdestState {
    Unchecked,
    NotDefined,
    Defined
};

struct ProducedPackageArtifact {
    // Archive identityのpure projection。filesystem path proofは別capabilityが所有する。
    std::string package_name;
};

struct RequiredPackageArtifactTarget {
    std::string package_base;
    std::string package_name;
    // POLICY(#268): role展開前ではなく、既存reducer適用後のinstall intentを受ける。
    // 未指定reasonをExplicitへvalue-initializeせず、pure boundaryでunknownとして拒否する。
    DesiredInstallReason desired_reason =
        static_cast<DesiredInstallReason>(-1);
    // A confirmed coordinated replacement pins the full archive version.
    // Ordinary source/devel updates leave this unset.
    std::optional<std::string> expected_full_version = std::nullopt;
};

struct PackageBaseArtifactSelectionRequest {
    std::string package_base;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    std::vector<ProducedPackageArtifact> produced_artifacts;
};

struct SelectedPackageArtifact {
    ProducedPackageArtifact artifact;
    DesiredInstallReason desired_reason;
};

struct DiagnosticPackageArtifactMatch {
    std::size_t required_target_index;
    std::size_t produced_artifact_index;
    ProducedPackageArtifact artifact;
    DesiredInstallReason desired_reason;
};

struct MissingRequiredArtifact {
    std::size_t required_target_index;
    RequiredPackageArtifactTarget target;
};

struct DuplicateProducedPackageIdentity {
    std::size_t first_artifact_index;
    std::size_t duplicate_artifact_index;
    std::string package_name;
};

struct DuplicateRequiredArtifactTarget {
    std::string package_name;
    DesiredInstallReason desired_reason;
    std::vector<std::size_t> required_target_indices;
};

struct RequiredArtifactReasonConflict {
    struct Occurrence {
        std::size_t required_target_index;
        DesiredInstallReason desired_reason;
    };

    std::string package_name;
    std::vector<Occurrence> occurrences;
};

struct RequiredArtifactAttributionMismatch {
    std::size_t required_target_index;
    std::string expected_package_base;
    std::string actual_package_base;
    std::string package_name;
};

enum class ArtifactSelectionIdentityInput {
    PackageBase,
    RequiredPackageBase,
    RequiredPackage,
    ProducedPackage
};

struct ArtifactSelectionIdentityInconsistency {
    ArtifactSelectionIdentityInput input;
    std::optional<std::size_t> input_index;
    std::string identity;
};

struct UnknownArtifactInstallReason {
    std::size_t required_target_index;
    RequiredPackageArtifactTarget target;
};

struct PackageBaseArtifactSelectionSuccess {
    std::string package_base;
    // POLICY(#268): selectedはrequired target順、unselectedはproduced artifact順を保つ。
    std::vector<SelectedPackageArtifact> selected_artifacts;
    std::vector<ProducedPackageArtifact> unselected_artifacts;
};

struct PackageBaseArtifactSelectionFailure {
    std::string package_base;
    // LANDMINE(#268): selectionとして利用できない部分照合は診断に限定する。
    std::vector<DiagnosticPackageArtifactMatch> diagnostic_partial_matches;
    std::vector<ProducedPackageArtifact> diagnostic_unselected_artifacts;
    std::vector<MissingRequiredArtifact> missing_required_artifacts;
    std::vector<DuplicateProducedPackageIdentity> duplicate_produced_identities;
    std::vector<DuplicateRequiredArtifactTarget> duplicate_required_targets;
    std::vector<RequiredArtifactReasonConflict> required_reason_conflicts;
    std::vector<RequiredArtifactAttributionMismatch> attribution_mismatches;
    std::vector<ArtifactSelectionIdentityInconsistency>
        identity_inconsistencies;
    std::vector<UnknownArtifactInstallReason> unknown_install_reasons;
};

class PackageBaseArtifactSelectionResult final {
public:
    PackageBaseArtifactSelectionResult() = delete;
    PackageBaseArtifactSelectionResult(
        const PackageBaseArtifactSelectionResult&) = default;
    PackageBaseArtifactSelectionResult(
        PackageBaseArtifactSelectionResult&&) noexcept = default;
    PackageBaseArtifactSelectionResult& operator=(
        const PackageBaseArtifactSelectionResult&) = delete;
    PackageBaseArtifactSelectionResult& operator=(
        PackageBaseArtifactSelectionResult&&) noexcept = delete;
    ~PackageBaseArtifactSelectionResult() = default;

    [[nodiscard]] bool is_success() const noexcept;
    [[nodiscard]] const PackageBaseArtifactSelectionSuccess* success()
        const noexcept;
    [[nodiscard]] const PackageBaseArtifactSelectionFailure* failure()
        const noexcept;

private:
    explicit PackageBaseArtifactSelectionResult(
        PackageBaseArtifactSelectionSuccess success);
    explicit PackageBaseArtifactSelectionResult(
        PackageBaseArtifactSelectionFailure failure);

    std::variant<PackageBaseArtifactSelectionSuccess,
                 PackageBaseArtifactSelectionFailure>
        outcome_;

    friend PackageBaseArtifactSelectionResult select_package_base_artifacts(
        const PackageBaseArtifactSelectionRequest& request);
};

struct ArtifactSelectionRequest {
    std::string requested_name;
    std::string package_base;
    ArtifactWorkspaceOwnership workspace_ownership =
        ArtifactWorkspaceOwnership::ExternalOrShared;
    SourcePkgdestState source_pkgdest_state =
        SourcePkgdestState::Unchecked;
    std::vector<ProducedPackageArtifact> artifacts;
};

struct ValidatedArtifactInstallTarget {
    // Pure policy result。filesystem proofはValidatedPackageArtifactPathが別に所有する。
    std::string package_name;
};

enum class ExistingInstallReason {
    Explicit,
    Dependency
};

enum class InstalledVersionState {
    NotInstalled,
    SameVersion,
    DifferentVersion
};

enum class InstallReasonDirective {
    Default,
    AsExplicit,
    AsDependency
};

ValidatedArtifactInstallTarget validate_single_output_artifact(
    const ArtifactSelectionRequest& request);

PackageBaseArtifactSelectionResult select_package_base_artifacts(
    const PackageBaseArtifactSelectionRequest& request);

InstallReasonDirective resolve_install_reason_directive(
    DesiredInstallReason desired_reason,
    InstalledVersionState version_state,
    std::optional<ExistingInstallReason> existing_reason,
    bool needed);

void require_supported_separated_install_options(bool rm_deps);
