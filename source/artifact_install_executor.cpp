#include "artifact_install_executor.hpp"

#include "localization.hpp"
#include "process.hpp"
#include "shell_words.hpp"

#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

[[noreturn]] void throw_malformed_installed_metadata(
    const std::string& diagnostic) {
    throw PackageMetadataError(PackageMetadataFailure{
        PackageMetadataErrorCode::MalformedMetadata, diagnostic});
}

ExistingInstallReason map_existing_install_reason(
    InstalledPackageReason reason) {
    switch(reason) {
        case InstalledPackageReason::Explicit:
            return ExistingInstallReason::Explicit;
        case InstalledPackageReason::Dependency:
            return ExistingInstallReason::Dependency;
        case InstalledPackageReason::Unknown:
            throw_malformed_installed_metadata(localization::translate_message(
                "Installed package metadata contains an unknown install reason."));
    }

    throw_malformed_installed_metadata(localization::translate_message(
        "Installed package metadata contains an invalid install reason."));
}

} // namespace

InstalledArtifactPolicyState map_installed_artifact_policy_state(
    const ArtifactPackageIdentity& identity,
    const InstalledPackageQueryResult& query_result) {
    if(std::holds_alternative<PackageNotFound>(query_result)) {
        return InstalledArtifactPolicyState{
            InstalledVersionState::NotInstalled, std::nullopt};
    }

    if(const auto* failure =
           std::get_if<PackageMetadataFailure>(&query_result)) {
        // POLICY: query failureとpackage absenceを区別し、code/diagnosticを失わない。
        throw PackageMetadataError(*failure);
    }

    const auto* metadata =
        std::get_if<InstalledPackageMetadata>(&query_result);
    if(metadata == nullptr) {
        throw std::logic_error(localization::translate_message(
            "Unknown installed package query result."));
    }
    if(metadata->name != identity.package_name) {
        throw_malformed_installed_metadata(localization::translate_message(
            "Installed package metadata name does not match the package artifact identity."));
    }
    if(metadata->version.empty()) {
        throw_malformed_installed_metadata(localization::translate_message(
            "Installed package metadata contains an empty version."));
    }

    // POLICY: epoch/pkgrelを含むversionを加工せず、artifact identityとexact比較する。
    InstalledVersionState version_state =
        metadata->version == identity.full_version
            ? InstalledVersionState::SameVersion
            : InstalledVersionState::DifferentVersion;
    return InstalledArtifactPolicyState{
        version_state, map_existing_install_reason(metadata->reason)};
}

PreparedArtifactInstall::PreparedArtifactInstall(
    std::string&& requested_name,
    DesiredInstallReason desired_reason,
    bool needed,
    ArtifactPackageIdentity&& identity,
    ValidatedArtifactInstallTarget&& target,
    InstalledVersionState installed_version_state,
    std::optional<ExistingInstallReason> existing_reason,
    InstallReasonDirective directive,
    ValidatedPackageArtifactPath&& artifact) noexcept
    : requested_name_(std::move(requested_name)),
      desired_reason_(desired_reason), needed_(needed),
      identity_(std::move(identity)), target_(std::move(target)),
      installed_version_state_(installed_version_state),
      existing_reason_(existing_reason), directive_(directive),
      artifact_(std::move(artifact)) {
}

void PreparedArtifactInstall::retain_workspace_for_diagnostics() noexcept {
    artifact_.retain_workspace_for_diagnostics();
}

void PreparedArtifactInstall::cleanup_workspace() {
    artifact_.cleanup_workspace();
}

PreparedArtifactInstall prepare_artifact_install(
    ValidatedPackageArtifactPath& artifact,
    const std::string& requested_name,
    const std::string& package_base,
    DesiredInstallReason desired_reason,
    const ArtifactInstallPreparationOptions& options,
    const PacmanDatabasePaths& database_paths) {
    artifact.require_validity();
    require_supported_separated_install_options(options.rm_deps);

    ArtifactPackageIdentity identity =
        query_artifact_package_identity(artifact);

    // POLICY: PR4 callerはArtifactMakepkgContextのPKGDEST guardを通過したartifactだけを
    // 渡す。filesystem capabilityが証明するfresh ownershipと合わせ、pure policyへ
    // exactly one identityをprojectする。
    ArtifactSelectionRequest selection_request{
        requested_name,
        package_base,
        ArtifactWorkspaceOwnership::InvocationOwnedFresh,
        SourcePkgdestState::NotDefined,
        {{identity.package_name}}};
    ValidatedArtifactInstallTarget target =
        validate_single_output_artifact(selection_request);

    InstalledArtifactPolicyState installed_state = [&]() {
        PackageMetadataSession session =
            PackageMetadataSession::open(database_paths);
        InstalledPackageQueryResult query_result =
            session.query_installed_package(identity.package_name);
        return map_installed_artifact_policy_state(identity, query_result);
    }();
    // LANDMINE: reason reducerとpacman -Uの前にsession scopeを必ず閉じる。
    InstallReasonDirective directive = resolve_install_reason_directive(
        desired_reason, installed_state.version_state,
        installed_state.existing_reason, options.needed);

    std::string owned_requested_name = requested_name;
    // 全throw可能処理を終えてから、noexcept constructorでだけartifact ownershipを移す。
    return PreparedArtifactInstall(
        std::move(owned_requested_name), desired_reason, options.needed,
        std::move(identity), std::move(target),
        installed_state.version_state, installed_state.existing_reason,
        directive, std::move(artifact));
}

ArtifactInstallExecutionOutcome execute_prepared_artifact_install(
    PreparedArtifactInstall& install,
    const ArtifactInstallExecutionOptions& options) {
    std::vector<std::string> arguments = {"sudo", "pacman", "-U"};
    if(options.no_confirm) arguments.emplace_back("--noconfirm");
    if(install.needed_) arguments.emplace_back("--needed");

    switch(install.directive_) {
        case InstallReasonDirective::Default:
            break;
        case InstallReasonDirective::AsExplicit:
            arguments.emplace_back("--asexplicit");
            break;
        case InstallReasonDirective::AsDependency:
            arguments.emplace_back("--asdeps");
            break;
        default:
            throw std::logic_error(localization::translate_message(
                "Unknown install reason directive."));
    }

    arguments.emplace_back("--");
    arguments.push_back(install.artifact_.path().string());
    std::string command = shell_words::join(arguments);

    // LANDMINE: validated capability取得後のreplacement/extra entryをtransaction直前に拒否する。
    install.artifact_.require_validity();
    int exit_code = run_command(command);
    if(exit_code != 0) {
        // package-controlled pathをtransaction diagnosticへ埋め込まない。
        throw std::runtime_error(localization::format_translated_message(
            // TRANSLATORS: The first placeholder is the literal command
            // "pacman -U"; the second is its numeric exit code.
            "{} failed with exit code {}.", "pacman -U", exit_code));
    }

    // POLICY(#267): --neededのskipはpacman outputではなく、transaction前に
    // exactに固定したversion/directive stateからだけ判定する。
    if(install.needed_ &&
       install.installed_version_state_ == InstalledVersionState::SameVersion &&
       install.directive_ == InstallReasonDirective::Default) {
        return ArtifactInstallExecutionOutcome::SkippedAsNeeded;
    }
    return ArtifactInstallExecutionOutcome::Installed;
}
