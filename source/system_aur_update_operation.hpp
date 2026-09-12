#pragma once

#include "aur_update_execution_preparation.hpp"
#include "cli_routing.hpp"
#include "filtered_aur_update_operation.hpp"
#include "package_metadata.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

struct AppConfig;

enum class SystemAurUpdateDryRunMode {
    Auto,
    RepoOnly,
};

enum class SystemAurUpdateDryRunAurObservationBasis {
    CurrentInstalledState,
};

enum class SystemAurUpdateDryRunActualAuthorityRefresh {
    AfterRepositorySuccess,
};

class SystemAurUpdateDryRunRequest final {
public:
    static std::optional<SystemAurUpdateDryRunRequest> from_auto_candidate(
        AutoSystemUpdateRouteCandidate candidate) {
        if(!std::holds_alternative<
               CompatibleAutoSystemUpdatePacmanArguments>(
               candidate.pacman_compatibility) ||
           candidate.ordered_pacman_args.empty()) {
            return std::nullopt;
        }
        return SystemAurUpdateDryRunRequest(
            SystemAurUpdateDryRunMode::Auto,
            std::move(candidate.ordered_pacman_args),
            candidate.repository_needed);
    }

    explicit SystemAurUpdateDryRunRequest(
        RepoOnlySystemUpdateRouteCandidate candidate)
        : SystemAurUpdateDryRunRequest(
              SystemAurUpdateDryRunMode::RepoOnly,
              std::move(candidate.ordered_pacman_args),
              candidate.repository_needed) {
    }

    SystemAurUpdateDryRunRequest(const SystemAurUpdateDryRunRequest&) =
        default;
    SystemAurUpdateDryRunRequest& operator=(
        const SystemAurUpdateDryRunRequest&) = default;
    SystemAurUpdateDryRunRequest(SystemAurUpdateDryRunRequest&&) noexcept =
        default;
    SystemAurUpdateDryRunRequest& operator=(
        SystemAurUpdateDryRunRequest&&) noexcept = default;
    ~SystemAurUpdateDryRunRequest() noexcept = default;

    SystemAurUpdateDryRunMode mode() const noexcept {
        return mode_;
    }
    const std::vector<std::string>& ordered_pacman_args() const noexcept {
        return ordered_pacman_args_;
    }
    bool repository_needed() const noexcept {
        return repository_needed_;
    }

private:
    SystemAurUpdateDryRunRequest(
        SystemAurUpdateDryRunMode mode,
        std::vector<std::string> ordered_pacman_args,
        bool repository_needed)
        : mode_(mode),
          ordered_pacman_args_(std::move(ordered_pacman_args)),
          repository_needed_(repository_needed) {
    }

    SystemAurUpdateDryRunMode mode_;
    std::vector<std::string> ordered_pacman_args_;
    bool repository_needed_ = false;
};

enum class SystemAurUpdateDryRunIssueKind {
    RepositoryConfigurationFailure,
    ForeignInventoryFailure,
    AurQueryFailure,
    AurAssessmentFailure,
    InconsistentObservation,
};

struct SystemAurUpdateDryRunIssue {
    SystemAurUpdateDryRunIssueKind kind =
        SystemAurUpdateDryRunIssueKind::InconsistentObservation;
    std::optional<PackageMetadataFailure> package_metadata_failure;
    std::string diagnostic;
};

// Current-state read-only observation for the combined -Syu route.
// It owns no Prepared* capability and has no conversion to the actual
// coordinator. Auto retains exact current authority; RepoOnly retains none of
// the AUR fields.
struct SystemAurUpdateDryRunObservation {
    explicit SystemAurUpdateDryRunObservation(
        SystemAurUpdateDryRunRequest request_value)
        : request(std::move(request_value)) {
    }

    SystemAurUpdateDryRunObservation(
        SystemAurUpdateDryRunRequest request_value,
        std::optional<SystemAurUpdateDryRunAurObservationBasis>
            observation_basis_value,
        std::optional<SystemAurUpdateDryRunActualAuthorityRefresh>
            authority_refresh_value,
        std::optional<NoExplicitSourceSatisfaction>
            explicit_source_satisfaction_value,
        std::optional<SavedSourcePreferencePolicy>
            saved_source_preference_policy_value,
        std::optional<DevelRequiresCheckPolicy>
            devel_requires_check_policy_value,
        std::optional<PacmanRepositoryConfiguration>
            repository_configuration_value,
        ForeignPackageInventory foreign_inventory_value,
        std::optional<FilteredAurUpdateObservation>
            aur_observation_value,
        std::vector<SystemAurUpdateDryRunIssue> issues_value)
        : request(std::move(request_value)),
          aur_observation_basis(observation_basis_value),
          actual_authority_refresh(authority_refresh_value),
          explicit_source_satisfaction(
              std::move(explicit_source_satisfaction_value)),
          saved_source_preference_policy(
              saved_source_preference_policy_value),
          devel_requires_check_policy(
              devel_requires_check_policy_value),
          repository_configuration(
              std::move(repository_configuration_value)),
          foreign_inventory(std::move(foreign_inventory_value)),
          aur_observation(std::move(aur_observation_value)),
          issues(std::move(issues_value)) {
    }

    SystemAurUpdateDryRunRequest request;
    std::optional<SystemAurUpdateDryRunAurObservationBasis>
        aur_observation_basis;
    std::optional<SystemAurUpdateDryRunActualAuthorityRefresh>
        actual_authority_refresh;
    std::optional<NoExplicitSourceSatisfaction>
        explicit_source_satisfaction;
    std::optional<SavedSourcePreferencePolicy>
        saved_source_preference_policy;
    std::optional<DevelRequiresCheckPolicy>
        devel_requires_check_policy;
    std::optional<PacmanRepositoryConfiguration>
        repository_configuration;
    ForeignPackageInventory foreign_inventory;
    std::optional<FilteredAurUpdateObservation> aur_observation;
    std::vector<SystemAurUpdateDryRunIssue> issues;

    bool is_ready() const noexcept;
    bool is_blocked() const noexcept;
    bool has_current_aur_update_intent() const noexcept;
};

SystemAurUpdateDryRunObservation observe_system_aur_update_dry_run(
    SystemAurUpdateDryRunRequest request,
    const AppConfig& config);

// Slice 1のclassifierがcompatibleと確定したexact repository request。
// constructorを閉じ、raw argvやlegacy modifier heuristicからの再構築を許さない。
class CompatibleSystemAurUpdateRequest final {
    explicit CompatibleSystemAurUpdateRequest(
        std::vector<std::string> ordered_pacman_args)
        : ordered_pacman_args_(std::move(ordered_pacman_args)) {
    }

    std::vector<std::string> ordered_pacman_args_;

    friend std::optional<CompatibleSystemAurUpdateRequest>
    make_compatible_system_aur_update_request(
        AutoSystemUpdateRouteCandidate candidate);

public:
    CompatibleSystemAurUpdateRequest(
        const CompatibleSystemAurUpdateRequest&) = default;
    CompatibleSystemAurUpdateRequest& operator=(
        const CompatibleSystemAurUpdateRequest&) = default;
    CompatibleSystemAurUpdateRequest(
        CompatibleSystemAurUpdateRequest&&) noexcept = default;
    CompatibleSystemAurUpdateRequest& operator=(
        CompatibleSystemAurUpdateRequest&&) noexcept = default;
    ~CompatibleSystemAurUpdateRequest() noexcept = default;

    const std::vector<std::string>& ordered_pacman_args() const noexcept {
        return ordered_pacman_args_;
    }
};

// Incompatible candidateはpublic validationのownerへ返し、
// coordinatorへ渡さない。filesystem / network / processには到達しない。
std::optional<CompatibleSystemAurUpdateRequest>
make_compatible_system_aur_update_request(
    AutoSystemUpdateRouteCandidate candidate);

enum class SystemAurUpdateOperationPhase {
    None,
    Repository,
    ForeignInventory,
    AurQuery,
    AurPreparation,
    AurExecution,
    Reduction,
};

enum class SystemAurUpdateOperationStatus {
    Completed,
    StoppedOnRepositoryFailure,
    StoppedBeforeAurExecution,
    StoppedOnAurFailure,
    StoppedAfterAurCleanupFailure,
    InconsistentResult,
    StoppedOnAurCancellation,
};

enum class SystemAurUpdateRepositoryPhaseStatus {
    NotAttempted,
    Completed,
    Failed,
};

enum class SystemAurUpdateForeignInventoryPhaseStatus {
    NotAttempted,
    Completed,
    Failed,
};

enum class SystemAurUpdateQueryPhaseStatus {
    NotAttempted,
    Completed,
    Failed,
};

enum class SystemAurUpdateAurPhaseStatus {
    NotAttempted,
    NoUpdates,
    Completed,
    BlockedBeforeExecution,
    StoppedOnProviderTransactionFailure,
    StoppedOnWorkItemFailure,
    StoppedAfterCleanupFailure,
    InconsistentResult,
    StoppedOnWorkItemCancellation,
};

enum class SystemAurUpdateNotAttemptedReason {
    RepositoryFailure,
    ForeignInventoryFailure,
    AurQueryFailure,
    PriorAggregateInconsistency,
};

struct SystemAurUpdateRepositoryPhaseResult {
    SystemAurUpdateRepositoryPhaseStatus status =
        SystemAurUpdateRepositoryPhaseStatus::NotAttempted;
    std::optional<SystemAurUpdateNotAttemptedReason>
        not_attempted_reason;
    std::optional<CompatibleSystemAurUpdateRequest>
        compatible_request;
    std::vector<std::string> ordered_pacman_args;
    std::optional<int> command_exit_status;
    std::optional<std::string> diagnostic;
};

struct SystemAurUpdateForeignInventoryPhaseResult {
    SystemAurUpdateForeignInventoryPhaseStatus status =
        SystemAurUpdateForeignInventoryPhaseStatus::NotAttempted;
    std::optional<SystemAurUpdateNotAttemptedReason>
        not_attempted_reason;
    std::optional<PacmanRepositoryConfiguration>
        repository_configuration;
    ForeignPackageInventory inventory;
    std::optional<PackageMetadataFailure> failure;
    std::optional<std::string> diagnostic;
};

struct SystemAurUpdateQueryPhaseResult {
    SystemAurUpdateQueryPhaseStatus status =
        SystemAurUpdateQueryPhaseStatus::NotAttempted;
    std::optional<SystemAurUpdateNotAttemptedReason>
        not_attempted_reason;
    std::optional<AurUpdateQueryResult> query_result;
    std::optional<std::string> diagnostic;
};

struct SystemAurUpdateAurPhaseResult {
    SystemAurUpdateAurPhaseStatus status =
        SystemAurUpdateAurPhaseStatus::NotAttempted;
    std::optional<SystemAurUpdateNotAttemptedReason>
        not_attempted_reason;
    std::optional<FilteredAurUpdateExecutionResult> operation_result;
    std::optional<std::string> diagnostic;
};

struct SystemAurUpdateOperationResult {
    SystemAurUpdateOperationStatus status =
        SystemAurUpdateOperationStatus::InconsistentResult;
    SystemAurUpdateOperationPhase stopped_phase =
        SystemAurUpdateOperationPhase::Reduction;
    SystemAurUpdateRepositoryPhaseResult repository;
    SystemAurUpdateForeignInventoryPhaseResult foreign_inventory;
    SystemAurUpdateQueryPhaseResult query;
    SystemAurUpdateAurPhaseResult aur;

    bool is_success() const noexcept;
    PackageStateChange package_state_change() const noexcept;
    bool has_partial_completion() const noexcept;
    bool has_not_attempted_phase() const noexcept;
    bool has_cleanup_failure() const noexcept;
    bool has_query_failure() const noexcept;
    bool has_inconsistency() const noexcept;
};

class PreparedSystemAurUpdateOperation final {
    explicit PreparedSystemAurUpdateOperation(
        CompatibleSystemAurUpdateRequest request)
        : request_(std::move(request)) {
    }

    bool valid_ = true;
    CompatibleSystemAurUpdateRequest request_;

    friend PreparedSystemAurUpdateOperation
    prepare_system_aur_update_operation(
        CompatibleSystemAurUpdateRequest request);
    friend SystemAurUpdateOperationResult
    execute_prepared_system_aur_update_operation(
        PreparedSystemAurUpdateOperation prepared,
        const AppConfig& config);

public:
    PreparedSystemAurUpdateOperation(
        const PreparedSystemAurUpdateOperation&) = delete;
    PreparedSystemAurUpdateOperation& operator=(
        const PreparedSystemAurUpdateOperation&) = delete;
    PreparedSystemAurUpdateOperation(
        PreparedSystemAurUpdateOperation&& other) noexcept;
    PreparedSystemAurUpdateOperation& operator=(
        PreparedSystemAurUpdateOperation&&) = delete;
    ~PreparedSystemAurUpdateOperation() noexcept = default;

    bool is_valid() const noexcept {
        return valid_;
    }
    const CompatibleSystemAurUpdateRequest& repository_request()
        const noexcept {
        return request_;
    }
};

// Outer preparation intentionally retains only the canonical repository
// request. AUR authority cannot be prepared until repository success.
PreparedSystemAurUpdateOperation prepare_system_aur_update_operation(
    CompatibleSystemAurUpdateRequest request);

// Child phase evidenceを文字列やexit codeから再推定せずaggregateへ畳む。
// Testとpublic presentationは同じfail-closed reducerを共有する。
SystemAurUpdateOperationResult reduce_system_aur_update_result(
    SystemAurUpdateOperationResult result) noexcept;

SystemAurUpdateOperationResult
execute_prepared_system_aur_update_operation(
    PreparedSystemAurUpdateOperation prepared,
    const AppConfig& config);
