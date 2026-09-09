#include "artifact_install_plan.hpp"
#include "separated_package_base_source_build.hpp"
#include "separated_source_build.hpp"

#include <stdexcept>

namespace {

enum class FailureMode {
    Unexpected,
    InstallTransaction,
    PackageMetadata,
};

FailureMode g_failure_mode = FailureMode::Unexpected;

} // namespace

namespace reviewed_source_production_execution_stub {

void fail_next_install_transaction() {
    g_failure_mode = FailureMode::InstallTransaction;
}

void fail_next_package_metadata() {
    g_failure_mode = FailureMode::PackageMetadata;
}

} // namespace reviewed_source_production_execution_stub

#ifndef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
void require_supported_separated_install_options(bool) {
}
#endif

SeparatedSourceBuildExecutionResult execute_separated_source_build_unit(
    SeparatedSourceBuildUnitRequest request,
    const SeparatedSourceBuildUnitOptions&) {
    ProductionSourceBuildStagedOutcome production_outcome{
        request.source_tree.provenance(),
        ProductionSourceBuildCommandOutcome::Succeeded,
        ProductionSourceInstallOutcome::Failed};
    const FailureMode failure_mode = g_failure_mode;
    g_failure_mode = FailureMode::Unexpected;
    if(failure_mode == FailureMode::InstallTransaction) {
        throw SeparatedSourceBuildPhaseError(
            SeparatedSourceBuildFailurePhase::InstallTransaction,
            std::move(production_outcome),
            "scripted pacman transaction failure");
    }
    if(failure_mode == FailureMode::PackageMetadata) {
        const PackageMetadataFailure failure{
            PackageMetadataErrorCode::QueryFailed,
            "scripted PackageMetadataError"};
        throw SeparatedSourceBuildPhaseError(
            SeparatedSourceBuildFailurePhase::InstallPreparation,
            std::move(production_outcome), failure.diagnostic,
            failure);
    }
    throw std::logic_error(
        "Reviewed source production preparation test reached execution.");
}

SeparatedSourceBuildPhaseError::SeparatedSourceBuildPhaseError(
    SeparatedSourceBuildFailurePhase phase,
    ProductionSourceBuildStagedOutcome production_outcome,
    const std::string& diagnostic,
    std::optional<PackageMetadataFailure> package_metadata_failure,
    std::exception_ptr failure_exception)
    : std::runtime_error(diagnostic), phase_(phase),
      production_outcome_(std::move(production_outcome)),
      package_metadata_failure_(std::move(package_metadata_failure)),
      failure_exception_(std::move(failure_exception)) {
}

PackageBaseSourceBuildExecutionResult
execute_separated_package_base_source_build(
    SeparatedPackageBaseSourceBuildRequest,
    const SeparatedSourceBuildUnitOptions&) {
    throw std::logic_error(
        "Reviewed source production preparation test reached PackageBase execution.");
}

PackageBaseSourceBuildExecutionResult
execute_separated_package_base_source_build_with_cleanup_authority(
    SeparatedPackageBaseSourceBuildRequest,
    const SeparatedSourceBuildUnitOptions&,
    RemoteAurCleanupCandidateCollector&,
    std::size_t) {
    throw std::logic_error(
        "Reviewed source production preparation test reached cleanup-authority PackageBase execution.");
}

SeparatedPackageBaseSourceBuildPhaseError::
    SeparatedPackageBaseSourceBuildPhaseError(
        SeparatedPackageBaseSourceBuildFailurePhase phase,
        const std::string& diagnostic,
        std::optional<ReviewedSourceProductionFailure>
            reviewed_source_failure,
        std::optional<PackageMetadataFailure>
            package_metadata_failure,
        std::optional<ProductionSourceBuildStagedOutcome>
            production_outcome,
        std::exception_ptr failure_exception)
    : std::runtime_error(diagnostic), phase_(phase),
      reviewed_source_failure_(std::move(reviewed_source_failure)),
      package_metadata_failure_(std::move(package_metadata_failure)),
      production_outcome_(std::move(production_outcome)),
      failure_exception_(std::move(failure_exception)) {
}

SeparatedPackageBaseSourceBuildFailurePhase
SeparatedPackageBaseSourceBuildPhaseError::phase() const noexcept {
    return phase_;
}

const std::optional<ReviewedSourceProductionFailure>&
SeparatedPackageBaseSourceBuildPhaseError::reviewed_source_failure()
    const noexcept {
    return reviewed_source_failure_;
}

const std::optional<PackageMetadataFailure>&
SeparatedPackageBaseSourceBuildPhaseError::package_metadata_failure()
    const noexcept {
    return package_metadata_failure_;
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPhaseError::production_outcome()
    const noexcept {
    return production_outcome_;
}

#ifdef MOGUET_ENABLE_SOURCE_INVOCATION_EXECUTION_TEST_HOOKS
// Pure legacy accessors needed by the real aggregate loop's error/display arms.
// No legacy result constructor or authoritative producer is provided here.
const std::string& PackageBaseSourceBuildExecutionResult::package_base() const noexcept {
    return package_base_;
}
const ProductionSourceBuildStagedOutcome& PackageBaseSourceBuildExecutionResult::production_outcome() const noexcept {
    return production_outcome_;
}
const std::vector<PackageBaseSourceBuildSelectedResult>& PackageBaseSourceBuildExecutionResult::selected_children() const noexcept {
    return selected_children_;
}
const std::vector<ArtifactPackageIdentity>& PackageBaseSourceBuildExecutionResult::unselected_artifacts() const noexcept {
    return unselected_artifacts_;
}
const std::optional<ProductionSourceBuildStagedOutcome>& SeparatedPackageBaseSourceBuildPreparationError::production_outcome() const noexcept {
    return production_outcome_;
}
const PackageBaseSourceBuildExecutionResult& SeparatedPackageBaseSourceBuildCleanupError::result() const noexcept {
    return result_;
}
#endif
