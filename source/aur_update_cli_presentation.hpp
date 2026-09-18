#pragma once

#include <optional>
#include <string>
#include <vector>

struct AurUpdateOperationResult;
struct AurUpdateOperationTargetResult;

// AUR execution resultからfilesystem capabilityやraw diagnosticを持たない
// user-facing lineだけを事前構築する。command層はstreamと周辺summaryを所有する。
struct AurUpdateCliPresentation {
    std::vector<std::string> summary_lines;
    std::vector<std::string> error_lines;
};

AurUpdateCliPresentation format_aur_update_cli_presentation(
    const AurUpdateOperationResult& result);

// target-level summaryでraw diagnosticへfallbackせず、decisive typed detailだけを
// safeなfailure categoryへ射影する。
std::string aur_update_cli_target_failure_summary(
    const AurUpdateOperationTargetResult& target);

struct CrossSourceVersionLockCorrelationResult;
struct CrossSourceCoordinatedTransitionPlan;

// Required confirmation presentation must propagate formatting failures.
std::string format_cross_source_transition_plan(
    const CrossSourceCoordinatedTransitionPlan& plan);

// Candidate indices remain the sole inclusion authority. Incomplete or invalid
// evidence cannot become a confirmed cause; formatting failure is secondary.
std::optional<std::string> format_cross_source_version_lock_cli_presentation(
    const CrossSourceVersionLockCorrelationResult& correlation) noexcept;
