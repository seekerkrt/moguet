#pragma once
#include "aur_update_query.hpp"
#include "devel_package_assessment.hpp"
#include "aur_update_execution_preflight.hpp"

// Read-only target context evidence. Configured inventory must refer to the
// same fixed system DB world as 7-B; no inference across alternate roots.
struct AurDevelUpdateContextObservation {
    PacmanDatabasePaths configured_paths;
    std::optional<InstalledDatabaseWorldResult> trusted_world;
    std::optional<InstalledPackageStateSnapshotResult> installed_inventory;
};

// Complete local inventory is observed here, never reconstructed from a
// selected singleton or from persistent provenance. No remote cache/retry.
std::vector<AurDevelUpdateObservation> refine_aur_devel_updates(AurUpdatePlan& plan);

// Registered source refines through the same normal version/assessment owner.
AurUpdateQueryResult query_registered_aur_devel_update(const PackageBaseIdentity& base, const std::string& child);

#ifdef MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
void set_aur_devel_update_database_paths_for_test(std::optional<PacmanDatabasePaths> paths);
#endif

class SystemSourceUpgradeProjectionAuthority;
struct RegisteredAurDevelObservation {
    std::size_t preference_index;
    std::string package_name;
    std::string package_base;
    AurUpdateQueryResult query;
    std::vector<AurUpdateExecutionIssue> issues;
};
std::vector<RegisteredAurDevelObservation> observe_registered_aur_devel_updates(const SystemSourceUpgradeProjectionAuthority& prepared);

// Same complete configured/trusted installed context as the normal query.
AurDevelUpdateContextObservation observe_aur_devel_update_context();
// Called only from actual exact target-less system/AUR orchestration.
struct AppConfig;
void observe_aur_devel_bootstrap_candidates(AurUpdateQueryResult& query, const AppConfig& config);

bool is_initial_devel_bootstrap_observation(const AurUpdatePlanEntry& entry, const DevelPackageAssessment& evidence) noexcept;
