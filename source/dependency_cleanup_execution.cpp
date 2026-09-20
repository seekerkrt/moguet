#include "dependency_cleanup_execution.hpp"

#include "invocation_owned_cleanup_adapter.hpp"
#include "package_identifier.hpp"
#include "process.hpp"
#include "shell_words.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include <fnmatch.h>

namespace {
using Status = DependencyCleanupCandidateRevalidationStatus;

bool valid_approval_candidate(const DependencyCleanupCandidateSnapshot& candidate) {
    // Reuse the classifier's identity/role/provider/correlation checks. These
    // are frozen invocation facts, not fresh system safety evidence.
    return candidate.classification() == CleanupClassification::Eligible &&
           classify_invocation_owned_cleanup(InvocationOwnedCleanupCandidate{
                                                 candidate.package(), CleanupBaselineObservation::NewlyObserved, {CleanupInstalledState::Present, candidate.expected_installed(), CleanupEvidenceVerification::Verified}, CleanupCausalOwnership::Unknown, candidate.shared_requirement(), candidate.policy_protection(), CleanupCorrelationCoverage::Complete, candidate.correlations()})
                   .classification() == CleanupClassification::Eligible;
}

void revalidate_candidate(DependencyCleanupCandidateRevalidation& result,
                          const InstalledPackageStateSnapshotResult& snapshot,
                          const PackageMetadataSession& session,
                          const PacmanRepositoryConfiguration& configuration,
                          const ConfiguredHoldPackagePatterns& hold_packages) {
    const auto& expected = result.approved.expected_installed();
    const auto current = project_cleanup_current_package_evidence(snapshot, expected.name);
    if(current.state == CleanupInstalledState::Absent) {
        result.status = Status::AlreadyAbsent;
        return;
    }
    result.current = current.metadata;
    if(current.verification != CleanupEvidenceVerification::Verified || !result.current) return;
    const auto& actual = *result.current;
    if(actual.name != expected.name || actual.version != expected.version ||
       actual.package_base != expected.package_base || actual.architecture != expected.architecture) {
        result.status = Status::IdentityChanged;
        return;
    }
    if(actual.reason != InstalledPackageReason::Dependency) {
        result.status = actual.reason == InstalledPackageReason::Explicit ? Status::InstallReasonChanged : Status::Unknown;
        return;
    }
    // Policy owns its own newly opened local/sync read phase. Bind that
    // observation back to the same exact name/version; never reuse preview policy.
    const auto policy = query_cleanup_policy_protection_evidence(configuration, expected.name);
    if(!policy.failures.empty()) result.failure = policy.failures.front();
    if(!policy.candidate || policy.candidate->package_name != actual.name || policy.candidate->version != actual.version) return;
    const auto protection = project_cleanup_policy_protection(policy);
    if(protection != CleanupPolicyProtection::NotProtected) {
        result.status = protection == CleanupPolicyProtection::Protected ? Status::Protected : Status::Unknown;
        return;
    }
    // Match pacman's remove.c fnmatch_cmp: flags 0 (no extended/brace or
    // inverted-list semantics). Cleanup Yes never overrides HoldPkg. Apply
    // protection before shared closure so retained held consumers protect deps.
    for(const auto& pattern : hold_packages.patterns) {
        const int match = fnmatch(pattern.c_str(), actual.name.c_str(), 0);
        if(match == 0) {
            result.status = Status::Protected;
            return;
        }
        if(match != FNM_NOMATCH) return;
    }
    const auto consumers = session.query_installed_runtime_consumers(expected.name);
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&consumers)) {
        result.failure = *failure;
        return;
    }
    result.runtime_consumers = std::get<std::vector<std::string>>(consumers);
    result.status = Status::Ready;
}
} // namespace

DependencyCleanupRevalidationState DependencyCleanupRevalidationResult::state() const noexcept {
    return state_;
}
const std::vector<DependencyCleanupCandidateRevalidation>& DependencyCleanupRevalidationResult::candidates() const noexcept {
    return candidates_;
}
const std::optional<PackageMetadataFailure>& DependencyCleanupRevalidationResult::failure() const noexcept {
    return failure_;
}

DependencyCleanupRevalidationResult revalidate_dependency_cleanup(const DependencyCleanupApprovalSnapshot& approval) {
    DependencyCleanupRevalidationResult result;
    const auto& preview = approval.preview();
    std::set<std::string> names;
    bool invalid = preview.state() != DependencyCleanupPreviewState::Ready || preview.eligible_candidates().empty();
    for(const auto& candidate : preview.eligible_candidates()) {
        const bool valid = valid_approval_candidate(candidate) && names.insert(candidate.expected_installed().name).second;
        result.candidates_.push_back({candidate, valid ? Status::Unknown : Status::Invalid, std::nullopt, {}, std::nullopt});
        invalid = invalid || !valid;
    }
    if(invalid) return result;
    try {
        const auto configuration = resolve_pacman_repository_configuration();
        // Both this query and the fixed removal use default effective pacman
        // configuration; no caller can inject custom config/root options.
        const auto hold_packages = query_configured_hold_package_patterns();
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&hold_packages)) {
            result.failure_ = *failure;
            return result;
        }
        auto session = PackageMetadataSession::open(configuration.database_paths);
        const auto snapshot = session.snapshot_installed_package_states();
        if(const auto* failure = std::get_if<PackageMetadataFailure>(&snapshot)) {
            result.failure_ = *failure;
            return result;
        }
        for(auto& candidate : result.candidates_) {
            revalidate_candidate(candidate, snapshot, session, configuration,
                                 std::get<ConfiguredHoldPackagePatterns>(hold_packages));
        }
        // A skipped consumer stays installed. Repeatedly remove its requirements
        // from Ready until the subset is closed. This only filters observed
        // relations; it neither selects providers nor solves an install/order.
        // Alternative providers are deliberately conservative: any matching
        // retained consumer protects the candidate even if another could satisfy it.
        bool changed;
        do {
            changed = false;
            std::set<std::string> ready;
            for(const auto& candidate : result.candidates_) {
                if(candidate.status == Status::Ready) ready.insert(candidate.approved.expected_installed().name);
            }
            for(auto& candidate : result.candidates_) {
                if(candidate.status == Status::Ready && std::any_of(candidate.runtime_consumers.begin(), candidate.runtime_consumers.end(),
                                                                    [&](const auto& consumer) { return !ready.contains(consumer); })) {
                    candidate.status = Status::StillRequired;
                    changed = true;
                }
            }
        } while(changed);
        result.state_ = std::any_of(result.candidates_.begin(), result.candidates_.end(),
                                    [](const auto& candidate) { return candidate.status == Status::Ready; })
                            ? DependencyCleanupRevalidationState::Ready
                            : DependencyCleanupRevalidationState::NoCandidatesReady;
    } catch(const PackageMetadataError& error) {
        result.failure_ = error.failure();
        // No partially collected Ready fact can survive an invocation-wide failure.
        for(auto& candidate : result.candidates_) {
            if(candidate.status == Status::Ready) candidate.status = Status::Unknown;
        }
    }
    return result;
}

namespace {
DependencyCleanupExecutionResult remove_revalidated_candidates(DependencyCleanupRevalidationResult revalidation) {
    DependencyCleanupExecutionResult result;
    result.revalidation = std::move(revalidation);
    if(result.revalidation->state() == DependencyCleanupRevalidationState::Blocked) return result;
    if(result.revalidation->state() == DependencyCleanupRevalidationState::NoCandidatesReady) {
        result.status = DependencyCleanupExecutionStatus::NoCandidatesReady;
        return result;
    }
    // The cleanup prompt owns approval. This internal option only suppresses
    // pacman's duplicate confirmation after explicit Yes and fresh revalidation;
    // it does not derive approval from the user's no_confirm configuration.
    std::vector<std::string> arguments{"sudo", "pacman", "-R", "--noconfirm", "--"};
    for(const auto& candidate : result.revalidation->candidates()) {
        if(candidate.status != Status::Ready) continue;
        const auto& name = candidate.approved.package().package().package_name();
        if(!is_valid_package_name(name) || !candidate.current ||
           candidate.current->name != name || *candidate.current != candidate.approved.expected_installed()) {
            return result;
        }
        arguments.push_back(name);
    }
    if(arguments.size() == 5) {
        result.status = DependencyCleanupExecutionStatus::NoCandidatesReady;
        return result;
    }
    const auto command = shell_words::join(arguments);
    for(const auto& candidate : result.revalidation->candidates()) {
        if(candidate.status == Status::Ready) result.attempted.push_back(candidate.approved);
    }
    // Mark failure before crossing the process boundary. A thrown wait/launch
    // error cannot justify retry, fallback, rollback, or a removed-state claim.
    result.status = DependencyCleanupExecutionStatus::RemovalFailed;
    try {
        result.removal_exit_status = run_command(command);
        if(*result.removal_exit_status == 0) result.status = DependencyCleanupExecutionStatus::Removed;
    } catch(const std::exception&) {
        // No known exit status. Preserve the attempted set and all skip reasons.
    }
    return result;
}
} // namespace

DependencyCleanupExecutionResult execute_dependency_cleanup(const DependencyCleanupInteractionResult& interaction) {
    if(interaction.status() != DependencyCleanupInteractionStatus::Approved || !interaction.approved_snapshot()) return {};
    // Do not expose the destructive helper: retaining an earlier read-only
    // revalidation result must never bypass a new mutation-time observation.
    return remove_revalidated_candidates(revalidate_dependency_cleanup(*interaction.approved_snapshot()));
}
