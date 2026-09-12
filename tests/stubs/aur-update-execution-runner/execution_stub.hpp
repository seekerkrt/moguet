#pragma once

#include "app_config.hpp"
#include "interactive_confirmation.hpp"
#include "source_install.hpp"
#include "trusted_cache.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace aur_update_execution_runner_test_stub {

// set ownerが隠すlifecycleを、runner/operation testでmutation有無と順序だけ
// 観測するためのconceptual event。production primitiveは再実装しない。
enum class EventKind {
    Checkout,
    Build,
    Install,
    Cleanup,
};

enum class InvocationEventKind {
    RepositoryProviderTransaction,
    CacheActivation,
    SourceExecution,
};

struct Event {
    std::size_t call_index = 0;
    EventKind kind = EventKind::Checkout;
    // singular compatibility snapshot。multipleではempty。
    std::string package_name;
    std::string package_base;

    bool operator==(const Event&) const = default;
};

// FIFO call orderを含め、runnerがset ownerへ渡すimmutable inputをexact照合する。
struct ExpectedExecution {
    std::size_t call_index = 0;
    std::string package_base;
    std::vector<RequiredPackageArtifactTarget> ordered_required_targets;
    bool needed = false;
    PacmanDatabasePaths database_paths;
    AppConfig config;
};

struct ExecutionCall {
    std::size_t call_index = 0;
    std::string package_base;
    std::vector<RequiredPackageArtifactTarget> ordered_required_targets;
    // singular compatibility snapshot。multipleではempty。
    std::string package_name;
    std::vector<std::string> plan_package_names;
    bool needed = false;
    PacmanDatabasePaths database_paths;
    AppConfig config;
    std::vector<EventKind> events;
};

void reset();

void enqueue_success(
    ExpectedExecution expected,
    std::string returned_package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts = {});

void enqueue_cleanup_failure(
    ExpectedExecution expected,
    std::string returned_package_base,
    std::vector<PackageBaseSourceBuildSelectedResult> selected_children,
    std::vector<ArtifactPackageIdentity> unselected_artifacts,
    std::string diagnostic);

void enqueue_phase_failure(
    ExpectedExecution expected,
    SeparatedPackageBaseSourceBuildFailurePhase phase,
    std::string diagnostic,
    std::optional<ReviewedSourceProductionFailure>
        reviewed_source_failure = std::nullopt,
    std::optional<PackageMetadataFailure>
        package_metadata_failure = std::nullopt,
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome = std::nullopt);

void enqueue_selection_failure(
    ExpectedExecution expected,
    PackageBaseArtifactIdentitySelectionFailure failure,
    std::string diagnostic);

void enqueue_mixed_reason_failure(
    ExpectedExecution expected,
    MixedPackageBaseInstallReasonUnsupported failure,
    std::string diagnostic);

void enqueue_metadata_failure(
    ExpectedExecution expected,
    PackageMetadataFailure failure);

void enqueue_trusted_cache_failure(
    ExpectedExecution expected,
    TrustedCacheFailure failure);

void enqueue_transaction_failure(
    ExpectedExecution expected,
    PackageBaseArtifactInstallTransactionFailureKind failure_kind,
    std::vector<PackageBaseArtifactInstallTransactionAttempt> attempts,
    std::optional<int> exit_code,
    std::string diagnostic,
    std::optional<std::string> returned_package_base = std::nullopt,
    std::optional<ProductionSourceBuildStagedOutcome>
        production_outcome = std::nullopt);

void enqueue_confirmation_stop(ExpectedExecution expected, ConfirmationResult result);

void enqueue_unknown_failure(ExpectedExecution expected);

void fail_repository_provider_transaction(std::string diagnostic);
void fail_cache_activation(TrustedCacheFailure failure);

const std::vector<ExecutionCall>& call_history();
const std::vector<Event>& event_history();
const std::vector<InvocationEventKind>& invocation_event_history();
void require_script_consumed();

} // namespace aur_update_execution_runner_test_stub
