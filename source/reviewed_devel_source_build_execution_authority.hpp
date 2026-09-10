#pragma once

#include <optional>
#include <variant>

class ValidatedCachePath;
class PinnedReviewedSourceBuild;
class ProductionArtifactSourceTree;
class PreparedReviewedDevelSourceBuildExecution;
class ReviewedDevelSourceBuildExecutionResult;
struct ReviewedDevelSourceBuildIntent;
struct ReviewedDevelSourceBuildRejected;
enum class ReviewedProductionExecutionChoice;
enum class ProductionReviewedSourceOutcome;
enum class ReviewedSourceAbnormalStateReason;

using ReviewedProductionSourceExecution = std::variant<
    ProductionArtifactSourceTree,
    PreparedReviewedDevelSourceBuildExecution,
    ReviewedDevelSourceBuildRejected>;

// Complete at both granting headers. This owner composes existing sealed
// producers; it receives no private access to S4, S5 or S6 construction.
class ReviewedDevelSourceBuildExecutionAuthority final {
    ReviewedDevelSourceBuildExecutionAuthority() = delete;
    friend ReviewedProductionSourceExecution prepare_reviewed_production_source_execution(
        ReviewedProductionExecutionChoice, ValidatedCachePath, PinnedReviewedSourceBuild,
        ProductionReviewedSourceOutcome, std::optional<ReviewedSourceAbnormalStateReason>,
        const ReviewedDevelSourceBuildIntent&);
    friend std::optional<ReviewedDevelSourceBuildExecutionResult> execute_reviewed_devel_source_build(
        PreparedReviewedDevelSourceBuildExecution) noexcept;

    static ReviewedProductionSourceExecution prepare(
        ReviewedProductionExecutionChoice, ValidatedCachePath, PinnedReviewedSourceBuild,
        ProductionReviewedSourceOutcome, std::optional<ReviewedSourceAbnormalStateReason>,
        const ReviewedDevelSourceBuildIntent&);
    static std::optional<ReviewedDevelSourceBuildExecutionResult> execute(
        PreparedReviewedDevelSourceBuildExecution) noexcept;
};
