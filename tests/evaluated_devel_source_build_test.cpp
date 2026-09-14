#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
#include "pinned_submodule_closure.hpp"
#endif
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
#include "aur_devel_update.hpp"
#include "system_aur_update_operation.hpp"
#include "commands_aur_update.hpp"
#include "cli_parser.hpp"
#include "cli_runtime_contract.hpp"
#include "devel_tracking_bootstrap.hpp"
#include "invocation_owned_recipe_acquisition.hpp"
namespace aur_devel_update_test_stub {
void reset_registered_calls() {
}
unsigned registered_call_count() {
    return 0;
}
} // namespace aur_devel_update_test_stub
#endif

#include "evaluated_devel_source_build.hpp"
#include <csignal>

#ifdef MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
#include "reviewed_devel_source_build_execution.hpp"
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
#include "source_build.hpp"
#include "source_install.hpp"
#include "reviewed_devel_source_route.hpp"
#include "app_config.hpp"
namespace aur_devel_update_test_stub {
void reset_registered_calls();
unsigned registered_call_count();
} // namespace aur_devel_update_test_stub
#endif
namespace publication_allocation {
extern bool blocked;
extern unsigned failures;
} // namespace publication_allocation
#endif

#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
#include "devel_build_provenance_publication_fixture.hpp"
#endif

#ifdef MOGUET_TEST_EVALUATED_DEVEL_ARTIFACT_TRANSPORT
#include "evaluated_devel_source_artifact_transport.hpp"
#include "source_artifact_install_trusted_transport.hpp"
#include "source_artifact_install_trusted_helper_state.hpp"
#include <cerrno>
#endif

#ifdef MOGUET_TEST_EXACT_INSTALLED_BINDING
#include "exact_artifact_transaction_receipt.hpp"
#include "fresh_installed_artifact_binding.hpp"
#include "devel_source_artifact_install.hpp"
#include "installed_package_record_observation.hpp"
#endif

#include "process.hpp"
#include "git_remote_revision_observer.hpp"
#include "reviewed_source_acceptance.hpp"
#include "reviewed_source_presentation.hpp"
#include "reviewed_source_review.hpp"
#include "reviewed_source_state_store.hpp"
#include "reviewed_source_trusted_review.hpp"
#include "trusted_cache.hpp"
#include "trusted_cache_test_support.hpp"
#include "xdg_generation_store.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace fs = std::filesystem;

// The Slice 4 and bridge executables may run concurrently. Only this
// process's successful context creations belong to its cleanup inventory.
std::vector<fs::path> g_fixture_context_roots;

#ifdef MOGUET_TEST_EXACT_INSTALLED_BINDING
static_assert(!std::is_default_constructible_v<InstalledDevelSourceBuildProof>);
static_assert(!std::is_copy_constructible_v<InstalledDevelSourceBuildProof>);
static_assert(std::is_nothrow_move_constructible_v<InstalledDevelSourceBuildProof>);
static_assert(std::is_nothrow_move_constructible_v<DevelSourceArtifactInstallResult>);
static_assert(!std::is_constructible_v<DevelSourceArtifactInstallResult, InstalledArtifactBinding>);
#endif
static_assert(!std::is_default_constructible_v<
              EvaluatedDevelSourceBuildProof>);
static_assert(!std::is_copy_constructible_v<
              EvaluatedDevelSourceBuildProof>);
static_assert(std::is_move_constructible_v<
              EvaluatedDevelSourceBuildProof>);
static_assert(!std::is_default_constructible_v<
              EvaluatedDevelSourceProjection>);
static_assert(!std::is_default_constructible_v<
              FreshDevelPackageArtifact>);
static_assert(!std::is_constructible_v<
              FreshDevelPackageArtifact,
              fs::path>);
static_assert(std::is_invocable_v<
              decltype(build_evaluated_devel_source),
              InvocationOwnedSourceBuildContext,
              InvocationOwnedMakepkgEnvironment>);
static_assert(!std::is_invocable_v<
              decltype(build_evaluated_devel_source),
              PackageBaseIdentity,
              std::string,
              fs::path>);

void require(bool condition, std::string_view message) {
    if(!condition) throw std::runtime_error(std::string(message));
}

template <typename Expected, typename Variant>
Expected take_arm(Variant& value, std::string_view message) {
    Expected* arm = std::get_if<Expected>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return std::move(*arm);
}

template <typename Expected, typename Variant>
const Expected& require_arm(
    const Variant& value,
    std::string_view message) {
    const Expected* arm = std::get_if<Expected>(&value);
    if(arm == nullptr) throw std::runtime_error(std::string(message));
    return *arm;
}

class TemporaryTree final {
public:
    explicit TemporaryTree(std::string_view label) {
        std::string path_template =
            "/tmp/moguet-evaluated-build-test-" +
            std::string(label) + "-XXXXXX";
        std::vector<char> writable(
            path_template.begin(), path_template.end());
        writable.push_back('\0');
        char* created = ::mkdtemp(writable.data());
        require(created != nullptr, "Failed to create test root");
        path_ = created;
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    ~TemporaryTree() noexcept {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

private:
    fs::path path_;
};

class ScopedEnvironmentVariable final {
public:
    ScopedEnvironmentVariable(
        std::string name,
        const std::optional<std::string>& value)
        : name_(std::move(name)) {
        const char* previous = std::getenv(name_.c_str());
        if(previous != nullptr) previous_ = previous;
        const int status = value.has_value()
                               ? ::setenv(
                                     name_.c_str(), value->c_str(), 1)
                               : ::unsetenv(name_.c_str());
        require(status == 0, "Failed to set fixture environment");
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

    ~ScopedEnvironmentVariable() noexcept {
        if(previous_.has_value()) {
            static_cast<void>(::setenv(
                name_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

std::vector<std::string> git_environment(const fs::path& home) {
    return {
        "PATH=/usr/bin:/bin",
        "LC_ALL=C",
        "LANG=C",
        "HOME=" + home.string(),
        "GIT_CONFIG_NOSYSTEM=1",
        "GIT_CONFIG_SYSTEM=/dev/null",
        "GIT_CONFIG_GLOBAL=/dev/null",
        "GIT_AUTHOR_NAME=Slice 4 Fixture",
        "GIT_AUTHOR_EMAIL=slice4@example.invalid",
        "GIT_COMMITTER_NAME=Slice 4 Fixture",
        "GIT_COMMITTER_EMAIL=slice4@example.invalid",
        "GIT_TERMINAL_PROMPT=0",
    };
}

CapturedCommandResult capture_process(
    std::string executable,
    std::vector<std::string> arguments,
    std::vector<std::string> environment,
    const fs::path* working_directory = nullptr,
    std::size_t limit = 16U * 1024U * 1024U) {
    int directory_descriptor = -1;
    if(working_directory != nullptr) {
        directory_descriptor = ::open(
            working_directory->c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        require(directory_descriptor >= 0,
                "Failed to open fixture process cwd");
    }
    ExplicitProcessInvocation invocation{
        std::move(executable), std::move(arguments),
        std::move(environment), limit};
    if(directory_descriptor >= 0) {
        invocation.working_directory_fd = directory_descriptor;
    }
    CapturedCommandResult result =
        capture_explicit_process_output_raw(invocation, true);
    if(directory_descriptor >= 0) {
        static_cast<void>(::close(directory_descriptor));
    }
    return result;
}

void require_process_success(
    std::string executable,
    std::vector<std::string> arguments,
    std::vector<std::string> environment,
    const fs::path* working_directory = nullptr) {
    CapturedCommandResult result = capture_process(
        std::move(executable), std::move(arguments),
        std::move(environment), working_directory);
    require(
        result.exit_code == 0 &&
            !result.stdout_capture_limit_exceeded,
        "Fixture process failed");
}

std::string require_output_line(CapturedCommandResult result) {
    require(
        result.exit_code == 0 &&
            !result.stdout_capture_limit_exceeded &&
            !result.output.empty() && result.output.back() == '\n',
        "Fixture process output failed");
    result.output.pop_back();
    require(
        !result.output.empty() &&
            result.output.find('\n') == std::string::npos,
        "Fixture process did not produce one line");
    return result.output;
}

class UpstreamGitFixture final {
public:
    explicit UpstreamGitFixture(
        std::string label,
        GitObjectFormat object_format = GitObjectFormat::Sha1)
        : tree_(label), label_(std::move(label)),
          object_format_(object_format) {
        home_ = tree_.path() / "home";
        remote_ = tree_.path() / "upstream.git";
        work_ = tree_.path() / "upstream-work";
        fs::create_directory(home_);
        std::vector<std::string> bare_init{
            "init", "--bare", "--initial-branch=main"};
        std::vector<std::string> work_init{
            "init", "--initial-branch=main"};
        if(object_format_ == GitObjectFormat::Sha256) {
            bare_init.push_back("--object-format=sha256");
            work_init.push_back("--object-format=sha256");
        }
        bare_init.push_back(remote_.string());
        work_init.push_back(work_.string());
        require_process_success(
            "/usr/bin/git", std::move(bare_init),
            git_environment(home_));
        require_process_success(
            "/usr/bin/git", std::move(work_init),
            git_environment(home_));
        run_git({"remote", "add", "origin", remote_.string()});
        url_ = "https://fixture.invalid/" + label_ + ".git";
        commit("revision-one\n");
    }

    [[nodiscard]] const std::string& url() const noexcept {
        return url_;
    }

    [[nodiscard]] const fs::path& remote() const noexcept {
        return remote_;
    }

    [[nodiscard]] const std::string& oid() const noexcept {
        return oid_;
    }

    std::string commit(std::string_view payload) {
        write_file(work_ / "payload.txt", payload);
        run_git({"add", "--", "payload.txt"});
        run_git({"commit", "-q", "-m", std::string(payload)});
        oid_ = output_git({"rev-parse", "HEAD"});
        run_git({"push", "-u", "origin", "main"});
        return oid_;
    }

#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
    std::string pin_tree(const std::string& modules, const std::vector<std::pair<std::string, std::string>>& pins) {
        write_file(work_ / ".gitmodules", modules);
        run_git({"add", "--", ".gitmodules"});
        for(const auto& [path, oid] : pins)
            run_git({"update-index", "--add", "--cacheinfo", "160000," + oid + "," + path});
        run_git({"commit", "--allow-empty", "-q", "-m", "pinned tree"});
        oid_ = output_git({"rev-parse", "HEAD"});
        run_git({"push", "origin", "main"});
        return oid_;
    }
    void modules_mode(const std::string& mode) {
        // update-index rejects a symlink .gitmodules before the consumer can
        // observe it. Construct the invalid raw tree only in this fixture.
        auto tree = capture_process("/usr/bin/git", {"-C", work_.string(), "cat-file", "tree", "HEAD^{tree}"}, git_environment(home_));
        require(tree.exit_code == 0 && !tree.stdout_capture_limit_exceeded && tree.output.starts_with("100644 .gitmodules"), "Fixture raw parent tree unavailable");
        tree.output.replace(0, 6, mode);
        const auto raw_tree = tree_.path() / "invalid-modules-tree";
        write_file(raw_tree, tree.output);
        const auto tree_oid = output_git({"hash-object", "--literally", "-t", "tree", "-w", raw_tree.string()});
        oid_ = output_git({"commit-tree", tree_oid, "-p", oid_, "-m", "invalid modules type"});
        run_git({"update-ref", "refs/heads/main", oid_});
        run_git({"push", "origin", "main"});
    }
    void remove_modules() {
        run_git({"update-index", "--force-remove", ".gitmodules"});
        run_git({"commit", "-q", "-m", "missing modules"});
        oid_ = output_git({"rev-parse", "HEAD"});
        run_git({"push", "origin", "main"});
    }
    std::string annotated_tag() {
        run_git({"tag", "-a", "closure-tag", "-m", "not a commit"});
        run_git({"push", "origin", "refs/tags/closure-tag"});
        return output_git({"rev-parse", "refs/tags/closure-tag"});
    }
    std::string object_oid(const std::string& expression) const {
        return output_git({"rev-parse", expression});
    }
#endif

private:
    static void write_file(
        const fs::path& path,
        std::string_view contents) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output),
                "Failed to open upstream fixture file");
        output.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        output.close();
        require(static_cast<bool>(output),
                "Failed to write upstream fixture file");
    }

    void run_git(std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", work_.string()};
        complete.insert(
            complete.end(), arguments.begin(), arguments.end());
        require_process_success(
            "/usr/bin/git", std::move(complete),
            git_environment(home_));
    }

    [[nodiscard]] std::string output_git(
        std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", work_.string()};
        complete.insert(
            complete.end(), arguments.begin(), arguments.end());
        return require_output_line(capture_process(
            "/usr/bin/git", std::move(complete),
            git_environment(home_)));
    }

    TemporaryTree tree_;
    std::string label_;
    GitObjectFormat object_format_;
    fs::path home_;
    fs::path remote_;
    fs::path work_;
    std::string url_;
    std::string oid_;
};

enum class RecipeShape {
    Valid,
    RawEvaluatedMismatch,
    MultipleGit,
    UnsupportedVcs,
    UnsupportedSelector,
    DynamicVersionDrift,
    MissingLocal,
    DirectoryLocal,
};

struct ArchitectureFixture {
    std::vector<std::string> declared{"any"};
    std::optional<std::vector<std::string>> reviewed;
    std::string effective;
    std::string extension;
    std::string epoch;
    std::string pkgrel = "1";
    std::string recipe_suffix;
    std::string package_commands;
    std::string reviewed_child_arch;
    bool qualified_source = false;
};

class ReviewedBuildFixture final {
public:
    ReviewedBuildFixture(
        std::string label,
        const UpstreamGitFixture& upstream,
        RecipeShape shape = RecipeShape::Valid,
        bool prepare_mutation = false,
        bool exact_branch = false,
        bool tracked_local_source = false,
        // The reviewed fixture owns its one-artifact shape even when the
        // current Arch makepkg.conf enables a debug package by default.
        bool disable_debug = true,
        ArchitectureFixture architecture = {})
        : tree_(label), upstream_(upstream),
          package_base_("example-base"),
          package_name_("moguet-slice4-" + label),
          aur_remote_("https://aur.archlinux.org/example-base.git"),
          architecture_(std::move(architecture)) {
        cache_home_ = tree_.path() / "cache";
        state_home_ = tree_.path() / "state";
        home_ = tree_.path() / "home";
        for(const fs::path& directory :
            {cache_home_, state_home_, home_}) {
            fs::create_directory(directory);
            fs::permissions(
                directory, fs::perms::owner_all,
                fs::perm_options::replace);
        }
        environment_.push_back(
            std::make_unique<ScopedEnvironmentVariable>(
                "XDG_CACHE_HOME", cache_home_.string()));
        environment_.push_back(
            std::make_unique<ScopedEnvironmentVariable>(
                "XDG_STATE_HOME", state_home_.string()));
        environment_.push_back(
            std::make_unique<ScopedEnvironmentVariable>(
                "HOME", home_.string()));

        cache_root_.emplace(prepare_test_trusted_cache_root());
        checkout_.emplace(create_trusted_cache_directory(
            *cache_root_, package_base_));
        repository_ = checkout_->path();
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        // Authoritative recipe bytes are authored independently, before the old
        // cache copy exists. Neither trial metadata nor fetch reads that copy.
        recipe_remote_ = tree_.path() / "authoritative-recipe";
        fs::create_directory(recipe_remote_);
        repository_ = recipe_remote_;
#endif
        run_git({"init", "-q", "-b", "main"});
        run_git({"config", "--local", "remote.origin.url", aur_remote_});
        run_git({"config", "--local", "remote.origin.fetch",
                 "+refs/heads/*:refs/remotes/origin/*"});
        run_git({"config", "--local", "branch.main.remote", "origin"});
        run_git({"config", "--local", "branch.main.merge",
                 "refs/heads/main"});
        write_file(
            "PKGBUILD",
            pkgbuild(
                shape, prepare_mutation, exact_branch,
                tracked_local_source) +
                (disable_debug ? "\noptions=('!debug')\n" : "") +
                architecture_.recipe_suffix);
        write_file(
            ".SRCINFO",
            srcinfo(shape, exact_branch, tracked_local_source));
        if(tracked_local_source) {
            write_file("fix.patch", "--- a/payload.txt\n+++ b/payload.txt\n@@ -1 +1 @@\n-revision-one\n+reviewed-patch-applied\n");
            write_file("config.toml", "setting = \"reviewed-config\"\n");
            if(shape == RecipeShape::MissingLocal || shape == RecipeShape::DirectoryLocal) {
                require(fs::remove(repository_ / "fix.patch"), "Cannot remove fixture-owned patch");
                if(shape == RecipeShape::DirectoryLocal) {
                    fs::create_directory(repository_ / "fix.patch");
                    write_file("fix.patch/entry", "not a regular supplemental input\n");
                }
            }
        }
        recipe_oid_ = commit("reviewed recipe");
        run_git({"update-ref", "refs/remotes/origin/main", recipe_oid_});
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        require_process_success("/usr/bin/git", {"clone", "--no-local", recipe_remote_.string(), checkout_->path().string()}, git_environment(home_));
        repository_ = checkout_->path();
        run_git({"config", "--local", "remote.origin.url", aur_remote_});
#endif
    }

#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
    const fs::path& recipe_remote() const {
        return recipe_remote_;
    }
    std::string recipe_metadata() const {
        std::ifstream input(recipe_remote_ / ".SRCINFO");
        require(input.good(), "Independent authoritative metadata unavailable");
        return std::string((std::istreambuf_iterator<char>(input)), {});
    }
    void advance_recipe_for_bootstrap_test() {
        repository_ = recipe_remote_;
        write_file("review-again.txt", "This tracked file is part of the bootstrap full review.\n");
        recipe_oid_ = commit("changed recipe for bootstrap");
        run_git({"update-ref", "refs/remotes/origin/main", recipe_oid_});
        repository_ = checkout_->path();
    }
#endif

    [[nodiscard]] const std::string& recipe_oid() const noexcept {
        return recipe_oid_;
    }

    [[nodiscard]] const std::string& package_base() const noexcept {
        return package_base_;
    }

    [[nodiscard]] const std::string& package_name() const noexcept {
        return package_name_;
    }

    [[nodiscard]] const fs::path& home() const noexcept {
        return home_;
    }

    void require_no_provenance_publication() const {
        require(!fs::exists(state_home_ / "moguet/devel-build-provenance"), "Slice 5 wrote XDG provenance state");
    }

    [[nodiscard]] const UpstreamGitFixture& upstream() const noexcept {
        return upstream_;
    }

    [[nodiscard]] InvocationOwnedSourceBuildContext make_context() {
        InvocationOwnedSourceBuildContextResult result =
            create_invocation_owned_source_build_context(
                make_pinned_build());
        if(const auto* failure =
               std::get_if<InvocationOwnedSourceBuildContextFailure>(
                   &result)) {
            std::ostringstream message;
            message << "Context creation failed: stage="
                    << static_cast<int>(failure->stage)
                    << " reason=" << static_cast<int>(failure->reason);
            if(failure->system_error.has_value()) {
                message << " errno=" << failure->system_error->value();
            }
            if(failure->diagnostic.has_value()) {
                message << " diagnostic=" << *failure->diagnostic;
            }
            throw std::runtime_error(message.str());
        }
        auto context = take_arm<InvocationOwnedSourceBuildContext>(
            result, "Context creation returned no context");
        g_fixture_context_roots.push_back(context.owned_root());
        return context;
    }

    [[nodiscard]] InvocationOwnedMakepkgEnvironment make_environment(
        const InvocationOwnedSourceBuildContext& context) const {
        const std::string rewrite_key =
            "url.file://" + upstream_.remote().string() + ".insteadOf";
        SourceBuildEnvironment customization{{
            {"HOME", home_.string()},
            {"PATH", "/usr/bin:/bin"},
            {"LANG", "C"},
            {"LC_ALL", "C"},
            {"MAKEPKG_LIBRARY", "/usr/share/makepkg"},
            {"GIT_TERMINAL_PROMPT", "0"},
            {"GIT_CONFIG_COUNT", "1"},
            {"GIT_CONFIG_KEY_0", rewrite_key},
            {"GIT_CONFIG_VALUE_0", upstream_.url()},
        }};
        if(!architecture_.effective.empty()) {
            customization.ordered_assignments.push_back({"CARCH", architecture_.effective});
        }
        if(!architecture_.extension.empty()) {
            customization.ordered_assignments.push_back({"PKGEXT", architecture_.extension});
        }
        InvocationOwnedMakepkgEnvironmentResult result =
            context.make_makepkg_environment(
                customization,
                SourceEnvironmentEmptyValuePolicy::Forward);
        return take_arm<InvocationOwnedMakepkgEnvironment>(
            result, "Makepkg environment creation failed");
    }

#ifdef MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
    // Same typed publication/pin boundary as the normal source owner, before
    // its legacy ProductionArtifactSourceTree lifetime erasure.
    [[nodiscard]] PinnedReviewedSourceBuild execution_pin(bool overlay = false) {
        if(!overlay) return make_pinned_build();
        auto planned = plan_reviewed_source_lifecycle(identity());
        auto requirement = take_arm<ReviewedSourceReviewRequirement>(planned, "overlay fixture expected initial review");
        auto materialized = materialize_accepted_reviewed_source_checkout(accept_initial(std::move(requirement)), checkout());
        auto accepted = take_arm<AcceptedReviewedSourceCheckout>(materialized, "overlay checkout failed");
        auto boundary = begin_reviewed_source_editor_boundary(accepted);
        {
            std::ofstream file(repository_ / "PKGBUILD", std::ios::app);
            file << "\n# invocation editor overlay\n";
        }
        auto overlay_proof = seal_reviewed_source_editor_overlay(accepted, take_arm<ReviewedSourceEditorBoundary>(boundary, "overlay boundary failed"));
        auto publication = publish_accepted_reviewed_source_checkout_with_editor_overlay(std::move(accepted), take_arm<ReviewedSourceEditorOverlayProof>(overlay_proof, "overlay seal failed"));
        return take_arm<PinnedReviewedSourceBuild>(publication, "overlay publication failed");
    }
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
    SourceBuildExecutionResult normal_execution(const ReviewedDevelSourceBuildIntent& intent, bool no_confirm = false, bool package_base = false) {
        // Seed an actually accepted #411 record, then release the pin/lease.
        {
            auto accepted = execution_pin();
            require(accepted.valid(), "normal fixture review failed");
        }
        const auto wrapper = tree_.path() / "normal-git";
        std::ofstream script(wrapper);
        script << "#!/bin/sh\nfor argument do if [ \"$argument\" = fetch ]; then exit 0; fi; done\nexec /usr/bin/git \"$@\"\n";
        script.close();
        fs::permissions(wrapper, fs::perms::owner_all);
        ScopedEnvironmentVariable git("MOGUET_TEST_GIT_EXECUTABLE", wrapper.string());
        AppConfig config;
        config.no_confirm = no_confirm;
        config.user_config.review.pkgbuild = ReviewPolicy::Skip;
        config.rm_deps = false;
        if(package_base) {
            auto execution = execute_source_build_package_base_typed(intent.request, intent.required_targets, *cache_root_, intent.database_paths, config);
            SourceBuildExecutionResult out;
            out.devel_execution.emplace(std::get<ReviewedDevelExecutionSnapshot>(std::move(execution)));
            out.status = out.devel_execution->complete ? SourceBuildExecutionStatus::Installed : SourceBuildExecutionStatus::AuthoritativeIncomplete;
            return out;
        }
        return execute_source_build_typed(intent.request, *cache_root_, intent.required_targets.front().desired_reason, intent.database_paths, config);
    }
#endif
    [[nodiscard]] ValidatedCachePath execution_checkout() const {
        return checkout();
    }
    [[nodiscard]] ReviewedDevelSourceBuildIntent execution_intent(const PacmanDatabasePaths& database) const {
        SourceBuildRequest request;
        request.package_name = package_name_;
        request.checkout_name = package_base_;
        request.git_url = aur_remote_;
        request.aur_review_identity = identity().package_base();
        request.empty_value_policy = SourceEnvironmentEmptyValuePolicy::Forward;
        request.custom_environment = SourceBuildEnvironment{{{"HOME", home_.string()}, {"PATH", "/usr/bin:/bin"}, {"LANG", "C"}, {"LC_ALL", "C"}, {"MAKEPKG_LIBRARY", "/usr/share/makepkg"}, {"GIT_TERMINAL_PROMPT", "0"}, {"GIT_CONFIG_COUNT", "1"}, {"GIT_CONFIG_KEY_0", "url.file://" + upstream_.remote().string() + ".insteadOf"}, {"GIT_CONFIG_VALUE_0", upstream_.url()}}};
        return {std::move(request), {{package_base_, package_name_, DesiredInstallReason::Explicit}}, database, false, {true}};
    }
#endif

    [[nodiscard]] std::string stale_packagelist_filename() const {
        const fs::path pkgdest = tree_.path() / "stale-pkgdest";
        const fs::path builddir = tree_.path() / "stale-builddir";
        const fs::path srcdest = tree_.path() / "stale-srcdest";
        fs::create_directory(pkgdest);
        fs::create_directory(builddir);
        fs::create_directory(srcdest);
        std::vector<std::string> environment{
            "HOME=" + home_.string(),
            "PATH=/usr/bin:/bin",
            "LANG=C",
            "LC_ALL=C",
            "MAKEPKG_LIBRARY=/usr/share/makepkg",
            "PKGDEST=" + pkgdest.string(),
            "BUILDDIR=" + builddir.string(),
            "SRCDEST=" + srcdest.string(),
        };
        CapturedCommandResult result = capture_process(
            "/usr/bin/makepkg", {"--packagelist"},
            std::move(environment), &repository_);
        return fs::path(require_output_line(std::move(result)))
            .filename()
            .string();
    }

private:
    [[nodiscard]] std::string source_value(
        RecipeShape shape,
        bool exact_branch,
        bool raw) const {
        if(shape == RecipeShape::UnsupportedVcs) {
            return package_name_ + "::hg+https://fixture.invalid/repo";
        }
        std::string remote = upstream_.url();
        if(shape == RecipeShape::RawEvaluatedMismatch && raw) {
            remote = "https://raw.fixture.invalid/untrusted.git";
        }
        std::string value = package_name_ + "::git+" + remote;
        if(shape == RecipeShape::UnsupportedSelector) {
            value += "#tag=v1";
        } else if(exact_branch) {
            value += "#branch=main";
        }
        return value;
    }

    [[nodiscard]] std::string pkgbuild(
        RecipeShape shape,
        bool prepare_mutation,
        bool exact_branch,
        bool tracked_local_source) const {
        const std::string effective_source =
            source_value(shape, exact_branch, false);
        std::string second_source;
        if(shape == RecipeShape::MultipleGit) {
            second_source =
                "\n    \"second::git+https://fixture.invalid/second.git\"";
        }
        if(tracked_local_source) {
            second_source += "\n    \"fix.patch\"\n    \"config.toml\"";
        }
        std::string prepare;
        if(prepare_mutation) {
            prepare =
                "prepare() {\n"
                "    cd \"$srcdir/$pkgname\"\n"
                "    printf 'prepared\\n' >> payload.txt\n"
                "}\n\n";
        }
        if(tracked_local_source) {
            prepare =
                "prepare() {\n"
                "    cd \"$srcdir/$pkgname\"\n"
                "    patch -p1 < \"$srcdir/fix.patch\"\n"
                "}\n\n"
                "build() {\n"
                "    cp \"$srcdir/config.toml\" \"$srcdir/$pkgname/built-config.toml\"\n"
                "}\n\n";
        }
        const std::string pkgver_function =
            shape == RecipeShape::DynamicVersionDrift
                ? "pkgver() {\n"
                  "    cd \"$srcdir/$pkgname\"\n"
                  "    local counter=.moguet-pkgver-counter\n"
                  "    local value=0\n"
                  "    if [[ -f $counter ]]; then read -r value < $counter; fi\n"
                  "    ((value += 1))\n"
                  "    printf '%s\\n' \"$value\" > $counter\n"
                  "    printf '1.r%s.g%s' \"$value\" \"$(git rev-parse --short=12 HEAD)\"\n"
                  "}\n\n"
                : "pkgver() {\n"
                  "    cd \"$srcdir/$pkgname\"\n"
                  "    printf '1.r%s.g%s' \"$(git rev-list --count HEAD)\" \"$(git rev-parse --short=12 HEAD)\"\n"
                  "}\n\n";
        std::string arch_declaration = "arch=(";
        for(const auto& arch : architecture_.declared)
            arch_declaration += "'" + arch + "' ";
        arch_declaration += ")\n";
        return "pkgbase=" + package_base_ + "\n"
                                            "pkgname=" +
               package_name_ + "\n"
                               "pkgver=0\n"
                               "pkgrel=" +
               architecture_.pkgrel + "\n" +
               (architecture_.epoch.empty() ? "" : "epoch=" + architecture_.epoch + "\n") +
               "pkgdesc='Moguet Slice 4 fixture'\n" + arch_declaration +
               "license=('GPL-3.0-or-later')\n"
               "source=(\"" +
               effective_source + "\"" +
               second_source + ")\n"
                               "sha256sums=('SKIP'" +
               (shape == RecipeShape::MultipleGit ? " 'SKIP'" : "") +
               (tracked_local_source ? " 'SKIP' 'SKIP'" : "") +
               ")\n\n" + pkgver_function + prepare +
               "package() {\n" + architecture_.package_commands +
               "    install -Dm644 \"$srcdir/$pkgname/payload.txt\" \"$pkgdir/usr/share/$pkgname/payload.txt\"\n" +
               (tracked_local_source ? "    install -Dm644 \"$srcdir/$pkgname/built-config.toml\" \"$pkgdir/usr/share/$pkgname/config.toml\"\n" : "") +
               "}\n";
    }

    [[nodiscard]] std::string srcinfo(
        RecipeShape shape,
        bool exact_branch,
        bool tracked_local_source) const {
        std::string result =
            "pkgbase = " + package_base_ + "\n"
                                           "\tpkgdesc = Moguet Slice 4 fixture\n"
                                           "\tpkgver = 0\n"
                                           "\tpkgrel = " +
            architecture_.pkgrel + "\n" +
            (architecture_.epoch.empty() ? "" : "\tepoch = " + architecture_.epoch + "\n") +
            "\tlicense = GPL-3.0-or-later\n" +
            (architecture_.qualified_source ? "\tsource_x86_64 = " : "\tsource = ") +
            source_value(shape, exact_branch, true) +
            "\n\tsha256sums = SKIP\n";
        for(const auto& arch : architecture_.reviewed.value_or(architecture_.declared)) {
            result += "\tarch = " + arch + "\n";
        }
        if(shape == RecipeShape::MultipleGit) {
            result +=
                "\tsource = second::git+https://fixture.invalid/second.git\n"
                "\tsha256sums = SKIP\n";
        }
        if(tracked_local_source) {
            result +=
                "\tsource = fix.patch\n"
                "\tsha256sums = SKIP\n"
                "\tsource = config.toml\n"
                "\tsha256sums = SKIP\n";
        }
        result += "pkgname = " + package_name_ + "\n";
        result += architecture_.reviewed_child_arch;
        return result;
    }

    void write_file(
        const std::string& relative_path,
        std::string_view contents) const {
        const fs::path path = repository_ / relative_path;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output),
                "Failed to open reviewed fixture file");
        output.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        output.close();
        require(static_cast<bool>(output),
                "Failed to write reviewed fixture file");
    }

    void run_git(std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", repository_.string()};
        complete.insert(
            complete.end(), arguments.begin(), arguments.end());
        require_process_success(
            "/usr/bin/git", std::move(complete),
            git_environment(home_));
    }

    [[nodiscard]] std::string output_git(
        std::vector<std::string> arguments) const {
        std::vector<std::string> complete{"-C", repository_.string()};
        complete.insert(
            complete.end(), arguments.begin(), arguments.end());
        return require_output_line(capture_process(
            "/usr/bin/git", std::move(complete),
            git_environment(home_)));
    }

    [[nodiscard]] std::string commit(std::string_view message) const {
        run_git({"add", "-A"});
        run_git({"commit", "-q", "-m", std::string(message)});
        return output_git({"rev-parse", "HEAD"});
    }

    [[nodiscard]] ValidatedCachePath checkout() const {
        return revalidate_trusted_cache_path(
            *checkout_, CachePathRequirement::ExistingDirectory);
    }

    [[nodiscard]] AurReviewedSourceReviewIdentity identity() const {
        return AurReviewedSourceReviewIdentity::make(
            PackageBaseIdentity::make(
                PackageSourceIdentity::aur(
                    SourceLocationIdentity::known_git_remote(aur_remote_)),
                package_base_),
            SourceRevisionIdentity::git_commit(recipe_oid_));
    }

    [[nodiscard]] static ExplicitConfirmationResult explicit_yes() {
        ExplicitConfirmationInputParseResult parsed =
            parse_explicit_confirmation_input("yes");
        return take_arm<ExplicitConfirmationAcceptance>(
            parsed, "Explicit yes was not accepted");
    }

    [[nodiscard]] AcceptedReviewedSourceTarget accept_initial(
        ReviewedSourceReviewRequirement requirement) const {
        ReviewedSourceVerifiedMaterializedReview verified =
            seal_reviewed_source_materialized_review_for_test(
                ReviewedSourceMaterializedInitialFullReview{
                    identity().target_revision(),
                    ReviewedSourceReviewBody{
                        ReviewedSourceReviewReadiness::Complete, {}}});
        TrustedAurReviewedSourceReview trusted =
            make_trusted_aur_reviewed_source_review_fixture_for_test(
                std::move(verified));
        ReviewedSourceVerifiedLifecycleResult bound =
            bind_reviewed_source_verified_review(
                std::move(requirement), std::move(trusted));
        ReviewedSourceVerifiedLifecycleTarget target =
            take_arm<ReviewedSourceVerifiedLifecycleTarget>(
                bound, "Verified review did not bind");
        std::ostringstream output;
        PresentedReviewedSourceTargetResult presented =
            present_reviewed_source_target(std::move(target), output);
        ReviewedSourceAcceptanceDisposition disposition =
            decide_reviewed_source_acceptance(
                take_arm<PresentedReviewedSourceTarget>(
                    presented, "Review presentation failed"),
                explicit_yes());
        return take_arm<AcceptedReviewedSourceTarget>(
            disposition, "Review acceptance did not produce a target");
    }

    [[nodiscard]] PinnedReviewedSourceBuild make_pinned_build() {
        ReviewedSourceLifecyclePlanResult planned =
            plan_reviewed_source_lifecycle(identity());
        if(auto* requirement =
               std::get_if<ReviewedSourceReviewRequirement>(&planned)) {
            AcceptedReviewedSourceCheckoutResult materialized =
                materialize_accepted_reviewed_source_checkout(
                    accept_initial(std::move(*requirement)), checkout());
            AcceptedReviewedSourceCheckout accepted =
                take_arm<AcceptedReviewedSourceCheckout>(
                    materialized,
                    "Accepted checkout materialization failed");
            ReviewedSourcePublicationResult publication =
                publish_accepted_reviewed_source_checkout(
                    std::move(accepted));
            return take_arm<PinnedReviewedSourceBuild>(
                publication,
                "Accepted publication did not produce a pin");
        }
        ReviewedSourceAlreadyReviewedContinue already =
            take_arm<ReviewedSourceAlreadyReviewedContinue>(
                planned, "Reviewed lifecycle did not produce a build route");
        AlreadyReviewedSourceCheckoutResult materialized =
            materialize_already_reviewed_source_checkout(
                std::move(already), checkout());
        AlreadyReviewedSourceCheckout checkout_capability =
            take_arm<AlreadyReviewedSourceCheckout>(
                materialized,
                "Already-reviewed checkout materialization failed");
        ReviewedSourcePublicationResult confirmed =
            confirm_already_reviewed_source_checkout(
                std::move(checkout_capability));
        return take_arm<PinnedReviewedSourceBuild>(
            confirmed,
            "Already-reviewed confirmation did not produce a pin");
    }

    TemporaryTree tree_;
    const UpstreamGitFixture& upstream_;
    std::string package_base_;
    std::string package_name_;
    std::string aur_remote_;
    ArchitectureFixture architecture_;
    fs::path cache_home_;
    fs::path state_home_;
    fs::path home_;
    fs::path repository_;
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
    fs::path recipe_remote_;
#endif
    std::string recipe_oid_;
    std::vector<std::unique_ptr<ScopedEnvironmentVariable>> environment_;
    std::optional<ValidatedCacheRoot> cache_root_;
    std::optional<ValidatedCachePath> checkout_;
};

EvaluatedDevelSourceBuildProof build_success(
    ReviewedBuildFixture& fixture) {
    InvocationOwnedSourceBuildContext context = fixture.make_context();
    InvocationOwnedMakepkgEnvironment environment =
        fixture.make_environment(context);
    EvaluatedDevelSourceBuildResult result =
        build_evaluated_devel_source(
            std::move(context), std::move(environment));
    if(const auto* failure =
           std::get_if<EvaluatedDevelSourceBuildFailure>(&result)) {
        std::ostringstream message;
        message << "Slice 4 build failed: stage="
                << static_cast<int>(failure->stage)
                << " reason=" << static_cast<int>(failure->reason);
        if(failure->process_outcome.has_value()) {
            message << " process-arm="
                    << failure->process_outcome->index();
            if(const auto* exited = std::get_if<BoundedProcessExited>(
                   &*failure->process_outcome)) {
                message << " exit=" << exited->exit_code;
            }
        }
        if(failure->system_error.has_value()) {
            message << " errno=" << failure->system_error->value()
                    << " " << failure->system_error->message();
        }
        if(failure->diagnostic.has_value()) {
            message << " diagnostic=" << *failure->diagnostic;
        }
        throw std::runtime_error(message.str());
    }
    return take_arm<EvaluatedDevelSourceBuildProof>(
        result, "Slice 4 build returned no proof");
}

void cleanup_proof(EvaluatedDevelSourceBuildProof& proof) {
    InvocationOwnedSourceBuildContextCleanupResult cleanup =
        proof.cleanup();
    require(
        std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(
            cleanup),
        "Slice 4 proof cleanup failed");
    require(!proof.valid(), "Cleaned Slice 4 proof remained active");
}

std::string sha256_path(const fs::path& path) {
    return require_output_line(capture_process(
                                   "/usr/bin/sha256sum", {path.string()},
                                   {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C"}))
        .substr(0, 64);
}

std::string mtree_digest(const fs::path& archive) {
    CapturedCommandResult result = capture_process(
        "/usr/bin/bsdtar", {"-xOf", archive.string(), ".MTREE"},
        {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C"});
    require(
        result.exit_code == 0 && !result.output.empty(),
        "Failed to extract fixture MTREE");
    return xdg_generation_store_raw_contents_sha256(result.output);
}

std::string archive_member(
    const fs::path& archive,
    const std::string& member) {
    CapturedCommandResult result = capture_process(
        "/usr/bin/bsdtar", {"-xOf", archive.string(), member},
        {"PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C"});
    require(result.exit_code == 0,
            "Failed to extract fixture archive member");
    return result.output;
}

void test_valid_dynamic_build_and_prepare_mutation() {
    UpstreamGitFixture upstream("valid-prepare");
    ReviewedBuildFixture fixture(
        "valid-prepare", upstream, RecipeShape::Valid, true, true);
    const std::string stale = fixture.stale_packagelist_filename();
    EvaluatedDevelSourceBuildProof proof = build_success(fixture);

    const std::string* reviewed_oid = proof.reviewed_binding()
                                          .reviewed_recipe_revision()
                                          .value()
                                          .git_commit();
    const std::string* built_oid = proof.actual_built_revision()
                                       .revision()
                                       .value()
                                       .git_commit();
    const ArtifactPackageIdentity& artifact =
        proof.artifact().evidence().identity;
    require(reviewed_oid != nullptr && *reviewed_oid == fixture.recipe_oid(),
            "Reviewed recipe OID was not retained");
    require(
        proof.snapshot_identity().reviewed_binding() ==
            proof.reviewed_binding(),
        "Reviewed snapshot binding drifted");
    require(
        proof.evaluated_source().git_source().source_location() ==
                upstream.url() &&
            proof.evaluated_source().git_source().selector().kind() ==
                VcsSelectorKind::Branch &&
            proof.evaluated_source().source_count() == 1 &&
            proof.evaluated_source().tracked_local_source_count() == 0,
        "Evaluated source projection differs");
    require(built_oid != nullptr && *built_oid == upstream.oid(),
            "Actual complete Git OID differs from the built workspace");
    require(
        built_oid->size() == 40 || built_oid->size() == 64,
        "Actual built OID is abbreviated");
    require(
        artifact.package_name == fixture.package_name() &&
            artifact.package_base.value() != nullptr &&
            *artifact.package_base.value() == fixture.package_base() &&
            artifact.architecture.value() != nullptr &&
            *artifact.architecture.value() == "any" &&
            artifact.full_version.find(upstream.oid().substr(0, 12)) !=
                std::string::npos,
        "Dynamic artifact metadata differs");
    require(
        stale.find("-0-1-") != std::string::npos &&
            proof.artifact().path().filename().string() != stale,
        "Pre-preparation stale packagelist became final identity");
    require(
        proof.artifact().evidence().archive_digest.value() ==
            sha256_path(proof.artifact().path()),
        "Archive SHA-256 differs from the exact artifact");
    require(
        proof.artifact().evidence().mtree_digest.value() ==
            mtree_digest(proof.artifact().path()),
        "MTREE SHA-256 differs from the exact archive member");
    require(
        archive_member(
            proof.artifact().path(),
            "usr/share/" + fixture.package_name() + "/payload.txt") ==
            "revision-one\nprepared\n",
        "prepare() tracked-file mutation did not reach the artifact");
    cleanup_proof(proof);
}

void require_supplemental_artifact(const EvaluatedDevelSourceBuildProof& proof, const ReviewedBuildFixture& fixture) {
    require(proof.evaluated_source().source_count() == 3 &&
                proof.evaluated_source().tracked_local_source_count() == 2,
            "Supplemental input counts differ");
    require(archive_member(proof.artifact().path(), "usr/share/" + fixture.package_name() + "/payload.txt") == "reviewed-patch-applied\n",
            "Actual applied patch bytes did not reach the archive");
    require(archive_member(proof.artifact().path(), "usr/share/" + fixture.package_name() + "/config.toml") == "setting = \"reviewed-config\"\n",
            "Actual build-copied config bytes did not reach the archive");
}

void test_reviewed_local_source_remains_supported_input() {
    UpstreamGitFixture upstream("tracked-local");
    ArchitectureFixture architecture;
    architecture.declared = {"i686", "x86_64"};
    architecture.effective = "x86_64";
    ReviewedBuildFixture fixture(
        "tracked-local", upstream, RecipeShape::Valid,
        false, true, true, true, architecture);
    EvaluatedDevelSourceBuildProof proof = build_success(fixture);
    require_supplemental_artifact(proof, fixture);
    require(
        *proof.artifact().evidence().identity.architecture.value() == "x86_64" &&
            proof.evaluated_source().git_source().selector().kind() == VcsSelectorKind::Branch &&
            proof.actual_built_revision().revision().value().git_commit() !=
                nullptr &&
            *proof.actual_built_revision()
                    .revision()
                    .value()
                    .git_commit() == upstream.oid(),
        "Reviewed local source changed the single Git workspace proof");
    cleanup_proof(proof);
}

void test_sha256_upstream_revision() {
    UpstreamGitFixture upstream(
        "sha256-upstream", GitObjectFormat::Sha256);
    ReviewedBuildFixture fixture("sha256-upstream", upstream);
    EvaluatedDevelSourceBuildProof proof = build_success(fixture);
    const SourceRevisionIdentity& revision =
        proof.actual_built_revision().revision().value();
    require(
        revision.git_commit() != nullptr &&
            revision.git_commit()->size() == 64 &&
            revision.git_object_format() != nullptr &&
            *revision.git_object_format() == GitObjectFormat::Sha256 &&
            *revision.git_commit() == upstream.oid(),
        "SHA-256 upstream repository lost its complete object format");
    cleanup_proof(proof);
}

struct FixtureNode {
    fs::path path;
    struct stat identity;
};

std::vector<FixtureNode> retained_fixture_inventory(const fs::path& root) {
    struct stat root_status{};
    require(root.is_absolute() && root.lexically_normal() == root &&
                root.filename().string().starts_with("moguet-source-build-context-") &&
                ::lstat(root.c_str(), &root_status) == 0 &&
                S_ISDIR(root_status.st_mode) && root_status.st_uid == ::geteuid(),
            "Invalid exact retained fixture root");
    std::vector<FixtureNode> nodes{{root, root_status}};
    for(const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        struct stat status{};
        require(nodes.size() < 100000 &&
                    ::lstat(entry.path().c_str(), &status) == 0 &&
                    status.st_uid == ::geteuid() && status.st_dev == root_status.st_dev &&
                    (S_ISDIR(status.st_mode) || S_ISREG(status.st_mode) || S_ISLNK(status.st_mode)),
                "Retained fixture has foreign or unsupported content");
        nodes.push_back({entry.path(), status});
    }
    std::sort(nodes.begin(), nodes.end(), [](const FixtureNode& left, const FixtureNode& right) {
        return left.path < right.path;
    });
    return nodes;
}

bool same_fixture_inventory(const std::vector<FixtureNode>& left,
                            const std::vector<FixtureNode>& right) {
    if(left.size() != right.size()) return false;
    for(std::size_t i = 0; i < left.size(); ++i) {
        if(left[i].path != right[i].path ||
           left[i].identity.st_dev != right[i].identity.st_dev ||
           left[i].identity.st_ino != right[i].identity.st_ino ||
           (left[i].identity.st_mode & S_IFMT) != (right[i].identity.st_mode & S_IFMT) ||
           left[i].identity.st_uid != right[i].identity.st_uid) return false;
    }
    return true;
}

void cleanup_retained_fixture(const fs::path& root, const struct stat& created_root) {
    // This test owns recipe execution and injections. Prove the exact created
    // root and complete owner/device/type/path inventory before any deletion;
    // remove only recorded entries, never a fresh recursive-delete adoption.
    const auto nodes = retained_fixture_inventory(root);
    require(nodes.front().path == root &&
                nodes.front().identity.st_dev == created_root.st_dev &&
                nodes.front().identity.st_ino == created_root.st_ino,
            "Retained fixture root was replaced");
    require(same_fixture_inventory(nodes, retained_fixture_inventory(root)),
            "Retained fixture inventory drifted before fixture cleanup");
    for(const auto& node : nodes) {
        if(S_ISDIR(node.identity.st_mode)) {
            require(::chmod(node.path.c_str(), 0700) == 0, "Cannot access owned fixture directory");
        }
    }
    for(auto node = nodes.rbegin(); node != nodes.rend(); ++node) {
        struct stat current{};
        require(::lstat(node->path.c_str(), &current) == 0 &&
                    current.st_ino == node->identity.st_ino && current.st_dev == node->identity.st_dev &&
                    current.st_uid == node->identity.st_uid &&
                    (current.st_mode & S_IFMT) == (node->identity.st_mode & S_IFMT),
                "Fixture entry changed before removal");
        require((S_ISDIR(current.st_mode) ? ::rmdir(node->path.c_str()) : ::unlink(node->path.c_str())) == 0,
                "Failed to remove exact owned fixture entry");
    }
}

void expect_failure(
    ReviewedBuildFixture& fixture,
    EvaluatedDevelSourceBuildFailureReason expected_reason,
    bool retains_unproven_content = false,
    const std::function<void(const fs::path&)>& inspect_retained = {}) {
    InvocationOwnedSourceBuildContext context = fixture.make_context();
    const fs::path root = context.owned_root();
    struct stat created_root{};
    require(::lstat(root.c_str(), &created_root) == 0, "Failed to retain test root identity");
    InvocationOwnedMakepkgEnvironment environment =
        fixture.make_environment(context);
    EvaluatedDevelSourceBuildResult result =
        build_evaluated_devel_source(
            std::move(context), std::move(environment));
    const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(
        result, "Unsupported fixture produced a proof");
    require(failure.reason == expected_reason,
            "Unsupported fixture returned a different typed failure: expected=" +
                std::to_string(static_cast<int>(expected_reason)) + " actual=" +
                std::to_string(static_cast<int>(failure.reason)) + " stage=" +
                std::to_string(static_cast<int>(failure.stage)));
    if(retains_unproven_content) {
        require(failure.cleanup_consequence.has_value() &&
                    failure.cleanup_consequence->failure.reason ==
                        InvocationOwnedSourceBuildContextFailureReason::UnprovenCleanupContent &&
                    failure.cleanup_consequence->retained_root == root && fs::exists(root),
                "Rejected content was adopted by automatic cleanup");
        if(inspect_retained) inspect_retained(root);
        cleanup_retained_fixture(root, created_root);
        return;
    }
    require(!failure.cleanup_consequence.has_value(),
            "Ordinary failure also reported cleanup failure");
    require(!fs::exists(root),
            "Ordinary Slice 4 failure left its private context");
}

void test_source_projection_fail_closed() {
    UpstreamGitFixture upstream("source-negative");
    {
        ReviewedBuildFixture fixture(
            "raw-eval", upstream,
            RecipeShape::RawEvaluatedMismatch);
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::RawEvaluatedSourceMismatch);
    }
    {
        ReviewedBuildFixture fixture(
            "multi-git", upstream, RecipeShape::MultipleGit);
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }
    {
        ReviewedBuildFixture fixture(
            "unsupported-vcs", upstream,
            RecipeShape::UnsupportedVcs);
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }
    {
        ReviewedBuildFixture fixture(
            "unsupported-selector", upstream,
            RecipeShape::UnsupportedSelector);
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::UnsupportedSourceShape);
    }
}

void test_supplemental_input_fail_closed() {
    using Reason = EvaluatedDevelSourceBuildFailureReason;
    UpstreamGitFixture upstream("supplemental-negative");
    for(const auto shape : {RecipeShape::MissingLocal, RecipeShape::DirectoryLocal}) {
        ReviewedBuildFixture fixture("local-input", upstream, shape, false, false, true);
        expect_failure(fixture, Reason::UnsupportedSourceShape);
        std::cout << "S564 missing/non-regular local input rejected PASS\n";
    }
    for(const bool prepared : {false, true}) {
        for(const std::string change : {"addition", "deletion", "replacement"}) {
            ArchitectureFixture architecture;
            const std::string mutation = change == "addition"   ? "source+=('extra.patch'); sha256sums+=('SKIP')"
                                         : change == "deletion" ? "unset 'source[1]' 'sha256sums[1]'; source=(\"${source[@]}\"); sha256sums=(\"${sha256sums[@]}\")"
                                                                : "source[1]='config.toml'";
            architecture.recipe_suffix = prepared ? "if [[ $pkgver != 0 ]]; then " + mutation + "; fi\n" : mutation + "\n";
            ReviewedBuildFixture fixture("source-drift", upstream, RecipeShape::Valid, false, false, true, true, architecture);
            expect_failure(fixture, Reason::RawEvaluatedSourceMismatch);
            std::cout << "S564 " << (prepared ? "prepared" : "initial") << " source " << change << " rejected PASS\n";
        }
    }
    ReviewedBuildFixture fixture("local-replacement", upstream, RecipeShape::Valid, false, false, true);
    bool replaced = false;
    set_evaluated_devel_source_build_test_hook(
        [&](EvaluatedDevelSourceBuildTestEvent event, const fs::path& root, const fs::path&) {
            if(event != EvaluatedDevelSourceBuildTestEvent::AfterSourcePreparation) return;
            const auto input = root / "build/.moguet-evaluated-recipe/config.toml";
            fs::permissions(input, fs::perms::owner_write, fs::perm_options::add);
            {
                std::ofstream file(input);
                file << "setting = \"unreviewed-config\"\n";
                file.close();
                require(file.good(), "Cannot replace working supplemental input");
            }
            fs::permissions(input, fs::perms::owner_read, fs::perm_options::replace);
            replaced = true;
        });
    expect_failure(fixture, Reason::WorkingRecipeFailure);
    set_evaluated_devel_source_build_test_hook({});
    require(replaced, "Supplemental replacement boundary was not reached");
    std::cout << "S564 reviewed local bytes replaced after preparation rejected PASS\n";
}

void test_declared_architecture_outputs() {
    UpstreamGitFixture upstream("architecture-positive");
    struct Case {
        std::string label;
        std::vector<std::string> declared;
        std::string effective;
        std::string expected;
        std::string extension;
    };
    for(const Case& entry : std::vector<Case>{
            {"multiple", {"i686", "x86_64"}, "x86_64", "x86_64", ".pkg.tar"},
            {"reversed", {"x86_64", "i686"}, "x86_64", "x86_64", ".pkg.tar.gz"},
            {"other-context", {"x86_64", "i686"}, "i686", "i686", ".pkg.tar.xz"},
            {"singleton", {"x86_64"}, "x86_64", "x86_64", ".pkg.tar.zst"},
            {"independent", {"any"}, "i686", "any", ".pkg.tar"},
            {"unfamiliar", {"fixture_cpu"}, "fixture_cpu", "fixture_cpu", ".pkg.tar"}}) {
        ArchitectureFixture architecture;
        architecture.declared = entry.declared;
        architecture.reviewed = entry.declared;
        std::reverse(architecture.reviewed->begin(), architecture.reviewed->end());
        architecture.effective = entry.effective;
        architecture.extension = entry.extension;
        architecture.epoch = "3";
        // pkgver() changes pkgrel to 1 during preparation; restore a dotted
        // release in the post-prepare evaluations to exercise exact identity.
        architecture.pkgrel = "1.2";
        architecture.recipe_suffix = "if [[ $pkgver != 0 ]]; then pkgrel=1.2; fi\n";
        architecture.package_commands =
            "    printf 'package-built\\n' >> \"$srcdir/$pkgname/payload.txt\"\n";
        ReviewedBuildFixture fixture(
            "arch-" + entry.label, upstream, RecipeShape::Valid,
            true, false, false, true, architecture);
        auto proof = build_success(fixture);
        const auto& identity = proof.artifact().evidence().identity;
        require(identity.architecture.state() == ArtifactMetadataValueState::Known &&
                    identity.architecture.value() && *identity.architecture.value() == entry.expected,
                "Selected architecture did not reach retained archive evidence");
        require(identity.full_version.starts_with("3:1.r") && identity.full_version.ends_with("-1.2") &&
                    proof.artifact().path().filename() ==
                        fixture.package_name() + "-" + identity.full_version + "-" + entry.expected + entry.extension,
                "Packagelist identity lost epoch, version dots, package hyphens or PKGEXT");
        require(archive_member(proof.artifact().path(), "usr/share/" + fixture.package_name() + "/payload.txt") ==
                        "revision-one\nprepared\npackage-built\n" &&
                    proof.evaluated_source().source_count() == 1 &&
                    *proof.actual_built_revision().revision().value().git_commit() == upstream.oid(),
                "Tree-sitter-shaped prepare/pkgver/package build evidence differs");
        cleanup_proof(proof);
        std::cout << "S4 architecture " << entry.label << " / selected=" << entry.expected << " PASS\n";
    }
}

void test_architecture_declaration_rejection() {
    using Reason = EvaluatedDevelSourceBuildFailureReason;
    UpstreamGitFixture upstream("architecture-negative");
    for(const std::string kind : {"initial-drift", "prepared-drift", "child-drift", "empty-child",
                                  "duplicate", "mixed-any", "malformed", "empty", "outside-set", "qualified-source"}) {
        ArchitectureFixture architecture;
        architecture.declared = {"i686", "x86_64"};
        architecture.effective = "x86_64";
        Reason reason = Reason::RawEvaluatedSourceMismatch;
        if(kind == "initial-drift") architecture.reviewed = {{"x86_64"}};
        if(kind == "prepared-drift") architecture.recipe_suffix = "if [[ $pkgver != 0 ]]; then arch=('x86_64'); fi\n";
        if(kind == "child-drift") architecture.reviewed_child_arch = "\tarch = x86_64\n";
        if(kind == "empty-child") {
            architecture.reviewed_child_arch = "\tarch =\n";
            reason = Reason::UnsupportedSourceShape;
        }
        if(kind == "duplicate") architecture.reviewed = {{"x86_64", "x86_64"}};
        if(kind == "mixed-any") architecture.reviewed = {{"any", "x86_64"}};
        if(kind == "malformed") architecture.reviewed = {{"x86-64"}};
        if(kind == "empty") architecture.reviewed = std::vector<std::string>{};
        if(kind == "duplicate" || kind == "mixed-any" || kind == "malformed" || kind == "empty") {
            reason = Reason::EvaluatedSourceFailure;
        }
        if(kind == "outside-set") {
            architecture.declared = {"i686"};
            reason = Reason::MakepkgPhaseFailure;
        }
        if(kind == "qualified-source") {
            // The source itself is unchanged; only its qualifier differs.
            architecture.recipe_suffix = "source_x86_64=(\"${source[@]}\"); source=(); sha256sums_x86_64=('SKIP'); sha256sums=()\n";
            architecture.qualified_source = true;
            reason = Reason::UnsupportedSourceShape;
        }
        ReviewedBuildFixture fixture("arch-" + kind, upstream, RecipeShape::Valid,
                                     false, false, false, true, architecture);
        expect_failure(fixture, reason);
        std::cout << "S4 architecture declaration " << kind << " rejected PASS\n";
    }
}

void test_packagelist_output_rejection() {
    using Reason = EvaluatedDevelSourceBuildFailureReason;
    UpstreamGitFixture upstream("packagelist-negative");
    const std::string valid = "$PKGDEST/$pkgname-$pkgver-$pkgrel-x86_64.pkg.tar";
    for(const auto& [kind, output] : std::vector<std::pair<std::string, std::string>>{
            {"empty", ""}, {"multiple", valid + "\\n" + valid}, {"relative", "relative.pkg.tar"}, {"outside", "/tmp/outside.pkg.tar"}, {"traversal", "$PKGDEST/../unexpected.pkg.tar"}, {"wrong-child", "$PKGDEST/unexpected-$pkgver-$pkgrel-x86_64.pkg.tar"}, {"wrong-version", "$PKGDEST/$pkgname-0-1-x86_64.pkg.tar"}, {"wrong-extension", "$PKGDEST/$pkgname-$pkgver-$pkgrel-x86_64.tar.zst"}, {"missing-arch", "$PKGDEST/$pkgname-$pkgver-$pkgrel-.pkg.tar"}, {"malformed-arch", "$PKGDEST/$pkgname-$pkgver-$pkgrel-x86-64.pkg.tar"}, {"outside-arch", "$PKGDEST/$pkgname-$pkgver-$pkgrel-aarch64.pkg.tar"}}) {
        ArchitectureFixture architecture;
        architecture.declared = {"i686", "x86_64"};
        architecture.effective = "x86_64";
        // Deliberately corrupt only the real makepkg process's packagelist
        // output. This fixture does not inject a selected arch or S4 proof.
        architecture.recipe_suffix = "if (( PACKAGELIST )); then printf \"" + output + "\\n\"; exit 0; fi\n";
        ReviewedBuildFixture fixture("packagelist-" + kind, upstream, RecipeShape::Valid,
                                     false, false, false, true, architecture);
        const bool invalid_arch = kind == "missing-arch" || kind == "malformed-arch" || kind == "outside-arch";
        expect_failure(fixture, invalid_arch ? Reason::UnsupportedSourceShape : Reason::DynamicVersionUnavailable);
        std::cout << "S4 packagelist " << kind << " rejected PASS\n";
    }
}

void test_selected_architecture_archive_drift() {
    using Reason = EvaluatedDevelSourceBuildFailureReason;
    UpstreamGitFixture upstream("archive-arch-drift");
    for(const std::string kind : {"path-drift", "metadata-drift", "any-drift"}) {
        ArchitectureFixture architecture;
        architecture.declared = kind == "any-drift" ? std::vector<std::string>{"any"}
                                                    : std::vector<std::string>{"i686", "x86_64"};
        architecture.effective = "x86_64";
        architecture.extension = ".pkg.tar";
        architecture.package_commands = "    CARCH=i686\n";
        if(kind == "any-drift") architecture.package_commands += "    printf -v 'arch[0]' '%s' i686\n";
        ReviewedBuildFixture fixture("archive-" + kind, upstream, RecipeShape::Valid,
                                     false, false, false, true, architecture);
        bool observed = false;
        set_evaluated_devel_source_build_test_hook(
            [&](EvaluatedDevelSourceBuildTestEvent event, const fs::path& root, const fs::path&) {
                if(event != EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild) return;
                const fs::path archive = fs::directory_iterator(root / "pkgdest")->path();
                require(std::distance(fs::directory_iterator(root / "pkgdest"), fs::directory_iterator{}) == 1, "Arch drift fixture artifact cardinality differs");
                require(archive_member(archive, ".PKGINFO").find("\narch = i686\n") != std::string::npos,
                        "package() CARCH drift did not change actual archive metadata");
                observed = true;
                if(kind != "path-drift") {
                    // Preserve the expected leaf to reach the retained-FD
                    // metadata guard; mere membership in {i686,x86_64} fails.
                    std::string leaf = archive.filename().string();
                    const auto offset = leaf.rfind("-i686.pkg.tar");
                    require(offset != std::string::npos, "Arch drift output name differs");
                    leaf.replace(offset, std::string::npos, kind == "any-drift" ? "-any.pkg.tar" : "-x86_64.pkg.tar");
                    fs::rename(archive, archive.parent_path() / leaf);
                }
            });
        expect_failure(fixture, kind == "path-drift" ? Reason::ArtifactInventoryMismatch : Reason::ArtifactMetadataMismatch, true);
        set_evaluated_devel_source_build_test_hook({});
        require(observed, "Archive drift never reached package build");
        std::cout << "S4 selected/archive " << kind << " rejected PASS\n";
    }
}

struct BuiltObservation {
    std::string oid;
    std::string version;
};

BuiltObservation build_observation(
    std::string label,
    UpstreamGitFixture& upstream) {
    ReviewedBuildFixture fixture(std::move(label), upstream);
    EvaluatedDevelSourceBuildProof proof = build_success(fixture);
    const std::string* oid = proof.actual_built_revision()
                                 .revision()
                                 .value()
                                 .git_commit();
    require(oid != nullptr, "Dynamic fixture has no Git OID");
    BuiltObservation result{
        *oid, proof.artifact().evidence().identity.full_version};
    cleanup_proof(proof);
    return result;
}

void test_two_upstream_revisions_change_dynamic_identity() {
    UpstreamGitFixture upstream("two-revisions");
    const BuiltObservation first =
        build_observation("dynamic-first", upstream);
    upstream.commit("revision-two\n");
    const BuiltObservation second =
        build_observation("dynamic-second", upstream);
    require(first.oid != second.oid,
            "Two upstream revisions produced one actual Git OID");
    require(first.version != second.version,
            "Dynamic package identity did not follow the actual revision");
    require(
        first.version.find(first.oid.substr(0, 12)) !=
                std::string::npos &&
            second.version.find(second.oid.substr(0, 12)) !=
                std::string::npos,
        "Dynamic versions are not tied to their complete Git proofs");
}

void test_dynamic_version_drift_fails_closed() {
    UpstreamGitFixture upstream("dynamic-drift");
    ReviewedBuildFixture fixture(
        "dynamic-drift", upstream,
        RecipeShape::DynamicVersionDrift);
    expect_failure(
        fixture,
        EvaluatedDevelSourceBuildFailureReason::DynamicVersionUnavailable, true);
}

void write_file(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Failed to open injected file");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "Failed to finish injected file");
}

void test_git_replacement_and_grafts_rejected() {
    UpstreamGitFixture upstream("git-replacement");
    // Raw non-commit objects must never become commit evidence through
    // replacement interpretation, including a packed-only replacement ref.
    for(const std::string kind : {"blob", "tree", "tag", "packed", "reftable",
                                  "drift", "mirror-graft", "worktree-graft"}) {
        ReviewedBuildFixture fixture("replace-" + kind, upstream);
        bool injected = false;
        set_evaluated_devel_source_build_test_hook(
            [&](EvaluatedDevelSourceBuildTestEvent event,
                const fs::path& root, const fs::path&) {
                const auto injection_event = kind == "drift"
                                                 ? EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild
                                                 : EvaluatedDevelSourceBuildTestEvent::AfterSourcePreparation;
                if(event != injection_event) return;
                const fs::path mirror = root / "srcdest" / fixture.package_name();
                const fs::path workspace = root / "build" / fixture.package_base() /
                                           "src" / fixture.package_name();
                const auto environment = git_environment(fixture.home());
                auto git_line = [&](std::vector<std::string> arguments) {
                    return require_output_line(capture_process(
                        "/usr/bin/git", std::move(arguments), environment, &workspace));
                };
                const std::string commit = git_line({"rev-parse", "HEAD"});
                injected = true;
                if(kind == "mirror-graft" || kind == "worktree-graft") {
                    const fs::path metadata = kind == "mirror-graft" ? mirror : workspace / ".git";
                    fs::create_directories(metadata / "info");
                    write_file(metadata / "info/grafts", commit + "\n");
                    return;
                }
                std::string raw_oid;
                if(kind == "tag") {
                    require_process_success("/usr/bin/git",
                                            {"tag", "-a", "replacement-tag", "-m", "replacement"},
                                            environment, &workspace);
                    raw_oid = git_line({"rev-parse", "refs/tags/replacement-tag"});
                    const fs::path object = fs::path("objects") / raw_oid.substr(0, 2) / raw_oid.substr(2);
                    fs::create_directories((mirror / object).parent_path());
                    fs::copy_file(workspace / ".git" / object, mirror / object);
                } else {
                    raw_oid = git_line({"rev-parse", kind == "tree" ? "HEAD^{tree}" : "HEAD:payload.txt"});
                }
                const std::string raw_type = kind == "tree" ? "tree" : kind == "tag" ? "tag"
                                                                                     : "blob";
                for(const fs::path& metadata : {mirror, workspace / ".git"}) {
                    fs::create_directories(metadata / "refs/replace");
                    write_file(metadata / "refs/replace" / raw_oid, commit + "\n");
                    if(kind == "packed") {
                        std::ifstream packed_input(metadata / "packed-refs");
                        const std::string packed_bytes((std::istreambuf_iterator<char>(packed_input)), {});
                        write_file(metadata / "packed-refs", packed_bytes +
                                                                 commit + " refs/replace/" + raw_oid + "\n");
                        require(fs::remove(metadata / "refs/replace" / raw_oid), "Remove exact replacement fixture ref");
                        require(fs::remove(metadata / "refs/replace"), "Remove exact empty replacement fixture directory");
                    }
                }
                require(git_line({"--no-replace-objects", "cat-file", "-t", raw_oid}) == raw_type,
                        "Replacement fixture lost raw object type");
                require(git_line({"cat-file", "-t", raw_oid}) == "commit",
                        "Fixture did not reproduce replacement type substitution");
                if(kind == "reftable") {
                    for(const fs::path& repository : {mirror, workspace}) {
                        require_process_success("/usr/bin/git",
                                                {"refs", "migrate", "--ref-format=reftable"},
                                                environment, &repository);
                    }
                    return;
                }
                if(kind == "drift") return;
                const std::string head = git_line({"symbolic-ref", "HEAD"});
                write_file(mirror / "refs/heads/main", raw_oid + "\n");
                write_file(workspace / ".git/refs/remotes/origin/main", raw_oid + "\n");
                write_file(workspace / ".git" / head, raw_oid + "\n");
                const fs::path recipe = root / "build/.moguet-evaluated-recipe/PKGBUILD";
                std::ifstream input(recipe);
                std::string bytes((std::istreambuf_iterator<char>(input)), {});
                const std::size_t offset = bytes.find(commit.substr(0, 12));
                require(offset != std::string::npos, "Missing dynamic version in replacement fixture");
                bytes.replace(offset, 12, raw_oid.substr(0, 12));
                write_file(recipe, bytes);
            });
        expect_failure(fixture, EvaluatedDevelSourceBuildFailureReason::GitRepositoryInvalid, kind == "drift");
        set_evaluated_devel_source_build_test_hook({});
        require(injected, "Git metadata regression did not reach injection point");
    }
}

void test_stale_pkgdest_and_extra_artifact() {
    UpstreamGitFixture upstream("inventory-negative");
    {
        ReviewedBuildFixture fixture("stale-pkgdest", upstream);
        InvocationOwnedSourceBuildContext context = fixture.make_context();
        const fs::path root = context.owned_root();
        struct stat created_root{};
        require(::lstat(root.c_str(), &created_root) == 0, "Missing stale fixture root");
        write_file(context.pkgdest() / "stale.pkg.tar.zst", "stale");
        InvocationOwnedMakepkgEnvironment environment =
            fixture.make_environment(context);
        EvaluatedDevelSourceBuildResult result =
            build_evaluated_devel_source(
                std::move(context), std::move(environment));
        require(
            require_arm<EvaluatedDevelSourceBuildFailure>(
                result, "Stale PKGDEST produced a proof")
                    .reason ==
                EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch,
            "Stale PKGDEST returned the wrong failure");
        const auto& failure = std::get<EvaluatedDevelSourceBuildFailure>(result);
        require(failure.cleanup_consequence.has_value() &&
                    failure.cleanup_consequence->retained_root == root &&
                    fs::file_size(root / "pkgdest/stale.pkg.tar.zst") == 5,
                "Stale artifact was deleted by automatic cleanup");
        cleanup_retained_fixture(root, created_root);
    }
    {
        ReviewedBuildFixture fixture("extra-artifact", upstream);
        set_evaluated_devel_source_build_test_hook(
            [](EvaluatedDevelSourceBuildTestEvent event,
               const fs::path& root,
               const fs::path&) {
                if(event ==
                   EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild) {
                    write_file(
                        root / "pkgdest" / "extra.pkg.tar.zst",
                        "extra");
                }
            });
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch, true,
            [](const fs::path& root) {
                require(sha256_path(root / "pkgdest/extra.pkg.tar.zst") ==
                            xdg_generation_store_raw_contents_sha256("extra"),
                        "Cleanup deleted or changed the unexpected sibling");
            });
        set_evaluated_devel_source_build_test_hook({});
    }
    {
        ReviewedBuildFixture fixture("partial-artifact", upstream);
        set_evaluated_devel_source_build_test_hook(
            [](EvaluatedDevelSourceBuildTestEvent event, const fs::path& root, const fs::path&) {
                if(event != EvaluatedDevelSourceBuildTestEvent::AfterSourcePreparation) return;
                write_file(root / "pkgdest/partial.pkg.tar", "partial");
                throw std::runtime_error("Unrelated preparation failure");
            });
        expect_failure(fixture, EvaluatedDevelSourceBuildFailureReason::InternalFailure, true,
                       [](const fs::path& root) {
                           require(fs::file_size(root / "pkgdest/partial.pkg.tar") == 7,
                                   "Unproven partial output was adopted for cleanup");
                       });
        set_evaluated_devel_source_build_test_hook({});
    }
}

void test_artifact_replacement() {
    UpstreamGitFixture upstream("replacement");
    for(const auto point : {EvaluatedDevelSourceBuildTestEvent::AfterArtifactInventory,
                            EvaluatedDevelSourceBuildTestEvent::AfterArtifactOpen,
                            EvaluatedDevelSourceBuildTestEvent::BeforeFinalArtifactReproof}) {
        ReviewedBuildFixture fixture("replacement", upstream);
        fs::path replaced_path;
        std::string original_digest;
        set_evaluated_devel_source_build_test_hook(
            [&](EvaluatedDevelSourceBuildTestEvent event, const fs::path&, const fs::path& artifact) {
                if(event != point) return;
                replaced_path = artifact;
                original_digest = sha256_path(artifact);
                fs::rename(artifact, artifact.parent_path() / "displaced");
                write_file(artifact, "replacement");
            });
        expect_failure(fixture, EvaluatedDevelSourceBuildFailureReason::ArtifactReplacement, true,
                       [&](const fs::path& root) {
                           require(replaced_path.parent_path() == root / "pkgdest" &&
                                       fs::file_size(replaced_path) == 11 &&
                                       sha256_path(replaced_path) == xdg_generation_store_raw_contents_sha256("replacement") &&
                                       sha256_path(root / "pkgdest/displaced") == original_digest,
                                   "Cleanup touched replacement/displaced bytes");
                       });
        set_evaluated_devel_source_build_test_hook({});
    }
}

void test_ambiguous_workspace_and_artifact_hardlink() {
    UpstreamGitFixture upstream("ambiguity-hardlink");
    {
        ReviewedBuildFixture fixture("ambiguous-workspace", upstream);
        set_evaluated_devel_source_build_test_hook(
            [](EvaluatedDevelSourceBuildTestEvent event,
               const fs::path& root,
               const fs::path&) {
                if(event ==
                   EvaluatedDevelSourceBuildTestEvent::AfterSourcePreparation) {
                    fs::create_directories(
                        root / "build" / "ambiguous" / ".git");
                }
            });
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::SourceWorkspaceAmbiguous, true);
        set_evaluated_devel_source_build_test_hook({});
    }
    {
        ReviewedBuildFixture fixture("artifact-hardlink", upstream);
        const fs::path external_link =
            fixture.home() / "external-artifact-hardlink";
        set_evaluated_devel_source_build_test_hook(
            [external_link](EvaluatedDevelSourceBuildTestEvent event,
                            const fs::path& root,
                            const fs::path&) {
                if(event !=
                   EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild) {
                    return;
                }
                std::vector<fs::path> artifacts;
                for(const fs::directory_entry& entry :
                    fs::directory_iterator(root / "pkgdest")) {
                    if(entry.is_regular_file()) {
                        artifacts.push_back(entry.path());
                    }
                }
                require(artifacts.size() == 1,
                        "Hardlink hook did not find one artifact");
                fs::create_hard_link(artifacts.front(), external_link);
            });
        expect_failure(
            fixture,
            EvaluatedDevelSourceBuildFailureReason::ArtifactInventoryMismatch, true);
        set_evaluated_devel_source_build_test_hook({});
    }
}

fs::path build_foreign_artifact(TemporaryTree& tree) {
    const fs::path recipe = tree.path() / "foreign-recipe";
    const fs::path pkgdest = tree.path() / "foreign-pkgdest";
    const fs::path builddir = tree.path() / "foreign-build";
    const fs::path srcdest = tree.path() / "foreign-srcdest";
    const fs::path home = tree.path() / "foreign-home";
    for(const fs::path& directory :
        {recipe, pkgdest, builddir, srcdest, home}) {
        fs::create_directory(directory);
    }
    write_file(
        recipe / "PKGBUILD",
        "pkgname=moguet-foreign-artifact\n"
        "pkgver=9\n"
        "pkgrel=1\n"
        "pkgdesc='foreign'\n"
        "arch=('any')\n"
        "license=('GPL-3.0-or-later')\n"
        "package() { install -dm755 \"$pkgdir/usr/share/moguet-foreign-artifact\"; }\n");
    std::vector<std::string> environment{
        "HOME=" + home.string(),
        "PATH=/usr/bin:/bin",
        "LANG=C",
        "LC_ALL=C",
        "MAKEPKG_LIBRARY=/usr/share/makepkg",
        "PKGDEST=" + pkgdest.string(),
        "BUILDDIR=" + builddir.string(),
        "SRCDEST=" + srcdest.string(),
    };
    require_process_success(
        "/usr/bin/makepkg", {"--nodeps", "--noconfirm"},
        environment, &recipe);
    std::vector<fs::path> artifacts;
    for(const fs::directory_entry& entry :
        fs::directory_iterator(pkgdest)) {
        if(entry.is_regular_file()) artifacts.push_back(entry.path());
    }
    require(artifacts.size() == 1,
            "Foreign fixture did not build exactly one artifact");
    return artifacts.front();
}

void test_artifact_metadata_mismatch() {
    UpstreamGitFixture upstream("metadata-mismatch");
    ReviewedBuildFixture fixture("metadata-mismatch", upstream);
    TemporaryTree foreign_tree("foreign-artifact");
    const fs::path foreign = build_foreign_artifact(foreign_tree);
    set_evaluated_devel_source_build_test_hook(
        [foreign](EvaluatedDevelSourceBuildTestEvent event,
                  const fs::path& root,
                  const fs::path&) {
            if(event !=
               EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild) {
                return;
            }
            std::vector<fs::path> artifacts;
            for(const fs::directory_entry& entry :
                fs::directory_iterator(root / "pkgdest")) {
                if(entry.is_regular_file()) artifacts.push_back(entry.path());
            }
            require(artifacts.size() == 1,
                    "Metadata hook did not find one artifact");
            fs::copy_file(
                foreign, artifacts.front(),
                fs::copy_options::overwrite_existing);
        });
    expect_failure(
        fixture,
        EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataMismatch, true);
    set_evaluated_devel_source_build_test_hook({});
}

void test_cross_context_environment_rejected() {
    UpstreamGitFixture upstream("cross-context");
    ReviewedBuildFixture fixture("cross-context", upstream);
    InvocationOwnedSourceBuildContext first = fixture.make_context();
    InvocationOwnedSourceBuildContext second = fixture.make_context();
    const fs::path first_root = first.owned_root();
    const fs::path second_root = second.owned_root();
    InvocationOwnedMakepkgEnvironment second_environment =
        fixture.make_environment(second);
    EvaluatedDevelSourceBuildResult result =
        build_evaluated_devel_source(
            std::move(first), std::move(second_environment));
    require(
        require_arm<EvaluatedDevelSourceBuildFailure>(
            result, "Cross-context environment produced a proof")
                .reason ==
            EvaluatedDevelSourceBuildFailureReason::EnvironmentLineageMismatch,
        "Cross-context environment returned the wrong failure");
    require(!fs::exists(first_root),
            "Rejected context A was not cleaned");
    InvocationOwnedSourceBuildContextCleanupResult cleanup =
        second.cleanup();
    require(
        std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(
            cleanup) &&
            !fs::exists(second_root),
        "Context B cleanup failed");
}

void test_malformed_archive_metadata_taxonomy() {
    UpstreamGitFixture upstream("malformed-metadata");
    for(const std::string kind : {"missing-name", "invalid-name", "invalid-version"}) {
        ReviewedBuildFixture fixture("malformed-" + kind, upstream);
        TemporaryTree archive_tree("malformed-" + kind);
        std::string pkginfo = kind == "missing-name"
                                  ? "pkgver = 1-1\n"
                                  : "pkgname = " + (kind == "invalid-name" ? std::string("bad/name") : fixture.package_name()) +
                                        "\npkgver = " + (kind == "invalid-version" ? "bad version" : "1-1") + "\n";
        write_file(archive_tree.path() / ".PKGINFO", pkginfo);
        write_file(archive_tree.path() / ".MTREE", "#mtree\n");
        const fs::path archive = archive_tree.path() / "malformed.pkg.tar";
        require_process_success("/usr/bin/bsdtar",
                                {"-cf", archive.string(), ".PKGINFO", ".MTREE"},
                                git_environment(fixture.home()), &archive_tree.path());
        bool injected = false;
        set_evaluated_devel_source_build_test_hook(
            [&](EvaluatedDevelSourceBuildTestEvent event, const fs::path& root, const fs::path&) {
                if(event != EvaluatedDevelSourceBuildTestEvent::AfterPackageBuild) return;
                std::vector<fs::path> artifacts;
                for(const auto& entry : fs::directory_iterator(root / "pkgdest"))
                    artifacts.push_back(entry.path());
                require(artifacts.size() == 1, "Malformed fixture did not find one built artifact");
                fs::copy_file(archive, artifacts.front(), fs::copy_options::overwrite_existing);
                injected = true;
            });
        auto context = fixture.make_context();
        const fs::path root = context.owned_root();
        struct stat created_root{};
        require(::lstat(root.c_str(), &created_root) == 0, "Missing malformed fixture root");
        auto environment = fixture.make_environment(context);
        const auto result = build_evaluated_devel_source(std::move(context), std::move(environment));
        set_evaluated_devel_source_build_test_hook({});
        const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(result, "Malformed metadata produced a proof");
        require(injected && failure.stage == EvaluatedDevelSourceBuildStage::ArtifactMetadata &&
                    failure.reason == EvaluatedDevelSourceBuildFailureReason::ArtifactMetadataQueryFailure &&
                    failure.diagnostic.has_value() &&
                    (failure.diagnostic == "Failed to read package archive metadata with libalpm." ||
                     failure.diagnostic == "Package archive metadata contains an invalid name or version.") &&
                    failure.cleanup_consequence.has_value() &&
                    failure.cleanup_consequence->failure.reason == InvocationOwnedSourceBuildContextFailureReason::UnprovenCleanupContent &&
                    failure.cleanup_consequence->retained_root == root,
                "Archive query failure lost artifact stage, original diagnostic, or cleanup refusal");
        require(std::distance(fs::directory_iterator(root / "pkgdest"), fs::directory_iterator{}) == 1,
                "Malformed artifact was deleted during failure cleanup");
        cleanup_retained_fixture(root, created_root);
    }
}

void test_cleanup_failure_preserves_primary() {
    UpstreamGitFixture upstream("cleanup-failure");
    ReviewedBuildFixture fixture("cleanup-failure", upstream, RecipeShape::RawEvaluatedMismatch);
    InvocationOwnedSourceBuildContext context = fixture.make_context();
    const fs::path root = context.owned_root();
    struct stat created_root{};
    require(::lstat(root.c_str(), &created_root) == 0, "Missing cleanup fixture root");
    InvocationOwnedMakepkgEnvironment environment =
        fixture.make_environment(context);
    bool injected = false;
    set_invocation_owned_source_build_context_test_hook(
        [&injected](InvocationOwnedSourceBuildContextTestEvent event,
                    const fs::path& owned_root) {
            if(event ==
                   InvocationOwnedSourceBuildContextTestEvent::BeforeCleanup &&
               !injected) {
                injected = true;
                write_file(owned_root / "unexpected", "cleanup blocker");
            }
        });
    EvaluatedDevelSourceBuildResult result =
        build_evaluated_devel_source(
            std::move(context), std::move(environment));
    set_invocation_owned_source_build_context_test_hook({});
    const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(
        result, "Cleanup-failure fixture produced a proof");
    require(
        failure.reason ==
                EvaluatedDevelSourceBuildFailureReason::RawEvaluatedSourceMismatch &&
            failure.cleanup_consequence.has_value() &&
            failure.cleanup_consequence->failure.reason ==
                InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement,
        "Cleanup failure replaced or lost the primary failure");
    require(fs::exists(root),
            "Injected cleanup failure did not retain the exact test root");
    cleanup_retained_fixture(root, created_root);
}

void test_cleanup_budgets() {
    UpstreamGitFixture upstream("cleanup-budget");
    for(const std::string kind : {"exact", "entries", "depth"}) {
        ReviewedBuildFixture fixture("budget-" + kind, upstream);
        fs::path root;
        struct stat created_root{};
        std::vector<FixtureNode> inventory;
        std::size_t cleanup_attempts = 0;
        {
            InvocationOwnedSourceBuildContext context = fixture.make_context();
            root = context.owned_root();
            require(::lstat(root.c_str(), &created_root) == 0, "Missing budget fixture root");
            fs::create_directories(context.builddir() / "one/two/three");
            write_file(context.builddir() / "one/two/three/payload", "budget fixture");
            inventory = retained_fixture_inventory(root);
            const std::size_t entries = inventory.size() - 1;
            std::size_t depth = 0;
            for(const auto& node : inventory) {
                if(node.path == root) continue;
                const auto relative = node.path.lexically_relative(root);
                depth = std::max(depth, static_cast<std::size_t>(std::distance(relative.begin(), relative.end())));
            }
            set_invocation_owned_source_build_context_cleanup_limits_for_test(
                std::pair{entries - (kind == "entries" ? 1U : 0U),
                          depth - (kind == "depth" ? 1U : 0U)});
            set_invocation_owned_source_build_context_test_hook(
                [&](InvocationOwnedSourceBuildContextTestEvent event, const fs::path&) {
                    if(event == InvocationOwnedSourceBuildContextTestEvent::BeforeCleanup) ++cleanup_attempts;
                });
            const auto cleanup = context.cleanup();
            if(kind == "exact") {
                require(std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(cleanup) && !fs::exists(root),
                        "Exactly-at-limit cleanup failed");
            } else {
                const auto& failure = require_arm<InvocationOwnedSourceBuildContextFailure>(cleanup, "Budget overflow was accepted");
                require(failure.stage == InvocationOwnedSourceBuildContextStage::Cleanup &&
                            failure.reason == InvocationOwnedSourceBuildContextFailureReason::CleanupResourceLimitExceeded &&
                            same_fixture_inventory(inventory, retained_fixture_inventory(root)),
                        "Budget refusal lost type or started deleting entries");
                set_invocation_owned_source_build_context_cleanup_limits_for_test(std::nullopt);
                require(std::get<InvocationOwnedSourceBuildContextFailure>(context.cleanup()).reason == failure.reason &&
                            cleanup_attempts == 1,
                        "Explicit cleanup retried after budget refusal");
            }
            set_invocation_owned_source_build_context_cleanup_limits_for_test(std::nullopt);
        }
        require(cleanup_attempts == 1, "Destructor retried refused cleanup");
        set_invocation_owned_source_build_context_test_hook({});
        if(kind != "exact") {
            require(same_fixture_inventory(inventory, retained_fixture_inventory(root)),
                    "Destructor deleted a budget-refused entry");
            cleanup_retained_fixture(root, created_root);
        }
    }

    ReviewedBuildFixture fixture("budget-primary", upstream, RecipeShape::RawEvaluatedMismatch);
    InvocationOwnedSourceBuildContext context = fixture.make_context();
    const fs::path root = context.owned_root();
    struct stat created_root{};
    require(::lstat(root.c_str(), &created_root) == 0, "Missing primary budget root");
    auto environment = fixture.make_environment(context);
    set_invocation_owned_source_build_context_cleanup_limits_for_test(std::pair<std::size_t, std::size_t>{4, 8});
    const auto result = build_evaluated_devel_source(std::move(context), std::move(environment));
    set_invocation_owned_source_build_context_cleanup_limits_for_test(std::nullopt);
    const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(result, "Budget-primary fixture produced a proof");
    require(failure.reason == EvaluatedDevelSourceBuildFailureReason::RawEvaluatedSourceMismatch &&
                failure.cleanup_consequence.has_value() &&
                failure.cleanup_consequence->failure.reason == InvocationOwnedSourceBuildContextFailureReason::CleanupResourceLimitExceeded &&
                failure.cleanup_consequence->retained_root == root,
            "Cleanup budget refusal replaced the primary failure or lost retained location");
    cleanup_retained_fixture(root, created_root);
}

#ifdef MOGUET_TEST_EVALUATED_DEVEL_ARTIFACT_TRANSPORT
static_assert(!std::is_default_constructible_v<EvaluatedDevelSourceArtifactTransport>);
static_assert(!std::is_copy_constructible_v<EvaluatedDevelSourceArtifactTransport>);
static_assert(!std::is_copy_assignable_v<EvaluatedDevelSourceArtifactTransport>);
static_assert(std::is_nothrow_move_constructible_v<EvaluatedDevelSourceArtifactTransport>);
static_assert(!std::is_constructible_v<EvaluatedDevelSourceArtifactTransport, fs::path>);
static_assert(!std::is_constructible_v<EvaluatedDevelSourceArtifactTransport, InstalledArtifactBinding>);
static_assert(!std::is_invocable_v<decltype(prepare_evaluated_devel_source_artifact_transport),
                                   const EvaluatedDevelSourceBuildProof&>);

// Real Slice 4 producer and real sealed helper state; only the privileged
// process/pacman exec boundary is replaced. No host transaction is executed.
void test_evaluated_artifact_transport() {
    enum class Scenario { Positive,
                          DigestDrift,
                          SameSizeReplacement,
                          SameBytesReplacement,
                          CopyRace,
                          Unobserved,
                          UnknownWait,
                          ConsumeFailure,
                          SealingRefusal };
    UpstreamGitFixture upstream("slice5-bridge");
    int case_index = 0;
    int completed_cases = 0;
    for(const auto scenario : {Scenario::Positive, Scenario::DigestDrift, Scenario::SameSizeReplacement,
                               Scenario::SameBytesReplacement, Scenario::CopyRace, Scenario::Unobserved,
                               Scenario::UnknownWait, Scenario::ConsumeFailure, Scenario::SealingRefusal}) {
        const std::string label = "slice5-bridge-" + std::to_string(case_index++);
        const auto case_started = std::chrono::steady_clock::now();
        std::cout << "Slice 5 retained bridge: " << label << " START\n"
                  << std::flush;
        ReviewedBuildFixture fixture(label, upstream);
        auto proof = build_success(fixture);
        const auto artifact_path = proof.artifact().path();
        const auto saved_digest = proof.artifact().evidence().archive_digest.value();
        const auto saved_size = proof.artifact().size();
        struct stat original{};
        require(lstat(artifact_path.c_str(), &original) == 0, "Missing original Slice 4 artifact");
        TemporaryTree runtime(label);
        const int runtime_fd = open(runtime.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        require(runtime_fd >= 0, "Cannot open isolated transport runtime");
        auto store = SourceArtifactInstallTrustedStateStore::open_below_runtime_parent(runtime_fd, geteuid());
        static_cast<void>(close(runtime_fd));
        const std::string token(64, 'a');
        std::optional<SourceArtifactInstallRootPrepareRequest> request;
        int borrowed_fd = -1;
        int prepare_count = 0;
        int execute_count = 0;
        int consume_count = 0;
        int abort_count = 0;
        bool reached_copy = false;
        const auto mutate_bytes = [&](const fs::path& path) {
            std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
            char byte = 0;
            file.read(&byte, 1);
            require(file.good(), "Cannot read mutation byte");
            byte ^= 1;
            file.seekp(0);
            file.write(&byte, 1);
            file.close();
            require(!file.fail(), "Cannot mutate retained artifact bytes");
            // Same size and restored mtime are deliberately insufficient.
            timespec times[]{original.st_atim, original.st_mtim};
            require(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0, "Cannot restore fixture mtime");
        };
        const auto require_original_fd = [&] {
            struct stat retained{};
            require(borrowed_fd >= 0 && fstat(borrowed_fd, &retained) == 0 &&
                        retained.st_dev == original.st_dev && retained.st_ino == original.st_ino,
                    "Bridge lost/reopened the original retained artifact FD");
        };
        set_evaluated_devel_source_artifact_transport_test_hooks({[&](const ExplicitProcessInvocation& invocation) -> CapturedCommandResult {
                                                                      require(invocation.executable == "/usr/bin/sudo" && invocation.arguments.size() >= 4 &&
                                                                                  invocation.arguments[0] == "--" &&
                                                                                  invocation.arguments[1] == MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH,
                                                                              "Bridge bypassed the fixed privileged helper");
                                                                      const auto& verb = invocation.arguments[2];
                                                                      if(verb == "prepare") {
                                                                          ++prepare_count;
                                                                          require_original_fd();
                                                                          const std::vector<std::string> arguments(invocation.arguments.begin() + 2, invocation.arguments.end());
                                                                          const auto parsed = parse_source_artifact_install_trusted_helper_arguments(arguments);
                                                                          const auto& helper = require_arm<SourceArtifactInstallTrustedHelperInvocation>(parsed, "Invalid bridge request");
                                                                          request.emplace(SourceArtifactInstallRootPrepareRequest{
                                                                              helper.transaction_token, helper.package_base, helper.directive,
                                                                              helper.needed, helper.no_confirm, helper.artifacts});
                                                                          require(request->artifacts.size() == 1 && request->artifacts[0].artifact_index == 0 &&
                                                                                      request->artifacts[0].archive_sha256 == saved_digest &&
                                                                                      request->artifacts[0].artifact_size == saved_size &&
                                                                                      request->artifacts[0].signature_size == 0 && request->artifacts[0].signature_sha256 == "-" &&
                                                                                      !request->needed && request->no_confirm &&
                                                                                      request->directive == SourceArtifactInstallTrustedDirective::PreserveExistingReason,
                                                                                  "Bridge changed its Slice 4 input, signature absence or install policy");
                                                                          require(invocation.standard_input_fd.has_value(), "Missing sealed bridge stream");
                                                                          const int snapshot_fd = *invocation.standard_input_fd;
                                                                          constexpr int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
                                                                          require((fcntl(snapshot_fd, F_GET_SEALS) & seals) == seals &&
                                                                                      xdg_generation_store_file_descriptor_sha256(snapshot_fd, saved_size,
                                                                                                                                  SOURCE_ARTIFACT_INSTALL_MAXIMUM_ARTIFACT_BYTES) == saved_digest,
                                                                                  "Bridge did not seal the original build bytes");
                                                                          const auto response = store.prepare(*request, snapshot_fd);
                                                                          return {serialize_source_artifact_install_root_prepare_response(response, *request), 0, false};
                                                                      }
                                                                      if(verb == "execution-status")
                                                                          return {serialize_source_artifact_install_execution_observation(store.execution_status(token)), 0, false};
                                                                      require(verb == "consume", "Unexpected bridge capture verb");
                                                                      ++consume_count;
                                                                      require_original_fd();
                                                                      if(scenario == Scenario::ConsumeFailure) return {"", 1, false};
                                                                      return {store.consume(token), 0, false};
                                                                  },
                                                                  [&](const ExplicitProcessInvocation& invocation) -> ExplicitProcessExecutionResult {
                                                                      require(invocation.executable == "/usr/bin/sudo" && invocation.arguments.size() == 4 &&
                                                                                  invocation.arguments[1] == MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH,
                                                                              "Bridge ran a process outside the fixed helper");
                                                                      if(invocation.arguments[2] == "abort") {
                                                                          ++abort_count;
                                                                          store.abort(token);
                                                                          return {ExplicitProcessExecutionStatus::StartedKnownOutcome, 0};
                                                                      }
                                                                      require(invocation.arguments[2] == "execute", "Unexpected bridge execution verb");
                                                                      ++execute_count;
                                                                      require_original_fd();
                                                                      int status = 42;
                                                                      try {
                                                                          status = store.execute(token);
                                                                      } catch(const SourceArtifactInstallTrustedStateError&) {
                                                                          require(scenario == Scenario::SealingRefusal, "Unexpected helper sealing failure");
                                                                      }
                                                                      if(scenario == Scenario::UnknownWait)
                                                                          return {ExplicitProcessExecutionStatus::StartedOutcomeUnknown, std::nullopt};
                                                                      return {ExplicitProcessExecutionStatus::StartedKnownOutcome, status};
                                                                  },
                                                                  [&](int descriptor) {
                                                                      reached_copy = true;
                                                                      borrowed_fd = descriptor;
                                                                      require_original_fd();
                                                                      if(scenario == Scenario::CopyRace) mutate_bytes(artifact_path);
                                                                  }});
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) {
            require_original_fd();
            if(scenario != Scenario::Unobserved) {
                store.observe_execution(token);
                int pipe_fds[2];
                require(pipe(pipe_fds) == 0, "Cannot create test hook input");
                const auto names = request->artifacts[0].package_name + "\n";
                require(write(pipe_fds[1], names.data(), names.size()) == static_cast<ssize_t>(names.size()),
                        "Cannot write test hook input");
                static_cast<void>(close(pipe_fds[1]));
                store.record(token, pipe_fds[0]);
                static_cast<void>(close(pipe_fds[0]));
            }
            return 0;
        });
        if(scenario == Scenario::SealingRefusal) {
            set_source_artifact_install_trusted_state_test_hook([&](auto event, int, const auto&) {
                if(event == SourceArtifactInstallTrustedStateTestEvent::BeforeFinalReproof)
                    throw SourceArtifactInstallTrustedStateError(SourceArtifactInstallSealingFailure::StagedArtifactDigestMismatch,
                                                                 "injected final reproof refusal");
            });
        }
        if(scenario == Scenario::DigestDrift) mutate_bytes(artifact_path);
        if(scenario == Scenario::SameSizeReplacement || scenario == Scenario::SameBytesReplacement) {
            const auto replacement = runtime.path() / "replacement";
            fs::copy_file(artifact_path, replacement);
            if(scenario == Scenario::SameSizeReplacement) mutate_bytes(replacement);
            require(fs::file_size(replacement) == saved_size, "Replacement did not preserve size");
            if(scenario == Scenario::SameBytesReplacement)
                require(sha256_path(replacement) == saved_digest, "Same-byte replacement differs");
            fs::rename(replacement, artifact_path);
        }
        // Even a later path-based sidecar must not be adopted by this route.
        std::ofstream(artifact_path.string() + ".sig") << "not Slice 4 authority\n";
        {
            auto transport = prepare_evaluated_devel_source_artifact_transport(std::move(proof));
            require(!proof.valid() && transport.active(), "Bridge did not consume the Slice 4 proof");
            bool rejected = false;
            try {
                auto duplicate = prepare_evaluated_devel_source_artifact_transport(std::move(proof));
            } catch(const std::logic_error&) {
                rejected = true;
            }
            require(rejected, "Bridge accepted a twice-consumed build proof");
            auto moved_transport = std::move(transport);
            require(!transport.active() && moved_transport.active(), "Moved transport retained execution authority");
            require(transport.execute_for_test({true}, token).status() == SourceArtifactInstallTrustedExecutionStatus::InvalidRequest,
                    "Moved-from transport was executable");
            const auto result = moved_transport.execute_for_test({true}, token);
            const bool local_failure = scenario == Scenario::DigestDrift || scenario == Scenario::SameSizeReplacement ||
                                       scenario == Scenario::SameBytesReplacement || scenario == Scenario::CopyRace;
            using Status = SourceArtifactInstallTrustedExecutionStatus;
            const auto expected = local_failure                                                           ? Status::ArtifactSnapshotFailed
                                  : scenario == Scenario::Unobserved || scenario == Scenario::UnknownWait ? Status::OutcomeUnknown
                                  : scenario == Scenario::ConsumeFailure                                  ? Status::ConsumeFailed
                                  : scenario == Scenario::SealingRefusal                                  ? Status::ArtifactSealingFailed
                                                                                                          : Status::Complete;
            require(result.status() == expected, "Unexpected evaluated transport result");
            require(!result.expectation() && !result.observation() && !result.operation_result(),
                    "Slice 4 transport fabricated legacy cleanup authority");
            require(!moved_transport.active() && moved_transport.transaction_token() == token,
                    "Bridge lost its consumed state/token");
            require(moved_transport.execute_for_test({true}, token).status() == Status::InvalidRequest,
                    "Bridge authorized double execution");
            require(prepare_count == (local_failure ? 0 : 1) && execute_count == (local_failure ? 0 : 1),
                    "Bridge attempted a rejected input or repeated privileged execution");
            if(scenario == Scenario::DigestDrift)
                require(!reached_copy && result.diagnostic() && result.diagnostic()->find("saved archive digest") != std::string::npos,
                        "Saved digest mismatch was not rejected before copying");
            if(!local_failure) require_original_fd();
            if(scenario == Scenario::Unobserved || scenario == Scenario::UnknownWait) {
                require(!result.pacman_exit_status() && consume_count == 0 && abort_count == 0,
                        "Unknown bridge outcome authorized consume/abort or package success");
                // An exact duplicate execute must still fail against the retained
                // real private stage, even after the in-process lease is released.
                bool replay_rejected = false;
                try {
                    static_cast<void>(store.execute(token));
                } catch(const SourceArtifactInstallTrustedStateError&) {
                    replay_rejected = true;
                }
                require(replay_rejected && store.execution_status(token).authorized,
                        "Unknown outcome lost private execution evidence or allowed replay");
            }
            if(scenario == Scenario::Positive || scenario == Scenario::ConsumeFailure)
                require(result.pacman_exit_status() == 0 && consume_count == 1,
                        "Known successful operation was lost at the receipt boundary");
            if(scenario == Scenario::SealingRefusal)
                require(result.sealing_failure() && !result.pacman_exit_status() && consume_count == 0 && abort_count == 1,
                        "Bridge bypassed shared final-reproof refusal handling");
        }
        if(borrowed_fd >= 0) {
            errno = 0;
            require(fcntl(borrowed_fd, F_GETFD) == -1 && errno == EBADF,
                    "Transport destruction did not release the original artifact FD");
        }
        set_evaluated_devel_source_artifact_transport_test_hooks({});
        set_source_artifact_install_trusted_exec_test_hook({});
        set_source_artifact_install_trusted_state_test_hook({});
        ++completed_cases;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - case_started);
        std::cout << "Slice 5 retained bridge: " << label << " PASS (" << elapsed.count() << " ms)\n"
                  << std::flush;
    }
    require(completed_cases == 9, "Transport suite did not complete all nine scenarios");
    std::cout << "Slice 5 retained bridge: " << completed_cases << " cases passed\n";
}
#endif

#ifdef MOGUET_TEST_EXACT_INSTALLED_BINDING
void test_installed_exact_binding(bool publication = false) {
    const char* authorized = std::getenv("MOGUET_EXACT_INSTALLED_ACCEPTANCE");
    require(authorized && std::string(authorized) == "isolated-container" && fs::exists("/.dockerenv") && geteuid() != 0,
            "actual transaction acceptance requires its isolated unprivileged container lane");
    set_installed_record_observation_test_hooks({});
    set_evaluated_devel_source_artifact_transport_test_hooks({});
    const auto world = resolve_trusted_installed_database_world();
    require(std::holds_alternative<InstalledDatabaseWorld>(world) &&
                std::get<InstalledDatabaseWorld>(world).root_directory == "/" &&
                std::get<InstalledDatabaseWorld>(world).database_path == "/var/lib/moguet-exact-installed-binding/db",
            "actual transaction fixture did not resolve its isolated fixed pacman world");
    UpstreamGitFixture upstream("s5b-installed");
    // Current Arch enables debug globally and --packagelist predicts a debug
    // sibling even for this data-only package. The reviewed fixture explicitly
    // selects one artifact; the production cardinality guard is unchanged.
    ReviewedBuildFixture fixture("s5b-installed", upstream, RecipeShape::Valid, false, false, false, true);
    auto first_install = build_success(fixture);
    auto later_downgrade = build_success(fixture);
    const auto lower_version = first_install.artifact().evidence().identity.full_version;
    std::string previous_generation;
    std::vector<DevelBuildProvenanceStoreLoaded> publication_history;
    const auto install = [&](const std::string& label, EvaluatedDevelSourceBuildProof proof,
                             ExactArtifactTransactionOperation operation) {
        const auto artifact = proof.artifact().evidence();
        auto transport = prepare_evaluated_devel_source_artifact_transport(std::move(proof));
        const auto result = transport.execute_exact({true});
        std::ostringstream failure;
        failure << label << ": status=" << static_cast<int>(result.status());
        if(result.diagnostic()) failure << " diagnostic=" << *result.diagnostic();
        if(transport.exact_receipt_issue()) failure << " receipt=" << static_cast<int>(*transport.exact_receipt_issue());
        if(transport.installed_binding_issue()) failure << " binding=" << static_cast<int>(*transport.installed_binding_issue());
        require(result.pacman_exit_status() == 0 && transport.exact_receipt() && transport.fresh_binding(), failure.str());
        require(transport.exact_receipt()->operations().size() == 1 && transport.exact_receipt()->operations().front().operation == operation,
                "actual root hook recorded the wrong operation");
        const auto& binding = transport.fresh_binding()->binding();
        require(binding.mtree_digest().value() == artifact.mtree_digest.value() && *binding.version().full_version() == artifact.identity.full_version &&
                    binding.package().package_name() == fixture.package_name() && *binding.architecture().value() == "any",
                "actual transaction lost raw MTREE or semantic identity");
        const auto generation = binding.record_generation().opaque_identity();
        require(generation.starts_with("linux-name-to-handle-at-v1|") && generation != previous_generation,
                "actual reinstall/upgrade retained an old record generation");
        previous_generation = generation;
        std::cout << "S5B-INSTALLED\t" << label << "\t" << (operation == ExactArtifactTransactionOperation::Install ? "Install" : "Upgrade")
                  << "\t" << artifact.identity.full_version << "\t" << artifact.mtree_digest.value() << "\t" << generation << '\n';
        auto aggregate = transport.finalize();
        require(aggregate && aggregate->operation() == DevelSourceArtifactInstallOperation::Succeeded &&
                    aggregate->receipt_state() == DevelSourceArtifactInstallReceipt::Complete &&
                    aggregate->proof_state() == DevelSourceArtifactInstallProof::Complete &&
                    aggregate->privileged_cleanup().state == DevelSourceArtifactInstallCleanupState::Complete,
                "actual S5-B complete path did not produce the final Slice 5 proof");
        const auto* final_proof = aggregate->proof();
        require(final_proof && final_proof->valid() && final_proof->operation() == operation &&
                    final_proof->built_proof().artifact().evidence() == artifact &&
                    final_proof->installed_binding().record_generation().opaque_identity() == generation,
                "actual final proof lost built/receipt/binding identity");
        require(!transport.finalize(), "actual final proof was minted twice");
        if(publication) {
            check_installed_devel_publication(label, std::move(*aggregate), publication_history);
        } else {
            fixture.require_no_provenance_publication();
            std::cout << "S5C-INSTALLED\t" << label << "\tComplete\tcleanup-Complete\tpublication-none\n";
        }
    };
    install("first-install", std::move(first_install), ExactArtifactTransactionOperation::Install);
    upstream.commit("revision-two\n");
    auto upgrade = build_success(fixture);
    auto reinstall = build_success(fixture);
    require(upgrade.artifact().evidence().identity.full_version != lower_version &&
                upgrade.artifact().evidence().identity.full_version == reinstall.artifact().evidence().identity.full_version,
            "actual Upgrade/reinstall fixture versions are not distinct/equal as required");
    install("upgrade", std::move(upgrade), ExactArtifactTransactionOperation::Upgrade);
    install("same-version-reinstall", std::move(reinstall), ExactArtifactTransactionOperation::Upgrade);
    install("downgrade", std::move(later_downgrade), ExactArtifactTransactionOperation::Upgrade);
}

void test_exact_installed_binding(std::string_view finalization = {}) {
    using Status = SourceArtifactInstallTrustedExecutionStatus;
    using Issue = InstalledRecordObservationIssue;
    UpstreamGitFixture upstream("slice5-exact-binding");
    int case_index = 0;
    std::vector<std::string> modes = {"install", "upgrade", "reinstall", "downgrade", "wrong-operation", "no-post", "nonzero", "partial-nonzero",
                                      "authorized-only", "unknown-wait", "malformed-status", "needed-skip", "consume-failure",
                                      "wrong-name", "wrong-base", "wrong-version", "wrong-architecture", "missing-base",
                                      "anchor-mtree", "outer-mtree", "outer-desc", "outer-files", "identical-reinstall",
                                      "no-mtree", "empty-mtree", "truncated-mtree", "generation-unsupported", "outer-alpm-failure",
                                      "outer-resource", "anchor-resource", "anchor-eio", "outer-world", "missing-package", "missing-baseline", "missing-anchor",
                                      "anchor-xdata", "outer-xdata", "outer-desc-oversize", "outer-lazy-failure", "outer-empty-files", "outer-empty-core-conflict"};
#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
    using M = DevelSourceArtifactInstallTestMismatch;
    using F = InstalledDevelSourceBuildIssue;
    using O = DevelSourceArtifactInstallOperation;
    using R = DevelSourceArtifactInstallReceipt;
    using P = DevelSourceArtifactInstallProof;
    using C = DevelSourceArtifactInstallCleanupState;
    const std::vector<std::pair<std::string, std::pair<M, F>>> mismatches{
        {"built-lineage", {M::BuiltLineage, F::BuiltLineageMismatch}},
        {"receipt-lineage", {M::ReceiptLineage, F::TransactionLineageMismatch}},
        {"binding-lineage", {M::BindingLineage, F::TransactionLineageMismatch}},
        {"package-name", {M::PackageName, F::PackageIdentityMismatch}},
        {"package-base", {M::PackageBase, F::PackageIdentityMismatch}},
        {"version", {M::Version, F::PackageIdentityMismatch}},
        {"architecture", {M::Architecture, F::PackageIdentityMismatch}},
        {"artifact-index", {M::ArtifactIndex, F::ArtifactIndexMismatch}},
        {"archive-digest", {M::ArchiveDigest, F::ArchiveDigestMismatch}},
        {"mtree-digest", {M::MtreeDigest, F::MtreeMismatch}},
        {"transaction-token", {M::TransactionToken, F::TransactionLineageMismatch}},
        {"purpose", {M::Purpose, F::TransactionLineageMismatch}},
        {"staged-identity", {M::StagedIdentity, F::TransactionLineageMismatch}},
        {"generation", {M::Generation, F::InstalledGenerationMismatch}},
        {"database-digest", {M::DatabaseDigest, F::DatabaseRecordMismatch}},
        {"descriptor-identity", {M::DescriptorIdentity, F::DatabaseRecordMismatch}},
        {"receipt-cardinality", {M::ReceiptCardinality, F::UnsupportedCardinality}},
        {"binding-cardinality", {M::BindingCardinality, F::UnsupportedCardinality}},
        {"built-cardinality", {M::BuiltCardinality, F::UnsupportedCardinality}},
        {"final-resource", {M::ResourceFailure, F::ResourceFailure}},
    };
    if(finalization == "proof") {
        modes = {"install", "upgrade", "reinstall", "downgrade", "wrong-built", "donor", "swap-all", "donor", "swap-receipt", "donor", "swap-binding"};
        for(const auto& entry : mismatches)
            modes.push_back(entry.first);
    } else if(finalization == "aggregate") {
        modes = {"not-attempted", "prepare-failure", "execute-exception", "unknown-wait", "nonzero", "partial-nonzero", "no-post", "malformed-receipt", "consume-failure",
                 "outer-mtree", "outer-empty-files", "generation-unsupported", "outer-alpm-failure", "outer-world",
                 "outer-xdata", "outer-desc-oversize", "outer-resource", "install", "cleanup-failure", "retirement-failure", "abort-resource", "receipt-resource"};
    }
    std::optional<EvaluatedDevelSourceArtifactTransport> donor;
#endif
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
    const bool publication = finalization == "publication-projection" || finalization == "publication-aggregate";
    if(publication) modes = devel_publication_fixture_cases(finalization == "publication-projection");
#endif
    for(const std::string& scenario : modes) {
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        const std::string mode = publication ? devel_publication_fixture_install_mode(scenario) : scenario;
#else
        const std::string& mode = scenario;
#endif
        const auto label = finalization.empty() ? "s5b-" + std::to_string(case_index++) : "s5c-common";
        ReviewedBuildFixture fixture(label, upstream);
        auto proof = build_success(fixture);
        const auto expected = proof.artifact().evidence();
        std::optional<EvaluatedDevelSourceBuildProof> different_build;
        if(mode == "wrong-built") {
            different_build.emplace(build_success(fixture));
            require(different_build->artifact().evidence().identity == expected.identity,
                    "wrong-build control must have the same package/version identity");
        }
        if(mode == "not-attempted") {
            auto transport = prepare_evaluated_devel_source_artifact_transport(std::move(proof));
            auto aggregate = transport.finalize();
            require(aggregate && aggregate->operation() == DevelSourceArtifactInstallOperation::NotAttempted &&
                        aggregate->receipt_state() == DevelSourceArtifactInstallReceipt::NotAttempted &&
                        aggregate->proof_state() == DevelSourceArtifactInstallProof::NotAttempted &&
                        aggregate->privileged_cleanup().state == DevelSourceArtifactInstallCleanupState::Complete &&
                        !aggregate->transport_result() && !transport.finalize(),
                    "NotAttempted finalization is inconsistent");
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
            if(publication) check_devel_publication_fixture(scenario, std::move(*aggregate));
#endif
            std::cout << "S5-C aggregate not-attempted PASS\n";
            continue;
        }
        const auto raw_mtree = capture_process("/usr/bin/bsdtar", {"-xOf", proof.artifact().path().string(), ".MTREE"}, {"PATH=/usr/bin:/bin", "LC_ALL=C"});
        require(raw_mtree.exit_code == 0 && !raw_mtree.stdout_capture_limit_exceeded &&
                    xdg_generation_store_raw_contents_sha256(raw_mtree.output) == expected.mtree_digest.value(),
                "fixture lost built raw MTREE");
        TemporaryTree runtime(label);
        const auto db = runtime.path() / "db";
        fs::create_directories(db / "local");
        write_file(db / "local/ALPM_DB_VERSION", "9\n");
        const int runtime_fd = open(runtime.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        require(runtime_fd >= 0, "missing exact fixture runtime");
        auto store = SourceArtifactInstallTrustedStateStore::open_below_runtime_parent(runtime_fd, geteuid());
        static_cast<void>(close(runtime_fd));
        const std::string token(64, 'a');
        unsigned generation = 1;
        bool outer = false;
        unsigned outer_sessions = 0;
        unsigned outer_semantic_loads = 0;
        bool late_partial_values = false;
        unsigned consumes = 0, aborts = 0;
        fs::path installed_record;
        const auto write_package = [&](const std::string& version) {
            if(!installed_record.empty() && fs::exists(installed_record)) fs::remove_all(installed_record);
            installed_record = db / "local" / (fixture.package_name() + "-" + version);
            fs::create_directory(installed_record);
            write_file(installed_record / "desc", "%NAME%\n" + fixture.package_name() + "\n\n%BASE%\n" + fixture.package_base() +
                                                      "\n\n%VERSION%\n" + version + "\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
            write_file(installed_record / "files", "%FILES%\nusr/share/moguet-test\n\n");
            write_file(installed_record / "mtree", raw_mtree.output);
        };
        const bool upgrade = mode == "upgrade" || mode == "reinstall" || mode == "downgrade";
        if(upgrade) write_package(mode == "upgrade" ? "0-1" : mode == "downgrade" ? "9999-1"
                                                                                  : expected.identity.full_version);
        InstalledRecordObservationTestHooks observation_hooks;
        observation_hooks.database_path = db.string();
        observation_hooks.expected_owner = geteuid();
        observation_hooks.name_to_handle = [&](int fd, const char* path, struct file_handle* handle, int* mount, int flags) {
            require(fd >= 0 && path[0] == '\0' && flags == AT_EMPTY_PATH, "live observer used pathname generation");
            if(mode == "generation-unsupported" && generation > 1) {
                errno = EOPNOTSUPP;
                return -1;
            }
            if(mode == "outer-resource" && outer) throw std::bad_alloc();
            if(mode == "anchor-resource" && generation > 1 && !outer) throw std::bad_alloc();
            *mount = outer ? 99 : 17;
            if(handle->handle_bytes == 0) {
                handle->handle_bytes = 4;
                errno = EOVERFLOW;
                return -1;
            }
            handle->handle_type = 1;
            for(unsigned index = 0; index < 4; ++index)
                handle->f_handle[index] = static_cast<unsigned char>(generation + index);
            return 0;
        };
        observation_hooks.event = [&](InstalledRecordObservationTestEvent event) {
            if(outer && event == InstalledRecordObservationTestEvent::BeforeSessionOpen) ++outer_sessions;
            if(outer && event == InstalledRecordObservationTestEvent::BeforeSemanticLoad) {
                ++outer_semantic_loads;
                if(mode == "outer-lazy-failure") {
                    std::ofstream file(installed_record / "desc", std::ios::app);
                    file << "%XDATA%\nnot-key-value\n\n";
                }
            }
            if(outer && mode == "outer-lazy-failure" && event == InstalledRecordObservationTestEvent::AfterSemanticLoad)
                late_partial_values = true;
        };
        observation_hooks.pread = [&](int fd, void* buffer, std::size_t count, off_t offset) -> ssize_t {
            if(mode == "anchor-eio" && generation > 1 && !outer) {
                errno = EIO;
                return -1;
            }
            return pread(fd, buffer, count, offset);
        };
        set_installed_record_observation_test_hooks(observation_hooks);
        std::optional<SourceArtifactInstallRootPrepareRequest> request;
        set_evaluated_devel_source_artifact_transport_test_hooks({[&](const ExplicitProcessInvocation& invocation) -> CapturedCommandResult {
                                                                      require(invocation.executable == "/usr/bin/sudo" && invocation.arguments[1] == MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH,
                                                                              "exact transport bypassed fixed helper");
                                                                      const auto& verb = invocation.arguments[2];
                                                                      if(verb == "prepare-exact") {
                                                                          if(mode == "prepare-failure") return {"", 77, false};
                                                                          const auto parsed = parse_source_artifact_install_trusted_helper_arguments({invocation.arguments.begin() + 2, invocation.arguments.end()});
                                                                          const auto& helper = require_arm<SourceArtifactInstallTrustedHelperInvocation>(parsed, "invalid exact prepare arguments");
                                                                          request.emplace(SourceArtifactInstallRootPrepareRequest{helper.transaction_token, helper.package_base, helper.directive,
                                                                                                                                  helper.needed, helper.no_confirm, helper.artifacts,
                                                                                                                                  SourceArtifactInstallTrustedPurpose::ExactInstalledBinding});
                                                                          require(request->artifacts.front().archive_sha256 == expected.archive_digest.value() &&
                                                                                      request->artifacts.front().raw_mtree_sha256 == expected.mtree_digest.value(),
                                                                                  "exact transport changed built identity");
                                                                          const auto prepared = store.prepare(*request, *invocation.standard_input_fd);
                                                                          return {serialize_source_artifact_install_root_prepare_response(prepared, *request), 0, false};
                                                                      }
                                                                      if(verb == "execution-status") {
                                                                          if(mode == "malformed-status") return {"unknown protocol\n", 0, false};
                                                                          return {serialize_source_artifact_install_execution_observation(store.execution_status(token)), 0, false};
                                                                      }
                                                                      require(verb == "consume-exact", "exact route called legacy consume");
                                                                      ++consumes;
                                                                      if(mode == "consume-failure") return {"", 1, false};
                                                                      if(mode == "receipt-resource") throw std::bad_alloc();
                                                                      auto protocol = store.consume_exact(token);
                                                                      if(mode == "malformed-receipt") protocol += "TRAILING\n";
                                                                      return {std::move(protocol), 0, false};
                                                                  },
                                                                  [&](const ExplicitProcessInvocation& invocation) -> ExplicitProcessExecutionResult {
                                                                      if(invocation.arguments[2] == "abort") {
                                                                          ++aborts;
                                                                          if(mode == "abort-resource") throw std::bad_alloc();
                                                                          store.abort(token);
                                                                          return {ExplicitProcessExecutionStatus::StartedKnownOutcome, 0};
                                                                      }
                                                                      require(invocation.arguments[2] == "execute", "unexpected exact execution verb");
                                                                      const int status = store.execute(token);
                                                                      outer = true;
                                                                      if(mode == "outer-alpm-failure") {
                                                                          observation_hooks.fail_database_load = true;
                                                                          set_installed_record_observation_test_hooks(observation_hooks);
                                                                      }
                                                                      if(mode == "outer-world") {
                                                                          observation_hooks.database_path = (runtime.path() / "different-db").string();
                                                                          fs::create_directories(*observation_hooks.database_path + "/local");
                                                                          set_installed_record_observation_test_hooks(observation_hooks);
                                                                      }
                                                                      if(mode == "execute-exception") throw std::bad_alloc();
                                                                      if(mode == "unknown-wait") return {ExplicitProcessExecutionStatus::StartedOutcomeUnknown, std::nullopt};
                                                                      return {ExplicitProcessExecutionStatus::StartedKnownOutcome, status};
                                                                  },
                                                                  {}});
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) {
            if(mode == "authorized-only" || mode == "needed-skip") return 0;
            store.observe_execution(token);
            if(mode == "nonzero" || mode == "abort-resource") return 42;
            ++generation;
            write_package(expected.identity.full_version);
            if(mode == "partial-nonzero") return 42;
            if(mode == "no-post") return 0;
            if(mode == "wrong-name" || mode == "wrong-base" || mode == "wrong-version" || mode == "wrong-architecture" || mode == "missing-base") {
                std::ifstream file(installed_record / "desc");
                std::string bytes{std::istreambuf_iterator<char>(file), {}};
                const auto from = mode == "wrong-name" ? fixture.package_name() : mode == "wrong-base"  ? fixture.package_base()
                                                                              : mode == "wrong-version" ? expected.identity.full_version
                                                                              : mode == "missing-base"  ? "%BASE%\n" + fixture.package_base() + "\n\n"
                                                                                                        : "any";
                const auto to = mode == "missing-base" ? "" : mode == "wrong-version" ? "0-1"
                                                                                      : "incorrect";
                bytes.replace(bytes.find(from), from.size(), to);
                write_file(installed_record / "desc", bytes);
            }
            if(mode == "anchor-mtree") {
                auto bytes = raw_mtree.output;
                bytes[4] ^= 1;
                write_file(installed_record / "mtree", bytes);
            }
            if(mode == "no-mtree") fs::remove(installed_record / "mtree");
            if(mode == "empty-mtree") write_file(installed_record / "mtree", "");
            if(mode == "truncated-mtree") write_file(installed_record / "mtree", raw_mtree.output.substr(0, 12));
            if(mode == "anchor-xdata") {
                std::ofstream file(installed_record / "desc", std::ios::app);
                file << "%XDATA%\nnot-key-value\n\n";
            }
            int descriptors[2];
            require(pipe(descriptors) == 0, "exact NeedsTargets pipe failed");
            const auto targets = fixture.package_name() + "\n";
            require(write(descriptors[1], targets.data(), targets.size()) == static_cast<ssize_t>(targets.size()), "exact target write failed");
            static_cast<void>(close(descriptors[1]));
            if(upgrade || mode == "wrong-operation")
                store.record_upgrade(token, descriptors[0]);
            else
                store.record_install(token, descriptors[0]);
            static_cast<void>(close(descriptors[0]));
            if(mode == "outer-mtree") {
                auto bytes = raw_mtree.output;
                bytes[4] ^= 1;
                write_file(installed_record / "mtree", bytes);
            }
            if(mode == "outer-desc" || mode == "outer-files") {
                std::ofstream file(installed_record / (mode == "outer-desc" ? "desc" : "files"), std::ios::app);
                file << '\n';
            }
            if(mode == "outer-xdata" || mode == "outer-empty-core-conflict") {
                std::ofstream file(installed_record / "desc", std::ios::app);
                file << (mode == "outer-xdata" ? "%XDATA%\nnot-key-value\n\n" : "%DESC%\n\n%NAME%\nconflicting-name\n\n");
            }
            if(mode == "outer-desc-oversize") fs::resize_file(installed_record / "desc", INSTALLED_RECORD_MAXIMUM_METADATA_BYTES + 1);
            if(mode == "outer-empty-files") write_file(installed_record / "files", "%FILES%\n\n");
            if(mode == "identical-reinstall") {
                ++generation;
                write_package(expected.identity.full_version);
            }
            if(mode == "missing-package") fs::remove_all(installed_record);
            const auto exact_directory = runtime.path() / "moguet/source-artifact-installs/active" / token / "exact";
            if(mode == "missing-baseline") fs::remove(exact_directory / "baseline");
            if(mode == "missing-anchor") fs::remove(exact_directory / "install-anchor");
            return 0;
        });
        set_source_artifact_install_trusted_state_test_hook([&](auto event, int, const auto&) {
            if((mode == "cleanup-failure" && event == SourceArtifactInstallTrustedStateTestEvent::BeforeExactCleanup) ||
               (mode == "retirement-failure" && event == SourceArtifactInstallTrustedStateTestEvent::BeforeExactRetirement))
                throw std::bad_alloc();
        });
        {
            auto transport = prepare_evaluated_devel_source_artifact_transport(std::move(proof));
            const auto result = transport.execute_exact_for_test({true}, token);
            const bool unknown = mode == "authorized-only" || mode == "needed-skip" || mode == "unknown-wait" || mode == "malformed-status" || mode == "execute-exception";
            const bool not_attempted = mode == "prepare-failure";
            const bool no_receipt = unknown || not_attempted || mode == "no-post" || mode == "nonzero" || mode == "partial-nonzero" || mode == "consume-failure" || mode == "malformed-receipt" || mode == "abort-resource" || mode == "receipt-resource";
            if(not_attempted)
                require(!result.pacman_exit_status() && consumes == 0 && aborts == 0 && outer_sessions == 0, "failed preparation attempted transaction");
            else if(unknown)
                require(result.status() == Status::OutcomeUnknown && !result.pacman_exit_status() && consumes == 0 && aborts == 0 && outer_sessions == 0,
                        "Unknown exact transaction was consumed/aborted/observed: " + mode);
            else if(mode == "abort-resource")
                require(result.pacman_exit_status() == 42 && outer_sessions == 0, "abort allocation failure erased known nonzero");
            else if(mode == "nonzero" || mode == "partial-nonzero")
                require(result.status() == Status::PacmanFailed && result.pacman_exit_status() == 42 && outer_sessions == 0, "nonzero transaction minted proof");
            else
                require(result.pacman_exit_status() == 0, "binding failure erased known transaction success: " + mode);
            require((transport.exact_receipt() == nullptr) == no_receipt, "exact receipt completion changed: " + mode);
            const bool positive = mode == "install" || upgrade || (!finalization.empty() && (finalization == "proof" || mode == "cleanup-failure" || mode == "retirement-failure"));
            require((transport.fresh_binding() != nullptr) == positive, "fresh binding decision changed: " + mode);
            if(mode == "partial-nonzero") require(fs::exists(installed_record), "failed transaction was assumed to roll back its package changes");
            if(positive) {
                const auto& binding = transport.fresh_binding()->binding();
                require(binding.mtree_digest().value() == expected.mtree_digest.value() && binding.package().package_name() == fixture.package_name() &&
                            *binding.version().full_version() == expected.identity.full_version && *binding.architecture().value() == "any" && outer_sessions == 1,
                        "live binding did not preserve exact observed identity");
                require(!transport.installed_binding_issue() && !transport.exact_receipt_issue(), "positive binding retained an issue");
            } else if(!no_receipt) {
                require(transport.installed_binding_issue().has_value(), "binding failure was lost: " + mode);
                const auto issue = *transport.installed_binding_issue();
                if(mode == "identical-reinstall") require(issue == Issue::GenerationMismatch, "identical reinstall did not invalidate the anchor generation");
                if(mode == "generation-unsupported") require(issue == Issue::UnsupportedGeneration, "generation failure used fallback");
                if(mode == "outer-resource") require(issue == Issue::ResourceFailure, "resource failure was reclassified");
                if(mode == "anchor-resource") require(issue == Issue::ResourceFailure, "Post anchor resource failure erased operation receipt");
                if(mode == "anchor-eio") require(issue == Issue::ReadFailure, "Post anchor read failure erased operation receipt");
                if(mode == "anchor-xdata" || mode == "outer-xdata" || mode == "outer-empty-core-conflict")
                    require(issue == Issue::MalformedMetadata && outer_semantic_loads == 0, "malformed raw desc reached outer ALPM semantics");
                if(mode == "outer-desc-oversize")
                    require(issue == Issue::MetadataTooLarge && outer_semantic_loads == 0, "oversized desc reached ALPM lazy read");
                if(mode == "outer-lazy-failure")
                    require(issue == Issue::DatabaseLoadFailure && late_partial_values && outer_semantic_loads == 1,
                            "partial lazy getter values became a fresh binding");
                if(mode == "outer-empty-files") require(issue == Issue::DatabaseLoadFailure, "ambiguous empty file cache became proof");
            }
            require(!result.operation_result() && !result.expectation() && !result.observation(), "exact path gained cleanup authority");
            require(transport.execute_exact_for_test({true}, token).status() == Status::InvalidRequest, "exact capability retried");
#ifdef MOGUET_ENABLE_DEVEL_SOURCE_ARTIFACT_INSTALL_TEST_HOOKS
            if(!finalization.empty()) {
                if(mode == "donor") {
                    donor.reset();
                    donor.emplace(std::move(transport));
                    require(!transport.finalize(), "moved-from donor finalized");
                } else {
                    std::optional<F> expected_final_issue;
                    if(positive) require(DevelSourceArtifactInstallFixture::check_component_moves(transport), "component move did not revoke source capability");
                    if(mode == "wrong-built") {
                        DevelSourceArtifactInstallFixture::replace_built(transport, std::move(*different_build));
                        require(!different_build->valid(), "different build was copied");
                        expected_final_issue = F::BuiltLineageMismatch;
                    }
                    if(mode == "swap-all" || mode == "swap-receipt" || mode == "swap-binding") {
                        require(donor && donor->exact_receipt() && donor->fresh_binding(), "missing independently valid transaction donor");
                        require(donor->exact_receipt()->manifest().artifacts.front().package_name == expected.identity.package_name &&
                                    donor->exact_receipt()->manifest().artifacts.front().full_version == expected.identity.full_version,
                                "cross-transaction control lost same package/version identity");
                        if(mode == "swap-binding")
                            DevelSourceArtifactInstallFixture::exchange_bindings(transport, *donor);
                        else
                            DevelSourceArtifactInstallFixture::exchange_receipts(transport, *donor, mode == "swap-all");
                        expected_final_issue = mode == "swap-binding" ? F::TransactionLineageMismatch : F::BuiltLineageMismatch;
                    }
                    for(const auto& entry : mismatches)
                        if(mode == entry.first) {
                            DevelSourceArtifactInstallFixture::mismatch(transport, entry.second.first);
                            expected_final_issue = entry.second.second;
                        }
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
                    if(publication && (scenario == "final-mismatch" || scenario == "unsupported-cardinality")) {
                        DevelSourceArtifactInstallFixture::mismatch(transport, scenario == "final-mismatch" ? M::BuiltLineage : M::ReceiptCardinality);
                        expected_final_issue = scenario == "final-mismatch" ? F::BuiltLineageMismatch : F::UnsupportedCardinality;
                    }
#endif
                    const auto binding_issue = transport.installed_binding_issue();
                    auto moved_transport = std::move(transport);
                    require(!transport.finalize(), "moved-from transport finalized");
                    const auto effects_before_finalize = std::tuple{consumes, aborts, outer_sessions};
                    auto aggregate = moved_transport.finalize();
                    require(effects_before_finalize == std::tuple{consumes, aborts, outer_sessions}, "finalization repeated transaction/observer/cleanup");
                    require(aggregate && !moved_transport.finalize() && !moved_transport.active() &&
                                !moved_transport.exact_receipt() && !moved_transport.fresh_binding(),
                            "finalization did not consume its owner once");
                    const bool failed = mode == "nonzero" || mode == "partial-nonzero" || mode == "abort-resource";
                    require(aggregate->operation() == (unknown ? O::OutcomeUnknown : not_attempted ? O::NotAttempted
                                                                                 : failed          ? O::Failed
                                                                                                   : O::Succeeded) &&
                                aggregate->pacman_exit_status() == result.pacman_exit_status(),
                            "finalization lost operation outcome");
                    require(aggregate->transport_result() && aggregate->transport_result()->status() == result.status() &&
                                aggregate->binding_issue() == binding_issue,
                            "finalization flattened the S5-B result");
                    require(aggregate->receipt_state() == (unknown || failed || not_attempted ? R::NotAttempted : mode == "no-post"        ? R::Missing
                                                                                                              : mode == "receipt-resource" ? R::Incomplete
                                                                                                              : no_receipt                 ? R::Invalid
                                                                                                                                           : R::Complete),
                            "finalization lost receipt completion");
                    require(aggregate->proof_state() == (no_receipt ? P::NotAttempted : positive && !expected_final_issue ? P::Complete
                                                                                                                          : P::Incomplete),
                            "finalization minted or lost proof");
                    if(expected_final_issue) require(aggregate->proof_issue() == expected_final_issue, "final correlation mismatch taxonomy changed");
                    const auto expected_cleanup = unknown ? C::Retained : not_attempted || mode == "malformed-receipt" || mode == "receipt-resource"                                       ? C::OutcomeUnknown
                                                                      : mode == "consume-failure" || mode == "cleanup-failure" || mode == "retirement-failure" || mode == "abort-resource" ? C::Failed
                                                                                                                                                                                           : C::Complete;
                    require(aggregate->privileged_cleanup().state == expected_cleanup && aggregate->source_context_cleanup() == C::Retained,
                            "cleanup consequence was conflated with proof/operation");
                    if(mode == "cleanup-failure") require(aggregate->privileged_cleanup().issue == DevelSourceArtifactInstallCleanupIssue::PrivateStageCleanupFailed,
                                                          "private cleanup cause missing");
                    if(mode == "retirement-failure") require(aggregate->privileged_cleanup().issue == DevelSourceArtifactInstallCleanupIssue::RetirementFailed,
                                                             "retirement cause missing");
                    const auto* final_proof = aggregate->proof();
                    if(final_proof) require(final_proof->valid() && final_proof->built_proof().valid() && final_proof->artifact_index() == 0 &&
                                                final_proof->installed_binding().mtree_digest() == expected.mtree_digest,
                                            "final proof lost owned build/binding evidence");
                    auto moved_result = std::move(*aggregate);
                    require(!aggregate->valid() && !aggregate->proof() && (!final_proof || !final_proof->valid()) && moved_result.valid(),
                            "final proof/result move source remained usable");
                    bool rejected = false;
                    try {
                        static_cast<void>(aggregate->operation());
                    } catch(const std::logic_error&) {
                        rejected = true;
                    }
                    require(rejected, "moved-from result allowed use");
                    if(final_proof) {
                        rejected = false;
                        try {
                            static_cast<void>(final_proof->built_proof());
                        } catch(const std::logic_error&) {
                            rejected = true;
                        }
                        require(rejected, "moved-from final proof allowed use");
                    }
                    require(moved_transport.execute_exact_for_test({true}, token).status() == Status::InvalidRequest, "finalized transport reexecuted");
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
                    if(publication) {
                        const auto observation_effects = std::tuple{consumes, aborts, outer_sessions, outer_semantic_loads};
                        check_devel_publication_fixture(scenario, std::move(moved_result), [&] {
                            ++generation;
                            write_package("9999-2");
                        });
                        require(observation_effects == std::tuple{consumes, aborts, outer_sessions, outer_semantic_loads},
                                "publisher repeated transaction/observer/cleanup");
                    } else
#endif
                    {
                        fixture.require_no_provenance_publication();
                        std::cout << "S5-C " << finalization << ' ' << mode << " PASS\n";
                    }
                }
            }
#endif
        }
        set_evaluated_devel_source_artifact_transport_test_hooks({});
        set_source_artifact_install_trusted_exec_test_hook({});
        set_installed_record_observation_test_hooks({});
        set_source_artifact_install_trusted_state_test_hook({});
        std::cout << "S5-B integrated " << mode << " PASS\n";
    }
}
#endif

#ifdef MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
unsigned g_bridge_publication_entries = 0;
void count_bridge_publication(DevelBuildProvenancePublicationStage stage) {
    if(stage == DevelBuildProvenancePublicationStage::Projection) ++g_bridge_publication_entries;
}

void deny_after_bridge_publication(const XdgGenerationStoreTestRaceContext&) {
    publication_allocation::blocked = true;
}

#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
std::map<fs::path, std::string> bootstrap_cache_snapshot(const fs::path& checkout) {
    std::map<fs::path, std::string> out;
    if(!fs::exists(checkout)) return out;
    for(const auto& entry : fs::recursive_directory_iterator(checkout)) {
        const auto status = entry.symlink_status();
        std::string value;
        if(fs::is_symlink(status))
            value = "symlink:" + fs::read_symlink(entry.path()).string();
        else if(fs::is_directory(status))
            value = "directory";
        else if(fs::is_regular_file(status)) {
            std::ifstream input(entry.path(), std::ios::binary);
            require(static_cast<bool>(input), "snapshot file open failed");
            value = xdg_generation_store_raw_contents_sha256(std::string((std::istreambuf_iterator<char>(input)), {}));
        } else
            throw std::runtime_error("Unsupported bootstrap fixture entry");
        out.emplace(entry.path().lexically_relative(checkout), value + ":" + std::to_string(static_cast<unsigned>(status.permissions())));
    }
    return out;
}
#endif

void test_reviewed_devel_execution_bridge(bool normal = false, const std::string& bootstrap_case = {}) {
    const bool bootstrap = !bootstrap_case.empty();
    using Stage = ReviewedDevelSourceBuildStage;
    using Issue = ReviewedDevelSourceBuildIssue;
    using Operation = DevelSourceArtifactInstallOperation;
    using Pub = DevelBuildProvenancePublicationState;
    using Cleanup = DevelSourceArtifactInstallCleanupState;
    static_assert(!std::is_default_constructible_v<PreparedReviewedDevelSourceBuildExecution>);
    static_assert(!std::is_copy_constructible_v<PreparedReviewedDevelSourceBuildExecution>);
    static_assert(!std::is_default_constructible_v<ReviewedDevelSourceBuildExecutionResult>);
    static_assert(!std::is_copy_constructible_v<ReviewedDevelSourceBuildExecutionResult>);
    static_assert(std::is_nothrow_move_constructible_v<ReviewedDevelSourceBuildExecutionResult>);
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
    if(normal && !bootstrap) {
        for(const auto lifecycle : {ArtifactLifecycleIntent::SingularCompatibility, ArtifactLifecycleIntent::PackageBaseSet}) {
            PreparedProductionSourceBuildInvocation invocation;
            ProductionSourceBuildWorkItem work;
            work.request.checkout_name = "legacy-exception";
            work.artifact_lifecycle_intent = lifecycle;
            invocation.work_items = {work, work};
            unsigned calls = 0;
            SourceInvocationExecutionTestHooks hooks;
            hooks.singular = [&](const auto&, const auto&, const auto&) -> SourceBuildExecutionResult { ++calls; throw std::runtime_error("original legacy failure"); };
            hooks.package_base = [&](const auto&, const auto&, const auto&) -> SourceBuildPackageBaseExecutionResult { ++calls; throw std::runtime_error("original legacy failure"); };
            set_source_invocation_execution_test_hooks(std::move(hooks));
            bool caught = false;
            try {
                static_cast<void>(execute_prepared_source_build_invocation(std::move(invocation), AppConfig{}));
            } catch(const ProductionSourceBuildInvocationError& error) {
                caught = true;
                require(error.result().work_items[0].status == ProductionSourceBuildWorkItemStatus::Failed && error.result().work_items[0].failure_exception &&
                            !error.result().work_items[0].devel_execution && error.result().work_items[1].status == ProductionSourceBuildWorkItemStatus::NotAttempted,
                        "legacy exception aggregate changed");
                bool original = false;
                try {
                    error.rethrow_failure();
                } catch(const std::runtime_error& cause) {
                    original = std::string(cause.what()) == "original legacy failure";
                }
                require(original && calls == 1, "legacy typed cause or one-shot lost");
            }
            require(caught, "legacy exception became normal partial");
        }
        set_source_invocation_execution_test_hooks({});
        std::cout << "S7D outer legacy exception / singular + PackageBaseSet / original cause PASS\n";
    }
#endif
    UpstreamGitFixture upstream("s7c-upstream", GitObjectFormat::Sha1);
    std::vector<std::string> bridge_cases = {"install", "branch", "upgrade-explicit", "upgrade-dependency", "dependency-keeps-explicit",
                                             "new-dependency", "promotion", "needed", "split", "rmdeps", "only-if-updated", "legacy", "overlay", "overlay-legacy",
                                             "environment", "build-failure", "artifact-mismatch", "database-world", "snapshot-failure", "prepare-failure",
                                             "nonzero", "unknown", "no-post", "binding-failure", "publication-failure", "publication-unknown",
                                             "cleanup-failure", "retirement-failure", "no-allocation", "registered-different", "registered-same", "registered-unknown", "registered-check",
                                             "outer-singular-publication-failure", "outer-singular-publication-unknown", "outer-singular-cleanup-failure", "outer-singular-no-allocation",
                                             "outer-set-publication-failure", "outer-set-publication-unknown", "outer-set-cleanup-failure", "outer-set-no-allocation"};
    if(bootstrap) bridge_cases = {bootstrap_case};
    for(const std::string& case_name : bridge_cases) {
        const bool outer = case_name.starts_with("outer-");
        const bool multi = bootstrap && case_name.starts_with("multi-");
        const bool outer_set = case_name.starts_with("outer-set-");
        const std::string mode = multi ? case_name.substr(6) : outer ? case_name.substr(outer_set ? 10 : 15)
                                                                     : case_name;
        if(outer && !normal) continue;
        if(!normal && mode.starts_with("registered-")) continue;
        ReviewedBuildFixture fixture(bootstrap ? "bootstrap-git" : "s7c-fixture", upstream, mode == "build-failure" ? RecipeShape::RawEvaluatedMismatch : normal && mode == "legacy" ? RecipeShape::UnsupportedVcs
                                                                                                                                                                                     : RecipeShape::Valid,
                                     false, mode == "branch", mode == "supplemental" || mode == "supplemental-collision");
        struct ResetBridgeHooks {
            ~ResetBridgeHooks() {
                publication_allocation::blocked = false;
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
                set_source_invocation_execution_test_hooks({});
#endif
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
                set_aur_update_non_bootstrap_execution_test_hook({});
                set_devel_tracking_bootstrap_test_hooks({});
                set_recipe_acquisition_test_hooks({});
                set_devel_package_assessment_test_hooks({});
                set_aur_devel_update_database_paths_for_test(std::nullopt);
#endif
                set_devel_build_provenance_publication_test_hook(nullptr);
                set_reviewed_devel_source_build_execution_test_hooks({});
                set_evaluated_devel_source_artifact_transport_test_hooks({});
                set_source_artifact_install_trusted_exec_test_hook({});
                set_source_artifact_install_trusted_state_test_hook({});
                set_installed_record_observation_test_hooks({});
                reset_xdg_generation_store_test_hooks();
            }
        } reset_bridge_hooks;
        TemporaryTree runtime("s7c-runtime");
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        unsigned acquisition_fetches = 0, acquisition_processes = 0, acquisition_creations = 0, acquisition_cleanups = 0, context_entries = 0;
        fs::path acquisition_root;
#endif
        const auto db = runtime.path() / "db";
        fs::create_directories(db / "local");
        write_file(db / "local/ALPM_DB_VERSION", "9\n");
        int parent = open(runtime.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        require(parent >= 0, "S7-C runtime open failed");
        auto store = SourceArtifactInstallTrustedStateStore::open_below_runtime_parent(parent, geteuid());
        static_cast<void>(close(parent));
        unsigned generation = 1, prepare_calls = 0, execute_calls = 0, consumes = 0, aborts = 0, build_entries = 0;
        g_bridge_publication_entries = 0;
        set_devel_build_provenance_publication_test_hook(&count_bridge_publication);
        const std::string token(64, 'c');
        std::optional<BuiltPackageArtifactEvidence> expected;
        std::string actual_oid, raw_mtree;
        fs::path installed_record;
        const bool existing = bootstrap || mode == "upgrade-explicit" || mode == "upgrade-dependency" || mode == "dependency-keeps-explicit" || mode == "promotion" || mode.starts_with("registered-");
        const bool dependency_reason = mode == "upgrade-dependency" || mode == "promotion";
        const auto write_package = [&](const std::string& version) {
            if(!installed_record.empty() && fs::exists(installed_record)) fs::remove_all(installed_record);
            installed_record = db / "local" / (fixture.package_name() + "-" + version);
            fs::create_directory(installed_record);
            write_file(installed_record / "desc", "%NAME%\n" + fixture.package_name() + "\n\n%BASE%\n" + fixture.package_base() +
                                                      "\n\n%VERSION%\n" + version + "\n\n%ARCH%\nany\n\n%REASON%\n" + (dependency_reason ? "1" : "0") + "\n\n");
            write_file(installed_record / "files", "%FILES%\nusr/share/moguet-test\n\n");
            write_file(installed_record / "mtree", raw_mtree.empty() ? "old-mtree" : raw_mtree);
        };
        if(existing) write_package(mode.starts_with("required-") || mode == "shared-base" ? "2-1" : "0-1");
        InstalledRecordObservationTestHooks record_hooks;
        record_hooks.database_path = db.string();
        record_hooks.expected_owner = geteuid();
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        if(bootstrap) record_hooks.statfs = [](int, struct statfs* value) { *value = {}; value->f_type = 0xef53; return 0; };
#endif
        record_hooks.name_to_handle = [&](int fd, const char* path, struct file_handle* handle, int* mount, int flags) {
            require(fd >= 0 && path[0] == '\0' && flags == AT_EMPTY_PATH, "S7-C generation reopened path");
            *mount = 1;
            if(handle->handle_bytes == 0) {
                handle->handle_bytes = 1;
                errno = EOVERFLOW;
                return -1;
            }
            handle->handle_type = 1;
            handle->f_handle[0] = static_cast<unsigned char>(generation);
            return 0;
        };
        set_installed_record_observation_test_hooks(record_hooks);
        set_evaluated_devel_source_artifact_transport_test_hooks({[&](const ExplicitProcessInvocation& invocation) -> CapturedCommandResult {
                                                                      require(invocation.executable == "/usr/bin/sudo" && invocation.arguments[1] == MOGUET_SOURCE_ARTIFACT_INSTALL_HELPER_PATH, "bridge bypassed S5 helper");
                                                                      const auto& verb = invocation.arguments[2];
                                                                      if(verb == "prepare-exact") {
                                                                          ++prepare_calls;
                                                                          if(mode == "prepare-failure") return {"", 77, false};
                                                                          auto parsed = parse_source_artifact_install_trusted_helper_arguments({invocation.arguments.begin() + 2, invocation.arguments.end()});
                                                                          const auto& helper = require_arm<SourceArtifactInstallTrustedHelperInvocation>(parsed, "invalid S7-C prepare");
                                                                          SourceArtifactInstallRootPrepareRequest request{helper.transaction_token, helper.package_base, helper.directive, helper.needed, helper.no_confirm,
                                                                                                                          helper.artifacts, SourceArtifactInstallTrustedPurpose::ExactInstalledBinding};
                                                                          require(expected && !request.needed && request.directive == SourceArtifactInstallTrustedDirective::PreserveExistingReason &&
                                                                                      request.artifacts.size() == 1 && request.artifacts[0].archive_sha256 == expected->archive_digest.value() &&
                                                                                      request.artifacts[0].raw_mtree_sha256 == expected->mtree_digest.value(),
                                                                                  "bridge changed S4 identity/S5 intent");
                                                                          return {serialize_source_artifact_install_root_prepare_response(store.prepare(request, *invocation.standard_input_fd), request), 0, false};
                                                                      }
                                                                      if(verb == "execution-status") return {serialize_source_artifact_install_execution_observation(store.execution_status(token)), 0, false};
                                                                      require(verb == "consume-exact", "bridge called a legacy consume");
                                                                      ++consumes;
                                                                      return {store.consume_exact(token), 0, false};
                                                                  },
                                                                  [&](const ExplicitProcessInvocation& invocation) -> ExplicitProcessExecutionResult {
                                                                      if(invocation.arguments[2] == "abort") {
                                                                          ++aborts;
                                                                          store.abort(token);
                                                                          return {ExplicitProcessExecutionStatus::StartedKnownOutcome, 0};
                                                                      }
                                                                      require(invocation.arguments[2] == "execute", "unexpected bridge transport action");
                                                                      ++execute_calls;
                                                                      const auto status = store.execute(token);
                                                                      if(mode == "unknown") return {ExplicitProcessExecutionStatus::StartedOutcomeUnknown, std::nullopt};
                                                                      return {ExplicitProcessExecutionStatus::StartedKnownOutcome, status};
                                                                  },
                                                                  {}});
        set_source_artifact_install_trusted_exec_test_hook([&](const auto&) {
            store.observe_execution(token);
            if(mode == "nonzero") return 42;
            ++generation;
            write_package(expected->identity.full_version);
            if(mode == "no-post") return 0;
            int fds[2];
            require(pipe(fds) == 0, "bridge NeedsTargets pipe");
            const auto targets = fixture.package_name() + "\n";
            require(write(fds[1], targets.data(), targets.size()) == static_cast<ssize_t>(targets.size()), "bridge NeedsTargets write");
            static_cast<void>(close(fds[1]));
            if(existing)
                store.record_upgrade(token, fds[0]);
            else
                store.record_install(token, fds[0]);
            static_cast<void>(close(fds[0]));
            if(mode == "binding-failure") write_file(installed_record / "mtree", raw_mtree + "changed");
            return 0;
        });
        set_source_artifact_install_trusted_state_test_hook([&](auto event, int, const auto&) {
            if((mode == "cleanup-failure" && event == SourceArtifactInstallTrustedStateTestEvent::BeforeExactCleanup) ||
               (mode == "retirement-failure" && event == SourceArtifactInstallTrustedStateTestEvent::BeforeExactRetirement)) throw std::bad_alloc();
        });
        set_reviewed_devel_source_build_execution_test_hooks({[&](Stage stage, const EvaluatedDevelSourceBuildProof* built) {
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
                                                                  if(bootstrap && !acquisition_root.empty()) {
                                                                      if(stage == Stage::Context) {
                                                                          ++context_entries;
                                                                          require(fs::exists(acquisition_root / "moguet/example-base/.git"), "acquisition died before S3");
                                                                          const auto fresh = acquisition_root / "moguet/example-base";
                                                                          for(const auto& entry : fs::directory_iterator(fresh)) {
                                                                              require(entry.path().filename() != "evil.patch" && entry.path().filename() != "random-file" && entry.path().filename() != "ignored-residue" && entry.path().filename() != "wrong-head", "old overlay reached pin/S3");
                                                                              if(entry.is_regular_file()) {
                                                                                  std::ifstream input(entry.path());
                                                                                  require(std::string((std::istreambuf_iterator<char>(input)), {}).find("malicious-old") == std::string::npos, "old bytes reached pin/S3");
                                                                              }
                                                                          }
                                                                          if(mode == "s3-failure") write_file(fresh / "PKGBUILD", "changed after pin\n");
                                                                      }
                                                                      if(stage == Stage::Build) require(!fs::exists(acquisition_root) && acquisition_cleanups == 1, "S4 began before acquisition cleanup");
                                                                  }
#endif
                                                                  if(stage == Stage::Build) ++build_entries;
                                                                  if(stage != Stage::Transport) return;
                                                                  require(built && built->valid(), "bridge did not retain S4 proof");
                                                                  if(mode == "supplemental" || mode == "supplemental-collision") require_supplemental_artifact(*built, fixture);
                                                                  expected.emplace(built->artifact().evidence());
                                                                  actual_oid = *built->actual_built_revision().revision().value().git_commit();
                                                                  const auto mtree = capture_process("/usr/bin/bsdtar", {"-xOf", built->artifact().path().string(), ".MTREE"}, {"PATH=/usr/bin:/bin", "LC_ALL=C"});
                                                                  require(mtree.exit_code == 0 && xdg_generation_store_raw_contents_sha256(mtree.output) == expected->mtree_digest.value(), "bridge MTREE oracle mismatch");
                                                                  raw_mtree = mtree.output;
                                                                  if(mode == "snapshot-failure") {
                                                                      std::ofstream file(built->artifact().path(), std::ios::app);
                                                                      file << 'x';
                                                                  }
                                                                  if(mode == "publication-failure") fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::Write);
                                                                  if(mode == "publication-unknown") fail_next_xdg_generation_store_operation_for_test(XdgGenerationStoreTestFailurePoint::DirectorySync);
                                                                  if(mode == "no-allocation") run_xdg_generation_store_race_once_for_test(XdgGenerationStoreTestRacePoint::AfterVerifiedPublication, &deny_after_bridge_publication);
                                                              },
                                                              token});

#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        if(bootstrap) {
            const auto bin = runtime.path() / "bin";
            fs::create_directory(bin);
            const auto commands = runtime.path() / "commands.log";
            const auto script = [&](const fs::path& path, const std::string& text) {
                write_file(path, text);
                fs::permissions(path, fs::perms::owner_all);
            };
            script(bin / "pacman-conf", "#!/bin/sh\nif [ \"$1\" = --repo-list ]; then printf 'core\\n'; else printf 'RootDir = /\\nDBPath = %s\\n' \"$MOGUET_TEST_PACKAGE_METADATA_DB_PATH\"; fi\n");
            script(bin / "sudo", "#!/bin/sh\ncase \"$*\" in 'pacman -Syu'|'pacman -Syu --noconfirm') ;; *) exit 99 ;; esac\nprintf '%s\\n' \"$*\" >> \"$MOGUET_TEST_COMMAND_LOG\"\n");
            const bool provider_case = mode.starts_with("provider-");
            if(provider_case) {
                script(bin / "sudo", "#!/bin/sh\ncase \"$*\" in 'pacman -Syu') ;; 'pacman -S --asdeps --needed -- core/anchor') ;; *) exit 99 ;; esac\nprintf '%s\\n' \"$*\" >> \"$MOGUET_TEST_COMMAND_LOG\"\n" +
                                         std::string(mode == "provider-failure" ? "[ \"$2\" != -S ] || exit 42\n" : ""));
            }
            script(bin / "recipe-git", "#!/bin/sh\nfor argument do if [ \"$argument\" = fetch ]; then exit 0; fi; done\nexec /usr/bin/git \"$@\"\n");
            fs::create_directories(runtime.path() / "sync-stage/anchor-1-1");
            write_file(runtime.path() / "sync-stage/anchor-1-1/desc", "%NAME%\nanchor\n\n%BASE%\nanchor\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n");
            if(provider_case) {
                write_file(runtime.path() / "sync-stage/anchor-1-1/desc", "%NAME%\nanchor\n\n%BASE%\nanchor\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%PROVIDES%\nvirtual-active\n\n");
                fs::create_directories(runtime.path() / "sync-stage/declined-1-1");
                write_file(runtime.path() / "sync-stage/declined-1-1/desc", "%NAME%\ndeclined\n\n%BASE%\ndeclined\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%PROVIDES%\nvirtual-declined\n\n");
            }
            fs::create_directory(db / "sync");
            std::vector<std::string> archive_arguments{"-cf", (db / "sync/core.db").string(), "-C", (runtime.path() / "sync-stage").string(), "anchor-1-1"};
            if(provider_case) {
                archive_arguments.push_back("declined-1-1");
                for(const std::string name : {"zz-anchor", "zz-declined"}) {
                    const auto entry = name + "-1-1";
                    fs::create_directories(runtime.path() / "sync-stage" / entry);
                    write_file(runtime.path() / "sync-stage" / entry / "desc", "%NAME%\n" + name + "\n\n%BASE%\n" + name +
                                                                                   "\n\n%VERSION%\n1-1\n\n%ARCH%\nany\n\n%PROVIDES%\nvirtual-" + (name == "zz-anchor" ? "active" : "declined") + "\n\n");
                    archive_arguments.push_back(entry);
                }
            }
            require_process_success("/usr/bin/bsdtar", archive_arguments, {"PATH=/usr/bin:/bin"});
            ScopedEnvironmentVariable path("PATH", bin.string() + ":/usr/bin:/bin");
            ScopedEnvironmentVariable database("MOGUET_TEST_PACKAGE_METADATA_DB_PATH", db.string());
            ScopedEnvironmentVariable command_log("MOGUET_TEST_COMMAND_LOG", commands.string());
            ScopedEnvironmentVariable git("MOGUET_TEST_GIT_EXECUTABLE", (bin / "recipe-git").string());
            ScopedEnvironmentVariable git_count("GIT_CONFIG_COUNT", "1");
            ScopedEnvironmentVariable git_key("GIT_CONFIG_KEY_0", "url.file://" + upstream.remote().string() + ".insteadOf");
            ScopedEnvironmentVariable git_value("GIT_CONFIG_VALUE_0", upstream.url());
            ScopedEnvironmentVariable library("MAKEPKG_LIBRARY", "/usr/share/makepkg");
            set_aur_devel_update_database_paths_for_test(PacmanDatabasePaths{"/", db});
            const auto child = PackageChildIdentity::make(*fixture.execution_intent({"/", db}).request.aur_review_identity, fixture.package_name());
            const auto base = child.package_base();
            const auto p_directory = devel_build_provenance_store_entry_path(base);
            const auto r_directory = reviewed_source_state_store_entry_path(base);
            if(mode == "reviewed-same" || mode == "reviewed-changed") {
                {
                    auto pin = fixture.execution_pin();
                    require(pin.valid(), "prior reviewed fixture failed");
                }
                if(mode == "reviewed-changed") fixture.advance_recipe_for_bootstrap_test();
            }
            const auto old_cache = fixture.execution_checkout().canonical_path();
            const auto recipe_x = fixture.recipe_oid();
            const auto authoritative_metadata = fixture.recipe_metadata() + (mode == "acquire-metadata" ? "\n" : "");
            const auto old_git_calls = runtime.path() / "old-git.log";
            const bool migration_cache_case = mode != "newer" && mode != "provider-ordinary";
            if(migration_cache_case) {
                script(bin / "recipe-git", "#!/bin/sh\nfor argument do case \"$argument\" in *'" + old_cache.string() + "'*) printf 'old argument\\n' >> '" + old_git_calls.string() + "'; exit 98;; esac; done\nif [ \"$(pwd -P)\" = '" + old_cache.string() + "' ]; then printf 'old cwd\\n' >> '" + old_git_calls.string() + "'; exit 98; fi\nexec /usr/bin/git \"$@\"\n");
            }
            if(mode == "dirty-pkgbuild") write_file(old_cache / "PKGBUILD", "malicious-old-PKGBUILD\n");
            if(mode == "overlay") {
                write_file(old_cache / "evil.patch", "malicious-old-overlay\n");
                write_file(old_cache / "random-file", "malicious-old-overlay\n");
            }
            if(mode == "ignored") {
                write_file(old_cache / ".git/info/exclude", "ignored-residue\n");
                write_file(old_cache / "ignored-residue", "malicious-old-ignored\n");
            }
            if(mode == "supplemental-collision") {
                write_file(old_cache / "fix.patch", "malicious-old-patch\n");
                write_file(old_cache / "config.toml", "malicious-old-config\n");
            }
            if(mode == "wrong-head") {
                write_file(old_cache / "wrong-head", "unrelated-old-revision\n");
                require_process_success("/usr/bin/git", {"-C", old_cache.string(), "add", "wrong-head"}, git_environment(fixture.home()));
                require_process_success("/usr/bin/git", {"-C", old_cache.string(), "commit", "-qm", "wrong old HEAD"}, git_environment(fixture.home()));
            }
            if(mode == "malicious-config") {
                std::ofstream config_file(old_cache / ".git/config", std::ios::app);
                config_file << "\n[remote \"origin\"]\nurl = https://malicious.invalid/old.git\n[url \"https://malicious.invalid/\"]\ninsteadOf = https://aur.archlinux.org/\n[core]\nhooksPath = /nonexistent/old-hooks\n[filter \"old\"]\nclean = false\n[credential]\nhelper = false\n";
            }
            if(mode == "no-cache") fs::remove_all(old_cache);
            const auto cache_before = bootstrap_cache_snapshot(old_cache);
            std::optional<std::string> existing_provenance;
            if(mode == "invalid" || mode == "corrupt" || mode == "future" || mode == "unsafe") {
                fs::create_directories(p_directory);
                for(auto directory = p_directory; directory != fs::path(std::getenv("XDG_STATE_HOME")); directory = directory.parent_path())
                    fs::permissions(directory, fs::perms::owner_all);
                existing_provenance = mode == "invalid" ? "schema_version = 1\n" : mode == "future" ? "schema_version = 2\n"
                                                                                                    : "not TOML = [\n";
                write_file(p_directory / "1.toml", *existing_provenance);
                fs::permissions(p_directory / "1.toml", fs::perms::owner_read | fs::perms::owner_write);
                if(mode == "unsafe") write_file(p_directory / "unexpected", "unsafe history");
            }

            const bool decisions_only = multi && (mode == "decisions" || mode == "decision-cancel" || mode == "all-decline");
            const std::string first_name = decisions_only ? "bootstrap-a-git" : "bootstrap-a";
            const std::string last_name = decisions_only ? "zz-bootstrap-c-git" : "zz-bootstrap-c";
            const auto write_sidecar = [&](const std::string& name, const std::string& version) {
                const auto prior = db / "local" / (name + "-0-1");
                if(version != "0-1" && fs::exists(prior)) fs::remove_all(prior);
                const auto record = db / "local" / (name + "-" + version);
                fs::create_directories(record);
                write_file(record / "desc", "%NAME%\n" + name + "\n\n%BASE%\n" + name + "\n\n%VERSION%\n" + version + "\n\n%ARCH%\nany\n\n%REASON%\n0\n\n");
                write_file(record / "files", "%FILES%\nusr/share/fixture\n\n");
                write_file(record / "mtree", "sidecar-mtree");
            };
            std::vector<std::string> sidecar_calls;
            if(multi) {
                write_sidecar(first_name, "0-1");
                write_sidecar(last_name, "0-1");
                if(!decisions_only) set_aur_update_non_bootstrap_execution_test_hook([&](const auto& work) -> std::optional<PackageBaseSourceBuildExecutionResult> {
                    const auto& name = work.request.package_name;
                    if(mode == "provider-ordinary" && name == fixture.package_name()) return std::nullopt;
                    require(name == first_name || name == last_name, "bootstrap entered legacy fixture seam");
                    if(provider_case) {
                        require(work.cache_root.has_value(), "source lost activated cache authority");
                        work.cache_root->require_unchanged_identity();
                        std::ifstream log(commands);
                        const std::string recorded((std::istreambuf_iterator<char>(log)), {});
                        require(recorded == "pacman -Syu\npacman -S --asdeps --needed -- core/anchor\n", "provider did not precede source or retained declined provider");
                    }
                    sidecar_calls.push_back(name);
                    write_sidecar(name, "2-1");
                    return PackageBaseSourceBuildExecutionResult::make_for_aur_update_runner_test(work.request.checkout_name,
                                                                                                  {{ArtifactPackageIdentity{name, "2-1", ArtifactPackageBaseIdentity::known(name), ArtifactPackageArchitectureIdentity::known("any")},
                                                                                                    DesiredInstallReason::Explicit, ArtifactInstallExecutionOutcome::Installed}},
                                                                                                  {});
                });
            }
            std::map<std::string, unsigned> recipe_counts;
            unsigned trial_calls = 0;
            set_devel_tracking_bootstrap_test_hooks({[&](const PackageChildIdentity& requested) -> std::optional<DevelTrackingBootstrapRecipeObservation> {
                                                         ++trial_calls;
                                                         const auto calls = ++recipe_counts[requested.package_name()];
                                                         if(mode == "decisions" && requested.package_name() == first_name && calls >= 4) return std::nullopt;
                                                         std::string metadata = authoritative_metadata;
                                                         if(decisions_only && requested.package_name() != fixture.package_name()) {
                                                             const auto base_position = metadata.find("pkgbase = " + fixture.package_base());
                                                             const auto name_position = metadata.find("pkgname = " + fixture.package_name());
                                                             require(base_position != std::string::npos && name_position != std::string::npos, "trial fixture identity missing");
                                                             metadata.replace(name_position, 10 + fixture.package_name().size(), "pkgname = " + requested.package_name());
                                                             metadata.replace(base_position, 10 + fixture.package_base().size(), "pkgbase = " + requested.package_base().package_base());
                                                         }
                                                         if(mode == "unsupported") metadata += "pkgname = unsupported-sibling\n";
                                                         return DevelTrackingBootstrapRecipeObservation{SourceRevisionIdentity::git_commit(recipe_x), metadata};
                                                     },
                                                     {}});
            if(!decisions_only) {
                // Keep production HEAD parsing, exact-id cgit URL construction,
                // metadata parsing; inject no trial identity or old-cache bytes.
                set_devel_tracking_bootstrap_test_hooks({{}, {}, [&](const ExplicitProcessInvocation& invocation, const BoundedProcessPolicy&) {
                        ++trial_calls;
                        if(mode == "advance-before-revalidation" && trial_calls == 3) fixture.advance_recipe_for_bootstrap_test();
                        require(invocation.arguments.back() == "HEAD", "Supplemental trial did not observe recipe HEAD");
                        const auto record = (mode == "advance-before-revalidation" && trial_calls >= 3 ? fixture.recipe_oid() : recipe_x) + "\tHEAD\n";
                        return BoundedCapturedProcessResult{record + record, BoundedProcessExited{0}}; }, [&](const std::string& url) -> std::optional<std::string> {
                        require(url == "https://aur.archlinux.org/cgit/aur.git/plain/.SRCINFO?h=" + fixture.package_base() + "&id=" + (mode == "advance-before-revalidation" && trial_calls >= 3 ? fixture.recipe_oid() : recipe_x),
                                "Supplemental trial lost exact recipe metadata URL");
                        return authoritative_metadata + (mode == "unsupported" ? "pkgname = unsupported-sibling\n" : ""); }});
            }
            AppConfig config;
            config.user_config.review.diff = mode == "diff-skip" ? ReviewPolicy::Skip : ReviewPolicy::Prompt;
            std::vector<std::string> argument_values{"moguet", "-Syu", "--noedit"};
            if(mode == "nodiff") argument_values.push_back("--nodiff");
            if(mode == "noconfirm") argument_values.push_back("--noconfirm");
            std::vector<char*> arguments;
            for(auto& value : argument_values)
                arguments.push_back(value.data());
            const auto parsed = parse_cli_arguments(static_cast<int>(arguments.size()), arguments.data());
            require(parsed && validate_cli_invocation_contract(*parsed).is_valid(), "ordinary -Syu CLI contract failed");
            config.user_config = compose_user_config(config.user_config, parsed->cli_overrides);
            config.no_confirm = parsed->cli_overrides.no_confirm;
            if(provider_case) config.provider_selection = make_provider_selection_session(config.no_confirm);
            auto route = classify_sync_invocation_route(*parsed);
            auto request = make_compatible_system_aur_update_request(std::get<AutoSystemUpdateRouteCandidate>(std::move(route)));
            require(request.has_value(), "exact targetless route did not produce authority");
            const auto acquisition_parent = runtime.path() / "acquisitions";
            fs::create_directory(acquisition_parent);
            RecipeAcquisitionTestHooks acquisition_hooks;
            acquisition_hooks.parent = acquisition_parent;
            acquisition_hooks.event = [&](RecipeAcquisitionStage stage, const fs::path& root) {
                if(stage == RecipeAcquisitionStage::Initialization) {
                    ++acquisition_creations;
                    acquisition_root = root;
                    require(trial_calls >= 4, "acquisition began before final accepted revalidation");
                    if(mode == "acquire-unsafe") write_file(root / "moguet/example-base/.git", "gitdir: /never-open\n");
                }
                if(stage == RecipeAcquisitionStage::Fetch && mode == "advance-after-revalidation") fixture.advance_recipe_for_bootstrap_test();
                if(stage == RecipeAcquisitionStage::Cleanup) {
                    ++acquisition_cleanups;
                    if(mode == "acquire-cleanup" || mode == "review-decline-cleanup" || mode == "review-cancel-cleanup")
                        fs::permissions(root, fs::perms::group_write, fs::perm_options::add);
                }
            };
            acquisition_hooks.process = [&](ExplicitProcessInvocation invocation, const BoundedProcessPolicy& policy) {
                ++acquisition_processes;
                require(invocation.executable == "/usr/bin/git", "acquisition lost fixed Git executable");
                for(const auto* key : {"GIT_DIR=", "GIT_WORK_TREE=", "GIT_CONFIG_COUNT=", "GIT_OBJECT_DIRECTORY=", "HOME="})
                    for(const auto& value : invocation.environment)
                        require(!value.starts_with(key), "acquisition inherited old Git policy");
                const bool fetch = std::find(invocation.arguments.begin(), invocation.arguments.end(), "fetch") != invocation.arguments.end();
                if(fetch) {
                    ++acquisition_fetches;
                    require(invocation.arguments[invocation.arguments.size() - 2] == "https://aur.archlinux.org/example-base.git" && invocation.arguments.back() == recipe_x,
                            "acquisition changed canonical URL or expected X");
                    if(mode == "acquire-unavailable") return BoundedCapturedProcessResult{"", BoundedProcessExited{0}};
                    if(mode == "acquire-launch") {
                        invocation.executable = (runtime.path() / "missing-git").string();
                    } else if(mode == "acquire-nonzero" || mode == "acquire-signal" || mode == "acquire-timeout" || mode == "acquire-cancel" || mode == "acquire-cancel-zero") {
                        invocation.executable = "/bin/sh";
                        invocation.arguments = {"-c", mode == "acquire-nonzero" ? "exit 42" : mode == "acquire-signal"    ? "kill -TERM $$"
                                                                                          : mode == "acquire-timeout"     ? "while :; do :; done"
                                                                                          : mode == "acquire-cancel-zero" ? "trap 'exit 0' INT; kill -INT $PPID; while :; do :; done"
                                                                                                                          : "trap 'exit 130' INT; kill -INT $PPID; while :; do :; done"};
                        auto bounded = policy;
                        bounded.hard_timeout = std::chrono::milliseconds(200);
                        return capture_bounded_explicit_process_output_raw(invocation, bounded);
                    } else {
                        // Offline transport only, after asserting the production URL,
                        // complete environment and HTTPS policy. No acquired success injection.
                        invocation.arguments[invocation.arguments.size() - 2] = fixture.recipe_remote().string();
                        auto file_policy = std::find(invocation.arguments.begin(), invocation.arguments.end(), "protocol.file.allow=never");
                        require(file_policy != invocation.arguments.end(), "acquisition lost HTTPS-only policy");
                        *file_policy = "protocol.file.allow=always";
                    }
                }
                return capture_bounded_explicit_process_output_raw(invocation, policy);
            };
            set_recipe_acquisition_test_hooks(std::move(acquisition_hooks));
            auto result = execute_prepared_system_aur_update_operation(prepare_system_aur_update_operation(std::move(*request)), config);
            require(result.repository.status == SystemAurUpdateRepositoryPhaseStatus::Completed, "fixture repository phase did not complete");
            if(!result.aur.operation_result && result.aur.diagnostic) std::cerr << *result.aur.diagnostic << '\n';
            require(result.aur.operation_result.has_value(), "bootstrap lost filtered result");
            const auto& filtered = *result.aur.operation_result;
            const auto& targets = filtered.reduced_operation_result.targets;
            const std::size_t bootstrap_index = multi ? 1 : 0;
            require(targets.size() == (multi ? 3U : 1U) && targets[bootstrap_index].update.installed_name == fixture.package_name(), "bootstrap lost original query target correlation");
            const auto& target = targets[bootstrap_index];
            if(migration_cache_case) {
                require(cache_before == bootstrap_cache_snapshot(old_cache), "migration changed old checkout bytes/inventory/HEAD/refs/config");
                require(!fs::exists(old_git_calls), "migration invoked Git on old checkout");
            }
            if(provider_case) {
                require(filtered.execution.has_value(), "provider fixture has no execution");
                const auto& transaction = filtered.execution->selected_repository_provider_transaction;
                if(transaction.selected_providers.size() != 1 || transaction.selected_providers.front().package_name != "anchor") {
                    present_filtered_aur_update_execution_result(filtered);
                    std::cerr << "provider count=" << transaction.selected_providers.size() << '\n';
                }
                require(transaction.selected_providers.size() == 1 && transaction.selected_providers.front().package_name == "anchor",
                        "filtered provider transaction lost active provider or retained declined contribution");
            }
            if(mode == "provider-failure") {
                const auto assert_provider_failure = [&](const AurUpdateSourceBuildExecutionResult& execution, SelectedRepositoryProviderTransactionStatus status) {
                    const auto reduced = reduce_aur_update_operation_result(filtered.preflight, filtered.preparation,
                                                                            DevelRequiresCheckPolicy::SkipIndependentTarget, execution);
                    require(reduced.status == AurUpdateOperationStatus::StoppedOnProviderTransactionFailure &&
                                reduced.reduction_issues.empty() && !reduced.is_success() &&
                                reduced.selected_repository_provider_transaction.status == status &&
                                reduced.selected_repository_provider_transaction.command_exit_status == execution.selected_repository_provider_transaction.command_exit_status,
                            "legitimate bootstrap skip corrupted provider failure classification");
                    require(reduced.targets[0].status == AurUpdateOperationTargetStatus::NotAttempted &&
                                reduced.targets[1].status == AurUpdateOperationTargetStatus::Skipped &&
                                reduced.targets[2].status == AurUpdateOperationTargetStatus::NotAttempted &&
                                execution.work_item_results[1].status == AurUpdateWorkItemExecutionStatus::BootstrapSkipped,
                            "provider failure lost declined target or executed source");
                };
                require(filtered.execution->status == AurUpdateInvocationExecutionStatus::StoppedOnProviderTransactionFailure &&
                            filtered.reduced_operation_result.reduction_issues.empty() && sidecar_calls.empty() &&
                            build_entries == 0 && execute_calls == 0 && g_bridge_publication_entries == 0,
                        "provider failure did not stop before source execution");
                assert_provider_failure(*filtered.execution, SelectedRepositoryProviderTransactionStatus::Failed);
                auto unknown = *filtered.execution;
                unknown.selected_repository_provider_transaction.status = SelectedRepositoryProviderTransactionStatus::OutcomeUnknown;
                unknown.selected_repository_provider_transaction.command_exit_status.reset();
                assert_provider_failure(unknown, SelectedRepositoryProviderTransactionStatus::OutcomeUnknown);
                for(const std::string corruption : {"decision", "origin", "intent", "accepted", "index", "root", "mapping", "updated", "completed", "cancelled"}) {
                    auto forged = unknown;
                    auto preflight = filtered.preflight;
                    auto& item = forged.work_item_results[1];
                    if(corruption == "decision") item.bootstrap_decision.reset();
                    if(corruption == "origin") item.bootstrap_decision->confirmation.reset();
                    if(corruption == "intent") preflight.targets[1].update.bootstrap.reset();
                    if(corruption == "accepted") item.bootstrap_decision = AurUpdateBootstrapDecision{AurUpdateBootstrapDecisionState::Accepted,
                                                                                                      ConfirmationAccepted{ConfirmationDecisionOrigin::ExplicitToken}};
                    if(corruption == "index") item.work_item_index = 99;
                    if(corruption == "root") item.bootstrap_skipped_roots = {0};
                    if(corruption == "mapping") item.affected_roots = forged.work_item_results[0].affected_roots;
                    if(corruption == "updated") forged.work_item_results[0].status = AurUpdateWorkItemExecutionStatus::Updated;
                    if(corruption == "completed") forged.status = AurUpdateInvocationExecutionStatus::Completed;
                    if(corruption == "cancelled") item.cancellation = ConfirmationCancelled{ConfirmationCancellationReason::ExplicitToken};
                    const auto rejected = reduce_aur_update_operation_result(preflight, filtered.preparation,
                                                                             DevelRequiresCheckPolicy::SkipIndependentTarget, forged);
                    require(rejected.status == AurUpdateOperationStatus::InconsistentResult && !rejected.reduction_issues.empty(),
                            "provider failure accepted forged bootstrap skip: " + corruption);
                }
                require(!fs::exists(p_directory / "1.toml"), "provider failure published baseline");
                std::cout << "S553 provider Failed / OutcomeUnknown + default-No skip / 10 negatives PASS\n";
                std::cout << "S553 production " << case_name << " PASS\n";
                continue;
            }
            if(mode.starts_with("required-") || mode == "shared-base") {
                if(filtered.reduced_operation_result.status != AurUpdateOperationStatus::BlockedBeforeExecution) {
                    present_filtered_aur_update_execution_result(filtered);
                    for(const auto& issue : filtered.issues)
                        std::cerr << "filtered: " << issue.diagnostic << '\n';
                }

                require(!result.is_success() && filtered.reduced_operation_result.status == AurUpdateOperationStatus::BlockedBeforeExecution &&
                            !filtered.execution && sidecar_calls.empty() && build_entries == 0 && execute_calls == 0,
                        "required RequiresCheck was weakened by bootstrap");
                const auto wanted = mode == "required-provider" ? AurUpdateRequiredDevelTargetRelation::AurProvider : mode == "shared-base" ? AurUpdateRequiredDevelTargetRelation::RequiredArtifactChild
                                                                                                                                            : AurUpdateRequiredDevelTargetRelation::AurExactDependency;
                require(std::any_of(targets.front().preflight_issues.begin(), targets.front().preflight_issues.end(), [&](const auto& issue) {
                            return issue.reason == AurUpdateExecutionReason::RequiredDevelTargetRequiresCheck &&
                                   issue.required_devel_target_blocker && issue.required_devel_target_blocker->relation == wanted;
                        }),
                        "required relation lost its typed blocker");
                if(mode == "shared-base") require(trial_calls == 0, "shared PackageBase received bootstrap trial");
                require(!fs::exists(p_directory / "1.toml"), "required target published provenance");
                std::cout << "S553 production " << case_name << " PASS\n";
                continue;
            }
            if(decisions_only) {
                require(filtered.execution && sidecar_calls.empty() && build_entries == 0 && execute_calls == 0, "decision fixture performed package mutation");
                const auto& items = filtered.execution->work_item_results;
                require(items.size() == 3, "multiple bootstrap candidates lost ordering");
                if(mode == "all-decline") {
                    require(result.is_success(), "all-decline did not complete with attention");
                    for(const auto& item : targets)
                        require(item.status == AurUpdateOperationTargetStatus::Skipped, "all-decline was not a typed skip");
                } else if(mode == "decision-cancel") {
                    require(!result.is_success() && filtered.execution->phase == AurUpdateInvocationExecutionPhase::BootstrapDecisions &&
                                targets[0].status == AurUpdateOperationTargetStatus::NotAttempted && targets[1].status == AurUpdateOperationTargetStatus::Cancelled &&
                                targets[2].status == AurUpdateOperationTargetStatus::NotAttempted && !items[2].bootstrap_decision,
                            "decision cancellation continued or lost unattempted targets");
                } else {
                    require(!result.is_success() && targets[0].status == AurUpdateOperationTargetStatus::Failed &&
                                targets[1].status == AurUpdateOperationTargetStatus::Skipped && targets[2].status == AurUpdateOperationTargetStatus::NotAttempted,
                            "accepted/declined candidate execution order changed");
                    require(items[0].bootstrap_decision->state == AurUpdateBootstrapDecisionState::Accepted &&
                                items[1].bootstrap_decision->state == AurUpdateBootstrapDecisionState::Declined &&
                                items[2].bootstrap_decision->state == AurUpdateBootstrapDecisionState::Accepted,
                            "multiple decisions were flattened");
                }
                require(!fs::exists(p_directory / "1.toml"), "decision-only fixture published provenance");
                std::cout << "S553 production " << case_name << " PASS\n";
                continue;
            }
            if(mode == "newer" || mode == "provider-ordinary") {
                require(result.is_success() && target.status == AurUpdateOperationTargetStatus::Updated && !target.update.bootstrap &&
                            aur_update_basis(target.update) == AurUpdateBasis::Version && trial_calls == 0 && build_entries == 1 && execute_calls == 1,
                        "normal version update was intercepted or executed twice");
                require(!filtered.execution->work_item_results.front().bootstrap_decision, "normal version update gained bootstrap confirmation");
                if(provider_case) require(sidecar_calls == std::vector<std::string>{first_name, last_name} &&
                                              filtered.execution->selected_repository_provider_transaction.status == SelectedRepositoryProviderTransactionStatus::Succeeded,
                                          "ordinary provider/source positive path did not complete");
                std::cout << "S553 production " << case_name << " PASS\n";
                continue;
            }
            const bool success = mode == "accept" || mode == "older" || mode == "reviewed-same" || mode == "reviewed-changed" || mode == "supplemental" || mode == "supplemental-collision" || mode == "no-cache" || mode == "clean-cache" || mode == "dirty-pkgbuild" || mode == "overlay" || mode == "ignored" || mode == "wrong-head" || mode == "malicious-config" || mode == "advance-after-revalidation";
            const bool skipped = mode == "advance-before-revalidation" || mode == "provider-decline" || mode == "decline" || mode == "default-no" || mode == "non-tty" || mode == "noconfirm" || mode == "nodiff" || mode == "diff-skip" ||
                                 mode == "unsupported" || mode == "invalid" || mode == "corrupt" || mode == "future" || mode == "unsafe";
            const bool process_cancelled = mode == "acquire-cancel" || mode == "acquire-cancel-zero";
            const bool cancelled = mode == "cancel" || mode == "eof" || mode == "review-cancel" || mode == "review-eof" || mode == "review-cancel-cleanup" || process_cancelled;
            if(success) {
                require(result.is_success() && target.status == AurUpdateOperationTargetStatus::Updated, "bootstrap did not complete");
                require(target.update.devel_assessment.state() == DevelUpdateAssessmentState::RequiresCheck && !aur_update_basis(target.update), "bootstrap fabricated update availability");
                require(filtered.execution && filtered.execution->work_item_results.size() == (multi ? 3U : 1U), "bootstrap execution missing");
                const auto& execution = filtered.execution->work_item_results[bootstrap_index];
                require(execution.bootstrap_decision && execution.bootstrap_decision->state == AurUpdateBootstrapDecisionState::Accepted &&
                            execution.devel_execution && execution.devel_execution->complete &&
                            execution.devel_execution->publication == Pub::Complete,
                        "bootstrap lost acceptance or S6 authority");
                require(execution.devel_execution->production_outcome->source_provenance.reviewed_outcome == ProductionReviewedSourceOutcome::BootstrapFullReview,
                        "bootstrap reused incremental/already-reviewed continuation");
                if(!multi && mode == "accept") {
                    auto observation = observe_aur_update_source_build_preparation(
                        filtered.preflight, filtered.preparation.build_unit_selection,
                        DevelRequiresCheckPolicy::SkipIndependentTarget, SavedSourcePreferencePolicy::Strict, false, config);
                    require(observation.is_ready(), "bootstrap intent failed preparation correlation");
                    auto dropped = observation;
                    dropped.production_preflight->work_items.front().request.devel_tracking_bootstrap.reset();
                    require(!dropped.is_ready(), "preparation accepted a dropped bootstrap intent");
                    observation.affected_update_targets.front().update.bootstrap.reset();
                    require(!observation.is_ready(), "preparation accepted a bootstrap intent without its original target");
                }
                for(const bool corrupt_origin : {true, false}) {
                    auto forged = *filtered.execution;
                    auto& item = forged.work_item_results[bootstrap_index];
                    item.status = AurUpdateWorkItemExecutionStatus::BootstrapSkipped;
                    item.failure_kind = AurUpdateWorkItemFailureKind::None;
                    item.production_outcome.reset();
                    item.devel_execution.reset();
                    item.bootstrap_decision = AurUpdateBootstrapDecision{AurUpdateBootstrapDecisionState::Declined,
                                                                         corrupt_origin ? ConfirmationResult{ConfirmationAccepted{ConfirmationDecisionOrigin::ExplicitToken}}
                                                                                        : ConfirmationResult{ConfirmationDeclined{ConfirmationDecisionOrigin::ExplicitToken}}};
                    item.bootstrap_skipped_roots = {corrupt_origin ? bootstrap_index : targets.size() + 1};
                    for(auto& child_result : item.child_results) {
                        child_result.status = AurUpdateChildExecutionStatus::BootstrapSkipped;
                        child_result.selected_artifact.reset();
                    }
                    const auto reduced = reduce_aur_update_operation_result(filtered.preflight, filtered.preparation,
                                                                            DevelRequiresCheckPolicy::SkipIndependentTarget, forged);
                    require(!reduced.is_success() && reduced.status == AurUpdateOperationStatus::InconsistentResult,
                            "forged bootstrap decline/root mapping became success");
                }
                const auto readback = read_devel_build_provenance(base);
                const auto& loaded = require_arm<DevelBuildProvenanceStoreLoaded>(readback, "bootstrap publication readback missing");
                require(*loaded.provenance.reviewed_recipe_revision().value().git_commit() == recipe_x && recipe_x != actual_oid, "recipe pin changed X or contaminated upstream S4 identity");
                require(*loaded.provenance.actual_built_revision().revision().value().git_commit() == actual_oid && actual_oid == upstream.oid(), "bootstrap published an observed/cache OID instead of built proof");
                set_devel_package_assessment_test_hooks({{}, [&](const auto& remote_request) {
                                                             return parse_git_remote_revision_observation(remote_request, 0, upstream.oid() + "\tHEAD\n");
                                                         }});
                const DevelPackageAssessmentTarget assessment_target{base, {child}, true};
                const auto same = assess_current_devel_package(assessment_target);
                require(same.assessment.state() == DevelUpdateAssessmentState::UpToDate, "post-bootstrap same OID was not UpToDate");
                const auto trials_before_fast_path = trial_calls;
                const auto fast_request = make_compatible_system_aur_update_request(
                    std::get<AutoSystemUpdateRouteCandidate>(classify_sync_invocation_route(*parsed)));
                auto fast = execute_prepared_system_aur_update_operation(prepare_system_aur_update_operation(*fast_request), config);
                require(fast.is_success() && trial_calls == trials_before_fast_path && build_entries == 1 && execute_calls == 1 && acquisition_creations == 1 && acquisition_fetches == 1,
                        "valid provenance fast path gained bootstrap/rebuild");
                require(fast.aur.operation_result->reduced_operation_result.targets[bootstrap_index].update.devel_assessment.state() == DevelUpdateAssessmentState::UpToDate,
                        "next ordinary update did not use the published baseline");
                upstream.commit("bootstrap remote advanced\n");
                const auto different = assess_current_devel_package(assessment_target);
                require(different.assessment.state() == DevelUpdateAssessmentState::UpdateAvailable && different.update_basis == DevelPackageUpdateBasis::GitRevision,
                        "post-bootstrap remote advance was not GitRevision update");
                std::cout << "S553 lifecycle S6 Complete generation=" << loaded.observed.generation
                          << " sha256=" << xdg_generation_store_raw_contents_sha256(loaded.observed.raw_contents)
                          << " built=" << actual_oid << " same=UpToDate different=UpdateAvailable(GitRevision)\n";
            } else if(skipped) {
                require(result.is_success() && target.status == AurUpdateOperationTargetStatus::Skipped &&
                            target.skip_kind == AurUpdateExecutionSkipKind::IndependentDevelRequiresCheck,
                        "bootstrap decline/unavailable lost RequiresCheck skip");
                require(build_entries == 0 && execute_calls == 0 && g_bridge_publication_entries == 0, "unaccepted bootstrap mutated package/provenance");
                require(cache_before == bootstrap_cache_snapshot(old_cache), "unaccepted bootstrap mutated checkout/cache");
            } else {
                require(!result.is_success(), "failed/cancelled bootstrap became success");
                present_filtered_aur_update_execution_result(filtered);
                if(cancelled) require(target.status == AurUpdateOperationTargetStatus::Cancelled && (process_cancelled ? !target.cancellation.has_value() : target.cancellation.has_value()),
                                      "bootstrap cancellation lost typed partial result");
                if(mode == "publication-failure" || mode == "publication-unknown") {
                    const auto& execution = filtered.execution->work_item_results[bootstrap_index];
                    require(execution.devel_execution && execution.devel_execution->operation == Operation::Succeeded &&
                                execution.devel_execution->publication == (mode == "publication-failure" ? Pub::Failed : Pub::OutcomeUnknown),
                            "bootstrap flattened install and publication outcomes");
                    require(!execution.devel_execution->owner->publication()->identity(), "failed/unknown publication exposed success identity");
                }
            }
            if(mode.starts_with("acquire-") || mode.ends_with("-cleanup")) {
                require(filtered.execution && filtered.reduced_operation_result.reduction_issues.empty(), "acquisition detail broke operation correlation");
                const auto& item = filtered.execution->work_item_results[bootstrap_index];
                require(item.recipe_acquisition_failure && target.recipe_acquisition_failure, "runner/reducer lost typed acquisition detail");
                const auto& failure = *item.recipe_acquisition_failure;
                using Reason = RecipeAcquisitionFailureReason;
                const auto reason = mode == "acquire-metadata" ? Reason::MetadataMismatch : mode == "acquire-unavailable" ? Reason::ExpectedCommitUnavailable
                                                                                        : mode == "acquire-unsafe"        ? Reason::UnsafeFilesystem
                                                                                        : mode.ends_with("-cleanup")      ? Reason::IdentityChanged
                                                                                        : process_cancelled               ? Reason::Cancelled
                                                                                                                          : Reason::GitProcessFailed;
                require(failure.reason == reason, "acquisition reason was flattened");
                if(mode == "acquire-launch") require(failure.process && std::holds_alternative<BoundedProcessLaunchOrSetupFailure>(failure.process->outcome), "launch detail lost");
                if(mode == "acquire-nonzero") require(failure.process && std::get<BoundedProcessExited>(failure.process->outcome).exit_code == 42, "Git exit detail lost");
                if(mode == "acquire-timeout") require(failure.process && std::holds_alternative<BoundedProcessTimedOut>(failure.process->outcome), "timeout detail lost");
                if(mode == "acquire-signal") require(failure.process && std::get<BoundedProcessSignaled>(failure.process->outcome).signal_number == SIGTERM, "signal detail lost");
                if(process_cancelled) require(failure.process && failure.process->cancellation_signal == SIGINT, "parent cancellation detail lost");
                if(mode == "acquire-cancel-zero") require(std::get<BoundedProcessExited>(failure.process->outcome).exit_code == 0, "cancel test did not actually exit zero");
                if(process_cancelled) {
                    for(const bool fake_confirmation : {true, false}) {
                        auto forged = *filtered.execution;
                        auto& altered = forged.work_item_results[bootstrap_index];
                        if(fake_confirmation)
                            altered.cancellation = ConfirmationCancelled{ConfirmationCancellationReason::ExplicitToken};
                        else
                            altered.recipe_acquisition_failure.reset();
                        const auto rejected = reduce_aur_update_operation_result(filtered.preflight, filtered.preparation, DevelRequiresCheckPolicy::SkipIndependentTarget, forged);
                        require(rejected.status == AurUpdateOperationStatus::InconsistentResult && !rejected.is_success(), "acquisition cancellation accepted invented/missing cause");
                    }
                }
                if(mode.ends_with("-cleanup")) require(failure.cleanup && failure.abandoned_root && fs::exists(*failure.abandoned_root) && result.has_cleanup_failure(), "cleanup consequence lost");
                require(build_entries == 0 && execute_calls == 0 && g_bridge_publication_entries == 0, "acquisition failure fell back to old X or started S4/install/P");
                if(mode == "acquire-cleanup")
                    require(context_entries == 1 && item.devel_execution && item.devel_execution->owner->stage() == Stage::RecipeCleanup &&
                                item.production_outcome->build_outcome == ProductionSourceBuildCommandOutcome::NotAttempted && fs::exists(r_directory / "1.toml"),
                            "S3 cleanup failure continued build or rolled back R");
                else
                    require(context_entries == 0 && !fs::exists(r_directory / "1.toml"), "failed acquisition/review minted pin/S3");
            }
            if(mode == "s3-failure") {
                require(filtered.execution && filtered.execution->work_item_results[bootstrap_index].devel_execution->owner->context_failure() &&
                            context_entries == 1 && build_entries == 0 && execute_calls == 0 && fs::exists(r_directory / "1.toml"),
                        "S3 failure resumed or rolled back R");
            }
            const bool never_acquired = skipped || mode == "cancel" || mode == "eof";
            require(acquisition_creations == (never_acquired ? 0U : 1U), "unexpected acquisition count before/after Yes");
            if(never_acquired)
                require(acquisition_processes == 0 && acquisition_fetches == 0, "decline/cancel acquired recipe");
            else
                require(acquisition_cleanups == 1 && (mode.ends_with("-cleanup") || !fs::exists(acquisition_root)), "acquisition lifetime leaked or retried cleanup");
            if(success) require(acquisition_fetches == 1 && context_entries == 1, "migration did not use fresh acquire/pin/S3");
            if(!success && mode != "publication-unknown") {
                if(existing_provenance) {
                    std::ifstream file(p_directory / "1.toml");
                    require(std::string((std::istreambuf_iterator<char>(file)), {}) == *existing_provenance, "bootstrap repaired existing invalid provenance");
                } else
                    require(!fs::exists(p_directory / "1.toml"), "negative bootstrap published a baseline");
            }
            if(mode == "noconfirm" || mode == "nodiff" || mode == "diff-skip" || mode == "non-tty") require(trial_calls == 0, "noninteractive/bypassed route observed trial source");
            if(skipped || mode == "cancel" || mode == "eof" || mode == "review-decline" || mode == "review-cancel") {
                require(!fs::exists(r_directory / "1.toml"), "unaccepted review advanced reviewed-source state");
                require(execute_calls == 0, "negative bootstrap attempted install");
            }
            if(multi) {
                const bool decision_cancel = mode == "cancel" || mode == "eof";
                require(targets[0].status == (decision_cancel ? AurUpdateOperationTargetStatus::NotAttempted : AurUpdateOperationTargetStatus::Updated),
                        "bootstrap lost completed prefix");
                const bool continues = success || skipped;
                require(targets[2].status == (continues ? AurUpdateOperationTargetStatus::Updated : AurUpdateOperationTargetStatus::NotAttempted),
                        "bootstrap executed the suffix after stop or skipped it after decline");
                require(sidecar_calls.size() == (decision_cancel ? 0U : continues ? 2U
                                                                                  : 1U),
                        "wrong number of unrelated target mutations");
            }
            std::cout << "S553 production " << case_name << " PASS\n";
            continue;
        }
#endif
        auto intent = fixture.execution_intent({"/", db});
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
        std::optional<ScopedEnvironmentVariable> registered_state;
        if(normal && mode.starts_with("registered-")) {
            registered_state.emplace("MOGUET_TEST_REGISTERED_DEVEL_STATE", mode.substr(11));
            intent.request.only_if_updated = true;
            intent.request.installed_snapshot = SourceInstalledSnapshot{std::string("0-1")};
            aur_devel_update_test_stub::reset_registered_calls();
        }
#endif

        if(mode == "new-dependency" || mode == "upgrade-dependency" || mode == "dependency-keeps-explicit") intent.required_targets.front().desired_reason = DesiredInstallReason::Dependency;
        if(mode == "needed") intent.request.needed = true;
        if(mode == "split") intent.required_targets.push_back({fixture.package_base(), "another-child", DesiredInstallReason::Explicit});
        if(mode == "rmdeps") intent.rm_deps = true;
        if(mode == "only-if-updated") intent.request.only_if_updated = true;
        if(mode == "environment") intent.request.custom_environment.ordered_assignments.push_back({"PKGDEST", "/forbidden"});
        if(mode == "database-world") intent.database_paths.db_path = "/different-db";
        if(mode == "artifact-mismatch") {
            intent.request.package_name = "wrong-child";
            intent.required_targets[0].package_name = "wrong-child";
        }
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
        if(normal && (mode == "registered-same" || mode == "registered-unknown" || mode == "registered-check")) {
            const auto result = fixture.normal_execution(intent, true);
            const auto expected_status = mode == "registered-same" ? SourceBuildExecutionStatus::UpToDate : mode == "registered-check" ? SourceBuildExecutionStatus::DevelRequiresCheckSkipped
                                                                                                                                       : SourceBuildExecutionStatus::AuthoritativeIncomplete;
            require(result.status == expected_status && result.devel_update_query && !result.devel_execution, "registered decision was flattened into execution");
            if(mode == "registered-check") require(result.devel_rebuild_confirmation == ConfirmationDecisionOrigin::NoConfirm, "noconfirm approved a rebuild");
            require(aur_devel_update_test_stub::registered_call_count() == 1 && build_entries == 0 && prepare_calls == 0 && execute_calls == 0, "registered negative repeated query or built");
            fixture.require_no_provenance_publication();
            std::cout << "S7D normal registered " << mode << " / query1 / build0 / publication0 PASS\n";
            continue;
        }
        if(normal && (mode == "new-dependency" || mode == "promotion" || mode == "needed" || mode == "split" || mode == "rmdeps" || mode == "only-if-updated" || mode == "legacy" || mode == "overlay" || mode == "overlay-legacy")) {
            if(mode == "overlay") intent.request.authoritative_devel_update = true;
            auto pin = fixture.execution_pin(mode == "overlay" || mode == "overlay-legacy");
            auto selected = select_normal_reviewed_source_execution(fixture.execution_checkout(), std::move(pin), ProductionReviewedSourceOutcome::InitialFullReview, std::nullopt, &intent);
            const bool reject = mode == "overlay" || mode == "only-if-updated";
            require(reject ? std::holds_alternative<ReviewedDevelSourceBuildRejected>(selected) : std::holds_alternative<ProductionArtifactSourceTree>(selected), "normal Legacy/Reject selection differs");
            require(build_entries == 0 && prepare_calls == 0 && execute_calls == 0, "normal selection started an unexpected authority path");
            fixture.require_no_provenance_publication();
            std::cout << "S7D normal selection " << mode << " / authoritative0 / publication0 PASS\n";
            continue;
        }
        if(normal && mode == "database-world") {
            bool rejected = false;
            try {
                static_cast<void>(fixture.normal_execution(intent));
            } catch(const std::exception& error) {
                rejected = std::string(error.what()).find("package metadata session") != std::string::npos;
            }
            require(rejected && build_entries == 0 && prepare_calls == 0 && execute_calls == 0, "invalid normal DB intent reached build/transaction");
            fixture.require_no_provenance_publication();
            std::cout << "S7D normal-finalizer database-world / early policy rejection / build0 PASS\n";
            continue;
        }
#endif
        fs::path root;
        {
            bool blocked = false;
            std::optional<ReviewedDevelSourceBuildExecutionResult> executed;
            std::optional<ReviewedDevelSourceBuildExecutionResult> direct_moved;
            const ReviewedDevelSourceBuildExecutionResult* observed = nullptr;
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
            std::optional<SourceBuildExecutionResult> normal_result;
            std::optional<ProductionSourceBuildInvocationResult> outer_result;
            if(normal) {
                publication_allocation::failures = 0;
                const ReviewedDevelExecutionSnapshot* normal_snapshot = nullptr;
                if(outer) {
                    PreparedProductionSourceBuildInvocation invocation;
                    invocation.database_paths = intent.database_paths;
                    ProductionSourceBuildWorkItem work;
                    work.request = intent.request;
                    work.required_targets = intent.required_targets;
                    work.artifact_lifecycle_intent = outer_set ? ArtifactLifecycleIntent::PackageBaseSet : ArtifactLifecycleIntent::SingularCompatibility;
                    invocation.work_items.push_back(work);
                    invocation.work_items.push_back(work); // Must remain NotAttempted after partial.
                    unsigned singular_calls = 0, set_calls = 0;
                    SourceInvocationExecutionTestHooks hooks;
                    hooks.singular = [&](const auto&, const auto&, const auto&) {
                        ++singular_calls;
                        return fixture.normal_execution(intent);
                    };
                    hooks.package_base = [&](const auto&, const auto&, const auto&) -> SourceBuildPackageBaseExecutionResult {
                        ++set_calls;
                        auto actual = fixture.normal_execution(intent, false, true);
                        return std::move(*actual.devel_execution);
                    };
                    set_source_invocation_execution_test_hooks(std::move(hooks));
                    outer_result.emplace(execute_prepared_source_build_invocation(std::move(invocation), AppConfig{}));
                    blocked = publication_allocation::blocked;
                    publication_allocation::blocked = false;
                    require(singular_calls == (outer_set ? 0U : 1U) && set_calls == (outer_set ? 1U : 0U), "outer invocation repeated or mixed execution");
                    require(!outer_result->is_success() && outer_result->command_exit_status() == 1 && outer_result->work_items.size() == 2, "outer partial became command success");
                    const auto& item = outer_result->work_items.front();
                    require(item.status == ProductionSourceBuildWorkItemStatus::AuthoritativePartial && !item.failure_exception && !item.failure_stage, "normal partial became exception failure");
                    require(outer_result->work_items.back().status == ProductionSourceBuildWorkItemStatus::NotAttempted, "partial executed the suffix");
                    require(item.devel_execution && item.devel_execution->owner, "outer discarded live owner");
                    require(prepare_calls == 1 && execute_calls == 1 && consumes == 1 && g_bridge_publication_entries == 1 && aborts == 0, "outer retried/finalized/published more than once");
                    const auto& dimensions = *item.devel_execution;
                    const auto expected_publication = mode == "publication-failure" ? Pub::Failed : mode == "publication-unknown" ? Pub::OutcomeUnknown
                                                                                                                                  : Pub::Complete;
                    require(dimensions.operation == Operation::Succeeded && dimensions.receipt == DevelSourceArtifactInstallReceipt::Complete &&
                                dimensions.proof == DevelSourceArtifactInstallProof::Complete && dimensions.publication == expected_publication &&
                                dimensions.cleanup == (mode == "cleanup-failure" ? Cleanup::Failed : Cleanup::Complete),
                            "outer partial dimensions changed");
                    normal_snapshot = &*item.devel_execution;
                    if(mode == "no-allocation") require(blocked && normal_snapshot->projection_failed, "outer allocation fault was missed");
                } else {
                    normal_result.emplace(fixture.normal_execution(intent));
                    normal_snapshot = &*normal_result->devel_execution;
                    blocked = publication_allocation::blocked;
                }
                publication_allocation::blocked = false;
                require(normal_snapshot && normal_snapshot->owner, "normal finalizer did not choose authoritative execution");
                observed = normal_snapshot->owner.get();
                root = observed->owned_root();
                if(mode == "environment") require(normal_result->production_outcome && normal_result->production_outcome->build_outcome == ProductionSourceBuildCommandOutcome::NotAttempted, "pre-build environment failure invented a build attempt");
                const bool expected_complete = mode == "install" || mode == "branch" || mode == "upgrade-explicit" || mode == "upgrade-dependency" || mode == "dependency-keeps-explicit" || mode == "registered-different";
                if(mode == "registered-different") require(aur_devel_update_test_stub::registered_call_count() == 1 && normal_result->devel_update_query && aur_update_basis(normal_result->devel_update_query->plan.entries.front()) == AurUpdateBasis::GitRevision, "registered same-version Git selection queried twice or lost basis");
                require((normal_snapshot->complete) == expected_complete, "normal partial outcome became complete success");
            } else
#endif
            {
                auto pin = fixture.execution_pin(mode == "overlay" || mode == "overlay-legacy");
                auto selected = prepare_reviewed_production_source_execution(
                    mode == "legacy" || mode == "overlay-legacy" ? ReviewedProductionExecutionChoice::Legacy : ReviewedProductionExecutionChoice::AuthoritativeDevel,
                    fixture.execution_checkout(), std::move(pin), ProductionReviewedSourceOutcome::InitialFullReview, std::nullopt, intent);
                require(!pin.valid(), "pin was copied instead of moved");
                if(mode == "legacy" || mode == "overlay-legacy") {
                    require(std::holds_alternative<ProductionArtifactSourceTree>(selected) && build_entries == 0 && prepare_calls == 0, "legacy path entered authoritative execution");
                    fixture.require_no_provenance_publication();
                    if(mode == "overlay-legacy") require(std::get<ProductionArtifactSourceTree>(selected).provenance().editor_overlay == ReviewedSourceEditorOverlayStatus::InvocationLocal, "legacy overlay lost");
                    std::cout << "S7C " << mode << " branch / authoritative-build0 / publication0 PASS\n";
                    continue;
                }
                if(mode == "needed" || mode == "split" || mode == "rmdeps" || mode == "only-if-updated" || mode == "overlay") {
                    require(std::holds_alternative<ReviewedDevelSourceBuildRejected>(selected) && build_entries == 0 && prepare_calls == 0, "unsupported intent entered S4/S5");
                    fixture.require_no_provenance_publication();
                    if(mode == "overlay") require(std::get<ReviewedDevelSourceBuildRejected>(selected).issue == Issue::EditorOverlay, "overlay admitted");
                    std::cout << "S7C intent " << mode << " rejected/build0/transaction0/publication0 PASS\n";
                    continue;
                }
                auto prepared = take_arm<PreparedReviewedDevelSourceBuildExecution>(selected, "bridge preparation failed");
                publication_allocation::failures = 0;
                auto returned = execute_reviewed_devel_source_build(std::move(prepared));
                require(returned.has_value(), "direct bridge result absent");
                executed.emplace(std::move(*returned));
                blocked = publication_allocation::blocked;
                publication_allocation::blocked = false;
                require(executed && executed->valid() && !prepared.valid() && !execute_reviewed_devel_source_build(std::move(prepared)), "bridge replay/move failed");
                root = executed->owned_root();
                direct_moved.emplace(std::move(*executed));
                require(direct_moved->valid() && !executed->valid(), "bridge result was copied");
                observed = &*direct_moved;
            }
            require(observed, "missing execution observation");
            const auto& moved = *observed;
            const auto* publication = moved.publication();
            const bool early = mode == "environment" || mode == "build-failure" || mode == "artifact-mismatch" || mode == "database-world" || mode == "new-dependency" || mode == "promotion";
            if(early) {
                require(!publication && prepare_calls == 0 && execute_calls == 0, "early failure reached S5/S6");
                if(mode == "build-failure") require(moved.build_failure() && !moved.build_completed(), "S4 failure lost");
                if(mode == "environment") require(moved.context_failure(), "S3 environment failure lost");
                if(mode == "new-dependency" || mode == "promotion") require(moved.issue() == Issue::InstallReasonUnsupported && moved.install_reason_directive() != InstallReasonDirective::Default, "reason was silently changed");
                fixture.require_no_provenance_publication();
            } else {
                require(publication && publication->valid() && moved.build_completed(), "S6 product missing");
                const auto& installation = publication->installation();
                const bool not_attempted = mode == "prepare-failure" || mode == "snapshot-failure";
                const bool no_proof = not_attempted || mode == "nonzero" || mode == "unknown" || mode == "no-post" || mode == "binding-failure";
                require(installation.operation() == (not_attempted ? Operation::NotAttempted : mode == "nonzero" ? Operation::Failed
                                                                                           : mode == "unknown"   ? Operation::OutcomeUnknown
                                                                                                                 : Operation::Succeeded),
                        "operation fact flattened");
                require(installation.source_context_cleanup() == Cleanup::Retained && fs::exists(root), "source context destroyed before final owner");
                if(no_proof) {
                    require(publication->state() == Pub::NotAttempted && installation.proof_state() != DevelSourceArtifactInstallProof::Complete, "incomplete S5 published");
                    fixture.require_no_provenance_publication();
                } else {
                    require(installation.receipt_state() == DevelSourceArtifactInstallReceipt::Complete && installation.proof_state() == DevelSourceArtifactInstallProof::Complete, "successful proof flattened");
                    const auto expected_pub = mode == "publication-failure" ? Pub::Failed : mode == "publication-unknown" ? Pub::OutcomeUnknown
                                                                                                                          : Pub::Complete;
                    require(publication->state() == expected_pub && installation.operation() == Operation::Succeeded, "publication partial outcome flattened install");
                    if(mode == "cleanup-failure" || mode == "retirement-failure") require(installation.privileged_cleanup().state == Cleanup::Failed && publication->state() == Pub::Complete, "cleanup failure lost");
                    if(expected_pub == Pub::Complete) {
                        const auto read = read_devel_build_provenance(installation.proof()->built_proof().package_base());
                        const auto& loaded = require_arm<DevelBuildProvenanceStoreLoaded>(read, "bridge readback failed");
                        require(loaded.provenance.artifact() == *expected && loaded.provenance.installed_binding() == installation.proof()->installed_binding() &&
                                    *loaded.provenance.actual_built_revision().revision().value().git_commit() == actual_oid && actual_oid == upstream.oid() &&
                                    *loaded.provenance.reviewed_recipe_revision().value().git_commit() == fixture.recipe_oid(),
                                "publication did not derive from actual S4/S5 proof");
                        auto session = PackageMetadataSession::open({"/", db});
                        const auto metadata = session.query_installed_package(fixture.package_name());
                        require(require_arm<InstalledPackageMetadata>(metadata, "installed reason readback failed").reason == (dependency_reason ? InstalledPackageReason::Dependency : InstalledPackageReason::Explicit), "default reason semantics changed");
                    }
                }
                require(execute_calls == (not_attempted ? 0U : 1U), "transaction repeated");
                if(mode == "unknown") require(consumes == 0 && aborts == 0, "unknown transaction retried/consumed/aborted");
                if(mode == "no-allocation") require(blocked && (normal ? publication_allocation::failures > 0 : publication_allocation::failures == 0), "allocation projection probe missed boundary");
            }
            require(build_entries == (mode == "environment" ? 0U : 1U), "multiple build paths executed");
        }
        require(root.empty() || !fs::exists(root), "last bridge owner left source context behind");
        set_reviewed_devel_source_build_execution_test_hooks({});
        set_evaluated_devel_source_artifact_transport_test_hooks({});
        set_source_artifact_install_trusted_exec_test_hook({});
        set_source_artifact_install_trusted_state_test_hook({});
        set_installed_record_observation_test_hooks({});
        reset_xdg_generation_store_test_hooks();
        std::cout << (normal ? "S7D normal-finalizer " : "S7C integrated ") << case_name << " / one-shot / lifetime / lossless PASS\n";
    }
}
#endif

std::vector<fs::path> context_root_inventory() {
    std::vector<fs::path> roots;
    for(const auto& root : g_fixture_context_roots) {
        struct stat status{};
        if(::lstat(root.c_str(), &status) == 0) {
            roots.push_back(root);
        }
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}


void test_preprepare_selection() {
    using Process = EvaluatedDevelSourceBuildProcess;
    UpstreamGitFixture upstream("selection");
    for(const std::string kind : {"default", "branch", "multi-arch", "supplemental"}) {
        ArchitectureFixture architecture;
        if(kind == "multi-arch") {
            architecture.declared = {"i686", "x86_64"};
            architecture.reviewed = std::vector<std::string>{"x86_64", "i686"};
        }
        architecture.recipe_suffix =
            "prepare() { touch \"$HOME/prepare-ran\"; }\n"
            "build() { touch \"$HOME/build-ran\"; }\n"
            "package() { touch \"$HOME/package-ran\"; }\n";
        ReviewedBuildFixture fixture("selection-" + kind, upstream, RecipeShape::Valid,
                                     false, kind == "branch", kind == "supplemental", true, architecture);
        auto context = fixture.make_context();
        const auto root = context.owned_root();
        const auto snapshot = context.snapshot_identity();
        auto environment = fixture.make_environment(context);
        unsigned initial = 0;
        set_evaluated_devel_source_build_process_test_hook(
            [&](const auto& invocation, const auto& policy, Process process) {
                require(process == Process::InitialPrintSrcinfo &&
                            invocation.arguments == std::vector<std::string>{"--printsrcinfo"} &&
                            invocation.executable_fd && invocation.working_directory_fd,
                        "Selection started a later phase or lost retained execution");
                ++initial;
                return capture_bounded_explicit_process_output_raw(invocation, policy);
            });
        auto result = select_evaluated_devel_source(std::move(context), std::move(environment));
        set_evaluated_devel_source_build_process_test_hook({});
        auto selection = take_arm<EvaluatedDevelSourceSelection>(result, "Matching evaluation did not select source");
        require(!context.valid() && selection.valid() && initial == 1 &&
                    selection.git_source().source_location() == upstream.url() &&
                    selection.git_source().selector().kind() == (kind == "branch" ? VcsSelectorKind::Branch : VcsSelectorKind::DefaultHead) &&
                    (kind != "branch" || *selection.git_source().selector().value() == "main") &&
                    selection.snapshot_identity() == snapshot &&
                    selection.source_count() == (kind == "supplemental" ? 3U : 1U) &&
                    selection.tracked_local_source_count() == (kind == "supplemental" ? 2U : 0U) &&
                    selection.source_declaration().raw_value.starts_with(fixture.package_name() + "::git+"),
                "Selection lost identity, declaration, selector or invocation ownership");
        require(fs::is_empty(root / "srcdest") && fs::is_empty(root / "pkgdest") &&
                    !fs::exists(fixture.home() / "prepare-ran") && !fs::exists(fixture.home() / "build-ran") &&
                    !fs::exists(fixture.home() / "package-ran"),
                "Selection prepared, acquired or built source");
        auto moved = std::move(selection);
        require(!selection.valid() && moved.valid(), "Selection move duplicated authority");
        bool rejected = false;
        try {
            static_cast<void>(selection.git_source());
        } catch(const std::logic_error&) {
            rejected = true;
        }
        require(rejected, "Moved selection still exposed authority");
        const auto invalid = resume_evaluated_devel_source(std::move(selection));
        require(require_arm<EvaluatedDevelSourceBuildFailure>(invalid, "Moved selection resumed").reason ==
                        EvaluatedDevelSourceBuildFailureReason::InvalidBuildContext &&
                    fs::exists(root),
                "Moved selection consumed the live owner");
        require(std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(moved.cleanup()) &&
                    !moved.valid() && !fs::exists(root),
                "Selection cleanup did not consume its own context");
        fixture.require_no_provenance_publication();
        std::cout << "S564 4A0 selection " << kind << " PASS\n";
    }
}

void test_selection_projection_rejection() {
    using Reason = EvaluatedDevelSourceBuildFailureReason;
    UpstreamGitFixture upstream("selection-negative");
    const std::vector<std::pair<std::string, std::string>> mutations{
        {"conditional-url", "if (( PRINTSRCINFO )); then source[0]='other::git+https://other.invalid/repo'; fi\n"},
        {"addition", "source+=('extra.patch'); sha256sums+=('SKIP')\n"},
        {"deletion", "unset 'source[1]' 'sha256sums[1]'; source=(\"${source[@]}\"); sha256sums=(\"${sha256sums[@]}\")\n"},
        {"replacement", "source[1]='config.toml'\n"},
        {"selector", "source[0]+=\"#branch=changed\"\n"},
        {"destination", "source[0]=\"other::${source[0]#*::}\"\n"},
        {"architecture", "arch=('x86_64')\n"},
        {"malformed", "if (( PRINTSRCINFO )); then printf 'not metadata\\n'; exit 0; fi\n"},
        {"working-drift", "if (( PRINTSRCINFO )); then printf '\\n# drift\\n' >> \"$startdir/PKGBUILD\"; fi\n"},
        {"local-drift", "if (( PRINTSRCINFO )); then chmod u+w \"$startdir/fix.patch\"; printf 'drift' >> \"$startdir/fix.patch\"; fi\n"},
    };
    for(const auto& [kind, mutation] : mutations) {
        ArchitectureFixture architecture;
        architecture.recipe_suffix = mutation;
        ReviewedBuildFixture fixture("selection-" + kind, upstream, RecipeShape::Valid,
                                     false, false, true, true, architecture);
        auto context = fixture.make_context();
        const auto root = context.owned_root();
        auto environment = fixture.make_environment(context);
        unsigned initial = 0;
        set_evaluated_devel_source_build_process_test_hook([&](const auto& invocation, const auto& policy, auto process) {
            require(process == EvaluatedDevelSourceBuildProcess::InitialPrintSrcinfo, "Rejected selection entered later phase");
            ++initial;
            return capture_bounded_explicit_process_output_raw(invocation, policy);
        });
        auto result = select_evaluated_devel_source(std::move(context), std::move(environment));
        set_evaluated_devel_source_build_process_test_hook({});
        const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(result, "Selection accepted source/recipe drift");
        const auto reason = kind == "malformed" ? Reason::EvaluatedSourceFailure : kind.ends_with("-drift") ? Reason::WorkingRecipeFailure
                                                                                                            : Reason::RawEvaluatedSourceMismatch;
        require(failure.reason == reason && initial == 1 && !fs::exists(root), "Selection rejection changed typed cause or cleanup");
        std::cout << "S564 4A0 reject " << kind << " PASS\n";
    }
}

void test_selection_resume_and_environment() {
    using Process = EvaluatedDevelSourceBuildProcess;
    UpstreamGitFixture upstream("selection-resume");
    for(const bool staged : {false, true}) {
        ReviewedBuildFixture fixture(staged ? "staged-selection" : "wrapper-selection", upstream, RecipeShape::Valid, true);
        auto context = fixture.make_context();
        auto environment = fixture.make_environment(context);
        ScopedEnvironmentVariable initial_environment("MOGUET_TEST_SELECTION_VALUE", "initial");
        std::optional<ScopedEnvironmentVariable> changed_environment;
        std::vector<Process> phases;
        std::vector<std::string> initial_effective;
        struct stat initial_cwd{};
        unsigned foundation_events = 0;
        set_evaluated_devel_source_build_test_hook([&](auto event, const auto& root, const auto&) {
            if(event == EvaluatedDevelSourceBuildTestEvent::AfterInitialSourceSelection) {
                ++foundation_events;
                require(phases == std::vector<Process>{Process::InitialPrintSrcinfo} &&
                            fs::is_empty(root / "srcdest") && fs::is_empty(root / "pkgdest"),
                        "Foundation was not before preparation");
                changed_environment.emplace("MOGUET_TEST_SELECTION_VALUE", "changed");
            }
        });
        set_evaluated_devel_source_build_process_test_hook([&](const auto& invocation, const auto& policy, Process process) {
            if(process == Process::InitialPrintSrcinfo || process == Process::SourcePreparation ||
               process == Process::PreparedPrintSrcinfo || process == Process::PreparedPackagelist || process == Process::PackageBuild) {
                struct stat current{};
                require(invocation.working_directory_fd && ::fstat(*invocation.working_directory_fd, &current) == 0,
                        "Lost retained makepkg cwd");
                if(phases.empty()) {
                    initial_effective = invocation.environment;
                    initial_cwd = current;
                }
                require(invocation.environment == initial_effective && current.st_dev == initial_cwd.st_dev &&
                            current.st_ino == initial_cwd.st_ino,
                        "Resume changed effective environment or working recipe");
                phases.push_back(process);
            }
            return capture_bounded_explicit_process_output_raw(invocation, policy);
        });
        auto result = [&]() {
            if(!staged) return build_evaluated_devel_source(std::move(context), std::move(environment));
            auto selected = select_evaluated_devel_source(std::move(context), std::move(environment));
            auto selection = take_arm<EvaluatedDevelSourceSelection>(selected, "Staged evaluation failed");
            require(phases.size() == 1 && foundation_events == 0, "Selection producer ran prepare");
            return resume_evaluated_devel_source(std::move(selection));
        }();
        set_evaluated_devel_source_build_test_hook({});
        set_evaluated_devel_source_build_process_test_hook({});
        auto proof = take_arm<EvaluatedDevelSourceBuildProof>(result, "Resume did not complete S4");
        require(phases == std::vector<Process>{Process::InitialPrintSrcinfo, Process::SourcePreparation,
                                               Process::PreparedPrintSrcinfo, Process::PreparedPackagelist, Process::PackageBuild} &&
                    foundation_events == 1,
                "Initial evaluation repeated or S4 phase order changed");
        require(archive_member(proof.artifact().path(), "usr/share/" + fixture.package_name() + "/payload.txt") ==
                    "revision-one\nprepared\n",
                "Resume lost prepare transformation");
        cleanup_proof(proof);
        std::cout << "S564 4A0 " << (staged ? "resume" : "wrapper") << " phase/env PASS\n";
    }
}

void test_selection_lifetime_and_drift() {
    using Reason = EvaluatedDevelSourceBuildFailureReason;
    UpstreamGitFixture upstream("selection-lifetime");
    for(const std::string kind : {"abandon", "working-bytes", "working-name", "snapshot", "pkgdest", "cleanup"}) {
        ReviewedBuildFixture fixture("selection-" + kind, upstream);
        auto context = fixture.make_context();
        const auto root = context.owned_root();
        struct stat root_identity{};
        require(::lstat(root.c_str(), &root_identity) == 0, "Missing selection root");
        auto environment = fixture.make_environment(context);
        {
            auto result = select_evaluated_devel_source(std::move(context), std::move(environment));
            auto selection = take_arm<EvaluatedDevelSourceSelection>(result, "Selection lifetime fixture failed");
            if(kind != "abandon") {
                const auto working = root / "build/.moguet-evaluated-recipe";
                if(kind == "working-bytes") write_file(working / "PKGBUILD", "changed\n");
                if(kind == "working-name") {
                    fs::rename(working, root / "build/renamed");
                    fs::create_directory(working);
                }
                if(kind == "snapshot") {
                    fs::permissions(root / "recipe/PKGBUILD", fs::perms::owner_write, fs::perm_options::add);
                    write_file(root / "recipe/PKGBUILD", "changed\n");
                }
                if(kind == "pkgdest") write_file(root / "pkgdest/unproven", "not an archive");
                if(kind == "cleanup") {
                    set_invocation_owned_source_build_context_test_hook([](auto event, const auto& owned_root) {
                        if(event == InvocationOwnedSourceBuildContextTestEvent::BeforeCleanup) write_file(owned_root / "unexpected", "retain");
                    });
                    const auto cleanup = selection.cleanup();
                    set_invocation_owned_source_build_context_test_hook({});
                    require(std::holds_alternative<InvocationOwnedSourceBuildContextFailure>(cleanup) && !selection.valid(),
                            "Failed cleanup left selection reusable");
                } else {
                    set_evaluated_devel_source_build_process_test_hook([](const auto&, const auto&, auto) -> BoundedCapturedProcessResult {
                        throw std::runtime_error("Drifted selection started a process");
                    });
                    const auto resumed = resume_evaluated_devel_source(std::move(selection));
                    set_evaluated_devel_source_build_process_test_hook({});
                    const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(resumed, "Drifted selection resumed");
                    const auto expected = kind == "working-bytes" ? Reason::WorkingRecipeFailure : kind == "working-name" ? Reason::SourceContainmentFailure
                                                                                               : kind == "snapshot"       ? Reason::ContextRevalidationFailure
                                                                                                                          : Reason::ArtifactInventoryMismatch;
                    require(failure.reason == expected, "Drift rejected at wrong boundary");
                }
            }
        }
        if(kind == "abandon") require(!fs::exists(root), "Abandoned selection leaked its context");
        if(fs::exists(root)) cleanup_retained_fixture(root, root_identity);
        std::cout << "S564 4A0 lifetime " << kind << " PASS\n";
    }
    // Same package/source values do not make another context's environment valid.
    ReviewedBuildFixture first_fixture("selection-context-a", upstream);
    auto first = first_fixture.make_context();
    const auto first_root = first.owned_root();
    ReviewedBuildFixture second_fixture("selection-context-b", upstream);
    auto second = second_fixture.make_context();
    auto wrong_environment = second_fixture.make_environment(second);
    const auto result = select_evaluated_devel_source(std::move(first), std::move(wrong_environment));
    require(require_arm<EvaluatedDevelSourceBuildFailure>(result, "Cross-context environment selected source").reason ==
                    Reason::EnvironmentLineageMismatch &&
                !fs::exists(first_root) && second.valid(),
            "Selection mixed context lifetimes");
    require(std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(second.cleanup()), "Other context cleanup failed");
}

void test_selection_resume_failure_stage() {
    UpstreamGitFixture upstream("selection-stage");
    ReviewedBuildFixture fixture("selection-stage", upstream);
    auto context = fixture.make_context();
    const auto root = context.owned_root();
    struct stat root_identity{};
    require(::lstat(root.c_str(), &root_identity) == 0, "Missing stage fixture root");
    auto environment = fixture.make_environment(context);
    bool injected = false;
    set_evaluated_devel_source_build_test_hook([&](auto event, const auto&, const auto&) {
        if(event == EvaluatedDevelSourceBuildTestEvent::AfterSourcePreparation) {
            injected = true;
            fs::permissions(root / "recipe/PKGBUILD", fs::perms::owner_write, fs::perm_options::add);
            write_file(root / "recipe/PKGBUILD", "changed\n");
        }
    });
    const auto result = build_evaluated_devel_source(std::move(context), std::move(environment));
    set_evaluated_devel_source_build_test_hook({});
    const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(result, "Context drift produced S4");
    require(injected && failure.stage == EvaluatedDevelSourceBuildStage::DynamicVersion &&
                failure.reason == EvaluatedDevelSourceBuildFailureReason::ContextRevalidationFailure,
            "Resumed context failure lost its original phase");
    if(fs::exists(root)) cleanup_retained_fixture(root, root_identity);
    std::cout << "S564 4A0 resumed context failure stage PASS\n";
}

void test_selection_explicit_branch_cancellation() {
    using Process = EvaluatedDevelSourceBuildProcess;
    UpstreamGitFixture upstream("selection-branch-cancel");
    ArchitectureFixture architecture;
    architecture.recipe_suffix =
        "prepare() { touch \"$HOME/prepare-ran\"; }\n"
        "build() { touch \"$HOME/build-ran\"; }\n"
        "package() { touch \"$HOME/package-ran\"; }\n";
    ReviewedBuildFixture fixture("selection-branch-cancel", upstream, RecipeShape::Valid,
                                 false, true, false, true, architecture);
    auto context = fixture.make_context();
    const auto root = context.owned_root();
    auto environment = fixture.make_environment(context);
    unsigned initial = 0;
    unsigned branch_calls = 0;
    std::optional<BoundedCapturedProcessResult> observed;
    set_evaluated_devel_source_build_process_test_hook([&](const auto& invocation, const auto& policy, Process process) {
        require(process == Process::InitialPrintSrcinfo, "Cancelled branch entered prepare/build");
        ++initial;
        return capture_bounded_explicit_process_output_raw(invocation, policy);
    });
    set_exact_git_branch_validation_process_test_hook([&](const auto& invocation, const auto& policy) {
        const auto& arguments = invocation.arguments;
        require(invocation.executable == "/usr/bin/git" && invocation.working_directory_fd &&
                    invocation.standard_input_fd && arguments.size() >= 3 &&
                    std::vector<std::string>(arguments.end() - 3, arguments.end()) ==
                        std::vector<std::string>{"check-ref-format", "--branch", "main"},
                "Cancellation fixture bypassed explicit branch helper");
        ++branch_calls;
        // Exercise real parent signal capture and a child that exits successfully.
        auto injected = invocation;
        injected.executable = "/bin/sh";
        injected.arguments = {"-c", "trap 'printf \"main\\n\"; exit 0' INT; kill -INT \"$PPID\"; while :; do :; done"};
        observed = capture_bounded_explicit_process_output_raw(injected, policy);
        return *observed;
    });
    const auto result = select_evaluated_devel_source(std::move(context), std::move(environment));
    set_exact_git_branch_validation_process_test_hook({});
    set_evaluated_devel_source_build_process_test_hook({});
    const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(result, "Cancelled branch minted selection");
    require(initial == 1 && branch_calls == 1 && observed && observed->output == "main\n" &&
                observed->cancellation_signal == SIGINT &&
                require_arm<BoundedProcessExited>(observed->outcome, "Cancellation child did not exit").exit_code == 0,
            "Explicit branch fixture did not produce SIGINT + exit0 + valid stdout");
    require(failure.stage == EvaluatedDevelSourceBuildStage::EvaluatedSource &&
                failure.reason == EvaluatedDevelSourceBuildFailureReason::EvaluatedSourceFailure &&
                failure.cancellation_signal == SIGINT && failure.process_outcome == observed->outcome,
            "Branch cancellation lost typed signal, child outcome or phase");
    require(!fs::exists(root) && !fs::exists(fixture.home() / "prepare-ran") &&
                !fs::exists(fixture.home() / "build-ran") && !fs::exists(fixture.home() / "package-ran"),
            "Cancelled branch ran prepare/build/package or retained context");
    fixture.require_no_provenance_publication();
    std::cout << "S564 FG-F1 explicit branch SIGINT / exit0 / selection0 / prepare0 / build0 / package0 PASS\n";
}

void test_selection_process_failures() {
    using Process = EvaluatedDevelSourceBuildProcess;
    UpstreamGitFixture upstream("selection-process");
    for(const std::string kind : {"launch", "nonzero", "timeout", "signal", "capture", "io", "cancel"}) {
        ArchitectureFixture architecture;
        if(kind == "cancel") architecture.recipe_suffix =
                                 "if (( PRINTSRCINFO )); then trap 'cat \"$startdir/.SRCINFO\"; exit 0' INT; "
                                 "kill -INT \"$PPID\"; while :; do :; done; fi\n";
        ReviewedBuildFixture fixture("selection-" + kind, upstream, RecipeShape::Valid, false, false, false, true, architecture);
        auto context = fixture.make_context();
        const auto root = context.owned_root();
        auto environment = fixture.make_environment(context);
        std::optional<BoundedCapturedProcessResult> observed;
        set_evaluated_devel_source_build_process_test_hook([&](const auto& invocation, const auto& policy, Process process) {
            require(process == Process::InitialPrintSrcinfo && invocation.executable_fd &&
                        invocation.arguments == std::vector<std::string>{"--printsrcinfo"},
                    "Failure fixture skipped actual initial boundary");
            if(kind == "io")
                observed = BoundedCapturedProcessResult{"", BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::Poll, EIO}};
            else if(kind == "cancel")
                observed = capture_bounded_explicit_process_output_raw(invocation, policy);
            else {
                auto injected = invocation;
                injected.executable_fd.reset();
                injected.executable = kind == "launch" ? "/moguet-missing-executable" : "/bin/sh";
                injected.arguments = {"-c", kind == "nonzero" ? "exit 23" : kind == "signal" ? "kill -TERM $$"
                                                                        : kind == "capture"  ? "printf '01234567890123456789'"
                                                                                             : "while :; do :; done"};
                auto bounded = policy;
                bounded.hard_timeout = std::chrono::milliseconds(100);
                bounded.termination_grace = std::chrono::milliseconds(20);
                if(kind == "capture") bounded.stdout_capture_limit = 4;
                observed = capture_bounded_explicit_process_output_raw(injected, bounded);
            }
            return *observed;
        });
        const auto result = select_evaluated_devel_source(std::move(context), std::move(environment));
        set_evaluated_devel_source_build_process_test_hook({});
        const auto& failure = require_arm<EvaluatedDevelSourceBuildFailure>(result, "Initial process failure selected source");
        require(observed && failure.process == Process::InitialPrintSrcinfo && failure.process_outcome == observed->outcome &&
                    failure.cancellation_signal == observed->cancellation_signal && !fs::exists(root),
                "Process failure lost outcome/cancel or left context");
        const bool expected_outcome = kind == "launch" ? std::holds_alternative<BoundedProcessLaunchOrSetupFailure>(observed->outcome) : kind == "nonzero" ? std::holds_alternative<BoundedProcessExited>(observed->outcome) && std::get<BoundedProcessExited>(observed->outcome).exit_code == 23
                                                                                                                                     : kind == "timeout"   ? std::holds_alternative<BoundedProcessTimedOut>(observed->outcome)
                                                                                                                                     : kind == "signal"    ? std::holds_alternative<BoundedProcessSignaled>(observed->outcome) && std::get<BoundedProcessSignaled>(observed->outcome).signal_number == SIGTERM
                                                                                                                                     : kind == "capture"   ? std::holds_alternative<BoundedProcessCaptureLimitExceeded>(observed->outcome)
                                                                                                                                     : kind == "io"        ? std::holds_alternative<BoundedProcessIoOrWaitFailure>(observed->outcome)
                                                                                                                                                           : std::holds_alternative<BoundedProcessExited>(observed->outcome);
        require(expected_outcome, "Process fixture did not reach its expected failure boundary");
        if(kind == "cancel") {
            require(observed->cancellation_signal == SIGINT && std::get<BoundedProcessExited>(observed->outcome).exit_code == 0 &&
                        parse_srcinfo_source_metadata(observed->output).is_success(),
                    "Cancel fixture did not produce valid metadata plus exit0");
        }
        std::cout << "S564 4A0 process " << kind << " PASS\n";
    }
}

#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
void test_pinned_closure_allocation_cleanup() {
    UpstreamGitFixture upstream("closure-allocation");
    for(const bool cleanup_failure : {false, true}) {
        const std::string kind = cleanup_failure ? "allocation-cleanup-failure" : "allocation-cleanup-success";
        ReviewedBuildFixture fixture(kind, upstream);
        auto context = fixture.make_context();
        const auto root = context.owned_root();
        struct stat root_identity{};
        require(::lstat(root.c_str(), &root_identity) == 0, "Missing selection root");
        auto environment = fixture.make_environment(context);
        unsigned process_calls = 0, object_events = 0, cleanup_attempts = 0;
        {
            auto selected = select_evaluated_devel_source(std::move(context), std::move(environment));
            auto selection = take_arm<EvaluatedDevelSourceSelection>(selected, "Allocation fixture selection failed");
            require(selection.valid(), "Allocation fixture needs live selection");
            set_evaluated_devel_source_build_process_test_hook([&](const auto&, const auto&, auto) -> BoundedCapturedProcessResult {
                ++process_calls;
                throw std::runtime_error("Allocation failure ran makepkg");
            });
            set_invocation_owned_source_build_context_test_hook([&](auto event, const auto& owned_root) {
                if(event != InvocationOwnedSourceBuildContextTestEvent::BeforeCleanup) return;
                require(owned_root == root, "Cleanup targeted another context");
                ++cleanup_attempts;
                if(cleanup_failure) write_file(root / "unexpected", "retain");
            });
            PinnedClosureTestHooks hooks;
            hooks.fail_next_backing_allocation = true;
            hooks.process = [&](const auto&, const auto&) -> BoundedCapturedProcessResult {
                ++process_calls;
                throw std::runtime_error("Allocation failure launched Git");
            };
            hooks.event = [&](auto, const auto&) { ++object_events; };
            hooks.before_remove = [&](const auto&) { ++object_events; };
            set_pinned_closure_test_hooks(std::move(hooks));
            const auto result = acquire_pinned_submodule_closure(std::move(selection));
            const auto& failure = require_arm<PinnedClosureFailure>(result, "Allocation failure minted closure owner");
            require(failure.stage == PinnedClosureStage::Input && failure.reason == PinnedClosureFailureReason::ResourceLimitExceeded &&
                        !failure.process && !failure.error_number && !selection.valid(),
                    "Allocation failure lost primary or retained input authority");
            require(!failure.cleanup.objects && !failure.abandoned_root && object_events == 0 && process_calls == 0,
                    "Allocation failure invented object cleanup or started work");
            require(cleanup_attempts == 1, "Selection cleanup missing or retried");
            if(cleanup_failure) {
                require(failure.cleanup.selection && !failure.cleanup.succeeded() && fs::exists(root / "unexpected"),
                        "Allocation failure lost typed selection cleanup consequence or residue");
                const auto& consequence = *failure.cleanup.selection;
                require(consequence.stage == InvocationOwnedSourceBuildContextStage::Cleanup &&
                            consequence.reason == InvocationOwnedSourceBuildContextFailureReason::ConcurrentReplacement &&
                            consequence.relative_path.empty() && !consequence.system_error && !consequence.binding_failure &&
                            !consequence.git_failure && !consequence.review_failure && !consequence.checkout_failure &&
                            !consequence.diagnostic && !consequence.construction_cleanup_failure,
                        "Selection cleanup consequence fields changed");
            } else {
                require(!failure.cleanup.selection && failure.cleanup.succeeded() && !fs::exists(root),
                        "Allocation failure did not clean selection");
            }
        }
        require(cleanup_attempts == 1 && process_calls == 0 && object_events == 0 && fs::exists(root) == cleanup_failure,
                "Destruction retried cleanup or changed residue");
        set_pinned_closure_test_hooks({});
        set_evaluated_devel_source_build_process_test_hook({});
        set_invocation_owned_source_build_context_test_hook({});
        if(cleanup_failure) cleanup_retained_fixture(root, root_identity);
        fixture.require_no_provenance_publication();
        std::cout << "S564 4A " << kind << " PASS\n"
                  << std::flush;
    }
}

std::string module_declaration(const std::string& name, const std::string& path, const std::string& url) {
    return "[submodule \"" + name + "\"]\n\tpath = " + path + "\n\turl = " + url + "\n";
}
void test_pinned_closure() {
    using Closure = InvocationOwnedPinnedSubmoduleClosure;
    using Reason = PinnedClosureFailureReason;
    using Stage = PinnedClosureStage;
    static_assert(!std::is_default_constructible_v<Closure> && !std::is_copy_constructible_v<Closure> && std::is_move_constructible_v<Closure>);
    static_assert(!std::is_constructible_v<Closure, VcsSourceIdentity, std::string>);
    static_assert(!std::is_constructible_v<Closure, DevelBuildProvenance>);
    static_assert(!std::is_invocable_v<decltype(acquire_pinned_submodule_closure), VcsSourceIdentity>);
    static_assert(!std::is_invocable_v<decltype(acquire_pinned_submodule_closure), EvaluatedDevelSourceBuildProof>);
    unsigned completed = 0;
    for(const std::string kind : {"single", "nested", "siblings", "freeze", "parent-move", "sha256", "branch",
                                  "missing-declaration", "extra-declaration", "duplicate-name", "duplicate-path", "mismatch", "malformed",
                                  "file", "ssh", "scp", "ext", "git", "http", "relative", "branch-key", "merge", "rebase", "none", "custom",
                                  "absolute", "dotdot", "dot", "empty-component", "overlong", "component", "missing-object", "blob", "tree",
                                  "depth-bound", "edge-bound", "declaration-bound", "aggregate-bound", "records-bound", "metadata-bound", "process-bound",
                                  "launch", "nonzero", "timeout", "signal", "cancel", "overflow", "config", "alternates", "cleanup",
                                  "tag", "wrong-format", "missing-modules", "symlink-modules", "gitlink-modules", "root-unavailable",
                                  "child-cancel", "io", "cleanup-primary", "duplicate-tree", "tree-framing", "tree-mode", "tree-oid",
                                  "limit-slack", "overlap", "missing-path", "missing-url", "duplicate-key", "quoted", "moved-input",
                                  "duplicate-tree-record", "read-cleanup-process", "read-cleanup-exception",
                                  "init-launch", "init-nonzero", "fetch-launch", "fetch-timeout", "fetch-signal", "fetch-overflow", "fetch-io", "fetch-nonzero",
                                  "observation-duplicate", "observation-wrong-ref", "observation-malformed"}) {
        const auto format = kind == "sha256" ? GitObjectFormat::Sha256 : GitObjectFormat::Sha1;
        UpstreamGitFixture child("closure-child-" + kind, format);
        UpstreamGitFixture root("closure-root-" + kind, format);
        const std::string child_pin = child.oid();
        std::unique_ptr<UpstreamGitFixture> nested;
        std::string declaration = module_declaration("logical/A", "deps/a", child.url());
        std::vector<std::pair<std::string, std::string>> pins{{"deps/a", child_pin}};
        std::optional<Reason> expected;
        if(kind == "nested") {
            nested = std::make_unique<UpstreamGitFixture>("closure-leaf");
            child.pin_tree(module_declaration("leaf-name", "nested/b", nested->url()), {{"nested/b", nested->oid()}});
            pins.front().second = child.oid();
        }
        if(kind == "siblings") {
            declaration += module_declaration("other-name", "deps/b", child.url());
            pins.push_back({"deps/b", child_pin});
        }
        if(kind == "missing-declaration") {
            declaration.clear();
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "extra-declaration") {
            declaration += module_declaration("extra", "extra", child.url());
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "duplicate-name") {
            declaration += module_declaration("logical/A", "deps/b", child.url());
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "duplicate-path") {
            declaration += module_declaration("other", "deps/a", child.url());
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "mismatch") {
            declaration = module_declaration("logical/A", "different", child.url());
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "malformed") {
            declaration = "[broken";
            expected = Reason::MalformedDeclaration;
        }
        const std::map<std::string, std::string> transports{{"file", "file:///tmp/repo"}, {"ssh", "ssh://fixture.invalid/repo"}, {"scp", "git@fixture.invalid:repo"}, {"ext", "ext::false"}, {"git", "git://fixture.invalid/repo"}, {"http", "http://fixture.invalid/repo"}, {"relative", "../repo"}};
        if(transports.contains(kind)) {
            declaration = module_declaration("logical/A", "deps/a", transports.at(kind));
            expected = Reason::UnsupportedTransport;
        }
        if(kind == "branch-key") {
            declaration += "branch = main\n";
            expected = Reason::UnsupportedUpdatePolicy;
        }
        if(kind == "merge" || kind == "rebase" || kind == "none" || kind == "custom") {
            declaration += "update = " + (kind == "custom" ? "!false" : kind) + "\n";
            expected = Reason::UnsupportedUpdatePolicy;
        }
        const std::map<std::string, std::string> paths{{"absolute", "/deps/a"}, {"dotdot", "deps/../a"}, {"dot", "deps/./a"}, {"empty-component", "deps//a"}, {"overlong", std::string(4097, 'x')}, {"component", std::string(256, 'x')}};
        if(paths.contains(kind)) {
            declaration = module_declaration("logical/A", paths.at(kind), child.url());
            expected = Reason::UnsafePath;
        }
        if(kind == "overlap") {
            declaration += module_declaration("other", "deps/a/sub", child.url());
            expected = Reason::UnsafePath;
        }
        if(kind == "missing-path") {
            declaration = "[submodule \"a\"]\nurl = " + child.url() + "\n";
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "missing-url") {
            declaration = "[submodule \"a\"]\npath = deps/a\n";
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "duplicate-key") {
            declaration += "path = deps/a\n";
            expected = Reason::MalformedDeclaration;
        }
        if(kind == "quoted") declaration = "# declaration\n[submodule \"logical/A\"]\npath = \"deps/a\" # same path\nurl = \"" + child.url() + "\"\nupdate = checkout\n";
        if(kind == "tag") {
            pins.front().second = child.annotated_tag();
            expected = Reason::UnexpectedObjectType;
        }
        if(kind == "missing-object") {
            pins.front().second = std::string(40, '1');
            expected = Reason::PinnedObjectUnavailable;
        }
        if(kind == "blob" || kind == "tree") {
            pins.front().second = child.object_oid(kind == "blob" ? "HEAD:payload.txt" : "HEAD^{tree}");
            expected = Reason::UnexpectedObjectType;
        }
        const auto declaration_check = check_pinned_declaration_fixture(declaration);
        if(expected && *expected != Reason::PinnedObjectUnavailable && *expected != Reason::UnexpectedObjectType && kind != "missing-declaration" && kind != "extra-declaration" && kind != "mismatch")
            require(declaration_check && declaration_check->reason == *expected, "Pure declaration refusal mismatch: " + kind);
        auto root_pin = root.pin_tree(declaration, pins);
        if(kind == "missing-modules") {
            root.remove_modules();
            root_pin = root.oid();
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "symlink-modules" || kind == "gitlink-modules") {
            root.modules_mode(kind == "symlink-modules" ? "120000" : "160000");
            root_pin = root.oid();
            expected = Reason::DeclarationMismatch;
        }
        if(kind == "parent-move") {
            child.commit("child-second\n");
            pins.front().second = child.oid();
            root_pin = root.pin_tree(declaration, pins);
        }
        // Every success remains parent-pinned even though the child's remote
        // default branch has already advanced before any observation begins.
        const auto pinned_child = pins.front().second;
        child.commit("child-remote-only-advance\n");
        ReviewedBuildFixture fixture("closure-" + kind, root, RecipeShape::Valid, false, kind == "branch");
        require(fixture.recipe_oid() != root_pin && root_pin != pinned_child && fixture.recipe_oid() != pinned_child, "Rr/X/SA were conflated");
        auto context = fixture.make_context();
        auto environment = fixture.make_environment(context);
        unsigned phases = 0;
        set_evaluated_devel_source_build_process_test_hook([&](const auto& invocation, const auto& policy, auto process) {
            require(process == EvaluatedDevelSourceBuildProcess::InitialPrintSrcinfo, "4A ran prepare/build");
            ++phases;
            return capture_bounded_explicit_process_output_raw(invocation, policy);
        });
        auto selected = select_evaluated_devel_source(std::move(context), std::move(environment));
        set_evaluated_devel_source_build_process_test_hook({});
        auto selection = take_arm<EvaluatedDevelSourceSelection>(selected, "4A0 selection failed");
        const auto snapshot = selection.snapshot_identity();
        if(kind == "moved-input") {
            auto keeper = std::move(selection);
            unsigned calls = 0;
            PinnedClosureTestHooks forbidden;
            forbidden.process = [&](const auto&, const auto&) -> BoundedCapturedProcessResult { ++calls; throw std::runtime_error("moved input launched Git"); };
            set_pinned_closure_test_hooks(std::move(forbidden));
            auto rejected = acquire_pinned_submodule_closure(std::move(selection));
            require(std::holds_alternative<PinnedClosureFailure>(rejected) && std::get<PinnedClosureFailure>(rejected).reason == Reason::InvalidSelection && calls == 0, "Moved input minted closure");
            require(std::holds_alternative<InvocationOwnedSourceBuildContextCleaned>(keeper.cleanup()), "Moved input retained context");
            set_pinned_closure_test_hooks({});
            ++completed;
            std::cout << "S564 4A moved-input PASS\n";
            continue;
        }
        require(phases == 1, "4A0 evaluation count changed");
        std::map<std::string, fs::path> remotes{{root.url(), root.remote()}, {child.url(), child.remote()}};
        if(nested) remotes.emplace(nested->url(), nested->remote());
        unsigned observations = 0, fetches = 0;
        bool injected = false;
        bool reading = false;
        unsigned removal_attempts = 0;
        std::optional<BoundedCapturedProcessResult> injected_process;
        fs::path owned_root;
        PinnedClosureTestHooks hooks;
        PinnedClosureLimits limits;
        if(kind == "depth-bound") {
            limits.depth = 0;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "edge-bound") {
            limits.edges = 0;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "declaration-bound") {
            limits.declaration_bytes = declaration.size() - 1;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "aggregate-bound") {
            limits.aggregate_declaration_bytes = declaration.size() - 1;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "records-bound") {
            limits.tree_records = 1;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "metadata-bound") {
            limits.metadata_bytes = 1;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "process-bound") {
            limits.processes = 1;
            expected = Reason::ResourceLimitExceeded;
        }
        if(kind == "limit-slack") {
            limits.depth = 2;
            limits.edges = 2;
            limits.declaration_bytes = declaration.size() + 1;
            limits.aggregate_declaration_bytes = declaration.size() + 1;
        }
        if(kind == "single") {
            limits.depth = 1;
            limits.edges = 1;
            limits.declaration_bytes = declaration.size();
            limits.aggregate_declaration_bytes = declaration.size();
        }
        hooks.limits = limits;
        hooks.event = [&](Stage stage, const fs::path& path) {
            owned_root = path;
            if(!injected && stage == Stage::ChildAcquisition && (kind == "config" || kind == "alternates")) {
                injected = true;
                std::ofstream output(path / "node-0" / (kind == "config" ? "config" : "objects/info/alternates"), std::ios::app);
                output << (kind == "config" ? "[include]\npath=/tmp/never\n" : "/tmp/never\n");
            }
        };
        if(kind == "config" || kind == "alternates") expected = Reason::MalformedRepository;
        hooks.process = [&](const ExplicitProcessInvocation& original, const BoundedProcessPolicy& policy) {
            auto invocation = original;
            const auto& argv = original.arguments;
            auto has = [&](const std::string& value) { return std::find(argv.begin(), argv.end(), value) != argv.end(); };
            require(original.executable == "/usr/bin/git" && original.working_directory_fd && original.standard_input_fd &&
                        has("protocol.https.allow=always") && has("protocol.http.allow=never") && has("protocol.file.allow=never") &&
                        has("http.followRedirects=false") && has("core.hooksPath=/dev/null"),
                    "Trusted HTTPS argv weakened");
            for(const auto& entry : original.environment)
                require(!entry.starts_with("HOME=") && !entry.starts_with("GIT_CONFIG_COUNT=") && !entry.starts_with("GIT_DIR=") && !entry.starts_with("XDG_"), "Ambient environment leaked");
            for(const auto& required : {"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_SYSTEM=/dev/null", "GIT_ASKPASS=/bin/false"})
                require(std::find(original.environment.begin(), original.environment.end(), required) != original.environment.end(), "Trusted environment missing");
            if(has("ls-remote")) {
                ++observations;
                require(argv[argv.size() - 2] == root.url() && argv.back() == (kind == "branch" ? "refs/heads/main" : "HEAD"), "Child HEAD or wrong root selector queried");
            }
            if(has("fetch")) {
                ++fetches;
                require(has("--no-replace-objects") && has("fetch.fsckObjects=true") && has("--no-write-fetch-head") && !has("--remote"), "Exact fetch policy weakened");
                require(argv.back() == (fetches == 1 ? root_pin : fetches == 2     ? pinned_child
                                                              : kind == "siblings" ? pinned_child
                                                                                   : nested->oid()),
                        "Fetch refspec did not retain parent pin");
            }
            const bool init_fault = kind.starts_with("init-") && has("init");
            const bool fetch_fault = kind.starts_with("fetch-") && has("fetch") && fetches == 2;
            if(!injected && (init_fault || fetch_fault || (reading && kind == "read-cleanup-process"))) {
                injected = true;
                BoundedProcessOutcome outcome = BoundedProcessExited{9};
                if(kind.ends_with("launch")) outcome = BoundedProcessLaunchOrSetupFailure{BoundedProcessLaunchStage::Execve, ENOENT};
                if(kind.ends_with("timeout")) outcome = BoundedProcessTimedOut{};
                if(kind.ends_with("signal")) outcome = BoundedProcessSignaled{SIGTERM};
                if(kind.ends_with("overflow")) outcome = BoundedProcessCaptureLimitExceeded{policy.stdout_capture_limit};
                if(kind.ends_with("io")) outcome = BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::Poll, EIO};
                injected_process = BoundedCapturedProcessResult{"original process diagnostic", outcome};
                return *injected_process;
            }
            const bool fault_boundary = (has("ls-remote") && kind != "cleanup-primary") ||
                                        (kind == "child-cancel" && has("fetch") && fetches == 2) ||
                                        (kind == "cleanup-primary" && has("fsck"));
            if(!injected && fault_boundary && (kind == "launch" || kind == "nonzero" || kind == "timeout" || kind == "signal" || kind == "overflow" || kind == "cancel" || kind == "child-cancel" || kind == "io" || kind == "cleanup-primary")) {
                if(kind == "child-cancel" && has("ls-remote")) { /* wait for child acquisition */
                } else {
                    injected = true;
                    if(kind == "io") return BoundedCapturedProcessResult{{}, BoundedProcessIoOrWaitFailure{BoundedProcessIoStage::Poll, EIO}};
                    auto fault = original;
                    fault.executable = kind == "launch" ? "/moguet-no-such-git" : "/bin/sh";
                    std::string script = kind == "nonzero" || kind == "cleanup-primary" ? "exit 9" : kind == "signal" ? "kill -TERM $$"
                                                                                                 : kind == "overflow" ? "printf 012345678901234567890123456789"
                                                                                                                      : "while :; do :; done";
                    if(kind == "cancel" || kind == "child-cancel") script = "trap 'exit 0' INT; printf '" + root_pin + "\\tHEAD\\n'; kill -INT \"$PPID\"; while :; do :; done";
                    fault.arguments = {"-c", script};
                    auto bounded = policy;
                    bounded.hard_timeout = std::chrono::milliseconds(300);
                    bounded.termination_grace = std::chrono::milliseconds(50);
                    if(kind == "overflow") bounded.stdout_capture_limit = 4;
                    return capture_bounded_explicit_process_output_raw(fault, bounded);
                }
            }
            for(auto& argument : invocation.arguments) {
                if(argument == "protocol.file.allow=never")
                    argument = "protocol.file.allow=always";
                else if(remotes.contains(argument))
                    argument = "file://" + remotes.at(argument).string();
            }
            if(kind == "root-unavailable" && has("fetch") && fetches == 1) invocation.arguments[invocation.arguments.size() - 2] = "file://" + child.remote().string();
            auto result = capture_bounded_explicit_process_output_raw(invocation, policy);
            if(kind == "wrong-format" && has("--show-object-format=storage")) result.output = "sha256\n";
            if(has("ls-tree") && has("--name-only")) {
                if(kind == "duplicate-tree" || kind == "duplicate-tree-record") {
                    const auto end = result.output.find('\0');
                    result.output += result.output.substr(0, end + 1);
                }
                if(kind == "tree-framing") result.output += "garbage";
            }
            if(has("ls-tree") && !has("--name-only")) {
                if(kind == "duplicate-tree-record") {
                    // Match the duplicated path with all four metadata fields:
                    // record counts/framing stay valid, only uniqueness fails.
                    std::size_t end = 0;
                    for(unsigned field = 0; field < 4; ++field) {
                        end = result.output.find('\0', end);
                        require(end != std::string::npos, "Fixture metadata record incomplete");
                        ++end;
                    }
                    result.output += result.output.substr(0, end);
                }
                if(kind == "tree-mode") result.output.replace(0, 6, "040000");
                if(kind == "tree-oid") {
                    auto first = result.output.find('\0');
                    auto second = result.output.find('\0', first + 1);
                    result.output[second + 1] = 'z';
                }
            }
            if(has("ls-remote")) {
                if(kind == "observation-duplicate") result.output += result.output;
                if(kind == "observation-wrong-ref") result.output = root_pin + "\trefs/heads/other\n";
                if(kind == "observation-malformed") result.output = "invalid-oid\tHEAD\n";
            }
            if(has("ls-remote") && (kind == "freeze" || kind == "root-unavailable")) root.commit("remote-X-to-Y\n");
            return result;
        };
        if(kind == "launch" || kind == "nonzero" || kind == "timeout" || kind == "signal" || kind == "overflow") expected = Reason::GitProcessFailed;
        if(kind == "cancel" || kind == "child-cancel") expected = Reason::Cancelled;
        if(kind == "io" || kind == "cleanup-primary") expected = Reason::GitProcessFailed;
        if(kind == "root-unavailable") expected = Reason::PinnedObjectUnavailable;
        if(kind == "wrong-format") expected = Reason::ObjectFormatMismatch;
        if(kind == "duplicate-tree" || kind == "duplicate-tree-record" || kind == "tree-framing" || kind == "tree-mode" || kind == "tree-oid") expected = Reason::MalformedTree;
        if(kind.starts_with("init-") || kind.starts_with("fetch-")) expected = kind == "fetch-nonzero" ? Reason::PinnedObjectUnavailable : Reason::GitProcessFailed;
        if(kind.starts_with("observation-")) expected = Reason::MalformedObservation;
        if(kind == "cleanup" || kind == "cleanup-primary" || kind.starts_with("read-cleanup-")) hooks.before_remove = [&](const fs::path&) {
            ++removal_attempts;
            throw std::runtime_error("injected cleanup failure");
        };
        set_pinned_closure_test_hooks(std::move(hooks));
        auto result = acquire_pinned_submodule_closure(std::move(selection));
        require(!selection.valid(), "Selection was not consumed");
        if(expected) {
            const auto* failure = std::get_if<PinnedClosureFailure>(&result);
            // Git's mandatory fetch fsck can reject unsafe .gitmodules bytes
            // before our declaration parser. The pure parser oracle above
            // independently checks our more specific supported-subset reason.
            const bool fsck_rejected = failure && failure->stage == Stage::RootAcquisition &&
                                       failure->reason == Reason::PinnedObjectUnavailable && failure->process &&
                                       std::holds_alternative<BoundedProcessExited>(failure->process->outcome) &&
                                       std::get<BoundedProcessExited>(failure->process->outcome).exit_code != 0 && (declaration_check || kind == "symlink-modules" || kind == "gitlink-modules");
            if(!failure || (failure->reason != *expected && !fsck_rejected)) {
                std::ostringstream message;
                message << kind << " wrong failure expected=" << static_cast<int>(*expected);
                if(failure) message << " stage=" << static_cast<int>(failure->stage) << " reason=" << static_cast<int>(failure->reason);
                throw std::runtime_error(message.str());
            }
            if(kind == "cancel" || kind == "child-cancel") require(failure->process && failure->process->cancellation_signal == SIGINT && std::get<BoundedProcessExited>(failure->process->outcome).exit_code == 0, "Cancel+exit0 lost");
            if(injected_process) require(failure->process && failure->process->outcome == injected_process->outcome && failure->process->output == injected_process->output &&
                                             failure->stage == (kind.starts_with("init-") ? Stage::RootAcquisition : Stage::ChildAcquisition),
                                         "Acquisition process failure lost its original stage/outcome");
            if(kind == "cleanup-primary") {
                require(failure->cleanup.objects && failure->process && std::get<BoundedProcessExited>(failure->process->outcome).exit_code == 9 && failure->abandoned_root == owned_root, "Cleanup erased primary failure");
                set_pinned_closure_test_hooks({});
                fs::remove_all(owned_root);
            } else
                require(failure->cleanup.succeeded() && !fs::exists(owned_root), "Failed acquisition retained root");
        } else {
            if(const auto* failure = std::get_if<PinnedClosureFailure>(&result)) {
                std::ostringstream message;
                message << kind << " acquisition failed stage=" << static_cast<int>(failure->stage) << " reason=" << static_cast<int>(failure->reason);
                if(failure->process) message << " process=" << failure->process->outcome.index();
                throw std::runtime_error(message.str());
            }
            auto closure = take_arm<Closure>(result, "No closure owner");
            const std::size_t edge_count = kind == "nested" || kind == "siblings" ? 2 : 1;
            require(closure.valid() && closure.nodes().size() == edge_count + 1 && closure.edges().size() == edge_count &&
                        closure.nodes()[0].commit.value() == root_pin && closure.edges()[0].pin.value() == pinned_child &&
                        closure.edges()[0].logical_name == "logical/A" && closure.edges()[0].path == "deps/a" && closure.selection().snapshot_identity() == snapshot,
                    "Closure inventory/pins/lineage lost");
            require(closure.nodes()[1].commit.format() == format && observations == 1 && fetches == edge_count + 1, "Wrong format or acquisition inventory");
            if(kind == "siblings") require(closure.edges()[0].pin == closure.edges()[1].pin && closure.edges()[0].child != closure.edges()[1].child, "Sibling edge collapsed");
            if(kind == "nested") require(closure.edges()[1].parent == 1 && closure.nodes()[2].commit.value() == nested->oid(), "Nested pin wrong");
            const auto& inventory = closure.nodes()[1].inventory.entries;
            const auto payload = std::find_if(inventory.begin(), inventory.end(), [](const auto& entry) { return entry.path().raw_bytes() == "payload.txt"; });
            require(payload != inventory.end(), "Child source inventory incomplete");
            const auto bytes = closure.read_blob(1, static_cast<std::size_t>(payload - inventory.begin()));
            require(std::holds_alternative<std::string>(bytes) && std::get<std::string>(bytes) == (kind == "parent-move" ? "child-second\n" : "revision-one\n"), "Exact retained child backing unavailable");
            if(kind.starts_with("read-cleanup-")) {
                reading = true;
                {
                    auto failed_owner = std::move(closure);
                    const auto failed_read = failed_owner.read_blob(kind == "read-cleanup-exception" ? failed_owner.nodes().size() : 1, 0);
                    const auto* failure = std::get_if<PinnedClosureFailure>(&failed_read);
                    require(failure && failure->cleanup.objects && !failure->cleanup.selection && failure->abandoned_root == owned_root &&
                                fs::exists(owned_root) && !failed_owner.valid(),
                            "Read failure lost cleanup residue or retained authority");
                    if(kind == "read-cleanup-process")
                        require(failure->reason == Reason::GitProcessFailed && failure->stage == Stage::ObjectProof && failure->process &&
                                    failure->process->outcome == injected_process->outcome && failure->process->output == injected_process->output,
                                "Read cleanup erased original process failure");
                    else
                        require(failure->reason == Reason::MalformedTree && !failure->process, "Read exception taxonomy changed");
                    require(!failed_owner.cleanup().succeeded() && removal_attempts == 1, "Closed owner retried cleanup");
                }
                require(removal_attempts == 1 && fs::exists(owned_root), "Destructor retried abandoned cleanup");
                set_pinned_closure_test_hooks({});
                fs::remove_all(owned_root);
                fixture.require_no_provenance_publication();
                ++completed;
                std::cout << "S564 4A " << kind << " PASS\n"
                          << std::flush;
                continue;
            }
            auto moved = std::move(closure);
            require(!closure.valid() && moved.valid(), "Move retained duplicate authority");
            auto cleaned = moved.cleanup();
            require(!moved.valid(), "Cleanup left authority usable");
            if(kind == "cleanup") {
                require(!cleaned.succeeded() && cleaned.objects && fs::exists(owned_root), "Cleanup failure not separated");
                set_pinned_closure_test_hooks({});
                require(!moved.cleanup().succeeded(), "Cleanup retried without authority");
                // Explicit fixture cleanup of this exact test-created residue.
                fs::remove_all(owned_root);
            } else
                require(cleaned.succeeded() && !fs::exists(owned_root), "Closure cleanup failed");
        }
        set_pinned_closure_test_hooks({});
        fixture.require_no_provenance_publication();
        ++completed;
        std::cout << "S564 4A " << kind << " PASS\n"
                  << std::flush;
    }
    require(completed == 84, "Closure matrix coverage count changed");
    std::cout << "S564 4A final inventory PASS: " << completed << " cases\n";
}
#endif

} // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    const std::vector<fs::path> before = context_root_inventory();
    try {
#ifdef MOGUET_ENABLE_PINNED_SUBMODULE_CLOSURE_TEST_HOOKS
        if(argc != 2 || std::string(argv[1]) != "--pinned-closure") throw std::invalid_argument("Explicit closure mode required");
        test_pinned_closure_allocation_cleanup();
        test_pinned_closure();
        require(context_root_inventory() == before, "Closure retained selection context");
        return 0;
#endif
#ifdef MOGUET_TEST_DEVEL_BOOTSTRAP_INTEGRATION
        if(argc == 3 && std::string(argv[1]) == "--devel-bootstrap") {
            test_reviewed_devel_execution_bridge(true, argv[2]);
            require(context_root_inventory() == before, "bootstrap retained build context");
            return 0;
        }
#endif
#ifdef MOGUET_TEST_REVIEWED_DEVEL_SOURCE_EXECUTION
#ifdef MOGUET_TEST_NORMAL_REVIEWED_DEVEL_EXECUTION
        if(argc == 2 && std::string(argv[1]) == "--normal-reviewed-devel-execution") {
            test_reviewed_devel_execution_bridge(true);
            require(context_root_inventory() == before, "normal finalizer retained context");
            return 0;
        }
#endif
        if(argc == 2 && std::string(argv[1]) == "--reviewed-devel-execution") {
            test_reviewed_devel_execution_bridge();
            require(context_root_inventory() == before, "S7-C retained a build context");
            return 0;
        }
#endif
#ifdef MOGUET_ENABLE_DEVEL_BUILD_PROVENANCE_PUBLICATION_TEST_HOOKS
        if(argc == 2 && std::string(argv[1]) == "--installed-devel-publication") {
            test_installed_exact_binding(true);
            require(context_root_inventory() == before, "installed S6 fixture retained a build context");
            return 0;
        }
        if(argc == 2 && (std::string(argv[1]) == "--devel-build-provenance-publication" ||
                         std::string(argv[1]) == "--devel-build-provenance-publication-result")) {
            test_exact_installed_binding(std::string(argv[1]) == "--devel-build-provenance-publication" ? "publication-projection" : "publication-aggregate");
            require(context_root_inventory() == before, "S6-B fixture retained a build context");
            return 0;
        }
#endif
#ifdef MOGUET_TEST_EXACT_INSTALLED_BINDING
        if(argc == 2 && std::string(argv[1]) == "--installed-exact-binding") {
            test_installed_exact_binding();
            require(context_root_inventory() == before, "installed S5-B fixture retained a build context");
            return 0;
        }
        if(argc == 2 && (std::string(argv[1]) == "--installed-devel-source-build-proof" ||
                         std::string(argv[1]) == "--devel-source-artifact-install-result")) {
            test_exact_installed_binding(std::string(argv[1]) == "--installed-devel-source-build-proof" ? "proof" : "aggregate");
            require(context_root_inventory() == before, "S5-C fixture retained a build context");
            return 0;
        }
        if(argc == 2 && std::string(argv[1]) == "--exact-installed-binding") {
            test_exact_installed_binding();
            require(context_root_inventory() == before, "S5-B fixture retained a build context");
            return 0;
        }
#endif
// Every transport-capable executable has registered modes. Do not let a
// missing/mistyped mode pass by running only the common owner suite instead.
#ifdef MOGUET_TEST_EVALUATED_DEVEL_ARTIFACT_TRANSPORT
        if(argc == 2 && std::string(argv[1]) == "--evaluated-artifact-transport") {
            test_evaluated_artifact_transport();
            require(context_root_inventory() == before, "Transport suite retained a build context");
            std::cout << "Slice 5 retained bridge: final context inventory/cleanup PASS\n";
            return 0;
        }
        throw std::invalid_argument("Transport fixture requires an explicit registered test mode.");
#endif
        // The default owner lane keeps every 4A0 and common S4 regression.
        const auto selection_started = std::chrono::steady_clock::now();
        test_preprepare_selection();
        test_selection_projection_rejection();
        test_selection_resume_and_environment();
        test_selection_lifetime_and_drift();
        test_selection_process_failures();
        test_selection_explicit_branch_cancellation();
        test_selection_resume_failure_stage();
        const auto selection_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - selection_started);
        std::cout << "S564 4A0 owner prefix PASS (" << selection_elapsed.count() << " ms)\n"
                  << std::flush;
        const auto common_started = std::chrono::steady_clock::now();
        test_valid_dynamic_build_and_prepare_mutation();
        test_declared_architecture_outputs();
        test_architecture_declaration_rejection();
        test_packagelist_output_rejection();
        test_selected_architecture_archive_drift();
        test_reviewed_local_source_remains_supported_input();
        test_sha256_upstream_revision();
        test_source_projection_fail_closed();
        test_supplemental_input_fail_closed();
        test_two_upstream_revisions_change_dynamic_identity();
        test_dynamic_version_drift_fails_closed();
        test_git_replacement_and_grafts_rejected();
        test_stale_pkgdest_and_extra_artifact();
        test_artifact_replacement();
        test_ambiguous_workspace_and_artifact_hardlink();
        test_artifact_metadata_mismatch();
        test_malformed_archive_metadata_taxonomy();
        test_cross_context_environment_rejected();
        test_cleanup_failure_preserves_primary();
        test_cleanup_budgets();
        set_evaluated_devel_source_build_test_hook({});
        set_evaluated_devel_source_build_process_test_hook({});
        set_exact_git_branch_validation_process_test_hook({});
        set_invocation_owned_source_build_context_test_hook({});
        require(
            context_root_inventory() == before,
            "Focused test left an invocation-owned context root");
        const auto common_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - common_started);
        std::cout << "S4 owner regressions / final inventory PASS (" << common_elapsed.count() << " ms)\n";
    } catch(const std::exception& error) {
        set_evaluated_devel_source_build_test_hook({});
        set_evaluated_devel_source_build_process_test_hook({});
        set_exact_git_branch_validation_process_test_hook({});
        set_invocation_owned_source_build_context_test_hook({});
        std::cerr << "evaluated devel source-build tests failed: "
                  << error.what() << '\n';
        return 1;
    }
    std::cout << "evaluated devel source-build tests passed\n";
    return 0;
}
