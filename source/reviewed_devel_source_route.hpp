#pragma once
#include "source_build.hpp"
// Classify the unmodified upstream recipe before customization can introduce
// an overlay. This uses the same intent/environment/install policy as execution.
bool requires_authoritative_devel_recipe(
    const std::string& upstream_srcinfo, const ReviewedDevelSourceBuildIntent& intent);
ReviewedProductionSourceExecution select_normal_reviewed_source_execution(
    const ValidatedCachePath& checkout, PinnedReviewedSourceBuild pin,
    ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal,
    const ReviewedDevelSourceBuildIntent* intent,
    InvocationOwnedRecipeAcquisition* acquisition = nullptr);
ReviewedDevelExecutionSnapshot execute_normal_reviewed_devel(PreparedReviewedDevelSourceBuildExecution prepared, PresentationDetail presentation_detail);
