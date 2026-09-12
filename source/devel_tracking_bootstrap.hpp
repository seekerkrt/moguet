#pragma once

#include "installed_artifact_binding.hpp"
#include "reviewed_source_state_store.hpp"

#include <memory>
#include <string>
#include <variant>

// Trial observations only. None of these values authorize a reviewed build,
// an installed-artifact proof, or provenance publication.
enum class DevelTrackingBootstrapUnavailableReason {
    ProvenanceNotMissing,
    InstalledStateUnavailable,
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
    const InstalledArtifactBinding& installed() const noexcept {
        return installed_;
    }
    const std::string& installed_version() const noexcept {
        return installed_version_;
    }
    const ReviewedSourceStateStoreRead& reviewed() const noexcept {
        return reviewed_;
    }

private:
    DevelTrackingBootstrapTrial(PackageChildIdentity package, SourceRevisionIdentity recipe_revision,
                                std::string source_metadata, InstalledArtifactBinding installed,
                                ReviewedSourceStateStoreRead reviewed);
    PackageChildIdentity package_;
    SourceRevisionIdentity recipe_revision_;
    std::string source_metadata_;
    InstalledArtifactBinding installed_;
    std::string installed_version_;
    ReviewedSourceStateStoreRead reviewed_;

    friend DevelTrackingBootstrapObservation observe_devel_tracking_bootstrap(
        const PackageChildIdentity& package);
};

// Read-only network/local observation. Never creates cache/state/workspaces,
// evaluates PKGBUILD, or calls an authoritative Git tracking observer.
DevelTrackingBootstrapObservation observe_devel_tracking_bootstrap(const PackageChildIdentity& package);
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
