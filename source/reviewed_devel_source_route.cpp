#include "reviewed_devel_source_route.hpp"
#include "devel_tracking_bootstrap.hpp"
#include "srcinfo_source_metadata.hpp"
#include <fstream>
#include <iterator>
namespace {
// Syntax only selects an execution path. This does not mint evaluated source
// metadata; S4 revalidates/evaluates the reviewed pin before any S5 operation.
bool selects_reviewed_devel_execution(const ValidatedCachePath& checkout,
                                      const ReviewedDevelSourceBuildIntent* intent,
                                      bool overlay) {
    if(!intent) return false;
    if(intent->request.authoritative_devel_update || intent->request.devel_tracking_bootstrap) return true;
    if(overlay || intent->request.needed || intent->rm_deps || intent->required_targets.size() != 1) return false;
    std::ifstream file(checkout.canonical_path() / ".SRCINFO");
    if(!file) return false;
    std::string contents((std::istreambuf_iterator<char>(file)), {});
    const auto parsed = parse_srcinfo_source_metadata(contents);
    const auto package = parse_local_package_metadata(contents);
    if(!parsed.is_success() || !package.is_success() || package.metadata()->children.size() != 1) return false;
    unsigned git_count = 0;
    for(const auto& entry : parsed.metadata()->source_entries) {
        if(entry.architecture_qualifier) return false;
        const auto& source = entry.parsed_source;
        if(source.kind == ParsedSourceEntryKind::Local) continue;
        if(source.kind != ParsedSourceEntryKind::Vcs || !source.vcs ||
           source.vcs->recognized_kind != ParsedSourceVcsKind::Git || source.transport_scheme != std::optional<std::string>("https") ||
           source.vcs->query || (source.vcs->selector && source.vcs->selector->recognized_role != ParsedSourceSelectorRole::Branch)) return false;
        ++git_count;
    }
    if(git_count != 1) return false;
    // Keep intents that need AsDeps/AsExplicit on the existing legacy path.
    // The authoritative path still performs its own fresh policy recheck.
    auto session = PackageMetadataSession::open(intent->database_paths);
    const auto installed = session.query_installed_package(intent->required_targets.front().package_name);
    if(const auto* failure = std::get_if<PackageMetadataFailure>(&installed)) throw PackageMetadataError(*failure);
    const auto* metadata = std::get_if<InstalledPackageMetadata>(&installed);
    const auto desired = intent->required_targets.front().desired_reason;
    if(desired == DesiredInstallReason::Dependency && !metadata) return false;
    if(desired == DesiredInstallReason::Explicit && metadata && metadata->reason == InstalledPackageReason::Dependency) return false;
    return true;
}

} // namespace
ReviewedProductionSourceExecution select_normal_reviewed_source_execution(
    const ValidatedCachePath& checkout, PinnedReviewedSourceBuild pin,
    ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal,
    const ReviewedDevelSourceBuildIntent* intent) {
    if(intent && intent->request.devel_tracking_bootstrap) {
        const auto& trial = *intent->request.devel_tracking_bootstrap;
        std::ifstream file(checkout.canonical_path() / ".SRCINFO");
        const std::string metadata((std::istreambuf_iterator<char>(file)), {});
        if(!file || metadata != trial.source_metadata() || pin.identity().target_revision() != trial.recipe_revision()) {
            return ReviewedDevelSourceBuildRejected{ReviewedDevelSourceBuildIssue::IdentityMismatch};
        }
    }
    const bool authoritative = selects_reviewed_devel_execution(checkout, intent, pin.editor_overlay_status() != ReviewedSourceEditorOverlayStatus::None);
    if(!intent) return make_reviewed_production_artifact_source_tree(checkout, std::move(pin), outcome, abnormal);
    return prepare_reviewed_production_source_execution(authoritative ? ReviewedProductionExecutionChoice::AuthoritativeDevel : ReviewedProductionExecutionChoice::Legacy,
                                                        checkout, std::move(pin), outcome, abnormal, *intent);
}
ReviewedDevelExecutionSnapshot execute_normal_reviewed_devel(PreparedReviewedDevelSourceBuildExecution prepared) {
    auto storage = std::make_shared<std::optional<ReviewedDevelSourceBuildExecutionResult>>();
    auto executed = execute_reviewed_devel_source_build(std::move(prepared));
    if(!executed) throw std::logic_error("Reviewed devel prepared execution is inactive.");
    storage->emplace(std::move(*executed));
    ReviewedDevelExecutionSnapshot out;
    out.owner = std::shared_ptr<const ReviewedDevelSourceBuildExecutionResult>(storage, &storage->value());
    const auto& result = *out.owner;
    out.build_completed = result.build_completed();
    out.complete = reviewed_devel_execution_succeeded(result);
    if(const auto* publication = result.publication()) {
        const auto& installed = publication->installation();
        out.operation = installed.operation();
        out.pacman_exit_status = installed.pacman_exit_status();
        out.receipt = installed.receipt_state();
        out.proof = installed.proof_state();
        out.publication = publication->state();
        out.cleanup = installed.privileged_cleanup().state;
    }
    try {
        out.production_outcome = project_reviewed_devel_execution_outcome(result);
        if(const auto* p = result.publication(); p && p->installation().proof())
            out.artifact = p->installation().proof()->built_proof().artifact().evidence().identity;
    } catch(...) {
        out.projection_failed = true;
        out.complete = false;
    }
    return out;
}

ProductionSourceBuildStagedOutcome project_reviewed_devel_execution_outcome(const ReviewedDevelSourceBuildExecutionResult& result) {
    ProductionSourceBuildStagedOutcome out;
    out.source_provenance = result.source_provenance();
    if(result.build_completed())
        out.build_outcome = ProductionSourceBuildCommandOutcome::Succeeded;
    else if(result.stage() != ReviewedDevelSourceBuildStage::Intent && result.stage() != ReviewedDevelSourceBuildStage::Context && result.stage() != ReviewedDevelSourceBuildStage::Environment) {
        // The S4 invocation started; do not invent a terminal makepkg outcome
        // from an incomplete proof. Preserve the original typed S4 failure.
        out.build_outcome = ProductionSourceBuildCommandOutcome::Started;
        if(const auto* failure = result.build_failure(); failure && failure->process == EvaluatedDevelSourceBuildProcess::PackageBuild && failure->process_outcome) {
            if(const auto* exited = std::get_if<BoundedProcessExited>(&*failure->process_outcome))
                out.build_outcome = exited->exit_code == 0 ? ProductionSourceBuildCommandOutcome::Succeeded : ProductionSourceBuildCommandOutcome::Failed;
        }
    }
    if(const auto* publication = result.publication()) {
        switch(publication->installation().operation()) {
            case DevelSourceArtifactInstallOperation::Succeeded: out.install_outcome = ProductionSourceInstallOutcome::Succeeded; break;
            case DevelSourceArtifactInstallOperation::Failed: out.install_outcome = ProductionSourceInstallOutcome::Failed; break;
            case DevelSourceArtifactInstallOperation::OutcomeUnknown: out.install_outcome = ProductionSourceInstallOutcome::Started; break;
            case DevelSourceArtifactInstallOperation::NotAttempted: break;
        }
    }
    return out;
}
bool reviewed_devel_execution_succeeded(const ReviewedDevelSourceBuildExecutionResult& result) noexcept {
    if(!result.valid() || result.issue() || !result.publication()) return false;
    const auto& p = *result.publication();
    return p.state() == DevelBuildProvenancePublicationState::Complete &&
           p.installation().operation() == DevelSourceArtifactInstallOperation::Succeeded &&
           p.installation().privileged_cleanup().state == DevelSourceArtifactInstallCleanupState::Complete;
}
