#include "local_recipe_candidate.hpp"

#include "local_source_metadata_evaluation.hpp"
#include "logging.hpp"
#include "process.hpp"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <regex>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

struct LocalRecipeCandidateAccess final {
    static int directory_descriptor(const LocalSourceRoot& root) noexcept {
        return root.directory_descriptor_;
    }
    static void restore_recipe_mode(const LocalSourceRoot& root, const std::string& target,
                                    std::uintmax_t mode) {
        root.require_unchanged_identity();
        if(target == "PKGBUILD") {
            if(::fchmod(root.pkgbuild_descriptor_, static_cast<mode_t>(mode)) != 0)
                throw std::runtime_error("local-recipe-mode-preservation-failed");
            return;
        }
        const auto snapshot = snapshot_supported_recipe_files(root);
        const auto found = std::find_if(snapshot.begin(), snapshot.end(), [&](const auto& file) {
            return file.relative_path == target;
        });
        if(found == snapshot.end()) throw std::runtime_error("local-recipe-mode-target-missing");
        const int descriptor = ::openat(root.directory_descriptor_, target.c_str(),
                                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if(descriptor < 0) throw std::runtime_error("local-recipe-mode-target-open-failed");
        struct DescriptorCloser {
            int value;
            ~DescriptorCloser() {
                static_cast<void>(::close(value));
            }
        } held{descriptor};
        struct stat opened{};
        struct stat named{};
        if(::fstat(descriptor, &opened) != 0 ||
           ::fstatat(root.directory_descriptor_, target.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
           !S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
           opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
           static_cast<std::uintmax_t>(opened.st_dev) != found->file.identity.device ||
           static_cast<std::uintmax_t>(opened.st_ino) != found->file.identity.inode ||
           static_cast<std::uintmax_t>(opened.st_uid) != found->file.identity.owner)
            throw std::runtime_error("local-recipe-mode-target-changed");
        if(::fchmod(descriptor, static_cast<mode_t>(mode)) != 0)
            throw std::runtime_error("local-recipe-mode-preservation-failed");
    }
};

namespace {

constexpr std::size_t MAX_SERIES_BYTES = 64U * 1024U * 1024U;
constexpr std::size_t MAX_SERIES_ENTRIES = 64;

struct InputCloser {
    void operator()(std::FILE* input) const noexcept {
        static_cast<void>(std::fclose(input));
    }
};

bool git_path_needs_quotes(std::string_view path) {
    return std::any_of(path.begin(), path.end(), [](unsigned char byte) {
        return byte < 0x20 || byte >= 0x7f || byte == '"' || byte == '\\';
    });
}

std::string git_patch_path(std::string_view path) {
    if(!git_path_needs_quotes(path)) return std::string(path);
    std::string result{"\""};
    for(const unsigned char byte : path) {
        switch(byte) {
            case '\a': result += "\\a"; break;
            case '\b': result += "\\b"; break;
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\v': result += "\\v"; break;
            case '\f': result += "\\f"; break;
            case '\r': result += "\\r"; break;
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            default:
                if(byte < 0x20 || byte >= 0x7f) {
                    result.push_back('\\');
                    result.push_back(static_cast<char>('0' + ((byte >> 6) & 7)));
                    result.push_back(static_cast<char>('0' + ((byte >> 3) & 7)));
                    result.push_back(static_cast<char>('0' + (byte & 7)));
                } else {
                    result.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    result.push_back('"');
    return result;
}

std::optional<std::pair<std::string, std::size_t>> decode_git_quoted_path(
    std::string_view line, std::size_t offset) {
    if(offset >= line.size() || line[offset] != '"') return std::nullopt;
    std::string result;
    for(std::size_t i = offset + 1; i < line.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(line[i]);
        if(byte == '"') return std::pair(std::move(result), i + 1);
        if(byte != '\\') {
            result.push_back(static_cast<char>(byte));
            continue;
        }
        if(++i >= line.size()) return std::nullopt;
        const char escaped = line[i];
        switch(escaped) {
            case 'a': result.push_back('\a'); break;
            case 'b': result.push_back('\b'); break;
            case 't': result.push_back('\t'); break;
            case 'n': result.push_back('\n'); break;
            case 'v': result.push_back('\v'); break;
            case 'f': result.push_back('\f'); break;
            case 'r': result.push_back('\r'); break;
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            default:
                if(escaped < '0' || escaped > '7' || i + 2 >= line.size() ||
                   line[i + 1] < '0' || line[i + 1] > '7' ||
                   line[i + 2] < '0' || line[i + 2] > '7')
                    return std::nullopt;
                result.push_back(static_cast<char>(
                    ((escaped - '0') << 6) | ((line[i + 1] - '0') << 3) |
                    (line[i + 2] - '0')));
                i += 2;
                break;
        }
    }
    return std::nullopt;
}

std::optional<std::string> patch_header_target(const std::string& bytes) {
    constexpr std::string_view prefix = "diff --git ";
    const auto newline = bytes.find('\n');
    if(newline == std::string::npos || newline > 4096) return std::nullopt;
    const std::string_view line(bytes.data(), newline);
    if(!line.starts_with(prefix)) return std::nullopt;
    const std::string_view framed = line.substr(prefix.size());
    std::string old_path;
    std::string new_path;
    if(framed.starts_with('"')) {
        auto old = decode_git_quoted_path(framed, 0);
        if(!old || old->second >= framed.size() || framed[old->second] != ' ')
            return std::nullopt;
        auto current = decode_git_quoted_path(framed, old->second + 1);
        if(!current || current->second != framed.size()) return std::nullopt;
        old_path = std::move(old->first);
        new_path = std::move(current->first);
    } else {
        const auto separator = framed.find(" b/");
        if(separator == std::string_view::npos) return std::nullopt;
        old_path = std::string(framed.substr(0, separator));
        new_path = std::string(framed.substr(separator + 1));
    }
    if(!old_path.starts_with("a/") || new_path != "b/" + old_path.substr(2))
        return std::nullopt;
    return old_path.substr(2);
}

// This is an envelope/shape guard, not a hunk replay implementation. Count
// framing prevents a second traditional diff hiding after a hunk from reaching
// Git. Actual context matching and application remain Git's responsibility.
std::optional<LocalRecipeCandidateFailureReason> validate_patch(
    const std::string& bytes, const std::string& target) {
    using Reason = LocalRecipeCandidateFailureReason;
    if(target != "PKGBUILD") {
        if(target.size() <= std::string_view(".install").size() || !target.ends_with(".install") ||
           target.find('/') != std::string::npos || target.find('\0') != std::string::npos)
            return Reason::UnsupportedPatch;
    }
    if(bytes.empty() || bytes.size() > LOCAL_RECIPE_PATCH_MAX_BYTES ||
       bytes.find('\0') != std::string::npos || bytes.back() != '\n') {
        return Reason::InvalidMaterial;
    }
    std::istringstream input(bytes);
    std::string line;
    const std::string old_path = "a/" + target;
    const std::string new_path = "b/" + target;
    if(!std::getline(input, line) ||
       line != "diff --git " + git_patch_path(old_path) + " " + git_patch_path(new_path)) {
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
    const std::string path_suffix =
        !git_path_needs_quotes(old_path) && target.find(' ') != std::string::npos ? "\t" : "";
    if(line != "--- " + git_patch_path(old_path) + path_suffix ||
       !std::getline(input, line) ||
       line != "+++ " + git_patch_path(new_path) + path_suffix)
        return Reason::UnsupportedPatch;

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

std::optional<std::string> local_recipe_patch_header_target(const std::string& bytes) {
    return patch_header_target(bytes);
}

std::optional<LocalRecipeCandidateFailureReason> validate_local_recipe_patch(const std::string& bytes) {
    return validate_patch(bytes, "PKGBUILD");
}

std::optional<LocalRecipeCandidateFailureReason> validate_local_recipe_patch(
    const std::string& bytes, const std::string& target_relative_path) {
    return validate_patch(bytes, target_relative_path);
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

LocalSourceRoot apply_recipe_patch_series(
    const LocalSourceRoot& before, const std::vector<LocalRecipePatch>& patches,
    LocalRecipeCandidateFailure& failure, SupportedRecipeSnapshot* verified_recipe) {
    using Phase = LocalRecipeCandidatePhase;
    using Reason = LocalRecipeCandidateFailureReason;
    before.require_unchanged_identity();
    auto expected = snapshot_supported_recipe_files(before);
    std::size_t total = 0;
    if(patches.empty() || patches.size() > MAX_SERIES_ENTRIES)
        throw std::invalid_argument("local-recipe-invalid-series");
    failure.patches.assign(patches.size(), LocalRecipePatchOutcome::NotAttempted);
    for(std::size_t i = 0; i < patches.size(); ++i) {
        if(patches[i].bytes.size() > LOCAL_RECIPE_PATCH_MAX_BYTES || total > MAX_SERIES_BYTES - patches[i].bytes.size())
            throw std::invalid_argument("local-recipe-series-limit");
        total += patches[i].bytes.size();
        if(auto invalid = validate_patch(patches[i].bytes, patches[i].target_relative_path)) {
            failure.reason = *invalid;
            failure.rejected_patch_index = i;
            throw std::invalid_argument("local-recipe-patch-shape-rejected");
        }
    }
    // Each snapshot expires only after a successful mutation of its one owned
    // target. Every other supported recipe file must retain its full identity.
    for(std::size_t i = 0; i < patches.size(); ++i) {
        failure.phase = Phase::Apply;
        failure.reason = Reason::CandidateChanged;
        failure.patches[i] = LocalRecipePatchOutcome::Failed;
        auto current = open_local_source_root(before.canonical_path(), true);
        if(current.directory_identity() != before.directory_identity() ||
           snapshot_supported_recipe_files(current) != expected)
            throw std::runtime_error("local-recipe-prepatch-changed");
        const auto target = std::find_if(expected.begin(), expected.end(), [&](const auto& file) {
            return file.relative_path == patches[i].target_relative_path;
        });
        if(target == expected.end()) throw std::runtime_error("local-recipe-target-missing");
        apply_patch(patches[i], current, failure);
        failure.reason = Reason::CandidateChanged;
        auto next = open_local_source_root(before.canonical_path(), true);
        if(next.directory_identity() != before.directory_identity())
            throw std::runtime_error("local-recipe-candidate-identity-changed");
        if(patches[i].target_relative_path == "PKGBUILD" &&
           next.pkgbuild().identity.mode != before.pkgbuild().identity.mode)
            LocalRecipeCandidateAccess::restore_recipe_mode(next, "PKGBUILD", before.pkgbuild().identity.mode);
        if(patches[i].target_relative_path != "PKGBUILD") {
            const auto current_snapshot = snapshot_supported_recipe_files(next);
            const auto current_target = std::find_if(current_snapshot.begin(), current_snapshot.end(), [&](const auto& file) {
                return file.relative_path == patches[i].target_relative_path;
            });
            if(current_target == current_snapshot.end()) throw std::runtime_error("local-recipe-target-missing-after-apply");
            if(current_target->file.identity.mode != target->file.identity.mode)
                LocalRecipeCandidateAccess::restore_recipe_mode(next, patches[i].target_relative_path,
                                                                target->file.identity.mode);
        }
        auto next_snapshot = snapshot_supported_recipe_files(
            open_local_source_root(before.canonical_path(), true));
        if(next_snapshot.size() != expected.size())
            throw std::runtime_error("local-recipe-target-topology-changed");
        for(std::size_t j = 0; j < expected.size(); ++j) {
            if(next_snapshot[j].relative_path != expected[j].relative_path ||
               next_snapshot[j].file.identity.mode != expected[j].file.identity.mode ||
               next_snapshot[j].file.identity.owner != expected[j].file.identity.owner ||
               (next_snapshot[j].relative_path != patches[i].target_relative_path &&
                next_snapshot[j] != expected[j]))
                throw std::runtime_error("local-recipe-unexpected-mutation");
        }
        expected = std::move(next_snapshot);
        failure.patches[i] = LocalRecipePatchOutcome::Applied;
    }
    auto modified = open_local_source_root(before.canonical_path(), true);
    if(snapshot_supported_recipe_files(modified) != expected)
        throw std::runtime_error("local-recipe-modified-candidate-changed");
    if(verified_recipe) *verified_recipe = expected;
    return modified;
}

PreparedLocalRecipeBuild prepare_local_recipe_build(
    LocalSourceRoot original, ValidatedCacheRoot cache_root,
    SourceBuildEnvironment environment, std::vector<LocalRecipePatch> patches,
    ArtifactMakepkgBuildOptions options,
    const ProviderSelectionCallback& select_provider,
    std::optional<PackageBaseIdentity> expected_source,
    const LocalRecipeReviewCallback& review_modified) {
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
            if(patch.target_relative_path != "PKGBUILD") {
                failure.reason = Reason::UnsupportedPatch;
                failure.rejected_patch_index = i;
                throw std::invalid_argument("local-recipe-local-target-unsupported");
            }
            if(patch.bytes.size() > LOCAL_RECIPE_PATCH_MAX_BYTES || total > MAX_SERIES_BYTES - patch.bytes.size())
                throw std::invalid_argument("local-recipe-series-limit");
            total += patch.bytes.size();
            if(const auto invalid = validate_patch(patch.bytes, patch.target_relative_path)) {
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

        workspace->require_unchanged_identity();
        LocalSourceRoot modified = apply_recipe_patch_series(before, patches, failure);
        workspace->require_unchanged_identity();
        if(review_modified) {
            failure.phase = Phase::Review;
            failure.reason = Reason::ReviewStopped;
            auto result = review_modified(modified.pkgbuild());
            if(!std::holds_alternative<ConfirmationAccepted>(result)) {
                failure.review_stop = std::move(result);
                throw std::runtime_error("local-recipe-review-stopped");
            }
            failure.reason = Reason::CandidateChanged;
            modified.require_unchanged_identity();
        }
        failure.phase = Phase::PostpatchMetadata;
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
