#include "aur_update_execution_preparation.hpp"
#include "build_plan_artifact_target_projection.hpp"
#include "aur_upgrade_patch.hpp"

#include "app_config.hpp"
#include "cache_authority.hpp"
#include "local_source_metadata_evaluation.hpp"
#include "localization.hpp"
#include "logging.hpp"
#include "package_metadata.hpp"
#include "recipe_patch_review.hpp"
#include "reviewed_devel_source_route.hpp"
#include "source_install.hpp"
#include "source_preference.hpp"
#include "terminal_safe_text.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace {

[[noreturn]] void unsupported_devel() {
    throw std::runtime_error(localization::translate_message(
        "Saved recipe patches are not supported by authoritative devel execution. Update stopped; no stock or legacy fallback was attempted."));
}

[[noreturn]] void patch_failure(const PatchAssociationFailure& failure) {
    using Kind = PatchAssociationFailureKind;
    std::string reason;
    switch(failure.kind) {
        case Kind::Missing: reason = localization::translate_message("missing patch material or association"); break;
        case Kind::Changed: reason = localization::translate_message("patch material digest changed"); break;
        case Kind::Unsafe: reason = localization::translate_message("unsafe patch customization"); break;
        case Kind::Corrupt: reason = localization::translate_message("corrupt patch association"); break;
        case Kind::Unsupported: reason = localization::translate_message("unsupported patch association"); break;
        case Kind::InvalidMaterial: reason = localization::translate_message("invalid patch material"); break;
        case Kind::InvalidIdentity:
        case Kind::AssociationMismatch: reason = localization::translate_message("patch association identity mismatch"); break;
        case Kind::ConcurrentChange: reason = localization::translate_message("patch association changed during the operation"); break;
        default: reason = localization::translate_message("patch customization I/O failure"); break;
    }
    throw std::runtime_error(localization::format_translated_message(
        "Saved patch customization failed: {} ({}). Update stopped.", reason,
        terminal_safe_text::escape_utf8(failure.path.string())));
}

template <class Value, class Result>
Value require_patch_value(Result result) {
    if(const auto* failure = std::get_if<PatchAssociationFailure>(&result)) patch_failure(*failure);
    return std::get<Value>(std::move(result));
}

SourceBuildEnvironment effective_environment(const SourceBuildRequest& request) {
    SourceBuildEnvironment result;
    for(const auto& assignment : request.custom_environment.ordered_assignments) {
        if(request.empty_value_policy == SourceEnvironmentEmptyValuePolicy::Omit && assignment.value.empty()) continue;
        result.ordered_assignments.push_back(assignment);
    }
    return result;
}

void confirm_evaluation(const LocalSourceRoot& root, const AppConfig& config, bool patched) {
    if(auto stop = offer_patch_recipe_review(root.pkgbuild(), config)) throw ConfirmationOperationStopped(*stop);
    root.require_unchanged_identity();
    auto result = request_confirmation(
        patched ? localization::format_translated_message("Evaluate patched {} metadata with {}?", "PKGBUILD", "makepkg --printsrcinfo")
                : localization::format_translated_message("Evaluate {} metadata with {}?", "PKGBUILD", "makepkg --printsrcinfo"),
        ConfirmationDefault::None, config.no_confirm);
    const auto* accepted = std::get_if<ConfirmationAccepted>(&result);
    if(!accepted || accepted->origin != ConfirmationDecisionOrigin::ExplicitToken)
        throw ConfirmationOperationStopped(std::move(result));
}

AurRecipeMetadataSet project_recipe(const LocalSourceBuildMetadata& evaluated) {
    const auto& metadata = evaluated.metadata();
    const auto& architecture = evaluated.effective_architecture();
    AurRecipeMetadataSet result;
    for(const auto& child : metadata.children) {
        const auto& architectures = child.has_architecture_override ? child.architectures : metadata.architectures;
        AurPackageInfo package;
        package.recipe_architecture_supported = std::find(architectures.begin(), architectures.end(), "any") != architectures.end() ||
                                                std::find(architectures.begin(), architectures.end(), architecture) != architectures.end();
        package.Name = child.name;
        package.PackageBase = metadata.package_base;
        package.Version = (metadata.epoch && metadata.epoch->find_first_not_of('0') != std::string::npos ? *metadata.epoch + ":" : "") + metadata.pkgver + "-" + metadata.pkgrel;
        package.metadata_origin = AurPackageMetadataOrigin::EvaluatedRecipe;
        for(const auto& relation : metadata.relations) {
            if(relation.architecture_qualifier && *relation.architecture_qualifier != architecture) continue;
            if(relation.scope.kind == LocalPackageMetadataScopeKind::ChildPackage) {
                if(relation.scope.package_name != child.name) continue;
            } else if(std::any_of(metadata.relations.begin(), metadata.relations.end(), [&](const auto& override) {
                          return override.scope.kind == LocalPackageMetadataScopeKind::ChildPackage &&
                                 override.scope.package_name == child.name && override.kind == relation.kind &&
                                 override.architecture_qualifier == relation.architecture_qualifier;
                      }))
                continue;
            if(relation.is_explicit_unset) continue;
            switch(relation.kind) {
                case LocalPackageMetadataRelationKind::Depends: package.Depends.push_back(relation.raw_value); break;
                case LocalPackageMetadataRelationKind::Makedepends: package.MakeDepends.push_back(relation.raw_value); break;
                case LocalPackageMetadataRelationKind::Checkdepends: package.CheckDepends.push_back(relation.raw_value); break;
                case LocalPackageMetadataRelationKind::Optdepends: package.OptDepends.push_back(relation.raw_value); break;
                case LocalPackageMetadataRelationKind::Provides: package.Provides.push_back(relation.raw_value); break;
                case LocalPackageMetadataRelationKind::Conflicts: package.Conflicts.push_back(relation.raw_value); break;
                case LocalPackageMetadataRelationKind::Replaces: package.Replaces.push_back(relation.raw_value); break;
            }
        }
        auto projection = project_aur_constraint_metadata(package);
        if(!std::holds_alternative<AurPackageConstraintMetadata>(projection))
            throw std::runtime_error(localization::translate_message("Patched recipe dependency metadata is invalid."));
        package.constraint_metadata = std::get<AurPackageConstraintMetadata>(std::move(projection));
        result.emplace(child.name, std::move(package));
    }
    return result;
}

void require_same_identity(const LocalPackageMetadata& before, const LocalPackageMetadata& after) {
    if(before.package_base != after.package_base || before.children.size() != after.children.size())
        throw std::runtime_error(localization::format_translated_message("Patched recipe changed {} or child identity.", "PackageBase"));
    for(std::size_t i = 0; i < before.children.size(); ++i)
        if(before.children[i].name != after.children[i].name)
            throw std::runtime_error(localization::format_translated_message("Patched recipe changed {} or child identity.", "PackageBase"));
}

bool same_environment(const SourceBuildRequest& a, const SourceBuildRequest& b) {
    return a.empty_value_policy == b.empty_value_policy &&
           materialize_source_build_environment_assignment_words(a.custom_environment, SourceEnvironmentEmptyValuePolicy::Forward) ==
               materialize_source_build_environment_assignment_words(b.custom_environment, SourceEnvironmentEmptyValuePolicy::Forward);
}

} // namespace

struct AurUpgradePatchCandidate::State {
    PackageBaseIdentity identity;
    AcquiredLocalRecipeSeries series;
    SourceBuildRequest request;
    LocalSourceWorkspace workspace;
    ValidatedCacheRoot recipe_cache;
    std::optional<SourceBuildPreparationOutcome> prepared;
    std::optional<LocalSourceRoot> modified;
    std::optional<LocalSourceBuildMetadata> metadata;
    std::string upstream_srcinfo;
    std::optional<std::string> upstream_version;
    bool reviewed_route = false;
    bool preparing = true;
    bool consumed = false;
    bool cleaned = false;
    bool handed_off = false;

    State(PackageBaseIdentity source, AcquiredLocalRecipeSeries acquired, SourceBuildRequest input,
          LocalSourceWorkspace root, ValidatedCacheRoot cache)
        : identity(std::move(source)), series(std::move(acquired)), request(std::move(input)),
          workspace(std::move(root)), recipe_cache(std::move(cache)) {
    }
};

AurUpgradePatchCandidate::AurUpgradePatchCandidate(std::unique_ptr<State> state) : state_(std::move(state)) {
}
AurUpgradePatchCandidate::~AurUpgradePatchCandidate() noexcept {
    try {
        cleanup();
    } catch(const std::exception& error) {
        Logger::warn_noexcept([&] { return std::string(error.what()); });
    } catch(...) {
        Logger::warn_noexcept([] { return localization::translate_message("Saved patch candidate cleanup failed."); });
    }
}
const ValidatedCacheRoot& AurUpgradePatchCandidate::recipe_cache() const {
    return state_->recipe_cache;
}
bool AurUpgradePatchCandidate::is_preparing() const noexcept {
    return state_->preparing;
}

void AurUpgradePatchCandidate::apply_before_sealing(
    const ValidatedCachePath& checkout, const ReviewedDevelSourceBuildIntent& intent,
    bool reviewed_route, std::optional<std::string> upstream_version, const AppConfig& config) {
    auto& state = *state_;
    if(!state.preparing || state.modified || checkout.canonical_path().parent_path() != state.recipe_cache.canonical_path() ||
       checkout.canonical_path().filename() != state.identity.package_base())
        throw std::logic_error(localization::format_translated_message("Invalid {} patch candidate lifecycle.", "AUR"));
    state.workspace.require_unchanged_identity();
    auto before = open_local_source_root(checkout.canonical_path(), true);
    if(const auto* file = before.metadata().file()) state.upstream_srcinfo = file->contents;
    state.reviewed_route = reviewed_route;
    state.upstream_version = std::move(upstream_version);
    if(intent.request.authoritative_devel_update || intent.request.devel_tracking_bootstrap ||
       (reviewed_route && requires_authoritative_devel_recipe(state.upstream_srcinfo, intent))) unsupported_devel();
    confirm_evaluation(before, config, false);
    auto environment = effective_environment(state.request);
    const auto architecture = resolve_local_source_effective_architecture(environment);
    auto baseline = evaluate_recipe_metadata(before, environment, architecture, state.request.empty_value_policy);
    if(baseline.metadata().package_base != state.identity.package_base())
        throw std::runtime_error(localization::translate_message("Current upstream recipe does not match the saved patch association."));
    for(const auto& required : intent.required_targets) {
        const auto child = std::find_if(baseline.metadata().children.begin(), baseline.metadata().children.end(),
                                        [&](const auto& value) { return value.name == required.package_name; });
        if(required.package_base != state.identity.package_base() || child == baseline.metadata().children.end())
            throw std::runtime_error(localization::translate_message("Current upstream recipe does not match the saved patch association."));
    }
    LocalRecipeCandidateFailure failure{LocalRecipeCandidatePhase::Preflight, LocalRecipeCandidateFailureReason::InvalidMaterial, {}, std::nullopt, std::nullopt, checkout.canonical_path()};
    try {
        auto patches = std::move(state.series).take_patches();
        state.modified.emplace(apply_recipe_patch_series(before, patches, failure));
    } catch(const std::exception& error) {
        throw std::runtime_error(localization::format_translated_message(
            "Saved recipe patch application failed: {}. Update stopped; no stock fallback was attempted.", error.what()));
    }
    confirm_evaluation(*state.modified, config, true);
    state.metadata.emplace(evaluate_recipe_metadata(*state.modified, environment, architecture, state.request.empty_value_policy));
    require_same_identity(baseline.metadata(), state.metadata->metadata());
    require_unchanged();
}

std::optional<std::string> AurUpgradePatchCandidate::upstream_version() const {
    return state_->upstream_version;
}
std::optional<ProductionSourceBuildProvenance> AurUpgradePatchCandidate::provenance() const {
    if(state_->prepared) {
        if(const auto* pending = std::get_if<PreparedSourceBuildNeedsBuild>(&*state_->prepared)) return pending->recipe_provenance();
    }
    return std::nullopt;
}

void AurUpgradePatchCandidate::require_unchanged() const {
    if(state_->cleaned) throw std::logic_error(localization::format_translated_message("{} patch candidate already cleaned.", "AUR"));
    state_->workspace.require_unchanged_identity();
    if(state_->modified) {
        state_->modified->require_unchanged_identity();
        if(state_->metadata) state_->metadata->require_matches(*state_->modified);
    }
}

void AurUpgradePatchCandidate::require_matches(const ProductionSourceBuildWorkItem& item) const {
    const auto& request = item.request;
    if(state_->request.installed_snapshot && request.installed_snapshot &&
       state_->request.installed_snapshot->installed_version != request.installed_snapshot->installed_version)
        throw std::runtime_error(localization::translate_message("Installed package state changed after patch candidate preparation. Update stopped."));
    if(!request.aur_review_identity || *request.aur_review_identity != state_->identity ||
       request.checkout_name != state_->identity.package_base() || request.git_url != state_->request.git_url ||
       !same_environment(request, state_->request))
        throw std::runtime_error(localization::translate_message("Patch candidate identity or build environment changed after metadata evaluation."));
    require_unchanged();
}

SourceBuildPreparationOutcome AurUpgradePatchCandidate::consume(
    const SourceBuildRequest& request, const ReviewedDevelSourceBuildIntent& intent) {
    if(state_->preparing || state_->consumed || !state_->prepared)
        throw std::logic_error(localization::format_translated_message("{} patch candidate is unavailable or already consumed.", "AUR"));
    ProductionSourceBuildWorkItem item;
    item.request = request;
    require_matches(item);
    // Final child/reason shape may differ from root discovery. Reclassify the
    // retained UNMODIFIED upstream metadata, never the patched overlay.
    if(request.authoritative_devel_update || request.devel_tracking_bootstrap ||
       (state_->reviewed_route && requires_authoritative_devel_recipe(state_->upstream_srcinfo, intent))) unsupported_devel();
    if(!state_->metadata) throw std::logic_error(localization::translate_message("Selected patch candidate has no fresh metadata."));
    for(const auto& target : intent.required_targets) {
        if(target.package_base != state_->identity.package_base() ||
           std::none_of(state_->metadata->metadata().children.begin(), state_->metadata->metadata().children.end(),
                        [&](const auto& child) { return child.name == target.package_name; }))
            throw std::runtime_error(localization::format_translated_message("Patched recipe changed {} or child identity.", "PackageBase"));
    }
    state_->consumed = true;
    auto result = std::move(*state_->prepared);
    state_->prepared.reset();
    return result;
}

void AurUpgradePatchCandidate::cleanup() {
    if(state_->cleaned) return;
    state_->prepared.reset(); // release the source lease before recursive cleanup
    state_->modified.reset();
    state_->cleaned = true; // the workspace cleanup itself is one-shot, including failure
    try {
        state_->workspace.cleanup();
    } catch(const std::exception& error) {
        throw std::runtime_error(localization::format_translated_message(
            "Saved patch candidate cleanup failed: {}. Inspect the retained workspace: {}",
            error.what(), terminal_safe_text::escape_utf8(state_->workspace.path().string())));
    }
}

AurUpgradePatchSet::~AurUpgradePatchSet() noexcept {
    for(const auto& [base, candidate] : selected_) {
        if(candidate->state_->handed_off) continue;
        try {
            candidate->cleanup();
        } catch(const std::exception& error) {
            Logger::warn_noexcept([&] { return std::string(error.what()); });
        } catch(...) {
            Logger::warn_noexcept([] { return localization::translate_message("Saved patch candidate cleanup failed."); });
        }
    }
}

bool AurUpgradePatchSet::consider(const ResolvedAurSourceBuildIdentity& source,
                                  SourceBuildRequest request,
                                  std::vector<RequiredPackageArtifactTarget> targets,
                                  const AppConfig& config) {
    auto identity = require_patch_value<PackageBaseIdentity>(aur_patch_association_identity(source));
    const auto& base = identity.package_base();
    if(request.checkout_name != base || request.git_url != source.checkout().git_url() || targets.empty() ||
       (request.aur_review_identity && *request.aur_review_identity != identity) ||
       std::none_of(targets.begin(), targets.end(), [&](const auto& target) { return target.package_name == source.requested_name(); }))
        throw std::runtime_error(localization::format_translated_message("Current {} source identity changed before patch discovery.", "AUR"));
    for(const auto& target : targets) {
        if(target.package_base != base) throw std::runtime_error(localization::format_translated_message("Current {} source identity changed before patch discovery.", "AUR"));
        static_cast<void>(PackageChildIdentity::make(identity, target.package_name));
    }
    if(const auto found = examined_.find(base); found != examined_.end()) {
        if(found->second != identity) throw std::logic_error(localization::format_translated_message("{} patch source identity changed.", "AUR"));
        return false;
    }
    auto found = read_patch_association(identity);
    if(const auto* failure = std::get_if<PatchAssociationFailure>(&found)) patch_failure(*failure);
    examined_.emplace(base, identity);
    if(std::holds_alternative<PatchAssociationAbsent>(found)) return false;
    Logger::info(localization::format_translated_message("Saved patch customization found for {}.", base));
    auto answer = request_confirmation(localization::format_translated_message("Apply saved patch customization to this update of {}?", base), ConfirmationDefault::No, config.no_confirm);
    if(std::holds_alternative<ConfirmationDeclined>(answer)) return false;
    const auto* accepted = std::get_if<ConfirmationAccepted>(&answer);
    if(!accepted || accepted->origin != ConfirmationDecisionOrigin::ExplicitToken)
        throw ConfirmationOperationStopped(std::move(answer));
    // upgrade-aur captures read-only preflight diagnostics before its default
    // state log starts. Explicit recipe preparation must show commands NOW,
    // not replay them only after clone/evaluation have already happened.
    Logger::flush_diagnostic_capture();
    if(request.authoritative_devel_update || request.devel_tracking_bootstrap) unsupported_devel();
    if(!request.only_if_updated)
        request.custom_environment = read_upgrade_patch_environment(source.requested_name(), base, targets.size() == 1);
    const auto database = resolve_pacman_database_paths();
    // Registered upgrades need a pre-system snapshot only for a selected
    // candidate. Absence/No must retain the stock query order and failures.
    if(request.only_if_updated) {
        if(targets.size() != 1 || request.package_name != source.requested_name())
            throw std::logic_error(localization::format_translated_message("Current {} source identity changed before patch discovery.", "AUR"));
        auto session = PackageMetadataSession::open(database);
        const auto installed = session.query_installed_package(source.requested_name());
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&installed)) throw PackageMetadataError(*failure);
        request.installed_snapshot = SourceInstalledSnapshot{};
        targets.front().desired_reason = DesiredInstallReason::Explicit;
        if(const auto* present = std::get_if<InstalledPackageMetadata>(&installed)) {
            request.installed_snapshot->installed_version = present->version;
            if(present->reason == InstalledPackageReason::Unknown)
                throw std::runtime_error(localization::format_translated_message("Registered {} install reason is unknown.", "AUR"));
            if(present->reason == InstalledPackageReason::Dependency) targets.front().desired_reason = DesiredInstallReason::Dependency;
        }
    }
    auto series = require_patch_value<AcquiredLocalRecipeSeries>(acquire_aur_patch_series(std::get<LoadedPatchAssociation>(found)));
    auto cache = prepare_process_cache_root();
    auto workspace = create_aur_patch_workspace(cache);
    xdg_paths::EnvironmentSnapshot environment;
    environment.xdg_cache_home = workspace.path().string();
    auto paths = xdg_paths::resolve_cache(environment);
    auto recipe_cache = adopt_trusted_cache_root(paths, xdg_directory_safety::prepare_directory(paths));
    request.upgrade_patch.reset();
    request.aur_review_identity = identity;
    auto candidate = std::shared_ptr<AurUpgradePatchCandidate>(new AurUpgradePatchCandidate(std::make_unique<AurUpgradePatchCandidate::State>(
        identity, std::move(series), request, std::move(workspace), std::move(recipe_cache))));
    selected_.emplace(base, candidate);
    request.upgrade_patch = candidate;
    const ReviewedDevelSourceBuildIntent intent{request, std::move(targets), database, config.rm_deps, {config.no_confirm}};
    try {
        candidate->state_->prepared.emplace(prepare_source_build_for_execution(
            request, base, SourceBuildUpdatePolicy::AlwaysBuild,
            cache, config, &intent));
        candidate->state_->preparing = false;
        candidate->require_unchanged();
        if(candidate->state_->metadata) {
            auto projected = project_recipe(*candidate->state_->metadata);
            for(auto& [name, package] : projected) {
                if(!metadata_.emplace(name, std::move(package)).second)
                    throw std::runtime_error(localization::format_translated_message("Conflicting evaluated {} child identity.", "AUR"));
            }
        }
    } catch(...) {
        const auto primary = std::current_exception();
        try {
            cleanup();
        } catch(const std::exception& error) {
            Logger::warn_noexcept([&] { return std::string(error.what()); });
        }
        std::rethrow_exception(primary);
    }
    return true;
}

void AurUpgradePatchSet::attach(ProductionSourceBuildWorkItem& item) const {
    const auto found = selected_.find(item.request.checkout_name);
    if(found == selected_.end()) return;
    found->second->require_matches(item);
    item.request.upgrade_patch = found->second;
    found->second->state_->handed_off = true;
}
void AurUpgradePatchSet::require_unchanged() const {
    for(const auto& [base, candidate] : selected_)
        candidate->require_unchanged();
}
void AurUpgradePatchSet::finish_preparation() {
    for(const auto& [base, candidate] : selected_) {
        if(!candidate->state_->handed_off) candidate->cleanup();
    }
}
void AurUpgradePatchSet::cleanup() {
    std::exception_ptr first;
    for(const auto& [base, candidate] : selected_) {
        try {
            candidate->cleanup();
        } catch(...) {
            if(!first) first = std::current_exception();
        }
    }
    if(first) std::rethrow_exception(first);
}

SourceBuildEnvironment read_upgrade_patch_environment(const std::string& requested, const std::string& base, bool singular) {
    const auto read = [](const std::string& name) {
        auto result = read_source_preference_strict(name);
        if(const auto* absent = std::get_if<SourcePreferenceAbsent>(&result)) {
            static_cast<void>(absent);
            return SourceBuildEnvironment{};
        }
        if(const auto* loaded = std::get_if<SourcePreferenceLoaded>(&result)) return loaded->environment;
        throw std::runtime_error(std::get<SourcePreferenceFailure>(result).diagnostic);
    };
    auto environment = read(singular ? requested : base);
    if(singular && requested != base && !environment.has_forwarded_nonempty_assignment() && !environment.defines("PKGDEST"))
        environment = read(base);
    require_unclaimed_artifact_pkgdest(environment);
    return environment;
}

bool prepare_aur_upgrade_patch_roots(AurUpgradePatchSet& patches, const AurUpdatePlan& plan, const AppConfig& config) {
    // Query-level blockers do not require recipe execution to establish. Keep
    // them ahead of every custom candidate mutation; graph checks follow fresh metadata.
    for(const auto& entry : plan.entries) {
        const auto state = project_aur_update_effective_state(entry);
        if((state == AurUpdateEffectiveState::UpdateAvailable || entry.bootstrap) &&
           (!entry.aur_package || entry.aur_package->aur_name != entry.installed_name ||
            entry.install_reason == InstalledPackageReason::Unknown)) return false;
        if(state != AurUpdateEffectiveState::UpdateAvailable && state != AurUpdateEffectiveState::UpToDate &&
           state != AurUpdateEffectiveState::NonAurForeign && !entry.bootstrap) return false;
    }
    std::map<std::string, std::vector<const AurUpdatePlanEntry*>> groups;
    for(const auto& entry : plan.entries) {
        if(project_aur_update_effective_state(entry) != AurUpdateEffectiveState::UpdateAvailable && !entry.bootstrap) continue;
        if(!entry.aur_package) throw std::runtime_error(localization::format_translated_message("{} patch discovery requires exact update metadata.", "AUR"));
        groups[entry.aur_package->package_base].push_back(&entry);
    }
    struct RootDraft {
        ResolvedAurSourceBuildIdentity source;
        SourceBuildRequest request;
        std::vector<RequiredPackageArtifactTarget> targets;
    };
    std::vector<RootDraft> drafts;
    // Resolve every root's exact source before evaluating any recipe. Saved
    // environment belongs to the stock owner unless custom input is selected.
    for(const auto& [base, entries] : groups) {
        std::vector<RequiredPackageArtifactTarget> targets;
        bool forced = false;
        for(const auto* entry : entries) {
            auto resolved = resolve_source_build_identity(entry->installed_name);
            if(!resolved.aur_identity() || resolved.package_base() != base)
                throw std::runtime_error(localization::format_translated_message("Current {} source identity changed before patch discovery.", "AUR"));
            if(entry->install_reason == InstalledPackageReason::Unknown)
                throw std::runtime_error(localization::format_translated_message("{} update install reason is unknown.", "AUR"));
            targets.push_back({base, entry->installed_name, entry->install_reason == InstalledPackageReason::Explicit ? DesiredInstallReason::Explicit : DesiredInstallReason::Dependency});
            forced = forced || aur_update_basis(*entry) == AurUpdateBasis::GitRevision || static_cast<bool>(entry->bootstrap);
        }
        const auto resolved = resolve_source_build_identity(entries.front()->installed_name);
        if(!resolved.aur_identity() || resolved.package_base() != base)
            throw std::runtime_error(localization::format_translated_message("Current {} source identity changed before patch discovery.", "AUR"));
        SourceBuildRequest request;
        request.package_name = targets.size() == 1 ? targets.front().package_name : "";
        request.checkout_name = base;
        request.git_url = resolved.git_url();
        request.authoritative_devel_update = forced;
        request.devel_tracking_bootstrap = entries.front()->bootstrap;
        drafts.push_back({*resolved.aur_identity(), std::move(request), std::move(targets)});
    }
    for(auto& draft : drafts)
        patches.consider(draft.source, std::move(draft.request), std::move(draft.targets), config);
    return true;
}

bool prepare_aur_upgrade_patch_dependencies(AurUpgradePatchSet& patches, const BuildPlan& plan, const AurUpdateBuildUnitSelection& selection, const AppConfig& config) {
    const auto projection = project_build_plan_required_artifact_targets(plan);
    if(!projection.is_success()) return false; // existing preflight owns the blocker
    bool changed = false;
    if(selection.entries.size() != projection.success()->build_units.size())
        throw std::logic_error(localization::format_translated_message("{} patch discovery lost build-unit selection correlation.", "AUR"));
    for(const auto& unit : projection.success()->build_units) {
        const auto& selected = selection.entries.at(unit.build_plan_order_index);
        std::vector<std::string> children;
        for(const auto& target : unit.required_targets)
            children.push_back(target.package_name);
        if(selected.build_plan_order_index != unit.build_plan_order_index || selected.package_base != unit.package_base || selected.package_names != children)
            throw std::logic_error(localization::format_translated_message("{} patch discovery build-unit identity mismatch.", "AUR"));
        if(selected.status == AurUpdateBuildUnitSelectionStatus::ExternallySatisfiedByExplicitSourcePackageBase) continue;
        if(selected.status != AurUpdateBuildUnitSelectionStatus::SelectedForAurExecution)
            throw std::logic_error(localization::format_translated_message("{} patch discovery received an unknown build-unit selection.", "AUR"));
        if(unit.required_targets.empty()) continue;
        const auto source = resolve_source_build_identity(unit.required_targets.front().package_name);
        if(!source.aur_identity() || source.package_base() != unit.package_base)
            throw std::runtime_error(localization::format_translated_message("Current {} source identity changed before patch discovery.", "AUR"));
        SourceBuildRequest request;
        request.package_name = unit.required_targets.size() == 1 ? unit.required_targets.front().package_name : "";
        request.checkout_name = source.package_base();
        request.git_url = source.git_url();
        changed = patches.consider(*source.aur_identity(), std::move(request), unit.required_targets, config) || changed;
    }
    return changed;
}
