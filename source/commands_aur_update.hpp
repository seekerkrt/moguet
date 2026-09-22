#pragma once

#include "filtered_aur_update_operation.hpp"
#include "presentation_detail.hpp"

#include <string>

struct AppConfig;

// Repository-internal presentation-only mappings for shared AUR update
// surfaces. They do not decide readiness or policy, reduce results, select
// execution, or determine process exit status.
std::string aur_update_preflight_reason_label(AurUpdateExecutionReason reason);
std::string aur_update_preparation_reason_label(
    AurUpdatePreparationReason reason);
bool is_routine_aur_update_skip(AurUpdateExecutionReason reason);

// Shared typed presentation for normal AUR update results. Callers provide
// the retained query/result authority; status is never reconstructed from
// localized text.
void present_filtered_aur_update_execution_result(
    const FilteredAurUpdateExecutionResult& result,
    PresentationDetail detail = PresentationDetail::Normal);

// Query/static preflight remains before the default state log. Only a
// prepared executable capability may cross into the normal mutation route.
PreparedFilteredAurUpdateOperation prepare_upgrade_aur_operation(
    const AppConfig& config);

// Consume the prepared capability, execute only when ready, and present the
// owned typed result for executable, blocked, and no-op outcomes.
int cmd_upgrade_aur(
    PreparedFilteredAurUpdateOperation prepared,
    const AppConfig& config);
