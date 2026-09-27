#include "artifact_install_executor.hpp"
#include "cache_authority.hpp"
#include "system_source_upgrade.hpp"
#include "review_recipe_patch_save.hpp"
#include "stubs/system-source-upgrade/phase_stub.hpp"
#include "unified_plan_projection.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

using SystemSourceUpgradeExecutor = SystemSourceUpgradeResult (*)(
    PreparedSystemSourceUpgrade,
    const AppConfig&,
    const SystemSourceUpgradeEventObserver&,
    std::optional<ValidatedCacheRoot>);

static_assert(!std::is_copy_constructible_v<PreparedSystemSourceUpgrade>);
static_assert(std::is_move_constructible_v<PreparedSystemSourceUpgrade>);
static_assert(!std::is_copy_constructible_v<
              SystemSourceUpgradeProjectionAuthority>);
static_assert(!std::is_copy_constructible_v<
              PreparedSystemSourceWorkReference>);
static_assert(
    std::is_same_v<
        decltype(&execute_prepared_system_source_upgrade),
        SystemSourceUpgradeExecutor>);
static_assert(
    std::is_invocable_v<
        SystemSourceUpgradeExecutor,
        PreparedSystemSourceUpgrade&&,
        const AppConfig&,
        const SystemSourceUpgradeEventObserver&,
        std::optional<ValidatedCacheRoot>>);
static_assert(
    !std::is_invocable_v<
        SystemSourceUpgradeExecutor,
        PreparedSystemSourceUpgrade&,
        const AppConfig&,
        const SystemSourceUpgradeEventObserver&,
        std::optional<ValidatedCacheRoot>>);

namespace {

namespace fs = std::filesystem;
namespace stub = system_source_upgrade_test_stub;

class ScopedEnvironmentVariable final {
    std::string name_;
    std::optional<std::string> original_value_;

public:
    ScopedEnvironmentVariable(std::string name, std::string value)
        : name_(std::move(name)) {
        const char* original = std::getenv(name_.c_str());
        if(original != nullptr) original_value_ = original;
        if(setenv(name_.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error(
                "Failed to set scoped test environment variable.");
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() noexcept {
        if(original_value_.has_value()) {
            static_cast<void>(setenv(
                name_.c_str(), original_value_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv(name_.c_str()));
        }
    }
};

class TemporaryCacheEnvironment final {
    fs::path root_;

public:
    TemporaryCacheEnvironment() {
        std::string path_template =
            "/tmp/moguet-system-source-cache-test-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        if(created == nullptr) {
            throw std::runtime_error(
                "Failed to create system/source cache test directory.");
        }
        root_ = created;
        if(setenv("XDG_CACHE_HOME", root_.c_str(), 1) != 0) {
            throw std::runtime_error(
                "Failed to set system/source cache test environment.");
        }
    }

    TemporaryCacheEnvironment(const TemporaryCacheEnvironment&) = delete;
    TemporaryCacheEnvironment& operator=(
        const TemporaryCacheEnvironment&) = delete;

    ~TemporaryCacheEnvironment() noexcept {
        std::error_code cleanup_error;
        fs::remove_all(root_, cleanup_error);
    }
};

constexpr const char* SYSTEM_COMMAND =
    "sudo pacman '-Syu' '--noconfirm'";

void expect(bool condition, const std::string& diagnostic) {
    if(!condition) throw std::runtime_error(diagnostic);
}

AppConfig full_option_config() {
    AppConfig config;
    config.user_config.review.pkgbuild = ReviewPolicy::Skip;
    config.user_config.review.diff = ReviewPolicy::Skip;
    config.user_config.build.mode = BuildMode::Clean;
    config.no_confirm = true;
    config.editor = "test-editor";
    return config;
}

stub::ConfigSnapshot snapshot_config(const AppConfig& config) {
    return stub::ConfigSnapshot{
        config.user_config.review.pkgbuild == ReviewPolicy::Skip,
        config.user_config.review.diff == ReviewPolicy::Skip,
        config.no_confirm,
        config.user_config.build.mode == BuildMode::Rebuild,
        config.user_config.build.mode == BuildMode::Clean,
        config.rm_deps,
        config.editor};
}

SourcePreferenceDirectorySnapshot preference_directory(
    const std::vector<std::string>& package_names,
    bool root_exists = true) {
    SourcePreferenceDirectorySnapshot snapshot;
    snapshot.root_exists = root_exists;
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        snapshot.entries.push_back(SourcePreferenceEntrySnapshot{
            index,
            fs::path("/preferences") / package_names[index],
            package_names[index],
            true});
    }
    return snapshot;
}

SourcePreferenceLoaded loaded_preference(
    const std::string& package_name,
    std::vector<std::string> warnings = {}) {
    SourceBuildEnvironment environment;
    environment.ordered_assignments.push_back(
        SourceEnvironmentAssignment{
            "PACKAGE_NAME", package_name});
    return SourcePreferenceLoaded{
        .entry_path = fs::path("/preferences") / package_name,
        .environment = std::move(environment),
        .warnings = std::move(warnings),
        .raw_contents = {},
        .identity = std::nullopt,
    };
}

InstalledPackageMetadata installed_package(
    const std::string& package_name,
    const std::string& version = "1.0-1") {
    return InstalledPackageMetadata{
        package_name,
        version,
        InstalledPackageReason::Explicit};
}

stub::MetadataSessionScript metadata_session(
    const std::vector<std::string>& package_names,
    LocalPackageVersionSnapshot local_snapshot,
    const std::string& installed_version = "1.0-1") {
    stub::MetadataSessionScript script;
    script.local_package_snapshot = std::move(local_snapshot);
    for(const auto& package_name : package_names) {
        script.installed_package_results.emplace(
            package_name,
            installed_package(package_name, installed_version));
    }
    return script;
}

SourceBuildExecutionResult source_execution(
    SourceBuildExecutionStatus status,
    std::string diagnostic = {},
    std::optional<ProductionSourceBuildProvenance>
        source_provenance = std::nullopt) {
    SourceBuildExecutionResult result;
    result.status = status;
    result.diagnostic = std::move(diagnostic);
    if(source_provenance.has_value()) {
        result.production_outcome =
            ProductionSourceBuildStagedOutcome{
                .source_provenance =
                    std::move(*source_provenance),
                .build_outcome =
                    status == SourceBuildExecutionStatus::Installed
                        ? ProductionSourceBuildCommandOutcome::
                              Succeeded
                        : ProductionSourceBuildCommandOutcome::
                              NotAttempted,
                .install_outcome =
                    status == SourceBuildExecutionStatus::Installed
                        ? ProductionSourceInstallOutcome::Succeeded
                        : ProductionSourceInstallOutcome::
                              NotAttempted};
    }
    if(status == SourceBuildExecutionStatus::UpdateStatusUnknownSkipped) {
        result.update_status_unknown_skip_reason =
            SourceBuildUpdateStatusUnknownSkipReason::NoConfirm;
    }
    return result;
}

ProductionSourceBuildProvenance compatibility_review_provenance() {
    ProductionSourceBuildProvenance provenance;
    provenance.review_status =
        ProductionSourceReviewStatus::CompatibilityWithoutReview;
    provenance.compatibility_reason =
        ReviewedSourceCompatibilityBuildReason::NoDiff;
    return provenance;
}

PackageBaseSourceBuildSelectedResult selected_child(
    const std::string& package_name,
    ArtifactInstallExecutionOutcome outcome =
        ArtifactInstallExecutionOutcome::Installed,
    const std::string& version = "2.0-1") {
    return PackageBaseSourceBuildSelectedResult{
        ArtifactPackageIdentity{package_name, version},
        DesiredInstallReason::Explicit,
        outcome};
}

ProvidedDependency selected_repository_provider() {
    return ProvidedDependency::from_repository(
        "extra", "registered-source-provider",
        "virtual-registered-api", "virtual-registered-api=2",
        "2.0-1");
}

ProvidedDependency alternate_repository_provider() {
    return ProvidedDependency::from_repository(
        "core", "alternate-registered-source-provider",
        "virtual-registered-api", "virtual-registered-api=2",
        "2.1-1");
}

ProvidedDependency selected_aur_provider() {
    return ProvidedDependency::from_aur(
        "aur-registered-source-provider",
        "aur-registered-source-provider-base",
        "virtual-registered-api", "virtual-registered-api=2",
        "2.2-1");
}

ResolvedSourceBuildIdentity registered_aur_identity(
    const std::string& package_name) {
    return ResolvedSourceBuildIdentity{
        ResolvedAurSourceBuildIdentity{package_name, package_name}};
}

ResolvedSourceBuildIdentity registered_repository_identity(
    const std::string& requested_child,
    const std::string& package_base) {
    return ResolvedSourceBuildIdentity{
        ResolvedRepositorySourceBuildIdentity{
            RepositoryPackagePresent{
                "extra", 0, requested_child, package_base,
                std::nullopt,
                std::vector<std::string>{"extra"}}}};
}

std::string progress_subject(const SystemSourceUpgradeEvent& event) {
    switch(event.kind) {
        case SystemSourceUpgradeEventKind::LoadingSourcePreference:
            return "load:" + event.preference_package_name.value_or("<missing>");
        case SystemSourceUpgradeEventKind::SourcePreferenceWarning:
            return "warning:" + event.diagnostic;
        case SystemSourceUpgradeEventKind::SystemUpgradeStarting:
            return "system-start";
        case SystemSourceUpgradeEventKind::CheckingSourcePackages:
            return "source-check";
        case SystemSourceUpgradeEventKind::InvalidPreferenceWarning:
            return "invalid:" +
                   event.preference_package_name.value_or("<missing>");
    }
    return "unknown";
}

const SystemSourceUpgradeEventObserver OBSERVER =
    [](const SystemSourceUpgradeEvent& event) {
        stub::record_progress(progress_subject(event));
    };

void configure_preferences(
    const std::vector<std::string>& package_names,
    bool add_first_warning = false) {
    stub::set_preference_directory(preference_directory(package_names));
    for(std::size_t index = 0; index < package_names.size(); ++index) {
        std::vector<std::string> warnings;
        if(add_first_warning && index == 0) {
            warnings.push_back("scripted preference warning");
        }
        stub::enqueue_preference_result(
            package_names[index],
            loaded_preference(package_names[index], std::move(warnings)));
    }
}

PreparedSystemSourceUpgrade prepare_sources(
    const std::vector<std::string>& package_names,
    const AppConfig& config,
    LocalPackageVersionSnapshot before_snapshot =
        LocalPackageVersionSnapshot{{"core", "1.0-1"}},
    bool add_first_warning = false) {
    configure_preferences(package_names, add_first_warning);
    stub::enqueue_metadata_session(metadata_session(
        package_names, std::move(before_snapshot)));
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(config, OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "Source preparation unexpectedly blocked");
    return std::move(
        std::get<PreparedSystemSourceUpgrade>(preparation));
}

void enqueue_post_metadata(
    const std::vector<std::string>& package_names,
    LocalPackageVersionSnapshot after_snapshot =
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}) {
    stub::enqueue_metadata_session(metadata_session(
        package_names, std::move(after_snapshot)));
}

template <typename ConfigureScript>
SystemSourceUpgradeResult execute_registered_repository_case(
    const std::string& package_name,
    const std::string& package_base,
    ConfigureScript&& configure_script) {
    stub::reset();
    stub::set_source_identity(
        package_name,
        registered_repository_identity(package_name, package_base));
    const AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared =
        prepare_sources({package_name}, config);
    enqueue_post_metadata({package_name});
    std::forward<ConfigureScript>(configure_script)();
    return execute_prepared_system_source_upgrade(
        std::move(prepared), config, OBSERVER);
}

SystemSourceUpgradeResult take_blocked(
    SystemSourceUpgradePreparation preparation) {
    expect(
        std::holds_alternative<SystemSourceUpgradeResult>(preparation),
        "Preparation unexpectedly returned an executable capability");
    return std::move(std::get<SystemSourceUpgradeResult>(preparation));
}

std::size_t event_position(
    stub::EventKind kind,
    const std::string& subject,
    std::size_t occurrence = 0) {
    std::size_t seen = 0;
    const auto& history = stub::events();
    for(std::size_t index = 0; index < history.size(); ++index) {
        if(history[index].kind != kind || history[index].subject != subject) {
            continue;
        }
        if(seen == occurrence) return index;
        ++seen;
    }
    throw std::runtime_error(
        "Missing event: " + subject + " occurrence " +
        std::to_string(occurrence));
}

void expect_source_order(
    const SystemSourceUpgradeResult& result,
    const std::vector<std::string>& expected_names) {
    expect(
        result.registered_source_results.size() == expected_names.size(),
        "Registered source result count differs from preference count");
    for(std::size_t index = 0; index < expected_names.size(); ++index) {
        const RegisteredSourceUpgradeResult& source =
            result.registered_source_results[index];
        expect(
            source.preference_package_name == expected_names[index],
            "Registered source result order changed");
        expect(
            source.original_preference_index == index,
            "Original preference index changed");
    }
}

void test_empty_registered_snapshot() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    AppConfig config = full_option_config();
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(config, OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "Empty preference snapshot was not executable");
    PreparedSystemSourceUpgrade prepared = std::move(
        std::get<PreparedSystemSourceUpgrade>(preparation));
    const SystemSourceUpgradePreparedSnapshot* snapshot = prepared.snapshot();
    expect(snapshot != nullptr, "Prepared snapshot is unavailable");
    expect(snapshot->preference_root_exists, "Existing empty root was lost");
    expect(snapshot->registered_sources.empty(), "Empty root gained sources");

    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "2.0-1"}}));
    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(result.is_success(), "System-only result is not successful");
    expect(
        result.system.status == SystemUpgradePhaseStatus::Completed,
        "System-only phase did not complete");
    expect(
        result.system.package_state_change == PackageStateChange::Changed,
        "System-only phase did not compare authoritative snapshots");
    expect(result.registered_source_results.empty(), "System-only result gained sources");
    expect(stub::system_commands() == std::vector<std::string>{SYSTEM_COMMAND},
           "System command changed");
    expect(stub::source_execution_calls().empty(), "System-only phase executed source");
    stub::require_script_consumed();
}

void test_system_only_observation_failure_is_non_blocking() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::MetadataSessionScript failed_snapshot;
    failed_snapshot.local_package_snapshot = PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "scripted system-only snapshot failure"};
    stub::enqueue_metadata_session(std::move(failed_snapshot));

    AppConfig config = full_option_config();
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(config, OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "System-only observation failure blocked execution");

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(
                std::get<PreparedSystemSourceUpgrade>(preparation)),
            config, OBSERVER);
    expect(result.is_success(),
           "System-only observation failure became operation failure");
    expect(
        result.system.status == SystemUpgradePhaseStatus::Completed &&
            result.system.package_state_change ==
                PackageStateChange::Unknown,
        "System-only observation failure did not remain unverified");
    expect(
        result.system.before_snapshot_failure.has_value() &&
            result.issues.size() == 1 &&
            result.issues.front().impact ==
                SystemSourceUpgradeIssueImpact::ObservabilityOnly,
        "System-only observation failure lost typed evidence");
    expect(stub::system_commands() == std::vector<std::string>{SYSTEM_COMMAND},
           "System-only observation failure skipped system mutation");
    stub::require_script_consumed();
}

void test_preparation_retains_aur_plan_and_source_kind() {
    stub::reset();
    const std::string package_name = "retained-aur-source";
    stub::set_source_identity(
        package_name, registered_aur_identity(package_name));

    PreparedSystemSourceUpgrade prepared = prepare_sources(
        {package_name}, full_option_config());
    const SystemSourceUpgradePreparedSnapshot* snapshot =
        prepared.snapshot();
    const BuildPlan* plan = prepared.aur_invocation_plan();
    const SystemSourceUpgradeProjectionAuthority* projection_authority =
        prepared.projection_authority();
    expect(snapshot != nullptr, "Prepared AUR snapshot is unavailable");
    expect(
        snapshot->registered_sources.size() == 1 &&
            snapshot->registered_sources.front().source_kind ==
                SourceBuildSourceKind::Aur,
        "Prepared AUR source kind was not retained as typed identity");
    expect(
        plan != nullptr && plan->root_targets ==
                               std::vector<RootTargetIdentity>{
                                   RootTargetIdentity{0, package_name}},
        "Invocation-wide AUR BuildPlan was discarded or rebuilt");
    expect(
        prepared.issues().empty(),
        "Successful preparation gained a route issue");
    expect(
        projection_authority != nullptr &&
            &projection_authority->snapshot() == snapshot &&
            projection_authority->aur_invocation_plan() == plan &&
            projection_authority->source_work_items().size() == 1 &&
            &projection_authority->source_work_items()
                    .front()
                    .source() ==
                &snapshot->registered_sources.front(),
        "Prepared AUR projection seam lost owner correlation");
    const auto& targets = projection_authority->source_work_items()
                              .front()
                              .required_targets();
    const PreparedSystemSourceWorkReference& work =
        projection_authority->source_work_items().front();
    expect(
        targets.size() == 1 &&
            targets.front().package_base == package_name &&
            targets.front().package_name == package_name &&
            targets.front().desired_reason ==
                DesiredInstallReason::Explicit &&
            work.required_target_provenance() ==
                RequiredTargetProvenance::AurBuildPlanProjection &&
            work.artifact_lifecycle_intent() ==
                ArtifactLifecycleIntent::SingularCompatibility &&
            work.only_if_updated() && !work.needed() &&
            !work.uses_system_update_baseline() &&
            !work.repository_identity().has_value(),
        "Prepared AUR projection seam did not retain actual work targets");
    stub::require_script_consumed();
}

void test_preparation_exposes_repository_work_targets() {
    stub::reset();
    const std::string package_name = "retained-repository-child";
    const std::string package_base = "retained-repository-base";
    stub::set_source_identity(
        package_name,
        registered_repository_identity(package_name, package_base));
    PreparedSystemSourceUpgrade prepared = prepare_sources(
        {package_name}, full_option_config());
    const SystemSourceUpgradePreparedSnapshot* snapshot = prepared.snapshot();
    const SystemSourceUpgradeProjectionAuthority* projection_authority =
        prepared.projection_authority();
    expect(
        snapshot != nullptr && projection_authority != nullptr &&
            projection_authority->aur_invocation_plan() == nullptr &&
            projection_authority->source_work_items().size() == 1 &&
            &projection_authority->source_work_items()
                    .front()
                    .source() ==
                &snapshot->registered_sources.front() &&
            snapshot->registered_sources.front().source_kind ==
                SourceBuildSourceKind::Repository &&
            snapshot->registered_sources.front()
                    .resolved_package_base ==
                std::optional<std::string>(package_base) &&
            snapshot->registered_sources.front()
                    .required_target_provenance ==
                RequiredTargetProvenance::
                    RepositoryExactPackageProjection &&
            snapshot->registered_sources.front()
                    .artifact_lifecycle_intent ==
                ArtifactLifecycleIntent::PackageBaseSet,
        "Prepared repository projection seam lost source authority");
    const PreparedSystemSourceWorkReference& work =
        projection_authority->source_work_items().front();
    const auto& targets = work.required_targets();
    expect(
        targets.size() == 1 &&
            targets.front().package_base == package_base &&
            targets.front().package_name == package_name &&
            targets.front().desired_reason ==
                DesiredInstallReason::Explicit &&
            work.requested_package_name() == package_name &&
            work.checkout_package_base() == package_base &&
            work.required_target_provenance() ==
                RequiredTargetProvenance::
                    RepositoryExactPackageProjection &&
            work.artifact_lifecycle_intent() ==
                ArtifactLifecycleIntent::PackageBaseSet &&
            work.repository_identity().has_value() &&
            work.repository_identity()->requested_child() ==
                package_name &&
            work.repository_identity()->package_base() ==
                package_base &&
            work.uses_system_update_baseline() &&
            !work.needed() &&
            !work.only_if_updated() &&
            work.source().environment.has_value() &&
            work.source().environment->ordered_assignments.size() == 1 &&
            work.source().environment->ordered_assignments.front().value == package_name,
        "Prepared repository projection seam did not retain actual work targets");
    stub::require_script_consumed();
}

void test_all_sources_success_order_options_and_change() {
    stub::reset();
    const std::vector<std::string> packages = {"beta", "alpha", "gamma"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(
        packages, config,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}},
        true);
    const SystemSourceUpgradePreparedSnapshot* prepared_snapshot =
        prepared.snapshot();
    expect(prepared_snapshot != nullptr,
           "Prepared source snapshot is unavailable before execution");
    expect(prepared_snapshot->options.no_edit ==
                   (config.user_config.review.pkgbuild ==
                    ReviewPolicy::Skip) &&
               prepared_snapshot->options.no_diff ==
                   (config.user_config.review.diff ==
                    ReviewPolicy::Skip) &&
               prepared_snapshot->options.no_confirm == config.no_confirm &&
               prepared_snapshot->options.rebuild ==
                   (config.user_config.build.mode ==
                    BuildMode::Rebuild) &&
               prepared_snapshot->options.clean_build ==
                   (config.user_config.build.mode == BuildMode::Clean) &&
               prepared_snapshot->options.rm_deps == config.rm_deps &&
               prepared_snapshot->options.editor == config.editor,
           "Options were not frozen during preparation");
    expect(prepared_snapshot->registered_sources.size() == packages.size(),
           "Prepared source intent count changed before execution");
    for(std::size_t index = 0; index < packages.size(); ++index) {
        const RegisteredSourcePreferenceSnapshot& source =
            prepared_snapshot->registered_sources[index];
        expect(source.preference_package_name == packages[index] &&
                   source.original_preference_index == index,
               "Preference order was not frozen during preparation");
        expect(source.environment.has_value() &&
                   source.environment->ordered_assignments.front().value ==
                       packages[index],
               "Strict preference environment was not owned during preparation");
        expect(source.canonical_source_identity_key ==
                       std::optional<std::string>(
                           "repository:" + packages[index]) &&
                   source.resolved_package_base ==
                       std::optional<std::string>(packages[index]),
               "Canonical source identity was not frozen during preparation");
    }
    expect(prepared_snapshot->registered_sources.front().preference_load_warnings ==
               std::vector<std::string>{"scripted preference warning"},
           "Preference warning was not retained in the prepared snapshot");
    enqueue_post_metadata(
        packages,
        LocalPackageVersionSnapshot{{"core", "2.0-1"}});
    for(std::size_t index = 0; index < packages.size(); ++index) {
        stub::enqueue_source_success(source_execution(
            SourceBuildExecutionStatus::Installed));
    }

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect_source_order(result, packages);
    expect(result.is_success(), "All-source success helper returned false");
    expect(
        result.system.package_state_change == PackageStateChange::Changed,
        "System name/version delta was not Changed");
    expect(
        result.package_state_change() == PackageStateChange::Changed,
        "Aggregate package-state change was not Changed");
    expect(result.definitely_changed_package_state(),
           "Strict changed helper returned false");
    expect(!result.has_partial_completion(),
           "Complete result reported partial completion");
    expect(result.warnings.size() == 1,
           "Preference warning was not retained exactly once");

    const auto& calls = stub::source_execution_calls();
    expect(calls.size() == packages.size(), "Source executor call count changed");
    for(std::size_t index = 0; index < packages.size(); ++index) {
        expect(calls[index].package_name == packages[index],
               "Source executor order changed");
        expect(calls[index].package_base == packages[index],
               "Resolved PackageBase was not forwarded");
        expect(calls[index].config == snapshot_config(config),
               "Source option propagation changed");
        expect(!calls[index].request.only_if_updated,
               "Registered repository work item retained singular update policy");
        expect(!calls[index].request.needed,
               "Upgrade source work item invented --needed");
        expect(
            calls[index].request.custom_environment.ordered_assignments.front().value == packages[index],
            "Strict preference environment was not forwarded");
        expect(
            result.registered_source_results[index].canonical_source_identity_key ==
                std::optional<std::string>(
                    "repository:" + packages[index]),
            "Canonical source identity was not retained");
        expect(
            result.registered_source_results[index].status ==
                RegisteredSourceUpgradeStatus::Updated,
            "Installed source did not map to Updated");
    }
    const auto& preparation_calls = stub::source_preparation_calls();
    const auto& package_base_calls =
        stub::package_base_source_execution_calls();
    expect(
        preparation_calls.size() == packages.size() &&
            package_base_calls.size() == packages.size(),
        "Registered repository route did not use prepare then PackageBase execution");
    for(std::size_t index = 0; index < packages.size(); ++index) {
        expect(
            preparation_calls[index].package_name == packages[index] &&
                preparation_calls[index].update_policy ==
                    SourceBuildUpdatePolicy::OnlyIfUpdated &&
                preparation_calls[index].request.update_baseline.has_value() &&
                preparation_calls[index].request.update_baseline->installed_version ==
                    std::optional<std::string>{"1.0-1"} &&
                preparation_calls[index].request.installed_snapshot.has_value() &&
                preparation_calls[index].request.installed_snapshot->installed_version ==
                    std::optional<std::string>{"1.0-1"} &&
                preparation_calls[index].request.custom_environment.ordered_assignments.front().value ==
                    packages[index] &&
                package_base_calls[index].package_name ==
                    packages[index],
            "Registered repository preparation/execution correlation changed");
    }
    expect(stub::supported_option_configs() ==
               std::vector<stub::ConfigSnapshot>{snapshot_config(config)},
           "Supported-option guard did not receive the exact config");
    expect(stub::invocation_configs() ==
               std::vector<stub::ConfigSnapshot>{snapshot_config(config)},
           "Invocation preparation did not receive the exact config");
    expect(stub::system_commands() == std::vector<std::string>{SYSTEM_COMMAND},
           "System phase received source-only options");

    expect(
        event_position(stub::EventKind::LocalPackageSnapshot,
                       "local-packages", 0) <
            event_position(stub::EventKind::Progress, "system-start"),
        "Pre-system package snapshot occurred after system mutation");
    expect(
        event_position(stub::EventKind::SystemCommand, SYSTEM_COMMAND) <
            event_position(stub::EventKind::Progress, "source-check"),
        "Source check started before system command");
    expect(
        event_position(stub::EventKind::InstalledPackageQuery, "gamma", 1) <
            event_position(stub::EventKind::SourceExecution, "beta"),
        "Source execution started before all post-system queries completed");
    expect(
        event_position(stub::EventKind::SourceExecution, "beta") <
                event_position(stub::EventKind::SourceExecution, "alpha") &&
            event_position(stub::EventKind::SourceExecution, "alpha") <
                event_position(stub::EventKind::SourceExecution, "gamma"),
        "External source mutation order changed");
    stub::require_script_consumed();
}

void test_source_options_propagate_independently() {
    struct OptionCase {
        const char* name;
        void (*enable)(AppConfig& config);
    };
    const std::vector<OptionCase> cases = {
        {"noedit", [](AppConfig& config) {
             config.user_config.review.pkgbuild = ReviewPolicy::Skip;
         }},
        {"nodiff", [](AppConfig& config) {
             config.user_config.review.diff = ReviewPolicy::Skip;
         }},
        {"noconfirm", [](AppConfig& config) {
             config.no_confirm = true;
         }},
        {"rebuild", [](AppConfig& config) {
             config.user_config.build.mode = BuildMode::Rebuild;
         }},
        {"cleanbuild", [](AppConfig& config) {
             config.user_config.build.mode = BuildMode::Clean;
         }},
    };

    for(const auto& option_case : cases) {
        stub::reset();
        AppConfig config;
        config.editor = "option-test-editor";
        option_case.enable(config);
        PreparedSystemSourceUpgrade prepared =
            prepare_sources({"source"}, config);
        enqueue_post_metadata({"source"});
        stub::enqueue_source_success(source_execution(
            SourceBuildExecutionStatus::UpToDate));

        SystemSourceUpgradeResult result =
            execute_prepared_system_source_upgrade(
                std::move(prepared), config, OBSERVER);
        expect(result.status == SystemSourceUpgradeStatus::Completed,
               std::string("Independent option case failed: ") +
                   option_case.name);
        expect(stub::source_execution_calls().size() == 1 &&
                   stub::source_execution_calls().front().config ==
                       snapshot_config(config),
               std::string("Source option propagation was coupled: ") +
                   option_case.name);
        const std::string expected_system_command = config.no_confirm
                                                        ? "sudo pacman '-Syu' '--noconfirm'"
                                                        : "sudo pacman '-Syu'";
        expect(stub::system_commands() ==
                   std::vector<std::string>{expected_system_command},
               std::string("Source-only option leaked to system command: ") +
                   option_case.name);
        stub::require_script_consumed();
    }
}

void test_nonregular_preference_blocks_before_mutation() {
    stub::reset();
    SourcePreferenceDirectorySnapshot directory;
    directory.root_exists = true;
    directory.entries = {
        SourcePreferenceEntrySnapshot{
            3, "/preferences/not-a-file", "not-a-file", false},
    };
    stub::set_preference_directory(std::move(directory));
    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(
        result.status == SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.failure_diagnostic().has_value() &&
            result.failure_diagnostic()->find("non-regular") !=
                std::string::npos,
        "Non-regular preference entry was not a hard preparation error");
    expect(stub::system_commands().empty(),
           "Non-regular preference entry reached system mutation");
    stub::require_script_consumed();
}

void test_source_no_change_and_package_state_no_change() {
    stub::reset();
    const std::vector<std::string> packages = {"stable"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::UpToDate,
        "stable is up to date"));

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(result.is_success(), "No-change phase is not successful");
    expect(
        result.registered_source_results.front().status ==
            RegisteredSourceUpgradeStatus::NoChange,
        "UpToDate did not map to NoChange");
    expect(
        result.package_state_change() == PackageStateChange::NoChange,
        "Equal system/source snapshots did not reduce to NoChange");
    expect(!result.definitely_changed_package_state(),
           "NoChange was reported as definitely changed");
    expect(
        stub::source_preparation_calls().size() == 1 &&
            stub::source_preparation_calls().front().update_policy ==
                SourceBuildUpdatePolicy::OnlyIfUpdated &&
            stub::package_base_source_execution_calls().empty(),
        "UpToDate did not terminate at the closed preparation outcome");
    stub::require_script_consumed();
}

void test_system_failure_keeps_all_sources_not_attempted() {
    stub::reset();
    const std::vector<std::string> packages = {"first", "second", "third"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    stub::set_system_command_exit_status(42);

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.status ==
            SystemSourceUpgradeStatus::StoppedOnSystemFailure,
        "System failure did not stop the phase");
    expect(
        result.system.status == SystemUpgradePhaseStatus::Failed,
        "System failure was not typed Failed");
    expect(result.system.command_exit_status == std::optional<int>(42),
           "System command exit status was lost");
    expect(result.stopped_phase == SystemSourceUpgradePhase::System,
           "System failure stopped phase was not retained");
    expect(result.failure_diagnostic() ==
               std::optional<std::string>("The update failed."),
           "System failure diagnostic was lost");
    expect(
        result.system.package_state_change == PackageStateChange::Unknown,
        "Failed pacman transaction guessed package state");
    expect_source_order(result, packages);
    expect(result.has_not_attempted_sources(),
           "System failure lost NotAttempted sources");
    expect(std::all_of(
               result.registered_source_results.begin(),
               result.registered_source_results.end(),
               [](const RegisteredSourceUpgradeResult& source) {
                   return source.status ==
                          RegisteredSourceUpgradeStatus::NotAttempted;
               }),
           "System failure changed a source status");
    expect(stub::source_execution_calls().empty(),
           "System failure reached source mutation");
    expect(
        std::none_of(
            stub::events().begin(), stub::events().end(),
            [](const stub::Event& event) {
                return event.kind == stub::EventKind::Progress &&
                       event.subject == "source-check";
            }),
        "System failure reached post-system source check");
    stub::require_script_consumed();
}

void test_repository_provider_transaction_runs_after_system_and_blocks_all_sources_on_failure() {
    const std::vector<std::string> packages = {"first", "second"};
    const AppConfig config = full_option_config();

    stub::reset();
    stub::set_selected_repository_provider(
        "first", selected_repository_provider());
    PreparedSystemSourceUpgrade successful = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::Installed));
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::UpToDate));
    const SystemSourceUpgradeResult success =
        execute_prepared_system_source_upgrade(
            std::move(successful), config, OBSERVER);
    expect(success.is_success() &&
               success.selected_repository_provider_transaction.status ==
                   SelectedRepositoryProviderTransactionStatus::
                       Succeeded &&
               success.selected_repository_provider_transaction.package_state_change ==
                   PackageStateChange::Unknown &&
               success.selected_repository_provider_transaction.selected_providers ==
                   std::vector<ProvidedDependency>{
                       selected_repository_provider()} &&
               success.package_state_change() ==
                   PackageStateChange::Changed,
           "Repository provider transaction success blocked registered sources");
    expect(
        event_position(
            stub::EventKind::InstalledPackageQuery,
            "second", 1) <
                event_position(
                    stub::EventKind::
                        RepositoryProviderTransaction,
                    "selected-repository-providers") &&
            event_position(stub::EventKind::SystemCommand, SYSTEM_COMMAND) <
                event_position(
                    stub::EventKind::
                        RepositoryProviderTransaction,
                    "selected-repository-providers") &&
            event_position(
                stub::EventKind::RepositoryProviderTransaction,
                "selected-repository-providers") <
                event_position(
                    stub::EventKind::SourcePreparation,
                    "first") &&
            event_position(
                stub::EventKind::SourcePreparation,
                "first") <
                event_position(
                    stub::EventKind::
                        PackageBaseSourceExecution,
                    "first"),
        "Post-system guards/provider transaction/preparation/Set execution order changed");
    stub::require_script_consumed();

    stub::reset();
    stub::set_selected_repository_provider(
        "first", selected_repository_provider());
    PreparedSystemSourceUpgrade failing = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::fail_repository_provider_transaction(
        "scripted repository provider transaction failure");
    const SystemSourceUpgradeResult failure =
        execute_prepared_system_source_upgrade(
            std::move(failing), config, OBSERVER);
    expect(
        failure.status ==
                SystemSourceUpgradeStatus::StoppedOnSourceFailure &&
            failure.system.status ==
                SystemUpgradePhaseStatus::Completed &&
            failure.stopped_phase ==
                SystemSourceUpgradePhase::RegisteredSource &&
            failure.selected_repository_provider_transaction.status ==
                SelectedRepositoryProviderTransactionStatus::Failed &&
            failure.selected_repository_provider_transaction.selected_providers ==
                std::vector<ProvidedDependency>{
                    selected_repository_provider()} &&
            failure.package_state_change() ==
                PackageStateChange::Unknown &&
            failure.failure_diagnostic() ==
                std::optional<std::string>{
                    "scripted repository provider transaction failure"},
        "Repository provider transaction failure lost phase result");
    expect(
        std::all_of(
            failure.registered_source_results.begin(),
            failure.registered_source_results.end(),
            [](const RegisteredSourceUpgradeResult& source) {
                return source.status ==
                       RegisteredSourceUpgradeStatus::NotAttempted;
            }) &&
            stub::source_execution_calls().empty(),
        "Repository provider transaction failure started registered-source work");
    expect(
        event_position(stub::EventKind::SystemCommand, SYSTEM_COMMAND) <
            event_position(
                stub::EventKind::RepositoryProviderTransaction,
                "selected-repository-providers"),
        "Repository provider transaction failure ran before the system phase");
    stub::require_script_consumed();

    stub::reset();
    stub::set_selected_repository_provider(
        "first", selected_repository_provider());
    PreparedSystemSourceUpgrade cache_replaced =
        prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::fail_repository_provider_cache_revalidation();
    const SystemSourceUpgradeResult cache_failure =
        execute_prepared_system_source_upgrade(
            std::move(cache_replaced), config, OBSERVER);
    const auto cache_issue = std::find_if(
        cache_failure.issues.begin(), cache_failure.issues.end(),
        [](const SystemSourceUpgradeIssue& issue) {
            return issue.kind ==
                       SystemSourceUpgradeIssueKind::
                           CacheAuthorityInvalid &&
                   issue.phase ==
                       SystemSourceUpgradePhase::RegisteredSource &&
                   issue.trusted_cache_failure.has_value();
        });
    expect(
        cache_failure.status ==
                SystemSourceUpgradeStatus::StoppedOnSourceFailure &&
            cache_failure.system.status ==
                SystemUpgradePhaseStatus::Completed &&
            cache_failure.stopped_phase ==
                SystemSourceUpgradePhase::RegisteredSource &&
            cache_failure.selected_repository_provider_transaction.status ==
                SelectedRepositoryProviderTransactionStatus::
                    BlockedBeforeExecution &&
            cache_failure.selected_repository_provider_transaction.selected_providers ==
                std::vector<ProvidedDependency>{
                    selected_repository_provider()} &&
            cache_failure.selected_repository_provider_transaction.package_state_change ==
                PackageStateChange::NoChange &&
            cache_issue != cache_failure.issues.end() &&
            cache_issue->trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement &&
            std::all_of(
                cache_failure.registered_source_results.begin(),
                cache_failure.registered_source_results.end(),
                [](const RegisteredSourceUpgradeResult& source) {
                    return source.status ==
                           RegisteredSourceUpgradeStatus::
                               NotAttempted;
                }) &&
            stub::source_execution_calls().empty() &&
            std::none_of(
                stub::events().begin(), stub::events().end(),
                [](const stub::Event& event) {
                    return event.kind == stub::EventKind::
                                             RepositoryProviderTransaction;
                }),
        "Post-system cache replacement was not blocked before the provider transaction");
    stub::require_script_consumed();
}

void test_registered_source_provider_selection_precedes_cache_and_suppresses_aur_candidates() {
    const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME");
    expect(xdg_cache_home != nullptr,
           "System/source cache test environment is unavailable");
    const fs::path selection_cache_home =
        fs::path(xdg_cache_home) / "registered-provider-selection";
    fs::create_directory(selection_cache_home);
    fs::permissions(
        selection_cache_home, fs::perms::owner_all,
        fs::perm_options::replace);
    ScopedEnvironmentVariable cache_environment(
        "XDG_CACHE_HOME", selection_cache_home.string());

    stub::reset();
    configure_preferences({"callbackless-source"});
    stub::set_source_identity(
        "callbackless-source",
        registered_aur_identity("callbackless-source"));
    stub::set_provider_candidates(
        "callbackless-source",
        {selected_repository_provider(),
         alternate_repository_provider()});
    SystemSourceUpgradeResult callbackless = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(
        callbackless.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            !fs::exists(selection_cache_home / "moguet") &&
            stub::system_commands().empty(),
        "Callbackless registered-source selection changed its legacy boundary");
    stub::require_script_consumed();

    stub::reset();
    configure_preferences({"aur-candidate-source"});
    stub::set_source_identity(
        "aur-candidate-source",
        registered_aur_identity("aur-candidate-source"));
    stub::set_provider_candidates(
        "aur-candidate-source",
        {selected_repository_provider(), selected_aur_provider()});
    std::size_t aur_candidate_selector_calls = 0;
    stub::set_provider_selector(
        [&aur_candidate_selector_calls](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
            -> std::optional<ProvidedDependency> {
            ++aur_candidate_selector_calls;
            return candidates.front();
        });

    SystemSourceUpgradeResult blocked = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(
        blocked.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            aur_candidate_selector_calls == 0 &&
            !fs::exists(selection_cache_home / "moguet") &&
            stub::system_commands().empty(),
        "Registered source offered an AUR provider or mutated cache/system state");
    stub::require_script_consumed();

    stub::reset();
    const std::string repository_source = "repository-candidate-source";
    configure_preferences({repository_source});
    stub::set_source_identity(
        repository_source,
        registered_aur_identity(repository_source));
    stub::set_provider_candidates(
        repository_source,
        {selected_repository_provider(),
         alternate_repository_provider()});
    std::size_t repository_selector_calls = 0;
    bool selection_preceded_cache = false;
    stub::set_provider_selector(
        [&repository_selector_calls, &selection_preceded_cache,
         &selection_cache_home](
            const std::string&,
            const std::vector<ProvidedDependency>& candidates)
            -> std::optional<ProvidedDependency> {
            ++repository_selector_calls;
            selection_preceded_cache =
                !fs::exists(selection_cache_home / "moguet");
            return candidates.back();
        });
    stub::enqueue_metadata_session(metadata_session(
        {repository_source},
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(full_option_config(), OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation) &&
            repository_selector_calls == 1 &&
            selection_preceded_cache &&
            !fs::exists(selection_cache_home / "moguet"),
        "Read-only registered-source preparation activated the cache");

    enqueue_post_metadata({repository_source});
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::UpToDate, {},
        compatibility_review_provenance()));
    SystemSourceUpgradeResult completed =
        execute_prepared_system_source_upgrade(
            std::move(
                std::get<PreparedSystemSourceUpgrade>(preparation)),
            full_option_config(), OBSERVER);
    expect(completed.is_success(),
           "Repository-only registered-source provider selection failed");
    expect(
        completed.registered_source_results.front().production_outcome.has_value() &&
            completed.registered_source_results.front().production_outcome->source_provenance ==
                compatibility_review_provenance(),
        "Registered AUR aggregate flattened compatibility provenance");
    expect(
        fs::is_directory(selection_cache_home / "moguet"),
        "Actual registered-source execution did not activate the cache");
    stub::require_script_consumed();
}

void test_first_source_failure_stops_suffix() {
    stub::reset();
    const std::vector<std::string> packages = {"first", "second", "third"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_failure("scripted first source failure");

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.status ==
            SystemSourceUpgradeStatus::StoppedOnSourceFailure,
        "First source failure did not stop source phase");
    expect(
        result.registered_source_results[0].status ==
            RegisteredSourceUpgradeStatus::Failed,
        "Failing source was not Failed");
    expect(
        result.registered_source_results[0].package_state_change ==
            PackageStateChange::Unknown,
        "Ordinary source failure guessed package state");
    expect(
        result.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.registered_source_results[2].status ==
                RegisteredSourceUpgradeStatus::NotAttempted,
        "Suffix sources were not retained as NotAttempted");
    expect(stub::source_execution_calls().size() == 1,
           "Source executor continued after first failure");
    expect(result.has_partial_completion(),
           "Completed system + source failure lost partial completion");
    expect(result.failure_diagnostic() ==
               std::optional<std::string>(
                   "Failed while building/installing PackageBase first (first): scripted first source failure"),
           "Source failure diagnostic was lost");
    stub::require_script_consumed();
}

void test_cache_activation_failure_blocks_system_mutation() {
    stub::reset();
    const std::vector<std::string> packages = {"cache-activation"};
    const AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    stub::fail_cache_activation();

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system.status ==
                SystemUpgradePhaseStatus::NotAttempted,
        "Cache activation failure did not block before system mutation");
    expect(
        !result.issues.empty() &&
            result.issues.back().kind ==
                SystemSourceUpgradeIssueKind::
                    CacheAuthorityInvalid &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Cache activation failure lost typed detail");
    expect(stub::system_commands().empty(),
           "Cache activation failure reached the system command");
    expect(stub::source_execution_calls().empty(),
           "Cache activation failure reached source execution");
    stub::require_script_consumed();
}

void test_source_cache_failure_is_typed() {
    stub::reset();
    const std::vector<std::string> packages = {"cache-replaced"};
    const AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_cache_failure();

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.status ==
                SystemSourceUpgradeStatus::StoppedOnSourceFailure &&
            result.system.status ==
                SystemUpgradePhaseStatus::Completed,
        "Source cache failure lost completed system phase");
    expect(
        result.registered_source_results.front().failure_kind ==
            RegisteredSourceUpgradeFailureKind::
                CacheAuthorityFailure,
        "Source cache failure was flattened to build/install failure");
    expect(
        !result.issues.empty() &&
            result.issues.back().kind ==
                SystemSourceUpgradeIssueKind::
                    CacheAuthorityInvalid &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Source cache failure lost typed detail");
    stub::require_script_consumed();
}

void test_partial_source_completion() {
    stub::reset();
    const std::vector<std::string> packages = {"first", "second", "third"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::Installed));
    stub::enqueue_source_failure("scripted second source failure");

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Updated &&
            result.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::Failed &&
            result.registered_source_results[2].status ==
                RegisteredSourceUpgradeStatus::NotAttempted,
        "Partial source completion statuses are inconsistent");
    expect(result.package_state_change() == PackageStateChange::Changed,
           "Known prior update was hidden by later Unknown failure");
    expect(result.has_partial_completion(),
           "Partial source completion helper returned false");
    stub::require_script_consumed();
}

void test_save_failure_preserves_registered_source_partial_outcome() {
    stub::reset();
    stub::set_source_identity("second", ResolvedSourceBuildIdentity(ResolvedAurSourceBuildIdentity("second", "second")));
    const std::vector<std::string> packages = {"first", "second", "third"};
    const auto config = full_option_config();
    auto prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_success(source_execution(SourceBuildExecutionStatus::Installed));
    RecipePatchSaveFailure save{RecipePatchSaveStage::Registration};
    save.retained_material = "/user/material/verified.patch";
    save.association = PatchAssociationFailure{PatchAssociationFailureKind::PublicationUncertain,
                                               "/registry/record.toml",
                                               {},
                                               std::nullopt,
                                               std::nullopt};
    stub::enqueue_source_exception(std::make_exception_ptr(RecipePatchSaveError(save, "scripted save registration uncertain")));
    const auto result = execute_prepared_system_source_upgrade(std::move(prepared), config, OBSERVER);
    expect(result.status == SystemSourceUpgradeStatus::StoppedOnSourceFailure &&
               result.system.status == SystemUpgradePhaseStatus::Completed &&
               result.registered_source_results[0].status == RegisteredSourceUpgradeStatus::Updated &&
               result.registered_source_results[2].status == RegisteredSourceUpgradeStatus::NotAttempted,
           "Save failure lost completed prefix or executed suffix");
    const auto* failure = std::get_if<RegisteredSourceBuildFailureSnapshot>(&result.registered_source_results[1].failure_detail);
    expect(failure && failure->failure_exception && failure->category == RegisteredSourceBuildFailureCategory::PatchCustomizationSave,
           "Registered source lost typed save failure/category");
    bool retained = false;
    try {
        std::rethrow_exception(failure->failure_exception);
    } catch(const RecipePatchSaveError& error) {
        retained = error.failure().stage == RecipePatchSaveStage::Registration &&
                   error.failure().retained_material == save.retained_material &&
                   error.failure().association->kind == PatchAssociationFailureKind::PublicationUncertain;
    }
    expect(retained, "Registered source flattened save partial outcome");
    stub::require_script_consumed();
}

void test_registered_package_base_success_retains_typed_aggregate() {
    const SystemSourceUpgradeResult result =
        execute_registered_repository_case(
            "aggregate-child", "aggregate-base", []() {
                stub::enqueue_package_base_success(
                    "aggregate-base",
                    {selected_child("aggregate-child")},
                    {ArtifactPackageIdentity{
                         "aggregate-sibling", "3.0-1"},
                     ArtifactPackageIdentity{
                         "aggregate-child-debug", "2.0-1"}});
            });
    const RegisteredSourceUpgradeResult& source =
        result.registered_source_results.front();
    expect(
        result.status == SystemSourceUpgradeStatus::Completed &&
            source.status == RegisteredSourceUpgradeStatus::Updated &&
            source.failure_kind ==
                RegisteredSourceUpgradeFailureKind::None &&
            source.package_state_change ==
                PackageStateChange::Changed &&
            source.package_base_execution.has_value() &&
            source.package_base_execution->package_base ==
                "aggregate-base" &&
            source.package_base_execution->selected_child
                    .identity.package_name ==
                "aggregate-child" &&
            source.package_base_execution->selected_child
                    .identity.full_version ==
                "2.0-1" &&
            source.package_base_execution->unselected_artifacts
                    .size() ==
                2 &&
            source.package_base_execution->unselected_artifacts[0]
                    .package_name ==
                "aggregate-sibling" &&
            source.package_base_execution->unselected_artifacts[1]
                    .package_name ==
                "aggregate-child-debug" &&
            std::holds_alternative<std::monostate>(
                source.failure_detail),
        "Registered PackageBase success did not retain its typed aggregate");
    stub::require_script_consumed();
}

void test_registered_package_base_phase_and_preparation_failures_are_typed() {
    struct PhaseCase {
        SeparatedPackageBaseSourceBuildFailurePhase phase;
        RegisteredSourceBuildFailureCategory category;
    };
    const std::vector<PhaseCase> phase_cases = {
        {SeparatedPackageBaseSourceBuildFailurePhase::Build,
         RegisteredSourceBuildFailureCategory::Build},
        {SeparatedPackageBaseSourceBuildFailurePhase::ArtifactValidation,
         RegisteredSourceBuildFailureCategory::ArtifactValidation},
        {SeparatedPackageBaseSourceBuildFailurePhase::ArtifactIdentity,
         RegisteredSourceBuildFailureCategory::ArtifactIdentity},
    };
    for(const PhaseCase& phase_case : phase_cases) {
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "phase-child", "phase-base", [&]() {
                    stub::enqueue_package_base_phase_failure(
                        phase_case.phase,
                        "scripted PackageBase phase failure");
                });
        const RegisteredSourceUpgradeResult& source =
            result.registered_source_results.front();
        const auto* detail =
            std::get_if<RegisteredSourceBuildFailureSnapshot>(
                &source.failure_detail);
        expect(
            result.status ==
                    SystemSourceUpgradeStatus::
                        StoppedOnSourceFailure &&
                source.status ==
                    RegisteredSourceUpgradeStatus::Failed &&
                source.failure_kind ==
                    RegisteredSourceUpgradeFailureKind::
                        BuildOrInstallFailed &&
                source.package_state_change ==
                    PackageStateChange::Unknown &&
                detail != nullptr &&
                detail->category == phase_case.category &&
                detail->diagnostic ==
                    "scripted PackageBase phase failure",
            "Registered PackageBase phase failure lost typed detail");
        stub::require_script_consumed();
    }

    {
        PackageBaseArtifactIdentitySelectionFailure failure;
        failure.package_base = "selection-base";
        failure.missing_required_artifacts.push_back(
            MissingRequiredArtifact{
                0,
                RequiredPackageArtifactTarget{
                    "selection-base", "selection-child",
                    DesiredInstallReason::Explicit}});
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "selection-child", "selection-base", [&]() {
                    stub::enqueue_package_base_selection_failure(
                        std::move(failure),
                        "scripted selection failure");
                });
        const RegisteredSourceUpgradeResult& source =
            result.registered_source_results.front();
        const auto* detail = std::get_if<
            PackageBaseArtifactIdentitySelectionFailure>(
            &source.failure_detail);
        expect(
            detail != nullptr &&
                detail->package_base == "selection-base" &&
                detail->missing_required_artifacts.size() == 1 &&
                source.package_state_change ==
                    PackageStateChange::Unknown,
            "Registered selection failure was flattened to diagnostic text");
        stub::require_script_consumed();
    }

    {
        MixedPackageBaseInstallReasonUnsupported failure;
        failure.package_base = "mixed-base";
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "mixed-child", "mixed-base", [&]() {
                    stub::enqueue_package_base_mixed_reason_failure(
                        std::move(failure),
                        "scripted mixed reason failure");
                });
        const auto* detail = std::get_if<
            MixedPackageBaseInstallReasonUnsupported>(
            &result.registered_source_results.front().failure_detail);
        expect(
            detail != nullptr && detail->package_base == "mixed-base",
            "Registered mixed-reason failure was flattened");
        stub::require_script_consumed();
    }
}

void test_registered_package_base_transaction_and_metadata_failures_are_typed() {
    {
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "transaction-child", "transaction-base", []() {
                    stub::enqueue_package_base_transaction_failure(
                        PackageBaseArtifactInstallTransactionFailureKind::
                            NonzeroExit,
                        "transaction-base",
                        {PackageBaseArtifactInstallTransactionAttempt{
                            ArtifactPackageIdentity{
                                "transaction-child",
                                "4.0-1"},
                            DesiredInstallReason::Explicit}},
                        73,
                        "scripted package transaction failure");
                });
        const RegisteredSourceUpgradeResult& source =
            result.registered_source_results.front();
        const auto* detail = std::get_if<
            RegisteredSourcePackageTransactionFailureSnapshot>(
            &source.failure_detail);
        expect(
            detail != nullptr &&
                detail->category ==
                    PackageBaseArtifactInstallTransactionFailureKind::
                        NonzeroExit &&
                detail->attempts.size() == 1 &&
                detail->attempts.front().identity.package_name ==
                    "transaction-child" &&
                detail->attempts.front().identity.full_version ==
                    "4.0-1" &&
                detail->exit_code == std::optional<int>{73} &&
                source.package_transaction_failure.has_value() &&
                source.package_transaction_failure->attempts.size() ==
                    1,
            "Registered package transaction failure lost typed attempt evidence");
        stub::require_script_consumed();
    }

    {
        const PackageMetadataFailure failure{
            PackageMetadataErrorCode::MalformedMetadata,
            "scripted PackageBase metadata failure"};
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "metadata-child", "metadata-base", [&]() {
                    stub::enqueue_package_base_metadata_failure(
                        failure);
                });
        const auto* detail = std::get_if<PackageMetadataFailure>(
            &result.registered_source_results.front().failure_detail);
        expect(
            detail != nullptr &&
                detail->code ==
                    PackageMetadataErrorCode::MalformedMetadata &&
                detail->diagnostic ==
                    "scripted PackageBase metadata failure",
            "Registered PackageBase metadata failure lost typed detail");
        stub::require_script_consumed();
    }
}

void test_registered_package_base_correlation_failures_fail_closed() {
    {
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "correlation-child", "correlation-base", []() {
                    stub::enqueue_package_base_success(
                        "other-base",
                        {selected_child("correlation-child")});
                });
        const auto* detail = std::get_if<
            RegisteredSourceExecutionCorrelationFailure>(
            &result.registered_source_results.front().failure_detail);
        expect(
            detail != nullptr &&
                detail->reason ==
                    RegisteredSourceExecutionCorrelationFailureReason::
                        PackageBaseMismatch &&
                result.registered_source_results.front()
                        .package_state_change ==
                    PackageStateChange::Unknown,
            "Mismatched PackageBase aggregate was accepted as success");
        stub::require_script_consumed();
    }

    {
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "needed-child", "needed-base", []() {
                    stub::enqueue_package_base_success(
                        "needed-base",
                        {selected_child(
                            "needed-child",
                            ArtifactInstallExecutionOutcome::
                                SkippedAsNeeded)});
                });
        const auto* detail = std::get_if<
            RegisteredSourceExecutionCorrelationFailure>(
            &result.registered_source_results.front().failure_detail);
        expect(
            detail != nullptr &&
                detail->reason ==
                    RegisteredSourceExecutionCorrelationFailureReason::
                        UnexpectedSkippedAsNeeded,
            "needed=false PackageBase result accepted SkippedAsNeeded");
        stub::require_script_consumed();
    }

    {
        PackageBaseArtifactIdentitySelectionFailure failure;
        failure.package_base = "wrong-preparation-base";
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "preparation-correlation-child",
                "preparation-correlation-base", [&]() {
                    stub::enqueue_package_base_selection_failure(
                        std::move(failure),
                        "scripted mismatched preparation failure");
                });
        const auto* detail = std::get_if<
            RegisteredSourceExecutionCorrelationFailure>(
            &result.registered_source_results.front().failure_detail);
        expect(
            detail != nullptr &&
                detail->reason ==
                    RegisteredSourceExecutionCorrelationFailureReason::
                        PackageBaseMismatch,
            "Mismatched preparation failure retained false authority");
        stub::require_script_consumed();
    }

    {
        const SystemSourceUpgradeResult result =
            execute_registered_repository_case(
                "attempt-child", "attempt-base", []() {
                    stub::enqueue_package_base_transaction_failure(
                        PackageBaseArtifactInstallTransactionFailureKind::
                            ProcessException,
                        "attempt-base",
                        {PackageBaseArtifactInstallTransactionAttempt{
                            ArtifactPackageIdentity{
                                "other-child", "5.0-1"},
                            DesiredInstallReason::Explicit}},
                        std::nullopt,
                        "scripted mismatched attempt");
                });
        const RegisteredSourceUpgradeResult& source =
            result.registered_source_results.front();
        const auto* detail = std::get_if<
            RegisteredSourceExecutionCorrelationFailure>(
            &source.failure_detail);
        expect(
            detail != nullptr &&
                detail->reason ==
                    RegisteredSourceExecutionCorrelationFailureReason::
                        SelectedArtifactIdentityMismatch &&
                source.package_transaction_failure.has_value() &&
                source.package_transaction_failure->attempts.front()
                        .identity.package_name ==
                    "other-child",
            "Transaction correlation failure discarded safe attempt evidence");
        stub::require_script_consumed();
    }
}

void test_cleanup_failure_preserves_transaction_outcome() {
    stub::reset();
    const std::vector<std::string> packages = {"first", "second"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_package_base_cleanup_failure(
        "first",
        {selected_child("first")},
        {ArtifactPackageIdentity{"first-debug", "2.0-1"}},
        "scripted cleanup failure");

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    const RegisteredSourceUpgradeResult& failed =
        result.registered_source_results.front();
    expect(
        result.status == SystemSourceUpgradeStatus::
                             StoppedAfterSourceCleanupFailure,
        "Cleanup failure did not retain aggregate status");
    expect(
        failed.status ==
            RegisteredSourceUpgradeStatus::UpdatedCleanupFailed,
        "Installed cleanup failure lost Updated outcome");
    expect(failed.package_state_change == PackageStateChange::Changed,
           "Installed cleanup failure lost package-state change");
    expect(failed.cleanup_diagnostic ==
               std::optional<std::string>("scripted cleanup failure"),
           "Cleanup diagnostic was lost");
    expect(
        failed.package_base_execution.has_value() &&
            failed.package_base_execution->package_base == "first" &&
            failed.package_base_execution->selected_child
                    .identity.package_name ==
                "first" &&
            failed.package_base_execution->unselected_artifacts
                    .size() ==
                1 &&
            failed.package_base_execution->unselected_artifacts
                    .front()
                    .package_name ==
                "first-debug",
        "Registered cleanup failure flattened the PackageBase aggregate");
    expect(
        result.registered_source_results[1].status ==
            RegisteredSourceUpgradeStatus::NotAttempted,
        "Cleanup failure executed a later source");
    expect(result.has_cleanup_failure(),
           "Cleanup helper returned false");
    expect(result.has_partial_completion(),
           "Cleanup partial completion helper returned false");
    stub::require_script_consumed();
}

void test_no_change_cleanup_failure() {
    stub::reset();
    const std::vector<std::string> packages = {"needed-skip"};
    stub::set_source_identity(
        packages.front(), registered_aur_identity(packages.front()));
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_cleanup_failure(
        ArtifactInstallExecutionOutcome::SkippedAsNeeded,
        "scripted no-change cleanup failure");

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.registered_source_results.front().status ==
            RegisteredSourceUpgradeStatus::NoChangeCleanupFailed,
        "SkippedAsNeeded cleanup failure lost NoChange outcome");
    expect(
        result.registered_source_results.front().package_state_change ==
            PackageStateChange::NoChange,
        "No-change cleanup failure guessed a package change");
    expect(result.has_cleanup_failure(),
           "No-change cleanup helper returned false");
    expect(
        stub::source_preparation_calls().empty() &&
            stub::package_base_source_execution_calls().empty() &&
            stub::source_execution_calls().size() == 1,
        "Registered AUR cleanup route leaked into repository PackageBase execution");
    stub::require_script_consumed();
}

void test_preference_read_failure_blocks_before_mutation() {
    const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME");
    expect(xdg_cache_home != nullptr,
           "System/source cache test environment is unavailable");
    const fs::path cache_root = fs::path(xdg_cache_home) / "moguet";
    expect(!fs::exists(cache_root),
           "Cache root existed before strict preference validation");

    stub::reset();
    stub::set_preference_directory(preference_directory({"broken"}));
    SourcePreferenceFailure failure{
        SourcePreferenceFailureKind::ReadFailed,
        fs::path("/preferences/broken"),
        std::make_error_code(std::errc::io_error),
        std::nullopt,
        "scripted strict preference read failure"};
    stub::enqueue_preference_result("broken", failure);

    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(
        result.status ==
            SystemSourceUpgradeStatus::BlockedBeforeMutation,
        "Preference failure did not block preparation");
    expect(
        result.system.status == SystemUpgradePhaseStatus::NotAttempted,
        "Preference failure attempted the system phase");
    expect(
        result.registered_source_results.front().status ==
            RegisteredSourceUpgradeStatus::Incomplete,
        "Preference failure was not retained as Incomplete");
    expect(
        result.issues.front().source_preference_failure.has_value(),
        "Typed preference failure was lost");
    expect(result.has_blocking_issue(),
           "Preference failure helper did not report a blocker");
    expect(stub::system_commands().empty(),
           "Preference failure reached system mutation");
    expect(stub::source_execution_calls().empty(),
           "Preference failure reached source mutation");
    expect(!fs::exists(cache_root),
           "Preference failure created the Moguet cache root");
    stub::require_script_consumed();
}

void test_initial_cache_resolution_failure_is_typed() {
    stub::reset();
    const std::vector<std::string> packages = {"cache-resolution"};
    configure_preferences(packages);
    stub::enqueue_metadata_session(metadata_session(
        packages,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    ScopedEnvironmentVariable cache_environment(
        "XDG_CACHE_HOME", "relative-cache-home");

    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(full_option_config(), OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "Read-only preparation consulted the relative cache path");
    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(std::get<PreparedSystemSourceUpgrade>(
                preparation)),
            full_option_config(), OBSERVER);
    expect(
        result.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system.status ==
                SystemUpgradePhaseStatus::NotAttempted &&
            !result.issues.empty() &&
            result.issues.back().kind ==
                SystemSourceUpgradeIssueKind::
                    CacheAuthorityInvalid,
        "Relative XDG cache path did not produce a typed blocker");
    expect(
        result.issues.back().cache_resolution_failure.has_value() &&
            result.issues.back().cache_resolution_failure->code ==
                xdg_paths::ResolutionErrorCode::RelativePath &&
            !result.issues.back().cache_preparation_failure.has_value() &&
            !result.issues.back().trusted_cache_failure.has_value(),
        "Cache resolution failure lost or mixed typed detail");
    expect(stub::system_commands().empty() &&
               stub::source_execution_calls().empty(),
           "Cache resolution failure reached external mutation");
    stub::require_script_consumed();
}

void test_initial_cache_preparation_failure_is_typed() {
    const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME");
    expect(xdg_cache_home != nullptr,
           "System/source cache test environment is unavailable");
    const fs::path missing_anchor =
        fs::path(xdg_cache_home) / "missing-explicit-cache-anchor";
    expect(!fs::exists(missing_anchor),
           "Missing cache anchor fixture already exists");

    stub::reset();
    const std::vector<std::string> packages = {"cache-preparation"};
    configure_preferences(packages);
    stub::enqueue_metadata_session(metadata_session(
        packages,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    ScopedEnvironmentVariable cache_environment(
        "XDG_CACHE_HOME", missing_anchor.string());

    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(full_option_config(), OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "Read-only preparation consulted the missing cache anchor");
    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(std::get<PreparedSystemSourceUpgrade>(
                preparation)),
            full_option_config(), OBSERVER);
    expect(
        result.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system.status ==
                SystemUpgradePhaseStatus::NotAttempted &&
            !result.issues.empty() &&
            result.issues.back().kind ==
                SystemSourceUpgradeIssueKind::
                    CacheAuthorityInvalid,
        "Missing explicit cache anchor did not produce a typed blocker");
    expect(
        result.issues.back().cache_preparation_failure.has_value() &&
            result.issues.back().cache_preparation_failure->code ==
                xdg_directory_safety::PreparationErrorCode::
                    MissingAnchor &&
            !result.issues.back().cache_resolution_failure.has_value() &&
            !result.issues.back().trusted_cache_failure.has_value(),
        "Cache preparation failure lost or mixed typed detail");
    expect(!fs::exists(missing_anchor),
           "Cache preparation failure created a missing explicit anchor");
    expect(stub::system_commands().empty() &&
               stub::source_execution_calls().empty(),
           "Cache preparation failure reached external mutation");
    stub::require_script_consumed();
}

void test_initial_trusted_cache_failure_is_typed() {
    stub::reset();
    const std::vector<std::string> packages = {
        "trusted-cache-preparation"};
    configure_preferences(packages);
    stub::enqueue_metadata_session(metadata_session(
        packages,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    ValidatedCacheRoot cache_root = prepare_process_cache_root();
    const fs::path active_path = cache_root.path();
    const fs::path moved_path =
        active_path.parent_path() / "moguet-revoked-preparation";

    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(full_option_config(), OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "Read-only preparation unexpectedly depended on an external cache authority");

    std::error_code rename_error;
    fs::rename(active_path, moved_path, rename_error);
    expect(!rename_error,
           "Failed to revoke trusted cache preparation fixture");

    std::optional<SystemSourceUpgradeResult> execution_result;
    try {
        execution_result.emplace(execute_prepared_system_source_upgrade(
            std::move(std::get<PreparedSystemSourceUpgrade>(preparation)),
            full_option_config(), OBSERVER, cache_root));
    } catch(...) {
        std::error_code restore_error;
        fs::rename(moved_path, active_path, restore_error);
        throw;
    }
    fs::rename(moved_path, active_path, rename_error);
    expect(!rename_error,
           "Failed to restore trusted cache preparation fixture");

    SystemSourceUpgradeResult result = std::move(execution_result.value());
    expect(
        result.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system.status ==
                SystemUpgradePhaseStatus::NotAttempted &&
            !result.issues.empty() &&
            result.issues.back().kind ==
                SystemSourceUpgradeIssueKind::
                    CacheAuthorityInvalid,
        "Revoked trusted cache did not produce a typed blocker");
    expect(
        result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement &&
            !result.issues.back().cache_resolution_failure.has_value() &&
            !result.issues.back().cache_preparation_failure.has_value(),
        "Trusted cache preparation failure lost or mixed typed detail");
    expect(stub::system_commands().empty() &&
               stub::source_execution_calls().empty(),
           "Trusted cache preparation failure reached external mutation");
    stub::require_script_consumed();
}

void test_cache_seed_failure_is_typed() {
    stub::reset();
    const std::vector<std::string> packages = {"cache-seed"};
    configure_preferences(packages);
    stub::enqueue_metadata_session(metadata_session(
        packages,
        LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(full_option_config(), OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "Read-only preparation did not produce an execution capability");
    stub::fail_cache_seed();

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(std::get<PreparedSystemSourceUpgrade>(
                preparation)),
            full_option_config(), OBSERVER);
    expect(
        result.status ==
                SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.system.status ==
                SystemUpgradePhaseStatus::NotAttempted &&
            !result.issues.empty() &&
            result.issues.back().kind ==
                SystemSourceUpgradeIssueKind::
                    CacheAuthorityInvalid &&
            result.issues.back().trusted_cache_failure.has_value() &&
            result.issues.back().trusted_cache_failure->code ==
                TrustedCacheErrorCode::ConcurrentReplacement,
        "Cache seed failure was flattened to invocation preparation");
    expect(stub::system_commands().empty() &&
               stub::source_execution_calls().empty(),
           "Cache seed failure reached external mutation");
    stub::require_script_consumed();
}

void test_registered_preference_disappearance_blocks_before_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({"vanished"}));
    stub::enqueue_preference_result(
        "vanished", SourcePreferenceAbsent{});

    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(
        result.status ==
            SystemSourceUpgradeStatus::BlockedBeforeMutation,
        "Disappeared registered preference did not block preparation");
    expect(
        result.registered_source_results.front().status ==
            RegisteredSourceUpgradeStatus::Incomplete,
        "Disappeared preference was not retained as Incomplete");
    expect(result.failure_diagnostic() ==
               std::optional<std::string>(
                   "The registered source preference disappeared before preparation: /preferences/vanished"),
           "Disappeared preference diagnostic was lost");
    expect(stub::system_commands().empty(),
           "Disappeared preference reached system mutation");
    stub::require_script_consumed();
}

void test_known_package_base_survives_later_preparation_failure() {
    stub::reset();
    configure_preferences({"split"});
    stub::set_source_identity(
        "split",
        ResolvedSourceBuildIdentity{
            ResolvedAurSourceBuildIdentity{
                "split", "known-base"}});
    BuildPlan plan;
    const RootTargetIdentity root{0, "split"};
    plan.root_targets.push_back(root);
    plan.order.push_back(BuildPlanEntry{"known-base", {"split"}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "split", "known-base", {PackageRole::Root}, {root}});
    stub::set_aur_invocation_plan(std::move(plan));
    stub::fail_source_work_item(
        "split", "scripted post-identity preparation failure");

    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_system_source_upgrade_unified_plan(
            SystemSourceUpgradeUnifiedPlanProjectionInput{
                std::cref(result)});
    const UnifiedPlanObservationResult& observation_result =
        projection->observation_result();
    const UnifiedPlanObservation* observation =
        observation_result.observation();
    expect(
        observation_result.is_valid() && observation != nullptr &&
            observation->status() ==
                UnifiedPlanObservationStatus::Blocked &&
            observation->transaction_intents().empty(),
        "Actual blocked preparation result did not reach a typed Blocked observation");
    const RegisteredSourceUpgradeResult& source =
        result.registered_source_results.front();
    expect(source.resolved_package_base ==
               std::optional<std::string>("known-base"),
           "Known PackageBase was lost after later failure");
    expect(source.canonical_source_identity_key ==
               std::optional<std::string>("aur:known-base"),
           "Known canonical source key was lost after later failure");
    expect(source.diagnostic ==
               std::optional<std::string>(
                   "scripted post-identity preparation failure"),
           "Post-identity failure diagnostic was lost");
    expect(stub::system_commands().empty(),
           "Post-identity preparation failure reached mutation");
    stub::require_script_consumed();
}

void test_actual_constraint_blocker_reaches_unified_projection() {
    stub::reset();
    configure_preferences({"constraint-root"});
    stub::set_source_identity(
        "constraint-root",
        ResolvedSourceBuildIdentity{
            ResolvedAurSourceBuildIdentity{
                "constraint-root", "constraint-base"}});

    BuildPlan plan;
    const RootTargetIdentity root{0, "constraint-root"};
    plan.root_targets.push_back(root);
    plan.order.push_back(
        BuildPlanEntry{"constraint-base", {"constraint-root"}});
    plan.package_targets.push_back(PlannedPackageTarget{
        "constraint-root", "constraint-base", {PackageRole::Root}, {root}});
    BuildPlanDependencyEdge edge;
    edge.parent_package_name = "constraint-root";
    edge.parent_package_base = "constraint-base";
    edge.dependency_spec = "missing-runtime>=2";
    edge.role = PackageRole::RuntimeDependency;
    edge.kind = DependencyKind::Unknown;
    edge.constraint_evaluation = ConstraintEvaluation::unsatisfied();
    plan.dependency_edges.push_back(std::move(edge));
    stub::set_aur_invocation_plan(std::move(plan));
    stub::fail_build_plan_guard("scripted constraint blocker");

    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    const std::unique_ptr<UnifiedPlanProjection> projection =
        project_system_source_upgrade_unified_plan(
            SystemSourceUpgradeUnifiedPlanProjectionInput{
                std::cref(result)});
    const UnifiedPlanObservation* observation =
        projection->observation_result().observation();
    expect(
        observation != nullptr &&
            observation->status() ==
                UnifiedPlanObservationStatus::Blocked &&
            std::any_of(
                observation->blockers().begin(),
                observation->blockers().end(),
                [](const UnifiedPlanBlocker& blocker) {
                    return std::holds_alternative<
                        ConstraintFailureUnifiedPlanBlocker>(
                        blocker);
                }) &&
            observation->transaction_intents().empty(),
        "Actual constraint failure was not retained as a typed Blocked observation");
    expect(stub::system_commands().empty(),
           "Constraint projection crossed the system mutation boundary");
    stub::require_script_consumed();
}

void test_system_snapshot_failure_is_unknown_and_nonblocking() {
    stub::reset();
    const std::vector<std::string> packages = {"source"};
    configure_preferences(packages);
    stub::MetadataSessionScript before = metadata_session(packages, {});
    before.local_package_snapshot = PackageMetadataFailure{
        PackageMetadataErrorCode::MalformedMetadata,
        "scripted pre-system snapshot failure"};
    stub::enqueue_metadata_session(std::move(before));
    AppConfig config = full_option_config();
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(config, OBSERVER);
    expect(
        std::holds_alternative<PreparedSystemSourceUpgrade>(preparation),
        "System observability failure blocked source preparation");
    PreparedSystemSourceUpgrade prepared = std::move(
        std::get<PreparedSystemSourceUpgrade>(preparation));
    enqueue_post_metadata(packages);
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::UpToDate));

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.system.status == SystemUpgradePhaseStatus::Completed,
        "Snapshot failure changed successful system command status");
    expect(
        result.system.package_state_change == PackageStateChange::Unknown,
        "Snapshot failure was rounded to a boolean state");
    expect(result.package_state_change() == PackageStateChange::Unknown,
           "Aggregate pre-system snapshot failure was not Unknown");
    expect(!result.definitely_changed_package_state(),
           "Unknown pre-system snapshot was reported as definitely changed");
    expect(result.system.before_snapshot_failure.has_value(),
           "Pre-system snapshot failure was not retained");
    expect(result.is_success(),
           "Pre-system snapshot failure changed successful operation outcome");
    expect(!result.has_blocking_issue(),
           "Observability-only snapshot failure became a blocker");
    expect(stub::source_execution_calls().size() == 1,
           "System snapshot failure blocked valid source execution");
    stub::require_script_consumed();
}

void test_post_system_snapshot_failure_is_unknown_and_nonblocking() {
    stub::reset();
    const std::vector<std::string> packages = {"source"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    stub::MetadataSessionScript after = metadata_session(packages, {});
    after.local_package_snapshot = PackageMetadataFailure{
        PackageMetadataErrorCode::MalformedMetadata,
        "scripted post-system snapshot failure"};
    stub::enqueue_metadata_session(std::move(after));
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::UpToDate));

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.system.package_state_change == PackageStateChange::Unknown,
        "Post-system snapshot failure was not Unknown");
    expect(result.package_state_change() == PackageStateChange::Unknown,
           "Aggregate post-system snapshot failure was not Unknown");
    expect(!result.definitely_changed_package_state(),
           "Unknown post-system snapshot was reported as definitely changed");
    expect(result.system.after_snapshot_failure.has_value(),
           "Post-system snapshot failure was lost");
    expect(result.is_success(),
           "Post-system snapshot failure changed successful operation outcome");
    expect(result.status == SystemSourceUpgradeStatus::Completed,
           "Observability failure stopped source execution");
    expect(result.registered_source_results.front().status ==
               RegisteredSourceUpgradeStatus::NoChange,
           "Source result was lost after system snapshot failure");
    stub::require_script_consumed();
}

void test_post_source_metadata_failure_stops_all_source_mutation() {
    stub::reset();
    const std::vector<std::string> packages = {"first", "second"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    stub::MetadataSessionScript after = metadata_session(
        packages, LocalPackageVersionSnapshot{{"core", "1.0-1"}});
    after.installed_package_results["second"] = PackageMetadataFailure{
        PackageMetadataErrorCode::QueryFailed,
        "scripted post-system query failure"};
    stub::enqueue_metadata_session(std::move(after));

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.status ==
            SystemSourceUpgradeStatus::StoppedOnSourceFailure,
        "Post-system source metadata failure did not stop phase");
    expect(
        result.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::NotAttempted &&
            result.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::Incomplete,
        "Post-system metadata result attribution is inconsistent");
    expect(stub::source_execution_calls().empty(),
           "Post-system metadata failure allowed source mutation");
    expect(result.has_partial_completion(),
           "System-completed metadata stop lost partial completion");
    stub::require_script_consumed();
}

void test_update_status_unknown_is_incomplete_but_continues() {
    stub::reset();
    const std::vector<std::string> packages = {"unknown", "later"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    enqueue_post_metadata(packages);
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::UpdateStatusUnknownSkipped,
        "scripted unknown update status"));
    stub::enqueue_source_success(source_execution(
        SourceBuildExecutionStatus::Installed));

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(result.status == SystemSourceUpgradeStatus::Completed,
           "Normal unknown-status skip stopped the phase");
    expect(
        result.registered_source_results[0].status ==
                RegisteredSourceUpgradeStatus::Incomplete &&
            result.registered_source_results[1].status ==
                RegisteredSourceUpgradeStatus::Updated,
        "Unknown-status skip result/continue policy changed");
    expect(!result.is_success(),
           "Incomplete source was rounded to aggregate success");
    expect(result.has_partial_completion(),
           "Incomplete+updated result lost partial completion");
    expect(result.diagnostics.back().diagnostic ==
               "scripted unknown update status",
           "Unknown-status diagnostic was lost");
    expect(
        stub::source_preparation_calls().size() == 2 &&
            stub::package_base_source_execution_calls().size() == 1 &&
            stub::source_preparation_calls().front().package_name ==
                "unknown" &&
            stub::package_base_source_execution_calls().front().package_name ==
                "later",
        "Unknown closed preparation outcome did not continue to the later NeedsBuild source");
    stub::require_script_consumed();
}

void test_invalid_preference_name_blocks_before_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({"bad name", "valid"}));
    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(
        result.status == SystemSourceUpgradeStatus::BlockedBeforeMutation &&
            result.failure_diagnostic().has_value() &&
            result.failure_diagnostic()->find("invalid entry name") !=
                std::string::npos,
        "Invalid preference name was not a hard preparation error");
    expect(
        std::none_of(
            stub::events().begin(), stub::events().end(),
            [](const stub::Event& event) {
                return event.kind == stub::EventKind::StrictPreferenceRead;
            }),
        "Invalid preference name allowed a later preference read");
    expect(stub::system_commands().empty(),
           "Invalid preference name reached system mutation");
    stub::require_script_consumed();
}

void test_prepared_capability_replay_is_rejected() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    AppConfig config = full_option_config();
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(config, OBSERVER);
    PreparedSystemSourceUpgrade prepared = std::move(
        std::get<PreparedSystemSourceUpgrade>(preparation));
    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    SystemSourceUpgradeResult first =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(first.status == SystemSourceUpgradeStatus::Completed,
           "First capability execution failed");
    expect(!prepared.is_valid(),
           "Caller capability remained valid after consume");

    SystemSourceUpgradeResult replay =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        replay.status ==
            SystemSourceUpgradeStatus::InconsistentResult,
        "Capability replay was not rejected");
    expect(replay.has_blocking_issue(),
           "Capability replay lacks a blocking issue");
    expect(stub::system_commands().size() == 1,
           "Capability replay reached a second system mutation");
    stub::require_script_consumed();
}

void test_option_mismatch_is_rejected_before_mutation() {
    stub::reset();
    stub::set_preference_directory(preference_directory({}));
    stub::enqueue_metadata_session(metadata_session(
        {}, LocalPackageVersionSnapshot{{"core", "1.0-1"}}));
    AppConfig prepared_config = full_option_config();
    SystemSourceUpgradePreparation preparation =
        prepare_system_source_upgrade(prepared_config, OBSERVER);
    PreparedSystemSourceUpgrade prepared = std::move(
        std::get<PreparedSystemSourceUpgrade>(preparation));
    AppConfig execution_config = prepared_config;
    execution_config.no_confirm = false;

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), execution_config, OBSERVER);
    expect(
        result.status ==
            SystemSourceUpgradeStatus::InconsistentResult,
        "Option mismatch was not rejected");
    expect(result.system.status == SystemUpgradePhaseStatus::NotAttempted,
           "Option mismatch reached system phase");
    expect(stub::system_commands().empty(),
           "Option mismatch reached system mutation");
    stub::require_script_consumed();
}

void test_inconsistent_correlation_is_rejected_before_mutation() {
    stub::reset();
    const std::vector<std::string> packages = {"source"};
    AppConfig config = full_option_config();
    PreparedSystemSourceUpgrade prepared = prepare_sources(packages, config);
    prepared.make_first_source_correlation_inconsistent_for_test();

    SystemSourceUpgradeResult result =
        execute_prepared_system_source_upgrade(
            std::move(prepared), config, OBSERVER);
    expect(
        result.status ==
            SystemSourceUpgradeStatus::InconsistentResult,
        "Inconsistent correlation was not rejected");
    expect(result.system.status == SystemUpgradePhaseStatus::NotAttempted,
           "Inconsistent correlation reached system phase");
    expect(stub::system_commands().empty(),
           "Inconsistent correlation reached system mutation");
    expect(result.has_not_attempted_sources(),
           "Inconsistent correlation lost NotAttempted source");
    stub::require_script_consumed();
}

void test_preference_enumeration_failure_blocks() {
    stub::reset();
    stub::fail_preference_directory("scripted preference enumeration failure");
    SystemSourceUpgradeResult result = take_blocked(
        prepare_system_source_upgrade(full_option_config(), OBSERVER));
    expect(result.status ==
               SystemSourceUpgradeStatus::BlockedBeforeMutation,
           "Preference enumeration failure did not block");
    expect(result.failure_diagnostic() ==
               std::optional<std::string>(
                   "scripted preference enumeration failure"),
           "Preference enumeration diagnostic was lost");
    expect(stub::system_commands().empty(),
           "Preference enumeration failure reached mutation");
    stub::require_script_consumed();
}

template <typename Function>
void run_case(
    const std::string& name,
    Function&& function,
    std::size_t& completed_cases) {
    function();
    ++completed_cases;
    std::cout << "  ok: " << name << '\n';
}

} // namespace

int main() {
    try {
        TemporaryCacheEnvironment cache_environment;
        std::size_t completed_cases = 0;
        run_case(
            "empty registered source snapshot",
            test_empty_registered_snapshot,
            completed_cases);
        run_case(
            "system-only observation failure remains unverified",
            test_system_only_observation_failure_is_non_blocking,
            completed_cases);
        run_case(
            "preference read failure blocks mutation and cache creation",
            test_preference_read_failure_blocks_before_mutation,
            completed_cases);
        run_case(
            "initial cache resolution failure remains typed",
            test_initial_cache_resolution_failure_is_typed,
            completed_cases);
        run_case(
            "initial cache preparation failure remains typed",
            test_initial_cache_preparation_failure_is_typed,
            completed_cases);
        run_case(
            "initial trusted cache failure remains typed",
            test_initial_trusted_cache_failure_is_typed,
            completed_cases);
        run_case(
            "cache seed failure remains typed",
            test_cache_seed_failure_is_typed,
            completed_cases);
        run_case(
            "preparation retains AUR plan and typed source kind",
            test_preparation_retains_aur_plan_and_source_kind,
            completed_cases);
        run_case(
            "preparation exposes repository work targets",
            test_preparation_exposes_repository_work_targets,
            completed_cases);
        run_case(
            "all sources success, order, options, and package change",
            test_all_sources_success_order_options_and_change,
            completed_cases);
        run_case(
            "source options propagate independently",
            test_source_options_propagate_independently,
            completed_cases);
        run_case(
            "non-regular preference blocks before mutation",
            test_nonregular_preference_blocks_before_mutation,
            completed_cases);
        run_case(
            "source no-change and aggregate no-change",
            test_source_no_change_and_package_state_no_change,
            completed_cases);
        run_case(
            "system failure leaves every source NotAttempted",
            test_system_failure_keeps_all_sources_not_attempted,
            completed_cases);
        run_case(
            "repository provider phase transaction",
            test_repository_provider_transaction_runs_after_system_and_blocks_all_sources_on_failure,
            completed_cases);
        run_case(
            "registered-source provider selection boundary",
            test_registered_source_provider_selection_precedes_cache_and_suppresses_aur_candidates,
            completed_cases);
        run_case(
            "first source failure stops suffix",
            test_first_source_failure_stops_suffix,
            completed_cases);
        run_case(
            "cache activation failure blocks system mutation",
            test_cache_activation_failure_blocks_system_mutation,
            completed_cases);
        run_case(
            "source cache failure remains typed",
            test_source_cache_failure_is_typed,
            completed_cases);
        run_case(
            "partial source completion",
            test_partial_source_completion,
            completed_cases);
        run_case(
            "registered PackageBase success retains typed aggregate",
            test_registered_package_base_success_retains_typed_aggregate,
            completed_cases);
        run_case(
            "registered PackageBase phase and preparation failures are typed",
            test_registered_package_base_phase_and_preparation_failures_are_typed,
            completed_cases);
        run_case(
            "registered PackageBase transaction and metadata failures are typed",
            test_registered_package_base_transaction_and_metadata_failures_are_typed,
            completed_cases);
        run_case(
            "registered PackageBase correlation failures fail closed",
            test_registered_package_base_correlation_failures_fail_closed,
            completed_cases);
        run_case(
            "updated cleanup failure",
            test_cleanup_failure_preserves_transaction_outcome,
            completed_cases);
        run_case(
            "no-change cleanup failure",
            test_no_change_cleanup_failure,
            completed_cases);
        run_case(
            "registered preference disappearance blocks mutation",
            test_registered_preference_disappearance_blocks_before_mutation,
            completed_cases);
        run_case(
            "known PackageBase survives preparation failure",
            test_known_package_base_survives_later_preparation_failure,
            completed_cases);
        run_case(
            "actual constraint blocker reaches unified projection",
            test_actual_constraint_blocker_reaches_unified_projection,
            completed_cases);
        run_case(
            "pre-system package snapshot failure is Unknown",
            test_system_snapshot_failure_is_unknown_and_nonblocking,
            completed_cases);
        run_case(
            "post-system package snapshot failure is Unknown",
            test_post_system_snapshot_failure_is_unknown_and_nonblocking,
            completed_cases);
        run_case(
            "post-system source metadata failure blocks all source mutation",
            test_post_source_metadata_failure_stops_all_source_mutation,
            completed_cases);
        run_case(
            "unknown update status is Incomplete and continues",
            test_update_status_unknown_is_incomplete_but_continues,
            completed_cases);
        run_case(
            "invalid preference name blocks before mutation",
            test_invalid_preference_name_blocks_before_mutation,
            completed_cases);
        run_case(
            "prepared capability replay is rejected",
            test_prepared_capability_replay_is_rejected,
            completed_cases);
        run_case(
            "option mismatch is rejected before mutation",
            test_option_mismatch_is_rejected_before_mutation,
            completed_cases);
        run_case(
            "inconsistent correlation is rejected before mutation",
            test_inconsistent_correlation_is_rejected_before_mutation,
            completed_cases);
        run_case(
            "preference enumeration failure blocks",
            test_preference_enumeration_failure_blocks,
            completed_cases);
        run_case("save failure preserves committed prefix and typed material outcome",
                 test_save_failure_preserves_registered_source_partial_outcome, completed_cases);
        std::cout << "system/source upgrade tests: " << completed_cases
                  << " scenarios passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "system/source upgrade test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
