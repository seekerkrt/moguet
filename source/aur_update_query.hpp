#pragma once

#include "aur_update_plan.hpp"
#include "package_metadata.hpp"

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

AurUpdateQueryResult query_installed_aur_updates();
AurUpdateQueryResult query_aur_updates_for_foreign_inventory(
    ForeignPackageInventory installed_packages);
