#include "aur_rpc.hpp"
#include "local_package_metadata.hpp"
#include "process.hpp"
#include "srcinfo_source_metadata.hpp"
#include "trusted_git.hpp"
#include "trusted_git_process_policy.hpp"
#include <curl/curl.h>
#include <fcntl.h>
#include <chrono>
#include <limits>
#include <utility>
#include "aur_devel_update.hpp"
#include "app_config.hpp"
#include "devel_tracking_bootstrap.hpp"
#include <unistd.h>
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

AurDevelUpdateContextObservation observe_aur_devel_update_context() {
    return observe_context();
}

void observe_aur_devel_bootstrap_candidates(AurUpdateQueryResult& query, const AppConfig& config) {
    if(config.no_confirm || config.user_config.review.diff != ReviewPolicy::Prompt || isatty(STDIN_FILENO) != 1) return;
    for(const auto& observation : query.devel_observations) {
        if(observation.plan_index >= query.plan.entries.size() || !observation.evidence || !observation.context) continue;
        auto& entry = query.plan.entries[observation.plan_index];
        const auto& evidence = *observation.evidence;
        if(entry.bootstrap || !is_initial_devel_bootstrap_observation(entry, evidence)) continue;
        if(std::count_if(query.devel_observations.begin(), query.devel_observations.end(), [&](const auto& item) {
               return item.plan_index == observation.plan_index;
           }) != 1) continue;
        const auto& base_name = entry.aur_package->package_base;
        if(std::count_if(query.plan.entries.begin(), query.plan.entries.end(), [&](const auto& other) {
               return other.aur_package && other.aur_package->package_base == base_name;
           }) != 1) continue; // No trial from a partial/shared RPC PackageBase view.

        const auto base = PackageBaseIdentity::make(PackageSourceIdentity::aur(
                                                        SourceLocationIdentity::known_git_remote("https://aur.archlinux.org/" + base_name + ".git")),
                                                    base_name);
        const auto trial = observe_devel_tracking_bootstrap(PackageChildIdentity::make(base, entry.installed_name));
        if(const auto* available = std::get_if<std::shared_ptr<const DevelTrackingBootstrapTrial>>(&trial)) {
            if((*available)->installed().version().full_version() &&
               *(*available)->installed().version().full_version() == entry.installed_version) entry.bootstrap = *available;
        }
    }
}


namespace {
using Reason = DevelTrackingBootstrapUnavailableReason;
constexpr std::size_t MAX_RECIPE_METADATA = 256 * 1024;

struct Descriptor {
    int value;
    ~Descriptor() {
        if(value >= 0) static_cast<void>(::close(value));
    }
};

struct RecipeObservation {
    SourceRevisionIdentity revision;
    std::string srcinfo;
};

#ifdef MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
DevelTrackingBootstrapTestHooks g_bootstrap_hooks;
#endif

bool clean_checkout(const PackageChildIdentity& package) {
#ifdef MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
    if(g_bootstrap_hooks.checkout) return g_bootstrap_hooks.checkout(package);
#endif
    const auto paths = xdg_paths::resolve_cache_process_environment();
    auto directory = xdg_directory_safety::open_existing_directory(paths);
    if(!directory) return true;
    const auto root = adopt_trusted_cache_root(paths, std::move(*directory));
    const auto checkout = require_trusted_cache_path(root, paths.directory / package.package_base().package_base(),
                                                     CachePathRequirement::ExistingOrMissing);
    if(!checkout.exists()) return true;
    return trusted_git_checkout_has_no_overlay(checkout, *package.package_base().source().location().value());
}

std::size_t receive_metadata(void* bytes, std::size_t size, std::size_t count, void* context) noexcept {
    auto& out = *static_cast<std::string*>(context);
    if(size != 0 && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    const auto length = size * count;
    if(length > MAX_RECIPE_METADATA - out.size()) return 0;
    try {
        out.append(static_cast<char*>(bytes), length);
    } catch(...) {
        return 0;
    }
    return length;
}

std::optional<RecipeObservation> observe_recipe(const PackageChildIdentity& package) {
#ifdef MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
    if(g_bootstrap_hooks.recipe) {
        const auto result = g_bootstrap_hooks.recipe(package);
        if(!result) return std::nullopt;
        return RecipeObservation{result->revision, result->srcinfo};
    }
#endif
    Descriptor directory{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    Descriptor input{::open("/dev/null", O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
    if(directory.value < 0 || input.value < 0) return std::nullopt;
    auto arguments = trusted_git_observer_process_arguments();
    arguments.insert(arguments.end(), {"ls-remote", "--exit-code", "--", *package.package_base().source().location().value(), "HEAD"});
    ExplicitProcessInvocation invocation{"/usr/bin/git", std::move(arguments),
                                         trusted_git_process_environment(TrustedGitProcessEnvironmentMode::ReadOnlyObservation)};
    invocation.working_directory_fd = directory.value;
    invocation.standard_input_fd = input.value;
    const auto observed = capture_bounded_explicit_process_output_raw(invocation,
                                                                      {std::chrono::seconds(30), std::chrono::seconds(1), 256, true});
    const auto* exited = std::get_if<BoundedProcessExited>(&observed.outcome);
    if(!exited || exited->exit_code != 0) return std::nullopt;
    const auto& bytes = observed.output;
    const auto tab = bytes.find('\t');
    if(tab == std::string::npos || bytes.substr(tab) != "\tHEAD\n") return std::nullopt;
    const auto revision = SourceRevisionIdentity::git_commit(bytes.substr(0, tab));

    // cgit's immutable id selects the same recipe commit advertised by Git.
    // The bytes remain untrusted trial metadata, never reviewed/source proof.
    static CurlGlobal curl_global;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if(!curl) return std::nullopt;
    std::unique_ptr<char, decltype(&curl_free)> escaped(
        curl_easy_escape(curl.get(), package.package_base().package_base().c_str(), 0), curl_free);
    if(!escaped) return std::nullopt;
    const std::string url = "https://aur.archlinux.org/cgit/aur.git/plain/.SRCINFO?h=" +
                            std::string(escaped.get()) + "&id=" + *revision.git_commit();
    std::string metadata;
    if(curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()) != CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https") != CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L) != CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L) != CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_metadata) != CURLE_OK ||
       curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &metadata) != CURLE_OK ||
       curl_easy_perform(curl.get()) != CURLE_OK) return std::nullopt;
    long status = 0;
    if(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status != 200) return std::nullopt;
    return RecipeObservation{revision, std::move(metadata)};
}

bool complete_installed_group(const PackageChildIdentity& package) {
    const auto context = observe_aur_devel_update_context();
    if(!context.installed_inventory) return false;
    const auto* inventory = std::get_if<InstalledPackageStateSnapshot>(&*context.installed_inventory);
    if(!inventory) return false;
    const auto selected = inventory->find(package.package_name());
    if(selected == inventory->end() || !selected->second.package_base.value() ||
       *selected->second.package_base.value() != package.package_base().package_base() ||
       selected->second.reason == InstalledPackageReason::Unknown) return false;
    std::size_t children = 0;
    for(const auto& [name, installed] : *inventory) {
        static_cast<void>(name);
        if(!installed.package_base.value()) return false;
        if(*installed.package_base.value() == package.package_base().package_base()) ++children;
    }
    return children == 1;
}

bool valid_reviewed(const ReviewedSourceStateStoreRead& read) {
    return (std::holds_alternative<ReviewedSourceStateMissing>(read.observation) && !read.observed) ||
           (std::holds_alternative<ReviewedSourceStateLoaded>(read.observation) && read.observed);
}
} // namespace

DevelTrackingBootstrapTrial::DevelTrackingBootstrapTrial(
    PackageChildIdentity package, SourceRevisionIdentity revision, std::string metadata,
    InstalledArtifactBinding installed, ReviewedSourceStateStoreRead reviewed)
    : package_(std::move(package)), recipe_revision_(std::move(revision)), source_metadata_(std::move(metadata)),
      installed_(std::move(installed)), installed_version_(*installed_.version().full_version()), reviewed_(std::move(reviewed)) {
}

bool has_supported_devel_bootstrap_source(const PackageChildIdentity& package, const std::string& srcinfo) {
    const auto sources = parse_srcinfo_source_metadata(srcinfo);
    const auto metadata = parse_local_package_metadata(srcinfo);
    if(!sources.is_success() || !metadata.is_success() ||
       sources.metadata()->package_base != package.package_base().package_base() ||
       metadata.metadata()->package_base != package.package_base().package_base() ||
       metadata.metadata()->children.size() != 1 || metadata.metadata()->children.front().name != package.package_name()) return false;
    // Local supplemental files cannot be proven regular/tracked by RPC or a
    // plain metadata response. Leave those unverified recipes unoffered.
    if(sources.metadata()->source_entries.size() != 1) return false;
    const auto& entry = sources.metadata()->source_entries.front();
    const auto& source = entry.parsed_source;
    if(entry.architecture_qualifier || source.kind != ParsedSourceEntryKind::Vcs || !source.vcs ||
       source.vcs->recognized_kind != ParsedSourceVcsKind::Git ||
       source.vcs->declaration_kind != ParsedSourceVcsDeclarationKind::ExplicitPrefix ||
       source.transport_scheme != std::optional<std::string>("https") || source.vcs->query) return false;
    if(source.vcs->selector) {
        const auto& selector = *source.vcs->selector;
        if(source.vcs->component_order != ParsedSourceVcsComponentOrder::FragmentOnly ||
           selector.recognized_role != ParsedSourceSelectorRole::Branch || selector.key != "branch" ||
           !std::holds_alternative<ValidatedExactGitBranch>(validate_exact_git_branch(selector.value))) return false;
    } else if(source.vcs->component_order != ParsedSourceVcsComponentOrder::None)
        return false;
    try {
        static_cast<void>(ValidatedHttpsGitRemote::make(source.source_location));
    } catch(const std::invalid_argument&) {
        return false;
    }
    return true;
}

DevelTrackingBootstrapObservation observe_devel_tracking_bootstrap(const PackageChildIdentity& package) {
    try {
        const auto& base = package.package_base();
        if(!is_valid_package_name(package.package_name()) || !is_valid_package_name(base.package_base()) ||
           base.source() != PackageSourceIdentity::aur(SourceLocationIdentity::known_git_remote(
                                "https://aur.archlinux.org/" + base.package_base() + ".git"))) return DevelTrackingBootstrapUnavailable{Reason::RecipeUnavailable};
        if(!complete_installed_group(package)) return DevelTrackingBootstrapUnavailable{Reason::InstalledStateUnavailable};
        const auto local = observe_devel_bootstrap_local_state(package);
        if(!local.provenance || !std::holds_alternative<DevelBuildProvenanceStoreMissing>(*local.provenance))
            return DevelTrackingBootstrapUnavailable{Reason::ProvenanceNotMissing};
        const auto* installed = local.installed ? std::get_if<CurrentInstalledArtifactBindingObserved>(&*local.installed) : nullptr;
        if(!installed) return DevelTrackingBootstrapUnavailable{Reason::InstalledStateUnavailable};
        const auto* reviewed = local.reviewed ? std::get_if<ReviewedSourceStateStoreRead>(&*local.reviewed) : nullptr;
        if(!reviewed || !valid_reviewed(*reviewed)) return DevelTrackingBootstrapUnavailable{Reason::ReviewedStateInvalid};
        if(!clean_checkout(package)) return DevelTrackingBootstrapUnavailable{Reason::CheckoutOverlayOrUnavailable};
        const auto recipe = observe_recipe(package);
        if(!recipe) return DevelTrackingBootstrapUnavailable{Reason::RecipeUnavailable};
        if(!has_supported_devel_bootstrap_source(package, recipe->srcinfo)) return DevelTrackingBootstrapUnavailable{Reason::UnsupportedSource};
        // Network observation is outside all local reader lifetimes. Recheck
        // the same local facts once before returning a trial to the prompt.
        const auto after = observe_devel_bootstrap_local_state(package);
        const auto* after_installed = after.installed ? std::get_if<CurrentInstalledArtifactBindingObserved>(&*after.installed) : nullptr;
        const auto* after_reviewed = after.reviewed ? std::get_if<ReviewedSourceStateStoreRead>(&*after.reviewed) : nullptr;
        if(!after.provenance || !std::holds_alternative<DevelBuildProvenanceStoreMissing>(*after.provenance) ||
           !after_installed || after_installed->binding() != installed->binding() ||
           !after_reviewed || *after_reviewed != *reviewed || !complete_installed_group(package)) {
            return DevelTrackingBootstrapUnavailable{Reason::ObservationChanged};
        }
        return std::shared_ptr<const DevelTrackingBootstrapTrial>(new DevelTrackingBootstrapTrial(
            package, recipe->revision, recipe->srcinfo, installed->binding(), *reviewed));
    } catch(const std::bad_alloc&) {
        throw;
    } catch(const std::exception&) {
        return DevelTrackingBootstrapUnavailable{Reason::ObservationChanged};
    }
}

bool revalidate_devel_tracking_bootstrap(const DevelTrackingBootstrapTrial& trial) {
    const auto fresh = observe_devel_tracking_bootstrap(trial.package());
    const auto* observed = std::get_if<std::shared_ptr<const DevelTrackingBootstrapTrial>>(&fresh);
    return observed && (*observed)->recipe_revision() == trial.recipe_revision() &&
           (*observed)->source_metadata() == trial.source_metadata() &&
           (*observed)->installed() == trial.installed() && (*observed)->reviewed() == trial.reviewed();
}

#ifdef MOGUET_ENABLE_DEVEL_TRACKING_BOOTSTRAP_TEST_HOOKS
void set_devel_tracking_bootstrap_test_hooks(DevelTrackingBootstrapTestHooks hooks) {
    g_bootstrap_hooks = std::move(hooks);
}
#endif

bool is_initial_devel_bootstrap_observation(const AurUpdatePlanEntry& entry, const DevelPackageAssessment& evidence) noexcept {
    return entry.devel_assessment_origin == AurDevelAssessmentOrigin::CurrentObservation &&
           entry.devel_assessment == evidence.assessment && evidence.stage == DevelPackageAssessmentStage::Provenance &&
           !evidence.issue && evidence.before.provenance &&
           std::holds_alternative<DevelBuildProvenanceStoreMissing>(*evidence.before.provenance) &&
           !evidence.before.installed && !evidence.before.reviewed && !evidence.after.provenance &&
           !evidence.after.installed && !evidence.after.reviewed && !evidence.remote &&
           evidence.post_check == DevelPackagePostCheck::NotAttempted &&
           project_aur_update_effective_state(entry) == AurUpdateEffectiveState::RequiresCheck &&
           *entry.devel_assessment.requires_check_reason() == DevelRequiresCheckReason::ProvenanceMissing &&
           entry.aur_package && entry.installed_name == entry.aur_package->aur_name;
}
