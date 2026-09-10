#include "devel_build_provenance_publication.hpp"

#include "evaluated_devel_source_build.hpp"

#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {
using State = DevelBuildProvenancePublicationState;
using Stage = DevelBuildProvenancePublicationStage;
using Issue = DevelBuildProvenancePublicationIssue;

static_assert(std::is_nothrow_move_constructible_v<DevelSourceArtifactInstallResult>);
static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenance>);
static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenanceStoreReadResult>);
static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenanceStorePublishResult>);
static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenancePublicationIdentity>);

#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
DevelBuildProvenancePublicationTestHook g_publication_hook = nullptr;
void before_stage(Stage stage) {
    if(g_publication_hook) g_publication_hook(stage);
}
#endif
} // namespace

DevelBuildProvenancePublicationResult::DevelBuildProvenancePublicationResult(
    DevelSourceArtifactInstallResult result) noexcept
    : installation_(std::move(result)) {
}

DevelBuildProvenancePublicationResult::DevelBuildProvenancePublicationResult(
    DevelBuildProvenancePublicationResult&& other) noexcept
    : installation_(std::move(other.installation_)), active_(std::exchange(other.active_, false)),
      state_(other.state_), stage_(other.stage_), issue_(other.issue_), projection_issue_(other.projection_issue_),
      projected_(std::move(other.projected_)), encoded_(std::move(other.encoded_)), identity_(std::move(other.identity_)),
      read_(std::move(other.read_)), publication_(std::move(other.publication_)) {
}

DevelBuildProvenancePublicationResult::~DevelBuildProvenancePublicationResult() noexcept = default;

bool DevelBuildProvenancePublicationResult::valid() const noexcept {
    return active_ && installation_.valid();
}
void DevelBuildProvenancePublicationResult::require_valid() const {
    if(!valid()) throw std::logic_error("Inactive devel provenance publication result.");
}
const DevelSourceArtifactInstallResult& DevelBuildProvenancePublicationResult::installation() const {
    require_valid();
    return installation_;
}
State DevelBuildProvenancePublicationResult::state() const {
    require_valid();
    return state_;
}
Stage DevelBuildProvenancePublicationResult::stage() const {
    require_valid();
    return stage_;
}
std::optional<Issue> DevelBuildProvenancePublicationResult::issue() const {
    require_valid();
    return issue_;
}
std::optional<DevelBuildProvenanceFailure> DevelBuildProvenancePublicationResult::projection_issue() const {
    require_valid();
    return projection_issue_;
}
const DevelBuildProvenance* DevelBuildProvenancePublicationResult::projected_provenance() const {
    require_valid();
    return projected_ ? &*projected_ : nullptr;
}
const DevelBuildProvenanceStoreReadResult* DevelBuildProvenancePublicationResult::store_read_result() const {
    require_valid();
    return read_ ? &*read_ : nullptr;
}
const DevelBuildProvenanceStorePublishResult* DevelBuildProvenancePublicationResult::store_publish_result() const {
    require_valid();
    return publication_ ? &*publication_ : nullptr;
}
const DevelBuildProvenancePublicationIdentity* DevelBuildProvenancePublicationResult::identity() const noexcept {
    return valid() && state_ == State::Complete && identity_ ? &*identity_ : nullptr;
}

std::optional<DevelBuildProvenancePublicationResult> DevelBuildProvenancePublicationAuthority::publish(
    DevelSourceArtifactInstallResult installation, DevelBuildProvenancePublicationRequest request) noexcept {
    if(!installation.valid()) return std::nullopt;
    // No allocation precedes ownership of the known installation outcome.
    DevelBuildProvenancePublicationResult result(std::move(installation));
    const auto& installed = result.installation_;
    switch(installed.operation()) {
        case DevelSourceArtifactInstallOperation::NotAttempted: result.issue_ = Issue::NotExecuted; return result;
        case DevelSourceArtifactInstallOperation::OutcomeUnknown: result.issue_ = Issue::OperationUnknown; return result;
        case DevelSourceArtifactInstallOperation::Failed: result.issue_ = Issue::OperationFailed; return result;
        case DevelSourceArtifactInstallOperation::Succeeded: break;
        default:
            result.state_ = State::Failed;
            result.issue_ = Issue::InternalFailure;
            return result;
    }
    if(installed.receipt_state() != DevelSourceArtifactInstallReceipt::Complete) {
        result.issue_ = Issue::ReceiptUnavailable;
        return result;
    }
    const auto* proof = installed.proof();
    if(installed.proof_state() != DevelSourceArtifactInstallProof::Complete || !proof || !proof->valid()) {
        result.issue_ = Issue::ProofIncomplete;
        return result;
    }
    if(request == DevelBuildProvenancePublicationRequest::NotRequested) {
        result.issue_ = Issue::NotRequested;
        return result;
    }
    result.state_ = State::Failed;
    if(request != DevelBuildProvenancePublicationRequest::Publish) {
        result.issue_ = Issue::InternalFailure;
        return result;
    }
    try {
        result.stage_ = Stage::Projection;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        before_stage(result.stage_);
#endif
        const auto& built = proof->built_proof();
        // S5 already closed lineage/correlation. This is only the persistent
        // schema consistency check over values owned by that same final proof.
        auto projected = make_devel_build_provenance(
            built.package_base(), built.reviewed_binding(), built.evaluated_source().git_source(),
            built.actual_built_revision(), built.artifact().evidence(), proof->installed_binding());
        if(auto* failure = std::get_if<DevelBuildProvenanceFailure>(&projected)) {
            result.projection_issue_ = *failure;
            result.issue_ = Issue::ProjectionRejected;
            return result;
        }
        result.projected_.emplace(std::move(std::get<DevelBuildProvenance>(projected)));
        result.stage_ = Stage::Serialization;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        before_stage(result.stage_);
#endif
        result.encoded_ = encode_devel_build_provenance(*result.projected_);
        result.identity_.emplace(DevelBuildProvenancePublicationIdentity{
            built.package_base(), 0, xdg_generation_store_raw_contents_sha256(result.encoded_)});
        result.stage_ = Stage::PredecessorRead;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        before_stage(result.stage_);
#endif
        result.read_.emplace(read_devel_build_provenance(built.package_base()));
        std::optional<DevelBuildProvenanceStoreObservedRecord> predecessor;
        if(const auto* loaded = std::get_if<DevelBuildProvenanceStoreLoaded>(&*result.read_)) {
            predecessor = loaded->observed;
        } else if(!std::holds_alternative<DevelBuildProvenanceStoreMissing>(*result.read_)) {
            result.issue_ = Issue::StoreReadRejected;
            return result;
        }
        result.stage_ = Stage::StorePublication;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        before_stage(result.stage_);
#endif
        // The existing store re-encodes this immutable value deterministically
        // with the same v1 codec. Keep its exact-predecessor/no-retry contract.
        // 6-A owns commit classification; every operation after this return is
        // a nothrow move or primitive assignment, never a new commit inference.
        result.publication_.emplace(publish_devel_build_provenance(*result.projected_, predecessor));
        if(const auto* published = std::get_if<DevelBuildProvenanceStorePublished>(&*result.publication_)) {
            result.identity_->generation = published->observed.generation;
            result.state_ = State::Complete;
            result.issue_.reset();
        } else if(std::holds_alternative<DevelBuildProvenanceStorePublishedUncertain>(*result.publication_)) {
            result.state_ = State::OutcomeUnknown;
            result.issue_ = Issue::StorePublicationUncertain;
        } else {
            result.issue_ = Issue::StorePublicationFailed;
        }
    } catch(const std::bad_alloc&) {
        result.issue_ = Issue::ResourceFailure;
    } catch(const std::length_error&) {
        result.issue_ = Issue::ResourceFailure;
    } catch(...) {
        result.issue_ = result.stage_ == Stage::Serialization ? Issue::SerializationFailure : Issue::InternalFailure;
    }
    return result;
}

std::optional<DevelBuildProvenancePublicationResult> publish_installed_devel_source_build(
    DevelSourceArtifactInstallResult result, DevelBuildProvenancePublicationRequest request) noexcept {
    return DevelBuildProvenancePublicationAuthority::publish(std::move(result), request);
}

#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
void set_devel_build_provenance_publication_test_hook(DevelBuildProvenancePublicationTestHook hook) {
    g_publication_hook = hook;
}
#endif
