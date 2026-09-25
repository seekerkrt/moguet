#include "local_recipe_candidate.hpp"

#include "local_source_metadata_evaluation.hpp"
#include "logging.hpp"
#include "process.hpp"

#include <cstdio>
#include <memory>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <utility>

struct LocalRecipeCandidateAccess final {
    static int directory_descriptor(const LocalSourceRoot& root) noexcept {
        return root.directory_descriptor_;
    }
    static void restore_recipe_mode(const LocalSourceRoot& root, std::uintmax_t mode) {
        root.require_unchanged_identity();
        if(::fchmod(root.pkgbuild_descriptor_, static_cast<mode_t>(mode)) != 0)
            throw std::runtime_error("local-recipe-mode-preservation-failed");
    }
};

namespace {

constexpr std::size_t MAX_PATCH_BYTES = 16U * 1024U * 1024U;
constexpr std::size_t MAX_SERIES_BYTES = 64U * 1024U * 1024U;
constexpr std::size_t MAX_SERIES_ENTRIES = 64;

struct InputCloser {
    void operator()(std::FILE* input) const noexcept {
        static_cast<void>(std::fclose(input));
    }
};

// This is an envelope/shape guard, not a hunk replay implementation. Count
// framing prevents a second traditional diff hiding after a hunk from reaching
// Git. Actual context matching and application remain Git's responsibility.
std::optional<LocalRecipeCandidateFailureReason> validate_patch(
    const std::string& bytes) {
    using Reason = LocalRecipeCandidateFailureReason;
    if(bytes.empty() || bytes.size() > MAX_PATCH_BYTES ||
       bytes.find('\0') != std::string::npos || bytes.back() != '\n') {
        return Reason::InvalidMaterial;
    }
    std::istringstream input(bytes);
    std::string line;
    if(!std::getline(input, line) ||
       line != "diff --git a/PKGBUILD b/PKGBUILD") {
        return Reason::UnsupportedPatch;
    }
    if(!std::getline(input, line)) return Reason::InvalidMaterial;
    if(line.starts_with("index ")) {
        if(line.size() > 160) return Reason::InvalidMaterial;
        const std::regex index_header(
            R"(index [0-9a-f]+\.\.[0-9a-f]+( 100(644|755))?)");
        if(!std::regex_match(line, index_header)) return Reason::UnsupportedPatch;
        if(!std::getline(input, line)) return Reason::InvalidMaterial;
    }
    if(line != "--- a/PKGBUILD" || !std::getline(input, line) ||
       line != "+++ b/PKGBUILD") return Reason::UnsupportedPatch;

    const std::regex hunk_header(
        R"(@@ -([0-9]+)(,([0-9]+))? \+([0-9]+)(,([0-9]+))? @@.*)");
    std::size_t old_remaining = 0;
    std::size_t new_remaining = 0;
    std::size_t context = 0;
    bool saw_hunk = false;
    bool may_mark_newline = false;
    while(std::getline(input, line)) {
        if(line == "\\ No newline at end of file" && may_mark_newline) {
            may_mark_newline = false;
            continue;
        }
        if(old_remaining == 0 && new_remaining == 0) {
            if(line.size() > 1024) return Reason::InvalidMaterial;
            if(saw_hunk && context == 0) return Reason::UnsupportedPatch;
            std::smatch match;
            if(!std::regex_match(line, match, hunk_header))
                return Reason::UnsupportedPatch;
            try {
                old_remaining = match[3].matched ? std::stoull(match[3]) : 1;
                new_remaining = match[6].matched ? std::stoull(match[6]) : 1;
            } catch(...) {
                return Reason::InvalidMaterial;
            }
            if(old_remaining > bytes.size() || new_remaining > bytes.size())
                return Reason::InvalidMaterial;
            context = 0;
            saw_hunk = true;
            may_mark_newline = false;
            continue;
        }
        if(line.empty()) return Reason::InvalidMaterial;
        if(line[0] == ' ' || line[0] == '-') {
            if(old_remaining == 0) return Reason::InvalidMaterial;
            --old_remaining;
        }
        if(line[0] == ' ' || line[0] == '+') {
            if(new_remaining == 0) return Reason::InvalidMaterial;
            --new_remaining;
        }
        if(line[0] == ' ')
            ++context;
        else if(line[0] != '+' && line[0] != '-')
            return Reason::InvalidMaterial;
        may_mark_newline = true;
    }
    if(!saw_hunk || old_remaining != 0 || new_remaining != 0)
        return Reason::InvalidMaterial;
    if(context == 0) return Reason::UnsupportedPatch;
    return std::nullopt;
}

void apply_patch(const LocalRecipePatch& patch, const LocalSourceRoot& candidate,
                 LocalRecipeCandidateFailure& failure) {
    failure.reason = LocalRecipeCandidateFailureReason::ToolFailure;
    // An unlinked invocation-owned input retains exactly the selected bytes
    // for both check and apply. No external material path is reopened.
    std::unique_ptr<std::FILE, InputCloser> input(std::tmpfile());
    if(!input || std::fwrite(patch.bytes.data(), 1, patch.bytes.size(), input.get()) != patch.bytes.size() || std::fflush(input.get()) != 0) {
        throw std::runtime_error("local-recipe-patch-input-failed");
    }
    for(const bool check_only : {true, false}) {
        if(std::fseek(input.get(), 0, SEEK_SET) != 0)
            throw std::runtime_error("local-recipe-patch-input-rewind-failed");
        candidate.require_unchanged_identity();
        std::vector<std::string> arguments{
            "-c", "apply.ignoreWhitespace=no", "apply", "-p1", "--whitespace=nowarn"};
        if(check_only) arguments.push_back("--check");
        arguments.push_back("-");
        // No inherited Git directory/config/index authority, including a .git
        // copied from the local source. Apply changes only recipe files.
        ExplicitProcessInvocation invocation{
            "/usr/bin/git", std::move(arguments), {"PATH=/usr/bin:/bin", "LC_ALL=C", "GIT_DIR=/dev/null", "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null"}, 4096, LocalRecipeCandidateAccess::directory_descriptor(candidate), ::fileno(input.get())};
        Logger::raw_cmd(check_only ? "git apply -p1 --whitespace=nowarn --check -"
                                   : "git apply -p1 --whitespace=nowarn -");
        const auto result = capture_explicit_process_output_raw(invocation, true);
        if(result.exit_code != 0 || result.stdout_capture_limit_exceeded) {
            failure.tool_exit_code = result.exit_code;
            throw std::runtime_error("local-recipe-patch-tool-failed");
        }
    }
}

void require_same_children(const LocalPackageMetadata& before,
                           const LocalPackageMetadata& after) {
    if(before.package_base != after.package_base ||
       before.children.size() != after.children.size())
        throw std::runtime_error("local-recipe-package-identity-changed");
    for(std::size_t i = 0; i < before.children.size(); ++i) {
        if(before.children[i].name != after.children[i].name)
            throw std::runtime_error("local-recipe-child-identity-changed");
    }
}

void cleanup_failure_workspace(std::optional<LocalSourceWorkspace>& workspace,
                               LocalRecipeCandidateFailure& failure) {
    if(!workspace) return;
    try {
        workspace->cleanup();
    } catch(const LocalSourceWorkspaceError& error) {
        failure.cleanup_failure = error.failure();
    } catch(...) {
        failure.cleanup_failure = LocalSourceWorkspaceFailure{
            LocalSourceWorkspaceStage::Cleanup,
            LocalSourceWorkspaceErrorCode::CleanupFailure,
            {},
            std::nullopt};
    }
}

} // namespace

std::optional<LocalRecipeCandidateFailureReason> validate_local_recipe_patch(const std::string& bytes) {
    return validate_patch(bytes);
}

LocalRecipeCandidateError::LocalRecipeCandidateError(
    LocalRecipeCandidateFailure failure, const std::string& diagnostic)
    : std::runtime_error(diagnostic), failure_(std::move(failure)) {
}

const LocalRecipeCandidateFailure& LocalRecipeCandidateError::failure() const noexcept {
    return failure_;
}

PreparedLocalRecipeBuild::PreparedLocalRecipeBuild(
    LocalSourceRoot original, PackageBaseIdentity identity,
    LocalSourceFileSnapshot prepatch, LocalSourceBuildRequest request,
    LocalSourceWorkspace& workspace)
    : original_(std::move(original)), identity_(std::move(identity)),
      prepatch_(std::move(prepatch)),
      build_(PreparedLocalSourceBuild::from_recipe_candidate(
          std::move(request), workspace)) {
}

const PackageBaseIdentity& PreparedLocalRecipeBuild::source_identity() const noexcept {
    return identity_;
}
const LocalSourceFileSnapshot& PreparedLocalRecipeBuild::prepatch_candidate() const noexcept {
    return prepatch_;
}
const LocalSourceFileSnapshot& PreparedLocalRecipeBuild::modified_candidate() const noexcept {
    return build_.request_.source_root.pkgbuild();
}
const LocalSourceBuildMetadata& PreparedLocalRecipeBuild::metadata() const noexcept {
    return build_.request_.metadata;
}
const LocalBuildPlan& PreparedLocalRecipeBuild::plan() const noexcept {
    return build_.request_.build_plan;
}
void PreparedLocalRecipeBuild::require_unchanged_identity() const {
    original_.require_unchanged_identity();
    if(!build_.recipe_workspace_) throw std::logic_error("local-recipe-already-consumed");
    build_.recipe_workspace_->require_unchanged_identity();
    build_.request_.metadata.require_matches(build_.request_.source_root);
}

PreparedLocalRecipeBuild prepare_local_recipe_build(
    LocalSourceRoot original, ValidatedCacheRoot cache_root,
    SourceBuildEnvironment environment, std::vector<LocalRecipePatch> patches,
    ArtifactMakepkgBuildOptions options,
    const ProviderSelectionCallback& select_provider,
    std::optional<PackageBaseIdentity> expected_source) {
    using Phase = LocalRecipeCandidatePhase;
    using Reason = LocalRecipeCandidateFailureReason;
    LocalRecipeCandidateFailure failure{
        Phase::Preflight, Reason::InvalidMaterial, std::vector<LocalRecipePatchOutcome>(patches.size(), LocalRecipePatchOutcome::NotAttempted), std::nullopt, std::nullopt, {}};
    std::optional<LocalSourceWorkspace> workspace;
    try {
        std::size_t total = 0;
        if(patches.empty() || patches.size() > MAX_SERIES_ENTRIES)
            throw std::invalid_argument("local-recipe-invalid-series");
        for(std::size_t i = 0; i < patches.size(); ++i) {
            const auto& patch = patches[i];
            if(patch.bytes.size() > MAX_PATCH_BYTES || total > MAX_SERIES_BYTES - patch.bytes.size())
                throw std::invalid_argument("local-recipe-series-limit");
            total += patch.bytes.size();
            if(const auto invalid = validate_patch(patch.bytes)) {
                failure.reason = *invalid;
                failure.rejected_patch_index = i;
                throw std::invalid_argument("local-recipe-patch-shape-rejected");
            }
        }
        failure.reason = Reason::PreparationFailure;
        require_unclaimed_artifact_pkgdest(environment);
        const std::string architecture = resolve_local_source_effective_architecture(environment);
        original.require_unchanged_identity();
        if(expected_source && expected_source->source() != PackageSourceIdentity::local(
                                                               SourceLocationIdentity::known_local_path(original.canonical_path().string()))) {
            failure.phase = Phase::Identity;
            failure.reason = Reason::IdentityChanged;
            throw std::runtime_error("local-recipe-source-association-mismatch");
        }
        failure.phase = Phase::Snapshot;
        failure.reason = Reason::PreparationFailure;
        workspace.emplace(materialize_local_source_workspace(original, cache_root));
        failure.candidate_path = workspace->path();
        LocalSourceRoot before = open_local_source_root(workspace->path(), true);
        const LocalSourceFileSnapshot prepatch = before.pkgbuild();
        LocalSourceFileSnapshot expected = prepatch;
        failure.phase = Phase::PrepatchMetadata;
        failure.reason = Reason::MetadataFailure;
        const auto baseline = evaluate_local_source_metadata(before, environment, architecture);
        PackageBaseIdentity identity = PackageBaseIdentity::make(
            PackageSourceIdentity::local(SourceLocationIdentity::known_local_path(
                original.canonical_path().string())),
            baseline.metadata().package_base);

        failure.phase = Phase::Identity;
        failure.reason = Reason::IdentityChanged;
        if(expected_source && *expected_source != identity)
            throw std::runtime_error("local-recipe-association-mismatch");

        // Each root snapshot expires on successful apply. Reopen only after a
        // known tool success; a failed/unknown mutator never produces authority.
        for(std::size_t i = 0; i < patches.size(); ++i) {
            failure.phase = Phase::Apply;
            failure.reason = Reason::CandidateChanged;
            failure.patches[i] = LocalRecipePatchOutcome::Failed;
            workspace->require_unchanged_identity();
            LocalSourceRoot current = open_local_source_root(workspace->path(), true);
            if(current.pkgbuild() != expected)
                throw std::runtime_error("local-recipe-prepatch-changed");
            apply_patch(patches[i], current, failure);
            failure.reason = Reason::CandidateChanged;
            workspace->require_unchanged_identity();
            LocalSourceRoot next = open_local_source_root(workspace->path(), true);
            if(next.directory_identity() != before.directory_identity())
                throw std::runtime_error("local-recipe-candidate-identity-changed");
            // Git may recreate a 0600 file as 0644. Retain the original recipe
            // mode through this owned, descriptor-validated replacement only.
            if(next.pkgbuild().identity.mode != prepatch.identity.mode)
                LocalRecipeCandidateAccess::restore_recipe_mode(next, prepatch.identity.mode);
            expected = open_local_source_root(workspace->path(), true).pkgbuild();
            failure.patches[i] = LocalRecipePatchOutcome::Applied;
        }
        failure.phase = Phase::PostpatchMetadata;
        failure.reason = Reason::CandidateChanged;
        LocalSourceRoot modified = open_local_source_root(workspace->path(), true);
        if(modified.pkgbuild() != expected)
            throw std::runtime_error("local-recipe-modified-candidate-changed");
        failure.reason = Reason::MetadataFailure;
        auto metadata = evaluate_local_source_metadata(modified, environment, architecture);
        failure.phase = Phase::Identity;
        failure.reason = Reason::IdentityChanged;
        require_same_children(baseline.metadata(), metadata.metadata());
        original.require_unchanged_identity();
        workspace->require_unchanged_identity();
        failure.phase = Phase::Plan;
        failure.reason = Reason::PlanFailure;
        auto plan = resolve_local_build_plan(
            metadata.metadata(), architecture,
            PackageRelationLocalSourceIdentity{
                original.canonical_path(), original.directory_identity().device,
                original.directory_identity().inode},
            select_provider);
        return PreparedLocalRecipeBuild(
            std::move(original), std::move(identity), prepatch,
            LocalSourceBuildRequest{std::move(modified), std::move(plan), std::move(cache_root),
                                    std::move(metadata), options},
            *workspace);
    } catch(const std::exception& error) {
        cleanup_failure_workspace(workspace, failure);
        throw LocalRecipeCandidateError(std::move(failure), error.what());
    } catch(...) {
        cleanup_failure_workspace(workspace, failure);
        throw LocalRecipeCandidateError(std::move(failure), "local-recipe-candidate-unknown-failure");
    }
}

LocalSourceBuildResult execute_local_recipe_build(PreparedLocalRecipeBuild prepared) {
    return prepared.execute();
}

LocalSourceBuildResult PreparedLocalRecipeBuild::execute() {
    try {
        require_unchanged_identity();
    } catch(const std::exception& error) {
        LocalRecipeCandidateFailure failure{
            LocalRecipeCandidatePhase::Preflight,
            LocalRecipeCandidateFailureReason::CandidateChanged,
            {},
            std::nullopt,
            std::nullopt,
            {}};
        cleanup_failure_workspace(build_.recipe_workspace_, failure);
        throw LocalSourceBuildPhaseError(
            LocalSourceBuildFailurePhase::Preflight, error.what(),
            std::nullopt, std::nullopt, std::nullopt, std::nullopt,
            failure.cleanup_failure);
    }
    return execute_prepared_local_source_build(std::move(build_));
}
