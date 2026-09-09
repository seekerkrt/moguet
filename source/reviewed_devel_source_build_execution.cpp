#include "reviewed_devel_source_build_execution.hpp"

#include "package_identifier.hpp"

#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

struct ReviewedDevelSourceBuildExecutionState {
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
    std::optional<InstalledDatabaseWorldResult> world;
    std::optional<InstalledPackageQueryResult> policy_query;
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
const EvaluatedDevelSourceBuildFailure* ReviewedDevelSourceBuildExecutionResult::build_failure() const {
    const auto& value = require_state().build_failure;
    return value ? &*value : nullptr;
}
const InstalledDatabaseWorldResult* ReviewedDevelSourceBuildExecutionResult::database_world() const {
    const auto& value = require_state().world;
    return value ? &*value : nullptr;
}
const InstalledPackageQueryResult* ReviewedDevelSourceBuildExecutionResult::install_policy_observation() const {
    const auto& value = require_state().policy_query;
    return value ? &*value : nullptr;
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
    const ReviewedDevelSourceBuildIntent& intent) {
    if(!reviewed.valid()) return ReviewedDevelSourceBuildRejected{Issue::InvalidPin};
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
    if(intent.required_targets.size() != 1) return ReviewedDevelSourceBuildRejected{Issue::UnsupportedCardinality};
    const auto& target = intent.required_targets.front();
    const auto& base = reviewed.identity().package_base();
    if(!intent.request.aur_review_identity || *intent.request.aur_review_identity != base ||
       intent.request.checkout_name != base.package_base() || target.package_base != base.package_base() ||
       !is_valid_package_name(target.package_name) || (!intent.request.package_name.empty() && intent.request.package_name != target.package_name) ||
       !base.source().location().value() || intent.request.git_url != *base.source().location().value())
        return ReviewedDevelSourceBuildRejected{Issue::IdentityMismatch};
    if(target.desired_reason != DesiredInstallReason::Explicit && target.desired_reason != DesiredInstallReason::Dependency)
        return ReviewedDevelSourceBuildRejected{Issue::InvalidInstallReason};
    // Allocate/copy the outer result storage and intent before S3/S4/S5 begin.
    return PreparedReviewedDevelSourceBuildExecution(std::make_unique<ReviewedDevelSourceBuildExecutionState>(
        std::move(reviewed), intent, outcome, abnormal));
}

std::optional<ReviewedDevelSourceBuildExecutionResult> ReviewedDevelSourceBuildExecutionAuthority::execute(
    PreparedReviewedDevelSourceBuildExecution prepared) noexcept {
    if(!prepared.valid()) return std::nullopt;
    ReviewedDevelSourceBuildExecutionResult result(std::move(prepared.state_));
    auto& state = *result.state_;
    try {
        enter(state, Stage::Context);
        auto context = create_invocation_owned_source_build_context(std::move(*state.pin));
        state.pin.reset();
        if(auto* failure = std::get_if<InvocationOwnedSourceBuildContextFailure>(&context)) {
            state.context_failure.emplace(std::move(*failure));
            state.issue = Issue::ContextFailure;
            return result;
        }
        state.context.emplace(std::move(std::get<InvocationOwnedSourceBuildContext>(context)));
        state.owned_root = state.context->owned_root();
        enter(state, Stage::Environment);
        auto environment = state.context->make_makepkg_environment(state.intent.request.custom_environment, state.intent.request.empty_value_policy);
        if(auto* failure = std::get_if<InvocationOwnedSourceBuildContextFailure>(&environment)) {
            state.context_failure.emplace(std::move(*failure));
            state.issue = Issue::ContextFailure;
            return result;
        }
        enter(state, Stage::Build);
        auto built = build_evaluated_devel_source(std::move(*state.context), std::move(std::get<InvocationOwnedMakepkgEnvironment>(environment)));
        if(auto* failure = std::get_if<EvaluatedDevelSourceBuildFailure>(&built)) {
            state.build_failure.emplace(std::move(*failure));
            state.issue = Issue::BuildFailure;
            return result;
        }
        state.built.emplace(std::move(std::get<EvaluatedDevelSourceBuildProof>(built)));
        state.build_completed = true;
        enter(state, Stage::ArtifactCorrelation);
        const auto& target = state.intent.required_targets.front();
        const auto& artifact = state.built->artifact();
        if(state.built->package_base() != *state.intent.request.aur_review_identity || artifact.package().package_base() != state.built->package_base() ||
           artifact.package().package_name() != target.package_name) {
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
            // Match the ordinary artifact install reducer, with a fresh session
            // after build. S5's fixed Default/needed=false semantics are not widened.
            auto session = PackageMetadataSession::open({world->root_directory, world->database_path});
            state.policy_query.emplace(session.query_installed_package(target.package_name));
        }
        if(std::holds_alternative<PackageMetadataFailure>(*state.policy_query)) {
            state.issue = Issue::InstallPolicyFailure;
            return result;
        }
        const auto policy = map_installed_artifact_policy_state(artifact.evidence().identity, *state.policy_query);
        state.directive = resolve_install_reason_directive(target.desired_reason, policy.version_state, policy.existing_reason, false);
        if(*state.directive != InstallReasonDirective::Default) {
            state.issue = Issue::InstallReasonUnsupported;
            return result;
        }

        enter(state, Stage::Transport);
        state.transport.emplace(prepare_evaluated_devel_source_artifact_transport(std::move(*state.built)));
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
            state.policy_query.emplace(error.failure());
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
    return result;
}

ReviewedProductionSourceExecution prepare_reviewed_production_source_execution(
    ReviewedProductionExecutionChoice choice, ValidatedCachePath checkout, PinnedReviewedSourceBuild reviewed,
    ProductionReviewedSourceOutcome outcome, std::optional<ReviewedSourceAbnormalStateReason> abnormal,
    const ReviewedDevelSourceBuildIntent& intent) {
    return ReviewedDevelSourceBuildExecutionAuthority::prepare(choice, std::move(checkout), std::move(reviewed), outcome, abnormal, intent);
}
std::optional<ReviewedDevelSourceBuildExecutionResult> execute_reviewed_devel_source_build(
    PreparedReviewedDevelSourceBuildExecution prepared) noexcept {
    return ReviewedDevelSourceBuildExecutionAuthority::execute(std::move(prepared));
}
#ifdef MOGUET_ENABLE_REVIEWED_DEVEL_SOURCE_BUILD_EXECUTION_TEST_HOOKS
void set_reviewed_devel_source_build_execution_test_hooks(ReviewedDevelSourceBuildExecutionTestHooks hooks) {
    g_execution_hooks = std::move(hooks);
}
#endif
