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
                                                                          ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal, const ReviewedDevelSourceBuildIntent* intent, InvocationOwnedRecipeAcquisition* acquisition) {
    if(acquisition) throw std::logic_error("Legacy fixture received recipe acquisition.");
    if(intent && (intent->request.authoritative_devel_update || intent->request.devel_tracking_bootstrap)) throw std::logic_error("Legacy fixture received authoritative execution intent.");
    return make_reviewed_production_artifact_source_tree(checkout, std::move(pin), outcome, abnormal);
}
ReviewedDevelExecutionSnapshot execute_normal_reviewed_devel(PreparedReviewedDevelSourceBuildExecution, PresentationDetail) {
    throw std::logic_error("Legacy fixture entered authoritative execution.");
}

// This legacy-only profile cannot acquire or manufacture an isolated owner.
// Production bootstrap tests link the real factory and low-level transport seam.
struct InvocationOwnedRecipeAcquisition::State {};
InvocationOwnedRecipeAcquisition::InvocationOwnedRecipeAcquisition(InvocationOwnedRecipeAcquisition&&) noexcept = default;
InvocationOwnedRecipeAcquisition::~InvocationOwnedRecipeAcquisition() noexcept = default;
const ValidatedCachePath& InvocationOwnedRecipeAcquisition::checkout() const {
    throw std::logic_error("Legacy fixture has no recipe acquisition.");
}
const AurReviewedSourceReviewIdentity& InvocationOwnedRecipeAcquisition::identity() const {
    throw std::logic_error("Legacy fixture has no recipe acquisition.");
}
const std::filesystem::path& InvocationOwnedRecipeAcquisition::workspace_path() const {
    throw std::logic_error("Legacy fixture has no recipe acquisition.");
}
RecipeAcquisitionCleanupResult InvocationOwnedRecipeAcquisition::cleanup() noexcept {
    return std::nullopt;
}
RecipeAcquisitionResult acquire_invocation_owned_recipe(const DevelTrackingBootstrapTrial&) {
    throw std::logic_error("Legacy fixture cannot acquire a bootstrap recipe.");
}
