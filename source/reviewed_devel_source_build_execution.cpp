#include "reviewed_devel_source_build_execution.hpp"

#include "package_identifier.hpp"
#include "artifact_identity_selection.hpp"
#include <set>

#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

struct ReviewedDevelSourceBuildExecutionState {
    std::optional<InvocationOwnedRecipeAcquisition> acquisition;
    std::optional<RecipeAcquisitionFailure> acquisition_failure;
    std::optional<PinnedReviewedSourceBuild> pin;
    ReviewedDevelSourceBuildIntent intent;
    ProductionSourceBuildProvenance provenance;
    ReviewedDevelSourceBuildStage stage = ReviewedDevelSourceBuildStage::Intent;
    std::optional<ReviewedDevelSourceBuildIssue> issue;
    bool build_completed = false;
    std::filesystem::path owned_root;
    std::optional<InvocationOwnedSourceBuildContext> context;
    std::optional<InvocationOwnedSourceBuildContextFailure> context_failure;
    std::optional<EvaluatedDevelSourceBuildProof> built;
    std::optional<EvaluatedDevelSourceBuildFailure> build_failure;
    std::optional<PinnedClosureFailure> closure_failure;
    std::optional<PinnedClosureReviewFailure> closure_review_failure;
    std::optional<InstalledDatabaseWorldResult> world;
    std::vector<std::pair<std::string, InstalledPackageQueryResult>> policy_queries;
    std::optional<InstallReasonDirective> directive;
    std::optional<EvaluatedDevelSourceArtifactTransport> transport;
    std::optional<DevelBuildProvenancePublicationResult> publication;

    ReviewedDevelSourceBuildExecutionState(PinnedReviewedSourceBuild reviewed, const ReviewedDevelSourceBuildIntent& value,
                                           ProductionReviewedSourceOutcome outcome,
                                           std::optional<ReviewedSourceAbnormalStateReason> abnormal)
        : pin(std::move(reviewed)), intent(value) {
        // Preserve normal review/publication dimensions before the pin moves
        // into S3. These are diagnostics, not a replacement build authority.
        provenance.review_status = ProductionSourceReviewStatus::Reviewed;
        provenance.editor_overlay = pin->editor_overlay_status();
        provenance.reviewed_upstream_base_revision = pin->reviewed_upstream_base_revision();
        provenance.publication_status = pin->publication_status();
        provenance.reviewed_outcome = outcome;
        provenance.abnormal_state_reason = abnormal;
        provenance.reviewed_state_generation = pin->published_record().generation;
    }
};

namespace {
using Stage = ReviewedDevelSourceBuildStage;
using Issue = ReviewedDevelSourceBuildIssue;
#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
ReviewedDevelSourceBuildExecutionTestHooks g_execution_hooks;
#endif

void enter(ReviewedDevelSourceBuildExecutionState& state, Stage stage) {
    state.stage = stage;
#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
    if(g_execution_hooks.before_stage) g_execution_hooks.before_stage(stage, state.built && state.built->valid() ? &*state.built : nullptr);
#endif
}

void cleanup_recipe(ReviewedDevelSourceBuildExecutionState& state) {
    if(!state.acquisition) return;
    const auto& root = state.acquisition->workspace_path();
    const auto failure = state.acquisition->cleanup();
    if(failure) {
        state.acquisition_failure.emplace(RecipeAcquisitionFailure{
            RecipeAcquisitionStage::Cleanup, failure->reason, failure->error_number,
            std::nullopt, failure, root});
        if(!state.issue) state.issue = Issue::RecipeCleanupFailure;
    }
    state.acquisition.reset();
}

std::filesystem::path comparable_database_path(std::filesystem::path path) {
    // Accept the resolver's trailing directory separator, but do not collapse
    // dot/dot-dot components across potentially different named authorities.
    if(path != path.root_path() && !path.has_filename()) path = path.parent_path();
    return path;
}

static_assert(std::is_nothrow_move_constructible_v<ReviewedDevelSourceBuildExecutionResult>);
static_assert(std::is_nothrow_move_constructible_v<DevelBuildProvenancePublicationResult>);
static_assert(std::is_nothrow_constructible_v<std::optional<DevelBuildProvenancePublicationResult>, DevelBuildProvenancePublicationResult>);
} // namespace

PreparedReviewedDevelSourceBuildExecution::PreparedReviewedDevelSourceBuildExecution(std::unique_ptr<ReviewedDevelSourceBuildExecutionState> state) noexcept
    : state_(std::move(state)) {
}
PreparedReviewedDevelSourceBuildExecution::PreparedReviewedDevelSourceBuildExecution(PreparedReviewedDevelSourceBuildExecution&&) noexcept = default;
PreparedReviewedDevelSourceBuildExecution::~PreparedReviewedDevelSourceBuildExecution() noexcept = default;
bool PreparedReviewedDevelSourceBuildExecution::valid() const noexcept {
    return state_ && state_->pin && state_->pin->valid();
}

ReviewedDevelSourceBuildExecutionResult::ReviewedDevelSourceBuildExecutionResult(std::unique_ptr<ReviewedDevelSourceBuildExecutionState> state) noexcept
    : state_(std::move(state)) {
}
ReviewedDevelSourceBuildExecutionResult::ReviewedDevelSourceBuildExecutionResult(ReviewedDevelSourceBuildExecutionResult&&) noexcept = default;
ReviewedDevelSourceBuildExecutionResult::~ReviewedDevelSourceBuildExecutionResult() noexcept = default;
bool ReviewedDevelSourceBuildExecutionResult::valid() const noexcept {
    return static_cast<bool>(state_);
}
const ReviewedDevelSourceBuildExecutionState& ReviewedDevelSourceBuildExecutionResult::require_state() const {
    if(!state_) throw std::logic_error("Inactive reviewed devel execution result.");
    return *state_;
}
Stage ReviewedDevelSourceBuildExecutionResult::stage() const {
    return require_state().stage;
}
std::optional<Issue> ReviewedDevelSourceBuildExecutionResult::issue() const {
    return require_state().issue;
}
bool ReviewedDevelSourceBuildExecutionResult::build_completed() const {
    return require_state().build_completed;
}
const std::filesystem::path& ReviewedDevelSourceBuildExecutionResult::owned_root() const {
    return require_state().owned_root;
}
const ProductionSourceBuildProvenance& ReviewedDevelSourceBuildExecutionResult::source_provenance() const {
    return require_state().provenance;
}
const InvocationOwnedSourceBuildContextFailure* ReviewedDevelSourceBuildExecutionResult::context_failure() const {
    const auto& value = require_state().context_failure;
    return value ? &*value : nullptr;
}
const RecipeAcquisitionFailure* ReviewedDevelSourceBuildExecutionResult::recipe_acquisition_failure() const {
    const auto& value = require_state().acquisition_failure;
    return value ? &*value : nullptr;
}
const EvaluatedDevelSourceBuildFailure* ReviewedDevelSourceBuildExecutionResult::build_failure() const {
    const auto& value = require_state().build_failure;
    return value ? &*value : nullptr;
}
const PinnedClosureFailure* ReviewedDevelSourceBuildExecutionResult::closure_failure() const {
    const auto& value = require_state().closure_failure;
    return value ? &*value : nullptr;
}
const PinnedClosureReviewFailure* ReviewedDevelSourceBuildExecutionResult::closure_review_failure() const {
    const auto& value = require_state().closure_review_failure;
    return value ? &*value : nullptr;
}
const InstalledDatabaseWorldResult* ReviewedDevelSourceBuildExecutionResult::database_world() const {
    const auto& value = require_state().world;
    return value ? &*value : nullptr;
}
const InstalledPackageQueryResult* ReviewedDevelSourceBuildExecutionResult::install_policy_observation() const {
    const auto& values = require_state().policy_queries;
    return values.size() == 1 ? &values.front().second : nullptr;
}
std::optional<InstallReasonDirective> ReviewedDevelSourceBuildExecutionResult::install_reason_directive() const {
    return require_state().directive;
}
const DevelBuildProvenancePublicationResult* ReviewedDevelSourceBuildExecutionResult::publication() const {
    const auto& value = require_state().publication;
    return value ? &*value : nullptr;
}

ReviewedProductionSourceExecution ReviewedDevelSourceBuildExecutionAuthority::prepare(
    ReviewedProductionExecutionChoice choice, ValidatedCachePath checkout, PinnedReviewedSourceBuild reviewed,
    ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal,
    const ReviewedDevelSourceBuildIntent& intent, InvocationOwnedRecipeAcquisition* acquisition) {
    if(!reviewed.valid()) return ReviewedDevelSourceBuildRejected{Issue::InvalidPin};
    if(static_cast<bool>(intent.request.devel_tracking_bootstrap) != (acquisition != nullptr))
        return ReviewedDevelSourceBuildRejected{Issue::IdentityMismatch};
    if(acquisition && (!intent.request.devel_tracking_bootstrap || choice != ReviewedProductionExecutionChoice::AuthoritativeDevel ||
                       acquisition->checkout().device() != checkout.device() || acquisition->checkout().inode() != checkout.inode() ||
                       acquisition->identity() != reviewed.identity()))
        return ReviewedDevelSourceBuildRejected{Issue::IdentityMismatch};
    if(choice == ReviewedProductionExecutionChoice::Legacy)
        return make_reviewed_production_artifact_source_tree(std::move(checkout), std::move(reviewed), outcome, abnormal);
    if(choice != ReviewedProductionExecutionChoice::AuthoritativeDevel) return ReviewedDevelSourceBuildRejected{Issue::UnsupportedChoice};
    if(reviewed.checkout_device() != checkout.device() || reviewed.checkout_inode() != checkout.inode())
        return ReviewedDevelSourceBuildRejected{Issue::CheckoutMismatch};
    if((outcome == ProductionReviewedSourceOutcome::AbnormalStateRebindFullReview) != abnormal.has_value())
        return ReviewedDevelSourceBuildRejected{Issue::IdentityMismatch};
    if(reviewed.editor_overlay_status() != ReviewedSourceEditorOverlayStatus::None) return ReviewedDevelSourceBuildRejected{Issue::EditorOverlay};
    if(intent.request.needed) return ReviewedDevelSourceBuildRejected{Issue::NeededRequested};
    if(intent.rm_deps) return ReviewedDevelSourceBuildRejected{Issue::DependencyCleanupRequested};
    if(intent.request.only_if_updated) return ReviewedDevelSourceBuildRejected{Issue::UpdateSelectionRequired};
    if(intent.required_targets.empty()) return ReviewedDevelSourceBuildRejected{Issue::UnsupportedCardinality};
    const auto& base = reviewed.identity().package_base();
    if(!intent.request.aur_review_identity || *intent.request.aur_review_identity != base ||
       intent.request.checkout_name != base.package_base() ||
       !base.source().location().value() || intent.request.git_url != *base.source().location().value())
        return ReviewedDevelSourceBuildRejected{Issue::IdentityMismatch};
    std::set<std::string> selected_names;
    for(const auto& target : intent.required_targets) {
        if(target.package_base != base.package_base() || !is_valid_package_name(target.package_name) ||
           !selected_names.insert(target.package_name).second ||
           (intent.required_targets.size() == 1 && !intent.request.package_name.empty() && intent.request.package_name != target.package_name))
            return ReviewedDevelSourceBuildRejected{Issue::IdentityMismatch};
        if(target.desired_reason != DesiredInstallReason::Explicit && target.desired_reason != DesiredInstallReason::Dependency)
            return ReviewedDevelSourceBuildRejected{Issue::InvalidInstallReason};
    }
    // Allocate/copy the outer result storage and intent before S3/S4/S5 begin.
    auto state = std::make_unique<ReviewedDevelSourceBuildExecutionState>(std::move(reviewed), intent, outcome, abnormal);
    // Only the successful prepared arm takes ownership. On rejection/exception
    // the caller still owns acquisition and observes its explicit cleanup.
    if(acquisition) state->acquisition.emplace(std::move(*acquisition));
    return PreparedReviewedDevelSourceBuildExecution(std::move(state));
}

std::optional<ReviewedDevelSourceBuildExecutionResult> ReviewedDevelSourceBuildExecutionAuthority::execute(
    PreparedReviewedDevelSourceBuildExecution prepared, PresentationDetail presentation_detail) noexcept {
    if(!prepared.valid()) return std::nullopt;
    ReviewedDevelSourceBuildExecutionResult result(std::move(prepared.state_));
    auto& state = *result.state_;
    try {
#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
        if(g_execution_hooks.before_execution) g_execution_hooks.before_execution(presentation_detail);
#endif
        enter(state, Stage::Context);
        auto context = create_invocation_owned_source_build_context(std::move(*state.pin));
        state.pin.reset();
        if(auto* failure = std::get_if<InvocationOwnedSourceBuildContextFailure>(&context)) {
            state.context_failure.emplace(std::move(*failure));
            state.issue = Issue::ContextFailure;
            cleanup_recipe(state);
            return result;
        }
        state.context.emplace(std::move(std::get<InvocationOwnedSourceBuildContext>(context)));
        state.owned_root = state.context->owned_root();
        // S3 has completed final reproof and owns its independent snapshot.
        // Cleanup failure stops before S4 without rolling back published R.
        if(state.acquisition) {
            enter(state, Stage::RecipeCleanup);
            cleanup_recipe(state);
            if(state.issue) return result;
        }
        enter(state, Stage::Environment);
        auto environment = state.context->make_makepkg_environment(state.intent.request.custom_environment, state.intent.request.empty_value_policy);
        if(auto* failure = std::get_if<InvocationOwnedSourceBuildContextFailure>(&environment)) {
            state.context_failure.emplace(std::move(*failure));
            state.issue = Issue::ContextFailure;
            return result;
        }
        enter(state, Stage::Build);
        auto built = [&]() -> EvaluatedDevelSourceBuildResult {
            if(!state.intent.request.devel_tracking_bootstrap &&
               !state.intent.request.ordinary_devel_package_base)
                return build_evaluated_devel_source(std::move(*state.context), std::move(std::get<InvocationOwnedMakepkgEnvironment>(environment)));
            // Bootstrap and ordinary authoritative updates share the exact
            // closure owner. Ordinary intent is not a Missing-baseline trial:
            // retain existing provenance and independently acquire/review the
            // build input instead of adopting a planning OID or an old cache.
            auto selected = select_evaluated_devel_source(std::move(*state.context), std::move(std::get<InvocationOwnedMakepkgEnvironment>(environment)));
            if(auto* failure = std::get_if<EvaluatedDevelSourceBuildFailure>(&selected)) return std::move(*failure);
            auto closure = acquire_pinned_submodule_closure(std::get<EvaluatedDevelSourceSelection>(std::move(selected)), presentation_detail);
            EvaluatedDevelSourceBuildFailure stopped;
            stopped.stage = EvaluatedDevelSourceBuildStage::SourceWorkspace;
            stopped.reason = EvaluatedDevelSourceBuildFailureReason::SourceReadyInvalid;
            if(auto* failure = std::get_if<PinnedClosureFailure>(&closure)) {
                state.closure_failure.emplace(std::move(*failure));
                return stopped;
            }
            auto accepted = review_pinned_submodule_closure(std::get<InvocationOwnedPinnedSubmoduleClosure>(std::move(closure)),
                                                            ReviewPolicy::Prompt, state.intent.execution_options.no_confirm);
            if(auto* failure = std::get_if<PinnedClosureReviewFailure>(&accepted)) {
                state.closure_review_failure.emplace(std::move(*failure));
                return stopped;
            }
            auto ready = materialize_pinned_submodule_workspace(std::get<AcceptedPinnedSubmoduleClosure>(std::move(accepted)));
            if(auto* failure = std::get_if<PinnedWorkspaceFailure>(&ready)) {
                stopped.pinned_workspace_failure = std::make_shared<PinnedWorkspaceFailure>(std::move(*failure));
                return stopped;
            }
            return resume_evaluated_devel_source(std::get<SourceReadyPinnedSubmoduleWorkspace>(std::move(ready)));
        }();
        if(auto* failure = std::get_if<EvaluatedDevelSourceBuildFailure>(&built)) {
            state.build_failure.emplace(std::move(*failure));
            state.issue = Issue::BuildFailure;
            return result;
        }
        state.built.emplace(std::move(std::get<EvaluatedDevelSourceBuildProof>(built)));
        state.build_completed = true;
        enter(state, Stage::ArtifactCorrelation);
        if(state.built->declared_children().size() > 1 && !state.intent.request.devel_tracking_bootstrap &&
           !state.intent.request.ordinary_devel_package_base) {
            state.issue = Issue::UnsupportedCardinality;
            return result;
        }
        const auto selection = correlate_package_base_artifact_identities(
            state.built->package_base().package_base(), state.intent.required_targets, query_artifact_package_identities(*state.built));
        if(state.built->package_base() != *state.intent.request.aur_review_identity || !selection.is_success() ||
           selection.success()->selected_artifacts.empty()) {
            state.issue = Issue::ArtifactMismatch;
            return result;
        }
        enter(state, Stage::InstallPolicy);
        state.world.emplace(resolve_trusted_installed_database_world());
        const auto* world = std::get_if<InstalledDatabaseWorld>(&*state.world);
        if(!world) {
            state.issue = Issue::DatabaseWorldUnavailable;
            return result;
        }
        if(comparable_database_path(state.intent.database_paths.root_dir) != comparable_database_path(world->root_directory) ||
           comparable_database_path(state.intent.database_paths.db_path) != comparable_database_path(world->database_path)) {
            state.issue = Issue::DatabaseWorldMismatch;
            return result;
        }
        {
            auto session = PackageMetadataSession::open({world->root_directory, world->database_path});
            for(const auto& selected : selection.success()->selected_artifacts)
                state.policy_queries.emplace_back(selected.identity.package_name, session.query_installed_package(selected.identity.package_name));
        }
        for(const auto& selected : selection.success()->selected_artifacts) {
            const auto query = std::find_if(state.policy_queries.begin(), state.policy_queries.end(),
                                            [&](const auto& value) { return value.first == selected.identity.package_name; });
            if(query == state.policy_queries.end() || std::holds_alternative<PackageMetadataFailure>(query->second)) {
                state.issue = Issue::InstallPolicyFailure;
                return result;
            }
            const auto policy = map_installed_artifact_policy_state(selected.identity, query->second);
            state.directive = resolve_install_reason_directive(selected.desired_reason, policy.version_state, policy.existing_reason, false);
            // Existing Default/needed=false preserves each installed reason in
            // the shared transaction. No promotion or dependency install is inferred.
            if(*state.directive != InstallReasonDirective::Default) {
                state.issue = Issue::InstallReasonUnsupported;
                return result;
            }
        }

        enter(state, Stage::Transport);
        state.transport.emplace(prepare_evaluated_devel_source_artifact_transport(std::move(*state.built), state.intent.required_targets));
        try {
#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
            if(g_execution_hooks.exact_transaction_token)
                static_cast<void>(state.transport->execute_exact_for_test(state.intent.execution_options, *g_execution_hooks.exact_transaction_token));
            else
#endif
                static_cast<void>(state.transport->execute_exact(state.intent.execution_options));
        } catch(const std::bad_alloc&) {
            state.issue = Issue::ResourceFailure;
        } catch(const std::length_error&) {
            state.issue = Issue::ResourceFailure;
        } catch(...) {
            // S5 retains fixed operation facts even if a diagnostic return copy
            // fails. Finalize that same state once; never execute another path.
            state.issue = Issue::InternalFailure;
        }
        auto installed = state.transport->finalize();
        if(!installed) {
            state.issue = Issue::InvalidFinalization;
            return result;
        }
        // From here only sealed ownership moves/primitive assignments remain.
        // S6 owns all fallible projection/publication while retaining S5 facts.
        state.stage = Stage::Publication;
        auto publication = publish_installed_devel_source_build(std::move(*installed));
        if(publication)
            state.publication.emplace(std::move(*publication));
        else
            state.issue = Issue::InvalidFinalization;
        state.stage = Stage::Finished;
        return result;
    } catch(const PackageMetadataError& error) {
        state.issue = Issue::InstallPolicyFailure;
        try {
            state.policy_queries.emplace_back(std::string{}, error.failure());
        } catch(...) {
            state.issue = Issue::ResourceFailure;
        }
    } catch(const std::bad_alloc&) {
        state.issue = Issue::ResourceFailure;
    } catch(const std::length_error&) {
        state.issue = Issue::ResourceFailure;
    } catch(...) {
        state.issue = Issue::InternalFailure;
    }
    try {
        cleanup_recipe(state);
    } catch(...) {
        // Resource failure must not turn a failed execution into success.
        state.issue = Issue::ResourceFailure;
    }
    return result;
}

ReviewedProductionSourceExecution prepare_reviewed_production_source_execution(
    ReviewedProductionExecutionChoice choice, ValidatedCachePath checkout, PinnedReviewedSourceBuild reviewed,
    ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal,
    const ReviewedDevelSourceBuildIntent& intent, InvocationOwnedRecipeAcquisition* acquisition) {
    return ReviewedDevelSourceBuildExecutionAuthority::prepare(choice, std::move(checkout), std::move(reviewed), outcome, abnormal, intent, acquisition);
}
std::optional<ReviewedDevelSourceBuildExecutionResult> execute_reviewed_devel_source_build(
    PreparedReviewedDevelSourceBuildExecution prepared, PresentationDetail presentation_detail) noexcept {
    return ReviewedDevelSourceBuildExecutionAuthority::execute(std::move(prepared), presentation_detail);
}
#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
void set_reviewed_devel_source_build_execution_test_hooks(ReviewedDevelSourceBuildExecutionTestHooks hooks) {
    g_execution_hooks = std::move(hooks);
}
#endif
