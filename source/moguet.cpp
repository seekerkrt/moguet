/**
 * Moguet - A full-featured Pacman wrapper and AUR helper
 *
 * Features:
 * - Smart Upgrade: Skips rebuilding packages if the version hasn't changed during 'upgrade'.
 * - Strict typed user configuration with CLI override composition.
 * - '--nodiff' option support.
 * - '--noconfirm' option support.
 */

// このファイルは、Moguet の CLI 入口、pacman wrapper、AUR/source build 補助をまとめる実装単位。
// 関数宣言と詳細実装は、将来の分割候補が見えるように section comment で大まかな責務ごとに分類する。

#include "application_identity.hpp"
#include "app_config.hpp"
#include "aur_rpc.hpp"
#include "cli_authority.hpp"
#include "cli_parser.hpp"
#include "cli_routing.hpp"
#include "cli_runtime_contract.hpp"
#include "commands_aur_update.hpp"
#include "commands_inspect.hpp"
#include "commands_source_maintenance.hpp"
#include "commands_sync.hpp"
#include "commands_upgrade_all.hpp"
#include "dry_run.hpp"
#include "interactive_confirmation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "provider_installed_state_presentation.hpp"
#include "runtime_diagnostic.hpp"
#include "shell_words.hpp"
#include "source_install.hpp"
#include "system_aur_update_operation.hpp"
#include "user_config.hpp"
#include "xdg_directory_safety.hpp"
#include "xdg_paths.hpp"
#include "xdg_state_log.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

AppConfig g_config;

constexpr std::size_t HELP_DESCRIPTION_COLUMN = 42;

void print_help_section(const std::string& heading) {
    std::cout << "\033[1m" << heading << "\033[0m" << std::endl;
}

void print_help_entry(
    std::string_view syntax, const std::string& description) {
    // NO_TRANSLATE: Help syntax and ANSI/layout bytes are locale-independent
    // CLI grammar; each human-readable description is translated by its caller.
    std::cout << "    \033[1m" << syntax << "\033[0m";
    const std::size_t visible_width = 4 + syntax.size();
    if(visible_width >= HELP_DESCRIPTION_COLUMN) {
        std::cout << '\n'
                  << std::string(HELP_DESCRIPTION_COLUMN, ' ');
    } else {
        std::cout << std::string(
            HELP_DESCRIPTION_COLUMN - visible_width, ' ');
    }
    std::cout << description << std::endl;
}

void print_help_continuation(const std::string& description) {
    std::cout << std::string(HELP_DESCRIPTION_COLUMN, ' ')
              << description << std::endl;
}

void report_direct_error(const std::string& diagnostic) {
    // TRANSLATORS: The placeholder is a complete startup or shutdown diagnostic.
    std::cerr << "\033[1;31m::\033[0m "
              << localization::format_translated_message(
                     "Error: {}", diagnostic)
              << std::endl;
}

} // namespace

// --- 関数宣言 ---

// CLI入口 / help
int run_moguet(int argc, char* argv[]);
void print_help();
bool handle_info_only_option(int argc, char* argv[]);
bool argv_requests_pkgbuild_export_diagnostics(int argc, char* argv[]);

// shell引数 / command construction
std::vector<std::string> pacman_args_with_global_options(std::vector<std::string> args);
std::string join_pacman_args(const std::vector<std::string>& args);

// pacman / repository補助
bool validate_optionless_moguet_operation(const std::string& operation, const std::vector<std::string>& flags);
namespace {

UserConfig load_invocation_user_config() {
#ifdef MOGUET_ENABLE_TEST_CONFIG_PATH
    // POLICY: productionはXDG authorityを使い、専用test binaryだけが
    // strict TOML loaderの明示pathを選ぶ。
    const char* test_config_path = std::getenv("MOGUET_TEST_CONFIG_FILE");
    if(test_config_path && test_config_path[0] != '\0') {
        return load_user_config(test_config_path);
    }
#endif
    const xdg_paths::ConfigPaths config_paths =
        xdg_paths::resolve_config_process_environment();
    return load_user_config(config_paths.config_file);
}

bool is_source_preference_target_operation(
    const std::string& operation) {
    return operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::AddSource)
                   .token ||
           operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::DeleteSource)
                   .token ||
           operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Revert)
                   .token ||
           operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::EditSource)
                   .token;
}

bool validate_pre_log_source_preference_targets(
    const ParsedCliArguments& parsed) {
    if(!is_source_preference_target_operation(parsed.operation)) return true;

    const bool is_add_src =
        parsed.operation ==
        cli_authority::operation_spec(
            cli_authority::OperationId::AddSource)
            .token;
    try {
        for(const std::string& target : parsed.targets) {
            // POLICY(#335): add-srcのV=K operandはpackageではない。
            // handlerがassignment ordering/valueを検証する既存責務を維持する。
            if(is_add_src && target.find('=') != std::string::npos) continue;
            require_valid_package_name(target);
        }
    } catch(const std::exception& error) {
        Logger::error(error.what());
        return false;
    }
    return true;
}

bool validate_pre_log_operation_route(const ParsedCliArguments& parsed) {
    return validate_pre_log_source_preference_targets(parsed);
}

} // namespace

// --- CLI 入口 ---
int run_moguet(int argc, char* argv[]) {
    if(handle_info_only_option(argc, argv)) return 0;

    // POLICY(#167): parser/root-check failureも -Gp のmachine-readable stdoutへ混ぜない。
    if(argv_requests_pkgbuild_export_diagnostics(argc, argv)) {
        Logger::set_diagnostics_to_stderr();
    }

    if(geteuid() == 0) {
        Logger::error(
            localization::format_translated_message(
                "Do not run {} as {} or with {}.",
                application_identity::PROJECT_NAME,
                "root", "sudo"));
        Logger::error(
            localization::format_translated_message(
                "Run {} as a normal user; {} will invoke {}/{} when needed.",
                application_identity::COMMAND_NAME,
                application_identity::PROJECT_NAME,
                "sudo", "pacman"));
        return 1;
    }

    if(argc < 2) {
        print_help();
        return 1;
    }

    // Configはparse成功までlocalに保持し、invalid inputから最終実行設定への
    // partial publishを防ぐ。missing leafだけがbuilt-in defaultになる。
    UserConfig user_config;
    try {
        user_config = load_invocation_user_config();
    } catch(const std::exception& e) {
        Logger::error(e.what());
        return 1;
    } catch(...) {
        Logger::error(localization::translate_message(
            "Failed to load the user configuration because of an unknown error."));
        return 1;
    }

    std::optional<ParsedCliArguments> parsed_result = parse_cli_arguments(argc, argv);
    if(!parsed_result.has_value()) return 1;

    const ParsedCliArguments& parsed = parsed_result.value();
    if(parsed.operation.empty()) {
        // POLICY: usage presentationはrunnerが所有し、CLI override publish前に終了する。
        print_help();
        return 1;
    }
    const CliInvocationValidation invocation_validation =
        validate_cli_invocation_contract(parsed);
    if(!invocation_validation.is_valid()) {
        const auto& diagnostic = invocation_validation.diagnostic.value();
        report_runtime_diagnostic(
            diagnostic,
            cli_invocation_issue_message(diagnostic.reason));
        return 1;
    }
    UserConfig final_user_config = compose_user_config(
        std::move(user_config), parsed.cli_overrides);
    g_config = make_app_config(
        std::move(final_user_config),
        parsed.cli_overrides.no_confirm,
        parsed.cli_overrides.rm_deps,
        parsed.cli_overrides.presentation_detail);
    g_config.provider_candidate_presenter_factory =
        make_provider_installed_state_candidate_presenter_factory();

    // POLICY(#352): dry-run owns a fail-closed route before every persistent
    // state, export/Git, local evaluator, cache, or executor boundary.
    if(parsed.cli_overrides.dry_run) {
        return run_dry_run(parsed, g_config);
    }

    // POLICY(#505): only the shared exact targetless classifier may create
    // the composite capability. Auto option rejection and source-build option
    // rejection both finish before the repository transaction or state log.
    const SyncInvocationRouteClassification sync_invocation_route =
        classify_sync_invocation_route(parsed);
    std::optional<PreparedSystemAurUpdateOperation>
        prepared_system_aur_update;
    if(const auto* auto_candidate =
           std::get_if<AutoSystemUpdateRouteCandidate>(
               &sync_invocation_route)) {
        try {
            require_supported_production_source_build_options(g_config);
            std::optional<CompatibleSystemAurUpdateRequest> request =
                make_compatible_system_aur_update_request(
                    *auto_candidate);
            if(!request.has_value()) {
                throw std::logic_error(
                    localization::format_translated_message(
                        // TRANSLATORS: The placeholder is the AUR project identity.
                        "The combined system and {} update request is incompatible.",
                        "AUR"));
            }
            prepared_system_aur_update.emplace(
                prepare_system_aur_update_operation(
                    std::move(request.value())));
        } catch(const std::exception& error) {
            Logger::error(error.what());
            return 1;
        }
    }

    // POLICY(#271): exact operation-local selectorをgeneric build option
    // rejectionより先にstrict projectionし、directory/root inspectionまでを
    // default state logやcacheの作成前に完了する。
    std::optional<PreparedLocalSourceBuildRoute>
        prepared_local_source_build;
    if(local_source_build_requested(parsed)) {
        try {
            LocalSourceBuildInvocation invocation =
                require_local_source_build_invocation(parsed);
            prepared_local_source_build.emplace(
                prepare_local_source_build_route(
                    std::move(invocation), g_config));
            require_executable_local_source_build_route(
                prepared_local_source_build.value());
        } catch(const std::exception& error) {
            Logger::error(error.what());
            return 1;
        }
    }

    if(parsed.operation ==
       cli_authority::operation_spec(
           cli_authority::OperationId::UpgradeAll)
           .token) {
        // POLICY(#281): upgrade-all is target-less and does not inherit
        // pacman operands. Reject misuse before default state log
        // initialization or any source/cache/inventory/AUR preparation.
        const std::vector<std::string> validation_errors =
            validate_upgrade_all_invocation(parsed);
        if(!validation_errors.empty()) {
            for(const std::string& error : validation_errors) {
                Logger::error(error);
            }
            return 1;
        }
        try {
            // Config-file RMDEPS must also fail before preparation, including
            // invocations with no registered source preferences.
            require_supported_production_source_build_options(g_config);
        } catch(const std::exception& error) {
            Logger::error(error.what());
            return 1;
        }
    }

    std::optional<PkgbuildExportMode> export_mode = pkgbuild_export_mode(parsed);
    if(export_mode.has_value()) {
        // POLICY(#167,#305): export/print はdefault state log初期化より前に
        // 分岐し、内部build cacheを作らない。
        Logger::set_diagnostics_to_stderr();
        try {
            PkgbuildExportInvocation invocation =
                require_pkgbuild_export_invocation(parsed);
            if(invocation.mode == PkgbuildExportMode::Tree) {
                return cmd_export_pkgbuild_tree(invocation);
            }
            return cmd_print_pkgbuild(invocation);
        } catch(const std::exception& e) {
            Logger::error(e.what());
            return 1;
        }
    }

    // POLICY(#168,#305): selector conflict / scope errors must stop before
    // the default state directory or log file is created.
    std::optional<std::string> source_selection_error =
        validate_source_selection_operation(parsed);
    if(source_selection_error.has_value()) {
        Logger::error(source_selection_error.value());
        return 1;
    }

    std::optional<PreparedRootPackageInstall>
        prepared_root_package_install;
    if(parsed.root_package_selection_requested) {
        // POLICY(#217): `--select <bare-token>` is not an operation-omission
        // form. Preserve the existing unknown/custom operation validation
        // before applying the select-specific plain `-S` contract.
        if(!validate_pre_log_operation_route(parsed)) return 1;
        try {
            RootPackageSelectionInvocation invocation =
                require_root_package_selection_invocation(parsed);
            RootPackageInstallPreparation preparation =
                prepare_root_package_install(
                    parsed, std::move(invocation), g_config);
            auto* prepared = std::get_if<PreparedRootPackageInstall>(
                &preparation);
            if(prepared == nullptr) return 1;
            prepared_root_package_install.emplace(std::move(*prepared));
        } catch(const std::exception& error) {
            Logger::error(error.what());
            return 1;
        }
    }

    std::optional<PreparedFilteredAurUpdateOperation>
        prepared_aur_update;
    std::optional<ScopedLoggerDiagnosticCapture>
        aur_update_diagnostic_capture;

    const cli_authority::OperationSpec* custom_operation =
        cli_authority::find_moguet_operation(parsed.operation);
    if(custom_operation != nullptr &&
       !prepared_local_source_build.has_value() &&
       custom_operation->rejects_options_before_dispatch &&
       !validate_optionless_moguet_operation(parsed.operation, parsed.flags)) {
        // POLICY(#169): unsupported custom-operation options must stop before cache or source mutation.
        return 1;
    }

    if(!validate_pre_log_operation_route(parsed)) return 1;

    if(parsed.operation ==
       cli_authority::operation_spec(
           cli_authority::OperationId::UpgradeAur)
           .token) {
        try {
            // POLICY(#267/#270): query/static preflight and option rejection
            // finish before the default state log. RequiresCheck/no-op never
            // gain a state/cache mutation merely by being observed.
            aur_update_diagnostic_capture.emplace();
            prepared_aur_update.emplace(
                prepare_upgrade_aur_operation(g_config));
            aur_update_diagnostic_capture->stop();
            if(!prepared_aur_update->is_prepared()) {
                aur_update_diagnostic_capture->replay();
                return cmd_upgrade_aur(
                    std::move(*prepared_aur_update), g_config);
            }
        } catch(const std::exception& error) {
            if(aur_update_diagnostic_capture.has_value()) {
                aur_update_diagnostic_capture->replay();
            }
            Logger::error(error.what());
            return 1;
        }
    }

    try {
        // POLICY(#305,#306): stateだけを解決し、validated directory
        // descriptorからfixed default logをopenしてLoggerへownershipを移す。
        xdg_paths::StatePaths state_paths =
            xdg_paths::resolve_state_process_environment();
        xdg_directory_safety::DirectoryCreationPrecondition
            state_creation_precondition;
        if(prepared_local_source_build.has_value()) {
            state_creation_precondition =
                [&prepared_local_source_build](
                    const xdg_directory_safety::DirectoryIdentity&
                        parent_identity) {
                    require_directory_identity_outside_local_source_tree(
                        prepared_local_source_build->source_root,
                        parent_identity.device,
                        parent_identity.inode);
                };
        }
        xdg_directory_safety::PreparedDirectory state_directory =
            xdg_directory_safety::prepare_directory(
                state_paths, state_creation_precondition);
        if(prepared_local_source_build.has_value()) {
            // Existing state directories do not enter the mkdir callback, so
            // pin their final identity before the fixed log name is opened.
            require_directory_identity_outside_local_source_tree(
                prepared_local_source_build->source_root,
                state_directory.device(), state_directory.inode());
        }
        xdg_state_log::PreparedLogFile state_log =
            xdg_state_log::open_default_state_log(
                state_paths, state_directory);
        Logger::init(
            std::move(state_log),
            // TRANSLATORS: The placeholders are the project name and version.
            localization::format_translated_message(
                "Started {} v{}.",
                application_identity::PROJECT_NAME,
                application_identity::VERSION));
        if(aur_update_diagnostic_capture.has_value()) {
            // POLICY(#270): executable upgrade-aur restores its existing
            // Started -> query -> execution trail only after persistent
            // logging is allowed.
            aur_update_diagnostic_capture->replay();
        }
    } catch(const LocalSourceWorkspaceError& error) {
        if(aur_update_diagnostic_capture.has_value()) {
            aur_update_diagnostic_capture->replay();
        }
        report_direct_error(
            local_source_workspace_failure_diagnostic(error.failure()));
        return 1;
    } catch(const std::exception& error) {
        if(aur_update_diagnostic_capture.has_value()) {
            aur_update_diagnostic_capture->replay();
        }
        // Default state authorityのfailureはoperation dispatch前にfatal。
        // Logger adoption後のwrite failureでも同じbackendへ再書込しない。
        report_direct_error(error.what());
        return 1;
    } catch(...) {
        if(aur_update_diagnostic_capture.has_value()) {
            aur_update_diagnostic_capture->replay();
        }
        // TRANSLATORS: The placeholder is the project name.
        const std::string diagnostic = localization::format_translated_message(
            "Cannot initialize the {} default state log because of an unknown error.",
            application_identity::PROJECT_NAME);
        report_direct_error(diagnostic);
        return 1;
    }

#ifdef MOGUET_ENABLE_APP_CONFIG_TEST_HOOKS
    // Test-only seam: closed-stdin regression cases need fd 0 to become
    // available again after production state-log initialization.
    const char* release_state_log =
        std::getenv("MOGUET_TEST_RELEASE_STATE_LOG_BEFORE_DISPATCH");
    if(release_state_log && release_state_log[0] != '\0') {
        try {
            Logger::shutdown();
        } catch(const std::exception& error) {
            // NO_TRANSLATE: The colored prefix and this branch are a test-only seam.
            std::cerr << "\033[1;31m:: Error:\033[0m " << error.what()
                      << std::endl;
            return 1;
        } catch(...) {
            // NO_TRANSLATE: Test-only hook failure, unreachable in production builds.
            std::cerr
                << "\033[1;31m:: Error:\033[0m Cannot release "
                << application_identity::PROJECT_NAME
                << " test state log: unknown error." << std::endl;
            return 1;
        }
    }
#endif

    const std::string& operation = parsed.operation;
    const std::vector<std::string>& args = parsed.ordered_pacman_args;
    const std::vector<std::string>& targets = parsed.targets;
    const std::vector<std::string>& flags = parsed.flags;

    const int operation_status = [&]() -> int {
        try {
            if(prepared_local_source_build.has_value()) {
                return cmd_build_local(
                    std::move(
                        prepared_local_source_build.value()),
                    g_config);
            }
            if(prepared_root_package_install.has_value()) {
                return execute_prepared_root_package_install(
                    std::move(
                        prepared_root_package_install.value()),
                    g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Build)
                   .token) {
                return cmd_build(targets, g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Upgrade)
                   .token) {
                return cmd_upgrade(g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::UpgradeAur)
                   .token) {
                if(!prepared_aur_update.has_value()) {
                    throw std::logic_error(
                        "Prepared AUR update operation is missing.");
                }
                return cmd_upgrade_aur(
                    std::move(*prepared_aur_update), g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::UpgradeAll)
                   .token) {
                return cmd_upgrade_all(g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Clean)
                   .token) {
                return cmd_clean(g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Deps)
                   .token) {
                return cmd_deps(targets, flags, g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Plan)
                   .token) {
                return cmd_plan(targets, flags, g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::Fetch)
                   .token) {
                return cmd_fetch(targets, flags, g_config);
            }
            if(operation ==
                   cli_authority::operation_spec(
                       cli_authority::OperationId::AddSource)
                       .token &&
               !targets.empty()) {
                return cmd_add_src(targets);
            }
            if(operation ==
                   cli_authority::operation_spec(
                       cli_authority::OperationId::DeleteSource)
                       .token &&
               !targets.empty()) {
                return cmd_del_src(targets);
            }
            if(operation ==
                   cli_authority::operation_spec(
                       cli_authority::OperationId::Revert)
                       .token &&
               !targets.empty()) {
                cmd_revert(targets, g_config);
                return 0;
            }
            if(operation ==
                   cli_authority::operation_spec(
                       cli_authority::OperationId::EditSource)
                       .token &&
               !targets.empty()) {
                return cmd_edit_src(targets, g_config);
            }
            if(operation ==
               cli_authority::operation_spec(
                   cli_authority::OperationId::ListSources)
                   .token) {
                cmd_list_src();
                return 0;
            }

            if(prepared_system_aur_update.has_value()) {
                return cmd_system_aur_update(
                    std::move(prepared_system_aur_update.value()),
                    g_config);
            }
            if(const auto* repo_only = std::get_if<
                   RepoOnlySystemUpdateRouteCandidate>(
                   &sync_invocation_route)) {
                return execute_ordered_repository_sync_transaction(
                    repo_only->ordered_pacman_args, g_config);
            }

            bool requests_refresh = pacman_operation_requests_refresh(operation, flags);
            bool is_sync = operation.starts_with("-S");
            bool is_query = operation.starts_with("-Q");
            bool is_foreign_updates = (is_query && operation.find('u') != std::string::npos && operation.find('a') != std::string::npos);
            SourceSelectableSyncOperation selected_sync_operation = source_selectable_sync_operation(parsed);
            bool is_search = parsed.source_selection == PackageSourceSelection::Auto
                                 ? (is_sync && operation.find('s') != std::string::npos)
                                 : selected_sync_operation == SourceSelectableSyncOperation::Search;
            bool is_info = parsed.source_selection == PackageSourceSelection::Auto
                               ? (is_sync && operation.find('i') != std::string::npos)
                               : selected_sync_operation == SourceSelectableSyncOperation::Info;
            bool is_clean = (is_sync && operation.find('c') != std::string::npos);
            bool is_sys_upgrade = (is_sync && (operation.find('u') != std::string::npos || operation.find('y') != std::string::npos));
            bool needs_sudo =
                is_sync || operation.starts_with("-R") || operation.starts_with("-U") ||
                operation.starts_with("-D") || (operation.starts_with("-F") && requests_refresh);

            if(is_foreign_updates) return cmd_query_foreign_updates();

            if(is_search) {
                return cmd_sync_search(
                    parsed, requests_refresh, parsed.source_selection, g_config);
            }
            if(is_info) {
                return cmd_sync_info(
                    parsed, requests_refresh, parsed.source_selection, g_config);
            }
            if(is_clean) return run_command("sudo pacman " + join_pacman_args(args));

            if(is_sync) {
                return cmd_sync_install(
                    parsed, is_sys_upgrade, parsed.source_selection, g_config);
            }
            std::string cmd_prefix = needs_sudo ? "sudo pacman " : "pacman ";
            return run_command(cmd_prefix + join_pacman_args(args));
        } catch(const ConfirmationOperationStopped&) {
            return 1;
        } catch(const std::exception& e) {
            try {
                Logger::error(e.what());
            } catch(...) {
                // A pending checked state-log failure is reported by shutdown().
            }
            return 1;
        }
    }();

    try {
        Logger::shutdown();
    } catch(const std::exception& error) {
        report_direct_error(error.what());
        return 1;
    } catch(...) {
        // TRANSLATORS: The placeholder is the project name.
        const std::string diagnostic = localization::format_translated_message(
            "Cannot finalize the {} default state log because of an unknown error.",
            application_identity::PROJECT_NAME);
        report_direct_error(diagnostic);
        return 1;
    }
    return operation_status;
}

#ifdef MOGUET_ENABLE_APP_CONFIG_TEST_HOOKS
namespace {

int verify_parse_failure_does_not_publish_cli_overrides() {
    char program[] = "moguet";
    char no_edit[] = "--noedit";
    char no_diff[] = "--nodiff";
    char no_confirm[] = "--noconfirm";
    char build_mode[] = "--build-mode=rebuild";
    char rebuild_alias[] = "--rebuild";
    char rm_deps[] = "--rmdeps";
    char operation[] = "-Q";
    char missing_value_option[] = "--config";
    std::array<char*, 9> parse_failure_argv = {
        program, no_edit, no_diff, no_confirm, build_mode,
        rebuild_alias, rm_deps, operation, missing_value_option};

    if(run_moguet(static_cast<int>(parse_failure_argv.size()), parse_failure_argv.data()) != 1) {
        // NO_TRANSLATE: Test-hook assertion diagnostic, unreachable in production builds.
        std::cerr << "Expected CLI parse failure." << std::endl;
        return 1;
    }
    if(g_config.user_config.review.pkgbuild != ReviewPolicy::Prompt ||
       g_config.user_config.review.diff != ReviewPolicy::Prompt ||
       g_config.user_config.build.mode != BuildMode::Normal ||
       g_config.no_confirm || g_config.rm_deps) {
        // NO_TRANSLATE: Test-hook assertion diagnostic, unreachable in production builds.
        std::cerr << "CLI parse failure published partial config overrides." << std::endl;
        return 1;
    }
    return 0;
}

} // namespace
#endif

int main(int argc, char* argv[]) {
    if(!localization::initialize_runtime_catalog()) {
        // NO_TRANSLATE: The message catalog is unavailable, so this bootstrap failure cannot be translated.
        Logger::error("Failed to initialize the Moguet message catalog.");
        return 1;
    }
#ifdef MOGUET_ENABLE_SYSTEM_AUR_UPDATE_PRESENTATION_TEST_HOOKS
    const char* system_aur_presentation_case =
        std::getenv("MOGUET_TEST_SYSTEM_AUR_PRESENTATION_CASE");
    if(system_aur_presentation_case &&
       system_aur_presentation_case[0] != '\0') {
        return run_system_aur_update_presentation_test(
            system_aur_presentation_case);
    }
#endif
#ifdef MOGUET_ENABLE_APP_CONFIG_TEST_HOOKS
    const char* app_config_test_case = std::getenv("MOGUET_TEST_APP_CONFIG_CASE");
    if(app_config_test_case && std::string(app_config_test_case) == "parse-failure-cli-overrides") {
        return verify_parse_failure_does_not_publish_cli_overrides();
    }
#endif
    return run_moguet(argc, argv);
}

// --- 関数実装 ---

// CLI入口 / help
void print_help() {
    using cli_authority::OperationId;
    using cli_authority::OptionId;
    using cli_authority::SpecialOperationId;

    // NO_TRANSLATE: Product/version identity and CLI grammar tokens are
    // locale-independent; surrounding headings and descriptions are msgids.
    std::cout << "\033[1;36m" << application_identity::PROJECT_NAME
              << "\033[0m v"
              << application_identity::VERSION << "\n"
              << std::endl;
    print_help_section(localization::translate_message("USAGE"));
    std::cout << "    " << application_identity::COMMAND_NAME
              << " <op> [options] [targets...]\n"
              << std::endl;
    print_help_section(localization::translate_message("OPERATIONS"));
    print_help_entry(
        cli_operation_syntax(OperationId::Build),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal PKGBUILD artifact identity.
            "Build one remote package or local {} root without saving a preference",
            "PKGBUILD"));
    print_help_entry(
        cli_operation_syntax(OperationId::Upgrade),
        localization::translate_message(
            "Run the source-aware system update and apply saved source-build preferences"));
    print_help_continuation(
        localization::translate_message(
            "Update repository packages before rebuilding configured source packages"));
    print_help_entry(
        cli_operation_syntax(OperationId::UpgradeAur),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Update installed {} packages only", "AUR"));
    print_help_continuation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Do not update repository packages; apply saved source-build preferences for matching {} packages",
            "AUR"));
    print_help_entry(
        cli_operation_syntax(OperationId::UpgradeAll),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Update the system, registered source packages, and remaining installed {} packages",
            "AUR"));
    print_help_continuation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal PackageBase identity.
            "Give explicit source preferences priority and avoid duplicate package/{} builds",
            "PackageBase"));
    print_help_continuation(localization::translate_message(
        "Apply saved source-build preferences as a source-aware workflow"));
    print_help_continuation(localization::translate_message(
        "Do not accept target operands"));
    print_help_entry(
        cli_operation_syntax(OperationId::Clean),
        localization::translate_message("Clean package and build caches"));
    std::cout << std::endl;
    print_help_section(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "{} INSPECTION", "AUR"));
    print_help_entry(
        cli_special_operation_syntax(
            SpecialOperationId::PkgbuildExport),
        localization::format_translated_message(
            // TRANSLATORS: AUR and PackageBase are literal identities.
            "Export one {} {} repository under the command-start current directory or selected output parent without building or installing",
            "AUR", "PackageBase"));
    print_help_entry(
        cli_special_operation_syntax(
            SpecialOperationId::PkgbuildPrint),
        localization::format_translated_message(
            // TRANSLATORS: AUR, PackageBase, PKGBUILD, and stdout are literal identities.
            "Print one {} {} {} to {} without keeping a checkout",
            "AUR", "PackageBase", "PKGBUILD", "stdout"));
    print_help_entry(
        cli_operation_syntax(OperationId::Deps),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Classify {} dependencies and show constraint and conflict/replacement assessments",
            "AUR"));
    print_help_entry(
        cli_operation_syntax(OperationId::Plan),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Show the {} build-order plan, constraint completeness, and conflict/replacement assessments",
            "AUR"));
    print_help_entry(
        cli_operation_syntax(OperationId::Fetch),
        localization::format_translated_message(
            // TRANSLATORS: The placeholders are literal git commands and the AUR identity.
            "Safely run {} or {} for {} build repositories",
            "git clone", "git fetch", "AUR"));
    print_help_continuation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholders are literal git commands.
            "For existing clones, run {} only; do not update the working tree or run {}, {}, build, or install",
            "git fetch", "git pull", "git reset"));
    std::cout << std::endl;
    print_help_section(
        localization::translate_message("SOURCE BUILD PREFERENCES"));
    print_help_entry(
        "${XDG_CONFIG_HOME:-$HOME/.config}/moguet/source-build.d/<package-name>",
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal XDG identity.
            "Store each preference in the active user's {} config authority",
            "XDG"));
    print_help_entry(
        cli_operation_syntax(OperationId::AddSource),
        localization::translate_message(
            "Enable source-build preferences for one or more items"));
    print_help_entry(
        cli_operation_syntax(OperationId::EditSource),
        localization::translate_message(
            "Edit one or more source-build preferences"));
    print_help_entry(
        cli_operation_syntax(OperationId::ListSources),
        localization::translate_message(
            "List source-build preferences"));
    print_help_entry(
        cli_operation_syntax(OperationId::DeleteSource),
        localization::translate_message(
            "Remove one or more source-build preferences"));
    print_help_entry(
        cli_operation_syntax(OperationId::Revert),
        localization::translate_message(
            "Remove preferences and reinstall binary packages"));
    std::cout << std::endl;
    print_help_section(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal pacman program identity.
            "{} COMPATIBILITY", "pacman"));
    print_help_entry(
        cli_authority::PACMAN_SYNC_INSTALL_SYNTAX,
        localization::translate_message("Install packages"));
    print_help_entry(
        cli_special_operation_syntax(SpecialOperationId::SyncSelect),
        localization::translate_message(
            "Interactively search for and select root packages to install"));
    print_help_continuation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Require an interactive terminal; install selected repository roots first, then build selected {} roots only if that transaction succeeds",
            "AUR"));
    for(const bool refresh : {true, false}) {
        const auto aur_operation = refresh ? SpecialOperationId::SystemAurUpdate : SpecialOperationId::SystemAurUpdateNoRefresh;
        const auto repository_operation = refresh ? SpecialOperationId::SystemRepositoryUpdate : SpecialOperationId::SystemRepositoryUpdateNoRefresh;
        print_help_entry(
            cli_special_operation_syntax(
                aur_operation),
            localization::format_translated_message(
                // TRANSLATORS: The placeholder is the AUR project identity.
                "Upgrade official repository packages and normal installed {} packages",
                "AUR"));
        print_help_continuation(
            localization::translate_message(
                "Do not read or apply saved source-build preferences"));
        print_help_continuation(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Run repository and {} phases sequentially; a later failure does not roll back the repository upgrade",
            "AUR"));
        print_help_entry(
            cli_special_operation_syntax(
                repository_operation),
            localization::translate_message(
                "Run the repository system upgrade only"));
        print_help_continuation(localization::format_translated_message(
            // TRANSLATORS: The placeholder is the pacman program identity.
            "Allow the full {}-compatible repository argument tail", "pacman"));
        if(!refresh) {
            print_help_continuation(localization::translate_message(
                "Use the current sync databases without refreshing them"));
        }
    }
    print_help_entry(
        cli_authority::PACMAN_SYNC_SEARCH_SYNTAX,
        localization::translate_message("Search for packages"));
    print_help_entry(
        cli_authority::PACMAN_SYNC_INFO_SYNTAX,
        localization::translate_message("Show package information"));
    print_help_entry(
        cli_authority::PACMAN_FOREIGN_UPDATES_SYNTAX,
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Check {} and foreign-package updates", "AUR"));
    std::cout << std::endl;
    print_help_section(localization::translate_message("OPTIONS"));
    print_help_entry(
        cli_option_syntax(OptionId::Details),
        localization::translate_message(
            "Show detailed diagnostic and provenance information"));
    print_help_continuation(localization::format_translated_message(
        // TRANSLATORS: The placeholders are literal supported CLI forms.
        "For {}, {}, and {}; changes presentation only, not execution",
        "plan", "deps", "-S --select"));
    print_help_entry(
        cli_option_syntax(OptionId::Help),
        localization::translate_message(
            "Show this help message and exit"));
    print_help_entry(
        cli_option_syntax(OptionId::Version),
        localization::translate_message(
            "Show version information and exit"));
    print_help_entry(
        cli_option_syntax(OptionId::DryRun),
        localization::translate_message(
            "Observe supported mutating operations without changing persistent state"));
    print_help_continuation(localization::translate_message(
        "Reject unsupported routes and do not create state, cache, or workspaces"));
    print_help_continuation(localization::translate_message(
        "Show assessed conflict/replacement blockers before any supported mutation"));
    print_help_continuation(localization::format_translated_message(
        // TRANSLATORS: The placeholders are the literal -Syu / -Su tokens and AUR project identity.
        "For {}, assess the current installed {} state; actual execution re-evaluates it after the repository upgrade succeeds",
        "-Syu / -Su", "AUR"));
    print_help_entry(
        cli_option_syntax(OptionId::Edit),
        localization::format_translated_message(
            // TRANSLATORS: PKGBUILD and .install are literal artifact names.
            "Open or edit {} and {} files for this invocation",
            "PKGBUILD", ".install"));
    print_help_entry(
        cli_option_syntax(OptionId::NoEdit),
        localization::format_translated_message(
            // TRANSLATORS: PKGBUILD and .install are literal artifact names.
            "Skip invocation-local {} and {} editing",
            "PKGBUILD", ".install"));
    print_help_entry(
        cli_option_syntax(OptionId::Diff),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Review repository updates; for {}, review the exact target",
            "AUR"));
    print_help_continuation(localization::translate_message(
        "Use the previous reviewed revision, or all tracked source on first review"));
    print_help_continuation(localization::translate_message(
        "Advance reviewed state only after explicit acceptance"));
    print_help_entry(
        cli_option_syntax(OptionId::NoDiff),
        localization::translate_message(
            "Skip repository diff / reviewed-source review without advancing reviewed state"));
    print_help_entry(
        cli_option_syntax(OptionId::NoConfirm),
        localization::format_translated_message(
            // TRANSLATORS: pacman and makepkg are literal external-program identities.
            "Pass this option to {} and {}", "pacman", "makepkg"));
    print_help_continuation(localization::translate_message(
        "Do not bypass conflict/replacement safety stops or perform automatic replacement"));
    print_help_entry(
        cli_option_syntax(OptionId::PkgbuildOutputDirectory),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal -G CLI operation.
            "Select an existing export parent for {}; resolve relative paths from the command-start current directory",
            "-G"));
    print_help_continuation(localization::translate_message(
        "Reject symlink components and do not create the parent directory"));
    print_help_entry(
        cli_option_syntax(OptionId::Needed),
        localization::format_translated_message(
            // TRANSLATORS: pacman, AUR, and -S are literal identities or CLI tokens.
            "Pass this option to {}; on {} or source-build {} routes, apply it only to final installation",
            "pacman", "AUR", "-S"));
    print_help_continuation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal Moguet project identity.
            "Do not skip {} build, review, or plan steps",
            application_identity::PROJECT_NAME));
    print_help_entry(
        cli_option_syntax(OptionId::BuildMode),
        localization::translate_message("Select the source-build mode"));
    print_help_entry(
        cli_option_syntax(OptionId::Rebuild),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is a literal compatibility option form.
            "Compatibility alias for {}",
            cli_authority::BUILD_MODE_REBUILD_OPTION));
    print_help_entry(
        cli_option_syntax(OptionId::CleanBuild),
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is a literal compatibility option form.
            "Compatibility alias for {}",
            cli_authority::BUILD_MODE_CLEAN_OPTION));
    print_help_entry(
        cli_option_syntax(OptionId::RmDeps),
        localization::translate_message(
            "Unsupported for separated source builds; no dependency cleanup is performed"));
    print_help_entry(
        cli_option_syntax(OptionId::Aur),
        localization::format_translated_message(
            // TRANSLATORS: -S, -Ss, -Si, and AUR are literal CLI/project identities.
            "Limit {}, {}, and {} to {}; do not fall back to repositories",
            "-S", "-Ss", "-Si", "AUR"));
    print_help_continuation(localization::format_translated_message(
        // TRANSLATORS: The placeholders are literal -Syu / -Su, upgrade-aur, and AUR identities.
        "Do not use with {}; use {} for an {}-only source-aware update",
        "-Syu / -Su", "upgrade-aur", "AUR"));
    print_help_entry(
        cli_option_syntax(OptionId::Repo),
        localization::format_translated_message(
            // TRANSLATORS: -S, -Ss, -Si, -Syu / -Su, and AUR are literal CLI/project identities.
            "Limit {}, {}, and {} to official binary repositories; with {}, run the repository system upgrade only; do not use {} or source builds",
            "-S", "-Ss", "-Si", "-Syu / -Su", "AUR"));
    std::cout << std::endl;
    print_help_section(localization::translate_message("CONFIGURATION"));
    print_help_entry(
        "$XDG_CONFIG_HOME/moguet/config.toml",
        localization::translate_message(
            "Read-only typed configuration; a missing file uses built-in defaults"));
    print_help_entry(
        "schema_version = 1",
        localization::translate_message("Required schema version"));
    print_help_entry(
        "review.pkgbuild = \"prompt\"|\"skip\"",
        localization::format_translated_message(
            // TRANSLATORS: PKGBUILD and .install are literal artifact names.
            "Invocation-local {} / {} editor policy; not reviewed-source acceptance",
            "PKGBUILD", ".install"));
    print_help_entry(
        "review.diff = \"prompt\"|\"skip\"",
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the AUR project identity.
            "Repository diff / {} reviewed-source review policy; skipping does not advance reviewed state",
            "AUR"));
    print_help_entry(
        "build.mode = \"normal\"|\"rebuild\"|\"clean\"",
        localization::translate_message("Source-build mode"));
}

bool argv_requests_pkgbuild_export_diagnostics(int argc, char* argv[]) {
    // POLICY(#173): operation 前のglobal optionだけを飛ばし、option value/opaque operandは見ない。
    for(int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if(is_moguet_global_option(arg)) continue;
        return arg == cli_authority::PKGBUILD_EXPORT_OPERATION ||
               arg == cli_authority::PKGBUILD_PRINT_OPERATION;
    }
    return false;
}

bool handle_info_only_option(int argc, char* argv[]) {
    for(int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if(is_moguet_global_option(arg)) continue;
        if(arg == cli_authority::HELP_SHORT_OPTION ||
           arg == cli_authority::HELP_LONG_OPTION) {
            print_help();
            return true;
        }
        if(arg == cli_authority::VERSION_SHORT_OPTION ||
           arg == cli_authority::VERSION_LONG_OPTION) {
            // NO_TRANSLATE: Product/version identity is locale-independent.
            std::cout << application_identity::PROJECT_NAME << " v"
                      << application_identity::VERSION << std::endl;
            return true;
        }
        // POLICY(#173): help/versionはoperation位置だけで扱い、option valueやopaque operandを横取りしない。
        return false;
    }
    return false;
}

// shell引数 / command construction
std::vector<std::string> pacman_args_with_global_options(std::vector<std::string> args) {
    if(g_config.no_confirm) {
        // POLICY(#173): generated optionはoperation直後へ置き、semantic `--`やoption valueを再解釈しない。
        // 認識済みglobal tokenはordered viewから除外済みなので、ここでは常に1件だけ生成する。
        if(args.empty())
            args.push_back("--noconfirm");
        else
            args.insert(args.begin() + 1, "--noconfirm");
    }
    return args;
}

std::string join_pacman_args(const std::vector<std::string>& args) {
    return shell_words::join(pacman_args_with_global_options(args));
}

// pacman / repository補助
bool validate_optionless_moguet_operation(const std::string& operation, const std::vector<std::string>& flags) {
    for(const auto& flag : flags) {
        if(flag == operation) continue;
        // POLICY(#335): target-bearing source-preference operations alone use
        // semantic `--` to pass a leading-hyphen operand to package validation.
        if(flag == "--" &&
           is_source_preference_target_operation(operation)) {
            continue;
        }

        // TRANSLATORS: The placeholders are literal CLI operation and option tokens.
        Logger::error(localization::format_translated_message(
            "Unsupported {} option: {}", operation, flag));
        return false;
    }
    return true;
}
