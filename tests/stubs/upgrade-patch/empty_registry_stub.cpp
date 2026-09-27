#include "aur_upgrade_patch.hpp"
#include "source_install.hpp"
#include "review_recipe_patch_save.hpp"
#include "app_config.hpp"

#include <stdexcept>

#ifndef MOGUET_TEST_RECIPE_PATCH_SAVE
// Pure execution fixtures have no persistent-save owner. They cannot report a
// saved customization or silently exercise publication through a fake store.
void save_review_recipe_edit(const ReviewRecipeEditCorrelation* edit, bool unsupported,
                             const SourceBuildRequest&, const ValidatedCachePath&, const ValidatedCacheRoot&,
                             const AppConfig& config, const ReviewedDevelSourceBuildIntent*) {
    if((edit || unsupported) && !config.no_confirm)
        throw std::logic_error("Pure execution fixture reached persistent recipe save interaction.");
}
#endif

// Legacy pure runner/planner fixtures have no patch registry. This replacement
// models only absence, and cannot fabricate or execute a selected candidate.
// Actual selection/material/recipe behavior is tested by cli.upgrade_patch and
// the production bootstrap fixture, which do not link this file.
AurUpgradePatchSet::~AurUpgradePatchSet() noexcept = default;
bool AurUpgradePatchSet::consider(const ResolvedAurSourceBuildIdentity&, SourceBuildRequest,
                                  std::vector<RequiredPackageArtifactTarget>, const AppConfig&) {
    return false;
}
void AurUpgradePatchSet::attach(ProductionSourceBuildWorkItem&) const {
}
void AurUpgradePatchSet::require_unchanged() const {
}
void AurUpgradePatchSet::finish_preparation() {
}
void AurUpgradePatchSet::cleanup() {
}
bool prepare_aur_upgrade_patch_roots(AurUpgradePatchSet&, const AurUpdatePlan&, const AppConfig&) {
    return false;
}
bool prepare_aur_upgrade_patch_dependencies(AurUpgradePatchSet&, const BuildPlan&, const AurUpdateBuildUnitSelection&, const AppConfig&) {
    return false;
}

struct AurUpgradePatchCandidate::State {};
AurUpgradePatchCandidate::~AurUpgradePatchCandidate() noexcept = default;
const ValidatedCacheRoot& AurUpgradePatchCandidate::recipe_cache() const {
    throw std::logic_error("Empty registry fixture has no patch candidate.");
}
bool AurUpgradePatchCandidate::is_preparing() const noexcept {
    return false;
}
void AurUpgradePatchCandidate::apply_before_sealing(const ValidatedCachePath&, const ReviewedDevelSourceBuildIntent&, bool,
                                                    std::optional<std::string>, const AppConfig&) {
    throw std::logic_error("Empty registry fixture cannot apply patches.");
}
SourceBuildPreparationOutcome AurUpgradePatchCandidate::consume(const SourceBuildRequest&, const ReviewedDevelSourceBuildIntent&) {
    throw std::logic_error("Empty registry fixture cannot execute a patch candidate.");
}
std::optional<std::string> AurUpgradePatchCandidate::upstream_version() const {
    throw std::logic_error("Empty registry fixture has no patch metadata.");
}
std::optional<ProductionSourceBuildProvenance> AurUpgradePatchCandidate::provenance() const {
    throw std::logic_error("Empty registry fixture has no patch provenance.");
}
void AurUpgradePatchCandidate::require_unchanged() const {
    throw std::logic_error("Empty registry fixture has no patch candidate.");
}
void AurUpgradePatchCandidate::require_matches(const ProductionSourceBuildWorkItem&) const {
    throw std::logic_error("Empty registry fixture has no patch candidate.");
}
void AurUpgradePatchCandidate::cleanup() {
    throw std::logic_error("Empty registry fixture has no patch candidate.");
}
