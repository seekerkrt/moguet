#include "reviewed_devel_source_route.hpp"
// Legacy-only unit profiles never construct an authoritative prepared value.
// No constructor/factory for a live S4/S5/S6 or Complete result is supplied.
struct ReviewedDevelSourceBuildExecutionState {};
PreparedReviewedDevelSourceBuildExecution::PreparedReviewedDevelSourceBuildExecution(PreparedReviewedDevelSourceBuildExecution&&) noexcept = default;
PreparedReviewedDevelSourceBuildExecution::~PreparedReviewedDevelSourceBuildExecution() noexcept = default;
bool PreparedReviewedDevelSourceBuildExecution::valid() const noexcept {
    return false;
}
ReviewedProductionSourceExecution select_normal_reviewed_source_execution(const ValidatedCachePath& checkout, PinnedReviewedSourceBuild pin,
                                                                          ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal, const ReviewedDevelSourceBuildIntent* intent) {
    if(intent && intent->request.authoritative_devel_update) throw std::logic_error("Legacy fixture received authoritative execution intent.");
    return make_reviewed_production_artifact_source_tree(checkout, std::move(pin), outcome, abnormal);
}
ReviewedDevelExecutionSnapshot execute_normal_reviewed_devel(PreparedReviewedDevelSourceBuildExecution) {
    throw std::logic_error("Legacy fixture entered authoritative execution.");
}
