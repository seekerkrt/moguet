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
      state_(other.state_), stage_(other.stage_), issue_(other.issue_), children_(std::move(other.children_)) {
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
    return children_.size() == 1 ? children_.front().projection_issue : std::nullopt;
}
const DevelBuildProvenance* DevelBuildProvenancePublicationResult::projected_provenance() const {
    require_valid();
    return children_.size() == 1 && children_.front().projected ? &*children_.front().projected : nullptr;
}
const DevelBuildProvenanceStoreReadResult* DevelBuildProvenancePublicationResult::store_read_result() const {
    require_valid();
    return children_.size() == 1 && children_.front().read ? &*children_.front().read : nullptr;
}
const DevelBuildProvenanceStorePublishResult* DevelBuildProvenancePublicationResult::store_publish_result() const {
    require_valid();
    return children_.size() == 1 && children_.front().publication ? &*children_.front().publication : nullptr;
}
const DevelBuildProvenancePublicationIdentity* DevelBuildProvenancePublicationResult::identity() const noexcept {
    return valid() && state_ == State::Complete && children_.size() == 1 && children_.front().identity
               ? &*children_.front().identity
               : nullptr;
}

const std::vector<DevelBuildProvenanceChildPublication>& DevelBuildProvenancePublicationResult::children() const {
    require_valid();
    return children_;
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
    std::size_t position = 0;
    try {
        result.children_.reserve(proof->bindings().size());
        for(const auto& binding : proof->bindings())
            result.children_.push_back({binding.package_name, State::NotAttempted, Stage::Eligibility, {}, {}, {}, {}, {}, {}, {}});
        for(; position < result.children_.size(); ++position) {
            auto& child = result.children_[position];
            child.state = State::Failed;
            const auto& binding = proof->bindings()[position];
            child.stage = result.stage_ = Stage::Projection;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
            before_stage(result.stage_);
#endif
            const auto& built = proof->built_proof();
            // S5 already closed lineage/correlation. This is only the persistent
            // schema consistency check over values owned by that same final proof.
            auto projected = make_devel_build_provenance(
                built.package_base(), built.reviewed_binding(), built.evaluated_source().git_source(),
                built.actual_built_revision(), built.artifacts().at(binding.artifact_index).evidence(), binding.binding->binding());
            if(auto* failure = std::get_if<DevelBuildProvenanceFailure>(&projected)) {
                child.projection_issue = *failure;
                child.issue = result.issue_ = Issue::ProjectionRejected;
                return result;
            }
            child.projected.emplace(std::move(std::get<DevelBuildProvenance>(projected)));
            child.stage = result.stage_ = Stage::Serialization;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
            before_stage(result.stage_);
#endif
            child.encoded = encode_devel_build_provenance(*child.projected);
            child.identity.emplace(DevelBuildProvenancePublicationIdentity{
                built.package_base(), 0, xdg_generation_store_raw_contents_sha256(child.encoded), child.package_name});
            child.stage = result.stage_ = Stage::PredecessorRead;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
            before_stage(result.stage_);
#endif
            child.read.emplace(read_devel_build_provenance(binding.binding->binding().package()));
            std::optional<DevelBuildProvenanceStoreObservedRecord> predecessor;
            if(const auto* loaded = std::get_if<DevelBuildProvenanceStoreLoaded>(&*child.read)) {
                predecessor = loaded->observed;
            } else if(!std::holds_alternative<DevelBuildProvenanceStoreMissing>(*child.read)) {
                child.issue = result.issue_ = Issue::StoreReadRejected;
                return result;
            }
            child.stage = result.stage_ = Stage::StorePublication;
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
            before_stage(result.stage_);
#endif
            // The existing store re-encodes this immutable value deterministically
            // with the same v1 codec. Keep its exact-predecessor/no-retry contract.
            // 6-A owns commit classification; every operation after this return is
            // a nothrow move or primitive assignment, never a new commit inference.
            child.publication.emplace(publish_child_devel_build_provenance(*child.projected, predecessor));
            if(const auto* published = std::get_if<DevelBuildProvenanceStorePublished>(&*child.publication)) {
                child.identity->generation = published->observed.generation;
                child.state = State::Complete;
                child.issue.reset();
            } else if(std::holds_alternative<DevelBuildProvenanceStorePublishedUncertain>(*child.publication)) {
                child.state = result.state_ = State::OutcomeUnknown;
                child.issue = result.issue_ = Issue::StorePublicationUncertain;
            } else {
                child.issue = result.issue_ = Issue::StorePublicationFailed;
            }
            if(child.state != State::Complete) return result;
        }
        result.state_ = State::Complete;
        result.issue_.reset();
    } catch(const std::bad_alloc&) {
        result.issue_ = Issue::ResourceFailure;
    } catch(const std::length_error&) {
        result.issue_ = Issue::ResourceFailure;
    } catch(...) {
        result.issue_ = result.stage_ == Stage::Serialization ? Issue::SerializationFailure : Issue::InternalFailure;
    }
    if(position < result.children_.size()) {
        auto& child = result.children_[position];
        child.issue = result.issue_;
        child.state = State::Failed;
        child.stage = result.stage_;
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
