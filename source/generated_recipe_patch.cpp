#include "generated_recipe_patch.hpp"

#include "source_build.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <regex>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

GeneratedRecipePatch::GeneratedRecipePatch(
    AurReviewedSourceReviewIdentity identity, std::string bytes)
    : identity_(std::move(identity)), bytes_(std::move(bytes)) {
}
const AurReviewedSourceReviewIdentity& GeneratedRecipePatch::identity() const noexcept {
    return identity_;
}
const std::string& GeneratedRecipePatch::bytes() const noexcept {
    return bytes_;
}

struct RecipePatchGenerationAccess final {
    static GeneratedRecipePatch verified(const AurReviewedSourceReviewIdentity& identity, std::string bytes) {
        return GeneratedRecipePatch(identity, std::move(bytes));
    }
};

namespace {
namespace fs = std::filesystem;
using Reason = RecipePatchGenerationFailureReason;

class Descriptor final {
    int fd_;

public:
    explicit Descriptor(int fd) : fd_(fd) {
        if(fd < 0) throw std::system_error(errno, std::generic_category(), "generated-recipe-open-failed");
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        static_cast<void>(::close(fd_));
    }
    int get() const noexcept {
        return fd_;
    }
};

// Only disposable copies of two recipes and a replay root live here. The
// original checkout is not a cleanup target. No generic workspace capability.
class RecipeDiffDirectory final {
    fs::path path_;
    bool cleanup_attempted_ = false;
    std::optional<std::pair<dev_t, ino_t>> identity_;

public:
    RecipeDiffDirectory() = default;
    RecipeDiffDirectory(const RecipeDiffDirectory&) = delete;
    RecipeDiffDirectory& operator=(const RecipeDiffDirectory&) = delete;
    ~RecipeDiffDirectory() {
        if(!cleanup_attempted_) static_cast<void>(cleanup());
    }
    void create() {
        for(const char* parent : {"/ramdisk", "/tmp"}) {
            std::string pattern = (fs::path(parent) / "moguet-recipe-diff-XXXXXX").string();
            if(char* created = ::mkdtemp(pattern.data())) {
                path_ = created;
                struct stat status{};
                if(::lstat(path_.c_str(), &status) != 0)
                    throw std::system_error(errno, std::generic_category(), "generated-recipe-root-inspection-failed");
                if(!S_ISDIR(status.st_mode))
                    throw std::system_error(std::make_error_code(std::errc::not_a_directory), "generated-recipe-root-inspection-failed");
                identity_.emplace(status.st_dev, status.st_ino);
                return;
            }
        }
        throw std::system_error(errno, std::generic_category(), "generated-recipe-temporary-directory-failed");
    }
    const fs::path& path() const noexcept {
        return path_;
    }
    std::error_code cleanup() {
        cleanup_attempted_ = true;
        std::error_code error;
        if(!path_.empty()) {
            struct stat status{};
            if(!identity_ || ::lstat(path_.c_str(), &status) != 0 ||
               !S_ISDIR(status.st_mode) || std::pair(status.st_dev, status.st_ino) != *identity_) {
                return std::make_error_code(std::errc::state_not_recoverable);
            }
            fs::remove_all(path_, error);
        }
        if(!error) path_.clear();
        return error;
    }
};

void write_recipe(const fs::path& directory, const std::string& bytes) {
    if(::mkdir(directory.c_str(), 0700) != 0)
        throw std::system_error(errno, std::generic_category(), "generated-recipe-directory-failed");
    Descriptor file(::open((directory / "PKGBUILD").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    // Slice 1 freezes contents only, not live mode metadata. Identical private
    // modes prevent inventing a mode edit; no live mode is read or repaired.
    if(::fchmod(file.get(), 0600) != 0)
        throw std::system_error(errno, std::generic_category(), "generated-recipe-mode-failed");
    std::size_t offset = 0;
    while(offset < bytes.size()) {
        const auto written = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
        if(written < 0 && errno == EINTR) continue;
        if(written <= 0) throw std::system_error(written < 0 ? errno : EIO, std::generic_category(), "generated-recipe-write-failed");
        offset += static_cast<std::size_t>(written);
    }
}

using DiffProcess = std::function<BoundedCapturedProcessResult(
    const ExplicitProcessInvocation&, const BoundedProcessPolicy&)>;

RecipePatchGenerationResult generate_frozen_recipe_patch(
    const AurReviewedSourceReviewIdentity& identity, const std::string& baseline,
    const std::string& accepted, const DiffProcess& process) {
    if(baseline == accepted) return RecipePatchNoChange{};
    if(baseline.find('\0') != std::string::npos || accepted.find('\0') != std::string::npos)
        return RecipePatchGenerationFailure{Reason::UnsupportedEdit};

    RecipeDiffDirectory temporary;
    RecipePatchGenerationFailure failure{Reason::TemporaryIoFailure};
    std::optional<std::string> verified_bytes;
    try {
        temporary.create();
        write_recipe(temporary.path() / "a", baseline);
        write_recipe(temporary.path() / "b", accepted);
        write_recipe(temporary.path() / "replay", baseline);
        Descriptor cwd(::open(temporary.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        Descriptor input(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        // Offline analogue of the existing repositoryless Git/apply policy:
        // complete envp, no HOME/XDG/config injection/proxy/CA inheritance, no
        // repository discovery or attributes. Fixed argv delegates diff to Git.
        ExplicitProcessInvocation invocation{
            "/usr/bin/git",
            {"--no-pager", "--git-dir=/dev/null", "-c", "core.attributesFile=/dev/null",
             "-c", "core.autocrlf=false", "-c", "core.safecrlf=false",
             "diff", "--no-index", "--no-prefix", "--no-ext-diff", "--no-textconv",
             "--no-renames", "--diff-algorithm=myers", "--no-indent-heuristic",
             "--unified=3", "--no-color", "--full-index", "--text", "--", "a/PKGBUILD", "b/PKGBUILD"},
            {"PATH=/usr/bin:/bin", "LC_ALL=C", "LANG=C", "GIT_DIR=/dev/null",
             "GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_SYSTEM=/dev/null", "GIT_CONFIG_GLOBAL=/dev/null",
             "GIT_ATTR_NOSYSTEM=1"},
            std::nullopt,
            cwd.get(),
            input.get()};
        // Existing bounded primitive has a combined diagnostic capture. Any
        // unexpected diagnostic contaminates strict patch framing and fails
        // validation; stderr is never silently discarded on a successful diff.
        const BoundedProcessPolicy policy{std::chrono::seconds(30), std::chrono::milliseconds(250), LOCAL_RECIPE_PATCH_MAX_BYTES, true, true};
        failure.reason = Reason::DiffToolFailure;
        auto diff = process(invocation, policy);
        const auto* exited = std::get_if<BoundedProcessExited>(&diff.outcome);
        if(std::holds_alternative<BoundedProcessCaptureLimitExceeded>(diff.outcome) || diff.output.size() > LOCAL_RECIPE_PATCH_MAX_BYTES) {
            failure.reason = Reason::DiffOutputLimit;
            failure.diff_process.emplace(std::move(diff));
        } else if(diff.cancellation_signal || !exited || exited->exit_code != 1 || diff.output.empty()) {
            // Frozen bytes differ: exit 0 is an inconsistent tool observation,
            // not NoChange. Only exit 1 means a normal --no-index difference.
            failure.diff_process.emplace(std::move(diff));
        } else {
            failure.reason = Reason::PatchRejected;
            if(auto invalid = validate_local_recipe_patch(diff.output)) {
                failure.rejected_shape = invalid;
            } else {
                failure.reason = Reason::TemporaryIoFailure;
                LocalRecipeCandidateFailure replay{
                    LocalRecipeCandidatePhase::Preflight, LocalRecipeCandidateFailureReason::InvalidMaterial, {}, std::nullopt, std::nullopt, temporary.path() / "replay"};
                bool replay_attempted = false;
                try {
                    auto before = open_local_source_root(temporary.path() / "replay", true);
                    if(before.pkgbuild().contents != baseline) {
                        failure.reason = Reason::ReproductionMismatch;
                    } else {
                        failure.reason = Reason::ApplyFailed;
                        replay_attempted = true;
                        auto after = apply_recipe_patch_series(before, {{diff.output}}, replay);
                        failure.reason = Reason::CandidateChanged;
                        after.require_unchanged_identity();
                        if(after.pkgbuild().contents != accepted) {
                            failure.reason = Reason::ReproductionMismatch;
                        } else {
                            verified_bytes.emplace(std::move(diff.output));
                        }
                    }
                } catch(const LocalSourceRootError& error) {
                    failure.recipe_snapshot_failure = error.failure();
                } catch(const std::regex_error&) {
                    failure.reason = Reason::InternalFailure;
                    failure.exception = std::current_exception();
                } catch(const std::runtime_error&) {
                    // The existing apply owner reports known failures with
                    // runtime_error and the separately retained replay detail.
                    if(failure.reason != Reason::ApplyFailed) failure.reason = Reason::InternalFailure;
                    failure.exception = std::current_exception();
                } catch(...) {
                    failure.reason = Reason::InternalFailure;
                    failure.exception = std::current_exception();
                }
                if(replay_attempted && !verified_bytes) failure.replay.emplace(std::move(replay));
            }
        }
    } catch(const std::system_error& error) {
        if(failure.reason == Reason::TemporaryIoFailure) {
            failure.temporary_io_error = error.code();
        } else {
            failure.reason = Reason::InternalFailure;
            failure.exception = std::current_exception();
        }
    } catch(...) {
        // Unexpected validator/process/internal exceptions are not Git tool
        // failures or unsupported material. Cleanup still runs before return.
        failure.reason = Reason::InternalFailure;
        failure.exception = std::current_exception();
    }
    const auto cleanup_error = temporary.cleanup();
    if(cleanup_error) {
        if(verified_bytes) failure.reason = Reason::CleanupFailure;
        failure.cleanup_error = cleanup_error;
        failure.abandoned_directory = temporary.path();
        return failure;
    }
    if(verified_bytes) return RecipePatchGenerationAccess::verified(identity, std::move(*verified_bytes));
    return failure;
}
} // namespace

RecipePatchGenerationResult generate_recipe_patch(const ReviewRecipeEditCorrelation& edit) {
    return generate_frozen_recipe_patch(edit.identity(), edit.baseline_pkgbuild(), edit.accepted_pkgbuild(), capture_bounded_explicit_process_output_raw);
}

#ifdef MOGUET_ENABLE_GENERATED_RECIPE_PATCH_TEST_HOOKS
RecipePatchGenerationResult generate_recipe_patch_for_test(
    const AurReviewedSourceReviewIdentity& identity, const std::string& baseline,
    const std::string& accepted, const RecipePatchDiffProcessForTest& process) {
    return generate_frozen_recipe_patch(identity, baseline, accepted, process ? process : DiffProcess(capture_bounded_explicit_process_output_raw));
}
#endif
