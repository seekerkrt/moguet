#include "app_config.hpp"
#include "aur_update_execution_preflight.hpp"
#include "aur_update_execution_preparation.hpp"
#include "aur_update_execution_runner.hpp"
#include "aur_update_operation_result.hpp"
#include "aur_update_query.hpp"
#include "filtered_aur_update_operation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// CLI command boundaryだけをdeterministicに検証するscenario-driven stub。
// POLICY(#267): queryからreducerまでのproduction実装は個別testで検証済みのため、
// このbinaryでは公開typed APIを保ったまま外部状態とmutationを完全に遮断する。
namespace {

const std::string& scenario() {
    static const std::string s_scenario = [] {
        const char* value = std::getenv("MOGUET_TEST_AUR_UPDATE_SCENARIO");
        if(value == nullptr || value[0] == '\0') {
            throw std::logic_error(
                "MOGUET_TEST_AUR_UPDATE_SCENARIO is required.");
        }
        return std::string(value);
    }();
    return s_scenario;
}

void append_event(const std::string& event) {
    const char* event_log_path = std::getenv("MOGUET_TEST_COMMAND_LOG");
    if(event_log_path == nullptr || event_log_path[0] == '\0') {
        throw std::logic_error("MOGUET_TEST_COMMAND_LOG is required.");
    }

    std::ofstream event_log(event_log_path, std::ios::app);
    if(!event_log) {
        throw std::runtime_error("Cannot open the AUR update command event log.");
    }
    event_log << event << '\n';
}

const char* bool_text(bool value) {
    return value ? "true" : "false";
}

ProductionSourceBuildProvenance reviewed_source_provenance(
    ProductionReviewedSourceOutcome outcome,
    std::uint64_t generation,
    ReviewedSourcePublicationStatus publication_status =
        ReviewedSourcePublicationStatus::Published,
    ReviewedSourceEditorOverlayStatus editor_overlay =
        ReviewedSourceEditorOverlayStatus::None) {
    ProductionSourceBuildProvenance provenance;
    provenance.review_status = ProductionSourceReviewStatus::Reviewed;
    provenance.editor_overlay = editor_overlay;
    provenance.reviewed_upstream_base_revision =
        SourceRevisionIdentity::git_commit(
            "2222222222222222222222222222222222222222");
    provenance.publication_status = publication_status;
    provenance.reviewed_outcome = outcome;
    provenance.reviewed_state_generation = generation;
    return provenance;
}

ProductionSourceBuildProvenance compatibility_source_provenance(
    ReviewedSourceCompatibilityBuildReason reason) {
    ProductionSourceBuildProvenance provenance;
    provenance.review_status =
        ProductionSourceReviewStatus::CompatibilityWithoutReview;
    provenance.compatibility_reason = reason;
    return provenance;
}

ProductionSourceBuildStagedOutcome production_outcome(
    ProductionSourceBuildProvenance provenance,
    ProductionSourceBuildCommandOutcome build_outcome =
        ProductionSourceBuildCommandOutcome::Succeeded,
    ProductionSourceInstallOutcome install_outcome =
        ProductionSourceInstallOutcome::Succeeded) {
    return ProductionSourceBuildStagedOutcome{
        std::move(provenance), build_outcome, install_outcome};
}

std::string config_snapshot(const AppConfig& config) {
    return "noedit=" + std::string(bool_text(config.user_config.review.pkgbuild == ReviewPolicy::Skip)) +
           " nodiff=" + bool_text(config.user_config.review.diff == ReviewPolicy::Skip) +
           " noconfirm=" + bool_text(config.no_confirm) +
           " rebuild=" + bool_text(config.user_config.build.mode == BuildMode::Rebuild) +
           " cleanbuild=" + bool_text(config.user_config.build.mode == BuildMode::Clean) +
           " rmdeps=" + bool_text(config.rm_deps);
}

AurUpdatePlanEntry make_plan_entry(
    std::string name,
    AurUpdateClassification classification,
    std::string package_base = {}) {
    AurUpdatePlanEntry entry;
    entry.installed_name = std::move(name);
    entry.installed_version = "1.0-1";
    entry.install_reason = InstalledPackageReason::Explicit;
    entry.classification = classification;

    if(classification == AurUpdateClassification::UpdateAvailable ||
       classification == AurUpdateClassification::UpToDate ||
       classification == AurUpdateClassification::VersionComparisonUnavailable) {
        AurUpdateRemotePackage remote;
        remote.aur_name = entry.installed_name;
        remote.package_base = package_base.empty() ? entry.installed_name
                                                   : std::move(package_base);
        remote.version = classification == AurUpdateClassification::UpToDate
                             ? "1.0-1"
                             : "2.0-1";
        remote.version_relation =
            classification == AurUpdateClassification::UpToDate
                ? AurVersionRelation::SameAsInstalled
            : classification ==
                    AurUpdateClassification::VersionComparisonUnavailable
                ? AurVersionRelation::Unavailable
                : AurVersionRelation::NewerThanInstalled;
        entry.aur_package = std::move(remote);
    }
    return entry;
}

AurUpdatePlanEntry make_requires_check_plan_entry(
    const std::string& package_name) {
    AurUpdatePlanEntry entry = make_plan_entry(
        package_name, AurUpdateClassification::UpToDate);
    entry.devel_classification = DevelPackageClassification::classify(
        DevelPackageSuffixEvidence::classify(
            package_name, {package_name}));
    entry.devel_assessment = DevelUpdateAssessment::requires_check(
        DevelRequiresCheckReason::SuffixCandidateOnly);
    return entry;
}

AurUpdateExecutionIssue make_preflight_issue(
    AurUpdateExecutionReason reason,
    const std::string& package_name,
    std::string diagnostic,
    std::optional<DevelRequiresCheckReason>
        devel_requires_check_reason = std::nullopt) {
    AurUpdateExecutionIssue issue;
    issue.reason = reason;
    issue.package_name = package_name;
    issue.diagnostic = std::move(diagnostic);
    issue.devel_requires_check_reason = devel_requires_check_reason;
    return issue;
}

AurUpdatePreparationIssue make_preparation_issue(
    AurUpdatePreparationReason reason,
    std::size_t update_plan_index,
    std::string diagnostic) {
    AurUpdatePreparationIssue issue;
    issue.reason = reason;
    issue.affected_update_plan_indices = {update_plan_index};
    issue.diagnostic = std::move(diagnostic);
    return issue;
}

AurUpdateOperationTargetResult make_target_result(
    const AurUpdateExecutionTarget& target,
    AurUpdateOperationTargetStatus status) {
    AurUpdateOperationTargetResult result;
    result.update_plan_index = target.update_plan_index;
    result.update = target.update;
    if(target.update.aur_package.has_value()) {
        result.package_base = target.update.aur_package->package_base;
    }
    result.status = status;
    result.preflight_issues = target.issues;
    result.skip_kind = target.skip_kind;
    return result;
}

AurUpdateOperationExecutionContribution make_contribution(
    std::size_t work_item_index,
    const std::string& package_name,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic = std::nullopt) {
    AurUpdateOperationExecutionContribution contribution;
    contribution.work_item_index = work_item_index;
    contribution.package_name = package_name;
    contribution.package_base = package_name;
    contribution.status = status;
    contribution.failure_kind = failure_kind;
    contribution.diagnostic = std::move(diagnostic);
    return contribution;
}

AurUpdateWorkItemExecutionResult make_work_item_result(
    std::size_t work_item_index,
    std::size_t update_plan_index,
    const std::string& package_name,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic = std::nullopt) {
    AurUpdateWorkItemExecutionResult result;
    result.work_item_index = work_item_index;
    result.build_plan_order_index = work_item_index;
    result.package_name = package_name;
    result.package_base = package_name;
    result.plan_package_names = {package_name};
    result.affected_update_plan_indices = {update_plan_index};
    result.affected_roots = {{update_plan_index, package_name}};
    result.status = status;
    result.failure_kind = failure_kind;
    result.diagnostic = std::move(diagnostic);

    AurUpdateChildExecutionResult child;
    child.work_item_index = work_item_index;
    child.build_plan_order_index = work_item_index;
    child.required_child_index = 0;
    child.package_base = package_name;
    child.required_package_name = package_name;
    child.desired_install_reason = DesiredInstallReason::Explicit;
    child.affected_update_plan_indices = {update_plan_index};
    child.affected_roots = {{update_plan_index, package_name}};
    child.roles = {PackageRole::Root};
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::Updated:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::Installed;
            break;
        case AurUpdateWorkItemExecutionStatus::NoChange:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::SkippedAsNeeded;
            break;
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status =
                AurUpdateChildExecutionStatus::InstalledCleanupFailed;
            break;
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
            child.selected_artifact =
                ArtifactPackageIdentity{package_name, "2.0-1"};
            child.status = AurUpdateChildExecutionStatus::
                SkippedAsNeededCleanupFailed;
            break;
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            child.status = AurUpdateChildExecutionStatus::NotAttempted;
            break;
    }
    result.child_results.push_back(std::move(child));
    return result;
}

AurUpdateChildExecutionResult make_child_result(
    std::size_t work_item_index,
    std::size_t required_child_index,
    const std::string& package_base,
    std::string package_name,
    DesiredInstallReason desired_reason,
    AurUpdateChildExecutionStatus status,
    std::size_t update_plan_index,
    std::string full_version = "3.0-1") {
    AurUpdateChildExecutionResult child;
    child.work_item_index = work_item_index;
    child.build_plan_order_index = work_item_index;
    child.required_child_index = required_child_index;
    child.package_base = package_base;
    child.required_package_name = std::move(package_name);
    child.desired_install_reason = desired_reason;
    child.affected_update_plan_indices = {update_plan_index};
    child.affected_roots = {{update_plan_index, child.required_package_name}};
    child.roles = {desired_reason == DesiredInstallReason::Explicit
                       ? PackageRole::Root
                       : PackageRole::RuntimeDependency};
    child.status = status;
    if(status != AurUpdateChildExecutionStatus::NotAttempted) {
        child.selected_artifact = ArtifactPackageIdentity{
            child.required_package_name, std::move(full_version)};
    }
    return child;
}

AurUpdateWorkItemExecutionResult make_multi_child_work_item(
    std::size_t work_item_index,
    std::string package_base,
    std::vector<AurUpdateChildExecutionResult> children,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic = std::nullopt) {
    AurUpdateWorkItemExecutionResult result;
    result.work_item_index = work_item_index;
    result.build_plan_order_index = work_item_index;
    result.package_base = std::move(package_base);
    result.status = status;
    result.failure_kind = failure_kind;
    result.diagnostic = std::move(diagnostic);
    for(const AurUpdateChildExecutionResult& child : children) {
        result.plan_package_names.push_back(child.required_package_name);
        result.affected_update_plan_indices.insert(
            result.affected_update_plan_indices.end(),
            child.affected_update_plan_indices.begin(),
            child.affected_update_plan_indices.end());
        result.affected_roots.insert(
            result.affected_roots.end(),
            child.affected_roots.begin(), child.affected_roots.end());
    }
    if(children.size() == 1) {
        result.package_name = children.front().required_package_name;
    }
    result.child_results = std::move(children);
    return result;
}

AurUpdateOperationExecutionContribution make_child_contribution(
    const AurUpdateChildExecutionResult& child,
    AurUpdateWorkItemExecutionStatus status,
    AurUpdateWorkItemFailureKind failure_kind) {
    AurUpdateOperationExecutionContribution contribution;
    contribution.work_item_index = child.work_item_index;
    contribution.required_child_index = child.required_child_index;
    contribution.package_name = child.required_package_name;
    contribution.package_base = child.package_base;
    contribution.selected_artifact = child.selected_artifact;
    contribution.desired_install_reason = child.desired_install_reason;
    contribution.affected_roots = child.affected_roots;
    contribution.roles = child.roles;
    contribution.status = status;
    contribution.failure_kind = failure_kind;
    return contribution;
}

void set_execution_failure(
    AurUpdateOperationTargetResult& target,
    std::size_t work_item_index,
    AurUpdateWorkItemExecutionStatus work_item_status,
    AurUpdateWorkItemFailureKind failure_kind,
    std::optional<std::string> diagnostic) {
    target.execution_work_item_index = work_item_index;
    target.execution_failure_kind = failure_kind;
    target.execution_diagnostic = diagnostic;
    target.execution_contributions.push_back(make_contribution(
        work_item_index,
        target.update.installed_name,
        work_item_status,
        failure_kind,
        std::move(diagnostic)));
}

bool is_blocking_status(AurUpdateExecutionTargetStatus status) {
    return status == AurUpdateExecutionTargetStatus::Unsupported ||
           status == AurUpdateExecutionTargetStatus::Incomplete;
}

bool is_completed_failure_matrix_scenario(const std::string& test_scenario) {
    return test_scenario == "completed-preparation-issue" ||
           test_scenario == "completed-reduction-issue" ||
           test_scenario == "completed-unsupported-target" ||
           test_scenario == "completed-incomplete-target" ||
           test_scenario == "completed-execution-failure" ||
           test_scenario == "completed-invocation-execution-failure" ||
           test_scenario == "completed-invocation-cleanup-failure" ||
           test_scenario == "completed-missing-invocation-status" ||
           test_scenario == "completed-cleanup-failure" ||
           test_scenario == "completed-not-attempted";
}

bool target_status_is_success(
    AurUpdateOperationTargetStatus status) noexcept {
    switch(status) {
        case AurUpdateOperationTargetStatus::Updated:
        case AurUpdateOperationTargetStatus::NoChange:
        case AurUpdateOperationTargetStatus::Skipped:
            return true;
        case AurUpdateOperationTargetStatus::Unsupported:
        case AurUpdateOperationTargetStatus::Incomplete:
        case AurUpdateOperationTargetStatus::Failed:
        case AurUpdateOperationTargetStatus::UpdatedCleanupFailed:
        case AurUpdateOperationTargetStatus::NoChangeCleanupFailed:
        case AurUpdateOperationTargetStatus::NotAttempted:
            return false;
    }
    return false;
}

bool work_item_status_is_success(
    AurUpdateWorkItemExecutionStatus status) noexcept {
    switch(status) {
        case AurUpdateWorkItemExecutionStatus::Updated:
        case AurUpdateWorkItemExecutionStatus::NoChange:
            return true;
        case AurUpdateWorkItemExecutionStatus::Failed:
        case AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed:
        case AurUpdateWorkItemExecutionStatus::NotAttempted:
            return false;
    }
    return false;
}

bool invocation_status_matches_operation(
    AurUpdateOperationStatus operation_status,
    const std::optional<AurUpdateInvocationExecutionStatus>& status) noexcept {
    switch(operation_status) {
        case AurUpdateOperationStatus::NoUpdates:
            return !status.has_value();
        case AurUpdateOperationStatus::Completed:
            return status.has_value() &&
                   *status == AurUpdateInvocationExecutionStatus::Completed;
        case AurUpdateOperationStatus::BlockedBeforeExecution:
        case AurUpdateOperationStatus::StoppedOnProviderTransactionFailure:
        case AurUpdateOperationStatus::StoppedOnWorkItemFailure:
        case AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure:
        case AurUpdateOperationStatus::InconsistentResult:
            return false;
    }
    return false;
}

bool reduced_result_is_defensively_successful(
    const AurUpdateOperationResult& result) noexcept {
    if(!result.is_success() ||
       !invocation_status_matches_operation(
           result.status, result.execution_status) ||
       !result.preparation_issues.empty() || !result.reduction_issues.empty() ||
       result.has_blocking_targets() || result.has_cleanup_failure() ||
       result.has_not_attempted_targets()) {
        return false;
    }
    if(!std::all_of(
           result.targets.begin(), result.targets.end(),
           [](const AurUpdateOperationTargetResult& target) {
               return target_status_is_success(target.status);
           })) {
        return false;
    }
    return std::all_of(
        result.execution_work_items.begin(),
        result.execution_work_items.end(),
        [](const AurUpdateWorkItemExecutionResult& work_item) {
            return work_item_status_is_success(work_item.status) &&
                   work_item.failure_kind ==
                       AurUpdateWorkItemFailureKind::None;
        });
}

} // namespace

AurUpdateQueryResult query_installed_aur_updates() {
    append_event("query");

    AurUpdateQueryResult query;
    const std::string& test_scenario = scenario();
    if(test_scenario == "no-installed-foreign") return query;
    if(test_scenario == "all-up-to-date") {
        query.plan.entries.push_back(make_plan_entry(
            "up-to-date-pkg", AurUpdateClassification::UpToDate));
        return query;
    }
    if(test_scenario == "devel-requires-check") {
        query.plan.entries.push_back(
            make_requires_check_plan_entry("manual-check-git"));
        return query;
    }
    if(test_scenario == "non-aur-foreign") {
        query.plan.entries.push_back(make_plan_entry(
            "non-aur-pkg", AurUpdateClassification::NonAurForeign));
        return query;
    }
    if(test_scenario == "metadata-unavailable") {
        query.plan.entries.push_back(make_plan_entry(
            "metadata-pkg", AurUpdateClassification::MetadataUnavailable));
        return query;
    }
    if(test_scenario == "version-comparison-unavailable") {
        query.plan.entries.push_back(make_plan_entry(
            "version-pkg",
            AurUpdateClassification::VersionComparisonUnavailable));
        return query;
    }
    if(test_scenario == "unsupported-blocker") {
        query.plan.entries.push_back(make_plan_entry(
            "unsupported-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "incomplete-blocker") {
        query.plan.entries.push_back(make_plan_entry(
            "incomplete-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "preparation-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "preparation-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "preparation-warning") {
        query.plan.entries.push_back(make_plan_entry(
            "warning-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "all-updated" ||
       test_scenario == "options-propagation") {
        query.plan.entries.push_back(make_plan_entry(
            "updated-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "all-no-change") {
        query.plan.entries.push_back(make_plan_entry(
            "no-change-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "updated-no-change-mixed") {
        query.plan.entries.push_back(make_plan_entry(
            "zeta-pkg", AurUpdateClassification::UpdateAvailable));
        query.plan.entries.push_back(make_plan_entry(
            "alpha-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "split-child-success") {
        query.plan.entries.push_back(make_plan_entry(
            "split-cli",
            AurUpdateClassification::UpdateAvailable,
            "split-suite"));
        return query;
    }
    if(test_scenario == "multiple-child-mixed") {
        query.plan.entries.push_back(make_plan_entry(
            "split-main",
            AurUpdateClassification::UpdateAvailable,
            "split-suite"));
        return query;
    }
    if(test_scenario == "transaction-failure" ||
       test_scenario == "transaction-process-exception") {
        query.plan.entries.push_back(make_plan_entry(
            "tx-main",
            AurUpdateClassification::UpdateAvailable,
            "tx-suite"));
        query.plan.entries.push_back(make_plan_entry(
            "tx-later", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "cleanup-mixed") {
        query.plan.entries.push_back(make_plan_entry(
            "cleanup-main",
            AurUpdateClassification::UpdateAvailable,
            "cleanup-suite"));
        query.plan.entries.push_back(make_plan_entry(
            "cleanup-later", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "unknown-child-result" ||
       test_scenario == "incoherent-child-result") {
        query.plan.entries.push_back(make_plan_entry(
            "defensive-split",
            AurUpdateClassification::UpdateAvailable,
            "defensive-suite"));
        return query;
    }
    if(test_scenario == "ordinary-execution-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "failed-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "reviewed-published-uncertain") {
        query.plan.entries.push_back(make_plan_entry(
            "uncertain-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "reviewed-build-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "reviewed-build-pkg",
            AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "provider-transaction-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "provider-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "updated-cleanup-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "updated-cleanup-pkg",
            AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "no-change-cleanup-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "no-change-cleanup-pkg",
            AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "partial-completion") {
        query.plan.entries.push_back(make_plan_entry(
            "first-pkg", AurUpdateClassification::UpdateAvailable));
        query.plan.entries.push_back(make_plan_entry(
            "failed-pkg", AurUpdateClassification::UpdateAvailable));
        query.plan.entries.push_back(make_plan_entry(
            "later-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "reducer-inconsistency") {
        query.plan.entries.push_back(make_plan_entry(
            "inconsistent-pkg",
            AurUpdateClassification::UpdateAvailable));
        return query;
    }
    if(test_scenario == "query-recoverable-failure") {
        query.plan.entries.push_back(make_plan_entry(
            "query-survivor", AurUpdateClassification::UpdateAvailable));
        query.recoverable_failures.push_back(AurUpdateQueryFailure{
            {"query-broken", "query-also-broken"},
            "fixture RPC timeout"});
        return query;
    }
    if(is_completed_failure_matrix_scenario(test_scenario)) {
        query.plan.entries.push_back(make_plan_entry(
            "defensive-pkg", AurUpdateClassification::UpdateAvailable));
        return query;
    }

    throw std::logic_error("Unknown AUR update command test scenario: " +
                           test_scenario);
}

AurUpdateQueryResult query_aur_updates_for_foreign_inventory(
    ForeignPackageInventory) {
    throw std::logic_error(
        "System AUR update queries are outside the upgrade-aur command fixture.");
}

AurUpdateExecutionPreflight resolve_aur_update_execution_preflight(
    const AurUpdatePlan& update_plan,
    DevelRequiresCheckPolicy devel_requires_check_policy) {
    if(devel_requires_check_policy !=
       DevelRequiresCheckPolicy::BlockOperation) {
        throw std::logic_error(
            "AUR update command did not request BlockOperation RequiresCheck policy.");
    }
    append_event("preflight");

    AurUpdateExecutionPreflight preflight;
    preflight.devel_requires_check_policy =
        devel_requires_check_policy;
    for(std::size_t index = 0; index < update_plan.entries.size(); ++index) {
        AurUpdateExecutionTarget target;
        target.update_plan_index = index;
        target.update = update_plan.entries[index];

        switch(project_aur_update_effective_state(target.update)) {
            case AurUpdateEffectiveState::UpdateAvailable:
                target.status = AurUpdateExecutionTargetStatus::Executable;
                target.desired_install_reason = DesiredInstallReason::Explicit;
                break;
            case AurUpdateEffectiveState::UpToDate:
                target.status = AurUpdateExecutionTargetStatus::Skipped;
                target.skip_kind = AurUpdateExecutionSkipKind::UpToDate;
                target.issues.push_back(make_preflight_issue(
                    AurUpdateExecutionReason::UpToDate,
                    target.update.installed_name,
                    "installed package is already up to date"));
                break;
            case AurUpdateEffectiveState::RequiresCheck:
                target.status = AurUpdateExecutionTargetStatus::Incomplete;
                target.issues.push_back(make_preflight_issue(
                    AurUpdateExecutionReason::DevelRequiresCheck,
                    target.update.installed_name,
                    "fixture suffix candidate only; authoritative build provenance is unavailable",
                    DevelRequiresCheckReason::SuffixCandidateOnly));
                break;
            case AurUpdateEffectiveState::NonAurForeign:
                target.status = AurUpdateExecutionTargetStatus::Skipped;
                target.skip_kind =
                    AurUpdateExecutionSkipKind::NonAurForeign;
                target.issues.push_back(make_preflight_issue(
                    AurUpdateExecutionReason::NonAurForeign,
                    target.update.installed_name,
                    "foreign package does not exist in AUR"));
                break;
            case AurUpdateEffectiveState::MetadataUnavailable:
                target.status = AurUpdateExecutionTargetStatus::Incomplete;
                target.issues.push_back(make_preflight_issue(
                    AurUpdateExecutionReason::AurMetadataUnavailable,
                    target.update.installed_name,
                    "fixture AUR metadata request failed"));
                break;
            case AurUpdateEffectiveState::VersionComparisonUnavailable:
                target.status = AurUpdateExecutionTargetStatus::Incomplete;
                target.issues.push_back(make_preflight_issue(
                    AurUpdateExecutionReason::VersionComparisonUnavailable,
                    target.update.installed_name,
                    "fixture version comparator failed"));
                break;
            case AurUpdateEffectiveState::Unknown:
            case AurUpdateEffectiveState::Unsupported:
                target.status = AurUpdateExecutionTargetStatus::Incomplete;
                target.issues.push_back(make_preflight_issue(
                    project_aur_update_effective_state(target.update) == AurUpdateEffectiveState::Unknown ? AurUpdateExecutionReason::DevelObservationUnknown : AurUpdateExecutionReason::DevelUnsupported,
                    target.update.installed_name, "fixture devel observation is not executable"));
                break;
            case AurUpdateEffectiveState::Inconsistent:
                target.status = AurUpdateExecutionTargetStatus::Incomplete;
                target.issues.push_back(make_preflight_issue(
                    AurUpdateExecutionReason::UpdatePlanInconsistent,
                    target.update.installed_name,
                    "fixture update state is inconsistent"));
                break;
        }

        if(scenario() == "unsupported-blocker") {
            target.status = AurUpdateExecutionTargetStatus::Unsupported;
            target.issues.push_back(make_preflight_issue(
                AurUpdateExecutionReason::SplitPackageSelectionRequired,
                target.update.installed_name,
                "fixture split package needs selection"));
        } else if(scenario() == "incomplete-blocker") {
            target.status = AurUpdateExecutionTargetStatus::Incomplete;
            target.issues.push_back(make_preflight_issue(
                AurUpdateExecutionReason::UnresolvedDependency,
                target.update.installed_name,
                "fixture dependency could not be resolved"));
        }

        preflight.targets.push_back(std::move(target));
    }
    return preflight;
}

bool has_executable_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return target.status == AurUpdateExecutionTargetStatus::Executable;
        });
}

bool has_blocking_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return is_blocking_status(target.status);
        });
}

bool can_execute(const AurUpdateExecutionPreflight& preflight) noexcept {
    return has_executable_targets(preflight) && !has_blocking_targets(preflight);
}

PreparedAurUpdateSourceBuildInvocation::PreparedAurUpdateSourceBuildInvocation(
    PreparedProductionSourceBuildInvocation&& production_invocation,
    std::vector<AurUpdatePreparedWorkItemAttribution>&&
        work_item_attributions) noexcept
    : production_invocation_(std::move(production_invocation)),
      work_item_attributions_(std::move(work_item_attributions)) {
}

PreparedAurUpdateSourceBuildInvocation::PreparedAurUpdateSourceBuildInvocation(
    PreparedAurUpdateSourceBuildInvocation&& other) noexcept
    : production_invocation_(std::move(other.production_invocation_)),
      work_item_attributions_(std::move(other.work_item_attributions_)),
      valid_(other.valid_) {
    other.valid_ = false;
}

bool AurUpdateSourceBuildPreparation::is_prepared() const noexcept {
    return devel_requires_check_policy ==
               std::optional<DevelRequiresCheckPolicy>{
                   DevelRequiresCheckPolicy::BlockOperation} &&
           issues.empty() && invocation.has_value() && invocation->is_valid();
}

bool AurUpdateSourceBuildPreparation::is_noop() const noexcept {
    return devel_requires_check_policy ==
               std::optional<DevelRequiresCheckPolicy>{
                   DevelRequiresCheckPolicy::BlockOperation} &&
           issues.empty() && !invocation.has_value() &&
           std::none_of(
               affected_update_targets.begin(),
               affected_update_targets.end(),
               [](const AurUpdateExecutionTarget& target) {
                   return target.status ==
                          AurUpdateExecutionTargetStatus::Executable;
               });
}

bool AurUpdateSourceBuildPreparation::is_blocked() const noexcept {
    return !is_prepared() && !is_noop();
}

AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config) {
    if(devel_requires_check_policy !=
           DevelRequiresCheckPolicy::BlockOperation ||
       preflight.devel_requires_check_policy !=
           std::optional<DevelRequiresCheckPolicy>{
               DevelRequiresCheckPolicy::BlockOperation}) {
        throw std::logic_error(
            "upgrade-aur command lost its BlockOperation RequiresCheck snapshot.");
    }
    if(saved_source_preference_policy !=
       SavedSourcePreferencePolicy::Strict) {
        throw std::logic_error(
            "upgrade-aur command did not request strict saved source preferences.");
    }
    append_event(
        "prepare needed=" + std::string(bool_text(needed)) + " " +
        config_snapshot(config));

    AurUpdateSourceBuildPreparation preparation;
    preparation.devel_requires_check_policy =
        devel_requires_check_policy;
    preparation.affected_update_targets = preflight.targets;

    const bool should_create_invocation =
        has_executable_targets(preflight) && !has_blocking_targets(preflight);
    if(should_create_invocation) {
        PreparedProductionSourceBuildInvocation production_invocation;
        std::vector<AurUpdatePreparedWorkItemAttribution> attributions;
        PreparedAurUpdateSourceBuildInvocation invocation(
            std::move(production_invocation), std::move(attributions));
        preparation.invocation.emplace(std::move(invocation));
    }

    if(scenario() == "preparation-failure") {
        // LANDMINE(#267): invocationの存在だけではrunnerへ進めないことを検証する。
        preparation.issues.push_back(make_preparation_issue(
            AurUpdatePreparationReason::SourcePreferenceUnavailable,
            0,
            "fixture source preference read failed"));
    }
    if(scenario() == "preparation-warning") {
        AurUpdatePreparationWarning warning;
        warning.preference_name = "warning-pkg";
        warning.entry_path =
            "/fixture/config/moguet/source-build.d/warning-pkg";
        warning.affected_update_plan_indices = {0};
        warning.diagnostic = "fixture source preference warning";
        preparation.warnings.push_back(std::move(warning));
    }
    return preparation;
}

bool AurUpdateSourceBuildExecutionResult::is_success() const noexcept {
    return status == AurUpdateInvocationExecutionStatus::Completed &&
           selected_repository_provider_transaction.is_success();
}

PackageStateChange
AurUpdateSourceBuildExecutionResult::package_state_change() const noexcept {
    if(selected_repository_provider_transaction.package_state_change ==
           PackageStateChange::Changed ||
       changed_package_state()) {
        return PackageStateChange::Changed;
    }
    return selected_repository_provider_transaction.package_state_change ==
                   PackageStateChange::Unknown
               ? PackageStateChange::Unknown
               : PackageStateChange::NoChange;
}

bool AurUpdateSourceBuildExecutionResult::changed_package_state() const noexcept {
    return std::any_of(
        work_item_results.begin(), work_item_results.end(),
        [](const AurUpdateWorkItemExecutionResult& work_item) {
            return work_item.status == AurUpdateWorkItemExecutionStatus::Updated ||
                   work_item.status ==
                       AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed;
        });
}

bool AurUpdateSourceBuildExecutionResult::has_not_attempted_items() const noexcept {
    return std::any_of(
        work_item_results.begin(), work_item_results.end(),
        [](const AurUpdateWorkItemExecutionResult& work_item) {
            return work_item.status ==
                   AurUpdateWorkItemExecutionStatus::NotAttempted;
        });
}

bool AurUpdateSourceBuildExecutionResult::has_cleanup_failure() const noexcept {
    return status ==
               AurUpdateInvocationExecutionStatus::
                   StoppedAfterPackageCleanupFailure ||
           std::any_of(
               work_item_results.begin(), work_item_results.end(),
               [](const AurUpdateWorkItemExecutionResult& work_item) {
                   return work_item.status ==
                              AurUpdateWorkItemExecutionStatus::
                                  UpdatedCleanupFailed ||
                          work_item.status ==
                              AurUpdateWorkItemExecutionStatus::
                                  NoChangeCleanupFailed;
               });
}

std::optional<std::size_t>
AurUpdateSourceBuildExecutionResult::stopped_work_item_index() const noexcept {
    for(const auto& work_item : work_item_results) {
        if(work_item.status == AurUpdateWorkItemExecutionStatus::Failed ||
           work_item.status ==
               AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed ||
           work_item.status ==
               AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed) {
            return work_item.work_item_index;
        }
    }
    return std::nullopt;
}

AurUpdateSourceBuildExecutionResult
execute_prepared_aur_update_source_build_invocation(
    PreparedAurUpdateSourceBuildInvocation invocation,
    const AppConfig& config) {
    if(!invocation.is_valid()) {
        throw std::logic_error(
            "AUR update command passed an invalid prepared invocation.");
    }
    append_event("execute " + config_snapshot(config));
    if(scenario() == "provider-transaction-failure") {
        append_event("external sudo pacman -S provider fixture");
        AurUpdateSourceBuildExecutionResult execution;
        execution.status = AurUpdateInvocationExecutionStatus::
            StoppedOnProviderTransactionFailure;
        execution.selected_repository_provider_transaction.status =
            SelectedRepositoryProviderTransactionStatus::Failed;
        execution.selected_repository_provider_transaction.selected_providers = {
            ProvidedDependency::from_repository(
                "extra", "selected-provider")};
        execution.selected_repository_provider_transaction.package_state_change =
            PackageStateChange::Unknown;
        execution.selected_repository_provider_transaction.command_exit_status =
            42;
        execution.selected_repository_provider_transaction.diagnostic =
            "fixture repository provider transaction failure";
        return execution;
    }
    append_event("external git clone fixture");
    if(scenario() == "reviewed-published-uncertain") {
        AurUpdateSourceBuildExecutionResult execution;
        execution.status =
            AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure;
        return execution;
    }
    append_event("external makepkg -sc fixture");
    if(scenario() == "reviewed-build-failure") {
        AurUpdateSourceBuildExecutionResult execution;
        execution.status =
            AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure;
        return execution;
    }
    append_event("external sudo pacman -U fixture");

    AurUpdateSourceBuildExecutionResult execution;
    if(scenario() == "ordinary-execution-failure" ||
       scenario() == "reviewed-published-uncertain" ||
       scenario() == "reviewed-build-failure" ||
       scenario() == "partial-completion" ||
       scenario() == "transaction-failure" ||
       scenario() == "transaction-process-exception") {
        execution.status =
            AurUpdateInvocationExecutionStatus::StoppedOnWorkItemFailure;
    } else if(scenario() == "updated-cleanup-failure" ||
              scenario() == "no-change-cleanup-failure" ||
              scenario() == "cleanup-mixed") {
        execution.status = AurUpdateInvocationExecutionStatus::
            StoppedAfterPackageCleanupFailure;
    }
    return execution;
}

AurUpdateOperationResult reduce_aur_update_operation_result(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateSourceBuildPreparation& preparation,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    const std::optional<AurUpdateSourceBuildExecutionResult>& execution) {
    if(devel_requires_check_policy !=
           DevelRequiresCheckPolicy::BlockOperation ||
       preflight.devel_requires_check_policy !=
           std::optional<DevelRequiresCheckPolicy>{
               DevelRequiresCheckPolicy::BlockOperation} ||
       preparation.devel_requires_check_policy !=
           std::optional<DevelRequiresCheckPolicy>{
               DevelRequiresCheckPolicy::BlockOperation}) {
        throw std::logic_error(
            "AUR update command reducer lost its BlockOperation policy snapshot.");
    }
    append_event(
        "reduce execution=" +
        std::string(execution.has_value() ? "yes" : "no"));

    AurUpdateOperationResult result;
    result.devel_requires_check_policy =
        devel_requires_check_policy;
    result.preparation_issues = preparation.issues;
    result.preparation_warnings = preparation.warnings;
    if(execution.has_value()) {
        result.execution_status = execution->status;
        result.selected_repository_provider_transaction =
            execution->selected_repository_provider_transaction;
    }

    const std::string& test_scenario = scenario();
    if(test_scenario == "no-installed-foreign") {
        result.status = AurUpdateOperationStatus::NoUpdates;
        return result;
    }

    for(const auto& target : preflight.targets) {
        AurUpdateOperationTargetStatus status =
            AurUpdateOperationTargetStatus::Updated;
        if(target.status == AurUpdateExecutionTargetStatus::Skipped) {
            status = AurUpdateOperationTargetStatus::Skipped;
        } else if(target.status == AurUpdateExecutionTargetStatus::Unsupported) {
            status = AurUpdateOperationTargetStatus::Unsupported;
        } else if(target.status == AurUpdateExecutionTargetStatus::Incomplete) {
            status = AurUpdateOperationTargetStatus::Incomplete;
        }
        result.targets.push_back(make_target_result(target, status));
    }

    if(test_scenario == "all-up-to-date" ||
       test_scenario == "non-aur-foreign") {
        result.status = AurUpdateOperationStatus::NoUpdates;
        return result;
    }
    if(test_scenario == "metadata-unavailable" ||
       test_scenario == "version-comparison-unavailable" ||
       test_scenario == "devel-requires-check" ||
       test_scenario == "unsupported-blocker" ||
       test_scenario == "incomplete-blocker") {
        result.status = AurUpdateOperationStatus::BlockedBeforeExecution;
        return result;
    }
    if(test_scenario == "preparation-failure") {
        result.status = AurUpdateOperationStatus::BlockedBeforeExecution;
        result.targets.front().status = AurUpdateOperationTargetStatus::Incomplete;
        result.targets.front().preparation_issues = preparation.issues;
        return result;
    }
    if(test_scenario == "all-updated" ||
       test_scenario == "options-propagation" ||
       test_scenario == "preparation-warning" ||
       test_scenario == "query-recoverable-failure") {
        result.status = AurUpdateOperationStatus::Completed;
        const std::string package_name =
            result.targets.front().update.installed_name;
        AurUpdateWorkItemExecutionResult work_item = make_work_item_result(
            0,
            0,
            package_name,
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None);
        if(test_scenario == "all-updated") {
            work_item.production_outcome = production_outcome(
                reviewed_source_provenance(
                    ProductionReviewedSourceOutcome::InitialFullReview, 1,
                    ReviewedSourcePublicationStatus::Published,
                    ReviewedSourceEditorOverlayStatus::InvocationLocal));
        }
        result.targets.front().execution_contributions.push_back(
            make_child_contribution(
                work_item.child_results.front(),
                AurUpdateWorkItemExecutionStatus::Updated,
                AurUpdateWorkItemFailureKind::None));
        result.execution_work_items.push_back(std::move(work_item));
        return result;
    }
    if(test_scenario == "all-no-change") {
        result.status = AurUpdateOperationStatus::Completed;
        result.targets.front().status = AurUpdateOperationTargetStatus::NoChange;
        AurUpdateWorkItemExecutionResult work_item = make_work_item_result(
            0,
            0,
            "no-change-pkg",
            AurUpdateWorkItemExecutionStatus::NoChange,
            AurUpdateWorkItemFailureKind::None);
        work_item.production_outcome = production_outcome(
            reviewed_source_provenance(
                ProductionReviewedSourceOutcome::AlreadyReviewed, 4,
                ReviewedSourcePublicationStatus::
                    AlreadyPublishedSameTarget));
        result.targets.front().execution_contributions.push_back(
            make_child_contribution(
                work_item.child_results.front(),
                AurUpdateWorkItemExecutionStatus::NoChange,
                AurUpdateWorkItemFailureKind::None));
        result.execution_work_items.push_back(std::move(work_item));
        return result;
    }
    if(test_scenario == "updated-no-change-mixed") {
        result.status = AurUpdateOperationStatus::Completed;
        result.targets[0].status = AurUpdateOperationTargetStatus::Updated;
        result.targets[1].status = AurUpdateOperationTargetStatus::NoChange;
        result.execution_work_items.push_back(make_work_item_result(
            0,
            0,
            "zeta-pkg",
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None));
        result.execution_work_items.push_back(make_work_item_result(
            1,
            1,
            "alpha-pkg",
            AurUpdateWorkItemExecutionStatus::NoChange,
            AurUpdateWorkItemFailureKind::None));
        result.execution_work_items[0].production_outcome =
            production_outcome(reviewed_source_provenance(
                ProductionReviewedSourceOutcome::UpdateReview, 9));
        result.execution_work_items[1].production_outcome =
            production_outcome(compatibility_source_provenance(
                ReviewedSourceCompatibilityBuildReason::NoDiff));
        return result;
    }
    if(test_scenario == "split-child-success") {
        result.status = AurUpdateOperationStatus::Completed;
        AurUpdateWorkItemExecutionResult work_item = make_multi_child_work_item(
            0,
            "split-suite",
            {make_child_result(
                0,
                0,
                "split-suite",
                "split-cli",
                DesiredInstallReason::Explicit,
                AurUpdateChildExecutionStatus::Installed,
                0,
                "2.4.1-3")},
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None);
        result.targets.front().execution_contributions.push_back(
            make_child_contribution(
                work_item.child_results.front(),
                AurUpdateWorkItemExecutionStatus::Updated,
                AurUpdateWorkItemFailureKind::None));
        result.execution_work_items.push_back(std::move(work_item));
        return result;
    }
    if(test_scenario == "multiple-child-mixed") {
        result.status = AurUpdateOperationStatus::Completed;
        AurUpdateWorkItemExecutionResult work_item = make_multi_child_work_item(
            0,
            "split-suite",
            {make_child_result(
                 0,
                 0,
                 "split-suite",
                 "split-main",
                 DesiredInstallReason::Explicit,
                 AurUpdateChildExecutionStatus::Installed,
                 0,
                 "3.7.0-2"),
             make_child_result(
                 0,
                 1,
                 "split-suite",
                 "split-dependency",
                 DesiredInstallReason::Dependency,
                 AurUpdateChildExecutionStatus::SkippedAsNeeded,
                 0,
                 "3.7.0-2")},
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None);
        work_item.unselected_artifacts = {
            {"split-sibling", "3.7.0-2"},
            {"split-suite-debug", "3.7.0-2"}};
        result.targets.front().execution_contributions.push_back(
            make_child_contribution(
                work_item.child_results[0],
                AurUpdateWorkItemExecutionStatus::Updated,
                AurUpdateWorkItemFailureKind::None));
        result.targets.front().execution_contributions.push_back(
            make_child_contribution(
                work_item.child_results[1],
                AurUpdateWorkItemExecutionStatus::NoChange,
                AurUpdateWorkItemFailureKind::None));
        result.execution_work_items.push_back(std::move(work_item));
        return result;
    }
    if(test_scenario == "transaction-failure" ||
       test_scenario == "transaction-process-exception") {
        result.status = AurUpdateOperationStatus::StoppedOnWorkItemFailure;
        result.targets[0].status = AurUpdateOperationTargetStatus::Failed;
        result.targets[1].status = AurUpdateOperationTargetStatus::NotAttempted;

        AurUpdateWorkItemExecutionResult failed = make_multi_child_work_item(
            0,
            "tx-suite",
            {make_child_result(
                 0,
                 0,
                 "tx-suite",
                 "tx-main",
                 DesiredInstallReason::Explicit,
                 AurUpdateChildExecutionStatus::NotAttempted,
                 0),
             make_child_result(
                 0,
                 1,
                 "tx-suite",
                 "tx-dependency",
                 DesiredInstallReason::Dependency,
                 AurUpdateChildExecutionStatus::NotAttempted,
                 0)},
            AurUpdateWorkItemExecutionStatus::Failed,
            AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
            "/private/workspace/aur-cli-secret/transaction failed");
        AurUpdatePackageTransactionFailureSnapshot transaction;
        transaction.category = test_scenario == "transaction-failure"
                                   ? AurUpdatePackageTransactionFailureCategory::CommandFailed
                                   : AurUpdatePackageTransactionFailureCategory::
                                         CommandExecutionFailed;
        transaction.attempted_artifacts = {
            {{"tx-main", "4.0.0-1"}, DesiredInstallReason::Explicit},
            {{"tx-dependency", "4.0.0-1"},
             DesiredInstallReason::Dependency}};
        if(test_scenario == "transaction-failure") {
            transaction.exit_code = 73;
        }
        transaction.diagnostic =
            "/private/artifacts/tx-main-4.0.0-1.pkg.tar.zst";
        failed.production_outcome = production_outcome(
            reviewed_source_provenance(
                ProductionReviewedSourceOutcome::UpdateReview, 11),
            ProductionSourceBuildCommandOutcome::Succeeded,
            ProductionSourceInstallOutcome::Failed);
        failed.transaction_failure = transaction;
        failed.failure_detail = transaction;

        result.targets[0].execution_work_item_index = 0;
        result.targets[0].execution_failure_kind =
            AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
        result.targets[0].execution_failure_detail = transaction;
        result.targets[0].execution_diagnostic = failed.diagnostic;
        for(const AurUpdateChildExecutionResult& child : failed.child_results) {
            AurUpdateOperationExecutionContribution contribution =
                make_child_contribution(
                    child,
                    AurUpdateWorkItemExecutionStatus::Failed,
                    AurUpdateWorkItemFailureKind::BuildOrInstallFailed);
            contribution.failure_detail = transaction;
            contribution.diagnostic = failed.diagnostic;
            result.targets[0].execution_contributions.push_back(
                std::move(contribution));
        }

        AurUpdateWorkItemExecutionResult later = make_work_item_result(
            1,
            1,
            "tx-later",
            AurUpdateWorkItemExecutionStatus::NotAttempted,
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped);
        result.targets[1].execution_work_item_index = 1;
        result.targets[1].execution_failure_kind =
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
        result.targets[1].execution_contributions.push_back(
            make_child_contribution(
                later.child_results.front(),
                AurUpdateWorkItemExecutionStatus::NotAttempted,
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped));
        result.execution_work_items.push_back(std::move(failed));
        result.execution_work_items.push_back(std::move(later));
        return result;
    }
    if(test_scenario == "cleanup-mixed") {
        result.status =
            AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure;
        result.targets[0].status =
            AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
        result.targets[1].status = AurUpdateOperationTargetStatus::NotAttempted;

        AurUpdateWorkItemExecutionResult cleanup = make_multi_child_work_item(
            0,
            "cleanup-suite",
            {make_child_result(
                 0,
                 0,
                 "cleanup-suite",
                 "cleanup-main",
                 DesiredInstallReason::Explicit,
                 AurUpdateChildExecutionStatus::InstalledCleanupFailed,
                 0,
                 "5.1.0-4"),
             make_child_result(
                 0,
                 1,
                 "cleanup-suite",
                 "cleanup-dependency",
                 DesiredInstallReason::Dependency,
                 AurUpdateChildExecutionStatus::
                     SkippedAsNeededCleanupFailed,
                 0,
                 "5.1.0-4")},
            AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
            AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction,
            "/private/workspace/aur-cli-secret/cleanup failed");
        cleanup.unselected_artifacts = {
            {"cleanup-suite-debug", "5.1.0-4"}};
        result.targets[0].execution_work_item_index = 0;
        result.targets[0].execution_failure_kind = AurUpdateWorkItemFailureKind::
            CleanupFailedAfterPackageTransaction;
        result.targets[0].execution_diagnostic = cleanup.diagnostic;
        result.targets[0].execution_contributions.push_back(
            make_child_contribution(
                cleanup.child_results[0],
                AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
                AurUpdateWorkItemFailureKind::
                    CleanupFailedAfterPackageTransaction));
        result.targets[0].execution_contributions.push_back(
            make_child_contribution(
                cleanup.child_results[1],
                AurUpdateWorkItemExecutionStatus::
                    NoChangeCleanupFailed,
                AurUpdateWorkItemFailureKind::
                    CleanupFailedAfterPackageTransaction));

        AurUpdateWorkItemExecutionResult later = make_work_item_result(
            1,
            1,
            "cleanup-later",
            AurUpdateWorkItemExecutionStatus::NotAttempted,
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped);
        result.targets[1].execution_work_item_index = 1;
        result.targets[1].execution_failure_kind =
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped;
        result.execution_work_items.push_back(std::move(cleanup));
        result.execution_work_items.push_back(std::move(later));
        return result;
    }
    if(test_scenario == "unknown-child-result" ||
       test_scenario == "incoherent-child-result") {
        result.status = AurUpdateOperationStatus::Completed;
        AurUpdateChildExecutionResult child = make_child_result(
            0,
            0,
            "defensive-suite",
            "defensive-split",
            DesiredInstallReason::Explicit,
            AurUpdateChildExecutionStatus::Installed,
            0,
            "6.0.0-1");
        if(test_scenario == "unknown-child-result") {
            child.status = static_cast<AurUpdateChildExecutionStatus>(-1);
        } else {
            child.selected_artifact.reset();
        }
        result.execution_work_items.push_back(make_multi_child_work_item(
            0,
            "defensive-suite",
            {std::move(child)},
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None));
        return result;
    }
    if(is_completed_failure_matrix_scenario(test_scenario)) {
        // POLICY(#267): production reducerの正常出力では成立しない組合せを
        // stub境界だけで作り、Completedだけを信頼せずfail-closedに扱う契約を固定する。
        result.status = AurUpdateOperationStatus::Completed;
        if(test_scenario == "completed-preparation-issue") {
            result.preparation_issues.push_back(make_preparation_issue(
                AurUpdatePreparationReason::SourcePreferenceUnavailable,
                0,
                "fixture completed preparation issue"));
        } else if(test_scenario == "completed-reduction-issue") {
            AurUpdateOperationReductionIssue issue;
            issue.reason = AurUpdateOperationReductionReason::
                UnknownExecutionUpdatePlanIndex;
            issue.stage = AurUpdateOperationReductionStage::Execution;
            issue.affected_update_plan_indices = {99};
            issue.diagnostic = "fixture completed reduction issue";
            result.reduction_issues.push_back(std::move(issue));
        } else if(test_scenario == "completed-unsupported-target") {
            result.targets.front().status =
                AurUpdateOperationTargetStatus::Unsupported;
            result.targets.front().preflight_issues.push_back(
                make_preflight_issue(
                    AurUpdateExecutionReason::
                        SplitPackageSelectionRequired,
                    "defensive-pkg",
                    "fixture completed unsupported target"));
        } else if(test_scenario == "completed-incomplete-target") {
            result.targets.front().status =
                AurUpdateOperationTargetStatus::Incomplete;
            result.targets.front().preflight_issues.push_back(
                make_preflight_issue(
                    AurUpdateExecutionReason::UnresolvedDependency,
                    "defensive-pkg",
                    "fixture completed incomplete target"));
        } else if(test_scenario == "completed-execution-failure") {
            result.execution_work_items.push_back(make_work_item_result(
                0,
                0,
                "defensive-pkg",
                AurUpdateWorkItemExecutionStatus::Failed,
                AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
                "fixture completed execution failure"));
        } else if(test_scenario ==
                  "completed-invocation-execution-failure") {
            result.execution_status = AurUpdateInvocationExecutionStatus::
                StoppedOnWorkItemFailure;
        } else if(test_scenario ==
                  "completed-invocation-cleanup-failure") {
            result.execution_status = AurUpdateInvocationExecutionStatus::
                StoppedAfterPackageCleanupFailure;
        } else if(test_scenario ==
                  "completed-missing-invocation-status") {
            result.execution_status.reset();
        } else if(test_scenario == "completed-cleanup-failure") {
            result.targets.front().status =
                AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
            set_execution_failure(
                result.targets.front(),
                0,
                AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
                AurUpdateWorkItemFailureKind::
                    CleanupFailedAfterPackageTransaction,
                "fixture completed cleanup failure");
        } else if(test_scenario == "completed-not-attempted") {
            result.targets.front().status =
                AurUpdateOperationTargetStatus::NotAttempted;
            set_execution_failure(
                result.targets.front(),
                0,
                AurUpdateWorkItemExecutionStatus::NotAttempted,
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped,
                std::nullopt);
            result.execution_work_items.push_back(make_work_item_result(
                0,
                0,
                "defensive-pkg",
                AurUpdateWorkItemExecutionStatus::NotAttempted,
                AurUpdateWorkItemFailureKind::PriorWorkItemStopped));
        }
        return result;
    }
    if(test_scenario == "ordinary-execution-failure") {
        result.status = AurUpdateOperationStatus::StoppedOnWorkItemFailure;
        result.targets.front().status = AurUpdateOperationTargetStatus::Failed;
        set_execution_failure(
            result.targets.front(),
            0,
            AurUpdateWorkItemExecutionStatus::Failed,
            AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
            "fixture build or install failed");
        return result;
    }
    if(test_scenario == "reviewed-published-uncertain" ||
       test_scenario == "reviewed-build-failure") {
        result.status = AurUpdateOperationStatus::StoppedOnWorkItemFailure;
        result.targets.front().status = AurUpdateOperationTargetStatus::Failed;
        const std::string package_name =
            result.targets.front().update.installed_name;
        AurUpdateWorkItemExecutionResult work_item = make_work_item_result(
            0,
            0,
            package_name,
            AurUpdateWorkItemExecutionStatus::Failed,
            AurUpdateWorkItemFailureKind::BuildOrInstallFailed);
        AurUpdateSourceBuildFailureSnapshot failure;
        failure.category = AurUpdateSourceBuildFailureCategory::Build;
        if(test_scenario == "reviewed-published-uncertain") {
            failure.diagnostic =
                "fixture post-commit publication ambiguity";
            failure.reviewed_source_failure =
                ReviewedSourceProductionFailure{
                    ReviewedSourceProductionFailureStage::
                        StatePublication,
                    ReviewedSourceProductionFailureReason::
                        PublishedUncertain,
                    std::monostate{}};
        } else {
            failure.diagnostic =
                "fixture makepkg failed after reviewed publication";
            work_item.production_outcome = production_outcome(
                reviewed_source_provenance(
                    ProductionReviewedSourceOutcome::UpdateReview,
                    12),
                ProductionSourceBuildCommandOutcome::Failed,
                ProductionSourceInstallOutcome::NotAttempted);
        }
        work_item.failure_detail = failure;
        work_item.diagnostic = failure.diagnostic;
        result.execution_work_items.push_back(std::move(work_item));
        result.targets.front().execution_work_item_index = 0;
        result.targets.front().execution_failure_kind =
            AurUpdateWorkItemFailureKind::BuildOrInstallFailed;
        result.targets.front().execution_failure_detail = failure;
        result.targets.front().execution_diagnostic = failure.diagnostic;
        result.targets.front().execution_contributions.push_back(
            make_contribution(
                0,
                package_name,
                AurUpdateWorkItemExecutionStatus::Failed,
                AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
                failure.diagnostic));
        result.targets.front().execution_contributions.back().failure_detail =
            failure;
        return result;
    }
    if(test_scenario == "provider-transaction-failure") {
        result.status = AurUpdateOperationStatus::
            StoppedOnProviderTransactionFailure;
        result.targets.front().status =
            AurUpdateOperationTargetStatus::NotAttempted;
        return result;
    }
    if(test_scenario == "updated-cleanup-failure") {
        result.status =
            AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure;
        result.targets.front().status =
            AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
        set_execution_failure(
            result.targets.front(),
            0,
            AurUpdateWorkItemExecutionStatus::UpdatedCleanupFailed,
            AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction,
            "fixture cleanup failed after update");
        return result;
    }
    if(test_scenario == "no-change-cleanup-failure") {
        result.status =
            AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure;
        result.targets.front().status =
            AurUpdateOperationTargetStatus::NoChangeCleanupFailed;
        set_execution_failure(
            result.targets.front(),
            0,
            AurUpdateWorkItemExecutionStatus::NoChangeCleanupFailed,
            AurUpdateWorkItemFailureKind::
                CleanupFailedAfterPackageTransaction,
            "fixture cleanup failed without package change");
        return result;
    }
    if(test_scenario == "partial-completion") {
        result.status = AurUpdateOperationStatus::StoppedOnWorkItemFailure;
        result.targets[0].status = AurUpdateOperationTargetStatus::Updated;
        result.targets[0].execution_contributions.push_back(make_contribution(
            0,
            "first-pkg",
            AurUpdateWorkItemExecutionStatus::Updated,
            AurUpdateWorkItemFailureKind::None));
        result.targets[1].status = AurUpdateOperationTargetStatus::Failed;
        set_execution_failure(
            result.targets[1],
            1,
            AurUpdateWorkItemExecutionStatus::Failed,
            AurUpdateWorkItemFailureKind::BuildOrInstallFailed,
            "fixture second work item failed");
        result.targets[2].status = AurUpdateOperationTargetStatus::NotAttempted;
        set_execution_failure(
            result.targets[2],
            2,
            AurUpdateWorkItemExecutionStatus::NotAttempted,
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped,
            std::nullopt);
        result.execution_work_items.push_back(make_work_item_result(
            2,
            2,
            "later-pkg",
            AurUpdateWorkItemExecutionStatus::NotAttempted,
            AurUpdateWorkItemFailureKind::PriorWorkItemStopped));
        return result;
    }
    if(test_scenario == "reducer-inconsistency") {
        result.status = AurUpdateOperationStatus::InconsistentResult;
        result.targets.front().status = AurUpdateOperationTargetStatus::Incomplete;
        AurUpdateOperationReductionIssue issue;
        issue.reason =
            AurUpdateOperationReductionReason::UnknownExecutionUpdatePlanIndex;
        issue.stage = AurUpdateOperationReductionStage::Execution;
        issue.affected_update_plan_indices = {99};
        issue.diagnostic = "fixture reducer mismatch";
        result.reduction_issues.push_back(std::move(issue));
        return result;
    }

    result.status = AurUpdateOperationStatus::Completed;
    return result;
}

bool AurUpdateOperationResult::is_success() const noexcept {
    return devel_requires_check_policy ==
               std::optional<DevelRequiresCheckPolicy>{
                   DevelRequiresCheckPolicy::BlockOperation} &&
           (status == AurUpdateOperationStatus::NoUpdates ||
            status == AurUpdateOperationStatus::Completed) &&
           selected_repository_provider_transaction.is_success();
}

PackageStateChange AurUpdateOperationResult::package_state_change()
    const noexcept {
    if(selected_repository_provider_transaction.package_state_change ==
           PackageStateChange::Changed ||
       changed_package_state()) {
        return PackageStateChange::Changed;
    }
    return selected_repository_provider_transaction.package_state_change ==
                   PackageStateChange::Unknown
               ? PackageStateChange::Unknown
               : PackageStateChange::NoChange;
}

bool AurUpdateOperationResult::changed_package_state() const noexcept {
    return std::any_of(
        targets.begin(), targets.end(),
        [](const AurUpdateOperationTargetResult& target) {
            return target.status == AurUpdateOperationTargetStatus::Updated ||
                   target.status ==
                       AurUpdateOperationTargetStatus::UpdatedCleanupFailed;
        });
}

bool AurUpdateOperationResult::has_partial_completion() const noexcept {
    return !is_success() &&
           (changed_package_state() ||
            selected_repository_provider_transaction.status ==
                SelectedRepositoryProviderTransactionStatus::Succeeded);
}

bool AurUpdateOperationResult::has_not_attempted_targets() const noexcept {
    return std::any_of(
        targets.begin(), targets.end(),
        [](const AurUpdateOperationTargetResult& target) {
            return target.status ==
                   AurUpdateOperationTargetStatus::NotAttempted;
        });
}

bool AurUpdateOperationResult::has_cleanup_failure() const noexcept {
    return status ==
               AurUpdateOperationStatus::StoppedAfterPackageCleanupFailure ||
           std::any_of(
               targets.begin(), targets.end(),
               [](const AurUpdateOperationTargetResult& target) {
                   return target.status ==
                              AurUpdateOperationTargetStatus::
                                  UpdatedCleanupFailed ||
                          target.status ==
                              AurUpdateOperationTargetStatus::
                                  NoChangeCleanupFailed;
               });
}

bool AurUpdateOperationResult::has_blocking_targets() const noexcept {
    return std::any_of(
        targets.begin(), targets.end(),
        [](const AurUpdateOperationTargetResult& target) {
            return target.status == AurUpdateOperationTargetStatus::Unsupported ||
                   target.status ==
                       AurUpdateOperationTargetStatus::Incomplete;
        });
}

PreparedFilteredAurUpdateOperation::PreparedFilteredAurUpdateOperation(
    PreparedFilteredAurUpdateOperation&& other) noexcept
    : valid_(other.valid_),
      devel_requires_check_policy(
          std::move(other.devel_requires_check_policy)),
      query_result(std::move(other.query_result)),
      target_adapter(std::move(other.target_adapter)),
      upgrade_all_plan(std::move(other.upgrade_all_plan)),
      filtered_update_plan(std::move(other.filtered_update_plan)),
      filtered_to_original_query_plan_index(
          std::move(other.filtered_to_original_query_plan_index)),
      original_query_plan_to_filtered_index(
          std::move(other.original_query_plan_to_filtered_index)),
      target_correlations(std::move(other.target_correlations)),
      preflight(std::move(other.preflight)),
      build_unit_correlations(std::move(other.build_unit_correlations)),
      preparation(std::move(other.preparation)),
      issues(std::move(other.issues)) {
    other.valid_ = false;
}

bool PreparedFilteredAurUpdateOperation::is_valid() const noexcept {
    return valid_;
}

bool PreparedFilteredAurUpdateOperation::is_prepared() const noexcept {
    return valid_ && devel_requires_check_policy == std::optional<DevelRequiresCheckPolicy>{DevelRequiresCheckPolicy::BlockOperation} &&
           issues.empty() &&
           !has_upgrade_all_planning_issues(upgrade_all_plan) &&
           preparation.has_value() && preparation->is_prepared();
}

bool PreparedFilteredAurUpdateOperation::is_noop() const noexcept {
    return valid_ && devel_requires_check_policy == std::optional<DevelRequiresCheckPolicy>{DevelRequiresCheckPolicy::BlockOperation} &&
           issues.empty() &&
           !has_upgrade_all_planning_issues(upgrade_all_plan) &&
           preparation.has_value() && preparation->is_noop();
}

bool PreparedFilteredAurUpdateOperation::is_blocked() const noexcept {
    return valid_ && !is_prepared() && !is_noop();
}

bool FilteredAurUpdateObservation::is_ready() const noexcept {
    return false;
}

bool FilteredAurUpdateObservation::is_noop() const noexcept {
    return false;
}

bool FilteredAurUpdateObservation::is_blocked() const noexcept {
    return true;
}

FilteredAurUpdateObservation observe_filtered_aur_update_operation(
    AurUpdateQueryResult,
    FilteredAurUpdateExplicitSourceSatisfaction,
    DevelRequiresCheckPolicy,
    SavedSourcePreferencePolicy,
    const AppConfig&) {
    throw std::logic_error(
        "System AUR dry-run observation is outside the upgrade-aur command fixture.");
}

PreparedFilteredAurUpdateOperation prepare_filtered_aur_update_operation(
    AurUpdateQueryResult query_result,
    FilteredAurUpdateExplicitSourceSatisfaction
        explicit_source_satisfaction,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    const AppConfig& config,
    std::optional<ValidatedCacheRoot> cache_root) {
    if(!std::holds_alternative<NoExplicitSourceSatisfaction>(
           explicit_source_satisfaction)) {
        throw std::logic_error(
            "AUR update command stub requires no explicit source satisfaction.");
    }
    if(devel_requires_check_policy !=
       DevelRequiresCheckPolicy::BlockOperation) {
        throw std::logic_error(
            "AUR update command stub requires BlockOperation RequiresCheck policy.");
    }
    if(saved_source_preference_policy !=
       SavedSourcePreferencePolicy::Strict) {
        throw std::logic_error(
            "AUR update command stub requires strict saved source preferences.");
    }
    if(cache_root.has_value()) {
        throw std::logic_error(
            "AUR update command supplied cache authority before provider preflight.");
    }

    PreparedFilteredAurUpdateOperation prepared;
    prepared.devel_requires_check_policy =
        devel_requires_check_policy;
    prepared.query_result = std::move(query_result);
    // POLICY(#281): command regression fixtures keep the full legacy plan so
    // normal skipped targets remain visible in the unchanged presentation.
    prepared.filtered_update_plan = prepared.query_result.plan;
    prepared.preflight = resolve_aur_update_execution_preflight(
        prepared.filtered_update_plan,
        devel_requires_check_policy);

    AurUpdateSourceBuildPreparation legacy_preparation =
        prepare_aur_update_source_build_invocation(
            prepared.preflight, devel_requires_check_policy,
            saved_source_preference_policy, false, config);
    prepared.preparation.emplace(std::move(legacy_preparation));
    return prepared;
}

FilteredAurUpdateExecutionResult execute_prepared_filtered_aur_update_operation(
    PreparedFilteredAurUpdateOperation prepared,
    const AppConfig& config) {
    if(!prepared.is_valid()) {
        throw std::logic_error(
            "AUR update command passed an invalid filtered operation.");
    }

    std::optional<AurUpdateSourceBuildExecutionResult> execution;
    if(!prepared.preparation.has_value()) {
        throw std::logic_error(
            "AUR update command filtered operation lost its preparation.");
    }
    if(prepared.preparation->is_prepared()) {
        execution.emplace(
            execute_prepared_aur_update_source_build_invocation(
                std::move(*prepared.preparation->invocation), config));
    }

    AurUpdateOperationResult reduced = reduce_aur_update_operation_result(
        prepared.preflight, *prepared.preparation,
        *prepared.devel_requires_check_policy, execution);
    prepared.valid_ = false;
    return FilteredAurUpdateExecutionResult{
        std::move(prepared.query_result),
        std::move(prepared.target_adapter),
        std::move(prepared.upgrade_all_plan),
        std::move(prepared.filtered_update_plan),
        std::move(prepared.filtered_to_original_query_plan_index),
        std::move(prepared.original_query_plan_to_filtered_index),
        std::move(prepared.target_correlations),
        std::move(prepared.preflight),
        std::move(prepared.build_unit_correlations),
        std::move(*prepared.preparation),
        std::move(execution),
        std::move(reduced),
        {},
        std::move(prepared.issues),
        std::move(prepared.devel_requires_check_policy)};
}

bool FilteredAurUpdateExecutionResult::is_success() const noexcept {
    return has_consistent_devel_requires_check_policy_snapshot() &&
           !has_query_failure() && !has_planning_issue() &&
           reduced_result_is_defensively_successful(
               reduced_operation_result);
}

PackageStateChange
FilteredAurUpdateExecutionResult::package_state_change() const noexcept {
    return reduced_operation_result.package_state_change();
}

bool FilteredAurUpdateExecutionResult::changed_package_state() const noexcept {
    return reduced_operation_result.changed_package_state();
}

bool FilteredAurUpdateExecutionResult::has_partial_completion() const noexcept {
    return reduced_operation_result.has_partial_completion();
}

bool FilteredAurUpdateExecutionResult::has_not_attempted_targets() const noexcept {
    return reduced_operation_result.has_not_attempted_targets();
}

bool FilteredAurUpdateExecutionResult::has_cleanup_failure() const noexcept {
    return reduced_operation_result.has_cleanup_failure();
}

bool FilteredAurUpdateExecutionResult::has_query_failure() const noexcept {
    return !query_result.recoverable_failures.empty();
}

bool FilteredAurUpdateExecutionResult::has_planning_issue() const noexcept {
    return !issues.empty() ||
           has_upgrade_all_planning_issues(upgrade_all_plan);
}

bool FilteredAurUpdateExecutionResult::has_duplicate_exclusions() const noexcept {
    return !upgrade_all_plan.excluded_duplicate_target_indexes.empty();
}

bool FilteredAurUpdateObservation::
    has_consistent_devel_requires_check_policy_snapshot() const noexcept {
    return false;
}

bool FilteredAurUpdateExecutionResult::
    has_consistent_devel_requires_check_policy_snapshot() const noexcept {
    return devel_requires_check_policy.has_value() &&
           is_known_devel_requires_check_policy(
               *devel_requires_check_policy) &&
           preflight.devel_requires_check_policy ==
               devel_requires_check_policy &&
           preparation.devel_requires_check_policy ==
               devel_requires_check_policy &&
           reduced_operation_result.devel_requires_check_policy ==
               devel_requires_check_policy;
}
