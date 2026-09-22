#include "dependency_cleanup_interaction.hpp"

#include "app_config.hpp"
#include "invocation_owned_cleanup_adapter.hpp"
#include "localization.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

#include <unistd.h>

DependencyCleanupCandidateSnapshot::DependencyCleanupCandidateSnapshot(
    const InvocationOwnedCleanupCandidate& candidate)
    : package_(candidate.package),
      expected_installed_(candidate.current_package.metadata.value()),
      correlations_(candidate.correlations),
      classification_(CleanupClassification::Eligible),
      shared_requirement_(candidate.shared_requirement),
      policy_protection_(candidate.policy_protection) {
}

const SourceAwarePackageIdentity& DependencyCleanupCandidateSnapshot::package() const noexcept {
    return package_;
}
const InstalledPackageMetadata& DependencyCleanupCandidateSnapshot::expected_installed() const noexcept {
    return expected_installed_;
}
const std::vector<CleanupPackageCorrelation>& DependencyCleanupCandidateSnapshot::correlations() const noexcept {
    return correlations_;
}
CleanupClassification DependencyCleanupCandidateSnapshot::classification() const noexcept {
    return classification_;
}
CleanupSharedRequirementState DependencyCleanupCandidateSnapshot::shared_requirement() const noexcept {
    return shared_requirement_;
}
CleanupPolicyProtection DependencyCleanupCandidateSnapshot::policy_protection() const noexcept {
    return policy_protection_;
}

DependencyCleanupPreview::DependencyCleanupPreview(
    DependencyCleanupPreviewState state, std::vector<DependencyCleanupCandidateSnapshot> candidates)
    : state_(state), eligible_candidates_(std::move(candidates)) {
}
DependencyCleanupPreviewState DependencyCleanupPreview::state() const noexcept {
    return state_;
}
const std::vector<DependencyCleanupCandidateSnapshot>& DependencyCleanupPreview::eligible_candidates() const noexcept {
    return eligible_candidates_;
}

DependencyCleanupPreview make_dependency_cleanup_preview(const RemoteAurCleanupCollectionResult& collection) {
    // Incomplete currently conflates universe/consumer/identity/policy gaps and
    // individual unsafe candidates. Do not reinterpret it as safe partial approval.
    const auto blocked = [] {
        return DependencyCleanupPreview(DependencyCleanupPreviewState::Blocked, {});
    };
    if(!collection.invocation_result().is_success() ||
       collection.completeness() != CleanupEvidenceCompleteness::Complete ||
       !collection.issues().empty()) {
        return blocked();
    }
    std::vector<DependencyCleanupCandidateSnapshot> candidates;
    for(const auto& assessment : collection.assessments()) {
        switch(assessment.classification) {
            case CleanupClassification::Protected:
                continue;
            case CleanupClassification::Unknown:
            case CleanupClassification::Invalid:
                // Such assessments currently imply collection Incomplete.
                return blocked();
            case CleanupClassification::Eligible:
                break;
            default:
                return blocked();
        }
        if(!assessment.preview_snapshot.has_value() ||
           assessment.preview_snapshot->package() != assessment.package) {
            return blocked();
        }
        const auto& snapshot = assessment.preview_snapshot.value();
        // Collector order is first factual origin order: ordered work-item
        // receipts, then BuildPlan edges. It already merges all correlations
        // for a package. Reject duplicates rather than silently lose evidence.
        if(std::any_of(candidates.begin(), candidates.end(), [&](const auto& existing) {
               return existing.package().package().package_name() == snapshot.package().package().package_name();
           })) {
            return blocked();
        }
        candidates.push_back(snapshot);
    }
    const auto state = candidates.empty() ? DependencyCleanupPreviewState::NoCandidates
                                          : DependencyCleanupPreviewState::Ready;
    return DependencyCleanupPreview(state, std::move(candidates));
}

void render_dependency_cleanup_preview(const DependencyCleanupPreview& preview, std::ostream& output) {
    if(preview.state() != DependencyCleanupPreviewState::Ready) return;
    // NO_TRANSLATE: UI framing and exact package/version facts are not prose.
    output << ":: " << localization::translate_message("Assessed build dependencies eligible for cleanup:") << '\n';
    for(const auto& candidate : preview.eligible_candidates()) {
        output << "  " << candidate.expected_installed().name << ' '
               << candidate.expected_installed().version << '\n';
    }
}

DependencyCleanupApprovalSnapshot::DependencyCleanupApprovalSnapshot(DependencyCleanupPreview preview)
    : preview_(std::move(preview)) {
}
const DependencyCleanupPreview& DependencyCleanupApprovalSnapshot::preview() const noexcept {
    return preview_;
}

DependencyCleanupInteractionResult::DependencyCleanupInteractionResult(
    DependencyCleanupInteractionStatus status,
    std::optional<DependencyCleanupApprovalSnapshot> approved,
    std::optional<ConfirmationCancellationReason> cancelled,
    std::optional<DependencyCleanupUnavailableReason> unavailable)
    : status_(status), approved_snapshot_(std::move(approved)),
      cancellation_reason_(cancelled), unavailable_reason_(unavailable) {
}
DependencyCleanupInteractionStatus DependencyCleanupInteractionResult::status() const noexcept {
    return status_;
}
const std::optional<DependencyCleanupApprovalSnapshot>& DependencyCleanupInteractionResult::approved_snapshot() const noexcept {
    return approved_snapshot_;
}
const std::optional<ConfirmationCancellationReason>& DependencyCleanupInteractionResult::cancellation_reason() const noexcept {
    return cancellation_reason_;
}
const std::optional<DependencyCleanupUnavailableReason>& DependencyCleanupInteractionResult::unavailable_reason() const noexcept {
    return unavailable_reason_;
}

DependencyCleanupInteractionResult interact_dependency_cleanup(
    const DependencyCleanupPreview& preview, const AppConfig& config) {
    return interact_dependency_cleanup(preview, config, isatty(STDIN_FILENO) != 0, std::cin, std::cout);
}

DependencyCleanupInteractionResult interact_dependency_cleanup(
    const DependencyCleanupPreview& preview, const AppConfig& config,
    bool is_interactive_input, std::istream& input, std::ostream& output) {
    using Status = DependencyCleanupInteractionStatus;
    using Unavailable = DependencyCleanupUnavailableReason;
    // Freeze before rendering or reading user input. Even a caller replacing
    // its preview during interaction cannot change the set being approved.
    const DependencyCleanupPreview shown = preview;
    const auto unavailable = [](Unavailable reason) {
        return DependencyCleanupInteractionResult(Status::InteractionUnavailable, std::nullopt, std::nullopt, reason);
    };
    if(shown.state() == DependencyCleanupPreviewState::Blocked) {
        return DependencyCleanupInteractionResult(Status::Blocked);
    }
    if(shown.state() == DependencyCleanupPreviewState::NoCandidates) {
        return DependencyCleanupInteractionResult(Status::NoCandidates);
    }
    if(shown.eligible_candidates().empty()) {
        return DependencyCleanupInteractionResult(Status::Blocked);
    }
    // #486 requires unavailable, not the primitive's automatic safe-No result.
    // AppConfig remains the sole noconfirm authority.
    if(config.no_confirm) return unavailable(Unavailable::NoConfirm);
    if(!is_interactive_input) return unavailable(Unavailable::NonInteractiveInput);
    try {
        render_dependency_cleanup_preview(shown, output);
        output.flush();
        if(!output.good()) return unavailable(Unavailable::OutputFailure);
        const auto answer = request_confirmation(
            localization::translate_message("Remove build dependencies?"),
            ConfirmationDefault::No, config.no_confirm, is_interactive_input, input, output);
        output.flush();
        if(!output.good()) return unavailable(Unavailable::OutputFailure);
        if(const auto* accepted = std::get_if<ConfirmationAccepted>(&answer)) {
            if(accepted->origin == ConfirmationDecisionOrigin::ExplicitToken) {
                return DependencyCleanupInteractionResult(Status::Approved, DependencyCleanupApprovalSnapshot(shown));
            }
            return DependencyCleanupInteractionResult(Status::Blocked);
        }
        if(std::holds_alternative<ConfirmationDeclined>(answer)) {
            return DependencyCleanupInteractionResult(Status::Declined);
        }
        if(const auto* cancelled = std::get_if<ConfirmationCancelled>(&answer)) {
            return DependencyCleanupInteractionResult(Status::Cancelled, std::nullopt, cancelled->reason);
        }
        if(const auto* stopped = std::get_if<ConfirmationUnavailable>(&answer)) {
            return unavailable(stopped->reason == ConfirmationUnavailableReason::NoConfirm
                                   ? Unavailable::NoConfirm
                                   : Unavailable::NonInteractiveInput);
        }
        return unavailable(Unavailable::InputFailure);
    } catch(const std::ios_base::failure&) {
        if(output.good() && input.eof() && !input.bad()) {
            return DependencyCleanupInteractionResult(Status::Cancelled, std::nullopt, ConfirmationCancellationReason::EndOfInput);
        }
        return unavailable(!output.good() ? Unavailable::OutputFailure : Unavailable::InputFailure);
    }
}
