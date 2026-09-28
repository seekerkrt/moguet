#pragma once

#include "aur_rpc.hpp"
#include "local_patch_association.hpp"
#include "source_build.hpp"

#include <map>
#include <memory>

class ResolvedAurSourceBuildIdentity;
struct ProductionSourceBuildWorkItem;
struct AurUpdatePlan;
struct AurUpdateBuildUnitSelection;

// One selected association, one fresh checkout and one prepared build. Copies
// of work-item snapshots share the slot; they cannot cause a second execution.
class AurUpgradePatchCandidate final {
    struct State;
    std::unique_ptr<State> state_;
    explicit AurUpgradePatchCandidate(std::unique_ptr<State> state);
    friend class AurUpgradePatchSet;

public:
    ~AurUpgradePatchCandidate() noexcept;
    AurUpgradePatchCandidate(const AurUpgradePatchCandidate&) = delete;
    AurUpgradePatchCandidate& operator=(const AurUpgradePatchCandidate&) = delete;
    const ValidatedCacheRoot& recipe_cache() const;
    bool is_preparing() const noexcept;
    void apply_before_sealing(const ValidatedCachePath& checkout,
                              const ReviewedDevelSourceBuildIntent& intent,
                              bool reviewed_route, std::optional<std::string> upstream_version, const AppConfig& config);
    SourceBuildPreparationOutcome consume(const SourceBuildRequest& request,
                                          const ReviewedDevelSourceBuildIntent& intent);
    std::optional<std::string> upstream_version() const;
    std::optional<ProductionSourceBuildProvenance> provenance() const;
    void require_unchanged() const;
    void require_matches(const ProductionSourceBuildWorkItem& item) const;
    void cleanup();
};

// Selection is scoped to this invocation and exact source/base. Metadata-only
// planner passes never reacquire material or repeat an already answered prompt.
class AurUpgradePatchSet final {
    std::map<std::string, PackageBaseIdentity> examined_;
    std::map<std::string, std::shared_ptr<AurUpgradePatchCandidate>> selected_;
    AurRecipeMetadataSet metadata_;

public:
    ~AurUpgradePatchSet() noexcept;
    bool consider(const ResolvedAurSourceBuildIdentity& source,
                  SourceBuildRequest request,
                  std::vector<RequiredPackageArtifactTarget> required_targets,
                  const AppConfig& config);
    const AurRecipeMetadataSet& metadata() const noexcept {
        return metadata_;
    }
    void attach(ProductionSourceBuildWorkItem& item) const;
    void require_unchanged() const;
    void finish_preparation();
    void cleanup();
};

// Shared saved-preference reader for early custom candidate preparation. The
// final existing work-item owner independently revalidates the same environment.
SourceBuildEnvironment read_upgrade_patch_environment(
    const std::string& requested_child, const std::string& package_base,
    bool singular);

bool prepare_aur_upgrade_patch_roots(AurUpgradePatchSet& patches,
                                     const AurUpdatePlan& plan, const AppConfig& config);
bool prepare_aur_upgrade_patch_dependencies(AurUpgradePatchSet& patches,
                                            const BuildPlan& plan, const AurUpdateBuildUnitSelection& selection, const AppConfig& config);
