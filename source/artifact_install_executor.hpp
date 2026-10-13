#pragma once

#include "artifact_identity.hpp"
#include "artifact_install_plan.hpp"
#include "artifact_workspace.hpp"
#include "dependency_plan.hpp"
#include "package_metadata.hpp"

#include <filesystem>
#include <optional>
#include <string>

struct ArtifactInstallPreparationOptions {
    bool needed = false;
    bool rm_deps = false;
};

struct ArtifactInstallExecutionOptions {
    bool no_confirm = false;
};

// pacman -Uのsuccessful return後に、preparation済みstateから確定する変更結果。
enum class ArtifactInstallExecutionOutcome {
    Installed,
    SkippedAsNeeded,
};

// InstalledPackageQueryResultからinstall policyへ渡すowned value。
// libalpm sessionやborrowed metadataを保持しない。
struct InstalledArtifactPolicyState {
    InstalledVersionState version_state;
    std::optional<ExistingInstallReason> existing_reason;
};

// Package absenceだけをNotInstalledへ写し、metadata failureや未知reasonはfail closedにする。
InstalledArtifactPolicyState map_installed_artifact_policy_state(
    const ArtifactPackageIdentity& identity,
    const InstalledPackageQueryResult& query_result);

class PreparedArtifactInstall final {
    std::string requested_name_;
    DesiredInstallReason desired_reason_;
    bool needed_ = false;
    ArtifactPackageIdentity identity_;
    ValidatedArtifactInstallTarget target_;
    InstalledVersionState installed_version_state_;
    std::optional<ExistingInstallReason> existing_reason_;
    InstallReasonDirective directive_;
    ValidatedPackageArtifactPath artifact_;

    PreparedArtifactInstall(
        std::string&& requested_name,
        DesiredInstallReason desired_reason,
        bool needed,
        ArtifactPackageIdentity&& identity,
        ValidatedArtifactInstallTarget&& target,
        InstalledVersionState installed_version_state,
        std::optional<ExistingInstallReason> existing_reason,
        InstallReasonDirective directive,
        ValidatedPackageArtifactPath&& artifact) noexcept;

    friend PreparedArtifactInstall prepare_artifact_install(
        ValidatedPackageArtifactPath& artifact,
        const std::string& requested_name,
        const std::string& package_base,
        DesiredInstallReason desired_reason,
        const ArtifactInstallPreparationOptions& options,
        const PacmanDatabasePaths& database_paths);
    friend ArtifactInstallExecutionOutcome execute_prepared_artifact_install(
        PreparedArtifactInstall& install,
        const ArtifactInstallExecutionOptions& options);

public:
    PreparedArtifactInstall(const PreparedArtifactInstall&) = delete;
    PreparedArtifactInstall& operator=(const PreparedArtifactInstall&) = delete;
    PreparedArtifactInstall(PreparedArtifactInstall&&) noexcept = default;
    PreparedArtifactInstall& operator=(PreparedArtifactInstall&&) = delete;
    ~PreparedArtifactInstall() noexcept = default;

    const std::filesystem::path& artifact_path() const noexcept {
        return artifact_.path();
    }

    const std::filesystem::path& workspace_path() const noexcept {
        return artifact_.workspace_path();
    }

    const ArtifactPackageIdentity& identity() const noexcept {
        return identity_;
    }

    const ValidatedArtifactInstallTarget& target() const noexcept {
        return target_;
    }

    InstalledVersionState installed_version_state() const noexcept {
        return installed_version_state_;
    }

    std::optional<ExistingInstallReason> existing_reason() const noexcept {
        return existing_reason_;
    }

    InstallReasonDirective directive() const noexcept {
        return directive_;
    }

    const std::string& requested_name() const noexcept {
        return requested_name_;
    }

    DesiredInstallReason desired_reason() const noexcept {
        return desired_reason_;
    }

    bool needed() const noexcept {
        return needed_;
    }

    void retain_workspace_for_diagnostics() noexcept;
    void cleanup_workspace();
};

// artifactは全preparation failure中borrowされ、成功時のaggregate構築でだけmoveされる。
PreparedArtifactInstall prepare_artifact_install(
    ValidatedPackageArtifactPath& artifact,
    const std::string& requested_name,
    const std::string& package_base,
    DesiredInstallReason desired_reason,
    const ArtifactInstallPreparationOptions& options,
    const PacmanDatabasePaths& database_paths);

// POLICY(#242): raw pathや個別directiveを受けず、相関済みaggregateだけをtransactionへ渡す。
ArtifactInstallExecutionOutcome execute_prepared_artifact_install(
    PreparedArtifactInstall& install,
    const ArtifactInstallExecutionOptions& options);
