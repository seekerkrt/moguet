#pragma once

#include <filesystem>
#include <optional>
#include <string>

enum class DiagnosticClass {
    Invalid,
    Unsupported,
    Ambiguous,
    Declined,
    Cancelled,
    Unavailable,
    InputFailure,
    QueryFailure,
    MetadataFailure,
    RequiresCheck,
    Blocked,
    PartialFailure,
    ExecutionFailure,
    InternalInconsistency,
};

enum class DiagnosticSeverity {
    Info,
    Warning,
    Error,
};

enum class DiagnosticOperation {
    CliParsing,
    Build,
    Upgrade,
    UpgradeAur,
    UpgradeAll,
    Clean,
    Deps,
    Plan,
    Fetch,
    AddSource,
    DeleteSource,
    Revert,
    EditSource,
    ListSources,
    PkgbuildExport,
    PkgbuildPrint,
    SyncSearch,
    SyncInfo,
    SyncInstall,
    RootPackageSelection,
    PacmanDelegation,
    PatchCustomization,
};

enum class DiagnosticPhase {
    Parsing,
    Selection,
    Query,
    Metadata,
    Planning,
    Preflight,
    Fetch,
    Build,
    Install,
    Cleanup,
    Reduction,
    Observation,
};

enum class DiagnosticSourceKind {
    Unspecified,
    RepositoryBinary,
    RepositorySource,
    Aur,
    Local,
    Pacman,
};

enum class DiagnosticRequiredAction {
    None,
    CorrectInput,
    SelectCandidate,
    EnableInteraction,
    RetryQuery,
    InspectMetadata,
    ConfirmEvaluation,
    ResolveBlocker,
    InspectPartialResult,
    ReportInconsistency,
};

enum class DiagnosticBlockingDecision {
    NonBlocking,
    BlocksCurrentOperation,
    StopsFollowingPhases,
};

enum class DiagnosticExitStatusEffect {
    None,
    SuccessPermitted,
    Failure,
};

struct DiagnosticIdentity {
    DiagnosticSourceKind source_kind =
        DiagnosticSourceKind::Unspecified;
    std::optional<std::string> repository;
    std::optional<std::string> requested_package;
    std::optional<std::string> package_base;
    std::optional<std::string> canonical_source_identity;
    std::optional<std::filesystem::path> local_root;

    bool operator==(const DiagnosticIdentity&) const = default;
};

// Reason remains a route-owned enum/value. Normalization shares the dimensions
// needed by presentation and control-flow review without replacing those
// route-specific reasons with one universal reason enum.
template <typename Reason>
struct NormalizedDiagnostic {
    DiagnosticClass classification;
    DiagnosticSeverity severity;
    DiagnosticOperation operation;
    DiagnosticPhase phase;
    DiagnosticIdentity identity;
    Reason reason;
    DiagnosticRequiredAction required_action;
    DiagnosticBlockingDecision blocking_decision;
    DiagnosticExitStatusEffect exit_status_effect;
    std::optional<std::string> supplemental_detail;

    bool operator==(const NormalizedDiagnostic&) const = default;
};
