#include "devel_source_artifact_install_state.hpp"

#include <stdexcept>
#include <utility>

namespace {
using Issue = InstalledDevelSourceBuildIssue;
using Operation = DevelSourceArtifactInstallOperation;

const InstalledPackageRecordObservation* observation(
    const ExactArtifactRecordObservationsResult& records, std::size_t index,
    const std::string& name) noexcept {
    const auto* values = std::get_if<ExactArtifactRecordObservations>(&records);
    if(!values || values->size() != 1 || values->front().artifact_index != index ||
       values->front().package_name != name) return nullptr;
    return &values->front().observation;
}
} // namespace

std::optional<Issue> DevelSourceArtifactInstallAuthority::correlate(
    const DevelSourceArtifactInstallState& state) noexcept {
    try {
#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
        if(state.fail_final_correlation) throw std::bad_alloc();
#endif
        if(state.built_artifact_count != 1 || state.fresh_binding_count != 1 ||
           state.receipt->manifest().artifacts.size() != 1 || state.receipt->operations().size() != 1)
            return Issue::UnsupportedCardinality;
        const auto& built = state.proof;
        const auto& receipt = *state.receipt;
        const auto& fresh = *state.fresh_binding;
        if(!built.valid() || !receipt.active() || !fresh.active()) return Issue::InactiveComponent;
        // Content equality is insufficient: preserve the unique build identity
        // through the original owner, and the unique transaction seal through
        // receipt -> observer. Neither identity is caller supplied or decoded.
        if(!built.lineage_ || receipt.built_lineage_ != built.lineage_) return Issue::BuiltLineageMismatch;
        if(!receipt.transaction_lineage_ || receipt.transaction_lineage_ != state.transaction_lineage ||
           fresh.transaction_lineage_ != receipt.transaction_lineage_ ||
           !state.transaction_token || *state.transaction_token != receipt.manifest().transaction_token ||
           fresh.transaction_token_ != receipt.manifest().transaction_token ||
           fresh.staged_identity_ != receipt.staged_identity_sha256() || receipt.staged_identity_sha256().empty() ||
           receipt.manifest().purpose != SourceArtifactInstallTrustedPurpose::ExactInstalledBinding)
            return Issue::TransactionLineageMismatch;

        const auto& selected = receipt.manifest().artifacts.front();
        const auto& operation = receipt.operations().front();
        const auto& artifact = built.artifact();
        const auto& expected = artifact.evidence();
        const auto& binding = fresh.binding();
        // The Slice 4 single-artifact bridge selects index 0. Do not infer the
        // artifact index from a generic receipt's vector position.
        if(selected.artifact_index != 0 || operation.artifact.artifact_index != 0 || fresh.artifact_index_ != 0)
            return Issue::ArtifactIndexMismatch;
        if(operation.artifact != selected) return Issue::TransactionLineageMismatch;
        if(selected.package_name != expected.identity.package_name ||
           !expected.identity.package_base.value() || selected.package_base != *expected.identity.package_base.value() ||
           receipt.manifest().package_base != selected.package_base ||
           selected.full_version != expected.identity.full_version ||
           !expected.identity.architecture.value() || selected.architecture != *expected.identity.architecture.value() ||
           binding.package() != artifact.package() || !binding.version().full_version() ||
           *binding.version().full_version() != selected.full_version || !binding.architecture().value() ||
           *binding.architecture().value() != selected.architecture)
            return Issue::PackageIdentityMismatch;
        if(selected.archive_sha256 != expected.archive_digest.value() || selected.artifact_size != artifact.size() ||
           selected.signature_size != 0 || selected.signature_sha256 != "-") return Issue::ArchiveDigestMismatch;
        if(selected.raw_mtree_sha256 != expected.mtree_digest.value() ||
           binding.mtree_digest() != expected.mtree_digest) return Issue::MtreeMismatch;

        const auto& evidence = receipt.database_evidence();
        if(!std::holds_alternative<InstalledDatabaseWorld>(evidence.world)) return Issue::DatabaseRecordMismatch;
        if(operation.operation != ExactArtifactTransactionOperation::Install &&
           operation.operation != ExactArtifactTransactionOperation::Upgrade) return Issue::TransactionLineageMismatch;
        const auto* before = observation(evidence.baseline, selected.artifact_index, selected.package_name);
        const auto* after = observation(operation.operation == ExactArtifactTransactionOperation::Install
                                            ? evidence.install_anchors
                                            : evidence.upgrade_anchors,
                                        selected.artifact_index, selected.package_name);
        const auto* anchor = after ? std::get_if<InstalledPackageRecordSnapshot>(after) : nullptr;
        if(!before || !anchor) return Issue::DatabaseRecordMismatch;
        if(anchor->package_name != selected.package_name || anchor->package_base != selected.package_base ||
           anchor->full_version != selected.full_version || anchor->architecture != selected.architecture)
            return Issue::PackageIdentityMismatch;
        if(anchor->raw_mtree_sha256 != selected.raw_mtree_sha256 || fresh.snapshot_.raw_mtree_sha256 != anchor->raw_mtree_sha256)
            return Issue::MtreeMismatch;
        if(binding.record_generation().scheme() != InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt ||
           binding.record_generation().opaque_identity() != anchor->record_generation ||
           fresh.snapshot_.record_generation != anchor->record_generation)
            return Issue::InstalledGenerationMismatch;
        if(binding.database_record_digest().value() != anchor->raw_database_sha256 || fresh.snapshot_ != *anchor)
            return Issue::DatabaseRecordMismatch;
        if(operation.operation == ExactArtifactTransactionOperation::Install) {
            if(!std::holds_alternative<InstalledPackageRecordAbsent>(*before)) return Issue::TransactionLineageMismatch;
        } else {
            const auto* previous = std::get_if<InstalledPackageRecordSnapshot>(before);
            if(!previous || previous->package_name != selected.package_name) return Issue::TransactionLineageMismatch;
            if(previous->record_generation == anchor->record_generation) return Issue::InstalledGenerationMismatch;
        }
        return std::nullopt;
    } catch(const std::bad_alloc&) {
        return Issue::ResourceFailure;
    } catch(...) {
        return Issue::InternalFailure;
    }
}

DevelSourceArtifactInstallResult DevelSourceArtifactInstallAuthority::finalize(
    std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept {
    if(state->operation != Operation::Succeeded || state->pacman_exit_status != 0) {
        state->proof_issue = state->operation == Operation::NotAttempted ? Issue::NotExecuted : Issue::OperationNotSuccessful;
    } else if(!state->receipt || !state->receipt->active() || state->receipt_issue) {
        state->proof_issue = Issue::ReceiptUnavailable;
    } else if(!state->fresh_binding || state->binding_issue) {
        state->proof_issue = Issue::BindingUnavailable;
    } else {
        state->proof_issue = correlate(*state);
        if(!state->proof_issue)
            return DevelSourceArtifactInstallResult(InstalledDevelSourceBuildProof(std::move(state)));
    }
    return DevelSourceArtifactInstallResult(std::move(state));
}

InstalledDevelSourceBuildProof::InstalledDevelSourceBuildProof(std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept
    : state_(std::move(state)) {
}
InstalledDevelSourceBuildProof::InstalledDevelSourceBuildProof(InstalledDevelSourceBuildProof&&) noexcept = default;
InstalledDevelSourceBuildProof::~InstalledDevelSourceBuildProof() noexcept = default;
bool InstalledDevelSourceBuildProof::valid() const noexcept {
    return state_ && state_->proof.valid();
}
const DevelSourceArtifactInstallState& InstalledDevelSourceBuildProof::require_state() const {
    if(!valid()) throw std::logic_error("installed devel build proof is inactive");
    return *state_;
}
const EvaluatedDevelSourceBuildProof& InstalledDevelSourceBuildProof::built_proof() const {
    return require_state().proof;
}
const ExactArtifactTransactionReceipt& InstalledDevelSourceBuildProof::receipt() const {
    return *require_state().receipt;
}
const InstalledArtifactBinding& InstalledDevelSourceBuildProof::installed_binding() const {
    return require_state().fresh_binding->binding();
}
std::size_t InstalledDevelSourceBuildProof::artifact_index() const {
    return require_state().fresh_binding->artifact_index();
}
ExactArtifactTransactionOperation InstalledDevelSourceBuildProof::operation() const {
    return receipt().operations().front().operation;
}

DevelSourceArtifactInstallResult::DevelSourceArtifactInstallResult(std::unique_ptr<DevelSourceArtifactInstallState> state) noexcept
    : value_(std::in_place_index<0>, std::move(state)) {
}
DevelSourceArtifactInstallResult::DevelSourceArtifactInstallResult(InstalledDevelSourceBuildProof proof) noexcept
    : value_(std::in_place_index<1>, std::move(proof)) {
}
DevelSourceArtifactInstallResult::DevelSourceArtifactInstallResult(DevelSourceArtifactInstallResult&&) noexcept = default;
DevelSourceArtifactInstallResult::~DevelSourceArtifactInstallResult() noexcept = default;
bool DevelSourceArtifactInstallResult::valid() const noexcept {
    if(const auto* complete = std::get_if<InstalledDevelSourceBuildProof>(&value_)) return complete->valid();
    return std::get<0>(value_) != nullptr;
}
const DevelSourceArtifactInstallState& DevelSourceArtifactInstallResult::require_state() const {
    if(!valid()) throw std::logic_error("devel source artifact install result is moved from");
    if(const auto* complete = std::get_if<InstalledDevelSourceBuildProof>(&value_)) return complete->require_state();
    return *std::get<0>(value_);
}
DevelSourceArtifactInstallOperation DevelSourceArtifactInstallResult::operation() const {
    return require_state().operation;
}
std::optional<int> DevelSourceArtifactInstallResult::pacman_exit_status() const {
    return require_state().pacman_exit_status;
}
const SourceArtifactInstallTrustedExecutionResult* DevelSourceArtifactInstallResult::transport_result() const {
    const auto& result = require_state().execution;
    return result ? &*result : nullptr;
}
DevelSourceArtifactInstallReceipt DevelSourceArtifactInstallResult::receipt_state() const {
    const auto& state = require_state();
    if(state.operation != Operation::Succeeded) return DevelSourceArtifactInstallReceipt::NotAttempted;
    if(receipt()) return DevelSourceArtifactInstallReceipt::Complete;
    if(state.receipt_issue == ExactArtifactReceiptIssue::Missing) return DevelSourceArtifactInstallReceipt::Missing;
    if(state.receipt_issue == ExactArtifactReceiptIssue::ResourceFailure) return DevelSourceArtifactInstallReceipt::Incomplete;
    return DevelSourceArtifactInstallReceipt::Invalid;
}
const ExactArtifactTransactionReceipt* DevelSourceArtifactInstallResult::receipt() const {
    const auto& state = require_state();
    return state.operation == Operation::Succeeded && state.pacman_exit_status == 0 && state.receipt &&
                   state.receipt->active() && !state.receipt_issue
               ? &*state.receipt
               : nullptr;
}
std::optional<ExactArtifactReceiptIssue> DevelSourceArtifactInstallResult::receipt_issue() const {
    return require_state().receipt_issue;
}
DevelSourceArtifactInstallProof DevelSourceArtifactInstallResult::proof_state() const {
    if(proof()) return DevelSourceArtifactInstallProof::Complete;
    return receipt() ? DevelSourceArtifactInstallProof::Incomplete : DevelSourceArtifactInstallProof::NotAttempted;
}
const InstalledDevelSourceBuildProof* DevelSourceArtifactInstallResult::proof() const noexcept {
    const auto* complete = std::get_if<InstalledDevelSourceBuildProof>(&value_);
    return complete && complete->valid() ? complete : nullptr;
}
std::optional<Issue> DevelSourceArtifactInstallResult::proof_issue() const {
    return require_state().proof_issue;
}
std::optional<InstalledRecordObservationIssue> DevelSourceArtifactInstallResult::binding_issue() const {
    return require_state().binding_issue;
}
const DevelSourceArtifactInstallCleanup& DevelSourceArtifactInstallResult::privileged_cleanup() const {
    return require_state().cleanup;
}
DevelSourceArtifactInstallCleanupState DevelSourceArtifactInstallResult::source_context_cleanup() const {
    static_cast<void>(require_state());
    return DevelSourceArtifactInstallCleanupState::Retained;
}
