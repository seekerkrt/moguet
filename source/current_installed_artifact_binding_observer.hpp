#pragma once

#include "current_installed_artifact_binding_observer_authority.hpp"
#include "installed_artifact_binding.hpp"
#include "installed_package_record_observation.hpp"

// A copyable semantic observation, not a transaction capability or a lease.
// Copies preserve the original observation; they do not refresh its time.
class CurrentInstalledArtifactBindingObserved final {
public:
    CurrentInstalledArtifactBindingObserved() = delete;
    CurrentInstalledArtifactBindingObserved(const CurrentInstalledArtifactBindingObserved&) = default;
    CurrentInstalledArtifactBindingObserved(CurrentInstalledArtifactBindingObserved&&) noexcept = default;
    CurrentInstalledArtifactBindingObserved& operator=(const CurrentInstalledArtifactBindingObserved&);
    CurrentInstalledArtifactBindingObserved& operator=(CurrentInstalledArtifactBindingObserved&&) noexcept = default;

    [[nodiscard]] const InstalledArtifactBinding& binding() const noexcept;
    [[nodiscard]] const InstalledDatabaseWorld& world() const noexcept;

private:
    friend class CurrentInstalledArtifactBindingObserver;
    CurrentInstalledArtifactBindingObserved(InstalledArtifactBinding binding, InstalledDatabaseWorld world) noexcept;
    InstalledArtifactBinding binding_;
    InstalledDatabaseWorld world_;
};

struct CurrentInstalledArtifactBindingAbsent {
    bool operator==(const CurrentInstalledArtifactBindingAbsent&) const = default;
};

enum class CurrentInstalledArtifactBindingStage {
    ExpectedIdentity,
    WorldResolution,
    RecordObservation,
    Projection,
    WorldRevalidation,
};
enum class CurrentInstalledArtifactBindingIssue {
    InvalidExpectedIdentity,
    MalformedBinding,
    InternalFailure,
};

// Low-level issues are retained verbatim. In particular RecordChanged is not
// relabelled as absence, and ResourceFailure needs no diagnostic allocation.
struct CurrentInstalledArtifactBindingFailure {
    CurrentInstalledArtifactBindingStage stage;
    std::variant<InstalledRecordObservationIssue, CurrentInstalledArtifactBindingIssue> cause;
    bool operator==(const CurrentInstalledArtifactBindingFailure&) const = default;
};

// The caller supplies a resolved AUR child/source correlation context. The
// local DB proves the child/base and installed bytes, never the AUR URL.
// Each call resolves its own fixed DB world and opens a fresh ALPM session.
// No transaction, XDG lookup/write, network, cache, or historical input.
[[nodiscard]] CurrentInstalledArtifactBindingObservation
observe_current_installed_artifact_binding(const PackageChildIdentity& expected) noexcept;

#ifdef MOGUET_ENABLE_CURRENT_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
using CurrentInstalledArtifactBindingTestHook = void (*)(CurrentInstalledArtifactBindingStage);
void set_current_installed_artifact_binding_test_hook(CurrentInstalledArtifactBindingTestHook hook);
#endif
