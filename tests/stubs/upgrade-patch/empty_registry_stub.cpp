#include "aur_upgrade_patch.hpp"
#include "source_install.hpp"

#include <stdexcept>

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
