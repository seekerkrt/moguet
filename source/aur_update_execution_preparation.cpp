#include "aur_update_execution_preparation.hpp"
#include "devel_tracking_bootstrap.hpp"

#include "aur_update_required_devel_relation_projection.hpp"
#include "app_config.hpp"
#include "localization.hpp"
#include "package_identifier.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// AUR update preflightをmutation-freeなproduction invocationへ射影する。
// POLICY(#267): source preference、static work item、Pacman DB snapshotまでを所有し、
// checkout/build/installやCLI表示へ接続しない。
namespace {

const std::string AUR_BASE_URL = "https://aur.archlinux.org/";
constexpr std::string_view AUR_SERVICE_NAME = "AUR";
constexpr std::string_view BUILD_PLAN_TYPE_NAME = "BuildPlan";
constexpr std::string_view PKGDEST_ENVIRONMENT_KEY = "PKGDEST";
constexpr std::string_view ROOT_ROLE_NAME = "Root";
constexpr std::string_view DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC =
    "Devel RequiresCheck policy or typed skip snapshot is inconsistent.";

struct ExecutableRootBinding {
    const AurUpdateExecutionTarget* target = nullptr;
    std::size_t root_index = 0;
    RootTargetIdentity root;
};

struct UpdateWorkItemDraft {
    ProductionSourceBuildWorkItem work_item;
    std::size_t build_plan_order_index = 0;
    std::vector<AurUpdateRequiredTargetAttribution>
        required_target_attributions;
    std::vector<std::size_t> affected_update_plan_indices;
    std::vector<RootTargetIdentity> affected_roots;
};

template <typename Value>
void add_unique(std::vector<Value>& values, const Value& value) {
    if(std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void add_selected_repository_provider(
    std::vector<ProvidedDependency>& providers,
    const ProvidedDependency& provider) {
    const auto same = [&provider](const ProvidedDependency& existing) {
        return same_provider_identity(existing, provider);
    };
    if(std::find_if(providers.begin(), providers.end(), same) ==
       providers.end()) {
        providers.push_back(provider);
    }
}

void attach_selected_repository_providers(
    ProductionSourceBuildWorkItem& work_item,
    const BuildPlan& plan) {
    for(const BuildPlanDependencyEdge& edge : plan.dependency_edges) {
        if(edge.parent_package_base != work_item.request.checkout_name ||
           edge.kind != DependencyKind::Provided ||
           edge.provider_resolution != ProviderResolutionKind::UserSelected ||
           !edge.resolved_provider.has_value() ||
           !std::holds_alternative<RepositoryProviderOrigin>(
               edge.resolved_provider->origin)) {
            continue;
        }
        add_selected_repository_provider(
            work_item.selected_repository_providers,
            edge.resolved_provider.value());
    }
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

bool is_blocking_status(AurUpdateExecutionTargetStatus status) noexcept {
    return status == AurUpdateExecutionTargetStatus::Unsupported ||
           status == AurUpdateExecutionTargetStatus::Incomplete;
}

bool is_known_status(AurUpdateExecutionTargetStatus status) noexcept {
    return status == AurUpdateExecutionTargetStatus::Executable ||
           status == AurUpdateExecutionTargetStatus::Skipped ||
           is_blocking_status(status);
}

bool has_known_devel_requires_check_policy(
    const std::optional<DevelRequiresCheckPolicy>& policy) noexcept {
    return policy.has_value() &&
           is_known_devel_requires_check_policy(*policy);
}

bool has_valid_update_target_snapshots(
    const std::vector<AurUpdateExecutionTarget>& targets,
    const std::optional<DevelRequiresCheckPolicy>& policy) noexcept {
    if(!has_known_devel_requires_check_policy(policy)) return false;
    return std::all_of(
        targets.begin(), targets.end(),
        [&policy](const AurUpdateExecutionTarget& target) {
            return is_valid_aur_update_execution_target_skip_snapshot(
                target, *policy);
        });
}

bool is_normal_skipped_preflight_reason(
    AurUpdateExecutionReason reason) noexcept {
    return reason == AurUpdateExecutionReason::UpToDate ||
           reason == AurUpdateExecutionReason::NonAurForeign;
}

bool has_blocking_preflight_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return is_blocking_status(target.status);
        });
}

bool has_executable_preflight_targets(
    const AurUpdateExecutionPreflight& preflight) noexcept {
    return std::any_of(
        preflight.targets.begin(), preflight.targets.end(),
        [](const AurUpdateExecutionTarget& target) {
            return target.status ==
                   AurUpdateExecutionTargetStatus::Executable;
        });
}

bool has_role(
    const PlannedPackageTarget& target, PackageRole expected_role) {
    return std::find(
               target.roles.begin(), target.roles.end(), expected_role) !=
           target.roles.end();
}

bool is_dependency_role(PackageRole role) noexcept {
    return role == PackageRole::RuntimeDependency ||
           role == PackageRole::BuildDependency ||
           role == PackageRole::CheckDependency;
}

bool is_known_role(PackageRole role) noexcept {
    return role == PackageRole::Root || is_dependency_role(role);
}

bool is_known_desired_install_reason(DesiredInstallReason reason) noexcept {
    return reason == DesiredInstallReason::Explicit ||
           reason == DesiredInstallReason::Dependency;
}

bool is_known_selection_status(
    AurUpdateBuildUnitSelectionStatus status) noexcept {
    return status ==
               AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution ||
           status == AurUpdateBuildUnitSelectionStatus::
                         ExternallySatisfiedByExplicitSourcePackageBase;
}

AurUpdatePreparationIssue make_localized_preparation_issue(
    AurUpdatePreparationReason reason,
    std::string diagnostic) {
    AurUpdatePreparationIssue issue;
    issue.reason = reason;
    issue.diagnostic = std::move(diagnostic);
    return issue;
}

template <std::size_t Size>
AurUpdatePreparationIssue make_localized_preparation_issue(
    AurUpdatePreparationReason reason,
    const char (&diagnostic)[Size]) {
    return make_localized_preparation_issue(
        reason, localization::translate_message(diagnostic));
}

void attribute_issue_to_target(
    AurUpdatePreparationIssue& issue,
    const AurUpdateExecutionTarget& target) {
    add_unique(
        issue.affected_update_plan_indices,
        target.update_plan_index);
}

void attribute_issue_to_draft(
    AurUpdatePreparationIssue& issue,
    const UpdateWorkItemDraft& draft) {
    for(const std::size_t update_plan_index :
        draft.affected_update_plan_indices) {
        add_unique(
            issue.affected_update_plan_indices,
            update_plan_index);
    }
    for(const auto& root : draft.affected_roots) {
        add_unique(issue.affected_roots, root);
    }
}

void attribute_issue_to_all_executable_targets(
    AurUpdatePreparationIssue& issue,
    const AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& target : preparation.affected_update_targets) {
        add_unique(
            issue.affected_update_plan_indices,
            target.update_plan_index);
    }
    for(const auto& root : preparation.affected_roots) {
        add_unique(issue.affected_roots, root);
    }
}

const ExecutableRootBinding* find_root_binding(
    const std::vector<ExecutableRootBinding>& bindings,
    const RootTargetIdentity& root) {
    auto found = std::find_if(
        bindings.begin(), bindings.end(),
        [&root](const ExecutableRootBinding& binding) {
            return binding.root == root;
        });
    return found == bindings.end() ? nullptr : &(*found);
}

bool collect_exact_package_target_attribution(
    const PlannedPackageTarget& package_target,
    const std::vector<ExecutableRootBinding>& bindings,
    std::vector<std::size_t>& affected_update_plan_indices,
    std::vector<RootTargetIdentity>& affected_roots) {
    if(package_target.roots.empty()) return false;

    for(const auto& root : package_target.roots) {
        if(std::find(affected_roots.begin(), affected_roots.end(), root) !=
           affected_roots.end()) {
            return false;
        }
        const ExecutableRootBinding* binding =
            find_root_binding(bindings, root);
        if(binding == nullptr) return false;

        add_unique(
            affected_update_plan_indices,
            binding->target->update_plan_index);
        add_unique(affected_roots, binding->root);
    }
    return true;
}

bool target_contains_root(
    const PlannedPackageTarget& target,
    const RootTargetIdentity& root) {
    return std::find(target.roots.begin(), target.roots.end(), root) !=
           target.roots.end();
}

void retain_preflight_blockers(
    const AurUpdateExecutionPreflight& preflight,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& target : preflight.targets) {
        if(is_known_status(target.status) &&
           !is_blocking_status(target.status)) {
            continue;
        }

        preparation.affected_update_targets.push_back(target);
        bool retained_typed_issue = false;
        for(const auto& preflight_issue : target.issues) {
            if(preflight_issue.reason == AurUpdateExecutionReason::None) continue;

            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::BlockingPreflight,
                preflight_issue.diagnostic);
            attribute_issue_to_target(issue, target);
            issue.preflight_issue = preflight_issue;
            issue.package_name = preflight_issue.package_name;
            issue.package_base = preflight_issue.package_base;
            preparation.issues.push_back(std::move(issue));
            retained_typed_issue = true;
        }

        if(retained_typed_issue && is_known_status(target.status)) continue;

        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::PreflightInconsistent,
            is_known_status(target.status)
                ? localization::format_translated_message(
                      // TRANSLATORS: {} is the literal service name "AUR".
                      "Blocking {} update preflight target has no typed issue.",
                      AUR_SERVICE_NAME)
                : localization::format_translated_message(
                      // TRANSLATORS: {} is the literal service name "AUR".
                      "{} update preflight target has an unknown status.",
                      AUR_SERVICE_NAME));
        attribute_issue_to_target(issue, target);
        issue.package_name = target.update.installed_name;
        preparation.issues.push_back(std::move(issue));
    }
}

void retain_executable_preflight_inconsistencies(
    const AurUpdateExecutionPreflight& preflight,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& target : preflight.targets) {
        if(target.status != AurUpdateExecutionTargetStatus::Executable) continue;
        if(preflight.devel_requires_check_policy.has_value() &&
           is_valid_aur_update_execution_target_skip_snapshot(
               target, *preflight.devel_requires_check_policy)) {
            continue;
        }

        bool retained_target = false;
        for(const auto& preflight_issue : target.issues) {
            if(!retained_target) {
                preparation.affected_update_targets.push_back(target);
                retained_target = true;
            }

            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::PreflightInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // service name "AUR"; the second is a diagnostic.
                    "Executable {} update target retains a blocking preflight issue: {}",
                    AUR_SERVICE_NAME,
                    preflight_issue.diagnostic));
            attribute_issue_to_target(issue, target);
            issue.preflight_issue = preflight_issue;
            issue.package_name = preflight_issue.package_name;
            issue.package_base = preflight_issue.package_base;
            preparation.issues.push_back(std::move(issue));
        }
    }
}

void retain_skipped_preflight_inconsistencies(
    const AurUpdateExecutionPreflight& preflight,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& target : preflight.targets) {
        if(target.status != AurUpdateExecutionTargetStatus::Skipped) continue;

        if(preflight.devel_requires_check_policy.has_value() &&
           is_known_devel_requires_check_policy(
               *preflight.devel_requires_check_policy) &&
           is_valid_aur_update_execution_target_skip_snapshot(
               target, *preflight.devel_requires_check_policy)) {
            continue;
        }

        if(target.skip_kind.has_value() &&
           (!is_known_aur_update_execution_skip_kind(
                *target.skip_kind) ||
            *target.skip_kind == AurUpdateExecutionSkipKind::
                                     IndependentDevelRequiresCheck ||
            *target.skip_kind == AurUpdateExecutionSkipKind::
                                     RequiredDevelRequiresCheck)) {
            preparation.affected_update_targets.push_back(target);
            AurUpdatePreparationIssue issue =
                make_localized_preparation_issue(
                    AurUpdatePreparationReason::
                        DevelRequiresCheckPolicyInconsistent,
                    std::string{
                        DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
            attribute_issue_to_target(issue, target);
            issue.package_name = target.update.installed_name;
            preparation.issues.push_back(std::move(issue));
            continue;
        }

        std::size_t normal_skip_issue_count = 0;
        bool has_matching_normal_skip_issue = false;
        bool retained_target = false;
        for(const auto& preflight_issue : target.issues) {
            if(preflight_issue.reason == AurUpdateExecutionReason::None) continue;
            if(is_normal_skipped_preflight_reason(preflight_issue.reason)) {
                ++normal_skip_issue_count;
                has_matching_normal_skip_issue =
                    has_matching_normal_skip_issue ||
                    (preflight_issue.reason ==
                         AurUpdateExecutionReason::UpToDate &&
                     target.skip_kind ==
                         std::optional<AurUpdateExecutionSkipKind>{
                             AurUpdateExecutionSkipKind::UpToDate}) ||
                    (preflight_issue.reason ==
                         AurUpdateExecutionReason::NonAurForeign &&
                     target.skip_kind ==
                         std::optional<AurUpdateExecutionSkipKind>{
                             AurUpdateExecutionSkipKind::NonAurForeign});
                continue;
            }

            if(!retained_target) {
                preparation.affected_update_targets.push_back(target);
                retained_target = true;
            }
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::PreflightInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The first placeholder is the literal
                    // service name "AUR"; the second is a diagnostic.
                    "Skipped {} update target retains a non-skip preflight issue: {}",
                    AUR_SERVICE_NAME,
                    preflight_issue.diagnostic));
            attribute_issue_to_target(issue, target);
            issue.preflight_issue = preflight_issue;
            issue.package_name = preflight_issue.package_name;
            issue.package_base = preflight_issue.package_base;
            preparation.issues.push_back(std::move(issue));
        }

        if(normal_skip_issue_count == 1 &&
           has_matching_normal_skip_issue &&
           is_valid_aur_update_execution_target_skip_snapshot(target)) {
            continue;
        }
        if(retained_target) continue;

        // POLICY(#267): reasonのないSkipped snapshotをnormal no-opとして受理しない。
        preparation.affected_update_targets.push_back(target);
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::PreflightInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "Skipped {} update target has no normal skip preflight issue.",
                AUR_SERVICE_NAME));
        attribute_issue_to_target(issue, target);
        issue.package_name = target.update.installed_name;
        preparation.issues.push_back(std::move(issue));
    }
}

void retain_non_skipped_skip_kind_inconsistencies(
    const AurUpdateExecutionPreflight& preflight,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& target : preflight.targets) {
        if(target.status == AurUpdateExecutionTargetStatus::Skipped ||
           !target.skip_kind.has_value()) {
            continue;
        }
        preparation.affected_update_targets.push_back(target);
        AurUpdatePreparationIssue issue =
            make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    DevelRequiresCheckPolicyInconsistent,
                std::string{
                    DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
        attribute_issue_to_target(issue, target);
        issue.package_name = target.update.installed_name;
        preparation.issues.push_back(std::move(issue));
    }
}

bool collect_executable_root_bindings(
    const AurUpdateExecutionPreflight& preflight,
    const BuildPlan& plan,
    AurUpdateSourceBuildPreparation& preparation,
    std::vector<ExecutableRootBinding>& bindings) {
    std::vector<const AurUpdateExecutionTarget*> executable_targets;
    for(std::size_t target_index = 0;
        target_index < preflight.targets.size(); ++target_index) {
        const AurUpdateExecutionTarget& target =
            preflight.targets[target_index];
        if(target.status != AurUpdateExecutionTargetStatus::Executable) continue;
        executable_targets.push_back(&target);
        preparation.affected_update_targets.push_back(target);
        if(target.update_plan_index != target_index) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::RootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "Executable {} update target index differs from its position in the preflight snapshot.",
                    AUR_SERVICE_NAME));
            attribute_issue_to_target(issue, target);
            issue.package_name = target.update.installed_name;
            preparation.issues.push_back(std::move(issue));
        }
    }

    if(plan.root_targets.size() != executable_targets.size()) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::RootAttributionInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                "{} root count does not match the executable update target count.",
                BUILD_PLAN_TYPE_NAME));
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
    }

    std::set<std::size_t> seen_update_plan_indices;
    std::set<std::size_t> seen_root_indices;
    for(const auto* target : executable_targets) {
        if(!seen_update_plan_indices.insert(target->update_plan_index).second) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::RootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "Executable {} update targets contain a duplicate update plan index.",
                    AUR_SERVICE_NAME));
            attribute_issue_to_target(issue, *target);
            issue.package_name = target->update.installed_name;
            preparation.issues.push_back(std::move(issue));
        }

        if(!target->build_plan_root_index.has_value()) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::RootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // service name "AUR" and internal type name
                    // "BuildPlan", respectively.
                    "Executable {} update target has no {} root index.",
                    AUR_SERVICE_NAME,
                    BUILD_PLAN_TYPE_NAME));
            attribute_issue_to_target(issue, *target);
            issue.package_name = target->update.installed_name;
            preparation.issues.push_back(std::move(issue));
            continue;
        }

        const std::size_t root_index = *target->build_plan_root_index;
        if(root_index >= plan.root_targets.size()) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::RootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // service name "AUR" and internal type name
                    // "BuildPlan", respectively.
                    "Executable {} update target refers to an out-of-range {} root index.",
                    AUR_SERVICE_NAME,
                    BUILD_PLAN_TYPE_NAME));
            attribute_issue_to_target(issue, *target);
            issue.package_name = target->update.installed_name;
            preparation.issues.push_back(std::move(issue));
            continue;
        }
        if(!seen_root_indices.insert(root_index).second) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::RootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // service name "AUR" and internal type name
                    // "BuildPlan", respectively.
                    "Multiple executable {} update targets refer to the same {} root index.",
                    AUR_SERVICE_NAME,
                    BUILD_PLAN_TYPE_NAME));
            attribute_issue_to_target(issue, *target);
            issue.package_name = target->update.installed_name;
            preparation.issues.push_back(std::move(issue));
            continue;
        }

        const RootTargetIdentity& root = plan.root_targets[root_index];
        if(root.invocation_index != root_index ||
           root.requested_name != target->update.installed_name) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::RootAttributionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the literal
                    // internal type name "BuildPlan" and service name
                    // "AUR", respectively.
                    "{} root identity does not match its executable {} update target.",
                    BUILD_PLAN_TYPE_NAME,
                    AUR_SERVICE_NAME));
            attribute_issue_to_target(issue, *target);
            issue.affected_roots.push_back(root);
            issue.package_name = target->update.installed_name;
            preparation.issues.push_back(std::move(issue));
            continue;
        }

        if(!target->desired_install_reason.has_value()) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::DesiredInstallReasonMissing,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "Executable {} update target has no desired install reason.",
                    AUR_SERVICE_NAME));
            attribute_issue_to_target(issue, *target);
            issue.affected_roots.push_back(root);
            issue.package_name = target->update.installed_name;
            preparation.issues.push_back(std::move(issue));
        }

        bindings.push_back(ExecutableRootBinding{target, root_index, root});
    }

    if(seen_root_indices.size() != plan.root_targets.size()) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::RootAttributionInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are the literal internal
                // type name "BuildPlan" and service name "AUR",
                // respectively.
                "{} contains a root that is not attributed to an executable {} update target.",
                BUILD_PLAN_TYPE_NAME,
                AUR_SERVICE_NAME));
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
    }

    // 解決できたrootは、別fieldのinconsistencyでblockしてもowned attributionに残す。
    for(const auto& root : plan.root_targets) {
        if(find_root_binding(bindings, root) != nullptr) {
            add_unique(preparation.affected_roots, root);
        }
    }
    if(!preparation.issues.empty()) return false;

    // root_targetsはcandidate順、affected_update_targetsは元update plan順の正本を保つ。
    return true;
}

bool require_root_package_attribution(
    const std::vector<ExecutableRootBinding>& bindings,
    const BuildPlan& plan,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& binding : bindings) {
        std::vector<const PlannedPackageTarget*> matching_targets;
        for(const auto& package_target : plan.package_targets) {
            if(package_target.package_name == binding.root.requested_name &&
               has_role(package_target, PackageRole::Root) &&
               target_contains_root(package_target, binding.root)) {
                matching_targets.push_back(&package_target);
            }
        }

        bool is_consistent = matching_targets.size() == 1 &&
                             binding.target->update.aur_package.has_value();
        if(is_consistent) {
            const PlannedPackageTarget& package_target = *matching_targets.front();
            const AurUpdateRemotePackage& remote =
                *binding.target->update.aur_package;
            is_consistent = remote.aur_name == package_target.package_name &&
                            remote.package_base == package_target.package_base;
        }
        if(is_consistent) continue;

        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::PackageTargetAttributionInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "Executable {} update root does not have exactly one matching planned package target.",
                AUR_SERVICE_NAME));
        attribute_issue_to_target(issue, *binding.target);
        issue.affected_roots.push_back(binding.root);
        issue.package_name = binding.root.requested_name;
        preparation.issues.push_back(std::move(issue));
    }
    return preparation.issues.empty();
}

std::optional<DesiredInstallReason> desired_reason_for_package_target(
    const PlannedPackageTarget& package_target,
    const std::vector<ExecutableRootBinding>& bindings,
    AurUpdatePreparationIssue& inconsistency) {
    const ExecutableRootBinding* update_root = nullptr;
    for(const auto& binding : bindings) {
        if(binding.root.requested_name != package_target.package_name ||
           !target_contains_root(package_target, binding.root)) {
            continue;
        }
        if(update_root != nullptr) {
            inconsistency.diagnostic = localization::translate_message(
                "Planned package target matches multiple executable update roots.");
            return std::nullopt;
        }
        update_root = &binding;
    }

    if(update_root != nullptr) {
        if(!has_role(package_target, PackageRole::Root) ||
           !update_root->target->desired_install_reason.has_value()) {
            inconsistency.diagnostic =
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal enum value "Root".
                    "Planned update root lost its {} role or desired install reason.",
                    ROOT_ROLE_NAME);
            return std::nullopt;
        }
        // LANDMINE(#267): Root roleから再計算するとdependency-installed rootを
        // Explicitへ誤昇格する。installed reason由来のpreflight値だけを使う。
        return *update_root->target->desired_install_reason;
    }

    if(has_role(package_target, PackageRole::Root) ||
       !std::any_of(
           package_target.roles.begin(), package_target.roles.end(),
           is_dependency_role)) {
        inconsistency.diagnostic = localization::translate_message(
            "Non-root planned package target has no attributable dependency role.");
        return std::nullopt;
    }
    return DesiredInstallReason::Dependency;
}

void retain_localized_build_unit_selection_issue(
    AurUpdateSourceBuildPreparation& preparation,
    AurUpdatePreparationReason reason,
    std::string diagnostic,
    const AurUpdateBuildUnitSelectionEntry* selection_entry = nullptr) {
    AurUpdatePreparationIssue issue = make_localized_preparation_issue(
        reason, std::move(diagnostic));
    if(selection_entry != nullptr) {
        issue.package_base = selection_entry->package_base;
        if(selection_entry->package_names.size() == 1) {
            issue.package_name = selection_entry->package_names.front();
        }
    }
    attribute_issue_to_all_executable_targets(issue, preparation);
    preparation.issues.push_back(std::move(issue));
}

template <std::size_t Size>
void retain_localized_build_unit_selection_issue(
    AurUpdateSourceBuildPreparation& preparation,
    AurUpdatePreparationReason reason,
    const char (&diagnostic)[Size],
    const AurUpdateBuildUnitSelectionEntry* selection_entry = nullptr) {
    retain_localized_build_unit_selection_issue(
        preparation, reason,
        localization::translate_message(diagnostic), selection_entry);
}

bool has_valid_external_satisfaction(
    const AurUpdateBuildUnitSelectionEntry& selection_entry) noexcept {
    if(!selection_entry.external_satisfaction.has_value()) return false;

    const AurUpdateExternalSatisfactionAttribution& external =
        *selection_entry.external_satisfaction;
    if(external.explicit_source_indexes.empty() ||
       external.source_identity_keys.empty() ||
       has_duplicate_value(external.explicit_source_indexes) ||
       has_duplicate_value(external.source_identity_keys) ||
       std::any_of(
           external.source_identity_keys.begin(),
           external.source_identity_keys.end(),
           [](const std::string& key) { return key.empty(); }) ||
       !external.matched_package_base.has_value() ||
       *external.matched_package_base != selection_entry.package_base) {
        return false;
    }

    if(external.matched_package_name.has_value() &&
       std::find(
           selection_entry.package_names.begin(),
           selection_entry.package_names.end(),
           *external.matched_package_name) ==
           selection_entry.package_names.end()) {
        return false;
    }
    return true;
}

bool validate_build_unit_selection(
    const BuildPlan& plan,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    AurUpdateSourceBuildPreparation& preparation) {
    if(build_unit_selection.entries.size() != plan.order.size()) {
        retain_localized_build_unit_selection_issue(
            preparation,
            AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                "Build-unit selection count does not match {} execution order.",
                BUILD_PLAN_TYPE_NAME));
    }

    std::size_t selected_execution_index = 0;
    const std::size_t comparable_count = std::min(
        build_unit_selection.entries.size(), plan.order.size());
    for(std::size_t order_index = 0; order_index < comparable_count;
        ++order_index) {
        const BuildPlanEntry& plan_entry = plan.order[order_index];
        const AurUpdateBuildUnitSelectionEntry& selection_entry =
            build_unit_selection.entries[order_index];

        if(selection_entry.build_plan_order_index != order_index ||
           selection_entry.package_base != plan_entry.package_base ||
           selection_entry.package_names != plan_entry.package_names) {
            retain_localized_build_unit_selection_issue(
                preparation,
                AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Build-unit selection identity or order differs from {} execution order.",
                    BUILD_PLAN_TYPE_NAME),
                &selection_entry);
        }

        if(!is_known_selection_status(selection_entry.status)) {
            retain_localized_build_unit_selection_issue(
                preparation,
                AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
                "Build-unit selection has an unknown status.",
                &selection_entry);
            continue;
        }

        if(selection_entry.status == AurUpdateBuildUnitSelectionStatus::
                                         SelectedForAurExecution) {
            if(selection_entry.selected_execution_index !=
                   selected_execution_index ||
               selection_entry.external_satisfaction.has_value()) {
                retain_localized_build_unit_selection_issue(
                    preparation,
                    AurUpdatePreparationReason::
                        BuildUnitSelectionInconsistent,
                    "Selected build unit does not have the expected dense execution index or retains external satisfaction.",
                    &selection_entry);
            }
            ++selected_execution_index;
            continue;
        }

        if(selection_entry.selected_execution_index.has_value() ||
           !has_valid_external_satisfaction(selection_entry)) {
            retain_localized_build_unit_selection_issue(
                preparation,
                AurUpdatePreparationReason::
                    ExternalSatisfactionInconsistent,
                "Externally satisfied build unit has an inconsistent execution index or explicit source attribution.",
                &selection_entry);
        }
    }
    return preparation.issues.empty();
}

bool collect_work_item_drafts(
    const BuildPlan& plan,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    const std::vector<ExecutableRootBinding>& bindings,
    AurUpdateSourceBuildPreparation& preparation,
    std::vector<UpdateWorkItemDraft>& drafts,
    bool needed) {
    BuildPlanArtifactTargetProjectionResult target_projection =
        project_build_plan_required_artifact_targets(plan);
    if(!target_projection.is_success()) {
        for(const auto& projection_issue : target_projection.failure()->issues) {
            const bool has_uncovered_issue_for_same_target = std::any_of(
                target_projection.failure()->issues.begin(),
                target_projection.failure()->issues.end(),
                [&projection_issue](
                    const BuildPlanArtifactTargetProjectionIssue& other) {
                    return other.kind ==
                               BuildPlanArtifactTargetProjectionIssueKind::
                                   UncoveredPlannedPackageTarget &&
                           other.package_name ==
                               projection_issue.package_name &&
                           other.package_base ==
                               projection_issue.package_base;
                });
            if(projection_issue.kind ==
                   BuildPlanArtifactTargetProjectionIssueKind::
                       RootAttributionInconsistent &&
               has_uncovered_issue_for_same_target) {
                continue;
            }

            const bool diagnostic_covers_root_attribution =
                projection_issue.kind ==
                BuildPlanArtifactTargetProjectionIssueKind::
                    RootAttributionInconsistent;
            std::string diagnostic = projection_issue.diagnostic;
            if(diagnostic_covers_root_attribution) {
                diagnostic = localization::translate_message(
                    "Planned package target roots cannot be attributed exactly to executable update targets.");
            } else if(projection_issue.kind ==
                      BuildPlanArtifactTargetProjectionIssueKind::
                          UncoveredPlannedPackageTarget) {
                diagnostic = localization::format_translated_message(
                    // TRANSLATORS: {} is the literal internal type name "BuildPlan".
                    "Planned package target must occur exactly once in {} execution order.",
                    BUILD_PLAN_TYPE_NAME);
            }
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    PackageTargetAttributionInconsistent,
                std::move(diagnostic));
            issue.package_name = projection_issue.package_name;
            issue.package_base = projection_issue.package_base;
            issue.build_plan_projection_issue = projection_issue;
            bool roots_are_exact = !projection_issue.roots.empty();
            for(const auto& root : projection_issue.roots) {
                const ExecutableRootBinding* binding =
                    find_root_binding(bindings, root);
                if(binding == nullptr) {
                    roots_are_exact = false;
                    break;
                }
                add_unique(
                    issue.affected_update_plan_indices,
                    binding->target->update_plan_index);
                add_unique(issue.affected_roots, root);
            }
            if(!roots_are_exact) {
                if(!diagnostic_covers_root_attribution) {
                    issue.diagnostic = localization::format_translated_message(
                        "{} Its roots cannot be attributed exactly to executable update targets.",
                        issue.diagnostic);
                }
                issue.affected_update_plan_indices.clear();
                issue.affected_roots.clear();
                attribute_issue_to_all_executable_targets(issue, preparation);
            }
            preparation.issues.push_back(std::move(issue));
        }
        return false;
    }

    std::vector<AurUpdateProjectedBuildUnit> projected_build_units;
    projected_build_units.reserve(
        target_projection.success()->build_units.size());
    for(const auto& projected_targets :
        target_projection.success()->build_units) {
        const std::size_t order_index =
            projected_targets.build_plan_order_index;
        const BuildPlanEntry& entry = plan.order[order_index];
        const AurUpdateBuildUnitSelectionEntry& selection_entry =
            build_unit_selection.entries[order_index];

        UpdateWorkItemDraft draft;
        draft.build_plan_order_index = order_index;
        draft.work_item.request.checkout_name = entry.package_base;
        draft.work_item.request.git_url =
            AUR_BASE_URL + entry.package_base + ".git";
        draft.work_item.request.aur_review_identity =
            PackageBaseIdentity::make(
                PackageSourceIdentity::aur(
                    SourceLocationIdentity::known_git_remote(
                        draft.work_item.request.git_url)),
                entry.package_base);
        draft.work_item.request.empty_value_policy =
            SourceEnvironmentEmptyValuePolicy::Omit;
        draft.work_item.request.needed = needed;
        draft.work_item.request.only_if_updated = false;
        draft.work_item.request.installed_snapshot = std::nullopt;
        draft.work_item.request.update_baseline = std::nullopt;
        draft.work_item.required_target_provenance =
            RequiredTargetProvenance::AurBuildPlanProjection;
        draft.work_item.artifact_lifecycle_intent =
            ArtifactLifecycleIntent::PackageBaseSet;
        draft.work_item.uses_system_update_baseline = false;
        draft.work_item.configured_repository_order =
            plan.configured_repository_order;
        attach_selected_repository_providers(draft.work_item, plan);

        bool unit_is_consistent = true;
        for(const auto& generic_target : projected_targets.required_targets) {
            const PlannedPackageTarget* package_target = nullptr;
            for(const auto& candidate : plan.package_targets) {
                if(candidate.package_name != generic_target.package_name ||
                   candidate.package_base != generic_target.package_base) {
                    continue;
                }
                if(package_target != nullptr) {
                    package_target = nullptr;
                    break;
                }
                package_target = &candidate;
            }

            AurUpdatePreparationIssue target_issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    PackageTargetAttributionInconsistent,
                "Planned package target attribution is inconsistent.");
            target_issue.package_name = generic_target.package_name;
            target_issue.package_base = generic_target.package_base;
            if(package_target == nullptr) {
                attribute_issue_to_all_executable_targets(
                    target_issue, preparation);
                preparation.issues.push_back(std::move(target_issue));
                unit_is_consistent = false;
                continue;
            }

            std::vector<std::size_t> child_update_plan_indices;
            std::vector<RootTargetIdentity> child_roots;
            const bool roots_are_attributed =
                collect_exact_package_target_attribution(
                    *package_target, bindings,
                    child_update_plan_indices, child_roots);
            const bool roles_are_known = !package_target->roles.empty() &&
                                         std::all_of(
                                             package_target->roles.begin(),
                                             package_target->roles.end(), is_known_role);
            if(!roots_are_attributed || !roles_are_known) {
                target_issue.diagnostic = !roots_are_attributed
                                              ? localization::translate_message(
                                                    "Planned package target roots cannot be attributed exactly to executable update targets.")
                                              : localization::translate_message(
                                                    "Planned package target contains no known package role.");
                attribute_issue_to_all_executable_targets(
                    target_issue, preparation);
                preparation.issues.push_back(std::move(target_issue));
                unit_is_consistent = false;
                continue;
            }

            for(const auto index : child_update_plan_indices) {
                add_unique(
                    target_issue.affected_update_plan_indices, index);
            }
            for(const auto& root : child_roots) {
                add_unique(target_issue.affected_roots, root);
            }
            std::optional<DesiredInstallReason> desired_reason =
                desired_reason_for_package_target(
                    *package_target, bindings, target_issue);
            if(!desired_reason.has_value() ||
               !is_known_desired_install_reason(*desired_reason)) {
                preparation.issues.push_back(std::move(target_issue));
                unit_is_consistent = false;
                continue;
            }

            RequiredPackageArtifactTarget required_target{
                entry.package_base,
                package_target->package_name,
                *desired_reason};
            draft.work_item.required_targets.push_back(required_target);
            draft.required_target_attributions.push_back(
                AurUpdateRequiredTargetAttribution{
                    required_target,
                    child_update_plan_indices,
                    child_roots,
                    package_target->roles});
            for(const auto index : child_update_plan_indices) {
                for(const auto& binding : bindings) {
                    if(binding.target->update_plan_index == index && binding.target->update.installed_name == package_target->package_name &&
                       binding.target->update.aur_package && binding.target->update.aur_package->package_base == entry.package_base &&
                       aur_update_basis(binding.target->update) == AurUpdateBasis::GitRevision)
                        draft.work_item.request.authoritative_devel_update = true;
                    if(binding.target->update_plan_index == index && has_aur_update_bootstrap_intent(binding.target->update) &&
                       binding.target->update.installed_name == package_target->package_name &&
                       binding.target->update.aur_package->package_base == entry.package_base) {
                        draft.work_item.request.devel_tracking_bootstrap = binding.target->update.bootstrap;
                    }
                }
                add_unique(draft.affected_update_plan_indices, index);
            }
            for(const auto& root : child_roots) {
                add_unique(draft.affected_roots, root);
            }
        }

        if(!unit_is_consistent) continue;
        if(draft.work_item.required_targets.size() == 1) {
            draft.work_item.request.package_name =
                draft.work_item.required_targets.front().package_name;
        }

        projected_build_units.push_back(AurUpdateProjectedBuildUnit{
            order_index,
            entry.package_base,
            draft.required_target_attributions,
            draft.affected_update_plan_indices,
            draft.affected_roots});

        if(selection_entry.status == AurUpdateBuildUnitSelectionStatus::
                                         ExternallySatisfiedByExplicitSourcePackageBase) {
            const bool is_singular =
                draft.required_target_attributions.size() == 1;
            std::string compatibility_package_name;
            std::vector<PackageRole> compatibility_roles;
            std::optional<DesiredInstallReason> compatibility_reason;
            if(is_singular) {
                const AurUpdateRequiredTargetAttribution& child =
                    draft.required_target_attributions.front();
                compatibility_package_name =
                    child.required_target.package_name;
                compatibility_roles = child.roles;
                compatibility_reason =
                    child.required_target.desired_reason;
            }
            preparation.externally_satisfied_build_units.push_back(
                AurUpdateExternallySatisfiedBuildUnit{
                    order_index,
                    std::move(compatibility_package_name),
                    entry.package_base,
                    entry.package_names,
                    draft.required_target_attributions,
                    draft.affected_update_plan_indices,
                    draft.affected_roots,
                    std::move(compatibility_roles),
                    compatibility_reason,
                    *selection_entry.external_satisfaction});
            continue;
        }

        drafts.push_back(std::move(draft));
    }

    if(!preparation.issues.empty()) return false;
    preparation.projected_build_units = std::move(projected_build_units);

    // POLICY(#281): dependency unitだけがselectedでもroot updateは完了しない。
    // 各Executable root自身のwork itemがcapabilityへ残ることをmutation前に固定する。
    for(const auto& binding : bindings) {
        const bool has_selected_root_work_item = std::any_of(
            drafts.begin(), drafts.end(),
            [&binding](const UpdateWorkItemDraft& draft) {
                const bool contains_root_child = std::any_of(
                    draft.work_item.required_targets.begin(),
                    draft.work_item.required_targets.end(),
                    [&binding](
                        const RequiredPackageArtifactTarget& target) {
                        return target.package_name ==
                               binding.root.requested_name;
                    });
                return contains_root_child &&
                       std::find(
                           draft.affected_roots.begin(),
                           draft.affected_roots.end(), binding.root) !=
                           draft.affected_roots.end();
            });
        if(has_selected_root_work_item) continue;

        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::BuildUnitSelectionInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "Executable {} update root has no selected build unit.",
                AUR_SERVICE_NAME));
        attribute_issue_to_target(issue, *binding.target);
        issue.affected_roots.push_back(binding.root);
        issue.package_name = binding.root.requested_name;
        preparation.issues.push_back(std::move(issue));
    }
    return preparation.issues.empty();
}

void retain_loaded_warnings(
    const std::string& preference_name,
    const SourcePreferenceLoaded& loaded,
    const UpdateWorkItemDraft& draft,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& diagnostic : loaded.warnings) {
        preparation.warnings.push_back(AurUpdatePreparationWarning{
            preference_name,
            loaded.entry_path,
            draft.affected_update_plan_indices,
            draft.affected_roots,
            diagnostic});
    }
}

std::optional<SourceBuildEnvironment> read_strict_environment(
    const std::string& preference_name,
    const UpdateWorkItemDraft& draft,
    AurUpdateSourceBuildPreparation& preparation) {
    StrictSourcePreferenceResult result;
    try {
        result = read_source_preference_strict(preference_name);
    } catch(const std::exception& error) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::GenericPreparationInconsistent,
            localization::format_translated_message(
                "Strict source preference reader threw an unexpected exception: {}",
                error.what()));
        issue.package_name = preference_name;
        issue.package_base = draft.work_item.request.checkout_name;
        attribute_issue_to_draft(issue, draft);
        preparation.issues.push_back(std::move(issue));
        return std::nullopt;
    } catch(...) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::GenericPreparationInconsistent,
            "Strict source preference reader threw an unknown exception.");
        issue.package_name = preference_name;
        issue.package_base = draft.work_item.request.checkout_name;
        attribute_issue_to_draft(issue, draft);
        preparation.issues.push_back(std::move(issue));
        return std::nullopt;
    }

    if(std::get_if<SourcePreferenceAbsent>(&result) != nullptr) {
        return SourceBuildEnvironment{};
    }
    if(const auto* loaded = std::get_if<SourcePreferenceLoaded>(&result)) {
        retain_loaded_warnings(
            preference_name, *loaded, draft, preparation);
        return loaded->environment;
    }

    const SourcePreferenceFailure& failure =
        std::get<SourcePreferenceFailure>(result);
    AurUpdatePreparationIssue issue = make_localized_preparation_issue(
        AurUpdatePreparationReason::SourcePreferenceUnavailable,
        failure.diagnostic);
    issue.package_name = preference_name;
    issue.package_base = draft.work_item.request.checkout_name;
    issue.source_preference_failure = failure;
    attribute_issue_to_draft(issue, draft);
    preparation.issues.push_back(std::move(issue));
    return std::nullopt;
}

void consume_strict_source_preferences(
    std::vector<UpdateWorkItemDraft>& drafts,
    AurUpdateSourceBuildPreparation& preparation) {
    for(auto& draft : drafts) {
        const bool is_singular = draft.work_item.required_targets.size() == 1;
        const std::string& package_base =
            draft.work_item.request.checkout_name;
        const std::string& preference_name = is_singular
                                                 ? draft.work_item.request.package_name
                                                 : package_base;

        std::optional<SourceBuildEnvironment> selected_environment =
            read_strict_environment(
                preference_name, draft, preparation);
        if(!selected_environment.has_value()) continue;

        // POLICY(#242,#267): fallback eligibilityはforward可能なnonempty assignment、
        // PKGDEST definition、requested/Base identityの3条件を既存順で判定する。
        if(!selected_environment->has_forwarded_nonempty_assignment() &&
           !selected_environment->defines("PKGDEST") &&
           is_singular && preference_name != package_base) {
            selected_environment = read_strict_environment(
                package_base, draft, preparation);
            if(!selected_environment.has_value()) continue;
        }

        if(selected_environment->defines("PKGDEST")) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::SourcePreferencePkgdestConflict,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal environment key "PKGDEST".
                    "Source preference defines invocation-owned {}.",
                    PKGDEST_ENVIRONMENT_KEY));
            issue.package_name = preference_name;
            issue.package_base = package_base;
            attribute_issue_to_draft(issue, draft);
            preparation.issues.push_back(std::move(issue));
            continue;
        }
        draft.work_item.request.custom_environment =
            std::move(*selected_environment);
    }
}

void apply_saved_source_preference_policy(
    SavedSourcePreferencePolicy policy,
    std::vector<UpdateWorkItemDraft>& drafts,
    AurUpdateSourceBuildPreparation& preparation) {
    switch(policy) {
        case SavedSourcePreferencePolicy::Strict:
            consume_strict_source_preferences(drafts, preparation);
            return;
        case SavedSourcePreferencePolicy::Ignore:
            // POLICY(#505): normal AUR work starts with the draft's empty
            // custom environment and never calls the saved-preference authority.
            return;
    }
    throw std::logic_error("Unknown saved source preference policy.");
}

void validate_static_work_items(
    const std::vector<UpdateWorkItemDraft>& drafts,
    AurUpdateSourceBuildPreparation& preparation) {
    for(const auto& draft : drafts) {
        try {
            require_static_production_source_build_work_item(
                draft.work_item);
        } catch(const std::exception& error) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::StaticWorkItemInvalid,
                error.what());
            issue.package_name = draft.work_item.request.package_name;
            issue.package_base = draft.work_item.request.checkout_name;
            attribute_issue_to_draft(issue, draft);
            preparation.issues.push_back(std::move(issue));
        } catch(...) {
            AurUpdatePreparationIssue issue = make_localized_preparation_issue(
                AurUpdatePreparationReason::StaticWorkItemInvalid,
                "Static production source-build work item validation threw an unknown exception.");
            issue.package_name = draft.work_item.request.package_name;
            issue.package_base = draft.work_item.request.checkout_name;
            attribute_issue_to_draft(issue, draft);
            preparation.issues.push_back(std::move(issue));
        }
    }
}

std::vector<ProductionSourceBuildWorkItem> release_work_items(
    std::vector<UpdateWorkItemDraft> drafts) {
    std::vector<ProductionSourceBuildWorkItem> work_items;
    work_items.reserve(drafts.size());
    for(auto& draft : drafts) {
        work_items.push_back(std::move(draft.work_item));
    }
    return work_items;
}

std::vector<AurUpdatePreparedWorkItemAttribution>
snapshot_work_item_attributions(
    const std::vector<UpdateWorkItemDraft>& drafts) {
    std::vector<AurUpdatePreparedWorkItemAttribution> attributions;
    attributions.reserve(drafts.size());
    for(std::size_t index = 0; index < drafts.size(); ++index) {
        const UpdateWorkItemDraft& draft = drafts[index];
        attributions.push_back(AurUpdatePreparedWorkItemAttribution{
            index,
            draft.build_plan_order_index,
            draft.work_item.request.package_name,
            draft.work_item.request.checkout_name,
            draft.required_target_attributions,
            draft.affected_update_plan_indices,
            draft.affected_roots});
    }
    return attributions;
}

template <typename Observation>
bool has_update_target_snapshot(
    const Observation& preparation,
    std::size_t update_plan_index) noexcept {
    return std::any_of(
        preparation.affected_update_targets.begin(),
        preparation.affected_update_targets.end(),
        [update_plan_index](const AurUpdateExecutionTarget& target) {
            return target.update_plan_index == update_plan_index;
        });
}

template <typename Observation>
bool has_root_snapshot(
    const Observation& preparation,
    const RootTargetIdentity& root) noexcept {
    return std::find(
               preparation.affected_roots.begin(),
               preparation.affected_roots.end(), root) !=
           preparation.affected_roots.end();
}

template <typename Value, typename ChildValues>
bool is_exact_first_seen_union(
    const std::vector<Value>& aggregate,
    const std::vector<AurUpdateRequiredTargetAttribution>& children,
    ChildValues child_values) noexcept {
    std::size_t aggregate_index = 0;
    for(std::size_t child_index = 0; child_index < children.size();
        ++child_index) {
        const std::vector<Value>& values = child_values(children[child_index]);
        for(std::size_t value_index = 0; value_index < values.size();
            ++value_index) {
            const Value& value = values[value_index];
            bool was_seen = std::find(
                                values.begin(),
                                values.begin() + value_index, value) !=
                            values.begin() + value_index;
            for(std::size_t prior_child = 0;
                !was_seen && prior_child < child_index; ++prior_child) {
                const std::vector<Value>& prior_values =
                    child_values(children[prior_child]);
                was_seen = std::find(
                               prior_values.begin(), prior_values.end(),
                               value) != prior_values.end();
            }
            if(was_seen) continue;
            if(aggregate_index >= aggregate.size() ||
               aggregate[aggregate_index] != value) {
                return false;
            }
            ++aggregate_index;
        }
    }
    return aggregate_index == aggregate.size();
}

template <typename Observation>
bool has_exact_projected_build_unit_correlation(
    const Observation& preparation) noexcept {
    if(preparation.projected_build_units.size() !=
       preparation.build_unit_selection.entries.size()) {
        return false;
    }

    for(std::size_t order_index = 0;
        order_index < preparation.projected_build_units.size();
        ++order_index) {
        const AurUpdateProjectedBuildUnit& build_unit =
            preparation.projected_build_units[order_index];
        const AurUpdateBuildUnitSelectionEntry& selection_entry =
            preparation.build_unit_selection.entries[order_index];
        if(build_unit.build_plan_order_index != order_index ||
           build_unit.package_base != selection_entry.package_base ||
           build_unit.required_target_attributions.size() !=
               selection_entry.package_names.size() ||
           build_unit.affected_update_plan_indices.empty() ||
           build_unit.affected_roots.empty() ||
           has_duplicate_value(build_unit.affected_update_plan_indices) ||
           has_duplicate_value(build_unit.affected_roots)) {
            return false;
        }

        for(std::size_t child_index = 0;
            child_index < build_unit.required_target_attributions.size();
            ++child_index) {
            const AurUpdateRequiredTargetAttribution& child =
                build_unit.required_target_attributions[child_index];
            if(child.required_target.package_base != build_unit.package_base ||
               child.required_target.package_name !=
                   selection_entry.package_names[child_index] ||
               !is_known_desired_install_reason(
                   child.required_target.desired_reason) ||
               child.affected_update_plan_indices.empty() ||
               child.affected_roots.empty() || child.roles.empty() ||
               has_duplicate_value(child.affected_update_plan_indices) ||
               has_duplicate_value(child.affected_roots) ||
               has_duplicate_value(child.roles) ||
               !std::all_of(
                   child.affected_update_plan_indices.begin(),
                   child.affected_update_plan_indices.end(),
                   [&preparation](std::size_t update_plan_index) {
                       return has_update_target_snapshot(
                           preparation, update_plan_index);
                   }) ||
               !std::all_of(
                   child.affected_roots.begin(),
                   child.affected_roots.end(),
                   [&preparation](const RootTargetIdentity& root) {
                       return has_root_snapshot(preparation, root);
                   }) ||
               !std::all_of(
                   child.roles.begin(), child.roles.end(),
                   is_known_role)) {
                return false;
            }
        }

        if(!is_exact_first_seen_union<std::size_t>(
               build_unit.affected_update_plan_indices,
               build_unit.required_target_attributions,
               [](const AurUpdateRequiredTargetAttribution& child)
                   -> const std::vector<std::size_t>& {
                   return child.affected_update_plan_indices;
               }) ||
           !is_exact_first_seen_union<RootTargetIdentity>(
               build_unit.affected_roots,
               build_unit.required_target_attributions,
               [](const AurUpdateRequiredTargetAttribution& child)
                   -> const std::vector<RootTargetIdentity>& {
                   return child.affected_roots;
               })) {
            return false;
        }
    }
    return true;
}

bool has_exact_required_target_attributions(
    const std::vector<AurUpdateRequiredTargetAttribution>& lhs,
    const std::vector<AurUpdateRequiredTargetAttribution>& rhs) noexcept {
    if(lhs.size() != rhs.size()) return false;
    for(std::size_t index = 0; index < lhs.size(); ++index) {
        if(lhs[index].required_target.package_base !=
               rhs[index].required_target.package_base ||
           lhs[index].required_target.package_name !=
               rhs[index].required_target.package_name ||
           lhs[index].required_target.desired_reason !=
               rhs[index].required_target.desired_reason ||
           lhs[index].affected_update_plan_indices !=
               rhs[index].affected_update_plan_indices ||
           lhs[index].affected_roots != rhs[index].affected_roots ||
           lhs[index].roles != rhs[index].roles) {
            return false;
        }
    }
    return true;
}

bool has_compatible_singular_package_name(
    const std::string& package_name,
    const std::vector<std::string>& package_names) noexcept {
    if(package_names.size() == 1) {
        return package_name == package_names.front();
    }
    return package_name.empty();
}

bool has_exact_external_compatibility_fields(
    const AurUpdateExternallySatisfiedBuildUnit& external,
    const AurUpdateProjectedBuildUnit& projected) noexcept {
    if(projected.required_target_attributions.size() == 1) {
        const AurUpdateRequiredTargetAttribution& child =
            projected.required_target_attributions.front();
        return external.package_name ==
                   child.required_target.package_name &&
               external.roles == child.roles &&
               external.desired_install_reason ==
                   child.required_target.desired_reason;
    }

    // multipleではlegacy singular fieldsをidentity/reason authorityにしない。
    return external.package_name.empty() && external.roles.empty() &&
           !external.desired_install_reason.has_value();
}

template <typename Observation>
bool has_exact_build_unit_selection_correlation(
    const std::vector<AurUpdatePreparedWorkItemAttribution>& attributions,
    const Observation& preparation) noexcept {
    if(!has_exact_projected_build_unit_correlation(preparation)) return false;

    std::size_t selected_index = 0;
    std::size_t external_index = 0;
    for(std::size_t order_index = 0;
        order_index < preparation.build_unit_selection.entries.size();
        ++order_index) {
        const AurUpdateBuildUnitSelectionEntry& selection_entry =
            preparation.build_unit_selection.entries[order_index];
        if(selection_entry.build_plan_order_index != order_index ||
           selection_entry.package_names.empty() ||
           selection_entry.package_base.empty() ||
           !is_known_selection_status(selection_entry.status)) {
            return false;
        }
        const AurUpdateProjectedBuildUnit& projected =
            preparation.projected_build_units[order_index];

        if(selection_entry.status == AurUpdateBuildUnitSelectionStatus::
                                         SelectedForAurExecution) {
            if(selection_entry.selected_execution_index != selected_index ||
               selection_entry.external_satisfaction.has_value() ||
               selected_index >= attributions.size()) {
                return false;
            }
            const AurUpdatePreparedWorkItemAttribution& attribution =
                attributions[selected_index];
            if(attribution.invocation_work_item_index != selected_index ||
               attribution.build_plan_order_index != order_index ||
               !has_compatible_singular_package_name(
                   attribution.package_name,
                   selection_entry.package_names) ||
               attribution.package_base != selection_entry.package_base ||
               !has_exact_required_target_attributions(
                   attribution.required_target_attributions,
                   projected.required_target_attributions) ||
               attribution.affected_update_plan_indices !=
                   projected.affected_update_plan_indices ||
               attribution.affected_roots != projected.affected_roots) {
                return false;
            }
            ++selected_index;
            continue;
        }

        if(selection_entry.selected_execution_index.has_value() ||
           !has_valid_external_satisfaction(selection_entry) ||
           external_index >=
               preparation.externally_satisfied_build_units.size()) {
            return false;
        }
        const AurUpdateExternallySatisfiedBuildUnit& external =
            preparation.externally_satisfied_build_units[external_index];
        if(external.build_plan_order_index != order_index ||
           external.package_base != selection_entry.package_base ||
           external.plan_package_names != selection_entry.package_names ||
           !has_exact_required_target_attributions(
               external.required_target_attributions,
               projected.required_target_attributions) ||
           external.affected_update_plan_indices.empty() ||
           external.affected_roots.empty() ||
           has_duplicate_value(external.affected_update_plan_indices) ||
           has_duplicate_value(external.affected_roots) ||
           !std::all_of(
               external.affected_update_plan_indices.begin(),
               external.affected_update_plan_indices.end(),
               [&preparation](std::size_t update_plan_index) {
                   return has_update_target_snapshot(
                       preparation, update_plan_index);
               }) ||
           !std::all_of(
               external.affected_roots.begin(),
               external.affected_roots.end(),
               [&preparation](const RootTargetIdentity& root) {
                   return has_root_snapshot(preparation, root);
               }) ||
           external.affected_update_plan_indices !=
               projected.affected_update_plan_indices ||
           external.affected_roots != projected.affected_roots ||
           !has_exact_external_compatibility_fields(external, projected) ||
           external.external_satisfaction !=
               *selection_entry.external_satisfaction) {
            return false;
        }
        ++external_index;
    }

    return selected_index == attributions.size() &&
           external_index ==
               preparation.externally_satisfied_build_units.size();
}

template <typename WorkItem, typename Observation>
bool has_exact_prepared_correlation(
    const std::vector<WorkItem>& work_items,
    const std::vector<AurUpdatePreparedWorkItemAttribution>& attributions,
    const Observation& preparation) noexcept {
    if(work_items.empty() || attributions.size() != work_items.size() ||
       preparation.affected_update_targets.empty() ||
       preparation.affected_roots.empty() ||
       !has_exact_build_unit_selection_correlation(
           attributions, preparation)) {
        return false;
    }

    for(std::size_t index = 0; index < work_items.size(); ++index) {
        const WorkItem& work_item = work_items[index];
        const AurUpdatePreparedWorkItemAttribution& attribution =
            attributions[index];
        if(attribution.build_plan_order_index >=
           preparation.projected_build_units.size()) {
            return false;
        }
        const AurUpdateProjectedBuildUnit& projected_build_unit =
            preparation.projected_build_units[attribution.build_plan_order_index];
        std::shared_ptr<const DevelTrackingBootstrapTrial> expected_bootstrap;
        for(const auto& target : preparation.affected_update_targets) {
            if(target.update.installed_name != work_item.request.package_name ||
               !target.update.aur_package ||
               target.update.aur_package->package_base != work_item.request.checkout_name) continue;
            if(target.update.bootstrap) {
                if(expected_bootstrap || !has_aur_update_bootstrap_intent(target.update)) return false;
                expected_bootstrap = target.update.bootstrap;
            }
        }
        // The private execution capability is published only after the exact
        // root intent survives generic preparation. Dependencies receive none.
        if(work_item.request.devel_tracking_bootstrap != expected_bootstrap ||
           (expected_bootstrap && work_item.required_targets.size() != 1)) return false;
        if(attribution.invocation_work_item_index != index ||
           attribution.package_name != work_item.request.package_name ||
           attribution.package_base != work_item.request.checkout_name ||
           work_item.required_targets.size() !=
               projected_build_unit.required_target_attributions.size() ||
           attribution.affected_update_plan_indices.empty() ||
           attribution.affected_roots.empty() ||
           has_duplicate_value(attribution.affected_update_plan_indices) ||
           has_duplicate_value(attribution.affected_roots)) {
            return false;
        }
        if(!std::all_of(
               attribution.affected_update_plan_indices.begin(),
               attribution.affected_update_plan_indices.end(),
               [&preparation](std::size_t update_plan_index) {
                   return has_update_target_snapshot(
                       preparation, update_plan_index);
               }) ||
           !std::all_of(
               attribution.affected_roots.begin(),
               attribution.affected_roots.end(),
               [&preparation](const RootTargetIdentity& root) {
                   return has_root_snapshot(preparation, root);
               })) {
            return false;
        }
        for(std::size_t child_index = 0;
            child_index < work_item.required_targets.size(); ++child_index) {
            const RequiredPackageArtifactTarget& work_item_target =
                work_item.required_targets[child_index];
            const RequiredPackageArtifactTarget& projected_target =
                projected_build_unit
                    .required_target_attributions[child_index]
                    .required_target;
            if(work_item_target.package_base != projected_target.package_base ||
               work_item_target.package_name != projected_target.package_name ||
               work_item_target.desired_reason !=
                   projected_target.desired_reason) {
                return false;
            }
        }
    }

    for(const auto& target : preparation.affected_update_targets) {
        const bool is_attributed = std::any_of(
            attributions.begin(), attributions.end(),
            [&target](
                const AurUpdatePreparedWorkItemAttribution& attribution) {
                return std::find(
                           attribution.affected_update_plan_indices.begin(),
                           attribution.affected_update_plan_indices.end(),
                           target.update_plan_index) !=
                       attribution.affected_update_plan_indices.end();
            });
        if(!is_attributed) return false;
    }
    for(const auto& root : preparation.affected_roots) {
        const bool is_attributed = std::any_of(
            attributions.begin(), attributions.end(),
            [&root](
                const AurUpdatePreparedWorkItemAttribution& attribution) {
                return std::find(
                           attribution.affected_roots.begin(),
                           attribution.affected_roots.end(), root) !=
                       attribution.affected_roots.end();
            });
        if(!is_attributed) return false;
    }
    return true;
}

AurUpdateBuildUnitSelection make_all_selected_build_unit_selection(
    const AurUpdateExecutionPreflight& preflight) {
    AurUpdateBuildUnitSelection selection;
    if(!preflight.build_plan.has_value()) return selection;

    const std::vector<BuildPlanEntry>& order = preflight.build_plan->order;
    selection.entries.reserve(order.size());
    for(std::size_t index = 0; index < order.size(); ++index) {
        selection.entries.push_back(AurUpdateBuildUnitSelectionEntry{
            index,
            order[index].package_base,
            order[index].package_names,
            AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution,
            index,
            std::nullopt});
    }
    return selection;
}

} // namespace

PreparedAurUpdateSourceBuildInvocation::
    PreparedAurUpdateSourceBuildInvocation(
        PreparedProductionSourceBuildInvocation&& production_invocation,
        std::vector<AurUpdatePreparedWorkItemAttribution>&&
            work_item_attributions) noexcept
    : production_invocation_(std::move(production_invocation)),
      work_item_attributions_(std::move(work_item_attributions)) {
}

PreparedAurUpdateSourceBuildInvocation::
    PreparedAurUpdateSourceBuildInvocation(
        PreparedAurUpdateSourceBuildInvocation&& other) noexcept
    : production_invocation_(std::move(other.production_invocation_)),
      work_item_attributions_(std::move(other.work_item_attributions_)),
      valid_(std::exchange(other.valid_, false)) {
}

bool AurUpdateSourceBuildPreparation::is_prepared() const noexcept {
    return has_known_devel_requires_check_policy(
               devel_requires_check_policy) &&
           has_valid_update_target_snapshots(
               affected_update_targets,
               devel_requires_check_policy) &&
           invocation.has_value() && invocation->is_valid() && issues.empty() &&
           has_exact_prepared_correlation(
               invocation->production_invocation_.work_items,
               invocation->work_item_attributions(), *this);
}

bool AurUpdateSourceBuildPreparation::is_noop() const noexcept {
    return has_known_devel_requires_check_policy(
               devel_requires_check_policy) &&
           has_valid_update_target_snapshots(
               affected_update_targets,
               devel_requires_check_policy) &&
           !invocation.has_value() && issues.empty();
}

bool AurUpdateSourceBuildPreparation::is_blocked() const noexcept {
    return !is_prepared() && !is_noop();
}

bool AurUpdateSourceBuildObservation::is_ready() const noexcept {
    return has_known_devel_requires_check_policy(
               devel_requires_check_policy) &&
           has_valid_update_target_snapshots(
               affected_update_targets,
               devel_requires_check_policy) &&
           production_preflight.has_value() && issues.empty() &&
           has_exact_prepared_correlation(
               production_preflight->work_items,
               work_item_attributions, *this);
}

bool AurUpdateSourceBuildObservation::is_noop() const noexcept {
    return has_known_devel_requires_check_policy(
               devel_requires_check_policy) &&
           has_valid_update_target_snapshots(
               affected_update_targets,
               devel_requires_check_policy) &&
           !production_preflight.has_value() && issues.empty();
}

bool AurUpdateSourceBuildObservation::is_blocked() const noexcept {
    return !is_ready() && !is_noop();
}

void seed_aur_update_source_build_cache(
    AurUpdateSourceBuildPreparation& preparation,
    const ValidatedCacheRoot& cache_root) {
    if(!preparation.invocation.has_value()) return;
    seed_production_source_build_cache(
        preparation.invocation->production_invocation_, cache_root);
}

namespace {

struct AurUpdateSourceBuildDraftPreparation {
    AurUpdateSourceBuildPreparation observation;
    std::vector<UpdateWorkItemDraft> drafts;
};

AurUpdateSourceBuildDraftPreparation
prepare_aur_update_source_build_drafts(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed) {
    AurUpdateSourceBuildPreparation preparation;
    preparation.build_unit_selection = build_unit_selection;
    preparation.devel_requires_check_policy =
        devel_requires_check_policy;

    const bool policy_matches =
        is_known_devel_requires_check_policy(
            devel_requires_check_policy) &&
        preflight.devel_requires_check_policy ==
            std::optional<DevelRequiresCheckPolicy>{
                devel_requires_check_policy};
    if(!policy_matches) {
        AurUpdatePreparationIssue issue =
            make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    DevelRequiresCheckPolicyInconsistent,
                std::string{
                    DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
        preparation.issues.push_back(std::move(issue));
        return {std::move(preparation), {}};
    }

    if(devel_requires_check_policy ==
           DevelRequiresCheckPolicy::SkipIndependentTarget &&
       !has_valid_aur_update_execution_policy_snapshot(preflight)) {
        AurUpdatePreparationIssue issue =
            make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    DevelRequiresCheckPolicyInconsistent,
                std::string{
                    DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
        preparation.issues.push_back(std::move(issue));
        return {std::move(preparation), {}};
    }
    if(devel_requires_check_policy ==
           DevelRequiresCheckPolicy::SkipIndependentTarget &&
       !has_complete_aur_update_required_devel_relation_snapshot(
           preflight)) {
        AurUpdatePreparationIssue issue =
            make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    DevelRequiresCheckPolicyInconsistent,
                std::string{
                    DEVEL_REQUIRES_CHECK_POLICY_DIAGNOSTIC});
        preparation.issues.push_back(std::move(issue));
        return {std::move(preparation), {}};
    }

    // Blocking preflight and normal no-op never reach preference or DB
    // authority. The same snapshot feeds both executable preparation and the
    // capability-free dry-run observation.
    retain_non_skipped_skip_kind_inconsistencies(
        preflight, preparation);
    retain_skipped_preflight_inconsistencies(preflight, preparation);
    if(has_blocking_preflight_targets(preflight) ||
       std::any_of(
           preflight.targets.begin(), preflight.targets.end(),
           [](const AurUpdateExecutionTarget& target) {
               return !is_known_status(target.status);
           })) {
        retain_preflight_blockers(preflight, preparation);
        return {std::move(preparation), {}};
    }
    if(!preparation.issues.empty()) {
        return {std::move(preparation), {}};
    }
    if(!has_executable_preflight_targets(preflight)) {
        if(!build_unit_selection.entries.empty() ||
           (preflight.build_plan.has_value() &&
            !preflight.build_plan->order.empty())) {
            retain_localized_build_unit_selection_issue(
                preparation,
                AurUpdatePreparationReason::
                    BuildUnitSelectionInconsistent,
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "{} update preflight has build-unit selection without an executable target.",
                    AUR_SERVICE_NAME));
        }
        return {std::move(preparation), {}};
    }

    retain_executable_preflight_inconsistencies(preflight, preparation);
    if(!preparation.issues.empty()) {
        return {std::move(preparation), {}};
    }

    for(const auto& target : preflight.targets) {
        if(target.status == AurUpdateExecutionTargetStatus::Executable ||
           target.status == AurUpdateExecutionTargetStatus::Skipped) {
            continue;
        }

        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::PreflightInconsistent,
            localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "{} update preflight contains an unexpected target status.",
                AUR_SERVICE_NAME));
        attribute_issue_to_target(issue, target);
        preparation.issues.push_back(std::move(issue));
    }
    if(!preparation.issues.empty()) {
        return {std::move(preparation), {}};
    }

    if(!preflight.build_plan.has_value()) {
        for(const auto& target : preflight.targets) {
            if(target.status == AurUpdateExecutionTargetStatus::Executable) {
                preparation.affected_update_targets.push_back(target);
            }
        }
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::BuildPlanMissing,
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are the literal service
                // name "AUR" and internal type name "BuildPlan".
                "Executable {} update preflight has no combined {}.",
                AUR_SERVICE_NAME, BUILD_PLAN_TYPE_NAME));
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
        return {std::move(preparation), {}};
    }

    const BuildPlan& plan = *preflight.build_plan;
    if(plan.order.empty()) {
        for(const auto& target : preflight.targets) {
            if(target.status == AurUpdateExecutionTargetStatus::Executable) {
                preparation.affected_update_targets.push_back(target);
            }
        }
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::BuildPlanOrderEmpty,
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are the literal service
                // name "AUR" and internal type name "BuildPlan".
                "Executable {} update {} has an empty execution order.",
                AUR_SERVICE_NAME, BUILD_PLAN_TYPE_NAME));
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
        return {std::move(preparation), {}};
    }

    std::vector<ExecutableRootBinding> bindings;
    if(!collect_executable_root_bindings(
           preflight, plan, preparation, bindings) ||
       !require_root_package_attribution(bindings, plan, preparation) ||
       !validate_build_unit_selection(
           plan, build_unit_selection, preparation)) {
        return {std::move(preparation), {}};
    }

    std::vector<UpdateWorkItemDraft> drafts;
    drafts.reserve(plan.order.size());
    if(!collect_work_item_drafts(
           plan, build_unit_selection, bindings, preparation, drafts,
           needed)) {
        return {std::move(preparation), {}};
    }

    apply_saved_source_preference_policy(
        saved_source_preference_policy, drafts, preparation);
    if(!preparation.issues.empty()) {
        return {std::move(preparation), {}};
    }

    validate_static_work_items(drafts, preparation);
    if(!preparation.issues.empty()) {
        return {std::move(preparation), {}};
    }
    return {std::move(preparation), std::move(drafts)};
}

void retain_generic_preparation_failure(
    AurUpdateSourceBuildPreparation& preparation,
    AurUpdatePreparationIssue issue) {
    attribute_issue_to_all_executable_targets(issue, preparation);
    preparation.issues.push_back(std::move(issue));
}

AurUpdateSourceBuildObservation make_source_build_observation(
    AurUpdateSourceBuildPreparation preparation,
    std::vector<AurUpdatePreparedWorkItemAttribution> attributions = {},
    std::optional<ProductionSourceBuildPreparationObservation>
        production_preflight = std::nullopt) {
    return AurUpdateSourceBuildObservation{
        std::move(preparation.issues),
        std::move(preparation.warnings),
        std::move(preparation.affected_update_targets),
        std::move(preparation.affected_roots),
        std::move(preparation.build_unit_selection),
        std::move(preparation.projected_build_units),
        std::move(preparation.externally_satisfied_build_units),
        std::move(attributions),
        std::move(production_preflight),
        preparation.devel_requires_check_policy};
}

} // namespace

AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config) {
    AurUpdateSourceBuildDraftPreparation draft_preparation =
        prepare_aur_update_source_build_drafts(
            preflight, build_unit_selection,
            devel_requires_check_policy,
            saved_source_preference_policy, needed);
    AurUpdateSourceBuildPreparation preparation =
        std::move(draft_preparation.observation);
    std::vector<UpdateWorkItemDraft> drafts =
        std::move(draft_preparation.drafts);
    if(!preparation.issues.empty() || drafts.empty()) {
        return preparation;
    }

    try {
        // POLICY(#267): generic preparationをinvocation全体で一度だけ呼び、
        // PacmanDatabasePathsを全work itemで共有するowned snapshotにする。
        std::vector<AurUpdatePreparedWorkItemAttribution> attributions =
            snapshot_work_item_attributions(drafts);
        PreparedProductionSourceBuildInvocation production_invocation =
            prepare_production_source_build_invocation(
                release_work_items(std::move(drafts)), config);
        if(!has_exact_prepared_correlation(
               production_invocation.work_items, attributions,
               preparation)) {
            throw std::logic_error(localization::format_translated_message(
                // TRANSLATORS: {} is the literal service name "AUR".
                "Prepared {} update source-build invocation correlation is inconsistent.",
                AUR_SERVICE_NAME));
        }

        // POLICY(#267): generic invocationとattributionの両方が完成してから、
        // 相関済みaggregateを一度だけpublishする。
        PreparedAurUpdateSourceBuildInvocation prepared_invocation(
            std::move(production_invocation), std::move(attributions));
        return AurUpdateSourceBuildPreparation{
            std::move(preparation.issues),
            std::move(preparation.warnings),
            std::move(preparation.affected_update_targets),
            std::move(preparation.affected_roots),
            std::move(preparation.build_unit_selection),
            std::move(preparation.projected_build_units),
            std::move(preparation.externally_satisfied_build_units),
            std::optional<PreparedAurUpdateSourceBuildInvocation>{
                std::move(prepared_invocation)},
            preparation.devel_requires_check_policy};
    } catch(const ReviewedSourceProductionError& error) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::GenericPreparationInconsistent,
            error.what());
        issue.reviewed_source_failure = error.failure();
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
    } catch(const PackageMetadataError& error) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::PacmanDatabaseUnavailable,
            error.failure().diagnostic);
        issue.package_metadata_failure = error.failure();
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
    } catch(const std::exception& error) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::GenericPreparationInconsistent,
            localization::format_translated_message(
                "Generic production source-build preparation failed unexpectedly: {}",
                error.what()));
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
    } catch(...) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::GenericPreparationInconsistent,
            "Generic production source-build preparation failed with an unknown exception.");
        attribute_issue_to_all_executable_targets(issue, preparation);
        preparation.issues.push_back(std::move(issue));
    }
    return preparation;
}

AurUpdateSourceBuildObservation observe_aur_update_source_build_preparation(
    const AurUpdateExecutionPreflight& preflight,
    const AurUpdateBuildUnitSelection& build_unit_selection,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config) {
    AurUpdateSourceBuildDraftPreparation draft_preparation =
        prepare_aur_update_source_build_drafts(
            preflight, build_unit_selection,
            devel_requires_check_policy,
            saved_source_preference_policy, needed);
    AurUpdateSourceBuildPreparation preparation =
        std::move(draft_preparation.observation);
    if(!preparation.issues.empty() ||
       draft_preparation.drafts.empty()) {
        return make_source_build_observation(
            std::move(preparation));
    }

    try {
        std::vector<AurUpdatePreparedWorkItemAttribution> attributions =
            snapshot_work_item_attributions(draft_preparation.drafts);
        ProductionSourceBuildPreparationObservation production_preflight =
            observe_production_source_build_preparation(
                release_work_items(
                    std::move(draft_preparation.drafts)),
                config);
        if(!has_exact_prepared_correlation(
               production_preflight.work_items, attributions,
               preparation)) {
            throw std::logic_error(
                localization::format_translated_message(
                    // TRANSLATORS: {} is the literal service name "AUR".
                    "Observed {} update source-build preflight correlation is inconsistent.",
                    AUR_SERVICE_NAME));
        }
        return make_source_build_observation(
            std::move(preparation), std::move(attributions),
            std::move(production_preflight));
    } catch(const ReviewedSourceProductionError& error) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::GenericPreparationInconsistent,
            error.what());
        issue.reviewed_source_failure = error.failure();
        retain_generic_preparation_failure(
            preparation, std::move(issue));
    } catch(const PackageMetadataError& error) {
        AurUpdatePreparationIssue issue = make_localized_preparation_issue(
            AurUpdatePreparationReason::PacmanDatabaseUnavailable,
            error.failure().diagnostic);
        issue.package_metadata_failure = error.failure();
        retain_generic_preparation_failure(
            preparation, std::move(issue));
    } catch(const std::exception& error) {
        retain_generic_preparation_failure(
            preparation,
            make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    GenericPreparationInconsistent,
                localization::format_translated_message(
                    "Generic production source-build observation failed unexpectedly: {}",
                    error.what())));
    } catch(...) {
        retain_generic_preparation_failure(
            preparation,
            make_localized_preparation_issue(
                AurUpdatePreparationReason::
                    GenericPreparationInconsistent,
                "Generic production source-build observation failed with an unknown exception."));
    }
    return make_source_build_observation(std::move(preparation));
}

AurUpdateSourceBuildPreparation prepare_aur_update_source_build_invocation(
    const AurUpdateExecutionPreflight& preflight,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config) {
    const AurUpdateBuildUnitSelection build_unit_selection =
        make_all_selected_build_unit_selection(preflight);
    return prepare_aur_update_source_build_invocation(
        preflight, build_unit_selection, devel_requires_check_policy,
        saved_source_preference_policy, needed, config);
}

AurUpdateSourceBuildObservation observe_aur_update_source_build_preparation(
    const AurUpdateExecutionPreflight& preflight,
    DevelRequiresCheckPolicy devel_requires_check_policy,
    SavedSourcePreferencePolicy saved_source_preference_policy,
    bool needed,
    const AppConfig& config) {
    const AurUpdateBuildUnitSelection build_unit_selection =
        make_all_selected_build_unit_selection(preflight);
    return observe_aur_update_source_build_preparation(
        preflight, build_unit_selection, devel_requires_check_policy,
        saved_source_preference_policy, needed, config);
}
