#pragma once

#include "package_base_artifact_install_executor.hpp"
#include "reviewed_source_production_failure.hpp"
#include "separated_source_build.hpp"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class RemoteAurCleanupCandidateCollector;
class SeparatedPackageBaseSourceBuildExecutionOwner;

#ifdef MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS
#include <filesystem>
#endif

// 1 PackageBaseのordered required child setを一つのworkspace/transactionで扱う。
// Artifact path、selection index、install directiveはlifecycle内部だけで生成する。
struct SeparatedPackageBaseSourceBuildRequest {
    ProductionArtifactSourceTree source_tree;
    ValidatedPrivateCacheRoot artifact_root;
    std::string package_base;
    std::vector<RequiredPackageArtifactTarget> required_targets;
    SourceBuildEnvironment source_environment;
    SourceEnvironmentEmptyValuePolicy empty_value_policy =
        SourceEnvironmentEmptyValuePolicy::Omit;
    PacmanDatabasePaths database_paths;
};

struct PackageBaseSourceBuildSelectedResult {
    ArtifactPackageIdentity identity;
    DesiredInstallReason desired_reason;
    ArtifactInstallExecutionOutcome outcome;
};

// Transaction成功後のPackageBase aggregate。path/index/directiveを公開せず、
// selected childをrequired順、unselected outputをproduced順でowned保持する。
class PackageBaseSourceBuildExecutionResult final {
    std::string package_base_;
    ProductionSourceBuildStagedOutcome production_outcome_;
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children_;
    std::vector<ArtifactPackageIdentity> unselected_artifacts_;

    PackageBaseSourceBuildExecutionResult(
        std::string package_base,
        ProductionSourceBuildStagedOutcome production_outcome,
        std::vector<PackageBaseSourceBuildSelectedResult>
            selected_children,
        std::vector<ArtifactPackageIdentity> unselected_artifacts) noexcept;

#if defined(MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS) ||       \
    defined(MOGUET_ENABLE_REMOTE_AUR_CLEANUP_RUNNER_TEST_HOOKS)
    struct RunnerTestTag {};

    PackageBaseSourceBuildExecutionResult(
        RunnerTestTag,
        std::string package_base,
        std::vector<PackageBaseSourceBuildSelectedResult>
            selected_children,
        std::vector<ArtifactPackageIdentity> unselected_artifacts) noexcept
        : package_base_(std::move(package_base)),
          production_outcome_({.source_provenance = {},
                               .build_outcome =
                                   ProductionSourceBuildCommandOutcome::Succeeded,
                               .install_outcome =
                                   ProductionSourceInstallOutcome::Succeeded}),
          selected_children_(std::move(selected_children)),
          unselected_artifacts_(std::move(unselected_artifacts)) {
    }
#endif

    friend PackageBaseSourceBuildExecutionResult
    execute_separated_package_base_source_build(
        SeparatedPackageBaseSourceBuildRequest request,
        const SeparatedSourceBuildUnitOptions& options);
    friend PackageBaseSourceBuildExecutionResult
    execute_separated_package_base_source_build_with_cleanup_authority(
        SeparatedPackageBaseSourceBuildRequest request,
        const SeparatedSourceBuildUnitOptions& options,
        RemoteAurCleanupCandidateCollector& collector,
        std::size_t work_item_index);
    friend class SeparatedPackageBaseSourceBuildExecutionOwner;
    friend class LocalSourceInstallAccess;

public:
    PackageBaseSourceBuildExecutionResult(
        const PackageBaseSourceBuildExecutionResult&) = default;
    PackageBaseSourceBuildExecutionResult(
        PackageBaseSourceBuildExecutionResult&&) noexcept = default;
    PackageBaseSourceBuildExecutionResult& operator=(
        const PackageBaseSourceBuildExecutionResult&) = delete;
    PackageBaseSourceBuildExecutionResult& operator=(
        PackageBaseSourceBuildExecutionResult&&) noexcept = delete;
    ~PackageBaseSourceBuildExecutionResult() = default;

    const std::string& package_base() const noexcept;
    const ProductionSourceBuildStagedOutcome& production_outcome()
        const noexcept;
    const ProductionSourceBuildProvenance& source_provenance()
        const noexcept;
    ProductionSourceBuildCommandOutcome build_outcome() const noexcept;
    ProductionSourceInstallOutcome install_outcome() const noexcept;
    const std::vector<PackageBaseSourceBuildSelectedResult>&
    selected_children() const noexcept;
    const std::vector<ArtifactPackageIdentity>&
    unselected_artifacts() const noexcept;

    bool installed_any() const noexcept;
    bool all_skipped_as_needed() const noexcept;

    // Runnerがtransaction成功済みresultをallocationなしでowned child snapshotへ
    // promoteするためのrvalue-only transfer。
    std::string release_package_base() && noexcept;
    std::vector<PackageBaseSourceBuildSelectedResult>
    release_selected_children() && noexcept;
    std::vector<ArtifactPackageIdentity>
    release_unselected_artifacts() && noexcept;

#if defined(MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS)
    static PackageBaseSourceBuildExecutionResult
    make_for_aur_update_runner_test(
        std::string package_base,
        std::vector<PackageBaseSourceBuildSelectedResult>
            selected_children,
        std::vector<ArtifactPackageIdentity> unselected_artifacts) {
        return PackageBaseSourceBuildExecutionResult(
            RunnerTestTag{}, std::move(package_base),
            std::move(selected_children),
            std::move(unselected_artifacts));
    }
#endif

#ifdef MOGUET_ENABLE_REMOTE_AUR_CLEANUP_RUNNER_TEST_HOOKS
    // The installed composition fixture replaces only checkout/review/build
    // orchestration below the production ordered runner. It cannot construct
    // collector/session authority or a full invocation result.
    static PackageBaseSourceBuildExecutionResult
    make_for_remote_aur_cleanup_runner_test(
        std::string package_base,
        std::vector<PackageBaseSourceBuildSelectedResult>
            selected_children,
        std::vector<ArtifactPackageIdentity> unselected_artifacts) {
        return PackageBaseSourceBuildExecutionResult(
            RunnerTestTag{}, std::move(package_base),
            std::move(selected_children),
            std::move(unselected_artifacts));
    }
#endif
};

enum class SeparatedPackageBaseSourceBuildFailurePhase {
    Build,
    ArtifactValidation,
    ArtifactIdentity,
    InstallPreparation,
    InstallTransaction,
};

// selection/policy、metadata、transaction、cleanupの専用型より前のphase failure。
// raw pathを持たず、runnerがgeneric runtime_errorへflattenせず分類できる。
class SeparatedPackageBaseSourceBuildPhaseError final
    : public std::runtime_error {
    SeparatedPackageBaseSourceBuildFailurePhase phase_;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure_;
    std::optional<PackageMetadataFailure> package_metadata_failure_;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome_;
    std::exception_ptr failure_exception_;

#ifdef MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    struct AurUpdateRunnerTestTag {};

    SeparatedPackageBaseSourceBuildPhaseError(
        AurUpdateRunnerTestTag,
        SeparatedPackageBaseSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<ReviewedSourceProductionFailure>
            reviewed_source_failure = std::nullopt,
        std::optional<PackageMetadataFailure>
            package_metadata_failure = std::nullopt,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt)
        : std::runtime_error(diagnostic), phase_(phase),
          reviewed_source_failure_(
              std::move(reviewed_source_failure)),
          package_metadata_failure_(
              std::move(package_metadata_failure)),
          production_outcome_(std::move(production_outcome)) {
    }
#endif

public:
    SeparatedPackageBaseSourceBuildPhaseError(
        SeparatedPackageBaseSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<ReviewedSourceProductionFailure>
            reviewed_source_failure = std::nullopt,
        std::optional<PackageMetadataFailure>
            package_metadata_failure = std::nullopt,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt,
        std::exception_ptr failure_exception = nullptr);

    SeparatedPackageBaseSourceBuildFailurePhase phase() const noexcept;
    const std::optional<ReviewedSourceProductionFailure>&
    reviewed_source_failure() const noexcept;
    const std::optional<PackageMetadataFailure>&
    package_metadata_failure() const noexcept;
    const std::optional<ProductionSourceBuildStagedOutcome>&
    production_outcome() const noexcept;
    void rethrow_failure() const {
        if(failure_exception_ == nullptr) {
            throw std::logic_error(
                "Separated PackageBase source-build failure has no nested exception.");
        }
        std::rethrow_exception(failure_exception_);
    }

#ifdef MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    static SeparatedPackageBaseSourceBuildPhaseError
    make_for_aur_update_runner_test(
        SeparatedPackageBaseSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<ReviewedSourceProductionFailure>
            reviewed_source_failure = std::nullopt,
        std::optional<PackageMetadataFailure>
            package_metadata_failure = std::nullopt,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt) {
        return SeparatedPackageBaseSourceBuildPhaseError(
            AurUpdateRunnerTestTag{}, phase, diagnostic,
            std::move(reviewed_source_failure),
            std::move(package_metadata_failure),
            std::move(production_outcome));
    }
#endif
};

// PR4のclosed selection/policy failureをstringへ潰さず、workspace pathや
// cleanup capabilityを公開しないset lifecycle専用error。
class SeparatedPackageBaseSourceBuildPreparationError final
    : public std::runtime_error {
    PackageBaseArtifactInstallPreparationFailure failure_;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome_;

#ifdef MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    struct AurUpdateRunnerTestTag {};

    SeparatedPackageBaseSourceBuildPreparationError(
        AurUpdateRunnerTestTag,
        PackageBaseArtifactInstallPreparationFailure failure,
        const std::string& diagnostic,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt)
        : std::runtime_error(diagnostic), failure_(std::move(failure)),
          production_outcome_(std::move(production_outcome)) {
    }
#endif

public:
    SeparatedPackageBaseSourceBuildPreparationError(
        PackageBaseArtifactInstallPreparationFailure failure,
        const std::string& diagnostic,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome = std::nullopt);

    const PackageBaseArtifactInstallPreparationFailure& failure()
        const noexcept;
    const PackageBaseArtifactIdentitySelectionFailure* selection_failure()
        const noexcept;
    const MixedPackageBaseInstallReasonUnsupported* mixed_reason_failure()
        const noexcept;
    const std::optional<ProductionSourceBuildStagedOutcome>&
    production_outcome() const noexcept;

#ifdef MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS
    static SeparatedPackageBaseSourceBuildPreparationError
    make_selection_failure_for_aur_update_runner_test(
        PackageBaseArtifactIdentitySelectionFailure failure,
        const std::string& diagnostic) {
        return SeparatedPackageBaseSourceBuildPreparationError(
            AurUpdateRunnerTestTag{},
            PackageBaseArtifactInstallPreparationFailure::
                make_selection_failure_for_aur_update_runner_test(
                    std::move(failure)),
            diagnostic);
    }

    static SeparatedPackageBaseSourceBuildPreparationError
    make_mixed_reason_failure_for_aur_update_runner_test(
        MixedPackageBaseInstallReasonUnsupported failure,
        const std::string& diagnostic) {
        return SeparatedPackageBaseSourceBuildPreparationError(
            AurUpdateRunnerTestTag{},
            PackageBaseArtifactInstallPreparationFailure::
                make_mixed_reason_failure_for_aur_update_runner_test(
                    std::move(failure)),
            diagnostic);
    }
#endif
};

// Transaction成功後のcleanup failure。全child outcomeとunselected identityを
// owned保持し、既存singular cleanup errorへflattenしない。
class SeparatedPackageBaseSourceBuildCleanupError final
    : public std::runtime_error {
    PackageBaseSourceBuildExecutionResult result_;

#if defined(MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS)
    struct AurUpdateRunnerTestTag {};

    SeparatedPackageBaseSourceBuildCleanupError(
        AurUpdateRunnerTestTag,
        PackageBaseSourceBuildExecutionResult result,
        const std::string& diagnostic)
        : std::runtime_error(diagnostic), result_(std::move(result)) {
    }
#endif

public:
    SeparatedPackageBaseSourceBuildCleanupError(
        PackageBaseSourceBuildExecutionResult result,
        const std::string& diagnostic);

    const PackageBaseSourceBuildExecutionResult& result() const noexcept;
    PackageBaseSourceBuildExecutionResult release_result() && noexcept;

#if defined(MOGUET_ENABLE_AUR_UPDATE_EXECUTION_RUNNER_TEST_HOOKS) || \
    defined(MOGUET_ENABLE_UPGRADE_ALL_OPERATION_TEST_HOOKS)
    static SeparatedPackageBaseSourceBuildCleanupError
    make_for_aur_update_runner_test(
        PackageBaseSourceBuildExecutionResult result,
        const std::string& diagnostic) {
        return SeparatedPackageBaseSourceBuildCleanupError(
            AurUpdateRunnerTestTag{}, std::move(result), diagnostic);
    }
#endif
};

PackageBaseSourceBuildExecutionResult
execute_separated_package_base_source_build(
    SeparatedPackageBaseSourceBuildRequest request,
    const SeparatedSourceBuildUnitOptions& options);

PackageBaseSourceBuildExecutionResult
execute_separated_package_base_source_build_with_cleanup_authority(
    SeparatedPackageBaseSourceBuildRequest request,
    const SeparatedSourceBuildUnitOptions& options,
    RemoteAurCleanupCandidateCollector& collector,
    std::size_t work_item_index);

#ifdef MOGUET_ENABLE_SEPARATED_PACKAGE_BASE_SOURCE_BUILD_TEST_HOOKS
using SeparatedPackageBaseSourceBuildWorkspaceObserverForTest =
    void (*)(const std::filesystem::path& workspace_path);

void set_separated_package_base_source_build_workspace_observer_for_test(
    SeparatedPackageBaseSourceBuildWorkspaceObserverForTest observer);
#endif
