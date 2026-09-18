#pragma once
#include "source_build.hpp"
ReviewedProductionSourceExecution select_normal_reviewed_source_execution(
    const ValidatedCachePath& checkout, PinnedReviewedSourceBuild pin,
    ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal,
    const ReviewedDevelSourceBuildIntent* intent,
    InvocationOwnedRecipeAcquisition* acquisition = nullptr);
ReviewedDevelExecutionSnapshot execute_normal_reviewed_devel(PreparedReviewedDevelSourceBuildExecution prepared);
