#include "installed_artifact_binding_observer_authority.hpp"

#include "evaluated_devel_source_build.hpp"
#include "exact_artifact_transaction_receipt.hpp"
#include "fresh_installed_artifact_binding.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace {
using Issue = InstalledRecordObservationIssue;

const InstalledPackageRecordObservation& selected_observation(
    const ExactArtifactRecordObservationsResult& observations,
    const SourceArtifactInstallRootArtifactExpectation& selected, Issue missing) {
    if(const auto* issue = std::get_if<Issue>(&observations)) throw *issue;
    const auto& records = std::get<ExactArtifactRecordObservations>(observations);
    const auto found = std::find_if(records.begin(), records.end(), [&](const auto& record) {
        return record.artifact_index == selected.artifact_index && record.package_name == selected.package_name;
    });
    if(found == records.end()) throw missing;
    if(const auto* issue = std::get_if<Issue>(&found->observation)) throw *issue;
    return found->observation;
}

void require_semantic_identity(const InstalledPackageRecordSnapshot& observed,
                               const SourceArtifactInstallRootArtifactExpectation& expected) {
    if(observed.package_name != expected.package_name || observed.package_base != expected.package_base ||
       observed.full_version != expected.full_version || observed.architecture != expected.architecture)
        throw Issue::MetadataMismatch;
}
} // namespace

ExactArtifactTransactionReceipt::ExactArtifactTransactionReceipt(
    SourceArtifactInstallRootPrepareRequest manifest, std::string stage,
    ExactArtifactOperationRecords operations, ExactArtifactRootEvidence evidence) noexcept
    : manifest_(std::move(manifest)), staged_identity_sha256_(std::move(stage)),
      operations_(std::move(operations)), evidence_(std::move(evidence)) {
}

const SourceArtifactInstallRootPrepareRequest& ExactArtifactTransactionReceipt::manifest() const noexcept {
    return manifest_;
}
const std::string& ExactArtifactTransactionReceipt::staged_identity_sha256() const noexcept {
    return staged_identity_sha256_;
}
const ExactArtifactOperationRecords& ExactArtifactTransactionReceipt::operations() const noexcept {
    return operations_;
}
const ExactArtifactRootEvidence& ExactArtifactTransactionReceipt::database_evidence() const noexcept {
    return evidence_;
}

FreshInstalledArtifactBinding::FreshInstalledArtifactBinding(InstalledArtifactBinding binding, std::string token, std::size_t index) noexcept
    : binding_(std::move(binding)), transaction_token_(std::move(token)), artifact_index_(index) {
}
const InstalledArtifactBinding& FreshInstalledArtifactBinding::binding() const noexcept {
    return binding_;
}
const std::string& FreshInstalledArtifactBinding::transaction_token() const noexcept {
    return transaction_token_;
}
std::size_t FreshInstalledArtifactBinding::artifact_index() const noexcept {
    return artifact_index_;
}

FreshInstalledArtifactBindingObservation InstalledArtifactBindingObserver::observe(
    const ExactArtifactTransactionReceipt& receipt, const EvaluatedDevelSourceBuildProof& built) noexcept {
    try {
        // The generic transport/receipt retains N records. This live source
        // binding consumes the already-fixed one-artifact Slice 4 subset only.
        if(!built.valid() || receipt.operations().size() != 1 || receipt.manifest().artifacts.size() != 1)
            throw Issue::MetadataMismatch;
        const auto& operation = receipt.operations().front();
        const auto& selected = operation.artifact;
        const auto& artifact = built.artifact();
        const auto& expected = artifact.evidence();
        if(selected != receipt.manifest().artifacts.front() ||
           selected.package_name != expected.identity.package_name || selected.full_version != expected.identity.full_version ||
           !expected.identity.package_base.value() || selected.package_base != *expected.identity.package_base.value() ||
           !expected.identity.architecture.value() || selected.architecture != *expected.identity.architecture.value() ||
           selected.archive_sha256 != expected.archive_digest.value() || selected.raw_mtree_sha256 != expected.mtree_digest.value())
            throw Issue::MetadataMismatch;
        const auto& evidence = receipt.database_evidence();
        if(const auto* issue = std::get_if<Issue>(&evidence.world)) throw *issue;
        const auto& world = std::get<InstalledDatabaseWorld>(evidence.world);
        const auto current_world = resolve_trusted_installed_database_world();
        if(const auto* issue = std::get_if<Issue>(&current_world)) throw *issue;
        if(std::get<InstalledDatabaseWorld>(current_world) != world) throw Issue::DatabaseWorldMismatch;

        const auto& before = selected_observation(evidence.baseline, selected, Issue::MissingBaseline);
        const auto& anchors = operation.operation == ExactArtifactTransactionOperation::Install
                                  ? evidence.install_anchors
                                  : evidence.upgrade_anchors;
        const auto& anchor_observation = selected_observation(anchors, selected, Issue::MissingAnchor);
        const auto* anchor = std::get_if<InstalledPackageRecordSnapshot>(&anchor_observation);
        if(!anchor) throw Issue::MissingPackage;
        require_semantic_identity(*anchor, selected);
        if(anchor->raw_mtree_sha256 != expected.mtree_digest.value()) throw Issue::MtreeMismatch;
        switch(operation.operation) {
            case ExactArtifactTransactionOperation::Install:
                if(!std::holds_alternative<InstalledPackageRecordAbsent>(before)) throw Issue::OperationMismatch;
                break;
            case ExactArtifactTransactionOperation::Upgrade: {
                const auto* previous = std::get_if<InstalledPackageRecordSnapshot>(&before);
                if(!previous || previous->package_name != selected.package_name) throw Issue::OperationMismatch;
                if(previous->record_generation == anchor->record_generation) throw Issue::GenerationMismatch;
                break;
            }
            default: throw Issue::OperationMismatch;
        }

        // This call constructs a new ALPM handle and retains all queried DB
        // objects through the raw read and generation reproof. Anchor alone
        // cannot mint a live binding; a decoded historical binding is not input.
        const auto observation = observe_installed_package_record(world, selected.package_name);
        if(const auto* issue = std::get_if<Issue>(&observation)) throw *issue;
        const auto* fresh = std::get_if<InstalledPackageRecordSnapshot>(&observation);
        if(!fresh) throw Issue::MissingPackage;
        require_semantic_identity(*fresh, selected);
        if(fresh->record_generation != anchor->record_generation) throw Issue::GenerationMismatch;
        if(fresh->raw_mtree_sha256 != anchor->raw_mtree_sha256 || fresh->raw_mtree_sha256 != expected.mtree_digest.value())
            throw Issue::MtreeMismatch;
        if(fresh->raw_database_sha256 != anchor->raw_database_sha256) throw Issue::RecordDigestMismatch;
        if(fresh->descriptor_identity != anchor->descriptor_identity) throw Issue::RecordChanged;
        // Re-resolve after observation as well: a different fixed configuration
        // world cannot be adopted through otherwise identical package metadata.
        const auto after_world = resolve_trusted_installed_database_world();
        if(const auto* issue = std::get_if<Issue>(&after_world)) throw *issue;
        if(std::get<InstalledDatabaseWorld>(after_world) != world) throw Issue::DatabaseWorldMismatch;
        auto binding = InstalledArtifactBinding::make(
            artifact.package(), PackageVersionIdentity::composite(fresh->full_version),
            InstalledPackageArchitectureIdentity::known(fresh->architecture),
            AlpmMtreeSha256Digest::make(fresh->raw_mtree_sha256),
            InstalledDatabaseRecordSha256Digest::make(fresh->raw_database_sha256),
            InstalledPackageRecordGeneration(InstalledPackageRecordGenerationScheme::LinuxNameToHandleAt, fresh->record_generation));
        return FreshInstalledArtifactBinding(std::move(binding), receipt.manifest().transaction_token, selected.artifact_index);
    } catch(Issue issue) {
        return FreshInstalledArtifactBindingFailure{issue};
    } catch(const std::bad_alloc&) {
        return FreshInstalledArtifactBindingFailure{Issue::ResourceFailure};
    } catch(...) {
        return FreshInstalledArtifactBindingFailure{Issue::MalformedMetadata};
    }
}
