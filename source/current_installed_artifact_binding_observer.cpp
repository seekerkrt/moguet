#include "current_installed_artifact_binding_observer.hpp"

#include "package_identifier.hpp"

#include <new>
#include <stdexcept>
#include <utility>

namespace {
using Stage = CurrentInstalledArtifactBindingStage;
using Issue = InstalledRecordObservationIssue;
#ifdef MOGUET_ENABLE_CURRENT_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
CurrentInstalledArtifactBindingTestHook g_current_binding_hook = nullptr;
#endif
} // namespace

CurrentInstalledArtifactBindingObserved::CurrentInstalledArtifactBindingObserved(
    InstalledArtifactBinding binding, InstalledDatabaseWorld world) noexcept
    : binding_(std::move(binding)), world_(std::move(world)) {
}
CurrentInstalledArtifactBindingObserved& CurrentInstalledArtifactBindingObserved::operator=(
    const CurrentInstalledArtifactBindingObserved& other) {
    // Preserve the binding/world pair if copying a diagnostic snapshot runs
    // out of memory. Only the completed copy replaces the destination.
    if(this != &other) {
        CurrentInstalledArtifactBindingObserved copy(other);
        *this = std::move(copy);
    }
    return *this;
}
const InstalledArtifactBinding& CurrentInstalledArtifactBindingObserved::binding() const noexcept {
    return binding_;
}
const InstalledDatabaseWorld& CurrentInstalledArtifactBindingObserved::world() const noexcept {
    return world_;
}

CurrentInstalledArtifactBindingObservation CurrentInstalledArtifactBindingObserver::observe(
    const PackageChildIdentity& expected) noexcept {
    Stage stage = Stage::ExpectedIdentity;
    try {
        const auto& base = expected.package_base();
        if(!is_valid_package_name(expected.package_name()) || !is_valid_package_name(base.package_base()) ||
           base.source().kind() != PackageSourceKind::Aur || !base.source().location().value() ||
           base.source().location().value()->empty()) {
            return CurrentInstalledArtifactBindingFailure{stage, CurrentInstalledArtifactBindingIssue::InvalidExpectedIdentity};
        }
        stage = Stage::WorldResolution;
        auto resolved = resolve_trusted_installed_database_world();
        if(const auto* issue = std::get_if<Issue>(&resolved)) return CurrentInstalledArtifactBindingFailure{stage, *issue};
        auto& world = std::get<InstalledDatabaseWorld>(resolved);
        stage = Stage::RecordObservation;
        auto observation = observe_installed_package_record(world, expected.package_name());
        if(const auto* issue = std::get_if<Issue>(&observation)) return CurrentInstalledArtifactBindingFailure{stage, *issue};

        stage = Stage::Projection;
#ifdef MOGUET_ENABLE_CURRENT_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
        if(g_current_binding_hook) g_current_binding_hook(stage);
#endif
        std::optional<InstalledArtifactBinding> binding;
        if(const auto* snapshot = std::get_if<InstalledPackageRecordSnapshot>(&observation)) {
            if(snapshot->package_name != expected.package_name() || snapshot->package_base != base.package_base())
                return CurrentInstalledArtifactBindingFailure{stage, Issue::MetadataMismatch};
            // The raw reader owns generation acquisition/reproof. Do not parse
            // its opaque filesystem handle or infer an ordering from it.
            if(snapshot->record_generation.empty())
                return CurrentInstalledArtifactBindingFailure{stage, Issue::UnsupportedGeneration};
            binding.emplace(InstalledArtifactBinding::make(
                expected, PackageVersionIdentity::composite(snapshot->full_version),
                InstalledPackageArchitectureIdentity::known(snapshot->architecture),
                AlpmMtreeSha256Digest::make(snapshot->raw_mtree_sha256),
                InstalledDatabaseRecordSha256Digest::make(snapshot->raw_database_sha256),
                InstalledPackageRecordGeneration(InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt, snapshot->record_generation)));
        }
        stage = Stage::WorldRevalidation;
#ifdef MOGUET_ENABLE_CURRENT_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
        if(g_current_binding_hook) g_current_binding_hook(stage);
#endif
        const auto after = resolve_trusted_installed_database_world();
        if(const auto* issue = std::get_if<Issue>(&after)) return CurrentInstalledArtifactBindingFailure{stage, *issue};
        if(std::get<InstalledDatabaseWorld>(after) != world)
            return CurrentInstalledArtifactBindingFailure{stage, Issue::DatabaseWorldMismatch};
        // Absence needs the same final world check as a present record. This
        // boundary does not hold a package-manager lock or promise future state.
        if(!binding) return CurrentInstalledArtifactBindingAbsent{};
        return CurrentInstalledArtifactBindingObserved(std::move(*binding), std::move(world));
    } catch(const std::bad_alloc&) {
        return CurrentInstalledArtifactBindingFailure{stage, Issue::ResourceFailure};
    } catch(const std::length_error&) {
        return CurrentInstalledArtifactBindingFailure{stage, Issue::ResourceFailure};
    } catch(const std::invalid_argument&) {
        return CurrentInstalledArtifactBindingFailure{stage, CurrentInstalledArtifactBindingIssue::MalformedBinding};
    } catch(...) {
        return CurrentInstalledArtifactBindingFailure{stage, CurrentInstalledArtifactBindingIssue::InternalFailure};
    }
}

CurrentInstalledArtifactBindingObservation observe_current_installed_artifact_binding(
    const PackageChildIdentity& expected) noexcept {
    return CurrentInstalledArtifactBindingObserver::observe(expected);
}

#ifdef MOGUET_ENABLE_CURRENT_INSTALLED_ARTIFACT_BINDING_TEST_HOOKS
void set_current_installed_artifact_binding_test_hook(CurrentInstalledArtifactBindingTestHook hook) {
    g_current_binding_hook = hook;
}
#endif
