#include "invocation_owned_cleanup_model.hpp"

#include "package_identifier.hpp"
#include "source_install.hpp"
#include "trusted_alpm_receipt_protocol.hpp"

#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

struct CleanupInvocationSessionState {
    explicit CleanupInvocationSessionState(
        PreparedRemoteSourceBuild prepared_value) noexcept
        : prepared(std::move(prepared_value)) {
    }

    mutable std::mutex mutex;
    PreparedRemoteSourceBuild prepared;
    bool baseline_observed = false;
    std::size_t post_success_observation_count = 0;
    std::vector<CleanupTrustedTransactionTokenInventoryEntry>
        transaction_inventory;
};

namespace {

void add_receipt_issue(
    std::vector<PacmanTransactionReceiptIssueKind>& issues,
    PacmanTransactionReceiptIssueKind issue) {
    if(std::find(issues.begin(), issues.end(), issue) == issues.end()) {
        issues.push_back(issue);
    }
}

void canonicalize_receipt_issues(
    std::vector<PacmanTransactionReceiptIssueKind>& issues) {
    std::sort(issues.begin(), issues.end(), [](auto lhs, auto rhs) {
        return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
}

bool is_valid_transaction_owner(
    InvocationDependencyTransactionOwner owner) noexcept {
    switch(owner) {
        case InvocationDependencyTransactionOwner::SelectedRepositoryProvider:
        case InvocationDependencyTransactionOwner::SourceArtifactInstall:
        case InvocationDependencyTransactionOwner::MakepkgSyncDependencies:
            return true;
        case InvocationDependencyTransactionOwner::Unknown:
            return false;
    }
    return false;
}

bool is_valid_receipt_observation_state(
    PacmanTransactionReceiptObservationState state) noexcept {
    switch(state) {
        case PacmanTransactionReceiptObservationState::Missing:
        case PacmanTransactionReceiptObservationState::Incomplete:
        case PacmanTransactionReceiptObservationState::Complete:
            return true;
    }
    return false;
}

bool is_supported_install_transaction_operation(
    PacmanTransactionPackageOperation operation) noexcept {
    switch(operation) {
        case PacmanTransactionPackageOperation::Install:
        case PacmanTransactionPackageOperation::Upgrade:
            return true;
        case PacmanTransactionPackageOperation::Remove:
        case PacmanTransactionPackageOperation::Unknown:
            return false;
    }
    return false;
}

void add_reason(
    std::vector<CleanupClassificationReason>& reasons,
    CleanupClassificationReason reason) {
    if(std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
        reasons.push_back(reason);
    }
}

void canonicalize_reasons(
    std::vector<CleanupClassificationReason>& reasons) {
    std::sort(reasons.begin(), reasons.end(), [](auto lhs, auto rhs) {
        return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
}

bool is_valid_baseline(CleanupBaselineObservation baseline) noexcept {
    switch(baseline) {
        case CleanupBaselineObservation::PreExisting:
        case CleanupBaselineObservation::NewlyObserved:
        case CleanupBaselineObservation::Unknown:
            return true;
    }
    return false;
}

bool is_valid_installed_metadata_value_state(
    InstalledPackageMetadataValueState state) noexcept {
    switch(state) {
        case InstalledPackageMetadataValueState::Known:
        case InstalledPackageMetadataValueState::Missing:
        case InstalledPackageMetadataValueState::Malformed:
        case InstalledPackageMetadataValueState::Unavailable:
        case InstalledPackageMetadataValueState::Unknown:
            return true;
    }
    return false;
}

bool is_valid_installed_state(CleanupInstalledState state) noexcept {
    switch(state) {
        case CleanupInstalledState::Present:
        case CleanupInstalledState::Absent:
        case CleanupInstalledState::Unknown:
            return true;
    }
    return false;
}

bool is_valid_install_reason(InstalledPackageReason reason) noexcept {
    switch(reason) {
        case InstalledPackageReason::Explicit:
        case InstalledPackageReason::Dependency:
        case InstalledPackageReason::Unknown:
            return true;
    }
    return false;
}

bool is_valid_causal_ownership(CleanupCausalOwnership ownership) noexcept {
    switch(ownership) {
        case CleanupCausalOwnership::InvocationOwned:
        case CleanupCausalOwnership::NotInvocationOwned:
        case CleanupCausalOwnership::Unknown:
            return true;
    }
    return false;
}

bool is_valid_shared_requirement(
    CleanupSharedRequirementState state) noexcept {
    switch(state) {
        case CleanupSharedRequirementState::StillRequired:
        case CleanupSharedRequirementState::NoLongerRequired:
        case CleanupSharedRequirementState::Unknown:
            return true;
    }
    return false;
}

bool is_valid_verification(CleanupEvidenceVerification verification) noexcept {
    switch(verification) {
        case CleanupEvidenceVerification::Verified:
        case CleanupEvidenceVerification::Unverified:
            return true;
    }
    return false;
}

bool is_valid_correlation_coverage(
    CleanupCorrelationCoverage coverage) noexcept {
    switch(coverage) {
        case CleanupCorrelationCoverage::Complete:
        case CleanupCorrelationCoverage::Incomplete:
        case CleanupCorrelationCoverage::Unknown:
            return true;
    }
    return false;
}

bool is_valid_policy_protection(CleanupPolicyProtection protection) noexcept {
    switch(protection) {
        case CleanupPolicyProtection::NotProtected:
        case CleanupPolicyProtection::Protected:
        case CleanupPolicyProtection::Unknown:
            return true;
    }
    return false;
}

bool is_valid_package_role(PackageRole role) noexcept {
    switch(role) {
        case PackageRole::Root:
        case PackageRole::RuntimeDependency:
        case PackageRole::BuildDependency:
        case PackageRole::CheckDependency:
            return true;
    }
    return false;
}

bool is_valid_provider_resolution(ProviderResolutionKind resolution) noexcept {
    switch(resolution) {
        case ProviderResolutionKind::Unique:
        case ProviderResolutionKind::UserSelected:
            return true;
    }
    return false;
}

bool provider_matches_package(
    const CleanupProviderCorrelation& correlation,
    const PackageChildIdentity& package) noexcept {
    const ProvidedDependency& provider = correlation.provider;
    if(provider.package_name != package.package_name()) return false;

    const PackageBaseIdentity& package_base = package.package_base();
    const PackageSourceIdentity& source = package_base.source();
    if(const auto* repository =
           std::get_if<RepositoryProviderOrigin>(&provider.origin);
       repository != nullptr) {
        if(source.kind() != PackageSourceKind::Repository) return false;
        const std::string* repository_name = source.repository_name();
        if(repository_name == nullptr ||
           *repository_name != repository->repository_name) {
            return false;
        }
        // Existing repository-provider identity may not retain PackageBase.
        // A nonempty value must agree; a verified correlation is the adapter's
        // authority for an omitted value and is never inferred from the name.
        return provider.package_base.empty() ||
               provider.package_base == package_base.package_base();
    }

    return source.kind() == PackageSourceKind::Aur &&
           provider.package_base == package_base.package_base();
}

bool direct_requirement_mismatches_package(
    const CleanupDependencyEdgeCorrelation& edge,
    const PackageChildIdentity& package) noexcept {
    if(edge.provider.has_value()) return false;
    const auto* requirement =
        std::get_if<ConsumerDependencyRequirement>(&edge.requirement);
    return requirement != nullptr &&
           requirement->package_name() != package.package_name();
}

bool provider_requirement_identity_mismatch(
    const CleanupDependencyEdgeCorrelation& edge) noexcept {
    if(!edge.provider.has_value()) return false;
    const auto* requirement =
        std::get_if<ConsumerDependencyRequirement>(&edge.requirement);
    if(requirement == nullptr) {
        // SONAME association remains adapter authority. The pure classifier
        // does not reinterpret its package:soname grammar.
        return false;
    }

    const ProvidedDependency& provider = edge.provider->provider;
    if(!provider.provided_dependency_name.empty() &&
       provider.provided_dependency_name != requirement->package_name()) {
        return true;
    }

    if(!provider.constraint_metadata.has_value()) return false;
    const ProviderCapability& capability =
        provider.constraint_metadata->provided_capability;
    return capability.package_name() != requirement->package_name() ||
           (!provider.provided_dependency_name.empty() &&
            provider.provided_dependency_name != capability.package_name()) ||
           (!provider.provided_dependency_specification.empty() &&
            provider.provided_dependency_specification !=
                capability.raw_specification());
}

std::vector<CleanupClassificationReason> structural_reasons(
    const InvocationOwnedCleanupCandidate& candidate) {
    std::vector<CleanupClassificationReason> reasons;

    if(!is_valid_baseline(candidate.baseline) ||
       !is_valid_installed_state(candidate.current_package.state) ||
       !is_valid_verification(candidate.current_package.verification) ||
       !is_valid_causal_ownership(candidate.causal_ownership) ||
       !is_valid_shared_requirement(candidate.shared_requirement) ||
       !is_valid_correlation_coverage(candidate.correlation_coverage) ||
       !is_valid_policy_protection(candidate.policy_protection)) {
        add_reason(reasons, CleanupClassificationReason::InvalidTypedState);
    }

    if(candidate.current_package.metadata.has_value()) {
        const InstalledPackageMetadata& metadata =
            candidate.current_package.metadata.value();
        if(!is_valid_install_reason(metadata.reason)) {
            add_reason(
                reasons, CleanupClassificationReason::InvalidTypedState);
        }
        if(!is_valid_installed_metadata_value_state(
               metadata.package_base.state()) ||
           !is_valid_installed_metadata_value_state(
               metadata.architecture.state())) {
            add_reason(
                reasons, CleanupClassificationReason::InvalidTypedState);
        }
        if(metadata.name != candidate.package.package().package_name()) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    CurrentPackageIdentityMismatch);
        }
        if(metadata.package_base.state() ==
           InstalledPackageMetadataValueState::Known) {
            const std::string* package_base = metadata.package_base.value();
            if(package_base == nullptr ||
               !is_valid_package_name(*package_base)) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::InvalidTypedState);
            } else if(*package_base !=
                      candidate.package.package()
                          .package_base()
                          .package_base()) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::
                        CurrentPackageIdentityMismatch);
            }
        }
        if(metadata.architecture.state() ==
           InstalledPackageMetadataValueState::Known) {
            const std::string* architecture =
                metadata.architecture.value();
            const bool valid_architecture =
                architecture != nullptr && !architecture->empty() &&
                std::all_of(
                    architecture->begin(), architecture->end(),
                    [](unsigned char character) {
                        return character > 0x20 && character != 0x7f;
                    });
            if(!valid_architecture) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::InvalidTypedState);
            } else if(candidate.package.architecture().state() ==
                          PackageArchitectureState::Known &&
                      (candidate.package.architecture().architectures().size() != 1 ||
                       candidate.package.architecture().architectures().front() != *architecture)) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::
                        CurrentPackageIdentityMismatch);
            }
        }
        const PackageVersionIdentity& package_version =
            candidate.package.package_version();
        if(candidate.current_package.state == CleanupInstalledState::Present &&
           package_version.state() == PackageVersionState::Known) {
            const std::string* full_version = package_version.full_version();
            if(full_version == nullptr) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::InvalidTypedState);
            } else if(*full_version != metadata.version) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::
                        CurrentPackageVersionMismatch);
            }
        }
    }

    for(const CleanupPackageCorrelation& correlation :
        candidate.correlations) {
        if(!is_valid_package_name(correlation.requested_root.requested_name)) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    MalformedRequestedRootIdentity);
        }
        if(correlation.package != candidate.package.package()) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    CorrelationPackageIdentityMismatch);
        }
        if(!is_valid_verification(correlation.verification)) {
            add_reason(
                reasons, CleanupClassificationReason::InvalidTypedState);
        }
        if(correlation.role.has_value()) {
            if(!is_valid_package_role(correlation.role.value())) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::InvalidTypedState);
            } else if(correlation.role.value() == PackageRole::Root &&
                      correlation.dependency_edge.has_value()) {
                add_reason(
                    reasons,
                    CleanupClassificationReason::
                        CorrelationShapeMismatch);
            }
        }

        if(!correlation.dependency_edge.has_value()) continue;
        const CleanupDependencyEdgeCorrelation& edge =
            correlation.dependency_edge.value();
        if(direct_requirement_mismatches_package(
               edge, correlation.package)) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    DependencyRequirementIdentityMismatch);
        }
        if(!edge.provider.has_value()) continue;
        const CleanupProviderCorrelation& provider = edge.provider.value();
        if(!is_valid_provider_resolution(provider.resolution)) {
            add_reason(
                reasons, CleanupClassificationReason::InvalidTypedState);
        }
        if(!provider_matches_package(provider, correlation.package)) {
            add_reason(
                reasons,
                CleanupClassificationReason::ProviderIdentityMismatch);
        }
        if(provider_requirement_identity_mismatch(edge)) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    ProviderRequirementIdentityMismatch);
        }
    }

    canonicalize_reasons(reasons);
    return reasons;
}

InstalledPackageReason current_install_reason(
    const InvocationOwnedCleanupCandidate& candidate) noexcept {
    return candidate.current_package.metadata.has_value()
               ? candidate.current_package.metadata->reason
               : InstalledPackageReason::Unknown;
}

std::vector<CleanupClassificationReason> protection_reasons(
    const InvocationOwnedCleanupCandidate& candidate) {
    std::vector<CleanupClassificationReason> reasons;
    if(candidate.baseline == CleanupBaselineObservation::PreExisting) {
        add_reason(reasons, CleanupClassificationReason::PreExisting);
    }
    if(candidate.current_package.state == CleanupInstalledState::Absent) {
        add_reason(
            reasons, CleanupClassificationReason::CurrentPackageAbsent);
    }
    if(current_install_reason(candidate) == InstalledPackageReason::Explicit) {
        add_reason(
            reasons,
            CleanupClassificationReason::ExplicitInstallReason);
    }
    if(candidate.causal_ownership ==
       CleanupCausalOwnership::NotInvocationOwned) {
        add_reason(
            reasons,
            CleanupClassificationReason::KnownNotInvocationOwned);
    }

    for(const CleanupPackageCorrelation& correlation :
        candidate.correlations) {
        if(!correlation.role.has_value()) continue;
        if(correlation.role.value() == PackageRole::Root) {
            add_reason(reasons, CleanupClassificationReason::RootTarget);
        } else if(correlation.role.value() ==
                  PackageRole::RuntimeDependency) {
            add_reason(
                reasons,
                CleanupClassificationReason::RuntimeDependency);
        }
    }

    if(candidate.shared_requirement ==
       CleanupSharedRequirementState::StillRequired) {
        add_reason(reasons, CleanupClassificationReason::StillRequired);
    }
    if(candidate.policy_protection == CleanupPolicyProtection::Protected) {
        add_reason(reasons, CleanupClassificationReason::PolicyProtected);
    }

    canonicalize_reasons(reasons);
    return reasons;
}

std::vector<CleanupClassificationReason> unknown_reasons(
    const InvocationOwnedCleanupCandidate& candidate) {
    std::vector<CleanupClassificationReason> reasons;
    if(candidate.baseline == CleanupBaselineObservation::Unknown) {
        add_reason(
            reasons,
            CleanupClassificationReason::BaselineObservationUnknown);
    }
    if(candidate.current_package.state == CleanupInstalledState::Unknown) {
        add_reason(
            reasons,
            CleanupClassificationReason::CurrentInstalledStateUnknown);
    }
    if(current_install_reason(candidate) == InstalledPackageReason::Unknown) {
        add_reason(
            reasons, CleanupClassificationReason::InstallReasonUnknown);
    }
    switch(candidate.package.package_version().state()) {
        case PackageVersionState::Known:
            break;
        case PackageVersionState::Unknown:
            add_reason(
                reasons,
                CleanupClassificationReason::CurrentPackageVersionUnknown);
            break;
        case PackageVersionState::Unavailable:
            add_reason(
                reasons,
                CleanupClassificationReason::
                    CurrentPackageVersionUnavailable);
            break;
    }
    if(candidate.current_package.state == CleanupInstalledState::Present) {
        if(!candidate.current_package.metadata.has_value() ||
           candidate.current_package.metadata->package_base.state() !=
               InstalledPackageMetadataValueState::Known) {
            add_reason(
                reasons,
                CleanupClassificationReason::CurrentPackageBaseUnknown);
        }
        if(!candidate.current_package.metadata.has_value() ||
           candidate.current_package.metadata->architecture.state() !=
               InstalledPackageMetadataValueState::Known ||
           candidate.package.architecture().state() !=
               PackageArchitectureState::Known ||
           candidate.package.architecture().architectures().size() != 1) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    CurrentPackageArchitectureUnknown);
        }
    }
    // POLICY(#486): missing strict causal proof alone does not block cleanup.
    // Affirmative NotInvocationOwned remains a protection reason; all other
    // evidence requirements below remain independent of causal ownership.
    if(candidate.current_package.verification ==
       CleanupEvidenceVerification::Unverified) {
        add_reason(
            reasons,
            CleanupClassificationReason::
                CurrentPackageEvidenceUnverified);
    }
    if(candidate.correlation_coverage ==
       CleanupCorrelationCoverage::Incomplete) {
        add_reason(
            reasons,
            CleanupClassificationReason::CorrelationCoverageIncomplete);
    } else if(candidate.correlation_coverage ==
              CleanupCorrelationCoverage::Unknown) {
        add_reason(
            reasons,
            CleanupClassificationReason::CorrelationCoverageUnknown);
    }
    if(candidate.correlations.empty()) {
        add_reason(
            reasons,
            CleanupClassificationReason::DependencyCorrelationMissing);
    }

    bool has_meaningful_build_or_check_correlation = false;
    for(const CleanupPackageCorrelation& correlation :
        candidate.correlations) {
        if(!correlation.role.has_value()) {
            add_reason(
                reasons,
                CleanupClassificationReason::DependencyRoleUnknown);
        } else if(correlation.role.value() == PackageRole::BuildDependency ||
                  correlation.role.value() == PackageRole::CheckDependency) {
            if(correlation.dependency_edge.has_value()) {
                has_meaningful_build_or_check_correlation = true;
            } else {
                add_reason(
                    reasons,
                    CleanupClassificationReason::
                        DependencyEdgeCorrelationMissing);
            }
        }

        if(correlation.verification ==
           CleanupEvidenceVerification::Unverified) {
            add_reason(
                reasons,
                CleanupClassificationReason::
                    DependencyCorrelationUnverified);
        }
    }
    if(!has_meaningful_build_or_check_correlation) {
        add_reason(
            reasons,
            CleanupClassificationReason::
                BuildOrCheckDependencyCorrelationMissing);
    }

    if(candidate.shared_requirement ==
       CleanupSharedRequirementState::Unknown) {
        add_reason(
            reasons,
            CleanupClassificationReason::SharedRequirementUnknown);
    }
    if(candidate.policy_protection == CleanupPolicyProtection::Unknown) {
        add_reason(
            reasons,
            CleanupClassificationReason::PolicyProtectionUnknown);
    }

    canonicalize_reasons(reasons);
    return reasons;
}

} // namespace

CleanupInvocationAuthority::CleanupInvocationAuthority(
    std::shared_ptr<CleanupInvocationSessionState> state) noexcept
    : state_(std::move(state)) {
}

bool CleanupInvocationAuthority::register_trusted_transaction_token(
    InvocationDependencyTransactionOwner owner,
    const std::string& transaction_token,
    std::vector<std::size_t> work_item_indices) const {
    if(!is_active() ||
       (owner != InvocationDependencyTransactionOwner::
                     SelectedRepositoryProvider &&
        owner != InvocationDependencyTransactionOwner::
                     SourceArtifactInstall) ||
       !is_valid_pacman_transaction_token(transaction_token) ||
       work_item_indices.empty()) {
        return false;
    }
    std::sort(work_item_indices.begin(), work_item_indices.end());
    if(std::adjacent_find(
           work_item_indices.begin(), work_item_indices.end()) !=
       work_item_indices.end()) {
        return false;
    }

    std::scoped_lock lock(state_->mutex);
    if(!state_->baseline_observed ||
       std::any_of(
           work_item_indices.begin(), work_item_indices.end(),
           [this](std::size_t work_item_index) {
               return work_item_index >=
                      state_->prepared.invocation.work_items.size();
           }) ||
       std::any_of(
           state_->transaction_inventory.begin(),
           state_->transaction_inventory.end(),
           [&transaction_token](
               const CleanupTrustedTransactionTokenInventoryEntry& entry) {
               return entry.transaction_token == transaction_token;
           })) {
        return false;
    }
    state_->transaction_inventory.push_back(
        CleanupTrustedTransactionTokenInventoryEntry{
            owner, transaction_token, std::move(work_item_indices), false});
    return true;
}

bool CleanupInvocationAuthority::mark_trusted_transaction_completed(
    InvocationDependencyTransactionOwner owner,
    const std::string& transaction_token) const {
    if(!is_active()) return false;
    std::scoped_lock lock(state_->mutex);
    const auto entry = std::find_if(
        state_->transaction_inventory.begin(),
        state_->transaction_inventory.end(),
        [owner, &transaction_token](const auto& candidate) {
            return candidate.owner == owner &&
                   candidate.transaction_token == transaction_token;
        });
    if(entry == state_->transaction_inventory.end() ||
       entry->completed_successfully) {
        return false;
    }
    entry->completed_successfully = true;
    return true;
}

const PreparedRemoteSourceBuild& CleanupInvocationAuthority::prepared()
    const noexcept {
    return state_->prepared;
}

bool CleanupInvocationAuthority::is_active() const noexcept {
    return state_ != nullptr;
}

bool CleanupInvocationAuthority::baseline_was_observed() const noexcept {
    if(!is_active()) return false;
    std::scoped_lock lock(state_->mutex);
    return state_->baseline_observed;
}

CleanupInvocationSession::CleanupInvocationSession(
    std::shared_ptr<CleanupInvocationSessionState> state) noexcept
    : state_(std::move(state)), authority_(state_) {
}

CleanupInvocationSession CleanupInvocationSession::begin_for_collector(
    PreparedRemoteSourceBuild prepared) {
    if(prepared.source.source_kind() != SourceBuildSourceKind::Aur ||
       !prepared.aur_build_plan.has_value()) {
        throw std::invalid_argument(
            "Cleanup invocation session requires a remote AUR preparation.");
    }
    return CleanupInvocationSession(
        std::make_shared<CleanupInvocationSessionState>(
            std::move(prepared)));
}

#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
CleanupInvocationSession CleanupInvocationSession::begin(
    PreparedRemoteSourceBuild prepared) {
    return begin_for_collector(std::move(prepared));
}
#endif

const CleanupInvocationAuthority& CleanupInvocationSession::authority()
    const noexcept {
    return authority_;
}

const PreparedRemoteSourceBuild& CleanupInvocationSession::prepared()
    const noexcept {
    return state_->prepared;
}

PreparedRemoteSourceBuild&
CleanupInvocationSession::prepared_for_execution() noexcept {
    return state_->prepared;
}

bool CleanupInvocationSession::is_active() const noexcept {
    return state_ != nullptr && authority_.is_active();
}

bool CleanupInvocationSession::record_baseline_observation() const {
    if(!is_active()) return false;
    std::scoped_lock lock(state_->mutex);
    if(state_->baseline_observed ||
       !state_->transaction_inventory.empty() ||
       state_->post_success_observation_count != 0) {
        return false;
    }
    state_->baseline_observed = true;
    return true;
}

std::optional<std::size_t>
CleanupInvocationSession::record_post_success_observation() const {
    if(!is_active()) return std::nullopt;
    std::scoped_lock lock(state_->mutex);
    // Ordinary invocation success is checked by the phase producer. No trusted
    // transaction is required, but a registered unfinished transaction is bad
    // evidence and must still prevent post-success observations.
    if(!state_->baseline_observed ||
       !std::all_of(
           state_->transaction_inventory.begin(),
           state_->transaction_inventory.end(), [](const auto& entry) {
               return entry.completed_successfully;
           })) {
        return std::nullopt;
    }
    ++state_->post_success_observation_count;
    return state_->transaction_inventory.size();
}

bool CleanupInvocationSession::contains_trusted_transaction_token(
    InvocationDependencyTransactionOwner owner,
    const std::string& transaction_token,
    std::size_t work_item_index) const noexcept {
    if(!is_active()) return false;
    std::scoped_lock lock(state_->mutex);
    return std::any_of(
        state_->transaction_inventory.begin(),
        state_->transaction_inventory.end(),
        [owner, &transaction_token, work_item_index](
            const CleanupTrustedTransactionTokenInventoryEntry& entry) {
            return entry.owner == owner &&
                   entry.transaction_token == transaction_token &&
                   entry.completed_successfully &&
                   std::find(
                       entry.work_item_indices.begin(),
                       entry.work_item_indices.end(), work_item_index) !=
                       entry.work_item_indices.end();
        });
}

std::vector<CleanupTrustedTransactionTokenInventoryEntry>
CleanupInvocationSession::transaction_token_inventory() const {
    if(!is_active()) return {};
    std::scoped_lock lock(state_->mutex);
    return state_->transaction_inventory;
}

#ifdef MOGUET_ENABLE_CLEANUP_INVOCATION_SESSION_TEST_HOOKS
void mark_cleanup_invocation_baseline_observed_for_test(
    CleanupInvocationSession& session) {
    if(!session.record_baseline_observation()) {
        throw std::logic_error(
            "Cleanup invocation test baseline is out of phase.");
    }
}

bool register_cleanup_invocation_transaction_token_for_test(
    CleanupInvocationSession& session,
    InvocationDependencyTransactionOwner owner,
    const std::string& transaction_token,
    std::vector<std::size_t> work_item_indices,
    bool completed_successfully) {
    if(!session.authority_.register_trusted_transaction_token(
           owner, transaction_token, std::move(work_item_indices))) {
        return false;
    }
    if(!completed_successfully) return true;
    return session.authority_.mark_trusted_transaction_completed(
        owner, transaction_token);
}
#endif

PacmanTransactionReceipt::PacmanTransactionReceipt(
    PacmanTransactionReceiptState state,
    std::optional<std::string> transaction_token,
    std::optional<InvocationDependencyTransactionOwner> owner,
    std::vector<PacmanTransactionPackageObservation> package_operations,
    std::vector<PacmanInstalledPackageReceipt> newly_installed_packages,
    std::vector<PacmanTransactionReceiptIssueKind> issues) noexcept
    : state_(state), transaction_token_(std::move(transaction_token)),
      owner_(owner), package_operations_(std::move(package_operations)),
      newly_installed_packages_(std::move(newly_installed_packages)),
      issues_(std::move(issues)) {
}

PacmanTransactionReceiptState PacmanTransactionReceipt::state()
    const noexcept {
    return state_;
}

const std::optional<std::string>&
PacmanTransactionReceipt::transaction_token() const noexcept {
    return transaction_token_;
}

const std::optional<InvocationDependencyTransactionOwner>&
PacmanTransactionReceipt::owner() const noexcept {
    return owner_;
}

const std::vector<PacmanTransactionPackageObservation>&
PacmanTransactionReceipt::package_operations() const noexcept {
    return package_operations_;
}

const std::vector<PacmanInstalledPackageReceipt>&
PacmanTransactionReceipt::newly_installed_packages() const noexcept {
    return newly_installed_packages_;
}

const std::vector<PacmanTransactionReceiptIssueKind>&
PacmanTransactionReceipt::issues() const noexcept {
    return issues_;
}

bool PacmanTransactionReceipt::is_complete_for(
    const std::string& expected_transaction_token,
    InvocationDependencyTransactionOwner expected_owner) const noexcept {
    return state_ == PacmanTransactionReceiptState::Complete &&
           issues_.empty() && transaction_token_.has_value() &&
           transaction_token_.value() == expected_transaction_token &&
           owner_.has_value() && owner_.value() == expected_owner &&
           is_valid_pacman_transaction_token(expected_transaction_token) &&
           is_valid_transaction_owner(expected_owner);
}

bool PacmanTransactionReceipt::contains_newly_installed_package(
    const std::string& package_name) const noexcept {
    if(state_ != PacmanTransactionReceiptState::Complete ||
       !issues_.empty()) {
        return false;
    }
    return std::any_of(
        newly_installed_packages_.begin(),
        newly_installed_packages_.end(),
        [&package_name](const PacmanInstalledPackageReceipt& package) {
            return package.package_name == package_name;
        });
}

bool is_valid_pacman_transaction_token(
    const std::string& transaction_token) noexcept {
    return is_valid_trusted_alpm_receipt_token(transaction_token);
}

PacmanTransactionReceipt validate_pacman_transaction_receipt(
    const std::string& expected_transaction_token,
    InvocationDependencyTransactionOwner expected_owner,
    const PacmanTransactionReceiptObservation& observation) {
    std::vector<PacmanTransactionReceiptIssueKind> issues;
    bool is_invalid = false;
    const auto invalidate = [&issues, &is_invalid](
                                PacmanTransactionReceiptIssueKind issue) {
        add_receipt_issue(issues, issue);
        is_invalid = true;
    };

    if(!is_valid_pacman_transaction_token(expected_transaction_token)) {
        invalidate(
            PacmanTransactionReceiptIssueKind::
                InvalidExpectedTransactionToken);
    }
    if(!is_valid_transaction_owner(expected_owner)) {
        invalidate(
            PacmanTransactionReceiptIssueKind::
                InvalidExpectedTransactionOwner);
    }
    if(!is_valid_receipt_observation_state(observation.state)) {
        invalidate(
            PacmanTransactionReceiptIssueKind::InvalidObservationState);
    }

    if(observation.state ==
       PacmanTransactionReceiptObservationState::Missing) {
        add_receipt_issue(
            issues, PacmanTransactionReceiptIssueKind::ReceiptMissing);
        if(observation.transaction_token.has_value() ||
           observation.owner.has_value() ||
           !observation.package_operations.empty()) {
            invalidate(
                PacmanTransactionReceiptIssueKind::
                    UnexpectedReceiptData);
        }
        canonicalize_receipt_issues(issues);
        return PacmanTransactionReceipt(
            is_invalid ? PacmanTransactionReceiptState::Invalid
                       : PacmanTransactionReceiptState::Unavailable,
            observation.transaction_token, observation.owner,
            observation.package_operations, {}, std::move(issues));
    }

    if(!observation.transaction_token.has_value()) {
        invalidate(
            PacmanTransactionReceiptIssueKind::
                ObservedTransactionTokenMissing);
    } else if(!is_valid_pacman_transaction_token(
                  observation.transaction_token.value())) {
        invalidate(
            PacmanTransactionReceiptIssueKind::
                InvalidObservedTransactionToken);
    } else if(observation.transaction_token.value() !=
              expected_transaction_token) {
        invalidate(
            PacmanTransactionReceiptIssueKind::TransactionTokenMismatch);
    }

    if(!observation.owner.has_value()) {
        invalidate(
            PacmanTransactionReceiptIssueKind::
                ObservedTransactionOwnerMissing);
    } else if(!is_valid_transaction_owner(observation.owner.value())) {
        invalidate(
            PacmanTransactionReceiptIssueKind::
                InvalidObservedTransactionOwner);
    } else if(observation.owner.value() != expected_owner) {
        invalidate(
            PacmanTransactionReceiptIssueKind::TransactionOwnerMismatch);
    }

    std::set<std::string> observed_package_names;
    std::vector<PacmanInstalledPackageReceipt> newly_installed_packages;
    for(const PacmanTransactionPackageObservation& package :
        observation.package_operations) {
        if(!is_supported_install_transaction_operation(package.operation)) {
            invalidate(
                PacmanTransactionReceiptIssueKind::
                    InvalidPackageOperation);
        }
        if(!is_valid_package_name(package.package_name)) {
            invalidate(
                PacmanTransactionReceiptIssueKind::InvalidPackageName);
        }
        if(!observed_package_names.insert(package.package_name).second) {
            invalidate(
                PacmanTransactionReceiptIssueKind::DuplicatePackageName);
        }
        if(package.operation == PacmanTransactionPackageOperation::Install &&
           is_valid_package_name(package.package_name)) {
            newly_installed_packages.push_back(
                PacmanInstalledPackageReceipt{package.package_name});
        }
    }

    PacmanTransactionReceiptState state =
        PacmanTransactionReceiptState::Complete;
    if(observation.state ==
       PacmanTransactionReceiptObservationState::Incomplete) {
        add_receipt_issue(
            issues,
            PacmanTransactionReceiptIssueKind::ReceiptIncomplete);
        state = PacmanTransactionReceiptState::Incomplete;
    }
    if(is_invalid) state = PacmanTransactionReceiptState::Invalid;
    if(state != PacmanTransactionReceiptState::Complete) {
        // Partial or malformed observations never expose a package as an
        // authoritative Install receipt.
        newly_installed_packages.clear();
    }

    canonicalize_receipt_issues(issues);
    return PacmanTransactionReceipt(
        state, observation.transaction_token, observation.owner,
        observation.package_operations,
        std::move(newly_installed_packages), std::move(issues));
}

CleanupClassificationResult::CleanupClassificationResult(
    CleanupClassification classification,
    std::vector<CleanupClassificationReason> reasons) noexcept
    : classification_(classification), reasons_(std::move(reasons)) {
}

CleanupClassification CleanupClassificationResult::classification()
    const noexcept {
    return classification_;
}

const std::vector<CleanupClassificationReason>&
CleanupClassificationResult::reasons() const noexcept {
    return reasons_;
}

CleanupClassificationResult classify_invocation_owned_cleanup(
    const InvocationOwnedCleanupCandidate& candidate) {
    std::vector<CleanupClassificationReason> reasons =
        structural_reasons(candidate);
    if(!reasons.empty()) {
        return CleanupClassificationResult(
            CleanupClassification::Invalid, std::move(reasons));
    }

    reasons = protection_reasons(candidate);
    if(!reasons.empty()) {
        return CleanupClassificationResult(
            CleanupClassification::Protected, std::move(reasons));
    }

    reasons = unknown_reasons(candidate);
    if(!reasons.empty()) {
        return CleanupClassificationResult(
            CleanupClassification::Unknown, std::move(reasons));
    }

    return CleanupClassificationResult(
        CleanupClassification::Eligible,
        {CleanupClassificationReason::EligibleEvidenceComplete});
}
