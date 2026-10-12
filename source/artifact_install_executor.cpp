#include "artifact_install_executor.hpp"

#include "localization.hpp"
#include "process.hpp"
#include "shell_words.hpp"
#include "terminal_safe_text.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

bool is_valid_full_package_version(std::string_view version) noexcept {
    const auto is_digits = [](std::string_view part) {
        return !part.empty() && std::all_of(part.begin(), part.end(), [](char byte) {
            return byte >= '0' && byte <= '9';
        });
    };
    const auto epoch = version.find(':');
    if(epoch != std::string_view::npos) {
        if(!is_digits(version.substr(0, epoch))) return false;
        version.remove_prefix(epoch + 1);
    }
    const auto release = version.find('-');
    if(release == std::string_view::npos || release == 0) return false;
    // This is makepkg's full package identity grammar, not dependency syntax.
    // In particular, '=', '<' and '>' are legal pkgver bytes. Ordering remains
    // exclusively alpm_pkg_vercmp; validation never normalizes the evidence.
    if(!std::all_of(version.begin(), version.begin() + release, [](char byte) {
           const auto value = static_cast<unsigned char>(byte);
           return value >= 0x21U && value <= 0x7eU && byte != ':' && byte != '/';
       })) return false;
    const auto pkgrel = version.substr(release + 1);
    const auto subrelease = pkgrel.find('.');
    return subrelease == std::string_view::npos
               ? is_digits(pkgrel)
               : is_digits(pkgrel.substr(0, subrelease)) && is_digits(pkgrel.substr(subrelease + 1));
}

std::string guard_diagnostic_value(std::string_view value) {
    constexpr std::size_t MAX_VALUE_BYTES = 160;
    std::string display = terminal_safe_text::escape_utf8(value.substr(0, MAX_VALUE_BYTES));
    if(value.size() > MAX_VALUE_BYTES) display += "...";
    return display;
}

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

std::optional<AutomaticAurVersionGuardFailureKind> evaluate_automatic_aur_version_ordering(
    std::optional<ArchVersionOrdering> actual_vs_installed,
    std::optional<ArchVersionOrdering> actual_vs_rpc) noexcept {
    using Kind = AutomaticAurVersionGuardFailureKind;
    const auto known_ordering = [](std::optional<ArchVersionOrdering> value) {
        return value && (*value == ArchVersionOrdering::Less || *value == ArchVersionOrdering::Equal || *value == ArchVersionOrdering::Greater);
    };
    if(!known_ordering(actual_vs_installed) || !known_ordering(actual_vs_rpc)) return Kind::ComparisonUnavailable;
    if(*actual_vs_installed != ArchVersionOrdering::Greater) return Kind::ArtifactNotNewerThanInstalled;
    if(*actual_vs_rpc == ArchVersionOrdering::Less) return Kind::ArtifactBelowObservedRpcVersion;
    return std::nullopt;
}

std::optional<AutomaticAurVersionGuardFailure> validate_automatic_aur_version_install(
    const RequiredPackageArtifactTarget& target,
    const ArtifactPackageIdentity& actual,
    const InstalledPackageQueryResult& fresh_installed) {
    if(!target.automatic_version_update) return std::nullopt;
    const auto& intent = *target.automatic_version_update;
    using Kind = AutomaticAurVersionGuardFailureKind;
    AutomaticAurVersionGuardFailure failure{
        Kind::IntentIdentityMismatch, target.package_base, target.package_name,
        intent.observed_rpc_version, intent.observed_installed_version,
        actual.full_version, std::nullopt, std::nullopt};
    if(target.expected_full_version || intent.package_base != target.package_base ||
       intent.package_name != target.package_name || actual.package_name != target.package_name ||
       actual.package_base.state() == ArtifactMetadataValueState::Malformed ||
       (actual.package_base.value() && *actual.package_base.value() != intent.package_base))
        return failure;
    if(const auto* metadata_failure = std::get_if<PackageMetadataFailure>(&fresh_installed)) {
        failure.kind = Kind::InstalledObservationFailed;
        failure.metadata_failure = *metadata_failure;
        return failure;
    }
    const auto* installed = std::get_if<InstalledPackageMetadata>(&fresh_installed);
    if(!installed) {
        failure.kind = Kind::InstalledPackageDisappeared;
        return failure;
    }
    failure.fresh_installed_version = installed->version;
    if(!is_valid_full_package_version(actual.full_version) ||
       !is_valid_full_package_version(intent.observed_rpc_version) ||
       !is_valid_full_package_version(intent.observed_installed_version) ||
       !is_valid_full_package_version(installed->version)) {
        failure.kind = Kind::InvalidVersionEvidence;
        return failure;
    }
    // An installed selected update cannot become a new dependency after the
    // query. Exact binding drift requires a new invocation, never a retry.
    // Query evidence contains name/version/reason, not the old installed Base.
    // A known installed Base may legitimately differ from the newly reviewed
    // RPC/artifact Base; do not manufacture source drift from that difference.
    if(installed->name != intent.package_name || installed->version != intent.observed_installed_version ||
       (intent.observed_installed_reason != InstalledPackageReason::Unknown && installed->reason != intent.observed_installed_reason) ||
       installed->package_base.state() == InstalledPackageMetadataValueState::Malformed) {
        failure.kind = Kind::InstalledStateDrift;
        return failure;
    }
    const auto actual_vs_installed = compare_arch_package_versions(actual.full_version, installed->version);
    const auto actual_vs_rpc = compare_arch_package_versions(actual.full_version, intent.observed_rpc_version);
    const auto refused = evaluate_automatic_aur_version_ordering(actual_vs_installed, actual_vs_rpc);
    if(refused) {
        failure.kind = *refused;
        return failure;
    }
    return std::nullopt;
}

std::string automatic_aur_version_guard_diagnostic(const AutomaticAurVersionGuardFailure& failure) {
    using Kind = AutomaticAurVersionGuardFailureKind;
    std::string reason = localization::translate_message("version comparison authority unavailable");
    std::string recovery = localization::translate_message("Check the package identity and full version evidence before starting a new invocation.");
    switch(failure.kind) {
        case Kind::IntentIdentityMismatch: reason = localization::format_translated_message("selected artifact and {} child identity mismatch", "AUR RPC"); break;
        case Kind::InvalidVersionEvidence: reason = localization::translate_message("invalid or missing full version evidence"); break;
        case Kind::ComparisonUnavailable:
            recovery = localization::format_translated_message("Restore {} version comparison support, then re-check the state in a new invocation.", "Arch");
            break;
        case Kind::InstalledObservationFailed:
            reason = localization::translate_message("fresh installed package observation failed");
            recovery = localization::translate_message("Re-check the installed package state, then start a new invocation.");
            break;
        case Kind::InstalledPackageDisappeared:
            reason = localization::translate_message("selected installed package disappeared");
            recovery = localization::translate_message("Re-check the installed package state, then start a new invocation.");
            break;
        case Kind::InstalledStateDrift:
            reason = localization::translate_message("installed package version, install reason, or identity changed since query");
            recovery = localization::translate_message("Re-check the installed package state, then start a new invocation.");
            break;
        case Kind::ArtifactNotNewerThanInstalled:
            reason = localization::translate_message("artifact is the same version as or older than installed");
            recovery = localization::format_translated_message("Check the recipe, observed {} version, and actual artifact version before starting a new invocation.", "AUR RPC");
            break;
        case Kind::ArtifactBelowObservedRpcVersion:
            reason = localization::format_translated_message("artifact version is below the observed {} version", "AUR RPC");
            recovery = localization::format_translated_message("Check the recipe, observed {} version, and actual artifact version before starting a new invocation.", "AUR RPC");
            break;
        default: throw std::logic_error(localization::translate_message("Unknown automatic Version guard failure kind."));
    }
    // Stored typed facts are presentation input, including malformed evidence.
    // Bound and escape each value before formatting; do not re-query, compare,
    // or grant authority from a displayed version or recovery instruction.
    return localization::format_translated_message(
               "Automatic {} Version update refused before package transaction for {}: {}; actual={}, fresh installed={}, query installed={}, observed {}={}.",
               "AUR", guard_diagnostic_value(failure.package_name), reason, guard_diagnostic_value(failure.actual_version),
               failure.fresh_installed_version ? guard_diagnostic_value(*failure.fresh_installed_version) : localization::translate_message("unavailable"),
               guard_diagnostic_value(failure.observed_installed_version), "RPC", guard_diagnostic_value(failure.observed_rpc_version)) +
           " " + recovery + " " + localization::format_translated_message("The {} and {} options do not bypass this guard.", "--devel", "--no-devel");
}
