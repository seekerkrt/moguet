#include "execution_stub.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {

namespace stub = aur_update_execution_runner_test_stub;

struct ScriptedSuccess {
    std::string returned_package_base;
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
};

struct ScriptedCleanupFailure {
    std::string returned_package_base;
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children;
    std::vector<ArtifactPackageIdentity> unselected_artifacts;
    std::string diagnostic;
};

struct ScriptedPhaseFailure {
    SeparatedPackageBaseSourceBuildFailurePhase phase =
        SeparatedPackageBaseSourceBuildFailurePhase::Build;
    std::string diagnostic;
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure;
    std::optional<PackageMetadataFailure> package_metadata_failure;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome;
};

struct ScriptedSelectionFailure {
    PackageBaseArtifactIdentitySelectionFailure failure;
    std::string diagnostic;
};

struct ScriptedMixedReasonFailure {
    MixedPackageBaseInstallReasonUnsupported failure;
    std::string diagnostic;
};

struct ScriptedMetadataFailure {
    PackageMetadataFailure failure;
};

struct ScriptedTrustedCacheFailure {
    TrustedCacheFailure failure;
};

struct ScriptedTransactionFailure {
    PackageBaseArtifactInstallTransactionFailureKind failure_kind =
        PackageBaseArtifactInstallTransactionFailureKind::NonzeroExit;
    std::string returned_package_base;
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts;
    std::optional<int> exit_code;
    std::string diagnostic;
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome;
};

struct ScriptedUnknownFailure {};

using ScriptedOutcome = std::variant<
    ScriptedSuccess,
    ScriptedCleanupFailure,
    ScriptedPhaseFailure,
    ScriptedSelectionFailure,
    ScriptedMixedReasonFailure,
    ScriptedMetadataFailure,
    ScriptedTrustedCacheFailure,
    ScriptedTransactionFailure,
    ScriptedUnknownFailure, ConfirmationResult>;

struct ScriptedExecution {
    stub::ExpectedExecution expected;
    ScriptedOutcome outcome;
};

struct ExecutionStubState {
    std::deque<ScriptedExecution> executions;
    std::vector<stub::ExecutionCall> calls;
    std::vector<stub::Event> events;
    std::vector<stub::InvocationEventKind> invocation_events;
    std::optional<TrustedCacheFailure> cache_activation_failure;
    std::optional<std::string> repository_provider_transaction_failure;
    const PacmanDatabasePaths* first_database_paths_address = nullptr;
    std::optional<std::string> expectation_failure;
};

struct UnknownExecutionFailure {};

ExecutionStubState g_state;

bool same_required_target(
    const RequiredPackageArtifactTarget& actual,
    const RequiredPackageArtifactTarget& expected) noexcept {
    return actual.package_base == expected.package_base &&
           actual.package_name == expected.package_name &&
           actual.desired_reason == expected.desired_reason;
}

bool same_required_targets(
    const std::vector<RequiredPackageArtifactTarget>& actual,
    const std::vector<RequiredPackageArtifactTarget>& expected) noexcept {
    return actual.size() == expected.size() && std::equal(
                                                   actual.begin(), actual.end(), expected.begin(),
                                                   same_required_target);
}

bool same_database_paths(
    const PacmanDatabasePaths& actual,
    const PacmanDatabasePaths& expected) noexcept {
    return actual.root_dir == expected.root_dir &&
           actual.db_path == expected.db_path;
}

bool same_config(const AppConfig& actual, const AppConfig& expected) noexcept {
    return actual.user_config.schema_version ==
               expected.user_config.schema_version &&
           actual.user_config.review.pkgbuild ==
               expected.user_config.review.pkgbuild &&
           actual.user_config.review.diff ==
               expected.user_config.review.diff &&
           actual.user_config.build.mode ==
               expected.user_config.build.mode &&
           actual.no_confirm == expected.no_confirm &&
           actual.rm_deps == expected.rm_deps &&
           actual.editor == expected.editor;
}

std::vector<std::string> required_package_names(
    const ProductionSourceBuildWorkItem& work_item) {
    std::vector<std::string> package_names;
    package_names.reserve(work_item.required_targets.size());
    for(const auto& target : work_item.required_targets) {
        package_names.push_back(target.package_name);
    }
    return package_names;
}

[[noreturn]] void fail_expectation(const std::string& diagnostic) {
    g_state.expectation_failure = diagnostic;
    throw std::logic_error(diagnostic);
}

void require_expected_call(
    const stub::ExpectedExecution& expected,
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config,
    std::size_t call_index) {
    if(expected.call_index != call_index) {
        fail_expectation(
            "AUR update set executor call order differs from its strict expectation.");
    }
    if(expected.package_base != work_item.request.checkout_name) {
        fail_expectation(
            "AUR update set executor PackageBase differs from its strict expectation.");
    }
    if(!same_required_targets(
           work_item.required_targets,
           expected.ordered_required_targets)) {
        fail_expectation(
            "AUR update set executor ordered required targets differ from their strict expectation.");
    }
    if(work_item.request.needed != expected.needed) {
        fail_expectation(
            "AUR update set executor needed option differs from its strict expectation.");
    }
    if(!same_database_paths(database_paths, expected.database_paths)) {
        fail_expectation(
            "AUR update set executor Pacman database snapshot differs from its strict expectation.");
    }
    if(!same_config(config, expected.config)) {
        fail_expectation(
            "AUR update set executor AppConfig snapshot differs from its strict expectation.");
    }
    if(work_item.required_target_provenance !=
           RequiredTargetProvenance::AurBuildPlanProjection ||
       work_item.artifact_lifecycle_intent !=
           ArtifactLifecycleIntent::PackageBaseSet ||
       work_item.request.only_if_updated) {
        fail_expectation(
            "AUR update set executor received a non-AUR or only-if-updated work item.");
    }

    const bool singular = work_item.required_targets.size() == 1;
    if((singular &&
        work_item.request.package_name !=
            work_item.required_targets.front().package_name) ||
       (!singular && !work_item.request.package_name.empty())) {
        fail_expectation(
            "AUR update set executor singular compatibility identity is inconsistent.");
    }

    // POLICY(#267): runnerはinvocation-owned DB valueを全callへ同じreferenceで渡す。
    if(g_state.first_database_paths_address == nullptr) {
        g_state.first_database_paths_address = &database_paths;
    } else if(g_state.first_database_paths_address != &database_paths) {
        fail_expectation(
            "AUR update runner did not reuse one Pacman database snapshot.");
    }
}

void record_event(std::size_t call_index, stub::EventKind kind) {
    stub::ExecutionCall& call = g_state.calls.at(call_index);
    call.events.push_back(kind);
    g_state.events.push_back(stub::Event{
        call_index,
        kind,
        call.package_name,
        call.package_base});
}

template <typename Outcome>
void enqueue(stub::ExpectedExecution expected, Outcome outcome) {
    g_state.executions.push_back(ScriptedExecution{
        std::move(expected),
        ScriptedOutcome{std::in_place_type<Outcome>,
                        std::move(outcome)}});
}

} // namespace

void activate_production_source_build_cache(
    PreparedProductionSourceBuildInvocation&) {
    // Runner tests isolate cache/filesystem mutation behind this seam.
    g_state.invocation_events.push_back(
        stub::InvocationEventKind::CacheActivation);
    if(g_state.cache_activation_failure.has_value()) {
        throw TrustedCacheError(
            std::move(g_state.cache_activation_failure.value()));
    }
}

SelectedRepositoryProviderTransactionResult
execute_selected_repository_provider_transaction(
    const PreparedProductionSourceBuildInvocation& invocation,
    const AppConfig&) {
    SelectedRepositoryProviderTransactionResult result;
    result.selected_providers = invocation.selected_repository_providers;
    if(result.selected_providers.empty()) return result;
    g_state.invocation_events.push_back(
        stub::InvocationEventKind::RepositoryProviderTransaction);
    if(g_state.repository_provider_transaction_failure.has_value()) {
        result.status =
            SelectedRepositoryProviderTransactionStatus::Failed;
        result.package_state_change = PackageStateChange::Unknown;
        result.diagnostic =
            *g_state.repository_provider_transaction_failure;
        return result;
    }
    result.status = SelectedRepositoryProviderTransactionStatus::Succeeded;
    result.package_state_change = PackageStateChange::Unknown;
    result.command_exit_status = 0;
    return result;
}

namespace aur_update_execution_runner_test_stub {

void reset() {
    g_state = ExecutionStubState{};
}

void enqueue_success(
    ExpectedExecution expected,
    std::string returned_package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts) {
    enqueue(
        std::move(expected),
        ScriptedSuccess{
            std::move(returned_package_base),
            std::move(selected_children),
            std::move(unselected_artifacts)});
}

void enqueue_cleanup_failure(
    ExpectedExecution expected,
    std::string returned_package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    std::string diagnostic) {
    enqueue(
        std::move(expected),
        ScriptedCleanupFailure{
            std::move(returned_package_base),
            std::move(selected_children),
            std::move(unselected_artifacts),
            std::move(diagnostic)});
}

void enqueue_phase_failure(
    ExpectedExecution expected,
    SeparatedPackageBaseSourceBuildFailurePhase phase,
    std::string diagnostic,
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure,
    std::optional<PackageMetadataFailure> package_metadata_failure,
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome) {
    enqueue(
        std::move(expected),
        ScriptedPhaseFailure{
            phase, std::move(diagnostic),
            std::move(reviewed_source_failure),
            std::move(package_metadata_failure),
            std::move(production_outcome)});
}

void enqueue_selection_failure(
    ExpectedExecution expected,
    PackageBaseArtifactIdentitySelectionFailure failure,
    std::string diagnostic) {
    enqueue(
        std::move(expected),
        ScriptedSelectionFailure{
            std::move(failure), std::move(diagnostic)});
}

void enqueue_mixed_reason_failure(
    ExpectedExecution expected,
    MixedPackageBaseInstallReasonUnsupported failure,
    std::string diagnostic) {
    enqueue(
        std::move(expected),
        ScriptedMixedReasonFailure{
            std::move(failure), std::move(diagnostic)});
}

void enqueue_metadata_failure(
    ExpectedExecution expected,
    PackageMetadataFailure failure) {
    enqueue(
        std::move(expected),
        ScriptedMetadataFailure{std::move(failure)});
}

void enqueue_trusted_cache_failure(
    ExpectedExecution expected,
    TrustedCacheFailure failure) {
    enqueue(
        std::move(expected),
        ScriptedTrustedCacheFailure{std::move(failure)});
}

void enqueue_transaction_failure(
    ExpectedExecution expected,
    PackageBaseArtifactInstallTransactionFailureKind failure_kind,
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts,
    std::optional<int> exit_code,
    std::string diagnostic,
    std::optional<std::string> returned_package_base,
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome) {
    std::string failure_package_base = returned_package_base.has_value()
                                           ? std::move(*returned_package_base)
                                           : expected.package_base;
    enqueue(
        std::move(expected),
        ScriptedTransactionFailure{
            failure_kind,
            std::move(failure_package_base),
            std::move(attempts),
            exit_code,
            std::move(diagnostic),
            std::move(production_outcome)});
}

void enqueue_confirmation_stop(ExpectedExecution expected, ConfirmationResult result) {
    enqueue(std::move(expected), std::move(result));
}

void enqueue_unknown_failure(ExpectedExecution expected) {
    enqueue(std::move(expected), ScriptedUnknownFailure{});
}

void fail_repository_provider_transaction(std::string diagnostic) {
    g_state.repository_provider_transaction_failure = std::move(diagnostic);
}

void fail_cache_activation(TrustedCacheFailure failure) {
    g_state.cache_activation_failure = std::move(failure);
}

const std::vector<ExecutionCall>& call_history() {
    return g_state.calls;
}

const std::vector<Event>& event_history() {
    return g_state.events;
}

const std::vector<InvocationEventKind>& invocation_event_history() {
    return g_state.invocation_events;
}

void require_script_consumed() {
    if(g_state.expectation_failure.has_value()) {
        throw std::logic_error(*g_state.expectation_failure);
    }
    if(!g_state.executions.empty()) {
        throw std::logic_error(
            "AUR update set executor stub has unconsumed strict expectations.");
    }
}

} // namespace aur_update_execution_runner_test_stub

// The runner target intentionally does not link the production lifecycle TU.
// Define only the owned-value observers needed by the fake set-owner boundary.
TrustedCacheError::TrustedCacheError(TrustedCacheFailure failure)
    : std::runtime_error("scripted trusted cache failure"),
      failure_(std::move(failure)) {
}

const std::string&
PackageBaseSourceBuildExecutionResult::package_base() const noexcept {
    return package_base_;
}

const ProductionSourceBuildProvenance&
PackageBaseSourceBuildExecutionResult::source_provenance() const noexcept {
    return production_outcome_.source_provenance;
}

const ProductionSourceBuildStagedOutcome&
PackageBaseSourceBuildExecutionResult::production_outcome()
    const noexcept {
    return production_outcome_;
}

const std::vector<PackageBaseSourceBuildSelectedResult>&
PackageBaseSourceBuildExecutionResult::selected_children() const noexcept {
    return selected_children_;
}

const std::vector<ArtifactPackageIdentity>&
PackageBaseSourceBuildExecutionResult::unselected_artifacts() const noexcept {
    return unselected_artifacts_;
}

bool PackageBaseSourceBuildExecutionResult::installed_any() const noexcept {
    return std::any_of(
        selected_children_.begin(), selected_children_.end(),
        [](const PackageBaseSourceBuildSelectedResult& child) {
            return child.outcome ==
                   ArtifactInstallExecutionOutcome::Installed;
        });
}

bool PackageBaseSourceBuildExecutionResult::all_skipped_as_needed()
    const noexcept {
    return !selected_children_.empty() && std::all_of(
                                              selected_children_.begin(), selected_children_.end(),
                                              [](const PackageBaseSourceBuildSelectedResult& child) {
                                                  return child.outcome ==
                                                         ArtifactInstallExecutionOutcome::SkippedAsNeeded;
                                              });
}

std::string
PackageBaseSourceBuildExecutionResult::release_package_base() && noexcept {
    return std::move(package_base_);
}

std::vector<PackageBaseSourceBuildSelectedResult>
PackageBaseSourceBuildExecutionResult::release_selected_children() && noexcept {
    return std::move(selected_children_);
}

std::vector<ArtifactPackageIdentity>
PackageBaseSourceBuildExecutionResult::release_unselected_artifacts() && noexcept {
    return std::move(unselected_artifacts_);
}

SeparatedPackageBaseSourceBuildFailurePhase
SeparatedPackageBaseSourceBuildPhaseError::phase() const noexcept {
    return phase_;
}

const std::optional<ReviewedSourceProductionFailure>&
SeparatedPackageBaseSourceBuildPhaseError::reviewed_source_failure()
    const noexcept {
    return reviewed_source_failure_;
}

const std::optional<PackageMetadataFailure>&
SeparatedPackageBaseSourceBuildPhaseError::package_metadata_failure()
    const noexcept {
    return package_metadata_failure_;
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPhaseError::production_outcome()
    const noexcept {
    return production_outcome_;
}

const PackageBaseArtifactIdentitySelectionFailure*
PackageBaseArtifactInstallPreparationFailure::selection_failure()
    const noexcept {
    return std::get_if<PackageBaseArtifactIdentitySelectionFailure>(&failure_);
}

const MixedPackageBaseInstallReasonUnsupported*
PackageBaseArtifactInstallPreparationFailure::mixed_reason_failure()
    const noexcept {
    return std::get_if<MixedPackageBaseInstallReasonUnsupported>(&failure_);
}

const PackageBaseArtifactInstallPreparationFailure&
SeparatedPackageBaseSourceBuildPreparationError::failure() const noexcept {
    return failure_;
}

const PackageBaseArtifactIdentitySelectionFailure*
SeparatedPackageBaseSourceBuildPreparationError::selection_failure()
    const noexcept {
    return failure_.selection_failure();
}

const MixedPackageBaseInstallReasonUnsupported*
SeparatedPackageBaseSourceBuildPreparationError::mixed_reason_failure()
    const noexcept {
    return failure_.mixed_reason_failure();
}

const std::optional<ProductionSourceBuildStagedOutcome>&
SeparatedPackageBaseSourceBuildPreparationError::production_outcome()
    const noexcept {
    return production_outcome_;
}

const PackageBaseSourceBuildExecutionResult&
SeparatedPackageBaseSourceBuildCleanupError::result() const noexcept {
    return result_;
}

PackageBaseSourceBuildExecutionResult
SeparatedPackageBaseSourceBuildCleanupError::release_result() && noexcept {
    return std::move(result_);
}

PackageBaseArtifactInstallTransactionFailureKind
PackageBaseArtifactInstallTransactionError::failure_kind() const noexcept {
    return failure_kind_;
}

const std::string&
PackageBaseArtifactInstallTransactionError::package_base() const noexcept {
    return package_base_;
}

const std::vector<PackageBaseArtifactInstallTransactionAttempt>&
PackageBaseArtifactInstallTransactionError::attempts() const noexcept {
    return attempts_;
}

const std::optional<int>&
PackageBaseArtifactInstallTransactionError::exit_code() const noexcept {
    return exit_code_;
}

const std::optional<ProductionSourceBuildStagedOutcome>&
PackageBaseArtifactInstallTransactionError::production_outcome()
    const noexcept {
    return production_outcome_;
}

void PackageBaseArtifactInstallTransactionError::attach_production_outcome(
    ProductionSourceBuildStagedOutcome production_outcome) {
    if(production_outcome_.has_value()) {
        throw std::logic_error(
            "Scripted transaction failure already has a production outcome.");
    }
    production_outcome_ = std::move(production_outcome);
}

std::vector<PackageBaseArtifactInstallTransactionAttempt>
PackageBaseArtifactInstallTransactionError::release_attempts() && noexcept {
    return std::move(attempts_);
}

SourceBuildPackageBaseExecutionResult
execute_prepared_package_base_source_build_work_item_typed(
    const ProductionSourceBuildWorkItem& work_item,
    const PacmanDatabasePaths& database_paths,
    const AppConfig& config) {
    g_state.invocation_events.push_back(
        stub::InvocationEventKind::SourceExecution);
    const std::size_t call_index = g_state.calls.size();
    const bool singular = work_item.required_targets.size() == 1;
    g_state.calls.push_back(stub::ExecutionCall{
        call_index,
        work_item.request.checkout_name,
        work_item.required_targets,
        singular ? work_item.required_targets.front().package_name
                 : std::string{},
        required_package_names(work_item),
        work_item.request.needed,
        database_paths,
        config,
        {}});

    if(g_state.executions.empty()) {
        fail_expectation(
            "Unexpected AUR update set executor call with no pending strict expectation.");
    }
    require_expected_call(
        g_state.executions.front().expected,
        work_item,
        database_paths,
        config,
        call_index);

    ScriptedExecution scripted = std::move(g_state.executions.front());
    g_state.executions.pop_front();

    record_event(call_index, stub::EventKind::Checkout);
    if(const auto* stop = std::get_if<ConfirmationResult>(&scripted.outcome)) {
        throw ConfirmationOperationStopped(*stop);
    }
    record_event(call_index, stub::EventKind::Build);

    if(auto* success = std::get_if<ScriptedSuccess>(&scripted.outcome)) {
        record_event(call_index, stub::EventKind::Install);
        record_event(call_index, stub::EventKind::Cleanup);
        return PackageBaseSourceBuildExecutionResult::
            make_for_aur_update_runner_test(
                std::move(success->returned_package_base),
                std::move(success->selected_children),
                std::move(success->unselected_artifacts));
    }
    if(auto* cleanup =
           std::get_if<ScriptedCleanupFailure>(&scripted.outcome)) {
        record_event(call_index, stub::EventKind::Install);
        record_event(call_index, stub::EventKind::Cleanup);
        throw SeparatedPackageBaseSourceBuildCleanupError::
            make_for_aur_update_runner_test(
                PackageBaseSourceBuildExecutionResult::
                    make_for_aur_update_runner_test(
                        std::move(cleanup->returned_package_base),
                        std::move(cleanup->selected_children),
                        std::move(cleanup->unselected_artifacts)),
                cleanup->diagnostic);
    }
    if(auto* phase =
           std::get_if<ScriptedPhaseFailure>(&scripted.outcome)) {
        throw SeparatedPackageBaseSourceBuildPhaseError::
            make_for_aur_update_runner_test(
                phase->phase, phase->diagnostic,
                std::move(phase->reviewed_source_failure),
                std::move(phase->package_metadata_failure),
                std::move(phase->production_outcome));
    }
    if(auto* selection =
           std::get_if<ScriptedSelectionFailure>(&scripted.outcome)) {
        throw SeparatedPackageBaseSourceBuildPreparationError::
            make_selection_failure_for_aur_update_runner_test(
                std::move(selection->failure),
                selection->diagnostic);
    }
    if(auto* mixed =
           std::get_if<ScriptedMixedReasonFailure>(&scripted.outcome)) {
        throw SeparatedPackageBaseSourceBuildPreparationError::
            make_mixed_reason_failure_for_aur_update_runner_test(
                std::move(mixed->failure), mixed->diagnostic);
    }
    if(auto* metadata =
           std::get_if<ScriptedMetadataFailure>(&scripted.outcome)) {
        throw PackageMetadataError(std::move(metadata->failure));
    }
    if(auto* cache =
           std::get_if<ScriptedTrustedCacheFailure>(&scripted.outcome)) {
        throw TrustedCacheError(std::move(cache->failure));
    }
    if(auto* transaction =
           std::get_if<ScriptedTransactionFailure>(&scripted.outcome)) {
        record_event(call_index, stub::EventKind::Install);
        PackageBaseArtifactInstallTransactionError error =
            PackageBaseArtifactInstallTransactionError::
                make_for_aur_update_runner_test(
                    transaction->failure_kind,
                    std::move(
                        transaction->returned_package_base),
                    std::move(transaction->attempts),
                    transaction->exit_code,
                    transaction->diagnostic);
        if(transaction->production_outcome.has_value()) {
            error.attach_production_outcome(
                std::move(*transaction->production_outcome));
        }
        throw error;
    }
    if(std::holds_alternative<ScriptedUnknownFailure>(scripted.outcome)) {
        throw UnknownExecutionFailure{};
    }

    throw std::logic_error(
        "AUR update set executor stub has an unknown scripted outcome.");
}
