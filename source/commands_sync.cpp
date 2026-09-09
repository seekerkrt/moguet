#include "commands_sync.hpp"

#include "app_config.hpp"
#include "aur_rpc.hpp"
#include "aur_update_cli_presentation.hpp"
#include "cli_routing.hpp"
#include "commands_aur_update.hpp"
#include "dependency_plan.hpp"
#include "diagnostic_projection.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "repository_query.hpp"
#include "root_package_route_projection.hpp"
#include "root_package_search.hpp"
#include "root_package_selection.hpp"
#include "runtime_diagnostic.hpp"
#include "shell_words.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"
#include "system_aur_update_operation.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// sync search / info / installのselector別policyと、専用presentation / command constructionを所有する。
// POLICY: runnerのconfig ownerとtop-level catchは維持し、source_installへCLI policyを逆流させない。
namespace {

std::vector<std::string> pacman_args_with_global_options(
    std::vector<std::string> args, const AppConfig& config) {
    if(config.no_confirm) {
        // POLICY(#173): generated optionはoperation直後へ置き、semantic `--`やoption valueを再解釈しない。
        // 認識済みglobal tokenはordered viewから除外済みなので、ここでは常に1件だけ生成する。
        if(args.empty())
            args.push_back("--noconfirm");
        else
            args.insert(args.begin() + 1, "--noconfirm");
    }
    return args;
}

std::string join_pacman_args(
    const std::vector<std::string>& args, const AppConfig& config) {
    return shell_words::join(pacman_args_with_global_options(args, config));
}

void preflight_aur_search_schema(const std::vector<std::string>& keywords) {
    // POLICY(#174): refresh付きsearchはAUR responseのschemaをDB mutationより先に検証する。
    for(const auto& keyword : keywords) {
        if(keyword.empty() || keyword[0] == '-') continue;
        try {
            static_cast<void>(AurClient::search(keyword));
        } catch(const AurRpcResponseError&) {
            throw;
        } catch(const std::exception&) {
            // transport/その他の既存search契約は、pacman実行後の通常search phaseへ委ねる。
        }
    }
}

bool is_orphaned(const AurPackageInfo& pkg) {
    return pkg.Maintainer.empty();
}

bool search_aur(
    const std::vector<std::string>& keywords, bool query_installed_state = true) {
    bool found = false;
    // POLICY(#168): AurOnly search must not invoke pacman, even for the optional [installed] annotation.
    std::set<std::string> installed_foreign_packages =
        query_installed_state ? get_foreign_package_names() : std::set<std::string>{};
    for(const auto& pkg_name : keywords) {
        if(pkg_name.empty()) continue;
        if(pkg_name[0] == '-') continue;
        for(const auto& info : AurClient::search(pkg_name)) {
            found = true;
            const std::string& name = info.Name;
            // NO_TRANSLATE(Issue #308): "aur/" is the stable repository
            // namespace prefix; name and version are package identities.
            std::cout << "\033[1;35maur\033[0m/\033[1m" << name << "\033[0m \033[1;32m"
                      << info.Version << "\033[0m";
            if(installed_foreign_packages.contains(name)) {
                std::cout << " \033[1;36m"
                          << localization::translate_message("[installed]")
                          << "\033[0m";
            }
            if(info.OutOfDate.has_value()) {
                std::cout << " \033[1;31m"
                          << localization::translate_message("[out-of-date]")
                          << "\033[0m";
            }
            if(is_orphaned(info)) {
                std::cout << " \033[1;33m"
                          << localization::translate_message("[orphaned]")
                          << "\033[0m";
            }
            std::cout << std::endl;
            if(!info.Description.empty()) std::cout << "    " << info.Description << std::endl;
        }
    }
    return found;
}

std::string join_display_values(const std::vector<std::string>& values) {
    if(values.empty()) return localization::translate_message("None");
    std::stringstream ss;
    for(size_t i = 0; i < values.size(); ++i) {
        if(i > 0) ss << "  ";
        ss << values[i];
    }
    return ss.str();
}

std::string installed_display(const AurPackageInfo& pkg) {
    if(!is_installed_package(pkg.Name)) {
        return localization::translate_message("no");
    }
    return "\033[1;36m" + localization::translate_message("yes") +
           "\033[0m";
}

std::string orphaned_display(const AurPackageInfo& pkg) {
    if(!is_orphaned(pkg)) return localization::translate_message("no");
    return "\033[1;33m" + localization::translate_message("yes") +
           "\033[0m";
}

std::string out_of_date_display(const std::optional<long long>& out_of_date) {
    if(!out_of_date.has_value()) return localization::translate_message("no");
    return "\033[1;31m" + localization::translate_message("yes") +
           "\033[0m";
}

void print_aur_info(const AurPackageInfo& pkg) {
    std::cout << localization::format_translated_message(
                     "Repository      : {}", "aur")
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Name            : {}", pkg.Name)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Package Base    : {}", pkg.PackageBase)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Version         : {}", pkg.Version)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Description     : {}",
                     pkg.Description.empty()
                         ? localization::translate_message("None")
                         : pkg.Description)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Depends On      : {}",
                     join_display_values(pkg.Depends))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Make Deps       : {}",
                     join_display_values(pkg.MakeDepends))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Check Deps      : {}",
                     join_display_values(pkg.CheckDepends))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Optional Deps   : {}",
                     join_display_values(pkg.OptDepends))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Provides        : {}",
                     join_display_values(pkg.Provides))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Conflicts With  : {}",
                     join_display_values(pkg.Conflicts))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Replaces        : {}",
                     join_display_values(pkg.Replaces))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Relation Check  : {}",
                     localization::translate_message(
                         "deferred to planning and build preflight"))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Maintainer      : {}",
                     pkg.Maintainer.empty()
                         ? localization::translate_message("None")
                         : pkg.Maintainer)
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Installed       : {}", installed_display(pkg))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Orphaned        : {}", orphaned_display(pkg))
              << std::endl;
    std::cout << localization::format_translated_message(
                     "Out of Date     : {}",
                     out_of_date_display(pkg.OutOfDate))
              << std::endl;
}

void require_valid_aur_package_target(const std::string& target) {
    if(target.find('/') != std::string::npos || !is_valid_package_name(target)) {
        throw std::runtime_error(
            localization::format_translated_message(
                // TRANSLATORS: The placeholders are the literal AUR identity and a package target.
                "Invalid {} package target: {}", "AUR", target));
    }
}

void append_source_build_work_items(
    std::vector<ProductionSourceBuildWorkItem>& destination,
    std::vector<ProductionSourceBuildWorkItem> source) {
    destination.reserve(destination.size() + source.size());
    for(auto& work_item : source) {
        destination.push_back(std::move(work_item));
    }
}

std::size_t earliest_root_index_for_source_build_work_item(
    const BuildPlan& plan,
    const ProductionSourceBuildWorkItem& work_item) {
    std::optional<std::size_t> earliest_root;
    for(const auto& target : plan.package_targets) {
        if(target.package_base != work_item.request.checkout_name) continue;
        for(const auto& root : target.roots) {
            if(!earliest_root.has_value() ||
               root.invocation_index < earliest_root.value()) {
                earliest_root = root.invocation_index;
            }
        }
    }
    if(!earliest_root.has_value() ||
       earliest_root.value() >= plan.root_targets.size()) {
        throw std::logic_error(localization::format_translated_message(
            "{} work item has no invocation root ownership: {}",
            "BuildPlan", work_item.request.checkout_name));
    }
    return earliest_root.value();
}

int execute_sync_source_build_invocation(
    PreparedProductionSourceBuildInvocation invocation,
    const AppConfig& config) {
    try {
        const auto result = execute_prepared_source_build_invocation(
            std::move(invocation), config);
        return result.command_exit_status();
    } catch(const ProductionSourceBuildInvocationError& error) {
        Logger::error(
            format_production_source_build_invocation_failure(error));
        return 1;
    } catch(const SeparatedPackageBaseSourceBuildCleanupError& error) {
        // Direct source routeは利用者がretained workspaceを手動確認できる
        // 既存contractを維持する。AUR update resultのpath firewallとは別境界。
        Logger::error(error.what());
        return 1;
    }
}

struct PendingAurSyncRoot {
    std::size_t target_index = 0;
    RootTargetIdentity invocation_correlation;
    std::optional<RepositoryPackageNotFound> repository_absence;
    std::size_t aur_input_index = 0;
};

struct PendingRepositorySourceWork {
    std::size_t target_index = 0;
    RepositoryPackagePresent package;
    ResolvedSourceBuildIdentity source;
    SourcePreferenceLoaded preference;
};

struct PendingAurSourceWork {
    std::size_t aur_input_index = 0;
};

using PendingSyncSourceWork = std::variant<
    PendingRepositorySourceWork,
    PendingAurSourceWork>;

std::vector<SyncInstallRoot> take_observed_sync_roots(
    std::vector<std::optional<SyncInstallRoot>>& root_slots) {
    std::vector<SyncInstallRoot> roots;
    roots.reserve(root_slots.size());
    for(auto& root : root_slots) {
        if(root.has_value()) roots.push_back(std::move(root.value()));
    }
    return roots;
}

SyncInstallPreparationIssue make_sync_install_issue(
    SyncInstallPreparationIssueKind kind,
    std::string diagnostic,
    std::optional<RootTargetIdentity> root = std::nullopt,
    std::optional<std::string> option = std::nullopt) {
    return SyncInstallPreparationIssue{
        kind, std::move(root), std::move(option),
        std::move(diagnostic), std::nullopt};
}

void present_loaded_source_preference(
    const SourcePreferenceLoaded& preference) {
    Logger::info(localization::format_translated_message(
        // TRANSLATORS: The placeholder is a source preference file path.
        "Loading custom build flags from {}.",
        preference.entry_path.string()));
    for(const std::string& warning : preference.warnings) {
        Logger::warn(warning);
    }
}

std::optional<std::size_t> build_plan_root_index(
    const BuildPlan& plan,
    std::size_t aur_input_index,
    const std::string& requested_name) {
    std::optional<std::size_t> matched_index;
    for(std::size_t index = 0; index < plan.root_targets.size(); ++index) {
        const RootTargetIdentity& candidate = plan.root_targets[index];
        if(candidate.invocation_index != aur_input_index ||
           candidate.requested_name != requested_name) {
            continue;
        }
        if(matched_index.has_value()) return std::nullopt;
        matched_index = index;
    }
    return matched_index;
}

void report_sync_install_preparation_failure(
    const SyncInstallPreparationFailure& failure) {
    for(const SyncInstallPreparationFailureDetail& detail : failure.details) {
        std::visit(
            [](const auto& typed_detail) {
                using Detail = std::decay_t<decltype(typed_detail)>;
                if constexpr(std::is_same_v<
                                 Detail,
                                 SyncInstallPreparationIssue>) {
                    const auto diagnostic =
                        project_sync_install_diagnostic(typed_detail);
                    if(typed_detail.kind ==
                       SyncInstallPreparationIssueKind::
                           UnsupportedSourceOption) {
                        // The compound diagnostic remains one typed
                        // projection payload. Actual CLI compatibility
                        // requires separate error output and log records.
                        const std::size_t separator =
                            typed_detail.diagnostic.find('\n');
                        if(separator != std::string::npos) {
                            report_runtime_diagnostic(
                                diagnostic,
                                typed_detail.diagnostic.substr(
                                    0, separator));
                            report_runtime_diagnostic(
                                diagnostic,
                                typed_detail.diagnostic.substr(
                                    separator + 1));
                            return;
                        }
                    }
                    report_runtime_diagnostic(
                        diagnostic, typed_detail.diagnostic);
                } else if constexpr(std::is_same_v<
                                        Detail,
                                        SyncRepositoryMetadataReadFailure>) {
                    report_runtime_diagnostic(
                        project_sync_install_diagnostic(typed_detail),
                        typed_detail.failure.diagnostic);
                } else {
                    Logger::error(typed_detail.diagnostic);
                }
            },
            detail);
    }
}

std::string root_package_presentation_value(
    const std::optional<std::string>& value) {
    return value.has_value() ? value.value() : "-";
}

void present_root_package_candidate(
    std::ostream& output,
    std::size_t index,
    const RootPackageSearchCandidate& candidate) {
    // NO_TRANSLATE: source/repository/package/PackageBase/version/groups are
    // stable machine-readable candidate field labels.
    output << index << ") ";
    if(const auto* repository =
           std::get_if<RepositoryRootPackageIdentity>(
               &candidate.candidate.identity());
       repository != nullptr) {
        output << "source=repository"
               << " repository=" << repository->repository_name
               << " package=" << repository->package_name;
    } else {
        const auto& aur = std::get<AurRootPackageIdentity>(
            candidate.candidate.identity());
        output << "source=AUR"
               << " package=" << aur.package_name
               << " PackageBase=" << aur.package_base;
    }
    output << " version="
           << root_package_presentation_value(
                  candidate.candidate.presentation().version);
    if(!candidate.selectable_group_names.empty()) {
        output << " groups=";
        for(std::size_t group_index = 0;
            group_index < candidate.selectable_group_names.size();
            ++group_index) {
            if(group_index > 0) output << ',';
            output << '@' << candidate.selectable_group_names[group_index];
        }
    }
    output << '\n';
    if(candidate.candidate.presentation().description.has_value()) {
        output << "    "
               << candidate.candidate.presentation().description.value()
               << '\n';
    }
}

std::string root_package_selection_issue_message(
    const RootPackageSelectionIssue& issue) {
    return std::visit(
        [](const auto& typed_issue) -> std::string {
            using Issue = std::decay_t<decltype(typed_issue)>;
            if constexpr(std::is_same_v<
                             Issue,
                             MalformedRootPackageSelectionToken>) {
                return localization::translate_message(
                    "Invalid package selection token.");
            } else if constexpr(std::is_same_v<
                                    Issue,
                                    RootPackageSelectionIndexOutOfRange>) {
                // TRANSLATORS: The placeholder is the number of displayed package candidates.
                return localization::format_translated_message(
                    "Package selection index is outside the displayed range 1-{}.",
                    typed_issue.candidate_count);
            } else if constexpr(std::is_same_v<
                                    Issue,
                                    DescendingRootPackageSelectionRange>) {
                return localization::translate_message(
                    "Package selection ranges must use ascending endpoints.");
            } else if constexpr(std::is_same_v<
                                    Issue,
                                    UnknownRootPackageSelectionGroup>) {
                return localization::translate_message(
                    "Package selection names an unknown displayed group.");
            } else if constexpr(std::is_same_v<
                                    Issue,
                                    MixedRootPackageSelectionCancellationToken>) {
                return localization::translate_message(
                    "A package selection cancellation token cannot be combined with selectors.");
            } else if constexpr(std::is_same_v<
                                    Issue,
                                    ConflictingRootPackageSelectionAlternatives>) {
                // package_name comes from the validated candidate snapshot; raw input tokens are not echoed.
                // TRANSLATORS: The placeholder is a validated package name.
                return localization::format_translated_message(
                    "Package {} was selected from more than one source; select exactly one source.",
                    typed_issue.package_name);
            }
            // NO_TRANSLATE: Exhaustiveness guard for a typed variant;
            // never presented as a recoverable user-facing diagnostic.
            throw std::logic_error(
                "Unknown root package selection validation issue.");
        },
        issue);
}

RootPackageSelectionInteractionCallback
root_package_selection_interaction() {
    return [](const RootPackageSelectionInteractionEvent& event,
              const RootPackageSearchSnapshot& snapshot) {
        if(std::holds_alternative<
               PresentRootPackageSelectionCandidates>(event)) {
            std::cout << ":: "
                      << localization::translate_message(
                             "Package candidates:")
                      << '\n';
            for(std::size_t index = 0; index < snapshot.candidates.size();
                ++index) {
                present_root_package_candidate(
                    std::cout, index + 1, snapshot.candidates[index]);
            }
            return;
        }
        if(std::holds_alternative<PromptForRootPackageSelection>(event)) {
            // TRANSLATORS: Enter and q/quit/cancel are literal input tokens; @group is fixed selector syntax.
            std::cout << ":: "
                      << localization::format_translated_message(
                             "Select package numbers, ascending ranges, or displayed {}; press {} or enter {} to cancel:",
                             "@group", "Enter", "q/quit/cancel")
                      << ' ' << std::flush;
            return;
        }

        const auto& invalid =
            std::get<InvalidRootPackageSelectionAttempt>(event);
        const auto diagnostics = project_root_selection_diagnostics(
            invalid.selection);
        for(std::size_t index = 0; index < diagnostics.size(); ++index) {
            const RuntimeDiagnosticPresentation presentation =
                present_runtime_diagnostic(
                    diagnostics[index],
                    root_package_selection_issue_message(
                        diagnostics[index].reason));
            // Interactive retry feedback remains on the prompt stream. The
            // typed reporter still owns classification and identity formatting.
            std::cout << ":: " << presentation.message << '\n';
        }
    };
}

void report_root_package_selection_input_gate(
    RootPackageSelectionInputGate input_gate) {
    RootPackageSelectionUnavailableReason reason =
        RootPackageSelectionUnavailableReason::NonInteractiveInput;
    std::string message;
    switch(input_gate) {
        case RootPackageSelectionInputGate::NonTty:
            reason = RootPackageSelectionUnavailableReason::NonInteractiveInput;
            message = localization::translate_message(
                "Interactive package selection requires a TTY on standard input.");
            break;
        case RootPackageSelectionInputGate::NoConfirm:
            // TRANSLATORS: The placeholder is the literal CLI option --noconfirm.
            reason = RootPackageSelectionUnavailableReason::NoConfirm;
            message = localization::format_translated_message(
                "Interactive package selection is not available with {}.",
                "--noconfirm");
            break;
        case RootPackageSelectionInputGate::Interactive:
            throw std::logic_error(localization::translate_message(
                "Interactive package selection has an inconsistent input gate."));
    }
    report_runtime_diagnostic(
        project_root_selection_diagnostic(
            UnavailableRootPackageSelection{reason}),
        message);
}

RootPackageSearchScope root_package_search_scope(
    PackageSourceSelection source_selection) {
    switch(source_selection) {
        case PackageSourceSelection::Auto:
            return RootPackageSearchScope::All;
        case PackageSourceSelection::AurOnly:
            return RootPackageSearchScope::Aur;
        case PackageSourceSelection::RepoOnly:
            return RootPackageSearchScope::Repository;
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package source selection."));
}

void report_root_package_search_failure(
    const RootPackageSearchResult& result) {
    if(const auto* repository =
           std::get_if<RepositoryRootPackageSearchFailure>(&result);
       repository != nullptr) {
        // TRANSLATORS: The placeholder is a repository metadata diagnostic.
        Logger::error(localization::format_translated_message(
            "Repository package search failed: {}",
            repository->failure.diagnostic));
        return;
    }
    if(const auto* aur = std::get_if<AurRootPackageSearchFailure>(&result);
       aur != nullptr) {
        // TRANSLATORS: The placeholders are the AUR project identity and a query diagnostic.
        Logger::error(localization::format_translated_message(
            "{} package search failed: {}", "AUR", aur->diagnostic));
        return;
    }
    if(std::holds_alternative<InvalidRootPackageSearchSnapshot>(result)) {
        // Raw invalid candidate metadata is intentionally not reflected to the terminal.
        Logger::error(localization::translate_message(
            "Package search returned an invalid candidate snapshot."));
        return;
    }
    throw std::logic_error(localization::translate_message(
        "Package search failure reporting received a successful snapshot."));
}

bool has_source_build_cli_override(const ParsedCliArguments& parsed) noexcept {
    return parsed.cli_overrides.review_pkgbuild.has_value() ||
           parsed.cli_overrides.review_diff.has_value() ||
           parsed.cli_overrides.build_mode.has_value();
}

std::string join_root_package_names(
    const std::vector<AurRootPackageRouteTarget>& targets) {
    std::stringstream joined;
    for(std::size_t index = 0; index < targets.size(); ++index) {
        if(index > 0) joined << ", ";
        joined << targets[index].identity().package_name;
    }
    return joined.str();
}

std::string join_package_names(
    const std::vector<std::string>& package_names) {
    std::stringstream joined;
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        if(index > 0) joined << ", ";
        joined << package_names[index];
    }
    return joined.str();
}

void require_selected_aur_root_plan_correlation(
    const std::vector<AurRootPackageRouteTarget>& selected_targets,
    const BuildPlan& plan) {
    if(plan.root_targets.size() != selected_targets.size()) {
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "The {} build plan does not cover every selected root package.",
            "AUR"));
    }

    for(std::size_t index = 0; index < selected_targets.size(); ++index) {
        const AurRootPackageIdentity& selected =
            selected_targets[index].identity();
        const RootTargetIdentity& planned_root = plan.root_targets[index];
        if(planned_root.invocation_index != index ||
           planned_root.requested_name != selected.package_name) {
            throw std::runtime_error(localization::format_translated_message(
                // TRANSLATORS: The placeholder is the AUR project identity.
                "The {} build plan changed the selected root package order or identity.",
                "AUR"));
        }

        const PlannedPackageTarget* planned_target = nullptr;
        for(const auto& candidate : plan.package_targets) {
            if(candidate.package_name != selected.package_name) continue;
            if(planned_target != nullptr) {
                throw std::runtime_error(localization::format_translated_message(
                    // TRANSLATORS: The placeholders are the AUR project identity and a validated package name.
                    "The {} build plan contains duplicate identities for selected package {}.",
                    "AUR", selected.package_name));
            }
            planned_target = &candidate;
        }
        if(planned_target == nullptr) {
            throw std::runtime_error(localization::format_translated_message(
                // TRANSLATORS: The placeholders are the AUR project identity and a validated package name.
                "The {} build plan omitted selected package {}.",
                "AUR", selected.package_name));
        }
        if(planned_target->package_base != selected.package_base) {
            // Both PackageBase values are validated identities. Refuse to route a stale
            // selection to metadata returned by the later build-plan query.
            // TRANSLATORS: The placeholders are the PackageBase and AUR identities, a validated package name, and two PackageBase names.
            throw std::runtime_error(localization::format_translated_message(
                "The {} for selected {} package {} changed from {} to {}; rerun package selection.",
                "PackageBase", "AUR", selected.package_name,
                selected.package_base, planned_target->package_base));
        }
        if(std::find(
               planned_target->roles.begin(),
               planned_target->roles.end(),
               PackageRole::Root) == planned_target->roles.end() ||
           std::find(
               planned_target->roots.begin(),
               planned_target->roots.end(),
               planned_root) == planned_target->roots.end()) {
            throw std::runtime_error(localization::format_translated_message(
                // TRANSLATORS: The placeholders are the AUR project identity and a validated package name.
                "The {} build plan lost root attribution for selected package {}.",
                "AUR", selected.package_name));
        }
    }
}

} // namespace

RootPackageInstallPreparation prepare_root_package_install(
    const ParsedCliArguments& parsed,
    RootPackageSelectionInvocation invocation,
    const AppConfig& config) {
    auto issue_failure = [](RootPackageInstallPreparationIssue issue) {
        RootPackageInstallPreparationFailure failure;
        failure.details.push_back(std::move(issue));
        return failure;
    };
    RootPackageSelectionSession selection_session =
        make_root_package_selection_session(
            root_package_selection_interaction(),
            config.no_confirm);
    if(invocation.query.empty()) {
        const std::string diagnostic = localization::translate_message(
            "Package selection query must not be empty.");
        Logger::error(diagnostic);
        return issue_failure(RootPackageInstallPreparationIssue{
            RootPackageInstallPreparationIssueKind::EmptyQuery,
            std::nullopt, std::nullopt, std::nullopt, diagnostic,
            std::nullopt});
    }
    if(config.rm_deps || parsed.cli_overrides.rm_deps) {
        // TRANSLATORS: The placeholder is the literal CLI option --rmdeps.
        const std::string diagnostic = localization::format_translated_message(
            "Interactive package selection does not support {}.",
            "--rmdeps");
        Logger::error(diagnostic);
        return issue_failure(RootPackageInstallPreparationIssue{
            RootPackageInstallPreparationIssueKind::
                RemoveDependenciesUnsupported,
            std::nullopt, std::nullopt, std::nullopt, diagnostic,
            std::nullopt});
    }

    // POLICY(#217): gate must be observable before official/AUR candidate query.
    if(!selection_session.is_interactive()) {
        const RootPackageSelectionInputGate input_gate =
            selection_session.input_gate();
        report_root_package_selection_input_gate(
            input_gate);
        return issue_failure(RootPackageInstallPreparationIssue{
            RootPackageInstallPreparationIssueKind::InputGateUnavailable,
            input_gate,
            std::nullopt,
            std::nullopt,
            {},
            std::nullopt});
    }

    RootPackageSearchResult search_result = search_root_package_candidates(
        invocation.query,
        root_package_search_scope(parsed.source_selection));
    const auto* snapshot =
        std::get_if<RootPackageSearchSnapshot>(&search_result);
    if(snapshot == nullptr) {
        report_root_package_search_failure(search_result);
        RootPackageInstallPreparationFailure failure;
        std::visit(
            [&failure](auto&& detail) {
                using Detail = std::decay_t<decltype(detail)>;
                if constexpr(!std::is_same_v<
                                 Detail,
                                 RootPackageSearchSnapshot>) {
                    failure.details.push_back(
                        std::forward<decltype(detail)>(detail));
                }
            },
            std::move(search_result));
        return failure;
    }

    RootPackageSelectionSessionResult selection_result =
        selection_session.select(*snapshot);
    if(const auto* unavailable =
           std::get_if<UnavailableRootPackageSelection>(
               &selection_result);
       unavailable != nullptr) {
        switch(unavailable->reason) {
            case RootPackageSelectionUnavailableReason::NoCandidates: {
                const std::string diagnostic = localization::translate_message(
                    "No package candidates were found.");
                report_runtime_diagnostic(
                    project_root_selection_diagnostic(*unavailable),
                    diagnostic);
                RootPackageInstallPreparationFailure failure = issue_failure(
                    RootPackageInstallPreparationIssue{
                        RootPackageInstallPreparationIssueKind::
                            SelectionUnavailable,
                        std::nullopt, unavailable->reason, std::nullopt,
                        diagnostic, std::nullopt});
                failure.discovery_snapshot.emplace(std::move(*snapshot));
                return failure;
            }
            case RootPackageSelectionUnavailableReason::NonInteractiveInput:
                report_root_package_selection_input_gate(
                    RootPackageSelectionInputGate::NonTty);
                break;
            case RootPackageSelectionUnavailableReason::NoConfirm:
                report_root_package_selection_input_gate(
                    RootPackageSelectionInputGate::NoConfirm);
                break;
            default:
                throw std::logic_error(localization::translate_message(
                    "Package selection returned an unknown unavailable reason."));
        }
        RootPackageInstallPreparationFailure failure = issue_failure(
            RootPackageInstallPreparationIssue{
                RootPackageInstallPreparationIssueKind::
                    SelectionUnavailable,
                unavailable->reason ==
                        RootPackageSelectionUnavailableReason::
                            NonInteractiveInput
                    ? std::optional<RootPackageSelectionInputGate>{
                          RootPackageSelectionInputGate::
                              NonTty}
                    : std::optional<RootPackageSelectionInputGate>{RootPackageSelectionInputGate::NoConfirm},
                unavailable->reason,
                std::nullopt,
                {},
                std::nullopt});
        failure.discovery_snapshot.emplace(std::move(*snapshot));
        return failure;
    }
    if(const auto* cancelled =
           std::get_if<CancelledRootPackageSelection>(
               &selection_result);
       cancelled != nullptr) {
        const std::string diagnostic = localization::translate_message(
            "Package selection was cancelled.");
        report_runtime_diagnostic(
            project_root_selection_diagnostic(*cancelled),
            diagnostic);
        RootPackageInstallPreparationFailure failure = issue_failure(
            RootPackageInstallPreparationIssue{
                RootPackageInstallPreparationIssueKind::
                    SelectionCancelled,
                std::nullopt, std::nullopt, cancelled->reason,
                diagnostic, std::nullopt});
        failure.discovery_snapshot.emplace(std::move(*snapshot));
        return failure;
    }

    const RootPackageSelection& selection =
        std::get<RootPackageSelection>(selection_result);
    RootPackageRoutingProjectionResult routing =
        project_root_package_routing(selection);
    if(!routing.is_valid()) {
        // Unsafe repository identity is deliberately not echoed.
        const std::string diagnostic = localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal pacman program identity.
            "A selected repository package cannot be represented as an exact {} target.",
            "pacman");
        Logger::error(diagnostic);
        RootPackageInstallPreparationFailure failure;
        failure.details.push_back(*routing.failure());
        failure.discovery_snapshot.emplace(std::move(*snapshot));
        return failure;
    }
    const RootPackageRoutingProjection& projection = *routing.projection();

    if(projection.aur_targets().empty() &&
       has_source_build_cli_override(parsed)) {
        const std::string diagnostic = localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Source-build review and build-mode options require at least one selected {} package.",
            "AUR");
        Logger::error(diagnostic);
        RootPackageInstallPreparationFailure failure = issue_failure(
            RootPackageInstallPreparationIssue{
                RootPackageInstallPreparationIssueKind::
                    SourceOptionsWithoutAurTarget,
                std::nullopt, std::nullopt, std::nullopt,
                diagnostic, std::nullopt});
        failure.discovery_snapshot.emplace(std::move(*snapshot));
        failure.routing_projection.emplace(projection);
        return failure;
    }

    PreparedRootPackageInstall prepared;
    prepared.needed = invocation.needed;
    prepared.exact_repository_targets.reserve(
        projection.repository_targets().size());
    for(const auto& target : projection.repository_targets()) {
        prepared.exact_repository_targets.push_back(
            target.exact_package_target());
    }
    prepared.discovery_snapshot.emplace(
        std::get<RootPackageSearchSnapshot>(std::move(search_result)));
    prepared.routing_projection.emplace(projection);

    if(projection.aur_targets().empty()) return prepared;

    // All AUR roots share one plan so same-PackageBase selected children become
    // one ordered work item. This preserves source-local selection order while
    // keeping dependency units before their consumers.
    std::vector<std::string> aur_package_names;
    aur_package_names.reserve(projection.aur_targets().size());
    for(const auto& target : projection.aur_targets()) {
        aur_package_names.push_back(target.identity().package_name);
    }

    BuildPlan plan;
    bool plan_resolved = false;
    try {
        require_supported_production_source_build_options(config);
        plan = resolve_build_plan(
            aur_package_names,
            provider_selection_callback(config));
        plan_resolved = true;
        require_selected_aur_root_plan_correlation(
            projection.aur_targets(), plan);
        require_executable_build_plan(
            join_root_package_names(projection.aur_targets()), plan);
    } catch(const std::exception& error) {
        Logger::error(error.what());
        RootPackageInstallPreparationFailure failure = issue_failure(
            RootPackageInstallPreparationIssue{
                RootPackageInstallPreparationIssueKind::
                    BuildPlanPreparationFailed,
                std::nullopt, std::nullopt, std::nullopt,
                error.what(), std::nullopt});
        failure.discovery_snapshot =
            std::move(prepared.discovery_snapshot);
        failure.routing_projection =
            std::move(prepared.routing_projection);
        if(plan_resolved) failure.aur_build_plan.emplace(std::move(plan));
        return failure;
    }

    try {
        std::vector<ProductionSourceBuildWorkItem> work_items =
            prepare_aur_source_build_work_items(
                plan, false, prepared.needed);
        // Do not prepare/seed a cache here. execute activates it only after
        // the selected repository transaction succeeds.
        prepared.source_invocation =
            prepare_production_source_build_invocation(
                std::move(work_items), config);
    } catch(const ReviewedSourceProductionError& error) {
        Logger::error(error.what());
        RootPackageInstallPreparationIssue issue{
            RootPackageInstallPreparationIssueKind::
                SourceWorkPreparationFailed,
            std::nullopt, std::nullopt, std::nullopt, error.what(),
            std::nullopt};
        issue.reviewed_source_failure = error.failure();
        RootPackageInstallPreparationFailure failure = issue_failure(
            std::move(issue));
        failure.discovery_snapshot =
            std::move(prepared.discovery_snapshot);
        failure.routing_projection =
            std::move(prepared.routing_projection);
        failure.aur_build_plan.emplace(std::move(plan));
        return failure;
    } catch(const std::exception& error) {
        Logger::error(error.what());
        RootPackageInstallPreparationFailure failure = issue_failure(
            RootPackageInstallPreparationIssue{
                RootPackageInstallPreparationIssueKind::
                    SourceWorkPreparationFailed,
                std::nullopt, std::nullopt, std::nullopt,
                error.what(), std::nullopt});
        failure.discovery_snapshot =
            std::move(prepared.discovery_snapshot);
        failure.routing_projection =
            std::move(prepared.routing_projection);
        failure.aur_build_plan.emplace(std::move(plan));
        return failure;
    }
    prepared.aur_build_plan.emplace(std::move(plan));
    return prepared;
}

int execute_prepared_root_package_install(
    PreparedRootPackageInstall prepared,
    const AppConfig& config) {
    const bool has_repository_transaction =
        !prepared.exact_repository_targets.empty();
    if(!has_repository_transaction && !prepared.source_invocation.has_value()) {
        throw std::invalid_argument(localization::translate_message(
            "Prepared package selection contains no install targets."));
    }

    if(has_repository_transaction) {
        std::vector<std::string> pacman_args{"-S"};
        if(prepared.needed) pacman_args.push_back("--needed");
        pacman_args.push_back("--");
        pacman_args.insert(
            pacman_args.end(),
            prepared.exact_repository_targets.begin(),
            prepared.exact_repository_targets.end());

        const int repository_status = run_command(
            "sudo pacman " + shell_words::join(pacman_args));
        if(repository_status != 0) {
            if(prepared.source_invocation.has_value()) {
                Logger::error(localization::format_translated_message(
                    // TRANSLATORS: The placeholder is the AUR project identity.
                    "The selected repository package transaction failed; selected {} packages were not executed.",
                    "AUR"));
            } else {
                Logger::error(localization::translate_message(
                    "The selected repository package transaction failed."));
            }
            return repository_status;
        }
    }

    if(!prepared.source_invocation.has_value()) return 0;

    try {
        const int source_status = execute_sync_source_build_invocation(
            prepared.source_invocation.value(), config);
        if(source_status != 0 && has_repository_transaction) {
            Logger::warn(localization::format_translated_message(
                // TRANSLATORS: The placeholder is the AUR project identity.
                "The repository package transaction completed before the {} route failed; it was not rolled back.",
                "AUR"));
        }
        return source_status;
    } catch(...) {
        if(has_repository_transaction) {
            Logger::warn(localization::format_translated_message(
                // TRANSLATORS: The placeholder is the AUR project identity.
                "The repository package transaction completed before the {} route failed; it was not rolled back.",
                "AUR"));
        }
        throw;
    }
}

int cmd_sync_search(
    const ParsedCliArguments& parsed, bool use_sudo,
    PackageSourceSelection source_selection, const AppConfig& config) {
    if(parsed.targets.empty()) {
        Logger::error(localization::translate_message("Missing search query."));
        return 1;
    }

    std::string pacman_prefix = use_sudo ? "sudo pacman " : "pacman ";
    switch(source_selection) {
        case PackageSourceSelection::Auto: {
            if(use_sudo) preflight_aur_search_schema(parsed.targets);
            int pacman_status =
                run_command(pacman_prefix + join_pacman_args(parsed.ordered_pacman_args, config));
            Logger::info(localization::format_translated_message(
                "Searching {}...", "AUR"));
            bool aur_found = search_aur(parsed.targets);
            return (pacman_status == 0 || aur_found) ? 0 : 1;
        }
        case PackageSourceSelection::AurOnly:
            if(parsed_has_semantic_pacman_option(parsed, "--needed")) {
                Logger::error(localization::format_translated_message(
                    "Unsupported {} option for {} search: {}",
                    "pacman", "AUR", "--needed"));
                return 1;
            }
            Logger::info(localization::format_translated_message(
                "Searching {}...", "AUR"));
            return search_aur(parsed.targets, false) ? 0 : 1;
        case PackageSourceSelection::RepoOnly:
            return run_command(
                pacman_prefix + join_pacman_args(parsed.ordered_pacman_args, config));
    }
    throw std::logic_error(localization::translate_message(
        "Unknown package source selection."));
}

SyncInstallPreparation prepare_sync_install(
    const ParsedCliArguments& parsed,
    bool system_update,
    PackageSourceSelection source_selection,
    const AppConfig& config) {
    const SourceSyncOptions source_sync_options = parse_source_sync_options(parsed);
    std::vector<std::optional<SyncInstallRoot>> root_slots(
        parsed.targets.size());
    std::optional<BuildPlan> aur_build_plan;

    auto fail = [&](SyncInstallPreparationFailureDetail detail) {
        SyncInstallPreparationFailure failure;
        failure.details.push_back(std::move(detail));
        failure.ordered_roots = take_observed_sync_roots(root_slots);
        failure.source_selection = source_selection;
        failure.system_update = system_update;
        failure.needed = source_sync_options.needed;
        if(aur_build_plan.has_value()) {
            failure.aur_build_plan.emplace(
                std::move(aur_build_plan.value()));
        }
        return SyncInstallPreparation{std::move(failure)};
    };

    if(source_selection == PackageSourceSelection::RepoOnly) {
        return fail(make_sync_install_issue(
            SyncInstallPreparationIssueKind::
                UnsupportedSourceSelection,
            localization::format_translated_message(
                "{} is not supported for operation {}.",
                "--repo", parsed.operation)));
    }
    if(source_selection == PackageSourceSelection::Auto &&
       parsed.targets.empty() && !system_update) {
        return fail(make_sync_install_issue(
            SyncInstallPreparationIssueKind::EmptyPreparedRoute,
            localization::translate_message(
                "Sync preflight requires a package target or a system update.")));
    }
    if(source_selection == PackageSourceSelection::AurOnly && system_update) {
        return fail(make_sync_install_issue(
            SyncInstallPreparationIssueKind::
                UnsupportedSourceSelection,
            localization::format_translated_message(
                "Cannot combine {} with {} refresh for operation {}.",
                "--aur", "pacman", parsed.operation)));
    }
    if(parsed.target_token_indices.size() != parsed.targets.size()) {
        return fail(make_sync_install_issue(
            SyncInstallPreparationIssueKind::TargetCorrelationFailed,
            localization::translate_message(
                "Sync target token correlation is incomplete.")));
    }

    if(source_selection == PackageSourceSelection::AurOnly) {
        if(parsed.targets.empty()) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::MissingAurTarget,
                localization::format_translated_message(
                    "Missing {} package target.", "AUR")));
        }
        for(std::size_t index = 0; index < parsed.targets.size(); ++index) {
            const RootTargetIdentity root{index, parsed.targets[index]};
            try {
                require_valid_aur_package_target(parsed.targets[index]);
            } catch(const std::exception& error) {
                return fail(make_sync_install_issue(
                    SyncInstallPreparationIssueKind::InvalidTarget,
                    error.what(), root));
            }
        }
    }

    if(source_selection == PackageSourceSelection::Auto) {
        for(std::size_t index = 0; index < parsed.targets.size(); ++index) {
            const RootTargetIdentity root{index, parsed.targets[index]};
            try {
                require_valid_package_name(parsed.targets[index]);
            } catch(const std::exception& error) {
                return fail(make_sync_install_issue(
                    SyncInstallPreparationIssueKind::InvalidTarget,
                    error.what(), root));
            }
        }
    }

    std::vector<PendingAurSyncRoot> pending_aur_roots;
    std::vector<std::string> aur_plan_targets;
    std::vector<PendingSyncSourceWork> pending_source_work;
    std::set<std::size_t> source_target_token_indices;
    bool has_repository_transaction_root = false;
    bool has_repository_order = false;
    std::optional<std::vector<std::string>> repository_order;

    auto retain_repository_order = [&](const std::optional<std::vector<std::string>>& observed) {
        if(!has_repository_order) {
            repository_order = observed;
            has_repository_order = true;
            return true;
        }
        return repository_order == observed;
    };

    if(source_selection == PackageSourceSelection::AurOnly) {
        pending_aur_roots.reserve(parsed.targets.size());
        aur_plan_targets = parsed.targets;
        pending_source_work.reserve(parsed.targets.size());
        for(std::size_t index = 0; index < parsed.targets.size(); ++index) {
            pending_aur_roots.push_back(PendingAurSyncRoot{
                index,
                RootTargetIdentity{index, parsed.targets[index]},
                std::nullopt,
                index});
            pending_source_work.push_back(PendingAurSourceWork{index});
            source_target_token_indices.insert(
                parsed.target_token_indices[index]);
        }
    } else {
        for(std::size_t index = 0; index < parsed.targets.size(); ++index) {
            const std::string& target = parsed.targets[index];
            const RootTargetIdentity root{index, target};

            StrictSourcePreferenceResult preference =
                read_source_preference_strict(target);
            if(const auto* preference_failure =
                   std::get_if<SourcePreferenceFailure>(&preference);
               preference_failure != nullptr) {
                return fail(*preference_failure);
            }
            std::optional<SourcePreferenceLoaded> loaded_preference;
            if(std::holds_alternative<SourcePreferenceLoaded>(preference)) {
                loaded_preference.emplace(std::get<SourcePreferenceLoaded>(
                    std::move(preference)));
            }

            StrictRepositoryPackageQueryResult repository_result =
                query_repository_package_strict(target);
            if(const auto* metadata_failure =
                   std::get_if<RepositoryMetadataFailure>(
                       &repository_result);
               metadata_failure != nullptr) {
                return fail(SyncRepositoryMetadataReadFailure{
                    root, *metadata_failure});
            }

            if(auto* package = std::get_if<RepositoryPackagePresent>(
                   &repository_result);
               package != nullptr) {
                if(package->package_name != target ||
                   !retain_repository_order(
                       package->configured_repository_order)) {
                    return fail(make_sync_install_issue(
                        SyncInstallPreparationIssueKind::
                            RepositoryAuthorityChanged,
                        localization::format_translated_message(
                            "Repository authority changed while preparing sync target {}.",
                            target),
                        root));
                }

                if(!loaded_preference.has_value()) {
                    root_slots[index] = SyncRepositoryTransactionRoot{
                        root, *package};
                    has_repository_transaction_root = true;
                    continue;
                }

                try {
                    ResolvedSourceBuildIdentity source =
                        make_repository_source_build_identity(*package);
                    root_slots[index] = SyncRepositorySourceRoot{
                        root, *package, source};
                    pending_source_work.push_back(
                        PendingRepositorySourceWork{
                            index, *package, std::move(source),
                            std::move(loaded_preference.value())});
                    source_target_token_indices.insert(
                        parsed.target_token_indices[index]);
                } catch(const std::exception& error) {
                    return fail(make_sync_install_issue(
                        SyncInstallPreparationIssueKind::
                            SourceWorkPreparationFailed,
                        error.what(), root));
                }
                continue;
            }

            RepositoryPackageNotFound absence =
                std::get<RepositoryPackageNotFound>(
                    std::move(repository_result));
            if(!retain_repository_order(
                   absence.configured_repository_order)) {
                return fail(make_sync_install_issue(
                    SyncInstallPreparationIssueKind::
                        RepositoryAuthorityChanged,
                    localization::format_translated_message(
                        "Repository authority changed while preparing sync target {}.",
                        target),
                    root));
            }
            const std::size_t aur_input_index = aur_plan_targets.size();
            aur_plan_targets.push_back(target);
            pending_aur_roots.push_back(PendingAurSyncRoot{
                index, root, std::move(absence), aur_input_index});
            pending_source_work.push_back(
                PendingAurSourceWork{aur_input_index});
            source_target_token_indices.insert(
                parsed.target_token_indices[index]);
        }
    }

    if(!pending_source_work.empty()) {
        std::optional<std::string> unsupported_option = unsupported_source_sync_option(parsed);
        if(unsupported_option.has_value()) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::UnsupportedSourceOption,
                source_selection == PackageSourceSelection::AurOnly
                    ? localization::format_translated_message(
                          "Unsupported {} option for {}/source-build target: {}",
                          "pacman", "AUR",
                          unsupported_option.value()) +
                          "\n" +
                          localization::format_translated_message(
                              "Rerun {} without this option.",
                              "--aur")
                    : localization::format_translated_message(
                          "Unsupported {} option for {}/source-build target: {}",
                          "pacman", "AUR",
                          unsupported_option.value()) +
                          "\n" +
                          localization::format_translated_message(
                              "Split official repository and {}/source-build targets, or rerun without this option.",
                              "AUR"),
                std::nullopt, unsupported_option));
        }
        try {
            require_supported_production_source_build_options(config);
        } catch(const std::exception& error) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    SourceBuildOptionsUnsupported,
                error.what()));
        }
    }

    if(!aur_plan_targets.empty()) {
        try {
            aur_build_plan.emplace(resolve_build_plan(
                aur_plan_targets,
                provider_selection_callback(config)));
        } catch(const std::exception& error) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    BuildPlanResolutionFailed,
                error.what()));
        }

        for(const PendingAurSyncRoot& pending : pending_aur_roots) {
            std::optional<std::size_t> plan_root_index =
                build_plan_root_index(
                    aur_build_plan.value(),
                    pending.aur_input_index,
                    pending.invocation_correlation.requested_name);
            if(!plan_root_index.has_value()) {
                return fail(make_sync_install_issue(
                    SyncInstallPreparationIssueKind::
                        BuildPlanCorrelationFailed,
                    localization::format_translated_message(
                        "{} root correlation failed for package {}.",
                        "BuildPlan",
                        pending.invocation_correlation.requested_name),
                    pending.invocation_correlation));
            }
            root_slots[pending.target_index] = SyncAurRoot{
                pending.invocation_correlation,
                pending.repository_absence,
                plan_root_index.value()};
        }

        if(has_repository_order && repository_order.has_value() &&
           aur_build_plan->configured_repository_order.has_value() &&
           repository_order !=
               aur_build_plan->configured_repository_order) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    RepositoryAuthorityChanged,
                localization::translate_message(
                    "Repository authority changed during sync build-plan resolution.")));
        }

        try {
            require_executable_build_plan(
                join_package_names(aur_plan_targets),
                aur_build_plan.value());
        } catch(const std::exception& error) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::BuildPlanBlocked,
                error.what()));
        }
    }

    std::vector<std::vector<ProductionSourceBuildWorkItem>>
        aur_work_items_by_root(aur_plan_targets.size());
    if(aur_build_plan.has_value()) {
        try {
            std::vector<ProductionSourceBuildWorkItem> aur_work_items =
                prepare_aur_source_build_work_items(
                    aur_build_plan.value(),
                    source_selection == PackageSourceSelection::Auto,
                    source_sync_options.needed);
            for(auto& work_item : aur_work_items) {
                const std::size_t root_index =
                    earliest_root_index_for_source_build_work_item(
                        aur_build_plan.value(), work_item);
                aur_work_items_by_root[root_index].push_back(
                    std::move(work_item));
            }
        } catch(const SourcePreferenceError& error) {
            return fail(error.failure());
        } catch(const std::exception& error) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    SourceWorkPreparationFailed,
                error.what()));
        }
    }

    std::vector<ProductionSourceBuildWorkItem> source_work_items;
    try {
        ProviderSelectionCallback select_provider =
            provider_selection_callback(config);
        for(PendingSyncSourceWork& pending : pending_source_work) {
            if(auto* repository_source =
                   std::get_if<PendingRepositorySourceWork>(&pending);
               repository_source != nullptr) {
                present_loaded_source_preference(
                    repository_source->preference);
                const std::size_t source_work_item_index =
                    source_work_items.size();
                ProductionSourceBuildWorkItem work_item =
                    prepare_resolved_source_build_work_item(
                        repository_source->source,
                        std::move(
                            repository_source->preference.environment),
                        false, source_sync_options.needed,
                        select_provider);
                work_item.configured_repository_order =
                    repository_source->package.configured_repository_order;
                source_work_items.push_back(std::move(work_item));
                if(repository_source->target_index >= root_slots.size() ||
                   !root_slots[repository_source->target_index]
                        .has_value()) {
                    throw std::logic_error(
                        "Prepared repository source work lost its invocation root.");
                }
                auto* root = std::get_if<SyncRepositorySourceRoot>(
                    &root_slots[repository_source->target_index]
                         .value());
                if(root == nullptr ||
                   root->invocation_correlation.invocation_index !=
                       repository_source->target_index ||
                   root->package != repository_source->package ||
                   root->source != repository_source->source) {
                    throw std::logic_error(
                        "Prepared repository source work differs from its invocation root.");
                }
                root->source_work_item_index = source_work_item_index;
                continue;
            }

            const std::size_t aur_input_index =
                std::get<PendingAurSourceWork>(pending).aur_input_index;
            if(aur_input_index >= aur_work_items_by_root.size()) {
                throw std::logic_error(localization::translate_message(
                    "Build plan root ownership is inconsistent with source targets."));
            }
            append_source_build_work_items(
                source_work_items,
                std::move(
                    aur_work_items_by_root[aur_input_index]));
        }
    } catch(const SourcePreferenceError& error) {
        return fail(error.failure());
    } catch(const std::exception& error) {
        return fail(make_sync_install_issue(
            SyncInstallPreparationIssueKind::
                SourceWorkPreparationFailed,
            error.what()));
    }

    for(const auto& work_items : aur_work_items_by_root) {
        if(!work_items.empty()) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    BuildPlanCorrelationFailed,
                localization::translate_message(
                    "Build plan source targets were not fully projected.")));
        }
    }

    std::optional<PreparedProductionSourceBuildInvocation>
        source_invocation;
    if(!source_work_items.empty()) {
        try {
            // No cache capability is created here. Actual execution activates
            // one before its first repository/source mutation.
            source_invocation.emplace(
                prepare_production_source_build_invocation(
                    std::move(source_work_items), config));
        } catch(const ReviewedSourceProductionError& error) {
            SyncInstallPreparationIssue issue = make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    SourceWorkPreparationFailed,
                error.what());
            issue.reviewed_source_failure = error.failure();
            return fail(std::move(issue));
        } catch(const std::exception& error) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    SourceWorkPreparationFailed,
                error.what()));
        }
    }

    for(const auto& root : root_slots) {
        if(!root.has_value()) {
            return fail(make_sync_install_issue(
                SyncInstallPreparationIssueKind::
                    TargetCorrelationFailed,
                localization::translate_message(
                    "Sync root projection did not retain every target.")));
        }
    }

    PreparedSyncInstall prepared;
    prepared.ordered_roots = take_observed_sync_roots(root_slots);
    prepared.source_selection = source_selection;
    prepared.repository_transaction_required =
        source_selection == PackageSourceSelection::Auto &&
        (has_repository_transaction_root || system_update);
    if(prepared.repository_transaction_required) {
        prepared.repository_pacman_args =
            ordered_pacman_args_excluding_targets(
                parsed, source_target_token_indices);
    }
    prepared.system_update = system_update;
    prepared.needed = source_sync_options.needed;
    prepared.aur_build_plan = std::move(aur_build_plan);
    prepared.source_invocation = std::move(source_invocation);

    if(!prepared.repository_transaction_required &&
       !prepared.source_invocation.has_value()) {
        SyncInstallPreparationFailure failure;
        failure.details.push_back(make_sync_install_issue(
            SyncInstallPreparationIssueKind::EmptyPreparedRoute,
            localization::translate_message(
                "Prepared sync install contains no executable route.")));
        failure.ordered_roots = std::move(prepared.ordered_roots);
        failure.source_selection = prepared.source_selection;
        failure.system_update = prepared.system_update;
        failure.needed = prepared.needed;
        failure.aur_build_plan =
            std::move(prepared.aur_build_plan);
        return failure;
    }
    return prepared;
}

int execute_prepared_sync_install(
    PreparedSyncInstall prepared,
    const AppConfig& config) {
    if(prepared.repository_transaction_required &&
       prepared.repository_pacman_args.empty()) {
        throw std::logic_error(localization::format_translated_message(
            "Prepared repository transaction has no {} arguments.",
            "pacman"));
    }

    if(prepared.source_invocation.has_value()) {
        // POLICY(#352): cache activation is part of actual execution, but it
        // must complete before a mixed route can mutate repository state.
        // The source executor revalidates the same retained capability after
        // the repository transaction before starting source mutation.
        activate_production_source_build_cache(
            prepared.source_invocation.value());
    }

    if(prepared.repository_transaction_required) {
        if(execute_ordered_repository_sync_transaction(
               prepared.repository_pacman_args, config) != 0) {
            throw std::runtime_error(
                localization::format_translated_message(
                    "{} failed.", "Pacman"));
        }
    }

    if(prepared.source_invocation.has_value()) {
        return execute_sync_source_build_invocation(
            std::move(prepared.source_invocation.value()), config);
    }
    if(!prepared.repository_transaction_required) {
        throw std::logic_error(localization::translate_message(
            "Prepared sync install contains no executable route."));
    }
    return 0;
}

int execute_ordered_repository_sync_transaction(
    const std::vector<std::string>& ordered_pacman_args,
    const AppConfig& config) {
    if(ordered_pacman_args.empty()) {
        throw std::logic_error(localization::format_translated_message(
            "Prepared repository transaction has no {} arguments.",
            "pacman"));
    }
    return run_command(
        "sudo pacman " +
        join_pacman_args(ordered_pacman_args, config));
}

namespace {

struct SystemAurRetainedDiagnosticProjection {
    SystemAurUpdateOperationPhase phase =
        SystemAurUpdateOperationPhase::None;
    RuntimeDiagnosticPresentation presentation;
};

std::optional<SystemAurRetainedDiagnosticProjection>
make_system_aur_retained_diagnostic_projection(
    SystemAurUpdateOperationPhase phase,
    DiagnosticClass classification,
    DiagnosticPhase diagnostic_phase,
    DiagnosticSourceKind source_kind,
    DiagnosticRequiredAction required_action,
    const std::string& retained_detail) {
    if(retained_detail.empty()) return std::nullopt;

    DiagnosticIdentity identity;
    identity.source_kind = source_kind;
    const NormalizedDiagnostic<SystemAurUpdateOperationPhase> diagnostic{
        classification,
        DiagnosticSeverity::Error,
        DiagnosticOperation::PacmanDelegation,
        diagnostic_phase,
        std::move(identity),
        phase,
        required_action,
        DiagnosticBlockingDecision::StopsFollowingPhases,
        DiagnosticExitStatusEffect::Failure,
        retained_detail};
    return SystemAurRetainedDiagnosticProjection{
        phase, present_runtime_diagnostic(diagnostic, retained_detail)};
}

std::optional<SystemAurRetainedDiagnosticProjection>
project_system_aur_retained_diagnostic(
    const SystemAurUpdateOperationResult& result) {
    // POLICY(#505): a retained typed child owns its own diagnostic projection.
    // Outer raw detail is considered only for a childless fatal boundary, so
    // suppression depends on capability state rather than matching strings.
    if(result.aur.operation_result.has_value()) return std::nullopt;

    if(result.stopped_phase == SystemAurUpdateOperationPhase::Repository &&
       result.repository.status ==
           SystemAurUpdateRepositoryPhaseStatus::Failed &&
       result.repository.diagnostic.has_value()) {
        return make_system_aur_retained_diagnostic_projection(
            SystemAurUpdateOperationPhase::Repository,
            DiagnosticClass::ExecutionFailure,
            DiagnosticPhase::Install,
            DiagnosticSourceKind::Pacman,
            DiagnosticRequiredAction::ResolveBlocker,
            *result.repository.diagnostic);
    }

    if(result.stopped_phase ==
           SystemAurUpdateOperationPhase::ForeignInventory &&
       result.foreign_inventory.status ==
           SystemAurUpdateForeignInventoryPhaseStatus::Failed) {
        DiagnosticIdentity identity;
        identity.source_kind = DiagnosticSourceKind::Pacman;
        if(result.foreign_inventory.failure.has_value()) {
            const PackageMetadataFailure& failure =
                *result.foreign_inventory.failure;
            const std::string retained_detail =
                !failure.diagnostic.empty()
                    ? failure.diagnostic
                    : result.foreign_inventory.diagnostic.value_or(
                          localization::translate_message(
                              "package metadata failure"));
            const auto diagnostic = project_package_metadata_diagnostic(
                failure,
                DiagnosticOperation::PacmanDelegation,
                DiagnosticPhase::Query,
                std::move(identity));
            return SystemAurRetainedDiagnosticProjection{
                SystemAurUpdateOperationPhase::ForeignInventory,
                present_runtime_diagnostic(diagnostic, retained_detail)};
        }
        if(result.foreign_inventory.diagnostic.has_value()) {
            return make_system_aur_retained_diagnostic_projection(
                SystemAurUpdateOperationPhase::ForeignInventory,
                DiagnosticClass::QueryFailure,
                DiagnosticPhase::Query,
                DiagnosticSourceKind::Pacman,
                DiagnosticRequiredAction::RetryQuery,
                *result.foreign_inventory.diagnostic);
        }
        return std::nullopt;
    }

    if(result.stopped_phase == SystemAurUpdateOperationPhase::AurQuery &&
       result.query.status == SystemAurUpdateQueryPhaseStatus::Failed &&
       !result.query.query_result.has_value() &&
       result.query.diagnostic.has_value()) {
        return make_system_aur_retained_diagnostic_projection(
            SystemAurUpdateOperationPhase::AurQuery,
            DiagnosticClass::QueryFailure,
            DiagnosticPhase::Query,
            DiagnosticSourceKind::Aur,
            DiagnosticRequiredAction::RetryQuery,
            *result.query.diagnostic);
    }

    if(result.stopped_phase ==
           SystemAurUpdateOperationPhase::AurPreparation &&
       result.aur.diagnostic.has_value()) {
        const bool is_inconsistent =
            result.aur.status ==
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
        if(is_inconsistent ||
           result.aur.status ==
               SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution) {
            return make_system_aur_retained_diagnostic_projection(
                SystemAurUpdateOperationPhase::AurPreparation,
                is_inconsistent
                    ? DiagnosticClass::InternalInconsistency
                    : DiagnosticClass::Blocked,
                DiagnosticPhase::Planning,
                DiagnosticSourceKind::Aur,
                is_inconsistent
                    ? DiagnosticRequiredAction::ReportInconsistency
                    : DiagnosticRequiredAction::ResolveBlocker,
                *result.aur.diagnostic);
        }
    }

    if(result.stopped_phase ==
           SystemAurUpdateOperationPhase::AurExecution &&
       result.aur.status ==
           SystemAurUpdateAurPhaseStatus::InconsistentResult &&
       result.aur.diagnostic.has_value()) {
        return make_system_aur_retained_diagnostic_projection(
            SystemAurUpdateOperationPhase::AurExecution,
            DiagnosticClass::InternalInconsistency,
            DiagnosticPhase::Install,
            DiagnosticSourceKind::Aur,
            DiagnosticRequiredAction::ReportInconsistency,
            *result.aur.diagnostic);
    }
    return std::nullopt;
}

void report_system_aur_retained_diagnostic(
    const SystemAurUpdateOperationResult& result) {
    const std::optional<SystemAurRetainedDiagnosticProjection> projection =
        project_system_aur_retained_diagnostic(result);
    if(projection.has_value()) {
        report_runtime_diagnostic(projection->presentation);
    }
}

void report_system_aur_not_attempted(
    const SystemAurUpdateOperationResult& result) {
    if(result.aur.status !=
       SystemAurUpdateAurPhaseStatus::NotAttempted) {
        return;
    }
    std::cout << localization::format_translated_message(
                     // TRANSLATORS: AUR is a runtime project identity.
                     "The {} update was not attempted.", "AUR")
              << std::endl;
}

void report_system_aur_partial_failure(
    const SystemAurUpdateOperationResult& result) {
    switch(result.status) {
        case SystemAurUpdateOperationStatus::StoppedBeforeAurExecution:
            switch(result.stopped_phase) {
                case SystemAurUpdateOperationPhase::ForeignInventory:
                    Logger::error(localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "The repository system upgrade completed, but the fresh installed-package inventory for {} could not be obtained.",
                        "AUR"));
                    break;
                case SystemAurUpdateOperationPhase::AurQuery:
                    Logger::error(localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "The repository system upgrade completed, but the fresh {} update query failed.",
                        "AUR"));
                    break;
                case SystemAurUpdateOperationPhase::AurPreparation:
                    Logger::error(localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "The repository system upgrade completed, but the {} update was blocked before execution.",
                        "AUR"));
                    break;
                case SystemAurUpdateOperationPhase::None:
                case SystemAurUpdateOperationPhase::Repository:
                case SystemAurUpdateOperationPhase::AurExecution:
                case SystemAurUpdateOperationPhase::Reduction:
                    Logger::error(localization::format_translated_message(
                        // TRANSLATORS: AUR is a runtime project identity.
                        "The repository system upgrade completed, but the {} update could not start.",
                        "AUR"));
                    break;
            }
            break;
        case SystemAurUpdateOperationStatus::StoppedOnAurFailure:
            Logger::error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "The repository system upgrade completed, but the {} update failed.",
                "AUR"));
            break;
        case SystemAurUpdateOperationStatus::
            StoppedAfterAurCleanupFailure:
            Logger::error(localization::format_translated_message(
                // TRANSLATORS: AUR is a runtime project identity.
                "The repository system upgrade completed, but {} cleanup failed after a package transaction.",
                "AUR"));
            break;
        case SystemAurUpdateOperationStatus::Completed:
        case SystemAurUpdateOperationStatus::StoppedOnRepositoryFailure:
            break;
        case SystemAurUpdateOperationStatus::InconsistentResult:
            if(result.stopped_phase ==
               SystemAurUpdateOperationPhase::AurPreparation) {
                Logger::error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "The repository system upgrade completed, but the {} update was blocked before execution.",
                    "AUR"));
            } else if(result.stopped_phase ==
                      SystemAurUpdateOperationPhase::AurExecution) {
                Logger::error(localization::format_translated_message(
                    // TRANSLATORS: AUR is a runtime project identity.
                    "The repository system upgrade completed, but the {} update failed.",
                    "AUR"));
            }
            break;
    }
    report_system_aur_retained_diagnostic(result);
    report_system_aur_not_attempted(result);
    Logger::warn(localization::translate_message(
        "The completed repository system upgrade was not rolled back."));
}

} // namespace

void present_system_aur_update_operation_result(
    SystemAurUpdateOperationResult result) {
    const SystemAurUpdateOperationResult authority =
        reduce_system_aur_update_result(std::move(result));

    // Validate the nested presenter before emitting the repository success
    // fact. A malformed child must fail closed without leaking a success line.
    if(authority.aur.operation_result.has_value()) {
        static_cast<void>(format_aur_update_cli_presentation(
            authority.aur.operation_result->reduced_operation_result));
    }

    // Incoherent aggregate state never emits aggregate success. A childless
    // exception with a reducer-validated repository prefix may still expose
    // that completed phase before its typed AUR failure detail.
    if(authority.has_inconsistency() ||
       authority.status ==
           SystemAurUpdateOperationStatus::InconsistentResult) {
        const std::optional<SystemAurRetainedDiagnosticProjection>
            retained_diagnostic =
                project_system_aur_retained_diagnostic(authority);
        const bool has_coherent_post_repository_failure =
            retained_diagnostic.has_value() &&
            authority.repository.status ==
                SystemAurUpdateRepositoryPhaseStatus::Completed &&
            (retained_diagnostic->phase ==
                 SystemAurUpdateOperationPhase::AurPreparation ||
             retained_diagnostic->phase ==
                 SystemAurUpdateOperationPhase::AurExecution);
        if(has_coherent_post_repository_failure) {
            std::cout << localization::translate_message(
                             "The repository system upgrade completed.")
                      << std::endl;
        }
        Logger::error(localization::format_translated_message(
            // TRANSLATORS: AUR is a runtime project identity.
            "The repository and {} update result is inconsistent; no success was reported.",
            "AUR"));
        if(has_coherent_post_repository_failure) {
            report_system_aur_partial_failure(authority);
        } else {
            report_system_aur_retained_diagnostic(authority);
        }
        return;
    }

    switch(authority.repository.status) {
        case SystemAurUpdateRepositoryPhaseStatus::Failed:
            Logger::error(localization::translate_message(
                "The repository system upgrade failed."));
            report_system_aur_retained_diagnostic(authority);
            report_system_aur_not_attempted(authority);
            return;
        case SystemAurUpdateRepositoryPhaseStatus::Completed:
            std::cout << localization::translate_message(
                             "The repository system upgrade completed.")
                      << std::endl;
            break;
        case SystemAurUpdateRepositoryPhaseStatus::NotAttempted:
            Logger::error(localization::translate_message(
                "The repository system upgrade was not attempted."));
            return;
    }

    if(authority.aur.operation_result.has_value()) {
        present_filtered_aur_update_execution_result(
            authority.aur.operation_result.value());
    }

    if(authority.status == SystemAurUpdateOperationStatus::Completed) {
        std::cout << localization::format_translated_message(
                         // TRANSLATORS: AUR is a runtime project identity.
                         "The repository system upgrade and normal {} update completed.",
                         "AUR")
                  << std::endl;
        return;
    }
    report_system_aur_partial_failure(authority);
}

int cmd_system_aur_update(
    PreparedSystemAurUpdateOperation prepared,
    const AppConfig& config) {
    SystemAurUpdateOperationResult result =
        execute_prepared_system_aur_update_operation(
            std::move(prepared), config);
    const bool is_success = result.is_success();
    present_system_aur_update_operation_result(std::move(result));
    return is_success ? 0 : 1;
}

#ifdef MOGUET_ENABLE_SYSTEM_AUR_UPDATE_PRESENTATION_TEST_HOOKS
namespace {

CompatibleSystemAurUpdateRequest compatible_system_aur_request_for_test() {
    std::optional<CompatibleSystemAurUpdateRequest> request =
        make_compatible_system_aur_update_request(
            AutoSystemUpdateRouteCandidate{
                CompatibleAutoSystemUpdatePacmanArguments{},
                {"-Syu"},
                false});
    if(!request.has_value()) {
        throw std::logic_error(
            "Test-only system/AUR request construction failed.");
    }
    return std::move(request.value());
}

SystemAurUpdateOperationResult completed_system_aur_prefix_for_test(
    const AurUpdateQueryResult& query_result) {
    SystemAurUpdateOperationResult result;
    result.repository.status =
        SystemAurUpdateRepositoryPhaseStatus::Completed;
    result.repository.compatible_request =
        compatible_system_aur_request_for_test();
    result.repository.ordered_pacman_args = {"-Syu"};
    result.repository.command_exit_status = 0;
    result.foreign_inventory.status =
        SystemAurUpdateForeignInventoryPhaseStatus::Completed;
    result.foreign_inventory.repository_configuration =
        PacmanRepositoryConfiguration{
            PacmanDatabasePaths{
                "/fixture/root", "/fixture/database"},
            {"core"}};
    for(const AurUpdatePlanEntry& entry : query_result.plan.entries) {
        result.foreign_inventory.inventory.push_back(
            InstalledPackageMetadata{
                entry.installed_name,
                entry.installed_version,
                entry.install_reason});
    }
    result.query.status = query_result.recoverable_failures.empty()
                              ? SystemAurUpdateQueryPhaseStatus::Completed
                              : SystemAurUpdateQueryPhaseStatus::Failed;
    result.query.query_result = query_result;
    return result;
}

AurUpdateQueryResult executable_system_aur_query_for_test() {
    AurUpdateQueryResult result;
    result.plan.entries.push_back(AurUpdatePlanEntry{
        "fixture-child",
        "1.0-1",
        InstalledPackageReason::Explicit,
        AurUpdateRemotePackage{
            "fixture-child",
            "fixture-base",
            "2.0-1",
            AurVersionRelation::NewerThanInstalled},
        AurUpdateClassification::UpdateAvailable});
    return result;
}

int present_system_aur_test_result(
    SystemAurUpdateOperationResult result) {
    SystemAurUpdateOperationResult reduced =
        reduce_system_aur_update_result(std::move(result));
    const bool is_success = reduced.is_success();
    present_system_aur_update_operation_result(std::move(reduced));
    return is_success ? 0 : 1;
}

} // namespace

int run_system_aur_update_presentation_test(
    const std::string& test_case) {
    if(test_case == "repository-exception") {
        SystemAurUpdateOperationResult result;
        result.repository.status =
            SystemAurUpdateRepositoryPhaseStatus::Failed;
        result.repository.compatible_request =
            compatible_system_aur_request_for_test();
        result.repository.ordered_pacman_args = {"-Syu"};
        result.repository.diagnostic =
            std::string{"fixture repository exception\n"} +
            std::string{"\x1b", 1} + "unsafe\\detail";
        result.foreign_inventory.status =
            SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted;
        result.foreign_inventory.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::RepositoryFailure;
        result.query.status =
            SystemAurUpdateQueryPhaseStatus::NotAttempted;
        result.query.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::RepositoryFailure;
        result.aur.status = SystemAurUpdateAurPhaseStatus::NotAttempted;
        result.aur.not_attempted_reason =
            SystemAurUpdateNotAttemptedReason::RepositoryFailure;
        return present_system_aur_test_result(std::move(result));
    }

    if(test_case == "preparation-exception") {
        SystemAurUpdateOperationResult result =
            completed_system_aur_prefix_for_test(
                executable_system_aur_query_for_test());
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::BlockedBeforeExecution;
        result.aur.diagnostic =
            std::string{"fixture preparation exception\n"} +
            std::string{"\x1b", 1} + "unsafe\\detail";
        return present_system_aur_test_result(std::move(result));
    }

    if(test_case == "execution-exception") {
        SystemAurUpdateOperationResult result =
            completed_system_aur_prefix_for_test(
                executable_system_aur_query_for_test());
        result.aur.status =
            SystemAurUpdateAurPhaseStatus::InconsistentResult;
        result.aur.diagnostic =
            std::string{"fixture execution exception\n"} +
            std::string{"\x1b", 1} + "unsafe\\detail";
        result.stopped_phase =
            SystemAurUpdateOperationPhase::AurExecution;
        return present_system_aur_test_result(std::move(result));
    }

    if(test_case == "child-query-failure") {
        const AppConfig config;
        FilteredAurUpdateExecutionResult child =
            execute_prepared_filtered_aur_update_operation(
                prepare_upgrade_aur_operation(config), config);
        // The command stub produces the strict upgrade-aur snapshot. This
        // test-only wrapper changes every policy snapshot together so the
        // retained child models the ordinary system+AUR route instead.
        child.devel_requires_check_policy =
            DevelRequiresCheckPolicy::SkipIndependentTarget;
        child.preflight.devel_requires_check_policy =
            DevelRequiresCheckPolicy::SkipIndependentTarget;
        child.preparation.devel_requires_check_policy =
            DevelRequiresCheckPolicy::SkipIndependentTarget;
        child.reduced_operation_result.devel_requires_check_policy =
            DevelRequiresCheckPolicy::SkipIndependentTarget;
        SystemAurUpdateOperationResult result =
            completed_system_aur_prefix_for_test(child.query_result);
        result.aur.operation_result.emplace(std::move(child));
        return present_system_aur_test_result(std::move(result));
    }

    if(test_case != "inconsistent") {
        throw std::logic_error(
            "Unknown test-only system/AUR presentation case: " +
            test_case);
    }

    CompatibleSystemAurUpdateRequest request =
        compatible_system_aur_request_for_test();
    PreparedSystemAurUpdateOperation prepared =
        prepare_system_aur_update_operation(
            std::move(request));
    PreparedSystemAurUpdateOperation retained(
        std::move(prepared));
    if(!retained.is_valid() || prepared.is_valid()) {
        throw std::logic_error(
            "Test-only system/AUR capability move state is inconsistent.");
    }
    return cmd_system_aur_update(
        std::move(prepared), AppConfig{});
}
#endif

int cmd_sync_install(
    const ParsedCliArguments& parsed, bool is_sys_upgrade,
    PackageSourceSelection source_selection, const AppConfig& config) {
    if(source_selection == PackageSourceSelection::RepoOnly) {
        // POLICY(#168): RepoOnly is one ordered binary repository transaction; no classification probe.
        return execute_ordered_repository_sync_transaction(
            parsed.ordered_pacman_args, config);
    }

    const bool system_update = is_sys_upgrade;
    if(source_selection == PackageSourceSelection::Auto &&
       parsed.targets.empty() && !system_update) {
        return execute_ordered_repository_sync_transaction(
            parsed.ordered_pacman_args, config);
    }

    SyncInstallPreparation preparation = prepare_sync_install(
        parsed, system_update, source_selection, config);
    if(const auto* failure =
           std::get_if<SyncInstallPreparationFailure>(&preparation);
       failure != nullptr) {
        report_sync_install_preparation_failure(*failure);
        return 1;
    }
    return execute_prepared_sync_install(
        std::move(std::get<PreparedSyncInstall>(preparation)), config);
}

int cmd_sync_info(
    const ParsedCliArguments& parsed, bool use_sudo,
    PackageSourceSelection source_selection, const AppConfig& config) {
    if(source_selection == PackageSourceSelection::Auto && use_sudo) {
        auto unqualified_target =
            std::find_if(parsed.targets.begin(), parsed.targets.end(), [](const std::string& target) {
                return target.find('/') == std::string::npos;
            });
        if(unqualified_target != parsed.targets.end()) {
            // POLICY(#172): refresh 後に AUR fallback すると official DB の更新だけが先行する。
            // refresh 付き info は repository-qualified target に限定し、分類前に停止する。
            Logger::error(localization::format_translated_message(
                "Cannot combine {} refresh with {} info fallback for unqualified target: {}",
                "pacman", "AUR", *unqualified_target));
            Logger::error(localization::format_translated_message(
                "Use a repository-qualified target such as {}, or run refresh and {} separately.",
                "repo/package", "-Si"));
            return 1;
        }
    }

    std::string pacman_prefix = use_sudo ? "sudo pacman " : "pacman ";

    if(source_selection == PackageSourceSelection::RepoOnly) {
        return run_command(
            pacman_prefix + join_pacman_args(parsed.ordered_pacman_args, config));
    }

    if(source_selection == PackageSourceSelection::AurOnly) {
        if(parsed_has_semantic_pacman_option(parsed, "--needed")) {
            Logger::error(localization::format_translated_message(
                "Unsupported {} option for {} info: {}",
                "pacman", "AUR", "--needed"));
            return 1;
        }
        if(parsed.targets.empty()) {
            Logger::error(localization::format_translated_message(
                "Missing {} package target.", "AUR"));
            return 1;
        }
        for(const auto& target : parsed.targets) {
            require_valid_aur_package_target(target);
        }

        bool failed = false;
        std::vector<AurPackageInfo> aur_infos;
        for(const auto& target : parsed.targets) {
            try {
                std::optional<AurPackageInfo> info = AurClient::info(target);
                if(info.has_value())
                    aur_infos.push_back(info.value());
                else {
                    Logger::error(localization::format_translated_message(
                        "{} package not found: {}", "AUR", target));
                    failed = true;
                }
            } catch(const std::exception& e) {
                Logger::error(localization::format_translated_message(
                    "Failed to fetch {} info for {}: {}",
                    "AUR", target, e.what()));
                failed = true;
            }
        }
        for(size_t i = 0; i < aur_infos.size(); ++i) {
            if(i > 0) std::cout << std::endl;
            print_aur_info(aur_infos[i]);
        }
        return failed ? 1 : 0;
    }

    if(parsed.targets.empty()) {
        return run_command(
            pacman_prefix + join_pacman_args(parsed.ordered_pacman_args, config));
    }

    bool failed = false;
    std::vector<std::string> repo_targets;
    std::set<size_t> aur_target_token_indices;
    std::vector<AurPackageInfo> aur_infos;

    for(size_t i = 0; i < parsed.targets.size(); ++i) {
        const std::string& target = parsed.targets[i];
        if(target.find('/') != std::string::npos) {
            repo_targets.push_back(target);
            continue;
        }

        require_valid_package_name(target);
        if(is_repo_package(target)) {
            repo_targets.push_back(target);
            continue;
        }

        try {
            std::optional<AurPackageInfo> info = AurClient::info(target);
            if(info.has_value()) {
                aur_infos.push_back(info.value());
                aur_target_token_indices.insert(parsed.target_token_indices[i]);
            } else {
                Logger::error(localization::format_translated_message(
                    "Package not found in repos or {}: {}",
                    "AUR", target));
                failed = true;
                aur_target_token_indices.insert(parsed.target_token_indices[i]);
            }
        } catch(const std::exception& e) {
            Logger::error(localization::format_translated_message(
                "Failed to fetch {} info for {}: {}",
                "AUR", target, e.what()));
            failed = true;
            aur_target_token_indices.insert(parsed.target_token_indices[i]);
        }
    }

    if(!repo_targets.empty()) {
        // POLICY(#173): 同名のoption valueを残し、AUR targetのtoken位置だけを除外する。
        std::vector<std::string> pacman_args =
            ordered_pacman_args_excluding_targets(parsed, aur_target_token_indices);
        if(run_command(pacman_prefix + join_pacman_args(pacman_args, config)) != 0) failed = true;
        if(!aur_infos.empty()) std::cout << std::endl;
    }

    for(size_t i = 0; i < aur_infos.size(); ++i) {
        if(i > 0) std::cout << std::endl;
        print_aur_info(aur_infos[i]);
    }

    return failed ? 1 : 0;
}
