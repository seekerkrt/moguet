#include "devel_source_artifact_install_state.hpp"

#ifndef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
#error "synthetic finalization fixtures must not enter the production graph"
#endif

#include <utility>

void DevelSourceArtifactInstallFixture::mismatch(EvaluatedDevelSourceArtifactTransport& transport,
                                                 DevelSourceArtifactInstallTestMismatch mismatch) {
    auto& state = *transport.state_;
    auto& receipt = *state.receipt;
    auto& fresh = *state.fresh_binding;
    auto& selected = receipt.manifest_.artifacts.front();
    using M = DevelSourceArtifactInstallTestMismatch;
    switch(mismatch) {
        case M::BuiltLineage: receipt.built_lineage_ = std::make_shared<const unsigned char>(0); break;
        case M::ReceiptLineage: receipt.transaction_lineage_ = std::make_shared<const unsigned char>(0); break;
        case M::BindingLineage: fresh.transaction_lineage_ = std::make_shared<const unsigned char>(0); break;
        case M::PackageName: selected.package_name += "-other"; break;
        case M::PackageBase: selected.package_base += "-other"; break;
        case M::Version: selected.full_version = "999-1"; break;
        case M::Architecture: selected.architecture = "different"; break;
        case M::ArtifactIndex: selected.artifact_index = fresh.artifact_index_ = 7; break;
        case M::ArchiveDigest: selected.archive_sha256[0] = selected.archive_sha256[0] == 'a' ? 'b' : 'a'; break;
        case M::MtreeDigest: selected.raw_mtree_sha256[0] = selected.raw_mtree_sha256[0] == 'a' ? 'b' : 'a'; break;
        case M::TransactionToken: receipt.manifest_.transaction_token.assign(64, 'f'); break;
        case M::Purpose: receipt.manifest_.purpose = SourceArtifactInstallTrustedPurpose::CleanupInstallOnly; break;
        case M::StagedIdentity: fresh.staged_identity_.assign(64, 'f'); break;
        case M::Generation: fresh.snapshot_.record_generation += "different"; break;
        case M::DatabaseDigest: fresh.snapshot_.raw_database_sha256.assign(64, 'f'); break;
        case M::DescriptorIdentity: fresh.snapshot_.descriptor_identity += "different"; break;
        case M::ReceiptCardinality: receipt.manifest_.artifacts.push_back(selected); break;
        case M::BindingCardinality: state.fresh_binding_count = 2; break;
        case M::BuiltCardinality: state.built_artifact_count = 2; break;
        case M::ResourceFailure: state.fail_final_correlation = true; break;
    }
    // Keep the operation's own semantic projection internally consistent; the
    // finalizer must compare it to the separately sealed built/fresh components.
    receipt.operations_.front().artifact = receipt.manifest_.artifacts.front();
}

namespace {
template <class T>
void exchange(std::optional<T>& left, std::optional<T>& right) {
    T saved(std::move(*left));
    left.reset();
    left.emplace(std::move(*right));
    right.reset();
    right.emplace(std::move(saved));
}
} // namespace

void DevelSourceArtifactInstallFixture::exchange_receipts(EvaluatedDevelSourceArtifactTransport& left,
                                                          EvaluatedDevelSourceArtifactTransport& right,
                                                          bool include_binding) {
    exchange(left.state_->receipt, right.state_->receipt);
    if(include_binding) exchange(left.state_->fresh_binding, right.state_->fresh_binding);
}

void DevelSourceArtifactInstallFixture::replace_built(EvaluatedDevelSourceArtifactTransport& transport,
                                                      EvaluatedDevelSourceBuildProof proof) {
    // Test-only exchange of independently valid whole capabilities. Production
    // never replaces the transport's original proof, even with identical bytes.
    std::destroy_at(&transport.state_->proof);
    std::construct_at(&transport.state_->proof, std::move(proof));
}

void DevelSourceArtifactInstallFixture::exchange_bindings(EvaluatedDevelSourceArtifactTransport& left,
                                                          EvaluatedDevelSourceArtifactTransport& right) {
    exchange(left.state_->fresh_binding, right.state_->fresh_binding);
}

bool DevelSourceArtifactInstallFixture::check_component_moves(EvaluatedDevelSourceArtifactTransport& transport) {
    auto& state = *transport.state_;
    ExactArtifactTransactionReceipt receipt(std::move(*state.receipt));
    FreshInstalledArtifactBinding fresh(std::move(*state.fresh_binding));
    const bool inactive = !state.receipt->active() && !state.fresh_binding->active();
    state.receipt.reset();
    state.fresh_binding.reset();
    state.receipt.emplace(std::move(receipt));
    state.fresh_binding.emplace(std::move(fresh));
    return inactive && !receipt.active() && !fresh.active() && state.receipt->active() && state.fresh_binding->active();
}
