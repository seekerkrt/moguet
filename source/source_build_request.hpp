#pragma once
#include "source_environment.hpp"
#include "source_package_identity.hpp"
#include <memory>
#include <optional>
#include <string>
class ReviewedSourceFatalStatePreflightSlot;
class DevelTrackingBootstrapTrial;

// upgrade baselineの有無と、snapshot時点の未installを別状態として保持する。
struct SourceUpdateBaseline {
    std::optional<std::string> installed_version;
};

// authoritative snapshotの有無はrequest側のoptionalで表し、観測済みの未installと分ける。
struct SourceInstalledSnapshot {
    std::optional<std::string> installed_version;
};

struct SourceBuildRequest {
    std::string package_name;
    std::string checkout_name;
    std::string git_url;
    SourceBuildEnvironment custom_environment;
    SourceEnvironmentEmptyValuePolicy empty_value_policy =
        SourceEnvironmentEmptyValuePolicy::Omit;
    std::optional<SourceUpdateBaseline> update_baseline;
    std::optional<SourceInstalledSnapshot> installed_snapshot;
    bool only_if_updated = false;
    bool needed = false;
    std::optional<PackageBaseIdentity> aur_review_identity;
    // Invocation preparation owns this read. Copies of a prepared work item
    // share one consumable snapshot rather than minting another observation.
    std::shared_ptr<ReviewedSourceFatalStatePreflightSlot>
        reviewed_state_preflight;
    // Selection intent only; S4 owns actual built identity.
    bool authoritative_devel_update = false;
    std::shared_ptr<const DevelTrackingBootstrapTrial> devel_tracking_bootstrap;
};
