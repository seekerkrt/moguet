#include "aur_devel_update.hpp"
#include "package_identifier.hpp"
#include "system_source_upgrade.hpp"
#include <algorithm>

namespace {
#ifdef MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
std::optional<PacmanDatabasePaths> test_paths;
#endif
DevelPackageAssessment assess(const PackageBaseIdentity& base, const std::string& child,
                              const InstalledPackageStateSnapshot& inventory) {
    DevelPackageAssessmentTarget target{base, {}, devel_suffix_candidate_kind(base.package_base()).has_value() || devel_suffix_candidate_kind(child).has_value()};
    bool complete = true;
    for(const auto& [name, installed] : inventory) {
        if(!installed.package_base.value()) {
            complete = false;
            continue;
        }
        if(*installed.package_base.value() == base.package_base())
            target.installed_children.push_back(PackageChildIdentity::make(base, name));
    }
    const auto found = inventory.find(child);
    if(!complete || found == inventory.end() || !found->second.package_base.value() ||
       *found->second.package_base.value() != base.package_base()) {
        DevelPackageAssessment result;
        result.issue = DevelPackageAssessmentIssue::InvalidTarget;
        return result;
    }
    return assess_current_devel_package(target);
}
std::filesystem::path comparable_path(std::filesystem::path path) {
    if(path != path.root_path() && !path.has_filename()) path = path.parent_path();
    return path;
}
AurDevelUpdateContextObservation observe_context() {
    AurDevelUpdateContextObservation out;
    out.configured_paths = [&] {
#ifdef MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
        if(test_paths) return *test_paths;
#endif
        return resolve_pacman_database_paths();
    }();
    out.trusted_world = resolve_trusted_installed_database_world();
    const auto* world = std::get_if<InstalledDatabaseWorld>(&*out.trusted_world);
    if(world && comparable_path(out.configured_paths.root_dir) == comparable_path(world->root_directory) &&
       comparable_path(out.configured_paths.db_path) == comparable_path(world->database_path))
        out.installed_inventory = snapshot_installed_package_states(out.configured_paths);
    return out;
}
InstalledPackageStateSnapshot inventory() {
    auto context = observe_context();
    if(!context.installed_inventory) throw std::runtime_error("AUR devel assessment requires the configured inventory to match the trusted system DB world.");
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&*context.installed_inventory)) throw PackageMetadataError(*failure);
    return std::get<InstalledPackageStateSnapshot>(std::move(*context.installed_inventory));
}
} // namespace

std::vector<AurDevelUpdateObservation> refine_aur_devel_updates(AurUpdatePlan& plan) {
    const auto eligible = [](const auto& e) {
        return e.classification == AurUpdateClassification::UpToDate && e.aur_package && e.installed_name == e.aur_package->aur_name &&
               (e.aur_package->version_relation == AurVersionRelation::SameAsInstalled || e.aur_package->version_relation == AurVersionRelation::OlderThanInstalled);
    };
    if(std::none_of(plan.entries.begin(), plan.entries.end(), eligible)) return {};
    const auto context = std::make_shared<AurDevelUpdateContextObservation>(observe_context());
    const auto* installed = context->installed_inventory ? std::get_if<InstalledPackageStateSnapshot>(&*context->installed_inventory) : nullptr;
    std::vector<AurDevelUpdateObservation> observations;
    observations.reserve(plan.entries.size());
    for(std::size_t index = 0; index < plan.entries.size(); ++index) {
        auto& entry = plan.entries[index];
        if(!eligible(entry)) continue; // RPC-newer wins without a Git query.
        const auto& remote = *entry.aur_package;
        require_valid_package_name(remote.package_base);
        const auto base = PackageBaseIdentity::make(PackageSourceIdentity::aur(
                                                        SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/" + remote.package_base + ".git")),
                                                    remote.package_base);
        auto observed = std::make_shared<DevelPackageAssessment>();
        if(installed && entry.installed_name == remote.aur_name)
            *observed = assess(base, entry.installed_name, *installed);
        else
            observed->issue = DevelPackageAssessmentIssue::InvalidTarget;
        entry.devel_assessment = observed->assessment;
        entry.devel_assessment_origin = AurDevelAssessmentOrigin::CurrentObservation;
        observations.push_back({index, std::move(observed), context});
    }
    return observations;
}
AurUpdateQueryResult query_registered_aur_devel_update(const PackageBaseIdentity& base, const std::string& child) {
    require_valid_package_name(child);
    const auto expected = PackageBaseIdentity::make(PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote(
                                                        "https://aur.archlinux.org/" + base.package_base() + ".git")),
                                                    base.package_base());
    if(base != expected) throw std::runtime_error("Registered AUR source identity differs from the canonical AUR source.");
    const auto installed = inventory();
    const auto found = installed.find(child);
    if(found == installed.end()) throw std::runtime_error("Registered AUR update target is not installed.");
    auto query = query_aur_updates_for_foreign_inventory({found->second});
    if(query.plan.entries.size() != 1 || (query.plan.entries.front().aur_package && query.plan.entries.front().aur_package->package_base != base.package_base()))
        throw std::runtime_error("Registered AUR update target/source correlation failed.");
    return query;
}

#ifdef MOGUET_ENABLE_AUR_DEVEL_UPDATE_TEST_HOOKS
void set_aur_devel_update_database_paths_for_test(std::optional<PacmanDatabasePaths> paths) {
    test_paths = std::move(paths);
}
#endif

std::vector<RegisteredAurDevelObservation> observe_registered_aur_devel_updates(const SystemSourceUpgradeProjectionAuthority& prepared) {
    std::vector<RegisteredAurDevelObservation> result;
    const auto eligible = [](const auto& work) { return work.source().source_kind == SourceBuildSourceKind::Aur && work.only_if_updated(); };
    if(std::none_of(prepared.source_work_items().begin(), prepared.source_work_items().end(), eligible)) return result;
    const auto installed = inventory();
    for(const auto& work : prepared.source_work_items()) {
        if(!eligible(work)) continue;
        if(work.required_targets().size() != 1) throw std::logic_error("Registered AUR projection has no singular child context.");
        const auto& child = work.required_targets().front().package_name;
        if(!installed.contains(child)) continue; // Existing explicit new-source intent remains conditional.
        const auto& base_name = work.checkout_package_base();
        const auto base = PackageBaseIdentity::make(PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/" + base_name + ".git")), base_name);
        RegisteredAurDevelObservation observed{work.source().original_preference_index, child, base_name, query_registered_aur_devel_update(base, child), {}};
        const auto& entry = observed.query.plan.entries.front();
        AurUpdateExecutionIssue issue;
        issue.package_name = child;
        issue.package_base = base_name;
        switch(project_aur_update_effective_state(entry)) {
            case AurUpdateEffectiveState::UpdateAvailable:
            case AurUpdateEffectiveState::UpToDate: break;
            case AurUpdateEffectiveState::RequiresCheck:
                issue.reason = AurUpdateExecutionReason::DevelRequiresCheck;
                issue.devel_requires_check_reason = *entry.devel_assessment.requires_check_reason();
                issue.diagnostic = "Devel package requires explicit review/rebuild confirmation; dry-run does not authorize a build.";
                break;
            case AurUpdateEffectiveState::Unknown:
                issue.reason = AurUpdateExecutionReason::DevelObservationUnknown;
                issue.diagnostic = "Devel Git observation failed; no automatic build.";
                break;
            case AurUpdateEffectiveState::Unsupported:
                issue.reason = AurUpdateExecutionReason::DevelUnsupported;
                issue.diagnostic = "Devel automatic update is unsupported.";
                break;
            case AurUpdateEffectiveState::VersionComparisonUnavailable:
                issue.reason = AurUpdateExecutionReason::VersionComparisonUnavailable;
                issue.diagnostic = "AUR version comparison unavailable.";
                break;
            default:
                issue.reason = AurUpdateExecutionReason::AurMetadataUnavailable;
                issue.diagnostic = "AUR update metadata unavailable.";
                break;
        }
        if(issue.reason != AurUpdateExecutionReason::None) observed.issues.push_back(std::move(issue));
        result.push_back(std::move(observed));
    }
    return result;
}
