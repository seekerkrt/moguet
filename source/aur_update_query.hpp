#pragma once

#include "aur_update_plan.hpp"
#include "package_metadata.hpp"
#include "user_config.hpp"

#include <string>
#include <memory>
#include <cstddef>
#include <vector>

// 継続可能なquery失敗を、external exception型から切り離したowned diagnostic。
struct AurUpdateQueryFailure {
    std::vector<std::string> package_names;
    std::string diagnostic;

    bool operator==(const AurUpdateQueryFailure&) const = default;
};

struct DevelPackageAssessment;
struct AurDevelUpdateContextObservation;
struct AurDevelUpdateObservation {
    std::size_t plan_index;
    std::shared_ptr<const DevelPackageAssessment> evidence;
    std::shared_ptr<const AurDevelUpdateContextObservation> context = nullptr;
    // Trial diagnostics are separate from the original assessment and never
    // authorize execution. Empty also covers a trial that was not attempted.
    std::optional<DevelTrackingBootstrapUnavailable> bootstrap_unavailable = std::nullopt;
    bool operator==(const AurDevelUpdateObservation&) const = default;
};

// 1 invocation分のread-only query結果。presentationや終了statusの判断はcallerが所有する。
struct AurUpdateQueryResult {
    AurUpdatePlan plan;
    std::vector<AurUpdateQueryFailure> recoverable_failures;
    std::vector<AurDevelUpdateObservation> devel_observations = {};

    bool operator==(const AurUpdateQueryResult&) const = default;
};

enum class AurUpdateQueryPolicy {
    VersionOnly,
    TrackingEnabled,
};

// Public invocation defaults belong to UserConfig. Internal callers keep their
// compatibility default and public routes always pass the resolved policy.
inline AurUpdateQueryPolicy aur_update_query_policy(const UserConfig& config) noexcept {
    return config.updates.devel ? AurUpdateQueryPolicy::TrackingEnabled : AurUpdateQueryPolicy::VersionOnly;
}

// VersionOnly skips tracking I/O; it does not change build/install authority.
AurUpdateQueryResult query_installed_aur_updates(
    AurUpdateQueryPolicy policy = AurUpdateQueryPolicy::TrackingEnabled);
AurUpdateQueryResult query_aur_updates_for_foreign_inventory(
    ForeignPackageInventory installed_packages,
    AurUpdateQueryPolicy policy = AurUpdateQueryPolicy::TrackingEnabled);
