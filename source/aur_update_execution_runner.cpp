#include "aur_update_execution_runner.hpp"

#include "interactive_confirmation.hpp"
#include "localization.hpp"
#include "trusted_cache.hpp"

#include <algorithm>
#include <exception>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view AUR_SERVICE_NAME = "AUR";
constexpr std::string_view PACKAGE_BASE_FIELD_NAME = "PackageBase";

std::string unknown_exception_diagnostic() {
    return localization::format_translated_message(
        // TRANSLATORS: {} is the literal service name "AUR".
        "Prepared {} update source-build work item failed with an unknown exception.",
        AUR_SERVICE_NAME);
}

template <typename Value>
bool has_duplicate_value(const std::vector<Value>& values) noexcept {
    for(std::size_t index = 0; index < values.size(); ++index) {
        if(std::find(values.begin(), values.begin() + index, values[index]) !=
           values.begin() + index) {
            return true;
        }
    }
    return false;
}

template <typename Value>
void add_unique(std::vector<Value>& values, const Value& value) {
    if(std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

bool is_known_desired_install_reason(DesiredInstallReason reason) noexcept {
    return reason == DesiredInstallReason::Explicit ||
           reason == DesiredInstallReason::Dependency;
}

bool is_known_package_role(PackageRole role) noexcept {
    switch(role) {
        case PackageRole::Root:
        case PackageRole::RuntimeDependency:
        case PackageRole::BuildDependency:
        case PackageRole::CheckDependency:
            return true;
    }
    return false;
}

bool same_required_target(
    const RequiredPackageArtifactTarget& lhs,
    const RequiredPackageArtifactTarget& rhs) noexcept {
    return lhs.package_base == rhs.package_base &&
           lhs.package_name == rhs.package_name &&
           lhs.desired_reason == rhs.desired_reason;
}

std::vector<std::string> required_package_names(
    const ProductionSourceBuildWorkItem& work_item) {
    std::vector<std::string> package_names;
    package_names.reserve(work_item.required_targets.size());
    for(const auto& target : work_item.required_targets) {
        package_names.push_back(target.package_name);
    }
    return package_names;
}

void require_valid_prepared_child_attribution(
    const ProductionSourceBuildWorkItem& work_item,
    const AurUpdatePreparedWorkItemAttribution& attribution) {
    if(attribution.required_target_attributions.size() !=
       work_item.required_targets.size()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared {} update source-build child correlation count is inconsistent.",
            AUR_SERVICE_NAME));
    }

    std::vector<std::size_t> aggregate_update_plan_indices;
    std::vector<RootTargetIdentity> aggregate_roots;
    std::set<std::string> package_names;
    for(std::size_t child_index = 0;
        child_index < work_item.required_targets.size(); ++child_index) {
        const RequiredPackageArtifactTarget& required_target =
            work_item.required_targets[child_index];
        const AurUpdateRequiredTargetAttribution& child =
            attribution.required_target_attributions[child_index];
        if(!same_required_target(required_target, child.required_target) ||
           child.required_target.package_base != attribution.package_base ||
           child.affected_update_plan_indices.empty() ||
           child.affected_roots.empty() || child.roles.empty() ||
           has_duplicate_value(child.affected_update_plan_indices) ||
           has_duplicate_value(child.affected_roots) ||
           has_duplicate_value(child.roles) ||
           !is_known_desired_install_reason(
               child.required_target.desired_reason) ||
           !std::all_of(
               child.roles.begin(), child.roles.end(),
               is_known_package_role) ||
           !package_names.insert(child.required_target.package_name).second) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "Prepared {} update source-build required child attribution is inconsistent.",
                AUR_SERVICE_NAME));
        }
        for(const std::size_t update_plan_index :
            child.affected_update_plan_indices) {
            add_unique(aggregate_update_plan_indices, update_plan_index);
        }
        for(const auto& root : child.affected_roots) {
            add_unique(aggregate_roots, root);
        }
    }

    if(aggregate_update_plan_indices !=
           attribution.affected_update_plan_indices ||
       aggregate_roots != attribution.affected_roots) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared {} update source-build aggregate child attribution is inconsistent.",
            AUR_SERVICE_NAME));
    }
}

void require_valid_prepared_invocation(
    const PreparedAurUpdateSourceBuildInvocation& invocation,
    const PreparedProductionSourceBuildInvocation& production_invocation) {
    if(!invocation.is_valid()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared {} update source-build invocation is invalid or has already been consumed.",
            AUR_SERVICE_NAME));
    }

    const std::vector<AurUpdatePreparedWorkItemAttribution>& attributions =
        invocation.work_item_attributions();
    if(production_invocation.work_items.empty() ||
       attributions.size() != production_invocation.work_items.size()) {
        throw std::logic_error(localization::format_translated_message(
            // TRANSLATORS: {} is the literal service name "AUR".
            "Prepared {} update source-build invocation correlation count is inconsistent.",
            AUR_SERVICE_NAME));
    }

    for(std::size_t index = 0;
        index < production_invocation.work_items.size(); ++index) {
        const ProductionSourceBuildWorkItem& work_item =
            production_invocation.work_items[index];
        const AurUpdatePreparedWorkItemAttribution& attribution =
            attributions[index];

        require_static_production_source_build_work_item(work_item);
        const bool is_singular = work_item.required_targets.size() == 1;
        if(attribution.invocation_work_item_index != index ||
           (index > 0 &&
            attribution.build_plan_order_index <=
                attributions[index - 1].build_plan_order_index) ||
           attribution.package_base.empty() ||
           attribution.package_base != work_item.request.checkout_name ||
           attribution.affected_update_plan_indices.empty() ||
           attribution.affected_roots.empty() ||
           has_duplicate_value(attribution.affected_update_plan_indices) ||
           has_duplicate_value(attribution.affected_roots) ||
           (is_singular &&
            attribution.package_name !=
                work_item.required_targets.front().package_name) ||
           (!is_singular && !attribution.package_name.empty()) ||
           attribution.package_name != work_item.request.package_name) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "Prepared {} update source-build invocation work-item correlation is inconsistent.",
                AUR_SERVICE_NAME));
        }
        require_valid_prepared_child_attribution(work_item, attribution);
    }
}

AurUpdateChildExecutionResult make_not_attempted_child_result(
    const AurUpdatePreparedWorkItemAttribution& attribution,
    std::size_t child_index) {
    const AurUpdateRequiredTargetAttribution& child =
        attribution.required_target_attributions[child_index];
    return AurUpdateChildExecutionResult{
        .work_item_index = attribution.invocation_work_item_index,
        .build_plan_order_index = attribution.build_plan_order_index,
        .required_child_index = child_index,
        .package_base = child.required_target.package_base,
        .required_package_name = child.required_target.package_name,
        .desired_install_reason = child.required_target.desired_reason,
        .affected_update_plan_indices =
            child.affected_update_plan_indices,
        .affected_roots = child.affected_roots,
        .roles = child.roles,
        .selected_artifact = std::nullopt,
        .status = AurUpdateChildExecutionStatus::NotAttempted,
    };
}

AurUpdateWorkItemExecutionResult make_not_attempted_result(
    const ProductionSourceBuildWorkItem& work_item,
    const AurUpdatePreparedWorkItemAttribution& attribution) {
    std::vector<AurUpdateChildExecutionResult> child_results;
    child_results.reserve(attribution.required_target_attributions.size());
    for(std::size_t child_index = 0;
        child_index < attribution.required_target_attributions.size();
        ++child_index) {
        child_results.push_back(
            make_not_attempted_child_result(attribution, child_index));
    }

    return AurUpdateWorkItemExecutionResult{
        .work_item_index = attribution.invocation_work_item_index,
        .build_plan_order_index = attribution.build_plan_order_index,
        .package_name = attribution.package_name,
        .package_base = attribution.package_base,
        .plan_package_names = required_package_names(work_item),
        .affected_update_plan_indices =
            attribution.affected_update_plan_indices,
        .affected_roots = attribution.affected_roots,
        .production_outcome = std::nullopt,
        .child_results = std::move(child_results),
        .unselected_artifacts = {},
        .transaction_failure = std::nullopt,
        .status = AurUpdateWorkItemExecutionStatus::NotAttempted,
        .failure_kind =
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped,
        .failure_detail = std::monostate{},
        .diagnostic = std::nullopt,
    };
}

AurUpdateExecutionCorrelationFailure correlation_failure(
    AurUpdateExecutionCorrelationFailureReason reason,
    std::string diagnostic,
    std::optional<std::size_t> required_child_index = std::nullopt,
    std::optional<std::string> package_name = std::nullopt) {
    return AurUpdateExecutionCorrelationFailure{
        reason,
        required_child_index,
        std::move(package_name),
        std::move(diagnostic)};
}

std::optional<AurUpdateExecutionCorrelationFailure>
validate_completed_package_base_result(
    const PackageBaseSourceBuildExecutionResult& completed,
    const AurUpdateWorkItemExecutionResult& planned) {
    if(completed.package_base() != planned.package_base) {
        return correlation_failure(
            AurUpdateExecutionCorrelationFailureReason::
                PackageBaseMismatch,
            localization::format_translated_message(
                // TRANSLATORS: Both placeholders are the literal field
                // name "PackageBase".
                "{} source-build result does not match the prepared {}.",
                PACKAGE_BASE_FIELD_NAME,
                PACKAGE_BASE_FIELD_NAME));
    }

    const auto& selected = completed.selected_children();
    if(selected.size() != planned.child_results.size()) {
        const bool is_missing = selected.size() < planned.child_results.size();
        return correlation_failure(
            is_missing
                ? AurUpdateExecutionCorrelationFailureReason::
                      MissingSelectedChild
                : AurUpdateExecutionCorrelationFailureReason::
                      ExtraSelectedChild,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal field name "PackageBase".
                "{} source-build selected child count does not match the prepared required children.",
                PACKAGE_BASE_FIELD_NAME));
    }

    std::set<std::string> selected_names;
    for(std::size_t child_index = 0; child_index < selected.size();
        ++child_index) {
        const PackageBaseSourceBuildSelectedResult& actual =
            selected[child_index];
        const AurUpdateChildExecutionResult& expected =
            planned.child_results[child_index];
        if(!selected_names.insert(actual.identity.package_name).second) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    DuplicateSelectedChild,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal field name "PackageBase".
                    "{} source-build result contains a duplicate selected child.",
                    PACKAGE_BASE_FIELD_NAME),
                child_index, actual.identity.package_name);
        }
        if(actual.identity.package_name != expected.required_package_name) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    SelectedArtifactIdentityMismatch,
                localization::translate_message(
                    "Selected artifact identity does not match the prepared required child order."),
                child_index, actual.identity.package_name);
        }
        if(actual.identity.full_version.empty()) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    EmptySelectedArtifactVersion,
                localization::translate_message(
                    "Selected artifact result has an empty full version."),
                child_index, actual.identity.package_name);
        }
        if(actual.desired_reason != expected.desired_install_reason) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    DesiredInstallReasonMismatch,
                localization::translate_message(
                    "Selected artifact desired install reason does not match preparation."),
                child_index, actual.identity.package_name);
        }
        switch(actual.outcome) {
            case ArtifactInstallExecutionOutcome::Installed:
            case ArtifactInstallExecutionOutcome::SkippedAsNeeded:
                break;
            default:
                return correlation_failure(
                    AurUpdateExecutionCorrelationFailureReason::
                        UnknownChildOutcome,
                    localization::translate_message(
                        "Selected artifact result has an unknown execution outcome."),
                    child_index, actual.identity.package_name);
        }
    }

    std::set<std::string> unselected_names;
    for(const ArtifactPackageIdentity& identity :
        completed.unselected_artifacts()) {
        if(identity.package_name.empty() || identity.full_version.empty()) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    InvalidUnselectedArtifactIdentity,
                localization::translate_message(
                    "Unselected artifact identity has an empty package name or full version."),
                std::nullopt, identity.package_name);
        }
        if(selected_names.contains(identity.package_name)) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    SelectedAndUnselectedIdentityOverlap,
                localization::translate_message(
                    "Unselected artifact identity overlaps a selected child."),
                std::nullopt, identity.package_name);
        }
        if(!unselected_names.insert(identity.package_name).second) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    DuplicateUnselectedArtifactIdentity,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal field name "PackageBase".
                    "{} source-build result contains a duplicate unselected artifact identity.",
                    PACKAGE_BASE_FIELD_NAME),
                std::nullopt, identity.package_name);
        }
    }
    return std::nullopt;
}

std::optional<AurUpdateExecutionCorrelationFailure>
validate_package_transaction_failure(
    const PackageBaseArtifactInstallTransactionError& error,
    const AurUpdateWorkItemExecutionResult& planned) {
    if(error.package_base() != planned.package_base) {
        return correlation_failure(
            AurUpdateExecutionCorrelationFailureReason::
                PackageBaseMismatch,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal field name "PackageBase".
                "Package transaction failure does not match the prepared {}.",
                PACKAGE_BASE_FIELD_NAME));
    }
    if(error.attempts().size() != planned.child_results.size()) {
        return correlation_failure(
            error.attempts().size() < planned.child_results.size()
                ? AurUpdateExecutionCorrelationFailureReason::
                      MissingSelectedChild
                : AurUpdateExecutionCorrelationFailureReason::
                      ExtraSelectedChild,
            localization::translate_message(
                "Package transaction attempt count does not match the prepared required children."));
    }

    std::set<std::string> attempted_names;
    for(std::size_t child_index = 0;
        child_index < error.attempts().size(); ++child_index) {
        const PackageBaseArtifactInstallTransactionAttempt& attempt =
            error.attempts()[child_index];
        const AurUpdateChildExecutionResult& expected =
            planned.child_results[child_index];
        if(!attempted_names.insert(attempt.identity.package_name).second) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    DuplicateSelectedChild,
                localization::translate_message(
                    "Package transaction failure contains a duplicate attempted child."),
                child_index, attempt.identity.package_name);
        }
        if(attempt.identity.package_name != expected.required_package_name) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    SelectedArtifactIdentityMismatch,
                localization::translate_message(
                    "Package transaction attempted identity does not match the prepared required child order."),
                child_index, attempt.identity.package_name);
        }
        if(attempt.identity.full_version.empty()) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    EmptySelectedArtifactVersion,
                localization::translate_message(
                    "Package transaction attempt has an empty full version."),
                child_index, attempt.identity.package_name);
        }
        if(attempt.desired_reason != expected.desired_install_reason) {
            return correlation_failure(
                AurUpdateExecutionCorrelationFailureReason::
                    DesiredInstallReasonMismatch,
                localization::translate_message(
                    "Package transaction attempt desired reason does not match preparation."),
                child_index, attempt.identity.package_name);
        }
    }
    return std::nullopt;
}

AurUpdatePackageTransactionFailureCategory map_transaction_failure_kind(
    PackageBaseArtifactInstallTransactionFailureKind kind) noexcept {
    switch(kind) {
        case PackageBaseArtifactInstallTransactionFailureKind::NonzeroExit:
            return AurUpdatePackageTransactionFailureCategory::CommandFailed;
        case PackageBaseArtifactInstallTransactionFailureKind::ProcessException:
            return AurUpdatePackageTransactionFailureCategory::
                CommandExecutionFailed;
        case PackageBaseArtifactInstallTransactionFailureKind::UnknownException:
            return AurUpdatePackageTransactionFailureCategory::Other;
    }
    return AurUpdatePackageTransactionFailureCategory::Other;
}

AurUpdateSourceBuildFailureCategory map_source_build_failure_phase(
    SeparatedPackageBaseSourceBuildFailurePhase phase) noexcept {
    switch(phase) {
        case SeparatedPackageBaseSourceBuildFailurePhase::Build:
            return AurUpdateSourceBuildFailureCategory::Build;
        case SeparatedPackageBaseSourceBuildFailurePhase::ArtifactValidation:
            return AurUpdateSourceBuildFailureCategory::ArtifactValidation;
        case SeparatedPackageBaseSourceBuildFailurePhase::ArtifactIdentity:
            return AurUpdateSourceBuildFailureCategory::ArtifactIdentity;
        case SeparatedPackageBaseSourceBuildFailurePhase::InstallPreparation:
            return AurUpdateSourceBuildFailureCategory::InstallPreparation;
        case SeparatedPackageBaseSourceBuildFailurePhase::InstallTransaction:
            return AurUpdateSourceBuildFailureCategory::InstallTransaction;
    }
    return AurUpdateSourceBuildFailureCategory::Other;
}

AurUpdateChildExecutionStatus completed_child_status(
    ArtifactInstallExecutionOutcome outcome,
    bool cleanup_failed) {
    switch(outcome) {
        case ArtifactInstallExecutionOutcome::Installed:
            return cleanup_failed
                       ? AurUpdateChildExecutionStatus::InstalledCleanupFailed
                       : AurUpdateChildExecutionStatus::Installed;
        case ArtifactInstallExecutionOutcome::SkippedAsNeeded:
            return cleanup_failed
                       ? AurUpdateChildExecutionStatus::SkippedAsNeededCleanupFailed
                       : AurUpdateChildExecutionStatus::SkippedAsNeeded;
    }
    throw std::logic_error(localization::translate_message(
        "Unknown artifact install execution outcome."));
}

void promote_completed_result(
    AurUpdateWorkItemExecutionResult& work_item_result,
    PackageBaseSourceBuildExecutionResult completed,
    bool cleanup_failed) {
    work_item_result.production_outcome =
        completed.production_outcome();
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children =
        std::move(completed).release_selected_children();
    std::vector<ArtifactPackageIdentity> unselected_artifacts =
        std::move(completed).release_unselected_artifacts();
    bool installed_any = false;
    for(std::size_t child_index = 0;
        child_index < selected_children.size(); ++child_index) {
        PackageBaseSourceBuildSelectedResult& selected =
            selected_children[child_index];
        AurUpdateChildExecutionResult& child =
            work_item_result.child_results[child_index];
        child.selected_artifact.emplace(std::move(selected.identity));
        child.status = completed_child_status(
            selected.outcome, cleanup_failed);
        installed_any = installed_any ||
                        selected.outcome == ArtifactInstallExecutionOutcome::Installed;
    }
    work_item_result.unselected_artifacts = std::move(unselected_artifacts);
    work_item_result.status = cleanup_failed
                                  ? (installed_any
                                         ? AurUpdateWorkItemExecutionStatus::
                                               UpdatedCleanupFailed
                                         : AurUpdateWorkItemExecutionStatus::
                                               NoChangeCleanupFailed)
                                  : (installed_any ? AurUpdateWorkItemExecutionStatus::Updated
                                                   : AurUpdateWorkItemExecutionStatus::NoChange);
    work_item_result.failure_kind = cleanup_failed
                                        ? AurUpdateWorkItemFailureKind::
                                              CleanupFailedAfterPackageTransaction
                                        : AurUpdateWorkItemFailureKind::None;
    work_item_result.failure_detail = std::monostate{};
}

void record_correlation_failure(
    AurUpdateWorkItemExecutionResult& work_item_result,
    AurUpdateExecutionCorrelationFailure failure) {
    work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
    work_item_result.failure_kind =
        AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
    work_item_result.diagnostic = failure.diagnostic;
    work_item_result.failure_detail.emplace<
        AurUpdateExecutionCorrelationFailure>(std::move(failure));
}

std::optional<AurUpdateExecutionCorrelationFailure>
validate_preparation_failure(
    const SeparatedPackageBaseSourceBuildPreparationError& error,
    const AurUpdateWorkItemExecutionResult& planned) {
    const std::string* failure_package_base = nullptr;
    if(const auto* selection = error.selection_failure()) {
        failure_package_base = &selection->package_base;
    } else if(const auto* mixed = error.mixed_reason_failure()) {
        failure_package_base = &mixed->package_base;
    }

    if(failure_package_base == nullptr ||
       *failure_package_base == planned.package_base) {
        return std::nullopt;
    }
    return correlation_failure(
        AurUpdateExecutionCorrelationFailureReason::PackageBaseMismatch,
        localization::format_translated_message(
            // TRANSLATORS: Both placeholders are the literal field
            // name "PackageBase".
            "{} install preparation failure does not match the prepared {}.",
            PACKAGE_BASE_FIELD_NAME,
            PACKAGE_BASE_FIELD_NAME),
        std::nullopt, *failure_package_base);
}

void record_preparation_failure(
    AurUpdateWorkItemExecutionResult& work_item_result,
    const SeparatedPackageBaseSourceBuildPreparationError& error) {
    work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
    work_item_result.failure_kind =
        AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
    work_item_result.diagnostic = error.what();
    work_item_result.production_outcome = error.production_outcome();
    if(const auto* selection = error.selection_failure()) {
        work_item_result.failure_detail.emplace<
            PackageBaseArtifactIdentitySelectionFailure>(*selection);
    } else if(const auto* mixed = error.mixed_reason_failure()) {
        work_item_result.failure_detail.emplace<
            MixedPackageBaseInstallReasonUnsupported>(*mixed);
    } else {
        work_item_result.failure_detail.emplace<
            AurUpdateSourceBuildFailureSnapshot>(
            AurUpdateSourceBuildFailureSnapshot{
                AurUpdateSourceBuildFailureCategory::Other,
                error.what(), std::nullopt});
    }
}

void record_phase_failure(
    AurUpdateWorkItemExecutionResult& work_item_result,
    const SeparatedPackageBaseSourceBuildPhaseError& error) {
    work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
    work_item_result.failure_kind =
        AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
    work_item_result.diagnostic = error.what();
    work_item_result.production_outcome = error.production_outcome();
    if(error.package_metadata_failure().has_value()) {
        work_item_result.failure_detail.emplace<PackageMetadataFailure>(
            *error.package_metadata_failure());
        return;
    }
    work_item_result.failure_detail.emplace<
        AurUpdateSourceBuildFailureSnapshot>(
        AurUpdateSourceBuildFailureSnapshot{
            map_source_build_failure_phase(error.phase()),
            error.what(), error.reviewed_source_failure()});
}

void record_transaction_failure(
    AurUpdateWorkItemExecutionResult& work_item_result,
    PackageBaseArtifactInstallTransactionError& error) {
    const AurUpdatePackageTransactionFailureCategory category =
        map_transaction_failure_kind(error.failure_kind());
    const std::optional<int> exit_code = error.exit_code();
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts =
        std::move(error).release_attempts();
    AurUpdatePackageTransactionFailureSnapshot failure{
        category, std::move(attempts), exit_code, error.what()};
    work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
    work_item_result.failure_kind =
        AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
    work_item_result.transaction_failure = failure;
    work_item_result.production_outcome = error.production_outcome();
    work_item_result.diagnostic = error.what();
    work_item_result.failure_detail.emplace<
        AurUpdatePackageTransactionFailureSnapshot>(
        std::move(failure));
}

void retain_transaction_failure_evidence(
    AurUpdateWorkItemExecutionResult& work_item_result,
    const PackageBaseArtifactInstallTransactionError& error) {
    work_item_result.transaction_failure =
        AurUpdatePackageTransactionFailureSnapshot{
            map_transaction_failure_kind(error.failure_kind()),
            error.attempts(), error.exit_code(), error.what()};
    work_item_result.production_outcome = error.production_outcome();
}

} // namespace

bool AurUpdateSourceBuildExecutionResult::is_success() const noexcept {
    return status == AurUpdateInvocationExecutionStatus::Completed &&
           selected_repository_provider_transaction.is_success();
}

PackageStateChange
AurUpdateSourceBuildExecutionResult::package_state_change()
    const noexcept {
    if(selected_repository_provider_transaction.package_state_change ==
       PackageStateChange::Changed) {
        return PackageStateChange::Changed;
    }
    bool uncertain = selected_repository_provider_transaction.package_state_change == PackageStateChange::Unknown;
    for(const auto& work_item_result : work_item_results) {
        if(work_item_result.devel_execution && work_item_result.devel_execution->operation &&
           *work_item_result.devel_execution->operation != DevelSourceArtifactInstallOperation::NotAttempted) uncertain = true;
        for(const auto& child : work_item_result.child_results) {
            if(child.status == AurUpdateChildExecutionStatus::Installed ||
               child.status == AurUpdateChildExecutionStatus::
                                   InstalledCleanupFailed) {
                return PackageStateChange::Changed;
            }
        }
    }
    return uncertain
               ? PackageStateChange::Unknown
               : PackageStateChange::NoChange;
}

bool AurUpdateSourceBuildExecutionResult::changed_package_state()
    const noexcept {
    return package_state_change() == PackageStateChange::Changed;
}

bool AurUpdateSourceBuildExecutionResult::has_not_attempted_items()
    const noexcept {
    for(const auto& work_item_result : work_item_results) {
        if(work_item_result.status ==
           AurUpdateWorkItemExecutionStatus::NotAttempted) {
            return true;
        }
    }
    return false;
}

bool AurUpdateSourceBuildExecutionResult::has_cleanup_failure()
    const noexcept {
    for(const auto& work_item_result : work_item_results) {
        if(work_item_result.status ==
               AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed ||
           work_item_result.status == AurUpdateWorkItemExecutionStatus::
                                          NoChangeCleanupFailed) {
            return true;
        }
    }
    return false;
}

std::optional<std::size_t>
AurUpdateSourceBuildExecutionResult::stopped_work_item_index()
    const noexcept {
    for(const auto& work_item_result : work_item_results) {
        if(work_item_result.status == AurUpdateWorkItemExecutionStatus::Failed ||
           work_item_result.status ==
               AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed ||
           work_item_result.status == AurUpdateWorkItemExecutionStatus::
                                          NoChangeCleanupFailed) {
            return work_item_result.work_item_index;
        }
    }
    return std::nullopt;
}

AurUpdateSourceBuildExecutionResult
execute_prepared_aur_update_source_build_invocation(
    PreparedAurUpdateSourceBuildInvocation invocation,
    const AppConfig& config) {
    // POLICY(#267): capability validity/correlationは最初のexecutor callより前に
    // 全件検証し、moved-from/replayed snapshotをempty successへ潰さない。
    PreparedProductionSourceBuildInvocation& production_invocation =
        invocation.production_invocation_;
    require_valid_prepared_invocation(invocation, production_invocation);
    const std::vector<AurUpdatePreparedWorkItemAttribution>& attributions =
        invocation.work_item_attributions();

    AurUpdateSourceBuildExecutionResult result;
    result.work_item_results.reserve(production_invocation.work_items.size());
    for(std::size_t index = 0;
        index < production_invocation.work_items.size(); ++index) {
        result.work_item_results.push_back(make_not_attempted_result(
            production_invocation.work_items[index], attributions[index]));
    }
    // choice/static preflight完了後、高コストなpackage transactionより先に
    // shared cache capabilityを確定する。
    activate_production_source_build_cache(production_invocation);

    // POLICY(#272): source executionより前にexact provider transactionを
    // invocation全体で1回だけ行う。
    result.selected_repository_provider_transaction =
        execute_selected_repository_provider_transaction(
            production_invocation, config);
    if(!result.selected_repository_provider_transaction.is_success()) {
        result.status = AurUpdateInvocationExecutionStatus::
            StoppedOnProviderTransactionFailure;
        return result;
    }

    // POLICY(#267/#268): mutation前に全work item/required child snapshotを
    // owned化済み。成功時だけ次のPackageBaseへ進み、最初のfailureで停止する。
    for(std::size_t index = 0;
        index < production_invocation.work_items.size(); ++index) {
        AurUpdateWorkItemExecutionResult& work_item_result =
            result.work_item_results[index];
        try {
            auto execution = execute_prepared_package_base_source_build_work_item_typed(
                production_invocation.work_items[index],
                production_invocation.database_paths,
                config);
            if(auto* devel = std::get_if<ReviewedDevelExecutionSnapshot>(&execution)) {
                work_item_result.devel_execution.emplace(std::move(*devel));
                const auto& observed = *work_item_result.devel_execution;
                work_item_result.status = observed.complete ? AurUpdateWorkItemExecutionStatus::Updated : AurUpdateWorkItemExecutionStatus::Failed;
                work_item_result.failure_kind = observed.complete ? AurUpdateWorkItemFailureKind::None : AurUpdateWorkItemFailureKind::AuthoritativeExecutionIncomplete;
                if(observed.artifact) {
                    if(work_item_result.child_results.size() != 1 || observed.artifact->package_name != work_item_result.child_results.front().required_package_name)
                        throw std::logic_error("Authoritative execution child correlation failed.");
                    work_item_result.child_results.front().selected_artifact = observed.artifact;
                    work_item_result.child_results.front().status = AurUpdateChildExecutionStatus::Installed;
                }
                work_item_result.production_outcome = observed.production_outcome;
                if(!observed.complete) {
                    work_item_result.diagnostic = "Authoritative devel execution incomplete; installation/publication details retained.";
                    result.status = AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure;
                    return result;
                }
                continue;
            }
            auto completed = std::get<PackageBaseSourceBuildExecutionResult>(std::move(execution));
            work_item_result.production_outcome =
                completed.production_outcome();
            if(auto failure = validate_completed_package_base_result(
                   completed, work_item_result)) {
                record_correlation_failure(
                    work_item_result, std::move(*failure));
                result.status = AurUpdateInvocationExecutionStatus::
                    StoppedOnWorkItemFailure;
                return result;
            }
            promote_completed_result(
                work_item_result, std::move(completed), false);
        } catch(SeparatedPackageBaseSourceBuildCleanupError& error) {
            work_item_result.production_outcome =
                error.result().production_outcome();
            if(auto failure = validate_completed_package_base_result(
                   error.result(), work_item_result)) {
                record_correlation_failure(
                    work_item_result, std::move(*failure));
                result.status = AurUpdateInvocationExecutionStatus::
                    StoppedOnWorkItemFailure;
                return result;
            }
            work_item_result.diagnostic = error.what();
            promote_completed_result(
                work_item_result,
                std::move(error).release_result(), true);
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedAfterPackageCleanupFailure;
            return result;
        } catch(const SeparatedPackageBaseSourceBuildPreparationError& error) {
            if(auto failure = validate_preparation_failure(
                   error, work_item_result)) {
                record_correlation_failure(
                    work_item_result, std::move(*failure));
            } else {
                record_preparation_failure(work_item_result, error);
            }
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
            return result;
        } catch(SeparatedPackageBaseSourceBuildPhaseError& error) {
            record_phase_failure(work_item_result, error);
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
            return result;
        } catch(PackageBaseArtifactInstallTransactionError& error) {
            if(auto failure = validate_package_transaction_failure(
                   error, work_item_result)) {
                retain_transaction_failure_evidence(
                    work_item_result, error);
                record_correlation_failure(
                    work_item_result, std::move(*failure));
            } else {
                record_transaction_failure(work_item_result, error);
            }
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
            return result;
        } catch(const PackageMetadataError& error) {
            work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
            work_item_result.failure_kind =
                AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
            work_item_result.failure_detail.emplace<PackageMetadataFailure>(
                error.failure());
            work_item_result.diagnostic = error.what();
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
            return result;
        } catch(const TrustedCacheError&) {
            // Aggregate ownerがcache authorityのtyped payloadを転写できるよう、
            // ordinary work-item failureへcontainせずrethrowする。
            throw;
        } catch(const ConfirmationOperationStopped&) {
            throw;
        } catch(const std::exception& error) {
            work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
            work_item_result.failure_kind =
                AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
            work_item_result.failure_detail.emplace<
                AurUpdateSourceBuildFailureSnapshot>(
                AurUpdateSourceBuildFailureSnapshot{
                    AurUpdateSourceBuildFailureCategory::Other,
                    error.what(), std::nullopt});
            work_item_result.diagnostic = error.what();
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
            return result;
        } catch(...) {
            work_item_result.status = AurUpdateWorkItemExecutionStatus::Failed;
            work_item_result.failure_kind =
                AurUpdateWorkItemFailureKind::UnknownException;
            work_item_result.failure_detail = std::monostate{};
            work_item_result.diagnostic = unknown_exception_diagnostic();
            result.status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
            return result;
        }
    }

    return result;
}
