#pragma once

#include <variant>

class PackageChildIdentity;
class CurrentInstalledArtifactBindingObserved;
struct CurrentInstalledArtifactBindingAbsent;
struct CurrentInstalledArtifactBindingFailure;
using CurrentInstalledArtifactBindingObservation = std::variant<
    CurrentInstalledArtifactBindingObserved,
    CurrentInstalledArtifactBindingAbsent,
    CurrentInstalledArtifactBindingFailure>;

// Complete at every granting header. Only the own-I/O entrance is a friend;
// raw snapshots, decoded bindings and result classes receive no mint access.
class CurrentInstalledArtifactBindingObserver final {
    CurrentInstalledArtifactBindingObserver() = delete;
    friend CurrentInstalledArtifactBindingObservation
    observe_current_installed_artifact_binding(const PackageChildIdentity&) noexcept;

    [[nodiscard]] static CurrentInstalledArtifactBindingObservation observe(
        const PackageChildIdentity& expected) noexcept;
};
