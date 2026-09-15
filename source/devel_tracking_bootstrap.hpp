#pragma once

#include "installed_artifact_binding.hpp"
#include "package_metadata.hpp"
#include <vector>
#include "reviewed_source_state_store.hpp"

#include <memory>
#include <string>
#include <variant>

// Trial observations only. None of these values authorize a reviewed build,
// an installed-artifact proof, or provenance publication.
enum class DevelTrackingBootstrapUnavailableReason {
    ProvenanceNotMissing,
    InstalledStateUnavailable,
    UndeclaredInstalledChild,
    ReviewedStateInvalid,
    RecipeUnavailable,
    RecipeHeadProcessFailed,
    RecipeHeadTimedOut,
    RecipeHeadOutputLimitExceeded,
    RecipeHeadMalformed,
    RecipeHeadConflicting,
    RecipeMetadataUnavailable,
    RecipeMetadataMalformed,
    UnsupportedSource,
    UnsupportedSourceCount,
    MultipleTrackingSources,
    UnsupportedLocalSource,
    SourceDestinationCollision,
    CheckoutOverlayOrUnavailable,
    ObservationChanged,
};

struct DevelTrackingBootstrapUnavailable {
    DevelTrackingBootstrapUnavailableReason reason;
    bool operator==(const DevelTrackingBootstrapUnavailable&) const = default;
};

class DevelTrackingBootstrapTrial;
using DevelTrackingBootstrapObservation = std::variant<
    std::shared_ptr<const DevelTrackingBootstrapTrial>, DevelTrackingBootstrapUnavailable>;

struct DevelTrackingBootstrapChild {
    PackageChildIdentity package;
    InstalledArtifactBinding installed;
    // Cached by the observer for pure runner/preparation projections.
    std::string installed_version;
    bool operator==(const DevelTrackingBootstrapChild&) const = default;
};

class DevelTrackingBootstrapTrial final {
public:
    const PackageChildIdentity& package() const noexcept {
        return package_;
    }
    const SourceRevisionIdentity& recipe_revision() const noexcept {
        return recipe_revision_;
    }
    const std::string& source_metadata() const noexcept {
        return source_metadata_;
    }
    const ReviewedSourceStateStoreRead& reviewed() const noexcept {
        return reviewed_;
    }

    // D and I_db are observations; only selected children carry trial intent T.
    const std::vector<std::string>& declared_children() const noexcept {
        return declared_children_;
    }
    const InstalledPackageStateSnapshot& installed_group() const noexcept {
        return installed_group_;
    }
    const std::vector<DevelTrackingBootstrapChild>& selected_children() const noexcept {
        return selected_children_;
    }
    const DevelTrackingBootstrapChild* selected_child(const std::string& name) const noexcept {
        for(const auto& child : selected_children_)
            if(child.package.package_name() == name) return &child;
        return nullptr;
    }

private:
    DevelTrackingBootstrapTrial(PackageChildIdentity package, SourceRevisionIdentity recipe_revision,
                                std::string source_metadata, std::vector<DevelTrackingBootstrapChild> selected,
                                InstalledPackageStateSnapshot installed_group, std::vector<std::string> declared,
                                ReviewedSourceStateStoreRead reviewed);
    std::vector<DevelTrackingBootstrapChild> selected_children_;
    InstalledPackageStateSnapshot installed_group_;
    std::vector<std::string> declared_children_;
    PackageChildIdentity package_;
    SourceRevisionIdentity recipe_revision_;
    std::string source_metadata_;
    ReviewedSourceStateStoreRead reviewed_;

    friend DevelTrackingBootstrapObservation observe_devel_tracking_bootstrap(
        const std::vector<PackageChildIdentity>& packages);
};

// Read-only network/local observation. Never creates cache/state/workspaces,
// evaluates PKGBUILD, or calls an authoritative Git tracking observer.
DevelTrackingBootstrapObservation observe_devel_tracking_bootstrap(const PackageChildIdentity& package);
DevelTrackingBootstrapObservation observe_devel_tracking_bootstrap(const std::vector<PackageChildIdentity>& packages);
bool revalidate_devel_tracking_bootstrap(const DevelTrackingBootstrapTrial& trial);

#ifdef MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
#include "process.hpp"
#include <functional>
struct DevelTrackingBootstrapRecipeObservation {
    SourceRevisionIdentity revision;
    std::string srcinfo;
};
struct DevelTrackingBootstrapTestHooks {
    std::function<std::optional<DevelTrackingBootstrapRecipeObservation>(const PackageChildIdentity&)> recipe;
    std::function<bool(const PackageChildIdentity&)> checkout;
    // Raw boundaries retain production argv/policy, HEAD parsing and cgit URL
    // construction. Unlike recipe, these hooks do not supply a parsed identity.
    std::function<BoundedCapturedProcessResult(const ExplicitProcessInvocation&, const BoundedProcessPolicy&)> recipe_head = {};
    std::function<std::optional<std::string>(const std::string&)> recipe_metadata = {};
};
void set_devel_tracking_bootstrap_test_hooks(DevelTrackingBootstrapTestHooks hooks);
#endif
