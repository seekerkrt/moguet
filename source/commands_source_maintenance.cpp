#include "commands_source_maintenance.hpp"

#include "application_identity.hpp"
#include "app_config.hpp"
#include "cache_authority.hpp"
#include "cli_authority.hpp"
#include "cli_runtime_contract.hpp"
#include "diagnostic_projection.hpp"
#include "interactive_confirmation.hpp"
#include "local_source_build.hpp"
#include "local_source_install.hpp"
#include "local_source_metadata_evaluation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "runtime_diagnostic.hpp"
#include "package_metadata.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "repository_query.hpp"
#include "reviewed_source_production_outcome.hpp"
#include "separated_source_build.hpp"
#include "shell_words.hpp"
#include "source_environment.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"
#include "system_source_upgrade.hpp"
#include "trusted_cache.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool same_temporary_file_state(
    const struct stat& expected, const struct stat& actual) {
    return expected.st_dev == actual.st_dev &&
           expected.st_ino == actual.st_ino &&
           (expected.st_mode & S_IFMT) == (actual.st_mode & S_IFMT) &&
           expected.st_uid == actual.st_uid &&
           (expected.st_mode & 07777) == (actual.st_mode & 07777) &&
           expected.st_size == actual.st_size &&
           expected.st_mtim.tv_sec == actual.st_mtim.tv_sec &&
           expected.st_mtim.tv_nsec == actual.st_mtim.tv_nsec &&
           expected.st_ctim.tv_sec == actual.st_ctim.tv_sec &&
           expected.st_ctim.tv_nsec == actual.st_ctim.tv_nsec;
}

bool is_safe_command_token(const std::string& token) {
    if(token.empty()) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' || ch == '+' || ch == '-' || ch == '=' ||
               ch == ':' || ch == '@' || ch == '%';
    });
}

std::vector<std::string> split_command_words(const std::string& command) {
    std::stringstream ss(command);
    std::string word;
    std::vector<std::string> words;
    while(ss >> word) {
        if(!is_safe_command_token(word)) {
            // TRANSLATORS: The placeholder is an editor command token.
            throw std::runtime_error(localization::format_translated_message(
                "Unsafe command token: {}", word));
        }
        words.push_back(word);
    }
    if(words.empty()) {
        throw std::runtime_error(localization::translate_message(
            "Editor command is empty."));
    }
    return words;
}

std::vector<std::string> pacman_args_with_global_options(
    std::vector<std::string> args,
    const AppConfig& config) {
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
    const std::vector<std::string>& args,
    const AppConfig& config) {
    return shell_words::join(pacman_args_with_global_options(args, config));
}

std::string build_editor_command(const std::string& editor, const fs::path& target) {
    std::vector<std::string> args = split_command_words(editor);
    args.push_back(target.string());
    return shell_words::join(args);
}

void require_valid_package_targets(
    const std::vector<std::string>& targets) {
    for(const std::string& target : targets) {
        require_valid_package_name(target);
    }
}

void require_valid_add_src_packages(
    const std::vector<std::string>& args) {
    for(const std::string& arg : args) {
        if(arg.find('=') == std::string::npos) {
            require_valid_package_name(arg);
        }
    }
}

void report_confirmation_stop(
    const ConfirmationResult& result,
    DiagnosticOperation operation,
    DiagnosticPhase phase,
    DiagnosticIdentity identity = {}) {
    report_runtime_diagnostic(
        project_confirmation_diagnostic(
            result, operation, phase, std::move(identity)),
        confirmation_stop_diagnostic(result));
}

[[noreturn]] void stop_after_confirmation(
    ConfirmationResult result,
    DiagnosticOperation operation,
    DiagnosticPhase phase,
    DiagnosticIdentity identity = {}) {
    report_confirmation_stop(result, operation, phase, std::move(identity));
    throw ConfirmationOperationStopped(std::move(result));
}

void present_system_source_upgrade_event(
    const SystemSourceUpgradeEvent& event) {
    switch(event.kind) {
        case SystemSourceUpgradeEventKind::LoadingSourcePreference:
            if(!event.entry_path.has_value()) {
                throw std::logic_error(
                    localization::translate_message(
                        "Source preference load event has no entry path."));
            }
            // TRANSLATORS: The placeholder is a source preference file path.
            Logger::info(localization::format_translated_message(
                "Loading custom build flags from {}.",
                event.entry_path->string()));
            return;
        case SystemSourceUpgradeEventKind::SourcePreferenceWarning:
        case SystemSourceUpgradeEventKind::InvalidPreferenceWarning:
            Logger::warn(event.diagnostic);
            return;
        case SystemSourceUpgradeEventKind::SystemUpgradeStarting:
            Logger::info(localization::translate_message(
                "System upgrade..."));
            return;
        case SystemSourceUpgradeEventKind::CheckingSourcePackages:
            Logger::info(localization::translate_message(
                "Checking source packages..."));
            return;
    }
    throw std::logic_error(localization::translate_message(
        "Unknown system/source upgrade event kind."));
}

bool should_present_registered_package_base_result(
    const RegisteredSourceUpgradeResult& source) noexcept {
    if(!source.package_base_execution.has_value()) return false;
    const RegisteredSourcePackageBaseExecutionSnapshot& execution =
        *source.package_base_execution;
    return execution.package_base != source.preference_package_name ||
           !execution.unselected_artifacts.empty();
}

void present_registered_package_base_result(
    const RegisteredSourceUpgradeResult& source) {
    if(!should_present_registered_package_base_result(source)) return;
    const RegisteredSourcePackageBaseExecutionSnapshot& execution =
        *source.package_base_execution;

    // TRANSLATORS: The placeholders are the PackageBase field identity and a PackageBase name.
    Logger::info(localization::format_translated_message(
        "{} result: {}", "PackageBase", execution.package_base));
    // TRANSLATORS: The placeholders are the requested package, produced package, and full version.
    Logger::info(localization::format_translated_message(
        "  required child: {} -> {} {} (explicit): installed",
        source.preference_package_name,
        execution.selected_child.identity.package_name,
        execution.selected_child.identity.full_version));
    for(const ArtifactPackageIdentity& artifact :
        execution.unselected_artifacts) {
        // TRANSLATORS: The placeholders are a produced package name and full version.
        Logger::info(localization::format_translated_message(
            "  produced artifact: {} {} (not selected; not installed)",
            artifact.package_name, artifact.full_version));
    }
}

void present_registered_package_base_results(
    const SystemSourceUpgradeResult& result) {
    for(const RegisteredSourceUpgradeResult& source :
        result.registered_source_results) {
        present_registered_package_base_result(source);
    }
}

void present_registered_reviewed_source_outcomes(
    const SystemSourceUpgradeResult& result) {
    for(const RegisteredSourceUpgradeResult& source :
        result.registered_source_results) {
        if(!source.production_outcome.has_value()) continue;
        const std::string& package_base =
            source.resolved_package_base.has_value()
                ? *source.resolved_package_base
                : source.preference_package_name;
        ReviewedSourceProductionOutcomePresentation presentation =
            format_production_source_build_staged_outcome(
                package_base, *source.production_outcome);
        for(const std::string& line : presentation.info_lines) {
            Logger::info(line);
        }
    }
}

[[noreturn]] void throw_system_source_upgrade_failure(
    const SystemSourceUpgradeResult& result) {
    std::optional<std::string> diagnostic = result.failure_diagnostic();
    if(diagnostic.has_value()) throw std::runtime_error(*diagnostic);
    throw std::logic_error(
        localization::translate_message(
            "System/source upgrade stopped without a failure diagnostic."));
}

std::string local_source_root_failure_diagnostic(
    const LocalSourceRootFailure& failure) {
    const std::string path = failure.path.string();
    switch(failure.code) {
        case LocalSourceRootErrorCode::InvalidInputPath:
            return localization::format_translated_message(
                "Invalid local source root path: {}", path);
        case LocalSourceRootErrorCode::Missing:
            return localization::format_translated_message(
                "Local source root entry is missing: {}", path);
        case LocalSourceRootErrorCode::Symlink:
            return localization::format_translated_message(
                "Local source root entry must not be a symlink: {}", path);
        case LocalSourceRootErrorCode::NotDirectory:
            return localization::format_translated_message(
                "Local source root is not a directory: {}", path);
        case LocalSourceRootErrorCode::NotRegularFile:
            return localization::format_translated_message(
                "Local source entry is not a regular file: {}", path);
        case LocalSourceRootErrorCode::OwnershipMismatch:
            return localization::format_translated_message(
                "Local source entry is not owned by the current user: {}",
                path);
        case LocalSourceRootErrorCode::UnsafePermissions:
            return localization::format_translated_message(
                "Local source entry is writable by its group or others: {}",
                path);
        case LocalSourceRootErrorCode::PermissionDenied:
            return localization::format_translated_message(
                "Permission was denied while inspecting local source entry: {}",
                path);
        case LocalSourceRootErrorCode::ReadFailure:
            return localization::format_translated_message(
                "Failed to read local source entry: {}", path);
        case LocalSourceRootErrorCode::ConcurrentReplacement:
            return localization::format_translated_message(
                "Local source entry changed identity during the operation: {}",
                path);
        case LocalSourceRootErrorCode::ContentChanged:
            return localization::format_translated_message(
                "Local source entry content changed during the operation: {}",
                path);
        case LocalSourceRootErrorCode::UnsafeMetadata:
            return localization::format_translated_message(
                "Local source metadata is unsafe and cannot be evaluated: {}",
                path);
        case LocalSourceRootErrorCode::MetadataFailure:
            return localization::format_translated_message(
                "Failed to validate local source metadata: {}", path);
    }
    return localization::format_translated_message(
        "Failed to validate local source root: {}", path);
}

struct ReviewedLocalSourceRoot {
    LocalSourceRoot source_root;
    bool pkgbuild_changed = false;
};

ReviewedLocalSourceRoot review_local_source_root(
    LocalSourceRoot source_root,
    bool has_one_off_environment_assignment,
    const AppConfig& config) {
    // TRANSLATORS: The placeholder is a canonical local source directory.
    Logger::info(localization::format_translated_message(
        "Local source root: {}", source_root.canonical_path().string()));

    if(config.user_config.review.pkgbuild == ReviewPolicy::Skip) {
        // TRANSLATORS: The placeholders are the literal PKGBUILD artifact and --noedit option.
        Logger::info(localization::format_translated_message(
            "Skipping {} review ({}).", "PKGBUILD", "--noedit"));
        source_root.require_unchanged_identity();
        return ReviewedLocalSourceRoot{std::move(source_root), false};
    }

    const fs::path pkgbuild_path =
        source_root.canonical_path() / "PKGBUILD";
    // TRANSLATORS: The placeholder is an absolute PKGBUILD path.
    Logger::info(localization::format_translated_message(
        "Review target: {}", pkgbuild_path.string()));
    ConfirmationResult edit_confirmation = request_confirmation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholder is the literal PKGBUILD artifact name.
            "Edit {}?", "PKGBUILD"),
        ConfirmationDefault::No, config.no_confirm);
    if(std::holds_alternative<ConfirmationDeclined>(
           edit_confirmation)) {
        source_root.require_unchanged_identity();
        return ReviewedLocalSourceRoot{std::move(source_root), false};
    }
    if(!std::holds_alternative<ConfirmationAccepted>(
           edit_confirmation)) {
        DiagnosticIdentity identity;
        identity.source_kind = DiagnosticSourceKind::Local;
        identity.local_root = source_root.canonical_path();
        stop_after_confirmation(
            std::move(edit_confirmation), DiagnosticOperation::Build,
            DiagnosticPhase::Preflight, std::move(identity));
    }

    const LocalSourceDirectoryIdentity expected_directory =
        source_root.directory_identity();
    const LocalSourceFileSnapshot expected_pkgbuild = source_root.pkgbuild();
    if(run_command(build_editor_command(config.editor, pkgbuild_path)) != 0) {
        throw std::runtime_error(localization::translate_message(
            "Editor failed."));
    }

    LocalSourceRoot reviewed = open_local_source_root(
        source_root.canonical_path(),
        has_one_off_environment_assignment);
    if(reviewed.directory_identity() != expected_directory) {
        // TRANSLATORS: The placeholder is the literal PKGBUILD artifact name.
        throw std::runtime_error(localization::format_translated_message(
            "Local source root changed identity while reviewing {}.",
            "PKGBUILD"));
    }
    const bool pkgbuild_changed = reviewed.pkgbuild() != expected_pkgbuild;
    ConfirmationResult proceed_confirmation = request_confirmation(
        localization::translate_message("Proceed with build?"),
        ConfirmationDefault::Yes, config.no_confirm);
    if(!std::holds_alternative<ConfirmationAccepted>(
           proceed_confirmation)) {
        DiagnosticIdentity identity;
        identity.source_kind = DiagnosticSourceKind::Local;
        identity.local_root = reviewed.canonical_path();
        stop_after_confirmation(
            std::move(proceed_confirmation),
            DiagnosticOperation::Build, DiagnosticPhase::Preflight,
            std::move(identity));
    }
    return ReviewedLocalSourceRoot{
        std::move(reviewed), pkgbuild_changed};
}

std::string local_metadata_evaluation_reason(
    const LocalSourceMetadataSnapshot& metadata,
    bool pkgbuild_changed) {
    if(pkgbuild_changed) {
        // TRANSLATORS: The placeholder is the literal PKGBUILD artifact name.
        return localization::format_translated_message(
            "{} changed during review", "PKGBUILD");
    }
    switch(metadata.state()) {
        case LocalSourceMetadataState::Missing:
            // TRANSLATORS: The placeholder is the literal .SRCINFO artifact name.
            return localization::format_translated_message(
                "{} is missing", ".SRCINFO");
        case LocalSourceMetadataState::Invalid:
            // TRANSLATORS: The placeholder is the literal .SRCINFO artifact name.
            return localization::format_translated_message(
                "{} is invalid", ".SRCINFO");
        case LocalSourceMetadataState::KnownStale:
            if(std::find(
                   metadata.stale_reasons().begin(),
                   metadata.stale_reasons().end(),
                   LocalSourceMetadataStaleReason::
                       OneOffEnvironmentAssignment) !=
               metadata.stale_reasons().end()) {
                // TRANSLATORS: The placeholder is the literal .SRCINFO artifact name.
                return localization::format_translated_message(
                    "{} does not represent the one-off environment",
                    ".SRCINFO");
            }
            // TRANSLATORS: The placeholders are the literal .SRCINFO and PKGBUILD artifact names.
            return localization::format_translated_message(
                "{} is older than {}", ".SRCINFO", "PKGBUILD");
        case LocalSourceMetadataState::Unsafe:
            // TRANSLATORS: The placeholder is the literal .SRCINFO artifact name.
            return localization::format_translated_message(
                "{} is unsafe", ".SRCINFO");
        case LocalSourceMetadataState::UsableUnverified:
            // TRANSLATORS: The placeholder is the literal .SRCINFO artifact name.
            return localization::format_translated_message(
                "{} is usable but unverified", ".SRCINFO");
    }
    return localization::translate_message(
        "local metadata requires evaluation");
}

LocalSourceBuildMetadata accept_local_source_build_metadata(
    const LocalSourceRoot& source_root,
    SourceBuildEnvironment source_environment,
    std::string effective_architecture,
    bool pkgbuild_changed,
    const AppConfig& config) {
    const LocalSourceMetadataSnapshot& metadata = source_root.metadata();
    if(metadata.state() == LocalSourceMetadataState::Unsafe) {
        if(metadata.unsafe_failure() != nullptr) {
            throw std::runtime_error(local_source_root_failure_diagnostic(
                *metadata.unsafe_failure()));
        }
        throw std::runtime_error(localization::translate_message(
            "Local source metadata is unsafe and cannot be evaluated."));
    }
    if(metadata.state() == LocalSourceMetadataState::UsableUnverified &&
       !pkgbuild_changed) {
        return bind_existing_local_source_metadata(
            source_root, std::move(effective_architecture));
    }

    const std::string reason =
        local_metadata_evaluation_reason(metadata, pkgbuild_changed);
    // TRANSLATORS: The placeholders are a canonical local source directory and a reason metadata evaluation is required.
    Logger::warn(localization::format_translated_message(
        "Local metadata evaluation is required for {}: {}.",
        source_root.canonical_path().string(), reason));
    ConfirmationResult evaluation_confirmation = request_confirmation(
        localization::format_translated_message(
            // TRANSLATORS: The placeholders are the literal PKGBUILD artifact name and makepkg option.
            "Evaluate {} metadata with {}?", "PKGBUILD",
            "makepkg --printsrcinfo"),
        ConfirmationDefault::None, config.no_confirm);
    if(!std::holds_alternative<ConfirmationAccepted>(
           evaluation_confirmation)) {
        DiagnosticIdentity identity;
        identity.source_kind = DiagnosticSourceKind::Local;
        identity.local_root = source_root.canonical_path();
        stop_after_confirmation(
            std::move(evaluation_confirmation),
            DiagnosticOperation::Build, DiagnosticPhase::Metadata,
            std::move(identity));
    }
    return evaluate_local_source_metadata(
        source_root, std::move(source_environment),
        std::move(effective_architecture));
}

void require_complete_local_build_plan(const LocalBuildPlan& plan) {
    if(plan.failures().empty()) {
        require_executable_build_plan(
            plan.local_metadata().package_base, plan.build_plan());
        return;
    }

    const LocalDependencyPlanFailure& failure = plan.failures().front();
    switch(failure.kind) {
        case LocalDependencyPlanFailureKind::UnsupportedArchitecture:
            throw std::runtime_error(localization::format_translated_message(
                "Local package {} does not support architecture {}.",
                failure.parent_package_name,
                failure.effective_architecture.value_or("?")));
        case LocalDependencyPlanFailureKind::ConstraintMismatch:
            throw std::runtime_error(localization::format_translated_message(
                "Local dependency constraint cannot be satisfied for {}: {}",
                failure.parent_package_name,
                failure.dependency_specification.value_or("?")));
        case LocalDependencyPlanFailureKind::AmbiguousLocalProvider:
            throw std::runtime_error(localization::format_translated_message(
                "Local dependency has multiple matching local providers for {}: {}",
                failure.parent_package_name,
                failure.dependency_specification.value_or("?")));
        case LocalDependencyPlanFailureKind::RemoteProviderIdentityConflict:
            throw std::runtime_error(localization::format_translated_message(
                "Selected dependency provider identity conflicts for {}: {}",
                failure.parent_package_name,
                failure.dependency_specification.value_or("?")));
    }
    throw std::runtime_error(localization::translate_message(
        "Local dependency plan is incomplete."));
}

ArtifactMakepkgBuildOptions local_makepkg_options(
    const AppConfig& config) noexcept {
    return ArtifactMakepkgBuildOptions{
        .no_confirm = config.no_confirm,
        .rebuild = config.user_config.build.mode == BuildMode::Rebuild,
        .clean_build = config.user_config.build.mode == BuildMode::Clean};
}

void present_local_source_install_result(
    const PackageBaseSourceBuildExecutionResult& result) {
    // TRANSLATORS: The placeholders are the literal PackageBase field identity and a local PackageBase name.
    Logger::info(localization::format_translated_message(
        "Local {} result: {}", "PackageBase", result.package_base()));
    for(const PackageBaseSourceBuildSelectedResult& child :
        result.selected_children()) {
        const std::string reason = child.desired_reason ==
                                           DesiredInstallReason::Explicit
                                       ? localization::translate_message("explicit")
                                       : localization::translate_message("dependency");
        const std::string outcome = child.outcome ==
                                            ArtifactInstallExecutionOutcome::Installed
                                        ? localization::translate_message("installed")
                                        : localization::translate_message("skipped as needed");
        // TRANSLATORS: The placeholders are a package name, full version, install reason, and install outcome.
        Logger::info(localization::format_translated_message(
            "  required child: {} {} ({}): {}",
            child.identity.package_name, child.identity.full_version,
            reason, outcome));
    }
    for(const ArtifactPackageIdentity& artifact :
        result.unselected_artifacts()) {
        // TRANSLATORS: The placeholders are a produced package name and full version.
        Logger::info(localization::format_translated_message(
            "  produced artifact: {} {} (not selected; not installed)",
            artifact.package_name, artifact.full_version));
    }
}

} // namespace

std::string local_source_workspace_failure_diagnostic(
    const LocalSourceWorkspaceFailure& failure) {
    const std::string path = failure.relative_path.empty()
                                 ? "."
                                 : failure.relative_path.string();
    switch(failure.code) {
        case LocalSourceWorkspaceErrorCode::CacheInsideSource:
            // TRANSLATORS: The placeholder is the project identity.
            return localization::format_translated_message(
                "{} state and cache directories must be outside the local source tree.",
                application_identity::PROJECT_NAME);
        case LocalSourceWorkspaceErrorCode::OwnershipMismatch:
            return localization::format_translated_message(
                "Local source entry is not owned by the current user: {}",
                path);
        case LocalSourceWorkspaceErrorCode::UnsafePermissions:
            return localization::format_translated_message(
                "Local source entry is writable by its group or others: {}",
                path);
        case LocalSourceWorkspaceErrorCode::PermissionDenied:
            return localization::format_translated_message(
                "Permission was denied while inspecting local source entry: {}",
                path);
        case LocalSourceWorkspaceErrorCode::ConcurrentMutation:
            return localization::format_translated_message(
                "Local source entry changed identity during the operation: {}",
                path);
        case LocalSourceWorkspaceErrorCode::ContentChanged:
            return localization::format_translated_message(
                "Local source entry content changed during the operation: {}",
                path);
        case LocalSourceWorkspaceErrorCode::RandomnessUnavailable:
        case LocalSourceWorkspaceErrorCode::NameCollision:
        case LocalSourceWorkspaceErrorCode::UnsafeName:
        case LocalSourceWorkspaceErrorCode::UnsupportedFileType:
        case LocalSourceWorkspaceErrorCode::FilesystemBoundary:
        case LocalSourceWorkspaceErrorCode::SymlinkEscape:
        case LocalSourceWorkspaceErrorCode::MetadataFailure:
        case LocalSourceWorkspaceErrorCode::ReadFailure:
        case LocalSourceWorkspaceErrorCode::WriteFailure:
        case LocalSourceWorkspaceErrorCode::InvalidState:
        case LocalSourceWorkspaceErrorCode::CleanupFailure:
            return localization::format_translated_message(
                "Failed to validate local source metadata: {}", path);
    }
    return localization::format_translated_message(
        "Failed to validate local source root: {}", path);
}

namespace {

std::string local_source_build_phase_failure_diagnostic(
    const LocalSourceBuildPhaseError& error) {
    if(error.source_root_failure() != nullptr) {
        return local_source_root_failure_diagnostic(
            *error.source_root_failure());
    }
    if(error.source_workspace_failure() != nullptr) {
        return local_source_workspace_failure_diagnostic(
            *error.source_workspace_failure());
    }
    if(error.phase() == LocalSourceBuildFailurePhase::SourceCleanup &&
       error.source_cleanup_failure() != nullptr) {
        return localization::format_translated_message(
            "Local source workspace cleanup failed: {}",
            local_source_workspace_failure_diagnostic(
                *error.source_cleanup_failure()));
    }
    if(error.build_exit_code().has_value()) {
        // TRANSLATORS: The first placeholder is the literal command name
        // "makepkg"; the second is its exit code.
        return localization::format_translated_message(
            "The build-only {} command failed with exit code {}.",
            "makepkg", *error.build_exit_code());
    }
    if(error.selection_failure() != nullptr) {
        // TRANSLATORS: The first placeholder is the literal PackageBase field identity; the second is a local PackageBase name.
        return localization::format_translated_message(
            "Local source artifact selection failed for {} {}.",
            "PackageBase",
            error.selection_failure()->package_base);
    }

    switch(error.phase()) {
        case LocalSourceBuildFailurePhase::Preflight:
            return localization::translate_message(
                "Local source build preflight failed due to an internal consistency error.");
        case LocalSourceBuildFailurePhase::SourceWorkspace:
            return localization::translate_message(
                "Local source workspace preparation failed.");
        case LocalSourceBuildFailurePhase::ArtifactWorkspace:
            return localization::translate_message(
                "Local source artifact workspace preparation failed.");
        case LocalSourceBuildFailurePhase::BuildContext:
            return localization::translate_message(
                "Local source build context preparation failed.");
        case LocalSourceBuildFailurePhase::Packagelist:
            // TRANSLATORS: The placeholder is the literal command name "makepkg".
            return localization::format_translated_message(
                "Local source {} artifact-list query failed.",
                "makepkg");
        case LocalSourceBuildFailurePhase::Build:
            return localization::translate_message(
                "Local source build command failed.");
        case LocalSourceBuildFailurePhase::ArtifactValidation:
            return localization::translate_message(
                "Local source artifact validation failed.");
        case LocalSourceBuildFailurePhase::ArtifactIdentity:
            return localization::translate_message(
                "Local source artifact identity query failed.");
        case LocalSourceBuildFailurePhase::ArtifactSelection:
            return localization::translate_message(
                "Local source artifact selection failed.");
        case LocalSourceBuildFailurePhase::SourceCleanup:
            return localization::translate_message(
                "Local source workspace cleanup failed.");
    }
    return localization::translate_message(
        "Local source build failed due to an internal error.");
}

std::string local_source_build_failure_diagnostic(
    const LocalSourceBuildPhaseError& error) {
    std::string diagnostic =
        local_source_build_phase_failure_diagnostic(error);
    if(error.retained_artifact_workspace() != nullptr) {
        // TRANSLATORS: The placeholder is a display-only artifact workspace path retained for diagnosis.
        diagnostic += "\n" + localization::format_translated_message(
                                 "Retained artifact workspace: {}",
                                 error.retained_artifact_workspace()->string());
    }
    if(error.phase() != LocalSourceBuildFailurePhase::SourceCleanup &&
       error.source_cleanup_failure() != nullptr) {
        diagnostic += "\n" + localization::format_translated_message(
                                 "Local source workspace cleanup also failed: {}",
                                 local_source_workspace_failure_diagnostic(
                                     *error.source_cleanup_failure()));
    }
    return diagnostic;
}

} // namespace

PreparedLocalSourceBuildRoute prepare_local_source_build_route(
    LocalSourceBuildInvocation invocation,
    const AppConfig& config) {
    require_supported_production_source_build_options(config);
    require_unclaimed_artifact_pkgdest(invocation.source_environment);
    try {
        LocalSourceRoot source_root = open_local_source_root(
            invocation.directory,
            !invocation.source_environment.ordered_assignments.empty());
        return PreparedLocalSourceBuildRoute{
            std::move(invocation), std::move(source_root)};
    } catch(const LocalSourceRootError& error) {
        throw std::runtime_error(
            local_source_root_failure_diagnostic(error.failure()));
    }
}

void require_executable_local_source_build_route(
    const PreparedLocalSourceBuildRoute& route) {
    if(route.source_root.metadata().state() !=
       LocalSourceMetadataState::Unsafe) {
        return;
    }
    if(route.source_root.metadata().unsafe_failure() != nullptr) {
        throw std::runtime_error(local_source_root_failure_diagnostic(
            *route.source_root.metadata().unsafe_failure()));
    }
    throw std::runtime_error(localization::translate_message(
        "Local source metadata is unsafe and cannot be evaluated."));
}

RemoteSourceBuildInvocation require_remote_source_build_invocation(
    const std::vector<std::string>& args) {
    if(args.empty()) {
        // TRANSLATORS: The placeholders are the literal CLI command and the complete build syntax.
        throw std::invalid_argument(localization::format_translated_message(
            "Usage: {} {}",
            application_identity::COMMAND_NAME,
            cli_operation_syntax(cli_authority::OperationId::Build)));
    }

    RemoteSourceBuildInvocation invocation;
    for(const auto& arg : args) {
        std::string key;
        std::string value;
        if(split_env_assignment(arg, key, value)) {
            invocation.source_environment.ordered_assignments.push_back(
                {std::move(key), std::move(value)});
        } else if(arg.find('=') != std::string::npos) {
            // TRANSLATORS: The placeholder is a literal environment assignment.
            throw std::invalid_argument(
                localization::format_translated_message(
                    "Invalid environment assignment: {}", arg));
        } else if(invocation.package_name.empty()) {
            invocation.package_name = arg;
        } else {
            // Shared runtime validation rejects this before dispatch. Keep the
            // direct helper fail-closed for test/support callers as well.
            throw std::invalid_argument(
                localization::format_translated_message(
                    "Operation {} requires exactly one {} operand.",
                    "build", "<pkg>"));
        }
    }
    if(invocation.package_name.empty()) {
        throw std::invalid_argument(localization::translate_message(
            "No package specified."));
    }
    require_valid_package_name(invocation.package_name);
    return invocation;
}

int cmd_build_local(
    PreparedLocalSourceBuildRoute route,
    const AppConfig& config) {
    try {
        const bool has_one_off_environment_assignment =
            !route.invocation.source_environment
                 .ordered_assignments.empty();
        ReviewedLocalSourceRoot reviewed = review_local_source_root(
            std::move(route.source_root),
            has_one_off_environment_assignment, config);
        const std::string effective_architecture =
            resolve_local_source_effective_architecture(
                route.invocation.source_environment);
        LocalSourceBuildMetadata accepted_metadata =
            accept_local_source_build_metadata(
                reviewed.source_root,
                std::move(route.invocation.source_environment),
                effective_architecture, reviewed.pkgbuild_changed,
                config);
        LocalBuildPlan plan = resolve_local_build_plan(
            accepted_metadata.metadata(), effective_architecture,
            PackageRelationLocalSourceIdentity{
                reviewed.source_root.canonical_path(),
                reviewed.source_root.directory_identity().device,
                reviewed.source_root.directory_identity().inode},
            provider_selection_callback(config));
        require_complete_local_build_plan(plan);

        LocalSourceBuildDependencyPreparation dependency_preparation =
            prepare_local_source_build_dependencies(
                plan, true, false);
        preflight_local_source_build_dependencies(
            dependency_preparation, config);

        // POLICY(#271): all option/root/metadata/plan validation precedes
        // cache creation. POLICY(#411): remote dependency fatal-state
        // observation also completes before this persistent mutation. Both
        // remote dependencies and local build then receive the same retained
        // cache authority before the first transaction.
        const xdg_directory_safety::DirectoryCreationPrecondition
            cache_creation_precondition =
                [&reviewed](
                    const xdg_directory_safety::DirectoryIdentity&
                        parent_identity) {
                    require_directory_identity_outside_local_source_tree(
                        reviewed.source_root,
                        parent_identity.device,
                        parent_identity.inode);
                };
        ValidatedCacheRoot cache_root = prepare_process_cache_root(
            cache_creation_precondition);
        require_directory_identity_outside_local_source_tree(
            reviewed.source_root, cache_root.device(),
            cache_root.inode());
        PreparedProductionSourceBuildInvocation dependency_invocation =
            prepare_local_source_build_dependency_invocation(
                std::move(dependency_preparation), cache_root, config);
        const PacmanDatabasePaths database_paths =
            dependency_invocation.database_paths;
        PreparedLocalSourceBuild local_build = prepare_local_source_build(
            LocalSourceBuildRequest{
                std::move(reviewed.source_root), plan, cache_root,
                std::move(accepted_metadata),
                local_makepkg_options(config)});

        const auto dependency_result = execute_prepared_source_build_invocation(
            std::move(dependency_invocation), config);
        if(!dependency_result.is_success()) return dependency_result.command_exit_status();
        LocalSourceBuildResult build_result =
            execute_prepared_local_source_build(
                std::move(local_build));
        PackageBaseSourceBuildExecutionResult install_result = [&]() {
            try {
                return execute_local_source_install(
                    std::move(build_result), database_paths,
                    SeparatedSourceBuildUnitOptions{
                        .no_confirm = config.no_confirm});
            } catch(const SeparatedPackageBaseSourceBuildCleanupError&
                        error) {
                // Local transaction completion is not flattened into a
                // dependency build failure; preserve and present its result.
                present_local_source_install_result(error.result());
                throw;
            }
        }();
        present_local_source_install_result(install_result);
        return 0;
    } catch(const ProductionSourceBuildInvocationError& error) {
        Logger::error(
            format_production_source_build_invocation_failure(error));
        return 1;
    } catch(const SeparatedPackageBaseSourceBuildCleanupError& error) {
        // Dependency executor already presents its PackageBase result; the
        // inner local-install boundary above presents the local result.
        Logger::error(error.what());
        return 1;
    } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
        if(error.reviewed_source_failure().has_value()) {
            Logger::error(reviewed_source_production_failure_diagnostic(
                *error.reviewed_source_failure()));
        } else {
            Logger::error(localization::format_translated_message(
                "Build Error: {}", error.what()));
        }
        return 1;
    } catch(const ReviewedSourceProductionError& error) {
        Logger::error(reviewed_source_production_failure_diagnostic(
            error.failure()));
        return 1;
    } catch(const LocalSourceBuildPhaseError& error) {
        // TRANSLATORS: The placeholder is a classified local source-build diagnostic.
        Logger::error(localization::format_translated_message(
            "Build Error: {}",
            local_source_build_failure_diagnostic(error)));
        return 1;
    } catch(const LocalSourceRootError& error) {
        report_runtime_diagnostic(
            project_local_source_root_diagnostic(error.failure()),
            local_source_root_failure_diagnostic(error.failure()));
        return 1;
    } catch(const LocalSourceWorkspaceError& error) {
        Logger::error(
            local_source_workspace_failure_diagnostic(error.failure()));
        return 1;
    } catch(const ConfirmationOperationStopped&) {
        return 1;
    } catch(const std::exception& error) {
        // TRANSLATORS: The placeholder is a diagnostic from the failed build.
        Logger::error(localization::format_translated_message(
            "Build Error: {}", error.what()));
        return 1;
    }
}

int cmd_build(
    const std::vector<std::string>& args,
    const AppConfig& config) {
    RemoteSourceBuildInvocation invocation;
    try {
        invocation = require_remote_source_build_invocation(args);
    } catch(const std::exception& error) {
        Logger::error(error.what());
        return 1;
    }

    try {
        if(!build_source_target(
               invocation.package_name,
               invocation.source_environment, config)) return 1;
    } catch(const ProductionSourceBuildInvocationError& error) {
        Logger::error(
            format_production_source_build_invocation_failure(error));
        return 1;
    } catch(const SeparatedPackageBaseSourceBuildCleanupError& error) {
        // Direct buildはretained workspaceを手動確認できる既存contractを
        // 維持し、transaction成功済みcleanup failureとしてそのまま表示する。
        Logger::error(error.what());
        return 1;
    } catch(const SeparatedPackageBaseSourceBuildPhaseError& error) {
        if(error.reviewed_source_failure().has_value()) {
            Logger::error(reviewed_source_production_failure_diagnostic(
                *error.reviewed_source_failure()));
        } else {
            Logger::error(localization::format_translated_message(
                "Build Error: {}", error.what()));
        }
        return 1;
    } catch(const SeparatedSourceBuildCleanupError& error) {
        // Package transactionは成功済みなので、generic Build Error prefixを付けない。
        Logger::error(error.what());
        return 1;
    } catch(const ReviewedSourceProductionError& error) {
        Logger::error(reviewed_source_production_failure_diagnostic(
            error.failure()));
        return 1;
    } catch(const ConfirmationOperationStopped&) {
        return 1;
    } catch(const std::exception& e) {
        // TRANSLATORS: The placeholder is a diagnostic from the failed build.
        Logger::error(localization::format_translated_message(
            "Build Error: {}", e.what()));
        return 1;
    }
    return 0;
}

int cmd_add_src(const std::vector<std::string>& args) {
    require_valid_add_src_packages(args);
    bool failed = false;
    std::vector<std::string> current_pkgs;
    for(const auto& arg : args) {
        std::string key, val;
        if(arg.find('=') == std::string::npos) {
            // POLICY: 1 package = 1 preference file。ファイル名は package name validation で固定する。
            try {
                create_source_preference_entry(arg);
                // TRANSLATORS: The placeholder is a package name.
                Logger::info(localization::format_translated_message(
                    "Added {} to source-build list.", arg));
                current_pkgs.push_back(arg);
            } catch(const std::exception& error) {
                Logger::error(error.what());
                failed = true;
            }
        } else if(split_env_assignment(arg, key, val)) {
            if(current_pkgs.empty()) {
                // TRANSLATORS: The placeholder is a literal environment assignment.
                Logger::error(localization::format_translated_message(
                    "Environment assignment requires a preceding package: {}",
                    arg));
                failed = true;
                continue;
            }
            for(const std::string& package_name : current_pkgs) {
                const fs::path entry_path =
                    source_preference_entry_path(package_name);
                // TRANSLATORS: The placeholders are a literal environment assignment and a source preference file path.
                Logger::info(localization::format_translated_message(
                    "Appending {} to {}", arg,
                    entry_path.string()));
                try {
                    append_source_preference_assignment(
                        package_name, key + "=" + val);
                } catch(const std::exception& error) {
                    Logger::error(error.what());
                    failed = true;
                }
            }
        } else {
            // TRANSLATORS: The placeholder is a literal environment assignment.
            Logger::error(localization::format_translated_message(
                "Invalid environment assignment: {}", arg));
            failed = true;
        }
    }
    return failed ? 1 : 0;
}

int cmd_edit_src(
    const std::vector<std::string>& targets,
    const AppConfig& config) {
    require_valid_package_targets(targets);
    bool failed = false;
    for(const auto& pkg : targets) {
        const fs::path p = source_preference_entry_path(pkg);
        StrictSourcePreferenceResult current =
            read_source_preference_strict(pkg);
        std::string existing_contents;
        std::optional<SourcePreferenceEntryIdentity> expected_identity;
        if(const auto* failure =
               std::get_if<SourcePreferenceFailure>(&current)) {
            Logger::error(failure->diagnostic);
            failed = true;
            continue;
        }
        if(auto* loaded = std::get_if<SourcePreferenceLoaded>(&current)) {
            if(!loaded->identity.has_value()) {
                throw std::logic_error(
                    localization::format_translated_message(
                        "Source preference reader did not return an entry identity for {}.",
                        p.string()));
            }
            existing_contents = std::move(loaded->raw_contents);
            expected_identity = loaded->identity;
        }

        std::string temp_template = "/tmp/moguet-edit-src-" + pkg + ".XXXXXX";
        std::vector<char> temp_name(temp_template.begin(), temp_template.end());
        temp_name.push_back('\0');

        int fd = mkstemp(temp_name.data());
        if(fd == -1) {
            // TRANSLATORS: The placeholder is a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to create a temporary source preference file: {}",
                std::strerror(errno)));
            failed = true;
            continue;
        }

        fs::path temp_path = temp_name.data();
        auto cleanup_temp = [&temp_path]() {
            std::error_code ec;
            fs::remove(temp_path, ec);
            if(ec) {
                // TRANSLATORS: The placeholders are a temporary file path and a system error message.
                Logger::warn(localization::format_translated_message(
                    "Failed to remove temporary file {}: {}",
                    temp_path.string(),
                    ec.message()));
            }
        };

        std::size_t copied_size = 0;
        bool copy_failed = false;
        while(copied_size < existing_contents.size()) {
            const ssize_t write_size = ::write(
                fd, existing_contents.data() + copied_size,
                existing_contents.size() - copied_size);
            if(write_size > 0) {
                copied_size += static_cast<std::size_t>(write_size);
                continue;
            }
            if(write_size < 0 && errno == EINTR) continue;
            copy_failed = true;
            break;
        }
        if(copy_failed) {
            // TRANSLATORS: The placeholders are the source preference path and temporary file path.
            Logger::error(localization::format_translated_message(
                "Failed to copy source preference file {} to temporary file {}.",
                p.string(), temp_path.string()));
            static_cast<void>(::close(fd));
            cleanup_temp();
            failed = true;
            continue;
        }
        if(::close(fd) != 0) {
            // TRANSLATORS: The placeholders are a temporary file path and a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to close temporary file {}: {}",
                temp_path.string(),
                std::strerror(errno)));
            cleanup_temp();
            failed = true;
            continue;
        }

        if(run_command(build_editor_command(config.editor, temp_path)) != 0) {
            // TRANSLATORS: The placeholders are a source preference path and a retained temporary file path.
            Logger::error(localization::format_translated_message(
                "Editor failed for {}; the edited file was kept at {}.",
                p.string(), temp_path.string()));
            failed = true;
            continue;
        }

        // POLICY: editorのrename型saveを許容しつつ、最終pathnameはnofollowでpinする。
        int source_fd = open(
            temp_path.c_str(),
            O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if(source_fd == -1) {
            int open_error = errno;
            // TRANSLATORS: The placeholders are a temporary file path and a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to open edited temporary file {}: {}",
                temp_path.string(),
                std::strerror(open_error)));
            failed = true;
            continue;
        }

        auto close_source = [&]() {
            if(close(source_fd) == 0) return true;
            int close_error = errno;
            // TRANSLATORS: The placeholders are a temporary file path and a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to close edited temporary file {}: {}",
                temp_path.string(),
                std::strerror(close_error)));
            return false;
        };

        struct stat source_status{};
        if(fstat(source_fd, &source_status) != 0) {
            int stat_error = errno;
            // TRANSLATORS: The placeholders are a temporary file path and a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to inspect edited temporary file {}: {}",
                temp_path.string(),
                std::strerror(stat_error)));
            close_source();
            failed = true;
            continue;
        }
        if(!S_ISREG(source_status.st_mode)) {
            // TRANSLATORS: The placeholder is a temporary file path.
            Logger::error(localization::format_translated_message(
                "Edited temporary file is not a regular file: {}",
                temp_path.string()));
            close_source();
            failed = true;
            continue;
        }

        try {
            replace_source_preference_entry_from_descriptor(
                pkg, source_fd, expected_identity);
        } catch(const std::exception& error) {
            const bool source_closed = close_source();
            static_cast<void>(source_closed);
            Logger::error(error.what());
            // TRANSLATORS: The placeholders are the destination and retained temporary file paths.
            Logger::error(localization::format_translated_message(
                "Failed to install the edited source-build preference at {}; the edited file was kept at {}.",
                p.string(),
                temp_path.string()));
            failed = true;
            continue;
        }

        struct stat final_source_status{};
        if(::fstat(source_fd, &final_source_status) != 0) {
            const int stat_error = errno;
            static_cast<void>(close_source());
            // TRANSLATORS: The placeholders are a temporary file path and a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to inspect edited temporary file {}: {}",
                temp_path.string(),
                std::strerror(stat_error)));
            failed = true;
            continue;
        }
        if(!same_temporary_file_state(
               source_status, final_source_status)) {
            // TRANSLATORS: The placeholder is a retained temporary file path.
            Logger::error(localization::format_translated_message(
                "Edited temporary file changed during publication and was not removed: {}",
                temp_path.string()));
            static_cast<void>(close_source());
            failed = true;
            continue;
        }
        struct stat named_source_status{};
        int named_status_result = -1;
        do {
            named_status_result = ::lstat(
                temp_path.c_str(), &named_source_status);
        } while(named_status_result != 0 && errno == EINTR);
        if(named_status_result != 0 ||
           !same_temporary_file_state(
               final_source_status, named_source_status)) {
            // TRANSLATORS: The placeholder is a retained temporary file path.
            Logger::error(localization::format_translated_message(
                "Edited temporary file changed during publication and was not removed: {}",
                temp_path.string()));
            static_cast<void>(close_source());
            failed = true;
            continue;
        }

        if(!close_source()) {
            failed = true;
            continue;
        }
        do {
            named_status_result = ::lstat(
                temp_path.c_str(), &named_source_status);
        } while(named_status_result != 0 && errno == EINTR);
        if(named_status_result != 0 ||
           !same_temporary_file_state(
               final_source_status, named_source_status)) {
            // TRANSLATORS: The placeholder is a retained temporary file path.
            Logger::error(localization::format_translated_message(
                "Edited temporary file changed during publication and was not removed: {}",
                temp_path.string()));
            failed = true;
            continue;
        }
        if(::unlink(temp_path.c_str()) != 0) {
            const int remove_error = errno;
            // TRANSLATORS: The placeholders are a temporary file path and a system error message.
            Logger::error(localization::format_translated_message(
                "Failed to remove temporary file {}: {}",
                temp_path.string(),
                std::strerror(remove_error)));
            failed = true;
            continue;
        }
    }
    return failed ? 1 : 0;
}

void cmd_list_src() {
    const SourcePreferenceListSnapshot snapshot =
        snapshot_source_preferences_for_listing();
    if(!snapshot.root_exists) {
        std::cout << localization::translate_message(
                         "No source-build packages registered.")
                  << std::endl;
        return;
    }
    std::cout << "\033[1m"
              << localization::translate_message(
                     "Registered Source Packages:")
              << "\033[0m" << std::endl;
    for(const SourcePreferenceListEntrySnapshot& entry : snapshot.entries) {
        // NO_TRANSLATE: Package identity and stored environment key/value
        // lines are runtime data and must remain byte-for-byte unchanged.
        std::cout << "  \033[1;36m" << entry.package_name
                  << "\033[0m" << std::endl;
        for(const std::string& line : entry.display_lines) {
            std::cout << "    " << line << std::endl;
        }
    }
    if(snapshot.entries.empty()) {
        std::cout << "  " << localization::translate_message("(none)")
                  << std::endl;
    }
}

int cmd_del_src(const std::vector<std::string>& targets) {
    require_valid_package_targets(targets);
    bool failed = false;
    for(const auto& pkg : targets) {
        // TRANSLATORS: The placeholder is a package name.
        Logger::info(localization::format_translated_message(
            "Removing {} from list...", pkg));
        try {
            static_cast<void>(remove_source_preference_entry(pkg));
        } catch(const std::exception& error) {
            Logger::error(error.what());
            failed = true;
        }
    }
    return failed ? 1 : 0;
}

void cmd_revert(
    const std::vector<std::string>& targets,
    const AppConfig& config) {
    require_valid_package_targets(targets);
    bool failed = false;
    std::vector<std::string> reinstall_targets;
    for(const auto& pkg : targets) {
        // The repository decision precedes this target's preference mutation.
        // Independent targets still continue and share the final reinstall.
        const StrictRepositoryPackageQueryResult repository =
            query_repository_package_strict(pkg);
        if(const auto* failure = std::get_if<RepositoryMetadataFailure>(&repository)) {
            Logger::error(localization::format_translated_message(
                "Failed to inspect repository metadata for {}: {}",
                pkg, failure->diagnostic));
            failed = true;
            continue;
        }
        bool was_removed = false;
        try {
            was_removed = remove_source_preference_entry(pkg);
        } catch(const std::exception& error) {
            Logger::error(error.what());
            failed = true;
            continue;
        }
        if(was_removed) {
            // TRANSLATORS: The placeholder is a package name.
            Logger::info(localization::format_translated_message(
                "Unmarking source-build for {}", pkg));
        } else {
            // TRANSLATORS: The placeholder is a package name.
            Logger::warn(localization::format_translated_message(
                "{} was not marked.", pkg));
        }
        if(std::holds_alternative<RepositoryPackagePresent>(repository)) {
            // TRANSLATORS: The placeholder is a package name.
            Logger::info(localization::format_translated_message(
                "{} exists in official repos. Will reinstall binary.",
                pkg));
            reinstall_targets.push_back(pkg);
        } else {
            // TRANSLATORS: The placeholders are a package name and the AUR project identity.
            Logger::info(localization::format_translated_message(
                "{} is likely an {} package. Config removed only.",
                pkg, "AUR"));
        }
    }
    if(!reinstall_targets.empty()) {
        std::string pkg_list = shell_words::join(reinstall_targets);
        std::vector<std::string> pacman_args = {"-S"};
        pacman_args.insert(pacman_args.end(), reinstall_targets.begin(), reinstall_targets.end());
        // TRANSLATORS: The placeholder is a shell-escaped list of package names.
        Logger::info(localization::format_translated_message(
            "Reinstalling binaries: {}", pkg_list));
        if(run_command("sudo pacman " + join_pacman_args(pacman_args, config)) != 0) {
            throw std::runtime_error(localization::translate_message(
                "Failed to reinstall binaries."));
        }
    }
    if(failed) {
        throw std::runtime_error(localization::translate_message(
            "Failed to revert one or more packages."));
    }
}

int cmd_clean(const AppConfig& config) {
    // POLICY(#175,#305): new Moguet XDG cache rootだけをadoptし、全targetの
    // descriptor-relative preflight capabilityをpacman/promptより前に構築する。
    // 同じmove-only capabilityをconsumeまで保持し、opaque generationを再検証する。
    ValidatedCacheRoot cache = prepare_process_cache_root();
    PreparedCacheCleanup cleanup =
        preflight_cache_cleanup(cache);
    const bool cache_has_entries = !cleanup.empty();
    bool failed = false;
    Logger::info(localization::translate_message(
        "Cleaning package caches..."));
    if(run_command("sudo pacman " + join_pacman_args({"-Sc"}, config)) != 0) {
        // TRANSLATORS: The placeholder is the literal pacman program identity, capitalized to preserve the existing CLI contract.
        Logger::warn(localization::format_translated_message(
            "{} clean failed or cancelled.", "Pacman"));
        failed = true;
    }
    if(cache_has_entries) {
        // TRANSLATORS: The placeholders are the Moguet identity and build cache path.
        const std::string clean_question =
            localization::format_translated_message(
                "Clean {} build cache ({})?",
                application_identity::PROJECT_NAME,
                cleanup.cache_path().string());
        ConfirmationResult clean_confirmation = request_confirmation(
            clean_question, ConfirmationDefault::No,
            config.no_confirm);
        if(std::holds_alternative<ConfirmationAccepted>(
               clean_confirmation)) {
            Logger::info(localization::translate_message(
                "Removing cached build files..."));
            bool cleanup_failed = false;
            try {
                // new root内のlegacy-named logもordinary cache entry。legacy sibling
                // rootやXDG state/configは列挙・削除対象へ入らない。
                remove_preflighted_cache_paths(std::move(cleanup));
            } catch(const std::exception& e) {
                // TRANSLATORS: The placeholders are the Moguet identity and a cache cleanup diagnostic.
                Logger::error(localization::format_translated_message(
                    "Failed to clean the {} cache: {}",
                    application_identity::PROJECT_NAME,
                    e.what()));
                failed = true;
                cleanup_failed = true;
                Logger::warn(localization::format_translated_message(
                    "{} cache cleanup was incomplete.",
                    application_identity::PROJECT_NAME));
            }
            if(!cleanup_failed) {
                Logger::info(localization::format_translated_message(
                    "{} cache cleaned.",
                    application_identity::PROJECT_NAME));
            }
        } else if(std::holds_alternative<ConfirmationDeclined>(
                      clean_confirmation)) {
            Logger::info(localization::format_translated_message(
                "Skipped {} cache cleaning.",
                application_identity::PROJECT_NAME));
        } else {
            report_confirmation_stop(
                clean_confirmation, DiagnosticOperation::Clean,
                DiagnosticPhase::Cleanup);
            return 1;
        }
    } else {
        Logger::info(localization::format_translated_message(
            "{} cache is empty.",
            application_identity::PROJECT_NAME));
    }
    return failed ? 1 : 0;
}

int cmd_upgrade(const AppConfig& config) {
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(
            config, present_system_source_upgrade_event);
    if(const auto* blocked =
           std::get_if<SystemSourceUpgradeResult>(&preparation)) {
        throw_system_source_upgrade_failure(*blocked);
    }

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(
                std::get<PreparedSystemSourceUpgrade>(
                    preparation)),
            config,
            present_system_source_upgrade_event);
    present_registered_reviewed_source_outcomes(result);
    present_registered_package_base_results(result);
    if(result.status != SystemSourceUpgradeStatus::Completed) {
        throw_system_source_upgrade_failure(result);
    }

    // POLICY(#281): typed Incomplete（従来のunknown update status skip）は
    // aggregateへ残すが、legacy upgradeのexit statusは変えない。
    const bool has_invalid_preference = std::any_of(
        result.registered_source_results.begin(),
        result.registered_source_results.end(),
        [](const RegisteredSourceUpgradeResult& source) {
            return source.failure_kind ==
                   RegisteredSourceUpgradeFailureKind::
                       InvalidPreferenceName;
        });
    return has_invalid_preference ? 1 : 0;
}
