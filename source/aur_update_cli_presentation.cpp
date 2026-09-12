#include "aur_update_cli_presentation.hpp"

#include "aur_update_operation_result.hpp"
#include "localization.hpp"
#include "reviewed_source_production_failure.hpp"
#include "reviewed_source_production_outcome.hpp"

#include <algorithm>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

bool is_known_install_reason(DesiredInstallReason reason) noexcept {
    return reason == DesiredInstallReason::Explicit ||
           reason == DesiredInstallReason::Dependency;
}

std::string install_reason_label(DesiredInstallReason reason) {
    switch(reason) {
        case DesiredInstallReason::Explicit:
            return localization::translate_message("explicit");
        case DesiredInstallReason::Dependency:
            return localization::translate_message("dependency");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} child desired install reason.", "AUR"));
}

std::string metadata_error_label(PackageMetadataErrorCode code) {
    switch(code) {
        case PackageMetadataErrorCode::ConfigurationUnavailable:
            return localization::translate_message("configuration unavailable");
        case PackageMetadataErrorCode::ConfigurationMalformed:
            return localization::translate_message("configuration malformed");
        case PackageMetadataErrorCode::InitializationFailed:
            return localization::translate_message(
                "database initialization failed");
        case PackageMetadataErrorCode::LocalDatabaseUnavailable:
            return localization::translate_message(
                "local database unavailable");
        case PackageMetadataErrorCode::InvalidPackageName:
            return localization::translate_message("invalid package name");
        case PackageMetadataErrorCode::QueryFailed:
            return localization::translate_message("package query failed");
        case PackageMetadataErrorCode::MalformedMetadata:
            return localization::translate_message(
                "malformed package metadata");
        case PackageMetadataErrorCode::SyncDatabaseUnavailable:
            return localization::translate_message(
                "sync database unavailable");
        case PackageMetadataErrorCode::RepositoryNotConfigured:
            return localization::translate_message(
                "repository not configured");
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package metadata failure code."));
}

std::string source_build_failure_summary(
    const AurUpdateSourceBuildFailureSnapshot& failure) {
    if(failure.reviewed_source_failure.has_value()) {
        return reviewed_source_production_failure_diagnostic(
            *failure.reviewed_source_failure);
    }
    switch(failure.category) {
        case AurUpdateSourceBuildFailureCategory::Build:
            return localization::translate_message("source build failure");
        case AurUpdateSourceBuildFailureCategory::ArtifactValidation:
            return localization::translate_message(
                "artifact validation failure");
        case AurUpdateSourceBuildFailureCategory::ArtifactIdentity:
            return localization::translate_message("artifact identity failure");
        case AurUpdateSourceBuildFailureCategory::InstallPreparation:
            return localization::translate_message(
                "install preparation failure");
        case AurUpdateSourceBuildFailureCategory::InstallTransaction:
            return localization::translate_message(
                "install transaction failure");
        case AurUpdateSourceBuildFailureCategory::Other:
            return localization::translate_message("build or install failure");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} source-build failure category.", "AUR"));
}

std::string correlation_failure_label(
    AurUpdateExecutionCorrelationFailureReason reason) {
    switch(reason) {
        case AurUpdateExecutionCorrelationFailureReason::PackageBaseMismatch:
            return localization::format_translated_message(
                // TRANSLATORS: PackageBase is a runtime Arch metadata-key identity.
                "{} mismatch", "PackageBase");
        case AurUpdateExecutionCorrelationFailureReason::DesiredInstallReasonMismatch:
            return localization::translate_message("install reason mismatch");
        case AurUpdateExecutionCorrelationFailureReason::
            SelectedArtifactIdentityMismatch:
            return localization::translate_message(
                "selected artifact identity mismatch");
        case AurUpdateExecutionCorrelationFailureReason::EmptySelectedArtifactVersion:
            return localization::translate_message(
                "selected artifact version missing");
        case AurUpdateExecutionCorrelationFailureReason::UnknownChildOutcome:
            return localization::translate_message("unknown child outcome");
        case AurUpdateExecutionCorrelationFailureReason::DuplicateSelectedChild:
            return localization::translate_message("duplicate selected child");
        case AurUpdateExecutionCorrelationFailureReason::MissingSelectedChild:
            return localization::translate_message("missing selected child");
        case AurUpdateExecutionCorrelationFailureReason::ExtraSelectedChild:
            return localization::translate_message("extra selected child");
        case AurUpdateExecutionCorrelationFailureReason::
            InvalidUnselectedArtifactIdentity:
            return localization::translate_message(
                "invalid unselected artifact identity");
        case AurUpdateExecutionCorrelationFailureReason::
            SelectedAndUnselectedIdentityOverlap:
            return localization::translate_message(
                "selected/unselected identity overlap");
        case AurUpdateExecutionCorrelationFailureReason::
            DuplicateUnselectedArtifactIdentity:
            return localization::translate_message(
                "duplicate unselected artifact identity");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} execution correlation failure reason.", "AUR"));
}

std::string transaction_failure_summary(
    const AurUpdatePackageTransactionFailureSnapshot& failure) {
    switch(failure.category) {
        case AurUpdatePackageTransactionFailureCategory::CommandFailed: {
            if(failure.exit_code.has_value()) {
                return localization::format_translated_message(
                    // TRANSLATORS: The placeholder is an external process exit code.
                    "package transaction failed (exit code {})",
                    *failure.exit_code);
            }
            return localization::translate_message(
                "package transaction failed");
        }
        case AurUpdatePackageTransactionFailureCategory::CommandExecutionFailed:
            if(failure.exit_code.has_value()) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} transaction process failure unexpectedly has an exit code.",
                    "AUR"));
            }
            return localization::translate_message(
                "package transaction process exception");
        case AurUpdatePackageTransactionFailureCategory::Other:
            if(failure.exit_code.has_value()) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} unknown transaction failure unexpectedly has an exit code.",
                    "AUR"));
            }
            return localization::translate_message(
                "package transaction unknown exception");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} package transaction failure category.", "AUR"));
}

std::string failure_detail_summary(
    AurUpdateWorkItemFailureKind kind,
    const AurUpdateWorkItemFailureDetail* detail) {
    const bool has_typed_detail = detail != nullptr &&
                                  !std::holds_alternative<std::monostate>(*detail);
    switch(kind) {
        case AurUpdateWorkItemFailureKind::None:
            if(has_typed_detail) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Successful {} work item unexpectedly has failure detail.",
                    "AUR"));
            }
            return localization::translate_message("none");
        case AurUpdateWorkItemFailureKind::CleanupFailedAfterPackageTransaction:
            if(has_typed_detail) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} cleanup failure has unexpected typed failure detail.",
                    "AUR"));
            }
            return localization::translate_message(
                "cleanup failure after successful package transaction");
        case AurUpdateWorkItemFailureKind::UnknownException:
            if(has_typed_detail) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Unknown {} failure has unexpected typed failure detail.",
                    "AUR"));
            }
            return localization::translate_message("unknown exception");
        case AurUpdateWorkItemFailureKind::PriorWorkItemStopped:
            if(has_typed_detail) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: NotAttempted is a runtime enum token; AUR is a runtime project identity.
                    "{} {} work item has unexpected failure detail.",
                    "NotAttempted", "AUR"));
            }
            return localization::translate_message("prior work item stopped");
        case AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete:
            return localization::translate_message("authoritative execution incomplete; installation and publication outcomes are retained separately");
        case AurUpdateWorkItemFailureKind::BuildOrInstallFailed:
            break;
        default:
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "Unknown {} work-item failure kind.", "AUR"));
    }

    if(detail == nullptr) {
        return localization::translate_message("build or install failure");
    }
    return std::visit(
        [](const auto& failure) -> std::string {
            using Failure = std::decay_t<decltype(failure)>;
            if constexpr(std::is_same_v<Failure, std::monostate>) {
                return localization::translate_message(
                    "build or install failure");
            } else if constexpr(std::is_same_v<
                                    Failure,
                                    PackageBaseArtifactIdentitySelectionFailure>) {
                return localization::translate_message(
                    "artifact selection failure");
            } else if constexpr(std::is_same_v<
                                    Failure,
                                    MixedPackageBaseInstallReasonUnsupported>) {
                return localization::translate_message(
                    "mixed install reason unsupported");
            } else if constexpr(std::is_same_v<Failure, PackageMetadataFailure>) {
                return localization::translate_message(
                           "package metadata failure") +
                       " (" + metadata_error_label(failure.code) + ")";
            } else if constexpr(std::is_same_v<
                                    Failure,
                                    AurUpdateSourceBuildFailureSnapshot>) {
                return source_build_failure_summary(failure);
            } else if constexpr(std::is_same_v<
                                    Failure,
                                    AurUpdatePackageTransactionFailureSnapshot>) {
                return transaction_failure_summary(failure);
            } else if constexpr(std::is_same_v<
                                    Failure,
                                    AurUpdateExecutionCorrelationFailure>) {
                return localization::translate_message(
                           "result correlation failure") +
                       " (" + correlation_failure_label(failure.reason) +
                       ")";
            }
        },
        *detail);
}

bool is_known_work_item_status(
    AurUpdateWorkItemExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
        case AurUpdateWorkItemExecutionStatus::Updated:
        case AurUpdateWorkItemExecutionStatus::NoChange:
        case AurUpdateWorkItemExecutionStatus::Cancelled:
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return true;
    }
    return false;
}

bool is_known_child_status(AurUpdateChildExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateChildExecutionStatus::BootstrapSkipped:
        case AurUpdateChildExecutionStatus::Installed:
        case AurUpdateChildExecutionStatus::SkippedAsNeeded:
        case AurUpdateChildExecutionStatus::InstalledCleanupFailed:
        case AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed:
        case AurUpdateChildExecutionStatus::NotAttempted:
            return true;
    }
    return false;
}

bool is_selected_child_status(AurUpdateChildExecutionStatus status) noexcept {
    return status == AurUpdateChildExecutionStatus::Installed ||
           status == AurUpdateChildExecutionStatus::SkippedAsNeeded ||
           status == AurUpdateChildExecutionStatus::InstalledCleanupFailed ||
           status ==
               AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed;
}

bool child_status_matches_work_item(
    AurUpdateWorkItemExecutionStatus work_item_status,
    AurUpdateChildExecutionStatus child_status) noexcept {
    switch(work_item_status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
            return child_status == AurUpdateChildExecutionStatus::BootstrapSkipped;
        case AurUpdateWorkItemExecutionStatus::Updated:
            return child_status == AurUpdateChildExecutionStatus::Installed ||
                   child_status == AurUpdateChildExecutionStatus::SkippedAsNeeded;
        case AurUpdateWorkItemExecutionStatus::NoChange:
            return child_status == AurUpdateChildExecutionStatus::SkippedAsNeeded;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return child_status == AurUpdateChildExecutionStatus::NotAttempted;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            return child_status ==
                       AurUpdateChildExecutionStatus::InstalledCleanupFailed ||
                   child_status == AurUpdateChildExecutionStatus::
                                       SkippedAsNeededCleanupFailed;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return child_status == AurUpdateChildExecutionStatus::
                                       SkippedAsNeededCleanupFailed;
    }
    return false;
}

bool failure_kind_matches_work_item(
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind kind) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::BootstrapSkipped:
        case AurUpdateWorkItemExecutionStatus::Updated:
        case AurUpdateWorkItemExecutionStatus::NoChange:
            return kind == AurUpdateWorkItemFailureKind::None;
        case AurUpdateWorkItemExecutionStatus::Cancelled:
            return kind == AurUpdateWorkItemFailureKind::None;
        case AurUpdateWorkItemExecutionStatus::Failed:
            return kind == AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete || kind == AurUpdateWorkItemFailureKind::BuildOrInstallFailed ||
                   kind == AurUpdateWorkItemFailureKind::UnknownException;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            return kind == AurUpdateWorkItemFailureKind::
                               CleanupFailedAfterPackageTransaction;
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return kind == AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
    }
    return false;
}

void require_valid_identity(
    const ArtifactPackageIdentity& identity,
    std::string diagnostic) {
    if(identity.package_name.empty() || identity.full_version.empty()) {
        throw std::logic_error(std::move(diagnostic));
    }
}

bool transaction_snapshots_match(
    const AurUpdatePackageTransactionFailureSnapshot& left,
    const AurUpdatePackageTransactionFailureSnapshot& right) noexcept {
    if(left.category != right.category || left.exit_code != right.exit_code ||
       left.attempted_artifacts.size() != right.attempted_artifacts.size()) {
        return false;
    }
    for(std::size_t index = 0; index < left.attempted_artifacts.size();
        ++index) {
        const AurUpdatePackageTransactionAttempt& left_attempt =
            left.attempted_artifacts[index];
        const AurUpdatePackageTransactionAttempt& right_attempt =
            right.attempted_artifacts[index];
        if(left_attempt.identity.package_name !=
               right_attempt.identity.package_name ||
           left_attempt.identity.full_version !=
               right_attempt.identity.full_version ||
           left_attempt.desired_reason != right_attempt.desired_reason) {
            return false;
        }
    }
    return true;
}

void require_coherent_work_item(
    const AurUpdateWorkItemExecutionResult& work_item) {
    if(!is_known_work_item_status(work_item.status)) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "Unknown {} work-item execution status.", "AUR"));
    }
    const bool cancelled = work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled;
    const bool valid_cancellation = cancelled == work_item.cancellation.has_value() &&
                                    (!work_item.cancellation ||
                                     work_item.cancellation->reason == ConfirmationCancellationReason::ExplicitToken ||
                                     work_item.cancellation->reason == ConfirmationCancellationReason::EndOfInput);
    if(!valid_cancellation ||
       (cancelled && (work_item.production_outcome || work_item.devel_execution || work_item.diagnostic)) ||
       work_item.package_base.empty() || work_item.child_results.empty() ||
       work_item.child_results.size() != work_item.plan_package_names.size() ||
       !failure_kind_matches_work_item(work_item.status, work_item.failure_kind)) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "{} work-item presentation snapshot is incoherent.", "AUR"));
    }
    if(work_item.child_results.size() == 1) {
        if(work_item.package_name !=
           work_item.child_results.front().required_package_name) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "{} singular work-item package identity is incoherent.",
                "AUR"));
        }
    } else if(!work_item.package_name.empty()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "{} multiple-child work item retained a singular package name.",
            "AUR"));
    }

    std::set<std::string> selected_names;
    bool has_installed_child = false;
    for(std::size_t index = 0; index < work_item.child_results.size(); ++index) {
        const AurUpdateChildExecutionResult& child =
            work_item.child_results[index];
        if(!is_known_child_status(child.status)) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "Unknown {} child execution status.", "AUR"));
        }
        if(child.work_item_index != work_item.work_item_index ||
           child.build_plan_order_index != work_item.build_plan_order_index ||
           child.required_child_index != index ||
           child.package_base != work_item.package_base ||
           child.required_package_name.empty() ||
           child.required_package_name != work_item.plan_package_names[index] ||
           !is_known_install_reason(child.desired_install_reason) ||
           (!child_status_matches_work_item(work_item.status, child.status) &&
            !(work_item.devel_execution && work_item.status == AurUpdateWorkItemExecutionStatus::Failed &&
              work_item.devel_execution->operation == DevelSourceArtifactInstallOperation::Succeeded &&
              work_item.devel_execution->proof == DevelSourceArtifactInstallProof::Complete && child.status == AurUpdateChildExecutionStatus::Installed))) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "{} child presentation snapshot is incoherent.", "AUR"));
        }

        if(is_selected_child_status(child.status)) {
            if(!child.selected_artifact.has_value()) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Completed {} child has no selected artifact identity.",
                    "AUR"));
            }
            require_valid_identity(
                *child.selected_artifact,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Selected {} artifact has an incomplete package identity.",
                    "AUR"));
            if(child.selected_artifact->package_name !=
                   child.required_package_name ||
               !selected_names.insert(
                                  child.selected_artifact->package_name)
                    .second) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "Selected {} child artifact identity is incoherent.",
                    "AUR"));
            }
        } else if(child.selected_artifact.has_value()) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "Uncompleted {} child unexpectedly has a selected artifact.",
                "AUR"));
        }
        has_installed_child = has_installed_child ||
                              child.status == AurUpdateChildExecutionStatus::Installed ||
                              child.status ==
                                  AurUpdateChildExecutionStatus::InstalledCleanupFailed;
    }

    if((work_item.status == AurUpdateWorkItemExecutionStatus::Updated ||
        work_item.status ==
            AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed) &&
       !has_installed_child) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "Updated {} work item has no installed child outcome.", "AUR"));
    }

    std::set<std::string> unselected_names;
    for(const ArtifactPackageIdentity& identity :
        work_item.unselected_artifacts) {
        require_valid_identity(
            identity, localization::format_translated_message(
                          // TRANSLATORS: AUR is a runtime project identity.
                          "Unselected {} artifact has an incomplete package identity.",
                          "AUR"));
        if(selected_names.contains(identity.package_name) ||
           !unselected_names.insert(identity.package_name).second) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "Unselected {} artifact identity is incoherent.", "AUR"));
        }
    }
    if((work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled ||
        work_item.status == AurUpdateWorkItemExecutionStatus::Failed ||
        work_item.status == AurUpdateWorkItemExecutionStatus::NotAttempted) &&
       !work_item.unselected_artifacts.empty()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "Uncompleted {} work item retained unselected artifacts.",
            "AUR"));
    }

    const auto* transaction_detail = std::get_if<
        AurUpdatePackageTransactionFailureSnapshot>(
        &work_item.failure_detail);
    if(work_item.transaction_failure.has_value()) {
        if(work_item.status != AurUpdateWorkItemExecutionStatus::Failed) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "Non-failed {} work item retained transaction failure evidence.",
                "AUR"));
        }
        static_cast<void>(transaction_failure_summary(
            *work_item.transaction_failure));
        for(const AurUpdatePackageTransactionAttempt& attempt :
            work_item.transaction_failure->attempted_artifacts) {
            require_valid_identity(
                attempt.identity,
                localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} transaction attempt has an incomplete package identity.",
                    "AUR"));
            static_cast<void>(install_reason_label(attempt.desired_reason));
        }
        if(transaction_detail != nullptr) {
            if(!transaction_snapshots_match(
                   *transaction_detail, *work_item.transaction_failure)) {
                throw std::logic_error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "{} transaction failure snapshots are inconsistent.",
                    "AUR"));
            }
        } else if(!std::holds_alternative<
                      AurUpdateExecutionCorrelationFailure>(
                      work_item.failure_detail)) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "{} transaction evidence has no typed transaction or correlation failure.",
                "AUR"));
        }
    } else if(transaction_detail != nullptr) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "{} transaction failure detail has no attempt evidence.",
            "AUR"));
    }

    if(work_item.status == AurUpdateWorkItemExecutionStatus::NotAttempted &&
       work_item.production_outcome.has_value()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "An unattempted {} work item unexpectedly has a production outcome.",
            "AUR"));
    }
    if(work_item.production_outcome.has_value()) {
        static_cast<void>(format_production_source_build_staged_outcome(
            work_item.package_base, *work_item.production_outcome));
    }

    static_cast<void>(failure_detail_summary(
        work_item.failure_kind, &work_item.failure_detail));
}

bool is_ordinary_singular_success(
    const AurUpdateWorkItemExecutionResult& work_item) noexcept {
    if(work_item.child_results.size() != 1 ||
       !work_item.unselected_artifacts.empty() ||
       work_item.failure_kind != AurUpdateWorkItemFailureKind::None ||
       (work_item.status != AurUpdateWorkItemExecutionStatus::Updated &&
        work_item.status != AurUpdateWorkItemExecutionStatus::NoChange)) {
        return false;
    }
    const AurUpdateChildExecutionResult& child =
        work_item.child_results.front();
    return work_item.package_base == child.required_package_name &&
           (child.status == AurUpdateChildExecutionStatus::Installed ||
            child.status == AurUpdateChildExecutionStatus::SkippedAsNeeded);
}

std::string child_outcome_label(
    const AurUpdateWorkItemExecutionResult& work_item,
    AurUpdateChildExecutionStatus status) {
    if(work_item.status == AurUpdateWorkItemExecutionStatus::Cancelled) {
        return localization::translate_message("Cancelled");
    }
    if(status == AurUpdateChildExecutionStatus::NotAttempted && work_item.devel_execution) {
        const auto operation = work_item.devel_execution->operation;
        if(operation == DevelSourceArtifactInstallOperation::Succeeded) return localization::translate_message("transaction succeeded; exact installed proof unavailable");
        if(operation == DevelSourceArtifactInstallOperation::OutcomeUnknown) return localization::translate_message("transaction outcome unknown; installed artifact unverified");
        if(operation == DevelSourceArtifactInstallOperation::Failed) return localization::translate_message("transaction failed; package effects unverified");
    }
    switch(status) {
        case AurUpdateChildExecutionStatus::BootstrapSkipped:
            return localization::translate_message("skipped: devel tracking bootstrap");
        case AurUpdateChildExecutionStatus::Installed:
            return localization::translate_message("installed / updated");
        case AurUpdateChildExecutionStatus::SkippedAsNeeded:
            return localization::translate_message(
                "skipped as needed / no change");
        case AurUpdateChildExecutionStatus::InstalledCleanupFailed:
            return localization::translate_message(
                "installed / updated, but cleanup failed");
        case AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed:
            return localization::translate_message(
                "skipped as needed / no change, but cleanup failed");
        case AurUpdateChildExecutionStatus::NotAttempted:
            return work_item.status == AurUpdateWorkItemExecutionStatus::NotAttempted
                       ? localization::translate_message(
                             "not attempted: prior work item stopped")
                       : localization::translate_message("no successful outcome");
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} child execution status.", "AUR"));
}

std::string child_summary_line(
    const AurUpdateWorkItemExecutionResult& work_item,
    const AurUpdateChildExecutionResult& child) {
    if(child.selected_artifact.has_value()) {
        return localization::format_translated_message(
                   // TRANSLATORS: Package names and version are runtime data.
                   "  required child: {} -> {} {}",
                   child.required_package_name,
                   child.selected_artifact->package_name,
                   child.selected_artifact->full_version) +
               " (" + install_reason_label(child.desired_install_reason) +
               "): " + child_outcome_label(work_item, child.status);
    }
    return localization::format_translated_message(
               // TRANSLATORS: The placeholder is a package name.
               "  required child: {}", child.required_package_name) +
           " (" + install_reason_label(child.desired_install_reason) +
           "): " + child_outcome_label(work_item, child.status);
}

bool should_print_failure(AurUpdateWorkItemFailureKind kind) {
    switch(kind) {
        case AurUpdateWorkItemFailureKind::None:
        case AurUpdateWorkItemFailureKind::PriorWorkItemStopped:
            return false;
        case AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete:
            return true;
        case AurUpdateWorkItemFailureKind::BuildOrInstallFailed:
        case AurUpdateWorkItemFailureKind::CleanupFailedAfterPackageTransaction:
        case AurUpdateWorkItemFailureKind::UnknownException:
            return true;
    }
    throw std::logic_error(localization::format_translated_message(
        // TRANSLATORS: AUR is a runtime project identity.
        "Unknown {} work-item failure kind.", "AUR"));
}

void append_work_item_presentation(
    AurUpdateCliPresentation& presentation,
    const AurUpdateWorkItemExecutionResult& work_item) {
    require_coherent_work_item(work_item);
    if(work_item.production_outcome.has_value()) {
        ReviewedSourceProductionOutcomePresentation reviewed =
            format_production_source_build_staged_outcome(
                work_item.package_base,
                *work_item.production_outcome);
        presentation.summary_lines.insert(
            presentation.summary_lines.end(),
            std::make_move_iterator(reviewed.info_lines.begin()),
            std::make_move_iterator(reviewed.info_lines.end()));
    }
    if(!is_ordinary_singular_success(work_item)) {
        presentation.summary_lines.push_back(
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are the PackageBase metadata-key
                // identity and a package-base identity.
                "{} result: {}", "PackageBase", work_item.package_base));
        for(const AurUpdateChildExecutionResult& child :
            work_item.child_results) {
            presentation.summary_lines.push_back(
                child_summary_line(work_item, child));
        }
        for(const ArtifactPackageIdentity& identity :
            work_item.unselected_artifacts) {
            presentation.summary_lines.push_back(
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are package identity and version.
                    "  produced artifact: {} {} (not selected; not installed)",
                    identity.package_name, identity.full_version));
        }
    }

    if(!should_print_failure(work_item.failure_kind)) return;
    presentation.error_lines.push_back(
        localization::format_translated_message(
            // TRANSLATORS: The placeholders are the PackageBase metadata-key
            // identity and a package-base identity.
            "  execution failure for {} {}:", "PackageBase",
            work_item.package_base) +
        " " + failure_detail_summary(work_item.failure_kind, &work_item.failure_detail));
    if(!work_item.transaction_failure.has_value()) return;

    const bool detail_is_transaction = std::holds_alternative<
        AurUpdatePackageTransactionFailureSnapshot>(
        work_item.failure_detail);
    if(!detail_is_transaction) {
        presentation.error_lines.push_back(
            localization::translate_message(
                "    package transaction evidence:") +
            " " + transaction_failure_summary(*work_item.transaction_failure));
    }
    for(const AurUpdatePackageTransactionAttempt& attempt :
        work_item.transaction_failure->attempted_artifacts) {
        presentation.error_lines.push_back(
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are package identity and version.
                "    transaction attempt: {} {}",
                attempt.identity.package_name,
                attempt.identity.full_version) +
            " (" + install_reason_label(attempt.desired_reason) + ")");
    }
}

} // namespace

std::string aur_update_cli_target_failure_summary(
    const AurUpdateOperationTargetResult& target) {
    if(!target.execution_failure_kind.has_value() ||
       *target.execution_failure_kind == AurUpdateWorkItemFailureKind::None) {
        // Preparation can fail before any execution result exists. Use only
        // this target's attributed payload; an execution failure above None
        // must still pass through its existing validation and projection.
        for(const AurUpdatePreparationIssue& issue : target.preparation_issues) {
            if(issue.reviewed_source_failure.has_value()) {
                return reviewed_source_production_failure_diagnostic(
                    *issue.reviewed_source_failure);
            }
        }
        return localization::translate_message(
            "failure category unavailable");
    }
    const AurUpdateWorkItemFailureDetail* detail =
        target.execution_failure_detail.has_value()
            ? &*target.execution_failure_detail
            : nullptr;
    return failure_detail_summary(*target.execution_failure_kind, detail);
}

AurUpdateCliPresentation format_aur_update_cli_presentation(
    const AurUpdateOperationResult& result) {
    AurUpdateCliPresentation presentation;
    const SelectedRepositoryProviderTransactionResult& provider_transaction =
        result.selected_repository_provider_transaction;
    if(provider_transaction.status ==
           SelectedRepositoryProviderTransactionStatus::Failed ||
       provider_transaction.status ==
           SelectedRepositoryProviderTransactionStatus::OutcomeUnknown) {
        if(!provider_transaction.diagnostic.has_value() ||
           provider_transaction.diagnostic->empty()) {
            throw std::logic_error(localization::translate_message(
                "Failed selected repository provider transaction has no diagnostic."));
        }
        presentation.error_lines.push_back(
            localization::format_translated_message(
                "  selected repository provider transaction failed: {}",
                *provider_transaction.diagnostic));
    }
    std::set<std::size_t> presented_failure_work_items;
    for(const AurUpdateWorkItemExecutionResult& work_item :
        result.execution_work_items) {
        append_work_item_presentation(presentation, work_item);
        if(should_print_failure(work_item.failure_kind)) {
            presented_failure_work_items.insert(work_item.work_item_index);
        }
    }

    // Correlationが壊れwork-item snapshotを失ったdefensive resultでも、targetの
    // typed decisive failureだけはraw diagnosticなしで一度表示する。
    for(const AurUpdateOperationTargetResult& target : result.targets) {
        if(!target.execution_failure_kind.has_value() ||
           !should_print_failure(*target.execution_failure_kind)) {
            continue;
        }
        if(target.execution_work_item_index.has_value() &&
           presented_failure_work_items.contains(
               *target.execution_work_item_index)) {
            continue;
        }
        if(target.execution_work_item_index.has_value()) {
            presented_failure_work_items.insert(
                *target.execution_work_item_index);
        }
        presentation.error_lines.push_back(
            localization::translate_message("  execution failure:") +
            " " + aur_update_cli_target_failure_summary(target));
    }
    return presentation;
}
